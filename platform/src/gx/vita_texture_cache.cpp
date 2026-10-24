#include "vita_texture_cache.h"
#include "../../../vita/runtime/gpu_device_internal.h"
#include "../../../vita/runtime/gpu_texture_internal.h"
#include <cstdio>
#include <new>

namespace gpu {
namespace {
constexpr int invalid=-1;
struct ImageEntry {
    std::shared_ptr<const TexImage> source;
    std::shared_ptr<const GxRamTexture> ramSource;
    PortVitaGxmTexture* native=nullptr;
    uint64_t version=0;
    uint32_t width=0,height=0,levels=0;
    bool ready=false;
};
struct ViewEntry {
    std::shared_ptr<ImageEntry> image;
    PortVitaGxmTextureView* native=nullptr;
    uint32_t mode0=0,mode1=0;
    bool explicitLinearMip=false;
};
}
struct VitaTextureCache {
    PortVitaGxmDevice* device=nullptr;
    bool closing=false;
    uint64_t nextRamVersion=1;
    std::vector<std::shared_ptr<ImageEntry>> images;
    std::vector<std::shared_ptr<ViewEntry>> views;
};

namespace {
int resolveView(VitaTextureCache* cache,const std::shared_ptr<ImageEntry>& image,const SpanTexture& binding,
                PortVitaGxmTextureView*& out,std::shared_ptr<const void>& lease,std::string& error,bool explicitLinearMip) {
    for (const auto& entry:cache->views) if (entry->image==image && entry->mode0==binding.mode0 && entry->mode1==binding.mode1 &&
        entry->explicitLinearMip==explicitLinearMip) {
        out=entry->native; lease=entry; return 0;
    }
    auto view=std::make_shared<ViewEntry>(); view->image=image; view->mode0=binding.mode0; view->mode1=binding.mode1;
    view->explicitLinearMip=explicitLinearMip;
    cache->views.push_back(view);
    const int result=port_vita_gxm_texture_view_create_owned(image->native,binding.mode0,binding.mode1,cache,&view->native,explicitLinearMip);
    if (result<0) {
        cache->views.pop_back();
        char parameters[160];
        std::snprintf(parameters,sizeof(parameters)," (mode0=%06x mode1=%06x image=%ux%u/%u native=%08x)",
            binding.mode0,binding.mode1,image->width,image->height,image->levels,unsigned(result));
        error=std::string("GX sampler resolution failed")+parameters;
        return result;
    }
    out=view->native; lease=view; return 0;
}
}

int createVitaTextureCache(PortVitaGxmDevice* device,VitaTextureCache** out) {
    if (!out || *out) return invalid;
    const int held=port_vita_gxm_device_hold_resource(device);
    if (held<0) return held;
    auto* cache=new (std::nothrow) VitaTextureCache;
    if (!cache) { port_vita_gxm_device_drop_resource(device); return invalid; }
    cache->device=device; *out=cache; return 0;
}

int resolveVitaImage(VitaTextureCache* cache,const Frame& frame,const SpanTexture& binding,
                     PortVitaGxmTextureView*& out,std::shared_ptr<const void>& lease,std::string& error,bool explicitLinearMip) {
    error.clear();
    auto fail=[&](int code,const char* message) { error=message; return code; };
    if (out || lease || !cache || cache->closing || port_vita_gxm_device_scene_active(cache->device) ||
        !port_vita_gxm_device_context(cache->device)) return fail(invalid,"native image cache is not open for scene-closed resolution");
    if (binding.kind!=SpanTextureKind::Image || binding.imageIndex>=frame.textures.size() ||
        !frame.textures[binding.imageIndex]) return fail(invalid,"texture binding has no immutable Frame image; EFB history needs the copier");
    const auto& source=frame.textures[binding.imageIndex];
    if (source->width!=binding.width || source->height!=binding.height || source->levels!=binding.levels ||
        source->version!=binding.version || source->rgba.size()>UINT32_MAX/4)
        return fail(invalid,"recorded texture dimensions/version do not match the immutable image");
    std::shared_ptr<ImageEntry> image;
    for (const auto& entry:cache->images) if (entry->ready && entry->source.get()==source.get() &&
        entry->version==binding.version && entry->width==binding.width && entry->height==binding.height && entry->levels==binding.levels) {
        image=entry; break;
    }
    if (!image) {
        image=std::make_shared<ImageEntry>(); image->source=source; image->version=binding.version;
        image->width=binding.width; image->height=binding.height; image->levels=binding.levels;
        // Install the CPU recovery owner before any fallible native allocation.
        cache->images.push_back(image);
        const int result=port_vita_gxm_texture_create(cache->device,binding.width,binding.height,binding.levels,binding.version,
            source->rgba.data(),uint32_t(source->rgba.size()*4),&image->native);
        if (result<0) {
            if (!image->native) cache->images.pop_back();
            return fail(result,"native immutable image upload failed; any partial GPU owner remains in the cache");
        }
        image->ready=true;
    }
    return resolveView(cache,image,binding,out,lease,error,explicitLinearMip);
}

int resolveVitaRamImage(VitaTextureCache* cache,uint64_t frame,const SpanTexture& binding,const SpanRegisters& registers,
                        unsigned unit,const VitaRamTextures& ram,PortVitaGxmTextureView*& out,
                        std::shared_ptr<const void>& lease,std::shared_ptr<const GxRamTexture>& provenance,std::string& error,bool explicitLinearMip) {
    error.clear();
    auto fail=[&](int code,const char* message) { error=message; return code; };
    if (out || lease || provenance || !cache || cache->closing || port_vita_gxm_device_scene_active(cache->device) ||
        !port_vita_gxm_device_context(cache->device) || !ram.history || ram.memory.frame!=frame)
        return fail(invalid,"RAM texture resolution needs empty outputs, an open cache and an ordered draw-time snapshot");
    GxRamTextureLayout layout;
    if (describeGxRamTexture(registers,unit,layout,error)<0) return invalid;
    const unsigned base=(unit<4 ? 0x80 : 0xa0)+(unit&3);
    if (binding.kind!=SpanTextureKind::EfbAddress || binding.address!=layout.address ||
        binding.mode0!=registers.bp[base] || binding.mode1!=registers.bp[base+4])
        return fail(invalid,"RAM texture binding disagrees with its actual GX BP address or sampler");
    GxTextureMemory memory=ram.memory;
    if (ram.window && !ram.window(layout.address,memory,error)) return invalid;
    if (memory.frame!=ram.memory.frame || memory.copySequence!=ram.memory.copySequence ||
        memory.tlut!=ram.memory.tlut || memory.tlutBase!=ram.memory.tlutBase || memory.tlutBytes!=ram.memory.tlutBytes)
        return fail(invalid,"RAM region query changed the ordered frame, copy cut or TLUT snapshot");
    std::shared_ptr<const GxCopyBytes> origin;
    if (lookupGxCopyBytes(ram.history,frame,binding.copyId,binding.address,origin,error)<0) return invalid;
    std::shared_ptr<ImageEntry> image;
    for (const auto& entry:cache->images) if (entry->ready && entry->ramSource && entry->ramSource->matches(layout,memory,origin)) {
        image=entry; break;
    }
    if (!image) {
        if (cache->nextRamVersion==UINT64_MAX) return fail(invalid,"RAM texture native version counter is exhausted");
        auto source=std::make_shared<GxRamTexture>();
        if (decodeGxRamTexture(registers,unit,memory,std::move(origin),*source,error)<0) return invalid;
        image=std::make_shared<ImageEntry>(); image->ramSource=std::move(source); image->version=cache->nextRamVersion++;
        image->width=layout.width; image->height=layout.height; image->levels=layout.levels;
        cache->images.push_back(image);
        const int result=port_vita_gxm_texture_create(cache->device,layout.width,layout.height,layout.levels,image->version,
            image->ramSource->rgba(),layout.texels*4,&image->native);
        if (result<0) {
            if (!image->native) cache->images.pop_back();
            return fail(result,"native RAM texture upload failed; any partial GPU owner remains in the cache");
        }
        image->ready=true;
    }
    const int result=resolveView(cache,image,binding,out,lease,error,explicitLinearMip);
    if (result>=0) provenance=image->ramSource;
    return result;
}

int collectVitaTextures(VitaTextureCache* cache) {
    if (!cache || port_vita_gxm_device_scene_active(cache->device)) return invalid;
    bool idle=false;
    for (const auto& entry:cache->views) if (entry.use_count()==1) { idle=true; break; }
    if (!idle) for (const auto& entry:cache->images) if (entry.use_count()==1) { idle=true; break; }
    // A leased or empty cache has no native deletions to fence.
    if (!idle) return 0;
    const int retired=port_vita_gxm_device_retire_resource(cache->device);
    if (retired<0) return retired;
    for (auto it=cache->views.begin();it!=cache->views.end();) {
        if (it->use_count()!=1) { ++it; continue; }
        const int result=port_vita_gxm_texture_view_release_owned(&(*it)->native,cache);
        if (result<0) return result;
        it=cache->views.erase(it);
    }
    for (auto it=cache->images.begin();it!=cache->images.end();) {
        if (it->use_count()!=1) { ++it; continue; }
        (*it)->ready=false;
        if ((*it)->native) {
            const int result=port_vita_gxm_texture_release(&(*it)->native);
            if (result<0) return result;
        }
        it=cache->images.erase(it);
    }
    return 0;
}

int releaseVitaTextureCache(VitaTextureCache** pointer) {
    if (!pointer || !*pointer) return invalid;
    auto* cache=*pointer; cache->closing=true;
    const int result=collectVitaTextures(cache);
    if (result<0) return result;
    if (!cache->views.empty() || !cache->images.empty()) return invalid;
    port_vita_gxm_device_drop_resource(cache->device);
    delete cache; *pointer=nullptr; return 0;
}
}
