#pragma once
// halo31 (18 Nov 2026), GXV_HRW_HALO: the 60 Assist's second picture without the player's box. Pure logic, no GL: tests/halo31_test.cpp runs
// it on a ray-cast scene against a true redraw, with mutants. The back end's use: platform/vgx/src/vgx_backend_gl.cpp, "halo31".
//
// The "force field" (the owner, 14-18 Nov; console c181 and console6): the second picture of a pair is the first one re-projected (the scene's
// warp: the camera's rotation, and the ground plane under the player where he has ground), then a BOX round the player (about 80 x 110 px) is
// drawn again moved by HIS OWN move, then he is drawn fresh on top. The box is there for one reason: the scene's warp shows the FIRST picture's
// player where the scene took him, a few pixels from where the fresh player is drawn, and without the box that old player sticks out behind
// the fresh one. But the box moves everything inside it by his move, the ground, the water and the wall behind him too, while the scene's warp
// moves the same things by the camera's move just outside it: a patch round him that moves differently from its surroundings, in every second
// picture, 30 times a second.
//
// halo31: no box. The scene's warp is left as it is everywhere (the background round him moves exactly as its surroundings). The old player's
// pixels are found without any depth and without touching the first picture: the fresh player's own draws are drawn once more, moved to where
// the scene's warp shows the old player, into the STENCIL only (the stamp). Where the stamp is and the fresh player is not (the rim the old
// player would show: as wide as the two are apart), the scene's warp is drawn again taken from beside him (moved by the same vector, so the
// source lies outside the old player). Then nothing else: the fresh player's own draws have already covered the rest.
//   - exact where the c181 path is exact: the first picture is not touched (no mask, no depth copy, no scene split, no marker);
//   - no depth is read (the console keeps none: console6, 18 Nov, GXV_HRW_KEEPDEPTH refused all 900 pairs of a window);
//   - no new program: the stamp is his own draws with the colour masked, the fill is the scene's own warp program in another viewport;
//   - the pixels changed against the plain scene warp are the rim only (a few hundred), not the box (6,500).
// What it cannot know: the old player's POSE (the stamp has the fresh one's, a sixtieth later). A limb that moved against his body leaves a
// sliver of the old limb, as it does with the box (the host scene: 40-55 px of 5,000).
#include <algorithm>
#include <cmath>
#include <cstdint>

#ifndef HALO31_MUTANT
#define HALO31_MUTANT 0  // (tests only: tests/halo31_test.cpp builds the rules with each fault in turn; 0 = the rules as shipped)
#endif

namespace port::halo31 {

constexpr int kMaxBands = 8;

// One point of the player in both pictures, in the picture's pixels: (ax, ay) where the first picture shows it, (tx, ty) where the second
// picture's camera sees it now (where his fresh draw puts it).
struct Anchor { double ax = 0, ay = 0, tx = 0, ty = 0; };

enum class Why : uint8_t { On, Still, TooFar, NoSource, NoAnchor };
// The scene's warp does not move the old player as one piece: the ground plane takes his feet with the ground and his head with the ground
// far behind it (he "leans"), a pan turns him about the camera. So the plan is a few BANDS across his body (along the line feet - head), each
// with its own move: inside a band's rectangle the fresh player's silhouette is stamped moved by `stamp`, and the rim is filled from the
// scene's warp moved by `fill`.
struct Band {
    int rect[4] = {0, 0, 0, 0};  // x0, y0, x1, y1 (the scissor of this band's stamp and fill)
    int stamp[2] = {0, 0};       // the viewport's move for the stamp (pixels): the fresh player's silhouette -> where the scene's warp shows the old player
    int fill[2] = {0, 0};        // the viewport's move for the fill's scene pass: the pixel p then shows what the scene's warp has at p - fill
    // outline49: the second stamp. The depth mask is a quarter-size texture (nearest of four taps): a texel at the old player's edge that has his
    // depth is moved with him (inside `stamp`), one that has the background's is NOT moved by his move, so the first picture's own pixels of him at
    // the edge stay where the first picture had them (the scene's warp at the background's move: a sliver, the "outline trailing a few px behind"
    // when he runs). The same silhouette is stamped (stencil bit 0x10) at stamp0, where the first picture shows him, and the sliver filled from
    // beside him on the far side, fill0.
    bool back = false;           // stamp0 / fill0 are drawn
    int stamp0[2] = {0, 0};      // the fresh silhouette -> where the FIRST picture shows the old player (his own move not applied)
    int fill0[2] = {0, 0};       // the viewport's move for the fill of the stamp0-only pixels
};
struct Plan {
    Why why = Why::NoAnchor;
    bool on = false;             // stamps and fills are to be drawn
    int n = 0;
    Band band[kMaxBands];
    int rect[4] = {0, 0, 0, 0};  // all of it: the stencil's reset (x0, y0, x1, y1; even, inside the picture)
    double apart = 0.0;          // how far the old player (as the scene's warp shows him) is from the fresh one, pixels (the largest of the anchors)
    double lean = 0.0;           // how much that differs along his body, pixels
};

// The pixel of the second picture whose SOURCE under the scene's warp is the first picture's pixel (ax, ay): the scene warp's map is the
// identity plus a few pixels, smooth, so a fixed-point step converges in two or three rounds. src(bx, by, sx, sy) -> bool: the source, in the
// picture's pixels, of the second picture's pixel (bx, by), exactly as the pass that is drawn takes it (rotation, the ground plane, a depth
// warp at his distance).
template <class Src>
inline bool Where(Src&& src, double ax, double ay, double& bx, double& by) {
    bx = ax;
    by = ay;
    double err = 1e9;
    for (int i = 0; i < 12; ++i) {
        double sx, sy;
        if (!src(bx, by, sx, sy) || !std::isfinite(sx) || !std::isfinite(sy)) return false;
        const double ex = sx - ax, ey = sy - ay;
        err = std::hypot(ex, ey);
        if (err < 0.02) return true;
        bx -= ex;
        by -= ey;
        if (!std::isfinite(bx) || !std::isfinite(by) || std::fabs(bx - ax) > 4096.0 || std::fabs(by - ay) > 4096.0) return false;
    }
    return err < 0.5;
}

// The plan of a pair. anchors: his centre first, then (if known) his feet and his head. box: the rectangle his body takes in the second
// picture (the c181 box's: x0, y0, x1, y1). minPx: the old and the fresh player nearer than this are one (nothing is drawn, no box either).
// maxPx: farther apart than this is not a move the fill can hide (a warp to another place, a camera cut): the caller keeps the box.
// stepPx: a band for every this many pixels of lean, maxBands at most. pad: pixels the rectangle is grown by.
template <class Src>
inline Plan Make(Src&& src, const Anchor* anchors, int count, const int box[4], int cw, int ch, double minPx, double maxPx, double stepPx, int pad, int maxBands = kMaxBands, double backMin = 1.0e9) {
    Plan p;
    if (count <= 0 || cw <= 0 || ch <= 0) return p;
    // per anchor: where the scene's warp shows the old player's point (wx, wy), and its move from the fresh one's (sx, sy)
    double wx[3] = {}, wy[3] = {}, sx[3] = {}, sy[3] = {}, ux0[3] = {}, uy0[3] = {};  // (ux0, uy0: where the first picture shows him, against the fresh one)
    int n = 0;
    for (int i = 0; i < count && i < 3; ++i) {
        double bx, by;
        if (!Where(src, anchors[i].ax, anchors[i].ay, bx, by)) {
            if (i == 0) { p.why = Why::NoSource; return p; }
            continue;  // (his feet or head has no source: the others stand for it)
        }
#if HALO31_MUTANT == 1
        bx = anchors[i].ax; by = anchors[i].ay;  // (fault: the old player taken where the FIRST picture had him, not where the scene's warp shows him)
#endif
        wx[n] = bx; wy[n] = by;
#if HALO31_MUTANT == 2
        sx[n] = anchors[i].tx - bx; sy[n] = anchors[i].ty - by;  // (fault: the stamp's sign)
#else
        sx[n] = bx - anchors[i].tx;
        sy[n] = by - anchors[i].ty;
#endif
        ux0[n] = anchors[i].ax - anchors[i].tx; uy0[n] = anchors[i].ay - anchors[i].ty;
        p.apart = std::max(p.apart, std::hypot(sx[n], sy[n]));
        ++n;
    }
    if (!std::isfinite(p.apart)) { p.why = Why::NoSource; return p; }
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) p.lean = std::max(p.lean, std::hypot(sx[i] - sx[j], sy[i] - sy[j]));
    if (p.apart < minPx) { p.why = Why::Still; return p; }
    if (p.apart > maxPx) { p.why = Why::TooFar; return p; }
    int x0 = box[0], y0 = box[1], x1 = box[2], y1 = box[3];
    for (int i = 0; i < n; ++i) {
        const int ix = static_cast<int>(sx[i] < 0 ? std::floor(sx[i]) : std::ceil(sx[i])), iy = static_cast<int>(sy[i] < 0 ? std::floor(sy[i]) : std::ceil(sy[i]));
        x0 = std::min(x0, box[0] + ix); y0 = std::min(y0, box[1] + iy);
        x1 = std::max(x1, box[2] + ix); y1 = std::max(y1, box[3] + iy);
        if (backMin < 1.0e8) {  // (outline49: the second stamp's place too)
            const int jx = static_cast<int>(ux0[i] < 0 ? std::floor(ux0[i]) : std::ceil(ux0[i])), jy = static_cast<int>(uy0[i] < 0 ? std::floor(uy0[i]) : std::ceil(uy0[i]));
            x0 = std::min(x0, box[0] + jx); y0 = std::min(y0, box[1] + jy);
            x1 = std::max(x1, box[2] + jx); y1 = std::max(y1, box[3] + jy);
        }
    }
    p.rect[0] = std::clamp((x0 - pad) & ~1, 0, cw);
    p.rect[1] = std::clamp((y0 - pad) & ~1, 0, ch);
    p.rect[2] = std::clamp((x1 + pad + 1) & ~1, 0, cw);
    p.rect[3] = std::clamp((y1 + pad + 1) & ~1, 0, ch);
    if (p.rect[2] - p.rect[0] < 2 || p.rect[3] - p.rect[1] < 2) { p.why = Why::NoAnchor; return p; }
    // the bands: across the axis his body lies along on the screen (the old player's own points), equal slices of the rectangle
    double ux = 0.0, uy = 0.0;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (std::hypot(wx[i] - wx[j], wy[i] - wy[j]) > std::hypot(ux, uy)) { ux = wx[i] - wx[j]; uy = wy[i] - wy[j]; }
    const bool alongY = std::fabs(uy) >= std::fabs(ux);
#if HALO31_MUTANT == 3
    const int bands = 1;  // (fault: one move for his whole body whatever the lean)
#else
    const int bands = n < 2 ? 1 : std::clamp(static_cast<int>(std::ceil(p.lean / std::max(stepPx, 0.5))), 1, std::clamp(maxBands, 1, kMaxBands));
#endif
    const int lo = alongY ? p.rect[1] : p.rect[0], hi = alongY ? p.rect[3] : p.rect[2];
    // the move at a place along the axis: straight lines between the anchors (sorted along it), the end values beyond them
    int order[3] = {0, 1, 2};
    const auto key = [&](int i) { return alongY ? wy[i] : wx[i]; };
    for (int i = 0; i < n; ++i) for (int j = i + 1; j < n; ++j) if (key(order[j]) < key(order[i])) std::swap(order[i], order[j]);
    const auto moveAtOf = [&](const double* sx, const double* sy, double u, double& mx, double& my) {
        if (n == 1 || u <= key(order[0])) { mx = sx[order[0]]; my = sy[order[0]]; return; }
        for (int i = 1; i < n; ++i) {
            const double a = key(order[i - 1]), b = key(order[i]);
            if (u <= b) {
                const double t = b - a > 1e-6 ? (u - a) / (b - a) : 0.0;
                mx = sx[order[i - 1]] + (sx[order[i]] - sx[order[i - 1]]) * t;
                my = sy[order[i - 1]] + (sy[order[i]] - sy[order[i - 1]]) * t;
                return;
            }
        }
        mx = sx[order[n - 1]]; my = sy[order[n - 1]];
    };
    const auto moveAt = [&](double u, double& mx, double& my) { moveAtOf(sx, sy, u, mx, my); };
    for (int b = 0; b < bands; ++b) {
        const int a0 = lo + (hi - lo) * b / bands, a1 = lo + (hi - lo) * (b + 1) / bands;
        if (a1 <= a0) continue;
        double mx, my;
        moveAt(0.5 * (a0 + a1), mx, my);
        const double len = std::hypot(mx, my);
        if (len < minPx) continue;  // (this part of him is where the fresh one is: nothing to hide)
        Band& B = p.band[p.n++];
        B.rect[0] = alongY ? p.rect[0] : a0; B.rect[1] = alongY ? a0 : p.rect[1];
        B.rect[2] = alongY ? p.rect[2] : a1; B.rect[3] = alongY ? a1 : p.rect[3];
        B.stamp[0] = static_cast<int>(std::lround(mx));
        B.stamp[1] = static_cast<int>(std::lround(my));
        // The fill's source: beside the old player, on the far side from the fresh one. A rim pixel p is in the old player and not in the
        // fresh one; the old one is the fresh one moved by s, so p + s is not in the old one: the scene's warp at p + s k (k a little over 1).
        const double k = (len + 1.0) / len;
#if HALO31_MUTANT == 4
        B.fill[0] = static_cast<int>(std::lround(mx * k)); B.fill[1] = static_cast<int>(std::lround(my * k));  // (fault: the fill taken from the fresh player's side: the old player again)
#elif HALO31_MUTANT == 5
        B.fill[0] = 0; B.fill[1] = 0;  // (fault: the fill is the scene's warp where it is: the old player again)
#else
        B.fill[0] = -static_cast<int>(std::lround(mx * k));
        B.fill[1] = -static_cast<int>(std::lround(my * k));
#endif
        if (backMin < 1.0e8) {  // (outline49) the old player where the first picture shows him: his move by the scene's warp is m = s - d0 (the parallax of his depth)
            double dx, dy;
            moveAtOf(ux0, uy0, 0.5 * (a0 + a1), dx, dy);
            const double qx = mx - dx, qy = my - dy, ql = std::hypot(qx, qy);
            if (ql >= backMin && std::hypot(dx, dy) >= 0.5) {
                B.back = true;
                B.stamp0[0] = static_cast<int>(std::lround(dx)); B.stamp0[1] = static_cast<int>(std::lround(dy));
                const double k0 = (ql + 1.0) / ql;  // (the sliver lies on the far side of his move: the source is further along it; the fill's move is its opposite sign to fill's)
                B.fill0[0] = static_cast<int>(std::lround(qx * k0)); B.fill0[1] = static_cast<int>(std::lround(qy * k0));
            }
        }
    }
    if (p.n == 0) { p.why = Why::Still; return p; }
    p.why = Why::On;
    p.on = true;
    return p;
}

// One pixel of the second picture, as the stencil decides it (the test's model of the passes; the back end has the same three values):
// 0 nothing, 1 the stamp alone (the old player's rim: filled), 2 the fresh player (his own pixel stays).
enum : uint8_t { kNone = 0, kStamp = 1, kFresh = 2 };
inline uint8_t Stencil(bool stamped, bool fresh) {
#if HALO31_MUTANT == 6
    return stamped ? kStamp : fresh ? kFresh : kNone;  // (fault: the stamp wins over the fresh player: the fill is drawn over him)
#else
    return fresh ? kFresh : stamped ? kStamp : kNone;
#endif
}

}  // namespace port::halo31
