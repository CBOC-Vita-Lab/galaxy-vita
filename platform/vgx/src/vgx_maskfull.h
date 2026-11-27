// GXV_MASKFULL: a full colour mask where the EFB's alpha is dead, as pure functions (lodmask37, idea 6 of FABLE-IDEAS.md). Host test:
// tests/vgx_maskfull_test.cpp (tools/test_vgx_maskfull.sh).
//
// A draw made with a partial colour mask (RGB written, alpha not) cannot hide what is behind it on the SGX: the hardware cannot discard the covered layers'
// shading when the draw's own pixels do not replace everything the target holds (Sony / MH3: 20.9 -> 18.5 ms there). Galaxy's opaque world writes all four
// channels; the draws that do not are those made with GXSetAlphaUpdate(FALSE) (the player, fur, direct draws) and every draw while the EFB's format has no alpha.
// The back end already plans a picture before it executes it (dplan, vgx_backend_gl.cpp): per position, whether the EFB's alpha is read again before it is
// overwritten whole (g_liveA, the plan GXV_EFBSWAP's restore trusts). Where it is dead after a draw, writing the alpha channel too is invisible: the draw's
// alpha is never seen, so the mask can be full and the draw can hide what is behind it. Colour is exact; alpha differs only where nothing reads it.
//
// A draw is changed only when ALL hold: it is drawn into the EFB (not a pass or copy target), the picture has a plan, the draw is not blended, writes colour,
// has the alpha masked, is not one of the blended-alpha forms (dstAlphaZero), has no stencil work, and the plan says the alpha is dead after it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vgx {
namespace maskfull {

// dplan's event bits (vgx_backend_gl.cpp: static_assert there keeps them equal): the alpha is read by the position / overwritten whole by it.
constexpr uint8_t kEvReadAlpha = 32, kEvFullAlpha = 128;

// The alpha plane's liveness before a position, from its events and the liveness after it (the recurrence PlanFrame runs backwards over the frame).
inline uint8_t LiveAlphaStep(uint8_t ev, uint8_t liveAfter) { return (ev & kEvReadAlpha) ? 1 : ((ev & kEvFullAlpha) ? 0 : liveAfter); }

// Whether a blended draw reads the EFB's alpha (PlanFrame's EV_RA for a draw): its blend equation takes the destination alpha when it writes alpha, the
// override forms read it, and a destination-alpha factor reads it. (An unblended draw reads nothing.)
inline bool DrawReadsAlpha(bool blend, bool alphaWrite, bool dstAlphaZero, bool dstAlphaFactor) { return blend && (alphaWrite || dstAlphaZero || dstAlphaFactor); }

// What the decision says about a draw. kNotCandidate: the draw is not a partial-mask draw of the kind this is about (not counted).
enum Why : uint8_t { kNotCandidate = 0, kTake, kNotEfb, kNoPlan, kAlphaLive, kWhyCount };

struct In {
    bool onEfb = false;       // the bound target is the EFB (not a pass or a copy target)
    bool planValid = false;   // the picture has a plan
    bool haveRecord = false;  // the draw record is known (the stencil operation is read from it)
    bool blend = false, colorWrite = false, alphaWrite = false, dstAlphaZero = false, stencilOp = false;
    bool inRange = false;     // the draw's position + 1 is inside the plan's table
    uint8_t liveAfter = 1;    // the plan's alpha liveness at position + 1 (read only when inRange)
};

inline Why Decide(const In& in) {
    if (in.blend || !in.colorWrite || in.alphaWrite || in.dstAlphaZero) return kNotCandidate;
    if (in.haveRecord && in.stencilOp) return kNotCandidate;
    if (!in.onEfb) return kNotEfb;
    if (!in.planValid || !in.haveRecord || !in.inRange) return kNoPlan;
    return in.liveAfter ? kAlphaLive : kTake;
}


// ---- hsr24: the clean prefix of a picture (GXV_MASKFULL=3, 4, 5). Galaxy's picture begins by clearing the EFB's alpha to exactly 0 (MainLoopFramework::clearEfb: a covering
// quad with GXSetDstAlpha(TRUE, 0), or a copy's clear). From that clear on, the EFB's alpha is 0 at every pixel until something WRITES a non-zero alpha. Two ends:
//   endW  : the first command that may write a non-zero alpha (a draw with the alpha mask on, a stencil-window draw, a clear that is not alpha 0). Up to it a draw that writes
//           the constant 0 over the 0 changes nothing (mode 3: needs a shader whose alpha output is 0).
//   endRW : the first command that may write OR READ the alpha (also: a blend that reads the destination alpha, a copy that keeps alpha, an effects run). Up to it nobody
//           looks at the alpha, so any alpha may be written there provided one alpha-only quad puts 0 back before endRW (mode 5: no shader change).
// The scan is pure: one kind per command.
enum CmdKind : uint8_t { kInert = 0, kEstablish, kCandidate, kReader, kWriter };
struct Prefix {
    size_t e = SIZE_MAX;       // the command whose end leaves the alpha 0 whole (SIZE_MAX: none in the picture)
    size_t endW = SIZE_MAX;    // first writer after it (SIZE_MAX: none: the picture's end)
    size_t endRW = SIZE_MAX;   // first reader or writer after it
    uint32_t readersBeforeE = 0, readers = 0, writers = 0;   // whole picture
    uint32_t candBefore = 0;   // candidates up to and including e
    uint32_t candPrefixRW = 0; // candidates in (e, endRW)
    uint32_t candPrefixW = 0;  // candidates in (e, endW)
    uint32_t candAfterRW = 0;  // candidates from endRW on (the picture's end included)
    bool hasE() const { return e != SIZE_MAX; }
};
inline Prefix AnalysePrefix(const uint8_t* kinds, size_t n) {
    Prefix p;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t k = kinds[i];
        if (k == kReader) { ++p.readers; if (!p.hasE()) ++p.readersBeforeE; }
        else if (k == kWriter) ++p.writers;
        if (!p.hasE()) {
            if (k == kCandidate) ++p.candBefore;
            if (k == kEstablish) p.e = i; // (the first one)
            continue;
        }
        if (k == kEstablish) continue; // (a later clear: not followed)
        if (p.endRW == SIZE_MAX && (k == kReader || k == kWriter)) p.endRW = i;
        if (p.endW == SIZE_MAX && k == kWriter) p.endW = i;
        if (k == kCandidate) {
            if (p.endRW == SIZE_MAX) ++p.candPrefixRW;
            else ++p.candAfterRW;
            if (p.endW == SIZE_MAX) ++p.candPrefixW;
        }
    }
    return p;
}
// Where the mode-5 quad goes and whether it is needed: `readerAt` = prefix.endRW when it is a reader / writer, else the tail: the picture's first display copy.
enum QuadWhy : uint8_t { kQuadNone = 0, kQuadAtReader, kQuadAtWriter, kQuadAtDisp, kQuadTailSkipped, kQuadNoDisp, kQuadNoCandidates, kQuadNoPrefix, kQuadInPass, kQuadWhyCount };
struct QuadPlan { size_t at = SIZE_MAX; QuadWhy why = kQuadNone; bool draw() const { return at != SIZE_MAX; } };
// kinds[endRW] says reader / writer. dispAt: the first display copy after e (SIZE_MAX: none);
// tailPredicted: GXV_MASKFULL_TAIL's predictor says the next picture begins clean.
inline QuadPlan PlanQuad(const Prefix& p, const uint8_t* kinds, size_t dispAt, bool tailPredicted) {
    QuadPlan q;
    if (!p.hasE()) { q.why = kQuadNoPrefix; return q; }
    if (p.candPrefixRW == 0) { q.why = kQuadNoCandidates; return q; }
    if (p.endRW != SIZE_MAX) { q.at = p.endRW; q.why = kinds[p.endRW] == kWriter ? kQuadAtWriter : kQuadAtReader; return q; }
    if (dispAt == SIZE_MAX) { q.why = kQuadNoDisp; return q; }
    if (tailPredicted) { q.why = kQuadTailSkipped; return q; }
    q.at = dispAt; q.why = kQuadAtDisp;
    return q;
}
// The tail predictor (GXV_MASKFULL_TAIL=1): after `run` pictures in a row that began clean (a clear that leaves the alpha 0 with no reader of the alpha before it) the next is
// predicted to begin so as well. Same idea as GXV_TAILDEPTH.
struct TailPredictor {
    uint32_t run = 0, need = 120;
    void Observe(bool beganClean) { run = beganClean ? (run < 1000000u ? run + 1 : run) : 0; }
    bool Predicts() const { return run >= need; }
};

} // namespace maskfull
} // namespace vgx
