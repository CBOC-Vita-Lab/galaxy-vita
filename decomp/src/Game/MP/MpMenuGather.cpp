// Galaxy 3.0 multiplayer: the menus, the gathering / lobby family: the Join list, the wait, the room (lobby), Room Settings (flow split of MpMenuSequence.cpp, 11 Nov).
// Owner of this family's screens: see MpMenuSequence.cpp "the file map". Pure code, as MpMenuSequence.cpp.
#include "MpMenuInternal.hpp"
#include "port/mp_invite.h"   // code5

extern "C" bool mp_update_take_notice(void) __attribute__((weak));   // release3: port/mp_update.h (platform side: not in the game's include path)

namespace MpMenu {

// ------------------------------------------------------------------ waits
void Sequence::tickBusy() {
    MpUiStatus st;
    memset(&st, 0, sizeof st);
    mp_ui_status(&st);
    const uint32_t elapsed = mFrame - mBusyStart;
    if (mBusy == B_SIGNIN) {   // flow (one door): the silent sign-in; then the row that needed it
        const int r = mp_service_ensure_account ? mp_service_ensure_account() : 1;
        if (r == 1) {
            mBusy = B_NONE;
            pop();
            const int item = mPendingItem;
            mPendingItem = 0;
            mp_log("ui: signed in");
            if (item) choose(item);
            return;
        }
        if (r < 0) { showServiceError(-r); return; }
        if (elapsed > kRegisterTimeout) { showError(MP_ERR_TIMEOUT); return; }
        return;
    }
    if (mBusy == B_ACCT_DELETE) {
        // the account goes only when the service said so (the port then removes the account file and its cached friend list); a failure changes nothing
        const int s = mp_service_account_delete_state ? mp_service_account_delete_state() : -2;
        if (s == -1 && elapsed <= kRegisterTimeout) return;
        if (s == 1) {
            mBusy = B_NONE;
            mFriendCodeText[0] = 0;
            sFriendCount = -1;   // (no list: the next Friends screen asks again, for a new account)
            if (mIo) mIo->sound(Se::Success);
            mp_log("ui: online account deleted");
            cutToSafe();   // (the Multiplayer menu: a Friends screen would sign up again at once)
            char16_t t[96];
            txt::format(t, 96, MP_T_ACCOUNT_DELETED, Args());
            MpOverlay::toastText(t, 0);
            return;
        }
        if (s == -1) { showError(MP_ERR_TIMEOUT); return; }
        showServiceError(MP_ERR_SERVICE_DOWN, MP_T_ERR_SERVICE_DOWN);   // (the plain error; nothing was changed here)
        return;
    }
    if (mBusy == B_FRIEND_ADD) {
        // fix3: "added" only once the service said so; its refusal (list full, no such player, your own code) in its own words
        const int s = mp_service_friend_op_state ? mp_service_friend_op_state() : 1;
        if (s == -1 && elapsed <= kRegisterTimeout) return;
        if (s == 1) {
            mBusy = B_NONE;
            pop();
            if (mIo) mIo->sound(Se::Success);
            mFriendCodeText[0] = 0;   // M1: added: the next keyboard starts empty
            Args a;
            a.str = mFriendAdding;
            say(P_FRIEND_ADDED, MP_T_DLG_FRIEND_ADDED, a);
            refreshFriends();
            return;
        }
        if (s == -1) { showError(MP_ERR_TIMEOUT); return; }
        showServiceError(MP_ERR_SERVICE_DOWN, MP_T_DLG_FRIEND_FAILED);
        return;
    }
    if (st.phase == MP_UI_LOBBY || st.phase == MP_UI_PLAYING) {
        mBusy = B_NONE;
        mQuickRegather = false;   // uxfix #3
        // fix2: whoever opened the room is its original host (Host, or a Quick Match that found no room); a join is a guest
        mOriginalHost = mp_is_host();
        mp_log("ui: in the room (%s)", mOriginalHost ? "host" : "guest");
        pop();
        if (st.phase == MP_UI_LOBBY) openRoomGather();   // lobby: the same gathering screen as every other game (heads, one line, big buttons)
        // (PLAYING: watchSession launches)
        if (mIo) mIo->sound(Se::Success);
        return;
    }
    // fix3: an Internet room that failed: the service's own reason when it gave one (banned, not friends, ...), else the core's
    const bool online = mConn == MP_CONN_ONLINE && !mByAddr && mBusy != B_ADDR;   // (play by address never asks the service)
    if (st.phase == MP_UI_FAILED) { online ? showServiceError(st.error) : showError(st.error); return; }
    if (st.phase == MP_UI_IDLE && elapsed > 2 * kFps) {
        const int err = st.error != MP_ERR_NONE ? st.error : MP_ERR_INTERNAL;
        online ? showServiceError(err) : showError(err);
        return;
    }
    if (elapsed > kBusyTimeout) {
        mp_log("ui: wait timed out (phase %d)", st.phase);
        mLeavingByUs = true;
        mp_ui_leave();
        showError(MP_ERR_TIMEOUT);
    }
}

// ------------------------------------------------------------------ flow (one door): the gathering screen (port/mp_gather.h)
// Play Together / Star Race open it at once: this console is findable and looks for others; consoles that see each other gather in one
// game by themselves; the first player (the room's leader) presses START. No Join / Host, no settings: falls are asked once (the first
// Start), bumping is always on, four players. A friend far away: the leader's "Invite from Far Away" gives a code read aloud, the friend
// types it in "Join with a Code" on the Multiplayer menu. Nothing on this screen ever names an address, a port or a router.
namespace {
constexpr uint32_t kNobodyAfter = 20 * kFps;     // alone and nothing heard this long: the "nobody found" line (and the other radio)
constexpr uint32_t kCodeJoinTimeout = 25 * kFps; // joined with a code and still not in a game: "Your friend can't reach you from here."
constexpr uint32_t kInviteLookupExtra = 13 * kFps; // code5: a 5-character code first asks the service (12 s at most) before the game is reached
MpGatherStatus GatherReal() {
    MpGatherStatus s;
    memset(&s, 0, sizeof s);
    mp_gather_status(&s);
    return s;
}
// lobby: a room the core already holds (Play with Anyone, a friend's game, Continue X's Game) read as a gathering: self first, then the
// others by slot; the host slot leads; Start belongs to the host once someone else is there. The gathering driver is not involved.
MpGatherStatus RoomNow() {
    MpGatherStatus s;
    memset(&s, 0, sizeof s);
    s.state = MP_GATHER_GATHERING;
    s.conn = MP_CONN_ONLINE;
    s.roomsSeen = 1;   // (never the "nobody found" line: this room exists)
    const int local = mp_local_slot();
    const int host = mp_host_slot();
    auto add = [&](int k) {
        if (s.headCount >= MP_GATHER_HEADS) return;
        MpGatherHead& h = s.heads[s.headCount++];
        h.id = static_cast<uint32_t>(k + 1);
        snprintf(h.name, sizeof h.name, "%s", mp_slot_name(k));
        h.look = static_cast<uint8_t>(mp_slot_look(k));
        h.flags = static_cast<uint8_t>((k == local ? MP_HEAD_SELF : 0) | (k == host ? MP_HEAD_LEADER : 0));
    };
    if (local >= 0 && local < MP_MAX_PLAYERS && mp_slot_present(local)) add(local);
    for (int k = 0; k < MP_MAX_PLAYERS; k++)
        if (k != local && mp_slot_present(k)) add(k);
    s.canStart = mp_is_host() && s.headCount >= 2;
    return s;
}
MpGatherStatus GatherNow(bool room) { return room ? RoomNow() : GatherReal(); }
bool GatherLeader(const MpGatherStatus& s) { return s.headCount > 0 && (s.heads[0].flags & MP_HEAD_LEADER) != 0; }
const char* GatherLeaderName(const MpGatherStatus& s) {
    for (int i = 0; i < s.headCount && i < MP_GATHER_HEADS; i++)
        if (s.heads[i].flags & MP_HEAD_LEADER) return s.heads[i].name;
    return "";
}
}  // namespace
// core2: mp_gather_code_available (port/mp_gather.h): may the leader ask for a far-away code now, with no side effect. Weak here too: a
// host test without it has no "Invite from Far Away" (absent, never dead).
extern "C" bool mp_gather_code_available(void) __attribute__((weak));
// mpwifi44: mp_gather_last_error (port/mp_gather.h): why a gathering did not start / failed. Weak: a host test's fake may have none.
extern "C" int mp_gather_last_error(const char** reason) __attribute__((weak));
namespace {
// The error window for a gathering that could not start or failed. The gathering driver's own reason first (it says "no Wi-Fi" only when the
// console is not connected to an access point), then the core's, then `fallback`. One log line names the reason for the next console log.
int GatherError(int coreError, int fallback, const char* where) {
    const char* reason = "";
    const int g = mp_gather_last_error ? mp_gather_last_error(&reason) : 0;
    const int e = g != MP_ERR_NONE ? g : (coreError != MP_ERR_NONE ? coreError : fallback);
    mp_log("ui: gathering failed (%s): reason=%s gather_error=%d core_error=%d shown=%d", where, reason && *reason ? reason : "unknown", g, coreError, e);
    return e;
}
}  // namespace

bool Sequence::gathering() const {
    for (int i = 0; i < mDepth; i++)
        if (mStack[i].scr == S_GATHER) return true;
    return false;
}

void Sequence::refreshGatherCache() const {
    mGatherCache = GatherNow(mRoom);
    mGatherCached = true;
}

void Sequence::invalidateGatherCache() {
    mGatherCached = false;
}

const MpGatherStatus& Sequence::gatherStatus() const {
    if (!mGatherCached) refreshGatherCache();
    return mGatherCache;
}

void Sequence::openGather(bool byCode) {
    if (mIo) mIo->sound(Se::Decide);
    mCodeWait = false;
    refreshStars();   // populate cache once before defaultCoopSave() and S_GATHER per-frame model builds
    defaultCoopSave();
    mFileChosen = false;
    mFileOnly = false;
    mOriginalHost = false;   // (the leader at Start is the one whose file is played: requestStart)
    if (!byCode && !mp_gather_start(mMode, MP_CONN_NONE)) {
        mp_log("ui: gathering could not start");
        // mpwifi44: the reason the gathering driver gives. Without one (a fake, an older driver) the old answer: the core's error, else "no Wi-Fi"
        showError(GatherError(failError(MP_ERR_NONE), MP_ERR_NO_WIFI, "start"));
        return;
    }
    mByCode = byCode;
    mGatherAt = mFrame;
    mGatherLogged = 0xFF;
    invalidateGatherCache();
    for (uint32_t& id : mHeadIds) id = 0;
    MpChat::noteRoomPublic(false);
    push(S_GATHER);
    mp_log("ui: gathering mode=%d%s", mMode, byCode ? " (a friend's code)" : "");
}

// lobby: the core holds a room this console is in (Play with Anyone, a friend's game, Continue X's Game): the same gathering screen as
// every other game. Leaving and Start go to the core (mp_ui_leave / mp_ui_start), not to the gathering driver.
void Sequence::openRoomGather() {
    mRoom = true;
    mCodeWait = false;
    mByCode = false;
    mFileChosen = false;
    mFileOnly = false;
    mGatherAt = mFrame;
    mGatherLogged = 0xFF;
    invalidateGatherCache();
    for (uint32_t& id : mHeadIds) id = 0;
    refreshStars();   // populate cache once before defaultCoopSave() and S_GATHER per-frame model builds
    defaultCoopSave();
    push(S_GATHER);
    mp_log("ui: the game's gathering screen (online room)");
}

// a head keeps its layout slot (its spin, its 3D head) for as long as that player is there, whatever the order of the list
int Sequence::headSlot(uint32_t id) const {
    for (int i = 0; i < 4; i++)
        if (mHeadIds[i] == id) return i;
    for (int i = 0; i < 4; i++)
        if (mHeadIds[i] == 0) { mHeadIds[i] = id; return i; }
    return -1;
}

// code5: a code join that failed before any game was reached. The lookup's own reasons (MpInviteError) each have their sentence; a typed code
// the service did not know, or no longer has, comes back in the keyboard (OK) to be corrected; the service's trouble keeps the text too
// (try again later); "full" and "another version" are about the game, not the code. 0 = the game of the code never answered.
void Sequence::codeJoinFailed(int why) {
    switch (why) {
    case MP_INV_WRONG:
    case MP_INV_SEALED_BAD:
        say(P_CODE_BAD, MP_T_DLG_CODE_BAD); return;
    case MP_INV_EXPIRED:
        say(P_CODE_BAD, MP_T_DLG_CODE_EXPIRED); return;
    case MP_INV_BUSY:
        say(P_CODE_BAD, MP_T_SVC_BUSY); return;
    case MP_INV_OFF:
        say(P_CODE_BAD, MP_T_SVC_OFFLINE); return;
    case MP_INV_FULL:
        mCodeKeep = false;
        showError(MP_ERR_ROOM_FULL); return;
    case MP_INV_VERSION:
        mCodeKeep = false;
        showError(MP_ERR_DIFFERENT_VERSION); return;
    case MP_INV_NO_WIFI:
        showError(MP_ERR_NO_WIFI); return;
    case MP_INV_CLOCK:
        showError(MP_ERR_CLOCK); return;
    case MP_INV_UPDATE:
        showError(MP_ERR_UPDATE_REQUIRED); return;
    default:
        say(P_CANT_REACH, MP_T_DLG_CANT_REACH); return;
    }
}

void Sequence::showFarCode() {
    const MpGatherStatus& st = gatherStatus();
    char code[MP_DIRECT_CODE_MAX] = {0};
    if (st.code[0]) snprintf(code, sizeof code, "%s", st.code);
    else if (!mp_gather_get_code(code, sizeof code)) code[0] = 0;
    invalidateGatherCache();
    if (!code[0]) {   // core2: the first call turns the game into a by-code game; the code comes in a few seconds ("Getting your code...")
        if (!mCodeWait) {
            if (mIo) mIo->sound(Se::Decide);
            mp_log("ui: far-away code asked");
        }
        mCodeWait = true;
        return;
    }
    mCodeWait = false;
    if (mIo) mIo->sound(Se::Success);
    mp_log("ui: far-away code shown");   // (on the gathering screen itself, big: MpPanel KIND_HEADS draws Model::big)
}

// ------------------------------------------------------------------ flow: Join, Host, Start, a player's choices
// flow: the host's Start row. The original host is asked "Which file?" first (one question, the cursor on the co-op file played last,
// or on a new one the first time); a host by migration starts at once (only the original host's save choice exists, MpSaveGate).
void Sequence::requestStart() {
    if (gathering() && mRoom) {   // lobby: a room the core holds: its host starts it (no gathering driver, no falls question: the room has its rules)
        if (!mp_is_host() || othersPresent() < 1) {
            if (mIo) mIo->sound(Se::Buzzer);
            return;
        }
        if (mIo) mIo->sound(Se::Decide);
        if (!mOriginalHost || mMode == MP_MODE_RACE || mFileChosen) {   // (a host by migration, a Star Race and a file already chosen need no question)
            if (mOriginalHost && !mFileChosen) defaultCoopSave();
            startSession();
            return;
        }
        defaultCoopSave();
        push(S_SAVE);
        int it[kItems];
        const int n = items(S_SAVE, 0, it);
        for (int i = 0; i < n; i++)
            if (it[i] == I_COOP1 + mCoopSlot - 1) top().cursor = static_cast<int8_t>(i);
        fitScroll(top());
        return;
    }
    if (gathering()) {
        // flow (one door): only the settled leader starts (core2: canStart is the only thing that may show "Press START")
        const MpGatherStatus& st = gatherStatus();
        if (!st.canStart || st.headCount < 2) {
            if (mIo) mIo->sound(Se::Buzzer);
            return;
        }
        if (mIo) mIo->sound(Se::Decide);
        mOriginalHost = true;   // (the leader's file is the game's: MpSaveGate's host choice)
        if (mMode == MP_MODE_TOGETHER && !FlowStore().fallsAsked) {   // asked once, the first time; remembered after
            push(S_FALLS);
            top().cursor = mp_core_saved_downed() == MP_DOWNED_INFINITE ? 1 : 0;
            return;
        }
        mFalls = mp_core_saved_downed() == MP_DOWNED_INFINITE ? 1 : 0;
        // returning: the co-op file played last goes on at once ("Change File" chose another); the first time: Which File?
        const int last = FlowStore().lastCoop;
        // (uxfix #6: a Star Race has no shared progress to keep: it never asks Which File?, the first time or any other)
        if (mFileChosen || mMode == MP_MODE_RACE || (last >= 1 && last <= 6 && coopStars(last) >= 0)) {
            if (!mFileChosen) defaultCoopSave();
            startSession();
            return;
        }
        defaultCoopSave();
        push(S_SAVE);
        int it[kItems];
        const int n = items(S_SAVE, 0, it);
        for (int i = 0; i < n; i++)
            if (it[i] == I_COOP1 + mCoopSlot - 1) top().cursor = static_cast<int8_t>(i);
        fitScroll(top());
        return;
    }
}

// Start (a file just chosen, or START with the file going on): the host's save is made ready, then the core starts the session.
void Sequence::startSession() {
    const bool gather = gathering();
    auto backToLobby = [this] { while (mDepth > 0 && top().scr != S_GATHER) --mDepth; };
    if (gather && mFileOnly) {   // flow (one door): Change File: the file is chosen; START goes on with it
        mFileOnly = false;
        mFileChosen = true;
        mp_log("ui: file changed kind=%d coop=%d", mSaveKind, mCoopSlot);
        backToLobby();
        return;
    }
    const MpGatherStatus& st = gatherStatus();
    const bool canGo = gather && st.canStart && st.headCount >= 2;
    if (!canGo) {   // (everyone left while the file was chosen)
        if (mIo) mIo->sound(Se::Buzzer);
        MpSaveGame::abandonHostChoice();
        backToLobby();
        return;
    }
    if (mOriginalHost && !prepareHostSave()) {   // (prepareHostSave forgets the choice when it fails)
        if (mIo) mIo->sound(Se::Buzzer);
        backToLobby();
        say(P_INFO, MP_T_DLG_SAVE_FAILED);
        return;
    }
    if (gather && !mRoom) {   // flow (one door): the room's rules, then the leader's Start (bumping on, four players, typed messages on)
        MpSettings s = *mp_settings();
        s.mode = mMode;
        s.maxPlayers = 4;
        s.interaction = 1;
        s.flags = static_cast<uint8_t>(s.flags & ~MP_SET_TEXT_OFF);
        s.downed = mMode == MP_MODE_TOGETHER && mFalls ? MP_DOWNED_INFINITE : MP_DOWNED_BUBBLES;
        (void)mp_core_update_settings(&s);
    }
    const bool started = mRoom ? mp_ui_start() : mp_gather_begin();
    invalidateGatherCache();
    if (!started) {
        if (mOriginalHost && MpSaveGame::isSessionRoot()) MpSaveGame::leaveSessionRoot();
        if (mOriginalHost) MpSaveGame::abandonHostChoice();   // gamefix: the start failed: forget "Play on my file"
        backToLobby();   // (flow: the room is still there: its gathering screen, with the window)
        noteErrorShown(MP_ERR_INTERNAL);
        if (mIo) mIo->sound(Se::Buzzer);
        say(P_INFO, txt::errorText(MP_ERR_INTERNAL));
        return;
    }
    if (mOriginalHost && mSaveKind != SAVE_MINE && mCoopSlot >= 1 && mCoopSlot <= 6) {
        MpFlowStore fs = FlowStore();
        fs.lastCoop = static_cast<uint8_t>(mCoopSlot);
        SaveFlowStore(fs);
        mPlayCoop = mCoopSlot;   // (its players and galaxy are written to coop.cfg as the session goes)
        // (review 2 #3: in use from now on, even before a star; a copy starts a new history, a co-op file in use keeps its names)
        MpCoopMeta had;
        memset(&had, 0, sizeof had);
        if (mSaveKind == SAVE_COPY || !mp_coop_meta_get || !mp_coop_meta_get(mCoopSlot, &had)) {
            mSessionCount = 0, mLastGalaxy = -1;
            writeCoopMeta(mCoopSlot);
        }
    } else {
        mPlayCoop = 0;
    }
    backToLobby();
    mp_log("ui: start pressed");
}

// The Leaderboards' pickers (left / right): the galaxy and its star. (One door: no other value rows: no Room Settings.)
void Sequence::changeValue(int item, int dir) {
    auto cycle = [dir](int v, int lo, int hi) { v += dir; return v < lo ? hi : (v > hi ? lo : v); };
    bool changed = true;
    switch (item) {
    case I_GALAXY: {
        const int n = mIo ? mIo->galaxyCount() : 0;
        if (n <= 0) { changed = false; break; }
        mBoardGalaxy = cycle(mBoardGalaxy, 0, n - 1);
        mBoardStar = 1;
        mBoardDirty = true;
        mBoardDirtyAt = mFrame;
        break;
    }
    case I_STARPICK: {
        const int n = mIo ? mIo->scenarioCount(mBoardGalaxy) : 0;
        if (n <= 0) { changed = false; break; }
        mBoardStar = cycle(mBoardStar, 1, n);
        mBoardDirty = true;
        mBoardDirtyAt = mFrame;
        break;
    }
    default: changed = false; break;
    }
    if (mIo && changed) mIo->sound(Se::Cursor);
    if (changed) mPulse = 1.0f;
}

bool Sequence::itemsGather(Scr s, uint8_t param, int* out, int* np) const {
    int& n = *np;
    auto add = [&](int i) { if (n < kItems) out[n++] = i; };
    switch (s) {
    case S_GATHER:
        {   // flow (one door): at most three big buttons, each only when it can do something now
        const MpGatherStatus& st = gatherStatus();
        const bool alone = st.headCount <= 1;
        const bool settled = st.state == MP_GATHER_GATHERING;
        const bool nobody = alone && settled && st.roomsSeen == 0 && mFrame - mGatherAt > kNobodyAfter;
        if (alone && st.playingRooms > 0) add(I_GJOIN);   // a game already playing nearby: "Join Luigi64's Game"
        // (owner: the code is the biggest thing on the screen once it is there, so the button goes then; while it comes the button
        // stays where it is and the line says "Getting your code...")
        if (GatherLeader(st) && !mByCode && !st.code[0] && (mCodeWait || (settled && mp_gather_code_available && mp_gather_code_available())))
            add(I_GINVITE);
        const bool hasService = (alone && settled && !mByCode) ? NetInfo().service : false;
        if (hasService && st.wifiConnected) add(I_QUICK);   // Play with Anyone
        if (hasService && mMode == MP_MODE_RACE) add(I_BOARDS);   // (Star Race: the times)
        if (nobody && !mByCode && st.wifiConnected && st.nearbyPossible) add(I_GSWITCH);   // (the friend may be on the other radio)
        if (mRoom) { n = 0; break; }   // lobby: a room the core holds has no buttons: START starts, the guide has the rest
        if (n > 3) n = 3;   // (three big buttons at most: the look; the first three are what matters most)
        break;
    }
    case S_FALLS:
        add(I_FALLS_BUBBLE), add(I_FALLS_TRY);
        break;
    default: return false;
    }
    (void)param, (void)add;
    return true;
}

bool Sequence::chooseGather(int item) {
    Io* io = mIo;
    auto decide = [io] { if (io) io->sound(Se::Decide); };
    (void)decide;
    switch (item) {
    case I_GJOIN:
        {   // flow (one door): the game playing nearby (the first one heard)
        MpLocalRoom rooms[4];
        const int n = mp_gather_playing_rooms(rooms, 4);
        if (n <= 0) { if (io) io->sound(Se::Buzzer); return true; }
        decide();
        mp_log("ui: join the game playing nearby (%s)", rooms[0].host);
        const bool joined = mp_gather_join_playing(rooms[0].id);
        invalidateGatherCache();
        if (!joined) { if (io) io->sound(Se::Buzzer); }
        return true;
    }
    case I_GINVITE:
        showFarCode(); return true;
    case I_GSWITCH:
        {   // flow (one door): Wi-Fi <-> consoles nearby (the Vita cannot look on both): the heads go, they come back
        decide();
        const MpGatherStatus& st = gatherStatus();
        const MpConn to = st.conn == MP_CONN_NEARBY ? MP_CONN_WIFI : MP_CONN_NEARBY;
        mp_log("ui: gathering looks %s", to == MP_CONN_NEARBY ? "nearby" : "on wi-fi");
        if (mp_gather_switch(to)) { mGatherAt = mFrame; invalidateGatherCache(); }
        else if (io) io->sound(Se::Buzzer);
        return true;
    }
    case I_FALLS_BUBBLE:
    case I_FALLS_TRY:
        {   // flow (one door): the falls question's answer: remembered (mp.cfg "downed"); asked never again; Start goes on
        decide();
        mFalls = item == I_FALLS_TRY ? 1 : 0;
        mp_core_save_downed(mFalls ? MP_DOWNED_INFINITE : MP_DOWNED_BUBBLES);
        MpFlowStore fs = FlowStore();
        fs.fallsAsked = 1;
        SaveFlowStore(fs);
        mp_log("ui: falls %s", mFalls ? "try again" : "bubble");
        pop();
        requestStart();
        return true;
    }
    default: break;
    }
    return false;
}

bool Sequence::dialogGather(int purpose, bool yes, int param) {
    switch (purpose) {
    case P_KICK:
        if (yes) {
            mp_ui_kick(param);
            invalidateGatherCache();
            mp_log("ui: kick slot=%d", param);
            if (mDepth > 0 && top().scr == S_PLAYER) pop();
        }
        return true;
    case P_LEAVE_GATHER:
        if (yes) {   // flow (one door)
            mp_gather_stop();
            invalidateGatherCache();
            MpSaveGame::abandonHostChoice();
            mp_log("ui: left the gathering");
            while (mDepth > 0 && top().scr != S_GATHER) --mDepth;
            if (mDepth > 0) pop();
        }
        return true;
    case P_LEAVE_LOBBY:
        if (yes) {
            mLeavingByUs = true;
            mp_ui_leave();
            MpSaveGame::abandonHostChoice();   // gamefix: closed the lobby after "Play on my file": forget it
            mp_log("ui: left the room");
            cutToSafe();
        }
        return true;
    default: return false;
    }
    return true;
}

bool Sequence::keyboardGather(int purpose, const char* text) {
    switch (purpose) {
    default: return false;
    }
    return true;
}

bool Sequence::tickGather(Scr scr, const Input& in, bool* stop) {
    switch (scr) {
    case S_BUSY:
        tickBusy();
        if (mDepth == 0 || top().scr != S_BUSY || mDlg) { *stop = true; return true; }
        if (in.back) {
            mp_log("ui: wait cancelled");
            if (mIo) mIo->sound(Se::Cancel);
            if (mBusy != B_FRIEND_ADD && mBusy != B_SIGNIN) {
                mLeavingByUs = true;
                mp_ui_leave();
            }
            const bool wasQuick = mBusy == B_QUICK;
            if (mBusy == B_SIGNIN) mQuickRegather = false;   // (uxfix #3: the gathering is still there under the sign-in wait)
            mBusy = B_NONE;
            pop();
            if (wasQuick) regatherAfterQuick();   // uxfix #3: cancelled the search: back to looking for friends
        }
        { *stop = true; return true; }
    case S_GATHER:
        {   // flow (one door): START (or Cross where there is no button) starts; what the gathering does shows here
        const MpGatherStatus& st = gatherStatus();
        if (st.state != mGatherLogged) {
            mp_log("ui: gathering state %d heads=%d leader=%d", st.state, st.headCount, GatherLeader(st) ? 1 : 0);
            mGatherLogged = st.state;
        }
        if (st.state == MP_GATHER_FAILED) {
            MpUiStatus us;
            memset(&us, 0, sizeof us);
            mp_ui_status(&us);
            const uint8_t invErr = st.inviteError;
            mp_gather_stop();
            invalidateGatherCache();
            pop();
            if (mByCode) {
                if (mIo) mIo->sound(Se::Buzzer);
                codeJoinFailed(invErr);
            } else {
                showError(GatherError(us.error, MP_ERR_NO_WIFI, "gathering"));   // mpwifi44
            }
            *stop = true;
            return true;
        }
        if (mByCode && st.headCount <= 1 && mFrame - mGatherAt > kCodeJoinTimeout + (mp_invite_kind(mCodeText) == MP_INVITE_LEN ? kInviteLookupExtra : 0)) {   // the code's game never answered
            mp_log("ui: a friend's code: no answer");
            mp_gather_stop();
            invalidateGatherCache();
            pop();
            if (mIo) mIo->sound(Se::Buzzer);
            say(P_CANT_REACH, MP_T_DLG_CANT_REACH);
            *stop = true;
            return true;
        }
        if (mByCode && st.headCount >= 2) mCodeKeep = false;   // uxfix #1: the friend's game answered: the typed code is spent
        if (mCodeWait) {   // the far-away code arrived: the screen shows it
            char code[MP_DIRECT_CODE_MAX] = {0};
            if (st.code[0] || mp_gather_get_code(code, sizeof code)) showFarCode();
        }
        const int c = itemCount();
        if (top().cursor >= c) top().cursor = static_cast<int8_t>(c - 1);
        if (top().cursor < 0 && c > 0) top().cursor = 0;
        if (in.start || (in.ok && c == 0)) {
            requestStart();
            *stop = true;
            return true;
        }
        break;
    }
    default: return false;
    }
    (void)in;
    return true;
}

bool Sequence::labelGather(int item, MpPanel::Row& r, const Frame& f) const {
    using namespace MpPanel;
    (void)f;
    switch (item) {
    case I_GJOIN:
        {
        MpLocalRoom rooms[1];
        memset(rooms, 0, sizeof rooms);
        if (mp_gather_playing_rooms(rooms, 1) > 0) rooms[0].host[MP_NAME_BYTES] = 0;
        Str(r.label, kLabel, MP_T_G_JOIN_PLAYING, rooms[0].host);
        return true;
    }
    case I_GINVITE:
        Text(r.label, kLabel, MP_T_INVITE_FAR); return true;
    case I_GSWITCH:
        Text(r.label, kLabel, gatherStatus().conn == MP_CONN_NEARBY ? MP_T_SCAN_WIFI : MP_T_SCAN_NEARBY); return true;
    case I_FALLS_BUBBLE:
        Text(r.label, kLabel, MP_T_FALLS_A_BUBBLE); return true;
    case I_FALLS_TRY:
        Text(r.label, kLabel, MP_T_FALLS_A_TRY); return true;
    default: break;
    }
    return false;
}

bool Sequence::modelGather(MpPanel::Model* m, const Frame& f, int cursorItem, int count, const int* it) const {
    using namespace MpPanel;
    auto button = [m](int pic, MpText label) { ModelButton(m, pic, label); };
    auto hint = [m](MpText t) { Text(m->hint, 80, t); };
    auto label = [this, &f](int item, Row& r) { rowLabel(item, r, f); };
    (void)button, (void)hint, (void)label, (void)cursorItem, (void)count, (void)it;
    switch (f.scr) {
    case S_BUSY:
        {   // uxfix #4: a wait is the file select's look too: the game's starry background, its one line at the bottom, Cancel in the guide
        m->kind = KIND_BUTTONS;
        MpUiStatus st;
        memset(&st, 0, sizeof st);
        mp_ui_status(&st);
        MpText s = MP_T_CONNECTING;
        switch (f.param) {
        case B_SIGNIN: s = MP_T_CONNECTING; break;
        case B_HOST: s = MP_T_CREATING; break;
        case B_QUICK: s = MP_T_SEARCHING; break;
        default: s = st.phase == MP_UI_JOINING ? MP_T_JOINING : MP_T_CONNECTING; break;
        }
        Text(m->title, 48, f.param == B_FRIEND_ADD ? MP_T_FRIENDS : ModeName(mMode));   // (logs and tests; not drawn in the look)
        if (st.progress > 0 && st.progress < 100) {
            Num(m->status, 80, MP_T_RECEIVING, st.progress);
            m->progress = st.progress / 100.0f;
            Num(m->line, 96, MP_T_RECEIVING, st.progress);
            m->look = LOOK_GAME;
        } else {
            Text(m->status, 80, s);
            GameLook(m, s);
        }
        m->spinner = (mFrame % 90) / 90.0f;
        button(txt::PIC_CIRCLE, MP_T_CANCEL);
        break;
    }
    case S_GATHER:
        {   // flow (one door): the players' heads, one line, the buttons that can do something now
        const MpGatherStatus& st = gatherStatus();
        m->kind = KIND_BUTTONS;
        Text(m->title, 48, ModeName(mMode));   // (logs and tests; not drawn in the look)
        const bool alone = st.headCount <= 1;
        const bool leader = GatherLeader(st);
        const bool nobody = alone && st.state == MP_GATHER_GATHERING && st.roomsSeen == 0 && mFrame - mGatherAt > kNobodyAfter;
        if (st.code[0] && GatherLeader(st)) {   // the code itself, big, in its groups, on this screen (no window to close)
            char code[MP_DIRECT_CODE_MAX];
            snprintf(code, sizeof code, "%s", st.code);
            txt::utf8To16(m->big, static_cast<int>(sizeof m->big / sizeof m->big[0]), code);
            // B6b: the caption over the code (the band says it only when nothing else needs the band: not while Press START, not for the long code's why)
            if (mCodeWait || st.codeLong || (st.canStart && st.headCount >= 2)) Text(m->bigLabel, 40, MP_T_G_TELL_CODE);
        }
        if (mCodeWait) GameLook(m, MP_T_G_GETTING_CODE);
        else if (st.code[0] && GatherLeader(st) && !(st.canStart && st.headCount >= 2)) GameLook(m, st.codeLong ? MP_T_G_CODE_LONG : MP_T_G_TELL_CODE);   // code5: a long code says why it is long
        else if (st.canStart && st.headCount >= 2) GameLook(m, MP_T_G_PRESS_START);
        else if (!leader && st.headCount >= 2 && st.state == MP_GATHER_GATHERING) GameLookName(m, MP_T_G_WAIT_NAME, GatherLeaderName(st));
        else if (mByCode && alone) GameLook(m, MP_T_G_REACHING);   // uxfix #1: a code joiner is not "waiting for friends", it is reaching one
        else if (alone && !mRoom && !mByCode && st.state == MP_GATHER_GATHERING && st.fullRooms > 0) GameLook(m, MP_T_G_FULL);   // lobby: heard, never offered: said now
        else if (alone && !mRoom && !mByCode && st.state == MP_GATHER_GATHERING && st.otherVersionRooms > 0) GameLook(m, MP_T_G_OTHER_VER);
        else if (nobody && !mByCode) GameLook(m, MP_T_G_NOBODY);
        else GameLook(m, MP_T_G_WAITING);   // (listening, merging, alone, a leader not settled yet)
        const int heads = st.headCount < MP_GATHER_HEADS ? st.headCount : MP_GATHER_HEADS;
        // the slots of players who left are free again
        for (int s = 0; s < 4; s++) {
            bool there = false;
            for (int i = 0; i < heads; i++) there = there || st.heads[i].id == mHeadIds[s];
            if (!there) mHeadIds[s] = 0;
        }
        Heads(m, heads, [&](int i, Row& r) {
            const MpGatherHead& h = st.heads[i];
            char name[MP_NAME_BYTES + 1];
            snprintf(name, sizeof name, "%s", h.name);
            txt::utf8To16(r.label, kLabel, name);
            r.icon = static_cast<uint8_t>(ICON_MARIO + (h.look < MP_LOOK_COUNT ? h.look : 0));
            bool fresh = true;
            for (uint32_t id : mHeadIds) fresh = fresh && id != h.id;
            r.owner = static_cast<int8_t>(headSlot(h.id));
            if (fresh && !(h.flags & MP_HEAD_SELF)) r.marks |= MARK_ARRIVED;   // (heads: pops in with the game's sound)
            if (h.flags & MP_HEAD_LEADER) r.marks |= MARK_CROWN;
            if (h.flags & MP_HEAD_SELF) {
                r.marks |= MARK_YOU;
                if (st.headCount >= 2) Text(r.value, kValue, MP_T_YOU);   // uxfix #9: "You" under your own head (which one am I?)
            }
            if (h.flags & MP_HEAD_ARRIVING) r.marks |= MARK_DISABLED;   // (core2: still on its way in: drawn, faint)
        });
        if (st.canStart && st.headCount >= 2) button(txt::PIC_START, MP_T_START);
        else if (count > 0) button(txt::PIC_CROSS, MP_T_SELECT);
        const int last = FlowStore().lastCoop;
        if (leader && !mRoom && !mByCode && mMode == MP_MODE_TOGETHER && last >= 1 && last <= 6 && coopStars(last) >= 0) button(txt::PIC_SQUARE, MP_T_CHANGE_FILE);
        button(txt::PIC_TRIANGLE, MP_T_CHANGE_CHAR);
        button(txt::PIC_CIRCLE, MP_T_LEAVE);
        break;
    }
    case S_FALLS:
        m->kind = KIND_BUTTONS;
        GameLook(m, MP_T_FALLS_Q);
        button(txt::PIC_CROSS, MP_T_SELECT), button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    default: return false;
    }
    return true;
}

// Input this family takes before the shared rules (stage 0: a direction, for grids; stage 1: Square / Triangle). True: taken.
bool Sequence::pressGather(Scr scr, const Input& in, int item, int count, int stage) {
    if (stage == 1 && in.sq && scr == S_GATHER) {   // flow (one door): Change File (the leader, when a co-op file goes on by itself)
        const MpGatherStatus& st = gatherStatus();
        const int last = FlowStore().lastCoop;
        if (!GatherLeader(st) || mRoom || mByCode || mMode != MP_MODE_TOGETHER || !(last >= 1 && last <= 6 && coopStars(last) >= 0)) return true;
        if (mIo) mIo->sound(Se::Decide);
        if (!mFileChosen) defaultCoopSave();
        mFileOnly = true;
        push(S_SAVE);
        int it[kItems];
        const int n = items(S_SAVE, 0, it);
        for (int i = 0; i < n; i++)
            if (it[i] == I_COOP1 + mCoopSlot - 1) top().cursor = static_cast<int8_t>(i);
        fitScroll(top());
        return true;
    }
    if (stage == 1 && in.tri && scr == S_GATHER) {   // flow (one door): Change Character (the character select, as in the lobby)
        if (mIo) mIo->sound(Se::Decide);
        openChars(1);
        return true;
    }
    (void)scr, (void)in, (void)item, (void)count, (void)stage;
    return false;
}

// Back (Circle) on one of this family's screens: phase 0 before anything (true: taken, nothing else happens), 1 just before the
// screen goes, 2 just after.
bool Sequence::leaveGather(Scr scr, uint8_t param, int phase) {
    if (phase == 0 && scr == S_GATHER && mRoom) {   // lobby: a room the core holds: the host closes it, a guest leaves it (always asked, as the lobby did)
        ask(P_LEAVE_LOBBY, mp_is_host() ? MP_T_DLG_CLOSE_ROOM : MP_T_DLG_LEAVE_GUEST, Args(), Dlg::YesNo);
        return true;
    }
    if (phase == 0 && scr == S_GATHER) {   // flow (one door): with others there, one question; alone, just go
        if (gatherStatus().headCount > 1) {
            ask(P_LEAVE_GATHER, MP_T_DLG_LEAVE_GUEST, Args(), Dlg::YesNo);
            return true;
        }
    }
    if (phase == 1 && scr == S_GATHER && !mRoom) {
        mp_gather_stop();
        invalidateGatherCache();
        MpSaveGame::abandonHostChoice();
        mp_log("ui: left the gathering");
    }
    (void)scr, (void)param, (void)phase;
    return false;
}

}  // namespace MpMenu
