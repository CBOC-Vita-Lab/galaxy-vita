// nohitch26: the few GL names this base's GL stub (armhf/glstub) lacked for vgx_backend_gl.cpp (pre-existing: tests/vgx_async_shaders_backend.cpp does not build on this base either).
// Used by the host harnesses that build the back end on the GL stub (-include). The stencil calls are never made by the tests' draws.
// emufix31 (18 Nov 2026): pgotool31 (d73df8c4) put these names into the stub itself (GL_KEEP.., GL_BYTE/SHORT, GL_OUT_OF_MEMORY, the stencil calls, and
// VGL_MEM_* under __VGX_VITA__): defined here too, they no longer compiled ("redefinition of enumerator"), and every harness that -includes this file
// (test_keepdepth31_backend, test_dma31, test_res_adopt, test_vgx_stage_warm, test_c185_astro_mask) failed to build. Only what the stub still lacks stays.
#pragma once
#include <GLES2/gl2.h>
#ifndef __VGX_VITA__
enum { VGL_MEM_RAM = 1, VGL_MEM_PHYCONT = 2, VGL_MEM_BUDGET = 3, VGL_MEM_EXTERNAL = 4 };
#endif
