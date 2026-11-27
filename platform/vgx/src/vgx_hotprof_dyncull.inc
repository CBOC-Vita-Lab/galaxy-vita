// GXV_HOTPROF_DYNCULL (hotprof25; vgx_draw.cpp DynDrawOutsideFast has the reasons): GXV_DYNCULL's test of a rigid draw in a perspective
// projection: whether every one of the `count` model-space points at `v` (three floats each, `stride` bytes apart), through the position
// rows m, is beyond one and the same plane of the clip volume c by more than its margin. The answer is the one the loop over PointOutMask
// gives (tests/vgx_hotprof_dyncull.cpp checks it against that loop). Included inside vgx_draw.cpp's namespace, after ClipNow; the test
// includes it after its own copy of that struct.
#ifndef HP_BOUND_FACTOR
#define HP_BOUND_FACTOR 1.00001f // (the test's second negative control builds with a factor below 1: the bound is then no bound)
#endif
inline bool HpRigidOutside(const ClipNow& c, const float* const m[3], const uint8_t* v, uint32_t count, uint32_t stride) {
    using F = ::fpb::Active; // (neonbox40: the compares without vcmp / vmrs, port/fpbits.h; exact for every input)
    uint32_t all = 31u;
    // The matrix rows' absolute values and the largest absolute row sum, once.
    float am[3][4], R = 0.0f;
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 4; ++k) am[r][k] = std::fabs(m[r][k]);
        R = F::Max(R, am[r][0] + am[r][1] + am[r][2] + am[r][3]);
    }
    const uint32_t sideBits = c.sides ? 15u : 0u;
    for (uint32_t i = 0; i < count; ++i, v += stride) {
        float q[3];
        std::memcpy(q, v, sizeof q);
        const float x = q[0], y = q[1], z = q[2];
        // (PointOutMask's expressions, a perspective projection)
        const float* const p = c.p;
        const float ex = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
        const float ey = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
        const float ez = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
        const float cx = p[0] * ex + p[1] * ez, cy = p[2] * ey + p[3] * ez, cw = -ez;
        const float tx = cx - c.kx * cw, nx = -cx - c.kx * cw, ty = cy - c.ky * cw, ny = -cy - c.ky * cw;
        // (the planes the point is beyond at margin 0; a NaN is beyond none)
        const uint32_t m0 = (((F::GtZ(tx) ? 1u : 0u) | (F::GtZ(nx) ? 2u : 0u) | (F::GtZ(ty) ? 4u : 0u) | (F::GtZ(ny) ? 8u : 0u)) & sideBits) | (F::GtZ(-cw) ? 16u : 0u);
        const uint32_t cand = all & m0;
        if (cand == 0) return false;
        const float a = std::fabs(p[0] * ex) + std::fabs(p[1] * ez) + std::fabs(p[2] * ey) + std::fabs(p[3] * ez) + std::fabs(ez);
        const float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
        {
            // (the bound: beyond every plane in question by more than it, the exact margin changes nothing)
            const float mx = std::max(std::max(std::max(1.0f, ax), ay), az);
            const float bound = 2.0f * ((1e-4f * (R * mx * HP_BOUND_FACTOR) + 1e-6f) * c.pSum + 1e-4f * a) + 1e-6f;
            if (F::Lt(bound, 1e30f) &&
                (!(cand & 1u) || F::Gt(tx, bound)) && (!(cand & 2u) || F::Gt(nx, bound)) && (!(cand & 4u) || F::Gt(ty, bound)) && (!(cand & 8u) || F::Gt(ny, bound)) &&
                (!(cand & 16u) || F::Gt(-cw, bound))) {
                all = cand;
                continue;
            }
        }
        float erow = 0.0f;
        for (int r = 0; r < 3; ++r) erow = F::Max(erow, am[r][0] * ax + am[r][1] * ay + am[r][2] * az + am[r][3]);
        const float margin = 2.0f * ((1e-4f * erow + 1e-6f) * c.pSum + 1e-4f * a) + 1e-6f;
        if (!F::Lt(margin, 1e30f)) return false;
        uint32_t o = 0;
        if (c.sides) {
            if (F::Gt(tx, margin)) o |= 1u;
            if (F::Gt(nx, margin)) o |= 2u;
            if (F::Gt(ty, margin)) o |= 4u;
            if (F::Gt(ny, margin)) o |= 8u;
        }
        if (F::Gt(-cw, margin)) o |= 16u;
        all &= o;
        if (all == 0) return false;
    }
    return all != 0;
}
