// GXV_STALL_FORGIVE (frame_pace.h StallForgiveNs, LegacySkip / Controller SetForgive): what the pacing rules do after one long turn.
// A model of the game thread's loop in virtual time around the REAL port::pace::LegacySkip (Performance / "up to 60": one turn per
// picture) and port::pace::Controller (Fixed30: Quality and Locked 30), and of MainLoopFramework's waitForTick on the retrace queue
// (JUTVideo: a message queue of ONE slot filled by the retrace interrupt without blocking, so after a stall it holds the first retrace
// since the last receive, and the wait of the turn that ended the stall takes it at once). Turn costs are normal around the console's
// (Honeyhive: a turn that skips ~9.5 ms, a drawn turn 15 ms here, a little under the 16.7 ms retrace so the model alone stays on time); the back end is a serial queue of 12 ms pictures with a gate at 2 in
// flight. Nothing here is a console measurement. Prints a table, and checks:
//   (a) with the switch a stall of 80 ms .. 3 s leaves at most ONE turn without a picture afterwards, and the picture rate and the
//       game clock are back to normal within 2 pictures (the clock offset does not move after the stalled turn);
//   (b) without a stall, the decisions with the switch on (80 ms) and off are identical over a long random sequence of turn times
//       (ordinary slow frames of 17-80 ms included), for both rules;
//   (c) the TurnClock resync, and the clamping of the switch's value.
//   tools/test_stall_forgive.sh
#include "port/frame_pace.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

namespace {
constexpr double kP = 16.68335;  // ms
constexpr long kForgiveMs = 80;  // the recommended console value (the debt's cap is 4 turns = 66.7 ms: a gap of 83 ms or more saturates it)

// The retrace queue (JUTVideo::mMessageQueue, 1 slot, OSSendMessage NOBLOCK) and waitForTick(retraceCount 1).
struct Retrace {
    long nextCount = -1;
    double freeSince = 0;  // when the slot was last emptied: the next message in it is the first retrace after that time
    // returns the time the wait ends; sets `stale` when the message was already there on entry
    double Wait(double t, bool& stale) {
        if (nextCount < 0) nextCount = static_cast<long>(std::floor(t / kP));
        stale = false;
        for (;;) {
            const long v = static_cast<long>(std::floor(freeSince / kP + 1e-9)) + 1;  // the retrace that filled the slot (the epsilon: a receive exactly on a vblank)
            const double at = v * kP;
            if (at > t) t = at; else stale = true;
            freeSince = t;
            if (v >= nextCount) { nextCount = v + 1; return t; }
        }
    }
};

struct Params {
    bool fixed30 = false;       // Controller(Fixed30) instead of LegacySkip
    int maxRun = 5, pipeDepth = 2, pipeRun = 6, maxStep = 3;
    double drawMs = 15.0, skipMs = 9.5, jitter = 0.5;
    double beMs = 12.0;         // back end per picture
    int inflight = 2;           // the draw waits when this many pictures are unfinished
    int64_t forgiveNs = 0;
    bool resyncClock = false;
};

struct TurnRec { double start; int code; bool waited; bool stale; double work; };

struct Trace {
    std::vector<TurnRec> turns;
    std::vector<double> shownAt;  // back end completion of each picture
    std::vector<int> pictureTurn;
};

// stallTurn >= 0: that turn's work takes stallMs more (a game-thread stall); beStallPicture >= 0: that picture takes beStallMs more
// on the back end (the game thread then blocks at the gate).
Trace Run(const Params& p, int nTurns, int stallTurn, double stallMs, int beStallPicture, double beStallMs, unsigned seed, bool randomTimes = false) {
    std::mt19937 rng(seed);
    auto cost = [&](double m) { std::normal_distribution<double> d(m, p.jitter); const double v = d(rng); return v < m * 0.5 ? m * 0.5 : v; };
    port::pace::LegacySkip legacy(p.maxRun, p.pipeDepth, p.pipeRun, nullptr, p.maxStep, p.forgiveNs);
    port::pace::Controller ctl(false, port::pace::Policy::Fixed30);
    ctl.SetForgive(p.forgiveNs);
    Retrace rt;
    Trace tr;
    double t = 5.0, beFree = 0;
    std::vector<double> beDone;
    int pictures = 0;
    for (int n = 0; n < nTurns; ++n) {
        unsigned pending = 0;
        for (double x : beDone) if (x > t) ++pending;
        int code;
        if (p.fixed30) code = ctl.Turn(static_cast<uint64_t>(t * 1000.0), true, false, pending, 0);
        else code = legacy.Turn(static_cast<int64_t>(t * 1e6), true, pending).code;
        double w = t + (code == 0 ? cost(p.drawMs) : cost(p.skipMs));
        if (randomTimes) { std::uniform_real_distribution<double> u(0, 1); const double r = u(rng); w += r < 0.1 ? 10.0 + 40.0 * u(rng) : r < 0.3 ? 5.0 * u(rng) : 0.0; }
        if (n == stallTurn) w += stallMs;
        if (code == 0) {
            // the gate: with `inflight` pictures unfinished the draw waits for the oldest
            std::vector<double> open;
            for (double x : beDone) if (x > w) open.push_back(x);
            std::sort(open.begin(), open.end());
            while (static_cast<int>(open.size()) >= p.inflight) { w = std::max(w, open.front()); open.erase(open.begin()); }
            double d = std::max(w, beFree) + p.beMs;
            if (pictures == beStallPicture) d += beStallMs;
            beFree = d;
            beDone.push_back(d);
            if (beDone.size() > 16) beDone.erase(beDone.begin());
            tr.shownAt.push_back(d);
            tr.pictureTurn.push_back(n);
            ++pictures;
        }
        TurnRec r{t, code, false, false, w - t};
        if (code != 2) {  // a late skip takes no retrace wait (GameSystem::frameLoop)
            bool stale = false;
            const double e = rt.Wait(w, stale);
            r.waited = e > w + 1e-9;
            r.stale = stale;
            w = e;
        }
        tr.turns.push_back(r);
        t = w;
    }
    return tr;
}

struct Report {
    int skippedAfter = 0;      // turns without a picture within `window` ms of the stalled turn's end
    int lateSkips = 0;         // of them the late ones (code 2: no retrace wait, the rule paying back a debt)
    int longestRun = 0;
    int noWaitTurns = 0;       // turns after the stall that ran without a retrace wait (as fast as the work allows)
    double catchupMs = 0;      // game-clock gained on real time after the stall: how far the scene jumped ahead
    double maxHoldMs = 0;      // the longest time a picture of the turns after the stall stayed on screen (turn starts of two pictures)
    int maxStep = 0;           // the most turns one picture advanced
    int settleTurns = 0;       // turns until two pictures in a row come at the normal cadence (1 turn apart, 60 fps)
    std::string pattern;       // 'D' picture, 's' skipped (not late: has its retrace), 'S' skipped late (no wait)
};

Report Analyse(const Trace& tr, int stallTurn, double windowMs, bool fixed = false) {
    Report r;
    const int n0 = stallTurn + 1;  // the first turn after the stalled one
    const double t0 = tr.turns[n0].start;
    // game-clock offset D = turns * P - time; right after the stall it is -stall; later it may rise (catch-up)
    const double d0 = (n0)*kP - t0;
    int run = 0, lastPic = -1;
    double lastPicStart = 0;
    for (size_t i = n0; i < tr.turns.size() && tr.turns[i].start - t0 < windowMs; ++i) {
        const TurnRec& x = tr.turns[i];
        if (x.code == 2) ++r.lateSkips;
        if (x.code != 0) { ++r.skippedAfter; ++run; r.longestRun = std::max(r.longestRun, run); }
        else {
            run = 0;
            if (lastPic >= 0) {
                r.maxStep = std::max<int>(r.maxStep, static_cast<int>(i) - lastPic);
                r.maxHoldMs = std::max(r.maxHoldMs, x.start - lastPicStart);
            }
            lastPic = static_cast<int>(i);
            lastPicStart = x.start;
        }
        if (i < static_cast<size_t>(n0) + 10 && ((!x.waited && x.code != 2) || x.code == 2)) ++r.noWaitTurns;  // (the first 10 turns: those that ran without a retrace wait)
        if (r.pattern.size() < 40) r.pattern += x.code == 0 ? 'D' : x.code == 1 ? 's' : 'S';
        const double d = static_cast<double>(i + 1) * kP - (x.start + x.work);
        r.catchupMs = std::max(r.catchupMs, d - d0);
    }
    // settled: the first turn from which 4 turns in a row are regular (legacy: all draw; fixed 30: none is a late skip)
    r.settleTurns = -1;
    for (size_t i = n0; i + 3 < tr.turns.size(); ++i) {
        bool ok = true;
        for (size_t j = i; j < i + 4; ++j) ok = ok && (fixed ? tr.turns[j].code != 2 : tr.turns[j].code == 0);
        if (ok) { r.settleTurns = static_cast<int>(i - n0); break; }
    }
    return r;
}

void Table(const char* title, const Params& base, bool backendStall) {
    std::printf("\n%s\n", title);
    std::printf("  stall    | turns without picture in 2 s (longest run) [late] | no-wait turns of the first 10 | scene jump (game clock gained, ms) | longest hold (ms) | max turns/picture | settles after | pattern after the stall (D picture, s skipped with retrace, S skipped late)\n");
    const double stalls[] = {40, 60, 100, 150, 200, 249, 251, 300, 700, 2400};
    for (double s : stalls) {
        Params p = base;
        const int stallTurn = 200;
        Trace tr = backendStall ? Run(p, 600, -1, 0, 60, s, 7) : Run(p, 600, stallTurn, s, -1, 0, 7);
        int st = stallTurn;
        if (backendStall) {  // find the turn that waited at the gate (the long one)
            double best = 0;
            for (size_t i = 0; i < tr.turns.size(); ++i) if (tr.turns[i].work > best) { best = tr.turns[i].work; st = static_cast<int>(i); }
        }
        const Report r = Analyse(tr, st, 2000.0, base.fixed30);
        std::printf("  %6.0f   | %3d (%d) [%d]                              | %2d            | %6.1f                             | %6.1f            | %d                 | %2d turns     | %s\n",
                    s, r.skippedAfter, r.longestRun, r.lateSkips, r.noWaitTurns, r.catchupMs, r.maxHoldMs, r.maxStep, r.settleTurns, r.pattern.c_str());
    }
}
}  // namespace

int main() {
    // ---- the table: today's behaviour, then with the switch
    Params legacy;  // Performance, "Game speed Full": PETARI_FRAMESKIP=5, pipe depth 2 / run 6, GXV_SKIP_MAXSTEP=3
    Params fixed30 = legacy;
    fixed30.fixed30 = true;
    fixed30.drawMs = 19.0;  // a pair of turns (~28.5 ms) well under the 33.3 ms of the fixed 30 (at 22 + 9.5 the model alone drifts into late turns)
    fixed30.skipMs = 9.5;
    Params legacyOn = legacy; legacyOn.forgiveNs = port::pace::StallForgiveNs(kForgiveMs);
    Params fixedOn = fixed30; fixedOn.forgiveNs = port::pace::StallForgiveNs(kForgiveMs);
    std::printf("MODEL (virtual time; not a console measurement). Turns after one long turn; a skipped turn costs 9.5 ms, a drawn one 15 ms (19 ms in the fixed 30).\n");
    Table("Performance (LegacySkip), game thread stalled in one turn, GXV_STALL_FORGIVE off", legacy, false);
    Table("Performance (LegacySkip), game thread stalled in one turn, GXV_STALL_FORGIVE=80", legacyOn, false);
    Table("Quality / Locked 30 (Controller Fixed30), one turn stalled, off", fixed30, false);
    Table("Quality / Locked 30 (Controller Fixed30), one turn stalled, GXV_STALL_FORGIVE=80", fixedOn, false);
    Table("Performance, the BACK END stalls on one picture (the game thread blocks at the gate), off", legacy, true);
    Table("Performance, the BACK END stalls on one picture, GXV_STALL_FORGIVE=80", legacyOn, true);

    // ---- (a) with the switch: at most one turn without a picture, normal pace within 2 pictures, no game-clock gain
    for (int rule = 0; rule < 2; ++rule) {
        for (double s : {80.0, 100.0, 120.0, 180.0, 249.0, 300.0, 700.0, 2400.0, 3000.0}) {
            for (unsigned seed = 1; seed <= 20; ++seed) {
                Params p = rule ? fixedOn : legacyOn;
                const Trace tr = Run(p, 500, 200, s, -1, 0, seed);
                const Report r = Analyse(tr, 200, 2000.0, rule == 1);
                char why[160];
                std::snprintf(why, sizeof why, "rule %d stall %.0f seed %u: %d turns without a picture (%d late, longest run %d)", rule, s, seed, r.skippedAfter, r.lateSkips, r.longestRun);
                Require(rule ? r.lateSkips == 0 : r.skippedAfter <= 1, why);
                std::snprintf(why, sizeof why, "rule %d stall %.0f seed %u: game clock gained %.1f ms", rule, s, seed, r.catchupMs);
                Require(r.catchupMs < 1.5 * kP, why);
                std::snprintf(why, sizeof why, "rule %d stall %.0f seed %u: back to normal after %d turns", rule, s, seed, r.settleTurns);
                Require(r.settleTurns >= 0 && r.settleTurns <= 2, why);
            }
        }
    }
    // the back end stalling instead (the game thread blocks at the gate for ~S): with the switch no catch-up either
    for (double s : {200.0, 700.0, 2400.0}) {  // (the game thread blocks for ~S minus the slack of the queue: 150 ms and up reach the threshold)
        Params p = legacyOn;
        const Trace tr = Run(p, 600, -1, 0, 60, s, 3);
        int st = 0; double best = 0;
        for (size_t i = 0; i < tr.turns.size(); ++i) if (tr.turns[i].work > best) { best = tr.turns[i].work; st = static_cast<int>(i); }
        const Report r = Analyse(tr, st, 2000.0);
        Require(r.catchupMs < 1.5 * kP, "back end stall with the switch: game clock gained");
        Require(r.longestRun <= 2, "back end stall with the switch: a long run of skipped turns");
    }
    // and the model reproduces the fast-forward without it (so (a) is a real test): a 150 ms stall pays back the capped debt (4 turns)
    {
        const Trace tr = Run(legacy, 500, 200, 150.0, -1, 0, 1);
        const Report r = Analyse(tr, 200, 2000.0);
        Require(r.skippedAfter >= 3 && r.catchupMs > 3 * kP, "model: without the switch a 150 ms stall should be paid back by skipped turns");
        const Trace tr2 = Run(legacy, 500, 200, 700.0, -1, 0, 1);
        const Report r2 = Analyse(tr2, 200, 2000.0);
        Require(r2.skippedAfter <= 1 && r2.catchupMs < 1.5 * kP, "model: a 700 ms stall is already forgiven by the rules' own 250 ms gap test");
    }

    // ---- (b) no stall: identical decisions with the switch on and off, random turn times, both rules
    for (int rule = 0; rule < 2; ++rule) {
        for (unsigned seed = 1; seed <= 30; ++seed) {
            Params a = rule ? fixed30 : legacy, b = rule ? fixedOn : legacyOn;
            // ordinary slow turns: 25-65 ms with 10 % probability, 5 ms more with 20 %; never a gap of 80 ms
            a.jitter = b.jitter = 2.0;
            const Trace ta = Run(a, 20000, -1, 0, -1, 0, seed, true), tb = Run(b, 20000, -1, 0, -1, 0, seed, true);
            Require(ta.turns.size() == tb.turns.size(), "(b) length");
            double maxGap = 0;
            for (size_t i = 1; i < ta.turns.size(); ++i) maxGap = std::max(maxGap, ta.turns[i].start - ta.turns[i - 1].start);
            Require(maxGap < 80.0, "(b) the random sequence has a gap that is a stall");
            for (size_t i = 0; i < ta.turns.size(); ++i) {
                if (ta.turns[i].code != tb.turns[i].code || ta.turns[i].start != tb.turns[i].start) {
                    std::fprintf(stderr, "FAIL: (b) rule %d seed %u turn %zu differs: code %d vs %d\n", rule, seed, i, ta.turns[i].code, tb.turns[i].code);
                    return 1;
                }
            }
        }
    }
    // the same, driven straight into the classes with arbitrary gaps below the threshold (14-79.9 ms) and pending counts
    {
        std::mt19937 rng(99);
        for (int rule = 0; rule < 2; ++rule) {
            port::pace::LegacySkip la(5, 2, 6, nullptr, 3, 0), lb(5, 2, 6, nullptr, 3, port::pace::StallForgiveNs(kForgiveMs));
            port::pace::Controller ca(false, port::pace::Policy::Fixed30), cb(false, port::pace::Policy::Fixed30);
            cb.SetForgive(port::pace::StallForgiveNs(kForgiveMs));
            int64_t t = 1000000000;
            for (int i = 0; i < 2000000; ++i) {
                const double r = std::uniform_real_distribution<double>(0, 1)(rng);
                const double ms = r < 0.6 ? 14.0 + 6.0 * std::uniform_real_distribution<double>(0, 1)(rng) : r < 0.95 ? 20.0 + 40.0 * std::uniform_real_distribution<double>(0, 1)(rng) : 60.0 + 19.9 * std::uniform_real_distribution<double>(0, 1)(rng);
                t += static_cast<int64_t>(ms * 1e6);
                const unsigned pending = static_cast<unsigned>(rng() % 4);
                const bool allowed = (rng() % 50) != 0;
                if (rule == 0) {
                    const auto va = la.Turn(t, allowed, pending), vb = lb.Turn(t, allowed, pending);
                    if (va.code != vb.code || va.behind != vb.behind || va.gpuBusy != vb.gpuBusy || va.capped != vb.capped) { std::fprintf(stderr, "FAIL: (b) LegacySkip direct, turn %d\n", i); return 1; }
                    if (vb.forgiven) { std::fprintf(stderr, "FAIL: (b) forgave a %.1f ms gap\n", ms); return 1; }
                } else {
                    const uint64_t us = static_cast<uint64_t>(t / 1000);
                    const int da = ca.Turn(us, allowed, false, pending, 0), db = cb.Turn(us, allowed, false, pending, 0);
                    if (da != db || ca.PresentAllowed() != cb.PresentAllowed()) { std::fprintf(stderr, "FAIL: (b) Controller direct, turn %d\n", i); return 1; }
                }
            }
            Require(rule == 0 ? lb.Forgiven() == 0 : cb.Forgiven() == 0, "(b) nothing forgiven below the threshold");
        }
    }

    // ---- (c) the knob and the clock
    Require(port::pace::StallForgiveNs(0) == 0 && port::pace::StallForgiveNs(-5) == 0, "knob 0 / negative is off");
    Require(port::pace::StallForgiveNs(10) == 80000000 && port::pace::StallForgiveNs(100) == 100000000 && port::pace::StallForgiveNs(99999) == 10000000000ll, "knob is held to 80 ms .. 10 s");
    {
        port::pace::TurnClock c;
        uint32_t v = 1000;
        c.Turn(v);
        for (int i = 0; i < 5; ++i) { ++v; c.Turn(v); }
        v += 40;  // a stall of 40 vblanks
        const uint32_t stamp = c.Turn(v);
        Require(stamp == v - port::pace::TurnClock::kMaxBehind, "TurnClock stays 4 behind after a stall (today)");
        c.Reset();
        ++v;
        Require(c.Turn(v) == v, "TurnClock after Reset takes the display's counter");
    }
    std::printf("\nstall_forgive_model: all checks passed\n");
    return 0;
}
