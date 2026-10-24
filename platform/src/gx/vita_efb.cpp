#include "vita_efb.h"
#include "../../../vita/runtime/gpu_device_internal.h"
#include "../../../vita/runtime/gpu_staging.h"
#include "../../../vita/runtime/gpu_raster.h"
#include <new>
#include <algorithm>
#include <cstring>
#include <cstddef>
#include <cstdlib>
#include <cstdio>

namespace gpu {
namespace {
constexpr int invalid=-1;
constexpr int noMemory=-2;
struct ShaderBytes {
    std::unique_ptr<unsigned char[]> data;
    uint32_t size=0;
    bool copy(const std::vector<unsigned char>& source) noexcept {
        data.reset(new(std::nothrow) unsigned char[source.size()]);
        if (!data) return false;
        size=uint32_t(source.size()); std::memcpy(data.get(),source.data(),source.size()); return true;
    }
};
struct Target {
    PortVitaGxmSurface* surface=nullptr;
    uint32_t width=0,height=0;
    bool ready=false,reusable=false;
    bool captured=false,captureDepth=false;
    uint32_t captureLeft=0,captureTop=0,sourceWidth=0,sourceHeight=0;
    std::shared_ptr<Target> next;
};
struct TargetStorage { alignas(std::max_align_t) unsigned char bytes[256]; };
template<class T> struct TargetAllocator {
    using value_type=T;
    TargetStorage* storage;
    explicit TargetAllocator(TargetStorage* p) noexcept : storage(p) {}
    template<class U> TargetAllocator(const TargetAllocator<U>& other) noexcept : storage(other.storage) {}
    T* allocate(size_t count) noexcept {
        // Both pinned standard libraries allocate one scalar control block.
        static_assert(sizeof(T)<=sizeof(TargetStorage) && alignof(T)<=alignof(TargetStorage));
        if (count!=1) std::abort();
        return reinterpret_cast<T*>(storage->bytes);
    }
    void deallocate(T*,size_t) noexcept { delete storage; }
    template<class U> bool operator==(const TargetAllocator<U>& other) const noexcept { return storage==other.storage; }
};
ShaderUid clearVertexUid() { ShaderUid uid{}; uid.numTevStages=1; uid.alphaFunc=63; return uid; }
uint32_t expand(uint32_t value,unsigned bits) {
    const unsigned shift=8-bits; const uint32_t high=value>>shift;
    return (high<<shift)|(high>>(bits-shift));
}
uint32_t compressedDepth(uint32_t z,unsigned mode) {
    if (!mode) return z>>8;
    const unsigned exponentBits=mode+1,mantissaBits=16-exponentBits;
    const unsigned limit=mode==3 ? 12 : (1u<<exponentBits)-1;
    unsigned exponent=0;
    while (exponent<limit && (z&(1u<<(23-exponent)))) ++exponent;
    const unsigned shift=24-exponent-mantissaBits-unsigned(exponent<limit);
    return (exponent<<mantissaBits)|((z>>shift)&((1u<<mantissaBits)-1));
}
}
struct VitaEfbCache {
    PortVitaGxmDevice* device=nullptr;
    PortVitaGxmStaging* staging=nullptr;
    PortVitaGxmProgram* programs[4]{};
    PortVitaGxmProgram* capturePrograms[2]{};
    bool programReady[4]{},open=false,closing=false;
    bool captureReady[2]{};
    ShaderBytes vertex,fragment;
    ShaderBytes captureFragment[2];
    std::shared_ptr<Target> targets;
    std::shared_ptr<Target> active;
    std::shared_ptr<Target> activeSource;
    std::shared_ptr<Target> blendScratch;
    std::weak_ptr<const VitaEfbBlendSnapshot> issuedBlend;
    unsigned traceCaptures=0;
};
struct VitaEfbBlendSnapshot {
    VitaEfbBlendSnapshot(VitaEfbCache* cache,std::shared_ptr<Target> source,std::shared_ptr<Target> target,
                         const VitaEfbBlendSnapshotView& view):cache(cache),source(std::move(source)),target(std::move(target)),view(view) {}
    VitaEfbCache* const cache;
    const std::shared_ptr<Target> source,target;
    const VitaEfbBlendSnapshotView view;
};

namespace {
void traceNativeCapture(VitaEfbCache* cache,const char* phase,PortVitaGxmSurface* surface) {
#if defined(__vita__)
    if (!cache || !cache->traceCaptures || cache->traceCaptures>4) return;
    PortVitaGxmSurfaceInfo info{};
    const int result=port_vita_gxm_surface_info(surface,&info);
    char line[256];
    const int size=std::snprintf(line,sizeof(line),
        "native-efb capture=%u phase=%s surface=%p info=%d color=%p extent=%ux%u stride=%u\n",
        cache->traceCaptures,phase,static_cast<void*>(surface),result,info.color_data,info.width,info.height,info.stride);
    if (size>0 && size<int(sizeof(line))) { std::fwrite(line,1,size,stderr); std::fflush(stderr); }
#endif
}
}

std::string genVitaEfbClearFragmentShader() {
    return R"(struct ClearOutput { float4 color : COLOR0; float depth : DEPTH; };
uniform float4 u_pixel[4];
uniform float4 u_raster[2];
ClearOutput main(float4 p : WPOS) {
    if (p.x<u_raster[0].x || p.y<u_raster[0].y || p.x>=u_raster[0].z || p.y>=u_raster[0].w) discard;
    ClearOutput o;
    o.color=u_pixel[0];
    if (u_raster[1].x>0.5) o.color.a=u_raster[1].y;
    o.depth=u_pixel[1].x;
    return o;
}
)";
}
std::string genVitaEfbSnapshotFragmentShader(bool depth) {
    std::string source=R"(uniform float4 u_pixel[5];
uniform float4 u_raster[2];
uniform sampler2D s_tex0 : TEXUNIT0;
float4 main(float4 p : WPOS) : COLOR0 {
    if (p.x<u_raster[0].x || p.y<u_raster[0].y || p.x>=u_raster[0].z || p.y>=u_raster[0].w) discard;
    float4 sampled=tex2D(s_tex0,(u_pixel[0].xy+p.xy)/u_pixel[0].zw);
)";
    if (depth) source+=R"(    int z=int(round(saturate(sampled.r)*16777215.0));
    float4 color=float4(float(z>>16),float((z>>8)&255),float(z&255),255.0)/255.0;
)";
    else source+="    float4 color=sampled;\n";
    source+=R"(    if (u_raster[1].x>0.5) color.a=u_raster[1].y;
    return color;
}
)";
    return source;
}
int createVitaEfbCache(PortVitaGxmDevice* device,const VitaShaderLibrary& library,VitaEfbCache** out,std::string& error) {
    error.clear();
    if (!out || *out) { error="EFB cache output is not empty"; return invalid; }
    const auto* vs=findVitaShader(library,VitaShaderStage::Vertex,genVitaCpuVertexShader(clearVertexUid()));
    const auto* fs=findVitaShader(library,VitaShaderStage::Fragment,genVitaEfbClearFragmentShader());
    const std::vector<unsigned char>* captured[2]={
        findVitaShader(library,VitaShaderStage::Fragment,genVitaEfbSnapshotFragmentShader(false)),
        findVitaShader(library,VitaShaderStage::Fragment,genVitaEfbSnapshotFragmentShader(true))};
    if (!vs || !fs) { error="missing exact full-precision native EFB clear compiler artifacts"; return invalid; }
    int result=port_vita_gxm_device_hold_resource(device); if (result<0) return result;
    auto* cache=new(std::nothrow) VitaEfbCache;
    if (!cache || !cache->vertex.copy(*vs) || !cache->fragment.copy(*fs) ||
        (captured[0] && !cache->captureFragment[0].copy(*captured[0])) ||
        (captured[1] && !cache->captureFragment[1].copy(*captured[1]))) {
        delete cache; port_vita_gxm_device_drop_resource(device); error="EFB CPU OOM"; return noMemory;
    }
    cache->device=device;
    result=port_vita_gxm_staging_create(device,4096,&cache->staging);
    if (result<0) { port_vita_gxm_device_drop_resource(device); delete cache; return result; }
    *out=cache; return 0;
}
static int createTarget(VitaEfbCache* cache,uint32_t width,uint32_t height,bool reusable,
                         std::shared_ptr<Target>& target,std::string& error) {
    auto* storage=new(std::nothrow) TargetStorage;
    if (!storage) { error="EFB CPU OOM"; return noMemory; }
    target=std::allocate_shared<Target>(TargetAllocator<Target>{storage});
    target->width=width; target->height=height; target->reusable=reusable;
    target->next=cache->targets; cache->targets=target;
    const int result=port_vita_gxm_surface_create_owned(cache->device,width,height,cache,&target->surface);
    if (result<0) {
        if (!target->surface) cache->targets=std::move(target->next);
        error="native EFB creation failed; partial rollback remains cache-owned"; return result;
    }
    target->ready=true; return 0;
}
int resolveVitaEfb(VitaEfbCache* cache,uint32_t width,uint32_t height,PortVitaGxmSurface*& out,
                   std::shared_ptr<const void>& lease,std::string& error) {
    error.clear();
    if (out || lease || !cache || cache->closing || cache->active || !width || !height || width>1024 || height>1024 ||
        port_vita_gxm_device_scene_active(cache->device) || !port_vita_gxm_device_context(cache->device)) {
        error="EFB resolution requires an empty binding and owned scene-closed full dimensions"; return invalid;
    }
    for (auto* entry=&cache->targets;*entry;entry=&(*entry)->next) if ((*entry)->reusable && (*entry)->width==width && (*entry)->height==height) {
        if (!(*entry)->ready) { error="EFB has retained native rollback ownership; collect before retry"; return invalid; }
        out=(*entry)->surface; lease=*entry; return 0;
    }
    std::shared_ptr<Target> target;
    const int result=createTarget(cache,width,height,true,target,error); if (result<0) return result;
    out=target->surface; lease=target; return 0;
}
int bindVitaEfbBlendSnapshot(const VitaEfbBlendSnapshot* binding,PortVitaGxmDevice* device,PortVitaGxmSurface* source,
                            uint32_t width,uint32_t height,VitaEfbBlendSnapshotView& out,std::string& error) {
    out={}; error.clear();
    if (!binding || !binding->cache || binding->cache->closing || binding->cache->device!=device ||
        port_vita_gxm_device_scene_active(device) || !port_vita_gxm_device_context(device) ||
        !binding->source->ready || !binding->source->reusable || binding->source->surface!=source ||
        binding->source->width!=width || binding->source->height!=height || !binding->target->ready ||
        binding->target->reusable || binding->target->captured || binding->target->width!=width ||
        binding->target->height!=height || binding->view.target!=binding->target->surface ||
        binding->view.program!=binding->cache->capturePrograms[0] || !binding->cache->captureReady[0]) {
        error="blend snapshot does not match its owned source/target/program/device extent"; return invalid;
    }
    const auto& view=binding->view;
    PortVitaGxmSurfaceInfo a{},b{}; PortVitaGxmProgramInfo program{};
    if (view.target==source || port_vita_gxm_surface_info(source,&a)<0 || port_vita_gxm_surface_info(view.target,&b)<0 ||
        !a.color_data || !b.color_data || a.color_data==b.color_data || a.width!=width || a.height!=height ||
        b.width!=width || b.height!=height || port_vita_gxm_device_program_info(device,view.program,&program)<0 ||
        program.pixel_floats<4 || program.pixel_floats>20 || program.raster_floats!=8 || program.texture_mask!=1) {
        error="blend snapshot has aliased storage or lost its exact reflected copy contract"; return invalid;
    }
    for(const auto* sample:{&view.sourceSample,&view.targetSample})
        if (sample->width!=width || sample->height!=height || sample->color_width<width || sample->color_height<height ||
            sample->color_width>4096 || sample->color_height>4096 || !sample->color_texture) {
            error="blend snapshot has no validated actual padded color-sampling extent"; return invalid;
        }
    out=view; return 0;
}
int resolveVitaEfbBlendSnapshot(VitaEfbCache* cache,PortVitaGxmSurface* surface,
                               std::shared_ptr<const VitaEfbBlendSnapshot>& out,std::string& error) {
    error.clear();
    auto fail=[&](int code,const char* text) { error=text; return code; };
    if (!cache || cache->closing || cache->active || out || !surface ||
        port_vita_gxm_device_scene_active(cache->device) || !port_vita_gxm_device_context(cache->device))
        return fail(invalid,"blend snapshot resolution requires empty outputs and scene-closed owned EFB");
    std::shared_ptr<Target> source;
    for(auto entry=cache->targets;entry;entry=entry->next)
        if(entry->ready && entry->reusable && entry->surface==surface) { source=entry; break; }
    if (!source || !cache->captureFragment[0].data)
        return fail(invalid,"blend snapshot needs an owned mutable EFB and exact color-copy artifact");
    if (auto issued=cache->issuedBlend.lock()) {
        if (issued->source!=source) return fail(invalid,"another source retains the ordered blend scratch lease");
        VitaEfbBlendSnapshotView checked;
        const int result=bindVitaEfbBlendSnapshot(issued.get(),cache->device,surface,source->width,source->height,checked,error);
        if (result<0) return result;
        out=std::move(issued); return 0;
    }
    auto*& program=cache->capturePrograms[0];
    if (!cache->captureReady[0]) {
        if (program) return fail(invalid,"blend copy program retains native rollback ownership; close before retry");
        const PortVitaGxmBlend blend{15,0,0,1,0,1,0}; PortVitaGxmProgramInfo info{};
        int result=port_vita_gxm_device_program_create_owned(cache->device,cache,
            cache->vertex.data.get(),cache->vertex.size,cache->captureFragment[0].data.get(),cache->captureFragment[0].size,
            &blend,5,1,&program,&info);
        if (result<0) return fail(result,"blend copy creation failed; native rollback remains cache-owned");
        if (info.pixel_floats<4 || info.pixel_floats>20 || info.raster_floats!=8 || info.texture_mask!=1)
            return fail(invalid,"blend copy lost its exact pixel/raster/sampler reflection");
        cache->captureReady[0]=true;
    }
    auto target=cache->blendScratch;
    if (target && !target->ready) return fail(invalid,"blend scratch retains native rollback ownership; collect before retry");
    if (!target || target->width!=source->width || target->height!=source->height) {
        target.reset();
        const int result=createTarget(cache,source->width,source->height,false,target,error);
        if (target && target->surface) cache->blendScratch=target;
        if (result<0) return result;
    }
    VitaEfbBlendSnapshotView view; view.target=target->surface; view.program=program;
    int result=port_vita_gxm_surface_sample(cache->device,surface,&view.sourceSample);
    if (result>=0) result=port_vita_gxm_surface_sample(cache->device,target->surface,&view.targetSample);
    if (result<0) return fail(result,"blend snapshot descriptor creation failed before producer work");
    auto* storage=new(std::nothrow) TargetStorage;
    if (!storage) return fail(noMemory,"blend snapshot CPU lease OOM");
    auto lease=std::allocate_shared<VitaEfbBlendSnapshot>(TargetAllocator<VitaEfbBlendSnapshot>{storage},cache,source,target,view);
    VitaEfbBlendSnapshotView checked;
    result=bindVitaEfbBlendSnapshot(lease.get(),cache->device,surface,source->width,source->height,checked,error);
    if (result<0) return result;
    cache->issuedBlend=lease; out=std::move(lease); return 0;
}
int peekVitaEfb(VitaEfbCache* cache,PortVitaGxmSurface* surface,const RecorderPeek& request,
                  uint32_t& out,std::string& error) {
    error.clear();
    const unsigned format=request.pixelFormat&7,compression=(request.pixelFormat>>3)&7;
    if (request.pixelFormat>0x7F || format>3 || compression>3 || (!request.depth && request.alphaRead>2)) {
        error="unsupported original GX EFB access pixel/alpha/compression mode"; return invalid;
    }
    PortVitaGxmSurface* captured=nullptr; std::shared_ptr<const void> lease;
    int result=captureVitaEfb(cache,surface,request.x,request.y,1,1,request.depth,captured,lease,error);
    if (result<0) return result;
    PortVitaGxmSurfaceInfo info{};
    result=port_vita_gxm_surface_info(captured,&info); if (result<0) return result;
    if (!info.color_data || info.width!=1 || info.height!=1 || !info.stride) {
        error="native EFB access capture has no completed one-pixel color storage"; return invalid;
    }
    traceNativeCapture(cache,"peek-read",captured);
    const auto* p=static_cast<const unsigned char*>(info.color_data);
    uint32_t value=0;
    if (request.depth) {
        value=(uint32_t(p[0])<<16)|(uint32_t(p[1])<<8)|p[2];
        if (format==2) value=compressedDepth(value,compression);
    } else {
        uint32_t r=p[0],g=p[1],b=p[2],a=255;
        if (format==1) { r=expand(r,6); g=expand(g,6); b=expand(b,6); a=expand(p[3],6); }
        else if (format==2) { r=expand(r,5); g=expand(g,6); b=expand(b,5); }
        if (request.alphaRead<2) a=request.alphaRead ? 255 : 0;
        value=(a<<24)|(r<<16)|(g<<8)|b;
    }
    lease.reset();
    result=collectVitaEfbs(cache);
    if (result<0) { error="native EFB access capture retirement failed"; return result; }
    out=value; return 0;
}
bool vitaEfbSceneOpen(const VitaEfbCache* cache) { return cache && cache->open; }
int discardVitaEfbClear(VitaEfbCache* cache) {
    if (!cache) return invalid;
    if (cache->open) {
        const int result=port_vita_gxm_staging_end(cache->staging); if (result<0) return result;
        cache->open=false;
    }
    const int result=port_vita_gxm_staging_reset(cache->staging); if (result<0) return result;
    cache->active.reset(); cache->activeSource.reset(); return 0;
}
int clearVitaEfb(VitaEfbCache* cache,PortVitaGxmSurface* surface,const VitaEfbClear& request,std::string& error) {
    error.clear();
    auto fail=[&](int code,const char* text) { error=text; return code; };
    if (!cache || cache->closing || cache->active || port_vita_gxm_device_scene_active(cache->device) ||
        !port_vita_gxm_device_context(cache->device) || !surface || request.colorWrite>1 || request.alphaWrite>1 ||
        request.depthWrite>1 || request.pixelFormat>0x7F || (request.pixelFormat&7)>3 ||
        ((request.pixelFormat>>3)&7)>3 || request.ar>0xFFFF || request.gb>0xFFFF || request.z>0xFFFFFF)
        return fail(invalid,"invalid or retained native EFB clear state");
    std::shared_ptr<Target> target;
    for (auto* entry=&cache->targets;*entry;entry=&(*entry)->next)
        if ((*entry)->ready && (*entry)->reusable && (*entry)->surface==surface) { target=*entry; break; }
    if (!target || !request.width || !request.height || request.left>target->width || request.top>target->height ||
        request.width>target->width-request.left || request.height>target->height-request.top)
        return fail(invalid,"clear rectangle is outside its owned full-size EFB");
    const unsigned format=request.pixelFormat&7;
    if (format==2 && request.depthWrite)
        return fail(invalid,"native GX Z16/MSAA/compression clear strategy is not implemented");
    const bool rgb=request.colorWrite && format!=3;
    const bool alpha=request.alphaWrite && format==1;
    if (!rgb && !alpha && !request.depthWrite) return 0;
    const unsigned key=unsigned(rgb)*2+unsigned(alpha);
    auto*& program=cache->programs[key];
    if (!cache->programReady[key]) {
        if (program) return fail(invalid,"clear program has retained native rollback ownership; close cache to retry");
        PortVitaGxmBlend blend{unsigned(rgb)*14u+unsigned(alpha),0,0,1,0,1,0};
        PortVitaGxmProgramInfo info{};
        const int result=port_vita_gxm_device_program_create_owned(cache->device,cache,
            cache->vertex.data.get(),cache->vertex.size,cache->fragment.data.get(),cache->fragment.size,
            &blend,4,0,&program,&info);
        if (result<0) return fail(result,"native clear program creation failed; recovery owner retained");
        if (info.raster_floats!=8 || info.texture_mask) return fail(invalid,"clear shader lost exact raster/no-sampler reflection");
        cache->programReady[key]=true;
    }
    PortVitaGxmVertex vertices[3]{};
    const float clip[3][4]={{-1,-1,0,1},{3,-1,0,1},{-1,3,0,1}};
    for (unsigned i=0;i<3;++i) std::copy(clip[i],clip[i]+4,vertices[i].clip);
    const uint16_t indices[]={0,1,2}; PortVitaGxmSlice slice{};
    int result=port_vita_gxm_staging_upload(cache->staging,vertices,3,indices,3,&slice);
    if (result<0) return fail(result,"native clear upload failed; staging recovery owner retained");
    cache->active=target;
    result=port_vita_gxm_staging_begin(cache->staging,surface);
    if (result<0) return fail(result,"EFB clear BeginScene failed; discard before retry");
    cache->open=true;
    PortVitaGxmRaster raster{}; raster.width=target->width; raster.height=target->height;
    raster.viewport[2]=float(target->width); raster.viewport[3]=float(target->height);
    raster.scissor[0]=int32_t(request.left); raster.scissor[1]=int32_t(request.top);
    raster.scissor[2]=int32_t(request.left+request.width); raster.scissor[3]=int32_t(request.top+request.height);
    raster.depth_test=1; raster.depth_func=7; raster.depth_write=request.depthWrite;
    float rasterUniform[8]{};
    result=port_vita_gxm_raster_apply(port_vita_gxm_device_context(cache->device),&raster,PORT_VITA_GXM_TRIANGLES,rasterUniform);
    if (result<0) return fail(result,"native clear raster setup failed; retained scene requires discard");
    uint32_t color[]={request.ar&255,request.gb>>8,request.gb&255,request.ar>>8};
    if (format==1) for (auto& component:color) component=expand(component,6);
    else if (format==2) {
        color[0]=expand(color[0],5); color[1]=expand(color[1],6); color[2]=expand(color[2],5); color[3]=255;
    } else color[3]=255;
    float pixel[16]{};
    for (unsigned i=0;i<4;++i) pixel[i]=float(color[i])/255;
    pixel[4]=float(request.z)/16777215.0f;
    result=port_vita_gxm_staging_draw(cache->staging,program,&slice,PORT_VITA_GXM_TRIANGLES,pixel,16,nullptr,rasterUniform);
    if (result<0) return fail(result,"native EFB clear draw failed; discard fences without replay");
    result=discardVitaEfbClear(cache);
    return result<0 ? fail(result,"native clear End/Finish failed; target/staging ownership retained") : 0;
}
int clearVitaEfbAfterCopy(VitaEfbCache* cache,PortVitaGxmSurface* surface,const SpanCopy& copy,std::string& error) {
    error.clear();
    if (!(copy.words[9]&(1u<<11))) return 0;
    bool matching=false;
    if (cache) for (auto* entry=&cache->targets;*entry;entry=&(*entry)->next)
        if ((*entry)->ready && (*entry)->reusable && (*entry)->surface==surface && (*entry)->width==copy.efbWidth && (*entry)->height==copy.efbHeight) {
            matching=true; break;
        }
    if (!matching) { error="copy clear requires the recorded pre-copy full-size EFB"; return invalid; }
    const VitaEfbClear request{copy.words[1]&1023,(copy.words[1]>>10)&1023,
        (copy.words[2]&1023)+1,((copy.words[2]>>10)&1023)+1,copy.words[6],copy.words[7],copy.words[8],
        (copy.colorMode>>3)&1,(copy.colorMode>>4)&1,(copy.depthMode>>4)&1,copy.pixelFormat};
    return clearVitaEfb(cache,surface,request,error);
}
int captureVitaEfb(VitaEfbCache* cache,PortVitaGxmSurface* surface,uint32_t left,uint32_t top,
                    uint32_t width,uint32_t height,bool depth,PortVitaGxmSurface*& out,
                    std::shared_ptr<const void>& lease,std::string& error) {
    error.clear();
    auto fail=[&](int code,const char* text) { error=text; return code; };
    if (!cache || cache->closing || cache->active || out || lease || !surface || !width || !height ||
        port_vita_gxm_device_scene_active(cache->device) || !port_vita_gxm_device_context(cache->device))
        return fail(invalid,"capture requires empty outputs and an owned scene-closed EFB");
    std::shared_ptr<Target> source;
    for (auto* entry=&cache->targets;*entry;entry=&(*entry)->next)
        if ((*entry)->ready && (*entry)->reusable && (*entry)->surface==surface) { source=*entry; break; }
    if (!source || left>source->width || top>source->height || width>source->width-left || height>source->height-top)
        return fail(invalid,"capture rectangle is outside its owned full-size EFB");
    // Bound development checkpoints to the first four captures, including peeks.
    if (cache->traceCaptures<=4) ++cache->traceCaptures;
    traceNativeCapture(cache,depth ? "begin-depth" : "begin-color",surface);
    const unsigned key=unsigned(depth);
    if (!cache->captureFragment[key].data)
        return fail(invalid,"missing exact full-precision native EFB plane-capture compiler artifact");
    PortVitaGxmSurfaceSampleInfo sample{};
    int result=port_vita_gxm_surface_sample(cache->device,surface,&sample);
    if (result<0) return fail(result,"native source-plane sampling descriptor failed");
    auto*& program=cache->capturePrograms[key];
    if (!cache->captureReady[key]) {
        if (program) return fail(invalid,"capture program has retained native rollback ownership; close cache to retry");
        const PortVitaGxmBlend blend{15,0,0,1,0,1,0}; PortVitaGxmProgramInfo info{};
        result=port_vita_gxm_device_program_create_owned(cache->device,cache,
            cache->vertex.data.get(),cache->vertex.size,cache->captureFragment[key].data.get(),cache->captureFragment[key].size,
            &blend,5,1,&program,&info);
        if (result<0) return fail(result,"native capture program creation failed; recovery owner retained");
        if (!info.pixel_floats || info.raster_floats!=8 || info.texture_mask!=1)
            return fail(invalid,"capture shader lost exact pixel/raster/sampler reflection");
        cache->captureReady[key]=true;
    }
    std::shared_ptr<Target> target;
    result=createTarget(cache,width,height,false,target,error); if (result<0) return result;
    PortVitaGxmVertex vertices[3]{};
    const float clip[3][4]={{-1,-1,0,1},{3,-1,0,1},{-1,3,0,1}};
    for (unsigned i=0;i<3;++i) std::copy(clip[i],clip[i]+4,vertices[i].clip);
    const uint16_t indices[]={0,1,2}; PortVitaGxmSlice slice{};
    result=port_vita_gxm_staging_upload(cache->staging,vertices,3,indices,3,&slice);
    if (result<0) return fail(result,"native capture upload failed; staging recovery owner retained");
    cache->active=target; cache->activeSource=source;
    result=port_vita_gxm_staging_begin(cache->staging,target->surface);
    if (result<0) return fail(result,"EFB capture BeginScene failed; discard before retry");
    cache->open=true;
    PortVitaGxmRaster raster{}; raster.width=width; raster.height=height;
    raster.viewport[2]=float(width); raster.viewport[3]=float(height);
    raster.scissor[2]=int32_t(width); raster.scissor[3]=int32_t(height);
    float rasterUniform[8]{};
    result=port_vita_gxm_raster_apply(port_vita_gxm_device_context(cache->device),&raster,PORT_VITA_GXM_TRIANGLES,rasterUniform);
    if (result<0) return fail(result,"native capture raster failed; retained scene requires discard");
    float pixel[20]{float(left),float(top),float(depth ? sample.depth_width : sample.color_width),
                    float(depth ? sample.depth_height : sample.color_height)};
    const void* textures[8]{depth ? sample.depth_texture : sample.color_texture};
    result=port_vita_gxm_staging_draw(cache->staging,program,&slice,PORT_VITA_GXM_TRIANGLES,pixel,20,textures,rasterUniform);
    if (result<0) return fail(result,"native EFB capture draw failed; discard fences without replay");
    result=discardVitaEfbClear(cache);
    if (result<0) return fail(result,"native capture End/Finish failed; source/target/staging ownership retained");
    traceNativeCapture(cache,"gpu-finished",target->surface);
    target->captured=true; target->captureDepth=depth;
    target->captureLeft=left; target->captureTop=top;
    target->sourceWidth=source->width; target->sourceHeight=source->height;
    out=target->surface; lease=target; return 0;
}
int processVitaEfbCapture(VitaEfbCache* cache,PortVitaGxmSurface* surface,const SpanCopy& copy,
                           GxCopyPixels& out,std::string& error) {
    error.clear();
    auto fail=[&](int code,const char* text) { error=text; return code; };
    if (!cache || cache->closing || cache->active || !surface || out.texels().data ||
        port_vita_gxm_device_scene_active(cache->device) || !port_vita_gxm_device_context(cache->device))
        return fail(invalid,"processing requires an open cache, empty output and scene-closed immutable capture");
    std::shared_ptr<Target> target;
    for (auto* entry=&cache->targets;*entry;entry=&(*entry)->next)
        if ((*entry)->ready && !(*entry)->reusable && (*entry)->captured && (*entry)->surface==surface) { target=*entry; break; }
    const bool depth=(copy.pixelFormat&7)==3 && !(copy.words[9]&(1u<<14));
    if (!target || target->sourceWidth!=copy.efbWidth || target->sourceHeight!=copy.efbHeight ||
        target->captureDepth!=depth)
        return fail(invalid,"capture plane/original EFB provenance does not match copy command");
    const int finished=port_vita_gxm_device_finish(cache->device);
    if (finished<0) return fail(finished,"native capture CPU read requires completed GPU work");
    PortVitaGxmSurfaceInfo info{};
    const int described=port_vita_gxm_surface_info(surface,&info); if (described<0) return described;
    const GxCopyPlane plane{static_cast<const unsigned char*>(info.color_data),int32_t(target->captureLeft),
        int32_t(target->captureTop),info.width,info.height,info.stride*4,size_t(info.stride)*info.height*4,target->captureDepth};
    traceNativeCapture(cache,"copy-read",surface);
    const int produced=produceGxCopyPixels(copy,plane,out,error);
    if (produced>=0) traceNativeCapture(cache,"copy-produced",surface);
    return produced;
}
int copyVitaEfbToMemory(VitaEfbCache* cache,GxCopyHistory* history,PortVitaGxmSurface* surface,uint64_t frame,
                         const SpanCopy& copy,const GxCopyMemory& memory,std::shared_ptr<const GxCopyBytes>& out,
                         std::string& error) {
    error.clear();
    auto fail=[&](int code,const char* text) { error=text; return code; };
    if (!cache || cache->closing || cache->active || !history || !surface || out ||
        port_vita_gxm_device_scene_active(cache->device) || !port_vita_gxm_device_context(cache->device))
        return fail(invalid,"native copy requires scene-closed owners and an empty commit marker");
    std::shared_ptr<Target> source;
    for (auto* entry=&cache->targets;*entry;entry=&(*entry)->next)
        if ((*entry)->ready && (*entry)->reusable && (*entry)->surface==surface) { source=*entry; break; }
    if (!source || source->width!=copy.efbWidth || source->height!=copy.efbHeight)
        return fail(invalid,"native copy source does not match the recorded full-size EFB");
    GxCopyGeometry geometry;
    const bool xfb=(copy.words[9]&(1u<<14))!=0;
    int result=xfb ? describeGxXfbCopy(copy,geometry,error) : describeGxTextureCopy(copy,geometry,error);
    if (result<0) return result;
    if (!xfb && (copy.words[9]&0x3400))
        return fail(invalid,"native field/vertical-scaled copy strategy is not installed");
    if ((copy.words[9]&(1u<<11)) && (copy.pixelFormat&7)==2 && (copy.depthMode&(1u<<4)))
        return fail(invalid,"native GX Z16/MSAA/compression clear strategy is not implemented");
    result=collectVitaEfbs(cache); if (result<0) return fail(result,"native copy retained target retirement failed");
    PortVitaGxmSurface* captured=nullptr; std::shared_ptr<const void> captureLease;
    // Full source is a correctness-first transport for padding/filter neighbors.
    // Missing out-of-EFB halo samples still reject; no invented edge values.
    result=captureVitaEfb(cache,surface,0,0,source->width,source->height,!xfb && (copy.pixelFormat&7)==3,
        captured,captureLease,error);
    if (result<0) return result;
    GxCopyPixels processed;
    result=processVitaEfbCapture(cache,captured,copy,processed,error); if (result<0) return result;
    captureLease.reset(); captured=nullptr;
    result=collectVitaEfbs(cache); if (result<0) return fail(result,"processed copy raw-capture retirement failed");
    traceNativeCapture(cache,"copy-retired",surface);
    result=storeGxCopyBytes(history,frame,copy,processed.texels(),memory,out,error); if (result<0) return result;
    traceNativeCapture(cache,"copy-committed",surface);
    // RAM/history is now irreversible. Preserve out on clear failure so the
    // caller can stop/recover rather than pretend rollback or replay the copy.
    result=clearVitaEfbAfterCopy(cache,surface,copy,error);
    if (result>=0) traceNativeCapture(cache,"copy-complete",surface);
    return result;
}
int collectVitaEfbs(VitaEfbCache* cache) {
    if (!cache || cache->active || port_vita_gxm_device_scene_active(cache->device)) return invalid;
    if (cache->blendScratch && !cache->blendScratch->ready) cache->blendScratch.reset();
    const int retired=port_vita_gxm_device_retire_resource(cache->device); if (retired<0) return retired;
    for (auto* entry=&cache->targets;*entry;) {
        if (entry->use_count()!=1) { entry=&(*entry)->next; continue; }
        (*entry)->ready=false;
        if ((*entry)->surface) {
            const int result=port_vita_gxm_surface_release_owned(&(*entry)->surface,cache); if (result<0) return result;
        }
        auto next=std::move((*entry)->next); *entry=std::move(next);
    }
    return 0;
}
int releaseVitaEfbCache(VitaEfbCache** pointer) {
    if (!pointer || !*pointer) return invalid;
    auto* cache=*pointer;
    if (!cache->closing) {
        const int discarded=discardVitaEfbClear(cache); if (discarded<0) return discarded;
        cache->closing=true;
        cache->blendScratch.reset();
    }
    const int collected=collectVitaEfbs(cache); if (collected<0) return collected;
    if (cache->targets) return invalid;
    for (auto*& program:cache->programs) if (program) {
        const int result=port_vita_gxm_device_program_release_owned(cache->device,&program,cache); if (result<0) return result;
    }
    for (auto*& program:cache->capturePrograms) if (program) {
        const int result=port_vita_gxm_device_program_release_owned(cache->device,&program,cache); if (result<0) return result;
    }
    const int result=port_vita_gxm_staging_release(&cache->staging); if (result<0) return result;
    port_vita_gxm_device_drop_resource(cache->device); delete cache; *pointer=nullptr; return 0;
}
}
