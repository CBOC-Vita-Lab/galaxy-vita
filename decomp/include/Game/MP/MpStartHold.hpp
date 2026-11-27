// Galaxy 3.0 multiplayer: how the renderer's closed-screen warm-up hold and the multiplayer start barrier share the closed screen
// (GameSystemSceneController::isReadyToStartScene). Pure logic, no game types: tests/mp/starthold_test.cpp includes this file.
//
// Without a session the answer is the old one: ready and no warm-up hold. In a session (merge fix 2):
//  - the session is pumped on every frame the scene is ready (MpFlow::holdSceneStart pumps), also while the warm-up hold is in force,
//    so flow / UI messages do not queue and the others' barrier sees this console alive;
//  - LOADED is sent only when this console is really ready to open its screen: the warm-up hold is honoured for at most kWarmHoldCapMs
//    (the old 5 s cap of GXV_STAGEWARM_HOLD_MS; the performance line's longer GXV_STAGEWARM_HOLD_OWN_MS does not apply in a session);
//  - the renderer's hold is polled on every ready frame (the caller does that), also while the barrier holds the screen, so its queue
//    keeps running in whole slices during the barrier wait.
#ifndef MP_START_HOLD_HPP
#define MP_START_HOLD_HPP

namespace MpStartHold {
    constexpr int kWarmHoldCapMs = 5000;

    struct In {
        bool ready;            // the scene controller is in its ready nerve and no scenario select is up
        bool inSession;        // mp_active()
        bool warmActive;       // port_stage_warm_hold() returned non-zero this frame (false where there is no such hold)
        int heldMs;            // ms this console has been seeing the warm-up hold in force for this scene (0 when not)
        bool barrierHolding;   // MpFlow::holdSceneStart() returned true (only meaningful when pump was asked for)
    };
    struct Out {
        bool ready;            // isReadyToStartScene's return value: true = open the screen now
        bool pump;             // call MpFlow::holdSceneStart (pumps the session; with sendLoaded it also announces LOADED and runs the barrier)
        bool sendLoaded;       // announce LOADED (this console is ready to open its screen)
    };

    inline Out decide(const In& in) {
        if (!in.inSession) return {in.ready && !in.warmActive, false, false};   // exactly the single-player code
        if (!in.ready) return {false, false, false};
        const bool warmWaiting = in.warmActive && in.heldMs < kWarmHoldCapMs;
        return {!warmWaiting && !in.barrierHolding, true, !warmWaiting};
    }
}

#endif
