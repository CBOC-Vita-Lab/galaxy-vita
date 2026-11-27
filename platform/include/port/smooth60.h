// GXV_SMOOTH60 (policy26, 12 Nov 2026): the per-stage "hold 60, else step down once, else an even 30" decision. Pure header, no platform includes: the back end
// (vgx_backend_gl.cpp, namespace stagepre) feeds it one sample a second and carries out what it answers; tests/smooth60_test.cpp checks it.
//
// The ladder (the owner, 12 Nov: "small sacrifices are fine; wants 60 wherever it can hold; dislikes an uneven 40 more than a clean 30"):
//   k60   the stage's own preset row (960x544, or 848x480 when the row starts there), the cheapest sacrifices that the evidence says reach 58+.
//   k848  848x480 through the light upscaler (the stage's "Stage@848" row, or its own row with the rung changed).
//   k30   an even 30 (every picture held two refreshes, the logic still 60 turns a second) at 960x544 with the quality put back (the "Stage@30" row, or the class
//         s60-q30): the steady 30 of port/frame_pace.h forced on for the stage.
// Rules (Params; the defaults are the owner's numbers):
//   - Only down, never up, inside a stage; the next stage load starts again at the row's own start mode. No flapping is possible.
//   - A step is judged on the pictures a second the player really saw (the caller gives one sample per second, and says when a second was not play: a load, a
//     fade, the pause menu: such a second empties the window).
//   - k60 -> k848 when at least needBelow of the last windowSec seconds were under step848Fps (54) and the stage allows 848 (s60_848=1).
//   - k60 (848 not allowed) or k848 -> k30 when at least needBelow of the last windowSec seconds were under step30Fps (50) and the stage allows 30 (s60_30=1).
//   - No judgment in the first settleSec seconds of a stage or of a mode (the stage's opening camera, the new size's first pictures), and never two changes less
//     than gapSec apart (20 s), at most maxChanges a stage.
//   - Every decision is returned with a reason string for the log; the caller prints it.
// gpugen26 (12 Nov): an optional middle rung, k896 (896x512 through the same light upscaler, 88% of the pixels of 960x544 against 78% at 848x480). Off unless
//   Params::step896Fps > 0 AND the stage allows it (allow896). The step is meant for the stages that sit at 54..57: too close to 60 for the 848 step's threshold (54),
//   too far from it to hold. k60 -> k896 when 5 of the last 5 seconds are under step896Fps (default proposal 57) and NOT under step848Fps (a deeper deficit goes to
//   848 at once: one change, not two); k896 -> k848 when under step848Fps (54) again after the settle time and the gap; k896 -> k30 when 848 is not allowed and
//   under step30Fps. The chain 60 -> 896 -> 848 -> 30 needs one more change a stage (maxChanges + 1 with the middle rung allowed). With step896Fps = 0 every
//   decision is exactly the one of policy26 (tests/smooth60_test.cpp checks that on a noisy trace).
#pragma once
#include <cstdint>
#include <cstdio>
#include <stdio.h>  // (unqualified snprintf: game units map it to port_snprintf, so std::snprintf does not exist there)

namespace port {
namespace smooth60 {

enum Mode : int { k60 = 0, k848 = 1, k30 = 2, k896 = 3 };  // (k896: gpugen26's middle rung; the numbers of the first three are the preset files' s60_start values)
enum Action : int { kNone = 0, kTo848 = 1, kTo30 = 2, kTo896 = 3 };

inline const char* ModeName(int m) { return m == k60 ? "60" : m == k848 ? "848x480" : m == k30 ? "even 30" : m == k896 ? "896x512" : "?"; }

struct Params {
    double step848Fps = 54.0;   // settled pictures a second under this: step to 848x480
    double step30Fps = 50.0;    // settled pictures a second under this (at 848x480, or at 960 when 848 does not help the stage): an even 30
    int windowSec = 5;          // the seconds looked at
    int needBelow = 5;          // how many of them must be under the threshold (5 of 5: a 4 s dip never steps)
    double settleSec = 10.0;    // no judgment this long after a stage starts or a mode begins
    double gapSec = 20.0;       // no two changes closer than this
    int maxChanges = 2;         // a stage (one more when the stage allows the middle rung)
    double step896Fps = 0.0;    // gpugen26: settled pictures a second under this (and not under step848Fps): 896x512 first. 0 = no middle rung (policy26's ladder, bit for bit)
};

class Controller {
public:
    void Configure(const Params& p) { p_ = p; if (p_.windowSec < 1) p_.windowSec = 1; if (p_.windowSec > kMax) p_.windowSec = kMax; if (p_.needBelow < 1) p_.needBelow = 1; if (p_.needBelow > p_.windowSec) p_.needBelow = p_.windowSec; }
    const Params& Cfg() const { return p_; }
    // A stage starts loading (the screen is closed). start: the row's start mode; allow848 / allow30: the row's s60_848 / s60_30.
    void StageBegin(uint64_t nowUs, int start, bool allow848, bool allow30, bool allow896 = false) {
        mode_ = start < k60 ? k60 : start > k896 ? k896 : start;
        allow848_ = allow848;
        allow30_ = allow30;
        allow896_ = allow896 && p_.step896Fps > 0.0;
        if (mode_ == k896 && !allow896_) mode_ = allow848_ ? k848 : k60;  // (a row that starts at 896 in a build or run without the middle rung: 848, else its own 60)
        modeSinceUs_ = nowUs;
        lastChangeUs_ = 0;
        changes_ = 0;
        n_ = 0;
        stageActive_ = true;
        lastWhy_[0] = 0;
    }
    void StageEnd() { stageActive_ = false; n_ = 0; }
    // One sample a second: `fps` pictures shown in the last second; `play` false when that second was not play (loading, fade, pause menu, movie).
    // Returns the action to carry out (the controller has already moved to the new mode); Why() says why.
    Action Tick(uint64_t nowUs, double fps, bool play) {
        if (!stageActive_) return kNone;
        if (!play || fps < 0.0) { n_ = 0; return kNone; }
        if (n_ == p_.windowSec) { for (int i = 1; i < n_; ++i) ring_[i - 1] = ring_[i]; n_ = p_.windowSec - 1; }
        ring_[n_++] = fps;
        if (mode_ == k30) return kNone;  // nothing lower; the caller may log "not holding" itself (Holding30())
        if (static_cast<double>(nowUs - modeSinceUs_) < p_.settleSec * 1e6) { n_ = 0; return kNone; }  // (the window starts after the settle time)
        if (n_ < p_.windowSec) return kNone;
        if (changes_ >= p_.maxChanges + (allow896_ ? 1 : 0)) return kNone;
        if (lastChangeUs_ != 0 && static_cast<double>(nowUs - lastChangeUs_) < p_.gapSec * 1e6) return kNone;
        const int below848 = Below(p_.step848Fps), below30 = Below(p_.step30Fps), below896 = allow896_ ? Below(p_.step896Fps) : 0;
        Action a = kNone;
        if (mode_ == k60 || mode_ == k896) {
            if (allow848_ && below848 >= p_.needBelow) a = kTo848;  // (a deep deficit skips the middle rung: one change)
            else if (mode_ == k60 && allow896_ && below896 >= p_.needBelow) a = kTo896;
            else if (!allow848_ && allow30_ && below30 >= p_.needBelow) a = kTo30;
        } else if (mode_ == k848) {
            if (allow30_ && below30 >= p_.needBelow) a = kTo30;
        }
        if (a == kNone) return kNone;
        double mean = 0;
        for (int i = 0; i < n_; ++i) mean += ring_[i];
        mean /= n_;
        const int belowN = a == kTo848 ? below848 : a == kTo896 ? below896 : below30;
        const double thr = a == kTo848 ? p_.step848Fps : a == kTo896 ? p_.step896Fps : p_.step30Fps;
        snprintf(lastWhy_, sizeof lastWhy_, "%.1f pictures a second over the last %d s (%d of them under %.0f) in %s", mean, n_, belowN, thr, ModeName(mode_));
        mode_ = a == kTo848 ? k848 : a == kTo896 ? k896 : k30;
        modeSinceUs_ = lastChangeUs_ = nowUs;
        ++changes_;
        n_ = 0;
        return a;
    }
    int Mode() const { return mode_; }
    int Changes() const { return changes_; }
    const char* Why() const { return lastWhy_; }
    bool Allow848() const { return allow848_; }
    bool Allow30() const { return allow30_; }
    bool Allow896() const { return allow896_; }
    // At k30, whether the last window sat under 27 (the even 30 does not hold: nothing is left to step to; the caller logs it once a stage).
    bool Holding30() const {
        if (mode_ != k30 || n_ < p_.windowSec) return true;
        int low = 0;
        for (int i = 0; i < n_; ++i) low += ring_[i] < 27.0 ? 1 : 0;
        return low < p_.needBelow;
    }

private:
    static constexpr int kMax = 16;
    int Below(double thr) const {
        int c = 0;
        for (int i = 0; i < n_; ++i) c += ring_[i] < thr ? 1 : 0;
        return c;
    }
    Params p_;
    int mode_ = k60;
    bool allow848_ = true, allow30_ = true, allow896_ = false, stageActive_ = false;
    uint64_t modeSinceUs_ = 0, lastChangeUs_ = 0;
    int changes_ = 0, n_ = 0;
    double ring_[kMax] = {};
    char lastWhy_[160] = "";
};

}  // namespace smooth60
}  // namespace port
