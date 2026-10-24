#pragma once

#include "JSystem/JKernel/JKRArchive.hpp"

enum JKRMemBreakFlag { JKR_MEM_BREAK_FLAG_0 = 0, JKR_MEM_BREAK_FLAG_1 = 1 };

class JKRMemArchive : public JKRArchive {
public:
    JKRMemArchive();
    JKRMemArchive(int, EMountDirection);
    virtual ~JKRMemArchive();

    virtual void removeResourceAll();
    virtual bool removeResource(void*);
    virtual u32 getExpandedResSize(const void*) const;
    virtual void* fetchResource(SDIFileEntry*, unsigned int*);
    virtual void* fetchResource(void*, unsigned int, SDIFileEntry*, unsigned int*);

    void fixedInit(int);
    bool mountFixed(void*, JKRMemBreakFlag);
    bool open(int, EMountDirection);
    bool open(void*, unsigned int, JKRMemBreakFlag);
    static s32 fetchResource_subroutine(unsigned char*, unsigned int, unsigned char*, unsigned int, int);

    RarcHeader* mHeader;  // 0x64
    u8* mFileDataStart;   // 0x68
    bool _6C;
    u8 _6D[3];
};
