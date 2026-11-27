#include "Game/Screen/CinemaFrame.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DAnimation.hpp>
#ifdef TARGET_PC
#include "port/xskip.h"
#endif

namespace NrvCinemaFrame {
    NEW_NERVE(CinemaFrameNrvScreen, CinemaFrame, Screen);
    NEW_NERVE(CinemaFrameNrvFrame, CinemaFrame, Frame);
    NEW_NERVE(CinemaFrameNrvBlank, CinemaFrame, Blank);
    NEW_NERVE(CinemaFrameNrvScreenToFrame, CinemaFrame, ScreenToFrame);
    NEW_NERVE(CinemaFrameNrvFrameToBlank, CinemaFrame, FrameToBlank);
    NEW_NERVE(CinemaFrameNrvBlankToFrame, CinemaFrame, BlankToFrame);
    NEW_NERVE(CinemaFrameNrvFrameToScreen, CinemaFrame, FrameToScreen);
};  // namespace NrvCinemaFrame

#ifdef TARGET_PC
// xskip27 (GXV_XSKIP): after an accepted skip the frame is fully black (the Blank nerve); a request to leave the black (Blank -> Frame, Blank -> Screen) waits while
// port_xskip_cover_hold says the stage's work is not done (at most GXV_XSKIP_HOLD_MS), and is made by exeBlank on the first turn it does not. One frame object a scene.
namespace {
    int sXsHeld = 0;           // 0 none, 1 Blank -> Frame asked, 2 Blank -> Screen asked
    bool sXsReleasing = false;  // (the release's own call is not held again)
    unsigned sXsBlankTurn = 0;  // the turn exeBlank last ran
}
// 1 while the frame has been black this turn or the turn before (GameScene::update asks, for the "xskip:" cover marker)
extern "C" int port_xskip_cinema_blank(void) {
    return sXsBlankTurn != 0 && port_xskip_turn_no() - sXsBlankTurn <= 1 ? 1 : 0;
}
#endif

CinemaFrame::CinemaFrame(bool isConnectToScene) : LayoutActor("\x83\x56\x83\x6c\x83\x7d\x83\x74\x83\x8c\x81\x5b\x83\x80", true) {
    if (isConnectToScene) {
        MR::connectToScene(this, MR::MovementType_Layout, MR::CalcAnimType_Layout, MR::DrawBufferType_None, MR::DrawType_CinemaFrame);
    }
}

void CinemaFrame::init(const JMapInfoIter& rIter) {
#ifdef TARGET_PC
    sXsHeld = 0;
    sXsReleasing = false;
    sXsBlankTurn = 0;
#endif
    initLayoutManager("CinemaFrame", 1);
    initNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvScreen));
    kill();
}

void CinemaFrame::appear() {
    LayoutActor::appear();
    setNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvScreenToFrame));
}

#if defined(TARGET_PC) && !defined(__vita__)
// In the VR diorama the letterbox bars would only frame the HUD panel in
// front of it (they stayed up through a galaxy's whole opening), so they are
// left out; the frame's black (closing to it and opening from it, around the
// fly-in) darkens the whole view instead, like the game's fades.  On the
// virtual screen the bars are left out too; the frame shows only while it
// closes to black or opens from it.
void CinemaFrame::draw() const {
    f32 black = getBlackRate();
    if (port_vr_diorama()) {
        if (black > 0.0f) {
            port_vr_wipe(0, black, 0x000000);
        }
        return;
    }
    if (black <= 0.0f) {
        return;
    }
    LayoutActor::draw();
}
#endif

#ifdef TARGET_PC
// How far the frame has closed to black: 1 blank, 0 bars or less.
f32 CinemaFrame::getBlackRate() const {
    if (MR::isDead(this)) {
        return 0.0f;
    }
    if (isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvBlank))) {
        return 1.0f;
    }
    bool closing = isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvFrameToBlank));
    bool opening = isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvBlankToFrame));
    if (!closing && !opening) {
        return 0.0f;
    }
    f32 end = MR::getAnimFrameMax(this, 0u);
    f32 rate = end > 0.0f ? MR::clamp(MR::getAnimFrame(this, 0u) / end, 0.0f, 1.0f) : 1.0f;
    return closing ? rate : 1.0f - rate;
}
#endif

void CinemaFrame::tryScreenToFrame() {
    if (MR::isDead(this)) {
        appear();
    }

    bool isInvalid = isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvFrame)) || isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvScreenToFrame)) ||
                     isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvBlankToFrame));

    if (!isInvalid) {
        setNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvScreenToFrame));
    }
}

void CinemaFrame::tryFrameToBlank() {
    if (MR::isDead(this)) {
        appear();
    }

    bool isInvalid = isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvBlank)) || isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvFrameToBlank));

    if (!isInvalid) {
        setNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvFrameToBlank));
    }
}

void CinemaFrame::tryBlankToFrame() {
#ifdef TARGET_PC
    if (!sXsReleasing && !MR::isDead(this) && isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvBlank)) && port_xskip_cover_hold()) {
        sXsHeld = 1;
        return;
    }
#endif
    if (MR::isDead(this)) {
        appear();
    }

    bool isInvalid = isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvFrame)) || isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvScreenToFrame)) ||
                     isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvBlankToFrame));

    if (!isInvalid) {
        setNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvBlankToFrame));
    }
}

void CinemaFrame::tryFrameToScreen() {
#ifdef TARGET_PC
    if (!sXsReleasing && !MR::isDead(this) && isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvBlank)) && port_xskip_cover_hold()) {
        sXsHeld = 2;
        return;
    }
#endif
    if (MR::isDead(this)) {
        appear();
    }

    bool isInvalid = isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvScreen)) || isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvFrameToScreen));

    if (!isInvalid) {
        setNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvFrameToScreen));
    }
}

void CinemaFrame::forceToScreen() {
    if (MR::isDead(this)) {
        appear();
    }

    setNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvScreen));
}

void CinemaFrame::forceToFrame() {
    if (MR::isDead(this)) {
        appear();
    }

    setNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvFrame));
}

void CinemaFrame::forceToBlank() {
    if (MR::isDead(this)) {
        appear();
    }

    setNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvBlank));
}

bool CinemaFrame::isStop() const {
    return isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvScreen)) || isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvFrame)) ||
           isNerve(GET_NERVE(CinemaFrame, CinemaFrameNrvBlank));
}

void CinemaFrame::exeScreen() {
    if (MR::isFirstStep(this)) {
        MR::startAnim(this, "End", 0);
        MR::setAnimFrameAndStop(this, MR::getAnimCtrl(this, 0)->getEnd() - 1.0f, 0);
        kill();
    }
}

void CinemaFrame::exeFrame() {
    MR::startAnimAtFirstStep(this, "Wait", 0);
}

void CinemaFrame::exeBlank() {
    if (MR::isFirstStep(this)) {
        MR::startAnim(this, "Open", 0);
        MR::setAnimFrameAndStop(this, 0.0f, 0);
    }
#ifdef TARGET_PC
    sXsBlankTurn = port_xskip_turn_no();
    if (sXsHeld != 0 && !port_xskip_cover_hold()) {  // (xskip27) the work is done or the cap is reached: the request that was held is made now
        const int held = sXsHeld;
        sXsHeld = 0;
        sXsReleasing = true;
        if (held == 1) {
            tryBlankToFrame();
        } else {
            tryFrameToScreen();
        }
        sXsReleasing = false;
    }
#endif
}

void CinemaFrame::exeScreenToFrame() {
    MR::startAnimAtFirstStep(this, "Appear", 0);
    MR::setNerveAtAnimStopped(this, GET_NERVE(CinemaFrame, CinemaFrameNrvFrame), 0);
}

void CinemaFrame::exeFrameToBlank() {
    MR::startAnimAtFirstStep(this, "Close", 0);
    MR::setNerveAtAnimStopped(this, GET_NERVE(CinemaFrame, CinemaFrameNrvBlank), 0);
}

void CinemaFrame::exeBlankToFrame() {
    MR::startAnimAtFirstStep(this, "Open", 0);
    MR::setNerveAtAnimStopped(this, GET_NERVE(CinemaFrame, CinemaFrameNrvFrame), 0);
}

void CinemaFrame::exeFrameToScreen() {
    MR::startAnimAtFirstStep(this, "End", 0);
    MR::setNerveAtAnimStopped(this, GET_NERVE(CinemaFrame, CinemaFrameNrvScreen), 0);
}
