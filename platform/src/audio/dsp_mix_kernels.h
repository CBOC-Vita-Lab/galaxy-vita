// Exact integer DSP buffer operations; no mixer state, voice ordering or floating-point changes.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(PORT_DSP_NO_NEON)
#include <arm_neon.h>
#endif

namespace port::dspmix {
inline long EnvInt(const char* name, long fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::strtol(value, nullptr, 0) : fallback;
}
inline int Mode() {
    // GXV_DSPNEON: 1 uses exact NEON PCM add/scale kernels; 2 compares to scalar and falls back on mismatch. Default off.
    static const long mode = EnvInt("GXV_DSPNEON", 0);
    return mode == 1 || mode == 2 ? static_cast<int>(mode) : 0;
}
inline int16_t Clamp(int32_t value) { return static_cast<int16_t>(value < -32768 ? -32768 : value > 32767 ? 32767 : value); }
inline void AddScalar(int16_t* dst, const int16_t* src, int count, uint16_t volume) {
    for (int i = 0; i < count; ++i) dst[i] = Clamp(dst[i] + Clamp((int32_t(src[i]) * int32_t(volume)) >> 15));
}
inline void ScaleScalar(int16_t* dst, int count, uint16_t volume, int shift) {
    for (int i = 0; i < count; ++i) dst[i] = Clamp((int32_t(dst[i]) * int32_t(volume)) >> shift);
}
inline bool PartialOverlap(const int16_t* dst, const int16_t* src, int count) {
    if (count <= 0 || dst == src) return false;
    const uintptr_t a = reinterpret_cast<uintptr_t>(dst), b = reinterpret_cast<uintptr_t>(src);
    const uint64_t bytes = uint64_t(count) * sizeof(int16_t);
    return a > b ? a - b < bytes : b - a < bytes;
}

#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(PORT_DSP_NO_NEON)
inline constexpr bool kHasNeon = true;
inline void AddFast(int16_t* dst, const int16_t* src, int count, uint16_t volume) {
    if (PartialOverlap(dst, src, count)) { AddScalar(dst, src, count, volume); return; }
    int i = 0;
    for (; i + 8 <= count; i += 8) {
        const int16x8_t samples = vld1q_s16(src + i);
        // Volume is unsigned (including 0xB820); widening avoids signed-16 volume errors and preserves the full product.
        const int32x4_t lo = vmulq_n_s32(vmovl_s16(vget_low_s16(samples)), int32_t(volume));
        const int32x4_t hi = vmulq_n_s32(vmovl_s16(vget_high_s16(samples)), int32_t(volume));
        const int16x8_t scaled = vcombine_s16(vqshrn_n_s32(lo, 15), vqshrn_n_s32(hi, 15));
        vst1q_s16(dst + i, vqaddq_s16(vld1q_s16(dst + i), scaled));
    }
    AddScalar(dst + i, src + i, count - i, volume);
}
template <int Shift> inline void ScaleNeon(int16_t* dst, int count, uint16_t volume) {
    int i = 0;
    for (; i + 8 <= count; i += 8) {
        const int16x8_t samples = vld1q_s16(dst + i);
        const int32x4_t lo = vmulq_n_s32(vmovl_s16(vget_low_s16(samples)), int32_t(volume));
        const int32x4_t hi = vmulq_n_s32(vmovl_s16(vget_high_s16(samples)), int32_t(volume));
        vst1q_s16(dst + i, vcombine_s16(vqshrn_n_s32(lo, Shift), vqshrn_n_s32(hi, Shift)));
    }
    ScaleScalar(dst + i, count - i, volume, Shift);
}
inline void ScaleFast(int16_t* dst, int count, uint16_t volume, int shift) {
    if (shift == 15) ScaleNeon<15>(dst, count, volume);
    else if (shift == 12) ScaleNeon<12>(dst, count, volume);
    else ScaleScalar(dst, count, volume, shift);
}
#else
inline constexpr bool kHasNeon = false;
inline void AddFast(int16_t* dst, const int16_t* src, int count, uint16_t volume) { AddScalar(dst, src, count, volume); }
inline void ScaleFast(int16_t* dst, int count, uint16_t volume, int shift) { ScaleScalar(dst, count, volume, shift); }
#endif

// The mixer only calls these with 40/80 samples. Bigger/unusual overlapping inputs retain the old path without allocating.
inline bool AddChecked(int16_t* dst, const int16_t* src, int count, uint16_t volume) {
    if (count < 0 || count > 80 || PartialOverlap(dst, src, count)) { AddScalar(dst, src, count, volume); return true; }
    int16_t reference[80];
    std::memcpy(reference, dst, static_cast<size_t>(count) * sizeof(int16_t));
    AddScalar(reference, src, count, volume);
    AddFast(dst, src, count, volume);
    const bool match = std::memcmp(reference, dst, static_cast<size_t>(count) * sizeof(int16_t)) == 0;
    if (!match) std::memcpy(dst, reference, static_cast<size_t>(count) * sizeof(int16_t));
    return match;
}
inline bool ScaleChecked(int16_t* dst, int count, uint16_t volume, int shift) {
    if (count < 0 || count > 80) { ScaleScalar(dst, count, volume, shift); return true; }
    int16_t reference[80];
    std::memcpy(reference, dst, static_cast<size_t>(count) * sizeof(int16_t));
    ScaleScalar(reference, count, volume, shift);
    ScaleFast(dst, count, volume, shift);
    const bool match = std::memcmp(reference, dst, static_cast<size_t>(count) * sizeof(int16_t)) == 0;
    if (!match) std::memcpy(dst, reference, static_cast<size_t>(count) * sizeof(int16_t));
    return match;
}
} // namespace port::dspmix
