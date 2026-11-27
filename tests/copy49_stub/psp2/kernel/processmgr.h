// copy49 host test stub: just what vita/vgx-game/smallcopy_wrap.cpp uses of the SDK
#pragma once
#include <stdint.h>
typedef uint64_t SceUInt64;
extern "C" SceUInt64 sceKernelGetProcessTimeWide(void);
