#include "Game/Boss/KoopaRockBreak.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/LiveActorUtil.hpp"

KoopaRockBreak::KoopaRockBreak(LiveActor* pActor) : PartsModel(pActor, "\x8a\xe2\x89\xf3\x82\xea\x83\x82\x83\x66\x83\x8b", "KoopaRockBreak", nullptr, MR::DrawBufferType_Enemy, false) {
}

void KoopaRockBreak::init(const JMapInfoIter& rIter) {
    PartsModel::init(rIter);

    MR::invalidateClipping(this);

    MR::initLightCtrl(this);

    loadFixedPosition("RockBreakFixPos");

    makeActorDead();
}

void KoopaRockBreak::control() {
    if (MR::isActionEnd(this)) {
        kill();
    }
}

KoopaRockBreak::~KoopaRockBreak() {
}
