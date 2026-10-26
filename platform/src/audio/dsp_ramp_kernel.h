// Exact signed 1.31 volume ramps for the synchronous 80-sample mixer.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(PORT_DSPRAMP_NO_NEON)
#include <arm_neon.h>
#endif

namespace port::dspramp {
inline long EnvInt(const char* name, long fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 0);
    return end != value && !*end ? parsed : fallback;
}
inline int Mode() {
    // GXV_DSPRAMP: 1 uses exact NEON ramps; 2 compares PCM/final volume with the original path. Default off, read once.
    static const long mode = EnvInt("GXV_DSPRAMP", 0);
    return mode == 1 || mode == 2 ? int(mode) : 0;
}
inline bool PartialOverlap(const int16_t* dst, const int16_t* src) {
    if (dst == src) return false;
    const uintptr_t a = reinterpret_cast<uintptr_t>(dst), b = reinterpret_cast<uintptr_t>(src);
    return a > b ? a - b < 160 : b - a < 160;
}
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(PORT_DSPRAMP_NO_NEON)
inline constexpr bool kHasNeon = true;
inline int32_t AddNeon(int16_t* dst, const int16_t* src, int32_t volume, int32_t step) {
    if (!step) {
        const int16x4_t gain = vdup_n_s16(int16_t(volume >> 16));
        for (int i = 0; i < 80; i += 8) {
            const int16x8_t input = vld1q_s16(src + i);
            const int32x4_t productLo = vmull_s16(vget_low_s16(input), gain);
            const int32x4_t productHi = vmull_s16(vget_high_s16(input), gain);
            const int16x8_t scaled = vcombine_s16(vshrn_n_s32(productLo, 16), vshrn_n_s32(productHi, 16));
            vst1q_s16(dst + i, vqaddq_s16(vld1q_s16(dst + i), scaled));
        }
        return volume;
    }
    const uint32_t stride = uint32_t(step);
    const uint32_t initial[4] = {uint32_t(volume), uint32_t(volume) + stride,
        uint32_t(volume) + stride * 2u, uint32_t(volume) + stride * 3u};
    uint32x4_t lo = vld1q_u32(initial);
    uint32x4_t hi = vaddq_u32(lo, vdupq_n_u32(stride * 4u));
    const uint32x4_t advance = vdupq_n_u32(stride * 8u);
    for (int i = 0; i < 80; i += 8) {
        const int16x8_t input = vld1q_s16(src + i);
        const int32x4_t productLo = vmull_s16(vget_low_s16(input), vshrn_n_s32(vreinterpretq_s32_u32(lo), 16));
        const int32x4_t productHi = vmull_s16(vget_high_s16(input), vshrn_n_s32(vreinterpretq_s32_u32(hi), 16));
        const int16x8_t scaled = vcombine_s16(vshrn_n_s32(productLo, 16), vshrn_n_s32(productHi, 16));
        vst1q_s16(dst + i, vqaddq_s16(vld1q_s16(dst + i), scaled));
        lo = vaddq_u32(lo, advance);
        hi = vaddq_u32(hi, advance);
    }
    return vgetq_lane_s32(vreinterpretq_s32_u32(lo), 0);
}
#else
inline constexpr bool kHasNeon = false;
#endif
} // namespace port::dspramp
