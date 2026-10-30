// Host stand-ins for the vitaGL-only names that platform/vgx/src/vgx_backend_gl.cpp uses when it is built with
// __VGX_VITA__ against Mesa (the console gets them from <vitaGL.h> through platform/vgx/vita/include/GLES2/gl2.h).
// Mesa owns the textures and has no video-memory pool: report plenty.
#pragma once
#include <cstddef>
#include <GLES2/gl2.h>
constexpr int VGL_MEM_VRAM = 0;
inline size_t vglMemFree(int) { return 128u << 20; }
inline size_t vglMemTotal(int) { return 128u << 20; }
inline void* vglGetTexDataPointer(GLenum) { static unsigned char storage; return &storage; }
