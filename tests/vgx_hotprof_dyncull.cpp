// hotprof25 (11 Nov): host test and micro-benchmark of GXV_HOTPROF_DYNCULL's rigid, perspective test (platform/vgx/src/vgx_hotprof_dyncull.inc,
// the very file vgx_draw.cpp includes) against the loop over PointOutMask it replaces (copied below from vgx_draw.cpp as of base a558298,
// GXV_FE24_CLIP=0 form). Every draw must get the same answer: random draws, draws built to sit on the planes, on the margins and on the margin's
// bound, and draws with NaN / infinite / denormal / huge numbers. `-DHP_NEG=1`: a negative control (the bound made too small): must FAIL.
//   tools/test_vgx_hotprof_dyncull.sh
#include "../platform/include/port/fpbits.h" // (neonbox40: the .inc uses fpb::Active)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace {
struct ClipNow {
    float p[6];
    bool ortho, sides;
    float kx, ky, pSum;
};
// ---- the reference: vgx_draw.cpp PointOutMask (GXV_FE24_CLIP=0) and DynDrawOutside's loop, rigid case
inline uint32_t PointOutMask(const ClipNow& c, const float* const m[3], float x, float y, float z) {
    const float* const p = c.p;
    const float ex = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
    const float ey = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
    const float ez = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
    float cx, cy, cw, a;
    if (c.ortho) {
        cx = p[0] * ex + p[1]; cy = p[2] * ey + p[3]; cw = 1.0f;
        a = std::fabs(p[0] * ex) + std::fabs(p[1]) + std::fabs(p[2] * ey) + std::fabs(p[3]) + 1.0f;
    } else {
        cx = p[0] * ex + p[1] * ez; cy = p[2] * ey + p[3] * ez; cw = -ez;
        a = std::fabs(p[0] * ex) + std::fabs(p[1] * ez) + std::fabs(p[2] * ey) + std::fabs(p[3] * ez) + std::fabs(ez);
    }
    const float tx = cx - c.kx * cw, nx = -cx - c.kx * cw, ty = cy - c.ky * cw, ny = -cy - c.ky * cw;
    const float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
    float erow = 0.0f;
    for (int r = 0; r < 3; ++r)
        erow = std::max(erow, std::fabs(m[r][0]) * ax + std::fabs(m[r][1]) * ay + std::fabs(m[r][2]) * az + std::fabs(m[r][3]));
    const float margin = 2.0f * ((1e-4f * erow + 1e-6f) * c.pSum + 1e-4f * a) + 1e-6f;
    if (!(margin < 1e30f)) return 0;
    uint32_t o = 0;
    if (c.sides) {
        if (tx > margin) o |= 1u;
        if (nx > margin) o |= 2u;
        if (ty > margin) o |= 4u;
        if (ny > margin) o |= 8u;
    }
    if (!c.ortho && -cw > margin) o |= 16u;
    return o;
}
__attribute__((noinline)) bool RefOutside(const ClipNow& c, const float* const m[3], const uint8_t* v, uint32_t count, uint32_t stride) {
    uint32_t all = 31u;
    for (uint32_t i = 0; i < count && all; ++i, v += stride) {
        float q[4];
        std::memcpy(q, v, sizeof q);
        all &= PointOutMask(c, m, q[0], q[1], q[2]);
    }
    return all != 0;
}
#if defined(HP_NEG)
#define HP_NEG_BOUND 1 // (the include's bound is replaced below: see NegOutside)
#endif
#include "../platform/vgx/src/vgx_hotprof_dyncull.inc"
__attribute__((noinline)) bool NewOutside(const ClipNow& c, const float* const m[3], const uint8_t* v, uint32_t count, uint32_t stride) {
#if defined(HP_NEG)
    // (negative control: a "bound" below the margin: a plane between them is taken as beyond; the test must notice)
    uint32_t all = 31u;
    for (uint32_t i = 0; i < count; ++i, v += stride) {
        float q[3];
        std::memcpy(q, v, sizeof q);
        const float* const p = c.p;
        const float ex = m[0][0] * q[0] + m[0][1] * q[1] + m[0][2] * q[2] + m[0][3];
        const float ey = m[1][0] * q[0] + m[1][1] * q[1] + m[1][2] * q[2] + m[1][3];
        const float ez = m[2][0] * q[0] + m[2][1] * q[1] + m[2][2] * q[2] + m[2][3];
        const float cx = p[0] * ex + p[1] * ez, cy = p[2] * ey + p[3] * ez, cw = -ez;
        const float tx = cx - c.kx * cw, nx = -cx - c.kx * cw, ty = cy - c.ky * cw, ny = -cy - c.ky * cw;
        const uint32_t m0 = (c.sides ? ((tx > 0.0f ? 1u : 0u) | (nx > 0.0f ? 2u : 0u) | (ty > 0.0f ? 4u : 0u) | (ny > 0.0f ? 8u : 0u)) : 0u) | (-cw > 0.0f ? 16u : 0u);
        all &= m0;
        if (!all) return false;
    }
    return all != 0;
#else
    return HpRigidOutside(c, m, v, count, stride);
#endif
}

uint64_t g_seed = 0x9E3779B97F4A7C15ull;
inline uint32_t Rnd() {
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17;
    return static_cast<uint32_t>(g_seed >> 32);
}
inline float U() { return static_cast<float>(Rnd() >> 8) * (1.0f / 16777216.0f); } // [0, 1)
inline float S() { return U() * 2.0f - 1.0f; }
float Odd() { // a number of every kind
    switch (Rnd() % 12) {
    case 0: return std::numeric_limits<float>::quiet_NaN();
    case 1: return std::numeric_limits<float>::infinity();
    case 2: return -std::numeric_limits<float>::infinity();
    case 3: return 1e-42f; // denormal
    case 4: return -1e-40f;
    case 5: return 3e38f;
    case 6: return -1e30f;
    case 7: return 0.0f;
    case 8: return -0.0f;
    case 9: return 1e20f * S();
    default: return 1e-20f * S();
    }
}
void MakeClip(ClipNow& c) {
    // (GX perspective: p0 = cot / aspect, p1 = 0 (or an off-centre term), p2 = cot, p3 = 0, p4, p5 the depth terms)
    const float cot = 0.5f + 3.0f * U();
    c.p[0] = cot / (1.0f + U());
    c.p[1] = (Rnd() % 4 == 0) ? 0.3f * S() : 0.0f;
    c.p[2] = cot;
    c.p[3] = (Rnd() % 4 == 0) ? 0.3f * S() : 0.0f;
    c.p[4] = -1.0f - U() * 1e-3f;
    c.p[5] = -(1.0f + 100.0f * U());
    c.ortho = false;
    c.sides = Rnd() % 8 != 0;
    const float vpW = 16.0f + 1000.0f * U(), vpH = 16.0f + 600.0f * U();
    c.kx = 1.0f + 16.0f / (c.sides ? vpW : 1.0f);
    c.ky = 1.0f + 16.0f / (c.sides ? vpH : 1.0f);
    c.pSum = std::fabs(c.p[0]) + std::fabs(c.p[1]) + std::fabs(c.p[2]) + std::fabs(c.p[3]) + 1.0f;
}
void MakeMtx(float mm[3][4], float scale, float trans) {
    // (a rotation by three angles, a scale per column, a translation)
    const float a = 6.2831853f * U(), b = 6.2831853f * U(), g = 6.2831853f * U();
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cg = std::cos(g), sg = std::sin(g);
    const float r[3][3] = {{cb * cg, sa * sb * cg - ca * sg, ca * sb * cg + sa * sg}, {cb * sg, sa * sb * sg + ca * cg, ca * sb * sg - sa * cg}, {-sb, sa * cb, ca * cb}};
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) mm[i][k] = r[i][k] * scale * (0.25f + U());
        mm[i][3] = trans * S();
    }
}
struct Draw {
    ClipNow c;
    float mm[3][4];
    float v[8][4];
    uint32_t count;
};
// kind 0: anywhere; 1: a small quad somewhere (a particle); 2: a quad pushed to a chosen plane, at a chosen multiple of the margin;
// 3: with odd numbers in the points; 4: with odd numbers in the matrix or the projection
void MakeDraw(Draw& d, int kind) {
    MakeClip(d.c);
    const float scale = (Rnd() % 3 == 0) ? 0.02f : (Rnd() % 2 ? 1.0f : 40.0f);
    MakeMtx(d.mm, scale, (Rnd() % 2) ? 3000.0f : 200.0f);
    d.count = (Rnd() % 4 == 0) ? 1 + Rnd() % 8 : (Rnd() % 2 ? 4 : 8);
    const float ext = (Rnd() % 2) ? 50.0f : 127.0f;
    for (uint32_t i = 0; i < d.count; ++i) {
        d.v[i][0] = ext * S(); d.v[i][1] = ext * S(); d.v[i][2] = (Rnd() % 2) ? 0.0f : ext * S(); d.v[i][3] = 0.0f;
        if (kind == 1) { d.v[i][0] = (i & 1) ? ext : -ext; d.v[i][1] = (i & 2) ? ext : -ext; d.v[i][2] = 0.0f; }
    }
    if (kind == 2) {
        // Move the draw along eye x, y or z so that its first point lands at `f` margins beyond a plane (f around 1: on the margin; around the
        // bound's ratio; 0: on the plane), the others following.
        const float* m[3] = {d.mm[0], d.mm[1], d.mm[2]};
        const int plane = Rnd() % 5;
        static const float kF[] = {0.0f, 0.5f, 0.999f, 1.0f, 1.001f, 1.5f, 2.0f, 3.0f, 10.0f, -1.0f};
        const float f = kF[Rnd() % 10] * (1.0f + 1e-6f * S());
        for (int it = 0; it < 6; ++it) { // (a few steps of a secant-like correction on the translation)
            const float x = d.v[0][0], y = d.v[0][1], z = d.v[0][2];
            const float ex = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
            const float ey = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
            const float ez = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
            const float cx = d.c.p[0] * ex + d.c.p[1] * ez, cy = d.c.p[2] * ey + d.c.p[3] * ez, cw = -ez;
            const float a = std::fabs(d.c.p[0] * ex) + std::fabs(d.c.p[1] * ez) + std::fabs(d.c.p[2] * ey) + std::fabs(d.c.p[3] * ez) + std::fabs(ez);
            float erow = 0.0f;
            for (int r = 0; r < 3; ++r)
                erow = std::max(erow, std::fabs(m[r][0]) * std::fabs(x) + std::fabs(m[r][1]) * std::fabs(y) + std::fabs(m[r][2]) * std::fabs(z) + std::fabs(m[r][3]));
            const float margin = 2.0f * ((1e-4f * erow + 1e-6f) * d.c.pSum + 1e-4f * a) + 1e-6f;
            const float want = f * margin;
            float val;
            switch (plane) {
            case 0: val = cx - d.c.kx * cw; if (d.c.p[0] != 0.0f) d.mm[0][3] += (want - val) / d.c.p[0]; break;
            case 1: val = -cx - d.c.kx * cw; if (d.c.p[0] != 0.0f) d.mm[0][3] -= (want - val) / d.c.p[0]; break;
            case 2: val = cy - d.c.ky * cw; if (d.c.p[2] != 0.0f) d.mm[1][3] += (want - val) / d.c.p[2]; break;
            case 3: val = -cy - d.c.ky * cw; if (d.c.p[2] != 0.0f) d.mm[1][3] -= (want - val) / d.c.p[2]; break;
            default: val = -cw; d.mm[2][3] += want - val; break;
            }
        }
    }
    if (kind == 3) {
        const uint32_t n = 1 + Rnd() % 3;
        for (uint32_t k = 0; k < n; ++k) d.v[Rnd() % d.count][Rnd() % 3] = Odd();
    }
    if (kind == 4) {
        const uint32_t n = 1 + Rnd() % 3;
        for (uint32_t k = 0; k < n; ++k) {
            if (Rnd() % 3) d.mm[Rnd() % 3][Rnd() % 4] = Odd();
            else d.c.p[Rnd() % 4] = Odd();
        }
        // (ClipNowOfCalc's sum: at least 1, or infinite, or NaN; never anything else)
        d.c.pSum = std::fabs(d.c.p[0]) + std::fabs(d.c.p[1]) + std::fabs(d.c.p[2]) + std::fabs(d.c.p[3]) + 1.0f;
    }
}
} // namespace

int main(int argc, char** argv) {
    const uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 4000000ull;
    uint64_t bad = 0, outside[5] = {}, total[5] = {};
    for (uint64_t i = 0; i < n; ++i) {
        const int kind = static_cast<int>(i % 5);
        Draw d;
        MakeDraw(d, kind);
        const float* m[3] = {d.mm[0], d.mm[1], d.mm[2]};
        const uint32_t stride = 16;
        const bool r = RefOutside(d.c, m, reinterpret_cast<const uint8_t*>(d.v), d.count, stride);
        const bool f = NewOutside(d.c, m, reinterpret_cast<const uint8_t*>(d.v), d.count, stride);
        ++total[kind];
        if (r) ++outside[kind];
        if (r != f) {
            if (++bad <= 5) std::printf("MISMATCH at draw %llu (kind %d, %u points): the loop over PointOutMask says %d, the new test %d\n", static_cast<unsigned long long>(i), kind, d.count, r, f);
        }
    }
    static const char* const kName[5] = {"random", "particle quads", "on the planes and margins", "odd numbers in the points", "odd numbers in the matrix / projection"};
    for (int k = 0; k < 5; ++k)
        std::printf("%-40s %9llu draws, %9llu outside\n", kName[k], static_cast<unsigned long long>(total[k]), static_cast<unsigned long long>(outside[k]));
    std::printf("%llu draws, %llu mismatches\n", static_cast<unsigned long long>(n), static_cast<unsigned long long>(bad));

    // ---- the micro-benchmark (host time): quads that are outside (every point tested: the 65 % of the console's dynamic draws) and quads whose
    // first point is inside (one point tested)
    if (argc > 2) {
        for (int inside = 0; inside < 2; ++inside) {
            std::vector<Draw> ds;
            g_seed = 12345;
            while (ds.size() < 4096) {
                Draw d;
                MakeDraw(d, 1);
                d.c.sides = true;
                d.count = 4;
                const float* m[3] = {d.mm[0], d.mm[1], d.mm[2]};
                const bool r = RefOutside(d.c, m, reinterpret_cast<const uint8_t*>(d.v), d.count, 16);
                const bool firstIn = PointOutMask(d.c, m, d.v[0][0], d.v[0][1], d.v[0][2]) == 0;
                if (inside ? firstIn : r) ds.push_back(d);
            }
            double ns[2];
            for (int which = 0; which < 2; ++which) {
                uint64_t acc = 0;
                const auto t0 = std::chrono::steady_clock::now();
                for (int rep = 0; rep < 2000; ++rep)
                    for (const Draw& d : ds) {
                        const float* m[3] = {d.mm[0], d.mm[1], d.mm[2]};
                        acc += which ? NewOutside(d.c, m, reinterpret_cast<const uint8_t*>(d.v), d.count, 16) : RefOutside(d.c, m, reinterpret_cast<const uint8_t*>(d.v), d.count, 16);
                    }
                const auto t1 = std::chrono::steady_clock::now();
                ns[which] = std::chrono::duration<double, std::nano>(t1 - t0).count() / (2000.0 * ds.size());
                if (acc == 12345678901ull) std::printf(" ");
            }
            std::printf("benchmark (host time), 4-point quads %s: the loop over PointOutMask %.1f ns a draw, the new test %.1f ns a draw (%.0f %%)\n",
                        inside ? "whose first point is inside" : "outside (all four points tested)", ns[0], ns[1], 100.0 * ns[1] / ns[0]);
        }
    }
    if (bad) { std::printf("FAIL\n"); return 1; }
    std::printf("PASS\n");
    return 0;
}
