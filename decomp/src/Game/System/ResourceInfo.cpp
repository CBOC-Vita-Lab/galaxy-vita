#include "Game/System/ResourceInfo.hpp"
#include "Game/Util.hpp"
#include <cstdio>
#include <cstring>
#include <size_t.h>
#ifdef TARGET_PC
#include "port/hot62_memo.h"  // (hot62: GXV_RESIDX_MEMO)
#include "port/hot_knob.h"
extern "C" int port_on_frame_thread(void);
namespace {
port::hot62::ResMemo sResMemo;
inline bool ResIdxMemoOn() {
    return PORT_HOT_KNOB(bool, [] {
        const char* e = getenv("GXV_RESIDX_MEMO");
        return e && *e && atoi(e) != 0;   // (w24: default off until a console A/B)
    }());
}
}  // namespace
#endif

ResFileInfo::ResFileInfo() {
    mName = 0;
    mHashCode = 0;
    mResource = 0;
    _8 = 0;
    _4 = 0;
    _C = 0;
}

ResTable::ResTable() : mFileInfoTable(0), mCount(0) {
#ifdef TARGET_PC
    port::hot62::ResBump();
#endif
}

void ResTable::newFileInfoTable(u32 count) {
    mFileInfoTable = new ResFileInfo[count];
#ifdef TARGET_PC
    port::hot62::ResBump();  // (hot62: a table array is a new life of its address)
#endif
}

ResFileInfo* ResTable::add(const char* pName, void* pRes, bool stripExt) {
    ResFileInfo* curInfo = &mFileInfoTable[mCount];
    curInfo->mResource = pRes;
    curInfo->setName(pName, stripExt);
    mCount++;
#ifdef TARGET_PC
    port::hot62::ResBump();
#endif
    return curInfo;
}

const char* ResTable::getResName(u32 idx) const {
    return mFileInfoTable[idx].mName;
}

void* ResTable::getRes(u32 idx) const {
    return mFileInfoTable[idx].mResource;
}

void* ResTable::getRes(const char* pName) const {
    return findRes(pName);
}

ResFileInfo* ResTable::findFileInfo(const char* pName) const {
    int resIdx = getResIndex(pName);
    if (resIdx != -1) {
        return &mFileInfoTable[resIdx];
    }

    return 0;
}

ResFileInfo* ResTable::getFileInfo(u32 idx) const {
    return &mFileInfoTable[idx];
}

bool ResTable::isExistRes(const char* pRes) const {
    return getResIndex(pRes) != -1;
}

void* ResTable::findRes(const char* pName) const {
    int idx = getResIndex(pName);

    if (idx != -1) {
        return mFileInfoTable[idx].mResource;
    }

    return 0;
}

int ResTable::getResIndex(const char* pName) const {
    u32 hash = MR::getHashCodeLower(pName);

#ifdef TARGET_PC
    // hot62 GXV_RESIDX_MEMO (port/hot62_memo.h): the same first index (or -1), remembered per (table, array, count, hash, generation).
    if (mCount >= 8 && ResIdxMemoOn() && port_on_frame_thread()) {
        const ResFileInfo* const pTable = mFileInfoTable;
        return port::hot62::ResIndex(sResMemo, this, pTable, mCount, hash, [pTable](u32 i) { return pTable[i].mHashCode; });
    }
#endif
    for (int i = 0; i < mCount; i++) {
        u32 curHash = mFileInfoTable[i].mHashCode;

        if (curHash == hash) {
            return i;
        }
    }

    return -1;
}

const char* ResTable::findResName(const void* pRes) const {
    for (int i = 0; i < mCount; i++) {
        ResFileInfo* inf = &mFileInfoTable[i];
        if (pRes == inf->mResource) {
            return inf->mName;
        }
    }

    return 0;
}

const char* ResTable::getResName(const void* pResource) const {
    return findResName(pResource);
}

void ResFileInfo::setName(const char* pName, bool stripExt) {
    size_t len = strlen(pName) + 1;
    mName = new char[len];

    snprintf(mName, len, "%s", pName);

    if (stripExt) {
        char* out = strrchr(mName, '.');

        if (out) {
            out[0] = 0;
        }
    }

    mHashCode = MR::getHashCodeLower(mName);
}