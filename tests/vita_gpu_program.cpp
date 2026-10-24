// SDK boundary double checks the real reflected submission/ownership code.
#include "gpu_program.h"
#include "cpu_xf.h"
#include "gpu_raster.h"
#include <psp2/gxm.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#pragma weak port_vita_gxm_program_draw_raster
#pragma weak port_vita_gxm_raster_apply

static_assert(sizeof(PortVitaGxmVertex) == sizeof(gpu::ClipVertex));
static_assert(offsetof(PortVitaGxmVertex, color) == offsetof(gpu::ClipVertex, color));
static_assert(offsetof(PortVitaGxmVertex, tex) == offsetof(gpu::ClipVertex, tex));
static_assert(offsetof(PortVitaGxmVertex, view_z) == offsetof(gpu::ClipVertex, viewZ));
static_assert(offsetof(PortVitaGxmVertex, gx_z) == offsetof(gpu::ClipVertex, gxZ));
struct SceGxmProgram { unsigned size, type; unsigned char tail[4]{}; };
struct SceGxmProgramParameter { const char* name; unsigned category, components, count, resource; };
static const SceGxmProgramParameter attributes[] = {
    // Actual Sony Cg reflection rounds float2/float3 inputs to F32x4 registers.
    {"a_clip",0,4,1,3}, {"a_clr1",0,4,1,9}, {"a_tex7",0,4,1,17}, {"a_depth",0,4,1,21}};
static const SceGxmProgramParameter uniform = {"u_pixel",1,4,25,11}, sampler = {"s_tex7",2,1,1,7};
static const SceGxmProgramParameter raster_uniform = {"u_raster",1,4,2,128};
static const SceGxmProgramParameter* pixel_parameter = &uniform;
static const SceGxmProgramParameter* raster_parameter;
static int sampler_resource = 7, fragment_error = 0, unregister_error = 0, scene_error = 0;
static unsigned checks = 0, draws = 0, register_count = 0;
static std::vector<int> events;
static const void* source_vertex = nullptr;
static const void* source_fragment = nullptr;
static const void* stream_pointer = nullptr;
static float copied_uniforms[100];
static float copied_raster[8];
static unsigned raster_sets;
static float viewport[6];
static SceGxmCullMode cull_mode;
static SceGxmRegionClipMode region_mode;
static SceGxmDepthFunc front_depth, back_depth;
static SceGxmDepthWriteMode front_write, back_write;
static unsigned raster_calls;
static float uniform_buffer[160];
static void require(bool valid, const char* message) {
    ++checks; if (!valid) { fprintf(stderr, "GXM program FAILED: %s\n", message); exit(1); }
}
static SceGxmVertexProgram* vp = reinterpret_cast<SceGxmVertexProgram*>(0x4000);
static SceGxmFragmentProgram* fp = reinterpret_cast<SceGxmFragmentProgram*>(0x5000);
static auto patcher = reinterpret_cast<SceGxmShaderPatcher*>(0x6000);
static auto context = reinterpret_cast<SceGxmContext*>(0x7000);
extern "C" int sceGxmProgramCheck(const SceGxmProgram* p) { return p->size >= 8 && p->size <= sizeof(*p) ? 0 : -1; }
extern "C" unsigned sceGxmProgramGetSize(const SceGxmProgram* p) { return p->size; }
extern "C" SceGxmProgramType sceGxmProgramGetType(const SceGxmProgram* p) { return static_cast<SceGxmProgramType>(p->type); }
extern "C" unsigned sceGxmProgramGetParameterCount(const SceGxmProgram* p) { return p->type == 0 ? 4 : raster_parameter ? 3 : 2; }
extern "C" const SceGxmProgramParameter* sceGxmProgramGetParameter(const SceGxmProgram* p, unsigned i) {
    if (p->type == 0) return &attributes[i];
    return i == 0 ? pixel_parameter : i == 1 ? &sampler : raster_parameter;
}
extern "C" const SceGxmProgramParameter* sceGxmProgramFindParameterByName(const SceGxmProgram* p, const char* name) {
    for (unsigned i=0;i<sceGxmProgramGetParameterCount(p);++i) {
        const auto* parameter=sceGxmProgramGetParameter(p,i);
        if (parameter && strcmp(name,parameter->name)==0) return parameter;
    }
    return nullptr;
}
extern "C" const char* sceGxmProgramParameterGetName(const SceGxmProgramParameter* p) { return p->name; }
extern "C" SceGxmParameterCategory sceGxmProgramParameterGetCategory(const SceGxmProgramParameter* p) { return static_cast<SceGxmParameterCategory>(p->category); }
extern "C" SceGxmParameterType sceGxmProgramParameterGetType(const SceGxmProgramParameter*) { return SCE_GXM_PARAMETER_TYPE_F32; }
extern "C" unsigned sceGxmProgramParameterGetComponentCount(const SceGxmProgramParameter* p) { return p->components; }
extern "C" unsigned sceGxmProgramParameterGetArraySize(const SceGxmProgramParameter* p) { return p->count; }
extern "C" unsigned sceGxmProgramParameterGetResourceIndex(const SceGxmProgramParameter* p) { return p==&sampler ? unsigned(sampler_resource) : p->resource; }
extern "C" unsigned sceGxmProgramParameterGetContainerIndex(const SceGxmProgramParameter*) { return 14; }
extern "C" SceBool sceGxmProgramParameterIsSamplerCube(const SceGxmProgramParameter*) { return SCE_FALSE; }
extern "C" unsigned sceGxmProgramGetDefaultUniformBufferSize(const SceGxmProgram* p) { return p->type==0 ? 0 : 640; }
extern "C" int sceGxmShaderPatcherRegisterProgram(SceGxmShaderPatcher* owner, const SceGxmProgram* p, SceGxmShaderPatcherId* id) {
    require(owner==patcher && p!=source_vertex && p!=source_fragment, "GXP copy owned independently of caller");
    *id=reinterpret_cast<SceGxmShaderPatcherId>(const_cast<SceGxmProgram*>(p)); ++register_count; events.push_back(1); return 0;
}
extern "C" int sceGxmShaderPatcherUnregisterProgram(SceGxmShaderPatcher*, SceGxmShaderPatcherId) {
    events.push_back(4); return unregister_error;
}
extern "C" int sceGxmShaderPatcherCreateVertexProgram(SceGxmShaderPatcher*, SceGxmShaderPatcherId,
      const SceGxmVertexAttribute* a, unsigned n, const SceGxmVertexStream* s, unsigned ns, SceGxmVertexProgram** out) {
    require(n==4 && ns==1 && s->stride==152 && s->indexSource==SCE_GXM_INDEX_SOURCE_INDEX_16BIT, "reflected live attribute layout, no frame-wide stride");
    const unsigned offsets[]={0,32,132,144}, registers[]={3,9,17,21}, components[]={4,4,3,2};
    for (unsigned i=0;i<n;++i) require(a[i].offset==offsets[i] && a[i].regIndex==registers[i] &&
       a[i].componentCount==components[i] && a[i].format==SCE_GXM_ATTRIBUTE_FORMAT_F32, "SDK register indices and complete F32 payload");
    *out=vp; return 0;
}
extern "C" int sceGxmShaderPatcherCreateFragmentProgram(SceGxmShaderPatcher*, SceGxmShaderPatcherId,
      SceGxmOutputRegisterFormat format, SceGxmMultisampleMode, const SceGxmBlendInfo* blend,
      const SceGxmProgram*, SceGxmFragmentProgram** out) {
    require(format==SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED && blend->colorMask==15 &&
       blend->colorSrc==4 && blend->alphaSrc==1, "full output declaration and separate alpha/color blend preserved");
    if (fragment_error) return fragment_error; *out=fp; return 0;
}
extern "C" int sceGxmShaderPatcherReleaseFragmentProgram(SceGxmShaderPatcher*, SceGxmFragmentProgram*) { events.push_back(2); return 0; }
extern "C" int sceGxmShaderPatcherReleaseVertexProgram(SceGxmShaderPatcher*, SceGxmVertexProgram*) { events.push_back(3); return 0; }
extern "C" void sceGxmFinish(SceGxmContext* c) { require(c==context,"finish exact last submission context"); events.push_back(5); }
extern "C" int sceGxmEndScene(SceGxmContext* c, const SceGxmNotification* vertex, const SceGxmNotification* fragment) {
    require(c==context && !vertex && !fragment,"end exact scene with caller notifications");
    events.push_back(6); return scene_error;
}
extern "C" void sceGxmSetVertexProgram(SceGxmContext*, const SceGxmVertexProgram* p) { require(p==vp,"bind exact vertex program"); }
extern "C" void sceGxmSetFragmentProgram(SceGxmContext*, const SceGxmFragmentProgram* p) { require(p==fp,"bind exact fragment program"); }
extern "C" int sceGxmReserveFragmentDefaultUniformBuffer(SceGxmContext*, void** out) { *out=uniform_buffer; return 0; }
extern "C" int sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext*, void**) { require(false,"no dead vertex uniforms allocated"); return -1; }
extern "C" int sceGxmSetUniformDataF(void* dest, const SceGxmProgramParameter* p, unsigned offset, unsigned count, const float* data) {
    if (p==&raster_uniform) {
        require(dest==uniform_buffer && offset==0 && count==8,"independent reflected raster setter");
        memcpy(copied_raster,data,sizeof(copied_raster)); ++raster_sets; return 0;
    }
    require(dest==uniform_buffer && p==&uniform && offset==0 && count==100, "use reflected setter rather than packed-buffer memcpy");
    memcpy(copied_uniforms,data,sizeof(copied_uniforms)); return 0;
}
extern "C" int sceGxmSetVertexStream(SceGxmContext*, unsigned stream, const void* data) { require(stream==0,"single mapped vertex stream"); stream_pointer=data; return 0; }
extern "C" int sceGxmSetFragmentTexture(SceGxmContext*, unsigned unit, const SceGxmTexture*) { require(unit==7,"reflected explicit sampler7 unit, not default0"); return 0; }
extern "C" int sceGxmDraw(SceGxmContext*, SceGxmPrimitiveType primitive, SceGxmIndexFormat format, const void*, unsigned count) {
    require(primitive==SCE_GXM_PRIMITIVE_TRIANGLES && format==SCE_GXM_INDEX_FORMAT_U16 && count==3,"whole triangles submitted as local U16"); ++draws; return 0;
}
extern "C" void sceGxmSetViewport(SceGxmContext* c,float xo,float xs,float yo,float ys,float zo,float zs) {
    require(c==context,"native raster context");
    const float values[]={xo,xs,yo,ys,zo,zs}; memcpy(viewport,values,sizeof(viewport)); ++raster_calls;
}
extern "C" void sceGxmSetCullMode(SceGxmContext*,SceGxmCullMode mode) { cull_mode=mode; }
extern "C" void sceGxmSetRegionClip(SceGxmContext*,SceGxmRegionClipMode mode,unsigned,unsigned,unsigned,unsigned) { region_mode=mode; }
extern "C" void sceGxmSetFrontDepthFunc(SceGxmContext*,SceGxmDepthFunc mode) { front_depth=mode; }
extern "C" void sceGxmSetBackDepthFunc(SceGxmContext*,SceGxmDepthFunc mode) { back_depth=mode; }
extern "C" void sceGxmSetFrontDepthWriteEnable(SceGxmContext*,SceGxmDepthWriteMode mode) { front_write=mode; }
extern "C" void sceGxmSetBackDepthWriteEnable(SceGxmContext*,SceGxmDepthWriteMode mode) { back_write=mode; }

int main() {
    SceGxmProgram vertex{sizeof(SceGxmProgram),0}, fragment{sizeof(SceGxmProgram),1};
    source_vertex=&vertex; source_fragment=&fragment;
    PortVitaGxmBlend blend{15,1,1,4,5,1,0};
    PortVitaGxmProgram* program=nullptr; PortVitaGxmProgramInfo info{};
    auto create=[&] { return port_vita_gxm_program_create(patcher,&vertex,sizeof(vertex),&fragment,sizeof(fragment),
                                                        &blend,25,128,&program,&info); };
    require(create()==0 && program,"create reflected GXM program from complete blobs");
    require(info.attribute_mask==(1u|4u|1u<<10|1u<<11) && info.texture_mask==128 &&
            info.pixel_floats==100 && info.fragment_uniform_bytes==640,"reflection metadata including over16vec4/defaultbuffer padding");
    float pixels[100]; for (unsigned i=0;i<100;++i) pixels[i]=float(i);
    alignas(16) PortVitaGxmVertex vertices[3]{};
    alignas(16) uint16_t indices[]={0,1,2};
    PortVitaGpuMemory v{vertices,sizeof(vertices),42,1,0}, ix{indices,sizeof(indices),43,1,0};
    const void* textures[8]{}; textures[7]=reinterpret_cast<const void*>(0x8000);
    auto draw=[&] { return port_vita_gxm_program_draw(program,context,&v,0,&ix,0,3,3,2,pixels,100,textures); };
    require(draw()==0 && draws==1 && stream_pointer==vertices,"mapped draw transport");
    require(memcmp(copied_uniforms,pixels,sizeof(pixels))==0,"all100pixel components delivered");
    indices[2]=64000; require(draw()<0 && draws==1,"reject documented GXM U16 overflow before submission"); indices[2]=2;
    textures[7]=nullptr; require(draw()<0 && draws==1,"no missing-texture placeholder draw"); textures[7]=reinterpret_cast<const void*>(0x8000);
    v.mapping=0; require(draw()<0 && draws==1,"reject unmapped CPU vertices"); v.mapping=1;
    v.size=1; require(draw()<0 && draws==1,"reject buffer overrun"); v.size=sizeof(vertices);
    events.clear();
    require(port_vita_gxm_program_release(&program)<0 && program && events.empty(),
            "do not finish/free a program while its last scene remains unsubmitted");
    scene_error=-106;
    require(port_vita_gxm_scene_end(context,&program,1,nullptr,nullptr)==-106,
            "actual SDK scene submission failure reported");
    require(port_vita_gxm_program_release(&program)<0 && program && events==std::vector<int>({6}),
            "failed EndScene keeps program resources alive without calling Finish");
    scene_error=0; events.clear();
    require(port_vita_gxm_scene_end(context,&program,1,nullptr,nullptr)==0,
            "successful SDK scene submission permits fenced release");
    unregister_error=-101;
    require(port_vita_gxm_program_release(&program)==-101 && program,"failed unregister keeps owned blobs retryable");
    require(events==std::vector<int>({6,5,2,3,4}),"EndScene then finish GPU before releases and unregister");
    unregister_error=0; events.clear(); require(port_vita_gxm_program_release(&program)==0 && !program,"unregister retry clears ownership");
    require(events==std::vector<int>({4,4}),"retry does not repeat released programs or GPU finish");
    fragment_error=-102; events.clear(); require(create()==-102 && !program,"fragment-create rollback with no leaked program"); fragment_error=0;
    require(events==std::vector<int>({1,1,3,4,4}),"rollback releases created vertex and both registrations");
    sampler_resource=0; const auto previous=register_count;
    require(create()<0 && !program && register_count==previous,"reject sampler resource mismatch before registration"); sampler_resource=7;
    require(port_vita_gxm_program_release(&program)<0,"empty release rejected");
    raster_parameter=&raster_uniform;
    require(create()==0 && program && info.raster_floats==8,"reflect exact pixel scissor and alpha override separately");
    const unsigned before_raster_draws=draws;
    require(draw()<0 && draws==before_raster_draws,"legacy draw cannot erase raster contract");
    require(&port_vita_gxm_program_draw_raster!=nullptr,"raster-aware draw exists");
    const float raster_values[]={3,5,635,475,1,0.625f,0,0};
    auto draw_raster=[&](const float* values) { return port_vita_gxm_program_draw_raster(program,context,&v,0,&ix,0,3,3,2,pixels,100,textures,values); };
    require(draw_raster(nullptr)<0 && draws==before_raster_draws,"missing raster fails before native bind");
    require(draw_raster(raster_values)==0 && draws==before_raster_draws+1 && raster_sets==1 &&
            memcmp(copied_raster,raster_values,sizeof(copied_raster))==0 &&
            memcmp(copied_uniforms,pixels,sizeof(pixels))==0,"pixel and raster uniforms reach their own reflected parameters");
    require(port_vita_gxm_scene_end(context,&program,1,nullptr,nullptr)==0 &&
            port_vita_gxm_program_release(&program)==0,"raster scene retirement");
    auto bad_raster=raster_uniform; bad_raster.count=3; raster_parameter=&bad_raster;
    require(create()<0 && !program,"reject unexpected raster array extent");
    bad_raster=raster_uniform; bad_raster.components=3;
    require(create()<0 && !program,"reject reduced-width raster vectors");
    bad_raster=raster_uniform; bad_raster.name="u_other";
    require(create()<0 && !program,"reject unknown additional uniform");
    raster_parameter=nullptr;
    // Earlier accepted raster0 GXP has logical 1401 bytes plus three zero file
    // alignment bytes. The SDK size is the logical extent, not padded length.
    for (unsigned logical:{9u,10u,11u,12u}) {
        vertex.size=fragment.size=logical;
        require(create()==0 && program,"accept complete compiler blob with up to three zero alignment bytes");
        require(port_vita_gxm_program_release(&program)==0,"padded compiler blob release");
    }
    vertex.size=fragment.size=9; fragment.tail[3]=1;
    require(create()<0 && !program,"reject nonzero data after logical GXP extent"); fragment.tail[3]=0;
    vertex.size=fragment.size=8;
    require(create()<0 && !program,"reject arbitrary trailing bytes beyond native four-byte alignment");
    vertex.size=fragment.size=sizeof(vertex);
    require(&port_vita_gxm_raster_apply!=nullptr,"native raster state bridge exists");
    PortVitaGxmRaster raster{{10,20,620,460},{3,5,635,475},640,480,1,1,3,2,0,0,0,0};
    float raster_out[8];
    require(port_vita_gxm_raster_apply(context,&raster,2,raster_out)==0,"native viewport/depth/cull and exact scissor");
    const float want_viewport[]={320,310,250,-230,0.5f,0.5f};
    const float want_uniforms[]={3,5,635,475,0,0,0,0};
    require(memcmp(viewport,want_viewport,sizeof(viewport))==0 &&
            memcmp(raster_out,want_uniforms,sizeof(raster_out))==0,"literal top-left viewport with one Z remap and exclusive scissor");
    require(front_depth==SCE_GXM_DEPTH_FUNC_LESS_EQUAL && back_depth==front_depth &&
            front_write==SCE_GXM_DEPTH_WRITE_ENABLED && back_write==front_write &&
            cull_mode==SCE_GXM_CULL_CW && region_mode==SCE_GXM_REGION_CLIP_NONE,
            "CCW-front/back culling and both-face depth, not tile-rounded scissor");
    raster.depth_test=0; raster.cull=1; raster.alpha_override=1; raster.alpha=0.625f;
    require(port_vita_gxm_raster_apply(context,&raster,2,raster_out)==0 && front_depth==SCE_GXM_DEPTH_FUNC_ALWAYS &&
            front_write==SCE_GXM_DEPTH_WRITE_DISABLED && cull_mode==SCE_GXM_CULL_CCW &&
            raster_out[4]==1 && raster_out[5]==0.625f,"depth disable also inhibits writes, explicit final alpha override");
    raster.rgba6=1; raster.dither=1;
    require(port_vita_gxm_raster_apply(context,&raster,2,raster_out)==0 && raster_out[6]==1 && raster_out[7]==1,
            "RGBA6 storage and Bayer flags occupy the existing raster vectors without changing alpha/scissor");
    const unsigned before_invalid_storage=raster_calls;
    raster.rgba6=0;
    require(port_vita_gxm_raster_apply(context,&raster,2,raster_out)<0 && raster_calls==before_invalid_storage,
            "dither without RGBA6 rejects before mutating native raster state");
    raster.rgba6=2;
    require(port_vita_gxm_raster_apply(context,&raster,2,raster_out)<0 && raster_calls==before_invalid_storage,
            "malformed RGBA6 selector rejects before native mutation");
    raster.rgba6=1; raster.dither=2;
    require(port_vita_gxm_raster_apply(context,&raster,2,raster_out)<0 && raster_calls==before_invalid_storage,
            "malformed dither selector rejects before native mutation");
    raster.rgba6=raster.dither=0;
    raster.scissor[0]=-10; raster.scissor[1]=481; raster.scissor[2]=650; raster.scissor[3]=499; raster.cull=3;
    require(port_vita_gxm_raster_apply(context,&raster,2,raster_out)==0 && raster_out[0]==0 && raster_out[1]==480 &&
            raster_out[2]==640 && raster_out[3]==480 && region_mode==SCE_GXM_REGION_CLIP_ALL,"clamped empty bounds and both polygon faces culled");
    require(port_vita_gxm_raster_apply(context,&raster,1,raster_out)==0 && region_mode==SCE_GXM_REGION_CLIP_NONE &&
            cull_mode==SCE_GXM_CULL_NONE,"polygon culling must not remove line primitives");
    const unsigned before_invalid_raster=raster_calls;
    raster.viewport[0]=std::numeric_limits<float>::infinity();
    require(port_vita_gxm_raster_apply(context,&raster,2,raster_out)<0 && raster_calls==before_invalid_raster,
            "invalid raster cannot partially mutate GPU state");
    printf("GXM programs: %u reflected submission/lifetime checks; SDK mock, not GPU/device proof\n",checks);
}
