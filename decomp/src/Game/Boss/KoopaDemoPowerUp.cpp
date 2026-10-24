#include "Game/Boss/KoopaDemoPowerUp.hpp"
#include "Game/Boss/Koopa.hpp"
#include "Game/Boss/KoopaFunction.hpp"
#include "Game/Boss/KoopaPowerUpSwitch.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Util/ActorSwitchUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/NerveUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SoundUtil.hpp"

namespace {
    static const s32 sStepToScreenBlur = 65;
    static const s32 sScreenBlurStep = 135;
    static const s32 sStepToScreenBlurFinal = 50;
    static const s32 sScreenBlurStepFinal = 140;
};  // namespace

namespace NrvKoopaDemoPowerUp {
    NEW_NERVE(KoopaDemoPowerUpNrvWaitDemo, KoopaDemoPowerUp, WaitDemo);
    NEW_NERVE(KoopaDemoPowerUpNrvDemo, KoopaDemoPowerUp, Demo);
};  // namespace NrvKoopaDemoPowerUp

KoopaDemoPowerUp::~KoopaDemoPowerUp() {
}

KoopaDemoPowerUp::KoopaDemoPowerUp(Koopa* pKoopa) : ActorStateBase< Koopa >("Demo[\x83\x70\x83\x8f\x81\x5b\x83\x41\x83\x62\x83\x76]", pKoopa) {
}

void KoopaDemoPowerUp::init() {
    KoopaFunction::initKoopaCamera(getHost(), "\x83\x70\x83\x8f\x81\x5b\x83\x41\x83\x62\x83\x76\x83\x66\x83\x82");
    KoopaFunction::initKoopaCamera(getHost(), "\x8d\xc5\x8f\x49\x83\x70\x83\x8f\x81\x5b\x83\x41\x83\x62\x83\x76\x83\x66\x83\x82");

    MR::createCenterScreenBlur();
    initNerve(GET_NERVE(KoopaDemoPowerUp, KoopaDemoPowerUpNrvWaitDemo));
}

void KoopaDemoPowerUp::appear() {
    mIsDead = false;

    setNerve(GET_NERVE(KoopaDemoPowerUp, KoopaDemoPowerUpNrvWaitDemo));
}

void KoopaDemoPowerUp::kill() {
    mIsDead = true;

    KoopaFunction::endKoopaCamera(getHost(), "\x83\x70\x83\x8f\x81\x5b\x83\x41\x83\x62\x83\x76\x83\x66\x83\x82", false, -1);
    KoopaFunction::endKoopaCamera(getHost(), "\x8d\xc5\x8f\x49\x83\x70\x83\x8f\x81\x5b\x83\x41\x83\x62\x83\x76\x83\x66\x83\x82", false, -1);
}

void KoopaDemoPowerUp::exeWaitDemo() {
    if (!MR::tryStartDemoMarioPuppetable(getHost(), "\x83\x70\x83\x8f\x81\x5b\x83\x41\x83\x62\x83\x76\x83\x66\x83\x82")) {
        return;
    }

    if (KoopaFunction::isKoopaVs1(getHost()) || KoopaFunction::isKoopaVs2(getHost())) {
        KoopaFunction::setKoopaPos(getHost(), "\x83\x70\x83\x8f\x81\x5b\x83\x41\x83\x62\x83\x76\x83\x66\x83\x82\x81\x69\x83\x4e\x83\x62\x83\x70\x81\x6a");
        MR::setPlayerPosAndWait("\x83\x70\x83\x8f\x81\x5b\x83\x41\x83\x62\x83\x76\x83\x66\x83\x82\x81\x69\x83\x7d\x83\x8a\x83\x49\x81\x6a");

        if (KoopaFunction::isKoopaLv3(getHost())) {
            MR::startAction(getHost(), "DemoKoopaPowerUpFinal");
            KoopaFunction::startKoopaTargetCamera(getHost(), "\x8d\xc5\x8f\x49\x83\x70\x83\x8f\x81\x5b\x83\x41\x83\x62\x83\x76\x83\x66\x83\x82");

            MR::onSwitchB(KoopaFunction::getKoopaPowerUpSwitch(getHost()));
        } else {
            MR::startAction(getHost(), "DemoKoopaPowerUp");
            KoopaFunction::startKoopaTargetCamera(getHost(), "\x83\x70\x83\x8f\x81\x5b\x83\x41\x83\x62\x83\x76\x83\x66\x83\x82");

            MR::onSwitchA(KoopaFunction::getKoopaPowerUpSwitch(getHost()));
        }
    } else {
        KoopaFunction::setKoopaPos(getHost(), "\x83\x70\x83\x8f\x81\x5b\x83\x41\x83\x62\x83\x76\x83\x66\x83\x82\x82\x6b\x82\x96\x82\x52\x81\x69\x83\x4e\x83\x62\x83\x70\x81\x6a");
        MR::setPlayerPosAndWait("\x83\x70\x83\x8f\x81\x5b\x83\x41\x83\x62\x83\x76\x83\x66\x83\x82\x82\x6b\x82\x96\x82\x52\x81\x69\x83\x7d\x83\x8a\x83\x49\x81\x6a");

        MR::startAction(getHost(), "DemoKoopaPowerUpFinal");
        KoopaFunction::startKoopaTargetCamera(getHost(), "\x8d\xc5\x8f\x49\x83\x70\x83\x8f\x81\x5b\x83\x41\x83\x62\x83\x76\x83\x66\x83\x82");

        MR::onSwitchA(KoopaFunction::getKoopaPowerUpSwitch(getHost()));
    }

    MR::startBckPlayer("BattleWait");
    KoopaFunction::startRecoverKoopaArmor(getHost());
    KoopaFunction::startRecoverKoopaTailThorn(getHost());

    KoopaFunction::endFaceCtrl(getHost(), -1);

    setNerve(GET_NERVE(KoopaDemoPowerUp, KoopaDemoPowerUpNrvDemo));
}

void KoopaDemoPowerUp::exeDemo() {
    if (KoopaFunction::isKoopaLv3(mHost)) {
        if (MR::isStep(this, ::sStepToScreenBlurFinal)) {
            MR::startCenterScreenBlur(::sScreenBlurStepFinal, 15.0f, 80, 5, 30);
        }
    } else {
        if (MR::isStep(this, ::sStepToScreenBlur)) {
            MR::startCenterScreenBlur(::sScreenBlurStep, 15.0f, 80, 5, 30);
        }
    }

    if (!MR::isActionEnd(mHost)) {
        return;
    }

    if (KoopaFunction::isKoopaVs3(mHost) || KoopaFunction::isKoopaLv3(mHost)) {
        KoopaFunction::endKoopaCamera(mHost, "\x8d\xc5\x8f\x49\x83\x70\x83\x8f\x81\x5b\x83\x41\x83\x62\x83\x76\x83\x66\x83\x82", false, -1);
    } else {
        KoopaFunction::endKoopaCamera(mHost, "\x83\x70\x83\x8f\x81\x5b\x83\x41\x83\x62\x83\x76\x83\x66\x83\x82", false, -1);
    }

    MR::endDemo(mHost, "\x83\x70\x83\x8f\x81\x5b\x83\x41\x83\x62\x83\x76\x83\x66\x83\x82");

    TVec3f gravity;
    gravity.negate(mHost->mGravity);
    MR::appearStarPieceToDirection(mHost, mHost->mPosition, gravity, 10, 50.0f, 60.0f, false);

    MR::startSound(mHost, "SE_OJ_STAR_PIECE_BURST");

    kill();
}
