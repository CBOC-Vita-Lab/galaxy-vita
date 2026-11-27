// Galaxy 3.0 multiplayer: the entry screen's game side (see MpEntry.hpp; owner: entry).
//
// The two big buttons are the game's RetryButton layout (CometRetryButton's "Retry / Return to the Observatory" choice): its upper
// button (NBackUp) is Single Player, the lower one (NBackDown) Multiplayer, its dark background pane hidden so the file select's
// starry sky stays behind. ButtonPaneController plays ButtonAppear / ButtonWait / ButtonSelectIn / ButtonSelectOut / ButtonDecide /
// ButtonEnd as in the game; the cursor (d-pad, stick) points a button by hand (onPointing), the star pointer / a finger moves the
// cursor when it moves onto a button. A decides, B or the file select's own round Back button (a second BackButton) goes back to
// the title. Sounds: the file select's (cursor SE_SY_BUTTON_CURSOR_ON, decide SE_SY_FILE_SEL_UPPER_DECIDE, back
// SE_SY_GALAXY_DECIDE_CANCEL). Scene objects (file select scene heap): the pointers here are replaced at every FileSelector::init
// and only used from FileSelector's own calls in that scene.
#include "Game/MP/MpEntry.hpp"

#include "Game/MP/MpDemo.hpp"   // MP3 demo (SMG_MP_DEMO=menus)
#include "Game/MP/MpMenuSequence.hpp"   // settingsnative: MpOverlay::toastText
#include "Game/MP/MpMessages.hpp"
#include "Game/MP/MpUi.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Screen/BackButton.hpp"
#include "Game/Screen/ButtonPaneController.hpp"
#include "Game/Screen/LayoutActor.hpp"
#include "Game/Screen/LayoutCoreUtil.hpp"
#include "Game/Screen/Manual2P.hpp"
#include "Game/System/Language.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include "Game/Util/StarPointerUtil.hpp"
#include "port/mp_bridge.h"
#include "port/mp_core_game.h"
#include "port/settings_native.h"   // settingsnative: is there a Settings pill
#include <nw4r/lyt/textBox.h>

namespace MpEntry {
namespace {

const float kSpreadY = 8.0f;    // (layout units each button moves away from the middle)
const float kScale = 1.3f;   // (the game's big buttons, larger: the screen has nothing else; the selected one grows 1.15 more and still clears Back)
// settingsnative: with three pills the buttons are smaller (1.1) so the column clears the Back button; the two pills of the first layout
// and the Settings pill (a second layout instance, its lower button hidden) stand kPitch layout units apart
const float kScale3 = 1.1f;
const float kPitch3 = 96.0f;

// mode 0: the two pills (Single Player, Multiplayer) at the game's size 1.3; mode 1: the same two at 1.1, in the upper two places of three;
// mode 2: the Settings pill alone (settingsnative): the RetryButton layout with its lower button hidden, in the third place.
class EntryLayout : public LayoutActor {
public:
    explicit EntryLayout(int mode = 0) : LayoutActor("MpEntry", true) { mMode = mode; }
    virtual void init(const JMapInfoIter&) {
        initLayoutManager("RetryButton", 1);
        MR::connectToSceneLayout(this);
        MR::hidePaneRecursive(this, "BG");
        const bool alone = mMode == 2;
        if (alone) MR::hidePaneRecursive(this, "NBackDown");
        MR::createAndAddPaneCtrl(this, "NBackUp", 1);
        if (!alone) MR::createAndAddPaneCtrl(this, "NBackDown", 1);
        mCtrl[0] = new ButtonPaneController(this, "NBackUp", "BoxButton1", 0, true);
        if (!alone) mCtrl[1] = new ButtonPaneController(this, "NBackDown", "BoxButton2", 0, true);
        for (ButtonPaneController* c : mCtrl) {
            if (!c) continue;
            c->_22 = false;   // (decided: it waits; we make both go)
            c->_23 = true;    // (pointed by hand from the cursor; the star pointer moves the cursor)
            c->mAppearAnimName = "ButtonAppear";
            c->mWaitAnimName = "ButtonWait";
            c->mPointingAnimName = "ButtonSelectIn";
            c->mDecideAnimName = "ButtonDecide";
            c->mNotPointingAnimName = "ButtonSelectOut";
            c->mDisappearAnimName = "ButtonEnd";
        }
        // the layout's text boxes were sized for "Retry" / its own words: room for ours first
        for (const char* name : {"Txt1", "Sha1", "Txt2", "Sha2"})
            if (nw4r::lyt::Pane* p = MR::getPane(this, name)) LayoutCoreUtil::reallocateTextBoxMessage(static_cast< nw4r::lyt::TextBox* >(p), 47);
        const float scale = mMode == 0 ? kScale : kScale3;
        if (nw4r::lyt::Pane* root = MR::getRootPane(this)) {
            root->mScale.x = root->mScale.y = scale;
            // (three pills: the first layout's two stand at +pitch / 0 about the picture's middle, the Settings layout's one at -pitch: a pill is at
            // its pane's 48 (NBackUp) / -48 (NBackDown) in the layout, so the roots sit where those land)
            if (mMode == 1) root->mTranslate.y += kPitch3 * 0.5f * kScale3;
            if (mMode == 2) root->mTranslate.y -= (kPitch3 + 48.0f) * kScale3;
        }
        // a clear gap: the game's two buttons stand close (a selected, grown one nearly touches the other): the pair moves apart about
        // its middle (the panes sit at 0 here, the buttons' own offsets are below them), so the rest gap is about half a button's height
        // and the grown one still clears the other button and the Back button
        nw4r::lyt::Pane* pUp = MR::getPane(this, "NBackUp");
        nw4r::lyt::Pane* pDown = MR::getPane(this, "NBackDown");
        if (pUp && pDown && mMode != 1) {
            pUp->mTranslate.y += kSpreadY;
            pDown->mTranslate.y -= kSpreadY;
            mp_log("entry: buttons spread y=%.1f / %.1f", pUp->mTranslate.y, pDown->mTranslate.y);
        }
        kill();
    }
    void setTexts() {
        MpMsgText::setLanguage(MpMsgText::languageIndex(MR::getCurrentLanguagePrefix()));
        char16_t raw[48], box[96];
        const MpText ids[2] = {mMode == 2 ? MP_T_SET_ENTRY : MP_T_ENTRY_SINGLE, MP_T_ENTRY_MULTI};
        const char* panes[2] = {"Text1", "Text2"};
        for (int i = 0; i < (mMode == 2 ? 1 : 2); i++) {
            MpMsgText::format(raw, 48, ids[i], MpMsgText::Args());
            MpMsgText::toWindowText(box, 96, raw, false);
            MR::setTextBoxMessageRecursive(this, panes[i], reinterpret_cast< const wchar_t* >(box));
        }
    }
    virtual void control() {
        // the cursor's button is pointed (ButtonSelectIn), the other one lets go (ButtonSelectOut)
        if (mPointCursor >= 0 && mPointCursor < 2 && mCtrl[mPointCursor]) mCtrl[mPointCursor]->onPointing();
        for (ButtonPaneController* c : mCtrl)
            if (c) c->update();
    }
    ButtonPaneController* mCtrl[2] = {nullptr, nullptr};
    int mPointCursor = -1;
    int mMode = 0;
};

// settingsnative: the third pill is there where the platform has the native Settings (the Vita with GXV_SETTINGS=0 has none)
bool SettingsPill() {
    const port::settings::NativeHost* h = port::settings::NativeHostGet();
    return h != nullptr && h->available;
}

struct GameHost : Host {
    void sound(Se se) override {
        switch (se) {
        case SE_CURSOR: MR::startSystemSE("SE_SY_BUTTON_CURSOR_ON", -1, -1); break;
        case SE_DECIDE: MR::startSystemSE("SE_SY_FILE_SEL_UPPER_DECIDE", -1, -1); break;
        case SE_BACK: MR::startSystemSE("SE_SY_GALAXY_DECIDE_CANCEL", -1, -1); break;
        default: break;
        }
    }
    int savedChoice() override { return mp_core_saved_entry() == 1 ? CHOICE_MULTI : CHOICE_SINGLE; }
    void saveChoice(int c, bool persistNow) override { if (!MpDemo::persistOk()) return; mp_entry_remember(static_cast< uint8_t >(c == CHOICE_MULTI ? 1 : 0), persistNow); }
    void openMenu() override { MpUi::openMultiplayerMenuFromEntry(); }
    bool menuOpen() override { return MpUi::isMenuOpen(); }
    bool takeLaunch(int* file) override { return MpUi::takeLaunchRequest(file); }
    bool takeManual() override { return MpUi::takeManualRequest(); }
    bool singleBlocked() override { return MpUi::singlePlayerGate(); }   // flow (review 2 #4)
    bool settingsAvailable() override { return SettingsPill(); }
    void openSettings() override { MpUi::openSettingsFromEntry(); }
    bool settingsLatched() override { return MpUi::takeSettingsLatch(); }
};

GameHost sHost;
Logic sLogic;
EntryLayout* sLayout = nullptr;   // (this file select scene's; replaced at every createForFileSelect)
EntryLayout* sLayout3 = nullptr;  // settingsnative: the Settings pill's own layout instance (null: no third pill)
ButtonPaneController* sCtrl[3] = {nullptr, nullptr, nullptr};   // the pills in order (Single Player, Multiplayer, Settings)
int sCount = 2;                   // how many pills there are
BackButton* sBack = nullptr;
bool sReturnToEntry = false;      // a session was launched from here: the next file select comes back to this screen, no title
bool sSkipTitle = false;          // (this scene: decided at createForFileSelect; or by headsBack)
bool sFromHeads = false;          // menuback32: this entry screen is the heads' Back (cursor on Single Player)
bool sBgm = false;                // the file select's music was started here (this scene)
bool sBegun = false;

bool Hidden() {
    for (int i = 0; i < sCount; i++)
        if (!sCtrl[i]->isHidden()) return false;
    return sBack == nullptr || sBack->isHidden();
}

}  // namespace

void createForFileSelect() {
    sLayout = nullptr;
    sLayout3 = nullptr;
    sCount = 2;
    sBack = nullptr;
    sLogic = Logic();
    sBegun = false;
    sBgm = false;
    sSkipTitle = false;
    sFromHeads = false;
    if (!MpUi::featureEnabled()) return;
    sSkipTitle = sReturnToEntry;   // (back from a session played from here: straight to this screen)
    sReturnToEntry = false;
    const bool three = SettingsPill();   // settingsnative
    sLayout = new EntryLayout(three ? 1 : 0);
    sLayout->initWithoutIter();
    sCtrl[0] = sLayout->mCtrl[0], sCtrl[1] = sLayout->mCtrl[1], sCtrl[2] = nullptr;
    if (three) {
        sLayout3 = new EntryLayout(2);
        sLayout3->initWithoutIter();
        sCtrl[2] = sLayout3->mCtrl[0];
        sCount = 3;
        mp_log("entry: three pills (Settings)");
    }
    // (owner, 20 Nov: this screen IS the main menu, as the file select was the first screen in the original game: no Back button here and
    //  no way back to the title from it; each mode it opens has its own back to this screen. sBack stays null: every use below checks it.)
}

bool skipTitle() {
    if (!sSkipTitle) return false;
    mp_log(sFromHeads ? "entry: back from the heads: no title" : "entry: back from a session: no title");
    return true;
}

bool headsBackAllowed() { return backFromHeadsAllowed(sLayout != nullptr, MpUi::isMenuOpen()); }

void headsBack() {   // (under the closed wipe: the title nerve's first step comes next, and the entry screen with it)
    sSkipTitle = true;
    sFromHeads = true;
    sBegun = false;
    sBgm = true;   // (the file select's music has been playing all along and goes on)
    mp_log("entry: back from the heads");
}

bool bgmStarted() { return sBgm; }

void flushChoice() { mp_entry_flush(); }

Step fileSelectStep(Manual2P* manual, int* launchFile) {
    {   // settingsnative: the first call, before anything is tested (the entry screen does not exist with SMG_MP_MENU=0): the start got this far,
        // so the recovery marker goes; a start in a hold says so, once
        static bool sReached = false;
        if (!sReached) {
            sReached = true;
            port::settings::NativeEntryReached();
            MpMenu::settingsBootToast();   // (a start in a hold says so, once)
        }
    }
    if (sLayout == nullptr) return STEP_SINGLE;   // (SMG_MP_MENU=0)
    if (!sBegun) {
        sBegun = true;
        sLayout->setTexts();
        if (sLayout3) sLayout3->setTexts();
        sLogic.begin(&sHost, sSkipTitle && !sFromHeads, sFromHeads);
        sSkipTitle = false;
        sFromHeads = false;
        MR::startStarPointerModeFileSelect(sLayout);
        if (!MR::isPlayingStageBgmName("MBGM_FILE_SELECT")) MR::startStageBGM("MBGM_FILE_SELECT", false);
        sBgm = true;
    }
    In in;
    if (sLogic.state() == Logic::ST_CHOOSE) {
        in.up = MR::testCorePadTriggerUp(0) || MR::testSubPadStickTriggerUp(0);
        in.down = MR::testCorePadTriggerDown(0) || MR::testSubPadStickTriggerDown(0);
        in.ok = MR::testCorePadTriggerA(0);
        in.back = false;   // (the main menu: B does nothing here, as on the original file select)
        if (sBack && sBack->isPointing() && in.ok) in.ok = false, in.back = true;   // (A on the Back button is the Back button)
        for (int i = 0; i < sCount; i++)
            if (sCtrl[i]->isPointingPaneDirect()) in.pointed = i;
    }
    ButtonPaneController** c = sCtrl;
    in.shown = !MR::isDead(sLayout);
    for (int i = 0; i < sCount; i++) in.shown = in.shown && !c[i]->isAppearing() && !c[i]->isHidden();
    in.decided = sLogic.cursor() >= 0 && sLogic.cursor() < sCount && c[sLogic.cursor()]->isDecidedWait();
    in.hidden = Hidden();
    in.manualOpen = manual != nullptr && !manual->isClosed();
    const bool backByButton = sBack && sBack->_24;
    if (MpDemo::enabled()) MpDemo::entryInput(&in, static_cast< int >(sLogic.state()), sLogic.cursor());   // MP3 demo: the tour's presses (nothing without the switch)
    const Out out = sLogic.update(in);
    {
        const int pc = sLogic.state() == Logic::ST_CHOOSE || sLogic.state() == Logic::ST_APPEAR ? sLogic.cursor() : -1;
        sLayout->mPointCursor = pc >= 0 && pc < 2 ? pc : -1;
        if (sLayout3) sLayout3->mPointCursor = pc == 2 ? 0 : -1;
    }
    switch (out.cmd) {
    case CMD_APPEAR:
        sLayout->appear();
        if (sLayout3) sLayout3->appear();
        for (int i = 0; i < sCount; i++) c[i]->appear();
        if (sBack) sBack->appear();
        MR::startStarPointerModeFileSelect(sLayout);
        return STEP_WAIT;
    case CMD_DECIDE:
        if (out.button >= 0 && out.button < sCount) c[out.button]->decide();
        return STEP_WAIT;
    case CMD_DISAPPEAR:
        for (int i = 0; i < sCount; i++)
            if (!c[i]->isHidden() && !c[i]->isDisappearing()) c[i]->disappear();
        if (sBack && !sBack->isHidden() && !backByButton) sBack->disappear();   // (the Back button chosen goes by itself)
        return STEP_WAIT;
    case CMD_MANUAL:
        if (manual) manual->appear();
        return STEP_WAIT;
    case CMD_SINGLE:
        sLayout->kill();
        if (sLayout3) sLayout3->kill();
        return STEP_SINGLE;
    case CMD_TITLE:
        sLayout->kill();
        if (sLayout3) sLayout3->kill();
        sBegun = false;   // (the title, then this screen again)
        sBgm = false;
        return STEP_TITLE;
    case CMD_LAUNCH:
        sLayout->kill();
        if (sLayout3) sLayout3->kill();
        sReturnToEntry = true;
        if (launchFile) *launchFile = out.file;
        return STEP_LAUNCH;
    default: break;
    }
    if (sLogic.state() == Logic::ST_DONE) return STEP_SINGLE;   // (the heads)
    return STEP_WAIT;
}

}  // namespace MpEntry
