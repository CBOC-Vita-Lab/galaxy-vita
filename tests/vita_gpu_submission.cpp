// Reuse only the actual-SDK driver doubles. All program/device/staging code is
// real in this fixture, including ownership adoption and scene acknowledgment.
#define PORT_VITA_REAL_PROGRAM
#define main device_fixture_not_run
#include "vita_gpu_device.cpp"
#undef main

struct SceGxmProgram { uint32_t size,type; };
struct SceGxmProgramParameter { uint32_t category; };
static const SceGxmProgramParameter clip_parameter{0}, pixel_parameter{1}, raster_parameter{1}, texture_parameter{2};
static float default_uniforms[116];
static unsigned native_draws, native_bindings, uniform_sets;
static int uniform_error;
static bool reflect_raster=true;
static bool reflect_texture=false;
static bool reflect_manual=false;
[[maybe_unused]] static unsigned shader_registrations, shader_programs;
static unsigned shader_register_fail_call;
static int shader_release_error;
[[maybe_unused]] static PortVitaGxmBlend patched_blend{};
[[maybe_unused]] static SceGxmTexture sampled_texture{};
[[maybe_unused]] static unsigned texture_bindings;
[[maybe_unused]] static const SceGxmTexture* last_sampled_descriptor;
static const void* last_vertex_stream;
static const void* last_draw_indices;
static unsigned last_draw_count;
static SceGxmPrimitiveType last_draw_topology;
[[maybe_unused]] static void (*draw_result)();
static bool programTexture(const SceGxmProgram* p) { return p->type>=2 ? p->type==3 || p->type==5 : reflect_texture; }
extern "C" int sceGxmProgramCheck(const SceGxmProgram* p) { return p->size==sizeof(*p) ? 0 : -1; }
extern "C" unsigned sceGxmProgramGetSize(const SceGxmProgram* p) { return p->size; }
extern "C" SceGxmProgramType sceGxmProgramGetType(const SceGxmProgram* p) { return static_cast<SceGxmProgramType>(!!p->type); }
extern "C" unsigned sceGxmProgramGetParameterCount(const SceGxmProgram* p) {
    reflect_manual=p->type==5;
    if (p->type>=2) reflect_texture=programTexture(p);
    return p->type ? 1+unsigned(reflect_raster)+unsigned(programTexture(p)) : 1;
}
extern "C" const SceGxmProgramParameter* sceGxmProgramGetParameter(const SceGxmProgram* p,unsigned i) {
    return !p->type ? &clip_parameter : !i ? &pixel_parameter : i==1 && reflect_raster ? &raster_parameter : &texture_parameter;
}
extern "C" const char* sceGxmProgramParameterGetName(const SceGxmProgramParameter* p) {
    return p==&clip_parameter ? "a_clip" : p==&pixel_parameter ? "u_pixel" : p==&raster_parameter ? "u_raster" : "s_tex0";
}
extern "C" SceGxmParameterCategory sceGxmProgramParameterGetCategory(const SceGxmProgramParameter* p) { return static_cast<SceGxmParameterCategory>(p->category); }
extern "C" SceGxmParameterType sceGxmProgramParameterGetType(const SceGxmProgramParameter*) { return SCE_GXM_PARAMETER_TYPE_F32; }
extern "C" unsigned sceGxmProgramParameterGetComponentCount(const SceGxmProgramParameter*) { return 4; }
extern "C" unsigned sceGxmProgramParameterGetArraySize(const SceGxmProgramParameter* p) { return p==&clip_parameter ? 1 : p==&pixel_parameter ? (reflect_manual ? 6 : reflect_texture ? 5 : 4) : p==&raster_parameter ? 2 : 1; }
extern "C" unsigned sceGxmProgramParameterGetContainerIndex(const SceGxmProgramParameter*) { return 14; }
extern "C" unsigned sceGxmProgramParameterGetResourceIndex(const SceGxmProgramParameter* p) { return p==&raster_parameter ? (reflect_manual ? 24 : reflect_texture ? 20 : 16) : 0; }
extern "C" SceBool sceGxmProgramParameterIsSamplerCube(const SceGxmProgramParameter*) { return SCE_FALSE; }
extern "C" unsigned sceGxmProgramGetDefaultUniformBufferSize(const SceGxmProgram* p) { return p->type ? (p->type==5 ? 96 : programTexture(p) ? 80 : 64)+(reflect_raster ? 32 : 0) : 0; }
extern "C" int sceGxmShaderPatcherRegisterProgram(SceGxmShaderPatcher* value,const SceGxmProgram* p,SceGxmShaderPatcherId* out) {
    require(value==patcher,"real program registers with real device-owned patcher");
    if (++shader_registrations==shader_register_fail_call) return -891;
    if (p->type>=2) reflect_texture=programTexture(p);
    *out=reinterpret_cast<SceGxmShaderPatcherId>(const_cast<SceGxmProgram*>(p)); return 0;
}
extern "C" int sceGxmShaderPatcherUnregisterProgram(SceGxmShaderPatcher*,SceGxmShaderPatcherId) { return shader_release_error; }
extern "C" int sceGxmShaderPatcherCreateVertexProgram(SceGxmShaderPatcher*,SceGxmShaderPatcherId,
    const SceGxmVertexAttribute*,unsigned,const SceGxmVertexStream*,unsigned,SceGxmVertexProgram** out) {
    *out=reinterpret_cast<SceGxmVertexProgram*>(0x7700); return 0;
}
extern "C" int sceGxmShaderPatcherCreateFragmentProgram(SceGxmShaderPatcher*,SceGxmShaderPatcherId id,
    SceGxmOutputRegisterFormat,SceGxmMultisampleMode,const SceGxmBlendInfo* blend,const SceGxmProgram*,SceGxmFragmentProgram** out) {
    patched_blend={uint32_t(blend->colorMask),uint32_t(blend->colorFunc),uint32_t(blend->alphaFunc),
        uint32_t(blend->colorSrc),uint32_t(blend->colorDst),uint32_t(blend->alphaSrc),uint32_t(blend->alphaDst)};
    ++shader_programs;
    const auto* source=reinterpret_cast<const SceGxmProgram*>(id);
    *out=reinterpret_cast<SceGxmFragmentProgram*>(source->type>=2 ? reinterpret_cast<uintptr_t>(id) : 0x8800); return 0;
}
extern "C" int sceGxmShaderPatcherReleaseVertexProgram(SceGxmShaderPatcher*,SceGxmVertexProgram*) { return 0; }
extern "C" int sceGxmShaderPatcherReleaseFragmentProgram(SceGxmShaderPatcher*,SceGxmFragmentProgram*) { return 0; }
extern "C" void sceGxmSetVertexProgram(SceGxmContext*,const SceGxmVertexProgram*) { ++native_bindings; }
extern "C" void sceGxmSetFragmentProgram(SceGxmContext*,const SceGxmFragmentProgram* p) {
    reflect_manual=false;
    if (reinterpret_cast<uintptr_t>(p)!=0x8800) {
        const auto* source=reinterpret_cast<const SceGxmProgram*>(p);
        reflect_texture=programTexture(source); reflect_manual=source->type==5;
    }
}
extern "C" int sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext*,void**) { return -1; }
extern "C" int sceGxmReserveFragmentDefaultUniformBuffer(SceGxmContext*,void** out) { *out=default_uniforms; return 0; }
extern "C" int sceGxmSetUniformDataF(void* dest,const SceGxmProgramParameter* p,unsigned offset,unsigned count,const float* data) {
    require(dest==default_uniforms && !offset && count==(p==&pixel_parameter ? (reflect_manual ? 24u : reflect_texture ? 20u : 16u) : 8u),"complete independently reflected uniform payload");
    if (uniform_error) return uniform_error;
    std::memcpy(default_uniforms+(p==&pixel_parameter ? 0 : reflect_manual ? 24 : reflect_texture ? 20 : 16),data,count*sizeof(float)); ++uniform_sets; return 0;
}
extern "C" int sceGxmSetVertexStream(SceGxmContext*,unsigned,const void* data) { last_vertex_stream=data; return 0; }
extern "C" int sceGxmSetFragmentTexture(SceGxmContext*,unsigned,const SceGxmTexture* texture) {
    sampled_texture=*texture; last_sampled_descriptor=texture; ++texture_bindings; return 0;
}
extern "C" int sceGxmDraw(SceGxmContext*,SceGxmPrimitiveType topology,SceGxmIndexFormat,const void* indices,unsigned count) {
    last_draw_topology=topology; last_draw_indices=indices; last_draw_count=count; ++native_draws;
    if (draw_result) draw_result();
    return 0;
}

#ifndef PORT_VITA_SUBMISSION_DRIVER_ONLY
int main() {
    PortVitaGxmDevice* device=nullptr; PortVitaGxmSurface* surface=nullptr;
    PortVitaGxmProgram* program=nullptr; PortVitaGxmStaging* staging=nullptr;
    require(port_vita_gxm_device_create(&device)==0 && port_vita_gxm_surface_create(device,640,480,&surface)==0,
            "real native device/surface owners");
    const SceGxmProgram vs{8,0}, fs{8,1}; PortVitaGxmProgramInfo info{}; PortVitaGxmBlend blend{};
    require(port_vita_gxm_device_program_create(device,&vs,8,&fs,8,&blend,4,0,&program,&info)==0 && program,
            "real program is created/adopted by real device wrapper");
    auto* alias=program;
    require(port_vita_gxm_program_release(&program)<0 && program==alias,
            "raw release cannot orphan a managed device lease or free its registered pointer");
    require(port_vita_gxm_staging_create(device,4096,&staging)==0,"real staged owner");
    const PortVitaGxmVertex vertices[3]{}; const uint16_t indices[]={0,1,2};
    const float pixels[16]{}, raster[8]{0,0,640,480,0,0,0,0}; PortVitaGxmSlice slice{};
    require(port_vita_gxm_staging_upload(staging,vertices,3,indices,3,&slice)==0 &&
            port_vita_gxm_staging_begin(staging,surface)==0,"real mapped upload and native BeginScene");
    auto draw=[&](PortVitaGxmProgram* p) { return port_vita_gxm_staging_draw(staging,p,&slice,2,pixels,16,nullptr,raster); };
    require(draw(program)==0 && native_draws==1 && uniform_sets==2,"actual program submission through staged mapped slice");
    require(port_vita_gxm_device_program_release(device,&program)<0 && program,
            "managed release also cannot bypass open staged scene");
    end_error=-831;
    require(port_vita_gxm_staging_end(staging)==-831 && port_vita_gxm_program_release(&program)<0 && program,
            "failed EndScene cannot expose/free actual pending program"); end_error=0;
    require(port_vita_gxm_staging_end(staging)==0 && port_vita_gxm_device_program_release(device,&program)==0 && !program,
            "actual acknowledged managed program releases once after Finish");
    require(port_vita_gxm_staging_begin(staging,surface)==0 && draw(alias)<0 && native_draws==1 &&
            port_vita_gxm_staging_end(staging)==0,"stale program alias rejected by ownership before dereference");
    require(port_vita_gxm_staging_reset(staging)==0,"safe slice reuse fence");
    require(port_vita_gxm_device_program_create(device,&vs,8,&fs,8,&blend,4,0,&program,&info)==0 &&
            port_vita_gxm_staging_upload(staging,vertices,3,indices,3,&slice)==0 &&
            port_vita_gxm_staging_begin(staging,surface)==0,"actual failure-after-bind fixture");
    uniform_error=-832;
    require(draw(program)==-832 && native_bindings==2 && native_draws==1 &&
            port_vita_gxm_staging_end(staging)==0,"real failed uniform bind is included in scene acknowledgment"); uniform_error=0;
    require(port_vita_gxm_device_program_release(device,&program)==0 &&
            port_vita_gxm_staging_release(&staging)==0 && port_vita_gxm_surface_release(&surface)==0 &&
            port_vita_gxm_device_release(&device)==0,"no orphan lease or dangling program after full cross-unit cleanup");
    empty_state();
    printf("GXM submission: %u real cross-unit ownership/submission checks; SDK driver doubles, not GPU proof\n",checks);
}
#endif
