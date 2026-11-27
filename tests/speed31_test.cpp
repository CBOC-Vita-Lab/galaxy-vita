// speed31 acceptance table (host model, tests/speed31_model.h): game speed, pictures a second, hold pattern and the longest run for every pair cost 30..60 ms, Quality and Performance (steady-30 offers),
// leak 2 / 1 / 0.5 % ... (per mille 20/10/5), plus the checks. The closed loop is tests/speed31_loop_test.cpp, the off-hash is tests/speed31_offhash.sh.
#include "speed31_model.h"
using namespace m31;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
static Res One(bool quality, double pair, long leakPm, double jit = 0, unsigned slip = 20, bool guard = true, bool esc = true) {
    Scn s; s.quality = quality; s.leakPm = leakPm; s.budgetMs = 20; s.jitter = jit; s.slipMax = slip; s.guard = guard; s.esc = esc;
    s.steady = true; s.seconds = 90; s.pair = [pair](double) { return pair; };
    return Run(s);
}
int main() {
    const double pairs[] = {30, 32, 33, 34, 36, 38, 40, 41.3, 45, 50, 55, 60};
    for (int q = 1; q >= 0; --q) {
        std::printf("== %s, leak 5 per mille, budget 20 ms, 90 s per pair; holds = share of pictures held 2 / 3 / 4+ vblanks (Quality) or 1 / 2 / 3+ (Performance)\n", q ? "Quality" : "Performance");
        std::printf("  pair  speed  worst1s worst5s  pic/s   hold<=b  b+1   more  longestHold  longestDropRun  guard-off speed/pic\n");
        for (double p : pairs) {
            Res r = One(q, p, 5), off = One(q, p, 5, 0, 20, false);
            std::printf("  %4.1f %6.1f  %6.1f  %6.1f  %5.1f   %5.1f %5.1f %5.1f   %4u        %4u          %5.1f / %5.1f\n", p, r.speed, r.worst1s, r.worst5s, r.fps, r.h2, r.h3, r.h4, r.longestHold, r.longestDropRun, off.speed, off.fps);
            if (p <= 33.0) { CHECK(r.extras == 0); CHECK(r.hash == off.hash); CHECK(r.speed >= 99.9); }
            else { CHECK(r.speed >= 99.0); CHECK(r.worst5s >= 98.0); CHECK(r.worst1s >= 97.0); }
        }
    }
    std::printf("== leak comparison, Quality, worst 1 s / worst 5 s over pairs 34..60 (per mille leak 20 = 2 %%, 10 = 1 %%, 5 = 0.5 %%)\n");
    for (long lk : {20L, 10L, 5L}) {
        double w1 = 100, w5 = 100;
        for (double p : pairs) if (p >= 34) { Res r = One(true, p, lk); w1 = std::min(w1, r.worst1s); w5 = std::min(w5, r.worst5s); }
        std::printf("  leak %.1f %%: worst 1 s %5.1f, worst 5 s %5.1f\n", lk / 10.0, w1, w5);
        if (lk == 5) { CHECK(w5 >= 98.0); CHECK(w1 >= 97.0); }
    }
    std::printf("== jitter +-10 %%, Quality pair 40 / 60: worst 1 s\n");
    for (double p : {40.0, 60.0}) { Res r = One(true, p, 5, 0.10); std::printf("  pair %.0f: speed %.1f worst1s %.1f worst5s %.1f pic/s %.1f\n", p, r.speed, r.worst1s, r.worst5s, r.fps); CHECK(r.worst5s >= 97.0); }
    std::printf("== hitch / stall / load: no extras beyond a few, none for stall and load\n");
    {
        Scn s; s.quality = true; s.guard = true; s.budgetMs = 20; s.leakPm = 5; s.seconds = 60; s.pair = [](double) { return 30.0; };
        s.hitchAtS = 20; s.hitchMs = 70; Res h = Run(s); std::printf("  70 ms hitch: extras %.0f speed %.2f\n", h.extras, h.speed); CHECK(h.extras <= 3);
        s.hitchAtS = -1; s.stallAtS = 20; s.stallMs = 600; Res st = Run(s); std::printf("  600 ms stall: extras %.0f\n", st.extras); CHECK(st.extras == 0);
        s.stallAtS = -1; s.loadAtS = 20; s.loadS = 2; Res ld = Run(s); std::printf("  2 s load: extras %.0f\n", ld.extras); CHECK(ld.extras == 0);
    }
    std::printf("== slip cap with the guard on (Quality pairs 34..60: speed, pic/s) -- the report's decision\n");
    for (unsigned cap : {20u, 8u, 3u, 0u}) {
        double w5 = 100, f = 0; int n = 0;
        for (double p : pairs) if (p >= 34) { Res r = One(true, p, 5, 0, cap); w5 = std::min(w5, r.worst5s); f += r.fps; ++n; }
        std::printf("  slip cap %2u: worst 5 s %5.1f, mean pic/s %5.1f\n", cap, w5, f / n);
    }
    std::printf(g_fail ? "speed31_test: FAILED (%d)\n" : "speed31_test: ALL PASSED\n", g_fail);
    return g_fail != 0;
}
