// across26 (12 Nov 2026): GXV_SILH_OCC, the silhouette gate confirmed by a hardware occlusion query. Pure logic (no GL, no clocks): host-tested by
// tests/vgx_silhocc_test.cpp. The query glue is in vgx_silhocc_gl.h, the front end's use in vgx_draw.cpp (QueueCopyTex, SilhGatePass).
//
// The problem. Every picture pays for the player's silhouette: a whole-EFB alpha capture (a scene split, ~0.6 ms 3D + 0.5 ms TA) and a full-screen fill
// quad (~0.8 ms), whether or not he is behind anything. GXV_SILH_GATE leaves both out when the game's rays (collision lines from the camera to him) find
// nothing in between. It is blind to what has no collision (decorative foliage, model-only pillars, canopies): there his silhouette would be missing.
// The cure. The silhouette's own draws (a few triangles, depth GEQUAL, colour off) are wrapped in GL_SAMPLES_PASSED. The count is how many pixels of him
// are behind something: 0 = nothing to show, whatever hides him. The result arrives 2-3 pictures later and is polled without waiting; this file turns it
// into a decision: "the gate may leave the capture and fill out in picture P" only if
//   - the results are flowing (the newest result is at most `lagMax` pictures old; otherwise fail closed: the silhouette stays),
//   - no picture in the last `hold` pictures (as far as results have arrived) showed `minPx` pixels or more,
//   - (level 1) the counter has been SEEN to count at least once since start-up (a hit happened), so a counter that never counts cannot switch the
//     silhouette off by silence. Level 2 trusts it from the start.
// The game's ray hint stays in force on top of this (it is instant for collision occluders; the query adds the rest with a 2-3 picture delay).
#pragma once

#include <atomic>
#include <cstdint>

namespace vgx {
namespace silhocc {

constexpr uint32_t kRing = 64;

struct Params {
    int hold;         // pictures after the newest hit during which the silhouette stays
    int lagMax;       // newest result older than this many pictures: fail closed
    uint32_t minPx;   // samples that count as "he shows"
    bool needProof;   // a hit must have been seen once
};
// level 1: careful (hold 12, proof needed); level 2: hold 5, no proof; 3 (census) behaves as 1 but changes nothing (the caller does not act on it)
inline Params ParamsFor(int level, uint32_t minPx = 1) {
    Params p{12, 12, minPx ? minPx : 1u, true};
    if (level == 2) { p.hold = 5; p.needProof = false; }
    return p;
}

enum Why : int { kAllowed = 0, kNoProof, kNoFlow, kHold, kWhyCount };
inline const char* WhyName(int w) {
    static const char* const n[kWhyCount] = {"allowed", "no hit seen yet (level 1)", "results not flowing", "a hit within the hold"};
    return w >= 0 && w < kWhyCount ? n[w] : "?";
}

struct History {
    std::atomic<uint64_t> lastHit{0};   // frame + 1 of the newest picture whose silhouette showed >= minPx samples (0: none)
    std::atomic<uint64_t> lastSeen{0};  // frame + 1 of the newest picture whose result arrived (0: none)
    std::atomic<uint32_t> hits{0};      // pictures with a hit, ever
    // per-picture note written by the front end: tag (frame low 24 bits) << 8 | 0x80 valid | rawHint << 4 | downgraded << 3 | gated << 2
    std::atomic<uint32_t> ring[kRing];
    // census: results by raw hint (0 none, 1 cannot show, 2 may show) x hit; gated pictures with a result and the ones that showed (the misses);
    // decisions by Why
    std::atomic<uint64_t> byHint[3][2];
    std::atomic<uint64_t> gatedResults{0}, gatedShowed{0}, unmatched{0}, results{0}, samplesSum{0};
    std::atomic<uint64_t> decisions[kWhyCount];
    std::atomic<uint64_t> glueBegan{0}, gluePolled{0}, glueNoSlot{0}, glueForced{0}; // (the back end's totals, stored once a picture)

    History() { Reset(); }
    void Reset() {
        lastHit.store(0); lastSeen.store(0); hits.store(0);
        for (auto& r : ring) r.store(0);
        for (auto& h : byHint) for (auto& c : h) c.store(0);
        gatedResults.store(0); gatedShowed.store(0); unmatched.store(0); results.store(0); samplesSum.store(0);
        for (auto& d : decisions) d.store(0);
        glueBegan.store(0); gluePolled.store(0); glueNoSlot.store(0); glueForced.store(0);
    }
    static uint32_t Tag(uint64_t frame) { return static_cast<uint32_t>(frame & 0xFFFFFFu); }
    // Front end, once a picture (at its capture): what the game's hint was and whether the hint was downgraded to "may show" by this very gate.
    void NoteFrame(uint64_t frame, int rawHint, bool downgraded) {
        const uint32_t w = Tag(frame) << 8 | 0x80u | static_cast<uint32_t>(rawHint & 3) << 4 | (downgraded ? 8u : 0u);
        ring[frame % kRing].store(w, std::memory_order_release);
    }
    // Front end: the picture's capture and fill were left out.
    void NoteGated(uint64_t frame) {
        std::atomic<uint32_t>& r = ring[frame % kRing];
        uint32_t w = r.load(std::memory_order_acquire);
        if ((w & 0x80u) && (w >> 8) == Tag(frame)) r.store(w | 4u, std::memory_order_release);
    }
    // Back end: the count of one picture's silhouette draws arrived.
    void OnResult(uint64_t frame, uint32_t samples, uint32_t minPx) {
        results.fetch_add(1, std::memory_order_relaxed);
        samplesSum.fetch_add(samples, std::memory_order_relaxed);
        const bool hit = samples >= (minPx ? minPx : 1u);
        if (hit) {
            hits.fetch_add(1, std::memory_order_relaxed);
            uint64_t o = lastHit.load(std::memory_order_relaxed);
            while (o < frame + 1 && !lastHit.compare_exchange_weak(o, frame + 1, std::memory_order_release)) {}
        }
        uint64_t s = lastSeen.load(std::memory_order_relaxed);
        while (s < frame + 1 && !lastSeen.compare_exchange_weak(s, frame + 1, std::memory_order_release)) {}
        const uint32_t w = ring[frame % kRing].load(std::memory_order_acquire);
        if (!(w & 0x80u) || (w >> 8) != Tag(frame)) { unmatched.fetch_add(1, std::memory_order_relaxed); return; }
        uint32_t h = (w >> 4) & 3u;
        if (h > 2) h = 2;
        byHint[h][hit ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
        if (w & 4u) {
            gatedResults.fetch_add(1, std::memory_order_relaxed);
            if (hit) gatedShowed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Front end: may the capture and fill of picture `frame` be left out (the game's hint already says "cannot show")?
    int Check(uint64_t frame, const Params& p) const {
        const uint64_t seen = lastSeen.load(std::memory_order_acquire), hit = lastHit.load(std::memory_order_acquire);
        if (p.needProof && hits.load(std::memory_order_relaxed) == 0) return kNoProof;
        if (seen == 0 || frame + 1 > seen + static_cast<uint64_t>(p.lagMax)) return kNoFlow;
        if (hit && frame + 1 <= hit + static_cast<uint64_t>(p.hold)) return kHold;
        return kAllowed;
    }
    bool Allows(uint64_t frame, const Params& p) {
        const int w = Check(frame, p);
        decisions[w].fetch_add(1, std::memory_order_relaxed);
        return w == kAllowed;
    }
};

}  // namespace silhocc
}  // namespace vgx
