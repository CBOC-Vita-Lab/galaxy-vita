// corefix2: a batch of messages popped together (mp_poll_many pops up to 8 before the first is routed) stops when a handler ends the session,
// and routes the rest as "scene not live" (held for the next scene) when a handler moves the scene (MpDrainLogic.hpp; game side MpGame.cpp).
//   bash tests/mp/drain_build.sh
#include <stdio.h>
#include <vector>

#include "Game/MP/MpDrainLogic.hpp"

static int gChecks, gFails;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Sim {
    bool active = true;
    unsigned serial = 7;
    int endAt = -1, moveAt = -1;   // the handler of message endAt ends the session / moveAt changes the scene
    std::vector<int> got;
    std::vector<bool> live;
    uint32_t run(uint32_t n, bool sceneLive) {
        return MpDrainLogic::routeBatch(n, sceneLive,
            [&](uint32_t i, bool l) {
                got.push_back((int)i);
                live.push_back(l);
                if ((int)i == endAt) active = false;
                if ((int)i == moveAt) serial++;
            },
            [&] { return active; }, [&] { return serial; });
    }
};

int main() {
    {   // nothing happens: all 8, scene-live as asked
        Sim s;
        CHECK(s.run(8, true) == 8 && s.got.size() == 8);
        for (bool l : s.live) CHECK(l);
        Sim t;
        t.run(3, false);
        for (bool l : t.live) CHECK(!l);
    }
    {   // message 2 ends the session (a goodbye / removal): 3..7 are not routed (before: all 8 were)
        Sim s;
        s.endAt = 2;
        CHECK(s.run(8, true) == 3);
        CHECK(s.got.size() == 3 && s.got.back() == 2);
    }
    {   // the first message ends it: the rest still dropped
        Sim s;
        s.endAt = 0;
        CHECK(s.run(8, true) == 1);
    }
    {   // message 3 moves the scene: 4..7 are routed as not live (the scene-bound ones are held), 0..3 as live
        Sim s;
        s.moveAt = 3;
        CHECK(s.run(8, true) == 8);
        for (int i = 0; i < 8; i++) CHECK(s.live[(size_t)i] == (i <= 3));
    }
    {   // both: the scene moved at 1, the session ended at 4
        Sim s;
        s.moveAt = 1;
        s.endAt = 4;
        CHECK(s.run(8, true) == 5);
        CHECK(s.live[1] && !s.live[2] && !s.live[4]);
    }
    {   // a session already over drops the whole batch
        Sim s;
        s.active = false;
        CHECK(s.run(8, true) == 0);
    }
    printf("drain_test: %d checks, %d failed\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
