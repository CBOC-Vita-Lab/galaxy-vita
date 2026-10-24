#include "gpu_program.h"
#include "gpu_program_internal.h"
#include <psp2/gxm.h>
#include <cstdlib>
#include <cstring>
#include <new>

struct PortVitaGxmProgram {
    PortVitaGxmDevice* owner = nullptr;
    SceGxmShaderPatcher* patcher = nullptr;
    SceGxmContext* context = nullptr;
    SceGxmProgram *vertex_gxp = nullptr, *fragment_gxp = nullptr;
    SceGxmShaderPatcherId vertex_id = nullptr, fragment_id = nullptr;
    SceGxmVertexProgram* vertex = nullptr;
    SceGxmFragmentProgram* fragment = nullptr;
    const SceGxmProgramParameter *pixel = nullptr, *raster = nullptr;
    PortVitaGxmProgramInfo info{};
    uint32_t declared_pixel_floats = 0;
    bool ready = false, scene_pending = false;
};

namespace {
constexpr int invalid = static_cast<int>(SCE_GXM_ERROR_INVALID_VALUE);
constexpr int no_memory = static_cast<int>(SCE_GXM_ERROR_OUT_OF_MEMORY);
constexpr uint32_t stride = sizeof(PortVitaGxmVertex);
static_assert(stride == 152);
constexpr uint32_t default_container = 14;
static_assert(SCE_GXM_BLEND_FUNC_NONE==0 && SCE_GXM_BLEND_FUNC_ADD==1);
static_assert(SCE_GXM_BLEND_FACTOR_ZERO==0 && SCE_GXM_BLEND_FACTOR_ONE==1 &&
    SCE_GXM_BLEND_FACTOR_SRC_ALPHA==4 && SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA==5);

bool complete_gxp(const SceGxmProgram* program, uint32_t bytes) {
    const uint32_t logical=sceGxmProgramGetSize(program);
    if (logical<8 || logical>bytes || (bytes!=logical && bytes!=((logical+3)&~3u))) return false;
    const auto* data=reinterpret_cast<const unsigned char*>(program);
    for (uint32_t i=logical;i<bytes;++i) if (data[i]) return false;
    return true;
}

bool blend_valid(const PortVitaGxmBlend* b) {
    return b && b->color_mask <= 15 && b->color_func <= SCE_GXM_BLEND_FUNC_MAX &&
        b->alpha_func <= SCE_GXM_BLEND_FUNC_MAX && b->color_src <= SCE_GXM_BLEND_FACTOR_DST_ALPHA_SATURATE &&
        b->color_dst <= SCE_GXM_BLEND_FACTOR_DST_ALPHA_SATURATE &&
        b->alpha_src <= SCE_GXM_BLEND_FACTOR_DST_ALPHA_SATURATE && b->alpha_dst <= SCE_GXM_BLEND_FACTOR_DST_ALPHA_SATURATE;
}

bool attribute(const char* name, uint32_t& bit, uint32_t& offset, uint32_t& components) {
    if (!name) return false;
    if (!strcmp(name,"a_clip")) { bit=0; offset=0; components=4; }
    else if (!strcmp(name,"a_clr0")) { bit=1; offset=16; components=4; }
    else if (!strcmp(name,"a_clr1")) { bit=2; offset=32; components=4; }
    else if (!strcmp(name,"a_depth")) { bit=11; offset=144; components=2; }
    else if (!strncmp(name,"a_tex",5) && name[5]>='0' && name[5]<='7' && !name[6]) {
        bit=3+uint32_t(name[5]-'0'); offset=48+(bit-3)*12; components=3;
    } else return false;
    return true;
}

bool reflected_vertex(PortVitaGxmProgram& p, SceGxmVertexAttribute (&attributes)[12], unsigned& count) {
    count=0;
    const unsigned parameters=sceGxmProgramGetParameterCount(p.vertex_gxp);
    if (parameters>12) return false;
    for (unsigned i=0;i<parameters;++i) {
        const auto* parameter=sceGxmProgramGetParameter(p.vertex_gxp,i);
        if (!parameter || sceGxmProgramParameterGetCategory(parameter)!=SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) return false;
        uint32_t bit, offset, components;
        const unsigned register_components=sceGxmProgramParameterGetComponentCount(parameter);
        if (!attribute(sceGxmProgramParameterGetName(parameter),bit,offset,components) ||
            p.info.attribute_mask & (1u<<bit) ||
            sceGxmProgramParameterGetType(parameter)!=SCE_GXM_PARAMETER_TYPE_F32 ||
            register_components<components || register_components>4 ||
            sceGxmProgramParameterGetArraySize(parameter)!=1) return false;
        const auto resource=sceGxmProgramParameterGetResourceIndex(parameter);
        if (resource>UINT16_MAX) return false;
        // Reflected F32x4 register capacity is not the vertex fetch width.
        // Depth fetches two floats at byte 144; fetching four would overrun.
        attributes[count++]={0,static_cast<uint16_t>(offset),SCE_GXM_ATTRIBUTE_FORMAT_F32,
                             static_cast<uint8_t>(components),static_cast<uint16_t>(resource)};
        p.info.attribute_mask|=1u<<bit;
    }
    p.info.vertex_uniform_bytes=sceGxmProgramGetDefaultUniformBufferSize(p.vertex_gxp);
    return (p.info.attribute_mask&1)!=0;
}

bool reflected_fragment(PortVitaGxmProgram& p, uint32_t vectors, uint32_t textures) {
    const unsigned parameters=sceGxmProgramGetParameterCount(p.fragment_gxp);
    if (parameters>10) return false;
    for (unsigned i=0;i<parameters;++i) {
        const auto* parameter=sceGxmProgramGetParameter(p.fragment_gxp,i);
        if (!parameter) return false;
        const char* name=sceGxmProgramParameterGetName(parameter);
        if (!name) return false;
        const auto category=sceGxmProgramParameterGetCategory(parameter);
        if (category==SCE_GXM_PARAMETER_CATEGORY_UNIFORM && !strcmp(name,"u_pixel")) {
            if (p.pixel || sceGxmProgramParameterGetType(parameter)!=SCE_GXM_PARAMETER_TYPE_F32 ||
                sceGxmProgramParameterGetComponentCount(parameter)!=4 ||
                sceGxmProgramParameterGetContainerIndex(parameter)!=default_container) return false;
            const unsigned array_size=sceGxmProgramParameterGetArraySize(parameter);
            if (!array_size || array_size>vectors) return false;
            p.pixel=parameter; p.info.pixel_floats=array_size*4;
        } else if (category==SCE_GXM_PARAMETER_CATEGORY_UNIFORM && !strcmp(name,"u_raster")) {
            if (p.raster || sceGxmProgramParameterGetType(parameter)!=SCE_GXM_PARAMETER_TYPE_F32 ||
                sceGxmProgramParameterGetComponentCount(parameter)!=4 ||
                sceGxmProgramParameterGetArraySize(parameter)!=2 ||
                sceGxmProgramParameterGetContainerIndex(parameter)!=default_container) return false;
            p.raster=parameter; p.info.raster_floats=8;
        } else if (category==SCE_GXM_PARAMETER_CATEGORY_SAMPLER && !strncmp(name,"s_tex",5) &&
                   name[5]>='0' && name[5]<='7' && !name[6]) {
            const unsigned unit=unsigned(name[5]-'0');
            if (!(textures&(1u<<unit)) || p.info.texture_mask&(1u<<unit) ||
                sceGxmProgramParameterGetResourceIndex(parameter)!=unit ||
                sceGxmProgramParameterIsSamplerCube(parameter)) return false;
            p.info.texture_mask|=1u<<unit;
        } else return false;
    }
    p.info.fragment_uniform_bytes=sceGxmProgramGetDefaultUniformBufferSize(p.fragment_gxp);
    if (p.info.fragment_uniform_bytes<(p.info.pixel_floats+p.info.raster_floats)*sizeof(float)) return false;
    p.declared_pixel_floats=vectors*4;
    return true;
}

bool mapped_range(const PortVitaGpuMemory* memory, uint32_t offset, uint64_t bytes) {
    return memory && memory->base && memory->uid>=0 &&
        (memory->mapping==PORT_VITA_GPU_READ || memory->mapping==PORT_VITA_GPU_RW) &&
        (reinterpret_cast<uintptr_t>(memory->base)+offset)%4==0 && uint64_t(offset)+bytes<=memory->size;
}
}

static int release_program(PortVitaGxmProgram** handle, PortVitaGxmDevice* owner) {
    if (!handle || !*handle) return invalid;
    auto& p=**handle;
    if (p.owner!=owner || p.scene_pending) return invalid;
    p.ready=false;
    if (p.context) { sceGxmFinish(p.context); p.context=nullptr; }
    int result;
    if (p.fragment) {
        result=sceGxmShaderPatcherReleaseFragmentProgram(p.patcher,p.fragment);
        if (result<0) return result;
        p.fragment=nullptr;
    }
    if (p.vertex) {
        result=sceGxmShaderPatcherReleaseVertexProgram(p.patcher,p.vertex);
        if (result<0) return result;
        p.vertex=nullptr;
    }
    if (p.fragment_id) {
        result=sceGxmShaderPatcherUnregisterProgram(p.patcher,p.fragment_id);
        if (result<0) return result;
        p.fragment_id=nullptr;
    }
    if (p.vertex_id) {
        result=sceGxmShaderPatcherUnregisterProgram(p.patcher,p.vertex_id);
        if (result<0) return result;
        p.vertex_id=nullptr;
    }
    free(p.fragment_gxp); free(p.vertex_gxp); delete *handle; *handle=nullptr;
    return 0;
}

extern "C" int port_vita_gxm_program_release(PortVitaGxmProgram** handle) {
    return release_program(handle,nullptr);
}
void port_vita_gxm_program_adopt(PortVitaGxmProgram* p, PortVitaGxmDevice* owner) {
    p->owner=owner;
}
int port_vita_gxm_program_release_owned(PortVitaGxmProgram** handle, PortVitaGxmDevice* owner) {
    return owner ? release_program(handle,owner) : invalid;
}

extern "C" int port_vita_gxm_scene_end(void* context, PortVitaGxmProgram* const* programs,
                                       uint32_t count, const void* vertex_notification,
                                       const void* fragment_notification) {
    if (!context || (count && !programs)) return invalid;
    for (uint32_t i=0;i<count;++i)
        if (!programs[i] || programs[i]->context!=context) return invalid;
    const int result=sceGxmEndScene(static_cast<SceGxmContext*>(context),
        static_cast<const SceGxmNotification*>(vertex_notification),
        static_cast<const SceGxmNotification*>(fragment_notification));
    if (result<0) return result;
    for (uint32_t i=0;i<count;++i) programs[i]->scene_pending=false;
    return 0;
}

extern "C" int port_vita_gxm_program_create(void* patcher, const void* vertex_gxp, uint32_t vertex_bytes,
                                            const void* fragment_gxp, uint32_t fragment_bytes,
                                            const PortVitaGxmBlend* blend, uint32_t pixel_vectors,
                                            uint32_t texture_mask, PortVitaGxmProgram** out,
                                            PortVitaGxmProgramInfo* info) {
    if (!out || *out || !info) return invalid;
    *info={};
    if (!patcher || !vertex_gxp || !fragment_gxp || vertex_bytes<8 || fragment_bytes<8 ||
        vertex_bytes>4*1024*1024 || fragment_bytes>4*1024*1024 || !blend_valid(blend) ||
        !pixel_vectors || pixel_vectors>27 || texture_mask>255) return invalid;
    auto* p=new(std::nothrow) PortVitaGxmProgram;
    if (!p) return no_memory;
    *out=p; p->patcher=static_cast<SceGxmShaderPatcher*>(patcher);
    p->vertex_gxp=static_cast<SceGxmProgram*>(malloc(vertex_bytes));
    p->fragment_gxp=static_cast<SceGxmProgram*>(malloc(fragment_bytes));
    int result=0;
    if (!p->vertex_gxp || !p->fragment_gxp) result=no_memory;
    else {
        memcpy(p->vertex_gxp,vertex_gxp,vertex_bytes); memcpy(p->fragment_gxp,fragment_gxp,fragment_bytes);
        if (sceGxmProgramCheck(p->vertex_gxp)<0 || sceGxmProgramCheck(p->fragment_gxp)<0 ||
            !complete_gxp(p->vertex_gxp,vertex_bytes) || !complete_gxp(p->fragment_gxp,fragment_bytes) ||
            sceGxmProgramGetType(p->vertex_gxp)!=SCE_GXM_VERTEX_PROGRAM ||
            sceGxmProgramGetType(p->fragment_gxp)!=SCE_GXM_FRAGMENT_PROGRAM) result=invalid;
    }
    SceGxmVertexAttribute attributes[12]{}; unsigned attribute_count=0;
    if (!result && (!reflected_vertex(*p,attributes,attribute_count) ||
                   !reflected_fragment(*p,pixel_vectors,texture_mask))) result=invalid;
    if (!result) result=sceGxmShaderPatcherRegisterProgram(p->patcher,p->vertex_gxp,&p->vertex_id);
    if (!result) result=sceGxmShaderPatcherRegisterProgram(p->patcher,p->fragment_gxp,&p->fragment_id);
    const SceGxmVertexStream stream{stride,SCE_GXM_INDEX_SOURCE_INDEX_16BIT};
    if (!result) result=sceGxmShaderPatcherCreateVertexProgram(p->patcher,p->vertex_id,attributes,attribute_count,&stream,1,&p->vertex);
    SceGxmBlendInfo native_blend{};
    native_blend.colorMask=blend->color_mask; native_blend.colorFunc=blend->color_func;
    native_blend.alphaFunc=blend->alpha_func; native_blend.colorSrc=blend->color_src; native_blend.colorDst=blend->color_dst;
    native_blend.alphaSrc=blend->alpha_src; native_blend.alphaDst=blend->alpha_dst;
    if (!result) result=sceGxmShaderPatcherCreateFragmentProgram(p->patcher,p->fragment_id,
        SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,SCE_GXM_MULTISAMPLE_NONE,&native_blend,p->vertex_gxp,&p->fragment);
    if (result<0) {
        const int cleanup=port_vita_gxm_program_release(out);
        return cleanup<0 ? cleanup : result;
    }
    p->ready=true; *info=p->info;
    return 0;
}

static int draw_program(PortVitaGxmProgram* p, void* context,
                                          const PortVitaGpuMemory* vertices, uint32_t vertex_offset,
                                          const PortVitaGpuMemory* indices, uint32_t index_offset,
                                          uint32_t vertex_count, uint32_t index_count, uint32_t topology,
                                          const float* pixel, uint32_t pixel_floats,
                                          const void* const textures[8], const float* raster) {
    if (!p || !p->ready || !context || topology>PORT_VITA_GXM_TRIANGLES ||
        (p->context && p->context!=context) || (p->raster && !raster)) return invalid;
    const uint32_t arity=topology==PORT_VITA_GXM_TRIANGLES ? 3 : topology==PORT_VITA_GXM_LINES ? 2 : 1;
    if (index_count%arity || vertex_count>64000) return invalid;
    if (!index_count) return 0;
    if (!vertex_count || !mapped_range(vertices,vertex_offset,uint64_t(vertex_count)*stride) ||
        !mapped_range(indices,index_offset,uint64_t(index_count)*sizeof(uint16_t)) ||
        !pixel || pixel_floats!=p->declared_pixel_floats || (p->info.texture_mask && !textures)) return invalid;
    for (unsigned unit=0;unit<8;++unit)
        if (p->info.texture_mask&(1u<<unit) && !textures[unit]) return invalid;
    const auto* data=static_cast<const unsigned char*>(indices->base)+index_offset;
    for (uint32_t i=0;i<index_count;++i) {
        uint16_t index; memcpy(&index,data+size_t(i)*2,2);
        if (index>=vertex_count || index>=64000) return invalid;
    }
    auto* native_context=static_cast<SceGxmContext*>(context);
    // Reserve through GXM's fenced context rings. Never guess offsets or memcpy
    // packed uniforms over reflection-dependent padding/constant allocations.
    p->context=native_context;
    p->scene_pending=true;
    sceGxmSetVertexProgram(native_context,p->vertex); sceGxmSetFragmentProgram(native_context,p->fragment);
    void* buffer=nullptr; int result;
    if (p->info.vertex_uniform_bytes) {
        result=sceGxmReserveVertexDefaultUniformBuffer(native_context,&buffer);
        if (result<0) return result;
        if (!buffer) return invalid;
        memset(buffer,0,p->info.vertex_uniform_bytes);
    }
    if (p->info.fragment_uniform_bytes) {
        result=sceGxmReserveFragmentDefaultUniformBuffer(native_context,&buffer);
        if (result<0) return result;
        if (!buffer) return invalid;
        memset(buffer,0,p->info.fragment_uniform_bytes);
        if (p->pixel) {
            result=sceGxmSetUniformDataF(buffer,p->pixel,0,p->info.pixel_floats,pixel);
            if (result<0) return result;
        }
        if (p->raster) {
            result=sceGxmSetUniformDataF(buffer,p->raster,0,p->info.raster_floats,raster);
            if (result<0) return result;
        }
    }
    for (unsigned unit=0;unit<8;++unit) if (p->info.texture_mask&(1u<<unit)) {
        result=sceGxmSetFragmentTexture(native_context,unit,static_cast<const SceGxmTexture*>(textures[unit]));
        if (result<0) return result;
    }
    result=sceGxmSetVertexStream(native_context,0,static_cast<const unsigned char*>(vertices->base)+vertex_offset);
    if (result<0) return result;
    const auto primitive=topology==PORT_VITA_GXM_TRIANGLES ? SCE_GXM_PRIMITIVE_TRIANGLES :
                         topology==PORT_VITA_GXM_LINES ? SCE_GXM_PRIMITIVE_LINES : SCE_GXM_PRIMITIVE_POINTS;
    return sceGxmDraw(native_context,primitive,SCE_GXM_INDEX_FORMAT_U16,data,index_count);
}

extern "C" int port_vita_gxm_program_draw(PortVitaGxmProgram* p, void* context,
    const PortVitaGpuMemory* vertices, uint32_t vertex_offset,
    const PortVitaGpuMemory* indices, uint32_t index_offset,
    uint32_t vertex_count, uint32_t index_count, uint32_t topology,
    const float* pixel, uint32_t pixel_floats, const void* const textures[8]) {
    return draw_program(p,context,vertices,vertex_offset,indices,index_offset,
                        vertex_count,index_count,topology,pixel,pixel_floats,textures,nullptr);
}

extern "C" int port_vita_gxm_program_draw_raster(PortVitaGxmProgram* p, void* context,
    const PortVitaGpuMemory* vertices, uint32_t vertex_offset,
    const PortVitaGpuMemory* indices, uint32_t index_offset,
    uint32_t vertex_count, uint32_t index_count, uint32_t topology,
    const float* pixel, uint32_t pixel_floats, const void* const textures[8], const float raster[8]) {
    return draw_program(p,context,vertices,vertex_offset,indices,index_offset,
                        vertex_count,index_count,topology,pixel,pixel_floats,textures,raster);
}

bool port_vita_gxm_program_bound(PortVitaGxmProgram* p, void* context) {
    return p && p->scene_pending && p->context==context;
}
