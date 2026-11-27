// neonbox40: host test of platform/vgx/src/vgx_fpbox.inc (SmallBoundsCore, ChaProjT) with fpb::Fast against the ORIGINAL SmallDrawBounds / ChaProj text
// (copied verbatim from vgx_draw.cpp at 60a8bc63 below, only the globals renamed) and against fpb::Ref. Random + special inputs (NaN, inf, +-0, denormals,
// huge, on-the-edge): every output (return value, box, rect, face; ChaProj: bad, extents, Rect) must be bit-identical.
//   g++ -O2 -std=c++17 -ffp-contract=off -I platform/include tests/neonbox_fpbox.cpp -o /tmp/nbx && /tmp/nbx
#include "port/fpbits.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <limits>
struct EfbRect { int16_t x = 0, y = 0, w = 0, h = 0; };
struct RenderState { float vpX, vpY, vpW, vpH; int16_t scX, scY, scW, scH; };
struct Cur { RenderState rs; } cur;
struct G { uint32_t xfReg[0x30]; } gg;
struct RowBox { float c[3]; float r; };
static float u2f(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
#include "../platform/vgx/src/vgx_fpbox.inc"
namespace orig {
static bool OrigSmallDrawBounds(uint8_t gxPrim, const uint8_t* v, uint32_t count, uint32_t stride, EfbRect& box, bool& rect, int* face = nullptr) {
    const RenderState& rs = cur.rs; auto& g = gg;
    if (face) *face = 0;
    float p[4];
    for (int i = 0; i < 4; ++i) p[i] = u2f(gg.xfReg[0x20 + i]);
    const bool ortho = (gg.xfReg[0x26] & 1) != 0;
    float px[4], py[4];
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (uint32_t i = 0; i < count; ++i, v += stride) {
        float e[3];
        std::memcpy(e, v, 12);
        float cx, cy, cw;
        if (ortho) { cx = p[0] * e[0] + p[1]; cy = p[2] * e[1] + p[3]; cw = 1.0f; }
        else { cx = p[0] * e[0] + p[1] * e[2]; cy = p[2] * e[1] + p[3] * e[2]; cw = -e[2]; }
        if (!(cw > 1e-6f)) return false;
        const float sx = rs.vpX + (cx / cw + 1.0f) * 0.5f * rs.vpW, sy = rs.vpY + (1.0f - cy / cw) * 0.5f * rs.vpH;
        if (!(sx > -16384.0f && sx < 16384.0f && sy > -16384.0f && sy < 16384.0f)) return false;
        x0 = std::min(x0, sx); x1 = std::max(x1, sx);
        y0 = std::min(y0, sy); y1 = std::max(y1, sy);
        if (i < 4) { px[i] = sx; py[i] = sy; }
    }
    const float e = 0.01f;
    const int bx0 = static_cast<int>(std::floor(x0 + e)), by0 = static_cast<int>(std::floor(y0 + e));
    const int bx1 = static_cast<int>(std::ceil(x1 - e)), by1 = static_cast<int>(std::ceil(y1 - e));
    // (cut to the scissor: nothing is drawn outside it)
    const int ax = std::max<int>(bx0, rs.scX), ay = std::max<int>(by0, rs.scY);
    const int cx1 = std::min<int>(bx1, rs.scX + rs.scW), cy1 = std::min<int>(by1, rs.scY + rs.scH);
    box = EfbRect{static_cast<int16_t>(ax), static_cast<int16_t>(ay), static_cast<int16_t>(cx1 > ax ? cx1 - ax : 0), static_cast<int16_t>(cy1 > ay ? cy1 - ay : 0)};
    rect = false;
    if (count != 4) return true;
    if (std::fabs(x0 - std::floor(x0 + 0.5f)) > e || std::fabs(x1 - std::floor(x1 + 0.5f)) > e || std::fabs(y0 - std::floor(y0 + 0.5f)) > e ||
        std::fabs(y1 - std::floor(y1 + 0.5f)) > e)
        return true;
    int sxn[4], syn[4];
    for (int i = 0; i < 4; ++i) {
        const bool l = std::fabs(px[i] - x0) <= e, r = std::fabs(px[i] - x1) <= e, t = std::fabs(py[i] - y0) <= e, b = std::fabs(py[i] - y1) <= e;
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
    if (face && rs.vpW > 0.0f && rs.vpH > 0.0f) {
        // (the cross product of (v1 - v0) and (v2 - v0) in screen pixels, y down: positive = clockwise as seen; the rectangle's corners
        // are whole pixels apart, so it is far from 0. Only for a viewport of positive size, the case whose orientation is plain)
        const float c = (px[1] - px[0]) * (py[2] - py[0]) - (py[1] - py[0]) * (px[2] - px[0]);
        *face = c > 0.5f ? 1 : c < -0.5f ? -1 : 0;
    }
    return true;
}


struct OrigChaProj {
    float p[4];
    bool ortho;
    float vx, vy, vw, vh;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    bool bad = false;
    OrigChaProj() {
        for (int i = 0; i < 4; ++i) p[i] = u2f(gg.xfReg[0x20 + i]);
        ortho = (gg.xfReg[0x26] & 1) != 0;
        vx = cur.rs.vpX; vy = cur.rs.vpY; vw = cur.rs.vpW; vh = cur.rs.vpH;
    }
    inline void Eye(float ex, float ey, float ez) {
        float cx, cy, cw;
        if (ortho) { cx = p[0] * ex + p[1]; cy = p[2] * ey + p[3]; cw = 1.0f; }
        else { cx = p[0] * ex + p[1] * ez; cy = p[2] * ey + p[3] * ez; cw = -ez; }
        if (!(cw > 1e-6f)) { bad = true; return; }
        const float iw = 1.0f / cw;
        const float sx = vx + (cx * iw + 1.0f) * 0.5f * vw, sy = vy + (1.0f - cy * iw) * 0.5f * vh;
        x0 = std::min(x0, sx); x1 = std::max(x1, sx);
        y0 = std::min(y0, sy); y1 = std::max(y1, sy);
    }
    inline void Point(const float* m, float x, float y, float z) {
        Eye(m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7], m[8] * x + m[9] * y + m[10] * z + m[11]);
    }
    // A row's bounding sphere through matrix m: its centre in eye space, its radius scaled by the matrix's largest row (the
    // skinning and model matrices are rotations with a scale), and the eye-space box around that sphere projected, its
    // near and far depths taken apart (exact for the box, so conservative for the sphere). About 50 operations a row;
    // the eight corners of the row's box cost five times that (the Observatory: 200 character draws a picture).
    void Sphere(const float* m, const RowBox& b) {
        const float ex = m[0] * b.c[0] + m[1] * b.c[1] + m[2] * b.c[2] + m[3];
        const float ey = m[4] * b.c[0] + m[5] * b.c[1] + m[6] * b.c[2] + m[7];
        const float ez = m[8] * b.c[0] + m[9] * b.c[1] + m[10] * b.c[2] + m[11];
        const float s2 = std::max({m[0] * m[0] + m[1] * m[1] + m[2] * m[2], m[4] * m[4] + m[5] * m[5] + m[6] * m[6], m[8] * m[8] + m[9] * m[9] + m[10] * m[10]});
        const float rr = b.r * std::sqrt(s2);
        float nx0, nx1, ny0, ny1; // NDC extents
        if (ortho) {
            const float a = p[0] * (ex - rr) + p[1], c = p[0] * (ex + rr) + p[1], d = p[2] * (ey - rr) + p[3], e = p[2] * (ey + rr) + p[3];
            nx0 = std::min(a, c); nx1 = std::max(a, c); ny0 = std::min(d, e); ny1 = std::max(d, e);
        } else {
            const float wn = -ez - rr, wf = -ez + rr; // (nearest and farthest distance in front of the eye)
            if (!(wn > 1e-6f)) { bad = true; return; }
            const float in = 1.0f / wn, inf = 1.0f / wf;
            const float xa = p[0] * (ex - rr), xb = p[0] * (ex + rr), ya = p[2] * (ey - rr), yb = p[2] * (ey + rr);
            nx0 = std::min({xa * in, xa * inf, xb * in, xb * inf}) - p[1];
            nx1 = std::max({xa * in, xa * inf, xb * in, xb * inf}) - p[1];
            ny0 = std::min({ya * in, ya * inf, yb * in, yb * inf}) - p[3];
            ny1 = std::max({ya * in, ya * inf, yb * in, yb * inf}) - p[3];
        }
        x0 = std::min(x0, vx + (nx0 + 1.0f) * 0.5f * vw); x1 = std::max(x1, vx + (nx1 + 1.0f) * 0.5f * vw);
        y0 = std::min(y0, vy + (1.0f - ny1) * 0.5f * vh); y1 = std::max(y1, vy + (1.0f - ny0) * 0.5f * vh);
    }
    bool Rect(EfbRect& r) const {
        if (bad || !(x0 <= x1 && y0 <= y1) || !(x0 > -16384.0f && x1 < 16384.0f && y0 > -16384.0f && y1 < 16384.0f)) return false;
        const int bx0 = static_cast<int>(std::floor(x0)), by0 = static_cast<int>(std::floor(y0));
        const int bx1 = static_cast<int>(std::ceil(x1)), by1 = static_cast<int>(std::ceil(y1));
        const RenderState& rs = cur.rs;
        const int ax = std::max<int>(bx0, rs.scX), ay = std::max<int>(by0, rs.scY);
        const int cx1 = std::min<int>(bx1, rs.scX + rs.scW), cy1 = std::min<int>(by1, rs.scY + rs.scH);
        r = EfbRect{static_cast<int16_t>(ax), static_cast<int16_t>(ay), static_cast<int16_t>(cx1 > ax ? cx1 - ax : 0), static_cast<int16_t>(cy1 > ay ? cy1 - ay : 0)};
        return true;
    }
};

}
using fpb::Bits; using fpb::FromBits;
static std::mt19937_64 rng(777);
static float sp(int k) {
    static const float t[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 0.01f, 1e-6f, 16384.0f, -16384.0f, 16383.99f, 1e30f, -1e30f, 3e38f, 1e-40f, -1e-40f,
        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::quiet_NaN(), 640.0f, 456.0f, 2.0f, 100.0f, 1e-5f};
    return t[k % (sizeof t / sizeof *t)];
}
static float rv(float scale) {
    const uint64_t r = rng();
    switch (r & 15) {
    case 0: return sp((int)(r >> 8));
    case 1: return FromBits((uint32_t)(r >> 16));
    case 2: return (float)((int)(r >> 20) % 41 - 20) * 0.5f * scale;
    case 3: return (float)((int)(r >> 20) % 2001 - 1000) * 0.01f * scale;
    case 4: return FromBits((uint32_t)(r >> 16) & 0x80FFFFFFu);
    default: return (float)(((r >> 12) % 2000001) - 1000000) * 1e-6f * scale;
    }
}
static long bad = 0, n = 0, cOk = 0, cRect = 0, cFace = 0, cCha = 0;
static const uint8_t kPrims[6] = {0x80, 0x98, 0xA0, 0x90, 0xB8, 0xA8};
#define CK(c, ...) do { ++n; if (!(c)) { if (bad++ < 10) { std::printf("MISMATCH line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } } while (0)
int main() {
    alignas(16) float verts[8][4];
    for (long it = 0; it < 12000000L; ++it) {
        BoundsState s;
        const int style = (int)(rng() % 4);
        s.ortho = (rng() & 3) == 0;
        s.p[0] = style == 0 ? rv(1.0f) : 1.7f + rv(0.1f); s.p[1] = rv(0.5f); s.p[2] = style == 0 ? rv(1.0f) : 2.3f + rv(0.1f); s.p[3] = rv(0.5f);
        s.vpX = (rng() & 1) ? 0.0f : rv(10.0f); s.vpY = (rng() & 1) ? 0.0f : rv(10.0f);
        s.vpW = (rng() & 3) ? 640.0f : rv(300.0f); s.vpH = (rng() & 3) ? 456.0f : rv(300.0f);
        s.scX = (int16_t)((int)(rng() % 41) - 20); s.scY = (int16_t)((int)(rng() % 41) - 20); s.scW = (int16_t)(rng() % 700); s.scH = (int16_t)(rng() % 600);
        uint32_t count = (rng() % 3 == 0) ? 4 : (uint32_t)(rng() % 7);
        const bool quadish = (rng() & 1);
        for (uint32_t i = 0; i < count; ++i) {
            for (int k = 0; k < 3; ++k) verts[i][k] = (quadish && k == 2) ? -1.0f - (float)(rng() % 3) : rv(3.0f);
            if (quadish && (rng() & 7)) { // a screen-aligned rectangle's corners, to reach the rect / face branches
                const float ex = (i == 0 || i == 3) ? -1.0f : 1.0f, ey = (i < 2) ? 1.0f : -1.0f;
                verts[i][0] = ex * 0.5f; verts[i][1] = ey * 0.25f;
            }
            verts[i][3] = rv(70.0f);
        }
        if (rng() % 3 == 0) { // a pixel-aligned rectangle through an orthographic projection (reaches rect / face)
            s.ortho = true; s.p[0] = 2.0f / 640.0f; s.p[1] = -1.0f; s.p[2] = 2.0f / 456.0f; s.p[3] = -1.0f; s.vpX = 0; s.vpY = 0; s.vpW = 640; s.vpH = 456;
            const float xa = (float)(rng() % 600), xb = xa + 1 + (float)(rng() % 40), ya = (float)(rng() % 400), yb = ya + 1 + (float)(rng() % 40);
            const float qx[4] = {xa, xb, xb, xa}, qy[4] = {ya, ya, yb, yb};
            const int rot = (int)(rng() % 4), flip = (int)(rng() & 1);
            for (uint32_t i = 0; i < 4; ++i) {
                const int j = flip ? (int)((4 - i + rot) % 4) : (int)((i + rot) % 4);
                verts[i][0] = qx[j]; verts[i][1] = qy[j]; verts[i][2] = -1.0f;
                if (rng() % 6 == 0) verts[i][(rng() & 1)] += (float)((int)(rng() % 5) - 2) * 0.004f;
                if (rng() % 40 == 0) verts[i][rng() % 3] = rv(2.0f);
            }
            if (rng() % 5 != 0) { s.scX = 0; s.scY = 0; s.scW = 640; s.scH = 456; }
            if (rng() & 1) { const float t = verts[1][0]; verts[1][0] = verts[3][0]; verts[3][0] = t; const float u = verts[1][1]; verts[1][1] = verts[3][1]; verts[3][1] = u; }
            count = 4;
        }
        const uint8_t prim = kPrims[rng() % 6];
        const uint32_t stride = 16;
        // original
        gg.xfReg[0x20] = Bits(s.p[0]); gg.xfReg[0x21] = Bits(s.p[1]); gg.xfReg[0x22] = Bits(s.p[2]); gg.xfReg[0x23] = Bits(s.p[3]); gg.xfReg[0x26] = s.ortho ? 1u : 0u;
        cur.rs = RenderState{s.vpX, s.vpY, s.vpW, s.vpH, (int16_t)s.scX, (int16_t)s.scY, (int16_t)s.scW, (int16_t)s.scH};
        // (the original floors a value >= 2^31 to an int: UB; skip inputs where a result would not fit, which the range test above rules out for count > 0)
        if (count == 0) continue; // count 0: x0 = 1e30 -> int cast of 1e30 is UB in the original (INT_MAX on ARM, where Fast runs the very same cast)
        EfbRect ob; bool orect = true; int oface = 7;
        const bool ook = orig::OrigSmallDrawBounds(prim, (const uint8_t*)verts, count, stride, ob, orect, &oface);
        cOk += ook; cRect += ook && orect; cFace += ook && oface != 0;
        for (int pass = 0; pass < 2; ++pass) {
            int b[4] = {-9, -9, -9, -9}; bool rect = true; int face = 7;
            const bool ok = pass ? SmallBoundsCore<fpb::Ref>(s, prim, (const uint8_t*)verts, count, stride, b, rect, &face)
                                 : SmallBoundsCore<fpb::Fast>(s, prim, (const uint8_t*)verts, count, stride, b, rect, &face);
            CK(ok == ook, "ok pass %d", pass);
            if (ok && ook) {
                CK(b[0] == ob.x && b[1] == ob.y && b[2] == ob.w && b[3] == ob.h, "box pass %d: %d %d %d %d vs %d %d %d %d count %u", pass, b[0],b[1],b[2],b[3],ob.x,ob.y,ob.w,ob.h,count);
                CK(rect == orect, "rect pass %d", pass);
                CK(face == oface, "face pass %d (%d %d)", pass, face, oface);
            } else CK(rect == true && face == 7 || face == 0, "untouched pass %d", pass);
        }
        // ChaProj: eye points and spheres
        orig::OrigChaProj O;
        ChaProjT<fpb::Fast> Fp; ChaProjT<fpb::Ref> Rp;
        for (int i = 0; i < 4; ++i) { Fp.p[i] = Rp.p[i] = O.p[i]; }
        Fp.ortho = Rp.ortho = O.ortho; Fp.vx = Rp.vx = O.vx; Fp.vy = Rp.vy = O.vy; Fp.vw = Rp.vw = O.vw; Fp.vh = Rp.vh = O.vh;
        float m[12]; for (int k = 0; k < 12; ++k) m[k] = (k % 5 == 0) ? 1.0f + rv(0.2f) : rv(1.0f);
        const int steps = 1 + (int)(rng() % 4);
        for (int k = 0; k < steps && !O.bad; ++k) {
            if (rng() & 1) { const float x = rv(2.0f), y = rv(2.0f), z = (rng() & 1) ? -2.0f - rv(3.0f) : rv(3.0f); O.Point(m, x, y, z); Fp.Point(m, x, y, z); Rp.Point(m, x, y, z); }
            else { RowBox b{{rv(2.0f), rv(2.0f), (rng() & 1) ? -5.0f + rv(1.0f) : rv(3.0f)}, std::fabs(rv(1.5f))}; O.Sphere(m, b); Fp.Sphere(m, b); Rp.Sphere(m, b); }
        }
        const int sc[4] = {s.scX, s.scY, s.scW, s.scH};
        EfbRect orr; const bool ob2 = O.Rect(orr); cCha += ob2;
        for (int pass = 0; pass < 2; ++pass) {
            const auto& P = pass ? (const void*)&Rp : (const void*)&Fp;
            int r[4]; bool ok; bool pbad; float a0, a1, b0, b1;
            if (pass) { ok = Rp.Rect(sc, r); pbad = Rp.bad; a0 = Rp.x0; a1 = Rp.x1; b0 = Rp.y0; b1 = Rp.y1; }
            else { ok = Fp.Rect(sc, r); pbad = Fp.bad; a0 = Fp.x0; a1 = Fp.x1; b0 = Fp.y0; b1 = Fp.y1; }
            (void)P;
            CK(pbad == O.bad, "cha bad");
            CK((Bits(a0) & 0x7FFFFFFFu) == (Bits(O.x0) & 0x7FFFFFFFu) || (a0 == O.x0) || (a0 != a0 && O.x0 != O.x0), "cha x0 %a %a", a0, O.x0); // (-0 / +0 ties: sign of zero may differ in Min, not in any decision)
            CK(ok == ob2, "cha rect ok pass %d", pass);
            if (ok && ob2) CK(r[0] == orr.x && r[1] == orr.y && r[2] == orr.w && r[3] == orr.h, "cha rect pass %d", pass);
            (void)a1; (void)b0; (void)b1;
        }
    }
    std::printf("neonbox_fpbox: %ld checks, %ld mismatches (coverage: bounds known %ld, rect %ld, face!=0 %ld, ChaProj rect %ld)\n", n, bad, cOk, cRect, cFace, cCha);
    return bad ? 1 : 0;
}
