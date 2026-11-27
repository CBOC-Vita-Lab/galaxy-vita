// dma31: the GL stub's names this base's back end needs beyond tests/asynclink_glshim.h (used by tools/test_dma31.sh only, -include).
#pragma once
#include "asynclink_glshim.h"
#include <psp2/gxm.h>
extern "C" void sceGxmFinish(SceGxmContext*);
// (the stub declares it returning const void*, the back end stores it in a void*)
#define vglGetTexDataPointer(t) const_cast<void*>(vglGetTexDataPointer(t))
