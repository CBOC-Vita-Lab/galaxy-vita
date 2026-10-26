#pragma once
#include <stdint.h>
typedef uint64_t SceUInt64;
#ifdef __cplusplus
extern "C" {
#endif
SceUInt64 sceKernelGetProcessTimeWide(void);
#ifdef __cplusplus
}
#endif
