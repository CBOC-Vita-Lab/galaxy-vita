// Real production ownership state machine; SDK calls are fault-injected here.
#include "gpu_memory.h"
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

static unsigned checks = 0, allocations = 0, unmaps = 0, frees = 0;
static int base_error = 0, map_error = 0, unmap_error = 0, free_error = 0;
static bool invalid_base = false;
static unsigned mapped_kind = 0;
static std::vector<int> events;
static void* allocation = nullptr;
static unsigned allocation_size = 0;
static bool expect_cdram = false;
extern "C" int port_vita_gpu_memory_alloc_cdram(const char*, uint32_t, uint32_t,
                                               PortVitaGpuMemory*) __attribute__((weak));
static void require(bool valid, const char* message) {
    ++checks; if (!valid) { fprintf(stderr, "GPU memory FAILED: %s\n", message); exit(1); }
}

extern "C" SceUID sceKernelAllocMemBlock(const char*, SceKernelMemBlockType type, SceSize size, SceKernelAllocMemBlockOpt*) {
    require(type == (expect_cdram ? SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW :
                    SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE), "requested physical memory class");
    require(size && size % 4096 == 0, "SDK allocation page alignment");
    if (expect_cdram) require(size % (256 * 1024) == 0, "CDRAM allocation granularity");
    allocation = mmap(reinterpret_cast<void*>(0x60000000), size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    require(allocation != MAP_FAILED, "isolated low-address fixture allocation");
    allocation_size = size; ++allocations; events.push_back(1); return 42;
}
extern "C" int sceKernelGetMemBlockBase(SceUID uid, void** output) {
    require(uid == 42, "correct owned UID queried");
    if (base_error) return base_error;
    *output = invalid_base ? static_cast<char*>(allocation) + 1 : allocation; return 0;
}
extern "C" int sceKernelFreeMemBlock(SceUID uid) {
    require(uid == 42 && mapped_kind == 0, "never free memory still mapped to GXM");
    ++frees; events.push_back(3); if (free_error) return free_error;
    require(munmap(allocation, allocation_size) == 0, "fixture release"); allocation = nullptr; return 0;
}
static int map(void* base, SceSize size, unsigned kind) {
    require(base == allocation && size == allocation_size && !mapped_kind, "map full owned range exactly once");
    events.push_back(2); if (map_error) return map_error; mapped_kind = kind; return 0;
}
static int unmap(void* base, unsigned kind) {
    require(base == allocation && mapped_kind == kind, "matching GXM unmap kind and base");
    ++unmaps; events.push_back(4); if (unmap_error) return unmap_error; mapped_kind = 0; return 0;
}
extern "C" int sceGxmMapMemory(void* base, SceSize size, SceGxmMemoryAttribFlags flags) {
    return map(base, size, static_cast<unsigned>(flags));
}
extern "C" int sceGxmUnmapMemory(void* base) { return unmap(base, mapped_kind); }
extern "C" int sceGxmMapVertexUsseMemory(void* base, SceSize size, unsigned* offset) {
    *offset = 0x1234; return map(base, size, 4);
}
extern "C" int sceGxmUnmapVertexUsseMemory(void* base) { return unmap(base, 4); }
extern "C" int sceGxmMapFragmentUsseMemory(void* base, SceSize size, unsigned* offset) {
    *offset = 0x5678; return map(base, size, 5);
}
extern "C" int sceGxmUnmapFragmentUsseMemory(void* base) { return unmap(base, 5); }

static PortVitaGpuMemory empty() { return {nullptr, 0, -1, 0, 0}; }
int main() {
    for (unsigned mapping : {1u, 3u, 4u, 5u}) {
        auto block = empty(); events.clear();
        require(port_vita_gpu_memory_alloc("test", 4097, mapping, &block) == 0, "valid GXM-mapped allocation");
        require(block.base == allocation && block.size == 8192 && block.uid == 42 && block.mapping == mapping, "exact rounded ownership descriptor");
        require(block.usse_offset == (mapping == 4 ? 0x1234u : mapping == 5 ? 0x5678u : 0u), "USSE offsets are SDK outputs, not pointer truncation");
        const unsigned previous = allocations;
        require(port_vita_gpu_memory_alloc("test", 1, mapping, &block) < 0 && allocations == previous, "reject overwriting owned descriptor");
        unmap_error = -101;
        require(port_vita_gpu_memory_free(&block) == -101 && block.mapping == mapping && block.uid == 42, "failed unmap retains complete ownership");
        unmap_error = 0; free_error = -102;
        require(port_vita_gpu_memory_free(&block) == -102 && block.mapping == 0 && block.uid == 42, "failed kernel free retains unmapped UID");
        const unsigned previous_unmaps = unmaps;
        free_error = 0;
        require(port_vita_gpu_memory_free(&block) == 0 && unmaps == previous_unmaps, "retry free without double unmap");
        require(block.base == nullptr && block.size == 0 && block.uid == -1 && block.mapping == 0, "successful release empties descriptor");
        require(events == (std::vector<int>{1,2,4,4,3,3}), "map/unmap/free ordering including retry");
    }
    for (int fault : {1, 2, 3}) {
        auto block = empty();
        base_error = fault == 1 ? -103 : 0; invalid_base = fault == 2; map_error = fault == 3 ? -104 : 0;
        free_error = -105;
        require(port_vita_gpu_memory_alloc("test", 1, 1, &block) == -105 && block.uid == 42 && block.mapping == 0, "allocation rollback failure keeps recoverable UID");
        base_error = map_error = free_error = 0; invalid_base = false;
        require(port_vita_gpu_memory_free(&block) == 0, "retry failed allocation rollback");
    }
    auto block = empty(); const unsigned previous = allocations;
    for (unsigned size : {0u, 0xfffff001u, 0xffffffffu})
        require(port_vita_gpu_memory_alloc("test", size, 1, &block) < 0 && block.uid == -1, "zero/overflow size rejected");
    for (unsigned mapping : {0u, 2u, 6u, 0xffffffffu})
        require(port_vita_gpu_memory_alloc("test", 4096, mapping, &block) < 0, "invalid mapping rejected");
    require(port_vita_gpu_memory_alloc(nullptr, 4096, 1, &block) < 0, "missing name rejected");
    require(allocations == previous && !allocation, "invalid requests have no SDK side effects");
    require(port_vita_gpu_memory_free(&block) < 0 && block.uid == -1, "double free rejected");
    require(port_vita_gpu_memory_alloc_cdram != nullptr, "CDRAM allocator exists");
    expect_cdram = true;
    for (unsigned mapping : {1u, 3u}) {
        require(port_vita_gpu_memory_alloc_cdram("render", 256 * 1024 + 1, mapping, &block) == 0,
                "CDRAM allocates and maps native render storage");
        require(block.size == 512 * 1024 && block.mapping == mapping, "CDRAM rounded extent and flags");
        require(port_vita_gpu_memory_free(&block) == 0, "CDRAM follows same unmap/free ownership");
    }
    for (unsigned mapping : {0u, 4u, 5u})
        require(port_vita_gpu_memory_alloc_cdram("bad", 1, mapping, &block) < 0, "CDRAM rejects USSE/invalid mappings");
    for (unsigned size : {0u, 0xfffc0001u, 0xffffffffu})
        require(port_vita_gpu_memory_alloc_cdram("bad", size, 3, &block) < 0, "CDRAM rejects rounded overflow");
    printf("GXM GPU memory: %u ownership/fault checks passed; SDK mock boundary, not device proof\n", checks);
}
