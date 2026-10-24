#include "Game/NPC/TicoDemoGetPower.hpp"
#include "Game/Demo/DemoFunction.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/NPC/Tico.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SoundUtil.hpp"

namespace {
    static const s32 sTicoAppearFrame = 110;
    static const s32 sTicoAppearEndFrame = 140;
    static const s32 sTicoAppearVoiceFrame = 180;
    static const s32 sFanfareFrame = 10;
    static const s32 sWaitSeStopFrame = 65;
    static const s32 sFlashSpinSeFrame = 72;
    static const s32 sFlashLightSeFrame = 170;
    static const s32 sFusionSeFrame = 255;
    static const s32 sMarioJumpFrame = 420;
};  // namespace

namespace NrvTicoDemoGetPower {
    NEW_NERVE(TicoDemoGetPowerNrvWait, TicoDemoGetPower, Wait);
    NEW_NERVE(TicoDemoGetPowerNrvDemo, TicoDemoGetPower, Demo);
};  // namespace NrvTicoDemoGetPower

TicoDemoGetPower::TicoDemoGetPower(Tico* pHost, const JMapInfoIter& rIter) : NerveExecutor("\x83\x60\x83\x52\x83\x51\x83\x62\x83\x67\x83\x70\x83\x8f\x81\x5b\x83\x66\x83\x82\x8e\xc0\x8d\x73\x8e\xd2"), mHost(pHost) {
    initNerve(GET_NERVE(TicoDemoGetPower, TicoDemoGetPowerNrvWait));
    DemoFunction::tryCreateDemoTalkAnimCtrlForScene(mHost, rIter, "DemoGetPower", "\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""1]", 0, 0);
    MR::registerDemoActionFunctor(mHost, MR::Functor(this, &TicoDemoGetPower::startDemo), "\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""1]");
    mHost->makeActorDead();
}

void TicoDemoGetPower::startDemo() {
    mHost->makeActorAppeared();
    setNerve(GET_NERVE(TicoDemoGetPower, TicoDemoGetPowerNrvDemo));
    exeDemo();
}

void TicoDemoGetPower::exeWait() {
}

void TicoDemoGetPower::exeDemo() {
    if (MR::isDemoPartFirstStep("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""5]")) {
        MR::explainEnableToSpin(mHost);
    }

    if (MR::isDemoPartActive("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""3]")) {
        s32 partStep = MR::getDemoPartStep("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""3]");

        if (partStep == ::sTicoAppearFrame) {
            MR::startSound(mHost, "SE_SM_TICO_OP_APPEAR");
        }

        if (partStep == ::sTicoAppearEndFrame) {
            MR::startSound(mHost, "SE_SM_TICO_OP_APPEAR_END");
        }

        if (partStep == ::sTicoAppearVoiceFrame) {
            MR::startSound(mHost, "SE_SV_TICO_OP_APPEAR");
        }
    }

    if (MR::isDemoPartActive("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""4]")) {
        s32 partStep = MR::getDemoPartStep("\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""4]");

        if (partStep == ::sFanfareFrame) {
            MR::startSubBGM("BGM_STAR_POW_GET", false);
        }

        if (partStep <= ::sWaitSeStopFrame) {
            MR::startLevelSound(mHost, "SE_SM_LV_TICO_WAIT");
        }

        if (partStep == ::sFlashSpinSeFrame) {
            MR::startSound(mHost, "SE_SM_TICO_FLASH_SPIN");
        }

        if (partStep == ::sFlashLightSeFrame) {
            MR::startSound(mHost, "SE_SM_TICO_FLASH_LIGHT");
        }

        if (partStep == ::sFusionSeFrame) {
            MR::startSound(mHost, "SE_SM_TICO_FUSION");
        }

        if (partStep == ::sMarioJumpFrame) {
            MR::startSoundPlayer("SE_PV_POWER_STAR_GET", -1);
        }
    }
}
