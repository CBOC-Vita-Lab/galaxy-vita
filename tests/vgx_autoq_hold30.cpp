// Host-side test of Quality mode judged by the pictures (GXV_AUTOQ_HOLD30, Config::hold30 in platform/vgx/src/vgx_autoq.cpp, vgx_autoq.h
// "Quality mode judged by the pictures"). tools/test_vgx_autoq_hold30.sh builds and runs it.
//
// The model is the fixed 30 cap: a picture is a 2-vblank hold (33.4 ms) unless it is late, then a 3-vblank hold (50 ms). Which pictures are
// late, and which stage the floor census blames, is scripted (evenly spread, not random); the GPU's reported busy time is the scene's
// value (with noise), which under the cap may be above the budget although the picture came on time (the timer counts waits).
// The configuration is the Settings' Quality as the Vita runs it: target 30, sizes 608x346 (rung 0, wide ladder) to 960x544 4x, start
// 960x544, cheaper background step, anisotropic filtering from rung 2, FREE 85, LATEROOM, severe overloads, recovery after 20 s.
// 1. Hub kind (r82, lit Observatory): GPU 32.5 ms reported, 99 % on time: stays at 960x544 (the old rules step down).
// 2. GPU reads 102-110 % of the budget but 98 % on time: stays (the old rules step down).
// 3. A real GPU overload, 15 % of the pictures late for the GPU: one rung down after two miss windows (not one), not before.
// 4. Late pictures blamed on the game's CPU (also when the GPU timer reads larger but the game alone was over the budget): no step.
// 5. After a step down, a calm stretch climbs back once (and stays); in a scene where the size does not fit, the climb trial fails,
//    goes back and is retried only after a doubling block (no oscillation).
// 6. Performance (target 60) and the other targets ignore the option; the floor census counts as before.
#include "../platform/vgx/src/vgx_autoq.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
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
constexpr int kEdgeOff = -100, kEdgeOn = -101; // (Event::from of an edge smoothing change; Event::to is the rung)

// What a scene shows at a rung: the GPU's reported busy time of an on-time picture, the share of pictures late, and for the late ones
// the stages' times (GPU reported, two game turns).
struct Shot {
    double gpuMs = 30, gpuJitter = 0.02; // on-time pictures: reported GPU busy (uniform +- jitter share)
    double lateShare = 0;                 // pictures that take 3 vblanks
    double lateGpuMs = 45, lateGameMs = 26; // ... their GPU (reported) and two game turns' CPU
    bool physical = false;                // also late (3 vblanks, the GPU's) whenever this picture's GPU time exceeds the budget + 3 %
};
// (t: the sim's time, seconds; edgeOff: the controller has the present's edge smoothing off, EdgeStepOn())
using SceneFn = std::function<Shot(int rung, double t, bool edgeOff)>;

struct Event {
    double t;
    int from, to;
    Why why;
};

struct Sim {
    Controller ctl;
    std::mt19937 rng{7};
    uint64_t now = 1, backend = 1;
    uint32_t game = 1, turns = 1, front = 1, frontPictures = 1;
    double t = 0, lateAcc = 0;
    std::vector<Event> ev;
    bool edgeOff = false;
    double gameMs = 26, backendMs = 12, frontMs = 22; // on-time pictures: two game turns, renderer, front end (lit hub kind)
    explicit Sim(const Config& c) : ctl(c) {}
    void Frame(const Shot& s) {
        std::uniform_real_distribution<double> u(-s.gpuJitter, s.gpuJitter);
        lateAcc += s.lateShare;
        const bool scripted = lateAcc >= 1.0 - 1e-9;
        if (scripted) lateAcc -= 1.0;
        const double drawn = s.gpuMs * (1.0 + u(rng));
        const bool late = scripted || (s.physical && drawn > 1000.0 / 30.0 * 1.03);
        const unsigned n = late ? 3u : 2u;
        const double gpu = scripted ? s.lateGpuMs : drawn;
        const double game2 = late ? s.lateGameMs : gameMs;
        const uint64_t interval = static_cast<uint64_t>(n * kRetraceUs);
        game += static_cast<uint32_t>(game2 / 2.0 * 1000.0) * n; // (one turn a retrace: n turns a picture)
        turns += n;
        backend += static_cast<uint32_t>(backendMs * 1000.0);
        front += static_cast<uint32_t>(frontMs * 1000.0);
        ++frontPictures;
        FrameTimes f;
        f.startUs = now;
        now += interval;
        f.presentUs = now;
        f.backendRunUs = backend;
        f.gameRunUs = game;
        f.gameTurns = turns;
        f.gameKnown = true;
        f.frontRunUs = front;
        f.frontPictures = frontPictures;
        f.frontKnown = true;
        f.timers = f.busyKnown = true;
        f.gpuBusyUs = static_cast<uint32_t>(gpu * 1000.0);
        f.scene = 0x51u;
        const int before = ctl.Current();
        ctl.Add(f);
        t += static_cast<double>(interval) / 1e6;
        if (ctl.Current() != before) ev.push_back({t, before, ctl.Current(), ctl.LastChange().why});
        if (ctl.EdgeStepOn() != edgeOff) {
            edgeOff = ctl.EdgeStepOn();
            ev.push_back({t, edgeOff ? kEdgeOff : kEdgeOn, ctl.Current(), ctl.LastFxChange().why});
        }
    }
    void Run(const SceneFn& scene, double seconds) {
        const double end = t + seconds;
        while (t < end) Frame(scene(ctl.Current(), t, ctl.EdgeStepOn()));
    }
    unsigned Count(Why why) const {
        unsigned n = 0;
        for (const Event& e : ev) n += e.why == why;
        return n;
    }
    unsigned Ups() const {
        unsigned n = 0;
        for (const Event& e : ev) n += e.from >= kEmergency && e.to > e.from;
        return n;
    }
    unsigned Rungs() const { // changes of size (not edge smoothing)
        unsigned n = 0;
        for (const Event& e : ev) n += e.from >= kEmergency;
        return n;
    }
    void Print(const char* title) const {
        std::printf("    %s: %zu change(s)\n", title, ev.size());
        for (const Event& e : ev) {
            if (e.from < kEmergency) std::printf("      t=%7.1f s  edge smoothing %s at rung %d (%s)\n", e.t, e.from == kEdgeOff ? "OFF" : "on ", e.to, WhyText(e.why));
            else std::printf("      t=%7.1f s  rung %d -> %d (%s)\n", e.t, e.from, e.to, WhyText(e.why));
        }
    }
};

// The Settings' Quality as the Vita runs it (entry.cpp defaults + settings_launch.cpp + the back end's cap rules).
Config Quality(bool hold) {
    Config c;
    c.targetFps = 30;
    c.budgetMs = 1000.0 / 30.0;
    c.minRung = 0;
    c.maxRung = 8;
    c.startRung = kTopSingle;
    c.recover = true;
    c.recoverHoldSec = 20.0;
    c.bgStep = true;            // GXV_AUTOQ_FXSTEP=3
    c.aniso = true;
    c.anisoRung = 2;            // GXV_AUTOQ_ANISO_RUNG=2
    c.freeFrac = 0.85;          // GXV_AUTOQ_FREE=85
    c.lateRoom = true;
    c.newStage = true;
    c.overload = c.overloadFx = true; // GXV_AUTOQ_OVERLOAD=2
    c.hold30 = hold;
    return c;
}

Shot Steady(double gpuMs, double lateShare = 0, double lateGpuMs = 45, double lateGameMs = 26) {
    Shot s;
    s.gpuMs = gpuMs;
    s.lateShare = lateShare;
    s.lateGpuMs = lateGpuMs;
    s.lateGameMs = lateGameMs;
    return s;
}
} // namespace

int main(int argc, char** argv) {
    const bool verbose = argc > 1 && std::string(argv[1]) == "verbose";
    setenv("GXV_WIDE_RUNGS", "1", 1); // (the Vita's ladder: 960x544 / 896x512 / 848x480 / 784x440 / 720x404 ...; read once)

    std::printf("the configuration\n");
    {
        Controller q(Quality(true));
        Check(q.Cfg().hold30 && q.Current() == kTopSingle, "Quality: the option on, starting at 960x544");
        Config p = Quality(true);
        p.targetFps = 60;
        Controller perf(p);
        Check(!perf.Cfg().hold30, "target 60 (Performance): the option is off whatever was given");
        Config z = Quality(true);
        z.targetFps = 0;
        Controller old(z);
        Check(!old.Cfg().hold30, "no target: the option is off");
        Check(!Config().hold30, "Config's default is off (the back end turns it on for Quality only)");
    }

    std::printf("1. hub kind: GPU 32.5 ms reported, 99 %% of the pictures on time\n");
    {
        // (the 1 % late: GPU 36 ms reported, the GPU the largest stage: the census blames the GPU, under the 3 % of a miss; and every 30 s
        // a 3 s stretch in which the GPU timer reads 34 ms, 102 % of the budget, while the pictures still come on time: the console's window)
        const SceneFn hub = [](int, double t, bool) { return Steady(std::fmod(t, 30.0) >= 20.0 && std::fmod(t, 30.0) < 23.0 ? 34.0 : 32.5, 0.01, 36.0); };
        Sim on(Quality(true)), off(Quality(false));
        on.Run(hub, 180.0);
        off.Run(hub, 180.0);
        if (verbose) { on.Print("with the option"); off.Print("without"); }
        Check(on.ev.empty() && on.ctl.Current() == kTopSingle, "with the option: 960x544 for 3 minutes, no change");
        Check(on.ctl.HoldWindows() > 80 && on.ctl.LastHold().pictures >= 55, "... judged in hold windows of ~60 pictures");
        Check(!off.ev.empty() && off.ctl.Current() < kTopSingle, "without it (A/B): the GPU's busy share steps the size down (the console's 960 -> 784)");
    }

    std::printf("2. GPU reads 102-110 %% of the budget, 98 %% of the pictures on time\n");
    {
        const SceneFn heavy = [](int, double, bool) { Shot s = Steady(35.6, 0.02, 40.0); s.gpuJitter = 0.035; return s; }; // 34.4 .. 36.8 ms
        Sim on(Quality(true)), off(Quality(false));
        on.Run(heavy, 180.0);
        off.Run(heavy, 180.0);
        if (verbose) { on.Print("with the option"); off.Print("without"); }
        Check(on.ev.empty() && on.ctl.Current() == kTopSingle, "with the option: 960x544 kept, no change (no severe overload either)");
        Check(!on.ctl.AnisoOn(), "... anisotropic filtering stays off (no room for it)");
        Check(!off.ev.empty() && off.ctl.Current() < kTopSingle, "without it (A/B): stepped down");
    }

    std::printf("3. a real GPU overload: 15 %% of the pictures late for the GPU at 960x544, none below\n");
    {
        const SceneFn over = [](int r, double, bool) { return r >= kTopSingle ? Steady(33.0, 0.15, 45.0) : Steady(29.0); };
        Sim s(Quality(true));
        s.Run(over, 30.0);
        if (verbose) s.Print("overload");
        Check(s.ev.size() == 1 && s.ev[0].from == kTopSingle && s.ev[0].to == kTopSingle - 1 && s.ev[0].why == Why::HoldMiss,
              "one rung down (960x544 -> 896x512), for the pictures late for the GPU");
        Check(!s.ev.empty() && s.ev[0].t >= 2.0 * Quality(true).holdWindowSec && s.ev[0].t <= 7.0,
              "... after two miss windows (4 s), within 7 s");
        Check(s.ctl.BlockedUntilUs(kTopSingle) != 0, "... 960x544 blocked as after an overload");
        // one miss window alone (a 2.5 s burst between calm stretches) costs nothing
        Sim b(Quality(true));
        const SceneFn burstB = [](int, double t, bool) { return t >= 10.0 && t < 12.3 ? Steady(33.0, 0.15, 45.0) : Steady(31.0); };
        b.Run(burstB, 40.0);
        if (verbose) b.Print("one burst");
        Check(b.ev.empty(), "a burst that spoils one hold window only: no step");
    }

    std::printf("4. late pictures blamed on the game's CPU\n");
    {
        // (a) the game alone: two turns 44 ms, the GPU reports 40 ms (it waited)
        const SceneFn cpu = [](int, double, bool) { return Steady(31.0, 0.15, 40.0, 44.0); };
        Sim a(Quality(true));
        a.Run(cpu, 120.0);
        const FloorWindow fa = a.ctl.TakeFloor();
        if (verbose) a.Print("game CPU late");
        Check(a.ev.empty() && a.ctl.Current() == kTopSingle, "15 % late for the game's CPU: 960x544 kept for 2 minutes");
        Check(fa.late > 0 && fa.game == fa.late && fa.gpu == 0, "... the floor census still blames the game's CPU for each");
        // (b) the GPU timer reads larger (46 ms, waits counted) but the game alone was over the budget (40 ms): not the size's fault
        const SceneFn both = [](int, double, bool) { return Steady(31.0, 0.15, 46.0, 40.0); };
        Sim bb(Quality(true));
        bb.Run(both, 120.0);
        const FloorWindow fb = bb.ctl.TakeFloor();
        if (verbose) bb.Print("GPU largest, game over the budget too");
        Check(fb.gpu > 0 && bb.ctl.LastHold().lateGpu == 0, "census blames the GPU, but with the game over the budget it is not late for the GPU");
        Check(bb.ev.empty() && bb.ctl.Current() == kTopSingle, "... 960x544 kept (no severe step either)");
        // (c) the GPU near the budget with game-CPU late pictures: no step even after minutes
        const SceneFn hubCpu = [](int, double, bool) { return Steady(32.8, 0.10, 34.0, 50.0); };
        Sim c(Quality(true));
        c.Run(hubCpu, 180.0);
        Check(c.ev.empty(), "GPU at 98 % of the budget and 10 % late for the game: no step in 3 minutes");
    }

    std::printf("5. the way back: one climb after a calm stretch; a failed climb backs off\n");
    {
        // (a) an overload for 10 s, then the scene is light: down once, then back to 960x544 once, and it stays
        Sim s(Quality(true));
        const SceneFn heavy = [](int r, double, bool) { return r >= kTopSingle ? Steady(33.0, 0.15, 45.0) : Steady(29.0); };
        const SceneFn light = [](int, double, bool) { return Steady(28.0); };
        s.Run(heavy, 10.0);
        const size_t downs = s.ev.size();
        s.Run(light, 240.0);
        if (verbose) s.Print("overload then calm");
        Check(downs == 1 && s.ctl.Current() == kTopSingle, "down once in the overload, back at 960x544 after the calm stretch");
        Check(s.Count(Why::HoldClimb) == 1 && s.ev.size() == 2, "... in one climb trial, and nothing moves after it");
        Check(s.ev.size() == 2 && s.ev[1].t >= s.ev[0].t + 60.0 && s.ev[1].t <= s.ev[0].t + 75.0,
              "... once the block on 960x544 ended (60 s) and the calm windows were there");
        // (b) the size never fits at 960x544: the climb trial fails, goes back, and is retried only after a doubling block
        Sim f(Quality(true));
        const SceneFn never = [](int r, double, bool) { return r >= kTopSingle ? Steady(33.0, 0.15, 45.0) : Steady(29.0); };
        f.Run(never, 600.0);
        if (verbose) f.Print("960x544 never fits");
        std::vector<double> trials;
        unsigned backs = 0;
        for (const Event& e : f.ev) {
            if (e.why == Why::HoldClimb) trials.push_back(e.t);
            if (e.why == Why::HoldBack) ++backs;
        }
        Check(f.ev.size() >= 3 && f.ev[0].why == Why::HoldMiss, "first the sustained miss: one rung down");
        Check(!trials.empty() && backs == trials.size(), "every climb trial that lost pictures went back (HoldBack)");
        bool fast = true, doubling = true;
        for (size_t i = 0; i < f.ev.size(); ++i)
            if (f.ev[i].why == Why::HoldBack && (i == 0 || f.ev[i].t - f.ev[i - 1].t > 4.5)) fast = false;
        for (size_t i = 2; i < trials.size(); ++i)
            if (trials[i] - trials[i - 1] < 1.5 * (trials[i - 1] - trials[i - 2])) doubling = false;
        Check(fast, "... within one hold window of the trial (one miss window, not two)");
        Check(trials.size() >= 2 && trials.size() <= 4 && doubling, "... retried after 60, 120, 240 s: at most 4 trials in 10 minutes, spaced further each time");
        Check(f.ctl.Current() == kTopSingle - 1, "... and it ends at 896x512");
        double at6 = 0;
        for (size_t i = 0; i < f.ev.size(); ++i)
            if (f.ev[i].to == kTopSingle && i + 1 < f.ev.size()) at6 += f.ev[i + 1].t - f.ev[i].t;
        Check(at6 < 4.0 * trials.size() + 1.0, "... less than ~4 s at 960x544 per trial");
        // (c) the hub after a wrong step: GPU 31 ms at 784x440 (it hardly scales), 99 % on time: climbs back trial by trial to 960x544
        Sim h(Quality(true));
        const SceneFn spike = [](int r, double, bool) { return r >= kTopSingle ? Steady(33.0, 0.15, 45.0) : Steady(31.0); };
        h.Run(spike, 6.0);
        const SceneFn hub = [](int, double, bool) { return Steady(31.2, 0.0); };
        h.Run(hub, 300.0);
        if (verbose) h.Print("hub after a spike");
        Check(h.ctl.Current() == kTopSingle && h.Count(Why::HoldBack) == 0, "the hub after one step down is back at 960x544, no failed trial");
    }

    std::printf("6. other modes and the census\n");
    {
        // Performance: the same trace with the option given and not: the same changes
        Config p1 = Quality(true), p0 = Quality(false);
        p1.targetFps = p0.targetFps = 60;
        p1.budgetMs = p0.budgetMs = 1000.0 / 60.0;
        p1.maxRung = p0.maxRung = kTopSingle;
        p1.startRung = p0.startRung = 2;
        Sim a(p1), b(p0);
        const SceneFn gpu = [](int r, double, bool) { return Steady(14.0 + 6.0 * Cost(r), 0.05, 30.0); };
        a.Run(gpu, 120.0);
        b.Run(gpu, 120.0);
        bool same = a.ev.size() == b.ev.size();
        for (size_t i = 0; same && i < a.ev.size(); ++i) same = a.ev[i].to == b.ev[i].to && a.ev[i].why == b.ev[i].why && std::abs(a.ev[i].t - b.ev[i].t) < 1e-9;
        Check(same, "target 60: identical decisions with and without the option");
        // Locked 30 is the same controller with its floor at 960x544; the back end leaves the option off there (cfg.minRung < 6). Here: target
        // 30 without the option decides as before on the overload trace (the old rules: the GPU's busy share)
        Sim o(Quality(false));
        const SceneFn over = [](int r, double, bool) { return r >= kTopSingle ? Steady(33.0, 0.15, 45.0) : Steady(29.0); };
        o.Run(over, 30.0);
        Check(!o.ev.empty() && o.Count(Why::HoldMiss) == 0 && o.Count(Why::HoldClimb) == 0, "target 30 without the option: none of the new reasons");
        // the floor census is the same with the option on and off
        Sim c1(Quality(true)), c0(Quality(false));
        const SceneFn hub = [](int, double, bool) { return Steady(30.0, 0.05, 36.0, 26.0); };
        c1.ctl.TakeFloor();
        c0.ctl.TakeFloor();
        c1.Run(hub, 4.0);
        c0.Run(hub, 4.0);
        const FloorWindow w1 = c1.ctl.TakeFloor(), w0 = c0.ctl.TakeFloor();
        Check(w1.pictures == w0.pictures && w1.late == w0.late && w1.gpu == w0.gpu && w1.game == w0.game && w1.front == w0.front,
              "the floor census counts the same pictures and stages");
    }

    std::printf("7. the present's edge smoothing first (GXV_AUTOQ_EDGESTEP=1 in Quality): ~3.5 ms of GPU at 960x544\n");
    {
        Config qe = Quality(true);
        qe.edgeStep = true;
        // A GPU model: fixed + per-pixel work at 960x544 (scaled by Cost) + the edge pass while it is on; late whenever the picture's GPU
        // time is over 34.3 ms (physical), +-4 % from picture to picture.
        auto model = [](double fixedMs, double pixMs, double edgeMs) {
            return [=](int r, double, bool edgeOff) {
                Shot s = Steady(fixedMs + pixMs * Cost(r) / Cost(kTopSingle) + (edgeOff ? 0.0 : edgeMs));
                s.gpuJitter = 0.04;
                s.physical = true;
                return s;
            };
        };
        unsigned edgeEvents = 0;
        auto edges = [&edgeEvents](const Sim& x) { edgeEvents = 0; for (const Event& e : x.ev) edgeEvents += e.from < kEmergency; return edgeEvents; };
        // (a) hub at 960x544, a real overload with edge smoothing on (34.5 ms), 31 ms without it
        {
            Sim h(qe);
            h.Run(model(13.0, 18.0, 3.5), 180.0);
            if (verbose) h.Print("hub 34.5 ms");
            Check(edges(h) == 1 && h.ev[0].from == kEdgeOff && h.ev[0].why == Why::HoldEdgeOff && h.ev[0].to == kTopSingle,
                  "GPU 34.5 ms at 960x544: edge smoothing off at 960x544, once");
            Check(!h.ev.empty() && h.ev[0].t >= 4.0 && h.ev[0].t <= 7.0, "... after two miss windows");
            Check(h.Rungs() == 0 && h.ctl.Current() == kTopSingle, "... no change of size in 3 minutes");
            Check(h.ctl.HoldEdgeSavingMs() > 2.5 && h.ctl.HoldEdgeSavingMs() < 5.0, "... its saving measured (~3.5 ms), so on again is not predicted to fit");
        }
        // (b) 38 ms with it on, 34.5 without: off first, then still missing: one size down (896x512 fits), edge smoothing stays off there
        {
            Sim g(qe);
            g.Run(model(14.5, 20.0, 3.5), 40.0);
            if (verbose) g.Print("GPU 38 ms");
            Check(g.ev.size() == 2 && g.ev[0].from == kEdgeOff && g.ev[1].from == kTopSingle && g.ev[1].to == kTopSingle - 1 &&
                  g.ev[1].why == Why::HoldMiss, "GPU 38 ms: edge smoothing off, then 960x544 -> 896x512 for the misses that remained");
            Check(g.ev.size() == 2 && g.ev[1].t - g.ev[0].t >= 4.0, "... the size only after two more miss windows with it off");
            Check(g.ctl.EdgeStepOn() && g.ctl.Current() == kTopSingle - 1, "... and it stays off at 896x512");
        }
        // (c) edge off, then the scene gets lighter: on again after the calm windows, as a trial that holds; the size never moved
        {
            Sim c(qe);
            c.Run(model(13.0, 18.0, 3.5), 20.0);
            c.Run(model(9.0, 13.0, 3.5), 120.0);
            if (verbose) c.Print("hub, then lighter");
            Check(edges(c) == 2 && c.ev.back().from == kEdgeOn && c.ev.back().why == Why::HoldEdgeOn && !c.ctl.EdgeStepOn(),
                  "lighter scene: edge smoothing on again, and it stays on");
            // (the calm windows count from the miss: 31 ms without edge smoothing is calm already; on again once the GPU plus its saving fits)
            Check(c.ev.size() == 2 && c.ev[1].t >= c.ev[0].t + 2.0 * qe.holdClimbWindows && c.ev[1].t >= 20.0, "... after holdClimbWindows calm windows (20 s), in the lighter part");
            Check(c.Rungs() == 0, "... no change of size");
        }
        // (d) down with edge off, then lighter: back to 960x544 first (arriving with edge smoothing off), then edge smoothing on by its rule
        {
            Sim d(qe);
            d.Run(model(14.5, 20.0, 3.5), 15.0);
            d.Run(model(9.0, 13.0, 3.5), 200.0);
            if (verbose) d.Print("down, then lighter");
            const bool order = d.ev.size() == 4 && d.ev[0].from == kEdgeOff && d.ev[1].to == kTopSingle - 1 && d.ev[2].to == kTopSingle &&
                               d.ev[2].why == Why::HoldClimb && d.ev[3].from == kEdgeOn;
            Check(order, "edge off, 896x512, back to 960x544 with edge off, then edge smoothing on: the reverse of the way down");
            Check(order && d.ev[3].t - d.ev[2].t >= 2.0 * qe.holdClimbWindows - 2.0, "... edge smoothing only after calm windows at 960x544");
            Check(d.ctl.Current() == kTopSingle && !d.ctl.EdgeStepOn(), "... ends at 960x544 with edge smoothing");
        }
        // (e) edge smoothing on never fits (it costs 2 ms but brings 10 % late pictures), off fits: on again is tried, fails, backs off doubling
        {
            Sim e(qe);
            const SceneFn never = [](int, double, bool edgeOff) {
                Shot s = edgeOff ? Steady(26.0) : Steady(28.0, 0.10, 36.0);
                return s;
            };
            e.Run(never, 600.0);
            if (verbose) e.Print("edge on never fits");
            std::vector<double> ons;
            unsigned backs = 0;
            for (const Event& x : e.ev) {
                if (x.from == kEdgeOn) ons.push_back(x.t);
                if (x.why == Why::HoldEdgeBack) ++backs;
            }
            bool doubling = true;
            for (size_t i = 2; i < ons.size(); ++i)
                if (ons[i] - ons[i - 1] < 1.5 * (ons[i - 1] - ons[i - 2])) doubling = false;
            Check(!e.ev.empty() && e.ev[0].why == Why::HoldEdgeOff, "first off for the misses");
            Check(!ons.empty() && backs == ons.size(), "every try on again lost pictures and went off again (HoldEdgeBack)");
            Check(ons.size() >= 2 && ons.size() <= 4 && doubling, "... tried at most 4 times in 10 minutes, further apart each time (60, 120, 240 s blocks)");
            Check(e.Rungs() == 0 && e.ctl.EdgeStepOn(), "... no change of size, and it ends off");
        }
        // (f) the hub that holds (GPU 32.5 ms with it on, 99 % on time): nothing changes, edge smoothing stays on
        {
            Sim f(qe);
            f.Run([](int, double, bool) { return Steady(32.5, 0.01, 36.0); }, 180.0);
            Check(f.ev.empty() && !f.ctl.EdgeStepOn(), "hub that holds 30: edge smoothing stays on, no change");
        }
        // (g) target 60 with the same config: the edge step is one of the steps at today's rung as before (identical decisions)
        {
            Config p1 = qe, p0 = qe;
            p1.targetFps = p0.targetFps = 60;
            p1.budgetMs = p0.budgetMs = 1000.0 / 60.0;
            p0.hold30 = false;
            p1.maxRung = p0.maxRung = kTopSingle;
            p1.startRung = p0.startRung = 2;
            Sim a(p1), b(p0);
            a.gameMs = b.gameMs = 14.0;
            a.frontMs = b.frontMs = 10.0;
            const SceneFn gpu = [](int r, double, bool edgeOff) { return Steady(12.0 + 6.0 * Cost(r) + (edgeOff ? 0.0 : 2.2), 0.02, 30.0); };
            a.Run(gpu, 120.0);
            b.Run(gpu, 120.0);
            bool same = a.ev.size() == b.ev.size() && !a.ev.empty();
            for (size_t i = 0; same && i < a.ev.size(); ++i) same = a.ev[i].from == b.ev[i].from && a.ev[i].to == b.ev[i].to && a.ev[i].why == b.ev[i].why;
            Check(same, "target 60 with edgeStep: identical decisions with and without hold30 given");
        }
    }

    if (g_failures) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
