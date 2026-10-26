#pragma once
#include <cstdint>

using SceUID = std::int32_t;
using SceSize = std::uint32_t;
using SceUInt = std::uint32_t;
extern "C" {
SceUID sceKernelCreateSema(const char*, unsigned, int, int, const void*);
int sceKernelWaitSema(SceUID, int, SceUInt*);
int sceKernelSignalSema(SceUID, int);
SceUID sceKernelCreateThread(const char*, int (*)(SceSize, void*), int, SceSize, unsigned, int, const void*);
int sceKernelStartThread(SceUID, SceSize, void*);
int sceKernelDelayThread(SceUInt);
}
