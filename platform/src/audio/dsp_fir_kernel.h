// Fixed 80-sample voice FIR. Products must be widened before accumulation.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(PORT_DSPFIR_NO_NEON)
#include <arm_neon.h>
#endif

namespace port::dspfir {
inline long EnvInt(const char* name, long fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const long result = std::strtol(value, &end, 0);
    return end != value && *end == '\0' ? result : fallback;
}
inline int Mode() {
    // GXV_DSPFIR: 1 uses exact NEON voice FIR; 2 compares PCM/history to the original and restores on mismatch. Default off.
    static const long mode = EnvInt("GXV_DSPFIR", 0);
    return mode == 1 || mode == 2 ? static_cast<int>(mode) : 0;
}
inline int16_t Clamp(int64_t value) { return value < -32768 ? -32768 : value > 32767 ? 32767 : static_cast<int16_t>(value); }
inline void Filter(int16_t* history, const int16_t* coefficients, int16_t* buffer, int taps) {
    if (taps > 24) taps = 24;
    if (taps < 0) taps = 0;
    // Four zero-prefix samples reproduce the old omitted negative history indices at 22-24 taps.
    int16_t input[104] = {};
    std::memcpy(input + 4, history, 40);
    std::memcpy(input + 24, buffer, 160);
    int i = 0;
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(PORT_DSPFIR_NO_NEON)
    for (; i + 4 <= 80; i += 4) {
        int64x2_t lo = vdupq_n_s64(0), hi = vdupq_n_s64(0);
        for (int k = 0; k < taps; ++k) {
            const int32x4_t product = vmull_n_s16(vld1_s16(input + 24 + i - k), coefficients[k]);
            lo = vaddw_s32(lo, vget_low_s32(product));
            hi = vaddw_s32(hi, vget_high_s32(product));
        }
        // The maximum 24-tap shifted sum fits s32; final saturation is exactly the original clamp16.
        const int32x4_t shifted = vcombine_s32(vshrn_n_s64(lo, 15), vshrn_n_s64(hi, 15));
        vst1_s16(buffer + i, vqmovn_s32(shifted));
    }
#endif
    for (; i < 80; ++i) {
        int64_t acc = 0;
        for (int k = 0; k < taps; ++k) acc += int64_t(coefficients[k]) * input[24 + i - k];
        buffer[i] = Clamp(acc >> 15);
    }
    std::memcpy(history, input + 84, 40);
}
} // namespace port::dspfir
