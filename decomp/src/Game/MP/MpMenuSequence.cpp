// Galaxy 3.0 multiplayer: the menus' state machine (see MpMenuSequence.hpp). Pure code: mp_bridge.h, MpSaveGame, MpMenu::Io.
#include "Game/MP/MpMenuSequence.hpp"
#include "MpMenuInternal.hpp"   // flow split: the families' shared internals

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Game/MP/MpChat.hpp"        // MP3 chat: the Messages page, typed messages, mute
#include "Game/MP/MpChatLogic.hpp"
#include "Game/MP/MpSaveGate.hpp"
#include "Game/MP/MpRulesLogic.hpp"   // MP3 touches: the host-rules card
#include "Game/MP/MpUi.hpp"
#include "port/mp_core_game.h"  // MP3: the character remembered in mp.cfg
#include "port/mp_ui_flow.h"    // flow: net info, rejoin, flow.cfg / coop.cfg

extern "C" bool mp_update_take_notice(void) __attribute__((weak));   // release3: port/mp_update.h (platform side: not in the game's include path)

namespace MpMenu {
namespace {

Sequence sSequence;
const char* const kScreenNames[S_COUNT] = {"none", "main", "save", "slot", "friends", "board", "busy",
                                           "session", "warp", "messages", "players", "chars", "requests", "request", "blocked",
                                           "spfile", "fileuse", "player", "played", "conntest", "keep", "keeplist", "keepcopy",
                                           "gather", "falls", "help", "report", "rpwait",
                                           "settings", "settings_more", "settings_controls", "settings_buttons", "settings_pick", "settings_leave", "settings_camera", "settings_capture", "settings_restart"};

}  // namespace

const char* screenName(Scr s) { return s < S_COUNT ? kScreenNames[s] : "?"; }
Sequence& sequence() { return sSequence; }

void Sequence::init(Io* io) {
    *this = Sequence();   // (everything back to its start: a fresh system, or a test)
    mIo = io;
    mDepth = 0;
    sIncomingCount = 0, sBlockedCount = 0;   // friendsui
    memset(&sReq, 0, sizeof sReq);
    DefaultSettings(&mSettings, MP_MODE_TOGETHER);
    const int saved = static_cast<int>(mp_core_saved_look());   // the character picked last time is the one preselected
    if (saved >= 0 && saved < MP_LOOK_COUNT) mLook = saved;
}

void Sequence::push(Scr s, uint8_t param) {
    // flow (review 2 #2): the file / gather screens read the save files once, here (and again after a write), never per frame
    if (s == S_SAVE || s == S_SPFILE || s == S_FILEUSE || s == S_SLOT || (s == S_GATHER && !mStarsValid)) refreshStars();
    if (mDepth >= kStack) {
        mp_log("ui: menu stack full at %s", screenName(s));
        return;
    }
    mStack[mDepth++] = {s, 0, 0, param};
    // the first choosable row
    int it[kItems];
    const int n = items(s, param, it);
    top().cursor = n > 0 ? 0 : -1;
    mPulse = 0;
    mp_log("ui: screen %s", screenName(s));
}
void Sequence::pop() {
    const bool leavingSet = mSettingsOn && mDepth > 0 && top().scr == S_SET_MAIN;   // settingsnative: the Settings' own first screen (over the session's menu too)
    if (mDepth > 0) --mDepth;
    if (mDepth == 0) mClosedAt = mFrame;
    if (mDepth == 0) mStarsValid = false;
    if (!gathering()) mGatherCached = false;
    if (mDepth == 0) mp_log("ui: menu closed");
    if (leavingSet) settingsClosed();   // settingsnative
    if (mDepth > 0) mp_log("ui: screen %s", screenName(top().scr));
}
void Sequence::popTo(Scr s) {
    while (mDepth > 0 && top().scr != s) --mDepth;
    if (mDepth > 0) mp_log("ui: screen %s", screenName(top().scr));
}
// After an error or an ended session: the nearest screen that works without a session.
void Sequence::cutToSafe() {
    if (gathering() && !mRoom) {   // flow (one door): the gathering screen goes: this console stops being findable
        mp_gather_stop();
        mp_log("ui: gathering stopped");
    }
    mRoom = false;   // (lobby: a room the core held; whoever called this has left it already)
    mGatherCached = false;
    while (mDepth > 0) {
        const Scr s = top().scr;
        if (s == S_MAIN) break;   // (one door: the Multiplayer menu)
        --mDepth;
    }
    if (mDepth == 0 && mFromFileSelect) push(S_MAIN);
    if (mDepth > 0) mp_log("ui: screen %s", screenName(top().scr));
}
void Sequence::demoShow(Scr s, uint8_t param) {   // MP3 demo
    mCharWaiting = false;
    push(s, param);
}
void Sequence::demoBack() {   // MP3 demo
    mCharWaiting = false;
    pop();
}
void Sequence::close() {
    if (mDepth > 0) mp_log("ui: menu closed");
    if (mDepth > 0) mClosedAt = mFrame;
    mDepth = 0;
    mStarsValid = false;
    mGatherCached = false;
    if (mSettingsOn) settingsClosed();   // settingsnative
}

bool Sequence::isBusy() const { return mDepth > 0 || mDlg != 0 || mKb != 0; }

int Sequence::items(Scr s, uint8_t param, int* out) const {
    int n = 0;
    if (!(itemsMain(s, param, out, &n) || itemsGather(s, param, out, &n) || itemsChars(s, param, out, &n) || itemsSession(s, param, out, &n) || itemsFiles(s, param, out, &n) || itemsFriends(s, param, out, &n) || itemsConnTest(s, param, out, &n) || itemsRules(s, param, out, &n) || itemsReport(s, param, out, &n) || itemsSettings(s, param, out, &n))) n = 0;   // (each family lists its own screens' rows; a screen nobody lists has none)
    return n;
}

int Sequence::itemCount() const {
    if (mDepth == 0) return 0;
    int it[kItems];
    return items(top().scr, top().param, it);
}

int Sequence::othersPresent() const {
    int n = 0;
    for (int k = 0; k < MP_MAX_PLAYERS; k++)
        if (k != mp_local_slot() && mp_slot_present(k)) ++n;
    return n;
}

// ------------------------------------------------------------------ windows and keyboard
void Sequence::ask(int purpose, MpText text, const Args& a, Dlg kind, int param) {
    mDlg = purpose;
    mDlgParam = param;
    const bool dynamic = a.str || a.str2 || a.num || a.turns || !txt::dialogName(text);
    const char* id = "MP_DYN0";
    if (dynamic) txt::setDynamic(0, text, a);
    else id = txt::dialogName(text);
    strncpy(mLastDialogId, txt::dialogName(text) ? txt::dialogName(text) : id, sizeof mLastDialogId - 1);
    mp_log("ui: window %s%s", mLastDialogId, kind == Dlg::YesNo ? " (yes/no)" : "");
    if (mIo) mIo->dialogOpen(kind, id);
}

void Sequence::openKeyboard(int purpose, MpText title, const char* initial, int maxChars, bool codeOnly) {
    // (the keyboard's title is UTF-8)
    char16_t t16[64];
    txt::format(t16, 64, title, Args());
    char t8[160];
    int n = 0;
    for (const char16_t* p = t16; *p && n < 150; ++p) {
        const uint32_t c = *p;
        if (c < 0x80) t8[n++] = static_cast<char>(c);
        else if (c < 0x800) t8[n++] = static_cast<char>(0xC0 | (c >> 6)), t8[n++] = static_cast<char>(0x80 | (c & 0x3F));
        else t8[n++] = static_cast<char>(0xE0 | (c >> 12)), t8[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F)),
             t8[n++] = static_cast<char>(0x80 | (c & 0x3F));
    }
    t8[n] = 0;
    if (!mp_keyboard_open(t8, initial ? initial : "", maxChars, codeOnly)) {
        mp_log("ui: keyboard could not open");
        showError(MP_ERR_INTERNAL);
        return;
    }
    mKb = purpose;
    mp_log("ui: keyboard open purpose=%d", purpose);
}

void Sequence::showError(int err) {
    if (err == MP_ERR_NONE) err = MP_ERR_INTERNAL;
    // netx: by address, a timeout means nobody answered at that address: say what to check
    const bool addrTimeout = mBusy == B_ADDR && err == MP_ERR_TIMEOUT;
    // conntest: a join or host that failed for a connection reason: "Run a Connection Test?" follows the error window
    const bool connecting = mBusy == B_HOST || mBusy == B_JOIN_CODE || mBusy == B_QUICK || mBusy == B_FRIEND || mBusy == B_LOCAL || mBusy == B_ADDR ||
                            mBusy == B_SIGNIN;   // 13 Nov: the silent sign-in that Play with Anyone / Friends need is the commonest way a missing connection shows: the offer follows it too
    mOfferTest = connecting && !addrTimeout && mp_conntest_start &&
                 (err == MP_ERR_NO_WIFI || err == MP_ERR_SERVICE_DOWN || err == MP_ERR_TIMEOUT || err == MP_ERR_NAT);
    MpUiStatus st;
    mp_ui_status(&st);
    if (st.phase == MP_UI_FAILED) mp_ui_leave();   // (always back to IDLE)
    mBusy = B_NONE;
    if (mDepth > 0) cutToSafe();
    noteErrorShown(err);
    if (mIo) mIo->sound(Se::Buzzer);
    say(P_ERROR, addrTimeout ? MP_T_DLG_ADDR_NO_ANSWER : txt::errorText(err));
}

int Sequence::failError(int fallback) const {
    MpUiStatus st;
    memset(&st, 0, sizeof st);
    mp_ui_status(&st);
    return st.error != MP_ERR_NONE && st.error <= MP_ERR_INTERNAL ? static_cast<int>(st.error) : fallback;
}

void Sequence::noteErrorShown(int err) {
    const char* name = txt::dialogName(txt::errorText(err));
    mp_log("ui: error shown %d (%s)", err, name ? name : "?");
}

void Sequence::showServiceError(int err, MpText fallback) {
    const char* code = mp_service_last_code ? mp_service_last_code() : nullptr;
    MpText t = txt::serviceText(code);
    if (t == MP_T_NONE && fallback == MP_T_NONE) {
        showError(err);
        return;
    }
    if (t == MP_T_NONE) t = fallback;
    if (err == MP_ERR_NONE) err = MP_ERR_INTERNAL;
    // a friend request that failed: back on the friend list (no session was involved); else as showError
    const bool friendOp = mBusy == B_FRIEND_ADD || mBusy == B_ACCT_DELETE || (mDepth > 0 && top().scr == S_FRIENDS);
    MpUiStatus st;
    mp_ui_status(&st);
    if (st.phase == MP_UI_FAILED && !friendOp) mp_ui_leave();   // (always back to IDLE)
    if (friendOp) {
        if (mDepth > 0 && top().scr == S_BUSY) pop();
    } else if (mDepth > 0) {
        cutToSafe();
    }
    mBusy = B_NONE;
    const char* name = txt::dialogName(t);
    mp_log("ui: error shown %d (%s) code=%s", err, name ? name : "?", code && *code ? code : "-");
    if (mIo) mIo->sound(Se::Buzzer);
    say(P_ERROR, t);
}

void Sequence::startBusy(int purpose) {
    if (purpose != B_ADDR && purpose != B_HOST) mByAddr = false;   // (flow: a join by address sets it; Host sets it from "Who can join")
    mBusy = purpose;
    mBusyStart = mFrame;
    mLeavingByUs = false;
    push(S_BUSY, static_cast<uint8_t>(purpose));
}

// A row was chosen: the family that owns the item acts (each returns false for items that are not its own).
void Sequence::choose(int item) {
    if (chooseSettings(item) || chooseMain(item) || chooseGather(item) || chooseChars(item) || chooseSession(item) || chooseFiles(item) || chooseFriends(item) || chooseConnTest(item) || chooseRules(item) || chooseReport(item)) return;
}
void Sequence::moveCursor(int dir, int count) {
    if (count <= 0 || mDepth == 0) return;
    Frame& f = top();
    int c = f.cursor < 0 ? 0 : f.cursor + dir;
    if (c < 0) c = count - 1;
    if (c >= count) c = 0;
    if (c != f.cursor && mIo) mIo->sound(Se::Cursor);
    f.cursor = static_cast<int8_t>(c);
    fitScroll(f);
    mPulse = 1.0f;
}

// uinative: a list shows MpPanel::kListRows big rows at a time and turns a whole page when the cursor leaves it (the star list's
// page number shows which); the other screens show every row (big buttons, heads, the players).
int Sequence::visibleRows(Scr s) {
    switch (s) {
    case S_SAVE: case S_SPFILE: case S_SLOT: case S_FRIENDS: case S_REQUESTS: case S_BLOCKED: case S_BOARD: case S_PLAYED:
    case S_SET_BUTTONS: case S_SET_PICK: case S_SET_MORE: case S_SET_CONTROLS: case S_SET_CAMERA:   // settingsnative: the button list and its picks: pages of three
    case S_KEEPLIST: case S_KEEPCOPY:   // menus4: up to 12 copies and 10 items: pages of three, as the others (eight rows shrank the words to 18.8)
        return MpPanel::kListRows;
    default: return MpPanel::kMaxRows;
    }
}

void Sequence::fitScroll(Frame& f) {
    const int vis = visibleRows(f.scr);
    if (f.cursor < 0) {
        if (f.scroll < 0) f.scroll = 0;
        return;
    }
    if (vis == MpPanel::kListRows) {
        f.scroll = static_cast<int8_t>((f.cursor / vis) * vis);
    } else {
        if (f.cursor < f.scroll) f.scroll = f.cursor;
        if (f.cursor >= f.scroll + vis) f.scroll = static_cast<int8_t>(f.cursor - vis + 1);
    }
}

// ------------------------------------------------------------------ answers
void Sequence::onDialog(int purpose, bool yes, int param) {
    mp_log("ui: window answer %s", yes ? "yes" : "no");
    if (dialogMain(purpose, yes, param) || dialogGather(purpose, yes, param) || dialogChars(purpose, yes, param) || dialogSession(purpose, yes, param) || dialogFiles(purpose, yes, param) || dialogFriends(purpose, yes, param) || dialogConnTest(purpose, yes, param) || dialogRules(purpose, yes, param) || dialogReport(purpose, yes, param) || dialogSettings(purpose, yes, param)) return;
}

void Sequence::onKeyboard(int purpose, bool done, const char* text) {
    mp_log("ui: keyboard %s purpose=%d", done ? "done" : "cancelled", purpose);
    if (!done || !text || !text[0]) {
        if (mIo) mIo->sound(Se::Cancel);
        return;
    }
    if (keyboardMain(purpose, text) || keyboardGather(purpose, text) || keyboardChars(purpose, text) || keyboardSession(purpose, text) || keyboardFiles(purpose, text) || keyboardFriends(purpose, text) || keyboardConnTest(purpose, text) || keyboardRules(purpose, text) || keyboardReport(purpose, text) || keyboardSettings(purpose, text)) return;
}

void Sequence::sessionEnded(bool byUs, int err) {
    mp_log("ui: session ended by=%s err=%d played=%d", byUs ? "us" : "them", err, mWasPlaying ? 1 : 0);
    // flow: a drop (the link went and the core gave up rejoining on its own): "Rejoin" on top of the Multiplayer menu for 2 minutes
    // uxfix #2: and the window says so: the connection was lost, and the way back in. (share/core/RECONNECT.md: after the core's own
    // reconnecting window a silent link ends as MP_ERR_NO_WIFI = "connection lost"; a sleep or Wi-Fi loss ends the same way.)
    // TODO(core): when the core gets a distinct "connection lost" reason, test for it in connectionLost() below.
    bool dropped = false;
    if (!byUs && connectionLost(err) && mp_ui_rejoin) {
        dropped = true;
        mRejoinUntil = mFrame + kRejoinFrames;
        const int h = mHostSlot >= 0 && mHostSlot < MP_MAX_PLAYERS ? mHostSlot : 0;
        snprintf(mRejoinHost, sizeof mRejoinHost, "%s", mNames[h]);
        mp_log("ui: rejoin offered (%s)", mRejoinHost);
    }
    // uxfix #2: this console left because its data did not match the session's (MpSaveGame): the plain sentence, never the generic one
    const bool mismatch = MpSaveGame::takeLeftForMismatch();
    const bool played = mWasPlaying;
    mWasPlaying = false;
    mLeavingByUs = false;
    mCharWaiting = false;
    if (byUs) {
        if (played) offerSave();
        else if (!mFromFileSelect) close();
        return;
    }
    if (mDepth > 0) {
        if (mFromFileSelect) cutToSafe();
        else close();
    }
    MpText t = err != MP_ERR_NONE ? txt::errorText(err) : MP_T_DLG_EVERYONE_LEFT;
    if (dropped) t = MP_T_ERR_DROPPED;
    if (mismatch) t = MP_T_DLG_DATA_MISMATCH;
    if (mDlg) {
        // (a window is up already: it closes first, then this one: the reason is never lost)
        mEndedPending = played ? 2 : 1;
        mEndedErr = err;
        mEndedText = t;
        return;
    }
    if (mIo) mIo->sound(Se::Buzzer);
    if (err != MP_ERR_NONE) noteErrorShown(err);
    ask(P_ENDED, t, Args(), Dlg::Ok, played ? 1 : 0);
}

// uxfix #2: the session ended because the link went (a silent host, Wi-Fi lost, a resume from sleep), not because anyone left or was removed
bool Sequence::connectionLost(int err) { return err == MP_ERR_NO_WIFI || err == MP_ERR_TIMEOUT || err == MP_ERR_NAT; }

void Sequence::watchSession() {
    MpUiStatus st;
    memset(&st, 0, sizeof st);
    mp_ui_status(&st);
    const bool active = mp_active();
    // fix2: a silent rejoin (the link went: Wi-Fi drop, sleep) is still the session: a small "Reconnecting..." toast, nothing
    // else; it ends only when the core gives up (IDLE with an error) or the player leaves
    const bool reconnecting = !active && st.phase == MP_UI_RECONNECTING && mWasActive;
    const bool inSession = active || reconnecting || st.phase == MP_UI_LOBBY || st.phase == MP_UI_PLAYING;
    const int local = mp_local_slot();
    if (mRoom && !gathering()) mRoom = false;   // lobby: the room's gathering screen is gone
    if (reconnecting != mReconnecting) {
        mReconnecting = reconnecting;
        mReconnectLong = false;
        mReconnectAt = mFrame;
        if (reconnecting) {
            char16_t t[96];
            txt::format(t, 96, MP_T_RECONNECTING, Args());
            MpOverlay::sticky(t, 0);
        } else {
            MpOverlay::sticky(nullptr, 0);
        }
        // (whole literals, so the scenario-regex suite and a grep can find each line)
        if (reconnecting) mp_log("ui: reconnecting");
        else if (active) mp_log("ui: reconnected");
        else mp_log("ui: reconnect over");
    }
    if (reconnecting && !mReconnectLong && mFrame - mReconnectAt > kReconnectLongFrames) {   // uxfix #2: after 10 s the line says it is still going
        mReconnectLong = true;
        char16_t t[96];
        txt::format(t, 96, MP_T_RECONNECT_LONG, Args());
        MpOverlay::sticky(t, 0);
    }
    if (reconnecting) return;   // (the roster reads empty meanwhile: no "left" / "joined" toasts, no host change)
    // flow (one door): while the gathering screen is up and the game has not started, rooms open, close and merge under it: the heads
    // show who is there (no "joined" / "left" toasts) and a room closing into another is no ending (the screen watches the gathering)
    if (gathering() && !mRoom && st.phase != MP_UI_PLAYING) {
        mWasActive = false;
        return;
    }

    if (inSession && mWasActive) {
        // host migration
        const uint16_t epoch = mp_host_epoch();
        const int host = mp_host_slot();
        if (epoch != mEpoch) {
            if (host == local) {
                char16_t t[96];
                txt::format(t, 96, MP_T_YOU_ARE_HOST, Args());
                MpOverlay::toastText(t, LookIcon(local));
            } else if (host >= 0 && host < MP_MAX_PLAYERS) {
                Args a;
                a.str = mp_slot_name(host);
                a.str2 = mHostSlot >= 0 && mHostSlot < MP_MAX_PLAYERS ? mNames[mHostSlot] : "";
                char16_t t[96];
                txt::format(t, 96, MP_T_HOST_MIGRATED, a);
                MpOverlay::toastText(t, LookIcon(host));
            }
            mp_log("ui: host is now slot %d (epoch %u)", host, static_cast<unsigned>(epoch));
            if (mHostLeftSlot >= 0) mHostLeftSlot = -1;   // (the migration toast says it)
        }
        // arrivals and departures
        for (int k = 0; k < MP_MAX_PLAYERS; k++) {
            const bool now = mp_slot_present(k);
            // lobby: on the gathering screen the heads say who came and who went (no toast on top of them)
            if (k != local && now != mPresent[k] && !gathering()) {
                if (now) {
                    MpOverlay::toast(MP_T_JOINED, k, 0);
                    if (mIo) mIo->sound(Se::Join);   // MP3 touches: a chime
                } else if (k == mHostSlot && epoch == mEpoch) {
                    mHostLeftSlot = k;   // (wait a moment: a migration toast may come instead)
                    mHostLeftAt = mFrame;
                } else if (!(k == mHostSlot && epoch != mEpoch)) {
                    Args a;
                    a.str = mNames[k];
                    char16_t t[96];
                    txt::format(t, 96, MP_T_LEFT, a);
                    MpOverlay::toastText(t, 0);
                    if (mIo) mIo->sound(Se::Leave);   // MP3 touches
                }
            }
        }
        if (mHostLeftSlot >= 0 && mFrame - mHostLeftAt > kFps) {
            Args a;
            a.str = mNames[mHostLeftSlot];
            char16_t t[96];
            txt::format(t, 96, MP_T_LEFT, a);
            MpOverlay::toastText(t, 0);
            if (mIo) mIo->sound(Se::Leave);   // MP3 touches
            mHostLeftSlot = -1;
        }
    }
    if (inSession) {
        mEpoch = mp_host_epoch();
        mHostSlot = mp_host_slot();
        int others = 0;
        for (int k = 0; k < MP_MAX_PLAYERS; k++) {
            mPresent[k] = mp_slot_present(k);
            if (mPresent[k]) {
                strncpy(mNames[k], mp_slot_name(k), MP_NAME_BYTES);
                mNames[k][MP_NAME_BYTES] = 0;
                if (k != local) ++others;
            }
        }
        mOthersBefore = others;
    }

    rulesTick(inSession && mWasActive);

    // the session starts: the file select loads the game
    if (st.phase == MP_UI_PLAYING && !mWasPlaying) {
        mWasPlaying = true;
        mNotedCount = 0, mSessionCount = 0;   // flow: who plays in this session (Played With, the co-op file's names)
        mLastGalaxy = -1;
        mp_log("ui: session playing (host=%d)", mp_is_host() ? 1 : 0);
        if (mBusy != B_NONE && mBusy != B_FRIEND_ADD && mBusy != B_SIGNIN) {
            // fix2: straight into a running room (no lobby): the wait ends here; the original-host flag from the core
            mBusy = B_NONE;
            mOriginalHost = mp_is_host();
            mp_log("ui: in the room (%s)", mOriginalHost ? "host" : "guest");
        }
        if (mFromFileSelect && mDepth > 0) {
            int file = mSaveKind == SAVE_MINE ? mSpFile : mCoopSlot;
            if (!mOriginalHost) {
                // guests: the multiplayer root (never their single-player file); the snapshot goes to the session slot
                if (!MpSaveGame::enterSessionRoot()) {
                    // flow (review 2 #9): never play a session on the single-player root: leave, say so, back to a usable screen
                    mp_log("ui: launch refused: the multiplayer save could not be opened");
                    mLeavingByUs = true;
                    mp_ui_leave();
                    mWasPlaying = false, mWasActive = false;
                    cutToSafe();
                    if (mIo) mIo->sound(Se::Buzzer);
                    say(P_INFO, MP_T_DLG_SAVE_FAILED);
                    return;
                }
                file = MpSaveGame::sessionSlot();
            } else if (!MpSaveGame::launchReady() && !prepareHostSave()) {
                // savefix (review finding 2): a host straight into a running room (Quick Match) plays only on a save it chose
                mp_log("ui: launch refused: no save to play on (leaving)");
                mLeavingByUs = true;
                mp_ui_leave();
                mWasPlaying = false, mWasActive = false;
                cutToSafe();
                if (mIo) mIo->sound(Se::Buzzer);
                say(P_INFO, MP_T_DLG_SAVE_FAILED);
                return;
            }
            if (file < 1 || file > 6) file = 1;
            mLaunchFile = file;
            mp_log("ui: launch file=%d", file);
            close();
        }
    }
    // flow: while playing: who is here (Played With, coop.cfg) and the galaxy (coop.cfg), now and then
    if (mWasPlaying && active && mFrame % 60 == 0) notePlayers();
    if (mWasPlaying && active && mFrame % 300 == 0 && mIo) {
        const int g = mIo->currentGalaxy();
        if (g >= 0 && g != mLastGalaxy) {
            mLastGalaxy = g;
            if (mPlayCoop > 0) writeCoopMeta(mPlayCoop);
        }
    }
    if (mWasActive && !inSession) {
        mWasActive = false;
        if (mPlayCoop > 0) writeCoopMeta(mPlayCoop);
        mPlayCoop = 0;
        sessionEnded(mLeavingByUs, st.error);
    }
    if (inSession) mWasActive = true;
}


// ------------------------------------------------------------------ once a frame
void Sequence::update(const Input& in) {
    ++mFrame;
    mPulse *= 0.8f;
    if (mPulse < 0.01f) mPulse = 0;
    if (mDepth > 0) mFade = mFade + 1.0f / 8.0f > 1.0f ? 1.0f : mFade + 1.0f / 8.0f;

    watchSession();
    if (gathering()) refreshGatherCache();
    else mGatherCached = false;

    // release3: "A newer version of the game is available." once per boot, when the online service said so (a toast, in any state)
    if (mp_update_take_notice && mp_update_take_notice()) {
        char16_t t[96];
        txt::format(t, 96, MP_T_UPDATE_AVAILABLE, Args());
        MpOverlay::toastText(t, 0);
        mp_log("ui: update notice shown");
    }

    // conntest: the run belongs to the open Connection Test screen: it is pumped while that screen is up (behind a window too) and
    // cancelled (the router mapping let go) the moment the screen is not the top one, whichever way it was left
    if (mCtActive) {
        if (mDepth > 0 && top().scr == S_CONNTEST) {
            if (mp_conntest_pump) mp_conntest_pump();
        } else {
            mCtActive = false;
            if (mp_conntest_cancel) mp_conntest_cancel();
        }
    }

    if (mKb) {
        char out[4 * 200 + 8];   // (chat: a message is up to 24 characters; a problem report note up to 200: of up to 4 bytes each)
        memset(out, 0, sizeof out);
        const int r = mp_keyboard_poll(out, sizeof out);
        if (r != 0) {
            const int p = mKb;
            mKb = 0;
            out[sizeof out - 1] = 0;
            onKeyboard(p, r > 0, out);
        }
        return;
    }
    if (mDlg) {
        const DlgState s = mIo ? mIo->dialogState() : DlgState::Yes;
        if (s == DlgState::Open) return;
        const int p = mDlg, param = mDlgParam;
        mDlg = 0;
        onDialog(p, s == DlgState::Yes, param);
        if (mEndedPending && !mDlg && !mKb && !mp_active()) {
            const int err = mEndedErr;
            const bool played = mEndedPending == 2;
            mEndedPending = 0;
            if (mIo) mIo->sound(Se::Buzzer);
            if (err != MP_ERR_NONE) noteErrorShown(err);
            ask(P_ENDED, mEndedText != MP_T_NONE ? mEndedText : (err != MP_ERR_NONE ? txt::errorText(err) : MP_T_DLG_EVERYONE_LEFT), Args(), Dlg::Ok, played ? 1 : 0);
            mEndedText = MP_T_NONE;
        } else if (mp_active()) {
            mEndedPending = 0;
        }
        return;
    }
    if (mDepth == 0) return;

    const Scr scr = top().scr;
    // ---- what the screen waits for
    {
        bool stop = false;
        if (tickMain(scr, in, &stop) || tickGather(scr, in, &stop) || tickChars(scr, in, &stop) || tickSession(scr, in, &stop) || tickFiles(scr, in, &stop) || tickFriends(scr, in, &stop) || tickConnTest(scr, in, &stop) || tickRules(scr, in, &stop) || tickReport(scr, in, &stop) || tickSettings(scr, in, &stop)) {
            if (stop) return;
        }
    }

    // ---- buttons
    int it[kItems];
    const int count = items(scr, top().param, it);
    const int cur = top().cursor;
    const int item = cur >= 0 && cur < count ? it[cur] : I_NONE;
    if ((in.up || in.down || in.left || in.right) && (pressMain(scr, in, item, count, 0) || pressGather(scr, in, item, count, 0) || pressChars(scr, in, item, count, 0) || pressSession(scr, in, item, count, 0) || pressFiles(scr, in, item, count, 0) || pressFriends(scr, in, item, count, 0) || pressConnTest(scr, in, item, count, 0) || pressRules(scr, in, item, count, 0) || pressReport(scr, in, item, count, 0) || pressSettings(scr, in, item, count, 0))) return;   // (grids)
    if (in.up) moveCursor(-1, count);
    else if (in.down) moveCursor(1, count);
    else if (in.left || in.right) changeValue(item, in.left ? -1 : 1);
    else if (in.back || (in.start && (scr == S_SESSION))) {
        const uint8_t param = top().param;
        if (leaveMain(scr, param, 0) || leaveGather(scr, param, 0) || leaveChars(scr, param, 0) || leaveSession(scr, param, 0) || leaveFiles(scr, param, 0) || leaveFriends(scr, param, 0) || leaveConnTest(scr, param, 0) || leaveRules(scr, param, 0) || leaveReport(scr, param, 0) || leaveSettings(scr, param, 0)) return;   // (a family asks first)
        if (mIo) mIo->sound(mDepth == 1 ? Se::Close : Se::Cancel);
        (void)(leaveMain(scr, param, 1) || leaveGather(scr, param, 1) || leaveChars(scr, param, 1) || leaveSession(scr, param, 1) || leaveFiles(scr, param, 1) || leaveFriends(scr, param, 1) || leaveConnTest(scr, param, 1) || leaveRules(scr, param, 1) || leaveReport(scr, param, 1) || leaveSettings(scr, param, 1));
        pop();
        (void)(leaveMain(scr, param, 2) || leaveGather(scr, param, 2) || leaveChars(scr, param, 2) || leaveSession(scr, param, 2) || leaveFiles(scr, param, 2) || leaveFriends(scr, param, 2) || leaveConnTest(scr, param, 2) || leaveRules(scr, param, 2) || leaveReport(scr, param, 2) || leaveSettings(scr, param, 2));
    } else if (in.ok) {
        if (item != I_NONE) {
            mDecidedRow = cur - (top().scroll < 0 ? 0 : top().scroll), mDecidedFrame = mFrame;   // uinative: the big button's decide
            choose(item);
        } else if (mIo) mIo->sound(Se::Buzzer);
    } else if (in.sq || in.tri) {
        (void)(pressMain(scr, in, item, count, 1) || pressGather(scr, in, item, count, 1) || pressChars(scr, in, item, count, 1) || pressSession(scr, in, item, count, 1) || pressFiles(scr, in, item, count, 1) || pressFriends(scr, in, item, count, 1) || pressConnTest(scr, in, item, count, 1) || pressRules(scr, in, item, count, 1) || pressReport(scr, in, item, count, 1) || pressSettings(scr, in, item, count, 1));
    }
}

void Sequence::pointAt(int row) {
    if (mDepth == 0 || mDlg || mKb || row < 0 || row == top().cursor) return;
    if (row >= itemCount()) return;
    top().cursor = static_cast<int8_t>(row);
    mPulse = 1.0f;
    if (mIo) mIo->sound(Se::Cursor);
}

bool Sequence::firstTapPoints(int visible) {
    if (visible < 0 || mDepth <= 0 || mDlg || mKb) return false;
    if (top().scr != S_MESSAGES && !(mSettingsOn && top().scr >= S_SET_MAIN && top().scr <= S_SET_CAPTURE)) return false;   // (settingsnative: the first tap on a row only selects it)
    const int row = (top().scroll > 0 ? top().scroll : 0) + visible;
    if (row == top().cursor) return false;   // (already on it: this tap sends it)
    pointAt(row);
    return true;
}

void Sequence::pointAtVisible(int i) {   // vitabtn: the i-th row shown (the list may have scrolled)
    if (i < 0) return;
    pointAt((mDepth > 0 && top().scroll > 0 ? top().scroll : 0) + i);
}

bool Sequence::takeLaunch(int* file) {
    if (mLaunchFile <= 0) return false;
    if (file) *file = mLaunchFile;
    mLaunchFile = 0;
    return true;
}
bool Sequence::takeManual() {
    const bool m = mManual;
    mManual = false;
    return m;
}
bool Sequence::takeWarp(int* slot) {
    if (mWarp < 0) return false;
    if (slot) *slot = mWarp;
    mWarp = -1;
    return true;
}
bool Sequence::takeDome() {
    const bool d = mDome;
    mDome = false;
    return d;
}


// ------------------------------------------------------------------ what the panel shows
void Sequence::buildModel(MpPanel::Model* m) const {
    using namespace MpPanel;
    clear(m);
    if (mDepth == 0) return;
    const Frame& f = top();
    m->alpha = mFade;
    m->dialogUp = mDlg != 0;   // uinative3: no hand under a native window's dim
    m->pulse = mPulse;
    int it[kItems];
    const int count = items(f.scr, f.param, it);
    auto button = [m](int pic, MpText label) { ModelButton(m, pic, label); };
    auto hint = [m](MpText t) { Text(m->hint, 80, t); };
    const int cursorItem = f.cursor >= 0 && f.cursor < count ? it[f.cursor] : I_NONE;
    const int vis = visibleRows(f.scr);
    const int first = f.scroll < 0 ? 0 : f.scroll;
    m->moreAbove = first > 0;
    m->moreBelow = count > first + vis;
    m->cursor = f.cursor >= 0 ? f.cursor - first : -1;
    if (vis == kListRows && count > vis) m->pages = (count + vis - 1) / vis, m->page = first / vis;
    // uinative: the screen's identity (a change ends its big buttons and brings the next ones in) and the big button just chosen
    m->screenKey = static_cast<uint16_t>(((mDepth & 15) << 12) | ((f.param & 15) << 8) | f.scr);
    m->decided = mDecidedFrame == mFrame && mDecidedFrame != 0 ? static_cast<int8_t>(mDecidedRow) : -1;

    auto label = [this, &f](int item, Row& r) { rowLabel(item, r, f); };

    // rows
    for (int i = first; i < count && m->rowCount < vis && m->rowCount < kMaxRows; i++) {
        Row& r = m->rows[m->rowCount++];
        r.ping = -1;
        label(it[i], r);
    }

    // per screen: title, hint, buttons, extras
    if (modelMain(m, f, cursorItem, count, it) || modelGather(m, f, cursorItem, count, it) || modelChars(m, f, cursorItem, count, it) || modelSession(m, f, cursorItem, count, it) || modelFiles(m, f, cursorItem, count, it) || modelFriends(m, f, cursorItem, count, it) || modelConnTest(m, f, cursorItem, count, it) || modelRules(m, f, cursorItem, count, it) || modelReport(m, f, cursorItem, count, it) || modelSettings(m, f, cursorItem, count, it)) return;
    (void)button, (void)hint;
}

// A row's words: the family that owns the item fills them in; a list row of players / friends is shared (rowLabelList).
void Sequence::rowLabel(int item, MpPanel::Row& r, const Frame& f) const {
    if (labelSettings(item, r, f) || labelMain(item, r, f) || labelGather(item, r, f) || labelChars(item, r, f) || labelSession(item, r, f) || labelFiles(item, r, f) || labelFriends(item, r, f) || labelConnTest(item, r, f) || labelRules(item, r, f) || labelReport(item, r, f)) return;
    rowLabelList(item, r, f);
}

void Sequence::rowLabelList(int item, MpPanel::Row& r, const Frame& f) const {
    using namespace MpPanel;
    if (item >= I_LIST0) {
        const int k = item - I_LIST0;
        if (f.scr == S_FRIENDS && k < sFriendCount) {
            txt::utf8To16(r.label, kLabel, sFriends[k].name);
            const MpFriend& fr = sFriends[k];
            r.icon = fr.joinable ? ICON_DOT_ROOM : (fr.online ? ICON_DOT_ONLINE : ICON_DOT_OFFLINE);
            Text(r.value, kValue, fr.joinable ? MP_T_IN_ROOM : (fr.online ? MP_T_ONLINE : MP_T_OFFLINE));
        } else if ((f.scr == S_REQUESTS && k < sIncomingCount) || (f.scr == S_BLOCKED && k < sBlockedCount)) {
            const MpFriend& fr = f.scr == S_REQUESTS ? sIncoming[k] : sBlocked[k];
            txt::utf8To16(r.label, kLabel, fr.name);
            txt::utf8To16(r.value, kValue, fr.code);
        } else if (f.scr == S_PLAYED) {
            const MpFlowStore fs = FlowStore();
            if (k < fs.playedCount) txt::utf8To16(r.label, kLabel, fs.playedWith[k]);
        } else if (f.scr == S_FRIENDS || f.scr == S_REQUESTS || f.scr == S_BLOCKED) {
            // (a stale row: nothing)
        } else if (k < MP_MAX_PLAYERS) {
            txt::utf8To16(r.label, kLabel, mp_slot_name(k));
            r.icon = LookIcon(k);
            if (k == mp_host_slot()) r.marks |= MARK_CROWN;
            if (k == mp_local_slot()) {
                r.marks |= MARK_YOU;
                Text(r.value, kValue, MP_T_YOU);
            } else {
                const int ms = mp_slot_ping_ms(k);
                r.ping = PingBars(ms);
                if (mp_slot_quiet(k)) r.marks |= MARK_LAG, Text(r.value, kValue, MP_T_LAG);
                // (review: bars only, never "N ms")
                if (MpChat::muted(k)) Text(r.value, kValue, MP_T_MUTED);   // chat
            }
        }
    }
}

}  // namespace MpMenu
