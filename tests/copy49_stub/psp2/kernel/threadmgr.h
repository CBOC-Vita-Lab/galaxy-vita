#pragma once
#include <stddef.h>
typedef int SceUID;
struct SceKernelThreadInfo { size_t size; char name[32]; };
extern "C" int sceKernelGetThreadId(void);
extern "C" int sceKernelGetThreadInfo(SceUID, SceKernelThreadInfo*);
extern "C" int sceKernelDelayThread(unsigned usec);
