#include "gpu_memory.h"
#include <psp2/gxm.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/sysmem.h>

namespace {
constexpr int invalid = static_cast<int>(SCE_KERNEL_ERROR_INVALID_ARGUMENT);
constexpr uint32_t page = 4096;
bool descriptor(const PortVitaGpuMemory* block) {
    return block && reinterpret_cast<uintptr_t>(block) % alignof(PortVitaGpuMemory) == 0;
}
bool mapping_valid(uint32_t mapping) {
    return mapping == PORT_VITA_GPU_READ || mapping == PORT_VITA_GPU_RW ||
           mapping == PORT_VITA_GPU_VERTEX_USSE || mapping == PORT_VITA_GPU_FRAGMENT_USSE;
}
bool base_valid(void* base, uint32_t size) {
    const uint64_t address = reinterpret_cast<uintptr_t>(base);
    return base && address % page == 0 && address + size <= UINT32_MAX;
}
}

static int allocate(const char* name, uint32_t bytes, uint32_t mapping,
                    PortVitaGpuMemory* out, uint32_t granularity, SceKernelMemBlockType type) {
    if (!descriptor(out) || out->uid != -1 || out->base || out->size || out->mapping || out->usse_offset)
        return invalid;
    if (!name || !*name || !mapping_valid(mapping)) return invalid;
    if (!bytes || bytes > UINT32_MAX - (granularity - 1))
        return static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_SIZE);
    const uint32_t size = (bytes + granularity - 1) & ~(granularity - 1);
    const SceUID uid = sceKernelAllocMemBlock(name, type, size, nullptr);
    if (uid < 0) return uid;
    out->uid = uid;

    void* base = nullptr;
    int result = sceKernelGetMemBlockBase(uid, &base);
    if (result >= 0 && !base_valid(base, size)) result = static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    if (result >= 0) {
        out->base = base; out->size = size;
        if (mapping == PORT_VITA_GPU_VERTEX_USSE)
            result = sceGxmMapVertexUsseMemory(base, size, &out->usse_offset);
        else if (mapping == PORT_VITA_GPU_FRAGMENT_USSE)
            result = sceGxmMapFragmentUsseMemory(base, size, &out->usse_offset);
        else result = sceGxmMapMemory(base, size, mapping == PORT_VITA_GPU_RW ?
                                     SCE_GXM_MEMORY_ATTRIB_RW : SCE_GXM_MEMORY_ATTRIB_READ);
    }
    if (result < 0) {
        out->usse_offset = 0;
        const int cleanup = port_vita_gpu_memory_free(out);
        return cleanup < 0 ? cleanup : result;
    }
    out->mapping = mapping;
    return 0;
}

extern "C" int port_vita_gpu_memory_alloc(const char* name, uint32_t bytes, uint32_t mapping,
                                          PortVitaGpuMemory* out) {
    return allocate(name, bytes, mapping, out, page, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
}

extern "C" int port_vita_gpu_memory_alloc_cdram(const char* name, uint32_t bytes, uint32_t mapping,
                                                PortVitaGpuMemory* out) {
    if (mapping != PORT_VITA_GPU_READ && mapping != PORT_VITA_GPU_RW) return invalid;
    return allocate(name, bytes, mapping, out, 256 * 1024, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW);
}

extern "C" int port_vita_gpu_memory_free(PortVitaGpuMemory* block) {
    if (!descriptor(block)) return invalid;
    if (block->uid < 0) return static_cast<int>(SCE_KERNEL_ERROR_INVALID_UID);
    if (block->mapping) {
        if (!mapping_valid(block->mapping) || !base_valid(block->base, block->size)) return invalid;
        int result;
        if (block->mapping == PORT_VITA_GPU_VERTEX_USSE) result = sceGxmUnmapVertexUsseMemory(block->base);
        else if (block->mapping == PORT_VITA_GPU_FRAGMENT_USSE) result = sceGxmUnmapFragmentUsseMemory(block->base);
        else result = sceGxmUnmapMemory(block->base);
        if (result < 0) return result;
        block->mapping = block->usse_offset = 0;
    }
    const int result = sceKernelFreeMemBlock(block->uid);
    if (result < 0) return result;
    *block = {nullptr, 0, -1, 0, 0};
    return 0;
}
