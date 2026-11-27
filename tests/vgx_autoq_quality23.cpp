// Host-side test of GXV_AUTOQ_QGUARD (Config::qg) in the automatic quality controller (platform/vgx/src/vgx_autoq.cpp "qguard"): quality is
// given up only while it buys pictures. tools/test_vgx_autoq_quality23.sh builds and runs it. A closed loop as in vgx_autoq_pace60.cpp: the
// GPU of a scene is a TA / 3D model (TA the same at any size, the EFB 3D falling with the pixels by a TRUE share, the display scene with /
// without edge smoothing, an overlap), the CPU stages given per phase; pictures come at whole retraces; under the steady 30
// (port/frame_pace.h SteadyCadence, fed the controller's stage hint as port/perf.cpp does) or the fixed 30 cap at least 2.
//  1. The switch: off by default, forced off for target 30 (Quality, Locked 30), cap30 only with it.
//  2. Replay of the console log 483-c8P (Honeyhive, target 60 under the fixed 30 cap, GXV_PACE60=1): the title with the front end swinging
//     12 <-> 21 ms (7 verdict flaps), the load, the loading scene (TA 1.5 ms, other stages 4 ms: "60 reachable"), then play (front end 19-25,
//     GPU 20-23 ms). Old: the steps stay on for good. Guard: no verdict from the title or the loading scene, full quality at rung 2.
//  3. The same with the loading / title signals missing and no settling (defence in depth): the false verdict forms, pace60 takes the steps
//     and 608x348, and the trial restores all of it ("bought no speed"): full quality at rung 2, the reductions blocked.
//  4. Console 491-c9SF1 (Honeyhive, no pace60): fast60 stepped 720 -> 664 -> 608 at an even 30 with the GPU at 19-20 ms; under the fixed cap
//     (with and without the cap30 flag) and on the steady 30.
//  5. Performance's steady 30 with steps left on (an overload burst before it): restored within a few seconds, sharpest first.
//  6. Good Egg (GPU-limited: the steps and sizes buy 60): kept, as without the guard.
//  7. pace60's verdict in play with the other stages swinging around the line: no flapping.
//  8. Quality (target 30) and Locked 30: the same decisions with the switch on or off.
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
    double ta = 7, efb720 = 9, dispOn = 4.5, dispOff = 2.3, k = 0.3, share = 0.6; // GPU (ms at 720x404); share: the TRUE pixel share
    double bg = 0.95;
    double gameDraw = 12, gameIdle = 8, front = 10, backend = 9;                  // CPU per turn / picture
    double frontSwing = 0, frontSwingSec = 0;                                     // the front end swings by +- this (ms) with this period
    double dispWaitEvery = 0, dispWaitMs = 0;
    double cpuNoise = 0, gpuSwing = 0, gpuSwingSec = 0;                           // per-picture CPU noise (+- share); the EFB 3D swings (camera)
    bool cpuPipe = false;                                                         // the front end / renderer run ahead as the GPU does (a 1-2 mix)
    bool loading = false, noStage = false;                                        // FrameTimes::loading / noStage
    uint32_t scene = 0x1234;
};

uint32_t g_rng = 12345;
double Noise(double amp) { g_rng = g_rng * 1664525u + 1013904223u; return 1.0 + amp * ((g_rng >> 8) / 16777216.0 * 2.0 - 1.0); }

struct Gpu { double ta, efb, disp, busy; };
Gpu GpuAt(const Scene& s, int rung, unsigned steps, double t = 0) {
    Gpu g;
    const double swing = s.gpuSwingSec > 0 ? 1.0 + s.gpuSwing * std::sin(6.283185307 * t / s.gpuSwingSec) : 1.0;
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
    bool cap;                                       // the fixed 30 cap: every picture at least 2 retraces (no steady 30)
    uint64_t now = 1000000, backend = 1;
    uint32_t game = 1, turns = 1, front = 1, frontPics = 1;
    double t = 0, tMs_ = 0, gDone_ = 0, cDone_ = 0;
    unsigned pics = 0, rets = 0, picNo = 0;
    unsigned rungChanges = 0, downChanges = 0, stepChanges = 0, stepsOn = 0, verdictsAtMark = 0;
    double atRung[kRungs] = {};                     // seconds shown at each rung since Mark()
    int lastRung;
    unsigned lastSteps = 0;
    bool lastAniso = false;
    std::vector<std::string> log;
    std::vector<Why> whys;
    Sim(const Config& c, bool steady, bool cap30) : ctl(c), cap(cap30), lastRung(c.startRung) {
        port::pace::SteadyCadenceParams p;
        if (c.p60) {
            p.belowFps = port::pace::kPace23Steady30BelowFps;
            p.aboveFps = port::pace::kPace23Steady30AboveFps;
            p.engageSec = 2.5;
            p.releaseSec = 3.0;
        }
        cad.Configure(steady && !cap30, p);
    }
    void Gap(double ms) { // a load's stall: no picture for this long
        tMs_ += ms;
        now += static_cast<uint64_t>(ms * 1000.0);
        t += ms / 1000.0;
        gDone_ = tMs_;
    }
    void Picture(const Scene& s) {
        const int rung = ctl.Current();
        const unsigned steps = ctl.Steps();
        const Gpu g = GpuAt(s, rung, steps, t);
        const double frontMs = (s.front + (s.frontSwingSec > 0 ? s.frontSwing * std::sin(6.283185307 * t / s.frontSwingSec) : 0.0)) * Noise(s.cpuNoise);
        unsigned n = 1;
        if (s.cpuPipe) {
            cDone_ = std::max(cDone_, tMs_ - kR) + std::max(frontMs, s.backend);
            while (n < 8 && (tMs_ + n * kR < cDone_ - 0.01 || s.gameDraw + (n - 1) * s.gameIdle > n * kR)) ++n;
        } else {
            while (n < 8 && (std::max(frontMs, s.backend) > n * kR || s.gameDraw + (n - 1) * s.gameIdle > n * kR)) ++n;
        }
        gDone_ = std::max(gDone_, tMs_ - kR) + g.busy;
        while (n < 8 && tMs_ + n * kR < gDone_ - 0.01) ++n;
        if ((cad.Engaged() || cap) && n < 2) n = 2;
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
        front += static_cast<uint32_t>(frontMs * 1000.0);
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
        f.scene = s.noStage ? 0 : s.scene;
        f.loading = s.loading;
        f.noStage = s.noStage;
        ctl.Add(f);
        double disp = g.disp, busy = g.busy;
        if (s.dispWaitEvery > 0 && (++picNo % static_cast<unsigned>(s.dispWaitEvery)) == 0) { disp += s.dispWaitMs; busy += s.dispWaitMs; }
        ctl.AddSplit(static_cast<uint32_t>(g.ta * 1000.0), static_cast<uint32_t>(g.efb * 1000.0), static_cast<uint32_t>(disp * 1000.0),
                     static_cast<uint32_t>(busy * 1000.0));
        t += interval / 1e6;
        if (rung >= 0 && rung < kRungs) atRung[rung] += interval / 1e6;
        ++pics;
        rets += n;
        if (ctl.Current() != lastRung) {
            ++rungChanges;
            if (ctl.Current() < lastRung) ++downChanges;
            char line[200];
            std::snprintf(line, sizeof line, "t=%6.1f rung %d -> %d (%s)", t, lastRung, ctl.Current(), WhyText(ctl.LastChange().why));
            log.push_back(line);
            whys.push_back(ctl.LastChange().why);
            lastRung = ctl.Current();
        }
        if (ctl.Steps() != lastSteps) {
            ++stepChanges;
            if (ctl.Steps() & ~lastSteps) ++stepsOn;
            char line[200];
            std::snprintf(line, sizeof line, "t=%6.1f steps %u -> %u (%s)", t, lastSteps, ctl.Steps(), WhyText(ctl.LastFxChange().why));
            log.push_back(line);
            whys.push_back(ctl.LastFxChange().why);
            lastSteps = ctl.Steps();
        }
        if (ctl.AnisoOn() != lastAniso) {
            char line[200];
            std::snprintf(line, sizeof line, "t=%6.1f anisotropic %d -> %d (%s)", t, lastAniso, ctl.AnisoOn(), WhyText(ctl.LastAnisoChange().why));
            log.push_back(line);
            lastAniso = ctl.AnisoOn();
        }
    }
    void Run(const Scene& s, double sec) {
        const double end = t + sec;
        while (t < end) Picture(s);
    }
    void Mark() { pics = rets = 0; for (double& a : atRung) a = 0; }
    double Fps() const { return rets ? 60.0 * pics / rets : 0; }
    bool Took(Why w) const { return std::find(whys.begin(), whys.end(), w) != whys.end(); }
    void Print(const char* title) const {
        if (!g_verbose) return;
        std::printf("    %s:\n", title);
        for (const std::string& l : log) std::printf("      %s\n", l.c_str());
    }
};

// Performance mode as the console runs it (entry.cpp + the env.txt PERF line: TARGET=60 MAX=2 MSAA=0 EDGESTEP=1 STEADY30=1; FXSTEP=3, OVERLOAD=2,
// FREE 85, LATEROOM, RECOVER 20 s, CADENCE_UP, FAST60, EDGECHECK, ANISO at rung 2).
Config Perf(bool p60, bool qg, bool cap30 = false) {
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
    c.lateRoom = true;
    c.cadenceUp = true;
    c.newStage = true;
    c.bgStep = c.edgeStep = true;
    c.edgeCheck = true;
    c.fast60 = true;
    c.aniso = true;
    c.anisoRung = kToday;
    c.p60 = p60;
    if (p60) c.cadenceReachFps = port::pace::kPace23Steady30AboveFps;
    c.qg = qg;
    c.cap30 = cap30;
    return c;
}

// The phases of 483-c8P (pace60 lines: TA / 3D / display / busy; other stages from the verdict lines)
Scene Title() { // 16:14:04-16:14:50: busy 9.8 ms, the other stages 12.4 <-> 20.7 ms
    Scene s;
    s.ta = 2.5; s.efb720 = 7.4; s.dispOn = 1.5; s.dispOff = 1.0; s.k = 0.35; s.share = 0.6;
    s.gameDraw = 9; s.gameIdle = 6; s.front = 16.5; s.frontSwing = 4.5; s.frontSwingSec = 6; s.backend = 8;
    s.noStage = true;
    return s;
}
Scene Loading() { // 16:15:10-16:15:17: TA 1.5, 3D 6.2 + display 4.5, busy 11.4, other stages 4 ms; the loading thread runs
    Scene s;
    s.ta = 1.5; s.efb720 = 6.2; s.dispOn = 4.5; s.dispOff = 2.3; s.k = 0.5; s.share = 0.6;
    s.gameDraw = 4; s.gameIdle = 3; s.front = 4; s.backend = 4;
    s.loading = true;
    return s;
}
Scene HoneyPlay() { // 16:15:20 on: TA 10.3-13, 3D 16.4-18 + display 1.6, busy 20.5-23, other stages 18.6-25 (front end), a 30 at 720x404
    Scene s;
    s.ta = 10.5; s.efb720 = 16.4; s.dispOn = 2.0; s.dispOff = 1.6; s.k = 0.24; s.share = 0.9;
    s.gameDraw = 19.5; s.gameIdle = 8.5; s.front = 20.5; s.frontSwing = 1.5; s.frontSwingSec = 9; s.backend = 12;
    s.dispWaitEvery = 3; s.dispWaitMs = 4;
    return s;
}
Scene GoodEgg() {
    Scene s;
    s.ta = 6.5; s.efb720 = 11.5; s.dispOn = 4.5; s.dispOff = 2.3; s.k = 0.3; s.share = 0.6;
    s.gameDraw = 12.5; s.gameIdle = 9; s.front = 9; s.backend = 9;
    return s;
}

// The 483 sequence: title, load, loading scene, play
void Replay483(Sim& s, bool signals) {
    Scene title = Title(), load = Loading(), play = HoneyPlay();
    if (!signals) { title.noStage = false; load.loading = false; }
    s.Run(title, 50);
    s.verdictsAtMark = s.ctl.Pace60().verdicts;
    s.Gap(2000);
    s.Run(load, 8);
    s.Run(play, 100);
}
} // namespace

int main(int argc, char** argv) {
    g_verbose = argc > 1 && std::string(argv[1]) == "verbose";
    if (const char* seed = std::getenv("Q23SEED")) g_rng = static_cast<uint32_t>(std::strtoul(seed, nullptr, 10));
    setenv("GXV_WIDE_RUNGS", "1", 1);
    setenv("GXV_AUTOQ_TARGET", "60", 1); // (port_autoq_stages answers in target 60 only)
    char line[480];

    std::printf("the switch\n");
    {
        Config d;
        Check(!d.qg && !d.cap30, "Config's own default: off");
        Config q = Perf(true, true, true);
        q.targetFps = 30;
        q.budgetMs = 1000.0 / 30.0;
        Controller c30(q);
        Check(!c30.Cfg().qg && !c30.Cfg().cap30, "target 30 (Quality, Locked 30): forced off, cap30 with it");
        Controller c60(Perf(true, true, true));
        Check(c60.Cfg().qg && c60.Cfg().cap30, "target 60: on");
        Controller nc(Perf(true, false, true));
        Check(!nc.Cfg().cap30, "cap30 without the guard: off");
    }

    std::printf("replay of 483-c8P (Honeyhive, target 60 under the fixed 30 cap, GXV_PACE60=1): title, load, loading scene, play\n");
    {
        Sim old(Perf(true, false), true, true);
        Replay483(old, true);
        std::snprintf(line, sizeof line, "old:   %u verdicts in the title, %u in all; final rung %d, steps %u (2 cheaper background, 4 edge off); %u size changes, %u step changes%s",
                      old.verdictsAtMark, old.ctl.Pace60().verdicts, old.ctl.Current(), old.ctl.Steps(), old.rungChanges, old.stepChanges,
                      old.Took(Why::Pace60) ? "; pace60's direct step taken" : "");
        old.Print("old");
        Check(true, line); // (the leak as on the console: the steps stay on at an even 30)
        Sim g(Perf(true, true, true), true, true);
        Replay483(g, true);
        const Controller::QualityStats& q = g.ctl.Quality();
        std::snprintf(line, sizeof line, "guard: %u verdicts in the title (no stage), %u in all, 60 %s; final rung %d, steps %u, anisotropic %s; %u size changes (%u down), "
                      "%u steps on; %u non-play pictures, %u reverts, %u restores; %.1f a second in the last 60 s",
                      g.verdictsAtMark, g.ctl.Pace60().verdicts, g.ctl.Pace60().reach ? "reachable" : "out of reach", g.ctl.Current(), g.ctl.Steps(),
                      g.ctl.AnisoOn() ? "on" : "off", g.rungChanges, g.downChanges, g.stepsOn, q.nonPlay, q.reverts, q.restores, g.Fps());
        g.Print("guard");
        Check(g.verdictsAtMark == 0 && !g.ctl.Pace60().reach && g.ctl.Current() == kToday && g.ctl.Steps() == 0 && g.downChanges == 0 &&
              !g.Took(Why::Pace60) && g.ctl.AnisoOn(), line);
        // the last minute again with the frame rate: an even 30 at full quality
        g.Mark();
        g.Run(HoneyPlay(), 60);
        std::snprintf(line, sizeof line, "guard, 60 s more: %.1f a second, rung %d, steps %u, %u changes in all", g.Fps(), g.ctl.Current(), g.ctl.Steps(), g.rungChanges + g.stepChanges);
        Check(std::fabs(g.Fps() - 30.0) < 0.5 && g.ctl.Current() == kToday && g.ctl.Steps() == 0, line);
    }

    std::printf("the same with the loading and title signals missing and no settling (the false verdict forms): the trial restores everything\n");
    {
        Config c = Perf(true, true, false); // (and no cap30 flag: the 16.7 ms rules as on the console)
        c.qgPlayWindows = 0;
        c.qgPlaySec = 0;
        c.qgReachHoldSec = 0;
        c.qgNeedSec = 0;
        c.qgGpuHoldFrac = 3.0; // (the hold rule out of the way: the trial alone)
        Sim g(c, true, true);
        Replay483(g, false);
        const Controller::QualityStats& q = g.ctl.Quality();
        std::snprintf(line, sizeof line, "%s; final rung %d, steps %u; %u trials, %u kept, %u reverts, %u restores; %u size changes (%u down), %u steps on",
                      g.Took(Why::Pace60) ? "pace60's direct step taken" : "no direct step", g.ctl.Current(), g.ctl.Steps(), q.trials, q.kept, q.reverts,
                      q.restores, g.rungChanges, g.downChanges, g.stepsOn);
        g.Print("no signals");
        Check(g.ctl.Current() == kToday && g.ctl.Steps() == 0 && q.reverts >= 1 && g.Took(Why::QgNoGain) && g.stepsOn <= 4, line);
    }

    std::printf("491-c9SF1 (Honeyhive, no pace60, an even 30 with the GPU at 19-20 ms): fast60 720 -> 664 -> 608\n");
    {
        Scene h = HoneyPlay();
        h.efb720 = 15.4; // (busy ~19.5 at 720x404)
        h.frontSwing = 0;
        h.front = 16.2;  // (fits 17.2: the late pictures are "late for the GPU" against 16.7)
        h.gameDraw = 17.0; h.gameIdle = 8.5;
        const char* name[4] = {"old, fixed cap", "guard, fixed cap, no cap30 flag", "guard, fixed cap30", "guard, steady 30"};
        for (int i = 0; i < 4; ++i) {
            const bool qg = i > 0, cap = i < 3;
            Sim s(Perf(false, qg, i == 2), true, cap);
            s.Run(h, 150);
            std::snprintf(line, sizeof line, "%s: %.1f a second, rung %d, steps %u; %u size changes (%u down), %u steps on%s",
                          name[i], s.Fps(), s.ctl.Current(), s.ctl.Steps(), s.rungChanges, s.downChanges, s.stepsOn, s.Took(Why::FastDown) ? "; fast60 stepped" : "");
            s.Print(name[i]);
            if (!qg) Check(true, line);
            else Check(s.ctl.Current() == kToday && s.ctl.Steps() == 0 && s.downChanges <= 1, line);
        }
    }

    std::printf("Performance's steady 30 with the steps left on by an overload burst: restored within a few seconds, sharpest first\n");
    {
        Scene heavy = HoneyPlay();
        heavy.ta = 12; heavy.efb720 = 36; heavy.share = 0.9; heavy.k = 0.24; heavy.dispOn = 4.5; heavy.dispOff = 2.3; heavy.front = 15; heavy.frontSwing = 0; heavy.gameDraw = 15; // (GPU ~48 ms: a severe overload)
        for (int qg = 0; qg <= 1; ++qg) {
            Sim s(Perf(false, qg != 0), true, false);
            s.Run(HoneyPlay(), 10);
            s.Run(heavy, 12);
            const int rungHeavy = s.ctl.Current();
            const unsigned stepsHeavy = s.ctl.Steps();
            const double t0 = s.t;
            double back = -1;
            while (s.t < t0 + 60) {
                s.Picture(HoneyPlay());
                if (back < 0 && s.ctl.Current() == kToday && s.ctl.Steps() == 0) back = s.t - t0;
            }
            std::snprintf(line, sizeof line, "%s: after the burst rung %d steps %u; then steady 30 %s, full quality %s%.1f s after; final rung %d, steps %u",
                          qg ? "guard" : "old  ", rungHeavy, stepsHeavy, s.cad.Engaged() ? "on" : "off", back < 0 ? "never (" : "", back < 0 ? 0.0 : back,
                          s.ctl.Current(), s.ctl.Steps());
            s.Print(qg ? "guard" : "old");
            if (qg) Check(rungHeavy < kToday && stepsHeavy != 0 && back >= 0 && back <= 12.0 && s.ctl.Current() == kToday && s.ctl.Steps() == 0, line);
            else Check(true, line);
        }
    }

    std::printf("Good Egg (GPU-limited at ~18 ms: a step buys 60): kept with the guard\n");
    for (int p60 = 0; p60 <= 1; ++p60) {
        for (int qg = 0; qg <= 1; ++qg) {
            Sim s(Perf(p60 != 0, qg != 0), true, false);
            s.Run(GoodEgg(), 40);
            s.Mark();
            s.Run(GoodEgg(), 60);
            std::snprintf(line, sizeof line, "%s%s: %.1f a second, steady 30 %s, rung %d, steps %u; %u size changes, %u step changes; %u kept, %u reverts",
                          p60 ? "pace60" : "rules ", qg ? " + guard" : "        ", s.Fps(), s.cad.Engaged() ? "on" : "off", s.ctl.Current(), s.ctl.Steps(), s.rungChanges,
                          s.stepChanges, s.ctl.Quality().kept, s.ctl.Quality().reverts);
            s.Print(line);
            if (qg) Check(s.Fps() > 57.0 && !s.cad.Engaged(), line);
            else Check(true, line);
        }
    }

    std::printf("Good Egg on the console (c17): GPU the limit at 720x404 (~50 a second), 608x348 ~57; the front end close to the GPU, noisy\n");
    {
        // 720x404: TA 7, 3D 14 + display 4.5 (2.3 without edge smoothing), busy ~20.6 (edge off ~18.4); 608x348 edge off ~15.7. The front
        // end 16.3 ms +-12 % a picture, the game's drawn turn 14: the other stages sit right beside the GPU's time (the "GPU the limit" coin flip)
        Scene sc[2];
        for (Scene& e : sc) {
            e = GoodEgg();
            e.ta = 7; e.efb720 = 14; e.share = 0.7; e.dispOn = 4.5; e.dispOff = 2.3; e.k = 0.3;
            e.front = 14.8; e.cpuNoise = 0.08; e.gameDraw = 14; e.gameIdle = 9; e.backend = 11;
            e.gpuSwing = 0.08; e.gpuSwingSec = 23; // (the camera: the EFB 3D +-8 % over 23 s)
        }
        // (b: the console's own numbers: 608x348 gives ~57, not 60: the front end at ~17.5 ms holds it there, right beside the GPU's time with
        // edge smoothing off at 720x404)
        sc[1].front = 17.4; sc[1].cpuNoise = 0.05; sc[1].cpuPipe = true; sc[1].efb720 = 16;
        for (int v = 0; v < 2; ++v) {
        const Scene& e = sc[v];
        std::printf("  %s\n", v ? "b: the front end ~17.4 ms (608x348 ~57 a second)" : "a: the front end ~14.8 ms");
        double fps[2][2] = {};
        for (int p60 = 0; p60 <= 1; ++p60) {
            for (int qg = 0; qg <= 1; ++qg) {
                Sim s(Perf(p60 != 0, qg != 0), true, false);
                s.Run(e, 15);
                s.Mark();
                s.Run(e, 105);
                fps[p60][qg] = s.Fps();
                std::snprintf(line, sizeof line, "%s%s: %.1f a second; time at 608/664/720: %.0f/%.0f/%.0f s; steady 30 %s; %u size changes (%u down), %u steps on; "
                              "%u trials, %u kept, %u reverts", p60 ? "pace60" : "rules ", qg ? " + guard" : "        ", s.Fps(), s.atRung[0], s.atRung[1], s.atRung[2],
                              s.cad.Engaged() ? "on" : "off", s.rungChanges, s.downChanges, s.stepsOn, s.ctl.Quality().trials, s.ctl.Quality().kept,
                              s.ctl.Quality().reverts);
                s.Print(line);
                // (b with pace60: the front end over 16.7 ms makes 60 out of reach by its verdict (FITIN 100 %): the steady 30, as without the guard)
                if (qg && !(v && p60)) Check(s.Fps() >= fps[p60][0] - 1.0 && s.Fps() > 54.0 && s.atRung[2] < 0.25 * 105.0, line);
                else if (qg) Check(s.Fps() >= fps[p60][0] - 1.0, line);
                else Check(true, line);
            }
        }
        }
    }

    std::printf("pace60's verdict in play, the other stages swinging 13 <-> 20 ms every 6 s (Good Egg's GPU): no flapping\n");
    for (int qg = 0; qg <= 1; ++qg) {
        Scene b = GoodEgg();
        b.ta = 6; b.efb720 = 8; b.front = 16.5; b.frontSwing = 3.5; b.frontSwingSec = 6;
        Sim s(Perf(true, qg != 0), true, false);
        s.Run(b, 120);
        std::snprintf(line, sizeof line, "%s: %u verdict changes in 120 s, %u steady-30 changes, %u size changes, %u step changes",
                      qg ? "guard" : "old  ", s.ctl.Pace60().verdicts, s.cad.Changes(), s.rungChanges, s.stepChanges);
        s.Print(qg ? "guard" : "old");
        if (qg) Check(s.ctl.Pace60().verdicts <= 2, line);
        else Check(true, line);
    }

    std::printf("a loading scene alone: no verdict, no model, nothing decided\n");
    {
        Sim s(Perf(true, true, false), true, false);
        s.Run(Loading(), 20);
        std::snprintf(line, sizeof line, "20 s of loading pictures: model %s, %u verdicts, %u changes, %u non-play pictures", s.ctl.Pace60().ready ? "filled" : "empty",
                      s.ctl.Pace60().verdicts, s.rungChanges + s.stepChanges, s.ctl.Quality().nonPlay);
        Check(!s.ctl.Pace60().ready && s.ctl.Pace60().verdicts == 0 && s.rungChanges + s.stepChanges == 0, line);
        s.Run(HoneyPlay(), 2);
        Check(!s.ctl.QualitySettled(), "2 s of play after it: not settled yet");
        s.Run(HoneyPlay(), 6);
        std::snprintf(line, sizeof line, "8 s of play after it: settled, first verdict: 60 %s", s.ctl.Pace60().reach ? "reachable" : "out of reach");
        Check(s.ctl.QualitySettled() && !s.ctl.Pace60().reach, line);
    }

    std::printf("Quality (target 30) and Locked 30: the same decisions with the switch on or off\n");
    {
        for (int locked = 0; locked <= 1; ++locked) {
            std::vector<std::string> logs[2];
            for (int qg = 0; qg <= 1; ++qg) {
                g_rng = 4242u + static_cast<uint32_t>(locked); // (the same noise for both runs: only the switch differs)
                Config c;
                c.targetFps = 30;
                c.budgetMs = 1000.0 / 30.0;
                c.minRung = locked ? kTopSingle : 0;
                c.maxRung = locked ? 7 : kTopSingle;
                c.startRung = locked ? 7 : kTopSingle;
                c.bgStep = c.edgeStep = true;
                c.overload = true;
                c.hold30 = !locked;
                c.qg = qg != 0;
                c.cap30 = qg != 0;
                Sim s(c, false, true);
                Scene h = HoneyPlay();
                h.ta = 14; h.efb720 = 22; // (heavy at 960x544: the 30 needs sizes)
                s.Run(Title(), 10);
                s.Gap(2000);
                s.Run(Loading(), 5);
                s.Run(h, 90);
                logs[qg] = s.log;
                if (qg) {
                    std::snprintf(line, sizeof line, "%s: %zu changes with the switch off, %zu with it on, the same: %s; guard %s", locked ? "Locked 30" : "Quality  ",
                                  logs[0].size(), logs[1].size(), logs[0] == logs[1] ? "yes" : "no", s.ctl.Cfg().qg ? "ON" : "off");
                    Check(logs[0] == logs[1] && !s.ctl.Cfg().qg, line);
                }
            }
        }
    }

    std::printf("gov77: Dreadnought in Performance (GPU 19-20 ms a picture, 960x544 fixed, the cheaper steps buy nothing): the same two steps are not retried every 20 s\n");
    {
        unsigned took[2] = {0, 0};
        for (int stageBlock = 0; stageBlock <= 1; ++stageBlock) {
            g_rng = 777u;
            Config c = Perf(false, true, false);
            c.minRung = c.maxRung = c.startRung = kToday;
            c.qgStageBlock = stageBlock != 0;
            Sim s(c, false, false);
            Scene dn;
            dn.ta = 5; dn.efb720 = 17.5; dn.dispOn = 2.0; dn.dispOff = 2.0; dn.bg = 1.0; dn.k = 0.2; dn.share = 0.0;   // (the GPU does not scale with anything the steps touch)
            dn.gameDraw = 10.3; dn.gameIdle = 4; dn.front = 15.1; dn.backend = 8;
            s.Run(GoodEgg(), 10);
            s.Run(dn, 300);
            took[stageBlock] = s.stepsOn;
            std::snprintf(line, sizeof line, "%s: steps taken %u times in 300 s, %u reverts as no gain, %u size changes", stageBlock ? "stage block" : "old 20 s    ", s.stepsOn, s.ctl.Quality().reverts, s.rungChanges);
            s.Print(stageBlock ? "stage block" : "old");
            Check(s.rungChanges == 0, line);
        }
        std::snprintf(line, sizeof line, "fewer retries with the stage block: %u against %u", took[1], took[0]);
        Check(took[1] < took[0] && took[1] <= 6, line);
    }

    std::printf("all checks done: %d failed\n", g_failures);
    return g_failures ? 1 : 0;
}
