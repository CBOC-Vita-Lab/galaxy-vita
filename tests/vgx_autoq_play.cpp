// Host-side test of the play-time quality options of the automatic quality controller (platform/vgx/src/vgx_autoq.cpp, vgx_autoq.h
// "Play-time quality"): GXV_AUTOQ_FREE (Config::freeFrac), GXV_AUTOQ_WIPE (Config::wipe), GXV_AUTOQ_LATEROOM (Config::lateRoom) and
// GXV_AUTOQ_ANISO_RUNG (Config::anisoRung). tools/test_vgx_autoq_play.sh builds and runs it.
//
// 0. With the options off every decision is the one made before they existed (the golden digests of the other controller tests);
//    outside target mode the controller switches them off; with them on, traces without wipes, loads or late pictures are unchanged.
// 1. free: a front end at 18 ms a picture and a GPU at 12 ms at 640x456: without the option the picture climbs to 960x544 and the
//    GPU (21.5 ms) becomes the limit (fewer pictures); with it the size stops where the GPU stays within 85 % of the interval and
//    the picture rate stays the front end's. A size above today's at which the GPU sets the pace steps back to where it does not.
// 2. wipe: a closed fade/wipe never steps up (the room before it is forgotten), and a rung above today's goes back to it there;
//    the pause menu still steps up; a load in the same stage and a new stage do not start above today's rung.
// 3. lateRoom: the front end's late pictures (one every 0.7 s, the GPU light) no longer keep the picture below today's size.
// 5. overload: the r41 drop-in (GPU ~100 ms a picture at 640x456, the game's estimate over the budget too): without the option nothing
//    happens for a minute; with it the first change comes within 1.5 s, in one step, and the floor is reached within a few seconds;
//    a moderate overload (GPU ~45 ms, game estimate ~40 ms) is now seen as GPU-limited; light scenes are untouched.
// 4. anisoRung: anisotropic filtering comes on at 640x456 and up when the GPU has room, never below it, goes off at any change of
//    size and when the GPU sets the pace (with free), and stays off without the option at the rungs below the top.
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

// ---------------------------------------------------------------------------------------------------- the golden traces
// (copied from tests/vgx_autoq_aniso.cpp: the controller as it was before these options)
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
// The picture interval is the slowest of: the front end (unmeasured by the controller, the console's limit), the renderer, the GPU;
// the game's two turns are measured but overlap. Not paced to vblanks (PRESENT_SYNC's mean interval is the same).
struct Load {
    double frontMs = 18;                       // the front end a picture (not measured by the controller)
    double gameMs = 13, backendMs = 8;         // two game turns' CPU, renderer CPU a picture
    double fixedGpuMs = 0, pixGpuMs = 12;      // GPU: fixed + pixGpuMs x Cost(rung)
    double anisoCost = 1.12;                   // GPU time with anisotropic filtering / without
    double lateEverySec = 0, lateMs = 45;      // a late front-end picture every lateEverySec (0: none)
    double bgGain = 1.1, fxGain = 1.4;         // GPU time without the cheaper background / half-size effects, over with
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
    double t = 0, nextLate = 0;
    std::vector<Event> ev;
    bool anisoPrev = false;
    bool anisoBelowMin = false;      // anisotropic filtering on below the lowest rung allowed
    double pictures = 0, playTime = 0;
    Sim(const Config& c, uint32_t seed, double n) : ctl(c), rng(seed), noise(n) {}
    double Gpu(const Load& l) const {
        return (l.fixedGpuMs + l.pixGpuMs * Cost(ctl.Current())) * (ctl.AnisoOn() ? l.anisoCost : 1.0) / (ctl.BgStepOn() ? l.bgGain : 1.0) /
            (ctl.FxStepOn() ? l.fxGain : 1.0);
    }
    void Frame(const Load& l, bool quiet = false, bool wipe = false, uint32_t scene = 0, double forceMs = 0) {
        std::uniform_real_distribution<double> u(-noise, noise);
        const double gms = l.gameMs * (1.0 + u(rng)), bms = l.backendMs * (1.0 + u(rng)), fe = l.frontMs * (1.0 + u(rng));
        const double gpu = Gpu(l) * (1.0 + u(rng));
        double ms = std::max({fe, bms, gpu});
        if (l.lateEverySec > 0 && t >= nextLate) { ms = std::max(ms, l.lateMs); nextLate = t + l.lateEverySec; }
        if (forceMs > 0) ms = forceMs;
        const uint64_t interval = static_cast<uint64_t>(ms * 1000.0);
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
        f.gameKnown = true;
        f.timers = f.busyKnown = true;
        f.gpuBusyUs = static_cast<uint32_t>(gpu * 1000.0);
        f.quiet = quiet;
        f.wipe = wipe;
        f.scene = scene;
        const int before = ctl.Current();
        const bool changed = ctl.Add(f);
        t += static_cast<double>(interval) / 1e6;
        if (!quiet && forceMs == 0) { pictures += 1; playTime += static_cast<double>(interval) / 1e6; }
        if (changed || ctl.Current() != before) ev.push_back({t, 'R', before, ctl.Current(), ctl.LastChange().why});
        if (ctl.AnisoOn() != anisoPrev) {
            ev.push_back({t, ctl.AnisoOn() ? 'A' : 'a', ctl.Current(), ctl.Current(), ctl.LastAnisoChange().why});
            anisoPrev = ctl.AnisoOn();
        }
        if (ctl.AnisoOn() && ctl.Current() < ctl.AnisoMinRung()) anisoBelowMin = true;
    }
    // `pauseEvery`: the last 2 s of every such period are the pause menu (quiet, not a wipe); `wipeEvery`: the last 1 s a closed wipe
    void Run(const Load& l, double seconds, double pauseEvery = 0, double wipeEvery = 0, uint32_t scene = 0) {
        const double end = t + seconds, start = t;
        while (t < end) {
            const bool pause = pauseEvery > 0 && std::fmod(t - start, pauseEvery) >= pauseEvery - 2.0;
            const bool wipe = !pause && wipeEvery > 0 && std::fmod(t - start, wipeEvery) >= wipeEvery - 1.0;
            Frame(l, pause || wipe, wipe, scene);
        }
    }
    void Pause(const Load& l, double seconds, bool wipe, uint32_t scene = 0) {
        const double end = t + seconds;
        while (t < end) Frame(l, true, wipe, scene);
    }
    void Gap(const Load& l, double ms, uint32_t scene) { Frame(l, false, false, scene, ms); }
    double Fps() const { return playTime > 0 ? pictures / playTime : 0; }
    void ResetRate() { pictures = playTime = 0; }
    int Index(char kind, double after = -1, int to = 9999) const {
        for (size_t i = 0; i < ev.size(); ++i)
            if (ev[i].t >= after && ev[i].kind == kind && (to == 9999 || ev[i].to == to)) return static_cast<int>(i);
        return -1;
    }
    unsigned Count(char kind, double after = -1, Why why = Why::None) const {
        unsigned n = 0;
        for (const Event& e : ev) n += e.kind == kind && e.t >= after && (why == Why::None || e.why == why);
        return n;
    }
    int MaxRung(double from, double to) const {
        int m = -9;
        for (const Event& e : ev)
            if (e.kind == 'R' && e.t >= from && e.t < to) m = std::max(m, e.to);
        return m;
    }
    void Print(const char* title) const {
        std::printf("    %s:\n", title);
        for (const Event& e : ev) {
            if (e.kind == 'R') std::printf("      t=%7.1f s  rung %2d -> %2d (%s)\n", e.t, e.from, e.to, WhyText(e.why));
            else std::printf("      t=%7.1f s  anisotropic filtering %s at rung %d (%s)\n", e.t, e.kind == 'A' ? "ON " : "off", e.from, WhyText(e.why));
        }
    }
};

Config Target(double freeFrac = 0, bool wipe = false, bool lateRoom = false, bool aniso = false, int anisoRung = kNoRung) {
    Config c;
    c.targetFps = 30;
    c.minRung = kEmergency;
    c.budgetMs = 1000.0 / 30.0;
    c.recover = true;
    c.recoverHoldSec = 10.0; // (the Vita's GXV_AUTOQ_RECOVER_SEC=10)
    c.freeFrac = freeFrac;
    c.wipe = wipe;
    c.lateRoom = lateRoom;
    c.aniso = aniso;
    c.anisoRung = anisoRung;
    return c;
}
} // namespace

int main(int argc, char** argv) {
    const bool verbose = argc > 1 && std::string(argv[1]) == "verbose";

    std::printf("options off: the decisions are those of the controller before them\n");
    Check(Digest(Config(), 15) == kGoldenCalm, "calm rules: every change of 28 seeded traces is unchanged");
    Check(Digest(Config::Legacy(), 15) == kGoldenLegacy, "legacy rules: the same traces give the same changes");
    {
        Config on;
        on.freeFrac = 0.85;
        on.wipe = on.lateRoom = true;
        on.anisoRung = kToday;
        Controller c(on);
        Check(c.Cfg().freeFrac == 0.0 && !c.Cfg().wipe && !c.Cfg().lateRoom, "outside target mode the controller switches them off");
        Check(Digest(on, 3) == Digest(Config(), 3), "... and the traces without a target are unchanged");
        Config onLegacy = Config::Legacy();
        onLegacy.wipe = onLegacy.lateRoom = true;
        onLegacy.freeFrac = 0.85;
        Check(Digest(onLegacy, 3) == Digest(Config::Legacy(), 3), "legacy rules with them: unchanged");
        // (the golden traces have no wipes, no loads and no late pictures: those two options change nothing there)
        Config w;
        w.wipe = w.lateRoom = true;
        Check(Digest(w, 15) == kGoldenCalm, "wipe and lateRoom on, traces without wipes, loads or late pictures: every change unchanged");
    }

    std::printf("free: quality never costs pictures\n");
    {
        Load l; // front end 18 ms; GPU 12 ms at 640x456 (21.5 ms at 960x544)
        Sim off(Target(), 11, 0.03), on(Target(0.85), 11, 0.03);
        off.Run(l, 60);
        on.Run(l, 60);
        off.ResetRate();
        on.ResetRate();
        off.Run(l, 120);
        on.Run(l, 120);
        if (verbose) { off.Print("without free"); on.Print("with free 85 %"); }
        std::printf("    without: rung %d, %.1f pictures a second; with: rung %d, %.1f pictures a second\n", off.ctl.Current(), off.Fps(), on.ctl.Current(),
                    on.Fps());
        Check(off.ctl.Current() >= 5 && off.Fps() < 50.0, "without it the size climbs until the GPU is the limit (fewer pictures than the front end makes)");
        Check(on.ctl.Current() == 3, "with it the size stops at 720x480 (12 x 1.18 = 14.2 ms within 85 % of 18 ms; 800x504 would need 16.6)");
        Check(on.Fps() > 53.0, "... and the picture rate stays the front end's (about 55)");
        Check(on.Count('R', -1, Why::FreeDown) == 0, "... without stepping back (no flicker)");
    }
    {
        // a size above today's at which the GPU sets the pace (the scene got heavier): steps back one rung at a time, to where it does not
        Load l;
        l.frontMs = 16;
        Config c = Target(0.85);
        c.startRung = 5;
        Sim s(c, 12, 0.03);
        s.Run(l, 90);
        if (verbose) s.Print("start at 880x520, GPU 18.8 ms against a 16 ms front end");
        Check(s.Count('R', -1, Why::FreeDown) >= 2, "steps back with its reason (the GPU sets the picture rate)");
        const int first = s.Index('R');
        Check(first >= 0 && s.ev[first].t >= 5.9 && s.ev[first].to == 4, "... the first after freeDownSec (6 s), one rung");
        Check(s.ctl.Current() == kToday || s.ctl.Current() == kToday + 1, "... to where the GPU no longer sets the pace (640x456 or 720x480 here)");
        Check(s.Count('R', -1, Why::Headroom) == 0, "... and does not climb back into the blocked rungs");
        Config c0 = c;
        c0.freeFrac = 0;
        Sim off(c0, 12, 0.03);
        off.Run(l, 90);
        Check(off.ctl.Current() >= 5 && off.Count('R', -1, Why::FreeDown) == 0, "without the option it never steps back (and even climbs: the GPU is far from the 30 budget)");
    }

    std::printf("wipe: fades, wipes and loads lead into unmeasured areas\n");
    {
        // 9 s of room, then a closed wipe: without the option the step up is taken in the wipe, with it not
        Load light;
        light.pixGpuMs = 7;
        Sim off(Target(0, false), 21, 0.03), on(Target(0, true), 21, 0.03);
        for (Sim* s : {&off, &on}) {
            s->Run(light, 9.0);
            s->Pause(light, 1.0, true);
            s->Run(light, 3.0);
        }
        if (verbose) { off.Print("room, then a wipe (without)"); on.Print("room, then a wipe (with)"); }
        Check(off.MaxRung(9.0, 10.5) > kToday, "without the option a closed wipe takes the step up the play before it allowed");
        Check(on.Count('R') == 0 && on.ctl.Current() == kToday, "with it the wipe takes no step up");
        // the pause menu still does
        Sim p(Target(0, true), 22, 0.03);
        p.Run(light, 9.0);
        p.Pause(light, 1.0, false);
        Check(p.ctl.Current() > kToday && p.ev.size() && p.ev[0].why == Why::Headroom, "the pause menu (not a wipe) still takes it");
        // above today, a wipe goes back to today while the screen is closed, and the area after it starts measuring afresh
        const double before = p.t;
        p.Run(light, 4.0);
        const int high = p.ctl.Current();
        p.Pause(light, 1.0, true);
        const int w = p.Index('R', before + 3.9);
        Check(w >= 0 && p.ev[w].from == high && p.ev[w].to == kToday && p.ev[w].why == Why::Wipe, "a closed wipe above today: back to today, with its reason");
        Check(p.ctl.BlockedUntilUs(high) == 0 || p.ctl.BlockedUntilUs(high) < 1000, "... without blocking the rung it left");
        const double after = p.t;
        p.Run(light, 8.0);
        Check(p.MaxRung(after, after + 8.0) == -9, "... and no step up in the first 8 s after it (the room before it is forgotten)");
        p.Run(light, 30.0);
        Check(p.ctl.Current() > kToday, "the size climbs again once the new area has shown room (recoverHoldSec in play)");
    }
    {
        // a load (a long gap) inside the same stage, and a new stage remembered above today's
        Load light;
        light.pixGpuMs = 7;
        Sim s(Target(0, true), 23, 0.03), off(Target(0, false), 23, 0.03);
        for (Sim* x : {&s, &off}) {
            x->Run(light, 9.0, 0, 0, 0x1111);
            x->Pause(light, 1.0, false, 0x1111); // (a pause: up)
            x->Run(light, 2.0, 0, 0, 0x1111);
        }
        const int high = s.ctl.Current();
        Check(high > kToday && off.ctl.Current() == high, "(setup: the pause menu stepped up)");
        s.Gap(light, 3700, 0x1111);
        off.Gap(light, 3700, 0x1111);
        Check(off.ctl.Current() == high, "without the option a 3.7 s load in the same stage keeps the size (r27: the drop-in at 880x520)");
        Check(s.ctl.Current() == kToday && s.ctl.LastChange().why == Why::Wipe, "with it the load inside the stage goes back to today's size");
        // new stage, then back to the first one: it is not resumed above today
        Sim m(Target(0, true), 24, 0.03);
        m.Run(light, 9.0, 0, 0, 0x2222);
        m.Pause(light, 1.0, false, 0x2222);
        m.Run(light, 2.0, 0, 0, 0x2222);
        Check(m.ctl.Current() > kToday, "(setup: stage A above today)");
        m.Gap(light, 3000, 0x3333);
        m.Run(light, 2.0, 0, 0, 0x3333);
        m.Gap(light, 3000, 0x2222);
        Check(m.ctl.Current() == kToday, "a stage remembered above today starts at today's size");
    }
    {
        // a scene that is GPU-limited at today's size keeps its step below today across a wipe (the rule only caps above today)
        Load heavy;
        heavy.frontMs = 18;
        heavy.pixGpuMs = 36;
        Sim s(Target(0.85, true, true), 25, 0.03);
        s.Run(heavy, 40.0, 0, 13.0);
        if (verbose) s.Print("GPU-limited at today's size, a wipe every 13 s");
        Check(s.ctl.Current() < kToday, "a GPU-limited scene still goes below today's size (wipes do not stop that)");
        Check(s.Count('R', -1, Why::Wipe) == 0, "... and a wipe there changes nothing");
    }

    std::printf("lateRoom: the front end's late pictures do not keep the picture small\n");
    {
        Load l;
        l.pixGpuMs = 9;
        l.lateEverySec = 0.7; // (r27: 5-9 late pictures every 5 s, none of them the GPU's)
        Config c = Target(0.85, true, false);
        c.startRung = 1;
        Config cl = c;
        cl.lateRoom = true;
        Sim off(c, 31, 0.03), on(cl, 31, 0.03);
        off.Run(l, 150);
        on.Run(l, 150);
        if (verbose) { off.Print("592x422, late front-end pictures (without)"); on.Print("(with)"); }
        Check(off.ctl.Current() == 1 && off.Count('R') == 0, "without the option: 592x422 for 150 s (each late picture ends the stretch of room)");
        const int up = on.Index('R');
        Check(up >= 0 && on.ev[up].to >= kToday && on.ev[up].t < 25.0, "with it: back to 640x456 or above within 25 s");
        Check(on.Count('R', -1, Why::FreeDown) == 0 && on.Count('R', -1, Why::Overload) == 0, "... and nothing to undo afterwards");
        // a late picture that is the GPU's own still ends the stretch
        Load g = l;
        g.lateEverySec = 0;
        g.pixGpuMs = 9;
        Sim gpuLate(cl, 32, 0.03);
        for (int i = 0; i < 150 * 50; ++i) {
            // every 0.7 s one picture whose GPU work is 40 ms: predicted at the next rung over the budget
            const bool spike = std::fmod(gpuLate.t, 0.7) < 0.02;
            Load x = g;
            if (spike) x.pixGpuMs = 9 * 40.0 / 9.0 / Cost(gpuLate.ctl.Current());
            gpuLate.Frame(x);
            if (gpuLate.t > 150) break;
        }
        Check(gpuLate.ctl.Current() <= kToday, "a late picture whose GPU work does not fit still ends the stretch (no climb above today)");
    }

    std::printf("anisoRung: anisotropic filtering at the rungs normal play holds\n");
    {
        Load l; // front end 18 ms, GPU 12 ms at 640x456: anisotropic filtering (13.4 ms) comes on first and then 720x480 (15.9 ms) no longer fits
        Sim s(Target(0.85, true, true, true, kToday), 41, 0.03);
        s.Run(l, 120);
        if (verbose) s.Print("anisoRung 2, free 85 %");
        const int an = s.Index('A');
        Check(an >= 0 && s.ev[an].from >= kToday && s.ev[an].from < kRungs - 1, "it comes on below the top rung");
        Check(s.ctl.AnisoOn(), "... and stays on while the GPU has room");
        Check(!s.anisoBelowMin, "it is never on below 640x456");
        // the scene gets heavier: the GPU sets the pace -> it goes first, before any rung
        Load h = l;
        h.pixGpuMs = 16.5; // 16.5 x 1.12 = 18.5 ms at 640x456 against the 18 ms front end
        const double t0 = s.t;
        s.Run(h, 30);
        if (verbose) s.Print("heavier");
        const int off = s.Index('a', t0);
        const int down = s.Index('R', t0);
        Check(off >= 0 && s.ev[off].why == Why::AnisoOff, "when the GPU sets the pace it goes off first");
        Check(down < 0 || s.ev[down].t > s.ev[off].t, "... before any rung changes");
        Check(!s.anisoBelowMin, "never on below 640x456 (also after the change)");
        // without anisoRung: never on at these rungs
        Sim old(Target(0.85, true, true, true), 41, 0.03);
        old.Run(l, 120);
        Check(old.Count('A') == 0, "without GXV_AUTOQ_ANISO_RUNG it stays the top rung's (off here)");
        // a change of rung has it off
        Load light;
        light.frontMs = 30;
        light.pixGpuMs = 6;
        Sim c(Target(0.85, true, true, true, kToday), 42, 0.03);
        bool onAtChange = false;
        int prev = c.ctl.Current();
        while (c.t < 200) {
            c.Frame(light);
            if (c.ctl.Current() != prev && c.ctl.AnisoOn()) onAtChange = true;
            prev = c.ctl.Current();
        }
        if (verbose) c.Print("light, 30 ms front end");
        Check(c.Count('A') >= 1 && !onAtChange, "it comes on at the rungs on the way up and every change of size has it off");
        double b = 0, a = 0;
        Check(c.ctl.TakeAnisoMeasure(b, a) || c.Count('A') >= 1, "its measured cost is offered to the log (diagnostics)");
    }

    std::printf("overload: a severe GPU overload in play is acted on within a second or two\n");
    {
        auto cfgOf = [](bool on, bool fx) {
            Config c = Target(0.85, true, true, true, kToday);
            c.bgStep = true; // (the Vita's GXV_AUTOQ_FXSTEP=3: the cheaper background only)
            c.overload = on;
            c.overloadFx = fx;
            return c;
        };
        Load drop;
        drop.frontMs = 18;
        drop.gameMs = 30;     // (two turns: the r41 floor lines blamed the game in a third of the late pictures)
        drop.backendMs = 10;
        drop.fixedGpuMs = 5;
        drop.pixGpuMs = 105;  // 110 ms a picture at 640x456: ~9 FPS, every picture over outlierMs (100 ms) as in the r41 log
        Sim off(cfgOf(false, false), 51, 0.05), on(cfgOf(true, true), 51, 0.05);
        off.Run(drop, 60);
        on.Run(drop, 60);
        if (verbose) { off.Print("drop-in, without"); on.Print("drop-in, GXV_AUTOQ_OVERLOAD=2"); }
        Check(off.Count('R') == 0 && !off.ctl.BgStepOn(), "without the option: no reaction in 60 s (the r41 log: nothing at all)");
        const int first = on.Index('R');
        Check(first >= 0 && on.ev[first].t <= 1.6 && on.ev[first].why == Why::Severe, "with it: the first change within 1.6 s, with its reason");
        Check(on.ctl.BgStepOn() && on.ctl.FxStepOn(), "... the cheaper background and half-size effects on (=2)");
        Check(on.ctl.Current() == kEmergency, "... and the picture at the emergency rung (110 ms of GPU does not fit otherwise)");
        Check(first >= 0 && on.ev[first].to == kEmergency, "... in one change, not a rung at a time");
        Check(on.Count('R') <= 2, "... and kept (the check finds the GPU time fell): no back and forth");
        // =1: no half-size effects when FXSTEP leaves them out
        Sim one(cfgOf(true, false), 51, 0.05);
        one.Run(drop, 20);
        Check(one.ctl.BgStepOn() && !one.ctl.FxStepOn() && one.ctl.Current() == kEmergency, "=1: the background and the size, no half-size effects");
        // the overload ends: the way back is the usual one (room for recoverHoldSec, the steps off before the size rises above today)
        Load calm;
        on.Run(calm, 120);
        if (verbose) on.Print("then a light area");
        Check(on.ctl.Current() >= kToday && !on.ctl.FxStepOn() && !on.ctl.BgStepOn(), "after it, a light area brings everything back");
    }
    {
        // 2.5x the budget at 880x520, from a quiet step up: straight to what fits, the rung above today blocked
        Load drop;
        drop.frontMs = 18;
        drop.gameMs = 20;
        drop.pixGpuMs = 50; // 78 ms at 880x520, 50 at 640x456
        Config c = Target(0.85, false, true, true, kToday);
        c.bgStep = true;
        c.overload = true;
        c.startRung = 5;
        Sim s(c, 52, 0.1);
        s.Run(drop, 20);
        if (verbose) s.Print("880x520 into a drop-in");
        const int first = s.Index('R');
        Check(first >= 0 && s.ev[first].t <= 1.6 && s.ev[first].from == 5 && s.ev[first].to < kToday, "from 880x520: below today's size within 1.6 s, one change");
        Check(s.ctl.BlockedUntilUs(5) > 0, "... the rung left blocked");
    }
    {
        // a moderate overload (GPU ~38 ms with noise, the game's estimate ~40 ms): seen as GPU-limited now; nothing without the option
        Load mid;
        mid.frontMs = 18;
        mid.gameMs = 40;
        mid.pixGpuMs = 38;
        Config c = Target(0.85, true, true, true, kToday);
        c.bgStep = true;
        Config c1 = c;
        c1.overload = true;
        Sim off(c, 53, 0.25), on(c1, 53, 0.25);
        off.Run(mid, 60);
        on.Run(mid, 60);
        if (verbose) { off.Print("moderate, without"); on.Print("moderate, with"); }
        Check(off.Count('R') == 0 && !off.ctl.BgStepOn(), "without the option a GPU at 38 ms beside a 40 ms game estimate is never GPU-limited");
        Check(on.Count('R', -1, Why::Severe) == 0, "with it this is no severe overload (the regular, GPU-timed steps)");
        Check(on.ctl.BgStepOn() || on.ctl.Current() < kToday, "with it the controller steps (background, or a smaller picture)");
        Check(on.Count('R') + on.ctl.FxChanges() > 0 && (on.Index('R') < 0 || on.ev[on.Index('R')].t < 30.0), "... within 30 s");
    }
    {
        // light and normal scenes: nothing changes with the option (the golden digests already hold for traces under the budget)
        Load l;
        Sim a(Target(0.85, true, true, true, kToday), 54, 0.05);
        Config c = Target(0.85, true, true, true, kToday);
        c.overload = true;
        Sim b(c, 54, 0.05);
        a.Run(l, 120);
        b.Run(l, 120);
        Check(a.ev.size() == b.ev.size() && a.ctl.Current() == b.ctl.Current(), "a normal scene: the same decisions with and without it");
        // Performance mode (target 60): a GPU at 25 ms misses 60 but is no severe overload (that is against the 30 floor)
        Config p60 = c;
        p60.targetFps = 60;
        p60.budgetMs = 1000.0 / 60.0;
        Load l60;
        l60.frontMs = 12;
        l60.gameMs = 10;
        l60.backendMs = 6;
        l60.pixGpuMs = 25;
        Sim p(p60, 55, 0.05);
        p.Run(l60, 30);
        Check(p.Count('R', -1, Why::Severe) == 0, "target 60: a 25 ms GPU is not a severe overload (the regular steps of the mode)");
        Config g;
        g.overload = true;
        Check(Digest(g, 3) == Digest(Config(), 3), "golden traces without a target: unchanged with it (the controller switches it off)");
        std::printf("    target-30 golden traces with it: %s\n", Digest(g, 12) == Digest(Config(), 12) ? "unchanged" : "changed (their 3 s 40 ms bumps are now severe overloads)");
    }

    std::printf("%s (%d failed)\n", g_failures ? "FAILURES" : "all checks passed", g_failures);
    return g_failures ? 1 : 0;
}
