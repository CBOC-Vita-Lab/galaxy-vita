#pragma once
#include "gpu_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PortVitaGxmProgram PortVitaGxmProgram;
typedef struct PortVitaGxmVertex {
    float clip[4], color[2][4], tex[8][3], view_z, gx_z;
} PortVitaGxmVertex;
typedef struct PortVitaGxmBlend {
    uint32_t color_mask, color_func, alpha_func, color_src, color_dst, alpha_src, alpha_dst;
} PortVitaGxmBlend;
typedef struct PortVitaGxmProgramInfo {
    uint32_t attribute_mask, texture_mask, pixel_floats, vertex_uniform_bytes, fragment_uniform_bytes;
    uint32_t raster_floats;
} PortVitaGxmProgramInfo;

// Receives complete compiler-produced GXP blobs, not untrusted serialized data.
// Patcher is borrowed and must outlive this program. Copies both blobs and uses
// SDK reflection for attribute registers, sampler units and default uniforms.
// Blend values are GXM numeric values, conveyed as scalars across the enum ABI.
// Rollback failure returns remaining ownership in out for release/retry.
int port_vita_gxm_program_create(void* patcher, const void* vertex_gxp, uint32_t vertex_bytes,
                                const void* fragment_gxp, uint32_t fragment_bytes,
                                const PortVitaGxmBlend* blend, uint32_t pixel_vectors,
                                uint32_t texture_mask, PortVitaGxmProgram** out,
                                PortVitaGxmProgramInfo* info);
// Rejects release while the last bound scene is unsubmitted. Use scene_end
// below, then this finishes the context before releasing/unregistering programs.
// All failures preserve remaining ownership for retry. Context must outlive it.
// Device-created programs reject this raw release; use device_program_release.
int port_vita_gxm_program_release(PortVitaGxmProgram** program);

// Actually submits EndScene, then acknowledges all programs bound in that scene.
// The scene owner supplies its complete used-program list (including binds that
// failed before drawing). Failed submission keeps release blocked. Notifications
// are borrowed native GXM notification pointers; null is allowed. This is the
// scene owner's EndScene call, not an additional call after one elsewhere.
int port_vita_gxm_scene_end(void* context, PortVitaGxmProgram* const* programs,
                           uint32_t count, const void* vertex_notification,
                           const void* fragment_notification);

enum { PORT_VITA_GXM_POINTS = 0, PORT_VITA_GXM_LINES = 1, PORT_VITA_GXM_TRIANGLES = 2 };
// Caller owns the active scene/raster state and mapped staging. No draw allocation
// or circular wrap here. Buffers/textures must survive sceGxmFinish/scene fencing.
// Uses ClipVertex-compatible F32 data and whole-primitive, local U16 indices.
int port_vita_gxm_program_draw(PortVitaGxmProgram* program, void* context,
                              const PortVitaGpuMemory* vertices, uint32_t vertex_offset,
                              const PortVitaGpuMemory* indices, uint32_t index_offset,
                              uint32_t vertex_count, uint32_t index_count, uint32_t topology,
                              const float* pixel, uint32_t pixel_floats,
                              const void* const textures[8]);

// Raster-aware shaders require these eight separately reflected floats. Legacy
// draw rejects such a program rather than silently using an empty scissor.
int port_vita_gxm_program_draw_raster(PortVitaGxmProgram* program, void* context,
    const PortVitaGpuMemory* vertices, uint32_t vertex_offset,
    const PortVitaGpuMemory* indices, uint32_t index_offset,
    uint32_t vertex_count, uint32_t index_count, uint32_t topology,
    const float* pixel, uint32_t pixel_floats, const void* const textures[8],
    const float raster[8]);

#ifdef __cplusplus
}
#endif
