// GX FIFO objects and the CPU->GPU command path.
//
// When the CPU FIFO is linked to the GP FIFO, GX writes land in a large host
// staging buffer that the command processor drains at sync points (GXFlush,
// draw-done, EFB peeks, overflow).  When unlinked (GXBeginDisplayList), writes
// go straight into the game's display-list buffer, with the SDK's overflow
// ("wrap") semantics.
#include <string.h>

#include "gpu.h"
#include "vgx_galaxy.h"
#include "port/gx_fifo.h"
#include "port/heap_routing.h"
#include "port/port.h"
#include "revolution/gx.h"
#include "revolution/gx/GXFifo.h"
#include "revolution/gx/GXPerf.h"
#include "revolution/os/OSInterrupt.h"

extern "C" {
uint8_t* __PortGXFifoPtr;
uint8_t* __PortGXFifoLimit;
}

static __GXFifoObj sCPUFifo;
static __GXFifoObj sGPFifo;
static bool sCPUReady, sGPReady, sLinked;

#if defined(__vita__) || defined(PORT_ARMHF_HARNESS)
static constexpr size_t kStagingSize = 2u << 20; // (a busy frame's stream is a few hundred KiB, drained at every flush)
#else
static constexpr size_t kStagingSize = 16u << 20;
#endif
static constexpr size_t kSlack = 64;
static uint8_t* sStaging;
static uint8_t sOverflowSink[256];
static bool sInFlush;
// Where writing into the staging buffer continues while the CPU FIFO is pointed elsewhere
// (a display list being recorded): with the renderer's player thread the buffer is not
// empty after a flush.
static uint8_t* sLinkedPtr;
// Player thread: the write position is also published every so many bytes between the GX
// library's own flushes, so that the player can work while the game is still drawing.
static constexpr size_t kPublishStep = 32u << 10;

static void setLinkedLimit() {
    uint8_t* hard = sStaging + kStagingSize - kSlack;
    uint8_t* soft = port_vgx_player() ? __PortGXFifoPtr + kPublishStep : hard;
    __PortGXFifoLimit = soft < hard ? soft : hard;
}
// The other staging segment (player thread: see vgx_galaxy.cpp).
static uint8_t* sStagingOther;
static void publishLinked(bool full) {
    uint8_t* const p = __PortGXFifoPtr;
    int flags = full ? 1 : 0;
    // A flush right after "draw done" or a draw-sync token: the game is about to wait for it.
    // (GXFlush pads with 32 zero bytes first, as on the console's write-gather pipe.)
    if (p - sStaging >= 37 && p[-1] == 0 && p[-32] == 0 && p[-37] == 0x61 && (p[-36] == 0x45 || p[-36] == 0x47 || p[-36] == 0x48)) flags |= 2;
    else if (p - sStaging >= 5 && p[-5] == 0x61 && (p[-4] == 0x45 || p[-4] == 0x47 || p[-4] == 0x48)) flags |= 2;
    uint8_t* const at = port_vgx_publish(p, flags);
    if (at < sStaging || at > sStaging + kStagingSize) {  // moved on to the other segment
        uint8_t* const t = sStaging;
        sStaging = sStagingOther;
        sStagingOther = t;
    }
    __PortGXFifoPtr = at;
    sLinkedPtr = at;
    setLinkedLimit();
}

static void bindWritePointer() {
    if (sLinked) {
        if (!sStaging) {
            sStaging = (uint8_t*)port_host_alloc(kStagingSize, 64);
            sLinkedPtr = sStaging;
            if (port_vgx_player()) {
                sStagingOther = (uint8_t*)port_host_alloc(kStagingSize, 64);
                port_vgx_stream_setup(sStaging, sStagingOther, kStagingSize - kSlack);
            }
        }
        if (__PortGXFifoPtr < sStaging || __PortGXFifoPtr >= sStaging + kStagingSize) {
            __PortGXFifoPtr = sLinkedPtr;
        }
        setLinkedLimit();
    } else if (sCPUReady) {
        __PortGXFifoPtr = (uint8_t*)sCPUFifo.wrPtr;
        __PortGXFifoLimit = sCPUFifo.top;
    } else {
        __PortGXFifoPtr = sOverflowSink;
        __PortGXFifoLimit = sOverflowSink + sizeof(sOverflowSink) - 16;
    }
}

// Drains the staging buffer through the command processor.
extern "C" void __PortGXFifoFlush(void) {
    if (!sLinked || !sStaging || sInFlush) {
        return;
    }
    if (port_vgx_player()) {
        publishLinked(false);
        return;
    }
    sInFlush = true;
    size_t len = (size_t)(__PortGXFifoPtr - sStaging);
    PortHostAllocScope hostAlloc;  // the GPU emulation allocates from the host heap
    int64_t t0 = port_host_time_ns();
    // The vgx renderer parses the stream itself; the legacy path goes through gpu::process.
#if defined(PORT_GX_VGX_ONLY)
    size_t used = port_vgx_consume(sStaging, len);
#else
    size_t used = port_vgx_active() ? port_vgx_consume(sStaging, len) : gpu::process(sStaging, len, false);
#endif
    port_perf_record(port_host_time_ns() - t0);
    size_t left = len - used;
    if (left) {
        memmove(sStaging, sStaging + used, left);
    }
    __PortGXFifoPtr = sStaging + left;
    sInFlush = false;
}

extern "C" void __PortGXFifoOverflow(void) {
    if (sLinked && port_vgx_player()) {
        // A publish step, or the end of the buffer (then the player is waited for).
        const bool full = __PortGXFifoPtr >= sStaging + kStagingSize - kSlack;
        publishLinked(full);
        if (full && __PortGXFifoPtr >= sStaging + kStagingSize - kSlack) {
            port_fatal("gx: command larger than the %zu MiB staging FIFO", kStagingSize >> 20);
        }
        return;
    }
    if (sLinked) {
        size_t before = (size_t)(__PortGXFifoPtr - sStaging);
        __PortGXFifoFlush();
        if ((size_t)(__PortGXFifoPtr - sStaging) >= before) {
            // A single command larger than the staging buffer; nothing we can do.
            port_fatal("gx: command larger than the %zu MiB staging FIFO", kStagingSize >> 20);
        }
        return;
    }
    // Display list overflow: remember it and discard further writes.
    if (__PortGXFifoPtr >= sOverflowSink && __PortGXFifoPtr < sOverflowSink + sizeof(sOverflowSink)) {
        __PortGXFifoPtr = sOverflowSink;
        return;
    }
    sCPUFifo.wrap = GX_TRUE;
    sCPUFifo.wrPtr = sCPUFifo.top;
    __PortGXFifoPtr = sOverflowSink;
    __PortGXFifoLimit = sOverflowSink + sizeof(sOverflowSink) - 16;
}

static void syncCPUWritePointer() {
    if (!sLinked && sCPUReady && !(__PortGXFifoPtr >= sOverflowSink && __PortGXFifoPtr < sOverflowSink + sizeof(sOverflowSink))) {
        sCPUFifo.wrPtr = __PortGXFifoPtr;
        sCPUFifo.count = (s32)((u8*)sCPUFifo.wrPtr - sCPUFifo.base);
    }
}

static bool linkCheck() {
    return sCPUReady && sGPReady && sCPUFifo.base == sGPFifo.base && sCPUFifo.top == sGPFifo.top;
}

extern "C" {

void GXInitFifoBase(GXFifoObj* fifo, void* base, u32 size) {
    __GXFifoObj* f = (__GXFifoObj*)fifo;
    f->base = (u8*)base;
    f->top = (u8*)base + size - 4;
    f->size = size;
    f->count = 0;
    f->hiWatermark = size - 0x4000;
    f->loWatermark = (size >> 1) & ~31u;
    f->rdPtr = base;
    f->wrPtr = base;
    f->wrap = GX_FALSE;
    f->bind_cpu = GX_FALSE;
    f->bind_gp = GX_FALSE;
}

void GXSetCPUFifo(const GXFifoObj* fifo) {
    syncCPUWritePointer();
    if (sLinked) sLinkedPtr = __PortGXFifoPtr;
    if (fifo == NULL) {
        sCPUReady = false;
        sLinked = false;
        bindWritePointer();
        return;
    }
    bool wasLinked = sLinked;
    sCPUFifo = *(const __GXFifoObj*)fifo;
    sCPUReady = true;
    sCPUFifo.bind_cpu = GX_TRUE;
    sLinked = linkCheck();
    sCPUFifo.bind_gp = sLinked ? GX_TRUE : GX_FALSE;
    if (wasLinked && !sLinked) {
        sLinked = true;  // (the flush is of the staging buffer the writes went to until now)
        __PortGXFifoFlush();
        sLinkedPtr = __PortGXFifoPtr;
        sLinked = false;
    }
    bindWritePointer();
}

void GXSetGPFifo(const GXFifoObj* fifo) {
    if (sLinked) sLinkedPtr = __PortGXFifoPtr;
    if (fifo == NULL) {
        sGPReady = false;
        sLinked = false;
        bindWritePointer();
        return;
    }
    sGPFifo = *(const __GXFifoObj*)fifo;
    sGPReady = true;
    sGPFifo.bind_gp = GX_TRUE;
    sLinked = linkCheck();
    bindWritePointer();
}

GXBool GXGetCPUFifo(GXFifoObj* fifo) {
    syncCPUWritePointer();
    if (!sCPUReady) {
        return GX_FALSE;
    }
    *(__GXFifoObj*)fifo = sCPUFifo;
    return GX_TRUE;
}

GXBool GXGetGPFifo(GXFifoObj* fifo) {
    if (!sGPReady) {
        return GX_FALSE;
    }
    *(__GXFifoObj*)fifo = sGPFifo;
    return GX_TRUE;
}

void GXGetFifoPtrs(const GXFifoObj* fifo, void** readPtr, void** writePtr) {
    const __GXFifoObj* f = (const __GXFifoObj*)fifo;
    *readPtr = f->rdPtr;
    *writePtr = f->wrPtr;
}

u32 GXGetFifoCount(const GXFifoObj* fifo) { return (u32)((const __GXFifoObj*)fifo)->count; }

GXBool GXGetFifoWrap(const GXFifoObj* fifo) { return ((const __GXFifoObj*)fifo)->wrap; }

void GXGetGPStatus(GXBool* overhi, GXBool* underlow, GXBool* readIdle, GXBool* cmdIdle, GXBool* brkpt) {
    __PortGXFifoFlush();
    *overhi = GX_FALSE;
    *underlow = GX_TRUE;
    *readIdle = GX_TRUE;
    *cmdIdle = GX_TRUE;
    *brkpt = GX_FALSE;
}

GXBool __GXIsGPFifoReady(void) { return sGPReady ? GX_TRUE : GX_FALSE; }
void __GXFifoInit(void) {}
void __GXCleanGPFifo(void) {
    // (with the player thread the buffer holds commands it has yet to read: nothing is thrown away)
    if (sStaging && !port_vgx_player()) {
        __PortGXFifoPtr = sStaging;
    }
}

void GXEnableBreakPt(void* pBreakPoint) { (void)pBreakPoint; }
void GXDisableBreakPt(void) {}
GXBreakPtCallback GXSetBreakPtCallback(GXBreakPtCallback cb) {
    (void)cb;
    return nullptr;
}

void GXSetGPMetric(GXPerf0 perf0, GXPerf1 perf1) {
    (void)perf0;
    (void)perf1;
}
void GXClearGPMetric(void) {}
void GXReadXfRasMetric(u32* xfWaitIn, u32* xfWaitOut, u32* rasBusy, u32* clocks) {
    *xfWaitIn = *xfWaitOut = *rasBusy = *clocks = 0;
}

u32 __PortGXPeekARGB(u16 x, u16 y) {
    // vgx: no read-back of the frame buffer (it would stall the whole pipeline, and BrightObj
    // peeks 17 pixels per object per frame). "Fully visible" is the answer that test wants.
    if (port_vgx_active()) return 0xFFFFFFFFu;
#if defined(PORT_GX_VGX_ONLY)
    return 0xFFFFFFFFu;
#else
    __PortGXFifoFlush();
    gpu::Backend* b = gpu::backend();
    return b ? b->peekColor(x, y) : 0;
#endif
}

u32 __PortGXPeekZ(u16 x, u16 y) {
    if (port_vgx_active()) return 0x00FFFFFFu; // far: see __PortGXPeekARGB
#if defined(PORT_GX_VGX_ONLY)
    return 0x00FFFFFFu;
#else
    __PortGXFifoFlush();
    gpu::Backend* b = gpu::backend();
    return b ? b->peekZ(x, y) : 0x00FFFFFF;
#endif
}

}  // extern "C"

// PE interrupts are forwarded to the handlers the GX library registered.
extern "C" void port_call_sdk_interrupt(int interrupt);

static void peTokenIrq() { port_call_sdk_interrupt(__OS_INTERRUPT_PI_PE_TOKEN); }
static void peFinishIrq() { port_call_sdk_interrupt(__OS_INTERRUPT_PI_PE_FINISH); }

extern "C" void port_gx_init(void) {
    __PortGXFifoPtr = sOverflowSink;
    __PortGXFifoLimit = sOverflowSink + sizeof(sOverflowSink) - 16;
    port_irq_set_handler(PORT_IRQ_PE_TOKEN, peTokenIrq);
    port_irq_set_handler(PORT_IRQ_PE_FINISH, peFinishIrq);
}
