// GXV_GX_DEDUP (gxdedup31, 18 Nov; default 0) and GXV_GX_DEDUP_CENSUS (default 0): repeated register writes removed where they are made.
// The GX library's register-write macros (revolution/gx/GXRegs.h: GX_WRITE_RA_REG, GX_WRITE_CP_STRM_REG, GX_WRITE_XF_REG) and J3D's direct
// FIFO writers (J3DFifo.hpp J3DFifoLoadBPCmd / J3DFifoLoadCPCmd, J3DShape.cpp J3DLoadCPCmd, J3DShape.hpp J3DShapeMtxIdx::load) go through
// the gate below, in a build made with PORT_GX_DEDUP_BUILD=1. Without it (the default: tools/vgx_build/build.py does not set it) every
// one of them is the write it was before, byte for byte and instruction for instruction, the invalidations are empty and the switches
// below only log that the gate is not in this build. (The gate's check costs every register write five instructions on the Cortex-A9
// even with the switches 0: the reason it is a build option.)
//   GXV_GX_DEDUP=1  a write the decision core (port/gx_dedup_core.h: what is decided, which registers, the invalidations) finds the front
//                   end's register already holding is not written
//   GXV_GX_DEDUP=2  verify: everything is written; in front of each write =1 would leave out, the game writes XF 0x0C00 (what: class << 24 |
//                   register) and XF 0x0C01 (the value), an address the front end keeps nothing at; the front end (vgx_state.cpp, GxDedupProbe)
//                   checks that its register holds the value with no BP mask pending, and every GXV_GX_DEDUP_DIGEST-th time (default 64)
//                   makes the write there and compares the digest of everything a register write can touch: "gxdedup verify:" lines, 0
//                   differing. (The probe is an XF write, not a BP marker, because a BP marker would use up a pending mask.)
//   GXV_GX_DEDUP_CENSUS=<n>  count only (the stream is unchanged): every n pictures (1: 300) "gxdedup-census:" lines: tracked writes per
//                   picture by class (BP, CP, XF) and register group, how many the shadow finds identical, how many were identical to
//                   the last value but the shadow had been forgotten (by which invalidation), and the call sites (return addresses as
//                   offsets from GXInitFifoBase) with the most identical and the most forgotten writes. Works with any GXV_GX_DEDUP.
//   GXV_GX_DEDUP_CLASSES=<bits> (default 3; the census takes 3): which writes go through the gate. 1: the BP writes; 2: the CP and XF
//                   register writes (J3D's array bases and matrix indices, the GX library's vertex formats, texgens, colours); a class
//                   left out is written as before and costs nothing. (CP 0x30 / 0x40 and XF 0x1018 / 0x1019 are one cell in the front
//                   end, so CP and XF go together; BP and CP / XF share no state but the invalidations.)
//   GXV_GX_DEDUP_SHORT=0 (=1 only): every gated write takes the general path (the short path's own check; same bytes).
// The cost (host, x86-64 instructions, callgrind; reports/gxdedup31.md): with GXV_GX_DEDUP=1 the game thread spends about 28 more for each
// gated write (tools/test_gx_dedup.sh's harness at -O2: the short path, the general path's share, the call), and the front end saves
// about 32 for each write left out (tests/vgx_fe_work.cpp with GXV_FE_STATEFAST=1). At the console's census rates (69 % repeats) that
// moves more work onto the game thread than it takes off the front end's core: worth it only where the front end is the longer of the two.
// The gate is live only while the FIFO writes into the staging buffer the vgx front end reads (a display list being recorded, the overflow
// sink: the write goes out as before and the shadow is forgotten). The switch refuses (log line, stays 0) with the engines that copy bytes
// into the stream or reorder it: GXV_REPLAY25, GXV_PIPE25, GXV_GXCAPTURE, GXV_FXMERGE, GXV_DRAWPIPE.
// Game thread (the thread that owns GX), as every GX call.
#pragma once
#include <stdint.h>
#include "port/gx_fifo.h"
#include "port/gx_dedup_core.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { PORT_GXDD_CAUSE_LIST = 0, PORT_GXDD_CAUSE_MATLIST, PORT_GXDD_CAUSE_PRIMS, PORT_GXDD_CAUSE_MARKER, PORT_GXDD_CAUSE_PICTURE, PORT_GXDD_CAUSE_INIT,
       PORT_GXDD_CAUSE_FIFO, PORT_GXDD_CAUSE_OUTSIDE, PORT_GXDD_CAUSE_OTHER, PORT_GXDD_CAUSES };
void port_gx_dedup_init(void);  // (gx_fifo.cpp port_gx_init: the switches, read once)

// gx_fifo.cpp (PORT_GX_DEDUP_OWNER, gx_dedup.inc) holds the gate's functions and bytes, strongly, in every build; the other units of a
// PORT_GX_DEDUP_BUILD=1 build see weak declarations and weak zero bytes, so that host tests that link GX or J3D units without gx_fifo.cpp
// keep today's writes (the gate's functions are called only while a switch byte is set).
#if defined(PORT_GX_DEDUP_OWNER)
#define PORT_GXDD_WEAKFN
extern unsigned char __PortGXDedupOn;    // 1: the switch is on, 2: the census is on (the invalidations reach the gate); 0: off
extern unsigned char __PortGXDedupOnBP;  // nonzero: the BP writes go through the gate (GXV_GX_DEDUP_CLASSES bit 0)
extern unsigned char __PortGXDedupOnCX;  // nonzero: the CP and XF register writes go through the gate (bit 1)
extern unsigned char __PortGXDedupKeep;  // set around a list call whose list the caller vouches for: PORT_GXDD_BUMP_NOCP / _PRIMS (0: unknown)
#elif defined(PORT_GX_DEDUP_BUILD) && PORT_GX_DEDUP_BUILD
#define PORT_GXDD_WEAKFN __attribute__((weak))
#define PORT_GX_DEDUP_WEAK 1  // (gx_fifo.cpp included after this header, as host tests do: it keeps these definitions)
__attribute__((weak)) unsigned char __PortGXDedupOn = 0;
__attribute__((weak)) unsigned char __PortGXDedupOnBP = 0;
__attribute__((weak)) unsigned char __PortGXDedupOnCX = 0;
__attribute__((weak)) unsigned char __PortGXDedupKeep = 0;
#else
#define PORT_GXDD_WEAKFN __attribute__((weak))
#endif
void __PortGXDedupBP(uint32_t rv, const void* ra) PORT_GXDD_WEAKFN;
void __PortGXDedupCP(uint32_t reg, uint32_t v, const void* ra) PORT_GXDD_WEAKFN;
void __PortGXDedupXF(uint32_t hdr, uint32_t v, const void* ra) PORT_GXDD_WEAKFN;                // one XF word (hdr: the XF_LOADREGS word)
void __PortGXDedupXF2(uint32_t addr, uint32_t a, uint32_t b, const void* ra) PORT_GXDD_WEAKFN;  // two XF words at addr (J3DShapeMtxIdx::load)
void __PortGXDedupBumpAt(int kind, int cause) PORT_GXDD_WEAKFN;                                 // an invalidation (cause: PORT_GXDD_CAUSE_*)

#if defined(__GNUC__)
#define PORT_GXDD_INLINE static inline __attribute__((always_inline))
#else
#define PORT_GXDD_INLINE static inline
#endif

#if defined(PORT_GX_DEDUP_BUILD) && PORT_GX_DEDUP_BUILD
// The writes. (always_inline: __builtin_return_address(0) is then the GX function's caller, the census's call site)
PORT_GXDD_INLINE void __PortGXWriteBP(uint32_t rv) {
    if (__builtin_expect(__PortGXDedupOnBP != 0, 0)) {
        __PortGXDedupBP(rv, __builtin_return_address(0));
        return;
    }
    __PortGXFifo_u8(0x61);
    __PortGXFifo_u32(rv);
}
PORT_GXDD_INLINE void __PortGXWriteCP(uint8_t reg, uint32_t v) {
    if (__builtin_expect(__PortGXDedupOnCX != 0, 0)) {
        __PortGXDedupCP(reg, v, __builtin_return_address(0));
        return;
    }
    __PortGXFifo_op_u8_u32(0x08, reg, v);
}
PORT_GXDD_INLINE void __PortGXWriteXF(uint32_t hdr, uint32_t v) {
    if (__builtin_expect(__PortGXDedupOnCX != 0, 0)) {
        __PortGXDedupXF(hdr, v, __builtin_return_address(0));
        return;
    }
    __PortGXFifo_op_u32_u32(0x10, hdr, v);
}
// J3DShapeMtxIdx::load's XF half: the two matrix-index words as one command (the bytes J3DFifoLoadXFCmdHdr + two GXCmd1u32 wrote)
PORT_GXDD_INLINE void __PortGXWriteXF2(uint32_t addr, uint32_t a, uint32_t b) {
    if (__builtin_expect(__PortGXDedupOnCX != 0, 0)) {
        __PortGXDedupXF2(addr, a, b, __builtin_return_address(0));
        return;
    }
    __PortGXFifo_op_u16_u16(0x10, 1, (uint16_t)addr);
    __PortGXFifo_u32(a);
    __PortGXFifo_u32(b);
}
// What a caller vouches for at its list call (PORT_GXDD_BUMP_NOCP / _PRIMS), then 0 after it.
#define PORT_GXDD_KEEP(k) (__PortGXDedupKeep = (unsigned char)(k))
// A display-list call of a list of primitives and no-ops only, made by the caller itself (JParticle's quad lists): with GXV_GX_DEDUP its
// call forgets nothing the gate knows (otherwise exactly GXCallDisplayList).
#define PORT_GX_CALL_PRIMS(list, nbytes)                                                                                                           \
    do {                                                                                                                                           \
        __PortGXDedupKeep = PORT_GXDD_BUMP_PRIMS;                                                                                                  \
        GXCallDisplayList((list), (nbytes));                                                                                                       \
        __PortGXDedupKeep = 0;                                                                                                                     \
    } while (0)
// An invalidation at this point of the stream (one byte load when both switches are off). A list of primitives forgets nothing: its
// call reaches the gate only for the census's count.
PORT_GXDD_INLINE void __PortGXDedupBump(int kind, int cause) {
    if (__builtin_expect(__PortGXDedupOn != 0, 0) && (kind != PORT_GXDD_BUMP_PRIMS || __PortGXDedupOn == 2)) __PortGXDedupBumpAt(kind, cause);
}
#define PORT_GXDD_KEPT() ((int)__PortGXDedupKeep)
#else
// (not in this build: the writes as they were, nothing else)
PORT_GXDD_INLINE void __PortGXWriteBP(uint32_t rv) {
    __PortGXFifo_u8(0x61);
    __PortGXFifo_u32(rv);
}
PORT_GXDD_INLINE void __PortGXWriteCP(uint8_t reg, uint32_t v) { __PortGXFifo_op_u8_u32(0x08, reg, v); }
PORT_GXDD_INLINE void __PortGXWriteXF(uint32_t hdr, uint32_t v) { __PortGXFifo_op_u32_u32(0x10, hdr, v); }
PORT_GXDD_INLINE void __PortGXWriteXF2(uint32_t addr, uint32_t a, uint32_t b) {
    __PortGXFifo_op_u16_u16(0x10, 1, (uint16_t)addr);
    __PortGXFifo_u32(a);
    __PortGXFifo_u32(b);
}
#define PORT_GXDD_KEEP(k) ((void)0)
#define PORT_GX_CALL_PRIMS(list, nbytes) GXCallDisplayList((list), (nbytes))
PORT_GXDD_INLINE void __PortGXDedupBump(int kind, int cause) {
    (void)kind;
    (void)cause;
}
#define PORT_GXDD_KEPT() 0
#endif

#ifdef __cplusplus
}
#endif
