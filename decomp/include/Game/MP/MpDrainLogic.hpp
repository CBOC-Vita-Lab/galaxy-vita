// Galaxy 3.0 multiplayer: how a batch of messages popped together (mp_poll_many pops up to 8 under one core lock, before the first is
// routed) is routed (corefix2; pure logic, host tested in tests/mp/drain_test.cpp; the game side is MpGame.cpp drain).
//
// A handler may end the session (a goodbye / removal seen by the flow) or move the scene (a new scene serial). The messages popped
// after it were sent in the world before that: after the session is over they are dropped (never delivered into the next session or
// the menus), and after a scene change the scene-bound ones are held for the next scene's first turn instead of being applied to the
// scene that is going (scene-bound ones are what route() holds when the scene is not live; flow and ui messages are still handled).
#pragma once

#include <stdint.h>

namespace MpDrainLogic {
    // route(i, sceneLive): route message i of the batch. alive(): the session is still active. serial(): a counter that changes when
    // the scene does. Returns how many messages were routed (the rest were dropped because the session ended).
    template <class Route, class Alive, class Serial>
    inline uint32_t routeBatch(uint32_t n, bool sceneLive, Route route, Alive alive, Serial serial) {
        uint32_t routed = 0;
        const auto serial0 = serial();
        for (uint32_t i = 0; i < n; i++) {
            if (!alive()) break;   // the session ended in an earlier message of this batch: the rest belong to it, not to what comes next
            route(i, sceneLive && serial() == serial0);
            routed++;
        }
        return routed;
    }
}  // namespace MpDrainLogic
