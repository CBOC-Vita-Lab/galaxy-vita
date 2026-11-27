// geom26: host test of platform/vgx/src/vgx_hsrauto.h (GXV_HSR_AUTO: the per-stage decision of the draw-order planner).
//   tools/test_vgx_hsrauto.sh
#include <cstdint>
#include <cstdio>
#include "../platform/vgx/src/vgx_hsrauto.h"

using namespace vgx::hsrauto;
static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    Cfg c;
    c.probePics = 10; c.onWindow = 20; c.recheck = 50;
    // 1. a stage with plenty of alpha-tested draws before opaque ones: probe, then ON
    {
        Auto a;
        CHECK(a.phase == kProbe && a.Planning() && !a.Applying());
        bool ch = false;
        for (int i = 0; i < 9; ++i) ch = ch || a.Feed(100, 11, 7, 80, c);
        CHECK(!ch && a.phase == kProbe);
        CHECK(a.Feed(100, 11, 7, 80, c));
        CHECK(a.phase == kOn && a.Applying() && a.why == kWhyEnough && a.lastT == 11.0 && a.lastUs == 80.0);
        // stays ON while it moves enough and is cheap
        bool any = false;
        for (int i = 0; i < 100; ++i) any = any || a.Feed(100, 11, 7, 80, c);
        CHECK(!any && a.phase == kOn);
    }
    // 2. a stage with none: OFF at once, the planner is not asked again, and it probes again after the recheck
    {
        Auto a;
        for (int i = 0; i < 10; ++i) a.Feed(200, 0, 0, 50, c);
        CHECK(a.phase == kOff && a.why == kWhyFewT && !a.Planning());
        CHECK(!a.Feed(200, 50, 50, 50, c));            // fed while OFF: ignored
        CHECK(a.phase == kOff);
        bool re = false; int k = 0;
        while (!re && k < 1000) { re = a.Skip(c); ++k; }
        CHECK(re && k == 50 && a.phase == kProbe && a.why == kWhyRecheck && a.Planning());
    }
    // 3. menu / fade pictures (few draws) do not count toward the probe
    {
        Auto a;
        for (int i = 0; i < 1000; ++i) a.Feed(3, 40, 40, 10, c);
        CHECK(a.phase == kProbe && a.n == 0);
    }
    // 4. ON ends when it stops moving anything, or when the planner is too dear
    {
        Auto a;
        for (int i = 0; i < 10; ++i) a.Feed(100, 5, 5, 100, c);
        CHECK(a.phase == kOn);
        bool ch = false;
        for (int i = 0; i < 20; ++i) ch = a.Feed(100, 5, 0, 100, c);
        CHECK(ch && a.phase == kOff && a.why == kWhyNoMove);
        Auto b;
        for (int i = 0; i < 10; ++i) b.Feed(100, 5, 5, 100, c);
        for (int i = 0; i < 20; ++i) ch = b.Feed(100, 5, 5, 900, c);
        CHECK(ch && b.phase == kOff && b.why == kWhyCost);
    }
    // 5. the threshold: exactly minTBefore is ON, just under is OFF (means of 10 pictures)
    {
        Auto a, b;
        for (int i = 0; i < 10; ++i) { a.Feed(100, 1, 1, 10, c); b.Feed(100, i == 0 ? 0 : 1, 1, 10, c); }  // a: mean 1.0; b: mean 0.9
        CHECK(a.phase == kOn && b.phase == kOff);
    }
    // 6. observeOnly: never leaves PROBE, reports a verdict per window
    {
        Cfg o = c; o.observeOnly = true;
        Auto a;
        int v = 0;
        for (int i = 0; i < 30; ++i) if (a.Feed(100, 9, 9, 10, o)) ++v;
        CHECK(v == 3 && a.phase == kProbe && a.verdictOn && !a.Applying());
        for (int i = 0; i < 10; ++i) a.Feed(100, 0, 0, 10, o);
        CHECK(!a.verdictOn && a.phase == kProbe);
    }
    // 7. Reset
    {
        Auto a;
        for (int i = 0; i < 10; ++i) a.Feed(100, 5, 5, 100, c);
        a.Reset();
        CHECK(a.phase == kProbe && a.n == 0 && a.changes == 0);
    }
    // 8. a long mixed run: the machine never applies while OFF and never leaves its three phases
    {
        Auto a;
        uint32_t seed = 7;
        uint64_t appliedWhileOff = 0;
        for (int i = 0; i < 20000; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const uint32_t t = (seed >> 24) % 6, d = 10 + ((seed >> 16) & 0x3F);
            if (a.Planning()) a.Feed(d, t, t > 2 ? t - 1 : 0, 50 + (seed & 0xFF), c); else a.Skip(c);
            if (!a.Planning() && a.Applying()) ++appliedWhileOff;
            CHECK(a.phase == kProbe || a.phase == kOn || a.phase == kOff);
        }
        CHECK(appliedWhileOff == 0);
    }
    std::printf("vgx_hsrauto_test: %d checks, %d failures\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
