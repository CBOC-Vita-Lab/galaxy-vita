// recover30: model of the Performance governors' give-back (port/fegov.h, GXV_FE_GOV / GXV_GAME_GOV) against today's rule (30 s of calm windows per level, backoff doubling).
// The true level costs are 0.5 / 1 / 2 times the governor's guess (0.8 ms); loads: a step drop, a load exactly at the budget, swings with a 10..60 s period.
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <cmath>
#include "port/fegov.h"
using namespace port::fegov;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
struct R { double tZero = -1; int changes = 0, retakes = 0, hotWindows = 0; double perMin = 0, meanLevel = 0; };
static R Run(bool recover, float scale, std::function<float(double)> load0, double dropAt, double secs) {
    Config c; c.maxLevel = 3; c.recover = recover; c.budgetMs = 15.5f;
    Governor g(c);
    R r; uint32_t rnd = 99; int lastLower = -1000; double lvlSum = 0; int n = 0;
    for (int w = 0; w * 5.0 < secs; ++w) {
        const double t = w * 5.0;
        const int lv = g.level();
        float mean = load0(t);
        for (int l = 1; l <= lv; ++l) mean -= 0.8f * scale * (l == 1 ? 1.0f : 1.0f);
        rnd = rnd * 1664525u + 1013904223u;
        mean += (static_cast<float>(rnd >> 8) / 16777216.0f - 0.5f) * 0.6f;
        Window win; win.pictures = 300; win.frontPics = 300; win.frontSumUs = static_cast<uint64_t>(mean * 1000.0f * 300);
        if (mean > c.budgetMs) ++r.hotWindows;
        const int before = g.level();
        if (g.Feed(win)) {
            ++r.changes;
            if (g.level() < before) lastLower = w; else if (w - lastLower <= 3) ++r.retakes;
        }
        lvlSum += g.level(); ++n;
        if (dropAt >= 0 && t >= dropAt && r.tZero < 0 && g.level() == 0) r.tZero = t - dropAt;
    }
    r.perMin = r.changes / (secs / 60.0); r.meanLevel = lvlSum / n;
    return r;
}
static void Row(const char* name, const R& a, const R& b) {
    std::printf("  %-36s | level 0 after %6.1f -> %6.1f s | changes/min %5.2f -> %5.2f | re-raised within 15 s %d -> %d | hot windows %3d -> %3d | mean level %.2f -> %.2f\n", name, a.tZero, b.tZero,
                a.perMin, b.perMin, a.retakes, b.retakes, a.hotWindows, b.hotWindows, a.meanLevel, b.meanLevel);
}
int main() {
    std::printf("recover30 governor model: today -> recover30\n");
    for (float sc : {0.5f, 1.0f, 2.0f}) {
        std::printf(" true level costs x%.1f of the guess (0.8 ms)\n", static_cast<double>(sc));
        auto drop = [](float after) { return [after](double t) { return t < 120 ? 19.0f : after; }; };
        { const R a = Run(false, sc, drop(12.0f), 120, 600), b = Run(true, sc, drop(12.0f), 120, 600); Row("drop 19 -> 12 ms (big room)", a, b); CHECK(b.tZero >= 0 && b.tZero <= a.tZero); CHECK(b.retakes <= 1); }
        { const R a = Run(false, sc, drop(15.0f), 120, 600), b = Run(true, sc, drop(15.0f), 120, 600); Row("drop 19 -> 15 ms (0.5 ms under budget)", a, b); CHECK(b.retakes <= a.retakes + 2); CHECK(b.perMin <= 3.0); }
        { const R a = Run(false, sc, drop(16.0f), 120, 600), b = Run(true, sc, drop(16.0f), 120, 600); Row("drop 19 -> 16 ms (over budget at 0)", a, b); CHECK(b.perMin <= 3.0); CHECK(b.hotWindows <= a.hotWindows + 6); }
        { auto at = [](double) { return 15.45f; }; const R a = Run(false, sc, at, -1, 600), b = Run(true, sc, at, -1, 600); Row("load exactly at the budget (15.45)", a, b); CHECK(b.perMin <= 2.0); }
        for (int per : {10, 20, 30, 60}) {
            auto sw = [per](double t) { return (static_cast<int>(t / (per * 0.5)) & 1) ? 18.0f : 12.0f; };
            char name[80]; std::snprintf(name, sizeof name, "load swings 12/18 ms, period %d s", per);
            const R a = Run(false, sc, sw, -1, 900), b = Run(true, sc, sw, -1, 900);
            Row(name, a, b);
            CHECK(b.perMin <= std::max(3.0, a.perMin * 1.5 + 1.0));
            CHECK(b.retakes <= a.retakes + 3);
            CHECK(b.hotWindows <= a.hotWindows * 1.5 + 6);
        }
    }
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
