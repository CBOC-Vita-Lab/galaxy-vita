#include "Game/NPC/RosettaDemoHeavensDoor.hpp"
#include "Game/Demo/DemoFunction.hpp"
#include "Game/LiveActor/PartsModel.hpp"
#include "Game/NPC/Rosetta.hpp"
#include "Game/NameObj/NameObjArchiveListCollector.hpp"
#include "Game/Util.hpp"

namespace {
    static const s32 sRosettaSwingVoiceFrame = 70;
    static const s32 sRosettaSwingFrame = 80;
    static const s32 sRosettaHideFrame = 10;
};  // namespace

namespace NrvRosettaDemoHeavensDoor1 {
    NEW_NERVE(RosettaDemoHeavensDoor1NrvWait, RosettaDemoHeavensDoor1, Wait);
    NEW_NERVE(RosettaDemoHeavensDoor1NrvFade, RosettaDemoHeavensDoor1, Fade);
    NEW_NERVE(RosettaDemoHeavensDoor1NrvDemo, RosettaDemoHeavensDoor1, Demo);
};  // namespace NrvRosettaDemoHeavensDoor1

RosettaDemoHeavensDoor1::RosettaDemoHeavensDoor1(Rosetta* pHost, const JMapInfoIter& rIter) : NerveExecutor("\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x66\x83\x82\x8e\xc0\x8d\x73\x8e\xd2"), mHost(pHost) {
    DemoFunction::tryCreateDemoTalkAnimCtrlForActor(mHost, "DemoGetPower", "\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""1]");
    initNerve(GET_NERVE(RosettaDemoHeavensDoor1, RosettaDemoHeavensDoor1NrvWait));

    mLightDomeModel = MR::createPartsModelNpc(mHost, "\x83\x89\x83\x43\x83\x67\x83\x68\x81\x5b\x83\x80", "LightDome", nullptr);
    mLightDomeModel->makeActorAppeared();
    mLightDomeModel->initFixedPosition(TVec3f(0.0f, -13.0f, -30.0f), TVec3f(0.0f, 0.0f, 0.0f), "Center");
    MR::startBrk(mLightDomeModel, "LightDome");
    MR::startBck(mLightDomeModel, "Appear");

    if (MR::isDemoCast(mHost, nullptr)) {
        MR::tryRegisterDemoCast(mLightDomeModel, rIter);
    }

    DemoFunction::tryCreateDemoTalkAnimCtrlForActor(mLightDomeModel, "DemoGetPower", "\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""1]");
    DemoFunction::registerDemoTalkMessageCtrl(mHost, mHost->mMsgCtrl);

    mLightHaloModel = MR::createPartsModelNpc(mHost, "\x83\x89\x83\x43\x83\x67\x8c\xe3\x8c\xf5", "DomeHalo", nullptr);
    mLightHaloModel->initFixedPosition(TVec3f(0.0f, 25.14f, -6.16f), TVec3f(0.0f, 0.0f, 0.0f), "Center");
    mLightHaloModel->makeActorAppeared();

    if (MR::isDemoCast(mHost, nullptr)) {
        MR::tryRegisterDemoCast(mLightHaloModel, rIter);
    }

    mLightHaloModel->mIsCalcOwnMtx = false;
    mLightHaloModel->mPosition.set(15064.593f, -7917.67f, 7541.112f);

    MR::needStageSwitchWriteA(mHost, rIter);
    MR::needStageSwitchWriteB(mHost, rIter);
    MR::registerDemoActionFunctor(mHost, MR::Functor(this, &RosettaDemoHeavensDoor1::preDemo), "\x8d\x82\x98\x4f\x8f\x6f\x8c\xbb[\x83\x66\x83\x82]");
    MR::registerDemoActionFunctor(mHost, MR::Functor(this, &RosettaDemoHeavensDoor1::pstDemo), "\x8d\x82\x98\x4f\x8f\x6f\x8c\xbb[\x83\x66\x83\x82\x8c\xe3]");
    MR::registerDemoActionFunctor(mHost, MR::Functor(this, &RosettaDemoHeavensDoor1::fadeOut), "\x8d\x82\x98\x4f\x8f\x6f\x8c\xbb[\x83\x74\x83\x46\x81\x5b\x83\x68\x83\x41\x83\x45\x83\x67]");
    MR::registerDemoActionFunctor(mHost, MR::Functor(this, &RosettaDemoHeavensDoor1::fadeIn), "\x8d\x82\x98\x4f\x8f\x6f\x8c\xbb[\x83\x74\x83\x46\x81\x5b\x83\x68\x83\x43\x83\x93]");
    MR::registerDemoActionFunctor(
        mHost, MR::Functor(this, &RosettaDemoHeavensDoor1::changeNerve< NrvRosettaDemoHeavensDoor1::RosettaDemoHeavensDoor1NrvDemo >),
        "\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""1]");
    MR::invalidateShadowAll(mHost);
    MR::invalidateHitSensors(mHost);
    MR::setClippingTypeSphere(mHost, 1500.0f);
    mHost->startDemo(this);
    mHost->makeActorDead();
}

void RosettaDemoHeavensDoor1::makeArchiveList(NameObjArchiveListCollector* pCollector, const JMapInfoIter& rIter) {
    pCollector->addArchive("LightDome");
    pCollector->addArchive("DomeHalo");
}

void RosettaDemoHeavensDoor1::preDemo() {
    MR::hidePlayer();
}

void RosettaDemoHeavensDoor1::pstDemo() {
    MR::startSound(mHost, "SE_OJ_ROSETTA_HALO_APPEAR");
}

void RosettaDemoHeavensDoor1::fadeOut() {
    MR::timeKeepDemoFadeOut();
}

void RosettaDemoHeavensDoor1::fadeIn() {
    mLightHaloModel->mIsCalcOwnMtx = true;

    MR::emitEffect(mLightDomeModel, "Light");
    MR::showPlayer();
    MR::timeKeepDemoFadeIn();
    MR::onSwitchB(mHost);
}

void RosettaDemoHeavensDoor1::exeWait() {
    if (MR::isFirstStep(this)) {
        MR::startAction(mHost, "DemoGetPowerStartWait");
    }

    if (MR::isNearPlayer(mHost->mMsgCtrl, 500.0f) && !MR::isTimeKeepDemoActive()) {
        MR::offPlayerControl();
        MR::timeKeepDemoFadeOut();
        MR::startBrk(mLightHaloModel, "Disappear");
        setNerve(GET_NERVE(RosettaDemoHeavensDoor1, RosettaDemoHeavensDoor1NrvFade));
    }
}

void RosettaDemoHeavensDoor1::exeFade() {
    if (MR::isStep(this, 90)) {
        MR::onPlayerControl(true);
        MR::startTimeKeepDemoMarioPuppetable(mHost, "\x83\x60\x83\x52\x83\x4b\x83\x43\x83\x68\x83\x66\x83\x82", "\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""1]");
        mLightHaloModel->kill();
    }
}

void RosettaDemoHeavensDoor1::exeDemo() {
    if (MR::isDemoPartFirstStep("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""6]")) {
        MR::deleteEffect(mLightDomeModel, "Light");
    }

    if (MR::isDemoPartActive("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""3]")) {
        s32 step = MR::getDemoPartStep("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""3]");

        if (step == ::sRosettaSwingVoiceFrame) {
            MR::startSound(mHost, "SE_SV_ROSETTA_SWING");
        }

        if (step == ::sRosettaSwingFrame) {
            MR::startSound(mHost, "SE_SM_ROSETTA_OP_SWING");
        }
    }

    if (MR::isDemoPartActive("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x89\xef\x98\x62""1]") || MR::isDemoPartActive("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x89\xef\x98\x62""2]") || MR::isDemoPartActive("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x89\xef\x98\x62""3]") ||
        MR::isDemoPartActive("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x89\xef\x98\x62""4]") || MR::isDemoPartActive("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""1]") || MR::isDemoPartActive("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""2]") ||
        MR::isDemoPartActive("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""3]") || MR::isDemoPartActive("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""4]") || MR::isDemoPartActive("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""5]")) {
        MR::startLevelSound(mHost, "SE_SM_LV_TICO_OP_WAIT");
    }

    if (MR::isDemoPartActive("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""6]")) {
        s32 step = MR::getDemoPartStep("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""6]");

        if (step < ::sRosettaHideFrame) {
            MR::startLevelSound(mHost, "SE_SM_LV_TICO_OP_WAIT");
        }

        if (step >= ::sRosettaHideFrame) {
            MR::startLevelSound(mHost, "SE_SM_LV_ROSETTA_OP_HIDE");
        }
    }
}

namespace NrvRosettaDemoHeavensDoor2 {
    NEW_NERVE(RosettaDemoHeavensDoor2NrvWait, RosettaDemoHeavensDoor2, Wait);
    NEW_NERVE(RosettaDemoHeavensDoor2NrvDemo, RosettaDemoHeavensDoor2, Demo);
};  // namespace NrvRosettaDemoHeavensDoor2

RosettaDemoHeavensDoor2::RosettaDemoHeavensDoor2(Rosetta* pHost, const JMapInfoIter& rIter)
    : NerveExecutor("\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x66\x83\x82\x8e\xc0\x8d\x73\x8e\xd2"), mDemoStarter(pHost), mHost(pHost) {
    DemoFunction::tryCreateDemoTalkAnimCtrlForScene(mHost, rIter, "DemoRedStar", "\x8b\xbd\x8f\x44[\x8a\x4a\x8e\x6e]", 0, 0);
    DemoFunction::registerDemoTalkMessageCtrl(mHost, mHost->mMsgCtrl);
    MR::registerDemoActionFunctor(
        mHost, MR::Functor(this, &RosettaDemoHeavensDoor2::changeNerve< NrvRosettaDemoHeavensDoor2::RosettaDemoHeavensDoor2NrvDemo >), "\x8b\xbd\x8f\x44[\x8a\x4a\x8e\x6e]");
    MR::needStageSwitchWriteA(mHost, rIter);

    if (MR::isOnGameEventFlagRosettaTalkAboutTicoInTower()) {
        mHost->makeActorDead();
    } else {
        MR::onSwitchA(mHost);
    }

    MR::invalidateShadowAll(mHost);
    MR::invalidateHitSensors(mHost);
    mHost->startDemo(this);
    initNerve(GET_NERVE(RosettaDemoHeavensDoor2, RosettaDemoHeavensDoor2NrvWait));
}

void RosettaDemoHeavensDoor2::makeArchiveList(NameObjArchiveListCollector* pCollector, const JMapInfoIter& rIter) {
}

void RosettaDemoHeavensDoor2::exeWait() {
    if (MR::isFirstStep(this)) {
        MR::startAction(mHost, "WaitB");
    }

    if (MR::isNearPlayer(mHost, 400.0f)) {
        mDemoStarter.start();
    }

    if (mDemoStarter.update()) {
        MR::tryStartTimeKeepDemoMarioPuppetable(mHost, "\x90\xd4\x82\xa2\x83\x58\x83\x5e\x81\x5b", "\x8b\xbd\x8f\x44[\x8a\x4a\x8e\x6e]");
        MR::onGameEventFlagRosettaTalkAboutTicoInTower();
    }
}
