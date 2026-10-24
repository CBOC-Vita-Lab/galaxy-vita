#include "Game/MapObj/MapPartsFunction.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/LiveActor/Spine.hpp"

MapPartsFunction::MapPartsFunction(LiveActor* pHost, const char* pName)
    : NameObj(pName != nullptr ? pName : "\x83\x7d\x83\x62\x83\x76\x83\x70\x81\x5b\x83\x63\x8b\x40\x94\x5c"), mSpine(), mHost(pHost), mIsActive(true) {
}

bool MapPartsFunction::sendMsgToHost(u32 msg) {
    LiveActor* host = mHost;
    return host->receiveMessage(msg, host->getSensor("body"), host->getSensor("body"));
}

void MapPartsFunction::movement() {
    if (mIsActive) {
        mSpine->update();
        control();
    }
}

void MapPartsFunction::initNerve(const Nerve* pNerve) {
    mSpine = new Spine(this, pNerve);
}

void MapPartsFunction::setNerve(const Nerve* pNerve) {
    mSpine->setNerve(pNerve);
}

s32 MapPartsFunction::getStep() const {
    return mSpine->mStep;
}

bool MapPartsFunction::isStep(s32 step) const {
    return step == mSpine->mStep;
}

bool MapPartsFunction::isNerve(const Nerve* pNerve) const {
    return mSpine->getCurrentNerve() == pNerve;
}

bool MapPartsFunction::isFirstStep() const {
    return mSpine->mStep == 0;
}
