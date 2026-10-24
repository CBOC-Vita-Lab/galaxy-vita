#include "Game/MapObj/ClipAreaDropHolder.hpp"
#include "Game/MapObj/ClipAreaDrop.hpp"
#include "Game/Scene/SceneObjHolder.hpp"

namespace {
    static const s32 sMaxClipAreaDrop = 32;
};  // namespace

ClipAreaDropHolder::ClipAreaDropHolder() : DeriveActorGroup< ClipAreaDrop >("\x83\x4e\x83\x8a\x83\x62\x83\x76\x83\x47\x83\x8a\x83\x41\x82\xcc\x82\xb5\x82\xb8\x82\xad\x8a\xc7\x97\x9d", ::sMaxClipAreaDrop) {
    ClipAreaDrop* pClipAreaDrop = nullptr;

    for (int i = 0; i < ::sMaxClipAreaDrop; i++) {
        pClipAreaDrop = new ClipAreaDrop("\x83\x4e\x83\x8a\x83\x62\x83\x76\x83\x47\x83\x8a\x83\x41\x82\xcc\x82\xb5\x82\xb8\x82\xad");
        pClipAreaDrop->initWithoutIter();
        registerActor(pClipAreaDrop);
    }
}

namespace MR {
    NameObj* createClipAreaDropHolder() {
        return MR::createSceneObj(SceneObj_ClipAreaDropHolder);
    }

    ClipAreaDrop* getDeadClipAreaDrop() {
        return MR::getSceneObj< ClipAreaDropHolder >(SceneObj_ClipAreaDropHolder)->getDeadMember();
    }

    bool appearClipAreaDrop(const TVec3f& rPos, f32 baseSize) {
        ClipAreaDrop* pClipAreaDrop = getDeadClipAreaDrop();

        if (pClipAreaDrop == nullptr) {
            return false;
        }

        pClipAreaDrop->mPosition.set(rPos);
        pClipAreaDrop->setBaseSize(baseSize);
        pClipAreaDrop->appear();

        return true;
    }
};  // namespace MR
