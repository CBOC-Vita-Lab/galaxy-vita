// (ported from Producdevity's fork) Exact 4-tap voice resampler for the synchronous 80-sample mixer.
#pragma once
#include <cstdint>
#include <cstdlib>
#include "../../include/port/hot_knob.h"
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(PORT_DSPRESAMPLE_NO_NEON)
#include <arm_neon.h>
#endif

namespace port::dspresample {
inline long EnvInt(const char* name, long fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 0);
    return end != value && !*end ? parsed : fallback;
}
inline int Mode() {
    // GXV_DSPRESAMPLE: 1 uses the exact NEON 4-tap resampler; 2 compares it with the original and restores on mismatch.
    // Default off here; vita/vgx-game/entry.cpp turns it on.
    const long mode = PORT_HOT_KNOB(long, EnvInt("GXV_DSPRESAMPLE", 0));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h; every voice of every mixed sub-frame asks)
    return mode == 1 || mode == 2 ? int(mode) : 0;
}

// 80 outputs from src at 4.12 position `pos`, stepping `ratio`; table: 64 phases of 4 coefficients. Returns the end position.
inline uint32_t FourTapScalar(const int16_t* table, const int16_t* src, int16_t* dst, uint32_t pos, uint32_t ratio) {
    for (int i = 0; i < 80; i++) {
        const int16_t* c = &table[((pos & 0xFFF) >> 6) * 4];
        const int16_t* in = &src[pos >> 12];
        int64_t acc = 0;
        for (int k = 0; k < 4; k++) acc += (int64_t)2 * c[k] * in[k];
        const int32_t s = (int32_t)(acc >> 16);
        dst[i] = (int16_t)(s < -0x8000 ? -0x8000 : s > 0x7FFF ? 0x7FFF : s);
        pos += ratio;
    }
    return pos;
}

#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(PORT_DSPRESAMPLE_NO_NEON)
inline constexpr bool kHasNeon = true;
// (2 * sum) >> 16 is sum >> 15. The four products fit 32 bits, their sum needs 33: pairs are widened before adding.
inline int64x1_t Dot4(const int16_t* table, const int16_t* src, uint32_t pos) {
    const int64x2_t pairs = vpaddlq_s32(vmull_s16(vld1_s16(table + ((pos & 0xFFF) >> 6) * 4), vld1_s16(src + (pos >> 12))));
    return vadd_s64(vget_low_s64(pairs), vget_high_s64(pairs));
}
inline uint32_t FourTap(const int16_t* table, const int16_t* src, int16_t* dst, uint32_t pos, uint32_t ratio) {
    for (int i = 0; i < 80; i += 4) {
        const int64x1_t a = Dot4(table, src, pos), b = Dot4(table, src, pos + ratio);
        const int64x1_t c = Dot4(table, src, pos + ratio * 2), d = Dot4(table, src, pos + ratio * 3);
        pos += ratio * 4;
        const int32x4_t sums = vcombine_s32(vqshrn_n_s64(vcombine_s64(a, b), 15), vqshrn_n_s64(vcombine_s64(c, d), 15));
        vst1_s16(dst + i, vqmovn_s32(sums));
    }
    return pos;
}
#else
inline constexpr bool kHasNeon = false;
inline uint32_t FourTap(const int16_t* table, const int16_t* src, int16_t* dst, uint32_t pos, uint32_t ratio) {
    return FourTapScalar(table, src, dst, pos, ratio);
}
#endif
}  // namespace port::dspresample
