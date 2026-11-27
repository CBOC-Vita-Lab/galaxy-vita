// pace26: frame pacing policies on a model of the console pipeline, driven by the per-window costs of the c79 console logs (tests/even26_trace.inc: jobs 1786, 1786b,
// 1786c, 1787, 1788; tools: new/pace26/tools/extract26.py). A MODEL, not a console measurement: it runs the real port::pace classes (LegacySkip, TurnClock, PresentSync,
// Latency24, Controller, PresentationSlots, Pace25, Even26, EvenMeter) on a timeline of
//   game thread (drawn turn d ms, other n ms, retrace queue, draw-done wait for the front end)
//   -> front end (fe ms a picture, FrameSink gate: at most 3 pictures in the back end's queue)
//   -> back end (cpu ms; its first scene waits for the display buffer two pictures back; AddEntry waits for the display queue)
//   -> GPU (g ms a picture, in order) -> display callback (PresentSync: held to stamp + L; or PresentationSlots every 2 vblanks; or when ready).
// Per row of the trace the GPU's occupancy is scaled (0.6..1.6) until the LEGACY rule's pictures a second equal the console's; the holds and the turns a picture
// advanced are then the check (section 1), not fitted. Section 2 runs every policy on the same fitted rows, section 3 sweeps the GPU's cost, section 4 is the
// Quality (locked 30) rhythm, section 5 the unit checks of Even26 and EvenMeter.
//   tools/test_even26_model.sh
#include "port/even26.h"
#include "port/frame_pace.h"
#include "port/pace25.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

namespace {
using namespace port::pace;
constexpr double kV = 16.68335;

struct Row {
    const char *run, *stage;
    int win;
    double d, n, gpu, be, fe;
    int pics, h1, h2, h3, h4, s1, s2, s3, s4, L, r3, r4, r5, r6, r7;
    double sema;
};
const Row kRows[] = {
#include "even26_trace.inc"
};
constexpr size_t kRowCount = sizeof kRows / sizeof kRows[0];

enum class Pol { Legacy, LegacyMs2, Free, Pace25, Even26, Even26L24, Steady30, Fixed30, Fixed30Even, Fixed30EvenSlack };
const char* PolName(Pol p) {
    switch (p) {
        case Pol::Legacy: return "legacy (shipped: maxstep 3, present sync 2)";
        case Pol::LegacyMs2: return "legacy + GXV_SKIP_MAXSTEP=2";
        case Pol::Free: return "free-running (present sync 0: shown when ready)";
        case Pol::Pace25: return "GXV_PACE25=1";
        case Pol::Even26: return "GXV_EVEN26=1";
        case Pol::Even26L24: return "GXV_EVEN26=1 + GXV_LATENCY24=1";
        case Pol::Steady30: return "steady 30 (every second turn, present sync 2)";
        case Pol::Fixed30: return "Quality today (Fixed30 + slots)";
        case Pol::Fixed30Even: return "Quality GXV_EVEN26_Q=4";
        case Pol::Fixed30EvenSlack: return "Quality GXV_EVEN26_Q=4 + QSLACK=1";
    }
    return "";
}

struct Params {
    double d = 18.0, n = 9.0, fe = 15.0, be = 11.0, g = 20.0;
    double sigma = 0.15, sigmaG = 0.08;  // relative spread of the CPU costs and of the GPU's (EVEN26_SIGMA / EVEN26_SIGMAG override)
    int bufs = 3;
    unsigned pipeDepth = 2, maxRun = 3, maxPipeRun = 6, queueLimit = 3;
    unsigned latMax = 5;
    double tickQueue = 4;
    double gscale = 1.0;
    int beTie = 0;  // how far the back end's job completion follows the GPU (EVEN26_BETIE: 0 not at all, 1 the previous picture's GPU, 2 half of its own)
    double seconds = 40, warm = 6;
    unsigned seed = 1;
    Pol pol = Pol::Legacy;
};

struct Agg {
    double secs = 0, pics = 0, ideal = 0, missed = 0, missedHold = 0, missedStep = 0, offSum = 0, stepKnown = 0, latSum = 0, latKnown = 0, turns = 0, holdSum = 0, holdSq = 0, breaks = 0,
           Lsum = 0;
    double hold[5] = {}, step[5] = {};
    double maxHold = 0, clumps = 0;
    double fps() const { return secs ? pics / secs : 0; }
    double idealPct() const { return pics ? 100 * ideal / pics : 0; }
    double judder() const { return stepKnown ? offSum / stepKnown : 0; }
    double latMs() const { return latKnown ? latSum / latKnown * kV : 0; }
    double missedPs() const { return secs ? missed / secs : 0; }
    double holdSd() const { const double m = pics ? holdSum / pics : 0; return pics ? std::sqrt(std::max(0.0, holdSq / pics - m * m)) : 0; }
    double breaksPs() const { return secs ? breaks / secs : 0; }
    double turnsPs() const { return secs ? turns / secs : 0; }
    double score() const { const double s = idealPct() - 25 * judder() - 5 * missedPs(); return s < 0 ? 0 : s > 100 ? 100 : s; }
    double Lmean() const { return pics ? Lsum / pics : 0; }
    void Add(const Agg& o) {
        secs += o.secs; pics += o.pics; ideal += o.ideal; missed += o.missed; missedHold += o.missedHold; missedStep += o.missedStep; offSum += o.offSum; stepKnown += o.stepKnown;
        latSum += o.latSum; latKnown += o.latKnown; turns += o.turns; holdSum += o.holdSum; holdSq += o.holdSq; breaks += o.breaks; Lsum += o.Lsum;
        for (int i = 0; i < 5; ++i) { hold[i] += o.hold[i]; step[i] += o.step[i]; }
        maxHold = std::max(maxHold, o.maxHold);
        clumps += o.clumps;
    }
};

Agg Simulate(const Params& P) {
    std::mt19937 rng(P.seed * 7919u + 13u);
    auto costS = [&](double m, double sg) {
        std::normal_distribution<double> dist(m, m * sg);
        const double v = dist(rng);
        return v < m * 0.5 ? m * 0.5 : v;
    };
    auto cost = [&](double m) { return costS(m, P.sigma); };
    const bool quality = P.pol == Pol::Fixed30 || P.pol == Pol::Fixed30Even || P.pol == Pol::Fixed30EvenSlack;
    const unsigned maxStep = P.pol == Pol::LegacyMs2 ? 2 : 3;
    LegacySkip rule(static_cast<int>(P.maxRun), static_cast<int>(P.pipeDepth), static_cast<int>(P.maxPipeRun), nullptr, static_cast<int>(maxStep));
    Pace25 pace25;
    Pace25Config pc;
    pc.mode = P.pol == Pol::Pace25 ? 1 : 0;
    pace25.Configure(pc);
    Even26 even;
    Even26Config ec;
    ec.mode = (P.pol == Pol::Even26 || P.pol == Pol::Even26L24) ? 1 : 0;
    if (const char* e = std::getenv("EVEN26_UP")) ec.gainUp = std::atof(e);
    if (const char* e = std::getenv("EVEN26_DOWN")) ec.gainDown = std::atof(e);
    if (const char* e = std::getenv("EVEN26_GUARD")) ec.guard = static_cast<unsigned>(std::atoi(e));
    if (const char* e = std::getenv("EVEN26_EVENTAT")) ec.eventAt = static_cast<unsigned>(std::atoi(e));
    if (const char* e = std::getenv("EVEN26_EARLY")) ec.early = std::atof(e);
    if (const char* e = std::getenv("EVEN26_EARLYP")) ec.earlyPending = static_cast<unsigned>(std::atoi(e));
    if (const char* e = std::getenv("EVEN26_HLO")) ec.hintLo = std::atof(e);
    if (const char* e = std::getenv("EVEN26_HHI")) ec.hintHi = std::atof(e);
    if (const char* e = std::getenv("EVEN26_WINDOW")) ec.window = static_cast<unsigned>(std::atoi(e));
    even.Configure(ec);
    Controller fixed(false, Policy::Fixed30);
    if (P.pol == Pol::Fixed30Even || P.pol == Pol::Fixed30EvenSlack) fixed.SetEvenQ(4);
    PresentationSlots slots;
    if (P.pol == Pol::Fixed30EvenSlack) slots.SetSlack(1);
    TurnClock clock;
    PresentSync sync;
    sync.Configure(P.pol != Pol::Free, 1, P.latMax, 3, 4);
    if (P.pol == Pol::Even26L24) {
        PresentSync::Latency24 l;
        l.on = true;
        l.minKnown = l.window / 2;
        sync.ConfigureLatency24(l);
    }
    const double pipeHint = std::max({P.fe, P.be, P.g * P.gscale});

    std::vector<double> feDone, beDone, show, turnAt;
    std::vector<int> turnIdx;
    std::vector<uint32_t> showV, turnV;
    std::vector<unsigned> latL;
    double nextTick = 1.0;
    double t = 0, feFree = 0, gpuFree = 0, cbReturn = 0, drawDoneWait = -1;
    unsigned turns = 0;
    bool lastDrew = false;
    const double end = P.seconds * 1000.0;
    uint64_t warmTurns = 0;
    double warmEnd = P.warm * 1000.0;
    bool warmMarked = false;
    while (t < end) {
        const uint32_t vcount = static_cast<uint32_t>(t / kV);
        unsigned queued = 0, done = 0;
        for (size_t k = feDone.size() > 12 ? feDone.size() - 12 : 0; k < feDone.size(); ++k) {
            if (feDone[k] <= t) ++queued;
            if (beDone[k] <= t) ++done;
        }
        const unsigned pending = queued > done ? queued - done : 0;
        const int64_t nowNs = static_cast<int64_t>(t * 1e6);
        int code = 0;
        if (quality) {
            code = fixed.Turn(static_cast<uint64_t>(t * 1000.0), true, false, pending, 0, 0);
        } else {
            const LegacySkip::Verdict v = rule.Turn(nowNs, true, pending);
            code = v.code;
            if (P.pol == Pol::Pace25) {
                Pace25::Turn pt{static_cast<int64_t>(t * 1000.0), true, false, v.code, v.behind, pending, static_cast<uint32_t>(pipeHint * 1000.0)};
                code = pace25.Decide(pt);
            } else if (P.pol == Pol::Even26 || P.pol == Pol::Even26L24) {
                Even26::Turn et{true, false, v.code, v.behind, v.gpuBusy && !v.behind, v.capped, pending, std::getenv("EVEN26_NOHINT") ? 0u : static_cast<uint32_t>(pipeHint * 1000.0)};
                code = even.Decide(et);
            } else if (P.pol == Pol::Steady30) {
                if (lastDrew && code == 0) code = 1;
            }
        }
        const bool drawn = code == 0;
        lastDrew = drawn;
        const uint32_t vblank = clock.Turn(vcount);
        double w = t + cost(0.6 * P.n);
        const double recStart = w;
        if (drawn && drawDoneWait > w) w = drawDoneWait;
        if (drawn) drawDoneWait = -1;
        w += cost(0.4 * P.n);
        if (drawn) w += cost(P.d - P.n);
        if (P.pol == Pol::Pace25) pace25.WorkDone(static_cast<int64_t>(w * 1000.0), drawn);
        if (drawn) {
            const size_t k = feDone.size();
            const double fe0 = std::max(recStart, feFree);
            double fe1 = std::max(fe0 + cost(P.fe), w);
            if (k >= P.queueLimit) fe1 = std::max(fe1, beDone[k - P.queueLimit]);
            feFree = fe1;
            drawDoneWait = fe1;
            const double prevBe = k ? beDone[k - 1] : 0.0;
            const double s = std::max(fe1, prevBe);
            const double bufFree = k >= static_cast<size_t>(P.bufs - 1) ? show[k - (P.bufs - 1)] : 0.0;
            const double tScene = std::max(s + 1.0, bufFree);
            double be1 = tScene + std::max(0.0, cost(P.be) - 1.0);
            const double gStart = std::max(tScene + 1.0, gpuFree);
            if (P.beTie == 1) be1 = std::max(be1, gpuFree);                  // the job ends when the GPU has finished the picture before it
            else if (P.beTie == 2) be1 = std::max(be1, gStart + 0.5 * P.g);  // ... or half way through its own
            else if (P.beTie == 3) be1 = std::max(be1, bufFree);             // ... or when the display queue has taken the picture two before it (AddEntry blocks)
            const double gDone = gStart + costS(P.g * P.gscale, P.sigmaG);
            gpuFree = gDone;
            const double e = std::max(gDone, cbReturn);
            const uint32_t earliest = static_cast<uint32_t>(e / kV) + 1;
            uint32_t target = earliest;
            if (quality) {
                target = std::max(earliest, slots.Next(earliest, 2));
                slots.Shown(target);
            } else {
                if (P.pol != Pol::Free) {
                    if (sync.Latency24On()) sync.NoteBacklog(gDone <= cbReturn + 1.2);
                    target = sync.Target(earliest, true, static_cast<uint16_t>(vblank));
                }
                sync.Shown(target, target > earliest);
                latL.push_back(sync.Latency());
            }
            if (quality) latL.push_back(0);
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
        if (!warmMarked && t >= warmEnd) { warmMarked = true; warmTurns = turns; }
        if (code != 2) {
            const double newest = std::floor(w / kV);
            if (nextTick < newest - P.tickQueue + 1) nextTick = newest - P.tickQueue + 1;
            t = std::max(w, nextTick * kV);
            nextTick += 1.0;
        } else {
            t = w;
        }
    }
    Agg a;
    EvenMeter meter;
    double prevHold = 0;
    for (size_t k = 1; k < showV.size(); ++k) {
        if (turnAt[k] < warmEnd) continue;
        const int32_t h = static_cast<int32_t>(showV[k] - showV[k - 1]);
        const unsigned hold = h < 1 ? 1u : static_cast<unsigned>(h);
        const unsigned step = static_cast<unsigned>(turnIdx[k] - turnIdx[k - 1]);
        const int lat = static_cast<int>(static_cast<int32_t>(showV[k] - turnV[k]));
        meter.Picture(hold, step, -1, lat < 0 ? 0 : lat);
        a.holdSum += hold; a.holdSq += static_cast<double>(hold) * hold;
        a.hold[hold < 4 ? hold : 4] += 1; a.step[step < 4 ? step : 4] += 1;
        a.maxHold = std::max<double>(a.maxHold, hold);
        a.offSum += hold > step ? hold - step : step - hold;
        a.stepKnown += 1;
        a.latSum += lat < 0 ? 0 : lat; a.latKnown += 1;
        a.Lsum += latL[k];
        if (prevHold && prevHold != hold) a.breaks += 1;
        prevHold = hold;
    }
    const double secs = P.seconds - P.warm;
    const EvenMeter::Window w = meter.Take();
    const EvenMeter::Score sc = EvenMeter::Compute(w, secs);
    a.secs = secs; a.pics = w.pictures; a.ideal = sc.ideal; a.missed = w.missed; a.missedHold = w.missedHold; a.missedStep = w.missedStep;
    a.turns = static_cast<double>(turns - warmTurns) * 1.0;
    a.turns = static_cast<double>(turns) / (t / 1000.0) * secs;
    return a;
}

Agg Mean(const Params& base, unsigned seeds) {
    Agg m;
    for (unsigned s = 1; s <= seeds; ++s) {
        Params p = base;
        p.seed = s;
        m.Add(Simulate(p));
    }
    return m;
}

Params FromRow(const Row& r) {
    Params p;
    p.d = r.d; p.n = std::min(r.n, r.d * 0.9); p.g = std::max(r.gpu, 4.0); p.fe = std::max(r.fe, 3.0); p.be = std::max(r.be + (std::getenv("EVEN26_NOSEMA") ? 0.0 : r.sema), 1.5);  // (the back end's wall: its cpu and its semaphore wait)
    if (p.d < p.n + 1) p.d = p.n + 1;
    p.seconds = 20; p.warm = 5;
    if (const char* e = std::getenv("EVEN26_SIGMA")) p.sigma = std::atof(e);
    if (const char* e = std::getenv("EVEN26_SIGMAG")) p.sigmaG = std::atof(e);
    if (const char* e = std::getenv("EVEN26_BETIE")) p.beTie = std::atoi(e);
    return p;
}
double RowFps(const Row& r) { return r.pics / 5.0; }
// fit the GPU scale so that the legacy rule's pictures a second equal the console's
Params Fit(const Row& r) {
    Params p = FromRow(r);
    p.pol = Pol::Legacy;
    double lo = 0.6, hi = 1.8;
    const double want = RowFps(r);
    Agg a = Mean(p, 2);
    p.gscale = 1.0;
    a = Mean(p, 2);
    if (a.fps() >= want - 0.5 && p.g < 14.0) return p;  // (not pipeline bound: the GPU's cost does not matter)
    for (int it = 0; it < 9; ++it) {
        p.gscale = 0.5 * (lo + hi);
        const Agg b = Mean(p, 2);
        if (b.fps() > want) lo = p.gscale; else hi = p.gscale;
    }
    p.gscale = 0.5 * (lo + hi);
    return p;
}

void Header(const char* s) { std::printf("\n== %s\n", s); }
bool Perf(const Row& r) { return std::strcmp(r.run, "S") != 0 && std::strcmp(r.run, "SS") != 0 && std::strcmp(r.run, "ALT") != 0; }
}  // namespace


namespace {
struct Cat { const char* name; double lo, hi; };
const Cat kCats[] = {{"at 60 (console >= 58 a second)", 58, 99}, {"50-58 a second", 50, 58}, {"40-50 a second", 40, 50}, {"30-40 a second", 30, 40}, {"under 30", 0, 30}};
const Pol kPerf[] = {Pol::Legacy, Pol::LegacyMs2, Pol::Free, Pol::Pace25, Pol::Even26, Pol::Even26L24, Pol::Steady30};
void PrintHead() {
    std::printf("%-46s  fps  ideal%%  hold 1/2/3+ %%    sd  breaks/s  judder  missed/s  lat ms   L  turns/s  score\n", "policy");
}
void PrintAgg(const char* name, const Agg& a) {
    const double n = a.pics ? a.pics : 1.0;
    std::printf("%-46s %5.1f  %5.1f   %3.0f/%3.0f/%3.0f   %4.2f   %5.1f   %6.3f   %6.2f   %6.1f  %3.1f   %5.1f  %5.1f\n", name, a.fps(), a.idealPct(), 100 * a.hold[1] / n, 100 * a.hold[2] / n,
                100 * (a.hold[3] + a.hold[4]) / n, a.holdSd(), a.breaksPs(), a.judder(), a.missedPs(), a.latMs(), a.Lmean(), a.turnsPs(), a.score());
}
}  // namespace

int main(int argc, char** argv) {
    const bool quick = argc > 1 && !std::strcmp(argv[1], "-q");
    // ---- 5. unit checks (first: they are quick) ----------------------------------------------------------------------------------------------
    {   // deadfix27: fit60: a stage whose cost hint is one turn keeps period 1.0 under routine busy verdicts (pending 2 at a due turn); without it the period creeps to the hintHi cap
        for (int fit = 0; fit < 2; ++fit) {
            Even26 e;
            Even26Config c;
            c.mode = 1;
            c.fit60 = fit != 0;
            e.Configure(c);
            for (int i = 0; i < 6000; ++i) {
                Even26::Turn t{true, false, 0, false, false, false, 2};
                t.pipeUs = 14400;   // Floater: GPU 14.4 ms
                e.Decide(t);
            }
            if (fit) Require(e.EffectivePeriod() <= 1.0 + 1e-9 && e.Period() < 1.03, "fit60: a stage that fits a refresh stays at period 1.0");
            else Require(e.Period() > 1.15, "without fit60 the busy verdicts of a healthy 60 raise the period (the Floater regression)");
        }
    }
    {
        // Even26: a fixed period p draws floor / ceil of p turns only; the long holds are never adjacent while p - floor(p) < 0.5; the rate is 1/p.
        for (double period : {1.0, 1.1, 1.2, 1.25, 1.333, 1.5, 1.75, 1.8, 2.0, 2.4, 2.5}) {
            Even26 e;
            Even26Config c;
            c.mode = 1;
            c.snap30 = 0;
            c.snap60 = 0;
            c.gainUp = 0.0;
            c.gainDown = 0.0;
            e.Configure(c);
            e.SetPeriod(period);
            unsigned run = 0, draws = 0, prevHold = 0, longAdj = 0, maxHold = 0, shortAdj = 0, holds = 0;
            const unsigned lo = static_cast<unsigned>(std::floor(period + 1e-9)), hi = static_cast<unsigned>(std::ceil(period - 1e-9));
            for (int i = 0; i < 6000; ++i) {
                Even26::Turn t{true, false, 0, false, false, false, 0};
                const int code = e.Decide(t);
                ++run;
                if (code == 0) {
                    if (draws > 0 || i > 100) {
                        maxHold = std::max(maxHold, run);
                        if (i > 100) {
                            Require(run >= lo && run <= std::max(hi, 1u), "the holds are floor and ceil of the period");
                            if (prevHold && hi > lo) {
                                if (run == hi && prevHold == hi && period - lo < 0.5) ++longAdj;
                                if (run == lo && prevHold == lo && period - lo > 0.5) ++shortAdj;
                            }
                            prevHold = run;
                            ++holds;
                        }
                    }
                    ++draws;
                    run = 0;
                }
            }
            Require(longAdj == 0 && shortAdj == 0, "the minority holds are never adjacent (Bresenham)");
            Require(std::abs(draws / 6000.0 - 1.0 / period) < 0.01, "the rate is one picture per period");
        }
        // snap: 1.9 is an even 30, 1.02 is every turn
        {
            Even26 e; Even26Config c; c.mode = 1; c.gainUp = c.gainDown = 0; e.Configure(c);
            e.SetPeriod(1.9);
            Require(std::abs(e.EffectivePeriod() - 2.0) < 1e-9, "a period of 1.9 is exactly 2");
            e.SetPeriod(1.02);
            Require(std::abs(e.EffectivePeriod() - 1.0) < 1e-9, "a period of 1.02 is exactly 1");
            e.SetPeriod(1.5);
            Require(std::abs(e.EffectivePeriod() - 1.5) < 1e-9, "1.5 stays");
        }
        // census and off never change an answer; a load draws; behind skips are the legacy rule's
        for (int mode : {0, 2}) {
            Even26 e; Even26Config c; c.mode = mode; e.Configure(c);
            unsigned diff = 0;
            for (int i = 0; i < 3000; ++i) {
                Even26::Turn t{true, false, i % 3 == 0 ? 1 : 0, false, i % 3 == 0, false, static_cast<unsigned>(i % 4)};
                diff += e.Decide(t) != t.legacy;
            }
            Require(diff == 0, "census and off answer the legacy rule");
        }
        {
            Even26 e; Even26Config c; c.mode = 1; e.Configure(c);
            e.SetPeriod(1.5);
            Even26::Turn t{false, false, 0, false, false, false, 5};
            Require(e.Decide(t) == 0, "a load draws");
        }
        // a busy verdict at a due turn raises the period and still draws below the guard; at the guard it skips; windows without verdicts bring it down, never under 1
        {
            Even26 e; Even26Config c; c.mode = 1; c.snap30 = c.snap60 = 0; e.Configure(c);
            double p0 = e.Period();
            Even26::Turn t{true, false, 0, false, true, false, 2};
            Require(e.Decide(t) == 0, "the first turn draws");
            t.pending = 2;
            Require(e.Decide(Even26::Turn{true, false, 1, false, true, false, 2}) == 0, "a busy verdict at a due turn below the guard still draws");
            Require(e.Period() > p0, "... and raises the period");
            e.Configure(c); e.SetPeriod(1.3);
            unsigned guard = 0;
            for (int i = 0; i < 40; ++i) { const int code = e.Decide(Even26::Turn{true, false, 1, false, true, false, 3}); if (code != 0) ++guard; }
            Require(guard > 20, "at the guard the due pictures are left out");
            e.Configure(c); e.SetPeriod(1.5);
            for (int i = 0; i < 30 * 400; ++i) e.Decide(Even26::Turn{true, false, 0, false, false, false, 0});
            Require(std::abs(e.Period() - 1.0) < 1e-9, "quiet windows bring the period down to 1 and not under");
        }
        // EvenMeter: a perfect 1-1-1-1-2 is 100 % ideal, an adjacent pair of long holds is not; a 3 is a missed moment; judder
        {
            EvenMeter m;
            m.Picture(1, 1, -1, 5);
            for (int i = 0; i < 100; ++i) { for (int k = 0; k < 4; ++k) m.Picture(1, 1, -1, 5); m.Picture(2, 2, -1, 5); }
            EvenMeter::Score s = EvenMeter::Compute(m.Take(), 10.0);
            Require(s.idealPct > 99.0 && s.missed == 0 && s.judder == 0.0 && s.score > 99.0, "1-1-1-1-2 is on the ideal cadence");
            Require(std::abs(s.latVblanks - 5.0) < 1e-9, "latency");
            EvenMeter m2;
            m2.Picture(1, 1, -1, 5);
            for (int i = 0; i < 50; ++i) { m2.Picture(1, 1, -1, 5); m2.Picture(1, 1, -1, 5); m2.Picture(1, 1, -1, 5); m2.Picture(3, 3, -1, 5); }
            s = EvenMeter::Compute(m2.Take(), 10.0);
            Require(s.missed == 50 && s.score < 100.0, "1-1-1-3 has a missed moment each time");
            EvenMeter m3;
            m3.Picture(2, 2, -1, 5);
            for (int i = 0; i < 100; ++i) m3.Picture(2, 2, -1, 5);
            s = EvenMeter::Compute(m3.Take(), 10.0);
            Require(s.idealPct > 99.0 && s.lo == 2 && s.hi == 2, "an even 30 is ideal at 2");
            EvenMeter m4;
            m4.Picture(2, 2, -1, 5);
            for (int i = 0; i < 100; ++i) { m4.Picture(i % 7 == 0 ? 1 : 2, 2, -1, 5); }
            s = EvenMeter::Compute(m4.Take(), 10.0);
            Require(s.judder > 0.0 && s.idealPct < 90.0, "a hold that does not match its step is judder");
            EvenMeter m5;  // clumps: 2,2,1,1,1,1,1,1... a long run of 2s inside a rhythm of mostly 1
            m5.Picture(1, 1, -1, 4);
            for (int i = 0; i < 40; ++i) { for (int k = 0; k < 8; ++k) m5.Picture(1, 1, -1, 4); m5.Picture(2, 2, -1, 4); m5.Picture(2, 2, -1, 4); }
            s = EvenMeter::Compute(m5.Take(), 10.0);
            Require(s.idealPct < 85.0, "two long holds in a row are not on the cadence");
            char line[512];
            EvenMeter::Format(line, sizeof line, s, 10.0, "even26 off", 5);
            Require(std::strncmp(line, "even: 10.0 s", 12) == 0, "the line starts as the log expects");
        }
        // Controller evenQ: a due turn is never left out for a backlog of 2 or 3, never for late; the late catch-up goes to the other turn; offer passes on at the deep guard
        {
            Controller c(false, Policy::Fixed30);
            c.SetEvenQ(4);
            uint64_t now = 1000000;
            unsigned dueSkips = 0, draws = 0, twos = 0;
            for (int i = 0; i < 2000; ++i) {
                now += i % 5 == 0 ? 30000 : 16683;  // a late turn now and then
                const int d = c.Turn(now, true, false, i % 3 == 0 ? 3u : 2u, 0, 0);
                if (d == 0) ++draws;
                if (d == 2) ++twos;
            }
            (void)dueSkips;
            Require(draws >= 950 && draws <= 1010, "evenQ: a picture on every second turn whatever the backlog (2-3) and the lateness");
            Controller c0(false, Policy::Fixed30);
            now = 1000000;
            unsigned draws0 = 0;
            for (int i = 0; i < 2000; ++i) {
                now += i % 5 == 0 ? 30000 : 16683;
                if (c0.Turn(now, true, false, i % 3 == 0 ? 3u : 2u, 0, 0) == 0) ++draws0;
            }
            Require(draws0 < draws, "... and today's rule loses pictures to the same backlog");
            std::printf("unit checks: evenQ draws %u of 1000 due turns, today's rule %u (backlog 2-3 on every turn, a late turn in 5, %u catch-ups)\n", draws, draws0, twos);
        }
        std::printf("unit checks ok\n");
    }

    // ---- 1. the model against the console -------------------------------------------------------------------------------------------------------
    std::vector<Params> fitted(kRowCount);
    std::vector<char> usable(kRowCount, 0);
    {
        Header("1. the model against the console (legacy rule). GPU occupancy fitted per 5 s row to the console's pictures a second; holds / L / judder are the check");
        double errFps = 0, errH3 = 0, errH1 = 0, errL = 0, errJ = 0;
        unsigned n = 0;
        std::map<std::string, std::vector<size_t>> groups;
        for (size_t i = 0; i < kRowCount; ++i) {
            if (!Perf(kRows[i]) && quick) continue;
            fitted[i] = Fit(kRows[i]);
            usable[i] = 1;
            groups[std::string(kRows[i].run) + " " + kRows[i].stage].push_back(i);
        }
        std::printf("%-30s rows | console: fps  hold 1/2/3+     L  | model:   fps  hold 1/2/3+     L  judder(model) | gpu scale\n", "run stage");
        for (auto& g : groups) {
            double cp = 0, c1 = 0, c2 = 0, c3 = 0, cl = 0, cs = 0;
            Agg m;
            for (size_t i : g.second) {
                const Row& r = kRows[i];
                cp += r.pics; c1 += r.h1; c2 += r.h2; c3 += r.h3 + r.h4; cl += r.L * r.pics; cs += fitted[i].gscale;
                Params p = fitted[i];
                p.pol = Pol::Legacy;
                m.Add(Mean(p, 2));
            }
            const double ct = c1 + c2 + c3, nrows = static_cast<double>(g.second.size());
            const double cfps = cp / (5.0 * nrows), mfps = m.fps() / nrows * nrows / 2.0;  // (Mean adds 2 seeds)
            const double mf = m.pics / m.secs;
            (void)mfps;
            const double mn = m.pics ? m.pics : 1.0;
            std::printf("%-30s %3zu  |  %5.1f  %4.2f/%4.2f/%4.2f  %3.1f |  %5.1f  %4.2f/%4.2f/%4.2f  %3.1f  %6.3f        | %.2f\n", g.first.c_str(), g.second.size(), cfps, c1 / ct, c2 / ct, c3 / ct,
                        cl / cp, mf, m.hold[1] / mn, m.hold[2] / mn, (m.hold[3] + m.hold[4]) / mn, m.Lmean(), m.judder(), cs / nrows);
            errFps += std::abs(mf - cfps);
            errH3 += std::abs((m.hold[3] + m.hold[4]) / mn - c3 / ct);
            errH1 += std::abs(m.hold[1] / mn - c1 / ct);
            errL += std::abs(m.Lmean() - cl / cp);
            ++n;
        }
        (void)errJ;
        std::printf("mean |model - console| over %u run-stages: pictures a second %.2f, share of holds of 1 vblank %.3f, share of 3+ %.3f, present latency L %.2f\n", n, errFps / n, errH1 / n, errH3 / n, errL / n);
    }

    // ---- 2. policies on the fitted rows -----------------------------------------------------------------------------------------------------------
    {
        Header("2. Performance mode: every policy on the same fitted rows (2 seeds, 20 s per row), by what the console made of the stage");
        std::map<int, std::map<int, Agg>> byCat;  // category -> policy -> Agg
        std::map<std::string, std::map<int, Agg>> byStage;
        for (size_t i = 0; i < kRowCount; ++i) {
            if (!usable[i] || !Perf(kRows[i])) continue;
            const double fps = RowFps(kRows[i]);
            int cat = 4;
            for (int c = 0; c < 5; ++c) if (fps >= kCats[c].lo && fps < kCats[c].hi) { cat = c; break; }
            for (size_t pi = 0; pi < sizeof kPerf / sizeof kPerf[0]; ++pi) {
                Params p = fitted[i];
                p.pol = kPerf[pi];
                const Agg a = Mean(p, 2);
                byCat[cat][static_cast<int>(pi)].Add(a);
                byStage[kRows[i].stage][static_cast<int>(pi)].Add(a);
            }
        }
        for (int c = 0; c < 5; ++c) {
            if (!byCat.count(c)) continue;
            std::printf("\n-- stages %s (%g windows)\n", kCats[c].name, byCat[c][0].secs / 15.0 / 2.0);
            PrintHead();
            for (size_t pi = 0; pi < sizeof kPerf / sizeof kPerf[0]; ++pi) PrintAgg(PolName(kPerf[pi]), byCat[c][static_cast<int>(pi)]);
        }
        std::printf("\n-- per stage, K3 + RM tours: legacy | GXV_EVEN26=1 | GXV_EVEN26=1 + GXV_LATENCY24=1   (fps / ideal%% / missed per s / latency ms / score)\n");
        std::printf("%-26s | %-34s | %-34s | %-34s\n", "stage", "legacy", "even26", "even26 + latency24");
        Agg tot[3];
        for (auto& s : byStage) {
            const Agg& a = s.second[0];
            const Agg& b = s.second[4];
            const Agg& c = s.second[5];
            auto f = [](const Agg& x) {
                static char buf[8][64];
                static int k = 0;
                char* o = buf[k++ & 7];
                std::snprintf(o, 64, "%5.1f %5.1f %5.2f %6.1f %5.1f", x.fps(), x.idealPct(), x.missedPs(), x.latMs(), x.score());
                return o;
            };
            std::printf("%-26s | %-34s | %-34s | %-34s\n", s.first.c_str(), f(a), f(b), f(c));
            tot[0].Add(a); tot[1].Add(b); tot[2].Add(c);
        }
        PrintHead();
        PrintAgg("ALL stages: legacy", tot[0]);
        PrintAgg("ALL stages: even26", tot[1]);
        PrintAgg("ALL stages: even26 + latency24", tot[2]);
        // The claims the report makes, as checks (a model's claims, 2 seeds; thresholds are loose)
        Require(tot[1].fps() >= tot[0].fps() - 1.5, "even26 gives up no more than 1.5 pictures a second on average against the legacy rule");
        Require(tot[1].idealPct() > tot[0].idealPct() + 3.0, "even26 puts clearly more pictures on the ideal cadence");
        Require(tot[1].missedPs() < 0.5 * tot[0].missedPs() + 0.01, "even26 halves the missed moments");
        Require(tot[1].turnsPs() > 59.0, "even26 keeps the logic rate");
        // near 60 nothing may get worse
        const Agg& n60l = byCat[0][0];
        const Agg& n60e = byCat[0][4];
        if (n60l.pics) {
            Require(n60e.fps() >= n60l.fps() - 0.6, "at 60: no pictures lost");
            Require(n60e.latMs() <= n60l.latMs() + 3.0, "at 60: no latency added");
        }
    }

    // ---- 3. sweep -----------------------------------------------------------------------------------------------------------------------------
    if (!quick) {
        Header("3. sweep of the GPU's time a picture (drawn turn 15.5 ms, other 7.5, front end 13, back end cpu 12, 6 seeds, 40 s). fps / ideal % / missed per s / latency ms");
        std::printf("GPU ms refr | %-28s | %-28s | %-28s | %-28s | %-28s\n", "legacy", "legacy+maxstep2", "pace25", "even26", "even26+latency24");
        Agg worst[5];
        for (double g : {13.0, 15.0, 16.5, 17.5, 18.5, 19.5, 20.5, 21.5, 23.0, 25.0, 27.0, 29.0, 31.0, 33.0}) {
            Params p;
            p.d = 15.5; p.n = 7.5; p.fe = 13; p.be = 12; p.g = g;
            char cells[5][64];
            const Pol pols[5] = {Pol::Legacy, Pol::LegacyMs2, Pol::Pace25, Pol::Even26, Pol::Even26L24};
            for (int k = 0; k < 5; ++k) {
                p.pol = pols[k];
                const Agg a = Mean(p, 6);
                std::snprintf(cells[k], 64, "%5.1f %5.1f %5.2f %6.1f", a.fps(), a.idealPct(), a.missedPs(), a.latMs());
                if (g >= 17.0 && g <= 31.0) worst[k].Add(a);
            }
            std::printf("%5.1f %5.2f | %-28s | %-28s | %-28s | %-28s | %-28s\n", g, g / kV, cells[0], cells[1], cells[2], cells[3], cells[4]);
        }
        for (int k = 0; k < 5; ++k) std::printf("mean over GPU 17-31 ms: %-18s fps %5.1f ideal %5.1f%% missed/s %5.2f latency %6.1f ms judder %5.3f score %5.1f turns/s %5.1f\n",
            k == 0 ? "legacy" : k == 1 ? "maxstep2" : k == 2 ? "pace25" : k == 3 ? "even26" : "even26+latency24", worst[k].fps(), worst[k].idealPct(), worst[k].missedPs(), worst[k].latMs(), worst[k].judder(), worst[k].score(), worst[k].turnsPs());
        Require(worst[3].fps() >= worst[0].fps() - 1.5, "sweep: even26 loses under 1.5 pictures a second on average");
    }

    // ---- 4. Quality (Locked 30 rhythm) -------------------------------------------------------------------------------------------------------------
    {
        Header("4. Quality (locked 30): today's Fixed30 + slots, GXV_EVEN26_Q=4, and + GXV_EVEN26_QSLACK=1. A picture every second turn; ideal = hold 2 and step 2.");
        const Pol qp[] = {Pol::Fixed30, Pol::Fixed30Even, Pol::Fixed30EvenSlack};
        std::printf("%-12s | %-44s | %-44s | %-44s\n", "GPU ms", PolName(qp[0]), PolName(qp[1]), PolName(qp[2]));
        std::printf("%-12s | %-44s | %-44s | %-44s\n", "", "fps ideal% 1/2/3+% missed/s lat ms turns/s", "", "");
        Agg sum[3];
        for (double g : {14.0, 18.0, 22.0, 25.0, 28.0, 30.0, 32.0, 34.0, 38.0}) {
            Params p;
            p.d = 17.0; p.n = 8.0; p.fe = 14; p.be = 12; p.g = g;
            p.seconds = 40;
            char cells[3][96];
            for (int k = 0; k < 3; ++k) {
                p.pol = qp[k];
                const Agg a = Mean(p, 6);
                const double n = a.pics ? a.pics : 1;
                std::snprintf(cells[k], 96, "%5.1f %5.1f %3.0f/%3.0f/%3.0f %5.2f %6.1f %5.1f", a.fps(), a.idealPct(), 100 * a.hold[1] / n, 100 * a.hold[2] / n, 100 * (a.hold[3] + a.hold[4]) / n, a.missedPs(), a.latMs(), a.turnsPs());
                if (g <= 30.0) sum[k].Add(a);
            }
            std::printf("%-12.1f | %-44s | %-44s | %-44s\n", g, cells[0], cells[1], cells[2]);
        }
        for (int k = 0; k < 3; ++k) std::printf("GPU <= 30 ms mean: %-40s fps %5.1f ideal %5.1f%% missed/s %5.2f latency %6.1f ms turns/s %5.1f\n", PolName(qp[k]), sum[k].fps(), sum[k].idealPct(), sum[k].missedPs(), sum[k].latMs(), sum[k].turnsPs());
        Require(sum[1].idealPct() >= sum[0].idealPct(), "Quality: GXV_EVEN26_Q does not lower the share of 2-vblank pictures");
        Require(sum[1].turnsPs() > 58.5, "Quality: GXV_EVEN26_Q keeps the game within 2.5 % of its speed while the GPU fits 30");
        // the same on the real rows of the tours whose GPU fits 33 ms (model, the console never ran Quality on them in these logs)
        std::printf("\n-- tour rows whose fitted pipeline fits 30 a second (model fps of the steady 30 >= 29.5): the three Quality policies\n");
        Agg rs[3];
        unsigned rowsUsed = 0;
        for (size_t i = 0; i < kRowCount; ++i) {
            if (!usable[i] || !Perf(kRows[i])) continue;
            Params p = fitted[i];
            p.pol = Pol::Fixed30Even;
            const Agg probe = Simulate(p);
            if (probe.fps() < 29.0) continue;
            ++rowsUsed;
            for (int k = 0; k < 3; ++k) { p.pol = qp[k]; rs[k].Add(Mean(p, 2)); }
        }
        std::printf("rows used: %u\n", rowsUsed);
        PrintHead();
        for (int k = 0; k < 3; ++k) PrintAgg(PolName(qp[k]), rs[k]);
    }
    std::printf("\neven26_model: ok\n");
    return 0;
}
