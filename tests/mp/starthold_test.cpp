// Host test of MpStartHold::decide (merge fix 2): the single-player truth table equals the old isReadyToStartScene, and the session rules.
#include "Game/MP/MpStartHold.hpp"
#include <cstdio>
#include <initializer_list>
#include <cstdlib>
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

// The old code (before the merge fix), for a console without a session:  if (ready && warm) return false;  return ready;
static bool oldReady(bool ready, bool warm) { if (ready && warm) return false; return ready; }

int main() {
    using namespace MpStartHold;
    int rows = 0;
    std::printf("single player truth table: ready warm -> old new (pump send)\n");
    for (int r = 0; r < 2; r++) for (int w = 0; w < 2; w++) for (int held : {0, 4999, 5000, 12000}) for (int b = 0; b < 2; b++) {
        const Out o = decide({bool(r), false, bool(w), held, bool(b)});
        CHECK(o.ready == oldReady(r, w));
        CHECK(!o.pump && !o.sendLoaded);   // no session: nothing is pumped or sent from here
        rows++;
        if (held == 0 && b == 0) std::printf("  %d %d -> %d %d (%d %d)\n", r, w, oldReady(r, w), o.ready, o.pump, o.sendLoaded);
    }
    std::printf("%d single-player rows equal the old code\n", rows);

    // session: not ready -> nothing
    for (int w = 0; w < 2; w++) { const Out o = decide({false, true, bool(w), 0, false}); CHECK(!o.ready && !o.pump && !o.sendLoaded); }
    // session, ready, no warm hold: pump, send LOADED at once, ready unless the barrier holds
    { Out o = decide({true, true, false, 0, false}); CHECK(o.ready && o.pump && o.sendLoaded); }
    { Out o = decide({true, true, false, 0, true});  CHECK(!o.ready && o.pump && o.sendLoaded); }
    // session, warm hold in force under the cap: pumped every frame, LOADED not sent, never ready (barrier answer irrelevant)
    for (int held : {0, 1, 4999}) for (int b = 0; b < 2; b++) { Out o = decide({true, true, true, held, bool(b)}); CHECK(!o.ready && o.pump && !o.sendLoaded); }
    // session, warm hold at / past the cap (5 s): proceed to the barrier: LOADED sent, ready when the barrier lets go
    for (int held : {5000, 12000}) {
        Out o = decide({true, true, true, held, true});  CHECK(!o.ready && o.pump && o.sendLoaded);
        o = decide({true, true, true, held, false});     CHECK(o.ready && o.pump && o.sendLoaded);
    }
    // the barrier holding the screen while the hold is in force beyond the cap: still waiting on the barrier, not on the hold
    CHECK(kWarmHoldCapMs == 5000);
    std::printf("starthold_test OK\n");
    return 0;
}
