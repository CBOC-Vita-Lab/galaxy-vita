// Replacement for the Broadway write-gather pipe (0xCC008000).
//
// The decompiled GX library builds exactly the command stream the real GPU would
// receive.  On the port, every FIFO write lands in a host buffer (big-endian, like
// the hardware stream and like display lists in game data) which the port's GPU
// command processor consumes at synchronisation points.
#pragma once

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t* __PortGXFifoPtr;
extern uint8_t* __PortGXFifoLimit;  // write position that triggers a flush (leaves slack)
void __PortGXFifoOverflow(void);
#if defined(TARGET_PC)
// Returns nonzero only when the complete XF payload was written.
int __PortGXFifoXfBurst12(const float* source);
#endif

static inline void __PortGXFifoCheck(void) {
    if (__PortGXFifoPtr >= __PortGXFifoLimit) {
        __PortGXFifoOverflow();
    }
}

static inline void __PortGXFifo_u8(uint8_t v) {
    *__PortGXFifoPtr++ = v;
    __PortGXFifoCheck();
}
static inline void __PortGXFifo_s8(int8_t v) { __PortGXFifo_u8((uint8_t)v); }

static inline void __PortGXFifo_u16(uint16_t v) {
    uint8_t* p = __PortGXFifoPtr;
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
    __PortGXFifoPtr = p + 2;
    __PortGXFifoCheck();
}
static inline void __PortGXFifo_s16(int16_t v) { __PortGXFifo_u16((uint16_t)v); }

static inline void __PortGXFifo_u32(uint32_t v) {
    v = __builtin_bswap32(v);
    memcpy(__PortGXFifoPtr, &v, 4);
    __PortGXFifoPtr += 4;
    __PortGXFifoCheck();
}
static inline void __PortGXFifo_s32(int32_t v) { __PortGXFifo_u32((uint32_t)v); }

static inline void __PortGXFifo_f32(float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    __PortGXFifo_u32(v);
}

// ---------------------------------------------------------------------------
// GXV_FIFO_BATCH (default 1; read once by port_gx_init, platform/src/gx/gx_fifo.cpp): a command of three or more
// elements (an XF register write; a CP register write; a display-list call; an indexed matrix load; an immediate matrix
// or light object; a three- or four-component immediate vertex attribute) is written through one local pointer with
// one room check, instead of one write-pointer load, store and limit check per element. With -fno-strict-aliasing
// every byte store may alias __PortGXFifoPtr / __PortGXFifoLimit, so the element-wise form reloads both after each
// element (about nine instructions a word on the Cortex-A9; three in a batch). Two-element commands (a BP register
// write) stay element-wise: the room check costs what the batch saves there.
//
// Exactness: the batch is taken only when the whole command ends strictly before the limit, i.e. exactly when the
// element-wise form would not have called __PortGXFifoOverflow at any element (its check is "ptr >= limit" after
// each one). Otherwise, or with GXV_FIFO_BATCH=0, the element-wise form runs as before. The bytes written and the
// final write pointer are the same in both forms; nothing reads the pointer between the elements (one producer).
// (A weak zero here, so that host tests that link GX code without gx_fifo.cpp keep the element-wise form; gx_fifo.cpp
// holds the real one.)
#if defined(PORT_GX_FIFO_BATCH_OWNER)
extern int __PortGXFifoBatch;
#else
#define PORT_GX_FIFO_BATCH_WEAK 1  // (gx_fifo.cpp included after this header, as host tests do: it keeps this definition)
__attribute__((weak)) int __PortGXFifoBatch = 0;
#endif

// Nonzero, with *pp at the write position, when a batch of `n` bytes may be written there (then the caller writes it and
// commits); zero: the caller writes element by element. One signed compare covers both "ptr < limit" and the room.
static inline int __PortGXFifoRoom(uint8_t** pp, uint32_t n) {
    uint8_t* const p = __PortGXFifoPtr;
    *pp = p;
    return __PortGXFifoBatch && (intptr_t)((uintptr_t)__PortGXFifoLimit - (uintptr_t)p) > (intptr_t)n;
}
static inline uint8_t* __PortGXPut_u8(uint8_t* p, uint8_t v) {
    *p = v;
    return p + 1;
}
static inline uint8_t* __PortGXPut_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
    return p + 2;
}
static inline uint8_t* __PortGXPut_u32(uint8_t* p, uint32_t v) {
    v = __builtin_bswap32(v);
    memcpy(p, &v, 4);
    return p + 4;
}
static inline uint8_t* __PortGXPut_s8(uint8_t* p, int8_t v) { return __PortGXPut_u8(p, (uint8_t)v); }
static inline uint8_t* __PortGXPut_s16(uint8_t* p, int16_t v) { return __PortGXPut_u16(p, (uint16_t)v); }
static inline uint8_t* __PortGXPut_f32(uint8_t* p, float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    return __PortGXPut_u32(p, v);
}
// The batch's end: the write pointer moves past it (no limit check: the room check proved it ends before the limit).
static inline void __PortGXFifoCommit(uint8_t* p) { __PortGXFifoPtr = p; }

// The common three-element commands, batched (see above; a two-element command gains nothing). `op`: the opcode byte.
static inline void __PortGXFifo_op_u8_u32(uint8_t op, uint8_t reg, uint32_t v) {  // CP register write (0x08)
    uint8_t* p;
    if (__PortGXFifoRoom(&p, 6)) {
        p = __PortGXPut_u8(p, op);
        p = __PortGXPut_u8(p, reg);
        __PortGXFifoCommit(__PortGXPut_u32(p, v));
        return;
    }
    __PortGXFifo_u8(op);
    __PortGXFifo_u8(reg);
    __PortGXFifo_u32(v);
}
static inline void __PortGXFifo_op_u32_u32(uint8_t op, uint32_t a, uint32_t b) {  // XF register write, display-list call
    uint8_t* p;
    if (__PortGXFifoRoom(&p, 9)) {
        p = __PortGXPut_u8(p, op);
        p = __PortGXPut_u32(p, a);
        __PortGXFifoCommit(__PortGXPut_u32(p, b));
        return;
    }
    __PortGXFifo_u8(op);
    __PortGXFifo_u32(a);
    __PortGXFifo_u32(b);
}
static inline void __PortGXFifo_op_u16_u16(uint8_t op, uint16_t a, uint16_t b) {  // indexed matrix load, XF header
    uint8_t* p;
    if (__PortGXFifoRoom(&p, 5)) {
        p = __PortGXPut_u8(p, op);
        p = __PortGXPut_u16(p, a);
        __PortGXFifoCommit(__PortGXPut_u16(p, b));
        return;
    }
    __PortGXFifo_u8(op);
    __PortGXFifo_u16(a);
    __PortGXFifo_u16(b);
}
static inline void __PortGXFifo_u64(uint64_t v) {
    __PortGXFifo_u32((uint32_t)(v >> 32));
    __PortGXFifo_u32((uint32_t)v);
}
static inline void __PortGXFifo_s64(int64_t v) { __PortGXFifo_u64((uint64_t)v); }
static inline void __PortGXFifo_f64(double d) {
    uint64_t v;
    memcpy(&v, &d, 8);
    __PortGXFifo_u64(v);
}

#if defined(TARGET_PC)
// ---------------------------------------------------------------------------
// GXV_FXMERGE (default 0; read by __PortGXFxMergeOk, platform/src/gx/gx_fifo.cpp): JParticle's particle loops sent as port-private
// commands. JPAResource::drawP / drawC start the gathering around an emitter's particle loop (port_gx_fx_begin / port_gx_fx_end,
// decomp/src/RVL_SDK/gx/GXDisplayList.c); while __PortGXFx is set, GXSetTevColor, GXLoadPosMtxImm, GXLoadTexMtxImm and
// GXCallDisplayList hand their command to the gathering instead of the FIFO. The front end's side, and why the pictures are the
// same: platform/vgx/src/vgx_state.cpp, FxMergeRun.
// (__PortGXFx: GXDisplayList.c holds the real one; a weak zero here keeps host tests that link GX units without it as they were.
// The functions are weak declarations for the same reason: they are called only while __PortGXFx is set.)
#if defined(PORT_GX_FX_OWNER)
extern uint8_t __PortGXFx;
#else
__attribute__((weak)) uint8_t __PortGXFx = 0;
#endif
void __PortGXFxTevColor(uint32_t ra, uint32_t bg) __attribute__((weak));               // GXDisplayList.c: the two BP words of GXSetTevColor
void __PortGXFxXF(uint32_t reg, const float* words, uint32_t n) __attribute__((weak));  // GXDisplayList.c: an XF load of n words
int port_gx_fx_begin(void) __attribute__((weak));                                       // GXDisplayList.c: 1 when gathering started
void port_gx_fx_end(void) __attribute__((weak));                                        // GXDisplayList.c: the gathered command out
int __PortGXFxMergeOk(void) __attribute__((weak));           // gx_fifo.cpp: the switch, and the FIFO feeds the vgx front end now
void __PortGXFifoWriteBytes(const void* src, uint32_t n) __attribute__((weak)); // gx_fifo.cpp: bytes in, as element writes would put them
void __PortGXFxForeign(void) __attribute__((weak));          // gx_fifo.cpp: a write the gathering did not take (logged; gathering off)
#endif

#ifdef __cplusplus
}
#endif
