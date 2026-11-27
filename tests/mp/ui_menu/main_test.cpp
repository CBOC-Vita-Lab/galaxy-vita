// The menus' host tests: the main family (flow split of ui_menu_test.cpp). Shared helpers: common.hpp.
#include "common.hpp"

void testEntry() {
    using namespace MpMenu;
    using MpEntry::Logic;
    // first boot: Single Player is the default; A = the file select's heads; nothing written to mp.cfg
    {
        fresh(); EntryRig r; r.begin(); r.settle();
        CHECK(r.logic.state() == Logic::ST_CHOOSE && r.logic.cursor() == MpEntry::CHOICE_SINGLE);
        CHECK(r.has(MpEntry::CMD_APPEAR));
        r.ok(); r.settle();
        CHECK(r.has(MpEntry::CMD_DECIDE) && r.has(MpEntry::CMD_SINGLE) && r.logic.state() == Logic::ST_DONE);
        CHECK(r.host.saves == 0 && r.host.opens == 0 && !S().isOpen());
    }
    // Multiplayer: remembered (mp.cfg), the menu opens from the entry; Back from the menu = the entry again (cursor on Multiplayer);
    // B there = the title
    {
        fresh(); EntryRig r; r.begin(); r.settle();
        r.down(); CHECK(r.logic.cursor() == MpEntry::CHOICE_MULTI);
        r.ok(); r.settle();
        CHECK(r.host.saved == 1 && r.host.saves == 1 && r.host.persists == 1 && r.host.opens == 1);   // (Multiplayer: written at once)
        CHECK(S().isOpen() && S().screen() == S_MAIN && S().fromEntry() && r.logic.state() == Logic::ST_MENU);
        CHECK(logHas("ui: multiplayer menu from the entry screen"));
        frames(2);
        r.back(); r.settle();
        CHECK(!S().isOpen() && r.logic.state() == Logic::ST_CHOOSE && r.logic.cursor() == MpEntry::CHOICE_MULTI);
        CHECK(logHas("entry: multiplayer menu closed: the entry screen again"));
        r.back(); r.settle();
        CHECK(r.cmds.back() == MpEntry::CMD_TITLE && r.logic.state() == Logic::ST_OFF);
        CHECK(r.host.saves == 1);   // (Back never changes the remembered choice)
    }
    // the remembered default; after a session: Multiplayer; Single Player chosen again is remembered too
    {
        fresh(); EntryRig r; r.host.saved = 1; r.begin(); r.settle();
        CHECK(r.logic.cursor() == MpEntry::CHOICE_MULTI);
        r.press(8); CHECK(r.logic.cursor() == MpEntry::CHOICE_SINGLE);
        r.ok(); r.settle();
        CHECK(r.host.saved == 0 && r.host.saves == 1 && r.has(MpEntry::CMD_SINGLE));
        CHECK(r.host.persists == 0);   // review2 #11: Single Player after Multiplayer: remembered in RAM only, no file written on its way (the flush comes with the file list)
        EntryRig a; a.begin(true); a.settle();
        CHECK(a.logic.cursor() == MpEntry::CHOICE_MULTI);
    }
    // the star pointer / a finger moves the cursor when it moves onto a button (the d-pad still wins while it stays there);
    // input waits until the buttons have appeared
    {
        fresh(); EntryRig r; r.begin();
        r.pointed = 1; r.ok();   // (still appearing: nothing)
        CHECK(r.logic.state() != Logic::ST_DECIDED && r.logic.cursor() == 0);
        r.settle();
        CHECK(r.logic.cursor() == 0);   // (a pointer already resting on a pill when the screen came up does not choose it)
        r.pointed = -1; r.frame(); r.pointed = 1; r.frame();
        CHECK(r.logic.cursor() == MpEntry::CHOICE_MULTI);
        r.press(8);
        CHECK(r.logic.cursor() == MpEntry::CHOICE_SINGLE);
        r.pointed = -1; r.frame(); r.pointed = 1; r.frame();
        CHECK(r.logic.cursor() == MpEntry::CHOICE_MULTI);
    }
    auto toMenu = [](EntryRig& r) { r.begin(); r.settle(); r.down(); r.ok(); r.settle(); frames(2); };
    // host (flow): Play Together > Host (the lobby at once) > Start > Which file? > Start from My Own File > file 2 > Make a Co-op Copy
    // (the first empty co-op file: 1) > the session launches co-op file 1 from the entry
    {
        fresh(); EntryRig r; toMenu(r);
        r.ok();                                     // Play Together: the gathering screen at once
        CHECK(S().screen() == S_GATHER);
        gatherHeads(2, 1);                          // (Luigi64 arrives; this console leads)
        session(true, 0, MP_UI_LOBBY); r.frames(3);
        r.press(7);                                 // START
        CHECK(S().screen() == S_SAVE && S().itemCount() == 3);
        r.down(2);
        CHECK(cursorLabel() == "Start from My File");
        r.ok();
        CHECK(S().screen() == S_SPFILE && S().cursor() == 0 && S().itemCount() == 6);
        CHECK(rowLabel(1) == "File 2");
        r.down(); r.ok();
        CHECK(S().screen() == S_FILEUSE && cursorLabel() == "Play on a Copy");
        r.back();   // Back: the file list again
        CHECK(S().screen() == S_SPFILE && S().cursor() == 1);
        r.ok();
        fake::onMpRoot = true;                      // (the fake import does not switch the root itself)
        r.ok();                                     // Make a Co-op Copy: starts
        CHECK(logHas("ui: single-player file 2 picked for copy") && logHas("ui: save choice kind=2 coop=1 sp=2"));
        CHECK(startsAll() == 1 && logHas("ui: host save kind=2 coop=1 sp=2 ready"));
        fake::status.phase = MP_UI_PLAYING; r.frames(4);
        CHECK(r.launched == 1 && r.logic.state() == Logic::ST_DONE && !S().isOpen());
        CHECK(logHas("entry: launch file=1"));
    }
    // host: Play on This File: file 4 (an empty file 1 cannot be chosen): yes to the backup question; the launch loads file 4
    {
        fresh(); fake::spMissing = 1; EntryRig r; toMenu(r);
        r.ok(); gatherHeads(2, 1); session(true, 0, MP_UI_LOBBY); r.frames(3);
        r.press(7); r.down(2); r.ok();
        CHECK(S().screen() == S_SPFILE && S().cursor() == 1);   // (the first file that exists)
        r.press(8); r.ok();                                     // file 1 is empty: the buzzer, still here
        CHECK(S().screen() == S_SPFILE && !S().dialogActive());
        r.down(3); r.ok(); r.down(); r.ok(); r.frames(3);       // file 4 > Play on This File > "saves go to file 4" Yes
        CHECK(logHas("ui: save choice kind=3 coop=2 sp=4"));
        CHECK(fake::backups == 1 && fake::spSession == 4);
        fake::status.phase = MP_UI_PLAYING; r.frames(4);
        CHECK(r.launched == 4);
    }
    // guest: Join, the first room: no file, no character question: the session's slot (the host's progress comes with the snapshot)
    {
        fresh(); EntryRig r; toMenu(r);
        r.ok();                                       // Play Together: the gathering screen; Luigi64 leads
        gatherHeads(2, 2);
        session(false, 1, MP_UI_LOBBY); r.frames(3);
        CHECK(S().screen() == S_GATHER);
        fake::status.phase = MP_UI_PLAYING; r.frames(4);
        CHECK(r.launched == 1 && logHas("ui: launch file=1"));
        for (auto& l : fake::log) CHECK(l.find("save choice") == std::string::npos && l.find("picked for") == std::string::npos);
    }
    // the recovery offer comes when Multiplayer is chosen
    {
        fresh(); fake::recovery = true; EntryRig r; toMenu(r);
        CHECK(logHas("ui: recovery offered stars=3"));
        CHECK(S().screen() == S_SLOT);
        r.ok(); r.frames(3);
        CHECK(fake::recoverSlot == 1 && S().screen() == S_MAIN);
    }
    // How to Play (Triangle, flow): one message window over the menu (no 2P manual), then the menu again
    {
        fresh(); EntryRig r; toMenu(r);
        io.answer = MpMenu::DlgState::No;   // (then "Run a Connection Test?": No)
        r.press(3); r.frames(4);
        io.answer = MpMenu::DlgState::Yes;
        CHECK(!r.has(MpEntry::CMD_MANUAL) && r.logic.state() == Logic::ST_MENU && S().screen() == S_MAIN && logHas("ui: window MP_HOWTO"));
    }
    // a host who closes the lobby, and Back all the way: the entry screen (never the file select)
    {
        fresh(); EntryRig r; toMenu(r);
        r.ok(); gatherHeads(2, 1);
        session(true, 0, MP_UI_LOBBY); r.frames(3);
        CHECK(S().screen() == S_GATHER);
        r.back(); r.frames(3);   // "Leave?" Yes
        CHECK(S().isOpen() && S().screen() == S_MAIN);
        for (int i = 0; i < 10 && S().isOpen(); i++) r.back();
        r.settle();
        CHECK(!S().isOpen() && r.logic.state() == Logic::ST_CHOOSE && r.launched == 0);
    }
    // a session that ends while waiting in the lobby (the host left): the menu stays usable, Back returns to the entry
    {
        fresh(); EntryRig r; toMenu(r);
        r.ok(); gatherHeads(2, 2);
        session(false, 1, MP_UI_LOBBY); r.frames(3);
        fake::active = false; fake::status.phase = MP_UI_IDLE; fake::status.error = MP_ERR_HOST_LEFT; r.frames(4);
        CHECK(S().isOpen() && r.logic.state() == Logic::ST_MENU);
        r.frames(3);   // (the window answered)
        for (int i = 0; i < 10 && S().isOpen(); i++) r.back();
        r.settle();
        CHECK(r.logic.state() == Logic::ST_CHOOSE && r.launched == 0);
    }
    // review 2 #4: Single Player waits while the save root could not be switched back (the window says so); once it can, it goes on
    {
        fresh();
        fake::onMpRoot = true;
        fake::leaveFails = 1000;
        S().onFileSelect();
        EntryRig r; r.begin(); r.settle();
        CHECK(r.logic.cursor() == MpEntry::CHOICE_SINGLE);
        r.ok(); r.settle();
        CHECK(!r.has(MpEntry::CMD_SINGLE) && logHas("ui: window MP_SP_UNAVAILABLE") && logHas("entry: single player waits for the save root"));
        CHECK(fake::onMpRoot);
        fake::leaveFails = 0;
        r.ok(); r.settle();
        CHECK(r.has(MpEntry::CMD_SINGLE) && !fake::onMpRoot);
    }
    // ... and with nothing pending it never touches the save root
    {
        fresh(); EntryRig r; r.begin(); r.settle();
        const int leaves = fake::rootLeaves;
        r.ok(); r.settle();
        CHECK(r.has(MpEntry::CMD_SINGLE) && fake::rootLeaves == leaves);
    }
    // the file select's own path: opened with file 3, "Start from My Own File" has its cursor on file 3
    {
        fresh(); S().openMain(3); frames(3);
        CHECK(!S().fromEntry());
        ok(); frames(2); gatherHeads(2, 1); session(true, 0, MP_UI_LOBBY); frames(3);
        ok(); down(2); ok();
        CHECK(S().screen() == S_SPFILE && S().cursor() == 2);
    }
}

// flow (wave 2b): Rejoin, the Join list's sources, Which file?'s names, the step counts of the flow sheet
static int sPresses = 0;
void testFlow() {
    // savefix: the settings cannot be saved this run: one plain window, once per boot, then the menu as usual
    fresh();
    fake::settingsRO = true;
    openMain();
    CHECK(Logged("ui: window MP_SETTINGS_RO") && S().screen() == MpMenu::S_MAIN);
    fake::log.clear();
    S().close(); openMain();
    CHECK(!Logged("ui: window MP_SETTINGS_RO"));
    // Rejoin: a drop (the core gave up) offers "Back to <host>'s Room" on top of the Multiplayer menu for 2 minutes
    fresh();
    dropOut();
    openMain();
    CHECK(S().itemCount() == 4 && rowLabel(0) == "Continue Player's Game");
    {   // flow look: the Multiplayer menu in the file select's look (no title band, no hand)
        static MpPanel::Model lm;
        S().buildModel(&lm);
        CHECK(lm.look == MpPanel::LOOK_GAME && lm.kind == MpPanel::KIND_BUTTONS);
    }
    ok();
    CHECK(fake::rejoins == 1 && S().screen() == MpMenu::S_BUSY);
    session(false, 1, MP_UI_PLAYING); frames(3);
    CHECK(Logged("ui: in the room (guest)"));
    // ... refused (too late, the room closed): one window, the row is gone
    fresh();
    fake::rejoinOk = false;
    dropOut();
    openMain(); ok(); frames(2);
    CHECK(Logged("ui: window MP_REJOIN_GONE") && S().screen() == MpMenu::S_MAIN && S().itemCount() == 3);
    // ... gone after 2 minutes
    fresh();
    dropOut();
    frames(2 * 60 * 60 + 10);
    openMain();
    CHECK(S().itemCount() == 3 && rowLabel(0) == "Play Together");
    // ... never after leaving on purpose, being removed, or the host closing the room
    for (int err : {(int)MP_ERR_KICKED, (int)MP_ERR_HOST_LEFT, (int)MP_ERR_NONE}) {
        fresh();
        session(false, 1, MP_UI_PLAYING); frames(3);
        fake::active = false, fake::status.phase = MP_UI_IDLE, fake::status.error = static_cast<MpUiError>(err);
        io.answer = MpMenu::DlgState::No;
        frames(10);
        io.answer = MpMenu::DlgState::Yes;
        S().close();
        openMain();
        CHECK(S().itemCount() == 3);
    }
    // Which file?: a co-op file with its players' names (coop.cfg) and the host's head; the last galaxy as the line under it
    fresh();
    fake::coop[1].count = 2, fake::coop[1].galaxy = 3, fake::coop[1].looks[0] = MP_LOOK_LUIGI;
    snprintf(fake::coop[1].names[0], 17, "Alex"), snprintf(fake::coop[1].names[1], 17, "Luigi64");
    toSave();
    {
        static MpPanel::Model m;
        S().buildModel(&m);
        CHECK(rowLabel(0) == "Alex & Luigi64" && m.rows[0].icon == MpPanel::ICON_LUIGI && U8(m.hint) == "Last played: Galaxy");
    }
    // the session writes who played on the co-op file it plays on (the host first), and remembers it as the last co-op file
    ok();
    fake::status.phase = MP_UI_PLAYING; frames(65);
    CHECK(fake::coop[1].count == 3 && std::string(fake::coop[1].names[0]) == "Player" && fake::flow.lastCoop == 2);
    // a guest who keeps the progress in a co-op file: that file's names too
    fresh();
    session(false, 1, MP_UI_PLAYING); frames(65);
    fake::active = false; fake::status.phase = MP_UI_IDLE;
    frames(6);   // (Everyone left; Save your progress? Yes)
    down(2); ok(); frames(3);
    CHECK(fake::leaveSlot == 3 && fake::coop[2].count == 3);

    // review 2 #2: the file screens read the save files when they open, never per frame
    for (int which = 0; which < 3; which++) {
        fresh();
        toSave();
        if (which >= 1) { down(2); ok(); }   // My Files
        if (which >= 2) ok();                // that file's choices
        const int reads = fake::slotReads;
        frames(120);
        CHECK(fake::slotReads == reads);
    }
    // review 2 #3: a co-op slot with no stars and nothing in coop.cfg is "New"; with coop.cfg (a session played on it) it is in use
    fresh();
    fake::coopFull = true;   // (every co-op slot reads 7 stars: all in use)
    toSave();
    CHECK(S().itemCount() == 7);
    fresh();
    fake::coopZero = true;   // (the real co-op file: a slot nobody played reads 0 stars, not "missing")
    toSave();
    CHECK(S().itemCount() == 3 && rowLabel(1) == "New Team File");   // (co-op 2 in use, one New, my own file)
    fresh();
    fake::coopZero = true;
    fake::coop[0].used = 1;   // (co-op file 1: played, no star yet: in use, not New)
    toSave();
    CHECK(S().itemCount() == 4 && rowLabel(0) == "Team File 1" && rowLabel(2) == "New Team File");
    // the session on a new co-op file marks it in use at once
    fresh();
    fake::noCoop = true;
    hostLobby(); fake::players[1].present = true; frames(2);
    ok(); ok(); frames(2);
    CHECK(fake::coop[0].used == 1 && startsAll() == 1);

    // review 2 #9: a guest whose multiplayer save cannot be opened never launches (on the single-player root): leaves, says so
    fresh();
    fake::enterFails = true;
    guestLobby();
    fake::status.phase = MP_UI_PLAYING;
    frames(3);
    {
        int f = 0;
        CHECK(!S().takeLaunch(&f) && Logged("ui: launch refused: the multiplayer save could not be opened") && fake::leaves >= 1);
        CHECK(Logged("ui: window MP_SAVE_FAILED") && S().isOpen() && !LoggedPart("MP_SAVE_ASK"));
    }

    // the press budget (owner, one door): from the Multiplayer menu to the game starting / being in the friend's game
    // 1) first-time leader: Play Together, the character, (a friend arrives) START, the falls question, Which File?: <= 6 + falls
    fresh();
    fake::flow.charPicked = 0, fake::flow.fallsAsked = 0, fake::flow.lastCoop = 0;
    sPresses = 0;
    openMain();
    ok(); ++sPresses; frames(6);
    CHECK(S().screen() == MpMenu::S_CHARS);
    ok(); ++sPresses; frames(2);
    CHECK(S().screen() == MpMenu::S_GATHER);
    gatherHeads(2, 1); session(true, 0, MP_UI_LOBBY); frames(2);
    press(7); ++sPresses;
    CHECK(S().screen() == MpMenu::S_FALLS);
    ok(); ++sPresses;
    CHECK(S().screen() == MpMenu::S_SAVE);
    ok(); ++sPresses;
    CHECK(startsAll() == 1 && sPresses <= 6);
    fprintf(stderr, "  press budget: first-time leader: %d presses (the falls question included)\n", sPresses);
    // 2) first-time friend: Play Together, the character: then waiting for the leader (<= 5)
    fresh();
    fake::flow.charPicked = 0;
    sPresses = 0;
    openMain();
    ok(); ++sPresses; frames(6); ok(); ++sPresses; frames(2);
    gatherHeads(2, 2);
    CHECK(S().screen() == MpMenu::S_GATHER && sPresses <= 5);
    fprintf(stderr, "  press budget: first-time friend: %d presses\n", sPresses);
    // 3) returning leader: Play Together, START (<= 4); 4) returning friend: Play Together (<= 3)
    fresh();
    fake::flow.lastCoop = 2;
    sPresses = 0;
    openMain();
    ok(); ++sPresses; gatherHeads(2, 1); session(true, 0, MP_UI_LOBBY); frames(2);
    press(7); ++sPresses;
    CHECK(startsAll() == 1 && sPresses <= 4);
    fprintf(stderr, "  press budget: returning leader: %d presses\n", sPresses);
    fresh();
    sPresses = 0;
    openMain();
    ok(); ++sPresses; gatherHeads(2, 2);
    CHECK(S().screen() == MpMenu::S_GATHER && sPresses <= 3);
    fprintf(stderr, "  press budget: returning friend: %d presses\n", sPresses);
    // 5) Play with Anyone from a console never online: Play Together, Play with Anyone: the wait at once (no screen in between)
    fresh();
    fake::profile.registered = false;
    sPresses = 0;
    openMain();
    ok(); ++sPresses; gatherHeads(1, 1);
    ok(); ++sPresses;
    fprintf(stderr, "  press budget: play with anyone: %d presses, screen %s\n", sPresses, MpMenu::screenName(S().screen()));
    // 6) wire: a console with no account yet (the profile name is empty) but a saved name in mp.cfg is never asked for a name; with no
    //    name at all the keyboard is still the way out
    fresh();
    fake::profile.registered = false;
    fake::profile.name[0] = 0;
    snprintf(fake::savedName, sizeof fake::savedName, "Ann");
    openMain();
    ok(); frames(3);
    CHECK(!S().keyboardActive() && S().screen() != MpMenu::S_MAIN);
    fresh();
    fake::profile.registered = false;
    fake::profile.name[0] = 0;
    fake::savedName[0] = 0;
    openMain();
    ok(); frames(3);
    CHECK(S().keyboardActive());
    fprintf(stderr, "flow: ok\n");
}

