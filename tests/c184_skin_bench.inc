#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using SkinFn = void (*)(J3DJointTree*, Mtx*, Mtx*, const u8*, u8*);
static double run(SkinFn fn, int n, J3DJointTree& t, Mtx* anm, Mtx* out, u8* sf, u8* ef) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        fn(&t, anm, out, sf, ef);
        asm volatile("" : : "g"(out) : "memory");
    }
    return std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now() - start).count() / n;
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream input(argv[1]);
    for (std::string line; std::getline(input, line);) {
        std::istringstream s(line);
        std::string name;
        int e;
        if (!(s >> name >> e) || e <= 0 || e > 65535) return 2;
        std::vector<u8> mix(e), flags(e);
        int size = 0;
        for (auto& m : mix) { unsigned c; if (!(s >> c) || c == 0 || c > 255) return 2; m = c; size += c; }
        std::vector<u16> ix(size);
        std::vector<float> w(size);
        int joints = 0;
        for (int i = 0; i < size; ++i) { unsigned j; if (!(s >> j) || j >= 65535) return 2; ix[i] = j; joints = std::max(joints, int(j) + 1); w[i] = rf(); }
        std::vector<u8> sf(joints,1);
        Mtx *a = new Mtx[joints], *b = new Mtx[joints], *out = new Mtx[e];
        for (int j = 0; j < joints; ++j) for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) { a[j][r][c] = rf(); b[j][r][c] = rf(); }
        J3DJointTree tree{(u16)e, mix.data(), ix.data(), w.data(), b};
        const int n = std::max(100, 350000/size);
        double raw[7], cached[7];
        for (int rep = 0; rep < 7; ++rep) {
            for (int phase = 0; phase < 2; ++phase) {
                const bool enable = ((rep + phase) & 1) != 0;
                (enable ? cached : raw)[rep] = run(enable ? J3DCalcWeightEnvelopeNeon : J3DEnvelopeLoop<false>, n, tree, a, out, sf.data(), flags.data());
            }
        }
        std::sort(raw,raw+7); std::sort(cached,cached+7);
        std::printf("host median ns/pose %-24s env=%3d refs=%4d %9.2f -> %9.2f\n", name.c_str(), e, size, raw[3], cached[3]);
        delete[] a; delete[] b; delete[] out;
    }
    return input.eof() ? 0 : 2;
}
