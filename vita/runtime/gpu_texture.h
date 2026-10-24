#pragma once
#include "gpu_device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PortVitaGxmTexture PortVitaGxmTexture;
typedef struct PortVitaGxmTextureView PortVitaGxmTextureView;
typedef struct PortVitaGxmTextureInfo {
    uint32_t width, height, levels, native_levels, storage_bytes;
    uint64_t version;
    const void* pixels;
} PortVitaGxmTextureInfo;
typedef struct PortVitaGxmTextureLevel {
    uint32_t width, height, stride, offset, bytes;
} PortVitaGxmTextureLevel;

typedef struct PortVitaGxmSampler {
    uint32_t mode0, mode1;
    uint32_t wrap_s, wrap_t, min_filter, mag_filter, mip_filter;
    uint32_t edge_lod, round_lod, field_predict, anisotropy, bias_clamp;
    int32_t bias_units; // signed GX 1/32; not the native descriptor bias
    uint32_t min_units, max_units; // GX 1/16
    float bias, min_lod, max_lod;
} PortVitaGxmSampler;
typedef struct PortVitaGxmTextureViewInfo {
    PortVitaGxmTextureInfo image;
    PortVitaGxmSampler sampler;
    uint32_t native_min_lod, native_max_lod;
    const void* source; // identity cookie only; cache/frame owns source lifetime
    uint32_t explicit_linear_mip; // point-fetch contract; never bind to an implicit-LOD shader
} PortVitaGxmTextureViewInfo;

// Decode hardware BP bits, not GX enum ordinals. Reserved values reject.
int port_vita_gxm_sampler_decode(uint32_t mode0, uint32_t mode1, PortVitaGxmSampler* out);
// One immutable descriptor per GX sampler state, sharing all parent pixels.
// Unsupported reachable fractional LOD, mip tails or advanced LOD strategies
// reject, not truncate. GX bias remains in the shader; native bias is neutral.
int port_vita_gxm_texture_view_create(PortVitaGxmTexture*, uint32_t mode0, uint32_t mode1,
                                     PortVitaGxmTextureView** out);
// Separate, narrow contract for shader-owned diagonal LOD + integer linear/mip
// filtering. Only square POT images, GX linear min/mag + linear mip, no advanced
// flags. All original levels remain addressable, with neutral clamped native
// point fetch. The matching shader must apply original wrap to each corner.
int port_vita_gxm_texture_view_create_explicit_linear_mip(PortVitaGxmTexture*,uint32_t mode0,uint32_t mode1,
                                                        PortVitaGxmTextureView** out);
int port_vita_gxm_texture_view_info(const PortVitaGxmTextureView*, PortVitaGxmTextureViewInfo* out);
int port_vita_gxm_texture_view_descriptor(const PortVitaGxmTextureView*, PortVitaGxmDevice*, const void** out);
// Finish-gated, scene-closed. Parent release rejects while any view exists.
// Rejects cache-owned views; cache/frame leases cannot be bypassed here.
int port_vita_gxm_texture_view_release(PortVitaGxmTextureView**);

// Copies every supplied tightly packed RGBA8 mip, never regenerates/drops one.
// The source is immutable Frame TexImage data, not console RAM; one GPU object
// represents exactly one version. Native row/level padding is initialized.
// Creation is outside an open scene; failed rollback may return recovery owner.
int port_vita_gxm_texture_create(PortVitaGxmDevice* device, uint32_t width, uint32_t height,
                                 uint32_t levels, uint64_t version, const void* rgba,
                                 uint32_t rgba_bytes, PortVitaGxmTexture** out);
int port_vita_gxm_texture_info(const PortVitaGxmTexture* texture, PortVitaGxmTextureInfo* out);
int port_vita_gxm_texture_level(const PortVitaGxmTexture* texture, uint32_t level, PortVitaGxmTextureLevel* out);
// A native default sampler descriptor, NOT a complete GX sampler binding.
// Rectangular mip tails beyond GXM's smaller-dimension chain reject direct use;
// they stay uploaded for a future explicit-LOD/atlas path, never disappear.
// Exact GX filters/wrap and fractional min/max LOD still belong to the backend.
int port_vita_gxm_texture_descriptor(const PortVitaGxmTexture* texture, const void** out);
// Rejects an open scene, Finish before unmap, keeps failures retryable. Device
// cannot be destroyed while a texture owns its lease. No in-place version reuse.
int port_vita_gxm_texture_release(PortVitaGxmTexture** texture);

#ifdef __cplusplus
}
#endif
