// GXV_PAR_PRIO_ALL (sched24): the priority a par worker takes when its own GXV_PAR_*_PRIO is left at 0 (platform/src/port/perf.cpp ParPrio explains why).
// Pure, so the rule is tested on a PC (tests/par_prio_test.cpp).
#pragma once

namespace port::par {

// own: the worker's own switch (0 = unset: its default, one step above or below the starting thread), all: GXV_PAR_PRIO_ALL (0 = unset).
// 0 = the worker's own default. A value outside the console's user range 0x40-0xBF in GXV_PAR_PRIO_ALL is ignored (an own switch is passed on as it is:
// the thread creation refuses what it cannot take).
constexpr int ResolvePrio(int own, int all) { return own > 0 ? own : (all >= 0x40 && all <= 0xBF ? all : 0); }


// GXV_PAR_PRIO=1 (parprio31, default 0 = as before): a core-3 worker whose own switch and GXV_PAR_PRIO_ALL are unset takes `above` (two steps above the game
// thread's priority: above the finishing thread, which sits one step above the game thread on the same core) instead of its default (one step above the
// game thread: the finishing thread's priority). own: the worker's own switch (a value under 0x40 is the mode switch GXV_PAR_PRIO=1, not a priority).
// above: 0 = not available (not the console, or the fourth core is not the worker's core).
constexpr int ResolvePrioAbove(int own, int all, bool aboveOn, int above) {
    return own >= 0x40 ? own : (all >= 0x40 && all <= 0xBF ? all : (aboveOn && above >= 0x40 && above <= 0xBF ? above : 0));
}

// GXV_PAR_PAUSE (parprio31, default 0): what the particle worker does after a join that waited over the stall limit.
//   0: pause `cap` turns (GXV_PAR_PTCL_PAUSE, 120: as before)   1: proportional: the wait divided by what one parallel turn saves (saveUs), at least 1, at most `cap`
//   2: none (the stall is counted, the worker keeps running)
// Returns the turns to run serial. waitUs <= stallUs: 0 in every mode.
constexpr unsigned PauseTurns(int mode, unsigned waitUs, unsigned stallUs, unsigned cap, unsigned saveUs) {
    return waitUs <= stallUs || mode == 2 ? 0u
         : mode == 1 ? ((waitUs / (saveUs ? saveUs : 1u) < 1u ? 1u : waitUs / (saveUs ? saveUs : 1u)) > cap ? cap : (waitUs / (saveUs ? saveUs : 1u) < 1u ? 1u : waitUs / (saveUs ? saveUs : 1u)))
         : cap;
}

}  // namespace port::par
