// vgx - GX renderer for the Mario Strikers Charged Vita port.
// Shared small helpers. No GL and no runtime (HLE) headers in here.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace vgx {

// Comparing and copying short runs of 4-byte words (uniform blocks, matrices, shader keys)
// without going through the C library: on the console's processor a memcmp / memcpy call
// for 48 bytes costs more than the 48 bytes, and there are thousands per frame.
// SameWords(a, b, n) is memcmp(a, b, 4 * n) == 0; CopyWords(d, s, n) is memcpy(d, s, 4 * n).
// Any alignment. -DVGX_NO_NEON builds the library version on ARM too (to compare the two:
// frame hashes of an ARM run with and without it must be equal).
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(VGX_NO_NEON)
inline bool SameWords(const void* pa, const void* pb, uint32_t n) {
    const uint8_t* a = static_cast<const uint8_t*>(pa);
    const uint8_t* b = static_cast<const uint8_t*>(pb);
    for (; n >= 16; n -= 16, a += 64, b += 64) {
        const uint8x16_t d0 = veorq_u8(vld1q_u8(a), vld1q_u8(b)), d1 = veorq_u8(vld1q_u8(a + 16), vld1q_u8(b + 16));
        const uint8x16_t d2 = veorq_u8(vld1q_u8(a + 32), vld1q_u8(b + 32)), d3 = veorq_u8(vld1q_u8(a + 48), vld1q_u8(b + 48));
        const uint32x4_t d = vreinterpretq_u32_u8(vorrq_u8(vorrq_u8(d0, d1), vorrq_u8(d2, d3)));
        const uint32x2_t h = vorr_u32(vget_low_u32(d), vget_high_u32(d));
        if ((vget_lane_u32(h, 0) | vget_lane_u32(h, 1)) != 0) return false;
    }
    for (; n >= 4; n -= 4, a += 16, b += 16) {
        const uint32x4_t d = vreinterpretq_u32_u8(veorq_u8(vld1q_u8(a), vld1q_u8(b)));
        const uint32x2_t h = vorr_u32(vget_low_u32(d), vget_high_u32(d));
        if ((vget_lane_u32(h, 0) | vget_lane_u32(h, 1)) != 0) return false;
    }
    uint32_t x = 0, y = 0, z = 0;
    switch (n) { // (no loop here: a compiler turns a word loop back into a library call)
    case 3: std::memcpy(&x, a + 8, 4); std::memcpy(&y, b + 8, 4); z |= x ^ y; [[fallthrough]];
    case 2: std::memcpy(&x, a + 4, 4); std::memcpy(&y, b + 4, 4); z |= x ^ y; [[fallthrough]];
    case 1: std::memcpy(&x, a, 4); std::memcpy(&y, b, 4); z |= x ^ y; [[fallthrough]];
    default: break;
    }
    return z == 0;
}
inline void CopyWords(void* pd, const void* ps, uint32_t n) {
    uint8_t* d = static_cast<uint8_t*>(pd);
    const uint8_t* s = static_cast<const uint8_t*>(ps);
    for (; n >= 4; n -= 4, d += 16, s += 16) vst1q_u8(d, vld1q_u8(s));
    switch (n) {
    case 3: std::memcpy(d + 8, s + 8, 4); [[fallthrough]];
    case 2: std::memcpy(d + 4, s + 4, 4); [[fallthrough]];
    case 1: std::memcpy(d, s, 4); [[fallthrough]];
    default: break;
    }
}
#else
inline bool SameWords(const void* a, const void* b, uint32_t n) { return std::memcmp(a, b, static_cast<size_t>(n) * 4) == 0; }
inline void CopyWords(void* d, const void* s, uint32_t n) { std::memcpy(d, s, static_cast<size_t>(n) * 4); }
#endif
// For structs: the word version when the size allows it.
template <typename T> inline bool SameStruct(const T& a, const T& b) {
    if (sizeof(T) % 4 == 0) return SameWords(&a, &b, static_cast<uint32_t>(sizeof(T) / 4));
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
// One (unaligned) load and a byte swap: the byte-wise form was a function call of ten instructions on the console's processor.
inline __attribute__((always_inline)) uint16_t be16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return __builtin_bswap16(v); }
inline __attribute__((always_inline)) uint32_t be32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }
#else
inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
#endif
inline float bef32(const uint8_t* p) {
    uint32_t v = be32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
inline uint32_t f2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float u2f(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

inline uint32_t bits(uint32_t v, int shift, int count) { return (v >> shift) & ((1u << count) - 1u); }

// GXV_LOG=1 turns on the chatty one-time diagnostics; errors always print.
bool LogEnabled();
// size_t in a printf format: the Vita's C library does not know %zu (it misreads the arguments after it; a %s that follows can
// crash). Write "%" VGX_PRIZU in place of "%zu".
#if defined(__vita__)
#define VGX_PRIZU "u"
#else
#define VGX_PRIZU "zu"
#endif
#define VGX_LOG(...) do { if (::vgx::LogEnabled()) { std::fprintf(stderr, "[vgx] " __VA_ARGS__); } } while (0)
#define VGX_ERR(...) do { std::fprintf(stderr, "[vgx] ERROR: " __VA_ARGS__); } while (0)
// Logs a message the first `n` times this line is reached.
#define VGX_LOG_N(n, ...) do { static int vgx_cnt_ = 0; if (vgx_cnt_ < (n)) { ++vgx_cnt_; std::fprintf(stderr, "[vgx] " __VA_ARGS__); } } while (0)

inline long EnvInt(const char* name, long dflt) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::strtol(v, nullptr, 0) : dflt;
}

// Quiet builds ignore observers, not functional rendering or cache settings.
namespace diag {
#if defined(PORT_RUNTIME_DIAGNOSTICS) && !PORT_RUNTIME_DIAGNOSTICS
inline constexpr bool kEnabled = false;
inline constexpr const char* Env(const char*) { return nullptr; }
inline constexpr long Int(const char*, long fallback) { return fallback; }
#else
inline constexpr bool kEnabled = true;
inline const char* Env(const char* name) { return std::getenv(name); }
inline long Int(const char* name, long fallback) { return EnvInt(name, fallback); }
#endif
} // namespace diag

} // namespace vgx
