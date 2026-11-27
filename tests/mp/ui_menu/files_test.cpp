// The menus' host tests: the files family (flow split of ui_menu_test.cpp). Shared helpers: common.hpp.
#include "common.hpp"

// fix2: the switch back to the single-player save root at the file select is retried; if it keeps failing, the game's message
// window says the single-player file can't be opened (once), and the retries go on
void testRootRetry() {
    fresh();
    fake::onMpRoot = true;
    fake::leaveFails = 3;
    S().onFileSelect();
    CHECK(fake::rootLeaves == 1 && LoggedPart("save root switch failed, retrying"));
    for (int i = 0; i < 200; i++) { step(); S().fileSelectTick(); }
    CHECK(!fake::onMpRoot && fake::rootLeaves == 4 && LoggedPart("ui: save root switched after"));
    CHECK(!LoggedPart("ui: window"));
    // never works: one warning after 5 s, retries go on, nothing else
    fresh();
    fake::onMpRoot = true;
    fake::leaveFails = 1000;
    S().onFileSelect();
    for (int i = 0; i < 5 * 60 + 40; i++) { step(); S().fileSelectTick(); }
    CHECK(Logged("ui: window MP_SP_UNAVAILABLE") && fake::onMpRoot);
    const int tries = fake::rootLeaves;
    for (int i = 0; i < 20 * 60; i++) { step(); S().fileSelectTick(); }
    int windows = 0;
    for (auto& l : fake::log) windows += l == "ui: window MP_SP_UNAVAILABLE";
    CHECK(windows == 1 && fake::rootLeaves > tries);
    fake::leaveFails = 0;
    for (int i = 0; i < 200; i++) { step(); S().fileSelectTick(); }
    CHECK(!fake::onMpRoot);
    // fix3: the warning waits while the file select cannot hold its input (title, its own windows), the retries go on meanwhile;
    // it opens on the first turn it can, says so once (the file select holds while S().isBusy()), and the window takes the A
    fresh();
    fake::onMpRoot = true;
    fake::leaveFails = 1000;
    S().onFileSelect();
    int opened = 0;
    for (int i = 0; i < 8 * 60; i++) { step(); opened += S().fileSelectTick(false) ? 1 : 0; }
    CHECK(opened == 0 && !LoggedPart("ui: window") && !S().isBusy() && fake::rootLeaves > 10);
    for (int i = 0; i < 60 && !opened; i++) { step(); opened += S().fileSelectTick(true) ? 1 : 0; }
    CHECK(opened == 1 && Logged("ui: window MP_SP_UNAVAILABLE") && S().isBusy());   // (the file select holds while busy)
    frames(3);   // (the test window answers at once)
    CHECK(!S().isBusy());
    for (int i = 0; i < 20 * 60; i++) { step(); opened += S().fileSelectTick(true) ? 1 : 0; }
    CHECK(opened == 1);
    // nothing pending: no tries
    fresh();
    S().onFileSelect();
    for (int i = 0; i < 100; i++) { step(); S().fileSelectTick(); }
    CHECK(fake::rootLeaves == 0);
}


// ================================================================== m-files
using MpMenu::DlgState;

// GUARANTEE 1: a file that could not be read is never "New", never a place to write, says so, and offers nothing that overwrites it
void testFilesUnreadable() {
    // Which file?: co-op file 2 (in use) cannot be read
    fresh();
    fake::coopBad = 1u << 1;
    fake::flow.lastCoop = 2;   // (it was the one played last: still not chosen by default)
    toSave();
    CHECK(S().screen() == Scr::S_SAVE);
    CHECK(rowLabel(0) == "Team File 2" && rowValue(0) == "Unreadable" && rowDisabled(0));
    CHECK(rowLabel(1) == "New Team File" && rowLabel(2) == "Start from My File" && S().itemCount() == 3);
    CHECK(rowLabel(0) != "New Team File" && rowValue(0) != "New");
    CHECK(hintLine().find("Can't read this file") == std::string::npos);   // (the cursor is on the default: the new file)
    CHECK(rowLabel(S().cursor()) == "New Team File");                  // (the default is never the unreadable one)
    // the cursor on it: the hint says plainly what happened; Cross does nothing at all
    press(8);   // up
    CHECK(rowLabel(S().cursor()) == "Team File 2" && hintLine().find("Can't read this file") != std::string::npos);
    const int starts = startsAll();
    ok();
    CHECK(S().screen() == Scr::S_SAVE && startsAll() == starts && !LoggedPart("ui: save choice") && !LoggedPart("ui: host save"));
    // the same file read again at the write: it became unreadable AFTER the screen showed it as a new one
    fresh();
    toSave();
    fake::coopBad = 1u << 0;   // (co-op 1 is the "New" row)
    down(); ok();
    CHECK(LoggedPart("ui: host save refused") && startsAll() == 0 && !LoggedPart("ui: host save kind=1 coop=1 sp=0 ready"));
    // every co-op file unreadable: no co-op choice at all, only My Files / Safe Copies
    fresh();
    fake::coopBad = 63;
    toSave();
    CHECK(S().itemCount() == 7);
    for (int i = 0; i < MpPanel::kListRows; i++) CHECK(rowValue(i) == "Unreadable" && rowDisabled(i));   // (the first page)
    for (int i = 0; i < 6; i++) { ok(); down(); }
    CHECK(!LoggedPart("ui: save choice") && startsAll() == 0 && S().screen() == Scr::S_SAVE);
    // ... and a copy of one of My Files has nowhere to go
    fresh();
    fake::coopBad = 63;
    toSave(); down(6); ok(); ok(); ok();
    CHECK(S().screen() == Scr::S_FILEUSE || S().screen() == Scr::S_SPFILE);
    fresh();
    fake::coopBad = 63;
    toSave(); down(6); ok(); ok();   // (My Files, file 1)
    CHECK(S().screen() == Scr::S_FILEUSE);
    ok();                           // Make a Co-op Copy
    CHECK(Logged("ui: window MP_NO_ROOM_COPY") && !LoggedPart("ui: save choice") && S().screen() == Scr::S_FILEUSE);
    // a full set of six in use and one unreadable: replacing the unreadable one is not offered
    fresh();
    fake::coopFull = true;
    fake::coopBad = 1u << 3;
    toSave(); down(6); ok(); ok(); ok();   // (My Files, file 1, Make a Co-op Copy: which co-op file to replace)
    down(3);   // (the fourth row: the next page's first)
    CHECK(S().screen() == Scr::S_SLOT && rowValue(0) == "Unreadable" && rowDisabled(0));
    ok();
    CHECK(S().screen() == Scr::S_SLOT && !S().dialogActive() && !LoggedPart("ui: save choice"));
    // My Files: file 3 cannot be read: not "New", not choosable
    fresh();
    fake::spBad = 1u << 2;
    toSave(); down(2); ok();
    CHECK(S().screen() == Scr::S_SPFILE && rowLabel(2) == "File 3" && rowValue(2) == "Unreadable" && rowDisabled(2) && rowValue(2) != "New");
    down(2);
    CHECK(hintLine().find("Can't read this file") != std::string::npos);
    ok();
    CHECK(S().screen() == Scr::S_SPFILE);
    // a missing file (really not there) is still "New" and not choosable
    fresh();
    fake::spMissing = 1u << 3;
    toSave(); down(2); ok();
    down(3);
    CHECK(rowValue(0) == "New" && S().screen() == Scr::S_SPFILE);
    // Play on this file / save to my file: the file is read again at the choice
    fresh();
    toSave(); down(2); ok(); ok();
    CHECK(S().screen() == Scr::S_FILEUSE);
    fake::spBad = 1u << 0;
    down(); ok();
    CHECK(Logged("ui: window MP_FILE_NOT_READY") && fake::backups == 0 && fake::spSession == 0 && startsAll() == 0);
    // leaving: a co-op file that cannot be read is no place to keep the progress (recovery question)
    fresh();
    fake::recovery = true;
    fake::coopBad = 1u << 0;
    S().openMain(1); frames(3);
    CHECK(S().screen() == Scr::S_SLOT && rowValue(0) == "Unreadable" && rowDisabled(0));
    ok(); frames(3);
    CHECK(fake::recoverSlot == 0 && fake::recoverChoice == 0 && S().screen() == Scr::S_SLOT);
    down(); ok(); frames(3);   // the next one is fine
    CHECK(fake::recoverSlot == 2 || fake::recoverSlot == 0);
    // the guest's leave offer
    fresh();
    session(false, 1, MP_UI_PLAYING);
    frames(3);
    fake::coopBad = 1u << 2;
    fake::active = false; fake::status.phase = MP_UI_IDLE; fake::status.error = MP_ERR_HOST_LEFT;
    frames(6);
    CHECK(S().screen() == Scr::S_SLOT && rowValue(2) == "Unreadable");
    down(2); ok(); frames(3);
    CHECK(fake::leaveChoice == 0 && S().screen() == Scr::S_SLOT);
    // ... and the save itself re-reads the file (it went bad after the row was shown)
    fresh();
    session(false, 1, MP_UI_PLAYING);
    frames(3);
    fake::active = false; fake::status.phase = MP_UI_IDLE; fake::status.error = MP_ERR_HOST_LEFT;
    frames(6);
    CHECK(S().screen() == Scr::S_SLOT);
    fake::coopBad = 1u << 2;
    down(2); ok(); frames(3);
    CHECK(fake::leaveChoice == 0 && S().screen() == Scr::S_SLOT);
}

// GUARANTEE 2: nothing here starts a write to a single-player file except the confirmed "Play on this file" / "Save to my file",
// whose question names the file and its Power Stars
void testFilesSinglePlayerWrite() {
    // browsing everything: nothing is backed up, begun or written back
    fresh();
    toSave(); down(2); ok(); ok();      // My Files > file 1
    back(); down(); ok(); ok();         // file 2 > Make a Co-op Copy (into the first empty co-op file: the session starts)
    CHECK(fake::backups == 0 && fake::spSession == 0 && fake::writeBacks == 0);
    // Play on this file: the question
    fresh();
    toSave(); down(2); ok(); down(2); ok();   // My Files > file 3
    CHECK(S().screen() == Scr::S_FILEUSE);
    down();
    io.answer = DlgState::No;
    ok(); frames(3);
    CHECK(Logged("ui: window MP_PLAY_MINE (yes/no)"));
    CHECK(io.lastText.find("File 3") != std::string::npos && io.lastText.find("5 Power Stars") != std::string::npos);
    CHECK(io.lastText.find("backup") != std::string::npos && windowFits(io.lastText));
    CHECK(fake::backups == 0 && fake::spSession == 0 && startsAll() == 0 && fake::writeBacks == 0);   // No: nothing happened
    back(); frames(2);                                                                                  // Back out of it all
    CHECK(fake::backups == 0 && fake::spSession == 0 && fake::writeBacks == 0);
    io.answer = DlgState::Yes;
    // Yes: a backup, the choice made
    fresh();
    toSave(); down(2); ok(); down(2); ok(); down(); ok(); frames(3);
    CHECK(fake::backups == 1 && fake::spSession == 3);
    // an unreadable file cannot be played on even when the cache still said it was fine (covered in testFilesUnreadable);
    // and a root switch that cannot be done blocks it
    fresh();
    fake::onMpRoot = true;
    fake::leaveFails = 1000;
    toSave(); down(2); ok(); ok(); down(); ok(); frames(3);
    CHECK(fake::backups == 0 && fake::spSession == 0 && startsAll() == 0);
    // saving the progress back to my file at the end: named, with the stars it has now and the progress's
    fresh();
    fake::spSession = 4;
    session(true, 0, MP_UI_PLAYING);
    frames(3);
    S().openSession(); frames(2);
    io.answer = DlgState::Yes;
    down(S().itemCount() - 1); ok(); frames(4);   // (Leave the Game: the last big button)
    CHECK(S().screen() == Scr::S_SLOT && S().itemCount() == 7);
    io.answer = DlgState::No;
    down(6); ok(); frames(3);
    CHECK(Logged("ui: window MP_SAVE_MINE (yes/no)") && fake::leaveChoice == 0);
    CHECK(io.lastText.find("File 4") != std::string::npos && io.lastText.find("5 Power Stars") != std::string::npos &&
          io.lastText.find("this has 8") != std::string::npos && windowFits(io.lastText));
    io.answer = DlgState::Yes;
}

// finding 5: after the player became the one running the game, "save to my file" is not offered (the gate says the source is gone)
void testFilesSourceGate() {
    fresh();   // gate says no source (a host by migration / a guest): five rows, no "Back to My File"
    session(false, 1, MP_UI_PLAYING);
    frames(3);
    fake::active = false; fake::status.phase = MP_UI_IDLE; fake::status.error = MP_ERR_HOST_LEFT;
    frames(6);
    CHECK(S().screen() == Scr::S_SLOT && S().itemCount() == 6);
    fresh();   // gate says the source is file 4: the row is there
    fake::spSession = 4;
    session(true, 0, MP_UI_PLAYING);
    frames(3);
    S().openSession(); frames(2);
    down(S().itemCount() - 1); ok(); frames(4);   // (Leave the Game: the last big button)
    CHECK(S().screen() == Scr::S_SLOT && S().itemCount() == 7);
    // the source file cannot be read (or is gone) now: not offered either
    fresh();
    fake::spSession = 4;
    fake::spBad = 1u << 3;
    session(true, 0, MP_UI_PLAYING);
    frames(3);
    S().openSession(); frames(2);
    down(S().itemCount() - 1); ok(); frames(4);   // (Leave the Game: the last big button)
    CHECK(S().screen() == Scr::S_SLOT && S().itemCount() == 6);
    // a recovery: the same
    fresh();
    fake::recovery = true; fake::recSingle = true;
    S().openMain(1); frames(3);
    CHECK(S().screen() == Scr::S_SLOT && S().itemCount() == 7);
    fresh();
    fake::recovery = true; fake::recSingle = false;
    S().openMain(1); frames(3);
    CHECK(S().screen() == Scr::S_SLOT && S().itemCount() == 6);
    fresh();
    fake::recovery = true; fake::recSingle = true; fake::spBad = 1u << 1;   // (the recovery's file 2 cannot be read)
    S().openMain(1); frames(3);
    CHECK(S().screen() == Scr::S_SLOT && S().itemCount() == 6);
}

// finding 1 (the menu's side): after multiplayer ends, the file screens show the single-player files from the single-player file,
// read again, never the co-op ones; a switch back that is still pending blocks play on my file
void testFilesAfterSession() {
    fresh();
    fake::coopFull = true;            // (co-op files read 7 stars, single-player files 5)
    fake::onMpRoot = true;
    S().onFileSelect();
    S().openMain(1); frames(3);
    toSave();
    fresh();
    fake::coopFull = true;
    fake::spMissing = 1u << 0;
    toSave(); down(6); ok();
    CHECK(S().screen() == Scr::S_SPFILE && rowValue(0) == "New" && rowValue(1).find('5') != std::string::npos && rowValue(1).find('7') == std::string::npos);
    for (int g = 0; g < 10 && S().isOpen(); g++) back();
    frames(5);
    CHECK(!S().isOpen());
    fake::spMissing = 0;   // (a session ran and the single-player file 1 was made meanwhile)
    toSave(); down(6); ok();
    CHECK(S().screen() == Scr::S_SPFILE && rowValue(0).find('5') != std::string::npos);   // read again, not the old answer
    // while the switch back is pending the menu does not start anything on a single-player file
    fresh();
    fake::onMpRoot = true;
    fake::leaveFails = 1000;
    S().onFileSelect();
    CHECK(S().singlePlayerGate());
    toSave(); down(2); ok(); ok(); down(); ok(); frames(3);
    CHECK(fake::backups == 0 && fake::spSession == 0);
    fake::leaveFails = 0;
    fake::onMpRoot = false;
}

// GUARANTEE 5: every Back leaves no half-made choice
void testFilesBackPaths() {
    const char* names[] = {"save", "spfile", "fileuse", "slot(copy)", "spfile(entry)"};
    for (const char* n : names) {
        fresh();
        for (const Path& p : kPaths)
            if (!strcmp(p.name, n)) p.go();
        CHECK(S().isOpen());
        const int a0 = fake::abandons;
        int guard = 0;
        while (S().isOpen() && guard++ < 20) back();
        CHECK(!S().isOpen() && fake::abandons > a0 && fake::spSession == 0 && fake::backups == 0);
        if (fake::abandons <= a0) fprintf(stderr, "  back path %s did not abandon the choice\n", n);
    }
    // a "Play on this file" that was asked and declined, then Back: nothing stays chosen
    fresh();
    toSave(); down(2); ok(); ok(); down();
    io.answer = DlgState::No;
    ok(); frames(3);
    io.answer = DlgState::Yes;
    const int a0 = fake::abandons;
    back(); back(); frames(3);
    CHECK(fake::abandons > a0 && fake::spSession == 0 && fake::backups == 0 && S().screen() != Scr::S_FILEUSE);
    // a start that fails after "Play on this file": the choice is forgotten
    fresh();
    toSave(); mp_gather_fake_remove_head(2); down(2); ok(); ok(); down(); ok(); frames(3);   // (one door: the friend left meanwhile)
    CHECK(fake::abandons >= 1);
}

// ---------------------------------------------------------------- Safe Copies (keepsafe API: share/keep/API.md)
static int ksCalls(const char* prefix) {
    int n = 0;
    for (auto& c : fake::ks.calls) n += c.rfind(prefix, 0) == 0;
    return n;
}
static std::string lineText() {   // the game's bottom message
    static MpPanel::Model m;
    S().buildModel(&m);
    return U8(m.line);
}
static void up(int n = 1) { for (int i = 0; i < n; i++) press(8); }
static void toKeep() { toSave(); press(3); }   // Which file? > Triangle: Safe Copies (uxfix #5: no longer a row)
static void clearTexts() { io.texts.clear(); }
static bool noOldWords() {   // the old design's words are gone from every window this run showed
    for (const char* w : {"Undo", "undo", "Found your multiplayer data", "interrupted", "Put Back", "Keep My Data"}) if (anyText(w)) return false;
    return true;
}

void testKeepScreens() {
    // (a) Make a Copy and Make a Copy to Share: no question; with no copy there is no Bring Back a Copy row (owner: no dead rows)
    fresh();
    toSave();
    // uxfix #5: not a row of Which File? and not on the Multiplayer menu: one quiet place, Which File?'s Triangle, in player words
    for (int i = 0; i < S().itemCount(); i++) CHECK(rowLabel(i) != "Safe Copies");
    {
        static MpPanel::Model sm;
        S().buildModel(&sm);
        bool tri = false;
        for (int i = 0; i < sm.buttonCount; i++) tri = tri || (sm.buttons[i].pic == MpMsgText::PIC_TRIANGLE && U8(sm.buttons[i].label) == "Safe Copies");
        CHECK(tri);
    }
    S().close(); fresh();
    openMain();
    for (int i = 0; i < S().itemCount(); i++) CHECK(rowLabel(i) != "Safe Copies");
    fresh();
    toSave();
    press(3);
    CHECK(S().screen() == Scr::S_KEEP);
    back();
    CHECK(S().screen() == Scr::S_SAVE);
    press(3);
    CHECK(S().screen() == Scr::S_KEEP);
    CHECK(S().screen() == Scr::S_KEEP && S().itemCount() == 2 && rowLabel(0) == "Make a Copy" && rowLabel(1) == "Make a Copy to Share");
    clearTexts();
    ok(); frames(3);
    CHECK(ksCalls("make:0") == 1 && Logged("ui: window MP_KEEP_MADE") && anyText("Saved a copy of your multiplayer data"));
    CHECK(anyText("Friend Code") && anyText("Make a Copy to Share") && windowFits(io.lastText));   // menus4: no alarm, where a copy for a friend comes from
    CHECK(io.lastText.find(".smgmp") == std::string::npos && io.lastText.find("/") == std::string::npos);   // (no file name, no path)
    CHECK(fake::ks.copies.size() == 1 && S().itemCount() == 3 && rowLabel(2) == "Bring Back a Copy");   // (the list has it now)
    clearTexts();
    fresh(); toKeep(); down(); ok(); frames(3);   // Make a Copy to Share
    CHECK(ksCalls("make:1") == 1 && fake::ks.lastFlags == MP_KS_COPY_NO_ACCOUNT && anyText("to share") && anyText("no Friend Code") && anyText("mp-backups") && windowFits(io.lastText));
    // nothing to copy / a failed write / a full card / busy: said plainly, nothing else happens
    struct Fail { int r; const char* word; };
    for (const Fail f : {Fail{MP_KS_NOTHING, "no multiplayer data"}, Fail{MP_KS_NO_SPACE, "couldn't be made"}, Fail{MP_KS_IO, "couldn't be made"}, Fail{MP_KS_VERIFY, "couldn't be made"}, Fail{MP_KS_BUSY, "try again"}}) {
        fresh();
        toKeep();
        fake::ks.makeRes = f.r;
        ok(); frames(3);
        CHECK(io.lastText.find(f.word) != std::string::npos && windowFits(io.lastText) && fake::ks.copies.empty());
    }
    // (b) the list: what made each copy (never a file name), its day, what it holds; newest first
    fresh();
    {
        MpKsCopy a = fake::ksCopy(MP_KS_COPY_BEFORE, "before-9.smgmp", 1794390000);
        a.account = 0, a.settings = 0;
        MpKsCopy b = fake::ksCopy(MP_KS_COPY_VERSION, "version-1", 1794380000);
        b.account = 1, b.settings = 7;
        MpKsCopy c = fake::ksCopy(MP_KS_COPY_MADE, "copy-3.smgmp", 1794300000);
        c.account = 1;
        fake::ks.copies = {a, b, c};
    }
    fake::ks.info.copyStars[1] = 12;
    toKeep();
    CHECK(S().itemCount() == 3 && rowLabel(2) == "Bring Back a Copy");
    down(2); ok();
    CHECK(S().screen() == Scr::S_KEEPLIST && S().itemCount() == 3);
    CHECK(rowLabel(0) == "Auto copy" && rowValue(0) == "2026-11-11" && rowLabel(1) == "Before 3.0.0" && rowLabel(2) == "Your copy" && rowValue(2) == "2026-11-10");
    CHECK(rowLabel(0).find(".smgmp") == std::string::npos);
    CHECK(hintLine() == "Holds: Team Files");
    down(); CHECK(hintLine() == "Holds: Team Files, Friend Code, Name and Character");
    down(); CHECK(hintLine() == "Holds: Team Files, Friend Code");
    // (c) one copy: ONLY what can be brought back. Team File 2 (in use here, has more stars here), 4 (already the same: not offered), 5 (empty here),
    // the online account, "My Settings" and "Menu Choices".
    fresh();
    fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_MADE, "copy-3.smgmp", 1794300000));
    fake::ks.copies.back().settings = 3;
    fake::ks.info.copyStars[1] = 12, fake::ks.info.copyStars[3] = 5, fake::ks.info.copyStars[4] = 7;
    fake::ks.info.liveStars[1] = 30, fake::ks.info.liveStars[3] = 5;
    fake::ks.info.slotInUse = 1u << 1 | 1u << 3, fake::ks.info.slotSame = 1u << 3, fake::ks.info.slotLacks = 1u << 1;
    fake::ks.info.account = 1, fake::ks.info.settings = MP_KS_SET_CONFIG | MP_KS_SET_FLOW;
    toKeep(); down(2); ok(); ok();
    CHECK(S().screen() == Scr::S_KEEPCOPY && S().itemCount() == 5);
    CHECK(rowLabel(0) == "Team File 2" && rowLabel(1) == "Team File 5" && rowLabel(2) == "Friend Code");
    down(3);   // (menus4: pages of three)
    CHECK(rowLabel(0) == "Name and Character" && rowLabel(1) == "Last Choices");
    down(2);
    CHECK(rowLabel(0) == "Team File 2");
    CHECK(rowValue(0).find("12") != std::string::npos);
    CHECK(hintLine() == "Here: 30 Power Stars. It is saved first.");
    down(); CHECK(hintLine().find("empty here") != std::string::npos);
    down(); CHECK(hintLine().find("Friend Code") != std::string::npos);
    up(2);
    // Team File 2: in use, this console's file has progress the copy lacks: asked; No changes nothing
    clearTexts();
    io.answer = DlgState::No;
    ok(); frames(3);
    io.answer = DlgState::Yes;
    CHECK(anyText("has progress the") && anyText("is saved first") && windowFits(io.lastText) && ksCalls("slot:") == 0);
    // Yes: ONE call, confirmed; then the result says where what was there went
    clearTexts();
    okDrive({DlgState::Yes});
    CHECK(ksCalls("slot:copy-3.smgmp:2:1") == 1 && ksCalls("slot:") == 1 && fake::ks.lastSlot == 2);
    CHECK(anyText("Team File 2 is back") && anyText("Auto copy") && noOldWords());
    for (auto& t : io.texts) CHECK(windowFits(t));
    CHECK(S().screen() == Scr::S_KEEPCOPY && S().itemCount() == 4 && rowLabel(0) == "Team File 5");   // (the one brought back is the same now: not offered)
    CHECK(fake::ks.copies.size() == 2 && fake::ks.copies[0].kind == MP_KS_COPY_BEFORE);              // (the undo is in the list)
    // Team File 5: nothing there: NO question, called with confirmed = 0
    clearTexts();
    ok(); frames(3);
    CHECK(ksCalls("slot:copy-3.smgmp:5:0") == 1 && anyText("Team File 5 is back") && !anyText("Auto copy"));
    // the online account: always asked; No: nothing; Yes: brought back
    clearTexts();
    io.answer = DlgState::No;
    ok(); frames(3);
    io.answer = DlgState::Yes;
    CHECK(anyText("Friend Code") && anyText("Use it on this console") && ksCalls("account:") == 0);
    okDrive({DlgState::Yes});
    CHECK(ksCalls("account:copy-3.smgmp:1") == 1 && anyText("Friend Code changed"));
    // settings, one file at a time
    CHECK(S().screen() == Scr::S_KEEPCOPY && S().itemCount() == 2 && rowLabel(0) == "Name and Character");
    clearTexts();
    okDrive({DlgState::Yes});
    CHECK(anyText("name and character") && ksCalls("settings:copy-3.smgmp:1") == 1 && fake::ks.lastWhich == MP_KS_SET_CONFIG && anyText("Auto copy"));
    CHECK(rowLabel(0) == "Last Choices");
    clearTexts();
    okDrive({DlgState::Yes});
    CHECK(anyText("remembered choices") && fake::ks.lastWhich == MP_KS_SET_FLOW && ksCalls("settings:") == 2);
    // nothing left in the copy: back at the list
    CHECK(S().screen() == Scr::S_KEEPLIST);
    // a Team File in use here that the copy does not lack: the plain question
    fresh();
    fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_VERSION, "version-1", 1794300000));
    fake::ks.info.copyStars[0] = 40, fake::ks.info.liveStars[0] = 3, fake::ks.info.slotInUse = 1;
    toKeep(); down(2); ok(); ok();
    clearTexts();
    okDrive({DlgState::Yes});
    CHECK(anyText("Replace Team File 1 with the one") && ksCalls("slot:version-1:1:1") == 1);
    // the module itself asks for a confirmation we did not expect (a file became "in use" meanwhile): the question, then the call again
    fresh();
    fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_MADE, "c.smgmp", 1794300000));
    fake::ks.info.copyStars[2] = 9;
    toKeep(); down(2); ok(); ok();
    fake::ks.info.slotInUse = 1u << 2;   // (changed after the screen was read)
    clearTexts();
    okDrive({DlgState::Yes});
    CHECK(ksCalls("slot:c.smgmp:3:0") == 1 && ksCalls("slot:c.smgmp:3:1") == 1 && anyText("Replace Team File 3"));
    // (d) a damaged copy / one from a newer game: refused before anything is offered
    for (int r : {MP_KS_BAD_FILE, MP_KS_NEWER}) {
        fresh();
        fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_MADE, "c.smgmp", 1794300000));
        toKeep(); down(2); ok();
        fake::ks.inspectRes = r;
        clearTexts();
        ok(); frames(3);
        CHECK(S().screen() == Scr::S_KEEPLIST && anyText(r == MP_KS_NEWER ? "newer game" : "damaged") && ksCalls("slot:") == 0);
    }
    // nothing in the copy to bring back
    fresh();
    fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_MADE, "c.smgmp", 1794300000));
    toKeep(); down(2); ok();
    clearTexts();
    ok(); frames(3);
    CHECK(S().screen() == Scr::S_KEEPLIST && anyText("nothing in this copy") && windowFits(io.lastText));
    // a failed call: said, nothing changed, the row stays
    struct Bad { int r; const char* word; };
    for (const Bad b : {Bad{MP_KS_IO, "nothing was changed"}, Bad{MP_KS_VERIFY, "nothing was changed"}, Bad{MP_KS_NO_SPACE, "enough room"}, Bad{MP_KS_BUSY, "try again"}, Bad{MP_KS_CONFIRM + 1, "too many saved copies"}}) {
        fresh();
        fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_MADE, "c.smgmp", 1794300000));
        fake::ks.info.copyStars[1] = 9;
        toKeep(); down(2); ok(); ok();
        fake::ks.slotRes = b.r;
        clearTexts();
        ok(); frames(3);
        CHECK(anyText(b.word) && windowFits(io.lastText) && !anyText("is back"));
        CHECK(S().screen() == Scr::S_KEEPCOPY && S().itemCount() == 1);
    }
    // never while a multiplayer save image cannot be reloaded, or a leave decision is pending: no call at all
    fresh();
    fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_MADE, "c.smgmp", 1794300000));
    fake::ks.info.copyStars[1] = 9;
    toKeep(); down(2); ok(); ok();
    fake::leavePending = true;
    clearTexts();
    ok(); frames(3);
    CHECK(ksCalls("slot:") == 0 && anyText("try again"));
    fake::leavePending = false;
    fake::onMpRoot = true; fake::leaveFails = 1000;
    ok(); frames(3);
    CHECK(ksCalls("slot:") == 0);
    fake::onMpRoot = false; fake::leaveFails = 0;
    // Back from the copy, the list and the screen: nothing written
    back(); CHECK(S().screen() == Scr::S_KEEPLIST);
    back(); CHECK(S().screen() == Scr::S_KEEP);
    back(); CHECK(S().screen() == Scr::S_SAVE);
    CHECK(ksCalls("slot:") == 0 && ksCalls("make:") == 0 && fake::backups == 0 && fake::spSession == 0);
}

// keepsafe API, "the exact order at entry": every entry to the Multiplayer menu takes the notice once (a card with OK), then asks
// may_migrate(); false puts a line on the Multiplayer menu and on Safe Copies for the whole run, with "Make a Copy" first and marked
void testKeepEntry() {
    // no notice, copies fine: nothing shown
    fresh();
    openMain(); frames(10);
    CHECK(io.texts.empty() && ksCalls("notice") == 0 && lineText().find("No copy from before") == std::string::npos);
    // the copy at the update could not be made: a card, once
    fresh();
    fake::ks.notice = MP_KS_NOTICE_NO_COPY;
    openMain(); frames(8);
    CHECK(Logged("ui: window MP_KS_NOTICE") && anyText("could not be copied before") && anyText("tried again at the next start") && windowFits(io.texts[0]) && !anyText("full"));
    CHECK(ksCalls("notice") == 1);
    frames(10);
    CHECK(io.texts.size() == 1);   // (not asked again this visit)
    // the card is full: the old words, and only then
    fresh();
    fake::ks.notice = 2;   // MP_KS_NOTICE_NO_SPACE
    openMain(); frames(8);
    CHECK(anyText("memory card is full") && windowFits(io.texts[0]));
    // may_migrate() false: no standing line any more (uxfix #5: the card above says it once; the Multiplayer menu keeps its own line)
    fresh();
    fake::ks.noMigrate = true;
    openMain(); frames(8);
    CHECK(io.texts.empty());   // (no notice: no card)
    CHECK(lineText().find("No copy from before this update") == std::string::npos && lineText().find("Make a copy now") == std::string::npos);
    for (int i = 0; i < S().itemCount(); i++) CHECK(rowLabel(i) != "Safe Copies");
    // never during a session
    fresh();
    session(true, 0, MP_UI_LOBBY);
    fake::ks.notice = MP_KS_NOTICE_NO_COPY;
    openMain(); frames(8);
    CHECK(ksCalls("notice") == 0 && io.texts.empty());
}
