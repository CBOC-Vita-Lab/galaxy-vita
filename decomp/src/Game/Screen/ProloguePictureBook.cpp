#include "Game/Screen/ProloguePictureBook.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Screen/IconAButton.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DAnimation.hpp>

namespace {
    static s32 sBookPageInfo[] = {0, 350, 700, 1050, 1400, 1748, -1};
}  // namespace

namespace NrvProloguePictureBook {
    NEW_NERVE(ProloguePictureBookActive, ProloguePictureBook, Active);
    NEW_NERVE(ProloguePictureBookPlaying, ProloguePictureBook, Playing);
    NEW_NERVE(ProloguePictureBookKeyWait, ProloguePictureBook, KeyWait);
    NEW_NERVE(ProloguePictureBookEnd, ProloguePictureBook, End);
}  // namespace NrvProloguePictureBook

ProloguePictureBook::ProloguePictureBook() : LayoutActor("\x83\x76\x83\x8d\x83\x8d\x81\x5b\x83\x4f\x82\xcc\x8a\x47\x96\x7b", true), mIconAButton(), mPage() {
}

void ProloguePictureBook::init(const JMapInfoIter& rIter) {
    MR::connectToSceneLayout(this);
    initLayoutManager("PrologueDemo", 1);
    initNerve(GET_NERVE(ProloguePictureBook, ProloguePictureBookPlaying));

    mIconAButton = MR::createAndSetupIconAButton(this, true, false);

    kill();
}

#if defined(TARGET_PC)
#include "Game/Util/DrawUtil.hpp"
// The storybook's pages cover the whole screen, but the castle scene behind them is still
// drawn in full for the 50 seconds they are shown (30 000 primitives a frame: the most
// expensive scene of the opening, for nothing). The library's picture book, the galaxy map
// and the staff roll switch the 3D scene off while they are up; this one never did.
static bool sPortHid3D;
#endif

void ProloguePictureBook::appear() {
#if defined(TARGET_PC)
    if (port_handheld() && !sPortHid3D) {
        MR::deactivateGameSceneDraw3D();
        sPortHid3D = true;
    }
#endif
    LayoutActor::appear();
    MR::requestMovementOn(mIconAButton);
    MR::startAnim(this, "Prologue", 0);
    setNerve(GET_NERVE(ProloguePictureBook, ProloguePictureBookPlaying));
}

void ProloguePictureBook::kill() {
#if defined(TARGET_PC)
    if (sPortHid3D) {
        MR::activateGameSceneDraw3D();
        sPortHid3D = false;
    }
#endif
    LayoutActor::kill();
    mIconAButton->kill();
}

void ProloguePictureBook::exeActive() {
    J3DFrameCtrl* animCtrl = MR::getAnimCtrl(this, 0);

    if (MR::isAnimStopped(this, 0)) {
        setNerve(GET_NERVE(ProloguePictureBook, ProloguePictureBookEnd));
    }
}

void ProloguePictureBook::exePlaying() {
    J3DFrameCtrl* animCtrl = MR::getAnimCtrl(this, 0);

    if (MR::isAnimStopped(this, 0)) {
        setNerve(GET_NERVE(ProloguePictureBook, ProloguePictureBookEnd));
    } else {
        int index = mPage + 1;

        if (::sBookPageInfo[index] < 0) {
            return;
        }

        MR::testSystemPadTriggerDecide();

        if (::sBookPageInfo[index] > static_cast< s32 >(animCtrl->getFrame())) {
            return;
        }

        animCtrl->setFrame(::sBookPageInfo[index] - 1.0f);

        setNerve(GET_NERVE(ProloguePictureBook, ProloguePictureBookKeyWait));
    }
}

void ProloguePictureBook::exeKeyWait() {
    J3DFrameCtrl* animCtrl = MR::getAnimCtrl(this, 0);

    if (MR::isFirstStep(this)) {
        mIconAButton->openWithoutMessage();
        animCtrl->setRate(0.0f);
    }

    if (MR::testCorePadTriggerA(WPAD_CHAN0)) {
        MR::startSystemSE("SE_SY_TALK_FOCUS_ITEM");
        mIconAButton->term();

        mPage++;
        animCtrl->setRate(1.0f);

        setNerve(GET_NERVE(ProloguePictureBook, ProloguePictureBookPlaying));
    }
}

void ProloguePictureBook::exeEnd() {
}

bool ProloguePictureBook::isEnd() const {
    return isNerve(GET_NERVE(ProloguePictureBook, ProloguePictureBookEnd));
}

void ProloguePictureBook::control() {
    MR::setLayoutScalePosAtPaneScaleTrans(mIconAButton, this, "AButtonPosition");
}
