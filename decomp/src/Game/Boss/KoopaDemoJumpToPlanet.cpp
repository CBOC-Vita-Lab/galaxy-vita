#include "Game/Boss/KoopaDemoJumpToPlanet.hpp"
#include "Game/Boss/Koopa.hpp"
#include "Game/Boss/KoopaBattleBase.hpp"
#include "Game/Boss/KoopaFunction.hpp"
#include "Game/Boss/KoopaPlanetShadow.hpp"
#include "Game/LiveActor/Nerve.hpp"
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
    static const s32 sFallWarpStepVs3 = 90;
};  // namespace

namespace NrvKoopaDemoJumpToPlanet {
    NEW_NERVE(KoopaDemoJumpToPlanetNrvStart, KoopaDemoJumpToPlanet, Start);
    NEW_NERVE(KoopaDemoJumpToPlanetNrvFall, KoopaDemoJumpToPlanet, Fall);
    NEW_NERVE(KoopaDemoJumpToPlanetNrvLand, KoopaDemoJumpToPlanet, Land);
    NEW_NERVE(KoopaDemoJumpToPlanetNrvWaitPlayer, KoopaDemoJumpToPlanet, WaitPlayer);
};  // namespace NrvKoopaDemoJumpToPlanet

KoopaDemoJumpToPlanet::KoopaDemoJumpToPlanet(Koopa* pKoopa) : ActorStateBase< Koopa >("Demo[\x98\x66\x90\xaf\x82\xdc\x82\xc5\x83\x57\x83\x83\x83\x93\x83\x76]", pKoopa) {
}

void KoopaDemoJumpToPlanet::init() {
    KoopaFunction::initKoopaCamera(mHost, "\x83\x45\x83\x46\x83\x43\x83\x67\x81\x69\x98\x66\x90\xaf\x82\xdc\x82\xc5\x83\x57\x83\x83\x83\x93\x83\x76\x81\x6a");
    KoopaFunction::initKoopaCamera(mHost, "\x97\x8e\x89\xba\x81\x69\x98\x66\x90\xaf\x82\xdc\x82\xc5\x83\x57\x83\x83\x83\x93\x83\x76\x81\x6a");
    KoopaFunction::initKoopaCamera(mHost, "\x83\x8f\x81\x5b\x83\x76\x8c\xe3\x81\x69\x98\x66\x90\xaf\x82\xdc\x82\xc5\x83\x57\x83\x83\x83\x93\x83\x76\x81\x6a");
    initNerve(GET_NERVE(KoopaDemoJumpToPlanet, KoopaDemoJumpToPlanetNrvStart));
}

void KoopaDemoJumpToPlanet::kill() {
    mIsDead = true;

    KoopaFunction::endKoopaCamera(mHost, "\x83\x45\x83\x46\x83\x43\x83\x67\x81\x69\x98\x66\x90\xaf\x82\xdc\x82\xc5\x83\x57\x83\x83\x83\x93\x83\x76\x81\x6a", false, -1);
    KoopaFunction::endKoopaCamera(mHost, "\x97\x8e\x89\xba\x81\x69\x98\x66\x90\xaf\x82\xdc\x82\xc5\x83\x57\x83\x83\x83\x93\x83\x76\x81\x6a", false, -1);
    KoopaFunction::endKoopaCamera(mHost, "\x83\x8f\x81\x5b\x83\x76\x8c\xe3\x81\x69\x98\x66\x90\xaf\x82\xdc\x82\xc5\x83\x57\x83\x83\x83\x93\x83\x76\x81\x6a", false, -1);
}

void KoopaDemoJumpToPlanet::startReady() {
    MR::onSwitchB(mHost);
}

void KoopaDemoJumpToPlanet::exeStart() {
    if (MR::isFirstStep(this)) {
        KoopaFunction::setKoopaPos(mHost, "\x90\xed\x93\xac\x8a\x4a\x8e\x6e\x81\x69\x83\x4e\x83\x62\x83\x70\x81\x6a");
        MR::setPlayerPosAndWait("\x90\xed\x93\xac\x8a\x4a\x8e\x6e\x81\x69\x83\x7d\x83\x8a\x83\x49\x81\x6a");

        KoopaFunction::startKoopaCamera(mHost, "\x97\x8e\x89\xba\x81\x69\x98\x66\x90\xaf\x82\xdc\x82\xc5\x83\x57\x83\x83\x83\x93\x83\x76\x81\x6a");
        MR::startAction(mHost, "JumpSoon");
        KoopaFunction::startFaceCtrl(mHost);

        setNerve(GET_NERVE(KoopaDemoJumpToPlanet, KoopaDemoJumpToPlanetNrvFall));
    }
}

void KoopaDemoJumpToPlanet::exeFall() {
    if (MR::isFirstStep(this) && KoopaFunction::getKoopaPlanetShadow(mHost)) {
        KoopaFunction::getKoopaPlanetShadow(mHost)->appear();
    }

    MR::moveAndTurnToPlayer(mHost, &mHost->mFront, ::sFallParam);

    if (KoopaFunction::isKoopaVs3(mHost) && MR::isStep(this, ::sFallWarpStepVs3)) {
        KoopaFunction::setKoopaPos(mHost, "\x82\x6b\x82\x96\x82\x50\x8a\x4a\x8e\x6e\x81\x69\x83\x4e\x83\x62\x83\x70\x81\x6a");
        MR::setPlayerPosAndWait("\x82\x6b\x82\x96\x82\x50\x8a\x4a\x8e\x6e\x81\x69\x83\x7d\x83\x8a\x83\x49\x81\x6a");

        KoopaFunction::endKoopaCamera(mHost, "\x97\x8e\x89\xba\x81\x69\x98\x66\x90\xaf\x82\xdc\x82\xc5\x83\x57\x83\x83\x83\x93\x83\x76\x81\x6a", false, -1);
        KoopaFunction::startKoopaCamera(mHost, "\x83\x8f\x81\x5b\x83\x76\x8c\xe3\x81\x69\x98\x66\x90\xaf\x82\xdc\x82\xc5\x83\x57\x83\x83\x83\x93\x83\x76\x81\x6a");
    }

    MR::startLevelSound(mHost, "SE_BM_LV_KOOPA_FALL_TO_PLANET");

    if (MR::isBindedGround(mHost)) {
        MR::zeroVelocity(mHost);

        setNerve(GET_NERVE(KoopaDemoJumpToPlanet, KoopaDemoJumpToPlanetNrvLand));
    }
}

void KoopaDemoJumpToPlanet::exeLand() {
    if (MR::isFirstStep(this)) {
        MR::startAction(mHost, "Land");
        MR::startSound(mHost, "SE_BM_KOOPA_FLIP_RECOVER_END");
        MR::startSound(mHost, "SE_BV_KOOPA_LAND_HEAVY");

        MR::tryRumblePadStrong(mHost, WPAD_CHAN0);
        MR::shakeCameraNormalStrong();
    }

    if (MR::isActionEnd(mHost)) {
        setNerve(GET_NERVE(KoopaDemoJumpToPlanet, KoopaDemoJumpToPlanetNrvWaitPlayer));
    }
}

void KoopaDemoJumpToPlanet::exeWaitPlayer() {
    if (MR::isFirstStep(this)) {
        KoopaFunction::startFaceCtrl(mHost);
    }

    MR::moveAndTurnToPlayer(mHost, &mHost->mFront, ::sWaitParam);

    if (MR::isOnGroundPlayer()) {
        KoopaFunction::startFaceCtrl(mHost);

        kill();
    }
}

KoopaDemoJumpToPlanet::~KoopaDemoJumpToPlanet() {
}
