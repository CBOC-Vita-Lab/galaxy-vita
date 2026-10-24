#pragma once
#include "gpu_program.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct PortVitaGxmRaster {
    float viewport[4]; // EFB top-left x/y/width/height; CPU-XF clip is OpenGL
    int32_t scissor[4]; // left/top/right-exclusive/bottom-exclusive
    uint32_t width, height;
    uint32_t depth_test, depth_write, depth_func; // GX depth functions 0..7
    uint32_t cull; // none/front/back/both = 0/1/2/3, OpenGL CCW front
    uint32_t alpha_override;
    float alpha;
    uint32_t rgba6, dither; // unblended/no-fog storage; RGB-only EFB Bayer dither
} PortVitaGxmRaster;

// Active scene only. Exact scissor goes to u_raster, not tile RegionClip.
// Alpha override is a final output operation, not GX destination-alpha blending.
int port_vita_gxm_raster_apply(void* context, const PortVitaGxmRaster* raster,
                              uint32_t topology, float uniforms[8]);
#ifdef __cplusplus
}
#endif
