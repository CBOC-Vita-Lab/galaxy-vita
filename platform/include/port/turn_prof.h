// GXV_TURN_PROF=1 (gamepar, 8 Nov): the game thread's turn split into categories, one "turn-prof:" line in log.txt every 5 s
// (platform/src/port/perf.cpp). A lap closes the time since the last lap into one bucket; laps outside a turn (between
// port_turn_prof_end and the next port_turn_prof_begin) are ignored. Off: one relaxed load per call.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int port_turn_prof_on(void);
void port_turn_prof_begin(void);
void port_turn_prof_lap(int bucket);
void port_turn_prof_end(int drawn);
void port_turn_prof_count(int what, unsigned n);
// (undrawn26) GXV_TURN_PROF=2: the calc-anim lists one by one (SceneFunction::executeCalcAnimList): cat_begin starts the clock, cat(k) books list k
void port_turn_prof_cat_begin(void);
void port_turn_prof_cat(int k);
// (strikers2audit) marks that split the drawn turn's "other" (PORT_TPM_*): books the time since the last mark; -1 only restarts the mark clock
void port_turn_prof_mark(int sub);
// GXV_TURN_PROF=2 / GXV_DRAW_PROF=2 (gamecpu23): a part of draw() timed (begin / end may repeat in a drawn turn; the parts add up)
void port_draw_part_begin(int part);
void port_draw_part_end(int part);
// GXV_ENDWAIT_LOG=1 (endwait26, default 0; platform/src/port/perf.cpp, platform/vgx/src/vgx_endwait.h): the game thread's wait at the end of its frame. begin returns the
// start time (0 when the switch is off: nothing else is done); end logs one "endwait:" line when the wait took GXV_ENDWAIT_MS (40) or more. where: 0 the end of the
// frame, 1+ a deferred draw-done settle (GXV_DRAWDONE_DEFER, MainLoopFramework.cpp).
int64_t port_endwait_begin(void);
void port_endwait_end(int64_t t0Ns, int where);
// Performance-mode cut knobs (perf.cpp, gamepar)
int port_perf_mode_60(void);
int port_flush_offscreen_mode(void);
int port_plant_drawcull_mode(void);
int port_hr_plantmove_mode(void);
float port_hr_plant_pdist(void);
#ifdef __cplusplus
}
#endif
// buckets (perf.cpp's order)
#define PORT_TP_FEWAIT 0
#define PORT_TP_FL_ANIM 1
#define PORT_TP_FL_DL 2
#define PORT_TP_FL_VIEW 3
#define PORT_TP_DRAW 4
#define PORT_TP_UPDATE 5
#define PORT_TP_CALC 6
#define PORT_TP_SHADOW 7
#define PORT_TP_PTCL 8
#define PORT_TP_VIEW 9
#define PORT_TP_OTHER 10
// marks (perf.cpp kSub*)
#define PORT_TPM_DECIDE 0
#define PORT_TPM_PREPARE 1
#define PORT_TPM_CAPTURE 2
#define PORT_TPM_ENDFRAME 3
#define PORT_TPM_TURNEND 4
// counters
#define PORT_TC_PLANT_SKIP 0
#define PORT_TC_PLANT_MOVED 1
#define PORT_TC_FLUSH_OFF 2
#define PORT_TC_FLUSH_DONE 3
// draw parts (perf.cpp kDp*)
#define PORT_DP_SHADOW 0
#define PORT_DP_EFFECT 1
#define PORT_DP_2D 2
#define PORT_DP_3D 3
#define PORT_DP_MIRROR 4
// cpu2-26: the shadow part split (nested inside PORT_DP_SHADOW): the volumes, silhouette + fill, the alpha shadow, the shadow surface
#define PORT_DP_SHV 5
#define PORT_DP_SIL 6
#define PORT_DP_SHA 7
#define PORT_DP_SHS 8
