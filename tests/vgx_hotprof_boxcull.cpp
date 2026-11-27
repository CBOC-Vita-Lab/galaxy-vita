// hotprof25 round 2: host test and micro-benchmark of GXV_HOTPROF_BOXCULL (platform/vgx/src/vgx_hotprof_boxcull.inc, the very file
// vgx_draw.cpp includes): HpBoxOutMask against BoxOutMaskOld, the box test as it was. Every box must get the same mask: random boxes
// around a camera, boxes built to sit on the planes and on the margin, boxes with NaN / infinite / denormal / huge numbers, perspective
// and orthographic, with and without the side planes. `-DHP_BOX_NEG=100.0f`: a negative control (the early end taken above the margin):
// must FAIL.   tools/test_vgx_hotprof_boxcull.sh
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
struct RowBox {
    float mn[3], mx[3];
    uint32_t row;
    float c[3] = {}, r = 0;
    uint32_t drawn = 0;
};
#include "../platform/vgx/src/vgx_hotprof_boxcull.inc"
// neonbox40: BoxOutMaskOld as it was at 60a8bc63 (plain float compares), verbatim: the .inc's version now uses fpb::Active, which must give its masks.
namespace origbox {
inline uint32_t OrigBoxOutMaskOld(const ClipNow& c, const float* const m[3], const RowBox& b) {
    const float* const p = c.p;
    const bool ortho = c.ortho;
    {
        // The box's centre first: inside the clip volume, the box is not outside it (most drawn runs stop here). An early
        // "not outside" is always safe; the eight corners below decide the rest.
        const float x = 0.5f * (b.mn[0] + b.mx[0]), y = 0.5f * (b.mn[1] + b.mx[1]), z = 0.5f * (b.mn[2] + b.mx[2]);
        const float ex = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
        const float ey = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
        const float ez = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
        const float cx = ortho ? p[0] * ex + p[1] : p[0] * ex + p[1] * ez, cy = ortho ? p[2] * ey + p[3] : p[2] * ey + p[3] * ez;
        const float cw = ortho ? 1.0f : -ez;
        if (std::fabs(cx) <= cw && std::fabs(cy) <= cw) return 0;
    }
    // The eye-space error bound: the magnitudes a row sums, for the largest coordinates of the box.
    const float ax = std::max(std::fabs(b.mn[0]), std::fabs(b.mx[0])), ay = std::max(std::fabs(b.mn[1]), std::fabs(b.mx[1])),
                az = std::max(std::fabs(b.mn[2]), std::fabs(b.mx[2]));
    float erow = 0.0f;
    for (int r = 0; r < 3; ++r)
        erow = std::max(erow, std::fabs(m[r][0]) * ax + std::fabs(m[r][1]) * ay + std::fabs(m[r][2]) * az + std::fabs(m[r][3]));
    const float eEye = 1e-4f * erow + 1e-6f;
    float cx[8], cy[8], cw[8], mag = 0.0f;
    for (int k = 0; k < 8; ++k) {
        const float x = (k & 1) ? b.mx[0] : b.mn[0], y = (k & 2) ? b.mx[1] : b.mn[1], z = (k & 4) ? b.mx[2] : b.mn[2];
        const float ex = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
        const float ey = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
        const float ez = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
        // (BuildProjC: x = p0 ex + p1 ez, y = -(p2 ey + p3 ez), w = -ez; orthographic: x = p0 ex + p1, y = -(p2 ey + p3), w = 1.
        // The sign of y does not matter: both of its planes are tested.)
        float a;
        if (ortho) {
            cx[k] = p[0] * ex + p[1]; cy[k] = p[2] * ey + p[3]; cw[k] = 1.0f;
            a = std::fabs(p[0] * ex) + std::fabs(p[1]) + std::fabs(p[2] * ey) + std::fabs(p[3]) + 1.0f;
        } else {
            cx[k] = p[0] * ex + p[1] * ez; cy[k] = p[2] * ey + p[3] * ez; cw[k] = -ez;
            a = std::fabs(p[0] * ex) + std::fabs(p[1] * ez) + std::fabs(p[2] * ey) + std::fabs(p[3] * ez) + std::fabs(ez);
        }
        mag = std::max(mag, a);
    }
    const float margin = 2.0f * (eEye * c.pSum + 1e-4f * mag) + 1e-6f;
    if (!(margin < 1e30f)) return 0; // (infinite or NaN: nothing is known)
    // ("outside by more than the margin" at all eight corners; a NaN corner fails the test)
    bool right = c.sides, left = c.sides, top = c.sides, bottom = c.sides, behind = !ortho;
    for (int k = 0; k < 8; ++k) {
        right = right && cx[k] - c.kx * cw[k] > margin;
        left = left && -cx[k] - c.kx * cw[k] > margin;
        top = top && cy[k] - c.ky * cw[k] > margin;
        bottom = bottom && -cy[k] - c.ky * cw[k] > margin;
        behind = behind && -cw[k] > margin;
    }
    return (right ? 1u : 0u) | (left ? 2u : 0u) | (top ? 4u : 0u) | (bottom ? 8u : 0u) | (behind ? 16u : 0u);
}
}
__attribute__((noinline)) uint32_t RunOld(const ClipNow& c, const float* const m[3], const RowBox& b) { return BoxOutMaskOld(c, m, b); }
__attribute__((noinline)) uint32_t RunNew(const ClipNow& c, const float* const m[3], const RowBox& b) { return HpBoxOutMask(c, m, b); }

struct Rng {
    uint64_t s;
    uint32_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<uint32_t>(s >> 33); }
    float unit() { return static_cast<float>(next() & 0xFFFFFF) / 16777216.0f; }
    float sym(float a) { return (unit() * 2.0f - 1.0f) * a; }
};
struct Case {
    ClipNow c;
    float m[3][4];
    RowBox b;
};
float Special(Rng& r) {
    switch (r.next() % 10) {
    case 0: return std::numeric_limits<float>::quiet_NaN();
    case 1: return std::numeric_limits<float>::infinity();
    case 2: return -std::numeric_limits<float>::infinity();
    case 3: return 1e-42f;
    case 4: return -1e-42f;
    case 5: return 3e38f;
    case 6: return -3e38f;
    case 7: return 1e20f;
    case 8: return -0.0f;
    default: return 0.0f;
    }
}
void MakeClip(Rng& r, ClipNow& c) {
    c.ortho = r.next() % 8 == 0;
    c.sides = r.next() % 6 != 0;
    if (c.ortho) {
        const float w = 100.0f + r.unit() * 2000.0f, h = 100.0f + r.unit() * 2000.0f;
        c.p[0] = 2.0f / w; c.p[1] = r.sym(0.2f); c.p[2] = 2.0f / h; c.p[3] = r.sym(0.2f); c.p[4] = -1e-4f; c.p[5] = -0.1f;
    } else {
        const float f = 0.6f + r.unit() * 3.0f, aspect = 1.0f + r.unit();
        c.p[0] = f / aspect; c.p[1] = r.next() % 4 == 0 ? r.sym(0.1f) : 0.0f; c.p[2] = f; c.p[3] = r.next() % 4 == 0 ? r.sym(0.1f) : 0.0f;
        c.p[4] = -1.0001f; c.p[5] = -10.0f;
    }
    const float vpW = 200.0f + r.unit() * 1000.0f, vpH = 150.0f + r.unit() * 600.0f;
    c.kx = 1.0f + 16.0f / (c.sides ? vpW : 1.0f);
    c.ky = 1.0f + 16.0f / (c.sides ? vpH : 1.0f);
    c.pSum = std::fabs(c.p[0]) + std::fabs(c.p[1]) + std::fabs(c.p[2]) + std::fabs(c.p[3]) + 1.0f; // (ClipNowOfCalc)
}
void MakeMtx(Rng& r, float m[3][4], float scale) {
    // a rotation about two axes, a scale, a translation in front of / around the eye
    const float a = r.sym(3.14159f), b2 = r.sym(3.14159f);
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b2), sb = std::sin(b2);
    const float R[3][3] = {{ca, 0.0f, sa}, {sa * sb, cb, -ca * sb}, {-sa * cb, sb, ca * cb}};
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) m[i][k] = R[i][k] * scale;
    m[0][3] = r.sym(4000.0f); m[1][3] = r.sym(3000.0f); m[2][3] = -r.unit() * 8000.0f + 1500.0f;
}
void MakeCase(Rng& r, Case& k) {
    MakeClip(r, k.c);
    const uint32_t kind = r.next() % 100;
    MakeMtx(r, k.m, kind < 8 ? std::pow(10.0f, r.sym(6.0f)) : 0.5f + r.unit() * 2.0f);
    float cen[3], ext[3];
    for (int i = 0; i < 3; ++i) { cen[i] = r.sym(600.0f); ext[i] = (r.next() % 5 == 0 ? r.unit() * 3000.0f : r.unit() * 300.0f); }
    if (kind >= 8 && kind < 60) {
        // a box placed near a chosen plane of the volume: a point at depth d on (or a hair off) the plane, as the box's centre or one corner
        const float d = 50.0f + r.unit() * 6000.0f;
        float ex, ey, ez = -d;
        const uint32_t plane = r.next() % 5;
        const float w = k.c.ortho ? 1.0f : d;
        const float hair = (r.next() % 3 == 0) ? 0.0f : r.sym(1.0f) * std::pow(10.0f, -1.0f - 6.0f * r.unit()) * w;
        ex = r.sym(0.9f) * w / std::max(1e-6f, std::fabs(k.c.p[0])); ey = r.sym(0.9f) * w / std::max(1e-6f, std::fabs(k.c.p[2]));
        if (plane == 0) ex = (k.c.kx * w + hair - (k.c.ortho ? k.c.p[1] : k.c.p[1] * ez)) / k.c.p[0];
        if (plane == 1) ex = (-k.c.kx * w - hair - (k.c.ortho ? k.c.p[1] : k.c.p[1] * ez)) / k.c.p[0];
        if (plane == 2) ey = (k.c.ky * w + hair - (k.c.ortho ? k.c.p[3] : k.c.p[3] * ez)) / k.c.p[2];
        if (plane == 3) ey = (-k.c.ky * w - hair - (k.c.ortho ? k.c.p[3] : k.c.p[3] * ez)) / k.c.p[2];
        if (plane == 4) ez = hair * 10.0f;
        // model = identity rotation with the translation chosen so that a box corner (or its centre) lands on that eye point
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) k.m[i][j] = i == j ? 1.0f : 0.0f;
        const uint32_t where = r.next() % 3; // 0: the min corner, 1: the max corner, 2: the centre
        const float e[3] = {ex, ey, ez};
        for (int i = 0; i < 3; ++i) {
            const float anchor = where == 0 ? cen[i] - ext[i] : where == 1 ? cen[i] + ext[i] : cen[i];
            k.m[i][3] = e[i] - anchor;
        }
        if (r.next() % 2) { // the box pushed outward from the plane so that the anchored corner is its nearest one (the margin decides)
            const float push = ext[plane < 2 ? 0 : plane < 4 ? 1 : 2];
            const int ax = plane < 2 ? 0 : plane < 4 ? 1 : 2;
            const float sgn = (plane == 0 || plane == 2 || plane == 4) ? 1.0f : -1.0f;
            k.m[ax][3] += sgn * push * (where == 2 ? 1.0f : where == 0 ? (sgn > 0 ? 0.0f : -2.0f) : (sgn > 0 ? 2.0f : 0.0f));
        }
    }
    for (int i = 0; i < 3; ++i) { k.b.mn[i] = cen[i] - ext[i]; k.b.mx[i] = cen[i] + ext[i]; }
    if (kind >= 60 && kind < 66) { // odd numbers somewhere
        const uint32_t n = 1 + r.next() % 3;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t w = r.next() % 24;
            const float v = Special(r);
            if (w < 12) k.m[w / 4][w % 4] = v;
            else if (w < 15) k.b.mn[w - 12] = v;
            else if (w < 18) k.b.mx[w - 15] = v;
            else if (w < 22) k.c.p[w - 18] = v;
            else if (w == 22) k.c.kx = v;
            else k.c.pSum = v;
        }
    }
    if (kind == 66) for (int i = 0; i < 3; ++i) k.b.mx[i] = k.b.mn[i]; // a point
    if (kind == 67) std::swap(k.b.mn[0], k.b.mx[0]);                    // an inverted box
}
} // namespace

int main(int argc, char** argv) {
    const uint64_t n = argc > 1 ? strtoull(argv[1], nullptr, 10) : 4000000ull;
    const bool bench = argc > 2;
    Rng r{12345};
    uint64_t bad = 0, out = 0, perPlane[5] = {}, multi = 0, centreIn = 0;
    std::vector<Case> keep;
    for (uint64_t i = 0; i < n; ++i) {
        Case k;
        MakeCase(r, k);
        const float* const m[3] = {k.m[0], k.m[1], k.m[2]};
        const uint32_t o = RunOld(k.c, m, k.b), f = RunNew(k.c, m, k.b), og = origbox::OrigBoxOutMaskOld(k.c, m, k.b);
        if (og != o && bad++ < 5) std::printf("MISMATCH box %llu: original %u BoxOutMaskOld %u\n", static_cast<unsigned long long>(i), og, o);
        if (o) { ++out; if (o & (o - 1)) ++multi; for (int p = 0; p < 5; ++p) if (o >> p & 1) ++perPlane[p]; }
        if (o != f && bad++ < 5) std::printf("MISMATCH box %llu: old %u new %u\n", static_cast<unsigned long long>(i), o, f);
        if (bench && keep.size() < 200000) keep.push_back(k);
    }
    std::printf("boxes %llu: mask not 0 for %llu (right %llu left %llu top %llu bottom %llu behind %llu; more than one plane %llu), %llu mismatches\n",
                static_cast<unsigned long long>(n), static_cast<unsigned long long>(out), static_cast<unsigned long long>(perPlane[0]),
                static_cast<unsigned long long>(perPlane[1]), static_cast<unsigned long long>(perPlane[2]), static_cast<unsigned long long>(perPlane[3]),
                static_cast<unsigned long long>(perPlane[4]), static_cast<unsigned long long>(multi), static_cast<unsigned long long>(bad));
    (void)centreIn;
    if (bench) {
        // three mixes: every kept box; only boxes that reach the corners with mask 0 (straddling); only boxes with a mask (culled)
        for (int mix = 0; mix < 3; ++mix) {
            std::vector<const Case*> v;
            for (const Case& k : keep) {
                const float* const m[3] = {k.m[0], k.m[1], k.m[2]};
                const uint32_t o = RunOld(k.c, m, k.b);
                if (mix == 0 || (mix == 1 && o == 0) || (mix == 2 && o != 0)) v.push_back(&k);
            }
            double t[2] = {1e30, 1e30};
            uint64_t sum[2] = {};
            for (int rep = 0; rep < 7; ++rep)
                for (int which = 0; which < 2; ++which) {
                    const auto t0 = std::chrono::steady_clock::now();
                    uint64_t s = 0;
                    for (int loop = 0; loop < 10; ++loop)
                        for (const Case* k : v) {
                            const float* const m[3] = {k->m[0], k->m[1], k->m[2]};
                            s += which ? RunNew(k->c, m, k->b) : RunOld(k->c, m, k->b);
                        }
                    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (10.0 * static_cast<double>(v.size()));
                    t[which] = std::min(t[which], ns);
                    sum[which] = s;
                }
            std::printf("bench %s: %llu boxes, old %.1f ns, new %.1f ns a box (host), sums %s\n", mix == 0 ? "all" : mix == 1 ? "mask 0" : "mask not 0",
                        static_cast<unsigned long long>(v.size()), t[0], t[1], sum[0] == sum[1] ? "equal" : "DIFFER");
        }
    }
    if (bad) { std::printf("FAIL\n"); return 1; }
    std::printf("PASS\n");
    return 0;
}
