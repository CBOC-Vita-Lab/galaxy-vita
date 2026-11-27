// Mario's drop shadow (CollisionShadow): which ground triangles are listed, as pure logic (shadow25, GXV_COLSHADOW_STABLE).
// Today (GXV_COLSHADOW_STABLE=0) CollisionShadow::create lists, every logic turn, the triangles of a box under Mario that pass fixed thresholds:
//   skip  facing = n . shadowDir > 0;  skip  n . camZ > 0.707;  facing within 0.15 of 0 -> the "edge" list (alpha clear), else the shadow list (minus PullBack/Glass).
// A triangle that sits near a threshold moves in and out of the set from one turn to the next (a small sphere, a far camera: many sit there), and when the
// box holds more than the cap (128) the keeper stops at the cap in its own order, so which triangles are cut changes as the box moves. Both show as a patch
// of the shadow that blinks. Stable mode: a triangle listed last turn keeps its place until it is clearly past the threshold (a band: it only ever KEEPS more
// than today, never fewer), and over the cap the nearest to the drop point are kept, with a bonus for the ones kept before.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace port {
namespace colshadow {

constexpr unsigned kSlots = 512;        // memory hash table (power of two, > 2 * 256)
constexpr float kCamEnter = 0.707f;     // as the game: skipped when n . camZ > this
constexpr float kCamStay = 0.75f;       // a triangle kept last turn stays until n . camZ > this
constexpr float kFaceStay = 0.03f;      // game: skipped when facing > 0; a kept one stays until facing > this
constexpr float kEdgeBand = 0.15f;      // game: edge list when |facing| < 0.15 (facing <= 0 here)
constexpr float kEdgeOut = 0.18f;       // a kept edge triangle stays edge until facing < -0.18
constexpr float kShadowToEdge = 0.12f;  // a kept shadow triangle stays shadow until facing > -0.12
constexpr float kKeepBonus = 20.0f;     // distance bonus (units) of a triangle kept last turn when over the cap

struct Cand {
    float camDot;     // normal . camera z direction
    float facing;     // normal . shadow direction
    float dist;       // distance of the triangle's centre to the drop point
    uint64_t key;     // identity (hash of its three vertex positions), never 0
    bool floorSkip;   // floor code PullBack / Glass
};

struct Mem {
    uint64_t key[kSlots];
    uint8_t cls[kSlots];
    uint16_t n;
    void Clear() { std::memset(this, 0, sizeof *this); }
    uint8_t Find(uint64_t k) const {  // 0 unknown, 1 shadow, 2 edge
        for (unsigned h = static_cast<unsigned>(k ^ (k >> 29)) & (kSlots - 1), i = 0; i < kSlots; ++i, h = (h + 1) & (kSlots - 1)) {
            if (key[h] == k) return cls[h];
            if (key[h] == 0) return 0;
        }
        return 0;
    }
    void Add(uint64_t k, uint8_t c) {
        if (n >= kSlots / 2) return;
        for (unsigned h = static_cast<unsigned>(k ^ (k >> 29)) & (kSlots - 1), i = 0; i < kSlots; ++i, h = (h + 1) & (kSlots - 1)) {
            if (key[h] == k) { cls[h] = c; return; }
            if (key[h] == 0) { key[h] = k; cls[h] = c; ++n; return; }
        }
    }
};

// 0 skipped, 1 shadow list, 2 edge list. prev: what the triangle was last turn (0 unknown). With prev == 0 this is exactly the game's rule.
inline uint8_t Classify(const Cand& c, uint8_t prev) {
    const bool kept = prev != 0;
    if (c.facing > (kept ? kFaceStay : 0.0f)) return 0;
    if (c.camDot > (kept ? kCamStay : kCamEnter)) return 0;
    const float bound = prev == 2 ? kEdgeOut : (prev == 1 ? kShadowToEdge : kEdgeBand);
    if (-c.facing < bound) return 2;  // (facing may be slightly positive for a kept triangle: |facing| < bound still holds)
    if (c.floorSkip) return -c.facing < kEdgeBand ? 2 : 0;  // (the game's edge list takes it when it was within 0.15 of edge-on; a kept shadow triangle in that band must not vanish)
    return 1;
}

// The game's own rule, for the reference path of the host test: no memory, everything as today.
inline void SelectLegacy(const Cand* c, unsigned n, uint8_t* cls) {
    for (unsigned i = 0; i < n; ++i) cls[i] = Classify(c[i], 0);
}

// Stable selection. cls[i] out (0/1/2); capShadow / capEdge: most triangles per list. next: the memory for the coming turn (cleared here).
// Returns the number of triangles dropped by a cap (0 = the cap was not hit).
inline unsigned SelectStable(const Cand* c, unsigned n, const Mem& prev, Mem& next, unsigned capShadow, unsigned capEdge, uint8_t* cls) {
    unsigned cnt[3] = {0, 0, 0};
    for (unsigned i = 0; i < n; ++i) {
        cls[i] = Classify(c[i], prev.Find(c[i].key));
        ++cnt[cls[i]];
    }
    unsigned dropped = 0;
    for (uint8_t k = 1; k <= 2; ++k) {
        const unsigned cap = k == 1 ? capShadow : capEdge;
        if (cnt[k] <= cap) continue;
        unsigned idx[512];
        unsigned m = 0;
        for (unsigned i = 0; i < n && m < 512; ++i) if (cls[i] == k) idx[m++] = i;
        auto score = [&](unsigned i) { return c[i].dist - (prev.Find(c[i].key) ? kKeepBonus : 0.0f); };
        std::sort(idx, idx + m, [&](unsigned a, unsigned b) {
            const float sa = score(a), sb = score(b);
            return sa != sb ? sa < sb : c[a].key < c[b].key;
        });
        for (unsigned j = cap; j < m; ++j) { cls[idx[j]] = 0; ++dropped; }
    }
    next.Clear();
    for (unsigned i = 0; i < n; ++i) if (cls[i]) next.Add(c[i].key, cls[i]);
    return dropped;
}

// The box radius feedback of CollisionShadow::create (the game: > 40 triangles in the box -10, > 10 -1, else +1). Where the next ring of tiles adds many triangles
// (10 -> 20 on a disc of big tiles) the box grows and shrinks EVERY turn and the list swings between the two sizes: the shadow's ground patch blinks.
// Stable: the same rule (grows at <= 10, shrinks by 10 above 40, shrinks by 1 in 11..40) EXCEPT that it never shrinks below the radius it last grew to
// (minOk): that radius held more than 10 triangles (the game's LARGER state) or is still being grown from. So it walks down as the game does, settles at the larger
// state and stays: never less shadow than the game shows in its larger state. (The price: it does not shrink back when the ground thins out, up to 40 triangles;
// above 40 it shrinks by 10 as the game.) m.minOk starts at 0 (no bound).
struct RadiusMem { float minOk = 0.0f; };
inline float RadiusNext(float r, unsigned boxCount, RadiusMem& m, bool stable) {
    if (boxCount > 40) return r - 10.0f;
    if (!stable) return boxCount > 10 ? r - 1.0f : r + 1.0f;
    if (boxCount > 10) return r - 1.0f >= m.minOk ? r - 1.0f : r;
    m.minOk = r + 1.0f;
    return r + 1.0f;
}

inline uint64_t Mix(uint64_t h, uint32_t v) { h ^= v; h *= 1099511628211ull; return h; }
inline uint64_t SetMix(uint64_t key) { key ^= key >> 33; key *= 0xff51afd7ed558ccdull; key ^= key >> 33; return key; }

}  // namespace colshadow
}  // namespace port
