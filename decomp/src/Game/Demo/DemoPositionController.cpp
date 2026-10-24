#include "Game/Demo/DemoPositionController.hpp"
#include "Game/LiveActor/ActorCameraInfo.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/Util/ActorCameraUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"

DemoPositionController::DemoPositionController(const char* pName, const JMapInfoIter& rIter) : LiveActor("\x83\x66\x83\x82\x83\x41\x83\x4e\x83\x5e\x81\x5b\x88\xca\x92\x75"), pCameraInfo(nullptr) {
    MR::initDefaultPos(this, rIter);
    initModelManagerWithAnm(pName, nullptr, false);
    pCameraInfo = new ActorCameraInfo(rIter);
    MR::invalidateClipping(this);
    makeActorDead();
}

void DemoPositionController::control() {
    calcAnim();
}

void DemoPositionController::initAnimCamera(const char* pCameraName) {
    MR::initAnimCamera(this, this->pCameraInfo, pCameraName);
}

void DemoPositionController::startDemo(const char* pBckName) {
    appear();
    MR::startBck(this, pBckName);
    MR::startAnimCameraTargetSelf(this, pCameraInfo, pBckName, 0, 1.0f);
}

void DemoPositionController::endDemo(const char* pCameraName) {
    MR::endAnimCamera(this, pCameraInfo, pCameraName, -1, true);
    kill();
}
