#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Scalar ABI only. These are port values, not SDK enum/structure layouts.
enum { PORT_VITA_GPU_READ = 1, PORT_VITA_GPU_RW = 3,
       PORT_VITA_GPU_VERTEX_USSE = 4, PORT_VITA_GPU_FRAGMENT_USSE = 5 };
typedef struct PortVitaGpuMemory {
    void* base;
    uint32_t size;
    int32_t uid;
    uint32_t mapping, usse_offset;
} PortVitaGpuMemory;

// Requires an empty {0,0,-1,0,0} descriptor. Rounds nonzero size to 4 KiB.
// Allocates uncached USER memory, then maps the exact allocated range for GXM.
// Failed rollback keeps recoverable ownership in out; never discard its UID.
int port_vita_gpu_memory_alloc(const char* name, uint32_t bytes, uint32_t mapping,
                               PortVitaGpuMemory* out);
// CDRAM color/depth/texture storage, rounded to 256 KiB. READ/RW only;
// executable USSE allocations remain uncached USER memory.
int port_vita_gpu_memory_alloc_cdram(const char* name, uint32_t bytes, uint32_t mapping,
                                    PortVitaGpuMemory* out);
// GPU use must already have completed. Unmaps before freeing. Any failure keeps
// remaining ownership for retry, and an unmapped block is never unmapped twice.
int port_vita_gpu_memory_free(PortVitaGpuMemory* block);

#ifdef __cplusplus
}
#endif
