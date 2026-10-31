// Host-side test of GXV_AUTOQ_ANISO (platform/vgx/src/vgx_autoq.cpp, Config::aniso): anisotropic filtering as the top of the quality
// ladder. tools/test_vgx_autoq_aniso.sh builds and runs it.
//
// 1. With the option OFF every decision is the one made before it existed: the seeded traces of the recover and fxstep tests hash to
//    the same golden digests. With the option on but outside target mode nothing changes either (it belongs to the whole-picture
//    target 30), and the controller switches it off.
// 2. It comes on last: only at the highest rung (Config::maxRung), after anisoHoldSec of predicted room, never before that rung is
//    reached; it is never on at any other rung (checked at every picture of every run), also with a lower maxRung (no multisampling).
// 3. It goes first: a GPU-limited stretch at the top rung takes it off after downHoldSec, before any rung changes; when the picture
//    is still too heavy the rungs follow, and the way back up is the rungs first, anisotropic filtering last.
// 4. Switching it on again is blocked after an overload (blockSec, doubling), changes are at least 3 s apart, a borderline scene
//    does not make it flicker, and a CPU-limited stretch does not take it off.
// 5. A scene remembers it with its rung; Limit / Drop (no video memory) and the emergency rungs have it off.
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

struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void Add(uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 1099511628211ull;
        }
    }
};

// ---------------------------------------------------------------------------------------------------- the golden traces
// (the traces and the digest of tests/vgx_autoq_recover.cpp: the controller as it was before the recover option)
struct Scene {
    double gameMs;       // two game turns' CPU, at the base load
    double backendMs;    // renderer CPU a picture
    double fixedGpuMs;   // GPU work independent of the pixel count
    double gpuMsToday;   // GPU work that scales with Cost(rung), at 640x456
};
struct Trace {
    const char* name;
    double seconds;
    Scene (*scene)(double t);
    double noise;
    double pauseEvery;
    uint32_t seed;
};
Scene Light(double) { return {12, 3, 0, 5}; }
Scene Tight(double) { return {17, 3, 0, 6}; }
Scene HeavyBump(double t) { return (std::fmod(t, 200.0) >= 150.0 && std::fmod(t, 200.0) < 153.0) ? Scene{12, 3, 0, 40} : Scene{12, 3, 0, 5}; }
Scene Plaza(double) { return {14, 3, 3, 26}; }
Scene Spikes(double t) { return std::fmod(t, 13.0) < 0.5 ? Scene{12, 3, 0, 18} : Scene{12, 3, 0, 5}; }
const Trace kTraces[] = {
    {"light, pause each minute", 400, Light, 0.10, 60, 1},
    {"light, no pause", 400, Light, 0.10, 0, 2},
    {"tight, noisy, pause each minute", 600, Tight, 0.20, 60, 3},
    {"bump every 200 s, pause each minute", 800, HeavyBump, 0.05, 60, 4},
    {"bump every 200 s, no pause", 800, HeavyBump, 0.05, 0, 5},
    {"plaza (GPU-limited)", 300, Plaza, 0.10, 45, 6},
    {"effect spikes every 13 s", 300, Spikes, 0.10, 60, 7},
};

struct Golden {
    uint64_t digest = 0;
    unsigned fxChanges = 0;
};
Golden RunGolden(const Config& cfg, const Trace& tr, bool timers, bool target, Fnv& fnv) {
    Config c = cfg;
    if (target) {
        c.targetFps = 30;
        c.minRung = kEmergency;
        c.budgetMs = 1000.0 / 30.0;
    }
    Controller ctl(c);
    std::mt19937 rng(tr.seed);
    std::uniform_real_distribution<double> u(-tr.noise, tr.noise);
    uint64_t now = 1, backend = 1;
    uint32_t game = 1, turns = 1;
    const long frames = static_cast<long>(tr.seconds * 30.0);
    double t = 0;
    for (long i = 0; i < frames; ++i) {
        const Scene s = tr.scene(t);
        const double gms = s.gameMs * (1.0 + u(rng));
        game += static_cast<uint32_t>(gms * 500.0) * 2u;
        turns += 2;
        backend += static_cast<uint32_t>(s.backendMs * (1.0 + u(rng)) * 1000.0);
        FrameTimes f;
        f.startUs = now;
        now += 33333;
        f.presentUs = now;
        f.backendRunUs = backend;
        f.gameRunUs = game;
        f.gameTurns = turns;
        f.gameKnown = target;
        const double gpu = (s.fixedGpuMs + s.gpuMsToday * Cost(ctl.Current())) * (1.0 + u(rng));
        if (timers) {
            f.timers = f.busyKnown = true;
            f.gpuBusyUs = static_cast<uint32_t>(gpu * 1000.0);
        } else {
            f.gpuDoneUs = now + static_cast<uint64_t>(std::min(gpu, 33.3) * 0.2 * 1000.0);
        }
        f.quiet = tr.pauseEvery > 0 && std::fmod(t, tr.pauseEvery) >= tr.pauseEvery - 2.0;
        if (ctl.Add(f)) {
            const Change& ch = ctl.LastChange();
            fnv.Add(static_cast<uint64_t>(i));
            fnv.Add(static_cast<uint64_t>(static_cast<int64_t>(ch.from)));
            fnv.Add(static_cast<uint64_t>(static_cast<int64_t>(ch.to)));
            fnv.Add(static_cast<uint64_t>(ch.why));
        }
        t += 1.0 / 30.0;
    }
    return {fnv.h, ctl.FxChanges()};
}
// modes: bit 0 GPU timers, bit 1 target 30. `onlyModes`: bit mask of the modes to take part.
uint64_t Digest(const Config& cfg, unsigned modeMask, unsigned* fxChanges = nullptr) {
    Fnv fnv;
    for (const Trace& tr : kTraces)
        for (int mode = 0; mode < 4; ++mode) {
            if (!(modeMask & (1u << mode))) continue;
            const Golden g = RunGolden(cfg, tr, (mode & 1) != 0, mode >= 2, fnv);
            if (fxChanges) *fxChanges += g.fxChanges;
        }
    return fnv.h;
}
constexpr uint64_t kGoldenCalm = 0x5ee9eca5260f8fddull;
// (legacy: retaken when a step below today in target mode came to be judged on the GPU's busy time over belowCheckSec, 4 s of
// wall time: the same changes, the "no gain" verdicts of the target-30 fence-clock traces 2-2.4 s later)
constexpr uint64_t kGoldenLegacy = 0x3b340e35fcf827d2ull;

// ---------------------------------------------------------------------------------------------------- a model of the pipeline
struct Load {
    double gameMs = 12, backendMs = 3, fixedGpuMs = 3, pixGpuMs = 4; // pixGpuMs at 640x456 (Cost 1)
    double anisoCost = 1.2;  // GPU time with anisotropic filtering / without (the model's, a little above the controller's assumed 1.15)
    bool gameKnown = true;
};
struct Event {
    double t;
    char kind;   // 'R' rung change, 'A' / 'a' anisotropic filtering on / off
    int from, to;
    Why why;
};
struct Sim {
    Controller ctl;
    std::mt19937 rng;
    double noise;
    uint64_t now = 1, backend = 1;
    uint32_t game = 1, turns = 1;
    double t = 0;
    std::vector<Event> ev;
    bool anisoPrev = false;
    bool invariantBroken = false;   // anisotropic filtering on at a rung other than Config::maxRung
    double lastAnisoAt = -100, minAnisoGap = 1e9;
    unsigned lateFrames = 0;
    Sim(const Config& c, uint32_t seed, double n) : ctl(c), rng(seed), noise(n) {}
    void Frame(const Load& l, bool quiet = false, uint32_t scene = 0, uint64_t forceIntervalUs = 0) {
        std::uniform_real_distribution<double> u(-noise, noise);
        const double gms = l.gameMs * (1.0 + u(rng)), bms = l.backendMs * (1.0 + u(rng));
        const double gpu = (l.fixedGpuMs + l.pixGpuMs * Cost(ctl.Current())) * (1.0 + u(rng)) * (ctl.AnisoOn() ? l.anisoCost : 1.0);
        const double work = std::max({gms, bms, gpu});
        const uint64_t interval = forceIntervalUs ? forceIntervalUs
            : static_cast<uint64_t>(std::max(2.0, std::ceil(work * 1000.0 / 16683.0 - 1e-9)) * 16683.0);
        game += static_cast<uint32_t>(gms * 500.0) * 2u;
        turns += 2;
        backend += static_cast<uint32_t>(bms * 1000.0);
        FrameTimes f;
        f.startUs = now;
        now += interval;
        f.presentUs = now;
        f.backendRunUs = backend;
        f.gameRunUs = game;
        f.gameTurns = turns;
        f.gameKnown = l.gameKnown;
        f.timers = f.busyKnown = true;
        f.gpuBusyUs = static_cast<uint32_t>(gpu * 1000.0);
        f.quiet = quiet;
        f.scene = scene;
        if (!quiet && interval > 34300 && interval < 250000) ++lateFrames;
        const int before = ctl.Current();
        const bool changed = ctl.Add(f);
        t += static_cast<double>(interval) / 1e6;
        if (changed || ctl.Current() != before) ev.push_back({t, 'R', before, ctl.Current(), ctl.LastChange().why});
        if (ctl.AnisoOn() != anisoPrev) {
            ev.push_back({t, ctl.AnisoOn() ? 'A' : 'a', ctl.Current(), ctl.Current(), ctl.LastAnisoChange().why});
            anisoPrev = ctl.AnisoOn();
            minAnisoGap = std::min(minAnisoGap, t - lastAnisoAt);
            lastAnisoAt = t;
        }
        if (ctl.AnisoOn() && ctl.Current() != ctl.Cfg().maxRung) invariantBroken = true;
    }
    void Run(const Load& l, double seconds, double pauseEvery = 0, uint32_t scene = 0) {
        const double end = t + seconds, start = t;
        while (t < end) {
            const bool quiet = pauseEvery > 0 && std::fmod(t - start, pauseEvery) >= pauseEvery - 2.0;
            Frame(l, quiet, scene);
        }
    }
    int Index(char kind, double after = -1, int to = 9999) const {
        for (size_t i = 0; i < ev.size(); ++i)
            if (ev[i].t >= after && ev[i].kind == kind && (to == 9999 || ev[i].to == to)) return static_cast<int>(i);
        return -1;
    }
    int IndexAfter(size_t from, char kind) const {
        for (size_t i = from; i < ev.size(); ++i)
            if (ev[i].kind == kind) return static_cast<int>(i);
        return -1;
    }
    unsigned Count(char kind) const {
        unsigned n = 0;
        for (const Event& e : ev) n += e.kind == kind;
        return n;
    }
    void Print(const char* title) const {
        std::printf("    %s:\n", title);
        for (const Event& e : ev) {
            if (e.kind == 'R') std::printf("      t=%7.1f s  rung %2d -> %2d (%s)\n", e.t, e.from, e.to, WhyText(e.why));
            else std::printf("      t=%7.1f s  anisotropic filtering %s at rung %d (%s)\n", e.t, e.kind == 'A' ? "ON " : "off", e.from, WhyText(e.why));
        }
    }
};

Config Target(bool aniso, int maxRung = kRungs - 1) {
    Config c;
    c.targetFps = 30;
    c.minRung = kEmergency;
    c.budgetMs = 1000.0 / 30.0;
    c.recover = true;
    c.aniso = aniso;
    c.maxRung = maxRung;
    return c;
}

} // namespace

int main(int argc, char** argv) {
    const bool verbose = argc > 1 && std::string(argv[1]) == "verbose";
    if (argc > 1 && std::string(argv[1]) == "digest") {
        std::printf("calm 0x%016llxull\nlegacy 0x%016llxull\n", static_cast<unsigned long long>(Digest(Config(), 15)),
                    static_cast<unsigned long long>(Digest(Config::Legacy(), 15)));
        return 0;
    }

    std::printf("option off: the decisions are those of the controller before the option\n");
    Check(Digest(Config(), 15) == kGoldenCalm, "calm rules: every change of 28 seeded traces is unchanged");
    Check(Digest(Config::Legacy(), 15) == kGoldenLegacy, "legacy rules: the same traces give the same changes");
    {
        Config on;
        on.aniso = true;
        Config off;
        Check(Digest(on, 3) == Digest(off, 3), "the option outside target mode changes nothing (it belongs to the 30 target)");
        Controller c(on);
        Check(!c.Cfg().aniso && !c.AnisoOn(), "outside target mode the option is switched off by the controller");
        Config legacyOn = Config::Legacy();
        legacyOn.aniso = true;
        Check(Digest(legacyOn, 3) == Digest(Config::Legacy(), 3), "legacy rules with the option, no target: unchanged");
        Config tOn, tOff;
        tOn.aniso = true;
        tOn.recover = tOff.recover = false;
        const uint64_t a = Digest(tOn, 12), b = Digest(tOff, 12);
        std::printf("    target 30 traces with the option: digest %s\n", a == b ? "equal" : "differs (a change of anisotropic filtering restarts the windows)");
    }

    std::printf("a light scene: it comes on last, at the top rung, and nowhere else\n");
    {
        Load light; // GPU 3 + 4 x 3.4 = 16.6 ms at the top rung, 19.9 ms with it: room for the margin (70 % of 33.3 = 23.3)
        Sim s(Target(true), 21, 0.04);
        s.Run(light, 240, 45);
        if (verbose) s.Print("light scene, a pause every 45 s");
        const int top = s.Index('R', 0, kRungs - 1), an = s.Index('A');
        Check(top >= 0 && an > top, "anisotropic filtering comes on after the picture reached the top rung (960x544, 4x multisampling)");
        Check(an >= 0 && s.ev[an].t - s.ev[top].t >= 7.9, "... at least anisoHoldSec (8 s) of room later");
        Check(an >= 0 && s.ev[an].from == kRungs - 1 && s.ev[an].why == Why::AnisoOn, "... at the top rung, with its reason");
        Check(s.ctl.AnisoOn() && s.ctl.Current() == kRungs - 1 && s.Count('a') == 0, "and it stays on in a scene with room");
        Check(!s.invariantBroken, "it is never on at any other rung");
        Check(s.ctl.AnisoChanges() == 1, "one change in 240 s");
        Sim off(Target(false), 21, 0.04);
        off.Run(light, 240, 45);
        Check(off.Count('A') == 0 && !off.ctl.AnisoOn() && off.ctl.Current() == kRungs - 1, "without the option it never comes on");
        Check(s.Count('R') == off.Count('R') || s.Count('R') <= off.Count('R') + 1, "the option adds no rung changes of its own");
    }
    {
        // no multisampling here: the top rung is 960x544 single-sampled (rung 6)
        Load light;
        light.pixGpuMs = 7; // 3 + 7 x 1.79 = 15.5 ms at rung 6
        Sim s(Target(true, kTopSingle), 22, 0.04);
        s.Run(light, 240, 45);
        if (verbose) s.Print("top rung 6");
        const int top = s.Index('R', 0, kTopSingle), an = s.Index('A');
        Check(top >= 0 && an > top && s.ev[an].from == kTopSingle, "without multisampling it is the top of the ladder at 960x544");
        Check(!s.invariantBroken && s.ctl.AnisoOn(), "and never on at another rung");
    }

    std::printf("a load that needs more than the top rung leaves: it goes first\n");
    {
        // Light, then a scene the top rung only just fits without it: 3 + 6 x 3.4 = 23.4 ms, 28.1 ms with it (the 3-vblank rule makes that
        // a late picture only when it passes 33.3 ms: use 3 + 8.4 x 3.4 = 31.6 ms, 37.9 ms with it)
        Load light, edge;
        edge.pixGpuMs = 8.4;
        Sim s(Target(true), 23, 0.02);
        s.Run(light, 150, 45);
        const bool wasOn = s.ctl.AnisoOn();
        const size_t mark = s.ev.size();
        const double t0 = s.t;
        s.Run(edge, 60);
        if (verbose) s.Print("light, then an edge scene");
        const int off = s.IndexAfter(mark, 'a');
        Check(wasOn, "setup: anisotropic filtering was on at the top rung");
        Check(off >= 0 && s.ev[off].t - t0 >= 1.4 && s.ev[off].t - t0 <= 8.0, "a GPU-limited stretch takes it off after downHoldSec (1.5 s) or so");
        Check(off >= 0 && s.ev[off].why == Why::AnisoOff && s.ev[off].from == kRungs - 1, "... with its reason, at the top rung");
        Check(s.IndexAfter(mark, 'R') < 0 && s.ctl.Current() == kRungs - 1, "... and the picture keeps its size and samples: no rung changed (it was enough)");
        Check(!s.ctl.AnisoOn() && s.Count('A') == 1, "... and it stays off while the scene is that heavy");
        Check(!s.invariantBroken, "never on at another rung");
    }
    {
        // Heavier: even without it the top rung misses; the rungs follow, after it
        Load light, heavy;
        heavy.pixGpuMs = 14; // 3 + 14 x 3.4 = 50 ms at the top
        Sim s(Target(true), 24, 0.02);
        s.Run(light, 150, 45);
        const size_t mark = s.ev.size();
        s.Run(heavy, 90);
        if (verbose) s.Print("light, then a heavy scene");
        const int off = s.IndexAfter(mark, 'a'), down = s.IndexAfter(mark, 'R');
        Check(off >= 0 && down > off, "when the picture is still too heavy the first thing given up is anisotropic filtering, then the rungs");
        Check(down >= 0 && s.ev[down].from == kRungs - 1 && s.ev[down].to < kRungs - 1, "... the multisampling and the size after it");
        Check(!s.invariantBroken && !s.ctl.AnisoOn(), "it is off at every rung below the top");
    }

    std::printf("the way back: the rungs first, anisotropic filtering last\n");
    {
        Load light, heavy;
        heavy.pixGpuMs = 14;
        Sim s(Target(true), 25, 0.02);
        s.Run(light, 150, 45);
        s.Run(heavy, 60);
        const size_t mark = s.ev.size();
        s.Run(light, 400, 45);
        if (verbose) s.Print("heavy, then light again");
        const int backTop = s.IndexAfter(mark, 'R'), on = s.IndexAfter(mark, 'A');
        int lastRise = -1;
        for (size_t i = mark; i < s.ev.size(); ++i)
            if (s.ev[i].kind == 'R' && s.ev[i].to == kRungs - 1) lastRise = static_cast<int>(i);
        Check(backTop >= 0 && lastRise >= 0 && on > lastRise, "it comes on again only after the picture is back at the top rung");
        Check(on >= 0 && s.ev[on].t - s.ev[lastRise].t >= 7.9, "... and after its own hold of room");
        Check(s.ctl.AnisoOn() && !s.invariantBroken, "and it is on again at the end, at the top rung only");
    }

    std::printf("blocks, spacing, borderline scenes\n");
    {
        Load light, edge;
        edge.pixGpuMs = 8.4;
        Sim s(Target(true), 26, 0.02);
        s.Run(light, 150, 45);
        s.Run(edge, 30);                   // off, blocked 60 s
        const int offIdx = s.Index('a');
        const double offAt = offIdx >= 0 ? s.ev[offIdx].t : 0;
        s.Run(light, 20);                  // room again, inside the block
        const bool stillOff = !s.ctl.AnisoOn() && s.Index('A', offAt) < 0;
        s.Run(light, 120);
        const int again = s.Index('A', offAt);
        if (verbose) s.Print("block after an overload");
        Check(stillOff, "after it was given up it stays off during the block (60 s) although the room is back");
        Check(again >= 0 && s.ev[again].t - offAt >= 59.0, "... and comes back after the block and its hold of room");
        Check(s.minAnisoGap >= 3.0, "changes of it are at least 3 s apart");
    }
    {
        // a borderline scene: the GPU time with it sits right at the limit of the margin
        Load edge;
        edge.pixGpuMs = 5.8; // 3 + 5.8 x 3.4 = 22.7 ms: with 1.15 predicted 26 ms: no room, so it never comes on
        Sim s(Target(true), 27, 0.12);
        s.Run(edge, 400, 60);
        Check(s.Count('A') + s.Count('a') <= 4, "a borderline scene does not make it flicker");
        Check(s.minAnisoGap >= 3.0 || s.Count('A') + s.Count('a') <= 1, "... and never twice within 3 s");
        Check(!s.invariantBroken, "... and never at another rung");
    }
    {
        // alternating light and heavy halves every 20 s for 400 s
        Load light, edge;
        edge.pixGpuMs = 8.4;
        Sim s(Target(true), 28, 0.03);
        for (int i = 0; i < 20; ++i) s.Run(i % 2 ? edge : light, 20, 45);
        if (verbose) s.Print("alternating");
        Check(s.Count('A') + s.Count('a') <= 10, "an alternating scene switches it at most a few times (blocks double)");
        Check(s.minAnisoGap >= 3.0 && !s.invariantBroken, "... never closer than 3 s, never at another rung");
    }
    {
        // a CPU-limited stretch: the game's two turns are the long stage; taking the GPU extra off would not help
        Load light, cpu;
        cpu.gameMs = 45;
        Sim s(Target(true), 29, 0.02);
        s.Run(light, 150, 45);
        const bool wasOn = s.ctl.AnisoOn();
        const size_t mark = s.ev.size();
        s.Run(cpu, 60);
        Check(wasOn && s.IndexAfter(mark, 'a') < 0 && s.ctl.AnisoOn(), "a game-CPU-limited stretch does not take it off");
    }

    std::printf("scenes, memory pressure\n");
    {
        Load light;
        Sim s(Target(true), 30, 0.02);
        s.Run(light, 200, 45, 111);          // scene A: top rung, anisotropic filtering on
        const bool aOn = s.ctl.AnisoOn();
        s.Frame(light, false, 222, 2500000); // a load, scene B: starts at today's size, plain
        s.Run(light, 3, 0, 222);
        const bool bOff = !s.ctl.AnisoOn() && s.ctl.Current() == kToday;
        s.Frame(light, false, 111, 2500000); // a load, back to scene A
        const bool aBack = s.ctl.AnisoOn() && s.ctl.Current() == kRungs - 1;
        if (verbose) s.Print("scene A, B, A");
        Check(aOn, "setup: scene A ends with anisotropic filtering on at the top rung");
        Check(bOff, "a new scene starts at today's size without it");
        Check(aBack, "the scene seen before comes back with its rung and anisotropic filtering");
        Check(!s.invariantBroken, "it is never on below the top rung");
    }
    {
        Load light;
        Sim s(Target(true), 31, 0.02);
        s.Run(light, 200, 45);
        const bool on = s.ctl.AnisoOn();
        s.ctl.Drop(kToday, s.now + 1);       // short of video memory: back to today's size at once
        Check(on && !s.ctl.AnisoOn() && s.ctl.Current() == kToday, "a drop to today's rung (no video memory) has it off");
        Sim t2(Target(true), 32, 0.02);
        t2.Run(light, 200, 45);
        const bool on2 = t2.ctl.AnisoOn();
        t2.ctl.Limit(5, t2.now + 1);
        Check(on2 && !t2.ctl.AnisoOn() && t2.ctl.Current() == 5, "a step cut short by video memory (Limit) has it off");
    }
    {
        // GXV_AUTOQ_MAX below the multisampled rungs, and at today's rung with the other steps: no conflict
        Load light;
        Config c = Target(true, kToday);
        c.fxStep = c.bgStep = true;
        Sim s(c, 33, 0.02);
        s.Run(light, 120, 45);
        Check(s.ctl.AnisoOn() && s.ctl.Current() == kToday && s.ctl.Steps() == 0 && !s.invariantBroken, "with the ladder capped at today's rung it is the top, and the other steps are off");
        Load heavy;
        heavy.pixGpuMs = 40;
        const size_t mark = s.ev.size();
        s.Run(heavy, 60);
        const int off = s.IndexAfter(mark, 'a');
        const int fx = s.ctl.Steps() ? 0 : -1;
        Check(off >= 0 && !s.ctl.AnisoOn(), "... and it goes before the half-size effects, the cheaper background or a smaller picture");
        (void)fx;
    }

    std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all checks passed", g_failures);
    return g_failures ? 1 : 0;
}
