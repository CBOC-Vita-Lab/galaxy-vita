#include "renderer.h"
#include "../../platform/src/gx/vita_efb.h"
#include "../runtime/gpu_raster.h"
#include "../runtime/gpu_staging.h"
#include <algorithm>
#include <cmath>
#include <new>

struct VitaBringupRenderer {
    PortVitaGxmDevice* device=nullptr;
    PortVitaGxmStaging* staging=nullptr;
    PortVitaGxmProgram* program=nullptr;
    bool open=false,ready=false,failed=false;
};

int createVitaBringupRenderer(const gpu::VitaShaderLibrary& library,VitaBringupRenderer** out,std::string& error) {
    error.clear();
    if (!out || *out) { error="bringup output is not empty"; return -1; }
    auto* renderer=new(std::nothrow) VitaBringupRenderer;
    if (!renderer) { error="bringup CPU allocation failed"; return -1; }
    *out=renderer;
    int result=port_vita_gxm_device_create(&renderer->device);
    if (result<0) { error="bringup GXM initialization failed"; return result; }
    result=port_vita_gxm_staging_create(renderer->device,4096,&renderer->staging);
    if (result<0) { error="bringup staging creation failed"; return result; }
    gpu::ShaderUid uid{}; uid.numTevStages=1; uid.alphaFunc=63;
    const auto* vertex=gpu::findVitaShader(library,gpu::VitaShaderStage::Vertex,gpu::genVitaCpuVertexShader(uid));
    const auto* fragment=gpu::findVitaShader(library,gpu::VitaShaderStage::Fragment,gpu::genVitaEfbClearFragmentShader());
    if (!vertex || !fragment) { error="bringup requires the exact verified clear shader pair"; return -1; }
    const PortVitaGxmBlend blend{15,0,0,1,0,1,0}; PortVitaGxmProgramInfo info{};
    result=port_vita_gxm_device_program_create(renderer->device,vertex->data(),uint32_t(vertex->size()),
        fragment->data(),uint32_t(fragment->size()),&blend,4,0,&renderer->program,&info);
    if (result<0) { error="bringup native shader creation failed"; return result; }
    if (info.raster_floats!=8 || info.texture_mask) { error="bringup shader reflection disagrees with clear contract"; return -1; }
    renderer->ready=true; return 0;
}

int drawVitaBringup(VitaBringupRenderer* renderer,const PortPadState& pad,std::string& error) {
    error.clear();
    if (!renderer || !renderer->ready || renderer->failed || renderer->open) {
        error="bringup renderer is unavailable or retains a failed frame"; return -1;
    }
    if (!std::isfinite(pad.pointerX) || !std::isfinite(pad.pointerY) || !std::isfinite(pad.stickX) ||
        std::abs(pad.pointerX)>1 || std::abs(pad.pointerY)>1 || std::abs(pad.stickX)>1) {
        error="bringup controller sample is nonfinite or outside normalized range"; return -1;
    }
    auto fail=[&](int result,const char* message) { renderer->failed=true; error=message; return result; };
    int result=port_vita_gxm_staging_reset(renderer->staging);
    if (result<0) return fail(result,"bringup previous staging fence failed");
    PortVitaGxmVertex vertices[3]{};
    const float clip[3][4]={{-1,-1,0,1},{3,-1,0,1},{-1,3,0,1}};
    for (unsigned i=0;i<3;++i) std::copy(clip[i],clip[i]+4,vertices[i].clip);
    const uint16_t indices[]={0,1,2}; PortVitaGxmSlice slice{};
    result=port_vita_gxm_staging_upload(renderer->staging,vertices,3,indices,3,&slice);
    if (result<0) return fail(result,"bringup mapped upload failed");
    result=port_vita_gxm_staging_begin(renderer->staging,nullptr);
    if (result<0) return fail(result,"bringup display BeginScene failed");
    renderer->open=true;
    auto rectangle=[&](int left,int top,int right,int bottom,float r,float g,float b) {
        PortVitaGxmRaster raster{}; raster.width=960; raster.height=544;
        raster.viewport[2]=960; raster.viewport[3]=544;
        raster.scissor[0]=left; raster.scissor[1]=top; raster.scissor[2]=right; raster.scissor[3]=bottom;
        float uniforms[8]{},pixel[16]={r,g,b,1};
        int status=port_vita_gxm_raster_apply(port_vita_gxm_device_context(renderer->device),&raster,PORT_VITA_GXM_TRIANGLES,uniforms);
        if (status>=0) status=port_vita_gxm_staging_draw(renderer->staging,renderer->program,&slice,
            PORT_VITA_GXM_TRIANGLES,pixel,16,nullptr,uniforms);
        return status;
    };
    result=rectangle(0,0,960,544,0.08f,0.08f,0.08f);
    const int bandRight=std::clamp(760+int(pad.stickX*140),620,900);
    for (unsigned i=0;result>=0 && i<3;++i)
        result=rectangle(60,50+int(i)*150,bandRight,170+int(i)*150,i==0 ? 1 : 0,i==1 ? 1 : 0,i==2 ? 1 : 0);
    if (result>=0 && pad.connected && pad.pointerValid) {
        const int x=std::clamp(int((pad.pointerX+1)*480),12,948),y=std::clamp(int((pad.pointerY+1)*272),12,532);
        result=rectangle(x-12,y-12,x+12,y+12,1,1,1);
    }
    if (result<0) return fail(result,"bringup display draw failed");
    result=port_vita_gxm_staging_end(renderer->staging);
    if (result<0) return fail(result,"bringup display EndScene failed");
    renderer->open=false;
    result=port_vita_gxm_device_present(renderer->device);
    return result<0 ? fail(result,"bringup display queue submission failed") : 0;
}

int releaseVitaBringupRenderer(VitaBringupRenderer** pointer) {
    if (!pointer || !*pointer) return -1;
    auto* renderer=*pointer; renderer->ready=false;
    int result;
    if (renderer->open) {
        result=port_vita_gxm_staging_end(renderer->staging); if (result<0) return result;
        renderer->open=false;
    }
    if (renderer->staging) {
        result=port_vita_gxm_device_drain_display(renderer->device); if (result<0) return result;
        result=port_vita_gxm_staging_release(&renderer->staging); if (result<0) return result;
    }
    if (renderer->program) {
        result=port_vita_gxm_device_program_release(renderer->device,&renderer->program); if (result<0) return result;
    }
    if (renderer->device) {
        result=port_vita_gxm_device_release(&renderer->device); if (result<0) return result;
    }
    delete renderer; *pointer=nullptr; return 0;
}
