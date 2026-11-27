// cpu24: the decision core of shapecull::Out (decomp/src/Game/System/DrawBuffer.cpp), in a header so that a host test can compare it with the
// original (Ref) over random boxes. `Fast` gives the same answer as `Ref` for every input the caller can pass: it drops only
//   (a) the products by the literal 0.0f of the sparse planes (0 * finite = +-0; added to a value, or fabs()'d, it changes nothing), plane 4's
//       t = -m[2][i] exactly (fabs: |m[2][i]|). 0 * infinity is NaN in the original, which makes that plane "not outside": so when Fast finds
//       a plane it checks that the 12 matrix entries and v are finite, and otherwise answers with RefPlane. (When some entry is not finite
//       and Fast finds nothing, Ref finds nothing too: a plane Ref finds has no NaN in its own terms, where Fast computes the very same
//       values, and Fast reaches it unless an earlier plane fires, which is "found" as well.)
//   (b) the work for `mag` (and `ch`, |c| + h made once per box) when it cannot matter: the test is r + d < -(1e-4 * mag + 1e-3) and
//       1e-4f * mag + 1e-3f >= 1e-3f for every mag >= 0, so r + d >= -1e-3f is "not outside" whatever mag is (a NaN mag makes the test false
//       too: the prefilter's answer, "not outside", is the same).
// Same float operations in the same order for what is kept, so no rounding differs (scalar: NEON would flush denormals, see fpbits.h).
// The plane that fires first (k = 0..4) is reported as before (it is only used by the verify mode).
#pragma once
#include <cmath>
#include "port/fpbits.h"
namespace shapecull_math {
typedef float f32;
// m: 3 rows of 4 (the draw matrix), c, h: box centre and half extents, ch: |c| + h per axis, pj: GXGetProjectionv's 7 floats, g: margin factor.
// v: the centre through m (already checked finite). Returns the plane (0..4) the box is outside of, or -1.
inline int RefPlane(const f32 (*m)[4], const f32* c, const f32* h, const f32* v, const f32* pj, f32 g) {
    const f32 pl[5][3] = {
        {pj[1], 0.0f, pj[2] - g},
        {-pj[1], 0.0f, -pj[2] - g},
        {0.0f, pj[3], pj[4] - g},
        {0.0f, -pj[3], -pj[4] - g},
        {0.0f, 0.0f, -1.0f},
    };
    for (int k = 0; k < 5; ++k) {
        const f32 a = pl[k][0], b = pl[k][1], cz = pl[k][2];
        f32 r = 0.0f, mag = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const f32 t = a * m[0][i] + b * m[1][i] + cz * m[2][i];
            const f32 at = std::fabs(t);
            r += at * h[i];
            const f32 ci = std::fabs(c[i]);
            mag += at * (ci + h[i]);
        }
        const f32 tr = a * m[0][3] + b * m[1][3] + cz * m[2][3];
        mag += std::fabs(tr);
        const f32 d = a * v[0] + b * v[1] + cz * v[2];
        if (fpb::Active::Lt(d + r, -(1.0e-4f * mag + 1.0e-3f))) return k;
    }
    return -1;
}
inline int FastPlane(const f32 (*m)[4], const f32* c, const f32* h, const f32* ch, const f32* v, const f32* pj, f32 g) {
    // plane k: t_i = a*m0i + b*m1i + cz*m2i, with the zero products left out
    f32 a, b, cz;
    for (int k = 0; k < 5; ++k) {
        f32 t[3], tr;
        if (k == 0 || k == 1) {
            a = k == 0 ? pj[1] : -pj[1];
            cz = k == 0 ? pj[2] - g : -pj[2] - g;
            b = 0.0f;
            for (int i = 0; i < 3; ++i) t[i] = a * m[0][i] + cz * m[2][i];
            tr = a * m[0][3] + cz * m[2][3];
        } else if (k == 2 || k == 3) {
            b = k == 2 ? pj[3] : -pj[3];
            cz = k == 2 ? pj[4] - g : -pj[4] - g;
            a = 0.0f;
            for (int i = 0; i < 3; ++i) t[i] = b * m[1][i] + cz * m[2][i];
            tr = b * m[1][3] + cz * m[2][3];
        } else {
            a = 0.0f; b = 0.0f; cz = -1.0f;
            for (int i = 0; i < 3; ++i) t[i] = -m[2][i];
            tr = -m[2][3];
        }
        const f32 a0 = std::fabs(t[0]), a1 = std::fabs(t[1]), a2 = std::fabs(t[2]);
        f32 r = a0 * h[0];
        r += a1 * h[1];
        r += a2 * h[2];
        // d: a*v0 + b*v1 + cz*v2 with the zero product dropped (v is finite)
        const f32 d = k < 2 ? a * v[0] + cz * v[2] : k < 4 ? b * v[1] + cz * v[2] : -v[2];
        const f32 s = d + r;
        if (!fpb::Active::Lt(s, -1.0e-3f)) continue;  // (not outside whatever mag is)
        f32 mag = a0 * ch[0];
        mag += a1 * ch[1];
        mag += a2 * ch[2];
        mag += std::fabs(tr);
        if (fpb::Active::Lt(s, -(1.0e-4f * mag + 1.0e-3f))) {
            uint32_t bad = 0;
            for (int i = 0; i < 3; ++i) {
                const f32 x[4] = {m[i][0], m[i][1], m[i][2], m[i][3]};
                for (int j = 0; j < 4; ++j) bad |= (fpb::Bits(x[j]) & 0x7F800000u) == 0x7F800000u;
                bad |= (fpb::Bits(v[i]) & 0x7F800000u) == 0x7F800000u;
            }
            return __builtin_expect(bad == 0, 1) ? k : RefPlane(m, c, h, v, pj, g);
        }
    }
    return -1;
}
}  // namespace shapecull_math
