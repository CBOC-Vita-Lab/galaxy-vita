// asyncsh31: the shader worker's allocations come from the C heap only (GXV_ASYNC_SHADERS' thread, vita/vgx-game/alloc_wrap.cpp).
//
// The hazard (the Strikers port's review of r110, item 1.2; reports/perf-inventory.md). vitaGL's translator, vitaShaRK and libshacccg allocate with
// vglMalloc / vglCalloc / vglMemalign / vglRealloc (gxm.c: shark_set_allocators(vglMalloc, vglFree)). Each tries newlib's heap first (locked: every thread
// of the game uses it) and, when that has no block, falls back to vitaGL's RAM / phycont / budget / VRAM pools (sceClibMspace, no lock, used by the render
// thread for textures, buffers and programs without a lock either). A worker compile that reaches the fallback while the render thread allocates from the
// same pool corrupts it. strikersrev25 made the worker check the C heap's room (8 MiB, AsyncHeapRoom) before a job; that check is a snapshot (kept 500 ms):
// another thread can use the room up while the compile runs (300-700 ms, many allocations), and the fallback is then still reachable.
//
// The fix. The link wraps the four entry points (-Wl,--wrap); on the worker thread they use newlib's heap and nothing else. A block newlib cannot give is a
// NULL: vitaShaRK / the translator fail the compile, vglCompileShaderPairToCache returns a failure, the engine marks the job Failed, and the draw takes the
// render thread's own compile, which may use every pool as before. On every other thread the wrappers are the originals. A realloc of a block that is not
// newlib's (never made by the worker: vitaGL hands the worker no pooled block) goes to the original and is counted.
// The decision is pure (this header) so tests/vgx_worker_alloc.cpp proves it with fake allocators.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace vgx::workeralloc {

struct Counts {
    std::atomic<uint32_t> heapBlocks{0};   // blocks the worker took from the C heap
    std::atomic<uint32_t> refused{0};      // requests the C heap could not meet (the worker's compile then fails over to the render thread)
    std::atomic<uint32_t> foreignRealloc{0}; // reallocs of a pooled block on the worker (expected 0): passed to the original
};

// Ops: the platform's allocators. heap* = newlib's (thread-safe); real* = vitaGL's originals (C heap, then the pools); isHeap(p) = p is newlib's block.
struct Ops {
    void* (*heapMalloc)(size_t) = nullptr;
    void* (*heapCalloc)(size_t, size_t) = nullptr;
    void* (*heapMemalign)(size_t, size_t) = nullptr;
    void* (*heapRealloc)(void*, size_t) = nullptr;
    void* (*realMalloc)(uint32_t) = nullptr;
    void* (*realCalloc)(uint32_t, uint32_t) = nullptr;
    void* (*realMemalign)(uint32_t, uint32_t) = nullptr;
    void* (*realRealloc)(void*, uint32_t) = nullptr;
    bool (*isHeap)(void*) = nullptr;
};

inline void* Note(void* p, Counts& c) {
    (p ? c.heapBlocks : c.refused).fetch_add(1, std::memory_order_relaxed);
    return p;
}
inline void* Malloc(bool worker, uint32_t size, const Ops& o, Counts& c) {
    return worker ? Note(o.heapMalloc(size), c) : o.realMalloc(size);
}
inline void* Calloc(bool worker, uint32_t n, uint32_t size, const Ops& o, Counts& c) {
    return worker ? Note(o.heapCalloc(n, size), c) : o.realCalloc(n, size);
}
inline void* Memalign(bool worker, uint32_t align, uint32_t size, const Ops& o, Counts& c) {
    return worker ? Note(o.heapMemalign(align, size), c) : o.realMemalign(align, size);
}
inline void* Realloc(bool worker, void* p, uint32_t size, const Ops& o, Counts& c) {
    if (!worker) return o.realRealloc(p, size);
    if (!p) return Note(o.heapMalloc(size), c);
    if (o.isHeap && !o.isHeap(p)) { c.foreignRealloc.fetch_add(1, std::memory_order_relaxed); return o.realRealloc(p, size); }
    return Note(o.heapRealloc(p, size), c);
}

} // namespace vgx::workeralloc
