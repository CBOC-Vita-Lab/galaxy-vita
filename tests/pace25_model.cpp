// GXV_PACE25 (platform/include/port/pace25.h) on a model of the console pipeline, replaying per-window costs and hold histograms extracted from the
// c69 console logs (tests/pace25_trace.inc: Toy Time, Honeyhive, Ocean Ring, Melty Molten, Good Egg, Tri-Leg; runs K control, E and EU with the optimisation
// switches). A model, not a console measurement: it uses the real port::pace::LegacySkip, PresentSync, TurnClock and Pace25 classes, and a timeline of
//   game thread (drawn turn d ms, other n ms, retrace wait, draw-done wait for the front end)
//   -> front end (fe ms a picture, FrameSink gate: at most 3 pictures in the back end's queue)
//   -> back end / render thread (cpu ms; its first scene waits for the display buffer of the picture two before to leave the screen; AddEntry waits for
//      the display queue's second entry)
//   -> GPU (g ms a picture, in order) -> display callback (GXV_PRESENT_SYNC=2: held to the vblank stamp + L, L adapting 1..5, never two on one vblank).
// Three things are checked: (1) the baseline reproduces what the console logged (1-1-1-3 with the faster E / EU costs, the mixed 1/2 of the K control);
// (2) Pace25 gives holds of 1 and 2 only while the work fits two refreshes, at the same logic rate (60 turns a second); (3) a stage that fits one refresh
// is not touched. Prints the tables the report quotes.
//   tools/test_pace25_model.sh
#include "port/frame_pace.h"
#include "port/pace25.h"

#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

namespace {
constexpr double kV = 16.68335;

struct Row {
    const char *run, *stage;
    int win;
    double d, n, gpu, be, fe;
    int pics, h1, h2, h3, h4, L;
    double sema;
};
const Row kRows[] = {
#include "pace25_trace.inc"
};

struct Params {
    double d = 18.0, n = 9.0, fe = 15.0, be = 11.0, g = 20.0;  // ms: drawn turn, other turn, front end, back end cpu, GPU a picture
    double sigma = 0.07, sigmaG = 0.03;                         // relative spread of the CPU costs and of the GPU's
    int bufs = 3;                                                // display buffers (display queue max pending = bufs - 1)
    unsigned pipeDepth = 2, maxRun = 3, maxPipeRun = 6, maxStep = 3, queueLimit = 3;
    int syncMode = 2;                                            // 2: GXV_PRESENT_SYNC=2; 0: pictures shown when ready
    unsigned latMax = 5;
    double tickQueue = 4;                                        // retrace messages the game's queue keeps
    bool pace25 = false;
    port::pace::Pace25Config pc;
    double gscale = 1.0;                                         // GPU occupancy a picture = g * gscale (fitted per run to the console's pictures a second)
    double seconds = 40, warm = 6;
    unsigned seed = 1;
};

struct Result {
    double fps = 0, turnsPerSec = 0, meanHold = 0, frac3 = 0, frac2 = 0, frac1 = 0, maxHold = 0, holdSd = 0, wait = 0, lat = 0, semaMs = 0;
    double hold[5] = {};
    double step[5] = {};
    double stepsPerPic = 0;
    unsigned level = 0, guard = 0;
    double Lmean = 0;
    double pat[3] = {};  // fraction of triples (hold a, b, c) that are 1-1-3 style: a hold of 3 after two holds of 1
};

Result Simulate(const Params& P) {
    std::mt19937 rng(P.seed * 7919u + 13u);
    auto costS = [&](double m, double sg) {
        std::normal_distribution<double> dist(m, m * sg);
        const double v = dist(rng);
        return v < m * 0.5 ? m * 0.5 : v;
    };
    auto cost = [&](double m) { return costS(m, P.sigma); };
    port::pace::LegacySkip rule(static_cast<int>(P.maxRun), static_cast<int>(P.pipeDepth), static_cast<int>(P.maxPipeRun), nullptr, static_cast<int>(P.maxStep));
    port::pace::Pace25 pace;
    port::pace::Pace25Config pc = P.pc;
    pc.mode = P.pace25 ? 1 : 0;
    if (const char* g = std::getenv("PACE25_GUARD")) pc.guardDepth = static_cast<unsigned>(std::atoi(g));
    pace.Configure(pc);
    port::pace::TurnClock clock;
    port::pace::PresentSync sync;
    sync.Configure(P.syncMode == 2, 1, P.latMax, 3, 4);
    const double pipeHint = std::max({P.fe, P.be, P.g});

    std::vector<double> feDone, beDone, show, turnAt;  // per picture
    std::vector<int> turnIdx;
    std::vector<unsigned> latL;
    std::vector<uint32_t> showV, turnV;
    double nextTick = 1.0;
    double t = 0, feFree = 0, gpuFree = 0, cbReturn = 0, drawDoneWait = -1, semaSum = 0;
    unsigned turns = 0;
    const double end = P.seconds * 1000.0;
    uint32_t lastVc = 0;
    (void)lastVc;
    while (t < end) {
        const uint32_t vcount = static_cast<uint32_t>(t / kV);
        unsigned queued = 0, done = 0;
        for (size_t k = feDone.size() > 12 ? feDone.size() - 12 : 0; k < feDone.size(); ++k) {
            if (feDone[k] <= t) ++queued;
            if (beDone[k] <= t) ++done;
        }
        // (pictures before the window are all done)
        const unsigned pending = queued > done ? queued - done : 0;
        const int64_t nowNs = static_cast<int64_t>(t * 1e6);
        const port::pace::LegacySkip::Verdict v = rule.Turn(nowNs, true, pending);
        int code = v.code;
        if (P.pace25) {
            port::pace::Pace25::Turn pt{static_cast<int64_t>(t * 1000.0), true, false, v.code, v.behind, pending, static_cast<uint32_t>(pipeHint * 1000.0)};
            code = pace.Decide(pt);
        }
        const bool drawn = code == 0;
        const uint32_t vblank = clock.Turn(vcount);
        double w = t + cost(0.6 * P.n);
        const double recStart = w;
        if (drawn && drawDoneWait > w) w = drawDoneWait;
        if (drawn) drawDoneWait = -1;
        w += cost(0.4 * P.n);
        if (drawn) w += cost(P.d - P.n);
        if (P.pace25) pace.WorkDone(static_cast<int64_t>(w * 1000.0), drawn);
        if (drawn) {
            const size_t k = feDone.size();
            const double fe0 = std::max(recStart, feFree);
            double fe1 = std::max(fe0 + cost(P.fe), w);
            if (k >= P.queueLimit) fe1 = std::max(fe1, beDone[k - P.queueLimit]);  // FrameSink: the back end's queue holds at most queueLimit pictures
            feFree = fe1;
            drawDoneWait = fe1;
            const double prevBe = k ? beDone[k - 1] : 0.0;
            const double s = std::max(fe1, prevBe);
            const double bufFree = k >= static_cast<size_t>(P.bufs - 1) ? show[k - (P.bufs - 1)] : 0.0;
            const double tScene = std::max(s + 1.0, bufFree);
            semaSum += tScene - (s + 1.0);
            const double be1 = tScene + std::max(0.0, cost(P.be) - 1.0);
            const double gStart = std::max(tScene + 1.0, gpuFree);
            const double gDone = gStart + costS(P.g * P.gscale, P.sigmaG);
            gpuFree = gDone;
            // display callback
            const double e = std::max(gDone, cbReturn);
            const uint32_t earliest = static_cast<uint32_t>(e / kV) + 1;
            uint32_t target = earliest;
            if (P.syncMode != 0) target = sync.Target(earliest, true, static_cast<uint16_t>(vblank));
            sync.Shown(target, target > earliest);
            latL.push_back(sync.Latency());
            const double shownAt = target * kV;
            cbReturn = shownAt;
            feDone.push_back(fe1);
            beDone.push_back(be1);
            show.push_back(shownAt);
            showV.push_back(target);
            turnV.push_back(vblank);
            turnIdx.push_back(static_cast<int>(turns));
            turnAt.push_back(t);
        }
        ++turns;
        // (MainLoopFramework waitForTick: the retrace messages are queued; a turn takes the next one that has not been taken, so a late turn finds its
        //  retrace already there and does not wait. A turn left out late (code 2) does not take one. The queue holds a few: older ones are dropped.)
        if (code != 2) {
            const double newest = std::floor(w / kV);
            if (nextTick < newest - P.tickQueue + 1) nextTick = newest - P.tickQueue + 1;
            t = std::max(w, nextTick * kV);
            nextTick += 1.0;
        } else {
            t = w;
        }
    }
    Result r;
    // statistics after the warm-up
    std::vector<unsigned> holds, steps;
    for (size_t k = 1; k < showV.size(); ++k) {
        if (turnAt[k] < P.warm * 1000.0) continue;
        const int32_t h = static_cast<int32_t>(showV[k] - showV[k - 1]);
        holds.push_back(h < 1 ? 1u : static_cast<unsigned>(h));
        steps.push_back(static_cast<unsigned>(turnIdx[k] - turnIdx[k - 1]));
        r.lat += static_cast<double>(static_cast<int32_t>(showV[k] - turnV[k])) * kV;
        r.Lmean += latL[k];
    }
    const double n = static_cast<double>(holds.size());
    if (!n) return r;
    const double secs = P.seconds - P.warm;
    double sum = 0, sq = 0;
    for (unsigned h : holds) {
        sum += h;
        sq += static_cast<double>(h) * h;
        r.hold[h < 4 ? h : 4] += 1.0 / n;
        r.maxHold = std::max<double>(r.maxHold, h);
    }
    for (unsigned s : steps) r.step[s < 4 ? s : 4] += 1.0 / n;
    r.meanHold = sum / n;
    r.holdSd = std::sqrt(std::max(0.0, sq / n - r.meanHold * r.meanHold));
    r.frac1 = r.hold[1];
    r.frac2 = r.hold[2];
    r.frac3 = r.hold[3] + r.hold[4];
    r.fps = n / secs;
    r.wait = sq / (2.0 * sum) * kV;  // mean time from a random instant to the next picture change beyond... (the screen's staleness), ms
    r.lat /= n;
    r.Lmean /= n;
    r.stepsPerPic = std::accumulate(steps.begin(), steps.end(), 0.0) / n;
    r.turnsPerSec = turns / (t / 1000.0);
    r.semaMs = feDone.empty() ? 0 : semaSum / feDone.size();
    r.level = pace.Level();
    r.guard = static_cast<unsigned>(pace.GetStats().guardHits);
    unsigned trip = 0;
    for (size_t i = 2; i < holds.size(); ++i) trip += holds[i - 2] == 1 && holds[i - 1] == 1 && holds[i] >= 3;
    r.pat[0] = static_cast<double>(trip) / n;
    return r;
}

Result Mean(const Params& base, unsigned seeds) {
    Result m;
    for (unsigned s = 1; s <= seeds; ++s) {
        Params p = base;
        p.seed = s;
        const Result r = Simulate(p);
        m.fps += r.fps / seeds; m.turnsPerSec += r.turnsPerSec / seeds; m.meanHold += r.meanHold / seeds; m.frac3 += r.frac3 / seeds;
        m.frac2 += r.frac2 / seeds; m.frac1 += r.frac1 / seeds; m.maxHold = std::max(m.maxHold, r.maxHold); m.holdSd += r.holdSd / seeds;
        m.wait += r.wait / seeds; m.lat += r.lat / seeds; m.semaMs += r.semaMs / seeds; m.Lmean += r.Lmean / seeds; m.stepsPerPic += r.stepsPerPic / seeds;
        m.pat[0] += r.pat[0] / seeds;
        m.level = std::max(m.level, r.level);
        m.guard += r.guard;
        for (int i = 0; i < 5; ++i) { m.hold[i] += r.hold[i] / seeds; m.step[i] += r.step[i] / seeds; }
    }
    return m;
}

// The console's own numbers for a run's windows of a stage: pictures a second, hold fractions.
struct Logged { double fps = 0, h1 = 0, h2 = 0, h3 = 0, d = 0, n = 0, g = 0, fe = 0, be = 0, sema = 0; int windows = 0; };
Logged LoggedFor(const char* run, const char* stage) {
    Logged l;
    double pics = 0, h1 = 0, h2 = 0, h3 = 0;
    for (const Row& r : kRows) {
        if (std::strcmp(r.run, run) || std::strcmp(r.stage, stage)) continue;
        ++l.windows;
        pics += r.pics; h1 += r.h1; h2 += r.h2; h3 += r.h3 + r.h4;
        l.d += r.d; l.n += r.n; l.g += r.gpu; l.fe += r.fe; l.be += r.be; l.sema += r.sema;
    }
    if (!l.windows) return l;
    l.fps = pics / (5.0 * l.windows);
    const double tot = h1 + h2 + h3;
    l.h1 = h1 / tot; l.h2 = h2 / tot; l.h3 = h3 / tot;
    l.d /= l.windows; l.n /= l.windows; l.g /= l.windows; l.fe /= l.windows; l.be /= l.windows; l.sema /= l.windows;
    return l;
}
Params MeanParams(const Logged& l) {
    Params p;
    p.d = l.d; p.n = l.n; p.g = l.g; p.fe = l.fe; p.be = std::max(l.be, 1.0);
    return p;
}

void Header(const char* s) { std::printf("\n== %s\n", s); }

}  // namespace

int main(int argc, char** argv) {
    const bool quiet = argc > 1 && !std::strcmp(argv[1], "-q");
    // ---- unit checks of the pure rule --------------------------------------------------------------------------------------------------------
    {
        using namespace port::pace;
        // a ladder of even rhythms: every level's holds are floor / ceil of its mean, spread (no two long holds in a row unless the mean is above 1.5)
        for (unsigned l = 0; l < kPace25Levels; ++l) {
            const Pace25Level lv = Pace25LevelAt(l);
            Pace25 p;
            Pace25Config c;
            c.mode = 1;
            p.Configure(c);
            // force the level through the evaluation path: a window whose cost asks for it
            Pace25::Turn t{0, true, false, 0, false, 0, static_cast<uint32_t>(lv.p * 16683.0 / lv.q / 1.05 + 1.0)};
            for (int w = 0; w < 80 * 30; ++w) {
                t.nowUs += 16683;
                const int code = p.Decide(t);
                p.WorkDone(t.nowUs + (code == 0 ? 14000 : 8000), code == 0);
            }
            Require(p.Level() <= l + 1, "the level the estimate asks for is not overshot by more than one ladder step");
        }
        // the schedule: level 7 (3/2) draws on every other turn, level 4 (5/4) on 4 of 5, never two skipped in a row
        for (unsigned l : {2u, 4u, 6u, 8u, 12u, 16u, 20u}) {
            Pace25 p;
            Pace25Config c;
            c.mode = 1;
            c.guardDepth = 99;
            p.Configure(c);
            Pace25::Turn t{0, true, false, 0, false, 0, static_cast<uint32_t>(Pace25LevelAt(l).p * 16683.0 / Pace25LevelAt(l).q / 1.05 - 300.0)};
            for (int w = 0; w < 80 * 30; ++w) { t.nowUs += 16683; const int code = p.Decide(t); p.WorkDone(t.nowUs + 9000, code == 0); }
            Require(p.Level() == l, "the estimate lands on its own ladder step");
            p.ClearStats();
            unsigned run = 0, maxRun = 0, draws = 0;
            for (int i = 0; i < 6000; ++i) {
                t.nowUs += 16683;
                const int code = p.Decide(t);
                p.WorkDone(t.nowUs + 9000, code == 0);
                if (code == 0) { ++draws; run = 0; } else { ++run; maxRun = std::max(maxRun, run); }
            }
            const Pace25Level lv = Pace25LevelAt(l);
            Require(std::abs(static_cast<double>(draws) / 6000.0 - static_cast<double>(lv.q) / lv.p) < 0.01, "the rhythm draws q of every p turns");
            Require(maxRun + 1 <= p.MaxHold(), "no hold above ceil(p/q) turns");
            const Pace25::Stats& s = p.GetStats();
            Require(s.hold[3] == 0 || p.MaxHold() >= 3, "no hold of 3 at a rhythm under 2 turns");
            Require(s.hold[1] + s.hold[2] + s.hold[3] + s.hold[4] > 0, "holds are counted");
        }
        // strict 60: a cost within the dead band is the legacy rule's, byte for byte
        {
            Pace25 p;
            Pace25Config c;
            c.mode = 1;
            p.Configure(c);
            Pace25::Turn t{0, true, false, 0, false, 0, 16000};
            for (int i = 0; i < 3000; ++i) {
                t.nowUs += 16683;
                t.legacy = i % 11 == 5 ? 1 : (i % 17 == 3 ? 2 : 0);
                t.legacyBehind = t.legacy == 2;
                t.pending = i % 7 == 0 ? 2 : 0;
                Require(p.Decide(t) == t.legacy, "level 0 answers the legacy rule");
                p.WorkDone(t.nowUs + 12000, t.legacy == 0);
            }
            Require(p.Level() == 0, "a picture that fits one refresh stays at level 0");
        }
        // census (mode 2) and off (mode 0) never change an answer
        for (int mode : {0, 2}) {
            Pace25 p;
            Pace25Config c;
            c.mode = mode;
            p.Configure(c);
            Pace25::Turn t{0, true, false, 0, false, 0, 30000};
            unsigned diff = 0;
            for (int i = 0; i < 3000; ++i) {
                t.nowUs += 16683;
                t.legacy = i % 3 == 0 ? 1 : 0;
                diff += p.Decide(t) != t.legacy;
                p.WorkDone(t.nowUs + 12000, t.legacy == 0);
            }
            Require(diff == 0, "census and off answer the legacy rule");
            if (mode == 2) Require(p.Level() > 0 && p.GetStats().turns > 0, "census still decides (what it would do)");
        }
        // a load or scene change draws and resets the rhythm; a pause is not measured
        {
            Pace25 p;
            Pace25Config c;
            c.mode = 1;
            p.Configure(c);
            Pace25::Turn t{0, false, false, 0, false, 0, 30000};
            t.nowUs += 16683;
            Require(p.Decide(t) == 0, "a scene change draws");
        }
    }

    // ---- 1. the baseline against the console's own histograms ---------------------------------------------------------------------------------
    // One free parameter per (stage, run): the GPU's occupancy a picture (the logged "busy" ms is a sum of TA and 3D spans, not the pipeline's real service
    // time) is scaled until the legacy rule's pictures a second equal the console's. Everything else is the logged cost. The hold histogram and the turns a
    // picture advanced are then NOT fitted: they are the check.
    Header("1. the model against the console (legacy rule), per stage and run: GPU scale fitted to the console's pictures a second; holds are the check");
    std::printf("%-9s %-3s | console: fps  hold 1/2/3+       | model: fps  hold 1/2/3+      1-1-3 | gpu scale (fitted)\n", "stage", "run");
    struct Fit { Params p; Logged l; bool ok = false; };
    static Fit fits[3][6];
    const char* stages[][2] = {{"FactoryGalaxy", "ToyTime"}, {"HoneyBeeKingdomGalaxy", "Honeyhive"}, {"OceanRingGalaxy", "OceanRing"}, {"HellProminenceGalaxy", "Melty"},
                               {"EggStarGalaxy", "GoodEgg"}, {"TriLegLv1Galaxy", "TriLeg"}};
    const char* runs[] = {"K", "E", "EU"};
    double errFps = 0, errH3 = 0;
    int nFit = 0;
    for (int si = 0; si < 6; ++si) {
        for (int ri = 0; ri < 3; ++ri) {
            const Logged l = LoggedFor(runs[ri], stages[si][0]);
            if (!l.windows) continue;
            Params p = MeanParams(l);
            p.seconds = 30;
            double lo = 0.5, hi = 1.6;
            for (int it = 0; it < (std::getenv("PACE25_FIT") ? 12 : 0); ++it) {
                p.gscale = 0.5 * (lo + hi);
                const Result r = Mean(p, 3);
                if (r.fps > l.fps) lo = p.gscale; else hi = p.gscale;
            }
            p.gscale = std::getenv("PACE25_FIT") ? 0.5 * (lo + hi) : 1.0;
            p.seconds = 40;
            const Result m = Mean(p, 6);
            fits[ri][si] = Fit{p, l, true};
            std::printf("%-9s %-3s | %6.1f %5.2f/%4.2f/%4.2f     | %5.1f %5.2f/%4.2f/%4.2f  %4.2f | %.2f\n", stages[si][1], runs[ri], l.fps, l.h1, l.h2, l.h3, m.fps, m.frac1, m.frac2, m.frac3, m.pat[0], p.gscale);
            errFps += std::abs(m.fps - l.fps);
            errH3 += std::abs(m.frac3 - l.h3);
            ++nFit;
        }
    }
    std::printf("mean |model - console|: pictures a second %.2f, share of 3+ holds %.3f (n=%d)\n", errFps / nFit, errH3 / nFit, nFit);

    // ---- 2. before / after on every logged stage ---------------------------------------------------------------------------------------------
    Header("2. legacy vs GXV_PACE25=1 on each stage's fitted costs (6 seeds, 34 s each)");
    std::printf("%-9s %-3s | %-6s fps  1/2/3+ holds    maxhold sd   wait(ms) lat(ms) L  turns/s guard/s level\n", "stage", "run", "rule");
    struct Agg { double frac3 = 0, fps = 0, sd = 0, lat = 0; int n = 0; } agg[2];
    double worstLogic = 0;
    for (int si = 0; si < 6; ++si) {
        for (int ri = 0; ri < 3; ++ri) {
            Fit& f = fits[ri][si];
            if (!f.ok) continue;
            Params p = f.p;
            const Result a = Mean(p, 6);
            p.pace25 = true;
            const Result b = Mean(p, 6);
            std::printf("%-9s %-3s | legacy %5.1f %4.2f/%4.2f/%4.2f   %4.0f  %4.2f  %6.1f  %6.1f %4.1f %6.1f    -     -\n", stages[si][1], runs[ri], a.fps, a.frac1, a.frac2, a.frac3, a.maxHold, a.holdSd, a.wait, a.lat, a.Lmean, a.turnsPerSec);
            std::printf("%-9s %-3s | pace25 %5.1f %4.2f/%4.2f/%4.2f   %4.0f  %4.2f  %6.1f  %6.1f %4.1f %6.1f  %5.1f  %2u\n", "", "", b.fps, b.frac1, b.frac2, b.frac3, b.maxHold, b.holdSd, b.wait, b.lat, b.Lmean, b.turnsPerSec,
                        b.guard / 6.0 / (p.seconds - p.warm), b.level);
            if (si < 4) {
                agg[0].frac3 += a.frac3; agg[0].fps += a.fps; agg[0].sd += a.holdSd; agg[0].lat += a.lat; ++agg[0].n;
                agg[1].frac3 += b.frac3; agg[1].fps += b.fps; agg[1].sd += b.holdSd; agg[1].lat += b.lat; ++agg[1].n;
            }
            worstLogic = std::max(worstLogic, std::min(a.turnsPerSec, 60.0) - b.turnsPerSec);
            if (f.l.fps > 52.0) {  // (stages at or near 60: nothing may get worse)
                Require(b.fps >= a.fps - 1.0, "a stage at or near 60 loses no pictures a second");
                Require(b.turnsPerSec >= a.turnsPerSec - 0.5, "and no logic rate");
            }
        }
    }
    std::printf("mean over Toy Time / Honeyhive / Ocean Ring / Melty (K, E, EU): legacy %.1f fps, 3+ holds %.3f, hold sd %.2f, shown-minus-turn %.0f ms; "
                "pace25 %.1f fps, 3+ holds %.3f, hold sd %.2f, shown-minus-turn %.0f ms\n", agg[0].fps / agg[0].n, agg[0].frac3 / agg[0].n, agg[0].sd / agg[0].n, agg[0].lat / agg[0].n,
                agg[1].fps / agg[1].n, agg[1].frac3 / agg[1].n, agg[1].sd / agg[1].n, agg[1].lat / agg[1].n);
    Require(worstLogic < 0.6, "pace25 never lowers the logic rate by more than 0.6 turns a second");
    Require(agg[1].frac3 < 0.25 * agg[0].frac3 + 0.005, "pace25 removes at least three quarters of the holds of 3+ vblanks");

    // ---- 3. Toy Time E in detail, and the existing parameters -------------------------------------------------------------------------------
    Header("3. Toy Time (E costs): what the existing parameters do, and pace25 (mean of 8 seeds)");
    {
        const Params base = fits[1][0].p;
        struct V { const char* name; void (*f)(Params&); } vars[] = {
            {"baseline: PIPE_DEPTH 2, MAXSTEP 3, 3 buffers, PRESENT_SYNC 2", [](Params&) {}},
            {"PETARI_PIPE_DEPTH=3", [](Params& p) { p.pipeDepth = 3; }},
            {"GXV_SKIP_MAXSTEP=2", [](Params& p) { p.maxStep = 2; }},
            {"GXV_SKIP_MAXSTEP=2 + PETARI_PIPE_DEPTH=3", [](Params& p) { p.maxStep = 2; p.pipeDepth = 3; }},
            {"GXV_VITA_DISPLAY_BUFFERS=4", [](Params& p) { p.bufs = 4; }},
            {"GXV_VITA_DISPLAY_BUFFERS=4 + PIPE_DEPTH=3", [](Params& p) { p.bufs = 4; p.pipeDepth = 3; }},
            {"GXV_PRESENT_SYNC=0 (pictures shown when ready)", [](Params& p) { p.syncMode = 0; }},
            {"GXV_PACE25=1", [](Params& p) { p.pace25 = true; }},
            {"GXV_PACE25=1 + GXV_VITA_DISPLAY_BUFFERS=4", [](Params& p) { p.pace25 = true; p.bufs = 4; }},
        };
        std::printf("%-52s  fps  1/2/3+ holds   maxhold  sd  1-1-3  turns/s  lat(ms)  guard/s\n", "setting");
        for (const V& v : vars) {
            Params p = base;
            v.f(p);
            const Result r = Mean(p, 8);
            std::printf("%-52s %5.1f  %4.2f/%4.2f/%4.2f  %4.0f   %4.2f  %4.2f  %6.1f  %6.1f   %.1f\n", v.name, r.fps, r.frac1, r.frac2, r.frac3, r.maxHold, r.holdSd, r.pat[0], r.turnsPerSec, r.lat,
                        r.guard / 8.0 / (p.seconds));
        }
    }

    // ---- 4. a sweep: the cadence as a function of the work per picture --------------------------------------------------------------------------
    Header("4. sweep of the GPU occupancy a picture (E's turns: drawn 17.7 ms, other 9.2; front end 14.8, back end cpu 11.5; 8 seeds). "
           "L = legacy rule, M2 = legacy + GXV_SKIP_MAXSTEP=2, P = GXV_PACE25=1");
    std::printf("GPU ms  refr |  fps L / M2 / P     | 3+ holds L / M2 / P   | hold sd L / M2 / P | logic turns/s L / M2 / P | shown-minus-turn ms L / M2 / P | P level\n");
    double worstP3 = 0, worstL3 = 0, worstM3 = 0, lostFps = 0, worstLogicP = 99, worstLogicM = 99;
    for (double g : {14.0, 16.0, 17.0, 18.0, 19.0, 19.5, 20.0, 20.5, 21.0, 21.5, 22.0, 22.5, 23.0, 24.0, 26.0, 28.0, 31.0, 34.0}) {
        Params p;
        p.d = 17.7; p.n = 9.2; p.fe = 14.8; p.be = 11.5; p.g = g;
        const Result a = Mean(p, 8);
        Params q = p;
        q.maxStep = 2;
        const Result c = Mean(q, 8);
        p.pace25 = true;
        const Result b = Mean(p, 8);
        std::printf("%5.1f  %4.2f | %5.1f %5.1f %5.1f   | %5.3f %5.3f %5.3f     | %4.2f %4.2f %4.2f     | %5.1f %5.1f %5.1f        | %5.1f %5.1f %5.1f            | %2u\n", g, g / kV, a.fps, c.fps, b.fps,
                    a.frac3, c.frac3, b.frac3, a.holdSd, c.holdSd, b.holdSd, a.turnsPerSec, c.turnsPerSec, b.turnsPerSec, a.lat, c.lat, b.lat, b.level);
        if (g <= 30.0) worstP3 = std::max(worstP3, b.frac3);
        if (g <= 30.0) worstM3 = std::max(worstM3, c.frac3);
        worstL3 = std::max(worstL3, a.frac3);
        lostFps = std::max(lostFps, a.fps - b.fps);
        worstLogicP = std::min(worstLogicP, b.turnsPerSec);
        worstLogicM = std::min(worstLogicM, c.turnsPerSec);
    }
    std::printf("worst share of 3+ holds in the sweep (GPU up to 30 ms): legacy %.3f, MAXSTEP=2 %.3f, pace25 %.3f; most pictures a second pace25 gives up against legacy: %.1f; "
                "lowest logic rate: MAXSTEP=2 %.1f, pace25 %.1f turns a second\n", worstL3, worstM3, worstP3, lostFps, worstLogicM, worstLogicP);
    Require(worstP3 < 0.02, "pace25: under 2 % of the holds are 3+ vblanks whenever the GPU needs less than 1.8 refreshes");
    Require(worstL3 > 0.08, "the legacy rule has a band of work per picture with more than 8 % holds of 3+ (the 1-1-1-3)");
    Require(lostFps < 4.5, "pace25 gives up less than 4.5 pictures a second against the legacy rule at any cost");
    Require(worstLogicP >= 59.5, "pace25 keeps the logic rate (60 turns a second)");

    Header("4b. a game that fits one refresh (drawn turn 15 ms, other 8): the strict 60 is the legacy rule's, nothing is added");
    for (double g : {10.0, 14.0, 16.0}) {
        Params p;
        p.d = 15.0; p.n = 8.0; p.fe = 12.0; p.be = 8.0; p.g = g;
        const Result a = Mean(p, 6);
        p.pace25 = true;
        const Result b = Mean(p, 6);
        std::printf("GPU %4.1f ms: legacy %5.1f fps, latency %5.1f ms | pace25 %5.1f fps, latency %5.1f ms, level %u\n", g, a.fps, a.lat, b.fps, b.lat, b.level);
        Require(b.level == 0 && std::abs(b.fps - a.fps) < 0.05 && std::abs(b.lat - a.lat) < 0.5, "a stage that fits one refresh is identical with pace25 on");
    }
    (void)quiet;
    std::printf("\npace25_model: ok\n");
    return 0;
}
