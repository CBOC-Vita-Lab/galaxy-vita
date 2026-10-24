// Exercise the real backend with Mesa; no game assets or physical GPU claims.
#include "../platform/vgx/src/vgx_coldtrace.h"
extern "C" { unsigned char vgxvita_maskGuardDue = 0; }
#include "../platform/vgx/src/vgx_backend_gl.cpp"
#include <EGL/egl.h>
#include <array>
#include <cstdio>
#include <string>

namespace vgx { bool LogEnabled() { return false; } }
namespace {
unsigned actualDraws = 0;
#if VGX_COLDTRACE
uint64_t TraceClock() noexcept { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint32_t TraceThread() noexcept { return 1; }
#endif
void Require(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
void Context() {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    Require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "surfaceless EGL initialized");
    const EGLint attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                 EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config; EGLint count;
    Require(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1, "RGBA8 pbuffer config");
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    const EGLint dimensions[] = {EGL_WIDTH, 32, EGL_HEIGHT, 32, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, dimensions);
    Require(context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE && eglMakeCurrent(display, surface, surface, context),
            "real GLES2 context current");
}
GLuint Shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type); glShaderSource(shader, 1, &source, nullptr); glCompileShader(shader);
    GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok); Require(ok, "synthetic shader compiled");
    return shader;
}
void Program(uint16_t vs, uint16_t fs, bool good = true) {
    const GLuint vertex = Shader(GL_VERTEX_SHADER, "attribute vec4 a_pos; void main(){gl_Position=a_pos;}");
    const GLuint fragment = Shader(GL_FRAGMENT_SHADER, "precision mediump float; void main(){gl_FragColor=vec4(0.25,0.75,0.5,1.0);}");
    GLuint id = glCreateProgram(); glAttachShader(id, vertex); glAttachShader(id, fragment);
    glBindAttribLocation(id, vgx::AT_POS, "a_pos"); glLinkProgram(id);
    GLint ok = 0; glGetProgramiv(id, GL_LINK_STATUS, &ok); Require(ok, "synthetic program linked");
    glDeleteShader(vertex); glDeleteShader(fragment);
    vgx::backend::Program p; p.id = id; p.ok = good;
    vgx::backend::g_programs.emplace((uint32_t(vs) << 16) | fs, std::move(p));
}
vgx::CmdBuf Triangle(uint16_t vs, uint16_t fs, unsigned offset = 0) {
    vgx::CmdBuf b;
    const float positions[][4] = {{-0.8f,-0.8f,0,1},{0.8f,-0.8f,0,1},{0,0.8f,0,1}};
    b.vtx.resize(offset + sizeof positions);
    std::memcpy(b.vtx.data() + offset, positions, sizeof positions);
    b.idx = {0,1,2};
    vgx::DrawRec d; d.vsId = vs; d.fsId = fs; d.idxCount = d.vtxCount = 3; d.vtxOffset = offset;
    b.draws.push_back(d); return b;
}
void Draw(vgx::CmdBuf& b) {
    vgx::backend::UploadStream(b); vgx::backend::Draw(b, b.draws[0]);
}
using Pixels = std::array<uint8_t,32 * 32 * 4>;
Pixels Read() {
    Pixels pixels{}; glReadPixels(0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    Require(glGetError() == GL_NO_ERROR, "real GL calls succeed"); return pixels;
}
GLuint OtherTarget() {
    GLuint texture, fbo; glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo); vgx::backend::BindFbo(fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "separate pass framebuffer complete");
    return fbo;
}
}

extern "C" void __real_glDrawElements(GLenum, GLsizei, GLenum, const void*);
extern "C" void __wrap_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    ++actualDraws; __real_glDrawElements(mode,count,type,indices);
}

int main(int argc, char** argv) {
    Require(argc == 3, "supply scenario and dedup flag");
    const std::string mode = argv[1]; const bool dedup = std::atoi(argv[2]) != 0;
    setenv("GXV_WARM_DEDUP", dedup ? "1" : "0", 1);
    setenv("GXV_MAXPROGRAMS", "1", 1); setenv("GXV_DUMPDISPLAY", "0", 1);
    Context();
#if VGX_COLDTRACE
    Require(vgx::coldtrace::g_recorder.Arm(TraceClock,TraceThread,1),"opt-in real backend capture armed before initialization");
#endif
    Require(vgx::backend::Init(32, 32), "real backend initialized");
    vgx::backend::g_serial = 7;
    Program(1, 0);
    vgx::VariantCmd variant{1,0,0,0xc00};
    auto b = Triangle(1,0,mode == "offset" ? 16 : 0);
    unsigned expected = 1, observedWarm = ~0u;
    if (mode == "repeat") {
        vgx::backend::WarmVariant(variant);
        const unsigned calls = vgx::backend::C.glCalls;
        vgx::backend::WarmVariant(variant);
        expected = dedup ? 1 : 2;
        if (dedup) Require(vgx::backend::C.glCalls == calls, "already-warmed variant issues no GL state/draw calls");
        ++vgx::backend::g_serial; vgx::backend::g_progsThisFrame = 0;
        vgx::backend::WarmVariant(variant);
        Require(vgx::backend::g_programs.at(1u << 16).usedSerial == vgx::backend::g_serial &&
                vgx::backend::g_progsThisFrame == 1, "skipped warm work retains program-use accounting in later frames");
        if (!dedup) ++expected;
    } else if (mode == "execute") {
        b.cmds.push_back({vgx::CMD_DRAW,0});
        b.copyDisp.push_back({{0,0,32,32},{},false}); b.cmds.push_back({vgx::CMD_COPY_DISP,0});
        b.cmds.push_back({vgx::CMD_PRESENT,0});
        vgx::backend::Execute(b); const Pixels before=Read();
        const size_t center=(16*32+16)*4;
        Require(before[center]>50 && before[center+1]>170 && before[center+2]>100,"Execute renders a visible foreground triangle before present");
        // Model a host display hook leaving another framebuffer bound between frames.
        OtherTarget();
        const unsigned drawsBefore=actualDraws;
        vgx::CmdBuf next; next.variants.push_back(variant); next.warm.push_back({vgx::CMD_VARIANT,0});
        vgx::backend::SetWarmBudget(1000); vgx::backend::Execute(next);
        observedWarm=actualDraws-drawsBefore;
        expected=dedup?0:1;
        Require(vgx::backend::WarmPending()==0,"real Execute retires queued work after rebinding the EFB");
        Require(before==Read(),"warm slice after actual Execute/present preserves EFB pixels");
    } else if (mode == "lines" || mode == "points" || mode == "real-blend") {
        if(mode=="lines")b.draws[0].prim=vgx::PRIM_LINES;
        if(mode=="points")b.draws[0].prim=vgx::PRIM_POINTS;
        if(mode=="real-blend")variant.blendKey=0x400;
        Draw(b); vgx::backend::WarmVariant(variant);
    } else if (mode == "real" || mode == "offset") {
        Draw(b); const Pixels before = Read();
        const size_t center = (16 * 32 + 16) * 4;
        Require(before[center] > 50 && before[center + 1] > 170 && before[center + 2] > 100,
                "real foreground triangle renders nonblank expected color");
        vgx::backend::WarmVariant(variant);
        expected = dedup && mode == "real" ? 0 : 1;
        Require(before == Read(), "prewarming preserves actual EFB pixels");
        Draw(b); Require(before == Read(), "next foreground draw retains its pixels and state");
    } else if (mode == "pass" || mode == "pass-after-warm") {
        if (mode == "pass-after-warm") vgx::backend::WarmVariant(variant);
        OtherTarget(); Draw(b);
        vgx::backend::BindFbo(vgx::backend::g_efbFbo);
        vgx::backend::WarmVariant(variant);
        if (mode == "pass-after-warm") expected = 2;
    } else if (mode == "switched") {
        vgx::backend::WarmVariant(variant);
        auto other = variant; other.blendKey = 0x400; vgx::backend::WarmVariant(other);
        vgx::backend::WarmVariant(variant); expected = 3;
    } else if (mode == "layout" || mode == "blend" || mode == "ids") {
        vgx::backend::WarmVariant(variant);
        if (mode == "layout") variant.layoutMask = vgx::VL_NRM;
        if (mode == "blend") variant.blendKey = 0x400;
        if (mode == "ids") { Program(0,1024); variant = {0,1024,0,0xc00}; }
        vgx::backend::WarmVariant(variant); expected = 2;
    } else if (mode == "evict") {
        vgx::backend::WarmVariant(variant); Program(0,2);
        vgx::backend::g_serial = 1000; vgx::backend::EvictPrograms();
        Require(vgx::backend::g_programs.empty(), "production eviction removes expired linked programs");
        Program(1,0); vgx::backend::WarmVariant(variant); expected = 2;
    } else if (mode == "empty" || mode == "clipped") {
        if (mode == "empty") b.draws[0].idxCount = 0; else b.draws[0].rs.scW = 0;
        Draw(b); vgx::backend::WarmVariant(variant);
    } else if (mode == "failed") {
        vgx::backend::g_programs.at(1u << 16).ok = false;
        vgx::backend::WarmVariant(variant);
        Require(vgx::backend::C.warmDraws == 0, "failed program never marks a completed warm draw");
        vgx::backend::g_programs.at(1u << 16).ok = true; vgx::backend::WarmVariant(variant);
    } else if(mode=="queue-shaders") {
        vgx::ShaderSource vs,fs;vs.id=3;fs.id=4;fs.fragment=true;
        vs.text="attribute vec4 a_pos; void main(){gl_Position=a_pos;}";
        fs.text="precision mediump float; void main(){gl_FragColor=vec4(0.25,0.75,0.5,1.0);}";
        vgx::CmdBuf queued;queued.shaders={&vs,&fs};queued.variants.push_back({3,4,0,0xc00});
        for(const auto c:std::array<vgx::Cmd,4>{{{vgx::CMD_SHADER,0},{vgx::CMD_SHADER,1},{vgx::CMD_PROGRAM,(3u<<16)|4u},{vgx::CMD_VARIANT,0}}})vgx::backend::DeferWarm(queued,c);
        vgx::backend::SetWarmBudget(1000);vgx::backend::RunWarm();
        Require(vgx::backend::WarmPending()==0 && vgx::backend::WarmTotal()==23,"real shader/program/variant queue retires all weighted units");
#if VGX_COLDTRACE
        vgx::coldtrace::g_recorder.RequestFreeze();const auto view=vgx::coldtrace::g_recorder.Frozen();
        unsigned shaders=0,programs=0,variants=0,compiles=0,links=0;
        for(size_t i=0;i<view.data->worstCount;++i){const auto& item=view.data->worst[i];shaders+=item.key.kind==vgx::CMD_SHADER;programs+=item.key.kind==vgx::CMD_PROGRAM;variants+=item.key.kind==vgx::CMD_VARIANT;compiles+=item.calls[size_t(vgx::coldtrace::Phase::Compile)];links+=item.calls[size_t(vgx::coldtrace::Phase::Link)];}
        Require(shaders==2 && programs==1 && variants==1 && compiles>=2 && links>=1,"real warm items retain shader/program identities and nested compile/link phases");
#endif
    } else if (mode == "queue") {
        vgx::CmdBuf queued; queued.variants.push_back(variant);
        vgx::backend::DeferWarm(queued, {vgx::CMD_VARIANT,0});
        vgx::backend::DeferWarm(queued, {vgx::CMD_VARIANT,0});
        vgx::backend::SetWarmBudget(1000); vgx::backend::RunWarm();
        Require(vgx::backend::WarmPending() == 0 && vgx::backend::WarmTotal() == 2,
                "deduplicated warm work still retires every weighted queue unit");
        expected = dedup ? 1 : 2;
#if VGX_COLDTRACE
        vgx::coldtrace::g_recorder.RequestFreeze();const auto view=vgx::coldtrace::g_recorder.Frozen();unsigned items=0;
        for(size_t i=0;i<view.data->worstCount;++i)items+=view.data->worst[i].key.kind==vgx::CMD_VARIANT;
        Require(items==2,"real RunWarm records both retired items even when one redundant draw is skipped");
#endif
    } else Require(false, "known warm-variant scenario");
    if(observedWarm==~0u)observedWarm=vgx::backend::C.warmDraws;
    Require(observedWarm == expected, "only unseen compatible variants execute empty warm draws");
    Require(glGetError() == GL_NO_ERROR, "real GL pipeline stays valid");
#if VGX_COLDTRACE
    vgx::coldtrace::g_recorder.RequestFreeze();const auto trace=vgx::coldtrace::g_recorder.Frozen();
    Require(trace.data && !trace.clockErrors,"real backend trace freezes without a clock error");
    for(size_t i=0;i<trace.data->worstCount;++i){const auto& item=trace.data->worst[i];uint64_t sum=item.selfUs;for(auto n:item.exclusiveUs)sum+=n;Require(sum==item.totalUs,"real backend timing is an exclusive partition, not a sum of nested inclusive clocks");}
#endif
    std::printf("PASS: warm variants %s dedup=%u warm-draws=%u GL-calls=%u (Mesa, probe=%d, diagnostics=%u, not Vita FPS/pixels)\n",
                mode.c_str(), dedup, observedWarm, vgx::backend::C.glCalls,VGX_COLDTRACE,vgx::backend::kBackendDiagnostics);
}
