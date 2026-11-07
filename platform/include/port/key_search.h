// GXV_LC_KEYSEARCH (logiccut, 6 Nov): the key search of J3DGetKeyFrameInterpolation (decomp/src/JSystem/J3DGraphAnimator/
// J3DAnimation.cpp) with integer compares. Header-only, so that tests/lc_keysearch.cpp runs this very code against the original.
//
// The original compares the frame with key times as floats at every step of a binary search. On the Cortex-A9 each compare is a
// load, (for s16 keys: a move to the VFP and a convert,) vcmp, and vmrs to the core's flags; the vmrs waits for the VFP pipeline,
// and every step depends on the one before. Here the frame becomes an integer once (from its bits: no VFP compare at all) and the
// steps compare integers. The answer is the same interval, so the same Hermite inputs:
//   s16 key times k (exact as floats): frame >= k  <=>  floor(frame) >= k, for any frame that is not NaN. floor(frame) is clamped to
//     [-40000, 40000], outside the s16 range, which keeps every comparison's answer.
//   f32 key times: for floats that are not NaN, a >= b  <=>  ord(a) >= ord(b), where ord(x) is x's bits for a clear sign bit and
//     minus its magnitude bits for a set one: ord(-0) == ord(+0) == 0, and the infinities order as they should.
// A NaN frame, or a NaN key time met on the way, gives up (kGaveUp): the caller runs the original search.
#pragma once

#include <cstdint>
#include <cstring>

namespace port {
namespace keysearch {

inline bool IsNaNBits(uint32_t b) { return (b & 0x7FFFFFFFu) > 0x7F800000u; }

struct KeyS16 {
    int32_t f;  // floor(frame), clamped to [-40000, 40000]
    bool Init(float frame) {
        uint32_t b;
        std::memcpy(&b, &frame, sizeof b);
        if (IsNaNBits(b)) return false;
        const uint32_t a = b & 0x7FFFFFFFu;
        const bool neg = (int32_t)b < 0;
        if (a >= 0x471C4000u) {  // |frame| >= 40000.0f
            f = neg ? -40000 : 40000;
            return true;
        }
        const int32_t e = (int32_t)(a >> 23) - 127;
        if (e < 0) {  // |frame| < 1 (zeros and denormals included)
            f = neg && a != 0 ? -1 : 0;
            return true;
        }
        const uint32_t m = (a & 0x7FFFFFu) | 0x800000u;
        const uint32_t sh = (uint32_t)(23 - e);  // 8..23 (e <= 15 here)
        const int32_t whole = (int32_t)(m >> sh);
        const bool frac = (m & ((1u << sh) - 1u)) != 0;
        f = neg ? -whole - (frac ? 1 : 0) : whole;
        return true;
    }
    static bool Key(const int16_t* p, int32_t* k) {
        *k = *p;
        return true;
    }
};

struct KeyF32 {
    int32_t f;  // ord(frame)
    static int32_t Ord(uint32_t b) { return (int32_t)b >= 0 ? (int32_t)b : -(int32_t)(b & 0x7FFFFFFFu); }
    bool Init(float frame) {
        uint32_t b;
        std::memcpy(&b, &frame, sizeof b);
        if (IsNaNBits(b)) return false;
        f = Ord(b);
        return true;
    }
    static bool Key(const float* p, int32_t* k) {
        uint32_t b;
        std::memcpy(&b, p, sizeof b);
        if (IsNaNBits(b)) return false;
        *k = Ord(b);
        return true;
    }
};

template <typename T> struct KeyOf;
template <> struct KeyOf<int16_t> { typedef KeyS16 Type; };
template <> struct KeyOf<float> { typedef KeyF32 Type; };

enum Result { kGaveUp = 0, kBeforeFirst, kAfterLast, kBetween };

// The original's search (maxFrame >= 2 keys of `stride` values: 3 for type 0, else 4). kBeforeFirst: the answer is pData[1];
// kAfterLast: pData[(maxFrame - 1) * stride + 1]; kBetween: the Hermite of the interval starting at *ppKey.
template <typename T>
inline Result Find(float frame, uint32_t maxFrame, uint32_t stride, const T* pData, const T** ppKey) {
    typename KeyOf<T>::Type kf;
    if (!kf.Init(frame)) return kGaveUp;
    int32_t k;
    if (!kf.Key(&pData[0], &k)) return kGaveUp;
    if (kf.f < k) return kBeforeFirst;  // frame < pData[0]
    const uint32_t last = (maxFrame - 1) * stride;
    if (!kf.Key(&pData[last], &k)) return kGaveUp;
    if (k <= kf.f) return kAfterLast;  // pData[last] <= frame
    uint32_t n = maxFrame;
    while (n > 1) {
        const uint32_t half = n >> 1;
        const uint32_t at = half * stride;
        if (!kf.Key(&pData[at], &k)) return kGaveUp;
        if (kf.f >= k) {  // frame >= pData[at]
            pData += at;
            n = n - half;
        } else {
            n = half;
        }
    }
    *ppKey = pData;
    return kBetween;
}

}  // namespace keysearch
}  // namespace port
