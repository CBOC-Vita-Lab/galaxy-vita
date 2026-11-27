// MP3 lodgate: host test of platform/include/port/mp_gate.h (the rules that hold the state-affecting speed features at their neutral setting in a multiplayer session).
// tools/test_mpgate.sh builds it with ASan + UBSan. Exit 0 = all pass.
#include "port/mp_gate.h"

#include <stdio.h>
#include <string.h>

using namespace port::mpgate;

static int sFail = 0, sChecks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        ++sChecks;                                                                \
        if (!(c)) {                                                               \
            ++sFail;                                                              \
            if (sFail < 40) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
        }                                                                         \
    } while (0)

static void TestDecor() {
    const float hs[] = {0.0f, 1500.0f, 4500.0f, 5000.0f, 7000.0f, 30000.0f};
    for (float h : hs) {
        CHECK(DecorHorizon(h, true) == 0.0f);   // in a session: no horizon whatever the budget / step / tier asked
        CHECK(DecorHorizon(h, false) == h);     // outside: untouched, bit for bit
    }
}
static void TestPose() {
    for (int n = 0; n < 6; ++n) {
        CHECK(PoseCutAllowed(false, n));              // single player: every actor may be cut, as before
        CHECK(PoseCutAllowed(true, 0));               // no hit sensor: nothing reads the pose, still a visual cut
        if (n > 0) CHECK(!PoseCutAllowed(true, n));   // a hit sensor: posed every turn in a session
    }
}
static void TestAlodIdleXskip() {
    CHECK(AlodGateOpen(true, false) && !AlodGateOpen(false, false));
    CHECK(!AlodGateOpen(true, true) && !AlodGateOpen(false, true));
    for (int m = 0; m <= 2; ++m) {
        CHECK(IdleMode(m, false) == m);
        CHECK(IdleMode(m, true) == 0);
    }
    CHECK(CoverHoldAllowed(false) && !CoverHoldAllowed(true));
}
static void TestSessionHeld() {
    // the states of the session (active, reconnecting, dyingReconnect, rejoining) -> held. Idle / scan / fresh join / leaving / ended / gave up / suspended: all four false.
    CHECK(!SessionHeld(false, false, false, false));   // idle, or a menu scan, a fresh join or host without a world, LEAVING, a deliberate end, a give-up, asleep
    CHECK(SessionHeld(true, false, false, false));     // hosting / joined, lobby or playing
    CHECK(SessionHeld(false, true, false, false));     // reconnecting (active is false)
    CHECK(SessionHeld(false, false, true, false));     // the link just failed: requestEnd ran, the reconnect not yet started
    CHECK(SessionHeld(false, false, false, true));     // a rejoin after a sleep is in flight
    for (int bits = 0; bits < 16; ++bits)              // the whole table: held = any of the four
        CHECK(SessionHeld(bits & 1, bits & 2, bits & 4, bits & 8) == (bits != 0));
    // the gates: single player (not held) is the old behaviour bit for bit; a reconnect (held, active false) is the session's
    CHECK(AnimDeferAllowed(false, 0) && AnimDeferAllowed(false, 3));   // single player: any actor may be deferred
    CHECK(AnimDeferAllowed(true, 0));                                  // held, no hit sensor: visual only, still deferred
    CHECK(!AnimDeferAllowed(true, 1) && !AnimDeferAllowed(true, 7));   // held, a hit sensor: never deferred
    CHECK(!SkipHoldWaits(false, false, false) && !SkipHoldWaits(false, false, true));   // single player: the hold skips as always
    CHECK(!SkipHoldWaits(true, true, false) && !SkipHoldWaits(true, true, true));       // active: the vote path decides, not this
    CHECK(SkipHoldWaits(true, false, false));                                           // reconnecting: waits
    CHECK(!SkipHoldWaits(true, false, true));                                           // reconnecting, dialogue: local as always
    // "following, still skipping" (cutscene_skip.cpp) asks the same rule: a reconnect starts no local skip of the next part, active continues the voted skip, dialogue stays local
    CHECK(SkipHoldWaits(true, false, false) && !SkipHoldWaits(true, true, false) && !SkipHoldWaits(false, false, false) && !SkipHoldWaits(true, false, true));
    CHECK(PlantRestAllowed(false) && !PlantRestAllowed(true));
    CHECK(StarBatchFarAllowed(false) && !StarBatchFarAllowed(true));
    // a reconnect does not re-arm the log lines: held stays true through the outage, the log is only reset when not held
    SessionLog l;
    for (int f = 0; f < kFeatureN; ++f) CHECK(l.Due(f, true));
    for (int step = 0; step < 100; ++step)             // 30 s of reconnect: the gate sites keep asking with held = true
        for (int f = 0; f < kFeatureN; ++f) CHECK(!l.Due(f, SessionHeld(false, true, false, false)));
    l.Reset();
    for (int f = 0; f < kFeatureN; ++f) CHECK(l.Due(f, SessionHeld(true, false, false, false)));
}
static void TestLog() {
    SessionLog l;
    for (int f = 0; f < kFeatureN; ++f) CHECK(!l.Due(f, false));   // no session: never a line
    for (int f = 0; f < kFeatureN; ++f) {
        CHECK(l.Due(f, true));                                       // first time in the session: one line
        CHECK(!l.Due(f, true) && !l.Due(f, true));                   // not again
    }
    CHECK(!l.Due(-1, true) && !l.Due(kFeatureN, true));
    CHECK(!l.Due(kDecor, false));                                    // the session ends: re-armed
    CHECK(l.Due(kDecor, true) && !l.Due(kDecor, true));              // the next session says it again
    CHECK(!l.Due(kPose, true));                                      // (another feature is not re-armed by this one's turn)
    l.Reset();
    for (int f = 0; f < kFeatureN; ++f) CHECK(l.Due(f, true));
}
static void TestReach() {
    for (int level = 1; level <= 5; ++level) {
        CHECK(StarBitReachLevel(level, false) == level);   // outside a Star Race: the stored choice, untouched
        CHECK(StarBitReachLevel(level, true) == 1);        // in a Star Race: the game's own
    }
}
static void TestNames() {
    for (int f = 0; f < kFeatureN; ++f) {
        CHECK(strlen(Name(f)) > 3 && strlen(Why(f)) > 10 && strlen(Neutral(f)) > 3);
        CHECK(strcmp(Name(f), "?") != 0);
    }
    CHECK(strcmp(Name(99), "?") == 0 && strcmp(Why(-1), "?") == 0);
}

int main() {
    TestDecor();
    TestPose();
    TestAlodIdleXskip();
    TestSessionHeld();
    TestLog();
    TestReach();
    TestNames();
    printf("mpgate: %d checks, %d failed\n", sChecks, sFail);
    return sFail ? 1 : 0;
}
