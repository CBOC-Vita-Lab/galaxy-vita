// (ported from Producdevity's fork) AFC ADPCM block decoding for the synchronous mixer, with the same samples and history as the original loop.
#pragma once
#include <cstdint>
#include <cstdlib>
#include "../../include/port/hot_knob.h"

namespace port::dspafc {
inline long EnvInt(const char* name, long fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 0);
    return end != value && !*end ? parsed : fallback;
}
inline int Mode() {
    // GXV_DSPAFC: 1 uses the reordered decoder; 2 compares it with the original and restores on mismatch. Default off
    // here; vita/vgx-game/entry.cpp turns it on.
    const long mode = PORT_HOT_KNOB(long, EnvInt("GXV_DSPAFC", 0));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h; every voice of every mixed sub-frame asks)
    return mode == 1 || mode == 2 ? int(mode) : 0;
}

// One sample: (nibble term + yn1 * c0 + yn2 * c1) >> 11, clamped. The sum wraps as the original's does (the platform
// builds with -fwrapv), so it can be added in any order: yn2's product leaves the chain that runs through yn1.
struct Predictor {
    int32_t c0, c1, yn1, yn2;
    inline int16_t Next(int32_t term) {
        const uint32_t partial = uint32_t(term) + uint32_t(yn2) * uint32_t(c1);
        const int32_t s = int32_t(partial + uint32_t(yn1) * uint32_t(c0)) >> 11;
        const int32_t clamped = s < -0x8000 ? -0x8000 : s > 0x7FFF ? 0x7FFF : s;
        yn2 = yn1;
        yn1 = clamped;
        return int16_t(clamped);
    }
};

// `blocks` blocks of 16 samples: a header byte (scale exponent, coefficient pair) and 8 (4-bit) or 4 (2-bit) data
// bytes. A nibble n contributes sign-extended n << 11 (4-bit) or << 13 (2-bit), times 1 << scale: the data byte's bits
// moved to the top of a signed byte, times 1 << (7 + scale).
inline void Decode(const uint8_t* src, int16_t* dst, uint32_t blocks, bool fourBit, const int16_t* coefficients,
                   int32_t& yn1, int32_t& yn2) {
    Predictor p{0, 0, yn1, yn2};
    for (uint32_t b = 0; b < blocks; b++) {
        const uint32_t header = *src++;
        p.c0 = coefficients[(header & 0xF) * 2];
        p.c1 = coefficients[(header & 0xF) * 2 + 1];
        const int32_t scale = int32_t(1) << (7 + ((header >> 4) & 0xF));
        if (fourBit) {
            for (int i = 0; i < 8; i++) {
                const uint32_t byte = *src++;
                *dst++ = p.Next(int8_t(byte & 0xF0) * scale);
                *dst++ = p.Next(int8_t(uint8_t(byte << 4)) * scale);
            }
        } else {
            for (int i = 0; i < 4; i++) {
                const uint32_t byte = *src++;
                *dst++ = p.Next(int8_t(byte & 0xC0) * scale);
                *dst++ = p.Next(int8_t(uint8_t(byte << 2) & 0xC0) * scale);
                *dst++ = p.Next(int8_t(uint8_t(byte << 4) & 0xC0) * scale);
                *dst++ = p.Next(int8_t(uint8_t(byte << 6)) * scale);
            }
        }
    }
    yn1 = p.yn1;
    yn2 = p.yn2;
}
}  // namespace port::dspafc
