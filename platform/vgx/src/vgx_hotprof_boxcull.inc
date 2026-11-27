// GXV_HOTPROF_BOXCULL (hotprof25 round 2): the box test of the static and cluster culls (vgx_draw.cpp BoxOutMask). BoxOutMaskOld is the
// test as it was (moved here unchanged so that tests/vgx_hotprof_boxcull.cpp runs the very code); HpBoxOutMask gives the same mask sooner.
// Included inside vgx_draw.cpp's namespace, after ClipNow and RowBox; the test includes it after its own copies of the two structs.
inline uint32_t BoxOutMaskOld(const ClipNow& c, const float* const m[3], const RowBox& b) {
    using F = ::fpb::Active; // (neonbox40: compares without vcmp / vmrs, port/fpbits.h; BoxOutMaskOld is still the same function of its inputs)
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
        if (F::Le(std::fabs(cx), cw) & F::Le(std::fabs(cy), cw)) return 0;
    }
    // The eye-space error bound: the magnitudes a row sums, for the largest coordinates of the box.
    const float ax = F::Max(std::fabs(b.mn[0]), std::fabs(b.mx[0])), ay = F::Max(std::fabs(b.mn[1]), std::fabs(b.mx[1])),
                az = F::Max(std::fabs(b.mn[2]), std::fabs(b.mx[2]));
    float erow = 0.0f;
    for (int r = 0; r < 3; ++r)
        erow = F::Max(erow, std::fabs(m[r][0]) * ax + std::fabs(m[r][1]) * ay + std::fabs(m[r][2]) * az + std::fabs(m[r][3]));
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
        mag = F::Max(mag, a);
    }
    const float margin = 2.0f * (eEye * c.pSum + 1e-4f * mag) + 1e-6f;
    if (!F::Lt(margin, 1e30f)) return 0; // (infinite or NaN: nothing is known)
    // ("outside by more than the margin" at all eight corners; a NaN corner fails the test)
    bool right = c.sides, left = c.sides, top = c.sides, bottom = c.sides, behind = !ortho;
    for (int k = 0; k < 8; ++k) {
        right = right && F::Gt(cx[k] - c.kx * cw[k], margin);
        left = left && F::Gt(-cx[k] - c.kx * cw[k], margin);
        top = top && F::Gt(cy[k] - c.ky * cw[k], margin);
        bottom = bottom && F::Gt(-cy[k] - c.ky * cw[k], margin);
        behind = behind && F::Gt(-cw[k], margin);
    }
    return (right ? 1u : 0u) | (left ? 2u : 0u) | (top ? 4u : 0u) | (bottom ? 8u : 0u) | (behind ? 16u : 0u);
}
// The same mask. The margin is positive whenever it is finite (2 (eEye pSum + 1e-4 mag) + 1e-6, every term >= 0), and a box whose margin
// is not finite has mask 0. So a plane that some corner is not beyond at margin 0 (the very expression compared, a NaN compares false) is
// not in the mask whatever the margin is; once no plane is left the mask is 0 and the remaining corners, the magnitudes and the margin
// are not needed. (A box that straddles the volume's edge with its centre outside ends after two to four corners.) Otherwise the margin
// is computed as before and the planes still in question are tested against it as before.
inline uint32_t HpBoxOutMask(const ClipNow& c, const float* const m[3], const RowBox& b) {
    using F = ::fpb::Active;
    const float* const p = c.p;
    const bool ortho = c.ortho;
    {
        const float x = 0.5f * (b.mn[0] + b.mx[0]), y = 0.5f * (b.mn[1] + b.mx[1]), z = 0.5f * (b.mn[2] + b.mx[2]);
        const float ex = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
        const float ey = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
        const float ez = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
        const float cx = ortho ? p[0] * ex + p[1] : p[0] * ex + p[1] * ez, cy = ortho ? p[2] * ey + p[3] : p[2] * ey + p[3] * ez;
        const float cw = ortho ? 1.0f : -ez;
        if (F::Le(std::fabs(cx), cw) & F::Le(std::fabs(cy), cw)) return 0;
    }
    // (the margin's sign argument needs pSum >= 0, which ClipNowOfCalc's sum of magnitudes + 1 always is unless it is a NaN; anything
    // else goes the old way)
    if (!F::Ge(c.pSum, 0.0f)) return BoxOutMaskOld(c, m, b);
    float tx[8], nx[8], ty[8], ny[8], nw[8], mag = 0.0f;
#if defined(HP_BOX_NEG)
    const float hpZero = HP_BOX_NEG; // (the test's negative control: "margin 0" above the smallest margin: the test must notice)
#else
    const float hpZero = 0.0f;
#endif
    uint32_t cand = (c.sides ? 15u : 0u) | (ortho ? 0u : 16u);
    for (int k = 0; k < 8; ++k) {
        const float x = (k & 1) ? b.mx[0] : b.mn[0], y = (k & 2) ? b.mx[1] : b.mn[1], z = (k & 4) ? b.mx[2] : b.mn[2];
        const float ex = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
        const float ey = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
        const float ez = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
        float cx, cy, cw;
        if (ortho) {
            cx = p[0] * ex + p[1]; cy = p[2] * ey + p[3]; cw = 1.0f;
        } else {
            cx = p[0] * ex + p[1] * ez; cy = p[2] * ey + p[3] * ez; cw = -ez;
        }
        // (the five numbers the old loop compares with the margin, in its very expressions)
        tx[k] = cx - c.kx * cw; nx[k] = -cx - c.kx * cw; ty[k] = cy - c.ky * cw; ny[k] = -cy - c.ky * cw; nw[k] = -cw;
        cand &= (F::Gt(tx[k], hpZero) ? 1u : 0u) | (F::Gt(nx[k], hpZero) ? 2u : 0u) | (F::Gt(ty[k], hpZero) ? 4u : 0u) | (F::Gt(ny[k], hpZero) ? 8u : 0u) | (F::Gt(nw[k], hpZero) ? 16u : 0u);
        if (cand == 0) return 0;
        float a;
        if (ortho) a = std::fabs(p[0] * ex) + std::fabs(p[1]) + std::fabs(p[2] * ey) + std::fabs(p[3]) + 1.0f;
        else a = std::fabs(p[0] * ex) + std::fabs(p[1] * ez) + std::fabs(p[2] * ey) + std::fabs(p[3] * ez) + std::fabs(ez);
        mag = F::Max(mag, a);
    }
    const float ax = F::Max(std::fabs(b.mn[0]), std::fabs(b.mx[0])), ay = F::Max(std::fabs(b.mn[1]), std::fabs(b.mx[1])),
                az = F::Max(std::fabs(b.mn[2]), std::fabs(b.mx[2]));
    float erow = 0.0f;
    for (int r = 0; r < 3; ++r)
        erow = F::Max(erow, std::fabs(m[r][0]) * ax + std::fabs(m[r][1]) * ay + std::fabs(m[r][2]) * az + std::fabs(m[r][3]));
    const float eEye = 1e-4f * erow + 1e-6f;
    const float margin = 2.0f * (eEye * c.pSum + 1e-4f * mag) + 1e-6f;
    if (!F::Lt(margin, 1e30f)) return 0; // (infinite or NaN: nothing is known)
    // (only the planes still in question: "beyond by more than the margin" at all eight corners)
    const float* const val[5] = {tx, nx, ty, ny, nw};
    uint32_t mask = cand;
    for (int pl = 0; pl < 5; ++pl) {
        if (!(cand >> pl & 1u)) continue;
        const float* const v = val[pl];
        for (int k = 0; k < 8; ++k)
            if (!F::Gt(v[k], margin)) { mask &= ~(1u << pl); break; }
    }
    return mask;
}
