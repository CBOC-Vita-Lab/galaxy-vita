#include "Game/Effect/SyncBckEffectInfo.hpp"
#include "Game/Animation/XanimePlayer.hpp"
#include "Game/Animation/XanimeResource.hpp"
#include "Game/Util/StringUtil.hpp"
#ifdef TARGET_PC
#include "port/hot_knob.h"
#endif

SyncBckEffectInfo::BckResourceInfo::BckResourceInfo(const XanimePlayer* pPlayer, const char* pName) : mName(pName), mResource() {
    mResource = static_cast< J3DAnmTransform* >(pPlayer->mResourceTable->findResMotion(pName));
}

bool SyncBckEffectInfo::BckResourceInfo::isLoop() const {
    return mResource->getAttribute() == J3DFrameCtrl::EMode_LOOP || mResource->getAttribute() == J3DFrameCtrl::EMode_LOOP_REVERSE;
}

SyncBckEffectInfo::SyncBckEffectInfo(const XanimePlayer* pPlayer, const char* pName, s32 capacity, f32 startFrame, f32 endFrame, bool continueAnimEnd)
    : mBckResources(), mCapacity(), mCount(), mStartFrame(startFrame), mEndFrame(endFrame), mContinueAnimEnd(continueAnimEnd) {
    mBckResources = new BckResourceInfo*[capacity];
    mCapacity = capacity;
    addBck(pPlayer, pName);
}

void SyncBckEffectInfo::addBck(const XanimePlayer* pPlayer, const char* pName) {
    BckResourceInfo* pInfo = new BckResourceInfo(pPlayer, pName);
    const s32 index = mCount++;
    mBckResources[index] = pInfo;
#ifdef TARGET_PC
    mPortLookupName = nullptr;
#endif
}

#ifdef TARGET_PC
// forkB (6 Nov; ported from Producdevity's fork, 497c013): SyncBckEffectChecker asks each emitter 4-5 times a turn whether the
// actor's current animation is one of its own, always with the same name pointer (an entry of the actor's animation table, which
// lives as long as the actor and its emitters). The last pointer looked up and its index are kept: exact.
// PETARI_BCK_MEMO=0: every lookup loops over the names as before.
static bool portBckMemoOn() {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("PETARI_BCK_MEMO"); return e && *e == '0' ? 0 : 1; }()) != 0;
}

s32 SyncBckEffectInfo::portFind(const char* pName) const {
    if (pName != mPortLookupName || !portBckMemoOn()) {
        mPortLookupIndex = -1;
        for (s32 i = 0; i < mCount; i++) {
            if (MR::isEqualStringCase(pName, mBckResources[i]->mName)) {
                mPortLookupIndex = i;
                break;
            }
        }
        mPortLookupName = pName;
    }
    return mPortLookupIndex;
}
#endif

bool SyncBckEffectInfo::isRegisteredBck(const char* pName) const {
    if (pName == nullptr) {
        return false;
    }

#ifdef TARGET_PC
    return portFind(pName) >= 0;
#else
    for (BckResourceInfo** pInfo = mBckResources; pInfo != mBckResources + mCount; pInfo++) {
        if (MR::isEqualStringCase(pName, (*pInfo)->mName)) {
            return true;
        }
    }

    return false;
#endif
}

bool SyncBckEffectInfo::isBckLoop(const char* pName) const {
    if (pName == nullptr) {
        return false;
    }

#ifdef TARGET_PC
    const s32 index = portFind(pName);
    return index >= 0 && mBckResources[index]->isLoop();
#else
    BckResourceInfo* pResource = nullptr;

    for (BckResourceInfo** pInfo = mBckResources; pInfo != mBckResources + mCount; pInfo++) {
        if (MR::isEqualStringCase((*pInfo)->mName, pName)) {
            pResource = *pInfo;
            break;
        }
    }

    if (pResource == nullptr) {
        return false;
    }

    return pResource->isLoop();
#endif
}

namespace MR {
    namespace Effect {
        bool isExistSyncBckDeleteFrame(const SyncBckEffectInfo* pInfo) {
            const f32 endFrame = pInfo->mEndFrame;
            return 0.0f <= endFrame;
        }
    }  // namespace Effect
}  // namespace MR
