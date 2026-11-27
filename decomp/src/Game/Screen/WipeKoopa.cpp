#include "Game/Screen/WipeKoopa.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DAnimation.hpp>
#ifdef TARGET_PC
#include "port/compat.h"
#endif

namespace {
    static const s32 sAnimFrame = 90;
};  // namespace

namespace {
    NEW_NERVE(WipeKoopaNrvWait, WipeKoopa, Wait);
    NEW_NERVE(WipeKoopaNrvWipeOut, WipeKoopa, WipeOut);
    NEW_NERVE(WipeKoopaNrvClose, WipeKoopa, Close);
};  // namespace

WipeKoopa::WipeKoopa() : WipeLayoutBase("\x83\x4e\x83\x62\x83\x70"), mFrame(::sAnimFrame) {
}

void WipeKoopa::init(const JMapInfoIter& rIter) {
    initNerve(GET_NERVE_ANON(WipeKoopaNrvWait));
    initLayoutManager("WipeKoopa", 1);
}

void WipeKoopa::exeWait() {
}

void WipeKoopa::exeWipeOut() {
    if (MR::isFirstStep(this)) {
        MR::startAnim(this, "out", 0);

        if (mFrame > 0) {
            MR::setAnimRate(this, static_cast< f32 >(MR::getAnimCtrl(this, 0)->getEnd()) / mFrame, 0);
        }

        MR::showLayout(this);
    }

    if (MR::isAnimStopped(this, 0)) {
        setNerve(GET_NERVE_ANON(WipeKoopaNrvClose));
    }
#ifdef TARGET_PC
    port_vr_wipe(0, 0.0f, 0x000000);   // (14 Nov: the Bowser wipe tells the port too; closing = not covered yet)
#endif
}

void WipeKoopa::exeClose() {
#ifdef TARGET_PC
    port_vr_wipe(0, 1.0f, 0x000000);   // (14 Nov, as WipeGameOver: closed = the screen is covered, no in-between pictures behind it)
#endif
}

void WipeKoopa::wipe(s32 frame) {
    setNerve(GET_NERVE_ANON(WipeKoopaNrvWipeOut));
    MR::hideLayout(this);

    if (frame > 0) {
        mFrame = frame;
    } else {
        mFrame = ::sAnimFrame;
    }
}

void WipeKoopa::forceClose() {
    setNerve(GET_NERVE_ANON(WipeKoopaNrvClose));

    MR::showLayout(this);
    
    MR::startAnim(this, "out", 0);
    MR::setAnimFrameAndStop(this, MR::getAnimCtrl(this, 0)->getEnd(), 0);
}

void WipeKoopa::forceOpen() {
    setNerve(GET_NERVE_ANON(WipeKoopaNrvWait));
    MR::hideLayout(this);
}

bool WipeKoopa::isOpen() const {
    return isNerve(GET_NERVE_ANON(WipeKoopaNrvWait));
}

bool WipeKoopa::isClose() const {
    return isNerve(GET_NERVE_ANON(WipeKoopaNrvClose));
}

bool WipeKoopa::isWipeIn() const {
    return false;
}

bool WipeKoopa::isWipeOut() const {
    return isNerve(GET_NERVE_ANON(WipeKoopaNrvWipeOut));
}
