#include "Game/Player/GhostPlayer.hpp"
#include "Game/Animation/XanimeCore.hpp"
#include "Game/Animation/XanimePlayer.hpp"
#include "Game/Camera/CameraTargetArg.hpp"
#include "Game/Camera/CameraTargetMtx.hpp"
#include "Game/LiveActor/HitSensor.hpp"
#include "Game/LiveActor/ModelManager.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Map/RaceManager.hpp"
#include "Game/NameObj/NameObjArchiveListCollector.hpp"
#include "Game/Player/GhostPacket.hpp"
#include "Game/Player/J3DModelX.hpp"
#include "Game/Player/JetTurtleShadow.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/ActorCameraUtil.hpp"
#include "Game/Util/ActorMovementUtil.hpp"
#include "Game/Util/ActorSensorUtil.hpp"
#include "Game/Util/ActorShadowUtil.hpp"
#include "Game/Util/ActorSwitchUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/EffectUtil.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/FixedPosition.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/JMapUtil.hpp"
#include "Game/Util/JointUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include "Game/Util/MtxUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/SequenceUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include "Game/Util/StringUtil.hpp"
#include <cstdio>

void GhostPlayer_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
}

namespace {
    struct AnimSoundInfo {
        /* 0x00 */ const char* pAnimName;
        /* 0x04 */ const char* pSoundBM;
        /* 0x08 */ const char* pSoundBV;
    };

    const AnimSoundInfo sAnimSoundTable[] = {{"\x83\x57\x83\x83\x83\x93\x83\x76", "SE_BM_GHOST_MARIO_JUMP_S", "SE_BV_GHOST_MARIO_JUMP_S"},
                                             {"\x83\x57\x83\x83\x83\x93\x83\x76""B", "SE_BM_GHOST_MARIO_JUMP_M", "SE_BV_GHOST_MARIO_JUMP_M"},
                                             {"\x83\x57\x83\x83\x83\x93\x83\x76""C", "SE_BM_GHOST_MARIO_JUMP_L", "SE_BV_GHOST_MARIO_JUMP_L"},
                                             {"\x95\xc7\x83\x57\x83\x83\x83\x93\x83\x76", "SE_BM_GHOST_MARIO_JUMP_S", "SE_BV_GHOST_MARIO_JUMP_S"},
                                             {"\x96\x84\x82\xdc\x82\xe8\x92\x45\x8f\x6f\x83\x57\x83\x83\x83\x93\x83\x76", "SE_BM_GHOST_MARIO_JUMP_S", "SE_BV_GHOST_MARIO_JUMP_S"},
                                             {"\x95\x9d\x82\xc6\x82\xd1", "SE_BM_GHOST_MARIO_JUMP_L", "SE_BV_GHOST_MARIO_JUMP_L"},
                                             {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76", "SE_BM_GHOST_MARIO_JUMP_L", "SE_BV_GHOST_MARIO_JUMP_L"},
                                             {"\x8a\x52\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x8a\x4a\x8e\x6e", nullptr, "SE_BV_GHOST_MARIO_HANG"},
                                             {"\x8a\x52\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x8f\x49\x97\xb9", nullptr, "SE_BV_GHOST_MARIO_CLIMB"},
                                             {"\x95\xc7\x89\x9f\x82\xb5", nullptr, "SE_BV_GHOST_MARIO_HANG"},
                                             {"\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x8a\x4a\x8e\x6e", "SE_BM_GHOST_MARIO_HIP_DROP_TURN", "SE_BV_GHOST_MARIO_HIPDROP_S"},
                                             {"\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x92\x85\x92\x6e", "SE_BM_GHOST_MARIO_HIP_DROP_LAND", "SE_BV_GHOST_MARIO_HIPDROP_E"},
                                             {"\x96\x84\x82\xdc\x82\xe8\x92\x45\x8f\x6f\x83\x57\x83\x83\x83\x93\x83\x76", "SE_BM_GHOST_MARIO_JUMP_S", "SE_BV_GHOST_MARIO_JUMP_S"},
                                             {"\x90\x85\x89\x6a\x83\x58\x83\x73\x83\x93", "SE_BM_GHOST_MARIO_SPIN", "SE_BV_GHOST_MARIO_SPIN"},
                                             {"\x90\x85\x89\x6a\x83\x57\x83\x46\x83\x62\x83\x67\x8a\x4a\x8e\x6e", nullptr, "SE_BV_GHOST_MARIO_SWM_ACCEL"},
                                             {"\x83\x4a\x83\x81\x8e\x9d\x82\xbf\x83\x8a\x83\x93\x83\x4f\x8f\x80\x94\xf5", nullptr, "SE_BV_GHOST_MARIO_TAKE"},
                                             {"\x83\x4a\x83\x81\x8e\x9d\x82\xbf\x83\x8a\x83\x93\x83\x4f", nullptr, "SE_BV_GHOST_MARIO_SWM_ACCEL"},
                                             {"\x93\x8a\x82\xb0", nullptr, "SE_BV_GHOST_MARIO_THROW"},
                                             {"\x8b\xf3\x92\x86\x82\xd0\x82\xcb\x82\xe8", "SE_BM_GHOST_MARIO_SPIN", "SE_BV_GHOST_MARIO_SPIN"},
                                             {"\x92\x6e\x8f\xe3\x82\xd0\x82\xcb\x82\xe8", "SE_BM_GHOST_MARIO_SPIN", "SE_BV_GHOST_MARIO_SPIN"},
                                             {"\x8b\xf3\x83\x70\x83\x93\x83\x60", "SE_BM_GHOST_MARIO_SPIN", "SE_BV_GHOST_MARIO_SPIN"},
                                             {"\x83\x41\x83\x43\x83\x58\x82\xd0\x82\xcb\x82\xe8", "SE_BM_GHOST_MARIO_SPIN", "SE_BV_GHOST_MARIO_SPIN"},
                                             {"\x83\x41\x83\x43\x83\x58\x82\xd0\x82\xcb\x82\xe8\x88\xda\x93\xae", "SE_BM_GHOST_MARIO_SPIN", "SE_BV_GHOST_MARIO_SPIN"},
                                             {"\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x41\x83\x4e\x83\x5a\x83\x8b\x83\x57\x83\x83\x83\x93\x83\x76", "SE_BM_GHOST_MARIO_JUMP_L", "SE_BV_GHOST_MARIO_JUMP_L"},
                                             {"\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76""2", "SE_BM_GHOST_MARIO_JUMP_L", "SE_BV_GHOST_MARIO_JUMP_L"},
                                             {"\x95\x58\x8f\xe3\x97\xcd\x8d\x73\x89\x45", nullptr, "SE_BV_GHOST_MARIO_JUMP_S"},
                                             {"\x95\x58\x8f\xe3\x97\xcd\x8d\x73\x8d\xb6", nullptr, "SE_BV_GHOST_MARIO_JUMP_S"},
                                             {"\x83\x56\x83\x87\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76", "SE_BM_GHOST_MARIO_JUMP_S", "SE_BV_GHOST_MARIO_JUMP_S"}};

    static const AnimSoundInfo* getAnimSoundInfo(const char* pAnimName) {
        for (u32 i = 0; i < ARRAY_SIZE(sAnimSoundTable); i++) {
            if (MR::isEqualString(pAnimName, sAnimSoundTable[i].pAnimName)) {
                return &sAnimSoundTable[i];
            }
        }
        return nullptr;
    }

    void playSound(LiveActor* pActor, const char* pAnimName) {
        const AnimSoundInfo* matchedEntry = getAnimSoundInfo(pAnimName);
        if (matchedEntry == nullptr) {
            return;
        }

        if (matchedEntry->pSoundBM != nullptr) {
            MR::startSound(pActor, matchedEntry->pSoundBM);
        }
        if (matchedEntry->pSoundBV != nullptr) {
            MR::startSound(pActor, matchedEntry->pSoundBV);
        }
    }
};  // namespace

namespace NrvGhostPlayer {
    NEW_NERVE(HostTypeNrvWait, GhostPlayer, Wait);
    NEW_NERVE(HostTypeNrvWinDemo, GhostPlayer, WinDemo);
    NEW_NERVE(HostTypeNrvLostDemo, GhostPlayer, LostDemo);
    NEW_NERVE(HostTypeNrvPreStartDemo0, GhostPlayer, PreStartDemo0);
    NEW_NERVE(HostTypeNrvPreStartDemo1, GhostPlayer, PreStartDemo1);
    NEW_NERVE(HostTypeNrvPreStartDemo2, GhostPlayer, PreStartDemo2);
    NEW_NERVE(HostTypeNrvStartDemo, GhostPlayer, Wait);
    NEW_NERVE(HostTypeNrvRun, GhostPlayer, Wait);
};  // namespace NrvGhostPlayer

void GhostPlayer::makeArchiveList(NameObjArchiveListCollector* pCollector, const JMapInfoIter& rIter) {
    char archiveName[256];
    strcpy(archiveName, "GhostData");
    strcat(archiveName, MR::getCurrentStageName());

    pCollector->addArchive(archiveName);

    if (MR::isPlayerLuigi()) {
        pCollector->addArchive("GhostLuigi");
    } else {
        pCollector->addArchive("GhostMario");
    }
}

GhostPlayer::GhostPlayer(const char* pName) : LiveActor(pName), _90(), mCameraInfo(), mXanimePlayer() {
    _8C = 0;
    mTargetRotation.zero();
    mKilledByStar = false;
    mPowerStarTarget = nullptr;
    mHasJetTurtle = false;
}

void GhostPlayer::init(const JMapInfoIter& rIter) {
    char gstPath[256];
    char arcPath[256];

    mRaceManagerLayout = new RaceManagerLayout("\x83\x8c\x81\x5b\x83\x58\x8a\xc7\x97\x9d\x97\x70\x83\x8c\x83\x43\x83\x41\x83\x45\x83\x67");
    mRaceManagerLayout->init(rIter);

    mCurrentPacket = nullptr;
    if (MR::isPlayerLuigi()) {
        const char* currentStageName = MR::getCurrentStageName();
        sprintf(gstPath, "%sLuigi.gst", currentStageName);
    } else {
        const char* currentStageName = MR::getCurrentStageName();
        sprintf(gstPath, "%s.gst", currentStageName);
    }
    strcpy(arcPath, "GhostData");
    strcat(arcPath, MR::getCurrentStageName());
    strcat(arcPath, ".arc");

    mCurrentPacket = static_cast< GhostPacket* >(MR::loadResourceFromArc(arcPath, gstPath));

    if (MR::isFileExist(gstPath, false)) {
        mCurrentPacket = static_cast< GhostPacket* >(MR::loadToMainRAM(gstPath, nullptr, nullptr, JKRDvdRipper::ALLOC_DIRECTION_FORWARD));
    }

    if (mCurrentPacket == nullptr) {
        makeActorDead();
        return;
    }

    if (MR::isPlayerLuigi()) {
        initModelManagerWithAnm("GhostLuigi", nullptr, false);
    } else {
        initModelManagerWithAnm("GhostMario", nullptr, false);
    }

    initEffectKeeper(5, "GhostMario", false);
    MR::connectToScene(this, MR::MovementType_Player, MR::CalcAnimType_Player, MR::DrawBufferType_None, MR::DrawType_Player);
    MR::initDefaultPos(this, rIter);
    mStartPos = mPosition;
    mVelocity.zero();
    initAnimation();
    initHitSensor(1);
    MR::addHitSensorRide(this, "body", 4, 40.0f, TVec3f(0.0f, 80.0f, 0.0f));
    MR::initShadowVolumeSphere(this, 50.0f);
    MR::validateShadow(this, nullptr);
    MR::calcGravity(this);
    MR::onCalcShadow(this, nullptr);
    MR::onCalcShadowDropGravity(this, nullptr);
    initSound(6, false);
    MR::invalidateClipping(this);
    _90 = 0;
    mCameraInfo = nullptr;
    if (MR::isValidInfo(rIter)) {
        MR::initMultiActorCamera(this, rIter, &mCameraInfo, "\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e""1");
        MR::initMultiActorCamera(this, rIter, &mCameraInfo, "\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e""2");
        MR::initMultiActorCamera(this, rIter, &mCameraInfo, "\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e""3");
        MR::initMultiActorCamera(this, rIter, &mCameraInfo, "\x83\x8c\x81\x5b\x83\x58\x8f\x49\x97\xb9");
    }
    mCameraTargetMtx = new CameraTargetMtx("\x83\x4a\x83\x81\x83\x89\x83\x5e\x81\x5b\x83\x51\x83\x62\x83\x67\x83\x5f\x83\x7e\x81\x5b");
    initNerve(GET_NERVE(GhostPlayer, HostTypeNrvWait));
    MR::needStageSwitchReadAppear(this, rIter);
    MR::syncStageSwitchAppear(this);
    makeActorDead();
    mWaitingToStart = true;
    _112 = true;
    MR::getRotatedAxisZ(&mInitialDirection, mRotation);
    PSMTXCopy(getBaseMtx(), mTargetRotationMtx);
    mJetTurtleShadow = new JetTurtleShadow("\x83\x4a\x83\x81\x83\x56\x83\x83\x83\x68\x83\x45\x83\x82\x83\x66\x83\x8b");
    mJetTurtleShadow->initWithoutIter();
    mHandRPos = new FixedPosition(this, "HandR", TVec3f(15.59f, 42.5f, 42.93f), TVec3f(-17.05f, -0.7f, 113.55f));
    MR::declareStarPiece(this, 50);
    MR::declareCoin(this, 50);
    mAppearStarPieceCooldown = 0;
    mPlayerTrampleCooldown = 0;
}

void GhostPlayer::appear() {
    LiveActor::appear();
    setNerve(GET_NERVE(GhostPlayer, HostTypeNrvPreStartDemo0));
    mIsHidden = true;
    MR::invalidateShadow(this, nullptr);
    if (MR::isPlayerLuigi()) {
        MR::startBtk(this, "GhostLuigi");
        MR::startBrk(this, "GhostLuigi");
    } else {
        MR::startBtk(this, "GhostMario");
        MR::startBrk(this, "GhostMario");
    }
    MR::emitEffect(this, "Shadow");
}

void GhostPlayer::control() {
    if (mWaitingToStart) {
        return;
    }
    if (MR::isDead(this)) {
        return;
    }

    MR::startLevelSound(this, "SE_BM_LV_GHOST_MARIO_AMBIENT");

    if (isNerve(GET_NERVE(GhostPlayer, HostTypeNrvLostDemo))) {
        return;
    }
    if (isNerve(GET_NERVE(GhostPlayer, HostTypeNrvWinDemo))) {
        return;
    }

    if (strcmp("powerstarget", MR::getPlayerCurrentBckName()) == 0 && isNerve(GET_NERVE(GhostPlayer, HostTypeNrvRun))) {
        setNerve(GET_NERVE(GhostPlayer, HostTypeNrvLostDemo));
        mKilledByStar = true;
    } else if (mCurrentPacket != nullptr) {
        u32 packetDelayTimer = 0;
        while (true) {
            GhostPacket packet(mCurrentPacket, 0);
            mCurrentPacket = reinterpret_cast< GhostPacket* >(reinterpret_cast< char* >(mCurrentPacket) + receiveGhostPacket(&packet));
            if (_112) {
                if (mXanimePlayer->mWeights[3] > 0.9f) {
                    continue;
                }
                _112 = false;
                packetDelayTimer = 10;
                continue;
            }

            if (packetDelayTimer == 0) {
                break;
            }
            packetDelayTimer--;
        }

        if (mAppearStarPieceCooldown != 0) {
            mAppearStarPieceCooldown--;
        }
        if (mPlayerTrampleCooldown != 0) {
            mPlayerTrampleCooldown--;
        }
    }
}

void GhostPlayer::exeWait() {
    if (mRaceManagerLayout->isAllAnimStopped()) {
        mRaceManagerLayout->kill();
    }
}

void GhostPlayer::warpPosition(const char* pName) {
    MR::findNamePos(pName, getBaseMtx());
    PSMTXCopy(getBaseMtx(), mTargetRotationMtx);
    MR::extractMtxTrans(getBaseMtx(), &mPosition);
}

void GhostPlayer::exePreStartDemo0() {
    if (MR::isFirstStep(this)) {
        MR::startSound(this, "SE_BM_GHOST_MARIO_APPEAR");
        MR::startSound(this, "SE_BV_GHOST_MARIO_APPEAR");
        MR::offPlayerControl();
        TPos3f* cameraTargetMatrix = &mCameraTargetMtx->mMatrix;
        cameraTargetMatrix->set(getBaseMtx());

        MR::startMultiActorCameraTargetOther(this, mCameraInfo, "\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e""1", CameraTargetArg(mCameraTargetMtx), -1);
        warpPosition("\x83\x53\x81\x5b\x83\x58\x83\x67\x83\x66\x83\x82\x83\x53\x81\x5b\x83\x58\x83\x67\x88\xca\x92\x75");
    }

    if (getNerveStep() == 1) {
        Mtx playerPosMtx;
        TVec3f playerPosVec;

        MR::findNamePos("\x83\x53\x81\x5b\x83\x58\x83\x67\x83\x66\x83\x82\x83\x7d\x83\x8a\x83\x49\x88\xca\x92\x75", playerPosMtx);
        MR::extractMtxTrans(playerPosMtx, &playerPosVec);
        MR::setPlayerPosAndWait(playerPosVec);
        MR::setPlayerBaseMtx(playerPosMtx);
        MR::startBckPlayerJ("\x83\x8c\x81\x5b\x83\x58\x8c\xa9\x82\xe9");
        MR::resetPlayerEffect();
        MR::tryStartDemo(this, "\x83\x8c\x81\x5b\x83\x58\x8f\x80\x94\xf5");
        MR::requestMovementOn(this);
        MR::requestMovementOnPlayer();
    }
    if (getNerveStep() == 60) {
        mIsHidden = false;
        setAnimation("\x83\x53\x81\x5b\x83\x58\x83\x67\x8f\x6f\x8c\xbb");
        MR::validateShadow(this, nullptr);
    }
    if (getNerveStep() == 107) {
        MR::startSound(this, "SE_BV_GHOST_MARIO_LAND");
    }
    if (getNerveStep() == 111) {
        MR::startSound(this, "SE_BM_GHOST_MARIO_LAND");
    }
    if (getNerveStep() == 146) {
        MR::startSound(this, "SE_BV_GHOST_MARIO_PROVOKE");
    }
    if (getNerveStep() == 240) {
        MR::endMultiActorCamera(this, mCameraInfo, "\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e""1", false, -1);
        TPos3f* cameraTargetMatrix = &mCameraTargetMtx->mMatrix;
        cameraTargetMatrix->set(getBaseMtx());
        MR::startMultiActorCameraTargetOther(this, mCameraInfo, "\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e""2", CameraTargetArg(mCameraTargetMtx), -1);
        setNerve(GET_NERVE(GhostPlayer, HostTypeNrvPreStartDemo1));
        setAnimation("\x83\x8c\x81\x5b\x83\x58\x8c\xa9\x82\xe9");
    } else if (isRequestSkipDemo()) {
        MR::endMultiActorCamera(this, mCameraInfo, "\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e""1", false, -1);
        MR::startMultiActorCameraTargetSelf(this, mCameraInfo, "\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e""3", -1);
        setNerve(GET_NERVE(GhostPlayer, HostTypeNrvPreStartDemo2));
        if (getNerveStep() < 60) {
            mIsHidden = false;
            setAnimation("\x83\x53\x81\x5b\x83\x58\x83\x67\x8f\x6f\x8c\xbb");
            MR::validateShadow(this, nullptr);
        }
    }
}

bool GhostPlayer::isRequestSkipDemo() const {
    if (MR::hasRetryGalaxySequence() && MR::testCorePadTriggerA(WPAD_CHAN0)) {
        return true;
    }
    return false;
}

void GhostPlayer::exePreStartDemo1() {
    if (getNerveStep() == 240) {
        MR::endMultiActorCamera(this, mCameraInfo, "\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e""2", false, -1);
        MR::startMultiActorCameraTargetSelf(this, mCameraInfo, "\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e""3", -1);
        setNerve(GET_NERVE(GhostPlayer, HostTypeNrvPreStartDemo2));
    } else if (isRequestSkipDemo()) {
        MR::endMultiActorCamera(this, mCameraInfo, "\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e""2", false, -1);
        MR::startMultiActorCameraTargetSelf(this, mCameraInfo, "\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e""3", -1);
        setNerve(GET_NERVE(GhostPlayer, HostTypeNrvPreStartDemo2));
    }
}

void GhostPlayer::exePreStartDemo2() {
    if (MR::isFirstStep(this)) {
        mRaceManagerLayout->setTime(0);
        mRaceManagerLayout->appear();
        mRaceManagerLayout->playCountAndGo();
        mRaceManagerLayout->hideRecordPane();
        mRaceManagerLayout->hideBestRecordPane();
        setAnimation("\x83\x53\x81\x5b\x83\x58\x83\x67\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e");
        MR::startBckPlayerJ("\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e");

        Mtx raceStartMarioPos;
        MR::findNamePos("\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e\x8e\x9e\x83\x7d\x83\x8a\x83\x49\x88\xca\x92\x75", raceStartMarioPos);
        MR::setPlayerBaseMtx(raceStartMarioPos);

        mPosition = mStartPos;
        MR::endDemo(this, "\x83\x8c\x81\x5b\x83\x58\x8f\x80\x94\xf5");
    }

    if (MR::getPlayerTriggerZ()) {
        MR::startBckPlayerJ("\x83\x8c\x81\x5b\x83\x58\x83\x4e\x83\x89\x83\x45\x83\x60\x83\x93\x83\x4f\x8a\x4a\x8e\x6e");
        MR::startSoundPlayer("SE_PV_SQUAT", -1);
    } else if (MR::testSubPadReleaseZ(WPAD_CHAN0)) {
        MR::startBckPlayerJ("\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e");
    }

    if (getNerveStep() % 60 == 0) {
        MR::startSystemSE("SE_SY_RACE_COUNT_DOWN");
    }

    TVec3f normalizedFrontVec = mInitialDirection;
    TVec3f upVec;
    TVec3f frontVec;

    bool isNormalized = MR::normalizeOrZero(&normalizedFrontVec);
    TPos3f* baseMtx = reinterpret_cast< TPos3f* >(getBaseMtx());
    baseMtx->getYDir(upVec);
    if (!isNormalized) {
        MR::calcFrontVec(&frontVec, this);
        if (MR::vecBlendSphere(frontVec, normalizedFrontVec, &frontVec, 0.1f) == false) {
            frontVec = normalizedFrontVec;
        }
        MR::makeMtxUpFront(reinterpret_cast< TPos3f* >(getBaseMtx()), upVec, frontVec);
        PSMTXCopy(getBaseMtx(), mTargetRotationMtx);
        MR::extractMtxTrans(getBaseMtx(), &mPosition);
    }
    if (!mRaceManagerLayout->isPlayCountAnim()) {
        mWaitingToStart = false;
        MR::startSound(this, "SE_BV_GHOST_MARIO_RUN_START");
        setNerve(GET_NERVE(GhostPlayer, HostTypeNrvRun));
        MR::onPlayerControl(true);
        MR::noticePlayerDashChance();
        MR::startBckPlayerJ("\x8a\xee\x96\x7b");
        MR::startSystemSE("SE_SY_RACE_START");
        MR::endMultiActorCamera(this, mCameraInfo, "\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e""3", false, -1);
    }
}

void GhostPlayer::exeWinDemo() {
    if (MR::isFirstStep(this)) {
        MR::offPlayerControl();
        MR::tryPlayerKillTakingActor();
        MR::readyPlayerDemo();

        TVec3f playerPosVec;
        Mtx playerPosMtx;
        MR::findNamePos("\x95\x89\x82\xaf\x8e\x9e\x83\x7d\x83\x8a\x83\x49\x88\xca\x92\x75", playerPosMtx);
        MR::extractMtxTrans(playerPosMtx, &playerPosVec);
        MR::setPlayerPosAndWait(playerPosVec);
        MR::setPlayerBaseMtx(playerPosMtx);

        MR::startBckPlayerJ("\x83\x8c\x81\x5b\x83\x58\x8c\xa9\x82\xe9");
        MR::startMultiActorCameraTargetSelf(this, mCameraInfo, "\x83\x8c\x81\x5b\x83\x58\x8f\x49\x97\xb9", -1);
        mIsDemoCameraActive = true;

        HitSensor* sensorBody = getSensor("body");
        for (int i = 0; i < sensorBody->mSensorCount; i++) {
            HitSensor* sensor = sensorBody->mSensors[i];
            if (sensor->mType == ATYPE_POWER_STAR_BIND) {
                mPowerStarTarget = sensor->mHost;
                mPosition = sensor->mHost->mPosition;
            }
        }

        MR::startSubBGM("BGM_RACE_LOSE", false);
        mRaceManagerLayout->appear();
        mRaceManagerLayout->playLose();
        setAnimation("\x83\x53\x81\x5b\x83\x58\x83\x67\x8f\x9f\x97\x98");
        MR::startSound(this, "SE_BV_GHOST_MARIO_WIN");
        MR::tryStartDemo(this, "\x83\x8c\x81\x5b\x83\x58\x8f\x49\x97\xb9");
        MR::requestMovementOn(this);
        MR::requestMovementOnPlayer();
        if (mPowerStarTarget != nullptr) {
            MR::requestMovementOn(mPowerStarTarget);
        }
    }

    if (mPowerStarTarget != nullptr) {
        PSMTXCopy(mPowerStarTarget->getBaseMtx(), getBaseMtx());
        PSMTXCopy(getBaseMtx(), mTargetRotationMtx);
        MR::extractMtxTrans(getBaseMtx(), &mPosition);
    }

    if (MR::isStep(this, 180)) {
        if (mIsDemoCameraActive) {
            MR::endMultiActorCamera(this, mCameraInfo, "\x83\x8c\x81\x5b\x83\x58\x8f\x49\x97\xb9", true, -1);
            mIsDemoCameraActive = false;
        }
        MR::endDemo(this, "\x83\x8c\x81\x5b\x83\x58\x8f\x49\x97\xb9");
        MR::forceKillPlayerByGhostRace();
    }
}

void GhostPlayer::exeLostDemo() {
    if (MR::isFirstStep(this)) {
        warpPosition("\x95\x89\x82\xaf\x8e\x9e\x83\x7d\x83\x8a\x83\x49\x88\xca\x92\x75");
        mXanimePlayer->changeAnimation("DieSwimEvent");
        mVelocity.zero();
        setAnimation("\x83\x8c\x81\x5b\x83\x58\x95\x89\x82\xaf");
    }
}

void GhostPlayer::calcAndSetBaseMtx() {
    if (MR::isDead(this)) {
        return;
    }

    MtxPtr baseMtx = MR::getJ3DModel(this)->getBaseTRMtx();
    MR::blendMtxRotate(getBaseMtx(), mTargetRotationMtx, 0.2f, baseMtx);
    MR::setMtxTrans(baseMtx, mPosition.x, mPosition.y, mPosition.z);
    MR::getJ3DModel(this)->setBaseScale(mScale);

    if (mHasJetTurtle) {
        mHandRPos->calc();
        mJetTurtleShadow->calcType0(mHandRPos->mMtx);
    }
}

void GhostPlayer::initAnimation() {
    XanimeResourceTable* pResource = MR::getPlayerXanimeResource();

    mAnimTrackWeights[0] = 0.0f;
    mAnimTrackWeights[1] = 0.0f;
    mAnimTrackWeights[2] = 0.0f;
    mAnimTrackWeights[3] = 1.0f;

    mXanimePlayer = new XanimePlayer(MR::getJ3DModel(this), pResource);
    mXanimePlayer->duplicateSimpleGroup();
    mModelManager->mXanimePlayer = mXanimePlayer;
    mXanimePlayer->setDefaultAnimation("\x8a\xee\x96\x7b");
    mXanimePlayer->getCore()->enableJointTransform(MR::getJ3DModelData(this));
    mXanimePlayerUpper = new XanimePlayer(MR::getJ3DModel(this), pResource, mXanimePlayer);
    mXanimePlayerUpper->changeAnimation("\x8a\xee\x96\x7b");
}

void GhostPlayer::setAnimation(const char* pName) {
    mXanimePlayer->changeAnimation(pName);
    if (mXanimePlayer->_20->mAttribute == 0) {
        mXanimePlayer->_20->mAttribute = 1;
    }
}

void GhostPlayer::setAnimationWeight(const f32* pWeights) {
    mXanimePlayer->changeTrackWeight(0, pWeights[0]);
    mXanimePlayer->changeTrackWeight(1, pWeights[1]);
    mXanimePlayer->changeTrackWeight(2, pWeights[2]);
    mXanimePlayer->changeTrackWeight(3, pWeights[3]);
}

void GhostPlayer::attackSensor(HitSensor* pSender, HitSensor* pReceiver) {
    if (pReceiver->isType(ATYPE_POWER_STAR_BIND)) {
        HitSensor* currentRushSensor = MR::getCurrentRushSensor();
        if ((currentRushSensor == nullptr || !currentRushSensor->isType(ATYPE_POWER_STAR_BIND)) && (!MR::isPlayerConfrontDeath() && !mKilledByStar)) {
            if (strcmp("powerstarget", MR::getPlayerCurrentBckName()) != 0) {
                setNerve(GET_NERVE(GhostPlayer, HostTypeNrvWinDemo));
                mKilledByStar = true;
                MR::preventPlayerRush();
            }
        }
    } else {
        if (isNerve(GET_NERVE(GhostPlayer, HostTypeNrvRun)) && MR::sendMsgPush(pReceiver, pSender) && mAppearStarPieceCooldown == 0) {
            if (MR::appearStarPiece(this, mPosition, 1, 10.0f, 40.0f, false)) {
                if (MR::isInWater(this, TVec3f(0.0f, 0.0f, 0.0f))) {
                    MR::startSound(this, "SE_OJ_STAR_PIECE_BURST_W");
                } else {
                    MR::startSound(this, "SE_OJ_STAR_PIECE_BURST");
                }
            }
            mAppearStarPieceCooldown = 5;
        }
        if (LiveActor::isNerve(GET_NERVE(GhostPlayer, HostTypeNrvWait))) {
            return;
        }
    }
}

bool GhostPlayer::receiveMsgPlayerAttack(u32 msg, HitSensor* pSender, HitSensor* pReceiver) {
    if (MR::isMsgJetTurtleAttack(msg)) {
        return true;
    }
    if (MR::isMsgPlayerTrample(msg) && mPlayerTrampleCooldown == 0) {
        TVec3f frontVec;
        TVec3f upVec;
        MR::calcUpVec(&upVec, this);
        MR::calcFrontVec(&frontVec, this);

        if (MR::appearStarPiece(this, mPosition, 5, 10.0f, 40.0f, false)) {
            if (MR::isInWater(this, TVec3f(0.0f, 0.0f, 0.0f))) {
                MR::startSound(this, "SE_OJ_STAR_PIECE_BURST_W");
            } else {
                MR::startSound(this, "SE_OJ_STAR_PIECE_BURST");
            }
        }

        mPlayerTrampleCooldown = 10;
        return true;
    }

    return false;
}

bool GhostPlayer::receiveOtherMsg(u32 msg, HitSensor* pSender, HitSensor* pReceiver) {
    return true;
}

void GhostPlayer::draw() const {
    if (MR::isDead(this)) {
        return;
    }
    if (mIsHidden) {
        return;
    }

    J3DModelX* model = reinterpret_cast< J3DModelX* >(MR::getJ3DModel(this));
    model->viewCalc2();
    GXInvalidateVtxCache();
    model->mFlags.clear();
    model->directDraw(nullptr);
    if (mHasJetTurtle) {
        mJetTurtleShadow->drawType1();
    }

    GXSetAlphaUpdate(GX_FALSE);
    GXSetColorUpdate(GX_TRUE);
    GXSetDstAlpha(GX_FALSE, 0);
}

u32 GhostPlayer::receiveGhostPacket(GhostPacket* pPacket) {
    u8 frameIndex;
    u8 bytesRead;
    u16 updateFlags;

    pPacket->read((s8*)&frameIndex);
    pPacket->read((s8*)&bytesRead);
    pPacket->read((s16*)&updateFlags);

    GhostPacket nextPacket = GhostPacket(pPacket->mDataPtr + bytesRead, 1);
    u8 nextFrameIndex;
    nextPacket.read((s8*)&nextFrameIndex);

    if (++frameIndex != nextFrameIndex) {
        if (mIsDemoCameraActive && MR::isBckOneTimeAndStopped(this)) {
            MR::endActorCamera(this, mCameraInfo, false, -1);
            mIsDemoCameraActive = false;
        }
        return false;
    }

    pPacket->mCurOffs = 4;

    if ((updateFlags & 0x0001) != 0) {
        TVec3s rawPos;
        pPacket->read(&rawPos);
        MR::convToFloat(rawPos, -2, &mPosition);
    }

    if ((updateFlags & 0x0800) != 0) {
        TVec3Sc rawVel;
        pPacket->read(&rawVel);
        MR::convToFloat(rawVel, 0, &mVelocity);
    }

    if ((updateFlags & 0x0400) != 0) {
        TVec3Sc rawScale;
        pPacket->read(&rawScale);
        MR::convToFloat(rawScale, 3, &mScale);
    }

    if ((updateFlags & 0x0002) != 0) {
        s8 rawRotX;
        f32 rotX;
        pPacket->read(&rawRotX);
        MR::convToFloat(rawRotX, 7, &rotX);
        rotX *= 180.0f;
        rotX = MR::clamp(rotX, -180.0f, 180.0f);
        mTargetRotation.x = rotX;
    }

    if ((updateFlags & 0x0004) != 0) {
        s8 rawRotY;
        f32 rotY;
        pPacket->read(&rawRotY);
        MR::convToFloat(rawRotY, 7, &rotY);
        rotY *= 180.0f;
        rotY = MR::clamp(rotY, -180.0f, 180.0f);
        mTargetRotation.y = rotY;
    }

    if ((updateFlags & 0x0008) != 0) {
        s8 rawRotZ;
        f32 rotZ;
        pPacket->read(&rawRotZ);
        MR::convToFloat(rawRotZ, 7, &rotZ);
        rotZ *= 180.0f;
        rotZ = MR::clamp(rotZ, -180.0f, 180.0f);
        mTargetRotation.z = rotZ;
    }

    if ((updateFlags & 0x000E) != 0) {
        MR::makeMtxRotate(mTargetRotationMtx, mTargetRotation);
    }

    if ((updateFlags & 0x0010) != 0) {
        char* animName;
        pPacket->read(&animName);
        setAnimation(animName);
        ::playSound(this, animName);

        bool isSpecialAnim = false;
        if (strcmp(animName, "\x8a\xee\x96\x7b") == 0) {
            isSpecialAnim = true;
            setAnimationWeight(mAnimTrackWeights);
        } else if (strcmp(animName, "\x95\xc7\x89\x9f\x82\xb5") == 0) {
            isSpecialAnim = true;
        } else if (strcmp(animName, "\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x95\xe0\x82\xab") == 0) {
            isSpecialAnim = true;
        } else if (strcmp(animName, "\x82\xaa\x82\xf1\x82\xce\x82\xe8\x91\x96\x82\xe8") == 0) {
            isSpecialAnim = true;
        }

        if (!isSpecialAnim) {
            mXanimePlayer->_20->mAttribute = 1;
        }
    }

    if ((updateFlags & 0x2000) != 0) {
        u32 animHash;
        pPacket->read(&animHash);
        mXanimePlayer->changeAnimationByHash(animHash);

        bool isSpecialAnim = false;
        const char* currentAnimName = mXanimePlayer->getCurrentAnimationName();
        ::playSound(this, currentAnimName);

        if (strcmp(currentAnimName, "\x8a\xee\x96\x7b") == 0) {
            isSpecialAnim = true;
            setAnimationWeight(mAnimTrackWeights);
        }
        if (strcmp(currentAnimName, "\x95\xc7\x89\x9f\x82\xb5") == 0) {
            isSpecialAnim = true;
        }
        if (strcmp(currentAnimName, "\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x95\xe0\x82\xab") == 0) {
            isSpecialAnim = true;
        }
        if (strcmp(currentAnimName, "\x82\xaa\x82\xf1\x82\xce\x82\xe8\x91\x96\x82\xe8") == 0) {
            isSpecialAnim = true;
        }
        if (strcmp(currentAnimName, "\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x41\x83\x4e\x83\x5a\x83\x8b\x83\x57\x83\x83\x83\x93\x83\x76") == 0) {
            isSpecialAnim = true;
        }
        if (strcmp(currentAnimName, "\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76""2") == 0) {
            isSpecialAnim = true;
        }
        if (strcmp(currentAnimName, "\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76""3") == 0) {
            isSpecialAnim = true;
        }

        if (strstr(currentAnimName, "\x90\x85\x89\x6a\x83\x57\x83\x46\x83\x62\x83\x67") != nullptr) {
            mHasJetTurtle = true;
        } else if (strstr(currentAnimName, "\x83\x4a\x83\x81\x8e\x9d\x82\xbf") != nullptr) {
            mHasJetTurtle = true;
        } else if (strstr(currentAnimName, "\x93\x8a\x82\xb0") != nullptr) {
            mHasJetTurtle = false;
            mXanimePlayerUpper->stopAnimation();
            MR::getJ3DModelData(this)->getJointTree().getJointNodePointer(MR::getJointIndex(this, "Spine1"))->setMtxCalc(nullptr);
        }

        if (mHasJetTurtle && (strcmp(currentAnimName, "\x8a\xee\x96\x7b") != 0 || strcmp(currentAnimName, "\x83\x57\x83\x83\x83\x93\x83\x76") != 0)) {
            mXanimePlayerUpper->changeAnimation("\x82\xd0\x82\xeb\x82\xa2\x83\x45\x83\x47\x83\x43\x83\x67");
            mXanimePlayerUpper->overWriteMtxCalc(MR::getJointIndex(this, "PartsControl"));
        }

        if (!isSpecialAnim) {
            mXanimePlayer->_20->mAttribute = 1;
        }
    }

    if ((updateFlags & 0x0020) != 0) {
        s16 rawAnimFrame;
        f32 animFrame;
        pPacket->read(&rawAnimFrame);
        MR::convToFloat(rawAnimFrame, 5, &animFrame);
        MR::getBckCtrl(this)->setFrame(animFrame);
    }

    for (u32 i = 0; i < ARRAY_SIZE(mAnimTrackWeights); i++) {
        if ((updateFlags & (0x0040 << i)) == 0) {
            continue;
        }

        s8 rawTrackWeight;
        f32 trackWeight;
        pPacket->read(&rawTrackWeight);
        if ((u8)rawTrackWeight == 128) {
            rawTrackWeight = 127;
        }
        MR::convToFloat(rawTrackWeight, 7, &trackWeight);
        mXanimePlayer->changeTrackWeight(i, trackWeight);
        mAnimTrackWeights[i] = trackWeight;
    }

    if ((updateFlags & 0x1000) != 0) {
        s8 rawAnimRate;
        f32 animRate;
        pPacket->read(&rawAnimRate);
        MR::convToFloat(rawAnimRate, 3, &animRate);
        MR::setBckRate(this, animRate);
    }

    return bytesRead;
}

f32 getShiftRatio(s8 shiftValue) {
    if (shiftValue > 0) {
        return static_cast< f32 >(256 << shiftValue) / 256.0f;
    }
    return static_cast< f32 >(256 >> -shiftValue) / 256.0f;
}

namespace MR {
    void convToFloat(TVec3s& rInVec, s8 shiftValue, TVec3f* pOutVec) {
        f32 factor = 1.0f / getShiftRatio(shiftValue);
        pOutVec->x = static_cast< f32 >(rInVec.x) * factor;
        pOutVec->y = static_cast< f32 >(rInVec.y) * factor;
        pOutVec->z = static_cast< f32 >(rInVec.z) * factor;
    }

    void convToFloat(TVec3Sc& rInVec, s8 shiftValue, TVec3f* pOutVec) {
        f32 factor = 1.0f / getShiftRatio(shiftValue);
        pOutVec->x = static_cast< f32 >(rInVec.x) * factor;
        pOutVec->y = static_cast< f32 >(rInVec.y) * factor;
        pOutVec->z = static_cast< f32 >(rInVec.z) * factor;
    }

    void convToFloat(s16 inShort, s8 shiftValue, f32* pOutFloat) {
        f32 factor = 1.0f / getShiftRatio(shiftValue);
        *pOutFloat = static_cast< f32 >(inShort) * factor;
    }

    void convToFloat(s8 inS8, s8 shiftValue, f32* pOutFloat) {
        f32 factor = 1.0f / getShiftRatio(shiftValue);
        *pOutFloat = static_cast< f32 >(inS8) * factor;
    }
};  // namespace MR

// Unused; Unknown data structure
struct BallData {
    f32 value1;
    u32 value2;
};
BallData ballData[] = {{0.0f, 0x00000000},  {0.0f, 0x00000000}, {0.0f, 0x00000000}, {0.0f, 0xFFFFFFFF},  {8.0f, 0x0000FFC0},  {0.0f, 0x0000FFC0},
                       {10.0f, 0xFF00FFFF}, {8.0f, 0x0000FFC0}, {0.0f, 0x0000FFC0}, {10.0f, 0xFF00FFFF}, {10.0f, 0x000000FF}, {6.0f, 0x000000FF},
                       {15.0f, 0xFFFF40FF}, {0.0f, 0x00000000}, {0.0f, 0x00000000}, {0.0f, 0x00000000},  {0.0f, 0x00000000},  {0.0f, 0x00000000},
                       {0.0f, 0x00000000},  {0.0f, 0xFF0000C0}, {6.0f, 0xFF0000C0}, {6.0f, 0xFF0000C0},  {0.0f, 0xFF0000C0},  {10.0f, 0xFF0000FF},
                       {0.0f, 0xFF0000C0},  {6.0f, 0xFF0000C0}, {6.0f, 0xFF0000C0}, {0.0f, 0xFF0000C0},  {10.0f, 0xFF0000FF}, {0.0f, 0x00000000}};
