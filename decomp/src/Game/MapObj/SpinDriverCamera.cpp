#include "Game/MapObj/SpinDriverCamera.hpp"
#include "Game/Camera/CameraTargetArg.hpp"
#include "Game/Camera/CameraTargetMtx.hpp"
#include "Game/LiveActor/ActorCameraInfo.hpp"
#include "Game/Util/ActorCameraUtil.hpp"
#include "Game/Util/JMapUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/MtxUtil.hpp"
#include "Game/Util/MultiEventCamera.hpp"

void SpinDriverCamera_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)2.0f;
}

SpinDriverCamera::SpinDriverCamera() : mCamera(), mTargetMtx(), mCameraInfo(), mAppearCameraFrame() {
}

void SpinDriverCamera::startAppearCamera(LiveActor* pActor, const TVec3f& a2, const TVec3f& a3, const TVec3f& a4) {
    if (mCameraInfo == nullptr) {
        return;
    }

    mAppearCameraFrame = MR::getMultiActorCameraFrames(pActor, mCameraInfo, "\x8f\x6f\x8c\xbb\x83\x43\x83\x78\x83\x93\x83\x67\x97\x70");

    if (mAppearCameraFrame <= 0) {
        return;
    }

    MR::startMultiActorCameraTargetOther(pActor, mCameraInfo, "\x8f\x6f\x8c\xbb\x83\x43\x83\x78\x83\x93\x83\x67\x97\x70", CameraTargetArg(mTargetMtx), -1);

    TPos3f upPos;
    MR::makeMtxUpFrontPos(&upPos, a2, a3, a4);
    mTargetMtx->setMtx(upPos.toMtxPtr());
}

void SpinDriverCamera::endAppearCamera(LiveActor* pActor) {
    if (mCameraInfo == nullptr) {
        return;
    }

    if (mAppearCameraFrame <= 0) {
        return;
    }

    MR::endMultiActorCamera(pActor, mCameraInfo, "\x8f\x6f\x8c\xbb\x83\x43\x83\x78\x83\x93\x83\x67\x97\x70", false, -1);
}

s32 SpinDriverCamera::getAppearCameraFrames() const {
    return mAppearCameraFrame;
}

void SpinDriverCamera::start(const TVec3f& a1, const TVec3f& a2, const TVec3f& a3) {
    if (mCamera == nullptr) {
        return;
    }

    mCamera->start(CameraTargetArg(mTargetMtx), 0);

    TPos3f mtx;
    MR::makeMtxUpFrontPos(&mtx, a1, a2, a3);
    mTargetMtx->setMtx(mtx.toMtxPtr());
}

void SpinDriverCamera::update(const TVec3f& a1, const TVec3f& a2) {
    if (mCamera == nullptr || mCamera->isEnd()) {
        return;
    }

    mCamera->update();
    updateTargetMatrix(a1, a2);
}

void SpinDriverCamera::cancel() {
    if (mCamera == nullptr) {
        return;
    }

    mCamera->endForceSoon();
}

void SpinDriverCamera::end() {
    if (mCamera == nullptr) {
        return;
    }

    mCamera->endForceAtLanding();
    mCamera->changeTargetPlayer();
}

void SpinDriverCamera::updateTargetMatrix(const TVec3f& rRot, const TVec3f& rTrans) {
    if (MR::isNearZero(rRot)) {
        return;
    }

    TPos3f targetMtx = mTargetMtx->mMatrix;

    TVec3f yDir;
    targetMtx.getYDir(yDir);

    TVec3f rotVec;
    MR::normalize(rRot, &rotVec);

    TPos3f rotMtx;
    rotMtx.identity();
    rotMtx.setRotate(yDir, rotVec);

    targetMtx.concat(rotMtx, targetMtx);
    targetMtx.setTrans(rTrans);

    mTargetMtx->setMtx(targetMtx);
}

bool SpinDriverCamera::isUseAppearCamera(LiveActor* pActor) const {
    if (mCameraInfo == nullptr) {
        return false;
    }

    return MR::getMultiActorCameraFrames(pActor, mCameraInfo, "\x8f\x6f\x8c\xbb\x83\x43\x83\x78\x83\x93\x83\x67\x97\x70") > 0;
}

void SpinDriverCamera::init(const JMapInfoIter& rIter, LiveActor* pActor) {
    ActorCameraInfo info(rIter);

    if (info.mCameraSetID == -1) {
        return;
    }

    s32 arg3 = 1;
    MR::getJMapInfoArg3NoInit(rIter, &arg3);

    mCamera = new MultiEventCamera();
    mCamera->setUp(pActor->mName, new ActorCameraInfo(rIter), arg3);
    mCamera->setEndCameraTypeAtLanding();

    mTargetMtx = new CameraTargetMtx("\x83\x4a\x83\x81\x83\x89\x83\x5e\x81\x5b\x83\x51\x83\x62\x83\x67\x83\x5f\x83\x7e\x81\x5b");
}

void SpinDriverCamera::initAppearCamera(const JMapInfoIter& rIter, LiveActor* pActor) {
    ActorCameraInfo info(rIter);

    if (info.mCameraSetID == -1) {
        return;
    }

    MR::initMultiActorCamera(pActor, rIter, &mCameraInfo, "\x8f\x6f\x8c\xbb\x83\x43\x83\x78\x83\x93\x83\x67\x97\x70");

    mAppearCameraFrame = MR::getMultiActorCameraFrames(pActor, mCameraInfo, "\x8f\x6f\x8c\xbb\x83\x43\x83\x78\x83\x93\x83\x67\x97\x70");
}
