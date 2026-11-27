// Galaxy 3.0 multiplayer: the menus, the session family: the session menu, Warp, Messages, Players, a player's choices (flow split of MpMenuSequence.cpp, 11 Nov).
// Owner of this family's screens: see MpMenuSequence.cpp "the file map". Pure code, as MpMenuSequence.cpp.
#include "MpMenuInternal.hpp"
#include "port/settings_native.h"

extern "C" bool mp_update_take_notice(void) __attribute__((weak));   // release3: port/mp_update.h (platform side: not in the game's include path)

// session: the friend code another console announced over the game link (port/mp_core_game.h, branch core2). Weak: null before core2 is merged.
extern "C" bool mp_core_slot_friend_code(int slot, char* out, size_t cap) __attribute__((weak));

// mppause: platform/src/port/perf.cpp: the pause menu's zoom row can show here (the handhelds). Weak: not there in the host tests (the old one-line note then).
extern "C" int port_zoom_menu_capable(void) __attribute__((weak));

namespace MpMenu {

bool Sequence::zoomRowShown() const {
    return mDepth > 0 && mStack[mDepth - 1].scr == S_SESSION && !mDlg && !mKb && port_zoom_menu_capable && port_zoom_menu_capable() != 0;
}

void Sequence::openSession() {
    // fix3: also while the core reconnects (mp_active() is false then): Start opens this menu (Leave is there), never the pause menu
    if (!(mp_active() || mReconnecting) || mDepth > 0 || mDlg || mKb) return;
    // fix2: the press (or the 12-frame hold) of the Start that just closed the menu reaches the scene's pause check after us
    if (mClosedAt != 0 && mFrame - mClosedAt < kReopenGuard) {
        mp_log("ui: session menu reopen ignored");
        return;
    }
    mFromFileSelect = false;
    mFade = 0;
    push(S_SESSION);
    mp_log("ui: session menu opened");
    if (mIo) mIo->sound(Se::Open);
    if (sessionLaggy()) {   // conntest: a weak link in this session: the Wi-Fi power-save tip (once)
        mTipWanted = true;
        showTip();
    }
}

// conntest: someone in the room has no packet for a while, or answers slowly (the player list's one-bar steps)
bool Sequence::sessionLaggy() const {
    for (int k = 0; k < MP_MAX_PLAYERS; k++) {
        if (k == mp_local_slot() || !mp_slot_present(k)) continue;
        if (mp_slot_quiet(k) || mp_slot_ping_ms(k) >= kLaggyMs) return true;
    }
    return false;
}

void Sequence::openPlayer(int slot) {
    int it[kItems];
    if (slot < 0 || slot >= MP_MAX_PLAYERS || !mp_slot_present(slot) || items(S_PLAYER, static_cast<uint8_t>(slot), it) == 0) {
        if (mIo) mIo->sound(Se::Buzzer);
        return;
    }
    if (mIo) mIo->sound(Se::Decide);
    push(S_PLAYER, static_cast<uint8_t>(slot));
}

void Sequence::choosePlayer(int item) {
    const int slot = top().param;
    Args a;
    a.str = mp_slot_name(slot);
    switch (item) {
    case I_P_MUTE:   // chat: their typed messages and balloons, on this console only
        MpChat::setMuted(slot, !MpChat::muted(slot));
        if (mIo) mIo->sound(MpChat::muted(slot) ? Se::Decide : Se::Cursor);
        mPulse = 1.0f;
        return;
    case I_P_KICK:
        if (mIo) mIo->sound(Se::Decide);
        ask(P_KICK, MP_T_DLG_KICK, a, Dlg::YesNo, slot);
        return;
    case I_P_ADD: {
        if (needAccount(item)) return;   // (the online account first; the row is chosen again after)
        const char* code = playerCode(slot);
        if (!code || !mp_ui_friend_add(code)) {
            if (mIo) mIo->sound(Se::Buzzer);
            say(P_INFO, MP_T_DLG_FRIEND_FAILED);
            return;
        }
        if (mIo) mIo->sound(Se::Decide);
        snprintf(mFriendAdding, sizeof mFriendAdding, "%s", mp_slot_name(slot));
        pop();
        startBusy(B_FRIEND_ADD);
        return;
    }
    case I_P_BLOCK:
        if (needAccount(item)) return;
        if (mIo) mIo->sound(Se::Decide);
        ask(P_PLAYER_BLOCK, MP_T_DLG_REQ_BLOCK, a, Dlg::YesNo, slot);
        return;
    default: return;
    }
}

// The friend code another console announced over the game link (any kind of game; false: not known yet, or nothing to add).
const char* Sequence::playerCode(int slot) const {
    static char code[MP_MAX_PLAYERS][12];
    if (slot < 0 || slot >= MP_MAX_PLAYERS || slot == mp_local_slot()) return nullptr;
    code[slot][0] = 0;
    if (RoomPlayerCode(slot, code[slot], sizeof code[slot])) return code[slot];   // wire: verified or announced (any kind of game)
    if (!mp_core_slot_friend_code) return nullptr;
    return mp_core_slot_friend_code(slot, code[slot], sizeof code[slot]) && code[slot][0] ? code[slot] : nullptr;
}

bool Sequence::itemsSession(Scr s, uint8_t param, int* out, int* np) const {
    int& n = *np;
    auto add = [&](int i) { if (n < kItems) out[n++] = i; };
    switch (s) {
    case S_PLAYERS:
        // the others (one's own row would open nothing while playing)
        for (int k = 0; k < MP_MAX_PLAYERS; k++)
            if (mp_slot_present(k) && k != mp_local_slot()) add(I_LIST0 + k);
        break;
    case S_PLAYER:
        {   // one player's page: Add Friend, Hide / Show Messages, Block, Remove from Game
        const int k = param;
        if (k == mp_local_slot()) {
            if (UiPhase() == MP_UI_LOBBY) add(I_P_CHAR);
            break;
        }
        if (!mp_slot_present(k)) break;
        // Add Friend and Block need the code and the online service; without either the rows are not there
        const bool friends = NetInfo().service && playerCode(k) != nullptr;
        if (friends) add(I_P_ADD);
        add(I_P_MUTE);
        if (friends) add(I_P_BLOCK);
        if (mOriginalHost && mp_is_host()) add(I_P_KICK);   // only the player who opened the game, while they still lead it
        break;
    }
    case S_WARP:
        for (int k = 0; k < MP_MAX_PLAYERS; k++)
            if (mp_slot_present(k) && k != mp_local_slot()) add(I_LIST0 + k);
        break;
    case S_SESSION:
        // the pause screen's few big buttons (Back: Circle or Start; Messages: Square; Players: Triangle, both on the guide band)
        if (mp_mode() == MP_MODE_TOGETHER && othersPresent() > 0) add(I_WARP);
        if (mp_mode() == MP_MODE_TOGETHER && mp_is_host()) add(I_DOME);
        if (port::settings::NativeHostGet() && port::settings::NativeHostGet()->available) add(I_SESSION_SETTINGS);   // settingsnative: the personal set, from the session's menu
        add(I_LEAVE);
        break;
    case S_MESSAGES:
        // chat: the phrases in the grid's reading order, then "Write a Message" where the room allows it
        for (int i = 0; i < MpChatLogic::kGridCount; i++) add(I_PHRASE0 + MpChatLogic::kGridOrder[i]);
        if (MpChat::typedAllowed()) add(I_WRITE);
        break;
    default: return false;
    }
    (void)param, (void)add;
    return true;
}

bool Sequence::chooseSession(int item) {
    Io* io = mIo;
    auto decide = [io] { if (io) io->sound(Se::Decide); };
    (void)decide;
    // the one place a warp is asked for (the Warp list, or the only other player)
    auto warpTo = [this, io](int k) {
        if (io) io->sound(Se::Decide);
        mWarp = k;
        Args a;
        a.str = mp_slot_name(k);
        char16_t t[96];
        txt::format(t, 96, MP_T_WARPING, a);
        MpOverlay::toastText(t, LookIcon(k));
        mp_log("ui: warp request slot=%d", k);
        close();
    };
    switch (item) {
    case I_RESUME:
        if (io) io->sound(Se::Close);
        close();
        return true;
    case I_WARP:
        {   // one other player: straight to them (a list of one is a question with one answer)
        int only = -1, n = 0;
        for (int k = 0; k < MP_MAX_PLAYERS; k++)
            if (k != mp_local_slot() && mp_slot_present(k)) only = k, ++n;
        if (n == 1) { warpTo(only); return true; }
        if (n == 0) { if (io) io->sound(Se::Buzzer); return true; }
        decide(); push(S_WARP); return true;
    }
    case I_STAMPS:
        decide(); push(S_MESSAGES); return true;
    case I_PLAYERS:
        {   // one other player: their page at once; nobody else: nothing to list
        int only = -1, n = 0;
        for (int k = 0; k < MP_MAX_PLAYERS; k++)
            if (k != mp_local_slot() && mp_slot_present(k)) only = k, ++n;
        if (n == 1) { openPlayer(only); return true; }
        if (n == 0) { if (io) io->sound(Se::Buzzer); return true; }
        decide(); push(S_PLAYERS); return true;
    }
    case I_DOME:
        decide(); ask(P_DOME, MP_T_DLG_BACK_DOME, Args(), Dlg::YesNo); return true;
    case I_SESSION_SETTINGS:
        decide(); openSettings(SO_SESSION); return true;
    case I_LEAVE:
        decide();
        ask(P_LEAVE_SESSION, mp_is_host() ? MP_T_DLG_LEAVE_HOST : MP_T_DLG_LEAVE_GUEST, Args(), Dlg::YesNo);
        return true;
    case I_P_CHAR:
        decide(); pop(); openChars(1); return true;   // flow: (the player's own row in the lobby)
    case I_P_ADD:
    case I_P_BLOCK:
    case I_P_MUTE:
    case I_P_KICK:
        choosePlayer(item); return true;
    default: break;
    }
    if (item >= I_PHRASE0 && item < I_PHRASE0 + MP_CHAT_PHRASES) {
        // chat: one press sends it, the menu closes (the balloon and the toast say it was sent)
        if (!MpChat::sendPhrase(item - I_PHRASE0)) {
            if (io) io->sound(Se::Buzzer);
            return true;
        }
        decide();
        close();
        return true;
    }
    if (item == I_WRITE) {
        decide();
        openKeyboard(K_CHAT, MP_T_KB_CHAT, "", MP_CHAT_TEXT_CHARS, false);
        return true;
    }
    if (item >= I_LIST0 && item < I_FROOM0) {
        const int k = item - I_LIST0;
        switch (mDepth > 0 ? top().scr : S_NONE) {
        case S_WARP:
            warpTo(k);
            return true;
        case S_PLAYERS:
            // flow: a player's choices (Change Character on one's own row in the lobby)
            openPlayer(k);
            return true;
        default: break;
        }
    }
    return false;
}

bool Sequence::dialogSession(int purpose, bool yes, int param) {
    switch (purpose) {
    case P_PLAYER_BLOCK:
        {   // flow: Block on a player of the room: no more requests or presence (service), and muted here
        if (!yes) return true;
        const char* code = playerCode(param);
        if (!code || !mp_ui_friend_block(code)) {
            if (mIo) mIo->sound(Se::Buzzer);
            say(P_INFO, MP_T_DLG_REQ_FAILED);
            return true;
        }
        MpChat::setMuted(param, true);
        Args a;
        a.str = mp_slot_name(param);
        mp_log("ui: player blocked slot=%d", param);
        if (mDepth > 0 && top().scr == S_PLAYER) pop();
        say(P_INFO, MP_T_DLG_REQ_BLOCKED, a);
        return true;
    }
    case P_LEAVE_SESSION:
        if (yes) {
            mLeavingByUs = true;
            mp_ui_leave();
            mp_log("ui: left the session");
            close();
        }
        return true;
    case P_DOME:
        if (yes) {
            mDome = true;
            mp_log("ui: back to the observatory requested");
            close();
        }
        return true;
    default: return false;
    }
    return true;
}

bool Sequence::keyboardSession(int purpose, const char* text) {
    switch (purpose) {
    case K_CHAT:
        // chat: a typed message goes (cleaned); nothing usable or too soon: the buzzer, the page stays
        if (MpChat::sendText(text)) {
            close();
        } else if (mIo) {
            mIo->sound(Se::Buzzer);
        }
        return true;
    default: return false;
    }
    return true;
}

bool Sequence::tickSession(Scr scr, const Input& in, bool* stop) {
    switch (scr) {
    case S_PLAYERS:
    case S_WARP:
    case S_PLAYER:
        {
        // keep the cursor on a present player (flow: a player who left closes their choices)
        if (scr == S_PLAYER && !mp_slot_present(top().param)) { pop(); { *stop = true; return true; } }
        const int c = itemCount();
        if (top().cursor >= c) top().cursor = static_cast<int8_t>(c - 1);
        if (top().cursor < 0 && c > 0) top().cursor = 0;
        if ((scr == S_WARP || scr == S_PLAYERS) && c == 0) { pop(); { *stop = true; return true; } }
        break;
    }
    default: return false;
    }
    (void)in;
    return true;
}

bool Sequence::labelSession(int item, MpPanel::Row& r, const Frame& f) const {
    using namespace MpPanel;
    (void)f;
    switch (item) {
    case I_P_CHAR:
        Text(r.label, kLabel, MP_T_CHANGE_CHAR_LONG); return true;
    case I_P_ADD:
        Text(r.label, kLabel, MP_T_ADD_FRIEND); return true;
    case I_P_BLOCK:
        Text(r.label, kLabel, MP_T_BLOCK); return true;
    case I_P_MUTE:
        Text(r.label, kLabel, MpChat::muted(f.param) ? MP_T_UNMUTE : MP_T_MUTE); return true;
    case I_P_KICK:
        Text(r.label, kLabel, MP_T_REMOVE_ROOM); return true;
    case I_RESUME:
        Text(r.label, kLabel, MP_T_RESUME); return true;
    case I_WARP:
        Text(r.label, kLabel, MP_T_WARP); return true;
    case I_STAMPS:
        Text(r.label, kLabel, MP_T_STAMPS); return true;
    case I_PLAYERS:
        Text(r.label, kLabel, MP_T_PLAYER_LIST); return true;
    case I_DOME:
        Text(r.label, kLabel, MP_T_BACK_DOME); return true;
    case I_SESSION_SETTINGS:
        Text(r.label, kLabel, MP_T_SET_ENTRY); return true;
    case I_LEAVE:
        Text(r.label, kLabel, MP_T_LEAVE_SESSION); return true;
    default: break;
    }
    if ((f.scr == S_PLAYERS || f.scr == S_WARP) && item >= I_LIST0 && item < I_LIST0 + MP_MAX_PLAYERS) {
        // the players of the game, as the game's big buttons: their name; no delay numbers or bars (the game has none), only that their messages are hidden
        const int k = item - I_LIST0;
        txt::utf8To16(r.label, kLabel, mp_slot_name(k));
        r.icon = LookIcon(k);
        r.ping = -1;
        if (MpChat::muted(k)) Text(r.value, kValue, MP_T_MUTED);
        return true;
    }
    if (item >= I_PHRASE0 && item < I_PHRASE0 + MP_CHAT_PHRASES) {
        Text(r.label, kLabel, static_cast<MpText>(MP_T_PHRASE_0 + (item - I_PHRASE0)));   // chat
        return true;
    }
    if (item == I_WRITE) {
        Text(r.label, kLabel, MP_T_WRITE);   // chat
        return true;
    }
    return false;
}

bool Sequence::modelSession(MpPanel::Model* m, const Frame& f, int cursorItem, int count, const int* it) const {
    using namespace MpPanel;
    auto button = [m](int pic, MpText label) { ModelButton(m, pic, label); };
    auto hint = [m](MpText t) { Text(m->hint, 80, t); };
    auto label = [this, &f](int item, Row& r) { rowLabel(item, r, f); };
    (void)button, (void)hint, (void)label, (void)cursorItem, (void)count, (void)it;
    switch (f.scr) {
    case S_PLAYER:
        {   // one player's page: their head and name over the choices (the name is the title where the look is off)
        const int k = f.param;
        m->kind = KIND_BUTTONS;
        txt::utf8To16(m->title, 48, mp_slot_name(k));
        GameLook(m);
        Heads(m, 1, [&](int, MpPanel::Row& r) {
            txt::utf8To16(r.label, kLabel, mp_slot_name(k));
            r.icon = LookIcon(k);
            if (k == mp_host_slot()) r.marks |= MARK_CROWN;
        });
        button(txt::PIC_CROSS, MP_T_SELECT), button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    }
    case S_SESSION:
        {   // the game's pause screen: no title band, a few big buttons; Messages and Players on the guide band
        m->kind = KIND_BUTTONS;
        if (port_zoom_menu_capable && port_zoom_menu_capable()) {
            GameLook(m);   // mppause: the zoom row and the one-line "keeps running" note (settings_panel.cpp) take the message window's place
            m->zoomRow = true;
        } else {
            GameLook(m, MP_T_SESSION_RUNS);
        }
        if (!mReconnecting) button(txt::PIC_SQUARE, MP_T_STAMPS);
        if (!mReconnecting && othersPresent() > 0) button(txt::PIC_TRIANGLE, MP_T_PLAYER_LIST);
        button(txt::PIC_CIRCLE, MP_T_RESUME);
        break;
    }
    case S_WARP:
        // the other players as big buttons (at most three); the names say it all
        m->kind = KIND_BUTTONS;
        Text(m->title, 48, MP_T_TITLE_WARP);
        GameLook(m, MP_T_GO_TO_Q);
        button(txt::PIC_CROSS, MP_T_SELECT), button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    case S_MESSAGES:
        {
        // chat: the phrase grid (the rows made above become its tiles)
        m->kind = KIND_GRID;
        Text(m->title, 48, MP_T_STAMPS);
        GameLook(m, MP_T_DESC_MESSAGES);   // lobby: the file select's look (no band: the description is the game's bottom message, two lines)
        m->cellCols = MpChatLogic::kGridCols;
        m->cellCount = 0;
        for (int i = 0; i < count && i < kMaxCells; i++) {
            Row tmp;
            memset(&tmp, 0, sizeof tmp);
            label(it[i], tmp);
            Copy16(m->cells[i].label, kCellText, tmp.label);
            m->cells[i].wide = it[i] == I_WRITE;
            ++m->cellCount;
        }
        m->rowCount = 0;
        m->moreAbove = m->moreBelow = false;
        m->cursor = f.cursor;
        button(txt::PIC_CROSS, cursorItem == I_WRITE ? MP_T_SELECT : MP_T_SEND), button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    }
    case S_PLAYERS:
        {   // flow: a player's row opens their choices (Add Friend, Block, Mute, Remove from Room)
        m->kind = KIND_BUTTONS;
        Text(m->title, 48, MP_T_PLAYER_LIST);
        GameLook(m, MP_T_PLAYERS_Q);
        button(txt::PIC_CROSS, MP_T_SELECT), button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    }
    default: return false;
    }
    return true;
}

// Input this family takes before the shared rules (stage 0: a direction, for grids; stage 1: Square / Triangle). True: taken.
bool Sequence::pressSession(Scr scr, const Input& in, int item, int count, int stage) {
    if (stage == 0 && scr == S_MESSAGES && (in.up || in.down || in.left || in.right)) {
        // chat: the phrase grid (3 x 4, the wide "Write a Message" under it)
        const int c = top().cursor < 0 ? 0 : top().cursor;
        const int n = MpChatLogic::gridMove(c, in.up ? 0 : (in.right ? 1 : (in.down ? 2 : 3)), count);
        if (n != c && n >= 0 && n < count) {
            top().cursor = static_cast<int8_t>(n);
            mPulse = 1.0f;
            if (mIo) mIo->sound(Se::Cursor);
        }
        return true;
    }
    if (stage == 1 && in.sq && scr == S_SESSION) {
        if (!mReconnecting) choose(I_STAMPS);   // uinative4: (fix3: nobody to greet while reconnecting)
        return true;
    }
    if (stage == 1 && in.tri && scr == S_SESSION) {
        if (!mReconnecting && othersPresent() > 0) choose(I_PLAYERS);   // (nobody else: no button, no press)
        return true;
    }
    (void)scr, (void)in, (void)item, (void)count, (void)stage;
    return false;
}

// Back (Circle) on one of this family's screens: phase 0 before anything (true: taken, nothing else happens), 1 just before the
// screen goes, 2 just after.
bool Sequence::leaveSession(Scr scr, uint8_t param, int phase) {

    (void)scr, (void)param, (void)phase;
    return false;
}

}  // namespace MpMenu
