// Host test of MpWaitLogic::Tracker (uxwait): the "Waiting for <name>..." banner shows only after a wait of about 2 s, goes at once, follows the host.
#include "Game/MP/MpWaitLogic.hpp"
#include <cstdio>
#include <cstdlib>
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

int main() {
    using namespace MpWaitLogic;
    CHECK(kShowAfterMs == 2000);
    { // a short wait is never announced
        Tracker t;
        for (int64_t ms = 0; ms < kShowAfterMs; ms += 16) CHECK(!t.step(true, 1, 1000 + ms).show);
        CHECK(!t.step(false, 1, 3016).show);
    }
    { // shown from 2 s, with the name's slot; gone the frame the wait ends
        Tracker t;
        CHECK(!t.step(true, 2, 0).show);
        CHECK(!t.step(true, 2, 1999).show);
        Out o = t.step(true, 2, 2000);
        CHECK(o.show && o.slot == 2);
        o = t.step(true, 2, 50000);
        CHECK(o.show && o.slot == 2);
        CHECK(!t.step(false, 2, 50016).show);
    }
    { // a new wait starts its own clock
        Tracker t;
        t.step(true, 1, 0);
        CHECK(t.step(true, 1, 2500).show);
        CHECK(!t.step(false, 1, 2516).show);
        CHECK(!t.step(true, 1, 2532).show);
        CHECK(!t.step(true, 1, 4531).show);
        CHECK(t.step(true, 1, 4532).show);
    }
    { // nobody known to wait for: never shown, and the clock does not run
        Tracker t;
        CHECK(!t.step(true, -1, 0).show);
        CHECK(!t.step(true, -1, 9000).show);
        CHECK(!t.step(true, 3, 9016).show);
        CHECK(t.step(true, 3, 11016).show);
    }
    { // the host changes while waiting (migration): the banner stays and names the new host
        Tracker t;
        t.step(true, 0, 0);
        Out o = t.step(true, 0, 2100);
        CHECK(o.show && o.slot == 0);
        o = t.step(true, 2, 2116);
        CHECK(o.show && o.slot == 2);
    }
    { // the bubbled player: the first loading teammate is named; the banner needs 2 s; it goes when nobody loads (pop / restart)
        const bool none[4] = {false, false, false, false};
        const bool two[4] = {false, false, true, true};
        CHECK(firstLoadingSlot(none, 4) == -1);
        CHECK(firstLoadingSlot(two, 4) == 2);
        Tracker t;
        int who = firstLoadingSlot(two, 4);
        CHECK(!t.step(who >= 0, who, 0).show);
        CHECK(!t.step(who >= 0, who, 1999).show);
        Out o = t.step(who >= 0, who, 2000);
        CHECK(o.show && o.slot == 2);
        const bool next[4] = {false, false, false, true};   // slot 2 arrived, slot 3 still loads: the name follows, the clock goes on
        who = firstLoadingSlot(next, 4);
        o = t.step(true, who, 2016);
        CHECK(o.show && o.slot == 3);
        who = firstLoadingSlot(none, 4);                    // everyone is in (or the bubble popped / the restart began)
        CHECK(!t.step(who >= 0, who, 2032).show);
        CHECK(!t.step(true, 2, 2048).show);                 // a new wait starts a new clock
    }
    { // bubble rule: only when everyone else in the scene is caught; loaders only inside the 40 s window (the caller passes that)
        const bool noFree[4] = {false, false, false, false};
        const bool oneFree[4] = {false, true, false, false};
        const bool ld[4] = {false, false, true, false};
        const bool ldNone[4] = {false, false, false, false};
        CHECK(bubbleWaitSlot(ld, noFree, 4) == 2);
        CHECK(bubbleWaitSlot(ld, oneFree, 4) == -1);       // a free teammate can still pop us: no banner
        CHECK(bubbleWaitSlot(ldNone, noFree, 4) == -1);    // window over / nobody loading
    }
    std::printf("wait_test OK\n");
    return 0;
}
