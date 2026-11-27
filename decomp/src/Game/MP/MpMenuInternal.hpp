// Galaxy 3.0 multiplayer: the menus' shared internals (flow split, 11 Nov). PRIVATE to decomp/src/Game/MP/MpMenu*.cpp: the item,
// window, keyboard and wait ids, the lists polled from the port, and the small helpers every screen family uses. The shared core
// (MpMenuSequence.cpp) owns this file: ask its owner (flow) before changing anything here; a family adds its own items at the end
// of its range in enum Item and its own window purposes at the end of enum Purpose.
#pragma once

#include <stdio.h>
#include <string.h>

#include "Game/MP/MpChat.hpp"
#include "Game/MP/MpChatLogic.hpp"
#include "Game/MP/MpMenuSequence.hpp"
#include "Game/MP/MpSaveGate.hpp"
#include "Game/MP/MpUi.hpp"
#include "port/mp_core_game.h"
#include "port/mp_gather.h"   // flow (one door): core2's gathering (the host tests link core2's fake)
#include "port/mp_ui_flow.h"

namespace MpMenu {


using MpMsgText::Args;
namespace txt = MpMsgText;

enum Item : int {
    I_NONE = 0, I_TOGETHER, I_RACE, I_BOARDS, I_FRIENDS, I_HOWTO, I_BACK,
    I_REJOIN,                          // flow: "Rejoin <host>'s room" on top of the Multiplayer menu (2 minutes after a drop)
    I_QUICK,                           // flow: Play with Anyone (Quick Match; the gathering screen, service only)
    I_START,                           // flow: the lobby's Start row (host)
    I_COOP1, I_OWNFILE = I_COOP1 + 6,  // flow: Which file?: co-op files 1..6, "Start from my own file..."
    I_USE_COPY, I_USE_PLAY,            // flow: one of my files: "Make a co-op copy" / "Play on this file"
    I_SLOT1, I_RESUME = I_SLOT1 + 6, I_WARP, I_STAMPS, I_PLAYERS, I_DOME, I_LEAVE,
    I_GALAXY, I_STARPICK, I_CHAR0, I_SLOT_SINGLE = I_CHAR0 + static_cast<int>(MP_LOOK_COUNT),
    I_REQUESTS, I_BLOCKED, I_REQ_ADD, I_REQ_NOTNOW, I_REQ_BLOCK,   // friendsui: the Friends screen's two extra rows, one request's choices
    I_PLAYED,                          // flow: Friends: "Played With (N)"
    I_FR_JOIN, I_FR_REMOVE,            // m-friends: a friend's page (S_REQUEST, param 1): Join their game / Remove Friend
    I_P_CHAR, I_P_ADD, I_P_BLOCK, I_P_MUTE, I_P_KICK,   // flow: one player's choices (lobby, session)
    I_PHRASE0, I_WRITE = I_PHRASE0 + MP_CHAT_PHRASES,   // chat: the Messages page's phrases, "Write a Message"
    I_SPFILE1, I_SPFILE_END = I_SPFILE1 + 6,   // entry: single-player files 1..6 (S_SPFILE)
    I_CONNTEST,                        // conntest: the Connection Test (a guide button; not a list row)
    I_CT_ROOM, I_CT_FAR, I_CT_ONLINE,  // conntest: the screen's three questions (rows; a question with no answer to give is not listed)
    I_GJOIN, I_GINVITE, I_GSWITCH,     // flow (one door): the gathering screen: join a game already playing nearby, invite from far away, look on the other radio
    I_FALLS_BUBBLE, I_FALLS_TRY,       // flow (one door): the falls question's two answers
    I_DIRECTCODE,                      // flow (one door): the Multiplayer menu's "Join with a Code"
    I_KEEP, I_KEEP_MAKE, I_KEEP_SHARE, I_KEEP_BACK, I_KEEP0, I_KEEP_END = I_KEEP0 + 12,   // m-files: Safe Copies: the Multiplayer menu's row, Make a Copy, Make a Copy to Share, Bring Back a Copy, the copies (S_KEEPLIST)
    I_KC_SLOT1, I_KC_ACCT = I_KC_SLOT1 + 6, I_KC_SET1, I_KC_END = I_KC_SET1 + 3, I_DELACCT,
    I_HELP, I_RP_CHECK, I_RP_SEND, I_RP_RESUME, I_RP_BSEND, I_RP_BNOTE, I_RP_BCANCEL,   // beta diagnostics (MpMenuReport.cpp): the Multiplayer menu's Having Trouble? row, its two rows, the consent screen's three   // Friends: Delete My Online Account (the list's last row, only with an account)   // keepsafe API: one copy (S_KEEPCOPY): its Team Files, its online account, its three settings files
    I_SET0, I_SESSION_SETTINGS = I_SET0 + 3, I_SET_END = I_SET0 + 6,   /* setfix31b: + 4 Start Again Now, + 5 Later (S_SET_RESTART) */    // settingsnative: the Settings screens' pills are I_SET0 + k, k counted per screen (MpMenuSettings.cpp names them)
    I_LIST0 = 130,                     // (130 since setnat31: the Settings and the beta diagnostics ids both landed below it) players (by slot), friends, requests, blocked players, played with
    I_FROOM0 = 170,                    // flow: the Join list: friends' rooms (by friend row)
    I_LROOM0 = 190                     // flow: the Join list: rooms on the same Wi-Fi / nearby (by scan row)
};
static_assert(I_SPFILE_END <= I_LIST0, "menu item ids run into the list rows");
static_assert(I_KC_END <= I_LIST0, "menu item ids run into the list rows");
static_assert(I_KEEP_END <= I_LIST0, "menu item ids run into the list rows");
static_assert(I_SET_END <= I_LIST0, "menu item ids run into the list rows");

enum Purpose : int {
    P_NONE = 0, P_TIP, P_ERROR, P_KICK, P_LEAVE_LOBBY, P_LEAVE_SESSION, P_PLAY_MINE, P_OVERWRITE, P_KEEP, P_KEPT,
    P_FRIEND_ADDED, P_FRIEND_REMOVE, P_DOME, P_ENDED, P_INFO, P_SAVE_ASK, P_FEWER, P_RECOVERY, P_SAVE_MINE, P_IMPORT_FEWER, P_TIP_NEXT, P_SP_FEWER,
    P_REQ_BLOCK,   // friendsui
    P_PLAYER_BLOCK, // flow: Block on a player of the room
    P_MET_BLOCK,    // m-friends: Block on a player of Played With
    P_CT_RESULT, P_CT_OFFER, P_CT_REPORT,  // conntest: the verdict's lines; "Run a Connection Test?" after a failed join / host
    P_RULES,       // touches: the host-rules card
    P_LEAVE_GATHER, P_HOWTO,   // flow (one door): leave the gathering (others are there); How to Play (then: the Connection Test?)
    P_CODE_BAD, P_CANT_REACH,   // uxfix #1: a code that does not decode / a code whose game never answered: OK reopens the keyboard with the text
    P_BK_SLOT, P_BK_ACCOUNT, P_BK_SET, P_BK_DONE, P_BK_NOTICE,   // keepsafe API: Safe Copies: the question for a Team File / the online account / a settings file (param = the item), the result, the entry-time card
    P_DELETE_ACCOUNT,   // Friends: Delete My Online Account: the Yes / No question
    P_HOWTO_SAFE,   // menus4: How to Play's second page: Keep My Multiplayer Data Safe (Yes opens Safe Copies)
    P_RP_INFO1, P_RP_INFO2, P_RP_THANKS, P_RP_ERR, P_RP_AFTER, P_RP_CKRES,   // beta diagnostics (MpMenuReport.cpp): what is sent (two pages), Thanks, a failure, the after-error offer, the check's result
    P_SET_RO, P_SET_RESET_ALL, P_SET_SAVE_FAIL, P_SET_RESTART, P_SET_RESTART_FAIL, P_SET_CONFLICT, P_SET_LOST, P_SET_UNAVAILABLE, P_SET_RESET   // settingsnative: the Settings' windows
};
enum KbPurpose : int { K_NONE = 0, K_NAME_LOCAL, K_CODE, K_FRIEND_ADD, K_ADDRESS /* netx */, K_UNBLOCK /* friendsui */, K_CHAT, K_DIRECT_CODE /* flow: a friend's far-away code */, K_REPORT_NOTE /* beta diagnostics: the problem report's note */ };
enum BusyPurpose : int { B_NONE = 0, B_HOST, B_JOIN_CODE, B_QUICK, B_FRIEND, B_LOCAL, B_ADDR /* netx */, B_FRIEND_ADD /* fix3 */,
                         B_REJOIN /* flow */, B_SIGNIN /* flow: the silent sign-in ("Connecting..." only) */,
                         B_ACCT_DELETE /* Friends: the service is removing the online account */ };
// service2: the online account, made or checked without a screen: 1 ready, 0 working (ask again next frame), -(MpUiError) failed
extern "C" int mp_service_ensure_account(void) __attribute__((weak));
// wire: the friend code of the player in `slot`, from the core's player record (port/mp_core_game.h mp_core_slot_player): the service's
// verified code in an online room AND the code a console announced over the game link in a Same Wi-Fi / Nearby / spoken-code room (the
// owner: Add Friend and Played With for players met in ANY kind of game). mp_core_slot_friend_code alone answers only the verified one, which
// left every gathering-screen room without Add Friend / Block / Played With codes. out: at least 10 bytes; true with a non-empty code.
extern "C" bool mp_core_slot_player(int slot, MpPlayerInfo* out) __attribute__((weak));
inline bool RoomPlayerCode(int slot, char* out, size_t cap) {
    if (!out || cap < 10) return false;
    out[0] = 0;
    if (!mp_core_slot_player) return false;
    MpPlayerInfo pi;
    memset(&pi, 0, sizeof pi);
    if (!mp_core_slot_player(slot, &pi)) return false;
    pi.code[9] = 0;
    if (!pi.code[0]) return false;
    memcpy(out, pi.code, 10);
    return true;
}
enum After : int { A_NONE = 0, A_HOSTJOIN, A_BOARD, A_FRIENDS, A_CODE /* flow: Join with a Code */ };
enum SlotPurpose : int { SP_COPY = 1, SP_LEAVE = 3, SP_RECOVER = 4 };
constexpr uint32_t kReopenGuard = 20;    // (frames) fix2: no reopening right after a close (> the pause check's 12-frame hold)
constexpr uint32_t kCharWait = 3 * 60;
constexpr uint32_t kRootRetry = 30, kRootRetrySlow = 120, kRootWarn = 5 * 60;   // (frames) fix2: the save root switch at the file select   // (frames) the host answers a character wish within this, or it was refused

constexpr int kLaggyMs = 220;   // conntest: a slow link (the same step as the player list's two bars and the engine's MP_CT_SLOW_MS)
constexpr uint32_t kFps = 60;
constexpr uint32_t kBusyTimeout = 45 * kFps, kRegisterTimeout = 30 * kFps;
constexpr int kMaxFriends = 32, kMaxRooms = 16, kMaxBoard = 6, kMaxBlocked = 16;

// lists polled from the port (cached each frame the screen is up)
inline MpFriend sFriends[kMaxFriends];
inline int sFriendCount = -1;           // -1 loading, -2 error
// friendsui: players who asked to be friends (0 while loading, on error, or when the service has none), the one being answered, and
// the players blocked from this menu this session (the service has no list of blocked players: this is what Unblock offers; a code
// typed with "By Code" unblocks anyone).
inline MpFriend sIncoming[kMaxFriends];
inline int sIncomingCount = 0;
inline MpFriend sReq;
inline MpFriend sBlocked[kMaxBlocked];
inline int sBlockedCount = 0;
constexpr uint32_t kRejoinFrames = 2 * 60 * 60;   // flow: the Rejoin row stays 2 minutes after a drop (the core keeps the way back 120 s)
constexpr uint32_t kFriendsRefresh = 10 * 60;      // flow: the Join list asks the service for friends' rooms again every 10 s
inline MpBoardRow sBoard[kMaxBoard];
inline int sBoardCount = -1;



// keepsafe API: true when this console has no copy of its multiplayer data from before this version (mp_keepsafe_may_migrate() is false): the Multiplayer menu says so for the whole run

inline void Copy16(char16_t* dst, int cap, const char16_t* src) { MpPanel::copy(dst, cap, src); }
inline void Text(char16_t* dst, int cap, MpText id, const Args& a = Args()) { txt::format(dst, cap, id, a); }
inline int Num(char16_t* dst, int cap, MpText id, int n) {
    Args a;
    a.num = n;
    return txt::format(dst, cap, id, a);
}
inline void Str(char16_t* dst, int cap, MpText id, const char* s) {
    Args a;
    a.str = s;
    txt::format(dst, cap, id, a);
}
// "a · b"
inline void Join16(char16_t* dst, int cap, const char16_t* a, const char16_t* b) {
    int n = 0;
    for (const char16_t* p = a; *p && n < cap - 1; ++p) dst[n++] = *p;
    const char16_t sep[] = u" · ";
    for (const char16_t* p = sep; *p && n < cap - 1; ++p) dst[n++] = *p;
    for (const char16_t* p = b; *p && n < cap - 1; ++p) dst[n++] = *p;
    dst[n] = 0;
}

inline uint8_t LookIcon(int slot) {
    const int look = mp_slot_look(slot);
    return static_cast<uint8_t>(look >= 0 && look < MP_LOOK_COUNT ? MpPanel::ICON_MARIO + look : MpPanel::ICON_MARIO);
}
inline int8_t PingBars(int ms) {
    if (ms < 0) return 0;
    return static_cast<int8_t>(ms < 60 ? 4 : (ms < 120 ? 3 : (ms < 220 ? 2 : 1)));
}
inline MpText ModeName(MpMode m) { return m == MP_MODE_RACE ? MP_T_STAR_RACE : MP_T_PLAY_TOGETHER; }
inline MpText ConnName(MpConn c) {
    return (c == MP_CONN_ONLINE || c == MP_CONN_DIRECT) ? MP_T_INTERNET : (c == MP_CONN_NEARBY ? MP_T_NEARBY : MP_T_SAME_WIFI);
}
// flow: "Who can join" in words (the lobby's line under the title, Room Settings)
inline MpNetInfo NetInfo() {
    MpNetInfo n;
    memset(&n, 0, sizeof n);
    n.wifiUp = true;
    if (mp_ui_net_info) mp_ui_net_info(&n);
    return n;
}
// flow (review 2 #2, #3): a co-op slot is in use when coop.cfg says a session played on it / progress was kept there, or it has
// Power Stars (coop.cfg lost); otherwise it is "New" (-1). A single-player file: MpSaveGame::slotStars (created files only).
// Both read the save file: call when a screen is pushed, never per frame.
inline int CoopStarsNow(int slot) {
    const int s = MpSaveGame::slotStars(true, slot);
    if (s < 0) return -1;
    MpCoopMeta m;
    memset(&m, 0, sizeof m);
    if (s > 0 || (mp_coop_meta_get && mp_coop_meta_get(slot, &m))) return s;
    return -1;
}
inline MpUiPhase UiPhase() {
    MpUiStatus st;
    memset(&st, 0, sizeof st);
    mp_ui_status(&st);
    return st.phase;
}
inline MpFlowStore FlowStore() {
    MpFlowStore f;
    memset(&f, 0, sizeof f);
    if (mp_flow_store_get) mp_flow_store_get(&f);
    return f;
}
inline void SaveFlowStore(const MpFlowStore& f) { if (mp_flow_store_set) mp_flow_store_set(&f); }
// the picture-font star and "x" then the number ("*x12")
inline void StarCount(char16_t* out, int cap, int stars) {
    char16_t v[12];
    v[0] = txt::picChar(txt::PIC_STAR), v[1] = txt::picChar(txt::PIC_TIMES);
    int n = 2, s = stars < 0 ? 0 : stars;
    char16_t d[4];
    int k = 0;
    do { d[k++] = static_cast<char16_t>(u'0' + s % 10); s /= 10; } while (s && k < 3);
    while (k > 0) v[n++] = d[--k];
    v[n] = 0;
    MpPanel::copy(out, cap, v);
}

inline void DefaultSettings(MpSettings* s, MpMode mode) {
    memset(s, 0, sizeof *s);
    s->mode = mode;
    s->maxPlayers = 4;
    s->interaction = 1;
    s->sharedKills = 1;
    s->rivals = 1;           // flow: rivals are ghosts (not a setting any more)
    s->raceWaitSec = 18;     // flow: not a setting any more (units of 10 s: 180 s, the value the core, the gathering and the config all use)
    s->nameTags = 1;
    s->flags = 7 | MP_SET_TEXT_OFF;   // touches: typed messages start off (Room Settings: Typed Messages turns them on)
}

inline bool ValidRoomCode(const char* in, char out[8]) {
    int n = 0;
    for (const char* p = in; *p; ++p) {
        char c = *p;
        if (c == ' ' || c == '-') continue;
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) || n >= 6) return false;
        out[n++] = c;
    }
    out[n] = 0;
    return n == 6;
}

// conntest: the words and the tick / cross (0 none, 1 tick, 2 cross) of one check's result

// a guide-band button (Cross / Circle / Triangle / Square picture and its word): every guide button of a screen comes from here
// (vitabtn: the touch boxes are made from Model::buttons)
inline void ModelButton(MpPanel::Model* m, int pic, MpText label) {
    if (m->buttonCount >= 4) return;
    m->buttons[m->buttonCount].pic = static_cast<uint8_t>(pic);
    Text(m->buttons[m->buttonCount].label, 20, label);
    ++m->buttonCount;
}

// flow look (share/flow/LOOK.md): the file select's look for a screen. Call in model##F after the rows are in: no title band, no
// breadcrumb, no hint, no hand; the one question or line in the game's own bottom message. (Titles / subtitles / hints a family
// still sets are not drawn.)
inline void GameLook(MpPanel::Model* m) { m->look = MpPanel::LOOK_GAME; }   // (a screen whose buttons say it all: no line)
inline void GameLook(MpPanel::Model* m, MpText line, const Args& a = Args()) {
    m->look = MpPanel::LOOK_GAME;
    Text(m->line, 96, line, a);
}
// flow look for a screen that was built with a title (and a hint): the title becomes the line, the hint under it when it is short
inline void GameLookFromTitle(MpPanel::Model* m) {
    m->look = MpPanel::LOOK_GAME;
    int n = 0;
    for (const char16_t* q = m->title; *q && n < 94; ++q) m->line[n++] = *q;
    int hl = 0;
    while (m->hint[hl]) ++hl;
    if (hl > 0 && hl <= 44 && n + 1 + hl < 95) {
        if (n > 0) m->line[n++] = u'\n';
        for (int c = 0; c < hl; c++) m->line[n++] = m->hint[c];
    }
    m->line[n] = 0;
}
inline void GameLookName(MpPanel::Model* m, MpText line, const char* name) {   // a line with the player's name in it ("Waiting for %s...")
    m->look = MpPanel::LOOK_GAME;
    Str(m->line, 96, line, name);
}
// flow look: the players as heads over the big buttons (KIND_HEADS). The screen's items stay its buttons (the cursor, the touch,
// choose##F know nothing of the heads); this puts n heads in front of the rows the core made. fill(i, row) fills head i: label =
// name, icon = MpPanel::ICON_MARIO + look, marks MARK_CROWN (the first player) / MARK_YOU / MARK_DISABLED (a place still waiting).
template <class Fill>
inline void Heads(MpPanel::Model* m, int n, Fill fill) {
    using namespace MpPanel;
    if (n > 4) n = 4;
    if (n < 0) n = 0;
    if (m->rowCount + n > kMaxRows) m->rowCount = kMaxRows - n;
    for (int i = m->rowCount - 1; i >= 0; i--) m->rows[i + n] = m->rows[i];
    for (int i = 0; i < n; i++) {
        m->rows[i] = Row();
        memset(m->rows[i].lead, 0, sizeof m->rows[i].lead);
        memset(m->rows[i].label, 0, sizeof m->rows[i].label);
        memset(m->rows[i].value, 0, sizeof m->rows[i].value);
        m->rows[i].ping = -1, m->rows[i].owner = -1;
        fill(i, m->rows[i]);
    }
    m->rowCount += n;
    m->headCount = n;
    if (n > 0) m->kind = KIND_HEADS;   // (no heads yet: the screen's own kind)
    if (m->cursor >= 0) m->cursor += n;
    // (m->decided stays: it is the button's place among the buttons, as the buttons' banks count them)
}

// ------------------------------------------------------------------ conntest (m-conntest): the Connection Test's three answers
// The screen answers what a player asks, in this order: "Can I play with people in this room?" (room), "Can a friend far away join me?"
// (far: the spoken-code route, which needs a home network that lets a friend in), "Can I play with anyone online?" (online: the online
// service; absent when the build has none). Each is one verdict and at most one short line of what to do (an index into the word table).
enum CtVerdict : uint8_t { CTV_UNKNOWN = 0, CTV_GOOD, CTV_CHOPPY, CTV_NOT_NOW };
struct CtAnswer { uint8_t verdict; bool ready; int8_t line; };   // ready: the checks behind it have finished; line: word index, -1 none
struct CtAnswers { CtAnswer room, far, online; bool onlineAbsent; };
// The pure judgement (the screen, the queries and the tests all use it). serviceBuilt: this build has an online service; clockWrong: the
// console's date and time are wrong (the service check failed for that reason); relayOnly: the Wi-Fi privacy setting is on.
CtAnswers ConnTestJudge(const MpCtView& v, bool serviceBuilt, bool clockWrong, bool relayOnly);
// For the gathering screen (flow): the last finished test's answer for "a friend far away", CTV_UNKNOWN when no test has finished, when one
// is running, or when the console's Wi-Fi is not as it was when the test ran. CTV_NOT_NOW means the spoken code cannot work from here.
CtVerdict ConnTestFarVerdict();
// True when the spoken-code route is NOT_NOW and the online service is reachable (good or choppy): steer the player to friends there.
bool ConnTestSteerFarToOnline();
// The screen's own words (a five-language stopgap until flow moves them into MpMessages): count, and one word's text in a language (Lang).
int ConnTestWordCount();
const char16_t* ConnTestWord(int index, int lang);

}  // namespace MpMenu
