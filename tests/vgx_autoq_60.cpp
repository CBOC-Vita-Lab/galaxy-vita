// Host-side test of Performance mode, the 60 target (GXV_AUTOQ_TARGET=60, Config::targetFps 60) of the automatic quality
// controller (platform/vgx/src/vgx_autoq.cpp). tools/test_vgx_autoq_60.sh builds and runs it.
//
// The pipeline model runs at 60 Hz: the game takes one turn a retrace, a picture is shown at the first retrace after the
// largest of its stages (one game turn's CPU, the renderer's CPU, the GPU's busy time) is done, so it takes one retrace when all
// of them fit 16.7 ms, two when one of them needs up to 33 ms, and so on.
// 1. A GPU-limited scene steps down until its pictures fit 16.7 ms; a heavier one reaches 480x340.
// 2. Cheaper background then half-size effects come before any smaller picture; when they are enough the size stays.
// 3. A game-CPU-limited scene (or one where the GPU is not clearly the largest stage) keeps its size and its steps.
// 4. The way back: room at 16.7 ms brings the picture back to 640x456 (the steps off first); above it only with clear room.
// 5. A smaller picture that buys no GPU time is undone, as in target 30.
// 6. The same scene that target 30 keeps at 640x456 (it fits 33 ms) is lowered by target 60, and the configuration itself.
// 8. Performance mode's additions (Settings): the present's edge smoothing as the first step (GXV_AUTOQ_EDGESTEP) and the last one back;
//    a window on the pacing's steady 30 (GXV_STEADY30, FrameTimes::cadence30) judged against 33.3 ms, the floor census with it; the
//    stage hint the steady 30 reads (port_autoq_stages); and the console's whole configuration on scenes of today's measured kinds.
// 7. Pictures, not GPU time: a series of steps below is kept only when it shows more pictures (the console's Comet Observatory:
//    the front end at about 26 ms a picture held 30, the GPU at 20 ms looked like the limit, and Performance mode went down to
//    544x388 / 480x340 for nothing). With the front end measured as a stage nothing moves; without it the series is undone.
//    A scene whose GPU time falls but whose pictures do not come faster goes back; one that gains pictures goes no lower than
//    the size that gained them; below 640x456 a scene another stage holds climbs back.
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
    double frontMs = 0;      // the front end's CPU a picture (GX stream parse, draw building: its own thread)
    bool frontKnown = true;  // ... measured (stamped at each picture)
    double edgeMs = 0;       // the present's edge smoothing (GXV_EDGE_AA): GPU time a picture it costs, whatever the size
    bool cadence = false;    // the pacing's steady 30 (GXV_STEADY30): a picture every second retrace at most
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
    uint32_t game = 1, turns = 1, front = 1, frontPictures = 1;
    double t = 0;
    std::vector<Event> ev;
    unsigned stepsPrev = 0;
    // pictures and retraces since Mark() (the picture rate of a stretch)
    unsigned pictures = 0, retraces = 0;
    uint64_t freezeUs = 0;   // the interval of the picture after a change of rung (the console: new render targets)
    bool freezeNext = false;
    Sim(const Config& c, uint32_t seed, double n) : ctl(c), rng(seed), noise(n) {}
    void Frame(const Load& l, bool quiet = false, uint32_t scene = 0, uint64_t forceIntervalUs = 0) {
        std::uniform_real_distribution<double> u(-noise, noise);
        const double tms = l.turnMs * (1.0 + u(rng)), bms = l.backendMs * (1.0 + u(rng));
        const double gpu = (l.fixedGpuMs + l.pixGpuMs * Cost(ctl.Current())) * (1.0 + u(rng)) / (ctl.FxStepOn() ? l.fxGain : 1.0) /
                           (ctl.BgStepOn() ? l.bgGain : 1.0) + (ctl.EdgeStepOn() ? 0.0 : l.edgeMs);
        const double fms = l.frontMs * (1.0 + u(rng));
        const double work = std::max({tms, bms, gpu, fms});
        if (freezeNext && !forceIntervalUs) forceIntervalUs = freezeUs;
        freezeNext = false;
        const unsigned n = forceIntervalUs ? 1u : static_cast<unsigned>(std::max(l.cadence ? 2.0 : 1.0, std::ceil(work * 1000.0 / kRetraceUs - 1e-9)));
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
        front += static_cast<uint32_t>(fms * 1000.0);
        ++frontPictures;
        f.frontRunUs = front;
        f.frontPictures = frontPictures;
        f.frontKnown = l.frontKnown;
        f.cadence30 = l.cadence;
        f.timers = f.busyKnown = true;
        f.gpuBusyUs = static_cast<uint32_t>(gpu * 1000.0);
        f.quiet = quiet;
        f.scene = scene;
        ++pictures;
        retraces += n;
        const int before = ctl.Current();
        const bool changed = ctl.Add(f);
        t += static_cast<double>(interval) / 1e6;
        if (changed || ctl.Current() != before) {
            ev.push_back({t, 'R', before, ctl.Current(), ctl.LastChange().why});
            freezeNext = freezeUs > 0 && ctl.Current() != before;
        }
        if (ctl.Steps() != stepsPrev) {
            for (unsigned bit = 1; bit <= 4; bit <<= 1) {
                const bool was = (stepsPrev & bit) != 0, is = (ctl.Steps() & bit) != 0;
                if (was != is) ev.push_back({t, bit == 1 ? (is ? 'F' : 'f') : bit == 2 ? (is ? 'B' : 'b') : (is ? 'E' : 'e'), ctl.Current(), ctl.Current(), ctl.LastFxChange().why});
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
            else std::printf("      t=%7.1f s  %s %s at rung %d (%s)\n", e.t, e.kind == 'F' || e.kind == 'f' ? "half-size effects and blur" :
                             e.kind == 'E' || e.kind == 'e' ? "edge smoothing given up     " : "cheaper background          ",
                             e.kind == 'F' || e.kind == 'B' || e.kind == 'E' ? "ON " : "off", e.from, WhyText(e.why));
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
// The Settings' Performance mode as the Vita runs it (entry.cpp defaults + settings_launch.cpp): target 60, sizes 608x346 (rung 0, wide
// ladder) to 960x544 (rung 6), the present's edge smoothing and the cheaper background as steps (GXV_AUTOQ_FXSTEP=3 + EDGESTEP), recovery
// after 20 s of room, play-time quality (FREE 85, WIPE, LATEROOM), severe overloads, the front end a stage.
Config Console() {
    Config c = Perf(false);
    c.minRung = 0;
    c.maxRung = kTopSingle;
    c.bgStep = true;
    c.edgeStep = true;
    c.overload = c.overloadFx = true;
    c.recoverHoldSec = 20.0;
    c.freeFrac = 0.85;
    c.wipe = c.lateRoom = true;
    return c;
}
} // namespace

extern "C" int port_autoq_stages(uint32_t* gpuUs, uint32_t* pipeUs, int* lowerable);

int main(int argc, char** argv) {
    const bool verbose = argc > 1 && std::string(argv[1]) == "verbose";
    setenv("GXV_AUTOQ_TARGET", "60", 1);  // (port_autoq_stages answers in target 60 only; read once)

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
        Load heavy;          // GPU 38 ms at 640x456 (20 a second), 32.8 at 592x422 (30), 28 at 544x388, 22 at 480x340 (30 still)
        heavy.pixGpuMs = 36;
        Sim s(Perf(false), 2, 0.03);
        s.Run(heavy, 120);
        if (verbose) s.Print("GPU 2 + 36 x Cost ms, no steps");
        s.Mark();
        s.Run(heavy, 20);
        char line[200];
        std::snprintf(line, sizeof line, "a much heavier GPU scene: the sizes that gain pictures are kept, 480x340 that gains none goes back (rung %d, %.1f a second)",
                      s.ctl.Current(), s.Rate());
        const int em = s.Index('R', 0, kEmergency);
        Check((s.ctl.Current() == 0 || s.ctl.Current() == 1) && s.Rate() >= 29.0 && (em < 0 || (em + 1 < static_cast<int>(s.ev.size()) && s.ev[em + 1].why == Why::NoGain)), line);
    }
    {
        Load stuck;          // GPU 32 ms at 640x456, 18.8 ms at 480x340: 30 a second at every size
        stuck.pixGpuMs = 30;
        Sim s(Perf(false), 2, 0.03);
        s.Run(stuck, 150);
        if (verbose) s.Print("GPU 2 + 30 x Cost ms: 30 a second at every size");
        const int back = s.Index('R', 0, kToday);
        Check(s.ev.empty() || (back >= 0 && s.ev[back].why == Why::NoGain), "a GPU scene no size makes faster (30 a second at every size): no step down, or one undone");
        // (this model shows a picture only at whole retraces; the console's pipeline is throughput-bound (a GPU at 24 ms gives ~41 a
        // second), so a step is tried when the smallest size could show more, and the series is undone when it did not: once a visit)
        Check(s.Count('R') <= 4 && s.ctl.Current() == kToday, "... it ends at 640x456: one series at most, undone, not tried again in the visit (no flicker)");
    }
    {
        // Beach Bowl's kind in Performance mode: each size saves 5-6 % (12.5 + 6.5 x Cost: 19 ms at 640x456, 16.1 ms at 480x340)
        Load beach;
        beach.turnMs = 6;
        beach.backendMs = 3;
        beach.fixedGpuMs = 12.5;
        beach.pixGpuMs = 6.5;
        Sim s(Perf(false), 10, 0.03);
        s.Run(beach, 90);
        if (verbose) s.Print("5-6 % a size, 480x340 fits 16.7 ms");
        s.Mark();
        s.Run(beach, 20);
        char line[200];
        std::snprintf(line, sizeof line, "small savings: down to 480x340, which shows %.1f a second", s.Rate());
        Check(s.ctl.Current() == kEmergency && !s.Any(Why::NoGain) && s.Rate() >= 50.0, line);
    }

    std::printf("cheaper background and half-size effects before a smaller picture\n");
    {
        Load l;
        l.bgGain = 1.15;     // 20 -> 17.4 ms: not enough ...
        l.fxGain = 1.12;     // ... with half-size effects 15.5 ms: enough
        Sim s(Perf(true), 3, 0.03);
        s.Run(l, 90);
        if (verbose) s.Print("GPU 20 ms, the steps are enough");
        const int fx = s.Index('F'), bg = s.Index('B');
        Check(bg >= 0 && fx > bg, "cheaper background first, then half-size effects");
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
        Check(fxOff > reachedToday && bgOff > fxOff, "... then half-size effects go off, then the cheaper background");
        Check(firstAbove > bgOff, "... and only then a size above 640x456");
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

    std::printf("pictures, not GPU time: a front-end-limited scene\n");
    {
        // The Comet Observatory as the console showed it: the front end about 26 ms a picture, the GPU 20 ms at 640x456.
        Load obs;
        obs.frontMs = 26;
        obs.pixGpuMs = 18;
        obs.turnMs = 9;
        for (int measured = 1; measured >= 0; --measured) {
            obs.frontKnown = measured != 0;
            Sim s(Perf(true), 11, 0.04);
            s.freezeUs = 260000;
            s.Run(obs, 180, 40);
            if (verbose) s.Print(measured ? "front end 26 ms (measured), GPU 20 ms" : "front end 26 ms (not measured), GPU 20 ms");
            char line[220];
            if (measured) {
                std::snprintf(line, sizeof line, "front end measured: it is the largest stage, no step and no size change (%.1f a second)", s.Rate());
                Check(s.ev.empty() && s.ctl.Current() == kToday, line);
            } else {
                double below = 0, lastDown = -1;
                for (const Event& e : s.ev)
                    if (e.kind == 'R' && e.to < kToday && e.from == kToday) lastDown = e.t;
                for (size_t i = 0; i + 1 < s.ev.size(); ++i)
                    if (s.ev[i].kind == 'R' && s.ev[i].to < kToday) {
                        for (size_t j = i + 1; j < s.ev.size(); ++j)
                            if (s.ev[j].kind == 'R') { if (s.ev[j].to == kToday) below += s.ev[j].t - s.ev[i].t; break; }
                    }
                std::snprintf(line, sizeof line, "front end not measured: each series of steps below is undone back to 640x456 (%u changes, last series at %.0f s)",
                              s.Count('R'), lastDown);
                Check(s.ctl.Current() == kToday && s.Count('R') <= 6 && s.Index('R', 0, kEmergency) < 0, line);
            }
        }
    }
    {
        // Below 640x456 for a GPU-limited stretch, then a scene the front end holds (26 ms) with a GPU of 16 ms at 640x456: the
        // smaller picture buys nothing there, the picture climbs back to 640x456 (and not above: that needs room at 16.7 ms).
        Load gpu;
        gpu.pixGpuMs = 36;   // 592x422 gains pictures (see above)
        Load fe;
        fe.frontMs = 26;
        fe.pixGpuMs = 14;
        Sim s(Perf(false), 12, 0.03);
        s.Run(gpu, 60);
        const int low = s.ctl.Current();
        const double from = s.t;
        s.Run(fe, 60);
        if (verbose) s.Print("GPU-limited 60 s, then front-end-limited 60 s");
        const int up = s.Index('R', from, kToday);
        Check(low < kToday && up >= 0 && s.ev[up].why == Why::Headroom && s.ev[up].t - from <= 25.0,
              "a scene held by the front end below 640x456 climbs back to 640x456 within 25 s");
        Check(s.ctl.Current() == kToday, "... and no higher");
    }


    std::printf("Performance mode's edge smoothing: the first step, the last one back\n");
    {
        Load l;              // GPU 2 + 15 x Cost + 2.2 ms edge smoothing = 19.2 ms at 640x456; without it 17.0, with the background 15.5
        l.pixGpuMs = 15;
        l.edgeMs = 2.2;
        l.bgGain = 1.1;
        Config c = Perf(false);
        c.edgeStep = c.bgStep = true;
        Sim s(c, 21, 0.02);
        s.Run(l, 90);
        if (verbose) s.Print("GPU 19.2 ms with edge smoothing");
        const int edge = s.Index('E'), bg = s.Index('B');
        char line[200];
        std::snprintf(line, sizeof line, "edge smoothing goes first (event %d), then the cheaper background (event %d); the picture keeps 640x456 (rung %d)", edge, bg,
                      s.ctl.Current());
        Check(edge >= 0 && bg > edge && s.Count('R') == 0 && s.ctl.Current() == kToday && s.ctl.EdgeStepOn() && s.ctl.BgStepOn(), line);
        Load light = l;      // then a light stretch: back in the reverse order, edge smoothing last
        light.pixGpuMs = 5;
        light.turnMs = 6;
        const double from = s.t;
        s.Run(light, 120);
        if (verbose) s.Print("then GPU 9 ms");
        const int bgOff = s.Index('b', from), edgeOff = s.Index('e', from);
        Check(bgOff >= 0 && edgeOff > bgOff && !s.ctl.EdgeStepOn(), "with room again the cheaper background goes off first, edge smoothing comes back last");
        Config off = Perf(false);
        off.bgStep = true;
        Sim t(off, 21, 0.02);
        t.Run(l, 90);
        Check(t.Index('E') < 0 && t.Count('E') == 0, "without GXV_AUTOQ_EDGESTEP no edge step is ever taken");
    }
    {
        Load flat;           // edge smoothing that buys nothing (the GPU's time does not change): tried once, undone, blocked
        flat.pixGpuMs = 17;
        Config c = Perf(false);
        c.edgeStep = true;
        Sim s(c, 22, 0.02);
        s.Run(flat, 40);
        if (verbose) s.Print("edge smoothing that saves nothing");
        Check(s.Index('E') >= 0 && s.Index('e') > s.Index('E') && s.ev[static_cast<size_t>(s.Index('e'))].why == Why::FxNoGain,
              "an edge step that saves no GPU time is undone for no gain");
    }

    std::printf("the steady 30: a CPU-limited scene is judged against 33.3 ms\n");
    {
        Load lit;            // the lit Observatory's kind: game turn 14 ms, front end 24 ms, GPU 22 ms at 640x456: the pacing holds an even 30
        lit.turnMs = 14;
        lit.frontMs = 24;
        lit.pixGpuMs = 20;
        lit.edgeMs = 0;
        lit.cadence = true;
        Sim s(Console(), 23, 0.04);
        s.Run(lit, 120, 40);
        if (verbose) s.Print("CPU-limited under the steady 30, GPU 22 ms");
        Check(s.ev.empty() && s.ctl.Current() == kToday, "GPU 22 ms under the steady 30: no step and no smaller picture (it fits 33.3 ms)");
        const FloorWindow w = s.ctl.TakeFloor();
        char line[200];
        std::snprintf(line, sizeof line, "the floor census: %u of %u pictures on the steady 30, %u late against 33.3 ms", w.cadence, w.pictures, w.late);
        Check(w.cadence == w.pictures && w.pictures > 0 && w.late * 20 < w.pictures, line);
        Load heavy = lit;    // a GPU that misses even 33.3 ms under the steady 30 (40 ms): the controller's again
        heavy.pixGpuMs = 38;
        heavy.frontMs = 18;
        Sim h(Console(), 24, 0.03);
        h.Run(heavy, 90);
        if (verbose) h.Print("GPU 40 ms under the steady 30");
        Check(h.ctl.Current() < kToday || h.ctl.Steps() != 0, "a GPU over 33.3 ms under the steady 30 still takes steps or a smaller size");
    }
    {
        Load gpu;            // the same GPU (22 ms) without the steady 30 (a GPU-limited scene): Performance mode takes steps and sizes
        gpu.turnMs = 8;
        gpu.frontMs = 12;
        gpu.pixGpuMs = 20;
        Sim s(Console(), 25, 0.03);
        s.Run(gpu, 120);
        if (verbose) s.Print("GPU 22 ms, no steady 30");
        Check(s.ctl.Steps() != 0 || s.ctl.Current() < kToday, "without it the same GPU time is GPU-limited at 16.7 ms: steps or a smaller size");
    }

    std::printf("the stage hint for the steady 30 (port_autoq_stages)\n");
    {
        Load fe;             // front end 26 ms, GPU 14 ms: the pipeline is the front end's, nothing to lower is needed
        fe.frontMs = 26;
        fe.pixGpuMs = 12;
        Sim s(Console(), 26, 0.02);
        s.Run(fe, 20);
        uint32_t g = 0, p = 0;
        int low = -1;
        const int known = port_autoq_stages(&g, &p, &low);
        char line[200];
        std::snprintf(line, sizeof line, "known %d: GPU %.1f ms, slowest per-picture stage %.1f ms (the front end), lowerable %d", known, g / 1000.0, p / 1000.0, low);
        Check(known == 1 && g > 12000 && g < 16000 && p > 24000 && p < 28500, line);
        Load gpu;            // at the floor rung with the steps on and a GPU over the budget: nothing left to lower
        gpu.pixGpuMs = 40;
        gpu.bgGain = 1.05;
        gpu.edgeMs = 1.0;
        Sim h(Console(), 27, 0.02);
        h.Run(gpu, 150);
        port_autoq_stages(&g, &p, &low);
        std::snprintf(line, sizeof line, "a GPU-limited scene at the floor (rung %d, steps %u): lowerable %d", h.ctl.Current(), h.ctl.Steps(), low);
        Check(h.ctl.Current() == 0 && low == 0, line);
    }

    std::printf("Performance mode as the console runs it (Settings)\n");
    {
        struct Case { const char* name; Load l; int lowest, highest; double minRate; };
        Load unlit;          // the early Observatory: 55-57 a second at 720x404 (front end ~16 ms), GPU 11 ms: nothing moves
        unlit.turnMs = 9; unlit.frontMs = 15.5; unlit.pixGpuMs = 7; unlit.edgeMs = 2.2;
        Load goodEgg;        // Good Egg's kind: GPU 19 ms at 720x404 (with edge smoothing), front end 14 ms: the steps, then 60
        goodEgg.turnMs = 8; goodEgg.frontMs = 14; goodEgg.pixGpuMs = 14.8; goodEgg.edgeMs = 2.2; goodEgg.bgGain = 1.08;
        Load beach;          // a pixel-bound GPU scene (Beach Bowl's kind, lighter): GPU 22 ms at 720x404: the steps and the sizes to 608x346
        beach.turnMs = 7; beach.frontMs = 13; beach.fixedGpuMs = 3; beach.pixGpuMs = 17; beach.edgeMs = 2.2; beach.bgGain = 1.05;
        const Case cases[] = {{"early Observatory, front end 15.5 ms", unlit, kToday, kToday, 50.0}, {"Good Egg's kind, GPU 19 ms", goodEgg, 1, kToday, 55.0},
                              {"pixel-bound GPU 22 ms", beach, 0, 0, 55.0}};
        for (const Case& c : cases) {
            Sim s(Console(), 28, 0.03);
            s.freezeUs = 150000;
            s.Run(c.l, 150, 45);
            if (verbose) s.Print(c.name);
            s.Mark();
            s.Run(c.l, 20);
            char line[220];
            std::snprintf(line, sizeof line, "%s: rung %d (%d..%d), steps %u, %.1f a second (at least %.0f), %u changes of size", c.name, s.ctl.Current(), c.lowest, c.highest,
                          s.ctl.Steps(), s.Rate(), c.minRate, s.Count('R'));
            Check(s.ctl.Current() >= c.lowest && s.ctl.Current() <= c.highest && s.Rate() >= c.minRate && s.Count('R') <= 4, line);
        }
    }

    std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all checks passed", g_failures);
    return g_failures ? 1 : 0;
}
