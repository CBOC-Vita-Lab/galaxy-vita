#include "JSystem/J3DGraphBase/J3DDrawBuffer.hpp"
#include "JSystem/J3DGraphBase/J3DMaterial.hpp"
#include "JSystem/J3DGraphBase/J3DPacket.hpp"
#include "JSystem/J3DGraphBase/J3DTexture.hpp"
#include "JSystem/JKernel/JKRHeap.hpp"
#include "port/offset32.h"

void J3DDrawBuffer::initialize() {
    mDrawMode = J3DDrawBufDrawMode_Head;
    mSortMode = J3DDrawBufSortMode_Mat;
    mZNear = 1.0f;
    mZFar = 10000.0f;
    mpZMtx = nullptr;
    mpCallBackPacket = nullptr;
    mEntryTableSize = 0x20;
    calcZRatio();
}

int J3DDrawBuffer::allocBuffer(u32 size) {
    mpBuffer = new (0x20) J3DPacket*[size];
    mEntryTableSize = size;
    frameInit();
    calcZRatio();
    return 0;
}

void J3DDrawBuffer::frameInit() {
    u32 bufSize = mEntryTableSize;
    for (u32 i = 0; i < bufSize; i++)
        mpBuffer[i] = nullptr;

    mpCallBackPacket = nullptr;
}

int J3DDrawBuffer::entryMatSort(J3DMatPacket* pMatPacket) {
    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

    if (pMatPacket->isChanged()) {
        pMatPacket->setNextPacket(mpBuffer[0]);
        mpBuffer[0] = pMatPacket;
        return 1;
    }

    J3DTexture* pTexture = j3dSys.getTexture();
    u16 texNo = pMatPacket->getMaterial()->getTexNo(0);

    u32 hash;
    if (texNo == 0xFFFF) {
        hash = 0;
    } else {
        hash = reinterpret_cast< uintptr_t >(port_offset32(pTexture->getResTIMG(texNo), pTexture->getResTIMG(texNo)->mImageDataOffset)) >> 5;
    }
    u32 slot = hash & (mEntryTableSize - 1);

    if (mpBuffer[slot] == nullptr) {
        mpBuffer[slot] = pMatPacket;
        return 1;
    }

    J3DMatPacket* packet;
    for (packet = static_cast< J3DMatPacket* >(mpBuffer[slot]); packet != nullptr; packet = static_cast< J3DMatPacket* >(packet->getNextPacket())) {
        if (packet->isSame(pMatPacket)) {
            packet->addShapePacket(pMatPacket->getShapePacket());
            return 0;
        }
    }

    pMatPacket->setNextPacket(mpBuffer[slot]);
    mpBuffer[slot] = pMatPacket;
    return 1;
}

int J3DDrawBuffer::entryMatAnmSort(J3DMatPacket* pMatPacket) {
    J3DMaterialAnm* pMaterialAnm = pMatPacket->mpMaterialAnm;
    u32 slot = reinterpret_cast< uintptr_t >(pMaterialAnm) & (mEntryTableSize - 1);

    if (pMaterialAnm == nullptr) {
        return entryMatSort(pMatPacket);
    }

    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

    if (mpBuffer[slot] == nullptr) {
        mpBuffer[slot] = pMatPacket;
        return 1;
    }

    J3DMatPacket* packet;
    for (packet = static_cast< J3DMatPacket* >(mpBuffer[slot]); packet != nullptr; packet = static_cast< J3DMatPacket* >(packet->getNextPacket())) {
        if (packet->mpMaterialAnm == pMaterialAnm) {
            packet->addShapePacket(pMatPacket->getShapePacket());
            return 0;
        }
    }

    pMatPacket->setNextPacket(mpBuffer[slot]);
    mpBuffer[slot] = pMatPacket;
    return 1;
}

int J3DDrawBuffer::entryZSort(J3DMatPacket* pMatPacket) {
    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

    Vec tmp;
    tmp.x = mpZMtx[0][3];
    tmp.y = mpZMtx[1][3];
    tmp.z = mpZMtx[2][3];

    f32 value = -J3DCalcZValue(j3dSys.getViewMtx(), tmp);

    u32 index;
    if (mZNear + mZRatio < value) {
        if (mZFar - mZRatio > value) {
            index = value / mZRatio;
        } else {
            index = mEntryTableSize - 1;
        }
    } else {
        index = 0;
    }

    index = (mEntryTableSize - 1) - index;
    pMatPacket->setNextPacket(mpBuffer[index]);
    mpBuffer[index] = pMatPacket;
    return 1;
}

int J3DDrawBuffer::entryModelSort(J3DMatPacket* pMatPacket) {
    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

    if (mpCallBackPacket != nullptr) {
        mpCallBackPacket->addChildPacket(pMatPacket);
        return 1;
    }

    return 0;
}

int J3DDrawBuffer::entryInvalidSort(J3DMatPacket* pMatPacket) {
    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

    if (mpCallBackPacket != nullptr) {
        mpCallBackPacket->addChildPacket(pMatPacket->getShapePacket());
        return 1;
    }

    return 0;
}

int J3DDrawBuffer::entryNonSort(J3DMatPacket* pMatPacket) {
    pMatPacket->drawClear();
    pMatPacket->getShapePacket()->drawClear();

    pMatPacket->setNextPacket(mpBuffer[0]);
    mpBuffer[0] = pMatPacket;
    return 1;
}

J3DDrawBuffer::sortFunc J3DDrawBuffer::sortFuncTable[6] = {
    &J3DDrawBuffer::entryMatSort,   &J3DDrawBuffer::entryMatAnmSort,  &J3DDrawBuffer::entryZSort,
    &J3DDrawBuffer::entryModelSort, &J3DDrawBuffer::entryInvalidSort, &J3DDrawBuffer::entryNonSort,
};

J3DDrawBuffer::drawFunc J3DDrawBuffer::drawFuncTable[2] = {
    &J3DDrawBuffer::drawHead,
    &J3DDrawBuffer::drawTail,
};

int J3DDrawBuffer::entryNum;

void J3DDrawBuffer::draw() const {
    drawFunc func = drawFuncTable[mDrawMode];
    (this->*func)();
}

// GXV_DRAW_PREFETCH (game24, default 0): the packets of a draw buffer are scattered through the heap, and the draw of one packet (material
// load, display lists, shape draws) is the only work between two reads of the chain; the next packet's first lines are asked for before
// the current packet is drawn. A hint only: the chain is still read from the current packet after its draw, as before, nothing else changes.
#ifdef TARGET_PC
extern "C" int port_draw_prefetch_on(void);
extern "C" int port_draw_prof_on(void);
extern "C" void port_draw_prof_count(int what, unsigned n);
#define PORT_DRAW_PF(pk) do { if (pf) { J3DPacket* pfn = (pk)->getNextPacket(); if (pfn != nullptr) { __builtin_prefetch(pfn, 0, 3); __builtin_prefetch(reinterpret_cast< char* >(pfn) + 32, 0, 3); ++pfCount; } } } while (0)
#else
#define PORT_DRAW_PF(pk) do {} while (0)
#endif

void J3DDrawBuffer::drawHead() const {
    u32 size = mEntryTableSize;
    J3DPacket** buf = mpBuffer;
#ifdef TARGET_PC
    const bool pf = port_draw_prefetch_on() != 0;
    unsigned pfCount = 0;
#endif

    for (u32 i = 0; i < size; i++) {
        for (J3DPacket* packet = buf[i]; packet != nullptr; packet = packet->getNextPacket()) {
            PORT_DRAW_PF(packet);
            packet->draw();
        }
    }
#ifdef TARGET_PC
    if (pfCount != 0 && port_draw_prof_on() != 0) port_draw_prof_count(19, pfCount);
#endif
}

void J3DDrawBuffer::drawTail() const {
#ifdef TARGET_PC
    const bool pf = port_draw_prefetch_on() != 0;
    unsigned pfCount = 0;
#endif
    for (int i = mEntryTableSize - 1; i >= 0; i--) {
        for (J3DPacket* packet = mpBuffer[i]; packet != nullptr; packet = packet->getNextPacket()) {
            PORT_DRAW_PF(packet);
            packet->draw();
        }
    }
#ifdef TARGET_PC
    if (pfCount != 0 && port_draw_prof_on() != 0) port_draw_prof_count(19, pfCount);
#endif
}

void J3DDrawBuffer::calcZRatio() {
    mZRatio = (mZFar - mZNear) / static_cast< f32 >(mEntryTableSize);
}
