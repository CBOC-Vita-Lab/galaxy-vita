// scratch scan: parameter exploration (not part of the deliverable tests)
#include "speed31_model.h"
using namespace m31;
int main(int argc, char** argv) {
    const bool quality = argc > 1 ? std::atoi(argv[1]) != 0 : true;
    const long leak = argc > 2 ? std::atol(argv[2]) : 1;
    const unsigned slip = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 20;
    const int esc = argc > 4 ? std::atoi(argv[4]) : 1;
    const int own = argc > 5 ? std::atoi(argv[5]) : 1;
    std::printf("quality=%d leak=%ld slip=%u esc=%d own=%d\n", quality, leak, slip, esc, own);
    for (double pair : {32.0, 34.0, 36.0, 38.0, 40.0, 41.3, 45.0, 50.0, 55.0, 60.0}) {
        Scn s; s.quality = quality; s.leakPm = leak; s.slipMax = slip; s.esc = esc; s.own = own;
        s.pair = [pair](double) { return pair; };
        s.guard = true; s.seconds = 60;
        Res r = Run(s);
        std::printf("pair %5.1f: speed %5.1f worst1s %5.1f worst5s %5.1f pic/s %5.1f drop %5.1f%% run %2u  k:", pair, r.speed, r.worst1s, r.worst5s, r.fps, r.dropPct, r.longestDropRun);
        for (int i = 0; i < 9; ++i) if (r.turnsPerPic[i] > 0.5) std::printf(" %d:%.0f", i + 2, r.turnsPerPic[i]);
        std::printf("  holds %4.0f/%4.0f/%4.0f longest %u chg/min %u bucketMax %d\n", r.h2, r.h3, r.h4, r.longestHold, r.changesPerMin, r.maxBucketMs);
    }
}
