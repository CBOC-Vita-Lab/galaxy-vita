#include "gpu_raster.h"
#include <psp2/gxm.h>
#include <algorithm>
#include <cmath>

extern "C" int port_vita_gxm_raster_apply(void* context, const PortVitaGxmRaster* r,
                                         uint32_t topology, float uniforms[8]) {
    constexpr int invalid=static_cast<int>(SCE_GXM_ERROR_INVALID_VALUE);
    if (!context || !r || !uniforms || !r->width || !r->height || r->width>4096 || r->height>4096 ||
        topology>PORT_VITA_GXM_TRIANGLES || r->depth_test>1 || r->depth_write>1 ||
        r->depth_func>7 || r->cull>3 || r->alpha_override>1 || !std::isfinite(r->alpha) ||
        r->alpha<0 || r->alpha>1 || r->rgba6>1 || r->dither>1 || (r->dither && !r->rgba6)) return invalid;
    for (float value:r->viewport) if (!std::isfinite(value)) return invalid;
    if (r->viewport[2]<0 || r->viewport[3]<0) return invalid;
    const float xs=r->viewport[2]*0.5f, ys=-r->viewport[3]*0.5f;
    const float xo=r->viewport[0]+xs, yo=r->viewport[1]-ys;
    if (!std::isfinite(xo) || !std::isfinite(yo)) return invalid;
    float packed[8]{};
    for (unsigned i=0;i<4;++i)
        packed[i]=float(std::clamp(r->scissor[i],0,int32_t((i&1) ? r->height : r->width)));
    packed[4]=float(r->alpha_override); packed[5]=r->alpha;
    packed[6]=float(r->rgba6); packed[7]=float(r->dither);
    auto* c=static_cast<SceGxmContext*>(context);
    // CPU XF already produces OpenGL clip Z. GXM applies the only [-1,1] to
    // [0,1] conversion here, and negative Y keeps EFB storage top-left.
    sceGxmSetViewport(c,xo,xs,yo,ys,0.5f,0.5f);
    const auto depth=static_cast<SceGxmDepthFunc>((r->depth_test ? r->depth_func : 7u)<<22);
    const auto write=r->depth_test && r->depth_write ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
    sceGxmSetFrontDepthFunc(c,depth); sceGxmSetBackDepthFunc(c,depth);
    sceGxmSetFrontDepthWriteEnable(c,write); sceGxmSetBackDepthWriteEnable(c,write);
    const bool polygons=topology==PORT_VITA_GXM_TRIANGLES;
    const auto cull=!polygons || r->cull==0 || r->cull==3 ? SCE_GXM_CULL_NONE :
                    r->cull==1 ? SCE_GXM_CULL_CCW : SCE_GXM_CULL_CW;
    sceGxmSetCullMode(c,cull);
    const bool empty=r->viewport[2]==0 || r->viewport[3]==0 || (polygons && r->cull==3);
    sceGxmSetRegionClip(c,empty ? SCE_GXM_REGION_CLIP_ALL : SCE_GXM_REGION_CLIP_NONE,0,0,0,0);
    for (unsigned i=0;i<8;++i) uniforms[i]=packed[i];
    return 0;
}
