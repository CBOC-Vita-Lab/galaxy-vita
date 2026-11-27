// Host test of vgx_dispwait.h (the display scene's estimated wait for its buffer, for the "gpu-scenes:" line). tools/test_vgx_dispwait.sh builds and runs it.
#include "vgx_dispwait.h"

#include <cstdint>
#include <cstdio>

namespace {
int g_failed = 0, g_checks = 0;
void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failed; std::printf("FAILED: %s\n", what); }
}
} // namespace

int main() {
    using vgx::dispwait::Estimator;
    {   // GPU-limited: the display scene costs 3.0-3.2 ms every picture and never waits
        Estimator e;
        uint64_t sum = 0;
        for (int i = 0; i < 300; ++i) sum += e.Add(3000u + static_cast<uint32_t>(i % 3) * 100u);
        Check(sum == 0, "a display scene that always takes its own 3.0-3.2 ms: no wait");
    }
    {   // near 60 fps, producer ahead: every other picture's display scene waits ~6 ms for the buffer
        Estimator e;
        uint64_t sum = 0, waits = 0;
        unsigned n = 0;
        for (int i = 0; i < 300; ++i) {
            const uint32_t span = 3000u + (i % 2 ? 6000u : 0u);
            const uint32_t w = e.Add(span);
            if (i >= 16) { sum += w; ++n; waits += w > 0; }
        }
        Check(waits == 142 && n == 284, "the pictures that wait are the ones found (every odd one of pictures 16 .. 299)");
        Check(sum / waits == 5325u, "a 6 ms wait on a 3 ms display scene: 5.325 ms found (6.0 less the 0.375 + 0.3 ms kept as the display's own: a lower bound)");
    }
    {   // the first pictures are not judged
        Estimator e;
        for (int i = 0; i < 7; ++i) Check(e.Add(i % 2 ? 12000u : 3000u) == 0, "fewer than 8 pictures: nothing is judged");
        Check(e.Add(12000u) > 0, "the eighth picture is");
    }
    {   // every picture waits: the minimum has a wait in it, the ceiling (8 ms) still takes the excess
        Estimator e;
        uint32_t last = 0;
        for (int i = 0; i < 100; ++i) last = e.Add(10000u);
        Check(last == 2000u, "10 ms every time: the lower bound is span - ceiling = 2 ms");
    }
    {   // a size change: the minimum is taken afresh
        Estimator e;
        for (int i = 0; i < 50; ++i) e.Add(2000u);
        Check(e.Add(6000u) > 3000u, "after a stable 2 ms, a 6 ms picture is mostly wait");
        e.Reset();
        Check(e.Pictures() == 0, "Reset forgets the pictures");
        for (int i = 0; i < 20; ++i) e.Add(6000u);
        Check(e.Add(6000u) == 0, "a display scene that now costs 6 ms (a bigger size): no wait");
    }
    {   // never more than the span, never from a span of 0
        Estimator e;
        bool ok = true;
        for (uint32_t s = 0; s < 40000u; s += 137u) { const uint32_t w = e.Add(s); if (w > s) ok = false; }
        Check(ok, "the wait never exceeds the span");
        Check(Estimator().Add(0) == 0, "no display scene in the picture: 0");
    }
    std::printf(g_failed ? "%d of %d checks FAILED\n" : "all %d checks passed\n", g_failed ? g_failed : g_checks, g_checks);
    return g_failed ? 1 : 0;
}
