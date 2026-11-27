// Galaxy 3.0 multiplayer: the menus, the friends family: Friends, Friend Requests, a request / a friend, Blocked Players, Played With, Leaderboards
// (flow split of MpMenuSequence.cpp, 11 Nov; m-friends rework, 11 Nov).
// Owner of this family's screens: see MpMenuSequence.cpp "the file map". Pure code, as MpMenuSequence.cpp.
//
// What this family does that the core's lists do not:
//  - SIGN-IN WITHOUT A SCREEN: every screen here calls mp_service_ensure_account() each frame; while it answers 0 the screen says only
//    "Connecting...", on 1 it shows what the player asked for, on a failure one plain line (busy / no Wi-Fi / set the date and time /
//    newer version / no service) and only Back.
//  - NAMES ARE NOT UNIQUE, THE FRIEND CODE IS: two rows with the same name both show the tail of their code; a friend's page shows the code.
//  - THE LISTS REACH THE SERVICE'S LIMITS (friends 50, blocked 200) although the core's lists stop at 32 / 16 / 40 rows: this family keeps its
//    own copies and turns its own pages (Frame::param = the page; items() answers one page; Up / Down walk the whole list, Left / Right turn a page).
#include <type_traits>
#include <utility>

#include "MpMenuInternal.hpp"
#include "Game/MP/MpListRefreshLogic.hpp"   // uxwait: the Friends list asks the service again by itself, within the quota rules

extern "C" bool mp_update_take_notice(void) __attribute__((weak));   // release3: port/mp_update.h (platform side: not in the game's include path)
// service2 (platform/src/mp/mp_service.h): 1 ready, 0 working, <0 = -(MpUiError) failed. Weak: tools and tests that link the menus without
// the service see "ready" (and the screens then behave as before).
extern "C" int mp_service_ensure_account(void) __attribute__((weak));
// core2 (port/mp_core_game.h): the friend code a player's console announced over the game link, "XXXX-XXXX"; false while unknown.
extern "C" bool mp_core_slot_friend_code(int slot, char* out, size_t cap) __attribute__((weak));

namespace MpMenu {

static_assert(I_CONNTEST < I_LIST0, "the friends family's item ids run into the list rows");

namespace {

constexpr int kFriendsMax = 50, kIncomingMax = 50, kBlockedMax = 200, kPlayedMax = 20, kPage = MpPanel::kListRows;
constexpr int kFirstRows = 3;   // the Friends screen's first page: the big code box takes the room of two rows
constexpr int kEntMax = kBlockedMax + 4;

// ---------------------------------------------------------------- the code: "1234-5678" == "12345678" == "1234 5678" == "abcd-efgh"
// 8 letters / digits after dropping spaces and dashes, upper case, "XXXX-XXXX". The service has the last word on which letters exist.
bool NormalizeCode(const char* in, char out[10]) {
    char k[9];
    int n = 0;
    for (; in && *in; ++in) {
        char c = *in;
        if (c == '-' || c == ' ') continue;
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) || n >= 8) return false;
        k[n++] = c;
    }
    if (n != 8) return false;
    memcpy(out, k, 4);
    out[4] = '-';
    memcpy(out + 5, k + 4, 4);
    out[9] = 0;
    return true;
}
bool SameCode(const char* x, const char* y) {
    char a[10], b[10];
    return NormalizeCode(x, a) && NormalizeCode(y, b) && !strcmp(a, b);
}
// "XXXX XXXX" for the big box (the space is a gap, to read it aloud in two halves)
void GroupCode(const char* code, char16_t* out, int cap) {
    char n[10];
    out[0] = 0;
    if (!NormalizeCode(code, n) || cap < 10) return;
    int k = 0;
    for (int i = 0; i < 9; i++) out[k++] = n[i] == '-' ? u' ' : static_cast<char16_t>(n[i]);
    out[k] = 0;
}
bool SameName(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
        if (x != y) return false;
    }
    return !*a && !*b;
}
// the name, and "(5678)" after it when another row of the same list has the same name (names are not unique: the code is)
void NameWithTail(char16_t* dst, int cap, const char* name, const char* code, bool dup) {
    char b[48];
    const size_t cl = code ? strlen(code) : 0;
    if (dup && cl >= 4) snprintf(b, sizeof b, "%s (%s)", name, code + cl - 4);
    else snprintf(b, sizeof b, "%s", name);
    txt::utf8To16(dst, cap, b);
}

// (wire: the English placeholders that stood here are table words now: MP_T_MY_FRIEND_CODE ... MP_T_SVC_OFFLINE)

// ---------------------------------------------------------------- silent sign-in
enum Gate : uint8_t { GATE_WORKING, GATE_READY, GATE_FAILED };
enum Why : uint8_t { WHY_BUSY, WHY_NO_WIFI, WHY_CLOCK, WHY_UPDATE, WHY_OFFLINE };
Gate gGate = GATE_WORKING;
Why gWhy = WHY_BUSY;

Why WhyOf(int r) {
    const char* code = mp_service_last_code ? mp_service_last_code() : nullptr;
    if (code && *code) {
        if (!strcmp(code, "clock")) return WHY_CLOCK;
        if (!strcmp(code, "no_wifi")) return WHY_NO_WIFI;
        if (!strcmp(code, "update") || !strcmp(code, "update_required")) return WHY_UPDATE;
        if (!strcmp(code, "offline_build")) return WHY_OFFLINE;
        if (!strcmp(code, "busy")) return WHY_BUSY;
    }
    if (r == -static_cast<int>(MP_ERR_NO_WIFI)) return WHY_NO_WIFI;
    if (r == -static_cast<int>(MP_ERR_UPDATE_REQUIRED)) return WHY_UPDATE;
    if (r == -static_cast<int>(MP_ERR_CLOCK)) return WHY_CLOCK;
    return WHY_BUSY;
}
// once per frame on a screen that needs the service; true when the screen may show its list
bool SignIn() {
    const int r = mp_service_ensure_account ? mp_service_ensure_account() : 1;
    if (r > 0) gGate = GATE_READY;
    else if (r == 0) gGate = GATE_WORKING;
    else gGate = GATE_FAILED, gWhy = WhyOf(r);
    return gGate == GATE_READY;
}
// uxwait: the Friends / Friend Requests lists (the only lists that come from the online service: who is online, who can be joined) refresh
// themselves while on screen: MpListRefresh::Pacer (30 s, backoff 60/120/240 on errors, stop after 3 minutes without a press or 20 requests), Start = Refresh by hand.
// One request per refresh (GET /v1/friends answers friends and incoming requests together): about 2 a minute at most.
MpListRefresh::Pacer gFriendsPacer;
uint32_t gPacerFrame = 0;     // the last frame one of the two screens ticked (a gap = the screen was left and entered again)
int64_t gPacerNowMs = 0;      // frames since multiplayer was first opened this boot (the menu system runs every frame from then on, also while playing): about wall time; the hourly ceiling survives leaving and entering
int64_t gClockFrames = 0;
uint32_t gClockLast = 0;
bool gKeepRows = false;       // a refresh by itself failed: the list on screen stays (not "Couldn't load")
bool gAutoAsk = false;        // the request in flight is the pacer's own: the rows stay as they are until its answer is taken
bool gWasLoading = false;     // the screen's own (loud) request was running last frame
bool Gated(Scr s) { return s == S_FRIENDS || s == S_REQUESTS || s == S_BLOCKED || s == S_PLAYED || s == S_BOARD; }
bool Paged(Scr s) { return s == S_FRIENDS || s == S_REQUESTS || s == S_BLOCKED || s == S_PLAYED; }

// ---------------------------------------------------------------- this family's copies of the lists (the core's stop at 32 / 16)
MpFriend gFriends[kFriendsMax];
int gFriendN = 0;
MpFriend gIncoming[kIncomingMax];
int gIncomingN = 0;
MpFriend gBlocked[kBlockedMax];   // the players this console blocked this session, newest first (the service lists none to this client)
int gBlockedN = 0;
uint32_t gLastFrame = 0;

void MirrorBlocked() { sBlockedCount = gBlockedN < kMaxBlocked ? gBlockedN : kMaxBlocked; for (int i = 0; i < sBlockedCount; i++) sBlocked[i] = gBlocked[i]; }
void NoteBlocked(const MpFriend& f) {
    int at = gBlockedN < kBlockedMax ? gBlockedN : kBlockedMax - 1;
    for (int i = 0; i < gBlockedN; i++)
        if (SameCode(gBlocked[i].code, f.code)) { at = i; break; }
    for (int i = at; i > 0; i--) gBlocked[i] = gBlocked[i - 1];
    gBlocked[0] = f;
    if (at == gBlockedN) ++gBlockedN;
    MirrorBlocked();
}
bool IsFriend(const char* code) {
    for (int i = 0; i < gFriendN; i++)
        if (SameCode(gFriends[i].code, code)) return true;
    return false;
}
int FriendRow(const char* code) {
    for (int i = 0; i < gFriendN; i++)
        if (SameCode(gFriends[i].code, code)) return i;
    return -1;
}

// ---------------------------------------------------------------- Played With
// flow.cfg keeps the names (MP_PLAYED_WITH of them, newest first); a friend code is known only if the store has room for one
// (core-wanted: MpFlowStore::playedCode[MP_PLAYED_WITH][10]) or this boot learned it (RAM, tied to the store's name at that place).
template <class T, class = void> struct HasCodes : std::false_type {};
template <class T> struct HasCodes<T, std::void_t<decltype(std::declval<T&>().playedCode[0][0])>> : std::true_type {};
constexpr bool kStoreCodes = HasCodes<MpFlowStore>::value;
constexpr int kMetCap = MP_PLAYED_WITH < kPlayedMax ? MP_PLAYED_WITH : kPlayedMax;

struct Met { char name[MP_NAME_BYTES + 1]; char code[10]; };
struct RamCode { char name[MP_NAME_BYTES + 1]; char code[10]; };
RamCode gRam[kPlayedMax];   // what this boot learned, by place in the store (valid while the store's name there is the same)
struct Seen { char name[MP_NAME_BYTES + 1]; char code[10]; };
Seen gSeen[MP_MAX_PLAYERS];   // who was noted in each slot (one player is noted once per stay)

template <class T> void StoreCode(T& fs, int i, char* out) {
    out[0] = 0;
    if constexpr (HasCodes<T>::value) { memcpy(out, fs.playedCode[i], 9), out[9] = 0; }
    (void)fs, (void)i;
}
template <class T> void SetStoreCode(T& fs, int i, const char* code) {
    if constexpr (HasCodes<T>::value) { memset(fs.playedCode[i], 0, sizeof fs.playedCode[i]), snprintf(fs.playedCode[i], sizeof fs.playedCode[i], "%s", code); }
    (void)fs, (void)i, (void)code;
}

int LoadMet(Met* out) {
    const MpFlowStore fs = FlowStore();
    const int n = fs.playedCount < kMetCap ? fs.playedCount : kMetCap;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof out[i]);
        memcpy(out[i].name, fs.playedWith[i], MP_NAME_BYTES);
        out[i].name[MP_NAME_BYTES] = 0;
        if (kStoreCodes) StoreCode(fs, i, out[i].code);
        char norm[10];
        if (!out[i].code[0] && gRam[i].code[0] && !strcmp(gRam[i].name, out[i].name)) snprintf(out[i].code, sizeof out[i].code, "%s", gRam[i].code);
        if (out[i].code[0] && NormalizeCode(out[i].code, norm)) snprintf(out[i].code, sizeof out[i].code, "%s", norm);
        else out[i].code[0] = 0;
    }
    return n;
}
void SaveMet(const Met* m, int n) {
    MpFlowStore fs = FlowStore();
    fs.playedCount = static_cast<uint8_t>(n);
    memset(fs.playedWith, 0, sizeof fs.playedWith);
    memset(gRam, 0, sizeof gRam);
    for (int i = 0; i < n; i++) {
        snprintf(fs.playedWith[i], sizeof fs.playedWith[i], "%s", m[i].name);
        SetStoreCode(fs, i, m[i].code);
        snprintf(gRam[i].name, sizeof gRam[i].name, "%s", m[i].name);
        snprintf(gRam[i].code, sizeof gRam[i].code, "%s", m[i].code);
    }
    SaveFlowStore(fs);
}
// every player met, newest first, but those with a friend code first (their rows can be chosen: Add Friend / Block / their page);
// the others (no code known: an older console, a code lost) follow, shown dim and not choosable (no dead rows). Returns the count; *nAct
// = how many have a code.
int SortedMet(Met* out, int* nAct = nullptr) {
    Met all[kPlayedMax];
    const int n = LoadMet(all);
    int k = 0;
    for (int i = 0; i < n; i++)
        if (all[i].code[0]) out[k++] = all[i];
    const int act = k;
    for (int i = 0; i < n; i++)
        if (!all[i].code[0]) out[k++] = all[i];
    if (nAct) *nAct = act;
    return k;
}
void DropMet(const char* code) {
    Met all[kPlayedMax];
    int n = LoadMet(all), k = 0;
    for (int i = 0; i < n; i++)
        if (!all[i].code[0] || !SameCode(all[i].code, code)) all[k++] = all[i];
    if (k != n) SaveMet(all, k);
}
// the code of a player in the room (core2 first, flow's older hook second); sanitised: a stranger's console can announce anything
bool SlotCode(int slot, char out[10]) {
    char raw[16];
    raw[0] = 0;
    bool got = false;
    got = RoomPlayerCode(slot, raw, sizeof raw);   // wire: verified or announced (any kind of game)
    if (!got && mp_core_slot_friend_code) got = mp_core_slot_friend_code(slot, raw, sizeof raw);
    if (!got && mp_ui_slot_friend_code) got = mp_ui_slot_friend_code(slot, raw);
    raw[15] = 0;
    return got && raw[0] && NormalizeCode(raw, out);
}
// a new Sequence (a restart of the system, a test) starts its frames from 0: forget what belonged to the old one
void Resync(uint32_t frame, bool force = false) {
    if (!force && frame >= gLastFrame && sBlockedCount == (gBlockedN < kMaxBlocked ? gBlockedN : kMaxBlocked)) {
        gLastFrame = frame;
        return;
    }
    gLastFrame = frame;
    gFriendN = gIncomingN = 0;
    gBlockedN = 0;
    sBlockedCount = 0;
    gGate = GATE_WORKING;
    memset(gRam, 0, sizeof gRam);
    memset(gSeen, 0, sizeof gSeen);
}
// ---------------------------------------------------------------- the rows of a screen, as one list of entries
enum Kind : uint8_t { E_PLAYED, E_REQUESTS, E_FRIEND, E_BLOCKROW, E_REQUEST, E_BLOCKED, E_MET, E_DELACCT };
bool HasAccount() { MpProfile p; memset(&p, 0, sizeof p); mp_ui_profile(&p); return p.registered; }
struct Ent { Kind kind; int16_t idx; };

int Entries(Scr s, Ent* out, int cap) {
    int n = 0;
    auto add = [&](Kind k, int i) { if (n < cap) out[n++] = {k, static_cast<int16_t>(i)}; };
    Met met[kPlayedMax];
    switch (s) {
    case S_FRIENDS:
        if (SortedMet(met) > 0) add(E_PLAYED, 0);
        if (sIncomingCount > 0) add(E_REQUESTS, 0);
        for (int i = 0; sFriendCount >= 0 && i < gFriendN; i++) add(E_FRIEND, i);
        if (sFriendCount >= 0) add(E_BLOCKROW, 0);
        if (sFriendCount >= 0 && HasAccount()) add(E_DELACCT, 0);   // (the last row, and only for a console that has an online account)
        break;
    case S_REQUESTS:
        for (int i = 0; i < gIncomingN; i++) add(E_REQUEST, i);
        break;
    case S_BLOCKED:
        for (int i = 0; i < gBlockedN; i++) add(E_BLOCKED, i);
        break;
    case S_PLAYED:
        for (int i = 0, m = SortedMet(met); i < m; i++) add(E_MET, i);
        break;
    default: break;
    }
    return n;
}
int FirstRows(Scr s) { return s == S_FRIENDS ? kFirstRows : kPage; }
int PageStart(Scr s, int page) { return page <= 0 ? 0 : FirstRows(s) + (page - 1) * kPage; }
int PageOf(Scr s, int e) { return e < FirstRows(s) ? 0 : 1 + (e - FirstRows(s)) / kPage; }
int PageCount(Scr s, int total) { return total <= FirstRows(s) ? 1 : PageOf(s, total - 1) + 1; }

bool GatedReady(Scr s) { return !Gated(s) || gGate == GATE_READY; }

}  // namespace

// ---------------------------------------------------------------- the lists polled from the service
void Sequence::pollFriends() {
    static MpFriend rows[kFriendsMax];
    static MpFriend inc[kIncomingMax];
    Resync(mFrame);
    const int n = mp_ui_friends_get(rows, kFriendsMax);
    if (gAutoAsk) {   // uxwait: a refresh by itself is quiet: no "Loading...", an error keeps the list, and nothing moves under a window
        if (sFriendCount == -1) gAutoAsk = false;   // (the screen asked loudly meanwhile: it is the same request, shown as loading)
        else if (n == -1 || mDlg || mKb) return;
        else {
            gAutoAsk = false;
            gFriendsPacer.answered(gPacerNowMs, n >= 0);
            gKeepRows = n < 0;
            if (n < 0) return;
        }
    } else if (gKeepRows) {   // (the service keeps answering "error" after a failed refresh: the old rows stay until a request succeeds)
        if (sFriendCount == -1 || n >= 0) gKeepRows = false;
        else return;
    }
    // uxwait: the cursor stays on the same FRIEND (by code) when a refresh changed the list, not on the same row number
    char keepCode[10] = "";
    if (n >= 0 && mDepth > 0 && top().scr == S_FRIENDS && sFriendCount >= 0) {
        Ent ents[kEntMax];
        const int total = Entries(S_FRIENDS, ents, kEntMax);
        const Frame& f = top();
        int it[kItems];
        const int cnt = items(S_FRIENDS, f.param, it);
        if (f.cursor >= 0 && f.cursor < cnt && it[f.cursor] >= I_LIST0) {
            const int e = PageStart(S_FRIENDS, f.param) + (it[f.cursor] - I_LIST0);
            if (e < total && ents[e].kind == E_FRIEND && ents[e].idx < gFriendN) snprintf(keepCode, sizeof keepCode, "%s", gFriends[ents[e].idx].code);
        }
    }
    if (n >= 0) {
        const int c = n > kFriendsMax ? kFriendsMax : n;
        for (int i = 0; i < c; i++) {
            gFriends[i] = rows[i];
            gFriends[i].name[MP_NAME_BYTES] = 0;
            gFriends[i].code[9] = 0;
        }
        gFriendN = c;
        // (the Join list's friends' rooms and the older screens read the core's copy: its first kMaxFriends)
        sFriendCount = c > kMaxFriends ? kMaxFriends : c;
        for (int i = 0; i < sFriendCount; i++) sFriends[i] = gFriends[i];
        // friendsui: an error here (an older service) just means no requests are shown
        const int m = mp_ui_incoming_get(inc, kIncomingMax);
        const int ci = m > kIncomingMax ? kIncomingMax : (m > 0 ? m : 0);
        for (int i = 0; i < ci; i++) {
            gIncoming[i] = inc[i];
            gIncoming[i].name[MP_NAME_BYTES] = 0;
            gIncoming[i].code[9] = 0;
        }
        gIncomingN = ci;
        sIncomingCount = ci > kMaxFriends ? kMaxFriends : ci;
        for (int i = 0; i < sIncomingCount; i++) sIncoming[i] = gIncoming[i];
        if (keepCode[0]) {
            Ent ents[kEntMax];
            const int total = Entries(S_FRIENDS, ents, kEntMax);
            for (int e = 0; e < total; e++) {
                if (ents[e].kind != E_FRIEND || strcmp(gFriends[ents[e].idx].code, keepCode) != 0) continue;
                Frame& f = top();
                const int page = PageOf(S_FRIENDS, e);
                int it[kItems];
                const int cnt = items(S_FRIENDS, static_cast<uint8_t>(page), it);
                for (int k = 0; k < cnt; k++)
                    if (it[k] == I_LIST0 + (e - PageStart(S_FRIENDS, page))) { f.param = static_cast<uint8_t>(page); f.cursor = static_cast<int8_t>(k); break; }
                break;
            }
        }
    } else {
        sFriendCount = n < -1 ? -2 : -1;
        sIncomingCount = 0;
        gFriendN = gIncomingN = 0;
    }
}

void Sequence::requestBoard() {
    sBoardCount = -1;
    mBoardDirty = false;
    mp_ui_board_request(static_cast<uint8_t>(mBoardGalaxy), static_cast<uint8_t>(mBoardStar), static_cast<uint8_t>(mBoardTab));
    mp_log("ui: board request galaxy=%d star=%d tab=%d", mBoardGalaxy, mBoardStar, mBoardTab);
}

// ------------------------------------------------------------------ friend requests and blocked players (friendsui)
// uxwait: may the screen make its own request on entering? Not above the hourly ceiling (30 requests in a rolling hour): then the rows it
// already has are shown as they are. (No rows yet: the error line, no request.)
bool Sequence::friendsEntryAllowed() {
    const int64_t nowMs = (gClockFrames + (mFrame >= gClockLast ? mFrame - gClockLast : mFrame)) * 1000 / kFps;
    if (gFriendsPacer.underCeiling(nowMs)) return true;
    mp_log("ui: friends list not asked on entering (30 requests in the last hour)");
    return false;
}

void Sequence::refreshFriends() {
    sFriendCount = -1;
    sIncomingCount = 0;
    mp_ui_friends_request();
}

void Sequence::chooseRequest(int item) {
    Args a;
    a.str = sReq.name;
    if (item == I_REQ_BLOCK) {
        if (mIo) mIo->sound(Se::Decide);
        ask(P_REQ_BLOCK, MP_T_DLG_REQ_BLOCK, a, Dlg::YesNo);
        return;
    }
    const bool add = item == I_REQ_ADD;
    if (add ? mp_ui_friend_add(sReq.code) : mp_ui_friend_dismiss(sReq.code)) {
        mp_log("ui: request %s", add ? "accepted" : "dismissed");
        if (mIo) mIo->sound(Se::Decide);
        refreshFriends();
        if (mDepth > 0 && top().scr == S_REQUEST) pop();
        if (add) say(P_INFO, MP_T_DLG_REQ_ACCEPTED, a);
    } else {
        if (mIo) mIo->sound(Se::Buzzer);
        say(P_INFO, MP_T_DLG_REQ_FAILED);
    }
}

void Sequence::blockRequest() {
    Args a;
    a.str = sReq.name;
    if (!mp_ui_friend_block(sReq.code)) {
        if (mIo) mIo->sound(Se::Buzzer);
        say(P_INFO, MP_T_DLG_REQ_FAILED);
        return;
    }
    mp_log("ui: request blocked");
    Resync(mFrame);
    NoteBlocked(sReq);   // newest first; the same player is listed once
    DropMet(sReq.code);
    refreshFriends();
    if (mDepth > 0 && top().scr == S_REQUEST) pop();
    say(P_INFO, MP_T_DLG_REQ_BLOCKED, a);
}

void Sequence::unblockAt(int index, const char* code) {
    char norm[10];
    if (!NormalizeCode(code, norm)) {   // a typed code that cannot be one: said plainly, nothing sent
        if (mIo) mIo->sound(Se::Buzzer);
        say(P_INFO, MP_T_ERR_NO_PLAYER);
        return;
    }
    if (!mp_ui_friend_unblock(norm)) {
        if (mIo) mIo->sound(Se::Buzzer);
        say(P_INFO, MP_T_DLG_REQ_FAILED);
        return;
    }
    mp_log("ui: player unblocked");
    Resync(mFrame);
    char who[MP_NAME_BYTES + 1];
    snprintf(who, sizeof who, "%s", norm);
    for (int i = 0; i < gBlockedN; i++) {
        if (i != index && !(index < 0 && SameCode(gBlocked[i].code, norm))) continue;
        snprintf(who, sizeof who, "%s", gBlocked[i].name);
        for (int k = i; k + 1 < gBlockedN; k++) gBlocked[k] = gBlocked[k + 1];
        --gBlockedN;
        break;
    }
    MirrorBlocked();
    if (mIo) mIo->sound(Se::Decide);
    Args a;
    a.str = who;
    say(P_INFO, MP_T_DLG_UNBLOCKED, a);
}

const char* Sequence::playedCode(const char* name) const {
    static char code[10];
    Met met[kPlayedMax];
    for (int i = 0, n = SortedMet(met); i < n; i++)
        if (met[i].code[0] && !strcmp(met[i].name, name)) return snprintf(code, sizeof code, "%s", met[i].code), code;
    return nullptr;
}

int Sequence::playedCount() const {
    Met met[kPlayedMax];
    return SortedMet(met);
}

// flow: who is in this session (for "Played with" and the co-op file's names), noted while playing.
// m-friends: a player is told apart by the friend code the other console announced (two players may share a name); a code that
// arrives later fills in a name noted without one.
void Sequence::notePlayers() {
    Resync(mFrame);
    if (mNotedCount == 0) memset(gSeen, 0, sizeof gSeen);   // (a new session: nobody noted yet)
    bool changed = false;
    bool metChanged = false;
    Met met[kPlayedMax];
    int metN = LoadMet(met);
    for (int k = 0; k < MP_MAX_PLAYERS; k++) {
        if (!mp_slot_present(k)) {
            gSeen[k].name[0] = gSeen[k].code[0] = 0;
            continue;
        }
        const char* name = mp_slot_name(k);
        if (!name || !name[0]) continue;
        if (k != mp_local_slot()) {
            char code[10] = {0};
            SlotCode(k, code);
            // svcfix3: a shared name is shown with its label ("Luma#4DXT"); the list of players met keeps the name itself (it adds the tail
            // again where two of them look alike)
            char plain[MP_NAME_BYTES + 1];
            snprintf(plain, sizeof plain, "%s", name);
            {
                const size_t pl = strlen(plain), cl = strlen(code);
                if (cl >= 4 && pl > 5 && plain[pl - 5] == '#' && !strcmp(plain + pl - 4, code + cl - 4)) plain[pl - 5] = 0;
            }
            if (strcmp(gSeen[k].name, name) || strcmp(gSeen[k].code, code)) {
                snprintf(gSeen[k].name, sizeof gSeen[k].name, "%s", name);
                snprintf(gSeen[k].code, sizeof gSeen[k].code, "%s", code);
                // the same player: the same code; or the same name noted before without a code (this one brings it)
                int at = -1;
                for (int i = 0; i < metN && at < 0; i++)
                    if (code[0] && met[i].code[0] ? SameCode(met[i].code, code) : (!met[i].code[0] || !code[0]) && !strcmp(met[i].name, plain)) at = i;
                const bool isNew = at < 0;
                if (isNew) at = metN < kMetCap ? metN : kMetCap - 1;
                Met m;
                memset(&m, 0, sizeof m);
                snprintf(m.name, sizeof m.name, "%s", plain);
                snprintf(m.code, sizeof m.code, "%s", code);
                if (!code[0] && !isNew) snprintf(m.code, sizeof m.code, "%s", met[at].code);   // (never lose a code already known)
                for (int i = at; i > 0; i--) met[i] = met[i - 1];
                met[0] = m;
                if (isNew && metN < kMetCap) ++metN;
                metChanged = true;
                mp_log("ui: played with %s", plain);
            }
        }
        // each name once per session (the first time it is seen while playing)
        bool seen = false;
        for (int i = 0; i < mNotedCount && !seen; i++) seen = !strcmp(mNoted[i], name);
        if (seen || mNotedCount >= kNoted) continue;
        snprintf(mNoted[mNotedCount++], sizeof mNoted[0], "%s", name);
        // the session's players (the host first: slot order), for coop.cfg
        if (mSessionCount < 4) {
            snprintf(mSessionNames[mSessionCount], sizeof mSessionNames[0], "%s", name);
            mSessionLooks[mSessionCount] = static_cast<uint8_t>(mp_slot_look(k));
            ++mSessionCount;
            changed = true;
        }
    }
    if (metChanged) SaveMet(met, metN);
    if (changed && mPlayCoop > 0) writeCoopMeta(mPlayCoop);
}

bool Sequence::itemsFriends(Scr s, uint8_t param, int* out, int* np) const {
    int& n = *np;
    auto add = [&](int i) { if (n < kItems) out[n++] = i; };
    switch (s) {
    case S_FRIENDS:
    case S_REQUESTS:
    case S_BLOCKED:
    case S_PLAYED:
        {
        // one page of the list (Frame::param is the page); nothing while the screen is signing in
        if (!GatedReady(s)) break;
        Resync(mFrame);
        Ent ents[kEntMax];
        const int total = Entries(s, ents, kEntMax);
        int p = param;
        if (PageStart(s, p) >= total && total > 0) p = PageCount(s, total) - 1;
        const int from = PageStart(s, p), to = from + (p == 0 ? FirstRows(s) : kPage);
        int act = 0;
        Met met[kPlayedMax];
        if (s == S_PLAYED) SortedMet(met, &act);
        for (int e = from; e < total && e < to; e++) {
            switch (ents[e].kind) {
            case E_MET: if (ents[e].idx < act) add(I_LIST0 + (e - from)); break;   // (no code known: shown, not choosable)
            case E_PLAYED: add(I_PLAYED); break;
            case E_REQUESTS: add(I_REQUESTS); break;
            case E_BLOCKROW: add(I_BLOCKED); break;
            case E_DELACCT: add(I_DELACCT); break;
            default: add(I_LIST0 + (e - from)); break;
            }
        }
        break;
    }
    case S_REQUEST:
        // one request (Add / Not Now / Block), or a friend (Join if they are in a game / Remove / Block)
        if (param == 1) {
            if (sReq.joinable) add(I_FR_JOIN);
            add(I_FR_REMOVE);
            add(I_REQ_BLOCK);
        } else {
            add(I_REQ_ADD), add(I_REQ_NOTNOW), add(I_REQ_BLOCK);
        }
        break;
    case S_BOARD:
        if (GatedReady(s)) add(I_GALAXY), add(I_STARPICK);
        break;
    default: return false;
    }
    (void)add;
    return true;
}

bool Sequence::chooseFriends(int item) {
    Io* io = mIo;
    auto decide = [io] { if (io) io->sound(Se::Decide); };
    switch (item) {
    case I_PLAYED:
        decide(); push(S_PLAYED); return true;
    case I_REQUESTS:
        decide(); push(S_REQUESTS); return true;
    case I_BLOCKED:
        decide(); push(S_BLOCKED); return true;
    case I_DELACCT:
        decide();
        ask(P_DELETE_ACCOUNT, MP_T_DLG_DELETE_ACCOUNT, Args(), Dlg::YesNo, 0);   // (Yes only: any other way the window closes is No)
        return true;
    case I_GALAXY:
    case I_STARPICK:
        return true;
    case I_REQ_ADD: case I_REQ_NOTNOW: case I_REQ_BLOCK:
        chooseRequest(item); return true;
    case I_FR_JOIN:
        // a friend in a game: join it (the same as the Join list's row)
        if (!sReq.joinable) { if (io) io->sound(Se::Buzzer); return true; }
        decide();
        mOriginalHost = false;
        mConn = MP_CONN_ONLINE;
        mMode = sReq.mode == MP_MODE_RACE ? MP_MODE_RACE : MP_MODE_TOGETHER;
        if (mp_ui_join_friend(sReq.code)) { MpChat::noteRoomPublic(false); startBusy(B_FRIEND); }
        else showError(failError(MP_ERR_ROOM_NOT_FOUND));
        return true;
    case I_FR_REMOVE:
        {
        decide();
        Args a;
        a.str = sReq.name;
        ask(P_FRIEND_REMOVE, MP_T_DLG_FRIEND_REMOVE, a, Dlg::YesNo, FriendRow(sReq.code));
        return true;
    }
    default: break;
    }
    if (item >= I_LIST0 && item < I_FROOM0 && mDepth > 0) {
        const Scr scr = top().scr;
        if (scr != S_FRIENDS && scr != S_REQUESTS && scr != S_BLOCKED && scr != S_PLAYED) return false;
        Ent ents[kEntMax];
        const int total = Entries(scr, ents, kEntMax);
        const int e = PageStart(scr, top().param) + (item - I_LIST0);
        if (e >= total) return true;
        const Ent ent = ents[e];
        switch (ent.kind) {
        case E_FRIEND:
            decide();
            sReq = gFriends[ent.idx];
            push(S_REQUEST, 1);   // the friend's page: their code, Join, Remove, Block
            return true;
        case E_REQUEST:
            decide();
            sReq = gIncoming[ent.idx];
            push(S_REQUEST, 0);
            return true;
        case E_BLOCKED:
            unblockAt(ent.idx, gBlocked[ent.idx].code);
            return true;
        case E_MET:
            {   // already a friend: their page; else Add Friend with the code the other console announced
            Met met[kPlayedMax];
            SortedMet(met);
            const Met& m = met[ent.idx];
            const int fr = FriendRow(m.code);
            decide();
            if (fr >= 0) {
                sReq = gFriends[fr];
                push(S_REQUEST, 1);
                return true;
            }
            snprintf(mFriendAdding, sizeof mFriendAdding, "%s", m.code);
            if (mp_ui_friend_add(m.code)) startBusy(B_FRIEND_ADD);
            else showServiceError(MP_ERR_INTERNAL, MP_T_DLG_FRIEND_FAILED);
            return true;
        }
        default: return true;
        }
    }
    return false;
}

bool Sequence::dialogFriends(int purpose, bool yes, int param) {
    switch (purpose) {
    case P_DELETE_ACCOUNT:
        if (yes) {
            if (mp_service_account_delete && mp_service_account_delete()) {
                mp_log("ui: delete my online account asked");
                startBusy(B_ACCT_DELETE);   // (the answer decides: the account stays until the service has said yes)
            } else {
                showServiceError(MP_ERR_SERVICE_DOWN, MP_T_ERR_SERVICE_DOWN);   // (the plain error)
            }
        }
        return true;
    case P_FRIEND_REMOVE:
        if (yes && param >= 0 && param < gFriendN) {
            mp_ui_friend_remove(gFriends[param].code);
            mp_ui_friends_request();
            sFriendCount = -1;
            if (mDepth > 0 && top().scr == S_REQUEST) pop();   // (their page: back to the list)
        }
        return true;
    case P_REQ_BLOCK:
        if (yes) blockRequest();
        return true;
    case P_MET_BLOCK:
        {
        if (!yes) return true;
        Met met[kPlayedMax];
        const int n = SortedMet(met);
        if (param < 0 || param >= n) return true;
        MpFriend f;
        memset(&f, 0, sizeof f);
        snprintf(f.name, sizeof f.name, "%s", met[param].name);
        snprintf(f.code, sizeof f.code, "%s", met[param].code);
        sReq = f;
        blockRequest();   // (the same as blocking a request: the service, the list, the window)
        return true;
    }
    default: return false;
    }
    return true;
}

bool Sequence::keyboardFriends(int purpose, const char* text) {
    switch (purpose) {
    case K_UNBLOCK:
        unblockAt(-1, text); return true;
    case K_FRIEND_ADD:
        {
        snprintf(mFriendCodeText, sizeof mFriendCodeText, "%s", text);   // M1: kept for the next keyboard until an add works
        char norm[10];
        if (!NormalizeCode(text, norm)) {   // not 8 letters and digits: said plainly, nothing sent
            if (mIo) mIo->sound(Se::Buzzer);
            say(P_INFO, MP_T_ERR_NO_PLAYER);
            return true;
        }
        if (mp_ui_friend_add(norm)) {
            snprintf(mFriendAdding, sizeof mFriendAdding, "%s", norm);
            startBusy(B_FRIEND_ADD);   // fix3: the answer decides the window
        } else {
            showServiceError(MP_ERR_INTERNAL, MP_T_DLG_FRIEND_FAILED);   // fix3: (refused at once: your own code, ...)
        }
        return true;
    }
    default: return false;
    }
    return true;
}

bool Sequence::tickFriends(Scr scr, const Input& in, bool* stop) {
    switch (scr) {
    case S_FRIENDS:
    case S_REQUESTS:
    case S_BLOCKED:
    case S_PLAYED:
    case S_BOARD:
        {
        Resync(mFrame);
        const Gate before = gGate;
        const bool ready = SignIn();
        if (!ready) break;
        if (before != GATE_READY) {   // signed in just now (or again): ask for what the screen shows
            if (scr == S_BOARD) requestBoard();
            else if (scr != S_BLOCKED && friendsEntryAllowed()) refreshFriends();
        }
        if (scr == S_BOARD) {   // wire: the times come back from the service (this poll was lost in the friends rework: the board said "Loading..." for ever)
            if (mBoardDirty && mFrame - mBoardDirtyAt > 20) requestBoard();
            static MpBoardRow rows[kMaxBoard];
            const int n = mp_ui_board_get(rows, kMaxBoard);
            if (n >= 0) {
                const int c = n > kMaxBoard ? kMaxBoard : n;
                for (int i = 0; i < c; i++) {
                    sBoard[i] = rows[i];
                    sBoard[i].name[MP_NAME_BYTES] = 0;
                }
                sBoardCount = c;
            } else {
                sBoardCount = n < -1 ? -2 : -1;
            }
            return true;   // (not break: the line below resets the count for a screen that is not signed in, and it reset this one every frame)
        }
        const bool pacedScr = scr == S_FRIENDS || scr == S_REQUESTS;
        if (mFrame < gClockLast) {   // a new Sequence (a restart of the system, a test): its frames start from 0, so does the history
            gFriendsPacer = MpListRefresh::Pacer();
            gClockFrames = 0;
            gClockLast = 0;
            gAutoAsk = gKeepRows = gWasLoading = false;
        }
        gClockFrames += mFrame - gClockLast;
        gClockLast = mFrame;
        gPacerNowMs = gClockFrames * 1000 / kFps;
        if (pacedScr) {
            if (mFrame - gPacerFrame > 3) { gFriendsPacer.enter(gPacerNowMs); gAutoAsk = gKeepRows = false; gWasLoading = false; }   // (just entered)
            gPacerFrame = mFrame;
            if (in.up || in.down || in.left || in.right || in.ok || in.back || in.tri || in.sq || in.start) gFriendsPacer.press(gPacerNowMs);
        } else {
            gFriendsPacer.leave();
        }
        if (scr != S_BLOCKED) pollFriends();
        if (pacedScr) {
            const bool loading = sFriendCount == -1;
            if (gWasLoading && !loading && !gAutoAsk) gFriendsPacer.answered(gPacerNowMs, sFriendCount >= 0);   // the screen's own request came back
            gWasLoading = loading;
            if (in.start && !loading && !mDlg && !mKb) {   // by hand: Start = Refresh (everything starts over, also after an automatic stop)
                if (gFriendsPacer.manual(gPacerNowMs)) {   // (not twice within 10 s, not above the hourly ceiling: then only a sound)
                    if (mIo) mIo->sound(Se::Decide);
                    gAutoAsk = false;
                    refreshFriends();
                    gWasLoading = true;
                    mp_log("ui: friends list refresh (by hand)");
                } else if (mIo) {
                    mIo->sound(Se::Buzzer);
                }
            } else if (gFriendsPacer.due(gPacerNowMs, loading || gAutoAsk, mDlg != 0 || mKb != 0 || mBusy != 0)) {
                gAutoAsk = true;
                mp_ui_friends_request();
                mp_log("ui: friends list refresh (by itself)");
            }
        }
        // the page and the cursor stay on rows that exist
        Ent ents[kEntMax];
        const int total = Entries(scr, ents, kEntMax);
        Frame& f = top();
        if (total > 0 && PageStart(scr, f.param) >= total) f.param = static_cast<uint8_t>(PageCount(scr, total) - 1);
        int it[kItems];
        const int n = items(scr, f.param, it);
        if (f.cursor >= n) f.cursor = static_cast<int8_t>(n - 1);
        if (f.cursor < 0 && n > 0) f.cursor = 0;
        f.scroll = 0;
        // the last request was answered / the last player was added or blocked: back one screen
        if ((scr == S_REQUESTS && sFriendCount >= 0 && gIncomingN == 0) || (scr == S_PLAYED && total == 0)) {
            pop();
            *stop = true;
        }
        return true;
    }
    default: return false;
    }
    // (not signed in: nothing to poll; the screen says why)
    if (scr == S_BOARD) { sBoardCount = -1; }
    (void)in, (void)stop;
    return true;
}

bool Sequence::labelFriends(int item, MpPanel::Row& r, const Frame& f) const {
    using namespace MpPanel;
    switch (item) {
    case I_REQUESTS:
        Num(r.label, kLabel, MP_T_REQUESTS, gIncomingN); r.marks |= MARK_GOLD; return true;
    case I_PLAYED:
        Num(r.label, kLabel, MP_T_PLAYED_WITH, playedCount()); return true;   // flow
    case I_BLOCKED:
        Text(r.label, kLabel, MP_T_BLOCKED_PLAYERS); return true;
    case I_DELACCT:
        Text(r.label, kLabel, MP_T_DELETE_ACCOUNT); return true;
    case I_REQ_ADD:
        Text(r.label, kLabel, MP_T_ADD); return true;
    case I_REQ_NOTNOW:
        Text(r.label, kLabel, MP_T_NOT_NOW); return true;
    case I_REQ_BLOCK:
        Text(r.label, kLabel, MP_T_BLOCK); return true;
    case I_FR_JOIN:
        Text(r.label, kLabel, MP_T_JOIN); return true;
    case I_FR_REMOVE:
        Text(r.label, kLabel, MP_T_REMOVE_FRIEND); return true;
    case I_GALAXY:
        Text(r.label, kLabel, MP_T_GALAXY);
        if (mIo) mIo->galaxyName(mBoardGalaxy, r.value, kValue);
        r.marks |= MARK_ARROWS | MARK_HEADER;
        return true;
    case I_STARPICK:
        Num(r.label, kLabel, MP_T_STAR_N, mBoardStar);
        if (mIo) mIo->scenarioName(mBoardGalaxy, mBoardStar, r.value, kValue);
        r.marks |= MARK_ARROWS | MARK_HEADER;
        return true;
    default: break;
    }
    if (item >= I_LIST0 && item < I_FROOM0 && (f.scr == S_FRIENDS || f.scr == S_REQUESTS || f.scr == S_BLOCKED || f.scr == S_PLAYED)) {
        Ent ents[kEntMax];
        const int total = Entries(f.scr, ents, kEntMax);
        const int e = PageStart(f.scr, f.param) + (item - I_LIST0);
        if (e >= total) return true;   // (a stale row: nothing)
        const Ent ent = ents[e];
        switch (ent.kind) {
        case E_FRIEND: {
            const MpFriend& fr = gFriends[ent.idx];
            bool dup = false;
            for (int j = 0; j < gFriendN && !dup; j++) dup = j != ent.idx && SameName(gFriends[j].name, fr.name);
            NameWithTail(r.label, kLabel, fr.name, fr.code, dup);
            r.icon = fr.joinable ? ICON_DOT_ROOM : (fr.online ? ICON_DOT_ONLINE : ICON_DOT_OFFLINE);
            Text(r.value, kValue, fr.joinable ? MP_T_IN_ROOM : (fr.online ? MP_T_ONLINE : MP_T_OFFLINE));
            break;
        }
        case E_REQUEST: case E_BLOCKED: {
            const MpFriend* arr = ent.kind == E_REQUEST ? gIncoming : gBlocked;
            const int cnt = ent.kind == E_REQUEST ? gIncomingN : gBlockedN;
            const MpFriend& fr = arr[ent.idx];
            bool dup = false;
            for (int j = 0; j < cnt && !dup; j++) dup = j != ent.idx && SameName(arr[j].name, fr.name);
            NameWithTail(r.label, kLabel, fr.name, fr.code, dup);
            txt::utf8To16(r.value, kValue, fr.code);
            break;
        }
        case E_MET: {
            Met met[kPlayedMax];
            const int cnt = SortedMet(met);
            const Met& m = met[ent.idx];
            bool dup = false;
            for (int j = 0; j < cnt && !dup; j++) dup = j != ent.idx && SameName(met[j].name, m.name);
            NameWithTail(r.label, kLabel, m.name, m.code, dup);
            if (IsFriend(m.code)) Text(r.value, kValue, MP_T_FRIEND_MARK);   // already a friend: marked, nothing to add
            break;
        }
        default: break;
        }
        return true;
    }
    return false;
}

bool Sequence::modelFriends(MpPanel::Model* m, const Frame& f, int cursorItem, int count, const int* it) const {
    using namespace MpPanel;
    auto button = [m](int pic, MpText label) { ModelButton(m, pic, label); };
    auto hint = [m](MpText t) { Text(m->hint, 80, t); };
    (void)count, (void)it;
    const bool needsService = Gated(f.scr);
    // not signed in (yet): only what is going on, and Back
    auto gateLine = [&]() {
        if (gGate == GATE_WORKING) {
            Text(m->status, 80, MP_T_CONNECTING);
            m->spinner = (mFrame % 120) / 120.0f;
        } else {
            switch (gWhy) {
            case WHY_NO_WIFI: Text(m->status, 80, MP_T_CT_L_WIFI); break;
            case WHY_CLOCK: Text(m->status, 80, MP_T_CT_L_CLOCK); break;
            case WHY_UPDATE: Text(m->status, 80, MP_T_CT_L_UPDATE); break;
            case WHY_OFFLINE: Text(m->status, 80, MP_T_SVC_OFFLINE); break;
            default: Text(m->status, 80, MP_T_SVC_BUSY); break;
            }
        }
        button(txt::PIC_CIRCLE, MP_T_BACK);
    };
    // a paged list: all the rows of this page, the page column for the whole list
    auto pages = [&](int total) {
        const int pc = PageCount(f.scr, total);
        m->moreAbove = m->moreBelow = false;
        m->pages = pc > 1 ? pc : 0;
        m->page = PageOf(f.scr, PageStart(f.scr, f.param));
    };
    Ent ents[kEntMax];
    const int total = Paged(f.scr) ? Entries(f.scr, ents, kEntMax) : 0;
    switch (f.scr) {
    case S_FRIENDS:
        {
        Text(m->title, 48, MP_T_FRIENDS);
        if (needsService && gGate != GATE_READY) { gateLine(); break; }
        pages(total);
        if (f.param == 0) {
            // my code: large, in two halves, easy to read aloud
            MpProfile p;
            memset(&p, 0, sizeof p);
            mp_ui_profile(&p);
            p.friendCode[9] = 0;
            GroupCode(p.friendCode, m->big, 24);
            if (m->big[0]) Text(m->bigLabel, 24, MP_T_MY_FRIEND_CODE);
        }
        if (sFriendCount == -1) {
            Text(m->status, 80, MP_T_LOADING);
            m->spinner = (mFrame % 120) / 120.0f;
        } else if (sFriendCount == -2) {
            Text(m->status, 80, MP_T_LOAD_FAILED);
            button(txt::PIC_TRIANGLE, MP_T_RETRY);
        } else if (sFriendCount == 0) {
            Text(m->status, 80, MP_T_NO_FRIENDS);
        }
        const bool onFriend = cursorItem >= I_LIST0;
        if (cursorItem == I_REQUESTS) hint(MP_T_DESC_REQUESTS);
        if (cursorItem == I_PLAYED) hint(MP_T_DESC_PLAYED);
        if (cursorItem == I_DELACCT) hint(MP_T_DESC_DELETE_ACCOUNT);
        if (onFriend) Text(m->hint, 80, MP_T_DESC_FRIEND_PAGE);
        if (cursorItem != I_NONE) button(txt::PIC_CROSS, MP_T_SELECT);
        if (sFriendCount != -2) button(txt::PIC_TRIANGLE, MP_T_ADD);
        if (sFriendCount >= 0) button(txt::PIC_START, MP_T_REFRESH);   // uxwait
        if (onFriend) button(txt::PIC_SQUARE, MP_T_REMOVE);
        button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    }
    case S_PLAYED:
        {
        // flow: Friends > Played With: the players with a friend code, newest first; Cross adds a friend, Square blocks; a friend is marked
        Text(m->title, 48, MP_T_TITLE_PLAYED);
        if (gGate != GATE_READY) { gateLine(); break; }
        pages(total);
        hint(MP_T_DESC_PLAYED);
        {   // the players whose code is not known: after the others, dim, not choosable
            int act = 0;
            Met met[kPlayedMax];
            SortedMet(met, &act);
            const int from = PageStart(f.scr, f.param);
            for (int e = from + count; e < total && e < from + kPage && m->rowCount < kMaxRows; e++) {
                Row& r = m->rows[m->rowCount++];
                r.ping = -1;
                labelFriends(I_LIST0 + (e - from), r, f);
                r.marks |= MARK_STATIC | MARK_DISABLED;
            }
        }
        bool friendRow = false;
        if (cursorItem >= I_LIST0) {
            const int e = PageStart(f.scr, f.param) + (cursorItem - I_LIST0);
            Met met[kPlayedMax];
            SortedMet(met);
            if (e < total) friendRow = IsFriend(met[ents[e].idx].code);
        }
        if (cursorItem != I_NONE) button(friendRow ? txt::PIC_CROSS : txt::PIC_CROSS, friendRow ? MP_T_SELECT : MP_T_ADD_FRIEND);
        if (cursorItem != I_NONE && !friendRow) button(txt::PIC_SQUARE, MP_T_BLOCK);
        button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    }
    case S_REQUESTS:
        // friendsui
        Text(m->title, 48, MP_T_TITLE_REQUESTS);
        if (gGate != GATE_READY) { gateLine(); break; }
        pages(total);
        if (sFriendCount == -1) {
            Text(m->status, 80, MP_T_LOADING);
            m->spinner = (mFrame % 120) / 120.0f;
        } else if (sFriendCount == -2) {
            Text(m->status, 80, MP_T_LOAD_FAILED);
            button(txt::PIC_TRIANGLE, MP_T_RETRY);
        } else {
            hint(MP_T_DESC_REQUESTS);
            button(txt::PIC_CROSS, MP_T_SELECT);
        }
        button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    case S_REQUEST:
        // friendsui: the game's big buttons, the player's name under the title; a friend's page (param 1) shows their code
        m->kind = KIND_BUTTONS;
        if (f.param == 1) {
            txt::utf8To16(m->title, 48, sReq.name);
            char16_t g[12];
            GroupCode(sReq.code, g, 12);
            char g8[12];
            for (int i = 0; i < 11; i++) g8[i] = static_cast<char>(g[i] < 128 ? g[i] : '?'), g8[11] = 0;
            Str(m->subtitle, 64, MP_T_FRIEND_CODE_LINE, g8);
            if (cursorItem == I_FR_JOIN) Text(m->hint, 80, MP_T_DESC_FRIEND_JOIN);
            else if (cursorItem == I_FR_REMOVE) Text(m->hint, 80, MP_T_DESC_FRIEND_REMOVE);
            else hint(MP_T_DESC_BLOCK);
        } else {
            Text(m->title, 48, MP_T_TITLE_REQUEST);
            char b[40];
            snprintf(b, sizeof b, "%s  %s", sReq.name, sReq.code);
            txt::utf8To16(m->subtitle, 64, b);
            hint(cursorItem == I_REQ_ADD ? MP_T_DESC_REQ_ADD : (cursorItem == I_REQ_NOTNOW ? MP_T_DESC_NOT_NOW : MP_T_DESC_BLOCK));
        }
        button(txt::PIC_CROSS, MP_T_SELECT), button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    case S_BLOCKED:
        // friendsui
        Text(m->title, 48, MP_T_BLOCKED_PLAYERS);
        if (gGate != GATE_READY) { gateLine(); break; }
        pages(total);
        if (gBlockedN == 0) {
            Text(m->status, 80, MP_T_NO_BLOCKED);
        } else {
            hint(MP_T_DESC_BLOCKED);
            button(txt::PIC_CROSS, MP_T_UNBLOCK);
        }
        button(txt::PIC_TRIANGLE, MP_T_BY_CODE);
        button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    case S_BOARD:
        {
        Text(m->title, 48, MP_T_LEADERBOARDS);
        if (gGate != GATE_READY) { gateLine(); break; }
        m->tabCount = 3;
        m->tab = mBoardTab;
        Text(m->tabs[0], 20, MP_T_TAB_TOP), Text(m->tabs[1], 20, MP_T_TAB_NEAR), Text(m->tabs[2], 20, MP_T_TAB_FRIENDS);
        if (sBoardCount == -1) {
            Text(m->status, 80, MP_T_LOADING);
            m->spinner = (mFrame % 120) / 120.0f;
        } else if (sBoardCount == -2) {
            Text(m->status, 80, MP_T_LOAD_FAILED);
        } else if (sBoardCount == 0) {
            Text(m->status, 80, MP_T_NO_TIMES);
        }
        // uinative: big rows: the window shows kBoardRows times (of those fetched), the player's own line always among them
        const int timeLimit = m->rowCount + kBoardRows;
        int firstTime = 0;
        for (int i = 0; i < sBoardCount; i++)
            if (sBoard[i].you && i >= kBoardRows) firstTime = i - kBoardRows + 1;
        for (int i = firstTime; i < sBoardCount && m->rowCount < timeLimit && m->rowCount < kMaxRows; i++) {
            Row& r = m->rows[m->rowCount++];
            const MpBoardRow& b = sBoard[i];
            r.ping = -1;
            r.marks = MARK_STATIC;
            uint32_t rank = b.rank;
            char16_t d[12];
            int k = 0;
            do { d[k++] = static_cast<char16_t>(u'0' + rank % 10); rank /= 10; } while (rank && k < 10);
            int n = 0;
            while (k > 0 && n < kCol - 1) r.lead[n++] = d[--k];
            r.lead[n] = 0;
            int ln = txt::utf8To16(r.label, kLabel, b.name);
            if (b.players > 1 && ln < kLabel - 5) {
                r.label[ln++] = u' ', r.label[ln++] = u' ', r.label[ln++] = static_cast<char16_t>(u'0' + b.players % 10), r.label[ln++] = u'P';
                r.label[ln] = 0;
            }
            txt::formatTime(r.value, kValue, b.frames);
            if (b.verified) r.marks |= MARK_VERIFIED;
            if (b.you) r.marks |= MARK_YOU;
            if (b.rank == 1) r.marks |= MARK_GOLD;
        }
        hint(MP_T_TAB_HINT);
        button(txt::PIC_TRIANGLE, MP_T_TAB), button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    }
    default: return false;
    }
    // flow look: the friends' lists and pages in the file select's look (the title, and a short hint, in the bottom message)
    GameLookFromTitle(m);
    return true;
}

// Input this family takes before the shared rules (stage 0: a direction, for grids; stage 1: Square / Triangle). True: taken.
bool Sequence::pressFriends(Scr scr, const Input& in, int item, int count, int stage) {
    if (stage == 0 && Paged(scr) && gGate == GATE_READY && count > 0 && mDepth > 0) {
        // the whole list is walked here (the core's cursor knows one page): Up / Down move by one row over the pages and wrap round,
        // Left / Right turn a page
        Ent ents[kEntMax];
        const int all = Entries(scr, ents, kEntMax);
        int total = all;
        if (scr == S_PLAYED) {   // (the players without a code are shown but never chosen: they come last)
            Met met[kPlayedMax];
            SortedMet(met, &total);
        }
        Frame& f = top();
        const int cur = f.cursor < 0 ? 0 : f.cursor;
        const int at = PageStart(scr, f.param) + cur;
        int to = at;
        if (in.down) to = at + 1 >= total ? 0 : at + 1;
        else if (in.up) to = at - 1 < 0 ? total - 1 : at - 1;
        else if (in.right || in.left) {
            const int pc = PageCount(scr, all), p = (f.param + (in.right ? 1 : pc - 1)) % pc;
            to = PageStart(scr, p);
            if (to >= total) to = 0;
        }
        if (total <= 0 || to == at) return true;
        const int page = PageOf(scr, to);
        f.param = static_cast<uint8_t>(page);
        f.cursor = static_cast<int8_t>(to - PageStart(scr, page));
        f.scroll = 0;
        if (mIo) mIo->sound(Se::Cursor);
        mPulse = 1.0f;
        return true;
    }
    if (stage == 1 && in.sq && scr == S_FRIENDS) {
        if (item >= I_LIST0 && gGate == GATE_READY) {
            Ent ents[kEntMax];
            const int total = Entries(scr, ents, kEntMax);
            const int e = PageStart(scr, top().param) + (item - I_LIST0);
            if (e < total && ents[e].kind == E_FRIEND) {
                Args a;
                a.str = gFriends[ents[e].idx].name;
                ask(P_FRIEND_REMOVE, MP_T_DLG_FRIEND_REMOVE, a, Dlg::YesNo, ents[e].idx);
            }
        }
        return true;
    }
    if (stage == 1 && in.sq && scr == S_PLAYED) {
        if (item >= I_LIST0 && gGate == GATE_READY) {
            Ent ents[kEntMax];
            const int total = Entries(scr, ents, kEntMax);
            const int e = PageStart(scr, top().param) + (item - I_LIST0);
            Met met[kPlayedMax];
            SortedMet(met);
            if (e < total && !IsFriend(met[ents[e].idx].code)) {   // (a friend is blocked from their own page)
                Args a;
                a.str = met[ents[e].idx].name;
                ask(P_MET_BLOCK, MP_T_DLG_REQ_BLOCK, a, Dlg::YesNo, ents[e].idx);
            }
        }
        return true;
    }
    if (stage == 1 && in.tri) {
        if (Gated(scr) && gGate != GATE_READY) return true;   // (signing in: nothing to add to yet)
        if (scr == S_FRIENDS) {
            if (sFriendCount == -2) {   // Retry: through the pacer too (10 s apart, 30 an hour); refused: a buzzer, the error line stays
                if (gFriendsPacer.manual(gPacerNowMs)) {
                    gAutoAsk = gKeepRows = false;
                    mp_ui_friends_request();
                    sFriendCount = -1;
                    gWasLoading = true;
                    if (mIo) mIo->sound(Se::Decide);
                } else if (mIo) {
                    mIo->sound(Se::Buzzer);
                }
            } else {
                openKeyboard(K_FRIEND_ADD, MP_T_KB_FRIEND, mFriendCodeText, 12, true);   // (12: "ABCD - EFGH" fits too)
            }
            return true;
        }
        if (scr == S_REQUESTS) {
            if (sFriendCount == -2) {   // (friendsui: Retry; through the pacer: 10 s apart, 30 an hour, else a buzzer)
                if (gFriendsPacer.manual(gPacerNowMs)) {
                    gAutoAsk = gKeepRows = false;
                    refreshFriends();
                    gWasLoading = true;
                    if (mIo) mIo->sound(Se::Decide);
                } else if (mIo) {
                    mIo->sound(Se::Buzzer);
                }
            }
            return true;
        }
        if (scr == S_BLOCKED) {
            if (mIo) mIo->sound(Se::Decide);
            openKeyboard(K_UNBLOCK, MP_T_KB_UNBLOCK, "", 12, true);
            return true;
        }
        if (scr == S_BOARD) {
            mBoardTab = (mBoardTab + 1) % 3;
            if (mIo) mIo->sound(Se::Tab);
            requestBoard();
            return true;
        }
    }
    (void)scr, (void)in, (void)item, (void)count, (void)stage;
    return false;
}

// Back (Circle) on one of this family's screens: phase 0 before anything (true: taken, nothing else happens), 1 just before the
// screen goes, 2 just after.
bool Sequence::leaveFriends(Scr scr, uint8_t param, int phase) {
    if (phase == 1 && (scr == S_BOARD || scr == S_FRIENDS || scr == S_REQUESTS || scr == S_BLOCKED || scr == S_PLAYED)) mp_log("ui: list closed");
    (void)scr, (void)param, (void)phase;
    return false;
}

}  // namespace MpMenu
