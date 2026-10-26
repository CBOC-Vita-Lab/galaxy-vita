#pragma once
typedef int SceUID;
typedef unsigned SceSize;
typedef unsigned SceUInt;
int sceKernelGetThreadCurrentPriority(void);
int sceKernelChangeThreadPriority(int, int);
int sceKernelChangeThreadCpuAffinityMask(int, int);
int sceKernelGetThreadId(void);
int sceKernelGetThreadCpuAffinityMask(int);
int sceKernelCreateThread(const char*, int (*)(SceSize, void*), int, unsigned, unsigned, unsigned, void*);
int sceKernelStartThread(int, unsigned, void*);
int sceKernelWaitThreadEnd(int, int*, void*);
int sceKernelDeleteThread(int);
int sceKernelDelayThread(unsigned);
