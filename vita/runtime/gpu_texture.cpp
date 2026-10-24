#include "gpu_texture.h"
#include "gpu_texture_internal.h"
#include "gpu_device_internal.h"
#include <psp2/gxm.h>
#include <psp2/kernel/error.h>
#include <cstring>
#include <new>

// Exported by SceGxm (NID B79E43DD); absent from this SDK's public header.
extern "C" int sceGxmTextureSetLodMin(SceGxmTexture*, unsigned int);

struct PortVitaGxmTexture {
    PortVitaGxmDevice* owner = nullptr;
    PortVitaGpuMemory memory{nullptr, 0, -1, 0, 0};
    SceGxmTexture descriptor{};
    PortVitaGxmTextureLevel layouts[13]{};
    uint32_t width = 0, height = 0, levels = 0, native_levels = 0, storage_bytes = 0;
    uint64_t version = 0;
    const void* source = nullptr;
    uint32_t views = 0;
    bool ready = false;
};

struct PortVitaGxmTextureView {
    PortVitaGxmTexture* image = nullptr;
    const void* manager = nullptr;
    SceGxmTexture descriptor{};
    PortVitaGxmSampler sampler{};
    uint32_t min_lod = 0, max_lod = 0;
    bool explicit_linear_mip = false;
};

namespace {
constexpr int invalid = static_cast<int>(SCE_KERNEL_ERROR_INVALID_ARGUMENT);
unsigned power_of_two(unsigned size) { unsigned result = 1; while (result < size) result *= 2; return result; }
unsigned level_count(unsigned size) { unsigned result = 0; do { ++result; size /= 2; } while (size); return result; }
unsigned half(unsigned size) { return size > 1 ? size / 2 : 1; }
constexpr int unsupported = static_cast<int>(SCE_GXM_ERROR_UNSUPPORTED);
}

extern "C" int port_vita_gxm_sampler_decode(uint32_t mode0, uint32_t mode1, PortVitaGxmSampler* out) {
    if (!out) return invalid;
    *out = {};
    const uint32_t s=mode0&3, t=(mode0>>2)&3, filter=(mode0>>5)&7, aniso=(mode0>>19)&3;
    if ((mode0>>22) || (mode1>>16) || s==3 || t==3 || filter==3 || filter==7 || aniso==3) return invalid;
    const int32_t bias=int32_t((mode0>>9)&255) - ((mode0&(1u<<16)) ? 256 : 0);
    const uint32_t min=mode1&255, max=(mode1>>8)&255;
    *out={mode0,mode1,s,t,filter,(mode0>>4)&1,
          (filter==1 || filter==5) ? 1u : (filter==2 || filter==6) ? 2u : 0u,
          (mode0&(1u<<8)) ? 0u : 1u,(mode0>>17)&1,(mode0>>18)&1,1u<<aniso,(mode0>>21)&1,
          bias,min,max,float(bias)/32,float(min)/16,float(max)/16};
    return 0;
}

static int create_view(PortVitaGxmTexture* image,uint32_t mode0,uint32_t mode1,bool explicitLinearMip,
                       PortVitaGxmTextureView** out) {
    if (!out || *out || !image || !image->ready || image->views==UINT32_MAX ||
        port_vita_gxm_device_scene_active(image->owner) || !port_vita_gxm_device_context(image->owner)) return invalid;
    PortVitaGxmSampler sampler{};
    int result=port_vita_gxm_sampler_decode(mode0,mode1,&sampler);
    if (result<0) return result;
    // Diagonal/edge derivatives cannot change a base-only sample when both
    // minification and magnification use the same filter.
    const bool lodSelectsSampling=sampler.mip_filter || ((sampler.min_filter==4)!=(sampler.mag_filter!=0));
    if ((!explicitLinearMip && !sampler.edge_lod && lodSelectsSampling) || sampler.round_lod || sampler.field_predict ||
        sampler.anisotropy!=1 || sampler.bias_clamp)
        return unsupported;
    const bool mirroredBase=image->levels==1 && sampler.min_filter==4 && sampler.mag_filter==1 &&
        (sampler.wrap_s==2 || sampler.wrap_t==2);
    if (explicitLinearMip && ((!mirroredBase && (sampler.edge_lod || sampler.min_filter!=6 || sampler.mag_filter!=1)) ||
        image->width!=image->height || (image->width&(image->width-1)) || image->levels!=image->native_levels))
        return unsupported;
    unsigned min=0, max=0;
    if (explicitLinearMip) {
        if (sampler.min_units>sampler.max_units) return invalid;
        max=image->levels-1;
    } else if (sampler.mip_filter) {
        if (sampler.min_units>sampler.max_units) return invalid;
        const unsigned last=(image->levels-1)*16;
        const unsigned low=sampler.min_units<last ? sampler.min_units : last;
        const unsigned high=sampler.max_units<last ? sampler.max_units : last;
        if ((low&15) || (high&15)) return unsupported;
        min=low/16; max=high/16;
        if (max>=image->native_levels) return unsupported;
    }
    auto* view=new (std::nothrow) PortVitaGxmTextureView;
    if (!view) return static_cast<int>(SCE_KERNEL_ERROR_NO_MEMORY);
    view->image=image; view->descriptor=image->descriptor; view->sampler=sampler;
    view->min_lod=min; view->max_lod=max;
    view->explicit_linear_mip=explicitLinearMip;
    constexpr SceGxmTextureAddrMode wraps[]={SCE_GXM_TEXTURE_ADDR_CLAMP,SCE_GXM_TEXTURE_ADDR_REPEAT,SCE_GXM_TEXTURE_ADDR_MIRROR};
    constexpr SceGxmTextureFilter filters[]={SCE_GXM_TEXTURE_FILTER_POINT,SCE_GXM_TEXTURE_FILTER_MIPMAP_POINT,
        SCE_GXM_TEXTURE_FILTER_MIPMAP_POINT,SCE_GXM_TEXTURE_FILTER_POINT,SCE_GXM_TEXTURE_FILTER_LINEAR,
        SCE_GXM_TEXTURE_FILTER_MIPMAP_LINEAR,SCE_GXM_TEXTURE_FILTER_MIPMAP_LINEAR};
    // Linear GXM images cannot use MIRROR. Manual shaders wrap each original
    // integer corner before fetching its center through this clamped descriptor.
    result=sceGxmTextureSetUAddrMode(&view->descriptor,explicitLinearMip ? SCE_GXM_TEXTURE_ADDR_CLAMP : wraps[sampler.wrap_s]);
    if (result>=0) result=sceGxmTextureSetVAddrMode(&view->descriptor,explicitLinearMip ? SCE_GXM_TEXTURE_ADDR_CLAMP : wraps[sampler.wrap_t]);
    if (result>=0) result=sceGxmTextureSetMinFilter(&view->descriptor,explicitLinearMip ?
        (mirroredBase ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_MIPMAP_POINT) : filters[sampler.min_filter]);
    if (result>=0) result=sceGxmTextureSetMagFilter(&view->descriptor,!explicitLinearMip && sampler.mag_filter ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
    if (result>=0) result=sceGxmTextureSetMipFilter(&view->descriptor,!explicitLinearMip && sampler.mip_filter==2 ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
    // Full GX 1/32 bias is already in the TEV shader's tex2Dbias argument.
    if (result>=0) result=sceGxmTextureSetLodBias(&view->descriptor,31);
    if (result>=0) result=sceGxmTextureSetMipmapCount(&view->descriptor,max+1);
    if (result>=0) result=sceGxmTextureSetLodMin(&view->descriptor,min);
    if (result>=0) result=sceGxmTextureValidate(&view->descriptor);
    if (result<0) { delete view; return result; }
    ++image->views; *out=view; return 0;
}
extern "C" int port_vita_gxm_texture_view_create(PortVitaGxmTexture* image,uint32_t mode0,uint32_t mode1,
                                                 PortVitaGxmTextureView** out) {
    return create_view(image,mode0,mode1,false,out);
}
extern "C" int port_vita_gxm_texture_view_create_explicit_linear_mip(PortVitaGxmTexture* image,uint32_t mode0,uint32_t mode1,
                                                                     PortVitaGxmTextureView** out) {
    return create_view(image,mode0,mode1,true,out);
}

extern "C" int port_vita_gxm_texture_view_info(const PortVitaGxmTextureView* view, PortVitaGxmTextureViewInfo* out) {
    if (!out) return invalid;
    *out={};
    if (!view || port_vita_gxm_texture_info(view->image,&out->image)<0) return invalid;
    out->sampler=view->sampler; out->native_min_lod=view->min_lod; out->native_max_lod=view->max_lod;
    out->source=view->image->source; out->explicit_linear_mip=view->explicit_linear_mip; return 0;
}
extern "C" int port_vita_gxm_texture_view_descriptor(const PortVitaGxmTextureView* view, PortVitaGxmDevice* device, const void** out) {
    if (!out || *out || !view || !view->image->ready || view->image->owner!=device ||
        !port_vita_gxm_device_context(device)) return invalid;
    *out=&view->descriptor; return 0;
}
static int release_view(PortVitaGxmTextureView** pointer, const void* owner) {
    if (!pointer || !*pointer) return invalid;
    auto* view=*pointer;
    if (view->manager!=owner) return invalid;
    const int result=port_vita_gxm_device_retire_resource(view->image->owner);
    if (result<0) return result;
    --view->image->views; delete view; *pointer=nullptr; return 0;
}
extern "C" int port_vita_gxm_texture_view_release(PortVitaGxmTextureView** pointer) {
    return release_view(pointer,nullptr);
}
int port_vita_gxm_texture_view_create_owned(PortVitaGxmTexture* image, uint32_t mode0, uint32_t mode1,
                                           const void* owner, PortVitaGxmTextureView** out,bool explicitLinearMip) {
    if (!owner) return invalid;
    const int result=create_view(image,mode0,mode1,explicitLinearMip,out);
    if (result>=0) (*out)->manager=owner;
    return result;
}
int port_vita_gxm_texture_view_release_owned(PortVitaGxmTextureView** pointer,const void* owner) {
    return owner ? release_view(pointer,owner) : invalid;
}

extern "C" int port_vita_gxm_texture_create(PortVitaGxmDevice* device, uint32_t width, uint32_t height,
                                             uint32_t levels, uint64_t version, const void* rgba,
                                             uint32_t rgba_bytes, PortVitaGxmTexture** out) {
    if (!out || *out || !rgba || !width || !height || width > 4096 || height > 4096 || !levels ||
        levels > level_count(width > height ? width : height)) return invalid;
    uint32_t expected = 0;
    for (unsigned i = 0, w = width, h = height; i < levels; ++i, w = half(w), h = half(h)) expected += w * h * 4;
    if (rgba_bytes != expected) return invalid;
    int result = port_vita_gxm_device_hold_resource(device);
    if (result < 0) return result;
    auto* texture = new (std::nothrow) PortVitaGxmTexture;
    if (!texture) { port_vita_gxm_device_drop_resource(device); return static_cast<int>(SCE_KERNEL_ERROR_NO_MEMORY); }
    *out = texture; texture->owner = device;
    texture->width = width; texture->height = height; texture->levels = levels; texture->version = version;
    texture->source = rgba;
    texture->native_levels = level_count(width < height ? width : height);
    if (texture->native_levels > levels) texture->native_levels = levels;
    // Linear GXM rows use 8-texel pitch. Mip starts reserve power-of-two
    // footprints independently of visible NPOT dimensions. Keep tail storage.
    unsigned layout_width = power_of_two(width), layout_height = power_of_two(height);
    for (unsigned i = 0, w = width, h = height; i < levels; ++i, w = half(w), h = half(h)) {
        const unsigned footprint_stride = (layout_width + 7) & ~7u;
        const unsigned bytes = footprint_stride * layout_height * 4;
        texture->layouts[i] = {w, h, (w + 7) & ~7u, texture->storage_bytes, bytes};
        texture->storage_bytes += bytes;
        layout_width = half(layout_width); layout_height = half(layout_height);
    }
    result = port_vita_gpu_memory_alloc("GalaxyTexture", texture->storage_bytes, PORT_VITA_GPU_READ, &texture->memory);
    if (result >= 0) {
        std::memset(texture->memory.base, 0, texture->memory.size);
        const auto* source = static_cast<const char*>(rgba);
        auto* destination = static_cast<char*>(texture->memory.base);
        for (unsigned i = 0; i < levels; ++i) {
            const auto& layout = texture->layouts[i];
            for (unsigned y = 0; y < layout.height; ++y) {
                std::memcpy(destination + layout.offset + y * layout.stride * 4, source, layout.width * 4);
                source += layout.width * 4;
            }
        }
        result = sceGxmTextureInitLinear(&texture->descriptor, texture->memory.base, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
                                          width, height, texture->native_levels);
    }
    if (result >= 0) result = sceGxmTextureValidate(&texture->descriptor);
    if (result < 0) {
        const int cleanup = port_vita_gxm_texture_release(out); return cleanup < 0 ? cleanup : result;
    }
    texture->ready = true; return 0;
}
extern "C" int port_vita_gxm_texture_info(const PortVitaGxmTexture* texture, PortVitaGxmTextureInfo* out) {
    if (!texture || !texture->ready || !out) return invalid;
    *out = {texture->width, texture->height, texture->levels, texture->native_levels, texture->storage_bytes,
            texture->version, texture->memory.base}; return 0;
}
extern "C" int port_vita_gxm_texture_level(const PortVitaGxmTexture* texture, uint32_t level, PortVitaGxmTextureLevel* out) {
    if (!texture || !texture->ready || !out || level >= texture->levels) return invalid;
    *out = texture->layouts[level]; return 0;
}
extern "C" int port_vita_gxm_texture_descriptor(const PortVitaGxmTexture* texture, const void** out) {
    if (!texture || !texture->ready || !out || *out) return invalid;
    if (texture->native_levels != texture->levels) return static_cast<int>(SCE_GXM_ERROR_UNSUPPORTED);
    *out = &texture->descriptor; return 0;
}
extern "C" int port_vita_gxm_texture_release(PortVitaGxmTexture** pointer) {
    if (!pointer || !*pointer) return invalid;
    auto* texture = *pointer;
    if (texture->views) return invalid;
    const int retired = port_vita_gxm_device_retire_resource(texture->owner);
    if (retired < 0) return retired;
    texture->ready = false;
    if (texture->memory.uid >= 0) {
        const int result = port_vita_gpu_memory_free(&texture->memory); if (result < 0) return result;
    }
    port_vita_gxm_device_drop_resource(texture->owner);
    delete texture; *pointer = nullptr; return 0;
}
