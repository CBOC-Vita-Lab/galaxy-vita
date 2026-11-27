// armhf PGO harness only (PORT_ARMHF_VITA_SHIM, force-included by armhf/CMakeLists.txt): declarations of the two psp2 kernel calls
// the console's branches of NowMs (vgx_backend_gl.cpp) and NapUs (port/wait_bound.h) make, so the harness compiles those branches
// and a profile of it matches the console's control flow.  The definitions are armhf/glue/vita_shim.cpp (host clock and nap).
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
unsigned long long sceKernelGetProcessTimeWide(void);
int sceKernelDelayThread(uint32_t usec);
extern unsigned int g_vgx_arena_min;  // (0; the console's floor is 0x80000000)
#ifdef __cplusplus
}
#endif
