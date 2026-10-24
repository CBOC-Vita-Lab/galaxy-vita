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

#ifdef __cplusplus
}
#endif
