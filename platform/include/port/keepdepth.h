#pragma once
// keepdepth31 (17 Nov 2026): the two rules of GXV_HRW_KEEPDEPTH and GXV_HRW_BOXDEPTH (the 60 Assist's second picture). Pure logic, no GL:
// tests/keepdepth31_test.cpp runs it against command lists shaped like the game's pictures and a small ray-cast scene, with mutants.
// The back end's use: platform/vgx/src/vgx_backend_gl.cpp, "keepdepth31" (PlanFrame classifies, ExecuteFrame leaves the command out, kFsBoxDepth
// is BoxDepthWeight per pixel).
//
// 1. Keep the first picture's depth. GameScene::draw3D ends with MR::clearZBuffer() (a covering quad: depth ALWAYS + write, no colour, no alpha, one
//    constant depth). From there to the next picture the EFB's depth is the far end everywhere (console c177), so the fresh player of the pair's second
//    picture, whose draws test against that depth, was never hidden by anything in front of him. On a pair's first picture the clear is left out when
//    it is the picture's LAST depth event: no later command reads, writes or overwrites the depth. The first picture's pixels are then the same (a
//    depth value nothing reads changes no pixel); the depth memory at its end holds the 3D's depth instead of the far plane. The only readers left are
//    the warp's own (the fresh player, the full-size box key), which want exactly that. The picture after the pair must not see the difference:
//    StartDead says whether it overwrites all of its depth before it reads any (the game's clearEfb quad does, first thing); when it does not, the back
//    end clears the depth for it first, to the left-out clear's value.
// 2. The player box keyed on that full-size depth. The box (his own move applied to the pixels round him) used to carry the background inside it with
//    him: the "force field". With the first picture's depth at full size the box takes a pixel only where that depth is HIS (within the tolerance of
//    his distance) at the box's own source (his body, moved) or at the scene warp's source for a point at his distance (the strip he left: filled from
//    beside him); everything else is the scene's own warp. port::hrw41::KeyWeight's rule, with the depth read from the window depth at the pixel
//    instead of from the quarter-size mask (no mask pass, no scene split, no mask-texel rim).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "port/hrw41.h"

#ifndef KEEPDEPTH_MUTANT
#define KEEPDEPTH_MUTANT 0  // (tests only: tests/keepdepth31_test.cpp builds the rules with each fault in turn; 0 = the rules as shipped)
#endif

namespace port::keepdepth {

// What a POSITION of a picture does to the EFB's depth (PlanFrame's positions: two a command, the command itself and then its clear, so the order
// inside a command is kept). The rules read nothing else.
enum : uint8_t {
    kRead = 1,       // tests or samples the depth (a draw whose function is not ALWAYS, a depth copy, an effect run, the present's depth-gated edge pass)
    kWrite = 2,      // writes some of it
    kFull = 4,       // writes all of it (a covering quad with ALWAYS + write and no discard; a copy's clear of the whole EFB's depth)
    kClear = 8,      // kFull and nothing else: no colour, no alpha, no stencil, one constant depth (MR::clearZBuffer)
    kWarpRead = 16,  // the warp's own read at the display copy (the fresh player / the box key of the pair's second picture): not a reader of the picture
    kDrawn = 32,     // (emufix31) the position is a draw (its own command, not a copy's clear): GXV_HRW_KEEPDEPTH_2D passes over such a partial write
};

enum class Why : uint8_t { Kept, NoClear, ReadAfter, WriteAfter, Count };
struct Decision {
    size_t at = SIZE_MAX;  // the position of the clear to leave out (SIZE_MAX: none)
    Why why = Why::NoClear;
    size_t by = SIZE_MAX;  // (emufix31) refused: the position of the event that refused it (the last depth event of the picture); for the census line
    uint32_t writes = 0;   // (emufix31) kept with allowWrites: the partial draw writes passed over after the clear
};

// The clear is left out only if it is the picture's last depth event: scanned from the end, the first event met must be a kClear. A read after it
// would see the 3D's depth instead of the far plane (a different picture); a write after it would leave the depth partly the 3D's and partly not
// (no use to the warp, and the plan would have to prove more); either refuses. The warp's own read does neither.
// allowWrites (emufix31, GXV_HRW_KEEPDEPTH_2D=1, default 0, not exact in one case): a partial depth write by a DRAW after the clear (kWrite | kDrawn,
// no kRead, no kFull: a depth-test-ALWAYS draw that writes depth, e.g. a 2D draw of the HUD) is passed over. The first picture stays exact: such a
// draw reads no depth, and nothing after the clear reads it, so no pixel of the picture changes. The pair's second picture: where such a draw wrote,
// the depth memory holds the draw's own value with or without the clear (it overwrites whatever was there: the same as c181), elsewhere the 3D's
// (the point of keeping). The one inexact case: the next ordinary picture reads its depth before overwriting it (never seen: the game's clearEfb
// quad comes first) - the back end's catch-up clear then writes the clear's value where c181 had that draw's: counted ("inexact"). A whole
// overwrite after the clear (kFull) still refuses: it would leave nothing of the 3D's depth.
inline Decision Decide(const uint8_t* ev, size_t n, bool allowWrites = false) {
#if KEEPDEPTH_MUTANT == 3
    (void)allowWrites;
    for (size_t j = 0; j < n; ++j) if (ev[j] & kClear) return {j, Why::Kept};  // (fault: the FIRST clear)
    return {SIZE_MAX, Why::NoClear};
#endif
    uint32_t writes = 0;
    for (size_t j = n; j-- > 0;) {
        const uint8_t e = ev[j];
#if KEEPDEPTH_MUTANT == 4
        if (e & kFull) return {j, Why::Kept};  // (fault: a covering quad that also writes colour taken for the clear)
#endif
        if (e & kClear) return {j, Why::Kept, SIZE_MAX, writes};
#if KEEPDEPTH_MUTANT == 13
        if ((e & kRead) && !allowWrites) return {SIZE_MAX, Why::ReadAfter, j};  // (fault: with allowWrites, a read after the clear is passed over)
#elif KEEPDEPTH_MUTANT != 1
        if (e & kRead) return {SIZE_MAX, Why::ReadAfter, j};
#endif
#if KEEPDEPTH_MUTANT == 5
        if (e & kWarpRead) return {SIZE_MAX, Why::ReadAfter, j};  // (fault: the warp's own read refuses: nothing is ever kept)
#endif
#if KEEPDEPTH_MUTANT == 12
        if (allowWrites && (e & (kWrite | kFull))) { ++writes; continue; }  // (fault: a whole overwrite or a copy's clear passed over too)
#elif KEEPDEPTH_MUTANT == 14
        if (allowWrites && (e & kWrite) && !(e & kFull)) { ++writes; continue; }  // (fault: a copy's partial clear passed over too)
#else
        if (allowWrites && (e & kWrite) && (e & kDrawn) && !(e & (kFull | kRead))) { ++writes; continue; }
#endif
#if KEEPDEPTH_MUTANT != 2
        if (e & (kWrite | kFull)) return {SIZE_MAX, Why::WriteAfter, j};
#endif
    }
    return {SIZE_MAX, Why::NoClear};
}

// halo32: the clear's footprint trick. A picture whose ONLY depth event after the clear is one depth-testing draw (the game's 2D tail: a HUD
// markers' draw, LEQUAL + write) may still leave the clear out, if that draw is preceded by a depth-only pre-pass of its own geometry that writes the
// clear's constant depth where the draw rasterises (back end: keepd's Pre). The draw then sees exactly the depth it saw after the clear (the clear's
// value at every pixel it can test; its own earlier writes), and nothing else reads. `reader` = the position of that draw (an even position: the
// command's own). Refuses (as Decide does) when there is a second depth event of any kind (a write before the reader would be wiped by the
// pre-pass), when the event is a whole overwrite or no draw, or there is no clear. Pure rule; tests/keepdepth31_test.cpp.
struct ReaderDecision {
    size_t at = SIZE_MAX;      // the clear's position
    size_t reader = SIZE_MAX;  // the one reading draw's position
    bool ok = false;
};
inline ReaderDecision DecideReader(const uint8_t* ev, size_t n) {
    ReaderDecision d;
    size_t clear = SIZE_MAX;
    for (size_t j = n; j-- > 0;) {
        if (ev[j] & kClear) { clear = j; break; }
    }
    if (clear == SIZE_MAX) return d;
    size_t reader = SIZE_MAX;
    for (size_t j = clear + 1; j < n; ++j) {
        const uint8_t e = ev[j];
        if (!(e & (kRead | kWrite | kFull))) continue;  // (the warp's own read is none of these)
#if KEEPDEPTH_MUTANT == 15
        if (reader != SIZE_MAX) continue;  // (fault: a second depth event is passed over)
#else
        if (reader != SIZE_MAX) return d;
#endif
        if ((e & kFull) || !(e & kDrawn) || !(e & kRead) || (j & 1)) return d;
        reader = j;
    }
    if (reader == SIZE_MAX) return d;
    d.at = clear;
    d.reader = reader;
    d.ok = true;
    return d;
}

// (emufix31) What follows the picture's last depth-only clear, for the census line and the refusal detail (log only): its position and the number of
// positions after it that read, write part of, or overwrite the whole depth (the warp's own read is not counted).
struct After {
    size_t clear = SIZE_MAX;
    uint32_t reads = 0, writes = 0, drawWrites = 0, fulls = 0;
};
inline After Survey(const uint8_t* ev, size_t n) {
    After a;
    for (size_t j = n; j-- > 0;) {
        if (ev[j] & kClear) { a.clear = j; break; }
    }
    if (a.clear == SIZE_MAX) return a;
    for (size_t j = a.clear + 1; j < n; ++j) {
        const uint8_t e = ev[j];
        if (e & kRead) ++a.reads;
        if (e & kFull) ++a.fulls;
        else if (e & kWrite) { ++a.writes; if (e & kDrawn) ++a.drawWrites; }
    }
    return a;
}

// Does the picture overwrite all of its depth before anything reads it (any read, the warp's too)? Then what the picture before it left in the
// depth memory is never seen.
inline bool StartDead(const uint8_t* ev, size_t n) {
    for (size_t j = 0; j < n; ++j) {
#if KEEPDEPTH_MUTANT != 6
        if (ev[j] & (kRead | kWarpRead)) return false;
#endif
        if (ev[j] & kFull) return true;
    }
    return false;
}

// The window depth w (0..1, the EFB depth texture's red) as the snapshot's depth code (hrw41::DepthCode of the view distance). zn = 2 w - 1 =
// f / dist - e with e = P[10], f = P[14] (WarpFragment's convention; e about -1, f negative): dist = f / (zn + e) = -f / -(zn + e).
inline double WindowCode(double w, double e, double f) {
#if KEEPDEPTH_MUTANT == 9
    const double zn = 1.0 - 2.0 * w;  // (fault: the depth's sign)
#else
    const double zn = 2.0 * w - 1.0;
#endif
    const double den = std::max(-(zn + e), 1e-7);
    return hrw41::DepthCode(-f / den);
}
// What stands in front of him where the box would draw: the first picture's depth at the scene's own source (the pixel's content without the box)
// nearer than his band by more than 1.5 tolerances (where the key has reached 0) is an occluder (a post, a blade of grass, a rail): 1 from 2
// tolerances on, so the box's moved pixels never cover it. (Without it the box drew his old pixels over the post: the depth key alone answers
// "is this his", not "is he in front".)
inline double Occluder(const hrw41::Key& k, double codeScene) {
    return std::clamp((static_cast<double>(k.code) - codeScene - 1.5 * static_cast<double>(k.tol)) * static_cast<double>(k.slope), 0.0, 1.0);
}
// One pixel of the box: 1 = the box's pixel (his, moved), 0 = the scene's own warp. wBox = the first picture's window depth at the box's source,
// wScene = at the scene warp's source for a point at his distance. kFsBoxDepth computes exactly this.
inline double BoxDepthWeight(const hrw41::Key& k, double wBox, double wScene, double e, double f) {
    const double cBox = WindowCode(wBox, e, f), cScene = WindowCode(wScene, e, f);
#if KEEPDEPTH_MUTANT == 10
    (void)k; (void)cBox; (void)cScene;
    return 1.0;  // (fault: no key: the plain box, the background moves with him)
#elif KEEPDEPTH_MUTANT == 7
    return hrw41::KeyWeight(k, cBox, 2.0) * (1.0 - Occluder(k, cScene));  // (fault: the box's own source only)
#elif KEEPDEPTH_MUTANT == 8
    return hrw41::KeyWeight(k, 2.0, cScene) * (1.0 - Occluder(k, cScene));  // (fault: the scene's source only)
#elif KEEPDEPTH_MUTANT == 11
    return hrw41::KeyWeight(k, cBox, cScene);  // (fault: no occluder term)
#else
    return hrw41::KeyWeight(k, cBox, cScene) * (1.0 - Occluder(k, cScene));
#endif
}

}  // namespace port::keepdepth
