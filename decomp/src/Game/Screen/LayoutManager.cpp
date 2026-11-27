#include "Game/Screen/LayoutManager.hpp"
#include "Game/Screen/LayoutCoreUtil.hpp"
#include "Game/Screen/LayoutGroupCtrl.hpp"
#include "Game/Screen/LayoutPaneCtrl.hpp"
#include "Game/System/Language.hpp"
#include "Game/System/LayoutHolder.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/HashUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/MessageUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/StringUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include <JSystem/JUtility/JUTTexture.hpp>
#include <cstdio>
#include <cstring>
#include <nw4r/lyt/group.h>
#include <nw4r/lyt/layout.h>
#include <nw4r/lyt/material.h>
#include <nw4r/lyt/pane.h>
#include <nw4r/lyt/picture.h>
#include <nw4r/lyt/texMap.h>
#include <nw4r/lyt/textBox.h>
#include <nw4r/ut/Rect.h>
#include <nw4r/ut/RuntimeTypeInfo.h>
#include <revolution/mtx.h>
#include <revolution/types.h>
#ifdef TARGET_PC
#include <revolution/gx.h>
#include <atomic>
#include <cstdlib>
// layoutwarm24 (GXV_LAYOUTWARM, GXV_LAYOUTWARM_LOG): the registry of the layouts a scene made, the warm-up and the census; at the end of this file.
static void PortLayoutRegister(LayoutManager* pManager);
static unsigned PortLayoutCensusBegin(const LayoutManager* pManager);
static void PortLayoutCensusEnd(unsigned id);
#endif

namespace {
    namespace Local {
        class BitFlagBase {
        public:
            virtual ~BitFlagBase() {
            }
            virtual void onBit(int) = 0;
            virtual void offBit(int) = 0;
            virtual bool isTrue(int) const = 0;
            virtual bool isAnythingTrue() const = 0;
        };

        template < int N >
        class BitFlag : public BitFlagBase {
        public:
            BitFlag() NO_INLINE {
                MR::zeroMemory(mBits, sizeof(mBits));
            }

            virtual bool isAnythingTrue() const;
            virtual void onBit(int);
            virtual void offBit(int);
            virtual bool isTrue(int) const;

            /* 0x04 */ u32 mBits[N / 32];
        };
    }  // namespace Local

    static const char* const cRemoveString[] = {
        "4x3",
        "16x9",
        "Replace",
    };
}  // namespace

namespace {
    namespace Local {
        inline void collectLanguagePanes(nw4r::lyt::Pane* pPane, BitFlagBase& rLanguagePanes, BitFlagBase& rCurrentLanguagePanes) {
            nw4r::lyt::PaneList& rPaneList = pPane->mChildList;
            s32 index = -1;
            for (nw4r::lyt::PaneList::Iterator it = rPaneList.GetBeginIter(); it != rPaneList.GetEndIter(); ++it) {
                index++;
                const char* pName = (*it).mName;
                u32 length = strlen(pName);
                if (length < 4) {
                    continue;
                }

                const char* pSuffix = pName + (length - 4);
                for (u32 i = 0; i < MR::getLanguageNum(); i++) {
                    if (strncmp(pSuffix, MR::getLanguagePrefixByIndex(i), 4) == 0) {
                        rLanguagePanes.onBit(index);
                        if (strncmp(pSuffix, MR::getCurrentLanguagePrefix(), 4) == 0) {
                            rCurrentLanguagePanes.onBit(index);
                        }

                        break;
                    }
                }
            }
        }

        inline void removeLanguagePanes(nw4r::lyt::Pane* pPane, const BitFlagBase& rLanguagePanes, const BitFlagBase& rCurrentLanguagePanes) {
            const char* pName;
            nw4r::lyt::PaneList& rChildren = pPane->mChildList;
            u32 length;
            s32 childIndex = 0;
            nw4r::lyt::PaneList::Iterator it = rChildren.GetBeginIter();
            if (rCurrentLanguagePanes.isAnythingTrue()) {
                while (it != rChildren.GetEndIter()) {
                    nw4r::lyt::PaneList::Iterator child = it;
                    it++;
                    if (rCurrentLanguagePanes.isTrue(childIndex)) {
                        pName = (*child).mName;
                        length = strlen(pName) - 4;
                        char name[17];
                        strncpy(name, pName, length);
                        name[length] = '\0';
                        (*child).SetName(name);
                    } else {
                        pPane->RemoveChild(&*child);
                    }

                    childIndex++;
                }
            } else {
                while (it != rChildren.GetEndIter()) {
                    nw4r::lyt::PaneList::Iterator child = it;
                    it++;
                    if (rLanguagePanes.isTrue(childIndex)) {
                        pPane->RemoveChild(&*child);
                    }

                    childIndex++;
                }
            }
        }
    }  // namespace Local
}  // namespace

LayoutManager::LayoutManager(const char* pLayoutName, bool useArchiveNamePrefix, u32 rootPaneAnimLayerNum, u32 textBoxBufferLength)
    : mLayoutHolder(), mLayout(), mAnimTransList(), mDrawInfo(), mIsScreenHidden(), _61(true), mIndDummyTexMap(), mPaneCount(), mPaneInfoList(),
      mGroupCtrlCount(), mGroupCtrlList(), mLayoutName() {
    // Function scope: pLayoutName may point into fileNameWithoutExtension
    // (the original declares these inside the if block).
    char fileNameWithoutExtension[0x60];
    char fileNameFromPrefix[0x80];
    if (useArchiveNamePrefix) {
        MR::makeLayoutArchiveFileNameFromPrefix(fileNameFromPrefix, sizeof(fileNameFromPrefix), pLayoutName, true);
        MR::removeExtensionString(fileNameWithoutExtension, sizeof(fileNameWithoutExtension), fileNameFromPrefix);
        pLayoutName = MR::getBasename(fileNameWithoutExtension);
    }

    char archiveName[0x40];
    snprintf(archiveName, sizeof(archiveName), "%s.arc", pLayoutName);

    char fileName[0x40];
    MR::copyString(fileName, pLayoutName, sizeof(fileName));

    for (u32 i = 0; i < ARRAY_SIZE(::cRemoveString); i++) {
        char* pos = strstr(fileName, ::cRemoveString[i]);

        if (pos != nullptr) {
            pos[0] = '\0';
            break;
        }
    }

    u32 len = strlen(fileName) + 1;
    mLayoutName = new char[len];
    MR::copyString(mLayoutName, fileName, len);

    initArc(archiveName, pLayoutName);
    initPaneInfo();
    initGroupCtrlList();
    initDrawInfo();
    initTextBoxRecursive(mLayout->mpRootPane, nullptr, mLayoutName, textBoxBufferLength);
    replaceIndDummyTexture();

    if (rootPaneAnimLayerNum != 0) {
        createAndAddRootPaneCtrl(rootPaneAnimLayerNum);
    }

    mLayout->Animate(0);
    mLayout->CalculateMtx(mDrawInfo);
#ifdef TARGET_PC
    PortLayoutRegister(this);
#endif
}

void LayoutManager::movement() {
    for (u32 i = 0; i < mPaneCount; i++) {
        if (mPaneInfoList[i].mPaneCtrl != nullptr) {
            mPaneInfoList[i].mPaneCtrl->movement();
        }
    }

    for (u32 i = 0; i < mGroupCtrlCount; i++) {
        if (mGroupCtrlList[i] != nullptr) {
            mGroupCtrlList[i]->movement();
        }
    }
}

void LayoutManager::calcAnim() {
    if (MR::isScreen16Per9()) {
        nw4r::math::VEC2 scale;
        scale.x = 0.75f;
        scale.y = 1.0f;
        mDrawInfo.SetLocationAdjustScale(scale);
    } else {
        nw4r::math::VEC2 scale;
        scale.x = 1.0f;
        scale.y = 1.0f;
        mDrawInfo.SetLocationAdjustScale(scale);
    }

    calcAnimWithoutLocationAdjust(mDrawInfo);
}

void LayoutManager::draw() const {
    if (mIsScreenHidden) {
        return;
    }

    MR::setupDrawForNW4RLayout(1.0f, true);
#ifdef TARGET_PC
    const unsigned censusId = PortLayoutCensusBegin(this);  // (GXV_LAYOUTWARM_LOG: 0 and nothing else without it)
    mLayout->Draw(mDrawInfo);
    if (censusId != 0) {
        PortLayoutCensusEnd(censusId);
    }
#else
    mLayout->Draw(mDrawInfo);
#endif
}

void LayoutManager::addPaneCtrl(LayoutPaneCtrl* pPaneCtrl) {
    s32 index = getIndexOfPane(pPaneCtrl->mPane->mName);

    if (mPaneInfoList[index].mPaneCtrl == nullptr) {
        pPaneCtrl->mPaneIndex = index;
        mPaneInfoList[index].mPaneCtrl = pPaneCtrl;
    }
}

LayoutPaneCtrl* LayoutManager::createAndAddRootPaneCtrl(u32 animLayerNum) {
    s32 index = getIndexOfPane(mLayout->mpRootPane->mName);

    if (mPaneInfoList[index].mPaneCtrl != nullptr) {
        return mPaneInfoList[index].mPaneCtrl;
    }

    LayoutPaneCtrl* pPaneCtrl = new LayoutPaneCtrl(this, mLayout->mpRootPane->mName, animLayerNum);
    addPaneCtrl(pPaneCtrl);
    return pPaneCtrl;
}

LayoutPaneCtrl* LayoutManager::createAndAddPaneCtrl(const char* pName, u32 animLayerNum) {
    s32 index = getIndexOfPane(pName);

    if (mPaneInfoList[index].mPaneCtrl != nullptr) {
        return mPaneInfoList[index].mPaneCtrl;
    }

    LayoutPaneCtrl* pPaneCtrl = new LayoutPaneCtrl(this, pName, animLayerNum);
    addPaneCtrl(pPaneCtrl);
    return pPaneCtrl;
}

LayoutPaneCtrl* LayoutManager::getPaneCtrl(const char* pName) const {
    if (pName == nullptr) {
        return mPaneInfoList[0].mPaneCtrl;
    }

    s32 index = getIndexOfPane(pName);
    return mPaneInfoList[index].mPaneCtrl;
}

#ifdef TARGET_PC
nw4r::lyt::Pane* LayoutManager::missingPane(const char* pName) {
    static char sLogged[64][17];
    static s32 sLoggedCount = 0;
    bool logged = false;
    for (s32 i = 0; i < sLoggedCount; i++) {
        logged = logged || strcmp(sLogged[i], pName) == 0;
    }
    if (!logged) {
        port_log("layout: no pane \"%s\" in this layout, ignored", pName);
        if (sLoggedCount < 64) {
            strncpy(sLogged[sLoggedCount], pName, 16);
            sLogged[sLoggedCount][16] = '\0';
            sLoggedCount++;
        }
    }
    // Built once in static storage (not on a game heap that a scene frees).
    alignas(nw4r::lyt::Pane) static u8 sStorage[sizeof(nw4r::lyt::Pane)];
    static nw4r::lyt::Pane* sPane = nullptr;
    if (sPane == nullptr) {
        static nw4r::lyt::res::Pane sRes;
        sRes.scale.x = 1.0f;
        sRes.scale.y = 1.0f;
        sPane = new (sStorage) nw4r::lyt::Pane(&sRes);
    }
    return sPane;
}
#endif

s32 LayoutManager::getIndexOfPane(const char* pName) const {
    u32 paneCount = mPaneCount;

    for (u32 i = 0; i < paneCount; i++) {
        if (strcmp(mPaneInfoList[i].mName, pName) == 0) {
            return i;
        }
    }

    return 0;
}

bool LayoutManager::isExistPaneCtrl(const char* pName) const {
    if (pName == nullptr) {
        return mPaneInfoList[0].mPaneCtrl != nullptr;
    }

    s32 index = getIndexOfPane(pName);
    return mPaneInfoList[index].mPaneCtrl != nullptr;
}

void LayoutManager::addGroupCtrl(LayoutGroupCtrl* pGroupCtrl) {
    s32 index = getIndexOfGroupCtrl(pGroupCtrl->mGroup->mName);
    mGroupCtrlList[index] = pGroupCtrl;

    for (u32 i = 0; i < pGroupCtrl->getPaneNum(); i++) {
        s32 paneIndex = getIndexOfPane(pGroupCtrl->getPane(i)->mName);
        LayoutGroupCtrlLink* pNext = mPaneInfoList[paneIndex].mGroupCtrlLink;
        LayoutGroupCtrlLink* pLink = new LayoutGroupCtrlLink;
        pLink->mGroupCtrl = pGroupCtrl;
        pLink->mNext = pNext;
        mPaneInfoList[paneIndex].mGroupCtrlLink = pLink;
    }
}

bool LayoutManager::isPointing(const nw4r::lyt::Pane* pPane, const TVec2f& rPos) const {
    Mtx inverse;
    PSMTXInverse(pPane->mGlbMtx, inverse);
    s32 horizontalPosition = static_cast< u8 >(pPane->mBasePosition % 3);
    s32 verticalPosition = static_cast< u8 >(pPane->mBasePosition / 3);
    TVec3f pos(0.0f, 0.0f, 0.0f);
    MR::convertScreenPosToLayoutPos(reinterpret_cast< TVec2f* >(&pos), rPos);
    PSMTXMultVec(inverse, &pos, &pos);

    switch (horizontalPosition) {
    case 0:
        if (pos.x < 0.0f || pPane->mSize.width < pos.x) {
            return false;
        }

        break;
    case 1:
        if (pos.x < -pPane->mSize.width / 2.0f || pPane->mSize.width / 2.0f < pos.x) {
            return false;
        }

        break;
    case 2:
        if (pos.x < -pPane->mSize.width || 0.0f < pos.x) {
            return false;
        }

        break;
    }

    switch (verticalPosition) {
    case 0:
        if (pos.y < -pPane->mSize.height || 0.0f < pos.y) {
            return false;
        }

        break;
    case 1:
        if (pos.y < -pPane->mSize.height / 2.0f || pPane->mSize.height / 2.0f < pos.y) {
            return false;
        }

        break;
    case 2:
        if (pos.y < 0.0f || pPane->mSize.height < pos.y) {
            return false;
        }

        break;
    }

    return true;
}

LayoutGroupCtrl* LayoutManager::createAndAddGroupCtrl(const char* pName, u32 animLayerNum) {
    LayoutGroupCtrl* pGroupCtrl = new LayoutGroupCtrl(this, pName, animLayerNum);
    addGroupCtrl(pGroupCtrl);
    return pGroupCtrl;
}

s32 LayoutManager::getIndexOfGroupCtrl(const char* pName) const {
    u32 index = 0;
    nw4r::lyt::GroupList& rGroupList = mLayout->mpGroupContainer->mGroupList;
    for (nw4r::lyt::GroupList::Iterator it = rGroupList.GetBeginIter(); it != rGroupList.GetEndIter(); it++) {
        if (strcmp(it->mName, pName) == 0) {
            return index;
        }

        index++;
    }

    return index;
}

void LayoutManager::createPaneMtxRef(const char* pName) {
    s32 index = 0;
    if (pName != nullptr) {
        index = getIndexOfPane(pName);
    }

    mPaneInfoList[index].mMtxRef = new f32[3][4];
}

MtxPtr LayoutManager::getPaneMtxRef(const char* pName) const {
    s32 index = 0;
    if (pName != nullptr) {
        index = getIndexOfPane(pName);
    }

    return mPaneInfoList[index].mMtxRef;
}

bool LayoutManager::isExistPaneMtxRef(const char* pName) const {
    s32 index = 0;
    if (pName != nullptr) {
        index = getIndexOfPane(pName);
    }

    return mPaneInfoList[index].mMtxRef != nullptr;
}

bool LayoutManager::isPointing(const char* pName, const TVec2f& rPos) const {
    return isPointing(findPaneByName(pName), rPos);
}

nw4r::lyt::AnimTransform* LayoutManager::getAnimTransform(const char* pName) const {
    char fileName[0x80];
    snprintf(fileName, sizeof(fileName), "%s.brlan", pName);
    u32 hash = MR::getHashCodeLower(fileName);
    for (u32 i = 0; i < mLayoutHolder->mAnimRes.mCount; i++) {
        if (mLayoutHolder->isAnimationHashEqual(hash, i)) {
            return mAnimTransList[i];
        }
    }

    return nullptr;
}

void LayoutManager::bindPaneCtrlAnim(LayoutPaneCtrl* pPaneCtrl, nw4r::lyt::AnimTransform* pAnimTransform) {
    u32 index = pPaneCtrl->mPaneIndex;
    mPaneInfoList[index].mPane->UnbindAnimationSelf(pAnimTransform);
    mPaneInfoList[index].mPane->BindAnimation(pAnimTransform, false);
    index++;

    while (index < pPaneCtrl->mPaneIndex + mPaneInfoList[pPaneCtrl->mPaneIndex].mChildCount) {
        bindPaneCtrlAnimSub(index, pAnimTransform);
    }
}

void LayoutManager::bindPaneCtrlAnimSub(u32& rIndex, nw4r::lyt::AnimTransform* pAnimTransform) {
    if (mPaneInfoList[rIndex].mPaneCtrl != nullptr) {
        rIndex += mPaneInfoList[rIndex].mChildCount;
        return;
    }

    mPaneInfoList[rIndex].mPane->UnbindAnimationSelf(pAnimTransform);
    mPaneInfoList[rIndex].mPane->BindAnimation(pAnimTransform, false);
    u32 startIndex = rIndex;
    rIndex++;

    while (rIndex < startIndex + mPaneInfoList[startIndex].mChildCount) {
        bindPaneCtrlAnimSub(rIndex, pAnimTransform);
    }
}

void LayoutManager::unbindPaneCtrlAnim(LayoutPaneCtrl* pPaneCtrl, nw4r::lyt::AnimTransform* pAnimTransform) {
    u32 index = pPaneCtrl->mPaneIndex;
    mPaneInfoList[index].mPane->UnbindAnimationSelf(pAnimTransform);
    index++;

    while (index < pPaneCtrl->mPaneIndex + mPaneInfoList[pPaneCtrl->mPaneIndex].mChildCount) {
        unbindPaneCtrlAnimSub(index, pAnimTransform);
    }
}

void LayoutManager::unbindPaneCtrlAnimSub(u32& rIndex, nw4r::lyt::AnimTransform* pAnimTransform) {
    if (mPaneInfoList[rIndex].mPaneCtrl != nullptr) {
        rIndex += mPaneInfoList[rIndex].mChildCount;
        return;
    }

    mPaneInfoList[rIndex].mPane->UnbindAnimationSelf(pAnimTransform);
    const u32 startIndex = rIndex;
    rIndex++;

    while (rIndex < startIndex + mPaneInfoList[startIndex].mChildCount) {
        unbindPaneCtrlAnimSub(rIndex, pAnimTransform);
    }
}

void LayoutManager::calcAnimWithoutLocationAdjust(const nw4r::lyt::DrawInfo& rDrawInfo) {
    u32 index = 0;
    animateRecursive(index, mLayout->mpRootPane);
    if (mIsScreenHidden) {
        return;
    }

    mLayout->CalculateMtx(rDrawInfo);
    for (u32 i = 0; i < mPaneCount; i++) {
        if (mPaneInfoList[i].mPaneCtrl != nullptr) {
            mPaneInfoList[i].mPaneCtrl->reflectFollowPos();
        }

        if (mPaneInfoList[i].mMtxRef != nullptr) {
            nw4r::lyt::Pane* pPane =
                mPaneInfoList[i].mPaneCtrl != nullptr ? mPaneInfoList[i].mPaneCtrl->mPane : findPaneByName(mPaneInfoList[i].mName);
            PSMTXCopy(pPane->mGlbMtx, mPaneInfoList[i].mMtxRef);
            if (MR::isScreen16Per9()) {
                mPaneInfoList[i].mMtxRef[0][3] *= MR::getScreenWidth() / 608.0f;
            }
        }
    }
}

nw4r::lyt::Group* LayoutManager::getGroup(const char* pName) const {
    nw4r::lyt::Group* pGroup = nullptr;
    nw4r::lyt::GroupList& rGroupList = mLayout->mpGroupContainer->mGroupList;
    for (nw4r::lyt::GroupList::Iterator it = rGroupList.GetBeginIter(); it != rGroupList.GetEndIter(); it++) {
        if (strcmp(it->mName, pName) == 0) {
            pGroup = &*it;
            break;
        }
    }

    return pGroup;
}

void LayoutManager::initArc(const char* pArchiveName, const char* pLayoutName) {
    mLayoutHolder = MR::createAndAddLayoutHolder(pArchiveName);

    char layoutResName[0x80];
    snprintf(layoutResName, sizeof(layoutResName), "%s.brlyt", pLayoutName);
    void* pLayoutRes = mLayoutHolder->GetResource('blyt', layoutResName, nullptr);
#ifdef TARGET_PC
    if (pLayoutRes == nullptr) {
        port_log("layout: %s not found in %s (%d layouts, %d anims, %d other)", layoutResName, pArchiveName, mLayoutHolder->mLayoutRes.mCount,
                 mLayoutHolder->mAnimRes.mCount, mLayoutHolder->mResOther.mCount);
        for (u32 i = 0; i < mLayoutHolder->mLayoutRes.mCount; i++) {
            port_log("layout:   %s", mLayoutHolder->mLayoutRes.getResName(i));
        }
    }
#endif

    mLayout = new nw4r::lyt::Layout();
    mLayout->Build(pLayoutRes, mLayoutHolder);

    if (mLayoutHolder->mAnimRes.mCount != 0) {
        mAnimTransList = new nw4r::lyt::AnimTransform*[mLayoutHolder->mAnimRes.mCount];

        for (u32 i = 0; i < mLayoutHolder->mAnimRes.mCount; i++) {
            void* pAnimRes = mLayoutHolder->mAnimRes.getRes(i);
            mAnimTransList[i] = mLayout->CreateAnimTransform(pAnimRes, mLayoutHolder);
        }
    }

    removeUnnecessaryPanes(mLayout->mpRootPane);
}

void LayoutManager::initDrawInfo() {
    nw4r::math::MTX34 mtx;
    PSMTXIdentity(mtx);

    mDrawInfo.mViewMtx = mtx;
    mDrawInfo.SetMultipleViewMtxOnDraw(true);
    mDrawInfo.mViewRect = mLayout->GetLayoutRect();
    mDrawInfo.SetLocationAdjust(true);
}

void LayoutManager::initPaneInfo() {
    mPaneCount = countPanes(mLayout->mpRootPane);
    mPaneInfoList = new LayoutPaneInfo[mPaneCount];

    u32 rIndex = 0;
    initPaneInfoRecursive(rIndex, mLayout->mpRootPane);
}

void LayoutManager::initPaneInfoRecursive(u32& rIndex, nw4r::lyt::Pane* pPane) {
    nw4r::lyt::PaneList& rPaneList = pPane->mChildList;

    mPaneInfoList[rIndex].mName = pPane->mName;
    mPaneInfoList[rIndex].mPaneCtrl = nullptr;
    mPaneInfoList[rIndex].mGroupCtrlLink = nullptr;
    mPaneInfoList[rIndex].mMtxRef = nullptr;
    mPaneInfoList[rIndex].mPane = pPane;

    const u32 startIndex = rIndex++;

    for (nw4r::lyt::PaneList::Iterator it = rPaneList.GetBeginIter(); it != rPaneList.GetEndIter(); ++it) {
        initPaneInfoRecursive(rIndex, &*it);
    }

    mPaneInfoList[startIndex].mChildCount = rIndex - startIndex;
}

u32 LayoutManager::countPanes(nw4r::lyt::Pane* pPane) {
    u32 count = 1;

    nw4r::lyt::PaneList& rPaneList = pPane->GetChildList();
    for (nw4r::lyt::PaneList::Iterator it = rPaneList.GetBeginIter(); it != rPaneList.GetEndIter(); ++it) {
        count += countPanes(&*it);
    }

    return count;
}

void LayoutManager::initGroupCtrlList() {
    mGroupCtrlCount = mLayout->mpGroupContainer->mGroupList.GetSize();
    mGroupCtrlList = new LayoutGroupCtrl*[mGroupCtrlCount];

    for (u32 i = 0; i < mGroupCtrlCount; i++) {
        mGroupCtrlList[i] = nullptr;
    }
}

void LayoutManager::initTextBoxRecursive(nw4r::lyt::Pane* pPane, nw4r::lyt::Pane* pUserDataPane, const char* pLayoutName, u32 textBoxBufferLength) {
    nw4r::lyt::TextBox* pTextBox;

    const nw4r::ut::detail::RuntimeTypeInfo* pTextBoxRuntimeInfo = &nw4r::lyt::TextBox::typeInfo;
    if (pPane != nullptr && pPane->GetRuntimeTypeInfo()->IsDerivedFrom(pTextBoxRuntimeInfo)) {
        pTextBox = static_cast< nw4r::lyt::TextBox* >(pPane);
    } else {
        pTextBox = nullptr;
    }

    char userData[9];
    LayoutCoreUtil::getPaneUserData(pPane, userData);

    if (!MR::isEqualString("", userData)) {
        pUserDataPane = pPane;
    }

    if (pTextBox != nullptr) {
        if (pUserDataPane != nullptr) {
            char messageID[0x100];
            LayoutCoreUtil::getPaneUserData(pUserDataPane, userData);
            MR::getLayoutMessageID(messageID, pLayoutName, userData);
            LayoutCoreUtil::initTextBoxPane(pTextBox, messageID, 0x100);
        } else {
            LayoutCoreUtil::initTextBoxPane(pTextBox, nullptr, textBoxBufferLength);
        }
    }

    nw4r::lyt::PaneList& rPaneList = pPane->mChildList;
    for (nw4r::lyt::PaneList::Iterator it = rPaneList.GetBeginIter(); it != rPaneList.GetEndIter(); ++it) {
        initTextBoxRecursive(&*it, pUserDataPane, pLayoutName, textBoxBufferLength);
    }
}

void LayoutManager::animateRecursive(u32& rIndex, nw4r::lyt::Pane* pPane) {
    if (mPaneInfoList[rIndex].mPaneCtrl != nullptr) {
        mPaneInfoList[rIndex].mPaneCtrl->calcAnim();
    }

    for (LayoutGroupCtrlLink* pLink = mPaneInfoList[rIndex].mGroupCtrlLink; pLink != nullptr; pLink = pLink->mNext) {
        pLink->mGroupCtrl->calcAnim();
    }

    pPane->AnimateSelf(1);
    rIndex++;

    for (nw4r::lyt::PaneList::Iterator it = pPane->mChildList.GetBeginIter(); it != pPane->mChildList.GetEndIter(); ++it) {
        animateRecursive(rIndex, &*it);
    }
}

nw4r::lyt::Pane* LayoutManager::findPaneByName(const char* pName) const {
    u32 paneCount = mPaneCount;
    for (u32 i = 0; i < paneCount; i++) {
        if (strcmp(mPaneInfoList[i].mName, pName) == 0) {
            return mPaneInfoList[i].mPane;
        }
    }

    return nullptr;
}

void LayoutManager::replaceIndDummyTexture() {
    if (!mLayoutHolder->isExistResOther("IndDummy.tpl")) {
        return;
    }

    JUTTexture screenTex(MR::getScreenResTIMG(), static_cast< u8 >(0));

    mIndDummyTexMap = new nw4r::lyt::TexMap(screenTex.getTexObj());

    void* pIndDummyResStart = mLayoutHolder->getResOther("IndDummy.tpl");
    void* pIndDummyResEnd = reinterpret_cast< void* >(-1);

    for (u32 i = 0; i < mLayoutHolder->getResOtherNum(); i++) {
        void* pRes = mLayoutHolder->getResOther(i);

        if (pIndDummyResStart < pRes && pIndDummyResEnd > pRes) {
            pIndDummyResEnd = pRes;
        }
    }

    for (u32 paneIdx = 0; paneIdx < mPaneCount; paneIdx++) {
        nw4r::lyt::Pane* pCurrPane = mPaneInfoList[paneIdx].mPane;
        nw4r::lyt::Picture* pPicPane = nw4r::ut::DynamicCast< nw4r::lyt::Picture* >(pCurrPane);

        if (pPicPane == nullptr) {
            continue;
        }

        nw4r::lyt::Material* pMaterial = pPicPane->GetMaterial();

        for (u8 texMapIdx = 0; texMapIdx < pMaterial->GetTextureNum(); texMapIdx++) {
            void* pImage = pMaterial->GetTexture(texMapIdx).mImage;

            if (pImage < pIndDummyResStart) {
                continue;
            }

            if (pImage >= pIndDummyResEnd) {
                continue;
            }

            pMaterial->SetTexture(texMapIdx, *mIndDummyTexMap);
        }
    }
}

void LayoutManager::removeUnnecessaryPanes(nw4r::lyt::Pane* pPane) {
    Local::BitFlag< 128 > languagePanes;
    Local::BitFlag< 128 > currentLanguagePanes;
    Local::collectLanguagePanes(pPane, languagePanes, currentLanguagePanes);
    if (languagePanes.isAnythingTrue()) {
        Local::removeLanguagePanes(pPane, languagePanes, currentLanguagePanes);
    } else {
        nw4r::lyt::PaneList& rChildren = pPane->mChildList;
        for (nw4r::lyt::PaneList::Iterator it = rChildren.GetBeginIter(); it != rChildren.GetEndIter(); ++it) {
            removeUnnecessaryPanes(&*it);
        }
    }
}

namespace {
    namespace Local {
        template < int N >
        bool BitFlag< N >::isAnythingTrue() const {
            for (u32 i = 0; i < N / 32; i++) {
                if (mBits[i] != 0) {
                    return true;
                }
            }

            return false;
        }

        template < int N >
        void BitFlag< N >::onBit(int index) {
            int word = index / 32;
            index -= word * 32;
            mBits[word] |= 1 << index;
        }

        template < int N >
        void BitFlag< N >::offBit(int index) {
            int word = index / 32;
            index -= word * 32;
            mBits[word] &= ~(1 << index);
        }

        template < int N >
        bool BitFlag< N >::isTrue(int index) const {
            int word = index / 32;
            index -= word * 32;
            return (mBits[word] & (1 << index)) != 0;
        }
    }  // namespace Local
}  // namespace

#ifdef TARGET_PC
// ---------------------------------------------------------------------------------------------------------------------------------
// layoutwarm24 (GXV_LAYOUTWARM, default 0; GXV_LAYOUTWARM_LOG, default 0): the 2D layouts a stage can show are made known to the renderer behind its
// closed screen, not at their first appearance in play (the pause menu, the star-get and miss sequences, the HUD's counters and meters, message and
// information windows, the wipes: each first draw otherwise links its program variants and uploads its textures in the picture that shows it).
//   Registry: every LayoutManager registers itself at the end of its constructor (PortLayoutRegister) with the scene serial it was made under
//   (port_scene_serial: counts the scenes the game has begun to build). A scene's layouts die with its heap, so an entry of another serial than the
//   current one is dropped (never dereferenced); entries made before the first scene (serial 0: the system wipes) stay.
//   Warm-up (GXV_LAYOUTWARM=1, and the renderer honours the scope: port_layoutwarm_mode): SceneFunction::executeDrawList2DNormal calls
//   port_layoutwarm_frame right after the 2D pass's GX re-initialisation (MR::drawInitFor2DModel: the state every layout's first draw starts from).
//   Only in frames whose screen is closed (port_screen_quiet() == 2, as the effects' warm-up: never in play, never in the pause menu), at most
//   GXV_LAYOUTWARM_FRAME_MS (4) of game time a frame, each layout once a scene. A layout is drawn inside a scope (PORT_GX_MARK_WARM_BEGIN / _END): the
//   layout draw's own preparation (MR::setupDrawForNW4RLayout), then the real DrawSelf of EVERY pane of it - shown or not, whatever its animation frame -
//   in two passes (nw4r::lyt::detail::gPortWarmPass): pass 1 Material::SetupGX(modulate false, alpha 255), pass 2 (true, alpha 128), which are the two ends of
//   what a pane's alpha and vertex colours do to a material's TEV set-up; a text box draws one glyph of its font through its writer's own set-up (pass 1).
//   Nothing is changed that the game can see: the panes are not animated, shown, hidden or moved (DrawSelf reads them), no text is formatted, no
//   message / tag processing runs, the layout's own draw() / the actor's overrides are not called; the GX library's state (GXData) and the draw counter are put
//   back as the effects' warm-up does, the front end puts its own back at the end marker. Pictures stay byte for byte (tests/vgx_layoutwarm*.cpp).
//   Census (GXV_LAYOUTWARM_LOG=1): once a scene, at its first picture with the screen open, one line per eight layouts it loaded (name, panes / materials /
//   textures); and each layout's draw in play is bracketed by PORT_GX_MARK_LAYOUT_BEGIN / _END (kinds 19 / 20), which makes the front end log the layout's first
//   draw with draws: its program variants, those the warm-up list did not know, the shaders and textures it made (vgx_draw.cpp LayoutCensusMarker).
// ---------------------------------------------------------------------------------------------------------------------------------
extern "C" {
void port_gx_marker(unsigned int kind);
int port_screen_quiet(void) __attribute__((weak));
int port_layoutwarm_mode(void) __attribute__((weak));
uint32_t port_scene_serial(void) __attribute__((weak));
uint32_t port_scene_key(void) __attribute__((weak));
uint64_t port_gx_recording_frame(void) __attribute__((weak));
unsigned long long port_par_ptcl_now(void);
void port_log_summary(const char* fmt, ...);
extern u32 __PortGXDrawSerial;
}

namespace {
constexpr int kLwMax = 320;
enum { kLwWarmed = 1, kLwSkipped = 2 };
struct LwEntry {
    LayoutManager* mManager;
    u32 mSerial;
    u32 mFlags;
    char mName[32];
};
LwEntry sLwEntries[kLwMax];
int sLwCount = 0;
u32 sLwSerial = 0;                    // the scene serial the entries were last synced to
unsigned sLwDropped = 0;              // layouts that did not fit in the registry
int sLwWarmMode = -1;                 // port_layoutwarm_mode(), read at the first frame hook (-1: not yet)
int sLwCensus = -1;                   // GXV_LAYOUTWARM_LOG (-1: not yet)
unsigned long long sLwFrameUs = 4000; // GXV_LAYOUTWARM_FRAME_MS
std::atomic< unsigned > sLwPending{0};  // (the back end's closed-screen hold reads it) layouts not drawn in a scope yet, once the warm-up has started
bool sLwStarted = false;
bool sLwDoneLogged = false;
u32 sLwCensusLogged = ~0u;
unsigned long long sLwFrame = ~0ull, sLwFrameSpentUs = 0;
struct LwCtr {
    unsigned warmed, skipped, paneDraws, frames;
    unsigned long long us;
} sLwCtr;

bool LwCensusOn() {
    if (sLwCensus < 0) {
        const char* e = getenv("GXV_LAYOUTWARM_LOG");
        sLwCensus = e != nullptr && atoi(e) != 0 ? 1 : 0;
    }
    return sLwCensus == 1;
}

// A new scene: the entries of the scenes before it are gone with their heaps (drop them, do not look at them); what stays is drawn in a scope again.
void LwSync() {
    const u32 serial = port_scene_serial != nullptr ? port_scene_serial() : 0;
    if (serial == sLwSerial) {
        return;
    }

    int kept = 0;
    for (int i = 0; i < sLwCount; i++) {
        if (sLwEntries[i].mSerial == 0 || sLwEntries[i].mSerial == serial) {
            sLwEntries[i].mFlags = 0;
            sLwEntries[kept++] = sLwEntries[i];
        }
    }
    sLwCount = kept;
    sLwSerial = serial;
    sLwDropped = 0;
    sLwStarted = false;
    sLwDoneLogged = false;
    sLwFrame = ~0ull;
    sLwFrameSpentUs = 0;
    sLwCtr = LwCtr();
    sLwPending.store(0, std::memory_order_relaxed);
}

unsigned LwUnwarmed() {
    unsigned n = 0;
    for (int i = 0; i < sLwCount; i++) {
        if ((sLwEntries[i].mFlags & (kLwWarmed | kLwSkipped)) == 0) {
            n++;
        }
    }
    return n;
}

void LwWalk(nw4r::lyt::Pane* pPane, const nw4r::lyt::DrawInfo& rInfo, unsigned& rDraws) {
    pPane->DrawSelf(rInfo);
    rDraws++;
    for (nw4r::lyt::PaneList::Iterator it = pPane->mChildList.GetBeginIter(); it != pPane->mChildList.GetEndIter(); ++it) {
        LwWalk(&*it, rInfo, rDraws);
    }
}

void LwCount(nw4r::lyt::Pane* pPane, unsigned& rPanes, unsigned& rMaterials, unsigned& rTextures) {
    rPanes++;
    const nw4r::lyt::Material* pMaterial = pPane->GetMaterial();
    if (pMaterial != nullptr) {
        rMaterials++;
        rTextures += pMaterial->GetTextureNum();
    }

    for (nw4r::lyt::PaneList::Iterator it = pPane->mChildList.GetBeginIter(); it != pPane->mChildList.GetEndIter(); ++it) {
        LwCount(&*it, rPanes, rMaterials, rTextures);
    }
}

// One layout, once, in a warm-up scope. Returns the number of pane draws made.
unsigned LwWarmOne(LayoutManager* pManager) {
    static GXData sSavedGx;
    memcpy(&sSavedGx, gx, sizeof sSavedGx);
    const u32 serial0 = __PortGXDrawSerial;
    unsigned draws = 0;
    port_gx_marker(PORT_GX_MARK_WARM_BEGIN);
    MR::setupDrawForNW4RLayout(1.0f, true);  // (LayoutManager::draw's own preparation)
    for (int pass = 1; pass <= 2; pass++) {
        nw4r::lyt::detail::gPortWarmPass = pass;
        LwWalk(pManager->mLayout->mpRootPane, pManager->mDrawInfo, draws);
    }
    nw4r::lyt::detail::gPortWarmPass = 0;
    port_gx_marker(PORT_GX_MARK_WARM_END);
    memcpy(gx, &sSavedGx, sizeof sSavedGx);
    __PortGXDrawSerial = serial0;
    return draws;
}

void LwCensusLoadLog() {
    unsigned totalPanes = 0, totalMaterials = 0, totalTextures = 0;
    char line[420];
    int len = 0;
    line[0] = '\0';
    int inLine = 0;
    const u32 stage = port_scene_key != nullptr ? port_scene_key() : 0;
    // (a first pass for the totals: the header names them)
    for (int i = 0; i < sLwCount; i++) {
        if (sLwEntries[i].mManager->mLayout == nullptr || sLwEntries[i].mManager->mLayout->mpRootPane == nullptr) {
            continue;
        }
        LwCount(sLwEntries[i].mManager->mLayout->mpRootPane, totalPanes, totalMaterials, totalTextures);
    }
    port_log_summary("layoutwarm census: stage %08x: %d layouts loaded (%u not registered: table full), %u panes, %u materials, %u texture maps; "
                     "name (panes/materials/textures), eight a line:",
                     stage, sLwCount, sLwDropped, totalPanes, totalMaterials, totalTextures);
    for (int i = 0; i < sLwCount; i++) {
        LayoutManager* pManager = sLwEntries[i].mManager;
        unsigned panes = 0, materials = 0, textures = 0;
        if (pManager->mLayout != nullptr && pManager->mLayout->mpRootPane != nullptr) {
            LwCount(pManager->mLayout->mpRootPane, panes, materials, textures);
        }
        len += snprintf(line + len, sizeof(line) - len, "#%d %s%s (%u/%u/%u)  ", i + 1, sLwEntries[i].mName, sLwEntries[i].mSerial == 0 ? "*" : "", panes,
                        materials, textures);
        if (++inLine == 8 || i + 1 == sLwCount || len > static_cast< int >(sizeof(line)) - 90) {
            port_log_summary("layoutwarm census: %s", line);
            len = 0;
            inLine = 0;
            line[0] = '\0';
        }
    }
}
}  // namespace

static void PortLayoutRegister(LayoutManager* pManager) {
    LwSync();
    if (sLwCount >= kLwMax) {
        sLwDropped++;
        return;
    }

    LwEntry& rEntry = sLwEntries[sLwCount++];
    rEntry.mManager = pManager;
    rEntry.mSerial = sLwSerial;
    rEntry.mFlags = 0;
    snprintf(rEntry.mName, sizeof(rEntry.mName), "%s", pManager->mLayoutName != nullptr ? pManager->mLayoutName : "?");
    if (sLwStarted) {
        sLwPending.store(LwUnwarmed(), std::memory_order_relaxed);
    }
}

// (the front end's census line: the name of layout #id)
extern "C" const char* port_layoutwarm_name(unsigned id) {
    return id >= 1 && id <= static_cast< unsigned >(sLwCount) ? sLwEntries[id - 1].mName : nullptr;
}

// (the back end's closed-screen hold) layouts the game has not drawn in a scope yet, once the warm-up has begun in this scene.
extern "C" unsigned port_layoutwarm_pending(void) {
    return sLwPending.load(std::memory_order_relaxed);
}

static unsigned PortLayoutCensusBegin(const LayoutManager* pManager) {
    if (!LwCensusOn() || port_screen_quiet == nullptr || port_screen_quiet() == 2) {
        return 0;  // (a closed screen: not a draw in play)
    }

    for (int i = 0; i < sLwCount; i++) {
        if (sLwEntries[i].mManager == pManager) {
            const unsigned id = static_cast< unsigned >(i + 1);
            port_gx_marker(PORT_GX_MARK_LAYOUT_BEGIN | (id << 8));
            return id;
        }
    }

    return 0;
}

static void PortLayoutCensusEnd(unsigned id) {
    port_gx_marker(PORT_GX_MARK_LAYOUT_END | (id << 8));
}

// SceneFunction::executeDrawList2DNormal, right after MR::drawInitFor2DModel (every frame of a game scene's 2D pass).
extern "C" void port_layoutwarm_frame(void) {
    if (sLwWarmMode < 0) {
        sLwWarmMode = port_layoutwarm_mode != nullptr && port_layoutwarm_mode() != 0 ? 1 : 0;
        const char* e = getenv("GXV_LAYOUTWARM_FRAME_MS");
        const int ms = e != nullptr ? atoi(e) : 4;
        sLwFrameUs = static_cast< unsigned long long >(ms < 1 ? 1 : ms > 200 ? 200 : ms) * 1000ull;
    }

    if (sLwWarmMode == 0 && !LwCensusOn()) {
        return;
    }

    LwSync();
    if (port_screen_quiet == nullptr) {
        return;
    }

    const int quiet = port_screen_quiet();
    if (quiet == 0 && LwCensusOn() && sLwCensusLogged != sLwSerial && sLwCount > 0) {
        sLwCensusLogged = sLwSerial;
        LwCensusLoadLog();
    }

    if (sLwWarmMode == 0 || quiet != 2) {
        return;
    }

    if (!sLwStarted) {
        sLwStarted = true;
        sLwPending.store(LwUnwarmed(), std::memory_order_relaxed);
    }

    const unsigned long long t0 = port_par_ptcl_now();
    const unsigned long long frame = port_gx_recording_frame != nullptr ? port_gx_recording_frame() : 0;
    if (frame != sLwFrame) {
        sLwFrame = frame;
        sLwFrameSpentUs = 0;
    }

    if (sLwFrameSpentUs >= sLwFrameUs) {
        return;
    }

    bool any = false;
    for (int i = 0; i < sLwCount; i++) {
        LwEntry& rEntry = sLwEntries[i];
        if ((rEntry.mFlags & (kLwWarmed | kLwSkipped)) != 0) {
            continue;
        }

        LayoutManager* pManager = rEntry.mManager;
        if (pManager->mLayout == nullptr || pManager->mLayout->mpRootPane == nullptr) {
            rEntry.mFlags |= kLwSkipped;
            sLwCtr.skipped++;
            continue;
        }

        sLwCtr.paneDraws += LwWarmOne(pManager);
        rEntry.mFlags |= kLwWarmed;
        sLwCtr.warmed++;
        any = true;
        if (sLwFrameSpentUs + (port_par_ptcl_now() - t0) >= sLwFrameUs) {
            break;
        }
    }

    if (any) {
        const unsigned long long us = port_par_ptcl_now() - t0;
        if (sLwFrameSpentUs == 0) {
            sLwCtr.frames++;
        }

        sLwFrameSpentUs += us;
        sLwCtr.us += us;
    }

    const unsigned left = LwUnwarmed();
    sLwPending.store(left, std::memory_order_relaxed);
    if (left == 0 && !sLwDoneLogged) {
        sLwDoneLogged = true;
        port_log_summary("layoutwarm: stage %08x: %d layouts (%u not registered), %u drawn in a scope (%u pane draws in 2 passes), %u skipped, %.1f ms of the "
                         "game thread in %u frames; the variants and textures they made are in the back end's 'prewarm:' line (GXV_PREWARM_LOG=1) and the "
                         "layouts' first draws in play in 'layoutwarm: first draw:' lines (GXV_LAYOUTWARM_LOG=1)",
                         port_scene_key != nullptr ? port_scene_key() : 0u, sLwCount, sLwDropped, sLwCtr.warmed, sLwCtr.paneDraws, sLwCtr.skipped,
                         static_cast< double >(sLwCtr.us) / 1000.0, sLwCtr.frames);
    }
}
#endif
