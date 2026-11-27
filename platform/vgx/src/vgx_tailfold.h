// GXV_TAILFOLD (tail37, 21 Nov 2026), day 1: the CENSUS of folding a picture's tail into the present. No GL, no SDK: the decision over a picture's commands
// (as a small description), the census and the log line, so tests/vgx_tailfold_test.cpp can drive them without the game.
//
// With the bloom on, the EFB's last scene of a picture is the scene's put-back (a covering quad that samples the capture _24, made at the picture's start and
// handed over to a texture by GXV_EFBSWAP), the glow add, sometimes a full-screen fade quad, and the 2D layer; then the present reads that EFB once more.
// GXV_HUD_NATIVE already draws the 2D tail straight into the display scene but stops at the first non-2D draw, so the put-back and the glow add still make a full
// size EFB scene whose only reader is the present. Folding: the present samples the put-back's source texture and draws the rest of the stretch into the display
// scene; the EFB scene after the last leave of the EFB is never begun (one full-size scene: colour load + store, a covering quad, the kick).
//
// A STRETCH is the draws between the picture's last command that leaves the EFB (a copy, a pass, a target change: dplan's EV_LEAVE) and the display copy. It is
// foldable when
//   - its first draw is an unblended covering quad (DrawRec::cover) that samples a texture which is a handed-over EFB buffer (the copy that made it was a whole
//     1:1 copy the front end marked swapOk), at 1:1;
//   - every draw has no depth test or write, no destination-alpha factor, no stencil operation and is not an effect-target draw;
//   - the picture is shown at the render size (1:1 rung), without the edge pass or the upscaler's sharpening (they run on the EFB the fold would not make), the EFB
//     is not multisampled, no capture or screenshot is asked for, GXV_HRW is 0, not in the Quality mode (it owns its scene structure) and no movie plays;
//   - the EFB's colour is dead at the end (the display copy clears it whole: nothing of the next picture reads what this scene would have left).
// Modes 1 and 2 count; mode 3 applies the fold (GXV_TAILFOLD=1 and =2 both: the log says "count only"); the fold itself (second task) will refuse by the same reasons.
// Log (basic log, every 600 pictures): "tailfold: GXV_TAILFOLD=m (count only): P pictures, F foldable (x%), draws in the stretch ..., refused: <reason> n, ...".
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace vgx::tf {

enum Reason : int {
    kFoldable = 0,  // every test passed (counted, not folded)
    kNoPicture,     // an upload-only buffer, or several display copies (not counted as a picture)
    kHrw,           // GXV_HRW is not 0
    kCapture,       // a capture / screenshot / diagnostic frame
    kMsaa,          // multisampled EFB
    kQuality,       // the Quality mode
    kMovie,         // a movie plays
    kEdge,          // the edge pass is on (GXV_EDGE_AA): it would run before the glow add
    kRung,          // the render size is not the screen's (the upscaler stretches before the glow add: equal only up to rounding)
    kColourLive,    // the display copy does not clear the EFB whole: the colour is live at the end
    kNoTail,        // no draw after the last command that leaves the EFB
    kFirstQuad,     // the stretch's first draw is not an unblended covering quad that samples a texture
    kNotHandover,   // its texture is not a handed-over whole 1:1 EFB capture (no swap, a partial or scaled copy, or not made in this picture)
    kDepth,         // a draw with a depth test or a depth write
    kDstAlpha,      // a draw that blends with the destination's alpha
    kStencil,       // a draw with a stencil operation
    kEffectTarget,  // an effect-target (half-size) draw
    kCopyInside,    // the put-back is there but a copy or pass comes after it: the stretch does not hold it
    kNoProj,        // a draw of the stretch has no projection block (the display mapping cannot be applied)
    kPresentPass,   // the display copy asks for a present pass that reads the EFB (character / player smoothing, a kept picture) or the picture is shown through a host that cannot do the phases
    kLate,          // armed, but declined at its start (run time): the capture was not handed over after all, or the display's rectangle is not the picture's
    kCount
};
inline const char* ReasonName(int r) {
    static const char* const n[kCount] = {"foldable", "not a picture", "hrw", "capture", "msaa", "quality mode", "movie", "edge pass", "rung not 1:1", "colour live at end", "no tail",
                                          "first quad not covering", "source not handed over", "depth", "dst alpha", "stencil", "effect target", "copy inside", "no projection", "present pass / host",
                                          "declined at run time"};
    return r >= 0 && r < kCount ? n[r] : "?";
}

struct Draw {
    bool cover = false;            // DrawRec::cover
    bool blend = false, depthTest = false, depthWrite = false, dstAlpha = false, stencil = false, effect = false;
    bool noProj = false;           // no projection block (the display mapping needs one)
    bool textured = false;         // samples at least one texture
    bool sourceHandover = false;   // texture 0 was made by a whole 1:1 copy of this picture that the front end marked swapOk
    bool sourceWhole = false;      // ... whole and 1:1 at all (a copy of the whole EFB to a texture of the EFB's size)
};
enum Kind : uint8_t { kResource, kDraw, kLeave, kDisp };
struct Item {
    Kind kind = kResource;
    Draw d;                        // kDraw
};
// The picture-wide inputs the machine fills.
struct Inputs {
    int hrw = 0;
    bool capture = false, msaa = false, quality = false, movie = false, edge = false, rung1to1 = true, clearWhole = false;
    bool presentPass = false;      // character / player smoothing or a kept picture asked for by the display copy, no host with phases, overscan, shader work in the frame, draws after the display copy
};
struct Result {
    Reason why = kNoPicture;
    unsigned stretch = 0;          // draws in the stretch (0 when it was not looked at)
};

// items[0 .. n): the picture's commands in order; the display copy is items[disp] (the last kDisp is the one used; the caller counts several as kNoPicture).
inline Result Scan(const Item* items, size_t n, size_t disp, const Inputs& in) {
    Result r;
    if (disp >= n || items[disp].kind != kDisp) return r;
    // the stretch: back from the display copy over draws and resource commands to the last command that leaves the EFB
    size_t start = disp;
    for (size_t i = disp; i-- > 0;) {
        if (items[i].kind == kLeave) break;
        start = i;
    }
    unsigned draws = 0;
    size_t first = SIZE_MAX;
    for (size_t i = start; i < disp; ++i)
        if (items[i].kind == kDraw) { if (first == SIZE_MAX) first = i; ++draws; }
    r.stretch = draws;
    if (in.hrw != 0) { r.why = kHrw; return r; }
    if (in.capture) { r.why = kCapture; return r; }
    if (in.msaa) { r.why = kMsaa; return r; }
    if (in.quality) { r.why = kQuality; return r; }
    if (in.movie) { r.why = kMovie; return r; }
    if (in.edge) { r.why = kEdge; return r; }
    if (!in.rung1to1) { r.why = kRung; return r; }
    if (in.presentPass) { r.why = kPresentPass; return r; }
    if (draws == 0) { r.why = kNoTail; return r; }
    const Draw& f = items[first].d;
    if (!f.cover || f.blend || !f.textured) {
        // a put-back that lies before a later copy / pass: the stretch after that copy does not hold it
        bool earlier = false;
        for (size_t i = 0; i < start && !earlier; ++i)
            if (items[i].kind == kDraw && items[i].d.cover && !items[i].d.blend && items[i].d.textured && items[i].d.sourceWhole) earlier = true;
        r.why = earlier ? kCopyInside : kFirstQuad;
        return r;
    }
    if (!f.sourceHandover) { r.why = kNotHandover; return r; }
    bool depth = false, dsta = false, sten = false, eff = false, noProj = false;
    for (size_t i = first; i < disp; ++i) {
        if (items[i].kind != kDraw) continue;
        const Draw& d = items[i].d;
        depth = depth || d.depthTest || d.depthWrite;
        dsta = dsta || d.dstAlpha;
        sten = sten || d.stencil;
        eff = eff || d.effect;
        noProj = noProj || d.noProj;
    }
    if (depth) { r.why = kDepth; return r; }
    if (dsta) { r.why = kDstAlpha; return r; }
    if (sten) { r.why = kStencil; return r; }
    if (eff) { r.why = kEffectTarget; return r; }
    if (noProj) { r.why = kNoProj; return r; }
    if (!in.clearWhole) { r.why = kColourLive; return r; }
    r.why = kFoldable;
    return r;
}

struct Census {
    uint64_t pictures = 0, foldable = 0, stretchDraws = 0, foldableDraws = 0, reason[kCount] = {};
    uint64_t folded = 0, foldedDraws = 0;   // mode 3: pictures whose tail went into the present (counted when the picture ends)
    uint64_t lastLog = 0;
    void Note(const Result& r) {
        if (r.why == kNoPicture) return;
        ++pictures;
        stretchDraws += r.stretch;
        if (r.why == kFoldable) { ++foldable; foldableDraws += r.stretch; }
        else ++reason[r.why];
    }
    // The basic-log line, every `every` pictures (0: never); returns the text length, 0 when not due.
    int Line(char* out, size_t cap, uint64_t every, int mode) {
        if (every == 0 || pictures < lastLog + every) return 0;
        lastLog = pictures;
        const double p = pictures ? static_cast<double>(pictures) : 1.0;
        int n = std::snprintf(out, cap, "tailfold: GXV_TAILFOLD=%d (%s): %llu pictures, foldable %llu (%.0f%%), draws in the stretch %.1f a picture (%.1f in the foldable ones); refused:", mode, mode == 3 ? "applied: foldable ones are folded" : "count only, nothing is folded",
                              static_cast<unsigned long long>(pictures), static_cast<unsigned long long>(foldable), 100.0 * static_cast<double>(foldable) / p,
                              static_cast<double>(stretchDraws) / p, foldable ? static_cast<double>(foldableDraws) / static_cast<double>(foldable) : 0.0);
        if (mode == 3 && n > 0 && n < static_cast<int>(cap)) {
            const double f = static_cast<double>(folded);
            n += std::snprintf(out + n, cap - static_cast<size_t>(n), " [folded %llu (%.0f%% of pictures), %.1f draws of the tail each]", static_cast<unsigned long long>(folded), 100.0 * f / p,
                               folded ? static_cast<double>(foldedDraws) / f : 0.0);
        }
        bool any = false;
        for (int r = kHrw; r < kCount && n > 0 && n < static_cast<int>(cap); ++r)
            if (reason[r]) { n += std::snprintf(out + n, cap - static_cast<size_t>(n), " %s %llu,", ReasonName(r), static_cast<unsigned long long>(reason[r])); any = true; }
        if (!any && n > 0 && n < static_cast<int>(cap)) n += std::snprintf(out + n, cap - static_cast<size_t>(n), " none");
        return n;
    }
};

// GXV_TAILFOLD=3, every 10 s (basic log): the window's pictures, folds and the EFB scenes saved (a fold removes one full-size EFB scene: the model; the measured
// scenes a picture of the last frame's counter, folded pictures against the others, are beside it).
struct Window {
    uint64_t pictures = 0, armed = 0, folded = 0, late = 0, foldedDraws = 0;
    uint64_t sceneSumFolded = 0, sceneNFolded = 0, sceneSumOther = 0, sceneNOther = 0;
    uint64_t refused[kCount] = {};
    void Clear() { *this = Window{}; }
    int Line(char* out, size_t cap, double seconds) const {
        const double p = pictures ? static_cast<double>(pictures) : 1.0;
        int n = std::snprintf(out, cap, "tailfold-window: GXV_TAILFOLD=3, %.1f s: %llu pictures, armed %llu, folded %llu (%.0f%%; %.1f tail draws each), declined at run time %llu, EFB scenes saved %llu (one a fold); "
                                         "scenes a picture (measured): folded %.2f over %llu, others %.2f over %llu; refused:", seconds, static_cast<unsigned long long>(pictures),
                              static_cast<unsigned long long>(armed), static_cast<unsigned long long>(folded), 100.0 * static_cast<double>(folded) / p,
                              folded ? static_cast<double>(foldedDraws) / static_cast<double>(folded) : 0.0, static_cast<unsigned long long>(late), static_cast<unsigned long long>(folded),
                              sceneNFolded ? static_cast<double>(sceneSumFolded) / static_cast<double>(sceneNFolded) : 0.0, static_cast<unsigned long long>(sceneNFolded),
                              sceneNOther ? static_cast<double>(sceneSumOther) / static_cast<double>(sceneNOther) : 0.0, static_cast<unsigned long long>(sceneNOther));
        bool any = false;
        for (int r = kHrw; r < kCount && n > 0 && n < static_cast<int>(cap); ++r)
            if (refused[r]) { n += std::snprintf(out + n, cap - static_cast<size_t>(n), " %s %llu,", ReasonName(r), static_cast<unsigned long long>(refused[r])); any = true; }
        if (!any && n > 0 && n < static_cast<int>(cap)) n += std::snprintf(out + n, cap - static_cast<size_t>(n), " none");
        return n;
    }
};

} // namespace vgx::tf
