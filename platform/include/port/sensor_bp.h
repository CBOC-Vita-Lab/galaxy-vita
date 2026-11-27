// GXV_SENSOR_BP (update23, 8 Nov; exact): a broad phase for SensorHitChecker::movement's 14 pair loops (GXV_HL_SENSOR's copies).
//
// The original tests every pair of two groups (and the character group against itself, i <= x), in a fixed order, with
//     hit  <=>  !( fl( fl( fl(dx*dx) + fl(dy*dy) ) + fl(dz*dz) ) >= fl(T*T) ),  dx = fl(x1 - x2) ..., T = fl(r1 + r2)
// and a hit appends to the two sensors' hit lists (HitSensor::addHitSensor, capped). The ORDER of the hits is the result: a list
// that is full drops the later ones, and the hosts read the lists in order. Here a pair that cannot hit is never looked at, and
// the pairs that can are looked at in exactly the original order, with the original test (the caller's), so the sequence of
// addHitSensor calls is the original's, element for element.
//
// Which pairs cannot hit (the proof; u = 2^-24, round to nearest, no overflow: every |coordinate| and radius <= kMaxAbs = 1e9, so
// every square and sum stays below 1.3e19 << FLT_MAX; NaN or a value out of range sends the whole turn to the plain loops):
//   - Rounding is monotone and the summands are >= 0, so the sum S >= fl(da*da) for EACH axis a (fl(p + q) >= fl(p) = p for q >= 0).
//   - hit  =>  da_f^2 (1 - u) <= fl(da_f^2) <= S < fl(T^2) <= T^2 (1 + u)  =>  |da_f| < T (1 + 1.01 u)
//     da_f = (a1 - a2)(1 + d), |d| <= u  =>  |a1 - a2| < T (1 + 1.01 u) / (1 - u)  and  T = fl(r1 + r2) <= (r1 + r2)(1 + u)
//     =>  |a1 - a2| < (r1 + r2)(1 + 4 u).  (A flush-to-zero da_f only happens for |a1 - a2| < 1.2e-38: inside anyway.)
//   - So two sensors whose intervals [a - r (1 + 4u), a + r (1 + 4u)] on ONE axis do not overlap cannot hit. The intervals used are
//     wider: [fl(a - R), fl(a + R)] with R = fl(r (1 + 1e-4) + (|a| 1e-6 + 1e-2)); the extra |a| 1e-6 + r 1e-4 + 1e-2 is far above the
//     float rounding of R and of the two ends (each <= u (|a| + R)), so each computed interval contains the exact one.
//   The axis is the one along which this turn's sensors spread most (any axis is valid by the argument above).
// The interval ends are compared as integers (a float's bits mapped to an order-preserving int32): on the console's Cortex-A9 every
// float compare that branches costs a vcmp + vmrs pipeline stall, which is most of what a pair test costs; the broad phase's own
// search and scan must not pay it.
// For each sensor of the outer list (in order) the inner list's candidates are found by a binary search on the inner list sorted
// by interval start (inner intervals are at most maxW wide; the search starts at qlo - maxW, widened the same way), then put back
// into index order and tested with the original test. For the same group, only candidates with index >= i (the original's x >= i).
// Small loops (outer * inner < kBruteBelow) run as before: the order of every loop is kept either way.
//
// No state survives a call; the scratch buffers belong to the caller (game thread only). PORT_SENSOR_BP_NEG (host test only)
// shrinks the margin to prove the test catches a too-narrow broad phase.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <climits>

namespace port {
namespace sensorbp {

struct Key {
    int32_t lo, hi;  // the interval's ends, order-preserving ints (OrderKey)
    int32_t idx;
};

struct Scratch {
    Key* keys = nullptr;      // all groups' keys, group after group (sorted per group)
    int32_t keyCap = 0;
    int32_t* cand = nullptr;  // one outer sensor's candidates
    int32_t candCap = 0;
};

static const float kMaxAbs = 1.0e9f;
#ifdef PORT_SENSOR_BP_NEG
static const float kRel = -2.0e-3f;  // (negative control: too narrow)
static const float kPos = 0.0f;
static const float kAbs = 0.0f;
#else
static const float kRel = 1.0e-4f;
static const float kPos = 1.0e-6f;
static const float kAbs = 1.0e-2f;
#endif
static const int32_t kBruteBelow = 48;  // outer * inner pairs below which a loop runs plainly

struct Group {
    const Key* keys;  // sorted by lo
    int32_t n;
    float maxW;       // the widest interval (hi - lo as floats)
};

inline uint32_t Bits(float f) {
    uint32_t u;
    __builtin_memcpy(&u, &f, 4);
    return u;
}
// The float's order as a signed int: a < b (floats, not NaN) <=> OrderKey(a) < OrderKey(b), a == b <=> equal keys (-0 and +0: 0).
inline int32_t OrderKey(float f) {
    const uint32_t u = Bits(f);
    const int32_t m = static_cast< int32_t >(u & 0x7FFFFFFFu);
    return (u >> 31) != 0 ? -m : m;
}
// |f| <= kMaxAbs and not NaN, without a float compare
inline bool InRange(float f) {
    return (Bits(f) & 0x7FFFFFFFu) <= Bits(kMaxAbs);
}

// Grows a buffer (host heap; never freed, as GXV_HL_SENSOR's): false if there is no room.
template < class T > inline bool Grow(T*& rBuf, int32_t& rCap, int32_t need) {
    if (need <= rCap) {
        return true;
    }
    T* p = static_cast< T* >(std::realloc(rBuf, sizeof(T) * static_cast< size_t >(need + 64)));
    if (p == nullptr) {
        return false;
    }
    rBuf = p;
    rCap = need + 64;
    return true;
}

// The axis along which the sensors spread most (0 x, 1 y, 2 z); -1 if a value is NaN, infinite or out of range or a radius is
// negative (=> plain loops).
template < class S > inline int ChooseAxis(const S* const* lists, const int32_t* counts, int nLists) {
    int32_t lo[3] = {INT32_MAX, INT32_MAX, INT32_MAX}, hi[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
    for (int g = 0; g < nLists; g++) {
        for (int32_t i = 0; i < counts[g]; i++) {
            const S& s = lists[g][i];
            const float v[3] = {s.x, s.y, s.z};
            for (int a = 0; a < 3; a++) {
                if (!InRange(v[a])) {
                    return -1;
                }
                const int32_t k = OrderKey(v[a]);
                lo[a] = k < lo[a] ? k : lo[a];
                hi[a] = k > hi[a] ? k : hi[a];
            }
            if (!InRange(s.r) || (Bits(s.r) >> 31) != 0) {  // (a negative radius, -0 included: plain loops)
                return -1;
            }
        }
    }
    // extents from the extreme keys (any axis is correct; this only picks the most selective one)
    float ext[3];
    for (int a = 0; a < 3; a++) {
        if (lo[a] > hi[a]) {
            return 0;  // (no sensors)
        }
        const int32_t kl = lo[a], kh = hi[a];
        const uint32_t ul = kl >= 0 ? static_cast< uint32_t >(kl) : static_cast< uint32_t >(-kl) | 0x80000000u;
        const uint32_t uh = kh >= 0 ? static_cast< uint32_t >(kh) : static_cast< uint32_t >(-kh) | 0x80000000u;
        float fl, fh;
        __builtin_memcpy(&fl, &ul, 4);
        __builtin_memcpy(&fh, &uh, 4);
        ext[a] = fh - fl;
    }
    return ext[1] > ext[0] ? (ext[2] > ext[1] ? 2 : 1) : (ext[2] > ext[0] ? 2 : 0);
}

template < class S > inline float AxisOf(const S& s, int axis) {
    return axis == 0 ? s.x : axis == 1 ? s.y : s.z;
}

// R for a sensor at axis value c with radius r (see the proof above)
inline float Inflate(float c, float r) {
    return r * (1.0f + kRel) + (__builtin_fabsf(c) * kPos + kAbs);
}

// One group's keys, sorted by lo (ties by index: any order would do, this one is deterministic).
template < class S > inline Group BuildGroup(const S* list, int32_t n, int axis, Key* out) {
    float maxW = 0.0f;
    for (int32_t i = 0; i < n; i++) {
        const float c = AxisOf(list[i], axis);
        const float R = Inflate(c, list[i].r);
        const float lo = c - R, hi = c + R;
        out[i].lo = OrderKey(lo);
        out[i].hi = OrderKey(hi);
        out[i].idx = i;
        const float w = hi - lo;
        maxW = w > maxW ? w : maxW;  // (the one float compare a sensor)
    }
    std::sort(out, out + n, [](const Key& a, const Key& b) { return a.lo < b.lo || (a.lo == b.lo && a.idx < b.idx); });
    Group g;
    g.keys = out;
    g.n = n;
    g.maxW = maxW;
    return g;
}

// The inner group's sensors whose intervals overlap [qlo, qhi] (floats), with index >= minIdx, in index order, into cand: their count.
inline int32_t Candidates(const Group& g, float qlo, float qhi, int32_t minIdx, int32_t* cand) {
    // A key that starts before qlo - maxW ends before qlo. The start is widened as R is (rounding of the subtraction, maxW's own).
    const float from = qlo - (g.maxW * (1.0f + kRel) + (__builtin_fabsf(qlo) * kPos + kAbs));
    const int32_t kFrom = OrderKey(from), kLo = OrderKey(qlo), kHi = OrderKey(qhi);
    int32_t l = 0, h = g.n;
    while (l < h) {
        const int32_t m = (l + h) >> 1;
        if (g.keys[m].lo < kFrom) {
            l = m + 1;
        } else {
            h = m;
        }
    }
    int32_t n = 0;
    for (int32_t k = l; k < g.n && g.keys[k].lo <= kHi; k++) {
        const Key& key = g.keys[k];
        if (key.hi >= kLo && key.idx >= minIdx) {
            // insertion into index order (few candidates)
            int32_t j = n++;
            while (j > 0 && cand[j - 1] > key.idx) {
                cand[j] = cand[j - 1];
                --j;
            }
            cand[j] = key.idx;
        }
    }
    return n;
}

// doObjColGroup on the copies: for each sensor of list1 in order, the candidates of list2 in index order, each tested by attack.
template < class S, class F >
inline void Groups(const S* list1, int32_t n1, const S* list2, const Group& g2, int axis, int32_t* cand, F&& attack, uint32_t& rTested) {
    if (n1 == 0 || g2.n == 0) {
        return;
    }
    if (n1 * g2.n < kBruteBelow) {
        for (int32_t i = 0; i < n1; i++) {
#pragma clang loop vectorize(disable)
            for (int32_t x = 0; x < g2.n; x++) {
                attack(list1[i], list2[x]);
            }
        }
        rTested += static_cast< uint32_t >(n1 * g2.n);
        return;
    }
    for (int32_t i = 0; i < n1; i++) {
        const float c = AxisOf(list1[i], axis);
        const float R = Inflate(c, list1[i].r);
        const int32_t nc = Candidates(g2, c - R, c + R, 0, cand);
        for (int32_t k = 0; k < nc; k++) {
            attack(list1[i], list2[cand[k]]);
        }
        rTested += static_cast< uint32_t >(nc);
    }
}

// doObjColInSameGroup on the copies (x >= i).
template < class S, class F >
inline void SameGroup(const S* list, const Group& g, int axis, int32_t* cand, F&& attack, uint32_t& rTested) {
    const int32_t n = g.n;
    if (n * (n + 1) / 2 < kBruteBelow) {
        for (int32_t i = 0; i < n; i++) {
#pragma clang loop vectorize(disable)
            for (int32_t x = i; x < n; x++) {
                attack(list[i], list[x]);
            }
        }
        rTested += static_cast< uint32_t >(n * (n + 1) / 2);
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        const float c = AxisOf(list[i], axis);
        const float R = Inflate(c, list[i].r);
        const int32_t nc = Candidates(g, c - R, c + R, i, cand);
        for (int32_t k = 0; k < nc; k++) {
            attack(list[i], list[cand[k]]);
        }
        rTested += static_cast< uint32_t >(nc);
    }
}

}  // namespace sensorbp
}  // namespace port
