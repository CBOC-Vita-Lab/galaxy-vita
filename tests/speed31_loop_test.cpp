// speed31: CLOSED-LOOP host model. The real speed guard + the real pacers (tests/speed31_model.h) drive the real Quality ladder (port/q30lock.h) and the real Performance governors
// (port/fegov.h), whose cuts take ms off the pair in force (the TRUE gains are the ladder's defaults times a scale; each step is walked in over its fade). Prints, per scenario, seconds of dropped
// pictures before the cuts absorb a load step, the worst 1 s of game speed, the first cut's delay, and the recovery after the load drops. The checks are the acceptance of the report.
#include "speed31_model.h"
#include "port/cpulod.h"
#include "port/fegov.h"
#include "port/lockfade.h"
#include "port/q30lock.h"
using namespace m31;
namespace q = port::q30lock;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct Out {
    double dropSecs = 0;        // seconds (after the step) with at least one picture dropped by the guard
    double dropSecsTotal = 0;
    double firstCut = -1;       // seconds from the load step to the first change of the step set
    double settled = -1;        // seconds from the load step to the first second after which no picture was dropped for 5 s
    double worst1s = 100, worst5s = 100;
    double fullAfterDrop = -1;  // seconds from the load drop to an empty step set
    unsigned changes = 0;
    double picAfter = 0;        // pictures a second in the last 20 s before the drop
    double speed = 0;
    int stepsAtEnd = 0, maxSteps = 0;
    unsigned dropPicsAfterSettle = 0;
    unsigned retakes = 0;
};

// ---- Quality: pair 30 -> `hi` at 20 s -> 30 at `dropAt`
static Out QualityLoop(double hi, bool signalOn, bool guardOn, double scale, double dropAt, double total, bool verbose, unsigned maxSteps = 4, bool esc = true) {
    q::Ladder L;
    q::Memory mem;
    q::Config cfg;
    cfg.guard = signalOn;
    cfg.guardMaxSteps = static_cast<int>(maxSteps);
    cfg.memory = false;
    cfg.allowed = 0x7FFF & ~((1u << q::k5) | (1u << q::k6));   // (5 and 6 need the back end)
    cfg.startSpec[0] = 0; cfg.openSpec[0] = 0;
    L.Configure(cfg);
    L.SetStep6Ready(false);
    const uint64_t base = 1000000;
    L.StageBegin(base, "Loop:1", &mem);
    Scn s;
    s.quality = true; s.guard = guardOn; s.esc = esc; s.seconds = total; s.leakPm = 5; s.budgetMs = 20;
    const double stepAt = 20.0;
    s.pair = [=](double t) { return t < stepAt ? 30.0 : (t < dropAt ? hi : 30.0); };
    // per-step ramp state (true gain walks in over the step's fade)
    double rampAt[q::kStepN];
    for (double& r : rampAt) r = -1;
    uint16_t lastMask = 0;
    int64_t lastT = 0;
    Out o;
    int64_t t0 = 0;
    std::vector<double> dropSecAt;   // per-second drop counts
    std::vector<unsigned> dropsInSec(static_cast<size_t>(total) + 2, 0);
    double firstChangeAt = -1, lastDropAt = -1, emptyAt = -1;
    unsigned changes = 0;
    int steps = 0, maxSt = 0;
    bool wasEmptyAfterDrop = false;
    s.perTurn = [&](int64_t tNs, SpeedGuard& g) {
        if (!t0) t0 = tNs;
        const uint64_t now = base + static_cast<uint64_t>((tNs - t0) / 1000);
        const double sec = (tNs - t0) / 1e9;
        const SpeedGuard::PressureInfo pr = g.Pressure();
        if (lastT) L.NoteSpeed(now, static_cast<uint32_t>((tNs - lastT) / 1000), true);
        lastT = tNs;
        L.NoteGuard(now, signalOn && pr.sustained, static_cast<float>(pr.needMs));
        const uint16_t m = L.mask();
        if (m != lastMask) {
            for (int st = 0; st < q::kStepN; ++st) {
                const bool was = lastMask >> st & 1u, is = m >> st & 1u;
                if (!was && is) rampAt[st] = sec;
                if (was && !is) rampAt[st] = -2 - sec;   // (given back: ramps out)
            }
            ++changes;
            if (firstChangeAt < 0 && sec >= stepAt) firstChangeAt = sec - stepAt;
            if (verbose) std::printf("   t=%6.1f mask %04x -> %04x\n", sec, lastMask, m);
            lastMask = m;
        }
        steps = q::PopCount(m);
        maxSt = std::max(maxSt, steps);
        double cut = 0;
        for (int st = 0; st < q::kStepN; ++st) {
            if (!(q::kHelps[st] & q::kGame)) continue;
            const double gain = q::kDefaultGainMs[st] * scale;
            const double dur = q::kTransMs[st] / 1000.0;
            if (rampAt[st] >= 0) cut += gain * std::min(1.0, (sec - rampAt[st]) / dur);
            else if (rampAt[st] <= -2) cut += gain * std::max(0.0, 1.0 - (sec - (-2 - rampAt[st])) / dur);
        }
        return cut;
    };
    s.afterTurn = [&](int64_t tNs, int code, bool extra, double cost) {
        const uint64_t now = base + static_cast<uint64_t>((tNs - t0) / 1000);
        if (!extra) L.NoteTurn(now, static_cast<uint32_t>(cost * 1000.0), code == 0);
        if (extra) { const size_t sec = static_cast<size_t>((tNs - t0) / 1e9); if (sec < dropsInSec.size()) ++dropsInSec[sec]; lastDropAt = (tNs - t0) / 1e9; }
    };
    s.onPicture = [&](int64_t tNs, unsigned hold) {
        const uint64_t now = base + static_cast<uint64_t>((tNs - t0) / 1000);
        L.NotePicture(now, 20000u, true, 20000u, true, true);
        L.NoteShown(now, hold, true);
        L.NoteCover(now, false);
        const double sec = (tNs - t0) / 1e9;
        if (sec >= dropAt && emptyAt < 0 && L.mask() == 0) emptyAt = sec - dropAt;
        (void)wasEmptyAfterDrop;
    };
    const Res r = Run(s);
    o.worst1s = r.worst1s; o.worst5s = r.worst5s; o.speed = r.speed;
    o.changes = changes; o.firstCut = firstChangeAt; o.fullAfterDrop = emptyAt; o.maxSteps = maxSt; o.stepsAtEnd = steps;
    // drop seconds after the step until the end of the loaded phase
    const int a = static_cast<int>(stepAt), b = static_cast<int>(std::min(dropAt, total));
    int lastDropSec = -1;
    for (int i = a; i < b; ++i) if (dropsInSec[static_cast<size_t>(i)]) { o.dropSecs += 1; lastDropSec = i; }
    o.settled = lastDropSec < 0 ? 0.0 : (lastDropSec + 1 - stepAt);
    for (int i = 0; i < static_cast<int>(dropsInSec.size()); ++i) if (dropsInSec[static_cast<size_t>(i)]) o.dropSecsTotal += 1;
    o.picAfter = r.fps;
    (void)lastDropAt;
    char lg[400];
    if (verbose) while (L.PopLog(lg, sizeof lg)) std::printf("   %s\n", lg);
    return o;
}

// ---- Performance (60 world: a picture on every turn the rule does not leave out): lane 15 -> `hi` at 20 s -> 15 at `dropAt`. The game thread's governor (real fegov::Governor, level 0..3, true gain `gain` ms a
// level, walked in over 3 s) reads 5 s windows of the drawn turns' ms; the guard's signal = 8 of the last 30 turns left out by the skip rule (what perf.cpp SpeedGuardAct reads), one press at most
// every 3.5 s.
struct POut {
    double dropSecs = 0, firstCut = -1, settled = -1, worst1s = 100, worst5s = 100, backAfterDrop = -1, picsLoaded = 0, picsFinal = 0;
    int maxLevel = 0, endLevel = 0;
    unsigned changes = 0;
};
static POut PerfLoop(double hi, bool signalOn, double gain, double dropAt, double total, bool verbose) {
    port::fegov::Config c;
    c.maxLevel = 3; c.budgetMs = 15.5f; c.recover = true;
    port::fegov::Governor G(c);
    G.Configure(c);
    Scn s;
    s.quality = false; s.steady = false; s.N = 9.0; s.seconds = total; s.guard = true; s.leakPm = 5; s.budgetMs = 20;
    const double stepAt = 20.0;
    s.pair = [=](double t) { return (t < stepAt ? 15.0 : (t < dropAt ? hi : 15.0)) + 9.0; };
    int64_t t0 = 0, lastPress = -100000000000ll, winAt = 0;
    uint32_t missBits = 0;
    int lastLevel = 0;
    double levelAt = -100;      // when the level last changed (the walk's start)
    double fromCut = 0;
    uint64_t winPics = 0, winDrawn = 0, winSumUs = 0;
    double firstCut = -1, backAt = -1;
    unsigned changes = 0;
    int maxL = 0;
    std::vector<unsigned> missSec(static_cast<size_t>(total) + 2, 0);
    double pressedAt = -100;
    s.perTurn = [&](int64_t tNs, SpeedGuard& g) {
        (void)g;
        if (!t0) { t0 = tNs; winAt = tNs; }
        const double sec = (tNs - t0) / 1e9;
        if (signalOn && __builtin_popcount(missBits & 0x3FFFFFFFu) >= 8 && tNs - lastPress >= 3500000000ll && sec > 3.0) {
            const int taken = G.Press(1.0f, 2);
            if (taken) {
                lastPress = tNs; pressedAt = sec;
                if (verbose) std::printf("   t=%6.1f press -> level %d\n", sec, G.level());
            }
        }
        if (tNs - winAt >= 5000000000ll) {
            port::fegov::Window w;
            w.pictures = static_cast<uint32_t>(winPics); w.frontPics = static_cast<uint32_t>(winDrawn); w.frontSumUs = winSumUs;
            w.guard = sec - pressedAt < 8.0;
            G.Feed(w);
            winPics = winDrawn = winSumUs = 0;
            winAt = tNs;
        }
        const int lv = G.level();
        if (lv != lastLevel) {
            fromCut = lastLevel * gain;   // (the cut in force when the walk started)
            const double elapsed = sec - levelAt;
            if (elapsed < 3.0) fromCut = std::min(lastLevel, lv) * gain + (std::max(lastLevel, lv) - std::min(lastLevel, lv)) * gain * 0;   // (a walk interrupted: take the lower)
            levelAt = sec;
            ++changes;
            if (firstCut < 0 && sec >= stepAt) firstCut = sec - stepAt;
            if (verbose) std::printf("   t=%6.1f level %d -> %d\n", sec, lastLevel, lv);
            if (lv == 0 && sec >= dropAt && backAt < 0) backAt = sec - dropAt;
            lastLevel = lv;
        }
        maxL = std::max(maxL, lv);
        const double target = lv * gain;
        const double f = std::min(1.0, (sec - levelAt) / 3.0);
        return fromCut + (target - fromCut) * f;
    };
    s.afterTurn = [&](int64_t tNs, int code, bool extra, double cost) {
        const bool missed = code != 0 && !extra;
        missBits = (missBits << 1) | (missed ? 1u : 0u);
        if (code == 0) { ++winPics; ++winDrawn; winSumUs += static_cast<uint64_t>(cost * 1000.0); }
        if (missed || extra) { const size_t sec = static_cast<size_t>((tNs - t0) / 1e9); if (sec < missSec.size()) ++missSec[sec]; }
    };
    const Res r = Run(s);
    POut o;
    o.worst1s = r.worst1s; o.worst5s = r.worst5s; o.firstCut = firstCut; o.maxLevel = maxL; o.endLevel = G.level(); o.changes = changes; o.backAfterDrop = backAt;
    int last = -1;
    for (int i = static_cast<int>(stepAt); i < static_cast<int>(std::min(dropAt, total)); ++i) if (missSec[static_cast<size_t>(i)]) { o.dropSecs += 1; last = i; }
    o.settled = last < 0 ? 0.0 : last + 1 - stepAt;
    o.picsFinal = r.fps;
    return o;
}
int main(int argc, char** argv) {
    const bool verbose = argc > 1 && std::atoi(argv[1]) != 0;
    std::printf("== Quality closed loop: pair 30 -> HI ms at 20 s -> 30 at 140 s (the ladder's own steps, true gains = the guesses x scale; the guard on, signal on / off)\n");
    for (double hi : {33.0, 34.0, 36.0, 38.0, 40.0, 42.0}) {
        for (int sig = 1; sig >= 0; --sig) {
            Out o = QualityLoop(hi, sig != 0, true, 1.0, 140, 220, verbose && hi == 38.0 && sig);
            std::printf("  pair %.0f, signal %s: first cut %5.1f s after the step; seconds with a dropped picture %4.0f of the %.0f loaded; dropping stopped %5.1f s after the step; worst 1 s %5.1f %%, worst 5 s %5.1f %%; steps max %d, at the end %d; full quality %5.1f s after the drop; changes %u\n",
                        hi, sig ? "ON " : "off", o.firstCut, o.dropSecs, 120.0, o.settled, o.worst1s, o.worst5s, o.maxSteps, o.stepsAtEnd, o.fullAfterDrop, o.changes);
            if (hi >= 36.0 && sig) {
                CHECK(o.firstCut >= 0 && o.firstCut <= 1.5);      // the first cut begins about 1 s after the step
                CHECK(o.worst5s >= 98.0);
                CHECK(o.worst1s >= 95.0);
                CHECK(o.fullAfterDrop >= 0 && o.fullAfterDrop <= 40.0);
                CHECK(o.settled >= 0 && o.settled <= 20.0);
                Out off = QualityLoop(hi, false, true, 1.0, 140, 220, false);
                CHECK(o.settled <= off.settled);
            }
        }
    }
    {   // an isolated hitch must NOT take a cut: pair 30 with one 3 s load step of 38 ms (shorter than the 0.25 s confirm is a hitch: 0.2 s)
        Out o = QualityLoop(38.0, true, true, 1.0, 20.2, 80, false);
        std::printf("  isolated 0.2 s hitch (pair 38): changes %u, steps max %d, worst 5 s %5.1f %%\n", o.changes, o.maxSteps, o.worst5s);
        Out o2 = QualityLoop(38.0, false, true, 1.0, 20.2, 80, false);
        std::printf("  same, signal off: changes %u, steps max %d\n", o2.changes, o2.maxSteps);
        CHECK(o.changes == o2.changes && o.maxSteps == o2.maxSteps);   // the guard signal adds no cut to a hitch (the ladder's own missed-picture signal is today's behaviour)
        Out o3 = QualityLoop(38.0, true, true, 1.0, 20.08, 80, false);
        std::printf("  0.08 s hitch: changes %u, steps max %d\n", o3.changes, o3.maxSteps);
        CHECK(o3.maxSteps == 0);
        CHECK(o.worst5s >= 98.0);
    }
    std::printf("== Performance closed loop (60 world, game thread governor, true gain 1.6 ms a level, levels 0..3): lane 15 -> HI ms at 20 s -> 15 at 140 s\n");
    for (double hi : {17.0, 18.0, 20.0, 22.0}) {
        for (int sig = 1; sig >= 0; --sig) {
            POut o = PerfLoop(hi, sig != 0, 1.6, 140, 260, verbose && hi == 20.0 && sig);
            if (hi == 20.0 && sig) { CHECK(o.firstCut >= 0 && o.firstCut <= 1.5); CHECK(o.worst5s >= 98.0); CHECK(o.endLevel == 0); }
            if (hi == 22.0) CHECK(o.worst5s >= 98.0);
            std::printf("  lane %.0f, signal %s: first cut %5.1f s after the step; seconds with a left-out turn %4.0f of 120; stopped %5.1f s after the step; worst 1 s %5.1f %%, worst 5 s %5.1f %%; level max %d, at the end %d; level 0 again %5.1f s after the drop; changes %u\n",
                        hi, sig ? "ON " : "off", o.firstCut, o.dropSecs, o.settled, o.worst1s, o.worst5s, o.maxLevel, o.endLevel, o.backAfterDrop, o.changes);
        }
    }
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
