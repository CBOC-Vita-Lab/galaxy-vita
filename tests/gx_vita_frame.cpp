// Real frame/XF/TEV/device/program/staging/raster; only SDK drivers are doubled.
#define PORT_VITA_SUBMISSION_DRIVER_ONLY
#include "vita_gpu_submission.cpp"
#include "vita_frame.h"
#include <span>
#if __has_include("vita_overlap.h")
#include "vita_overlap.h"
#else
namespace gpu {
struct VitaOverlapGroup { uint32_t firstIndex=0,indexCount=0; std::array<int32_t,4> bounds{}; };
bool partitionVitaTriangles(const VitaPreparedDraw&,std::span<const uint16_t>,
    std::vector<VitaOverlapGroup>&,std::string&) __attribute__((weak));
}
#endif
namespace gpu {
bool packVitaFragmentUniforms(const ShaderUid&,VitaFragmentProfile,const PixelState&,uint32_t,uint32_t,bool,uint32_t,
    std::array<float,108>&,std::string&,uint32_t) __attribute__((weak));
bool inventoryVitaFrameShaders(const IndexChunks&,const VitaShaderLibrary&,VitaShaderInventory&) __attribute__((weak));
bool configureVitaShaderCapture(FILE*,VitaShaderCapture&) __attribute__((weak));
int captureVitaShaderInventory(const char*,uint64_t,const VitaShaderInventory&,VitaShaderCapture&) __attribute__((weak));
bool writeVitaShaderInventory(FILE*,uint64_t,const char*,const VitaShaderInventory&) __attribute__((weak));
}
#include "vita_texture_cache.h"
#include "vita_material_cache.h"
#ifndef GPU_VITA_FRAGMENT_PROFILES
namespace gpu {
enum class VitaFragmentProfile : uint32_t { Raster, Rgba6SrcAlphaSnapshot };
struct VitaFragmentLayout { PixelUniformLayout pixel; uint32_t snapshotSampler=8,snapshotSlot=27; };
bool describeVitaFragment(const ShaderUid&,VitaFragmentProfile,VitaFragmentLayout&,std::string&) __attribute__((weak));
std::string genVitaFragmentShader(const ShaderUid&,VitaFragmentProfile) __attribute__((weak));
int resolveVitaMaterialProfile(VitaMaterialCache*,const ShaderUid&,const PortVitaGxmBlend&,VitaFragmentProfile,
    const VitaShaderLibrary&,VitaMaterial*&,std::shared_ptr<const void>&,std::string&) __attribute__((weak));
int bindVitaMaterialProfile(const VitaMaterial*,PortVitaGxmDevice*,const ShaderUid&,const PortVitaGxmBlend&,
    VitaFragmentProfile,const VitaFragmentLayout&,PortVitaGxmProgram*&,std::string&) __attribute__((weak));
}
#endif
#include "vita_efb.h"
#ifndef GPU_VITA_EFB_BLEND_SNAPSHOT
namespace gpu {
struct VitaEfbBlendSnapshot;
struct VitaEfbBlendSnapshotView {
    PortVitaGxmSurface* target=nullptr;
    PortVitaGxmProgram* program=nullptr;
    PortVitaGxmSurfaceSampleInfo sourceSample{},targetSample{};
};
int resolveVitaEfbBlendSnapshot(VitaEfbCache*,PortVitaGxmSurface*,std::shared_ptr<const VitaEfbBlendSnapshot>&,
    std::string&) __attribute__((weak));
int bindVitaEfbBlendSnapshot(const VitaEfbBlendSnapshot*,PortVitaGxmDevice*,PortVitaGxmSurface*,uint32_t,uint32_t,
    VitaEfbBlendSnapshotView&,std::string&) __attribute__((weak));
}
#endif
#include "vita_xfb.h"
#include "gx_copy_bytes.h"
#include "tex_decode.h"
#include "gpu_texture_internal.h"
#include "gpu_device_internal.h"
#include "../vita/bringup/renderer.h"
#include <cmath>
#include <algorithm>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <cstdarg>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include "gpu.h"
#if __has_include("gx_live.h")
#include "gx_live.h"
#else
namespace gpu {
struct RecorderMemory { const unsigned char* tlut=nullptr; size_t tlutBytes=0; };
struct RecorderPeek { bool depth=false; uint16_t x=0,y=0; uint32_t pixelFormat=0,alphaRead=1; };
class RecorderConsumer {
public:
    virtual ~RecorderConsumer() = default;
    virtual bool consume(std::shared_ptr<const Frame>,const RecorderMemory&,std::string&) = 0;
    virtual bool peek(const RecorderPeek&,uint32_t&,std::string&) = 0;
    virtual bool present(const void*,bool,std::string&) = 0;
};
}
#endif
#if __has_include("vita_game_worker.h")
#include "vita_game_worker.h"
#else
namespace gpu {
class VitaGameWorker : public RecorderConsumer {
public:
    bool close(std::string&) __attribute__((weak));
};
bool createVitaGameWorker(VitaShaderLibrary,std::shared_ptr<VitaGameWorker>&,std::string&) __attribute__((weak));
}
#endif
namespace gpu {
bool attachRecorderConsumer(std::shared_ptr<RecorderConsumer>,std::string&) __attribute__((weak));
bool writeVitaXfbSnapshot(FILE*,const unsigned char*,uint32_t,uint32_t,size_t) __attribute__((weak));
}
static thread_local int captureIoFailure;
extern "C" FILE* __real_fdopen(int,const char*);
extern "C" int __real_fclose(FILE*);
extern "C" FILE* __wrap_fdopen(int fd,const char* mode) {
    if (captureIoFailure==1) { errno=ENOMEM; return nullptr; }
    if (captureIoFailure==2) { ::close(fd); return std::fopen("/dev/full",mode); }
    return __real_fdopen(fd,mode);
}
extern "C" int __wrap_fclose(FILE* file) {
    const int result=__real_fclose(file);
    if (captureIoFailure==3) { errno=EIO; return EOF; }
    return result;
}
extern "C" void port_gx_recorder_present_xfb(const void*,int) __attribute__((weak));
extern "C" void port_gx_recorder_end_frame();
extern "C" void port_gx_marker(uint32_t);
extern "C" void port_gx_camera(const float*);
extern "C" int port_vita_game_entry() __attribute__((weak));
static bool entryInputFailure,entryBootObserved;
extern "C" int sceIoMkdir(const char*,int) { return 0; }
extern "C" void port_log_file(const char*) {}
extern "C" int port_vita_input_init() { return !entryInputFailure; }
extern "C" void port_vita_input_shutdown() {}
extern "C" void port_vita_input_poll(float,PortPadState*) { std::exit(95); }
extern "C" void port_input_set(int,const PortPadState*) {}
extern "C" int64_t port_host_time_ns() { return 1; }
extern "C" int port_vgx_active() { return 0; }
void testViFlush();
void configureTestVi(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,void*,uint32_t=1,uint32_t=22,uint32_t=0,uint32_t=0);
void setTestViPresenter(void (*)(void*,int));
void retraceTestVi();
extern "C" void port_boot(const char* data,const char* saves) {
    require(std::strcmp(data,"ux0:data/codex-galaxy/game")==0 &&
        std::strcmp(saves,"ux0:data/codex-galaxy/saves")==0,
        "actual native entry passes original cooked data and isolated saves to game boot");
    struct Probe final : gpu::RecorderConsumer {
        bool consume(std::shared_ptr<const gpu::Frame>,const gpu::RecorderMemory&,std::string&) override { return false; }
        bool peek(const gpu::RecorderPeek&,uint32_t&,std::string&) override { return false; }
        bool present(const void*,bool,std::string&) override { return false; }
    };
    std::string error;
    require(!gpu::attachRecorderConsumer(std::make_shared<Probe>(),error),
        "real native worker is attached before the real boot call can initialize GX");
    entryBootObserved=true;
}

// Only guest address/IRQ/allocator transports are doubled. The actual GX parser,
// recorder, immutable spans, native replay/capture and RAM writer remain real.
static unsigned char liveRam[4096];
static unsigned char liveRam2[4096];
static thread_local bool ownsGuestCpu;
static std::vector<int> liveIrqs;
static thread_local unsigned hostScopeDepth;
extern "C" {
volatile uint32_t __PortGXPERegs[0x400];
void port_log(const char*,...) {}
__attribute__((noreturn)) void port_fatal(const char* text,...) {
    std::fputs("live GX fatal: ",stderr); va_list args; va_start(args,text);
    std::vfprintf(stderr,text,args); va_end(args); std::fputc('\n',stderr); std::exit(94);
}
void* port_resolve_gpu_address(uint32_t address) { return address<sizeof(liveRam) ? liveRam+address : nullptr; }
int port_is_native_range(const void* p,size_t bytes) {
    const uintptr_t start=reinterpret_cast<uintptr_t>(liveRam), value=reinterpret_cast<uintptr_t>(p);
    return value>=start && value-start<=sizeof(liveRam) && bytes<=sizeof(liveRam)-(value-start);
}
void port_irq_raise(int irq) { liveIrqs.push_back(irq); }
void port_host_alloc_scope_enter() { ++hostScopeDepth; }
void port_host_alloc_scope_leave() { require(hostScopeDepth,"host allocation scope balance"); --hostScopeDepth; }
int port_owns_game_cpu() { return ownsGuestCpu; }
uint32_t port_host_to_phys(const void* pointer) {
    require(ownsGuestCpu,"VI pointer translation occurs before the guest releases its CPU");
    const auto p=reinterpret_cast<uintptr_t>(pointer);
    for (const auto& region:{std::pair{liveRam,0u},std::pair{liveRam2,0x10000000u}}) {
        const auto start=reinterpret_cast<uintptr_t>(region.first);
        if (p>=start && p-start<sizeof(liveRam)) return region.second+uint32_t(p-start);
    }
    port_fatal("unregistered original VI framebuffer pointer");
}
int port_physical_window(uint32_t address,void** data,uint32_t* base,size_t* bytes) {
    require(!ownsGuestCpu,"worker RAM lookup never runs as the guest CPU owner");
    *data=nullptr; *base=0; *bytes=0;
    if (address<sizeof(liveRam)) { *data=liveRam; *bytes=sizeof(liveRam); return 1; }
    if (address>=0x10000000u && address-0x10000000u<sizeof(liveRam2)) {
        *data=liveRam2; *base=0x10000000u; *bytes=sizeof(liveRam2); return 1;
    }
    return 0;
}
void __PortGXFifoFlush() {}
}

// Fail only the armed CPU allocation, independently of native SDK faults.
static thread_local size_t cpu_fail_bytes;
static thread_local unsigned cpu_fail_after;
static thread_local bool cpu_fail_any;
void* operator new(size_t bytes) {
    if (cpu_fail_after && (cpu_fail_any || bytes==cpu_fail_bytes) && !--cpu_fail_after) {
        cpu_fail_any=false; cpu_fail_bytes=0; throw std::bad_alloc();
    }
    if (void* p=std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void* operator new(size_t bytes,const std::nothrow_t&) noexcept {
    try { return ::operator new(bytes); } catch (const std::bad_alloc&) { return nullptr; }
}
void* operator new[](size_t bytes,const std::nothrow_t& tag) noexcept { return ::operator new(bytes,tag); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p,size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p,size_t) noexcept { ::operator delete(p); }
void operator delete(void* p,const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p,const std::nothrow_t&) noexcept { ::operator delete(p); }

int createVitaBringupRenderer(const gpu::VitaShaderLibrary&,VitaBringupRenderer**,std::string&) __attribute__((weak));
int drawVitaBringup(VitaBringupRenderer*,const PortPadState&,std::string&) __attribute__((weak));
int releaseVitaBringupRenderer(VitaBringupRenderer**) __attribute__((weak));

namespace gpu {
int createGxCopyHistory(GxCopyHistory**) __attribute__((weak));
int storeGxCopyBytes(GxCopyHistory*,uint64_t,const SpanCopy&,const GxCopyTexels&,const GxCopyMemory&,
                      std::shared_ptr<const GxCopyBytes>&,std::string&) __attribute__((weak));
int lookupGxCopyBytes(GxCopyHistory*,uint64_t,uint32_t,uint32_t,std::shared_ptr<const GxCopyBytes>&,std::string&) __attribute__((weak));
int collectGxCopyBytes(GxCopyHistory*,uint64_t) __attribute__((weak));
int releaseGxCopyHistory(GxCopyHistory**) __attribute__((weak));
int describeGxTextureCopy(const SpanCopy&,GxCopyGeometry&,std::string&) __attribute__((weak));
int produceGxCopyPixels(const SpanCopy&,const GxCopyPlane&,GxCopyPixels&,std::string&) __attribute__((weak));
int processVitaEfbCapture(VitaEfbCache*,PortVitaGxmSurface*,const SpanCopy&,GxCopyPixels&,std::string&) __attribute__((weak));
int describeGxRamTexture(const SpanRegisters&,unsigned,GxRamTextureLayout&,std::string&) __attribute__((weak));
int decodeGxRamTexture(const SpanRegisters&,unsigned,const GxTextureMemory&,std::shared_ptr<const GxCopyBytes>,GxRamTexture&,
                       std::string&) __attribute__((weak));
int resolveVitaRamImage(VitaTextureCache*,uint64_t,const SpanTexture&,const SpanRegisters&,unsigned,const VitaRamTextures&,
                        PortVitaGxmTextureView*&,std::shared_ptr<const void>&,std::shared_ptr<const GxRamTexture>&,std::string&,bool) __attribute__((weak));
int resolveVitaDrawWithRam(PortVitaGxmDevice*,VitaMaterialCache*,VitaTextureCache*,const VitaShaderLibrary&,const IndexChunks&,
                          const IndexChunk&,const VitaPreparedDraw&,const VitaRamTextures&,VitaDrawBinding&,std::string&) __attribute__((weak));
int copyVitaEfbToMemory(VitaEfbCache*,GxCopyHistory*,PortVitaGxmSurface*,uint64_t,const SpanCopy&,const GxCopyMemory&,
                       std::shared_ptr<const GxCopyBytes>&,std::string&) __attribute__((weak));
bool prepareVitaDraw(const IndexChunks&,size_t,VitaPreparedDraw&,VitaFrameError&) __attribute__((weak));
int createVitaFrameReplay(PortVitaGxmDevice*,VitaFrameReplay**) __attribute__((weak));
bool submitVitaFrame(VitaFrameReplay*,std::shared_ptr<const IndexChunks>,VitaFrameBackend&,VitaFrameError&) __attribute__((weak));
int discardVitaFrame(VitaFrameReplay*) __attribute__((weak));
int releaseVitaFrameReplay(VitaFrameReplay**) __attribute__((weak));
int createVitaTextureCache(PortVitaGxmDevice*,VitaTextureCache**) __attribute__((weak));
int resolveVitaImage(VitaTextureCache*,const Frame&,const SpanTexture&,PortVitaGxmTextureView*&,
                    std::shared_ptr<const void>&,std::string&,bool) __attribute__((weak));
int collectVitaTextures(VitaTextureCache*) __attribute__((weak));
int releaseVitaTextureCache(VitaTextureCache**) __attribute__((weak));
int createVitaMaterialCache(PortVitaGxmDevice*,VitaMaterialCache**) __attribute__((weak));
int resolveVitaMaterial(VitaMaterialCache*,const ShaderUid&,const PortVitaGxmBlend&,const VitaShaderLibrary&,
                       VitaMaterial*&,std::shared_ptr<const void>&,std::string&) __attribute__((weak));
int bindVitaMaterial(const VitaMaterial*,PortVitaGxmDevice*,const ShaderUid&,const PortVitaGxmBlend&,
                    const PixelUniformLayout&,PortVitaGxmProgram*&,std::string&) __attribute__((weak));
int collectVitaMaterials(VitaMaterialCache*) __attribute__((weak));
int releaseVitaMaterialCache(VitaMaterialCache**) __attribute__((weak));
bool installVitaShader(VitaShaderLibrary&,VitaShaderArtifact,std::string&) __attribute__((weak));
int resolveVitaDraw(PortVitaGxmDevice*,VitaMaterialCache*,VitaTextureCache*,const VitaShaderLibrary&,const IndexChunks&,
                    const IndexChunk&,const VitaPreparedDraw&,VitaDrawBinding&,std::string&) __attribute__((weak));
int createVitaEfbCache(PortVitaGxmDevice*,const VitaShaderLibrary&,VitaEfbCache**,std::string&) __attribute__((weak));
int resolveVitaEfb(VitaEfbCache*,uint32_t,uint32_t,PortVitaGxmSurface*&,std::shared_ptr<const void>&,std::string&) __attribute__((weak));
int clearVitaEfb(VitaEfbCache*,PortVitaGxmSurface*,const VitaEfbClear&,std::string&) __attribute__((weak));
int discardVitaEfbClear(VitaEfbCache*) __attribute__((weak));
int collectVitaEfbs(VitaEfbCache*) __attribute__((weak));
int releaseVitaEfbCache(VitaEfbCache**) __attribute__((weak));
std::string genVitaEfbSnapshotFragmentShader(bool) __attribute__((weak));
int captureVitaEfb(VitaEfbCache*,PortVitaGxmSurface*,uint32_t,uint32_t,uint32_t,uint32_t,bool,
                   PortVitaGxmSurface*&,std::shared_ptr<const void>&,std::string&) __attribute__((weak));
}
extern "C" int port_vita_gxm_surface_sample(PortVitaGxmDevice*,PortVitaGxmSurface*,PortVitaGxmSurfaceSampleInfo*) __attribute__((weak));
extern "C" int port_vita_gxm_staging_slice(PortVitaGxmStaging*,const PortVitaGxmSlice*,uint32_t,uint32_t,
    PortVitaGxmSlice*) __attribute__((weak));
static float native_viewport[6];
static unsigned native_rasters;
static SceGxmDepthFunc clear_depth_func;
static SceGxmDepthWriteMode clear_depth_write;
extern "C" void sceGxmSetViewport(SceGxmContext*,float xo,float xs,float yo,float ys,float zo,float zs) {
    require(active_scene,"raster only inside active scene");
    const float v[]={xo,xs,yo,ys,zo,zs}; std::memcpy(native_viewport,v,sizeof(v)); ++native_rasters;
}
extern "C" void sceGxmSetFrontDepthFunc(SceGxmContext*,SceGxmDepthFunc value) { clear_depth_func=value; }
extern "C" void sceGxmSetBackDepthFunc(SceGxmContext*,SceGxmDepthFunc) {}
extern "C" void sceGxmSetFrontDepthWriteEnable(SceGxmContext*,SceGxmDepthWriteMode value) { clear_depth_write=value; }
extern "C" void sceGxmSetBackDepthWriteEnable(SceGxmContext*,SceGxmDepthWriteMode) {}
extern "C" void sceGxmSetCullMode(SceGxmContext*,SceGxmCullMode) {}
extern "C" void sceGxmSetRegionClip(SceGxmContext*,SceGxmRegionClipMode,unsigned,unsigned,unsigned,unsigned) {}

namespace {
using namespace gpu;
void textureImageLifetime() {
    auto image=std::make_shared<TexImage>();
    image->width=4; image->height=4; image->rgba.assign(16,0x10203040u);
    std::weak_ptr<TexImage> observer=image;
    auto recording=std::make_shared<Frame>(); recording->textures.push_back(image);
    image.reset();
    require(!observer.expired() && recording->textures[0]->rgba[15]==0x10203040u,
        "real CPU image remains owned by its recording without a GL or GXM handle");
    auto retained=recording->textures[0];
    recording->clear();
    require(!observer.expired(),"replay CPU image lease survives recording retirement");
    retained.reset();
    require(observer.expired(),"real native CPU image destructor retires pixels when final recording lease ends");
}
uint32_t bits(float f) { uint32_t v; std::memcpy(&v,&f,4); return v; }
void command(Frame& f,std::initializer_list<uint32_t> words) { f.cmds.insert(f.cmds.end(),words); }
void draw(Frame& f,uint32_t primitive,uint32_t count) {
    uint32_t first=uint32_t(f.verts.size());
    for (uint32_t i=0;i<count;++i) f.verts.insert(f.verts.end(),{bits(float(i)+1),bits(2),bits(-3)});
    command(f,{CMD_DRAW,primitive,0,first,count});
}
std::shared_ptr<Frame> frame() {
    auto f=std::make_shared<Frame>(); f->number=127;
    f->startBp[0xF3]=63u<<16; f->startBp[0x41]=8;
    f->startXfRegs[0x20]=bits(1); f->startXfRegs[0x22]=bits(1); f->startXfRegs[0x24]=bits(1);
    f->startXfRegs[0x26]=1; f->startXfRegs[0x1F]=bits(16777215);
    f->startXfRegs[0x1A]=bits(320); f->startXfRegs[0x1B]=bits(228);
    f->startXfRegs[0x1D]=bits(320); f->startXfRegs[0x1E]=bits(228);
    f->startBp[0x21]=(639u<<12)|455u;
    for (unsigned row=0;row<3;++row) f->startXfMem[row*4+row]=1;
    return f;
}
std::shared_ptr<const IndexChunks> chunks(std::shared_ptr<const Frame> f) {
    std::shared_ptr<const DrawSpans> spans; DrawSpanError se;
    require(buildDrawSpans(f,spans,se),"real immutable draw spans");
    std::shared_ptr<const IndexChunks> result; IndexChunkError ce;
    require(buildIndexChunks(spans,result,ce),"real immutable index chunks"); return result;
}
std::shared_ptr<const IndexChunks> single(std::shared_ptr<const Frame> f,std::shared_ptr<const DrawSpan> d) {
    std::shared_ptr<const IndexChunks> result; IndexChunkError e;
    require(buildDrawIndexChunks(f,d,result,e,{123,456}),"sparse immutable chunks"); return result;
}

void installShaders(VitaShaderLibrary& library,const ShaderUid& uid) {
    std::string error;
    SceGxmProgram vs{8,0},fs{8,1};
    auto artifact=[&](VitaShaderStage stage,const std::string& source,const SceGxmProgram& program) {
        const auto* bytes=reinterpret_cast<const unsigned char*>(&program);
        return VitaShaderArtifact{stage,source,std::vector<unsigned char>(bytes,bytes+8),0};
    };
    require(installVitaShader(library,artifact(VitaShaderStage::Vertex,genVitaCpuVertexShader(uid),vs),error) &&
        installVitaShader(library,artifact(VitaShaderStage::Fragment,genPortableRasterFragmentShader(uid,PortableDialect::VitaCg),fs),error),
        "fixture installs exact current shader sources; GXP driver fixture is not compiler/pixel proof");
}
void installClearShader(VitaShaderLibrary& library) {
    ShaderUid uid{}; uid.numTevStages=1; uid.alphaFunc=63; installShaders(library,uid);
    const SceGxmProgram fs{8,1}; const auto* data=reinterpret_cast<const unsigned char*>(&fs); std::string error;
    require(installVitaShader(library,{VitaShaderStage::Fragment,genVitaEfbClearFragmentShader(),
        std::vector<unsigned char>(data,data+8),0},error),"clear fixture uses exact production Cg and independent SDK-driver blob");
}
void installCaptureShaders(VitaShaderLibrary& library) {
    installClearShader(library);
    const SceGxmProgram fs{8,1}; const auto* data=reinterpret_cast<const unsigned char*>(&fs); std::string error;
    for (bool depth:{false,true}) require(installVitaShader(library,
        {VitaShaderStage::Fragment,genVitaEfbSnapshotFragmentShader(depth),std::vector<unsigned char>(data,data+8),0},error),
        "capture fixture installs exact production source, not a guessed compatible material");
}

void shaderInventory() {
    require(inventoryVitaFrameShaders && writeVitaShaderInventory,"bounded original-frame shader inventory exists");
    const auto gpuBefore=std::array<unsigned,4>{finishes,native_draws,sampler_calls,native_rasters};
    const size_t memoryBefore=memory.size();
    auto f=frame(); draw(*f,0x90,3); draw(*f,0x90,3);
    auto source=chunks(f); VitaShaderLibrary library; VitaShaderInventory inventory;
    require(inventoryVitaFrameShaders(*source,library,inventory) && inventory.complete &&
        inventory.inspectedChunks==2 && !inventory.rejectedChunks && inventory.requests.size()==1,
        "all original chunks are inspected and identical shader requests deduplicate");
    const auto original=inventory.requests[0]; VitaPreparedDraw prepared; VitaFrameError error;
    require(prepareVitaDraw(*source,0,prepared,error) && original.uid==source->chunks()[0].state->uid &&
        original.profile==prepared.profile && original.manualTextureMask==prepared.fragment.manualTextureMask &&
        original.pixelVectors==prepared.fragment.pixel.vectors && original.occurrences==2 &&
        original.firstCommandWord==0 && original.vertexMissing && original.fragmentMissing,
        "inventory agrees with actual draw preparation without claiming native shader acceptance");
    installShaders(library,original.uid);
    require(inventoryVitaFrameShaders(*source,library,inventory) && inventory.requests.size()==1 &&
        !inventory.requests[0].vertexMissing && !inventory.requests[0].fragmentMissing,
        "only exact stage/source library matches satisfy an original request");
    auto drawSpan=std::make_shared<DrawSpan>(source->sourceSpans()->draws()[0]);
    auto state=std::make_shared<DrawSpanState>(*drawSpan->state); drawSpan->state=state;
    state->registers.bp[0x43]=1; state->registers.bp[0x41]&=~4u;
    state->raster.blend=true; state->raster.equation=SpanBlendEquation::Add;
    state->raster.src=SpanBlendFactor::SrcAlpha; state->raster.dst=SpanBlendFactor::OneMinusSrcAlpha;
    auto snapshot=single(f,drawSpan);
    require(inventoryVitaFrameShaders(*snapshot,library,inventory) && inventory.requests.size()==1 &&
        inventory.requests[0].profile==VitaFragmentProfile::Rgba6SrcAlphaSnapshot &&
        !inventory.requests[0].vertexMissing && inventory.requests[0].fragmentMissing &&
        inventory.requests[0].pixelVectors==prepared.fragment.pixel.vectors+1,
        "an ordinary raster artifact cannot satisfy the same UID's ordered snapshot profile");
    auto image=std::make_shared<TexImage>(); image->width=image->height=8; image->levels=4;
    image->version=999; image->rgba.assign(85,0x11223344);
    auto manual=frame(); manual->textures.push_back(image);
    manual->startXfRegs[0x3F]=1; manual->startXfRegs[0x40]=6;
    manual->startBp[0x28]=64; manual->startBp[0x80]=0x1d5;
    manual->startBp[0x84]=0x3000; manual->startBp[0x88]=7u|(7u<<10);
    command(*manual,{CMD_TEX,0,0}); draw(*manual,0x90,3);
    command(*manual,{CMD_BP,0x80000000u|0x1d5u|(64u<<9)}); draw(*manual,0x90,3);
    command(*manual,{CMD_BP,0x800000d5u}); draw(*manual,0x90,3);
    source=chunks(manual);
    require(inventoryVitaFrameShaders(*source,{},inventory) && inventory.requests.size()==2 &&
        inventory.requests[0].uid==inventory.requests[1].uid &&
        inventory.requests[0].manualTextureMask==1 && inventory.requests[0].occurrences==2 &&
        inventory.requests[1].manualTextureMask==0 && inventory.requests[1].occurrences==1,
        "dynamic original bias reuses a source while manual and implicit contracts stay distinct");
    for(size_t i=0;i<source->chunks().size();++i) {
        require(prepareVitaDraw(*source,i,prepared,error),"original inventory comparison draw prepares");
        const auto found=std::find_if(inventory.requests.begin(),inventory.requests.end(),[&](const auto& request) {
            return request.uid==source->chunks()[i].state->uid && request.profile==prepared.profile &&
                request.manualTextureMask==prepared.fragment.manualTextureMask;
        });
        require(found!=inventory.requests.end() && found->pixelVectors==prepared.fragment.pixel.vectors,
            "each inventory contract uses the renderer's actual canonical material layout");
    }
    FILE* file=std::tmpfile(); require(file,"inventory uses a real bounded local stream");
    for(const char* tag:{static_cast<const char*>(nullptr),"","000","00001","0001\n","../1","a001"})
        require(!writeVitaShaderInventory(file,manual->number,tag,inventory) && std::ftell(file)==0,
            "invalid inventory tags reject before writing any record");
    require(writeVitaShaderInventory(file,manual->number,"0001",inventory),"complete original inventory writes and flushes");
    std::rewind(file); char recorded[4096]{};
    const auto bytes=std::fread(recorded,1,sizeof(recorded)-1,file);
    require(bytes>0 && bytes<sizeof(recorded)-1 && std::strstr(recorded,"\"type\":\"begin\"") &&
        std::strstr(recorded,"\"type\":\"request\"") && std::strstr(recorded,"\"manual-mask\":1") &&
        std::strstr(recorded,"\"type\":\"end\"") && std::strstr(recorded,"\"complete\":true"),
        "local inventory has explicit complete begin/request/end records, not a gameplay claim");
    require(recorded[0]=='{' && !std::strstr(recorded,"native-shader-inventory"),
        "private inventory is pure JSONL rather than fragmented emulator TTY output");
    std::fclose(file);
    for(unsigned mutation=0;mutation<5;++mutation) {
        auto corrupt=inventory;
        if(mutation==0) corrupt.requests[0].occurrences=UINT32_MAX;
        if(mutation==1) corrupt.requests[0].pixelVectors=0;
        if(mutation==2) corrupt.requests[0].profile=static_cast<VitaFragmentProfile>(5);
        if(mutation==3) corrupt.limitReached=true;
        if(mutation==4) corrupt.rejectedChunks=1;
        file=std::tmpfile(); require(file,"corrupt diagnostic validates before touching the real stream");
        require(!writeVitaShaderInventory(file,manual->number,"0001",corrupt) && std::ftell(file)==0,
            "inconsistent contracts and bounded counts reject before any diagnostic bytes");
        std::fclose(file);
    }
    require(configureVitaShaderCapture && captureVitaShaderInventory,"exclusive private shader capture exists");
    char directory[]="/tmp/galaxy-shader-inventory-XXXXXX";
    require(mkdtemp(directory),"shader capture has an actual private output directory");
    VitaShaderCapture capture;
    auto configure=[&](const char* marker) {
        if(!marker) return configureVitaShaderCapture(nullptr,capture);
        FILE* input=std::tmpfile(); require(input,"shader marker uses an actual bounded stream");
        const auto length=std::strlen(marker);
        require(std::fwrite(marker,1,length,input)==length,"shader marker writes exact test bytes");
        std::rewind(input); const bool enabled=configureVitaShaderCapture(input,capture);
        std::fclose(input); return enabled;
    };
    for(const char* marker:{static_cast<const char*>(nullptr),"","0001","001\n","00001\n","0001\nextra","../1\n","a001\n"}) {
        require(!configure(marker) && !capture.tag[0] && !capture.attempted,
            "shader inventory only enables exactly four ASCII digits plus newline");
        require(captureVitaShaderInventory(directory,manual->number,inventory,capture)==0,
            "disabled shader diagnostics cannot create output or attempt capture");
    }
    require(configure("0001\n") && !std::strcmp(capture.tag,"0001"),"valid shader tag enables one capture");
    const std::string output=std::string(directory)+"/shader-inventory-0001.jsonl";
    require(captureVitaShaderInventory(directory,manual->number,inventory,capture)==1 && capture.attempted,
        "shader capture commits exclusive output only after successful writer and close");
    require(captureVitaShaderInventory(directory,manual->number,inventory,capture)==0,
        "shader diagnostic never retries in the same fatal process");
    file=std::fopen(output.c_str(),"rb"); require(file,"saved shader capture is readable");
    require(std::fread(recorded,1,sizeof(recorded)-1,file)==bytes && !std::ferror(file),
        "exclusive capture contains the real bounded inventory"); std::fclose(file);
    struct stat metadata{};
    require(!::stat(output.c_str(),&metadata) && (metadata.st_mode&0777)==0600,
        "shader inventory file permissions are private");
    require(configure("0001\n") && captureVitaShaderInventory(directory,manual->number,inventory,capture)<0 &&
        !::stat(output.c_str(),&metadata) && size_t(metadata.st_size)==bytes,
        "existing shader output remains untouched and never overwritten");
    require(!::unlink(output.c_str()) && !::symlink("/dev/full",output.c_str()),"shader symlink rejection fixture exists");
    require(configure("0001\n") && captureVitaShaderInventory(directory,manual->number,inventory,capture)<0 &&
        !::lstat(output.c_str(),&metadata) && S_ISLNK(metadata.st_mode),
        "exclusive shader output cannot follow or delete an existing symlink");
    require(!::unlink(output.c_str()),"shader test removes only its own symlink");
    for(int failure:{1,2,3}) {
        require(configure("0001\n"),"failed shader output starts with a fresh tag"); captureIoFailure=failure;
        const int result=captureVitaShaderInventory(directory,manual->number,inventory,capture); captureIoFailure=0;
        require(result<0 && ::lstat(output.c_str(),&metadata)<0,
            "fdopen, write and fclose failures remove only the newly created partial shader file");
        require(captureVitaShaderInventory(directory,manual->number,inventory,capture)==0,
            "failed shader diagnostic cannot silently retry");
    }
    require(configure("0001\n") && captureVitaShaderInventory(nullptr,manual->number,inventory,capture)<0,
        "missing shader output scope rejects without creating a file");
    const std::string tooLong(200,'a');
    require(configure("0001\n") && captureVitaShaderInventory(tooLong.c_str(),manual->number,inventory,capture)<0,
        "truncated shader output paths cannot redirect private creation");
    require(!::rmdir(directory),"shader capture leaves no diagnostic files or descriptors behind");
    file=std::fopen("/dev/full","wb"); require(file,"inventory tests actual stream failure");
    require(!writeVitaShaderInventory(file,manual->number,"0001",inventory),
        "buffered write or flush failure cannot report a saved inventory"); std::fclose(file);
    auto invalid=frame(); command(*invalid,{CMD_XF,0x101a,1,bits(NAN)});
    for(unsigned i=0;i<70;++i) draw(*invalid,0x90,3);
    command(*invalid,{CMD_XF,0x101a,1,bits(320)}); draw(*invalid,0x90,3);
    source=chunks(invalid);
    require(!inventoryVitaFrameShaders(*source,{},inventory) && !inventory.complete &&
        inventory.inspectedChunks==71 && inventory.rejectedChunks==70 && inventory.rejections.size()==64 &&
        inventory.requests.size()==1 && !inventory.rejections[0].message.empty(),
        "bounded rejection details never hide unsupported original chunks or stop later valid inspection");
    auto pressure=frame();
    for(unsigned i=0;i<1025;++i) { command(*pressure,{CMD_BP,0xc0000000u|(i<<4)}); draw(*pressure,0x90,3); }
    source=chunks(pressure);
    require(!inventoryVitaFrameShaders(*source,{},inventory) && inventory.limitReached && !inventory.complete &&
        inventory.requests.size()==1024 && inventory.inspectedChunks==1025,
        "unique request cap publishes an incomplete diagnostic, never truncated shader coverage");
    auto empty=chunks(frame());
    require(inventoryVitaFrameShaders(*empty,{},inventory) && inventory.complete && !inventory.inspectedChunks &&
        inventory.requests.empty() && inventory.rejections.empty(),"empty frame inventory clears prior partial evidence");
    require(memory.size()==memoryBefore && gpuBefore==std::array<unsigned,4>{finishes,native_draws,sampler_calls,native_rasters},
        "shader inventory never allocates or submits native GPU resources");
}

// Catches mismatched GX code/blend reuse, legacy scissor bypass, raw owner
// release, prematurely evicted material and lost native rollback ownership.
void materials() {
    require(createVitaMaterialCache && resolveVitaMaterial && bindVitaMaterial && collectVitaMaterials && releaseVitaMaterialCache,
            "production exact-source native material cache exists");
    PortVitaGxmDevice* device=nullptr; VitaMaterialCache* cache=nullptr;
    require(port_vita_gxm_device_create(&device)==0 && createVitaMaterialCache(device,&cache)==0,"native material cache device ownership");
    ShaderUid uid{}; uid.numTevStages=1; uid.alphaFunc=63;
    VitaShaderLibrary baked; std::string baked_error;
    require(installBakedVitaShaders(baked,baked_error),"real hash-verified compiler bundle is installed as CPU-owned native library data");
    const auto* baked_vs=findVitaShader(baked,VitaShaderStage::Vertex,genVitaCpuVertexShader(uid));
    const auto* baked_fs=findVitaShader(baked,VitaShaderStage::Fragment,genPortableRasterFragmentShader(uid,PortableDialect::VitaCg));
    require(baked_vs && baked_fs && baked_vs->size()==320 && baked_fs->size()==1676,
            "current generated base sources find exact genuine accepted blobs; no SDK/GPU execution is inferred");
    const auto* baked_clear=findVitaShader(baked,VitaShaderStage::Fragment,genVitaEfbClearFragmentShader());
    require(baked_clear && baked_clear->size()==404,
            "current exact native clear source resolves its genuine compiler artifact in the delivered bundle");
    for (bool depth:{false,true}) {
        const auto* baked_capture=findVitaShader(baked,VitaShaderStage::Fragment,genVitaEfbSnapshotFragmentShader(depth));
        require(baked_capture && baked_capture->size()>=8,
            "both exact current raw capture sources resolve genuine accepted compiler artifacts in the delivered bundle");
    }
    PortVitaGxmBlend blend{15,1,1,4,5,1,0};
    VitaShaderLibrary library; VitaMaterial *a=nullptr,*b=nullptr;
    std::shared_ptr<const void> al,bl; std::string error;
    require(resolveVitaMaterial(cache,uid,blend,library,a,al,error)<0 && !a && !al && !error.empty(),
            "missing exact compiler artifact never substitutes a compatible shader");
    const std::string missing="missing exact full-precision CPU-XF/raster-aware TEV compiler artifact (uid-bytes=";
    require(error.starts_with(missing) && error.size()==missing.size()+sizeof(uid)*2+1 && error.back()==')',
            "missing material diagnostic retains the complete original UID for exact-source compilation");
    const auto* uid_bytes=reinterpret_cast<const unsigned char*>(&uid);
    for (size_t i=0;i<sizeof(uid);++i) {
        const auto pair=error.substr(missing.size()+i*2,2);
        require(pair.find_first_not_of("0123456789abcdef")==std::string::npos &&
            std::stoul(pair,nullptr,16)==uid_bytes[i],"missing material UID bytes round-trip without truncation or normalization");
    }
    installShaders(library,uid);
    require(resolveVitaMaterial(cache,uid,blend,library,a,al,error)==0 && a && al,"real material program creation from exact source pair");
    require(patched_blend.color_mask==15 && patched_blend.color_func==1 && patched_blend.alpha_func==1 &&
        patched_blend.color_src==4 && patched_blend.color_dst==5 && patched_blend.alpha_src==1 && patched_blend.alpha_dst==0,
        "all seven actual native blend/mask scalars are patched, not just shader UID");
    unsigned registrations=shader_registrations, programs=shader_programs;
    require(resolveVitaMaterial(cache,uid,blend,library,b,bl,error)==0 && a==b &&
        shader_registrations==registrations && shader_programs==programs,"same material reuses native registrations and patcher program");
    PixelUniformLayout layout;
    require(makePixelUniformLayout(uid,layout,error),"real material pixel layout");
    PortVitaGxmProgram* native=nullptr;
    require(bindVitaMaterial(a,device,uid,blend,layout,native,error)==0 && native,"typed material binds actual owned reflected program");
    auto* alias=native; unsigned before=finishes;
    require(port_vita_gxm_program_release(&alias)<0 && alias==native &&
        port_vita_gxm_device_program_release(device,&alias)<0 && alias==native && finishes==before,
        "raw and public-device program release cannot bypass live material cache lease");
    bl.reset(); b=nullptr; auto alternate=blend; alternate.alpha_dst=9;
    require(resolveVitaMaterial(cache,uid,alternate,library,b,bl,error)==0 && b!=a && shader_programs==programs+1 && patched_blend.alpha_dst==9,
            "independent alpha blend factor creates distinct patched native material");
    native=nullptr;
    require(bindVitaMaterial(a,device,uid,alternate,layout,native,error)<0 && !native,
            "typed material rejects wrong blend before GPU submission");
    auto wrong=uid; wrong.tevColor[0]=1u<<19;
    require(bindVitaMaterial(a,device,wrong,blend,layout,native,error)<0 && !native,
            "typed material rejects a different TEV shader state");
    auto bad_layout=layout; bad_layout.slots[0]=3;
    require(bindVitaMaterial(a,device,uid,blend,bad_layout,native,error)<0 && !native,
            "pixel slot mismatch cannot feed uniforms to an unrelated layout");
    require(bindVitaMaterial(a,nullptr,uid,blend,layout,native,error)<0 && !native,
            "material cannot cross device ownership");
    unsigned allocations=memory.size();
    require(collectVitaMaterials(cache)==0 && memory.size()==allocations && al && bl,
            "material collection never retires an outstanding lease");
    al.reset(); bl.reset(); a=b=nullptr;
    registrations=shader_registrations; programs=shader_programs;
    const unsigned steady_finishes=finishes;
    for (unsigned frame=0;frame<120;++frame) {
        require(collectVitaMaterials(cache)==0 && finishes==steady_finishes,
                "steady-state material collection retains idle programs without a GPU drain");
        require(resolveVitaMaterial(cache,uid,blend,library,a,al,error)==0 &&
                shader_registrations==registrations && shader_programs==programs,
                "120 frame handoffs reuse the original exact native material without registrations");
        native=nullptr;
        require(bindVitaMaterial(a,device,uid,blend,layout,native,error)==0 && native,
                "retained material still passes exact UID blend layout and reflected native binding");
        al.reset(); a=nullptr;
    }
    require(port_vita_gxm_device_begin_display(device)==0 && collectVitaMaterials(cache)<0,
            "program cache cannot collect during an open native scene");
    require(port_vita_gxm_device_end(device,nullptr,0)==0,"material retirement fixture submitted");
    al.reset(); bl.reset(); a=b=nullptr;
    shader_release_error=-892;
    require(releaseVitaMaterialCache(&cache)==-892 && cache && !active_scene && !pending_gpu,
            "failed native unregister retains material/cache/device recovery owner after Finish");
    require(resolveVitaMaterial(cache,uid,blend,library,a,al,error)<0 && !a && !al,
            "closing cache cannot publish new material bindings"); shader_release_error=0;
    require(releaseVitaMaterialCache(&cache)==0 && !cache,"material close retries retained unregister without double release");
    require(createVitaMaterialCache(device,&cache)==0,"material rollback cache fixture");
    shader_register_fail_call=shader_registrations+2; shader_release_error=-893;
    require(resolveVitaMaterial(cache,uid,blend,library,a,al,error)==-893 && !a && !al,
            "failed fragment registration plus rollback keeps partial program solely in cache recovery owner");
    shader_register_fail_call=0; shader_release_error=0;
    require(collectVitaMaterials(cache)==0 && resolveVitaMaterial(cache,uid,blend,library,a,al,error)==0,
            "collected rollback owner permits a clean exact-material retry");
    al.reset(); a=nullptr;
    require(releaseVitaMaterialCache(&cache)==0,"rollback cache fully closes before retention-pressure fixture");
    require(createVitaMaterialCache(device,&cache)==0,"bounded idle material retention fixture");
    auto variant=[&](unsigned number) {
        auto value=blend; value.color_mask=number&15; value.color_dst=number>>4; return value;
    };
    const auto pinned_blend=variant(0);
    require(resolveVitaMaterial(cache,uid,pinned_blend,library,a,al,error)==0,"leased oldest material under pressure");
    for (unsigned number=1;number<=40;++number) {
        const auto value=variant(number);
        require(resolveVitaMaterial(cache,uid,value,library,b,bl,error)==0,"distinct immutable blend creates its own material");
        bl.reset(); b=nullptr;
        require(collectVitaMaterials(cache)==0,"idle material pressure collection succeeds");
        native=nullptr;
        require(bindVitaMaterial(a,device,uid,pinned_blend,layout,native,error)==0 && native,
                "oldest outstanding lease survives all 40 newer material keys");
    }
    registrations=shader_registrations; programs=shader_programs;
    const auto recent=variant(40);
    require(resolveVitaMaterial(cache,uid,recent,library,b,bl,error)==0 &&
            shader_registrations==registrations && shader_programs==programs,
            "most recent idle key survives the 32-program retention budget");
    bl.reset(); b=nullptr; al.reset(); a=nullptr;
    require(collectVitaMaterials(cache)==0,"newly idle oldest lease participates in bounded eviction");
    require(resolveVitaMaterial(cache,uid,pinned_blend,library,a,al,error)==0 &&
            shader_registrations==registrations+2 && shader_programs==programs+1,
            "retention is bounded: oldest idle program is really evicted and rebuilt from its exact artifacts");
    al.reset(); a=nullptr;
    require(releaseVitaMaterialCache(&cache)==0 && port_vita_gxm_device_release(&device)==0,
            "material/library/native resources fully retired"); empty_state();
}

void materialProfiles() {
    require(&describeVitaFragment && &genVitaFragmentShader && &resolveVitaMaterialProfile && &bindVitaMaterialProfile,
        "production typed snapshot shader layout and logical material profile exist");
    ShaderUid uid{}; uid.numTevStages=1; uid.alphaFunc=63;
    VitaFragmentLayout ordinary,snapshot; std::string error;
    require(describeVitaFragment(uid,VitaFragmentProfile::Raster,ordinary,error) && ordinary.pixel.vectors==4 &&
        ordinary.pixel.textureMask==0 && ordinary.snapshotSampler==8 && ordinary.snapshotSlot==27,
        "ordinary source profile preserves its original four-vector no-snapshot layout");
    require(describeVitaFragment(uid,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,snapshot,error) &&
        snapshot.pixel.slots==ordinary.pixel.slots && snapshot.pixel.vectors==5 && snapshot.pixel.textureMask==1 &&
        snapshot.snapshotSlot==4 && snapshot.snapshotSampler==0,
        "snapshot appends its own uniform and unoccupied sampler without aliasing original TEV slots");
    require(&packVitaFragmentUniforms,"production snapshot uniform packing preserves original slots and validates metadata");
    PixelState state{}; state.tevReg[1][0]=0.5f; state.tevReg[2][3]=0.75f;
    std::array<float,108> legacy{},packed{};
    require(packPixelUniforms(ordinary.pixel,state,legacy) &&
        packVitaFragmentUniforms(uid,VitaFragmentProfile::Raster,state,0,0,false,0,packed,error) && legacy==packed,
        "ordinary profile packing is unchanged, with no invented appended payload");
    require(packVitaFragmentUniforms(uid,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,state,64,32,true,173,packed,error) &&
        std::equal(legacy.begin(),legacy.begin()+16,packed.begin()) && packed[16]==64 && packed[17]==32 &&
        packed[18]==1 && packed[19]==173 && std::all_of(packed.begin()+20,packed.end(),[](float x){return x==0;}),
        "original compact TEV payload is packed first; exact extent/override bytes occupy only the appended vec4");
    for(const auto bad:std::array<std::array<uint32_t,3>,3>{{{0,32,173},{64,4097,173},{64,32,256}}}) {
        packed.fill(9);
        require(!packVitaFragmentUniforms(uid,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,state,bad[0],bad[1],true,bad[2],packed,error) &&
            !error.empty() && std::all_of(packed.begin(),packed.end(),[](float x){return x==0;}),
            "invalid snapshot metadata publishes no partial original or appended pixel payload");
    }
    const auto rasterSource=genPortableRasterFragmentShader(uid,PortableDialect::VitaCg);
    require(genVitaFragmentShader(uid,VitaFragmentProfile::Raster)==rasterSource,
        "ordinary profile source is byte-for-byte unchanged for every existing baked artifact");
    auto src=genVitaFragmentShader(uid,VitaFragmentProfile::Rgba6SrcAlphaSnapshot);
    require(!src.empty() && src!=rasterSource && src.find("s_tex0 : TEXUNIT0")!=std::string::npos &&
        src.find("u_pixel[5]")!=std::string::npos && src.find("snapshotDestination")!=std::string::npos,
        "snapshot TEV shader is a different exact full-precision source with explicit resources");
    auto textured=uid; textured.numTexGens=1; textured.tevOrder[0]=64;
    require(describeVitaFragment(textured,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,snapshot,error) &&
        snapshot.snapshotSampler==1 && snapshot.pixel.textureMask==3 && snapshot.snapshotSlot==5 && snapshot.pixel.vectors==6,
        "original sampled texture zero remains immutable; snapshot receives a separate live unit one");
    auto all=uid; all.numTexGens=8; all.numTevStages=8;
    for(unsigned n=0;n<4;++n) all.tevOrder[n]=(64u+2*n)|((65u+2*n)<<12);
    require(!describeVitaFragment(all,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,snapshot,error) &&
        !error.empty() && genVitaFragmentShader(all,VitaFragmentProfile::Rgba6SrcAlphaSnapshot).empty(),
        "all eight original samplers reject instead of stealing or truncating an original texture binding");
    auto fog=uid; fog.fogSel=4;
    require(!describeVitaFragment(fog,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,snapshot,error) &&
        !describeVitaFragment(uid,VitaFragmentProfile(99),snapshot,error),
        "unverified fog and unknown source profiles cannot become compatible snapshot materials");
    auto tested=uid; tested.alphaFunc=2|(7u<<3); tested.zTex=2|(2u<<2);
    src=genVitaFragmentShader(tested,VitaFragmentProfile::Rgba6SrcAlphaSnapshot);
    require(src.find("float a=outi.a")<src.find("snapshotDestination") &&
        src.find("result.depth=")<src.find("snapshotDestination") && src.find("return result;")!=std::string::npos,
        "original byte-alpha test and Z-texture depth execute before storage/blend without rewritten equations");
    PortVitaGxmDevice* device=nullptr; VitaMaterialCache* cache=nullptr;
    require(port_vita_gxm_device_create(&device)==0 && createVitaMaterialCache(device,&cache)==0,"typed profile cache fixture");
    VitaShaderLibrary library; installShaders(library,uid);
    const SceGxmProgram fs{8,3}; const auto* bytes=reinterpret_cast<const unsigned char*>(&fs);
    require(installVitaShader(library,{VitaShaderStage::Fragment,genVitaFragmentShader(uid,VitaFragmentProfile::Rgba6SrcAlphaSnapshot),
        std::vector<unsigned char>(bytes,bytes+8),0},error),"profile fixture uses its exact generated source, SDK doubles only");
    const PortVitaGxmBlend opaque{15,0,0,1,0,1,0}, logical{15,1,1,4,5,4,5};
    VitaMaterial *a=nullptr,*b=nullptr,*c=nullptr; std::shared_ptr<const void> al,bl,cl;
    require(resolveVitaMaterial(cache,uid,opaque,library,a,al,error)==0,"original opaque material resolves normally");
    require(resolveVitaMaterialProfile(cache,uid,logical,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,library,b,bl,error)==0 &&
        b && b!=a && std::memcmp(&patched_blend,&opaque,sizeof(opaque))==0,
        "logical SrcAlpha material stays distinct from opaque despite identical effective hardware blend");
    require(describeVitaFragment(uid,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,snapshot,error),"restore exact base snapshot layout");
    PortVitaGxmProgram* p=nullptr;
    require(bindVitaMaterialProfile(b,device,uid,logical,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,snapshot,p,error)==0 && p,
        "typed profile bind verifies genuine stored sampler/uniform/reflection ownership");
    const unsigned programs=shader_programs,registrations=shader_registrations;
    require(resolveVitaMaterialProfile(cache,uid,logical,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,library,c,cl,error)==0 &&
        c==b && programs==shader_programs && registrations==shader_registrations,
        "same logical blend/source profile reuses the exact existing native program");
    cl.reset(); c=nullptr;
    require(bindVitaMaterial(b,device,uid,logical,ordinary.pixel,p,error)<0 && !p,
        "legacy binding cannot mistake a programmable blend shader for its old UID/blend-compatible path");
    auto wrong=snapshot; wrong.snapshotSampler=1;
    require(bindVitaMaterialProfile(b,device,uid,logical,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,wrong,p,error)<0 && !p,
        "typed material rejects a different snapshot sampler before GPU use");
    wrong=snapshot; wrong.snapshotSlot=0;
    require(bindVitaMaterialProfile(b,device,uid,logical,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,wrong,p,error)<0 && !p,
        "typed material rejects a snapshot uniform alias to an original TEV register");
    require(bindVitaMaterialProfile(b,device,uid,opaque,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,snapshot,p,error)<0 && !p,
        "original logical factors are checked even though native blending is disabled");
    auto unsupported=logical; unsupported.color_src=1;
    require(resolveVitaMaterialProfile(cache,uid,unsupported,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,library,c,cl,error)<0 &&
        !c && !cl && programs==shader_programs,"unimplemented factors reject rather than reuse the SrcAlpha shader");
    al.reset(); bl.reset(); a=b=nullptr;
    require(releaseVitaMaterialCache(&cache)==0 && port_vita_gxm_device_release(&device)==0,"typed profile resources retire normally");
    reflect_texture=false; empty_state();
    require(port_vita_gxm_device_create(&device)==0 && createVitaMaterialCache(device,&cache)==0,
        "profile reflection rejection owns its separate cache fixture");
    VitaShaderLibrary broken; installShaders(broken,uid);
    const SceGxmProgram missingTexture{8,2}; const auto* badBytes=reinterpret_cast<const unsigned char*>(&missingTexture);
    require(installVitaShader(broken,{VitaShaderStage::Fragment,genVitaFragmentShader(uid,VitaFragmentProfile::Rgba6SrcAlphaSnapshot),
        std::vector<unsigned char>(badBytes,badBytes+8),0},error),"independent driver fixture omits compiled snapshot resources");
    require(resolveVitaMaterialProfile(cache,uid,logical,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,broken,b,bl,error)<0 &&
        !b && !bl && !error.empty(),"aggregate old four-vector/no-sampler reflection cannot satisfy a snapshot profile");
    require(resolveVitaMaterialProfile(cache,uid,logical,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,library,b,bl,error)<0 && !b && !bl,
        "rejected native reflection remains recovery-owned and unusable until collection");
    require(collectVitaMaterials(cache)==0 &&
        resolveVitaMaterialProfile(cache,uid,logical,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,library,b,bl,error)==0 && b && bl,
        "collection retires the actual rejected program before a valid exact-profile retry");
    bl.reset(); b=nullptr;
    require(releaseVitaMaterialCache(&cache)==0 && port_vita_gxm_device_release(&device)==0,"rejected profile owner is fully recovered");
    reflect_texture=false; empty_state();
}

void additiveProfile() {
    const auto profile=VitaFragmentProfile(2);
    ShaderUid uid{}; uid.numTevStages=1; uid.alphaFunc=63;
    VitaFragmentLayout layout; std::string error;
    require(describeVitaFragment(uid,profile,layout,error) && layout.pixel.vectors==5 &&
        layout.snapshotSampler==0 && layout.snapshotSlot==4,
        "additive RGBA6 profile owns a distinct exact snapshot contract");
    PixelState state{}; state.tevReg[1][0]=0.5f;
    std::array<float,108> pixel{};
    require(packVitaFragmentUniforms(uid,profile,state,64,32,true,173,pixel,error) &&
        pixel[16]==64 && pixel[17]==32 && pixel[18]==1 && pixel[19]==173,
        "additive profile preserves padded source extent and raw original alpha override");
    auto fog=uid; fog.fogSel=4;
    require(!describeVitaFragment(fog,profile,layout,error),"additive profile does not invent a fog strategy");
    require(describeVitaFragment(uid,profile,layout,error),"restore additive exact layout");
    PortVitaGxmDevice* device=nullptr; VitaMaterialCache* cache=nullptr;
    require(port_vita_gxm_device_create(&device)==0 && createVitaMaterialCache(device,&cache)==0,
        "additive material owns real device and cache");
    VitaShaderLibrary library; installShaders(library,uid);
    const SceGxmProgram fs{8,3}; const auto* bytes=reinterpret_cast<const unsigned char*>(&fs);
    require(installVitaShader(library,{VitaShaderStage::Fragment,genVitaFragmentShader(uid,profile),
        std::vector<unsigned char>(bytes,bytes+8),0},error),"install exact additive generated source");
    const PortVitaGxmBlend additive{14,1,1,1,1,1,1},effective{14,0,0,1,0,1,0};
    VitaMaterial* material=nullptr; std::shared_ptr<const void> lease;
    require(resolveVitaMaterialProfile(cache,uid,additive,profile,library,material,lease,error)==0 && material &&
        std::memcmp(&patched_blend,&effective,sizeof(effective))==0,
        "original RGB-only One/One factors select shader blending without enabling native double blending");
    PortVitaGxmProgram* program=nullptr;
    require(bindVitaMaterialProfile(material,device,uid,additive,profile,layout,program,error)==0 && program,
        "exact additive profile can bind its typed resources");
    require(bindVitaMaterialProfile(material,device,uid,additive,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,
        layout,program,error)<0 && !program,"additive material cannot alias the SrcAlpha shader");
    auto wrong=additive; wrong.alpha_dst=0;
    VitaMaterial* rejected=nullptr; std::shared_ptr<const void> rejectedLease;
    require(resolveVitaMaterialProfile(cache,uid,wrong,profile,library,rejected,rejectedLease,error)<0 &&
        !rejected && !rejectedLease,"unimplemented alpha factors reject before patcher use");
    lease.reset(); material=nullptr;
    require(releaseVitaMaterialCache(&cache)==0 && port_vita_gxm_device_release(&device)==0,
        "additive profile releases all actual native owners");
    reflect_texture=false; empty_state();
}

void srcAlphaDitherProfile() {
    const auto profile=VitaFragmentProfile(3);
    ShaderUid uid{}; uid.numTevStages=1; uid.alphaFunc=63;
    VitaFragmentLayout layout,oldLayout; std::string error;
    require(describeVitaFragment(uid,profile,layout,error) && layout.pixel.vectors==5 &&
        layout.snapshotSampler==0 && layout.snapshotSlot==4,
        "dithered SrcAlpha owns a distinct exact snapshot source profile");
    require(describeVitaFragment(uid,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,oldLayout,error) &&
        layout.pixel.slots==oldLayout.pixel.slots && layout.pixel.textureMask==oldLayout.pixel.textureMask,
        "dithered SrcAlpha preserves original TEV slots and the existing ordered scratch contract");
    const auto source=genVitaFragmentShader(uid,profile);
    const auto blended=source.find("vec4 stored=floor((outi*snapshotSrc+snapshotDestination*snapshotDst)/256.0)");
    const auto dither=source.find("stored.rgb+=vec3(bayer)-floor(stored.rgb/64.0)");
    const auto overrideAlpha=source.find("stored.a=floor(u_pixel[4].w)");
    require(!source.empty() && source!=genVitaFragmentShader(uid,VitaFragmentProfile::Rgba6SrcAlphaSnapshot) &&
        source!=genVitaFragmentShader(uid,VitaFragmentProfile::Rgba6AddSnapshot) &&
        blended!=std::string::npos && blended<dither && dither<overrideAlpha &&
        overrideAlpha<source.find("stored=floor(stored/4.0)") &&
        source.find("floor(gl_FragCoord.xy)")!=std::string::npos && source.find("half")==std::string::npos,
        "new full-F32 source orders complementary byte-factor blend, absolute RGB-only Bayer, alpha override, then RGBA6 storage");
    require(genVitaFragmentShader(uid,VitaFragmentProfile::Rgba6SrcAlphaSnapshot).find("float bayer")==std::string::npos,
        "legacy undithered SrcAlpha source is not silently rewritten");
    PixelState state{}; std::array<float,108> packed{};
    require(packVitaFragmentUniforms(uid,profile,state,64,32,true,173,packed,error) &&
        packed[16]==64 && packed[17]==32 && packed[18]==1 && packed[19]==173,
        "dither profile appends exact padded extent and raw post-factor alpha byte");
    auto fog=uid; fog.fogSel=4;
    require(!describeVitaFragment(fog,profile,layout,error) && genVitaFragmentShader(fog,profile).empty(),
        "new dither capability cannot broaden unverified fog ordering");
    auto full=uid; full.numTexGens=8; full.numTevStages=8;
    for(unsigned i=0;i<4;++i) full.tevOrder[i]=(64u+2*i)|((65u+2*i)<<12);
    require(!describeVitaFragment(full,profile,layout,error),"dithered blend cannot steal an occupied original sampler");
    PortVitaGxmDevice* device=nullptr; VitaMaterialCache* cache=nullptr;
    require(port_vita_gxm_device_create(&device)==0 && createVitaMaterialCache(device,&cache)==0,
        "dithered SrcAlpha has actual cache/native ownership");
    VitaShaderLibrary library; installShaders(library,uid);
    const SceGxmProgram fs{8,3}; const auto* bytes=reinterpret_cast<const unsigned char*>(&fs);
    for(const auto variant:{VitaFragmentProfile::Rgba6SrcAlphaSnapshot,profile})
        require(installVitaShader(library,{VitaShaderStage::Fragment,genVitaFragmentShader(uid,variant),
            std::vector<unsigned char>(bytes,bytes+8),0},error),"cache fixtures install each exact generated source separately");
    const PortVitaGxmBlend logical{14,1,1,4,5,4,5},effective{14,0,0,1,0,1,0};
    VitaMaterial *old=nullptr,*material=nullptr,*again=nullptr; std::shared_ptr<const void> oldLease,lease,againLease;
    require(resolveVitaMaterialProfile(cache,uid,logical,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,library,old,oldLease,error)==0 &&
        resolveVitaMaterialProfile(cache,uid,logical,profile,library,material,lease,error)==0 && material!=old &&
        std::memcmp(&patched_blend,&effective,sizeof(effective))==0,
        "new logical SrcAlpha/dither key stays distinct and does not apply native blending twice");
    const unsigned programs=shader_programs;
    require(resolveVitaMaterialProfile(cache,uid,logical,profile,library,again,againLease,error)==0 && again==material &&
        shader_programs==programs,"same exact dither profile reuses its native cache program");
    require(describeVitaFragment(uid,profile,layout,error),"restore dithered SrcAlpha layout");
    PortVitaGxmProgram* program=nullptr;
    require(bindVitaMaterialProfile(material,device,uid,logical,profile,layout,program,error)==0 && program,
        "dither profile binds only its exact reflected native layout");
    require(bindVitaMaterialProfile(material,device,uid,logical,VitaFragmentProfile::Rgba6SrcAlphaSnapshot,layout,program,error)<0 && !program,
        "dithered and undithered SrcAlpha cannot alias despite equal resource counts");
    auto wrong=logical; wrong.alpha_dst=1;
    VitaMaterial* rejected=nullptr; std::shared_ptr<const void> rejectedLease;
    require(resolveVitaMaterialProfile(cache,uid,wrong,profile,library,rejected,rejectedLease,error)<0 && !rejected && !rejectedLease,
        "dither profile retains narrow original color and alpha factor checks");
    oldLease.reset(); lease.reset(); againLease.reset(); old=material=again=nullptr;
    require(releaseVitaMaterialCache(&cache)==0 && port_vita_gxm_device_release(&device)==0,
        "dither cache resources retire through their actual owners"); reflect_texture=false; empty_state();
    auto original=frame(); original->startBp[0x41]=0x34ad; original->startBp[0x42]=0; original->startBp[0x43]=0x41;
    draw(*original,0x90,3); auto chunksForDraw=chunks(original); VitaPreparedDraw prepared; VitaFrameError failure;
    require(prepareVitaDraw(*chunksForDraw,0,prepared,failure) && prepared.profile==profile &&
        prepared.raster.rgba6 && prepared.raster.dither && prepared.blend.color_mask==14 &&
        prepared.blend.color_src==4 && prepared.blend.color_dst==5 && !prepared.raster.alpha_override,
        "actual blocking BP state selects RGB-only SrcAlpha/dither without inventing alpha updates");
    VitaShaderInventory inventory;
    require(inventoryVitaFrameShaders(*chunksForDraw,{},inventory) && inventory.requests.size()==1 &&
        inventory.requests[0].profile==profile && inventory.requests[0].pixelVectors==prepared.fragment.pixel.vectors,
        "native batch inventory agrees with the new renderer's exact profile");
    original->startBp[0x41]&=~4u; chunksForDraw=chunks(original);
    require(prepareVitaDraw(*chunksForDraw,0,prepared,failure) && prepared.profile==VitaFragmentProfile::Rgba6SrcAlphaSnapshot,
        "turning dither off retains the original already-baked undithered profile");
    auto lineFrame=frame();
    std::copy(std::begin(original->startBp),std::end(original->startBp),std::begin(lineFrame->startBp));
    lineFrame->startBp[0x41]|=4u;
    draw(*lineFrame,0xA8,2);
    auto lines=chunks(lineFrame);
    require(prepareVitaDraw(*lines,0,prepared,failure) && prepared.profile==VitaFragmentProfile::Raster,
        "dithered snapshot eligibility does not silently broaden line topology");
}

void srcAlphaAddProfile() {
    const auto profile=VitaFragmentProfile(4);
    ShaderUid uid{}; uid.numTevStages=1; uid.alphaFunc=63;
    VitaFragmentLayout layout,legacy; std::string error;
    require(describeVitaFragment(uid,profile,layout,error) && layout.snapshotSampler==0 &&
        layout.snapshotSlot==4 && layout.pixel.vectors==5,
        "weighted additive SrcAlpha/One has its own exact snapshot profile");
    require(describeVitaFragment(uid,VitaFragmentProfile::Rgba6AddSnapshot,legacy,error) &&
        layout.pixel.slots==legacy.pixel.slots && layout.pixel.textureMask==legacy.pixel.textureMask,
        "weighted addition preserves existing ordered scratch resource slots");
    const auto source=genVitaFragmentShader(uid,profile);
    const auto blend=source.find("min(floor((outi*snapshotSrc+snapshotDestination*256.0)/256.0),vec4(255.0))");
    const auto dither=source.find("stored.rgb+=vec3(bayer)-floor(stored.rgb/64.0)");
    const auto overrideAlpha=source.find("stored.a=floor(u_pixel[4].w)");
    require(!source.empty() && source!=genVitaFragmentShader(uid,VitaFragmentProfile::Rgba6AddSnapshot) &&
        source!=genVitaFragmentShader(uid,VitaFragmentProfile::Rgba6SrcAlphaSnapshot) &&
        blend!=std::string::npos && blend<dither && dither<overrideAlpha &&
        overrideAlpha<source.find("stored=floor(stored/4.0)") &&
        source.find("if(u_raster[1].w>0.5)")!=std::string::npos && source.find("half")==std::string::npos,
        "weighted full-F32 source saturates byte blend before optional RGB-only dither and raw alpha override");
    auto fog=uid; fog.fogSel=4;
    require(!describeVitaFragment(fog,profile,layout,error) && genVitaFragmentShader(fog,profile).empty(),
        "weighted additive snapshot cannot broaden unverified fog ordering");
    auto full=uid; full.numTexGens=8; full.numTevStages=8;
    for(unsigned i=0;i<4;++i) full.tevOrder[i]=(64u+2*i)|((65u+2*i)<<12);
    require(!describeVitaFragment(full,profile,layout,error) && genVitaFragmentShader(full,profile).empty(),
        "weighted additive snapshot cannot alias a fully occupied original sampler set");
    PortVitaGxmDevice* device=nullptr; VitaMaterialCache* cache=nullptr;
    require(port_vita_gxm_device_create(&device)==0 && createVitaMaterialCache(device,&cache)==0,
        "weighted additive cache has actual native owners");
    VitaShaderLibrary library; installShaders(library,uid);
    const SceGxmProgram fs{8,3}; const auto* bytes=reinterpret_cast<const unsigned char*>(&fs);
    for(const auto variant:{VitaFragmentProfile::Rgba6AddSnapshot,profile})
        require(installVitaShader(library,{VitaShaderStage::Fragment,genVitaFragmentShader(uid,variant),
            std::vector<unsigned char>(bytes,bytes+8),0},error),"weighted and unweighted addition require separate exact shader artifacts");
    const PortVitaGxmBlend weighted{14,1,1,4,1,4,1},unweighted{14,1,1,1,1,1,1},effective{14,0,0,1,0,1,0};
    VitaMaterial *old=nullptr,*material=nullptr,*again=nullptr; std::shared_ptr<const void> oldLease,lease,againLease;
    require(resolveVitaMaterialProfile(cache,uid,unweighted,VitaFragmentProfile::Rgba6AddSnapshot,library,old,oldLease,error)==0 &&
        resolveVitaMaterialProfile(cache,uid,weighted,profile,library,material,lease,error)==0 && material!=old &&
        std::memcmp(&patched_blend,&effective,sizeof(effective))==0,
        "weighted addition has a distinct cache identity and is not blended a second time by GXM");
    const unsigned programs=shader_programs;
    require(resolveVitaMaterialProfile(cache,uid,weighted,profile,library,again,againLease,error)==0 &&
        again==material && programs==shader_programs,"identical weighted addition reuses its native program");
    require(describeVitaFragment(uid,profile,layout,error),"restore weighted additive layout");
    PortVitaGxmProgram* program=nullptr;
    require(bindVitaMaterialProfile(material,device,uid,weighted,profile,layout,program,error)==0 && program,
        "weighted addition binds only its exact reflected layout");
    require(bindVitaMaterialProfile(material,device,uid,unweighted,VitaFragmentProfile::Rgba6AddSnapshot,layout,program,error)<0 && !program,
        "weighted and unweighted addition cannot alias at bind time");
    VitaMaterial* rejected=nullptr; std::shared_ptr<const void> rejectedLease;
    auto wrong=weighted; wrong.alpha_dst=5;
    require(resolveVitaMaterialProfile(cache,uid,wrong,profile,library,rejected,rejectedLease,error)<0 && !rejected && !rejectedLease,
        "weighted addition rejects unimplemented alpha factors before creating a program");
    oldLease.reset(); lease.reset(); againLease.reset(); old=material=again=nullptr;
    require(releaseVitaMaterialCache(&cache)==0 && port_vita_gxm_device_release(&device)==0,
        "weighted additive resources retire through actual native ownership"); reflect_texture=false; empty_state();
    for(uint32_t word:{0x3429u,0x342du}) {
        auto original=frame(); original->startBp[0x41]=word; original->startBp[0x42]=0; original->startBp[0x43]=0x41;
        draw(*original,0x90,3); auto sourceChunks=chunks(original); VitaPreparedDraw prepared; VitaFrameError failure;
        require(prepareVitaDraw(*sourceChunks,0,prepared,failure) && prepared.profile==profile &&
            prepared.blend.color_src==4 && prepared.blend.color_dst==1 && prepared.blend.color_mask==14 &&
            prepared.raster.dither==bool(word&4) && !prepared.raster.alpha_override,
            "original SrcAlpha/One RGB-only words select the same exact weighted profile with original dither state");
        VitaShaderInventory inventory;
        require(inventoryVitaFrameShaders(*sourceChunks,{},inventory) && inventory.requests.size()==1 &&
            inventory.requests[0].profile==profile,"native inventory retains the weighted factor strategy");
        original->startBp[0x43]=0; sourceChunks=chunks(original);
        require(prepareVitaDraw(*sourceChunks,0,prepared,failure) && prepared.profile==VitaFragmentProfile::Raster,
            "weighted snapshot selection does not change original RGB8 fixed-function blending");
    }
}

void earlyAlphaRaster() {
    auto f=frame(); f->startBp[0x41]=0x312d; f->startBp[0x42]=0; f->startBp[0x43]=0x41;
    draw(*f,0x90,3); auto source=chunks(f); VitaPreparedDraw prepared; VitaFrameError error;
    require(prepareVitaDraw(*source,0,prepared,error) && prepared.profile==VitaFragmentProfile(2) &&
        prepared.blend.color_mask==14 && prepared.raster.dither && !prepared.raster.alpha_override,
        "actual boot61 BP state is additive dithered RGB-only, not an alpha update");
    for(uint32_t blend:{0x312du,0x54b9u}) {
        auto early=frame(); early->startBp[0x41]=blend; early->startBp[0x43]=0x41;
        early->startBp[0x40]=17; early->startBp[0xf3]=uint32_t(4|(7<<3))<<16|128;
        draw(*early,0x90,3); auto earlySource=chunks(early);
        require(!prepareVitaDraw(*earlySource,0,prepared,error) && !error.message.empty() && prepared.vertices.empty(),
            "early GX depth writes plus possible alpha discard fail closed instead of using late GXM writes");
        for(uint32_t alpha:{63u,7u|(4u<<3)|64u,7u|128u,192u}) {
            early->startBp[0xf3]=alpha<<16; earlySource=chunks(early);
            require(prepareVitaDraw(*earlySource,0,prepared,error),
                "structurally always-passing AND/OR/XOR/XNOR tests do not invent an early-depth rejection");
        }
        early->startBp[0xf3]=uint32_t(4|(7<<3))<<16|128;
        for(uint32_t depth:{1u,16u,0u}) {
            early->startBp[0x40]=depth; earlySource=chunks(early);
            require(prepareVitaDraw(*earlySource,0,prepared,error),
                "disabled GX early depth test or update keeps original byte-alpha discard support");
        }
        early->startBp[0x40]=17; early->startBp[0x43]=1; earlySource=chunks(early);
        require(prepareVitaDraw(*earlySource,0,prepared,error),"late GX depth writes remain after the original alpha test");
    }
}

void vitaOverlaps() {
    require(&partitionVitaTriangles,"production conservative snapshot overlap partitioner exists");
    VitaPreparedDraw draw{}; draw.topology=PORT_VITA_GXM_TRIANGLES;
    draw.raster.width=draw.raster.height=64; draw.raster.viewport[2]=draw.raster.viewport[3]=64;
    draw.raster.scissor[2]=draw.raster.scissor[3]=64; draw.vertices.resize(9);
    const float positions[9][2]={{2,2},{8,2},{2,8},{40,40},{48,40},{40,48},{6,6},{10,6},{6,10}};
    for(unsigned i=0;i<9;++i) {
        draw.vertices[i].clip[0]=positions[i][0]/32-1;
        draw.vertices[i].clip[1]=1-positions[i][1]/32;
        draw.vertices[i].clip[3]=1;
    }
    const uint16_t indices[]={0,1,2,3,4,5,6,7,8};
    std::vector<VitaOverlapGroup> groups; std::string error;
    require(partitionVitaTriangles(draw,indices,groups,error) && groups.size()==2 &&
        groups[0].firstIndex==0 && groups[0].indexCount==6 && groups[1].firstIndex==6 && groups[1].indexCount==3,
        "third triangle overlaps first, not just last: ordered ranges preserve all original primitives");
    require(groups[0].bounds[0]<2 && groups[0].bounds[1]<2 && groups[0].bounds[2]>48 && groups[0].bounds[3]>48 &&
        groups[0].bounds[0]>=0 && groups[0].bounds[2]<=64,
        "negative-Y viewport bounds conservatively include both separated triangles and a sample margin");
    const auto saved=draw.vertices;
    groups={{77,9,{1,2,3,4}}}; const uint16_t bad[]={0,1,2,3,4,99};
    require(!partitionVitaTriangles(draw,bad,groups,error) && !error.empty() && groups.size()==1 &&
        groups[0].firstIndex==77 && groups[0].bounds==std::array<int32_t,4>{1,2,3,4},
        "invalid later index cannot publish an earlier partial group");
    require(!partitionVitaTriangles(draw,std::span(indices,8),groups,error) && groups[0].firstIndex==77,
        "incomplete trailing triangle preserves prior output");
    draw.topology=PORT_VITA_GXM_LINES;
    require(!partitionVitaTriangles(draw,indices,groups,error),"triangle partitioner rejects another topology");
    draw.topology=PORT_VITA_GXM_TRIANGLES;
    draw.raster.viewport[0]=INFINITY;
    require(!partitionVitaTriangles(draw,indices,groups,error),"nonfinite viewport is an invalid raster contract");
    draw.raster.viewport[0]=0;
    for(unsigned kind=0;kind<4;++kind) {
        draw.vertices=saved;
        if(kind==0) draw.vertices[0].clip[3]=0;
        if(kind==1) draw.vertices[0].clip[2]=-2;
        if(kind==2) draw.vertices[0].clip[0]=NAN;
        if(kind==3) draw.vertices[0].clip[0]=1e30f;
        require(partitionVitaTriangles(draw,indices,groups,error) && groups.size()==2 && groups[0].indexCount==3 &&
            groups[0].bounds==std::array<int32_t,4>{0,0,64,64},
            "unreliable projection uses the full scissor, never an optimistic disappearing triangle");
    }
    draw.vertices=saved;
    draw.raster.scissor[0]=7; draw.raster.scissor[1]=9; draw.raster.scissor[2]=47; draw.raster.scissor[3]=41;
    require(partitionVitaTriangles(draw,indices,groups,error),"clipped footprint partition succeeds");
    for(const auto& group:groups) if(group.bounds[2]>group.bounds[0] && group.bounds[3]>group.bounds[1])
        require(group.bounds[0]>=7 && group.bounds[1]>=9 && group.bounds[2]<=47 && group.bounds[3]<=41,
            "snapshot footprint never exceeds the clamped immutable scissor");
    draw.raster.scissor[0]=47; draw.raster.scissor[2]=7;
    require(partitionVitaTriangles(draw,indices,groups,error) && groups.size()==1 && groups[0].indexCount==9 &&
        groups[0].bounds==std::array<int32_t,4>{},"empty scissor retains all ordered input with empty coverage");
    draw.raster.scissor[0]=draw.raster.scissor[1]=0; draw.raster.scissor[2]=draw.raster.scissor[3]=64;
    draw.raster.viewport[2]=0;
    require(partitionVitaTriangles(draw,indices,groups,error) && groups.size()==1 && groups[0].indexCount==9 &&
        groups[0].bounds==std::array<int32_t,4>{},"zero viewport is empty coverage without deleting geometry");
    draw.raster.viewport[2]=64;
    require(partitionVitaTriangles(draw,{},groups,error) && groups.empty(),"empty input publishes no invented draw group");
    require(std::memcmp(saved.data(),draw.vertices.data(),saved.size()*sizeof(saved[0]))==0,
        "partitioning never changes original transformed vertices");
    draw.raster.width=draw.raster.height=2048;
    draw.raster.viewport[2]=draw.raster.viewport[3]=2048; draw.raster.scissor[2]=draw.raster.scissor[3]=2048;
    draw.vertices.resize(129*3); std::vector<uint16_t> pressure;
    for(unsigned n=0;n<129;++n) for(unsigned corner=0;corner<3;++corner) {
        const unsigned i=n*3+corner; pressure.push_back(uint16_t(i));
        draw.vertices[i]={}; draw.vertices[i].clip[0]=(float(n*6+2+(corner==1))/1024)-1;
        draw.vertices[i].clip[1]=1-float(2+(corner==2))/1024; draw.vertices[i].clip[3]=1;
    }
    require(partitionVitaTriangles(draw,pressure,groups,error) && groups.size()==2 &&
        groups[0].indexCount==128*3 && groups[1].firstIndex==128*3 && groups[1].indexCount==3,
        "bounded 128-rectangle working set splits early instead of unbounded pairwise work");
}

// Catches repeated uploads, mutable sampler aliases, version/address confusion,
// premature cache eviction and GPU owner loss on teardown failure.
void textureCache() {
    require(createVitaTextureCache && resolveVitaImage && collectVitaTextures && releaseVitaTextureCache,
            "production immutable-image/native-sampler cache exists");
    PortVitaGxmDevice* device=nullptr; VitaTextureCache* cache=nullptr;
    require(port_vita_gxm_device_create(&device)==0 && createVitaTextureCache(device,&cache)==0,"native cache device lease");
    const unsigned empty_finishes=finishes;
    require(collectVitaTextures(cache)==0 && finishes==empty_finishes,
            "empty texture collection cannot stall the GPU without a resource to retire");
    auto image=std::make_shared<TexImage>(); image->width=8; image->height=8; image->levels=4;
    image->version=1001; image->rgba.assign(85,0xAABBCCDD);
    Frame f{}; f.textures.push_back(image); SpanTexture texture;
    texture.kind=SpanTextureKind::Image; texture.imageIndex=0; texture.width=texture.height=8;
    texture.levels=4; texture.version=1001; texture.mode0=6u<<5; texture.mode1=0x3000;
    PortVitaGxmTextureView *a=nullptr,*b=nullptr,*c=nullptr;
    std::shared_ptr<const void> al,bl,cl; std::string error;
    require(resolveVitaImage(cache,f,texture,a,al,error)==0 && a && al,"real immutable image uploaded/bound");
    PortVitaGxmTextureViewInfo ai{},bi{},ci{};
    require(port_vita_gxm_texture_view_info(a,&ai)==0 && ai.image.version==1001 && ai.image.storage_bytes==480,
            "complete 8x8 chain native cache storage");
    unsigned allocations=memory.size(), calls=sampler_calls;
    auto* alias=a; const unsigned before_raw_release=finishes;
    require(port_vita_gxm_texture_view_release(&alias)<0 && alias==a && al && finishes==before_raw_release &&
            port_vita_gxm_texture_view_info(a,&ai)==0,
            "raw view release cannot bypass actual cache/frame lifetime lease or invalidate a borrowed handle");
    int wrong_owner=0;
    require(port_vita_gxm_texture_view_release_owned(&alias,&wrong_owner)<0 && alias==a &&
            port_vita_gxm_texture_view_release_owned(&alias,nullptr)<0 && alias==a && finishes==before_raw_release,
            "private cache release also requires matching nonnull owner before any fence or mutation");
    require(resolveVitaImage(cache,f,texture,b,bl,error)==0 && a==b && sampler_calls==calls && memory.size()==allocations,
            "identical immutable image/sampler reuses actual native owner without upload/setters");
    bl.reset(); b=nullptr; texture.mode0=(5u<<5)|1u|8u;
    require(resolveVitaImage(cache,f,texture,b,bl,error)==0 && b!=a && memory.size()==allocations &&
            port_vita_gxm_texture_view_info(b,&bi)==0 && ai.image.pixels==bi.image.pixels && bi.sampler.wrap_s==1 &&
            ai.sampler.min_filter==6 && bi.sampler.min_filter==5,"different GX samplers share pixels without changing an earlier binding");
    auto replacement=std::make_shared<TexImage>(); replacement->width=replacement->height=8;
    replacement->levels=4; replacement->version=1002; replacement->rgba.assign(85,0x11223344);
    f.textures[0]=replacement; texture.version=1002;
    require(resolveVitaImage(cache,f,texture,c,cl,error)==0 && c!=b &&
            port_vita_gxm_texture_view_info(c,&ci)==0 && ci.image.pixels!=ai.image.pixels && ci.image.version==1002 &&
            *static_cast<const uint32_t*>(ai.image.pixels)==0xAABBCCDD && *static_cast<const uint32_t*>(ci.image.pixels)==0x11223344,
            "recorded replacement/version preserves earlier frame's original pixels");
    texture.version=1001; PortVitaGxmTextureView* bad=nullptr; std::shared_ptr<const void> lease;
    require(resolveVitaImage(cache,f,texture,bad,lease,error)<0 && !bad && !lease && !error.empty(),
            "stale recorded image metadata rejects before cache upload"); texture.version=1002;
    texture.kind=SpanTextureKind::EfbAddress;
    require(resolveVitaImage(cache,f,texture,bad,lease,error)<0 && !bad && !lease,
            "EFB address/history cannot be mistaken for ordinary immutable image"); texture.kind=SpanTextureKind::Image;
    texture.mode0=6u<<5; texture.mode1=0x2F00;
    require(resolveVitaImage(cache,f,texture,bad,lease,error)<0 && !bad && !lease && !error.empty(),
            "unsupported reachable fractional sampler propagates without a default binding");
    require(error.find("mode0=0000c0 mode1=002f00 image=8x8/4")!=std::string::npos &&
            error.find("native=")!=std::string::npos,
            "sampler capability failure reports actual original modes, uploaded geometry and native status");
    texture.mode0=0; texture.mode1=0x3010;
    sampler_fail_call=sampler_calls+1;
    require(resolveVitaImage(cache,f,texture,bad,lease,error)==-861 && !bad && !lease &&
            error.find("mode0=000000 mode1=003010 image=8x8/4 native=fffffca3")!=std::string::npos,
            "SDK descriptor failure retains exact native status instead of implying an unsupported game strategy");
    sampler_fail_call=0;
    allocations=memory.size();
    const unsigned leased_finishes=finishes;
    require(collectVitaTextures(cache)==0 && memory.size()==allocations && finishes==leased_finishes,
            "leased frame bindings cannot be evicted or trigger a redundant GPU retirement");
    require(port_vita_gxm_device_release(&device)<0 && device,"cache/image owners block premature device destruction");
    require(port_vita_gxm_device_begin_display(device)==0 && collectVitaTextures(cache)<0,
            "even empty cache collection cannot bypass an open native scene");
    require(port_vita_gxm_device_end(device,nullptr,0)==0,"cache native pending-work fixture");
    al.reset(); bl.reset(); cl.reset(); a=b=c=nullptr;
    unmap_error=-871;
    require(releaseVitaTextureCache(&cache)==-871 && cache && memory.size()==allocations,
            "fallible cache close retains image UID/device owner on failed unmap");
    require(resolveVitaImage(cache,f,texture,bad,lease,error)<0 && !bad && !lease,
            "closing recovery owner cannot issue new native bindings"); unmap_error=0;
    require(releaseVitaTextureCache(&cache)==0 && !cache && port_vita_gxm_device_release(&device)==0,
            "retry completes native cache/device teardown without a fallible shared_ptr deleter");
    f.textures.clear(); image.reset(); replacement.reset(); empty_state();
}

// Catches wrong remap/word offset, partial vertex formats, TEV packing or enum
// numeric casts. Expected clip, STQ, colors and blend scalars are hand-derived.
void baseMirrorSampling() {
    auto image=std::make_shared<TexImage>(); image->width=image->height=8;
    image->levels=1; image->version=8821; image->rgba.assign(64,0x78563412);
    auto f=frame(); f->textures.push_back(image);
    f->startXfRegs[0x3F]=1; f->startXfRegs[0x40]=6;
    f->startBp[0x28]=64; f->startBp[0x88]=7u|(7u<<10);
    command(*f,{CMD_TEX,0,0}); draw(*f,0x90,3);
    for(unsigned wrap:{2u,8u,10u}) for(unsigned diagonal:{0u,1u}) {
        const uint32_t mode=(4u<<5)|(1u<<4)|wrap|(diagonal<<8);
        f->startBp[0x80]=mode;
        auto source=chunks(f); VitaPreparedDraw prepared; VitaFrameError error;
        require(prepareVitaDraw(*source,0,prepared,error) && prepared.fragment.manualTextureMask==1 &&
            prepared.fragment.manualSlots[0]==5 && prepared.pixel[20]==0 && prepared.pixel[21]==0 &&
            prepared.pixel[22]==0 && prepared.pixel[23]==float((wrap&3)+4*((wrap>>2)&3)),
            "base-only mirrored bilinear sampler selects exact per-corner wrapping without implicit mirror");
        PortVitaGxmDevice* device=nullptr; PortVitaGxmTexture* texture=nullptr; PortVitaGxmTextureView* view=nullptr;
        require(port_vita_gxm_device_create(&device)==0 &&
            port_vita_gxm_texture_create(device,8,8,1,image->version,image->rgba.data(),256,&texture)==0 &&
            port_vita_gxm_texture_view_create_explicit_linear_mip(texture,mode,0,&view)==0,
            "base-only manual sampler has real native image/view owners");
        PortVitaGxmTextureViewInfo info{}; const void* descriptor=nullptr;
        require(port_vita_gxm_texture_view_info(view,&info)==0 && info.explicit_linear_mip &&
            info.sampler.mode0==mode && info.image.levels==1 && info.native_max_lod==0 &&
            port_vita_gxm_texture_view_descriptor(view,device,&descriptor)==0,
            "base-only manual view retains exact original words and sole image level");
        const auto* native=static_cast<const SceGxmTexture*>(descriptor);
        require(native->generic.uaddr_mode==SCE_GXM_TEXTURE_ADDR_CLAMP &&
            native->generic.vaddr_mode==SCE_GXM_TEXTURE_ADDR_CLAMP &&
            native->generic.min_filter==SCE_GXM_TEXTURE_FILTER_POINT &&
            native->generic.mag_filter==SCE_GXM_TEXTURE_FILTER_POINT && !native->generic.mip_filter,
            "mirrored bilinear shader fetches four clamped native point corners, never hardware MIRROR");
        require(port_vita_gxm_texture_view_release(&view)==0 && port_vita_gxm_texture_release(&texture)==0 &&
            port_vita_gxm_device_release(&device)==0,"base-only manual sampler retires its real GPU owners");
        empty_state();
    }
}

void preparation() {
    auto f=frame(); f->verts.assign(11,0xDEADBEEF); draw(*f,0xB8,200003);
    auto original=chunks(f);
    auto d=std::make_shared<DrawSpan>(original->sourceSpans()->draws()[0]);
    auto s=std::make_shared<DrawSpanState>(*d->state); d->state=s;
    d->indices={200002,17,200002,65535,17,0};
    s->uid.numTexGens=8; s->uid.numChans=2;
    for (unsigned i=0;i<8;++i) {
        s->uid.texMtxInfo[i]=(1u<<1)|(1u<<2); // position -> STQ using matrix zero
        s->xf.matrixA=0; s->xf.matrixB=0;
    }
    s->xf.projection.depthParams[0]=0; s->xf.projection.depthParams[1]=8388607.5f;
    s->xf.lights.matColor[0][0]=0.25f; s->xf.lights.matColor[0][3]=0.75f;
    s->xf.lights.matColor[1][1]=0.5f; s->xf.lights.matColor[1][3]=1;
    s->raster.colorMask=9; s->raster.blend=true;
    s->raster.src=SpanBlendFactor::DstColor; s->raster.dst=SpanBlendFactor::SrcAlpha;
    auto source=single(f,d); VitaPreparedDraw out; VitaFrameError error;
    require(prepareVitaDraw(*source,0,out,error),"native sparse full-XF preparation");
    require(out.vertices.size()==4 && out.layout.vectors==4 && out.topology==0,"sparse only first-use vertices and GXM points");
    const float x[]={200003,18,65536,1};
    for (unsigned i=0;i<4;++i) {
        const auto& v=out.vertices[i];
        require(v.clip[0]==x[i] && v.clip[1]==2 && v.clip[2]==0 && v.clip[3]==1 &&
                v.view_z==-3 && v.gx_z==0.5f,"literal sparse clip/depth payload");
        for (unsigned t=0;t<8;++t) require(v.tex[t][0]==x[i] && v.tex[t][1]==2 && v.tex[t][2]==-3,"all eight full STQ outputs");
        require(v.color[0][0]==0.25f && v.color[0][3]==0.75f && v.color[1][1]==0.5f && v.color[1][3]==1,"both XF colors");
    }
    require(out.blend.color_mask==3 && out.blend.color_src==6 && out.blend.color_dst==4 &&
            out.blend.alpha_src==6 && out.blend.alpha_dst==4 && out.blend.color_func==1,"RGBA mask and nonmatching native factor order");
    require(out.raster.width==640 && out.raster.height==456 && !out.raster.alpha_override,"original EFB and no false destination-alpha override");
    for (unsigned r=0;r<4;++r) for (unsigned j=0;j<4;++j) s->pixel.tevReg[r][j]=float(r*4+j+1)/32;
    source=single(f,d); require(prepareVitaDraw(*source,0,out,error),"packed exact TEV entry values");
    for (unsigned j=0;j<16;++j) require(out.pixel[j]==float(j+1)/32,"literal compact TEV float order");
    const unsigned factors[]={0,1,2,3,6,7,4,5,8,9};
    for (unsigned i=0;i<10;++i) {
        s->raster.src=static_cast<SpanBlendFactor>(i); s->raster.dst=static_cast<SpanBlendFactor>(9-i);
        source=single(f,d); require(prepareVitaDraw(*source,0,out,error) && out.blend.color_src==factors[i] &&
            out.blend.color_dst==factors[9-i],"all native blend factor mappings");
    }
    s->raster.equation=SpanBlendEquation::ReverseSubtract; source=single(f,d);
    require(prepareVitaDraw(*source,0,out,error) && out.blend.color_func==3 && out.blend.alpha_func==3,"native reverse subtraction");
    s->raster.blend=false; source=single(f,d);
    require(prepareVitaDraw(*source,0,out,error) && out.blend.color_func==0 && out.blend.color_src==1 && out.blend.color_dst==0,"disabled blend ignores stale factors");
    s->xf.matrixA=64-1; s->uid.vtxFlags=VF_POSMTX; d->vtxFlags=VF_POSMTX; d->strideWords=4;
    // A separate well-formed recorded range with a bad decoded matrix ordinal.
    auto bad=frame(); bad->verts={bits(1),bits(2),bits(-3),64};
    d->firstWord=0; d->vertexCount=1; d->indices={0}; source=single(bad,d);
    require(!prepareVitaDraw(*source,0,out,error) && out.vertices.empty() &&
            error.drawIndex==123 && error.sourceEventIndex==456 && error.commandWord==d->commandWord &&
            error.vertexOrdinal==0 && !error.message.empty(),"transform failure clears output with source provenance");
    require(!prepareVitaDraw(*source,99,out,error) && out.vertices.empty(),"chunk bounds fail transactionally");
}

void drawResolver() {
    PortVitaGxmDevice* device=nullptr; VitaMaterialCache* materials=nullptr; VitaTextureCache* textures=nullptr;
    require(port_vita_gxm_device_create(&device)==0 && createVitaMaterialCache(device,&materials)==0 &&
        createVitaTextureCache(device,&textures)==0,"draw resolver uses actual device/material/image owners");
    auto image=std::make_shared<TexImage>(); image->width=image->height=8; image->levels=4;
    image->version=1991; image->rgba.assign(85,0x78563412);
    auto f=frame(); f->textures.push_back(image);
    f->startXfRegs[0x3F]=1; f->startXfRegs[0x40]=6;
    f->startBp[0]=1u<<10; f->startBp[0x28]=64u|((64u|7u)<<12);
    f->startBp[0x80]=f->startBp[0xA3]=6u<<5;
    f->startBp[0x84]=f->startBp[0xA7]=0x3000;
    f->startBp[0x88]=f->startBp[0xAB]=7u|(7u<<10);
    command(*f,{CMD_TEX,0,0}); command(*f,{CMD_TEX,7,0}); draw(*f,0x90,3);
    auto source=chunks(f); VitaPreparedDraw prepared; VitaFrameError frame_error;
    require(prepareVitaDraw(*source,0,prepared,frame_error) && prepared.layout.textureMask==0x81,
            "real immutable two-stage draw uses sparse texture units zero and seven");
    VitaShaderLibrary library,missing; installShaders(library,source->chunks()[0].state->uid);
    VitaDrawBinding binding; std::string error; unsigned registrations=shader_registrations;
    auto bad=prepared; bad.blend.alpha_dst^=1;
    require(resolveVitaDraw(device,materials,textures,library,*source,source->chunks()[0],bad,binding,error)<0 &&
        !binding.material && !binding.lifetime && shader_registrations==registrations,
        "wrong prepared blend rejects before native material allocation");
    bad=prepared; bad.layout.slots[0]^=1;
    require(resolveVitaDraw(device,materials,textures,library,*source,source->chunks()[0],bad,binding,error)<0 &&
        !binding.material && !binding.lifetime && shader_registrations==registrations,
        "wrong compact uniform layout rejects without a partially published resource");
    require(resolveVitaDraw(device,materials,textures,missing,*source,source->chunks()[0],prepared,binding,error)<0 &&
        !binding.material && !binding.lifetime && !error.empty(),"missing compiler artifact has no draw fallback");
    require(resolveVitaDraw(device,materials,textures,library,*source,source->chunks()[0],prepared,binding,error)==0 &&
        binding.material && binding.lifetime && binding.views[0] && binding.views[7] && !binding.views[1],
        "production resolver joins all UID-used samplers, not only unit zero");
    unsigned allocations=memory.size(); auto lease=binding.lifetime;
    require(resolveVitaDraw(device,materials,textures,library,*source,source->chunks()[0],prepared,binding,error)<0 &&
        binding.lifetime==lease && memory.size()==allocations,"resolver never replaces a potentially in-flight binding");
    require(collectVitaMaterials(materials)==0 && collectVitaTextures(textures)==0 && memory.size()==allocations,
        "one draw lease pins both real material and all native image/sampler owners");
    binding={}; lease.reset();
    f=frame(); f->textures.push_back(image);
    f->startXfRegs[0x3F]=1; f->startXfRegs[0x40]=6;
    f->startBp[0]=1u<<10; f->startBp[0x28]=64u|((64u|7u)<<12);
    f->startBp[0x80]=f->startBp[0xA3]=6u<<5;
    f->startBp[0x84]=0x3000; f->startBp[0xA7]=0x2F00;
    f->startBp[0x88]=f->startBp[0xAB]=7u|(7u<<10);
    command(*f,{CMD_TEX,0,0}); command(*f,{CMD_TEX,7,0}); draw(*f,0x90,3);
    source=chunks(f);
    require(prepareVitaDraw(*source,0,prepared,frame_error),"partial sampler resolution failure fixture");
    require(resolveVitaDraw(device,materials,textures,library,*source,source->chunks()[0],prepared,binding,error)<0 &&
        !binding.material && !binding.lifetime && !binding.views[0] && !binding.views[7] && !error.empty(),
        "later unsupported sampler publishes no earlier material/view and drops only CPU leases");
    require(releaseVitaMaterialCache(&materials)==0 && releaseVitaTextureCache(&textures)==0 &&
        port_vita_gxm_device_release(&device)==0,"failed draw resolution keeps native owners explicitly collectible");
    source.reset(); f.reset(); image.reset(); empty_state();
}

struct Backend final : VitaFrameBackend {
    PortVitaGxmDevice* device;
    PortVitaGxmSurface* efb;
    std::vector<std::string> order;
    std::vector<unsigned> draw_counts;
    std::weak_ptr<const void> last_lease;
    int resolve_error=0,copy_error=0,marker_error=0,surface_error=0;
    bool missing_lease=false;
    bool omit_material=false;
    bool omit_view=false;
    bool bypass_sampler_cache=false;
    VitaMaterialCache* material_cache=nullptr;
    VitaShaderLibrary library;
    VitaMaterial* override_material=nullptr;
    VitaTextureCache* texture_cache=nullptr;
    VitaEfbCache* efb_cache=nullptr;
    PortVitaGxmTextureView* override_view=nullptr;
    std::weak_ptr<const VitaEfbBlendSnapshot> last_snapshot;
    VitaEfbBlendSnapshotView last_snapshot_view;
    Backend(PortVitaGxmDevice* d,PortVitaGxmSurface* s):device(d),efb(s) {
        require(createVitaMaterialCache(d,&material_cache)==0,"frame backend owns real material cache");
        installClearShader(library); std::string error;
        require(createVitaEfbCache(d,library,&efb_cache,error)==0,"frame backend owns real native EFB surface cache");
    }
    int resolve(const IndexChunks& source,const IndexChunk& chunk,const VitaPreparedDraw& prepared,
                VitaDrawBinding& binding,std::string& error) override {
        require(!active_scene && source.frame().number==127 && chunk.state &&
                prepared.layout.vectors==(prepared.layout.textureMask ? 5u : 4u),"resource resolve outside native scene with immutable state");
        order.push_back("resolve");
        if (resolve_error) return resolve_error;
        installShaders(library,chunk.state->uid);
        const auto snapshotSource=genVitaFragmentShader(chunk.state->uid,prepared.profile,prepared.fragment.manualTextureMask);
        if ((prepared.profile!=VitaFragmentProfile::Raster || prepared.fragment.manualTextureMask) && !snapshotSource.empty()) {
            const SceGxmProgram fs{8,prepared.fragment.manualTextureMask ? 5u : 3u}; const auto* bytes=reinterpret_cast<const unsigned char*>(&fs);
            require(installVitaShader(library,{VitaShaderStage::Fragment,snapshotSource,
                std::vector<unsigned char>(bytes,bytes+8),0},error),"frame fixture installs exact typed snapshot source");
        }
        const int result=bypass_sampler_cache ?
            resolveVitaMaterialProfile(material_cache,chunk.state->uid,prepared.blend,prepared.profile,library,
                binding.material,binding.lifetime,error) :
            resolveVitaDraw(device,material_cache,texture_cache,library,source,chunk,prepared,binding,error);
        if (result<0) return result;
        if (bypass_sampler_cache)
            binding.lifetime=std::make_shared<std::array<std::shared_ptr<const void>,1>>(
                std::array<std::shared_ptr<const void>,1>{std::move(binding.lifetime)});
        if (omit_material) binding.material=nullptr;
        else if (override_material) binding.material=override_material;
        if (omit_view) binding.views[0]=nullptr;
        else if (override_view) binding.views[0]=override_view;
        if (missing_lease) binding.lifetime.reset();
        last_lease=binding.lifetime;
        return 0;
    }
    int surface(uint32_t w,uint32_t h,PortVitaGxmSurface*& out,std::shared_ptr<const void>& lease,std::string&) override {
        require(!active_scene && w==640 && h==456,"full native EFB requested scene-closed"); order.push_back("surface");
        std::string error; const int result=resolveVitaEfb(efb_cache,w,h,out,lease,error);
        return result<0 ? result : surface_error;
    }
    int copy(const Frame& f,const SpanCopy& copy,std::string&) override {
        require(!active_scene && !pending_gpu && f.number==127 && copy.words[0]==17 && last_lease.expired(),"exact copy only after prior draws are fenced and leases retired");
        order.push_back("copy"); draw_counts.push_back(native_draws); return copy_error;
    }
    int snapshot(PortVitaGxmSurface* source,uint32_t width,uint32_t height,
                 std::shared_ptr<const VitaEfbBlendSnapshot>& out,std::string& error) override {
        require(!active_scene,"snapshot resources resolve before any producer scene");
        const int result=resolveVitaEfbBlendSnapshot(efb_cache,source,out,error);
        if (result<0) return result;
        last_snapshot=out;
        return bindVitaEfbBlendSnapshot(out.get(),device,source,width,height,last_snapshot_view,error);
    }
    int marker(const Frame&,const SpanMarker&,std::string&) override {
        if (native_draws && last_vertex_stream) {
            const auto* vertices=static_cast<const PortVitaGxmVertex*>(last_vertex_stream);
            const auto* indices=static_cast<const uint16_t*>(last_draw_indices);
            require(vertices[0].clip[0]>=1 && vertices[0].clip[1]==2 && vertices[0].view_z==-3 && indices[0]==0,
                    "full transformed vertices and local indices actually reach the native stream");
        }
        order.push_back("marker"); draw_counts.push_back(native_draws); return marker_error;
    }
    int camera(const Frame&,const SpanCamera&,std::string&) override {
        order.push_back("camera"); draw_counts.push_back(native_draws); return 0;
    }
};
struct SnapshotObservation {
    const void* vertices;
    const void* target;
    const void* sampler;
    std::vector<uint16_t> indices;
    std::array<float,28> uniforms;
    unsigned finished;
};
static std::vector<SnapshotObservation> snapshotObserved;
void frameSnapshots(uint32_t blendWord=0x54b9) {
    require(&resolveVitaEfbBlendSnapshot && &bindVitaEfbBlendSnapshot,
        "production persistent leased EFB blend snapshot binding exists");
    require(&port_vita_gxm_staging_slice,"production staging derives validated aligned original index ranges");
    PortVitaGxmDevice* device=nullptr; VitaFrameReplay* replay=nullptr;
    require(port_vita_gxm_device_create(&device)==0 && createVitaFrameReplay(device,&replay)==0,
        "ordered frame snapshot test owns real device and staging");
    PortVitaGxmStaging* ranges=nullptr; PortVitaGxmSlice parent{},sub{};
    const PortVitaGxmVertex vertices[6]{}; const uint16_t padded[]={0,1,2,0,3,4,5};
    require(port_vita_gxm_staging_create(device,4096,&ranges)==0 &&
        port_vita_gxm_staging_upload(ranges,vertices,6,padded,7,&parent)==0,
        "index range validator begins with an actual upload and explicit unused alignment pad");
    unsigned rangeFinishes=finishes;
    require(port_vita_gxm_staging_slice(ranges,&parent,4,3,&sub)==0 && sub.block==parent.block &&
        sub.epoch==parent.epoch && sub.vertex_offset==parent.vertex_offset && sub.vertex_count==6 &&
        sub.index_offset==parent.index_offset+8 && sub.index_count==3 && finishes==rangeFinishes,
        "validated aligned range preserves original vertex ownership without a hidden upload or Finish");
    require(port_vita_gxm_staging_slice(ranges,&parent,3,3,&sub)<0 && !sub.block &&
        port_vita_gxm_staging_slice(ranges,&parent,6,2,&sub)<0 && !sub.block &&
        port_vita_gxm_staging_slice(ranges,&parent,0,0,&sub)<0 && !sub.block,
        "unaligned, out-of-range and empty index ranges publish no staging slice");
    auto forged=parent; forged.index_offset=parent.vertex_offset;
    require(port_vita_gxm_staging_slice(ranges,&forged,0,3,&sub)<0 && !sub.block,
        "index subrange cannot reinterpret its own uploaded vertex storage as indices");
    require(port_vita_gxm_staging_reset(ranges)==0 && port_vita_gxm_staging_slice(ranges,&parent,0,3,&sub)<0 &&
        !sub.block && port_vita_gxm_staging_release(&ranges)==0,"retired upload epochs cannot issue new index ranges");
    Backend backend(device,nullptr); std::string detail;
    const SceGxmProgram fs{8,3}; const auto* bytes=reinterpret_cast<const unsigned char*>(&fs);
    // The cache captures its exact artifact at creation, so replace the fixture cache.
    require(releaseVitaEfbCache(&backend.efb_cache)==0 &&
        installVitaShader(backend.library,{VitaShaderStage::Fragment,genVitaEfbSnapshotFragmentShader(false),
            std::vector<unsigned char>(bytes,bytes+8),0},detail) &&
        createVitaEfbCache(device,backend.library,&backend.efb_cache,detail)==0,"frame snapshot uses exact copy artifact");
    auto f=frame(); f->startBp[0x41]=blendWord; f->startBp[0x42]=0x100; f->startBp[0x43]=1;
    draw(*f,0x90,6);
    const float xy[6][2]={{-1,-1},{3,-1},{-1,3},{-1,-1},{3,-1},{-1,3}};
    for(unsigned i=0;i<6;++i) { f->verts[i*3]=bits(xy[i][0]); f->verts[i*3+1]=bits(xy[i][1]); }
    auto source=chunks(f); VitaFrameError error;
    if (blendWord==0x313d) {
        VitaPreparedDraw prepared;
        require(prepareVitaDraw(*source,0,prepared,error) && prepared.profile==VitaFragmentProfile(2) &&
            prepared.raster.rgba6 && prepared.raster.dither && prepared.blend.color_src==1 &&
            prepared.blend.color_dst==1 && prepared.blend.color_mask==15,
            "original additive+dither BP word selects its typed profile with exact update masks");
    }
    if (blendWord==0x34ad) {
        VitaPreparedDraw prepared;
        require(prepareVitaDraw(*source,0,prepared,error) && prepared.profile==VitaFragmentProfile(3) &&
            prepared.raster.dither && prepared.blend.color_mask==14,
            "actual blocking SrcAlpha/dither RGB-only frame reaches its ordered profile");
    }
    if (blendWord==0x3429 || blendWord==0x343d) {
        VitaPreparedDraw prepared;
        require(prepareVitaDraw(*source,0,prepared,error) && prepared.profile==VitaFragmentProfile(4) &&
            prepared.blend.color_src==4 && prepared.blend.color_dst==1 &&
            prepared.raster.dither==bool(blendWord&4),
            "weighted additive frames reach original ordered geometry with exact logical factors");
    }
    draw_result=[] {
        SnapshotObservation seen{last_vertex_stream,active_color_data,last_sampled_descriptor,{}, {},finishes};
        const auto* indices=static_cast<const uint16_t*>(last_draw_indices);
        seen.indices.assign(indices,indices+last_draw_count);
        std::copy_n(default_uniforms,seen.uniforms.size(),seen.uniforms.begin());
        snapshotObserved.push_back(std::move(seen));
    };
    const unsigned before=native_draws,ends=scene_ends;
    require(submitVitaFrame(replay,source,backend,error) && native_draws==before+4 && scene_ends==ends+4 &&
        !active_scene && !pending_gpu,"actual boot57 blend state reaches two fresh snapshots and both original triangle groups");
    require(snapshotObserved.size()==4 && snapshotObserved[0].target==snapshotObserved[2].target &&
        snapshotObserved[1].target==snapshotObserved[3].target && snapshotObserved[0].target!=snapshotObserved[1].target,
        "frame alternates a separate persistent scratch and the real EFB, never self-sampling");
    require(snapshotObserved[1].vertices==snapshotObserved[3].vertices &&
        snapshotObserved[1].indices==std::vector<uint16_t>({0,1,2}) &&
        snapshotObserved[3].indices==std::vector<uint16_t>({3,4,5}),
        "one original vertex upload supplies exact whole-triangle ordered ranges with aligned U16 padding");
    for(const auto& seen:snapshotObserved) require(seen.finished==snapshotObserved.front().finished,
        "no Finish, descriptor creation or staging reset is inserted between ordered snapshot groups");
    const auto view=backend.last_snapshot_view;
    require(snapshotObserved[0].sampler==view.sourceSample.color_texture &&
        snapshotObserved[1].sampler==view.targetSample.color_texture &&
        snapshotObserved[1].uniforms[16]==float(view.targetSample.color_width) &&
        snapshotObserved[1].uniforms[17]==float(view.targetSample.color_height) &&
        snapshotObserved[1].uniforms[18]==float(bool(blendWord&16)) && snapshotObserved[1].uniforms[19]==0 &&
        backend.last_snapshot.expired() && backend.last_lease.expired(),
        "original alpha byte and actual padded sampled extent reach shader; leases retire only after final fence");
    const unsigned allocations=memory.size(),programs=shader_programs;
    const void* scratchColor=snapshotObserved.front().target;
    snapshotObserved.clear();
    require(submitVitaFrame(replay,source,backend,error) && memory.size()==allocations && shader_programs==programs &&
        snapshotObserved.size()==4 && snapshotObserved.front().target==scratchColor,
        "next real frame reuses the same full-size scratch, program and staging without per-group allocation");
    // Failed EndScene must retain the snapshot and original draw resources without replay.
    draw_result=[] { end_error=-892; };
    const unsigned failedBefore=native_draws;
    require(!submitVitaFrame(replay,source,backend,error) && error.nativeCode==-892 && active_scene &&
        !backend.last_snapshot.expired() && !backend.last_lease.expired(),"failed snapshot EndScene keeps exact resource ownership");
    require(!submitVitaFrame(replay,source,backend,error),"partially submitted snapshot frame cannot replay automatically");
    draw_result=nullptr; end_error=0;
    require(discardVitaFrame(replay)==0 && native_draws==failedBefore+1 && backend.last_snapshot.expired() &&
        backend.last_lease.expired(),"snapshot discard closes/fences without reissuing original geometry or copy");
    require(releaseVitaFrameReplay(&replay)==0 && releaseVitaMaterialCache(&backend.material_cache)==0,
        "ordered frame and material resources retire before separate cache ownership tests");
    PortVitaGxmSurface* efb=nullptr; std::shared_ptr<const void> efbLease;
    std::shared_ptr<const VitaEfbBlendSnapshot> proof,again; VitaEfbBlendSnapshotView checked;
    require(backend.surface(640,456,efb,efbLease,detail)==0 &&
        resolveVitaEfbBlendSnapshot(backend.efb_cache,efb,proof,detail)==0 &&
        bindVitaEfbBlendSnapshot(proof.get(),device,efb,640,456,checked,detail)==0,
        "opaque scratch binding pins both original EFB and real copy program outside scenes");
    unsigned finished=finishes;
    require(resolveVitaEfbBlendSnapshot(backend.efb_cache,efb,again,detail)==0 && again==proof && finishes==finished,
        "same retained source lease is memoized without a second descriptor Finish or allocation");
    require(bindVitaEfbBlendSnapshot(proof.get(),device,efb,639,456,checked,detail)<0 && !checked.target && !checked.program &&
        bindVitaEfbBlendSnapshot(proof.get(),nullptr,efb,640,456,checked,detail)<0 && !checked.target,
        "wrong immutable extent or device publishes no compatible snapshot view");
    std::shared_ptr<const VitaEfbBlendSnapshot> wrong;
    require(resolveVitaEfbBlendSnapshot(backend.efb_cache,view.target,wrong,detail)<0 && !wrong,
        "scratch cannot masquerade as a mutable original EFB source");
    require(releaseVitaEfbCache(&backend.efb_cache)<0 && backend.efb_cache,
        "outstanding snapshot lease prevents cache-owned copy program and planes from being released");
    PortVitaGxmProgramInfo info{};
    require(port_vita_gxm_device_program_info(device,view.program,&info)==0 && info.texture_mask==1,
        "failed cache close leaves the actual leased native copy program alive");
    proof.reset(); again.reset(); efbLease.reset();
    require(releaseVitaEfbCache(&backend.efb_cache)==0 && port_vita_gxm_device_release(&device)==0,
        "persistent snapshot cache, programs and frame staging release through original owners");
    source.reset(); f.reset(); snapshotObserved.clear(); reflect_texture=false; empty_state();
}
void traversal() {
    PortVitaGxmDevice* device=nullptr; PortVitaGxmSurface* efb=nullptr; PortVitaGxmProgram* program=nullptr;
    require(port_vita_gxm_device_create(&device)==0 && port_vita_gxm_surface_create(device,640,456,&efb)==0,"native frame resources");
    SceGxmProgram vs{8,0},fs{8,1}; PortVitaGxmProgramInfo info{}; PortVitaGxmBlend blend{};
    require(port_vita_gxm_device_program_create(device,&vs,8,&fs,8,&blend,4,0,&program,&info)==0,"real raster-aware program");
    VitaFrameReplay* replay=nullptr;
    require(createVitaFrameReplay(device,&replay)==0 && replay,"real native frame replay staging owner");
    Backend backend(device,efb); VitaFrameError error;
    struct RasterStop { uint32_t blend,alpha,format; const char* registers; };
    const RasterStop stops[]={
        {13,0,1,"bp41=00000d bp42=000000 bp43=000001"},
        {9,0,1,"bp41=000009 bp42=000000 bp43=000001"},
        {12,0,2,"bp41=00000c bp42=000000 bp43=000002"},
        {25,384,1,"bp41=000019 bp42=000180 bp43=000001"},
        {24586,0,0,"bp41=00600a bp42=000000 bp43=000000"}
    };
    for (const auto& stop:stops) {
        auto rejected=frame(); rejected->startBp[0x41]=stop.blend;
        rejected->startBp[0x42]=stop.alpha; rejected->startBp[0x43]=stop.format;
        command(*rejected,{CMD_MARKER,MARK_PLAYER_BEGIN}); draw(*rejected,0x90,3);
        const unsigned drawsBefore=native_draws,endsBefore=scene_ends;
        auto rejectedSource=chunks(rejected);
        require(!submitVitaFrame(replay,rejectedSource,backend,error) && !active_scene &&
                native_draws==drawsBefore && scene_ends==endsBefore && backend.order.empty(),
                "unsupported native raster retains its actual state before resolving resources or drawing");
        require(error.message.find(stop.registers)!=std::string::npos && error.sourceEventIndex==1 &&
                error.commandWord==2 && error.drawIndex==0,
                "native raster rejection reports original BP registers with draw-command provenance");
        require(discardVitaFrame(replay)==0,"unsupported raster replay retires without native work");
    }
    for (uint32_t control:{1u,0x41u}) for (uint32_t blend:{0x18u,0xf11cu}) {
        auto rgba6=frame(); rgba6->startBp[0x41]=blend; rgba6->startBp[0x43]=control;
        draw(*rgba6,0x90,3); auto rgba6Source=chunks(rgba6);
        const unsigned rgba6Before=native_draws;
        require(submitVitaFrame(replay,rgba6Source,backend,error) && native_draws==rgba6Before+1 && !active_scene,
                "original unblended RGBA6 draw reaches actual raster-aware native submission without dropping its dither");
        require(default_uniforms[22]==1 && default_uniforms[23]==float((blend>>2)&1) &&
                last_draw_topology==SCE_GXM_PRIMITIVE_TRIANGLES && last_draw_count==3 &&
                static_cast<const uint16_t*>(last_draw_indices)[2]==2 &&
                static_cast<const PortVitaGxmVertex*>(last_vertex_stream)[2].clip[0]==3,
                "RGBA6 storage/dither flags reach existing reflected uniforms with complete original geometry");
    }
    for (uint32_t control:{1u,0x41u}) for (uint32_t mode:{0x10u,0x18u,0x501cu}) for (unsigned alpha=0;alpha<256;++alpha) {
        auto destination=frame(); destination->startBp[0x41]=mode;
        destination->startBp[0x42]=0x100|alpha; destination->startBp[0x43]=control;
        draw(*destination,0x90,3); const unsigned before=native_draws;
        auto destinationSource=chunks(destination); VitaPreparedDraw prepared;
        require(prepareVitaDraw(*destinationSource,0,prepared,error) &&
                prepared.blend.color_mask==(mode==0x10 ? 1u : 15u) && prepared.blend.color_func==0,
                "destination-alpha preparation preserves alpha-only and all-color write masks without enabling blending");
        require(submitVitaFrame(replay,destinationSource,backend,error) && native_draws==before+1 && !active_scene,
                "original unblended destination-alpha write submits without dropping alpha-only or dithered draws");
        const unsigned stored=(alpha&0xfc)|(alpha>>6);
        require(default_uniforms[20]==1 && default_uniforms[21]==float(stored)/255 &&
                default_uniforms[22]==1 && default_uniforms[23]==float((mode>>2)&1) &&
                last_draw_count==3 && static_cast<const uint16_t*>(last_draw_indices)[2]==2,
                "all destination-alpha bytes reach six-bit bit-expanded final override without changing color masks or original geometry");
    }
    for (uint32_t mode:{0u,8u}) {
        auto masked=frame(); masked->startBp[0x41]=mode;
        masked->startBp[0x42]=0x1ff; masked->startBp[0x43]=1; draw(*masked,0x90,3);
        auto maskedSource=chunks(masked); VitaPreparedDraw prepared;
        require(prepareVitaDraw(*maskedSource,0,prepared,error) && prepared.blend.color_mask==(mode ? 14u : 0u),
                "alpha-disabled destination-alpha preparation retains the original color write mask");
        require(submitVitaFrame(replay,maskedSource,backend,error) && !active_scene &&
                default_uniforms[20]==0 && default_uniforms[21]==0,
                "destination-alpha disabled by alpha write mask cannot leak the preceding override into color-only draws");
    }
    auto fogged=frame(); fogged->startBp[0x41]=0x18; fogged->startBp[0x43]=1;
    fogged->startBp[0xf1]=4u<<20; draw(*fogged,0x90,3);
    const unsigned beforeFog=native_draws; const auto orderBeforeFog=backend.order;
    require(!submitVitaFrame(replay,chunks(fogged),backend,error) && native_draws==beforeFog &&
            !active_scene && backend.order==orderBeforeFog && error.message.find("bpf1=400000")!=std::string::npos,
            "RGBA6 fog cannot silently use an ambiguous dither/fog order or skip fog during native resolution");
    require(discardVitaFrame(replay)==0,"RGBA6 fog stop retires without drawing or resolving a material");
    for (uint32_t control:{0u,0x40u}) {
        auto rgb8=frame(); rgb8->startBp[0x41]=0x501c;
        rgb8->startBp[0x42]=0x100; rgb8->startBp[0x43]=control;
        draw(*rgb8,0x90,3); auto rgb8Source=chunks(rgb8);
        const unsigned rgb8Before=native_draws;
        require(submitVitaFrame(replay,rgb8Source,backend,error) && native_draws==rgb8Before+1 && !active_scene,
                "original RGB8 low-three-bit format ignores only inapplicable dither and destination alpha");
        require(default_uniforms[20]==0 && !error.nativeCode && error.message.empty() &&
                last_draw_topology==SCE_GXM_PRIMITIVE_TRIANGLES && last_draw_count==3 &&
                static_cast<const uint16_t*>(last_draw_indices)[0]==0 &&
                static_cast<const uint16_t*>(last_draw_indices)[1]==1 &&
                static_cast<const uint16_t*>(last_draw_indices)[2]==2 &&
                static_cast<const PortVitaGxmVertex*>(last_vertex_stream)[0].clip[0]==1 &&
                static_cast<const PortVitaGxmVertex*>(last_vertex_stream)[2].clip[0]==3,
                "RGB8 preserves alpha output and submits all original triangle indices and representative vertices");
    }
    backend.order.clear(); backend.draw_counts.clear();
    const unsigned firstDraw=native_draws,firstRaster=native_rasters;
    auto f=frame(); draw(*f,0x90,3); command(*f,{CMD_MARKER,MARK_PLAYER_BEGIN});
    command(*f,{CMD_CAMERA}); f->cmds.insert(f->cmds.end(),kCameraWords,0x12345678);
    command(*f,{CMD_EFB_COPY,17,0,0,640,456,0,0,0,0,0});
    draw(*f,0xA8,2); command(*f,{CMD_MARKER,MARK_PLAYER_END}); draw(*f,0xB8,0);
    std::weak_ptr<const Frame> owner=f; auto source=chunks(f); f.reset();
    require(submitVitaFrame(replay,source,backend,error),"ordered immutable frame submits through actual native adapters");
    require(native_draws==firstDraw+2 && native_rasters==firstRaster+2 && last_draw_count==2 && last_draw_topology==SCE_GXM_PRIMITIVE_LINES,"correct triangle/line dispatch without inventing empty geometry");
    require(backend.order==(std::vector<std::string>{"resolve","surface","marker","camera","copy","resolve","surface","marker"}) &&
            backend.draw_counts==(std::vector<unsigned>{firstDraw+1,firstDraw+1,firstDraw+1,firstDraw+2}),"copy/marker/camera order preserves intervening actual draws");
    require(native_viewport[0]==320 && native_viewport[1]==320 && native_viewport[2]==228 && native_viewport[3]==-228 &&
            native_viewport[4]==0.5f && native_viewport[5]==0.5f,"native raster transport from immutable state");
    require(default_uniforms[16]==0 && default_uniforms[17]==0 && default_uniforms[18]==640 && default_uniforms[19]==456,"reflected exact scissor reaches native uniforms");
    source.reset(); require(owner.expired() && backend.last_lease.expired(),"successful frame released only after Finish");

    f=frame(); draw(*f,0x90,65538); source=chunks(f);
    unsigned prior=native_draws, ends=scene_ends;
    require(submitVitaFrame(replay,source,backend,error) && native_draws==prior+2 && scene_ends==ends+1 &&
            last_draw_count==1539 && static_cast<const PortVitaGxmVertex*>(last_vertex_stream)[0].clip[0]==64000 &&
            static_cast<const uint16_t*>(last_draw_indices)[1538]==1538,
            "all 65538 triangle vertices submit as two whole-primitive U16 chunks in one ordered scene");

    f=frame(); draw(*f,0x90,3); source=chunks(f); owner=f; f.reset();
    end_error=-841; require(!submitVitaFrame(replay,source,backend,error) && error.nativeCode==-841 && active_scene &&
            !backend.last_lease.expired(),"failed EndScene retains frame and all actual-bound resources");
    source.reset(); require(!owner.expired() && releaseVitaFrameReplay(&replay)==-841 && replay,"failed release does not drop pending frame");
    require(!submitVitaFrame(replay,chunks(frame()),backend,error),"partial frame cannot automatically replay or accept another frame");
    end_error=0; unsigned draws=native_draws;
    require(discardVitaFrame(replay)==0 && native_draws==draws && !active_scene && !pending_gpu && owner.expired() &&
            backend.last_lease.expired(),"EndScene retry retires without duplicated draw/copy callbacks");
    f=frame(); command(*f,{CMD_MARKER,MARK_PLAYER_BEGIN}); draw(*f,0x90,3); source=chunks(f); backend.resolve_error=-842;
    require(!submitVitaFrame(replay,source,backend,error) && error.nativeCode==-842 && native_draws==draws && !active_scene,
            "missing material fails before native scene or geometry"); backend.resolve_error=0;
    require(error.sourceEventIndex==1 && error.commandWord==2 && error.drawIndex==0,
            "resource failure preserves original draw/event command provenance after successful XF");
    require(discardVitaFrame(replay)==0,"failed preparation is safely discarded");
    backend.missing_lease=true;
    require(!submitVitaFrame(replay,source,backend,error) && native_draws==draws,"unleased descriptors cannot reach native GPU");
    backend.missing_lease=false; require(discardVitaFrame(replay)==0,"discard rejected unleased binding");
    backend.omit_material=true;
    require(!submitVitaFrame(replay,source,backend,error) && !active_scene && native_draws==draws,
            "raw compatible program cannot bypass exact typed GX material identity");
    backend.omit_material=false; require(discardVitaFrame(replay)==0,"discard missing typed material without draw");
    VitaPreparedDraw expected;
    require(prepareVitaDraw(*source,0,expected,error),"real prepared typed-material mismatch fixture");
    auto wrong_uid=source->chunks()[0].state->uid; wrong_uid.tevColor[0]^=1u<<19;
    installShaders(backend.library,wrong_uid); std::shared_ptr<const void> wrong_material_lease;
    require(resolveVitaMaterial(backend.material_cache,wrong_uid,expected.blend,backend.library,
        backend.override_material,wrong_material_lease,error.message)==0,"actual compatible-interface material has a different TEV key");
    require(!submitVitaFrame(replay,source,backend,error) && !active_scene && native_draws==draws,
            "wrong TEV material cannot bypass typed Frame shader identity despite compatible reflection");
    require(discardVitaFrame(replay)==0,"discard wrong typed material without drawing");
    backend.override_material=nullptr; wrong_material_lease.reset();
    uniform_error=-843;
    require(!submitVitaFrame(replay,source,backend,error) && error.nativeCode==-843 && active_scene && !backend.last_lease.expired(),"native failure after bind retains acknowledged scene state");
    uniform_error=0; require(discardVitaFrame(replay)==0 && native_draws==draws,"failed bound draw closed/fenced without reissuing");
    f=frame(); draw(*f,0x90,3); command(*f,{CMD_EFB_COPY,17,0,0,640,456,0,0,0,0,0}); draw(*f,0x90,3);
    backend.copy_error=-844; source=chunks(f);
    require(!submitVitaFrame(replay,source,backend,error) && error.nativeCode==-844 && native_draws==draws+1 && !active_scene,
            "failed copy halts before resolving/drawing post-copy batch");
    size_t callbacks=backend.order.size(); backend.copy_error=0;
    require(discardVitaFrame(replay)==0 && backend.order.size()==callbacks,"discard never retries non-idempotent copy");
    PortVitaGxmProgram* legacy=nullptr; reflect_raster=false;
    require(port_vita_gxm_device_program_create(device,&vs,8,&fs,8,&blend,4,0,&legacy,&info)==0 && !info.raster_floats,
            "valid legacy program has no reflected exact-scissor capability"); reflect_raster=true;
    PortVitaGxmProgramInfo queried{};
    require(port_vita_gxm_device_program_info(device,program,&queried)==0 && queried.raster_floats==8 && queried.pixel_floats==16 &&
            port_vita_gxm_device_program_info(device,legacy,&queried)==0 && queried.raster_floats==0,
            "owned query returns actual immutable reflection, not currently selected SDK fixture flags");
    f=frame(); f->startBp[0x21]=(2u<<12)|2u; f->startBp[0xC0]=1u<<18; draw(*f,0x90,3); source=chunks(f); prior=native_draws;
    reflect_raster=false;
    require(!submitVitaFrame(replay,source,backend,error) && native_draws==prior && !active_scene && !error.message.empty(),
            "material cache rejects actual legacy reflection before restrictive-scissor native frame submission"); reflect_raster=true;
    auto* stale=legacy;
    require(discardVitaFrame(replay)==0 && port_vita_gxm_device_program_release(device,&legacy)==0,
            "rejected legacy binding remains fenced and owned until discard");
    queried.raster_floats=8;
    require(port_vita_gxm_device_program_info(device,stale,&queried)<0 && !queried.raster_floats,
            "stale capability query fails before pointer dereference and clears output");
    reflect_texture=true; PortVitaGxmProgram* textured=nullptr;
    require(port_vita_gxm_device_program_create(device,&vs,8,&fs,8,&blend,5,1,&textured,&info)==0 && info.texture_mask==1,
            "real reflected texture program for sampler/frame integration");
    require(createVitaTextureCache(device,&backend.texture_cache)==0,"frame uses actual production image cache");
    auto image=std::make_shared<TexImage>(); image->width=image->height=8; image->levels=4;
    image->version=991; image->rgba.assign(85,0x78563412);
    f=frame(); f->textures.push_back(image); f->startXfRegs[0x3F]=1; f->startBp[0x28]=64;
    f->startXfRegs[0x40]=6; f->startBp[0x80]=(6u<<5)|1u|8u|(255u<<9); f->startBp[0x84]=0x3000;
    f->startBp[0x88]=7u|(7u<<10); command(*f,{CMD_TEX,0,0}); draw(*f,0x90,3); source=chunks(f);
    prior=native_draws; backend.omit_view=true;
    require(!submitVitaFrame(replay,source,backend,error) && !active_scene && native_draws==prior,
            "raw default descriptor cannot bypass typed GX sampler ownership");
    backend.omit_view=false; require(discardVitaFrame(replay)==0,"discard missing typed view without draw");
    SpanTexture wrong=source->chunks()[0].state->textures[0]; wrong.mode0=4u<<5;
    std::shared_ptr<const void> wrong_lease;
    require(resolveVitaImage(backend.texture_cache,*f,wrong,backend.override_view,wrong_lease,error.message)==0,
            "real wrong-sampler fixture shares correct image version");
    require(!submitVitaFrame(replay,source,backend,error) && !active_scene && native_draws==prior,
            "correct image with wrong GX sampler rejects before native scene");
    require(discardVitaFrame(replay)==0,"wrong sampler resource fence"); backend.override_view=nullptr; wrong_lease.reset();
    auto other_image=std::make_shared<TexImage>(); other_image->width=other_image->height=8;
    other_image->levels=4; other_image->version=991; other_image->rgba.assign(85,0xEEFF0011);
    Frame other_frame{}; other_frame.textures.push_back(other_image); wrong=source->chunks()[0].state->textures[0];
    require(resolveVitaImage(backend.texture_cache,other_frame,wrong,backend.override_view,wrong_lease,error.message)==0,
            "different immutable image may have the same per-image version counter");
    require(!submitVitaFrame(replay,source,backend,error) && !active_scene && native_draws==prior,
            "same dimensions/version/sampler on the wrong image cannot substitute unrelated pixels");
    require(discardVitaFrame(replay)==0,"wrong image identity fence"); backend.override_view=nullptr; wrong_lease.reset();
    auto manualFrame=frame(); manualFrame->textures.push_back(image);
    manualFrame->startXfRegs[0x3F]=1; manualFrame->startXfRegs[0x40]=6;
    manualFrame->startBp[0x28]=64; manualFrame->startBp[0x80]=0x1d5;
    manualFrame->startBp[0x84]=0x3000; manualFrame->startBp[0x88]=7u|(7u<<10);
    command(*manualFrame,{CMD_TEX,0,0}); draw(*manualFrame,0x90,3);
    auto manualChunks=chunks(manualFrame); VitaPreparedDraw manualPrepared;
    require(prepareVitaDraw(*manualChunks,0,manualPrepared,error) && manualPrepared.fragment.manualTextureMask==1 &&
            manualPrepared.fragment.manualSlots[0]==5 && manualPrepared.fragment.pixel.vectors==6 &&
            manualPrepared.pixel[20]==0 && manualPrepared.pixel[21]==48 && manualPrepared.pixel[22]==3 && manualPrepared.pixel[23]==5,
            "original immutable sampler words select a typed manual material with exact appended original mip/wrap parameters");
    PortVitaGxmTexture* manualImage=nullptr;
    require(port_vita_gxm_texture_create(device,8,8,4,image->version,image->rgba.data(),image->rgba.size()*4,&manualImage)==0 &&
            port_vita_gxm_texture_view_create_explicit_linear_mip(manualImage,0x1d5,0x3000,&backend.override_view)==0,
            "adversarial backend owns a real explicit point-fetch view with exact original pixels, words and mip levels");
    backend.bypass_sampler_cache=true;
    require(!submitVitaFrame(replay,manualChunks,backend,error) && !active_scene && native_draws==prior &&
            error.message.find("manual-sampling material contract")!=std::string::npos,
            "an exact-identity diagonal point-fetch view cannot reach an implicit-LOD material, even through a custom backend");
    require(discardVitaFrame(replay)==0 && backend.last_lease.expired() &&
            port_vita_gxm_texture_view_release(&backend.override_view)==0 && port_vita_gxm_texture_release(&manualImage)==0,
            "rejected manual/implicit shader pairing releases its real GPU resources only after replay retirement");
    backend.bypass_sampler_cache=false;
    const auto manualUploads=texture_inits.size();
    require(submitVitaFrame(replay,manualChunks,backend,error) && native_draws==prior+1 &&
            sampled_texture.generic.min_filter==SCE_GXM_TEXTURE_FILTER_MIPMAP_POINT && sampled_texture.generic.mag_filter==SCE_GXM_TEXTURE_FILTER_POINT && sampled_texture.generic.mip_filter==0 &&
            sampled_texture.generic.uaddr_mode==SCE_GXM_TEXTURE_ADDR_CLAMP && sampled_texture.generic.vaddr_mode==SCE_GXM_TEXTURE_ADDR_CLAMP && sampled_texture.generic.lod_bias==31 &&
            default_uniforms[20]==0 && default_uniforms[21]==48 && default_uniforms[22]==3 && default_uniforms[23]==5 &&
            texture_inits.size()==manualUploads,
            "real original frame traversal sends typed manual uniforms and an owned point-fetch descriptor to native submission");
    require(manualChunks->chunks()[0].state->uid==source->chunks()[0].state->uid,
            "manual and implicit sampler words intentionally share one original UID");
    VitaMaterial *implicitMaterial=nullptr,*manualMaterial=nullptr; std::shared_ptr<const void> implicitLease,manualLease;
    require(resolveVitaMaterialProfile(backend.material_cache,manualChunks->chunks()[0].state->uid,manualPrepared.blend,
        manualPrepared.profile,backend.library,implicitMaterial,implicitLease,error.message)==0 &&
        resolveVitaMaterialProfile(backend.material_cache,manualChunks->chunks()[0].state->uid,manualPrepared.blend,
        manualPrepared.profile,backend.library,manualMaterial,manualLease,error.message,1)==0 && implicitMaterial!=manualMaterial,
        "same original UID/blend/profile has distinct cached implicit and manual material identities");
    PortVitaGxmProgram* crossProgram=nullptr;
    require(bindVitaMaterialProfile(implicitMaterial,device,manualChunks->chunks()[0].state->uid,manualPrepared.blend,
        manualPrepared.profile,manualPrepared.fragment,crossProgram,error.message)<0 && !crossProgram,
        "compiled material identity cannot self-confirm a draw-derived manual mask");
    implicitLease.reset(); manualLease.reset(); prior=native_draws;
    require(resolveVitaImage(backend.texture_cache,*f,source->chunks()[0].state->textures[0],backend.override_view,
        wrong_lease,error.message)==0 && !submitVitaFrame(replay,manualChunks,backend,error) && native_draws==prior &&
        error.message.find("manual-sampling material contract")!=std::string::npos,
        "typed manual program cannot sample through a substituted implicit descriptor");
    require(discardVitaFrame(replay)==0,"wrong descriptor-kind pairing retires before reuse");
    backend.override_view=nullptr; wrong_lease.reset();
    auto changedManual=std::make_shared<Frame>(*manualFrame);
    changedManual->startBp[0x80]=0x1d5|(253u<<9); changedManual->startBp[0x84]=9u|(19u<<8);
    const auto programsBefore=shader_programs;
    require(submitVitaFrame(replay,chunks(changedManual),backend,error) && native_draws==prior+1 &&
        shader_programs==programsBefore && texture_inits.size()==manualUploads && default_uniforms[16]==-3.0f/32.0f &&
        default_uniforms[20]==9 && default_uniforms[21]==19 && default_uniforms[22]==3 && default_uniforms[23]==5,
        "dynamic original fractional LOD bounds and negative odd bias repack without a material alias, upload or compile");
    prior=native_draws; changedManual.reset(); manualChunks.reset(); manualFrame.reset(); reflect_manual=false;
    const auto bindingsBeforeImplicit=texture_bindings;
    require(submitVitaFrame(replay,source,backend,error) && native_draws==prior+1 && texture_bindings==bindingsBeforeImplicit+1 &&
            sampled_texture.generic.min_filter==2 && sampled_texture.generic.mip_filter==1 &&
            sampled_texture.generic.uaddr_mode==0 && sampled_texture.generic.vaddr_mode==1 && sampled_texture.generic.lod_bias==31 &&
            default_uniforms[16]==-0.03125f && default_uniforms[18]==0.125f,
            "typed immutable cache descriptor and exact single shader bias actually reach real native submission");
    end_error=-872; prior=native_draws;
    require(!submitVitaFrame(replay,source,backend,error) && active_scene && !backend.last_lease.expired() &&
            releaseVitaTextureCache(&backend.texture_cache)<0 && backend.texture_cache,
            "failed scene End retains typed texture cache lease and recovery owner");
    end_error=0;
    require(discardVitaFrame(replay)==0 && native_draws==prior+1 && backend.last_lease.expired() &&
            releaseVitaTextureCache(&backend.texture_cache)==0 && !backend.texture_cache,
            "retry closes/fences scene before explicit cache teardown without reissuing textured draw");
    require(port_vita_gxm_device_program_release(device,&textured)==0,"textured native program retired");
    reflect_texture=false;
    require(releaseVitaMaterialCache(&backend.material_cache)==0 && !backend.material_cache,
            "Frame material leases are retired before explicit native material cache close");
    require(releaseVitaEfbCache(&backend.efb_cache)==0 && !backend.efb_cache,
            "Frame target leases are retired before actual native EFB surface cache close");
    unmap_error=-845;
    require(releaseVitaFrameReplay(&replay)==-845 && replay && !submitVitaFrame(replay,source,backend,error),
            "failed staging unmap preserves closing replay and device lease"); unmap_error=0;
    require(releaseVitaFrameReplay(&replay)==0 && !replay && port_vita_gxm_device_program_release(device,&program)==0 &&
            port_vita_gxm_surface_release(&efb)==0 && port_vita_gxm_device_release(&device)==0,"all retained native/frame resources released");
    source.reset(); f.reset(); empty_state();
}

void efbCpuFailures() {
    VitaShaderLibrary library; installClearShader(library); std::string error; error.reserve(256);
    for (unsigned failure=1;failure<=2;++failure) {
        PortVitaGxmDevice* device=nullptr; VitaEfbCache* cache=nullptr;
        require(port_vita_gxm_device_create(&device)==0,"CPU shader allocation fault has an independent native device");
        cpu_fail_bytes=8; cpu_fail_after=failure; bool threw=false; int result=0;
        try { result=createVitaEfbCache(device,library,&cache,error); } catch (const std::bad_alloc&) { threw=true; }
        cpu_fail_after=0; cpu_fail_bytes=0;
        require(!threw && result<0 && !cache && !error.empty(),
                "each failed CPU shader copy returns an error without an inaccessible cache/device hold");
        require(port_vita_gxm_device_release(&device)==0,"failed CPU cache creation balances all native device holds"); empty_state();
    }
    PortVitaGxmDevice* device=nullptr; VitaEfbCache* cache=nullptr;
    require(port_vita_gxm_device_create(&device)==0 && createVitaEfbCache(device,library,&cache,error)==0,
            "CPU target allocation fault begins with a valid native cache");
    PortVitaGxmSurface* target=nullptr; std::shared_ptr<const void> lease;
    const unsigned allocations=memory.size(); cpu_fail_any=true; cpu_fail_after=1; bool threw=false; int result=0;
    try { result=resolveVitaEfb(cache,640,456,target,lease,error); } catch (const std::bad_alloc&) { threw=true; }
    cpu_fail_after=0; cpu_fail_any=false;
    require(!threw && result<0 && !target && !lease && memory.size()==allocations && !error.empty(),
            "failed CPU target/control-block allocation returns before native surface creation without throwing");
    require(resolveVitaEfb(cache,640,456,target,lease,error)==0 && target && lease,
            "failed CPU target allocation does not poison a later owned full-size target");
    target=nullptr; lease.reset();
    require(releaseVitaEfbCache(&cache)==0 && port_vita_gxm_device_release(&device)==0,
            "CPU target failure/retry preserves all cache and device teardown ownership"); empty_state();
}

void efbClears() {
    PortVitaGxmDevice* device=nullptr; VitaEfbCache* cache=nullptr; VitaShaderLibrary library,missing; std::string error;
    require(port_vita_gxm_device_create(&device)==0 && createVitaEfbCache(device,missing,&cache,error)<0 && !cache && !error.empty(),
            "missing exact compiled clear pair cannot create a placeholder EFB backend");
    installClearShader(library);
    require(createVitaEfbCache(device,library,&cache,error)==0 && cache,"real EFB owner holds device and mapped clear staging");
    PortVitaGxmSurface *a=nullptr,*b=nullptr,*small=nullptr; std::shared_ptr<const void> al,bl,sl;
    require(resolveVitaEfb(cache,640,456,a,al,error)==0 && a && al,"full native EFB resolved with real lease");
    unsigned allocations=memory.size();
    require(resolveVitaEfb(cache,640,456,b,bl,error)==0 && a==b && memory.size()==allocations,
            "full-size target and native descriptors reused without allocation");
    require(resolveVitaEfb(cache,320,228,small,sl,error)==0 && small!=a,"dimension transition has a separate native target");
    auto* alias=a; unsigned before=finishes;
    require(port_vita_gxm_surface_release(&alias)<0 && alias==a && finishes==before,
            "public surface release cannot bypass actual leased cache target");
    int wrong_owner=0;
    require(port_vita_gxm_surface_create_owned(device,640,456,&wrong_owner,&alias)<0 &&
        port_vita_gxm_surface_release_owned(&alias,&wrong_owner)<0 && alias==a && finishes==before,
            "failed creation with nonempty output cannot steal an existing cache manager");
    VitaEfbClear request{7,11,51,43,0xA1B2,0xC3D4,0xFEDCBA,1,1,1,0x59};
    unsigned draws=native_draws;
    require(clearVitaEfb(cache,a,request,error)==0 && native_draws==draws+1 && !active_scene && !pending_gpu,
            "complete GX format/early-Z state preserves RGBA6 clear and ends/fences once");
    require(patched_blend.color_mask==15 && patched_blend.color_func==0 && patched_blend.alpha_func==0 &&
        clear_depth_func==SCE_GXM_DEPTH_FUNC_ALWAYS && clear_depth_write==SCE_GXM_DEPTH_WRITE_ENABLED,
        "clear masks are unblended and depth writes do not depend on recorded depth test");
    require(default_uniforms[0]==float(178)/255 && default_uniforms[1]==float(195)/255 &&
        default_uniforms[2]==float(215)/255 && default_uniforms[3]==float(162)/255 &&
        default_uniforms[4]==float(0xFEDCBA)/16777215 && default_uniforms[16]==7 && default_uniforms[17]==11 &&
        default_uniforms[18]==58 && default_uniforms[19]==54,
        "RGBA6 clear bit expansion, full 24-bit depth and exact rectangular fragment scissor reach native uniforms");
    const auto* verts=static_cast<const PortVitaGxmVertex*>(last_vertex_stream);
    require(last_draw_count==3 && last_draw_topology==SCE_GXM_PRIMITIVE_TRIANGLES && verts[0].clip[0]==-1 &&
        verts[1].clip[0]==3 && verts[2].clip[1]==3,"full coverage triangle uses the real 152-byte stream");
    unsigned programs=shader_programs; draws=native_draws;
    require(clearVitaEfb(cache,a,request,error)==0 && shader_programs==programs && native_draws==draws+1,
            "identical color mask reuses actual native clear patcher program");
    request.colorWrite=0; request.depthWrite=0;
    require(clearVitaEfb(cache,a,request,error)==0 && patched_blend.color_mask==1 &&
        clear_depth_write==SCE_GXM_DEPTH_WRITE_DISABLED,"alpha-only clear does not overwrite RGB or depth");
    request.alphaWrite=0; request.colorWrite=1; request.pixelFormat=0x5A;
    require(clearVitaEfb(cache,a,request,error)==0 && patched_blend.color_mask==14 &&
        default_uniforms[0]==float(181)/255 && default_uniforms[1]==float(195)/255 && default_uniforms[2]==float(214)/255,
        "RGB565 clear precision and RGB-only native mask preserve alpha/depth");
    request.colorWrite=0; request.depthWrite=1; request.pixelFormat=3;
    require(clearVitaEfb(cache,a,request,error)==0 && patched_blend.color_mask==0 &&
        clear_depth_write==SCE_GXM_DEPTH_WRITE_ENABLED,"depth-only clear leaves every color/alpha channel untouched");
    request.depthWrite=0; draws=native_draws;
    require(clearVitaEfb(cache,a,request,error)==0 && native_draws==draws,"disabled update masks produce no clear draw");
    request.depthWrite=1; request.width=640;
    require(clearVitaEfb(cache,a,request,error)<0 && !active_scene && native_draws==draws,"out-of-bounds clear never draws");
    request.width=51; request.pixelFormat=0x5A;
    const int wrong_depth=clearVitaEfb(cache,a,request,error);
    require(wrong_depth<0 && native_draws==draws && !error.empty(),
            "compressed GX Z16 state cannot silently clear as unquantized native Z24");
    request.pixelFormat=3;
    SpanCopy copied{}; copied.efbWidth=320; copied.efbHeight=228;
    copied.words[1]=7u|(11u<<10); copied.words[2]=50u|(42u<<10); copied.words[6]=0xA1B2;
    copied.words[7]=0xC3D4; copied.words[8]=0xFEDCBA; copied.words[9]=1u<<11;
    copied.colorMode=24; copied.depthMode=0; copied.pixelFormat=0x59;
    require(clearVitaEfbAfterCopy(cache,a,copied,error)<0 && native_draws==draws,
            "copy-clear cannot target a different native EFB size even when its rectangle fits");
    copied.efbWidth=640; copied.efbHeight=456;
    require(clearVitaEfbAfterCopy(cache,a,copied,error)==0 && default_uniforms[16]==7 && default_uniforms[19]==54,
            "actual after-copy clear uses recorded pre-copy dimensions, update masks and clear words");
    copied.pixelFormat=0x80; draws=native_draws;
    require(clearVitaEfbAfterCopy(cache,a,copied,error)<0 && native_draws==draws && !error.empty(),
            "copy-clear forwards complete format state and never masks reserved bits into a supported format");
    copied.words[9]=0; draws=native_draws;
    require(clearVitaEfbAfterCopy(cache,a,copied,error)==0 && native_draws==draws,
            "copy without clear bit never invents an extra clear draw");
    request.width=51; end_error=-901;
    require(clearVitaEfb(cache,a,request,error)==-901 && active_scene && !pending_gpu,
            "failed clear EndScene retains exact target/staging/program and does not claim success");
    al.reset(); bl.reset(); sl.reset(); a=b=small=nullptr; draws=native_draws;
    require(collectVitaEfbs(cache)<0 && releaseVitaEfbCache(&cache)==-901 && cache &&
        clearVitaEfb(cache,alias,request,error)<0,"pending clear prevents eviction/destruction and automatic replay");
    end_error=0;
    require(discardVitaEfbClear(cache)==0 && native_draws==draws && !active_scene && !pending_gpu,
            "clear recovery retries only End/Finish, never repeats the draw");
    require(resolveVitaEfb(cache,640,456,a,al,error)==0,"fenced target remains usable after explicit recovery");
    uniform_error=-902;
    require(clearVitaEfb(cache,a,request,error)==-902 && active_scene && native_draws==draws,
            "failed native clear uniform upload retains after-bind ownership without false draw success");
    uniform_error=0;
    require(discardVitaEfbClear(cache)==0 && native_draws==draws,"after-bind cleanup acknowledges without reissuing clear");
    al.reset(); a=nullptr; unmap_error=-903;
    require(releaseVitaEfbCache(&cache)==-903 && cache,"failed native target unmap keeps explicit cache/device recovery owner");
    unmap_error=0;
    require(releaseVitaEfbCache(&cache)==0,"native EFB target failure recovery finishes");
    require(createVitaEfbCache(device,library,&cache,error)==0 && resolveVitaEfb(cache,64,64,a,al,error)==0,
            "isolated mapped clear-staging teardown failure fixture");
    request={0,0,64,64,0xFFFF,0xFFFF,0xFFFFFF,1,1,1,1};
    require(clearVitaEfb(cache,a,request,error)==0 && allocation(last_vertex_stream),"fresh clear stream begins at exact mapped slab base");
    unmap_failure_address=last_vertex_stream; unmap_address_error=-904; al.reset(); a=nullptr;
    require(releaseVitaEfbCache(&cache)==-904 && cache,"failed clear slab unmap preserves closing EFB owner after other resources retire");
    unmap_address_error=0; unmap_failure_address=nullptr;
    require(releaseVitaEfbCache(&cache)==0 && port_vita_gxm_device_release(&device)==0,
            "closing EFB cache retries staging release without resetting a closing staging owner"); empty_state();
}

// Catches logical-vs-padded UVs, guessed depth tiling, mutable target reuse,
// missing compiler artifacts and loss of either surface on after-bind failure.
void efbCaptures() {
    std::string error; error.reserve(256); VitaShaderLibrary library;
    installCaptureShaders(library);
    for (unsigned failure=3;failure<=4;++failure) {
        PortVitaGxmDevice* device=nullptr; VitaEfbCache* cache=nullptr;
        require(port_vita_gxm_device_create(&device)==0,"capture artifact CPU-fault device");
        cpu_fail_bytes=8; cpu_fail_after=failure; bool threw=false; int result=0;
        try { result=createVitaEfbCache(device,library,&cache,error); } catch (const std::bad_alloc&) { threw=true; }
        cpu_fail_after=0; cpu_fail_bytes=0;
        require(!threw && result<0 && !cache,"either capture shader-copy OOM returns without orphan cache/device hold");
        require(port_vita_gxm_device_release(&device)==0,"capture artifact OOM balances native holds"); empty_state();
    }
    PortVitaGxmDevice* device=nullptr; VitaEfbCache* cache=nullptr; VitaShaderLibrary clearOnly;
    installClearShader(clearOnly);
    require(port_vita_gxm_device_create(&device)==0 && createVitaEfbCache(device,clearOnly,&cache,error)==0,
            "clear-only cache does not pretend optional capture artifacts exist");
    PortVitaGxmSurface *source=nullptr,*first=nullptr,*second=nullptr,*ordinary=nullptr;
    std::shared_ptr<const void> sl,fl,dl,ol;
    require(resolveVitaEfb(cache,641,457,source,sl,error)==0,"unaligned full-size source for sampling");
    unsigned allocations=memory.size(),draws=native_draws,records=texture_inits.size();
    require(captureVitaEfb(cache,source,7,11,51,43,false,first,fl,error)<0 && !first && !fl &&
        memory.size()==allocations && native_draws==draws && texture_inits.size()==records && !error.empty(),
        "missing exact capture artifact rejects before allocation, sampling or GPU draw");
    sl.reset(); source=nullptr;
    require(releaseVitaEfbCache(&cache)==0 && createVitaEfbCache(device,library,&cache,error)==0 &&
        resolveVitaEfb(cache,641,457,source,sl,error)==0,"real capture cache and owned source");
    PortVitaGxmSurfaceInfo sourceInfo{}; PortVitaGxmSurfaceSampleInfo sample{};
    require(port_vita_gxm_surface_info(source,&sourceInfo)==0,"native source plane addresses");
    texture_validate_fail_call=texture_validations+2;
    require(port_vita_gxm_surface_sample(device,source,&sample)==-913 && !sample.color_texture && !sample.depth_texture &&
            !sample.width,"second-plane validation failure publishes neither descriptor");
    texture_validate_fail_call=0; records=texture_inits.size();
    require(port_vita_gxm_surface_sample(device,source,&sample)==0 && sample.width==641 && sample.height==457 &&
        sample.color_width==704 && sample.color_height==457 && sample.depth_width==672 && sample.depth_height==480,
        "native plane views report separate logical, 64-texel color-pitch and 32x32 depth-tile extents");
    require(texture_inits.size()==records+2 && texture_inits[records].data==sourceInfo.color_data &&
        texture_inits[records].width==704 && texture_inits[records].height==457 && !texture_inits[records].tiled &&
        texture_inits[records+1].data==sourceInfo.depth_data && texture_inits[records+1].width==672 &&
        texture_inits[records+1].height==480 && texture_inits[records+1].tiled,
        "production descriptors alias the exact real planes and physical extents");
    const auto* color=static_cast<const SceGxmTexture*>(sample.color_texture);
    const auto* depth=static_cast<const SceGxmTexture*>(sample.depth_texture);
    for (const auto* descriptor:{color,depth}) require(descriptor->generic.uaddr_mode==2 && descriptor->generic.vaddr_mode==2 &&
        !descriptor->generic.min_filter && !descriptor->generic.mag_filter && !descriptor->generic.mip_filter &&
        descriptor->generic.lod_bias==31 && !descriptor->generic.mip_count,"raw plane sampler is point/clamp with no mip or bias");
    records=texture_inits.size(); const unsigned setters=sampler_calls;
    require(port_vita_gxm_surface_sample(device,source,&sample)==0 && sample.color_texture==color && sample.depth_texture==depth &&
        texture_inits.size()==records && sampler_calls==setters,"stable native descriptors are reused without changing existing aliases");
    require(port_vita_gxm_device_begin(device,source)==0 && port_vita_gxm_surface_sample(device,source,&sample)<0 &&
        !sample.color_texture && !sample.depth_texture,"sampling cannot fence or expose planes during any open scene");
    require(port_vita_gxm_device_end(device,nullptr,0)==0 && pending_gpu &&
        port_vita_gxm_surface_sample(device,source,&sample)==0 && !pending_gpu,"native source Finish precedes descriptor exposure");
    allocations=memory.size(); draws=native_draws;
    require(captureVitaEfb(cache,source,641,11,1,43,false,first,fl,error)<0 && !first && !fl &&
        memory.size()==allocations && native_draws==draws,"out-of-bounds capture rejects before native work");
    reflect_texture=true;
    require(captureVitaEfb(cache,source,7,11,51,43,false,first,fl,error)==0 && first && fl && first!=source &&
        native_draws==draws+1 && !active_scene && !pending_gpu,"raw color capture publishes a fresh leased target only after End/Finish");
    require(last_sampled_descriptor==color && default_uniforms[0]==7 && default_uniforms[1]==11 &&
        default_uniforms[2]==704 && default_uniforms[3]==457 && default_uniforms[20]==0 && default_uniforms[21]==0 &&
        default_uniforms[22]==51 && default_uniforms[23]==43 && patched_blend.color_mask==15 &&
        !patched_blend.color_func && clear_depth_write==SCE_GXM_DEPTH_WRITE_DISABLED,
        "capture uses actual source color descriptor, padded UV extent, exact destination scissor and unblended depth-disabled draw");
    PortVitaGxmSurfaceInfo firstInfo{},secondInfo{};
    require(port_vita_gxm_surface_info(first,&firstInfo)==0 && firstInfo.width==51 && firstInfo.height==43 &&
        firstInfo.color_data!=sourceInfo.color_data,"capture target has exact requested logical size and separate native storage");
    require(captureVitaEfb(cache,source,7,11,51,43,true,second,dl,error)==0 && second!=first &&
        last_sampled_descriptor==depth && default_uniforms[2]==672 && default_uniforms[3]==480 &&
        port_vita_gxm_surface_info(second,&secondInfo)==0 && secondInfo.color_data!=firstInfo.color_data,
        "same-size depth capture has a new target and samples native tiled Z24 with its own padded extent");
    require(resolveVitaEfb(cache,51,43,ordinary,ol,error)==0 && ordinary!=first && ordinary!=second,
        "ordinary mutable EFB reuse cannot return an earlier capture target");
    // Controlled driver-result pixels, not GPU shader execution. The native
    // consumer must read real owned allocation/pitch/origin and produce bytes.
    auto* captureData=static_cast<unsigned char*>(const_cast<void*>(firstInfo.color_data));
    for (unsigned y=0;y<43;++y) for (unsigned x=0;x<51;++x) {
        auto* p=captureData+(size_t(y)*firstInfo.stride+x)*4;
        p[0]=unsigned(x+16); p[1]=unsigned(y+64); p[2]=128; p[3]=48;
    }
    SpanCopy processedCopy{}; processedCopy.efbWidth=641; processedCopy.efbHeight=457;
    processedCopy.words[0]=1; processedCopy.words[1]=8|(12u<<10); processedCopy.words[2]=3|(3u<<10);
    processedCopy.words[3]=1; processedCopy.words[4]=2; processedCopy.words[9]=6u<<4;
    processedCopy.filter[0]=21u<<12|22u<<18; processedCopy.filter[1]=21;
    GxCopyPixels processed;
    require(processVitaEfbCapture(cache,ordinary,processedCopy,processed,error)<0 && !processed.texels().data,
        "native pixel consumer rejects mutable EFB targets, not just unsupported dimensions");
    require(processVitaEfbCapture(cache,second,processedCopy,processed,error)<0 && !processed.texels().data,
        "color producer cannot consume a depth capture without matching recorded plane provenance");
    require(processVitaEfbCapture(cache,first,processedCopy,processed,error)==0 && processed.texels().data[0]==17 &&
        processed.texels().data[1]==65 && processed.texels().data[4*4]==17 && processed.texels().data[4*4+1]==66 &&
        processed.texels().data[3]==255 && !pending_gpu,
        "native capture consumer uses original crop coordinates, actual padded pitch and fenced owned pixels");
    GxCopyHistory* pixelHistory=nullptr; unsigned char pixelRam[128]{}; std::shared_ptr<const GxCopyBytes> pixelPacket;
    require(createGxCopyHistory(&pixelHistory)==0 &&
        storeGxCopyBytes(pixelHistory,1,processedCopy,processed.texels(),{pixelRam,0,sizeof(pixelRam)},pixelPacket,error)==0 &&
        pixelRam[32]==255 && pixelRam[33]==17 && pixelRam[64]==65 && pixelRam[65]==128,
        "actual native allocation readback feeds production processing, split-plane packer and explicit ordered RAM write");
    pixelPacket.reset(); processed={};
    auto* encodedDepth=static_cast<unsigned char*>(const_cast<void*>(secondInfo.color_data));
    for (unsigned y=0;y<43;++y) for (unsigned x=0;x<51;++x) {
        const unsigned z=255+(x&1)+(y&1)+(x/2)*512+(y/2)*65536;
        auto* p=encodedDepth+(size_t(y)*secondInfo.stride+x)*4;
        p[0]=z>>16; p[1]=(z>>8)&255; p[2]=z&255; p[3]=0;
    }
    processedCopy.pixelFormat=3; processedCopy.words[0]=2; processedCopy.words[2]=2|(2u<<10);
    processedCopy.words[9]=(6u<<4)|(1u<<9);
    require(processVitaEfbCapture(cache,second,processedCopy,processed,error)==0 && processed.texels().data[0]==0 &&
        processed.texels().data[1]==130 && processed.texels().data[2]==0 && processed.texels().data[3]==255 &&
        processed.texels().data[4*4]==1 && processed.texels().data[3*4+1]==136,
        "native encoded-depth allocation preserves full Z24 carry at nonzero origin with half-size padded samples");
    require(storeGxCopyBytes(pixelHistory,1,processedCopy,processed.texels(),{pixelRam,0,sizeof(pixelRam)},pixelPacket,error)==0 &&
        pixelRam[32]==255 && pixelRam[33]==0 && pixelRam[41]==1 && pixelRam[64]==130 && pixelRam[65]==0 && pixelRam[70]==136,
        "native encoded depth uses physical capture pitch and feeds actual split-plane packed RAM bytes");
    pixelPacket.reset(); processed={}; processedCopy.pixelFormat=0; processedCopy.words[9]=6u<<4;
    processedCopy.words[2]=3|(3u<<10);
    require(releaseGxCopyHistory(&pixelHistory)==0,"native readback CPU history retires independently of GPU source leases");
    processedCopy.efbWidth=640;
    require(processVitaEfbCapture(cache,first,processedCopy,processed,error)<0 && !processed.texels().data,
        "capture's original full-source dimensions cannot be replaced with another frame's EFB");
    processedCopy.efbWidth=641;
    require(port_vita_gxm_device_begin(device,source)==0 &&
        processVitaEfbCapture(cache,first,processedCopy,processed,error)<0 && !processed.texels().data,
        "native CPU capture reads cannot happen during a source-writing scene");
    require(port_vita_gxm_device_end(device,nullptr,0)==0,"scene closes before native CPU processing retry");
    draws=native_draws; unsigned programs=shader_programs;
    VitaEfbClear snapshotClear{0,0,51,43,0xFFFF,0xFFFF,0xFFFFFF,1,1,1,1};
    require(clearVitaEfb(cache,first,snapshotClear,error)<0 && native_draws==draws,
        "cache clear cannot mutate a published snapshot even if its rectangle fits");
    PortVitaGxmSurface* third=nullptr; std::shared_ptr<const void> tl;
    require(captureVitaEfb(cache,source,0,0,51,43,false,third,tl,error)==0 && third!=first && third!=second &&
        shader_programs==programs && native_draws==draws+1,"later capture reuses the exact shader but never an older snapshot's pixels");
    require(captureVitaEfb(cache,source,0,0,1,1,false,third,tl,error)<0 && native_draws==draws+1,
        "nonempty output cannot be overwritten or lose an existing snapshot lease");
    tl.reset(); third=nullptr; ol.reset(); ordinary=nullptr;
    reflect_texture=false; VitaEfbClear clear{0,0,641,457,0x1234,0x5678,0x9ABCDE,1,1,1,1};
    require(clearVitaEfb(cache,source,clear,error)==0 && allocation(firstInfo.color_data) && allocation(secondInfo.color_data),
        "later source clear does not free or reuse either leased snapshot's native storage");
    reflect_texture=true; draws=native_draws; end_error=-914;
    require(captureVitaEfb(cache,source,1,2,17,19,false,third,tl,error)==-914 && !third && !tl && active_scene,
        "failed capture End retains both source and destination without publishing incomplete pixels");
    sl.reset(); source=nullptr;
    require(collectVitaEfbs(cache)<0 && releaseVitaEfbCache(&cache)==-914 && cache && allocation(sourceInfo.color_data),
        "failed scene cannot evict a dropped source lease or lose the unpublished destination owner");
    end_error=0;
    require(discardVitaEfbClear(cache)==0 && native_draws==draws+1 && !active_scene && !pending_gpu &&
        collectVitaEfbs(cache)==0 && !allocation(sourceInfo.color_data) && allocation(firstInfo.color_data) && allocation(secondInfo.color_data),
        "explicit capture recovery ends/fences without replay, then releases only unleased source and failed targets");
    require(resolveVitaEfb(cache,64,64,source,sl,error)==0,"source for after-bind capture failure");
    draws=native_draws; uniform_error=-915;
    require(captureVitaEfb(cache,source,0,0,64,64,true,third,tl,error)==-915 && !third && !tl && active_scene &&
        native_draws==draws,"failed capture uniform bind retains the pending program without a false completed draw");
    uniform_error=0;
    require(discardVitaEfbClear(cache)==0 && native_draws==draws,"after-bind capture cleanup never reissues work");
    sl.reset(); fl.reset(); dl.reset(); source=first=second=nullptr;
    unmap_failure_address=firstInfo.color_data; unmap_address_error=-916;
    require(releaseVitaEfbCache(&cache)==-916 && cache,"snapshot target-unmap failure retains explicit native recovery ownership");
    unmap_failure_address=nullptr; unmap_address_error=0;
    require(releaseVitaEfbCache(&cache)==0 && port_vita_gxm_device_release(&device)==0,
        "source, all captures, both programs and staging retire without orphan GPU memory");
    reflect_texture=false; empty_state();
}

// Catches wrong tile/component order, invented padding, contiguous-vs-RAM
// stride confusion, partial publication, replayed writes and stale versions.
void gxCopyBytes() {
    GxCopyHistory* history=nullptr; std::string error; error.reserve(256);
    require(createGxCopyHistory(&history)==0 && history,"real packed-copy history owner");
    struct Fixture { uint32_t format,w,h,size; unsigned char a,b; };
    const Fixture cases[]={
        {0,8,8,32,0xAA,0xAA},{1,8,4,32,0xAB,0xAB},{2,8,4,32,0x9A,0x9A},
        {3,4,4,32,0x97,0xAB},{4,4,4,32,0xAE,0x7D},{5,4,4,32,0x4A,0xCE},
        {6,4,4,64,0x97,0xAB},{7,8,4,32,0x97,0x97},{8,8,4,32,0xAB,0xAB},
        {9,8,4,32,0xCD,0xCD},{10,8,4,32,0xEF,0xEF},{11,4,4,32,0xCD,0xAB},{12,4,4,32,0xEF,0xCD}};
    unsigned char pixels[8*8*4];
    for (unsigned i=0;i<64;++i) { pixels[i*4]=0xAB; pixels[i*4+1]=0xCD; pixels[i*4+2]=0xEF; pixels[i*4+3]=0x97; }
    unsigned char ram[1024]; std::shared_ptr<const GxCopyBytes> packed;
    for (const auto& fixture:cases) {
        std::memset(ram,0x55,sizeof(ram)); SpanCopy copy{};
        copy.efbWidth=640; copy.efbHeight=456; copy.pixelFormat=1;
        copy.words[0]=fixture.format+1; copy.words[2]=0; copy.words[3]=1;
        copy.words[4]=fixture.size/32;
        copy.words[9]=((fixture.format&7)<<4)|((fixture.format&8)<<0);
        copy.filter[0]=21u<<12|22u<<18; copy.filter[1]=21;
        GxCopyTexels input{pixels,fixture.w,fixture.h,fixture.w*4,size_t(fixture.w)*fixture.h*4};
        require(storeGxCopyBytes(history,100,copy,input,{ram,0,sizeof(ram)},packed,error)==0 && packed &&
            packed->width()==1 && packed->height()==1 && packed->format()==fixture.format && packed->bytes()==fixture.size &&
            packed->ramExtent()==fixture.size && packed->address()==32,"each original texture-copy layout packs complete destination blocks");
        for (unsigned i=0;i<fixture.size;++i) {
            const unsigned char expected=fixture.format==6 && i>=32 ? (i%2 ? 0xEF : 0xCD) :
                (i%2 ? fixture.b : fixture.a);
            require(packed->data()[i]==expected && ram[32+i]==expected,"literal independent byte fixture matches immutable packet and real RAM write");
        }
        require(ram[31]==0x55 && ram[32+fixture.size]==0x55 &&
            packed->command().filter[0]==(21u<<12|22u<<18),"copy writes only its extent and retains complete command-time filter metadata");
        if (fixture.format==0 || fixture.format==1 || fixture.format==3 || fixture.format==6) {
            uint32_t decoded=0; decodeTexture(reinterpret_cast<unsigned char*>(&decoded),packed->data(),fixture.format,1,1,nullptr,0);
            const uint32_t expected=fixture.format==0 ? 0xAAAAAAAA : fixture.format==1 ? 0xABABABAB :
                fixture.format==3 ? 0x97ABABAB : 0x97EFCDAB;
            require(decoded==expected,"real original-format decoder reads packed bytes, including intensity alpha rather than an RGBA crop");
        }
        packed.reset();
    }
    // Padding is actual producer-supplied data, not edge replication/zeros.
    for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x) pixels[(y*8+x)*4]=unsigned(y*16+x);
    SpanCopy copy{}; copy.efbWidth=640; copy.efbHeight=456; copy.words[0]=20; copy.words[3]=1; copy.words[4]=1;
    GxCopyTexels input{pixels,8,8,32,sizeof(pixels)};
    require(storeGxCopyBytes(history,101,copy,input,{ram,0,sizeof(ram)},packed,error)==0 &&
        packed->data()[0]==0 && packed->data()[4]==0x11 && packed->data()[31]==0x77,
        "partial logical texture still encodes all distinct supplied tile-padding samples");
    packed.reset();
    // RGB5A3 uses opaque encoding beginning at224, partial alpha below it.
    for (unsigned i=0;i<64;++i) { pixels[i*4]=0xAB; pixels[i*4+1]=0xCD; pixels[i*4+2]=0xEF; pixels[i*4+3]=224; }
    input={pixels,4,4,16,64}; copy.words[0]=21; copy.words[9]=5u<<4;
    require(storeGxCopyBytes(history,101,copy,input,{ram,0,sizeof(ram)},packed,error)==0 &&
        packed->data()[0]==0xD7 && packed->data()[1]==0x3D,"RGB5A3 opaque threshold retains original five-bit RGB layout");
    packed.reset(); pixels[3]=223; copy.words[0]=22;
    require(storeGxCopyBytes(history,101,copy,input,{ram,0,sizeof(ram)},packed,error)==0 &&
        packed->data()[0]==0x6A && packed->data()[1]==0xCE,"RGB5A3 partial-alpha threshold retains original A3/RGB4 layout");
    packed.reset();
    // Gap bytes retain their previous RAM state, not packed source bytes.
    unsigned char rows[4*8*4];
    for (unsigned i=0;i<32;++i) { rows[i*4]=i<16 ? 0x12 : 0x56; rows[i*4+1]=0x34; rows[i*4+2]=0x78; rows[i*4+3]=0x9A; }
    copy.words[0]=30; copy.words[2]=3u|(4u<<10); copy.words[3]=(0x10000000u+32)>>5;
    copy.words[4]=3; copy.words[9]=3u<<4; input={rows,4,8,16,sizeof(rows)};
    std::memset(ram,0x55,sizeof(ram));
    require(storeGxCopyBytes(history,102,copy,input,{ram,0x10000000u,192},packed,error)==0 &&
        packed->rowBytes()==32 && packed->rows()==2 && packed->ramStride()==96 && packed->ramExtent()==128 &&
        ram[32]==0x9A && ram[33]==0x12 && ram[128]==0x9A && ram[129]==0x56,
        "original physical RAM translation and block-row stride determine separate writes");
    for (unsigned i=64;i<128;++i) require(ram[i]==0x55,"unwritten RAM gaps are preserved byte-for-byte");
    auto old=packed; packed.reset();
    // Overlapping/zero rows use increasing hardware write order, not rejection
    // or a contiguous memcpy. Immutable source rows remain the original copy.
    unsigned char overlap[16*8*4]{};
    for (unsigned i=0;i<128;++i) overlap[i*4]=i<64 ? 0x10 : 0x20;
    copy.words[0]=31; copy.words[2]=8u|(4u<<10); copy.words[4]=1; copy.words[9]=1u<<4;
    input={overlap,16,8,64,sizeof(overlap)}; std::memset(ram,0x55,sizeof(ram));
    require(storeGxCopyBytes(history,102,copy,input,{ram,0x10000000u,sizeof(ram)},packed,error)==0 &&
        packed->rowBytes()==64 && packed->ramExtent()==96 && packed->data()[0]==0x10 && packed->data()[64]==0x20,
        "overlapping destination rows retain the full immutable packed source");
    for (unsigned i=32;i<128;++i) require(ram[i]==(i<64 ? 0x10 : 0x20),"later block-row writes win actual overlapping RAM bytes");
    auto newer=packed; packed.reset(); copy.words[0]=32; copy.words[4]=0;
    require(storeGxCopyBytes(history,103,copy,input,{ram,0x10000000u,sizeof(ram)},packed,error)==0 &&
        packed->ramExtent()==64 && ram[32]==0x20 && ram[95]==0x20,"zero hardware row stride repeatedly writes the same destination row");
    auto newest=packed; packed.reset();
    require(lookupGxCopyBytes(history,102,30,0x10000020u,packed,error)==0 && packed==old &&
        packed->data()[1]==0x12 && packed->data()[33]==0x56,"later overwrites never mutate an earlier exact copy ID lease");
    packed.reset();
    require(lookupGxCopyBytes(history,101,31,0x10000020u,packed,error)<0 && !packed &&
        lookupGxCopyBytes(history,104,31,0x10000040u,packed,error)<0 && !packed,
        "history lookup cannot substitute a future origin or the wrong physical address");
    require(lookupGxCopyBytes(history,104,31,0x10000020u,packed,error)==0 && packed==newer,
        "cross-frame exact copy lookup retains the original immutable packet");
    packed.reset(); std::memset(ram,0x55,sizeof(ram)); copy.words[0]=33;
    require(storeGxCopyBytes(history,102,copy,input,{ram,0x10000000u,sizeof(ram)},packed,error)<0 && !packed && ram[32]==0x55,
        "decreasing-frame copy cannot replay old writes into current RAM");
    copy.words[0]=32;
    require(storeGxCopyBytes(history,103,copy,input,{ram,0x10000000u,sizeof(ram)},packed,error)<0 && !packed && ram[32]==0x55,
        "duplicate frame/copy ID cannot replay committed RAM writes");
    copy.words[0]=33;
    require(storeGxCopyBytes(history,104,copy,input,{ram,0x10000000u,95},packed,error)<0 && !packed && ram[32]==0x55,
        "complete destination extent is validated before the first write");
    // Current zero-stride extent64 needs window offset32 +64=96 bytes.
    auto bad=input; bad.width=15;
    require(storeGxCopyBytes(history,104,copy,bad,{ram,0x10000000u,sizeof(ram)},packed,error)<0 && !packed && ram[32]==0x55,
        "logical-only or incorrectly padded producer input is rejected before writes");
    bad=input; --bad.bytes;
    require(storeGxCopyBytes(history,104,copy,bad,{ram,0x10000000u,sizeof(ram)},packed,error)<0 && !packed,
        "source availability includes the complete final padded row");
    copy.words[9]=1u<<14;
    require(storeGxCopyBytes(history,104,copy,input,{ram,0x10000000u,sizeof(ram)},packed,error)<0 && !packed && ram[32]==0x55,
        "XFB cannot silently use tiled texture-copy encoding");
    copy.words[9]=(5u<<4)|8;
    require(storeGxCopyBytes(history,104,copy,input,{ram,0x10000000u,sizeof(ram)},packed,error)<0 && !packed,
        "reserved texture-copy format never falls back to RGBA");
    copy.words[9]=1u<<4;
    for (unsigned failure=1;failure<=2;++failure) {
        cpu_fail_any=true; cpu_fail_after=failure; bool threw=false; int result=0;
        try { result=storeGxCopyBytes(history,104,copy,input,{ram,0x10000000u,sizeof(ram)},packed,error); }
        catch (const std::bad_alloc&) { threw=true; }
        cpu_fail_after=0; cpu_fail_any=false;
        require(!threw && result<0 && !packed && ram[32]==0x55,"checked record/control-block and byte-buffer OOM leave RAM/history untouched");
    }
    require(storeGxCopyBytes(history,104,copy,input,{ram,0x10000000u,sizeof(ram)},packed,error)==0 && packed,
        "failed preparation does not poison a later actual ordered copy write");
    auto successful=packed;
    require(storeGxCopyBytes(history,105,copy,input,{ram,0x10000000u,sizeof(ram)},packed,error)<0 && packed==successful,
        "nonempty publication output retains its original immutable lease");
    require(collectGxCopyBytes(history,103)==0 && old->data()[1]==0x12 && newest->data()[0]==0x10,
        "retirement cannot free leased older versions or mutate their bytes");
    old.reset(); newer.reset(); newest.reset(); packed.reset();
    require(collectGxCopyBytes(history,103)==0,"retired unleased overwritten copies can be collected");
    require(lookupGxCopyBytes(history,105,31,0x10000020u,packed,error)<0 && !packed,
        "collected overwritten history does not return a newer copy by address alone");
    require(releaseGxCopyHistory(&history)<0 && history &&
        lookupGxCopyBytes(history,105,33,0x10000020u,packed,error)<0 && !packed,
        "outstanding final lease retains a closing history owner and stops new lookups");
    successful.reset();
    require(releaseGxCopyHistory(&history)==0 && !history,
        "dropped CPU copy leases permit complete explicit history retirement");

    require(createGxCopyHistory(nullptr)<0,"history creation requires an explicit owner output");
    cpu_fail_any=true; cpu_fail_after=1;
    const int createFailure=createGxCopyHistory(&history);
    cpu_fail_any=false; cpu_fail_after=0;
    require(createFailure<0 && !history,"history allocation OOM cannot publish a missing owner");
    require(createGxCopyHistory(&history)==0 && createGxCopyHistory(&history)<0,
        "nonempty history creation cannot overwrite an existing owner");
    // Nonuniform multiple blocks catch linear output, transposition, bad row
    // pitch and a write-before-pack implementation when input aliases RAM.
    unsigned char pitched[8*72]; std::memset(pitched,0xFE,sizeof(pitched));
    for (unsigned y=0;y<8;++y) for (unsigned x=0;x<16;++x) {
        auto* p=pitched+y*72+x*4;
        p[0]=unsigned(y*16+x); p[1]=0x34; p[2]=0x56; p[3]=0x78;
    }
    copy={}; copy.efbWidth=640; copy.efbHeight=456; copy.words[0]=1;
    copy.words[2]=15|(7u<<10); copy.words[3]=1; copy.words[4]=2; copy.words[9]=1u<<4;
    input={pitched,16,8,72,sizeof(pitched)};
    require(storeGxCopyBytes(history,1,copy,input,{pitched,0,sizeof(pitched)},packed,error)==0 &&
        packed->data()[0]==0 && packed->data()[7]==7 && packed->data()[8]==16 && packed->data()[31]==55 &&
        packed->data()[32]==8 && packed->data()[63]==63 && packed->data()[64]==64 && packed->data()[127]==127 &&
        pitched[32]==0 && pitched[159]==127,
        "distinct texels encode original tile order with noncontiguous aliasing input fully packed before RAM writes");
    const uint64_t firstSequence=packed->sequence(); packed.reset();
    // Half-size destination uses ceil division even for odd original extents;
    // the samples have already been produced, not filtered by this byte encoder.
    copy.words[0]=2; copy.words[2]=8|(8u<<10); copy.words[9]=(6u<<4)|(1u<<9); copy.words[4]=4;
    input={pixels,8,8,32,sizeof(pixels)};
    require(storeGxCopyBytes(history,2,copy,input,{ram,0,sizeof(ram)},packed,error)==0 &&
        packed->width()==5 && packed->height()==5 && packed->rowBytes()==128 && packed->rows()==2 &&
        packed->bytes()==256 && packed->sequence()>firstSequence,
        "odd half-copy extents retain complete RGBA8 split-plane tiles and monotonic commit sequence");
    packed.reset(); copy.words[0]=3;
    require(storeGxCopyBytes(history,2,copy,input,{ram,0,sizeof(ram)},packed,error)==0,"same-frame replacement for retirement replay guard");
    packed.reset();
    require(collectGxCopyBytes(history,2)==0,"collection accepts cutoff without making current-frame replay possible");
    copy.words[0]=2; std::memset(ram,0x55,sizeof(ram));
    require(storeGxCopyBytes(history,2,copy,input,{ram,0,sizeof(ram)},packed,error)<0 && ram[32]==0x55 && !packed,
        "collecting an overwritten current-frame packet cannot forget its committed copy ID");
    auto invalidCopy=copy; invalidCopy.words[0]=4; invalidCopy.words[1]=639;
    require(storeGxCopyBytes(history,3,invalidCopy,input,{ram,0,sizeof(ram)},packed,error)<0 && ram[32]==0x55,
        "logical copy rectangle must fit command-time EFB before any writes");
    invalidCopy=copy; invalidCopy.words[0]=4; invalidCopy.words[3]=0x1000000;
    require(storeGxCopyBytes(history,3,invalidCopy,input,{ram,0,sizeof(ram)},packed,error)<0 && ram[32]==0x55,
        "oversized BP payload cannot silently truncate to a different destination");
    require(storeGxCopyBytes(history,3,copy,input,{ram,0xFFFFFFF0u,32},packed,error)<0 && ram[32]==0x55,
        "RAM physical window cannot wrap the original 32-bit address space");
    bad=input; bad.stride=31;
    require(storeGxCopyBytes(history,3,copy,bad,{ram,0,sizeof(ram)},packed,error)<0 && ram[32]==0x55,
        "source row pitch cannot be smaller than a complete padded RGBA row");
    require(lookupGxCopyBytes(history,2,3,32,packed,error)==0,"latest unleased packet survives collection for later-frame sampling");
    require(lookupGxCopyBytes(history,2,2,32,packed,error)<0 && packed->command().words[0]==3,
        "lookup rejects nonempty output without replacing its lease");
    packed.reset();
    require(releaseGxCopyHistory(&history)==0 && !history,"complete alias/half/validation history closes cleanly");
}
// Controlled driver-result pixels distinguish RAM-before-clear and failed
// capture/failed clear commit phases. No shader emulation or GPU-pixel claim.
static unsigned char* copyRam;
static void* copySource;
static bool failCopyClear;
static unsigned copyClears;
void copyDrawResult() {
    if (active_color_data==copySource) {
        require(copyRam[32]==48 && copyRam[33]==16 && copyRam[64]==65 && copyRam[65]==130,
            "copy clear can execute only after original packed RAM bytes are committed");
        ++copyClears;
        std::memset(active_color_data,0,color_planes.at(active_color_data).stride*color_planes.at(active_color_data).height*4);
        if (failCopyClear) end_error=-920;
    } else {
        const auto extent=color_planes.at(active_color_data);
        auto* data=static_cast<unsigned char*>(active_color_data);
        for (unsigned y=0;y<extent.height;++y) for (unsigned x=0;x<extent.stride;++x) {
            auto* p=data+(size_t(y)*extent.stride+x)*4;
            p[0]=16; p[1]=64; p[2]=128; p[3]=48;
        }
        reflect_texture=false; // subsequent real clear has no sampler
    }
}
void efbCopyCommit() {
    std::string error; error.reserve(256); VitaShaderLibrary library; installCaptureShaders(library);
    PortVitaGxmDevice* device=nullptr; VitaEfbCache* cache=nullptr; GxCopyHistory* history=nullptr;
    PortVitaGxmSurface* source=nullptr; std::shared_ptr<const void> sourceLease;
    require(port_vita_gxm_device_create(&device)==0 && createVitaEfbCache(device,library,&cache,error)==0 &&
        createGxCopyHistory(&history)==0 && resolveVitaEfb(cache,10,10,source,sourceLease,error)==0,
        "ordered native copy begins with real device, EFB cache and CPU history owners");
    PortVitaGxmSurfaceInfo info{}; require(port_vita_gxm_surface_info(source,&info)==0,"native copy source storage");
    unsigned char ram[192]; std::memset(ram,0xA5,sizeof(ram)); copyRam=ram; copySource=const_cast<void*>(info.color_data);
    draw_result=copyDrawResult; copyClears=0; failCopyClear=false;
    SpanCopy copy{}; copy.efbWidth=10; copy.efbHeight=10; copy.pixelFormat=1; copy.colorMode=0x18; copy.depthMode=0x10;
    copy.words[0]=1; copy.words[1]=1|(2u<<10); copy.words[2]=3|(3u<<10); copy.words[3]=1; copy.words[4]=2;
    copy.words[6]=0x0711; copy.words[7]=0x2233; copy.words[8]=0x112233; copy.words[9]=(6u<<4)|(1u<<11);
    copy.filter[0]=21u<<12|22u<<18; copy.filter[1]=21;
    std::shared_ptr<const GxCopyBytes> packet;
    copy.efbWidth=9; unsigned draws=native_draws; reflect_texture=true;
    require(copyVitaEfbToMemory(cache,history,source,1,copy,{ram,0,sizeof(ram)},packet,error)<0 && !packet &&
        native_draws==draws && ram[32]==0xA5,"native copy rejects wrong full-source provenance before capture or RAM changes");
    copy.efbWidth=10;
    require(copyVitaEfbToMemory(cache,history,source,1,copy,{ram,0,48},packet,error)<0 && !packet && !copyClears &&
        ram[32]==0xA5,"out-of-window native copy never clears source or partially mutates RAM");
    reflect_texture=true;
    require(copyVitaEfbToMemory(cache,history,source,1,copy,{ram,0,sizeof(ram)},packet,error)==0 && packet &&
        packet->frame()==1 && ram[32]==48 && ram[33]==16 && ram[64]==65 && ram[65]==130 &&
        ram[31]==0xA5 && ram[96]==0xA5 && copyClears==1 && !active_scene && !pending_gpu,
        "real native capture feeds processing, RAM packing and independent masks before source clear");
    draws=native_draws;
    require(copyVitaEfbToMemory(cache,history,source,1,copy,{ram,0,sizeof(ram)},packet,error)<0 && native_draws==draws && packet,
        "nonempty copy commit marker cannot be overwritten or automatically replayed");
    packet.reset(); std::memset(ram,0xA5,sizeof(ram)); copy.words[0]=2; end_error=-919; reflect_texture=true;
    require(copyVitaEfbToMemory(cache,history,source,2,copy,{ram,0,sizeof(ram)},packet,error)==-919 && !packet &&
        ram[32]==0xA5 && copyClears==1 && active_scene,
        "capture End failure retains native work but commits neither RAM nor clear");
    end_error=0; draws=native_draws;
    require(discardVitaEfbClear(cache)==0 && native_draws==draws,"failed native copy capture recovery never reissues GPU work");
    copy.words[0]=3; reflect_texture=true; failCopyClear=true;
    require(copyVitaEfbToMemory(cache,history,source,3,copy,{ram,0,sizeof(ram)},packet,error)==-920 && packet &&
        packet->frame()==3 && ram[32]==48 && copyClears==2 && active_scene,
        "post-commit clear failure reports the durable RAM/history marker rather than false rollback");
    std::shared_ptr<const GxCopyBytes> lookup;
    require(lookupGxCopyBytes(history,3,3,32,lookup,error)==0 && lookup==packet,
        "failed clear cannot erase the already committed ordered copy origin");
    end_error=0; failCopyClear=false; draws=native_draws;
    require(discardVitaEfbClear(cache)==0 && native_draws==draws && !active_scene && !pending_gpu,
        "post-commit clear End recovery acknowledges without recapture, rewrite or redraw");
    packet.reset(); lookup.reset(); copy.words[0]=4; copy.words[9]&=~(1u<<11); reflect_texture=true;
    require(copyVitaEfbToMemory(cache,history,source,4,copy,{ram,0,sizeof(ram)},packet,error)==0 && packet && copyClears==2,
        "copy without clear flag writes original bytes without silently clearing the EFB");
    draw_result=nullptr; packet.reset(); sourceLease.reset(); source=nullptr;
    require(releaseGxCopyHistory(&history)==0 && releaseVitaEfbCache(&cache)==0 && port_vita_gxm_device_release(&device)==0,
        "native copy operation retains no inaccessible capture, CPU or GPU lifetime after fences"); empty_state();
}
// Catches packet-vs-RAM substitution, prefix-only invalidation, copy-dimension
// reinterpretation, omitted palette/mips, missing origin and mutable GPU reuse.
static unsigned char* xfbRam;
static const void* xfbSource;
static const void* xfbColorDescriptor;
static unsigned xfbClears;
static bool xfbFailClear;
void xfbDrawResult() {
    if (active_color_data==xfbSource) {
        const unsigned char expected[]={82,81,144,188,41,165,235,95};
        require(std::memcmp(xfbRam+32,expected,8)==0 && std::memcmp(xfbRam+64,expected,8)==0,
            "native XFB clear runs only after original linear RAM rows are committed");
        ++xfbClears;
        if (xfbFailClear) end_error=-952;
    } else {
        require(last_sampled_descriptor==xfbColorDescriptor && default_uniforms[0]==0 && default_uniforms[1]==0,
            "native XFB transport samples its actual full source color descriptor, never encoded depth");
        const auto extent=color_planes.at(active_color_data);
        require(extent.height==6 && default_uniforms[22]==8 && default_uniforms[23]==6,
            "native XFB capture retains full original EFB coordinates for filter neighbors");
        const unsigned char colors[4][4]={{255,0,0,7},{0,255,0,8},{0,0,255,9},{255,255,255,10}};
        auto* data=static_cast<unsigned char*>(active_color_data);
        for (unsigned y=0;y<extent.height;++y) for (unsigned x=0;x<extent.stride;++x)
            std::memcpy(data+(size_t(y)*extent.stride+x)*4,colors[(x+2)%4],4);
        reflect_texture=false;
    }
}
void nativeXfbCopyCommit() {
    std::string error; error.reserve(256); VitaShaderLibrary library; installCaptureShaders(library);
    PortVitaGxmDevice* device=nullptr; VitaEfbCache* cache=nullptr; GxCopyHistory* history=nullptr;
    PortVitaGxmSurface* source=nullptr; std::shared_ptr<const void> lease;
    require(port_vita_gxm_device_create(&device)==0 && createVitaEfbCache(device,library,&cache,error)==0 &&
        createGxCopyHistory(&history)==0 && resolveVitaEfb(cache,8,6,source,lease,error)==0,
        "native original XFB operation owns real EFB, capture and ordered RAM history");
    PortVitaGxmSurfaceInfo info{}; PortVitaGxmSurfaceSampleInfo sample{};
    require(port_vita_gxm_surface_info(source,&info)==0 && port_vita_gxm_surface_sample(device,source,&sample)==0,
        "native XFB source has original color and separate depth sampling provenance");
    unsigned char ram[128]; std::memset(ram,0x55,sizeof(ram)); xfbRam=ram; xfbSource=info.color_data;
    xfbColorDescriptor=sample.color_texture; xfbClears=0; xfbFailClear=false; draw_result=xfbDrawResult;
    SpanCopy copy{}; copy.efbWidth=8; copy.efbHeight=6; copy.pixelFormat=1;
    copy.colorMode=0x18; copy.depthMode=0x10;
    copy.words[0]=1; copy.words[1]=2|(2u<<10); copy.words[2]=3|(1u<<10); copy.words[3]=1; copy.words[4]=1;
    copy.words[5]=256; copy.words[6]=0x0711; copy.words[7]=0x2233; copy.words[8]=0x112233;
    copy.words[9]=(1u<<14)|(1u<<11)|3; copy.filter[0]=21u<<12|22u<<18; copy.filter[1]=21;
    std::shared_ptr<const GxCopyBytes> packet; reflect_texture=true;
    require(copyVitaEfbToMemory(cache,history,source,1,copy,{ram,0,sizeof(ram)},packet,error)==0 && packet &&
        packet->format()==15 && xfbClears==1 && !active_scene && !pending_gpu && ram[31]==0x55 && ram[72]==0x55,
        "native XFB capture/filter/YUYV/RAM/clear completes in original order before returning to the guest");
    packet.reset(); copy.words[0]=2; end_error=-951; reflect_texture=true;
    std::memset(ram,0x55,sizeof(ram));
    require(copyVitaEfbToMemory(cache,history,source,2,copy,{ram,0,sizeof(ram)},packet,error)==-951 && !packet &&
        active_scene && ram[32]==0x55 && xfbClears==1,
        "failed XFB capture End retains GPU ownership but neither writes RAM nor clears the source");
    end_error=0; unsigned draws=native_draws;
    require(discardVitaEfbClear(cache)==0 && native_draws==draws,"XFB capture cleanup never recaptures or rewrites RAM");
    copy.words[0]=3; reflect_texture=true; xfbFailClear=true;
    require(copyVitaEfbToMemory(cache,history,source,3,copy,{ram,0,sizeof(ram)},packet,error)==-952 &&
        packet && packet->format()==15 && packet->frame()==3 && xfbClears==2 && active_scene,
        "failed post-XFB clear preserves its irreversible linear RAM/history commit marker");
    end_error=0; xfbFailClear=false; draws=native_draws;
    require(discardVitaEfbClear(cache)==0 && native_draws==draws,"post-XFB clear cleanup fences without repeat work");
    packet.reset(); copy.words[0]=4; copy.words[9]&=~(1u<<11); copy.pixelFormat=3; reflect_texture=true;
    require(copyVitaEfbToMemory(cache,history,source,4,copy,{ram,0,sizeof(ram)},packet,error)==0 && packet && xfbClears==2,
        "Z24 pixel type still copies color into XFB rather than presenting encoded depth or silently clearing");
    packet.reset(); copy.words[0]=5; copy.words[9]|=1u<<10; copy.words[5]=128; draws=native_draws;
    require(copyVitaEfbToMemory(cache,history,source,5,copy,{ram,0,sizeof(ram)},packet,error)<0 && !packet && native_draws==draws,
        "unsupported original XFB vertical resampling fails before native capture or RAM side effects");
    draw_result=nullptr; reflect_texture=false; lease.reset(); source=nullptr;
    require(releaseGxCopyHistory(&history)==0 && releaseVitaEfbCache(&cache)==0 && port_vita_gxm_device_release(&device)==0,
        "all original XFB source/capture/staging/program/history owners retire explicitly"); empty_state();
}
void ramTextures() {
    std::string error; error.reserve(256); GxCopyHistory* history=nullptr; VitaTextureCache* cache=nullptr;
    PortVitaGxmDevice* device=nullptr;
    require(createGxCopyHistory(&history)==0 && port_vita_gxm_device_create(&device)==0 && createVitaTextureCache(device,&cache)==0,
        "RAM texture integration uses real copy history, device and native image owners");
    unsigned char ram[384]; std::memset(ram,0xAA,sizeof(ram)); unsigned char texels[4*8*4];
    for (unsigned i=0;i<32;++i) { texels[i*4]=0x10; texels[i*4+1]=0x20; texels[i*4+2]=0x30; texels[i*4+3]=0x40; }
    SpanCopy copy{}; copy.efbWidth=copy.efbHeight=16; copy.words[0]=1; copy.words[2]=3|(7u<<10);
    copy.words[3]=1; copy.words[4]=3; copy.words[9]=6u<<4;
    std::shared_ptr<const GxCopyBytes> origin;
    require(storeGxCopyBytes(history,127,copy,{texels,4,8,16,sizeof(texels)},{ram,0,sizeof(ram)},origin,error)==0,
        "real copy writer creates stride gaps in authoritative RAM, not in immutable packet rows");
    SpanRegisters regs{}; regs.bp[0x88]=3|(7u<<10)|(TF_RGBA8<<20); regs.bp[0x94]=1;
    SpanTexture binding{}; binding.kind=SpanTextureKind::EfbAddress; binding.copyId=1; binding.address=32;
    binding.width=4; binding.height=8; binding.levels=1;
    VitaRamTextures context{history,{ram,0,sizeof(ram),nullptr,0,0,127,origin->sequence()}};
    PortVitaGxmTextureView *first=nullptr,*again=nullptr,*changed=nullptr; std::shared_ptr<const void> fl,al,cl;
    std::shared_ptr<const GxRamTexture> fp,ap,cp;
    require(resolveVitaRamImage(cache,127,binding,regs,0,context,first,fl,fp,error)==0 &&
        fp->rgba()[0]==0x40302010 && fp->rgba()[16]==0xAA1040AA,
        "RAM texture decodes actual preserved stride gap instead of pretending packet rows are contiguous RAM");
    PortVitaGxmTextureViewInfo fi{},ai{},ci{}; require(port_vita_gxm_texture_view_info(first,&fi)==0 &&
        static_cast<const uint32_t*>(fi.image.pixels)[4*8]==0xAA1040AA,"actual native upload contains composite gap pixels");
    require(resolveVitaRamImage(cache,127,binding,regs,0,context,again,al,ap,error)==0 && again==first && ap==fp,
        "unchanged complete RAM content reuses the immutable native image and exact sampler");
    again=nullptr; al.reset(); ap.reset(); regs.bp[0x80]=1; binding.mode0=1;
    require(resolveVitaRamImage(cache,127,binding,regs,0,context,again,al,ap,error)==0 && again!=first &&
        port_vita_gxm_texture_view_info(again,&ai)==0 && ai.image.pixels==fi.image.pixels && ap==fp,
        "distinct GX sampler states share one verified immutable composite RAM image");
    again=nullptr; al.reset(); ap.reset(); regs.bp[0x80]=0; binding.mode0=0;
    unsigned char partial[8*4*4]; std::memset(partial,0x77,sizeof(partial));
    copy.words[0]=2; copy.words[2]=7|(3u<<10); copy.words[3]=2; copy.words[4]=1; copy.words[9]=1u<<3;
    std::shared_ptr<const GxCopyBytes> patch;
    require(storeGxCopyBytes(history,127,copy,{partial,8,4,32,sizeof(partial)},{ram,0,sizeof(ram)},patch,error)==0,
        "actual later cross-base copy overwrites only first texture GB bytes"); context.memory.copySequence=patch->sequence();
    require(resolveVitaRamImage(cache,127,binding,regs,0,context,changed,cl,cp,error)==0 && cp->rgba()[0]==0x40777710 &&
        cp->rgba()[16]==0xAA1040AA && fp->rgba()[0]==0x40302010,
        "later overlapping copy is resolved from real RAM while earlier leased pixels remain immutable");
    changed=nullptr; cl.reset(); cp.reset(); ram[97]=0xF1;
    require(resolveVitaRamImage(cache,127,binding,regs,0,context,changed,cl,cp,error)==0 && cp->rgba()[16]==0xAA1040F1 &&
        port_vita_gxm_texture_view_info(changed,&ci)==0 && ci.image.pixels!=fi.image.pixels && fp->rgba()[16]==0xAA1040AA,
        "CPU change beyond first64 bytes forces exact full-content decode and fresh native storage");
    changed=nullptr; cl.reset(); cp.reset(); copy.words[0]=3; copy.words[2]=3|(3u<<10); copy.words[3]=1;
    copy.words[4]=2; copy.words[9]=6u<<4;
    for (unsigned i=0;i<16;++i) { texels[i*4]=0x40; texels[i*4+1]=0x50; texels[i*4+2]=0x60; texels[i*4+3]=0x80; }
    require(storeGxCopyBytes(history,127,copy,{texels,4,4,16,64},{ram,0,sizeof(ram)},patch,error)<0,
        "occupied packet output cannot lose prior overlap provenance"); patch.reset();
    require(storeGxCopyBytes(history,127,copy,{texels,4,4,16,64},{ram,0,sizeof(ram)},patch,error)==0,
        "shorter same-base copy updates RAM head without erasing its old tail");
    binding.copyId=3; binding.height=4; context.memory.copySequence=patch->sequence();
    require(resolveVitaRamImage(cache,127,binding,regs,0,context,changed,cl,cp,error)==0 && cp->layout().height==8 &&
        cp->rgba()[0]==0x80605040 && cp->rgba()[16]==0xAA1040F1,
        "later BP dimensions reinterpret shorter copy plus untouched tail without RGBA capture resampling");
    changed=nullptr; cl.reset(); cp.reset(); GxRamTextureLayout layout;
    const uint32_t formats[]={TF_CI4,TF_CI8,TF_CI14X2}; const uint32_t wanted[]={0x60202020,0xFF0000FF,0xFF00FF00};
    unsigned char palette[32768]{};
    for (unsigned i=0;i<3;++i) {
        const unsigned width=i==2 ? 4 : 8,height=i==0 ? 8 : 4;
        regs.bp[0x88]=(width-1)|((height-1)<<10)|(formats[i]<<20); regs.bp[0x98]=1|(i<<10);
        std::memset(ram+32,i==0 ? 0x11 : 1,32);
        if (i==2) for (unsigned p=0;p<16;++p) { ram[32+p*2]=0x40; ram[33+p*2]=1; }
        palette[2]=i==0 ? 0x60 : i==1 ? 0xF8 : 0x83; palette[3]=i==0 ? 0x20 : i==1 ? 0 : 0xE0;
        context.memory.tlut=palette; context.memory.tlutBase=512; context.memory.tlutBytes=i==0 ? 32 : i==1 ? 512 : sizeof(palette);
        require(resolveVitaRamImage(cache,127,binding,regs,0,context,changed,cl,cp,error)==0 && cp->rgba()[0]==wanted[i],
            "CI4/CI8/CI14X2 consume complete correctly banked original big-endian palettes, no grayscale fallback");
        changed=nullptr; cl.reset(); cp.reset();
        --context.memory.tlutBytes;
        require(resolveVitaRamImage(cache,127,binding,regs,0,context,changed,cl,cp,error)<0 && !changed && !cl && !cp,
            "one missing required palette byte rejects without publishing a substitute image"); ++context.memory.tlutBytes;
    }
    regs.bp[0x98]=0; regs.bp[0x88]=7|(7u<<10)|(TF_I8<<20); regs.bp[0x80]=6u<<5; regs.bp[0x84]=0x3000;
    binding.mode0=6u<<5; binding.mode1=0x3000;
    std::memset(ram+32,0x11,64); std::memset(ram+96,0x22,32); std::memset(ram+128,0x33,32); std::memset(ram+160,0x44,32);
    require(resolveVitaRamImage(cache,127,binding,regs,0,context,changed,cl,cp,error)==0 && cp->layout().levels==4 &&
        cp->rgba()[0]==0x11111111 && cp->rgba()[64]==0x22222222 && cp->rgba()[80]==0x33333333 && cp->rgba()[84]==0x44444444,
        "original padded mip offsets decode every authored level rather than regenerate or drop tails");
    changed=nullptr; cl.reset(); cp.reset(); regs.bp[0x84]=0x1100; binding.mode1=0x1100;
    require(describeGxRamTexture(regs,0,layout,error)==0 && layout.levels==3 && layout.packedBytes==128,
        "fractional max LOD preserves the reachable ceil level before native strategy validation");
    require(resolveVitaRamImage(cache,127,binding,regs,0,context,changed,cl,cp,error)<0 && !changed && !cl && !cp,
        "reachable fractional sampler remains explicitly unsupported, not truncated by RAM decode");
    regs.bp[0x80]=0; regs.bp[0x84]=0; binding.mode0=binding.mode1=0;
    context.memory.frame=128;
    require(resolveVitaRamImage(cache,127,binding,regs,0,context,changed,cl,cp,error)<0 && !changed,
        "future or mismatched frame RAM cannot be substituted for the requesting draw"); context.memory.frame=127;
    context.memory.copySequence=1;
    require(resolveVitaRamImage(cache,127,binding,regs,0,context,changed,cl,cp,error)<0 && !changed,
        "snapshot cut cannot claim a copy origin not yet committed within that snapshot"); context.memory.copySequence=patch->sequence();
    regs.bp[0x8C]=1u<<21;
    require(resolveVitaRamImage(cache,127,binding,regs,0,context,changed,cl,cp,error)<0 && !changed,
        "manual TMEM fetch cannot silently be replaced with console RAM"); regs.bp[0x8C]=0;
    context.memory.ramBytes=95;
    require(resolveVitaRamImage(cache,127,binding,regs,0,context,changed,cl,cp,error)<0 && !changed,
        "complete tiled texture RAM extent is validated before native upload"); context.memory.ramBytes=sizeof(ram);
    GxRamTexture decoded; cpu_fail_any=true; cpu_fail_after=1;
    int failed=decodeGxRamTexture(regs,0,context.memory,patch,decoded,error); cpu_fail_any=false; cpu_fail_after=0;
    require(failed<0 && !decoded.rgba(),"RAM decoder allocation failure publishes no partial source");
    first=nullptr; fl.reset(); fp.reset(); origin.reset(); patch.reset();
    require(releaseVitaTextureCache(&cache)==0 && releaseGxCopyHistory(&history)==0 && port_vita_gxm_device_release(&device)==0,
        "native RAM views retire before exact origin leases and explicit history/device close"); empty_state();
}

struct RamBackend final : VitaFrameBackend {
    PortVitaGxmDevice* device=nullptr;
    VitaMaterialCache* materials=nullptr;
    VitaTextureCache* textures=nullptr;
    VitaEfbCache* efbs=nullptr;
    GxCopyHistory* history=nullptr;
    PortVitaGxmSurface* target=nullptr;
    std::shared_ptr<const void> targetLease;
    std::shared_ptr<const GxCopyBytes> packet;
    VitaShaderLibrary library;
    unsigned char ram[192]{};
    bool omitProof=false;
    int resolve(const IndexChunks& source,const IndexChunk& chunk,const VitaPreparedDraw& prepared,
                 VitaDrawBinding& out,std::string& error) override {
        VitaRamTextures context{history,{ram,0,sizeof(ram),nullptr,0,0,source.frame().number,packet->sequence()}};
        const int result=resolveVitaDrawWithRam(device,materials,textures,library,source,chunk,prepared,context,out,error);
        if (result>=0) {
            require(out.ramSources[0] && out.ramSources[0]->layout().width==8 &&
                out.ramSources[0]->rgba()[0]==0x30303030 && out.ramSources[0]->rgba()[1]==0x10101010 &&
                out.ramSources[0]->rgba()[32]==0x41414141 && out.ramSources[0]->rgba()[33]==0x82828282,
                "actual native capture and RAM writer feed BP I8 reinterpretation, not copied RGBA resampling");
            if (omitProof) out.ramSources[0].reset();
        }
        return result;
    }
    int surface(uint32_t width,uint32_t height,PortVitaGxmSurface*& out,std::shared_ptr<const void>& lease,
                std::string&) override {
        require(width==10 && height==10,"RAM replay keeps original full EFB dimensions");
        out=target; lease=targetLease; return 0;
    }
    int copy(const Frame& frame,const SpanCopy& copy,std::string& error) override {
        PortVitaGxmSurfaceInfo info{};
        require(port_vita_gxm_surface_info(target,&info)==0,"RAM replay capture source belongs to real EFB cache");
        copyRam=ram; copySource=const_cast<void*>(info.color_data); draw_result=copyDrawResult; reflect_texture=true;
        const int result=copyVitaEfbToMemory(efbs,history,target,frame.number,copy,{ram,0,sizeof(ram)},packet,error);
        draw_result=nullptr; reflect_texture=true; return result;
    }
    int marker(const Frame&,const SpanMarker&,std::string&) override { return 0; }
    int camera(const Frame&,const SpanCamera&,std::string&) override { return 0; }
};
void ramReplay() {
    auto makeSource=[](bool copy) {
        auto f=frame(); f->startBp[0x43]=1; f->startBp[0x53]=21u<<12|22u<<18; f->startBp[0x54]=21;
        f->startBp[0x88]=7|(7u<<10)|(TF_I8<<20); f->startBp[0x94]=1;
        f->startXfRegs[0x3F]=1; f->startBp[0x28]=64; f->startBp[0x21]=(9u<<12)|9u;
        f->startXfRegs[0x1A]=f->startXfRegs[0x1B]=f->startXfRegs[0x1D]=f->startXfRegs[0x1E]=bits(5);
        if (copy) command(*f,{CMD_EFB_COPY,1,1|(2u<<10),3|(3u<<10),1,2,0,0,0,0,6u<<4});
        command(*f,{CMD_TEX_EFB,0,1,4,4,32}); draw(*f,0x90,3);
        std::shared_ptr<const DrawSpans> spans; DrawSpanError se;
        require(buildDrawSpans(f,spans,se,{10,10}),"ordered native copy/texture draw spans");
        std::shared_ptr<const IndexChunks> result; IndexChunkError ce;
        require(buildIndexChunks(spans,result,ce),"ordered native copy/texture index chunks"); return result;
    };
    auto source=makeSource(true); RamBackend backend; std::string error; VitaFrameError frameError;
    installCaptureShaders(backend.library); installShaders(backend.library,source->chunks()[0].state->uid);
    VitaFrameReplay* replay=nullptr;
    require(port_vita_gxm_device_create(&backend.device)==0 && createVitaMaterialCache(backend.device,&backend.materials)==0 &&
        createVitaTextureCache(backend.device,&backend.textures)==0 && createGxCopyHistory(&backend.history)==0 &&
        createVitaEfbCache(backend.device,backend.library,&backend.efbs,error)==0 &&
        resolveVitaEfb(backend.efbs,10,10,backend.target,backend.targetLease,error)==0 &&
        createVitaFrameReplay(backend.device,&replay)==0,"real copy/RAM replay creates every native lifetime owner");
    const unsigned before=native_draws;
    require(submitVitaFrame(replay,source,backend,frameError) && backend.packet && native_draws==before+2 &&
        !active_scene && !pending_gpu,"ordered actual capture-to-RAM-to-native-texture replay submits and fences both draws");
    backend.omitProof=true; source=makeSource(false); const unsigned after=native_draws;
    require(!submitVitaFrame(replay,source,backend,frameError) && native_draws==after &&
        frameError.message.find("provenance")!=std::string::npos,"native replay rejects an otherwise valid view without typed RAM provenance before drawing");
    require(discardVitaFrame(replay)==0 && releaseVitaFrameReplay(&replay)==0,"rejected RAM replay retires without reissuing prior copy");
    source.reset(); backend.targetLease.reset(); backend.packet.reset();
    require(releaseVitaTextureCache(&backend.textures)==0 && releaseVitaMaterialCache(&backend.materials)==0 &&
        releaseGxCopyHistory(&backend.history)==0 && releaseVitaEfbCache(&backend.efbs)==0 &&
        port_vita_gxm_device_release(&backend.device)==0,"RAM replay releases typed sources and every native owner after fences");
    empty_state();
}

struct LiveConsumer final : RecorderConsumer,VitaFrameBackend {
    PortVitaGxmDevice* device=nullptr;
    VitaFrameReplay* replay=nullptr;
    VitaMaterialCache* materials=nullptr;
    VitaTextureCache* textures=nullptr;
    VitaEfbCache* efbs=nullptr;
    GxCopyHistory* history=nullptr;
    PortVitaGxmSurface* target=nullptr;
    std::shared_ptr<const void> targetLease;
    std::shared_ptr<const GxCopyBytes> packet;
    VitaShaderLibrary library;
    RecorderMemory memory;
    std::shared_ptr<const GxRamTexture> lastRam;
    std::vector<char> events;
    std::vector<std::shared_ptr<const Frame>> retained;
    std::vector<uint64_t> frames;
    bool reject=false,checkFractional=false;
    bool consume(std::shared_ptr<const Frame> f,const RecorderMemory& ram,std::string& error) override {
        if (reject) { error="native consumer rejected work"; return false; }
        require(hostScopeDepth && f && f->number,"actual parser delivery holds caller allocation scope and frame identity");
        if (checkFractional) require(f->textures.size()==1 && f->textures[0]->levels==2 && f->textures[0]->rgba.size()==160,
            "fractional original max LOD retains its complete ceil-reachable CPU mip before unsupported sampler rejection");
        memory=ram; retained.push_back(f); frames.push_back(f->number);
        auto source=chunks(f); VitaFrameError failure;
        for (const auto& chunk:source->chunks()) installShaders(library,chunk.state->uid);
        const bool result=submitVitaFrame(replay,source,*this,failure);
        error=failure.message;
        if (!result) std::fprintf(stderr,"live segment %zu failed at event %zu/draw %zu: %s\n",retained.size(),
            failure.sourceEventIndex,failure.drawIndex,error.c_str());
        memory={}; return result;
    }
    bool peek(const RecorderPeek& request,uint32_t& value,std::string&) override {
        require(request.x==7 && request.y==9 && !pending_gpu && !active_scene,"peek arrives only after prior real replay fences");
        require(request.pixelFormat==g.bp[0x43] && request.alphaRead==2,
            "original producer snapshots current BP pixel format and SDK PE alpha mode into each request");
        events.push_back(request.depth ? 'z' : 'p'); value=request.depth ? 0x123456u : 0x89ABCDEFu; return true;
    }
    bool present(const void* xfb,bool black,std::string&) override {
        require(xfb==liveRam+1024 && black && !pending_gpu && !active_scene,
            "native VI delivery preserves requested XFB and black state without inventing a display counter");
        events.push_back('v'); return true;
    }
    int resolve(const IndexChunks& source,const IndexChunk& chunk,const VitaPreparedDraw& prepared,
                VitaDrawBinding& out,std::string& error) override {
        reflect_texture=prepared.layout.textureMask!=0;
        VitaRamTextures ram{history,{liveRam,0,sizeof(liveRam),memory.tlut,0,memory.tlutBytes,
            source.frame().number,packet ? packet->sequence() : 0}};
        const int result=resolveVitaDrawWithRam(device,materials,textures,library,source,chunk,prepared,ram,out,error);
        if (result>=0) lastRam=out.ramSources[0];
        return result;
    }
    int surface(uint32_t width,uint32_t height,PortVitaGxmSurface*& out,std::shared_ptr<const void>& lease,
                std::string& error) override {
        PortVitaGxmSurface* resolved=nullptr; std::shared_ptr<const void> resolvedLease;
        const int result=resolveVitaEfb(efbs,width,height,resolved,resolvedLease,error);
        if (result>=0) { target=resolved; targetLease=std::move(resolvedLease); }
        out=target; lease=targetLease; events.push_back('d'); return result;
    }
    int copy(const Frame& f,const SpanCopy& copy,std::string& error) override {
        require(target && !active_scene && !pending_gpu,"real parser copy delivery follows completed native draws");
        packet.reset(); reflect_texture=true; draw_result=copyDrawResult;
        const int result=copyVitaEfbToMemory(efbs,history,target,f.number,copy,{liveRam,0,sizeof(liveRam)},packet,error);
        draw_result=nullptr; events.push_back('c'); return result;
    }
    int marker(const Frame&,const SpanMarker&,std::string&) override { events.push_back('m'); return 0; }
    int camera(const Frame&,const SpanCamera&,std::string&) override { events.push_back('a'); return 0; }
};
void liveRecorder() {
    require(attachRecorderConsumer && port_gx_recorder_present_xfb,
        "actual GX recorder has synchronous ordered native-consumer delivery");
    auto consumer=std::make_shared<LiveConsumer>(); std::string error;
    reflect_texture=false; installCaptureShaders(consumer->library);
    require(port_vita_gxm_device_create(&consumer->device)==0 && createVitaFrameReplay(consumer->device,&consumer->replay)==0 &&
        createVitaMaterialCache(consumer->device,&consumer->materials)==0 && createVitaTextureCache(consumer->device,&consumer->textures)==0 &&
        createVitaEfbCache(consumer->device,consumer->library,&consumer->efbs,error)==0 && createGxCopyHistory(&consumer->history)==0,
        "live parser integration owns real native replay/material/image/EFB/copy resources");
    auto initial=frame(); g={};
    std::memcpy(g.bp,initial->startBp,sizeof(g.bp)); std::memcpy(g.xf.regs,initial->startXfRegs,sizeof(g.xf.regs));
    std::memcpy(g.xf.mem,initial->startXfMem,sizeof(g.xf.mem));
    g.cp.vcdLo=1u<<9; g.cp.vatA[0]=1|(4u<<1);
    g.bp[0x43]=1; g.bp[0x53]=21u<<12|22u<<18; g.bp[0x54]=21;
    require(attachRecorderConsumer(consumer,error),"installed consumer replaces latest-only delivery without an already pending frame");
    require(!attachRecorderConsumer(consumer,error),"active native consumer cannot be silently replaced");
    std::vector<unsigned char> triangle{0x90,0,3};
    auto word=[](std::vector<unsigned char>& bytes,uint32_t value) {
        for (int shift:{24,16,8,0}) bytes.push_back(static_cast<unsigned char>(value>>shift));
    };
    for (unsigned i=0;i<3;++i) { word(triangle,bits(float(i)+1)); word(triangle,bits(2)); word(triangle,bits(-3)); }
    auto bp=[&](uint32_t reg,uint32_t value) {
        std::vector<unsigned char> bytes{0x61}; word(bytes,reg<<24|value);
        require(process(bytes.data(),bytes.size(),false)==bytes.size(),"real BP parser consumes complete original register command");
    };
    const unsigned initialDraws=native_draws;
    require(process(triangle.data(),triangle.size(),false)==triangle.size() && consumer->events==std::vector<char>{'d'} &&
        native_draws==initialDraws+1 && !active_scene && !pending_gpu,
        "first original GX primitive reaches real native replay and Finish before parser returns");
    bp(0x49,1|(2u<<10)); bp(0x4A,3|(3u<<10)); bp(0x4B,1); bp(0x4D,2); bp(0x52,6u<<4);
    require(consumer->packet && liveRam[32]==48 && liveRam[33]==16 && liveRam[64]==65 && liveRam[65]==130 &&
        consumer->events==std::vector<char>({'d','c'}),
        "actual GX copy trigger commits original packed RAM before CPU/next commands resume");
    bp(0x45,2);
    require(liveIrqs.size()==1 && liveRam[32]==48,"PE_DONE IRQ follows committed native copy instead of latest-frame publication");
    bp(0x88,7|(15u<<10)|(TF_I8<<20)); bp(0x94,1); bp(0x28,64);
    std::vector<unsigned char> xf{0x10}; word(xf,0x103F); word(xf,1);
    require(process(xf.data(),xf.size(),false)==xf.size(),"actual original XF load selects one texture generator");
    require(process(triangle.data(),triangle.size(),false)==triangle.size() && native_draws==initialDraws+3,
        "post-copy original draw resolves actual packed RAM through native typed provenance");
    auto spans=chunks(consumer->retained.back());
    require(spans->chunks()[0].state->textures[0].kind==SpanTextureKind::EfbAddress,
        "same-address copy association survives only as ordered origin, not prefix hash authority");
    liveRam[128]=0xE7;
    require(process(triangle.data(),triangle.size(),false)==triangle.size() && native_draws==initialDraws+4 &&
        consumer->lastRam && consumer->lastRam->rgba()[96]==0xE7E7E7E7u,
        "same-frame CPU mutation beyond legacy 64-byte prefix still reaches next native RAM snapshot");
    require(consumer->frames==std::vector<uint64_t>({1,1,1,1}) && frameQueue().latestNumber()==0,
        "all draw/copy segments retain true recording frame while bypassing lossy latest queue");
    reinterpret_cast<volatile uint16_t*>(__PortGXPERegs)[4]=6;
    require(gpu::backend()->peekColor(7,9)==0x89ABCDEFu && gpu::backend()->peekZ(7,9)==0x123456u,
        "real recorder delegates peeks after ordered work rather than placeholder zero/far depth");
    port_gx_recorder_present_xfb(liveRam+1024,1);
    require(consumer->events.back()=='v',"real recorder forwards native VI request rather than incrementing fake present count");
    // A consumed copy segment must be immutable even as live registers change.
    auto copied=chunks(consumer->retained[1]);
    require(copied->sourceSpans()->copies()[0].words[3]==1 && copied->sourceSpans()->copies()[0].words[9]==(6u<<4),
        "retained submitted copy preserves original command-time register values");
    float camera[kCameraWords]{}; camera[4]=1; camera[12]=5;
    port_gx_marker(MARK_HUD_BEGIN); port_gx_camera(camera);
    require(process(triangle.data(),triangle.size(),false)==triangle.size() &&
        process(triangle.data(),triangle.size(),false)==triangle.size(),"two real draws cross a live metadata continuation boundary");
    const auto metadata=chunks(consumer->retained.back());
    require(metadata->chunks()[0].state->markers.hud && metadata->chunks()[0].state->camera.valid &&
        metadata->chunks()[0].state->camera.words[12]==bits(5) &&
        std::count(consumer->events.begin(),consumer->events.end(),'m')==1 &&
        std::count(consumer->events.begin(),consumer->events.end(),'a')==1,
        "live segments preserve active HUD/first-camera state without replaying metadata callbacks");
    port_gx_marker(MARK_HUD_END);
    std::memset(liveRam+512,0x22,192); bp(0x94,16); bp(0x88,7|(15u<<10)|(TF_I8<<20));
    bp(0x80,1u<<5); bp(0x84,16u<<8);
    require(process(triangle.data(),triangle.size(),false)==triangle.size(),"original RAM texture with integer mip reach is streamed");
    auto firstImage=consumer->retained.back()->textures[0];
    require(firstImage->levels==2 && firstImage->rgba.size()==160 && firstImage->rgba[96]==0x22222222u,
        "actual recorder retains ceil-reachable authored mip and original full image content");
    const pid_t mipChild=fork(); require(mipChild>=0,"isolated fractional native sampler capability scenario");
    if (!mipChild) {
        consumer->checkFractional=true; bp(0x84,1u<<8);
        process(triangle.data(),triangle.size(),false); std::exit(95);
    }
    int mipStatus=0;
    require(waitpid(mipChild,&mipStatus,0)==mipChild && WIFEXITED(mipStatus) && WEXITSTATUS(mipStatus)==94,
        "fractional clamp rejects explicitly after complete CPU decode instead of silently rounding native LOD");
    liveRam[512+96]=0x77;
    require(process(triangle.data(),triangle.size(),false)==triangle.size() &&
        consumer->retained.back()->textures[0]->rgba[96]==0x77777777u && firstImage->rgba[96]==0x22222222u,
        "same-frame authored RAM changes beyond 64 bytes replace immutable texture without stale memo/hash authority");
    bp(0x80,0); bp(0x84,0); bp(0x88,7|(7u<<10)|(TF_CI4<<20)); bp(0x98,1u<<10);
    std::memset(liveRam+512,0x11,32); std::memset(liveRam+2048,0,32);
    liveRam[2050]=0xF8; bp(0x64,64); bp(0x65,1u<<10);
    require(process(triangle.data(),triangle.size(),false)==triangle.size(),"actual TLUT load feeds streamed indexed RAM image");
    auto red=consumer->retained.back()->textures[0];
    require(red->rgba[0]==0xFF0000FFu,"original RGB565 palette entry decodes red in actual recorder");
    liveRam[2050]=0x07; liveRam[2051]=0xE0; bp(0x65,1u<<10);
    require(process(triangle.data(),triangle.size(),false)==triangle.size() &&
        consumer->retained.back()->textures[0]->rgba[0]==0xFF00FF00u && red->rgba[0]==0xFF0000FFu,
        "same-frame TLUT replacement updates actual indexed image while preserving prior immutable pixels");
    bp(0x28,0);
    std::memcpy(liveRam+3072,triangle.data(),triangle.size());
    std::vector<unsigned char> call{0x40}; word(call,3072); word(call,uint32_t(triangle.size()));
    const unsigned beforeDl=native_draws;
    require(process(call.data(),call.size(),false)==call.size() && process(call.data(),call.size(),false)==call.size() &&
        native_draws==beforeDl+2,"cached draw-only display lists cannot bypass synchronous native draw delivery");
    auto cp=[&](unsigned char reg,uint32_t value) {
        std::vector<unsigned char> bytes{0x08,reg}; word(bytes,value);
        require(process(bytes.data(),bytes.size(),false)==bytes.size(),"real CP register parser configures indexed original arrays");
    };
    cp(0x50,2u<<9); cp(0x70,1); cp(0xA0,32); cp(0xB0,3);
    for (unsigned i=0;i<9;++i) liveRam[32+i]=static_cast<unsigned char>(3+i);
    const unsigned char indexed[]={0x90,0,3,0,1,2}; std::memcpy(liveRam+3072,indexed,sizeof(indexed));
    call={0x40}; word(call,3072); word(call,sizeof(indexed));
    require(process(call.data(),call.size(),false)==call.size(),"original indexed display list cache warms before native RAM overwrite");
    std::vector<unsigned char> ordered=call; ordered.push_back(0x61); word(ordered,0x52000000|(6u<<4));
    ordered.insert(ordered.end(),call.begin(),call.end());
    require(process(ordered.data(),ordered.size(),false)==ordered.size() &&
        static_cast<const PortVitaGxmVertex*>(last_vertex_stream)[0].clip[0]==48,
        "same FIFO stream copy-to-vertex-array invalidates old display-list dependency memo before later draw");
    const pid_t paletteChild=fork(); require(paletteChild>=0,"isolated copy-origin palette completeness scenario");
    if (!paletteChild) {
        bp(0x94,1); bp(0x88,7|(7u<<10)|(TF_CI8<<20)); bp(0x28,64);
        process(indexed,sizeof(indexed),false); std::exit(95);
    }
    int paletteStatus=0;
    require(waitpid(paletteChild,&paletteStatus,0)==paletteChild && WIFEXITED(paletteStatus) && WEXITSTATUS(paletteStatus)==94,
        "copy-origin palette sampling cannot fabricate never-loaded TLUT entries as initialized black");
    // Production failures are terminal. No retry, later draw or PE IRQ is allowed.
    const pid_t child=fork(); require(child>=0,"isolated native-consumer error scenario");
    if (!child) { consumer->reject=true; bp(0x52,6u<<4); std::exit(95); }
    int status=0; require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==94,
        "failed native consumer stops actual parser before it can continue or auto-replay copy");
    spans.reset(); copied.reset(); firstImage.reset(); red.reset(); consumer->lastRam.reset(); consumer->retained.clear(); consumer->packet.reset(); consumer->targetLease.reset();
    require(releaseVitaFrameReplay(&consumer->replay)==0 && releaseVitaTextureCache(&consumer->textures)==0 &&
        releaseVitaMaterialCache(&consumer->materials)==0 && releaseVitaEfbCache(&consumer->efbs)==0 &&
        releaseGxCopyHistory(&consumer->history)==0 && port_vita_gxm_device_release(&consumer->device)==0,
        "all actual live-parser native owners release after callbacks and fences");
    empty_state();
}

// Catches running GX on the guest caller, returning before RAM/fences, losing
// segment dimensions, accepting missing native capabilities and unsafe retry.
static std::thread::id guestCaller, gpuCaller;
static uint32_t workerObservedTexture;
// Driver boundary only: exact bytes from a finished GPU capture, not shader
// emulation. Nonconstant coordinates expose wrong crop/plane/row selection.
static bool peekDepthPlane;
static uint32_t peekDepthBytes=0xE13579;
void peekCapturePixels() {
    require(default_uniforms[0]==7 && default_uniforms[1]==9,
        "real peek capture samples the original top-left EFB coordinate");
    const auto extent=color_planes.at(active_color_data);
    require(default_uniforms[22]==1 && default_uniforms[23]==1 && extent.height==1 && std::this_thread::get_id()!=guestCaller,
        "peek uses a one-pixel leased capture on the native worker only");
    auto* p=static_cast<unsigned char*>(active_color_data);
    const unsigned char color[]={0xD3,0x7E,0x25,0xAB},depth[]={
        static_cast<unsigned char>(peekDepthBytes>>16),static_cast<unsigned char>(peekDepthBytes>>8),
        static_cast<unsigned char>(peekDepthBytes),0xFF};
    std::memcpy(p,peekDepthPlane ? depth : color,4);
}
void workerDrawPixels() {
    const auto self=std::this_thread::get_id();
    require(self!=guestCaller && (gpuCaller==std::thread::id{} || gpuCaller==self),
        "actual draw/capture executes on one host GPU owner, never the guest caller");
    gpuCaller=self;
    if (!texture_inits.empty()) {
        const auto* stored=allocation(texture_inits.back().data);
        if (stored && stored->kind==1)
            workerObservedTexture=static_cast<const uint32_t*>(texture_inits.back().data)[0];
    }
    const auto extent=color_planes.at(active_color_data);
    auto* data=static_cast<unsigned char*>(active_color_data);
    for (unsigned y=0;y<extent.height;++y) for (unsigned x=0;x<extent.stride;++x) {
        auto* p=data+(size_t(y)*extent.stride+x)*4;
        p[0]=16; p[1]=64; p[2]=128; p[3]=48;
    }
    reflect_texture=true;
}
void nativeGameWorker() {
    require(&createVitaGameWorker,"production game GPU worker exists, not only the fixture consumer");
    VitaShaderLibrary library; installCaptureShaders(library);
    auto f=frame(); f->number=1; draw(*f,0x90,3);
    auto prepared=chunks(f); installShaders(library,prepared->chunks()[0].state->uid);
    std::shared_ptr<VitaGameWorker> worker; std::string error;
    require(createVitaGameWorker(std::move(library),worker,error) && worker,
        "actual worker creates native device/replay/cache owners before guest boot");
    unsigned draws=native_draws;
    require(!worker->consume(f,{},error) && native_draws==draws,
        "a host or parked guest without the virtual CPU cannot expose live RAM to the renderer");
    ownsGuestCpu=true;
    require(worker->consume(f,{},error) && native_draws==draws+1 && !active_scene && !pending_gpu,
        "actual native draw completes and fences before the owning guest CPU resumes");
    for (unsigned i=0;i<32;++i) require(worker->consume(f,{},error),
        "back-to-back original requests cannot lose the worker completion acknowledgement");
    guestCaller=std::this_thread::get_id(); reflect_texture=true; draw_result=peekCapturePixels;
    uint32_t value=0xDEADBEEFu;
    struct ColorCase { uint32_t format,alpha,expected; };
    const ColorCase colors[]={{0,2,0xFFD37E25u},{1,2,0xAAD37D24u},{2,2,0xFFD67D21u},
        {1,0,0x00D37D24u},{1,1,0xFFD37D24u},{3,2,0xFFD37E25u}};
    for (const auto& color:colors) {
        require(worker->peek({false,7,9,color.format,color.alpha},value,error) &&
            value==color.expected && !pending_gpu && !active_scene,
            "native worker returns original format-quantized fenced ARGB with command-time alpha read mode");
    }
    peekDepthPlane=true;
    require(worker->peek({true,7,9,0,2},value,error) && value==0xE13579u && !pending_gpu && !active_scene,
        "native worker returns exact original Z24 bytes before guest resumes");
    struct DepthCase { uint32_t z,format,expected; };
    const DepthCase depths[]={{0,2,0},{0xFFFFFF,2,0xFFFF},{0x400000,10,0x2000},
        {0x800000,10,0x4000},{0xE00000,10,0xC000},{0xFFFFFF,10,0xFFFF},
        {0x400000,18,0x1000},{0x800000,18,0x2000},{0xE00000,18,0x6000},
        {0xFFFFFF,18,0xFFFF},{0x400000,26,0x0800},{0x800000,26,0x1000},
        {0xE00000,26,0x3000},{0xFFFFFF,26,0xCFFF}};
    for (const auto& depth:depths) {
        peekDepthBytes=depth.z;
        require(worker->peek({true,7,9,depth.format,3},value,error) && value==depth.expected,
            "original Z16 linear/near/mid/far reads preserve exponent boundaries and ignore color-only alpha policy");
    }
    peekDepthPlane=false; draw_result=nullptr; reflect_texture=false;
    value=0xDEADBEEFu; draws=native_draws;
    require(!worker->peek({false,640,9,0,2},value,error) && value==0xDEADBEEFu && native_draws==draws,
        "out-of-original-EFB peek does not allocate/draw or overwrite caller output");
    require(!worker->consume(f,{},error) && native_draws==draws,
        "original peek failure poisons subsequent guest work without automatic retry");
    ownsGuestCpu=false;
    require(worker->close(error),"actual worker retires every native owner and joins at shutdown");
    bool closedFromNewThread=false;
    std::thread lateClose([&] { std::string detail; closedFromNewThread=worker->close(detail); });
    lateClose.join();
    require(closedFromNewThread,"a reused host thread identity cannot reject an already completed shutdown");
    worker.reset(); prepared.reset(); f.reset(); empty_state();

    installCaptureShaders(library); f=frame(); f->number=2; draw(*f,0x90,3);
    prepared=chunks(f); installShaders(library,prepared->chunks()[0].state->uid);
    require(createVitaGameWorker(std::move(library),worker,error),"peek native-failure session creates original owners");
    ownsGuestCpu=true;
    require(worker->consume(f,{},error),"original native EFB exists before failing its readback");
    reflect_texture=true; draw_result=peekCapturePixels; end_error=-946; value=0xDEADBEEFu;
    require(!worker->peek({false,7,9,1,2},value,error) && value==0xDEADBEEFu && active_scene,
        "native peek End failure preserves original result and retained source/capture ownership");
    draws=native_draws;
    require(!worker->peek({false,7,9,1,2},value,error) && native_draws==draws,
        "failed native peek never recaptures on a subsequent guest request");
    ownsGuestCpu=false;
    require(!worker->close(error) && active_scene,"failed peek cleanup retains its native scene for explicit recovery");
    end_error=0;
    require(worker->close(error) && native_draws==draws,"peek cleanup ends/fences/retires without sampling again");
    worker.reset(); prepared.reset(); f.reset(); draw_result=nullptr; reflect_texture=false; empty_state();

    installCaptureShaders(library); f=frame(); f->number=3;
    f->startBp[0x43]=1; f->startBp[0x53]=21u<<12|22u<<18; f->startBp[0x54]=21;
    draw(*f,0x90,3);
    command(*f,{CMD_EFB_COPY,1,1|(2u<<10),3|(3u<<10),0x00800001u,2,0,0,0,0,6u<<4});
    prepared=chunks(f); installShaders(library,prepared->chunks()[0].state->uid);
    auto post=frame(); post->number=3;
    post->startBp[0x88]=7|(7u<<10)|(TF_I8<<20); post->startBp[0x94]=0x00800001u;
    post->startXfRegs[0x3F]=1; post->startBp[0x28]=64;
    command(*post,{CMD_TEX_EFB,0,1,4,4,0x10000020u}); draw(*post,0x90,3);
    auto postChunks=chunks(post); installShaders(library,postChunks->chunks()[0].state->uid);
    reflect_texture=false; guestCaller=std::this_thread::get_id(); gpuCaller={};
    draw_result=workerDrawPixels; std::memset(liveRam2,0xA5,sizeof(liveRam2));
    require(createVitaGameWorker(std::move(library),worker,error),"actual disjoint-bank worker session initializes");
    ownsGuestCpu=true;
    require(worker->consume(f,{},error) && liveRam2[32]==48 && liveRam2[33]==16 &&
        liveRam2[64]==65 && liveRam2[65]==130 && liveRam2[31]==0xA5 && liveRam2[96]==0xA5,
        "MEM2 original tiled copy is authoritative and visible before guest CPU continuation");
    liveRam2[32]=0xEE;
    workerObservedTexture=0;
    require(worker->consume(post,{},error) && workerObservedTexture==0xEEEEEEEEu,
        "worker selects MEM2 per texture and observes command-time CPU mutation after copy");
    ownsGuestCpu=false; require(worker->close(error),"disjoint RAM session fences and retires native/copy history owners");
    worker.reset(); draw_result=nullptr; reflect_texture=false;
    prepared.reset(); postChunks.reset(); f.reset(); post.reset(); empty_state();

    installCaptureShaders(library); f=frame(); f->number=4;
    f->startBp[0x43]=1; f->startBp[0x53]=21u<<12|22u<<18; f->startBp[0x54]=21;
    draw(*f,0x90,3);
    command(*f,{CMD_EFB_COPY,1,2|(2u<<10),3|(3u<<10),0x00800001u,1,256,0,0,0,(1u<<14)|3});
    prepared=chunks(f); installShaders(library,prepared->chunks()[0].state->uid);
    reflect_texture=false; guestCaller=std::this_thread::get_id(); gpuCaller={}; draw_result=workerDrawPixels;
    std::memset(liveRam2,0x55,sizeof(liveRam2));
    require(createVitaGameWorker(std::move(library),worker,error),"actual original XFB worker session initializes");
    ownsGuestCpu=true;
    const unsigned char originalXfb[]={66,164,66,102,66,164,66,102};
    require(worker->consume(f,{},error) && std::memcmp(liveRam2+32,originalXfb,8)==0 &&
        std::memcmp(liveRam2+128,originalXfb,8)==0 && liveRam2[31]==0x55 && liveRam2[136]==0x55 &&
        !active_scene && !pending_gpu,
        "actual host GPU worker commits the original MEM2 XFB, including the last row, before guest continuation");
    ownsGuestCpu=false; require(worker->close(error),"original XFB worker closes and joins every native owner");
    worker.reset(); prepared.reset(); f.reset(); draw_result=nullptr; reflect_texture=false; empty_state();

    installCaptureShaders(library); f=frame(); f->number=2; draw(*f,0x90,3);
    prepared=chunks(f); installShaders(library,prepared->chunks()[0].state->uid);
    require(createVitaGameWorker(std::move(library),worker,error),"second real session starts after complete native teardown");
    ownsGuestCpu=true; end_error=-933;
    require(!worker->consume(f,{},error) && active_scene,
        "worker returns native End failure while retaining scene/source/cache ownership");
    draws=native_draws;
    require(!worker->consume(f,{},error) && native_draws==draws,
        "terminal game stream cannot retry or issue later commands");
    ownsGuestCpu=false;
    require(!worker->close(error) && active_scene,"failed shutdown retains the live worker for cleanup-only retry");
    end_error=0;
    require(worker->close(error) && native_draws==draws,"shutdown retry only fences/retire work, never redraws it");
    worker.reset(); prepared.reset(); f.reset(); empty_state();

    installCaptureShaders(library); f=frame(); draw(*f,0x90,3);
    prepared=chunks(f); installShaders(library,prepared->chunks()[0].state->uid);
    require(createVitaGameWorker(std::move(library),worker,error),"partial replay-retirement fixture initializes the actual worker");
    ownsGuestCpu=true; require(worker->consume(f,{},error),"actual replay owns a submitted native staging slab");
    ownsGuestCpu=false;
    unmap_failure_address=last_vertex_stream; unmap_address_error=-966;
    require(!worker->close(error) && !active_scene,"replay unmap failure retains a partially closing worker and native slab");
    const unsigned retiredDraws=native_draws;
    ownsGuestCpu=true;
    require(!worker->consume(f,{},error) && error=="native game GPU worker is closing" && native_draws==retiredDraws,
        "partial shutdown closes new guest work even when the original render stream had not failed");
    ownsGuestCpu=false;
    unmap_failure_address=nullptr; unmap_address_error=0;
    require(worker->close(error) && native_draws==retiredDraws,
        "worker retry resumes partial replay retirement instead of rediscarding a closing staging owner");
    worker.reset(); prepared.reset(); f.reset(); empty_state();

    installCaptureShaders(library);
    require(createVitaGameWorker(std::move(library),worker,error),"capability failure session initializes honestly");
    ownsGuestCpu=true; uint32_t peekValue=0xDEADBEEFu;
    require(!worker->peek({false,7,9,0,2},peekValue,error) && peekValue==0xDEADBEEFu,
        "peek without an original rendered EFB is explicit and never fabricates a pixel");
    draws=native_draws;
    require(!worker->present(liveRam,true,error) && native_draws==draws,
        "terminal capability failure cannot turn into synthetic XFB presentation");
    ownsGuestCpu=false; require(worker->close(error),"unsupported capability session closes without orphaning native state");
    worker.reset(); empty_state();

    installCaptureShaders(library);
    require(createVitaGameWorker(std::move(library),worker,error),"error publication fixture initializes actual native ownership");
    ownsGuestCpu=true; std::string allocationError; bool threw=false;
    cpu_fail_any=true; cpu_fail_after=1;
    try { worker->peek({false,7,9,0,2},peekValue,allocationError); } catch (const std::bad_alloc&) { threw=true; }
    cpu_fail_any=false; cpu_fail_after=0; ownsGuestCpu=false;
    require(!threw && !allocationError.empty(),
        "completion error transfer cannot allocate/throw before queue acknowledgement and strand cleanup");
    require(worker->close(error),"terminal capability error still permits explicit complete worker shutdown");
    worker.reset(); empty_state();

    for (unsigned attempt=0;attempt<8;++attempt) {
        installCaptureShaders(library);
        require(createVitaGameWorker(std::move(library),worker,error),"concurrent shutdown fixture initializes one actual native owner");
        std::atomic<bool> start{false}; std::atomic<unsigned> ready{0},failures{0};
        std::vector<std::thread> closers;
        for (unsigned i=0;i<8;++i) closers.emplace_back([&] {
            ++ready; while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            try { std::string detail; if (!worker->close(detail)) ++failures; }
            catch (const std::exception&) { ++failures; }
        });
        while (ready.load()!=8) std::this_thread::yield();
        start.store(true,std::memory_order_release);
        for (auto& thread:closers) thread.join();
        require(!failures,"concurrent host shutdown callers retire/join exactly once without double-join or system_error");
        worker.reset(); empty_state();
    }

    require(!createVitaGameWorker({},worker,error) && worker,
        "missing exact shader library leaves explicit partial initialization ownership");
    require(worker->close(error),"partial worker initialization closes real native resources");
    worker.reset(); empty_state();
}

void nativeGameEntry() {
    require(port_vita_game_entry,"actual native game application entry exists");
    for (bool fail:{true,false}) {
        const pid_t child=fork(); require(child>=0,"entry fixture child starts without live worker threads");
        if (!child) {
            entryInputFailure=fail;
            const int result=port_vita_game_entry();
            require(fail && result!=0 && !entryBootObserved,"native input failure prevents game boot and false success");
            empty_state(); _exit(0);
        }
        int status=0; require(waitpid(child,&status,0)==child && WIFEXITED(status) &&
            WEXITSTATUS(status)==(fail ? 0 : 95),
            "native entry gates prerequisites, installs actual worker, boots original path and polls native input");
    }
}

void nativeViDisplay() {
    auto install=[](VitaShaderLibrary& library) {
        ShaderUid uid{}; uid.numTevStages=1; uid.alphaFunc=63;
        std::string detail;
        const SceGxmProgram vs{8,0},clear{8,2},sample{8,3};
        const std::pair<VitaShaderStage,std::pair<std::string,const SceGxmProgram*>> stages[]={
            {VitaShaderStage::Vertex,{genVitaCpuVertexShader(uid),&vs}},
            {VitaShaderStage::Fragment,{genVitaEfbClearFragmentShader(),&clear}},
            {VitaShaderStage::Fragment,{genVitaEfbSnapshotFragmentShader(false),&sample}}};
        for (const auto& stage:stages) {
            const auto* bytes=reinterpret_cast<const unsigned char*>(stage.second.second);
            require(installVitaShader(library,{stage.first,stage.second.first,{bytes,bytes+8},0},detail),
                "display uses exact production shader sources with separately reflected SDK-driver artifacts");
        }
    };
    VitaShaderLibrary library; install(library);
    std::shared_ptr<VitaGameWorker> worker; std::string error;
    reflect_texture=false;
    require(createVitaGameWorker(std::move(library),worker,error),"VI display owns the original native renderer");
    const unsigned before=native_draws;
    require(!worker->present(nullptr,true,error) && native_draws==before,
        "VI presentation cannot be issued by a host without the guest CPU");
    ownsGuestCpu=true;
    const unsigned drains=std::count(events.begin(),events.end(),"drain");
    require(worker->present(nullptr,true,error) && native_draws==before+1 && !scanout && queue.size()==1 && !active_scene && !pending_gpu &&
        unsigned(std::count(events.begin(),events.end(),"drain"))==drains,
        "VI startup black queues fenced native output without holding the guest CPU until scanout");
    require(default_uniforms[0]==0 && default_uniforms[1]==0 && default_uniforms[2]==0 && default_uniforms[3]==1 &&
        default_uniforms[18]==960 && default_uniforms[19]==544 && clear_depth_write==SCE_GXM_DEPTH_WRITE_DISABLED,
        "native startup black is an opaque full display draw, not a frame counter");
    std::memset(liveRam,0x55,sizeof(liveRam)); std::memset(liveRam2,0xA5,sizeof(liveRam2));
    const unsigned char yuyv[]={16,128,235,128,81,90,145,240,41,240,235,110};
    std::memcpy(liveRam2+96,yuyv,sizeof(yuyv)); std::memcpy(liveRam2+128,yuyv,sizeof(yuyv));
    configureTestVi(8,6,1,2,4,2,liveRam2+32);
    require(worker->present(liveRam2+32,false,error) && !pending_gpu && !active_scene && queue.size()==2,
        "original progressive MEM2 RAM and odd pan become owned native output before guest continuation, not necessarily scanout");
    const unsigned char rgba[]={255,255,255,255,255,0,0,255,255,74,74,255,0,0,255,255};
    const auto record=texture_inits.back();
    require(record.width==4 && record.height==2 && record.levels==1 && allocation(record.data) &&
        std::memcmp(record.data,rgba,16)==0 && std::memcmp(static_cast<const unsigned char*>(record.data)+32,rgba,16)==0,
        "native immutable upload decodes literal original Y0/U/Y1/V with actual VI stride, pan and all visible rows");
    require(std::abs(default_uniforms[0]+float(17*4.0/3.0))<0.001f && default_uniforms[1]==-15.5f &&
        std::abs(default_uniforms[2]-float(686*4.0/3.0))<0.001f && default_uniforms[3]==2.25f &&
        sampled_texture.generic.min_filter==1 && sampled_texture.generic.mag_filter==1 &&
        sampled_texture.generic.uaddr_mode==2 && sampled_texture.generic.vaddr_mode==2 && !sampled_texture.generic.mip_filter,
        "16:9 VI output preserves centered 720x480 origins and crop with explicit linear/clamp/no-mip sampling");
    unsigned images=texture_inits.size();
    for (unsigned repeat=0;repeat<64;++repeat) {
        const unsigned drawn=native_draws,finished=finishes;
        require(worker->present(liveRam2+32,false,error) && texture_inits.size()==images &&
            native_draws==drawn+2 && finishes>finished && scanout && queue.size()==2 && !active_scene && !pending_gpu &&
            unsigned(std::count(events.begin(),events.end(),"drain"))==drains,
            "unchanged XFB still draws and finishes; bounded sync-gated buffer rotation advances scanout without a full queue drain");
    }
    liveRam2[111]^=0xFF;
    require(worker->present(liveRam2+32,false,error) && texture_inits.size()==images,
        "RAM outside the selected YUYV pairs does not trigger redundant image decoding or upload");
    liveRam2[98]=16;
    require(worker->present(liveRam2+32,false,error) && texture_inits.size()==++images &&
        static_cast<const unsigned char*>(texture_inits.back().data)[0]==0,
        "CPU direct-print mutation of the selected XFB replaces pixels, never reuses an EFB packet or old texture");
    liveRam2[129]=90;
    require(worker->present(liveRam2+32,false,error) && texture_inits.size()==++images &&
        static_cast<const unsigned char*>(texture_inits.back().data)[34]==178,
        "a CPU-written shared U in the second row invalidates the image and preserves literal YUYV decoding");
    liveRam2[139]=128;
    require(worker->present(liveRam2+32,false,error) && texture_inits.size()==++images &&
        static_cast<const unsigned char*>(texture_inits.back().data)[44]==29,
        "the final selected pair's shared V remains part of the exact current-RAM comparison");
    configureTestVi(8,6,1,2,4,2,liveRam2+32,0);
    require(worker->present(liveRam2+32,false,error) && texture_inits.size()==images &&
        std::abs(default_uniforms[0]+float(352.0/3.0+17*136.0/135.0))<0.001f &&
        std::abs(default_uniforms[2]-float(686*136.0/135.0))<0.001f &&
        std::abs(default_uniforms[1]+13.6f)<0.001f,
        "an unchanged image can be reused while changed VI aspect still updates the native output geometry");
    std::memcpy(liveRam+64,liveRam2+96,12); std::memcpy(liveRam+96,liveRam2+128,12);
    configureTestVi(8,6,1,2,4,2,liveRam,0);
    require(worker->present(liveRam,false,error) && texture_inits.size()==images,
        "another physical RAM bank with exactly identical selected pairs reuses only the owned immutable image");
    std::memcpy(liveRam+128,liveRam+96,12);
    configureTestVi(24,6,1,1,4,2,liveRam,0);
    require(worker->present(liveRam,false,error) && texture_inits.size()==images,
        "changed framebuffer stride and pan row are compared against actual selected rows, not a cached source pointer");
    liveRam[66]=235;
    require(worker->present(liveRam,false,error) && texture_inits.size()==++images &&
        static_cast<const unsigned char*>(texture_inits.back().data)[0]==255,
        "a CPU change after switching banks and stride invalidates the retained image");
    configureTestVi(24,6,1,1,3,2,liveRam,0);
    require(worker->present(liveRam,false,error) && texture_inits.size()==++images && texture_inits.back().width==3,
        "changed visible image dimensions cannot reuse a texture of the previous extent");
    configureTestVi(24,6,0,1,3,2,liveRam,0);
    require(worker->present(liveRam,false,error) && texture_inits.size()==++images &&
        static_cast<const unsigned char*>(texture_inits.back().data)[0]==0,
        "even and odd crops with identical raw pair spans still decode different selected luma");
    const unsigned blackDraws=native_draws;
    require(worker->present(nullptr,true,error) && native_draws==blackDraws+1 && !pending_gpu && queue.size()==2,
        "black transition retires fenced input pixels while queued native output retains its separate display storage");
    require(worker->present(liveRam,false,error) && texture_inits.size()==++images,
        "leaving black installs the actual current image instead of sampling retired storage");
    configureTestVi(24,6,2,1,3,2,liveRam,0);
    require(worker->present(liveRam,false,error) && texture_inits.size()==++images &&
        std::memcmp(texture_inits.back().data,rgba+4,4)==0,
        "an even crop after preceding YUYV pairs decodes from the actual nonzero selected-pair offset");
    liveRam[66]^=0xFF;
    require(worker->present(liveRam,false,error) && texture_inits.size()==images,
        "luma immediately before a nonzero selected-pair offset is not part of image freshness");
    liveRam[74]=16;
    require(worker->present(liveRam,false,error) && texture_inits.size()==++images &&
        std::memcmp(texture_inits.back().data,rgba+4,4)==0,
        "an invisible companion luma inside the final selected pair conservatively invalidates reuse");
    configureTestVi(24,6,3,1,3,2,liveRam,0);
    require(worker->present(liveRam,false,error) && texture_inits.size()==++images &&
        std::memcmp(texture_inits.back().data,rgba+8,4)==0,
        "an odd nonzero crop keeps the selected pair's shared chroma and chooses its second luma");
    liveRam[67]^=0xFF;
    require(worker->present(liveRam,false,error) && texture_inits.size()==images,
        "chroma immediately before an odd selected pair does not invalidate the retained image");
    liveRam[71]=128;
    const unsigned char oddChanged[]={150,165,74,255};
    require(worker->present(liveRam,false,error) && texture_inits.size()==++images &&
        std::memcmp(texture_inits.back().data,oddChanged,4)==0,
        "actual shared V at a nonzero odd crop invalidates reuse and changes decoded output");
    liveRam[68]=16;
    require(worker->present(liveRam,false,error) && texture_inits.size()==++images &&
        std::memcmp(texture_inits.back().data,oddChanged,4)==0,
        "an invisible first companion luma remains included in the exact selected-pair comparison");
    const unsigned draws=native_draws;
    configureTestVi(8,6,0,0,8,6,liveRam2+4080);
    require(!worker->present(liveRam2+4080,false,error) && native_draws==draws && !active_scene,
        "last XFB row cannot cross a physical RAM bank, and failure precedes native draws");
    require(!worker->present(nullptr,true,error) && native_draws==draws,
        "failed selected XFB poisons later presentation including black, without replay");
    ownsGuestCpu=false;
    require(worker->close(error),"VI black display drains scanout and retires actual native ownership");
    worker.reset(); empty_state();
    for (bool primed:{false,true}) for (int failure:{-961,-962,-963,-964,-965}) {
        library={}; install(library); reflect_texture=false;
        require(createVitaGameWorker(std::move(library),worker,error),"native VI failure session owns actual renderer resources");
        ownsGuestCpu=true; configureTestVi(8,6,1,2,4,2,liveRam2+32);
        if (primed) {
            require(worker->present(liveRam2+32,false,error),
                "VI cache-hit failure fixture first establishes a successful immutable image");
            completeSwap();
        }
        const unsigned expectedImages=texture_inits.size()+(primed ? 0 : 1);
        if (failure==-961) end_error=failure;
        if (failure==-962) queue_error=failure;
        if (failure==-963) display_error=failure;
        if (failure==-964) drain_error=failure;
        if (failure==-965) uniform_error=failure;
        const bool deferred=failure==-963 || failure==-964;
        require(worker->present(liveRam2+32,false,error)==deferred && texture_inits.size()==expectedImages,
            "End/queue/uniform errors fail immediately; display callback and drain errors are asynchronous, with uploads retained");
        if (failure==-963) completeSwap();
        const unsigned failedDraws=native_draws;
        if (failure!=-964) require(!worker->present(liveRam2+32,false,error) && native_draws==failedDraws,
            "a returned or delayed callback failure rejects the next guest draw, with no automatic replay");
        ownsGuestCpu=false;
        if (failure==-961) require(!worker->close(error) && active_scene,
            "display End failure retains its actual scene owner before unrelated staging cleanup");
        if (failure==-964) require(!worker->close(error) && !queue.empty(),
            "queue-drain failure preserves every native owner for cleanup-only retry");
        if (failure==-963 && !primed) require(worker->close(error),
            "persistent display callback error cannot trap child resources or enable new rendering");
        if (failure==-963 && primed) require(!worker->close(error) && scanout,
            "persistent scanout-detachment failure retains the previous displayed storage for cleanup-only retry");
        end_error=queue_error=display_error=drain_error=uniform_error=0;
        require(worker->close(error) && native_draws==failedDraws && texture_inits.size()==expectedImages,
            "cleanup retries retirement only and releases callback-failed or unqueued native display ownership");
        worker.reset(); empty_state();
    }
    for (bool failedVblank:{false,true}) {
        library={}; install(library); reflect_texture=false;
        require(createVitaGameWorker(std::move(library),worker,error),"backpressure callback failure owns a real native renderer");
        ownsGuestCpu=true; configureTestVi(8,6,1,2,4,2,liveRam2+32);
        require(worker->present(liveRam2+32,false,error) && worker->present(liveRam2+32,false,error) && queue.size()==2,
            "two bounded native output swaps remain pending before their asynchronous failure");
        if (failedVblank) vblank_error=-966; else display_error=-967;
        const unsigned uploads=texture_inits.size(),drawsBefore=native_draws;
        require(!worker->present(liveRam2+32,false,error) && queue.size()==1 && texture_inits.size()==uploads && native_draws==drawsBefore,
            "callback set-buffer or vblank failure during sync-reuse rejects work before another native draw");
        require(!worker->present(liveRam2+32,false,error) && native_draws==drawsBefore,
            "a backpressure callback error remains failed, without replay or new guest rendering");
        ownsGuestCpu=false; drain_error=-968;
        const size_t owners=memory.size();
        require(!worker->close(error) && queue.size()==1 && memory.size()==owners && !active_scene && !pending_gpu,
            "cleanup-only failed drain keeps the remaining queued output and all its owners");
        drain_error=0;
        require(worker->close(error) && texture_inits.size()==uploads && native_draws==drawsBefore,
            "cleanup retry retires persistent callback failure without rendering, reupload or premature free");
        vblank_error=display_error=0;
        worker.reset(); empty_state();
    }
    for (unsigned unsupported=0;unsupported<4;++unsupported) {
        library={}; install(library); reflect_texture=false;
        require(createVitaGameWorker(std::move(library),worker,error),"unsupported VI scanout has a real native owner");
        ownsGuestCpu=true;
        configureTestVi(8,6,1,2,4,2,liveRam2+32,unsupported==0 ? 2 : 1,
            unsupported==1 ? 20 : 22,unsupported==2 ? 1 : 0,unsupported==3 ? 1 : 0);
        const unsigned prior=native_draws,allocations=memory.size();
        require(!worker->present(liveRam2+32,false,error) && native_draws==prior && memory.size()==allocations,
            "unsupported aspect/interlaced/double-field/field-rendered VI fails closed before any native allocation or draw");
        ownsGuestCpu=false; require(worker->close(error),"unsupported VI strategy retires without losing game-mode requirements");
        worker.reset(); empty_state();
    }
    library={}; install(library);
    auto f=frame(); f->number=8; f->startBp[0x43]=1;
    f->startBp[0x53]=21u<<12|22u<<18; f->startBp[0x54]=21;
    draw(*f,0x90,3);
    command(*f,{CMD_EFB_COPY,1,2|(2u<<10),3|(3u<<10),0x00800001u,1,256,0,0,0,(1u<<14)|3});
    auto prepared=chunks(f); installShaders(library,prepared->chunks()[0].state->uid);
    reflect_texture=false; guestCaller=std::this_thread::get_id(); gpuCaller={}; draw_result=workerDrawPixels;
    require(createVitaGameWorker(std::move(library),worker,error),"complete EFB-to-VI transport owns the real native worker");
    ownsGuestCpu=true; require(worker->consume(f,{},error),"actual GPU worker captures and writes original MEM2 progressive YUYV");
    draw_result=nullptr;
    require(attachRecorderConsumer(worker,error),"real recorder attaches the actual worker before original VI handoff");
    setTestViPresenter([](void* xfb,int black) { port_gx_recorder_present_xfb(xfb,black); });
    configureTestVi(4,4,0,0,4,4,liveRam2+32);
    const unsigned char decoded[]={17,65,131,255};
    require(std::memcmp(texture_inits.back().data,decoded,4)==0 && queue.size()==1 && !active_scene && !pending_gpu,
        "actual original VI callback passes selected YUYV through recorder and worker to fenced queued native output");
    const unsigned char printed[]={16,128,16,128}; std::memcpy(liveRam2+32,printed,4);
    const unsigned prior=native_draws;
    retraceTestVi();
    require(native_draws==prior+2 && static_cast<const unsigned char*>(texture_inits.back().data)[0]==0,
        "hardware-style retrace observes CPU direct-print changes to current XFB even without a new VIFlush");
    setTestViPresenter(nullptr); ownsGuestCpu=false;
    require(worker->close(error),"complete original EFB/VI/recorder/display path drains and retires without replay");
    worker.reset(); prepared.reset(); f.reset(); reflect_texture=false; empty_state();
    std::fprintf(stderr,"VI/native display: %u real integration checks; selected RAM, pan, aspect, black, EFB/recorder and retirement passed\n",checks);
}

void nativeBringup() {
    require(createVitaBringupRenderer && drawVitaBringup && releaseVitaBringupRenderer,
        "native GXM/input diagnostic renderer exists independently of unfinished game boot");
    VitaBringupRenderer* renderer=nullptr; std::string error; PortPadState pad{};
    reflect_texture=false; VitaShaderLibrary library; installCaptureShaders(library);
    require(createVitaBringupRenderer(library,&renderer,error)==0 && renderer && queue.empty(),
        "native bringup owns real device/program/staging without presenting an invented frame");
    pad.connected=pad.pointerValid=1; pad.pointerX=0.5f; pad.pointerY=-0.5f;
    unsigned before=native_draws; reflect_texture=false;
    require(drawVitaBringup(renderer,pad,error)==0 && native_draws==before+5 && queue.size()==1 && !active_scene,
        "native bringup submits background, RGB bands and input pointer through real display staging");
    pad.pointerX=std::nanf(""); before=native_draws;
    require(drawVitaBringup(renderer,pad,error)<0 && native_draws==before,
        "invalid controller coordinates never upload or submit nonfinite geometry"); pad.pointerX=0;
    pad.pointerX=1e30f;
    require(drawVitaBringup(renderer,pad,error)<0 && native_draws==before,
        "out-of-range controller coordinates reject before floating-to-integer overflow"); pad.pointerX=0;
    uniform_error=-927;
    require(drawVitaBringup(renderer,pad,error)==-927 && active_scene,
        "native bringup retains a failed submitted scene for explicit close, not automatic replay"); uniform_error=0;
    before=native_draws;
    require(releaseVitaBringupRenderer(&renderer)==0 && !renderer && native_draws==before,
        "native bringup teardown closes and fences failed work without drawing again");
    empty_state();
}
void gxXfbCopies() {
    std::string error; error.reserve(256); GxCopyHistory* history=nullptr;
    require(createGxCopyHistory(&history)==0,"original XFB uses ordered copy history, not a display surrogate");
    SpanCopy copy{}; copy.efbWidth=8; copy.efbHeight=6; copy.words[0]=1;
    copy.words[1]=2|(2u<<10); copy.words[2]=3|(1u<<10); copy.words[3]=1; copy.words[4]=1;
    copy.words[5]=256; copy.words[9]=(1u<<14)|3;
    copy.filter[0]=21u<<12|22u<<18; copy.filter[1]=21;
    unsigned char plane[6*40]; std::memset(plane,0xA5,sizeof(plane));
    const unsigned char colors[4][4]={{255,0,0,7},{0,255,0,8},{0,0,255,9},{255,255,255,10}};
    for (unsigned y=0;y<6;++y) for (unsigned x=0;x<8;++x)
        std::memcpy(plane+y*40+x*4,colors[(x+2)%4],4);
    GxCopyPlane view{plane,0,0,8,6,40,sizeof(plane),false}; GxCopyPixels pixels;
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().width==4 &&
        pixels.texels().height==2 && pixels.texels().data[0]==255 && pixels.texels().data[3]==255,
        "original progressive XFB produces only its cropped filtered RGB rows, without texture-tile padding");
    unsigned char ram[128]; std::memset(ram,0x55,sizeof(ram));
    std::shared_ptr<const GxCopyBytes> packet;
    const unsigned char expected[]={82,81,144,188,41,165,235,95};
    require(storeGxCopyBytes(history,1,copy,pixels.texels(),{ram,0,sizeof(ram)},packet,error)==0 && packet &&
        packet->format()==15 && packet->width()==4 && packet->height()==2 && packet->rows()==2 &&
        packet->rowBytes()==8 && packet->ramStride()==32 && packet->ramExtent()==40 &&
        std::memcmp(packet->data(),expected,8)==0 && std::memcmp(ram+32,expected,8)==0 &&
        std::memcmp(ram+64,expected,8)==0,
        "XFB commits original linear Y0/U/Y1/V with signed BT601 rounding and chroma neighbors, not RGBA tiles");
    for (unsigned i=0;i<sizeof(ram);++i) if (!(i>=32 && i<40) && !(i>=64 && i<72))
        require(ram[i]==0x55,"XFB RAM stride gaps and unrelated bytes remain unchanged");
    auto original=packet; packet.reset(); pixels={}; copy.words[0]=2;
    copy.pixelFormat=1; copy.filter[0]=32|(32u<<12); copy.filter[1]=0;
    for (unsigned y=0;y<6;++y) for (unsigned x=0;x<8;++x) {
        auto* p=plane+y*40+x*4; p[0]=y*32; p[1]=64; p[2]=128; p[3]=17;
    }
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().data[0]==65 &&
        pixels.texels().data[16]==81,"XFB original 32/32 filter and crop clamps preserve RGBA6 point precision");
    copy.words[4]=0;
    require(storeGxCopyBytes(history,2,copy,pixels.texels(),{ram,0,sizeof(ram)},packet,error)==0 &&
        packet->ramExtent()==8 && std::memcmp(ram+32,packet->data()+8,8)==0 &&
        std::memcmp(original->data(),expected,8)==0,
        "zero XFB stride writes later rows last while immutable earlier copy bytes survive");
    packet.reset(); std::memset(ram,0x55,sizeof(ram));
    require(storeGxCopyBytes(history,2,copy,pixels.texels(),{ram,0,sizeof(ram)},packet,error)<0 &&
        !packet && ram[32]==0x55,"duplicate XFB copy cannot repeat original RAM writes");
    copy.words[0]=3; copy.words[4]=1;
    require(storeGxCopyBytes(history,3,copy,pixels.texels(),{ram,0,71},packet,error)<0 &&
        !packet && ram[32]==0x55,"XFB validates the last linear row before touching the first row");
    for (unsigned failure=1;failure<=2;++failure) {
        cpu_fail_any=true; cpu_fail_after=failure;
        const int result=storeGxCopyBytes(history,3,copy,pixels.texels(),{ram,0,sizeof(ram)},packet,error);
        cpu_fail_any=false; cpu_fail_after=0;
        require(result<0 && !packet && ram[32]==0x55,"XFB checked allocation failure leaves RAM/history uncommitted");
    }
    pixels={}; copy.words[9]|=1u<<10; copy.words[5]=128;
    require(produceGxCopyPixels(copy,view,pixels,error)<0 && !pixels.texels().data,
        "unimplemented scaled XFB rejects instead of silently dropping lines");
    copy.words[5]=256;
    require(produceGxCopyPixels(copy,view,pixels,error)==0,"explicit unity XFB scale preserves original row count");
    pixels={}; copy.words[9]|=1u<<12;
    require(produceGxCopyPixels(copy,view,pixels,error)<0 && !pixels.texels().data,
        "original field XFB strategy cannot silently become a progressive frame");
    copy.words[9]=(1u<<14)|3; copy.words[2]=2|(1u<<10);
    require(produceGxCopyPixels(copy,view,pixels,error)<0 && !pixels.texels().data,
        "unproven odd XFB chroma pairing rejects without invented final pixels");
    copy.words[2]=3|(1u<<10); view.depth=true;
    require(produceGxCopyPixels(copy,view,pixels,error)<0 && !pixels.texels().data,
        "XFB always consumes the original color plane, never a Z24 snapshot");
    view.depth=false; copy.words[9]=1u<<14;
    view.data=plane+2*40; view.top=2; view.height=2; view.bytes=2*40;
    require(produceGxCopyPixels(copy,view,pixels,error)<0 && !pixels.texels().data,
        "unclamped XFB filtering needs its true EFB neighbor rows, not crop-edge repetition");
    original.reset();
    // The recorder remembers its latest texture-copy ID separately from XFB
    // copies. Later texture reads still need that origin after an XFB overwrite,
    // but must decode actual current RAM, not the old packet's texture bytes.
    copy.words[0]=4; copy.words[1]=0; copy.words[2]=3|(3u<<10); copy.words[9]=6u<<4; copy.pixelFormat=0;
    unsigned char tile[4*4*4]{};
    require(storeGxCopyBytes(history,4,copy,{tile,4,4,16,sizeof(tile)},{ram,0,sizeof(ram)},packet,error)==0,
        "mixed XFB/texture history records a real original tiled-copy origin");
    packet.reset(); copy.words[0]=5; copy.words[2]=3|(1u<<10); copy.words[9]=(1u<<14)|3;
    view={plane,0,0,8,6,40,sizeof(plane),false};
    require(produceGxCopyPixels(copy,view,pixels,error)==0 &&
        storeGxCopyBytes(history,5,copy,pixels.texels(),{ram,0,sizeof(ram)},packet,error)==0,
        "original XFB may overwrite RAM previously copied to a texture at the same base");
    const uint64_t lastCopy=packet->sequence();
    packet.reset(); pixels={};
    require(collectGxCopyBytes(history,5)==0 && lookupGxCopyBytes(history,6,4,32,packet,error)==0 && !packet->isXfb(),
        "XFB retirement preserves the recorder's separate last texture-copy origin for current-RAM reinterpretation");
    SpanRegisters registers{}; registers.bp[0x88]=7|(3u<<10)|(TF_I8<<20); registers.bp[0x94]=1;
    GxRamTexture decoded;
    require(decodeGxRamTexture(registers,0,{ram,0,sizeof(ram),nullptr,0,0,6,lastCopy},packet,decoded,error)==0 &&
        decoded.rgba()[0]==0x3D3D3D3Du,
        "preserved texture origin decodes actual XFB-overwritten RAM, not old packet pixels or XFB-tag layout");
    decoded={};
    packet.reset(); require(releaseGxCopyHistory(&history)==0,"XFB copy history retires all original leases");
}
// Real producer/packer/decoder, not a second conversion implementation in a
// mock. Literal fixtures catch ordering, precision, neighbor and conversion bugs.
void gxIdentityCopyRows() {
    unsigned char storage[117]; std::memset(storage,0xA5,sizeof(storage));
    const unsigned char input[8][4]={
        {0,3,63,64},{65,127,128,191},{192,252,254,255},{171,205,239,151},
        {171,205,239,151},{192,252,254,255},{65,127,128,191},{0,3,63,64}};
    const unsigned char rgba6[8][4]={
        {0,0,60,65},{65,125,130,190},{195,255,255,255},{170,207,239,150},
        {170,207,239,150},{195,255,255,255},{65,125,130,190},{0,0,60,65}};
    for (unsigned y=0;y<2;++y) for (unsigned x=0;x<4;++x)
        std::memcpy(storage+1+(y+1)*29+(x+1)*4,input[y*4+x],4);
    unsigned char original[sizeof(storage)]; std::memcpy(original,storage,sizeof(storage));
    const GxCopyPlane view{storage+1,1,2,6,4,29,116,false};
    SpanCopy copy{}; copy.efbWidth=8; copy.efbHeight=8; copy.pixelFormat=1;
    copy.words[1]=2|(3u<<10); copy.words[2]=3|(1u<<10);
    copy.words[4]=1; copy.words[5]=256; copy.words[9]=(1u<<14)|3;
    copy.filter[0]=21u<<12|22u<<18; copy.filter[1]=21;
    std::string error; GxCopyPixels pixels;
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().bytes==32 &&
        std::memcmp(pixels.texels().data,rgba6,32)==0,
        "identity RGBA6 copy preserves all channels across cropped nonuniform rows and unaligned odd source pitch");
    require(std::memcmp(storage,original,sizeof(storage))==0,"identity processing preserves every borrowed pixel and guard byte");
    std::memset(storage,0,sizeof(storage));
    require(std::memcmp(pixels.texels().data,rgba6,32)==0,
        "processed identity output owns its bytes independently of subsequent source mutation");
    std::memcpy(storage,original,sizeof(storage));
    pixels={}; copy.pixelFormat=0;
    require(produceGxCopyPixels(copy,view,pixels,error)==0,"cropped RGB8 identity copy has complete source rows");
    for (unsigned i=0;i<8;++i)
        require(std::memcmp(pixels.texels().data+i*4,input[i],3)==0 && pixels.texels().data[i*4+3]==255,
            "RGB8 identity copy preserves each original RGB triplet but never imports stored alpha or pitch padding");
    pixels={}; copy.pixelFormat=1; copy.filter[0]=21u<<12|21u<<18;
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().data[2]==59 &&
        pixels.texels().data[3]==65,
        "a 63-weight filter must not use the unity shortcut or alter unfiltered alpha");
    pixels={}; copy.words[2]=2|(1u<<10); copy.words[9]=(6u<<4)|3;
    copy.filter[0]=21u<<12|22u<<18;
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().width==4 && pixels.texels().height==4,
        "unity texture copy retains original tile padding rather than cropping to logical width/height");
    for (unsigned y=0;y<4;++y)
        require(std::memcmp(pixels.texels().data+y*16,rgba6+(y ? 4 : 0),16)==0,
            "padded texture columns remain nonuniform source pixels and bottom-clamped rows repeat the actual final row");
    pixels={}; copy.words[1]=2|(2u<<10); copy.words[9]&=~2u;
    require(produceGxCopyPixels(copy,view,pixels,error)==0,"unclamped texture padding reads complete original footprint");
    for (unsigned x=0;x<16;++x)
        require(pixels.texels().data[x]==0xA6 && pixels.texels().data[48+x]==0xA6,
            "unclamped padded rows read real extra source rows instead of repeating the logical edge");
    require(std::memcmp(pixels.texels().data+16,rgba6,32)==0,"unclamped padded rows preserve both distinct interior rows");
    pixels={}; auto missing=view; missing.height=3; missing.bytes=87;
    require(produceGxCopyPixels(copy,missing,pixels,error)<0 && !pixels.texels().data,
        "unity shortcut rejects a missing padded row before reading beyond the validated source footprint");
    require(std::memcmp(storage,original,sizeof(storage))==0,
        "all identity/filter/padding processing preserves the entire borrowed source");
}
void gxCopyPixels() {
    std::string error; error.reserve(256); SpanCopy copy{};
    copy.efbWidth=16; copy.efbHeight=16; copy.pixelFormat=1;
    copy.words[0]=1; copy.words[1]=2|(2u<<10); copy.words[2]=3|(3u<<10);
    copy.words[3]=1; copy.words[4]=2; copy.words[9]=6u<<4;
    copy.filter[0]=21u<<12|22u<<18; copy.filter[1]=21;
    unsigned char plane[16*16*4];
    for (unsigned y=0;y<16;++y) for (unsigned x=0;x<16;++x) {
        auto* p=plane+(y*16+x)*4; p[0]=0xAB; p[1]=0xCD; p[2]=0xEF; p[3]=0x97;
    }
    GxCopyPlane view{plane,0,0,16,16,64,sizeof(plane),false}; GxCopyPixels pixels;
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().width==4 &&
        pixels.texels().height==4 && pixels.texels().data[0]==0xAA && pixels.texels().data[1]==0xCF &&
        pixels.texels().data[2]==0xEF && pixels.texels().data[3]==0x96,
        "RGBA6 producer quantizes and expands every channel before identity copy filtering");
    pixels={}; copy.pixelFormat=0;
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().data[0]==0xAB && pixels.texels().data[3]==255,
        "RGB8 EFB copies do not invent stored alpha");
    pixels={}; copy.pixelFormat=2;
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().data[0]==0xAD &&
        pixels.texels().data[1]==0xCF && pixels.texels().data[2]==0xEF && pixels.texels().data[3]==255,
        "RGB565 producer uses original five/six/five expanded point precision");
    pixels={}; copy.pixelFormat=1;
    for (unsigned y=0;y<16;++y) for (unsigned x=0;x<16;++x) {
        auto* p=plane+(y*16+x)*4; p[0]=unsigned(y*16); p[1]=unsigned(x*16); p[2]=64; p[3]=unsigned(32+y*4);
    }
    copy.filter[0]=8|(8u<<6)|(16u<<12)|(16u<<18); copy.filter[1]=8u<<6|8u<<12;
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().data[0]==32 &&
        pixels.texels().data[1]==32 && pixels.texels().data[3]==40,"seven coefficients group three source rows and do not filter alpha");
    pixels={}; copy.words[9]|=1;
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().data[0]==36,
        "top copy clamp uses copied rectangle, not the capture's top edge");
    pixels={}; copy.words[9]|=2;
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().data[12*4]==77,
        "bottom copy clamp uses the original rectangle before filtering");
    pixels={}; copy.words[9]=6u<<4; copy.filter[0]=63|(63u<<6)|(63u<<12)|(63u<<18);
    copy.filter[1]=63|(63u<<6)|(63u<<12);
    for (unsigned i=0;i<256;++i) { plane[i*4]=255; plane[i*4+1]=128; plane[i*4+2]=0; plane[i*4+3]=48; }
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().data[0]==221 &&
        pixels.texels().data[1]==255 && pixels.texels().data[3]==48,
        "filter overflow wraps nine bits before eight-bit saturation, alpha unaffected");
    pixels={}; copy.filter[0]=21u<<12|22u<<18; copy.filter[1]=21;
    copy.words[9]=(6u<<4)|(1u<<9); copy.words[2]=2|(2u<<10);
    for (unsigned i=0;i<256;++i) { plane[i*4]=0; plane[i*4+1]=0; plane[i*4+2]=0; plane[i*4+3]=0; }
    plane[(2*16+2)*4]=3; plane[(2*16+3)*4]=3; plane[(3*16+2)*4]=4; plane[(3*16+3)*4]=4;
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().width==4 && pixels.texels().height==4 &&
        pixels.texels().data[0]==4,"half-copy averages four point samples before RGBA6 round-even quantization");
    pixels={}; copy.pixelFormat=0;
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().data[0]==3 && pixels.texels().data[3]==255,
        "half RGB8 average truncates original byte result without RGBA6 quantization");
    pixels={}; unsigned char tight[8*12*4];
    for (unsigned y=0;y<12;++y) for (unsigned x=0;x<8;++x) {
        auto* p=tight+(y*8+x)*4; p[0]=y*10; p[1]=x*10; p[2]=64; p[3]=48;
    }
    GxCopyPlane tightView{tight,2,0,8,12,32,sizeof(tight),false};
    copy.filter[0]=8|(20u<<12)|(20u<<18); copy.filter[1]=16u<<6;
    require(produceGxCopyPixels(copy,tightView,pixels,error)==0 && pixels.texels().data[0]==27 &&
        pixels.texels().data[12*4]==87,"half copy filters nonuniform row pairs at scaled offsets through odd-size tile padding");
    pixels={}; tightView.height=11; tightView.bytes=8*11*4;
    require(produceGxCopyPixels(copy,tightView,pixels,error)<0 && !pixels.texels().data,
        "half-copy filtered padded output rejects exactly one missing required halo row");
    tightView.height=12; tightView.bytes=sizeof(tight); copy.words[9]|=1;
    require(produceGxCopyPixels(copy,tightView,pixels,error)==0 && pixels.texels().data[0]==29,
        "half-copy top clamp applies to each point before asymmetric three-row filtering");
    pixels={}; copy.words[9]|=2; tightView.data=tight+2*32; tightView.top=2; tightView.height=3; tightView.bytes=3*32;
    require(produceGxCopyPixels(copy,tightView,pixels,error)==0 && pixels.texels().data[0]==28 &&
        pixels.texels().data[4*4]==38 && pixels.texels().data[12*4]==40,
        "both half-copy clamps retain odd-height boundary pixels without requiring nonexistent padded halo rows");
    pixels={}; copy.filter[0]=21u<<12|22u<<18; copy.filter[1]=21;
    pixels={}; copy.words[9]=6u<<4; copy.words[2]=3|(3u<<10);
    for (unsigned i=0;i<256;++i) { plane[i*4]=16; plane[i*4+1]=64; plane[i*4+2]=128; plane[i*4+3]=48; }
    for (unsigned gamma=1;gamma<=3;++gamma) {
        copy.words[9]=(6u<<4)|(gamma<<7);
        require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().data[0]==(gamma==1 ? 50 : 72) &&
            pixels.texels().data[1]==(gamma==1 ? 113 : 136) && pixels.texels().data[2]==(gamma==1 ? 170 : 186) &&
            pixels.texels().data[3]==255,"gamma1.7/2.2 and selector3 operate after filter on RGB, not alpha"); pixels={};
    }
    copy.pixelFormat=1; copy.words[9]=(6u<<4)|(3u<<15);
    for (unsigned i=0;i<256;++i) { plane[i*4]=255; plane[i*4+1]=0; plane[i*4+2]=0; plane[i*4+3]=48; }
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().data[0]==82 && pixels.texels().data[1]==90 &&
        pixels.texels().data[2]==240 && pixels.texels().data[3]==48,
        "original integer YUV/intensity conversion runs on filtered RGB and preserves alpha");
    pixels={}; copy.words[9]=(6u<<4)|(1u<<15);
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().data[0]==255 &&
        pixels.texels().data[1]==0 && pixels.texels().data[2]==0 && pixels.texels().data[3]==48,
        "intensity flag without automatic conversion preserves RGB instead of performing YUV");
    pixels={}; copy.words[9]=(6u<<4)|(3u<<15);
    require(produceGxCopyPixels(copy,view,pixels,error)==0,"automatic conversion restores processed YUV producer input");
    GxCopyHistory* history=nullptr; require(createGxCopyHistory(&history)==0,"producer to byte history owner");
    unsigned char ram[256]{}; std::shared_ptr<const GxCopyBytes> packet,old;
    require(storeGxCopyBytes(history,1,copy,pixels.texels(),{ram,0,sizeof(ram)},packet,error)==0 && ram[32]==48 && ram[33]==82 &&
        ram[64]==90 && ram[65]==240,"actual producer feeds original split-plane packed RAM bytes");
    old=packet; packet.reset(); pixels={}; copy.words[9]=(0u<<4)|(2u<<15);
    plane[(2*16+2)*4]=0x10; plane[(2*16+3)*4]=0xE0;
    require(produceGxCopyPixels(copy,view,pixels,error)==0 &&
        storeGxCopyBytes(history,2,copy,pixels.texels(),{ram,0,sizeof(ram)},packet,error)==0 && ram[32]==0x1E,
        "producer and R4 packer preserve distinct adjacent high/low nibble order");
    packet.reset(); require(lookupGxCopyBytes(history,1,1,32,packet,error)==0 && packet==old,
        "same-ID/address lookup excludes newer frame origin"); packet.reset();
    require(lookupGxCopyBytes(history,2,1,32,packet,error)==0 && packet->format()==0,
        "same-ID/address lookup chooses newest eligible origin"); packet.reset(); old.reset();
    require(releaseGxCopyHistory(&history)==0,"complete producer/packer/history retirement");
    pixels={}; copy.pixelFormat=3; copy.words[9]=(6u<<4)|(1u<<9); view.depth=true;
    for (unsigned i=0;i<256;++i) { plane[i*4]=0; plane[i*4+1]=0; plane[i*4+2]=0; }
    plane[(2*16+2)*4+2]=255; plane[(2*16+3)*4+1]=1;
    plane[(3*16+2)*4+1]=1; plane[(3*16+3)*4+1]=1; plane[(3*16+3)*4+2]=1;
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().data[0]==0 && pixels.texels().data[1]==1 &&
        pixels.texels().data[2]==0 && pixels.texels().data[3]==255,
        "half depth samples average the full Z24 value before splitting bytes, retaining carry");
    pixels={}; view.depth=false;
    require(produceGxCopyPixels(copy,view,pixels,error)<0 && !pixels.texels().data,
        "depth-copy commands cannot reinterpret a color capture as Z24");
    copy.pixelFormat=0; copy.words[9]=6u<<4; view={plane+136,2,2,4,4,64,sizeof(plane)-136,false};
    copy.filter[0]=8|(8u<<6)|(16u<<12)|(16u<<18); copy.filter[1]=8u<<6|8u<<12;
    require(produceGxCopyPixels(copy,view,pixels,error)<0 && !pixels.texels().data,
        "missing unclamped filter neighbors reject instead of fabricating edge samples");
    copy.pixelFormat=1; copy.words[9]=7u<<4; view={plane+136,2,2,8,4,64,sizeof(plane)-136,false};
    require(produceGxCopyPixels(copy,view,pixels,error)==0 && pixels.texels().data[3]==48,
        "alpha-only copy needs current alpha but no RGB-only neighboring filter rows");
    pixels={}; copy.pixelFormat=0; copy.words[9]=6u<<4; view={plane+136,2,2,4,4,64,sizeof(plane)-136,false};
    copy.words[9]|=3;
    require(produceGxCopyPixels(copy,view,pixels,error)==0,"explicit top/bottom clamp needs no outside rows");
    const auto* preserved=pixels.texels().data;
    require(produceGxCopyPixels(copy,view,pixels,error)<0 && pixels.texels().data==preserved,
        "nonempty pixel output cannot lose an existing processed allocation");
    pixels={}; view={plane,0,0,16,16,64,sizeof(plane),false};
    cpu_fail_any=true; cpu_fail_after=1; int result=produceGxCopyPixels(copy,view,pixels,error);
    cpu_fail_any=false; cpu_fail_after=0;
    require(result<0 && !pixels.texels().data,"producer CPU OOM has no partial output");
    copy.words[9]|=2u<<12;
    require(produceGxCopyPixels(copy,view,pixels,error)<0 && !pixels.texels().data,
        "field sampling cannot silently fall back to progressive texture copy");
}
}
static void xfbSnapshot() {
    require(gpu::writeVitaXfbSnapshot,"opt-in native XFB snapshot writer exists");
    FILE* file=std::tmpfile(); require(file,"snapshot owns a real temporary stream");
    const unsigned char rgba[]={255,0,1,255,2,254,3,128,4,5,253,0,6,7,8,255};
    require(gpu::writeVitaXfbSnapshot(file,rgba,2,2,sizeof(rgba)),"snapshot accepts exact nonuniform decoded RGBA bytes");
    const char header[]="P7\nWIDTH 2\nHEIGHT 2\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
    require(std::ftell(file)==long(sizeof(header)-1+sizeof(rgba)),"snapshot length excludes padding and trailing bytes");
    std::rewind(file); unsigned char actual[sizeof(header)-1+sizeof(rgba)]{};
    require(std::fread(actual,1,sizeof(actual),file)==sizeof(actual) &&
        !std::memcmp(actual,header,sizeof(header)-1) && !std::memcmp(actual+sizeof(header)-1,rgba,sizeof(rgba)),
        "PAM preserves top-to-bottom RGB channel order and all original alpha bytes");
    std::fclose(file);
    file=std::tmpfile(); require(file,"invalid snapshot fixture has a real stream");
    require(!gpu::writeVitaXfbSnapshot(file,rgba,0,2,sizeof(rgba)) &&
        !gpu::writeVitaXfbSnapshot(file,rgba,2,1025,sizeof(rgba)) &&
        !gpu::writeVitaXfbSnapshot(file,rgba,UINT32_MAX,UINT32_MAX,sizeof(rgba)) &&
        !gpu::writeVitaXfbSnapshot(file,rgba,2,2,sizeof(rgba)-1) &&
        !gpu::writeVitaXfbSnapshot(file,nullptr,2,2,sizeof(rgba)) && std::ftell(file)==0,
        "malformed or truncated snapshots reject before reading pixels or writing a header");
    require(!gpu::writeVitaXfbSnapshot(nullptr,rgba,2,2,sizeof(rgba)),"missing diagnostic stream rejects without dereference");
    std::fclose(file);
    file=std::fopen("/dev/null","rb"); require(file,"write-error fixture uses a real read-only stream");
    require(!gpu::writeVitaXfbSnapshot(file,rgba,2,2,sizeof(rgba)),"actual stream write failure never reports captured pixels");
    std::fclose(file);
    file=std::fopen("/dev/full","wb"); require(file,"delayed-write fixture uses real failing device storage");
    require(!gpu::writeVitaXfbSnapshot(file,rgba,2,2,sizeof(rgba)),"buffered payload flush failure never reports a successful snapshot");
    std::fclose(file);

    char directory[]="/tmp/galaxy-xfb-XXXXXX";
    require(::mkdtemp(directory),"capture control owns an isolated output directory");
    auto configure=[](gpu::VitaXfbCapture& capture,const char* marker) {
        if (!marker) { capture.configure(nullptr); return; }
        FILE* file=std::tmpfile(); require(file,"capture marker owns a real stream");
        const size_t bytes=std::strlen(marker);
        require(std::fwrite(marker,1,bytes,file)==bytes,"test marker saved without simulated parsing");
        std::rewind(file); capture.configure(file); std::fclose(file);
    };
    for (const char* marker:{static_cast<const char*>(nullptr),"","0032","0032\r\n","0032\nextra"," 032\n","../2\n","a032\n","0032\n\n"}) {
        gpu::VitaXfbCapture capture; configure(capture,marker);
        for (unsigned i=0;i<181;++i) require(capture.capture(directory,rgba,2,2,sizeof(rgba))==0,
            "missing and malformed markers leave all capture slots disabled");
        require(!capture.tag()[0] && !capture.present(),"invalid markers do not retain a tag or advance capture counting");
    }
    gpu::VitaXfbCapture capture; configure(capture,"0032\n");
    require(!std::strcmp(capture.tag(),"0032"),"exact four digits plus newline enables the requested tag");
    for (unsigned i=1;i<=200;++i) require(capture.capture(directory,rgba,2,2,sizeof(rgba))==
        (i==1 || i==60 || i==180 ? 1 : 0),"exactly successful selected-XFB slots 1/60/180 save, without later retries or overflow");
    require(capture.present()==180,"finished diagnostic counter saturates");
    for (unsigned i:{1u,60u,180u}) {
        char path[128]; std::snprintf(path,sizeof(path),"%s/xfb-0032-%03u.pam",directory,i);
        struct stat info{}; require(::stat(path,&info)==0 && info.st_size==long(sizeof(actual)) &&
            (info.st_mode&0777)==0600,"each bounded capture owns a complete private output");
        file=std::fopen(path,"rb"); require(file,"saved diagnostic is independently readable");
        require(std::fread(actual,1,sizeof(actual),file)==sizeof(actual) &&
            !std::memcmp(actual,header,sizeof(header)-1) && !std::memcmp(actual+sizeof(header)-1,rgba,sizeof(rgba)),
            "real capture-control path preserves exact original decoded bytes");
        std::fclose(file);
        if (i==1) {
            configure(capture,"0032\n");
            require(capture.capture(directory,rgba,2,2,sizeof(rgba))<0 &&
                capture.capture(directory,rgba,2,2,sizeof(rgba))==0,"existing output rejects exclusively without retry");
            require(::stat(path,&info)==0 && info.st_size==long(sizeof(actual)),"exclusive rejection cannot remove or truncate previous evidence");
        }
        require(::unlink(path)==0,"owned diagnostic output removed after validation");
    }
    char path[128]; std::snprintf(path,sizeof(path),"%s/xfb-0033-001.pam",directory);
    auto openFds=[] { std::vector<int> result; for (int i=0;i<256;++i) if (::fcntl(i,F_GETFD)>=0) result.push_back(i); return result; };
    for (int failure:{1,2,3}) {
        configure(capture,"0033\n"); const auto before=openFds(); captureIoFailure=failure;
        const int result=capture.capture(directory,rgba,2,2,sizeof(rgba)); captureIoFailure=0;
        require(result<0 && ::access(path,F_OK)!=0 && openFds()==before,
            "fdopen, actual buffered-write and close errors remove only new partial outputs and close every owned descriptor");
        require(capture.capture(directory,rgba,2,2,sizeof(rgba))==0,"failed diagnostic output is not replayed on the next presentation");
    }
    configure(capture,"0033\n");
    require(capture.capture(nullptr,rgba,2,2,sizeof(rgba))<0,"missing output scope rejects before file creation");
    configure(capture,"0033\n"); std::string tooLong(200,'x');
    require(capture.capture(tooLong.c_str(),rgba,2,2,sizeof(rgba))<0,"truncated output paths cannot redirect diagnostic creation");
    configure(capture,"0033\n");
    require(capture.capture("/nonexistent-galaxy-capture-directory",rgba,2,2,sizeof(rgba))<0,
        "actual creation failure is not a successful capture");
    configure(capture,"bad\n"); require(!capture.tag()[0] && !capture.present(),"invalid reconfiguration clears a previous valid tag and counter");
    require(::rmdir(directory)==0,"isolated diagnostic directory has no leftover files");
}
int main() {
    xfbSnapshot();
    shaderInventory();
    require(describeGxRamTexture && decodeGxRamTexture && resolveVitaRamImage && resolveVitaDrawWithRam,
        "real RAM texture decoding/native provenance and draw resolver exist");
    require(copyVitaEfbToMemory,"production native capture/process/RAM commit/copy-clear operation exists");
    require(describeGxTextureCopy && produceGxCopyPixels && processVitaEfbCapture,
            "real GX processed padded pixels and provenance-checked native capture consumer exist");
    require(createGxCopyHistory && storeGxCopyBytes && lookupGxCopyBytes && collectGxCopyBytes && releaseGxCopyHistory,
            "real GX packed bytes, ordered RAM writes and immutable copy history exist");
    require(port_vita_gxm_surface_sample && genVitaEfbSnapshotFragmentShader && captureVitaEfb,
            "real native color/tiled-Z24 sampling and lease-pinned GPU plane capture exist");
    require(createVitaEfbCache && resolveVitaEfb && clearVitaEfb && discardVitaEfbClear && collectVitaEfbs && releaseVitaEfbCache,
            "production leased full-size EFB and GPU color/depth clear exist");
    require(resolveVitaDraw,"production material/image resolver joins immutable draw resources transactionally");
    require(prepareVitaDraw && createVitaFrameReplay && submitVitaFrame && discardVitaFrame && releaseVitaFrameReplay,
            "full CPU-XF native frame bridge is implemented");
    const pid_t viChild=fork(); require(viChild>=0,"VI clock fixture isolates its process-lifetime thread");
    if (!viChild) { testViFlush(); nativeViDisplay(); _exit(0); }
    int viStatus=0;
    require(waitpid(viChild,&viStatus,0)==viChild && WIFEXITED(viStatus) && WEXITSTATUS(viStatus)==0,
        "real VI flush and native-worker display integration passes in its reaped isolated process");
    baseMirrorSampling();
    textureImageLifetime(); materials(); materialProfiles(); additiveProfile(); srcAlphaDitherProfile(); srcAlphaAddProfile(); earlyAlphaRaster(); vitaOverlaps(); textureCache(); preparation(); drawResolver(); traversal(); frameSnapshots(); frameSnapshots(0x313d); frameSnapshots(0x34ad); frameSnapshots(0x3429); frameSnapshots(0x343d); efbCpuFailures(); efbClears(); efbCaptures(); gxCopyBytes(); gxXfbCopies(); gxIdentityCopyRows(); gxCopyPixels(); efbCopyCommit(); nativeXfbCopyCommit(); ramTextures(); ramReplay(); nativeBringup(); nativeGameWorker(); nativeGameEntry(); liveRecorder();
    if (const char* dump=std::getenv("VITA_EFB_DUMP_SHADER"); dump && !std::strcmp(dump,"1")) {
        const std::string shader=genVitaEfbClearFragmentShader();
        FILE* file=std::fopen("/shader-output/efb_clear_f.cg","wb");
        require(file && std::fwrite(shader.data(),1,shader.size(),file)==shader.size(),"emit current production clear shader into owned ignored build mount");
        require(std::fclose(file)==0,"generated clear source fully saved");
        for (bool depth:{false,true}) {
            const std::string source=genVitaEfbSnapshotFragmentShader(depth);
            file=std::fopen(depth ? "/shader-output/efb_snapshot_depth_f.cg" : "/shader-output/efb_snapshot_color_f.cg","wb");
            require(file && std::fwrite(source.data(),1,source.size(),file)==source.size(),"emit current production capture shader into owned ignored build mount");
            require(std::fclose(file)==0,"generated capture source fully saved");
        }
    }
    printf("Vita frame bridge: %u sanitized real cross-unit checks; SDK doubles, not native pixels/FPS\n",checks);
}
