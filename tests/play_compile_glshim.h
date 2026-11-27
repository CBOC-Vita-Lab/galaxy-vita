// tests31: tests/asynclink_glshim.h plus the one console name the back end's vram30 guard (GXV_VRAM_GUARD) needs: sceGxmFinish, which the
// stub <psp2/gxm.h> of tests/autoq_clock_stubs does not declare. Used by tools/test_play_compile.sh only (-include).
#pragma once
#include "asynclink_glshim.h"
inline void sceGxmFinish(void*) {}
// (the stub declares it returning const void*, the back end stores it in a void*; a test that defines the stub's function #undefs this first)
#define vglGetTexDataPointer(t) const_cast<void*>(vglGetTexDataPointer(t))
