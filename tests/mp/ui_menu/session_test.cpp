// The menus' host tests: the session family (flow split of ui_menu_test.cpp). Shared helpers: common.hpp.
#include "common.hpp"

void testSession() {
    // guest: the host leaves for good: "Save your progress?" -> co-op file 3 (empty)
    fresh();
    session(false, 1, MP_UI_PLAYING);
    frames(3);
    fake::active = false; fake::status.phase = MP_UI_IDLE; fake::status.error = MP_ERR_HOST_LEFT;
    frames(6);
    auto logged = [](const char* line) {
        for (auto& l : fake::log) if (l == line) return true;
        return false;
    };
    CHECK(logged("ui: window MP_ERR_HOST_LEFT"));
    CHECK(logged("ui: window MP_SAVE_ASK (yes/no)"));
    CHECK(S().screen() == MpMenu::S_SLOT);
    down(2); ok(); frames(2);
    CHECK(fake::leaveChoice == MpSaveGame::LEAVE_SAVE_COOP && fake::leaveSlot == 3);
    frames(3);
    CHECK(!S().isBusy());
    // co-op file 2 has more stars than the progress: a warning first; No keeps the slot screen, nothing saved
    fresh();
    session(false, 1, MP_UI_PLAYING);
    frames(3);
    fake::active = false; fake::status.phase = MP_UI_IDLE;
    frames(8);
    CHECK(S().screen() == MpMenu::S_SLOT);
    io.answer = MpMenu::DlgState::No;
    down(1); ok(); frames(2);
    CHECK(io.last == "MP_DYN0");   // (the "fewer stars" warning carries the slot number)
    CHECK(fake::leaveChoice == 0);
    CHECK(S().screen() == MpMenu::S_SLOT);
    // Back from choosing a slot asks again; No = don't save
    back(); frames(3);
    CHECK(fake::leaveChoice == MpSaveGame::LEAVE_DONT_SAVE);
    io.answer = MpMenu::DlgState::Yes;
    // the host who played on its single-player file can save back to it
    fresh();
    fake::spSession = 4;
    session(true, 0, MP_UI_PLAYING);
    frames(3);
    S().openSession(); frames(2);
    down(2); ok(); frames(4);   // Leave (Warp, Observatory, Leave) -> Yes -> ended by us -> Save? Yes
    CHECK(S().screen() == MpMenu::S_SLOT);
    down(6); ok(); frames(3);   // "Back to My File 4" -> Yes
    CHECK(fake::leaveChoice == MpSaveGame::LEAVE_SAVE_SINGLE);
    // recovery at the menu: Yes -> co-op slot 1; then the tip
    fresh();
    fake::recovery = true;
    S().openMain(1); frames(3);
    CHECK(S().screen() == MpMenu::S_SLOT);
    ok(); frames(3);
    CHECK(fake::recoverSlot == 1 && fake::recoverChoice == MpSaveGame::LEAVE_SAVE_COOP);
    CHECK(S().screen() == MpMenu::S_MAIN);
    // gamefix 5: keeping a recovery into a single-player file: ask first when that file has more Power Stars or is newer
    struct Case { int spStars; bool newer; bool warn; };
    for (const Case c : {Case{9, false, true}, Case{1, true, true}, Case{3, false, false}, Case{-1, false, false}}) {
        fresh();
        fake::recovery = true; fake::recSingle = true; fake::recSpStars = c.spStars; fake::recSpNewer = c.newer;
        S().openMain(1); frames(3);
        CHECK(S().screen() == MpMenu::S_SLOT);
        down(6); ok(); frames(4);   // "Back to My File 2": the question(s) answered Yes
        CHECK(Logged("ui: window MP_SP_FEWER (yes/no)") == c.warn);
        CHECK(fake::recoverChoice == MpSaveGame::LEAVE_SAVE_SINGLE && fake::recoverSlot == 0);
        CHECK(fake::recoverConfirmed == c.warn);   // savebug: the layer is told "the player answered the more-stars question" only then
    }
    {   // answering No to the warning writes nothing
        fresh();
        fake::recovery = true; fake::recSingle = true; fake::recSpStars = 9;
        S().openMain(1); frames(3);   // (the recovery question itself is answered Yes)
        down(6);
        io.answer = MpMenu::DlgState::No;
        ok(); frames(4);
        io.answer = MpMenu::DlgState::Yes;
        CHECK(Logged("ui: window MP_SP_FEWER (yes/no)") && fake::recoverChoice == 0);
    }
    // recovery declined: discarded
    fresh();
    fake::recovery = true;
    io.answer = MpMenu::DlgState::No;
    S().openMain(1); frames(4);
    CHECK(fake::recoveryDiscards == 1);
    io.answer = MpMenu::DlgState::Yes;
    CHECK(S().screen() == MpMenu::S_MAIN);
    // everyone else left (no error): "Everyone left", host: no keep prompt
    fresh();
    session(true, 0, MP_UI_PLAYING);
    frames(3);
    fake::active = false; fake::status.phase = MP_UI_IDLE;
    frames(6);
    {
        bool everyone = false, asked = false;
        for (auto& l : fake::log) everyone = everyone || l == "ui: window MP_EVERYONE_LEFT", asked = asked || l == "ui: window MP_SAVE_ASK (yes/no)";
        CHECK(everyone && asked);   // (then "Save your progress?": anyone can save)
    }
    // host migration: toasts
    fresh();
    session(false, 2, MP_UI_PLAYING);
    frames(3);
    fake::players[0].present = false; fake::hostSlot = 1; fake::epoch = 1;
    frames(3);
    bool migrated = false;
    for (auto& l : fake::log) migrated = migrated || l.find("Player left. Luigi64 chooses the galaxy now.") != std::string::npos;
    CHECK(migrated);
    fake::players[1].present = false; fake::hostSlot = 2; fake::epoch = 2;
    frames(3);
    bool you = false;
    for (auto& l : fake::log) you = you || l.find("You choose the galaxy now.") != std::string::npos;
    CHECK(you);
    // characters (flow): the first play shows the character select once; the pick is a wish and goes on to Join / Host
    fresh();
    fake::flow.charPicked = 0;
    openMain(); ok();
    CHECK(S().screen() == MpMenu::S_CHARS);
    right(2); ok();
    CHECK(fake::wish == MP_LOOK_TOAD && S().screen() == MpMenu::S_GATHER && fake::flow.charPicked == 1);
    back();
    CHECK(S().screen() == MpMenu::S_MAIN);   // (the character select is not in the way back)
    ok(); frames(2);
    CHECK(S().screen() == MpMenu::S_GATHER);   // (once: the next time straight to the gathering screen)
    // the six characters: a grid of two rows of three (down = the row below)
    fresh();
    fake::flow.charPicked = 0;
    openMain(); ok();
    down(); right(2); ok();
    CHECK(fake::wish == MP_LOOK_COSMIC_LUIGI);
    // in the lobby: Luigi is taken by slot 1 (buzz, stays); Cosmic Mario is free: granted by the host, back to the lobby
    fresh();
    fake::players[2].present = false;   // (Player Mario, Luigi64 Luigi)
    hostLobby();
    press(3);
    CHECK(S().screen() == MpMenu::S_CHARS && S().cursor() == 0);
    const int wishes = fake::wishes;
    right(1);
    CHECK(hintLine() == "Already taken");   // (uxfix #9: said as soon as the cursor is on it ...)
    ok();
    CHECK(fake::wishes == wishes && S().screen() == MpMenu::S_CHARS);   // (Luigi taken: no wish sent)
    frames(4 * 60);
    CHECK(hintLine() == "Already taken");   // (... and still said long after the buzz: it was gone after two seconds)
    down(1);
    CHECK(hintLine() != "Already taken");   // (free: the usual description)
    ok(); frames(2);   // Yellow Toad: free
    CHECK(fake::players[0].look == MP_LOOK_YELLOW_TOAD && S().screen() == MpMenu::S_GATHER);
    // refused by the host (another console took it first): "Already taken" after the wait, still on the screen
    fake::grant = false;
    press(3); right(1); ok();   // (from Yellow Toad: Cosmic Luigi, free; the host says no)
    frames(4 * 60);
    CHECK(S().screen() == MpMenu::S_CHARS);
    bool refused = false;
    for (auto& l : fake::log) refused = refused || l.find("refused") != std::string::npos;
    CHECK(refused);
    // flow (one door): Change Character on the gathering screen is Triangle; Back returns to the gathering
    fresh();
    hostLobby();
    press(3);
    CHECK(S().screen() == MpMenu::S_CHARS);
    back();
    CHECK(S().screen() == MpMenu::S_GATHER);
    // play on my file (flow: Start -> Which file? -> Start from My Own File -> file 1 -> Play on This File -> Yes): a backup, the save
    // module told, the session starts at once, the game loads my file
    fresh();
    toSave();
    CHECK(S().screen() == MpMenu::S_SAVE && rowLabel(0) == "Team File 2" && rowLabel(1) == "New Team File" && rowLabel(2) == "Start from My File");
    down(2); ok(); ok();   // (my file 1)
    CHECK(S().screen() == MpMenu::S_FILEUSE);
    down(); ok(); frames(2);
    CHECK(fake::backups == 1 && fake::spSession == 1 && startsAll() == 1);
    CHECK(Logged("ui: single-player file 1 picked for play") && Logged("ui: save choice kind=3 coop=2 sp=1"));
    CHECK(S().screen() == MpMenu::S_GATHER);
    fake::status.phase = MP_UI_PLAYING; frames(2);
    {
        int f = 0;
        CHECK(S().takeLaunch(&f) && f == 1);
    }
    // a co-op copy of my file goes into the first empty co-op file (no question) and starts
    fresh();
    fake::onMpRoot = true;   // (the fake import does not switch the root itself)
    toSave(); down(2); ok(); down(); ok(); ok(); frames(2);   // (my file 2: Make a Co-op Copy)
    CHECK(Logged("ui: single-player file 2 picked for copy") && Logged("ui: save choice kind=2 coop=1 sp=2") && startsAll() == 1);
    // six co-op files in use: which one to replace; one with more stars than my file asks first
    fresh();
    fake::coopFull = true;
    toSave();
    CHECK(S().itemCount() == 7);   // (six co-op files, no new one, my own file; uxfix #5: Safe Copies is a Triangle action now)
    down(6); ok(); ok(); ok();   // (my file 1: Make a Co-op Copy)
    CHECK(S().screen() == MpMenu::S_SLOT);
    io.answer = MpMenu::DlgState::No;
    down(1); ok(); frames(2);   // co-op 2: fewer stars in my file -> asked, No
    CHECK(S().screen() == MpMenu::S_SLOT && startsAll() == 0);
    io.answer = MpMenu::DlgState::Yes;
    // leaving as host asks the "goes on with the next player" question; Yes leaves
    fresh();
    session(true, 0, MP_UI_PLAYING);
    frames(2);
    S().openSession(); frames(2);
    down(2); ok();   // Warp, Observatory, Leave
    {
        bool asked = false;
        for (auto& l : fake::log) asked = asked || l == "ui: window MP_LEAVE_HOST (yes/no)";
        CHECK(asked);
    }
    frames(3);
    CHECK(fake::leaves == 1);
    // the file select's launch: Start in the lobby, Which file? on the co-op file in use, Start, then PLAYING
    fresh();
    toSave();
    CHECK(S().screen() == MpMenu::S_SAVE && S().cursor() == 0);   // (the cursor on the co-op file in use)
    ok();
    CHECK(startsAll() == 1);
    fake::status.phase = MP_UI_PLAYING;
    frames(2);
    int file = 0;
    CHECK(S().takeLaunch(&file) && file == 2);   // (the first co-op file in use)
    CHECK(!S().isOpen());
    CHECK(fake::flow.lastCoop == 2);
    // the very first time (no co-op file at all): Which file? has New Co-op File under the cursor: one press, nothing else asked
    fresh();
    fake::noCoop = true;
    fake::onMpRoot = true;
    hostLobby(); fake::players[1].present = true; frames(2);
    ok(); frames(2);
    CHECK(S().screen() == MpMenu::S_SAVE && S().itemCount() == 2 && rowLabel(0) == "New Team File" && rowLabel(1) == "Start from My File");
    ok(); frames(2);
    CHECK(startsAll() == 1 && Logged("ui: save choice kind=1 coop=1 sp=1") && !LoggedPart("(yes/no)"));
    // the gathering's leader alone: START does nothing (one door: no START picture either)
    fresh();
    fake::players[1].present = fake::players[2].present = false;
    toGather(); gatherHeads(1, 1); session(true, 0, MP_UI_LOBBY); frames(2);
    press(7);
    CHECK(startsAll() == 0 && S().screen() == MpMenu::S_GATHER);
    // gamefix 3: a start that cannot happen (the friend left while the file was chosen) forgets the host's save choice
    fresh();
    toSave();
    mp_gather_fake_remove_head(2);
    const int ab0 = fake::abandons;
    ok();
    CHECK(startsAll() == 0 && fake::abandons > ab0 && S().screen() == MpMenu::S_GATHER);
    // flow: every way out of choosing the host's file forgets the choice (abandonHostChoice), as closing the lobby always did
    struct Exit { const char* name; void (*go)(); int backs; };
    const Exit exits[] = {
        {"which file", [] { toSave(); }, 1},
        {"my files", [] { toSave(); down(2); ok(); }, 1},
        {"copy or play", [] { toSave(); down(2); ok(); ok(); }, 1},
        {"which co-op file to replace", [] { fake::coopFull = true; toSave(); down(6); ok(); ok(); ok(); }, 1},
    };
    for (const Exit& e : exits) {
        fresh();
        e.go();
        const int before = fake::abandons;
        for (int i = 0; i < e.backs; i++) back();
        if (fake::abandons <= before) fprintf(stderr, "  exit %s: no abandonHostChoice\n", e.name);
        CHECK(fake::abandons > before);
        // and back to the lobby: Leave (Yes) forgets it too
        while (S().screen() != MpMenu::S_GATHER && S().isOpen()) back();
        const int b2 = fake::abandons;
        back(); frames(2);
        CHECK(fake::abandons > b2);
    }
    // ... and the save failing to get ready (prepareHostSave: the module forgets it itself), back in the lobby with the window
    // (the fake root switch never fails; prepareHostSave's own abandon is unchanged code)
    // How to Play (flow): Triangle on the main menu: one native window with three short lines; the menu stays
    // (uxfix #7: and nothing else: the Connection Test is offered after a join that failed, not after How to Play)
    fresh();
    io.answer = MpMenu::DlgState::No;
    openMain(); press(3); frames(4);
    CHECK(Logged("ui: window MP_HOWTO") && !LoggedPart("MP_CT_OFFER") && !S().takeManual() && S().screen() == MpMenu::S_MAIN);
    io.answer = MpMenu::DlgState::Yes;
}

// fix2: Start closes the session menu; the same press reaching the scene's pause check (or its 12-frame hold) does not reopen it
void testStartReopen() {
    fresh();
    session(false, 1, MP_UI_PLAYING);
    frames(2);
    S().openSession(); frames(2);
    CHECK(S().screen() == MpMenu::S_SESSION);
    MpMenu::Input in;
    in.start = true;
    step(in);
    CHECK(!S().isOpen());
    S().openSession();   // (the scene's pause check, same frame)
    CHECK(!S().isOpen());
    frames(12);
    S().openSession();   // (+ held for 12 frames: PauseButtonCheckerInGame)
    CHECK(!S().isOpen() && Logged("ui: session menu reopen ignored"));
    frames(10);
    S().openSession();   // a new press later opens it
    CHECK(S().screen() == MpMenu::S_SESSION);
    // Resume (Circle) is the same
    back();
    CHECK(!S().isOpen());
    S().openSession();
    CHECK(!S().isOpen());
}

void testChatMenu() {
    static MpPanel::Model model;
    // the page from the session menu (a guest: Warp, Messages, Leave)
    fresh();
    session(false, 1, MP_UI_PLAYING);
    frames(2);
    S().openSession();
    frames(2);
    press(4);   // (uinative4: Messages sit on the guide band: Square)
    CHECK(S().screen() == MpMenu::S_MESSAGES);
    CHECK(S().itemCount() == MP_CHAT_PHRASES + 1);
    S().buildModel(&model);
    CHECK(model.kind == MpPanel::KIND_GRID && model.cellCount == MP_CHAT_PHRASES + 1 && model.rowCount == 0 && model.cursor == 0);
    CHECK(model.cells[MP_CHAT_PHRASES].wide && !model.cells[0].wide);
    for (int i = 0; i < MP_CHAT_PHRASES; i++) {
        char16_t want[64];
        MpMsgText::format(want, 64, static_cast<MpText>(MP_T_PHRASE_0 + MpChatLogic::kGridOrder[i]), MpMsgText::Args());
        CHECK(Same16(model.cells[i].label, want));
    }
    {   // every phrase once in the grid
        bool seen[MP_CHAT_PHRASES] = {};
        for (uint8_t id : MpChatLogic::kGridOrder) { CHECK(id < MP_CHAT_PHRASES && !seen[id]); seen[id] = true; }
    }
    CHECK(model.look == MpPanel::LOOK_GAME && model.line[0] != 0);   // (lobby: the file select's look; the quick ring's tip is in its line)
    // the grid: left / right wrap in a row, down to the wide cell, up from it to the middle column
    right();
    CHECK(S().cursor() == 1);
    right(2);
    CHECK(S().cursor() == 0);
    press(5);
    CHECK(S().cursor() == 2);
    down(3);
    CHECK(S().cursor() == 11);
    down();
    CHECK(S().cursor() == MP_CHAT_PHRASES);
    down();
    CHECK(S().cursor() == MP_CHAT_PHRASES);
    press(8);
    CHECK(S().cursor() == 10);
    // a phrase: sent by number, the menu closes
    ok();
    CHECK(fake::chatPhrases.size() == 1 && fake::chatPhrases[0] == MpChatLogic::kGridOrder[10]);
    CHECK(!S().isOpen());
    // too soon (the chat module says no): the buzzer, the page stays
    frames(30); S().openSession(); frames(2); press(4);
    fake::chatRefuse = true;
    ok();
    CHECK(S().screen() == MpMenu::S_MESSAGES && fake::chatPhrases.size() == 1);
    fake::chatRefuse = false;
    // Write a Message: the keyboard, then the text goes and the menu closes
    down(4);
    CHECK(S().cursor() == MP_CHAT_PHRASES);
    S().buildModel(&model);
    CHECK(model.cursor == MP_CHAT_PHRASES);
    ok();
    CHECK(S().keyboardActive());
    snprintf(fake::kbText, sizeof fake::kbText, "See you at the top!");
    fake::kbResult = 1;
    frames(3);
    fake::kbResult = 0;
    CHECK(fake::chatTexts.size() == 1 && fake::chatTexts[0] == "See you at the top!");
    CHECK(!S().isOpen() && !S().isBusy());
    // no typing in this room: twelve tiles, no wide one
    fake::chatTyped = false;
    frames(30); S().openSession(); frames(2); press(4);
    CHECK(S().screen() == MpMenu::S_MESSAGES && S().itemCount() == MP_CHAT_PHRASES);
    S().buildModel(&model);
    CHECK(model.cellCount == MP_CHAT_PHRASES);
    for (int i = 0; i < model.cellCount; i++) CHECK(!model.cells[i].wide);
    down(5);
    CHECK(S().cursor() == 9);   // (stays on the last row)
    fake::chatTyped = true;

    // Players: the others only; a player's row opens their page: Hide Messages (anyone but yourself); the row says so; Show Messages undoes it
    guestPlaying();
    press(3);
    CHECK(S().screen() == MpMenu::S_PLAYERS && S().cursor() == 0 && S().itemCount() == 2);   // (Player, Ros: not yourself)
    ok();
    CHECK(S().screen() == MpMenu::S_PLAYER && rowsAre({MP_T_MUTE}));   // (a guest: no Remove; no friend code: no Add / Block)
    ok();
    CHECK(fake::chatMuted[0] && rowsAre({MP_T_UNMUTE}));
    back();
    S().buildModel(&model);
    CHECK(Same16(model.rows[0].value, Fmt16(MP_T_MUTED).c_str()) && model.rows[1].value[0] == 0);
    ok(); ok();
    CHECK(!fake::chatMuted[0]);
    // the host who opened the game: Remove from Game, also during play (Yes: the core removes them); with a friend code known (and the
    // service): Add Friend and Block too, in this order
    hostPlaying(false, "1234-5678");
    press(3); ok();
    CHECK(S().screen() == MpMenu::S_PLAYER && rowsAre({MP_T_ADD_FRIEND, MP_T_MUTE, MP_T_BLOCK, MP_T_REMOVE_ROOM}));
    down(3); ok(); frames(2);
    CHECK(fake::kicks == 1 && Logged("ui: kick slot=1") && S().screen() == MpMenu::S_PLAYERS);
    ok(); down(2); ok(); frames(2);   // Block (Yes): the service blocks the code, hidden here too
    CHECK(fake::ops.size() == 1 && fake::ops[0] == "block:1234-5678" && fake::chatMuted[1]);
    // Played With: noted while playing (names only, newest first, each once), on top of Friends; Add uses a code known this boot
    frames(60);
    CHECK(fake::flow.playedCount == 2 && std::string(fake::flow.playedWith[0]) == "Ros" && std::string(fake::flow.playedWith[1]) == "Luigi64");
    S().close();
    fake::active = false; fake::status.phase = MP_UI_IDLE;
    io.answer = MpMenu::DlgState::No;
    frames(10);
    io.answer = MpMenu::DlgState::Yes;
    toFriends();
    CHECK(S().screen() == MpMenu::S_FRIENDS && rowLabel(0) == "Played With (2)");
    ok(); down(); ok(); frames(2);   // Luigi64: the code the room knew; m-friends: they are already a friend (the fake's friend list has that code): marked, no Add; their page opens (the Add itself: friends_test)
    CHECK(fake::ops.size() == 1 && S().screen() == MpMenu::S_REQUEST);
    frames(3);
    back();
    CHECK(S().screen() == MpMenu::S_PLAYED || S().screen() == MpMenu::S_FRIENDS);
    // more than twenty: the last twenty (MP_PLAYED_WITH), newest first
    fresh();
    fake::players[2].present = false;
    for (int i = 0; i < MP_PLAYED_WITH + 2; i++) {
        snprintf(fake::players[1].name, sizeof fake::players[1].name, "P%d", i);
        session(true, 0, MP_UI_PLAYING); frames(62);
        fake::active = false; fake::status.phase = MP_UI_IDLE;
        io.answer = MpMenu::DlgState::No;
        frames(10);
    }
    io.answer = MpMenu::DlgState::Yes;
    CHECK(fake::flow.playedCount == MP_PLAYED_WITH && std::string(fake::flow.playedWith[0]) == "P" + std::to_string(MP_PLAYED_WITH + 1));

    // one door: no Room Settings; typed messages are on in every game (the gathering's Start sends them on)
    fresh();
    hostLobby();
    press(4);
    CHECK(S().screen() == MpMenu::S_GATHER);
    fprintf(stderr, "chat menu: ok\n");
}

// ---- the session family, 11 Nov (m-session): the screen START opens in play reads as the game's own pause screen, and the people pages
static bool noRowSays(MpText id) {
    static MpPanel::Model m;
    S().buildModel(&m);
    for (int i = 0; i < m.rowCount; i++) if (std::u16string(m.rows[i].label) == Fmt16(id)) return false;
    return true;
}

void testSessionMenu() {
    static MpPanel::Model m;
    // the player who opened a Play Together game: three big buttons, no title band, no settings row; Messages and Players on the guide
    hostPlaying();
    CHECK(S().screen() == MpMenu::S_SESSION);
    CHECK(rowsAre({MP_T_WARP, MP_T_BACK_DOME, MP_T_LEAVE_SESSION}));
    S().buildModel(&m);
    CHECK(m.kind == MpPanel::KIND_BUTTONS && m.title[0] == 0 && m.subtitle[0] == 0 && m.hint[0] == 0);
    CHECK(guideHas(MP_T_STAMPS) && guideHas(MP_T_PLAYER_LIST) && guideHas(MP_T_RESUME) && m.buttonCount == 3);   // (three guide buttons at most; the big buttons are chosen by Cross)
    CHECK(noRowSays(MP_T_FALLS));
    // every row does something: Warp asks for a player (two others: a list), the Observatory asks, Leave asks
    ok();
    CHECK(S().screen() == MpMenu::S_WARP && S().itemCount() == 2);
    back();
    CHECK(S().screen() == MpMenu::S_SESSION);
    down(); ok();
    frames(3);
    CHECK(Logged("ui: window MP_BACK_DOME (yes/no)") && S().takeDome() && !S().isOpen());   // (asked, Yes: everyone goes)
    // Back (Circle) goes back to the game
    hostPlaying();
    back();
    CHECK(!S().isOpen());
    // the same press by touch-free Start closes it too
    hostPlaying();
    MpMenu::Input st;
    st.start = true;
    step(st);
    CHECK(!S().isOpen());

    // a player who joined: Go to a Friend and Leave (the Observatory takes everyone: only the one who leads)
    guestPlaying();
    CHECK(rowsAre({MP_T_WARP, MP_T_LEAVE_SESSION}));
    // Star Race: no Warp, no Observatory: Leave alone; the opener and a guest
    hostPlaying(true);
    CHECK(rowsAre({MP_T_LEAVE_SESSION}) && guideHas(MP_T_STAMPS) && guideHas(MP_T_PLAYER_LIST));
    guestPlaying(true);
    CHECK(rowsAre({MP_T_LEAVE_SESSION}));
    // alone in the game: no Warp (nobody to go to), no Players button, Triangle does nothing; Messages still there
    hostPlaying();
    fake::players[1].present = fake::players[2].present = false;
    frames(3);
    CHECK(rowsAre({MP_T_BACK_DOME, MP_T_LEAVE_SESSION}) && !guideHas(MP_T_PLAYER_LIST) && guideHas(MP_T_STAMPS));
    press(3);
    CHECK(S().screen() == MpMenu::S_SESSION);
    // reconnecting: only Leave (and Back): nobody to greet, nobody to go to, nobody to list
    guestPlaying();
    back();
    fake::active = false; fake::status.phase = MP_UI_RECONNECTING; fake::status.error = MP_ERR_NO_WIFI;
    frames(30);
    S().openSession(); frames(2);
    CHECK(S().screen() == MpMenu::S_SESSION && rowsAre({MP_T_LEAVE_SESSION}) && !guideHas(MP_T_STAMPS) && !guideHas(MP_T_PLAYER_LIST));
    press(4); press(3);
    CHECK(S().screen() == MpMenu::S_SESSION);
    // Leave: the one who opened it is told the others can go on; a guest is just asked; No stays, Yes leaves
    hostPlaying();
    down(2);
    io.answer = MpMenu::DlgState::No;
    ok(); frames(2);
    CHECK(Logged("ui: window MP_LEAVE_HOST (yes/no)") && fake::leaves == 0 && S().screen() == MpMenu::S_SESSION);
    io.answer = MpMenu::DlgState::Yes;
    ok(); frames(2);
    CHECK(fake::leaves == 1 && S().screen() == MpMenu::S_SLOT);   // (then: save the progress?)
    guestPlaying();
    down();
    ok(); frames(2);
    CHECK(Logged("ui: window MP_LEAVE_GUEST (yes/no)") && fake::leaves == 1 && S().screen() == MpMenu::S_SLOT);   // (Yes: the core is told)
}

void testSessionWarp() {
    // two others: the list (Back returns to the menu); the chosen player is the warp, and the menu closes
    guestPlaying();   // (this console is Luigi64, slot 1; Player is slot 0, Ros slot 2)
    ok();
    CHECK(S().screen() == MpMenu::S_WARP && S().itemCount() == 2 && rowLabel(0) == "Player" && rowLabel(1) == "Ros");
    back();
    CHECK(S().screen() == MpMenu::S_SESSION);
    ok(); down(); ok();
    int slot = -1;
    CHECK(S().takeWarp(&slot) && slot == 2 && !S().isOpen());
    // one other: no list of one; the warp goes at once
    guestPlaying();
    fake::players[2].present = false;
    frames(3);
    ok();
    CHECK(S().takeWarp(&slot) && slot == 0 && !S().isOpen());
    // the list's player leaves meanwhile: back to the menu, nothing stuck
    guestPlaying();
    ok();
    CHECK(S().screen() == MpMenu::S_WARP);
    fake::players[0].present = fake::players[2].present = false;
    frames(4);
    CHECK(S().screen() == MpMenu::S_SESSION);
}

void testSessionPlayers() {
    static MpPanel::Model m;
    // Players: the others, never yourself; a list of two; Back to the menu
    guestPlaying();
    press(3);
    CHECK(S().screen() == MpMenu::S_PLAYERS && S().itemCount() == 2 && rowLabel(0) == "Player" && rowLabel(1) == "Ros");
    S().buildModel(&m);
    for (int i = 0; i < m.rowCount; i++) CHECK(m.rows[i].ping < 0 && m.rows[i].value[0] == 0 && !(m.rows[i].marks & MpPanel::MARK_YOU));   // (no delay numbers or bars)
    back();
    CHECK(S().screen() == MpMenu::S_SESSION);
    // one other: Players opens their page at once; Back returns to the menu
    guestPlaying();
    fake::players[2].present = false;
    frames(3);
    press(3);
    CHECK(S().screen() == MpMenu::S_PLAYER && rowsAre({MP_T_MUTE}));
    back();
    CHECK(S().screen() == MpMenu::S_SESSION);
    // that player leaves while their page is up: back to the menu
    press(3);
    CHECK(S().screen() == MpMenu::S_PLAYER);
    fake::players[0].present = false;
    frames(3);
    CHECK(S().screen() == MpMenu::S_SESSION);

    // the page of a player with no friend code known: Hide Messages alone (no dead rows), in every game
    guestPlaying();
    press(3); ok();
    CHECK(rowsAre({MP_T_MUTE}) && guideHas(MP_T_BACK));
    // the page with a code and the service: Add Friend, Hide Messages, Block
    guestPlaying();
    fake::codes[0] = "1234-5678";
    press(3); ok();
    CHECK(rowsAre({MP_T_ADD_FRIEND, MP_T_MUTE, MP_T_BLOCK}));
    // ... the service gone (no online build): the code means nothing, only Hide Messages
    guestPlaying();
    fake::codes[0] = "1234-5678";
    fake::service = false;
    press(3); ok();
    CHECK(rowsAre({MP_T_MUTE}));
    // ... a code for Ros only: Player's page has no friend rows, Ros's has
    guestPlaying();
    fake::codes[2] = "9999-8888";
    press(3); ok();
    CHECK(rowsAre({MP_T_MUTE}));
    back(); down(); ok();
    CHECK(S().screen() == MpMenu::S_PLAYER && rowsAre({MP_T_ADD_FRIEND, MP_T_MUTE, MP_T_BLOCK}));   // (Ros's own page has them)

    // Remove from Game: only the player who opened the game (and still leads it); never a guest; not a guest who became the leader
    hostPlaying();
    press(3); ok();
    CHECK(rowsAre({MP_T_MUTE, MP_T_REMOVE_ROOM}));
    guestPlaying();
    press(3); ok();
    CHECK(noRowSays(MP_T_REMOVE_ROOM));
    guestPlaying();
    fake::host = true; fake::hostSlot = 1;   // (the leader left: this console leads now, but did not open the game)
    frames(3);
    press(3); ok();
    CHECK(noRowSays(MP_T_REMOVE_ROOM) && rowsAre({MP_T_MUTE}));
    // the opener who no longer leads (dropped and came back as a guest) cannot remove either
    hostPlaying();
    fake::host = false; fake::hostSlot = 1;
    frames(3);
    press(3); ok();
    CHECK(noRowSays(MP_T_REMOVE_ROOM));

    // Hide / Show Messages are per player and flip the row; Back to the list keeps the cursor
    guestPlaying();
    press(3);
    ok();
    CHECK(rowIs(0, MP_T_MUTE) && !fake::chatMuted[0]);
    ok();
    CHECK(rowIs(0, MP_T_UNMUTE) && fake::chatMuted[0] && !fake::chatMuted[2]);
    ok();
    CHECK(rowIs(0, MP_T_MUTE) && !fake::chatMuted[0]);
    back();
    CHECK(S().screen() == MpMenu::S_PLAYERS);
    back();
    CHECK(S().screen() == MpMenu::S_SESSION);
    back();
    CHECK(!S().isOpen());
}

void testSessionFriends() {
    // wire: a room that is not online (Same Wi-Fi, Nearby, a spoken code) gives only the code the other console ANNOUNCED: Add Friend and Block
    // are still there, and the service is asked with it (the menus asked for the verified code only, so no gathering-screen game had them)
    hostPlaying(false, "1234-5678");
    fake::announcedOnly = true;
    press(3); ok();
    CHECK(rowIs(0, MP_T_ADD_FRIEND));
    ok(); frames(4);
    CHECK(fake::ops.size() == 1 && fake::ops[0] == "add:1234-5678");
    fake::announcedOnly = false;
    // Add Friend (Yes path): the service is asked with the announced code, then the busy wait and "added"
    hostPlaying(false, "1234-5678");
    press(3); ok();
    CHECK(rowIs(0, MP_T_ADD_FRIEND));
    ok(); frames(4);
    CHECK(fake::ops.size() == 1 && fake::ops[0] == "add:1234-5678");
    CHECK(LoggedPart("MP_FRIEND_ADDED"));
    // the service refuses (list full ...): its answer in a window, the page stays usable
    hostPlaying(false, "1234-5678");
    fake::friendOpState = -2;
    press(3); ok();
    ok(); frames(4);
    CHECK(LoggedPart("ui: error shown") || Logged("ui: window MP_FRIEND_FAILED"));   // (the service's own refusal)
    // the service cannot take the request at all: a window, no busy wait that never ends
    hostPlaying(false, "1234-5678");
    fake::friendOpState = 1;
    fake::opFails = true;
    press(3); ok();
    ok(); frames(3);
    CHECK(Logged("ui: window MP_FRIEND_FAILED") && S().screen() == MpMenu::S_PLAYER);   // (a window, the page stays; no wait that never ends)
    fake::opFails = false;
    // Block: asked first; No does nothing; Yes blocks at the service and hides their messages here
    hostPlaying(false, "1234-5678");
    press(3); ok();
    down(2);
    io.answer = MpMenu::DlgState::No;
    ok(); frames(3);
    CHECK(fake::ops.empty() && !fake::chatMuted[1] && S().screen() == MpMenu::S_PLAYER);
    io.answer = MpMenu::DlgState::Yes;
    ok(); frames(3);
    CHECK(fake::ops.size() == 1 && fake::ops[0] == "block:1234-5678" && fake::chatMuted[1] && S().screen() != MpMenu::S_PLAYER);
    // no online account yet: Add Friend signs in silently ("Connecting..." only, no name asked), then does the add
    hostPlaying(false, "1234-5678");
    fake::profile.registered = false;
    fake::ensureWork = 3;
    press(3); ok();
    ok(); frames(2);
    CHECK(!S().keyboardActive() && fake::registers == 0);
    frames(8);
    CHECK(fake::ops.size() == 1 && fake::ops[0] == "add:1234-5678");
}

void testSessionMessages() {
    static MpPanel::Model m;
    // the Messages page (Square): the twelve quick phrases and, in every kind of game, Write a Message; Back returns to the menu
    for (int race = 0; race < 2; race++) {
        hostPlaying(race == 1);
        press(4);
        CHECK(S().screen() == MpMenu::S_MESSAGES && S().itemCount() == MP_CHAT_PHRASES + 1);
        S().buildModel(&m);
        CHECK(m.cells[MP_CHAT_PHRASES].wide && guideHas(MP_T_SEND) && guideHas(MP_T_BACK));
        back();
        CHECK(S().screen() == MpMenu::S_SESSION);
    }
    // a game open to anyone has them too
    hostPlaying();
    MpChat::noteRoomPublic(true);
    press(4);
    CHECK(S().itemCount() == MP_CHAT_PHRASES + 1 && fake::chatTyped);
    // Write a Message: the keyboard; cancel comes back to the page
    down(4); ok();
    CHECK(S().keyboardActive());
    fake::kbResult = -1;
    frames(3);
    fake::kbResult = 0;
    CHECK(S().screen() == MpMenu::S_MESSAGES && !S().keyboardActive());
    back();
    // a hidden player's page does not change the page (their messages are theirs to hide: the receiver's choice)
    hostPlaying();
    press(3); down(); ok();
    ok();
    CHECK(fake::chatMuted[2] || fake::chatMuted[1]);
}

// five languages: every word of these screens exists and fits
void testSessionWords() {
    static MpPanel::Model m;
    const MpMsgText::Lang langs[5] = {MpMsgText::LANG_EN, MpMsgText::LANG_FR, MpMsgText::LANG_ES, MpMsgText::LANG_DE, MpMsgText::LANG_IT};
    for (MpMsgText::Lang l : langs) {
        MpMsgText::setLanguage(l);
        hostPlaying(false, "1234-5678");
        S().buildModel(&m);
        CHECK(m.rowCount == 3);
        for (int i = 0; i < m.rowCount; i++) CHECK(m.rows[i].label[0] != 0);
        for (int i = 0; i < m.buttonCount; i++) CHECK(m.buttons[i].label[0] != 0);
        press(3); ok();
        S().buildModel(&m);
        CHECK(m.rowCount == 5 && m.headCount == 1);   // (the player's head, then four choices)
        for (int i = 0; i < m.rowCount; i++) CHECK(m.rows[i].label[0] != 0);
        back(); back(); press(4);
        S().buildModel(&m);
        CHECK(m.cellCount == MP_CHAT_PHRASES + 1);
        for (int i = 0; i < m.cellCount; i++) CHECK(m.cells[i].label[0] != 0);
    }
    MpMsgText::setLanguage(MpMsgText::LANG_EN);
}

// the look (LOOK.md): the busiest variants of the session family's screens through the same geometry checks as kPaths, five languages, both pictures:
// the pause screen with all three buttons, three others to go to / to list, a page with a head over four choices (the fullest it gets)
void testSessionLook() {
    MpPanel::FontMetrics fm[MpPanel::FONT_COUNT];
    for (auto& f : fm) f.height = 33.0f, f.baseline = 26.0f, f.advance = Adv, f.has = HasG;
    static MpPanel::Model model;
    static MpPanel::Plan plan;
    static MpPanel::Scene scene;
    struct V { const char* name; MpMenu::Scr want; void (*go)(); int choices; bool zoom = false; };
    const V variants[] = {
        {"session(opener, 3 buttons, zoom row)", MpMenu::S_SESSION, [] { hostPlaying(); }, 3, true},   // mppause: the zoom row's room, no message window
        {"session(race, zoom row)", MpMenu::S_SESSION, [] { hostPlaying(true); }, 1, true},
        {"session(opener, 3 buttons)", MpMenu::S_SESSION, [] { hostPlaying(); }, 3},
        {"session(race)", MpMenu::S_SESSION, [] { hostPlaying(true); }, 1},
        {"warp(3 others)", MpMenu::S_WARP, [] { hostPlaying(); fake::players[3].present = true; frames(2); ok(); }, 3},
        {"players(3 others)", MpMenu::S_PLAYERS, [] { hostPlaying(); fake::players[3].present = true; frames(2); press(3); }, 3},
        {"player(4 choices)", MpMenu::S_PLAYER, [] { hostPlaying(false, "1234-5678"); press(3); ok(); }, 4},
        {"player(guest, 3)", MpMenu::S_PLAYER, [] { guestPlaying(); fake::codes[0] = "1234-5678"; frames(2); press(3); ok(); }, 3},
        {"player(1)", MpMenu::S_PLAYER, [] { guestPlaying(); press(3); ok(); }, 1},
    };
    const char* langs[] = {"UsEnglish", "UsFrench", "UsSpanish", "EuGerman", "EuItalian"};
    for (float width : {810.6667f, 608.0f})
    for (const char* l : langs)
    for (const V& v : variants) {
        MpPanel::Screen scr;
        scr.width = width;
        MpMsgText::setLanguage(MpMsgText::languageIndex(l));
        g_zoomCapable = v.zoom;
        v.go();
        CHECK(S().screen() == v.want);
        S().buildModel(&model);
        if (v.want == MpMenu::S_SESSION) {   // mppause: with the zoom row the message window is gone (the panel's note says it); without it the one short line stays
            CHECK(model.zoomRow == v.zoom && (model.line[0] == 0) == v.zoom);
            CHECK(S().zoomRowShown() == v.zoom);
            if (!v.zoom) CHECK(std::u16string(model.line) == Fmt16(MP_T_SESSION_RUNS) && std::u16string(model.line).find(u'\n') == std::u16string::npos);
        } else CHECK(!model.zoomRow && !S().zoomRowShown());
        vitaCheckModel(model, v.name, l);
        CHECK(model.look == MpPanel::LOOK_GAME);   // the look is on
        CHECK(model.rowCount - model.headCount == v.choices && model.buttonCount <= 3);
        MpPanel::beginScene(&scene);
        MpPanel::begin(&plan);
        MpPanel::compose(model, scr, fm, 0.25f, &scene, &plan);
        CHECK(!plan.overflow);
        CHECK(plan.guideHitCount == model.buttonCount);
        int pointers = 0;
        for (int i = 0; i < scene.useCount; i++) if (scene.uses[i].kind == MpPanel::USE_POINTER) ++pointers;
        CHECK(pointers == 0);   // (the button's highlight is the cursor)
        uinativeGeometry(v.name, l, model, scr, scene, plan);
    }
    g_zoomCapable = 0;
    MpMsgText::setLanguage(0);
}

// uxfix #8 (touch): on the Messages page a finger's first tap on a tile only points it; the second tap on the same tile sends it
void testMessagesTapTwice() {
    fresh();
    session(true, 0, MP_UI_PLAYING); frames(2); S().openSession(); frames(2); press(4);
    CHECK(S().screen() == MpMenu::S_MESSAGES);
    const int before = S().cursor();
    const size_t sent = fake::chatPhrases.size();
    const int other = before == 5 ? 6 : 5;
    CHECK(S().firstTapPoints(other));                 // (first tap: the cursor moves there, the tap's Cross is swallowed)
    CHECK(S().cursor() == other && fake::chatPhrases.size() == sent);
    CHECK(!S().firstTapPoints(other));                // (second tap on the same tile: let the Cross through)
    ok();
    CHECK(fake::chatPhrases.size() == sent + 1);      // ... and it sends
    // other screens: a tap is never held back
    fresh();
    CHECK(!S().firstTapPoints(2));
}
