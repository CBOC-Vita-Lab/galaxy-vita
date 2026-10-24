#pragma once

#include <JSystem/JAudio2/JASSeqReader.hpp>

class AudMeSeqReader {
public:
    AudMeSeqReader() {
        init();
    }

    void init() {
        init(nullptr);
    }

    void init(void*);
    bool call(u32);
    bool loopStart(u16);
    bool loopEnd();
    bool ret();

    void jump(u32 addr) {
        mSeqCursor = mSeqBuff + addr;
    }

    void jump(void* pPtr) {
        mSeqCursor = (u8*)pPtr;
    }

    u32* getBase() {
        return (u32*)mSeqBuff;
    }

    u8* getAddr(u32 addr) {
        return mSeqBuff + addr;
    }

    u8 getByte(u32 addr) const {
        return *(mSeqBuff + addr);
    }

#ifdef __MWERKS__
    u16 get16(u32 addr) const {
        return *(u16*)(mSeqBuff + addr);
    }

    u32 get24(u32 addr) const {
        return (*(u32*)(mSeqBuff + addr - 1)) & 0xffffff;
    }

    u32 get32(u32 addr) const {
        return *(u32*)(mSeqBuff + addr);
    }
#else
    // Sequence byte code stays big-endian.
    u16 get16(u32 addr) const {
        const u8* p = mSeqBuff + addr;
        return (u16)((p[0] << 8) | p[1]);
    }

    u32 get24(u32 addr) const {
        const u8* p = mSeqBuff + addr;
        return ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
    }

    u32 get32(u32 addr) const {
        const u8* p = mSeqBuff + addr;
        return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
    }
#endif

    u8* getCur() {
        return mSeqCursor;
    }

    u32 read8() {
        return *mSeqCursor++;
    }

    u32 read16() {
        u32 value = *mSeqCursor++ << 8;
        value |= *mSeqCursor++;
        return value;
    }

    u32 read24() {
        u32 ret = read8() << 8 | read8();
        ret <<= 8;
        ret |= read8();
        return ret;
    }

    u16 getLoopCount() const {
        if (mNumStacks == 0) {
            return 0;
        }

        return mLoopCounts[mNumStacks - 1];
    }

    /* 0x00 */ u8* mSeqBuff;
    /* 0x04 */ u8* mSeqCursor;
    /* 0x08 */ u32 mNumStacks;
    /* 0x0C */ u8* mStackPtrs[8];
    /* 0x2C */ u16 mLoopCounts[8];
};
