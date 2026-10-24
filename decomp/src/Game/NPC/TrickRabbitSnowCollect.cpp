#include "Game/NPC/TrickRabbitSnowCollect.hpp"
#include "Game/LiveActor/ModelObj.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Screen/BombTimerLayout.hpp"
#include "Game/Util.hpp"

namespace {
    static const f32 sInStartEventRange = 700.0f;
};  // namespace

namespace NrvTrickRabbitFreeRunCollect {
    NEW_NERVE(TrickRabbitSnowCollectNrvTryDemo, TrickRabbitSnowCollect, TryDemo);
    NEW_NERVE(TrickRabbitSnowCollectNrvWait, TrickRabbitSnowCollect, Wait);
    NEW_NERVE(TrickRabbitSnowCollectNrvStartWipeOut, TrickRabbitSnowCollect, StartWipeOut);
    NEW_NERVE(TrickRabbitSnowCollectNrvStartWipeIn, TrickRabbitSnowCollect, StartWipeIn);
    NEW_NERVE(TrickRabbitSnowCollectNrvStartTalk, TrickRabbitSnowCollect, StartTalk);
    NEW_NERVE(TrickRabbitSnowCollectNrvWaitHideSnow, TrickRabbitSnowCollect, WaitHideSnow);
    NEW_NERVE(TrickRabbitSnowCollectNrvStart, TrickRabbitSnowCollect, Start);
    NEW_NERVE(TrickRabbitSnowCollectNrvFailedWipeOut, TrickRabbitSnowCollect, FailedWipeOut);
    NEW_NERVE(TrickRabbitSnowCollectNrvFailedWipeIn, TrickRabbitSnowCollect, FailedWipeIn);
    NEW_NERVE(TrickRabbitSnowCollectNrvFailedTalk, TrickRabbitSnowCollect, FailedTalk);
    NEW_NERVE(TrickRabbitSnowCollectNrvSuccessWipeOut, TrickRabbitSnowCollect, SuccessWipeOut);
    NEW_NERVE(TrickRabbitSnowCollectNrvSuccessWipeIn, TrickRabbitSnowCollect, SuccessWipeIn);
    NEW_NERVE(TrickRabbitSnowCollectNrvSuccessTalk, TrickRabbitSnowCollect, SuccessTalk);
    NEW_NERVE(TrickRabbitSnowCollectNrvTakeOutStar, TrickRabbitSnowCollect, TakeOutStar);
    NEW_NERVE(TrickRabbitSnowCollectNrvAppearPowerStar, TrickRabbitSnowCollect, AppearPowerStar);
    NEW_NERVE(TrickRabbitSnowCollectNrvEnd, TrickRabbitSnowCollect, End);
};  // namespace NrvTrickRabbitFreeRunCollect

TrickRabbitSnowCollect::TrickRabbitSnowCollect(const char* pName)
    : LiveActor(pName), mCameraInfo(), mPowerStarDemoModel(), mMsgCtrl(), mTimerLayout(), mRabbit(), mRabbitNum(), mTimeLimit(180), mIsDemo() {
    mBaseMtx.identity();
}

void TrickRabbitSnowCollect::init(const JMapInfoIter& rIter) {
    MR::initDefaultPos(this, rIter);
    MR::getJMapInfoMatrixFromRT(rIter, &mBaseMtx);
    MR::connectToSceneNpcMovement(this);
    MR::invalidateClipping(this);
    MR::getJMapInfoArg0NoInit(rIter, &mTimeLimit);
    initRabbits(rIter);
    initTalk(rIter);

    mTimerLayout = new BombTimerLayout(true);
    mTimerLayout->initWithoutIter();

    MR::initMultiActorCamera(this, rIter, &mCameraInfo, "\x83\x45\x83\x54\x83\x4d\x82\xc6\x89\xef\x98\x62");

    mPowerStarDemoModel = MR::createPowerStarDemoModel(this, "\x83\x70\x83\x8f\x81\x5b\x83\x58\x83\x5e\x81\x5b\x83\x66\x83\x82\x83\x82\x83\x66\x83\x8b", mRabbit[2]->getBaseMtx());
    mPowerStarDemoModel->kill();

    initNerve(GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvWait));
    initSound(4, false);
    MR::declarePowerStar(this);
    MR::tryRegisterNamePosLinkObj(this, rIter);
    makeActorAppeared();
}

void TrickRabbitSnowCollect::initRabbits(const JMapInfoIter& rIter) {
    mRabbitNum = MR::getChildObjNum(rIter);
    mRabbit = new TrickRabbitSnow*[mRabbitNum];

    TrickRabbitCollectListener* listener = new TrickRabbitCollectListener(this);

    for (s32 i = 0; i < mRabbitNum; i++) {
        const char* childObjName;
        MR::getChildObjName(&childObjName, rIter, i);

        mRabbit[i] = new TrickRabbitSnow("\x90\xe1\x83\x45\x83\x54\x83\x4d");
        mRabbit[i]->setListener(listener);
        MR::initChildObj(mRabbit[i], rIter, i);
    }
}

void TrickRabbitSnowCollect::initTalk(const JMapInfoIter& rIter) {
    mMsgCtrl = MR::createTalkCtrl(this, rIter, "SnowRabbitCollect", TVec3f(0.0f, 120.0f, 0.0f), nullptr);
}

MtxPtr TrickRabbitSnowCollect::getBaseMtx() const {
    return (MtxPtr)&mBaseMtx;
}

void TrickRabbitSnowCollect::setStartPosition() {
    for (s32 i = 0; i < mRabbitNum; i++) {
        MR::requestMovementOn(mRabbit[i]);
    }

    MR::setPlayerLinkPosAndWait(this, "\x83\x7d\x83\x8a\x83\x49\x88\xca\x92\x75");
}

void TrickRabbitSnowCollect::setFinishPosition() {
    for (s32 i = 0; i < mRabbitNum; i++) {
        mRabbit[i]->setFinishPosition();
    }

    MR::setPlayerLinkPosAndWait(this, "\x83\x7d\x83\x8a\x83\x49\x88\xca\x92\x75");
}

void TrickRabbitSnowCollect::noticeCaught(TrickRabbitSnow* pSubject) {
    for (s32 i = 0; i < mRabbitNum; i++) {
        if (pSubject == mRabbit[i]) {
            continue;
        }

        mRabbit[i]->setNotCaughtable();
    }
}

void TrickRabbitSnowCollect::noticeGiveUp(TrickRabbitSnow* pSubject) {
    for (s32 i = 0; i < mRabbitNum; i++) {
        if (!mRabbit[i]->isGiveUp()) {
            return;
        }
    }

    pSubject->mIsValidAppearStarPiece = false;

    startSuccessDemo();
}

void TrickRabbitSnowCollect::startSuccessDemo() {
    if (mIsDemo) {
        return;
    }

    mIsDemo = true;

    MR::startSystemSE("SE_SY_TOTAL_COMPLETE");
    MR::requestStartDemoMarioPuppetableWithoutCinemaFrame(this, "\x90\xe1\x83\x45\x83\x54\x83\x4d\x8f\x57\x82\xdf\x90\xac\x8c\xf7",
                                                          GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvSuccessWipeOut),
                                                          GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvTryDemo));
}

void TrickRabbitSnowCollect::startFailedDemo() {
    if (mIsDemo) {
        return;
    }

    mIsDemo = true;

    MR::requestStartDemoMarioPuppetableWithoutCinemaFrame(this, "\x90\xe1\x83\x45\x83\x54\x83\x4d\x8f\x57\x82\xdf\x8e\xb8\x94\x73",
                                                          GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvFailedWipeOut),
                                                          GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvTryDemo));
}

void TrickRabbitSnowCollect::exeTryDemo() {
}

void TrickRabbitSnowCollect::exeWait() {
    if (MR::isNearPlayer(this, ::sInStartEventRange) && MR::isOnGroundPlayer()) {
        setNerve(GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvStartWipeOut));
    }
}

void TrickRabbitSnowCollect::exeStartWipeOut() {
    if (MR::isFirstStep(this)) {
        MR::closeWipeFade();
    }

    if (MR::isWipeActive()) {
        return;
    }

    MR::requestStartDemoMarioPuppetableWithoutCinemaFrame(this, "\x90\xe1\x83\x45\x83\x54\x83\x4d\x8a\x4a\x8e\x6e",
                                                          GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvStartWipeIn),
                                                          GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvTryDemo));
}

void TrickRabbitSnowCollect::exeStartWipeIn() {
    if (MR::isFirstStep(this)) {
        MR::tryPlayerKillTakingActor();
        MR::startBckPlayer("BattleWait");
        MR::startMultiActorCameraTargetPlayer(this, mCameraInfo, "\x83\x45\x83\x54\x83\x4d\x82\xc6\x89\xef\x98\x62", 0);
        setStartPosition();
        MR::openWipeFade();
    }

    if (MR::isWipeActive()) {
        return;
    }

    setNerve(GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvStartTalk));
}

void TrickRabbitSnowCollect::exeStartTalk() {
    if (MR::isFirstStep(this)) {
        setStartPosition();
    }

    if (MR::tryTalkForceWithoutDemoMarioPuppetableAtEnd(mMsgCtrl)) {
        setNerve(GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvWaitHideSnow));
    }
}

void TrickRabbitSnowCollect::exeWaitHideSnow() {
    if (MR::isFirstStep(this)) {
        for (s32 i = 0; i < mRabbitNum; i++) {
            mRabbit[i]->requestStartHideSnow();
        }
    }

    bool isAllHideSnow = true;

    for (s32 i = 0; i < mRabbitNum; i++) {
        if (mRabbit[i]->isHideSnow()) {
            continue;
        }

        isAllHideSnow = false;
        break;
    }

    if (isAllHideSnow) {
        MR::endMultiActorCamera(this, mCameraInfo, "\x83\x45\x83\x54\x83\x4d\x82\xc6\x89\xef\x98\x62", 0, -1);
        MR::endDemo(this, "\x90\xe1\x83\x45\x83\x54\x83\x4d\x8a\x4a\x8e\x6e");
        setNerve(GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvStart));
    }
}

void TrickRabbitSnowCollect::exeStart() {
    if (MR::isFirstStep(this)) {
        MR::startEventBGM(1);
        mTimerLayout->setTimeLimit(mTimeLimit * 60);
        mTimerLayout->mDangerTransFrame = 20 * 60;
        mTimerLayout->appear();
    }

    if (!MR::isPlayerDead() && !MR::isPlayerConfrontDeath()) {
        u32 restTime = mTimerLayout->getRestTime();

        if (restTime == 0) {
            MR::startSystemSE("SE_SY_E3_TIMER_TIME_UP");
        } else if (restTime <= 120) {
            if (restTime % 60 == 0) {
                MR::startSystemSE("SE_SY_E3_TIMER_COUNT_4");
            }
        } else if (restTime <= 360) {
            if (restTime % 60 == 0) {
                MR::startSystemSE("SE_SY_E3_TIMER_COUNT_2");
            }
        } else if (restTime <= 1200) {
            if (restTime % 60 == 0) {
                MR::startSystemSE("SE_SY_E3_TIMER_COUNT_1");
            }
        }
    }

    if (mTimerLayout->isReadyToTimeUp()) {
        startFailedDemo();
    }
}

void TrickRabbitSnowCollect::exeFailedWipeOut() {
    if (MR::isFirstStep(this)) {
        MR::stopStageBGM(60);
        MR::closeWipeFade();
    }

    if (MR::isWipeActive()) {
        return;
    }

    MR::curePlayerElementMode();
    MR::tryPlayerKillTakingActor();
    mTimerLayout->kill();
    setNerve(GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvFailedWipeIn));
}

void TrickRabbitSnowCollect::exeFailedWipeIn() {
    if (MR::isFirstStep(this)) {
        setFinishPosition();
        MR::startBckPlayer("BattleWait");
        MR::startMultiActorCameraTargetPlayer(this, mCameraInfo, "\x83\x45\x83\x54\x83\x4d\x82\xc6\x89\xef\x98\x62", 0);
        MR::startLastStageBGM();
        MR::openWipeFade();
    }

    if (MR::isWipeActive()) {
        return;
    }

    MR::forwardNodeNextBranchRight(mMsgCtrl);
    setNerve(GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvFailedTalk));
}

void TrickRabbitSnowCollect::exeFailedTalk() {
    if (MR::tryTalkForceWithoutDemoMarioPuppetableAtEnd(mMsgCtrl)) {
        MR::endMultiActorCamera(this, mCameraInfo, "\x83\x45\x83\x54\x83\x4d\x82\xc6\x89\xef\x98\x62", 1, -1);
        MR::endDemo(this, "\x90\xe1\x83\x45\x83\x54\x83\x4d\x8f\x57\x82\xdf\x8e\xb8\x94\x73");
        MR::startSoundPlayer("SE_PM_LAST_DAMAGE", -1);
        MR::forceKillPlayerByGroundRace();
        setNerve(GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvEnd));
    }
}

void TrickRabbitSnowCollect::exeSuccessWipeOut() {
    if (MR::isFirstStep(this)) {
        MR::stopStageBGM(120);
        MR::closeWipeFade();
    }

    if (MR::isWipeActive()) {
        return;
    }

    MR::curePlayerElementMode();
    MR::tryPlayerKillTakingActor();
    setFinishPosition();
    setNerve(GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvSuccessWipeIn));
}

void TrickRabbitSnowCollect::exeSuccessWipeIn() {
    if (MR::isFirstStep(this)) {
        mTimerLayout->kill();
        MR::startBckPlayer("BattleWait");
        MR::startMultiActorCameraTargetPlayer(this, mCameraInfo, "\x83\x45\x83\x54\x83\x4d\x82\xc6\x89\xef\x98\x62", -1);
        MR::startLastStageBGM();
        MR::openWipeFade();
    }

    if (MR::isWipeActive()) {
        return;
    }

    MR::forwardNodeNextBranchLeft(mMsgCtrl);
    setNerve(GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvSuccessTalk));
}

void TrickRabbitSnowCollect::exeSuccessTalk() {
    if (MR::tryTalkForceWithoutDemoMarioPuppetableAtEnd(mMsgCtrl)) {
        setNerve(GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvTakeOutStar));
    }
}

void TrickRabbitSnowCollect::exeTakeOutStar() {
    if (MR::isFirstStep(this)) {
        mPowerStarDemoModel->appear();
        MR::requestMovementOn(mPowerStarDemoModel);
        MR::startAction(mRabbit[2], "TakeOutStarTrickRabbit");
        MR::startAction(mPowerStarDemoModel, "TakeOutStarTrickRabbit");
    }

    if (MR::isActionEnd(mRabbit[2])) {
        MR::startAction(mRabbit[2], "Wait");
        setNerve(GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvAppearPowerStar));
    }
}

void TrickRabbitSnowCollect::exeAppearPowerStar() {
    if (MR::isFirstStep(this)) {
        mPowerStarDemoModel->kill();

        TVec3f jointPos;
        MR::copyJointPos(mPowerStarDemoModel, "PowerStarC", &jointPos);
        MR::appearPowerStarContinueCurrentDemo(this, jointPos);

        MR::startAfterBossBGM();
    }

    if (MR::isEndPowerStarAppearDemo(this)) {
        MR::endMultiActorCamera(this, mCameraInfo, "\x83\x45\x83\x54\x83\x4d\x82\xc6\x89\xef\x98\x62", 1, -1);
        MR::endDemo(this, "\x90\xe1\x83\x45\x83\x54\x83\x4d\x8f\x57\x82\xdf\x90\xac\x8c\xf7");
        setNerve(GET_NERVE(TrickRabbitFreeRunCollect, TrickRabbitSnowCollectNrvEnd));
    }
}

void TrickRabbitSnowCollect::exeEnd() {
}
