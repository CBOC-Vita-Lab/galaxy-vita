// The menus' host tests: the gather family (flow split of ui_menu_test.cpp; one door: the gathering screen on core2's fake).
// Shared helpers: common.hpp.
#include "common.hpp"
#include "port/mp_invite.h"

namespace {
MpPanel::Model& G() { static MpPanel::Model m; S().buildModel(&m); return m; }
std::string lineNow() { return U8(G().line); }
std::string headName(int i) { MpPanel::Model& m = G(); return i < m.headCount ? U8(m.rows[i].label) : "<none>"; }
int gatherState() { MpGatherStatus st; mp_gather_status(&st); return st.state; }
bool sawWindow(const char* id) { for (auto& i : io.ids) if (i == id) return true; return false; }
bool StillTryingUp() {
    char16_t want[96];
    MpMsgText::format(want, 96, MP_T_RECONNECT_LONG, MpMsgText::Args());
    for (int i = 0; i < MpOverlay::toastCount(); i++) if (std::u16string(MpOverlay::toasts()[i].text) == want) return true;
    return false;
}
bool guideHasPic(int pic) { MpPanel::Model& m = G(); for (int i = 0; i < m.buttonCount; i++) if (m.buttons[i].pic == pic) return true; return false; }
}  // namespace

void testErrors() {
    // every MpUiError while Play with Anyone searches: one window, then a usable screen (the Multiplayer menu), the port back to IDLE
    for (int err = MP_ERR_NONE; err <= MP_ERR_INTERNAL; err++) {
        fresh();
        io.answer = MpMenu::DlgState::No;   // (conntest: a connection error is followed by "Run a Connection Test?": No here)
        toQuick();   // Play with Anyone: busy
        CHECK(S().screen() == MpMenu::S_BUSY);
        fake::status.phase = MP_UI_FAILED;
        fake::status.error = static_cast<MpUiError>(err);
        frames(12);
        CHECK(S().screen() == MpMenu::S_GATHER);   // (uxfix #3: Play with Anyone started from the gathering screen: it looks for friends again)
        CHECK(io.last.rfind("MP_ERR_", 0) == 0 || io.last == "MP_CT_OFFER");
        {
            char want[48];
            snprintf(want, sizeof want, "ui: error shown %d (", err == MP_ERR_NONE ? MP_ERR_INTERNAL : err);
            bool shown = false;
            for (auto& l : fake::log) shown = shown || l.rfind(want, 0) == 0;
            CHECK(shown);   // (fix2: every MpUiError that reaches the screen is logged for the harness)
        }
        CHECK(fake::leaves >= 1);
        frames(3);
        CHECK(!S().dialogActive());
        back();   // usable: Back leaves the gathering, then closes the menu
        CHECK(S().screen() == MpMenu::S_MAIN);
        back();
        CHECK(!S().isOpen());
    }
    // MP_ERR_CLOCK: the plain line about the date and time
    CHECK(MpMsgText::errorText(MP_ERR_CLOCK) == MP_T_ERR_CLOCK && std::string(MpMsgText::dialogName(MP_T_ERR_CLOCK)) == "MP_ERR_CLOCK");
    // a wait that never ends times out (45 s) and leaves
    fresh();
    io.answer = MpMenu::DlgState::No;
    toQuick();
    frames(46 * 60);
    CHECK(S().screen() == MpMenu::S_GATHER);
    CHECK(io.last == "MP_CT_OFFER" && logHas("ui: window MP_ERR_TIMEOUT"));
    CHECK(fake::leaves >= 1);
    io.answer = MpMenu::DlgState::Yes;
    // the gathering cannot even start (no radio at all): the core's reason, the Multiplayer menu stays
    fresh();
    mp_gather_fake_set_radio(false, false);
    fake::status.error = MP_ERR_NO_WIFI;
    openMain(); ok(); frames(2);
    // (the fake's start refuses only through FAILED: the screen says why and goes)
    mp_gather_fake_add_head(1, "Player", 0);
    frames(2);
    CHECK(S().screen() == MpMenu::S_GATHER || S().screen() == MpMenu::S_MAIN);
}

// mpwifi44: the owner's console said "Couldn't connect to Wi-Fi. Check your Wi-Fi settings." for Play Together although it was on Wi-Fi: every
// reason a gathering could not start read as that sentence. Now the gathering driver's own reason decides (port/mp_gather.h
// mp_gather_last_error), and the log names it.
void testGatherStartReasons() {
    // Wi-Fi really is off: the Wi-Fi sentence, the Multiplayer menu stays, the log names the reason
    fresh();
    mp_gather_fake_fail_start(MP_ERR_NO_WIFI, "link-wifi-down");
    openMain(); ok(); frames(2);
    CHECK(sawWindow("MP_ERR_NO_WIFI") && S().screen() == MpMenu::S_MAIN && gatherState() == MP_GATHER_OFF);
    CHECK(Logged("ui: gathering failed (start): reason=link-wifi-down gather_error=1 core_error=0 shown=1"));
    // the link could not be opened for another reason: "Something went wrong", never the Wi-Fi sentence
    fresh();
    mp_gather_fake_fail_start(MP_ERR_INTERNAL, "link-open-failed");
    openMain(); ok(); frames(2);
    CHECK(sawWindow("MP_ERR_INTERNAL") && !sawWindow("MP_ERR_NO_WIFI") && S().screen() == MpMenu::S_MAIN);
    CHECK(Logged("ui: gathering failed (start): reason=link-open-failed gather_error=14 core_error=0 shown=14"));
    // (the driver's reason wins over a stale error the core still shows from something earlier)
    fresh();
    fake::status.error = MP_ERR_NO_WIFI;
    mp_gather_fake_fail_start(MP_ERR_INTERNAL, "core-busy");
    openMain(); ok(); frames(2);
    CHECK(sawWindow("MP_ERR_INTERNAL") && !sawWindow("MP_ERR_NO_WIFI"));
    // and the failure does not stay: the same button works the next time
    io.ids.clear();
    frames(3);
    mp_gather_fake_fail_start(0, "");
    fake::status.error = MP_ERR_NONE;
    if (S().dialogActive()) { ok(); frames(3); }
    ok(); frames(2);
    CHECK(S().screen() == MpMenu::S_GATHER && gatherState() == MP_GATHER_LISTENING && !sawWindow("MP_ERR_NO_WIFI") && !sawWindow("MP_ERR_INTERNAL"));
    // a gathering that fails later: its reason, not "no Wi-Fi" by default
    fresh();
    toGather(); gatherHeads(1, 1);
    mp_gather_fake_fail(MP_ERR_INTERNAL, "host-failed");
    frames(3);
    CHECK(sawWindow("MP_ERR_INTERNAL") && !sawWindow("MP_ERR_NO_WIFI") && S().screen() == MpMenu::S_MAIN && gatherState() == MP_GATHER_OFF);
    CHECK(Logged("ui: gathering failed (gathering): reason=host-failed gather_error=14 core_error=0 shown=14"));
    fresh();
    toGather(); gatherHeads(1, 1);
    mp_gather_fake_fail(MP_ERR_NO_WIFI, "link-wifi-down");
    frames(3);
    CHECK(sawWindow("MP_ERR_NO_WIFI") && S().screen() == MpMenu::S_MAIN);
    // no reason from the driver (an older one): what was shown before: the core's error, else the Wi-Fi sentence
    fresh();
    toGather(); gatherHeads(1, 1);
    mp_gather_fake_fail(0, "");
    fake::status.error = MP_ERR_SERVICE_DOWN;
    frames(3);
    CHECK(sawWindow("MP_ERR_SERVICE_DOWN"));
    // the gathering without a Same Wi-Fi link (the driver's answer when the console is connected but the link cannot be opened): this console alone,
    // the leader: the far-away doors are there (Invite from Far Away; Play with Anyone, whose service has the relay), no error window
    fresh();
    fake::codeAvailable = true;   // (the driver's answer without a link: the code can be asked for)
    toGather(); gatherHeads(1, 1);
    frames(3);
    {
        bool invite = false, anyone = false;
        for (int i = 0; i < S().itemCount(); i++) {
            invite = invite || rowLabel(i) == "Invite from Far Away";
            anyone = anyone || rowLabel(i) == "Play with Anyone";
        }
        CHECK(invite && anyone && !sawWindow("MP_ERR_NO_WIFI") && S().screen() == MpMenu::S_GATHER);
    }
}

void testReconnect() {
    fresh();
    session(false, 1, MP_UI_PLAYING);
    frames(3);
    fake::log.clear();
    fake::active = false; fake::status.phase = MP_UI_RECONNECTING; fake::status.error = MP_ERR_NO_WIFI;
    frames(30 * 60);   // (the core tries for up to 30 s)
    CHECK(Logged("ui: reconnecting"));
    CHECK(!LoggedPart("ui: session ended"));
    CHECK(!LoggedPart("ui: window"));
    CHECK(!LoggedPart(" left.") && !LoggedPart("host is now"));
    CHECK(!S().dialogActive() && fake::leaveChoice == 0);
    CHECK(MpOverlay::toastCount() == 1 && (ReconnectToastUp() || StillTryingUp()));   // (uxfix #2: after ten seconds it reads "Still trying...")
    // fix3: Start meanwhile opens the session menu (Leave only: nobody to greet), never the game's pause menu (MpUi::reconnecting)
    CHECK(S().reconnecting());
    S().openSession(); frames(2);
    CHECK(S().isOpen() && S().screen() == MpMenu::S_SESSION && S().itemCount() == 1 && Logged("ui: session menu opened"));
    back(); frames(2);
    CHECK(!S().isOpen());
    // the rejoin works: back in the session, the toast goes, nothing was asked or discarded
    fake::active = true; fake::status.phase = MP_UI_PLAYING; fake::status.error = MP_ERR_NONE;
    frames(3);
    CHECK(Logged("ui: reconnected"));
    CHECK(!LoggedPart("ui: session ended") && !LoggedPart("ui: window") && !LoggedPart(" joined."));
    CHECK(MpOverlay::toastCount() == 0 && fake::leaveChoice == 0 && fake::leaves == 0);
    // a real departure during the outage is still told once back
    fake::active = false; fake::status.phase = MP_UI_RECONNECTING;
    frames(60);
    fake::players[2].present = false;
    fake::active = true; fake::status.phase = MP_UI_PLAYING;
    frames(3);
    CHECK(LoggedPart("Ros left."));
    // the core gives up: now the session is over (the error, then "Save your progress?")
    fake::log.clear();
    fake::active = false; fake::status.phase = MP_UI_RECONNECTING; fake::status.error = MP_ERR_NO_WIFI;
    frames(60);
    CHECK(!LoggedPart("ui: session ended"));
    fake::status.phase = MP_UI_IDLE;
    frames(6);
    CHECK(Logged("ui: reconnect over"));
    CHECK(LoggedPart("ui: session ended by=them err=1 played=1"));
    CHECK(Logged("ui: window MP_ERR_DROPPED") && Logged("ui: window MP_SAVE_ASK (yes/no)"));   // (uxfix #2: the connection was lost, not "Wi-Fi setup")
    CHECK(!ReconnectToastUp());
    // (an IDLE right away, without RECONNECTING, ends the session as before)
    fresh();
    session(false, 1, MP_UI_PLAYING);
    frames(3);
    fake::active = false; fake::status.phase = MP_UI_IDLE; fake::status.error = MP_ERR_HOST_LEFT;
    frames(6);
    CHECK(Logged("ui: window MP_ERR_HOST_LEFT") && !Logged("ui: reconnecting"));
}

// one door: Play Together / Star Race open the gathering screen at once; the leader starts with START; a returning leader goes on with
// the co-op file played last; the falls question is asked once
void testQuickMatchSave() {
    // first time ever: Play Together (1 press) -> the gathering screen; a friend arrives; START (1) -> the falls question (1) ->
    // Which File? (1) -> started. With the character select of the very first play: 5 presses and the falls question
    fresh();
    fake::flow.fallsAsked = 0;
    fake::flow.lastCoop = 0;
    openMain();
    CHECK(S().screen() == MpMenu::S_MAIN && rowLabel(0) == "Play Together" && rowLabel(1) == "Star Race" && rowLabel(2) == "Enter a Friend's Code");
    ok(); frames(2);
    CHECK(S().screen() == MpMenu::S_GATHER && Logged("ui: gathering mode=1"));
    CHECK(lineNow() == "Waiting for friends..." && G().kind == MpPanel::KIND_BUTTONS && G().look == MpPanel::LOOK_GAME);
    gatherHeads(1, 1);
    CHECK(G().kind == MpPanel::KIND_HEADS && G().headCount == 1 && headName(0) == "Player" && (G().rows[0].marks & MpPanel::MARK_CROWN));
    CHECK(lineNow() == "Waiting for friends..." && !guideHasPic(MpMsgText::PIC_START));   // (alone: no START)
    press(7);
    CHECK(S().screen() == MpMenu::S_GATHER && startsAll() == 0);   // (START alone: the buzzer)
    mp_gather_fake_add_head(2, "Luigi64", 1);
    frames(2);
    CHECK(G().headCount == 2 && headName(1) == "Luigi64" && lineNow().find("to start!") != std::string::npos && guideHasPic(MpMsgText::PIC_START));
    session(true, 0, MP_UI_LOBBY); frames(2);   // (the core's room under the gathering)
    press(7);
    CHECK(S().screen() == MpMenu::S_FALLS && U8(G().line) == "If someone falls,\nhow should they come back?" && S().cursor() == 0);
    CHECK(rowLabel(0) == "In a Bubble" && rowLabel(1) == "Try Again");
    down(); ok();   // Try Again
    CHECK(fake::flow.fallsAsked == 1 && Logged("ui: falls try again"));
    CHECK(S().screen() == MpMenu::S_SAVE);   // (first time: Which File?)
    ok();
    CHECK(startsAll() == 1 && mp_gather_fake_begins() == 1 && fake::settings.downed == MP_DOWNED_INFINITE && fake::settings.maxPlayers == 4 &&
          fake::settings.interaction == 1 && (fake::settings.flags & MP_SET_TEXT_OFF) == 0);
    CHECK(S().screen() == MpMenu::S_GATHER);
    fake::status.phase = MP_UI_PLAYING; frames(3);
    int file = 0;
    CHECK(S().takeLaunch(&file) && file >= 1 && file <= 6);
    // returning (the co-op file 2 played last, the falls answered): Play Together, START: two presses, nothing asked
    fresh();
    fake::flow.lastCoop = 2;
    openMain(); ok(); frames(2);
    gatherHeads(2, 1);
    session(true, 0, MP_UI_LOBBY); frames(2);
    CHECK(guideHasPic(MpMsgText::PIC_SQUARE));   // (Change File)
    press(7);
    CHECK(startsAll() == 1 && S().screen() == MpMenu::S_GATHER && Logged("ui: host save kind=1 coop=2 sp=1 ready"));
    // ...Cross starts too where there is no button to choose
    fresh();
    fake::flow.lastCoop = 2;
    hostLobby();
    CHECK(S().itemCount() == 0);
    ok();
    CHECK(startsAll() == 1);
    // Change File (Square): Which File?, the choice kept, back to the gathering; START goes on with it
    fresh();
    fake::flow.lastCoop = 2;
    hostLobby();
    press(4);
    CHECK(S().screen() == MpMenu::S_SAVE);
    down(); ok();   // (the next row: a new co-op file)
    CHECK(S().screen() == MpMenu::S_GATHER && startsAll() == 0);
    press(7);
    CHECK(startsAll() == 1);
    // a friend (not the leader): two presses (Play Together, nothing else); the line names who starts; no START, no Change File
    fresh();
    fake::flow.lastCoop = 2;
    guestLobby();
    CHECK(lineNow() == "Waiting for Luigi64 to start..." && !guideHasPic(MpMsgText::PIC_START) && !guideHasPic(MpMsgText::PIC_SQUARE));
    press(7); ok();
    CHECK(startsAll() == 0);
    fake::status.phase = MP_UI_PLAYING; frames(3);
    CHECK(S().takeLaunch(&file));
    // merging: the heads stay, no START (core2: only canStart shows it)
    fresh();
    hostLobby();
    mp_gather_fake_merge_begin(); frames(2);
    CHECK(G().headCount == 2 && lineNow() == "Waiting for friends..." && !guideHasPic(MpMsgText::PIC_START));
    press(7);
    CHECK(startsAll() == 0);
    mp_gather_fake_add_head(3, "Ros", 2);
    mp_gather_fake_set_arriving(3, true);
    mp_gather_fake_merge_end(3);   // (Ros's room was the bigger one: Ros leads now)
    frames(2);
    CHECK(G().headCount == 3 && lineNow() == "Waiting for Ros to start..." && !guideHasPic(MpMsgText::PIC_START));
    // Play with Anyone: only where the online service is, from the gathering screen alone (no extra screen); Quick Match underneath
    fresh();
    fake::service = false;
    toGather(); gatherHeads(1, 1);
    CHECK(S().itemCount() == 0);
    fresh();
    toGather(); gatherHeads(1, 1);
    CHECK(S().itemCount() == 1 && rowLabel(0) == "Play with Anyone");
    ok();
    CHECK(S().screen() == MpMenu::S_BUSY && fake::roomPublic);
    // Star Race alone: Play with Anyone, Leaderboards (no service: none)
    fresh();
    toGather(true); gatherHeads(1, 1);
    CHECK(S().itemCount() == 2 && rowLabel(1) == "Leaderboards");
}

// one door: the heads keep their slots by the player's id (core2: never by the list's order); a game playing nearby is offered
void testRoomIdentity() {
    fresh();
    toGather(); gatherHeads(3, 1);
    MpPanel::Model& m = G();
    CHECK(m.headCount == 3 && m.rows[0].owner == 0 && m.rows[1].owner == 1 && m.rows[2].owner == 2);
    mp_gather_fake_remove_head(2);   // Luigi64 goes: Ros keeps slot 2
    frames(2);
    CHECK(G().headCount == 2 && headName(1) == "Ros" && G().rows[1].owner == 2);
    mp_gather_fake_add_head(4, "Star", 3);   // a new one takes the free slot
    frames(2);
    CHECK(G().headCount == 3 && G().rows[2].owner == 1);
    // a game already playing nearby: "Play with Ros" while alone
    fresh();
    toGather(); gatherHeads(1, 1);
    mp_gather_fake_add_playing(77, "Ros", MP_MODE_TOGETHER, 2);
    frames(2);
    CHECK(rowLabel(0) == "Play with Ros");
    ok();
    CHECK(mp_gather_fake_joined_room() == 77);
    // nobody heard for 20 s: the friendly line, and the other radio offered
    fresh();
    toGather(); gatherHeads(1, 1);
    frames(20 * 60 + 2);
    CHECK(lineNow() == "No one was found nearby.\nInvite a friend, or go back to play alone.");
    bool sw = false;
    for (int i = 0; i < S().itemCount(); i++) sw = sw || rowLabel(i) == "Look for Friends Nearby";
    CHECK(sw);
    // Back alone: no question, the gathering stops; with others: one question first
    fresh();
    toGather(); gatherHeads(1, 1);
    back();
    CHECK(S().screen() == MpMenu::S_MAIN && Logged("ui: left the gathering"));
    fresh();
    hostLobby();
    io.answer = MpMenu::DlgState::No;
    back(); frames(2);
    CHECK(S().screen() == MpMenu::S_GATHER && Logged("ui: window MP_LEAVE_GUEST (yes/no)"));
    io.answer = MpMenu::DlgState::Yes;
    back(); frames(2);
    CHECK(S().screen() == MpMenu::S_MAIN);
}

void testJoinErrorsShown() {
    // the session ends with an error while another window is up: that window first, then the error (never dropped)
    fresh();
    session(true, 0, MP_UI_PLAYING);
    frames(2);
    S().openSession(); frames(2);
    down(2);   // (Warp, Observatory, Leave)
    io.answer = MpMenu::DlgState::No;
    MpMenu::Input in;
    in.ok = true;
    io.open = false;
    S().update(in);   // "Leave the game?" is up (answered next frame)
    CHECK(S().dialogActive());
    fake::active = false; fake::status.phase = MP_UI_IDLE; fake::status.error = MP_ERR_KICKED;
    frames(4);
    CHECK(Logged("ui: window MP_ERR_KICKED") && LoggedPart("ui: error shown 9 (MP_ERR_KICKED)"));
    io.answer = MpMenu::DlgState::Yes;
}



void testFalls() {
    // the falls question is asked once (the first Start of Play Together), remembered in mp.cfg, bubbles first; Star Race never asks
    fresh();
    fake::flow.fallsAsked = 0;
    toGather(true); gatherHeads(2, 1); session(true, 0, MP_UI_LOBBY); frames(2);
    press(7);
    CHECK(S().screen() != MpMenu::S_FALLS);
    fresh();
    fake::flow.fallsAsked = 0;
    hostLobby();
    press(7);
    CHECK(S().screen() == MpMenu::S_FALLS && S().cursor() == 0);
    back();   // (Back: not asked; the gathering again)
    CHECK(S().screen() == MpMenu::S_GATHER && fake::flow.fallsAsked == 0);
    // the host's session menu has no Falls row any more (session family: the rule is asked once, by the player who opened the game)
    fresh();
    session(true, 0, MP_UI_PLAYING);
    frames(2);
    S().openSession(); frames(2);
    CHECK(!rowShows(MP_T_FALLS, MP_T_FALLS_BUBBLES) && !rowShows(MP_T_FALLS, MP_T_FALLS_LIVES) && fake::settingsSent == 0);
    // a guest has no Falls row (and could not send SETTINGS)
    fresh();
    session(false, 1, MP_UI_PLAYING);
    frames(2);
    S().openSession(); frames(2);
    CHECK(!rowShows(MP_T_FALLS, MP_T_FALLS_BUBBLES) && !rowShows(MP_T_FALLS, MP_T_FALLS_LIVES));
    // the words: every language has them, the toasts fit the toast buffer
    for (int lang = 0; lang < MpMsgText::LANG_COUNT; lang++) {
        MpMsgText::setLanguage(lang);
        for (MpText id : {MP_T_FALLS, MP_T_FALLS_BUBBLES, MP_T_FALLS_LIVES, MP_T_BUBBLED, MP_T_POPPED, MP_T_ALL_CAUGHT,
                          MP_T_OUT_OF_LIVES}) {
            CHECK(MpMsgText::raw(id)[0] != 0);
            MpMsgText::Args a;
            a.str = "Toadsworth12345";
            a.str2 = "Luigi64Luigi64";
            char16_t t[96];
            CHECK(MpMsgText::format(t, 96, id, a) < 95);
        }
    }
    MpMsgText::setLanguage(MpMsgText::LANG_EN);
    // the popped toast names both players
    fresh();
    fake::active = true;
    MpOverlay::toast(MP_T_POPPED, 0, 1);
    bool popped = false;
    for (auto& l : fake::log) popped = popped || l == "ui: toast \"Player popped Luigi64's bubble!\"";
    CHECK(popped);
}

// one door: far away. The leader's "Invite from Far Away" exists only when the core can give a code (core2 stage 3); a friend types the
// code in "Enter a Friend's Code"; a code that does not decode: one plain line; a code whose game never answers: "can't reach you from here"
void testByAddress() {
    fresh();
    hostLobby();
    for (int i = 0; i < S().itemCount(); i++) CHECK(rowLabel(i) != "Invite from Far Away");   // (no code possible: absent, not dead)
    // the core can give a code: the button; the first press asks for it, the line says so, the window shows it once it is there
    fresh();
    fake::codeAvailable = true;
    hostLobby();
    CHECK(S().itemCount() == 1 && rowLabel(0) == "Invite from Far Away");
    ok(); frames(2);
    // (owner: the code is the biggest thing on the gathering screen itself, no window; the button goes once it is there)
    CHECK(Logged("ui: far-away code shown") && std::string(mp_gather_fake_joined_code()).empty() && S().itemCount() == 0);
    {
        static MpPanel::Model gm;
        S().buildModel(&gm);
        CHECK(gm.big[0] != 0 && gm.kind == MpPanel::KIND_HEADS);
        // 13 Nov (A2): the code is centred between the caption and the prompt band's top, one row and three rows
        {
            MpPanel::FontMetrics fm[MpPanel::FONT_COUNT];
            for (auto& f : fm) f.height = 33.0f, f.baseline = 26.0f, f.advance = Adv, f.has = HasG;
            static MpPanel::Model cm;
            static MpPanel::Plan cplan;
            static MpPanel::Scene cscene;
            const char* codes[2] = {"SUPZX", "KCBC4-SVVPU-JMM6S"};
            for (int k = 0; k < 2; k++) {
                cm = gm;
                for (int i = 0; i < 24; i++) cm.big[i] = 0;
                for (int i = 0; codes[k][i]; i++) cm.big[i] = static_cast<char16_t>(codes[k][i]);
                if (!cm.line[0]) cm.line[0] = u'P', cm.line[1] = u'r', cm.line[2] = 0;
                MpPanel::Screen scr;
                MpPanel::beginScene(&cscene);
                MpPanel::begin(&cplan);
                MpPanel::compose(cm, scr, fm, 0.25f, &cscene, &cplan);
                // the code rows are the runs in the code's colour below the caption: take the run centres of the biggest runs
                float top = 1e9f, bot = -1e9f, capY = -1e9f;
                for (int i = 0; i < cplan.runCount; i++) {
                    const MpPanel::TextRun& r = cplan.runs[i];
                    const char16_t c0 = cplan.text[r.start];
                    const float size = 33.0f * r.scaleY, cy = r.y + 17.4f * r.scaleY;
                    if (c0 == u'T' && size < 30.0f && size > 15.0f) capY = cy;   // "Tell ..." the caption (19 units)
                    if (size > 40.0f) top = top < cy - size * 0.5f ? top : cy - size * 0.5f, bot = bot > cy + size * 0.5f ? bot : cy + size * 0.5f;
                }
                const float bandTop = MpPanel::layoutToSquareY(scr, MpPanel::kLookBottom - 7.0f);
                CHECK(top < 1e8f && bot > -1e8f);
                if (top < 1e8f) {
                    const float mid = 0.5f * (top + bot), want = 0.5f * ((capY > -1e8f ? capY + 9.0f : top) + bandTop);
                    fprintf(stderr, "  code %d rows: centre %.1f, wanted %.1f (caption %.1f, band top %.1f)\n", k, mid, want, capY, bandTop);
                    CHECK(k == 0 ? std::fabs(mid - want) < 9.0f : bot <= bandTop && top > capY);
                }
            }
        }
    }
    {
        MpGatherStatus st;
        mp_gather_status(&st);
        CHECK(st.code[0] != 0 && st.conn == MP_CONN_DIRECT);
    }
    // Enter a Friend's Code: the keyboard, a bad code
    fresh();
    openMain(); down(2);
    snprintf(fake::kbText, sizeof fake::kbText, "K7QMD-4DXTA");
    fake::kbResult = 1;
    ok(); frames(2);
    CHECK(io.last == "MP_CODE_BAD" && S().screen() == MpMenu::S_MAIN);
    // a good code (spaces, dashes, case forgiven by the decoder): the gathering screen, waiting for the friend's game
    fresh();
    openMain(); down(2);
    fake::kbOnce = true;   // uxfix: a failure reopens the keyboard; it waits for the player
    validCode();
    for (char* c = fake::kbText; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = static_cast<char>(*c - 'A' + 'a');
    ok(); frames(2);
    CHECK(S().screen() == MpMenu::S_GATHER && std::string(mp_gather_fake_joined_code()).size() > 0);
    // nobody answers in 25 s: the friendly window, back to the menu; never an address, a port or a router
    frames(26 * 60);
    CHECK(io.last == "MP_CANT_REACH" && S().screen() == MpMenu::S_MAIN);
    {
        const char16_t* t = MpMsgText::findDialog("MP_CANT_REACH");
        const std::string w = t ? U8(t) : "";
        CHECK(w.find("address") == std::string::npos && w.find("port") == std::string::npos && w.find("router") == std::string::npos);
    }
}

// uxfix #1: a friend's code. A bad code: the keyboard opens again with the text still in it; no Wi-Fi: said before anything is typed; the
// joiner reads "Reaching your friend..." (not "Waiting for friends..."); the failure sentence is about the friend's game and the code.
void testCodeJoinFixes() {
    // a bad code: window, then the keyboard again with the same text
    fresh();
    openMain(); down(2);
    snprintf(fake::kbText, sizeof fake::kbText, "K7QMD-4DXTA");
    fake::kbResult = 1;
    fake::kbOnce = true;
    fake::kbOpens = 0;
    ok(); frames(2);
    CHECK(io.last == "MP_CODE_BAD");
    frames(3);   // (the player is typing again)
    CHECK(fake::kbOpens == 2 && std::string(fake::kbInitial) == "K7QMD-4DXTA" && fake::kbMax == 24);
    CHECK(S().screen() == MpMenu::S_MAIN);
    {   // the keyboard cancelled: the Multiplayer menu, usable
        fake::kbResult = -1;
        frames(3);
        CHECK(S().screen() == MpMenu::S_MAIN && !S().dialogActive());
        fake::kbResult = 0;
    }
    // the sentence blames neither the player nor an address
    {
        const std::string w = U8(MpMsgText::findDialog("MP_CODE_BAD") + 3);
        CHECK(w.find("didn't work") == std::string::npos && w.find("check it and try again") != std::string::npos);
    }
    // a fresh code after a good one starts empty
    fresh();
    openMain(); down(2);
    fake::kbOpens = 0;
    fake::kbInitial[0] = 'x';
    fake::kbResult = 0;
    ok(); frames(2);
    CHECK(fake::kbOpens == 1 && fake::kbInitial[0] == 0);
    // no Wi-Fi: the Wi-Fi sentence at once, no keyboard
    fresh();
    fake::wifi = false;
    openMain(); down(2);
    fake::kbOpens = 0;
    ok(); frames(2);
    CHECK(fake::kbOpens == 0 && io.last == "MP_ERR_NO_WIFI" && S().screen() == MpMenu::S_MAIN);
    // Wi-Fi lost while typing: the same sentence, the typed text is kept for the next try
    fresh();
    openMain(); down(2);
    fake::kbOnce = true;
    ok(); frames(2);
    fake::wifi = false;
    validCode();
    frames(2);
    CHECK(io.last == "MP_ERR_NO_WIFI" && std::string(mp_gather_fake_joined_code()).empty());
    fake::wifi = true;
    fake::kbResult = 0;
    fake::kbOpens = 0;
    frames(3);
    ok(); frames(2);
    CHECK(fake::kbOpens == 1 && std::string(fake::kbInitial) == std::string(fake::kbText));
    // a good code: the joiner is REACHING a friend; nobody answers: the corrected sentence, then the keyboard again with the code
    fresh();
    openMain(); down(2);
    fake::kbOnce = true;
    validCode();
    ok(); frames(2);
    CHECK(S().screen() == MpMenu::S_GATHER && lineNow() == "Reaching your friend...");
    {   // (a leader waiting for friends still reads "Waiting for friends..."; a game with others: the leader's name)
        fresh();
        toGather();
        CHECK(lineNow() == "Waiting for friends...");
    }
    fresh();
    openMain(); down(2);
    fake::kbOnce = true;
    validCode();
    ok(); frames(2);
    const std::string typed = fake::kbText;
    fake::kbResult = 0;
    fake::kbOpens = 0;
    frames(26 * 60);
    CHECK(io.last == "MP_CANT_REACH" && S().screen() == MpMenu::S_MAIN);
    frames(3);
    CHECK(fake::kbOpens == 1 && std::string(fake::kbInitial) == typed);
    {
        const std::string w = U8(MpMsgText::findDialog("MP_CANT_REACH") + 3);
        CHECK(w.find("reach your friend's game") != std::string::npos && w.find("Check the code") != std::string::npos);
        CHECK(w.find("can't reach you") == std::string::npos && w.find("start the") == std::string::npos);   // (the old sentence pointed backwards)
    }
}

// code5: the 5-character code. The host's button asks the service (the fake says how it answers); a friend types five characters, case and
// separators forgiven; every way the lookup can end says its own plain line and brings the keyboard back with the typed text still in it.
void testInviteCode() {
    // the host: the service is up: ONE short code, the short line; down or over budget: the long code and the line that says why; no service
    // in this build: the long code and the plain line
    for (int mode = 0; mode <= 2; mode++) {
        fresh();
        fake::codeAvailable = true;
        toGather(); gatherHeads(1, 1); session(true, 0, MP_UI_LOBBY); frames(3);   // (alone: the code, not "Press START")
        mp_gather_fake_set_service(mode);
        CHECK(rowLabel(0) == "Invite from Far Away");
        ok(); frames(2);
        MpGatherStatus st;
        mp_gather_status(&st);
        static MpPanel::Model gm;
        S().buildModel(&gm);
        CHECK(U8(gm.big) == std::string(st.code) && st.conn == MP_CONN_DIRECT);
        CHECK(st.codeLong == (mode == 1));
        fprintf(stderr, "  host mode %d: code %s line [%s]\n", mode, st.code, lineNow().c_str());
        if (mode == 0) CHECK(std::string(st.code) == "KCBC4" && lineNow() == "Tell your friend this code.");
        else CHECK(std::string(st.code).size() >= 15);
        if (mode == 1) CHECK(lineNow() == "Online play is busy, so this code is longer.\nShare it only with friends.");
        if (mode == 2) CHECK(lineNow() == "Tell your friend this code.");
    }
    // the friend: typed with a small letter and a dash: the lookup runs (the gathering screen, "Reaching your friend...")
    fresh();
    openMain(); down(2);
    fake::kbOnce = true;
    snprintf(fake::kbText, sizeof fake::kbText, "kc-bc4");
    fake::kbResult = 1;
    ok(); frames(2);
    CHECK(S().screen() == MpMenu::S_GATHER && std::string(mp_gather_fake_joined_code()) == "kc-bc4" && lineNow() == "Reaching your friend...");
    // every end of the lookup: its own window; the typed text comes back in the keyboard
    struct End { int err; const char* window; };
    const End ends[] = {{MP_INV_WRONG, "MP_CODE_BAD"}, {MP_INV_SEALED_BAD, "MP_CODE_BAD"}, {MP_INV_EXPIRED, "MP_CODE_EXPIRED"}, {MP_INV_BUSY, nullptr},
                        {MP_INV_OFF, nullptr}, {MP_INV_FULL, nullptr}, {MP_INV_VERSION, nullptr}, {MP_INV_NO_WIFI, nullptr}};
    for (const End& e : ends) {
        fresh();
        openMain(); down(2);
        fake::kbOnce = true;
        snprintf(fake::kbText, sizeof fake::kbText, "kc-bc4");
        fake::kbResult = 1;
        ok(); frames(2);
        CHECK(S().screen() == MpMenu::S_GATHER);
        fake::kbResult = 0;
        fake::kbOpens = 0;
        io.last.clear();
        mp_gather_fake_invite_fail(e.err);
        frames(3);
        fprintf(stderr, "  invite end %d: window '%s' screen %s\n", e.err, io.last.c_str(), MpMenu::screenName(S().screen()));
        if (e.window) CHECK(io.last == e.window);
        CHECK(S().screen() == MpMenu::S_MAIN || S().dialogActive() || !io.last.empty());
        if (e.err == MP_INV_WRONG || e.err == MP_INV_EXPIRED || e.err == MP_INV_BUSY || e.err == MP_INV_OFF) {
            frames(3);
            CHECK(fake::kbOpens == 1 && std::string(fake::kbInitial) == "kc-bc4");   // (the typed text is kept)
        }
    }
    // a look-alike (an O for a 0 is not in the code's letters): refused here, no lookup, the plain line, the text kept
    fresh();
    openMain(); down(2);
    fake::kbOnce = true;
    snprintf(fake::kbText, sizeof fake::kbText, "KO1C4");
    fake::kbResult = 1;
    ok(); frames(2);
    CHECK(io.last == "MP_CODE_BAD" && std::string(mp_gather_fake_joined_code()).empty() && S().screen() == MpMenu::S_MAIN);
    // the words exist in the five languages and fit a window line (44)
    for (const char* l : {"UsEnglish", "UsFrench", "EuSpanish", "EuGerman", "EuItalian"}) {
        MpMsgText::setLanguage(MpMsgText::languageIndex(l));
        const char16_t* t = MpMsgText::findDialog("MP_CODE_EXPIRED");
        CHECK(t != nullptr);
        int line = 0, longest = 0;
        for (const char16_t* p = t ? t + 3 : u""; *p; ++p) { if (*p == u'\n') line = 0; else if (++line > longest) longest = line; }
        CHECK(longest > 0 && longest <= 44);
        const char16_t* lg = MpMsgText::raw(MP_T_G_CODE_LONG);
        int n = 0, ln = 0, nl = 0;   // two lines of a window line (46) at most
        for (; lg[n]; n++) { if (lg[n] == u'\n') ln = 0, nl++; else if (++ln > 46) ln = 99; }
        CHECK(n > 0 && ln <= 46 && nl <= 1);
        CHECK(std::u16string(lg).find(u"\n") != std::u16string::npos);
    }
    MpMsgText::setLanguage(MpMsgText::languageIndex("UsEnglish"));
}

// uxfix #2: honest endings. A drop (silent link, Wi-Fi lost, sleep) says the connection was lost and that the way back in is on the
// Multiplayer menu; a left game, a removal and a closed game keep their own sentences; a data mismatch says so plainly; the sticky
// "Reconnecting..." line says it is still trying after ten seconds.
void testHonestEndings() {
    for (MpUiError err : {MP_ERR_NO_WIFI, MP_ERR_TIMEOUT, MP_ERR_NAT}) {
        fresh();
        session(false, 1, MP_UI_PLAYING);
        frames(3);
        fake::active = false, fake::status.phase = MP_UI_IDLE, fake::status.error = err;
        frames(10);
        CHECK(sawWindow("MP_ERR_DROPPED") && !sawWindow("MP_ERR_NO_WIFI") && !sawWindow("MP_ERR_TIMEOUT") && !sawWindow("MP_ERR_NAT"));
        {
            const std::string w = U8(MpMsgText::findDialog("MP_ERR_DROPPED") + 3);
            CHECK(w.find("connection was lost") != std::string::npos && w.find("Multiplayer") != std::string::npos && w.find("two minutes") != std::string::npos);
        }
        // the way back in is really there, and the window does not blame Wi-Fi setup
        fake::status.error = MP_ERR_NONE;
        openMain();
        bool rejoin = false;
        for (int i = 0; i < S().itemCount(); i++) rejoin = rejoin || rowLabel(i).rfind("Continue", 0) == 0;
        CHECK(rejoin);
    }
    // not a drop: the host closed the game, the player was removed, everyone left
    for (MpUiError err : {MP_ERR_HOST_LEFT, MP_ERR_KICKED, MP_ERR_NONE}) {
        fresh();
        session(false, 1, MP_UI_PLAYING);
        frames(3);
        fake::active = false, fake::status.phase = MP_UI_IDLE, fake::status.error = err;
        frames(10);
        CHECK(!io.ids.empty() && !sawWindow("MP_ERR_DROPPED"));
    }
    // without a way back in (no rejoin in this build) the old sentence stays
    // a data mismatch: a plain sentence, never "everyone left"
    fresh();
    session(false, 1, MP_UI_PLAYING);
    frames(3);
    fake::mismatchLeft = true;
    fake::active = false, fake::status.phase = MP_UI_IDLE, fake::status.error = MP_ERR_NONE;
    frames(10);
    CHECK(sawWindow("MP_DATA_MISMATCH") && !sawWindow("MP_ERR_DROPPED"));
    {
        const std::string w = U8(MpMsgText::findDialog("MP_DATA_MISMATCH") + 3);
        CHECK(w.find("didn't match your friend's") != std::string::npos && w.find("Nothing on your console") != std::string::npos);
    }
    CHECK(!fake::mismatchLeft);   // (taken once)
    // the toast version is short and plain too
    MpMsgText::Args a;
    char16_t t[96];
    MpMsgText::format(t, 96, MP_T_HOST_DATA_BAD, a);
    CHECK(U8(t).find("can't be used") == std::string::npos && U8(t).find("match") != std::string::npos);
    // the daily limit does not blame the player; a version mismatch says what to do
    CHECK(U8(MpMsgText::findDialog("MP_ERR_DAILY") + 3).find("reached today's limit") == std::string::npos &&
          U8(MpMsgText::findDialog("MP_ERR_DAILY") + 3).find("busy") != std::string::npos);
    CHECK(U8(MpMsgText::findDialog("MP_ERR_VERSION") + 3).find("update both") != std::string::npos);
    // "Still trying..." after 10 s of reconnecting
    fresh();
    session(false, 1, MP_UI_PLAYING);
    frames(3);
    fake::active = false; fake::status.phase = MP_UI_RECONNECTING; fake::status.error = MP_ERR_NO_WIFI;
    frames(5 * 60);
    CHECK(ReconnectToastUp());
    frames(6 * 60);
    CHECK(!ReconnectToastUp());
    CHECK(StillTryingUp() && MpOverlay::toastCount() == 1);
}

// uxfix #3: Play with Anyone is chosen on the gathering screen. Whatever fails (the sign-in, the search, the daily limit) or is cancelled,
// the player is looking for friends again; a success is not touched.
void testAnyoneDoesNotStrand() {
    // the search fails: the reason, then the gathering again (heads, the Play with Anyone button back)
    fresh();
    io.answer = MpMenu::DlgState::No;   // (the Connection Test offer: No)
    toQuick();
    CHECK(S().screen() == MpMenu::S_BUSY && gatherState() != MP_GATHER_GATHERING);
    fake::status.phase = MP_UI_FAILED;
    fake::status.error = MP_ERR_NO_WIFI;
    frames(12);
    CHECK(sawWindow("MP_ERR_NO_WIFI") && S().screen() == MpMenu::S_GATHER && gatherState() != MP_GATHER_OFF);
    CHECK(Logged("ui: Play with Anyone did not work: gathering again"));
    {
        bool anyone = false;
        gatherHeads(1, 1);   // (the gathering settles; the button is there once it has)
        frames(3);
        for (int i = 0; i < S().itemCount(); i++) anyone = anyone || rowLabel(i) == "Play with Anyone";
        CHECK(anyone);
    }
    // the sign-in fails with the service's own reason (the daily limit): the window, then the gathering
    friendsFresh();
    fake::ensureResult = -static_cast<int>(MP_ERR_SERVICE_DOWN);
    fake::svcCode = "daily_cap";
    toGather();
    gatherHeads(1, 1);
    ok(); frames(4);
    CHECK(sawWindow("MP_ERR_DAILY") && S().screen() == MpMenu::S_GATHER);
    fake::friendsReset();
    // cancelled (Circle) during the search: the gathering again, not the Multiplayer menu
    fresh();
    toQuick();
    CHECK(S().screen() == MpMenu::S_BUSY);
    back(); frames(2);
    CHECK(S().screen() == MpMenu::S_GATHER && gatherState() != MP_GATHER_OFF);
    // a success is not touched: the room opens, no gathering is started behind it
    fresh();
    toQuick();
    session(true, 0, MP_UI_LOBBY);
    frames(4);
    CHECK(!Logged("ui: Play with Anyone did not work: gathering again") && gatherState() == MP_GATHER_OFF);   // (lobby: the room's own gathering screen, the driver stays off)
    // leaving later does not bring a gathering back
    fresh();
    toQuick();
    session(true, 0, MP_UI_LOBBY);
    frames(4);
    fake::active = false, fake::status.phase = MP_UI_IDLE, fake::status.error = MP_ERR_HOST_LEFT;
    frames(6);
    CHECK(S().screen() != MpMenu::S_GATHER);
}

// uxfix #4: the waits are in the file select's look (the game's own background and bottom line, Cancel in the guide), not a window on
// the old pause-menu frame; the screen draws with nothing to press in the middle.
void testWaitLook() {
    struct W { const char* name; void (*go)(); const char* line; };
    const W waits[] = {
        {"Play with Anyone", [] { toQuick(); }, "Looking for a game..."},
        {"rejoin", [] { dropOut(); openMain(); ok(); }, nullptr},
        {"sign-in", [] { fake::ensureWork = 1000; toGather(); gatherHeads(1, 1); ok(); }, "Connecting..."},
    };
    for (const W& w : waits) {
        friendsFresh();
        w.go();
        frames(2);
        CHECK(S().screen() == MpMenu::S_BUSY);
        MpPanel::Model& m = G();
        CHECK(m.look == MpPanel::LOOK_GAME && m.kind == MpPanel::KIND_BUTTONS && m.rowCount == 0);
        CHECK(m.line[0] != 0 && (!w.line || U8(m.line) == w.line));
        CHECK(guideHasPic(MpMsgText::PIC_CIRCLE) && m.buttonCount == 1);
    }
    // the progress shows in the same line
    friendsFresh();
    toQuick();
    fake::status.progress = 40;
    frames(2);
    CHECK(S().screen() == MpMenu::S_BUSY && G().look == MpPanel::LOOK_GAME && U8(G().line).find("40%") != std::string::npos);
    fake::friendsReset();
}

// uxfix #6: a Star Race never asks Which File? (it has no shared progress); Play Together's first game still does.
void testRaceNeverAsksFile() {
    fresh();
    hostLobby(true);   // Star Race, no Team File yet, nothing played before
    press(7);
    CHECK(S().screen() != MpMenu::S_SAVE && startsAll() == 1 && mp_gather_fake_begins() == 1);
    CHECK(!Logged("ui: screen save"));
    // Play Together's first game: the question is still asked
    fresh();
    hostLobby();
    press(7);
    CHECK(S().screen() == MpMenu::S_SAVE && mp_gather_fake_begins() == 0);
}

// uxfix #9: on the gathering screen your own head wears a "You" mark once someone else is there; alone there is none
void testMeMark() {
    const char* want[MpMsgText::LANG_COUNT] = {};
    for (int l = 0; l < MpMsgText::LANG_COUNT; l++) {
        MpMsgText::setLanguage(l);
        fresh();
        toGather();
        gatherHeads(1, 1);
        frames(2);
        MpPanel::Model& a = G();
        int marks = 0;
        for (int i = 0; i < a.rowCount; i++) marks += a.rows[i].value[0] != 0 && (a.rows[i].marks & MpPanel::MARK_YOU) != 0;
        CHECK(marks == 0);
        mp_gather_fake_add_head(2, "Luigi64", 1);
        frames(3);
        MpPanel::Model& m = G();
        marks = 0;
        std::string word;
        for (int i = 0; i < m.headCount; i++) {
            const MpPanel::Row& r = m.rows[i];
            const bool you = (r.marks & MpPanel::MARK_YOU) != 0;
            CHECK(you == (r.value[0] != 0));
            if (you) ++marks, word = U8(r.value);
        }
        CHECK(m.headCount == 2 && marks == 1 && !word.empty() && word.size() <= 8);
        (void)want;
        if (l == MpMsgText::LANG_EN) CHECK(word == "You");
    }
    MpMsgText::setLanguage(MpMsgText::LANG_EN);
}

// Gather / Lobby star-cache fix: S_GATHER must populate the star cache once on entry (and lazily on any cache miss)
// so building the S_GATHER model every frame (leader with lastCoop set) performs ZERO per-turn disk reads.
void testGatherStarCache() {
    // 1. Play Together (openGather) with lastCoop = 2 as leader: modelGather checks coopStars(last) every frame
    fresh();
    fake::flow.lastCoop = 2;
    openMain();
    ok();   // Play Together -> openGather() -> push(S_GATHER)
    frames(2);
    gatherHeads(2, 1);   // leader with a friend present (PIC_SQUARE "Change File" check active every frame)
    CHECK(S().screen() == MpMenu::S_GATHER && guideHasPic(MpMsgText::PIC_SQUARE));
    {
        const int reads0 = fake::slotReads;
        for (int i = 0; i < 180; i++) {
            frames(1);
            (void)G();   // exercises modelGather() + coopStars(last) every frame
        }
        CHECK(fake::slotReads == reads0);
    }
    // 2. Open Change File (S_SAVE) and back out to S_GATHER: still zero per-frame reads
    press(4);   // Square -> S_SAVE
    CHECK(S().screen() == MpMenu::S_SAVE);
    back();     // Circle -> S_GATHER
    CHECK(S().screen() == MpMenu::S_GATHER);
    {
        const int reads0 = fake::slotReads;
        for (int i = 0; i < 120; i++) {
            frames(1);
            (void)G();
        }
        CHECK(fake::slotReads == reads0);
    }
    // 3. Online room gathering (openRoomGather): zero per-frame reads
    fresh();
    fake::flow.lastCoop = 2;
    toQuick();
    fake::active = true;
    fake::host = true;
    fake::status.phase = MP_UI_LOBBY;
    frames(3);
    CHECK(S().screen() == MpMenu::S_GATHER);
    {
        const int reads0 = fake::slotReads;
        for (int i = 0; i < 120; i++) {
            frames(1);
            (void)G();
        }
        CHECK(fake::slotReads == reads0);
    }
    // 4. Failed host save invalidates cache and returns to S_GATHER: lazy refreshStars populates once, then 0 per-frame reads
    fresh();
    fake::flow.lastCoop = 2;
    hostLobby();
    fake::enterFails = true;
    press(7);   // START -> prepareHostSave fails (mStarsValid = false) -> backToLobby (S_GATHER) + error dialog
    frames(4);
    if (S().dialogActive()) { ok(); frames(2); }
    CHECK(S().screen() == MpMenu::S_GATHER);
    (void)G();   // first frame after invalidation populates cache once
    {
        const int reads0 = fake::slotReads;
        for (int i = 0; i < 120; i++) {
            frames(1);
            (void)G();
        }
        CHECK(fake::slotReads == reads0);
    }
}
