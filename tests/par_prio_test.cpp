// Host test of GXV_PAR_PRIO_ALL's rule (platform/include/port/par_prio.h): a worker's own switch wins, then the umbrella inside the console's user range,
// else 0 (the worker's own default: one step above / below the thread that starts it).
#include "port/par_prio.h"

#include <cstdio>
#include <cstdlib>

using port::par::ResolvePrio; using port::par::ResolvePrioAbove; using port::par::PauseTurns;
static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
int main() {
    Require(ResolvePrio(0, 0) == 0, "nothing set: the worker's default");
    Require(ResolvePrio(0, 0x9C) == 0x9C, "the umbrella applies to a worker with no switch of its own");
    Require(ResolvePrio(0x8A, 0x9C) == 0x8A, "a switch of its own wins");
    Require(ResolvePrio(0, 0x3F) == 0 && ResolvePrio(0, 0xC0) == 0 && ResolvePrio(0, 0xFF) == 0, "an umbrella outside 0x40-0xBF is ignored");
    Require(ResolvePrio(0, 0x40) == 0x40 && ResolvePrio(0, 0xBF) == 0xBF, "the range's ends are valid");
    // parprio31: GXV_PAR_PRIO=1 (above the finishing thread), a switch of its own and the umbrella win, no change without the mode
    Require(ResolvePrioAbove(0, 0, false, 0x9D) == 0 && ResolvePrioAbove(1, 0, false, 0x9D) == 0, "mode off: the worker's default");
    Require(ResolvePrioAbove(1, 0, true, 0x9D) == 0x9D && ResolvePrioAbove(0, 0, true, 0x9D) == 0x9D, "mode on: above the finishing thread");
    Require(ResolvePrioAbove(0x8A, 0x9C, true, 0x9D) == 0x8A && ResolvePrioAbove(1, 0x9C, true, 0x9D) == 0x9C, "own switch, then the umbrella, win over the mode");
    Require(ResolvePrioAbove(0, 0, true, 0) == 0 && ResolvePrioAbove(0, 0, true, 0x30) == 0, "no console value: the default");
    // the pause: 0 = the fixed cap, 1 = proportional (wait / save, 1..cap), 2 = none; a wait at or under the stall limit never pauses
    Require(PauseTurns(0, 3000, 1500, 120, 250) == 120 && PauseTurns(0, 1500, 1500, 120, 250) == 0 && PauseTurns(0, 3000, 1500, 0, 250) == 0, "mode 0 is the fixed pause");
    Require(PauseTurns(1, 3000, 1500, 120, 250) == 12 && PauseTurns(1, 1600, 1500, 120, 250) == 6 && PauseTurns(1, 100000, 1500, 120, 250) == 120, "proportional, capped");
    Require(PauseTurns(1, 1501, 1500, 120, 100000) == 1 && PauseTurns(1, 3000, 1500, 0, 250) == 0 && PauseTurns(1, 3000, 1500, 120, 0) == 120, "proportional: at least 1, cap wins, save 0 safe");
    Require(PauseTurns(2, 99999, 1500, 120, 250) == 0, "mode 2: none");
    std::puts("PASS: par priority rule");
    return 0;
}
