#include "speed31_model.h"
using namespace m31;
int main(int argc, char** argv) {
    const double pair = std::atof(argv[1]);
    const int quality = std::atoi(argv[2]);
    Scn s; s.quality = quality != 0; s.pair = [pair](double) { return pair; }; s.guard = true; s.seconds = 60; s.leakPm = argc > 3 ? std::atol(argv[3]) : 1;
    if (argc > 4) s.slipMax = static_cast<unsigned>(std::atoi(argv[4]));
    Res r = Run(s);
    for (size_t i = 100; i < 190 && i < r.ks.size(); ++i) std::printf("%u", r.ks[i] > 9 ? 9 : r.ks[i]);
    std::printf("\n");
}
