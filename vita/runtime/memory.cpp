#include "memory.h"

#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/error.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr/thread.h>

namespace {

constexpr uint32_t page_size = 4096;
constexpr uint32_t image_alignment = 32;
constexpr int invalid_argument = static_cast<int>(SCE_KERNEL_ERROR_INVALID_ARGUMENT);
constexpr int illegal_address = static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
constexpr int illegal_size = static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_SIZE);

bool aligned(const void* base, uint32_t alignment) {
    return base && (reinterpret_cast<uintptr_t>(base) & (alignment - 1)) == 0;
}

bool native_range(const void* base, uint32_t size) {
    const uint64_t address = reinterpret_cast<uintptr_t>(base);
    return base && size && address <= UINT32_MAX &&
           address + uint64_t(size) <= UINT32_MAX;
}

bool valid_range(const void* base, uint32_t size, uint32_t alignment) {
    return aligned(base, alignment) && native_range(base, size);
}

} // namespace

extern "C" int port_vita_memory_alloc(const char* name, uint32_t size, PortVitaMemoryBlock* out) {
    if (!aligned(out, alignof(PortVitaMemoryBlock))) return invalid_argument;
    *out = {nullptr, 0, -1};
    if (!name) return invalid_argument;
    if (!size || size % page_size) return illegal_size;

    const SceUID uid = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, nullptr);
    if (uid < 0) return uid;
    out->uid = uid;

    void* base = nullptr;
    int result = sceKernelGetMemBlockBase(uid, &base);
    if (result >= 0 && !valid_range(base, size, page_size)) result = illegal_address;
    if (result < 0) {
        // Keep ownership recoverable if the SDK also rejects rollback.
        const int cleanup = port_vita_memory_free(out);
        return cleanup < 0 ? cleanup : result;
    }
    out->base = base;
    out->size = size;
    return 0;
}

extern "C" int port_vita_memory_free(PortVitaMemoryBlock* block) {
    if (!aligned(block, alignof(PortVitaMemoryBlock))) return invalid_argument;
    if (block->uid < 0) return static_cast<int>(SCE_KERNEL_ERROR_INVALID_UID);
    const int result = sceKernelFreeMemBlock(block->uid);
    if (result < 0) return result;
    *block = {nullptr, 0, -1};
    return 0;
}

extern "C" int port_vita_memory_image(PortVitaImageSegment* segments, uint32_t capacity, uint32_t* count) {
    if (!aligned(count, alignof(uint32_t))) return invalid_argument;
    *count = 0;
    if (!aligned(segments, alignof(PortVitaImageSegment)) || !capacity) return invalid_argument;

    const uintptr_t function_address = reinterpret_cast<uintptr_t>(&port_vita_memory_image) & ~uintptr_t(1);
    const SceUID uid = sceKernelGetModuleIdByAddr(reinterpret_cast<void*>(function_address));
    if (uid < 0) return uid;
    SceKernelModuleInfo info{};
    info.size = sizeof(info);
    const int result = sceKernelGetModuleInfo(uid, &info);
    if (result < 0) return result;

    constexpr uint32_t maximum_segments = sizeof(info.segments) / sizeof(info.segments[0]);
    PortVitaImageSegment found[maximum_segments]{};
    uint32_t found_count = 0;
    for (const SceKernelSegmentInfo& segment : info.segments) {
        if (!segment.memsz) continue;
        if (!valid_range(segment.vaddr, segment.memsz, image_alignment)) return illegal_address;
        found[found_count++] = {segment.vaddr, segment.memsz};
    }
    if (!found_count) return illegal_size;
    if (found_count > capacity) return static_cast<int>(SCE_KERNEL_ERROR_INVALID_ARGUMENT_SIZE);
    for (uint32_t i = 0; i < found_count; ++i) segments[i] = found[i];
    *count = found_count;
    return 0;
}

extern "C" int port_vita_memory_free_sizes(uint32_t* user, uint32_t* cdram, uint32_t* phycont) {
    if (!aligned(user, alignof(uint32_t)) || !aligned(cdram, alignof(uint32_t)) ||
        !aligned(phycont, alignof(uint32_t))) return invalid_argument;
    *user = *cdram = *phycont = 0;
    SceKernelFreeMemorySizeInfo info{};
    info.size = sizeof(info);
    const int result = sceKernelGetFreeMemorySize(&info);
    if (result < 0) return result;
    if (info.size_user < 0 || info.size_cdram < 0 || info.size_phycont < 0) return illegal_size;
    *user = static_cast<uint32_t>(info.size_user);
    *cdram = static_cast<uint32_t>(info.size_cdram);
    *phycont = static_cast<uint32_t>(info.size_phycont);
    return 0;
}

extern "C" int port_vita_memory_valid_range(const void* base, uint32_t size) {
    if (!native_range(base, size)) return 0;
    SceKernelMemBlockInfo info{};
    info.size = sizeof(info);
    if (sceKernelGetMemBlockInfoByRange(const_cast<void*>(base), size, &info) < 0) return 0;
    if (!valid_range(info.mappedBase, info.mappedSize, alignof(uint32_t))) return 0;
    const uint64_t begin = reinterpret_cast<uintptr_t>(base);
    const uint64_t mapped_begin = reinterpret_cast<uintptr_t>(info.mappedBase);
    return begin >= mapped_begin && begin + uint64_t(size) <= mapped_begin + uint64_t(info.mappedSize);
}

extern "C" int port_vita_pin_core(int core) {
    if (core < 0 || core > 2) return invalid_argument;
    return sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x10000 << core); // SCE_KERNEL_CPU_MASK_USER_0..2
}

// Priorities of the port's own threads. The C++ thread library (and pthread_create) gives every thread the lowest user
// priority, 0xBF, so the game, the renderer, the movie decoders, the disc reader and the clocks all took turns in equal slices;
// a movie decode that fills every core then delays the disc read and the audio clock. Lower numbers run first:
//   1 clocks (vertical-blank and OS timers: tiny, they must run when they are due)    0x50
//   2 disc reader (its copies of file data are short, the loads wait for them)        0x70
//   3 game, renderer front end and back end                                           0x90
//   4 background work (movie decoders): only the time the others leave                0xBF
// GXV_PRIO=<mask> chooses which of the classes 1-3 are applied (bit 0 = class 1, bit 1 = class 2, bit 2 = class 3; "keep" = 0 =
// every thread stays where the library put it). The default is class 1 only. Measured with the attack movie (three decoder
// threads, 40 ms of wall time per frame at the lowest priority): the disc reader at 0x70 makes the decoders 11 % slower
// (45 ms) and the full set 18 % (48 ms) although the file is then read four times faster; the clocks alone cost nothing
// measurable (42 ms). Game and renderer threads are made at 0x9F already, above the decoders.
extern "C" int port_vita_thread_class(int cls) {
    static const unsigned mask = [] {
        const char* e = getenv("GXV_PRIO");
        if (!e || !*e) return 1u;
        return !strcmp(e, "keep") ? 0u : static_cast<unsigned>(atoi(e));
    }();
    static const int kPriority[5] = {0xBF, 0x50, 0x70, 0x90, 0xBF};
    if (cls < 1 || cls > 4) return invalid_argument;
    // The clocks (1), the disc and host-job threads (2) and background work (4) keep off core 0, the game's: the system scheduler
    // otherwise places these unpinned threads anywhere, and at 0x50 / 0x70 they preempt the game thread there. In the Observatory
    // (18 Oct console log, thp48d) the game thread's turn ran 20-23 % longer by the clock than it computed; these threads and the
    // audio ones are the ones that can take that time. Cores 1 and 2 (back end and front end) have room. GXV_HELPER_CORES=0: as before.
    static const bool helperCores = [] { const char* e = getenv("GXV_HELPER_CORES"); return !(e && *e == '0'); }();
    if (helperCores && cls != 3) sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x60000);  // SCE_KERNEL_CPU_MASK_USER_1 | USER_2
    if (cls <= 3 && !(mask & (1u << (cls - 1)))) return 0;
    return sceKernelChangeThreadPriority(0, kPriority[cls]);
}

// CPU time the calling thread has used, in microseconds.
extern "C" int64_t port_thread_cpu_us(void) {
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0) return 0;
    return static_cast<int64_t>(info.runClocks);
}

extern "C" int port_vita_thread_stack(void** base, uint32_t* size) {
    const bool valid_base = aligned(base, alignof(void*));
    const bool valid_size = aligned(size, alignof(uint32_t));
    if (valid_base) *base = nullptr;
    if (valid_size) *size = 0;
    if (!valid_base || !valid_size) return invalid_argument;

    const SceUID uid = sceKernelGetThreadId();
    if (uid < 0) return uid;
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    const int result = sceKernelGetThreadInfo(uid, &info);
    if (result < 0) return result;
    if (info.stackSize <= 0) return illegal_size;
    const uint32_t stack_size = static_cast<uint32_t>(info.stackSize);
    if (!valid_range(info.stack, stack_size, alignof(uint32_t))) return illegal_address;
    *base = info.stack;
    *size = stack_size;
    return 0;
}
