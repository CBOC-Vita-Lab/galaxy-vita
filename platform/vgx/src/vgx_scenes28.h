// scenes28 (13 Nov 2026): the pure parts of GXV_CORNERPASS (Mario's corner shadow copy made from a small pass of its own at the picture's start, exact),
// GXV_INDPREV (the Indirect capture answered by the previous picture: the HUD mask, the A/B phase) and the copy's exact resample map.
// No GL, no game types: host test tests/vgx_scenes28_test.cpp (tools/test_vgx_scenes28.sh). The glue is in vgx_draw.cpp (CornerPassPlan, PrevCapPass) and
// vgx_backend_gl.cpp (CopyTex's passGrid quad, PrevKeep's alias).
//
// CORNERPASS in one paragraph. CollisionShadow::drawAndCaptureTex draws Mario's simple model into the EFB's top-right 160x160 corner (alpha only, as far as the copy
// is concerned) and copies it as A8: a copy of the EFB in the middle of the picture, so the EFB's scene is stored, a copy scene runs, and a new EFB scene loads colour
// and depth again. What the copy reads is the corner's ALPHA: its value before the first alpha-writing draw that touches the corner (provably 0 where the front end's
// alpha bookkeeping N, ShadowFillPass, says so), and the draws that write alpha there in order. None of them tests depth (checked), so the corner can be made without the
// EFB: a pass target of the corner's pixels (the EFB's own pixel grid: whole-pixel origin), cleared to alpha 0, the same draws replayed (with the same viewport / scissor
// numbers: only a whole-pixel origin shift), and the copy taken from it with the very sample positions the EFB copy would have used (ResampleMap). The EFB keeps drawing
// everything as before (the replayed draws stay in it: colour and depth side effects are untouched); only the copy that cut it is gone. The pass is placed before the
// picture's first draw, where the EFB has no scene yet, so it adds no cut.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vgx::s28 {

struct Rect { int x = 0, y = 0, w = 0, h = 0; };
inline bool Overlap(const Rect& a, const Rect& b) {
    return a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0 && a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
inline bool Inside(const Rect& a, const Rect& b) { return a.x >= b.x && a.y >= b.y && a.x + a.w <= b.x + b.w && a.y + a.h <= b.y + b.h; }

// ---- the exact resample of a pass copy
// The EFB copy (CopyTex, from g_efbColor) draws a quad that reads the render-size EFB texture over [src.x / efbW, ...] x [src.y / efbH, ...] (normalised).
// The same copy from a pass target (render pixels [x0, x0 + pw) x [y0, y0 + ph), a whole-pixel origin) must read the same render-pixel positions:
// normalised to the pass texture the rectangle is ((src.x * sx - x0) / pw, (src.y * sy - y0) / ph) with size (src.w * sx / pw, src.h * sy / ph), sx = rtW / efbW.
struct MapUV { float u0, v0, uw, vh; };
inline MapUV ResampleMap(int srcX, int srcY, int srcW, int srcH, float sx, float sy, int x0, int y0, int pw, int ph) {
    MapUV m;
    m.u0 = (static_cast<float>(srcX) * sx - static_cast<float>(x0)) / static_cast<float>(pw);
    m.v0 = (static_cast<float>(srcY) * sy - static_cast<float>(y0)) / static_cast<float>(ph);
    m.uw = static_cast<float>(srcW) * sx / static_cast<float>(pw);
    m.vh = static_cast<float>(srcH) * sy / static_cast<float>(ph);
    return m;
}

// The rectangle the pass covers: the copy's source and a margin of kCornerMargin EFB pixels (the copy's bilinear footprint and the pass target's rounding to render pixels
// reach about one render pixel beyond the source; 3 EFB pixels are 2.85 render pixels at the smallest scale, 0.95). The alpha before the draws must be known over all of it.
constexpr int kCornerMargin = 3;
inline Rect Expand(const Rect& s, int efbW, int efbH) {
    int x0 = s.x - kCornerMargin, y0 = s.y - kCornerMargin, x1 = s.x + s.w + kCornerMargin, y1 = s.y + s.h + kCornerMargin;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > efbW) x1 = efbW;
    if (y1 > efbH) y1 = efbH;
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

// ---- the plan
enum ItemKind : uint8_t { IK_DRAW, IK_COPY, IK_RES, IK_TARGET, IK_OTHER };
struct Item {
    ItemKind kind = IK_OTHER;
    Rect r;                     // draw: its extent (clipped by its scissor); copy: its source rectangle
    uint8_t alphaKind = 0;      // draw: 0 writes no alpha, 1 writes the constant 0, 2 anything else (ShadowFillPass's SfAlphaKind)
    bool depthRead = false;     // draw: it tests depth with a function other than ALWAYS
    bool unsafeReplay = false;  // draw: annotated for another path (stencil operation, fused fill, silhouette query ...): never replayed
    uint32_t tex[8] = {};       // draw: the textures it samples
    uint8_t nTex = 0;
    uint32_t vbuf = 0;          // draw: the static buffer it reads (0: the frame's arena)
    uint32_t resTex = 0;        // resource command: the texture an upload / free names
    uint32_t resBuf = 0;        // resource command: the buffer an upload / free names
    bool resShader = false;     // resource command: shader / program / variant (the pass could draw before it ran)
    uint32_t dstTex = 0;        // copy: its destination texture; draw: 0
    uint32_t srcTex = 0;        // copy: the atlas it reads
    bool cornerCand = false;    // copy: a plain A8 corner copy the front end may plan for
    bool zeroBefore = false;    // copy: alpha over its source is provably 0 before the first alpha-writing draw that touches it (ShadowFillPass's N)
};
// (W_PLANNED, not W_OK: <unistd.h> defines W_OK as a macro, and the back end includes this header)
enum Why : int { W_PLANNED = 0, W_NOTCAND, W_NOTZERO, W_NODRAW, W_ORDER, W_TARGET, W_RES, W_TEXWRITTEN, W_DEST, W_DEPTH, W_UNSAFE, W_EMPTY, W_TOOMANY, W_FILL, W_DESTCOPY, W_DESTRES, W_COUNT };
inline const char* WhyName(int w) {
    static const char* const n[W_COUNT] = {"planned", "not a corner copy", "alpha before the corner draws not known to be 0", "no draw before the copy", "copy before the picture's first draw",
                                           "render-size change in the picture", "resource command between", "a replayed draw's texture rewritten before the copy", "the copy's texture used before it",
                                           "a replayed draw tests depth", "a replayed draw is annotated", "no alpha writer touches the corner", "too many draws to replay",
                                           "the shadow window's fill may be left out", "another copy writes or reads the copy's texture before it", "the copy's texture uploaded or freed before it"};
    return w >= 0 && w < W_COUNT ? n[w] : "?";
}
struct Plan {
    int why = W_NOTCAND;
    size_t ins = 0;                // index of the item the pass is inserted before (the picture's first draw)
    std::vector<size_t> replay;    // the draws (item indices) the pass draws, in order
    bool alt = false;              // (ss24, GXV_CORNERPASS_ALT) a draw before the copy samples the copy's texture: the pass writes a second texture, swapped in at the copy
};
constexpr size_t kMaxReplay = 48;
// The copy at item j (cornerCand, zeroBefore). The pass is made before the first draw item. Returns why = W_PLANNED when it can be made.
// ss24: `allowAlt`: a draw between the pass and the copy that samples the copy's texture no longer refuses (W_DEST): it must see the texture as it was, which the back end keeps by
// making the pass's copy into a second texture and swapping the two at the copy's place (vgx_backend_gl.cpp, "ping-pong"); Plan::alt says the plan needs that.
// Another copy or an upload / free of the texture in between still refuses (W_DESTCOPY, W_DESTRES; both were W_DEST before ss24).
inline Plan PlanCorner(const std::vector<Item>& it, size_t j, bool allowAlt = false) {
    Plan p;
    if (j >= it.size() || it[j].kind != IK_COPY || !it[j].cornerCand) return p;
    const Item& c = it[j];
    if (!c.zeroBefore) { p.why = W_NOTZERO; return p; }
    size_t first = it.size();
    for (size_t i = 0; i < j; ++i)
        if (it[i].kind == IK_DRAW) { first = i; break; }
    if (first >= j) { p.why = first == it.size() ? W_NODRAW : W_ORDER; return p; }
    p.ins = first;
    // the draws to replay: every alpha writer (kind 2) that touches the corner, and from the first of them on the constant-0 writers that touch it too (they can undo it)
    bool begun = false;
    for (size_t i = first; i < j; ++i) {
        const Item& d = it[i];
        if (d.kind == IK_TARGET) { p.why = W_TARGET; return p; }
        if (d.kind != IK_DRAW) continue;
        if (!Overlap(d.r, c.r)) continue;
        if (d.alphaKind == 2) begun = true;
        if (!begun || d.alphaKind == 0) continue;
        if (d.depthRead) { p.why = W_DEPTH; return p; }
        if (d.unsafeReplay) { p.why = W_UNSAFE; return p; }
        p.replay.push_back(i);
        if (p.replay.size() > kMaxReplay) { p.why = W_TOOMANY; return p; }
    }
    if (p.replay.empty()) { p.why = W_EMPTY; return p; }
    // what the pass reads must be what the picture's start would have: no texture or buffer it uses is written / uploaded / freed between the start and the copy,
    // no shader is made between (the pass runs before them), and nothing between touches the copy's own texture.
    for (size_t i = first; i < j; ++i) {
        const Item& x = it[i];
        if (x.kind == IK_DRAW) {
            for (int m = 0; m < x.nTex; ++m)
                if (x.tex[m] == c.dstTex) {
                    if (!allowAlt) { p.why = W_DEST; return p; }
                    p.alt = true;
                }
            continue;
        }
        if (x.kind == IK_COPY) {
            if (x.dstTex == c.dstTex || x.srcTex == c.dstTex) { p.why = W_DESTCOPY; return p; }
            for (size_t q : p.replay) {
                const Item& d = it[q];
                for (int m = 0; m < d.nTex; ++m)
                    if (d.tex[m] == x.dstTex) { p.why = W_TEXWRITTEN; return p; }
            }
            continue;
        }
        if (x.kind == IK_RES) {
            if (x.resShader) { p.why = W_RES; return p; }
            if (x.resTex && x.resTex == c.dstTex) { p.why = W_DESTRES; return p; }
            for (size_t q : p.replay) {
                const Item& d = it[q];
                if (x.resBuf && d.vbuf == x.resBuf) { p.why = W_RES; return p; }
                for (int m = 0; m < d.nTex; ++m)
                    if (x.resTex && d.tex[m] == x.resTex) { p.why = W_RES; return p; }
            }
            continue;
        }
        if (x.kind == IK_TARGET) { p.why = W_TARGET; return p; }
    }
    p.why = W_PLANNED;
    return p;
}

// ---- GXV_INDPREV: the HUD mask. The previous picture's finished EFB holds the 2D tail (HUD, menus, wipes). A reader that samples it where the tail was would show it
// refracted. The front end marks the EFB cells the tail's draws touch (a coarse grid), and a reader whose (padded) extent touches a marked cell makes the picture use
// the real capture. A fullscreen 2D quad (pause dim, wipe) marks every cell.
struct HudGrid {
    static constexpr int kCols = 32, kRows = 24;
    uint8_t cell[kCols * kRows] = {};
    int efbW = 640, efbH = 456;
    void Reset(int w, int h) { std::memset(cell, 0, sizeof cell); efbW = w > 0 ? w : 640; efbH = h > 0 ? h : 456; }
    void Mark(const Rect& r) {
        if (r.w <= 0 || r.h <= 0) return;
        int c0 = r.x * kCols / efbW, c1 = (r.x + r.w - 1) * kCols / efbW, r0 = r.y * kRows / efbH, r1 = (r.y + r.h - 1) * kRows / efbH;
        c0 = c0 < 0 ? 0 : c0; r0 = r0 < 0 ? 0 : r0; c1 = c1 >= kCols ? kCols - 1 : c1; r1 = r1 >= kRows ? kRows - 1 : r1;
        for (int y = r0; y <= r1; ++y)
            for (int x = c0; x <= c1; ++x) cell[y * kCols + x] = 1;
    }
    bool Touches(const Rect& r, int pad) const {
        Rect q{r.x - pad, r.y - pad, r.w + 2 * pad, r.h + 2 * pad};
        if (q.w <= 0 || q.h <= 0) return false;
        int c0 = q.x * kCols / efbW, c1 = (q.x + q.w - 1) * kCols / efbW, r0 = q.y * kRows / efbH, r1 = (q.y + q.h - 1) * kRows / efbH;
        if (q.x < 0) c0 = 0;
        if (q.y < 0) r0 = 0;
        c0 = c0 < 0 ? 0 : c0; r0 = r0 < 0 ? 0 : r0; c1 = c1 >= kCols ? kCols - 1 : c1; r1 = r1 >= kRows ? kRows - 1 : r1;
        for (int y = r0; y <= r1; ++y)
            for (int x = c0; x <= c1; ++x)
                if (cell[y * kCols + x]) return true;
        return false;
    }
    int Marked() const { int n = 0; for (uint8_t v : cell) n += v; return n; }
};

// GXV_INDPREV_AB=N (debug): N pictures with the previous picture's capture, N with the exact one, alternating (a flicker comparison). 0 = off (the previous one always).
inline bool AbExactPhase(uint64_t picture, int n) { return n > 0 && ((picture / static_cast<uint64_t>(n)) & 1u) != 0; }

}  // namespace vgx::s28
