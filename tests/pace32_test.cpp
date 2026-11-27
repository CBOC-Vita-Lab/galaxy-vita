// pace32 host model (13 Nov 2026). Replays the console's measured pair times through the REAL pacing classes (tests/speed31_model.h: Controller Fixed30 / LegacySkip, SpeedGuard with the new slip band),
// the real late-turn planner (port/late_turn.h), the real governor (port/fegov.h, with and without GXV_HEADROOM_PCT's target) and the real rate controller's sticky rule, and prints, per option:
// picture spacing, game speed and an ESTIMATE of the pad-to-screen delay. Only the console measures milliseconds: every lane figure that is not marked [M] is a guess, the delay column is arithmetic.
//   g++ -std=c++17 -O1 -Iplatform/include tests/pace32_test.cpp -o /tmp/pace32/t && nice -n 19 /tmp/pace32/t
#include "speed31_model.h"
#include "port/fegov.h"
#include "port/late_turn.h"
#include "port/rate_auto.h"
using namespace m31;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// ---------------------------------------------------------------------------------------------------------------- A. spacing and speed
struct Sp { double fps, speed, worst1s, h2, h3, h4, slipS, slipLostMs; unsigned longest, drops, changes; uint64_t hash; double dropsFrom, fpsFrom; };
// from: only the holds shown at or after that second are counted
static Sp Spacing(const Res& r, double from, double to) {
    Sp s{};
    unsigned n = 0, a = 0, b = 0, c = 0, prev = 0;
    for (const auto& h : r.holds) {
        if (h.first < from || h.first >= to) continue;
        ++n;
        if (h.second <= 2) ++a; else if (h.second == 3) ++b; else ++c;
        s.longest = std::max(s.longest, h.second);
        if (prev && h.second != prev) ++s.changes;
        prev = h.second;
    }
    s.h2 = n ? 100.0 * a / n : 0; s.h3 = n ? 100.0 * b / n : 0; s.h4 = n ? 100.0 * c / n : 0;
    s.fps = n / (to - from);
    s.speed = r.speed; s.worst1s = r.worst1s; s.slipS = r.slipS; s.slipLostMs = r.slipLostMs; s.drops = r.drops; s.hash = r.hash;
    return s;
}
static Res RunPair(bool quality, std::function<double(double)> pair, bool slipBand, unsigned pm = 45, double secs = 60, bool guard = true, double jitter = 0) {
    Scn s; s.quality = quality; s.steady = true; s.guard = guard; s.esc = true; s.budgetMs = 20; s.leakPm = 5; s.seconds = secs; s.pair = pair; s.jitter = jitter;
    s.slipBand = slipBand; s.slipBandPm = pm;
    return Run(s);
}
static void SpRow(const char* label, const Res& r, double from, double to) {
    const Sp s = Spacing(r, from, to);
    std::printf("  %-58s %5.1f pic/s  held 2/3/4+ refreshes %5.1f/%5.1f/%5.1f %%  longest %u  rhythm changes %3u  game speed %5.1f %% (worst 1 s %5.1f)  slipped %4.1f s (%3.0f ms)\n", label, s.fps, s.h2, s.h3,
                s.h4, s.longest, s.changes, s.speed, s.worst1s, s.slipS, s.slipLostMs);
}

// ---------------------------------------------------------------------------------------------------------------- B. the delay estimate
// The even 30's pipeline, one pair (all ms; V = 16.683). A (drawn) starts at a retrace: flushes + draw walk (gd), then pad + update (ua). B (undrawn): pad + update (ub), then its retrace wait.
// The picture A' (the next pair's) shows B's pad read. A' start -> flip: the stream is through the front end at max(gd, fe), then the render thread (rd), the GPU (gpu), the first retrace after
// that, plus q retraces of queueing (today's display queue / present latency rule; LATENCY24 + FIF take them out). A retrace wait returns at once when a retrace came since the last wait.
struct Lanes { const char* name; double gd, ua, ub, fe, rd, gpu; };
struct Delay { double padToTurn, turnToFlip, total, idleB, late; bool fits; };
static const double V = 16.68335;
static Delay Delay30(const Lanes& L, bool lateTurn, bool padFresh, int q, double marginMs = 2.5, double maxMs = 12.0) {
    Delay d{};
    const double a = L.gd + L.ua, pair = a + L.ub;
    d.fits = pair <= 2 * V;
    const double bStart = std::max(a, V);                 // A's wait returns at once when it ran past its retrace
    d.idleB = std::max(0.0, 2 * V - (bStart + L.ub));
    d.late = lateTurn ? std::min(maxMs, std::max(0.0, d.idleB - marginMs)) : 0.0;
    const double padAge = padFresh ? 0.3 : 3.0;           // the poll thread samples every ~6 ms: 3 ms old on average; resampled on the game thread: ~0.3
    const double pairLen = std::max(2 * V, bStart + L.ub);   // (over budget: the turns run back to back; the spacing model above says what the pictures do then)
    d.padToTurn = pairLen - bStart - d.late + padAge;
    const double done = std::max(L.gd, L.fe) + L.rd + L.gpu;
    d.turnToFlip = (std::ceil(done / V) + q) * V;
    d.total = d.padToTurn + d.turnToFlip;
    return d;
}
// 60 (a picture every turn): the turn draws what the turn before made; its update reads the pad after the draw (gd into the turn).
static Delay Delay60(const Lanes& L, bool padFresh, int q) {
    Delay d{};
    const double turn = std::max(V, L.gd + L.ua);
    d.fits = L.gd + L.ua <= V && L.fe <= V && L.gpu <= V;
    d.padToTurn = turn - L.gd + (padFresh ? 0.3 : 3.0);
    const double done = std::max(L.gd, L.fe) + L.rd + L.gpu;
    d.turnToFlip = (std::ceil(done / V) + q) * V;
    d.total = d.padToTurn + d.turnToFlip;
    return d;
}

// ---------------------------------------------------------------------------------------------------------------- C. the late-turn planner
struct LtRes { double meanDelay, meanIdleLeft; unsigned lates, backoffs, tight; double firstDelayAt; };
// trueIdle(turn) = the idle (us) the turn would have without a delay. A turn whose delay eats more than its idle ends late by the difference; `penalty` = a late turn costs a whole refresh (the
// pessimistic reading of the retrace wait) and is reported as late.
static LtRes LtRun(std::function<double(unsigned)> trueIdle, unsigned turns, bool penalty, uint32_t margin = 2500) {
    port::lateturn::Config c; c.on = true; c.marginUs = margin;
    port::lateturn::Planner p; p.Configure(c);
    LtRes r{}; r.firstDelayAt = -1;
    double sum = 0, left = 0; unsigned n = 0, nl = 0;
    for (unsigned i = 0; i < turns; ++i) {
        const uint32_t d = p.Plan();
        const double idle0 = trueIdle(i);
        const double idle = idle0 - d;
        const bool over = idle < 0;
        if (d && r.firstDelayAt < 0) r.firstDelayAt = i / 30.0;
        p.Result(over ? 0u : static_cast<uint32_t>(idle), d, over && penalty);
        if (i >= turns / 3) { sum += d; ++n; if (d && !over) { left += idle; ++nl; } }
    }
    r.meanDelay = n ? sum / n / 1000.0 : 0; r.meanIdleLeft = nl ? left / nl / 1000.0 : 0;
    r.lates = p.GetStats().lates; r.backoffs = p.GetStats().backoffs; r.tight = p.GetStats().tight;
    return r;
}

// ---------------------------------------------------------------------------------------------------------------- D. headroom: the governor against a lower target
struct Hr { double meanLevel, meanMs, over16, changesPerMin; };
static Hr HrRun(float budget, float raw, float gainTrue, float swing, double secs = 900) {
    port::fegov::Config c; c.maxLevel = 3; c.recover = true; c.budgetMs = budget;
    port::fegov::Governor g(c);
    Hr r{}; uint32_t rnd = 7; double lv = 0, ms = 0; int n = 0, over = 0, chg = 0;
    for (int w = 0; w * 5.0 < secs; ++w) {
        float mean = raw - gainTrue * g.level() + swing * std::sin(w * 5.0 * 6.2831853 / 40.0);   // (a 40 s swing of the load)
        rnd = rnd * 1664525u + 1013904223u;
        mean += (static_cast<float>(rnd >> 8) / 16777216.0f - 0.5f) * 0.6f;
        port::fegov::Window win; win.pictures = 300; win.frontPics = 300; win.frontSumUs = static_cast<uint64_t>(mean * 1000.0f * 300);
        if (w >= 12) { lv += g.level(); ms += mean; ++n; if (mean > 16.0f) ++over; }
        if (g.Feed(win) && w >= 12) ++chg;
    }
    r.meanLevel = lv / n; r.meanMs = ms / n; r.over16 = 100.0 * over / n; r.changesPerMin = chg / ((secs - 60) / 60.0);
    return r;
}

// ---------------------------------------------------------------------------------------------------------------- E. the sticky rate class
struct RateRes { unsigned ups, downs; double firstUpS; };
// Even 30 from the start (a trigger of misses), then lanes that fit 60 from `fitAt`; a camera cut every `cutEvery` s (0 = never). Returns the switches.
static RateRes RateRun(bool sticky, double fitAt, double cutEvery, double flapPeriod, double secs) {
    port::rate::Params p; p.sticky = sticky;
    port::rate::Controller c; c.Configure(p);
    c.StageBegin(0, 0);
    RateRes r{}; r.firstUpS = -1;
    port::rate::Gov g; g.atMax = true;
    bool even30 = false;
    uint64_t now = 0;
    const uint64_t turn = 16683;
    for (uint64_t i = 0; i * turn < static_cast<uint64_t>(secs * 1e6); ++i) {
        now += turn;
        const double t = now / 1e6;
        bool heavy = t < fitAt;
        if (flapPeriod > 0 && t >= fitAt) heavy = (static_cast<int>((t - fitAt) / (flapPeriod * 0.5)) & 1) != 0;   // light first, then heavy, ...
        const float lane = heavy ? 21.0f : 11.0f;
        port::rate::Obs o;
        o.play = true; o.drawn = even30 ? (i & 1) == 0 : true; o.missed = !even30 && heavy && (i % 3) != 0; o.gameMs = lane; o.gapMs = 16.7f;
        o.atBreak = cutEvery > 0 && std::fmod(t, cutEvery) < 0.05;
        if (o.drawn) c.Picture(now, lane, lane * 0.4f, lane, true);
        const port::rate::Action a = c.Step(now, o, g);
        if (a == port::rate::kTo30) { even30 = true; ++r.downs; }
        if (a == port::rate::kTo60) { even30 = false; ++r.ups; if (r.firstUpS < 0) r.firstUpS = t - fitAt; }
    }
    return r;
}

int main() {
    std::printf("pace32 host model. [M] = a console figure (c90, 12 Nov); everything else is a model: only the console measures milliseconds.\n\n");

    // ---- A
    std::printf("== A. PICTURE SPACING AND GAME SPEED: the measured pair replayed through the real pacer + speed guard (leak 0.5 %%, budget 20 ms), 60 s, the first 3 s are the guard's grace\n");
    for (int q = 1; q >= 0; --q) {
        std::printf(q ? " Quality / the fixed 30 (Controller Fixed30):\n" : " Performance, the stage held at an even 30 (LegacySkip + the steady 30's offers):\n");
        auto bb = [](double) { return 34.5; };
        const Res off = RunPair(q, bb, false, 45, 150, false), c0 = RunPair(q, bb, false, 45, 150), s45 = RunPair(q, bb, true, 45, 150);
        SpRow("Beach Bowl 34.5 [M], guard off (c90's slow motion)", off, 6, 150);
        SpRow("Beach Bowl 34.5 [M], drops only: seconds 5-12", c0, 5, 12);
        SpRow("Beach Bowl 34.5 [M], drops only: settled (s 120-150)", c0, 120, 150);
        SpRow("Beach Bowl 34.5 [M], slip band: seconds 5-12 (slipping)", s45, 5, 12);
        SpRow("Beach Bowl 34.5 [M], slip band: settled (s 120-150)", s45, 120, 150);
        CHECK(Spacing(s45, 5, 12).h3 < 15.0 && Spacing(s45, 5, 12).h4 == 0);       // while slipping: two refreshes, a three-refresh hold once in seven to eleven pictures (the display grid), never more
        CHECK(Spacing(s45, 5, 12).h3 < 0.5 * Spacing(c0, 5, 12).h3);               // ... less than half the long holds of dropped pictures
        CHECK(s45.slipS > 9.0 && s45.slipS <= 10.1);                               // bounded: the allowance, once (the guard's settled rhythm has no pressure left to slip)
        CHECK(Spacing(c0, 120, 150).h2 == 0 && Spacing(s45, 120, 150).h2 == 0);    // both settle at the guard's locked 20 a second when nothing cuts the load
        // the case the band is for: the cuts engage and take the pair under the budget (2.5 ms off after 6 s of overload)
        auto cuts = [](double t) { return t < 4 ? 30.0 : t < 10 ? 34.5 : 32.0; };
        const Res k0 = RunPair(q, cuts, false, 45, 40), k1 = RunPair(q, cuts, true, 45, 40);
        SpRow("34.5 for 6 s, then the cuts take 2.5 ms: drops only", k0, 3, 40);
        SpRow("34.5 for 6 s, then the cuts take 2.5 ms: slip band", k1, 3, 40);
        std::printf("    -> pictures dropped in that transient: %u (drops only) against %u (slip band); game time given up by the slip: %.0f ms, once\n", k0.drops, k1.drops, k1.slipLostMs);
        CHECK(k1.drops == 0 && k0.drops > 0);
        CHECK(k1.slipLostMs < 400);
        for (double jit : {0.02, 0.05}) {
            const Res j0 = RunPair(q, bb, false, 45, 60, true, jit), j1 = RunPair(q, bb, true, 45, 60, true, jit);
            char nm[96];
            std::snprintf(nm, sizeof nm, "Beach Bowl 34.5 +-%.0f %% jitter, drops only: s 5-12", jit * 100); SpRow(nm, j0, 5, 12);
            std::snprintf(nm, sizeof nm, "Beach Bowl 34.5 +-%.0f %% jitter, slip band: s 5-12", jit * 100); SpRow(nm, j1, 5, 12);
        }
        const Res h0 = RunPair(q, [](double) { return 41.3; }, false), h1 = RunPair(q, [](double) { return 41.3; }, true);
        SpRow("Honeyhive 41.3 [M], drops only", h0, 6, 60);
        SpRow("Honeyhive 41.3 [M], slip band on (out of the band)", h1, 6, 60);
        CHECK(h1.hash == h0.hash && h1.slipS == 0);                                // 24 % over is not slipped: identical decisions
        SpRow("Honeyhive 41.3 [M], guard off (c90: 83.7 % measured)", RunPair(q, [](double) { return 41.3; }, false, 45, 60, false), 6, 60);
        const Res hc = RunPair(q, [](double) { return 30.0; }, true), base30 = RunPair(q, [](double) { return 30.0; }, false);
        SpRow("any stage whose pair is at the 30 ms target, band on", hc, 6, 60);
        CHECK(hc.drops == 0 && hc.slipS == 0 && hc.hash == base30.hash);           // under budget the band changes nothing
    }
    std::printf(" Good Egg, Performance (53 pictures a second [M] = a drawn turn of about 18.9 ms):\n");
    {
        // free 60: every turn offered; the model's N is the undrawn turn (10 ms guess), pair = drawn + undrawn
        Scn s; s.quality = false; s.steady = false; s.guard = true; s.esc = true; s.budgetMs = 20; s.leakPm = 5; s.seconds = 60; s.N = 10; s.pair = [](double) { return 28.9; };
        const Res free60 = Run(s);
        unsigned n = 0, one = 0, two = 0, more = 0;
        for (const auto& h : free60.holds) if (h.first >= 6) { ++n; if (h.second <= 1) ++one; else if (h.second == 2) ++two; else ++more; }
        std::printf("  %-58s %5.1f pic/s  held 1/2/3+ refreshes %5.1f/%5.1f/%5.1f %%  game speed %5.1f %%   <- uneven: the model's own rhythm, the console showed 53\n", "as today (60 target, turn 18.9 ms)", n / 54.0,
                    100.0 * one / n, 100.0 * two / n, 100.0 * more / n, free60.speed);
        s.steady = true;
        SpRow("the same stage held at an even 30 (rate_pin=2)", Run(s), 6, 60);
        s.steady = false; s.pair = [](double) { return 25.0; };
        const Res fit = Run(s);
        n = one = two = more = 0;
        for (const auto& h : fit.holds) if (h.first >= 6) { ++n; if (h.second <= 1) ++one; else if (h.second == 2) ++two; else ++more; }
        std::printf("  %-58s %5.1f pic/s  held 1/2/3+ refreshes %5.1f/%5.1f/%5.1f %%  game speed %5.1f %%\n", "with the far cuts to the 15 ms target (turn 15.0, estimate)", n / 54.0, 100.0 * one / n, 100.0 * two / n,
                    100.0 * more / n, fit.speed);
    }

    // ---- B
    std::printf("\n== B. PAD-TO-SCREEN DELAY, an estimate (ms). Lanes: gd = flushes + draw walk, ua / ub = pad + update of the drawn / undrawn turn, fe front end, rd render thread, gpu.\n");
    std::printf("   q = retraces a finished picture waits in queues. Today q is about 2 [inferred: c90 measured 85-104 ms at Performance, the lanes alone need 3 retraces]; GXV_LATENCY24=1 + GXV_FIF=2 aim at 0.\n");
    const Lanes lanes[] = {
        // name                                                   gd     ua     ub     fe    rd   gpu
        {"Honeyhive today [M: drawn 30.45, undrawn 12.45]",       16.5, 13.95, 12.45, 19.0, 5.0, 20.0},
        {"Honeyhive, pair cut to 30 ms (estimate)",               11.0,  9.5,   9.5,  17.0, 5.0, 18.0},
        {"Beach Bowl today [M: pair 34.5, GPU 24.9]",             10.5, 12.0,  12.0,  19.0, 5.0, 24.9},
        {"Beach Bowl, pair cut to 30 ms (estimate)",               9.0, 10.5,  10.5,  17.5, 5.0, 23.0},
        {"Dreadnought [M: GPU ~22; game est. 17 drawn]",           8.0,  9.0,   8.0,  14.6, 5.0, 22.0},
    };
    std::printf("  %-50s %-34s  pad->turn  turn->flip   total   (undrawn turn's idle, started late by)\n", "stage, an even 30", "option");
    for (const Lanes& L : lanes) {
        struct Opt { const char* name; bool late, pad; int q; } opts[] = {
            {"control (today's queues)", false, false, 2},
            {"+ GXV_LATENCY24=1, PAD=1, FIF=2", false, true, 0},
            {"+ GXV_LATE_TURN=1", true, true, 0},
        };
        for (const Opt& o : opts) {
            const Delay d = Delay30(L, o.late, o.pad, o.q);
            std::printf("  %-50s %-34s  %7.1f   %8.1f   %6.1f   (%.1f, %.1f)%s\n", L.name, o.name, d.padToTurn, d.turnToFlip, d.total, d.idleB, d.late, d.fits ? "" : "   PAIR OVER 33.4: no even 30, see A");
        }
    }
    {
        const Lanes egg{"Good Egg at 60 (estimate: game 10, fe 11.2, GPU 12.5)", 4.5, 5.5, 5.5, 11.2, 4.0, 12.5};
        const Delay a = Delay60(egg, false, 2), b = Delay60(egg, true, 0);
        std::printf("  %-50s %-34s  %7.1f   %8.1f   %6.1f\n", egg.name, "control (today's queues)", a.padToTurn, a.turnToFlip, a.total);
        std::printf("  %-50s %-34s  %7.1f   %8.1f   %6.1f\n", egg.name, "+ GXV_LATENCY24=1, PAD=1, FIF=2", b.padToTurn, b.turnToFlip, b.total);
        const Lanes d60{"Dreadnought at 60 as today (GPU 22: uneven 45 [M])", 8.0, 9.0, 8.0, 14.6, 5.0, 22.0};
        const Delay c = Delay60(d60, false, 2);
        std::printf("  %-50s %-34s  %7.1f   %8.1f   %6.1f   (uneven holds)\n", d60.name, "control (today's queues)", c.padToTurn, c.turnToFlip, c.total);
    }
    {   // sanity of the arithmetic
        const Lanes L = lanes[4];
        const Delay c = Delay30(L, false, false, 2), l = Delay30(L, true, true, 0);
        CHECK(l.total < c.total - 30);
        CHECK(Delay30(lanes[3], true, true, 0).late >= 0.0);
    }

    // ---- C
    std::printf("\n== C. THE LATE-TURN PLANNER (port/late_turn.h), 60 s of undrawn turns; idle = what the turn has left before its retrace without a delay\n");
    {
        uint32_t rnd = 5;
        auto noise = [&rnd](double amp) { rnd = rnd * 1664525u + 1013904223u; return ((rnd >> 8) / 16777216.0 - 0.5) * 2 * amp; };
        struct Case { const char* name; std::function<double(unsigned)> idle; bool penalty; } cases[] = {
            {"steady idle 8.7 ms (Dreadnought-like), +-0.5 ms", [&](unsigned) { return 8700 + noise(500); }, true},
            {"steady idle 3.4 ms (a pair at the 30 ms target), +-0.5", [&](unsigned) { return 3400 + noise(500); }, true},
            {"idle 0.9 ms (a pair of 32.5 ms)", [&](unsigned) { return 900 + noise(300); }, true},
            {"idle 8.7 ms with a 6 ms heavier turn every 2 s", [&](unsigned i) { return (i % 60 == 59 ? 2700 : 8700) + noise(500); }, true},
            {"idle 8.7 ms dropping to 2 ms at 30 s (a heavier area)", [&](unsigned i) { return (i < 900 ? 8700 : 2000) + noise(400); }, true},
            {"the same, an overrun only makes the next turn late", [&](unsigned i) { return (i < 900 ? 8700 : 2000) + noise(400); }, false},
            {"idle +-4 ms around 6 ms (noisy)", [&](unsigned) { return 6000 + noise(4000); }, true},
        };
        for (const Case& c : cases) {
            const LtRes r = LtRun(c.idle, 1800, c.penalty);
            std::printf("  %-56s delay %4.1f ms mean (first after %.1f s), idle left %4.1f ms, turns that missed their retrace %u, back-offs %u, no idle left %u\n", c.name, r.meanDelay, r.firstDelayAt,
                        r.meanIdleLeft, r.lates, r.backoffs, r.tight);
        }
        const LtRes a = LtRun([&](unsigned) { return 8700 + noise(500); }, 1800, true);
        CHECK(a.meanDelay > 5.0 && a.meanDelay < 6.5 && a.lates == 0);
        const LtRes b = LtRun([&](unsigned) { return 900 + noise(300); }, 1800, true);
        CHECK(b.meanDelay == 0 && b.lates == 0);
        const LtRes h = LtRun([&](unsigned i) { return (i % 60 == 59 ? 2700 : 8700) + noise(500); }, 1800, true);
        CHECK(h.lates <= 1);                                                    // one hitch teaches it; the smallest slack of the window keeps the next ones safe
        const LtRes d = LtRun([&](unsigned i) { return (i < 900 ? 8700 : 2000) + noise(400); }, 1800, true);
        CHECK(d.lates <= 2 && d.backoffs <= 2);
        port::lateturn::Planner off; port::lateturn::Config oc; off.Configure(oc);
        CHECK(off.Plan() == 0);                                                 // off: nothing
    }

    // ---- D
    std::printf("\n== D. HEADROOM: the real governor (port/fegov.h, recover30 rules) against a lane with a 40 s swing of +-0.5 ms, true gain 0.8 ms a level, 15 min\n");
    std::printf("  %-44s  mean level  lane mean   windows over 16.0 ms   changes/min\n", "raw lane (no cuts) / target");
    for (float raw : {14.6f, 15.2f, 15.8f, 16.4f, 17.0f}) {
        for (float budget : {15.5f, 15.0f}) {
            const Hr r = HrRun(budget, raw, 0.8f, 0.5f);
            char nm[96]; std::snprintf(nm, sizeof nm, "raw %.1f ms, target %.1f%s", raw, budget, budget < 15.4f ? " (GXV_HEADROOM_PCT=90)" : " (today)");
            std::printf("  %-44s  %6.2f     %6.2f ms      %5.1f %%             %5.2f\n", nm, r.meanLevel, r.meanMs, r.over16, r.changesPerMin);
        }
    }
    {
        const Hr a = HrRun(15.5f, 15.8f, 0.8f, 0.5f), b = HrRun(15.0f, 15.8f, 0.8f, 0.5f);
        CHECK(b.meanMs <= a.meanMs + 0.05);
        CHECK(b.over16 <= a.over16);
        CHECK(b.changesPerMin <= 2.0);
    }

    // ---- E
    std::printf("\n== E. THE RATE CLASS (port/rate_auto.h): heavy for 20 s (drops to the even 30), then the lanes fit 60; switches up / down, seconds from the first fit to 60\n");
    {
        struct Case { const char* name; double cutEvery, flap; } cases[] = {
            {"stays light, a camera cut every 15 s", 15, 0}, {"stays light, no camera cut for 100 s", 0, 0}, {"light / heavy every 8 s, cuts every 15 s", 15, 16}, {"light / heavy every 30 s, cuts every 15 s", 15, 60}};
        for (const Case& c : cases) {
            const RateRes a = RateRun(false, 20, c.cutEvery, c.flap, 120), b = RateRun(true, 20, c.cutEvery, c.flap, 120);
            std::printf("  %-44s  rate30: down %u up %u (first up after %5.1f s)   GXV_RATE_STICKY=1: down %u up %u (first up after %5.1f s)\n", c.name, a.downs, a.ups, a.firstUpS, b.downs, b.ups, b.firstUpS);
        }
        const RateRes s1 = RateRun(true, 20, 15, 0, 120), s2 = RateRun(true, 20, 0, 0, 120), s3 = RateRun(true, 20, 15, 16, 120), n3 = RateRun(false, 20, 15, 16, 120);
        CHECK(s1.ups == 1 && s1.firstUpS >= 20.0);     // only after the proof, at a cut
        CHECK(s2.ups == 0);                            // no break: no switch in the middle of play
        CHECK(s3.ups + s3.downs <= n3.ups + n3.downs); // never more switches than without it
        CHECK(s3.ups == 0);                            // a load that comes back every 8 s never proves 20 s of fit
    }

    std::printf(g_fail ? "\npace32_test: FAILED (%d)\n" : "\npace32_test: ALL PASSED\n", g_fail);
    return g_fail != 0;
}
