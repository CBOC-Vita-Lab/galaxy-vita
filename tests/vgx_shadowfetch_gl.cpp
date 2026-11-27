// GXV_SHADOWFETCH (gpu23b; vgx_draw.cpp ShadowFetchPass, vgx_backend_gl.cpp "shadow fetch"): the silhouette fill drawn by a program that
// reads the EFB's own alpha (on the desktop: GL_EXT_shader_framebuffer_fetch) instead of the whole-EFB A8 capture, through the
// production GX front end and GLES back end on Mesa. The scene is tests/vgx_shadowfill_gl.cpp's (16 game-like pictures: sky, floor,
// shadow volumes, the A8 capture, the exact fillSilhouetteColor quad, a translucent quad, the display copy); this file includes it
// and adds the counters. tools/test_vgx_shadowfetch_gl.sh runs it with GXV_SHADOWFETCH=0 / 2 / 1 (and GXV_SHADOWFILL_BOUNDS 0 / 1,
// several render sizes) and compares the pictures (the alpha capture texture itself is left stale when the copy is left out).
#include "vgx_state.h"
// (the scenes every executed frame began, summed: the scene's own SubmitToBackend calls backend::Execute, renamed here)
static unsigned g_sceneSum = 0;
namespace vgx::backend {
inline void ExecuteCounted(CmdBuf& b) {
    Execute(b);
    g_sceneSum += DebugLastFrameScenes();
}
}
#define Execute ExecuteCounted
#define main shadowfill_main
#include "vgx_shadowfill_gl.cpp"
#undef main
#undef Execute
int main() {
    const int r = shadowfill_main();
    uint64_t c[5];
    vgx::ShadowFetchCounters(c);
    std::printf("scenes total=%u\n", g_sceneSum);
    std::printf("shadowfetch pictures=%llu pairs=%llu fetched=%llu requests=%llu stale=%llu\n", static_cast<unsigned long long>(c[0]),
                static_cast<unsigned long long>(c[1]), static_cast<unsigned long long>(c[2]), static_cast<unsigned long long>(c[3]),
                static_cast<unsigned long long>(c[4]));
    return r;
}
