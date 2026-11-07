#include "Game/System/DrawBuffer.hpp"
#include "Game/LiveActor/ActorLightCtrl.hpp"
#include "Game/System/ShapePacketUserData.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphBase/J3DMaterial.hpp>
#include <cstring>
#ifdef TARGET_PC
#include <JSystem/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3DGraphBase/J3DPortDL.hpp>
#include <JSystem/J3DGraphBase/J3DShape.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#endif

DrawBufferShapeDrawer::DrawBufferShapeDrawer(J3DMaterial* pMaterial, J3DMatPacket* pMatPacket)
    : mMaterial(pMaterial), mMatPacket(pMatPacket), _8(1), mMaxPackets(0), mNumPackets(0), mPackets(nullptr) {
#ifdef TARGET_PC
    mMergedDL = nullptr;
    mMergedCap = 0;
    mMergedSize = 0;
    mMergedMatSize = 0;
    mMergedDiffSize = 0;
    mMergedLastCall = 0;
    mMergedQuietUntil = 0;
    mMergedChanges = 0;
#endif
}

void DrawBufferShapeDrawer::init(s32 a1) {
    mMaxPackets = a1 * _8;
    mPackets = new PacketInfo*[mMaxPackets];

    for (s32 idx = 0; idx < mMaxPackets; idx++) {
        PacketInfo* packet = new PacketInfo();
        mPackets[idx] = packet;
        mPackets[idx]->mShapePacket = nullptr;
        mPackets[idx]->mLightLoader = nullptr;
        mPackets[idx]->mLightCtrl = nullptr;
    }
}

void DrawBufferShapeDrawer::swap(DrawBufferShapeDrawer* pOther) {
    J3DMaterial* material = mMaterial;
    J3DMatPacket* matPacket = mMatPacket;
    s32 __8 = _8;

    mMaterial = pOther->mMaterial;
    mMatPacket = pOther->mMatPacket;
    _8 = pOther->_8;

    pOther->mMaterial = material;
    pOther->mMatPacket = matPacket;
    pOther->_8 = __8;
#ifdef TARGET_PC
    // (made at the table's sorting, before any draw: no merged list yet; dropped, so that none follows the wrong material)
    mMergedDL = pOther->mMergedDL = nullptr;
    mMergedCap = pOther->mMergedCap = 0;
    mMergedSize = pOther->mMergedSize = 0;
    mMergedMatSize = pOther->mMergedMatSize = 0;
    mMergedDiffSize = pOther->mMergedDiffSize = 0;
#endif
}

void DrawBufferShapeDrawer::draw() const {
#ifdef TARGET_PC
    if (J3DPortMatMergeOn() || J3DPortDiffSkipOn()) {
        drawPort();
        return;
    }
#endif
    mMatPacket->mpDisplayListObj->callDL();

    J3DShapePacket* firstPacket = mPackets[0]->mShapePacket;
    firstPacket->mpShape->loadPreDrawSetting();
    if (MR::getJ3DShapePacketUserData(firstPacket) != nullptr) {
        MR::getJ3DShapePacketUserData(firstPacket)->callDL();
    }

    for (s32 idx = 0; idx < mNumPackets; idx++) {
        J3DShapePacket* packet = mPackets[idx]->mShapePacket;

        if (getLightLoader(idx) != nullptr) {
            getLightLoader(idx)->loadLight();
        }

        if ((packet->mpShape->mFlags & 1) == 0) {
            if (packet->mpDisplayListObj != nullptr) {
                packet->mpDisplayListObj->callDL();
            }
            packet->drawFast();
        }
    }
}

#ifdef TARGET_PC
extern "C" void port_gx_call_prologue(void);  // RVL_SDK/gx/GXDisplayList.c
// ============================================================== (1.3 port) GXV_J3D_MATMERGE, GXV_J3D_DIFFSKIP
// What draw() sends, per drawer (one material of one model, for every actor of the model in the buffer):
//   the material's list; the first shape's vertex-format list (when it is not the last one sent) and matrix index
//   (J3DShape::loadPreDrawSetting); the first packet's user list (environment maps); then per packet: its light (the first
//   of a light group), its differed list (J3DShapePacket's: the instance's animated TEV colours, texture matrices, fog, ...)
//   and its shape (J3DShape::drawFast: array bases, matrix index, position / normal matrices, primitives).
// The front end's cost is mostly per list call. Two cuts, both leaving the front end's registers as draw() leaves them at every
// draw and at the end (see J3DPortDL.hpp for what a "pure" list is, and for the classes of what lists write):
//   GXV_J3D_MATMERGE=1 (default): when the first packet has a differed list, no light and no user list, the material's list and
//     that differed list are one call of a list holding both, one after the other (callMergedList). The differed list then comes
//     before loadPreDrawSetting's writes (CP registers, XF 0x1008 / 0x1018 / 0x1019) instead of after them: it must write none of
//     them, and nothing between reads a register (no draw). The merged list is in the drawer's heap (guest memory, as the front
//     end's list cache needs), made when the two lists' bytes change, and called only from the picture after it was made, when
//     its bytes are still those of the two lists (compared on every call). It is never written in a picture that follows one that
//     called it: the front end may still be reading the picture before (the guarantee J3D's own double buffers rely on).
//     Lists that keep changing (animated materials) are left alone for 300 pictures after three changes in a row.
//   GXV_J3D_DIFFSKIP=1 (default): a differed list with the bytes of the last one sent (instances of a model in the same state) is not
//     sent when between them there were only shapes and no light: a pure list that writes no CP register, no XF 0x1008 / 0x1018 /
//     0x1019 and no position or normal matrix (what a shape writes) is then writing every register with the value it holds. A light,
//     or a shape that may also load texture matrices (a differed texture matrix, user data, a normal matrix in texture memory), ends
//     the run. A hidden shape sends nothing and does not. The call's own work (the GX library's deferred state) is still done there.
//   With GXV_J3D_MATMERGE likewise, at the place of the first packet's differed list.
// Either switch at 0: the same calls as before for that part; both at 0: draw() exactly as before. Counters: the "j3d lists:" line.
void DrawBufferShapeDrawer::drawPort() const {
    J3DPortListStats& st = gJ3DPortListStats;
    ++st.drawers;
    J3DShapePacket* firstPacket = mPackets[0]->mShapePacket;
    ShapePacketUserData* firstUser = MR::getJ3DShapePacketUserData(firstPacket);
    J3DDisplayListObj* mergedDiff = nullptr;
    if (J3DPortMatMergeOn() && mNumPackets > 0 && firstUser == nullptr && getLightLoader(0) == nullptr && (firstPacket->mpShape->mFlags & 1) == 0 &&
        firstPacket->mpDisplayListObj != nullptr && callMergedList(mMatPacket->mpDisplayListObj, firstPacket->mpDisplayListObj)) {
        mergedDiff = firstPacket->mpDisplayListObj;
        ++st.matMerged;
    } else {
        mMatPacket->mpDisplayListObj->callDL();
    }

    firstPacket->mpShape->loadPreDrawSetting();
    if (firstUser != nullptr) {
        firstUser->callDL();
    }

    // (the last differed list sent and its flags, J3DPortScanDL, worked out when another list matches its bytes)
    const bool skipOn = J3DPortDiffSkipOn();
    const u8* last = nullptr;
    u32 lastSize = 0;
    u32 lastFlags = 0;
    if (mergedDiff != nullptr) {
        last = mergedDiff->getDisplayList(0);
        lastSize = mergedDiff->getDisplayListSize();
    }

    for (s32 idx = 0; idx < mNumPackets; idx++) {
        J3DShapePacket* packet = mPackets[idx]->mShapePacket;

        if (getLightLoader(idx) != nullptr) {
            getLightLoader(idx)->loadLight();
            last = nullptr;
        }

        if ((packet->mpShape->mFlags & 1) == 0) {
            J3DDisplayListObj* dl = packet->mpDisplayListObj;
            if (dl != nullptr) {
                const u8* bytes = dl->getDisplayList(0);
                const u32 size = dl->getDisplayListSize();
                if (idx == 0 && mergedDiff != nullptr) {
                    port_gx_call_prologue();  // (sent in the merged list; the call's own work, as it would have been done here)
                    ++st.diffMerged;
                } else if (skipOn && last != nullptr && size == lastSize && size != 0 && (bytes == last || memcmp(bytes, last, size) == 0) &&
                           J3DPortDiffSkippable(lastFlags != 0 ? lastFlags : (lastFlags = J3DPortScanDL(last, lastSize)))) {
                    port_gx_call_prologue();  // (the call's own work: state set since is sent here, as the call would have sent it)
                    ++st.diffSkipped;
                } else {
                    dl->callDL();
                    ++st.diffSent;
                    if (bytes != last || size != lastSize) {
                        lastFlags = 0;
                    }
                    last = bytes;
                    lastSize = size;
                }
            }
            packet->drawFast();
            if (last != nullptr && (packet->getUserArea() != nullptr || packet->getTexMtxObj() != nullptr || packet->mpShape->getTexMtxLoadType() == 0x2000)) {
                last = nullptr;  // (the shape may have loaded texture matrices)
            }
        }
    }
}

// The merged list's call (true), or nothing (false: the caller sends the material's list itself). See above.
bool DrawBufferShapeDrawer::callMergedList(J3DDisplayListObj* pMatDL, J3DDisplayListObj* pDiffDL) const {
    const u32 picture = J3DPortPicture();
    if (picture < mMergedQuietUntil) {
        return false;
    }
    const u8* mat = pMatDL->getDisplayList(0);
    const u8* diff = pDiffDL->getDisplayList(0);
    const u32 matSize = pMatDL->getDisplayListSize();
    const u32 diffSize = pDiffDL->getDisplayListSize();
    if (mat == nullptr || diff == nullptr || matSize == 0 || diffSize == 0 || ((matSize | diffSize) & 0x1F) != 0) {
        return false;
    }
    // (the block: the merged list's room, matSize + diffSize + 0x80, then the copy of the two lists)
    const u32 room = matSize + diffSize + 0x80;
    if (mMergedMatSize == matSize && mMergedDiffSize == diffSize && memcmp(mMergedDL + room, mat, matSize) == 0 &&
        memcmp(mMergedDL + room + matSize, diff, diffSize) == 0) {
        // (made in an earlier picture from these bytes)
        mMergedChanges = 0;
        mMergedLastCall = picture;
        GXCallDisplayList(mMergedDL, mMergedSize);
        return true;
    }
    // Changed (or new): made again, for the next picture, unless the front end may still read it.
    if (mMergedLastCall != 0 && mMergedLastCall + 1 >= picture) {
        return false;
    }
    if (++mMergedChanges >= 3) {
        mMergedChanges = 0;
        mMergedQuietUntil = picture + 300;  // (an animated material: its differed list changes on every picture)
        return false;
    }
    if (!J3DPortMergeable(J3DPortScanDL(mat, matSize), J3DPortScanDL(diff, diffSize))) {
        mMergedQuietUntil = picture + 600;
        return false;
    }
    if (mMergedDL == nullptr || mMergedCap < room + matSize + diffSize) {
        // In the heap that holds the drawer (the scene's), and only with room to spare there; not freed (the old one may be read by
        // the front end still), the heap goes with the scene.
        static const u32 kRoomLeft = 0x40000;
        const u32 cap = (room + matSize + diffSize + 0x7F) & ~0x1Fu;
        JKRHeap* heap = JKRHeap::findFromRoot(const_cast< DrawBufferShapeDrawer* >(this));
        void* p = nullptr;
        if (heap != nullptr && heap->getHeapType() == 'EXPH' && heap->getFreeSize() >= static_cast< s32 >(cap + kRoomLeft)) {
            p = JKRHeap::alloc(cap, 0x20, heap);
        }
        if (p == nullptr) {
            mMergedQuietUntil = picture + 3600;
            return false;
        }
        mMergedDL = static_cast< u8* >(p);
        mMergedCap = cap;
    }
    mMergedMatSize = 0;  // (until it is made)
    const u32 size = J3DPortBuildMerged(mat, matSize, diff, diffSize, mMergedDL, room);
    if (size == 0) {
        mMergedQuietUntil = picture + 600;  // (a register the two lists write with different masks: not merged)
        return false;
    }
    memcpy(mMergedDL + room, mat, matSize);
    memcpy(mMergedDL + room + matSize, diff, diffSize);
    mMergedSize = size;
    mMergedMatSize = matSize;
    mMergedDiffSize = diffSize;
    DCStoreRange(mMergedDL, size);  // (the front end's list cache learns the new bytes)
    ++gJ3DPortListStats.mergeWrites;
    return false;
}
#endif

void DrawBufferShapeDrawer::add(const J3DShapePacket* pShapePacket, const ActorLightCtrl* pLightCtrl) {
    bool setLightLoader = false;
    s32 index = findLightSortIndex(pLightCtrl, &setLightLoader);

    PacketInfo* packet = mPackets[mNumPackets];
    packet->mShapePacket = const_cast< J3DShapePacket* >(pShapePacket);
    packet->mLightCtrl = const_cast< ActorLightCtrl* >(pLightCtrl);

    if (setLightLoader) {
        packet->mLightLoader = const_cast< ActorLightCtrl* >(pLightCtrl);
    } else {
        packet->mLightLoader = nullptr;
    }

    for (s32 idx = mNumPackets - 1; idx >= index; idx--) {
        mPackets[idx + 1] = mPackets[idx];
    }

    mPackets[index] = packet;
    mNumPackets++;
}

void DrawBufferShapeDrawer::remove(const J3DShapePacket* pShapePacket) {
    PacketInfo* packet;
    for (s32 removeIdx = 0; removeIdx < mNumPackets; removeIdx++) {
        packet = mPackets[removeIdx];
        if (packet->mShapePacket == pShapePacket) {
            for (s32 idx = removeIdx; idx < mNumPackets - 1; idx++) {
                mPackets[idx] = mPackets[idx + 1];
            }
            if (packet->mLightLoader != nullptr && removeIdx < mNumPackets - 1) {
                mPackets[removeIdx]->mLightLoader = mPackets[removeIdx]->mLightCtrl;
            }
            mNumPackets--;
            mPackets[mNumPackets] = packet;
            return;
        }
    }
}

void DrawBufferShapeDrawer::resetLightSort(const ActorLightCtrl* pLightCtrl) {
    PacketInfo* packet;
    for (s32 idx = 0; idx < mNumPackets; idx++) {
        packet = mPackets[idx];
        if (packet->mLightCtrl == pLightCtrl) {
            J3DShapePacket* shapePacket = packet->mShapePacket;
            remove(shapePacket);
            add(shapePacket, pLightCtrl);
            return;
        }
    }
}

s32 DrawBufferShapeDrawer::findLightSortIndex(const ActorLightCtrl* pLightCtrl, bool* pSetLightLoader) const {
    *pSetLightLoader = false;

    if (pLightCtrl == nullptr) {
        return mNumPackets;
    }

    if (pLightCtrl->_1C != 0) {
        *pSetLightLoader = true;
        return mNumPackets;
    }

    bool foundLight = false;
    for (s32 idx = 0; idx < mNumPackets; idx++) {
        if (mPackets[idx]->mLightLoader != nullptr) {
            if (foundLight) {
                return idx;
            }

            if (pLightCtrl->isSameLight(mPackets[idx]->mLightLoader)) {
                foundLight = true;
            }
        }
    }

    if (!foundLight) {
        *pSetLightLoader = true;
    }
    return mNumPackets;
}

DrawBuffer::DrawBuffer(J3DModel* pModel)
    : mModelData(pModel->mModelData), mModel(pModel), _8(0), mNumActors(0), mNumMaterials(0), mMaterialNos(nullptr), mNumShapeDrawers(0),
      mNumOpaShapeDrawers(0), mShapeDrawers(nullptr) {
}

void DrawBuffer::init(int a1) {
    _8 = a1;
    initTable();
}

void DrawBuffer::add(const LiveActor* pActor) {
    J3DModel* model = MR::getJ3DModel(pActor);
    ActorLightCtrl* lightCtrl = MR::getLightCtrl(pActor);

    for (s32 idx = 0; idx < mNumMaterials; idx++) {
        getShapeDrawerByMatNo(idx)->add(model->getShapePacket(mModelData->mMaterialTable.getMaterialNodePointer(idx)->getShape()->mIndex), lightCtrl);
    }
    mNumActors++;
}

void DrawBuffer::remove(const LiveActor* pActor) {
    J3DModel* model = MR::getJ3DModel(pActor);

    for (s32 idx = 0; idx < mNumMaterials; idx++) {
        getShapeDrawerByMatNo(idx)->remove(model->getShapePacket(mModelData->mMaterialTable.getMaterialNodePointer(idx)->getShape()->mIndex));
    }
    mNumActors--;
}

void DrawBuffer::resetLightSort(const ActorLightCtrl* pLightCtrl) {
    J3DModel* model = MR::getJ3DModel(pLightCtrl->mActor);

    for (s32 idx = 0; idx < mNumMaterials; idx++) {
        getShapeDrawerByMatNo(idx)->resetLightSort(pLightCtrl);
    }
}

void DrawBuffer::drawOpa() const {
    if (mNumActors > 0) {
        J3DShape::sOldVcdVatCmd = nullptr;
        for (s32 idx = 0; idx < mNumOpaShapeDrawers; idx++) {
            getShapeDrawerByIndex(idx)->draw();
        }
    }
}

void DrawBuffer::drawXlu() const {
    if (mNumActors > 0) {
        J3DShape::sOldVcdVatCmd = nullptr;
        for (s32 idx = mNumOpaShapeDrawers; idx < mNumShapeDrawers; idx++) {
            getShapeDrawerByIndex(idx)->draw();
        }
    }
}

namespace {
    s32 getSortedMaterialIndex(int materialIndex, J3DModel* pModel) {
        J3DMaterial* material;
        material = MR::getMaterial(pModel, materialIndex);
        for (s32 idx = 0; idx < materialIndex; idx++) {
            if (MR::getMaterial(pModel, idx)->getDiffFlag() == material->getDiffFlag()) {
                return idx;
            }
        }
        return -1;
    }
};  // namespace

void DrawBuffer::initTable() {
    mNumMaterials = MR::getMaterialNum(mModel);
    mNumShapeDrawers = mModelData->mMaterialTable.mUniqueMatNum;

    J3DModel* model = mModel;
    s32 idx, numOpaMaterials;
    numOpaMaterials = 0;
    for (idx = 0; idx < MR::getMaterialNum(model); idx++) {
        if (!MR::getMaterial(model, idx)->isDrawModeOpaTexEdge()) {
            if (::getSortedMaterialIndex(idx, model) < 0) {
                numOpaMaterials++;
            }
        }
    }
    mNumOpaShapeDrawers = numOpaMaterials;

    mMaterialNos = new s32[mNumMaterials];
    for (s32 idx = 0; idx < mNumMaterials; idx++) {
        mMaterialNos[idx] = -1;
    }

    mShapeDrawers = new DrawBufferShapeDrawer*[mNumShapeDrawers];

    for (s32 idx = 0; idx < mNumShapeDrawers; idx++) {
        mShapeDrawers[idx] = nullptr;
    }

    s32 opaIndex = 0;
    s32 xluIndex = 0;
    for (s32 idx = 0; idx < mNumMaterials; idx++) {
        J3DMaterial* material = MR::getMaterial(mModel, idx);
        s32 index = ::getSortedMaterialIndex(idx, mModel);

        if (index >= 0) {
            s32 matNo = mMaterialNos[index];
            mMaterialNos[idx] = matNo;
            mShapeDrawers[matNo]->_8++;
            continue;
        }

        if (!material->isDrawModeOpaTexEdge()) {
            mMaterialNos[idx] = opaIndex;
            mShapeDrawers[opaIndex] = new DrawBufferShapeDrawer(material, mModel->getMatPacket(idx));
            opaIndex++;
        } else {
            mMaterialNos[idx] = xluIndex + mNumOpaShapeDrawers;
            mShapeDrawers[xluIndex + mNumOpaShapeDrawers] = new DrawBufferShapeDrawer(material, mModel->getMatPacket(idx));
            xluIndex++;
        }
    }

    // sort Opa
    for (s32 idx = 0; idx < mNumOpaShapeDrawers - 1; idx++) {
        for (s32 idx2 = idx + 1; idx2 < mNumOpaShapeDrawers; idx2++) {
            sortShapeDrawer(idx, idx2);
        }
    }

    // sort Xlu
    for (s32 idx = mNumOpaShapeDrawers; idx < mNumShapeDrawers - 1; idx++) {
        for (s32 idx2 = idx + 1; idx2 < mNumShapeDrawers; idx2++) {
            sortShapeDrawer(idx, idx2);
        }
    }

    for (s32 idx = 0; idx < mNumShapeDrawers; idx++) {
        mShapeDrawers[idx]->init(_8);
    }
}

void DrawBuffer::sortShapeDrawer(s32 matNo1, s32 matNo2) {
    DrawBufferShapeDrawer* drawerA = getShapeDrawerByIndex(matNo1);
    DrawBufferShapeDrawer* drawerB = getShapeDrawerByIndex(matNo2);
    const char* nameA = MR::getMaterialName(mModelData, drawerA->mMaterial->mIndex);
    const char* nameB = MR::getMaterialName(mModelData, drawerB->mMaterial->mIndex);

    if (strcasecmp(nameA, nameB) > 0) {
        drawerA->swap(drawerB);

        for (s32 idx = 0; idx < mNumMaterials; idx++) {
            if (matNo1 == mMaterialNos[idx]) {
                mMaterialNos[idx] = matNo2;
            } else if (matNo2 == mMaterialNos[idx]) {
                mMaterialNos[idx] = matNo1;
            }
        }
    }
}
