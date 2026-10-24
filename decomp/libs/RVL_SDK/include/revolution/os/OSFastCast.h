#ifndef OSFASTCAST_H
#define OSFASTCAST_H

#include "revolution/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __MWERKS__
static inline u16 __OSf32tou16(register f32 in) {
    f32 a;
    register f32* ptr = &a;
    register u16 r;
    asm {
        psq_st in, 0(ptr), 1, 3
        lhz r, 0(ptr)
    }
    return r;
}

static inline void OSf32tou16(register f32* in, volatile register u16* out) {
    *out = __OSf32tou16(*in);
}
#define OSu16tof32(in, out) asm volatile("psq_l   %0, 0(%1), 1, 3  " : "=f"(*(out)) : "b"(in))
#else
/* Quantized paired-single stores clamp to the integer type's range and truncate. */
static inline u8 __OSf32tou8(f32 in) {
    if (!(in > 0.0f)) return 0;
    if (in >= 255.0f) return 255;
    return (u8)in;
}
static inline s8 __OSf32tos8(f32 in) {
    if (in != in) return 0;
    if (in <= -128.0f) return -128;
    if (in >= 127.0f) return 127;
    return (s8)in;
}
static inline u16 __OSf32tou16(f32 in) {
    if (!(in > 0.0f)) return 0;
    if (in >= 65535.0f) return 65535;
    return (u16)in;
}
static inline s16 __OSf32tos16(f32 in) {
    if (in != in) return 0;
    if (in <= -32768.0f) return -32768;
    if (in >= 32767.0f) return 32767;
    return (s16)in;
}
static inline void OSf32tou8(const f32* in, volatile u8* out) { *out = __OSf32tou8(*in); }
static inline void OSf32tos8(const f32* in, volatile s8* out) { *out = __OSf32tos8(*in); }
static inline void OSf32tou16(const f32* in, volatile u16* out) { *out = __OSf32tou16(*in); }
static inline void OSf32tos16(const f32* in, volatile s16* out) { *out = __OSf32tos16(*in); }
static inline void OSu8tof32(const u8* in, volatile f32* out) { *out = (f32)*in; }
static inline void OSs8tof32(const s8* in, volatile f32* out) { *out = (f32)*in; }
static inline void OSu16tof32(const u16* in, volatile f32* out) { *out = (f32)*in; }
static inline void OSs16tof32(const s16* in, volatile f32* out) { *out = (f32)*in; }
#endif

static inline void OSInitFastCast(void) {
#ifdef __MWERKS__
    asm
    {
        li      r3, 4
        oris    r3, r3, 4
        mtspr   0x392, r3

        li      r3, 5
        oris    r3, r3, 5
        mtspr   0x393, r3

        li      r3, 6
        oris    r3, r3, 6
        mtspr   0x394, r3

        li      r3, 7
        oris    r3, r3, 7
        mtspr   0x395, r3
    }
#endif
}

#ifdef __cplusplus
}
#endif

#endif  // OSFASTCAST_H
