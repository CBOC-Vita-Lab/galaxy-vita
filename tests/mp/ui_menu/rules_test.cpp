// The menus' host tests: the rules family (flow split of ui_menu_test.cpp). Shared helpers: common.hpp.
#include "common.hpp"

// touches: the host-rules card: a window in the lobby the first time, a toast for a late joiner, "don't show again" kept in mp.cfg
void testRulesCard() {
    fresh();
    fake::settings.interaction = 1;
    fake::rulesWanted = true;
    toQuick(); session(false, 1, MP_UI_LOBBY);   // (Play with Anyone found Player's game: its lobby)
    io.answer = MpMenu::DlgState::No;   // (uxfix #7: the card is an OK card now: whatever the answer, it asks nothing and never says "never again")
    frames(4);
    CHECK(LoggedPart("ui: rules card host=Player mode=0 falls=0 bump=1") && io.opened >= 1 && io.last == "MP_DYN0");
    frames(3);
    CHECK(fake::rulesDone && !fake::rulesNever && fake::rulesKey != 0);
    // the card text is the host's rules in two lines: no bumping line, no question
    for (int lang = 0; lang < MpMsgText::LANG_COUNT; lang++) {
        MpMsgText::setLanguage(lang);
        MpMsgText::setDynamicRules(0, MpMsgText::Rules{"Player", 0, 0, true});
        int lines = 1;
        for (const char16_t* p = MpMsgText::dynamicText(0) + 3; *p; ++p) lines += *p == u'\n';
        CHECK(lines == 2);
    }
    MpMsgText::setLanguage(MpMsgText::LANG_EN);
    char16_t want[96];
    MpMsgText::Rules r{"Player", 0, 0, true};
    MpMsgText::setDynamicRules(0, r);
    CHECK(std::u16string(MpMsgText::dynamicText(0)).find(u"Player") != std::u16string::npos);
    (void)want;
    io.answer = MpMenu::DlgState::Yes;
    // a late joiner (straight into the play): the toast, nothing blocks
    fresh();
    fake::settings.interaction = 1;
    fake::rulesWanted = true;
    session(false, 1, MP_UI_PLAYING);
    frames(4);
    CHECK(LoggedPart("ui: toast \"Rules (Player): ") && !S().dialogActive());
    // "don't show again" on: nothing at all
    fresh();
    fake::settings.interaction = 1;
    fake::rulesWanted = false;
    fake::rulesHidden = true;
    session(false, 1, MP_UI_PLAYING);
    frames(4);
    CHECK(!LoggedPart("is the host"));
    // the host changes the falls rule in play: a toast once he is done
    fresh();
    fake::settings.interaction = 1;
    session(false, 1, MP_UI_PLAYING);
    frames(4);
    fake::settings.downed = MP_DOWNED_INFINITE;
    frames(MpRulesLogic::kSettleFrames + 10);
    CHECK(LoggedPart("ui: toast \"Player changed the rules: Play Together, try again.\""));
    // the host himself never sees a card
    fresh();
    fake::settings.interaction = 1;
    fake::rulesWanted = true;
    session(true, 0, MP_UI_PLAYING);
    frames(4);
    CHECK(!LoggedPart("ui: rules"));
}

// touches: a chime when a player joins, a soft sound when one leaves
void testJoinLeaveSounds() {
    fresh();
    session(true, 0, MP_UI_PLAYING);
    frames(4);
    io.sounds.clear();
    fake::players[3].present = true;
    snprintf(fake::players[3].name, sizeof fake::players[3].name, "Wario");
    frames(3);
    CHECK(io.count(MpMenu::Se::Join) == 1 && io.count(MpMenu::Se::Leave) == 0);
    fake::players[3].present = false;
    frames(3);
    CHECK(io.count(MpMenu::Se::Join) == 1 && io.count(MpMenu::Se::Leave) == 1);
    // two at once: one chime per player, the strip of toasts still names them
    io.sounds.clear();
    fake::players[3].present = true;
    fake::players[2].present = false;
    frames(3);
    CHECK(io.count(MpMenu::Se::Join) == 1 && io.count(MpMenu::Se::Leave) == 1);
    io.sounds.clear();
}

// touches: the destination card and the host-browsing lines
void testPickToast() {
    fresh();
    fake::active = true;
    const char16_t* names[3] = {u"Honeyhive Galaxy", u"Galaxie Rucheruche", u"Galaxia Colmena"};
    const char* want[3] = {"ui: toast \"Player picked Honeyhive Galaxy, Star 2.\"", "ui: toast \"Player a choisi Galaxie Rucheruche, \x3f" "toile 2.\"",
                           "ui: toast \"Player eligi\x3f"" Galaxia Colmena, estrella 2.\""};
    for (int lang = 0; lang < 3; lang++) {
        MpMsgText::setLanguage(lang);
        fake::log.clear();
        MpOverlay::toastPick(0, names[lang], 2);
        bool seen = false;
        for (auto& l : fake::log) seen = seen || l == want[lang];
        CHECK(seen);
        MpOverlay::toast(MP_T_PICKED, 0, 5);   // (never from the wire: ignored)
    }
    MpMsgText::setLanguage(MpMsgText::LANG_EN);
    fake::log.clear();
    MpOverlay::toastPick(0, u"", 1);   // no name: nothing shown
    MpOverlay::toastPick(0, nullptr, 1);
    MpOverlay::toast(MP_T_HOST_BROWSING, 0, 0);
    bool b = false;
    for (auto& l : fake::log) b = b || l == "ui: toast \"Player is choosing a galaxy.\"";
    CHECK(b && fake::log.size() == 1);
    // every language fits a toast line with the longest names
    for (int lang = 0; lang < 3; lang++) {
        MpMsgText::setLanguage(lang);
        MpMsgText::Args a;
        a.str = "Toadsworth12345";
        a.str2 = "Good Egg Galaxy Marathon";
        a.num = 6;
        char16_t t[96];
        for (MpText id : {MP_T_PICKED, MP_T_HOST_BROWSING, MP_T_HOST_BROWSING_STAR}) CHECK(MpMsgText::format(t, 96, id, a) < 80);
    }
    MpMsgText::setLanguage(MpMsgText::LANG_EN);
}

