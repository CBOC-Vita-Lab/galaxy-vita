// q30lock26 (12 Nov 2026): the transition rules shared by the Quality lock ladder (port/q30lock.h) and the Performance governors (GXV_FE_GOV / GXV_GAME_GOV, port/fegov.h). Pure logic; host-tested by
// tests/q30lock_test.cpp.
//
// Owner's rule (12 Nov 14:47): a level change, up or down, is invisible: no pop, no snap, no burst of work. The cuts that read the governors' level (port/cpulod.h tables: far material refresh,
// far pose rate, far star-bit thinning, particle thinning, the far-draw size) therefore do not switch on one picture. A Fader holds the level the cuts were at (`from`), the level they are going
// to (`to`) and a progress in per mille. Per ACTOR, the level it reads is `to` once the progress passes a per-actor threshold (a hash of its address), else `from`: over the transition's length
// the actors flip one after the other, never all on the same turn. A scalar cut (the fraction of far particles kept, the size under which far specks are left out) is interpolated by the same
// progress, monotonic, so no more than 1/length of its range changes in a turn. The tick is the game thread's turn (60 a second), the length 2 s (120 turns) unless set.
// Stepping back is the same walk the other way: a thing skipped is refreshed again progressively.
#pragma once

#include <atomic>
#include <cstdint>

namespace port {
namespace lockfade {

inline uint32_t Mix(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
// Levels are cpulod levels (Performance: 0..3) or Quality lock levels (port/q30lock.h CpuLodLevel: 0x100 | 1 | steps << 1). The union keeps every cut that either end has.
constexpr int kFlag = 0x100;
inline int Union(int a, int b) {
    if ((a & kFlag) || (b & kFlag)) return kFlag | (a & kFlag ? a & 0xFF : (a > 0 ? 1 : 0)) | (b & kFlag ? b & 0xFF : (b > 0 ? 1 : 0));
    return a > b ? a : b;
}

class Fader {
public:
    // A new target level; the walk takes `ticks` ticks. A change that arrives mid-walk starts from where the actors are: the actors that had flipped keep `to` as their new `from`... which is
    // approximated by making the old target the new start (the walk restarts from 0; no actor flips back and forth within one walk).
    void Begin(int to, int ticks) {
        const int cur = to_.load(std::memory_order_relaxed);
        if (cur == to) return;
        from_.store(prog_.load(std::memory_order_relaxed) >= 1000 ? cur : Union(from_.load(std::memory_order_relaxed), cur), std::memory_order_relaxed);
        to_.store(to, std::memory_order_relaxed);
        step_.store(ticks < 1 ? 1000 : (1000 + ticks - 1) / ticks, std::memory_order_relaxed);
        prog_.store(0, std::memory_order_release);
    }
    void Set(int level) { from_.store(level, std::memory_order_relaxed); to_.store(level, std::memory_order_relaxed); prog_.store(1000, std::memory_order_release); }
    // one tick; true while a walk is running (also the tick that completes it)
    bool Tick() {
        const int p = prog_.load(std::memory_order_relaxed);
        if (p >= 1000) return false;
        const int n = p + step_.load(std::memory_order_relaxed);
        prog_.store(n > 1000 ? 1000 : n, std::memory_order_release);
        return true;
    }
    bool Busy() const { return prog_.load(std::memory_order_acquire) < 1000; }
    int Progress() const { return prog_.load(std::memory_order_acquire); }
    int From() const { return from_.load(std::memory_order_relaxed); }
    int To() const { return to_.load(std::memory_order_relaxed); }
    // the level the cuts read when they do not know an actor: the stricter of the two ends while a walk runs
    int Level() const { return Busy() ? Union(from_.load(std::memory_order_relaxed), to_.load(std::memory_order_relaxed)) : to_.load(std::memory_order_relaxed); }
    // the level this actor reads now
    int LevelFor(uint32_t seed) const {
        const int p = prog_.load(std::memory_order_acquire);
        if (p >= 1000) return to_.load(std::memory_order_relaxed);
        return static_cast<int>(Mix(seed) % 1000u) < p ? to_.load(std::memory_order_relaxed) : from_.load(std::memory_order_relaxed);
    }
    // 0..1, monotonic from the start of a walk to its end
    float Frac() const { return static_cast<float>(prog_.load(std::memory_order_acquire)) * 0.001f; }
    // a scalar cut: its value at the `from` level and at the `to` level, interpolated
    float Lerp(float atFrom, float atTo) const { const float f = Frac(); return atFrom + (atTo - atFrom) * f; }

private:
    std::atomic<int> from_{0}, to_{0}, prog_{1000}, step_{1000};
};

// The largest share of the actors that flip in one tick, per mille (a test bound: at most step_ per mille of the actors change in a turn).
inline int MaxFlipPerTick(int ticks) { return ticks < 1 ? 1000 : (1000 + ticks - 1) / ticks; }

// A ramp for a weight that follows a target at a fixed rate per tick (the edge smoothing's depth-gate weight): monotonic toward the target, never overshoots.
class Weight {
public:
    void Target(float t) { target_ = t < 0 ? 0 : (t > 1 ? 1 : t); }
    // rate: the most it moves in a tick
    float Tick(float rate) {
        if (cur_ < target_) { cur_ += rate; if (cur_ > target_) cur_ = target_; }
        else if (cur_ > target_) { cur_ -= rate; if (cur_ < target_) cur_ = target_; }
        return cur_;
    }
    float Value() const { return cur_; }
    bool Settled() const { return cur_ == target_; }
    void Set(float v) { cur_ = target_ = v; }

private:
    float cur_ = 0.0f, target_ = 0.0f;
};

}  // namespace lockfade
}  // namespace port
