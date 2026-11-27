// asyncsh31: the shader worker's allocation rule (platform/vgx/src/vgx_worker_alloc.h; vita/vgx-game/alloc_wrap.cpp is the console's plumbing).
// Fake allocators: a small "C heap" that can run out, and vitaGL's originals, which here record the thread that called them. Checks:
//   1. off the worker every entry point is the original, unchanged;
//   2. on the worker every entry point takes the C heap, a block the heap cannot give is NULL (refused, counted), the original is never called;
//   3. realloc on the worker: NULL -> heap malloc, a heap block -> heap realloc, a pooled block -> the original (counted; never expected);
//   4. two threads at once (a "render thread" allocating through the originals, a "worker" through the wrappers) for 200,000 calls each: no
//      original is ever entered from the worker thread.
#include "vgx_worker_alloc.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

namespace {
using namespace vgx::workeralloc;
int g_fail = 0;
void Check(bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_fail; } }
std::atomic<long> g_heapLeft{1 << 20};
std::mutex g_heapM;
std::set<void*> g_heapBlocks;
void* HeapTake(size_t n) {
    if (g_heapLeft.fetch_sub(static_cast<long>(n)) < static_cast<long>(n)) { g_heapLeft.fetch_add(static_cast<long>(n)); return nullptr; }
    void* p = std::malloc(n ? n : 1);
    std::lock_guard<std::mutex> l(g_heapM); g_heapBlocks.insert(p);
    return p;
}
void* HMalloc(size_t n) { return HeapTake(n); }
void* HCalloc(size_t a, size_t b) { void* p = HeapTake(a * b); if (p) std::memset(p, 0, a * b); return p; }
void* HMemalign(size_t, size_t n) { return HeapTake(n); }
void* HRealloc(void* p, size_t n) { void* q = HeapTake(n); if (q && p) { std::lock_guard<std::mutex> l(g_heapM); g_heapBlocks.erase(p); } return q; }
bool IsHeapBlock(void* p) { std::lock_guard<std::mutex> l(g_heapM); return g_heapBlocks.count(p) != 0; }
std::atomic<std::thread::id> g_worker{};
std::atomic<long> g_realCalls{0}, g_realFromWorker{0};
char g_pool[64];
void* Real(uint32_t) {
    g_realCalls.fetch_add(1);
    if (std::this_thread::get_id() == g_worker.load()) g_realFromWorker.fetch_add(1);
    return g_pool;
}
void* RMalloc(uint32_t n) { return Real(n); }
void* RCalloc(uint32_t, uint32_t n) { return Real(n); }
void* RMemalign(uint32_t, uint32_t n) { return Real(n); }
void* RRealloc(void*, uint32_t n) { return Real(n); }
const Ops kOps = {&HMalloc, &HCalloc, &HMemalign, &HRealloc, &RMalloc, &RCalloc, &RMemalign, &RRealloc, &IsHeapBlock};
} // namespace

int main() {
    Counts c;
    // 1. off the worker
    Check(Malloc(false, 16, kOps, c) == g_pool && Calloc(false, 2, 8, kOps, c) == g_pool && Memalign(false, 16, 32, kOps, c) == g_pool &&
          Realloc(false, nullptr, 8, kOps, c) == g_pool, "off the worker: the originals");
    Check(g_realCalls.load() == 4 && c.heapBlocks.load() == 0 && c.refused.load() == 0, "off the worker: nothing counted");
    // 2. on the worker
    g_realCalls = 0;
    void* a = Malloc(true, 100, kOps, c);
    void* b = Calloc(true, 10, 10, kOps, c);
    void* m = Memalign(true, 64, 100, kOps, c);
    Check(a && b && m && IsHeapBlock(a) && IsHeapBlock(b) && IsHeapBlock(m) && c.heapBlocks.load() == 3, "on the worker: the C heap");
    Check(Malloc(true, 2u << 20, kOps, c) == nullptr && c.refused.load() == 1, "on the worker: a block the C heap cannot give is NULL (refused)");
    Check(g_realCalls.load() == 0, "on the worker: the originals (and their pools) are never called");
    // 3. realloc on the worker
    void* r0 = Realloc(true, nullptr, 50, kOps, c);
    Check(r0 && IsHeapBlock(r0), "worker realloc of NULL: heap malloc");
    void* r1 = Realloc(true, r0, 80, kOps, c);
    Check(r1 && IsHeapBlock(r1) && g_realCalls.load() == 0, "worker realloc of a heap block: heap realloc");
    Check(Realloc(true, g_pool, 10, kOps, c) == g_pool && c.foreignRealloc.load() == 1 && g_realCalls.load() == 1, "worker realloc of a pooled block: the original, counted");
    Check(Realloc(true, r1, 4u << 20, kOps, c) == nullptr && c.refused.load() == 2, "worker realloc the heap cannot meet: NULL");
    // 4. two threads
    g_heapLeft = 1L << 40;
    g_realCalls = 0;
    std::atomic<bool> go{false};
    std::thread worker([&] {
        g_worker = std::this_thread::get_id();
        while (!go.load()) {}
        void* last = nullptr;
        for (int i = 0; i < 200000; ++i) {
            switch (i & 3) {
            case 0: Malloc(true, 32, kOps, c); break;
            case 1: Calloc(true, 4, 8, kOps, c); break;
            case 2: Memalign(true, 16, 32, kOps, c); break;
            default: last = Realloc(true, last, 48, kOps, c); break;
            }
        }
    });
    std::thread render([&] {
        while (!go.load()) {}
        for (int i = 0; i < 200000; ++i) Malloc(false, 32, kOps, c);
    });
    while (g_worker.load() == std::thread::id{}) {}
    go = true;
    worker.join();
    render.join();
    Check(g_realFromWorker.load() == 0, "no original (pool) call from the worker thread under load");
    Check(g_realCalls.load() == 200000, "every render-thread call reached the original");
    std::printf("worker alloc: heap blocks %u, refused %u, foreign reallocs %u; originals from the worker %ld\n", c.heapBlocks.load(), c.refused.load(),
                c.foreignRealloc.load(), g_realFromWorker.load());
    if (g_fail) return 1;
    std::printf("PASS\n");
    return 0;
}
