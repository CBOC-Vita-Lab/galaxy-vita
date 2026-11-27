// Speculative GXV_MASKFULL Tail-Alpha predictor (port::maskfull::TailAlphaPredictor), mirroring vgx::s26::TailPredictor (GXV_TAILDEPTH).
//
// Why: GXV_MASKFULL=1 enables full RGBA writes (restoring SGX543 HSR) on unblended RGB-only opaque draws whenever alpha liveness says
// EFB alpha is dead after the draw. Without tail prediction, PlanFrame must initialize g_liveA[positions] = 1 (assuming the next frame
// might read EFB alpha before overwriting it), which keeps g_liveA == 1 across the entire frame after the opening clear quad even when
// 0 commands in the frame read EFB alpha.
//
// Speculative contract: this is a cross-frame predictor (like GXV_TAILDEPTH). Keep default-off (0) until console census confirms
// staleTrips == 0 across all stages. Shared dplan::g_liveA stays strictly conservative (tail = 1) for EfbSwap / directd::Settle /
// s26rt; only MaskFullWant reads the dedicated g_liveATail array when GXV_MASKFULL_TAIL=1 drops the tail.
// On any sudden pre-establish EFB alpha read after a dropped tail, there is a 1-frame trip window after which staleTrips increments
// and the predictor permanently disables itself (off_ = true).
//
// TailAlphaPredictor observes each planned picture's start:
//   - kCleanEstablish: EFB alpha is wholly established (cleared/overwritten across the full EFB) before any EFB alpha read.
//   - kReadBeforeEstablish: an EFB alpha read occurs before EFB alpha is wholly established.
//   - kNeither: neither an EFB alpha read nor a whole-EFB alpha establish occurs in the picture (e.g. an RGB8 / fade frame).
//
// After `need` (default 60) consecutive non-HRW pictures with kCleanEstablish, DropTail() predicts g_liveA[positions] = 0 for non-HRW
// pictures. HRW pair pictures (where SceneSnap / Snapshot read EFB alpha outside g_ev) are always refused and break the streak.
// Fail-safe trip-off: if any subsequent picture has kReadBeforeEstablish (or an HRW picture without a clean establish first) while an
// earlier picture dropped the alpha tail and alpha has not yet been re-established, staleTrips increments and the predictor permanently
// disables itself (off_ = true), matching TailDepth's exact trip policy.
#pragma once

#include <cstddef>
#include <cstdint>

namespace port {
namespace maskfull {

// Must match dplan::EV_RA and dplan::EV_FA (checked by static_assert in vgx_backend_gl.cpp).
constexpr uint8_t kEvReadAlpha = 32;
constexpr uint8_t kEvFullAlpha = 128;

enum class StartAlpha : uint8_t {
    kNeither = 0,
    kCleanEstablish,
    kReadBeforeEstablish,
};

// Classifies whether a picture reads EFB alpha before wholly establishing it.
// If `estab` is non-null, estab[p] != 0 marks positions conservatively proven to overwrite all of EFB alpha; otherwise (ev[p] & kEvFullAlpha).
// At any position with both read and establish, read happens first and wins conservatively.
inline StartAlpha ClassifyStartAlpha(const uint8_t* ev, const uint8_t* estab, size_t positions) {
    for (size_t p = 0; p < positions; ++p) {
        if (ev[p] & kEvReadAlpha) return StartAlpha::kReadBeforeEstablish;
        if (estab ? (estab[p] != 0) : ((ev[p] & kEvFullAlpha) != 0)) return StartAlpha::kCleanEstablish;
    }
    return StartAlpha::kNeither;
}

// Backward EFB alpha liveness pass (positions + 1 entries).
inline void ComputeLiveAlpha(const uint8_t* ev, size_t positions, uint8_t tailLive, uint8_t* liveOut) {
    liveOut[positions] = tailLive ? 1 : 0;
    for (size_t p = positions; p-- > 0;) {
        const uint8_t e = ev[p];
        liveOut[p] = (e & kEvReadAlpha) ? 1 : ((e & kEvFullAlpha) ? 0 : liveOut[p + 1]);
    }
}

class TailAlphaPredictor {
public:
    struct ObserveResult {
        bool tripped = false;
    };

    explicit TailAlphaPredictor(uint32_t need = 60) : need_(need < 1 ? 1 : need) {}

    // Called once per picture before DropTail().
    // planned: false when PlanFrame could not plan this picture (restarts run, counts unverified if staleArmed_).
    // hrwPair: true when HRW pair machinery (SceneSnap / Snapshot / replay / own-mark) is active for this picture.
    ObserveResult Observe(bool planned, StartAlpha start, bool hrwPair) {
        if (!planned) {
            if (staleArmed_) ++unverified_;
            staleArmed_ = false;
            run_ = 0;
            return ObserveResult{false};
        }
        ++pictures_;
        if (hrwPair) ++hrwSkips_;
        // On an HRW pair picture, SceneSnap/Snapshot reads EFB alpha outside g_ev: if alpha was not cleanly established first,
        // treat it conservatively as a pre-establish read.
        const StartAlpha eff = (hrwPair && start != StartAlpha::kCleanEstablish) ? StartAlpha::kReadBeforeEstablish : start;
        if (eff == StartAlpha::kReadBeforeEstablish) {
            bool tripped = false;
            if (staleArmed_) {
                ++staleTrips_;
                off_ = true;
                tripped = true;
            }
            staleArmed_ = false;
            run_ = 0;
            return ObserveResult{tripped};
        }
        if (eff == StartAlpha::kCleanEstablish) {
            staleArmed_ = false;
            if (hrwPair) run_ = 0;
            else if (run_ < 1000000u) ++run_;
            return ObserveResult{false};
        }
        // kNeither (non-HRW): neither read nor established EFB alpha. Do not drop tail on this picture,
        // and keep staleArmed_ set so a subsequent picture that reads before establishing is still caught.
        run_ = 0;
        return ObserveResult{false};
    }

    // Call after Observe() for the picture being planned. Returns true iff g_liveA[positions] may be predicted 0.
    bool DropTail(bool hrwPair) {
        if (hrwPair || off_ || run_ < need_) return false;
        staleArmed_ = true;
        ++drops_;
        return true;
    }

    bool Off() const { return off_; }
    bool StaleArmed() const { return staleArmed_; }
    uint32_t Run() const { return run_; }
    uint32_t Need() const { return need_; }
    uint64_t Drops() const { return drops_; }
    uint64_t StaleTrips() const { return staleTrips_; }
    uint64_t Unverified() const { return unverified_; }
    uint64_t HrwSkips() const { return hrwSkips_; }
    uint64_t Pictures() const { return pictures_; }

    void Reset() {
        run_ = 0;
        staleArmed_ = false;
    }

private:
    uint32_t need_ = 60;
    uint32_t run_ = 0;
    bool staleArmed_ = false;
    bool off_ = false;
    uint64_t pictures_ = 0;
    uint64_t drops_ = 0;
    uint64_t staleTrips_ = 0;
    uint64_t unverified_ = 0;
    uint64_t hrwSkips_ = 0;
};

} // namespace maskfull
} // namespace port
