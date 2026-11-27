// sampler30 (16 Nov 2026): the opt-in on-device sampling profiler. Off unless env.txt has GXV_SAMPLER=1; with it unset the only cost is one getenv at boot
// (port_sampler_boot) and one relaxed load in each stage hook. Vita only (the desktop build logs "sampler: Vita only"). Needs the CapUnlocker plug-in
// (it is what lets an application call sceKernelSuspendThreadForVM / sceKernelGetThreadContextForVM); without it the sampler logs one line and stops.
//
//   GXV_SAMPLER=1                  turn it on
//   GXV_SAMPLER_THREADS=game       a comma list of: game (game-main), guest (the guest threads), render (vgx-render), helper (vgx-player, vgx-finish,
//                                  vgx-fe-b, smg_par workers), or a thread's registered / kernel name. Default: game.
//   GXV_SAMPLER_US=1000            sampling period in microseconds, clamped to 250..10000
//   GXV_SAMPLER_SEC=N              the window ends after N seconds (default: when the stage changes, at most 120 s)
//   GXV_SAMPLER_STAGE=Name         the window opens in the first stage whose name contains Name (case-insensitive); default "Galaxy" (the playable
//                                  galaxies; the title and menu scenes also pass the stage hooks); "*" = any scene
//   GXV_SAMPLER_DELAY_SEC=N        the window opens N seconds after that stage starts playing (default 0)
//   GXV_SAMPLER_RING=N             ring size in samples (default 131072, 12 bytes each; 4096..1048576)
//   GXV_SAMPLER_SECS=20            (GXV_SAMPLER_SECS=0: no dumps) (samp57) every N seconds of the window: write ux0:data/smg/sampler-<n>.bin (a pc/lr histogram of that interval, the module's text base and
//                                  the run-time address of port_sampler_boot: layout in port/sampler_hist.h) and log a "sampler bin" line and a "sampler overhead" line
//                                  (how long each thread was stopped); the last partial interval is written when the window closes. Report: tools/sampler_report.py.
//                                  NOTE the window itself is still GXV_SAMPLER_SEC (singular: its length, default until the stage changes, at most 120 s).
// Output: ux0:data/smg/sampler-<time>.txt and one "sampler:" line in log.txt. tools/sampler/symbolize.py turns the file into function tables.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void port_sampler_boot(void);                      // once, after env.txt is read (vita/vgx-game/entry.cpp)
void port_sampler_stage_loading(const char* stage);  // a stage starts to load (perf.cpp port_scene_load_begin): a window in progress ends
void port_sampler_stage_playing(const char* stage);  // the stage's load is done and it plays (perf.cpp port_scene_load_end): a window may open
#ifdef __cplusplus
}
#endif
