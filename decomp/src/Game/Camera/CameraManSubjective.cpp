#include "Game/Camera/CameraManSubjective.hpp"
#include "Game/Camera/CameraLocalUtil.hpp"
#include "Game/Camera/CameraSubjective.hpp"

CameraManSubjective::CameraManSubjective(const char* pName) : CameraMan(pName) {
    mCamera = new CameraSubjective("\x8e\xe5\x8a\xcf\x83\x4a\x83\x81\x83\x89");
    mCamera->mCameraMan = this;
}

void CameraManSubjective::init(const JMapInfoIter& rIter) {
}

void CameraManSubjective::calc() {
    mCamera->calc();
    CameraLocalUtil::calcSafePose(this, mCamera);
}

void CameraManSubjective::notifyActivate() {
    mCamera->reset();
}

void CameraManSubjective::notifyDeactivate() {
}
