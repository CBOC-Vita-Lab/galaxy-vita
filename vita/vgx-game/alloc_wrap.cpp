// asyncsh31: vitaGL's four allocation entry points, wrapped at the link (CMakeLists.txt: -Wl,--wrap=vglMalloc,vglCalloc,vglMemalign,vglRealloc), so that
// the shader worker (GXV_ASYNC_SHADERS' "smg_shaders" thread) takes blocks from newlib's heap only and never from vitaGL's unlocked pools.
// platform/vgx/src/vgx_worker_alloc.h has the reasoning; this file is the console's plumbing. Every other thread gets the original call.
// (The wrap reaches every caller outside vitaGL's vgl.o: the translator (glsl_utils.o), custom_shaders.o, and vitaShaRK / libshacccg through
// gxm.o's shark_set_allocators(vglMalloc, vglFree). vgl.o's own internal calls -- vglRealloc's fallback to vglMalloc -- are not seen, which is why
// vglRealloc itself is wrapped.)
#include "vgx_worker_alloc.h"
#include <cstdint>
#include <cstdlib>
#include <malloc.h>
#include <psp2/kernel/threadmgr.h>

extern "C" {
void* __real_vglMalloc(uint32_t size);
void* __real_vglCalloc(uint32_t nmember, uint32_t size);
void* __real_vglMemalign(uint32_t alignment, uint32_t size);
void* __real_vglRealloc(void* ptr, uint32_t size);
// vitaGL (utils/mem_utils.c): which of its memories an address is in; 4 = VGL_MEM_EXTERNAL (newlib). Weak: an older archive without it is taken as "newlib's".
int vgl_mem_get_type_by_addr(void* addr) __attribute__((weak));
// vgx_host_galaxy_vita.cpp: the worker thread's id (-1 until it runs). Weak: a host without the worker leaves every thread on the originals.
extern volatile int vgxvita_asyncWorkerTid __attribute__((weak));
}

namespace {
vgx::workeralloc::Counts g_counts;
bool IsHeap(void* p) { return !vgl_mem_get_type_by_addr || vgl_mem_get_type_by_addr(p) == 4; }
void* HeapMemalign(size_t a, size_t s) { return memalign(a, s); }
// (constant-initialised: no static constructor, so a vitaGL allocation made before this unit's initialisers ran would still find it)
constexpr vgx::workeralloc::Ops kOps = {&malloc, &calloc, &HeapMemalign, &realloc, &__real_vglMalloc, &__real_vglCalloc, &__real_vglMemalign, &__real_vglRealloc, &IsHeap};
// (one load and a compare while no worker exists; sceKernelGetThreadId only then)
inline bool OnWorker() {
    if (&vgxvita_asyncWorkerTid == nullptr) return false;
    const int tid = vgxvita_asyncWorkerTid;
    return tid >= 0 && sceKernelGetThreadId() == tid;
}
} // namespace

extern "C" void* __wrap_vglMalloc(uint32_t size) { return vgx::workeralloc::Malloc(OnWorker(), size, kOps, g_counts); }
extern "C" void* __wrap_vglCalloc(uint32_t n, uint32_t size) { return vgx::workeralloc::Calloc(OnWorker(), n, size, kOps, g_counts); }
extern "C" void* __wrap_vglMemalign(uint32_t alignment, uint32_t size) { return vgx::workeralloc::Memalign(OnWorker(), alignment, size, kOps, g_counts); }
extern "C" void* __wrap_vglRealloc(void* ptr, uint32_t size) { return vgx::workeralloc::Realloc(OnWorker(), ptr, size, kOps, g_counts); }
// The back end's 5 s line (async-shaders-5s) reads these: blocks the worker took from the C heap, requests it refused, pooled reallocs passed on.
extern "C" void vgxvita_workerAllocStats_v1(uint32_t* heapBlocks, uint32_t* refused, uint32_t* foreignRealloc) {
    if (heapBlocks) *heapBlocks = g_counts.heapBlocks.load(std::memory_order_relaxed);
    if (refused) *refused = g_counts.refused.load(std::memory_order_relaxed);
    if (foreignRealloc) *foreignRealloc = g_counts.foreignRealloc.load(std::memory_order_relaxed);
}
