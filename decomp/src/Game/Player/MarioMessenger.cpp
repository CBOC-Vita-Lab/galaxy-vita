#include "Game/Player/MarioMessenger.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/ActorSensorUtil.hpp"
#include "Game/Util/ObjUtil.hpp"

#define MSG_SIZE 32

MarioMessenger::MarioMessenger(HitSensor* pSender) : NameObj("\x83\x7d\x83\x8a\x83\x49\x83\x81\x83\x62\x83\x5a\x83\x93\x83\x57\x83\x83\x81\x5b"), mSender(pSender) {
    mReceiverArray = new HitSensor*[MSG_SIZE];
    mMsgArray = new u32[MSG_SIZE];
    mArraySize = 0;

    MR::connectToScene(this, MR::MovementType_PlayerMessenger, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_None);
}

void MarioMessenger::movement() {
    for (int i = 0; i < mArraySize; i++) {
        MR::sendArbitraryMsg(mMsgArray[i], mReceiverArray[i], mSender);
    }

    mArraySize = 0;
}

void MarioMessenger::addRequest(HitSensor* pReceiver, u32 msg) {
    if (mArraySize == MSG_SIZE) {
        return;
    }

    mReceiverArray[mArraySize] = pReceiver;
    mMsgArray[mArraySize] = msg;
    mArraySize++;
}

MarioMessenger::~MarioMessenger() {
}
