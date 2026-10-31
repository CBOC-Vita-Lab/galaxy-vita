// Host-side test of GXV_AUTOQ_RECOVER (platform/vgx/src/vgx_autoq.cpp, Config::recover): quicker, still quiet recovery of the
// picture size after an overload. tools/test_vgx_autoq_recover.sh builds and runs it.
//
// 1. With the option OFF the controller must make exactly the decisions it made before the option existed. A set of
//    seeded synthetic traces (calm and target-30 rules, fence and GPU-timer clocks, noise, bumps, pauses) is fed through the
//    controller and every change (frame, from, to, why) is hashed; the hash must equal kGolden*, which was taken from the
//    controller as it was before the option (the commit that added this file).
// 2. With the option ON: a short overload is recovered from in play without waiting for a pause or 120 s; noisy scenes
//    whose mean fits are no longer held down by single frames; a single spike or an overload still steps down no sooner and
//    no deeper than before; repeated rare overloads do not raise their block to 8 minutes.
#include "../platform/vgx/src/vgx_autoq.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
#include <algorithm>

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
    double pauseEvery;   // seconds between quiet moments (0: none); each lasts 2 s
    uint32_t seed;
};

Scene Light(double) { return {12, 3, 0, 5}; }
Scene Tight(double) { return {17, 3, 0, 6}; }
Scene HeavyBump(double t) { return (std::fmod(t, 200.0) >= 150.0 && std::fmod(t, 200.0) < 153.0) ? Scene{12, 3, 0, 40} : Scene{12, 3, 0, 5}; }
Scene Plaza(double) { return {14, 3, 3, 26}; }
Scene OneFrame(double t) { return (t >= 200.0 && t < 200.0 + 1.0 / 30.0 - 1e-6) ? Scene{12, 3, 0, 200} : Scene{12, 3, 0, 5}; }
Scene FiveFrames(double t) { return (t >= 200.0 && t < 200.0 + 5.0 / 30.0 - 1e-6) ? Scene{12, 3, 0, 60} : Scene{12, 3, 0, 5}; }
Scene Spikes(double t) { return std::fmod(t, 13.0) < 0.5 ? Scene{12, 3, 0, 18} : Scene{12, 3, 0, 5}; }

// One run: `frames` presented pictures at the controller's pace. interval is 33.3 ms (a fixed 30 Hz ceiling).
struct Outcome {
    int finalRung = 0;
    unsigned changes = 0, ups = 0, downs = 0, playUps = 0, playDowns = 0;
    double firstRecoverAt = -1; // seconds: first step up after the first step down
    double firstDownAt = -1;
    int maxRung = 0;
    std::vector<std::pair<double, std::pair<int, int>>> log;
    uint64_t digest = 0;
};

Outcome Run(const Config& cfg, const Trace& tr, bool timers, bool target, Fnv* fnv = nullptr, bool verbose = false) {
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
    Outcome o;
    o.finalRung = ctl.Current();
    const long frames = static_cast<long>(tr.seconds * 30.0);
    double t = 0;
    for (long i = 0; i < frames; ++i) {
        const Scene s = tr.scene(t);
        const double gms = s.gameMs * (1.0 + u(rng));
        game += static_cast<uint32_t>(gms * 500.0) * 2u; // 2 turns
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
            ++o.changes;
            if (ch.to > ch.from) {
                ++o.ups;
                if (!f.quiet) ++o.playUps;
                if (o.firstDownAt >= 0 && o.firstRecoverAt < 0) o.firstRecoverAt = t;
            } else {
                ++o.downs;
                if (!f.quiet) ++o.playDowns;
                if (o.firstDownAt < 0) o.firstDownAt = t;
            }
            o.log.push_back({t, {ch.from, ch.to}});
            if (verbose) std::printf("    t=%7.1f rung %d -> %d (%s)\n", t, ch.from, ch.to, WhyText(ch.why));
            if (fnv) {
                fnv->Add(static_cast<uint64_t>(i));
                fnv->Add(static_cast<uint64_t>(static_cast<int64_t>(ch.from)));
                fnv->Add(static_cast<uint64_t>(static_cast<int64_t>(ch.to)));
                fnv->Add(static_cast<uint64_t>(ch.why));
            }
        }
        o.maxRung = std::max(o.maxRung, ctl.Current());
        t += 1.0 / 30.0;
    }
    o.finalRung = ctl.Current();
    return o;
}

const Trace kTraces[] = {
    {"light, pause each minute", 400, Light, 0.10, 60, 1},
    {"light, no pause", 400, Light, 0.10, 0, 2},
    {"tight, noisy, pause each minute", 600, Tight, 0.20, 60, 3},
    {"bump every 200 s, pause each minute", 800, HeavyBump, 0.05, 60, 4},
    {"bump every 200 s, no pause", 800, HeavyBump, 0.05, 0, 5},
    {"plaza (GPU-limited)", 300, Plaza, 0.10, 45, 6},
    {"effect spikes every 13 s", 300, Spikes, 0.10, 60, 7},
};
const Trace kOneFrame = {"one 200 ms GPU frame at 200 s", 260, OneFrame, 0.05, 60, 8};
const Trace kFiveFrames = {"five frames of three times the budget at 200 s", 260, FiveFrames, 0.05, 60, 9};
const Trace& TraceNamed(const char* prefix) {
    for (const Trace& tr : kTraces)
        if (std::string(tr.name).rfind(prefix, 0) == 0) return tr;
    std::abort();
}

uint64_t Digest(const Config& cfg) {
    Fnv fnv;
    for (const Trace& tr : kTraces)
        for (int mode = 0; mode < 4; ++mode) {
            Config c = cfg;
            if (mode >= 2) c = cfg; // (target mode is set in Run)
            Run(c, tr, (mode & 1) != 0, mode >= 2, &fnv);
        }
    return fnv.h;
}
// Golden digests, taken with the controller before Config::recover existed.
constexpr uint64_t kGoldenCalm = 0x5ee9eca5260f8fddull;
// (legacy: retaken when a step below today in target mode came to be judged on the GPU's busy time over belowCheckSec, 4 s of
// wall time: the same changes, the "no gain" verdicts of the target-30 fence-clock traces 2-2.4 s later)
constexpr uint64_t kGoldenLegacy = 0x3b340e35fcf827d2ull;

} // namespace

int main(int argc, char** argv) {
    const bool printDigest = argc > 1 && std::string(argv[1]) == "digest";
    if (printDigest) {
        std::printf("calm 0x%016llxull\nlegacy 0x%016llxull\n", static_cast<unsigned long long>(Digest(Config())),
                    static_cast<unsigned long long>(Digest(Config::Legacy())));
        return 0;
    }
    std::printf("option off: the decisions are those of the controller before the option\n");
    Check(Digest(Config()) == kGoldenCalm, "calm rules: every change of 28 seeded traces (fence and GPU timers, calm and target 30) is unchanged");
    Check(Digest(Config::Legacy()) == kGoldenLegacy, "legacy rules: the same traces give the same changes");

    std::printf("option on (GPU timers clock, as on the console)\n");
    Config on;
    on.recover = true;
    const Config off;
    for (int target = 0; target < 2; ++target) {
        std::printf(" %s\n", target ? "target 30 (Steady 30)" : "calm rules");
        {
            const Trace& tr = TraceNamed("bump every 200 s, no pause");
            const Outcome a = Run(off, tr, true, target), b = Run(on, tr, true, target);
            std::printf("    %s: first step up after the first step down: %.0f s without the option, %.0f s with it\n", tr.name,
                        a.firstRecoverAt - a.firstDownAt, b.firstRecoverAt - b.firstDownAt);
            Check(a.firstRecoverAt - a.firstDownAt >= 100.0, "without the option: no step up in play for 100 s or more after an overload");
            Check(b.firstRecoverAt >= 0 && b.firstRecoverAt - b.firstDownAt <= 30.0, "with the option: the first step up is within 30 s of the overload, in play");
            Check(b.downs <= a.downs + (target ? 0u : 4u), "no step downs beyond those of the same traffic (the climbing back is what meets the next bump)");
        }
        {
            const Trace& tr = TraceNamed("tight, noisy");
            const Outcome a = Run(off, tr, true, target), b = Run(on, tr, true, target);
            std::printf("    %s: final rung %d (max %d) without, %d (max %d) with; changes %u / %u\n", tr.name, a.finalRung, a.maxRung, b.finalRung, b.maxRung, a.changes, b.changes);
            Check(b.maxRung >= a.maxRung && b.finalRung >= a.finalRung, "a noisy scene whose mean fits is not held lower than without the option");
            if (target) Check(a.finalRung < kRungs - 1 && b.finalRung == kRungs - 1, "target 30: single frames above the margin held the picture at a middle rung; with the option it reaches 960x544 4x");
        }
        for (const char* name : {"light, pause each minute", "light, no pause", "plaza", "effect spikes"}) {
            const Trace& tr = TraceNamed(name);
            const Outcome a = Run(off, tr, true, target), b = Run(on, tr, true, target);
            std::printf("    %s: rung %d, %u changes (%u down) without; rung %d, %u changes (%u down) with\n", tr.name, a.finalRung, a.changes, a.downs, b.finalRung, b.changes, b.downs);
            Check(b.downs <= a.downs, "no more step downs than without the option");
            Check(b.finalRung >= a.finalRung, "never ends lower than without the option");
            if (std::string(name).rfind("plaza", 0) == 0) Check(b.changes == 0 && a.changes == 0, "a GPU-limited scene is left alone");
        }
        for (const Trace* tr : {&kOneFrame, &kFiveFrames}) {
            const Outcome a = Run(off, *tr, true, target), b = Run(on, *tr, true, target);
            std::printf("    %s: %u step downs without, %u with\n", tr->name, a.downs, b.downs);
            Check(a.downs == 0 && b.downs == 0, "a spike of 1 or 5 frames lowers nothing (a step down needs 12 saturated frames, or 1.5 s of GPU limit)");
        }
    }
    {
        std::printf(" blocks\n");
        // A rung that is refused (no video memory) is blocked 60 s, the next time 120 s. Long after the last block the
        // doubling starts again, with the option on.
        for (int rec = 0; rec < 2; ++rec) {
            Config c;
            c.recover = rec != 0;
            Controller ctl(c);
            FrameTimes f;
            const uint64_t s = 1000000ull;
            uint64_t first = 0, second = 0;
            // (a picture a second from 9 s on: more than a gap (250 ms) but less than a load (1.5 s), so nothing is forgotten)
            for (uint64_t at = 9 * s; at <= 600 * s; at += s) {
                f.presentUs = at;
                ctl.Add(f);
                if (at == 10 * s) { ctl.Refused(2, 5, at); first = ctl.BlockedUntilUs(5) - at; }
                if (at == 100 * s) { ctl.Refused(2, 5, at); second = ctl.BlockedUntilUs(5) - at; } // (the block of 60 s has ended; the next is twice as long)
            }
            ctl.Refused(2, 5, 600 * s); // (500 s after the last)
            const uint64_t third = ctl.BlockedUntilUs(5) - 600 * s;
            std::printf("    option %s: blocks of %.0f s, %.0f s, then (500 s later) %.0f s\n", rec ? "on" : "off", first / 1e6, second / 1e6, third / 1e6);
            Check(first == 60 * s && second == 120 * s, "the second block is twice the first");
            Check(rec ? third == 60 * s : third == 240 * s, rec ? "with the option the doubling starts again after 300 s without a block" : "without the option the doubling goes on");
        }
    }
    std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all checks passed", g_failures);
    return g_failures ? 1 : 0;
}
