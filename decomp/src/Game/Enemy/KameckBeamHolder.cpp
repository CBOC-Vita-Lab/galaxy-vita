#include "Game/Enemy/KameckBeamHolder.hpp"
#include "Game/Enemy/KameckBeam.hpp"
#include "Game/Enemy/KameckFireBall.hpp"
#include "Game/Enemy/KameckTurtle.hpp"
#include "Game/Scene/SceneObjHolder.hpp"

namespace {
    static const s32 sMaxKameckBeam = 16;
    static const s32 sMaxKameckFireBall = 16;
    static const s32 sMaxKameckBeamTurtle = 16;
};  // namespace

KameckBeamHolder::KameckBeamHolder() : DeriveActorGroup< KameckBeam >("\x83\x4a\x83\x81\x83\x62\x83\x4e\x83\x72\x81\x5b\x83\x80\x8a\xc7\x97\x9d", ::sMaxKameckBeam) {
    KameckBeam* pBeam;

    for (s32 i = 0; i < ::sMaxKameckBeam; i++) {
        pBeam = new KameckBeam("\x83\x4a\x83\x81\x83\x62\x83\x4e\x83\x72\x81\x5b\x83\x80");
        pBeam->initWithoutIter();
        registerActor(pBeam);
    }
}

KameckFireBallHolder::KameckFireBallHolder() : DeriveActorGroup< KameckFireBall >("\x83\x4a\x83\x81\x83\x62\x83\x4e\x89\xce\x82\xcc\x8b\xca\x8a\xc7\x97\x9d", ::sMaxKameckFireBall) {
    KameckFireBall* pFireBall;

    for (s32 i = 0; i < ::sMaxKameckFireBall; i++) {
        pFireBall = new KameckFireBall("\x83\x4a\x83\x81\x83\x62\x83\x4e\x83\x72\x81\x5b\x83\x80\x97\x70\x89\x8a");
        pFireBall->initWithoutIter();
        pFireBall->makeActorDead();
        registerActor(pFireBall);
    }
}

KameckBeamTurtleHolder::KameckBeamTurtleHolder() : DeriveActorGroup< KameckTurtle >("\x83\x4a\x83\x81\x83\x62\x83\x4e\x83\x72\x81\x5b\x83\x80\x97\x70\x83\x4a\x83\x81\x8a\xc7\x97\x9d", ::sMaxKameckBeamTurtle) {
    KameckTurtle* pTurtle;

    for (s32 i = 0; i < ::sMaxKameckBeamTurtle; i++) {
        pTurtle = new KameckTurtle("\x83\x4a\x83\x81\x83\x62\x83\x4e\x83\x72\x81\x5b\x83\x80\x97\x70\x83\x4a\x83\x81");
        pTurtle->initWithoutIter();
        pTurtle->makeActorDead();
        registerActor(pTurtle);
    }
}

namespace MR {
    void createKameckBeamHolder() {
        MR::createSceneObj(SceneObj_KameckBeamHolder);
    }

    void createKameckFireBallHolder() {
        MR::createSceneObj(SceneObj_KameckFireBallHolder);
    }

    void createKameckBeamTurtleHolder() {
        MR::createSceneObj(SceneObj_KameckBeamTurtleHolder);
    }

    KameckBeam* startFollowKameckBeam(s32 beamKind, MtxPtr pMtx, f32 scale, const TVec3f& rWandLocalPos, KameckBeamEventListener* pEventListener) {
        KameckBeam* pBeam = getKameckBeam();

        if (pBeam == nullptr) {
            return nullptr;
        }

        pBeam->setBeamKind(beamKind);

        if (!pBeam->requestFollowWand(pMtx, scale)) {
            return nullptr;
        }

        pBeam->setWandLocalPosition(rWandLocalPos);
        pBeam->setEventListener(pEventListener);

        return pBeam;
    }

    KameckBeam* getKameckBeam() {
        return MR::getSceneObj< KameckBeamHolder >(SceneObj_KameckBeamHolder)->getDeadMember();
    }

    KameckFireBall* getKameckFireBall() {
        return MR::getSceneObj< KameckFireBallHolder >(SceneObj_KameckFireBallHolder)->getDeadMember();
    }

    KameckTurtle* getKameckBeamTurtle() {
        return MR::getSceneObj< KameckBeamTurtleHolder >(SceneObj_KameckBeamTurtleHolder)->getDeadMember();
    }
};  // namespace MR
