#include "Game/Scene/StopSceneController.hpp"
#include "Game/NameObj/NameObjGroup.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/ObjUtil.hpp"

StopSceneDelayRequest::StopSceneDelayRequest() : NameObj("\x83\x56\x81\x5b\x83\x93\x8d\x64\x92\xbc\x92\x78\x89\x84\x94\xad\x8d\x73"), mFrame(), mDelay() {
    MR::connectToScene(this, MR::MovementType_StopSceneDelayRequest, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_None);
}

StopSceneController::StopSceneController() : NameObj("StopSceneController"), mDelayRequestArray(), mFrame() {
    mDelayRequestArray = new NameObjGroup("\x83\x56\x81\x5b\x83\x93\x8d\x64\x92\xbc\x92\x78\x89\x84\x94\xad\x8d\x73\x8e\xd2\x82\xcc\x8a\xc7\x97\x9d", 16);

    for (s32 i = 0; i < 16; i++) {
        StopSceneDelayRequest* delayRequest = new StopSceneDelayRequest();
        delayRequest->initWithoutIter();
        mDelayRequestArray->registerObj(delayRequest);
    }
}

void StopSceneDelayRequest::movement() {
    if (mDelay == 0) {
        return;
    }

    mDelay--;

    if (mDelay != 0) {
        return;
    }

    MR::stopScene(mFrame);
}

void StopSceneController::requestStopScene(s32 frame) {
    if (isSceneStopped()) {
        if (mFrame < frame) {
            mFrame = frame;
        }
    } else {
        mFrame = frame;
    }
}

void StopSceneController::requestStopSceneDelay(s32 frame, s32 delay) {
    NameObjGroup* pGroup = mDelayRequestArray;
    s32 count = pGroup->getObjNum();
    for (s32 i = 0; i < count; i++) {
        StopSceneDelayRequest* delayRequest = static_cast< StopSceneDelayRequest* >(pGroup->getObj(i));

        if (delayRequest->mDelay != 0) {
            continue;
        }

        delayRequest->mFrame = frame;
        delayRequest->mDelay = delay;
        return;
    }
}

void StopSceneController::movement() {
    if (mFrame > 0) {
        mFrame--;
    }
}

bool StopSceneController::isSceneStopped() const {
    return mFrame > 0;
}
