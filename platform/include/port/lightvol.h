// GXV_LIGHTVOL (lightvol25): the game's VolumeModelDrawer light volumes (decomp VolumeModelDrawer.cpp). Pure logic, no GX, no globals: tests/lightvol_test.cpp.
//
// One volume is four draws of the same closed model (flat colour C, constant alpha a):
//   1 clear   colour off, alpha written as the constant 0, no cull, no depth test             alpha := 0 over the model's whole footprint
//   2 back    colour off, back faces only, depth GEQUAL, blend SRCALPHA / ZERO                alpha := a * a / 255 where a back face is behind the scene
//   3 front   colour off, front faces only, depth GEQUAL, SUBTRACT                            alpha := max(0, alpha - a) where a front face is behind the scene
//   4 fill    colour on, no cull, no depth test, blend DSTALPHA / ONE, constant destination alpha 0   colour += C * alpha / 255 ; alpha := 0
// What is left is a tint C * a^2 on exactly the surface pixels that lie INSIDE the volume (the scene is behind the back face and in front of the front face), and
// alpha 0 over the footprint. Every volume ends in alpha 0 over everything it touched, so one volume's cycle never reads another's: the tints add (saturating,
// order-free) whatever the order of whole cycles, and an overlap is lit twice.
//
// What this header decides, per picture, from the list of this picture's volumes:
//   ProjectBox  the volume's 8 model-space box corners through the view and the projection: the screen rectangle (unclipped: its size; clipped: its footprint's bound),
//               "wholly outside the screen" and "wholly behind the eye" (those draw no pixel: culling them is EXACT, if the box holds the model);
//   Plan        which volumes keep their strength: f = 1 full, 0 < f < 1 tint scaled (smooth fade), f = 0 cut. By size (a volume under `areaCut` of the screen fades out over
//               the top `fadeFrac` of the range above it) and by rank (only the `keep` nearest to the player keep f = 1, the rest fade with the distance over `fadeFrac` of
//               the cut-off distance); volumes within `nearGuard` of the player are never reduced. A volume that was kept last picture needs `hyst` more to be lost
//               (the choice does not flicker under jitter) and f moves half the way to its target and at most `slew` per picture (a fade, never a jump);
//   Group       volumes whose padded screen rectangles are pairwise disjoint form a group: their passes commute, so the group's four passes can be drawn pass by pass (all
//               the clears, all the back faces, ...): the same picture, fewer state changes (EXACT).
#pragma once
#include <cmath>
#include <cstdint>

namespace port {
namespace lightvol {

constexpr int kMax = 64;  // the category's capacity (SceneNameObjListExecutor: DrawType_VolumeModel, 64)
constexpr float kCutEps = 1.0f / 256.0f;

struct Params {
    int keep = 0;           // the nearest `keep` volumes keep f = 1 (0: no cap)
    float nearGuard = 0.0f; // never reduce a volume whose surface is within this distance of the player (0: no guard)
    float areaCut = 0.0f;   // screen fraction (1 = the whole screen) under which a volume fades out (0: no size cut)
    float fadeFrac = 0.2f;  // the fade band, as a fraction of the range
    float hyst = 0.1f;      // a kept volume needs this fraction more to be lost
    float slew = 0.125f;    // the most f may move in one picture
};
struct Item {
    float dist = 0.0f;      // distance from the player to the volume's bounding sphere (surface), >= 0
    float area = -1.0f;     // screen fraction of the unclipped box rectangle; < 0: not known (never size-cut)
    bool culled = false;    // exact: draws no pixel
    float fPrev = -1.0f;    // f of the last picture; < 0: none (the volume was not drawn then: it starts at its target)
    bool keepPrev = false;
};
struct Out {
    float f = 1.0f;
    float target = 1.0f;    // before the slew
    bool keep = true;       // f >= 1/2: what the next picture's hysteresis reads
};

inline float Clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
inline float Smooth(float t) { t = Clamp01(t); return t * t * (3.0f - 2.0f * t); }

// f by size alone (monotone non-decreasing in `area`; 1 for an unknown size or no size cut)
inline float SizeFactor(float area, const Params& p, bool keepPrev) {
    if (!(p.areaCut > 0.0f) || area < 0.0f) return 1.0f;
    const float a0 = p.areaCut * (keepPrev ? 1.0f - p.hyst : 1.0f);
    const float a1 = a0 / (1.0f - p.fadeFrac);
    return Smooth((area - a0) / (a1 - a0));
}
// f by rank alone: `key` the volume's (hysteresis-scaled) distance, `dK` the cut-off (the keep-th smallest key). Monotone non-increasing in `key`.
inline float RankFactor(float key, float dK, const Params& p) {
    if (key <= dK) return 1.0f;
    float band = p.fadeFrac * dK;
    if (band < 1.0f) band = 1.0f;
    return Clamp01(1.0f - (key - dK) / band);
}

// out[i] for in[i], i < n (n <= kMax). Culled items get f = fPrev-independent 1 (they are not drawn; nothing is seen) and are not counted for the rank.
inline void Plan(const Params& p, const Item* in, int n, Out* out) {
    float keys[kMax];
    int m = 0;
    for (int i = 0; i < n; ++i)
        if (!in[i].culled) keys[m++] = in[i].dist * (in[i].keepPrev ? 1.0f - p.hyst : 1.0f);
    float dK = 1e30f;  // the keep-th smallest key (insertion sort of m <= 64 values)
    if (p.keep > 0 && m > p.keep) {
        for (int a = 1; a < m; ++a) {
            const float v = keys[a];
            int b = a - 1;
            while (b >= 0 && keys[b] > v) { keys[b + 1] = keys[b]; --b; }
            keys[b + 1] = v;
        }
        dK = keys[p.keep - 1];
    }
    for (int i = 0; i < n; ++i) {
        Out& o = out[i];
        const Item& it = in[i];
        if (it.culled) { o.f = o.target = 1.0f; o.keep = it.keepPrev; continue; }
        float t = 1.0f;
        const bool guarded = p.nearGuard > 0.0f && it.dist <= p.nearGuard;
        if (!guarded) {
            const float key = it.dist * (it.keepPrev ? 1.0f - p.hyst : 1.0f);
            t = RankFactor(key, dK, p);
            const float s = SizeFactor(it.area, p, it.keepPrev);
            if (s < t) t = s;
        }
        o.target = t;
        if (it.fPrev < 0.0f || p.slew >= 1.0f) {  // (a fresh volume starts at its target; slew >= 1: no smoothing, the test's naive control)
            o.f = t;
        } else {
            // half the gap, at most `slew` (a fade in 8 pictures from full to gone, a small oscillation of the target is halved), snapping onto the target when it is near
            float d = (t - it.fPrev) * 0.5f;
            if (d > p.slew) d = p.slew;
            if (d < -p.slew) d = -p.slew;
            o.f = it.fPrev + d;
            if (std::fabs(t - o.f) < kCutEps) o.f = t;
        }
        if (o.f > 1.0f) o.f = 1.0f;
        if (o.f < 0.0f) o.f = 0.0f;
        o.keep = o.f >= 0.5f;
    }
}

// ---- the box on the screen
struct Proj {  // a GX perspective projection: x_clip = p00 x + p02 z, y_clip = p11 y + p12 z, w = -z (P[3][2] == -1)
    float p00 = 1, p02 = 0, p11 = 1, p12 = 0;
    bool persp = true;
};
struct Rect { float x0 = -1, y0 = -1, x1 = 1, y1 = 1; };
struct BoxView {
    bool known = false;     // all 8 corners in front of the eye: the rectangle is the box's
    bool culled = false;    // wholly outside the screen, or wholly behind the eye
    float area = -1.0f;     // unclipped rectangle, screen fraction
    Rect unclipped;
    Rect foot;              // the footprint's bound: the rectangle clipped to the screen (the whole screen when not known)
    float minW = 0.0f;      // smallest depth of a corner
};
// mv: the 3x4 model -> view matrix, bmin / bmax the model-space box, padFrac the box's safety margin per side (fraction of its extent, + 1 unit), gpad the footprint's margin
// in NDC units. A volume whose box is not all in front of the eye is `known = false` and its footprint the whole screen (never size-cut, grouped alone).
inline BoxView ProjectBox(const float mv[3][4], const float* bmin, const float* bmax, const Proj& pr, float padFrac, float gpad) {
    BoxView out;
    float lo[3], hi[3];
    for (int k = 0; k < 3; ++k) {
        const float e = (bmax[k] - bmin[k]) * padFrac + 1.0f;
        lo[k] = bmin[k] - e;
        hi[k] = bmax[k] + e;
    }
    int behind = 0;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f, minW = 1e30f;
    for (int c = 0; c < 8; ++c) {
        const float x = (c & 1) ? hi[0] : lo[0], y = (c & 2) ? hi[1] : lo[1], z = (c & 4) ? hi[2] : lo[2];
        const float vx = mv[0][0] * x + mv[0][1] * y + mv[0][2] * z + mv[0][3];
        const float vy = mv[1][0] * x + mv[1][1] * y + mv[1][2] * z + mv[1][3];
        const float vz = mv[2][0] * x + mv[2][1] * y + mv[2][2] * z + mv[2][3];
        const float w = -vz;
        if (w < minW) minW = w;
        if (!(w > 1e-3f)) { ++behind; continue; }
        const float nx = (pr.p00 * vx + pr.p02 * vz) / w, ny = (pr.p11 * vy + pr.p12 * vz) / w;
        if (nx < x0) x0 = nx;
        if (nx > x1) x1 = nx;
        if (ny < y0) y0 = ny;
        if (ny > y1) y1 = ny;
    }
    out.minW = minW;
    if (!pr.persp) return out;  // (not a perspective camera: nothing is claimed)
    if (behind == 8) { out.culled = true; return out; }
    if (behind > 0) return out;
    out.known = true;
    out.unclipped = Rect{x0, y0, x1, y1};
    out.area = Clamp01((x1 - x0) * (y1 - y0) * 0.25f);
    if (x1 < -1.0f || x0 > 1.0f || y1 < -1.0f || y0 > 1.0f) { out.culled = true; return out; }
    out.foot = Rect{x0 < -1.0f ? -1.0f : x0 - gpad, y0 < -1.0f ? -1.0f : y0 - gpad, x1 > 1.0f ? 1.0f : x1 + gpad, y1 > 1.0f ? 1.0f : y1 + gpad};
    return out;
}

inline bool Disjoint(const Rect& a, const Rect& b) { return a.x1 < b.x0 || b.x1 < a.x0 || a.y1 < b.y0 || b.y1 < a.y0; }

// First-fit grouping in list order: groupOf[i] (-1 for inactive items), the number of groups. Within a group every pair of rectangles is disjoint.
inline int Group(const Rect* r, const bool* active, int n, int* groupOf) {
    int ng = 0;
    for (int i = 0; i < n; ++i) {
        groupOf[i] = -1;
        if (!active[i]) continue;
        int g = 0;
        for (; g < ng; ++g) {
            bool ok = true;
            for (int j = 0; j < i && ok; ++j)
                if (groupOf[j] == g && !Disjoint(r[i], r[j])) ok = false;
            if (ok) break;
        }
        groupOf[i] = g;
        if (g == ng) ++ng;
    }
    return ng;
}

}  // namespace lightvol
}  // namespace port
