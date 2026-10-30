// Real copy/allocation control flow; only the external GL and unrelated drawing helpers are faked.
// A dropped storage check or premature replacement makes these tests draw without storage or lose the last good picture.
#include "vgx_cmd.h"
#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cmath>

using GLuint = unsigned;
using GLenum = unsigned;
constexpr GLenum GL_TEXTURE_2D=1, GL_RGBA=2, GL_UNSIGNED_BYTE=3, GL_LINEAR=4, GL_NEAREST=5,
    GL_FRAMEBUFFER=6, GL_DEPTH_COMPONENT=7, GL_UNSIGNED_INT=8, GL_DEPTH_ATTACHMENT=9,
    GL_RENDERBUFFER=10, GL_DEPTH_COMPONENT24_OES=11, GL_DEPTH_COMPONENT16=12, GL_COLOR_BUFFER_BIT=13;
#define G(x) (x)
#undef VGX_LOG
#undef VGX_LOG_N
#define VGX_LOG(...) ((void)0)
#define VGX_LOG_N(...) ((void)0)
#define GPUPASS_NOTE(...) ((void)0)
#define __VGX_VITA__ 1
namespace vgx::autoq {
unsigned failureLogs=0;
std::string lastFailureLog;
void Log(const char* format, ...) {
    ++failureLogs;
    char line[512];
    va_list args; va_start(args,format);
    std::vsnprintf(line,sizeof line,format,args); va_end(args);
    lastFailureLog=line;
}
}
namespace vgx::backend {
namespace dplan { bool g_valid=true; }
#include "copy-alloc-types.inc"
struct Storage { int w=0, h=0; bool valid=false; };
std::map<GLuint, Storage> storage;
std::map<GLuint, GLuint> attached;
std::vector<Tex> g_tex;
std::vector<PoolFbo> g_pool;
struct { GLuint fbo=1; } S;
struct { unsigned textures=0; } g_stats;
struct { unsigned texDeletes=0, copyQuads=0, clears=0; } C;
GLuint g_inUseFbo=1, boundTex=0, nextTex=100, nextFbo=200;
GLuint g_efbFbo=1, g_efbColor=2, g_efbDepthTex=3, g_whiteTex=4, g_passDirectTex=0;
int g_efbW=640, g_efbH=456, g_rtW=640, g_rtH=456, g_pass=-1;
uint32_t g_fboMaxUses=8;
uint64_t g_presents=1;
float g_passScale=1.0f;
bool g_hasDepthTex=false, g_hasDepth24=false, g_inPass=false, g_rsValid=true, g_fbDirty=false;
bool failStorage=false, failName=false, failFramebuffer=false;
unsigned missingDraws=0, draws=0, allocations=0, storageChecks=0, clearCalls=0, guardReads=0;

long EnvInt(const char* name, long fallback) {
    if (std::string(name)=="GXV_RT_ALLOC_GUARD") ++guardReads;
    const char* e=std::getenv(name); return e ? std::strtol(e,nullptr,10) : fallback;
}
long DiagnosticInt(const char*, long fallback) { return fallback; }
void glGenTextures(int, GLuint* id) { *id=failName ? 0 : nextTex++; }
void glGenFramebuffers(int, GLuint* id) { *id=failFramebuffer ? 0 : nextFbo++; }
void glDeleteTextures(int, const GLuint* id) { storage.erase(*id); }
void glGenRenderbuffers(int, GLuint* id) { *id=nextTex++; }
void glBindRenderbuffer(GLenum, GLuint) {}
void glRenderbufferStorage(GLenum, GLenum, int, int) {}
void glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint) {}
void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, int) {}
void BindTexForEdit(GLuint id) { boundTex=id; }
void ForgetTex(GLuint) {}
void glTexImage2D(GLenum, int, GLenum, int w, int h, int, GLenum, GLenum, const void*) {
    ++allocations; storage[boundTex]={w,h,!failStorage && boundTex!=0};
}
void* vglGetTexDataPointer(GLenum) { ++storageChecks; return storage[boundTex].valid ? &storage[boundTex] : nullptr; }
void SetupNewTexture(GLenum) {}
void BindFbo(GLuint fbo) { S.fbo=fbo; }
void AttachColor(GLuint id) { attached[S.fbo]=id; }
GLuint TakeRtSpare(int,int) { return 0; }
void EfbBufferBack(Tex& t) { t=Tex{}; }
float ScaleX() { return static_cast<float>(g_rtW)/g_efbW; }
float ScaleY() { return static_cast<float>(g_rtH)/g_efbH; }
int Rnd(float x) { return static_cast<int>(x+0.5f); }
void SetViewport(int,int,int,int) {}
void SetScissor(bool) {}
void SetBlendOff() {}
void SetDepthTest(bool) {}
void SetCull(int) {}
void SetDepthMask(bool) {}
void SetColorMask(bool,bool) {}
void TouchTarget() {}
void glClearColor(float,float,float,float) {}
void glClear(GLenum) { if (!storage[attached[S.fbo]].valid) ++missingDraws; }
void Clear(const ClearSpec&, const EfbRect&) { ++clearCalls; }
void DrawQuad(GLuint,float,float,float,float,const float*,const float*) {
    ++draws; if (!S.fbo || !storage[attached[S.fbo]].valid) ++missingDraws;
}
bool SanitizeCopyOnlyClear(const CopyTexCmd&) { return false; } // (GXV_SANITIZE is not part of this contract)
bool EfbSwap(const CopyTexCmd&) { return false; } // RGB565's forced-alpha conversion cannot use the identity-only swap.
#include "copy-alloc-production.inc"
}

namespace {
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
vgx::CopyTexCmd Copy(int w=640,int h=456) {
    vgx::CopyTexCmd c;
    c.texId=9; c.src={0,0,640,456}; c.dstW=w; c.dstH=h;
    c.mat[0][0]=c.mat[1][1]=c.mat[2][2]=1;
    c.add[3]=1; // WaterCameraFilter's RGB565 capture.
    return c;
}
}
int main(int argc,char** argv) {
    using namespace vgx::backend;
    Check(argc==2,"scenario required");
    const std::string scenario=argv[1];
    if (scenario=="legacy" || scenario=="fill") {
        failStorage=true; auto c=Copy(); c.fill=scenario=="fill"; CopyTex(c);
        Check((scenario=="fill" ? C.clears==1 : draws==1) && missingDraws==1 && storageChecks==0,
              "default/off and cached fill copies preserve the existing allocation path");
        Check(vgx::autoq::failureLogs==0,"legacy and excluded fill copies do not emit guard diagnostics");
    } else if (scenario=="first" || scenario=="name" || scenario=="framebuffer") {
        failStorage=scenario=="first"; failName=scenario=="name"; failFramebuffer=scenario=="framebuffer";
        CopyTex(Copy());
        Check(draws==0 && missingDraws==0,"initial allocation failure must not attach/draw a missing target");
        Check(g_tex[9].id==0 && g_pool.empty(),"failed initial capture publishes neither texture nor framebuffer");
        Check(clearCalls==1 && S.fbo==g_efbFbo,"failed capture preserves its following clear and EFB binding");
        Check(!dplan::g_valid,"failed capture invalidates the speculative depth plan");
        failStorage=failName=failFramebuffer=false; CopyTex(Copy());
        Check(draws==1 && g_tex[9].id && storage[g_tex[9].id].valid,"initial failure can recover on the next capture");
    } else if (scenario=="fill_bad_framebuffer" || scenario=="fill_bad_framebuffer_new") {
        failFramebuffer=true; auto fill=Copy(); fill.fill=true; CopyTex(fill);
        Check(g_pool.size()==1 && g_pool[0].fbo==0,"excluded fill establishes invalid pooled FBO name");
        const Tex old=g_tex[9];
        auto ordinary=Copy();
        if (scenario=="fill_bad_framebuffer_new") ordinary.texId=10;
        CopyTex(ordinary);
        Check(draws==0 && missingDraws==0 && g_pool.size()==1,
              "guarded ordinary copy must not reuse FBO zero while allocation still fails");
        Check(g_tex[9].id==old.id && storage[old.id].valid && S.fbo==g_efbFbo,
              "rejected pooled FBO retains existing storage and restores EFB");
        failFramebuffer=false;
        CopyTex(ordinary);
        Check(draws==1 && missingDraws==0 && g_pool.size()==2 && g_pool.back().fbo!=0,
              "guarded ordinary copy recovers with a valid FBO instead of reusing zero");
    } else if (scenario=="fill_then_copy") {
        failStorage=true; auto fill=Copy(); fill.fill=true; CopyTex(fill);
        Check(missingDraws==1 && !storage[g_tex[9].id].valid,"fixture enters with the excluded fill path's invalid texture record");
        missingDraws=0; CopyTex(Copy());
        Check(draws==0 && missingDraws==0,"ordinary same-size copy refuses invalid storage published by excluded fill path");
        failStorage=false; CopyTex(Copy());
        Check(draws==1 && missingDraws==0 && storage[g_tex[9].id].valid,"ordinary copy replaces invalid fill storage once memory returns");
    } else if (scenario=="preexisting") {
        CopyTex(Copy()); storage[g_tex[9].id].valid=false;
        failStorage=true; CopyTex(Copy());
        Check(draws==1 && missingDraws==0,"same-size metadata is not proof of valid storage after another allocation path fails");
        failStorage=false; CopyTex(Copy());
        Check(draws==2 && missingDraws==0 && storage[g_tex[9].id].valid,"invalid preexisting capture recovers when allocation resumes");
    } else if (scenario=="framebuffer_existing") {
        CopyTex(Copy()); const Tex old=g_tex[9]; const auto n=allocations;
        g_pool[0].uses=g_fboMaxUses; failFramebuffer=true;
        CopyTex(Copy());
        Check(draws==1 && allocations==n && g_tex[9].id==old.id && storage[old.id].valid,
              "same-size capture with exhausted FBO pool preserves storage when new FBO name fails");
        failFramebuffer=false; CopyTex(Copy());
        Check(draws==2 && missingDraws==0,"existing storage recovers after framebuffer pressure");
    } else {
        auto original=Copy(); CopyTex(original);
        const Tex old=g_tex[9];
        const unsigned before=draws;
        auto resized=Copy(320,228);
        if (scenario=="atlas") { resized.dstTexW=640; resized.dstTexH=480; resized.dstX=32; resized.dstY=16; }
        if (scenario=="atlas_source") { resized.srcTex=9; resized.srcTexW=640; resized.srcTexH=456; resized.texId=10; }
        if (scenario=="efb") g_tex[9].efbBuf=true;
        if (scenario=="pass") {
            g_pass=0; g_inPass=true; g_passDirectTex=9;
            S.fbo=g_pool[0].fbo; g_inUseFbo=S.fbo;
            resized.texId=10; resized.fromPass=true; resized.passMore=true;
        }
        failStorage=true;
        const unsigned clearsBefore=clearCalls;
        CopyTex(resized);
        Check(draws==before && missingDraws==0,"failed replacement must not submit a copy draw");
        Check(g_tex[9].id==old.id && g_tex[9].w==640 && g_tex[9].h==456 && storage[old.id].valid,
              "failed replacement preserves the last valid texture and dimensions");
        Check(!dplan::g_valid,"failed replacement falls back to conservative depth hints");
        if (scenario=="efb") Check(g_tex[9].efbBuf,"failed replacement retains EFB-buffer ownership");
        if (scenario=="atlas_source") Check(clearCalls==clearsBefore && S.fbo==g_efbFbo,"failed atlas-source copy does not invent a clear");
        if (scenario=="pass") {
            Check(g_inPass && g_pass==0 && S.fbo==g_pool[0].fbo && clearCalls==clearsBefore,
                  "failed nonterminal pass copy keeps source pass bound and open without a clear");
            resized.passMore=false; CopyTex(resized);
            Check(!g_inPass && g_pass==-1 && S.fbo==g_efbFbo,"failed terminal pass copy restores EFB");
        }
        if (scenario=="pressure") {
            const size_t live=storage.size();
            for(int i=0;i<100;++i) CopyTex(resized);
            Check(draws==before && storage.size()==live && g_tex[9].id==old.id,"repeated failures neither leak candidates nor retire valid target");
            Check(vgx::autoq::failureLogs==4,"player-summary allocation diagnostics stop after four failures");
        }
        failStorage=false; failFramebuffer=true;
        CopyTex(resized);
        Check(draws==before && g_tex[9].id==old.id,"framebuffer allocation failure preserves last valid texture");
        failFramebuffer=false; CopyTex(resized);
        Check(draws==before+1 && missingDraws==0,"replacement capture recovers without a missing-storage draw");
        const unsigned after=allocations;
        CopyTex(resized);
        Check(allocations==after,"same-sized successful captures reuse storage");
        setenv("GXV_RT_ALLOC_GUARD","0",1);
        failStorage=true; CopyTex(Copy(160,114));
        Check(missingDraws==0 && guardReads==1,"guard is read once and cannot change mid-run");
    }
    if (scenario!="legacy" && scenario!="fill") {
        Check(vgx::autoq::failureLogs>0 && vgx::autoq::failureLogs<=4,"guarded failures reach the bounded player-summary logger");
        Check(vgx::autoq::lastFailureLog.find("GXV_RT_ALLOC_GUARD: copy texture ")!=std::string::npos &&
              vgx::autoq::lastFailureLog.find("previous capture kept when available")!=std::string::npos,
              "failure breadcrumb identifies the guard without promising an initial capture");
    }
    std::printf("PASS: copy allocation %s (draws=%u, missing-storage draws=%u, failure logs=%u)\n",scenario.c_str(),draws,missingDraws,vgx::autoq::failureLogs);
}
