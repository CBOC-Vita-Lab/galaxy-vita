// GXV_DRAWDONE_OTHER (default 1; vita/vgx-game/entry.cpp): a draw-done wait asked for by a guest thread other than the game thread.
//
// The deferred draw-done wait (GXV_DRAWDONE_DEFER, port/drawdone_defer.h) belongs to the game thread: it arms it at the end of a drawn
// turn and settles it before its next GXSetDrawDone.  A view pass or display-list write outside the drawn turn's flush also waits
// (port_gx_view_write_from, ModelManager::updateDL's settle), and the game's LOADING thread makes those writes too: every model a stage
// builds runs its first viewCalc on the thread that loads the scene (GameSystemSceneController::initializeScene).  Until 2.3 that
// thread took the game thread's wait (Deferral::MustWait cleared its pending flag) and slept in GXWaitDrawDone for the one-bit
// DrawDone flag.  Two faults:
//   1. Starvation (the "black screen after the title" hang, issue 3).  The finish interrupt wakes every thread in GXMisc.c's FinishQueue,
//      but the loading thread (priority 17) runs only when the game thread (16) sleeps, and the game thread resets DrawDone with its
//      next GXSetDrawDone first whenever the finish came while it ran or while it waited for that same finish itself.  At 60 pictures
//      a second with every turn drawn (Performance mode, GXV_FPS_LIMIT=0, the closed screen of a load) the front end's finishes come in
//      step with the game's turns, so the loading thread sees DrawDone false at every wake-up, forever: the stage never finishes loading
//      (console logs 424-438: the 19th outside write never returns; core 0 idle; the game's heap does not grow).  Under the fixed 30
//      cap every other turn has no GXSetDrawDone, which is why Quality / Locked 30 always got through.
//   2. With the game thread's pending flag cleared by another thread, its next GXSetDrawDone did not wait: two finishes outstanding
//      on a one-bit flag, so the game's following wait could return on the older finish.
// The fix: another thread never touches the game thread's deferral; it waits for the finish that was outstanding when it asked, by
// the count of GXSetDrawDone calls (GXMisc.c port_gx_wait_finish_other): it is done when DrawDone is true or when a later
// GXSetDrawDone has been issued (the game thread makes one only after the earlier finish, see port/drawdone_defer.h), so a reset of
// the flag can no longer hide a finish from it.  GXV_DRAWDONE_OTHER=0: the 2.2 / r110 behaviour (for A/B only).
#pragma once
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// The other thread's wait is over: the flag is set (the outstanding finish came), or the game thread issued a GXSetDrawDone since the
// request (`setsNow` != `setsAtRequest`), which it does only after the earlier finish.
static inline int port_drawdone_other_seen(int drawDone, uint32_t setsNow, uint32_t setsAtRequest) {
    return drawDone || setsNow != setsAtRequest;
}

// "0" off (the old shared wait); anything else, unset included, on.
static inline int port_drawdone_other_parse(const char* text) { return !(text && text[0] == '0' && text[1] == 0); }

#ifdef __cplusplus
}
#endif
