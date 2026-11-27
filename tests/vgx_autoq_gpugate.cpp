// Host-side test of the GPU gate (Config::gpuGate, GXV_AUTOQ_GPUGATE; platform/vgx/src/vgx_autoq.{h,cpp}, s1117b / Strikers 1117b): no rule lowers the picture
// for the GPU's sake unless the GPU's own busy time over the last ~90 pictures is >= 85 % of their intervals (or >= 70 % with >= 8 % of them late).
// tools/test_vgx_autoq_gpugate.sh builds and runs it; tools/test_vgx_autoq_gpugate_mutants.sh breaks the rule one line at a time and expects a failure.
//  1. The switch: on in Config's defaults; off gives today's rules.
//  2. The thresholds, with synthetic pictures: 85 % open, 84 % closed, 70 % with 8 % late open, with 7 % late closed, 69 % closed; fewer than 6 measured
//     pictures open; unmeasured pictures (no busy time) never count as busy; the fence clock (no timers) leaves it open; a load clears it.
//  3. A CPU-limited scene whose "GPU time" is a wait (GPU 14.6 ms of a 50 ms picture; GPU 17.5 ms of a 33 ms picture): the rules without the gate step down,
//     with it nothing changes, in Performance and in Quality.
//  4. A real GPU overload (the GPU's time falls with the pixels): the trace with the gate is the trace without it, in Performance and in Quality.
//  5. A lockstep property over random traces: whenever the controller with the gate lowers anything (a size, a step, anisotropic filtering) from the same
//     state as the one without, the one without lowers at the same picture: the gate removes step downs, it never adds one.
#define private public   // (the property test copies the whole controller, hidden state included, into a twin with the gate off)
#include "../platform/vgx/src/vgx_autoq.h"
#undef private

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace vgx::autoq;

extern "C" int64_t port_thread_cpu_us(void) { return 100; }

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}
constexpr double kRetraceUs = 16683.0;

struct Event { double t; int from, to; Why why; };

// One picture as the controller is handed it. All times in ms.
struct Pic {
    double gpuMs = 10;        // the GPU's busy time (corrected, as the back end delivers it with GXV_PACE23)
    bool busyKnown = true;
    double gameTurnMs = 7;    // one game turn's CPU
    int turns = 1;            // game turns in the picture (retraces it spans, when nothing else holds it)
    double frontMs = 6, backendMs = 4;
    double otherWaitMs = 0;   // a wait the census cannot see (shader build, upload, pacing): added to the picture interval
    bool frontKnown = true;
};

// the whole controller state a step down changes
struct State { int rung; unsigned steps; bool aniso, edge; bool operator==(const State& o) const { return rung == o.rung && steps == o.steps && aniso == o.aniso && edge == o.edge; } };
State Of(const Controller& c) { return {c.Current(), c.Steps(), c.AnisoOn(), c.HoldEdgeOff()}; }
// 1: lowered something (a size down, a step on, anisotropic filtering off, the edge pass off), 0: not
bool Lowered(const State& a, const State& b) {
    return b.rung < a.rung || (b.steps & ~a.steps) != 0 || (a.aniso && !b.aniso) || (!a.edge && b.edge);
}
struct Sim {
    Controller ctl;
    uint64_t now = 1000000, backend = 1;
    uint32_t game = 1, turnsTotal = 1, front = 1, frontPictures = 1;
    double t = 0;
    std::vector<Event> ev;
    unsigned pictures = 0, retraces = 0;
    explicit Sim(const Config& c) : ctl(c), twin(c) {}
    Controller twin;
    bool twinOn = false;
    unsigned twinCompared = 0, twinLoweredOn = 0, twinLoweredOff = 0, twinOnlyGate = 0;
    // the interval: the retraces the largest stage needs, plus a wait
    void Frame(const Pic& p, bool modelGpu = false, double fixedGpu = 3, double pixGpu = 9) {
        const double gpu = modelGpu ? fixedGpu + pixGpu * Cost(ctl.Current()) : p.gpuMs;
        const double game1 = p.gameTurnMs * p.turns;
        const double work = std::max({game1, p.backendMs, gpu, p.frontMs});
        const unsigned n = std::max<unsigned>(p.turns, static_cast<unsigned>(std::ceil((work + p.otherWaitMs) * 1000.0 / kRetraceUs - 1e-9)));
        const uint64_t interval = static_cast<uint64_t>(n * kRetraceUs);
        game += static_cast<uint32_t>(game1 * 1000.0);
        turnsTotal += p.turns;
        backend += static_cast<uint32_t>(p.backendMs * 1000.0);
        front += static_cast<uint32_t>(p.frontMs * 1000.0);
        ++frontPictures;
        FrameTimes f;
        f.startUs = now;
        now += interval;
        f.presentUs = now;
        f.backendRunUs = backend;
        f.gameRunUs = game;
        f.gameTurns = turnsTotal;
        f.gameKnown = true;
        f.frontRunUs = front;
        f.frontPictures = frontPictures;
        f.frontKnown = p.frontKnown;
        f.timers = true;
        f.busyKnown = p.busyKnown;
        f.gpuBusyUs = p.busyKnown ? static_cast<uint32_t>(gpu * 1000.0) : 0u;
        ++pictures;
        retraces += n;
        const int before = ctl.Current();
        const State pre = Of(ctl);
        if (twinOn) {   // the same controller, every hidden counter as it is now, with the gate off: it is handed the same picture
            twin = ctl;
            twin.cfg_.gpuGate = false;
        }
        const bool changed = ctl.Add(f);
        if (twinOn) {
            twin.Add(f);
            ++twinCompared;
            const bool la = Lowered(pre, Of(ctl)), lb = Lowered(pre, Of(twin));
            twinLoweredOn += la;
            twinLoweredOff += lb;
            if (la && !lb) ++twinOnlyGate;
        }
        t += static_cast<double>(interval) / 1e6;
        if (changed || ctl.Current() != before) ev.push_back({t, before, ctl.Current(), ctl.LastChange().why});
    }
    // a load: a stall of `ms` and a new scene
    void Load(double ms, uint32_t scene) {
        FrameTimes f;
        f.startUs = now;
        now += static_cast<uint64_t>(ms * 1000.0);
        f.presentUs = now;
        f.backendRunUs = backend;
        f.gameRunUs = game;
        f.gameTurns = turnsTotal;
        f.gameKnown = true;
        f.frontRunUs = front;
        f.frontPictures = frontPictures;
        f.frontKnown = true;
        f.timers = true;
        f.scene = scene;
        const int before = ctl.Current();
        const bool changed = ctl.Add(f);
        t += ms / 1000.0;
        if (changed || ctl.Current() != before) ev.push_back({t, before, ctl.Current(), ctl.LastChange().why});
    }
    void Run(const Pic& p, double seconds, bool modelGpu = false, double fixedGpu = 3, double pixGpu = 9) {
        const double end = t + seconds;
        while (t < end) Frame(p, modelGpu, fixedGpu, pixGpu);
    }
    void Mark() { pictures = retraces = 0; }
    double Rate() const { return retraces ? 60.0 * pictures / retraces : 0.0; }
    unsigned Changes(double after = 0, double before = 1e9, bool downOnly = true) const {
        unsigned n = 0;
        for (const Event& e : ev) n += e.t >= after && e.t < before && e.why != Why::NewScene && (!downOnly || e.to < e.from);
        return n;
    }
    double First(double after) const {
        for (const Event& e : ev) if (e.t >= after && e.why != Why::NewScene && e.to < e.from) return e.t - after;
        return -1;
    }
    void Print(const char* title) const {
        std::printf("    %s:\n", title);
        for (const Event& e : ev) std::printf("      t=%7.2f s  rung %2d -> %2d (%s)\n", e.t, e.from, e.to, WhyText(e.why));
    }
};

// Performance mode as the Vita runs it (vgx_autoq_fast60.cpp Console()), with GXV_PACE23's fast60 numbers.
Config Perf(bool p23) {
    Config c;
    c.targetFps = 60;
    c.budgetMs = 1000.0 / 60.0;
    c.minRung = 0;
    c.maxRung = kTopSingle;
    c.startRung = kToday;
    c.recover = true;
    c.recoverHoldSec = 20.0;
    c.overload = c.overloadFx = true;
    c.freeFrac = 0.85;
    c.wipe = c.lateRoom = true;
    c.cadenceUp = true;
    c.newStage = true;
    c.bgStep = c.edgeStep = true;
    c.edgeCheck = true;
    c.fast60 = true;
    c.pace23 = p23;
    if (p23) { c.fast60Pics = 16; c.fast60Late = 12; c.fast60SpanSec = 1.6; c.fast60GapSec = 1.5; c.cadenceGpuScale = c.p23Scale; }
    return c;
}
Config Quality(bool p23) {
    Config c;
    c.targetFps = 30;
    c.budgetMs = 1000.0 / 30.0;
    c.minRung = 0;
    c.maxRung = 8;
    c.startRung = kTopSingle;
    c.recover = true;
    c.recoverHoldSec = 20.0;
    c.bgStep = true;
    c.edgeStep = true;
    c.freeFrac = 0.85;
    c.lateRoom = true;
    c.newStage = true;
    c.overload = c.overloadFx = true;
    c.hold30 = true;
    c.pace23 = p23;
    return c;
}
Pic Calm() { Pic p; p.gpuMs = 10; return p; }

// A controller fed pictures of a fixed interval and busy time, for the gate's thresholds. late: the picture's interval exceeds the budget + 3 %.
struct Feeder {
    Controller c;
    uint64_t now = 1000000, backend = 1;
    uint32_t game = 1, turns = 1;
    explicit Feeder(const Config& cfg) : c(cfg) {}
    void Pic1(double intervalMs, double busyMs, bool known = true, bool timers = true) {
        FrameTimes f;
        f.startUs = now;
        now += static_cast<uint64_t>(intervalMs * 1000.0);
        f.presentUs = now;
        backend += 2000;
        game += 3000;
        turns += 1;
        f.backendRunUs = backend;
        f.gameRunUs = game;
        f.gameTurns = turns;
        f.gameKnown = true;
        f.timers = timers;
        f.busyKnown = known;
        f.gpuBusyUs = known ? static_cast<uint32_t>(busyMs * 1000.0) : 0u;
        if (!timers) f.gpuDoneUs = now - 100;
        c.Add(f);
    }
    // n pictures at share `share` of a 16.68 ms interval, `lateN` of every 100 stretched to 40 ms (busy held at the same share of their own interval)
    void Mix(int n, double share, int lateEvery) {
        for (int i = 0; i < n; ++i) {
            const bool late = lateEvery > 0 && i % lateEvery == 0;
            Pic1(late ? 40.0 : 16.68, 16.68 * share);   // (the period is the budget at most: a late picture's busy time is its share of 16.68 ms)
        }
    }
};
Config Plain60() {
    Config c = Perf(false);
    return c;
}

} // namespace

int main(int argc, char** argv) {
    const bool verbose = argc > 1 && std::string(argv[1]) == "verbose";
    setenv("GXV_WIDE_RUNGS", "1", 1);
    char line[300];

    std::printf("the switch\n");
    {
        Check(Config().gpuGate, "Config's own defaults: on (a bug fix)");
        Check(Config().gateWindow == 90 && Config().gateBusy == 0.85 && Config().gateBusyLate == 0.70 && Config().gateLateFrac == 0.08 && Config().gateMinPics == 6,
              "the numbers: 90 pictures, 85 %, 70 % with 8 % late, 6 measured pictures");
        Config off = Perf(false);
        off.gpuGate = false;
        Feeder f(off);
        f.Mix(100, 0.30, 0);
        Check(f.c.GpuGateOpen(), "switched off: always open");
    }

    std::printf("the thresholds (pictures of 16.68 ms against the 60 budget)\n");
    {
        Feeder a(Plain60()); a.Mix(100, 0.851, 0);
        Check(a.c.GpuGateOpen(), "85 % busy, none late: open");
        Feeder b(Plain60()); b.Mix(100, 0.84, 0);
        Check(!b.c.GpuGateOpen(), "84 % busy, none late: closed");
        // 8 of 100 pictures late (every 12th: 9 of 100; use every 13th: 8 of 100) at 70 %
        Feeder c(Plain60()); c.Mix(100, 0.701, 12);
        Check(c.c.GpuGateOpen(), "70 % busy with 8 of 100 late: open");
        // (late pictures early in the window, none in the last 12: only the 90-picture mean can open it)
        Feeder d(Plain60()); d.Pic1(16.68, 0, false);
        for (int k = 0; k < 7; ++k) d.Pic1(40.0, 11.7);
        for (int k = 0; k < 82; ++k) d.Pic1(16.68, 11.7);
        Check(!d.c.GpuGateOpen(), "70 % busy with 7 of 90 late (7.8 %): closed");
        Feeder d2(Plain60()); d2.Pic1(16.68, 0, false);
        for (int k = 0; k < 8; ++k) d2.Pic1(40.0, 11.7);
        for (int k = 0; k < 81; ++k) d2.Pic1(16.68, 11.7);
        Check(d2.c.GpuGateOpen(), "70 % busy with 8 of 90 late (8.9 %): open");
        // the last 12 pictures alone: a GPU that has just become the limit opens the gate at once
        Feeder r(Plain60()); r.Mix(80, 0.30, 0); r.Mix(12, 0.90, 0);
        Check(r.c.GpuGateOpen(), "the last 12 pictures at 90 % after 80 at 30 %: open (the mean has not caught up)");
        Feeder r2(Plain60()); r2.Mix(80, 0.30, 0); r2.Mix(10, 0.90, 0);
        Check(!r2.c.GpuGateOpen(), "the last 10 pictures at 90 % (a mixed last 12 at 80 %) after 80 at 30 %: closed");
        Feeder e(Plain60()); e.Mix(100, 0.69, 12);
        Check(!e.c.GpuGateOpen(), "69 % busy with 8 of 100 late: closed");
        Feeder g(Plain60()); g.Mix(100, 0.95, 5);
        Check(g.c.GpuGateOpen(), "95 % busy with 20 % late: open");
        Feeder h(Plain60()); h.Mix(6, 0.20, 0);   // (the first picture has no interval: 5 measured)
        Check(h.c.GpuGateOpen(), "5 measured pictures at 20 %: too few, open (today's rules)");
        Feeder i2(Plain60()); i2.Mix(7, 0.20, 0);
        Check(!i2.c.GpuGateOpen(), "6 measured pictures at 20 %: closed");
        // 90 pictures window: 100 pictures of 20 % followed by 90 of 90 %: open (the old ones are out of the window)
        Feeder w(Plain60()); w.Mix(100, 0.20, 0); w.Mix(90, 0.90, 0);
        Check(w.c.GpuGateOpen(), "a window of 90 pictures: the pictures before it do not count");
        // unmeasured pictures never count as busy: 60 pictures with no busy time, 12 measured at 20 %
        Feeder u(Plain60());
        for (int k = 0; k < 60; ++k) u.Pic1(16.68, 0, false);
        for (int k = 0; k < 7; ++k) u.Pic1(16.68, 3.3);
        Check(!u.c.GpuGateOpen(), "pictures with no busy time do not count as busy (6 measured at 20 %: closed)");
        Feeder u2(Plain60());
        for (int k = 0; k < 60; ++k) u2.Pic1(16.68, 0, false);
        Check(u2.c.GpuGateOpen(), "no measured picture at all: open");
        // the fence clock: no timers: open whatever the pictures look like
        Feeder fe(Plain60());
        for (int k = 0; k < 100; ++k) fe.Pic1(16.68, 1.0, true, false);
        Check(fe.c.GpuGateOpen(), "the fence clock (no GPU timers): open");
        // a load clears the ring
        Feeder ld(Plain60()); ld.Mix(100, 0.20, 0);
        const bool closedBefore = !ld.c.GpuGateOpen();
        ld.Pic1(2500.0, 10.0);
        Check(closedBefore && ld.c.GpuGateOpen(), "a load (a gap over 1.5 s): the gate starts afresh");
        // a picture on the steady 30 is judged against 33.3 ms: not "late"
        Feeder cad(Plain60());
        for (int k = 0; k < 100; ++k) {
            FrameTimes f;
            f.startUs = cad.now; cad.now += 33400; f.presentUs = cad.now;
            cad.backend += 2000; cad.game += 3000; cad.turns += 1;
            f.backendRunUs = cad.backend; f.gameRunUs = cad.game; f.gameTurns = cad.turns; f.gameKnown = true;
            f.timers = true; f.busyKnown = true; f.gpuBusyUs = 23500; f.cadence30 = true;   // 70 % of 33.4 ms, none late on the cadence
            cad.c.Add(f);
        }
        Check(!cad.c.GpuGateOpen(), "70 % busy on the steady 30, no picture late against 33.3 ms: closed");
    }

    std::printf("a CPU-limited scene: after a load the pictures come late for waits, the GPU timers deliver one picture in eight (GPU 10 ms), every other stage fits\n");
    {
        // (the 2.2 rules blame the late pictures on the GPU by elimination, the same scene as tests/vgx_autoq_pace23.cpp's)
        Pic p = Calm();
        p.otherWaitMs = 12;
        unsigned down[2] = {0, 0};
        for (int g = 0; g <= 1; ++g) {
            Config c = Perf(false);
            c.gpuGate = g != 0;
            Sim s(c);
            s.Run(Calm(), 10);
            s.Load(900, 7);
            const double t0 = s.t;
            for (int i = 0; i < 700; ++i) { p.busyKnown = i % 8 == 0; s.Frame(p); }
            down[g] = s.Changes(t0);
            if (verbose) s.Print(g ? "after a load, gate on" : "after a load, gate off");
            if (g) { std::snprintf(line, sizeof line, "gate on: closed for %u pictures", s.ctl.GateClosedPictures()); Check(s.ctl.GateClosedPictures() > 500, line); }
        }
        std::snprintf(line, sizeof line, "Performance: without the gate %u size steps down, with it %u", down[0], down[1]);
        Check(down[0] >= 1 && down[1] == 0, line);
    }
    {
        // the same scene with every timer delivered (GPU 10 ms of a 28 ms picture): the census already blames no GPU; the gate changes nothing and is closed
        Pic p = Calm();
        p.otherWaitMs = 12;
        unsigned down[2] = {0, 0};
        for (int g = 0; g <= 1; ++g) {
            Config c = Perf(false);
            c.gpuGate = g != 0;
            Sim s(c);
            s.Run(Calm(), 10);
            const double t0 = s.t;
            s.Run(p, 30);
            down[g] = s.Changes(t0);
        }
        std::snprintf(line, sizeof line, "Performance, every timer delivered: %u without the gate, %u with it (never more)", down[0], down[1]);
        Check(down[1] <= down[0], line);
    }

    std::printf("Quality (target 30, the fixed cap): a front end or a wait of ~30 ms a picture, the GPU timers delivering one picture in four (GPU 10 ms)\n");
    {
        // the late pictures are blamed on the GPU by elimination (the timer says nothing, the game thread and the renderer fit): without the gate the size
        // walks from 960x544 down to the smallest rung for a picture rate that no smaller picture can raise
        Pic p = Calm();
        p.otherWaitMs = 30;
        unsigned down[2] = {0, 0};
        int rung[2] = {0, 0};
        for (int g = 0; g <= 1; ++g) {
            Config c = Quality(false);
            c.gpuGate = g != 0;
            Sim s(c);
            s.Run(Calm(), 10);
            s.Load(900, 7);
            const double t0 = s.t;
            for (int i = 0; i < 2500; ++i) { p.busyKnown = i % 4 == 0; s.Frame(p); }
            down[g] = s.Changes(t0);
            rung[g] = s.ctl.Current();
            if (verbose) s.Print(g ? "Quality, a wait, gate on" : "Quality, a wait, gate off");
            if (g) { std::snprintf(line, sizeof line, "gate on: closed for %u pictures, %u verdicts stopped", s.ctl.GateClosedPictures(), s.ctl.GateVetoes()); Check(s.ctl.GateClosedPictures() > 2000 && s.ctl.GateVetoes() > 0, line); }
        }
        std::snprintf(line, sizeof line, "Quality: without the gate %u size steps down (rung %d), with it %u (rung %d)", down[0], rung[0], down[1], rung[1]);
        Check(down[0] >= 3 && down[1] == 0 && rung[1] == kTopSingle, line);
    }

    {
        // the same at today's size (640x456 in the wide ladder 720x404): the steps below today's are the `bound` rules'
        unsigned down[2] = {0, 0};
        for (int g = 0; g <= 1; ++g) {
            Config c = Quality(false);
            c.startRung = kToday;
            c.gpuGate = g != 0;
            Sim s(c);
            Pic p = Calm();
            p.otherWaitMs = 30;
            s.Run(Calm(), 10);
            s.Load(900, 7);
            const double t0 = s.t;
            for (int i = 0; i < 2500; ++i) { p.busyKnown = i % 4 == 0; s.Frame(p); }
            down[g] = s.Changes(t0);
        }
        std::snprintf(line, sizeof line, "Quality starting at today's size: without the gate %u size steps down, with it %u", down[0], down[1]);
        Check(down[0] >= 2 && down[1] == 0, line);
    }

    std::printf("a step held for a quiet moment is not taken once the gate has closed\n");
    {
        for (int closed = 0; closed <= 1; ++closed) {
            Config c = Perf(false);
            Feeder f(c);
            f.Mix(100, closed ? 0.30 : 0.95, 0);
            Controller& k = f.c;
            const int rung0 = k.Current();
            k.pendingBelow_ = true;            // (a window had found the GPU the limit at today's size and waits for a pause or a fade)
            k.belowAt_ = f.now;
            k.belowBefore_ = 16.0;
            k.belowInterval_ = 17.0;
            FrameTimes q;
            q.startUs = f.now; f.now += 16683; q.presentUs = f.now;
            q.backendRunUs = f.backend; q.gameRunUs = f.game; q.gameTurns = f.turns; q.gameKnown = true;
            q.timers = true; q.quiet = true;   // a pause menu
            k.Add(q);
            std::snprintf(line, sizeof line, "gate %s at the quiet moment: rung %d -> %d", closed ? "closed" : "open", rung0, k.Current());
            Check(closed ? k.Current() == rung0 : k.Current() == rung0 - 1, line);
        }
    }

    std::printf("a real GPU overload: the trace with the gate is the trace without it\n");
    {
        for (int mode = 0; mode < 2; ++mode) {
            std::vector<Event> t[2];
            unsigned n[2] = {0, 0};
            for (int g = 0; g <= 1; ++g) {
                Config c = mode == 0 ? Perf(false) : Quality(false);
                c.gpuGate = g != 0;
                Sim s(c);
                s.Run(Calm(), 12, true, 3, 7);
                const double t0 = s.t;
                s.Run(Calm(), 30, true, 3, mode == 0 ? 14 : 30);   // 60: 17 ms at 720x404; 30: ~33 ms at the start size
                n[g] = s.Changes(t0);
                t[g] = s.ev;
            }
            bool same = t[0].size() == t[1].size();
            for (size_t i = 0; same && i < t[0].size(); ++i)
                same = t[0][i].from == t[1][i].from && t[0][i].to == t[1][i].to && t[0][i].why == t[1][i].why && std::fabs(t[0][i].t - t[1][i].t) < 1e-9;
            std::snprintf(line, sizeof line, "%s: %u size steps down without the gate, %u with it, same trace (%zu events)", mode == 0 ? "Performance" : "Quality", n[0], n[1], t[0].size());
            Check(n[0] >= 1 && same, line);
        }
    }

    std::printf("a property over random traces: from any state, the gate only removes step downs\n");
    {
        // Each picture goes to the controller with the gate and to a copy of it (every hidden counter, hold and block included) with the gate off.
        // A step down (a smaller size, a step on, anisotropic filtering or the edge pass off) by the first that the second does not take at the same
        // picture would be a step down the gate added. The trajectory is the gate's own, so every state the gate leads to is compared.
        unsigned long seed = 12345;
        auto rnd = [&]() { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return static_cast<double>((seed >> 33) & 0xFFFFF) / 1048576.0; };
        unsigned compared = 0, loweredOn = 0, loweredOff = 0, onlyGate = 0, gateClosed = 0;
        for (int run = 0; run < 24; ++run) {
            const bool perf = run % 2 == 0;
            Config a = perf ? Perf(false) : Quality(false);
            a.gpuGate = true;
            Sim s2(a);
            s2.twinOn = true;
            // random phases of 3-12 s: GPU 4-30 ms, game 4-16 ms (1-3 turns), waits 0-14 ms, timers delivering one picture in 1-8
            for (int ph = 0; ph < 14; ++ph) {
                Pic p;
                p.gpuMs = 4 + 26 * rnd();
                p.gameTurnMs = 4 + 12 * rnd();
                p.turns = 1 + static_cast<int>(rnd() * 2.99);
                p.frontMs = 3 + 10 * rnd();
                p.backendMs = 2 + 8 * rnd();
                p.otherWaitMs = rnd() < 0.3 ? 14 * rnd() : 0;
                const int every = rnd() < 0.3 ? 1 + static_cast<int>(rnd() * 7.99) : 1;
                const double secs = 3 + 9 * rnd();
                const bool modelGpu = rnd() < 0.4;
                const double fixedGpu = 3 + 4 * rnd(), pixGpu = 4 + 18 * rnd();
                const double end = s2.t + secs;
                int i = 0;
                while (s2.t < end) {
                    p.busyKnown = i++ % every == 0;
                    s2.Frame(p, modelGpu, fixedGpu, pixGpu);
                }
                if (rnd() < 0.25) s2.Load(600 + 2000 * rnd(), 1 + static_cast<uint32_t>(rnd() * 3));
            }
            compared += s2.twinCompared;
            loweredOn += s2.twinLoweredOn;
            loweredOff += s2.twinLoweredOff;
            onlyGate += s2.twinOnlyGate;
            gateClosed += s2.ctl.GateClosedPictures();
        }
        std::snprintf(line, sizeof line, "%u pictures compared: the gate lowered %u times, the copy without it %u; %u times only the gate lowered; the gate was closed for %u pictures",
                      compared, loweredOn, loweredOff, onlyGate, gateClosed);
        Check(compared > 20000 && loweredOff >= 5 && gateClosed > 1000 && onlyGate == 0, line);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "all checks passed", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
