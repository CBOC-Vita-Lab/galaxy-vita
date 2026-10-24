#pragma once

#include "JSystem/JKernel/JKRArchive.hpp"

class JKRAramBlock;
class JKRFile;

class JKRAramArchive : public JKRArchive {
public:
    JKRAramArchive(int, EMountDirection);
    virtual ~JKRAramArchive();

    virtual u32 getExpandedResSize(const void*) const;
    virtual void* fetchResource(SDIFileEntry*, unsigned int*);
    virtual void* fetchResource(void*, unsigned int, SDIFileEntry*, unsigned int*);

    bool open(int);
    static u32 fetchResource_subroutine(unsigned int, unsigned int, unsigned char*, unsigned int, int);
    static u32 fetchResource_subroutine(unsigned int, unsigned int, JKRHeap*, int, unsigned char**);

    JKRAramBlock* mBlock;
    JKRFile* mDvdFile;
};

inline int JKRConvertAttrToCompressionType(int arg) {
    if ((arg & 0x4) == 0) {
        return 0;
    }

    return ((arg & 0x80) != 0) + 1;
}
