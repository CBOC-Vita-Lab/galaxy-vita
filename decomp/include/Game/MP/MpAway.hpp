// Galaxy 3.0 multiplayer: the local player's away state (touches). See MpAwayLogic.hpp. Game side: reads the pad, feeds STATE's
// MP_POSE_AWAY bit (MpLocalCapture.cpp) and "Be right back" (MpChat.cpp).
#pragma once

namespace MpAway {
    void tick();       // every game-scene turn of a session (MpChat::update)
    void brb();        // the "Be right back" phrase was sent
    bool local();      // the local player is away (MpLocalCapture: MP_POSE_AWAY)
    void reset();      // a session ended
}
