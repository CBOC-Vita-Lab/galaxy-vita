#include "Game/Screen/WipeFade.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#ifdef TARGET_PC
#include "port/reduced_motion.h"
#endif

namespace {
    static const s32 sFadeFrame = 30;
};  // namespace

WipeFade::WipeFade(const char* pName, const Color8& rFillColor)
    : WipeLayoutBase(pName), mIsWipeIn(false), mFrame(::sFadeFrame), mStep(::sFadeFrame), mFillColor(rFillColor) {
}

void WipeFade::init(const JMapInfoIter& rIter) {
    kill();
}

#ifdef TARGET_PC
// (hitch33 5a, GXV_WIPE_OPEN_HOLD; platform/vgx/src/vgx_backend_gl.cpp) 1 while the first opening wipe after a stage load should stay fully closed: the stage's first pictures
// (hundreds of texture uploads, render targets, a long first turn) are made behind it instead of inside the moving fade.
extern "C" int port_wipe_open_hold(void) __attribute__((weak));
#endif

void WipeFade::control() {
#ifdef TARGET_PC
    if (mIsWipeIn && mStep == 0 && mFrame > 1 && port_wipe_open_hold && port_wipe_open_hold()) {
        return;  // (still at the first step: alpha 1, the screen is the fill colour; the fade starts when the port lets it)
    }
#endif
    mStep++;

    if (isOpen()) {
        kill();
    }
}

void WipeFade::draw() const {
    if (MR::isDead(this)) {
        return;
    }

    f32 rate = static_cast< f32 >(mStep) / mFrame;
    f32 alpha = MR::clamp(rate, 0.0f, 1.0f);

    if (mIsWipeIn) {
        alpha = 1.0f - alpha;
    }

#ifdef TARGET_PC
    // Reduced motion (GXV_REDUCED_MOTION): a short bright fade that is under way is a flash and is drawn at 40 % opacity at most.
    // Scene fades (30 frames or more), fades to black and a held (force-closed) screen are not touched; the wipe state is not changed.
    if (mStep < mFrame) {
        alpha = port::motion::CapFlashAlpha(alpha, mFrame, mFillColor.r, mFillColor.g, mFillColor.b);
    }
#endif

#ifdef TARGET_PC
    port_vr_wipe(0, alpha, (u32)mFillColor.r << 16 | (u32)mFillColor.g << 8 | mFillColor.b);
    if (port_vr_diorama()) {
        return;
    }
#endif
    GXSetColorUpdate(GX_TRUE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);

    GXColor fillColor;
    fillColor.r = mFillColor.r;
    fillColor.g = mFillColor.g;
    fillColor.b = mFillColor.b;
    fillColor.a = 255.0f * alpha;
    MR::fillScreen(fillColor);
}

void WipeFade::wipe(s32 frame) {
    mIsWipeIn = !mIsWipeIn;

    if (frame < 0) {
        mFrame = ::sFadeFrame;
    } else if (frame == 0) {
        mFrame = 1;
    } else {
        mFrame = frame;
    }

    mStep = 0;

    if (MR::isDead(this)) {
        appear();
    }
}

void WipeFade::forceClose() {
    mIsWipeIn = false;
    mStep = mFrame;

    if (MR::isDead(this)) {
        appear();
    }
}

void WipeFade::forceOpen() {
    mIsWipeIn = true;
    mStep = mFrame;

    kill();
}

bool WipeFade::isOpen() const {
    return mIsWipeIn && mStep >= mFrame;
}

bool WipeFade::isClose() const {
    return !mIsWipeIn && mStep >= mFrame;
}

bool WipeFade::isWipeIn() const {
    return mIsWipeIn && mStep < mFrame;
}

bool WipeFade::isWipeOut() const {
    return !mIsWipeIn && mStep < mFrame;
}
