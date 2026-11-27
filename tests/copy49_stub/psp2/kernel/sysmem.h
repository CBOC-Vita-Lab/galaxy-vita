// sc55 host test stub: the memory-block query smallcopy_wrap.cpp uses. The test sets the map through g_stubBlocks.
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int SceUID;
typedef uint32_t SceKernelMemBlockType;
typedef size_t SceSize;
enum { SCE_KERNEL_MEMORY_TYPE_NORMAL_NC = 0x80, SCE_KERNEL_MEMORY_TYPE_NORMAL = 0xD0 };
struct SceKernelMemBlockInfo { SceSize size; void* mappedBase; SceSize mappedSize; int memoryType; uint32_t access; SceKernelMemBlockType type; };
extern "C" int sceKernelGetMemBlockInfoByAddr(void* base, SceKernelMemBlockInfo* info);
extern "C" int sceKernelFreeMemBlock(SceUID uid);
typedef struct SceKernelAllocMemBlockOpt SceKernelAllocMemBlockOpt;
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW 0x09408060
extern "C" int sceKernelGetMemBlockBase(SceUID uid, void** base);
