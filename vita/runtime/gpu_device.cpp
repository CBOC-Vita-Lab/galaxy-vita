#include "gpu_device.h"
#include "port/game_progress.h"
#include "gpu_device_internal.h"
#include "gpu_program_internal.h"
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/kernel/error.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>

namespace {
constexpr int invalid = static_cast<int>(SCE_KERNEL_ERROR_INVALID_ARGUMENT);
constexpr int no_memory = static_cast<int>(SCE_KERNEL_ERROR_NO_MEMORY);
constexpr unsigned display_count = 3;
std::atomic<bool> global_owner{false};
struct PatcherAllocation {
    PortVitaGpuMemory memory{nullptr, 0, -1, 0, 0};
    PatcherAllocation* next = nullptr;
};
struct OwnedProgram {
    PortVitaGxmProgram* program = nullptr;
    const void* manager = nullptr;
    PortVitaGxmProgramInfo info{};
    bool ready = false;
    OwnedProgram* next = nullptr;
};
}

struct PortVitaGxmSurface {
    PortVitaGxmDevice* owner = nullptr;
    const void* manager = nullptr;
    PortVitaGpuMemory color{nullptr, 0, -1, 0, 0}, depth{nullptr, 0, -1, 0, 0};
    SceGxmColorSurface color_surface{};
    SceGxmDepthStencilSurface depth_surface{};
    SceGxmTexture color_texture{}, depth_texture{};
    SceGxmRenderTarget* target = nullptr;
    SceGxmSyncObject* sync = nullptr;
    uint32_t width = 0, height = 0, stride = 0, depth_stride = 0, depth_height = 0;
    bool ready = false, external = false, sample_ready = false;
};

struct PortVitaGxmDevice {
    PortVitaGpuMemory rings[4]{{nullptr, 0, -1, 0, 0}, {nullptr, 0, -1, 0, 0},
                             {nullptr, 0, -1, 0, 0}, {nullptr, 0, -1, 0, 0}};
    void* host = nullptr;
    SceGxmContext* context = nullptr;
    SceGxmShaderPatcher* patcher = nullptr;
    PatcherAllocation* allocations = nullptr;
    OwnedProgram* programs = nullptr;
    PortVitaGxmSurface* display[display_count]{};
    PortVitaGxmSurface* current = nullptr;
    unsigned children = 0, front = 0, back = 1;
    int ready_display = -1;
    bool initialized = false, ready = false, closing = false, heartbeat = false;
    std::atomic<int> error{0};
    std::atomic<bool> scanout{false};
};

namespace {
struct DisplayData { PortVitaGxmDevice* owner; void* pixels; };
bool usable(const PortVitaGxmDevice* device) {
    return device && device->ready && !device->closing && device->error.load() == 0;
}
void remember_error(PortVitaGxmDevice* device, int error) {
    if (error < 0) {
        int expected = 0;
        device->error.compare_exchange_strong(expected, error);
    }
}
void display_callback(const void* data) {
    const auto& frame = *static_cast<const DisplayData*>(data);
    SceDisplayFrameBuf buffer{};
    buffer.size = sizeof(buffer); buffer.base = frame.pixels; buffer.pitch = 1024;
    buffer.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    buffer.width = 960; buffer.height = 544;
    const int result = sceDisplaySetFrameBuf(&buffer, SCE_DISPLAY_SETBUF_NEXTFRAME);
    if (result >= 0) {
        frame.owner->scanout.store(true);
        remember_error(frame.owner, sceDisplayWaitVblankStart());
    } else remember_error(frame.owner, result);
}
void* host_alloc(void*, SceSize size) { return std::malloc(size); }
void host_free(void*, void* memory) { std::free(memory); }
void* patcher_alloc(void* user, SceSize size, uint32_t mapping, unsigned* offset) {
    auto* device = static_cast<PortVitaGxmDevice*>(user);
    auto* node = new (std::nothrow) PatcherAllocation;
    if (!node) return nullptr;
    const int result = port_vita_gpu_memory_alloc("GalaxyPatcher", size, mapping, &node->memory);
    if (result < 0) {
        if (node->memory.uid < 0) { delete node; return nullptr; }
        // A clean allocation failure is retryable after cache eviction. Only
        // failed cleanup/remaining ownership poisons this device's operations.
        remember_error(device, result);
    }
    node->next = device->allocations; device->allocations = node;
    if (result < 0) return nullptr;
    if (offset) *offset = node->memory.usse_offset;
    return node->memory.base;
}
void patcher_free(void* user, void* memory) {
    auto* device = static_cast<PortVitaGxmDevice*>(user);
    auto** link = &device->allocations;
    while (*link && (*link)->memory.base != memory) link = &(*link)->next;
    if (!*link) { remember_error(device, invalid); return; }
    auto* node = *link;
    const int result = port_vita_gpu_memory_free(&node->memory);
    if (result < 0) { remember_error(device, result); return; }
    *link = node->next; delete node;
}
void* buffer_alloc(void* user, SceSize size) { return patcher_alloc(user, size, PORT_VITA_GPU_RW, nullptr); }
void* vertex_alloc(void* user, SceSize size, unsigned* offset) { return patcher_alloc(user, size, PORT_VITA_GPU_VERTEX_USSE, offset); }
void* fragment_alloc(void* user, SceSize size, unsigned* offset) { return patcher_alloc(user, size, PORT_VITA_GPU_FRAGMENT_USSE, offset); }

int destroy_surface(PortVitaGxmSurface** pointer) {
    auto* surface = *pointer;
    if (!surface) return 0;
    surface->ready = false;
    int result;
    if (surface->target) {
        result = sceGxmDestroyRenderTarget(surface->target); if (result < 0) return result;
        surface->target = nullptr;
    }
    if (surface->sync) {
        result = sceGxmSyncObjectDestroy(surface->sync); if (result < 0) return result;
        surface->sync = nullptr;
    }
    for (auto* block : {&surface->depth, &surface->color}) {
        if (block->uid >= 0) { result = port_vita_gpu_memory_free(block); if (result < 0) return result; }
    }
    if (surface->external) --surface->owner->children;
    delete surface; *pointer = nullptr; return 0;
}
int create_surface(PortVitaGxmDevice* device, uint32_t width, uint32_t height,
                   uint32_t stride, bool external, PortVitaGxmSurface** out) {
    if (!out || *out || !width || !height || width > 4096 || height > 4096) return invalid;
    auto* surface = new (std::nothrow) PortVitaGxmSurface;
    if (!surface) return no_memory;
    *out = surface; surface->owner = device; surface->external = external;
    if (external) ++device->children;
    surface->width = width; surface->height = height; surface->stride = stride;
    const uint32_t depth_stride = (width + SCE_GXM_TILE_SIZEX - 1) & ~(SCE_GXM_TILE_SIZEX - 1);
    const uint32_t depth_height = (height + SCE_GXM_TILE_SIZEY - 1) & ~(SCE_GXM_TILE_SIZEY - 1);
    surface->depth_stride=depth_stride; surface->depth_height=depth_height;
    int result = port_vita_gpu_memory_alloc_cdram("GalaxyColor", stride * height * 4, PORT_VITA_GPU_RW, &surface->color);
    if (result >= 0) result = port_vita_gpu_memory_alloc_cdram("GalaxyDepth", depth_stride * depth_height * 4, PORT_VITA_GPU_RW, &surface->depth);
    if (result >= 0) {
        std::memset(surface->color.base, 0, surface->color.size);
        std::memset(surface->depth.base, 0, surface->depth.size);
        result = sceGxmColorSurfaceInit(&surface->color_surface, SCE_GXM_COLOR_FORMAT_A8B8G8R8,
            SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
            width, height, stride, surface->color.base);
    }
    if (result >= 0) result = sceGxmDepthStencilSurfaceInit(&surface->depth_surface, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
        SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, depth_stride, surface->depth.base, nullptr);
    if (result >= 0) sceGxmDepthStencilSurfaceSetForceStoreMode(&surface->depth_surface, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
    if (result >= 0) {
        SceGxmRenderTargetParams target{};
        target.width = width; target.height = height; target.scenesPerFrame = 1;
        target.multisampleMode = SCE_GXM_MULTISAMPLE_NONE; target.driverMemBlock = -1;
        result = sceGxmCreateRenderTarget(&target, &surface->target);
    }
    if (result >= 0) result = sceGxmSyncObjectCreate(&surface->sync);
    if (result < 0) {
        const int cleanup = destroy_surface(out); return cleanup < 0 ? cleanup : result;
    }
    surface->ready = true; return 0;
}
int begin(PortVitaGxmDevice* device, PortVitaGxmSurface* surface) {
    if (!usable(device) || device->current || !surface || surface->owner != device || !surface->ready) return invalid;
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_BEGIN);
    const int result = sceGxmBeginScene(device->context, 0, surface->target, nullptr, nullptr,
                                       surface->sync, &surface->color_surface, &surface->depth_surface);
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_BEGIN_RETURN);
    if (result >= 0) device->current = surface;
    return result;
}
}

extern "C" int port_vita_gxm_device_create(PortVitaGxmDevice** out) {
    if (!out || *out) return invalid;
    bool expected = false;
    if (!global_owner.compare_exchange_strong(expected, true)) return invalid;
    auto* device = new (std::nothrow) PortVitaGxmDevice;
    if (!device) { global_owner.store(false); return no_memory; }
    *out = device;
    SceGxmInitializeParams initialize{};
    initialize.flags = SCE_GXM_INITIALIZE_FLAG_DEFAULT;
    initialize.displayQueueMaxPendingCount = display_count - 1;
    initialize.displayQueueCallback = display_callback;
    initialize.displayQueueCallbackDataSize = sizeof(DisplayData);
    initialize.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    int result = sceGxmInitialize(&initialize);
    if (result >= 0) {
        device->initialized = true;
        const unsigned sizes[4] = {SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE,
                                  SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE};
        for (unsigned i = 0; i < 4 && result >= 0; ++i)
            result = port_vita_gpu_memory_alloc("GalaxyRing", sizes[i], i == 3 ? PORT_VITA_GPU_FRAGMENT_USSE : PORT_VITA_GPU_READ,
                                                 &device->rings[i]);
    }
    if (result >= 0) {
        device->host = std::malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
        if (!device->host) result = no_memory;
    }
    if (result >= 0) {
        SceGxmContextParams context{};
        context.hostMem = device->host; context.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
        context.vdmRingBufferMem = device->rings[0].base; context.vdmRingBufferMemSize = device->rings[0].size;
        context.vertexRingBufferMem = device->rings[1].base; context.vertexRingBufferMemSize = device->rings[1].size;
        context.fragmentRingBufferMem = device->rings[2].base; context.fragmentRingBufferMemSize = device->rings[2].size;
        context.fragmentUsseRingBufferMem = device->rings[3].base; context.fragmentUsseRingBufferMemSize = device->rings[3].size;
        context.fragmentUsseRingBufferOffset = device->rings[3].usse_offset;
        result = sceGxmCreateContext(&context, &device->context);
    }
    if (result >= 0) {
        SceGxmShaderPatcherParams patcher{};
        patcher.userData = device; patcher.hostAllocCallback = host_alloc; patcher.hostFreeCallback = host_free;
        patcher.bufferAllocCallback = buffer_alloc; patcher.bufferFreeCallback = patcher_free;
        patcher.vertexUsseAllocCallback = vertex_alloc; patcher.vertexUsseFreeCallback = patcher_free;
        patcher.fragmentUsseAllocCallback = fragment_alloc; patcher.fragmentUsseFreeCallback = patcher_free;
        result = sceGxmShaderPatcherCreate(&patcher, &device->patcher);
        if (result >= 0 && device->error.load() < 0) result = device->error.load();
    }
    for (unsigned i = 0; i < display_count && result >= 0; ++i)
        result = create_surface(device, 960, 544, 1024, false, &device->display[i]);
    if (result < 0) {
        const int cleanup = port_vita_gxm_device_release(out); return cleanup < 0 ? cleanup : result;
    }
    device->ready = true; return 0;
}

extern "C" int port_vita_gxm_device_release(PortVitaGxmDevice** pointer) {
    if (!pointer || !*pointer) return invalid;
    auto* device = *pointer;
    if (device->current || device->children) return invalid;
    device->closing = true;
    if (device->context) {
        PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_FINISH);
        sceGxmFinish(device->context);
        PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_FINISH_RETURN);
    }
    int result;
    if (device->initialized) {
        PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_DRAIN);
        result = sceGxmDisplayQueueFinish();
        PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_DRAIN_RETURN);
        if (result < 0) return result;
    }
    if (device->scanout.load()) {
        result = sceDisplaySetFrameBuf(nullptr, SCE_DISPLAY_SETBUF_NEXTFRAME); if (result < 0) return result;
        result = sceDisplayWaitSetFrameBuf(); if (result < 0) return result;
        device->scanout.store(false);
    }
    if (device->patcher) {
        result = sceGxmShaderPatcherDestroy(device->patcher); if (result < 0) return result;
        device->patcher = nullptr;
    }
    // SDK free callbacks cannot return errors. Keep failed frees linked until
    // patcher destruction succeeds, then retry each retained owned allocation.
    while (device->allocations) {
        auto* node = device->allocations;
        result = port_vita_gpu_memory_free(&node->memory); if (result < 0) return result;
        device->allocations = node->next; delete node;
    }
    for (auto*& surface : device->display) {
        result = destroy_surface(&surface); if (result < 0) return result;
    }
    if (device->context) {
        result = sceGxmDestroyContext(device->context); if (result < 0) return result;
        device->context = nullptr;
    }
    std::free(device->host); device->host = nullptr;
    for (auto& block : device->rings) {
        if (block.uid >= 0) { result = port_vita_gpu_memory_free(&block); if (result < 0) return result; }
    }
    if (device->initialized) {
        result = sceGxmTerminate(); if (result < 0) return result;
        device->initialized = false;
    }
    delete device; *pointer = nullptr; global_owner.store(false); return 0;
}
extern "C" void* port_vita_gxm_device_context(PortVitaGxmDevice* device) { return usable(device) ? device->context : nullptr; }
extern "C" void* port_vita_gxm_device_patcher(PortVitaGxmDevice* device) { return usable(device) ? device->patcher : nullptr; }
extern "C" int port_vita_gxm_surface_create(PortVitaGxmDevice* device, uint32_t width, uint32_t height, PortVitaGxmSurface** out) {
    if (!usable(device) || device->current) return invalid;
    return create_surface(device, width, height, (width + 63) & ~63u, true, out);
}
extern "C" int port_vita_gxm_surface_info(const PortVitaGxmSurface* surface, PortVitaGxmSurfaceInfo* out) {
    if (!surface || !surface->ready || !out) return invalid;
    *out = {surface->width, surface->height, surface->stride, &surface->color_surface, &surface->depth_surface,
            surface->color.base, surface->depth.base}; return 0;
}
extern "C" int port_vita_gxm_surface_sample(PortVitaGxmDevice* device,PortVitaGxmSurface* surface,
                                            PortVitaGxmSurfaceSampleInfo* out) {
    if (!out) return invalid;
    *out={};
    if (!usable(device) || device->current || !surface || surface->owner!=device || !surface->ready) return invalid;
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_FINISH);
    sceGxmFinish(device->context);
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_FINISH_RETURN);
    if (!surface->sample_ready) {
        // Publish both aliases atomically; a failed setter never leaves a
        // partially configured borrowed descriptor in the surface.
        SceGxmTexture color{},depth{};
        int result=sceGxmTextureInitLinear(&color,surface->color.base,SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
                                           surface->stride,surface->height,1);
        if (result>=0) result=sceGxmTextureInitTiled(&depth,surface->depth.base,SCE_GXM_TEXTURE_FORMAT_X8U24_SD,
                                                   surface->depth_stride,surface->depth_height,1);
        for (auto* texture:{&color,&depth}) {
            if (result>=0) result=sceGxmTextureSetUAddrMode(texture,SCE_GXM_TEXTURE_ADDR_CLAMP);
            if (result>=0) result=sceGxmTextureSetVAddrMode(texture,SCE_GXM_TEXTURE_ADDR_CLAMP);
            if (result>=0) result=sceGxmTextureSetMinFilter(texture,SCE_GXM_TEXTURE_FILTER_POINT);
            if (result>=0) result=sceGxmTextureSetMagFilter(texture,SCE_GXM_TEXTURE_FILTER_POINT);
            if (result>=0) result=sceGxmTextureSetMipFilter(texture,SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
            if (result>=0) result=sceGxmTextureSetLodBias(texture,31);
            if (result>=0) result=sceGxmTextureValidate(texture);
        }
        if (result<0) return result;
        surface->color_texture=color; surface->depth_texture=depth; surface->sample_ready=true;
    }
    *out={surface->width,surface->height,surface->stride,surface->height,surface->depth_stride,surface->depth_height,
          &surface->color_texture,&surface->depth_texture}; return 0;
}
static int release_surface(PortVitaGxmSurface** pointer,const void* manager) {
    if (!pointer || !*pointer || !(*pointer)->external) return invalid;
    if ((*pointer)->manager!=manager) return invalid;
    auto* device = (*pointer)->owner;
    if (device->current) return invalid;
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_FINISH);
    sceGxmFinish(device->context);
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_FINISH_RETURN);
    // Display syncs/scanout are unrelated to this external offscreen surface.
    return destroy_surface(pointer);
}
extern "C" int port_vita_gxm_surface_release(PortVitaGxmSurface** pointer) {
    return release_surface(pointer,nullptr);
}
int port_vita_gxm_surface_create_owned(PortVitaGxmDevice* device,uint32_t width,uint32_t height,
                                       const void* manager,PortVitaGxmSurface** out) {
    if (!manager || !out || *out) return invalid;
    const int result=port_vita_gxm_surface_create(device,width,height,out);
    if (out && *out) (*out)->manager=manager;
    return result;
}
int port_vita_gxm_surface_release_owned(PortVitaGxmSurface** pointer,const void* manager) {
    return manager ? release_surface(pointer,manager) : invalid;
}
extern "C" int port_vita_gxm_device_begin(PortVitaGxmDevice* device, PortVitaGxmSurface* surface) {
    if (!surface || !surface->external) return invalid;
    return begin(device, surface);
}
extern "C" int port_vita_gxm_device_begin_display(PortVitaGxmDevice* device) {
    if (!usable(device) || device->ready_display >= 0) return invalid;
    return begin(device, device->display[device->back]);
}
extern "C" int port_vita_gxm_device_end(PortVitaGxmDevice* device, PortVitaGxmProgram* const* programs, uint32_t count) {
    if (!device || device->closing || !device->current) return invalid;
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_END);
    const int result = port_vita_gxm_scene_end(device->context, programs, count, nullptr, nullptr);
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_END_RETURN);
    if (result >= 0) {
        sceGxmDepthStencilSurfaceSetForceLoadMode(&device->current->depth_surface, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
        if (device->current == device->display[device->back]) device->ready_display = device->back;
        device->current = nullptr;
    }
    return result;
}
extern "C" int port_vita_gxm_device_present(PortVitaGxmDevice* device) {
    if (!usable(device) || device->current || device->ready_display < 0) return invalid;
    auto* surface = device->display[device->ready_display];
    int result = 0;
    if (!device->heartbeat) {
        PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_HEARTBEAT);
        result = sceGxmPadHeartbeat(&surface->color_surface, surface->sync);
        PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_HEARTBEAT_RETURN);
        if (result < 0) return result;
        device->heartbeat = true;
    }
    const DisplayData frame{device, surface->color.base};
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_QUEUE);
    result = sceGxmDisplayQueueAddEntry(device->display[device->front]->sync, surface->sync, &frame);
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_QUEUE_RETURN);
    if (result < 0) return result;
    device->front = device->ready_display; device->back = (device->front + 1) % display_count;
    device->ready_display = -1; device->heartbeat = false; return 0;
}
extern "C" int port_vita_gxm_device_finish(PortVitaGxmDevice* device) {
    if (!usable(device) || device->current) return invalid;
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_FINISH);
    sceGxmFinish(device->context);
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_FINISH_RETURN);
    return 0;
}
extern "C" int port_vita_gxm_device_drain_display(PortVitaGxmDevice* device) {
    if (!device || !device->initialized || device->current || device->closing) return invalid;
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_DRAIN);
    const int result = sceGxmDisplayQueueFinish();
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_DRAIN_RETURN);
    return result < 0 ? result : device->error.load();
}
extern "C" int port_vita_gxm_device_error(const PortVitaGxmDevice* device) { return device ? device->error.load() : invalid; }

int port_vita_gxm_device_hold_resource(PortVitaGxmDevice* device) {
    if (!usable(device) || device->current || device->children == UINT32_MAX) return invalid;
    ++device->children; return 0;
}
int port_vita_gxm_device_retire_resource(PortVitaGxmDevice* device) {
    if (!device || !device->context || device->current || device->closing) return invalid;
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_FINISH);
    sceGxmFinish(device->context);
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_FINISH_RETURN);
    return 0;
}
int port_vita_gxm_device_retire_display(PortVitaGxmDevice* device) {
    if (!device || !device->initialized || device->current || device->closing) return invalid;
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_DRAIN);
    const int result=sceGxmDisplayQueueFinish();
    PORT_GAME_GPU_PROGRESS(PORT_GAME_GPU_DRAIN_RETURN);
    return result;
}
void port_vita_gxm_device_drop_resource(PortVitaGxmDevice* device) { --device->children; }
bool port_vita_gxm_device_scene_active(const PortVitaGxmDevice* device) { return device && device->current; }
bool port_vita_gxm_device_owns_program(const PortVitaGxmDevice* device, const PortVitaGxmProgram* program) {
    if (!device || !program) return false;
    for (auto* node=device->programs; node; node=node->next)
        if (node->program==program) return true;
    return false;
}

static int create_device_program(PortVitaGxmDevice* device, const void* manager,
    const void* vertex_gxp, uint32_t vertex_bytes, const void* fragment_gxp, uint32_t fragment_bytes,
    const PortVitaGxmBlend* blend, uint32_t pixel_vectors, uint32_t texture_mask,
    PortVitaGxmProgram** out, PortVitaGxmProgramInfo* info) {
    if (!out || *out || !info) return invalid;
    const int held = port_vita_gxm_device_hold_resource(device); if (held < 0) return held;
    auto* node = new (std::nothrow) OwnedProgram;
    if (!node) { port_vita_gxm_device_drop_resource(device); return no_memory; }
    node->manager=manager;
    const int result = port_vita_gxm_program_create(device->patcher, vertex_gxp, vertex_bytes,
        fragment_gxp, fragment_bytes, blend, pixel_vectors, texture_mask, out, info);
    if (*out) {
        port_vita_gxm_program_adopt(*out,device);
        node->program = *out; node->next = device->programs; device->programs = node;
        if (result>=0) { node->info=*info; node->ready=true; }
    } else { delete node; port_vita_gxm_device_drop_resource(device); }
    return result;
}
extern "C" int port_vita_gxm_device_program_create(PortVitaGxmDevice* device,
    const void* vertex_gxp, uint32_t vertex_bytes, const void* fragment_gxp, uint32_t fragment_bytes,
    const PortVitaGxmBlend* blend, uint32_t pixel_vectors, uint32_t texture_mask,
    PortVitaGxmProgram** out, PortVitaGxmProgramInfo* info) {
    return create_device_program(device,nullptr,vertex_gxp,vertex_bytes,fragment_gxp,fragment_bytes,
                                 blend,pixel_vectors,texture_mask,out,info);
}
int port_vita_gxm_device_program_create_owned(PortVitaGxmDevice* device, const void* owner,
    const void* vertex_gxp, uint32_t vertex_bytes, const void* fragment_gxp, uint32_t fragment_bytes,
    const PortVitaGxmBlend* blend, uint32_t pixel_vectors, uint32_t texture_mask,
    PortVitaGxmProgram** out, PortVitaGxmProgramInfo* info) {
    return owner ? create_device_program(device,owner,vertex_gxp,vertex_bytes,fragment_gxp,fragment_bytes,
                                         blend,pixel_vectors,texture_mask,out,info) : invalid;
}
extern "C" int port_vita_gxm_device_program_info(const PortVitaGxmDevice* device,
    const PortVitaGxmProgram* program, PortVitaGxmProgramInfo* out) {
    if (!out) return invalid;
    *out={};
    if (!usable(device) || !program) return invalid;
    for (auto* node=device->programs; node; node=node->next) {
        if (node->program==program) {
            if (!node->ready) return invalid;
            *out=node->info; return 0;
        }
    }
    return invalid;
}
static int release_device_program(PortVitaGxmDevice* device, PortVitaGxmProgram** pointer, const void* manager) {
    if (!device || !pointer || !*pointer) return invalid;
    auto** link = &device->programs;
    while (*link && (*link)->program != *pointer) link = &(*link)->next;
    if (!*link || (*link)->manager!=manager) return invalid;
    const int retired = port_vita_gxm_device_retire_resource(device); if (retired < 0) return retired;
    const int result = port_vita_gxm_program_release_owned(pointer,device); if (result < 0) return result;
    auto* node = *link; *link = node->next; delete node;
    port_vita_gxm_device_drop_resource(device); return 0;
}
extern "C" int port_vita_gxm_device_program_release(PortVitaGxmDevice* device, PortVitaGxmProgram** pointer) {
    return release_device_program(device,pointer,nullptr);
}
int port_vita_gxm_device_program_release_owned(PortVitaGxmDevice* device, PortVitaGxmProgram** pointer, const void* owner) {
    return owner ? release_device_program(device,pointer,owner) : invalid;
}
