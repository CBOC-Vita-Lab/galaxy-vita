// Host-side test of GXV_AUTOQ_FXSTEP (platform/vgx/src/vgx_autoq.cpp, Config::fxStep) and of the floor census
// (Controller::TakeFloor). tools/test_vgx_autoq_fxstep.sh builds and runs it.
//
// 1. With the option OFF every decision is the one made before it existed: the seeded traces of the recover test (calm and
//    legacy rules, fence and GPU-timer clocks, calm and target 30) hash to the same golden digests. With the option on but
//    outside target mode nothing changes either (the step belongs to the whole-picture target), and in target mode a trace
//    that never needed the step makes the same changes as without it.
// 2. A heavy GPU stretch at today's rung reaches the half-size effects, then the cheaper background, before any picture size
//    below today's, and those before the 480x340 emergency rung; a scene whose effects hold no saving gets the step undone
//    (blocked) and the next one tried; a scene a step is enough for never needs a smaller picture.
// 3. The way back is the reverse: the emergency rung first, then the sizes up to today's, the cheaper background off, the
//    half-size effects off, then the picture size above today's; no step is on above today's rung (checked at every picture
//    of every run).
// 4. No oscillation: a stationary borderline scene and an alternating scene switch the step rarely, never closer than 3 s.
// 5. A CPU-limited stretch (the game's two turns, the renderer, an unmeasured stage, an unknown game clock) starts no step and
//    lowers nothing, at the bottom rung and above it; the floor census blames those pictures on the right stage.
// 6. The floor census: pictures, late pictures and their time, causes, stalls and quiet moments, reset by TakeFloor.
// 7. A scene remembers the step with its rung.
// 8. A step below today's size is judged on the GPU's busy time against the saving the sizes predict: a scene whose GPU time
//    hardly changes with the picture (under 3 %) keeps today's size, the useless step undone and blocked even
//    when the change's own freeze is a stall, never two steps down in a row, not retried on the next visit of the scene; a
//    scene whose GPU time scales with the picture still steps down and keeps it, and so does one where each size saves only
//    4-6 % while 30 is missed (Beach Bowl as rc2 measured it): the floor first.
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
    double gameMs = 12, backendMs = 3, fixedGpuMs = 3, pixGpuMs = 5; // pixGpuMs at 640x456 (Cost 1)
    double fxGain = 1.4;     // GPU time without the half-size effects / with them
    double bgGain = 1.12;    // ... without the cheaper background / with it
    double extraMs = 0;      // unmeasured time added to the picture (front end, waits)
    bool gameKnown = true;
};
struct Event {
    double t;
    char kind;   // 'R' rung change, 'F' / 'f' half-size effects on / off, 'B' / 'b' cheaper background on / off
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
    unsigned stepsPrev = 0;
    unsigned lateFrames = 0, frames = 0;
    bool invariantBroken = false;
    double lastFxAt = -100, minFxGap = 1e9;
    uint64_t freezeUs = 0;   // the interval of the picture after a change of rung (the console: new render targets, 0.26 s)
    bool freezeNext = false;
    double belowSec = 0;     // time spent below today's rung
    Sim(const Config& c, uint32_t seed, double n) : ctl(c), rng(seed), noise(n) {}
    void Frame(const Load& l, bool quiet = false, uint32_t scene = 0, uint64_t forceIntervalUs = 0) {
        std::uniform_real_distribution<double> u(-noise, noise);
        const double gms = l.gameMs * (1.0 + u(rng)), bms = l.backendMs * (1.0 + u(rng));
        const double gpu = (l.fixedGpuMs + l.pixGpuMs * Cost(ctl.Current())) * (1.0 + u(rng)) / (ctl.FxStepOn() ? l.fxGain : 1.0) / (ctl.BgStepOn() ? l.bgGain : 1.0);
        const double work = std::max({gms, bms, gpu}) + l.extraMs;
        // the picture rate is the 60 Hz retrace clock: two, three, ... retraces per picture
        const uint64_t interval = forceIntervalUs ? forceIntervalUs : freezeNext ? freezeUs
            : static_cast<uint64_t>(std::max(2.0, std::ceil(work * 1000.0 / 16683.0 - 1e-9)) * 16683.0);
        freezeNext = false;
        if (ctl.Current() < kToday) belowSec += static_cast<double>(interval) / 1e6;
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
        ++frames;
        if (!quiet && interval > 34300) ++lateFrames;
        const int before = ctl.Current();
        const bool changed = ctl.Add(f);
        t += static_cast<double>(interval) / 1e6;
        if (changed || ctl.Current() != before) {
            ev.push_back({t, 'R', before, ctl.Current(), ctl.LastChange().why});
            freezeNext = freezeUs > 0 && ctl.Current() != before;
        }
        if (ctl.Steps() != stepsPrev) {
            for (unsigned bit = 1; bit <= 2; bit <<= 1) {
                const bool was = (stepsPrev & bit) != 0, is = (ctl.Steps() & bit) != 0;
                if (was != is) ev.push_back({t, bit == 1 ? (is ? 'F' : 'f') : (is ? 'B' : 'b'), ctl.Current(), ctl.Current(), ctl.LastFxChange().why});
            }
            stepsPrev = ctl.Steps();
            minFxGap = std::min(minFxGap, t - lastFxAt);
            lastFxAt = t;
        }
        if (ctl.Steps() && ctl.Current() > kToday) invariantBroken = true; // (the steps belong to today's rung and below)
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
    unsigned Count(char kind) const {
        unsigned n = 0;
        for (const Event& e : ev) n += e.kind == kind;
        return n;
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

Config Target(bool fxStep, bool recover = true, bool bgStep = true) {
    Config c;
    c.targetFps = 30;
    c.minRung = kEmergency;
    c.budgetMs = 1000.0 / 30.0;
    c.fxStep = fxStep;
    c.bgStep = fxStep && bgStep;
    c.recover = recover;
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
        on.fxStep = on.bgStep = true;
        Config off;
        Check(Digest(on, 3) == Digest(off, 3), "the option outside target mode changes nothing (the step belongs to the 30 target)");
        Controller c(on);
        Check(!c.Cfg().fxStep && !c.Cfg().bgStep && !c.FxStepOn() && !c.BgStepOn(), "outside target mode the option is switched off by the controller");
        Config tOn = on, tOff = off;
        tOn.recover = tOff.recover = false;
        unsigned fx = 0;
        const uint64_t a = Digest(tOn, 12, &fx), b = Digest(tOff, 12);
        std::printf("    target 30 traces with the option: %u step changes; digest %s\n", fx, a == b ? "equal" : "differs");
        Check(fx > 0 || a == b, "target 30: traces that never needed the step make the same changes");
    }

    std::printf("a heavy GPU stretch: half-size effects, cheaper background, then the picture size, then the emergency rung\n");
    {
        Load heavy;
        heavy.pixGpuMs = 80; // 83 ms at 640x456; the steps bring it to 53 ms, the smaller pictures to 35 ms at 480x340
        Sim off(Target(false), 11, 0.05), on(Target(true), 11, 0.05);
        off.Run(heavy, 120);
        on.Run(heavy, 120);
        if (verbose) { off.Print("without the option"); on.Print("with the option"); }
        const int offEm = off.Index('R', 0, kEmergency), onEm = on.Index('R', 0, kEmergency);
        const int fx = on.Index('F'), bg = on.Index('B'), firstDown = on.Index('R');
        Check(off.Count('F') + off.Count('B') == 0 && offEm >= 0, "without the option: no step, the emergency rung is taken");
        Check(fx >= 0 && bg > fx && firstDown > bg && onEm > firstDown, "with the option: effects half size, then cheaper background, then a smaller picture, then 480x340");
        Check(fx >= 0 && on.ev[fx].from == kToday && on.ev[bg].from == kToday, "the steps start at today's rung (640x456)");
        Check(fx >= 0 && on.ev[fx].t >= 1.9, "the first step waits fxHoldSec (2 s) of a GPU-limited miss");
        Check(bg > fx && on.ev[bg].t - on.ev[fx].t >= 3.0, "the second step comes at least 3 s after the first (judged first)");
        Check(onEm >= 0 && offEm >= 0 && on.ev[onEm].t - off.ev[offEm].t <= 25.0, "the steps delay the emergency rung by no more than 25 s when they are not enough");
        Check(on.Count('R') <= 6 && on.Count('F') + on.Count('f') + on.Count('B') + on.Count('b') <= 2, "no oscillation under a lasting overload");
        Check(!on.invariantBroken && !off.invariantBroken, "no step is on above today's rung");
    }
    {
        Load enough;
        enough.pixGpuMs = 36; // 39 ms at 640x456, 27.9 ms with the effects at half size
        Sim off(Target(false), 12, 0.05), on(Target(true), 12, 0.05);
        off.Run(enough, 120);
        on.Run(enough, 120);
        if (verbose) { off.Print("without the option"); on.Print("with the option"); }
        Check(off.Index('R', 0, 1) >= 0, "without the option: the picture shrinks below 640x456");
        Check(on.Count('F') == 1 && on.Count('B') == 0 && on.Count('R') == 0 && on.ctl.Current() == kToday && on.ctl.FxStepOn(),
              "with the option: half-size effects are enough: the picture stays at 640x456, no cheaper background, no smaller picture");
        Check(on.lateFrames < off.lateFrames + 400, "the step does not lengthen the time below 30 in a way that matters");
        const int fx = on.Index('F');
        Check(fx >= 0 && on.ev[fx].t >= 1.9 && on.ev[fx].t <= 6.0, "the step waits fxHoldSec (2 s) of a GPU-limited miss");
        Check(on.ctl.LastFxChange().why == Why::FxOn, "the change is recorded with its reason");
    }
    {
        Load none;
        none.pixGpuMs = 36;
        none.fxGain = 1.0; // a scene with no effects to halve ...
        none.bgGain = 1.5; // ... but with background textures that cost
        Sim on(Target(true), 13, 0.03);
        on.Run(none, 120);
        if (verbose) on.Print("no saving from the effects, a good one from the background");
        const int fx = on.Index('F'), undo = on.Index('f'), bg = on.Index('B');
        Check(fx >= 0 && undo > fx && on.ev[undo].why == Why::FxNoGain, "effects that buy no speed are undone after their trial (reason: no gain)");
        Check(bg > undo && on.ctl.BgStepOn() && !on.ctl.FxStepOn(), "... and the cheaper background is tried next, and kept");
        Check(on.Count('F') == 1 && on.Count('R') == 0, "... the undone step is not tried again while blocked, and the picture keeps its size");
    }
    {
        Load none;
        none.pixGpuMs = 52;
        none.fxGain = 1.0;
        none.bgGain = 1.0;
        Sim on(Target(true), 14, 0.03);
        on.Run(none, 120);
        if (verbose) on.Print("no saving from either step");
        const int undoB = on.Index('b'), rung = on.Index('R'), em = on.Index('R', 0, kEmergency);
        Check(on.Count('F') == 1 && on.Count('B') == 1 && undoB >= 0, "two steps that buy nothing are each tried once and undone");
        Check(rung > undoB && em > rung, "... and the picture size follows, the emergency rung last");
        Check(!on.ctl.FxStepOn() && !on.ctl.BgStepOn(), "... with both steps off");
    }

    std::printf("the way back is the reverse\n");
    for (int recover = 0; recover < 2; ++recover) {
        Load heavy;
        heavy.pixGpuMs = 80;
        Load light;
        light.pixGpuMs = 6;
        Sim s(Target(true, recover != 0), 21, 0.05);
        s.Run(heavy, 90, 25);
        const bool reachedEmergency = s.ctl.Current() == kEmergency && s.ctl.FxStepOn() && s.ctl.BgStepOn();
        const double lightFrom = s.t;
        s.Run(light, 500, 25);
        if (verbose) s.Print(recover ? "heavy 90 s, then light (recover on)" : "heavy 90 s, then light (recover off)");
        int exitEm = -1;
        for (size_t i = 0; i < s.ev.size(); ++i)
            if (s.ev[i].t >= lightFrom && s.ev[i].kind == 'R' && s.ev[i].from == kEmergency) { exitEm = static_cast<int>(i); break; }
        const int bgOff = s.Index('b', lightFrom), fxOff = s.Index('f', lightFrom);
        int reachedToday = -1, firstAbove = -1;
        for (size_t i = 0; i < s.ev.size(); ++i) {
            if (s.ev[i].t < lightFrom || s.ev[i].kind != 'R') continue;
            if (reachedToday < 0 && s.ev[i].to == kToday) reachedToday = static_cast<int>(i);
            if (firstAbove < 0 && s.ev[i].to > kToday) firstAbove = static_cast<int>(i);
        }
        std::printf("   recover %s:\n", recover ? "on" : "off");
        Check(reachedEmergency, "the heavy stretch ends with both steps on at the emergency rung");
        Check(exitEm >= 0 && reachedToday >= exitEm, "the emergency rung is left first, the picture climbs back to 640x456");
        Check(bgOff > reachedToday && fxOff > bgOff, "then the cheaper background goes off, then the half-size effects");
        Check(firstAbove > fxOff, "the picture size rises above 640x456 only after both steps went off");
        Check(bgOff >= 0 && s.ev[bgOff].from == kToday && fxOff >= 0 && s.ev[fxOff].from == kToday, "the steps go off at today's rung");
        Check(!s.invariantBroken, "no step is on above today's rung");
        Check(s.ctl.Current() > kToday && s.ctl.Steps() == 0, "in the end: a higher rung than 640x456, steps off");
        Check(s.Count('F') == 1 && s.Count('B') == 1, "each step went on once");
    }

    std::printf("no oscillation\n");
    {
        // A stationary scene on the edge: the GPU at the bottom rung is 31-34 ms, with the step 22-24 ms, so room for the step
        // to go off (70 % of 33.3 ms = 23.3 ms) is borderline.
        Load edge;
        edge.fixedGpuMs = 3;
        edge.pixGpuMs = 29; // 32 ms at 640x456; with the effects at half size 22.9 ms
        Sim s(Target(true), 31, 0.06);
        s.Run(edge, 600, 40);
        if (verbose) s.Print("stationary borderline scene, 600 s");
        Check(s.Count('F') + s.Count('f') + s.Count('B') + s.Count('b') <= 4, "stationary borderline scene: at most 4 changes of the steps in 600 s");
        Check(s.minFxGap >= 3.0, "changes of the step are at least 3 s apart");
        Check(s.Count('R') <= 10, "... and the picture size changes rarely");
        Check(!s.invariantBroken, "the step is never on above the bottom rung");
    }
    {
        // heavy 50 s, light 50 s, alternating for 20 minutes
        Load heavy;
        heavy.pixGpuMs = 52;
        Load light;
        light.pixGpuMs = 5;
        Sim s(Target(true), 32, 0.05);
        for (int i = 0; i < 12; ++i) {
            s.Run(heavy, 50, 25);
            s.Run(light, 50, 25);
        }
        if (verbose) s.Print("alternating 50 s heavy, 50 s light");
        Check(s.Count('F') <= 12 && s.Count('B') <= 12 && s.Count('F') >= s.Count('f') && s.Count('B') >= s.Count('b'), "alternating scene: each step goes on at most once per heavy stretch");
        Check(s.minFxGap >= 3.0, "changes of the step are at least 3 s apart");
        Check(s.Count('R') <= 60, "... and the picture size changes less than 5 times per cycle on average");
        Check(!s.invariantBroken, "the step is never on above the bottom rung");
    }
    std::printf("a CPU-limited stretch starts no step and lowers nothing\n");
    struct Cpu { const char* name; Load l; int startRung; bool lateExpected; };
    {
        Load game;      // two game turns 45 ms, an idle GPU
        game.gameMs = 45; game.pixGpuMs = 5;
        Load gameHeavyGpu;  // game 40, GPU 32: the GPU is not the largest by 5 %
        gameHeavyGpu.gameMs = 40; gameHeavyGpu.pixGpuMs = 30; gameHeavyGpu.fixedGpuMs = 2;
        Load renderer;  // renderer CPU 40, GPU 33
        renderer.backendMs = 40; renderer.pixGpuMs = 30; renderer.fixedGpuMs = 3;
        Load other;     // 12 ms of game, 3 ms of renderer, GPU 20: 25 ms of unmeasured time (front end, waits)
        other.gameMs = 12; other.pixGpuMs = 20; other.extraMs = 25;
        Load unknown;   // the game clock is not known, the GPU is heavy
        unknown.gameKnown = false; unknown.pixGpuMs = 80;
        Load pairs[] = {game, gameHeavyGpu, renderer, other, unknown};
        const char* names[] = {"game CPU 45 ms, idle GPU", "game CPU 40 ms, GPU 32 ms", "renderer CPU 40 ms, GPU 33 ms",
                               "25 ms of unmeasured time", "game clock unknown, GPU 60 ms"};
        for (int k = 0; k < 5; ++k) {
            for (int startRung : {0, kToday}) {
                Config c = Target(true);
                c.startRung = startRung;
                c.maxRung = startRung;
                Sim s(c, 40 + k, 0.03);
                s.Run(pairs[k], 120, 30);
                char line[200];
                std::snprintf(line, sizeof line, "%s, start rung %d: no rung change, no step", names[k], startRung);
                // (the unknown game clock must hold the rung whatever the GPU says; the others are not GPU-limited)
                Check(s.ev.empty() && s.ctl.Current() == startRung && !s.ctl.FxStepOn(), line);
            }
        }
        // the unknown game clock case also with the ladder free to move
        Sim s(Target(true), 50, 0.03);
        s.Run(unknown, 120, 30);
        Check(s.ctl.Current() == kToday && s.Count('F') == 0, "game clock unknown: the controller holds the rung and the step even with a free ladder");
    }
    {
        // The census names the stage of each late picture. 100 pictures per stretch.
        auto census = [&](const Load& l, int startRung) {
            Config c = Target(true);
            c.startRung = c.maxRung = startRung;
            Sim s(c, 60, 0.0);
            s.Run(l, 4);
            return s.ctl.TakeFloor();
        };
        Load game;
        game.gameMs = 45; game.pixGpuMs = 5;
        const FloorWindow g = census(game, 0);
        Check(g.late > 0 && g.game == g.late && g.gpu == 0 && g.renderer == 0, "late pictures of a game-CPU-bound stretch are blamed on the game CPU");
        Load renderer;
        renderer.backendMs = 40; renderer.pixGpuMs = 5;
        const FloorWindow r = census(renderer, 0);
        Check(r.late > 0 && r.renderer == r.late && r.gpu == 0 && r.game == 0, "... of a renderer-CPU-bound stretch on the renderer CPU");
        Load gpu;
        gpu.pixGpuMs = 80;
        const FloorWindow p = census(gpu, 0);
        Check(p.late > 0 && p.gpu == p.late && p.game == 0 && p.renderer == 0, "... of a GPU-bound stretch on the GPU");
        Load other;
        other.gameMs = 12; other.pixGpuMs = 20; other.extraMs = 25;
        const FloorWindow o = census(other, 0);
        Check(o.late > 0 && o.other == o.late && o.unknown == 0, "... of an unexplained stretch on unmeasured work (front end, waits, pacing)");
        Load unknown = gpu;
        unknown.gameKnown = false;
        unknown.pixGpuMs = 5; unknown.extraMs = 40;
        const FloorWindow k = census(unknown, 0);
        Check(k.late > 0 && k.unknown == k.late, "... and with the game clock unknown and nothing else large: not measured");
    }

    std::printf("the floor census\n");
    {
        Controller c(Target(false));
        uint64_t now = 1, backend = 1;
        uint32_t game = 1, turns = 1;
        auto feed = [&](uint64_t interval, double gameMs, double backendMs, double gpuMs, bool quiet = false) {
            game += static_cast<uint32_t>(gameMs * 500.0) * 2u;
            turns += 2;
            backend += static_cast<uint32_t>(backendMs * 1000.0);
            FrameTimes f;
            f.startUs = now;
            now += interval;
            f.presentUs = now;
            f.backendRunUs = backend;
            f.gameRunUs = game;
            f.gameTurns = turns;
            f.gameKnown = true;
            f.timers = f.busyKnown = true;
            f.gpuBusyUs = static_cast<uint32_t>(gpuMs * 1000.0);
            f.quiet = quiet;
            c.Add(f);
        };
        feed(33333, 10, 3, 10); // (the first picture only seeds the controller)
        for (int i = 0; i < 100; ++i) feed(33370, 10, 3, 10);                  // on time
        for (int i = 0; i < 10; ++i) feed(50050, 10, 3, 32);                   // 3 vblanks, GPU 32 ms: GPU
        for (int i = 0; i < 6; ++i) feed(50050, 31, 3, 10);                    // game CPU
        for (int i = 0; i < 4; ++i) feed(66733, 10, 30, 10);                   // renderer CPU
        for (int i = 0; i < 3; ++i) feed(50050, 10, 3, 12);                    // nothing near the budget: other
        feed(1800000, 10, 3, 10);                                              // a load
        for (int i = 0; i < 5; ++i) feed(33333, 10, 3, 10, true);              // a pause
        const FloorWindow w = c.TakeFloor();
        char line[300];
        std::snprintf(line, sizeof line, "pictures %u, late %u (gpu %u, game %u, renderer %u, other %u, unknown %u), stalls %u, quiet %u, worst %.1f ms, late time %.3f s",
                      w.pictures, w.late, w.gpu, w.game, w.renderer, w.other, w.unknown, w.stalls, w.quiet, w.worstUs / 1000.0, w.lateUs / 1e6);
        std::printf("    %s\n", line);
        Check(w.pictures == 123 && w.late == 23, "counts the pictures and the late ones (a stall and the quiet moment are not pictures)");
        Check(w.gpu == 10 && w.game == 6 && w.renderer == 4 && w.other == 3 && w.unknown == 0, "blames each late picture on its largest stage");
        Check(w.stalls == 1 && w.stallUs == 1800000 && w.quiet == 5, "counts the stall and the quiet pictures apart");
        Check(w.worstUs == 66733, "keeps the longest late picture");
        Check(w.lateUs == 10ull * 50050 + 6ull * 50050 + 4ull * 66733 + 3ull * 50050, "adds up the time of the late pictures (seconds below 30)");
        const FloorWindow again = c.TakeFloor();
        Check(again.pictures == 0 && again.late == 0 && again.stalls == 0 && again.quiet == 0, "taking the census starts a new window");
    }

    std::printf("a scene remembers the steps with its rung\n");
    {
        Load heavy;
        heavy.pixGpuMs = 52;
        Load light;
        light.pixGpuMs = 5;
        Sim s(Target(true), 70, 0.03);
        s.Run(heavy, 40, 0, 111);                 // scene A: steps on, perhaps a smaller picture
        const unsigned aSteps = s.ctl.Steps();
        const int aRung = s.ctl.Current();
        s.Frame(light, false, 222, 2500000);      // a load, scene B
        s.Run(light, 5, 0, 222);
        const bool bOff = s.ctl.Steps() == 0 && s.ctl.Current() == kToday;
        s.Frame(heavy, false, 111, 2500000);      // a load, back to scene A
        const bool aBack = s.ctl.Steps() == aSteps && s.ctl.Current() == aRung;
        if (verbose) s.Print("scene A, B, A");
        Check(aSteps == 3 && aRung <= kToday, "setup: scene A ends with both steps on at or below 640x456");
        Check(bOff, "a new scene starts at today's size with the steps off");
        Check(aBack, "the scene seen before comes back with its rung and its steps");
        Check(!s.invariantBroken, "no step is on above today's rung");
    }

    std::printf("a step below today's size is judged on the GPU's busy time\n");
    {
        // Beach Bowl as the console measured it (31 Oct 2026): 38 ms of GPU a picture at 640x456, 36-38 ms at 592x422 and at
        // 480x340: nearly all of it does not scale with the picture. The steps buy nothing either. Every change of the picture
        // freezes it for 0.26 s (over gapResetMs). Without pauses and with one each minute.
        Load beach;
        beach.gameMs = 14;
        beach.fixedGpuMs = 34;
        beach.pixGpuMs = 4;
        beach.fxGain = beach.bgGain = 1.0;
        for (int pauses = 0; pauses < 2; ++pauses) {
            Sim s(Target(true), 80 + pauses, 0.05);
            s.freezeUs = 260000;
            s.Run(beach, 300, pauses ? 60 : 0);
            if (verbose) s.Print(pauses ? "Beach Bowl, flat GPU time, a pause each minute" : "Beach Bowl, flat GPU time, no pause");
            bool downTwice = false, undoneAll = true, undoSoon = true;
            int lowest = kToday;
            for (size_t i = 0; i < s.ev.size(); ++i) {
                const Event& e = s.ev[i];
                if (e.kind != 'R') continue;
                lowest = std::min(lowest, e.to);
                if (e.to < e.from && e.from <= kToday) {
                    // the next change of rung must be the undo, for no gain, within 4 s check + 5 s wait + settling and a window
                    int next = -1;
                    for (size_t j = i + 1; j < s.ev.size(); ++j)
                        if (s.ev[j].kind == 'R') { next = static_cast<int>(j); break; }
                    if (next < 0) undoneAll = false;
                    else {
                        if (s.ev[next].to < s.ev[next].from) downTwice = true;
                        if (s.ev[next].why != Why::NoGain || s.ev[next].to != e.from) undoneAll = false;
                        if (s.ev[next].t - e.t > 11.0) undoSoon = false;
                    }
                }
            }
            char line[240];
            std::printf("    %s: %u changes of rung, lowest rung %d, %.1f s of 300 below 640x456\n", pauses ? "a pause each minute" : "no pause",
                        s.Count('R'), lowest, s.belowSec);
            Check(lowest >= 1 && !downTwice, "flat GPU time: never two steps down in a row, never below 592x422, never the emergency rung");
            Check(undoneAll, "... every step below 640x456 is undone for no gain");
            std::snprintf(line, sizeof line, "... undone %s within 11 s of the step", pauses ? "at a pause or in play" : "in play without a pause");
            Check(undoSoon, line);
            Check(s.belowSec < 30.0 && s.Count('R') <= 8, "... the picture is at 640x456 nearly all the time and changes rarely (blocks doubling)");
            Check(s.ctl.Current() == kToday, "... and ends at 640x456");
        }
    }
    {
        // GPU time that scales with the picture (the plaza's kind): 3 + 36 x Cost ms, 39 ms at 640x456, 34 ms at 592x422,
        // 29 ms at 544x388. Smaller pictures are taken and kept, also with the freeze.
        Load plaza;
        plaza.fixedGpuMs = 3;
        plaza.pixGpuMs = 36;
        plaza.fxGain = plaza.bgGain = 1.0;
        Sim s(Target(true), 90, 0.05);
        s.freezeUs = 260000;
        s.Run(plaza, 120);
        if (verbose) s.Print("GPU time that scales with the picture");
        Check(s.Index('R', 0, 1) >= 0 && s.ctl.Current() <= 1, "scaling GPU time: the picture goes below 640x456 and stays there");
        bool noGain = false;
        for (const Event& e : s.ev) noGain |= e.kind == 'R' && e.why == Why::NoGain;
        Check(!noGain, "... no step is undone for no gain");
        // and in legacy rules the same
        Config legacy = Config::Legacy();
        legacy.targetFps = 30;
        legacy.minRung = kEmergency;
        legacy.budgetMs = 1000.0 / 30.0;
        Sim l(legacy, 91, 0.05);
        l.Run(plaza, 120);
        noGain = false;
        for (const Event& e : l.ev) noGain |= e.kind == 'R' && e.why == Why::NoGain;
        Check(l.ctl.Current() <= 1 && !noGain, "... and with the legacy rules (GXV_AUTOQ_CALM=0)");
        // much heavier: down to the emergency rung, each step kept
        Load heavy = plaza;
        heavy.pixGpuMs = 60; // 63 ms at 640x456, 36.6 ms at 480x340
        Sim h(Target(true), 92, 0.05);
        h.freezeUs = 260000;
        h.Run(heavy, 150);
        noGain = false;
        for (const Event& e : h.ev) noGain |= e.kind == 'R' && e.why == Why::NoGain;
        if (verbose) h.Print("GPU time that scales, much heavier");
        Check(h.ctl.Current() == kEmergency && !noGain, "... a much heavier one reaches 480x340, each step kept");
    }
    {
        // Beach Bowl as the console's rc2 build showed it: each smaller size saves 5-6 % of the GPU time (24 + 13 x Cost ms: 37 at
        // 640x456, 35.1 at 592x422, 33.4 at 544x388, 31.3 at 480x340), 640x456 misses 30. The floor comes first: every such step
        // is kept, down to the size that meets 30.
        Load beach;
        beach.gameMs = 14;
        beach.fixedGpuMs = 24;
        beach.pixGpuMs = 13;
        beach.fxGain = beach.bgGain = 1.0;
        Sim s(Target(true), 93, 0.05);
        s.freezeUs = 260000;
        s.Run(beach, 120);
        if (verbose) s.Print("Beach Bowl, 4-6 % saved by each size, 640x456 misses 30");
        bool noGain = false;
        for (const Event& e : s.ev) noGain |= e.kind == 'R' && e.why == Why::NoGain;
        const unsigned late = s.lateFrames;
        s.lateFrames = 0;
        s.Run(beach, 30);
        char line[200];
        std::snprintf(line, sizeof line, "small savings while 30 is missed: each step kept, down to 480x340 (rung %d), %u late pictures in the last 30 s",
                      s.ctl.Current(), s.lateFrames);
        Check(s.ctl.Current() == kEmergency && !noGain, line);
        Check(s.lateFrames <= 30 && late > 0, "... where the floor is met");
        // the same scene in Performance mode (60), against its own budget: the floor is missed all the way, every step kept
        Config p = Target(true);
        p.targetFps = 60;
        p.budgetMs = 1000.0 / 60.0;
        Load lighter = beach;
        lighter.gameMs = 6;       // (one turn's CPU at 60: game 3 ms, renderer 3 ms)
        lighter.fixedGpuMs = 12.5;
        lighter.pixGpuMs = 6.5;   // 19 ms at 640x456, 16.2 at 480x340
        Sim q(p, 94, 0.05);
        q.Run(lighter, 120);
        noGain = false;
        for (const Event& e : q.ev) noGain |= e.kind == 'R' && e.why == Why::NoGain;
        if (verbose) q.Print("the same in Performance mode");
        Check(q.ctl.Current() == kEmergency && !noGain, "... and in Performance mode (60) against 16.7 ms");
    }
    {
        // The scene remembers it: a stage whose GPU time does not change with the picture at all (key 333), another stage, the
        // first again: no useless step on the second visit.
        Load beach;
        beach.gameMs = 14;
        beach.fixedGpuMs = 37.5;
        beach.pixGpuMs = 0.5;
        beach.fxGain = beach.bgGain = 1.0;
        Load light;
        light.pixGpuMs = 5;
        Load plaza;
        plaza.fixedGpuMs = 3;
        plaza.pixGpuMs = 36;
        plaza.fxGain = plaza.bgGain = 1.0;
        Sim s(Target(true), 94, 0.05);
        s.freezeUs = 260000;
        s.Run(beach, 40, 0, 333);
        const bool firstTried = s.Index('R', 0, 1) >= 0;
        s.Frame(light, false, 444, 2500000); // a load, another stage
        s.Run(light, 10, 0, 444);
        s.Frame(beach, false, 333, 2500000); // a load, Beach Bowl again
        const double back = s.t;
        s.Run(beach, 120, 0, 333);
        int below = -1;
        for (size_t i = 0; i < s.ev.size(); ++i)
            if (s.ev[i].t >= back && s.ev[i].kind == 'R' && s.ev[i].to < kToday) { below = static_cast<int>(i); break; }
        s.Frame(plaza, false, 555, 2500000); // a load, a stage whose GPU time scales: nothing learnt elsewhere holds it back
        const double plazaFrom = s.t;
        s.Run(plaza, 60, 0, 555);
        if (verbose) s.Print("Beach Bowl, another stage, Beach Bowl again, a scaling stage");
        Check(firstTried, "setup: the first visit tries the step below 640x456 (and undoes it)");
        Check(below < 0, "the second visit of the stage takes no step below 640x456 (remembered with the scene)");
        Check(s.Index('R', plazaFrom, 1) >= 0, "a stage whose GPU time scales still steps down");
    }

    std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all checks passed", g_failures);
    return g_failures ? 1 : 0;
}
