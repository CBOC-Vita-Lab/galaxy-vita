#include "Game/Screen/PlayerLeft.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Screen/CountUpPaneRumbler.hpp"
#include "Game/Screen/CounterLayoutAppearer.hpp"
#include "Game/Screen/CounterLayoutController.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/ObjUtil.hpp"

#include "port/mp_bridge.h"  // MP3: the life counter follows the chosen character in a session
#include "Game/MP/MpTeam.hpp"  // MP3 celebrate: the party strip follows this counter's appear / hide
#include "Game/Screen/LayoutManager.hpp"  // MP3 ingame: the picture pane's place

namespace {
    // MP3: in a session the counter shows the chosen character's family (Luigi / Cosmic Luigi: Luigi's head, the others Mario's;
    // the layout has no other head); single player: the game's own rule
    bool counterShowsLuigi() {
        if (mp_active()) {
            const MpLook look = mp_slot_look(mp_local_slot());
            return look == MP_LOOK_LUIGI || look == MP_LOOK_COSMIC_LUIGI;
        }
        return MR::isPlayerLuigi();
    }

    // touches / ingame: Toad, Yellow Toad, Cosmic Mario and Cosmic Luigi have no picture in this layout (Mario and Luigi only): the
    // game's picture is hidden and the party strip draws the character's own head where it was (MpPartyLogic::counterPane)
    bool counterHeadFromStrip() {
        if (!mp_active()) return false;
        return MpTeam::counterOwnHead();
    }
}  // namespace


namespace NrvPlayerLeft {
    NEW_NERVE(PlayerLeftNrvHide, PlayerLeft, Hide);
    NEW_NERVE(PlayerLeftNrvAppear, PlayerLeft, Appear);
    NEW_NERVE(PlayerLeftNrvWait, PlayerLeft, Wait);
    NEW_NERVE(PlayerLeftNrvDisappear, PlayerLeft, Disappear);
};  // namespace NrvPlayerLeft

PlayerLeft::PlayerLeft(const char* pName)
    : LayoutActor(pName, true), mPlayerLeft(0), mLayoutAppearer(nullptr), mPaneRumbler(nullptr), mIsForceAppear(false) {
}

void PlayerLeft::init(const JMapInfoIter& rIter) {
    initLayoutManager("PlayerLeft", 3);
    MR::createAndAddPaneCtrl(this, "Counter", 1);
    MR::hidePaneRecursive(this, "CenterPlayerLeft");
    if (counterHeadFromStrip()) MR::hidePaneRecursive(this, "PicMario");   // touches: the strip draws the player's own head there

    mLayoutAppearer = new CounterLayoutAppearer(this, TVec2f(-50.0f, 0.0f));
    mPaneRumbler = new CountUpPaneRumbler(this, "Counter");
    mPlayerLeft = MR::getPlayerLeft();

    if (counterShowsLuigi()) {
        MR::startAnim(this, "Luigi", 2);
    }

    initNerve(GET_NERVE(PlayerLeft, PlayerLeftNrvHide));
    MR::connectToSceneLayout(this);
    appear();
}

void PlayerLeft::appear() {
    mLayoutAppearer->reset();
    mPaneRumbler->reset();

    mIsForceAppear = false;

    MR::hideLayout(this);
    setNerve(GET_NERVE(PlayerLeft, PlayerLeftNrvHide));
    LayoutActor::appear();
}

void PlayerLeft::forceAppear() {
    if (!isNerve(GET_NERVE(PlayerLeft, PlayerLeftNrvWait))) {
        appear();
        setNerve(GET_NERVE(PlayerLeft, PlayerLeftNrvAppear));
    }

    mIsForceAppear = true;
}

void PlayerLeft::disappear() {
    mIsForceAppear = false;

    setNerve(GET_NERVE(PlayerLeft, PlayerLeftNrvDisappear));
}

bool PlayerLeft::isWait() const {
    return isNerve(GET_NERVE(PlayerLeft, PlayerLeftNrvWait));
}

void PlayerLeft::control() {
    updateCounter();
    mLayoutAppearer->update();
    mPaneRumbler->update();

    // MP3 celebrate: the party strip slides in and out with this counter (shown = not in the Hide nerve)
    if (mp_active()) {
        MpTeam::noteHud(!isNerve(GET_NERVE(PlayerLeft, PlayerLeftNrvHide)));
        // ingame: exactly one head: the game's picture hidden whenever the character has its own (a change of character mid-scene
        // included), and its place and bounce handed to the strip, which draws the head there
        const bool own = counterHeadFromStrip();
        if (own != MR::isHiddenPane(this, "PicMario")) {
            if (own) MR::hidePaneRecursive(this, "PicMario");
            else MR::showPaneRecursive(this, "PicMario");
        }
        if (own) {
            nw4r::lyt::Pane* pic = getLayoutManager()->getPane("PicMario");
            if (pic != nullptr) {
                TVec2f at;
                MR::copyPaneTrans(&at, this, "PicMario");
                const f32 sx = pic->mGlbMtx._00, sy = pic->mGlbMtx._10;
                MpTeam::noteCounterPane(at.x, at.y, __builtin_sqrtf(sx * sx + sy * sy));
            }
        }
    }
}

void PlayerLeft::updateCounter() {
    MR::setTextBoxNumberRecursive(this, "Counter", mPlayerLeft);
}

bool PlayerLeft::isValidAppearSituation() const {
    return mPlayerLeft < MR::getPlayerLeft();
}

void PlayerLeft::exeHide() {
    if (MR::isFirstStep(this)) {
        MR::hideLayout(this);
    }

    if (isValidAppearSituation()) {
        setNerve(GET_NERVE(PlayerLeft, PlayerLeftNrvAppear));
    }
}

void PlayerLeft::exeAppear() {
    if (MR::isFirstStep(this)) {
        MR::showLayout(this);
        mLayoutAppearer->appear(TVec2f(0.0f, 0.0f));
        MR::startAnim(this, "Wait", 1);
    }

    if (mLayoutAppearer->isAppeared()) {
        setNerve(GET_NERVE(PlayerLeft, PlayerLeftNrvWait));
    }
}

void PlayerLeft::exeWait() {
    if (MR::isFirstStep(this)) {
        if (isValidAppearSituation()) {
            mPlayerLeft++;
            mPaneRumbler->start();
            MR::startAnim(this, "Flash", 0);
        }
    }

    if (mPaneRumbler->isRumbling()) {
        return;
    }

    if (isValidAppearSituation()) {
        setNerve(GET_NERVE(PlayerLeft, PlayerLeftNrvWait));
    } else if (!mIsForceAppear && CounterLayoutController::isWaitToDisappearCounter(this)) {
        setNerve(GET_NERVE(PlayerLeft, PlayerLeftNrvDisappear));
    }
}

void PlayerLeft::exeDisappear() {
    if (MR::isFirstStep(this)) {
        mLayoutAppearer->disappear();
    }

    if (mLayoutAppearer->isDisappeared()) {
        setNerve(GET_NERVE(PlayerLeft, PlayerLeftNrvHide));
    }
}
