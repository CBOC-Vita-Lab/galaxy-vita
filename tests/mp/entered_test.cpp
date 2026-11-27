// optfix: the "multiplayer entered" flag is cleared again when the session is over and nothing is pending (MpEnteredLogic.hpp, and the
// inline flag functions of port/mp_bridge.h). The key proof: single player after leaving multiplayer executes the SAME early-out as a boot
// that never entered (the per-frame hook's work counter does not move), and nothing that must go on is cut short.
//   bash tests/mp/entered_build.sh
#include <stdio.h>

#include "Game/MP/MpEnteredLogic.hpp"
#include "port/mp_bridge.h"

static int gChecks, gFails;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace MpEnteredLogic;

// The game's per-frame hook, as MpGame::onGameSceneUpdate has it: the flag first, then the work (a dozen calls).
static long gHookWork = 0;
static void perFrameHook() {
    if (!mp_entered()) return;
    gHookWork++;
}

// One game turn of the MpGame side with a retire object, like retireEntered().
struct Game {
    Retire r;
    bool coreIdle = true;
    int asked = 0;
    bool turn(Inputs in) {
        perFrameHook();
        const bool go = r.turn(in, [&] { asked++; return coreIdle; });
        if (go) mp_retire_entered();
        return go;
    }
};

static Inputs quiet() { return Inputs{false, true, true, false, false}; }

int main() {
    // a boot that never entered: the hook does nothing
    CHECK(!mp_entered() && !mp_ever_entered());
    for (int i = 0; i < 1000; i++) perFrameHook();
    CHECK(gHookWork == 0);
    const long neverEnteredWork = gHookWork;

    // the player opens the menu, plays a session, leaves
    mp_mark_entered();
    CHECK(mp_entered() && mp_ever_entered());
    Game g;
    Inputs session = quiet();
    session.sessionActive = true;
    for (int i = 0; i < 600; i++) CHECK(!g.turn(session));
    CHECK(mp_entered() && gHookWork == 600);
    // the session ends: the toast, the leave question: a menu / save decision pending keeps the flag
    Inputs menu = quiet();
    menu.uiSettled = false;
    for (int i = 0; i < 500; i++) CHECK(!g.turn(menu));
    Inputs held = quiet();
    held.saveSettled = false;
    for (int i = 0; i < 500; i++) CHECK(!g.turn(held));
    Inputs ret = quiet();
    ret.returnPending = true;
    for (int i = 0; i < 500; i++) CHECK(!g.turn(ret));
    CHECK(mp_entered() && g.asked == 0);   // (the core is not even asked while anything is pending)
    // all quiet: the flag goes after exactly kQuietTurns turns
    int turns = 0;
    bool gone = false;
    while (!gone && turns < 10000) { gone = g.turn(quiet()); turns++; }
    CHECK(gone && turns == kQuietTurns && !mp_entered() && mp_ever_entered());
    CHECK(g.asked == 1);
    // single player after leaving multiplayer: the hook's work counter no longer moves (the same early-out as the never-entered boot)
    const long before = gHookWork;
    for (int i = 0; i < 5000; i++) g.turn(quiet());
    CHECK(gHookWork == before);
    CHECK(gHookWork - before == neverEnteredWork);

    // a core that is not idle (a sign-in, a scan the menu left): the flag stays, asked again a second later
    mp_mark_entered();
    Game h;
    h.coreIdle = false;
    for (int i = 0; i < kQuietTurns + 3 * kRetryTurns; i++) h.turn(quiet());
    CHECK(mp_entered() && h.asked == 1 + 3);
    h.coreIdle = true;
    bool done = false;
    for (int i = 0; i < kRetryTurns + 1 && !done; i++) done = h.turn(quiet());
    CHECK(done && !mp_entered());

    // anything shown during the wait starts it again
    mp_mark_entered();
    Game k;
    for (int i = 0; i < kQuietTurns - 1; i++) CHECK(!k.turn(quiet()));
    CHECK(!k.turn(menu));                  // a toast at turn 179
    CHECK(k.r.quietTurns() == 0 && mp_entered());
    for (int i = 0; i < kQuietTurns - 1; i++) CHECK(!k.turn(quiet()));
    CHECK(k.turn(quiet()) && !mp_entered());

    // the autopilot / demo never retire
    mp_mark_entered();
    Game a;
    Inputs auto_ = quiet();
    auto_.demoOrAutopilot = true;
    for (int i = 0; i < 5000; i++) CHECK(!a.turn(auto_));
    CHECK(mp_entered());

    // the menu opens again later: the flag is set again, the hooks run again, and the sticky flag was never lost
    mp_retire_entered();
    CHECK(!mp_entered() && mp_ever_entered());
    mp_mark_entered();
    const long w0 = gHookWork;
    perFrameHook();
    CHECK(mp_entered() && gHookWork == w0 + 1);
    printf("entered_test: %d checks, %d failed\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
