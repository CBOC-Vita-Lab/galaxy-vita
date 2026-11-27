// GX FIFO objects and the CPU->GPU command path.
//
// When the CPU FIFO is linked to the GP FIFO, GX writes land in a large host
// staging buffer that the command processor drains at sync points (GXFlush,
// draw-done, EFB peeks, overflow).  When unlinked (GXBeginDisplayList), writes
// go straight into the game's display-list buffer, with the SDK's overflow
// ("wrap") semantics.
#include <string.h>

#define PORT_GX_FIFO_BATCH_OWNER 1  // (this file defines __PortGXFifoBatch, port/gx_fifo.h)
#define PORT_GX_DEDUP_OWNER 1       // (and __PortGXDedupOn / __PortGXDedupKeep, port/gx_dedup.h: gx_dedup.inc)
#include "gpu.h"
#include "vgx_galaxy.h"
#include "port/gx_fifo.h"
#include "port/gx_capture.h"
#include "port/gx_dedup.h"  // (GXV_GX_DEDUP: the gate behind the register-write macros, gx_dedup.inc below; the FIFO's own invalidations)
#include "port/pipe25.h"  // (GXV_PIPE25: the raw FIFO's chain)
#include "port/heap_routing.h"
#include "port/port.h"
#include "port/hot_knob.h"
#include "revolution/gx.h"
#include "revolution/gx/GXFifo.h"
#include "revolution/gx/GXPerf.h"
#include "revolution/os/OSInterrupt.h"
#if defined(TARGET_PC)
#include <errno.h>
#include <fenv.h>
#include "port/diagnostics.h"
#include "../../vgx/src/vgx_common.h"
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#endif

extern "C" {
uint8_t* __PortGXFifoPtr;
uint8_t* __PortGXFifoLimit;
// GXV_FIFO_BATCH (default 1, read once in port_gx_init): multi-element GX commands written as one batch, see port/gx_fifo.h.
// 0 until then (element-wise), and with GXV_FIFO_BATCH=0 in env.txt.
#if !defined(PORT_GX_FIFO_BATCH_WEAK)
int __PortGXFifoBatch = 0;
#endif
uint32_t __PortGXFifoOverflowSerial = 0;  // (GXV_REPLAY25, port/gx_fifo.h: one more at every overflow / publish step)
}

#if defined(TARGET_PC)
namespace {
int xfBurstMode() {
    // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h; every GXLoadPosMtxImm / GXLoadTexMtxImm of the game asks)
    return PORT_HOT_KNOB(int, [] {
        const int saved = errno;
        // Default-off exact XF payload batching: 1 fast, 2 original-write oracle.
        // Cache once; preserve errno even when EnvInt rejects an overflow value.
        const long value = vgx::EnvInt("GXV_XFBURST", 0);
        errno = saved;
        return value == 1 || value == 2 ? int(value) : 0;
    }());
}

bool overlaps(uintptr_t a, size_t an, uintptr_t b, size_t bn) {
    // Callers establish non-wrapping ranges before these unsigned differences.
    return a - b < bn || b - a < an;
}

void xfPayload12(uint8_t* __restrict out, const uint8_t* __restrict source) {
#if defined(__ARM_NEON)
    vst1q_u8(out, vrev32q_u8(vld1q_u8(source)));
    vst1q_u8(out + 16, vrev32q_u8(vld1q_u8(source + 16)));
    vst1q_u8(out + 32, vrev32q_u8(vld1q_u8(source + 32)));
#else
    for (unsigned i = 0; i < 12; ++i) {
        uint32_t word;
        memcpy(&word, source + i * 4, 4);
        word = __builtin_bswap32(word);
        memcpy(out + i * 4, &word, 4);
    }
#endif
}

#if PORT_RUNTIME_DIAGNOSTICS && PORT_LOGGING
void xfOracleReport(unsigned words, uint64_t checked, uint64_t mismatches, bool mismatch) {
    if (!(mismatch ? mismatches <= 4 : (checked & (checked - 1)) == 0)) return;
    const int saved = errno;
    fenv_t environment;
    if (fegetenv(&environment) == 0) {
        port_log(mismatch ? "xfburst mismatch words=%u checked=%llu mismatches=%llu" :
                            "xfburst oracle words=%u checked=%llu mismatches=%llu", words,
                 static_cast<unsigned long long>(checked), static_cast<unsigned long long>(mismatches));
        fesetenv(&environment);
    }
    errno = saved;
}
#endif
int xfBurst12(const float* source) {
    const int mode = xfBurstMode();
    if (!mode) return 0;
#if !(PORT_RUNTIME_DIAGNOSTICS && PORT_LOGGING)
    if (mode == 2) return 0; // No verification claims from a quiet build.
#endif
    const size_t bytes = 48, sourceBytes = 48;
    const uintptr_t p = reinterpret_cast<uintptr_t>(__PortGXFifoPtr);
    const uintptr_t limit = reinterpret_cast<uintptr_t>(__PortGXFifoLimit);
    const uintptr_t src = reinterpret_cast<uintptr_t>(source);
    // Equality would publish/overflow after the last scalar write. Keep that
    // path, all aliasing and malformed ranges exactly as the original loop.
    if (!p || !src || p >= limit || bytes >= limit - p || sourceBytes > UINTPTR_MAX - src ||
        overlaps(p, bytes, src, sourceBytes) ||
        overlaps(p, bytes, reinterpret_cast<uintptr_t>(&__PortGXFifoPtr), sizeof __PortGXFifoPtr) ||
        overlaps(p, bytes, reinterpret_cast<uintptr_t>(&__PortGXFifoLimit), sizeof __PortGXFifoLimit) ||
        overlaps(src, sourceBytes, reinterpret_cast<uintptr_t>(&__PortGXFifoPtr), sizeof __PortGXFifoPtr) ||
        overlaps(src, sourceBytes, reinterpret_cast<uintptr_t>(&__PortGXFifoLimit), sizeof __PortGXFifoLimit)) return 0;
    uint8_t* const start = __PortGXFifoPtr;
    if (mode == 1) {
        xfPayload12(start, reinterpret_cast<const uint8_t*>(source));
        __PortGXFifoPtr = start + bytes;
        return 1;
    }
#if PORT_RUNTIME_DIAGNOSTICS && PORT_LOGGING
    uint8_t candidate[48];
    xfPayload12(candidate, reinterpret_cast<const uint8_t*>(source));
    const float (*matrix)[4] = reinterpret_cast<const float (*)[4]>(source);
    for (unsigned i = 0; i < 12; ++i)
        __PortGXFifo_f32(matrix[i / 4][i % 4]);
    const bool mismatch = memcmp(candidate, start, bytes) != 0;
    // The FIFO is already a serialized single-producer path. No fast-mode
    // counters, additional submissions or callbacks are introduced here.
    static uint64_t checked = 0, mismatches = 0;
    ++checked;
    if (mismatch) ++mismatches;
    xfOracleReport(12, checked, mismatches, mismatch);
#endif
    return 1;
}
} // namespace

extern "C" int __PortGXFifoXfBurst12(const float* source) { return xfBurst12(source); }
#endif

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

// ---- GXV_PIPE25 (pipe26): the raw FIFO. While passes wait as tokens, the game thread's writes go into a chain of chunks instead of the
// staging buffer (the passes will write their bytes into the stream, the chain is copied behind each: port_gx_raw_*, below). The
// staging buffer's own events (a flush, an idle point, a publish step) are not the chain's: nothing is published while it is on.
//   sRawOpen    armed at the draw's start (port_gx_raw_open) until the splice is over
//   sRawOn      the FIFO writes into the chain (from the first token)
//   sRawClosed  the draw is over, the splice not done: the game thread's FIFO points at a sink (a stray inline writer loses its bytes
//               instead of corrupting the stream)
//   sRawLo/Hi   the memory that stands for the stream now (the chain's chunk, or verify's scratch buffer): a range the "FIFO feeds the
//               stream" tests accept besides the staging buffer
static port::pipe25::RawChain sRawChain;
static bool sRawOpen, sRawOn, sRawClosed, sScratchOn, sScratchFail;
static uint8_t* sRawTail;           // the staging position where the first token stood: the stream continues there
static uint8_t *sRawLo, *sRawHi;
static uint8_t sRawSink[256];
static uint8_t *sScratchSavePtr, *sScratchSaveLimit, *sScratchSaveLinked;
static inline bool rawHolds() { return sRawOn || sRawClosed || sScratchOn; }
static inline bool inRawRange(const uint8_t* p) { return sRawLo && p >= sRawLo && p < sRawHi; }

static void setLinkedLimit() {
    if (sRawOn) {  // (the chain's chunk: no publish steps)
        __PortGXFifoLimit = sRawChain.Limit();
        return;
    }
    uint8_t* hard = sStaging + kStagingSize - kSlack;
    uint8_t* soft = port_vgx_player() ? __PortGXFifoPtr + kPublishStep : hard;
    __PortGXFifoLimit = soft < hard ? soft : hard;
}
// The other staging segment (player thread: see vgx_galaxy.cpp).
static uint8_t* sStagingOther;
static uint32_t sSegFlips;  // (GXV_PIPE25=2: segment changes, so that a pass's bytes can be told to lie in one segment: port_gx_seg_flips)
static void publishLinked(bool full, int extraFlags = 0) {
    uint8_t* const p = __PortGXFifoPtr;
    int flags = (full ? 1 : 0) | extraFlags;
    // A flush right after "draw done" or a draw-sync token: the game is about to wait for it.
    // (GXFlush pads with 32 zero bytes first, as on the console's write-gather pipe.)
    if (p - sStaging >= 37 && p[-1] == 0 && p[-32] == 0 && p[-37] == 0x61 && (p[-36] == 0x45 || p[-36] == 0x47 || p[-36] == 0x48)) flags |= 2;
    else if (p - sStaging >= 5 && p[-5] == 0x61 && (p[-4] == 0x45 || p[-4] == 0x47 || p[-4] == 0x48)) flags |= 2;
    uint8_t* const at = port_vgx_publish(p, flags);
    if (at < sStaging || at > sStaging + kStagingSize) {  // moved on to the other segment
        ++sSegFlips;
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
        if (sRawOn ? !inRawRange(__PortGXFifoPtr) : (__PortGXFifoPtr < sStaging || __PortGXFifoPtr >= sStaging + kStagingSize)) {
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
static void fifoFlush();
// (GX capture, port/gx_capture.h: while a capture or mode 2's scratch replay owns the FIFO, its events are the layer's first)
extern "C" void __PortGXFifoFlush(void) {
    if (__builtin_expect(__PortGXCapFifo != 0, 0)) {
        if (port_gxcap_fifo_pre(PORT_GXCAP_EV_FLUSH)) return;
        fifoFlush();
        port_gxcap_fifo_post(PORT_GXCAP_EV_FLUSH);
        return;
    }
    fifoFlush();
}
static void fifoFlush() {
    if (!sLinked || !sStaging || sInFlush || rawHolds()) {  // (the raw FIFO: nothing is published until the splice)
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

// GXV_SEG_IDLE (platform/src/gx/vgx_galaxy.cpp, port_vgx_publish): the game thread at the start of a turn, between its draws; it
// takes the other staging segment now if the player thread has parsed everything published. Only while the FIFO writes into the
// staging buffer (not while a display list is recorded) and the player thread runs. Game thread only.
static void fifoIdlePoint();
extern "C" void __PortGXFifoIdlePoint(void) {
    if (__builtin_expect(__PortGXCapFifo != 0, 0)) {
        if (port_gxcap_fifo_pre(PORT_GXCAP_EV_IDLE)) return;
        fifoIdlePoint();
        port_gxcap_fifo_post(PORT_GXCAP_EV_IDLE);
        return;
    }
    fifoIdlePoint();
}
static void fifoIdlePoint() {
    if (!sLinked || !sStaging || sInFlush || rawHolds() || !port_vgx_seg_idle()) {
        return;
    }
    publishLinked(false, 4);
}

static void fifoOverflow();
extern "C" void __PortGXFifoOverflow(void) {
    ++__PortGXFifoOverflowSerial;
    if (__builtin_expect(__PortGXCapFifo != 0, 0)) {
        if (port_gxcap_fifo_pre(PORT_GXCAP_EV_OVERFLOW)) return;
        fifoOverflow();
        port_gxcap_fifo_post(PORT_GXCAP_EV_OVERFLOW);
        return;
    }
    fifoOverflow();
}
#if !defined(TARGET_PC)
static void rawRoll() {}
#else
static void rawRoll();
#endif
static void fifoOverflow() {
    if (sRawClosed) {  // (a stray writer between the draw and the splice: its bytes go nowhere)
        __PortGXFifoPtr = sRawSink;
        __PortGXFifoLimit = sRawSink + sizeof(sRawSink) - 16;
        return;
    }
    if (sScratchOn && inRawRange(__PortGXFifoPtr)) {  // (verify's scratch buffer is too small: the pass is not compared)
        sScratchFail = true;
        __PortGXFifoPtr = sRawLo;
        return;
    }
    if (sRawOn && inRawRange(__PortGXFifoPtr)) {  // (the chain's chunk is full: on to the next)
        rawRoll();
        return;
    }
    if (sLinked && port_vgx_player()) {
        // A publish step, or the end of the buffer (then the player is waited for).
        const bool full = __PortGXFifoPtr >= sStaging + kStagingSize - kSlack;
        publishLinked(full);
        if (full && __PortGXFifoPtr >= sStaging + kStagingSize - kSlack) {
            port_fatal("gx: command larger than the %lu MiB staging FIFO", (unsigned long)(kStagingSize >> 20));
        }
        return;
    }
    if (sLinked) {
        size_t before = (size_t)(__PortGXFifoPtr - sStaging);
        __PortGXFifoFlush();
        if ((size_t)(__PortGXFifoPtr - sStaging) >= before) {
            // A single command larger than the staging buffer; nothing we can do.
            port_fatal("gx: command larger than the %lu MiB staging FIFO", (unsigned long)(kStagingSize >> 20));
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
    PORT_GXCAP_ABANDON(GXSetCPUFifo);
#if defined(TARGET_PC)
    __PortGXDedupBump(PORT_GXDD_BUMP_ALL, PORT_GXDD_CAUSE_FIFO);  // (GXV_GX_DEDUP: the FIFO is bound again)
#endif
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
    PORT_GXCAP_ABANDON(GXSetGPFifo);
#if defined(TARGET_PC)
    __PortGXDedupBump(PORT_GXDD_BUMP_ALL, PORT_GXDD_CAUSE_FIFO);
#endif
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
    PORT_GXCAP_ABANDON(GXGetCPUFifo);
    syncCPUWritePointer();
    if (!sCPUReady) {
        return GX_FALSE;
    }
    *(__GXFifoObj*)fifo = sCPUFifo;
    return GX_TRUE;
}

GXBool GXGetGPFifo(GXFifoObj* fifo) {
    PORT_GXCAP_ABANDON(GXGetGPFifo);
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
    PORT_GXCAP_ABANDON(GXGetGPStatus);
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
#if defined(TARGET_PC)
    __PortGXDedupBump(PORT_GXDD_BUMP_ALL, PORT_GXDD_CAUSE_FIFO);  // (GXV_GX_DEDUP: written bytes may be thrown away here)
#endif
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

// (GX capture, port/gx_capture.h) 1: GXPeekARGB / GXPeekZ answer a constant and read nothing (the capture gives that answer).
int __PortGXPeekConst(void) {
#if defined(PORT_GX_VGX_ONLY)
    return 1;
#else
    return port_vgx_active() ? 1 : 0;
#endif
}
// (GX capture) 1: the FIFO writes into the staging buffer the stream is made of (not into a display list, not into the overflow
// sink): a capture may start, and the calls it records meet the same FIFO at replay.
int __PortGXFifoInStream(void) {
    return sLinked && sStaging && ((__PortGXFifoPtr >= sStaging && __PortGXFifoPtr < sStaging + kStagingSize) || inRawRange(__PortGXFifoPtr)) ? 1 : 0;
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

#if defined(TARGET_PC)
// GXV_FXMERGE (default 0 for now, the console A/B decides; 1: on; read once, GXV_HOT_KNOBS): JParticle's particle loops go to the
// front end as port-private commands (the gathering: decomp/src/RVL_SDK/gx/GXDisplayList.c; the front end and why its pictures are the
// same: platform/vgx/src/vgx_state.cpp, FxMergeRun). Only while the FIFO writes into the staging buffer that the vgx front end reads:
// not while a display list is recorded, not with another renderer.
static bool sFxForeign;
static int fxMergeKnob() {
    return PORT_HOT_KNOB(int, [] {
        const int saved = errno;
        const long v = vgx::EnvInt("GXV_FXMERGE", 0);
        errno = saved;
        return v == 1 ? 1 : 0;
    }());
}
// (GX capture: while mode 2 replays a record into its scratch buffer, that buffer stands for the stream: port_gxcap_stream_override)
static bool writesStream() {
    return (__PortGXFifoPtr >= sStaging && __PortGXFifoPtr < sStaging + kStagingSize) || inRawRange(__PortGXFifoPtr) ||
           (__builtin_expect(__PortGXCapFifo != 0, 0) && port_gxcap_stream_override());
}
extern "C" int __PortGXFxMergeOk(void) {
    return fxMergeKnob() && !sFxForeign && sLinked && sStaging && port_vgx_active() && writesStream();
}
// GXV_RETAINED (default 0; 1: on, 2: on, the front end verifies; read once, GXV_HOT_KNOBS): J3D's shape calls carry a handle for the
// front end (decomp/src/RVL_SDK/gx/GXDisplayList.c port_gx_call_retained; the front end: platform/vgx/src/vgx_dlcache.cpp, "retained
// shapes"). As GXV_FXMERGE, only while the FIFO writes into the staging buffer the vgx front end reads.
static int retainedKnob() {
    return PORT_HOT_KNOB(int, [] {
        const int saved = errno;
        const long v = vgx::EnvInt("GXV_RETAINED", 0);
        const long d = vgx::EnvInt("GXV_DIRECT_J3D", 0); // (GXV_DIRECT_J3D, vgx_direct.h: its templates are kept with the retained records)
        errno = saved;
        return v >= 1 || d >= 1 ? 1 : 0;
    }());
}
// GXV_J3DNATIVE (default 0; 1 census, 2 verify, 3 on, 4 stage-2 verify, 5 stage 2 on; read once, GXV_HOT_KNOBS): the material / differed list calls carry a bracket (see
// platform/include/port/gx_fifo.h). The mode is the front end's too (vgx_j3native.inc reads the same variable). Independent of
// GXV_RETAINED (the census counts the shape calls that carry a handle; run with GXV_RETAINED=1 GXV_DIRECT_J3D=1 as the console does).
static int j3nKnob() {
    return PORT_HOT_KNOB(int, [] {
        const int saved = errno;
        const long v = vgx::EnvInt("GXV_J3DNATIVE", 0);
        errno = saved;
        return v >= 1 && v <= 5 ? static_cast<int>(v) : 0; // (j3n2: 4 / 5 are stage 2's; before, they sent no bracket at all)
    }());
}
extern "C" int __PortGXJ3nMode(void) {
    return j3nKnob() && sLinked && sStaging && port_vgx_active() ? j3nKnob() : 0;
}
extern "C" int __PortGXBracketOk(void) {
    return j3nKnob() && sLinked && sStaging && port_vgx_active() && writesStream();
}
// GXV_REPLAY25 (port/gx_fifo.h): bytes may be copied into the stream: the FIFO is the staging buffer of the vgx front end, no capture
// is open (a capture records calls, not bytes) and the particle gathering is off.
extern "C" int __PortGXReplayOk(void) {
    return sLinked && sStaging && port_vgx_active() && writesStream() && __PortGXCap == 0 && !__PortGXFx;
}
extern "C" int __PortGXRetainedOk(void) {
    return retainedKnob() && sLinked && sStaging && port_vgx_active() && writesStream();
}
// A run of bytes into the stream, as element writes would put them: up to the limit, then __PortGXFifoOverflow (a publish step, or the
// end of the segment), and on.
extern "C" void __PortGXFifoWriteBytes(const void* src, uint32_t n) {
    const uint8_t* s = static_cast<const uint8_t*>(src);
    while (n) {
        const size_t room = __PortGXFifoLimit > __PortGXFifoPtr ? static_cast<size_t>(__PortGXFifoLimit - __PortGXFifoPtr) : 0;
        if (!room) {
            __PortGXFifoOverflow();
            continue;
        }
        const size_t k = n < room ? n : room;
        memcpy(__PortGXFifoPtr, s, k);
        __PortGXFifoPtr += k;
        s += k;
        n -= static_cast<uint32_t>(k);
        if (__PortGXFifoPtr >= __PortGXFifoLimit) __PortGXFifoOverflow();
    }
}
// The gathering found the write position moved between two of its items: a GX call it does not take was made inside a particle loop
// (JPAResource's list of particle functions should rule that out). The order of that picture's commands may be wrong; from now on the
// particle loops are sent as before.
extern "C" void __PortGXFxForeign(void) {
    if (!sFxForeign) port_log("gx: GXV_FXMERGE: a GX write inside a particle loop that the gathering does not take; gathering off from now on");
    sFxForeign = true;
}

#include "gx_dedup.inc"  // GXV_GX_DEDUP / GXV_GX_DEDUP_CENSUS (gxdedup31): the register-write gate
#endif


#if defined(TARGET_PC)
// ---- GXV_PIPE25 (pipe26): the raw FIFO's functions. The game thread: port_gx_raw_open at the draw's start, port_gx_raw_token at each pass,
// port_gx_raw_close after endRender. Then the thread that owns the GX library (the worker, or the game thread when it takes the job
// back or abandons): port_gx_raw_splice. Verify (GXV_PIPE25=2) runs a pass into a scratch buffer: port_gx_scratch_begin / _end.
static void rawRoll() {
    uint8_t* const nb = sRawChain.Roll(__PortGXFifoPtr);
    if (!nb) port_fatal("gx: no memory for the late draw's chain");
    sRawLo = nb;
    sRawHi = sRawChain.End();
    __PortGXFifoPtr = nb;
    sLinkedPtr = nb;
    setLinkedLimit();
}
extern "C" int port_gx_raw_open(void) {
    if (sRawOpen || sScratchOn || !sLinked || !sStaging || sInFlush || !port_vgx_player()) return 0;
    if (__PortGXCap != 0 || __PortGXCapFifo != 0) return 0;  // (a capture, or its scratch replay, owns the FIFO)
#if defined(TARGET_PC)
    if (__PortGXFx) return 0;
#endif
    if (__PortGXFifoPtr < sStaging || __PortGXFifoPtr >= sStaging + kStagingSize) return 0;  // (the FIFO is not the stream: a display list is recorded)
    sRawOpen = true, sRawOn = false, sRawClosed = false;
    return 1;
}
// 1: the pass is deferred (its place is marked); 0: not possible (the caller runs the pass where it stands).
extern "C" int port_gx_raw_token(void) {
    if (!sRawOpen) return 0;
    if (!sRawOn) {
        if (!sLinked || __PortGXFifoPtr < sStaging || __PortGXFifoPtr >= sStaging + kStagingSize) return 0;
        publishLinked(false);  // (what the front end can have of the draw so far it has now; the stream continues where this leaves the write position)
        uint8_t* const b = sRawChain.Open();
        if (!b) return 0;
        sRawTail = __PortGXFifoPtr;
        sRawOn = true;
        sRawLo = b;
        sRawHi = sRawChain.End();
        __PortGXFifoPtr = b;
        sLinkedPtr = b;
        setLinkedLimit();
    }
    if (!sLinked) return 0;  // (the draw is inside a display list recording: not at a token)
    sRawChain.Mark(__PortGXFifoPtr);
    return 1;
}
extern "C" int port_gx_raw_active(void) { return sRawOpen; }
// The draw's end (after endRender): the number of deferred passes. 0: none, the FIFO is the stream as before. Otherwise the game thread's
// FIFO points at a sink until the splice.
extern "C" uint32_t port_gx_raw_close(void) {
    if (!sRawOpen) return 0;
    if (!sRawOn) {
        sRawOpen = false;
        return 0;
    }
    sRawChain.Close(sLinked ? __PortGXFifoPtr : sLinkedPtr);
    sRawOn = false;
    sRawClosed = true;
    sRawLo = sRawHi = nullptr;
    __PortGXFifoPtr = sRawSink;
    __PortGXFifoLimit = sRawSink + sizeof(sRawSink) - 16;
    return sRawChain.Marks();
}
extern "C" uint64_t port_gx_raw_bytes(void) { return sRawChain.Total(); }
extern "C" uint64_t port_gx_raw_high(void) { return sRawChain.HighWater(); }
extern "C" uint32_t port_gx_raw_chunks(void) { return sRawChain.Chunks(); }
// pipe27: the splice in three parts, so that its owner can change between two passes (the late helper stops at a pass boundary when
// the game thread takes over, GXV_PIPE25_TAKEOVER_US). begin: the number of passes (0: no closed chain); the FIFO is the stream's tail.
// step(i): pass i (`token`), then the bytes Bi. end: after the last step.
extern "C" uint32_t port_gx_raw_splice_begin(void) {
    if (!sRawClosed) return 0;
    const uint32_t n = sRawChain.Marks();
    sRawClosed = false;
    __PortGXFifoPtr = sRawTail;
    sLinkedPtr = sRawTail;
    setLinkedLimit();
    return n;
}
extern "C" void port_gx_raw_splice_step(uint32_t i, void (*token)(uint32_t id, void* ctx), void* ctx) {
    token(i, ctx);
    sRawChain.CopyAfter(i, [](const uint8_t* p, size_t len) { __PortGXFifoWriteBytes(p, static_cast<uint32_t>(len)); });
}
extern "C" void port_gx_raw_splice_end(void) {
    sRawChain.Release();
    sRawOpen = false;
    publishLinked(false);  // (as the flush that ended the draw would have: the front end gets the tail)
}
extern "C" uint32_t port_gx_seg_flips(void) { return sSegFlips; }
// On the thread that owns the library: pass 0, then its Bi, pass 1 ... into the staging buffer from where the first token stood;
// `token(i, ctx)` runs pass i (its bytes go to the FIFO, which is the stream now). Ends with the stream published.
extern "C" void port_gx_raw_splice(void (*token)(uint32_t id, void* ctx), void* ctx) {
    if (!sRawClosed) return;
    const uint32_t n = port_gx_raw_splice_begin();
    for (uint32_t i = 0; i < n; ++i) {
        port_gx_raw_splice_step(i, token, ctx);
    }
    port_gx_raw_splice_end();
}
// The game thread, in the middle of the draw: the deferred passes run now and the draw goes on serially. 0: not possible (a display list
// is being recorded: the engine turns the pipeline off).
extern "C" int port_gx_raw_abandon(void (*token)(uint32_t id, void* ctx), void* ctx) {
    if (!sRawOpen) return 1;
    if (!sRawOn) {
        sRawOpen = false;
        return 1;
    }
    if (!sLinked) return 0;
    sRawChain.Close(__PortGXFifoPtr);
    sRawOn = false;
    sRawClosed = true;
    sRawLo = sRawHi = nullptr;
    port_gx_raw_splice(token, ctx);
    return 1;
}
// VERIFY: the pass runs on the calling thread (the worker, with the game thread idle) into `buf`; the FIFO is put back after.
extern "C" int port_gx_scratch_begin(uint8_t* buf, uint32_t cap) {
    if (sScratchOn || sRawOn || sRawClosed || cap < 4096) return 0;
    sScratchSavePtr = __PortGXFifoPtr, sScratchSaveLimit = __PortGXFifoLimit, sScratchSaveLinked = sLinkedPtr;
    sScratchOn = true, sScratchFail = false;
    sRawLo = buf;
    sRawHi = buf + cap;
    __PortGXFifoPtr = buf;
    __PortGXFifoLimit = buf + cap - 64;
    return 1;
}
// The bytes written (the last pass's), or 0xFFFFFFFF when the buffer was too small.
extern "C" uint32_t port_gx_scratch_end(void) {
    if (!sScratchOn) return 0xFFFFFFFFu;
    const uint32_t n = sScratchFail ? 0xFFFFFFFFu : static_cast<uint32_t>(__PortGXFifoPtr - sRawLo);
    __PortGXFifoPtr = sScratchSavePtr, __PortGXFifoLimit = sScratchSaveLimit, sLinkedPtr = sScratchSaveLinked;
    sRawLo = sRawHi = nullptr;
    sScratchOn = false;
    return n;
}
// The bytes written so far into the scratch buffer (VERIFY: where a model's bytes begin).
extern "C" uint32_t port_gx_scratch_pos(void) { return sScratchOn ? static_cast<uint32_t>(__PortGXFifoPtr - sRawLo) : 0xFFFFFFFFu; }

#endif  // TARGET_PC

// PE interrupts are forwarded to the handlers the GX library registered.
extern "C" void port_call_sdk_interrupt(int interrupt);

static void peTokenIrq() { port_call_sdk_interrupt(__OS_INTERRUPT_PI_PE_TOKEN); }
static void peFinishIrq() { port_call_sdk_interrupt(__OS_INTERRUPT_PI_PE_FINISH); }

extern "C" void port_gx_init(void) {
#if defined(TARGET_PC)
    {
        const int saved = errno;
        __PortGXFifoBatch = vgx::EnvInt("GXV_FIFO_BATCH", 1) == 1 ? 1 : 0;
        errno = saved;
        port_log("gx: command batching %s (GXV_FIFO_BATCH)", __PortGXFifoBatch ? "on" : "off");
    }
    port_gx_dedup_init();  // (GXV_GX_DEDUP, GXV_GX_DEDUP_CENSUS: read once)
#endif
    __PortGXFifoPtr = sOverflowSink;
    __PortGXFifoLimit = sOverflowSink + sizeof(sOverflowSink) - 16;
    port_irq_set_handler(PORT_IRQ_PE_TOKEN, peTokenIrq);
    port_irq_set_handler(PORT_IRQ_PE_FINISH, peFinishIrq);
    port_gx_capture_init();  // (GX capture: GXV_GXCAPTURE and its neighbours, read once)
}
