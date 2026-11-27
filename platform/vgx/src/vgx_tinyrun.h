// GXV_TINYRUN (dread25): static runs that cover almost nothing are left out. Pure helpers (tests/vgx_tinyrun_test.cpp runs this very file).
//
// A static run (one J3D shape / material of a cached list, rigid: one position matrix) has a model-space box. Through the position matrix and the
// projection the box's eight corners give a rectangle in normalised device coordinates that contains every triangle of the run (the projection of
// a convex set is the hull of its corners' projections, and a perspective divide of points in front of the eye keeps convexity). The run cannot
// cover more pixels than the rectangle's area. A run whose rectangle is under a few pixels of the viewport leaves, at most, a speck; leaving it
// out saves its draw (the tiler's ~17 us, the front end's 25-40 us). Not decided here: whether to do it (the switch) or how large "a few" is
// (GXV_TINYRUN_PX, in viewport pixels of the GX picture: the game's 3D view is 640x456, the screen at 960x544 shows each of them 1.5x larger).
//
// ProjectedAreaPx answers -1 ("not known: keep the run") when a corner is at or behind the eye (the rectangle then is not an upper bound), when a
// number is not finite, or when the viewport is not known. The rectangle is NOT cut to the view: a tiny box far outside the view is as tiny as one
// inside it (the plain cull, GXV_STATICCULL, decides what is outside; this one never makes a run vanish that it has not measured).
#pragma once
#include <cmath>

namespace vgx {
namespace tinyrun {

// p: the projection's six numbers as the front end keeps them (xfReg 0x20..0x25: p0 p1 p2 p3 p4 p5; this uses p0..p3 as BoxScreenFrac does),
// ortho: the projection is orthographic, m: the three rows of the position matrix (3 x 4), mn / mx: the model-space box.
// vpW / vpH: the viewport's width and height in pixels (2 * xfReg[0x1A], -2 * xfReg[0x1B]).
inline float ProjectedAreaPx(const float p[6], bool ortho, const float* const m[3], const float mn[3], const float mx[3], float vpW, float vpH) {
    if (!(vpW > 0.0f && vpH > 0.0f && vpW < 1e5f && vpH < 1e5f)) return -1.0f;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int k = 0; k < 8; ++k) {
        const float x = (k & 1) ? mx[0] : mn[0], y = (k & 2) ? mx[1] : mn[1], z = (k & 4) ? mx[2] : mn[2];
        const float ex = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
        const float ey = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
        const float ez = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
        float cx, cy, cw;
        if (ortho) { cx = p[0] * ex + p[1]; cy = p[2] * ey + p[3]; cw = 1.0f; }
        else { cx = p[0] * ex + p[1] * ez; cy = p[2] * ey + p[3] * ez; cw = -ez; }
        if (!(cw > 1e-4f)) return -1.0f;
        const float nx = cx / cw, ny = cy / cw;
        if (!std::isfinite(nx) || !std::isfinite(ny)) return -1.0f;
        if (nx < x0) x0 = nx;
        if (nx > x1) x1 = nx;
        if (ny < y0) y0 = ny;
        if (ny > y1) y1 = ny;
    }
    const float w = x1 - x0, h = y1 - y0;
    if (!(w >= 0.0f && h >= 0.0f)) return -1.0f;
    // NDC spans 2 x 2: a pixel of the viewport is (2 / vpW) x (2 / vpH)
    const float a = w * h * 0.25f * vpW * vpH;
    return std::isfinite(a) ? a : -1.0f;
}

// The decision. thresholdPx <= 0: never. A run of unknown area (< 0) is kept.
inline bool IsTiny(float areaPx, float thresholdPx) { return thresholdPx > 0.0f && areaPx >= 0.0f && areaPx < thresholdPx; }

// Census buckets (EFB pixels squared): <1, <2, <4, <8, <16, <32, <64, >= 64; index of the bucket of an area, -1 for unknown.
constexpr int kBuckets = 8;
inline int Bucket(float areaPx) {
    if (!(areaPx >= 0.0f)) return -1;
    float lim = 1.0f;
    for (int i = 0; i < kBuckets - 1; ++i, lim *= 2.0f)
        if (areaPx < lim) return i;
    return kBuckets - 1;
}

}  // namespace tinyrun
}  // namespace vgx
