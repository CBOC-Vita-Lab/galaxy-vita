#include "vita_frame.h"
#include "vita_overlap.h"
#include "../../../vita/runtime/gpu_device_internal.h"

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <fcntl.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace gpu {
namespace {
constexpr int invalid = -1;
bool alphaAlwaysPasses(uint32_t packed) {
    const unsigned a=packed&7,b=(packed>>3)&7;
    switch ((packed>>6)&3) {
    case 0: return a==7 && b==7;
    case 1: return a==7 || b==7;
    case 2: return (a==0 && b==7) || (a==7 && b==0);
    default: return (a==0 && b==0) || (a==7 && b==7);
    }
}
VitaFragmentProfile fragmentProfile(const DrawSpanState& state,SpanTopology topology) {
    const auto& r=state.raster;
    if (topology!=SpanTopology::Triangles || (state.registers.bp[0x43]&7)!=1 || !r.blend ||
        r.equation!=SpanBlendEquation::Add || ((state.uid.fogSel>>1)&7)>=2) return VitaFragmentProfile::Raster;
    if (r.src==SpanBlendFactor::One && r.dst==SpanBlendFactor::One) return VitaFragmentProfile::Rgba6AddSnapshot;
    if (r.src==SpanBlendFactor::SrcAlpha && r.dst==SpanBlendFactor::OneMinusSrcAlpha)
        return (state.registers.bp[0x41]&4) ? VitaFragmentProfile::Rgba6SrcAlphaDitherSnapshot : VitaFragmentProfile::Rgba6SrcAlphaSnapshot;
    if (r.src==SpanBlendFactor::SrcAlpha && r.dst==SpanBlendFactor::One) return VitaFragmentProfile::Rgba6SrcAlphaAddSnapshot;
    return VitaFragmentProfile::Raster;
}

bool drawFragment(const DrawSpanState& state,VitaFragmentProfile profile,VitaFragmentLayout& fragment,
                  std::array<float,108>* pixel,std::string& error) {
    PixelUniformLayout original;
    if(!makePixelUniformLayout(state.uid,original,error)) return false;
    uint32_t mask=0; std::array<std::array<float,4>,8> parameters{};
    for(unsigned unit=0;unit<8;++unit) if(original.textureMask&(1u<<unit)) {
        const auto& binding=state.textures[unit]; PortVitaGxmSampler sampler{};
        if(port_vita_gxm_sampler_decode(binding.mode0,binding.mode1,&sampler)<0) {
            error="invalid immutable GX sampler words"; return false;
        }
        const bool diagonalMip=!sampler.edge_lod && sampler.min_filter==6 && sampler.mag_filter==1;
        const bool mirroredBase=sampler.min_filter==4 && sampler.mag_filter==1 &&
            (sampler.wrap_s==2 || sampler.wrap_t==2);
        if(!diagonalMip && !mirroredBase) continue;
        unsigned width=binding.width,height=binding.height,levels=binding.levels;
        if(binding.kind==SpanTextureKind::EfbAddress) {
            GxRamTextureLayout ram;
            if(describeGxRamTexture(state.registers,unit,ram,error)<0) return false;
            width=ram.width; height=ram.height; levels=ram.levels;
        } else if(binding.kind!=SpanTextureKind::Image) {
            error="manual sampler has no original image or ordered RAM source"; return false;
        }
        if(!diagonalMip && levels!=1) continue;
        unsigned available=1; for(unsigned n=width;n>1;n>>=1) ++available;
        if(!width || width>4096 || width!=height || (width&(width-1)) || !levels || levels>available ||
           sampler.round_lod || sampler.field_predict || sampler.anisotropy!=1 || sampler.bias_clamp ||
           sampler.min_units>sampler.max_units) {
            error="manual sampler needs all original square-POT mips and a supported diagonal linear-filter contract"; return false;
        }
        if(state.pixel.texSize[unit][0]!=sampler.bias || state.pixel.texSize[unit][2]!=1.0f/float(width) ||
           state.pixel.texSize[unit][3]!=1.0f/float(height)) {
            error="manual sampler dimensions/bias disagree with original compact TEV uniforms"; return false;
        }
        mask|=1u<<unit;
        parameters[unit]={float(sampler.min_units),float(sampler.max_units),float(levels-1),float(sampler.wrap_s+4*sampler.wrap_t)};
    }
    if(!describeVitaFragment(state.uid,profile,fragment,error,mask)) return false;
    if(pixel) for(unsigned unit=0;unit<8;++unit) if(mask&(1u<<unit))
        std::copy(parameters[unit].begin(),parameters[unit].end(),pixel->begin()+fragment.manualSlots[unit]*4);
    return true;
}

void provenance(const IndexChunk& c, VitaFrameError& e) {
    e.sourceEventIndex=c.sourceEventIndex; e.drawIndex=c.drawIndex; e.commandWord=c.commandWord;
}

bool nativeRaster(const DrawSpanState& state, VitaPreparedDraw& out, std::string& error) {
    const auto& r=state.raster;
    if (!state.efbWidth || !state.efbHeight || state.efbWidth>4096 || state.efbHeight>4096 ||
        r.colorMask>15 || uint32_t(r.depthFunc)>7 || uint32_t(r.cull)>3 ||
        uint32_t(r.src)>9 || uint32_t(r.dst)>9 || uint32_t(r.equation)>1) {
        error="invalid immutable raster state"; return false;
    }
    for (float value:r.viewport) if (!std::isfinite(value)) {
        error="non-finite immutable viewport"; return false;
    }
    if (r.viewport[2]<0 || r.viewport[3]<0) { error="negative viewport extent"; return false; }
    // GXM discard suppresses depth writes; GX early-Z can write before alpha fails.
    if ((state.registers.bp[0x43]&64) && r.depthTest && r.depthWrite && !alphaAlwaysPasses(state.uid.alphaFunc)) {
        error="native early-Z writes before possible alpha discard need a separate depth strategy"; return false;
    }
    std::memcpy(out.raster.viewport,r.viewport,sizeof(r.viewport));
    std::memcpy(out.raster.scissor,r.scissor,sizeof(r.scissor));
    out.raster.width=state.efbWidth; out.raster.height=state.efbHeight;
    out.raster.depth_test=r.depthTest; out.raster.depth_write=r.depthWrite;
    out.raster.depth_func=uint32_t(r.depthFunc); out.raster.cull=uint32_t(r.cull);
    out.raster.rgba6=(state.registers.bp[0x43]&7)==1;
    out.raster.dither=out.raster.rgba6 && (state.registers.bp[0x41]&4);
    // Unblended destination alpha replaces storage after the original alpha test.
    // Blending still needs the original fragment alpha as its source factor.
    out.raster.alpha_override=out.raster.rgba6 && r.dstAlpha && (r.colorMask&8) &&
        !r.blend && ((state.uid.fogSel>>1)&7)<2;
    out.raster.alpha=0;
    if (out.raster.alpha_override) {
        const unsigned six=(state.registers.bp[0x42]&255)>>2;
        out.raster.alpha=float((six<<2)|(six>>4))/255;
    }
    out.blend.color_mask=((r.colorMask&8)>>3)|((r.colorMask&7)<<1);
    constexpr uint32_t factors[]={0,1,2,3,6,7,4,5,8,9};
    out.blend.color_func=out.blend.alpha_func=r.blend ? (r.equation==SpanBlendEquation::Add ? 1 : 3) : 0;
    out.blend.color_src=out.blend.alpha_src=r.blend ? factors[uint32_t(r.src)] : 1;
    out.blend.color_dst=out.blend.alpha_dst=r.blend ? factors[uint32_t(r.dst)] : 0;
    return true;
}

bool prepareDrawState(const IndexChunk& chunk,VitaPreparedDraw& output,VitaFrameError& error) {
    provenance(chunk,error);
    auto fail=[&](const char* message) { error.message=message; output={}; return false; };
    if (!chunk.state || chunk.remap.size()>kIndexChunkMaxVertices || chunk.indices.size()>UINT32_MAX ||
        chunk.strideWords!=vtxWords(chunk.vtxFlags) || chunk.vtxFlags!=chunk.state->uid.vtxFlags)
        return fail("invalid immutable chunk state or vertex extent");
    switch (chunk.topology) {
    case SpanTopology::Points: output.topology=PORT_VITA_GXM_POINTS; break;
    case SpanTopology::Lines: output.topology=PORT_VITA_GXM_LINES; break;
    case SpanTopology::Triangles: output.topology=PORT_VITA_GXM_TRIANGLES; break;
    default: return fail("unknown immutable chunk topology");
    }
    if (!makePixelUniformLayout(chunk.state->uid,output.layout,error.message) ||
        !packPixelUniforms(output.layout,chunk.state->pixel,output.pixel) ||
        !nativeRaster(*chunk.state,output,error.message)) {
        if (error.message.empty()) error.message="invalid compact pixel uniform layout";
        output={}; return false;
    }
    output.profile=fragmentProfile(*chunk.state,chunk.topology);
    if (!drawFragment(*chunk.state,output.profile,output.fragment,&output.pixel,error.message)) {
        output={}; return false;
    }
    return true;
}

struct ShaderInventoryKey {
    ShaderUid uid{};
    VitaFragmentProfile profile=VitaFragmentProfile::Raster;
    uint32_t mask=0;
    bool operator==(const ShaderInventoryKey& other) const {
        return uid==other.uid && profile==other.profile && mask==other.mask;
    }
};
struct ShaderInventoryHash {
    size_t operator()(const ShaderInventoryKey& key) const {
        return ((ShaderUidHash{}(key.uid)^uint32_t(key.profile))*1099511628211ull)^key.mask;
    }
};

struct ReadyDraw {
    size_t eventIndex=0;
    PortVitaGxmSlice slice{};
    std::array<float,108> pixel{};
    PortVitaGxmRaster raster{};
    uint32_t pixelFloats=0,topology=0;
    VitaDrawBinding binding;
    PortVitaGxmProgram* program=nullptr;
    const void* textures[8]{};
    VitaFragmentProfile profile=VitaFragmentProfile::Raster;
    VitaFragmentLayout fragment;
    struct Group { PortVitaGxmSlice slice{}; std::array<int32_t,4> bounds{}; };
    std::vector<Group> groups;
};
}

bool prepareVitaDraw(const IndexChunks& source, size_t chunkIndex,
                     VitaPreparedDraw& output, VitaFrameError& error) {
    output={}; error={};
    if (!source.frameOwner() || chunkIndex>=source.chunks().size()) {
        error.message="native preparation has no frame or chunk"; return false;
    }
    const auto& chunk=source.chunks()[chunkIndex];
    if (!prepareDrawState(chunk,output,error)) return false;
    auto fail=[&](const char* message) { error.message=message; output={}; return false; };
    output.vertices.resize(chunk.remap.size());
    const auto& words=source.frame().verts;
    for (size_t i=0;i<chunk.remap.size();++i) {
        const uint32_t ordinal=chunk.remap[i]; error.vertexOrdinal=ordinal;
        const uint64_t at=uint64_t(chunk.firstWord)+uint64_t(ordinal)*chunk.strideWords;
        if (ordinal>=chunk.sourceVertexCount || at>words.size() || chunk.strideWords>words.size()-size_t(at))
            return fail("chunk remap outside immutable frame words");
        ClipVertex vertex; const char* reason=nullptr;
        if (!transformVertexCpu(chunk.state->uid,chunk.state->xf,words.data()+size_t(at),chunk.strideWords,vertex,&reason))
            return fail(reason ? reason : "CPU XF failed");
        static_assert(sizeof(ClipVertex)==sizeof(PortVitaGxmVertex));
        static_assert(offsetof(ClipVertex,clip)==offsetof(PortVitaGxmVertex,clip));
        static_assert(offsetof(ClipVertex,color)==offsetof(PortVitaGxmVertex,color));
        static_assert(offsetof(ClipVertex,tex)==offsetof(PortVitaGxmVertex,tex));
        static_assert(offsetof(ClipVertex,viewZ)==offsetof(PortVitaGxmVertex,view_z));
        static_assert(offsetof(ClipVertex,gxZ)==offsetof(PortVitaGxmVertex,gx_z));
        std::memcpy(&output.vertices[i],&vertex,sizeof(vertex));
    }
    error={}; return true;
}

bool inventoryVitaFrameShaders(const IndexChunks& source,const VitaShaderLibrary& library,VitaShaderInventory& out) {
    out={};
    if (!source.frameOwner()) {
        VitaFrameError error; error.message="shader inventory has no original frame owner";
        out.rejections.push_back(std::move(error)); return false;
    }
    constexpr size_t maxChunks=65536,maxRequests=1024,maxRejections=64;
    std::unordered_map<ShaderInventoryKey,size_t,ShaderInventoryHash> seen;
    const size_t reserve=std::min(source.chunks().size(),maxRequests);
    seen.reserve(reserve); out.requests.reserve(reserve);
    auto reject=[&](VitaFrameError error) {
        ++out.rejectedChunks;
        if (out.rejections.size()<maxRejections) out.rejections.push_back(std::move(error));
    };
    for (const auto& chunk:source.chunks()) {
        if (out.inspectedChunks==maxChunks) { out.limitReached=true; break; }
        ++out.inspectedChunks;
        VitaPreparedDraw prepared; VitaFrameError error;
        if (!prepareDrawState(chunk,prepared,error)) { reject(std::move(error)); continue; }
        const ShaderInventoryKey key{chunk.state->uid,prepared.profile,prepared.fragment.manualTextureMask};
        const auto found=seen.find(key);
        if (found!=seen.end()) { ++out.requests[found->second].occurrences; continue; }
        if (out.requests.size()==maxRequests) {
            provenance(chunk,error); error.message="shader inventory unique-request bound reached";
            reject(std::move(error)); out.limitReached=true; break;
        }
        const auto vertex=genVitaCpuVertexShader(key.uid),fragment=genVitaFragmentShader(key.uid,key.profile,key.mask);
        if (vertex.empty() || fragment.empty()) {
            provenance(chunk,error); error.message="original shader source generation rejected its material contract";
            reject(std::move(error)); continue;
        }
        seen.emplace(key,out.requests.size());
        out.requests.push_back({key.uid,key.profile,key.mask,prepared.fragment.pixel.vectors,1,chunk.commandWord,
            !findVitaShader(library,VitaShaderStage::Vertex,vertex),!findVitaShader(library,VitaShaderStage::Fragment,fragment)});
    }
    out.complete=!out.rejectedChunks && !out.limitReached;
    return out.complete;
}

bool writeVitaShaderInventory(FILE* file,uint64_t frame,const char* tag,const VitaShaderInventory& inventory) {
    if (!file || !tag || std::strlen(tag)!=4 || !std::all_of(tag,tag+4,[](char c) { return c>='0' && c<='9'; }) ||
        inventory.requests.size()>1024 || inventory.rejections.size()>64 || inventory.inspectedChunks>65536)
        return false;
    size_t occurrences=0;
    for (const auto& request:inventory.requests) {
        VitaFragmentLayout layout; std::string error;
        if (!request.occurrences || !describeVitaFragment(request.uid,request.profile,layout,error,request.manualTextureMask) ||
            request.pixelVectors!=layout.pixel.vectors) return false;
        if (request.occurrences>inventory.inspectedChunks-occurrences) return false;
        occurrences+=request.occurrences;
    }
    if (occurrences>inventory.inspectedChunks || inventory.rejectedChunks>inventory.inspectedChunks-occurrences ||
        (inventory.complete && (inventory.limitReached || inventory.rejectedChunks || occurrences!=inventory.inspectedChunks)))
        return false;
    auto write=[&](const char* record,int length,size_t capacity) {
        return length>0 && size_t(length)<capacity && std::fwrite(record,1,size_t(length),file)==size_t(length);
    };
    char record[1536];
    int length=std::snprintf(record,sizeof(record),
        "{\"type\":\"begin\",\"tag\":\"%s\",\"frame\":%llu,\"chunks\":%llu,\"requests\":%llu,\"rejected\":%llu,\"details\":%llu,\"complete\":%s,\"limit\":%s}\n",
        tag,static_cast<unsigned long long>(frame),static_cast<unsigned long long>(inventory.inspectedChunks),
        static_cast<unsigned long long>(inventory.requests.size()),static_cast<unsigned long long>(inventory.rejectedChunks),
        static_cast<unsigned long long>(inventory.rejections.size()),inventory.complete ? "true" : "false",inventory.limitReached ? "true" : "false");
    if (!write(record,length,sizeof(record))) return false;
    constexpr char hex[]="0123456789abcdef";
    static_assert(sizeof(ShaderUid)==400);
    for (size_t i=0;i<inventory.requests.size();++i) {
        const auto& request=inventory.requests[i]; char uid[801];
        const auto* bytes=reinterpret_cast<const unsigned char*>(&request.uid);
        for (size_t j=0;j<sizeof(request.uid);++j) { uid[2*j]=hex[bytes[j]>>4]; uid[2*j+1]=hex[bytes[j]&15]; }
        uid[800]='\0';
        length=std::snprintf(record,sizeof(record),
            "{\"type\":\"request\",\"tag\":\"%s\",\"index\":%llu,\"uid\":\"%s\",\"profile\":%u,\"manual-mask\":%u,\"vectors\":%u,\"occurrences\":%u,\"command\":%llu,\"missing-v\":%s,\"missing-f\":%s}\n",
            tag,static_cast<unsigned long long>(i),uid,unsigned(request.profile),unsigned(request.manualTextureMask),
            unsigned(request.pixelVectors),unsigned(request.occurrences),static_cast<unsigned long long>(request.firstCommandWord),
            request.vertexMissing ? "true" : "false",request.fragmentMissing ? "true" : "false");
        if (!write(record,length,sizeof(record))) return false;
    }
    for (size_t i=0;i<inventory.rejections.size();++i) {
        const auto& error=inventory.rejections[i]; char reason[513];
        const size_t bytes=std::min(error.message.size(),size_t(256));
        for (size_t j=0;j<bytes;++j) { const auto value=static_cast<unsigned char>(error.message[j]); reason[2*j]=hex[value>>4]; reason[2*j+1]=hex[value&15]; }
        reason[bytes*2]='\0';
        length=std::snprintf(record,sizeof(record),
            "{\"type\":\"rejection\",\"tag\":\"%s\",\"index\":%llu,\"command\":%llu,\"event\":%llu,\"draw\":%llu,\"reason-hex\":\"%s\",\"reason-truncated\":%s}\n",
            tag,static_cast<unsigned long long>(i),static_cast<unsigned long long>(error.commandWord),
            static_cast<unsigned long long>(error.sourceEventIndex),static_cast<unsigned long long>(error.drawIndex),
            reason,error.message.size()>bytes ? "true" : "false");
        if (!write(record,length,sizeof(record))) return false;
    }
    length=std::snprintf(record,sizeof(record),"{\"type\":\"end\",\"tag\":\"%s\",\"requests\":%llu,\"details\":%llu,\"complete\":%s}\n",
        tag,static_cast<unsigned long long>(inventory.requests.size()),static_cast<unsigned long long>(inventory.rejections.size()),
        inventory.complete ? "true" : "false");
    return write(record,length,sizeof(record)) && std::fflush(file)==0 && !std::ferror(file);
}

bool configureVitaShaderCapture(FILE* file,VitaShaderCapture& capture) {
    capture={};
    if (!file) return false;
    char tag[6]{};
    const size_t bytes=std::fread(tag,1,sizeof(tag),file);
    if (bytes!=5 || std::ferror(file) || tag[4]!='\n' ||
        !std::all_of(tag,tag+4,[](char c) { return c>='0' && c<='9'; })) return false;
    std::copy(tag,tag+4,capture.tag); return true;
}

int captureVitaShaderInventory(const char* directory,uint64_t frame,const VitaShaderInventory& inventory,VitaShaderCapture& capture) {
    if (!capture.tag[0] || capture.attempted) return 0;
    capture.attempted=true;
    if (!directory || capture.tag[4]!='\0' ||
        !std::all_of(capture.tag,capture.tag+4,[](char c) { return c>='0' && c<='9'; })) return invalid;
    char path[128];
    const int length=std::snprintf(path,sizeof(path),"%s/shader-inventory-%s.jsonl",directory,capture.tag);
    if (length<=0 || size_t(length)>=sizeof(path)) return invalid;
    const int fd=::open(path,O_WRONLY|O_CREAT|O_EXCL,0600);
    if (fd<0) return invalid;
    bool saved=false;
    FILE* file=::fdopen(fd,"wb");
    if (file) {
#if defined(__cpp_exceptions)
        try {
#endif
        saved=writeVitaShaderInventory(file,frame,capture.tag,inventory);
#if defined(__cpp_exceptions)
        } catch (...) { std::fclose(file); ::unlink(path); throw; }
#endif
        if (std::fclose(file)!=0) saved=false;
    } else ::close(fd);
    if (!saved) ::unlink(path);
    return saved ? 1 : invalid;
}

static int resolveDraw(PortVitaGxmDevice* device,VitaMaterialCache* materials,VitaTextureCache* textures,
                    const VitaShaderLibrary& library,const IndexChunks& source,const IndexChunk& chunk,
                    const VitaPreparedDraw& prepared,const VitaRamTextures* ram,VitaDrawBinding& out,std::string& error) {
    error.clear();
    auto fail=[&](int code,const char* message) { error=message; return code; };
    if (out.material || out.lifetime || !source.frameOwner() || !chunk.state ||
        !port_vita_gxm_device_context(device) || port_vita_gxm_device_scene_active(device))
        return fail(invalid,"draw resolver requires an empty binding and owned scene-closed immutable state");
    for (const auto* view:out.views) if (view)
        return fail(invalid,"draw resolver cannot replace a retained sampler binding");
    for (const auto& proof:out.ramSources) if (proof)
        return fail(invalid,"draw resolver cannot replace retained RAM provenance");
    const auto& state=*chunk.state;
    PixelUniformLayout layout; VitaPreparedDraw canonical; VitaFragmentLayout fragment;
    if (!makePixelUniformLayout(state.uid,layout,error) || !nativeRaster(state,canonical,error)) return invalid;
    const auto profile=fragmentProfile(state,chunk.topology);
    if (!drawFragment(state,profile,fragment,nullptr,error)) return invalid;
    if (layout.vectors!=prepared.layout.vectors || layout.textureMask!=prepared.layout.textureMask ||
        layout.slots!=prepared.layout.slots || std::memcmp(&canonical.blend,&prepared.blend,sizeof(prepared.blend)) ||
        profile!=prepared.profile || fragment.pixel.vectors!=prepared.fragment.pixel.vectors ||
        fragment.pixel.textureMask!=prepared.fragment.pixel.textureMask || fragment.pixel.slots!=prepared.fragment.pixel.slots ||
        fragment.snapshotSampler!=prepared.fragment.snapshotSampler || fragment.snapshotSlot!=prepared.fragment.snapshotSlot ||
        fragment.manualTextureMask!=prepared.fragment.manualTextureMask || fragment.manualSlots!=prepared.fragment.manualSlots)
        return fail(invalid,"prepared draw layout/blend does not match its immutable GX state");
    VitaDrawBinding candidate;
    auto leases=std::make_shared<std::array<std::shared_ptr<const void>,9>>();
    int result=resolveVitaMaterialProfile(materials,state.uid,prepared.blend,profile,library,candidate.material,(*leases)[0],error,fragment.manualTextureMask);
    if (result<0) return result;
    PortVitaGxmProgram* program=nullptr;
    result=bindVitaMaterialProfile(candidate.material,device,state.uid,prepared.blend,profile,fragment,program,error);
    if (result<0) return result;
    for (unsigned t=0;t<8;++t) if (layout.textureMask&(1u<<t)) {
        if (ram && state.textures[t].kind==SpanTextureKind::EfbAddress)
            result=resolveVitaRamImage(textures,source.frame().number,state.textures[t],state.registers,t,*ram,
                candidate.views[t],(*leases)[t+1],candidate.ramSources[t],error,(fragment.manualTextureMask&(1u<<t))!=0);
        else result=resolveVitaImage(textures,source.frame(),state.textures[t],candidate.views[t],(*leases)[t+1],error,
            (fragment.manualTextureMask&(1u<<t))!=0);
        if (result<0) return result;
        const void* descriptor=nullptr;
        result=port_vita_gxm_texture_view_descriptor(candidate.views[t],device,&descriptor);
        if (result<0) return fail(result,"resolved GX sampler does not belong to the draw device");
    }
    candidate.lifetime=std::move(leases); out=std::move(candidate); return 0;
}

int resolveVitaDraw(PortVitaGxmDevice* device,VitaMaterialCache* materials,VitaTextureCache* textures,
                    const VitaShaderLibrary& library,const IndexChunks& source,const IndexChunk& chunk,
                    const VitaPreparedDraw& prepared,VitaDrawBinding& out,std::string& error) {
    return resolveDraw(device,materials,textures,library,source,chunk,prepared,nullptr,out,error);
}

int resolveVitaDrawWithRam(PortVitaGxmDevice* device,VitaMaterialCache* materials,VitaTextureCache* textures,
                    const VitaShaderLibrary& library,const IndexChunks& source,const IndexChunk& chunk,
                    const VitaPreparedDraw& prepared,const VitaRamTextures& ram,VitaDrawBinding& out,std::string& error) {
    return resolveDraw(device,materials,textures,library,source,chunk,prepared,&ram,out,error);
}

struct VitaFrameReplay {
    PortVitaGxmDevice* device=nullptr;
    PortVitaGxmStaging* staging=nullptr;
    std::shared_ptr<const IndexChunks> source;
    std::vector<ReadyDraw> ready;
    std::shared_ptr<const void> surfaceLease;
    std::shared_ptr<const VitaEfbBlendSnapshot> snapshotLease;
    VitaEfbBlendSnapshotView snapshotView;
    PortVitaGxmSlice snapshotSlice{};
    bool open=false,closing=false;
};

int createVitaFrameReplay(PortVitaGxmDevice* device, VitaFrameReplay** output) {
    if (!output || *output) return invalid;
    auto* replay=new(std::nothrow) VitaFrameReplay;
    if (!replay) return invalid;
    const int result=port_vita_gxm_staging_create(device,0,&replay->staging);
    if (result<0) { delete replay; return result; }
    replay->device=device; *output=replay; return 0;
}

int discardVitaFrame(VitaFrameReplay* replay) {
    if (!replay || replay->closing) return invalid;
    if (replay->open) {
        const int result=port_vita_gxm_staging_end(replay->staging);
        if (result<0) return result;
        replay->open=false;
    }
    const int result=port_vita_gxm_staging_reset(replay->staging);
    if (result<0) return result;
    replay->ready.clear(); replay->snapshotLease.reset(); replay->snapshotView={}; replay->snapshotSlice={};
    replay->surfaceLease.reset(); replay->source.reset();
    return 0;
}

int releaseVitaFrameReplay(VitaFrameReplay** pointer) {
    if (!pointer || !*pointer) return invalid;
    auto* replay=*pointer;
    if (!replay->closing) {
        const int result=discardVitaFrame(replay);
        if (result<0) return result;
        replay->closing=true;
    }
    const int result=port_vita_gxm_staging_release(&replay->staging);
    if (result<0) return result;
    delete replay; *pointer=nullptr; return 0;
}

bool submitVitaFrame(VitaFrameReplay* replay, std::shared_ptr<const IndexChunks> source,
                     VitaFrameBackend& backend, VitaFrameError& error) {
    error={};
    auto fail=[&](int code,const char* fallback) {
        error.nativeCode=code;
        if (error.message.empty()) error.message=fallback;
        return false;
    };
    if (!replay || replay->closing || replay->source || !source || !source->frameOwner())
        return fail(invalid,"native replay is invalid or retains a previous frame");
    replay->source=std::move(source);
    const auto& chunks=*replay->source;
    const auto& events=chunks.events();
    const auto* spans=chunks.sourceSpans().get();
    size_t cursor=0;
    auto eventError=[&](const IndexChunkEvent& event) {
        error={}; error.sourceEventIndex=event.sourceEventIndex; error.commandWord=event.commandWord;
        if (event.kind==SpanEventKind::Draw && event.index<chunks.chunks().size())
            error.drawIndex=chunks.chunks()[event.index].drawIndex;
    };
    // Every copy is an ordering/resource-history boundary. Resolve resources
    // only after preceding copies, but upload this batch before BeginScene.
    while (cursor<events.size()) {
        size_t end=cursor;
        while (end<events.size() && events[end].kind!=SpanEventKind::Copy) ++end;
        PortVitaGxmSurface* surface=nullptr;
        uint32_t width=0,height=0;
        for (size_t ei=cursor;ei<end;++ei) {
            const auto& event=events[ei]; eventError(event);
            if (event.kind!=SpanEventKind::Draw) continue;
            VitaPreparedDraw draw;
            if (!prepareVitaDraw(chunks,event.index,draw,error)) return false;
            eventError(event);
            const auto& chunk=chunks.chunks()[event.index]; const auto& state=*chunk.state;
            if (chunk.indices.empty()) continue; // retained event, zero geometry
            // Do not quietly reinterpret contracts the simple GXM pass cannot
            // reproduce. These require real native strategies before release.
            // RGB8 has no reduced-precision color quantization to dither.
            const unsigned format=state.registers.bp[0x43]&7;
            const bool needsDither=(state.registers.bp[0x41]&4) && format!=0 && format!=1;
            // Opaque, no-fog RGBA6 stores exact six-bit components in the
            // fragment shader. Blended/fogged paths need their own ordering.
            const bool needsRgba6=format==1 && (state.raster.blend || ((state.uid.fogSel>>1)&7)>=2);
            if ((!state.raster.blend && (state.registers.bp[0x41]&2)) || needsDither ||
                (needsRgba6 && draw.profile==VitaFragmentProfile::Raster)) {
                char registers[128];
                std::snprintf(registers,sizeof(registers)," (bp41=%06x bp42=%06x bp43=%06x bpf1=%06x)",
                    state.registers.bp[0x41],state.registers.bp[0x42],state.registers.bp[0x43],state.registers.bp[0xf1]);
                error.message=std::string("native logic/RGBA6 blend-fog/other-format dither strategy is not implemented")+registers;
                return fail(invalid,"native raster strategy is not implemented");
            }
            if (width && (width!=state.efbWidth || height!=state.efbHeight))
                return fail(invalid,"EFB dimensions changed without an ordered copy boundary");
            width=state.efbWidth; height=state.efbHeight;
            replay->ready.emplace_back(); auto& ready=replay->ready.back(); ready.eventIndex=ei;
            const int resolved=backend.resolve(chunks,chunk,draw,ready.binding,error.message);
            if (resolved<0) return fail(resolved,"native material or texture resolution failed");
            if (!ready.binding.material || !ready.binding.lifetime)
                return fail(invalid,"native binding has no exact typed material or resource lifetime lease");
            const int material=bindVitaMaterialProfile(ready.binding.material,replay->device,state.uid,draw.blend,draw.profile,draw.fragment,
                                               ready.program,error.message);
            if (material<0) return fail(material,"native material does not match exact draw shader/blend/layout");
            for (unsigned t=0;t<8;++t) if (draw.layout.textureMask&(1u<<t)) {
                const auto& expected=state.textures[t]; PortVitaGxmTextureViewInfo info{};
                if (!ready.binding.views[t] || port_vita_gxm_texture_view_info(ready.binding.views[t],&info)<0)
                    return fail(invalid,"native binding is missing a UID-used immutable GX sampler view");
                if (bool(info.explicit_linear_mip)!=bool(draw.fragment.manualTextureMask&(1u<<t)))
                    return fail(invalid,"explicit point-fetch mip view needs a matching manual-sampling material contract");
                if (info.sampler.mode0!=expected.mode0 || info.sampler.mode1!=expected.mode1 ||
                    info.sampler.bias!=state.pixel.texSize[t][0])
                    return fail(invalid,"native GX sampler does not match the immutable draw");
                if (expected.kind==SpanTextureKind::Image) {
                    if (ready.binding.ramSources[t] || expected.imageIndex>=chunks.frame().textures.size() ||
                        !chunks.frame().textures[expected.imageIndex])
                        return fail(invalid,"native sampler has no immutable Frame image identity");
                    const auto& image=*chunks.frame().textures[expected.imageIndex];
                    if (info.source!=image.rgba.data() || image.version!=expected.version || image.width!=expected.width ||
                        image.height!=expected.height || image.levels!=expected.levels || info.image.version!=expected.version ||
                        info.image.width!=expected.width || info.image.height!=expected.height || info.image.levels!=expected.levels)
                        return fail(invalid,"native sampler substituted a different immutable image");
                } else if (expected.kind==SpanTextureKind::EfbAddress) {
                    const auto& ram=ready.binding.ramSources[t]; GxRamTextureLayout layout;
                    if (!ram || describeGxRamTexture(state.registers,t,layout,error.message)<0)
                        return fail(invalid,"native EFB-address texture needs typed ordered RAM provenance");
                    const auto& origin=ram->origin();
                    if (ram->layout()!=layout || layout.address!=expected.address || ram->frame()!=chunks.frame().number ||
                        !origin || origin->command().words[0]!=expected.copyId || origin->address()!=expected.address ||
                        origin->frame()>chunks.frame().number || origin->sequence()>ram->copySequence() ||
                        info.source!=ram->rgba() || info.image.width!=layout.width || info.image.height!=layout.height ||
                        info.image.levels!=layout.levels)
                        return fail(invalid,"native EFB sampler substituted a different draw-time RAM snapshot");
                } else return fail(invalid,"native used texture has no immutable image or RAM origin");
                const int descriptor=port_vita_gxm_texture_view_descriptor(ready.binding.views[t],replay->device,&ready.textures[t]);
                if (descriptor<0) return fail(descriptor,"native GX sampler does not belong to the replay device");
            }
            std::vector<uint16_t> padded;
            std::vector<VitaOverlapGroup> groups;
            if (draw.profile!=VitaFragmentProfile::Raster) {
                if (ready.binding.views[draw.fragment.snapshotSampler] || ready.binding.ramSources[draw.fragment.snapshotSampler])
                    return fail(invalid,"snapshot unit substituted an original GX sampler binding");
                if (!partitionVitaTriangles(draw,chunk.indices,groups,error.message) || groups.empty())
                    return fail(invalid,"snapshot draw cannot preserve its whole-triangle ordered ranges");
                if (uint64_t(chunk.indices.size())+groups.size()>UINT32_MAX)
                    return fail(invalid,"aligned snapshot index stream exceeds native extent");
                padded.reserve(chunk.indices.size()+groups.size());
                for(auto& group:groups) {
                    if (padded.size()&1) padded.push_back(chunk.indices[0]);
                    const auto start=group.firstIndex;
                    group.firstIndex=uint32_t(padded.size());
                    padded.insert(padded.end(),chunk.indices.begin()+start,chunk.indices.begin()+start+group.indexCount);
                }
            }
            const auto& indices=padded.empty() ? chunk.indices : padded;
            const int uploaded=port_vita_gxm_staging_upload(replay->staging,draw.vertices.data(),uint32_t(draw.vertices.size()),
                indices.data(),uint32_t(indices.size()),&ready.slice);
            if (uploaded<0) return fail(uploaded,"native mapped draw upload failed");
            for(const auto& group:groups) {
                PortVitaGxmSlice slice{};
                const int result=port_vita_gxm_staging_slice(replay->staging,&ready.slice,group.firstIndex,group.indexCount,&slice);
                if (result<0) return fail(result,"native ordered index range has no aligned staging ownership");
                ready.groups.push_back({slice,group.bounds});
            }
            ready.profile=draw.profile; ready.fragment=draw.fragment;
            ready.pixel=draw.pixel; ready.pixelFloats=draw.fragment.pixel.vectors*4;
            ready.raster=draw.raster; ready.topology=draw.topology;
        }
        if (!replay->ready.empty()) {
            const int result=backend.surface(width,height,surface,replay->surfaceLease,error.message);
            if (result<0) return fail(result,"native EFB surface resolution failed");
            PortVitaGxmSurfaceInfo info{};
            if (!surface || !replay->surfaceLease || port_vita_gxm_surface_info(surface,&info)<0 || info.width!=width || info.height!=height)
                return fail(invalid,"native EFB surface/lifetime does not match full immutable dimensions");
        }
        if (std::any_of(replay->ready.begin(),replay->ready.end(),[](const auto& draw){return !draw.groups.empty();})) {
            int result=backend.snapshot(surface,width,height,replay->snapshotLease,error.message);
            if (result<0) return fail(result,"native ordered blend snapshot resolution failed");
            result=bindVitaEfbBlendSnapshot(replay->snapshotLease.get(),replay->device,surface,width,height,replay->snapshotView,error.message);
            if (result<0) return fail(result,"native ordered blend snapshot ownership does not match frame");
            PortVitaGxmVertex vertices[3]{};
            const float clip[3][4]={{-1,-1,0,1},{3,-1,0,1},{-1,3,0,1}};
            for(unsigned i=0;i<3;++i) std::copy(clip[i],clip[i]+4,vertices[i].clip);
            const uint16_t indices[]={0,1,2};
            result=port_vita_gxm_staging_upload(replay->staging,vertices,3,indices,3,&replay->snapshotSlice);
            if (result<0) return fail(result,"native snapshot copy geometry upload failed");
            for(auto& ready:replay->ready) if (!ready.groups.empty()) {
                const auto& state=*chunks.chunks()[events[ready.eventIndex].index].state;
                const auto& sample=replay->snapshotView.targetSample;
                if (!packVitaFragmentUniforms(state.uid,ready.profile,state.pixel,sample.color_width,sample.color_height,
                    state.raster.dstAlpha && (state.raster.colorMask&8),state.registers.bp[0x42]&255,ready.pixel,error.message,
                    ready.fragment.manualTextureMask))
                    return fail(invalid,"native snapshot uniforms do not match sampled padded extent");
                VitaFragmentLayout canonical;
                if(!drawFragment(state,ready.profile,canonical,&ready.pixel,error.message))
                    return fail(invalid,"native manual sampler uniforms lost their immutable original state");
                ready.textures[ready.fragment.snapshotSampler]=sample.color_texture;
            }
        }
        size_t drawIndex=0;
        for (size_t ei=cursor;ei<end;++ei) {
            const auto& event=events[ei]; eventError(event);
            int result=0;
            switch (event.kind) {
            case SpanEventKind::Draw: {
                const auto& chunk=chunks.chunks()[event.index];
                if (chunk.indices.empty()) break;
                if (drawIndex>=replay->ready.size() || replay->ready[drawIndex].eventIndex!=ei)
                    return fail(invalid,"native prepared batch lost command order");
                auto& ready=replay->ready[drawIndex++];
                if (!ready.groups.empty()) {
                    for(const auto& group:ready.groups) {
                        if (replay->open) {
                            result=port_vita_gxm_staging_end(replay->staging);
                            if (result<0) return fail(result,"ordered blend EFB EndScene failed; resources retained");
                            replay->open=false;
                        }
                        result=port_vita_gxm_staging_begin(replay->staging,replay->snapshotView.target);
                        if (result<0) return fail(result,"ordered blend snapshot BeginScene failed");
                        replay->open=true;
                        PortVitaGxmRaster copy{}; copy.width=width; copy.height=height;
                        copy.viewport[2]=float(width); copy.viewport[3]=float(height);
                        auto bounds=group.bounds;
                        if(bounds[0]>=bounds[2] || bounds[1]>=bounds[3]) bounds={0,0,int32_t(width),int32_t(height)};
                        std::copy(bounds.begin(),bounds.end(),copy.scissor);
                        float raster[8]{}; const auto& sample=replay->snapshotView.sourceSample;
                        const float pixel[20]={0,0,float(sample.color_width),float(sample.color_height)};
                        const void* textures[8]={sample.color_texture};
                        result=port_vita_gxm_raster_apply(port_vita_gxm_device_context(replay->device),&copy,PORT_VITA_GXM_TRIANGLES,raster);
                        if (result>=0) result=port_vita_gxm_staging_draw(replay->staging,replay->snapshotView.program,
                            &replay->snapshotSlice,PORT_VITA_GXM_TRIANGLES,pixel,20,textures,raster);
                        if (result<0) return fail(result,"ordered destination snapshot draw failed; resources retained");
                        result=port_vita_gxm_staging_end(replay->staging);
                        if (result<0) return fail(result,"ordered destination snapshot EndScene failed; resources retained");
                        replay->open=false;
                        result=port_vita_gxm_staging_begin(replay->staging,surface);
                        if (result<0) return fail(result,"ordered blend EFB BeginScene failed");
                        replay->open=true;
                        result=port_vita_gxm_raster_apply(port_vita_gxm_device_context(replay->device),&ready.raster,ready.topology,raster);
                        if (result>=0) result=port_vita_gxm_staging_draw(replay->staging,ready.program,&group.slice,ready.topology,
                            ready.pixel.data(),ready.pixelFloats,ready.textures,raster);
                        if (result<0) return fail(result,"ordered original GX blend draw failed; resources retained");
                    }
                    break;
                }
                if (!replay->open) {
                    result=port_vita_gxm_staging_begin(replay->staging,surface);
                    if (result<0) return fail(result,"native EFB BeginScene failed");
                    replay->open=true;
                }
                float raster[8]{};
                result=port_vita_gxm_raster_apply(port_vita_gxm_device_context(replay->device),&ready.raster,ready.topology,raster);
                if (result>=0) result=port_vita_gxm_staging_draw(replay->staging,ready.program,&ready.slice,ready.topology,
                    ready.pixel.data(),ready.pixelFloats,ready.textures,raster);
                break;
            }
            case SpanEventKind::Marker:
                if (!spans || event.index>=spans->markers().size()) return fail(invalid,"invalid immutable marker event");
                result=backend.marker(chunks.frame(),spans->markers()[event.index],error.message); break;
            case SpanEventKind::Camera:
                if (!spans || event.index>=spans->cameras().size()) return fail(invalid,"invalid immutable camera event");
                result=backend.camera(chunks.frame(),spans->cameras()[event.index],error.message); break;
            default: return fail(invalid,"unknown immutable frame event");
            }
            if (result<0) return fail(result,"native frame event failed");
        }
        if (replay->open) {
            const int result=port_vita_gxm_staging_end(replay->staging);
            if (result<0) return fail(result,"native EFB EndScene failed");
            replay->open=false;
        }
        const int retired=port_vita_gxm_staging_reset(replay->staging);
        if (retired<0) return fail(retired,"native frame batch fence failed");
        replay->ready.clear(); replay->snapshotLease.reset(); replay->snapshotView={}; replay->snapshotSlice={};
        replay->surfaceLease.reset();
        if (end<events.size()) {
            const auto& event=events[end]; eventError(event);
            if (!spans || event.index>=spans->copies().size()) return fail(invalid,"invalid immutable EFB copy event");
            const int result=backend.copy(chunks.frame(),spans->copies()[event.index],error.message);
            if (result<0) return fail(result,"native EFB copy failed");
        }
        cursor=end<events.size() ? end+1 : end;
    }
    // Also fence an empty/metadata-only frame; no scanout is invented here.
    const int result=discardVitaFrame(replay);
    if (result<0) return fail(result,"native frame retirement failed");
    error={}; return true;
}

} // namespace gpu
