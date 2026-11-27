#pragma once
// even60 (GXV_HRW_EVEN60, default 1; GXV_HRW_ALLOW50=1 in env.txt restores the old pacing): the Performance picture is an even 60 or an even 30, never an
// uneven in-between rate. Single player and multiplayer share this one policy; single player starts every stage at the 60 lock, a multiplayer session at an even 30.
//
// Console 25 Nov 2026 (Play Together, Performance, 60 Assist on): in busy places the game ran plain 60 with frame skips at an even 50, and the other player's character looked jumpy; locked at 30 it was
// smooth. Owner: "In multiplayer we go 60/30 and leave 50 as an option in the settings. Multiplayer should start at 30 and then ramp up with our game logic when able." and "Make sure 50 is removed from
// sp as well, keep it as a setting. Single player should lock at 60 to start."
//
// Three pictures, nothing else: kEven30 (an even 30: the world and the picture at 30, the stage's locked 30 of GXV_HRW_STAGE), kAssist (60/30, the governor's pairs: the world at
// 30, camera and Mario at 60) and kPlain60 (every turn drawn). The policy is only the DECISION: what is drawn. Game logic, turn counts and the network never read it (the turns run
// at 60 in every one of them; an undrawn turn is a full turn in a lockstep session, the lean undrawn turns being held off by port/lockstep.h).
//
//   Begin (a stage loads, the session starts) ...... kEven30 in multiplayer, kPlain60 in single player. Nothing is judged until `settlePlay` (8) seconds of play and 2 quiet seconds (no shader
//                         compiled, no program linked: the stage's first-use work); a second with such work is never counted as a skip.
//   kPlain60 -> kAssist   plain 60 skips: the native rate under 52 in 8 of 10 seconds, at least 4 in each half of 5 (two windows in a row), and the pair fits; else -> kEven30.
//                         When `assist` is enabled and settled (`judge`), `kGovEngaged` also moves kPlain60 -> kAssist; when `!assist`, `kGovEngaged` never drops kPlain60 -> kEven30.
//   kEven30  -> kAssist   the pair fits (PairFit on the costs measured at 30) in 4 of the last 5 seconds and the governor has not blocked Assist (a block is lifted after `blockRetrySec`).
//   kEven30  -> trial     after 3 consecutive quiet headroom seconds (`holdsVerdict`, respecting `gapSec`), or after 10 s at an even 30 (any reason: pairs blocked, not fitting, multiplayer's start):
//                         plain 60 for a trial, the first second ignored; kept when the native rate is 56 or more in 3 of the next 4 seconds (measured, not the governor's verdict) and resets `trialWait_`
//                         to `trialFirst`, else back to 30 and the next trial waits twice as long capped at 30 s (10, 20, 30). A new scene resets it.
//   kAssist  -> kPlain60  when the governor itself leaves because plain 60 would hold again; -> kEven30 when it leaves for any other reason or blocks Assist (a trial follows: never stranded at 30).
// `gapSec` between two changes the policy makes itself (the governor's own changes are taken as they come).
#include <cstdint>

namespace port {
namespace hrwmp {

enum Mode : int { kEven30 = 0, kAssist = 1, kPlain60 = 2 };
inline const char* ModeName(int m) {
    static const char* const n[3] = {"even 30 (world and picture at 30)", "60/30 (60 Assist: the world at 30, camera and Mario at 60)", "plain 60 (every picture real)"};
    return m >= 0 && m < 3 ? n[m] : "?";
}
// GXV_HRW_EVEN60 (default 1) and GXV_HRW_ALLOW50 (default 0; the Settings row "Allow in-between frame rates (50 fps)" sets it at start-up): the policy is available unless one says no.
inline bool Enabled(long even60, long allow50) { return even60 != 0 && allow50 == 0; }
// In force only with mode 5 under the governor (60 Assist on, Performance), outside Quality mode, in a stage whose row says nothing (GXV_HRW_STAGE 0).
// Single player and multiplayer alike (owner, 25 Nov: no uneven in-between rate anywhere); the setting "Allow in-between frame rates" (enabled false) leaves every path of vgx_galaxy.cpp as it was.
inline bool InForce(bool enabled, bool gov5, bool quality, long stageKnob) { return enabled && gov5 && !quality && stageKnob == 0; }
// even60 with 60 Assist OFF (GXV_HRW=0, Performance, single player, GXV_HRW_EVEN60_NOASSIST default 1):
// plain 60 <-> even 30 only (never pairs, never in Quality, never when a fixed cap or another steady-30 controller owns pacing).
inline bool InForceNoAssist(bool enabled, bool noAssistSwitch, int hrwMode, bool quality, long stageKnob, bool fixedCap, bool steady30) {
    return enabled && noAssistSwitch && hrwMode == 0 && !quality && stageKnob == 0 && !fixedCap && !steady30;
}
// True only during real gameplay that even60 may judge: live scene (hrwPlay), not loading, no opening camera / starter fly-in / movie / demo
// (cutscene_skip.cpp PORT_SKIP_MOVIE=1, _OPENING=2, _STARTER=3, _DEMO=4; LodCtrl.cpp kFlightOpening=1..kFlightDemo=4), and if PETARI_FLOW is in a leg,
// play turns > 0 (flowPlayTurns == 0 means the leg is still in its opening camera / demo; -2 = no flow, -1 = outside a leg).
inline bool PlayActive(bool hrwPlay, bool loading, int skipKind, int popinFlight, int flowPlayTurns) {
    if (!hrwPlay || loading) return false;
    if (flowPlayTurns == 0) return false;
    if (skipKind >= 1 && skipKind <= 4) return false;
    if (popinFlight >= 1 && popinFlight <= 4) return false;
    return true;
}
// A second is shader-quiet only when zero shaders were compiled, zero programs were linked, AND < 1.0 ms of shader work ran (cached/binary program loads
// still add to shaderWorkMs with 0 compiles and 0 links, e.g. 366 ms in night82/01-E-BBS.log; < 1.0 ms tolerates sub-millisecond timer/slice overhead).
inline bool ShaderQuiet(uint32_t compiles, uint32_t lastCompiles, uint32_t links, uint32_t lastLinks, double shaderWorkMs, double lastShaderWorkMs) {
    return compiles == lastCompiles && links == lastLinks && (shaderWorkMs - lastShaderWorkMs) < 1.0;
}
// Measured single-picture costs fit a 60 fps budget with headroom (used at kEven30 where every drawn picture is full-cost and the governor is paused).
// Any unmeasured core figure (<= 0) never claims headroom; when pipeMs > 0 (max(pipeGpu, backend, front)), it must also fit limitMs.
inline bool PlainCostsHold60(double gameDrawnMs, double feMs, double gpuMs, double pipeMs = 0.0, double limitMs = 15.5) {
    return gameDrawnMs > 0.0 && gameDrawnMs <= limitMs && feMs > 0.0 && feMs <= limitMs && gpuMs > 0.0 && gpuMs <= limitMs && (pipeMs <= 0.0 || pipeMs <= limitMs);
}
// What the governor did in the second the policy looks at.
enum GovEvent : int { kGovNothing = 0, kGovEngaged = 1, kGovLeftPlainHolds = 2, kGovLeftOther = 3 };

struct Params {
    bool on = true;                  // GXV_HRW_EVEN60 (and not GXV_HRW_ALLOW50)
    int settlePlay = 8;              // seconds of play after a scene starts before anything is judged, and the last `settleQuiet` seconds must be quiet (no shaders prepared, no program links)
    int settleQuiet = 2;
    double gapSec = 6.0;             // between two changes the policy makes itself (ticks)
    int win = 10;                    // the drop window, seconds: two halves of 5
    int needDrop = 8;                // plain 60 skips (native under dropBelowFps, in a quiet second) in this many of the 10 seconds, at least 4 in each half
    int needUp = 4;                  // kEven30 -> kAssist: the pair fits in this many of the last 5 seconds
    double dropBelowFps = 52.0;      // a second under this native rate (pictures drawn a second) is a skipping second
    int trialFirst = 10;             // seconds at an even 30 before the first trial of plain 60; doubles on each failed trial up to trialCap (10, 20, 30)
    int trialCap = 30;
    int trialSecs = 5;               // the trial: the first second is ignored, the other 4 judged
    double trialFps = 56.0;          // a trial second at or over this holds
    int trialNeed = 3;               // ... in this many of the 4 (measured native rate only)
    int holdsFastReturn = 3;         // consecutive quiet play seconds in kEven30 with holdsVerdict true -> immediately trial plain 60 (respecting gapSec); 0 disables
    double blockRetrySec = 30.0;     // seconds of play in kEven30 with Assist blocked before the block is lifted for one more try; 0 = never
    bool assist = true;              // mp31 (night81 console): 60/30 may be chosen; GXV_HRW_EVEN60_ASSIST (the game's default 0: on the console the pairs were blocked again within
                                     // 6-27 s on Beach Bowl, Honeyhive, Good Egg, Gusty Garden and Dreadnought, uneven meanwhile; so the policy goes plain 60 <-> even 30 only)
};

// One second of the game's figures, from the 1 Hz census (vgx_galaxy.cpp).
struct Sec {
    bool play = false;               // a stage in play (not a load, a menu, a pause, a wipe)
    double nativeFps = 0;            // pictures drawn a second
    bool fits = false;               // the pair fits on the costs of this second (vgx::hrw::PairFit(...).All(); unknown figures: false)
    bool holdsVerdict = false;       // the governor's own verdict is "plain 60 holds" (Governor::kVHolds)
    bool blocked = false;            // the governor has blocked Assist for the stage
    bool quiet = true;               // no shader compiled, no program linked in this second (first-use work: such a second is never counted as a skip)
    GovEvent ev = kGovNothing;       // the governor's decision of this second
};

struct Out {
    bool changed = false;
    int mode = kEven30;
    bool byGovernor = false;         // the governor made the change itself (nothing to hand it): only the stage's choice and the log follow
    bool unblock = false;            // lift the governor's block (blockRetrySec)
    int trial = 0;                   // 1: a trial of plain 60 begins; 2: the trial was kept; 3: the trial failed (back to 30)
    int nextTrialSec = 0;            // with trial 3: the wait before the next trial
    const char* why = "";
};

// Even 30 -> trial of plain 60 (fast return after 3 quiet headroom seconds, or from 10 s backed off 10/20/30 on failures, reset by a succeeded trial or new scene) -> kept, or back to 30. A blocked 60/30 never strands a stage at 30: the trial runs from an even 30 either way.
class Policy {
public:
    void Configure(const Params& p) {
        p_ = p;
        if (p_.win < 2) p_.win = 2;
        if (p_.win > kMax) p_.win = kMax;
        if (p_.trialFirst < 1) p_.trialFirst = 1;
        if (p_.trialCap < p_.trialFirst) p_.trialCap = p_.trialFirst;
    }
    bool On() const { return p_.on; }
    int Mode() const { return mode_; }
    bool Trialing() const { return trial_; }
    bool Settled() const { return playSec_ >= p_.settlePlay && quietRun_ >= p_.settleQuiet; }
    // The session starts or a new scene loads. start: kEven30 in a multiplayer session, kPlain60 in single player (a stage starts at the 60 lock; it falls only on sustained misses).
    void Begin(double nowSec, int start = kEven30) { mode_ = start; Enter(nowSec); lastChange_ = -1e9; blockedSec_ = 0; playSec_ = quietRun_ = 0; trialWait_ = p_.trialFirst; trial_ = false; evenSec_ = 0; holdsRun_ = 0; holdsOffRun_ = 0; fastArmed_ = true; retryMul_ = 1; }
    Out Tick(double nowSec, const Sec& s) {
        Out o;
        o.mode = mode_;
        if (!p_.on) return o;
        if (!s.play) { n_ = 0; quietRun_ = 0; holdsRun_ = 0; holdsOffRun_ = 0; return o; }   // (a load, a pause, a wipe: the windows start again)
        ++playSec_;
        quietRun_ = s.quiet ? quietRun_ + 1 : 0;
        ++modeSec_;
        if (mode_ == kEven30) {
            ++evenSec_;   // (seconds at an even 30 in this scene: an excursion to 60/30 does not zero it, or a backed-off trial would never come)
            if (s.quiet && s.holdsVerdict) {
                ++holdsRun_;
                holdsOffRun_ = 0;
            } else {
                holdsRun_ = 0;
                if (s.quiet && !s.holdsVerdict) {
                    ++holdsOffRun_;
                    if (!fastArmed_ && holdsOffRun_ >= 3) fastArmed_ = true;
                } else {
                    holdsOffRun_ = 0;
                }
            }
        } else {
            holdsRun_ = 0;
            holdsOffRun_ = 0;
        }
        if (!trial_ && mode_ == kPlain60 && modeSec_ == 60) trialWait_ = p_.trialFirst;   // (plain 60 has stood a minute: the backoff is forgotten)
        Push(s);
        if (trial_) return TrialTick(nowSec, s);
        // the governor's own changes first
        if (mode_ == kAssist) {
            if (s.ev == kGovLeftPlainHolds) return Move(kPlain60, nowSec, "the governor leaves 60/30: plain 60 would hold again", true);
            if (s.ev == kGovLeftOther || s.blocked) return Move(kEven30, nowSec, s.blocked ? "60/30 is blocked for the stage (it lost its second pictures or went over the GPU's budget): an even 30, a trial of plain 60 follows" : "the governor leaves 60/30 (the pair stopped fitting or its second pictures were lost): an even 30, a trial of plain 60 follows", true);
            return o;
        }
        const bool judge = playSec_ >= p_.settlePlay && quietRun_ >= p_.settleQuiet;   // (the stage's first seconds and its shader preparation judge nothing)
        const bool gap = nowSec - lastChange_ >= p_.gapSec;
        if (mode_ == kPlain60 && s.ev == kGovEngaged) {
            // When !p_.assist, kGovEngaged must not drop kPlain60 -> kEven30 (only the sustained 2-window drop rule below may drop).
            // When p_.assist is enabled, gate on judge so unsettled opening / shader-load seconds never trigger a premature switch.
            if (p_.assist && judge) return Move(kAssist, nowSec, "the governor engages 60/30: plain 60 does not hold", true);
        }
        if (mode_ == kEven30) {
            if (s.blocked) {
                blockedSec_ += 1;
                if (p_.blockRetrySec > 0 && blockedSec_ >= p_.blockRetrySec * retryMul_) { blockedSec_ = 0; if (retryMul_ < 8) retryMul_ *= 2; Out r = Move(kEven30, nowSec, "Assist was blocked for this stage: tried again", false); r.unblock = true; r.changed = false; return r; }
            } else {
                blockedSec_ = 0;
                if (judge && gap && n_ >= 5) {
                    int ok = 0;
                    for (int i = n_ - 5; i < n_; ++i) ok += fit_[i] ? 1 : 0;
                    if (p_.assist && ok >= p_.needUp) return Move(kAssist, nowSec, "the pair fits: 60/30", false);
                }
            }
            if (judge && gap && fastArmed_ && p_.holdsFastReturn > 0 && holdsRun_ >= p_.holdsFastReturn)
                return StartTrial(nowSec, "fast headroom trial of plain 60 from the even 30");
            if (judge && evenSec_ >= trialWait_) return StartTrial(nowSec);
            return o;
        }
        // kPlain60: a skipping plain 60 never stays: sustained, on two windows in a row, not in a first-use second
        if (judge && gap && n_ >= p_.win) {
            const int half = p_.win / 2;
            int bad1 = 0, bad2 = 0;
            for (int i = 0; i < n_; ++i) (i < half ? bad1 : bad2) += bad_[i] ? 1 : 0;
            if (bad1 + bad2 >= p_.needDrop && bad1 >= 4 && bad2 >= 4) {
                if (p_.assist && s.fits && !s.blocked) return Move(kAssist, nowSec, "plain 60 skips (the native rate under the limit in two windows in a row): 60/30", false);
                return Move(kEven30, nowSec, "plain 60 skips (two windows in a row) and the pair does not fit: an even 30", false);
            }
        }
        return o;
    }

private:
    static constexpr int kMax = 16;
    void Enter(double nowSec) { entered_ = nowSec; n_ = 0; modeSec_ = 0; holdsRun_ = 0; holdsOffRun_ = 0; }
    void Push(const Sec& s) {
        if (n_ == p_.win) { for (int i = 1; i < n_; ++i) { fit_[i - 1] = fit_[i]; bad_[i - 1] = bad_[i]; } n_ = p_.win - 1; }
        fit_[n_] = s.fits;
        bad_[n_] = s.quiet && s.nativeFps < p_.dropBelowFps;
        ++n_;
    }
    Out StartTrial(double nowSec, const char* why = "trial of plain 60 from the even 30") {
        const int waited = evenSec_;
        Out o = Move(kPlain60, nowSec, why, false);
        trial_ = true; trialTick_ = 0; trialGood_ = 0; evenSec_ = 0;
        o.trial = 1;
        o.nextTrialSec = waited;
        return o;
    }
    Out TrialTick(double nowSec, const Sec& s) {
        Out o;
        o.mode = kPlain60;
        ++trialTick_;
        if (trialTick_ == 1) return o;   // (the first second after the switch is ignored)
        if (s.nativeFps >= p_.trialFps) ++trialGood_;
        if (trialGood_ >= p_.trialNeed) {   // (measured native rate only: a stale verdict is no evidence)
            trial_ = false; trialWait_ = p_.trialFirst; fastArmed_ = true; holdsOffRun_ = 0; n_ = 0;   // (kept: reset backoff so a later heavy section recovers promptly)
            o.trial = 2; o.why = "plain 60 holds in the trial: kept";
            return o;
        }
        if (trialTick_ >= p_.trialSecs) {
            trial_ = false;
            const int next = trialWait_ > p_.trialCap / 2 ? p_.trialCap : trialWait_ * 2;
            trialWait_ = next;
            fastArmed_ = false; holdsOffRun_ = 0;   // (do not let optimistic holdsVerdict bypass trialWait_ backoff; re-arm only after 3 quiet !holdsVerdict seconds or a passing trial)
            Out r = Move(kEven30, nowSec, "plain 60 does not hold in the trial: back to an even 30", false);
            r.trial = 3; r.nextTrialSec = next;
            return r;
        }
        return o;
    }
    Out Move(int m, double nowSec, const char* why, bool byGovernor) {
        Out o;
        o.changed = m != mode_;
        o.mode = m;
        o.byGovernor = byGovernor;
        o.why = why;
        if (m != mode_) { mode_ = m; lastChange_ = nowSec; Enter(nowSec); }
        return o;
    }
    Params p_;
    int mode_ = kEven30;
    int n_ = 0;
    bool fit_[kMax] = {}, bad_[kMax] = {};
    double entered_ = 0, lastChange_ = -1e9, blockedSec_ = 0;
    int playSec_ = 0, quietRun_ = 0, modeSec_ = 0, evenSec_ = 0, holdsRun_ = 0, holdsOffRun_ = 0, retryMul_ = 1, trialWait_ = 10, trialTick_ = 0, trialGood_ = 0;
    bool trial_ = false, fastArmed_ = true;
};

}  // namespace hrwmp
}  // namespace port
