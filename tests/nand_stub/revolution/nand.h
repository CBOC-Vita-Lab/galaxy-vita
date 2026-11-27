// A reduced revolution/nand.h for the host test of platform/src/nand/nand.cpp (tests/nand_safe_test.cpp): the real header pulls in the whole SDK and
// the PowerPC intrinsics, which clash with the host's math library. Only what nand.cpp and the test use; the values are the SDK's.
#pragma once
#include <stdint.h>
typedef int32_t s32; typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8;
#ifdef __cplusplus
extern "C" {
#endif
#define NAND_RESULT_OK 0
#define NAND_RESULT_ACCESS (-1)
#define NAND_RESULT_EXISTS (-6)
#define NAND_RESULT_MAXBLOCKS (-9)
#define NAND_RESULT_NOEXISTS (-12)
#define NAND_RESULT_NOTEMPTY (-13)
#define NAND_RESULT_UNKNOWN (-64)
#define NAND_MAX_PATH 64
#define NAND_BANNER_COMMENT_SIZE 32
#define NAND_BANNER_SIGNATURE 0x5749424E
typedef enum { NAND_ACCESS_NONE, NAND_ACCESS_READ, NAND_ACCESS_WRITE, NAND_ACCESS_RW } NANDAccessType;
typedef struct NANDFileInfo { s32 fileDescriptor; s32 origFd; char origPath[NAND_MAX_PATH]; char tmpPath[NAND_MAX_PATH]; u8 accType; u8 stage; u8 mark; } NANDFileInfo;
typedef struct NANDBanner { u32 signature; u32 flag; u16 comment[2][NAND_BANNER_COMMENT_SIZE]; u8 pad[0x1000]; } NANDBanner;
s32 NANDInit(void);
s32 NANDGetHomeDir(char path[NAND_MAX_PATH]);
s32 NANDCreate(const char*, u8, u8);
s32 NANDPrivateCreate(const char*, u8, u8);
s32 NANDOpen(const char*, NANDFileInfo*, u8);
s32 NANDPrivateOpen(const char*, NANDFileInfo*, u8);
s32 NANDClose(NANDFileInfo*);
s32 NANDRead(NANDFileInfo*, void*, u32);
s32 NANDGetLength(NANDFileInfo*, u32*);
s32 NANDDelete(const char*);
s32 NANDMove(const char*, const char*);
s32 NANDCheck(u32, u32, u32*);
s32 NANDWrite(NANDFileInfo*, const void*, u32);
s32 NANDSeek(NANDFileInfo*, s32, s32);
void NANDInitBanner(NANDBanner*, u32, const u16*, const u16*);
#ifdef __cplusplus
}
#endif
