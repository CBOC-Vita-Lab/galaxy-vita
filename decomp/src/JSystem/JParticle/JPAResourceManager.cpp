#include "JSystem/JParticle/JPAResourceManager.hpp"
#include "JSystem/JParticle/JPADynamicsBlock.hpp"
#include "JSystem/JParticle/JPAResource.hpp"
#include <JSystem/JKernel/JKRHeap.hpp>
#include <cstring>

struct JPAResourceLoader {
    JPAResourceLoader(u8 const*, JPAResourceManager*);
};

JPAResourceManager::JPAResourceManager(void const* pData, JKRHeap* pHeap) {
    mpResArr = NULL;
    mpTexArr = NULL;
    mResMax = 0;
    mResNum = 0;
    mTexMax = 0;
    mTexNum = 0;
    mpHeap = pHeap;
#ifdef TARGET_PC
    mpIdxTable = NULL;
    mIdxMask = 0;
#endif
    JPAResourceLoader loader((u8 const*)pData, this);
}

JPAResource* JPAResourceManager::getResource(u16 usrIdx) const {
#ifdef TARGET_PC
    if (mpIdxTable != NULL) {
        for (u32 h = usrIdx & mIdxMask;; h = (h + 1) & mIdxMask) {
            const u16 at = mpIdxTable[h];
            if (at == 0)
                return NULL;
            if (mpResArr[at - 1]->getUsrIdx() == usrIdx)
                return mpResArr[at - 1];
        }
    }
#endif
    for (u16 i = 0; i < mResNum; i++)
        if (mpResArr[i]->getUsrIdx() == usrIdx)
            return mpResArr[i];
    return NULL;
}

const ResTIMG* JPAResourceManager::swapTexture(ResTIMG const* img, char const* swapName) {
    const ResTIMG* ret = NULL;

    for (s32 i = 0; i < mTexNum; i++) {
        if (strcmp(swapName, mpTexArr[i]->getName()) == 0) {
            JUTTexture* tex = mpTexArr[i]->getJUTTexture();
            ret = tex->getTexInfo();
            tex->storeTIMG(img, (u8)0);
            break;
        }
    }

    return ret;
}

void JPAResourceManager::registRes(JPAResource* res) {
    mpResArr[mResNum] = res;
#ifdef TARGET_PC
    if (mpIdxTable == NULL && mResMax != 0) {
        u32 size = 16;
        while (size < (u32)mResMax * 2)
            size <<= 1;
        mpIdxTable = new (mpHeap, 0) u16[size];
        memset(mpIdxTable, 0, size * sizeof(u16));
        mIdxMask = (u16)(size - 1);
    }
    if (mpIdxTable != NULL) {
        // (the search returns the first resource with an index, so a repeat does not replace it)
        u32 h = res->getUsrIdx() & mIdxMask;
        for (;; h = (h + 1) & mIdxMask) {
            const u16 at = mpIdxTable[h];
            if (at == 0) {
                mpIdxTable[h] = (u16)(mResNum + 1);
                break;
            }
            if (mpResArr[at - 1]->getUsrIdx() == res->getUsrIdx())
                break;
        }
    }
#endif
    mResNum++;
}

void JPAResourceManager::registTex(JPATexture* tex) {
    mpTexArr[mTexNum] = tex;
    mTexNum++;
}
