#include "Game/MapObj/AirBubbleHolder.hpp"
#include "Game/MapObj/AirBubble.hpp"
#include "Game/Scene/SceneObjHolder.hpp"

namespace {
    static const s32 sAirBubbleMax = 64;
    static const s32 sCreateAirBubbleNum = 32;
};  // namespace

AirBubbleHolder::AirBubbleHolder(const char* pName) : DeriveActorGroup< AirBubble >(pName, ::sAirBubbleMax) {
}

void AirBubbleHolder::init(const JMapInfoIter& rIter) {
    AirBubble* pAirBubble;

    for (s32 i = 0; i < ::sCreateAirBubbleNum; i++) {
        pAirBubble = new AirBubble("\x8b\xf3\x8b\x43\x83\x41\x83\x8f(\x8b\xa4\x97\x70)");
        pAirBubble->initWithoutIter();
        pAirBubble->makeActorDead();

        registerActor(pAirBubble);
    }
}

void AirBubbleHolder::appearAirBubble(const TVec3f& rTrans, s32 lifeTime) {
    AirBubble* pAirBubble = getDeadMember();

    if (pAirBubble != nullptr) {
        pAirBubble->appearMove(rTrans, lifeTime);
    }
}

namespace MR {
    AirBubbleHolder* createAirBubbleHolder() {
        return static_cast< AirBubbleHolder* >(MR::createSceneObj(SceneObj_AirBubbleHolder));
    }

    void appearAirBubble(const TVec3f& rTrans, s32 lifeTime) {
        MR::getSceneObj< AirBubbleHolder >(SceneObj_AirBubbleHolder)->appearAirBubble(rTrans, lifeTime);
    }
};  // namespace MR
