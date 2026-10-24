import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch import sub, done

H = 'libs/RVL_SDK/include/revolution/os/OSFastCast.h'
sub(H, '''#else
#define OSf32tou16(in, out) asm volatile("psq_st   %1, 0(%0), 1, 3 " : : "b"(out), "f"(*(in)) : "memory")
#endif
#define OSu16tof32(in, out) asm volatile("psq_l   %0, 0(%1), 1, 3  " : "=f"(*(out)) : "b"(in))
''', '''#define OSu16tof32(in, out) asm volatile("psq_l   %0, 0(%1), 1, 3  " : "=f"(*(out)) : "b"(in))
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
''')

sub('src/JSystem/JParticle/JPAExtraShape.cpp', '''static void OSf32tou8(f32* f, u8* out) {
    *out = __OSf32tou8(*f);
}''', '''#ifdef __MWERKS__
static void OSf32tou8(f32* f, u8* out) {
    *out = __OSf32tou8(*f);
}
#endif''')

sub('src/nw4r/db/db_assert.cpp', '''            register u32 stackPointer;
            asm {
        mr  stackPointer, r1
            }
            stackPointer = *((u32*)stackPointer);''', '''#ifdef __MWERKS__
            register u32 stackPointer;
            asm {
        mr  stackPointer, r1
            }
            stackPointer = *((u32*)stackPointer);
#else
            uintptr_t stackPointer = 0;
#endif''')

done()
