// Host-side test of the automatic quality controller (platform/vgx/src/vgx_autoq.cpp) with synthetic frame timings.
//   tools/test_vgx_autoq.sh            (builds and runs this; exit status 0 = every check passed)
//
// A small model of the console's pipeline drives the controller exactly as the back end does:
//   front end   makes picture n in `front` ms after picture n-1, and waits while the back end has not taken n-1;
//   back end    starts frame n when the picture is there, its previous frame is done, and the GPU has finished n-2
//               (the swap chain's back-pressure); submits for `sub` ms (CPU time), presents at the end;
//   GPU         starts after the frame's first scene is submitted and after frame n-1; takes gfix + gpix x cost(rung)
//               ms; cannot finish before the last scene is submitted plus that scene's share (20 %).
// The GPU clock's polling is modelled (completion times rounded up to 0.5 ms), samples reach the controller two
// frames late (as the watcher's results do), a rung change takes effect at the next frame and costs the GPU 2 ms
// once (the resample of the render targets).
#include "../platform/vgx/src/vgx_autoq.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <random>
#include <vector>

using namespace vgx::autoq;

extern "C" void port_autoq_game_begin(void) __attribute__((weak));
extern "C" void port_autoq_game_work_done(void) __attribute__((weak));
extern "C" int port_autoq_game_cpu_totals(uint32_t*, uint32_t*, uint32_t*) __attribute__((weak));
extern "C" uint64_t port_autoq_picture_work_us(void) __attribute__((weak));
static int64_t g_fakeCpuUs = 100;
static unsigned g_cpuReads = 0;
extern "C" int64_t port_thread_cpu_us(void) { ++g_cpuReads; return g_fakeCpuUs; }

namespace {
bool g_timers = false; // feed the controller as the GPU timers clock does (exact busy time per frame) instead of the frame fence
// The configuration the scenarios run with (Scenarios runs once with the legacy rules, once with calm mode), and what a
// change of rung costs the back end in the model: 0 = the GPU's 2 ms only; the console's 23 Oct run: 100-270 ms of a
// frozen picture (new render targets and the resample), 35-90 ms for multisampling alone.
vgx::autoq::Config g_base;
double g_changeStallMs = 0.0, g_msaaStallMs = 0.0;
vgx::autoq::Config Base() { return g_base; }

struct Load {
    double front, sub, gfix, gpix; // ms
    double gpow = 1.0;              // GPU time grows with cost^gpow (1: with the pixel count)
};

struct Result {
    int finalRung = 0, maxRung = 0, minRung = 99;
    unsigned changes = 0, ups = 0, downs = 0, vetoes = 0;
    unsigned minGapFrames = 1u << 30; // fewest frames between two changes (a new scene's return to the start rung not counted)
    double meanIntervalLast10s = 0;   // ms, presents over the last 10 s
    double todayIntervalLast10s = 0;  // the same scene at rung 2 with the controller off
    double slowFrac = 0;              // time spent with frames slower than rung 2 would give (over 2 %), share of the run
    std::vector<int> rungAt;          // rung at each whole second (-1: no picture in that second)
    struct Frame { double t, ms; int rung; };
    std::vector<Frame> frames;        // every frame: start time (s, picks the scene), interval (ms), rung
    double stallMs = 0;               // frozen picture the changes cost (g_changeStallMs)
    double playStallMs = 0;           // ... of changes during play (not in a quiet moment, not a new scene's)
    unsigned playChanges = 0;         // changes not in a quiet moment and not a new scene's
};

double Gpu(const Load& l, int rung) { return l.gfix + l.gpix * std::pow(Cost(rung), l.gpow); }

// Runs `seconds` of the model. scene(t) gives the load at time t (seconds). gap(t) returns a stall (ms) to insert
// before the frame that starts at t (a load), 0 for none. fixedRung >= 0: no controller.
Result Run(double seconds, const std::function<Load(double)>& scene, int fixedRung = -1, double noise = 0.0,
           const std::function<double(double)>& gap = nullptr, uint32_t seed = 1, Config cfg = Config(), bool log = false,
           const std::function<bool(int)>& fits = nullptr, const std::function<bool(double)>& quiet = nullptr,
           const std::function<uint32_t(double)>& sceneKey = nullptr) {
    Controller c(cfg);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(-noise, noise);
    auto jit = [&](double v) { return v * (1.0 + u(rng)); };
    Result r;
    double F = 0, S = 0, E = 0, Dm1 = 0, Dm2 = 0, run = 0;
    int rung = fixedRung >= 0 ? fixedRung : c.Current();
    int nextRung = rung;
    bool nextPlay = false;
    double changeCost = 0;
    struct Pending { FrameTimes f; };
    std::deque<FrameTimes> pending;
    std::vector<std::pair<double, double>> presents; // (time s, interval ms)
    double prevPresent = -1;
    int lastSecond = -1;
    long frameNo = 0, lastChangeFrame = -1; // (frames handed to the controller)
    while (E / 1000.0 < seconds) {
        const double t = S / 1000.0;
        const Load l = scene(t);
        double stall = gap ? gap(t) : 0.0;
        if (nextRung != rung) {
            if (nextPlay) r.playStallMs += (kLadder[nextRung].w == kLadder[rung].w && kLadder[nextRung].h == kLadder[rung].h) ? g_msaaStallMs : g_changeStallMs;
            const double st = (kLadder[nextRung].w == kLadder[rung].w && kLadder[nextRung].h == kLadder[rung].h) ? g_msaaStallMs : g_changeStallMs;
            stall += st;
            r.stallMs += st;
            rung = nextRung;
            changeCost = 2.0;
        }
        // front end
        F = std::max(F, S) + jit(l.front) + stall;
        // back end
        const double start = std::max({F, E, Dm2});
        const double sub = jit(l.sub);
        const double end = start + sub;
        const double g = jit(Gpu(l, rung)) + changeCost;
        changeCost = 0;
        const double gStart = std::max(Dm1, start + 0.1 * sub);
        double D = std::max(gStart + g, end + 0.2 * g);
        run += sub;
        S = start;
        E = end;
        Dm2 = Dm1;
        Dm1 = D;
        const double Dq = std::ceil(D / 0.5) * 0.5; // the watcher's polling
        FrameTimes f;
        f.startUs = static_cast<uint64_t>(start * 1000.0) + 1;
        f.presentUs = static_cast<uint64_t>(end * 1000.0) + 1;
        f.gpuDoneUs = static_cast<uint64_t>(Dq * 1000.0) + 1;
        f.backendRunUs = static_cast<uint64_t>(run * 1000.0) + 1;
        f.quiet = quiet && quiet(t);
        f.scene = sceneKey ? sceneKey(t) : 0;
        if (g_timers) { // the GPU timers clock: the frame's own GPU work, no completion time
            f.timers = true;
            f.busyKnown = true;
            f.gpuBusyUs = static_cast<uint32_t>(g * 1000.0);
            f.gpuDoneUs = 0;
        }
        if (prevPresent >= 0) {
            presents.push_back({end / 1000.0, end - prevPresent});
            if (stall == 0.0) r.frames.push_back({t, end - prevPresent, rung});
        }
        prevPresent = end;
        r.maxRung = std::max(r.maxRung, rung);
        r.minRung = std::min(r.minRung, rung);
        const int sec = static_cast<int>(end / 1000.0);
        while (lastSecond < sec) { r.rungAt.push_back(lastSecond + 1 == sec ? rung : -1); ++lastSecond; } // (-1: a second without a picture, a load)
        if (fixedRung >= 0) continue;
        pending.push_back(f);
        while (pending.size() > 2) {
            const FrameTimes done = pending.front();
            pending.pop_front();
            ++frameNo;
            if (c.Add(done)) {
                const Change& ch = c.LastChange();
                if (fits && ch.to > ch.from && !fits(ch.to)) { // (the back end's video-memory check)
                    int best = ch.to - 1;
                    while (best > ch.from && !fits(best)) --best;
                    ++r.vetoes;
                    if (log) std::printf("    t=%6.1f s  rung %d refused (no memory), %d taken\n", end / 1000.0, ch.to, best);
                    c.Limit(best, done.presentUs);
                    if (best == ch.from) continue;
                }
                ++r.changes;
                if (ch.why != Why::NewScene) {
                    if (lastChangeFrame >= 0) r.minGapFrames = std::min<unsigned>(r.minGapFrames, static_cast<unsigned>(frameNo - lastChangeFrame));
                    lastChangeFrame = frameNo;
                } else {
                    lastChangeFrame = -1;
                }
                if (ch.to > ch.from) ++r.ups; else ++r.downs;
                nextPlay = ch.why != Why::NewScene && !done.quiet;
                if (nextPlay) ++r.playChanges;
                if (log)
                    std::printf("    t=%6.1f s  rung %d -> %d (%s): load %.2f cpu %.2f interval %.1f ms\n", end / 1000.0, ch.from, ch.to, WhyText(ch.why), ch.load,
                                ch.cpu, ch.intervalMs);
                nextRung = c.Current();
            }
        }
    }
    r.finalRung = rung;
    double sum = 0;
    unsigned n = 0;
    for (const auto& p : presents)
        if (p.first >= seconds - 10.0) { sum += p.second; ++n; }
    r.meanIntervalLast10s = n ? sum / n : 0;
    return r;
}

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

// Share of the time in [from, to) of scenes that are GPU-bound at today's size (gpu(today) >= the front end's and the
// back end's times) spent in frames drawn above today's size (frames after a load stall only).
double OverBudgetShare(const Result& r, const std::function<Load(double)>& scene, double from, double to) {
    double over = 0, all = 0;
    for (const Result::Frame& f : r.frames) {
        if (f.t < from || f.t >= to) continue;
        const Load l = scene(f.t);
        const double today = Gpu(l, kToday);
        if (!(today >= l.front && today >= l.sub)) continue;
        all += f.ms;
        if (f.rung > kToday) over += f.ms;
    }
    return all > 0 ? over / all : 0.0;
}

} // namespace

int Scenarios() {
    std::printf("ladder:");
    for (int i = 0; i < kRungs; ++i) std::printf(" %d=%ux%u%s(%.2f)", i, kLadder[i].w, kLadder[i].h, kLadder[i].samples > 1 ? (kLadder[i].samples > 2 ? "x4" : "x2") : "", Cost(i));
    std::printf("\n");

    // 1. The Comet Observatory: the front end is the limit (30 ms a picture), the back end submits in 11 ms, the GPU
    //    needs 8 ms at 640x456. Expected: climbs to the highest rung whose predicted load stays under the up target
    //    (960x544 with 2x multisampling: 17 ms of 30; 4x would be 25 ms, 83 %) and stays; the frame interval does not
    //    change. Without multisampling rungs (maxRung 6: a vitaGL without FBO_MSAA): 960x544.
    {
        std::printf("observatory (front end 30 ms, back end 11 ms, GPU 8 ms at 640x456):\n");
        auto scene = [](double) { return Load{30, 11, 1, 7}; };
        int top = kToday;
        while (top + 1 < kRungs && Gpu(scene(0), top + 1) / 30.0 <= Base().upTarget) ++top;
        const Result base = Run(60, scene, kToday);
        const Result r = Run(60, scene, -1, 0.10, nullptr, 7, Base(), true);
        std::printf("  final rung %d, %u changes (%u up, %u down), interval %.1f ms (fixed 640x456: %.1f ms)\n", r.finalRung, r.changes, r.ups, r.downs,
                    r.meanIntervalLast10s, base.meanIntervalLast10s);
        char line[120];
        std::snprintf(line, sizeof line, "reaches rung %d (%ux%u, %u samples), the highest the load allows", top, kLadder[top].w, kLadder[top].h, kLadder[top].samples);
        Check(top == 7 && r.finalRung == top, line);
        Check(r.downs == 0, "never steps down");
        Check(r.meanIntervalLast10s <= base.meanIntervalLast10s * 1.03, "frames no slower than at 640x456");
        Config single = Base();
        single.maxRung = kTopSingle;
        const Result s1 = Run(60, scene, -1, 0.10, nullptr, 7, single);
        std::snprintf(line, sizeof line, "without multisampling rungs: final rung %d (960x544)", s1.finalRung);
        Check(s1.finalRung == kTopSingle && s1.downs == 0, line);
    }
    // 1b. A light scene (GPU 4 ms at 640x456): 960x544 with 4x multisampling (11 ms of 30), no slower frames.
    {
        std::printf("light scene (front end 30 ms, back end 11 ms, GPU 4 ms at 640x456):\n");
        auto scene = [](double) { return Load{30, 11, 1, 3}; };
        const Result base = Run(60, scene, kToday);
        const Result r = Run(60, scene, -1, 0.10, nullptr, 9, Base(), true);
        std::printf("  final rung %d, %u changes, interval %.1f ms (fixed 640x456: %.1f ms)\n", r.finalRung, r.changes, r.meanIntervalLast10s, base.meanIntervalLast10s);
        Check(r.finalRung == kRungs - 1 && r.downs == 0, "reaches 960x544 with 4x multisampling and never steps down");
        Check(r.meanIntervalLast10s <= base.meanIntervalLast10s * 1.03, "frames no slower than at 640x456");
    }
    // 2. A castle plaza: the GPU is the limit (31 ms a picture at 640x456), front end 20 ms, back end 14 ms.
    //    Expected: never above 640x456; steps below it because each step buys speed; ends faster than today.
    {
        std::printf("plaza (GPU 31 ms at 640x456, front end 20 ms, back end 14 ms):\n");
        auto scene = [](double) { return Load{20, 14, 3, 28}; };
        const Result base = Run(60, scene, kToday);
        const Result r = Run(60, scene, -1, 0.10, nullptr, 3, Base(), true);
        std::printf("  final rung %d (max %d), %u changes, interval %.1f ms (fixed 640x456: %.1f ms)\n", r.finalRung, r.maxRung, r.changes, r.meanIntervalLast10s,
                    base.meanIntervalLast10s);
        Check(r.maxRung <= kToday, "never above 640x456");
        Check(r.meanIntervalLast10s <= base.meanIntervalLast10s, "ends no slower than 640x456");
        Check(r.finalRung < kToday, "goes below 640x456 since that is faster");
    }
    // 2b. A GPU-limited scene where a smaller picture buys nothing (fixed GPU cost dominates): must come back to today.
    {
        std::printf("GPU-limited, pixel-independent (GPU 31 ms of which 29 fixed):\n");
        auto scene = [](double) { return Load{20, 14, 29, 2}; };
        const Result base = Run(90, scene, kToday);
        const Result r = Run(90, scene, -1, 0.05, nullptr, 5, Base(), true);
        std::printf("  final rung %d (max %d, min %d), %u changes, interval %.1f ms (fixed: %.1f)\n", r.finalRung, r.maxRung, r.minRung, r.changes, r.meanIntervalLast10s,
                    base.meanIntervalLast10s);
        Check(r.maxRung <= kToday, "never above 640x456");
        Check(r.changes <= 6, "at most a few tries in 90 s (the failed step down is blocked, with back-off)");
        Check(r.meanIntervalLast10s <= base.meanIntervalLast10s * 1.03, "no slower than 640x456");
    }
    // 3. The back end's own CPU is the limit (30 ms of submission; GPU 12 ms): the load reads 1.0 but the back end never
    //    waits, so nothing is changed.
    {
        std::printf("back end CPU-limited (submission 30 ms, GPU 12 ms):\n");
        auto scene = [](double) { return Load{15, 30, 2, 10}; };
        const Result r = Run(60, scene, -1, 0.10, nullptr, 11, Base(), true);
        std::printf("  final rung %d, %u changes\n", r.finalRung, r.changes);
        if (g_timers) {
            const Result base = Run(60, scene, kToday);
            std::printf("  interval %.1f ms (fixed 640x456: %.1f ms)\n", r.meanIntervalLast10s, base.meanIntervalLast10s);
            Check(r.meanIntervalLast10s <= base.meanIntervalLast10s * 1.03, "the GPU idles most of the frame: may go up, frames no slower");
        } else {
            Check(r.changes == 0 && r.finalRung == kToday, "stays at 640x456 (the frame fence cannot tell the GPU's work from the back end's)");
        }
    }
    // 4. Observatory, then a heavy GPU stretch (t = 30..60 s: GPU x4, GPU-limited even at 640x456), then light again.
    {
        std::printf("observatory -> heavy (30-60 s) -> observatory:\n");
        auto scene = [](double t) { return (t >= 30 && t < 60) ? Load{30, 11, 4, 28} : Load{30, 11, 1, 7}; };
        const Result r = Run(120, scene, -1, 0.10, nullptr, 13, Base(), true);
        const double share = OverBudgetShare(r, scene, 30, 60);
        std::printf("  max rung %d, %u changes (%u up, %u down), share of the heavy stretch above 640x456: %.0f %%, rung at 59 s %d, final %d\n", r.maxRung, r.changes,
                    r.ups, r.downs, share * 100.0, r.rungAt.size() > 59 ? r.rungAt[59] : -1, r.finalRung);
        Check(share <= 0.10, "above 640x456 for at most 10 % (3 s) of the GPU-limited stretch");
        Check(r.rungAt.size() > 59 && r.rungAt[59] <= kToday, "at or below 640x456 at the end of the heavy stretch");
        Check(r.finalRung >= 5, "back up after it");
        Check(r.changes <= 16, "no oscillation (at most 16 changes in 120 s)");
    }
    // 5. Short GPU spikes (0.3 s of x3 GPU every 4 s, effects) in the Observatory. At 960x544 a spike is GPU-limited (40 ms
    //    against the front end's 30), at 640x456 it is not (24 ms): the controller may step down for them (that is the rule:
    //    nothing above today's size may make frames slower), but it must settle where the spikes fit, not oscillate.
    {
        std::printf("observatory with 0.3 s GPU spikes every 4 s:\n");
        auto scene = [](double t) { return std::fmod(t, 4.0) < 0.3 ? Load{30, 11, 3, 21} : Load{30, 11, 1, 7}; };
        const Result r = Run(180, scene, -1, 0.10, nullptr, 17, Base(), true);
        double slow = 0, all = 0;
        for (const Result::Frame& f : r.frames) {
            const Load l = scene(f.t);
            all += f.ms;
            if (Gpu(l, f.rung) > l.front * 1.02 && f.rung > kToday) slow += f.ms;
        }
        std::printf("  final rung %d, %u changes (%u down) in 180 s; frames slower than at 640x456 because of the rung: %.1f %% of the time\n", r.finalRung,
                    r.changes, r.downs, 100.0 * slow / all);
        Check(r.changes <= 14, "at most 14 changes in 180 s (blocks with back-off)");
        char gap[100];
        std::snprintf(gap, sizeof gap, "changes at least 20 frames apart (fewest: %u)", r.minGapFrames);
        Check(r.minGapFrames >= 20, gap);
        Check(slow / all <= 0.03, "at most 3 % of the time slower than 640x456");
        Check(r.minRung >= kToday, "never below 640x456 (the scene is not GPU-limited there)");
    }
    // 6. Loads: a 2 s stall every 15 s (stage changes) between light and GPU-limited scenes.
    {
        std::printf("alternating scenes with 2 s loads between them:\n");
        auto scene = [](double t) { return (static_cast<int>(t / 15.0) % 2) ? Load{20, 14, 3, 28} : Load{30, 11, 1, 7}; };
        auto gap = [](double t) {
            static int last = -1;
            const int k = static_cast<int>(t / 15.0);
            if (k != last) { last = k; return k > 0 ? 2000.0 : 0.0; }
            return 0.0;
        };
        const Result r = Run(120, scene, -1, 0.10, gap, 19, Base(), true);
        const double share = OverBudgetShare(r, scene, 0, 120);
        std::printf("  %u changes, share of GPU-limited seconds above 640x456: %.0f %%\n  rung by second:", r.changes, share * 100.0);
        for (int x : r.rungAt) std::printf(" %d", x);
        std::printf("\n");
        Check(share <= 0.10, "GPU-limited scenes above 640x456 at most 10 % of their time");
        Check(r.changes <= 40, "bounded number of changes");
    }
    // 7. Under-predicted cost (GPU grows with cost^2): 880x520 overloads although the linear prediction allowed it.
    //    Expected: one overload, then that rung is blocked with back-off: few changes, little time over budget.
    {
        std::printf("superlinear GPU cost (front end 25 ms, GPU 2 + 10 x cost^2):\n");
        auto scene = [](double) { Load l{25, 10, 2, 10}; l.gpow = 2.0; return l; };
        const Result r = Run(300, scene, -1, 0.08, nullptr, 23, Base(), true);
        int slowSecs = 0;
        for (int s = 0; s < static_cast<int>(r.rungAt.size()); ++s) {
            const Load l = scene(s);
            if (r.rungAt[s] >= 0 && Gpu(l, r.rungAt[s]) > l.front * 1.02) ++slowSecs;
        }
        std::printf("  final rung %d (max %d), %u changes in 300 s, %d s with a GPU-limited picture\n", r.finalRung, r.maxRung, r.changes, slowSecs);
        Check(r.changes <= 12, "at most 12 changes in 300 s");
        Check(slowSecs <= 8, "at most 8 s of slower frames in 300 s");
    }
    // 8. Noise only: a scene with the load near the up threshold must not flap.
    {
        std::printf("load near the threshold, 25 %% noise:\n");
        auto scene = [](double) { return Load{30, 10, 1, 16}; }; // 17 ms at 640x456: 0.57 load; 720x480 predicted 0.67
        const Result r = Run(180, scene, -1, 0.25, nullptr, 29, Base(), true);
        std::printf("  final rung %d, %u changes in 180 s\n", r.finalRung, r.changes);
        Check(r.changes <= 8, "at most 8 changes in 180 s");
    }
    // 8b. Not enough video memory above 800x504: the back end vetoes rungs 5 and 6. Expected: settles at 4, and the
    //     vetoed rungs are retried rarely (blocked 30 s, doubling).
    {
        std::printf("observatory, video memory only up to rung 4:\n");
        auto scene = [](double) { return Load{30, 11, 1, 7}; };
        const Result r = Run(300, scene, -1, 0.10, nullptr, 31, Base(), true, [](int rung) { return rung <= 4; });
        std::printf("  final rung %d (max %d), %u changes, %u vetoes in 300 s\n", r.finalRung, r.maxRung, r.changes, r.vetoes);
        Check(r.maxRung <= 4 && r.finalRung == 4, "never uses a vetoed rung, ends at 4");
        Check(r.vetoes <= 6, "at most 6 vetoes in 300 s");
    }
    // 8c. The back end runs short of video memory at a high rung (Drop): back to 640x456 at once, the rungs above stay
    //     blocked for the block time (30 s), and after it the controller may climb again.
    {
        std::printf("video memory short at a high rung (Drop):\n");
        Controller c(Base());
        uint64_t t = 1;
        auto frames = [&](double seconds) {
            for (int i = 0; i < static_cast<int>(seconds * 30); ++i) { // 33 ms frames, GPU busy 6 ms (plenty of headroom)
                FrameTimes f;
                f.startUs = t;
                f.presentUs = t + 20000;
                f.timers = true;
                f.busyKnown = true;
                f.gpuBusyUs = 6000;
                t += 33333;
                c.Add(f);
            }
        };
        frames(20);
        const int high = c.Current();
        c.Drop(kToday, t);
        const bool dropped = c.Current() == kToday && c.LastChange().why == Why::NoMemory;
        int maxBlocked = kToday;
        for (int s = 0; s < 25; ++s) { frames(1); maxBlocked = std::max(maxBlocked, c.Current()); }
        frames(60);
        char line[200];
        std::snprintf(line, sizeof line, "rung %d before, %d after Drop, at most %d in the next 25 s, %d a minute later", high, kToday, maxBlocked, c.Current());
        Check(high > kToday && dropped && maxBlocked == kToday && c.Current() > kToday, line);
    }
    // 8d. The back end cannot make the targets of a rung (no memory; Refused): the rung is the old one again, the refused
    //     one and those above it stay blocked for the block time, and the controller does not ask for them meanwhile.
    {
        std::printf("a step up refused for lack of memory (Refused):\n");
        Controller c(Base());
        uint64_t t = 1;
        int asked = -1, maxAfter = 0;
        bool refusedOnce = false;
        auto frames = [&](double seconds) {
            for (int i = 0; i < static_cast<int>(seconds * 30); ++i) {
                FrameTimes f;
                f.startUs = t;
                f.presentUs = t + 20000;
                f.timers = true;
                f.busyKnown = true;
                f.gpuBusyUs = 6000;
                t += 33333;
                if (c.Add(f) && !refusedOnce && c.Current() > kToday) {
                    asked = c.Current();
                    refusedOnce = true;
                    c.Refused(kToday, asked, t);
                } else if (refusedOnce) {
                    maxAfter = std::max(maxAfter, c.Current());
                }
            }
        };
        frames(20);
        const int during = maxAfter;
        maxAfter = 0;
        frames(60);
        char line[200];
        std::snprintf(line, sizeof line, "asked for rung %d, refused: back to %d, at most %d in the next 20 s, %d a minute later", asked, kToday, during, c.Current());
        Check(asked > kToday && during < asked && c.LastChange().to == c.Current() && c.Current() >= asked, line);
    }
    // 9. Without a GPU clock (gpuDone = 0, the desktop): never moves.
    {
        Controller c;
        bool moved = false;
        for (int i = 0; i < 3000; ++i) {
            FrameTimes f;
            f.startUs = 1 + i * 33000ull;
            f.presentUs = f.startUs + 10000;
            moved |= c.Add(f);
        }
        Check(!moved && c.Current() == kToday, "no GPU clock: stays at 640x456");
    }
    return 0;
}

// Calm mode (the default): changes are rare, steps up wait for quiet moments, scenes keep their rungs.
int CalmScenarios() {
    g_changeStallMs = 150.0; // (the console: 100-270 ms a change)
    g_msaaStallMs = 60.0;
    // A. The console's Observatory as the 23 Oct log showed it: the game's CPU limits the frames (front end 26-34 ms) and
    //    the GPU's work swings with the camera (9-19 ms at 640x456, a 20 s cycle), with effect spikes (a triple jump: 0.5 s
    //    of twice the GPU work every 13 s), 4 s of 72 ms frames just after the load (streaming), and the pause menu for
    //    2 s every 60 s. The legacy rules in the same model for comparison.
    {
        std::printf("console-like observatory (CPU-limited, GPU swinging with the camera, spikes, pauses), 300 s:\n");
        auto scene = [](double t) {
            Load l{30 + 4 * std::sin(t * 0.7), 11, 1, 8 + 5 * std::sin(t * 2 * M_PI / 20.0)};
            if (std::fmod(t, 13.0) < 0.5) l.gpix *= 2.0;
            if (t < 4.0) l.front = 72;
            return l;
        };
        auto quiet = [](double t) { return std::fmod(t, 60.0) >= 50.0 && std::fmod(t, 60.0) < 52.0; };
        Config legacy = Config::Legacy();
        const Result old = Run(300, scene, -1, 0.10, nullptr, 41, legacy, false, nullptr, quiet);
        const Result r = Run(300, scene, -1, 0.10, nullptr, 41, Base(), true, nullptr, quiet);
        std::printf("  legacy: %u changes (%u during play), %.0f ms of frozen picture; calm: %u changes (%u during play, %u at quiet moments), %.0f ms; "
                    "rungs %d..%d, final %d\n", old.changes, old.playChanges, old.stallMs, r.changes, r.playChanges, r.changes - r.playChanges, r.stallMs, r.minRung, r.maxRung, r.finalRung);
        double slow = 0, all = 0;
        for (const Result::Frame& f : r.frames) {
            const Load l = scene(f.t);
            all += f.ms;
            if (Gpu(l, f.rung) > std::max(l.front, l.sub) * 1.05 && f.rung > kToday) slow += f.ms;
        }
        std::printf("  calm: frames made slower by the rung %.1f %% of the time\n", 100.0 * slow / all);
        Check(r.playChanges <= 4, "calm: at most 4 changes during play in 5 minutes");
        std::printf("  calm: %.0f ms of it during play\n", r.playStallMs);
        Check(r.playStallMs <= 600.0, "calm: at most 0.6 s of frozen picture during play in 5 minutes");
        Check(r.playStallMs * 4 <= old.stallMs, "calm: at most a quarter of the legacy rules' frozen picture during play");
        Check(slow / all <= 0.05, "calm: at most 5 % of the time slower because of the rung");
        Check(r.maxRung > kToday, "calm: still climbs above 640x456 where the GPU has room");
    }
    // B. A light scene: with no quiet moment the first step up comes only after upLongHoldSec; with a pause at 10 s it
    //    comes then.
    {
        std::printf("light scene, no quiet moment / a pause at 10 s:\n");
        auto scene = [](double) { return Load{30, 11, 1, 3}; };
        const Result none = Run(40, scene, -1, 0.05, nullptr, 43, Base(), false);
        const Result paused = Run(40, scene, -1, 0.05, nullptr, 43, Base(), true, nullptr, [](double t) { return t >= 10 && t < 11; });
        std::printf("  without: max rung %d; with the pause: rung %d at 12 s, %u changes\n", none.maxRung, paused.rungAt.size() > 12 ? paused.rungAt[12] : -1, paused.changes);
        Check(none.maxRung == kToday, "no step up in 40 s of play without a quiet moment");
        Check(paused.rungAt.size() > 12 && paused.rungAt[12] > kToday && paused.playChanges == 0, "the step up taken in the pause, none in play");
    }
    // C. Scenes with loads between them (keys 1 light, 2 heavy, 1, 2, ...): after the first visit each scene starts at its
    //    own rung in the load, so play sees almost no changes.
    {
        std::printf("two scenes alternating with loads, keyed:\n");
        auto scene = [](double t) { return (static_cast<int>(t / 40.0) % 2) ? Load{20, 14, 3, 28} : Load{30, 11, 1, 6}; };
        auto key = [](double t) { return static_cast<uint32_t>((static_cast<int>(t / 40.0) % 2) ? 2 : 1); };
        auto gap = [](double t) {
            static int last = -1;
            const int k = static_cast<int>(t / 40.0);
            if (k != last) { last = k; return k > 0 ? 2000.0 : 0.0; }
            return 0.0;
        };
        auto quiet = [](double t) { return std::fmod(t, 40.0) >= 30 && std::fmod(t, 40.0) < 31; };
        const Result r = Run(320, scene, -1, 0.08, gap, 47, Base(), true, nullptr, quiet, key);
        std::printf("  %u changes, %u during play; rung by 10 s:", r.changes, r.playChanges);
        for (size_t i = 0; i < r.rungAt.size(); i += 10) std::printf(" %d", r.rungAt[i]);
        std::printf("\n");
        unsigned lateChanges = 0;
        // (changes during play in the second half: the scenes' rungs are known by then)
        const Result r2 = r;
        int prev = -2;
        for (size_t i = 160; i < r2.rungAt.size(); ++i) {
            const int x = r2.rungAt[i];
            if (x >= 0 && prev >= 0 && x != prev && (i % 40) > 3) ++lateChanges;
            if (x >= 0) prev = x;
        }
        std::printf("  rung changes in play (not within 3 s of a load) in the second half: %u\n", lateChanges);
        Check(lateChanges <= 2, "the second visits start at their own rungs: at most 2 changes in play in the second half");
    }
    // E. The second console run: a scene that runs at 60 fps when the camera looks away (GPU 12-15 ms at 640x456, at the
    //    limit) and at 30 fps when it looks at the planet (GPU 25-30 ms), switching every 20-40 s, pause every 60 s.
    {
        std::printf("60/30 fps scene (GPU-limited at 640x456 half the time), pauses every 60 s, 300 s:\n");
        auto scene = [](double t) {
            const bool heavy = std::fmod(t, 57.0) >= 31.0;
            return heavy ? Load{16.7, 9, 3, 24} : Load{16.7, 8, 2, 12 + 2 * std::sin(t)};
        };
        auto quiet = [](double t) { return std::fmod(t, 60.0) >= 50.0 && std::fmod(t, 60.0) < 52.0; };
        const Result old = Run(300, scene, -1, 0.08, nullptr, 59, Config::Legacy(), false, nullptr, quiet);
        const Result r = Run(300, scene, -1, 0.08, nullptr, 59, Base(), true, nullptr, quiet);
        const Result fixed = Run(300, scene, kToday);
        std::printf("  legacy: %u changes (%u in play, %.0f ms frozen); calm: %u changes (%u in play, %.0f ms frozen in play); interval last 10 s %.1f (fixed %.1f)\n",
                    old.changes, old.playChanges, old.stallMs, r.changes, r.playChanges, r.playStallMs, r.meanIntervalLast10s, fixed.meanIntervalLast10s);
        Check(r.playChanges <= 3, "calm: at most 3 changes in play in 5 minutes");
        Check(r.playStallMs * 3 <= old.stallMs + 1, "calm: at most a third of the legacy rules' frozen picture");
    }
    // D. The same scene after a long gap (a shader compile, the console suspended): the rung stays.
    {
        std::printf("a 2 s stall within one scene:\n");
        auto scene = [](double) { return Load{30, 11, 1, 3}; };
        auto gap = [](double t) {
            static bool done = false;
            if (!done && t >= 20) { done = true; return 2000.0; }
            return 0.0;
        };
        const Result r = Run(40, scene, -1, 0.05, gap, 53, Base(), false, nullptr, [](double t) { return t >= 10 && t < 11; }, [](double) { return 7u; });
        Check(r.rungAt.size() > 30 && r.rungAt[30] == r.rungAt[15] && r.rungAt[15] > kToday, "the rung after the stall is the rung before it");
    }
    g_changeStallMs = g_msaaStallMs = 0.0;
    return 0;
}

// Target mode measures throughput work, not deliberate pacing sleeps. Game CPU
// is normalized to two 60 Hz turns even when only every third turn is shown.
int TargetScenarios() {
    Config cfg;
    cfg.targetFps = 30;
    cfg.minRung = -1;
    cfg.budgetMs = 1000.0 / 30.0;
    struct Clock {
        uint64_t now = 1, backend = 1;
        uint32_t game = 1, turns = 1;
        void Feed(Controller& c, int frames, uint32_t turnCpu, double fixedGpuMs, double pixelGpuMs,
                  uint32_t interval = 33333, bool quiet = false) {
            for (int i = 0; i < frames; ++i) {
                const unsigned n = interval >= 45000 ? 3 : 2;
                game += turnCpu * n;
                turns += n;
                backend += 3000;
                FrameTimes f;
                f.startUs = now;
                now += interval;
                f.presentUs = now;
                f.backendRunUs = backend;
                f.gameRunUs = game;
                f.gameTurns = turns;
                f.gameKnown = true;
                f.timers = f.busyKnown = true;
                f.gpuBusyUs = static_cast<uint32_t>((fixedGpuMs + pixelGpuMs * Cost(c.Current())) * 1000.0);
                f.quiet = quiet;
                c.Add(f);
            }
        }
    };
    {
        Config off = cfg;
        off.targetFps = 0;
        Controller c(off);
        Check(c.Cfg().minRung == 0, "target off: a negative floor is clamped to the old rung 0");
    }
    {
        Controller c(cfg);
        Clock clock;
        clock.Feed(c, 390, 22000, 0, 8);
        clock.Feed(c, 1, 22000, 0, 8, 33333, true);
        Check(c.Current() == kToday && c.Changes() == 0,
              "target 30: 44 ms of game CPU prevents quality growth despite an idle GPU");
    }
    {
        Controller c(cfg);
        Clock clock;
        clock.Feed(c, 900, 26000, 0, 46, 66666);
        clock.Feed(c, 1, 26000, 0, 46, 66666, true);
        Check(c.Current() == kToday && c.Changes() == 0,
              "target 30: a CPU bottleneck is not treated by sacrificing resolution");
    }
    {
        Controller c(cfg);
        Clock clock;
        clock.Feed(c, 390, 14000, 0, 7, 50000);
        clock.Feed(c, 1, 14000, 0, 7, 50000, true);
        Check(c.Current() == kToday && c.Changes() == 0 && std::abs(c.WindowWorkMs() - 28.0) < 0.01,
              "target 30: 20 fps pacing still measures two game turns, not three or a 50 ms sleep");
    }
    {
        Controller c(cfg);
        Clock clock;
        clock.game = UINT32_MAX - 1000;
        clock.turns = UINT32_MAX - 3;
        clock.Feed(c, 390, 14000, 0, 7, 50000);
        Check(c.Current() == kToday && std::abs(c.WindowWorkMs() - 28.0) < 0.01,
              "target 30: cumulative game CPU and turn counters remain correct across unsigned wrap");
    }
    {
        Controller c(cfg);
        for (unsigned i = 0; i < 500; ++i) {
            FrameTimes f;
            f.startUs = 1 + i * 50000ull;
            f.presentUs = f.startUs + 40000;
            f.gpuDoneUs = f.presentUs + 1000;
            f.backendRunUs = 1 + i * 3000ull;
            f.gameRunUs = 1 + i * 18000u;
            f.gameTurns = 1 + i * 3u;
            f.gameKnown = true;
            f.paced = true;
            f.quiet = (i % 100) == 99;
            c.Add(f);
        }
        Check(c.Current() == kToday && c.Changes() == 0,
              "target 30: a late frame fence during paced waits is not mistaken for GPU busy time");
    }
    {
        Controller c(cfg);
        Clock clock;
        clock.Feed(c, 390, 4000, 0, 5);
        clock.Feed(c, 1, 4000, 0, 5, 33333, true);
        Check(c.Current() > kToday, "target 30: whole-picture headroom still permits a quiet quality increase");
    }
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        Controller c(cfg);
        Clock clock;
        clock.Feed(c, 390, 4000, 0, 5);
        FrameTimes f;
        f.startUs = clock.now;
        f.presentUs = clock.now + 33333;
        f.backendRunUs = clock.backend + 3000;
        f.gameRunUs = clock.game + 8000;
        f.gameTurns = clock.turns + 2;
        f.gameKnown = true;
        f.timers = f.busyKnown = true;
        f.gpuBusyUs = 5000;
        f.quiet = true;
        if (scenario == 0) f.busyKnown = false;
        else if (scenario == 1) f.gpuBusyUs = 40000;
        else if (scenario == 2) f.gameRunUs += 36000;
        else f.gpuBusyUs = 20000; // today's rung fits, but the remembered larger rung does not
        c.Add(f);
        Check(c.Current() == kToday && c.Changes() == 0,
              "target 30: a quiet unknown/slow current picture cannot consume a stale pending quality increase");
        f.presentUs += 33333;
        f.backendRunUs += 3000;
        f.gameRunUs += 8000;
        f.gameTurns += 2;
        f.busyKnown = true;
        f.gpuBusyUs = 5000;
        c.Add(f);
        Check(c.Current() == kToday && c.Changes() == 0,
              "target 30: one newly good quiet picture cannot resurrect a canceled quality increase");
    }
    {
        Config shortHold = cfg;
        shortHold.windowFrames = 2;
        shortHold.upQuietHoldSec = 0.05;
        Controller c(shortHold);
        Clock clock;
        clock.Feed(c, 1, 4000, 0, 5);
        FrameTimes f;
        f.startUs = clock.now;
        f.presentUs = clock.now + 33333;
        f.backendRunUs = clock.backend + 3000;
        f.gameRunUs = clock.game + 8000;
        f.gameTurns = clock.turns + 2;
        f.gameKnown = true;
        f.timers = true;
        f.gpuBusyUs = 5000;
        c.Add(f); // first window sample has no completed GPU timer
        f.presentUs += 33333;
        f.backendRunUs += 3000;
        f.gameRunUs += 8000;
        f.gameTurns += 2;
        f.busyKnown = true;
        c.Add(f);
        f.presentUs += 33333;
        f.backendRunUs += 3000;
        f.gameRunUs += 8000;
        f.gameTurns += 2;
        f.quiet = true;
        c.Add(f);
        Check(c.Current() == kToday && c.Changes() == 0,
              "target 30: an incomplete GPU window cannot reseed a quality increase at its known last sample");
    }
    {
        Controller c(cfg);
        Clock clock;
        clock.Feed(c, 390, 4000, 0, 5, 50000);
        clock.Feed(c, 1, 4000, 0, 5, 50000, true);
        Check(c.Current() == kToday && c.Changes() == 0,
              "target 30: missed cadence from an unmeasured front end or deliberate 20 Hz pacing cannot raise quality");
        clock.Feed(c, 300, 4000, 0, 5);
        clock.Feed(c, 1, 4000, 0, 5, 33333, true);
        Check(c.Current() > kToday,
              "target 30: after actual cadence recovers to 30 Hz, whole-picture headroom can raise quality quietly");
    }
    {
        Config floor = cfg;
        floor.startRung = floor.maxRung = 0;
        Controller c(floor);
        Clock clock;
        clock.Feed(c, 300, 6000, 0, 30, 50000);
        clock.Feed(c, 1, 6000, 0, 30, 50000, true);
        Check(c.Current() == 0 && c.Changes() == 0,
              "target 30: 544x388 is kept when its GPU work fits, even if presentation is paced at 20");
    }
    {
        Config floor = cfg;
        floor.startRung = floor.maxRung = 0;
        Controller c(floor);
        Clock clock;
        clock.Feed(c, 25, 6000, 0, 59, 50000);
        Check(c.Current() == 0, "target 30: the emergency rung is not taken for a short floor miss");
        clock.Feed(c, 100, 6000, 0, 59, 50000);
        clock.Feed(c, 1, 6000, 0, 59, 50000, true);
        Check(c.Current() == -1 && c.LastChange().from == 0,
              "target 30: a sustained GPU miss at 544x388 unlocks only the 480x340 rung");
        clock.Feed(c, 180, 6000, 0, 59, 50000);
        clock.Feed(c, 1, 6000, 0, 59, 50000, true);
        Check(c.Current() == -1, "target 30: measured GPU-work gain is kept even before 20 fps pacing recovers");
    }
    {
        Config floor = cfg;
        floor.startRung = floor.maxRung = 0;
        Controller c(floor);
        Clock clock;
        clock.Feed(c, 125, 6000, 43, 0, 50000);
        clock.Feed(c, 1, 6000, 43, 0, 50000, true);
        Check(c.Current() == -1, "target 30: the emergency trial also covers unpredicted fixed GPU work");
        clock.Feed(c, 100, 6000, 43, 0, 50000);
        clock.Feed(c, 1, 6000, 43, 0, 50000, true);
        Check(c.Current() == 0 && c.LastChange().why == Why::NoGain,
              "target 30: an emergency trial that saves no GPU work is rolled back in a quiet moment");
    }
    {
        Controller c(cfg);
        Clock clock;
        clock.Feed(c, 1000, 6000, 0, 59, 50000);
        Check(c.Current() == -1 && c.Changes() <= 3,
              "target 30: persistent GPU overload prefers lower sizes to permanent skipped pictures, without oscillation");
        clock.Feed(c, 300, 6000, 0, 5);
        clock.Feed(c, 1, 6000, 0, 5, 33333, true);
        Check(c.Current() >= 0, "target 30: recovery leaves the emergency rung during a quiet moment");
    }
    return 0;
}

int main(int argc, char**) {
    if (argc > 1) {
        const bool expected = std::getenv("GXV_AUTOQ_TARGET") && std::atoi(std::getenv("GXV_AUTOQ_TARGET")) == 30;
        Check(Target30Enabled() == expected, "target switch accepts only 30; default/unsupported values stay off");
        setenv("GXV_AUTOQ_TARGET", expected ? "0" : "30", 1);
        Check(Target30Enabled() == expected, "target switch is read once even when the environment later changes");
        const bool hooks = port_autoq_game_begin && port_autoq_game_work_done && port_autoq_game_cpu_totals && port_autoq_picture_work_us;
        Check(hooks, "game CPU sampling hooks are linked");
        if (hooks) {
            uint32_t run = 0, turns = 0, faults = 0;
            port_autoq_game_begin();
            g_fakeCpuUs += 14000;
            port_autoq_game_work_done();
            const bool known = port_autoq_game_cpu_totals(&run, &turns, &faults) != 0;
            Check(expected ? known && run == 14000 && turns == 1 && g_cpuReads == 2 : !known && g_cpuReads == 0,
                  "game CPU sampling measures only work when enabled and queries no clocks when disabled");
            if (expected) {
                g_fakeCpuUs = 0;
                port_autoq_game_begin();
                g_fakeCpuUs = 20000;
                port_autoq_game_work_done();
                Check(!port_autoq_game_cpu_totals(&run, &turns, &faults) && faults == 1,
                      "a failed CPU-clock start invalidates the sample and records a fault generation");
                g_fakeCpuUs = 30000;
                port_autoq_game_begin();
                g_fakeCpuUs += 12000;
                port_autoq_game_work_done();
                Check(port_autoq_game_cpu_totals(&run, &turns, &faults) && run == 26000 && turns == 2 && faults == 1,
                      "game CPU sampling recovers without hiding the intervening fault generation");
            }
            Config cfg;
            cfg.targetFps = 30;
            cfg.windowFrames = 2;
            cfg.budgetMs = 1000.0 / 30.0;
            Controller c(cfg);
            for (unsigned i = 0; i < 3; ++i) {
                FrameTimes f;
                f.presentUs = 1 + i * 50000ull;
                f.backendRunUs = 1 + i * 3000ull;
                f.gameRunUs = 1 + i * 42000u;
                f.gameTurns = 1 + i * 3u;
                f.gameFaults = faults;
                f.gameKnown = true;
                f.timers = f.busyKnown = true;
                f.gpuBusyUs = 8000;
                c.Add(f);
            }
            Check(port_autoq_picture_work_us() == (expected ? 28000u : 0u),
                  "optional pacing hint is off by default and normalizes three paced game turns to two");
            if (expected) {
                for (unsigned scenario = 0; scenario < 9; ++scenario) {
                    Controller hint(cfg);
                    auto frame = [faults](unsigned i) {
                        FrameTimes f;
                        f.startUs = 1 + i * 50000ull;
                        f.presentUs = f.startUs + 3000;
                        f.backendRunUs = 1 + i * 3000ull;
                        f.gameRunUs = 1 + i * 24000u;
                        f.gameTurns = 1 + i * 3u;
                        f.gameFaults = faults;
                        f.gameKnown = true;
                        f.timers = f.busyKnown = true;
                        f.gpuBusyUs = 8000;
                        return f;
                    };
                    for (unsigned i = 0; i < 3; ++i) hint.Add(frame(i));
                    Check(port_autoq_picture_work_us() == 16000,
                          "hint regression starts with a valid low-work window");
                    FrameTimes bad = frame(3);
                    bad.quiet = scenario < 5;
                    if (scenario == 0 || scenario == 5 || scenario == 8) bad.busyKnown = false;
                    else if (scenario == 1) bad.gpuBusyUs = 40000;
                    else if (scenario == 2 || scenario == 6) bad.gameKnown = false;
                    else if (scenario == 3 || scenario == 7) bad.backendRunUs = 0;
                    else bad.gameRunUs += 60000;
                    hint.Add(bad);
                    const uint64_t current = port_autoq_picture_work_us();
                    Check(current == 0 || current >= 28000,
                          "a current unknown/slow sample cannot retain a recovery hint, including a quiet sample");
                    if (scenario == 0 || scenario >= 5) {
                        FrameTimes next = frame(4);
                        if (scenario == 8) next.busyKnown = false;
                        hint.Add(next);
                        Check(port_autoq_picture_work_us() == 0,
                              "one good partial sample or an unknown window cannot republish old headroom");
                        hint.Add(frame(5));
                        hint.Add(frame(6));
                        Check(port_autoq_picture_work_us() == 16000,
                              "a complete clean window can establish a fresh recovery hint");
                    }
                }
                g_fakeCpuUs = 0;
                port_autoq_game_begin();
                g_fakeCpuUs = 90000;
                port_autoq_game_work_done();
                Check(port_autoq_picture_work_us() == 0,
                      "a failed real game-clock turn invalidates the published hint immediately");
                g_fakeCpuUs = 100000;
                port_autoq_game_begin();
                g_fakeCpuUs += 1000;
                port_autoq_game_work_done();
                Check(port_autoq_picture_work_us() == 0,
                      "a valid game turn after a clock fault cannot resurrect an old hint");
                const uint32_t previousFaults = faults;
                Check(port_autoq_game_cpu_totals(&run, &turns, &faults) && faults == previousFaults + 1,
                      "a valid reader snapshot retains a failure hidden between two renderer observations");
                auto afterFault = [](unsigned i, uint32_t stamp) {
                    FrameTimes f;
                    f.presentUs = 1 + i * 50000ull;
                    f.backendRunUs = 1 + i * 3000ull;
                    f.gameRunUs = 1 + i * 24000u;
                    f.gameTurns = 1 + i * 3u;
                    f.gameFaults = stamp;
                    f.gameKnown = true;
                    f.timers = f.busyKnown = true;
                    f.gpuBusyUs = 8000;
                    return f;
                };
                Controller oldWindow(cfg);
                for (unsigned i = 0; i < 3; ++i) oldWindow.Add(afterFault(i, previousFaults));
                Check(port_autoq_picture_work_us() == 0,
                      "a delayed pre-fault renderer window cannot republish headroom after a real clock fault");
                c.Add(afterFault(3, faults));
                c.Add(afterFault(4, faults));
                Check(port_autoq_picture_work_us() == 0,
                      "a window crossing a hidden game-clock fault cannot establish headroom");
                c.Add(afterFault(5, faults));
                c.Add(afterFault(6, faults));
                Check(port_autoq_picture_work_us() == 16000,
                      "a complete clean post-fault window reestablishes headroom");
            }
        }
        return g_failures ? 1 : 0;
    }
    g_base = Config::Legacy();
    std::printf("==== legacy rules (GXV_AUTOQ_CALM=0), the frame fence clock (any vitaGL)\n");
    g_timers = false;
    Scenarios();
    std::printf("==== legacy rules, the GPU timers clock (vitaGL GPU_TIMERS: exact busy time per frame)\n");
    g_timers = true;
    Scenarios();
    g_base = Config();
    std::printf("==== calm mode (the default), the frame fence clock\n");
    g_timers = false;
    CalmScenarios();
    std::printf("==== calm mode, the GPU timers clock\n");
    g_timers = true;
    CalmScenarios();
    std::printf("==== whole-picture 30 fps target (GXV_AUTOQ_TARGET=30)\n");
    TargetScenarios();
    std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all checks passed", g_failures);
    return g_failures ? 1 : 0;
}
