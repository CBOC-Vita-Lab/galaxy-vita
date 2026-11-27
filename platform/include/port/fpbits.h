// neonbox40 (21 Nov 2026): float compares without the VFP flag transfer. On the Cortex-A9 every scalar `vcmp.f32` + `vmrs APSR_nzcv` stalls
// the pipeline (FP-AUDIT.md: the biggest cluster of them in hot code is the front end's box tests). The decisions below are made on
// integer bits instead, and are bit-identical to the plain float expressions for EVERY input (NaN, infinities, +-0, denormals):
//   * a > b  <=>  (a - b) > 0.  With gradual underflow (FPSCR.FZ off, which this port never sets, and SSE without FTZ on the host) a - b is zero
//     only when a == b (+-0 included), never changes sign, overflows to the right infinity, and is NaN when either is NaN or for inf - inf
//     (where a > b, a >= b are... see Ge: inf >= inf is TRUE but inf - inf is NaN: Ge/Le handle that, below). So the sign and class of
//     d = a - b are read from its bits: no vcmp.
//   * NEON is NOT used: Advanced SIMD flushes denormal operands to zero, which would change a decision for a denormal (the scalar VFP does
//     not), so "bit-identical" would only hold "except denormals". The integer form is exact and needs no emulator to argue it.
//   * floor / ceil to int: convert toward zero (vcvt, no flags), then correct by the sign of the remainder x - (float)i, which is exact.
//     Range proven below; outside it the original libm expression runs.
// `Ref` is the plain float code (the old behaviour, GALAXY_FPBITS=0 builds it, and host tests compare against it); `Fast` the flag-free one.
// `Active` is what the game code uses.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#ifndef GALAXY_FPBITS
#define GALAXY_FPBITS 1
#endif

namespace fpb {
inline uint32_t Bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float FromBits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

struct Ref {
    static inline bool Gt(float a, float b) { return a > b; }
    static inline bool Ge(float a, float b) { return a >= b; }
    static inline bool Lt(float a, float b) { return a < b; }
    static inline bool Le(float a, float b) { return a <= b; }
    static inline bool GtZ(float a) { return a > 0.0f; }
    static inline float Min(float a, float b) { return b < a ? b : a; } // (std::min)
    static inline float Max(float a, float b) { return a < b ? b : a; } // (std::max)
    static inline int FloorI(float x) { return static_cast<int>(std::floor(x)); }
    static inline int CeilI(float x) { return static_cast<int>(std::ceil(x)); }
};

struct Fast {
    // d's class from its bits (d is a - b)
    static inline bool PosZ(uint32_t d) { return (d - 1u) < 0x7F7FFFFFu; }                    // d > 0: +denormal .. +max (not +0, not inf, not NaN)
    static inline bool PosZInf(uint32_t d) { return (d - 1u) < 0x7F800000u; }                 // d > 0 incl. +inf
    static inline bool NegZInf(uint32_t d) { return (d - 0x80000001u) < 0x7F800000u; }        // d < 0 incl. -inf
    // a > b: d = a - b. inf > inf: false (d NaN); inf > finite: d = +inf: true. finite > inf: -inf: false.
    static inline bool Gt(float a, float b) { return PosZInf(Bits(a - b)); }
    static inline bool Lt(float a, float b) { return NegZInf(Bits(a - b)); }
    // a >= b: d >= 0, or both are the same infinity (d NaN). a - b is NaN for NaN operands too, so the equal-infinity case is tested on bits.
    static inline bool Ge(float a, float b) {
        const uint32_t d = Bits(a - b);
        return (d <= 0x7F800000u) | (d == 0x80000000u) | ((Bits(a) == Bits(b)) & ((Bits(a) & 0x7FFFFFFFu) == 0x7F800000u));
    }
    static inline bool Le(float a, float b) {
        const uint32_t d = Bits(a - b);
        return (d >= 0x80000000u && d <= 0xFF800000u) | (d == 0u) | (d == 0x80000000u) | ((Bits(a) == Bits(b)) & ((Bits(a) & 0x7FFFFFFFu) == 0x7F800000u));
    }
    static inline bool GtZ(float a) { return PosZInf(Bits(a)); }
    // Min / Max: the plain select on the flag-free compare (NaN: b < a false, so `a`, as std::min / std::max).
    static inline float Min(float a, float b) { return Lt(b, a) ? b : a; }
    static inline float Max(float a, float b) { return Lt(a, b) ? b : a; }
    // floor / ceil as int, |x| < 2^23 (bits(|x|) < 0x4B000000; a NaN, an infinity or a larger value takes the libm expression). There x - (float)i,
    // i = trunc(x), is exact (|i| < 2^23, the remainder is the fraction, finitely many bits below x's own): negative non-zero: floor is i - 1;
    // positive non-zero: ceil is i + 1. x = -0: i = 0, d = -0 (bits 0x80000000, not "negative non-zero"): floor 0, as floor(-0) = -0 -> 0.
    static inline int FloorI(float x) {
        if (__builtin_expect((Bits(x) & 0x7FFFFFFFu) < 0x4B000000u, 1)) {
            const int i = static_cast<int>(x);
            return i - static_cast<int>(NegZInf(Bits(x - static_cast<float>(i))));
        }
        return static_cast<int>(std::floor(x));
    }
    static inline int CeilI(float x) {
        if (__builtin_expect((Bits(x) & 0x7FFFFFFFu) < 0x4B000000u, 1)) {
            const int i = static_cast<int>(x);
            return i + static_cast<int>(PosZInf(Bits(x - static_cast<float>(i))));
        }
        return static_cast<int>(std::ceil(x));
    }
};

#if GALAXY_FPBITS
using Active = Fast;
#else
using Active = Ref;
#endif
} // namespace fpb
