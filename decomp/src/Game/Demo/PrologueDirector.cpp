#include "Game/Demo/PrologueDirector.hpp"
#include "Game/Camera/CameraTargetArg.hpp"
#include "Game/Camera/CameraTargetMtx.hpp"
#include "Game/LiveActor/ActorCameraInfo.hpp"
#include "Game/LiveActor/ModelObj.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Screen/PrologueLetter.hpp"
#include "Game/Screen/ProloguePictureBook.hpp"
#include "Game/Util/ActorCameraUtil.hpp"
#include "Game/Util/ActorSwitchUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/JointUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SoundUtil.hpp"

void PrologueDirector_FORCE_MATCH_SDATA2() {
    1.0f;
    0.0f;
}

namespace {
    static const char* sPictureBookDemoName = "\x83\x76\x83\x8d\x83\x8d\x81\x5b\x83\x4f\x83\x66\x83\x82";
    static const char* sArriveDemoName = "\x8e\xe5\x90\x6c\x8c\xf6\x83\x73\x81\x5b\x83\x60\x8f\xe9\x82\xc9\x93\x9e\x92\x85";
    static const s32 sPicBookStartWipeFrame = 60;
    static const s32 sPeachLetterWait = 20;
    static const s32 sPeachLetterStartWipeFrame = 60;
    static const s32 sPeachLetterEndWipeFrame = 60;
    static const s32 sArriveStartWipeFrame = 30;
    static const s32 sArriveEndWipeFrame = 30;
    static const s32 sGameStartWipeFrame = 30;
    static const s32 sGameStartFrame = 15;
    static const s32 sFallingStarStep = 130;

    PrologueHolder* getPrologueHolder() {
        return MR::getSceneObj< PrologueHolder >(SceneObj_PrologueHolder);
    }
};  // namespace

namespace {
    NEW_NERVE(PrologueDirectorNrvWait, PrologueDirector, Wait);
    NEW_NERVE(PrologueDirectorNrvPictureBook, PrologueDirector, PictureBook);
    NEW_NERVE(PrologueDirectorNrvPeachLetterStart, PrologueDirector, PeachLetterStart);
    NEW_NERVE(PrologueDirectorNrvPeachLetter, PrologueDirector, PeachLetter);
    NEW_NERVE(PrologueDirectorNrvPeachLetterWait, PrologueDirector, PeachLetterWait);
    NEW_NERVE(PrologueDirectorNrvPeachLetterEnd, PrologueDirector, PeachLetterEnd);
    NEW_NERVE(PrologueDirectorNrvBindWait, PrologueDirector, BindWait);
    NEW_NERVE(PrologueDirectorNrvArrive, PrologueDirector, Arrive);
    NEW_NERVE(PrologueDirectorNrvGameStart, PrologueDirector, GameStart);
};  // namespace

PrologueDirector::PrologueDirector(const char* pName)
    : LiveActor(pName), mPictureBook(nullptr), mLetter(nullptr), mScenery(nullptr), mMarioPosDummyModel(nullptr), mCameraTarget(nullptr), _D0(false) {
    _A0.identity();
}

void PrologueDirector::init(const JMapInfoIter& rIter) {
    if (MR::useStageSwitchWriteA(this, rIter)) {
        _D0 = true;
    }

    MR::connectToSceneMapObjMovement(this);
    MR::invalidateClipping(this);
    initNerve(GET_NERVE_ANON(PrologueDirectorNrvWait));
    createPictureBook();
    createLetter();
    createScenery();
    createMarioPosDummyModel();
    createCameraTarget();
    makeActorDead();

    MR::createSceneObj(SceneObj_PrologueHolder);
    ::getPrologueHolder()->registerPrologueObj(this);
}

void PrologueDirector::initAfterPlacement() {
    if (_D0) {
        MR::onSwitchA(this);
    }
}

void PrologueDirector::appear() {
    LiveActor::appear();
    setNerve(GET_NERVE_ANON(PrologueDirectorNrvWait));
    MR::forceCloseWipeFade();
    MR::forceOffImageEffect();

    if (_D0) {
        MR::offSwitchA(this);
    }
}

void PrologueDirector::kill() {
    LiveActor::kill();

    if (_D0) {
        MR::onSwitchA(this);
    }
}

void PrologueDirector::exeWait() {
    if (MR::tryStartDemoWithoutCinemaFrame(this, ::sPictureBookDemoName)) {
        MR::submitLevelSE();
        setNerve(GET_NERVE_ANON(PrologueDirectorNrvPictureBook));
        pauseOff();
        _A0.set(MR::getPlayerBaseMtx());
    }
}

void PrologueDirector::exePictureBook() {
    ActorCameraInfo cameraInfo = ActorCameraInfo();

    if (MR::isFirstStep(this)) {
        mPictureBook->appear();
        MR::openWipeFade(::sPicBookStartWipeFrame);
        mScenery->appear();
        MR::startAnimCameraTargetSelf(mScenery, &cameraInfo, "DemoLetter", 0, 1.0f);
        MR::startStageBGM("STM_PROLOGUE_01", false);
    }

    bool isEndOrDead = mPictureBook->isEnd() || MR::isDead(mPictureBook);

    if (isEndOrDead) {
        MR::forceCloseWipeFade();
        mPictureBook->kill();
        MR::stopStageBGM(90);
        setNerve(GET_NERVE_ANON(PrologueDirectorNrvPeachLetterStart));
    }
}

void PrologueDirector::exePeachLetterStart() {
    if (MR::isFirstStep(this)) {
        MR::openWipeFade(::sPeachLetterStartWipeFrame);
    }

    if (MR::isStep(this, 90)) {
        setNerve(GET_NERVE_ANON(PrologueDirectorNrvPeachLetter));
    }
}

void PrologueDirector::exePeachLetter() {
    if (MR::isFirstStep(this)) {
        MR::startStageBGM("STM_PROLOGUE_02", false);
        MR::startSystemSE("SE_SY_LETTER_APPEAR");
        MR::startSystemSE("SE_SV_PEACH_OPENING_LETTER");
        mLetter->appear();
    }

    if (MR::isDead(mLetter)) {
        MR::stopSystemSE("SE_SV_PEACH_OPENING_LETTER");
        setNerve(GET_NERVE_ANON(PrologueDirectorNrvPeachLetterWait));
    }
}

void PrologueDirector::exePeachLetterWait() {
    if (MR::isGreaterEqualStep(this, ::sPeachLetterWait)) {
        setNerve(GET_NERVE_ANON(PrologueDirectorNrvPeachLetterEnd));
    }
}

void PrologueDirector::exePeachLetterEnd() {
    if (MR::isFirstStep(this)) {
        MR::closeWipeFade(::sPeachLetterEndWipeFrame);
    }

    if (MR::isWipeActive()) {
        return;
    }

    ActorCameraInfo cameraInfo = ActorCameraInfo();

    MR::endAnimCamera(mScenery, &cameraInfo, "DemoLetter", 0, true);
    mScenery->kill();
    setNerve(GET_NERVE_ANON(PrologueDirectorNrvBindWait));
}

void PrologueDirector::exeBindWait() {
    if (MR::isFirstStep(this)) {
        MR::endDemo(this, ::sPictureBookDemoName);
    }

    if (MR::tryStartDemoMarioPuppetable(this, ::sArriveDemoName)) {
        pauseOff();
        mMarioPosDummyModel->appear();
        MR::startBck(mMarioPosDummyModel, "DemoPeachCastleGate");

        ActorCameraInfo cameraInfo = ActorCameraInfo();
        CameraTargetArg cameraTarget = CameraTargetArg(nullptr, mCameraTarget, nullptr, nullptr);

        MR::startAnimCameraTargetOther(mMarioPosDummyModel, &cameraInfo, "DemoPeachCastleGate", cameraTarget, 0, 1.0f);
        setNerve(GET_NERVE_ANON(PrologueDirectorNrvArrive));
    }
}

void PrologueDirector::exeArrive() {
    ActorCameraInfo cameraInfo = ActorCameraInfo();

    if (MR::isFirstStep(this)) {
        MR::permitLevelSE();

        TPos3f baseMtx;
        baseMtx.set(MR::getJointMtx(mMarioPosDummyModel, "MarioPosition"));

        TVec3f trans;
        baseMtx.getTrans(trans);

        MR::setPlayerBaseMtx(baseMtx);
        MR::startBckPlayer("DemoPeachCastleGate");
        MR::openWipeFade(::sArriveStartWipeFrame);
        MR::setImageEffectControlAuto();
    }

    TMtx34f baseMtx;
    baseMtx.set(MR::getJointMtx(mMarioPosDummyModel, "MarioPosition"));

    MR::setPlayerBaseMtx(baseMtx);

    if (MR::isStep(this, ::sFallingStarStep)) {
        MR::startAtmosphereSE("SE_DM_ARRIVE_CASTLE_STAR");
    }

    if (MR::isStep(this, MR::getBckFrameMaxPlayer("DemoPeachCastleGate") - ::sArriveEndWipeFrame)) {
        MR::closeWipeFade(::sArriveEndWipeFrame);
    }

    if (MR::isBckStoppedPlayer()) {
        MR::endAnimCamera(mMarioPosDummyModel, &cameraInfo, "DemoPeachCastleGate", 0, true);
        mMarioPosDummyModel->kill();
        setNerve(GET_NERVE_ANON(PrologueDirectorNrvGameStart));
    }
}

void PrologueDirector::exeGameStart() {
    if (MR::isFirstStep(this)) {
        MR::setPlayerBaseMtx(_A0);
        MR::forceCloseWipeFade();
    }

    if (MR::isStep(this, ::sGameStartFrame)) {
        MR::openWipeFade(::sGameStartWipeFrame);
        MR::endDemo(this, ::sArriveDemoName);
        MR::initPlayerAfterOpeningDemo();
        kill();
    }
}

void PrologueDirector::createPictureBook() {
    mPictureBook = new ProloguePictureBook();
    mPictureBook->initWithoutIter();
    mPictureBook->kill();
}

void PrologueDirector::createLetter() {
    mLetter = new PrologueLetter("\x83\x73\x81\x5b\x83\x60\x82\xa9\x82\xe7\x82\xcc\x8e\xe8\x8e\x86");
    mLetter->initWithoutIter();
}

void PrologueDirector::createScenery() {
    mScenery = new ModelObj("\x94\x77\x8c\x69\x8f\x91\x8a\x84", "DemoLetter", nullptr, MR::DrawBufferType_MapObjStrongLight, -2, -2, false);

    MR::invalidateClipping(mScenery);
    mScenery->initWithoutIter();
    MR::initLightCtrl(mScenery);
    mScenery->kill();

    ActorCameraInfo cameraInfo = ActorCameraInfo();

    MR::initAnimCamera(mScenery, &cameraInfo, "DemoLetter");
}

void PrologueDirector::createMarioPosDummyModel() {
    mMarioPosDummyModel = new ModelObj("\x83\x7d\x83\x8a\x83\x49\x82\xcc\x8c\x6f\x98\x48", "DemoPeachCastleGate", nullptr, -2, -2, -2, false);
    mMarioPosDummyModel->initWithoutIter();

    MR::invalidateClipping(mMarioPosDummyModel);

    mMarioPosDummyModel->kill();
    mMarioPosDummyModel->mPosition.zero();

    ActorCameraInfo cameraInfo = ActorCameraInfo();

    MR::initAnimCamera(mMarioPosDummyModel, &cameraInfo, "DemoPeachCastleGate");
}

void PrologueDirector::createCameraTarget() {
    mCameraTarget = new CameraTargetMtx("\x83\x4a\x83\x81\x83\x89\x83\x5e\x81\x5b\x83\x51\x83\x62\x83\x67\x83\x5f\x83\x7e\x81\x5b");
    mCameraTarget->mMatrix.identity();
}

void PrologueDirector::control() {
}

void PrologueDirector::pauseOff() {
    MR::requestMovementOn(mPictureBook);
    mLetter->pauseOff();
    MR::requestMovementOn(mScenery);
    MR::requestMovementOn(mMarioPosDummyModel);
}

PrologueHolder::PrologueHolder(const char* pName) : NameObj(pName), mDirector(nullptr) {
}

void PrologueHolder::registerPrologueObj(PrologueDirector* pDirector) {
    mDirector = pDirector;
}

void PrologueHolder::start() {
    mDirector->appear();
}

namespace MR {
    void startPrologue() {
        ::getPrologueHolder()->start();
    }
};  // namespace MR
