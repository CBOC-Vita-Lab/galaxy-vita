// Galaxy 3.0 multiplayer: the entry screen's state machine (see MpEntry.hpp). Pure code: port/mp_bridge.h (mp_log) only.
#include "Game/MP/MpEntry.hpp"

#include "port/mp_bridge.h"

namespace MpEntry {
namespace {
const char* const kStateNames[] = {"off", "appear", "choose", "decided", "hiding", "menu", "manual", "done"};
const char* ChoiceName(int c) { return c == CHOICE_MULTI ? "multiplayer" : (c == CHOICE_SETTINGS ? "settings" : "single"); }
}  // namespace

const char* stateName(Logic::State s) { return s <= Logic::ST_DONE ? kStateNames[s] : "?"; }

bool backFromHeadsAllowed(bool built, bool menuOpen) { return built && !menuOpen; }

void Logic::begin(Host* host, bool afterSession, bool fromHeads) {
    mHost = host;
    const int saved = host ? host->savedChoice() : CHOICE_SINGLE;
    mChoices = host && host->settingsAvailable() ? CHOICE_COUNT : CHOICE_COUNT - 1;   // settingsnative
    mCursor = fromHeads ? CHOICE_SINGLE : (afterSession ? CHOICE_MULTI : (saved == CHOICE_MULTI ? CHOICE_MULTI : CHOICE_SINGLE));
    mChoice = mCursor;
    mToTitle = false;
    mp_log("entry: screen shown cursor=%s%s", ChoiceName(mCursor), fromHeads ? " (back from the heads)" : (afterSession ? " (after a session)" : ""));
    enter(ST_APPEAR);
}

void Logic::enter(State s) {
    mState = s;
    mEntered = true;
    mLastPointed = -1;
}

Out Logic::update(const In& in) {
    Out out;
    if (!mHost) return out;
    const bool first = mEntered;
    mEntered = false;
    switch (mState) {
    case ST_APPEAR:
        if (first) out.cmd = CMD_APPEAR;
        if (in.shown) enter(ST_CHOOSE);   // (input waits for the buttons: the press that closed the menu or the title does nothing)
        return out;
    case ST_CHOOSE: {
        if (mHost->menuOpen()) return out;   // one of our windows (a session's message) has the buttons
        const bool latched = mChoices > CHOICE_SETTINGS && mHost->settingsLatched();   // settingsnative: the old LiveArea tile asked for Settings
        if (latched) mCursor = CHOICE_SETTINGS, mp_log("entry: Settings asked for by the old tile");
        const bool ok = in.ok || latched;
        if (first && in.pointed >= 0) mLastPointed = in.pointed;   // (a pointer that was already resting on a pill when the screen came up does not choose it: only moving on does)
        int want = mCursor;
        if (in.pointed >= 0 && in.pointed < mChoices && in.pointed != mLastPointed) want = in.pointed;   // (only when it moves on)
        mLastPointed = in.pointed;
        if (in.up && want > 0) want = want - 1;   // (one step; no wrap)
        if (in.down && want < mChoices - 1) want = want + 1;
        if (want != mCursor) {
            mCursor = want;
            mHost->sound(SE_CURSOR);
        }
        if (ok && mCursor == CHOICE_SINGLE && mHost->singleBlocked()) {
            // flow (review 2 #4): the save root is still the multiplayer one: never the file select on it (the window says why)
            mp_log("entry: single player waits for the save root");
        } else if (ok) {
            mChoice = mCursor;
            if (mChoice != CHOICE_SETTINGS && mHost->savedChoice() != mChoice) mHost->saveChoice(mChoice, mChoice == CHOICE_MULTI);   // (Single Player: RAM only here; written when the file select list is up)
            mHost->sound(SE_DECIDE);
            mp_log("entry: chose %s", ChoiceName(mChoice));
            out.cmd = CMD_DECIDE;
            out.button = mChoice;
            mToTitle = false;
            enter(ST_DECIDED);
        } else if (in.back) {
            mHost->sound(SE_BACK);
            mp_log("entry: back to the title");
            mToTitle = true;
            out.cmd = CMD_DISAPPEAR;
            enter(ST_HIDING);
        }
        return out;
    }
    case ST_DECIDED:
        if (in.decided) {
            out.cmd = CMD_DISAPPEAR;
            enter(ST_HIDING);
        }
        return out;
    case ST_HIDING:
        if (!in.hidden) return out;
        if (mToTitle) {
            out.cmd = CMD_TITLE;
            enter(ST_OFF);
        } else if (mChoice == CHOICE_SINGLE) {
            out.cmd = CMD_SINGLE;
            enter(ST_DONE);
        } else if (mChoice == CHOICE_SETTINGS) {   // settingsnative
            mHost->openSettings();
            out.cmd = CMD_OPEN_MENU;
            enter(ST_MENU);
        } else {
            mHost->openMenu();
            out.cmd = CMD_OPEN_MENU;
            enter(ST_MENU);
        }
        return out;
    case ST_MENU: {
        if (mHost->menuOpen()) return out;
        int file = 0;
        if (mHost->takeLaunch(&file)) {
            mp_log("entry: launch file=%d", file);
            out.cmd = CMD_LAUNCH;
            out.file = file;
            enter(ST_DONE);
        } else if (mHost->takeManual()) {
            mp_log("entry: how to play (2P manual)");
            out.cmd = CMD_MANUAL;
            mManualFrames = 0;
            mManualSeen = false;
            enter(ST_MANUAL);
        } else {
            const bool settings = mChoice == CHOICE_SETTINGS;
            mp_log(settings ? "entry: settings closed: the entry screen again" : "entry: multiplayer menu closed: the entry screen again");
            mCursor = settings ? CHOICE_SETTINGS : CHOICE_MULTI;
            enter(ST_APPEAR);
            out.cmd = CMD_APPEAR;
            mEntered = false;
        }
        return out;
    }
    case ST_MANUAL:
        ++mManualFrames;
        if (in.manualOpen) mManualSeen = true;
        else if (mManualSeen || mManualFrames > 30) {   // (closed; or it never opened: the menu again all the same)
            mHost->openMenu();
            out.cmd = CMD_OPEN_MENU;
            enter(ST_MENU);
        }
        return out;
    default: return out;
    }
}

}  // namespace MpEntry
