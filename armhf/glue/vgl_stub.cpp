// vitaGL entry points that vgx_backend_gl.cpp declares weak and calls when they exist (ARMHF_VGX_BACKEND=gl-stub with
// ARMHF_VITA_RUNTIME_FLAGS: the back end is compiled with -D__VGX_VITA__ like the console build, and the console's vitaGL has them).
// They do nothing here; what matters is that the back end takes the same branches as on the console.
// The GL entry points that answer with a value, as the driver's own symbols (see armhf/glstub/GLES2/gl2.h).
extern "C" {
unsigned vgx_stub_name_ = 0;
unsigned glCreateShader(unsigned) { return ++vgx_stub_name_; }
unsigned glCreateProgram(void) { return ++vgx_stub_name_; }
// No-GPU harness only: keep this result opaque to the backend compiler, like
// the native API. Finite driver occupancy is exercised by the residency tests.
unsigned vglGetFreeProgramSlots(void) { return 1024; }
int glGetUniformLocation(unsigned, const char*) { return 1; }
unsigned glCheckFramebufferStatus(unsigned) { return 0x8CD5; }       // GL_FRAMEBUFFER_COMPLETE
unsigned glGetError(void) { return 0; }
const unsigned char* glGetString(unsigned name) {
    return reinterpret_cast<const unsigned char*>(name == 0x1F03 ? "GL_OES_depth24 GL_EXT_texture_compression_s3tc" : name == 0x1F02 ? "vgx GL stub (no rasteriser)" : "stub");
}
unsigned vglMemFree(int) { return 64u << 20; }     // plenty of video memory
unsigned vglMemTotal(int) { return 128u << 20; }
void* vglGetTexDataPointer(unsigned) { return reinterpret_cast<void*>(1); }
}

extern "C" {
void vglSetNextSceneDepthModes(unsigned char, unsigned char) {}
void vglUseLowPrecisionFragmentOnly(unsigned char) {}
int vglFramebufferDepthTexture(unsigned) { return 0; }
}

// <psp2/gxm.h> functions the back end's __VGX_VITA__ part calls (GXV_VRAM_GUARD's GPU finish, GXV_MIDFLUSH, which needs vitaGL's weak
// gxm_context and so never runs here); nothing to wait for without a GPU.  (pgotool31, 18 Nov 2026)
extern "C" {
void sceGxmFinish(void*) {}
int sceGxmMidSceneFlush(void*, unsigned, void*, const void*) { return 0; }
}

// (the console host's: platform/vgx/vita/vgx_host_vita.cpp; the mask guard is off)
extern "C" {
unsigned char vgxvita_maskGuardOn = 0, vgxvita_maskGuardDue = 0;
void vgxvita_maskGuard(void) {}
}

// (the console host's, vgx_host_galaxy_vita.cpp)
namespace vgx::host {
bool CreateContext() { return true; }
}
