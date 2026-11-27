// Galaxy 3.0 multiplayer: the menus' state machine (owner: ui). Pure code: it talks to the port through mp_bridge.h, to the save
// module through MpSaveGame (MpUi.hpp), and to the game through MpMenu::Io (message windows, sounds, galaxy names), which
// MpUiSystem.cpp implements with the game's SysInfoWindow and the host tests implement with a fake. It never waits: every network
// step is a request plus a poll of mp_ui_status once a frame, with a timeout of its own on top of the port's.
//
// Screens (DESIGN.md section 1, "The menus"; flow, wave 2b): Multiplayer (Rejoin / Play Together / Star Race / Friends) > Join or
// Host (Quick Match, Leaderboards where the service is configured) > the Join list (every room it can find, Room Code, Type an
// address) or the lobby at once (Room Settings on its Square, the character on its Triangle, a player's row: their choices; Start:
// "Which file?" for the original host); Friends; Leaderboards; the session menu (Resume, Warp, Messages, Players, Observatory,
// Leave). Errors are one message window each, then a usable screen. The waiting screen, the results card and the toasts are
// MpOverlay (MpToast.cpp).
#pragma once

#include <stdint.h>
#include <string>

#include "Game/MP/MpMessages.hpp"
#include "Game/MP/MpRulesLogic.hpp"   // touches
#include "Game/MP/MpPanelPlan.hpp"
#include "Game/MP/MpSaveGame.hpp"
#include "Game/MP/MpUi.hpp"
#include "port/mp_bridge.h"
#include "port/mp_conntest.h"   // conntest: the Connection Test screen
#include "port/mp_diag.h"       // beta diagnostics: the report and the check (weak functions)
#include "port/mp_gather.h"     // flow (one door): MpGatherStatus per-frame cache

namespace MpMenu {

enum class Se : uint8_t { Cursor, Decide, Cancel, Open, Close, Buzzer, Success, Tab, Join, Leave, Fanfare /* touches */ };
enum class Dlg : uint8_t { Ok, YesNo };
enum class DlgState : uint8_t { None, Open, Yes, No };   // Ok windows answer Yes when closed

struct Input {
    bool up = false, down = false, left = false, right = false;   // with key repeat (the caller's)
    bool ok = false, back = false, tri = false, sq = false, start = false;
};

struct Io {
    virtual void sound(Se se) = 0;
    virtual void dialogOpen(Dlg kind, const char* messageId) = 0;
    virtual DlgState dialogState() = 0;
    virtual int galaxyCount() = 0;
    virtual int galaxyName(int galaxy, char16_t* out, int cap) = 0;      // returns the length
    virtual int scenarioCount(int galaxy) = 0;
    virtual int scenarioName(int galaxy, int scenario, char16_t* out, int cap) = 0;   // scenario 1..count
    virtual uint64_t rawButtons() { return ~0ull; }   // settingsnative: the buttons held now (the port's stable Button bits); ~0: not supported here (the button list is used)
    virtual bool touched() { return false; }          // settingsnative: a finger is on the screen
    virtual int currentGalaxy() { return -1; }   // flow: the galaxy being played (MpMsgText galaxy index), -1 none / unknown
};

enum Scr : uint8_t {
    S_NONE = 0, S_MAIN,
    S_SAVE,       // flow: Which file? (the original host's Start)
    S_SLOT, S_FRIENDS, S_BOARD, S_BUSY, S_SESSION, S_WARP, S_MESSAGES, S_PLAYERS, S_CHARS,
    S_REQUESTS, S_REQUEST, S_BLOCKED,   // friendsui: incoming friend requests (list), one request (Add / Not Now / Block), blocked players
    S_SPFILE,     // entry / flow: one of my files ("Start from my own file...")
    S_FILEUSE,    // flow: that file: Make a co-op copy / Play on this file
    S_PLAYER,     // flow: one player's choices (param = slot): Change Character / Add Friend / Block / Mute / Remove from Room
    S_PLAYED,     // flow: Friends > Played With (the last eight names)
    S_CONNTEST,   // conntest: Connection Test (the five checks filling in, then the verdict)
    S_KEEP,       // m-files (for flow to take over): Safe Copies: Make a Copy / Bring Back a Copy (family: MpMenuFiles.cpp)
    S_KEEPLIST,   // m-files (for flow to take over): Bring Back a Copy: the copies, newest first
    S_KEEPCOPY,   // keepsafe API: one copy: only what can be brought back (Team Files, online account, settings)
    S_GATHER,     // flow (one door): the gathering screen: the players' heads, one line, at most three big buttons (port/mp_gather.h)
    S_FALLS,      // flow (one door): "If someone falls, how should they come back?" (asked once, at the first Start of Play Together)
    S_HELP,       // beta diagnostics: Having Trouble? (Check Connection / Send a Problem Report; family: MpMenuReport.cpp)
    S_REPORT,     // beta diagnostics: the consent screen: Send / Add a Note / Cancel
    S_RPWAIT,     // beta diagnostics: a report is being sent (param 0) / the connection is being checked (param 1)
    // settingsnative (14 Nov): the in-game Settings, from the entry screen (family: MpMenuSettings.cpp). No session, no network.
    S_SET_MAIN,     // Picture / Camera / Controls / More (list rows with values)
    S_SET_MORE,     // Language / Less Motion / Star Bit Reach / Put Everything Back
    S_SET_CONTROLS, // Tilt / Tilt Strength / Pointer / Rear Touch / Tap to Jump / Change Buttons
    S_SET_BUTTONS,  // the layout row, then a row per move (a list)
    S_SET_PICK,     // the buttons of the layout, for the move being changed (param = its row)
    S_SET_LEAVE,    // Save / Don't Save / Keep Editing
    S_SET_CAMERA,   // Camera Zoom / Right Stick / Invert Camera
    S_SET_CAPTURE,  // "Press the button you want" (param = the move's row)
    S_SET_RESTART,  // setfix31b: "Save and Start Again" waits for a save in flight (param 0: Later; 1: it seems stuck: Start Again Now / Later)
    S_COUNT
};
const char* screenName(Scr s);

class Sequence {
public:
    void init(Io* io);
    // Entry points.
    void openMain(int selectedFile);   // file select (1..6, 0 = none)
    void openFromEntry();              // entry: the entry screen's Multiplayer (no file chosen: Copy / Play on a file pick one here)
    bool fromEntry() const { return mFromEntry; }
    void openSession();                // the pause button during a session
    enum SettingsOrigin { SO_ENTRY = 0, SO_PAUSE = 1, SO_SESSION = 2 };
    bool openSettings(int origin = SO_ENTRY);   // settingsnative: the Settings (entry screen / pause / the session's menu: false = not opened)
    int settingsOrigin() const;
    void settingsTestSwap();   // harness only (SMG_SETTINGS_SWAP_TEST=1): Jump to Circle through the model, then the menu closes (saves, applies the buttons live)
    void settingsTestSetWhere(int origin);   // host tests only: change what the open Settings may do (a session, the pause menu)
    bool settingsOpen() const { return mSettingsOn; }
    void update(const Input& in);      // once a frame, every scene, open or not
    bool isOpen() const { return mDepth > 0; }
    bool isBusy() const;               // a window, the keyboard or the menu wants the buttons
    bool reconnecting() const { return mReconnecting; }   // fix3: the core rejoins on its own: still in the session (Start = this menu)
    Scr screen() const { return mDepth > 0 ? mStack[mDepth - 1].scr : S_NONE; }
    // mppause: the session menu's first screen is up on a handheld (no window or keyboard over it): the port's zoom row (L / R) shows under its buttons
    bool zoomRowShown() const;
    int depth() const { return mDepth; }
    int cursor() const { return mDepth > 0 ? mStack[mDepth - 1].cursor : -1; }
    int itemCount() const;             // the current screen's choosable rows
    void buildModel(MpPanel::Model* m) const;
    bool firstTapPoints(int visible);  // uxfix #8: a finger's first tap on a Messages tile only points it (true: swallow the tap's Cross); a second tap on it sends
    void pointAtVisible(int i);        // vitabtn: a finger is on the i-th row / head / tile shown: it becomes the cursor
    void pointAt(int row);             // the star pointer is over this big button: it becomes the cursor
    // Requests for the scene side.
    // beta diagnostics (MpMenuReport.cpp; Game/MP/MpMenuReport.hpp is the door for other menus): the whole Send a Problem Report flow from any place
    // (account first, what is sent, then Send / Add a Note / Cancel), and Check Connection. where: 0 Multiplayer menu, 1 Settings, 2 after a session.
    void openReport(int where, int kind);
    void openCheck();
    bool takeLaunch(int* file);
    bool takeManual();
    bool takeWarp(int* slot);
    bool takeDome();
    void leaveByUs() { mLeavingByUs = true; mp_ui_leave(); }   // smallfix: the Leave Race row of the results card: leaves as the pause screen's Leave does (the save offer follows)
    void onFileSelect();               // back at the file select: tidy the save root
    // fix2: every file select turn: retry a save root switch that failed, then say so. fix3: the warning waits until the file
    // select can hold its input (canWarn); true on the turn the window opens (the file select then holds until isBusy() ends)
    bool fileSelectTick(bool canWarn = true);
    bool singlePlayerGate();           // flow (review 2 #4): true = Single Player must wait (a window says why)
    // (tests)
    bool dialogActive() const { return mDlg != 0; }
    bool keyboardActive() const { return mKb != 0; }
    const char* lastDialog() const { return mLastDialogId; }
    void close();
    // MP3 demo (SMG_MP_DEMO=menus, MpDemo.cpp): a screen shown for a tour, with none of its flow (no request, nothing started, nothing
    // saved, no profile set); and the way back out of it.
    void demoShow(Scr s, uint8_t param = 0);
    void demoBack();

    enum SaveKind : uint8_t { SAVE_NONE = 0, SAVE_COOP, SAVE_COPY, SAVE_MINE };

private:
    struct Frame { Scr scr; int8_t cursor, scroll; uint8_t param; };
    enum { kStack = 10, kItems = 40 };

    // flow split: each screen family (MpMenu<Family>.cpp) answers for its own screens, items, windows and keyboards; the shared core
    // asks them in turn. A hook returns false for what is not its family's.
#define MPMENU_FAMILY_HOOKS(F) \
    bool items##F(Scr s, uint8_t param, int* out, int* n) const; \
    bool choose##F(int item); \
    bool label##F(int item, MpPanel::Row& r, const Frame& f) const; \
    bool model##F(MpPanel::Model* m, const Frame& f, int cursorItem, int count, const int* it) const; \
    bool dialog##F(int purpose, bool yes, int param); \
    bool keyboard##F(int purpose, const char* text); \
    bool tick##F(Scr scr, const Input& in, bool* stop); \
    bool press##F(Scr scr, const Input& in, int item, int count, int stage); \
    bool leave##F(Scr scr, uint8_t param, int phase);
    MPMENU_FAMILY_HOOKS(Main)
    MPMENU_FAMILY_HOOKS(Gather)
    MPMENU_FAMILY_HOOKS(Chars)
    MPMENU_FAMILY_HOOKS(Session)
    MPMENU_FAMILY_HOOKS(Files)
    MPMENU_FAMILY_HOOKS(Friends)
    MPMENU_FAMILY_HOOKS(ConnTest)
    MPMENU_FAMILY_HOOKS(Rules)
    MPMENU_FAMILY_HOOKS(Report)
    MPMENU_FAMILY_HOOKS(Settings)   // settingsnative
#undef MPMENU_FAMILY_HOOKS
    void rowLabel(int item, MpPanel::Row& r, const Frame& f) const;       // a row's words (the families', else rowLabelList)
    void rowLabelList(int item, MpPanel::Row& r, const Frame& f) const;   // a player's / friend's / request's row (shared)
    // items of the current screen
    int items(Scr s, uint8_t param, int* out) const;
    static int visibleRows(Scr s);   // uinative: rows a screen shows at once (a longer list turns pages)
    void fitScroll(Frame& f);        // uinative: the page (or window of rows) that holds the cursor
    void push(Scr s, uint8_t param = 0);
    void pop();
    void popTo(Scr s);
    void cutToSafe();
    Frame& top() { return mStack[mDepth - 1]; }
    const Frame& top() const { return mStack[mDepth - 1]; }

    void ask(int purpose, MpText text, const MpMsgText::Args& a, Dlg kind, int param = 0);
    void say(int purpose, MpText text, const MpMsgText::Args& a = MpMsgText::Args(), int param = 0) { ask(purpose, text, a, Dlg::Ok, param); }
    void onDialog(int purpose, bool yes, int param);
    void onKeyboard(int purpose, bool done, const char* text);
    void openKeyboard(int purpose, MpText title, const char* initial, int maxChars, bool codeOnly);
    void choose(int item);
    void showError(int err);
    int failError(int fallback) const;   // fix2: the core's reason for a refused request (MpUiStatus.error), else fallback
    void noteErrorShown(int err);        // fix2: "ui: error shown <code>" whenever an MpUiError reaches the screen
    // fix3: an online failure: the service's own code worded natively (MpMsgText::serviceText of mp_service_last_code), else
    // `fallback` (a window text) or the MpUiError's text. Logged "ui: error shown <err> (<window>) code=<code>".
    void showServiceError(int err, MpText fallback = MP_T_NONE);
    void startBusy(int purpose);
    void tickBusy();
    void watchSession();
    void sessionEnded(bool byUs, int err);
    bool prepareHostSave();
    void saveChosen();                 // fix2: back to the screen that asked (More Options, or Quick Match's save question)
    void defaultCoopSave();            // fix2: the first co-op file in use (continue), else the first empty one (new)
    void startKeepFlow();
    void showTip();
    // flow: Join, Host, Start, a player's choices, what the session remembers (port/mp_ui_flow.h)
    void requestStart();
    void startSession();
    // flow (one door): the gathering screen (port/mp_gather.h)
    void openGather(bool byCode);
    void openRoomGather();             // lobby: the gathering screen for a room the core already holds (Play with Anyone, a friend's game, Continue X's Game)
    void afterFirstChars();            // the character select of the first play is done: where the player was going
    bool gathering() const;            // the gathering screen is on the stack (Start, the file, leaving belong to it)
    const MpGatherStatus& gatherStatus() const;   // cached once per frame in update() (refreshed lazily on miss)
    void refreshGatherCache() const;
    void invalidateGatherCache();
    int headSlot(uint32_t id) const;   // a player's head keeps its layout slot for the whole gathering (core2: key on the id)
    void showFarCode();
    void codeJoinFailed(int why);   // code5: a far-away code join that failed (MpInviteError, 0 = the game never answered)
    static bool connectionLost(int err);   // uxfix #2
    void regatherAfterQuick();         // uxfix #3
    void openCodeKeyboard();           // uxfix #1: Join with a Code: Wi-Fi check, then the keyboard (the text of a failed code comes back)
    const char* playerCode(int slot) const;
    const char* playedCode(const char* name) const;
    int playedCount() const;
    void openPlayer(int slot);
    void choosePlayer(int item);
    void notePlayers();
    void writeCoopMeta(int slot);
    bool rejoinOffered() const;
    void refreshStars() const;         // flow (review 2 #2): the file screens' star counts, read when pushed (or lazily on cache miss)
    int spStars(int file) const;       // a single-player file's stars, -1 never created / unreadable (cached while a file screen is up)
    // conntest: the Connection Test screen (Square on the main, connection, host / join and session screens; offered after a failed join / host)
    bool openConnTest();
    void tickConnTest(const Input& in);
    void askLines(int purpose, const MpText* ids, int count, Dlg kind);   // a window of up to three of the table's one-line texts
    bool sessionLaggy() const;
    int coopStars(int slot) const;
    void openChars(int fromLobby);
    int charOwner(int look) const;
    void pickChar(int look);
    void tickChars();
    void offerSave();
    void saveTo(int choice, int slot);
    void slotChosen(int slot);
    void requireName(int purposeAfter);
    bool needAccount(int item);        // netx: true when the online account is being made first (the row is chosen again after)
    void continueAfterName(int purposeAfter);
    void moveCursor(int dir, int count);
    void changeValue(int item, int dir);
    void requestBoard();
    void pollFriends();                // friendsui: the friends and the incoming requests (one request, two getters)
    void chooseRequest(int item);      // friendsui: Add / Not Now / Block on the request in view
    void blockRequest();               // friendsui: the Yes of the block question
    void unblockAt(int index, const char* code);   // friendsui: Blocked Players (index -1: a typed code)
    void refreshFriends();
    bool friendsEntryAllowed();        // uxwait: false above the hourly request ceiling (the screen then shows the rows it has)
    int othersPresent() const;

    Io* mIo = nullptr;
    Frame mStack[kStack];
    int mDepth = 0;
    uint32_t mFrame = 0;
    float mPulse = 0, mFade = 0;
    int mDecidedRow = -1;            // uinative: the row chosen with the decide button, on frame mDecidedFrame
    uint32_t mDecidedFrame = 0;
    // context
    int mSpFile = 0;
    bool mFromFileSelect = false;
    bool mFromEntry = false;      // entry: opened from the entry screen (mSpFile is picked in S_SPFILE)
    MpMode mMode = MP_MODE_TOGETHER;
    MpConn mConn = MP_CONN_ONLINE;
    bool mByAddr = false;         // netx: the room is hosted by address (Online > Play by Address): MP_CONN_DIRECT, no account
    int mPendingItem = 0;         // netx: a service row chosen before the online account existed: chosen again once registered
    MpSettings mSettings;
    uint8_t mVisibility = 0;
    uint8_t mFalls = 0;   // placeholder until MpSettings.downed lands (bubble agent): 0 bubbles (default), 1 infinite lives
    SaveKind mSaveKind = SAVE_NONE;
    int mCoopSlot = 0;
    int mPendingSlot = 0;
    // flow
    uint32_t mRejoinUntil = 0;         // the Rejoin row is offered until this frame (0 none)
    char mRejoinHost[MP_NAME_BYTES + 1] = {};
    // flow (one door)
    char mCodeText[28] = {};           // uxfix #1: the code the player typed in Join with a Code (kept when it failed: the keyboard reopens with it)
    bool mQuickRegather = false;       // uxfix #3: Play with Anyone was chosen on the gathering screen: when it fails or is cancelled, the gathering starts again
    bool mCodeKeep = false;            // uxfix #1: the last code failed: keep mCodeText for the next keyboard
    bool mRoom = false;                // lobby: the gathering screen shows a core room (not the gathering driver's): Start/leave go to mp_ui_*
    bool mByCode = false;              // the gathering screen was reached with a friend's code (failure: "can't reach you")
    bool mFileChosen = false;          // Change File chose the file already: Start goes straight on
    bool mFileOnly = false;
    bool mCodeWait = false;            // Invite from Far Away: the code is on its way ("Getting your code...")            // the file screens were opened by Change File (no Start after them)
    int mAfterChars = 0;               // A_GATHER / A_CODE: where the first character select goes on to
    uint32_t mGatherAt = 0;            // the frame the gathering began (the nobody-found line comes after a while)
    uint8_t mGatherLogged = 0xFF;      // the last state logged
    mutable MpGatherStatus mGatherCache = {};
    mutable bool mGatherCached = false;
    mutable uint32_t mHeadIds[4] = {};   // the id each head layout slot shows (0 free)
    enum { kNoted = 8, kCodeCache = 8 };
    char mNoted[kNoted][MP_NAME_BYTES + 1] = {};   // names seen this session (each noted once)
    int mNotedCount = 0;
    char mSessionNames[4][MP_NAME_BYTES + 1] = {};   // the session's first four players (coop.cfg)
    uint8_t mSessionLooks[4] = {};
    int mSessionCount = 0;
    int mLastGalaxy = -1;
    mutable bool mStarsValid = false;
    mutable int16_t mCoopStarsCache[6] = {}, mSpStarsCache[6] = {};
    int mPlayCoop = 0;                 // the co-op file this console's session plays on (original host), 0 none
    struct CodeCache { char name[MP_NAME_BYTES + 1]; char code[12]; };
    CodeCache mCodeCache[kCodeCache] = {};   // friend codes the core told this boot (RAM only: Played With > Add)
    char mFriendCodeText[16] = {};   // M1: the code last typed in Add a Friend; it fills the keyboard again until an add works (a typo is fixed, not typed again)
    char mFriendAdding[16] = {};   // fix3: the friend code being added (its "added" window waits for the service's answer)
    int mBoardGalaxy = 0, mBoardStar = 1, mBoardTab = 0;
    uint32_t mBoardDirtyAt = 0;
    bool mBoardDirty = false;
    // waits
    int mBusy = 0;
    uint32_t mBusyStart = 0;
    int mAfterRegister = 0;
    int mDlg = 0, mDlgParam = 0;
    int mKb = 0;
    char mLastDialogId[24] = {0};
    char mKbText[64] = {0};
    // session watch
    bool mWasActive = false, mWasPlaying = false, mLeavingByUs = false, mOriginalHost = false, mWasLobby = false;
    bool mReconnecting = false;   // fix2: the core rejoins on its own (MP_UI_RECONNECTING): still in the session
    MpRulesLogic::Watch mRules;   // touches: the host-rules card
    void rulesTick(bool inSession);
    void openPick(int row);
    bool bindTo(const std::string& moveId, unsigned slot, const std::string& button);
    void finishSettings(bool toast);
    void restartNow();      // setfix31b: the host relaunch now (a relaunch that returns: the "close the game" window)
    void cancelRestart();   // setfix31b: Later: no restart; the saved change applies at the next start
    void settingsClosed();        // settingsnative: the Settings menu is gone (NativeMenuOpenSet(false), "settings: closed")
    bool mSettingsOn = false;     // settingsnative: the open menu is the Settings (no session, no network)
    uint32_t mClosedAt = 0;       // fix2: the frame the menu last closed (the Start that closed it must not open it again)
    bool mCoopContinue = false;   // fix2: the chosen co-op file exists (Continue) or not (New)
    bool mRootPending = false, mRootWarned = false;   // fix2: back to the single-player save root not done yet
    uint32_t mRootSince = 0, mRootTry = 0;
    bool mReconnectLong = false;            // uxfix #2: the sticky line already says "Still trying..."
    uint32_t mReconnectAt = 0;
    static constexpr uint32_t kReconnectLongFrames = 10 * 60;
    MpText mEndedText = MP_T_NONE;          // uxfix #2: the words chosen for the waiting session end
    int mEndedPending = 0, mEndedErr = 0;   // fix2: a session end (and its error) that waits for the window already up
    bool mPresent[MP_MAX_PLAYERS] = {false, false, false, false};
    char mNames[MP_MAX_PLAYERS][MP_NAME_BYTES + 1] = {};
    int mHostSlot = -1;
    uint16_t mEpoch = 0;
    int mHostLeftSlot = -1;
    uint32_t mHostLeftAt = 0;
    int mOthersBefore = 0;
    bool mTipShown = false;
    bool mTipWanted = false;       // conntest: the Wi-Fi power-save tip is only for a weak or laggy link (a test or a session measured it)
    bool mCtActive = false, mCtReported = false, mOfferTest = false;   // conntest: a run belongs to the open screen; its verdict shown; offer after the error window
    // characters, saves
    int mLook = MP_LOOK_MARIO;          // the last wish (preselected from mp.cfg at init)
    bool mCharWaiting = false;
    uint32_t mCharAskedAt = 0, mCharTakenAt = 0;
    MpSaveGame::LeaveOffer mLeave = {};
    MpSaveGame::RecoveryInfo mRecovery = {};
    // requests
    int mLaunchFile = 0;
    bool mManual = false, mDome = false;
    int mWarp = -1;
    // beta diagnostics (MpMenuReport.cpp)
    int mRpWhere = 0, mRpKind = 0;      // where the flow was opened from; 0 user / 1 after_error
    char mRpNote[4 * 200 + 8] = {};      // the player's note (UTF-8, at most 200 characters)
    uint32_t mRpStart = 0;              // the frame the send / check began
    bool offerReportAfterError();       // the Multiplayer menu opened: "Something went wrong last time. Send a report?" once
    void reportFlow();                  // account, then the consent pages
    void reportSend();
    void reportFinished(int result, const char* id);
    void checkFinished(const MpDiagCheck& c);
};

Sequence& sequence();   // the one the game uses (MpUiSystem.cpp)
void setProblemReportHook(void (*fn)(const char* where));   // betadiag: the report module sets MpReport::open here; until then the Settings row is hidden
bool problemReportHooked();
void settingsBootToast();   // settingsnative: a start that ran in a hold says so, once (MpMenuSettings.cpp; the entry screen calls it)

}  // namespace MpMenu

// The overlays: toasts, the waiting screen, the race results card (MpToast.cpp). Pure code.
namespace MpOverlay {
void reset();
void toast(uint16_t textId, int slot, uint32_t arg);     // queued; names looked up now (mp_slot_name)
void toastText(const char16_t* text, uint8_t icon);
void toastPick(int slot, const char16_t* galaxy, int star);   // touches: "<slot> picked <galaxy>, Star <star>."
void sticky(const char16_t* text, uint8_t icon);           // fix2: a toast that stays until sticky(nullptr) ("Reconnecting...")
void showWaiting(bool on, int done, int total, int secondsLeft);
void waitBanner(int slot);                                 // uxwait: "Waiting for <name>..." (slot < 0 hides it)
bool waitBannerShowing();
void hideWaiting(bool hidden);                             // spectate: keep the waiting panel off while a teammate's view is open
void setStanding(int slot, uint32_t turns);
bool showResults(const MpMsg& m);                         // false: a bad payload (dropped)
void update(bool okPressed, bool hold = false);            // once a frame (hold: the session is being rejoined: the results card's idle waits stand still)
bool resultsShowing();
bool waitingShowing();
int toastCount();
const MpPanel::Toast* toasts();
bool buildWaiting(MpPanel::Model* m);                     // false = nothing to show
bool buildResults(MpPanel::Model* m);
// race: the results card with choices (a RESULTS with a tail) and the head start line; see MpToast.cpp
void setResultsChoice(int choice);                        // MpSeriesLogic::Choice: the host chose (shown on every console)
void forgetSeriesNames();                                 // a new session: the names kept for greyed lines go
void dismissResults();                                    // the session moved on: the card goes
void resultsInput(bool up, bool down, bool ok, bool isHost);   // the host's keys on the card
bool takeResultsChoice(int* choice);                      // the host pressed a choice (once)
bool resultsHaveChoices();
bool resultsPoint(int row);                              // smallfix: a finger is on this row of the card: the leader's cursor follows; true = this player can press it
bool takeResultsLeave();                                 // smallfix: a player who is not the leader pressed Leave Race (once)
bool takeResultsAsk();                                   // lobby: a guest pressed Race Again? (once per card): the caller sends the ask (MpSeriesLogic::kAskPhrase)
void resultsAskFailed();                                  // lobby: the ask could not be sent (chat rate limit): the row is live again
bool noteAskSentByHand();                                 // menus4: a guest sent the ask phrase by hand while their results card is up: it IS the ask (the card shows Asked!); false: not on such a card or already asked
void noteRaceAgainAsk(int slot);                          // lobby: slot's ask arrived: the leader's card shows a nudge (once per player per card)
void showHeadStart(int slot, int seconds);                // seconds <= 0 hides
bool headStartShowing();
bool buildHeadStart(MpPanel::Model* m);
// The team star card (MpUi.hpp MpStarCard): a window alone like the results, auto-dismissed after kStarCardFrames, no button.
constexpr int kStarCardFrames = 6 * 60;
void showStarCard(const MpStarCard& card);
void dismissStarCard();
bool starCardShowing();
bool buildStarCard(MpPanel::Model* m);                    // false = nothing to show
}  // namespace MpOverlay
