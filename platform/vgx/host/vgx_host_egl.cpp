// Offscreen GLES2 context through EGL on Mesa (surfaceless, or a 16x16
// pbuffer when the driver insists on a surface). Everything is rendered into
// FBOs by the back end.
#include "vgx_cmd.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

namespace vgx::host {

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
    const EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT) { VGX_ERR("eglCreateContext failed (0x%X)\n", eglGetError()); return false; }
    // Surfaceless: everything is rendered into FBOs.
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        const EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
        if (!eglMakeCurrent(dpy, surf, surf, ctx)) { VGX_ERR("eglMakeCurrent failed (0x%X)\n", eglGetError()); return false; }
    }
    return true;
}

} // namespace vgx::host
