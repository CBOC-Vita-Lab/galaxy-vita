#include "Game/Player/MarioHolder.hpp"
#include "Game/Scene/SceneObjHolder.hpp"

MarioHolder::MarioHolder() : NameObj("\x83\x7d\x83\x8a\x83\x49\x95\xdb\x8e\x9d") {
    mActor = nullptr;
}

MarioHolder::~MarioHolder() {
}

void MarioHolder::setMarioActor(MarioActor* pActor) {
    mActor = pActor;
}

MarioActor* MarioHolder::getMarioActor() const {
    return mActor;
}

namespace MR {
    MarioHolder* getMarioHolder() {
        return MR::getSceneObj< MarioHolder >(SceneObj_MarioHolder);
    }
};  // namespace MR
