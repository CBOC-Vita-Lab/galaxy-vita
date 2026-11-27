// stall23 (Galaxy Vita 2.3, GXV_STUTTER_WAITS): what the back end's "stutter:" lines cannot see from the inside - the shader compiler's runs
// and the render thread's waits for the GPU (platform/vgx/src/vgx_backend_gl.cpp, StutterFrameEnd reads vgxvita_waitStats_v1).
// Link wrappers (CMakeLists.txt adds their --wrap, only in the build without VGX_COLDTRACE_WRAPPERS: that build wraps the same calls in
// coldtrace_wrap.cpp, and then these counts are simply absent):
//   shark_compile_shader_extended  vitaGL's runtime compile (glCompileShader / glLinkProgram when no cache has the text; also the
//                                  GXV_ASYNC_SHADERS worker): counted on any thread
//   sceGxmFinish                   glFinish, program deletes, vitaGL's allocation recovery: a wait for all of the GPU's work
//   sceGxmBeginScene               a scene that has to wait for the GPU (a render target's scene slots, the parameter buffer)
//   sceGxmNotificationWait         fences (texture upload syncs, queries)
// The three GXM calls are counted only on the thread that called vgxvita_waitStatsStart_v1 (the back end's render thread, at Init; vitaGL's
// GPU-timer watcher waits on notifications all the time). Until then, or with on = 0, every wrapper only passes the call on.
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <vitashark.h>
#include <atomic>
#include <cstdint>
#include <type_traits>

// (dma31) clock_census.cpp: told of every picture (GXV_CLOCK_CENSUS)
extern "C" void vgxvita_clockCensusPicture_v1(void);
// (copy49) smallcopy_wrap.cpp: told of every picture (GXV_SMALLCOPY, GXV_COPY_CENSUS)
extern "C" void vgxvita_copyCensusPicture_v1(void);
// (packrefill25) vgx_host_galaxy_vita.cpp: the compiler run just made, its microseconds (weak: absent in the coldtrace build and on the desktop)
extern "C" void vgxvita_packNoteCompile_v1(uint64_t us) __attribute__((weak));

namespace {
enum : unsigned {
    kCompiles, kCompileUs, kFinishes, kFinishUs, kSceneUs, kSlowScenes, kFenceUs, kFences, // (v1: out[8])
    // bowser24 (v2): the render thread's other waits. us and calls; a call is also counted when it did not wait.
    kEndSceneUs, kEndScenes, kDispUs, kDisps, kResUs, kReserves, kXferUs, kXfers, kSemaUs, kSemas, kDelayUs, kDelays, kDrawUs, kDraws,
    kCount
};
constexpr unsigned kV1 = 8;
std::atomic<int> g_on{0}; // (0 off, 1 waits, 2 also every sceGxmDraw's own time)
std::atomic<int> g_thread{-1};
// (64-bit sums kept as two 32-bit halves: no 64-bit atomics on the ARM side; the low half's carry goes into the high half)
std::atomic<uint32_t> g_lo[kCount];
std::atomic<uint32_t> g_hi[kCount];
void Add(unsigned k, uint64_t v) {
    const uint32_t add = static_cast<uint32_t>(v);
    const uint32_t before = g_lo[k].fetch_add(add, std::memory_order_relaxed);
    uint32_t hi = static_cast<uint32_t>(v >> 32);
    if (static_cast<uint32_t>(before + add) < before) ++hi;
    if (hi) g_hi[k].fetch_add(hi, std::memory_order_relaxed);
}
// midflush31 (GXV_RING_CENSUS): the calls that could wait for a full sceGxm ring (a reserve; a draw, when GXV_BE_SPLIT times draws): the calls of 300 us or more
// (a call that does not wait takes 1-5 us), their sum and the longest one, read and reset by vgxvita_ringStats_v1. Written on the render thread only.
constexpr uint64_t kSlowUs = 300;
uint32_t g_slow[2], g_slowUs[2], g_slowMax[2]; // (0 reserve, 1 draw)
inline void NoteSlow(unsigned k, uint64_t us) {
    if (us < kSlowUs) return;
    ++g_slow[k]; g_slowUs[k] += static_cast<uint32_t>(us);
    if (us > g_slowMax[k]) g_slowMax[k] = static_cast<uint32_t>(us);
}
inline bool Mine() { return g_on.load(std::memory_order_relaxed) != 0 && sceKernelGetThreadId() == g_thread.load(std::memory_order_relaxed); }
inline uint64_t NowUs() { return sceKernelGetProcessTimeWide(); }
} // namespace

extern "C" {
void vgxvita_waitStatsStart_v1(int on) {
    g_thread.store(sceKernelGetThreadId(), std::memory_order_relaxed);
    g_on.store(on ? 1 : 0, std::memory_order_relaxed);
}
// bowser24: mode 0 off, 1 the waits below, 2 also each sceGxmDraw's time (two clock reads per draw)
void vgxvita_waitStatsStart_v2(int mode) {
    g_thread.store(sceKernelGetThreadId(), std::memory_order_relaxed);
    g_on.store(mode < 0 ? 0 : (mode > 2 ? 2 : mode), std::memory_order_relaxed);
}
// out[8]: compiler runs, their us; sceGxmFinish calls, their us; scene begins' us, begins of 1 ms or more; fence waits' us, fence waits
void vgxvita_waitStats_v1(uint64_t out[8]) {
    for (unsigned k = 0; k < kV1; ++k) {
        uint32_t hi = g_hi[k].load(std::memory_order_relaxed), lo = g_lo[k].load(std::memory_order_relaxed);
        const uint32_t again = g_hi[k].load(std::memory_order_relaxed);
        if (again != hi) { hi = again; lo = g_lo[k].load(std::memory_order_relaxed); } // (a carry in between: read once more)
        out[k] = (static_cast<uint64_t>(hi) << 32) | lo;
    }
}

// midflush31: out[0..5] = slow reserves, their us, the longest reserve us, slow draws, their us, the longest draw us (calls of 300 us or more); reset != 0 starts the
// next window. Called from the render thread (the thread that writes them).
void vgxvita_ringStats_v1(uint32_t out[6], int reset) {
    out[0] = g_slow[0]; out[1] = g_slowUs[0]; out[2] = g_slowMax[0]; out[3] = g_slow[1]; out[4] = g_slowUs[1]; out[5] = g_slowMax[1];
    if (reset) { g_slow[0] = g_slow[1] = g_slowUs[0] = g_slowUs[1] = g_slowMax[0] = g_slowMax[1] = 0; }
}

// v2: the same sums for all kCount entries (n = how many the caller has room for); returns how many were written
unsigned vgxvita_waitStats_v2(uint64_t* out, unsigned n) {
    const unsigned m = n < kCount ? n : static_cast<unsigned>(kCount);
    for (unsigned k = 0; k < m; ++k) {
        uint32_t hi = g_hi[k].load(std::memory_order_relaxed), lo = g_lo[k].load(std::memory_order_relaxed);
        const uint32_t again = g_hi[k].load(std::memory_order_relaxed);
        if (again != hi) { hi = again; lo = g_lo[k].load(std::memory_order_relaxed); }
        out[k] = (static_cast<uint64_t>(hi) << 32) | lo;
    }
    return m;
}

decltype(sceGxmEndScene) __real_sceGxmEndScene;
decltype(sceGxmDisplayQueueAddEntry) __real_sceGxmDisplayQueueAddEntry;
decltype(sceGxmReserveVertexDefaultUniformBuffer) __real_sceGxmReserveVertexDefaultUniformBuffer;
decltype(sceGxmReserveFragmentDefaultUniformBuffer) __real_sceGxmReserveFragmentDefaultUniformBuffer;
decltype(sceGxmTransferFinish) __real_sceGxmTransferFinish;
decltype(sceGxmDraw) __real_sceGxmDraw;
decltype(sceKernelWaitSema) __real_sceKernelWaitSema;
decltype(sceKernelDelayThread) __real_sceKernelDelayThread;

int __wrap_sceGxmEndScene(SceGxmContext* c, const SceGxmNotification* v, const SceGxmNotification* f) {
    if (!Mine()) return __real_sceGxmEndScene(c, v, f);
    const uint64_t t0 = NowUs();
    const int r = __real_sceGxmEndScene(c, v, f);
    Add(kEndScenes, 1); Add(kEndSceneUs, NowUs() - t0);
    return r;
}
int __wrap_sceGxmDisplayQueueAddEntry(SceGxmSyncObject* oldBuffer, SceGxmSyncObject* newBuffer, const void* callbackData) {
    vgxvita_clockCensusPicture_v1(); // (dma31, GXV_CLOCK_CENSUS: a picture ends; returns at once when off)
    vgxvita_copyCensusPicture_v1();  // (copy49: the first call reads GXV_SMALLCOPY / GXV_COPY_CENSUS; then returns at once when both are off)
    if (!Mine()) return __real_sceGxmDisplayQueueAddEntry(oldBuffer, newBuffer, callbackData);
    const uint64_t t0 = NowUs();
    const int r = __real_sceGxmDisplayQueueAddEntry(oldBuffer, newBuffer, callbackData);
    Add(kDisps, 1); Add(kDispUs, NowUs() - t0);
    return r;
}
int __wrap_sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext* c, void** buffer) {
    if (!Mine()) return __real_sceGxmReserveVertexDefaultUniformBuffer(c, buffer);
    const uint64_t t0 = NowUs();
    const int r = __real_sceGxmReserveVertexDefaultUniformBuffer(c, buffer);
    { const uint64_t d = NowUs() - t0; Add(kReserves, 1); Add(kResUs, d); NoteSlow(0, d); }
    return r;
}
int __wrap_sceGxmReserveFragmentDefaultUniformBuffer(SceGxmContext* c, void** buffer) {
    if (!Mine()) return __real_sceGxmReserveFragmentDefaultUniformBuffer(c, buffer);
    const uint64_t t0 = NowUs();
    const int r = __real_sceGxmReserveFragmentDefaultUniformBuffer(c, buffer);
    { const uint64_t d = NowUs() - t0; Add(kReserves, 1); Add(kResUs, d); NoteSlow(0, d); }
    return r;
}
int __wrap_sceGxmTransferFinish(void) {
    if (!Mine()) return __real_sceGxmTransferFinish();
    const uint64_t t0 = NowUs();
    const int r = __real_sceGxmTransferFinish();
    Add(kXfers, 1); Add(kXferUs, NowUs() - t0);
    return r;
}
int __wrap_sceGxmDraw(SceGxmContext* c, SceGxmPrimitiveType t, SceGxmIndexFormat f, const void* idx, unsigned int n) {
    if (g_on.load(std::memory_order_relaxed) < 2 || !Mine()) return __real_sceGxmDraw(c, t, f, idx, n);
    const uint64_t t0 = NowUs();
    const int r = __real_sceGxmDraw(c, t, f, idx, n);
    { const uint64_t d = NowUs() - t0; Add(kDraws, 1); Add(kDrawUs, d); NoteSlow(1, d); }
    return r;
}
int __wrap_sceKernelWaitSema(SceUID sema, int need, unsigned int* timeout) {
    if (!Mine()) return __real_sceKernelWaitSema(sema, need, timeout);
    const uint64_t t0 = NowUs();
    const int r = __real_sceKernelWaitSema(sema, need, timeout);
    Add(kSemas, 1); Add(kSemaUs, NowUs() - t0);
    return r;
}
int __wrap_sceKernelDelayThread(SceUInt usec) {
    if (!Mine()) return __real_sceKernelDelayThread(usec);
    const uint64_t t0 = NowUs();
    const int r = __real_sceKernelDelayThread(usec);
    Add(kDelays, 1); Add(kDelayUs, NowUs() - t0);
    return r;
}

decltype(shark_compile_shader_extended) __real_shark_compile_shader_extended;
decltype(sceGxmFinish) __real_sceGxmFinish;
decltype(sceGxmBeginScene) __real_sceGxmBeginScene;
decltype(sceGxmNotificationWait) __real_sceGxmNotificationWait;

SceGxmProgram* __wrap_shark_compile_shader_extended(const char* src, uint32_t* size, shark_type type, shark_opt opt, int32_t fastmath, int32_t fastprecision,
                                                    int32_t fastint) {
    const bool counting = g_on.load(std::memory_order_relaxed) != 0;
    if (!counting && !vgxvita_packNoteCompile_v1) return __real_shark_compile_shader_extended(src, size, type, opt, fastmath, fastprecision, fastint);
    const uint64_t t0 = NowUs();
    SceGxmProgram* const result = __real_shark_compile_shader_extended(src, size, type, opt, fastmath, fastprecision, fastint);
    const uint64_t us = NowUs() - t0;
    if (counting) {
        Add(kCompiles, 1);
        Add(kCompileUs, us);
    }
    if (vgxvita_packNoteCompile_v1) vgxvita_packNoteCompile_v1(us); // (packrefill25, GXV_PACK_MISSLOG: counts compiles in play; returns at once when off)
    return result;
}
void __wrap_sceGxmFinish(SceGxmContext* context) {
    if (!Mine()) { __real_sceGxmFinish(context); return; }
    const uint64_t t0 = NowUs();
    __real_sceGxmFinish(context);
    Add(kFinishes, 1);
    Add(kFinishUs, NowUs() - t0);
}
int __wrap_sceGxmBeginScene(SceGxmContext* context, unsigned int flags, const SceGxmRenderTarget* target, const SceGxmValidRegion* region,
                            SceGxmSyncObject* vertexSync, SceGxmSyncObject* fragmentSync, const SceGxmColorSurface* color,
                            const SceGxmDepthStencilSurface* depth) {
    if (!Mine()) return __real_sceGxmBeginScene(context, flags, target, region, vertexSync, fragmentSync, color, depth);
    const uint64_t t0 = NowUs();
    const int result = __real_sceGxmBeginScene(context, flags, target, region, vertexSync, fragmentSync, color, depth);
    const uint64_t us = NowUs() - t0;
    Add(kSceneUs, us);
    if (us >= 1000) Add(kSlowScenes, 1);
    return result;
}
int __wrap_sceGxmNotificationWait(const SceGxmNotification* notification) {
    if (!Mine()) return __real_sceGxmNotificationWait(notification);
    const uint64_t t0 = NowUs();
    const int result = __real_sceGxmNotificationWait(notification);
    Add(kFences, 1);
    Add(kFenceUs, NowUs() - t0);
    return result;
}
}

static_assert(std::is_same_v<decltype(&__wrap_shark_compile_shader_extended), decltype(&shark_compile_shader_extended)>);
static_assert(std::is_same_v<decltype(&__wrap_sceGxmFinish), decltype(&sceGxmFinish)>);
static_assert(std::is_same_v<decltype(&__wrap_sceGxmBeginScene), decltype(&sceGxmBeginScene)>);
static_assert(std::is_same_v<decltype(&__wrap_sceGxmNotificationWait), decltype(&sceGxmNotificationWait)>);
static_assert(std::is_same_v<decltype(&__wrap_sceGxmEndScene), decltype(&sceGxmEndScene)>);
static_assert(std::is_same_v<decltype(&__wrap_sceGxmDisplayQueueAddEntry), decltype(&sceGxmDisplayQueueAddEntry)>);
static_assert(std::is_same_v<decltype(&__wrap_sceGxmReserveVertexDefaultUniformBuffer), decltype(&sceGxmReserveVertexDefaultUniformBuffer)>);
static_assert(std::is_same_v<decltype(&__wrap_sceGxmReserveFragmentDefaultUniformBuffer), decltype(&sceGxmReserveFragmentDefaultUniformBuffer)>);
static_assert(std::is_same_v<decltype(&__wrap_sceGxmTransferFinish), decltype(&sceGxmTransferFinish)>);
static_assert(std::is_same_v<decltype(&__wrap_sceGxmDraw), decltype(&sceGxmDraw)>);
static_assert(std::is_same_v<decltype(&__wrap_sceKernelWaitSema), decltype(&sceKernelWaitSema)>);
static_assert(std::is_same_v<decltype(&__wrap_sceKernelDelayThread), decltype(&sceKernelDelayThread)>);
