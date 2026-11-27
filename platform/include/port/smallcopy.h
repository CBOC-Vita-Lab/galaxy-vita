// copy49 (22 Nov 2026), GXV_SMALLCOPY: inline copies and fills for small sizes.
// The console's memcpy (newlib's memcpy-stub.o) is a single tail jump to the system call sceClibMemcpy, which costs about 190 ns a call whatever
// the size (the Strikers port measured it; NFSUG2 sends copies under 1 KiB to inline NEON code, include/platform/smallcopy.h of
// github.com/danos-x5/NFSUG2_VITA). This header is the port's own version, written from scratch for the game build: header-only, no
// dependency on the SDK, so the host test (tests/copy49_smallcopy_test.cpp) compiles the same code on x86.
//
// Every routine is exact (a byte-for-byte memcpy / memmove / memset):
//   Move128 (1 <= n <= 128): every byte is LOADED into registers before any byte is stored, in 1 to 8 overlapping chunks (head and tail), so it
//     is a correct memmove for any overlap of source and destination, and so a correct memcpy too.
//   CopyFwd (n >= 64): 64-byte steps forward and one overlapping 64-byte tail; correct ONLY when the two ranges do not overlap (the caller
//     checks Overlaps() and sends an overlapping copy to the real memmove).
//   Fill (n >= 1): stores only; head and tail chunks overlap each other, which writes the same value twice.
// Alignment: the chunks are 16- and 8-byte vectors declared aligned(1) (NEON vld1.8 / vst1.8, which have no alignment requirement on a
// Cortex-A9) and 4-byte words declared aligned(1) (the compiler emits LDR only where it knows unaligned LDR is allowed: the game build has
// -munaligned-access; otherwise it builds the word from bytes). No LDRD / LDM / STM is ever formed from these types.
// The translation unit that includes this must be compiled with -fno-builtin (and, for GCC, -fno-tree-loop-distribute-patterns): the 64-byte
// loops must not be turned back into a call to memcpy, which is the function these routines stand in for.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace portsc {
typedef uint8_t v16 __attribute__((vector_size(16), aligned(1), may_alias));
typedef uint8_t v8 __attribute__((vector_size(8), aligned(1), may_alias));
typedef uint32_t u32a __attribute__((aligned(1), may_alias));
constexpr size_t kLoadAll = 128;   // the largest size Move128 / Fill handle without a loop
constexpr size_t kMaxLimit = 1024; // the largest size the wrappers send here (the verify mode keeps a copy of that size on the stack)

#define PORTSC_INLINE inline __attribute__((always_inline))

static PORTSC_INLINE bool Overlaps(const void* d, const void* s, size_t n) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(d), b = reinterpret_cast<uintptr_t>(s);
    return static_cast<size_t>(a - b) < n || static_cast<size_t>(b - a) < n;   // (equal addresses count as overlap)
}

// 1 <= n <= 128, any overlap.
static PORTSC_INLINE void Move128(void* dst, const void* src, size_t n) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    if (n >= 16) {
        if (n <= 32) {
            const v16 a = *reinterpret_cast<const v16*>(s), b = *reinterpret_cast<const v16*>(s + n - 16);
            *reinterpret_cast<v16*>(d) = a;
            *reinterpret_cast<v16*>(d + n - 16) = b;
        } else if (n <= 64) {
            const v16 a = *reinterpret_cast<const v16*>(s), b = *reinterpret_cast<const v16*>(s + 16);
            const v16 c = *reinterpret_cast<const v16*>(s + n - 32), e = *reinterpret_cast<const v16*>(s + n - 16);
            *reinterpret_cast<v16*>(d) = a;
            *reinterpret_cast<v16*>(d + 16) = b;
            *reinterpret_cast<v16*>(d + n - 32) = c;
            *reinterpret_cast<v16*>(d + n - 16) = e;
        } else {
            const v16 a0 = *reinterpret_cast<const v16*>(s), a1 = *reinterpret_cast<const v16*>(s + 16);
            const v16 a2 = *reinterpret_cast<const v16*>(s + 32), a3 = *reinterpret_cast<const v16*>(s + 48);
            const v16 b0 = *reinterpret_cast<const v16*>(s + n - 64), b1 = *reinterpret_cast<const v16*>(s + n - 48);
            const v16 b2 = *reinterpret_cast<const v16*>(s + n - 32), b3 = *reinterpret_cast<const v16*>(s + n - 16);
            *reinterpret_cast<v16*>(d) = a0;
            *reinterpret_cast<v16*>(d + 16) = a1;
            *reinterpret_cast<v16*>(d + 32) = a2;
            *reinterpret_cast<v16*>(d + 48) = a3;
            *reinterpret_cast<v16*>(d + n - 64) = b0;
            *reinterpret_cast<v16*>(d + n - 48) = b1;
            *reinterpret_cast<v16*>(d + n - 32) = b2;
            *reinterpret_cast<v16*>(d + n - 16) = b3;
        }
    } else if (n >= 8) {
        const v8 a = *reinterpret_cast<const v8*>(s), b = *reinterpret_cast<const v8*>(s + n - 8);
        *reinterpret_cast<v8*>(d) = a;
        *reinterpret_cast<v8*>(d + n - 8) = b;
    } else if (n >= 4) {
        const uint32_t a = *reinterpret_cast<const u32a*>(s), b = *reinterpret_cast<const u32a*>(s + n - 4);
        *reinterpret_cast<u32a*>(d) = a;
        *reinterpret_cast<u32a*>(d + n - 4) = b;
    } else {   // 1..3: first, middle, last
        const uint8_t a = s[0], b = s[n >> 1], c = s[n - 1];
        d[0] = a;
        d[n >> 1] = b;
        d[n - 1] = c;
    }
}

// n >= 64, the ranges do not overlap.
static PORTSC_INLINE void CopyFwd(void* dst, const void* src, size_t n) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        const v16 a = *reinterpret_cast<const v16*>(s + i), b = *reinterpret_cast<const v16*>(s + i + 16);
        const v16 c = *reinterpret_cast<const v16*>(s + i + 32), e = *reinterpret_cast<const v16*>(s + i + 48);
        *reinterpret_cast<v16*>(d + i) = a;
        *reinterpret_cast<v16*>(d + i + 16) = b;
        *reinterpret_cast<v16*>(d + i + 32) = c;
        *reinterpret_cast<v16*>(d + i + 48) = e;
    }
    if (i < n) {   // the last 64 bytes again (they overlap the last step; the ranges themselves do not overlap, so the same bytes are written)
        const v16 a = *reinterpret_cast<const v16*>(s + n - 64), b = *reinterpret_cast<const v16*>(s + n - 48);
        const v16 c = *reinterpret_cast<const v16*>(s + n - 32), e = *reinterpret_cast<const v16*>(s + n - 16);
        *reinterpret_cast<v16*>(d + n - 64) = a;
        *reinterpret_cast<v16*>(d + n - 48) = b;
        *reinterpret_cast<v16*>(d + n - 32) = c;
        *reinterpret_cast<v16*>(d + n - 16) = e;
    }
}

// memcpy semantics (the ranges are not supposed to overlap; when they do, the result is the memmove one up to 128 bytes, and the caller
// sends larger overlapping copies to the system call). 1 <= n <= kMaxLimit. Returns false when it did nothing (overlapping and above 128).
static PORTSC_INLINE bool Copy(void* dst, const void* src, size_t n) {
    if (n <= kLoadAll) { Move128(dst, src, n); return true; }
    if (Overlaps(dst, src, n)) return false;
    CopyFwd(dst, src, n);
    return true;
}

// 1 <= n <= kMaxLimit.
static PORTSC_INLINE void Fill(void* dst, int c, size_t n) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t b = static_cast<uint8_t>(c);
    if (n >= 16) {
        const v16 v = (v16){} + b;
        if (n <= 32) {
            *reinterpret_cast<v16*>(d) = v;
            *reinterpret_cast<v16*>(d + n - 16) = v;
        } else if (n <= 128) {
            *reinterpret_cast<v16*>(d) = v;
            *reinterpret_cast<v16*>(d + 16) = v;
            *reinterpret_cast<v16*>(d + n - 32) = v;
            *reinterpret_cast<v16*>(d + n - 16) = v;
            if (n > 64) {
                *reinterpret_cast<v16*>(d + 32) = v;
                *reinterpret_cast<v16*>(d + 48) = v;
                *reinterpret_cast<v16*>(d + n - 64) = v;
                *reinterpret_cast<v16*>(d + n - 48) = v;
            }
        } else {
            size_t i = 0;
            for (; i + 64 <= n; i += 64) {
                *reinterpret_cast<v16*>(d + i) = v;
                *reinterpret_cast<v16*>(d + i + 16) = v;
                *reinterpret_cast<v16*>(d + i + 32) = v;
                *reinterpret_cast<v16*>(d + i + 48) = v;
            }
            if (i < n) {
                *reinterpret_cast<v16*>(d + n - 64) = v;
                *reinterpret_cast<v16*>(d + n - 48) = v;
                *reinterpret_cast<v16*>(d + n - 32) = v;
                *reinterpret_cast<v16*>(d + n - 16) = v;
            }
        }
    } else if (n >= 8) {
        const v8 v = (v8){} + b;
        *reinterpret_cast<v8*>(d) = v;
        *reinterpret_cast<v8*>(d + n - 8) = v;
    } else if (n >= 4) {
        const uint32_t w = b * 0x01010101u;
        *reinterpret_cast<u32a*>(d) = w;
        *reinterpret_cast<u32a*>(d + n - 4) = w;
    } else {
        d[0] = b;
        d[n >> 1] = b;
        d[n - 1] = b;
    }
}
#undef PORTSC_INLINE
} // namespace portsc
