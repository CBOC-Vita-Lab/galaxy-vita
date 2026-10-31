// Offscreen GLES2 context through EGL on Mesa (surfaceless, or a 16x16
// pbuffer when the driver insists on a surface). Everything is rendered into
// FBOs by the back end.
#include "vgx_cmd.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <cstdio>
#include <cstdlib>

namespace vgx::host {

namespace {
// GXV_GLDEBUG=1 (desktop diagnostic, default off): a debug context whose KHR_debug messages (errors, undefined behaviour, performance
// warnings, medium severity and up) are written to stderr as "[vgx] GL debug" lines, synchronously, so the line names the call's
// neighbours in the log. GL errors on the Vita's vitaGL go unreported; here they are what Mesa's validation finds.
void GlDebugLine(GLenum, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* msg, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION_KHR) return;
    static int lines = 0;
    if (lines++ < 400) std::fprintf(stderr, "[vgx] GL debug: type 0x%X id %u severity 0x%X: %s\n", type, id, severity, msg);
}
bool GlDebugWanted() {
    const char* e = std::getenv("GXV_GLDEBUG");
    return e && *e && std::atoi(e) != 0;
}
void GlDebugInstall() {
    if (!GlDebugWanted()) return;
    using Callback = void (*)(GLDEBUGPROCKHR, const void*);
    const auto set = reinterpret_cast<Callback>(eglGetProcAddress("glDebugMessageCallbackKHR"));
    if (!set) { std::fprintf(stderr, "[vgx] GXV_GLDEBUG: the driver has no glDebugMessageCallbackKHR\n"); return; }
    glEnable(GL_DEBUG_OUTPUT_KHR);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR);
    set(GlDebugLine, nullptr);
    std::fprintf(stderr, "[vgx] GXV_GLDEBUG: KHR_debug messages on\n");
}
} // namespace

bool CreateContext() {
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) { VGX_ERR("eglInitialize failed (0x%X)\n", eglGetError()); return false; }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfgAttr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
                              EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &n) || n < 1) { VGX_ERR("eglChooseConfig failed\n"); return false; }
    const EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    const EGLint ctxDebugAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_CONTEXT_OPENGL_DEBUG, EGL_TRUE, EGL_NONE};
    EGLContext ctx = GlDebugWanted() ? eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxDebugAttr) : EGL_NO_CONTEXT;
    if (ctx == EGL_NO_CONTEXT) ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT) { VGX_ERR("eglCreateContext failed (0x%X)\n", eglGetError()); return false; }
    // Surfaceless: everything is rendered into FBOs.
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        const EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
        if (!eglMakeCurrent(dpy, surf, surf, ctx)) { VGX_ERR("eglMakeCurrent failed (0x%X)\n", eglGetError()); return false; }
    }
    GlDebugInstall();
    return true;
}

} // namespace vgx::host
