#include "Game/Boss/KoopaDemoFallToPlanetVs3Lv3.hpp"
#include "Game/Boss/Koopa.hpp"
#include "Game/Boss/KoopaBattleBase.hpp"
#include "Game/Boss/KoopaFunction.hpp"
#include "Game/Boss/KoopaPlanetShadow.hpp"
#include "Game/Boss/KoopaSwitchKeeper.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Map/KoopaBattleMapPlanet.hpp"
#include "Game/Util/ActorMovementUtil.hpp"
#include "Game/Util/ActorSwitchUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/NerveUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SoundUtil.hpp"

namespace {
    static MR::ActorMoveParam sFallParam = {0.0f, 1.0f, 0.98f, 0.0f};
    static MR::ActorMoveParam sWaitParam = {0.0f, 1.0f, 0.9f, 1.0f};
    static const s32 sFallOutsideChangeSun = 266;
    static const s32 sFinalBgmStartStepSub = 250;
    static const s32 sFallDownSeStep2 = 214;
    static const s32 sFallDownSeStep3 = 328;
};  // namespace

namespace NrvKoopaDemoFallToPlanetVs3Lv3 {
    NEW_NERVE(KoopaDemoFallToPlanetVs3Lv3NrvWaitFallDemo, KoopaDemoFallToPlanetVs3Lv3, WaitFallDemo);
    NEW_NERVE(KoopaDemoFallToPlanetVs3Lv3NrvFallOutside, KoopaDemoFallToPlanetVs3Lv3, FallOutside);
    NEW_NERVE(KoopaDemoFallToPlanetVs3Lv3NrvFallInside, KoopaDemoFallToPlanetVs3Lv3, FallInside);
    NEW_NERVE(KoopaDemoFallToPlanetVs3Lv3NrvLand, KoopaDemoFallToPlanetVs3Lv3, Land);
    NEW_NERVE(KoopaDemoFallToPlanetVs3Lv3NrvWaitPlayer, KoopaDemoFallToPlanetVs3Lv3, WaitPlayer);
};  // namespace NrvKoopaDemoFallToPlanetVs3Lv3

KoopaDemoFallToPlanetVs3Lv3::KoopaDemoFallToPlanetVs3Lv3(Koopa* pKoopa) : ActorStateBase< Koopa >("Demo[\x82\x6b\x82\x96\x82\x52\x98\x66\x90\xaf\x82\xdc\x82\xc5\x97\x8e\x89\xba]", pKoopa) {
}

void KoopaDemoFallToPlanetVs3Lv3::init() {
    KoopaFunction::initKoopaCamera(mHost, "\x98\x66\x90\xaf\x82\x6b\x82\x96\x82\x52\x93\xe0\x91\xa4\x82\xf0\x97\x8e\x89\xba");
    KoopaFunction::initKoopaAnimCamera(mHost, "DemoKoopaFallSun");
    initNerve(GET_NERVE(KoopaDemoFallToPlanetVs3Lv3, KoopaDemoFallToPlanetVs3Lv3NrvWaitFallDemo));
}

void KoopaDemoFallToPlanetVs3Lv3::appear() {
    mIsDead = false;

    KoopaFunction::getKoopaPlanetShadow(mHost)->kill();
    KoopaFunction::endFaceCtrl(mHost, -1);
}

void KoopaDemoFallToPlanetVs3Lv3::kill() {
    mIsDead = true;

    KoopaFunction::endKoopaCamera(mHost, "\x98\x66\x90\xaf\x82\x6b\x82\x96\x82\x52\x93\xe0\x91\xa4\x82\xf0\x97\x8e\x89\xba", false, -1);
}

void KoopaDemoFallToPlanetVs3Lv3::exeWaitFallDemo() {
    if (KoopaFunction::tryStartKoopaAndMarioCameraDemo(mHost, "\x83\x4e\x83\x62\x83\x70\x82\x75\x82\x93\x82\x52\x98\x66\x90\xaf\x88\xda\x93\xae\x81\x69\x82\x6b\x82\x96\x82\x51\x82\xa9\x82\xe7\x82\x6b\x82\x96\x82\x52\x81\x6a", "DemoKoopaFallSun", "\x83\x66\x83\x82\x92\x86\x90\x53")) {
        setNerve(GET_NERVE(KoopaDemoFallToPlanetVs3Lv3, KoopaDemoFallToPlanetVs3Lv3NrvFallOutside));
    }
}

void KoopaDemoFallToPlanetVs3Lv3::exeFallOutside() {
    if (MR::isFirstStep(this)) {
        MR::startBrk(KoopaFunction::getKoopaPlanetLv2(mHost), "Death");
        MR::showModel(KoopaFunction::getKoopaPlanetLv3(mHost));
    }

    if (MR::isStep(this, ::sFinalBgmStartStepSub)) {
        MR::startStageBGM("MBGM_BOSS_KOOPA_FINAL", false);
    }

    if (MR::isStep(this, ::sFallOutsideChangeSun)) {
        KoopaFunction::appearKoopaHoleSunPlanetInside(mHost);
        MR::onSwitchDead(KoopaFunction::getKoopaSwitchKeeper(mHost));
    }

    if (MR::isStep(this, ::sFallOutsideChangeSun + 1)) {
        KoopaFunction::getKoopaPlanet(mHost)->kill();
        KoopaFunction::getKoopaPlanetLv2(mHost)->kill();

        KoopaFunction::killKoopaHoleSunPlanetOutside(mHost);

        MR::offSwitchA(KoopaFunction::getKoopaSwitchKeeper(mHost));
        MR::onSwitchB(KoopaFunction::getKoopaSwitchKeeper(mHost));
        MR::offSwitchDead(KoopaFunction::getKoopaSwitchKeeper(mHost));
    }

    if (MR::isLessStep(this, ::sFallDownSeStep2)) {
        MR::startAtmosphereLevelSE("SE_BM_LV_KOOPA_FALL_SUN_1");
    } else if (MR::isLessStep(this, ::sFallDownSeStep3)) {
        MR::startAtmosphereLevelSE("SE_BM_LV_KOOPA_FALL_SUN_2");
    } else {
        MR::startAtmosphereLevelSE("SE_BM_LV_KOOPA_FALL_SUN_3");
    }

    if (MR::isStep(this, ::sFallDownSeStep3)) {
        MR::startAtmosphereSE("SE_BM_KOOPA_FALL_SUN_3_WIND");
    }

    if (KoopaFunction::tryEndKoopaCameraDemo(mHost, "\x83\x4e\x83\x62\x83\x70\x82\x75\x82\x93\x82\x52\x98\x66\x90\xaf\x88\xda\x93\xae\x81\x69\x82\x6b\x82\x96\x82\x51\x82\xa9\x82\xe7\x82\x6b\x82\x96\x82\x52\x81\x6a", "DemoKoopaFallSun")) {
        KoopaFunction::setKoopaPos(mHost, "\x82\x6b\x82\x96\x82\x52\x93\xe0\x91\xa4\x81\x69\x83\x4e\x83\x62\x83\x70\x81\x6a");
        MR::setPlayerPosAndWait("\x82\x6b\x82\x96\x82\x52\x93\xe0\x91\xa4\x81\x69\x83\x7d\x83\x8a\x83\x49\x81\x6a");

        KoopaFunction::startKoopaCamera(mHost, "\x98\x66\x90\xaf\x82\x6b\x82\x96\x82\x52\x93\xe0\x91\xa4\x82\xf0\x97\x8e\x89\xba");
        MR::startAction(mHost, "JumpSoon");

        KoopaFunction::getKoopaPlanetShadow(mHost)->appear();

        setNerve(GET_NERVE(KoopaDemoFallToPlanetVs3Lv3, KoopaDemoFallToPlanetVs3Lv3NrvFallInside));
    }
}

void KoopaDemoFallToPlanetVs3Lv3::exeFallInside() {
    MR::startAtmosphereLevelSE("SE_BM_LV_KOOPA_FALL_SUN_1");
    MR::moveAndTurnToPlayer(mHost, &mHost->mFront, ::sFallParam);

    if (MR::isBindedGround(mHost)) {
        MR::zeroVelocity(mHost);
        setNerve(GET_NERVE(KoopaDemoFallToPlanetVs3Lv3, KoopaDemoFallToPlanetVs3Lv3NrvLand));
    }
}

void KoopaDemoFallToPlanetVs3Lv3::exeLand() {
    if (MR::isFirstStep(this)) {
        MR::startAction(mHost, "Land");
        MR::startSound(mHost, "SE_BM_KOOPA_FLIP_RECOVER_END");
        MR::startSound(mHost, "SE_BV_KOOPA_LAND_HEAVY");

        MR::tryRumblePadStrong(mHost, WPAD_CHAN0);
        MR::shakeCameraNormalStrong();
    }

    if (MR::isActionEnd(mHost)) {
        setNerve(GET_NERVE(KoopaDemoFallToPlanetVs3Lv3, KoopaDemoFallToPlanetVs3Lv3NrvWaitPlayer));
    }
}

void KoopaDemoFallToPlanetVs3Lv3::exeWaitPlayer() {
    if (MR::isFirstStep(this)) {
        KoopaFunction::startFaceCtrl(mHost);
    }

    MR::moveAndTurnToPlayer(mHost, &mHost->mFront, ::sWaitParam);

    if (MR::isOnGroundPlayer()) {
        kill();
    }
}

KoopaDemoFallToPlanetVs3Lv3::~KoopaDemoFallToPlanetVs3Lv3() {
}
