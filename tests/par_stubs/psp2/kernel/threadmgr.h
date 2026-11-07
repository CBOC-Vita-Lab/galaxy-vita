#pragma once
// Host stand-ins for the few kernel calls platform/include/port/par_exec.h makes on the console (tests/par_exec_view.cpp runs its
// __vita__ branch on the host with these; tests/par_exec_view_stubs.cpp implements them with std::thread and a counting semaphore).
#include <cstdint>
using SceUID = std::int32_t;
using SceSize = std::uint32_t;
using SceUInt = std::uint32_t;
using SceUInt64 = std::uint64_t;
typedef int (*SceKernelThreadEntry)(SceSize args, void* argp);
extern "C" {
SceUID sceKernelCreateSema(const char* name, SceUInt attr, int initVal, int maxVal, const void* option);
int sceKernelWaitSema(SceUID semaid, int signal, SceUInt* timeout);
int sceKernelSignalSema(SceUID semaid, int signal);
SceUID sceKernelCreateThread(const char* name, SceKernelThreadEntry entry, int initPriority, SceSize stackSize, SceUInt attr,
                             int cpuAffinityMask, const void* option);
int sceKernelStartThread(SceUID thid, SceSize arglen, void* argp);
int sceKernelGetThreadCurrentPriority(void);
}
