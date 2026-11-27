// GXV_SPEED_GUARD (port/speed_guard.h): a host model of the game thread, the retrace wait and the display, run with the REAL pacing classes (port::pace::Controller Fixed30 for
// Quality, LegacySkip + Slip for Performance) and the real guard. Prints the table of the report (game speed %, pictures a second, share of three-turn pictures, longest run of
// them, holds on the display) and checks it.
//
// The model (read from the code, see REPORT.txt):
//  - a turn costs D (drawn) or N (undrawn) ms of work; after it, unless the pacer said "late" (code 2), the game waits for the retrace: the game's retrace queue holds ONE
//    message (JUTVideo.cpp OSInitMessageQueue(.., 1)), so the wait returns at once when a vblank has come since the last wait returned, else at the next vblank;
//  - pictures are shown on the first vblank after their drawn turn ended (Quality: at least 2 vblanks after the one before: port::pace::PresentationSlots; Performance: 1);
//  - the back end is never the limit (pending 0): this is the game thread's own pacing, the case in console c90 (Honeyhive pair 41 ms, GPU 20 ms).
#include "port/frame_pace.h"
#include "port/speed_guard.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace port::pace;

static int sFail = 0;
static void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++sFail; }
}

static constexpr int64_t P = SpeedGuard::kPeriodNs;
static int64_t Ms(double ms) { return static_cast<int64_t>(ms * 1e6); }

struct Scn {
    double D = 29.0, N = 12.0;       // work of a drawn / an undrawn turn, ms
    double seconds = 60;
    double jitter = 0;               // +- fraction of the cost, uniform
    double hitchAtS = -1, hitchMs = 0;   // one turn takes this long (ms), at that time
    double stallAtS = -1, stallMs = 0;   // a stall (a turn of this length, ms), at that time
    double loadAtS = -1, loadS = 0;      // a load: allowed false, covered, turns of 30 ms
    bool slip = true;                // GXV_SLIP=1 GXV_SLIP_MAX=20 (the console default)
    bool steady = true;              // Performance: the steady 30's offers (a turn is offered when the one before it did not draw)
    bool quality = true;             // Quality (Controller Fixed30) or Performance (LegacySkip, GXV_SKIP_MAXSTEP=2)
    bool guard = false;
    unsigned maxShare = 100, maxExtra = 1;
    long budgetMs = 30, leak = 2;
    bool stallForgive = false;
};
struct Res {
    double speed = 0, fps = 0, share3 = 0, longest3 = 0, extras = 0, turns = 0, wall = 0;
    double h2 = 0, h3 = 0, h4 = 0;   // holds on the display, share of pictures held 2 / 3 / 4+ vblanks (Quality) or 1 / 2 / 3+ (Performance, printed as the same columns)
    unsigned longestHold = 0, longestRun3 = 0;
    double lostMs = 0, censusSpeed = 0, censusExtras = 0;
    uint64_t hash = 1469598103934665603ull;
    int maxBucketMs = 0;
};

static uint64_t g_allHash = 1469598103934665603ull;   // (speed31: one hash of every decision of every scenario, printed at the end; compared with the base headers' by tests/speed31_offhash.sh)
static Res Run(const Scn& s) {
    Res r;
    Controller ctrl(false, Policy::Fixed30);
    Slip slip;
    if (s.slip) slip.Configure(20);
    ctrl.UseSlip(&slip);
    if (s.stallForgive) ctrl.SetForgive(80000);
    LegacySkip rule(3, 2, 6, &slip, 2, s.stallForgive ? 80000000 : 0);
    SpeedGuard g;
    {   // (speed31: this file is speed30's table: the escalation is off here; tests/speed31_test.cpp has the new one)
        SpeedGuard::Config c = MakeSpeedGuardConfig(s.guard, s.budgetMs, s.maxShare, s.maxExtra, 3000, s.leak);
        c.escalate = false;
        g.Configure(c);
    }
    bool lastDrew = false;      // the steady 30's rhythm: offered when the turn before did not draw
    uint32_t seed = 12345;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / static_cast<double>(1 << 24); };
    int64_t t = 5 * P, tr = 0;  // the first turn at vblank 5; tr = when the last retrace wait returned
    const int64_t end = Ms(s.seconds * 1000) + t;
    std::vector<int64_t> ready;       // drawn turns' ends
    std::vector<unsigned> turnsOf;    // turns between pictures
    unsigned since = 0;
    bool hitchDone = false, stallDone = false;
    uint64_t turnNo = 0;
    int64_t cStart = 0, cT0 = 0;
    SpeedGuard::Census c0;
    while (t < end) {
        const double sec = (t - 5 * P) / 1e9;
        const bool loading = s.loadAtS >= 0 && sec >= s.loadAtS && sec < s.loadAtS + s.loadS;
        const bool allowed = !loading;
        const bool quiet = loading;
        if (slip.Enabled() || s.slip) slip.Begin(static_cast<uint64_t>(t / 1000), allowed);
        g.BeginTurn(t, allowed, quiet);
        int code;
        if (s.quality) {
            if (s.guard) ctrl.SetExtra(g.WantExtra());
            code = ctrl.Turn(static_cast<uint64_t>(t / 1000), allowed, quiet, 0, 0, 0);
            if (ctrl.ExtraTaken()) g.MarkExtra();
        } else {
            LegacySkip::Verdict v = rule.Turn(t, allowed, 0);
            code = v.code;
            if (s.steady && s.quality == false) {
                if (allowed && lastDrew && code == 0) code = 1;   // not offered: the turn before drew
            }
            if (s.guard) {
                const int before = code;
                code = g.Filter(code, allowed);
                if (before == 0 && code != 0) { rule.ExtraTaken(); }
            }
        }
        g.EndDecision(code);
        r.maxBucketMs = std::max(r.maxBucketMs, static_cast<int>(g.BucketNs() / 1000000));
        lastDrew = code == 0;
        r.hash = (r.hash ^ static_cast<uint64_t>(code + 1)) * 1099511628211ull;
        double cost = code == 0 ? s.D : s.N;
        if (loading) cost = 30.0;
        else if (s.jitter > 0) cost *= 1.0 + s.jitter * (2 * rnd() - 1);
        if (!hitchDone && s.hitchAtS >= 0 && sec >= s.hitchAtS) { cost = s.hitchMs; hitchDone = true; }
        if (!stallDone && s.stallAtS >= 0 && sec >= s.stallAtS) { cost = s.stallMs; stallDone = true; }
        const int64_t workEnd = t + Ms(cost);
        if (s.slip) slip.End(static_cast<uint64_t>(workEnd / 1000), code == 0);
        ++since;
        ++turnNo;
        if (code == 0) {
            ready.push_back(workEnd);
            turnsOf.push_back(since);
            since = 0;
        }
        int64_t ret = workEnd;
        if (code != 2) {
            const int64_t v1 = (tr / P + 1) * P;   // first vblank after the last wait returned
            ret = std::max(workEnd, v1);
            tr = ret;
        }
        t = ret;
        if (turnNo == 600) { c0 = g.Total(); cStart = t; cT0 = turnNo; }
    }
    // game speed over the run after a 10 s warm-up (turns * 16.683 ms / wall)
    (void)cStart; (void)cT0;
    const SpeedGuard::Census tot = g.Total();
    r.turns = static_cast<double>(turnNo);
    r.wall = (t - 5 * P) / 1e9;
    r.speed = 100.0 * turnNo * P / (t - 5 * P);
    r.censusSpeed = tot.speedPct();
    r.lostMs = tot.lostNs() / 1e6;
    r.censusExtras = static_cast<double>(tot.extras);
    r.extras = static_cast<double>(tot.extras);
    r.fps = ready.size() / r.wall;
    // holds
    PresentationSlots slots;
    std::vector<int64_t> shown;
    int64_t lastShown = -1;
    for (size_t i = 0; i < ready.size(); ++i) {
        int64_t v = ready[i] / P + 1;
        int64_t sh;
        if (s.quality) sh = slots.Next(static_cast<uint32_t>(v), 2), slots.Shown(static_cast<uint32_t>(sh));
        else sh = std::max<int64_t>(v, lastShown + 1);
        lastShown = sh;
        shown.push_back(sh);
    }
    unsigned n = 0, n3 = 0, run3 = 0;
    const size_t skip = ready.size() > 400 ? 200 : 0;   // the first seconds
    unsigned nh = 0, h2 = 0, h3 = 0, h4 = 0;
    for (size_t i = skip; i < turnsOf.size(); ++i) {
        ++n;
        if (turnsOf[i] >= 3) { ++n3; ++run3; r.longestRun3 = std::max(r.longestRun3, run3); } else run3 = 0;
    }
    const unsigned base = s.quality ? 2 : 1;
    for (size_t i = skip + 1; i < shown.size(); ++i) {
        const unsigned h = static_cast<unsigned>(shown[i] - shown[i - 1]);
        ++nh;
        if (h <= base) ++h2; else if (h == base + 1) ++h3; else ++h4;
        r.longestHold = std::max(r.longestHold, h);
    }
    r.share3 = n ? 100.0 * n3 / n : 0;
    r.h2 = nh ? 100.0 * h2 / nh : 0; r.h3 = nh ? 100.0 * h3 / nh : 0; r.h4 = nh ? 100.0 * h4 / nh : 0;
    (void)c0;
    g_allHash = (g_allHash ^ r.hash) * 1099511628211ull;
    return r;
}

static void Row(const char* label, const Res& r) {
    std::printf("  %-34s speed %5.1f %%  %5.1f pic/s  3-turn pictures %5.1f %% (longest run %2u)  holds <=base/+1/+2 %5.1f/%5.1f/%5.1f %%  longest hold %u  extras %5.0f  census %5.1f %% lost %6.0f ms\n",
                label, r.speed, r.fps, r.share3, r.longestRun3, r.h2, r.h3, r.h4, r.longestHold, r.extras, r.censusSpeed, r.lostMs);
}

int main() {
    std::printf("== Quality (fixed 30 pair lock, Slip 20 %% on, N = 12 ms undrawn turn)\n");
    const double pairs[] = {32, 34, 36, 40, 41.3, 45, 50};
    for (double pair : pairs) {
        Scn s; s.D = pair - 12; s.N = 12;
        char lab[96];
        std::snprintf(lab, sizeof lab, "pair %.1f guard off", pair); const Res off = Run(s); Row(lab, off);
        s.guard = true;
        std::snprintf(lab, sizeof lab, "pair %.1f guard on (share 100)", pair); const Res on = Run(s); Row(lab, on);
        s.maxShare = 25;
        std::snprintf(lab, sizeof lab, "pair %.1f guard on, share 25", pair); const Res on25 = Run(s); Row(lab, on25);
        s.maxShare = 100; s.maxExtra = 2;
        std::snprintf(lab, sizeof lab, "pair %.1f guard on, 2 extra", pair); const Res on2 = Run(s); Row(lab, on2);
        if (pair <= 33.0) {
            Check(on.extras == 0, "a pair that fits takes no extra turn");
            Check(on.hash == off.hash, "a pair that fits: decisions identical with the guard on");
        }
        if (pair >= 36.0 && pair <= 40.5) {
            Check(on.speed > off.speed + 5.0, "guard on improves game speed");
            Check(on.speed >= 93.0, "guard on: speed >= 93 % with one extra turn");
        }
        if (pair >= 41.0) Check(on.speed >= off.speed - 0.1, "guard on does not lose speed");
        if (pair >= 40.0 && pair < 42.0) {
            Check(off.speed < 88.0, "model: guard off falls well under 100 % (console Honeyhive 83.7 %)");
        }
        if (pair >= 36.0 && pair <= 41.5) {
            Check(on25.share3 <= 26.0, "share 25: three-turn pictures <= 26 %");
            Check(on25.longestRun3 <= 1, "share 25: no two three-turn pictures in a row");
            Check(on25.speed >= off.speed - 0.5, "share 25 is not slower than off");
        }
    }
    std::printf("== Quality, guard on with GXV_SPEED_GUARD_LEAK=5 (aims at 95 %% speed)\n");
    for (double pair : {34.0, 36.0, 40.0}) {
        Scn s; s.D = pair - 12; s.N = 12; s.guard = true; s.leak = 5;
        char lab[96];
        std::snprintf(lab, sizeof lab, "pair %.1f guard on, leak 5 %%", pair); const Res r = Run(s); Row(lab, r);
        if (pair <= 36.0) Check(r.speed >= 94.5, "leak 5: speed >= 94.5 % where one extra turn can repay it");
    }
    std::printf("== Quality with GXV_SLIP=0 (what the rules do on their own: the late catch-up pays the debt, the Slip does not absorb it)\n");
    for (double pair : {36.0, 40.0, 41.3}) {
        Scn s; s.D = pair - 12; s.N = 12; s.slip = false;
        char lab[96];
        std::snprintf(lab, sizeof lab, "pair %.1f, no slip, guard off", pair); const Res off = Run(s); Row(lab, off);
        s.guard = true;
        std::snprintf(lab, sizeof lab, "pair %.1f, no slip, guard on", pair); Row(lab, Run(s));
        Check(off.speed > 99.0, "model: without the slip the rules' own catch-up holds the game speed (at a picture rate of 16-23 a second)");
    }
    std::printf("== Quality single events (pair 33.0 fits)\n");
    {
        Scn s; s.D = 21; s.N = 12; s.guard = true; s.hitchAtS = 20; s.hitchMs = 70;
        Res r = Run(s); Row("a single 70 ms hitch, guard on", r);
        Check(r.extras <= 3, "a 70 ms hitch takes at most 3 extra turns (the rule's own catch-up pays most of it)");
        Check(r.longestRun3 <= 3, "hitch: the extra turns are spread over few pictures");
        s.stallAtS = 20; s.stallMs = 600; s.hitchAtS = -1;
        r = Run(s); Row("a 600 ms stall, guard on", r);
        Check(r.extras == 0, "a stall over 80 ms is not counted: no extra turn");
        s.stallAtS = -1; s.loadAtS = 20; s.loadS = 2;
        r = Run(s); Row("a 2 s load (not allowed, covered)", r);
        Check(r.extras == 0, "loading / covered turns are not counted: no extra turn");
        Scn o = s; o.guard = false;
        Check(Run(o).hash == r.hash, "load / stall scenes: guard on decides exactly as off");
    }
    std::printf("== Quality with +-10 %% cost noise (jitter)\n");
    for (double pair : {34.0, 36.0, 40.0}) {
        Scn s; s.D = pair - 12; s.N = 12; s.jitter = 0.10;
        char lab[96];
        std::snprintf(lab, sizeof lab, "pair %.0f noise, guard off", pair); Row(lab, Run(s));
        s.guard = true;
        std::snprintf(lab, sizeof lab, "pair %.0f noise, guard on", pair); Row(lab, Run(s));
    }
    std::printf("== Performance (LegacySkip, GXV_SKIP_MAXSTEP=2, Slip 20 %%, steady 30 offers); picture = D + N where N = 12 ms\n");
    for (double pair : pairs) {
        Scn s; s.quality = false; s.D = pair - 12; s.N = 12;
        char lab[96];
        std::snprintf(lab, sizeof lab, "perf pair %.1f guard off", pair); const Res off = Run(s); Row(lab, off);
        s.guard = true;
        std::snprintf(lab, sizeof lab, "perf pair %.1f guard on", pair); const Res on = Run(s); Row(lab, on);
        s.maxShare = 25;
        std::snprintf(lab, sizeof lab, "perf pair %.1f guard on, share 25", pair); Row(lab, Run(s));
        if (pair <= 33.0) Check(on.extras == 0 && on.hash == off.hash, "performance: a pair that fits takes no extra turn");
        if (pair >= 36.0) Check(on.speed > off.speed + 3.0, "performance: guard on improves game speed");
    }
    {
        Scn s; s.quality = false; s.D = 21; s.N = 12; s.guard = true; s.hitchAtS = 20; s.hitchMs = 70;
        Res r = Run(s); Row("perf: a single 70 ms hitch", r);
        Check(r.extras <= 3, "performance: a 70 ms hitch takes at most 3 extra turns");
        s.hitchAtS = -1; s.stallAtS = 20; s.stallMs = 600;
        r = Run(s); Row("perf: a 600 ms stall", r);
        Check(r.extras == 0, "performance: a stall is not counted");
        s.stallAtS = -1; s.loadAtS = 20; s.loadS = 2;
        r = Run(s); Row("perf: a 2 s load", r);
        Check(r.extras == 0, "performance: loading is not counted");
    }
    std::printf("== Performance, 60 fps scenes (turns 14 ms drawn): the guard never acts\n");
    {
        Scn s; s.quality = false; s.D = 14; s.N = 8; s.steady = false; s.slip = false; s.guard = true;
        Res on = Run(s); Row("perf 14 ms turns, guard on", on);
        s.guard = false; Res off = Run(s);
        Check(on.extras == 0 && on.hash == off.hash, "turns that fit a refresh: no extra, same decisions");
    }
    // The guard off is exactly the old rule: with SetExtra never called the Controller and LegacySkip decisions are the same as with the guard configured off.
    {
        Scn s; s.D = 28; s.N = 12;
        Scn s2 = s; s2.guard = false;
        Check(Run(s).hash == Run(s2).hash, "guard off: identical decisions");
    }
    // The bucket arithmetic on its own.
    {
        SpeedGuard g;
        { SpeedGuard::Config c = MakeSpeedGuardConfig(true, 30, 100, 1, 3000); c.escalate = false; g.Configure(c); }
        int64_t t = 1000 * P;
        g.BeginTurn(t, false, true);   // (a load: the grace starts)
        t += Ms(3100);
        g.BeginTurn(t, true, false);
        for (int i = 0; i < 10; ++i) { t += P; g.BeginTurn(t, true, false); }   // on time: nothing lost
        Check(g.BucketNs() == 0 && !g.WantExtra(), "on-time turns lose nothing");
        g.EndDecision(1);   // (an undrawn turn: the next one is the picture's)
        t += Ms(70); g.BeginTurn(t, true, false);   // a 70 ms turn: clamped to two refreshes of lost time
        Check(g.BucketNs() > Ms(30) && g.BucketNs() < Ms(34), "a 70 ms turn adds at most two refreshes (minus the leak)");
        Check(g.WantExtra(), "just over the budget: one extra turn wanted");
        g.MarkExtra();
        g.EndDecision(2);
        Check(!g.WantExtra(), "one extra turn per picture");
        g.EndDecision(0);
        g.EndDecision(1);
        Check(g.WantExtra(), "the next picture may take one");
        t += Ms(500); g.BeginTurn(t, true, false);   // a stall: not counted
        Check(g.Window().exclNs >= Ms(500), "a stall is excluded");
        g.BeginTurn(t += P, true, false);
        g.BeginTurn(t += Ms(40), false, true);       // a loading turn: the bucket empties and the grace starts
        Check(g.BucketNs() == 0, "leaving play empties the bucket");
        g.BeginTurn(t += Ms(40), true, false);
        Check(!g.CountingNow(), "the first seconds after a load do not count");
        g.BeginTurn(t += Ms(3100), true, false);
        g.BeginTurn(t += P, true, false);
        Check(g.CountingNow(), "counting again after the grace");
    }
    std::printf("DECISION-HASH %016llx\n", static_cast<unsigned long long>(g_allHash));
    if (sFail) { std::fprintf(stderr, "%d FAILED\n", sFail); return 1; }
    std::puts("speed_guard_test: ALL PASSED");
    return 0;
}
