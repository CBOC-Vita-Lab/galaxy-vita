// lobby (12 Nov): the old lobby screen is gone. A game the core already holds (Play with Anyone, a friend's game, Continue X's Game)
// shows the gathering screen like every other game: heads, one line, big buttons; Start and Leave go to the core, not to the gathering.
#include "common.hpp"

namespace {
MpPanel::Model& G() { static MpPanel::Model m; S().buildModel(&m); return m; }
std::string lineNow() { return U8(G().line); }
std::string headName(int i) { MpPanel::Model& m = G(); return i < m.headCount ? U8(m.rows[i].label) : "<none>"; }
bool guideHasPic(int pic) { MpPanel::Model& m = G(); for (int i = 0; i < m.buttonCount; i++) if (m.buttons[i].pic == pic) return true; return false; }
bool sawWindow(const char* id) { for (auto& i : io.ids) if (i == id) return true; return false; }
void alone() { fake::players[1].present = fake::players[2].present = false; }
}  // namespace

void testLobbyIsGathering() {
    // Play with Anyone found nobody: this console's room, alone: the gathering screen, one head (the crown), the waiting line, no rows
    fresh();
    toGather(true); gatherHeads(1, 1); ok();   // (Star Race: Play with Anyone)
    session(true, 0, MP_UI_LOBBY); alone(); frames(4);
    CHECK(S().screen() == MpMenu::S_GATHER && Logged("ui: the game's gathering screen (online room)"));
    CHECK(G().kind == MpPanel::KIND_HEADS && G().headCount == 1 && headName(0) == "Player" && (G().rows[0].marks & MpPanel::MARK_CROWN));
    CHECK(lineNow() == "Waiting for friends..." && S().itemCount() == 0 && !guideHasPic(MpMsgText::PIC_START));
    CHECK(guideHasPic(MpMsgText::PIC_TRIANGLE) && guideHasPic(MpMsgText::PIC_CIRCLE));
    CHECK(lineNow().find("Code") == std::string::npos && U8(G().subtitle).empty());   // (no title band, no code)
    press(7);
    CHECK(startsAll() == 0 && S().screen() == MpMenu::S_GATHER);   // (alone: the buzzer, no dead row)
    // a friend comes in: two heads, START in the guide; START asks the core (not the gathering driver)
    fake::players[1].present = true; frames(3);
    CHECK(G().headCount == 2 && headName(1) == "Luigi64" && lineNow().find("to start!") != std::string::npos && guideHasPic(MpMsgText::PIC_START));
    CHECK(S().itemCount() == 0);   // (the greyed Start row is gone)
    press(7);
    CHECK(fake::starts == 1 && mp_gather_fake_begins() == 0);
    fake::status.phase = MP_UI_PLAYING; frames(3);
    int file = 0;
    CHECK(S().takeLaunch(&file));
    // a game a friend opened (or Continue X's Game): a guest sees who starts, cannot start, can leave (asked, then the core is told)
    fresh();
    dropOut(); openMain(); ok(); frames(2);
    CHECK(S().screen() == MpMenu::S_BUSY);
    session(false, 1, MP_UI_LOBBY); frames(4);
    CHECK(S().screen() == MpMenu::S_GATHER && G().headCount >= 2 && (G().rows[0].marks & MpPanel::MARK_YOU) && !(G().rows[0].marks & MpPanel::MARK_CROWN));
    CHECK(lineNow() == "Waiting for Player to start..." && !guideHasPic(MpMsgText::PIC_START) && S().itemCount() == 0);
    press(7); ok();
    CHECK(fake::starts == 0);
    io.ids.clear();
    back(); frames(2);
    CHECK(sawWindow("MP_LEAVE_GUEST") && fake::leaves == 1 && S().screen() != MpMenu::S_GATHER);
    // a host by migration in such a room starts at once (no Which File?)
    fresh();
    toGather(true); gatherHeads(1, 1); ok();
    session(true, 0, MP_UI_LOBBY); frames(4);
    press(7); frames(2);
    CHECK(S().screen() == MpMenu::S_GATHER && fake::starts == 1);
    // a host closing it is asked first
    fresh();
    toGather(true); gatherHeads(1, 1); ok();
    session(true, 0, MP_UI_LOBBY); frames(4);
    io.ids.clear();
    back(); frames(2);
    CHECK(sawWindow("MP_CLOSE_ROOM") && fake::leaves == 1);
}

// lobby (coordinator, monkey test): a lone player with the service and both radios, 20 s on: three big buttons, none under 0.9, in every language
void testGatherThreeButtons() {
    for (int l = 0; l < MpMsgText::LANG_COUNT; l++) {
        MpMsgText::setLanguage(l);
        fresh();
        fake::codeAvailable = true;
        toGather(); gatherHeads(1, 1); frames(20 * 60 + 5);
        static MpPanel::Model m;
        S().buildModel(&m);
        CHECK(S().screen() == MpMenu::S_GATHER && m.headCount == 1 && m.rowCount - m.headCount == 3);
        static MpPanel::Scene scene;
        static MpPanel::Plan plan;
        MpPanel::FontMetrics fm[MpPanel::FONT_COUNT];
        for (auto& f : fm) f.height = 33.0f, f.baseline = 26.0f, f.advance = [](void*, uint16_t) { return 16.0f; }, f.has = [](void*, uint16_t) { return true; };
        for (float width : {810.6667f, 608.0f}) {
            MpPanel::Screen scr;
            scr.width = width;
            MpPanel::beginScene(&scene);
            MpPanel::begin(&plan);
            MpPanel::compose(m, scr, fm, 0.25f, &scene, &plan);
            int buttons = 0;
            for (int i = 0; i < scene.useCount; i++)
                if (scene.uses[i].kind == MpPanel::USE_BUTTON) { ++buttons; CHECK(scene.uses[i].w >= 0.9f); }
            CHECK(buttons == 3 && !plan.overflow);
        }
    }
    MpMsgText::setLanguage(MpMsgText::LANG_EN);
    // Star Race -> Play with Anyone -> a room found -> START: no Which File? (a race has no shared progress), the room starts
    fresh();
    toGather(true); gatherHeads(1, 1); ok();
    session(true, 0, MP_UI_LOBBY); frames(4);
    press(7); frames(2);
    CHECK(S().screen() != MpMenu::S_SAVE && fake::starts == 1);
}

// lobby: a full game or a console of another version nearby is no longer invisible; a lone first-timer is told how to go on; the
// Continue row says why it went; the Friends screens are in the word table in five languages
void testHeardAndHints() {
    // (8) heard but never offered: the gathering line says so
    fresh();
    toGather(); gatherHeads(1, 1);
    mp_gather_fake_set_hidden(1, 0); frames(3);
    CHECK(lineNow() == "Your friend's game is full.");
    mp_gather_fake_set_hidden(0, 1); frames(3);
    CHECK(lineNow() == "A nearby console has a different\nversion of the game.");
    mp_gather_fake_set_hidden(0, 0); frames(3);
    CHECK(lineNow() == "Waiting for friends...");
    mp_gather_fake_set_hidden(1, 0);
    gatherHeads(2, 1); frames(3);   // (a friend arrived: the line is about starting, not the full game)
    CHECK(lineNow().find("to start!") != std::string::npos);
    // (14) nobody found: the second line tells how to play alone or invite, two lines that fit the box
    fresh();
    toGather(); gatherHeads(1, 1); frames(20 * 60 + 2);
    CHECK(lineNow() == "No one was found nearby.\nInvite a friend, or go back to play alone.");
    for (int l = 0; l < MpMsgText::LANG_COUNT; l++) {
        MpMsgText::setLanguage(l);
        const std::string ln = lineNow();
        int seg = 0, worst = 0, lines = 1;
        for (char c : ln) { if (c == '\n') { ++lines; seg = 0; } else { ++seg; if (seg > worst) worst = seg; } }
        CHECK(lines == 2 && worst <= 44);
    }
    MpMsgText::setLanguage(MpMsgText::LANG_EN);
    // (19) the Continue row goes after 2 minutes: a kind line says so
    fresh();
    dropOut(); openMain();
    CHECK(MpOverlay::toastCount() == 0);
    bool row = false;
    for (int i = 0; i < S().itemCount(); i++) row = row || rowLabel(i).find("Continue") != std::string::npos;
    CHECK(row);
    frames(2 * 60 * 60 + 30);
    row = false;
    for (int i = 0; i < S().itemCount(); i++) row = row || rowLabel(i).find("Continue") != std::string::npos;
    bool said = false;
    for (int i = 0; i < MpOverlay::toastCount(); i++) said = said || U8(MpOverlay::toasts()[i].text) == "Your last game has closed.";
    CHECK(!row && said);
    // (11) the Friends words: nothing is English in the other four languages
    {
        static const MpText ids[] = {MP_T_FRIENDS, MP_T_MY_FRIEND_CODE, MP_T_REMOVE_FRIEND, MP_T_DESC_FRIEND_PAGE};
        MpMsgText::setLanguage(MpMsgText::LANG_EN);
        for (MpText id : ids) {
            char16_t en[200], t[200];
            MpMsgText::format(en, 200, id, MpMsgText::Args());
            for (int l = 1; l < MpMsgText::LANG_COUNT; l++) {
                MpMsgText::setLanguage(l);
                MpMsgText::format(t, 200, id, MpMsgText::Args());
                CHECK(U8(t) != U8(en) || id == MP_T_FRIENDS);
                MpMsgText::setLanguage(MpMsgText::LANG_EN);
            }
        }
    }
}
