#include "JSystem/J3DGraphBase/J3DPacket.hpp"
#include "JSystem/J3DGraphAnimator/J3DModel.hpp"
#include "JSystem/J3DGraphAnimator/J3DModelData.hpp"
#include "JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp"
#include "JSystem/J3DGraphBase/J3DDrawBuffer.hpp"
#include "JSystem/J3DGraphBase/J3DMaterial.hpp"
#include "JSystem/J3DGraphBase/J3DShapeMtx.hpp"
#include "JSystem/JKernel/JKRHeap.hpp"
#include <cstdio>
#include <mem.h>
#include "port/hot_knob.h"

J3DError J3DDisplayListObj::newDisplayList(u32 maxSize) {
    mMaxSize = ALIGN_NEXT(maxSize, 0x20);
    mpDisplayList[0] = new (0x20) char[mMaxSize];
    mpDisplayList[1] = new (0x20) char[mMaxSize];
    mSize = 0;

    return kJ3DError_Success;
}

J3DError J3DDisplayListObj::newSingleDisplayList(u32 maxSize) {
    mMaxSize = ALIGN_NEXT(maxSize, 0x20);
    mpDisplayList[0] = new (0x20) char[mMaxSize];
    mpDisplayList[1] = mpDisplayList[0];
    mSize = 0;

    return kJ3DError_Success;
}

int J3DDisplayListObj::single_To_Double() {
    if (mpDisplayList[0] == mpDisplayList[1]) {
        mpDisplayList[1] = new (0x20) char[mMaxSize];

        memcpy(mpDisplayList[1], mpDisplayList[0], mMaxSize);
        DCStoreRange(mpDisplayList[1], mMaxSize);
    }

    return kJ3DError_Success;
}

void J3DDisplayListObj::setSingleDisplayList(void* pDLData, u32 size) {
    mMaxSize = ALIGN_NEXT(size, 0x20);
    mpDisplayList[0] = pDLData;
    mpDisplayList[1] = mpDisplayList[0];
    mSize = size;
}

void J3DDisplayListObj::swapBuffer() {
    void* pTmp = mpDisplayList[0];
    mpDisplayList[0] = mpDisplayList[1];
    mpDisplayList[1] = pTmp;
}

void J3DDisplayListObj::callDL() const {
    GXCallDisplayList(mpDisplayList[0], mSize);
}

void J3DDisplayListObj::beginDL() {
    swapBuffer();
    sInterruptFlag = OSDisableInterrupts();
    GDInitGDLObj(&sGDLObj, (u8*)mpDisplayList[0], mMaxSize);
    GDSetCurrent(&sGDLObj);
}

u32 J3DDisplayListObj::endDL() {
    GDPadCurr32();
    OSRestoreInterrupts(sInterruptFlag);
    mSize = GDGetGDLObjOffset(&sGDLObj);
    GDFlushCurrToMem();
    GDSetCurrent(NULL);
    return mSize;
}

void J3DDisplayListObj::beginPatch() {
    beginDL();
}

u32 J3DDisplayListObj::endPatch() {
    OSRestoreInterrupts(sInterruptFlag);
    GDSetCurrent(NULL);
    return mSize;
}

#ifdef TARGET_PC
// ============================================================== (1.3 port) game-side cuts of the J3D state lists
// The front end (core 2) is the limit of the frame rate, and what it costs is mostly per list call (finding the list's entry,
// its generation, the replay), not per register. Three exact cuts of what the draw pass sends, each behind a switch read once:
//   GXV_J3D_DIFFKEEP=1  (here) J3DMaterial::diff makes a differed list (J3DShapePacket's: the animated TEV colours, texture
//                       matrices, fog, ... of one instance) on every drawn turn, into the other of its two buffers, mostly with the
//                       bytes the current one holds (stopped animations, fog). It is now made aside and stored, in the other buffer
//                       as before, only when the bytes differ; otherwise nothing is written and the current buffer stays. The
//                       front end then finds the list where it was, its memory not written since (no compare of its bytes, nor of
//                       the material lists sharing its 4 KiB), and the game makes no write notification for it.
//   GXV_J3D_MATMERGE=1  (DrawBuffer.cpp) the material list and the first packet's differed list, sent one after the other, become
//                       one list call when the differed list has not changed since the last picture.
//   GXV_J3D_DIFFSKIP=1  (DrawBuffer.cpp) a differed list with the bytes of the one sent just before it (instances of a model in the
//                       same state) is not sent when nothing between could have changed what it writes.
// The front end receives the same register writes in an order with the same effect: see J3DPortDL.hpp and DrawBuffer.cpp.
#include <cstdlib>
#include <cstring>
extern "C" void port_log(const char* fmt, ...);

namespace {
int j3dPortEnv(const char* pName, int fallback) {
    const char* e = getenv(pName);
    return (e != nullptr && *e != '\0') ? atoi(e) : fallback;
}
u32 sJ3DPortPicture = 1;
// (one list made aside at a time: J3DDisplayListObj::sGDLObj, which it uses, is one too)
u8 sDiffScratch[0x1000] __attribute__((aligned(32)));
bool sDiffScratchOn = false;
J3DPortListStats sJ3DPortLogBase;
}  // namespace

J3DPortListStats gJ3DPortListStats;

// (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h): every material diff, material packet and shape drawer asks.
bool J3DPortDiffKeepOn() {
    return PORT_HOT_KNOB(bool, j3dPortEnv("GXV_J3D_DIFFKEEP", 1) != 0);
}
bool J3DPortMatMergeOn() {
    return PORT_HOT_KNOB(bool, j3dPortEnv("GXV_J3D_MATMERGE", 1) != 0);
}
bool J3DPortDiffSkipOn() {
    return PORT_HOT_KNOB(bool, j3dPortEnv("GXV_J3D_DIFFSKIP", 1) != 0);
}
u32 J3DPortPicture() {
    return sJ3DPortPicture;
}

// GameSystem::frameLoop, before a picture is drawn. The counters, per picture over the last 300 pictures.
extern "C" void port_j3d_picture_begin(void) {
    ++sJ3DPortPicture;
    if (sJ3DPortPicture % 300 != 0) {
        return;
    }
    const J3DPortListStats& c = gJ3DPortListStats;
    const J3DPortListStats& b = sJ3DPortLogBase;
    const u32 drawers = c.drawers - b.drawers;
    if (drawers != 0 || c.diffMade != b.diffMade) {
        const double n = 300.0;
        port_log("j3d lists: per picture %.1f material lists (%.1f merged with a differed list, GXV_J3D_MATMERGE), differed lists %.1f sent, "
                 "%.1f merged, %.1f not sent (GXV_J3D_DIFFSKIP); merged lists made %.2f; differed lists made %.1f, %.1f of them unchanged "
                 "and not stored (GXV_J3D_DIFFKEEP)",
                 drawers / n, (c.matMerged - b.matMerged) / n, (c.diffSent - b.diffSent) / n, (c.diffMerged - b.diffMerged) / n,
                 (c.diffSkipped - b.diffSkipped) / n, (c.mergeWrites - b.mergeWrites) / n, (c.diffMade - b.diffMade) / n,
                 (c.diffKept - b.diffKept) / n);
    }
    sJ3DPortLogBase = c;
}

void J3DDisplayListObj::beginDiffDL() {
    if (!J3DPortDiffKeepOn() || mMaxSize > sizeof(sDiffScratch) || mpDisplayList[0] == nullptr) {
        sDiffScratchOn = false;
        beginDL();
        return;
    }
    // (beginDL's, writing into the scratch buffer and without the swap)
    sDiffScratchOn = true;
    sInterruptFlag = OSDisableInterrupts();
    GDInitGDLObj(&sGDLObj, sDiffScratch, mMaxSize);
    GDSetCurrent(&sGDLObj);
}

u32 J3DDisplayListObj::endDiffDL() {
    ++gJ3DPortListStats.diffMade;
    if (!sDiffScratchOn) {
        return endDL();
    }
    sDiffScratchOn = false;
    GDPadCurr32();
    OSRestoreInterrupts(sInterruptFlag);
    const u32 size = GDGetGDLObjOffset(&sGDLObj);
    GDSetCurrent(NULL);
    if (size == mSize && memcmp(mpDisplayList[0], sDiffScratch, size) == 0) {
        // The bytes the current buffer holds: it stays the one called, unwritten (what endDL would have stored, in the other buffer,
        // is these same bytes).
        ++gJ3DPortListStats.diffKept;
        return mSize;
    }
    // (endDL's: into the other buffer, which becomes the current one, and the write made known as GDFlushCurrToMem does)
    swapBuffer();
    memcpy(mpDisplayList[0], sDiffScratch, size);
    mSize = size;
    DCFlushRange(mpDisplayList[0], mMaxSize);
    return mSize;
}
#endif

int J3DPacket::entry(J3DDrawBuffer* pBuffer) {
    return 1;
}

void J3DPacket::addChildPacket(J3DPacket* pPacket) {
    if (mpFirstChild == NULL) {
        mpFirstChild = pPacket;
    } else {
        pPacket->setNextPacket(mpFirstChild);
        mpFirstChild = pPacket;
    }
}

J3DDrawPacket::J3DDrawPacket() {
    mFlags = 0;
    mpDisplayListObj = NULL;
    mpTexMtxObj = NULL;
}

J3DDrawPacket::~J3DDrawPacket() {
}

J3DError J3DDrawPacket::newDisplayList(u32 size) {
    mpDisplayListObj = new J3DDisplayListObj();

    if (mpDisplayListObj == NULL)
        return kJ3DError_Alloc;

    J3DError ret = mpDisplayListObj->newDisplayList(size);
    if (ret != kJ3DError_Success)
        return ret;

    return kJ3DError_Success;
}

J3DError J3DDrawPacket::newSingleDisplayList(u32 size) {
    mpDisplayListObj = new J3DDisplayListObj();

    if (mpDisplayListObj == NULL)
        return kJ3DError_Alloc;

    J3DError ret = mpDisplayListObj->newSingleDisplayList(size);
    if (ret != kJ3DError_Success)
        return ret;

    return kJ3DError_Success;
}

void J3DDrawPacket::draw() {
    callDL();
}

J3DMatPacket::J3DMatPacket() {
    mpInitShapePacket = NULL;
    mpMaterial = NULL;
    mDiffFlag = 0xFFFFFFFF;
    mpTexture = NULL;
    mpMaterialAnm = NULL;
}

J3DMatPacket::~J3DMatPacket() {
}

void J3DMatPacket::addShapePacket(J3DShapePacket* pShape) {
    if (mpShapePacket == NULL) {
        mpShapePacket = pShape;
    } else {
        pShape->setNextPacket(mpShapePacket);
        mpShapePacket = pShape;
    }
}

void J3DMatPacket::beginDiff() {
#ifdef TARGET_PC
    mpInitShapePacket->getDisplayListObj()->beginDiffDL();  // (GXV_J3D_DIFFKEEP)
#else
    mpInitShapePacket->beginDL();
#endif
}

void J3DMatPacket::endDiff() {
#ifdef TARGET_PC
    mpInitShapePacket->getDisplayListObj()->endDiffDL();
#else
    mpInitShapePacket->endDL();
#endif
}

bool J3DMatPacket::isSame(J3DMatPacket* pOther) const {
    return mDiffFlag == pOther->mDiffFlag && (mDiffFlag >> 31) == 0;
}

void J3DMatPacket::draw() {
    mpMaterial->load();
    callDL();

    J3DShapePacket* packet = getShapePacket();
    packet->getShape()->loadPreDrawSetting();

    while (packet != NULL) {
        if (packet->getDisplayListObj() != NULL) {
            packet->getDisplayListObj()->callDL();
        }

        packet->drawFast();
        packet = (J3DShapePacket*)packet->getNextPacket();
    }

    J3DShape::resetVcdVatCache();
}

static u32 sDifferedRegister[7] = {
    J3DDiffFlag_MatColor, J3DDiffFlag_ColorChan,  J3DDiffFlag_TevReg,
    J3DDiffFlag_Fog,      J3DDiffFlag_Blend,    J3DDiffFlag_KonstColor, J3DDiffFlag_TevStageIndirect,
};

static s32 sSizeOfDiffered[7] = {
    13, 21, 120, 55, 15, 19, 45,
};

J3DShapePacket::J3DShapePacket() {
    mpShape = NULL;
    mpMtxBuffer = NULL;
    mpBaseMtxPtr = NULL;
    mDiffFlag = 0;
    mpModel = NULL;
}

J3DShapePacket::~J3DShapePacket() {
}

u32 J3DShapePacket::calcDifferedBufferSize(u32 diffFlags) {
    u32 bufferSize = 0;

    /* SMG1 change -- 7 iterations instead of 8 */
    for (u32 i = 0; i < 7; i++) {
        if ((diffFlags & sDifferedRegister[i]) != 0) {
            bufferSize += sSizeOfDiffered[i];
        }
    }

    u32 lightObjNum = getDiffFlag_LightObjNum(diffFlags);
    bufferSize += lightObjNum * 0x48;

    u32 texGenNum = getDiffFlag_TexGenNum(diffFlags);
    if (texGenNum != 0) {
        u32 mat_texGenNum = mpShape->getMaterial()->getTexGenNum();
        u32 sp30 = texGenNum > mat_texGenNum ? texGenNum : mat_texGenNum;

        if (diffFlags & J3DDiffFlag_TexGen) {
            bufferSize += sp30 * 0x3d + 10;
        } else {
            bufferSize += calcDifferedBufferSize_TexMtxSize(sp30);
        }
    }

    u32 texNoNum = getDiffFlag_TexNoNum(diffFlags);
    if (texNoNum != 0) {
        u8 sp9;
        if (mpShape->getMaterial()->getTevStageNum() > 8) {
            sp9 = 8;
        } else {
            sp9 = mpShape->getMaterial()->getTevStageNum();
        }

        u32 mat_texNoNum = sp9;
        u32 sp24 = texNoNum > mat_texNoNum ? texNoNum : mat_texNoNum;

        if (diffFlags & J3DDiffFlag_TexCoordScale) {
            bufferSize += calcDifferedBufferSize_TexNoAndTexCoordScaleSize(sp24);
        } else {
            bufferSize += calcDifferedBufferSize_TexNoSize(sp24);
        }
    }

    u32 tevStageNum = getDiffFlag_TevStageNum(diffFlags);
    if (tevStageNum != 0) {
        u8 sp8;
        if (mpShape->getMaterial()->getTevStageNum() > 8) {
            sp8 = 8;
        } else {
            sp8 = mpShape->getMaterial()->getTevStageNum();
        }

        u32 mat_tevStageNum = sp8;
        u32 sp18 = tevStageNum > mat_tevStageNum ? tevStageNum : mat_tevStageNum;

        bufferSize += calcDifferedBufferSize_TevStageSize(sp18);
        if (diffFlags & J3DDiffFlag_TevStageIndirect) {
            bufferSize += calcDifferedBufferSize_TevStageDirectSize(sp18);
        }
    }

    return OSRoundUp32B(bufferSize);
}

J3DError J3DShapePacket::newDifferedDisplayList(u32 diffFlags) {
    mDiffFlag = diffFlags;

    u32 bufSize = calcDifferedBufferSize(diffFlags);
    J3DError ret = newDisplayList(bufSize);
    if (ret != kJ3DError_Success) {
        return ret;
    }

    return kJ3DError_Success;
}

void J3DShapePacket::prepareDraw() const {
    mpModel->getVertexBuffer()->setArray();
    j3dSys.setModel(mpModel);
    j3dSys.setShapePacket((J3DShapePacket*)this);

    J3DShapeMtx::setLODFlag(mpModel->checkFlag(J3DMdlFlag_EnableLOD) != 0);

    if (mpModel->checkFlag(J3DMdlFlag_SkinPosCpu)) {
        mpShape->onFlag(J3DShpFlag_SkinPosCpu);
    } else {
        mpShape->offFlag(J3DShpFlag_SkinPosCpu);
    }

    if (mpModel->checkFlag(J3DMdlFlag_SkinNrmCpu) && mpShape->checkFlag(J3DShpFlag_EnableLod) == false) {
        mpShape->onFlag(J3DShpFlag_SkinNrmCpu);
    } else {
        mpShape->offFlag(J3DShpFlag_SkinNrmCpu);
    }

    mpShape->setCurrentViewNoPtr(mpMtxBuffer->getCurrentViewNoPtr());
    mpShape->setScaleFlagArray(mpMtxBuffer->getScaleFlagArray());
    mpShape->setDrawMtx(mpMtxBuffer->getDrawMtxPtrPtr());

    if (!mpShape->getNBTFlag()) {
        mpShape->setNrmMtx(mpMtxBuffer->getNrmMtxPtrPtr());
    } else {
        mpShape->setNrmMtx(mpMtxBuffer->mpBumpMtxArr[1][mpShape->getBumpMtxOffset()]);
    }

    mpModel->getModelData()->syncJ3DSysFlags();
}

void J3DShapePacket::draw() {
    if (!checkFlag(J3DShpFlag_Hidden) && mpShape != NULL) {
        prepareDraw();

        if (mpTexMtxObj != NULL) {
            J3DDifferedTexMtx::sTexGenBlock = mpShape->getMaterial()->getTexGenBlock();
            J3DDifferedTexMtx::sTexMtxObj = getTexMtxObj();
        } else {
            J3DDifferedTexMtx::sTexGenBlock = NULL;
        }

        if (mpDisplayListObj != NULL) {
            mpDisplayListObj->callDL();
        }

        mpShape->draw();
    }
}

void J3DShapePacket::drawFast() {
    if (!checkFlag(J3DShpFlag_Hidden) && mpShape != NULL) {
        prepareDraw();

        if (mpTexMtxObj != NULL) {
            J3DDifferedTexMtx::sTexGenBlock = mpShape->getMaterial()->getTexGenBlock();
            J3DDifferedTexMtx::sTexMtxObj = getTexMtxObj();
        } else {
            J3DDifferedTexMtx::sTexGenBlock = NULL;
        }

        mpShape->drawFast();
    }
}



int J3DMatPacket::entry(J3DDrawBuffer* pBuffer) {
    J3DDrawBuffer::sortFunc func = J3DDrawBuffer::sortFuncTable[pBuffer->getSortMode()];
    return (pBuffer->*func)(this);
}

GDLObj J3DDisplayListObj::sGDLObj;
s32 J3DDisplayListObj::sInterruptFlag;
