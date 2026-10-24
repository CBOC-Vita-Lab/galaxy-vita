#include "Game/Enemy/TakoHeiInkHolder.hpp"
#include "Game/Enemy/TakoHeiInk.hpp"
#include "Game/Scene/SceneObjHolder.hpp"

namespace {
    static const s32 sMaxTakoHeiInk = 16;
};  // namespace

TakoHeiInkHolder::TakoHeiInkHolder() : DeriveActorGroup< TakoHeiInk >("\x83\x5e\x83\x52\x83\x77\x83\x43\x96\x6e\x8a\xc7\x97\x9d", ::sMaxTakoHeiInk) {
    TakoHeiInk* pInk;

    for (int i = 0; i < ::sMaxTakoHeiInk; i++) {
        pInk = new TakoHeiInk("\x83\x5e\x83\x52\x83\x77\x83\x43\x96\x6e");
        pInk->initWithoutIter();
        registerActor(pInk);
    }
}

namespace MR {
    void createTakoHeiInkHolder() {
        createSceneObj(SceneObj_TakoHeiInkHolder);
    }

    bool spurtTakoHeiInk(const TVec3f& rTrans, const TVec3f& rVelocity) {
        TakoHeiInk* pInk = MR::getSceneObj< TakoHeiInkHolder >(SceneObj_TakoHeiInkHolder)->getDeadMember();

        if (pInk == nullptr) {
            return false;
        }

        pInk->start(rTrans, rVelocity, false);

        return true;
    }
};  // namespace MR
