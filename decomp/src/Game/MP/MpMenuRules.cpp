// Galaxy 3.0 multiplayer: the menus, the host-rules card (touches) and other cards (flow split of MpMenuSequence.cpp, 11 Nov).
// Owner of this family's screens: see MpMenuSequence.cpp "the file map". Pure code, as MpMenuSequence.cpp.
#include "MpMenuInternal.hpp"

extern "C" bool mp_update_take_notice(void) __attribute__((weak));   // release3: port/mp_update.h (platform side: not in the game's include path)

namespace MpMenu {

// touches: the host's rules, in the game's own window (lobby) or as a short toast (a late joiner, a change in play).
void Sequence::rulesTick(bool inSession) {
    if (!inSession || mp_is_host() || mp_host_slot() < 0 || mp_host_slot() >= MP_MAX_PLAYERS || !mp_slot_present(mp_host_slot())) {
        mRules.reset();
        return;
    }
    const MpSettings* st = mp_settings();
    MpMsgText::Rules r{mp_slot_name(mp_host_slot()), st->mode == MP_MODE_RACE ? 1 : 0, st->downed == MP_DOWNED_INFINITE ? 1 : 0,
                       st->interaction != 0};
    bool inLobby = mDepth > 0 && top().scr == S_GATHER && mRoom && !mDlg && !mKb;
#if defined(TARGET_PC) && !defined(__vita__)
    {   // desktop test hook (never on the Vita): SMG_MP_TEST_RULES=<secs> shows the card in play too (a harness has no lobby) from <secs> of frames, for a screenshot
        static const int hookSecs = getenv("SMG_MP_TEST_RULES") ? atoi(getenv("SMG_MP_TEST_RULES")) : -1;   // seconds of frames
        if (hookSecs >= 0) {
            if (mFrame < hookSecs * 60) return;   // (not yet: nothing, not even the late joiner's toast)
            if (mDepth == 0 && !mDlg && !mKb) inLobby = true;
        }
    }
#endif
    const uint32_t key = MpRulesLogic::hostKey(r.host);
    const MpRulesLogic::Action a = mRules.step(key, MpRulesLogic::sigOf(r.mode, r.falls, r.bump), inLobby, mp_core_rules_wanted(key),
                                               mp_core_rules_hidden());
    if (a == MpRulesLogic::NONE) return;
    mp_log("ui: rules %s host=%s mode=%d falls=%d bump=%d", a == MpRulesLogic::CARD ? "card" : (a == MpRulesLogic::TOAST_JOIN ? "toast" : "change toast"),
           r.host, r.mode, r.falls, r.bump ? 1 : 0);
    if (a == MpRulesLogic::CARD) {
        mDlg = P_RULES;
        mDlgParam = 0;
        txt::setDynamicRules(0, r);
        strncpy(mLastDialogId, "MP_RULES", sizeof mLastDialogId - 1);
        if (mIo) mIo->dialogOpen(Dlg::Ok, "MP_DYN0");   // uxfix #7: an OK card, not a question
    } else {
        char16_t t[96];
        txt::formatRulesToast(t, 96, r, a == MpRulesLogic::TOAST_CHANGE);
        MpOverlay::toastText(t, LookIcon(mp_host_slot()));
    }
}

bool Sequence::itemsRules(Scr s, uint8_t param, int* out, int* np) const {
    (void)s, (void)param, (void)out, (void)np;
    return false;
}

bool Sequence::chooseRules(int item) {
    Io* io = mIo;
    auto decide = [io] { if (io) io->sound(Se::Decide); };
    (void)decide;
    return false;
}

bool Sequence::dialogRules(int purpose, bool yes, int param) {
    switch (purpose) {
    case P_RULES:
        // touches (uxfix #7: an OK card now, no "Show this next time?"): it shows again for a new host, as before
        mp_core_rules_done(mRules.key, false);
        mp_log("ui: rules card read");
        return true;
    default: return false;
    }
    return true;
}

bool Sequence::keyboardRules(int purpose, const char* text) {
    (void)purpose;
    (void)text;
    return false;
}

bool Sequence::tickRules(Scr scr, const Input& in, bool* stop) {
    (void)scr, (void)in, (void)stop;
    return false;
}

bool Sequence::labelRules(int item, MpPanel::Row& r, const Frame& f) const {
    using namespace MpPanel;
    (void)f;
    return false;
}

bool Sequence::modelRules(MpPanel::Model* m, const Frame& f, int cursorItem, int count, const int* it) const {
    using namespace MpPanel;
    auto button = [m](int pic, MpText label) { ModelButton(m, pic, label); };
    auto hint = [m](MpText t) { Text(m->hint, 80, t); };
    auto label = [this, &f](int item, Row& r) { rowLabel(item, r, f); };
    (void)button, (void)hint, (void)label, (void)cursorItem, (void)count, (void)it;
    return false;
}

// Input this family takes before the shared rules (stage 0: a direction, for grids; stage 1: Square / Triangle). True: taken.
bool Sequence::pressRules(Scr scr, const Input& in, int item, int count, int stage) {

    (void)scr, (void)in, (void)item, (void)count, (void)stage;
    return false;
}

// Back (Circle) on one of this family's screens: phase 0 before anything (true: taken, nothing else happens), 1 just before the
// screen goes, 2 just after.
bool Sequence::leaveRules(Scr scr, uint8_t param, int phase) {

    (void)scr, (void)param, (void)phase;
    return false;
}

}  // namespace MpMenu
