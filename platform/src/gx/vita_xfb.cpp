#include "vita_xfb.h"
#include "vita_efb.h"
#include "../../../vita/runtime/gpu_device_internal.h"
#include "../../../vita/runtime/gpu_texture.h"
#include "../../../vita/runtime/gpu_raster.h"
#include "../../../vita/runtime/gpu_staging.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <fcntl.h>
#include <unistd.h>

namespace gpu {
namespace {
constexpr int invalid=-1,noMemory=-2;
struct Artifact {
    std::unique_ptr<unsigned char[]> data;
    uint32_t bytes=0;
    bool copy(const std::vector<unsigned char>& source) noexcept {
        data.reset(new(std::nothrow) unsigned char[source.size()]);
        if (!data) return false;
        bytes=uint32_t(source.size()); std::memcpy(data.get(),source.data(),source.size()); return true;
    }
};
struct Geometry {
    uint32_t width=0,height=0,pitch=0,rowBytes=0,parity=0;
    size_t offset=0;
    float left=0,top=0,displayWidth=0,displayHeight=0;
};
bool geometry(const VitaXfbPresent& request,const GxTextureMemory& memory,Geometry& out,std::string& error) {
    const auto& vi=request.display;
    const unsigned standard=vi.tv_mode>>2;
    if (!request.hasFramebuffer || vi.configured!=1 || (vi.tv_mode&3)!=2 ||
        (standard!=0 && standard!=2 && standard!=5) || vi.xfb_mode || vi.field_rendering || vi.aspect>1) {
        error="native VI requires a configured progressive NTSC/MPAL/EURGB60 single-field XFB; other scanout strategies are not installed";
        return false;
    }
    if (!vi.fb_width || vi.fb_width>1024 || !vi.fb_height || vi.fb_height>1024 ||
        !vi.pan_width || !vi.pan_height || vi.pan_x>=vi.fb_width || vi.pan_y>=vi.fb_height ||
        vi.pan_width>vi.fb_width-vi.pan_x || vi.pan_height>vi.fb_height-vi.pan_y ||
        !vi.display_width || vi.origin_x>=720 || vi.display_width>720-vi.origin_x ||
        vi.origin_y>=480 || vi.display_height!=vi.pan_height || vi.display_height>480-vi.origin_y) {
        error="original VI pan/output geometry is outside its XFB or 720x480 progressive canvas"; return false;
    }
    const uint32_t pitch=((vi.fb_width+15)&~15u)*2;
    const uint32_t pairStart=vi.pan_x&~1u;
    const uint32_t pairEnd=(vi.pan_x+vi.pan_width+1)&~1u;
    const uint64_t first=uint64_t(request.address)+uint64_t(vi.pan_y)*pitch;
    const uint64_t end=first+uint64_t(vi.pan_height-1)*pitch+pairEnd*2;
    const uint64_t bankEnd=uint64_t(memory.ramBase)+memory.ramBytes;
    if (!memory.ram || request.address<memory.ramBase || bankEnd>uint64_t(UINT32_MAX)+1 ||
        end>bankEnd || first<memory.ramBase || end<=first) {
        error="selected original VI YUYV rows/chroma pairs exceed their registered physical RAM bank"; return false;
    }
    const float canvasHeight=vi.aspect ? 540.0f : 544.0f;
    const float canvasWidth=vi.aspect ? 960.0f : canvasHeight*4.0f/3.0f;
    out={vi.pan_width,vi.pan_height,pitch,(pairEnd-pairStart)*2,vi.pan_x&1u,
        size_t(first-memory.ramBase)+pairStart*2,
        (960-canvasWidth)*0.5f+float(vi.origin_x)*canvasWidth/720,
        (544-canvasHeight)*0.5f+float(vi.origin_y)*canvasHeight/480,
        float(vi.display_width)*canvasWidth/720,float(vi.display_height)*canvasHeight/480};
    return true;
}
unsigned char component(int value) { return static_cast<unsigned char>(std::clamp((value+128)>>8,0,255)); }
}

struct VitaXfbDisplay {
    PortVitaGxmDevice* device=nullptr;
    PortVitaGxmStaging* staging=nullptr;
    PortVitaGxmProgram* programs[2]{};
    bool ready[2]{},open=false,failed=false,closing=false;
    Artifact vertex,fragment[2];
    PortVitaGxmTexture* texture=nullptr;
    PortVitaGxmTextureView* view=nullptr;
    std::unique_ptr<unsigned char[]> pixels;
    std::unique_ptr<unsigned char[]> yuyv;
    uint32_t imageWidth=0,imageHeight=0,imageRowBytes=0,imageParity=0;
    uint64_t version=0;
#if defined(__vita__)
    VitaXfbCapture capture;
    unsigned imageReports[2]{};
#endif
};

bool writeVitaXfbSnapshot(FILE* file,const unsigned char* rgba,uint32_t width,uint32_t height,size_t bytes) {
    if (!file || !rgba || !width || !height || width>1024 || height>1024 ||
        bytes!=size_t(width)*height*4) return false;
    char header[128];
    const int count=std::snprintf(header,sizeof(header),
        "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",width,height);
    return count>0 && size_t(count)<sizeof(header) &&
        std::fwrite(header,1,size_t(count),file)==size_t(count) &&
        std::fwrite(rgba,1,bytes,file)==bytes && std::fflush(file)==0;
}
void VitaXfbCapture::configure(FILE* file) {
    std::fill(std::begin(captureTag),std::end(captureTag),0); capturePresent=0;
    if (!file) return;
    char tag[6]{};
    const size_t bytes=std::fread(tag,1,sizeof(tag),file);
    const bool valid=bytes==5 && !std::ferror(file) && tag[4]=='\n' &&
        std::all_of(tag,tag+4,[](char c) { return c>='0' && c<='9'; });
    if (valid) std::copy(tag,tag+4,captureTag);
}
int VitaXfbCapture::capture(const char* directory,const unsigned char* rgba,uint32_t width,uint32_t height,size_t bytes) {
    if (!captureTag[0] || capturePresent>=180) return 0;
    const unsigned present=++capturePresent;
    if (present!=1 && present!=60 && present!=180) return 0;
    char path[128];
    if (!directory) return invalid;
    const int count=std::snprintf(path,sizeof(path),"%s/xfb-%s-%03u.pam",directory,captureTag,present);
    if (count<=0 || size_t(count)>=sizeof(path)) return invalid;
    const int fd=::open(path,O_WRONLY|O_CREAT|O_EXCL,0600);
    bool saved=false;
    if (fd>=0) {
        FILE* file=::fdopen(fd,"wb");
        if (file) {
            saved=writeVitaXfbSnapshot(file,rgba,width,height,bytes);
            if (std::fclose(file)!=0) saved=false;
        } else ::close(fd);
        if (!saved) ::unlink(path);
    }
    return saved ? 1 : invalid;
}
#if defined(__vita__)
static void configureCapture(VitaXfbDisplay* display) {
    FILE* file=std::fopen("ux0:data/codex-galaxy/diagnostics/capture-xfb","rb");
    display->capture.configure(file);
    if (file) std::fclose(file);
}
static void captureDecodedXfb(VitaXfbDisplay* display,const Geometry& image) {
    const int result=display->capture.capture("ux0:data/codex-galaxy/diagnostics",display->pixels.get(),
        image.width,image.height,size_t(image.width)*image.height*4);
    if (!result) return;
    char line[256];
    const int count=std::snprintf(line,sizeof(line),"native-xfb decoded capture=%s present=%u extent=%ux%u saved=%u\n",
        display->capture.tag(),display->capture.present(),image.width,image.height,unsigned(result>0));
    if (count>0 && size_t(count)<sizeof(line)) std::fwrite(line,1,size_t(count),stderr);
    std::fflush(stderr);
}
#endif

int createVitaXfbDisplay(PortVitaGxmDevice* device,const VitaShaderLibrary& library,VitaXfbDisplay** out,std::string& error) {
    error.clear();
    if (!out || *out) { error="native VI display requires an empty owner"; return invalid; }
    ShaderUid uid{}; uid.numTevStages=1; uid.alphaFunc=63;
    const auto* vs=findVitaShader(library,VitaShaderStage::Vertex,genVitaCpuVertexShader(uid));
    const auto* clear=findVitaShader(library,VitaShaderStage::Fragment,genVitaEfbClearFragmentShader());
    const auto* sample=findVitaShader(library,VitaShaderStage::Fragment,genVitaEfbSnapshotFragmentShader(false));
    if (!vs || !clear || !sample) { error="native VI requires exact full-precision clear and color-sampling compiler artifacts"; return invalid; }
    int result=port_vita_gxm_device_hold_resource(device); if (result<0) return result;
    auto* display=new(std::nothrow) VitaXfbDisplay;
    if (!display || !display->vertex.copy(*vs) || !display->fragment[0].copy(*clear) || !display->fragment[1].copy(*sample)) {
        delete display; port_vita_gxm_device_drop_resource(device); error="native VI shader CPU OOM"; return noMemory;
    }
    display->device=device;
    result=port_vita_gxm_staging_create(device,4096,&display->staging);
    if (result<0) { delete display; port_vita_gxm_device_drop_resource(device); return result; }
#if defined(__vita__)
    configureCapture(display);
#endif
    *out=display; return 0;
}
static int retireImage(VitaXfbDisplay* display) {
    if (display->view) {
        const int result=port_vita_gxm_texture_view_release(&display->view); if (result<0) return result;
    }
    if (display->texture) {
        const int result=port_vita_gxm_texture_release(&display->texture); if (result<0) return result;
    }
    display->pixels.reset(); display->yuyv.reset();
    display->imageWidth=display->imageHeight=display->imageRowBytes=display->imageParity=0;
    return 0;
}
static bool sameImage(const VitaXfbDisplay* display,const Geometry& image,const GxTextureMemory& memory) {
    if (!display->texture || !display->view || !display->pixels || !display->yuyv ||
        display->imageWidth!=image.width || display->imageHeight!=image.height ||
        display->imageRowBytes!=image.rowBytes || display->imageParity!=image.parity) return false;
    // Read every selected pair from current validated RAM, including odd-crop
    // shared chroma. Neither an address nor a previous GX copy is freshness proof.
    for (unsigned y=0;y<image.height;++y)
        if (std::memcmp(display->yuyv.get()+size_t(y)*image.rowBytes,
                memory.ram+image.offset+size_t(y)*image.pitch,image.rowBytes)!=0) return false;
    return true;
}
static int program(VitaXfbDisplay* display,unsigned key,std::string& error) {
    if (display->ready[key]) return 0;
    const PortVitaGxmBlend blend{15,0,0,1,0,1,0}; PortVitaGxmProgramInfo info{};
    const int result=port_vita_gxm_device_program_create_owned(display->device,display,
        display->vertex.data.get(),display->vertex.bytes,display->fragment[key].data.get(),display->fragment[key].bytes,
        &blend,4+key,key,&display->programs[key],&info);
    if (result<0) { error="native VI program creation failed; rollback ownership retained"; return result; }
    if (info.raster_floats!=8 || info.texture_mask!=key || !info.pixel_floats) {
        error="native VI compiler reflection lost exact raster/pixel/sampler contract"; return invalid;
    }
    display->ready[key]=true; return 0;
}
bool vitaXfbSceneOpen(const VitaXfbDisplay* display) { return display && display->open; }
int discardVitaXfb(VitaXfbDisplay* display) {
    if (!display || display->closing) return invalid;
    if (display->open) {
        const int result=port_vita_gxm_staging_end(display->staging); if (result<0) return result;
        display->open=false;
    }
    return port_vita_gxm_staging_reset(display->staging);
}
int presentVitaXfb(VitaXfbDisplay* display,const VitaXfbPresent& request,const GxTextureMemory& memory,std::string& error) {
    error.clear();
    if (!display || display->closing || display->failed || display->open ||
        port_vita_gxm_device_scene_active(display->device) || !port_vita_gxm_device_context(display->device)) {
        error="native VI display is unavailable or retains a failed submission"; return invalid;
    }
    auto fail=[&](int result,const char* message) {
        display->failed=true; if (message) error=message; return result;
    };
    Geometry image{};
    if (!request.black && !geometry(request,memory,image,error)) return fail(invalid,nullptr);
    const bool reuse=!request.black && sameImage(display,image,memory);
    int result=0;
    if (!reuse) {
        result=retireImage(display);
        if (result<0) return fail(result,"native VI previous immutable image retirement failed");
    }
    result=program(display,0,error); if (result<0) return fail(result,nullptr);
    const void* descriptor=nullptr;
    if (!request.black) {
        if (!reuse) {
            if (display->version==UINT64_MAX) return fail(invalid,"native VI image version exhausted");
            const uint32_t bytes=image.width*image.height*4;
            display->pixels.reset(new(std::nothrow) unsigned char[bytes]);
            display->yuyv.reset(new(std::nothrow) unsigned char[size_t(image.rowBytes)*image.height]);
            if (!display->pixels || !display->yuyv) return fail(noMemory,"native VI decoded XFB/snapshot CPU OOM");
            for (unsigned y=0;y<image.height;++y)
                std::memcpy(display->yuyv.get()+size_t(y)*image.rowBytes,
                    memory.ram+image.offset+size_t(y)*image.pitch,image.rowBytes);
            // Independently authored limited-range BT.601 inverse. Odd pan starts
            // retain the original pair's shared U/V; CPU-written XFB bytes win.
            for (unsigned y=0;y<image.height;++y) for (unsigned x=0;x<image.width;++x) {
                const unsigned sourceX=image.parity+x;
                const auto* pair=display->yuyv.get()+size_t(y)*image.rowBytes+(sourceX&~1u)*2;
                const int luma=298*(int(pair[(sourceX&1)*2])-16),u=int(pair[1])-128,v=int(pair[3])-128;
                auto* pixel=display->pixels.get()+(size_t(y)*image.width+x)*4;
                pixel[0]=component(luma+409*v); pixel[1]=component(luma-100*u-208*v);
                pixel[2]=component(luma+516*u); pixel[3]=255;
            }
            result=port_vita_gxm_texture_create(display->device,image.width,image.height,1,++display->version,
                display->pixels.get(),bytes,&display->texture);
            if (result<0) return fail(result,"native VI immutable XFB upload failed; rollback ownership retained");
            result=port_vita_gxm_texture_view_create(display->texture,(1u<<4)|(4u<<5),0,&display->view);
            if (result<0) return fail(result,"native VI linear/clamp sampler creation failed");
            display->imageWidth=image.width; display->imageHeight=image.height;
            display->imageRowBytes=image.rowBytes; display->imageParity=image.parity;
        }
        result=port_vita_gxm_texture_view_descriptor(display->view,display->device,&descriptor);
        if (result<0) return fail(result,"native VI linear/clamp sampler creation failed");
        result=program(display,1,error); if (result<0) return fail(result,nullptr);
    }
    result=port_vita_gxm_staging_reset(display->staging);
    if (result<0) return fail(result,"native VI previous staging fence failed");
    PortVitaGxmVertex vertices[3]{}; const float clip[3][4]={{-1,-1,0,1},{3,-1,0,1},{-1,3,0,1}};
    for (unsigned i=0;i<3;++i) std::copy(clip[i],clip[i]+4,vertices[i].clip);
    const uint16_t indices[]={0,1,2}; PortVitaGxmSlice slice{};
    result=port_vita_gxm_staging_upload(display->staging,vertices,3,indices,3,&slice);
    if (result<0) return fail(result,"native VI mapped display upload failed");
    result=port_vita_gxm_staging_begin(display->staging,nullptr);
    if (result<0) return fail(result,"native VI display BeginScene failed");
    display->open=true;
    PortVitaGxmRaster raster{}; raster.width=960; raster.height=544;
    raster.viewport[2]=960; raster.viewport[3]=544; raster.scissor[2]=960; raster.scissor[3]=544;
    float uniforms[8]{},black[16]={0,0,0,1};
    result=port_vita_gxm_raster_apply(port_vita_gxm_device_context(display->device),&raster,PORT_VITA_GXM_TRIANGLES,uniforms);
    if (result>=0) result=port_vita_gxm_staging_draw(display->staging,display->programs[0],&slice,
        PORT_VITA_GXM_TRIANGLES,black,16,nullptr,uniforms);
    if (result<0) return fail(result,"native VI full display black draw failed; scene owner retained");
    if (!request.black) {
        raster.scissor[0]=int32_t(std::ceil(image.left-0.5f)); raster.scissor[1]=int32_t(std::ceil(image.top-0.5f));
        raster.scissor[2]=int32_t(std::ceil(image.left+image.displayWidth-0.5f));
        raster.scissor[3]=int32_t(std::ceil(image.top+image.displayHeight-0.5f));
        float pixel[20]={-image.left,-image.top,image.displayWidth,image.displayHeight};
        const void* textures[8]{descriptor};
        result=port_vita_gxm_raster_apply(port_vita_gxm_device_context(display->device),&raster,PORT_VITA_GXM_TRIANGLES,uniforms);
        if (result>=0) result=port_vita_gxm_staging_draw(display->staging,display->programs[1],&slice,
            PORT_VITA_GXM_TRIANGLES,pixel,20,textures,uniforms);
        if (result<0) return fail(result,"native VI selected XFB draw failed; source/staging ownership retained");
    }
    result=port_vita_gxm_staging_end(display->staging);
    if (result<0) return fail(result,"native VI display EndScene failed; actual scene owner retained");
    display->open=false;
    result=port_vita_gxm_device_finish(display->device);
    if (result>=0) result=port_vita_gxm_device_present(display->device);
    // The GPU fence retires borrowed XFB inputs. The device's three output
    // buffers and sync objects own queued scanout; draining here holds the
    // guest's VI interrupt until a host vblank. Drain only during retirement.
    if (result>=0) result=port_vita_gxm_device_error(display->device);
#if defined(__vita__)
    if (result>=0 && !request.black) {
        if (display->imageReports[reuse]<2) {
            ++display->imageReports[reuse];
            char line[160];
            const int count=std::snprintf(line,sizeof(line),"native-xfb image=%s version=%llu extent=%ux%u row-bytes=%u\n",
                reuse ? "reuse" : "new",static_cast<unsigned long long>(display->version),image.width,image.height,image.rowBytes);
            if (count>0 && size_t(count)<sizeof(line)) std::fwrite(line,1,size_t(count),stderr);
        }
        captureDecodedXfb(display,image);
    }
#endif
    return result<0 ? fail(result,"native VI display queue/callback failed; no automatic replay") : 0;
}
int releaseVitaXfbDisplay(VitaXfbDisplay** pointer) {
    if (!pointer || !*pointer) return invalid;
    auto* display=*pointer;
    if (!display->closing) {
        const int result=discardVitaXfb(display); if (result<0) return result;
        display->closing=true;
    }
    int result=port_vita_gxm_device_retire_display(display->device); if (result<0) return result;
    result=retireImage(display); if (result<0) return result;
    for (auto*& owner:display->programs) if (owner) {
        result=port_vita_gxm_device_program_release_owned(display->device,&owner,display); if (result<0) return result;
    }
    result=port_vita_gxm_staging_release(&display->staging); if (result<0) return result;
    port_vita_gxm_device_drop_resource(display->device); delete display; *pointer=nullptr; return 0;
}
}
