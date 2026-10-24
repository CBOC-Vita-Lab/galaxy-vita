#pragma once
#include "gpu_program.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PortVitaGxmDevice PortVitaGxmDevice;
typedef struct PortVitaGxmSurface PortVitaGxmSurface;
typedef struct PortVitaGxmSurfaceInfo {
    uint32_t width, height, stride;
    const void* color_surface;
    const void* depth_surface;
    const void* color_data;
    const void* depth_data;
} PortVitaGxmSurfaceInfo;
typedef struct PortVitaGxmSurfaceSampleInfo {
    uint32_t width, height;
    uint32_t color_width, color_height, depth_width, depth_height;
    const void* color_texture;
    const void* depth_texture;
} PortVitaGxmSurfaceSampleInfo;

// One owner of GXM per process. SDK-default command/uniform rings, dynamic
// mapped patcher allocations, three 960x544 CDRAM display buffers and S8D24.
// No scene is begun and no frame is displayed by creation. Failure may return
// recovery ownership in out when rollback itself fails; release it to retry.
int port_vita_gxm_device_create(PortVitaGxmDevice** out);
// All external surfaces/programs/staging must be retired first. Open scenes
// reject release. Finish, drain display, detach scanout, destroy, then unmap.
// A failed teardown retains remaining ownership; only release may be retried.
int port_vita_gxm_device_release(PortVitaGxmDevice** device);
void* port_vita_gxm_device_context(PortVitaGxmDevice* device);
void* port_vita_gxm_device_patcher(PortVitaGxmDevice* device);

// Preferred program path: retains the device through registration, patched
// executable lifetime and failed rollback/release. Raw patcher borrowers must
// instead enforce that lifetime themselves. Creation/release are scene-closed.
int port_vita_gxm_device_program_create(PortVitaGxmDevice* device,
    const void* vertex_gxp, uint32_t vertex_bytes, const void* fragment_gxp, uint32_t fragment_bytes,
    const PortVitaGxmBlend* blend, uint32_t pixel_vectors, uint32_t texture_mask,
    PortVitaGxmProgram** out, PortVitaGxmProgramInfo* info);
int port_vita_gxm_device_program_release(PortVitaGxmDevice* device, PortVitaGxmProgram** program);
// Reflected capabilities of a ready device-owned program. Membership is checked
// before any program dereference; legacy/recovery handles are not guessed.
int port_vita_gxm_device_program_info(const PortVitaGxmDevice* device,
    const PortVitaGxmProgram* program, PortVitaGxmProgramInfo* out);

// Full-size offscreen EFB storage, not a reduced-resolution display proxy.
// Native descriptors are borrowed; opaque SDK enums/structures never cross ABI.
int port_vita_gxm_surface_create(PortVitaGxmDevice* device, uint32_t width,
                                 uint32_t height, PortVitaGxmSurface** out);
int port_vita_gxm_surface_info(const PortVitaGxmSurface* surface, PortVitaGxmSurfaceInfo* out);
// Scene-closed and Finish-gated. Point/clamp one-level descriptors alias the
// real color/tiled-depth planes, not immutable snapshots. Caller must retain
// the surface lease until the sampling scene Ends AND Finishes. Padded sampling
// dimensions differ from logical EFB dimensions and must be used for UVs.
int port_vita_gxm_surface_sample(PortVitaGxmDevice*,PortVitaGxmSurface*,PortVitaGxmSurfaceSampleInfo*);
// Public release rejects cache-managed borrowed surfaces.
int port_vita_gxm_surface_release(PortVitaGxmSurface** surface);

int port_vita_gxm_device_begin(PortVitaGxmDevice* device, PortVitaGxmSurface* surface);
int port_vita_gxm_device_begin_display(PortVitaGxmDevice* device);
// Complete list of programs bound during this scene, including failed draws.
int port_vita_gxm_device_end(PortVitaGxmDevice* device,
                             PortVitaGxmProgram* const* programs, uint32_t count);
// Only a successfully ended display scene can be queued. A queue failure keeps
// that exact buffer ready for retry. GPU sync, not CPU timing, gates scanout.
int port_vita_gxm_device_present(PortVitaGxmDevice* device);
// No Finish inside an open scene. These retire staging or await callbacks;
// they are not ordinary-play FPS measurements or replacements for EndScene.
int port_vita_gxm_device_finish(PortVitaGxmDevice* device);
int port_vita_gxm_device_drain_display(PortVitaGxmDevice* device);
int port_vita_gxm_device_error(const PortVitaGxmDevice* device);

#ifdef __cplusplus
}
#endif
