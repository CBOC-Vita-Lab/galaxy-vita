// Galaxy 3.0 multiplayer: the menus, the Multiplayer menu and the mode screen (Join / Host / Quick Match / Leaderboards) (flow split of MpMenuSequence.cpp, 11 Nov).
// Owner of this family's screens: see MpMenuSequence.cpp "the file map". Pure code, as MpMenuSequence.cpp.
#include "MpMenuInternal.hpp"

extern "C" bool mp_update_take_notice(void) __attribute__((weak));   // release3: port/mp_update.h (platform side: not in the game's include path)

namespace MpMenu {

namespace {
bool sToldSettingsReadOnly = false;   // (once per boot)
}  // namespace

void Sequence::openMain(int file) {
    mSpFile = file >= 1 && file <= 6 ? file : 0;
    mFromFileSelect = true;
    mFromEntry = false;
    mDepth = 0;
    mFade = 0;
    push(S_MAIN);
    mp_log("ui: multiplayer menu opened file=%d", mSpFile);
    {   // core2: reading the profile loads the online account file (no network): from then on this console announces its friend code
        MpProfile p;
        memset(&p, 0, sizeof p);
        mp_ui_profile(&p);
    }
    if (mIo) mIo->sound(Se::Open);
    // a session that ended unexpectedly left its progress in a recovery file: offer it first (then the tip)
    MpSaveGame::RecoveryInfo info;
    memset(&info, 0, sizeof info);
    if (MpSaveGame::recoveryPending(&info)) {
        mRecovery = info;
        mp_log("ui: recovery offered stars=%d", info.stars);
        ask(P_RECOVERY, MP_T_DLG_RECOVERY, Args(), Dlg::YesNo);
        return;
    }
    if (MpSaveGame::settingsReadOnly() && !sToldSettingsReadOnly) {   // savefix: once per boot, before the tip
        sToldSettingsReadOnly = true;
        mp_log("ui: settings are read only");
        say(P_TIP_NEXT, MP_T_DLG_SETTINGS_RO);
        return;
    }
    if (offerReportAfterError()) return;   // beta diagnostics: a session ended with an error since the last time
    showTip();
}

// entry: the entry screen's Multiplayer: as from the file select (the menu stays up until a session launches or Back closes it, the
// recovery offer and the tip first), but no file was chosen: "Copy a File" / "Play on a File" pick one (S_SPFILE)
void Sequence::openFromEntry() {
    mp_log("ui: multiplayer menu from the entry screen");
    openMain(0);
    mFromEntry = true;
}

void Sequence::showTip() {
    if (!mTipShown && mTipWanted) {   // conntest: only after a Connection Test or a session measured a weak or laggy link
        mTipShown = true;
        say(P_TIP, MP_T_DLG_POWER_SAVE);
    }
}

// ------------------------------------------------------------------ choosing
void Sequence::requireName(int after) {
    MpProfile p;
    memset(&p, 0, sizeof p);
    mp_ui_profile(&p);
    // netx: the online account is made when a row needs the service (Host / Join with a Code / Quick Match / Join a Friend), not
    // when Internet is chosen: Play by Address works without the service, so reaching it takes only a name
    // flow (one door, service2): no registration screen: the online account is made silently when a service row needs it
    // (needAccount); a name is asked only when this console has none at all
    if (p.name[0]) {
        continueAfterName(after);
        return;
    }
    // MP3 wire: the profile (the online account) is empty until the first sign-in, and in a build without the service for good. The
    // console's saved name (mp.cfg) is never empty: the name the player typed, else the character's name (mp_default_name; never the console's user name). Asking
    // for a name while one exists made every Play Together of a build with no account type it again.
    if (mp_core_saved_name()[0]) {
        continueAfterName(after);
        return;
    }
    mAfterRegister = after;
    openKeyboard(K_NAME_LOCAL, MP_T_KB_NAME, "", 10, false);
}

bool Sequence::needAccount(int item) {
    if (mp_service_ensure_account) {   // flow (one door): silent: "Connecting..." while it works, the service's own reason if it fails
        const int r = mp_service_ensure_account();
        if (r == 1) return false;
        mPendingItem = item;
        if (r < 0) {
            mBusy = B_SIGNIN;   // 13 Nov: as the waiting sign-in that fails (showError offers the Connection Test for a connection reason)
            showServiceError(-r);
            return true;
        }
        startBusy(B_SIGNIN);
        return true;
    }
    return false;   // (a build without the service never shows a service row)
}

void Sequence::continueAfterName(int after) {
    switch (after) {
    case A_HOSTJOIN:   // flow (one door): the character select once, the very first time; then the gathering screen at once
    case A_CODE:       // (Join with a Code: the same, then the code)
        mAfterChars = after;
        if (!FlowStore().charPicked) openChars(0);
        else afterFirstChars();
        break;
    case A_FRIENDS:
        if (friendsEntryAllowed()) {
            sFriendCount = -1;
            mp_ui_friends_request();
        } else if (sFriendCount < 0) {
            sFriendCount = -2;   // (nothing to show yet and no request above the ceiling: the error line, Retry by hand)
        }
        push(S_FRIENDS, 0);
        break;
    case A_BOARD:
        sBoardCount = -1;
        push(S_BOARD);
        requestBoard();
        break;
    default: break;
    }
}

// flow (one door): where the first character select goes on to (and the way there when the character is known already)
void Sequence::afterFirstChars() {
    if (mAfterChars == A_CODE) {
        mAfterChars = 0;
        openCodeKeyboard();
        return;
    }
    mAfterChars = 0;
    openGather(false);
}

// uxfix #1: the keyboard for a friend's code. Wi-Fi is checked first (a code is an address on the same internet: with no Wi-Fi there is
// nothing to reach, and typing 19 characters first is the worst moment to hear it). The text of a code that failed comes back in it.
void Sequence::openCodeKeyboard() {
    if (!NetInfo().wifiUp) {
        mp_log("ui: a friend's code: no Wi-Fi");
        showError(MP_ERR_NO_WIFI);
        return;
    }
    if (!mCodeKeep) mCodeText[0] = 0;
    mCodeKeep = false;
    openKeyboard(K_DIRECT_CODE, MP_T_KB_DIRECT_CODE, mCodeText, 24, false);   // (forgiving: spaces, dashes, case; the core decodes)
}

// uxfix #3: a Play with Anyone that came from the gathering screen did not work: the gathering starts again (once; if it cannot, the
// player gets that reason on the Multiplayer menu as before)
void Sequence::regatherAfterQuick() {
    if (!mQuickRegather) return;
    mQuickRegather = false;
    if (mDepth == 0 || top().scr != S_MAIN || mp_active()) return;
    mp_log("ui: Play with Anyone did not work: gathering again");
    openGather(false);
}

bool Sequence::rejoinOffered() const { return mRejoinUntil != 0 && mFrame < mRejoinUntil && mp_ui_rejoin && !mp_active(); }

bool Sequence::itemsMain(Scr s, uint8_t param, int* out, int* np) const {
    int& n = *np;
    auto add = [&](int i) { if (n < kItems) out[n++] = i; };
    switch (s) {
    case S_MAIN:
        {   // flow (one door): at most four big buttons. (How to Play: Triangle, then the Connection Test is offered; Friends: Square,
            // where the online service is; Back: Circle.) Play with Anyone is on the gathering screen (no extra screen to reach it).
        const bool rejoin = rejoinOffered();
        if (rejoin) add(I_REJOIN);
        add(I_TOGETHER), add(I_RACE), add(I_DIRECTCODE);
        // (uxfix #5: Safe Copies is not on the front door any more: it is a small Triangle action on Which File?; owner: "no tacked-on options")
        break;
    }
    default: return false;
    }
    (void)param, (void)add;
    return true;
}

bool Sequence::chooseMain(int item) {
    Io* io = mIo;
    auto decide = [io] { if (io) io->sound(Se::Decide); };
    (void)decide;
    switch (item) {
    case I_TOGETHER:
    case I_RACE:
        // flow: the mode is asked here and nowhere else; a name the first time, the character once, then Join / Host
        decide();
        mMode = item == I_RACE ? MP_MODE_RACE : MP_MODE_TOGETHER;
        requireName(A_HOSTJOIN);
        return true;
    case I_REJOIN:
        {   // flow: back into the room this console dropped out of
        decide();
        mRejoinUntil = 0;
        mOriginalHost = false;
        if (mp_ui_rejoin && mp_ui_rejoin()) {
            MpChat::noteRoomPublic(false);
            startBusy(B_REJOIN);
        } else {
            if (io) io->sound(Se::Buzzer);
            noteErrorShown(MP_ERR_ROOM_NOT_FOUND);
            say(P_INFO, MP_T_DLG_REJOIN_GONE);
        }
        return true;
    }
    case I_BOARDS:
        decide(); mConn = MP_CONN_ONLINE; requireName(A_BOARD); return true;
    case I_FRIENDS:
        {   // m-friends: the Friends screens sign in by themselves ("Connecting..."); only where the online service is
        if (!NetInfo().service) return true;
        decide();
        mConn = MP_CONN_ONLINE;
        sFriendCount = -1;
        push(S_FRIENDS, 0);
        return true;
    }
    case I_DIRECTCODE:
        decide();
        requireName(A_CODE);
        return true;
    case I_HOWTO:
        // flow: one native window with three short lines (the full pages come later)
        decide();
        // vitabtn: our own page with the Vita's glyphs (the game's Co-Star manual is about the Wii Remote); the menu stays
        mp_log("ui: how to play");
        say(P_HOWTO, MP_T_DLG_HOWTO);
        return true;
    case I_BACK:
        if (io) io->sound(Se::Close);
        close();
        return true;
    case I_QUICK:
        // uxfix #3: from the gathering screen: whatever goes wrong (the account, the search, the owner's daily limit) or is cancelled, the
        // player lands on the gathering screen again, looking for friends, not stranded on the Multiplayer menu
        if (mDepth > 0 && top().scr == S_GATHER) mQuickRegather = true;
        if (needAccount(item)) return true;
        if (mDepth > 0 && top().scr == S_GATHER) {   // flow (one door): Play with Anyone from the gathering screen: it stops looking here
            mp_gather_stop();
            pop();
        }
        // fix2 / flow: Quick Match may end up hosting: then the host is asked "Which file?" at Start like any host
        decide();
        defaultCoopSave();
        mOriginalHost = false;   // (known when the search ends: tickBusy asks the core)
        mConn = MP_CONN_ONLINE;
        if (mp_ui_quick_match(mMode)) { MpChat::noteRoomPublic(true); startBusy(B_QUICK); }   // gamefix: Quick Match rooms are public
        else showError(failError(MP_ERR_INTERNAL));
        return true;
    default: break;
    }
    return false;
}

bool Sequence::dialogMain(int purpose, bool yes, int param) {
    switch (purpose) {
    case P_TIP_NEXT:
        showTip(); return true;
    case P_CODE_BAD:
    case P_CANT_REACH:
        openCodeKeyboard();   // uxfix #1: the typed text comes back; Cancel on the keyboard leaves it on the Multiplayer menu
        return true;
    case P_HOWTO:
        // menus4: How to Play's second page: Keep My Multiplayer Data Safe (Safe Copies is three presses from the Multiplayer menu: Triangle, OK, Yes)
        ask(P_HOWTO_SAFE, MP_T_KS_HOWTO_SAFE, Args(), Dlg::YesNo);
        return true;   // (uxfix #7: the Connection Test is offered after a join that failed, not after How to Play)
    case P_HOWTO_SAFE:
        if (yes) choose(I_KEEP);
        return true;
    default: return false;
    }
    return true;
}

bool Sequence::keyboardMain(int purpose, const char* text) {
    switch (purpose) {
    case K_DIRECT_CODE:
        {   // flow (one door): a friend far away read us their code. Anything but OK: one plain line; nothing about addresses
        snprintf(mCodeText, sizeof mCodeText, "%s", text);
        mCodeKeep = true;   // (until a game forms: a failure of any kind brings the same text back)
        if (!NetInfo().wifiUp) {   // uxfix #1: Wi-Fi went while typing
            mp_log("ui: a friend's code: no Wi-Fi");
            showError(MP_ERR_NO_WIFI);
            return true;
        }
        const int r = mp_ui_join_direct_code(text);
        mp_log("ui: join with a code: %d", r);
        if (r != MP_CODE_OK) {
            if (mIo) mIo->sound(Se::Buzzer);
            say(P_CODE_BAD, MP_T_DLG_CODE_BAD);   // uxfix #1: OK, and the keyboard is open again with the text still in it
            return true;
        }
        openGather(true);
        return true;
    }
    case K_NAME_LOCAL:
        {
        mp_ui_set_profile(text, MP_LOOK_MARIO);
        continueAfterName(mAfterRegister);
        return true;
    }
    default: return false;
    }
    return true;
}

bool Sequence::tickMain(Scr scr, const Input& in, bool* stop) {
    (void)in, (void)stop;
    if (scr == S_MAIN && mRejoinUntil != 0 && mFrame >= mRejoinUntil) {   // lobby: the Continue row went; one kind line says why (it did not just vanish)
        mRejoinUntil = 0;
        char16_t t[96];
        txt::format(t, 96, MP_T_REJOIN_CLOSED, Args());
        MpOverlay::toastText(t, 0);
        mp_log("ui: the Continue row expired");
    }
    return false;
}

bool Sequence::labelMain(int item, MpPanel::Row& r, const Frame& f) const {
    using namespace MpPanel;
    (void)f;
    switch (item) {
    case I_TOGETHER:
        Text(r.label, kLabel, MP_T_PLAY_TOGETHER); return true;
    case I_RACE:
        Text(r.label, kLabel, MP_T_STAR_RACE); return true;
    case I_BOARDS:
        Text(r.label, kLabel, MP_T_LEADERBOARDS); return true;
    case I_FRIENDS:
        Text(r.label, kLabel, MP_T_FRIENDS); return true;
    case I_REJOIN:
        Str(r.label, kLabel, MP_T_REJOIN, mRejoinHost); return true;   // flow
    case I_DIRECTCODE:
        Text(r.label, kLabel, MP_T_JOIN_CODE); return true;   // flow (one door)
    case I_QUICK:
        Text(r.label, kLabel, MP_T_PLAY_ANYONE); return true;
    default: break;
    }
    return false;
}

bool Sequence::modelMain(MpPanel::Model* m, const Frame& f, int cursorItem, int count, const int* it) const {
    using namespace MpPanel;
    auto button = [m](int pic, MpText label) { ModelButton(m, pic, label); };
    auto hint = [m](MpText t) { Text(m->hint, 80, t); };
    auto label = [this, &f](int item, Row& r) { rowLabel(item, r, f); };
    (void)button, (void)hint, (void)label, (void)cursorItem, (void)count, (void)it;
    switch (f.scr) {
    case S_MAIN:
        {
        m->kind = KIND_BUTTONS;
        Text(m->title, 48, MP_T_TITLE_MULTI);
        GameLook(m);   // flow look
        button(txt::PIC_CROSS, MP_T_SELECT);
        if (mFromFileSelect) button(txt::PIC_TRIANGLE, MP_T_HOW_TO_PLAY);
        if (NetInfo().service) button(txt::PIC_SQUARE, MP_T_FRIENDS);   // (no service: no Friends, never a dead button)
        if (NetInfo().service) button(txt::PIC_START, MP_T_RP_HELP);    // beta diagnostics: Having Trouble? (Check Connection, Send a Problem Report): no fifth big button, a guide button
        button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    }
    default: return false;
    }
    return true;
}

// Input this family takes before the shared rules (stage 0: a direction, for grids; stage 1: Square / Triangle). True: taken.
bool Sequence::pressMain(Scr scr, const Input& in, int item, int count, int stage) {
    if (stage == 1 && in.sq && scr == S_MAIN) {   // flow (one door): Friends, where the online service is
        if (NetInfo().service) choose(I_FRIENDS);
        return true;
    }
    if (stage == 1 && in.tri && scr == S_MAIN && mFromFileSelect) {
        choose(I_HOWTO);
        return true;
    }
    (void)scr, (void)in, (void)item, (void)count, (void)stage;
    return false;
}

// Back (Circle) on one of this family's screens: phase 0 before anything (true: taken, nothing else happens), 1 just before the
// screen goes, 2 just after.
bool Sequence::leaveMain(Scr scr, uint8_t param, int phase) {

    (void)scr, (void)param, (void)phase;
    return false;
}

}  // namespace MpMenu
