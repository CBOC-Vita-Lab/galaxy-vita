// GXV_BLOOM_FAST host proof (tools/test_bloom_fast_gl.sh): the console's GX library (decomp RVL_SDK/gx, compiled for TARGET_PC as the
// game is) needs a few OS / FIFO / memory services. These are the stand-ins of this test: one guest arena (a "physical" address window
// at kGuestBase), the FIFO handed to the production front end (tests/bloom_fast_gl_main.cpp hx_fifo_drain), and no-op OS calls.
#include <stdint.h>
#include <string.h>
#include "revolution/types.h"
#include "revolution/gx.h"
#include "revolution/gx/GXRegs.h"

#define kGuestBase 0x00100000u
#define kArenaBytes (24u << 20)
static uint8_t sArena[kArenaBytes] __attribute__((aligned(64)));
static uint32_t sArenaUsed = 0;

volatile u32 __PortGXPIRegs[0x400], __PortGXCPRegs[0x400], __PortGXPERegs[0x400], __PortGXMEMRegs[0x400];
volatile u32 __PIRegs[0x400];

uint8_t* __PortGXFifoPtr;
uint8_t* __PortGXFifoLimit;
static uint8_t sFifo[4u << 20] __attribute__((aligned(64)));

void hx_fifo_drain(const uint8_t* base, uint8_t** ptr);  // (bloom_fast_gl_main.cpp: through vgx::ProcessStream)
void __PortGXFifoOverflow(void) { hx_fifo_drain(sFifo, &__PortGXFifoPtr); }
void __PortGXFifoFlush(void) { hx_fifo_drain(sFifo, &__PortGXFifoPtr); }
void hx_fifo_flush(void) { hx_fifo_drain(sFifo, &__PortGXFifoPtr); }
void __PortGXFifoWriteBytes(const void* src, uint32_t n) {
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; ++i) {
        *__PortGXFifoPtr++ = s[i];
        if (__PortGXFifoPtr >= __PortGXFifoLimit) __PortGXFifoOverflow();
    }
}
int __PortGXFifoXfBurst12(const float* s) { (void)s; return 0; }
int __PortGXRetainedOk(void) { return 0; }
int __PortGXFxMergeOk(void) { return 0; }
u32 __PortGXPeekARGB(u32 x, u32 y) { (void)x; (void)y; return 0; }
u32 __PortGXPeekZ(u32 x, u32 y) { (void)x; (void)y; return 0; }

// The guest arena: allocations the GX registers point at (textures, copy destinations, the blur matrices, the XFB).
void* hx_guest_alloc(uint32_t bytes) {
    sArenaUsed = (sArenaUsed + 63u) & ~63u;
    if (sArenaUsed + bytes > kArenaBytes) return 0;
    void* p = sArena + sArenaUsed;
    sArenaUsed += bytes + 4096u;  // (a gap: no copy's guest range touches the next one)
    memset(p, 0, bytes);
    return p;
}
u32 port_host_to_phys(const void* p) {
    if (!p) return 0;
    return kGuestBase + (u32)((const uint8_t*)p - sArena);
}
static uint32_t sLowMem[0x4000 / 4];  // (OS globals: GXInit reads the bus clock at 0x800000F8)
void* port_phys_to_host(u32 a) {
    if (a < 0x4000u) {
        sLowMem[0xF8 / 4] = 243000000u;
        return (uint8_t*)sLowMem + a;
    }
    return sArena + (a - kGuestBase);
}
const uint8_t* hx_resolve(uint32_t a, uint32_t* avail) {
    a &= 0x3FFFFFFFu;
    if (a < kGuestBase || a >= kGuestBase + kArenaBytes) return 0;
    if (avail) *avail = kGuestBase + kArenaBytes - a;
    return sArena + (a - kGuestBase);
}

void hx_fifo_init(void) {
    __PortGXFifoPtr = sFifo;
    __PortGXFifoLimit = sFifo + sizeof sFifo - 65536;
}

// OS / PPC / VI stand-ins (nothing of these is timing- or picture-relevant here)
void PPCSync(void) {}
u32 PPCMfhid2(void) { return 0; }
void PPCMthid2(u32 v) { (void)v; }
u32 PPCMfwpar(void) { return 0; }
void DCStoreRangeNoSync(void* p, u32 n) { (void)p; (void)n; }
void DCFlushRange(void* p, u32 n) { (void)p; (void)n; }
void DCInvalidateRange(void* p, u32 n) { (void)p; (void)n; }
int OSDisableInterrupts(void) { return 0; }
int OSRestoreInterrupts(int l) { return l; }
s64 OSGetTime(void) { return 0; }
void OSRegisterVersion(const char* v) { (void)v; }
void OSRegisterShutdownFunction(void* f) { (void)f; }
void OSReport(const char* f, ...) { (void)f; }
void OSClearContext(void* c) { (void)c; }
void OSSetCurrentContext(void* c) { (void)c; }
void* OSGetCurrentThread(void) { return 0; }
s32 OSResumeThread(void* t) { (void)t; return 0; }
s32 OSSuspendThread(void* t) { (void)t; return 0; }
void OSInitThreadQueue(void* q) { (void)q; }
void OSSleepThread(void* q) { (void)q; }
void OSWakeupThread(void* q) { (void)q; }
void* __OSSetInterruptHandler(s16 i, void* h) { (void)i; (void)h; return 0; }
u32 __OSUnmaskInterrupts(u32 m) { return m; }
u32 VIGetTvFormat(void) { return 0; }  // (VI_NTSC: GXNtsc480IntDf, 640 wide)
void port_native_phase_cpu_checkpoint(void) {}
void GXSetGPMetric(GXPerf0 a, GXPerf1 b) { (void)a; (void)b; }
void GXClearGPMetric(void) {}
// (the FIFO objects: the port writes straight into __PortGXFifoPtr; GXFifo.c is not linked)
void GXInitFifoBase(GXFifoObj* f, void* b, u32 s) { (void)f; (void)b; (void)s; }
void GXSetCPUFifo(const GXFifoObj* f) { (void)f; }
void GXSetGPFifo(const GXFifoObj* f) { (void)f; }
void __GXFifoInit(void) {}
GXBreakPtCallback GXSetBreakPtCallback(GXBreakPtCallback cb) { (void)cb; return 0; }
void __GXCleanGPFifo(void) {}
GXBool __GXIsGPFifoReady(void) { return GX_TRUE; }
GXBool GXGetCPUFifo(GXFifoObj* f) { (void)f; return GX_TRUE; }
u32 GXGetFifoCount(const GXFifoObj* f) { (void)f; return 0; }
GXBool GXGetFifoWrap(const GXFifoObj* f) { (void)f; return GX_FALSE; }
// (ImageEffectLocalUtil.cpp's port services: the basic log goes to stdout; the render size never changes here)
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
void port_log_summary(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
u32 port_render_size_gen(void) { return 1; }
#undef vsnprintf
int port_snprintf(char* dst, size_t n, const char* fmt, ...) {  // (compat.h maps the game's snprintf here)
    va_list ap;
    va_start(ap, fmt);
    const int r = vsnprintf(dst, n, fmt, ap);
    va_end(ap);
    return r;
}
int port_perf_mode_60(void) {  // (GXV_BLOOM_FAST=perf: Performance mode = GXV_AUTOQ_TARGET=60 without the 30 cap, as perf.cpp)
    const char* t = getenv("GXV_AUTOQ_TARGET");
    const char* f = getenv("GXV_FPS_LIMIT");
    return (t && atoi(t) == 60 && !(f && atoi(f) == 30)) ? 1 : 0;
}
// (services of the current tree's units: the GX capture hooks, the front end's log and clock, the stream census)
#include <time.h>
void port_log(const char* fmt, ...) { (void)fmt; }
void port_fatal(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); abort(); }
int64_t port_host_time_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec; }
void port_stream_census_copy(void) {}
