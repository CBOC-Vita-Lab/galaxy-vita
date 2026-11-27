// neonbox40: host test of platform/include/port/fpbits.h: Fast (integer-bit compares, int floor/ceil) against Ref (the plain float code) on random and
// special inputs (NaN of both signs and payloads, +-inf, +-0, denormals, 2^23 neighbours, int-range edges). Exit 0 = no mismatch.
//   g++ -O2 -std=c++17 -I platform/include tests/neonbox_fpbits.cpp -o /tmp/nb && /tmp/nb
#include "port/fpbits.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include <limits>
using namespace fpb;

// shapecull::Out's plane loop (decomp/src/Game/System/DrawBuffer.cpp): the original text and the neonbox40 text, same inputs, same plane found.
static int PlanesOrig(const float m[3][4], const float c[3], const float h[3], const float v[3], const float pl[5][3]) {
    for (int k = 0; k < 5; ++k) {
        const float a = pl[k][0], b = pl[k][1], cz = pl[k][2];
        float r = 0.0f, mag = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const float t = a * m[0][i] + b * m[1][i] + cz * m[2][i];
            r += (t < 0 ? -t : t) * h[i];
            const float ci = c[i] < 0 ? -c[i] : c[i];
            mag += (t < 0 ? -t : t) * (ci + h[i]);
        }
        const float tr = a * m[0][3] + b * m[1][3] + cz * m[2][3];
        mag += tr < 0 ? -tr : tr;
        const float d = a * v[0] + b * v[1] + cz * v[2];
        if (d + r < -(1.0e-4f * mag + 1.0e-3f)) return k;
    }
    return -1;
}
static int PlanesNew(const float m[3][4], const float c[3], const float h[3], const float v[3], const float pl[5][3]) {
    for (int k = 0; k < 5; ++k) {
        const float a = pl[k][0], b = pl[k][1], cz = pl[k][2];
        float r = 0.0f, mag = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const float t = a * m[0][i] + b * m[1][i] + cz * m[2][i];
            const float at = std::fabs(t);
            r += at * h[i];
            const float ci = std::fabs(c[i]);
            mag += at * (ci + h[i]);
        }
        const float tr = a * m[0][3] + b * m[1][3] + cz * m[2][3];
        mag += std::fabs(tr);
        const float d = a * v[0] + b * v[1] + cz * v[2];
        if (Fast::Lt(d + r, -(1.0e-4f * mag + 1.0e-3f))) return k;
    }
    return -1;
}
int main() {
    std::mt19937_64 rng(12345);
    std::vector<float> sp = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 0.01f, -0.01f, 1e-6f, -1e-6f, 16384.0f, -16384.0f, 8388607.5f, 8388608.0f, -8388608.0f, 8388609.0f,
                             16777216.0f, 2147483520.0f, 1e30f, -1e30f, 3.4028235e38f, -3.4028235e38f,
                             std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::denorm_min(),
                             -std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::min(), -std::numeric_limits<float>::min(),
                             FromBits(0x007FFFFF), FromBits(0x80000001), FromBits(0x7FC00000), FromBits(0xFFC00000), FromBits(0x7F800001), FromBits(0xFF800001),
                             0.99999994f, -0.99999994f, 1.0000001f, 2.5f, -2.5f, 3.5f, 4194303.5f, -4194303.5f};
    auto rnd = [&]() -> float {
        const uint64_t r = rng();
        switch (r & 7) {
        case 0: return sp[(r >> 8) % sp.size()];
        case 1: return FromBits(static_cast<uint32_t>(r >> 16));
        case 2: return static_cast<float>(static_cast<int>(r >> 20) % 40001 - 20000) * 0.25f;
        case 3: return static_cast<float>(static_cast<int64_t>(r >> 12) % 100000 - 50000) * 0.01f;
        case 4: return sp[(r >> 8) % sp.size()] + static_cast<float>(static_cast<int>(r >> 40) % 5 - 2) * 0.5f;
        case 5: return FromBits(static_cast<uint32_t>(r >> 16) & 0x80FFFFFFu); // small exponents / denormals
        case 6: return FromBits(0x4AFFFFF0u + static_cast<uint32_t>(r >> 20) % 64u) * ((r >> 63) ? -1.0f : 1.0f); // around 2^23
        default: return static_cast<float>(static_cast<int>(r >> 20) % 2000001 - 1000000) * 1e-3f;
        }
    };
    long bad = 0, n = 0;
    auto chk = [&](bool ok, const char* what, float a, float b) {
        ++n;
        if (!ok) { if (bad++ < 20) std::printf("MISMATCH %s a=%a b=%a\n", what, a, b); }
    };
    // exhaustive pairs over the specials
    for (float a : sp) for (float b : sp) {
        chk(Fast::Gt(a, b) == Ref::Gt(a, b), "Gt", a, b); chk(Fast::Ge(a, b) == Ref::Ge(a, b), "Ge", a, b);
        chk(Fast::Lt(a, b) == Ref::Lt(a, b), "Lt", a, b); chk(Fast::Le(a, b) == Ref::Le(a, b), "Le", a, b);
        chk(Bits(Fast::Min(a, b)) == Bits(Ref::Min(a, b)), "Min", a, b); chk(Bits(Fast::Max(a, b)) == Bits(Ref::Max(a, b)), "Max", a, b);
    }
    for (long i = 0; i < 300000000L; ++i) {
        const float a = rnd(), b = (i & 3) == 0 ? a : rnd();
        chk(Fast::Gt(a, b) == Ref::Gt(a, b), "Gt", a, b); chk(Fast::Ge(a, b) == Ref::Ge(a, b), "Ge", a, b);
        chk(Fast::Lt(a, b) == Ref::Lt(a, b), "Lt", a, b); chk(Fast::Le(a, b) == Ref::Le(a, b), "Le", a, b);
        chk(Fast::GtZ(a) == Ref::GtZ(a), "GtZ", a, 0);
        chk(Bits(Fast::Min(a, b)) == Bits(Ref::Min(a, b)), "Min", a, b); chk(Bits(Fast::Max(a, b)) == Bits(Ref::Max(a, b)), "Max", a, b);
        // floor / ceil to int, where the original is defined (the result fits an int; NaN is UB in the original: not tested)
        if (a == a && std::fabs(a) < 2.0e9f) {
            chk(Fast::FloorI(a) == Ref::FloorI(a), "FloorI", a, 0); chk(Fast::CeilI(a) == Ref::CeilI(a), "CeilI", a, 0);
        }
    }
    // every float in |x| < 2^24 on a stride, plus every one near integers
    for (uint32_t u = 0; u < 0x4C000000u; u += 7) {
        for (uint32_t s : {0u, 0x80000000u}) {
            const float a = FromBits(u | s);
            chk(Fast::FloorI(a) == Ref::FloorI(a), "FloorI", a, 0); chk(Fast::CeilI(a) == Ref::CeilI(a), "CeilI", a, 0);
        }
    }
    for (int i = -70000; i <= 70000; ++i) for (int k = -3; k <= 3; ++k) {
        float a = static_cast<float>(i) * 0.125f; uint32_t u = Bits(a) + k; a = FromBits(u);
        chk(Fast::FloorI(a) == Ref::FloorI(a), "FloorI", a, 0); chk(Fast::CeilI(a) == Ref::CeilI(a), "CeilI", a, 0);
    }
    for (long i = 0; i < 20000000L; ++i) {
        float m[3][4], c[3], h[3], v[3], pl[5][3];
        for (auto& r : m) for (float& x : r) x = rnd();
        for (int k = 0; k < 3; ++k) { c[k] = rnd(); h[k] = std::fabs(rnd()); v[k] = rnd(); }
        for (auto& r : pl) for (float& x : r) x = rnd();
        if (i & 1) for (auto& r : m) for (float& x : r) if (std::fabs(x) > 1e6f) x = (float)((int)(x * 1e-30f) % 100) * 0.1f;
        chk(PlanesOrig(m, c, h, v, pl) == PlanesNew(m, c, h, v, pl), "planes", m[0][0], v[0]);
    }
    std::printf("neonbox_fpbits: %ld checks, %ld mismatches\n", n, bad);
    return bad ? 1 : 0;
}
