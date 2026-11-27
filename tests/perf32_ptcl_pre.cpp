// perf32 (20 Nov 2026): host test and micro-benchmark of GXV_PTCL_PRE's centre test (platform/include/port/ptcl_cull.h QuadPreOutside) against the
// kernel it answers for (QuadOutside -> HpRigidOutside, the very code the game runs). The property: QuadPreOutside true => QuadOutside true, for
// every clip volume, matrix and quad; so "pre || kernel" is the kernel's answer for every quad (checked as such too). Random quads as the game
// makes them, quads walked across each plane one float at a time around the centre test's limit, and degenerate numbers (NaN, infinite, huge,
// denormal, zero). -DPTCL_PRE_SLACK=<negative>: the negative control (the allowance for rounding turned into a head start): must FAIL.
//   tools/test_perf32.sh
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <vector>
#include "port/ptcl_cull.h"

using namespace port::ptclcull;
namespace {
std::mt19937 rng(20261120u);
float U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
int I(int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); }
unsigned long long gCases = 0, gPre = 0, gOut = 0, gBad = 0;

struct Quad {
    ClipNow c;
    bool clipOk;
    float m[3][4];
    float verts[8][3];
    uint32_t n;
};
void Check(const Quad& q) {
    if (!q.clipOk) return;
    QuadPre pre;
    QuadPreOf(pre, q.c, q.verts, q.n);
    const bool k = QuadOutside(q.c, q.m, q.verts, q.n);
    const bool p = pre.ok && QuadPreOutside(q.c, pre, q.m);
    ++gCases;
    if (k) ++gOut;
    if (p) ++gPre;
    if (p && !k) {
        if (gBad < 5) {
            printf("  WRONG: the centre test says outside, the kernel does not: t = (%g, %g, %g), MX %g, sides %d, n %u\n", (double)q.m[0][3], (double)q.m[1][3],
                   (double)q.m[2][3], (double)pre.MX, q.c.sides ? 1 : 0, q.n);
        }
        ++gBad;
    }
}
// A perspective projection as the game's cameras make it (GXSetProjection's six numbers), a viewport and a scissor.
void MakeClip(Quad& q, bool odd) {
    float proj[7], vp[6];
    uint32_t sc[4];
    const float fovy = U(10.0f, 100.0f) * 3.14159265f / 180.0f, aspect = U(1.0f, 2.0f), n = U(1.0f, 200.0f), f = n * U(10.0f, 100000.0f);
    const float cot = 1.0f / std::tan(fovy * 0.5f);
    proj[0] = 0.0f;
    proj[1] = cot / aspect; proj[2] = odd ? U(-0.5f, 0.5f) : 0.0f; proj[3] = cot; proj[4] = odd ? U(-0.5f, 0.5f) : 0.0f;
    proj[5] = -n / (f - n); proj[6] = -(f * n) / (f - n);
    vp[0] = odd ? U(-50.0f, 50.0f) : 0.0f; vp[1] = odd ? U(-50.0f, 50.0f) : 0.0f; vp[2] = odd ? U(8.0f, 2000.0f) : 640.0f; vp[3] = odd ? U(8.0f, 2000.0f) : 456.0f;
    vp[4] = 0.0f; vp[5] = 1.0f;
    sc[0] = odd && I(0, 3) == 0 ? I(0, 100) : 0; sc[1] = 0; sc[2] = odd && I(0, 3) == 0 ? I(1, 900) : 640; sc[3] = 456;
    q.clipOk = ClipFromGx(q.c, proj, vp, sc);
}
void MakeVerts(Quad& q) {
    uint8_t base[0x4C * 3];
    for (auto& b : base) b = static_cast<uint8_t>(I(-128, 127));
    if (I(0, 2) != 0) {   // (the usual quad: +-25 or +-50 in a plane)
        const int8_t h = static_cast<int8_t>(I(0, 1) ? 25 : 50);
        const int8_t k[4][3] = {{static_cast<int8_t>(-h), h, 0}, {h, h, 0}, {h, static_cast<int8_t>(-h), 0}, {static_cast<int8_t>(-h), static_cast<int8_t>(-h), 0}};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 3; ++j) {
                base[i * 3 + j] = static_cast<uint8_t>(k[i][j]);
                base[(0x48 + i) * 3 + j] = static_cast<uint8_t>(k[i][(j + 1) % 3]);
            }
    }
    const bool cross = I(0, 1) != 0;
    q.n = QuadVerts(q.verts, base, cross);
}
// A rotation times a scale, and a translation given apart.
void MakeMtx(Quad& q, float scale, const float t[3]) {
    const float a = U(0.0f, 6.2831853f), b = U(0.0f, 6.2831853f), g = U(0.0f, 6.2831853f);
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cg = std::cos(g), sg = std::sin(g);
    const float r[3][3] = {{ca * cb, ca * sb * sg - sa * cg, ca * sb * cg + sa * sg}, {sa * cb, sa * sb * sg + ca * cg, sa * sb * cg - ca * sg}, {-sb, cb * sg, cb * cg}};
    const float sx = scale * U(0.2f, 3.0f), sy = scale * U(0.2f, 3.0f), sz = scale;
    for (int i = 0; i < 3; ++i) {
        q.m[i][0] = r[i][0] * sx; q.m[i][1] = r[i][1] * sy; q.m[i][2] = r[i][2] * sz; q.m[i][3] = t[i];
    }
}
}  // namespace

int main() {
    // 1. random quads around a camera: in front, behind, to the sides, near and far, small and large
    for (int it = 0; it < 3000000; ++it) {
        Quad q;
        MakeClip(q, it % 4 == 3);
        MakeVerts(q);
        const float dist = std::pow(10.0f, U(0.0f, 5.5f));
        float t[3] = {U(-3.0f, 3.0f) * dist, U(-3.0f, 3.0f) * dist, U(-1.0f, 0.3f) * dist};   // (the eye looks down -z)
        MakeMtx(q, std::pow(10.0f, U(-3.0f, 2.5f)), t);
        Check(q);
    }
    const unsigned long long randomCases = gCases, randomPre = gPre, randomOut = gOut;
    // 2. a quad walked across the centre test's limit of every plane: bisect the translation along an axis for the place where the centre test's
    //    answer changes, then every float within 64 of it on both sides
    unsigned long long walks = 0;
    for (int it = 0; it < 40000; ++it) {
        Quad q;
        MakeClip(q, it % 4 == 3);
        if (!q.clipOk) continue;
        MakeVerts(q);
        QuadPre pre;
        QuadPreOf(pre, q.c, q.verts, q.n);
        const float dist = std::pow(10.0f, U(0.5f, 4.5f));
        float t[3] = {U(-0.2f, 0.2f) * dist, U(-0.2f, 0.2f) * dist, -dist};
        MakeMtx(q, std::pow(10.0f, U(-2.0f, 1.5f)), t);
        const int axis = it % 3;
        const float dir = (it / 3) % 2 ? 1.0f : -1.0f;
        float lo = q.m[axis][3], hi = q.m[axis][3] + dir * dist * 40.0f;   // (lo: on the screen; hi: far outside)
        q.m[axis][3] = hi;
        if (!(pre.ok && QuadPreOutside(q.c, pre, q.m))) continue;
        q.m[axis][3] = lo;
        if (pre.ok && QuadPreOutside(q.c, pre, q.m)) continue;
        for (int k = 0; k < 200 && lo != hi; ++k) {
            const float mid = lo + (hi - lo) * 0.5f;
            if (mid == lo || mid == hi) break;
            q.m[axis][3] = mid;
            if (pre.ok && QuadPreOutside(q.c, pre, q.m)) hi = mid; else lo = mid;
        }
        float x = hi;
        for (int k = 0; k < 64; ++k) x = std::nextafter(x, lo);
        for (int k = 0; k < 128; ++k) {
            q.m[axis][3] = x;
            Check(q);
            x = std::nextafter(x, hi + dir * 1e30f);
        }
        ++walks;
    }
    // 3. degenerate numbers in the matrix and the projection
    const float odd[] = {0.0f, -0.0f, 1e-45f, -1e-45f, 1e-38f, 1e-20f, 1.0f, -1.0f, 1e10f, -1e10f, 1e19f, 1e29f, 1e31f, -1e31f, 3.0e38f, -3.0e38f,
                         std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
    const int nOdd = static_cast<int>(sizeof odd / sizeof odd[0]);
    for (int it = 0; it < 1500000; ++it) {
        Quad q;
        MakeClip(q, it % 2 == 1);
        MakeVerts(q);
        float t[3] = {U(-3000.0f, 3000.0f), U(-3000.0f, 3000.0f), U(-3000.0f, 800.0f)};
        MakeMtx(q, U(0.01f, 30.0f), t);
        const int k = I(1, 4);
        for (int j = 0; j < k; ++j) q.m[I(0, 2)][I(0, 3)] = odd[I(0, nOdd - 1)];
        if (it % 5 == 0) q.c.p[I(0, 3)] = odd[I(0, nOdd - 1)], q.c.pSum = std::fabs(q.c.p[0]) + std::fabs(q.c.p[1]) + std::fabs(q.c.p[2]) + std::fabs(q.c.p[3]) + 1.0f;
        if (it % 7 == 0) q.c.kx = odd[I(0, nOdd - 1)];
        if (it % 11 == 0) q.c.sides = !q.c.sides;
        if (!(q.c.pSum < 1e30f)) continue;   // (ClipFromGx refuses such a projection: no test in the game)
        Check(q);
    }
    printf("perf32_ptcl_pre: %llu quads (%llu random: %llu outside, %llu of them answered by the centre test = %.1f %%; %llu walks across the limit; the rest degenerate): %llu wrong\n",
           gCases, randomCases, randomOut, randomPre, randomOut ? 100.0 * randomPre / randomOut : 0.0, walks, gBad);
    // 4. the price on this machine (not the console's): the game's usual quads (+-25 / +-50, scale 0.3 .. 3, 300 .. 10,000 units from the eye, up to 2.5
    //    half-screens off the axis), the kernel alone against centre test + kernel; the best of five passes each
    {
        std::vector<Quad> qs(20000);
        std::vector<QuadPre> pres(qs.size());
        size_t n = 0;
        unsigned answered = 0;
        while (n < qs.size()) {
            Quad& q = qs[n];
            MakeClip(q, false);
            do MakeVerts(q); while (q.verts[0][0] != -25.0f && q.verts[0][0] != -50.0f);
            const float dist = std::pow(10.0f, U(2.5f, 4.0f));
            float t[3] = {U(-2.5f, 2.5f) * dist, U(-2.5f, 2.5f) * dist, -dist};
            MakeMtx(q, U(0.3f, 1.0f), t);
            if (!q.clipOk) continue;
            QuadPreOf(pres[n], q.c, q.verts, q.n);
            if (pres[n].ok && QuadPreOutside(q.c, pres[n], q.m)) ++answered;
            ++n;
        }
        unsigned a = 0, b = 0;
        double na = 1e30, nb = 1e30;
        for (int pass = 0; pass < 5; ++pass) {
            a = b = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (int rep = 0; rep < 50; ++rep)
                for (size_t i = 0; i < qs.size(); ++i) a += QuadOutside(qs[i].c, qs[i].m, qs[i].verts, qs[i].n) ? 1u : 0u;
            const auto t1 = std::chrono::steady_clock::now();
            for (int rep = 0; rep < 50; ++rep)
                for (size_t i = 0; i < qs.size(); ++i)
                    b += ((pres[i].ok && QuadPreOutside(qs[i].c, pres[i], qs[i].m)) || QuadOutside(qs[i].c, qs[i].m, qs[i].verts, qs[i].n)) ? 1u : 0u;
            const auto t2 = std::chrono::steady_clock::now();
            na = std::min(na, std::chrono::duration<double, std::nano>(t1 - t0).count() / (50.0 * qs.size()));
            nb = std::min(nb, std::chrono::duration<double, std::nano>(t2 - t1).count() / (50.0 * qs.size()));
        }
        printf("  price (host; %.0f %% of the quads outside, %.0f %% answered by the centre test): kernel %.1f ns a quad, centre test + kernel %.1f ns a quad (%.0f %% of it)\n",
               100.0 * a / (50.0 * qs.size()), 100.0 * answered / qs.size(), na, nb, 100.0 * nb / na);
        if (a != b) { printf("  WRONG: the two loops disagree (%u, %u)\n", a, b); ++gBad; }
    }
    if (gPre == 0) { printf("FAIL: the centre test answered nothing\n"); return 1; }
    printf(gBad ? "perf32_ptcl_pre: FAIL\n" : "perf32_ptcl_pre: PASS\n");
    return gBad ? 1 : 0;
}
