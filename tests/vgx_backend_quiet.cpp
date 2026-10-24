// Exercise stale diagnostic settings against real Mesa and the Vita stdio wrappers.
extern "C" { unsigned char vgxvita_maskGuardDue = 0; }
#include "../platform/vgx/src/vgx_backend_gl.cpp"
#include <EGL/egl.h>
#include <array>
#include <sys/stat.h>
#include <unistd.h>

namespace vgx { bool LogEnabled() { return false; } }
namespace {
unsigned finishCalls = 0, actualDraws = 0, diagnosticReads = 0, pixelReads = 0, unexpectedEnvReads = 0;
unsigned errorCallsDuringExecute = 0;
bool executing = false;
vgx::backend::Counts frameCounts;
constexpr const char* diagnosticNames[] = {
    "GXV_PROF", "GXV_SCENELOG", "GXV_PASSTIME", "GXV_PASSTIME_AT", "GXV_DRAWTIME", "GXV_DRAWTIME_AT",
    "GXV_GLSTATS", "GXV_STATS_RAM", "GXV_CONSTATTR_LOG", "GXV_VALIDATE", "GXV_CUT", "GXV_PROBE", "GXV_SIM_COMPILE_MS", "GXV_SIM_CACHE",
    "GXV_GLMEM", "GXV_PROGRESS_DUMP", "GXV_SIM_COMPILE_SLEEP"
};
constexpr const char* functionalNames[] = {
    "GXV_VITA", "GXV_DXT1", "GXV_MIPGEN", "GXV_MIPCLAMP", "GXV_LA88", "GXV_DEPTH_SKIP", "GXV_DEPTH_TEX",
    "GXV_S3TC", "GXV_QUADCLEAR", "GXV_GLCACHE", "GXV_FBO_USES", "GXV_STATIC_USAGE", "GXV_EFB0", "GXV_KEEP_RT",
    "GXV_MAXPROGRAMS", "GXV_WARM_DEDUP", "GXV_WARM_BACKOFF", "GXV_WARM_BACKOFF_MS", "GXV_PROGRESS",
    "GXV_PROGRESS_STARTUP", "GXV_PROGRESS_AFTER_MS", "GXV_PASSDIRECT", "GXV_COLDTRACE", "GXV_UNIFORM_REUSE"
};
void Require(bool good, const char* why) {
    if (good) return;
    ::write(STDERR_FILENO, "FAIL: ", 6); ::write(STDERR_FILENO, why, std::strlen(why));
    ::write(STDERR_FILENO, "\n", 1); std::exit(1);
}
bool Exists(const char* path) { struct stat value{}; return ::stat(path, &value) == 0; }
void Context() {
    const EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    Require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "surfaceless EGL initialized");
    const EGLint attrs[] = {EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,
                           EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig config; EGLint count;
    Require(eglChooseConfig(display, attrs, &config, 1, &count) && count == 1, "RGBA8 pbuffer config");
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE}, size[] = {EGL_WIDTH,32,EGL_HEIGHT,32,EGL_NONE};
    const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    const auto surface = eglCreatePbufferSurface(display, config, size);
    Require(context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE && eglMakeCurrent(display,surface,surface,context),
            "real GLES2 context current");
}
GLuint Shader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type); glShaderSource(shader,1,&source,nullptr); glCompileShader(shader);
    GLint good = 0; glGetShaderiv(shader,GL_COMPILE_STATUS,&good); Require(good,"synthetic shader compiled"); return shader;
}
void Program(uint16_t vs, uint16_t fs) {
    const GLuint vertex = Shader(GL_VERTEX_SHADER,"attribute vec4 a_pos; uniform mat4 u_proj; void main(){gl_Position=u_proj*a_pos;}");
    const GLuint fragment = Shader(GL_FRAGMENT_SHADER,"precision mediump float; void main(){gl_FragColor=vec4(0.25,0.75,0.5,1.0);}");
    const GLuint id = glCreateProgram(); glAttachShader(id,vertex); glAttachShader(id,fragment);
    glBindAttribLocation(id,vgx::AT_POS,"a_pos"); glLinkProgram(id);
    GLint good = 0; glGetProgramiv(id,GL_LINK_STATUS,&good); Require(good,"synthetic program linked");
    glDeleteShader(vertex); glDeleteShader(fragment);
    vgx::backend::Program p; p.id=id; p.ok=true; p.proj=glGetUniformLocation(id,"u_proj"); p.layoutNeed=vgx::VL_NRM;
    vgx::backend::g_programs.emplace((uint32_t(vs)<<16)|fs,std::move(p));
}
vgx::CmdBuf Triangle() {
    vgx::CmdBuf b; b.frame=10;
    const float positions[][4] = {{-0.8f,-0.8f,0,1},{0.8f,-0.8f,0,1},{0,0.8f,0,1}};
    b.vtx.resize(sizeof positions); std::memcpy(b.vtx.data(),positions,sizeof positions); b.idx={0,1,2};
    b.uni={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    vgx::DrawRec d; d.vsId=1; d.idxCount=d.vtxCount=3; d.ublock[vgx::UB_PROJ]=0;
    b.draws.assign(257,d);
    for (unsigned i=0;i<b.draws.size();++i) b.cmds.push_back({vgx::CMD_DRAW,i});
    b.copyDisp.push_back({{0,0,32,32},{},false}); b.cmds.push_back({vgx::CMD_COPY_DISP,0});
    b.cmds.push_back({vgx::CMD_PRESENT,0}); return b;
}
using Pixels = std::array<uint8_t,32*32*4>;
Pixels Read() {
    Pixels pixels{}; glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    Require(glGetError()==GL_NO_ERROR,"real GL pipeline succeeds"); return pixels;
}
void UniformBlocks(vgx::CmdBuf b, bool reuse) {
    using namespace vgx;
    using namespace vgx::backend;
    const auto vs=Shader(GL_VERTEX_SHADER,
        "attribute vec4 a_pos;uniform mat4 u_proj;uniform vec4 u_mv[1],u_tm[1],u_ptm[1],u_pal[1],u_lt[1];"
        "void main(){gl_Position=u_proj*a_pos+0.01*(u_mv[0]+u_tm[0]+u_ptm[0]+u_pal[0]+u_lt[0]);}");
    const auto fs=Shader(GL_FRAGMENT_SHADER,
        "precision mediump float;uniform vec4 u_tev[1];void main(){gl_FragColor=vec4(0.25,0.75,0.5,1.0)+0.01*u_tev[0];}");
    const auto id=glCreateProgram(); glAttachShader(id,vs); glAttachShader(id,fs);
    glBindAttribLocation(id,AT_POS,"a_pos"); glLinkProgram(id);
    GLint ok=0; glGetProgramiv(id,GL_LINK_STATUS,&ok); Require(ok,"all-block uniform fixture links");
    glDeleteShader(vs); glDeleteShader(fs);
    vgx::backend::Program p; p.id=id; p.ok=true;
    p.proj=glGetUniformLocation(id,"u_proj"); p.mv=glGetUniformLocation(id,"u_mv[0]");
    p.tm=glGetUniformLocation(id,"u_tm[0]"); p.ptm=glGetUniformLocation(id,"u_ptm[0]");
    p.pal=glGetUniformLocation(id,"u_pal[0]"); p.lt=glGetUniformLocation(id,"u_lt[0]"); p.tev=glGetUniformLocation(id,"u_tev[0]");
    Require(p.proj!=-1 && p.mv!=-1 && p.tm!=-1 && p.ptm!=-1 && p.pal!=-1 && p.lt!=-1 && p.tev!=-1,"every fixture uniform is active");
    p.nMv=p.nTm=p.nPtm=p.nPal=p.nLt=p.nTev=1; p.vsBytes=144; p.fsBytes=16;
    g_programs.emplace(3u<<16,p);
    b.uni.clear(); std::array<uint32_t,UB_COUNT> offsets{};
    for (int block=0;block<UB_COUNT;++block) {
        offsets[block]=b.uni.size(); b.uni.resize(b.uni.size()+kUbSize[block]);
    }
    const auto projection=offsets[UB_PROJ];
    for (unsigned i=0;i<4;++i) b.uni[projection+5*i]=1;
    const auto original=b.uni;
    b.uni.insert(b.uni.end(),original.begin(),original.end());
    const uint32_t duplicate=original.size();
    auto draw=b.draws[0]; draw.vsId=3;
    std::memcpy(draw.ublock,offsets.data(),sizeof draw.ublock);
    ++g_serial; C=Counts{}; Draw(b,draw);
    Require(C.uniformCalls==7 && C.ringBytes==160,"first draw applies every active matrix/vector block");
    for (auto& offset:draw.ublock) offset+=duplicate;
    Draw(b,draw);
    Require(C.uniformCalls==(reuse?7u:14u) && C.ringBytes==(reuse?160u:320u),"identical blocks reuse exact bytes or retain offset-only control uploads");
    // A new offset with changes only outside the shader's clamped prefix is equivalent.
    const uint32_t unused=b.uni.size();
    b.uni.insert(b.uni.end(),original.begin(),original.end());
    for (int block=UB_MV;block<UB_COUNT;++block) b.uni[unused+offsets[block]+4]=9;
    for (int block=0;block<UB_COUNT;++block) draw.ublock[block]=unused+offsets[block];
    Draw(b,draw);
    Require(C.uniformCalls==(reuse?7u:21u),"unused array tails do not force reuse-path uploads");
    // Post matrices are a separate region, not the first texture-matrix prefix.
    const uint32_t post=b.uni.size(); b.uni.insert(b.uni.end(),original.begin(),original.end());
    b.uni[post+offsets[UB_TEXM]+96]=0.5f; draw.ublock[UB_TEXM]=post+offsets[UB_TEXM];
    Draw(b,draw);
    Require(C.uniformCalls==(reuse?8u:23u),"post-only change uploads one region; reference control retains both");
    Require(C.ringBytes==(reuse?304u:624u),"post-only change still dirties one complete vertex uniform buffer, not both shader stages");
    float actual[4]; glGetUniformfv(id,p.ptm,actual);
    Require(actual[0]==0.5f,"post-matrix change reaches real GL");
    glGetUniformfv(id,p.tm,actual);
    Require(actual[0]==0,"post-only update leaves texture matrices unchanged");
    const uint32_t tex=b.uni.size();
    b.uni.insert(b.uni.end(),original.begin(),original.end());
    b.uni[tex+offsets[UB_TEXM]]=0.25f;
    b.uni[tex+offsets[UB_TEXM]+96]=0.5f;
    draw.ublock[UB_TEXM]=tex+offsets[UB_TEXM];
    Draw(b,draw);
    Require(C.uniformCalls==(reuse?9u:25u),"texture-only change retains independent post matrix or offset-only reference");
    glGetUniformfv(id,p.tm,actual); Require(actual[0]==0.25f,"texture-only change reaches real GL");
    glGetUniformfv(id,p.ptm,actual); Require(actual[0]==0.5f,"texture-only update retains post-matrix state");
    const uint32_t both=b.uni.size();
    b.uni.insert(b.uni.end(),original.begin(),original.end());
    b.uni[both+offsets[UB_TEXM]]=0.75f;
    b.uni[both+offsets[UB_TEXM]+96]=0.125f;
    draw.ublock[UB_TEXM]=both+offsets[UB_TEXM];
    Draw(b,draw);
    Require(C.uniformCalls==(reuse?11u:27u),"two changed regions still upload both matrices");
    glGetUniformfv(id,p.tm,actual); Require(actual[0]==0.75f,"combined change updates texture matrices");
    glGetUniformfv(id,p.ptm,actual); Require(actual[0]==0.125f,"combined change updates post matrices");
    const uint32_t zero=b.uni.size(); b.uni.insert(b.uni.end(),original.begin(),original.end());
    b.uni[zero+offsets[UB_TEV]]=-0.0f; draw.ublock[UB_TEV]=zero+offsets[UB_TEV];
    Draw(b,draw); Require(C.uniformCalls==(reuse?12u:28u),"signed-zero payloads are compared bit-exactly");
    g_programs.at(3u<<16).applied[UB_PAL]=0xfffffff0u;
    Draw(b,draw); Require(C.uniformCalls==(reuse?13u:29u),"invalid previous arena offset fails closed without an out-of-bounds read");
    const auto before=C.uniformCalls;
    g_fbDirty=true; Draw(b,draw);
    Require(C.uniformCalls==before+7,"scene transition re-uploads every named block regardless of byte equality");
    const auto freshCalls=C.uniformCalls;
    ++g_serial; Draw(b,draw);
    auto other=b; Draw(other,draw); Draw(b,draw);
    Require(C.uniformCalls==freshCalls+21,"new serial and changing arena identity re-upload both matrix regions and all other blocks");
    auto& live=g_programs.at(3u<<16);
    const auto halfCalls=C.uniformCalls;
    live.tm=-1; ++g_serial; Draw(b,draw);
    Require(C.uniformCalls==halfCalls+6,"post-only active program still uploads its matrix on a fresh frame");
    live.tm=p.tm; live.ptm=-1; ++g_serial; Draw(b,draw);
    Require(C.uniformCalls==halfCalls+12,"texture-only active program still uploads its matrix on a fresh frame");
    live.ptm=p.ptm; ++g_serial; Draw(b,draw);
    Require(C.uniformCalls==halfCalls+19,"restoring both active regions restores both fresh-frame uploads");
    Require(glGetError()==GL_NO_ERROR,"all-block reuse leaves the real GL pipeline valid");
}
#if VGX_COLDTRACE
uint64_t TraceClock() noexcept { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint32_t TraceThread() noexcept { return 1; }
#endif
}
extern "C" char* __real_getenv(const char*);
extern "C" char* __wrap_getenv(const char* name) {
    for (const char* diagnostic : diagnosticNames) if (!std::strcmp(name,diagnostic)) { ++diagnosticReads; break; }
    if (!std::strncmp(name,"GXV_",4) || !std::strncmp(name,"VGX_",4)) {
        bool functional=false;
        for (const auto* allowed:functionalNames) if (!std::strcmp(name,allowed)) { functional=true; break; }
        if (!functional) ++unexpectedEnvReads;
    }
    return __real_getenv(name);
}
extern "C" void __real_glFinish();
extern "C" void __wrap_glFinish() { ++finishCalls; __real_glFinish(); }
extern "C" GLenum __real_glGetError();
extern "C" GLenum __wrap_glGetError() {
    if (executing) { ++errorCallsDuringExecute; frameCounts=vgx::backend::C; }
    return __real_glGetError();
}
extern "C" void __real_glReadPixels(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*);
extern "C" void __wrap_glReadPixels(GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,void* data) {
    ++pixelReads; __real_glReadPixels(x,y,w,h,format,type,data);
}
extern "C" void __real_glDrawElements(GLenum,GLsizei,GLenum,const void*);
extern "C" void __wrap_glDrawElements(GLenum mode,GLsizei count,GLenum type,const void* indices) {
    ++actualDraws; __real_glDrawElements(mode,count,type,indices);
}
int main(int argc, char** argv) {
    Require(argc==2,"supply one diagnostic scenario"); const std::string mode=argv[1];
    for (const auto* name : diagnosticNames) unsetenv(name);
    for (const auto* name : functionalNames) unsetenv(name);
    const char* setting=nullptr; const char* value=nullptr;
    if(mode=="drawtime") { setting="GXV_DRAWTIME"; value="9-11"; }
    else if(mode=="draw-at") { setting="GXV_DRAWTIME_AT"; value="0,1"; }
    else if(mode=="passtime") { setting="GXV_PASSTIME"; value="10"; }
    else if(mode=="pass-at") { setting="GXV_PASSTIME_AT"; value="0,1"; }
    else if(mode=="prof1" || mode=="prof2" || mode=="prof3") { setting="GXV_PROF"; value=mode.c_str()+4; }
    else if(mode=="csv" || mode=="csv-ram") { setting="GXV_GLSTATS"; value="/tmp/backend-quiet.csv"; }
    else if(mode=="constattr") { setting="GXV_CONSTATTR_LOG"; value="1"; }
    else if(mode=="scene") { setting="GXV_SCENELOG"; value="10"; }
    else if(mode=="validate") { setting="GXV_VALIDATE"; value="10"; }
    else if(mode=="cut") { setting="GXV_CUT"; value="0,1,1,1"; }
    else if(mode=="probe") { setting="GXV_PROBE"; value="16,16,10"; }
    else Require(mode=="baseline" || mode=="lifecycle" || mode=="simulate" || mode=="functional" || mode=="uniform-reuse" || mode=="uniform-control","known diagnostic scenario");
    if (setting) setenv(setting,value,1);
    if (mode=="csv-ram") setenv("GXV_STATS_RAM","1",1);
    if (mode=="uniform-control") setenv("GXV_UNIFORM_REUSE","0",1);
    if(mode=="simulate") { setenv("GXV_SIM_COMPILE_MS","208",1); setenv("GXV_SIM_CACHE","/tmp/backend-sim-cache",1); }
    if(mode=="functional") { setenv("GXV_GLCACHE","0",1); setenv("GXV_DXT1","0",1); setenv("GXV_WARM_DEDUP","0",1); setenv("GXV_LA88","0",1); }
    ::unlink("/tmp/backend-quiet.csv");
    setenv("GXV_MAXPROGRAMS","1",1);
    Context();
#if VGX_COLDTRACE
    Require(vgx::coldtrace::g_recorder.Arm(TraceClock,TraceThread,1),"RAM-only tracing arms independently of diagnostics");
#endif
    Require(vgx::backend::Init(32,32),"real backend initialized"); Program(1,0);
    using namespace vgx::backend;
    bool validationEnabled=bool(g_validation);
    const bool cacheOff=g_noCache, dxt1=GetCaps().dxt1;
    Require(mode=="functional" ? cacheOff && !dxt1 : !cacheOff && dxt1,
            "functional graphics overrides differ from their enabled-by-default controls");
    Require(GetCaps().la88==(mode!="functional"),"Vita enables compact textures by default and retains the explicit off switch");
    if(mode=="simulate") { g_sw=ShaderWork{}; SimulateCompile("synthetic shader A"); SimulateCompile("synthetic shader A"); }
    const double simulated=g_sw.simPending; const auto simHits=g_sw.simHits;
    auto b=Triangle();
    if (mode=="uniform-reuse" || mode=="uniform-control") {
        const auto identity=b.uni;
        b.uni.insert(b.uni.end(),identity.begin(),identity.end());
        b.uni.insert(b.uni.end(),identity.begin(),identity.end());
        b.uni[32+12]=0.25f;
        for (size_t i=0;i<b.draws.size();++i) b.draws[i].ublock[vgx::UB_PROJ]=(i&1)?16:0;
        g_programs.at(1u<<16).vsBytes=64;
    }
    finishCalls=actualDraws=pixelReads=errorCallsDuringExecute=0; C=Counts{};
    executing=true; Execute(b); executing=false;
    if (mode=="csv-ram") {
        Require(!Exists("/tmp/backend-quiet.csv"),"backend RAM counters perform no file I/O during play");
        Require(ExportCountLog()==0,"explicit paused backend export succeeds");
    }
    Require(errorCallsDuringExecute==1 && frameCounts.glCalls>0,"first-frame GL counts were actually captured exactly once");
    validationEnabled=validationEnabled || bool(g_validation);
    const auto firstCounts=frameCounts; const auto firstReads=pixelReads;
    const auto firstFinishes=finishCalls, firstDraws=actualDraws;
    const auto pixels=Read();
    if (mode=="uniform-control") Require(firstCounts.uniformCalls==257 && firstCounts.ringBytes==257*64,
            "uniform reuse kill switch retains the offset-only reference path");
    if (mode=="uniform-reuse") {
        Require(firstCounts.uniformCalls==1 && firstCounts.ringBytes==64,
                "byte-identical uniform payloads at different frame offsets upload and dirty the ring only once");
        auto draw=b.draws[0];
        C=Counts{}; draw.ublock[vgx::UB_PROJ]=32; Draw(b,draw);
        Require(C.uniformCalls==1 && C.ringBytes==64,"changed uniform contents still upload");
        float actual[16]; const auto program=g_programs.at(1u<<16).id;
        glGetUniformfv(program,g_programs.at(1u<<16).proj,actual);
        Require(actual[12]==0.25f,"changed payload reaches the real GL program");
        draw.ublock[vgx::UB_PROJ]=vgx::kNoBlock; Draw(b,draw);
        draw.ublock[vgx::UB_PROJ]=32; Draw(b,draw);
        Require(C.uniformCalls==1,"unnamed block does not forget a still-applied same-frame value");
        ++g_serial; Draw(b,draw);
        Require(C.uniformCalls==2,"new frame never compares against the preceding arena");
        ++g_serial; draw.ublock[vgx::UB_PROJ]=vgx::kNoBlock; Draw(b,draw);
        draw.ublock[vgx::UB_PROJ]=0; Draw(b,draw);
        glGetUniformfv(program,g_programs.at(1u<<16).proj,actual);
        Require(C.uniformCalls==3 && actual[12]==0,"leading unnamed block cannot bless old offsets in a new frame");
        const auto uniformBeforeScene=C.uniformCalls, scenesBefore=C.sceneReuploads;
        g_fbDirty=true; Draw(b,draw);
        Require(C.uniformCalls==uniformBeforeScene+1 && C.sceneReuploads==scenesBefore+1,
                "scene boundary retains the mandatory uniform re-upload");
        auto other=b;
        other.uni[12]=other.uni[28]=0.75f;
        draw.ublock[vgx::UB_PROJ]=16; Draw(other,draw);
        glGetUniformfv(program,g_programs.at(1u<<16).proj,actual);
        Require(actual[12]==0.75f,"a second arena in the same serial cannot inherit another buffer's applied offsets");
        Draw(b,draw); glGetUniformfv(program,g_programs.at(1u<<16).proj,actual);
        Require(actual[12]==0,"returning to the first arena restores its own payload");
        const auto beforeWarm=C.uniformCalls;
        g_programs.at(1u<<16).warmCompatible=false;
        WarmVariant({1,0,0,0xc00}); Draw(b,draw);
        glGetUniformfv(program,g_programs.at(1u<<16).proj,actual);
        Require(C.uniformCalls==beforeWarm && actual[12]==0,"warm draws neither replace nor bless a live arena's uniforms");
    }
    if (mode=="uniform-reuse" || mode=="uniform-control") UniformBlocks(b,mode=="uniform-reuse");
    if(mode=="lifecycle") {
        auto& p=g_programs.at(1u<<16); const auto first=p.serial;
        Require(p.applied[vgx::UB_PROJ]==0 && p.usedSerial==g_serial,"draw records uniform offset and last-use frame");
        b.frame=11; b.uni[12]=0.4f; Execute(b);
        Require(p.serial>first && p.serial==g_serial,"uniform freshness advances across frames even at the same offset");
        Require(pixels!=Read(),"changed uniform contents at reused offset change actual pixels");
        ::Program(0,2); g_serial=7; WarmVariant({1,0,0,0xc00});
        g_serial=650; WarmVariant({0,2,0,0xc00}); UseProgram(p.id); g_serial=700; EvictPrograms();
        Require(!g_programs.count(1u<<16) && g_programs.count(2)==1,"eviction preserves recently used programs");
        Require(S.program==~0u,"evicting the bound expired program invalidates the GL cache");
        ::Program(1,0); const auto before=actualDraws; WarmVariant({1,0,0,0xc00});
        Require(actualDraws==before+1,"a replacement linked program warms again");
    }
    vgx::CmdBuf queued; queued.variants.push_back({1,0,0,0xc00});
    const auto drawsBeforeWarm=actualDraws;
    DeferWarm(queued,{vgx::CMD_VARIANT,0}); SetWarmBudget(1000); RunWarm();
    Require(WarmPending()==0 && WarmTotal()==1,"quiet build still retires deferred warm work");
    const auto warmDrawDelta=actualDraws-drawsBeforeWarm;
    if(mode=="baseline") Require(warmDrawDelta==0,"baseline skips its already-compatible warm draw");
    if(mode=="functional") Require(warmDrawDelta==1,"quiet mode preserves the requested warm-dedup override");
    Require(glGetError()==GL_NO_ERROR,"warm work leaves the GL pipeline valid");
#if VGX_COLDTRACE
    vgx::coldtrace::g_recorder.RequestFreeze(); const auto view=vgx::coldtrace::g_recorder.Frozen();
    Require(view.data && view.data->worstCount>0 && !view.clockErrors,"quiet build retains explicitly opted-in RAM tracing");
#endif
    uint64_t profCommands=0; for (const auto n:g_profCount) profCommands+=n;
    g_glStats.Close();
    if (mode=="csv-ram" && kBackendDiagnostics) {
        FILE* csv=std::fopen("/tmp/backend-quiet.csv","r");
        char header[4096], row[4096];
        Require(csv && std::fgets(header,sizeof header,csv) && std::fgets(row,sizeof row,csv),"RAM CSV has a header and complete frame row");
        Require(std::count(header,header+std::strlen(header),',')==std::count(row,row+std::strlen(row),',') &&
                header[std::strlen(header)-1]=='\n' && row[std::strlen(row)-1]=='\n',"RAM backend CSV retains the exact counter schema");
        std::fclose(csv);
    }
    std::string hex; hex.reserve(pixels.size()*2);
    const char digits[]="0123456789abcdef";
    for(const auto byte:pixels) { hex.push_back(digits[byte>>4]); hex.push_back(digits[byte&15]); }
    char result[10240];
    const int length=std::snprintf(result,sizeof result,
        "{\"mode\":\"%s\",\"finishes\":%u,\"draws\":%u,\"gl_calls\":%u,\"readbacks\":%u,\"total_finishes\":%u,\"total_draws\":%u,\"warm_draw_delta\":%u,\"cache_off\":%s,\"dxt1\":%s,\"prof_commands\":%llu,\"draw_profiler\":%s,\"scene\":%s,\"validation\":%s,\"csv\":%s,\"simulated_ms\":%.0f,\"sim_hits\":%u,\"diagnostic_env_reads\":%u,\"unexpected_env_reads\":%u,\"pixels\":\"%s\"}\n",
        mode.c_str(),firstFinishes,firstDraws,firstCounts.glCalls,firstReads,finishCalls,actualDraws,warmDrawDelta,
        cacheOff?"true":"false",dxt1?"true":"false",static_cast<unsigned long long>(profCommands),
        g_dprofOn?"true":"false",g_sceneLog?"true":"false",validationEnabled?"true":"false",
        Exists("/tmp/backend-quiet.csv")?"true":"false",simulated,simHits,diagnosticReads,unexpectedEnvReads,hex.c_str());
    Require(length>0 && size_t(length)<sizeof result,"bounded test report");
    Require(::write(STDOUT_FILENO,result,length)==length,"test report delivered independently of silent stdio");
}
