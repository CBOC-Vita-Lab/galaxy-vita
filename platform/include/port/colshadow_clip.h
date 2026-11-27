// GXV_COLSHADOW_CLIP (dreadblend25): Mario's drop shadow (CollisionShadow, decomp MarioShadow.cpp) draws, for every ground triangle of its list, two passes through the
// shadow texture (a 160 x 160 A8 capture of Mario's silhouette, wrap CLAMP) with an alpha test "texture alpha != 0" and no depth test. The triangles are WHOLE collision
// triangles (a deck plate is far larger than the 300-unit box the texture covers), the texture's alpha is 0 outside the silhouette and on its outermost ring of texels, so
// the great part of every triangle is shaded (a texture fetch and a `discard`) and thrown away. On the Dreadnought that is 40 blended draws a picture with a static coverage
// of 1.34 screens (c72 dump, picture 6131) for a shadow that fills a few hundredths of a screen.
//
// What this header does: clips a convex polygon (position xyz + texture uv, uv a LINEAR function of the position) to the rectangle uv in [lo, hi]^2 (the texture's own
// extent plus a margin of one texel). A fragment outside the rectangle samples the CLAMPED edge texel, which is 0 (the capture's ring is never written: the silhouette is drawn
// inside a 1-pixel inset scissor and the circle that masks it multiplies the silhouette's alpha), so the alpha test throws it away in the original; leaving it out is the same
// picture. Inside the rectangle the polygon is the original one: a polygon wholly inside is returned UNCHANGED (the same vertices in the same order), a polygon wholly outside
// is dropped (no fragment of it could pass the test), and a polygon across the border is cut. The new vertices lie on the polygon's own plane to float rounding (a lerp of two
// of its vertices, uv exact on the border); two neighbours that share an edge cut it at the SAME point (the intersection is always computed from the endpoint with the smaller
// bit pattern, so the order the two polygons walk the edge in does not matter): the cut stays watertight.
// Pure (no GX, no globals): tests/colshadow_clip_test.cpp, tests/vgx_colshadow_clip_gl.cpp.
#pragma once
#include <cstdint>
#include <cstring>

namespace port {
namespace colshadow {

struct ClipVtx {
    float x, y, z, u, v;
};
constexpr int kClipMax = 8;       // a triangle cut by a rectangle has at most 3 + 4 vertices; a quad 4 + 4
constexpr float kClipPad = 1.0f / 160.0f; // one texel of the 160 x 160 shadow texture

namespace detail {
inline uint32_t Bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
// The point where the segment a..b crosses the line coordinate == bound (coord: 0 u, 1 v). The result does not depend on the order (a, b): it is always computed from the
// endpoint with the smaller (u, v) bit pattern towards the other, so the two polygons that share the edge get the very same float values.
inline ClipVtx Cross(const ClipVtx& a, const ClipVtx& b, int coord, float bound) {
    const float ca = coord ? a.v : a.u, cb = coord ? b.v : b.u;
    const bool swap = ca != cb ? Bits(ca) > Bits(cb) : (Bits(a.u) != Bits(b.u) ? Bits(a.u) > Bits(b.u) : Bits(a.v) > Bits(b.v)); // (a total order on the endpoints)
    const ClipVtx& p = swap ? b : a;
    const ClipVtx& q = swap ? a : b;
    const float cp = coord ? p.v : p.u, cq = coord ? q.v : q.u;
    const float t = (bound - cp) / (cq - cp);
    ClipVtx r;
    r.x = p.x + (q.x - p.x) * t;
    r.y = p.y + (q.y - p.y) * t;
    r.z = p.z + (q.z - p.z) * t;
    r.u = coord ? p.u + (q.u - p.u) * t : bound;
    r.v = coord ? bound : p.v + (q.v - p.v) * t;
    return r;
}
inline bool Inside(const ClipVtx& p, int coord, bool upper, float bound) {
    const float c = coord ? p.v : p.u;
    return upper ? c <= bound : c >= bound;
}
}  // namespace detail

// Clips the convex polygon in[0..n) (n 3 or 4, any winding) to lo <= u <= hi and lo <= v <= hi. out must hold kClipMax vertices. Returns the number of vertices written:
// 0 when the polygon lies wholly outside (or only touches the border from outside with no area), n with the input unchanged when it lies wholly inside.
inline int ClipUvBox(const ClipVtx* in, int n, float lo, float hi, ClipVtx* out) {
    if (n < 3 || n > 4) return 0;
    bool allIn = true;
    for (int i = 0; i < n; ++i)
        if (!(in[i].u >= lo && in[i].u <= hi && in[i].v >= lo && in[i].v <= hi)) allIn = false;
    if (allIn) {
        for (int i = 0; i < n; ++i) out[i] = in[i];
        return n;
    }
    // wholly outside one border: nothing of it can be inside
    {
        int outLo[2] = {0, 0}, outHi[2] = {0, 0};
        for (int i = 0; i < n; ++i) {
            outLo[0] += in[i].u < lo;
            outHi[0] += in[i].u > hi;
            outLo[1] += in[i].v < lo;
            outHi[1] += in[i].v > hi;
        }
        for (int c = 0; c < 2; ++c)
            if (outLo[c] == n || outHi[c] == n) return 0;
    }
    ClipVtx a[16], b[16];
    int na = n;
    for (int i = 0; i < n; ++i) a[i] = in[i];
    ClipVtx* src = a;
    ClipVtx* dst = b;
    for (int side = 0; side < 4 && na > 0; ++side) {
        const int coord = side >> 1;
        const bool upper = (side & 1) != 0;
        const float bound = upper ? hi : lo;
        int nb = 0;
        for (int i = 0; i < na; ++i) {
            const ClipVtx& cur = src[i];
            const ClipVtx& nxt = src[(i + 1) % na];
            const bool ci = detail::Inside(cur, coord, upper, bound), ni = detail::Inside(nxt, coord, upper, bound);
            if (ci) dst[nb++] = cur;
            if (ci != ni) dst[nb++] = detail::Cross(cur, nxt, coord, bound);
        }
        na = nb;
        ClipVtx* t = src;
        src = dst;
        dst = t;
    }
    if (na < 3) return 0;
    const int m = na > kClipMax ? kClipMax : na; // (a convex polygon cut by a rectangle never exceeds n + 4)
    for (int i = 0; i < m; ++i) out[i] = src[i];
    return m;
}

// Area of the polygon in uv (shoelace, absolute), for the census.
inline double UvArea(const ClipVtx* p, int n) {
    double s = 0;
    for (int i = 0; i < n; ++i) {
        const ClipVtx& a = p[i];
        const ClipVtx& b = p[(i + 1) % n];
        s += static_cast<double>(a.u) * b.v - static_cast<double>(b.u) * a.v;
    }
    return s < 0 ? -0.5 * s : 0.5 * s;
}

// What the census counts (game thread only).
struct ClipCensus {
    uint64_t pictures = 0, polygons = 0, outside = 0, cut = 0, inside = 0, trisIn = 0, trisOut = 0;
    double areaIn = 0, areaOut = 0;
};

}  // namespace colshadow
}  // namespace port
