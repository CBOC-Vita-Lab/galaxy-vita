// Host-side test of Performance mode, the 60 target (GXV_AUTOQ_TARGET=60, Config::targetFps 60) of the automatic quality
// controller (platform/vgx/src/vgx_autoq.cpp). tools/test_vgx_autoq_60.sh builds and runs it.
//
// The pipeline model runs at 60 Hz: the game takes one turn a retrace, a picture is shown at the first retrace after the
// largest of its stages (one game turn's CPU, the renderer's CPU, the GPU's busy time) is done, so it takes one retrace when all
// of them fit 16.7 ms, two when one of them needs up to 33 ms, and so on.
// 1. A GPU-limited scene steps down until its pictures fit 16.7 ms; a heavier one reaches 480x340.
// 2. Half-size effects and the cheaper background come before any smaller picture; when they are enough the size stays.
// 3. A game-CPU-limited scene (or one where the GPU is not clearly the largest stage) keeps its size and its steps.
// 4. The way back: room at 16.7 ms brings the picture back to 640x456 (the steps off first); above it only with clear room.
// 5. A smaller picture that buys no GPU time is undone, as in target 30.
// 6. The same scene that target 30 keeps at 640x456 (it fits 33 ms) is lowered by target 60, and the configuration itself.
#include "../platform/vgx/src/vgx_autoq.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
    double turnMs = 8, backendMs = 4, fixedGpuMs = 2, pixGpuMs = 18; // pixGpuMs at 640x456 (Cost 1); one game turn's CPU
    double fxGain = 1.0, bgGain = 1.0;                                // GPU time without the step / with it
};
struct Event {
    double t;
    char kind; // 'R' rung, 'F'/'f' half-size effects on/off, 'B'/'b' cheaper background on/off
    int from, to;
    Why why;
};
constexpr double kRetraceUs = 16683.0;

struct Sim {
    Controller ctl;
    std::mt19937 rng;
    double noise;
    uint64_t now = 1, backend = 1;
    uint32_t game = 1, turns = 1;
    double t = 0;
    std::vector<Event> ev;
    unsigned stepsPrev = 0;
    // pictures and retraces since Mark() (the picture rate of a stretch)
    unsigned pictures = 0, retraces = 0;
    Sim(const Config& c, uint32_t seed, double n) : ctl(c), rng(seed), noise(n) {}
    void Frame(const Load& l, bool quiet = false, uint32_t scene = 0, uint64_t forceIntervalUs = 0) {
        std::uniform_real_distribution<double> u(-noise, noise);
        const double tms = l.turnMs * (1.0 + u(rng)), bms = l.backendMs * (1.0 + u(rng));
        const double gpu = (l.fixedGpuMs + l.pixGpuMs * Cost(ctl.Current())) * (1.0 + u(rng)) / (ctl.FxStepOn() ? l.fxGain : 1.0) /
                           (ctl.BgStepOn() ? l.bgGain : 1.0);
        const double work = std::max({tms, bms, gpu});
        const unsigned n = forceIntervalUs ? 1u : static_cast<unsigned>(std::max(1.0, std::ceil(work * 1000.0 / kRetraceUs - 1e-9)));
        const uint64_t interval = forceIntervalUs ? forceIntervalUs : static_cast<uint64_t>(n * kRetraceUs);
        game += static_cast<uint32_t>(tms * 1000.0) * n; // (one turn a retrace)
        turns += n;
        backend += static_cast<uint32_t>(bms * 1000.0);
        FrameTimes f;
        f.startUs = now;
        now += interval;
        f.presentUs = now;
        f.backendRunUs = backend;
        f.gameRunUs = game;
        f.gameTurns = turns;
        f.gameKnown = true;
        f.timers = f.busyKnown = true;
        f.gpuBusyUs = static_cast<uint32_t>(gpu * 1000.0);
        f.quiet = quiet;
        f.scene = scene;
        ++pictures;
        retraces += n;
        const int before = ctl.Current();
        const bool changed = ctl.Add(f);
        t += static_cast<double>(interval) / 1e6;
        if (changed || ctl.Current() != before) ev.push_back({t, 'R', before, ctl.Current(), ctl.LastChange().why});
        if (ctl.Steps() != stepsPrev) {
            for (unsigned bit = 1; bit <= 2; bit <<= 1) {
                const bool was = (stepsPrev & bit) != 0, is = (ctl.Steps() & bit) != 0;
                if (was != is) ev.push_back({t, bit == 1 ? (is ? 'F' : 'f') : (is ? 'B' : 'b'), ctl.Current(), ctl.Current(), ctl.LastFxChange().why});
            }
            stepsPrev = ctl.Steps();
        }
    }
    void Run(const Load& l, double seconds, double pauseEvery = 0, uint32_t scene = 0) {
        const double end = t + seconds, start = t;
        while (t < end) Frame(l, pauseEvery > 0 && std::fmod(t - start, pauseEvery) >= pauseEvery - 2.0, scene);
    }
    void Mark() { pictures = retraces = 0; }
    double Rate() const { return retraces ? 60.0 * pictures / retraces : 0.0; } // pictures a second (of 60 retraces)
    int Index(char kind, double after = -1, int to = 9999) const {
        for (size_t i = 0; i < ev.size(); ++i)
            if (ev[i].t >= after && ev[i].kind == kind && (to == 9999 || ev[i].to == to)) return static_cast<int>(i);
        return -1;
    }
    unsigned Count(char kind) const {
        unsigned n = 0;
        for (const Event& e : ev) n += e.kind == kind;
        return n;
    }
    bool Any(Why why) const {
        for (const Event& e : ev)
            if (e.kind == 'R' && e.why == why) return true;
        return false;
    }
    void Print(const char* title) const {
        std::printf("    %s:\n", title);
        for (const Event& e : ev) {
            if (e.kind == 'R') std::printf("      t=%7.1f s  rung %2d -> %2d (%s)\n", e.t, e.from, e.to, WhyText(e.why));
            else std::printf("      t=%7.1f s  %s %s at rung %d (%s)\n", e.t, e.kind == 'F' || e.kind == 'f' ? "half-size effects and blur" : "cheaper background          ",
                             e.kind == 'F' || e.kind == 'B' ? "ON " : "off", e.from, WhyText(e.why));
        }
    }
};

// The console's defaults for the mode: target 60, calm, recovery after 10 s of room, both steps.
Config Perf(bool steps = true, int target = 60) {
    Config c;
    c.targetFps = target;
    c.minRung = kEmergency;
    c.budgetMs = 1000.0 / target;
    c.recover = true;
    c.recoverHoldSec = 10.0;
    c.fxStep = c.bgStep = steps;
    return c;
}
} // namespace

int main(int argc, char** argv) {
    const bool verbose = argc > 1 && std::string(argv[1]) == "verbose";

    std::printf("the configuration\n");
    {
        Config c;
        c.targetFps = 60;
        c.minRung = kEmergency;
        Controller ctl(c);
        Check(std::abs(ctl.Cfg().budgetMs - 1000.0 / 60.0) < 1e-9 && ctl.Cfg().targetFps == 60, "target 60: the budget is 16.7 ms whatever was given");
        Check(ctl.Cfg().minRung == kEmergency, "target 60: the 480x340 emergency rung is allowed");
        Config other;
        other.targetFps = 45;
        other.minRung = kEmergency;
        Controller off(other);
        Check(off.Cfg().targetFps == 0 && off.Cfg().minRung == 0, "an unsupported target (45) is the old controller");
    }

    std::printf("a GPU-limited scene steps down until it fits 16.7 ms\n");
    {
        Load l;              // GPU 20 ms at 640x456, 17.4 at 592x422, 15.0 at 544x388; game turn 8 ms
        Sim s(Perf(false), 1, 0.04);
        s.Run(l, 60);
        if (verbose) s.Print("GPU 2 + 18 x Cost ms, no steps");
        s.Mark();
        s.Run(l, 30);
        char line[200];
        std::snprintf(line, sizeof line, "the picture goes down to 544x388 and no further (rung %d), %.1f pictures a second afterwards", s.ctl.Current(), s.Rate());
        Check(s.ctl.Current() == 0 && s.Index('R', 0, kEmergency) < 0, line);
        Check(s.Rate() >= 57.0, "... where it shows (nearly) 60");
        Check(!s.Any(Why::NoGain) && s.Count('R') <= 3, "... each step kept, no oscillation");
    }
    {
        Load heavy;          // GPU 32 ms at 640x456, 18.8 ms at 480x340: the emergency rung, still over
        heavy.pixGpuMs = 30;
        Sim s(Perf(false), 2, 0.04);
        s.Run(heavy, 90);
        if (verbose) s.Print("GPU 2 + 30 x Cost ms, no steps");
        Check(s.ctl.Current() == kEmergency && !s.Any(Why::NoGain), "a much heavier GPU scene reaches 480x340, each step kept");
    }

    std::printf("half-size effects and the cheaper background before a smaller picture\n");
    {
        Load l;
        l.fxGain = 1.12;     // 20 -> 17.9 ms: not enough ...
        l.bgGain = 1.15;     // ... with the background 15.5 ms: enough
        Sim s(Perf(true), 3, 0.03);
        s.Run(l, 90);
        if (verbose) s.Print("GPU 20 ms, the steps are enough");
        const int fx = s.Index('F'), bg = s.Index('B');
        Check(fx >= 0 && bg > fx, "half-size effects first, then the cheaper background");
        Check(s.Count('R') == 0 && s.ctl.Current() == kToday && s.ctl.FxStepOn() && s.ctl.BgStepOn(), "... and the picture keeps 640x456");
    }
    {
        Load l;              // the steps buy nothing: tried, undone, then the sizes
        Sim s(Perf(true), 4, 0.03);
        s.Run(l, 90);
        if (verbose) s.Print("GPU 20 ms, the steps buy nothing");
        const int undoB = s.Index('b'), down = s.Index('R', 0, 1);
        Check(s.Count('F') >= 1 && undoB >= 0 && down > undoB, "steps that buy nothing are undone before the first smaller picture");
        Check(s.ctl.Current() == 0, "... which then goes down to 544x388");
    }

    std::printf("a CPU-limited scene keeps its size\n");
    {
        struct Case { const char* name; Load l; };
        Load game;           // one game turn 22 ms, GPU 14 ms: the game is the limit
        game.turnMs = 22; game.pixGpuMs = 12;
        Load close;          // game 20 ms, GPU 19.7 ms: the GPU is not the largest by 5 %
        close.turnMs = 20; close.pixGpuMs = 17.7;
        Load renderer;       // renderer CPU 21 ms, GPU 18 ms
        renderer.backendMs = 21; renderer.pixGpuMs = 16;
        const Case cases[] = {{"game turn 22 ms, GPU 14 ms", game}, {"game turn 20 ms, GPU 19.7 ms", close}, {"renderer CPU 21 ms, GPU 18 ms", renderer}};
        for (const Case& c : cases) {
            Sim s(Perf(true), 5, 0.03);
            s.Run(c.l, 120, 30);
            char line[200];
            std::snprintf(line, sizeof line, "%s: no rung change, no step (%.1f pictures a second)", c.name, s.Rate());
            Check(s.ev.empty() && s.ctl.Current() == kToday, line);
        }
    }

    std::printf("the way back\n");
    {
        Load heavy;
        heavy.pixGpuMs = 30;
        heavy.fxGain = 1.1;
        heavy.bgGain = 1.15;
        Load light;          // GPU 2 + 6 x Cost: 8 ms at 640x456, 9 ms at 720x480, 12.7 ms at 960x544
        light.pixGpuMs = 6;
        light.turnMs = 6;
        Sim s(Perf(true), 6, 0.03);
        s.Run(heavy, 90);
        const int low = s.ctl.Current();
        const bool stepsOn = s.ctl.FxStepOn() && s.ctl.BgStepOn();
        const double lightFrom = s.t;
        s.Run(light, 240);
        if (verbose) s.Print("heavy 90 s, light 240 s");
        int reachedToday = -1, firstAbove = -1;
        for (size_t i = 0; i < s.ev.size(); ++i) {
            if (s.ev[i].t < lightFrom || s.ev[i].kind != 'R') continue;
            if (reachedToday < 0 && s.ev[i].to == kToday) reachedToday = static_cast<int>(i);
            if (firstAbove < 0 && s.ev[i].to > kToday) firstAbove = static_cast<int>(i);
        }
        const int bgOff = s.Index('b', lightFrom), fxOff = s.Index('f', lightFrom);
        Check(low <= 0 && stepsOn, "setup: the heavy stretch ends at 544x388 or lower with both steps on");
        Check(reachedToday >= 0 && s.ev[reachedToday].t - lightFrom <= 90.0, "with room at 16.7 ms the picture climbs back to 640x456 (within 90 s)");
        Check(bgOff > reachedToday && fxOff > bgOff, "... then the cheaper background goes off, then the half-size effects");
        Check(firstAbove > fxOff, "... and only then a size above 640x456");
        Check(s.ctl.Current() > kToday && s.ctl.Steps() == 0, "in the end: above 640x456 (clear room at 16.7 ms), steps off");
    }
    {
        Load moderate;       // GPU 11 ms at 640x456: fits 60, but 720x480 would be 13 ms, over 70 % of 16.7 ms
        moderate.pixGpuMs = 9;
        Sim s(Perf(true), 7, 0.03);
        s.Run(moderate, 200, 30);
        if (verbose) s.Print("GPU 11 ms");
        Check(s.ev.empty() && s.ctl.Current() == kToday, "a scene that fits 60 without clear room keeps 640x456 (no size above it)");
    }

    std::printf("a smaller picture that buys no GPU time is undone\n");
    {
        Load flat;           // 19 ms at every size
        flat.fixedGpuMs = 17;
        flat.pixGpuMs = 2;
        Sim s(Perf(true), 8, 0.03);
        s.Run(flat, 120);
        if (verbose) s.Print("flat GPU time 19 ms");
        const int down = s.Index('R', 0, 1);
        Check(down >= 0 && down + 1 < static_cast<int>(s.ev.size()) && s.ev[down + 1].why == Why::NoGain, "the step below 640x456 is undone for no gain");
        Check(s.Index('R', 0, 0) < 0 && s.ctl.Current() == kToday, "... and the picture never goes lower, it ends at 640x456");
    }

    std::printf("Quality mode (30) and Performance mode (60) on the same scene\n");
    {
        Load l;              // GPU 20 ms at 640x456: fits 33 ms, not 16.7 ms
        Sim q(Perf(true, 30), 9, 0.03), p(Perf(true, 60), 9, 0.03);
        q.Run(l, 60);
        p.Run(l, 60);
        q.Mark();
        p.Mark();
        q.Run(l, 30);
        p.Run(l, 30);
        char line[200];
        std::snprintf(line, sizeof line, "target 30 keeps 640x456 at %.1f pictures a second; target 60 goes to rung %d at %.1f", q.Rate(), p.ctl.Current(), p.Rate());
        Check(q.ev.empty() && q.ctl.Current() == kToday && p.ctl.Current() < kToday && p.Rate() > q.Rate() + 20.0, line);
    }

    std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all checks passed", g_failures);
    return g_failures ? 1 : 0;
}
