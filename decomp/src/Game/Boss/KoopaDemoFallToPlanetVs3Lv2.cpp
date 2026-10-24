#include "Game/Boss/KoopaDemoFallToPlanetVs3Lv2.hpp"
#include "Game/Boss/Koopa.hpp"
#include "Game/Boss/KoopaBattleBase.hpp"
#include "Game/Boss/KoopaFunction.hpp"
#include "Game/Boss/KoopaPlanetShadow.hpp"
#include "Game/Boss/KoopaSwitchKeeper.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Map/KoopaBattleMapPlanet.hpp"
#include "Game/Util/ActorMovementUtil.hpp"
#include "Game/Util/ActorSwitchUtil.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/NerveUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SoundUtil.hpp"

namespace {
    static MR::ActorMoveParam sFallParam = {0.0f, 1.0f, 0.98f, 0.0f};
    static MR::ActorMoveParam sWaitParam = {0.0f, 1.0f, 0.9f, 1.0f};
};  // namespace

namespace NrvKoopaDemoFallToPlanetVs3Lv2 {
    NEW_NERVE(KoopaDemoFallToPlanetVs3Lv2NrvWaitFall, KoopaDemoFallToPlanetVs3Lv2, WaitFall);
    NEW_NERVE(KoopaDemoFallToPlanetVs3Lv2NrvDemoFall, KoopaDemoFallToPlanetVs3Lv2, DemoFall);
    NEW_NERVE(KoopaDemoFallToPlanetVs3Lv2NrvFallToLand, KoopaDemoFallToPlanetVs3Lv2, FallToLand);
    NEW_NERVE(KoopaDemoFallToPlanetVs3Lv2NrvLand, KoopaDemoFallToPlanetVs3Lv2, Land);
    NEW_NERVE(KoopaDemoFallToPlanetVs3Lv2NrvWaitPlayer, KoopaDemoFallToPlanetVs3Lv2, WaitPlayer);
};  // namespace NrvKoopaDemoFallToPlanetVs3Lv2

KoopaDemoFallToPlanetVs3Lv2::KoopaDemoFallToPlanetVs3Lv2(Koopa* pKoopa) : ActorStateBase< Koopa >("Demo[\x82\x6b\x82\x96\x82\x51\x98\x66\x90\xaf\x82\xdc\x82\xc5\x97\x8e\x89\xba]", pKoopa) {
}

void KoopaDemoFallToPlanetVs3Lv2::init() {
    KoopaFunction::initKoopaCamera(mHost, "\x98\x66\x90\xaf\x82\x6b\x82\x96\x82\x51\x82\xdc\x82\xc5\x97\x8e\x89\xba\x81\x69\x8c\xe3\x94\xbc\x81\x6a");
    KoopaFunction::initKoopaAnimCamera(mHost, "DemoKoopaFall");
    initNerve(GET_NERVE(KoopaDemoFallToPlanetVs3Lv2, KoopaDemoFallToPlanetVs3Lv2NrvWaitFall));
}

void KoopaDemoFallToPlanetVs3Lv2::appear() {
    mIsDead = false;

    KoopaFunction::getKoopaPlanetShadow(mHost)->kill();

    KoopaFunction::endFaceCtrl(mHost, -1);
}

void KoopaDemoFallToPlanetVs3Lv2::kill() {
    mIsDead = true;

    KoopaFunction::endKoopaCamera(mHost, "\x98\x66\x90\xaf\x82\x6b\x82\x96\x82\x51\x82\xdc\x82\xc5\x97\x8e\x89\xba\x81\x69\x8c\xe3\x94\xbc\x81\x6a", false, -1);
}

void KoopaDemoFallToPlanetVs3Lv2::exeWaitFall() {
    if (KoopaFunction::tryStartKoopaAndMarioCameraDemo(mHost, "\x83\x4e\x83\x62\x83\x70\x82\x75\x82\x93\x82\x52\x98\x66\x90\xaf\x88\xda\x93\xae\x81\x69\x82\x6b\x82\x96\x82\x50\x82\xa9\x82\xe7\x82\x6b\x82\x96\x82\x51\x81\x6a", "DemoKoopaFall", "\x83\x66\x83\x82\x92\x86\x90\x53")) {
        setNerve(GET_NERVE(KoopaDemoFallToPlanetVs3Lv2, KoopaDemoFallToPlanetVs3Lv2NrvDemoFall));
    }
}

void KoopaDemoFallToPlanetVs3Lv2::exeDemoFall() {
    if (MR::isFirstStep(this)) {
        MR::startBrk(KoopaFunction::getKoopaPlanet(mHost), "Death");
    }

    if (KoopaFunction::tryEndKoopaCameraDemo(mHost, "\x83\x4e\x83\x62\x83\x70\x82\x75\x82\x93\x82\x52\x98\x66\x90\xaf\x88\xda\x93\xae\x81\x69\x82\x6b\x82\x96\x82\x50\x82\xa9\x82\xe7\x82\x6b\x82\x96\x82\x51\x81\x6a", "DemoKoopaFall")) {
        MR::offSwitchB(mHost);
        MR::onSwitchA(KoopaFunction::getKoopaSwitchKeeper(mHost));
        MR::overlayWithPreviousScreen(2);

        setNerve(GET_NERVE(KoopaDemoFallToPlanetVs3Lv2, KoopaDemoFallToPlanetVs3Lv2NrvFallToLand));
    }
}

void KoopaDemoFallToPlanetVs3Lv2::exeFallToLand() {
    if (MR::isFirstStep(this)) {
        KoopaFunction::setKoopaPos(mHost, "\x82\x6b\x82\x96\x82\x51\x8a\x4a\x8e\x6e\x81\x69\x83\x4e\x83\x62\x83\x70\x81\x6a");
        MR::setPlayerPosAndWait("\x82\x6b\x82\x96\x82\x51\x8a\x4a\x8e\x6e\x81\x69\x83\x7d\x83\x8a\x83\x49\x81\x6a");

        KoopaFunction::startKoopaCamera(mHost, "\x98\x66\x90\xaf\x82\x6b\x82\x96\x82\x51\x82\xdc\x82\xc5\x97\x8e\x89\xba\x81\x69\x8c\xe3\x94\xbc\x81\x6a");
        MR::startAction(mHost, "JumpSoon");

        KoopaFunction::getKoopaPlanetShadow(mHost)->appear();
    }

    MR::moveAndTurnToPlayer(mHost, &mHost->mFront, ::sFallParam);

    MR::startLevelSound(mHost, "SE_BM_LV_KOOPA_FALL_TO_PLANET");

    if (MR::isBindedGround(mHost)) {
        MR::zeroVelocity(mHost);
        setNerve(GET_NERVE(KoopaDemoFallToPlanetVs3Lv2, KoopaDemoFallToPlanetVs3Lv2NrvLand));
    }
}

void KoopaDemoFallToPlanetVs3Lv2::exeLand() {
    if (MR::isFirstStep(this)) {
        MR::startAction(mHost, "Land");
        MR::startSound(mHost, "SE_BM_KOOPA_FLIP_RECOVER_END");
        MR::startSound(mHost, "SE_BV_KOOPA_LAND_HEAVY");

        MR::tryRumblePadStrong(mHost, WPAD_CHAN0);
        MR::shakeCameraNormalStrong();
    }

    if (MR::isActionEnd(mHost)) {
        setNerve(GET_NERVE(KoopaDemoFallToPlanetVs3Lv2, KoopaDemoFallToPlanetVs3Lv2NrvWaitPlayer));
    }
}

void KoopaDemoFallToPlanetVs3Lv2::exeWaitPlayer() {
    if (MR::isFirstStep(this)) {
        KoopaFunction::startFaceCtrl(mHost);
    }

    MR::moveAndTurnToPlayer(mHost, &mHost->mFront, ::sWaitParam);

    if (MR::isOnGroundPlayer()) {
        KoopaFunction::startFaceCtrl(mHost);
        kill();
    }
}

KoopaDemoFallToPlanetVs3Lv2::~KoopaDemoFallToPlanetVs3Lv2() {
}
