// Portable replacements for the PowerPC (Broadway) compiler intrinsics that the
// decompiled code uses.  Included from revolution/types.h on non-CodeWarrior builds.
#pragma once

#include <math.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reciprocal estimates: the hardware returns ~12-bit estimates; the game only
// uses them as a fast path, so an exact result is a strict improvement.
static inline float __frsqrte(float x) { return 1.0f / sqrtf(x); }
static inline float __fres(float x) { return 1.0f / x; }

static inline unsigned int __cntlzw(unsigned int x) { return x ? (unsigned int)__builtin_clz(x) : 32u; }

// PowerPC bit numbering: bit 0 is the MSB.
static inline unsigned int __ppc_mask(int mb, int me) {
    unsigned int m1 = 0xFFFFFFFFu >> mb;
    unsigned int m2 = (me >= 31) ? 0u : (0xFFFFFFFFu >> (me + 1));
    return (mb <= me) ? (m1 & ~m2) : (m1 | ~m2);
}
static inline unsigned int __ppc_rotl(unsigned int v, int sh) {
    sh &= 31;
    return sh ? ((v << sh) | (v >> (32 - sh))) : v;
}
static inline unsigned int __rlwinm(unsigned int v, int sh, int mb, int me) {
    return __ppc_rotl(v, sh) & __ppc_mask(mb, me);
}
static inline unsigned int __rlwimi(unsigned int a, unsigned int s, int sh, int mb, int me) {
    unsigned int m = __ppc_mask(mb, me);
    return (__ppc_rotl(s, sh) & m) | (a & ~m);
}

static inline int __abs(int x) { return x < 0 ? -x : x; }
static inline float __fabsf(float x) { return fabsf(x); }
static inline double __fabs(double x) { return fabs(x); }
static inline float __fnabsf(float x) { return -fabsf(x); }

static inline void* __memcpy(void* d, const void* s, int n) { return memcpy(d, s, (size_t)n); }

// Cache-control instructions.  dcbz really does zero a 32-byte block, the rest
// are no-ops on a coherent host.
static inline void __dcbz(void* base, int off) {
    uintptr_t p = ((uintptr_t)base + (intptr_t)off) & ~(uintptr_t)31;
    memset((void*)p, 0, 32);
}
static inline void __dcbf(void* base, int off) { (void)base; (void)off; }
static inline void __dcbi(void* base, int off) { (void)base; (void)off; }
static inline void __dcbst(void* base, int off) { (void)base; (void)off; }
static inline void __dcbt(void* base, int off) { (void)base; (void)off; }
static inline void __icbi(void* base, int off) { (void)base; (void)off; }
static inline void __sync(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline void __isync(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline void __eieio(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

static inline float __fsel(float a, float b, float c) { return a >= 0.0f ? b : c; }
static inline float __fsels(float a, float b, float c) { return a >= 0.0f ? b : c; }

static inline unsigned int __lwbrx(const void* base, int off) {
    unsigned int v;
    memcpy(&v, (const char*)base + off, 4);
    return v;  // little-endian host: a byte-reversed load is a plain load
}
static inline unsigned short __lhbrx(const void* base, int off) {
    unsigned short v;
    memcpy(&v, (const char*)base + off, 2);
    return v;
}
static inline void __stwbrx(unsigned int v, void* base, int off) { memcpy((char*)base + off, &v, 4); }
static inline void __sthbrx(unsigned short v, void* base, int off) { memcpy((char*)base + off, &v, 2); }

#ifdef __cplusplus
}
#endif
