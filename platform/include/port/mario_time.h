// mariolag31 (18 Nov 2026): Mario's time in the 60 Assist second picture (GXV_HRW=5). Pure logic (no game types, no GL, no clock of its own), host-tested by
// tests/mario_time_test.cpp (tools/test_mario_time.sh, a synthetic jump arc and the pair-cost traces of the console's runs; mutants). The reasoning is in
// reports/mariolag31.md. Wired in platform/src/gx/vgx_galaxy.cpp (the game thread's pair and the render thread's deadline) and decomp/src/Game/Scene/GameScene.cpp
// (the camera of the fresh draw).
//
// What the second picture of a pair shows of Mario (CONFIRMED by reading GameSystem::frameLoop and the console logs of 18 Nov):
//   the drawn turn T draws the picture A from the state S(T-1), then updates to S(T); the undrawn turn T+1 reads the camera S(T), makes Mario's view pass from S(T) at its
//   start and draws him fresh for the picture B after its own update (GXV_HRW_FRESH_LATE). A = S(T-1), B = S(T), the next A = S(T+1): when the fresh draw is there, B is
//   exactly the turn it stands for. Nothing is extrapolated. When it is NOT there, B shows Mario's pixels of A moved by his centre's move (the player box): his pose one
//   turn old (a landing shows him still in the air for one picture). Console 18 Nov, Honeyhive in the default env (r2-601 K0a): 7,726 second pictures, 1,727 with the
//   fresh Mario, 5,851 left without it by GXV_HRW_FRESH_AUTO ("the pairs had no room"), for three minutes in a row.
//
// GXV_HRW_MARIOTIME (default 0 = as before):
//   1  no stale Mario: a pair whose second picture would show Mario's old pixels (the fresh draw left out by the auto rule, the speed guard or a give-up; or its partial
//      draw missed the render thread's deadline, or could not be merged) has NO second picture: the real picture A stays on the screen for both retraces (a 30 picture
//      for that moment, the same path as a pair over the guard band). Pairs without Mario in the picture (no player, nothing of him drawn) keep the plain warp: there is
//      nothing of him to be stale. The fresh draw also uses the camera of the turn whose pose it draws (the view and projection taken at the undrawn turn's start), not
//      the camera after that turn's update (a time base one turn apart; J3D's body was right already, its matrices are view-space from the pose's turn).
//   2  as 1, and the auto rule decides by cost: a pair counts against the fresh player only when the fresh draw is what takes (or would take) it over the limit
//      (GXV_HRW_FRESH_AUTO_OFF_MS10, 31.0 ms) or over the pair's 33.3 ms; off at 3 of the last 16 pairs as before, on again after 16 pairs off with at most 1 of the last 16 blamed. The old rule
//      counted every slow pair (the console's slow pairs are as slow with the fresh player off: windows' max 31.4 ms off against 30.9 on, K0a) and came back only after
//      90 pairs in a row under 27.0 ms, which a stage near 27 ms never gives.
// GXV_HRW_MARIOTIME_CENSUS=1: the census line in any mode (it is printed whenever the mode is not 0).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>

#ifndef MARIOTIME_MUTANT
#define MARIOTIME_MUTANT 0  // (tests only: 0 = as shipped)
#endif

namespace port {
namespace mariotime {

enum Mode { kOff = 0, kHold = 1, kHoldCost = 2 };
inline int ClampMode(long v) { return v <= 0 ? kOff : v >= 2 ? kHoldCost : static_cast<int>(v); }

// Why a pair's second picture has no fresh Mario (kFresh: it has, or will have, the fresh draw).
enum Why : int {
    kFresh = 0,
    kAutoOff,      // GXV_HRW_FRESH_AUTO has the fresh player off ("the pairs had no room")
    kGuard,        // the game-speed guard owns the pair (the fresh draw is game-thread time)
    kGivenUp,      // the fresh player is given up for this run (HrwFreshGiveUp), or switched off (GXV_HRW_FRESH=0 / GXV_HRW_WARP_FRESH=0)
    kDeadline,     // render thread: the partial draw missed its deadline (the plain re-projection would go out)
    kMerge,        // render thread: the partial draw came but was not merged (dangling commands, refused)
    kWhyCount
};
inline const char* WhyName(int w) {
    static const char* const n[kWhyCount] = {"fresh", "auto off", "speed guard", "given up / off", "deadline missed", "not merged"};
    return w >= 0 && w < kWhyCount ? n[w] : "?";
}

// The game thread, at the pair's undrawn turn (port_hrw_undrawn_begin), in mode 5: why this pair has no fresh draw, in the order the code tests.
struct PairIn {
    bool freshOn = true;      // sHrwFreshOn
    bool givenUp = false;     // sHrwFreshOff
    bool pairNoFresh = false; // decided at the drawn turn (GXV_HRW_PAIR_DEPTH: the first picture was made without the fresh player's depth)
    bool guard = false;       // the speed guard keeps B without the fresh draw
    bool autoOff = false;     // the auto rule's state
};
inline Why NoFresh(const PairIn& p) {
    if (!p.freshOn || p.givenUp) return kGivenUp;
    if (p.guard) return kGuard;
    if (p.autoOff || p.pairNoFresh) return kAutoOff;  // (pairNoFresh is the drawn turn's copy of the auto / give-up state)
    return kFresh;
}
// Hold (no second picture) instead of showing his old pixels? Only where Mario is in the picture: without a player the plain warp shows nothing stale of him.
inline bool HoldInstead(int mode, int why, bool player) {
#if MARIOTIME_MUTANT == 1
    (void)player;
    return mode >= kHold && why != kFresh;
#elif MARIOTIME_MUTANT == 2
    return why != kFresh && player;
#else
    return mode >= kHold && why != kFresh && player;
#endif
}
// The render thread: the partial draw did not come in time / was not merged: hold in modes 1 and 2 (the game thread asked for it, so a player was there when it asked).
// lost49: `limited` is HoldLimit::stale (below): the holds have been most of the pairs, so his old pixels are shown instead until the fresh draw is back.
inline bool RenderHold(int mode, bool limited = false) {
#if MARIOTIME_MUTANT == 6
    (void)mode; (void)limited;
    return false;
#elif MARIOTIME_MUTANT == 7
    (void)limited;
    return mode >= kHold;
#else
    return mode >= kHold && !limited;
#endif
}
// lost49 (22 Nov 2026, owner's Vita log 905, Beach Bowl, Performance, GXV_HRW_GOV5=1): a stage whose pairs cost 27-30 ms on the game thread has the speed guard repaying and the
// auto rule off on nearly every pair, so "hold instead of his old pixels" held EVERY second picture: 0.0 % shown (lost 259 of 259, game side) at ~30 fps, for as long as the stage
// lasted (GXV_HRW_STAGE_AUTO then fell to the locked even 30 because the windows scored low and the fresh player was off). A hold gives the game thread nothing back (the pair's
// two turns are played either way), so it is only worth it while it is the exception. Bounded: when 16 of the last 32 pairs were held the holds stop (his old pixels moved by
// the box are shown, as GXV_HRW_MARIOTIME 0 does; the census counts them as STALE) until 32 pairs in a row had the fresh draw; then holding is back. GXV_HRW_MARIOTIME_LIMIT=0: as before.
struct HoldLimit {
    uint32_t hist = 0;       // the last 32 pairs that wanted a hold or had the fresh draw, bit set = wanted a hold (while not limited)
    uint32_t freshRun = 0;   // pairs in a row with the fresh draw, while limited
    bool stale = false;      // limited: no holds
    uint32_t entered = 0;    // times it limited
    static constexpr int kEnter = 16;
    static constexpr uint32_t kExit = 32;
    // One pair (with a player in the picture): wantHold = HoldInstead said so; fresh = it has the fresh draw. Returns whether to hold it.
    bool Note(bool wantHold, bool fresh) {
        if (stale) {
            freshRun = fresh ? freshRun + 1 : 0;
            if (freshRun >= kExit) { stale = false; hist = 0; freshRun = 0; }
            return false;
        }
        hist = (hist << 1) | (wantHold ? 1u : 0u);
        if (__builtin_popcount(hist) >= kEnter) { stale = true; freshRun = 0; ++entered; return false; }
        return wantHold;
    }
};

// ---- the auto rule ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Mode 0 / 1: exactly the rule vgx_galaxy.cpp had since warp32 PATCH22 (the test compares it with a copy of that code). Mode 2: the cost rule above.
struct AutoConfig {
    bool on = true;           // GXV_HRW_FRESH_AUTO
    double offMs = 31.0;      // GXV_HRW_FRESH_AUTO_OFF_MS10 / 10
    double onMs = 27.0;       // GXV_HRW_FRESH_AUTO_ON_MS10 / 10 (the old rule's way back)
    uint32_t calmPairs = 90;  // GXV_HRW_FRESH_AUTO_PAIRS (the old rule's way back)
    bool cost = false;        // GXV_HRW_MARIOTIME=2
    double estInit = 1.0;     // the fresh draw's cost before one was measured (console 18 Nov: 0.35-2.0 ms a 5 s window's mean)
};
class FreshAuto {
public:
    static constexpr double kFrameMs = 1000.0 / 30.0;   // (a pair's two turns have one 30 picture's time)
    void Configure(const AutoConfig& c) { c_ = c; est_ = c.estInit; }
    // One finished pair: its game-thread time (drawn + undrawn turn), whether it had the fresh draw and what that draw cost the game thread.
    void Pair(double pairMs, bool freshDrawn, double freshMs) {
        if (!c_.on) return;
        if (!c_.cost) {  // (the old rule, unchanged)
            hist_ = ((hist_ << 1) | (pairMs > c_.offMs ? 1u : 0u)) & 0xFFFFu;
            if (!off_) {
                if (Pop(hist_) >= 3) { off_ = true; calm_ = 0; ++offN_; }
            } else {
#if MARIOTIME_MUTANT == 5
                calm_ = pairMs < c_.onMs ? calm_ + 1 : 0;
                if (calm_ >= c_.calmPairs / 2) { off_ = false; hist_ = 0; ++onN_; }
#else
                calm_ = pairMs < c_.onMs ? calm_ + 1 : 0;
                if (calm_ >= c_.calmPairs) { off_ = false; hist_ = 0; ++onN_; }
#endif
            }
            return;
        }
        if (freshDrawn && freshMs >= 0.0) est_ += (freshMs - est_) * 0.125;
        const double e = est_ > 0.05 ? est_ : 0.05;
#if MARIOTIME_MUTANT == 3
        const bool blame = pairMs > c_.offMs;
#else
        const double f = freshDrawn ? (freshMs > 0.0 ? freshMs : 0.0) : e;          // (what the fresh draw cost this pair, or would have cost)
        const double without = freshDrawn ? pairMs - f : pairMs, with = without + f;
        const bool blame = (without <= c_.offMs && with > c_.offMs) || (without <= kFrameMs && with > kFrameMs);   // (it alone takes the pair over the limit or over the frame)
#endif
        hist_ = ((hist_ << 1) | (blame ? 1u : 0u)) & 0xFFFFu;
        if (!off_) {
            if (Pop(hist_) >= 3) { off_ = true; calm_ = 0; ++offN_; }
        } else {
            ++calm_;
#if MARIOTIME_MUTANT == 4
            if (false) { off_ = false; ++onN_; }
#else
            if (calm_ >= 16 && Pop(hist_) <= 1) { off_ = false; hist_ = 0; ++onN_; }
#endif
        }
    }
    bool Off() const { return off_; }
    double Est() const { return est_; }
    uint32_t OffSwitches() const { return offN_; }
    uint32_t OnSwitches() const { return onN_; }
    void TakeSwitches(uint32_t* off, uint32_t* on) { *off = offN_; *on = onN_; offN_ = onN_ = 0; }
private:
    static int Pop(uint32_t v) { int n = 0; for (; v; v &= v - 1) ++n; return n; }
    AutoConfig c_;
    uint32_t hist_ = 0, calm_ = 0, offN_ = 0, onN_ = 0;
    bool off_ = false;
    double est_ = 1.0;
};

// ---- the census --------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// One window (the "hrw:" lines' 5 s). Game thread fields and render thread fields are summed by the caller into one of these before Format.
struct Census {
    uint32_t pairs = 0;             // pairs that reached the undrawn turn's decision with a second picture asked or held
    uint32_t fresh = 0;             // ... asked with the fresh draw (the game thread's view)
    uint32_t stale[kWhyCount] = {}; // second pictures that went out WITH his old pixels (mode 0), by reason (game side + render side)
    uint32_t held[kWhyCount] = {};  // second pictures held instead (modes 1, 2): the real picture stayed
    uint32_t noPlayer = 0;          // no fresh draw and no player in the picture: the plain warp, nothing of him stale
    uint32_t empty = 0;             // the fresh draw drew nothing (he is hidden: a pipe, a cannon, a stage's arrival): the plain warp
    uint32_t merged = 0;            // render: second pictures with the fresh draw merged (the truth for his pose)
    // the stale pictures' size: his own move between the two turns (the old pose is moved by it with the box; without a box this is the position error)
    double staleMoveSum = 0.0, staleMoveMax = 0.0;
    // the fresh draw's camera (GXV_HRW_FRESH_LATE: the draws come after the undrawn turn's update): pairs where the camera of that update differed from the pose's turn's
    uint32_t camPairs = 0, camMoved = 0, camSnap = 0;
    double camDegMax = 0.0, camUnitsMax = 0.0;
    uint32_t autoOffN = 0, autoOnN = 0;
    double est = 0.0;
};
inline uint32_t StaleTotal(const Census& c) { uint32_t t = 0; for (int i = 1; i < kWhyCount; ++i) t += c.stale[i]; return t; }
inline uint32_t HeldTotal(const Census& c) { uint32_t t = 0; for (int i = 1; i < kWhyCount; ++i) t += c.held[i]; return t; }
inline int Format(char* out, size_t n, int mode, const Census& c) {
    const uint32_t st = StaleTotal(c), hd = HeldTotal(c);
    return snprintf(out, n,
                    "hrw: mariotime | GXV_HRW_MARIOTIME %d: pairs %u, asked with the fresh Mario %u, merged (his pose of the picture's own turn) %u | STALE Mario shown %u "
                    "(auto off %u, speed guard %u, given up %u, deadline missed %u, not merged %u; his move on them mean %.1f max %.1f units) | held as a real 30 picture instead %u "
                    "(auto off %u, speed guard %u, given up %u, deadline missed %u, not merged %u) | no Mario in the picture %u, drawn nothing %u | fresh camera: %u pairs, the camera "
                    "moved in the update before the draws %u (max %.2f deg, %.1f units), the pose's camera used %u | auto (%s): off %u, on %u, fresh cost estimate %.2f ms",
                    mode, c.pairs, c.fresh, c.merged, st, c.stale[kAutoOff], c.stale[kGuard], c.stale[kGivenUp], c.stale[kDeadline], c.stale[kMerge],
                    st ? c.staleMoveSum / st : 0.0, c.staleMoveMax, hd, c.held[kAutoOff], c.held[kGuard], c.held[kGivenUp], c.held[kDeadline], c.held[kMerge], c.noPlayer, c.empty,
                    c.camPairs, c.camMoved, c.camDegMax, c.camUnitsMax, c.camSnap, mode == kHoldCost ? "cost" : "as before", c.autoOffN, c.autoOnN, c.est);
}

// ---- the camera's change between two view matrices (3 x 4, row major: the game's view matrix) ----------------------------------------------------------------------------------------
// Angle of R1 R0^T in degrees and the distance between the two camera positions (-R^T t) in world units.
inline void CameraDelta(const float a[12], const float b[12], double* deg, double* units) {
    double tr = 0.0;
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) tr += static_cast<double>(b[i * 4 + k]) * a[i * 4 + k];
    double c = (tr - 1.0) * 0.5;
    c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
    *deg = std::acos(c) * 57.29577951308232;
    double pa[3], pb[3];
    for (int k = 0; k < 3; ++k) {
        pa[k] = -(static_cast<double>(a[0 * 4 + k]) * a[3] + static_cast<double>(a[1 * 4 + k]) * a[7] + static_cast<double>(a[2 * 4 + k]) * a[11]);
        pb[k] = -(static_cast<double>(b[0 * 4 + k]) * b[3] + static_cast<double>(b[1 * 4 + k]) * b[7] + static_cast<double>(b[2 * 4 + k]) * b[11]);
    }
    *units = std::sqrt((pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[1] - pb[1]) * (pa[1] - pb[1]) + (pa[2] - pb[2]) * (pa[2] - pb[2]));
}

}  // namespace mariotime
}  // namespace port
