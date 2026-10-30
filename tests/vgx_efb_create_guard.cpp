// Actual backend and GLES pixels; only native OS queries and name exhaustion are supplied.
#include <cstddef>
#include <GLES2/gl2.h>
constexpr int VGL_MEM_VRAM = 0;
size_t vglMemFree(int) { return 128u << 20; }
size_t vglMemTotal(int) { return 128u << 20; }
void* vglGetTexDataPointer(GLenum) { static unsigned char storage; return &storage; }
extern "C" { unsigned char vgxvita_maskGuardDue = 0; }
#include "../platform/vgx/src/vgx_backend_gl.cpp"
#include "../platform/vgx/src/vgx_autoq.cpp"
#include <EGL/egl.h>
#include <cassert>
#include <cstdio>
#include <string>

namespace vgx { bool LogEnabled() { return false; } }
extern "C" {
void port_log(const char*, ...) {}
void port_log_summary(const char*, ...) {}
std::uint64_t sceKernelGetProcessTimeWide() { return 1; }
SceUID sceKernelCreateSema(const char*, unsigned, int, int, const void*) { return -1; }
int sceKernelWaitSema(SceUID, int, SceUInt*) { std::abort(); }
int sceKernelSignalSema(SceUID, int) { std::abort(); }
SceUID sceKernelCreateThread(const char*, int (*)(SceSize, void*), int, SceSize, unsigned, int, const void*) { std::abort(); }
int sceKernelStartThread(SceUID, SceSize, void*) { std::abort(); }
int sceKernelDelayThread(SceUInt) { std::abort(); }
}
namespace {
unsigned fboCalls, textureCalls, failFboAt, failTextureAt, defaultClears;
void Require(bool okay, const char* message) {
    if (!okay) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void Context() {
    const EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    Require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "EGL initialized");
    const EGLint attrs[] = {EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig config; EGLint count;
    Require(eglChooseConfig(display, attrs, &config, 1, &count) && count == 1, "RGBA config");
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE}, size[] = {EGL_WIDTH,32,EGL_HEIGHT,32,EGL_NONE};
    const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    const auto surface = eglCreatePbufferSurface(display, config, size);
    Require(context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE && eglMakeCurrent(display,surface,surface,context), "context current");
}
void CheckRed(GLuint fbo) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    unsigned char pixel[4]{};
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    Require(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0, "last good EFB pixel retained");
    vgx::backend::S.fbo = ~0u;
}
}
extern "C" {
void __real_glGenFramebuffers(GLsizei, GLuint*);
void __wrap_glGenFramebuffers(GLsizei n, GLuint* ids) {
    if (++fboCalls == failFboAt) return; // The pinned native allocator leaves names unchanged on exhaustion.
    __real_glGenFramebuffers(n, ids);
}
void __real_glGenTextures(GLsizei, GLuint*);
void __wrap_glGenTextures(GLsizei n, GLuint* ids) {
    if (++textureCalls == failTextureAt) return;
    __real_glGenTextures(n, ids);
}
void __real_glClear(GLbitfield);
void __wrap_glClear(GLbitfield mask) {
    GLint fbo = -1; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    if (!fbo) ++defaultClears;
    __real_glClear(mask);
}
}
int main(int argc, char** argv) {
    using namespace vgx::backend;
    Require(argc == 3, "scenario and expected guarded behavior required");
    const std::string mode = argv[1];
    const bool expectGuard = std::atoi(argv[2]) != 0;
    setenv("GXV_AUTOQ_LEVEL", "2", 1);
    setenv("GXV_EFB0", "640x456", 1);
    setenv("GXV_DEPTH_TEX", "0", 1);
    Context();
    if (mode == "startup-framebuffer") failFboAt = 1;
    if (mode == "startup-texture") failTextureAt = 2;
    const bool initialized = Init(0, 0);
    if (mode == "startup-framebuffer" || mode == "startup-texture") {
        std::printf("mode=%s guarded=%d initialized=%d fbo=%u color=%u default_clears=%u\n",
            mode.c_str(),expectGuard,initialized,g_efbFbo,g_efbColor,defaultClears);
        Require(initialized == !expectGuard, "initialization propagates guarded target failure");
        if (expectGuard) Require(!g_efbFbo && defaultClears == 0, "failed startup never renders into display framebuffer");
        return 0;
    }
    Require(initialized, "backend initializes");
    BindFbo(g_efbFbo); SetScissor(false); SetColorMask(true,true);
    glClearColor(1,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    const GLuint oldFbo = g_efbFbo, oldColor = g_efbColor;
    const unsigned beforeClears = defaultClears;
    if (mode == "framebuffer") failFboAt = fboCalls + 2; // Preserved picture's pool object, then new EFB.
    if (mode == "texture") failTextureAt = textureCalls + 2; // Preserved picture, then new EFB colour.
    if (mode == "read-once") {
        setenv("GXV_EFB_CREATE_GUARD", expectGuard ? "0" : "1", 1);
        failFboAt = fboCalls + 2;
    }
    const bool okay = ApplyRung(5, "synthetic name exhaustion");
    std::printf("mode=%s guarded=%d accepted=%d fbo=%u old=%u size=%dx%d default_clears=%u error=%x\n",
        mode.c_str(),expectGuard,okay,g_efbFbo,oldFbo,g_rtW,g_rtH,defaultClears-beforeClears,glGetError());
    if (mode == "healthy") {
        Require(okay && g_efbFbo && g_rtW == 880 && g_rtH == 520, "healthy change succeeds");
        CheckRed(g_efbFbo);
    } else if (expectGuard) {
        Require(!okay, "name exhaustion refuses quality change");
        Require(g_efbFbo == oldFbo && g_efbColor == oldColor && g_rtW == 640 && g_rtH == 456 && g_rung == 2,
                "failed change retains valid old target, size and rung");
        Require(defaultClears == beforeClears, "failed change never clears default framebuffer");
        CheckRed(g_efbFbo);
        failFboAt = failTextureAt = 0;
        Require(ApplyRung(5, "allocation recovered"), "later change retries successfully");
        CheckRed(g_efbFbo);
    } else {
        Require(okay, "OFF retains baseline accepted change after exhausted name");
        if (mode == "framebuffer" || mode == "read-once") Require(g_efbFbo == 0 && defaultClears > beforeClears, "baseline publishes FBO zero and clears display");
        if (mode == "texture") Require(g_efbColor == 0, "baseline publishes colour texture zero");
    }
    std::puts("PASS: EFB name-allocation contract (not native GPU proof)");
}
