// GX capture host proof (tools/test_gx_capture.sh): the services the console's GX library (decomp RVL_SDK/gx, compiled for TARGET_PC
// as the game is) needs, and the FIFO. The FIFO stands for gx_fifo.cpp's staging buffer: a small buffer (so that overflows happen
// often), drained into "the stream" (what the front end would receive) at every overflow and flush, with the capture layer's event
// hooks exactly where gx_fifo.cpp has them. A guest arena gives the stable addresses vertex arrays, lists and textures point at.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "revolution/types.h"
#include "revolution/gx.h"
#include "revolution/gx/GXRegs.h"
#include "port/gx_capture.h"

volatile u32 __PortGXPIRegs[0x400], __PortGXCPRegs[0x400], __PortGXPERegs[0x400], __PortGXMEMRegs[0x400];
volatile u32 __PIRegs[0x400];

uint8_t* __PortGXFifoPtr;
uint8_t* __PortGXFifoLimit;
// (__PortGXFifoBatch: gx_fifo.h's weak zero, set to 1 by the test: the batched writers, as the console runs them)

// ---- the FIFO
#define kFifoBytes 3000
static uint8_t sFifo[kFifoBytes + 64];
static uint8_t* sStream;
static size_t sStreamLen, sStreamCap;
int hx_fifo_small = 0;  // (tests: a tighter limit, more overflows)

static void streamPut(const uint8_t* p, size_t n) {
    if (sStreamLen + n > sStreamCap) {
        sStreamCap = (sStreamLen + n) * 2 + 4096;
        sStream = (uint8_t*)realloc(sStream, sStreamCap);
    }
    memcpy(sStream + sStreamLen, p, n);
    sStreamLen += n;
}
static void drain(void) {
    if (__PortGXFifoPtr > sFifo) streamPut(sFifo, (size_t)(__PortGXFifoPtr - sFifo));
    __PortGXFifoPtr = sFifo;
    __PortGXFifoLimit = sFifo + (hx_fifo_small ? 97 : kFifoBytes);
}
void hx_fifo_reset(void) {
    sStreamLen = 0;
    __PortGXFifoPtr = sFifo;
    __PortGXFifoLimit = sFifo + (hx_fifo_small ? 97 : kFifoBytes);
}
const uint8_t* hx_stream(size_t* n) {
    drain();
    *n = sStreamLen;
    return sStream;
}
int hx_fifo_in_stream(void) { return __PortGXFifoPtr >= sFifo && __PortGXFifoPtr <= sFifo + kFifoBytes + 64; }

// (as gx_fifo.cpp: the layer's events first while it owns the FIFO)
static void overflowReal(void) { drain(); }
void __PortGXFifoOverflow(void) {
    if (__PortGXCapFifo) {
        if (port_gxcap_fifo_pre(PORT_GXCAP_EV_OVERFLOW)) return;
        overflowReal();
        port_gxcap_fifo_post(PORT_GXCAP_EV_OVERFLOW);
        return;
    }
    overflowReal();
}
void __PortGXFifoFlush(void) {
    if (__PortGXCapFifo) {
        if (port_gxcap_fifo_pre(PORT_GXCAP_EV_FLUSH)) return;
        drain();
        port_gxcap_fifo_post(PORT_GXCAP_EV_FLUSH);
        return;
    }
    drain();
}
void __PortGXFifoWriteBytes(const void* src, uint32_t n) {
    const uint8_t* s = (const uint8_t*)src;
    while (n) {
        size_t room = __PortGXFifoLimit > __PortGXFifoPtr ? (size_t)(__PortGXFifoLimit - __PortGXFifoPtr) : 0;
        if (!room) {
            __PortGXFifoOverflow();
            continue;
        }
        size_t k = n < room ? n : room;
        memcpy(__PortGXFifoPtr, s, k);
        __PortGXFifoPtr += k;
        s += k;
        n -= (uint32_t)k;
        if (__PortGXFifoPtr >= __PortGXFifoLimit) __PortGXFifoOverflow();
    }
}
int __PortGXFifoXfBurst12(const float* s) { (void)s; return 0; }
int hx_retained = 1, hx_bracket = 1, hx_peek_const = 1;
static int inStreamOrOverride(void) { return hx_fifo_in_stream() || (__PortGXCapFifo && port_gxcap_stream_override()); }
int __PortGXRetainedOk(void) { return hx_retained && inStreamOrOverride(); }
int __PortGXBracketOk(void) { return hx_bracket && inStreamOrOverride(); }
int __PortGXFxMergeOk(void) { return 0; }
int __PortGXFifoInStream(void) { return hx_fifo_in_stream(); }
int __PortGXPeekConst(void) { return hx_peek_const; }
u32 __PortGXPeekARGB(u16 x, u16 y) { (void)x; (void)y; return 0xFFFFFFFFu; }
u32 __PortGXPeekZ(u16 x, u16 y) { (void)x; (void)y; return 0x00FFFFFFu; }

// ---- the guest arena
#define kGuestBase 0x00100000u
#define kArenaBytes (4u << 20)
static uint8_t sArena[kArenaBytes] __attribute__((aligned(64)));
uint8_t* hx_arena(void) { return sArena; }
u32 port_host_to_phys(const void* p) {
    if (!p) return 0;
    return kGuestBase + (u32)((const uint8_t*)p - sArena);
}
static uint32_t sLowMem[0x4000 / 4];
void* port_phys_to_host(u32 a) {
    if (a < 0x4000u) {
        sLowMem[0xF8 / 4] = 243000000u;
        return (uint8_t*)sLowMem + a;
    }
    return sArena + (a - kGuestBase);
}

// ---- OS / PPC / VI stand-ins
void PPCSync(void) {}
u32 PPCMfhid2(void) { return 0; }
void PPCMthid2(u32 v) { (void)v; }
u32 PPCMfwpar(void) { return 0; }
void DCStoreRangeNoSync(void* p, u32 n) { (void)p; (void)n; }
void DCFlushRange(void* p, u32 n) { (void)p; (void)n; }
void DCInvalidateRange(void* p, u32 n) { (void)p; (void)n; }
// (tests/gx_capture.cpp, the trap test: the guest OS belongs to guest threads; a call from any thread but the one hx_irq_main
// named is counted, as the console's owner check counts a guest-OS call on the draw thread)
#include <pthread.h>
static pthread_t sIrqMain;
static int sIrqMainSet;
int hx_irq_foreign = 0;
void hx_irq_main(void) {
    sIrqMain = pthread_self();
    sIrqMainSet = 1;
}
static void irqCall(void) {
    if (sIrqMainSet && !pthread_equal(sIrqMain, pthread_self())) __atomic_fetch_add(&hx_irq_foreign, 1, __ATOMIC_RELAXED);
}
int OSDisableInterrupts(void) {
    irqCall();
    return 0;
}
int OSRestoreInterrupts(int l) {
    irqCall();
    return l;
}
s64 OSGetTime(void) { return 0; }
void OSRegisterVersion(const char* v) { (void)v; }
void OSRegisterShutdownFunction(void* f) { (void)f; }
void OSReport(const char* f, ...) { (void)f; }
void OSClearContext(void* c) { (void)c; }
void OSSetCurrentContext(void* c) { (void)c; }
void OSInitThreadQueue(void* q) { (void)q; }
void OSSleepThread(void* q) { (void)q; }
void OSWakeupThread(void* q) { (void)q; }
void* __OSSetInterruptHandler(s16 i, void* h) { (void)i; (void)h; return 0; }
u32 __OSUnmaskInterrupts(u32 m) { return m; }
u32 VIGetTvFormat(void) { return 0; }
void port_native_phase_cpu_checkpoint(int p) { (void)p; }
void port_game_progress_checkpoint(int p) { (void)p; }
void GXSetGPMetric(GXPerf0 a, GXPerf1 b) { (void)a; (void)b; }
void GXClearGPMetric(void) {}
void GXInitFifoBase(GXFifoObj* f, void* b, u32 s) { (void)f; (void)b; (void)s; }
void GXSetCPUFifo(const GXFifoObj* f) { PORT_GXCAP_ABANDON(GXSetCPUFifo); (void)f; }
void GXSetGPFifo(const GXFifoObj* f) { PORT_GXCAP_ABANDON(GXSetGPFifo); (void)f; }
void __GXFifoInit(void) {}
GXBreakPtCallback GXSetBreakPtCallback(GXBreakPtCallback cb) { (void)cb; return 0; }
void __GXCleanGPFifo(void) {}
GXBool __GXIsGPFifoReady(void) { return GX_TRUE; }
GXBool GXGetCPUFifo(GXFifoObj* f) { PORT_GXCAP_ABANDON(GXGetCPUFifo); (void)f; return GX_TRUE; }
u32 GXGetFifoCount(const GXFifoObj* f) { (void)f; return 0; }
GXBool GXGetFifoWrap(const GXFifoObj* f) { (void)f; return GX_FALSE; }

// ---- port services
#include <time.h>
int64_t port_host_time_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}
int hx_quiet = 0;
void port_log(const char* fmt, ...) {
    if (hx_quiet) return;
    va_list ap;
    va_start(ap, fmt);
    printf("log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
void port_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("FATAL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    abort();
}
// (compat.h routes the game's snprintf here; mtx.c's profile hooks)
#undef vsnprintf
#undef snprintf
int port_vsnprintf(char* dst, size_t n, const char* fmt, va_list args) { return vsnprintf(dst, n, fmt, args); }
int port_snprintf(char* dst, size_t n, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(dst, n, fmt, ap);
    va_end(ap);
    return r;
}
unsigned port_mtx_profile_begin(const void* m) { (void)m; return 0; }
void port_mtx_profile_end(unsigned t, unsigned r) { (void)t; (void)r; }
void port_stream_census_copy(void) {}  /* (GXV_STREAM_CENSUS's marker at GXCopyDisp, perf.cpp: nothing here) */
