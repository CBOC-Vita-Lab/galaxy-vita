#pragma once
#include "gpu_device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PortVitaGxmStaging PortVitaGxmStaging;
typedef struct PortVitaGxmSlice {
    uint64_t epoch;
    const PortVitaGpuMemory* block;
    uint32_t vertex_offset, index_offset, vertex_count, index_count;
} PortVitaGxmSlice;

// Append-only mapped slabs. Zero slab_bytes selects 2 MiB; large whole draws
// get a larger slab rather than dropping vertices. Single submission thread.
int port_vita_gxm_staging_create(PortVitaGxmDevice* device, uint32_t slab_bytes,
                                 PortVitaGxmStaging** out);
// Copy immutable CPU data outside a scene. Never overwrites submitted data.
int port_vita_gxm_staging_upload(PortVitaGxmStaging* staging,
    const PortVitaGxmVertex* vertices, uint32_t vertex_count,
    const uint16_t* indices, uint32_t index_count, PortVitaGxmSlice* out);
// Scene-closed, same epoch/block/vertex upload, aligned U16 index range only.
// No allocation, upload or Finish. Output is cleared on rejection.
int port_vita_gxm_staging_slice(PortVitaGxmStaging*,const PortVitaGxmSlice*,uint32_t first_index,
    uint32_t index_count,PortVitaGxmSlice* out);
// Null surface selects display; an external offscreen surface selects EFB.
int port_vita_gxm_staging_begin(PortVitaGxmStaging* staging, PortVitaGxmSurface* surface);
int port_vita_gxm_staging_draw(PortVitaGxmStaging* staging, PortVitaGxmProgram* program,
    const PortVitaGxmSlice* slice, uint32_t topology, const float* pixel,
    uint32_t pixel_floats, const void* const textures[8], const float raster[8]);
// Supplies exactly the programs actually bound, including failed submissions.
int port_vita_gxm_staging_end(PortVitaGxmStaging* staging);
// Closed scene -> real GPU Finish -> reuse. All old slice epochs are invalid.
// Failure retains ownership; release/reset can be retried after callback errors.
int port_vita_gxm_staging_reset(PortVitaGxmStaging* staging);
int port_vita_gxm_staging_release(PortVitaGxmStaging** staging);

#ifdef __cplusplus
}
#endif
