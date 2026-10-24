// Native CPU pointers and logical Wii GX/DSP offsets are distinct address kinds.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <mutex>

#include "port/port.h"
#include "revolution/os.h"

#if defined(PORT_ARMHF_HARNESS)
// armhf/qemu harness only: PETARI_MEM2_MB=<n> enlarges MEM2 (default: the Vita's 64 MiB).  The Vita-sized heaps run out
// when the PETARI_STAGE debug jump enters Gateway Galaxy (HeavensDoorGalaxy), so that scene is measured with a larger bank.
static uint32_t harnessMem2Size() {
    static const uint32_t v = [] { const char* e = getenv("PETARI_MEM2_MB"); const int mb = e ? atoi(e) : 0; return mb >= 64 ? (uint32_t)mb << 20 : (uint32_t)PORT_MEM2_SIZE; }();
    return v;
}
#undef PORT_MEM2_SIZE
#define PORT_MEM2_SIZE harnessMem2Size()
#endif

#if defined(__vita__)
#include "memory.h"
#else
#include <errno.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#endif

namespace {
enum MemoryKind { Mem1, Mem2, Image, LowPool, LockedCache };
struct Region {
    uintptr_t base, alias;
    uint32_t physical, size;
    MemoryKind kind;
};
constexpr unsigned kMaxRegions = 128;
Region sRegions[kMaxRegions];
std::atomic<unsigned> sRegionCount{0};
std::mutex sLowMutex;
bool sMemReady;
uintptr_t sMem1, sMem2;
uintptr_t sLowOffset;
void *sArenaLo, *sArenaHi, *sMEM2ArenaLo, *sMEM2ArenaHi;

bool contains(uintptr_t base, uint32_t size, uintptr_t address, size_t bytes) {
    return address >= base && address - base <= size && bytes <= size - (address - base);
}
bool intersects(uintptr_t a, size_t na, uintptr_t b, size_t nb) {
    return uint64_t(a) < uint64_t(b) + nb && uint64_t(b) < uint64_t(a) + na;
}
void addRegion(uintptr_t base, uintptr_t alias, uint32_t physical, uint32_t size, MemoryKind kind) {
    const uint64_t limit = UINT32_MAX;  // One-past pointers must also fit cooked pointer fields.
    if (base < 0x40000000u || !size || uint64_t(base) + size > limit || uint64_t(physical) + size > 0x40000000u ||
        (alias && uint64_t(alias) + size > limit) || (alias && intersects(base, size, alias, size))) {
        port_fatal("invalid native memory region: %p/%p physical %08x+%x", (void*)base, (void*)alias, physical, size);
    }
    const unsigned count = sRegionCount.load(std::memory_order_acquire);
    if (count == kMaxRegions) port_fatal("native memory region table exhausted");
    for (unsigned i = 0; i < count; ++i) {
        const Region& r = sRegions[i];
        if (intersects(physical, size, r.physical, r.size) || intersects(base, size, r.base, r.size) ||
            (alias && intersects(alias, size, r.base, r.size)) ||
            (r.alias && (intersects(base, size, r.alias, r.size) || (alias && intersects(alias, size, r.alias, r.size))))) {
            port_fatal("overlapping native memory regions");
        }
    }
    sRegions[count] = {base, alias, physical, size, kind};
    // Readers never see a partially initialized region; published entries are immutable.
    sRegionCount.store(count + 1, std::memory_order_release);
}
const Region* nativeRegion(const void* p, size_t bytes, uintptr_t* start = nullptr) {
    const uintptr_t address = (uintptr_t)p;
    if (!address) return nullptr;
    const unsigned count = sRegionCount.load(std::memory_order_acquire);
    for (unsigned i = 0; i < count; ++i) {
        const Region& r = sRegions[i];
        if (contains(r.base, r.size, address, bytes)) { if (start) *start = r.base; return &r; }
        if (r.alias && contains(r.alias, r.size, address, bytes)) { if (start) *start = r.alias; return &r; }
    }
    return nullptr;
}
const Region* physicalRegion(uint32_t address) {
    const unsigned count = sRegionCount.load(std::memory_order_acquire);
    for (unsigned i = 0; i < count; ++i) {
        if (contains(sRegions[i].physical, sRegions[i].size, address, 1)) return &sRegions[i];
    }
    return nullptr;
}

#if !defined(__vita__)
uintptr_t sWindowBase, sWindowEnd, sLowBase;
bool sLowReserved;  // the whole low pool's address range is ours (reserved at start-up)
int makeSharedFd(const char* name, size_t size) {
    int fd = (int)syscall(__NR_memfd_create, name, 1u);
    if (fd < 0) port_fatal("memfd_create failed: %s", strerror(errno));
    if (ftruncate(fd, (off_t)size) != 0) port_fatal("ftruncate(%zu) failed: %s", size, strerror(errno));
    return fd;
}
void* mapAt(uintptr_t address, size_t size, int fd) {
    const bool reserved = (address >= sWindowBase && address + size <= sWindowEnd) ||
                          (sLowReserved && address >= sLowBase && address + size <= sLowBase + PORT_LOWPOOL_SIZE);
    const int flags = (fd >= 0 ? MAP_SHARED : (MAP_PRIVATE | MAP_ANONYMOUS)) | (reserved ? MAP_FIXED : MAP_FIXED_NOREPLACE);
    void* p = mmap((void*)address, size, PROT_READ | PROT_WRITE, flags, fd, 0);
    if (p == MAP_FAILED) {
#if defined(__linux__) && !defined(__ANDROID__)
        const int error = errno;
        FILE* maps = fopen("/proc/self/maps", "r");
        if (maps) { char line[1024]; while (fgets(line, sizeof(line), maps)) port_log("mem map: %s", line); fclose(maps); }
        errno = error;
#endif
        port_fatal("mmap at %08lx (+%zx) failed: %s", (unsigned long)address, size, strerror(errno));
    }
    if ((uintptr_t)p != address) port_fatal("mmap landed outside its requested 32-bit range");
    return p;
}
int registerImage(struct dl_phdr_info* info, size_t, void* data) {
    const uintptr_t own = (uintptr_t)&port_mem_init;
    bool ours = false;
    uintptr_t lo = UINTPTR_MAX;
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || !ph.p_memsz) continue;
        const uintptr_t base = info->dlpi_addr + ph.p_vaddr;
        if (own >= base && own - base < ph.p_memsz) ours = true;
        if (base < lo) lo = base;
    }
    if (!ours) return 0;
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || !ph.p_memsz) continue;
        const uintptr_t base = info->dlpi_addr + ph.p_vaddr;
        if (base - lo >= PORT_LIB_SIZE || ph.p_memsz > PORT_LIB_SIZE - (base - lo)) port_fatal("native image exceeds logical image window");
        addRegion(base, 0, 0x18000000u + (uint32_t)(base - lo), (uint32_t)ph.p_memsz, Image);
    }
    *(bool*)data = true;
    return 1;
}
#else
PortVitaMemoryBlock sBlocks[kMaxRegions];
unsigned sBlockCount;
uintptr_t allocateVita(const char* name, uint32_t size) {
    if (sBlockCount == kMaxRegions) port_fatal("Vita memory block table exhausted");
    PortVitaMemoryBlock& block = sBlocks[sBlockCount];
    const int result = port_vita_memory_alloc(name, size, &block);
    if (result < 0) {
        for (unsigned i = 0; i < sBlockCount; ++i) port_vita_memory_free(&sBlocks[i]);
        port_fatal("Vita allocation %s (+%x) failed: %08x", name, size, (unsigned)result);
    }
    ++sBlockCount;
    return (uintptr_t)block.base;
}
#endif

void storeGlobal(uint32_t offset, uint32_t value) { *(volatile uint32_t*)(sMem1 + offset) = value; }
bool aligned(uint32_t alignment) { return alignment && !(alignment & (alignment - 1)); }
void setArena(void*& destination, void* p, uintptr_t base, uint32_t size, void* opposite, bool low) {
    const uintptr_t a = (uintptr_t)p;
    if (!contains(base, size, a, 0) || (low ? a > (uintptr_t)opposite : a < (uintptr_t)opposite)) port_fatal("OS arena bounds invalid");
    destination = p;
}
void cpuCacheFence() {
#if defined(__vita__)
    // Wii DMA is CPU-emulated. GXM must consume separate uncached, fenced staging
    // allocations, never these cached game arenas. This is not a GPU cache flush.
    std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}
}

extern "C" void port_mem_set_reserved_window(uintptr_t base, size_t size) {
#if !defined(__vita__)
    if (size > UINTPTR_MAX - base) port_fatal("reserved memory window overflow");
    sWindowBase = base; sWindowEnd = base + size;
#else
    (void)base; (void)size;
    port_fatal("Vita cannot reserve console virtual windows");
#endif
}

extern "C" int port_mem_init(void) {
    if (sMemReady) return 1;
    uintptr_t alias1 = 0, alias2 = 0, scratch;
#if defined(__vita__)
    sMem1 = allocateVita("galaxy-mem1", PORT_MEM1_SIZE);
    sMem2 = allocateVita("galaxy-mem2", PORT_MEM2_SIZE);
    scratch = allocateVita("galaxy-lc", 0x4000);
#else
    const char* mode = getenv("PETARI_NATIVE_ADDRESSES");
    const bool rebased = mode && strcmp(mode, "0") != 0;
    sMem1 = rebased ? 0x60000000u : PORT_MEM1_BASE;
    sMem2 = rebased ? 0x70000000u : PORT_MEM2_BASE;
#if defined(PORT_ARMHF_HARNESS)
    // 32-bit guest under qemu-user: the stack and the shared libraries sit at 0x40000000-0x41000000.
    sLowBase = rebased ? 0x44000000u : PORT_LOWPOOL_BASE;
#else
    sLowBase = rebased ? 0x40000000u : PORT_LOWPOOL_BASE;
#endif
    scratch = rebased ? 0x50000000u : PORT_LOCKED_CACHE;
    if (rebased) {
        mapAt(sMem1, PORT_MEM1_SIZE, -1); mapAt(sMem2, PORT_MEM2_SIZE, -1);
        // The low pool (thread stacks) is handed out piece by piece at fixed addresses: keep the
        // C library's own mappings (stacks of host threads, heap arenas) out of its range.
        if (mmap((void*)sLowBase, PORT_LOWPOOL_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0) ==
            (void*)sLowBase)
            sLowReserved = true;
    } else {
        alias1 = PORT_MEM1_UNCACHED; alias2 = PORT_MEM2_UNCACHED;
        int fd = makeSharedFd("wii-mem1", PORT_MEM1_SIZE);
        mapAt(sMem1, PORT_MEM1_SIZE, fd); mapAt(alias1, PORT_MEM1_SIZE, fd); close(fd);
        fd = makeSharedFd("wii-mem2", PORT_MEM2_SIZE);
        mapAt(sMem2, PORT_MEM2_SIZE, fd); mapAt(alias2, PORT_MEM2_SIZE, fd); close(fd);
    }
    mapAt(scratch, 0x4000, -1);
#endif
    addRegion(sMem1, alias1, 0, PORT_MEM1_SIZE, Mem1);
    addRegion(sMem2, alias2, 0x10000000u, PORT_MEM2_SIZE, Mem2);
    addRegion(scratch, 0, 0x30000000u, 0x4000, LockedCache);
#if defined(__vita__)
    PortVitaImageSegment image[4]; uint32_t count = 0;
    const int imageResult = port_vita_memory_image(image, 4, &count);
    if (imageResult < 0 || !count) port_fatal("cannot discover Vita native image: %08x", (unsigned)imageResult);
    uint32_t offset = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (image[i].size > PORT_LIB_SIZE - offset) port_fatal("Vita image exceeds logical image window");
        addRegion((uintptr_t)image[i].base, 0, 0x18000000u + offset, image[i].size, Image);
        const uint64_t next = (uint64_t(offset) + image[i].size + 4095) & ~uint64_t(4095);
        if (next > PORT_LIB_SIZE) port_fatal("Vita image alignment exceeds logical image window");
        offset = (uint32_t)next;
    }
    uint32_t user, cdram, phycont;
    if (!port_vita_memory_free_sizes(&user, &cdram, &phycont)) PORT_LOG("mem: Vita free user %u CDRAM %u physical %u", user, cdram, phycont);
#else
    bool foundImage = false;
    dl_iterate_phdr(registerImage, &foundImage);
    if (!foundImage) port_fatal("cannot discover native game image");
#endif
    storeGlobal(0x20, 0x0D15EA5E); storeGlobal(0x24, 1);
    storeGlobal(0x28, PORT_MEM1_SIZE); storeGlobal(0x2C, 0x10000006);
    storeGlobal(0xCC, 5); storeGlobal(0xF0, PORT_MEM1_SIZE);
    storeGlobal(0xF8, 243000000); storeGlobal(0xFC, 729000000);
    storeGlobal(0x3118, PORT_MEM2_SIZE); storeGlobal(0x311C, PORT_MEM2_SIZE);
    storeGlobal(0x3120, (uint32_t)(sMem2 + PORT_MEM2_SIZE));
    storeGlobal(0x3124, (uint32_t)(sMem2 + 0x800));
    storeGlobal(0x3128, (uint32_t)(sMem2 + PORT_MEM2_SIZE));
    sArenaLo = (void*)(sMem1 + 0x4000); sArenaHi = (void*)(sMem1 + PORT_MEM1_SIZE);
    sMEM2ArenaLo = (void*)(sMem2 + 0x800); sMEM2ArenaHi = (void*)(sMem2 + PORT_MEM2_SIZE);
    storeGlobal(0x30, (uint32_t)(uintptr_t)sArenaLo); storeGlobal(0x34, (uint32_t)(uintptr_t)sArenaHi);
    sMemReady = true;
    PORT_LOG("mem: native MEM1 %p+%x MEM2 %p+%x aliases %p/%p, %u regions", (void*)sMem1, PORT_MEM1_SIZE,
             (void*)sMem2, PORT_MEM2_SIZE, (void*)alias1, (void*)alias2, sRegionCount.load());
    return 1;
}

extern "C" void port_mem_set_disc_id(const char* id) { memcpy(port_phys_to_host(0), id, 6); }
extern "C" void* port_low_alloc(size_t size) {
    const size_t page = 0x4000;
    if (!size || size > PORT_LOWPOOL_SIZE - page) port_fatal("invalid low pool allocation size");
    size = (size + page - 1) & ~(page - 1);
    std::lock_guard<std::mutex> lock(sLowMutex);
    if (!sMemReady || sLowOffset > PORT_LOWPOOL_SIZE - size - page) port_fatal("low pool exhausted or uninitialized");
    uintptr_t base;
#if defined(__vita__)
    base = allocateVita("galaxy-low", (uint32_t)size);
#else
    base = (uintptr_t)mapAt(sLowBase + sLowOffset, size, -1);
#endif
    addRegion(base, 0, 0x20000000u + (uint32_t)sLowOffset, (uint32_t)size, LowPool);
    sLowOffset += size + page;
    return (void*)base;
}
extern "C" int port_is_native_range(const void* p, size_t size) {
    if (nativeRegion(p, size)) return 1;
#if defined(__vita__)
    // Native pthread stacks are kernel-owned, not allocations in the low pool.
    if (size && size <= UINT32_MAX) return port_vita_memory_valid_range(p, (uint32_t)size);
#endif
    return 0;
}
extern "C" int port_is_mem1(const void* p) { const Region* r = nativeRegion(p, 1); return r && r->kind == Mem1; }
extern "C" int port_is_mem2(const void* p) { const Region* r = nativeRegion(p, 1); return r && r->kind == Mem2; }
extern "C" int port_is_game_memory(const void* p) { const Region* r = nativeRegion(p, 1); return r && (r->kind == Mem1 || r->kind == Mem2); }
extern "C" void* port_phys_to_host(uint32_t physical) {
    const Region* r = physicalRegion(physical);
    if (!r) port_fatal("unmapped logical physical address %08x (caller %p)",
                       physical, __builtin_return_address(0));
    return (void*)(r->base + (physical - r->physical));
}
extern "C" int port_physical_window(uint32_t physical,void** data,uint32_t* base,size_t* bytes) {
    if (data) *data=nullptr;
    if (base) *base=0;
    if (bytes) *bytes=0;
    if (!data || !base || !bytes) return 0;
    const Region* region=physicalRegion(physical);
    if (!region) return 0;
    *data=(void*)region->base; *base=region->physical; *bytes=region->size;
    return 1;
}
// For the renderer (vgx_galaxy.cpp): the registered region a native pointer lies in (its
// cached base, also for a pointer into the uncached alias), and the region table itself.
extern "C" int port_native_window(const void* p, void** base, size_t* bytes) {
    uintptr_t start = 0;
    const Region* r = nativeRegion(p, 1, &start);
    if (!r) return 0;
    *base = (void*)start; *bytes = r->size;
    return 1;
}
extern "C" int port_memory_region(unsigned index, void** base, void** alias, uint32_t* physical, uint32_t* size, int* kind) {
    if (index >= sRegionCount.load(std::memory_order_acquire)) return 0;
    const Region& r = sRegions[index];
    *base = (void*)r.base; *alias = (void*)r.alias; *physical = r.physical; *size = r.size; *kind = (int)r.kind;
    return 1;
}
extern "C" void port_vgx_notify_write(const void* p, size_t n);
// The game tells the GPU about changed memory by flushing the data cache over it.
static inline void gpuWriteNotify(void* addr, u32 size) {
    if (!size) return;
    uintptr_t start = 0;
    const Region* r = nativeRegion(addr, 1, &start);
    if (!r) return;
    // (a store through the uncached alias is a store to the same bytes as the cached address)
    port_vgx_notify_write((const void*)(r->base + ((uintptr_t)addr - start)), size);
}
// Bulk writes by the platform itself into memory the GPU may read (disc and save-file reads,
// decoded movie frames): the same notification a data-cache flush gives.
extern "C" void port_gpu_memory_written(void* addr, uint32_t size) { gpuWriteNotify(addr, size); }
extern "C" void* port_phys_to_uncached(uint32_t physical) {
    const Region* r = physicalRegion(physical);
    if (!r) port_fatal("unmapped logical uncached address %08x", physical);
    return (void*)((r->alias ? r->alias : r->base) + (physical - r->physical));
}
extern "C" uint32_t port_host_to_phys(const void* p) {
    if (!p) return 0;  // GX permits an empty/default image binding at physical zero.
    uintptr_t start;
    const Region* r = nativeRegion(p, 1, &start);
    if (!r) port_fatal("unregistered native physical-address source %p (caller %p)", p, __builtin_return_address(0));
    return r->physical + (uint32_t)((uintptr_t)p - start);
}
extern "C" void* port_resolve_gpu_address(uint32_t address) {
    if (address < 0x40000000u) return port_phys_to_host(address);
    void* p = (void*)(uintptr_t)address;
    if (!port_is_native_range(p, 1)) port_fatal("unmapped native GX address %08x", address);
    return p;
}

extern "C" {
void* OSGetArenaLo(void) { return sArenaLo; }
void* OSGetArenaHi(void) { return sArenaHi; }
void OSSetArenaLo(void* p) { setArena(sArenaLo, p, sMem1 + 0x4000, PORT_MEM1_SIZE - 0x4000, sArenaHi, true); }
void OSSetArenaHi(void* p) { setArena(sArenaHi, p, sMem1 + 0x4000, PORT_MEM1_SIZE - 0x4000, sArenaLo, false); }
void* OSGetMEM1ArenaLo(void) { return sArenaLo; }
void* OSGetMEM1ArenaHi(void) { return sArenaHi; }
void OSSetMEM1ArenaLo(void* p) { OSSetArenaLo(p); }
void OSSetMEM1ArenaHi(void* p) { OSSetArenaHi(p); }
void* OSGetMEM2ArenaLo(void) { return sMEM2ArenaLo; }
void* OSGetMEM2ArenaHi(void) { return sMEM2ArenaHi; }
void OSSetMEM2ArenaLo(void* p) { setArena(sMEM2ArenaLo, p, sMem2 + 0x800, PORT_MEM2_SIZE - 0x800, sMEM2ArenaHi, true); }
void OSSetMEM2ArenaHi(void* p) { setArena(sMEM2ArenaHi, p, sMem2 + 0x800, PORT_MEM2_SIZE - 0x800, sMEM2ArenaLo, false); }
void* OSAllocFromArenaLo(u32 size, u32 align) {
    if (!aligned(align)) port_fatal("invalid arena alignment");
    const uint64_t p = (uint64_t((uintptr_t)sArenaLo) + align - 1) & ~uint64_t(align - 1);
    const uint64_t next = (p + size + align - 1) & ~uint64_t(align - 1);
    if (next > (uintptr_t)sArenaHi) port_fatal("low arena exhausted");
    sArenaLo = (void*)(uintptr_t)next; return (void*)(uintptr_t)p;
}
void* OSAllocFromArenaHi(u32 size, u32 align) {
    if (!aligned(align)) port_fatal("invalid arena alignment");
    uintptr_t hi = (uintptr_t)sArenaHi & ~(uintptr_t)(align - 1);
    if (hi < size) port_fatal("high arena allocation underflow");
    hi = (hi - size) & ~(uintptr_t)(align - 1);
    if (hi < (uintptr_t)sArenaLo) port_fatal("high arena exhausted");
    sArenaHi = (void*)hi; return (void*)hi;
}
void* OSAllocFromMEM1ArenaLo(u32 size, u32 align) { return OSAllocFromArenaLo(size, align); }
void* OSAllocFromMEM1ArenaHi(u32 size, u32 align) { return OSAllocFromArenaHi(size, align); }
void* OSInitAlloc(void* start, void* end, int heaps) {
    if (heaps < 0) port_fatal("invalid OS heap count");
    const uint64_t p = (uint64_t((uintptr_t)start) + uint64_t(heaps) * 12 + 31) & ~uint64_t(31);
    if ((uintptr_t)start > (uintptr_t)end || p > (uintptr_t)end) port_fatal("OS heap descriptors exceed arena");
    return (void*)(uintptr_t)p;
}
void DCFlushRange(void* a, u32 n) { cpuCacheFence(); gpuWriteNotify(a, n); }
void DCFlushRangeNoSync(void* a, u32 n) { cpuCacheFence(); gpuWriteNotify(a, n); }
void DCStoreRange(void* a, u32 n) { cpuCacheFence(); gpuWriteNotify(a, n); }
void DCStoreRangeNoSync(void* a, u32 n) { cpuCacheFence(); gpuWriteNotify(a, n); }
void DCInvalidateRange(void*, u32) { cpuCacheFence(); }
void DCZeroRange(void* addr, u32 size) {
    if (!size) return;
    const uintptr_t a = (uintptr_t)addr & ~(uintptr_t)31;
    if ((uintptr_t)addr > UINTPTR_MAX - 31 || size > UINTPTR_MAX - (uintptr_t)addr - 31) port_fatal("cache zero address overflow");
    const uintptr_t e = ((uintptr_t)addr + size + 31) & ~(uintptr_t)31;
    memset((void*)a, 0, e - a);
}
// This port does not generate or patch executable ARM instructions.
void ICInvalidateRange(void*, u32) { cpuCacheFence(); }
void ICFlashInvalidate(void) { cpuCacheFence(); }
void LCEnable(void) {}
void LCDisable(void) {}
void OSProtectRange(u32, void*, u32, u32) {}
u32 PPCMfhid2(void) { return 0; }
void PPCMthid2(u32) {}
u32 PPCMfmsr(void) { return 0x8000; }
void PPCMtmsr(u32) {}
u32 PPCMfwpar(void) { return 0; }
void PPCMtwpar(u32) {}
void PPCSync(void) { cpuCacheFence(); }
void PPCHalt(void) { port_fatal("PPCHalt"); }
}
