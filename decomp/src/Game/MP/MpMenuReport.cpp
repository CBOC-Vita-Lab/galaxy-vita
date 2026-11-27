// Galaxy 3.0 multiplayer: beta diagnostics (Having Trouble?): Check Connection and Send a Problem Report, and the one-time offer after a
// session ended with an error. A screen family of MpMenu::Sequence (flow split). The work itself is the port's (port/mp_diag.h: the
// report's body, its POST, the NAT / relay check); this file is the screens and the words only. Nothing here touches saves or settings.
#include "Game/MP/MpMenuReport.hpp"

#include "MpMenuInternal.hpp"

namespace MpMenu {

namespace {
constexpr uint32_t kSendWatchdog = 40 * kFps;    // the port times the POST out at 30 s; this only catches a port that never answers
constexpr uint32_t kCheckWatchdog = 45 * kFps;
enum { W_SEND = 0, W_CHECK = 1 };                // S_RPWAIT's param
enum { R_FROM_REVIEW = 1 };                      // a window's param: the "What's Sent" page opened from the consent screen (no second consent screen)
}  // namespace

// ------------------------------------------------------------------ entry points
void Sequence::openReport(int where, int kind) {
    mRpWhere = where;
    mRpKind = kind;
    mRpNote[0] = 0;
    mp_log("ui: report flow opened where=%d kind=%d", where, kind);
    choose(I_RP_SEND);
}

void Sequence::openCheck() {
    mp_log("ui: connection check opened");
    choose(I_RP_CHECK);
}

// The Multiplayer menu opened: a session ended with an error since the last offer -> ask once.
bool Sequence::offerReportAfterError() {
    if (!mp_diag_error_pending || !mp_diag_error_pending() || mDlg || mKb || !NetInfo().service) return false;
    if (mp_diag_error_clear) mp_diag_error_clear();   // once: whatever the answer
    mp_log("ui: report offered after an error");
    ask(P_RP_AFTER, MP_T_RP_AFTER_ERR, Args(), Dlg::YesNo);
    return true;
}

// ------------------------------------------------------------------ the flow
void Sequence::reportFinished(int result, const char* id) {
    if (mDepth > 0 && top().scr == S_RPWAIT) pop();
    mp_log("ui: report result %d id=%s", result, id && *id ? id : "-");
    if (result == MP_DIAG_OK) {
        if (mDepth > 0 && top().scr == S_REPORT) pop();   // (done: back where it began)
        if (mIo) mIo->sound(Se::Success);
        Args a;
        a.str = id;
        say(P_RP_THANKS, MP_T_RP_THANKS, a);
        return;
    }
    MpText t = MP_T_RP_E_FAIL;
    switch (result) {
    case MP_DIAG_OFFLINE: t = MP_T_RP_E_OFFLINE; break;
    case MP_DIAG_TIMEOUT: t = MP_T_RP_E_TIMEOUT; break;
    case MP_DIAG_BAD_REQUEST: t = MP_T_RP_E_REJECT; break;
    case MP_DIAG_TOO_LARGE: t = MP_T_RP_E_BIG; break;
    case MP_DIAG_RATE_LIMITED: t = MP_T_RP_E_LIMIT; break;
    case MP_DIAG_BUSY: t = MP_T_RP_E_BUSY; break;
    case MP_DIAG_CLOCK: t = MP_T_ERR_CLOCK; break;
    default: break;
    }
    if (mIo) mIo->sound(Se::Buzzer);
    say(P_RP_ERR, t);   // (the consent screen stays: the player can try again, or cancel)
}

void Sequence::reportSend() {
    if (!mp_diag_report_begin || !mp_diag_report_begin(mRpKind, mRpWhere, mRpNote)) {
        mp_log("ui: report could not start");
        if (mIo) mIo->sound(Se::Buzzer);
        say(P_RP_ERR, MP_T_RP_E_FAIL);
        return;
    }
    mRpStart = mFrame;
    push(S_RPWAIT, W_SEND);
}

void Sequence::checkFinished(const MpDiagCheck& c) {
    if (mDepth > 0 && top().scr == S_RPWAIT) pop();
    mp_log("ui: check result nat=%s stun=%d relay=%s rtt=%d err=%s", c.nat, c.stunMs, c.relay, c.relayRttMs, c.err);
    MpText direct = MP_T_RP_CK_DIRECT_RELAY;   // symmetric / unknown: the relay may be needed
    if (!strcmp(c.nat, "open") || !strcmp(c.nat, "cone")) direct = MP_T_RP_CK_DIRECT_OK;
    else if (!strcmp(c.nat, "blocked")) direct = MP_T_RP_CK_DIRECT_BLOCKED;
    MpText relay = MP_T_RP_CK_RELAY_SKIP;
    if (!strcmp(c.relay, "ok")) relay = MP_T_RP_CK_RELAY_OK;
    else if (!strcmp(c.relay, "fail")) relay = MP_T_RP_CK_RELAY_FAIL;
    const MpText ids[2] = {direct, relay};
    Args args[2];
    args[1].num = c.relayRttMs > 0 ? c.relayRttMs : 0;
    txt::setDynamicLinesArgs(0, ids, args, 2);
    mDlg = P_RP_CKRES;
    mDlgParam = 0;
    strncpy(mLastDialogId, "MP_DYN0", sizeof mLastDialogId - 1);
    if (mIo) {
        mIo->sound(Se::Success);
        mIo->dialogOpen(Dlg::Ok, "MP_DYN0");
    }
}

// ------------------------------------------------------------------ hooks
bool Sequence::itemsReport(Scr s, uint8_t param, int* out, int* np) const {
    int& n = *np;
    auto add = [&](int i) { if (n < kItems) out[n++] = i; };
    (void)param;
    switch (s) {
    case S_HELP: add(I_RP_CHECK), add(I_RP_SEND); break;
    case S_REPORT: add(I_RP_BSEND), add(I_RP_BNOTE), add(I_RP_BCANCEL); break;
    case S_RPWAIT: break;
    default: return false;
    }
    return true;
}

bool Sequence::chooseReport(int item) {
    Io* io = mIo;
    auto decide = [io] { if (io) io->sound(Se::Decide); };
    switch (item) {
    case I_HELP:
        decide();
        push(S_HELP);
        return true;
    case I_RP_CHECK:
        decide();
        if (!mp_diag_check_begin || !mp_diag_check_begin()) {
            mp_log("ui: check could not start");
            if (io) io->sound(Se::Buzzer);
            say(P_RP_ERR, MP_T_RP_E_FAIL);
            return true;
        }
        mRpStart = mFrame;
        push(S_RPWAIT, W_CHECK);
        return true;
    case I_RP_SEND:
        decide();
        // a report begun from Having Trouble? is the player's own: a fresh note, kind "user" (the offer and the Settings door set theirs before choosing)
        if (mDepth > 0 && top().scr == S_HELP) {
            mRpKind = 0;
            mRpWhere = 0;
            mRpNote[0] = 0;
        }
        // the online account first (the existing sign-up; this row is chosen again after it), then what is sent
        if (!(mp_diag_has_account && mp_diag_has_account()) && needAccount(I_RP_SEND)) return true;
        say(P_RP_INFO1, MP_T_RP_INFO1);
        return true;
    case I_RP_BSEND:
        decide();
        reportSend();
        return true;
    case I_RP_BNOTE:
        decide();
        openKeyboard(K_REPORT_NOTE, MP_T_RP_KB_NOTE, mRpNote, MP_DIAG_NOTE_MAX, false);
        return true;
    case I_RP_BCANCEL:
        if (io) io->sound(Se::Cancel);
        mp_log("ui: report cancelled");
        pop();
        return true;
    default: return false;
    }
}

bool Sequence::dialogReport(int purpose, bool yes, int param) {
    switch (purpose) {
    case P_RP_INFO1:
        say(P_RP_INFO2, MP_T_RP_INFO2, Args(), param);
        return true;
    case P_RP_INFO2:
        if (param != R_FROM_REVIEW) push(S_REPORT);   // (the "What's Sent" page from the consent screen: back to it)
        return true;
    case P_RP_AFTER:
        if (yes) {
            mRpKind = 1;
            mRpWhere = 2;
            mRpNote[0] = 0;
            choose(I_RP_SEND);
        } else {
            mp_log("ui: report offer declined");
        }
        return true;
    case P_RP_THANKS:
    case P_RP_ERR:
    case P_RP_CKRES:
        return true;
    default: return false;
    }
}

bool Sequence::keyboardReport(int purpose, const char* text) {
    if (purpose != K_REPORT_NOTE) return false;
    snprintf(mRpNote, sizeof mRpNote, "%s", text);
    mp_log("ui: report note set (%d bytes)", static_cast<int>(strlen(mRpNote)));
    return true;
}

bool Sequence::tickReport(Scr scr, const Input& in, bool* stop) {
    if (scr == S_MAIN && in.start && !mDlg && !mKb && NetInfo().service) {   // Start on the Multiplayer menu: Having Trouble? (a guide button: no fifth big button)
        choose(I_HELP);
        *stop = true;
        return true;
    }
    if (scr != S_RPWAIT) return false;
    if (mDepth == 0 || top().scr != S_RPWAIT || mDlg) { *stop = true; return true; }
    const bool check = top().param == W_CHECK;
    if (in.back) {
        mp_log("ui: %s cancelled", check ? "check" : "report");
        if (mIo) mIo->sound(Se::Cancel);
        if (check) { if (mp_diag_check_cancel) mp_diag_check_cancel(); }
        else if (mp_diag_report_cancel) mp_diag_report_cancel();
        pop();
        *stop = true;
        return true;
    }
    if (check) {
        MpDiagCheck c;
        memset(&c, 0, sizeof c);
        const int r = mp_diag_check_poll ? mp_diag_check_poll(&c) : MP_DIAG_FAILED;
        if (r != MP_DIAG_PENDING) {
            if (r != MP_DIAG_OK) { snprintf(c.nat, sizeof c.nat, "unknown"); snprintf(c.relay, sizeof c.relay, "skipped"); c.stunMs = -1; c.relayRttMs = -1; }
            checkFinished(c);
        } else if (mFrame - mRpStart > kCheckWatchdog) {
            if (mp_diag_check_cancel) mp_diag_check_cancel();
            reportFinished(MP_DIAG_TIMEOUT, nullptr);
        }
    } else {
        char id[16] = {0};
        const int r = mp_diag_report_poll ? mp_diag_report_poll(id, sizeof id) : MP_DIAG_FAILED;
        if (r != MP_DIAG_PENDING) {
            reportFinished(r, id);
        } else if (mFrame - mRpStart > kSendWatchdog) {
            if (mp_diag_report_cancel) mp_diag_report_cancel();
            reportFinished(MP_DIAG_TIMEOUT, nullptr);
        }
    }
    *stop = true;
    return true;
}

bool Sequence::labelReport(int item, MpPanel::Row& r, const Frame& f) const {
    using namespace MpPanel;
    (void)f;
    switch (item) {
    case I_HELP: Text(r.label, kLabel, MP_T_RP_HELP); return true;
    case I_RP_CHECK: Text(r.label, kLabel, MP_T_RP_CHECK); return true;
    case I_RP_SEND: Text(r.label, kLabel, MP_T_RP_SEND); return true;
    case I_RP_BSEND: Text(r.label, kLabel, MP_T_RP_B_SEND); return true;
    case I_RP_BNOTE: Text(r.label, kLabel, MP_T_RP_B_NOTE); return true;
    case I_RP_BCANCEL: Text(r.label, kLabel, MP_T_RP_B_CANCEL); return true;
    default: return false;
    }
}

bool Sequence::modelReport(MpPanel::Model* m, const Frame& f, int cursorItem, int count, const int* it) const {
    using namespace MpPanel;
    auto button = [m](int pic, MpText label) { ModelButton(m, pic, label); };
    (void)count, (void)it;
    switch (f.scr) {
    case S_HELP:
        // one line of what the row under the cursor does (the Multiplayer menu itself has no room for a line)
        m->kind = KIND_BUTTONS;
        Text(m->title, 48, MP_T_RP_HELP);
        GameLook(m, cursorItem == I_RP_SEND ? MP_T_RP_DESC_SEND : MP_T_RP_DESC_CHECK);
        button(txt::PIC_CROSS, MP_T_SELECT);
        button(txt::PIC_CIRCLE, MP_T_BACK);
        return true;
    case S_REPORT:
        m->kind = KIND_BUTTONS;
        Text(m->title, 48, MP_T_RP_SEND);
        GameLook(m, mRpNote[0] ? MP_T_RP_ASK_NOTE : MP_T_RP_ASK);
        button(txt::PIC_CROSS, MP_T_SELECT);
        button(txt::PIC_TRIANGLE, MP_T_RP_B_INFO);
        button(txt::PIC_CIRCLE, MP_T_CANCEL);
        return true;
    case S_RPWAIT:
        m->kind = KIND_BUTTONS;
        Text(m->title, 48, f.param == W_CHECK ? MP_T_RP_CHECK : MP_T_RP_SEND);
        {
            const MpText s = f.param == W_CHECK ? MP_T_RP_CHECKING : MP_T_RP_SENDING;
            Text(m->status, 80, s);
            GameLook(m, s);
        }
        m->spinner = (mFrame % 90) / 90.0f;
        button(txt::PIC_CIRCLE, MP_T_CANCEL);
        return true;
    default: return false;
    }
}

bool Sequence::pressReport(Scr scr, const Input& in, int item, int count, int stage) {
    (void)item, (void)count;
    if (stage == 1 && in.tri && scr == S_REPORT) {   // "What's Sent" again
        say(P_RP_INFO1, MP_T_RP_INFO1, Args(), R_FROM_REVIEW);
        return true;
    }
    return false;
}

bool Sequence::leaveReport(Scr scr, uint8_t param, int phase) {
    (void)scr, (void)param, (void)phase;
    return false;
}

}  // namespace MpMenu

namespace MpReport {
void open(int where) { MpMenu::sequence().openReport(where, 0); }
void openCheck() { MpMenu::sequence().openCheck(); }
}  // namespace MpReport
