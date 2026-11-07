// Host-side test of Performance mode's fast step down (Config::fast60, GXV_AUTOQ_FAST60) in the automatic quality controller
// (platform/vgx/src/vgx_autoq.cpp). tools/test_vgx_autoq_fast60.sh builds and runs it.
//
// The pipeline model is the one of vgx_autoq_cadence.cpp: 60 Hz, a picture at the first retrace after its largest stage, the GPU a fixed
// part plus the pixels times the rung's cost. Scenes (rung 2 = 720x404 on the console's wide ladder, floor 608x348):
// 1. The switch: forced off for target 30 (Quality and Locked 30), off in Config's own defaults.
// 2. A spike (GPU 12 -> 17 ms at 720x404): the first reaction within 0.5 s, with and without the edge step / anisotropic filtering in the
//    way (their order is kept); the rung follows and the pictures are back on 60 (the block of the size left is the hysteresis).
// 3. Pictures late for the CPU stages (the game, the front end) never step; nor do the steady 30's pictures, quiet moments or a movie.
// 4. No oscillation: a short spike, a borderline scene (GPU 16.6 +-6 %), the way back; the size left stays blocked for blockSec.
// 5. Several rungs in a row while the pictures stay late (GPU 30 ms at 720x404, 15 at the floor), stopping at the floor rung; and
//    none when even the floor rung cannot hold 55 (GPU 24 ms at the floor: the steady 30's case).
// 6. Quality (target 30) and every calm Performance trace decide exactly as without the switch.
#include "../platform/vgx/src/vgx_autoq.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

struct Load {
    double turnMs = 7, backendMs = 4, fixedGpuMs = 3, pixGpuMs = 9; // pixGpuMs at 640x456 (Cost 1); one game turn's CPU
    double frontMs = 6;
    double edgeMs = 0;         // the present's edge smoothing: GPU time a picture whatever the size, while it is not given up (0: no such step in the scene)
    double burstMs = 0;        // added GPU time (a spike), whatever the size
    bool cadence = false, quiet = false, movie = false;
    int turns = 1;             // game turns per retrace (the CPU model: one turn a picture at 60)
};
struct Event { double t; char kind; int from, to; Why why; };
constexpr double kRetraceUs = 16683.0;

struct Sim {
    Controller ctl;
    std::mt19937 rng;
    double noise;
    uint64_t now = 1, backend = 1;
    uint32_t game = 1, turns = 1, front = 1, frontPictures = 1;
    double t = 0;
    std::vector<Event> ev;
    unsigned pictures = 0, retraces = 0;
    Sim(const Config& c, uint32_t seed, double n) : ctl(c), rng(seed), noise(n) {}
    void Frame(const Load& l) {
        std::uniform_real_distribution<double> u(-noise, noise);
        const double tms = l.turnMs * (1.0 + u(rng)), bms = l.backendMs * (1.0 + u(rng)), fms = l.frontMs * (1.0 + u(rng));
        const double gpu = (l.fixedGpuMs + l.pixGpuMs * Cost(ctl.Current())) * (1.0 + u(rng)) + (ctl.EdgeStepOn() ? 0.0 : l.edgeMs) + l.burstMs;
        const double work = std::max({tms, bms, gpu, fms});
        const unsigned n = static_cast<unsigned>(std::max(l.cadence ? 2.0 : 1.0, std::ceil(work * 1000.0 / kRetraceUs - 1e-9)));
        const uint64_t interval = static_cast<uint64_t>(n * kRetraceUs);
        game += static_cast<uint32_t>(tms * 1000.0) * n;
        turns += n;
        backend += static_cast<uint32_t>(bms * 1000.0);
        front += static_cast<uint32_t>(fms * 1000.0);
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
        f.cadence30 = l.cadence;
        f.quiet = l.quiet;
        f.movie = l.movie;
        f.timers = f.busyKnown = true;
        f.gpuBusyUs = static_cast<uint32_t>(gpu * 1000.0);
        ++pictures;
        retraces += n;
        const int before = ctl.Current();
        const unsigned stepsBefore = ctl.Steps();
        const bool aniBefore = ctl.AnisoOn();
        const bool changed = ctl.Add(f);
        t += static_cast<double>(interval) / 1e6;
        if (changed || ctl.Current() != before) ev.push_back({t, 'R', before, ctl.Current(), ctl.LastChange().why});
        if (ctl.Steps() != stepsBefore) ev.push_back({t, ctl.Steps() & 4u ? 'E' : 'e', ctl.Current(), ctl.Current(), ctl.LastFxChange().why});
        if (ctl.AnisoOn() != aniBefore) ev.push_back({t, ctl.AnisoOn() ? 'A' : 'a', ctl.Current(), ctl.Current(), ctl.LastAnisoChange().why});
    }
    void Run(const Load& l, double seconds) {
        const double end = t + seconds;
        while (t < end) Frame(l);
    }
    void Mark() { pictures = retraces = 0; }
    double Rate() const { return retraces ? 60.0 * pictures / retraces : 0.0; }
    unsigned Count(Why why, double after = 0) const {
        unsigned n = 0;
        for (const Event& e : ev) n += e.kind == 'R' && e.why == why && e.t >= after;
        return n;
    }
    unsigned Rungs(double after = 0) const {
        unsigned n = 0;
        for (const Event& e : ev) n += e.kind == 'R' && e.t >= after;
        return n;
    }
    unsigned Ups(double after = 0) const {
        unsigned n = 0;
        for (const Event& e : ev) n += e.kind == 'R' && e.t >= after && e.to > e.from;
        return n;
    }
    // the first change of any kind (rung, step, anisotropic filtering) at or after `after`: its time, or -1
    double FirstChange(double after) const {
        for (const Event& e : ev) if (e.t >= after) return e.t;
        return -1;
    }
    double FirstFast(double after) const {
        for (const Event& e : ev) if (e.t >= after && e.kind == 'R' && e.why == Why::FastDown) return e.t;
        return -1;
    }
    void Print(const char* title) const {
        std::printf("    %s:\n", title);
        for (const Event& e : ev) {
            if (e.kind == 'R') std::printf("      t=%7.2f s  rung %2d -> %2d (%s)\n", e.t, e.from, e.to, WhyText(e.why));
            else std::printf("      t=%7.2f s  %s at rung %d (%s)\n", e.t, e.kind == 'E' ? "edge smoothing given up" : e.kind == 'e' ? "edge smoothing back" :
                             e.kind == 'A' ? "aniso on" : "aniso off", e.from, WhyText(e.why));
        }
    }
};
bool SameEvents(const Sim& a, const Sim& b) {
    if (a.ev.size() != b.ev.size() || a.ctl.Current() != b.ctl.Current() || a.ctl.Steps() != b.ctl.Steps()) return false;
    for (size_t i = 0; i < a.ev.size(); ++i)
        if (a.ev[i].t != b.ev[i].t || a.ev[i].kind != b.ev[i].kind || a.ev[i].from != b.ev[i].from || a.ev[i].to != b.ev[i].to || a.ev[i].why != b.ev[i].why)
            return false;
    return true;
}

// Performance mode as the Vita runs it (vgx_autoq_cadence.cpp Console()), plus the switch.
Config Console(bool fast60, bool steps = true) {
    Config c;
    c.targetFps = 60;
    c.budgetMs = 1000.0 / 60.0;
    c.minRung = 0;
    c.maxRung = kTopSingle;
    c.recover = true;
    c.recoverHoldSec = 20.0;
    c.overload = c.overloadFx = true;
    c.freeFrac = 0.85;
    c.wipe = c.lateRoom = true;
    c.cadenceUp = true;
    if (steps) {
        c.bgStep = c.edgeStep = true;
        c.edgeCheck = true;
        c.aniso = true;
        c.anisoRung = 2;
    }
    c.fast60 = fast60;
    return c;
}
Load Calm() { Load l; l.pixGpuMs = 7; return l; }   // GPU 3 + 7 = 10 ms: 60 with room
Load Spike() { Load l; l.pixGpuMs = 14; return l; } // GPU 3 + 14 = 17 (+ 2.2 edge = 19.2) at 720x404; 664x376 15 ms: holds 60
} // namespace

int main(int argc, char** argv) {
    const bool verbose = argc > 1 && std::string(argv[1]) == "verbose";
    setenv("GXV_WIDE_RUNGS", "1", 1);
    char line[260];

    std::printf("the switch\n");
    {
        Config c = Console(true);
        c.targetFps = 30;
        c.budgetMs = 1000.0 / 30.0;
        Controller q(c);
        Check(!q.Cfg().fast60, "target 30 (Quality, Locked 30): forced off whatever was given");
        Controller p(Console(true));
        Check(p.Cfg().fast60 && p.Cfg().fast60Pics == 12 && p.Cfg().fast60Late == 8, "target 60: on, 8 of the last 12 pictures");
        Config d;
        Check(!d.fast60, "Config's own defaults: off (the back end turns it on for target 60)");
        Config e = Console(true);
        e.fast60Pics = 999;
        e.fast60Late = 0;
        Controller r(e);
        Check(r.Cfg().fast60Pics <= 32 && r.Cfg().fast60Late >= 1, "the numbers are clamped to the ring");
    }

    std::printf("a spike at 720x404 (GPU 12 -> 17 ms of play, 60 at 664x376)\n");
    for (int steps = 0; steps <= 1; ++steps) {
        double reactT = -1, rungT = -1;
        double rate = 0;
        int rung = -9, first = 0;
        bool blocked = false;
        for (int on = 1; on >= 0; --on) {
            Sim s(Console(on != 0, steps != 0), 11, 0.01);
            // 720x404 held with the steps on or off: a calm scene first, so the steps (if any) are as they would be
            Load spike = Spike(), calm = Calm();
            spike.edgeMs = calm.edgeMs = steps ? 2.2 : 0.0;
            s.Run(calm, 20);
            const double t0 = s.t;
            s.Run(spike, 12);
            if (verbose) s.Print(on ? (steps ? "spike, fast60 on, with steps" : "spike, fast60 on, no steps") : "spike, fast60 off");
            if (on) {
                reactT = s.FirstChange(t0) - t0;
                rungT = s.FirstFast(t0) - t0;
                rung = s.ctl.Current();
                first = s.ctl.FastSteps();
                blocked = s.ctl.BlockedUntilUs(2) > s.now;
                s.Mark();
                s.Run(spike, 20);
                rate = s.Rate();
                std::snprintf(line, sizeof line, "%s: first change %.2f s after the spike began, rung at %.2f s (-1: none), now rung %d, %.1f a second after",
                              steps ? "with edge smoothing and anisotropic filtering" : "no steps", reactT, rungT, rung, rate);
                Check(reactT >= 0 && reactT <= 0.5, line);
            } else {
                const double old = s.FirstChange(t0) - t0;
                std::snprintf(line, sizeof line, "switch off: the old pace (first change %.2f s after the spike, if any)", old);
                Check(s.Count(Why::FastDown) == 0 && (old < 0 || old > 0.5), line);
            }
        }
        if (!steps) {
            Check(rungT >= 0 && rungT <= 0.5 && rung <= 1, "the rung itself goes down within 0.5 s");
            Check(rate >= 58.0, "... and the pictures are back on 60");
            Check(blocked, "... the size left is blocked (no climb back into a size that was just too slow)");
        } else {
            Check(first >= 1, "the steps in the way go first (aniso, edge smoothing), as in the other rules, each within 0.5 s");
        }
    }

    std::printf("the order of giving up: anisotropic filtering, edge smoothing, then the size\n");
    {
        Sim s(Console(true), 12, 0.01);
        Load spike = Spike(), calm = Calm();
        spike.edgeMs = calm.edgeMs = 2.2;
        s.Run(calm, 20);
        const double t0 = s.t;
        s.Run(spike, 10);
        if (verbose) s.Print("spike with every step available");
        const int a = [&] { for (size_t i = 0; i < s.ev.size(); ++i) if (s.ev[i].t >= t0 && s.ev[i].kind == 'a') return int(i); return -1; }();
        const int e = [&] { for (size_t i = 0; i < s.ev.size(); ++i) if (s.ev[i].t >= t0 && s.ev[i].kind == 'E') return int(i); return -1; }();
        const int r = [&] { for (size_t i = 0; i < s.ev.size(); ++i) if (s.ev[i].t >= t0 && s.ev[i].kind == 'R') return int(i); return -1; }();
        std::snprintf(line, sizeof line, "indices: aniso off %d, edge smoothing %d, rung %d (-1: not taken; an earlier rule may have taken one)", a, e, r);
        Check(r < 0 || ((a < 0 || a < r) && (e < 0 || e < r)), line);
        // anisotropic filtering on (it comes on after 8 s of room at 720x404): it goes first, then the size (no edge step in this scene)
        Sim t(Console(true), 16, 0.01);
        t.Run(Calm(), 20);
        const double u0 = t.t;
        const bool on = t.ctl.AnisoOn();
        t.Run(Spike(), 10);
        if (verbose) t.Print("spike with anisotropic filtering on");
        const double ta = [&] { for (const Event& ev : t.ev) if (ev.t >= u0 && ev.kind == 'a') return ev.t - u0; return -1.0; }();
        const double tr = t.FirstFast(u0) - u0;
        std::snprintf(line, sizeof line, "aniso was on: off at %.2f s, the rung at %.2f s after the spike began", ta, tr);
        Check(on && ta >= 0 && ta <= 0.5 && tr > ta, line);
    }

    std::printf("late for the CPU stages, the steady 30, a quiet moment or a movie: no step\n");
    {
        struct Case { const char* name; Load l; };
        Load game = Calm(); game.turnMs = 24;                   // the game's CPU over the budget; the GPU 10 ms
        Load gameAndGpu = Spike(); gameAndGpu.turnMs = 24;       // the GPU late too, but the game alone is over the budget
        Load frontL = Calm(); frontL.frontMs = 25;
        Load cad = Spike(); cad.cadence = true;                 // the pacing's steady 30
        Load quiet = Spike(); quiet.quiet = true;
        Load movie = Spike(); movie.movie = true;
        Load backendL = Calm(); backendL.backendMs = 22;
        const Case cases[] = {{"the game's CPU 24 ms (GPU 10)", game}, {"the game's CPU 24 ms and the GPU 17 (the game alone over)", gameAndGpu},
                              {"the front end 25 ms", frontL}, {"the renderer's CPU 22 ms", backendL}, {"the steady 30 (GPU 17 ms)", cad},
                              {"a quiet moment (pause menu, fade) with the GPU 17 ms", quiet}, {"a movie with the GPU 17 ms", movie}};
        for (const Case& c : cases) {
            Sim s(Console(true), 13, 0.01);
            s.Run(Calm(), 10);
            const double t0 = s.t;
            s.Run(c.l, 30);
            if (verbose) s.Print(c.name);
            std::snprintf(line, sizeof line, "%s: %u fast steps, %u rung changes of any kind", c.name, s.ctl.FastSteps(), s.Rungs(t0));
            Check(s.ctl.FastSteps() == 0 && s.Count(Why::FastDown) == 0, line);
        }
        // pictures late for the GPU only now and then (3 of 12) do not make a burst
        Sim s(Console(true), 14, 0.01);
        s.Run(Calm(), 10);
        Load spiky = Spike();
        for (int i = 0; i < 40; ++i) { s.Run(Calm(), 0.15); s.Frame(spiky); s.Frame(spiky); s.Frame(spiky); }
        Check(s.ctl.FastSteps() == 0, "3 late pictures in every 12: not a burst");
    }

    std::printf("no oscillation\n");
    {
        // a spike of 1.5 s, then calm: down, and the size left stays blocked for the climb (60 s); no step down again
        Sim s(Console(true, false), 15, 0.01);
        s.Run(Calm(), 10);
        const double t0 = s.t;
        s.Run(Spike(), 1.5);
        s.Run(Calm(), 100);
        if (verbose) s.Print("a 1.5 s spike, then calm for 100 s");
        const unsigned downs = s.Count(Why::FastDown, t0);
        const double firstUp = [&] { for (const Event& e : s.ev) if (e.kind == 'R' && e.t >= t0 && e.to > e.from) return e.t - t0; return 1e9; }();
        std::snprintf(line, sizeof line, "%u fast step(s), the first climb back %.0f s after the spike (the block is 60 s)", downs, firstUp);
        Check(downs >= 1 && downs <= 2 && firstUp >= 55.0 && firstUp <= 100.0, line);
        Check(s.ctl.BlockedUntilUs(2) > 0, "the size left was blocked");
    }
    for (int seed = 0; seed < 4; ++seed) {
        // a borderline scene: GPU around the 16.7 ms edge for 3 minutes
        Load b = Spike();
        b.pixGpuMs = 13.2;
        Sim s(Console(true, false), 20 + seed, 0.06);
        s.Run(Calm(), 10);
        const double t0 = s.t;
        s.Run(b, 180);
        if (verbose && seed == 0) s.Print("borderline scene, GPU 16.2 +-6 % at 720x404");
        // direction changes: a climb followed by a step down within 60 s is an oscillation
        bool cycle = false;
        double lastUp = -1e9;
        for (const Event& e : s.ev) {
            if (e.kind != 'R' || e.t < t0) continue;
            if (e.to > e.from) lastUp = e.t;
            else if (e.t - lastUp < 60.0) cycle = true;
        }
        std::snprintf(line, sizeof line, "seed %d: %u rung changes in 3 minutes (%u fast, %u up), no climb-then-drop within 60 s", seed, s.Rungs(t0), s.Count(Why::FastDown, t0), s.Ups(t0));
        Check(!cycle && s.Rungs(t0) <= 5, line);
    }
    {
        // the repeat: the same spike again and again; each block doubles (60, 120, ...) so the number of drops stays small
        Sim s(Console(true, false), 24, 0.01);
        for (int i = 0; i < 6; ++i) { s.Run(Calm(), 30); s.Run(Spike(), 2); }
        s.Run(Calm(), 30);
        if (verbose) s.Print("six 2 s spikes, 30 s apart");
        std::snprintf(line, sizeof line, "six spikes: %u fast steps, %u climbs", s.Count(Why::FastDown), s.Ups());
        Check(s.Count(Why::FastDown) <= 6 && s.Ups() <= 4, line);
    }

    std::printf("several rungs in a row, the floor rung, and a floor that cannot hold 55\n");
    {
        Load heavy = Spike();
        heavy.pixGpuMs = 24;   // 720x404: 27 ms, 664x376 23.5, 608x348 20.4: the floor does not hold 55
        Load fits = Spike();
        fits.pixGpuMs = 16.5;  // 720x404: 19.5, 664x376 17.1, 608x348 15: holds only at the floor
        Sim s(Console(true, false), 30, 0.005);
        s.Run(Calm(), 10);
        const double t0 = s.t;
        s.Run(fits, 12);
        if (verbose) s.Print("a GPU that holds 60 only at the floor");
        Check(s.ctl.Current() == 0 && s.Count(Why::FastDown, t0) == 2, "two fast steps in a row, 720x404 -> 664x376 -> 608x348, then the floor");
        s.Mark();
        s.Run(fits, 20);
        Check(s.Rate() >= 58.0, "... and the pictures are back on 60 there");
        Sim h(Console(true, false), 31, 0.005);
        h.Run(Calm(), 10);
        const double h0 = h.t;
        h.Run(heavy, 30);
        if (verbose) h.Print("a GPU that cannot hold 60 even at the floor");
        std::snprintf(line, sizeof line, "the floor cannot hold 55 (GPU %.1f ms there): %u fast steps (the steady 30 is its case)", 3 + 24 * Cost(0), h.Count(Why::FastDown, h0));
        Check(h.Count(Why::FastDown, h0) == 0 && 3 + 24 * Cost(0) > 1000.0 / 55.0, line);
    }

    std::printf("Quality (target 30) and calm Performance traces: the same decisions with and without the switch\n");
    {
        // target 30: the switch is forced off, so a trace decides identically
        auto q30 = [&](bool on) {
            Config c = Console(on, true);
            c.targetFps = 30;
            c.budgetMs = 1000.0 / 30.0;
            c.minRung = 0;
            Sim s(c, 40, 0.03);
            Load l = Calm();
            s.Run(l, 20);
            l.pixGpuMs = 30;
            s.Run(l, 40);
            l.pixGpuMs = 8;
            s.Run(l, 60);
            return s;
        };
        Sim a = q30(true), b = q30(false);
        std::snprintf(line, sizeof line, "Quality (target 30), a heavy stretch between calm ones: %zu changes, identical with the switch on and off", a.ev.size());
        Check(SameEvents(a, b) && a.ctl.FastSteps() == 0 && a.Count(Why::FastDown) == 0, line);
        // target 60 without a late picture: identical
        auto calm60 = [&](bool on) {
            Sim s(Console(on, true), 41, 0.03);
            s.Run(Calm(), 120);
            return s;
        };
        Sim c = calm60(true), d = calm60(false);
        Check(SameEvents(c, d), "Performance, a calm scene (no late picture): identical with the switch on and off");
        // target 60 with the CPU late: identical
        auto cpu60 = [&](bool on) {
            Sim s(Console(on, true), 42, 0.03);
            Load l = Calm();
            l.turnMs = 24;
            s.Run(l, 60);
            return s;
        };
        Sim e = cpu60(true), f = cpu60(false);
        Check(SameEvents(e, f), "Performance, the game's CPU over the budget: identical with the switch on and off");
    }

    std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all checks passed", g_failures);
    return g_failures ? 1 : 0;
}
