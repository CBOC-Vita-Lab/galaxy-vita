// Production Execute/RunWarm/Upload with Mesa and controlled wall-clock attribution.
// These injected intervals test the recorder, not physical Vita execution time.
extern "C" { unsigned char vgxvita_maskGuardDue = 0; }
#include "../platform/vgx/src/vgx_backend_gl.cpp"
#if VGX_COLDTRACE
#include "../platform/vgx/src/vgx_coldtrace_control.h"
#include "../platform/vgx/src/vgx_coldtrace_report.h"
#endif
#include <EGL/egl.h>
#include <array>
#include <cstdio>
#include <string>

namespace vgx { bool LogEnabled() { return false; } }
namespace {
uint64_t clockUs = 1000;
std::string scenario;
bool inject = false;
unsigned draws = 0, presents = 0, callsAtPresent = 0;
void Require(bool ok,const char* message);
#if VGX_COLDTRACE
vgx::coldtrace::Control* control=nullptr;
unsigned exports=0;
int Export(const vgx::coldtrace::FrozenView& view,void*) noexcept {
    ++exports;Require(view.data && view.data->executeCount==1,"pause export sees only fully drained execution");return 0;
}
#endif
uint64_t Clock() noexcept { return clockUs; }
uint32_t Thread() noexcept { return 1; }
void Require(bool ok,const char* message) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
void Context() {
    const auto display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
    Require(display!=EGL_NO_DISPLAY && eglInitialize(display,nullptr,nullptr),"EGL initialized");
    const EGLint attributes[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig config;EGLint count;
    Require(eglChooseConfig(display,attributes,&config,1,&count) && count==1,"RGBA pbuffer config");
    const EGLint version[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE},size[]={EGL_WIDTH,32,EGL_HEIGHT,32,EGL_NONE};
    const auto context=eglCreateContext(display,config,EGL_NO_CONTEXT,version);
    const auto surface=eglCreatePbufferSurface(display,config,size);
    Require(context!=EGL_NO_CONTEXT && surface!=EGL_NO_SURFACE && eglMakeCurrent(display,surface,surface,context),"real GLES context current");
}
void Present(uint32_t,int,int,int,int) {
    ++presents;
    callsAtPresent=vgx::backend::C.glCalls;
#if VGX_COLDTRACE
    if(scenario=="freeze-present") {
        control->PauseTurn(true,Export,nullptr);control->RenderBoundary();
        control->PauseTurn(true,Export,nullptr);
        Require(!exports && !vgx::coldtrace::g_recorder.Frozen().data,"present-root and Execute permits prevent premature pause export");
    }
#endif
    if (inject && scenario=="present") {
#if VGX_COLDTRACE
        vgx::coldtrace::Scope finish(vgx::coldtrace::g_recorder,vgx::coldtrace::Phase::Finish);
#endif
        clockUs+=74000;
    }
}
#if VGX_COLDTRACE
void Partition(const vgx::coldtrace::Span& span) {
    uint64_t phases=span.selfUs,origins=0;
    for(const auto value:span.exclusiveUs)phases+=value;
    for(const auto value:span.originUs)origins+=value;
    Require(!(span.flags&~vgx::coldtrace::kFrozenMidItem) && phases==span.rootUs && origins==span.rootUs && span.rootUs+span.unclassifiedUs==span.totalUs,
            "real backend spans have non-overlapping complete phase/origin/gap partitions");
}
#endif
}
extern "C" {
void __real_glCompileShader(GLuint);
void __wrap_glCompileShader(GLuint shader) {
    if(inject && (scenario=="compile" || scenario=="read")) {
#if VGX_COLDTRACE
        using namespace vgx::coldtrace;
        Scope leaf(g_recorder,scenario=="read"?Phase::Read:Phase::Shark,
                   scenario=="read"?IoClass::Read:IoClass::None);
        leaf.Result(scenario=="read"?32:0,32);
#endif
        clockUs+=208000;
    }
    __real_glCompileShader(shader);
}
void __real_glLinkProgram(GLuint);
void __wrap_glLinkProgram(GLuint program) {
    if(inject && scenario=="link")clockUs+=208000;
    __real_glLinkProgram(program);
}
void __real_glDrawElements(GLenum,GLsizei,GLenum,const void*);
void __wrap_glDrawElements(GLenum mode,GLsizei count,GLenum type,const void* indices) {
    ++draws;
    if(inject && scenario=="draw" && draws==1) {
#if VGX_COLDTRACE
        vgx::coldtrace::Scope patch(vgx::coldtrace::g_recorder,vgx::coldtrace::Phase::PatchFragment);
#endif
        clockUs+=208000;
    }
    __real_glDrawElements(mode,count,type,indices);
}
void __real_glGenerateMipmap(GLenum);
void __wrap_glGenerateMipmap(GLenum target) {
    if(inject && scenario=="upload") {
#if VGX_COLDTRACE
        vgx::coldtrace::Scope transfer(vgx::coldtrace::g_recorder,vgx::coldtrace::Phase::TransferFinish);
#endif
        clockUs+=133000;
    }
    __real_glGenerateMipmap(target);
}
void __real_glBufferData(GLenum,GLsizeiptr,const void*,GLenum);
void __wrap_glBufferData(GLenum target,GLsizeiptr size,const void* data,GLenum usage) {
    if(inject && scenario=="stream" && usage==GL_STREAM_DRAW)clockUs+=33000;
    __real_glBufferData(target,size,data,usage);
}
}

int main(int argc,char** argv) {
    Require(argc==2 || argc==3,"supply coldtrace attribution scenario");scenario=argv[1];
    Require(scenario=="compile" || scenario=="read" || scenario=="link" || scenario=="draw" ||
            scenario=="upload" || scenario=="stream" || scenario=="present" || scenario=="direct" || scenario=="idle" || scenario=="freeze-present","known scenario");
    setenv("GXV_WARM_DEDUP","0",1);setenv("GXV_DUMPDISPLAY","0",1);
    Context();using namespace vgx;using namespace vgx::backend;
    Require(Init(32,32),"real backend initialized");
    ShaderSource vs,fs;vs.id=3;fs.id=4;fs.fragment=true;
    vs.text="attribute vec4 a_pos; void main(){gl_Position=a_pos;}";
    fs.text="precision mediump float; void main(){gl_FragColor=vec4(0.25,0.75,0.5,1.0);}";
    RegisterShader(&vs);CompileShader(false,3);
#if VGX_COLDTRACE
    Require(coldtrace::g_recorder.Arm(Clock,Thread,1),"arm real Execute fixture");
    coldtrace::Control captureControl(coldtrace::g_recorder);control=&captureControl;
    Require(control->Activate(),"activate actual pause/freeze controller");
#endif
    const auto before=GetStats();
    CmdBuf buffer;buffer.frame=42;buffer.shaders={&fs};buffer.variants.push_back({3,4,0,0xc00});
    const std::array<Cmd,3> work={{{CMD_SHADER,0},{CMD_PROGRAM,(3u<<16)|4u},{CMD_VARIANT,0}}};
    if(scenario=="direct")buffer.cmds.insert(buffer.cmds.end(),work.begin(),work.end());
    else buffer.warm.insert(buffer.warm.end(),work.begin(),work.end());
    const float positions[][4]={{-0.8f,-0.8f,0,1},{0.8f,-0.8f,0,1},{0,0.8f,0,1}};
    buffer.vtx.resize(sizeof positions);std::memcpy(buffer.vtx.data(),positions,sizeof positions);buffer.idx={0,1,2};
    DrawRec triangle;triangle.vsId=3;triangle.fsId=4;triangle.vtxCount=triangle.idxCount=3;
    buffer.draws.push_back(triangle);
    if(scenario=="upload") {
        TexUpload texture;texture.texId=19;texture.width=texture.height=8;texture.genMips=true;
        texture.levels.emplace_back(256,255);buffer.uploads.push_back(std::move(texture));buffer.cmds.push_back({CMD_TEX_UPLOAD,0});
    }
    buffer.cmds.push_back({CMD_DRAW,0});buffer.copyDisp.push_back({{0,0,32,32},{},false});buffer.cmds.push_back({CMD_COPY_DISP,0});
    SetPresentHook(Present);SetWarmBudget(1000000);inject=true;
    if(scenario=="idle") {
        for(const auto command:buffer.warm)DeferWarm(buffer,command);
        buffer.warm.clear();g_progFrame=999;WarmIdle();
    }
    Execute(buffer);inject=false;
    const auto after=GetStats();
    Require(after.fragmentShaders-before.fragmentShaders==1 && after.vertexShaders==before.vertexShaders && after.programLinks-before.programLinks==1,
            "all attribution cases execute the same one-fragment/one-program workload");
    Require(WarmPending()==0 && draws==2 && presents==1,"warm queue retires and foreground/present execute exactly once");
    std::array<uint8_t,32*32*4> pixels{};glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    const auto center=(16*32+16)*4;
    Require(pixels[center]>50 && pixels[center+1]>170 && pixels[center+2]>100 && glGetError()==GL_NO_ERROR,
            "actual foreground triangle stays visible and GL remains valid");
    uint64_t hash=14695981039346656037ull;for(const auto value:pixels)hash=(hash^value)*1099511628211ull;
#if VGX_COLDTRACE
    using namespace coldtrace;
    g_recorder.RequestFreeze();const auto view=g_recorder.Frozen();
    Require(view.data && view.data->executeCount==1 && !view.clockErrors && !view.data->spanErrors,"real execution freezes without tracing errors");
    const auto& execute=view.data->worstExecutions[0];Partition(execute);
    Require(execute.frame==42 && execute.parentSerial==0,"execution uses current command-buffer identity even before UploadStream");
    if(scenario=="compile" || scenario=="read" || scenario=="link" || scenario=="draw") {
        const auto phase=scenario=="compile"?Phase::Shark:scenario=="read"?Phase::Read:scenario=="link"?Phase::Link:Phase::PatchFragment;
        Require(view.data->sliceCount==1,"real RunWarm records a complete slice");
        const auto& slice=view.data->worstSlices[0];Partition(slice);
        Require(slice.totalUs==208000 && slice.rootUs==208000 && slice.parentSerial==execute.serial && slice.roots==3,
                "real208ms slice is attributed completely despite identical shader/program counters");
        Require(slice.exclusiveUs[size_t(phase)]==208000 && slice.maxItem.totalUs==208000,
                "injected compiler/read/link/draw-patcher delay reaches only its correct exclusive bucket");
        Require(slice.maxItem.origin==Origin::Queued && execute.totalUs==208000,"queued root keeps origin and does not double-count in Execute");
    } else if(scenario=="upload") {
        Require(execute.totalUs==133000 && execute.exclusiveUs[size_t(Phase::TransferFinish)]==133000 &&
                execute.originUs[size_t(Origin::Texture)]==133000,"real Upload owns the transfer wait separately from warm work");
        Require(view.data->hasUploadExecution && view.data->maxUploadExecution.frame==42,"upload-heavy frame selected independently");
    } else if(scenario=="stream") {
        Require(execute.totalUs==66000 && execute.originUs[size_t(Origin::Stream)]==66000,"both actual stream uploads have current Execute parentage");
    } else if(scenario=="present") {
        Require(execute.totalUs==74000 && execute.exclusiveUs[size_t(Phase::Finish)]==74000 &&
                execute.originUs[size_t(Origin::Present)]==74000,"present-hook finish stays inside actual Execute span");
    } else if(scenario=="idle") {
        Require(view.data->sliceCount==1 && view.data->worstSlices[0].parentSerial==0 && view.data->worstSlices[0].frame==0,
                "real WarmIdle cannot inherit stale g_progFrame999 as execution parent");
    } else if(scenario=="freeze-present") {
        Require(execute.flags&kFrozenMidItem,"freeze during actual present marks the whole active Execute span partial");
        control->PauseTurn(true,Export,nullptr);control->PauseTurn(true,Export,nullptr);
        Require(exports==1 && control->Status()==ControlState::Complete,"actual controller exports once only after Execute drains");
    } else {
        Require(view.data->sliceCount==0,"immediate commands are not mislabeled as queued warm slices");
        bool directShader=false,directProgram=false;
        for(unsigned i=0;i<view.data->worstCount;++i){const auto& item=view.data->worst[i];
            directShader|=item.origin==Origin::DirectShader;directProgram|=item.origin==Origin::DirectProgram;}
        Require(directShader && directProgram,"actual direct shader/program commands have truthful origins");
    }
    if(argc==3){
        const auto write=[](const char* bytes,size_t size,void*) noexcept->int64_t {
            return static_cast<int64_t>(std::fwrite(bytes,1,size,stdout));
        };
        Require(WriteReport(view,{write,nullptr}).error==0,"complete real backend capture serializes for independent parser");return 0;
    }
#endif
    Require(callsAtPresent>0,"parity checks observe actual GL calls before per-frame counters reset");
    std::printf("{\"scenario\":\"%s\",\"probe\":%d,\"draws\":%u,\"presents\":%u,\"gl_calls_at_present\":%u,\"pixel_hash\":\"%016llx\"}\n",
                scenario.c_str(),VGX_COLDTRACE,draws,presents,callsAtPresent,static_cast<unsigned long long>(hash));
}
