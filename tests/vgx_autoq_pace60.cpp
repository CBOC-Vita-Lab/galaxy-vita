// Host-side test of GXV_PACE60 (Config::p60) in the automatic quality controller (platform/vgx/src/vgx_autoq.cpp), closed loop with the
// pacing's steady 30 (port/frame_pace.h SteadyCadence, fed the controller's stage hint as port/perf.cpp does). tools/test_vgx_autoq_pace60.sh
// builds and runs it. The GPU of each scene is a TA / 3D model (TA the same at any size, the EFB 3D falling with the pixels by a TRUE share
// that differs from the controller's assumed one, the display scene's 3D with / without edge smoothing, an overlap), the CPU stages fixed;
// pictures come at whole retraces (the skip rule: n retraces for the slowest stage; under the steady 30 at least 2).
//  1. The switch: off by default, off for target 30, on for target 60.
//  2. The model: from a split at 720x404 the prediction at 608x348 with every step, against the true GPU; the display scene's wait trimmed;
//     the prediction carried across a change of size and of the edge step.
//  3. Honeyhive (console split TA 12.7 / 3D 18.9 / busy 22.2, the front end 18.2 ms): the steady 30 at 720x404, no size step, edge on.
//  4. Honeyhive's GPU with the CPU fitting 60: the model says 60 out of reach at 608 with every step: the steady 30 at 720x404 at once, no
//     chase (old rules: chase down the sizes / steps first).
//  5. Good Egg (GPU-limited at 17-18 ms at 720x404, TA small): reachable: steps / sizes down, 60 a second, no steady 30.
//  6. Beach Bowl (water: TA large, sizes save 4-6 %): out of reach: the steady 30 at 720x404.
//  7. Borderline, a GPU that swings around the threshold for 10 minutes: few verdicts, few changes (hysteresis), no flapping of the steady 30.
//  8. A heavy stretch (GPU over 33 ms at 720x404) while 60 is out of reach: still steps down on the steady 30 (30 must hold).
//  9. With the upscaler (display 5.5 ms with it, 2.3 without): the edge step's cost is modelled (~3 ms), the prediction follows.
#include "../platform/vgx/src/vgx_autoq.h"
#include "../platform/include/port/frame_pace.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace vgx::autoq;

extern "C" int64_t port_thread_cpu_us(void) { return 100; }
extern "C" int port_autoq_stages(uint32_t* gpuUs, uint32_t* pipeUs, int* lowerable);

namespace {
int g_failures = 0;
bool g_verbose = false;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}
constexpr double kR = 16683.0 / 1000.0; // a retrace, ms

struct Scene {
    double ta = 7, efb720 = 9, dispOn = 4.5, dispOff = 2.3, k = 0.3, share = 0.6; // GPU (ms at 720x404, rung 2); share: TRUE pixel share
    double bg = 0.95;                                                            // the cheaper background's true factor on the EFB 3D
    double gameDraw = 12, gameIdle = 8, front = 10, backend = 9;                  // CPU per turn / picture
    double dispWaitEvery = 0, dispWaitMs = 0;                                     // a display wait in one picture of N (0: none)
    double swingAmp = 0, swingSec = 0;                                            // the EFB 3D swings by +- this share with this period
};

uint32_t g_rng = 12345;
double Noise(double amp) { g_rng = g_rng * 1664525u + 1013904223u; return 1.0 + amp * ((g_rng >> 8) / 16777216.0 * 2.0 - 1.0); }

struct Gpu { double ta, efb, disp, busy; };
Gpu GpuAt(const Scene& s, int rung, unsigned steps, double t) {
    Gpu g;
    double swing = s.swingSec > 0 ? 1.0 + s.swingAmp * std::sin(6.283185307 * t / s.swingSec) : 1.0;
    g.ta = s.ta * Noise(0.03);
    g.efb = s.efb720 * swing * ((1.0 - s.share) + s.share * Cost(rung) / Cost(kToday)) * ((steps & 2u) ? s.bg : 1.0) * Noise(0.04);
    g.disp = ((steps & 4u) ? s.dispOff : s.dispOn) * Noise(0.02);
    const double f = g.efb + g.disp;
    g.busy = std::max(g.ta, f) + s.k * std::min(g.ta, f);
    return g;
}

struct Sim {
    Controller ctl;
    port::pace::SteadyCadence cad;
    bool steadyOn;
    uint64_t now = 1000000, backend = 1;
    uint32_t game = 1, turns = 1, front = 1, frontPics = 1;
    double t = 0, tMs_ = 0, gDone_ = 0;
    unsigned pics = 0, rets = 0, picNo = 0;
    unsigned rungChanges = 0, downChanges = 0, stepChanges = 0;
    int lastRung;
    unsigned lastSteps = 0;
    std::vector<std::string> log;
    Sim(const Config& c, bool steady, bool p23 = false) : ctl(c), steadyOn(steady), lastRung(c.startRung) {
        port::pace::SteadyCadenceParams p;
        if (p23 || c.p60) { // (perf.cpp: GXV_PACE23 or GXV_PACE60)
            p.belowFps = port::pace::kPace23Steady30BelowFps;
            p.aboveFps = port::pace::kPace23Steady30AboveFps;
            p.engageSec = 2.5;
            p.releaseSec = 3.0;
        }
        cad.Configure(steady, p);
    }
    void Picture(const Scene& s) {
        const int rung = ctl.Current();
        const unsigned steps = ctl.Steps();
        const Gpu g = GpuAt(s, rung, steps, t);
        // retraces this picture takes: the CPU stages (the game's drawn turn plus n-1 idle ones within n retraces, the front end, the renderer),
        // and the GPU, which runs behind the CPU and finishes pictures at its own pace (a GPU at 22 ms gives a mix of 1 and 2 retraces, ~45 a
        // second, as the console's "Up to 60" does); under the steady 30 at least 2
        unsigned n = 1;
        while (n < 8 && (std::max(s.front, s.backend) > n * kR || s.gameDraw + (n - 1) * s.gameIdle > n * kR)) ++n;
        gDone_ = std::max(gDone_, tMs_ - kR) + g.busy;
        while (n < 8 && tMs_ + n * kR < gDone_ - 0.01) ++n;
        if (cad.Engaged() && n < 2) n = 2;
        // the steady 30 (as perf.cpp: the stage hint, then the turn's offer)
        for (unsigned i = 0; i < n; ++i) {
            uint32_t gu = 0, pu = 0;
            int low = 0;
            const bool known = port_autoq_stages(&gu, &pu, &low) != 0;
            cad.Stages(known, gu, pu, low != 0);
            const uint64_t ts = static_cast<uint64_t>((tMs_ + i * kR) * 1000.0) + 1000000;
            cad.Begin(ts, true, false);
            const bool drawn = i == 0;
            cad.Decision(true, drawn);
            cad.End(ts + static_cast<uint64_t>((drawn ? s.gameDraw : s.gameIdle) * 1000.0));
        }
        tMs_ += n * kR;
        const uint64_t interval = static_cast<uint64_t>(n * kR * 1000.0);
        game += static_cast<uint32_t>((s.gameDraw + (n - 1) * s.gameIdle) * 1000.0);
        turns += n;
        backend += static_cast<uint32_t>(s.backend * 1000.0);
        front += static_cast<uint32_t>(s.front * 1000.0);
        ++frontPics;
        FrameTimes f;
        f.startUs = now;
        now += interval;
        f.presentUs = now;
        f.backendRunUs = backend;
        f.gameRunUs = game;
        f.gameTurns = turns;
        f.gameKnown = true;
        f.frontRunUs = front;
        f.frontPictures = frontPics;
        f.frontKnown = true;
        f.cadence30 = cad.Engaged();
        f.timers = true;
        f.busyKnown = true;
        f.gpuBusyUs = static_cast<uint32_t>(g.busy * 1000.0);
        ctl.Add(f);
        // the scene timers' split of the picture (a display wait now and then, as on a CPU-paced picture)
        double disp = g.disp, busy = g.busy;
        if (s.dispWaitEvery > 0 && (++picNo % static_cast<unsigned>(s.dispWaitEvery)) == 0) { disp += s.dispWaitMs; busy += s.dispWaitMs; }
        ctl.AddSplit(static_cast<uint32_t>(g.ta * 1000.0), static_cast<uint32_t>(g.efb * 1000.0), static_cast<uint32_t>(disp * 1000.0),
                     static_cast<uint32_t>(busy * 1000.0));
        t += interval / 1e6;
        ++pics;
        rets += n;
        if (ctl.Current() != lastRung) {
            ++rungChanges;
            if (ctl.Current() < lastRung) ++downChanges;
            char line[160];
            std::snprintf(line, sizeof line, "t=%6.1f rung %d -> %d (%s)", t, lastRung, ctl.Current(), WhyText(ctl.LastChange().why));
            log.push_back(line);
            lastRung = ctl.Current();
        }
        if (ctl.Steps() != lastSteps) {
            ++stepChanges;
            char line[160];
            std::snprintf(line, sizeof line, "t=%6.1f steps %u -> %u", t, lastSteps, ctl.Steps());
            log.push_back(line);
            lastSteps = ctl.Steps();
        }
    }
    void Run(const Scene& s, double sec) {
        const double end = t + sec;
        double next = t + 5;
        unsigned p0 = pics, r0 = rets;
        while (t < end) {
            Picture(s);
            if (std::getenv("P60TRACE") && t >= next) {
                uint32_t gu = 0, pu = 0;
                int low = 0;
                port_autoq_stages(&gu, &pu, &low);
                std::printf("      [%6.1f] %.1f/s rung %d steps %u cad %d rate %.1f est %.1f (game %.1f pipe %.1f) gpu %.1f pipe %.1f low %d reach %d floor %.1f\n", t,
                            60.0 * (pics - p0) / std::max(1u, rets - r0), ctl.Current(), ctl.Steps(), cad.Engaged(), cad.RateFps(), cad.EstimateFps(), cad.GameFps(),
                            cad.PipeFps(), gu / 1000.0, pu / 1000.0, low, ctl.Pace60().reach, ctl.Pace60().floorMs);
                next += 5;
                p0 = pics;
                r0 = rets;
            }
        }
    }
    void Mark() { pics = rets = 0; }
    double Fps() const { return rets ? 60.0 * pics / rets : 0; }
    void Print(const char* title) const {
        if (!g_verbose) return;
        std::printf("    %s:\n", title);
        for (const std::string& l : log) std::printf("      %s\n", l.c_str());
    }
};

// Performance mode as the Vita runs it (entry.cpp, Settings: GXV_AUTOQ_TARGET=60 GXV_AUTOQ_MAX=2 GXV_AUTOQ_MSAA=0 GXV_AUTOQ_EDGESTEP=1
// GXV_STEADY30=1, FXSTEP=3 = the cheaper background only, OVERLOAD=2, FREE 85, WIPE, LATEROOM, RECOVER 20 s, CADENCE_UP, FAST60).
Config Perf(bool p60, bool p23 = false) {
    Config c;
    c.targetFps = 60;
    c.budgetMs = 1000.0 / 60.0;
    c.minRung = 0;
    c.maxRung = kToday;
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
    if (p23) {
        c.fast60Pics = 16; c.fast60Late = 12; c.fast60SpanSec = 1.6; c.fast60GapSec = 1.5; c.cadenceGpuScale = c.p23Scale;
        c.cadenceReachFps = port::pace::kPace23Steady30AboveFps;
    }
    c.p60 = p60;
    if (p60) c.cadenceReachFps = port::pace::kPace23Steady30AboveFps; // (the back end: GXV_PACE60 gives the steady 30 GXV_PACE23's 50 / 57)
    return c;
}

Scene Honeyhive() {
    Scene s;
    s.ta = 12.7; s.efb720 = 14.4; s.dispOn = 4.5; s.dispOff = 2.3; s.k = 0.26; s.share = 0.6;   // 3D 18.9, busy 22.2
    s.gameDraw = 19.7; s.gameIdle = 8.5; s.front = 18.2; s.backend = 12;
    s.dispWaitEvery = 3; s.dispWaitMs = 6;  // (on the steady 30 the display scene waits for its buffer)
    return s;
}
Scene HoneyGpuOnly() { Scene s = Honeyhive(); s.gameDraw = 13; s.gameIdle = 8; s.front = 13; s.backend = 11; return s; }
Scene GoodEgg() {
    Scene s;
    s.ta = 6.5; s.efb720 = 11.5; s.dispOn = 4.5; s.dispOff = 2.3; s.k = 0.3; s.share = 0.6;    // 3D 16, busy ~18 at 720x404
    s.gameDraw = 12.5; s.gameIdle = 9; s.front = 9; s.backend = 9;
    return s;
}
Scene BeachBowl() {
    Scene s;
    s.ta = 16; s.efb720 = 12; s.dispOn = 4.5; s.dispOff = 2.3; s.k = 0.5; s.share = 0.35;      // busy ~24.3, sizes save ~4-6 %
    s.gameDraw = 13; s.gameIdle = 9; s.front = 12; s.backend = 11;
    return s;
}
} // namespace

int main(int argc, char** argv) {
    g_verbose = argc > 1 && std::string(argv[1]) == "verbose";
    setenv("GXV_WIDE_RUNGS", "1", 1);
    setenv("GXV_AUTOQ_TARGET", "60", 1); // (port_autoq_stages answers in target 60 only)
    char line[400];

    std::printf("the switch\n");
    {
        Config d;
        Check(!d.p60, "Config's own default: off");
        Config q = Perf(true);
        q.targetFps = 30;
        q.budgetMs = 1000.0 / 30.0;
        Controller c30(q);
        Check(!c30.Cfg().p60, "target 30 (Quality, Locked 30): forced off");
        Controller c60(Perf(true));
        Check(c60.Cfg().p60, "target 60: on");
        Controller off(Perf(false));
        off.AddSplit(12000, 14000, 4500, 22000);
        Check(!off.Pace60().ready && off.Pace60().pictures == 0, "off: a split is ignored");
    }

    std::printf("the model\n");
    {
        Controller c(Perf(true));
        const Scene s = Honeyhive();
        for (int i = 0; i < 60; ++i) {
            const Gpu g = GpuAt(s, kToday, 0, 0);
            const double wait = (i % 3 == 0) ? 6.0 : 0.0;
            c.AddSplit(static_cast<uint32_t>(g.ta * 1000), static_cast<uint32_t>(g.efb * 1000), static_cast<uint32_t>((g.disp + wait) * 1000),
                       static_cast<uint32_t>((g.busy + wait) * 1000));
        }
        const Controller::Pace60Stats& p = c.Pace60();
        std::snprintf(line, sizeof line, "filled from 60 pictures at 720x404: TA %.1f, 3D %.1f + display %.1f (a 6 ms wait in 1 of 3 trimmed), busy %.1f, overlap %.2f",
                      p.taMs, p.efbMs, p.dispMs, p.busyMs, p.overlap);
        Check(p.ready && std::fabs(p.taMs - 12.7) < 0.6 && std::fabs(p.efbMs - 14.4) < 0.8 && p.dispMs < 5.2 && std::fabs(p.busyMs - 22.2) < 1.2 &&
              std::fabs(p.overlap - 0.26) < 0.12, line);
        const double floor = c.Pace60Predict(0, 4u | 2u), truth = GpuAt(s, 0, 6u, 0).busy, now = c.Pace60Predict(kToday, 0);
        std::snprintf(line, sizeof line, "608x348 with edge off + cheaper background: predicted %.1f ms (true %.1f with the TRUE share 60 %%, assumed 70 %%); 720x404 now %.1f",
                      floor, truth, now);
        Check(std::fabs(floor - truth) < 1.0 && std::fabs(now - 22.2) < 1.2, line);
        std::snprintf(line, sizeof line, "the old estimates for the same: all pixels %.1f ms, pace23's 60 %% of busy %.1f ms, GPUSCALE 25 %.1f ms",
                      22.2 * Cost(0) / Cost(kToday), 22.2 * (0.4 + 0.6 * Cost(0) / Cost(kToday)), 22.2 * (1 - 0.25 * (1 - Cost(0) / Cost(kToday))));
        Check(true, line);
        // a rung per size, for the report
        std::string table = "per rung (edge on / off, + bg):";
        for (int r = 0; r <= kToday; ++r) {
            char cell[96];
            std::snprintf(cell, sizeof cell, " %dx%d %.1f/%.1f/%.1f", RungAt(r).w, RungAt(r).h, c.Pace60Predict(r, 0), c.Pace60Predict(r, 4), c.Pace60Predict(r, 6));
            table += cell;
        }
        Check(true, table.c_str());
    }

    std::printf("Honeyhive (front end 18.2 ms, game drawn turn 19.7 ms, GPU 22.2 ms at 720x404)\n");
    for (int p60 = 0; p60 <= 1; ++p60) {
        Sim s(Perf(p60 != 0), true);
        s.Run(Honeyhive(), 30);
        s.Mark();
        s.Run(Honeyhive(), 60);
        std::snprintf(line, sizeof line, "%s: %.1f a second, steady 30 %s, rung %d, steps %u; %u size changes (%u down), %u step changes; 60 %s",
                      p60 ? "pace60" : "old   ", s.Fps(), s.cad.Engaged() ? "on" : "off", s.ctl.Current(), s.ctl.Steps(), s.rungChanges, s.downChanges, s.stepChanges,
                      s.ctl.Pace60().ready ? (s.ctl.Pace60().reach ? "reachable" : "out of reach") : "-");
        s.Print(p60 ? "pace60" : "old");
        if (p60) Check(s.cad.Engaged() && s.ctl.Current() == kToday && s.downChanges == 0 && !s.ctl.Pace60().reach && std::fabs(s.Fps() - 30) < 1.0, line);
        else Check(true, line);
    }

    std::printf("Honeyhive's GPU with the CPU fitting 60 (the GPU alone decides)\n");
    for (int p60 = 0; p60 <= 1; ++p60) {
        Sim s(Perf(p60 != 0), true);
        s.Run(HoneyGpuOnly(), 40);
        s.Mark();
        s.Run(HoneyGpuOnly(), 60);
        std::snprintf(line, sizeof line, "%s: %.1f a second, steady 30 %s, rung %d, steps %u; %u size changes (%u down), %u step changes; 60 %s",
                      p60 ? "pace60" : "old   ", s.Fps(), s.cad.Engaged() ? "on" : "off", s.ctl.Current(), s.ctl.Steps(), s.rungChanges, s.downChanges, s.stepChanges,
                      s.ctl.Pace60().ready ? (s.ctl.Pace60().reach ? "reachable" : "out of reach") : "-");
        s.Print(p60 ? "pace60" : "old");
        // (the model puts this GPU at ~17 ms at 608x348 with every step: right at the line; either 60 (57+ a second) or the even 30 at 720x404,
        // never the chase that ends in neither)
        if (p60) Check(s.ctl.Pace60().verdicts <= 2 && ((!s.cad.Engaged() && s.Fps() >= 55.0) || (s.cad.Engaged() && s.ctl.Current() == kToday && s.downChanges == 0)), line);
        else Check(true, line);
    }

    std::printf("Good Egg (GPU-limited, ~18 ms at 720x404, TA 6.5 ms)\n");
    for (int p60 = 0; p60 <= 1; ++p60) {
        Sim s(Perf(p60 != 0), true);
        s.Run(GoodEgg(), 40);
        s.Mark();
        s.Run(GoodEgg(), 60);
        std::snprintf(line, sizeof line, "%s: %.1f a second, steady 30 %s (%u changes), rung %d, steps %u; %u size changes, %u step changes; 60 %s",
                      p60 ? "pace60" : "old   ", s.Fps(), s.cad.Engaged() ? "on" : "off", s.cad.Changes(), s.ctl.Current(), s.ctl.Steps(), s.rungChanges, s.stepChanges,
                      s.ctl.Pace60().ready ? (s.ctl.Pace60().reach ? "reachable" : "out of reach") : "-");
        s.Print(p60 ? "pace60" : "old");
        if (p60) Check(!s.cad.Engaged() && s.Fps() > 58.0 && s.ctl.Pace60().reach && s.rungChanges <= 2, line);
        else Check(true, line);
    }

    std::printf("Beach Bowl (TA 16 ms, sizes save 4-6 %%)\n");
    for (int p60 = 0; p60 <= 1; ++p60) {
        Sim s(Perf(p60 != 0), true);
        s.Run(BeachBowl(), 40);
        s.Mark();
        s.Run(BeachBowl(), 60);
        std::snprintf(line, sizeof line, "%s: %.1f a second, steady 30 %s, rung %d, steps %u; %u size changes (%u down), %u step changes; 60 %s",
                      p60 ? "pace60" : "old   ", s.Fps(), s.cad.Engaged() ? "on" : "off", s.ctl.Current(), s.ctl.Steps(), s.rungChanges, s.downChanges, s.stepChanges,
                      s.ctl.Pace60().ready ? (s.ctl.Pace60().reach ? "reachable" : "out of reach") : "-");
        s.Print(p60 ? "pace60" : "old");
        if (p60) Check(s.cad.Engaged() && s.ctl.Current() == kToday && s.downChanges == 0 && !s.ctl.Pace60().reach, line);
        else Check(true, line);
    }

    std::printf("with GXV_PACE23=1 as well (engage below 50, release at 57)\n");
    {
        const Scene sc[3] = {HoneyGpuOnly(), GoodEgg(), BeachBowl()};
        const char* name[3] = {"Honeyhive GPU", "Good Egg", "Beach Bowl"};
        for (int i = 0; i < 3; ++i) {
            Sim s(Perf(true, true), true, true);
            s.Run(sc[i], 40);
            s.Mark();
            s.Run(sc[i], 60);
            std::snprintf(line, sizeof line, "%s: %.1f a second, steady 30 %s, rung %d, steps %u, %u size changes (%u down)", name[i], s.Fps(),
                          s.cad.Engaged() ? "on" : "off", s.ctl.Current(), s.ctl.Steps(), s.rungChanges, s.downChanges);
            s.Print(name[i]);
            const bool want60 = i == 1;
            Check(want60 ? (!s.cad.Engaged() && s.Fps() > 58.0) : (s.cad.Engaged() && s.downChanges == 0), line);
        }
    }

    std::printf("borderline: the EFB 3D swings +-12 %% over 40 s around the threshold, 10 minutes\n");
    {
        Scene b = GoodEgg();
        b.ta = 11; b.efb720 = 15; b.swingAmp = 0.12; b.swingSec = 40; // (608x348 with every step: 15.7-18.5 ms over the swing)
        Sim s(Perf(true), true);
        s.Run(b, 600);
        std::snprintf(line, sizeof line, "%u verdicts, %u steady-30 changes, %u size changes, %u step changes in 10 minutes; %.1f a second",
                      s.ctl.Pace60().verdicts, s.cad.Changes(), s.rungChanges, s.stepChanges, 60.0 * s.pics / s.rets);
        s.Print("borderline");
        Check(s.ctl.Pace60().verdicts <= 32 && s.cad.Changes() <= 16 && s.rungChanges <= 20, line);
        // and a steady scene right at the threshold: one verdict, never flipping
        Scene e = GoodEgg();
        e.ta = 11; e.efb720 = 15;
        Sim f(Perf(true), true);
        f.Run(e, 300);
        std::snprintf(line, sizeof line, "a steady scene at the edge (floor predicted %.1f ms): %u verdict changes, %u steady-30 changes, %u size changes in 5 minutes",
                      f.ctl.Pace60().floorMs, f.ctl.Pace60().verdicts, f.cad.Changes(), f.rungChanges);
        f.Print("edge");
        Check(f.ctl.Pace60().verdicts <= 2 && f.cad.Changes() <= 3 && f.rungChanges <= 3, line);
    }

    std::printf("a GPU over 33 ms at 720x404 while 60 is out of reach: the steady 30 still gets its sizes\n");
    {
        Scene h = BeachBowl();
        h.ta = 20; h.efb720 = 22; h.k = 0.5; // busy ~37 at 720x404
        Sim s(Perf(true), true);
        s.Run(h, 60);
        s.Mark();
        s.Run(h, 30);
        std::snprintf(line, sizeof line, "%.1f a second, steady 30 %s, rung %d, steps %u, %u size changes down", s.Fps(), s.cad.Engaged() ? "on" : "off", s.ctl.Current(),
                      s.ctl.Steps(), s.downChanges);
        s.Print("heavy");
        Check(s.ctl.Current() < kToday || s.ctl.Steps() != 0, line);
    }

    std::printf("the upscaler: the display scene 5.5 ms with it, 2.3 without (its edge step)\n");
    {
        Config c = Perf(true);
        c.p60EdgeMs = 3.2; // (what the back end sets with GXV_UPSCALE on)
        Controller ctl(c);
        Scene u = GoodEgg();
        u.dispOn = 5.5;
        for (int i = 0; i < 40; ++i) {
            const Gpu g = GpuAt(u, kToday, 0, 0);
            ctl.AddSplit(static_cast<uint32_t>(g.ta * 1000), static_cast<uint32_t>(g.efb * 1000), static_cast<uint32_t>(g.disp * 1000), static_cast<uint32_t>(g.busy * 1000));
        }
        const double on = ctl.Pace60Predict(kToday, 0), off = ctl.Pace60Predict(kToday, 4), tOn = GpuAt(u, kToday, 0, 0).busy, tOff = GpuAt(u, kToday, 4, 0).busy;
        std::snprintf(line, sizeof line, "720x404: with the upscaler %.1f ms (true %.1f), its edge step off %.1f (true %.1f): the step's cost %.1f ms (edgeRatio's 12 %%: %.1f)",
                      on, tOn, off, tOff, on - off, on * (1 - 1 / 1.12));
        Check(std::fabs(on - tOn) < 0.8 && std::fabs(off - tOff) < 0.8, line);
    }

    std::printf("all checks done: %d failed\n", g_failures);
    return g_failures ? 1 : 0;
}
