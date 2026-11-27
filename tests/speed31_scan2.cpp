#include "speed31_model.h"
using namespace m31;
int main(int argc, char** argv) {
    const bool quality = std::atoi(argv[1]) != 0;
    const long leak = std::atol(argv[2]);
    const long budget = std::atol(argv[3]);
    const double jit = std::atof(argv[4]);
    const unsigned slip = argc > 5 ? static_cast<unsigned>(std::atoi(argv[5])) : 20;
    std::printf("q=%d leak=%ld budget=%ld jitter=%.2f slip=%u\n  pair:", quality, leak, budget, jit, slip);
    double w1min = 100, w5min = 100;
    for (double pair : {30.0, 32.0, 33.0, 34.0, 36.0, 38.0, 40.0, 41.3, 45.0, 50.0, 55.0, 60.0}) {
        Scn s; s.quality = quality; s.leakPm = leak; s.budgetMs = budget; s.jitter = jit; s.slipMax = slip;
        s.pair = [pair](double) { return pair; }; s.guard = true; s.seconds = 90;
        Res r = Run(s);
        std::printf("\n  %5.1f speed %5.1f w1s %5.1f w5s %5.1f pic/s %4.1f extras %5.0f dropped %3.0f%% k:", pair, r.speed, r.worst1s, r.worst5s, r.fps, r.extras, r.dropPct);
        for (int i = 0; i < 9; ++i) if (r.turnsPerPic[i] > 0.5) std::printf(" %d:%.0f", i + 2, r.turnsPerPic[i]);
        if (pair >= 34) { w1min = std::min(w1min, r.worst1s); w5min = std::min(w5min, r.worst5s); }
    }
    std::printf("\n  worst of 34..60: 1 s %.1f  5 s %.1f\n", w1min, w5min);
}
