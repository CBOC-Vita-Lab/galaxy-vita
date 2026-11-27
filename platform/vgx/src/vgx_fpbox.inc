// neonbox40 (21 Nov 2026): the front end's screen-bounds arithmetic (SmallDrawBounds, ChaProj of CharaNote) with its float compares, min / max and
// floor / ceil done by fpb::F (platform/include/port/fpbits.h): F = fpb::Ref is the code as it was, F = fpb::Fast the same decisions with no vcmp /
// vmrs APSR_nzcv (FP-AUDIT.md: 148 + 102 of them in these two, 85 M and 116 M calls on the console) and no floorf / ceilf call. The code is a template
// over F and takes the state it reads as plain arguments so that tests/neonbox_fpbox.cpp runs the very text against Ref. Included inside vgx_draw.cpp's
// namespace (after u2f / RowBox) and by the test after its own `Rs`-free definitions; needs <algorithm>, <cmath>, <cstring>, "port/fpbits.h".
struct BoundsState {       // what the two read of g / cur.rs
    float p[4];
    bool ortho;
    float vpX, vpY, vpW, vpH;
    int scX, scY, scW, scH;
};
// SmallDrawBounds' body. box[4] = x, y, w, h (the caller stores them as int16s, as before); false: not known.
template <class F>
inline bool SmallBoundsCore(const BoundsState& s, uint8_t gxPrim, const uint8_t* v, uint32_t count, uint32_t stride, int box[4], bool& rect, int* face) {
    if (face) *face = 0;
    const float* const p = s.p;
    const bool ortho = s.ortho;
    float px[4], py[4];
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (uint32_t i = 0; i < count; ++i, v += stride) {
        float e[3];
        std::memcpy(e, v, 12);
        float cx, cy, cw;
        if (ortho) { cx = p[0] * e[0] + p[1]; cy = p[2] * e[1] + p[3]; cw = 1.0f; }
        else { cx = p[0] * e[0] + p[1] * e[2]; cy = p[2] * e[1] + p[3] * e[2]; cw = -e[2]; }
        if (!F::Gt(cw, 1e-6f)) return false;
        const float sx = s.vpX + (cx / cw + 1.0f) * 0.5f * s.vpW, sy = s.vpY + (1.0f - cy / cw) * 0.5f * s.vpH;
        if (!(F::Gt(sx, -16384.0f) & F::Lt(sx, 16384.0f) & F::Gt(sy, -16384.0f) & F::Lt(sy, 16384.0f))) return false;
        x0 = F::Min(x0, sx); x1 = F::Max(x1, sx);
        y0 = F::Min(y0, sy); y1 = F::Max(y1, sy);
        if (i < 4) { px[i] = sx; py[i] = sy; }
    }
    const float e = 0.01f;
    const int bx0 = F::FloorI(x0 + e), by0 = F::FloorI(y0 + e);
    const int bx1 = F::CeilI(x1 - e), by1 = F::CeilI(y1 - e);
    // (cut to the scissor: nothing is drawn outside it)
    const int ax = std::max<int>(bx0, s.scX), ay = std::max<int>(by0, s.scY);
    const int cx1 = std::min<int>(bx1, s.scX + s.scW), cy1 = std::min<int>(by1, s.scY + s.scH);
    box[0] = static_cast<int16_t>(ax); box[1] = static_cast<int16_t>(ay);
    box[2] = static_cast<int16_t>(cx1 > ax ? cx1 - ax : 0); box[3] = static_cast<int16_t>(cy1 > ay ? cy1 - ay : 0);
    rect = false;
    if (count != 4) return true;
    // (count 4: every coordinate passed the range test above, so |x0 + 0.5| < 16385 and the int floor is exact; the float floor of the original
    // is that integer, converted back)
    const float fx0 = static_cast<float>(F::FloorI(x0 + 0.5f)), fx1 = static_cast<float>(F::FloorI(x1 + 0.5f));
    const float fy0 = static_cast<float>(F::FloorI(y0 + 0.5f)), fy1 = static_cast<float>(F::FloorI(y1 + 0.5f));
    if (F::Gt(std::fabs(x0 - fx0), e) | F::Gt(std::fabs(x1 - fx1), e) | F::Gt(std::fabs(y0 - fy0), e) | F::Gt(std::fabs(y1 - fy1), e)) return true;
    int sxn[4], syn[4];
    for (int i = 0; i < 4; ++i) {
        const bool l = F::Le(std::fabs(px[i] - x0), e), r = F::Le(std::fabs(px[i] - x1), e), t = F::Le(std::fabs(py[i] - y0), e), b = F::Le(std::fabs(py[i] - y1), e);
        if (!(l || r) || !(t || b) || (l && r) || (t && b)) return true;
        sxn[i] = r ? 1 : -1;
        syn[i] = b ? 1 : -1;
    }
    const uint8_t kind = gxPrim & 0xF8;
    int d1, o0, o1; // vertex 0's diagonal partner, and the other pair
    if (kind == 0x80 || kind == 0xA0) { d1 = 2; o0 = 1; o1 = 3; }      // quads, fan
    else if (kind == 0x98) { d1 = 3; o0 = 1; o1 = 2; }                 // strip
    else return true;
    if (sxn[0] != -sxn[d1] || syn[0] != -syn[d1] || sxn[o0] != -sxn[o1] || syn[o0] != -syn[o1]) return true;
    if (sxn[0] * syn[0] == sxn[o0] * syn[o0]) return true; // (the two pairs are the two diagonals)
    rect = true;
    if (face && F::GtZ(s.vpW) && F::GtZ(s.vpH)) {
        const float c = (px[1] - px[0]) * (py[2] - py[0]) - (py[1] - py[0]) * (px[2] - px[0]);
        *face = F::Gt(c, 0.5f) ? 1 : F::Lt(c, -0.5f) ? -1 : 0;
    }
    return true;
}

// The screen rectangle of points given in model space (CharaNote; see ChaProj's use in vgx_draw.cpp). B: a box with c[3] and r (RowBox).
template <class F>
struct ChaProjT {
    float p[4];
    bool ortho;
    float vx, vy, vw, vh;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    bool bad = false;
    inline void Eye(float ex, float ey, float ez) {
        float cx, cy, cw;
        if (ortho) { cx = p[0] * ex + p[1]; cy = p[2] * ey + p[3]; cw = 1.0f; }
        else { cx = p[0] * ex + p[1] * ez; cy = p[2] * ey + p[3] * ez; cw = -ez; }
        if (!F::Gt(cw, 1e-6f)) { bad = true; return; }
        const float iw = 1.0f / cw;
        const float sx = vx + (cx * iw + 1.0f) * 0.5f * vw, sy = vy + (1.0f - cy * iw) * 0.5f * vh;
        x0 = F::Min(x0, sx); x1 = F::Max(x1, sx);
        y0 = F::Min(y0, sy); y1 = F::Max(y1, sy);
    }
    inline void Point(const float* m, float x, float y, float z) {
        Eye(m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7], m[8] * x + m[9] * y + m[10] * z + m[11]);
    }
    template <class B>
    void Sphere(const float* m, const B& b) {
        const float ex = m[0] * b.c[0] + m[1] * b.c[1] + m[2] * b.c[2] + m[3];
        const float ey = m[4] * b.c[0] + m[5] * b.c[1] + m[6] * b.c[2] + m[7];
        const float ez = m[8] * b.c[0] + m[9] * b.c[1] + m[10] * b.c[2] + m[11];
        const float s2 = F::Max(F::Max(m[0] * m[0] + m[1] * m[1] + m[2] * m[2], m[4] * m[4] + m[5] * m[5] + m[6] * m[6]), m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
        const float rr = b.r * std::sqrt(s2);
        float nx0, nx1, ny0, ny1; // NDC extents
        if (ortho) {
            const float a = p[0] * (ex - rr) + p[1], c = p[0] * (ex + rr) + p[1], d = p[2] * (ey - rr) + p[3], e = p[2] * (ey + rr) + p[3];
            nx0 = F::Min(a, c); nx1 = F::Max(a, c); ny0 = F::Min(d, e); ny1 = F::Max(d, e);
        } else {
            const float wn = -ez - rr, wf = -ez + rr; // (nearest and farthest distance in front of the eye)
            if (!F::Gt(wn, 1e-6f)) { bad = true; return; }
            const float in = 1.0f / wn, inf = 1.0f / wf;
            const float xa = p[0] * (ex - rr), xb = p[0] * (ex + rr), ya = p[2] * (ey - rr), yb = p[2] * (ey + rr);
            // (std::min / std::max of an initializer list: the first, replaced by each later one that is less / greater)
            nx0 = F::Min(F::Min(F::Min(xa * in, xa * inf), xb * in), xb * inf) - p[1];
            nx1 = F::Max(F::Max(F::Max(xa * in, xa * inf), xb * in), xb * inf) - p[1];
            ny0 = F::Min(F::Min(F::Min(ya * in, ya * inf), yb * in), yb * inf) - p[3];
            ny1 = F::Max(F::Max(F::Max(ya * in, ya * inf), yb * in), yb * inf) - p[3];
        }
        x0 = F::Min(x0, vx + (nx0 + 1.0f) * 0.5f * vw); x1 = F::Max(x1, vx + (nx1 + 1.0f) * 0.5f * vw);
        y0 = F::Min(y0, vy + (1.0f - ny1) * 0.5f * vh); y1 = F::Max(y1, vy + (1.0f - ny0) * 0.5f * vh);
    }
    // r[4] = x, y, w, h cut to the scissor sc[4] = x, y, w, h.
    bool Rect(const int sc[4], int r[4]) const {
        if (bad || !(F::Le(x0, x1) & F::Le(y0, y1)) || !(F::Gt(x0, -16384.0f) & F::Lt(x1, 16384.0f) & F::Gt(y0, -16384.0f) & F::Lt(y1, 16384.0f))) return false;
        const int bx0 = F::FloorI(x0), by0 = F::FloorI(y0);
        const int bx1 = F::CeilI(x1), by1 = F::CeilI(y1);
        const int ax = std::max<int>(bx0, sc[0]), ay = std::max<int>(by0, sc[1]);
        const int cx1 = std::min<int>(bx1, sc[0] + sc[2]), cy1 = std::min<int>(by1, sc[1] + sc[3]);
        r[0] = static_cast<int16_t>(ax); r[1] = static_cast<int16_t>(ay);
        r[2] = static_cast<int16_t>(cx1 > ax ? cx1 - ax : 0); r[3] = static_cast<int16_t>(cy1 > ay ? cy1 - ay : 0);
        return true;
    }
};
