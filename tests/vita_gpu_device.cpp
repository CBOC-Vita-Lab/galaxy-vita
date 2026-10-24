// Production GXM/device ownership tested against the actual SDK declarations.
#include "gpu_device.h"
#include "gpu_texture.h"
#include "gpu_staging.h"
#include "gpu_program_internal.h"
#include <psp2/gxm.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

#pragma weak port_vita_gxm_device_create
#pragma weak port_vita_gxm_device_release
#pragma weak port_vita_gxm_device_context
#pragma weak port_vita_gxm_device_patcher
#pragma weak port_vita_gxm_device_present
#pragma weak port_vita_gxm_device_begin_display
#pragma weak port_vita_gxm_device_begin
#pragma weak port_vita_gxm_device_end
#pragma weak port_vita_gxm_device_finish
#pragma weak port_vita_gxm_device_drain_display
#pragma weak port_vita_gxm_device_error
#pragma weak port_vita_gxm_surface_create
#pragma weak port_vita_gxm_surface_info
#pragma weak port_vita_gxm_surface_release
#pragma weak port_vita_gxm_texture_create
#pragma weak port_vita_gxm_texture_info
#pragma weak port_vita_gxm_texture_level
#pragma weak port_vita_gxm_texture_descriptor
#pragma weak port_vita_gxm_texture_release
#pragma weak port_vita_gxm_device_program_create
#pragma weak port_vita_gxm_device_program_release
#pragma weak port_vita_gxm_staging_create
#pragma weak port_vita_gxm_staging_upload
#pragma weak port_vita_gxm_staging_begin
#pragma weak port_vita_gxm_staging_draw
#pragma weak port_vita_gxm_staging_end
#pragma weak port_vita_gxm_staging_reset
#pragma weak port_vita_gxm_staging_release
struct Allocation { void* base; unsigned size, kind; };
static std::map<int, Allocation> memory;
struct ColorPlaneExtent { unsigned stride,height; };
static std::map<const void*,ColorPlaneExtent> color_planes;
[[maybe_unused]] static std::map<const SceGxmColorSurface*,void*> color_surface_data;
[[maybe_unused]] static void* active_color_data;
static std::map<const void*,unsigned> depth_planes;
static unsigned checks, steps, fail_step, finishes, scene_ends;
static int next_uid = 10, begin_error, end_error, queue_error, destroy_error, display_error, free_error;
static int unmap_error, drain_error, wait_error, vblank_error;
static const void* unmap_failure_address;
static int unmap_address_error;
static bool initialized, active_scene, pending_gpu;
static bool detach_pending;
static void* scanout;
static SceGxmInitializeParams init_params;
static SceGxmShaderPatcherParams patcher_params;
static std::vector<void*> patcher_blocks;
struct QueuedSwap {
    std::vector<char> data;
    SceGxmSyncObject* oldSync;
    SceGxmSyncObject* newSync;
};
static std::vector<QueuedSwap> queue;
static std::vector<std::string> events;
static SceGxmContext* context = reinterpret_cast<SceGxmContext*>(0x1230);
static SceGxmShaderPatcher* patcher = reinterpret_cast<SceGxmShaderPatcher*>(0x2340);
static int texture_error;
static unsigned sampler_calls, sampler_fail_call;
struct TextureInitRecord { const void* data; unsigned width,height,levels; SceGxmTextureFormat format; bool tiled; };
[[maybe_unused]] static std::vector<TextureInitRecord> texture_inits;
static unsigned texture_validations, texture_validate_fail_call;
static int depth_texture_error;
static int program_error;
[[maybe_unused]] static bool program_alive;
static bool program_allocates;
[[maybe_unused]] static PortVitaGxmDevice* program_owner;
static bool program_bound;
static int draw_error;
static unsigned staged_draws, used_programs;
[[maybe_unused]] static std::vector<const void*> staged_streams;
static std::map<SceGxmRenderTarget*, bool> targets;
static std::map<SceGxmSyncObject*, bool> syncs;
struct DepthMode { bool load = false, store = false, submitted = false; };
static std::map<const SceGxmDepthStencilSurface*, DepthMode> depth_modes;
static const SceGxmDepthStencilSurface* active_depth;
static void require(bool value, const char* message) {
    ++checks; if (!value) { fprintf(stderr, "GPU device FAILED: %s (step %u)\n", message, steps); exit(1); }
}
static void completeSwap() {
    require(!queue.empty() && !active_scene, "display callback completes an owned queued swap outside a scene");
    pending_gpu=false;
    init_params.displayQueueCallback(queue.front().data.data());
    queue.erase(queue.begin());
}
static bool queuedBuffer(const void* base) {
    for (const auto& swap:queue) {
        require(swap.data.size()==2*sizeof(void*), "queued callback owns device and framebuffer pointers");
        void* pixels=nullptr;
        std::memcpy(&pixels,swap.data.data()+sizeof(void*),sizeof(pixels));
        if (pixels==base) return true;
    }
    return false;
}
static bool fail() { return ++steps == fail_step; }
static Allocation* allocation(const void* base) {
    for (auto& [uid, block] : memory) { (void)uid; if (block.base == base) return &block; }
    return nullptr;
}
extern "C" SceUID sceKernelAllocMemBlock(const char*, SceKernelMemBlockType type, SceSize size, SceKernelAllocMemBlockOpt*) {
    if (fail()) return -701;
    const unsigned alignment = type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? 256 * 1024 : 4096;
    require(size && size % alignment == 0, "native physical allocation granularity");
    uintptr_t address = 0x60000000;
    for (;;) {
        bool occupied = false;
        for (const auto& [uid, block] : memory) {
            (void)uid;
            const auto start = reinterpret_cast<uintptr_t>(block.base);
            if (address < start + block.size && start < address + size) { occupied = true; break; }
        }
        if (!occupied) break;
        address += 256 * 1024;
    }
    void* base = mmap(reinterpret_cast<void*>(address), size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    require(base != MAP_FAILED && reinterpret_cast<uintptr_t>(base) + size < UINT32_MAX, "isolated mapped low range");
    const int uid = next_uid++; memory[uid] = {base, size, 0}; events.push_back("alloc"); return uid;
}
extern "C" int sceKernelGetMemBlockBase(SceUID uid, void** base) {
    if (fail()) return -702;
    require(memory.count(uid), "owned UID base lookup"); *base = memory.at(uid).base; return 0;
}
extern "C" int sceKernelFreeMemBlock(SceUID uid) {
    require(!active_scene && !pending_gpu && !detach_pending && !queuedBuffer(memory.at(uid).base),
            "no GPU, queued framebuffer or scanout-wait reference at physical free");
    require(memory.count(uid) && !memory.at(uid).kind && memory.at(uid).base != scanout, "unmapped and not scanned out before free");
    if (free_error) return free_error;
    auto block = memory.at(uid); require(munmap(block.base, block.size) == 0, "host fixture free");
    color_planes.erase(block.base); depth_planes.erase(block.base);
    memory.erase(uid); events.push_back("free"); return 0;
}
static int map(void* base, unsigned size, unsigned kind, unsigned* offset = nullptr) {
    if (fail()) return -703;
    auto* block = allocation(base); require(block && block->size == size && !block->kind, "exact whole range map");
    block->kind = kind; if (offset) *offset = static_cast<unsigned>(reinterpret_cast<uintptr_t>(base)) & 0x0fffffff;
    return 0;
}
extern "C" int sceGxmMapMemory(void* base, SceSize size, SceGxmMemoryAttribFlags flags) { return map(base, size, flags); }
extern "C" int sceGxmMapVertexUsseMemory(void* base, SceSize size, unsigned* offset) { return map(base, size, 4, offset); }
extern "C" int sceGxmMapFragmentUsseMemory(void* base, SceSize size, unsigned* offset) { return map(base, size, 5, offset); }
static int unmap(void* base, unsigned kind) {
    auto* block = allocation(base); require(block && block->kind == kind && !pending_gpu && !active_scene,
                                          "matching kind and completed GPU before unmap");
    if (unmap_error) return unmap_error;
    if (unmap_address_error && base==unmap_failure_address) return unmap_address_error;
    block->kind = 0; events.push_back("unmap"); return 0;
}
extern "C" int sceGxmUnmapMemory(void* base) { return unmap(base, allocation(base)->kind); }
extern "C" int sceGxmUnmapVertexUsseMemory(void* base) { return unmap(base, 4); }
extern "C" int sceGxmUnmapFragmentUsseMemory(void* base) { return unmap(base, 5); }
extern "C" int sceGxmInitialize(const SceGxmInitializeParams* params) {
    if (fail()) return -704;
    require(!initialized && params->displayQueueMaxPendingCount == 2 && params->displayQueueCallback &&
            params->parameterBufferSize == SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE, "one real GXM initialization and default parameter ring");
    init_params = *params; initialized = true; events.push_back("init"); return 0;
}
extern "C" int sceGxmTerminate() {
    require(initialized && memory.empty() && targets.empty() && syncs.empty() && queue.empty() && !scanout,
            "all resources and scanout retired before terminate");
    if (destroy_error) return destroy_error;
    initialized = false; events.push_back("terminate"); return 0;
}
extern "C" int sceGxmCreateContext(const SceGxmContextParams* params, SceGxmContext** out) {
    if (fail()) return -705;
    require(params->hostMem && params->hostMemSize >= SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE, "context host memory lifetime");
    for (void* base : {params->vdmRingBufferMem, params->vertexRingBufferMem, params->fragmentRingBufferMem})
        require(allocation(base) && allocation(base)->kind == 1, "GPU-readable command/default-uniform rings");
    require(allocation(params->fragmentUsseRingBufferMem)->kind == 5, "mapped fragment USSE ring");
    require(params->vertexRingBufferMemSize >= SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE &&
            params->fragmentRingBufferMemSize >= SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, "not a reduced uniform ring");
    *out = context; events.push_back("context-create"); return 0;
}
extern "C" int sceGxmDestroyContext(SceGxmContext* value) {
    require(value == context && !active_scene && !pending_gpu && queue.empty(), "context only destroyed after completion");
    if (destroy_error) return destroy_error;
    events.push_back("context-destroy"); return 0;
}
extern "C" int sceGxmShaderPatcherCreate(const SceGxmShaderPatcherParams* params, SceGxmShaderPatcher** out) {
    if (fail()) return -706;
    require(params->userData && params->hostAllocCallback && params->hostFreeCallback && params->bufferAllocCallback &&
            params->bufferFreeCallback && params->vertexUsseAllocCallback && params->fragmentUsseAllocCallback,
            "dynamic real patcher allocation callbacks, not a fixed shader pool");
    patcher_params = *params;
    *out = patcher; events.push_back("patcher-create"); return 0;
}
static void allocate_patcher_blocks() {
    unsigned offset = 0;
    auto& p = patcher_params;
    patcher_blocks.push_back(p.bufferAllocCallback(p.userData, 9000));
    patcher_blocks.push_back(p.vertexUsseAllocCallback(p.userData, 5000, &offset));
    patcher_blocks.push_back(p.fragmentUsseAllocCallback(p.userData, 7000, &offset));
    for (auto* base : patcher_blocks) require(base && allocation(base), "patcher owns mapped executable/storage allocation");
}
extern "C" int sceGxmShaderPatcherDestroy(SceGxmShaderPatcher* value) {
    require(value == patcher && !active_scene && !pending_gpu, "patcher survives scene and GPU execution");
    if (destroy_error) return destroy_error;
    if (!patcher_blocks.empty()) {
        auto& p = patcher_params;
        p.bufferFreeCallback(p.userData, patcher_blocks[0]);
        p.vertexUsseFreeCallback(p.userData, patcher_blocks[1]);
        p.fragmentUsseFreeCallback(p.userData, patcher_blocks[2]); patcher_blocks.clear();
    }
    events.push_back("patcher-destroy"); return 0;
}
extern "C" int sceGxmCreateRenderTarget(const SceGxmRenderTargetParams* params, SceGxmRenderTarget** out) {
    if (fail()) return -707;
    require(params->width && params->height && params->multisampleMode == SCE_GXM_MULTISAMPLE_NONE && params->driverMemBlock == -1,
            "native render target retains dimensions");
    *out = reinterpret_cast<SceGxmRenderTarget*>(uintptr_t(next_uid++) * 16); targets[*out] = true; return 0;
}
extern "C" int sceGxmDestroyRenderTarget(SceGxmRenderTarget* value) {
    require(targets.count(value) && !active_scene && !pending_gpu, "render target fence and ownership");
    if (destroy_error) return destroy_error;
    targets.erase(value); events.push_back("target-destroy"); return 0;
}
extern "C" int sceGxmSyncObjectCreate(SceGxmSyncObject** out) {
    if (fail()) return -708;
    *out = reinterpret_cast<SceGxmSyncObject*>(uintptr_t(next_uid++) * 16); syncs[*out] = true; return 0;
}
extern "C" int sceGxmSyncObjectDestroy(SceGxmSyncObject* value) {
    require(syncs.count(value) && !active_scene && !pending_gpu &&
        std::none_of(queue.begin(),queue.end(),[&](const QueuedSwap& swap) {
            return swap.oldSync==value || swap.newSync==value;
        }), "specific sync outlives its GPU and queued display references");
    if (destroy_error) return destroy_error;
    syncs.erase(value); return 0;
}
extern "C" int sceGxmColorSurfaceInit(SceGxmColorSurface* surface, SceGxmColorFormat format, SceGxmColorSurfaceType type,
    SceGxmColorSurfaceScaleMode scale, SceGxmOutputRegisterSize reg, unsigned width, unsigned height, unsigned stride, void* data) {
    if (fail()) return -709;
    require(format == SCE_GXM_COLOR_FORMAT_A8B8G8R8 && type == SCE_GXM_COLOR_SURFACE_LINEAR &&
            scale == SCE_GXM_COLOR_SURFACE_SCALE_NONE && reg == SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
            "RGBA8 native output without downscaling");
    require(width <= stride && stride % 64 == 0 && allocation(data)->size >= stride * height * 4 && allocation(data)->kind == 3,
            "complete color extent and RW GXM mapping");
    color_planes[data]={stride,height}; color_surface_data[surface]=data; return 0;
}
extern "C" int sceGxmDepthStencilSurfaceInit(SceGxmDepthStencilSurface* surface, SceGxmDepthStencilFormat format,
    SceGxmDepthStencilSurfaceType type, unsigned stride, void* data, void* stencil) {
    if (fail()) return -710;
    require(format == SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24 && type == SCE_GXM_DEPTH_STENCIL_SURFACE_TILED && stride % 32 == 0 &&
            allocation(data)->kind == 3 && !stencil, "full 24-bit packed tiled depth, not reduced precision");
    depth_planes[data]=stride; depth_modes[surface] = {}; return 0;
}
extern "C" void sceGxmDepthStencilSurfaceSetForceLoadMode(SceGxmDepthStencilSurface* surface, SceGxmDepthStencilForceLoadMode mode) {
    depth_modes[surface].load = mode == SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED;
}
extern "C" void sceGxmDepthStencilSurfaceSetForceStoreMode(SceGxmDepthStencilSurface* surface, SceGxmDepthStencilForceStoreMode mode) {
    depth_modes[surface].store = mode == SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED;
}
extern "C" int sceGxmBeginScene(SceGxmContext* value, unsigned, const SceGxmRenderTarget* target, const SceGxmValidRegion*,
    SceGxmSyncObject*, SceGxmSyncObject* sync, const SceGxmColorSurface* color, const SceGxmDepthStencilSurface* depth) {
    require(value == context && targets.count(const_cast<SceGxmRenderTarget*>(target)) && syncs.count(sync) && color && depth && !active_scene,
            "scene uses owned render/sync/color/depth resources");
    if (begin_error) return begin_error;
    while (std::any_of(queue.begin(),queue.end(),[&](const QueuedSwap& swap) {
        return swap.oldSync==sync || swap.newSync==sync;
    })) completeSwap();
    require(depth_modes.at(depth).store && depth_modes.at(depth).load == depth_modes.at(depth).submitted,
            "depth stores every scene and reloads submitted EFB instead of implicit clear");
    active_depth = depth;
    active_color_data = color_surface_data.at(color);
    active_scene = true; events.push_back("begin"); return 0;
}
extern "C" int sceGxmEndScene(SceGxmContext* value, const SceGxmNotification*, const SceGxmNotification*) {
    require(value == context && active_scene, "EndScene exactly once on active context");
    ++scene_ends; if (end_error) return end_error;
    depth_modes.at(active_depth).submitted = true;
    active_scene = false; pending_gpu = true; events.push_back("end"); return 0;
}
#ifndef PORT_VITA_REAL_PROGRAM
extern "C" int port_vita_gxm_scene_end(void* value, PortVitaGxmProgram* const* programs, uint32_t count,
                                       const void*, const void*) {
    used_programs=count;
    require(count==(program_bound ? 1u : 0u) && (!count || programs[0]==reinterpret_cast<PortVitaGxmProgram*>(0x9900)),
            "complete deduplicated bound list, never every owned/unused program");
    const int result=sceGxmEndScene(static_cast<SceGxmContext*>(value), nullptr, nullptr);
    if (result>=0) program_bound=false;
    return result;
}
bool port_vita_gxm_program_bound(PortVitaGxmProgram* p, void* value) {
    return p==reinterpret_cast<PortVitaGxmProgram*>(0x9900) && value==context && program_bound;
}
extern "C" int port_vita_gxm_program_draw_raster(PortVitaGxmProgram* p, void* value,
    const PortVitaGpuMemory* vertices, uint32_t vertex_offset, const PortVitaGpuMemory* indices, uint32_t index_offset,
    uint32_t vertex_count, uint32_t index_count, uint32_t topology, const float*, uint32_t,
    const void* const[8], const float*) {
    require(p==reinterpret_cast<PortVitaGxmProgram*>(0x9900) && value==context && active_scene && program_alive,
            "staged submission uses leased program and active native scene");
    require(vertices==indices && vertices->mapping==1 && vertex_count && index_count && topology==2 &&
            uint64_t(vertex_offset)+vertex_count*152<=vertices->size && uint64_t(index_offset)+index_count*2<=indices->size,
            "staged mapped vertex/index extents");
    program_bound=true; ++staged_draws;
    staged_streams.push_back(static_cast<const char*>(vertices->base)+vertex_offset);
    return draw_error;
}
#endif
extern "C" void sceGxmFinish(SceGxmContext* value) {
    require(value == context && !active_scene, "Finish cannot submit an open scene");
    pending_gpu = false; ++finishes; events.push_back("finish");
}
extern "C" int sceGxmPadHeartbeat(const SceGxmColorSurface* color, SceGxmSyncObject* sync) {
    require(!active_scene && color && syncs.count(sync), "heartbeat only after EndScene"); return 0;
}
extern "C" int sceGxmDisplayQueueAddEntry(SceGxmSyncObject* old_sync, SceGxmSyncObject* new_sync, const void* data) {
    require(syncs.count(old_sync) && syncs.count(new_sync) && old_sync != new_sync && !active_scene,
            "distinct previous/new display GPU syncs");
    if (queue_error) return queue_error;
    while (queue.size()>=init_params.displayQueueMaxPendingCount) completeSwap();
    const char* bytes = static_cast<const char*>(data);
    queue.push_back({{bytes, bytes + init_params.displayQueueCallbackDataSize},old_sync,new_sync});
    events.push_back("queue"); return 0;
}
extern "C" int sceGxmDisplayQueueFinish() {
    if (drain_error) return drain_error;
    while (!queue.empty()) completeSwap();
    events.push_back("drain"); return 0;
}
extern "C" int sceDisplaySetFrameBuf(const SceDisplayFrameBuf* buffer, SceDisplaySetBufSync sync) {
    require(sync == SCE_DISPLAY_SETBUF_NEXTFRAME, "scanout changes at frame boundary");
    if (display_error) return display_error;
    if (buffer) {
        require(buffer->size == sizeof(*buffer) && buffer->width == 960 && buffer->height == 544 &&
                buffer->pitch == 1024 && buffer->pixelformat == SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 && allocation(buffer->base),
                "full native display buffer descriptor"); scanout = buffer->base;
    } else { scanout = nullptr; detach_pending = true; events.push_back("detach"); }
    return 0;
}
extern "C" int sceDisplayWaitVblankStart() { events.push_back("vblank"); return vblank_error; }
extern "C" int sceDisplayWaitSetFrameBuf() {
    events.push_back("detach-wait"); if (wait_error) return wait_error;
    detach_pending = false; return 0;
}
extern "C" int sceGxmTextureInitLinear(SceGxmTexture* texture, const void* data, SceGxmTextureFormat format,
                                      unsigned width, unsigned height, unsigned levels) {
    require(texture && allocation(data) && width && height && levels &&
            format == SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, "native RGBA8 linear descriptor has complete mapped storage");
    if (allocation(data)->kind==3) {
        require(color_planes.count(data) && width==color_planes.at(data).stride && height==color_planes.at(data).height &&
                !active_scene && !pending_gpu && levels==1 && width%64==0 &&
                uint64_t(width)*height*4<=allocation(data)->size,
                "native color alias uses its actual fenced RW pitch and one level");
    } else require(allocation(data)->kind==1,"ordinary immutable RGBA8 mip storage must still map READ");
    texture_inits.push_back({data,width,height,levels,format,false});
    if (texture_error) return texture_error;
    std::memset(texture, 0, sizeof(*texture)); return 0;
}
extern "C" int sceGxmTextureInitTiled(SceGxmTexture* texture,const void* data,SceGxmTextureFormat format,
                                     unsigned width,unsigned height,unsigned levels) {
    require(texture && allocation(data) && allocation(data)->kind==3 && depth_planes.count(data) && width==depth_planes.at(data) &&
            !active_scene && !pending_gpu &&
            levels==1 && width && height && width%32==0 && height%32==0 &&
            uint64_t(width)*height*4<=allocation(data)->size && format==SCE_GXM_TEXTURE_FORMAT_X8U24_SD,
            "depth alias uses real packed S8D24 storage with tiled X8U24 sampling, never a CPU linear reinterpretation");
    texture_inits.push_back({data,width,height,levels,format,true});
    if (depth_texture_error) return depth_texture_error;
    std::memset(texture,0,sizeof(*texture)); return 0;
}
extern "C" int sceGxmTextureValidate(const SceGxmTexture*) {
    if (++texture_validations==texture_validate_fail_call) return -913;
    return texture_error;
}
static int sampler_set() { return ++sampler_calls == sampler_fail_call ? -861 : 0; }
extern "C" int sceGxmTextureSetUAddrMode(SceGxmTexture* t, SceGxmTextureAddrMode v) {
    const int r=sampler_set(); if (!r) t->generic.uaddr_mode=v; return r;
}
extern "C" int sceGxmTextureSetVAddrMode(SceGxmTexture* t, SceGxmTextureAddrMode v) {
    const int r=sampler_set(); if (!r) t->generic.vaddr_mode=v; return r;
}
extern "C" int sceGxmTextureSetMinFilter(SceGxmTexture* t, SceGxmTextureFilter v) {
    const int r=sampler_set(); if (!r) t->generic.min_filter=v; return r;
}
extern "C" int sceGxmTextureSetMagFilter(SceGxmTexture* t, SceGxmTextureFilter v) {
    const int r=sampler_set(); if (!r) t->generic.mag_filter=v; return r;
}
extern "C" int sceGxmTextureSetMipFilter(SceGxmTexture* t, SceGxmTextureMipFilter v) {
    const int r=sampler_set(); if (!r) t->generic.mip_filter=v!=0; return r;
}
extern "C" int sceGxmTextureSetLodBias(SceGxmTexture* t, unsigned v) {
    const int r=sampler_set(); if (!r) t->generic.lod_bias=v; return r;
}
extern "C" int sceGxmTextureSetLodMin(SceGxmTexture* t, unsigned v) {
    const int r=sampler_set(); if (!r) { t->lod_min0=v&3; t->lod_min1=v>>2; } return r;
}
extern "C" int sceGxmTextureSetMipmapCount(SceGxmTexture* t, unsigned v) {
    const int r=sampler_set(); if (!r) t->generic.mip_count=v-1; return r;
}
#ifndef PORT_VITA_REAL_PROGRAM
extern "C" int port_vita_gxm_program_create(void* value, const void*, uint32_t, const void*, uint32_t,
    const PortVitaGxmBlend*, uint32_t, uint32_t, PortVitaGxmProgram** out, PortVitaGxmProgramInfo*) {
    require(value == patcher && !program_alive, "device program wrapper uses owned patcher");
    if (program_allocates) {
        void* block = patcher_params.bufferAllocCallback(patcher_params.userData, 9000);
        if (!block) return -811;
        patcher_params.bufferFreeCallback(patcher_params.userData, block);
    }
    *out = reinterpret_cast<PortVitaGxmProgram*>(0x9900); program_alive = true; return program_error;
}
extern "C" int port_vita_gxm_program_release(PortVitaGxmProgram** out) {
    require(!active_scene && !pending_gpu && program_alive && *out == reinterpret_cast<PortVitaGxmProgram*>(0x9900),
            "owned program release after context retirement");
    if (program_error) return program_error;
    *out = nullptr; program_alive = false; return 0;
}
void port_vita_gxm_program_adopt(PortVitaGxmProgram* p, PortVitaGxmDevice* owner) {
    require(p==reinterpret_cast<PortVitaGxmProgram*>(0x9900) && program_alive && owner,
            "device adopts actual returned recovery/ready program"); program_owner=owner;
}
int port_vita_gxm_program_release_owned(PortVitaGxmProgram** p, PortVitaGxmDevice* owner) {
    require(program_owner==owner,"managed release cannot bypass actual owner identity");
    const int result=port_vita_gxm_program_release(p);
    if (result>=0) program_owner=nullptr;
    return result;
}
#endif

static void empty_state() {
    require(memory.empty() && targets.empty() && syncs.empty() && queue.empty() && patcher_blocks.empty() &&
            !initialized && !active_scene && !pending_gpu && !scanout && !detach_pending, "all owned resources released");
    color_surface_data.clear(); active_color_data=nullptr;
}
int main() {
    require(port_vita_gxm_device_create != nullptr, "native GXM context/display owner exists");
    PortVitaGxmDevice* device = nullptr;
    require(port_vita_gxm_device_create(&device) == 0 && device, "complete device creation");
    const unsigned creation_steps = steps;
    require(!scanout && queue.empty(), "creation does not present a fake frame");
    require(port_vita_gxm_device_context(device) == context && port_vita_gxm_device_patcher(device) == patcher, "borrowed native context/patcher");
    PortVitaGxmDevice* other = nullptr;
    require(port_vita_gxm_device_create(&other) < 0 && !other, "only one global GXM owner");
    allocate_patcher_blocks();
    require(port_vita_gxm_device_program_create != nullptr, "device-retained native program registration exists");
    PortVitaGxmProgram* owned_program = nullptr;
    PortVitaGxmBlend blend{};
    PortVitaGxmProgramInfo program_info{};
    require(port_vita_gxm_device_program_create(device, nullptr, 0, nullptr, 0, &blend, 0, 0, &owned_program, &program_info) == 0 && owned_program,
            "patched program retains context/patcher owner");
    require(port_vita_gxm_device_release(&device) < 0, "registered program blocks premature patcher destruction");
    program_error = -808;
    require(port_vita_gxm_device_program_release(device, &owned_program) == -808 && owned_program &&
            port_vita_gxm_device_release(&device) < 0, "failed SDK program release preserves device lease"); program_error = 0;
    require(port_vita_gxm_device_program_release(device, &owned_program) == 0 && !owned_program, "successful unregister drops exactly one device lease");
    program_error = -808;
    require(port_vita_gxm_device_program_create(device, nullptr, 0, nullptr, 0, &blend, 0, 0, &owned_program, &program_info) == -808 && owned_program,
            "failed program creation with recovery ownership retains device"); program_error = 0;
    require(port_vita_gxm_device_program_release(device, &owned_program) == 0, "release program recovery state through same owner");
    program_allocates = true; fail_step = steps + 1;
    require(port_vita_gxm_device_program_create(device, nullptr, 0, nullptr, 0, &blend, 0, 0, &owned_program, &program_info) == -811 && !owned_program,
            "native patcher allocation failure with clean rollback"); fail_step = 0;
    require(port_vita_gxm_device_error(device) == 0 && port_vita_gxm_device_context(device) == context,
            "clean patcher allocation failure must not permanently poison the device");
    require(port_vita_gxm_device_program_create(device, nullptr, 0, nullptr, 0, &blend, 0, 0, &owned_program, &program_info) == 0 && owned_program,
            "same-device retry after clean patcher allocation failure reaches SDK"); program_allocates = false;
    require(port_vita_gxm_device_program_release(device, &owned_program) == 0, "retry program safely released");
    require(port_vita_gxm_device_present(device) < 0, "cannot present an unrendered buffer");
    begin_error = -801;
    require(port_vita_gxm_device_begin_display(device) == -801, "failed BeginScene remains closed"); begin_error = 0;
    require(port_vita_gxm_device_begin_display(device) == 0, "native display scene begin");
    const auto before = finishes;
    require(port_vita_gxm_device_begin_display(device) < 0 && port_vita_gxm_device_finish(device) < 0 &&
            port_vita_gxm_device_release(&device) < 0 && finishes == before, "no overwrite/Finish/destroy during open scene");
    end_error = -802;
    require(port_vita_gxm_device_end(device, nullptr, 0) == -802 && active_scene, "failed EndScene keeps scene ownership"); end_error = 0;
    require(port_vita_gxm_device_end(device, nullptr, 0) == 0 && !active_scene, "successful EndScene submits work");
    require(port_vita_gxm_device_begin_display(device) < 0, "unpresented display scene cannot be overwritten");
    queue_error = -803;
    require(port_vita_gxm_device_present(device) == -803 && queue.empty(), "queue failure preserves exact rendered buffer"); queue_error = 0;
    require(port_vita_gxm_device_present(device) == 0 && queue.size() == 1 && !scanout, "GPU-gated async display queued once");
    PortVitaGxmSurface* efb = nullptr;
    require(port_vita_gxm_surface_create(device, 640, 456, &efb) == 0 && efb, "full original EFB creation");
    PortVitaGxmSurfaceInfo info = {};
    require(port_vita_gxm_surface_info(efb, &info) == 0 && info.width == 640 && info.height == 456 &&
            info.stride == 640 && info.color_surface && info.depth_surface && info.color_data && info.depth_data,
            "EFB dimensions/precision are separate from native display");
    require(port_vita_gxm_device_release(&device) < 0, "external surface keeps its context owner alive");
    require(port_vita_gxm_device_begin(device, efb) == 0 && port_vita_gxm_surface_release(&efb) < 0, "surface cannot release an open EFB scene");
    require(port_vita_gxm_device_end(device, nullptr, 0) == 0 && port_vita_gxm_device_begin(device, efb) == 0 &&
            port_vita_gxm_device_end(device, nullptr, 0) == 0 && port_vita_gxm_surface_release(&efb) == 0 && !efb,
            "EFB retirement fences before releasing its buffers");
    require(queue.size()==1 && !scanout, "unrelated offscreen EFB can retire while display output remains queued");
    require(port_vita_gxm_device_drain_display(device) == 0 && scanout, "native callback sets full framebuffer");
    for (unsigned i = 0; i < 7; ++i) {
        require(port_vita_gxm_device_begin_display(device) == 0 && port_vita_gxm_device_end(device, nullptr, 0) == 0 &&
                port_vita_gxm_device_present(device) == 0, "three-buffer rotation preserves scene/queue order");
        require(port_vita_gxm_device_drain_display(device) == 0, "display callback completion");
    }
    const auto allocation_count = memory.size();
    drain_error = -812;
    require(port_vita_gxm_device_release(&device) == -812 && device && memory.size() == allocation_count && scanout,
            "failed display drain preserves all GPU/scanout memory"); drain_error = 0;
    display_error = -813;
    require(port_vita_gxm_device_release(&device) == -813 && device && memory.size() == allocation_count && scanout,
            "failed scanout detachment preserves live framebuffer memory"); display_error = 0;
    wait_error = -814;
    require(port_vita_gxm_device_release(&device) == -814 && device && memory.size() == allocation_count && detach_pending,
            "failed detach wait blocks all physical memory release"); wait_error = 0;
    destroy_error = -804;
    require(port_vita_gxm_device_release(&device) == -804 && device, "failed destroy retains ownership"); destroy_error = 0;
    unmap_error = -815;
    require(port_vita_gxm_device_release(&device) == -815 && device, "failed patcher unmap retains callback-owned allocation for retry"); unmap_error = 0;
    free_error = -805;
    require(port_vita_gxm_device_release(&device) == -805 && device, "callback/unmap free failure survives patcher destruction"); free_error = 0;
    require(port_vita_gxm_device_release(&device) == 0 && !device, "teardown retry does not reuse destroyed SDK resources"); empty_state();
    for (unsigned failure = 1; failure <= creation_steps; ++failure) {
        fail_step = failure; steps = 0;
        require(port_vita_gxm_device_create(&device) < 0, "every native creation failure surfaces");
        fail_step = 0;
        if (device) require(port_vita_gxm_device_release(&device) == 0, "creation failure recovery release");
        empty_state();
    }
    steps = 0; fail_step = 3; free_error = -816;
    require(port_vita_gxm_device_create(&device) == -816 && device && !memory.empty(), "creation rollback failure returns a real recovery owner");
    fail_step = 0; free_error = 0;
    require(port_vita_gxm_device_release(&device) == 0 && !device, "retry partial initialization teardown without losing UID/global owner"); empty_state();
    steps = 0;
    require(port_vita_gxm_device_create(&device) == 0, "global owner resets after failed creates");
    require(port_vita_gxm_texture_create != nullptr, "mip-preserving native texture upload exists");
    // Literal independent expected layout: 9x5, 4x2, 2x1 content. Native linear
    // row pitch rounds to 8 pixels; mip starts use 16x8, 8x4, 4x2 footprints.
    std::vector<uint32_t> pixels(45 + 8 + 2);
    for (unsigned i = 0; i < pixels.size(); ++i) pixels[i] = 0x80000000 | i * 0x010203;
    const auto original_pixels = pixels;
    PortVitaGxmTexture* texture = nullptr;
    require(port_vita_gxm_texture_create(device, 9, 5, 3, 42, pixels.data(), pixels.size() * 4, &texture) == 0,
            "NPOT supplied mip chain copied without regeneration");
    PortVitaGxmTextureInfo tex_info{};
    require(port_vita_gxm_texture_info(texture, &tex_info) == 0 && tex_info.width == 9 && tex_info.height == 5 &&
            tex_info.levels == 3 && tex_info.native_levels == 3 && tex_info.version == 42 && tex_info.storage_bytes == 704,
            "immutable version and complete padded storage extent");
    const uint32_t expected_offsets[]{0, 512, 640}, expected_strides[]{16, 8, 8}, expected_bytes[]{512, 128, 64};
    unsigned source = 0;
    for (unsigned level = 0, width = 9, height = 5; level < 3; ++level, width /= 2, height /= 2) {
        PortVitaGxmTextureLevel layout{};
        require(port_vita_gxm_texture_level(texture, level, &layout) == 0 && layout.width == width && layout.height == height &&
                layout.stride == expected_strides[level] && layout.offset == expected_offsets[level] && layout.bytes == expected_bytes[level],
                "literal NPOT mip offset/row pitch, not packed source layout");
        const auto* stored = reinterpret_cast<const uint32_t*>(static_cast<const char*>(tex_info.pixels) + layout.offset);
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) require(stored[y * layout.stride + x] == pixels[source++], "every supplied mip texel preserved");
            for (unsigned x = width; x < layout.stride; ++x) require(stored[y * layout.stride + x] == 0, "row padding initialized");
        }
    }
    pixels.assign(pixels.size(), 0);
    require(static_cast<const uint32_t*>(tex_info.pixels)[0] == original_pixels[0], "GPU texture never aliases mutable source pixels");
    const void* descriptor = nullptr;
    require(port_vita_gxm_texture_descriptor(texture, &descriptor) == 0 && descriptor, "whole native mip chain descriptor");
    require(port_vita_gxm_device_release(&device) < 0, "texture retains its context lease");
    require(port_vita_gxm_device_begin_display(device) == 0 && port_vita_gxm_texture_release(&texture) < 0,
            "texture cannot free inside an open scene");
    require(port_vita_gxm_device_end(device, nullptr, 0) == 0, "texture pending-use fixture submitted");
    free_error = -809;
    require(port_vita_gxm_texture_release(&texture) == -809 && texture, "failed texture free retains lease/UID"); free_error = 0;
    require(port_vita_gxm_texture_release(&texture) == 0 && !texture, "texture release retry after real Finish");
    std::vector<uint32_t> tail(8 * 2 + 4 + 2 + 1, 0x12345678);
    require(port_vita_gxm_texture_create(device, 8, 2, 4, 43, tail.data(), tail.size() * 4, &texture) == 0, "rectangular GX mip tail is uploaded too");
    require(port_vita_gxm_texture_info(texture, &tex_info) == 0 && tex_info.levels == 4 && tex_info.native_levels == 2,
            "full GX chain distinct from native smaller-dimension capability");
    descriptor = nullptr;
    require(port_vita_gxm_texture_descriptor(texture, &descriptor) < 0 && !descriptor,
            "no silent quality loss by dropping the rectangular mip tail");
    PortVitaGxmTextureLevel last{};
    require(port_vita_gxm_texture_level(texture, 3, &last) == 0 && last.width == 1 && last.height == 1 &&
            *reinterpret_cast<const uint32_t*>(static_cast<const char*>(tex_info.pixels) + last.offset) == 0x12345678,
            "last rectangular mip texel remains available for explicit sampling");
    require(port_vita_gxm_texture_release(&texture) == 0, "rectangular mip storage release");
    texture_error = -810;
    require(port_vita_gxm_texture_create(device, 9, 5, 3, 44, original_pixels.data(), original_pixels.size() * 4, &texture) == -810 && !texture,
            "SDK descriptor failure rolls back GPU memory and lease"); texture_error = 0;
    require(port_vita_gxm_texture_create(device, 9, 5, 3, 44, original_pixels.data(), original_pixels.size() * 4 - 1, &texture) < 0 && !texture,
            "truncated mip pixels cannot become a texture");
    require(port_vita_gxm_texture_create(device, 0, 5, 1, 44, original_pixels.data(), original_pixels.size() * 4, &texture) < 0 && !texture,
            "invalid dimensions rejected before allocation");
    // A display scene is still ready from the texture-use fixture.
    require(port_vita_gxm_device_present(device) == 0 && port_vita_gxm_device_drain_display(device) == 0, "complete earlier display fixture");
    require(port_vita_gxm_texture_create(device, 9, 5, 3, 45, original_pixels.data(), original_pixels.size() * 4, &texture) == 0,
            "child texture for unhealthy-device retirement");
    require(port_vita_gxm_device_begin_display(device) == 0 && port_vita_gxm_device_end(device, nullptr, 0) == 0 &&
            port_vita_gxm_device_present(device) == 0, "callback error fixture queued");
    display_error = -806;
    require(port_vita_gxm_device_drain_display(device) == -806 && port_vita_gxm_device_error(device) == -806,
            "asynchronous display failure is not silently counted as presentation");
    require(port_vita_gxm_device_begin_display(device) < 0, "callback failure closes further rendering"); display_error = 0;
    require(port_vita_gxm_texture_release(&texture) == 0 && !texture, "callback error must not trap a child texture lease");
    require(port_vita_gxm_device_release(&device) == 0, "failed display still permits safe full teardown"); empty_state();
    require(&port_vita_gxm_staging_create!=nullptr,"fenced append-only native staging exists");
    require(port_vita_gxm_device_create(&device)==0,"staging device fixture");
    efb=nullptr;
    require(port_vita_gxm_surface_create(device,640,480,&efb)==0,"staging full-size EFB");
    PortVitaGxmProgram* program=nullptr;
    uint32_t gxp=0;
    require(port_vita_gxm_device_program_create(device,&gxp,4,&gxp,4,&blend,4,0,&program,&program_info)==0,
            "leased staging program fixture");
    PortVitaGxmStaging* staging=nullptr;
    require(port_vita_gxm_staging_create(device,4096,&staging)==0 && staging,"scene-closed staging resource lease");
    require(port_vita_gxm_device_release(&device)<0,"staging prevents premature context destruction");
    std::vector<PortVitaGxmVertex> staged_vertices(3);
    staged_vertices[0].clip[0]=0.75f;
    const uint16_t staged_indices[]={0,1,2};
    PortVitaGxmSlice first{}, second{}, large{};
    require(port_vita_gxm_staging_upload(staging,staged_vertices.data(),3,staged_indices,3,&first)==0 &&
            port_vita_gxm_staging_upload(staging,staged_vertices.data(),3,staged_indices,3,&second)==0,
            "append two draws without circular overwrite");
    require(first.block==second.block && first.vertex_offset==0 && first.index_offset==456 &&
            second.vertex_offset==464 && second.index_offset==920,
            "literal four-byte aligned vertex/index slices in same mapped slab");
    staged_vertices[0].clip[0]=0;
    require(static_cast<const PortVitaGxmVertex*>(first.block->base)[0].clip[0]==0.75f,
            "staged vertices do not alias mutable CPU scratch");
    const float staged_pixel[16]{}, staged_raster[8]{0,0,640,480,0,0,0,0};
    auto submit=[&](const PortVitaGxmSlice& slice) { return port_vita_gxm_staging_draw(staging,program,&slice,2,
        staged_pixel,16,nullptr,staged_raster); };
    require(port_vita_gxm_staging_begin(staging,efb)==0 && submit(first)==0 && submit(second)==0 && staged_draws==2,
            "append slices submit through exact scene owner");
    require(port_vita_gxm_staging_reset(staging)<0 && port_vita_gxm_staging_release(&staging)<0 && staging,
            "open scene cannot Finish/reset/free mapped staging");
    PortVitaGxmSlice rejected{};
    require(port_vita_gxm_staging_upload(staging,staged_vertices.data(),3,staged_indices,3,&rejected)<0,
            "no CPU allocation/upload inside native scene");
    end_error=-821;
    require(port_vita_gxm_staging_end(staging)==-821 && active_scene && used_programs==1 &&
            port_vita_gxm_staging_reset(staging)<0,"failed EndScene keeps mapped slices and complete bound list");
    end_error=0;
    require(port_vita_gxm_staging_end(staging)==0 && pending_gpu,"successful native EndScene without premature Finish");
    const unsigned previous_finishes=finishes;
    require(port_vita_gxm_staging_reset(staging)==0 && finishes==previous_finishes+1 && !pending_gpu,
            "real fence precedes reuse");
    require(port_vita_gxm_staging_begin(staging,efb)==0 && submit(first)<0 && staged_draws==2 &&
            port_vita_gxm_staging_end(staging)==0 && used_programs==0,"old epoch rejected before binding, unused program not included");
    require(port_vita_gxm_staging_reset(staging)==0,"empty scene fence");
    staged_vertices.resize(64000); staged_vertices[63999].clip[0]=0.25f;
    const uint16_t max_indices[]={0,1,63999};
    require(port_vita_gxm_staging_upload(staging,staged_vertices.data(),64000,max_indices,3,&large)==0 &&
            large.block!=first.block && large.block->size>=9728008 &&
            reinterpret_cast<const PortVitaGxmVertex*>(static_cast<const char*>(large.block->base)+large.vertex_offset)[63999].clip[0]==0.25f,
            "maximum whole U16 draw gets larger slab, no vertex/quality truncation");
    require(port_vita_gxm_staging_begin(staging,efb)==0,"failed draw scene begin");
    draw_error=-822;
    require(submit(large)==-822 && port_vita_gxm_staging_end(staging)==0 && used_programs==1,
            "bind followed by submission failure still acknowledged at EndScene"); draw_error=0;
    require(port_vita_gxm_staging_reset(staging)==0,"failed draw retires safely");
    fail_step=steps+1;
    PortVitaGxmStaging* fault_staging=nullptr;
    require(port_vita_gxm_staging_create(device,4096,&fault_staging)==0,"lazy slabs avoid speculative kernel allocations");
    require(port_vita_gxm_staging_upload(fault_staging,staged_vertices.data(),3,staged_indices,3,&rejected)<0,
            "kernel slab failure surfaced without slice"); fail_step=0;
    fail_step=steps+3; free_error=-823;
    require(port_vita_gxm_staging_upload(fault_staging,staged_vertices.data(),3,staged_indices,3,&rejected)==-823,
            "map rollback failure keeps UID in staging owner"); fail_step=0; free_error=0;
    require(port_vita_gxm_staging_reset(fault_staging)==0 && port_vita_gxm_staging_release(&fault_staging)==0,
            "failed slab cleanup can be retried without losing lease");
    unmap_error=-824;
    require(port_vita_gxm_staging_release(&staging)==-824 && staging,"staging failed free preserves resource owner"); unmap_error=0;
    require(port_vita_gxm_staging_release(&staging)==0 && !staging,"staging release retry");
    require(port_vita_gxm_device_program_release(device,&program)==0 && port_vita_gxm_surface_release(&efb)==0 &&
            port_vita_gxm_device_release(&device)==0,"all staging leases retired before context"); empty_state();
    printf("GXM device: %u lifecycle/fault checks, %u creation fault points; SDK boundary, not hardware rendering\n", checks, creation_steps);
    return 0;
}
