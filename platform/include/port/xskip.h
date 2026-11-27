// xskip27 (13 Nov): the accepted skip of a galaxy's opening (X = Wii A), made measurable and smooth.
//
// Three native paths end an opening early: the galaxy intro camera (GameSceneScenarioOpeningCameraState::trySkipTrigger: a tap on a repeat visit, a
// hold of A on a first one), the starter rail (ScenarioStarter::trySkipTrigger: Mario's flight in), and the port's generic fast-forward (demos and
// dialogues, cutscene_skip.cpp, GameSystem::frameLoop). platform/src/port/xskip.cpp keeps the story of one stage entry and writes:
//   xskip: accepted ...   the skip (path, tap / hold, turn, the queue depths and the work left at that moment)
//   xskip: cover ...      the cinema frame is fully black (the opaque interval on the Vita build; port_screen_quiet does not see it)
//   xskip: cut ...        the camera cut (the starter's exeRailMoveCanceled, or the intro camera's end)
//   xskip: picture ...    the first picture drawn after the cut
//   xskip: control ...    the first turn Mario can be controlled
//   xskip: hold ...       the cover kept closed (GXV_XSKIP_HOLD_MS) for work that was not done: how long, what was left
//   xskip: summary ...    one line per stage entry (skipped yes/no, path, work left, cover hold ms, the longest picture in the 3 s after control, counters)
// GXV_XSKIP_LOG (default 1) switches the lines, GXV_XSKIP (default 1) the cover hold, the full budget while covered and the pacer's calm window.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_XSKIP_OPENING 1  // GameSceneScenarioOpeningCameraState
#define PORT_XSKIP_STARTER 2  // ScenarioStarter
#define PORT_XSKIP_FORWARD 3  // the generic fast-forward (cutscene_skip.cpp)

// The renderer's side: queue depths (work left) and cumulative counters, any thread. Filled by platform/vgx/src/vgx_backend_gl.cpp (vgx_xskip_depths).
typedef struct PortXskipDepths {
    unsigned stageLeft;     // the stage queue's draw variants not worked off (the filler included)
    unsigned ownLeft;       // the stage's own programs not linked yet (what the first views draw)
    unsigned planLeft;      // texture replay + premade target plan items left (GXV_TEXWARM / GXV_PREWARM_RES)
    unsigned prewarmLeft;   // warm-scope variants and effects not drawn yet (GXV_PREWARM)
    unsigned layoutLeft;    // layouts not drawn in a warm scope yet (GXV_LAYOUTWARM)
    unsigned backendPending;  // pictures queued to the back end it has not finished
    unsigned long long links;      // programs linked (draw-demanded + stage queue + warm)
    unsigned long long compiles;   // shaders compiled
    unsigned long long uploads;    // texture uploads
    unsigned long long targets;    // render targets made
    unsigned long long frames;     // back-end pictures
    double execMs;                 // back-end time in pictures
    double texMs;                  // of it, texture uploads
} PortXskipDepths;
void vgx_xskip_depths(PortXskipDepths* out) __attribute__((weak));
// The game thread asks for a full-budget drain (on = 1, every poll) or ends it (0): the renderer treats the frames as held closed (the stage queue's
// slices of the hold, not the 2 ms of play).
void vgx_xskip_drain(int on) __attribute__((weak));
// (dvd.cpp) cumulative: async reads asked for, the game thread's synchronous reads and ms it waited in them
void port_dvd_xskip_counters(unsigned long long* asyncReads, unsigned long long* gameSyncN, double* gameSyncMs) __attribute__((weak));

// Game thread.
void port_xskip_scene_begin(void);                 // GameScene::init (loading thread): a new game scene
void port_xskip_accept(int path);                  // a skip was just accepted (the intro camera, the starter rail, a fast-forward start)
void port_xskip_cut(int path);                     // the cut: the starter's exeRailMoveCanceled first step, the intro camera's end
// 1 = keep the cover closed one more turn (an accepted skip, the screen opaque, work left, under the cap); each call while held polls the renderer.
int port_xskip_cover_hold(void);
// GameScene::update, every turn: nerve 0 other, 1 intro camera, 2 starter, 3 action; playing = action without a demo; blank = the cinema frame is fully black.
void port_xskip_scene_turn(int nerve, int playing, int blank, const char* stage, int scenario);
unsigned port_xskip_turn_no(void);                 // game turns seen by port_xskip_scene_turn (monotonic, never 0 once a game scene has run)
void port_xskip_frame(int drawn);                  // GameSystem::frameLoop: this turn draws (1) or not (0)
void port_xskip_bgm(double ms);                    // the time the game thread spent starting the stage BGM
// The frame-skip rule: this turn is drawn at the normal pace (no catch-up) while 1.
int port_xskip_calm_turn(void);
// Any thread: an accepted skip is open (accept .. cut + GXV_XSKIP_CALM_TURNS turns).
int port_xskip_active(void);
// (cutscene_skip.cpp) game turns since port_skip_take last handed a skip over (-1 never): a hold of A, as against a tap
int port_skip_last_take_age_turns(void);
// The test injector (PETARI_XSKIP_TEST, wpad.cpp): A pressed on top of the pad for the harness.
int port_xskip_test_a(void);

#ifdef __cplusplus
}
#endif
