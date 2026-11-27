// Galaxy 3.0 multiplayer (gamefix): host tests for the save gate's state machine (decomp/include/Game/MP/MpSaveGate.hpp) with the
// real mp_save_policy. A fake "disk" (the single-player file, the co-op files) and a fake autosave nerve: a save writes the live
// image into the playing file unless MpSaveGate::suppressed says no. The owner's rule under test: the single-player file's hash
// never changes without an explicit choice, a guest never writes, a stale image is never written.
//   (built by tests/mp/savecheck_build.sh; g++ -std=c++20 -fsanitize=address,undefined -Iplatform/include -Idecomp/include
//    tests/mp/savegate_test.cpp platform/src/mp/mp_savefile.cpp -pthread)
#include "Game/MP/MpSaveGate.hpp"

#include <stdio.h>
#include <string>

static int sChecks = 0, sFails = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        sChecks++;                                                       \
        if (!(c)) {                                                      \
            sFails++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                                \
    } while (0)

using namespace MpSaveGate;

struct Sim {
    State g;
    // the net thread's view
    bool net = false;
    int localSlot = 0, hostSlot = 0;
    unsigned epoch = 0;
    bool rejoining = false;
    // disk and RAM
    std::string spFile = "SP-ORIGINAL", coopFile = "COOP-ORIGINAL", image = "SP-ORIGINAL";
    int starts = 0, ends = 0, noChoiceLogs = 0;
    bool flipAfterPolicy = false;   // the net thread ends the session between the latch and the policy read

    bool netActive() {
        const bool a = net;
        return a;
    }
    bool reconnecting() { return rejoining; }
    bool localIsOriginalHost() { return localSlot == 0 && epoch == 0; }
    int policy() {
        const int p = mp_save_policy(net, localSlot, hostSlot, epoch, false);
        if (flipAfterPolicy) { net = false; flipAfterPolicy = false; }
        return p;
    }
    void onStart() { starts++; }
    void onResumed() {}
    void onEnd() { ends++; }
    void onEnded(bool) {}
    void logNoChoice() { noChoiceLogs++; }

    // the game's save nerve (what SaveDataHandleSequence's hook does): writes only when not suppressed
    void autosave() {
        if (suppressed(g, *this)) return;
        (g.onMpRoot ? coopFile : spFile) = image;
    }
    void tick() { sync(g, *this); }   // MpSaveGame::tick, later in the scene update
    void startSession(int slot) { net = true; localSlot = slot; hostSlot = 0; epoch = 0; tick(); }
};

// 3: "Play on my file", then the start fails; a Quick Match hosting session follows
static void testSpChoiceLeak() {
    Sim s;
    beginSinglePlayer(s.g, 2);
    CHECK(hostWriteChosen(s.g));
    abandonChoice(s.g);                       // mp_ui_start() failed / lobby left / file select
    CHECK(!hostWriteChosen(s.g) && s.g.spSessionSlot == 0);
    s.image = "SP-WITH-SESSION-PROGRESS";
    s.startSession(0);                        // Quick Match: this console hosts, no choice made
    s.autosave();
    CHECK(s.spFile == "SP-ORIGINAL" && s.coopFile == "COOP-ORIGINAL");
    CHECK(s.noChoiceLogs == 1);               // the "host without a choice: saves held" guard is on
    CHECK(s.g.progressInRam);
    // without the fix (no abandon) the stale choice would have let the save through: the gate alone cannot know
    Sim t;
    beginSinglePlayer(t.g, 2);
    t.image = "SP-WITH-SESSION-PROGRESS";
    t.startSession(0);
    t.autosave();
    CHECK(t.spFile == "SP-WITH-SESSION-PROGRESS");   // (documents why every failure path must call abandonChoice)
    // a choice a session ran on is not abandoned by the lobby / file select calls
    Sim u;
    beginSinglePlayer(u.g, 3);
    u.startSession(0);
    abandonChoice(u.g);
    CHECK(u.g.spSessionSlot == 3);
    // a console that is not the original host never keeps a single-player choice
    Sim v;
    beginSinglePlayer(v.g, 4);
    v.startSession(1);
    CHECK(v.g.spSessionSlot == 0);
}

// 4: the net thread ends the session mid-frame; a save nerve runs before tick() latches it
static void testRaceBeforeTick() {
    // a guest: image holds the host's progress in all six co-op files
    Sim s;
    s.g.onMpRoot = true;
    s.image = "HOST-IMAGE";
    s.startSession(2);
    s.autosave();
    CHECK(s.coopFile == "COOP-ORIGINAL");
    s.net = false;                            // session over, tick() has not run yet
    s.autosave();
    CHECK(s.coopFile == "COOP-ORIGINAL");     // the old gate returned sHeldAfterSession (still false) here and wrote
    CHECK(s.g.heldAfterSession);
    s.tick();
    s.autosave();
    CHECK(s.coopFile == "COOP-ORIGINAL");
    // a host without a choice on the single-player root
    Sim h;
    h.image = "SP+SESSION";
    h.startSession(0);
    h.net = false;
    h.autosave();
    CHECK(h.spFile == "SP-ORIGINAL" && h.g.heldAfterSession);
    // the call order inside the frame does not matter
    Sim a, b;
    for (Sim* x : {&a, &b}) { x->g.onMpRoot = true; x->startSession(1); x->net = false; }
    a.tick(); const bool a1 = suppressed(a.g, a);
    const bool b1 = suppressed(b.g, b); b.tick();
    CHECK(a1 && b1 && a.g.heldAfterSession == b.g.heldAfterSession);
    // the session ends between the latch and the policy read
    Sim f;
    f.g.onMpRoot = true;
    f.startSession(1);
    f.flipAfterPolicy = true;
    CHECK(suppressed(f.g, f));
    CHECK(f.g.heldAfterSession && f.ends == 1);
    // a host WITH a choice may still save when the session is over (its own file), and during it
    Sim c;
    c.g.onMpRoot = true;
    c.image = "COOP-PLAYED";
    c.startSession(0);
    chooseCoop(c.g);                          // savefix: the lobby's Start entered the co-op root for this session
    c.autosave();
    CHECK(c.coopFile == "COOP-PLAYED");
    c.net = false;
    c.image = "COOP-PLAYED-2";
    c.autosave();
    CHECK(c.coopFile == "COOP-PLAYED-2" && !c.g.heldAfterSession);
}

// 7: a stale image on the multiplayer root
static void testStaleImage() {
    Sim s;
    s.g.onMpRoot = true;
    s.image = "PREVIOUS-SESSION-IMAGE";
    s.startSession(1);                        // a guest session, progress in RAM
    s.net = false;
    s.tick();
    CHECK(s.g.heldAfterSession && s.g.onMpRoot);
    // the switch back to the single-player root failed; the player picks "Continue co-op file N": entering the mp root while
    // already on it must reload when the image is stale
    CHECK(!switchIsNoop(s.g, true));
    CHECK(switchIsNoop(s.g, false) == false);   // (leaving is not a no-op while on the mp root)
    // even if a start path skipped the reload, the stale image is never written, by a host with a choice either
    s.startSession(0);
    s.autosave();
    CHECK(s.coopFile == "COOP-ORIGINAL");
    s.net = false; s.tick();
    // after the reload it is clean and writes are allowed again for the chosen file
    imageReloaded(s.g);
    s.image = "FRESH";
    s.startSession(0);
    chooseCoop(s.g);
    s.autosave();
    CHECK(s.coopFile == "FRESH");
    Sim n;
    CHECK(switchIsNoop(n.g, false) && !switchIsNoop(n.g, true));   // plain cases unchanged
    n.g.onMpRoot = true;
    CHECK(switchIsNoop(n.g, true));
}

// the original behaviour that must stay: no session, no hold
static void testPlain() {
    Sim s;
    s.image = "SP-PLAYED";
    s.autosave();
    CHECK(s.spFile == "SP-PLAYED" && s.starts == 0 && s.ends == 0);
    s.startSession(0);
    CHECK(s.starts == 1);
    s.net = false; s.tick();
    CHECK(s.ends == 1 && s.g.heldAfterSession);   // host without a choice: held until reload
    s.autosave();
    CHECK(s.spFile == "SP-PLAYED");
    imageReloaded(s.g);
    s.image = "SP-PLAYED-2";
    s.autosave();
    CHECK(s.spFile == "SP-PLAYED-2");
}

// 5: writing progress into a single-player file: warn on more stars there, or a newer file
static void testSingleOverwriteWarning() {
    CHECK(singleOverwriteWarning(3, 9, false));    // the file has more Power Stars
    CHECK(singleOverwriteWarning(9, 9, true));     // same stars, but the file was saved after the session
    CHECK(singleOverwriteWarning(3, 9, true));
    CHECK(!singleOverwriteWarning(9, 9, false) && !singleOverwriteWarning(9, 3, false));
    CHECK(!singleOverwriteWarning(0, -1, false));  // unknown file: no extra question (the backup + confirm still run)
}

// review (rev-save): the production hook is NOT suppressed() alone. SaveDataHandleSequence.cpp:29 is
//   mpSaveSuppressed() = (mp_active() || MpSaveGame::heldAfterSession()) && MpSaveGame::savesSuppressed()
// and GameSystem.cpp:603/615 runs the save sequence (mSequenceDirector->update) BEFORE the scene (GameScene -> MpGame::
// onGameSceneUpdate -> MpSaveGame::tick). So between the net thread ending the session and the next tick, every save nerve in
// that frame evaluates the wrapper with mp_active()==false and heldAfterSession still false: it short-circuits and the save runs.
struct ProdSim : Sim {
    // savefix: the hook is now exactly MpSaveGame::savesSuppressed() (decomp/include/Game/MP/MpSaveHook.hpp, no pre-check); the
    // real expression is driven against the real MpSaveGame.cpp by tests/mp/savelife_test.cpp. Was:
    //   (netActive() || g.heldAfterSession) && suppressed(g, *this)    -> fails the two checks below
    bool prodSuppressed() { return suppressed(g, *this); }
    void prodAutosave() {
        if (prodSuppressed()) return;
        (g.onMpRoot ? coopFile : spFile) = image;
    }
};
static void testProductionWrapperRace() {
    // a host without a save choice (straight into a running room / Quick Match) plays its single-player file on the sp root
    ProdSim h;
    h.image = "SP+SESSION-PROGRESS";
    h.startSession(0);
    h.prodAutosave();
    CHECK(h.spFile == "SP-ORIGINAL");          // during the session: held (passes)
    h.net = false;                             // net thread: session over; this frame's save nerve runs before tick()
    h.prodAutosave();
    CHECK(h.spFile == "SP-ORIGINAL");          // FAILS: the single-player file now holds the session's progress
    // a guest on the multiplayer root: the host's progress over the co-op file
    ProdSim gst;
    gst.g.onMpRoot = true;
    gst.image = "HOST-IMAGE";
    gst.startSession(2);
    gst.net = false;
    gst.prodAutosave();
    CHECK(gst.coopFile == "COOP-ORIGINAL");    // FAILS the same way
}

// savefix: the co-op root counts as the host's choice only when entered for this session; an undecided earlier session does not
// hand its choice on; a lost source clears "Play on my file"
static void testSavefixChoices() {
    Sim s;
    s.g.onMpRoot = true;                       // on the co-op root from an earlier session (switch back still pending)
    s.image = "NO-CHOICE-PROGRESS";
    s.startSession(0);                         // a host straight into a running room
    s.autosave();
    CHECK(s.coopFile == "COOP-ORIGINAL");      // not a choice: held
    s.net = false;
    s.autosave();
    CHECK(s.coopFile == "COOP-ORIGINAL" && s.g.heldAfterSession);
    Sim u;
    beginSinglePlayer(u.g, 3);
    u.startSession(0);
    u.net = false;
    u.tick();                                  // ends undecided (no saveOnLeave)
    CHECK(u.g.spSessionSlot == 3);             // (still offered for the leave decision)
    u.startSession(0);                         // the next session starts without a decision
    CHECK(u.g.spSessionSlot == 0 && !hostWriteChosen(u.g));
    beginSinglePlayer(u.g, 4);
    sourceLost(u.g);
    CHECK(u.g.spSessionSlot == 0);
}

int main() {
    testSavefixChoices();
    testProductionWrapperRace();
    testSingleOverwriteWarning();
    testSpChoiceLeak();
    testRaceBeforeTick();
    testStaleImage();
    testPlain();
    printf("savegate_test: %d checks, %d failures\n", sChecks, sFails);
    return sFails ? 1 : 0;
}
