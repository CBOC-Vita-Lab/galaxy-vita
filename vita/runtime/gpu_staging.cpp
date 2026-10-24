#include "gpu_staging.h"
#include "gpu_device_internal.h"
#include "gpu_program_internal.h"
#include <psp2/gxm.h>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
constexpr int invalid=static_cast<int>(SCE_GXM_ERROR_INVALID_VALUE);
constexpr int no_memory=static_cast<int>(SCE_GXM_ERROR_OUT_OF_MEMORY);
struct Slab {
    PortVitaGpuMemory memory{nullptr,0,-1,0,0};
    uint32_t used=0;
    bool failed=false;
    Slab* next=nullptr;
};
}
struct PortVitaGxmStaging {
    PortVitaGxmDevice* device=nullptr;
    void* context=nullptr;
    Slab* slabs=nullptr;
    PortVitaGxmProgram* inline_programs[16]{};
    PortVitaGxmProgram** programs=inline_programs;
    uint32_t capacity=16, count=0, slab_bytes=0;
    uint64_t epoch=1;
    bool open=false, closing=false;
};

extern "C" int port_vita_gxm_staging_create(PortVitaGxmDevice* device, uint32_t bytes, PortVitaGxmStaging** out) {
    if (!out || *out || bytes>UINT32_MAX-4095) return invalid;
    const int held=port_vita_gxm_device_hold_resource(device);
    if (held<0) return held;
    auto* s=new(std::nothrow) PortVitaGxmStaging;
    if (!s) { port_vita_gxm_device_drop_resource(device); return no_memory; }
    s->device=device; s->context=port_vita_gxm_device_context(device);
    s->slab_bytes=bytes ? (bytes+4095)&~4095u : 2*1024*1024;
    *out=s; return 0;
}
extern "C" int port_vita_gxm_staging_upload(PortVitaGxmStaging* s,
    const PortVitaGxmVertex* vertices, uint32_t vertex_count,
    const uint16_t* indices, uint32_t index_count, PortVitaGxmSlice* out) {
    if (!out) return invalid;
    *out={};
    if (!s || s->closing || s->open || port_vita_gxm_device_scene_active(s->device) ||
        !port_vita_gxm_device_context(s->device) || !vertices || !indices ||
        !vertex_count || vertex_count>64000 || !index_count) return invalid;
    const uint64_t vertex_bytes=uint64_t(vertex_count)*sizeof(PortVitaGxmVertex);
    const uint64_t bytes=(vertex_bytes+uint64_t(index_count)*2+3)&~uint64_t(3);
    if (bytes>UINT32_MAX-4095) return invalid;
    for (uint32_t i=0;i<index_count;++i) if (indices[i]>=vertex_count) return invalid;
    auto* slab=s->slabs;
    while (slab && (slab->failed || bytes>slab->memory.size-slab->used)) slab=slab->next;
    if (!slab) {
        slab=new(std::nothrow) Slab;
        if (!slab) return no_memory;
        const uint32_t extent=bytes>s->slab_bytes ? uint32_t(bytes) : s->slab_bytes;
        const int result=port_vita_gpu_memory_alloc("galaxy-draws",extent,PORT_VITA_GPU_READ,&slab->memory);
        if (result<0) {
            if (slab->memory.uid>=0) { slab->failed=true; slab->next=s->slabs; s->slabs=slab; }
            else delete slab;
            return result;
        }
        slab->next=s->slabs; s->slabs=slab;
    }
    const uint32_t vertex_offset=slab->used, index_offset=vertex_offset+uint32_t(vertex_bytes);
    auto* data=static_cast<unsigned char*>(slab->memory.base);
    std::memcpy(data+vertex_offset,vertices,size_t(vertex_bytes));
    std::memcpy(data+index_offset,indices,size_t(index_count)*2);
    slab->used+=uint32_t(bytes);
    *out={s->epoch,&slab->memory,vertex_offset,index_offset,vertex_count,index_count}; return 0;
}
extern "C" int port_vita_gxm_staging_slice(PortVitaGxmStaging* s,const PortVitaGxmSlice* parent,
                                           uint32_t first,uint32_t count,PortVitaGxmSlice* out) {
    if (!out || out==parent) return invalid;
    *out={};
    if (!s || s->closing || s->open || port_vita_gxm_device_scene_active(s->device) ||
        !port_vita_gxm_device_context(s->device) || !parent || parent->epoch!=s->epoch ||
        !parent->vertex_count || parent->vertex_count>64000 || !count || first>parent->index_count ||
        count>parent->index_count-first || parent->vertex_offset%4 || parent->index_offset%4) return invalid;
    auto* slab=s->slabs;
    while (slab && &slab->memory!=parent->block) slab=slab->next;
    const uint64_t at=uint64_t(parent->index_offset)+uint64_t(first)*2;
    const uint64_t vertexEnd=uint64_t(parent->vertex_offset)+uint64_t(parent->vertex_count)*sizeof(PortVitaGxmVertex);
    if (!slab || slab->failed || vertexEnd>parent->index_offset || vertexEnd>slab->used ||
        uint64_t(parent->index_offset)+uint64_t(parent->index_count)*2>slab->used || at%4 || at>UINT32_MAX) return invalid;
    *out=*parent; out->index_offset=uint32_t(at); out->index_count=count; return 0;
}
extern "C" int port_vita_gxm_staging_begin(PortVitaGxmStaging* s, PortVitaGxmSurface* surface) {
    if (!s || s->closing || s->open) return invalid;
    const int result=surface ? port_vita_gxm_device_begin(s->device,surface) : port_vita_gxm_device_begin_display(s->device);
    if (result>=0) { s->open=true; s->count=0; }
    return result;
}
extern "C" int port_vita_gxm_staging_draw(PortVitaGxmStaging* s, PortVitaGxmProgram* program,
    const PortVitaGxmSlice* slice, uint32_t topology, const float* pixel,
    uint32_t pixel_floats, const void* const textures[8], const float raster[8]) {
    if (!s || !s->open || s->closing || !slice || slice->epoch!=s->epoch ||
        !port_vita_gxm_device_owns_program(s->device,program) || !port_vita_gxm_device_context(s->device)) return invalid;
    auto* slab=s->slabs;
    while (slab && &slab->memory!=slice->block) slab=slab->next;
    if (!slab || slab->failed || !slice->vertex_count || !slice->index_count ||
        slice->vertex_offset%4 || slice->index_offset%4 ||
        uint64_t(slice->vertex_offset)+uint64_t(slice->vertex_count)*sizeof(PortVitaGxmVertex)>slab->used ||
        uint64_t(slice->index_offset)+uint64_t(slice->index_count)*2>slab->used) return invalid;
    bool known=false;
    for (uint32_t i=0;i<s->count;++i) if (s->programs[i]==program) known=true;
    // Reserve list capacity before binding so OOM cannot lose the scene's
    // acknowledgment of a program that failed after it became GPU-visible.
    if (!known && s->count==s->capacity) {
        if (s->capacity>UINT32_MAX/2 || uint64_t(s->capacity)*2*sizeof(program)>SIZE_MAX) return no_memory;
        auto** expanded=static_cast<PortVitaGxmProgram**>(std::malloc(size_t(s->capacity)*2*sizeof(program)));
        if (!expanded) return no_memory;
        std::memcpy(expanded,s->programs,size_t(s->count)*sizeof(program));
        if (s->programs!=s->inline_programs) std::free(s->programs);
        s->programs=expanded; s->capacity*=2;
    }
    const int result=port_vita_gxm_program_draw_raster(program,s->context,&slab->memory,slice->vertex_offset,
        &slab->memory,slice->index_offset,slice->vertex_count,slice->index_count,topology,pixel,pixel_floats,textures,raster);
    if (!known && port_vita_gxm_program_bound(program,s->context)) s->programs[s->count++]=program;
    return result;
}
extern "C" int port_vita_gxm_staging_end(PortVitaGxmStaging* s) {
    if (!s || !s->open || s->closing) return invalid;
    const int result=port_vita_gxm_device_end(s->device,s->programs,s->count);
    if (result>=0) { s->open=false; s->count=0; }
    return result;
}
extern "C" int port_vita_gxm_staging_reset(PortVitaGxmStaging* s) {
    if (!s || s->open || s->closing || s->epoch==UINT64_MAX) return invalid;
    const int retired=port_vita_gxm_device_retire_resource(s->device);
    if (retired<0) return retired;
    auto** link=&s->slabs;
    while (*link) {
        auto* slab=*link;
        if (slab->failed) {
            const int result=port_vita_gpu_memory_free(&slab->memory);
            if (result<0) return result;
            *link=slab->next; delete slab;
        } else link=&slab->next;
    }
    for (auto* slab=s->slabs; slab; slab=slab->next) slab->used=0;
    ++s->epoch; return 0;
}
extern "C" int port_vita_gxm_staging_release(PortVitaGxmStaging** pointer) {
    if (!pointer || !*pointer || (*pointer)->open) return invalid;
    auto* s=*pointer;
    const int retired=port_vita_gxm_device_retire_resource(s->device);
    if (retired<0) return retired;
    s->closing=true;
    while (s->slabs) {
        auto* slab=s->slabs;
        const int result=port_vita_gpu_memory_free(&slab->memory);
        if (result<0) return result;
        s->slabs=slab->next; delete slab;
    }
    if (s->programs!=s->inline_programs) std::free(s->programs);
    port_vita_gxm_device_drop_resource(s->device);
    delete s; *pointer=nullptr; return 0;
}
