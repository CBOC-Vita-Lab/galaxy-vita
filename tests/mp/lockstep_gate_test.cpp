// Host test of port/lockstep_gate.h (checks83): wall-clock decisions about how much logic runs are off in a lockstep session, the old behaviour bit for bit without one.
// Real-time pacing (undrawn turns, late catch-up, the speed guard's repayment) is deliberately NOT gated: see the header.
#include "port/lockstep_gate.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(c) do { ++sChecks; if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
static int sChecks = 0;
using namespace port::lockstep;

int main() {
    // far logic level
    for (int lv = 0; lv <= 3; ++lv) {
        CHECK(FarLogicLevel(lv, false, false) == lv);
        CHECK(FarLogicLevel(lv, true, false) == 0);
        CHECK(FarLogicLevel(lv, false, true) == 0);
        CHECK(FarLogicLevel(lv, true, true) == 0);
    }
    CHECK(QualityLogicLimit(6000.0f, false) == 6000.0f);
    CHECK(QualityLogicLimit(6000.0f, true) == 0.0f);

    std::printf("lockstep_gate_test: %d checks passed\n", sChecks);
    return 0;
}
