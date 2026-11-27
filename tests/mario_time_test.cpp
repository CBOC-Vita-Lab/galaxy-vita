// mariolag31: host test of port/mario_time.h (GXV_HRW_MARIOTIME). Build and run: tools/test_mario_time.sh (also runs the header's mutants: each must FAIL).
//
// 1. the rules (which reason, hold or not, render hold);
// 2. the old auto rule, as the header keeps it for modes 0 / 1, against a copy of vgx_galaxy.cpp's code before mariolag31 (identity on random traces);
// 3. the cost rule on pair-cost traces shaped like the console's runs of 18 Nov (K0a Honeyhive: pairs 26-28 ms with spikes over 31 that come with or without the fresh draw);
// 4. a synthetic jump arc through the pair rhythm: Mario's shown pose and position in every picture against the truth of that picture's turn, per mode; stale pictures counted;
//    extrapolation and interpolation shown for comparison (what they would cost);
// 5. the census line and the camera delta.
#include "port/mario_time.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace port::mariotime;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// ---- 2. the old rule, verbatim from vgx_galaxy.cpp (port_hrw_turn_end, warp32 PATCH22) --------------------------------------------------------------------------------------------------
struct OldAuto {
    uint32_t hist = 0, calm = 0, offN = 0, onN = 0;
    bool off = false;
    void Pair(double pair, double offMs, double onMs, uint32_t calmPairs) {
        hist = ((hist << 1) | (pair > offMs ? 1u : 0u)) & 0xFFFFu;
        if (!off) {
            if (__builtin_popcount(hist) >= 3) { off = true; calm = 0; ++offN; }
        } else {
            calm = pair < onMs ? calm + 1 : 0;
            if (calm >= calmPairs) { off = false; hist = 0; ++onN; }
        }
    }
};

// ---- pair-cost traces -------------------------------------------------------------------------------------------------------------------------------------------------------------------
// base: the game thread's pair WORK without the fresh draw; spike: a slow pair that is slow whatever the fresh player does (stage work, a stream read); the fresh draw adds its own
// cost; sleep: GXV_LATE_TURN's planned sleep inside the undrawn turn (idle time on purpose; the old rule's meter counts it, the cost rule's leaves it out).
struct Trace {
    double mean, sd, spikeP, spikeLo, spikeHi, freshMean, freshTailP, freshTailMax, sleepMean;
};
// console 18 Nov r2-601 K0a Honeyhive, the old rule's off stretch: pair (with the sleep) 26.9 ms mean, a window's max 31.4 mean; the undrawn turn slept 2.4-3.9 ms on purpose
// (late-turn lines) and still had 4.0-5.7 ms idle; fresh draw 0.35-2.0 ms a window's mean, single draws to 8.7 ms
static const Trace kK0a = {23.9, 1.3, 0.06, 28.2, 31.0, 0.7, 0.02, 8.0, 3.0};
static const Trace kLight = {17.8, 1.2, 0.01, 24.0, 27.0, 0.6, 0.01, 8.0, 5.0};  // SOAK Honeyhive: pairs 22.8 ms mean with the fresh player; slept 4-7 ms, 4-5 ms idle left
static const Trace kSpiky = {22.0, 1.0, 0.12, 31.5, 36.0, 0.7, 0.02, 8.0, 0.0};  // room, but 12 % of the pairs are slow (over the limit) with or without the fresh draw
static const Trace kHeavy = {30.9, 0.8, 0.05, 31.5, 34.0, 1.0, 0.02, 8.0, 0.0};  // a stage with no room (no idle, no sleep): the fresh draw itself goes over
struct PairCost { double base, fresh, sleep; };
static std::vector<PairCost> MakeTrace(const Trace& t, int n, unsigned seed) {
    std::mt19937 r(seed);
    std::normal_distribution<double> nd(t.mean, t.sd);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::exponential_distribution<double> ex(1.0 / t.freshMean);
    std::vector<PairCost> v(n);
    for (int i = 0; i < n; ++i) {
        double b = nd(r);
        if (u(r) < t.spikeP) b = t.spikeLo + (t.spikeHi - t.spikeLo) * u(r);
        double f = 0.25 + ex(r) * 0.6;
        if (u(r) < t.freshTailP) f = 2.0 + (t.freshTailMax - 2.0) * u(r);
        v[i] = {b, f, t.sleepMean};   // (GXV_LATE_TURN's nominal sleep in this trace; LateSleep takes the fresh draw's cost off it, as the console's adaptive plan does)
    }
    return v;
}
struct AutoRun { double freshShare; int overrun; int overrunNoFresh; int offN, onN; };
// GXV_LATE_TURN's planned sleep, modelled: it plans from the idle the recent undrawn turns left, so a fresh draw that is on takes its (average) cost off the sleep and the idle
// left before the retrace stays (console 18 Nov: missed retraces 2 in 20,819 undrawn turns with the fresh player on, SOAK; 0 in 3,069 on and 0 in 5,400 off, K0a). It is planned
// BEFORE the pair's work is known: a slow pair, or a slow fresh draw, is not shortened by it. The slack: what is left before the next picture is late (the console's 4.0-5.7 ms
// idle left); a pair whose wall time passes its trace's usual wall time by more than that is an "overrun" (the next picture late).
struct LateSleep {
    double freshEma = 0.0;
    double Plan(double nominal) const { const double s = nominal - freshEma; return s < 0.0 ? 0.0 : s; }
    void Done(bool freshOn, double freshMs) { freshEma += ((freshOn ? freshMs : 0.0) - freshEma) / 16.0; }
};
// (without a planned sleep the pair is late only past its 33.3 ms)
static double Limit(const PairCost& p, double traceMean) { return p.sleep > 0.0 && traceMean + p.sleep + 5.0 < 1000.0 / 30.0 ? traceMean + p.sleep + 5.0 : 1000.0 / 30.0; }
// Runs a trace through a rule: the pair's work is base + fresh when the rule had the fresh draw on for it; its wall time adds the planned sleep. A pair over 33.3 ms of wall time
// makes the next picture late (the "overrun").
template <class R> static AutoRun RunAuto(const std::vector<PairCost>& tr, double traceMean, R&& rule) {
    AutoRun o{0, 0, 0, 0, 0};
    int with = 0;
    LateSleep ls;
    for (const PairCost& p : tr) {
        const bool on = !rule.IsOff();
        const double sleep = ls.Plan(p.sleep);
        const double work = p.base + (on ? p.fresh : 0.0), wall = work + sleep;
        if (on) ++with;
        if (wall > Limit(p, traceMean)) ++o.overrun;
        if (p.base + p.sleep > Limit(p, traceMean)) ++o.overrunNoFresh;
        rule.Feed(wall, work, on, on ? p.fresh : 0.0);
        ls.Done(on, p.fresh);
    }
    o.freshShare = static_cast<double>(with) / tr.size();
    return o;
}
struct NewRule {
    FreshAuto a;
    explicit NewRule(bool cost) : cost_(cost) { AutoConfig c; c.cost = cost; a.Configure(c); }
    bool IsOff() const { return a.Off(); }
    void Feed(double wall, double work, bool drawn, double ms) { a.Pair(cost_ ? work : wall, drawn, ms); }   // (vgx_galaxy.cpp: mode 2 hands the work, modes 0 / 1 the wall time)
    bool cost_;
};

// ---- 4. the jump arc through the pair rhythm ----------------------------------------------------------------------------------------------------------------------------------------------
// The game: 60 turns a second; Mario runs at 10 units a turn and jumps every 47 turns (v0 = 22 units a turn up, gravity 1.6 a turn^2, the ground at y = 0). His "pose" is the turn
// it was computed for (a pose's age in turns is what the eye sees as "still in the air"). Some stretches have no Mario in the picture (a pipe, a cut away).
struct State { double x, y; bool air; int turn; bool present; };
static std::vector<State> Arc(int turns) {
    std::vector<State> s(turns + 2);
    double y = 0, vy = 0;
    for (int t = 0; t < turns + 2; ++t) {
        if (t % 47 == 5 && y <= 0.0) vy = 22.0;
        y += vy; vy -= 1.6;
        if (y <= 0.0) { y = 0.0; vy = 0.0; }
        const bool present = !((t / 600) % 5 == 4 && t % 600 < 90);   // (90 turns of every 3000 without Mario in the picture)
        s[t] = {10.0 * t, y, y > 0.0, t, present};
    }
    return s;
}
struct Shown { double x, y; bool air; int poseTurn; bool held; };
struct SimOut {
    int pictures = 0, bPictures = 0, fresh = 0, stale = 0, held = 0, noPlayerPlain = 0, airAfterLanding = 0, heldWithoutPlayer = 0;
    int poseAgeMax = 0;           // in pictures that are not holds
    double posErrMax = 0;         // his centre, pictures that are not holds
    int distinct = 0;             // pictures that differ from the one before (the 60's share)
    int overruns = 0;             // pairs over 33.3 ms of wall time (the next picture late)
};
struct SimCfg {
    int mode = 0;
    bool cost = false;
    bool box = true;              // the old pixels moved by his centre's move (GXV_HRW_WARP_DEPTH=4); false: warped with the scene (at his old place)
    double deadlineP = 0.004, guardP = 0.004;
    int freshViewAge = 0;         // 0: the view pass at the undrawn turn's start (the code); 1: a pass left as the first picture's (GXV_HRW_FRESH_VIEW=0)
};
static SimOut Simulate(const SimCfg& c, const std::vector<PairCost>& tr, unsigned seed) {
    const int pairs = static_cast<int>(tr.size());
    const std::vector<State> S = Arc(2 * pairs + 2);
    std::mt19937 r(seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    NewRule rule(c.cost);
    LateSleep ls;
    SimOut o;
    Shown prev{-1e9, -1e9, false, -1, false};
    auto show = [&](const Shown& sh, const State& truth) {
        ++o.pictures;
        if (sh.x != prev.x || sh.y != prev.y || sh.poseTurn != prev.poseTurn) ++o.distinct;
        prev = sh;
        if (sh.held || !truth.present) return;
        const int age = truth.turn - sh.poseTurn;
        if (age > o.poseAgeMax) o.poseAgeMax = age;
        const double e = std::hypot(sh.x - truth.x, sh.y - truth.y);
        if (e > o.posErrMax) o.posErrMax = e;
        if (!truth.air && sh.air) ++o.airAfterLanding;
    };
    for (int k = 1; k < pairs; ++k) {
        const State& sa = S[2 * k - 1];     // A: the drawn turn 2k draws the state the undrawn turn 2k-1 left
        const State& sb = S[2 * k];         // B: the drawn turn's own update (the undrawn turn 2k+1 draws him fresh from it)
        show({sa.x, sa.y, sa.air, sa.turn, false}, sa);
        // the pair's B
        PairIn in;
        in.autoOff = rule.IsOff();
        in.guard = u(r) < c.guardP;
        const int why0 = NoFresh(in);
        const bool player = sb.present && sa.present;
        Shown b;
        int why = why0;
        bool freshDrawn = false;
        if (why0 == kFresh) {
            freshDrawn = true;
            if (player && u(r) < c.deadlineP) why = kDeadline;   // (the render thread's deadline: the partial draw came too late; without him the partial draw is empty at once)
        }
        ++o.bPictures;
        const bool hold = why0 != kFresh ? HoldInstead(c.mode, why, player) : (why == kDeadline && RenderHold(c.mode));
        if (hold) {
            b = {sa.x, sa.y, sa.air, sa.turn, true};
            ++o.held;
            if (!player) ++o.heldWithoutPlayer;
        } else if (why == kFresh) {
            const State& pose = S[2 * k - c.freshViewAge];
            b = {sb.x, sb.y, pose.air, pose.turn, false};
            ++o.fresh;
        } else if (!player) {
            b = {sb.x, sb.y, sb.air, sb.turn, false};   // (nothing of him in the picture: the plain warp is right for him)
            ++o.noPlayerPlain;
        } else {
            // his pixels of A: the box moves them by his centre's move (the pose stays A's); without the box they stay where A had him
            b = c.box ? Shown{sb.x, sb.y, sa.air, sa.turn, false} : Shown{sa.x, sa.y, sa.air, sa.turn, false};
            ++o.stale;
        }
        show(b, sb);
        const PairCost& p = tr[k];
        const double work = p.base + (freshDrawn ? p.fresh : 0.0);
        const double sleep = ls.Plan(p.sleep);
        if (work + sleep > Limit(p, kK0a.mean)) ++o.overruns;
        rule.Feed(work + sleep, work, freshDrawn, freshDrawn ? p.fresh : 0.0);
        ls.Done(freshDrawn, p.fresh);
    }
    return o;
}

int main() {
    // ---- 1. rules
    {
        PairIn p;
        CHECK(NoFresh(p) == kFresh);
        p.autoOff = true; CHECK(NoFresh(p) == kAutoOff);
        p = PairIn{}; p.pairNoFresh = true; CHECK(NoFresh(p) == kAutoOff);
        p = PairIn{}; p.guard = true; CHECK(NoFresh(p) == kGuard);
        p = PairIn{}; p.givenUp = true; p.guard = true; CHECK(NoFresh(p) == kGivenUp);
        p = PairIn{}; p.freshOn = false; CHECK(NoFresh(p) == kGivenUp);
        for (int w = 0; w < kWhyCount; ++w)
            for (int pl = 0; pl < 2; ++pl) {
                CHECK(!HoldInstead(kOff, w, pl));
                CHECK(HoldInstead(kHold, w, pl) == (w != kFresh && pl));
                CHECK(HoldInstead(kHoldCost, w, pl) == (w != kFresh && pl));
            }
        CHECK(!RenderHold(kOff) && RenderHold(kHold) && RenderHold(kHoldCost));
        CHECK(ClampMode(-3) == 0 && ClampMode(1) == 1 && ClampMode(2) == 2 && ClampMode(9) == 2);
    }
    // ---- 2. identity of the old rule (modes 0 / 1)
    {
        std::mt19937 r(7);
        int mismatches = 0, switches = 0;
        for (int seq = 0; seq < 2000; ++seq) {
            OldAuto o;
            FreshAuto n;
            AutoConfig c;
            const double offMs = 25.0 + (r() % 100) / 10.0, onMs = offMs - (r() % 60) / 10.0;
            const uint32_t calm = 1 + r() % 120;
            c.offMs = offMs; c.onMs = onMs; c.calmPairs = calm;
            n.Configure(c);
            std::normal_distribution<double> nd(offMs - 3.0, 3.0);
            for (int i = 0; i < 400; ++i) {
                const double pair = nd(r);
                o.Pair(pair, offMs, onMs, calm);
                n.Pair(pair, (r() & 1) != 0, (r() % 50) / 10.0);
                if (o.off != n.Off()) ++mismatches;
            }
            if (o.offN != n.OffSwitches() || o.onN != n.OnSwitches()) ++mismatches;
            switches += o.offN + o.onN;
        }
        std::printf("old rule identity: 2000 sequences x 400 pairs, %d switches, mismatches %d\n", switches, mismatches);
        CHECK(mismatches == 0);
        CHECK(switches > 1000);
    }
    // ---- 3. the cost rule on the console-shaped traces
    struct Old { OldAuto a; bool IsOff() const { return a.off; } void Feed(double wall, double, bool, double) { a.Pair(wall, 31.0, 27.0, 90); } };
    {
        const Trace* tr[4] = {&kK0a, &kLight, &kHeavy, &kSpiky};
        const char* nm[4] = {"K0a-like (Honeyhive heavy)", "SOAK-like (light)", "no room (31 ms)", "room, 12 % slow pairs not of the fresh draw's making"};
        AutoRun res[4][2];
        for (int t = 0; t < 4; ++t) {
            const std::vector<PairCost> v = MakeTrace(*tr[t], 18000, 11 + t);   // (18,000 pairs = 10 minutes)
            Old old;
            NewRule cost(true);
            res[t][0] = RunAuto(v, tr[t]->mean, old);
            res[t][1] = RunAuto(v, tr[t]->mean, cost);
            std::printf("auto rule, %s: fresh on %.1f %% (old) vs %.1f %% (cost); pairs past their slack (the next picture late) %d (old) vs %d (cost), %d of them late without the fresh draw\n",
                        nm[t], 100 * res[t][0].freshShare, 100 * res[t][1].freshShare, res[t][0].overrun, res[t][1].overrun, res[t][0].overrunNoFresh);
        }
        CHECK(res[0][0].freshShare < 0.5);    // (the console's K0a: 22 % of the second pictures had him fresh)
        CHECK(res[0][1].freshShare > 0.9);
        // the price of the fresh pictures: late pairs it causes. In modes 1 / 2 a pair without the fresh draw loses its second picture for certain (held), a pair with it loses
        // a picture only when it is late; the cost rule must keep that under 1 late pair in 20 fresh pairs gained (the model's measure; the console's is the missed retraces).
        const auto price = [](const AutoRun& o, const AutoRun& c) { return (c.overrun - o.overrun) <= 0.05 * (c.freshShare - o.freshShare) * 18000.0 + 20.0; };
        CHECK(price(res[0][0], res[0][1]));
        CHECK(res[1][0].freshShare > 0.9 && res[1][1].freshShare > 0.9);
        CHECK(res[2][1].freshShare < 0.5);    // (no room: the cost rule keeps it off too)
        CHECK(price(res[2][0], res[2][1]));
        CHECK(res[3][0].freshShare < 0.5 && res[3][1].freshShare > 0.9);   // (slow pairs the fresh draw does not cause do not count against it)
        CHECK(price(res[3][0], res[3][1]));
    }
    // ---- 4. the jump arc
    {
        const std::vector<PairCost> v = MakeTrace(kK0a, 18000, 5);
        SimCfg c0; c0.mode = 0;
        SimCfg c1; c1.mode = 1;
        SimCfg c2; c2.mode = 2; c2.cost = true;
        SimCfg c0nb = c0; c0nb.box = false;
        SimCfg cview = c0; cview.freshViewAge = 1; cview.deadlineP = 0; cview.guardP = 0;
        const SimOut o0 = Simulate(c0, v, 3), o1 = Simulate(c1, v, 3), o2 = Simulate(c2, v, 3), o0nb = Simulate(c0nb, v, 3), ov = Simulate(cview, v, 3);
        const SimOut* all[5] = {&o0, &o0nb, &o1, &o2, &ov};
        const char* nm[5] = {"MARIOTIME=0 (today)", "MARIOTIME=0 without the box", "MARIOTIME=1", "MARIOTIME=2", "a pose pass left as A's (FRESH_VIEW=0 model)"};
        for (int i = 0; i < 5; ++i) {
            const SimOut& o = *all[i];
            std::printf("jump arc, %s: pictures %d (second %d): fresh %d, STALE %d, held %d, no Mario (plain) %d | shown 'in the air' after landing %d | pose age max %d turns | "
                        "centre error max %.1f units | new pictures a second %.1f | late pairs %d\n",
                        nm[i], o.pictures, o.bPictures, o.fresh, o.stale, o.held, o.noPlayerPlain, o.airAfterLanding, o.poseAgeMax, o.posErrMax, 60.0 * o.distinct / o.pictures, o.overruns);
        }
        CHECK(o0.stale > 1000 && o0.airAfterLanding > 0 && o0.poseAgeMax == 1 && o0.held == 0);
        CHECK(o0nb.posErrMax > 1.0);     // (without the box the stale Mario is also at his old place)
        CHECK(o0.posErrMax < 1e-9);      // (with the box his centre is right, only the pose is old)
        CHECK(o1.stale == 0 && o1.airAfterLanding == 0 && o1.poseAgeMax == 0 && o1.posErrMax < 1e-9);
        CHECK(o1.held > 0 && o1.heldWithoutPlayer == 0);
        CHECK(o2.stale == 0 && o2.airAfterLanding == 0 && o2.poseAgeMax == 0 && o2.heldWithoutPlayer == 0);
        CHECK(o2.held < o1.held / 4);    // (the cost rule keeps the 60: far fewer holds than mode 1 with the old rule)
        CHECK(o2.held + o2.overruns < (o1.held + o1.overruns) / 4);   // (pictures lost either way: held, or a late pair)
        CHECK(o2.distinct > o1.distinct);
        CHECK(o1.noPlayerPlain > 0 && o2.noPlayerPlain >= 0);
        CHECK(ov.poseAgeMax == 1 && ov.airAfterLanding > 0);   // (the model sees a one-turn-old pose pass: the console's counter for it is 0 in every run)
        // the fresh second picture is exact at every turn of the arc, landings included: nothing to extrapolate. For comparison, what the alternatives would show:
        //  extrapolating A's motion (x, y + vy) to B's turn overshoots into the ground at every landing; interpolating needs the turn after B (one turn = 16.7 ms more latency).
        const std::vector<State> S = Arc(4000);
        double exMax = 0; int exUnder = 0;
        for (int t = 2; t < 4000; t += 2) {
            const double vy = S[t - 1].y - S[t - 2].y, ex = S[t - 1].y + vy;
            const double e = std::fabs(ex - S[t].y);
            if (e > exMax) exMax = e;
            if (ex < -0.5) ++exUnder;
        }
        std::printf("for comparison: extrapolating his jump to the second picture's turn errs up to %.1f units (below the ground at %d landings); interpolating would need the next turn: "
                    "+16.7 ms latency; the fresh draw of the turn itself: 0 error, 0 latency added\n", exMax, exUnder);
        CHECK(exMax > 1.0);
    }
    // ---- 5. census line and camera delta
    {
        Census c;
        c.pairs = 150; c.fresh = 60; c.merged = 58; c.stale[kAutoOff] = 85; c.stale[kDeadline] = 2; c.held[kGuard] = 3; c.noPlayer = 2; c.empty = 1;
        c.staleMoveSum = 870.0; c.staleMoveMax = 22.0; c.camPairs = 60; c.camMoved = 40; c.camDegMax = 1.5; c.camUnitsMax = 12.0; c.camSnap = 0;
        char line[1100];
        const int n = Format(line, sizeof line, 0, c);
        std::printf("%s\n", line);
        CHECK(n > 0 && n < static_cast<int>(sizeof line));
        CHECK(std::strstr(line, "STALE Mario shown 87 (auto off 85, speed guard 0, given up 0, deadline missed 2, not merged 0; his move on them mean 10.0 max 22.0 units)") != nullptr);
        CHECK(std::strstr(line, "held as a real 30 picture instead 3 (auto off 0, speed guard 3,") != nullptr);
        CHECK(StaleTotal(c) == 87 && HeldTotal(c) == 3);
        const float a[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        const double th = 3.0 / 57.29577951308232;
        const float b[12] = {static_cast<float>(std::cos(th)), 0, static_cast<float>(std::sin(th)), -10.0f, 0, 1, 0, 0, static_cast<float>(-std::sin(th)), 0, static_cast<float>(std::cos(th)), 0};
        double deg = 0, units = 0;
        CameraDelta(a, b, &deg, &units);
        std::printf("camera delta: %.3f deg, %.3f units (expect 3, 10)\n", deg, units);
        CHECK(std::fabs(deg - 3.0) < 1e-3 && std::fabs(units - 10.0) < 1e-3);
        CameraDelta(a, a, &deg, &units);
        CHECK(deg < 1e-3 && units < 1e-6);
    }
    // ---- 6. lost49: the holds are bounded (the owner's log 905: every second picture held, 0.0 % shown)
    {
        HoldLimit hl;
        int held = 0, n = 0;
        for (int i = 0; i < 300; ++i) { ++n; if (hl.Note(true, false)) ++held; }   // a stage where no pair has the fresh draw
        std::printf("lost49: all-want-hold stage: %d of %d held, limited %d\n", held, n, hl.stale ? 1 : 0);
        CHECK(held == HoldLimit::kEnter - 1 && hl.stale && hl.entered == 1);   // the first 15 held, then none: second pictures shown from pair 16 on
        CHECK(!RenderHold(kHoldCost, hl.stale) && RenderHold(kHoldCost, false));
        for (int i = 0; i < 31; ++i) hl.Note(false, true);
        CHECK(hl.stale);                                                      // 31 fresh pairs: not yet
        hl.Note(false, false);                                                // a pair without it resets the run
        for (int i = 0; i < 31; ++i) hl.Note(false, true);
        CHECK(hl.stale);
        hl.Note(false, true);
        CHECK(!hl.stale && hl.hist == 0);                                     // 32 in a row: holding is back
        int h2 = 0;
        for (int i = 0; i < 400; ++i) if (hl.Note(i % 8 == 0, i % 8 != 0)) ++h2;   // the exception (1 in 8): every one is held
        CHECK(h2 == 50 && !hl.stale);
    }
    std::printf(g_fail ? "mario_time: %d FAILED\n" : "mario_time: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
