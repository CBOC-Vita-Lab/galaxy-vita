// Host-side test of GXV_PACE23 (Config::pace23) in the automatic quality controller (platform/vgx/src/vgx_autoq.cpp). tools/test_vgx_autoq_pace23.sh
// builds and runs it. Each scene runs with the switch off (the 2.2 rules) and on, and the traces are told apart:
//  1. The switch: off in Config's defaults, off for no target; on for target 30 and 60.
//  2. After a load (the 2.2 crash log): the GPU timers deliver a busy time for one picture in eight, the pictures are late for waits, the other stages
//     fit. Off: the burst rule blames the GPU by elimination and takes sizes down. On: no change.
//  3. A CPU-paced picture: the GPU's busy time reaches the share of the budget but is a small part of a long picture interval (3 turns a picture).
//     Off: blamed on the GPU, steps down. On: none.
//  4. A real GPU overload: one direct step to the size the GPU's time says (not two or three), the pictures back on 60.
//  5. The settle window: a real GPU overload that begins at a load changes nothing for the settle time, then acts.
//  6. The change limit: a scene that flips between overload and calm makes at most p23MaxChanges size changes in the window, none closer than the gap.
//  7. The game thread's span CPU: a picture of 3 turns filled by them is the game's (census), not the GPU's.
//  8. Quality (target 30, hold30): a picture rate below 30 that is the game thread's or a CPU-paced GPU's never takes the size away from 960x544; a real GPU
//     miss still takes the edge smoothing, then the size.
//  9. Performance mode's reach: a floor rung that the model (only 60 % of the GPU's time falls with the pixels) cannot bring to 55 a second: no size step.
#include "../platform/vgx/src/vgx_autoq.h"

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

struct Sim {
    Controller ctl;
    uint64_t now = 1000000, backend = 1;
    uint32_t game = 1, turnsTotal = 1, front = 1, frontPictures = 1;
    double t = 0;
    std::vector<Event> ev;
    unsigned pictures = 0, retraces = 0;
    explicit Sim(const Config& c) : ctl(c) {}
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
        const bool changed = ctl.Add(f);
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
    c.gpuGate = false;   // (s1117b: this test sets GXV_PACE23 against the rules without the GPU gate, tests/vgx_autoq_gpugate.cpp has the gate's own)
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
    c.gpuGate = false;   // (s1117b: this test sets GXV_PACE23 against the rules without the GPU gate, tests/vgx_autoq_gpugate.cpp has the gate's own)
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
} // namespace

int main(int argc, char** argv) {
    const bool verbose = argc > 1 && std::string(argv[1]) == "verbose";
    setenv("GXV_WIDE_RUNGS", "1", 1);
    char line[300];

    std::printf("the switch\n");
    {
        Check(!Config().pace23, "Config's own defaults: off");
        Config z = Perf(true);
        z.targetFps = 0;
        Check(!Controller(z).Cfg().pace23, "no target: forced off");
        Check(Controller(Perf(true)).Cfg().pace23 && Controller(Quality(true)).Cfg().pace23, "target 60 and target 30: on when asked");
        Config e = Perf(true);
        e.p23GpuMiss = 5; e.p23Scale = 9; e.p23MaxChanges = 99;
        const Controller c(e);
        Check(c.Cfg().p23GpuMiss <= 1.2 && c.Cfg().p23Scale <= 1.0 && c.Cfg().p23MaxChanges <= 8, "the numbers are clamped");
    }

    std::printf("after a load: the timers deliver 1 picture in 8, the pictures are late for waits, every other stage fits\n");
    {
        Pic p = Calm();
        p.otherWaitMs = 12;       // 28 ms pictures: a shader build, uploads, the pacing; game 7, renderer 4, front end 6, GPU 10
        unsigned off = 0, on = 0;
        for (int p23 = 0; p23 <= 1; ++p23) {
            Sim s(Perf(p23 != 0));
            s.Run(Calm(), 10);
            s.Load(900, 7);
            const double t0 = s.t;
            for (int i = 0; i < 700; ++i) { p.busyKnown = i % 8 == 0; s.Frame(p); }
            if (verbose) s.Print(p23 ? "after a load, switch on" : "after a load, switch off");
            (p23 ? on : off) = s.Changes(t0);
            std::snprintf(line, sizeof line, "switch %s: %u size steps down in %.0f s after the load, %u pictures not blamed by elimination", p23 ? "on" : "off",
                          s.Changes(t0), s.t - t0, s.ctl.Pace23().noElim);
        }
        std::snprintf(line, sizeof line, "off %u size steps (the 2.2 behaviour), on %u", off, on);
        Check(off >= 1, "the 2.2 rules do step down here (the scene means something)");
        Check(on == 0, line);
    }

    std::printf("a CPU-paced picture: GPU busy 14.6 ms of a 50 ms picture (3 turns of 12 ms), the GPU the largest per-picture stage\n");
    {
        Pic p;
        p.gpuMs = 14.6; p.gameTurnMs = 12; p.turns = 3; p.frontMs = 9; p.backendMs = 6;
        unsigned off = 0, on = 0;
        for (int p23 = 0; p23 <= 1; ++p23) {
            Sim s(Perf(p23 != 0));
            s.Run(Calm(), 10);
            const double t0 = s.t;
            s.Run(p, 40);
            if (verbose) s.Print(p23 ? "CPU-paced, on" : "CPU-paced, off");
            (p23 ? on : off) = s.Changes(t0);
            if (p23) {
                std::snprintf(line, sizeof line, "util %.0f %% of the interval; %u late pictures not blamed on the GPU (CPU-paced), %u by the game thread's span", s.ctl.Pace23().util * 100.0,
                              s.ctl.Pace23().lowUtil, s.ctl.Pace23().gameSpan);
                Check(s.ctl.Pace23().util > 0 && s.ctl.Pace23().util < 0.5, line);
            }
        }
        std::snprintf(line, sizeof line, "off: %u size steps, on: %u", off, on);
        Check(off >= 1 && on == 0, line);
    }

    std::printf("a real GPU overload at 720x404: GPU 3 ms + 12.5 ms of pixels (16 -> ~19 ms), the game 7 ms: one direct step, back on 60\n");
    {
        // 3 + 14 * cost(2) = 17 ms at 720x404; at the floor 3 + 14 * 0.727 = 13.2 ms. The model: P23Pred(17, 2, 0) = 17 * (0.4 + 0.6 * 0.727) = 14.2 ms, fit 15.3 ms: the floor
        for (int p23 = 0; p23 <= 1; ++p23) {
            Sim s(Perf(p23 != 0));
            s.Run(Calm(), 12, true, 3, 7);
            const double t0 = s.t;
            Pic p = Calm();
            s.Run(p, 14, true, 3, 14);
            const unsigned n = s.Changes(t0);
            s.Mark();
            s.Run(p, 20, true, 3, 14);
            if (verbose) s.Print(p23 ? "overload, on" : "overload, off");
            std::snprintf(line, sizeof line, "switch %s: %u size step(s) down, first after %.2f s, rung %d, %.1f pictures a second after", p23 ? "on" : "off", n, s.First(t0),
                          s.ctl.Current(), s.Rate());
            if (p23) Check(n >= 1 && n <= 2 && s.Rate() >= 57.0 && s.First(t0) >= 0 && s.First(t0) <= 4.0, line);
            else Check(n >= 1, line);
            if (p23) Check(s.ctl.Pace23().direct <= n, "direct steps are counted");
        }
    }
    {
        // a heavier scene: 25 ms at 720x404 -> the floor 25 * 0.836 = 20.9 ms: the model says 60 is out of reach by size alone (55 needs 18.2 ms): no size step
        Sim s(Perf(true));
        s.Run(Calm(), 12, true, 3, 7);
        const double t0 = s.t;
        s.Run(Calm(), 30, true, 3, 22);
        if (verbose) s.Print("floor out of reach");
        unsigned fast = 0;
        for (const Event& e : s.ev) fast += e.t >= t0 && e.why == Why::FastDown;
        std::snprintf(line, sizeof line, "GPU 25 ms at 720x404, 20.9 ms predicted at the floor: %u fast size steps (the steady 30 is the way; the slow rules still judge by the GPU's time)", fast);
        Check(fast == 0, line);
    }

    std::printf("the settle window: a real overload that begins with a load changes nothing for the settle time\n");
    {
        Sim s(Perf(true));
        s.Run(Calm(), 12, true, 3, 7);
        s.Load(900, 5);
        const double t0 = s.t;
        s.Run(Calm(), 20, true, 3, 14);
        if (verbose) s.Print("overload from a load, on");
        const double first = s.First(t0);
        std::snprintf(line, sizeof line, "first size step %.2f s after the load (settle %.1f s)", first, s.ctl.Cfg().p23SettleSec);
        Check(first >= s.ctl.Cfg().p23SettleSec - 0.01 && first < 14.0, line);
        Check(s.ctl.Pace23().settleHeld > 0, "pictures were judged inside the window and counted");
    }

    std::printf("the change limit: a scene whose GPU time swings over and under the budget every 10 s, for 4 minutes\n");
    {
        unsigned counts[2] = {0, 0};
        for (int p23 = 0; p23 <= 1; ++p23) {
            Sim s(Perf(p23 != 0));
            s.Run(Calm(), 12, true, 3, 7);
            const double t0 = s.t;
            for (int i = 0; i < 12; ++i) { s.Run(Calm(), 10, true, 3, 14.5); s.Run(Calm(), 10, true, 3, 6); }
            if (verbose) s.Print(p23 ? "flipping scene, on" : "flipping scene, off");
            std::vector<double> ts;
            for (const Event& e : s.ev) if (e.t >= t0 && e.why != Why::NewScene) ts.push_back(e.t);
            counts[p23] = static_cast<unsigned>(ts.size());
            if (!p23) continue;
            // no more than p23MaxChanges in any p23WindowSec, none closer than the gap (a new scene's own rung is not counted)
            bool gapOk = true, windowOk = true;
            for (size_t i = 0; i < ts.size(); ++i) {
                if (i && ts[i] - ts[i - 1] < s.ctl.Cfg().p23GapSec - 0.5) gapOk = false;
                unsigned in = 0;
                for (size_t j = 0; j < ts.size(); ++j) in += ts[j] >= ts[i] && ts[j] < ts[i] + s.ctl.Cfg().p23WindowSec;
                if (in > static_cast<unsigned>(s.ctl.Cfg().p23MaxChanges)) windowOk = false;
            }
            std::snprintf(line, sizeof line, "%zu size changes in %.0f s (switch off: %u); gaps >= %.1f s: %s; at most %d in any %.0f s: %s", ts.size(), s.t - t0, counts[0],
                          s.ctl.Cfg().p23GapSec, gapOk ? "yes" : "NO", s.ctl.Cfg().p23MaxChanges, s.ctl.Cfg().p23WindowSec, windowOk ? "yes" : "NO");
            Check(gapOk && windowOk && counts[1] <= counts[0], line);
        }
    }

    std::printf("the game thread's span: 3-turn pictures filled by turns of 15 ms (census)\n");
    {
        Pic p;
        p.gpuMs = 15; p.gameTurnMs = 15.5; p.turns = 3; p.frontMs = 9; p.backendMs = 6;
        for (int p23 = 0; p23 <= 1; ++p23) {
            Sim s(Perf(p23 != 0));
            s.Run(Calm(), 6);
            (void)s.ctl.TakeFloor();
            s.Run(p, 10);
            const FloorWindow w = s.ctl.TakeFloor();
            std::snprintf(line, sizeof line, "switch %s: of %u late pictures, game %u (by span %u), GPU %u, none of these %u", p23 ? "on" : "off", w.late, w.game, w.p23GameSpan, w.gpu, w.other);
            if (p23) Check(w.late > 0 && w.game == w.late && w.p23GameSpan == w.late && w.gpu == 0 && w.other == 0, line);
            else Check(w.late > 0 && w.p23GameSpan == 0, line);
        }
    }

    std::printf("Quality (target 30): 960x544 is not left for the CPU\n");
    {
        // 50 ms pictures (3 retraces) from the game thread (2 turns of 20 ms... a picture of 3 turns), the GPU's busy time 31 ms of them (waits counted): util 62 %
        Pic p;
        p.gpuMs = 31; p.gameTurnMs = 14; p.turns = 3; p.frontMs = 12; p.backendMs = 9;
        unsigned off = 0, on = 0;
        for (int p23 = 0; p23 <= 1; ++p23) {
            Sim s(Quality(p23 != 0));
            Pic ok = Calm(); ok.gpuMs = 18; ok.gameTurnMs = 7; ok.turns = 2;
            s.Run(ok, 10);
            const double t0 = s.t;
            s.Run(p, 60);
            if (verbose) s.Print(p23 ? "Quality, CPU-paced, on" : "Quality, CPU-paced, off");
            (p23 ? on : off) = s.Changes(t0);
            std::snprintf(line, sizeof line, "switch %s: %u size steps down, rung %d (6 = 960x544)", p23 ? "on" : "off", s.Changes(t0), s.ctl.Current());
            if (p23) Check(s.ctl.Current() == kTopSingle && s.Changes(t0) == 0, line);
        }
        std::snprintf(line, sizeof line, "off: %u size steps, on: %u", off, on);
        Check(off >= 1 && on == 0, line);
        // a real miss: GPU 40 ms for 33 ms pictures stays the GPU's: the edge smoothing, then the size
        Sim s(Quality(true));
        Pic ok = Calm(); ok.gpuMs = 18; ok.gameTurnMs = 7; ok.turns = 2;
        s.Run(ok, 10);
        const double t0 = s.t;
        Pic real; real.gpuMs = 40; real.gameTurnMs = 7; real.turns = 2; real.frontMs = 8; real.backendMs = 6;
        s.Run(real, 40);
        if (verbose) s.Print("Quality, a real GPU miss, on");
        std::snprintf(line, sizeof line, "a real GPU miss (40 ms): the edge smoothing off=%d, %u size step(s) down, rung %d", s.ctl.EdgeStepOn() ? 1 : 0, s.Changes(t0), s.ctl.Current());
        Check(s.ctl.EdgeStepOn() && s.Changes(t0) >= 1, line);
    }

    std::printf("all checks done: %d failed\n", g_failures);
    if (g_failures == 0) std::printf("all checks passed (0 failed)\n");
    return g_failures ? 1 : 0;
}
