// GXV_HRW (hrw27, stage e): the significance set. Pure logic (no game, no GL), host-tested by tests/hrw27_test.cpp.
//
// Of the actors in a pair's second picture, a few are drawn again with the pair's own camera and the rest keep the pose of the first picture. Shipping engines never half-rate everything
// (Unreal's animation budget allocator, Animancer's update-rate guidance): the hero, the near, the large and the fast things stay at full rate, the rest are staggered. So each pair:
//   * the protected owners (Mario, the star pointer) are always in, and cost what they cost (their own budget, counted apart);
//   * every other candidate gets a score from three things: how near it is (to Mario or the camera), how large it is on the screen, and how fast it moves on the screen times how large it is
//     (a big fast thing judders worst; a speck that moves fast does not matter; a large still thing is exact whatever its rate);
//   * a candidate that none of the three makes worth it (score under minScore) is never refreshed (it keeps its pose: far, small and slow);
//   * the rest are taken most significant first until the per-pair budget (GXV_HRW_FRESH_MS, ms beyond Mario) or the count is used up; one that is too dear for what is left is skipped
//     and a cheaper one after it may still go in;
//   * STAGGER: a candidate that was worth it but did not fit gains priority every pair it waits (its score grows), so nothing worth refreshing stays at the first picture's pose for long
//     when the budget allows, and an actor refreshed in the last pair is the first to give way when two are as significant.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace port {
namespace hrwset {

enum : uint8_t { kProtected = 1 };  // Cand::flags

struct Cand {
    uint32_t id = 0;         // the caller's handle (an index into its own list)
    float eye[3] = {};       // eye-space centre of the actor now (GX eye space: the camera looks down -z)
    float vel[3] = {};       // its eye-space centre now minus one turn ago (the camera's own motion included: what the player sees move)
    float radius = 0;        // bounding radius, world units
    float nearDist = 1e9f;   // distance to Mario (or to the camera, the smaller), world units
    float costMs = 0.1f;     // estimated cost of refreshing it (pose, view, its draws), ms
    uint8_t flags = 0;
    uint8_t stale = 0;       // pairs it has waited since it was last refreshed (0: refreshed in the last pair)
};
struct Params {
    float focalPx = 560.0f;      // the projection's focal length in picture pixels (height / (2 tan(fovy / 2)))
    float screenH = 456.0f;      // picture height, pixels
    float nearR = 900.0f;        // "near": within this many units of Mario or the camera
    float minScore = 1.0f;       // under this a candidate is not worth refreshing
    double budgetMs = 2.5;       // beyond the protected owners
    int maxN = 16;
    float staleGain = 0.75f;     // score gain per pair waited (stagger)
    uint8_t staleCap = 8;
};
struct Scored { float size = 0, speed = 0, nearS = 0, sizeS = 0, fastS = 0, score = 0; };
// projected diameter in pixels
inline float SizePx(const Cand& c, const Params& p) {
    const float z = -c.eye[2];
    if (z < 1.0f) return p.screenH;  // at or behind the camera: as large as it gets
    return 2.0f * c.radius * p.focalPx / z;
}
// screen-space speed, pixels per turn (the centre's motion in the picture plane, plus the growth of the projected size)
inline float SpeedPx(const Cand& c, const Params& p) {
    const float z = -c.eye[2];
    if (z < 1.0f) return 0.0f;
    const float zn = z - (-c.vel[2]) > 1.0f ? z - (-c.vel[2]) : z;  // depth one turn ago = z - dz/turn
    const float x0 = (c.eye[0] - c.vel[0]) / zn * p.focalPx, y0 = (c.eye[1] - c.vel[1]) / zn * p.focalPx;
    const float x1 = c.eye[0] / z * p.focalPx, y1 = c.eye[1] / z * p.focalPx;
    const float dx = x1 - x0, dy = y1 - y0;
    const float grow = std::fabs(2.0f * c.radius * p.focalPx * (1.0f / z - 1.0f / zn));
    return std::sqrt(dx * dx + dy * dy) + grow;
}
inline float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
inline Scored Score(const Cand& c, const Params& p) {
    Scored s;
    s.size = SizePx(c, p);
    s.speed = SpeedPx(c, p);
    s.nearS = 3.0f * Clamp01(1.0f - c.nearDist / p.nearR);
    s.sizeS = 3.0f * Clamp01(s.size / (p.screenH * 0.5f));
    s.fastS = 4.0f * Clamp01(s.speed / 16.0f) * Clamp01(s.size / 40.0f);
    s.score = s.nearS + s.sizeS + s.fastS;
    return s;
}
// Chooses the set. cands: n candidates (any order). out: indices into cands, most significant first (protected ones first, in their order). Returns the count; *usedMs the budget spent
// beyond the protected ones (they are not charged), *worthIt how many candidates were worth refreshing at all (the rest of them are the ones that keep their pose).
inline int Select(const Cand* cands, int n, const Params& p, int* out, int outCap, double* usedMs, int* worthIt) {
    struct Item { int i; float score; };
    Item items[512];
    int m = 0, k = 0, worth = 0;
    double used = 0;
    for (int i = 0; i < n && i < 512; ++i) {
        if (cands[i].flags & kProtected) { if (k < outCap) out[k++] = i; continue; }
        Scored s = Score(cands[i], p);
        if (s.score < p.minScore) continue;
        ++worth;
        const uint8_t w = cands[i].stale < p.staleCap ? cands[i].stale : p.staleCap;
        items[m++] = Item{i, s.score + p.staleGain * static_cast<float>(w)};
    }
    std::sort(items, items + m, [](const Item& a, const Item& b) { return a.score != b.score ? a.score > b.score : a.i < b.i; });
    int taken = 0;
    for (int j = 0; j < m && taken < p.maxN && k < outCap; ++j) {
        const double c = cands[items[j].i].costMs;
        if (used + c > p.budgetMs) continue;  // (too dear for what is left: a cheaper one further down may still fit)
        used += c;
        out[k++] = items[j].i;
        ++taken;
    }
    if (usedMs) *usedMs = used;
    if (worthIt) *worthIt = worth;
    return k;
}


// ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// hrw28: the set wired into the game. Everything below is pure logic too (no game types): the game side (decomp/src/Game/Scene/PortHrwSet.cpp) feeds it, tests/hrw28_test.cpp drives it.

// The owner id and the phase of a set member's draws (vgx_hrw.h OwnerKey: id << 8 | phase). A unit (an actor and its parts) gets an id for ONE pair: 16 .. 0xFD. The phase names the
// pass: 0x20 + the draw-buffer type for a group's opaque pass, 0x60 + the type for its translucent pass (types are under 0x40).
constexpr uint8_t kIdFirst = 16, kIdLast = 0xFD;
constexpr uint8_t kPhaseOpa = 0x20, kPhaseXlu = 0x60;
constexpr uint8_t kBufferTypeLimit = 0x40;
inline uint8_t PhaseOf(int bufferType, bool xlu) { return static_cast<uint8_t>((xlu ? kPhaseXlu : kPhaseOpa) + bufferType); }
inline bool PhaseValid(int bufferType) { return bufferType >= 0 && bufferType < kBufferTypeLimit; }

// Why a unit stays retained (counted, logged): the reasons in the order the game side tests them.
enum Reason : int {
    kRsnNone = 0,
    kRsnFamily,        // a batched family (star bits, coins, chasers, bees ...): drawn through the sorted / batched path, small, protected sprites
    kRsnNotDrawn,      // dead, clipped, hidden, no model, culled in the first picture
    kRsnNoLight,       // a packet without an actor light control: its light is the previous packet's (drawn alone it would light differently)
    kRsnJoints,        // its joints are deferred (the undrawn turn's lean work): the pose of the second picture does not exist yet
    kRsnPlayer,        // the player's own parts (drawn by the player path)
    kRsnScene,         // not the scene of the pair, or the pair is gone
    kRsnBelowMin,      // not worth refreshing: far, small and slow (the intended case)
    kRsnNoRoom,        // more candidates than the set holds (they wait: staggered)
    kRsnBudget,        // chosen, but the pair's fresh budget was spent before it (staggered: first next pair)
    kRsnViewDeferred,  // the view pass is deferred in this configuration (GXV_DRAWDONE_DEFER=4, GXV_PAR_DL_VIEW): the actors' view matrices of the second picture are not made when it starts, and cannot be (the front end reads the buffers)
    kRsnPartPass,      // a part of its drawn in a pass the set does not refresh (the unit would tear: host fresh, part old)
    kRsnCount
};
inline const char* ReasonName(int r) {
    static const char* const n[kRsnCount] = {"none", "batched family", "not drawn", "no own light", "joints deferred", "player part", "scene changed", "far/small/slow", "set full", "budget spent", "view pass deferred", "part in another pass"};
    return r >= 0 && r < kRsnCount ? n[r] : "?";
}

// The memory of the units across pairs: where each was, how many pairs it has waited, what drawing it costs. Open addressing over a fixed table; a record not seen for kForget pairs goes.
class Book {
public:
    struct Rec {
        uint64_t key = 0;        // the caller's identity of the unit (a pointer value)
        float pos[3] = {};       // world position at the last sample
        uint32_t samplePair = 0; // the pair of that sample
        uint32_t seenPair = 0;   // the last pair it was a candidate in
        uint32_t freshPair = 0;  // the last pair it was drawn fresh in (0: never)
        uint8_t stale = 0;       // pairs it waited since (worth it, not drawn)
        float costMs = 0.15f;    // running mean of the game-thread cost of its fresh draw
        bool hasPos = false;
    };
    static constexpr int kCap = 512;
    static constexpr uint32_t kForget = 16;
    Book() { Clear(); }
    void Clear() { for (Rec& r : recs_) r = Rec(); used_ = 0; }
    // the record of `key`, made when new (the oldest-seen record is replaced when the table is full)
    Rec& Touch(uint64_t key, uint32_t pair) {
        const uint32_t h = static_cast<uint32_t>((key >> 4) * 2654435761u) % kCap;
        int freeAt = -1, oldest = -1;
        for (int i = 0; i < kCap; ++i) {
            const int at = static_cast<int>((h + static_cast<uint32_t>(i)) % kCap);
            Rec& r = recs_[at];
            if (r.key == key) { r.seenPair = pair; return r; }
            if (r.key == 0) { if (freeAt < 0) freeAt = at; break; }   // (a hole ends the probe: the key is not here)
            if (oldest < 0 || r.seenPair < recs_[oldest].seenPair) oldest = at;
            if (pair - r.seenPair > kForget && freeAt < 0) freeAt = at;
        }
        const int at = freeAt >= 0 ? freeAt : (oldest >= 0 ? oldest : static_cast<int>(h));
        recs_[at] = Rec();
        recs_[at].key = key;
        recs_[at].seenPair = pair;
        ++used_;
        return recs_[at];
    }
    const Rec* Find(uint64_t key) const {
        const uint32_t h = static_cast<uint32_t>((key >> 4) * 2654435761u) % kCap;
        for (int i = 0; i < kCap; ++i) {
            const Rec& r = recs_[(h + static_cast<uint32_t>(i)) % kCap];
            if (r.key == key) return &r;
            if (r.key == 0) return nullptr;
        }
        return nullptr;
    }
    // After a pair: the records not seen for kForget pairs are dropped (the probe chains stay valid: a dropped record is kept as a tombstone with a key no caller uses)
    void Sweep(uint32_t pair) {
        for (Rec& r : recs_)
            if (r.key != 0 && r.key != kTomb && pair - r.seenPair > kForget) { r = Rec(); r.key = kTomb; }
    }
    // a record's stagger counter after a pair: drawn -> 0; worth refreshing and not drawn -> one more (saturating)
    static void Drawn(Rec& r, uint32_t pair, double costMs) {
        r.stale = 0;
        r.freshPair = pair;
        r.costMs = static_cast<float>(r.costMs * 0.75 + costMs * 0.25);
    }
    static void Waited(Rec& r) { if (r.stale < 250) ++r.stale; }
    static constexpr uint64_t kTomb = ~static_cast<uint64_t>(0);
private:
    Rec recs_[kCap];
    int used_ = 0;
};

// The running-timer loop of a pair's partial draw: the units in order (most significant first); before each one the clock is read, and when the budget is spent the loop stops, so the budget is
// overrun by at most the one unit that was in progress when it ran out. `draw(u)` draws unit u (an element of order) and returns the number of draws it made, or -1 when it could not be drawn at all (the unit is then not counted).
// `now()` is a monotonic clock in ms. Returns the units drawn; *spent the ms used.
template <class Now, class Draw>
inline int RunBudgeted(const int* order, int n, double budgetMs, Now now, Draw draw, double* spent, int* firstSkipped) {
    const double t0 = now();
    int done = 0, k = 0;
    for (; k < n; ++k) {
        if (k > 0 && now() - t0 >= budgetMs) break;   // (the first unit is always drawn: a budget of 0 still gives the most significant one; Mario is outside this budget)
        if (draw(order[k]) >= 0) ++done;  // (a unit that could not be drawn returns -1: it is skipped and the next one is tried)
    }
    if (spent) *spent = now() - t0;
    if (firstSkipped) *firstSkipped = k;
    return done;
}

// The census of one pair's partial draw, summed over the log interval.
struct PairStats {
    uint32_t pairs = 0;                 // pairs with a set
    uint32_t cands = 0, tagged = 0;     // candidates seen / units tagged in the first picture (summed)
    uint32_t refreshed = 0, stale = 0;  // units drawn fresh / tagged and kept at the first picture's pose
    uint32_t reasons[kRsnCount] = {};   // stayed retained, by reason (units, summed)
    double fresh_ms = 0, fresh_budget = 0, fresh_max = 0;  // game-thread ms spent on the set (not Mario), the budget, the largest single pair
    uint32_t overBudget = 0;            // pairs that ended over the budget (by the one unit allowed)
    uint32_t scanned = 0;               // actors visited by the candidate scan
    double scan_ms = 0;                 // the scan's own cost (first picture's turn)
    uint32_t lagBins[5] = {};           // Mario's move between the pair's pictures, screen px: <1 / <2 / <4 / <8 / >=8 (what his shadow, silhouette and reflection lag by)
    double lagMax = 0;
    uint32_t hudN = 0;                  // first pictures whose HUD block was timed (census only)
    double hud_ms = 0, hud_max = 0;     // the HUD block's draw time on the game thread (decides whether a fresh HUD would be cheap)
    void Clear() { *this = PairStats(); }
};
inline void NoteLag(PairStats& s, double px) {
    ++s.lagBins[px < 1 ? 0 : px < 2 ? 1 : px < 4 ? 2 : px < 8 ? 3 : 4];
    if (px > s.lagMax) s.lagMax = px;
}

}  // namespace hrwset
}  // namespace port
