#include "vita_material_cache.h"
#include "../../../vita/runtime/gpu_device_internal.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <limits>
#include <new>

namespace gpu {
namespace {
constexpr int invalid=-1;
constexpr size_t idleMaterialLimit=32;
static_assert(sizeof(PortVitaGxmBlend)==7*sizeof(uint32_t));
bool sameBlend(const PortVitaGxmBlend& a,const PortVitaGxmBlend& b) { return std::memcmp(&a,&b,sizeof(a))==0; }
struct Key {
    ShaderUid uid{};
    PortVitaGxmBlend blend{};
    VitaFragmentProfile profile=VitaFragmentProfile::Raster;
    uint32_t manualTextureMask=0;
    bool operator==(const Key& other) const { return uid==other.uid && sameBlend(blend,other.blend) && profile==other.profile && manualTextureMask==other.manualTextureMask; }
};
struct KeyHash {
    size_t operator()(const Key& key) const {
        size_t h=ShaderUidHash{}(key.uid);
        for (uint32_t value:{key.blend.color_mask,key.blend.color_func,key.blend.alpha_func,
            key.blend.color_src,key.blend.color_dst,key.blend.alpha_src,key.blend.alpha_dst})
            h=(h^value)*1099511628211ull;
        if(key.profile!=VitaFragmentProfile::Raster) h=(h^uint32_t(key.profile))*1099511628211ull;
        return key.manualTextureMask ? (h^key.manualTextureMask)*1099511628211ull : h;
    }
};
bool sameLayout(const VitaFragmentLayout& a,const VitaFragmentLayout& b) {
    return a.pixel.vectors==b.pixel.vectors && a.pixel.textureMask==b.pixel.textureMask && a.pixel.slots==b.pixel.slots &&
        a.snapshotSampler==b.snapshotSampler && a.snapshotSlot==b.snapshotSlot &&
        a.manualTextureMask==b.manualTextureMask && a.manualSlots==b.manualSlots;
}
bool reflectedLayout(VitaFragmentProfile profile,const VitaFragmentLayout& layout,const PortVitaGxmProgramInfo& info) {
    if (info.raster_floats!=8 || info.pixel_floats>layout.pixel.vectors*4 || (info.texture_mask&~layout.pixel.textureMask)) return false;
    if((info.texture_mask&layout.manualTextureMask)!=layout.manualTextureMask) return false;
    for(unsigned unit=0;unit<8;++unit) if(layout.manualTextureMask&(1u<<unit))
        if(layout.manualSlots[unit]<0 || info.pixel_floats<uint32_t(layout.manualSlots[unit]+1)*4) return false;
    return profile==VitaFragmentProfile::Raster ||
        (layout.snapshotSampler<8 && layout.snapshotSlot<27 && (info.texture_mask&(1u<<layout.snapshotSampler)) &&
         info.pixel_floats>=(layout.snapshotSlot+1)*4);
}
}
struct VitaMaterial {
    VitaMaterial(const Key& key,const VitaFragmentLayout& layout,PortVitaGxmDevice* device):key(key),layout(layout),device(device) {}
    const Key key;
    const VitaFragmentLayout layout;
    PortVitaGxmDevice* const device;
    PortVitaGxmProgram* program=nullptr;
    uint64_t lastUse=0;
    bool ready=false;
};
struct VitaMaterialCache {
    PortVitaGxmDevice* device=nullptr;
    bool closing=false;
    uint64_t useSequence=0;
    std::unordered_map<Key,std::shared_ptr<VitaMaterial>,KeyHash> entries;
};

bool installVitaShader(VitaShaderLibrary& library,VitaShaderArtifact artifact,std::string& error) {
    error.clear();
    if (uint32_t(artifact.stage)>1 || artifact.source.empty() || artifact.gxp.size()<8 ||
        artifact.gxp.size()>4*1024*1024 || artifact.precisionFlags) {
        error="shader library requires complete exact-source compiler GXP with every fast flag disabled"; return false;
    }
    auto& entries=artifact.stage==VitaShaderStage::Vertex ? library.vertex : library.fragment;
    auto found=entries.find(artifact.source);
    if (found!=entries.end()) {
        if (found->second==artifact.gxp) return true;
        error="conflicting compiler artifact for an immutable shader source/profile"; return false;
    }
    entries.emplace(std::move(artifact.source),std::move(artifact.gxp)); return true;
}
const std::vector<unsigned char>* findVitaShader(const VitaShaderLibrary& library,VitaShaderStage stage,const std::string& source) {
    if (uint32_t(stage)>1) return nullptr;
    const auto& entries=stage==VitaShaderStage::Vertex ? library.vertex : library.fragment;
    const auto found=entries.find(source); return found==entries.end() ? nullptr : &found->second;
}

int createVitaMaterialCache(PortVitaGxmDevice* device,VitaMaterialCache** out) {
    if (!out || *out) return invalid;
    const int held=port_vita_gxm_device_hold_resource(device); if (held<0) return held;
    auto* cache=new (std::nothrow) VitaMaterialCache;
    if (!cache) { port_vita_gxm_device_drop_resource(device); return invalid; }
    cache->device=device; *out=cache; return 0;
}
int resolveVitaMaterial(VitaMaterialCache* cache,const ShaderUid& uid,const PortVitaGxmBlend& blend,
                        const VitaShaderLibrary& library,VitaMaterial*& out,std::shared_ptr<const void>& lease,
                        std::string& error) {
    return resolveVitaMaterialProfile(cache,uid,blend,VitaFragmentProfile::Raster,library,out,lease,error);
}
int resolveVitaMaterialProfile(VitaMaterialCache* cache,const ShaderUid& uid,const PortVitaGxmBlend& blend,
                        VitaFragmentProfile profile,const VitaShaderLibrary& library,VitaMaterial*& out,
                        std::shared_ptr<const void>& lease,std::string& error,uint32_t manualMask) {
    error.clear();
    auto fail=[&](int code,const char* message) { error=message; return code; };
    if (out || lease || !cache || cache->closing || port_vita_gxm_device_scene_active(cache->device) ||
        !port_vita_gxm_device_context(cache->device)) return fail(invalid,"material cache is not open for scene-closed resolution");
    if (cache->useSequence==std::numeric_limits<uint64_t>::max())
        return fail(invalid,"material cache use sequence exhausted; close before retry");
    if (!isKnownVitaFragmentProfile(profile))
        return fail(invalid,"unknown native fragment source profile");
    PortVitaGxmBlend effective=blend;
    if (profile!=VitaFragmentProfile::Raster) {
        uint32_t src=0,dst=0;
        switch(profile) {
        case VitaFragmentProfile::Rgba6SrcAlphaSnapshot:
        case VitaFragmentProfile::Rgba6SrcAlphaDitherSnapshot: src=4; dst=5; break;
        case VitaFragmentProfile::Rgba6AddSnapshot: src=dst=1; break;
        case VitaFragmentProfile::Rgba6SrcAlphaAddSnapshot: src=4; dst=1; break;
        default: return fail(invalid,"unknown native snapshot factor strategy");
        }
        if (blend.color_mask>15 || blend.color_func!=1 || blend.alpha_func!=1 ||
            blend.color_src!=src || blend.color_dst!=dst || blend.alpha_src!=src || blend.alpha_dst!=dst)
            return fail(invalid,"snapshot profile does not implement these original blend factors");
        effective={blend.color_mask,0,0,1,0,1,0};
    }
    const Key key{uid,blend,profile,manualMask};
    const auto found=cache->entries.find(key);
    if (found!=cache->entries.end()) {
        if (!found->second->ready) return fail(invalid,"material has retained native rollback ownership; collect before retry");
        found->second->lastUse=++cache->useSequence;
        out=found->second.get(); lease=found->second; return 0;
    }
    VitaFragmentLayout layout;
    if (!describeVitaFragment(uid,profile,layout,error,manualMask)) return invalid;
    const auto vsSource=genVitaCpuVertexShader(uid);
    const auto fsSource=genVitaFragmentShader(uid,profile,manualMask);
    const auto* vs=findVitaShader(library,VitaShaderStage::Vertex,vsSource);
    const auto* fs=findVitaShader(library,VitaShaderStage::Fragment,fsSource);
    if (!vs || !fs) {
        error=profile==VitaFragmentProfile::Raster ?
            "missing exact full-precision CPU-XF/raster-aware TEV compiler artifact (uid-bytes=" :
            "missing exact full-precision CPU-XF/snapshot-aware TEV compiler artifact (uid-bytes=";
        const auto* bytes=reinterpret_cast<const unsigned char*>(&uid);
        constexpr char hex[]="0123456789abcdef";
        for (size_t i=0;i<sizeof(uid);++i) {
            error+=hex[bytes[i]>>4]; error+=hex[bytes[i]&15];
        }
        error+=')';
        if(manualMask) error+=" manual-mask="+std::to_string(manualMask);
        return invalid;
    }
    auto material=std::make_shared<VitaMaterial>(key,layout,cache->device);
    // Retain recovery ownership before entering the native patcher/allocator.
    cache->entries.emplace(key,material);
    PortVitaGxmProgramInfo info{};
    int result=port_vita_gxm_device_program_create_owned(cache->device,cache,
        vs->data(),uint32_t(vs->size()),fs->data(),uint32_t(fs->size()),&effective,layout.pixel.vectors,layout.pixel.textureMask,&material->program,&info);
    if (result<0) {
        if (!material->program) cache->entries.erase(key);
        return fail(result,"native material creation failed; partial patcher/program ownership stays in the cache");
    }
    if (!reflectedLayout(profile,layout,info))
        return fail(invalid,"compiled material has no reflected exact-scissor/profile resource contract; collect rejected program");
    material->lastUse=++cache->useSequence;
    material->ready=true; out=material.get(); lease=material; return 0;
}
int bindVitaMaterial(const VitaMaterial* material,PortVitaGxmDevice* device,const ShaderUid& uid,
                     const PortVitaGxmBlend& blend,const PixelUniformLayout& layout,
                     PortVitaGxmProgram*& out,std::string& error) {
    return bindVitaMaterialProfile(material,device,uid,blend,VitaFragmentProfile::Raster,{layout},out,error);
}
int bindVitaMaterialProfile(const VitaMaterial* material,PortVitaGxmDevice* device,const ShaderUid& uid,
                     const PortVitaGxmBlend& blend,VitaFragmentProfile profile,const VitaFragmentLayout& layout,
                     PortVitaGxmProgram*& out,std::string& error) {
    out=nullptr; error.clear();
    if(material && material->key.manualTextureMask!=layout.manualTextureMask) {
        error="compiled material has a different manual-sampling material contract"; return invalid;
    }
    if (!material || !material->ready || material->device!=device || !(material->key.uid==uid) ||
        !sameBlend(material->key.blend,blend) || material->key.profile!=profile || !sameLayout(material->layout,layout)) {
        error="typed native material does not match exact draw UID/blend/layout/device"; return invalid;
    }
    PortVitaGxmProgramInfo info{};
    const int result=port_vita_gxm_device_program_info(device,material->program,&info);
    if (result<0 || !reflectedLayout(profile,layout,info)) {
        error="typed native material lost its owned reflected raster/uniform/sampler contract"; return result<0 ? result : invalid;
    }
    out=material->program; return 0;
}
int collectVitaMaterials(VitaMaterialCache* cache) {
    if (!cache || port_vita_gxm_device_scene_active(cache->device)) return invalid;
    size_t idleReady=0;
    bool needsRetirement=false;
    for (const auto& [key,entry]:cache->entries) {
        if (entry.use_count()!=1) continue;
        if (entry->ready) ++idleReady;
        if (cache->closing || !entry->ready) needsRetirement=true;
    }
    if (!needsRetirement && idleReady<=idleMaterialLimit) return 0;
    uint64_t retainFrom=0;
    if (!cache->closing && idleReady>idleMaterialLimit) {
        // Select the newest idle programs with a fixed min-heap; no frame-time allocations.
        std::array<uint64_t,idleMaterialLimit> newest{};
        size_t count=0;
        const std::greater<uint64_t> compare;
        for (const auto& [key,entry]:cache->entries) {
            if (entry.use_count()!=1 || !entry->ready) continue;
            if (count<newest.size()) {
                newest[count++]=entry->lastUse;
                if (count==newest.size()) std::make_heap(newest.begin(),newest.end(),compare);
            } else if (entry->lastUse>newest.front()) {
                std::pop_heap(newest.begin(),newest.end(),compare);
                newest.back()=entry->lastUse;
                std::push_heap(newest.begin(),newest.end(),compare);
            }
        }
        retainFrom=newest.front();
    }
    const int retired=port_vita_gxm_device_retire_resource(cache->device); if (retired<0) return retired;
    for (auto it=cache->entries.begin();it!=cache->entries.end();) {
        auto& entry=it->second;
        if (entry.use_count()!=1) { ++it; continue; }
        if (!cache->closing && entry->ready && entry->lastUse>=retainFrom) { ++it; continue; }
        entry->ready=false;
        if (entry->program) {
            const int result=port_vita_gxm_device_program_release_owned(cache->device,&entry->program,cache);
            if (result<0) return result;
        }
        it=cache->entries.erase(it);
    }
    return 0;
}
int releaseVitaMaterialCache(VitaMaterialCache** pointer) {
    if (!pointer || !*pointer) return invalid;
    auto* cache=*pointer; cache->closing=true;
    const int result=collectVitaMaterials(cache); if (result<0) return result;
    if (!cache->entries.empty()) return invalid;
    port_vita_gxm_device_drop_resource(cache->device); delete cache; *pointer=nullptr; return 0;
}
}
