// Galaxy 3.0 multiplayer: the entry screen (owner: entry). After the title ("Press A and B"), before the file select's heads: two of
// the game's big buttons (the RetryButton layout of the "Retry / Return to the Observatory" choice, with its appear, select and
// decide animations and the file select's sounds) over the file select's starry sky: Single Player and Multiplayer.
//
//  - Single Player: the file select exactly as before (the title hands over to the heads).
//  - Multiplayer: the Multiplayer menu (MpMenuSequence, opened "from the entry": the host picks which single-player file to copy or
//    play on inside the menu) over the same sky; its launch request loads the chosen file the way "Play This File" does
//    (FileSelector's DemoStartWait / Demo). Back from the menu: the two buttons again. Back from the buttons: nothing (B does nothing on the main menu).
//  - The last choice is the default cursor next time (mp.cfg "entry"; Single Player on first boot). After a session played from here
//    the game comes back straight to this screen (no title), the cursor on Multiplayer.
//  - Settings (settingsnative, 14 Nov): a third pill under the two, where the platform has the native Settings. It opens the in-game Settings menu
//    (MpMenuSettings.cpp) over the same sky; Back from there: the three pills again, the cursor on Settings. It is never remembered as the
//    default (mp.cfg "entry" stays Single Player or Multiplayer) and starts nothing of multiplayer.
//  - Back from the heads (menuback32, 20 Nov): B (or a tap on the file select's own round Back button, shown while the heads are up) on the file
//    select's heads goes back to this screen: the game's wipe fade closes, the camera is put back to the title's, the wipe opens on the
//    three pills with the cursor on Single Player (the title's own text is not shown again; B there does nothing). Nothing
//    is opened or written: no file is picked, no save is touched (the "entry" choice stays what it was). Only while no Multiplayer
//    window is up. Without this screen (SMG_MP_MENU=0) the heads have no Back, as in the original game.
//  - SMG_MP_MENU=0: nothing here runs (the title goes to the heads as in 2.2).
//
// Two parts: the state machine (Logic, pure: mp_bridge.h only; tests/mp/ui_menu_test.cpp drives it with the real MpMenu::Sequence)
// and the game side (MpEntryScreen.cpp: the layouts, the FileSelector hooks).
#pragma once

#include <stdint.h>

class Manual2P;

namespace MpEntry {

enum Choice : uint8_t { CHOICE_SINGLE = 0, CHOICE_MULTI = 1, CHOICE_SETTINGS = 2, CHOICE_COUNT = 3 };   // (settingsnative: Settings is a third pill, never remembered; CHOICE_COUNT is the most; Host::settingsAvailable() says whether there are 3)

// ---- the file select's side (FileSelector hooks, game code: MpEntryScreen.cpp)
enum Step : uint8_t {
    STEP_SINGLE = 0,   // on to the heads, as before (also: SMG_MP_MENU=0)
    STEP_WAIT,         // the entry screen or the Multiplayer menu is up: the title nerve waits
    STEP_TITLE,        // Back: the title again
    STEP_LAUNCH,       // load *launchFile and start the game (FileSelector's DemoStartWait / Demo)
};
void createForFileSelect();                               // FileSelector::init (scene heap)
bool skipTitle();                                         // FileSelector::exeTitle's first step: true = no title this time
Step fileSelectStep(Manual2P* manual, int* launchFile);   // FileSelector::exeTitle once the title is done, every frame
void flushChoice();                                       // FileSelector::exeFileSelect's first step: a remembered Single Player choice is written now (one bool test otherwise)
bool bgmStarted();                                        // FileSelector::exeTitleEnd: the file select's music plays already
bool headsBackAllowed();                                  // menuback32: FileSelector::exeFileSelect: the heads may offer Back (this screen exists, no Multiplayer window is up)
void headsBack();                                         // menuback32: FileSelector::exeBackToEntry, under the closed wipe: the next exeTitle shows this screen again (no title, cursor on Single Player)
// menuback32 (pure): the rule behind headsBackAllowed. built: the entry screen's layouts exist (SMG_MP_MENU is not 0); menuOpen: MpUi::isMenuOpen()
bool backFromHeadsAllowed(bool built, bool menuOpen);

// ---- the state machine (pure)
enum Se : uint8_t { SE_NONE = 0, SE_CURSOR, SE_DECIDE, SE_BACK };
struct Host {
    virtual void sound(Se se) = 0;
    virtual int savedChoice() = 0;              // mp.cfg "entry"
    virtual void saveChoice(int choice, bool persistNow) = 0;   // remember (mp.cfg "entry"); persistNow: write the file now (Multiplayer) or later (Single Player: nothing of ours may write on its way)
    virtual void openMenu() = 0;                // the Multiplayer menu, from the entry (MpUi::openMultiplayerMenuFromEntry)
    virtual bool menuOpen() = 0;                // MpUi::isMenuOpen(): the menu, one of its windows or the keyboard is up
    virtual bool takeLaunch(int* file) = 0;     // the menu's launch request (a session started: load this user file)
    virtual bool takeManual() = 0;              // the menu's "How to Play" (the game's 2P manual)
    // flow (review 2 #4): Single Player waits while the save root cannot be switched back to the single-player one (true: blocked,
    // a window says so); the file select never opens on the multiplayer root
    virtual bool singleBlocked() { return false; }
    // settingsnative: the third pill. settingsAvailable: false = no pill (NativeHostGet() is null or not available); openSettings: the in-game
    // Settings (MpUi::openSettingsFromEntry): menuOpen() then holds the logic in ST_MENU until it closes, and the entry screen comes back
    // with the cursor on Settings.
    virtual bool settingsAvailable() { return false; }
    virtual bool settingsLatched() { return false; }   // the stale LiveArea tile started the game with -settings: take that request once
    virtual void openSettings() {}
};
struct In {
    bool up = false, down = false, ok = false, back = false;   // triggers this frame (back: B or the Back button)
    int pointed = -1;          // the big button under the star pointer / finger (-1 none)
    bool shown = false;        // both buttons have appeared (input is taken)
    bool decided = false;      // the chosen button's ButtonDecide has played
    bool hidden = false;       // both buttons and the Back button are hidden
    bool manualOpen = false;   // the 2P manual is up
};
enum Cmd : uint8_t {
    CMD_NONE = 0,
    CMD_APPEAR,      // the two buttons (and the Back button) appear
    CMD_DECIDE,      // Out::button plays its ButtonDecide
    CMD_DISAPPEAR,   // the buttons (and the Back button) go
    CMD_OPEN_MENU,   // (done: Host::openMenu was called)
    CMD_MANUAL,      // show the 2P manual
    CMD_SINGLE,      // on to the file select's heads
    CMD_TITLE,       // back to the title
    CMD_LAUNCH,      // load Out::file
};
struct Out {
    Cmd cmd = CMD_NONE;
    int button = -1;
    int file = 0;
};
class Logic {
public:
    enum State : uint8_t { ST_OFF = 0, ST_APPEAR, ST_CHOOSE, ST_DECIDED, ST_HIDING, ST_MENU, ST_MANUAL, ST_DONE };
    // the screen comes up; the cursor on the remembered choice (Multiplayer after a session; fromHeads: Single Player, the pill the player just left)
    void begin(Host* host, bool afterSession, bool fromHeads = false);
    Out update(const In& in);                    // once a frame
    State state() const { return mState; }
    int cursor() const { return mCursor; }
    int choices() const { return mChoices; }   // settingsnative: the pills there are (2, or 3 with Settings)
    bool active() const { return mState != ST_OFF && mState != ST_DONE; }
    bool buttonsWanted() const { return mState == ST_APPEAR || mState == ST_CHOOSE || mState == ST_DECIDED; }

private:
    void enter(State s);
    Host* mHost = nullptr;
    State mState = ST_OFF;
    int mCursor = CHOICE_SINGLE;
    int mChoices = 2;
    int mChoice = CHOICE_SINGLE;
    int mLastPointed = -1;
    bool mToTitle = false;
    bool mEntered = false;   // the state's first frame is still to come
    int mManualFrames = 0;
    bool mManualSeen = false;
};
const char* stateName(Logic::State s);

}  // namespace MpEntry
