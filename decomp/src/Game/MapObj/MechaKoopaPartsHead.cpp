#include "Game/MapObj/MechaKoopaPartsHead.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/MapObj/MapObjActorInitInfo.hpp"
#include "Game/Util.hpp"

void MechaKoopaPartsHead_FORCE_MATCH_SDATA2() {
    (void)0.0f;
    (void)2.0f;
}

namespace {
    static const s32 sStepForWhiteFadeOut = 60;
    static const s32 sStepForWhiteFadeIn = 60;
    static const s32 sStepToBreakFaceEffect = 120;
    static const s32 sStepToExplosionEffect = 300;
    static const char* const sDemoPartNameBreak = "\x89\xf3\x82\xea\x8a\x4a\x8e\x6e";
    static const char* const sDemoPartNameFadeOut = "\x83\x74\x83\x46\x81\x5b\x83\x68\x83\x41\x83\x45\x83\x67";
    static const char* const sDemoPartNameWhite = "\x94\x92\x89\xe6\x96\xca";
    static const char* const sDemoPartNameFadeIn = "\x83\x74\x83\x46\x81\x5b\x83\x68\x83\x43\x83\x93";
};  // namespace

namespace NrvMechaKoopaPartsHead {
    NEW_NERVE(MechaKoopaPartsHeadNrvWait, MechaKoopaPartsHead, Wait);
    NEW_NERVE(MechaKoopaPartsHeadNrvDemoBreak, MechaKoopaPartsHead, DemoBreak);
    NEW_NERVE(MechaKoopaPartsHeadNrvDemoWhiteFadeOut, MechaKoopaPartsHead, DemoWhiteFadeOut);
    NEW_NERVE(MechaKoopaPartsHeadNrvDemoWhiteWait, MechaKoopaPartsHead, DemoWhiteWait);
    NEW_NERVE(MechaKoopaPartsHeadNrvDemoWhiteFadeIn, MechaKoopaPartsHead, DemoWhiteFadeIn);
    NEW_NERVE(MechaKoopaPartsHeadNrvDemoAppearStar, MechaKoopaPartsHead, DemoAppearStar);
};  // namespace NrvMechaKoopaPartsHead

MechaKoopaPartsHead::MechaKoopaPartsHead(const char* pName) : MapObjActor(pName) {
}

void MechaKoopaPartsHead::init(const JMapInfoIter& rIter) {
    MapObjActor::init(rIter);
    MapObjActorInitInfo initInfo;
    initInfo.setupHioNode("\x92\x6e\x8c\x60\x83\x49\x83\x75\x83\x57\x83\x46");
    initInfo.setupDefaultPos();
    initInfo.setupConnectToScene();
    initInfo.setupEffect(nullptr);
    initInfo.setupSound(6);
    initInfo.setupNerve(GET_NERVE(MechaKoopaPartsHead, MechaKoopaPartsHeadNrvWait));
    initialize(rIter, initInfo);
    MR::declarePowerStar(this);
    MR::registerDemoActionNerve(this, GET_NERVE(MechaKoopaPartsHead, MechaKoopaPartsHeadNrvDemoBreak), ::sDemoPartNameBreak);
    MR::registerDemoActionNerve(this, GET_NERVE(MechaKoopaPartsHead, MechaKoopaPartsHeadNrvDemoWhiteFadeOut), ::sDemoPartNameFadeOut);
    MR::registerDemoActionNerve(this, GET_NERVE(MechaKoopaPartsHead, MechaKoopaPartsHeadNrvDemoWhiteWait), ::sDemoPartNameWhite);
    MR::registerDemoActionNerve(this, GET_NERVE(MechaKoopaPartsHead, MechaKoopaPartsHeadNrvDemoWhiteFadeIn), ::sDemoPartNameFadeIn);

    if (MR::isValidSwitchA(this)) {
        MR::onSwitchA(this);
    }
}

void MechaKoopaPartsHead::exeWait() {
}

void MechaKoopaPartsHead::exeDemoBreak() {
    if (MR::isFirstStep(this)) {
        MR::startSound(this, "SE_BM_MECHA_KOOPA_DOWN");
        MR::moveVolumeStageBGM(0.0f, 30);
        MR::shakeCameraInfinity(this, 0.2f, 2.0f);
    }

    MR::tryRumblePadMiddle(this, WPAD_CHAN0);
    MR::startLevelSound(this, "SE_BM_LV_MECHA_KOOPA_PRE_BREAK");

    if (MR::isStep(this, ::sStepToBreakFaceEffect)) {
        MR::emitEffect(this, "Explosion1");
        MR::emitEffect(this, "Explosion2");
        MR::emitEffect(this, "ExplosionEye1");
        MR::emitEffect(this, "ExplosionEye2");
        MR::startSound(this, "SE_OJ_M_KOOPA_EYE_BREAK");
    }

    if (MR::isStep(this, ::sStepToExplosionEffect)) {
        MR::emitEffect(this, "Explosion");
        MR::startSound(this, "SE_BM_MECHA_KOOPA_BREAK_FIRE_ST");
    }

    if (MR::isGreaterStep(this, ::sStepToExplosionEffect)) {
        MR::startLevelSound(this, "SE_BM_LV_MECHA_KOOPA_BREAK_FIRE");
    }
}

void MechaKoopaPartsHead::exeDemoWhiteFadeOut() {
    if (MR::isFirstStep(this)) {
        MR::stopShakingCamera(this);
        MR::tryRumblePadStrong(this, WPAD_CHAN0);
        MR::shakeCameraStrong();
        MR::startSound(this, "SE_BM_MECHA_KOOPA_EXPLODE");
        MR::closeWipeWhiteFade(::sStepForWhiteFadeOut);
    }
}

void MechaKoopaPartsHead::exeDemoWhiteWait() {
    if (MR::isFirstStep(this)) {
        MR::hideModel(this);

        if (MR::isValidSwitchA(this)) {
            MR::offSwitchA(this);
        }

        MR::setPlayerPos("\x94\x9a\x94\x6a\x83\x66\x83\x82\x8c\xe3\x83\x7d\x83\x8a\x83\x49");
        MR::setPlayerStateWait();
    }
}

void MechaKoopaPartsHead::exeDemoWhiteFadeIn() {
    if (MR::isFirstStep(this)) {
        MR::openWipeWhiteFade(::sStepForWhiteFadeIn);
    }

    if (MR::isDemoPartLastStep(::sDemoPartNameFadeIn)) {
        setNerve(GET_NERVE(MechaKoopaPartsHead, MechaKoopaPartsHeadNrvDemoAppearStar));
    }
}

void MechaKoopaPartsHead::exeDemoAppearStar() {
    if (MR::isFirstStep(this)) {
        MR::startAfterBossBGM();
        MR::requestAppearPowerStar(this);
    }

    if (MR::isEndPowerStarAppearDemo(this)) {
        kill();
    }
}
