// GXV_FIF (port/fif.h): a pipeline model. The game thread produces a picture every turn pair (8 ms of work), the back end and GPU take 32.5 ms a picture, the display shows one every 33.4 ms
// (2 vblanks). Without the gate the unbounded queue fills (a bounded one of `queue` pictures, as GXV_INFLIGHT+1) and turn to picture is long; with it, short, at the same picture rate.
// Checks: latency drops by >= 40 %, throughput within 1 %, the gate switches itself off when the display cannot keep up and tries again, a heavy moment stands it aside.
#include "port/fif.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <deque>
using namespace port::fif;
static void Require(bool ok, const char* w) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", w); std::exit(1); } }
struct R { double fps, latMs; unsigned strikes, waits; };
// GPU stage serial; display slot every 33.37 ms; picture shown at the first slot after GPU done. Game thread: starts a turn pair when there is room (queue < cap) and the gate allows.
static R Run(unsigned depth, unsigned queueCap, double gpuMs, double seconds) {
    Gate g; if (depth) g.Configure(depth);
    const double slot = 33.366 - 1e-3, work = 8.0;
    double t = 0, gpuFree = 0, lastShow = -1e9;
    std::deque<double> shownAt;   // flip time of each picture handed over (computed at hand-over: serial pipeline)
    std::deque<double> turnAt;
    unsigned sent = 0, n = 0; double latSum = 0; unsigned waits = 0;
    while (t < seconds * 1000) {
        // room: pictures not yet flipped may not exceed queueCap (back-pressure of the queues)
        while (!shownAt.empty() && shownAt.front() <= t) { shownAt.pop_front(); turnAt.pop_front(); }
        if (shownAt.size() >= queueCap) { t = shownAt.front(); continue; }
        const double start = t;
        t += work; ++sent;
        const double done = std::max(t, gpuFree) + gpuMs; gpuFree = done;
        double show = std::max(done, lastShow + slot); show = std::ceil((show - 1e-6) / 16.683) * 16.683; if (show < lastShow + slot - 1e-6) show = lastShow + slot; lastShow = show;
        shownAt.push_back(show); turnAt.push_back(start);
        if (start > 2000) { latSum += show - start; ++n; }
        uint64_t shown = sent - shownAt.size();
        if (depth && g.Want(sent, shown, static_cast<int64_t>(t * 1000))) {
            ++waits;
            const double t0 = t, limit = t + Gate::kMaxWaitUs / 1000.0;
            while (shownAt.size() > depth && shownAt.front() <= limit) { t = shownAt.front(); shownAt.pop_front(); turnAt.pop_front(); }
            if (shownAt.size() > depth) t = limit;
            g.Waited(shownAt.size() > depth, static_cast<int64_t>((t - t0) * 1000));
        }
    }
    { const auto& s = g.GetStats(); std::printf("  depth %u: probes %u waits %u stillOver %u aside %u strikes %u overMax %u\n", depth, s.probes, s.waits, s.stillOver, s.aside, s.strikes, s.overMax); }
    return {n / (seconds - 2.0), latSum / n, g.GetStats().strikes, waits};
}
int main() {
    R off = Run(0, 5, 32.5, 60), on3 = Run(3, 5, 32.5, 60), on2 = Run(2, 5, 32.5, 60);
    std::printf("no gate: %.2f fps, %.0f ms turn to picture\ndepth 3: %.2f fps, %.0f ms (%u waits)\ndepth 2: %.2f fps, %.0f ms (%u waits)\n", off.fps, off.latMs, on3.fps, on3.latMs, on3.waits, on2.fps, on2.latMs, on2.waits);
    Require(on3.latMs < off.latMs * 0.85, "depth 3 cuts the latency by 15 % or more");
    Require(on2.latMs < off.latMs * 0.65 && on2.fps > off.fps * 0.99, "depth 2 cuts it by 35 % or more at the same rate");
    Require(on3.fps > off.fps * 0.99, "depth 3 keeps the picture rate");
    Require(on3.strikes == 0, "no strike on a pipeline that can keep up");
    // a display that cannot keep up (GPU 40 ms): the gate keeps hitting the limit and switches itself off, then retries
    Gate g; g.Configure(2);
    uint64_t sent = 100, shown = 10; int64_t t = 0; unsigned waited = 0;
    for (int i = 0; i < 200; ++i) { t += 40000; ++sent; if (g.Want(sent, shown, t)) { ++waited; g.Waited(true); } }
    Require(g.GetStats().strikes >= 1 && g.Off() && waited == 30, "30 hopeless waits switch it off");
    for (int i = 0; i < 900; ++i) { t += 33000; ++sent; g.Want(sent, shown, t); }
    Require(g.GetStats().retries >= 1, "it tries again after 900 probes");
    // a late turn stands it aside for 150 probes
    Gate h; h.Configure(2); t = 0;
    for (int i = 0; i < 5; ++i) { t += 33000; h.Want(50, 10, t); h.Waited(false); }
    t += 200000; Require(!h.Want(50, 10, t), "a heavy moment: no wait");
    for (int i = 0; i < 149; ++i) { t += 33000; Require(!h.Want(50, 10, t), "stands aside"); }
    t += 33000; Require(h.Want(50, 10, t), "waits again after 150");
    Gate z; Require(!z.Enabled() && !z.Want(9, 1, 0), "default off");
    std::puts("PASS fif_test");
}
