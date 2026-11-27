// q30fin26 (12 Nov 2026): GXV_THINFADE, the fade of the far items the cuts thin out. Pure logic (no game types); host-tested by tests/piecefade_test.cpp.
//
// Owner's rule (12 Nov 14:47): "when a step is completed, the transition is invisible. No pop in etc. Smooth fade between." Before this, a far star bit that GXV_PIECETHIN (step 3d) chose to
// hide, and a far speck that GXV_FARDRAW (step 3b) dropped, vanished in ONE picture (the Fader of lockfade.h staggers which bit goes when, but each still popped).
//
// WHAT IT DOES. An item the cut wants hidden is not stamped "not drawn" at once. It is SHRUNK through its model's base scale (J3DModel::mBaseScale, which the view pass turns into the
// draw matrices: no shader, no material, no program, no blend-mode change) over `ticks` game turns (24 = 0.4 s at the game's 60 turns a second), smooth-stepped, and only when the scale
// has reached its floor does the cut's stamp take over (the model stops drawing, as before). A bit shown again grows back the same way. An item that stops being a candidate (it moved,
// the player or the camera came near, it was picked up) grows back 4x faster: it is never left small near the player. Items the game clips (off screen) are frozen where they were.
// The table is keyed by the actor; an entry exists only while an item is between full size and hidden, or hidden. The census counts them.
#pragma once

#include <cstdint>
#include <cstring>

namespace port {
namespace piecefade {

constexpr int kSlots = 1024;      // open-addressed, power of two
constexpr int kMaxLive = 700;     // never fuller than this (probe length)
constexpr float kMinScale = 0.03f;   // the floor of the shrink (never exactly 0: a zero scale makes a singular normal matrix)

inline float Smooth(float t) { t = t < 0 ? 0 : (t > 1 ? 1 : t); return t * t * (3.0f - 2.0f * t); }
inline uint32_t HashKey(uintptr_t k) {
    uint32_t x = static_cast<uint32_t>(k >> 4) ^ static_cast<uint32_t>(static_cast<uint64_t>(k) >> 20);
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
// the scale an item at `f` of `ticks` shows
inline float ScaleAt(int f, int ticks) { return kMinScale + (1.0f - kMinScale) * Smooth(static_cast<float>(f) / static_cast<float>(ticks < 1 ? 1 : ticks)); }

struct Entry {
    uintptr_t key = 0;
    uint32_t lastTurn = 0;
    int16_t f = 0;          // 0 hidden .. ticks full size
    uint8_t state = 0;      // 0 empty, 1 used, 2 dead
    uint8_t want = 0;       // the last judgement wanted it hidden
    uint8_t done = 0;       // back at full size: the apply hook restores the base scale and erases it
    uint8_t wrote = 0;
    float base[3] = {1, 1, 1};   // the scale the item has when not faded
    float lw[3] = {1, 1, 1};     // the scale this module wrote last
};

struct Census {
    uint32_t started = 0, hiddenNow = 0, shrinking = 0, growing = 0;   // gauges (per Sample)
    uint64_t begun = 0, completedOut = 0, completedIn = 0, snapped = 0, overflow = 0, applied = 0, turnsHidden = 0;
    uint32_t maxLive = 0;
    uint32_t maxStartsPerTurn = 0, startsThisTurn = 0, curTurn = 0;
};

class Table {
public:
    void Config(int ticks, int snapMul) { ticks_ = ticks < 2 ? 2 : (ticks > 240 ? 240 : ticks); snap_ = snapMul < 1 ? 1 : snapMul; }
    int Ticks() const { return ticks_; }
    int Live() const { return live_; }
    void Clear() { std::memset(static_cast<void*>(s_), 0, sizeof s_); live_ = 0; dead_ = 0; }
    Entry* Find(uintptr_t key) {
        if (live_ == 0) return nullptr;
        uint32_t i = HashKey(key) & (kSlots - 1);
        for (int n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1)) {
            Entry& e = s_[i];
            if (e.state == 0) return nullptr;
            if (e.state == 1 && e.key == key) return &e;
        }
        return nullptr;
    }
    const Entry* Find(uintptr_t key) const { return const_cast<Table*>(this)->Find(key); }
    // the last judgement of this item wanted it hidden (the distance hysteresis keeps a fading item a candidate)
    bool Wants(uintptr_t key) const { const Entry* e = Find(key); return e && e->want; }
    // One judgement of one item, once a turn. wantHide: the cut wants it hidden; cand: it is still a candidate for the cut (far from the player and the camera, resting). Returns true:
    // the item is fully faded: the cut's stamp takes over. `created` is set when a new entry was made (the caller marks the model).
    bool Judge(uintptr_t key, bool wantHide, bool cand, uint32_t turn, bool* created = nullptr) {
        if (created) *created = false;
        if (turn != census_.curTurn) { if (census_.startsThisTurn > census_.maxStartsPerTurn) census_.maxStartsPerTurn = census_.startsThisTurn; census_.startsThisTurn = 0; census_.curTurn = turn; }
        Entry* e = Find(key);
        if (!e) {
            if (!wantHide) return false;
            e = Insert(key, turn);
            if (!e) { ++census_.overflow; return true; }   // (table full: hidden at once, as before this module)
            e->f = static_cast<int16_t>(ticks_);
            e->want = 1;
            if (created) *created = true;
            ++census_.begun;
            ++census_.startsThisTurn;
            if (static_cast<uint32_t>(live_) > census_.maxLive) census_.maxLive = static_cast<uint32_t>(live_);
        }
        int dt = static_cast<int>(turn - e->lastTurn);
        if (dt < 0 || dt > 2) dt = dt < 0 ? 0 : 2;   // (a gap, e.g. while the game clipped it: the walk goes on from where it was, never in a jump)
        e->lastTurn = turn;
        const bool wasWant = e->want != 0;
        e->want = wantHide ? 1 : 0;
        int f = e->f;
        const int f0 = f;
        if (wantHide) {
            f -= dt;
        } else {
            const int up = cand ? dt : dt * snap_;
            if (!cand && f < ticks_ && wasWant == false) ++census_.snapped;
            f += up;
        }
        if (f < 0) f = 0;
        if (f > ticks_) f = ticks_;
        e->f = static_cast<int16_t>(f);
        if (wantHide) e->done = 0;
        if (wantHide && f == 0) { ++census_.turnsHidden; if (f0 > 0) ++census_.completedOut; return true; }
        if (!wantHide && f == ticks_) { if (!e->done) ++census_.completedIn; e->done = 1; }
        return false;
    }
    // The view pass: cur = the model's base scale as it is now, changed in place to what it must be. True: write it. Erases a finished entry (after restoring the base scale);
    // `erased` says so (the caller clears the model's mark).
    bool Apply(uintptr_t key, float cur[3], bool* erased) {
        *erased = false;
        Entry* e = Find(key);
        if (!e) { *erased = true; return false; }   // (the mark was stale)
        float base[3];
        if (e->wrote && cur[0] == e->lw[0] && cur[1] == e->lw[1] && cur[2] == e->lw[2]) {
            base[0] = e->base[0]; base[1] = e->base[1]; base[2] = e->base[2];
        } else {
            base[0] = e->base[0] = cur[0]; base[1] = e->base[1] = cur[1]; base[2] = e->base[2] = cur[2];   // (the actor set a fresh scale: that is the base now)
        }
        if (e->done) {
            cur[0] = base[0]; cur[1] = base[1]; cur[2] = base[2];
            Erase(e);
            *erased = true;
            return true;
        }
        const float s = ScaleAt(e->f, ticks_);
        for (int k = 0; k < 3; ++k) { cur[k] = base[k] * s; e->lw[k] = cur[k]; }
        e->wrote = 1;
        ++census_.applied;
        return true;
    }
    // gauges for the census line
    void Sample(Census& c) const {
        c = census_;
        c.hiddenNow = c.shrinking = c.growing = 0;
        for (int i = 0; i < kSlots; ++i) {
            const Entry& e = s_[i];
            if (e.state != 1) continue;
            if (e.f == 0 && e.want) ++c.hiddenNow;
            else if (e.want) ++c.shrinking;
            else if (e.f < ticks_) ++c.growing;
        }
    }
    const Census& counts() const { return census_; }
    void ResetCounts() { const uint32_t ml = census_.maxLive; census_ = Census(); census_.maxLive = ml; }

private:
    Entry* Insert(uintptr_t key, uint32_t turn) {
        if (live_ >= kMaxLive) {
            // evict an entry that has not been judged for 10 s (its actor is gone), else refuse
            for (int i = 0; i < kSlots; ++i)
                if (s_[i].state == 1 && turn - s_[i].lastTurn > 600u) { Erase(&s_[i]); break; }
            if (live_ >= kMaxLive) return nullptr;
        }
        if (dead_ > kSlots / 4) Rehash();
        uint32_t i = HashKey(key) & (kSlots - 1);
        for (int n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1)) {
            Entry& e = s_[i];
            if (e.state == 1) continue;
            if (e.state == 2) --dead_;
            e = Entry();
            e.state = 1;
            e.key = key;
            e.lastTurn = turn;
            ++live_;
            return &e;
        }
        return nullptr;
    }
    void Erase(Entry* e) { e->state = 2; --live_; ++dead_; }
    void Rehash() {
        static Entry tmp[kSlots];   // (the game thread only)
        int n = 0;
        for (int i = 0; i < kSlots; ++i) if (s_[i].state == 1) tmp[n++] = s_[i];
        std::memset(static_cast<void*>(s_), 0, sizeof s_);
        live_ = 0; dead_ = 0;
        for (int k = 0; k < n; ++k) {
            uint32_t i = HashKey(tmp[k].key) & (kSlots - 1);
            while (s_[i].state == 1) i = (i + 1) & (kSlots - 1);
            s_[i] = tmp[k];
            ++live_;
        }
    }
    Entry s_[kSlots] = {};
    int live_ = 0, dead_ = 0, ticks_ = 24, snap_ = 4;
    Census census_;
};

}  // namespace piecefade
}  // namespace port
