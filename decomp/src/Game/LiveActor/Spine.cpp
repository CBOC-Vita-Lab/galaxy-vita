#include "Game/LiveActor/Spine.hpp"
#include "Game/LiveActor/ActorStateKeeper.hpp"

Spine::Spine(void* pExecutor, const Nerve* pNerve) : mExecutor(pExecutor), mCurrNerve(pNerve), mNextNerve(nullptr), mStep(0), mStateKeeper(nullptr) {
}

void Spine::update() {
    changeNerve();
    mCurrNerve->execute(this);
    mStep++;
    changeNerve();
}

#ifdef TARGET_PC
extern "C" int port_trace_nerves;
int port_trace_nerves = 0;
#endif

void Spine::setNerve(const Nerve* pNerve) {
#ifdef TARGET_PC
    if (port_trace_nerves > 0) {
        port_trace_nerves--;
        port_log("nerve: exec %p -> %p", (void*)mExecutor, (const void*)pNerve);
    }
#endif
    if (mStep >= 0) {
        mCurrNerve->executeOnEnd(this);
    }

    mNextNerve = pNerve;
    mStep = -1;
}

const Nerve* Spine::getCurrentNerve() const {
    if (mNextNerve != nullptr) {
        return mNextNerve;
    }

    return mCurrNerve;
}

void Spine::changeNerve() {
    if (mNextNerve == nullptr) {
        return;
    }

    if (mStateKeeper != nullptr) {
        mStateKeeper->endState(mCurrNerve);
        mStateKeeper->startState(mNextNerve);
    }

    mCurrNerve = mNextNerve;
    mNextNerve = nullptr;
    mStep = 0;
}

void Spine::initStateKeeper(int capacity) {
    mStateKeeper = new ActorStateKeeper(capacity);
}
