#include "speed31_model.h"
using namespace m31;
int main() {
    for (int guard = 0; guard < 2; ++guard)
    for (double lane : {15.0, 17.0, 18.0, 20.0, 22.0, 25.0}) {
        Scn s; s.quality = false; s.steady = false; s.N = 9.0; s.pair = [lane](double) { return lane + 9.0; };
        s.guard = guard; s.seconds = 60; s.leakPm = 5; s.budgetMs = 20; s.slipMax = 20;
        Res r = Run(s);
        std::printf("guard %d lane %4.1f: speed %5.1f w1s %5.1f pic/s %4.1f extras %5.0f  k:", guard, lane, r.speed, r.worst1s, r.fps, r.extras);
        for (int i = 0; i < 9; ++i) if (r.turnsPerPic[i] > 0.5) std::printf(" %d:%.0f", i + 1, r.turnsPerPic[i]);
        std::printf("  holds %.0f/%.0f/%.0f longest %u\n", r.h2, r.h3, r.h4, r.longestHold);
    }
}
