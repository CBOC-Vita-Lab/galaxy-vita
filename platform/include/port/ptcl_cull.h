// GXV_HOTPROF_PTCLCULL (hotprof25, 11 Nov; decomp/src/JSystem/JParticle/JPAResource.cpp has the switch and the reasons): the game side of
// "a particle quad wholly outside one plane of the clip volume is not sent". The test is the front end's own (GXV_DYNCULL, vgx_draw.cpp
// DynDrawOutside -> platform/vgx/src/vgx_hotprof_dyncull.inc, the very file, included below): every vertex of the quad, through the position
// matrix the particle would load, beyond one and the same plane by more than the float error bound. What it needs of the GX state is taken
// from the GX library's getters and turned into the clip volume the way the front end's ClipNowOfCalc turns the registers those values
// become into it (ClipFromGx). A header on its own so that tests/jpa_ptcl_cull.cpp runs the very code the game runs.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "fpbits.h"

namespace port {
namespace ptclcull {
// (vgx_draw.cpp's ClipNow: the projection's six numbers, which planes may be used, the side planes' factors, the error bound's sum)
struct ClipNow {
    float p[6];
    bool ortho, sides;
    float kx, ky, pSum;
};
#include "../../vgx/src/vgx_hotprof_dyncull.inc"

// The clip volume of the GX state: GXGetProjectionv's seven numbers (type, then the six the library sends to XF 0x1020..0x1025),
// GXGetViewportv's (left, top, width, height, near, far) and GXGetScissor's (left, top, width, height). false: no test (an orthographic
// projection: 2D effects; the front end tests those with another formula, and they are few).
//   The side planes are used only where the scissor lies inside the viewport, give or take 3.5 pixels (the front end allows 4 and
//   rebuilds the same numbers from the registers with rounding far below the half pixel between them: where this says "sides" it does
//   too), and then they stand 8 pixels outside the viewport (kx, ky: the front end's expression), so that what is left out is more
//   than 4 pixels beyond the scissor: no pixel of it is drawn whatever clips. Behind the eye is behind the near plane.
inline bool ClipFromGx(ClipNow& c, const float proj[7], const float vp[6], const uint32_t scis[4]) {
    if (proj[0] != 0.0f) return false;
    for (int i = 0; i < 6; ++i) c.p[i] = proj[1 + i];
    c.ortho = false;
    constexpr float kGuard = 3.5f;
    const float vpX = vp[0], vpY = vp[1], vpW = vp[2], vpH = vp[3];
    const float sx0 = static_cast<float>(scis[0]), sy0 = static_cast<float>(scis[1]);
    const float sx1 = static_cast<float>(scis[0] + scis[2]), sy1 = static_cast<float>(scis[1] + scis[3]);
    c.sides = vpW >= 16.0f && vpH >= 16.0f && vpW < 1e5f && vpH < 1e5f && sx0 >= vpX - kGuard && sx1 <= vpX + vpW + kGuard && sy0 >= vpY - kGuard &&
              sy1 <= vpY + vpH + kGuard;
    c.kx = 1.0f + 4.0f * 4.0f / (c.sides ? vpW : 1.0f);
    c.ky = 1.0f + 4.0f * 4.0f / (c.sides ? vpH : 1.0f);
    c.pSum = std::fabs(c.p[0]) + std::fabs(c.p[1]) + std::fabs(c.p[2]) + std::fabs(c.p[3]) + 1.0f;
    return c.pSum < 1e30f; // (a projection with a NaN or an infinite number: no test)
}
// The quad's model-space vertices as the front end decodes them: JParticle's lists index a position array of three signed bytes a
// vertex (GX_S8, no fraction bits): indices 0..3, and for the crossed quad also 72..75 (jpa_dl / jpa_dl_x). Returns their number.
inline uint32_t QuadVerts(float out[8][3], const uint8_t* posBase, bool cross) {
    static const uint8_t kIdx[8] = {0, 1, 2, 3, 0x48, 0x49, 0x4A, 0x4B};
    const uint32_t n = cross ? 8u : 4u;
    for (uint32_t i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) out[i][k] = static_cast<float>(static_cast<int8_t>(posBase[kIdx[i] * 3u + static_cast<uint32_t>(k)]));
    return n;
}
// Whether the quad through position matrix m (three rows of four) is outside one plane of c.
inline bool QuadOutside(const ClipNow& c, const float (*m)[4], const float verts[8][3], uint32_t n) {
    const float* const rows[3] = {m[0], m[1], m[2]};
    return HpRigidOutside(c, rows, reinterpret_cast<const uint8_t*>(verts), n, 12);
}
// perf32 (GXV_PTCL_PRE, 20 Nov 2026; default 0 in the code, on in both modes through mode_keys.h since 21 Nov (Vita: exact, a little cheaper); JPAResource.cpp port_jpa_ptcl_cull has the switch): the answer "outside" of QuadOutside for a quad
// that is far outside, from the quad's CENTRE alone (one point instead of four or eight). The PGO profile's counts say why: 85 % of the quads
// tested are outside, 6.25 points a quad, and nearly every point is decided by the kernel's bound, so the kernel's cost is the points.
//   QuadPreOutside is true only when HpRigidOutside is true (never the other way round: false means "ask the kernel", and the caller does).
//   The proof, in the kernel's own terms. A vertex v of the quad has |x| <= X, |y| <= Y, |z| <= Z (the largest over the quad's vertices: for the
//   usual flat quad Z is 0), so its eye-space point is the matrix's translation column t plus a d with |d_r| <= D_r = |m_r0|*X + |m_r1|*Y +
//   |m_r2|*Z. A plane value of the kernel is linear in the eye point:
//   tx = p0*ex + (p1 + kx)*ez (cw = -ez), nx = -(p0*ex + p1*ez) + kx*ez, ty / ny the same with p2, p3, ky and ey, near = ez. So every vertex's
//   value is at least the centre's value minus dev = |p0|*D_0 + (|p1| + kx)*D_2 (dev = D_2 for the near plane). When
//       centre value > B + dev + 4e-5 * G                                                                              (*)
//   with B >= every vertex's `bound` (the kernel's expression, evaluated with MX >= its mx and Amax >= its a: each float operation is
//   monotone in its positive operands; times 1.0001 for a compiler that contracts the two sites differently) and G >= the sum of the
//   magnitudes of every term that enters the value (so the rounding of the kernel's ~12 and of this function's ~10 float operations is
//   under 18 * 2^-24 * G = 1.1e-6 * G, 37 times less than the 4e-5 * G allowed for; an underflow adds under 1e-43, and (*) needs
//   G >= B >= 1e-6), every vertex is beyond that one plane by more than its bound. Then, at every vertex, the kernel finds the plane in
//   `cand` and either keeps it by the bound test or takes the exact margin, which the bound is above (the kernel's own invariant, checked by
//   tests/vgx_hotprof_dyncull.cpp): the plane's bit stays in `all` to the end, and the kernel answers true. A NaN or an infinite number in the
//   matrix makes every comparison below false (G, B < 1e30): "ask the kernel". The side planes only where the kernel uses them (c.sides).
//   tests/perf32_ptcl_pre.cpp checks the implication on random, on-the-edge and degenerate inputs, with a negative control.
struct QuadPre {
    float ap0, ap1, ap2, ap3, bx, by, MX, X, Y, Z;
    bool ok;
};
#ifndef PTCL_PRE_SLACK
#define PTCL_PRE_SLACK 4e-5f // (the test's negative control builds with a negative slack: the implication must then break)
#endif
// The loop's constants: of the clip volume and of the quad's vertices (n of them).
inline void QuadPreOf(QuadPre& q, const ClipNow& c, const float verts[8][3], uint32_t n) {
    float mx = 1.0f, ext[3] = {0.0f, 0.0f, 0.0f};
    bool fin = true;
    for (uint32_t i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) {
            const float a = std::fabs(verts[i][k]);
            if (!(a < 1e30f)) fin = false; // (a NaN too)
            if (a > mx) mx = a;
            if (a > ext[k]) ext[k] = a;
        }
    q.X = ext[0]; q.Y = ext[1]; q.Z = ext[2];
    q.ap0 = std::fabs(c.p[0]); q.ap1 = std::fabs(c.p[1]); q.ap2 = std::fabs(c.p[2]); q.ap3 = std::fabs(c.p[3]);
    q.bx = q.ap1 + std::fabs(c.kx);
    q.by = q.ap3 + std::fabs(c.ky);
    q.MX = mx;
    q.ok = fin && !c.ortho && c.pSum < 1e30f;
}
inline bool QuadPreOutside(const ClipNow& c, const QuadPre& q, const float (*m)[4]) {
    using F = ::fpb::Active; // (neonbox40: no vcmp / vmrs; port/fpbits.h)
    const float t0 = m[0][3], t1 = m[1][3], t2 = m[2][3];
    const float cx = c.p[0] * t0 + c.p[1] * t2, cy = c.p[2] * t1 + c.p[3] * t2, zx = c.kx * t2, zy = c.ky * t2;
    const float txc = cx + zx, nxc = zx - cx, tyc = cy + zy, nyc = zy - cy;
    // (a centre beyond no plane: a quad on the screen, the common case of the drawn ones: the kernel ends at its first vertex)
    if (!(F::GtZ(t2) || (c.sides && (F::GtZ(txc) || F::GtZ(nxc) || F::GtZ(tyc) || F::GtZ(nyc))))) return false;
    const float at0 = std::fabs(t0), at1 = std::fabs(t1), at2 = std::fabs(t2);
    const float a00 = std::fabs(m[0][0]), a01 = std::fabs(m[0][1]), a02 = std::fabs(m[0][2]);
    const float a10 = std::fabs(m[1][0]), a11 = std::fabs(m[1][1]), a12 = std::fabs(m[1][2]);
    const float a20 = std::fabs(m[2][0]), a21 = std::fabs(m[2][1]), a22 = std::fabs(m[2][2]);
    const float R = F::Max(F::Max(a00 + a01 + a02 + at0, a10 + a11 + a12 + at1), a20 + a21 + a22 + at2); // (the kernel's R: the same sums in the same order)
    const float D0 = a00 * q.X + a01 * q.Y + a02 * q.Z, D1 = a10 * q.X + a11 * q.Y + a12 * q.Z, D2 = a20 * q.X + a21 * q.Y + a22 * q.Z;
    const float S0 = D0 + at0, S1 = D1 + at1, S2 = D2 + at2;
    const float amax = (q.ap0 * S0 + q.ap1 * S2 + q.ap2 * S1 + q.ap3 * S2 + S2) * 1.0001f;
    const float B = (2.0f * ((1e-4f * (R * q.MX * HP_BOUND_FACTOR) + 1e-6f) * c.pSum + 1e-4f * amax) + 1e-6f) * 1.0001f;
    if (!F::Lt(B, 1e30f)) return false;
    if (F::Lt(S2, 1e30f) && F::Gt(t2, B + D2 + PTCL_PRE_SLACK * S2)) return true; // (the near plane, bit 16)
    if (!c.sides) return false;
    {
        const float dev = q.ap0 * D0 + q.bx * D2, G = dev + q.ap0 * at0 + q.bx * at2;
        if (F::Lt(G, 1e30f)) {
            const float lim = B + dev + PTCL_PRE_SLACK * G;
            if (F::Gt(txc, lim) || F::Gt(nxc, lim)) return true;
        }
    }
    {
        const float dev = q.ap2 * D1 + q.by * D2, G = dev + q.ap2 * at1 + q.by * at2;
        if (F::Lt(G, 1e30f)) {
            const float lim = B + dev + PTCL_PRE_SLACK * G;
            if (F::Gt(tyc, lim) || F::Gt(nyc, lim)) return true;
        }
    }
    return false;
}
} // namespace ptclcull
} // namespace port
