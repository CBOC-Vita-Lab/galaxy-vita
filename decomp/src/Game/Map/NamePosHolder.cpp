#include "Game/Map/NamePosHolder.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/JMapLinkInfo.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/StringUtil.hpp"

NamePosHolder::NamePosHolder() : NameObj("\x88\xca\x92\x75\x83\x65\x81\x5b\x83\x75\x83\x8b\x95\xdb\x8e\x9d"), mPosNum(), mInfos() {
    mPosNum = MR::getGeneralPosNum();
    mInfos = new NamePosInfo[mPosNum];

    for (s32 i = 0; i < mPosNum; i++) {
        NamePosInfo* curInf = &mInfos[i];
        curInf->mLinkInfo = nullptr;
        curInf->_20 = nullptr;
        MR::getGeneralPosData(&curInf->mName, &curInf->mPosition, &curInf->mRotation, &curInf->mLinkInfo, i);
    }
}

NamePosInfo::NamePosInfo() {
}

bool NamePosHolder::tryRegisterLinkObj(const NameObj* pObj, const JMapInfoIter& rIter) {
    JMapLinkInfo info = JMapLinkInfo(rIter, true);

    for (s32 i = 0; i < mPosNum; i++) {
        NamePosInfo* currInfo = &mInfos[i];
        if (*currInfo->mLinkInfo == info) {
            currInfo->_20 = pObj;
            return true;
        }
    }

    return false;
}

bool NamePosHolder::find(const NameObj* pObj, const char* pName, TVec3f* pPos, TVec3f* pRot) const {
    for (s32 idx = 0; idx < mPosNum; idx++) {
        NamePosInfo* pInfo = &mInfos[idx];

        if (!MR::isEqualString(pName, mInfos[idx].mName)) {
            continue;
        }

        if (pObj != nullptr && pInfo->mLinkInfo->isValid() && (pObj != pInfo->_20)) {
            continue;
        }

        pPos->set(pInfo->mPosition);

        if (pRot != nullptr) {
            pRot->set(pInfo->mRotation);
        }

        return true;
    }

    return false;
}

namespace MR {
    NamePosHolder* getNamePosHolder() {
        return MR::getSceneObj< NamePosHolder >(SceneObj_NamePosHolder);
    }
};  // namespace MR
