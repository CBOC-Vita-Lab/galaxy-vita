#include "Game/MapObj/ArrowSwitchMultiHolder.hpp"
#include "Game/MapObj/ArrowSwitchMulti.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/JMapIdInfo.hpp"

namespace {
    static const s32 sMaxArrowSwitchMulti = 16;
};  // namespace

namespace {
    ArrowSwitchMultiHolder* getArrowSwitchMultiHolder() {
        return MR::getSceneObj< ArrowSwitchMultiHolder >(SceneObj_ArrowSwitchMultiHolder);
    }
};  // namespace

ArrowSwitchMultiHolder::ArrowSwitchMultiHolder() : DeriveActorGroup("\x95\xa1\x90\x94\x95\xfb\x8c\xfc\x96\xee\x88\xf3\x83\x58\x83\x43\x83\x62\x83\x60\x8a\xc7\x97\x9d", ::sMaxArrowSwitchMulti) {
}

ArrowSwitchMulti* ArrowSwitchMultiHolder::findSwitch(const JMapIdInfo* pIdInfo) {
    for (s32 i = 0; i < getObjNum(); i++) {
        ArrowSwitchMulti* pSwitch = getMember(i);

        if (*pSwitch->mIdInfo == *pIdInfo) {
            return pSwitch;
        }
    }

    return nullptr;
}

namespace MR {
    void createArrowSwitchMultiHolder() {
        MR::createSceneObj(SceneObj_ArrowSwitchMultiHolder);
    }

    void registerArrowSwitchMulti(ArrowSwitchMulti* pSwitch) {
        ::getArrowSwitchMultiHolder()->registerActor(pSwitch);
    }

    void registerArrowSwitchTarget(ArrowSwitchTarget* pTarget) {
        ::getArrowSwitchMultiHolder()->findSwitch(pTarget->mIdInfo)->registerTarget(pTarget);
    }
};  // namespace MR
