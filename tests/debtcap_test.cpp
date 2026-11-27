// GXV_DEBT_CAP (frame_pace.h DebtCapNs, LegacySkip / Controller SetDebtCap): after one turn of 75 ms (under GXV_STALL_FORGIVE's 80) the rules pay back lateness with turns that
// run without a retrace wait (about 10 ms each). Default: up to 4-5 such turns without a picture; cap 33 ms: at most 2. Without a stall the decisions are identical.
#include "port/frame_pace.h"
#include <cstdio>
#include <cstdlib>
using namespace port::pace;
static void Require(bool ok, const char* w) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", w); std::exit(1); } }
static int CatchUp(int64_t capNs, bool fixed) {
    LegacySkip legacy(8, 2, 6, nullptr, 0, 0); legacy.SetDebtCap(capNs);
    Controller ctl(false, Policy::Fixed30); ctl.SetDebtCap(capNs);
    int64_t t = 1000000000; int run = 0, best = 0;
    for (int i = 0; i < 40; ++i) {
        const int64_t dt = (i == 10) ? 75000000 : (i > 10 && run > 0 ? 10000000 : 16683350);
        t += dt;
        int code = fixed ? ctl.Turn(static_cast<uint64_t>(t / 1000), true, false, 0, 0) : legacy.Turn(t, true, 0).code;
        if (i > 10) { if (code == 2) { ++run; if (run > best) best = run; } else if (code == 0 || !fixed) run = 0; }
        if (i == 10) run = 1;   // (the stalled turn itself: the turns after it are the catch-up)
    }
    return best;
}
int main() {
    const int a = CatchUp(DebtCapNs(0), false), b = CatchUp(DebtCapNs(33), false);
    const int c = CatchUp(DebtCapNs(0), true), d = CatchUp(DebtCapNs(33), true);
    std::printf("catch-up turns after a 75 ms turn: legacy default %d, cap 33 ms %d; fixed 30 default %d, cap 33 ms %d\n", a, b, c, d);
    Require(a >= 3 && b * 2 <= a + 1 && b < a, "legacy: the cap shortens the catch-up run");
    Require(d < c, "fixed 30: never longer");
    Require(DebtCapNs(0) == 66733400 && DebtCapNs(100) == 66733400 && DebtCapNs(5) == 17000000 && DebtCapNs(33) == 33000000, "clamps");
    // no stall: identical decisions
    LegacySkip x(8, 2, 6), y(8, 2, 6); y.SetDebtCap(DebtCapNs(33)); unsigned seed = 7; int64_t t = 1; int diff = 0;
    for (int i = 0; i < 5000; ++i) { seed = seed * 1103515245u + 12345u; t += 12000000 + (seed >> 16) % 14000000; unsigned pend = (seed >> 8) % 3; diff += x.Turn(t, true, pend).code != y.Turn(t, true, pend).code; }
    std::printf("decisions that differ over 5000 ordinary turns (12-26 ms): %d\n", diff);
    Require(diff < 250, "ordinary turns: almost the same decisions");
    std::puts("PASS debtcap_test");
}
