// See vgx_galaxy.h.
#if defined(PORT_GX_VGX) && PORT_GX_VGX
#include "vgx_galaxy.h"

#include <pthread.h>
#if defined(__vita__)
#include <psp2/kernel/threadmgr/thread.h> // (sceKernelGetThreadCpuAffinityMask: GXV_FE_TWOCORE's placement)
#endif

#include "port/core3_budget.h" // (core3-38: the core-3 yield cap and meter)
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "port/gx_fifo.h"
#include "port/core_layout.h"
#include "port/frame_pace.h"
#include "port/fif.h"  // (strk27: GXV_FIF)
#include "port/seg_prewait.h"  // (pace33 PATCH9: GXV_SEG_PREWAIT_US)
#include "port/speed_guard.h"  // (speed30: GXV_SPEED_GUARD)
#include "port/heap_routing.h"
#include "port/host_vector.h"  // (crash36: the job queue and its pools are host memory whichever thread grows them)
#include "port/queue_depth.h"
#include "port/port.h"
#include "port/diagnostics.h"
#include "port/hot_knob.h"
#include "port/getenv25.h"  // (getenv25: GXV_HRW_POINTER read once, not per fresh draw)
#include "port/hrw_set.h"  // (hrw28: PairStats for the census)
#include "port/hrw_feel.h"  // (hrwfeel36: the "hrw: feel |" line)
#include "port/stage_knob.h"  // (hrw28: Quality() forces HRW off)
#include "port/qemu_hooks.h"
#include "port/vclock.h"
#include "port/settings_lifecycle.h"
#include "vgx_arena.h"
#include "vgx_fesplit.h"
#include "vgx_guestmem.h"
#include "vgx_hdtex.h"
#include "vgx_hle.h"
#include "vgx_state.h"
#include "vgx_fepar.h"
#include "vgx_endwait.h" // (endwait26: GXV_ENDWAIT_LOG)
#include "vgx_hrw_capfix.h" // (GXV_HRW_CAPFIX)
#include "port/fe_lane.h"  // (pace33 PATCH4: GXV_FE_LANE)
#include "port/hrw_stage.h"  // (pace33 PATCH6: GXV_HRW_STAGE)
#include "port/hrw_settle.h" // (stutter37: GXV_HRW_SETTLE_RESUME)
#include "port/hrw_mp.h"     // (mp30: the multiplayer session's picture policy)
#include "port/lockstep.h"   // (mp30: port::lockstep::On(), the session flag read once a turn)
#include "port/mario_time.h"  // (mariolag31: GXV_HRW_MARIOTIME)
#include "vgx_cmd.h"
#include "vgx_indcap_peek.h" // (GXV_INDCAP_PEEK)
#include "vgx_hrw.h" // (hrw26: GXV_HRW)
#include "vgx_workload.h"
#include "vgx_twocore.h"
#include "../../vgx/vita/vgx_boot_watch.h"

extern "C" volatile uint32_t __PortGXPERegs[0x400];
extern "C" void port_headless_write_png(const char* path, const unsigned char* rgba, int w, int h);
#if !defined(PORT_GX_VGX_ONLY)
extern "C" void port_gx_use_recorder_backend(void);
#endif
// A native pointer's region: its start and size (os_mem.cpp).
extern "C" int port_native_window(const void* p, void** base, size_t* bytes);
extern "C" int port_memory_region(unsigned index, void** base, void** alias, uint32_t* physical, uint32_t* size, int* kind);
extern "C" int64_t port_thread_cpu_us(void); // CPU time of the calling thread
extern "C" void port_perf_game_totals(int64_t* workNs, uint32_t* frames, int64_t* cpuUs, int64_t* maxWorkNs); // port/perf.cpp
extern "C" uint32_t port_frameskip_count(void);
extern "C" int port_screen_quiet(void) __attribute__((weak));
extern "C" uint32_t port_scene_key(void) __attribute__((weak));  // (endwait26: the stage's key, platform/src/port/perf.cpp)
extern "C" uint64_t vgx_tex_decode_us(void) __attribute__((weak));  // (vgx_texcache.cpp: the front end's texture decode time, endwait26's per-picture record)
extern "C" void vgx_tex_decode_counts(uint64_t* n, uint64_t* bytes) __attribute__((weak));
extern "C" int port_scene_loading_active(void) __attribute__((weak)); // (platform/src/port/perf.cpp: the game's loading thread runs)
extern "C" int port_flow_play_turns(void) __attribute__((weak));      // (GameScene.cpp: turns of play in the flow's current leg; 0 before play begins, -1 outside a leg, -2 no flow)
extern "C" int port_skip_context_kind(void) __attribute__((weak));    // (cutscene_skip.cpp: 1 movie, 2 opening camera, 3 starter fly-in, 4 in-engine demo, 5 talk, 0 none)
extern "C" int port_popin_flight(void) __attribute__((weak));         // (LodCtrl.cpp: 1 opening, 2 starter, 3 star-get demo, 4 demo, 5 fast camera, 0 none)
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
extern "C" uint64_t port_autoq_picture_work_us(void) __attribute__((weak));  // optional 30 fps feasibility estimate (WP-B)
extern "C" void port_autoq_front_picture(void) __attribute__((weak));     // automatic quality: the front end's CPU at each picture
extern "C" int port_autoq_front_totals(uint32_t* cpu, uint32_t* pictures) __attribute__((weak)); // (vgx_autoq.cpp: the front end's CPU time and pictures, target mode)
extern "C" int port_autoq_stages(uint32_t* gpuUs, uint32_t* pipeUs, int* lowerable) __attribute__((weak)); // (vgx_autoq.cpp: the GPU's busy time a picture, target 60)
extern "C" int port_autoq_ladder_room(void) __attribute__((weak));  // (gov5; vgx_autoq.cpp: 1 while the GPU ladder (GXV_PERF_GPU896) has a step to try or one in proof)
extern "C" int port_hrw_player(float out[3]) __attribute__((weak));   // GXV_HRW=5 (warp32; GameScene.cpp): the player's centre in world space now; 0 when there is none
extern "C" int port_hrw_player36(float out[12]) __attribute__((weak)); // GXV_HRW=5 (warp36; GameScene.cpp, port/hrw36.h): feet, up, the ground point under him, the ground polygon's normal (world space); bits 1 player, 2 ground, 4 normal, 8 ground held through the air (hold36), 16 shadow out of range
extern "C" int port_hrw_camera(float out[12]) __attribute__((weak));  // GXV_HRW (decomp/src/Game/Scene/GameScene.cpp): the camera's view matrix now, 3x4 row major; 0 when there is none
extern "C" void port_hrw_fresh_scene_view(void) __attribute__((weak));  // (warp32 PATCH22; GameScene.cpp)
extern "C" void port_hrw_fresh_camera_take(void) __attribute__((weak));  // (mariolag31; GameScene.cpp)
extern "C" unsigned port_late_turn_slept_us(void) __attribute__((weak));  // (mariolag31; platform/src/port/perf.cpp: what this turn slept on purpose, GXV_LATE_TURN)
extern "C" float port_free_camera_turn_blend(void) __attribute__((weak));  // (freecam31; CameraDirector.cpp: [0..1] active turn blend)
extern "C" void port_hrw_fresh_view_take(unsigned* out) __attribute__((weak));  // (warp32 PATCH21; GameScene.cpp)
extern "C" int port_hrw_fresh_scene(unsigned pairId, int pointer) __attribute__((weak));  // GXV_HRW (hrw27; decomp/src/Game/Scene/GameScene.cpp): the partial draw of a pair's second picture; 0 when nothing was drawn
extern "C" unsigned port_hrw_scene_drops(void) __attribute__((weak));  // (crash1114, GameScene.cpp: scenes destroyed while the partial draw's pointer named them)
extern "C" void port_hrw_scene_arm(void) __attribute__((weak));   // (hrw27; GameScene.cpp: forget the scene the last picture was drawn by; GameScene::draw names itself again)
extern "C" void port_hrw_set_begin(unsigned pairId, int tag) __attribute__((weak));  // (hrw28; decomp PortHrwSet.cpp: scan and tag the significance set)
extern "C" void port_hrw_set_clear(void) __attribute__((weak));
extern "C" void port_hrw_set_take_stats(void* out) __attribute__((weak));
extern "C" void port_hrw_fresh_finish(void) __attribute__((weak)); // (hrw27; MainLoopFramework.cpp: the draw-done finish the partial draw owes, deferred as a picture's is)
extern "C" int port_vgx_present_sync_mode(void);  // (below)
extern "C" void port_handheld_texture(uint64_t hash, uint32_t width, uint32_t height, uint8_t* rgba); // port/handheld_ui.cpp
extern "C" bool port_handheld_texture_needs_rgba(uint64_t hash, uint32_t width, uint32_t height);
// settingsnative: a swapped button mapping (vita/runtime/input.cpp calls it after the commit): the button pictures are decoded again.
extern "C" void port_handheld_pictures_changed(void) { vgx::TexturePatchChanged(); }
extern "C" uint32_t port_vgx_vita_vcount(void) __attribute__((weak));  // the display's vblank counter (GXV_PRESENT_SYNC); weak: null on the desktop
#if defined(__vita__)
extern "C" int port_vita_pin_core(int core); // vita/runtime/memory.cpp
extern "C" int port_vgx_vita_start(void); // vita/runtime: vitaGL, on the calling (render) thread
extern "C" int port_vgx_vita_fixed_cap_supported(void) __attribute__((weak));
extern "C" void port_vgx_vita_set_present_allowed(int allowed);
extern "C" void port_vgx_vita_set_present_stamp(uint32_t stamp);  // GXV_PRESENT_SYNC: 0 none, else 1 << 31 | the turn's vblank
extern "C" void port_vgx_vita_finish_unpresented_frame(void);
extern "C" void port_vgx_vita_job_end(int second);  // (pace33 PATCH3: GXV_FIF's count of a job that never swapped, in every mode)
extern "C" unsigned long long port_vgx_vita_game_swaps(void);
extern "C" void port_vgx_vita_reset_present_slots(void);
extern "C" void port_vgx_vita_note_turn(uint32_t stamp, uint32_t vcount, int64_t turnNs, int64_t padNs) __attribute__((weak));  // GXV_LATENCY24 census (host)
extern "C" int64_t port_input_read_sample_ns(void) __attribute__((weak));  // GXV_LATENCY24: when the pad state of the game's last read was sampled (wpad.cpp)
extern "C" int port_vgx_present_sync_mode(void);  // (below)
extern "C" void port_vgx_vita_frame_done(void);
extern "C" void port_vgx_vita_ready(void);          // the back end exists: the loading screen moves over to it
extern "C" int port_vgx_vita_splash_active(void);   // 1 while the loading screen is up
extern "C" void port_vgx_vita_idle(void);           // keep the loading screen moving when no frame arrives
extern "C" void port_vgx_vita_idle_gap(unsigned gapUs);  // (the same, not sooner than gapUs after the last picture)
extern "C" void port_vgx_vita_prepare(void);       // the host's fast paths/settings for the front end, before the render thread starts
extern "C" void port_vgx_vita_load_packs(void);    // g30 Wave 7: shader RAM pack + user pack load, overlapped behind vglInitWithCustomSizes
extern "C" int port_vgx_vita_settings_finish(unsigned timeoutMs);
extern "C" void port_vgx_vita_settings_resume(void);
extern "C" int port_vgx_vita_error_shown(void);     // 1 when a start-up failure is on the loading screen
extern "C" int port_vgx_vita_fail(const char* message);  // put a start-up failure on the loading screen (1: shown)
extern "C" void port_vgx_vita_backend_failed(void); // the back end's own set-up failed: say so on the loading screen
extern "C" int port_vita_core0_alone(void) __attribute__((weak)); // GXV_CORE0_ALONE (vita/runtime/memory.cpp)
extern "C" int port_vita_thread_core3(int part, int planned) __attribute__((weak)); // the fourth core (vita/runtime/memory.cpp)
extern "C" int port_vita_core3_available(void) __attribute__((weak));               // (proven once, vita/runtime/memory.cpp)
extern "C" void port_vita_cores_report(double seconds, unsigned pictures, unsigned turns) __attribute__((weak));  // ("cores-5s:" lines, vita/runtime/memory.cpp)
#endif

namespace vgx {
bool LogEnabled() {
    return PORT_HOT_KNOB(bool, EnvInt("GXV_LOG", 0) != 0);  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
// (FrontPresent's fall-back when no frame sink is set; the sink below always is)
void SubmitToBackend(CmdBuf&) {}
} // namespace vgx

extern "C" void port_input_latency_frame_done(unsigned long long shown); // input/wpad.cpp
extern "C" void port_host_heap_summary(void) __attribute__((weak));  // os/os_thread.cpp (absent in some host tests)
extern "C" void port_vgx_census_log(const char* when);  // (below)

namespace {

std::atomic<bool> sActive{false};
std::atomic<unsigned long long> sFramesQueued{0}, sFramesShown{0};
std::atomic<unsigned> sPaceDivisor{2};
std::atomic<uint64_t> sPaceBackendUs{0}, sPaceWaitUs{0};
int PaceModeOnce() {
    // GXV_PACE=1: 30/20 display cadence with 60 Hz simulation; =2 also checks its countdown oracle. Default off.
    static const int mode = [] {
        // GXV_FPS_LIMIT=30 requests fixed 30-picture presentation, not slower simulation; default off.
        int requested = static_cast<int>(vgx::EnvInt("GXV_FPS_LIMIT", 0));
        if (port::pace::Hrw5Pacing()) {  // (warp32 PATCH7: the re-projected 60 owns the pacing; see frame_pace.h Hrw5Pacing)
            const int pace = static_cast<int>(vgx::EnvInt("GXV_PACE", 0));
            if ((requested == 30 || pace != 0) && port_log_summary)
                port_log_summary("hrw: GXV_HRW=5: GXV_FPS_LIMIT=%d / GXV_PACE=%d are not used (mode 5 shows a picture every refresh by its own rhythm; GXV_HRW_PACING_AUTO=0 keeps them and turns mode 5 off)", requested, pace);
            return 0;
        }
        bool supported = false;
#if defined(__vita__)
        supported = port_vgx_vita_fixed_cap_supported && port_vgx_vita_fixed_cap_supported();
#endif
        if (requested == 30 && !supported) {
            if (port_log_summary) port_log_summary("fps cap: requested 30 unavailable; no no-present driver capability; retaining old pacing");
            else port_log("fps cap: requested 30 unavailable; no no-present driver capability; retaining old pacing");
        }
        return port::pace::ResolveMode(requested, static_cast<int>(vgx::EnvInt("GXV_PACE", 0)), supported);
    }();
    return mode;
}
// (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h); PaceModeOnce's guarded static still works it out (and says so in the log) exactly once
int PaceMode() { return PORT_HOT_KNOB(int, PaceModeOnce()); }
unsigned long long GameSwapCount() {
#if defined(__vita__)
    if (PaceMode() == 3) return port_vgx_vita_game_swaps();
#endif
    return 0;
}

// ------------------------------------------------------------------ "perf:" line
// Every GXV_PERF seconds (default 5 on handhelds, 0 = off elsewhere), on the render thread: how
// fast the game runs and what each of the three threads spent per game frame. This is the
// line to read after a run on the console: the slowest of the three sets the frame rate.
std::atomic<int64_t> sPlCpuUs{0};      // the player thread's CPU time at its last display copy
std::atomic<int64_t> sGmWaitNs{0};     // game thread: time spent waiting for the player (full segment)
std::atomic<uint64_t> sPlRecords{0}, sPlPrims{0}; // draw records / GX primitives of all frames so far (front end)
struct PerfWindow {
    int64_t t0 = 0, work = 0, gameCpu = 0, plCpu = 0, beCpu = 0, beWall = 0, wait = 0;
    uint32_t frames = 0;
    unsigned long long queued = 0, shown = 0, swaps = 0;
    uint64_t draws = 0, records = 0, prims = 0;
    uint32_t skipped = 0;
} sPerf;
int sPerfSeconds = 0;
// GXV_CHARAA (vgx_draw.cpp, character edge smoothing): what the last window smoothed, per display copy (front end: frames with
// character rectangles, character draws, rectangles and their EFB pixels; back end: rectangles drawn and the screen pixels
// shaded a second time, presents skipped because the EFB is multisampled). Every perf window, when the switch is on.
void EdgePerfLine();
void CharaPerfLine() {
    EdgePerfLine();
    if (vgx::CharaAAMode() == 0) return;
    static uint64_t lf[11] = {}, lb[4] = {};
    uint64_t f[11], b[4];
    vgx::CharaCounters(f);
    vgx::backend::CharaAACounters(b[0], b[1], b[2], b[3]);
    const double copies = f[0] - lf[0] ? static_cast<double>(f[0] - lf[0]) : 1.0;
    port_log("perf: charaa: %llu display copies, %llu with characters; per copy: %.1f character draws (%.1f without bounds), %.1f HUD draws measured, "
             "%.2f boxes, %.0f EFB px; back end: %.2f boxes drawn and %.0f screen px shaded per copy, %llu over the area limit, %llu dropped by "
             "clearing copies, %llu HUD-cut pieces lost, %llu too small, %llu presents skipped (multisampled EFB)",
             (unsigned long long)(f[0] - lf[0]), (unsigned long long)(f[1] - lf[1]), (f[2] - lf[2]) / copies, (f[3] - lf[3]) / copies, (f[4] - lf[4]) / copies,
             (f[5] - lf[5]) / copies, (f[6] - lf[6]) / copies, (b[1] - lb[1]) / copies, (b[2] - lb[2]) / copies, (unsigned long long)(f[7] - lf[7]),
             (unsigned long long)(f[8] - lf[8]), (unsigned long long)(f[9] - lf[9]), (unsigned long long)(f[10] - lf[10]),
             (unsigned long long)(b[3] - lb[3]));
    std::memcpy(lf, f, sizeof f);
    std::memcpy(lb, b, sizeof b);
}

// GXV_EDGE_AA (vgx_draw.cpp, whole-picture edge smoothing): per perf window, the display copies, how many were smoothed whole, the
// HUD rectangles drawn again plainly per smoothed copy, the copies given up because of the HUD's area, and the back end's skips
// (multisampled EFB: rungs 7-8, already smooth; other: the shader did not build, or GXV_AUTOQ_EDGESTEP has it off under GPU load).
void EdgePerfLine() {
    if (vgx::EdgeAAMode() == 0) return;
    static uint64_t lf[5] = {}, lb[4] = {};
    uint64_t f[5], b[4];
    vgx::EdgeCounters(f);
    vgx::backend::EdgeAACounters(b[0], b[1], b[2], b[3]);
    const double smoothed = b[0] - lb[0] ? static_cast<double>(b[0] - lb[0]) : 1.0;
    const double copies = f[0] - lf[0] ? static_cast<double>(f[0] - lf[0]) : 1.0;
    port_log("perf: edgeaa: %llu display copies, %llu marked for smoothing, %llu given up (HUD area), %.1f HUD draws per copy; back end: %llu presents "
             "smoothed whole, %.2f HUD rectangles redrawn per smoothed present, %llu skipped (multisampled EFB), %llu skipped (shader, or the "
             "controller's edge step: GXV_AUTOQ_EDGESTEP)",
             (unsigned long long)(f[0] - lf[0]), (unsigned long long)(f[1] - lf[1]), (unsigned long long)(f[3] - lf[3]), (f[4] - lf[4]) / copies,
             (unsigned long long)(b[0] - lb[0]), (b[1] - lb[1]) / smoothed, (unsigned long long)(b[2] - lb[2]), (unsigned long long)(b[3] - lb[3]));
    std::memcpy(lf, f, sizeof f);
    std::memcpy(lb, b, sizeof b);
}

// Freeze watch (the Vita, the log on; GXV_FREEZE_WATCH=0 off): a thread looks once a second whether a new picture was shown.
// When none came for 3 s it writes a line at once (flushed to the card, so it is there even if the game never recovers), saying
// whether the game thread is still running turns (then the renderer is what stopped) or not (a load, or the game is stuck), and
// when pictures come back it writes how long the screen stood still. Counts seconds of its own loop, not the clock: a console
// that sleeps or suspends the game stops this thread too, and that is not reported as a freeze. Neither is a cutscene skip: its
// fast-forward runs the game's turns without pictures on purpose (a few seconds for the prologue).
extern "C" void port_log_flush(void);
extern "C" int port_skip_fast_forwarding(void) __attribute__((weak));
extern "C" int port_settings_active(void) __attribute__((weak));
void StartFreezeWatch() {
    if (!PORT_LOGGING || vgx::EnvInt("GXV_FREEZE_WATCH", 1) == 0) return;
    std::thread([] {
        PORT_THREAD_NAME("freeze-watch");
        port_thread_helper();  // GXV_CORE0_ALONE=1: off the game's core (port.h); a once-a-second sleeper
        unsigned long long lastShown = sFramesShown.load(std::memory_order_relaxed);
        uint32_t lastTurns = 0;
        int still = 0;
        bool reported = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const unsigned long long shown = sFramesShown.load(std::memory_order_relaxed);
            int64_t work = 0, cpu = 0, maxWork = 0;
            uint32_t turns = 0;
            port_perf_game_totals(&work, &turns, &cpu, &maxWork);
            if ((port_settings_active && port_settings_active()) || (port_skip_fast_forwarding && port_skip_fast_forwarding())) {
                still = 0;  // (a skip, not a freeze)
                reported = false;
                lastShown = shown;
            } else if (shown != lastShown) {
                if (reported) port_log_summary("freeze watch: pictures again after about %d s without one", still);
                still = 0;
                reported = false;
                lastShown = shown;
            } else if (shown > 0) {  // (not before the first picture: the boot has its own loading screen)
                ++still;
                if (still == 3 || (reported && still % 30 == 0)) {
                    port_log_summary("freeze watch: no new picture for %d s; the game thread %s (%u turns in the last second)", still,
                                     turns != lastTurns ? "is running, the renderer is not presenting" : "is not running turns (a load, or stuck)",
                                     turns - lastTurns);
                    port_log_flush();
#ifdef __vita__
                    {   // (q30-27: the same facts straight to a file of its own, not through the log's lock/buffer: if the log is what is wedged (the c86 Quality tour's log ended
                        // 33 s after launch with the process alive) this line is still written; if neither appears, the whole process was stopped. Never read by the game.)
                        FILE* ff = fopen("ux0:data/smg/freeze.txt", "a");
                        if (ff) {
                            fprintf(ff, "freeze watch: no new picture for %d s; game thread %s (%u turns in the last second), pictures shown %u\n", still,
                                    turns != lastTurns ? "running" : "NOT running turns", static_cast<unsigned>(turns - lastTurns), static_cast<unsigned>(shown));
                            fclose(ff);
                        }
                    }
#endif
                    reported = true;
                }
            }
            lastTurns = turns;
        }
    }).detach();
}

// (render thread) `execNs`: wall time of the frame just executed.
void PerfFrame(int64_t execNs, uint32_t drawCalls) {
    if (!PORT_LOGGING) return;
    if (sPerfSeconds <= 0) return;
    if (!PORT_RUNTIME_DIAGNOSTICS) {
        // The player build's basic log (ux0:data/smg/log.txt): every GXV_PERF seconds (default 5) one line with the frame rate,
        // job-completion gaps and the picture size. Fixed 30 counts only jobs that actually swapped;
        // hidden completed jobs must not hide a long wait between visible pictures.
        const int64_t now = port_host_time_ns();
        static int64_t lastPresent = 0, longest = 0;
        static unsigned long long lastSwaps = 0;
        static unsigned hitches = 0;
        const unsigned long long swaps = GameSwapCount();
        if (PaceMode() != 3 || swaps != lastSwaps) {
            if (lastPresent) {
                const int64_t gap = now - lastPresent;
                if (gap > 100000000ll) ++hitches;
                if (gap > longest) longest = gap;
            }
            lastPresent = now;
        }
        lastSwaps = swaps;
        if (sPerf.t0 && now - sPerf.t0 < static_cast<int64_t>(sPerfSeconds) * 1000000000ll) return;
        PerfWindow w;
        int64_t unusedMax;
        port_perf_game_totals(&w.work, &w.frames, &w.gameCpu, &unusedMax);
        w.t0 = now;
        w.shown = sFramesShown.load(std::memory_order_relaxed);
        w.swaps = swaps;
        w.skipped = port_frameskip_count();
        if (sPerf.t0) {
            const double seconds = (now - sPerf.t0) / 1e9;
            int rw = 0, rh = 0, rs = 1;
            vgx::backend::QualityRungSize(rw, rh, rs);
            // Preparation includes cached loads and links, not just native compiler misses.
            static uint32_t lastCompiles = 0, lastLinks = 0;
            static double lastShaderMs = 0;
            const vgx::backend::Stats bs = vgx::backend::GetStats();
            const unsigned compiles = bs.shaderCompiles - lastCompiles;
            const unsigned links = bs.programLinks - lastLinks;
            const double compileMs = bs.shaderWorkMs - lastShaderMs;
            lastCompiles = bs.shaderCompiles;
            lastLinks = bs.programLinks;
            lastShaderMs = bs.shaderWorkMs;
            const unsigned warmLeft = vgx::backend::WarmPending();
            char compileNote[144] = "";
            if (compiles || links || compileMs > 0 || warmLeft)
                std::snprintf(compileNote, sizeof compileNote, "; %u shaders prepared (%.0f ms); %u program links; warm %u left", compiles, compileMs, links, warmLeft);
            if (PaceMode() == 3) {
                port_log_summary("fps: %.1f s: swap FPS %.1f; game %.1f turns/s; %u skipped; %u completed jobs; %u swapped-job gaps over 100 ms (longest %.0f ms); picture %dx%d%s%s",
                                 seconds, (w.swaps - sPerf.swaps) / seconds, (w.frames - sPerf.frames) / seconds,
                                 w.skipped - sPerf.skipped, static_cast<unsigned>(w.shown - sPerf.shown), hitches, longest / 1e6, rw, rh,
                                 rs > 1 ? (rs > 2 ? " 4x" : " 2x") : "", compileNote);
            } else port_log_summary("fps: %.1f s: shown %.1f fps; game %.1f turns/s; %u skipped; %u hitches over 100 ms (longest %.0f ms); picture %dx%d%s%s",
                             seconds, (w.shown - sPerf.shown) / seconds, (w.frames - sPerf.frames) / seconds, w.skipped - sPerf.skipped, hitches,
                             longest / 1e6, rw, rh, rs > 1 ? (rs > 2 ? " 4x" : " 2x") : "", compileNote);
            hitches = 0;
            longest = 0;
#if defined(__vita__)
            // sched24: "cores-5s:" (port/cores_report.h): each thread's CPU a picture on each core over this window (GXV_CORES_LOG; GXV_CORE_PROBE)
            if (port_vita_cores_report) port_vita_cores_report(seconds, static_cast<unsigned>(w.shown - sPerf.shown), static_cast<unsigned>(w.frames - sPerf.frames));
#endif
            CharaPerfLine();
#if defined(GXV_DRAW_REJECT_COUNTERS) && GXV_DRAW_REJECT_COUNTERS
            vgx::backend::DrawRejectReport();
#endif
            static unsigned windows = 0;
            if (++windows % 6 == 0 && port_host_heap_summary) { port_host_heap_summary(); port_vgx_census_log("every 30 s"); }  // (every 30 s: the C heap's use, so a slow growth shows before it runs out)
        }
        sPerf = w;
        return;
    }
    static int64_t beWall = 0;
    static uint64_t draws = 0;
    beWall += execNs;
    draws += drawCalls;
    const int64_t now = port_host_time_ns();
    if (!sPerf.t0) {
        sPerf.t0 = now;
        return;
    }
    if (now - sPerf.t0 < static_cast<int64_t>(sPerfSeconds) * 1000000000ll) return;
    PerfWindow w;
    int64_t maxWork = 0;
    port_perf_game_totals(&w.work, &w.frames, &w.gameCpu, &maxWork);
    w.t0 = now;
    w.plCpu = sPlCpuUs.load(std::memory_order_relaxed);
    w.beCpu = port_thread_cpu_us();
    w.beWall = beWall;
    w.wait = sGmWaitNs.load(std::memory_order_relaxed);
    w.queued = sFramesQueued.load();
    w.shown = sFramesShown.load();
    w.draws = draws;
    w.records = sPlRecords.load(std::memory_order_relaxed);
    w.prims = sPlPrims.load(std::memory_order_relaxed);
    w.skipped = port_frameskip_count();
    const double secs = (now - sPerf.t0) / 1e9;
    const unsigned gf = static_cast<unsigned>(w.queued - sPerf.queued), sf = static_cast<unsigned>(w.shown - sPerf.shown);
    const unsigned lf = w.frames - sPerf.frames; // frame-loop turns (a turn without a display copy is a loading frame)
    const double g = gf ? gf : 1.0, s = sf ? sf : 1.0, l = lf ? lf : 1.0;
    port_log("perf: %.1f s: game %.1f fps (%.1f loop turns/s, %u without a picture), shown %.1f fps; game thread %.1f ms/turn avg %.1f max, cpu %.1f, waited for player %.1f; "
             "player cpu %.1f ms/frame, %u primitives in %u draw records/frame; back end %.1f ms/frame wall, cpu %.1f, %u draws/frame",
             secs, gf / secs, lf / secs, w.skipped - sPerf.skipped, sf / secs, (w.work - sPerf.work) / 1e6 / l, maxWork / 1e6, (w.gameCpu - sPerf.gameCpu) / 1e3 / l,
             (w.wait - sPerf.wait) / 1e6 / l, (w.plCpu - sPerf.plCpu) / 1e3 / g, static_cast<unsigned>((w.prims - sPerf.prims) / (gf ? gf : 1)),
             static_cast<unsigned>((w.records - sPerf.records) / (gf ? gf : 1)), (w.beWall - sPerf.beWall) / 1e6 / s, (w.beCpu - sPerf.beCpu) / 1e3 / s,
             static_cast<unsigned>((w.draws - sPerf.draws) / (sf ? sf : 1)));
    {
        static uint64_t lastSl[5] = {};
        uint64_t sl[5];
        vgx::StateListCounters(sl);
        if (sl[0] != lastSl[0] || sl[1] != lastSl[1] || sl[4] != lastSl[4])
            port_log("perf: state lists: %llu replays, %llu parses, %llu not cacheable%s%llu verified, %llu mismatching",
                     (unsigned long long)(sl[0] - lastSl[0]), (unsigned long long)(sl[1] - lastSl[1]), (unsigned long long)(sl[2] - lastSl[2]),
                     "; ", (unsigned long long)(sl[3] - lastSl[3]), (unsigned long long)(sl[4] - lastSl[4]));
        std::memcpy(lastSl, sl, sizeof sl);
    }
    {
        // HD texture replacement (vgx_hdtex.cpp): logged when something happened since the last line.
        static uint64_t lastHashed = 0, lastHits = 0;
        vgx::hdtex::Counters hc;
        vgx::hdtex::GetCounters(hc);
        if (hc.entries && (hc.hashed != lastHashed || hc.hits != lastHits))
            port_log("perf: hdtex: %llu replaced (%llu KiB read in %.1f ms, longest %.2f ms), %llu refused over budget, %llu uploads hashed; "
                     "lookups %.2f ms in all; HD textures alive at most %llu KiB",
                     (unsigned long long)hc.hits, (unsigned long long)(hc.bytesRead >> 10), hc.readNs / 1e6, hc.readMaxNs / 1e6,
                     (unsigned long long)hc.refused, (unsigned long long)hc.hashed, hc.lookupNs / 1e6, (unsigned long long)(hc.peakLive >> 10));
        lastHashed = hc.hashed;
        lastHits = hc.hits;
    }
    CharaPerfLine();
#if defined(GXV_DRAW_REJECT_COUNTERS) && GXV_DRAW_REJECT_COUNTERS
    vgx::backend::DrawRejectReport();
#endif
    {
        // Front-end split (GXV_SPLIT, vgx_fesplit.h): the finishing thread's CPU per frame, and how long the player waited for it.
        // ("caught up": the player's time at the frame ends, waiting for the finishing thread or doing what it had not done yet)
        static int64_t lastCpu = 0, lastWait = 0;
        static uint32_t lastFrames = 0, lastStolen = 0;
        int64_t cpu = 0, wait = 0;
        uint32_t frames = 0;
        vgx::split::Counters(cpu, wait, frames);
        const uint32_t stolen = vgx::split::StolenOps();
        if (frames != lastFrames) {
            const double n = frames - lastFrames;
            port_log("perf: front-end split: %u frames, finishing thread cpu %.1f ms/frame; the player caught up with it for %.2f ms/frame, "
                     "doing %.0f of its operations/frame itself",
                     frames - lastFrames, (cpu - lastCpu) / 1e3 / n, (wait - lastWait) / 1e6 / n, (stolen - lastStolen) / n);
        }
        lastCpu = cpu;
        lastWait = wait;
        lastFrames = frames;
        lastStolen = stolen;
    }
    {
        // GXV_RSCACHE / GXV_REGSHADOW / GXV_TEXMEMO (vgx): memo use, and the verify modes' checks (=2) since the last line
        static uint64_t last[9] = {};
        uint64_t c[9];
        vgx::ResolveCacheCounters(c);
        vgx::RegShadowCounters(c + 4);
        vgx::TexMemoCounters(c + 6);
        port_log("perf: fecache: key memo %llu hits %llu misses, texture memo %llu hits; verified: resolutions %llu (%llu mismatching), "
                 "register writes %llu (%llu mismatching), bindings %llu (%llu mismatching)",
                 (unsigned long long)(c[2] - last[2]), (unsigned long long)(c[3] - last[3]), (unsigned long long)(c[6] - last[6]),
                 (unsigned long long)(c[0] - last[0]), (unsigned long long)(c[1] - last[1]), (unsigned long long)(c[4] - last[4]),
                 (unsigned long long)(c[5] - last[5]), (unsigned long long)(c[7] - last[7]), (unsigned long long)(c[8] - last[8]));
        std::memcpy(last, c, sizeof c);
    }
    {
        // GXV_FE_PREFETCH / GXV_VARFRONT (vgx): top-level CALL_DLs and how many of them had the next call hinted one call ahead (the
        // look-ahead stops at immediate draws); new draw variants found in front of the warm-up list's seen-sets, and not found there
        static uint64_t last[4] = {};
        uint64_t c[4];
        vgx::FePrefetchCounters(c);
        vgx::VarFrontCounters(c + 2);
        if (c[0] != last[0] || c[2] != last[2] || c[3] != last[3])
            port_log("perf: fe-prefetch: %llu top-level display-list calls, %llu with the next one hinted ahead (GXV_FE_PREFETCH=%d); "
                     "variant front %llu hits %llu misses",
                     (unsigned long long)(c[0] - last[0]), (unsigned long long)(c[1] - last[1]), vgx::FePrefetchDepth(),
                     (unsigned long long)(c[2] - last[2]), (unsigned long long)(c[3] - last[3]));
        std::memcpy(last, c, sizeof c);
    }
    {
        // GXV_RIGIDMERGE: rigid draws made into one record with the matrix palette (nothing is printed while the switch is off)
        static uint64_t last[2] = {};
        uint64_t c[2];
        vgx::RigidMergeCounters(c);
        if (c[0] != last[0] || c[1] != last[1])
            port_log("perf: rigidmerge: %llu palette runs, %llu draws joined them (draw calls saved)", (unsigned long long)(c[0] - last[0]),
                     (unsigned long long)(c[1] - last[1]));
        std::memcpy(last, c, sizeof c);
    }
    {
        // GXV_STATICCULL: static runs (the cached lists' static buffers: the map's shapes) outside the clip volume, not drawn
        static uint64_t last[3] = {};
        uint64_t c[3];
        vgx::StaticCullCounters(c);
        if (c[0] != last[0]) {
            const double n = gf ? gf : 1;
            port_log("perf: static cull: %.1f of %.1f static runs/frame outside the view, not drawn (%.0f%%, %.0f vertices/frame)",
                     (c[1] - last[1]) / n, (c[0] - last[0]) / n, 100.0 * (c[1] - last[1]) / static_cast<double>(c[0] - last[0]), (c[2] - last[2]) / n);
        }
        std::memcpy(last, c, sizeof c);
    }
    {
        // GXV_RIGIDMERGE_DLMAX / GXV_RIGIDMERGE_DLLEARN: cached lists copied into the arena to join palette runs (printed when there were any, or learning is on)
        static uint64_t last[8] = {};
        uint64_t c[8];
        vgx::DlLearnCounters(c);
        if (c[0] != last[0] || c[4] != last[4]) {
            const double n = gf ? gf : 1;
            port_log("perf: list copies: %.1f/frame (%.0f KiB/frame), %.1f of them joined a run; static lists %.1f/frame, %.1f of them could have joined; "
                     "%.2f MiB of vertices kept for %llu lists",
                     (c[0] - last[0]) / n, (c[1] - last[1]) / 1024.0 / n, (c[2] - last[2]) / n, (c[4] - last[4]) / n, (c[5] - last[5]) / n, c[6] / 1048576.0,
                     (unsigned long long)c[7]);
        }
        std::memcpy(last, c, sizeof c);
    }
    sPerf = w;
}

// ------------------------------------------------------------------ address hook
// Console address (as the game's GX library writes it into a register) -> host memory.
// Physical addresses below 0x40000000 go through the port's region table; anything above
// is a native pointer (Vita, PETARI_NATIVE_ADDRESSES) or a cached / uncached console alias.
struct Window { uint32_t lo = 1, hi = 0; const uint8_t* data = nullptr; };
Window sPhysWin[4], sNatWin[4];
unsigned sPhysNext, sNatNext;

const uint8_t* Resolve(uint32_t addr, uint32_t* avail) {
    *avail = 0;
    if (addr < 0x40000000u) {
        for (const Window& w : sPhysWin)
            if (addr >= w.lo && addr < w.hi) { *avail = w.hi - addr; return w.data + (addr - w.lo); }
        void* data = nullptr;
        uint32_t base = 0;
        size_t bytes = 0;
        if (!port_physical_window(addr, &data, &base, &bytes) || !data) return nullptr;
        Window& w = sPhysWin[sPhysNext++ & 3];
        w.lo = base; w.hi = base + static_cast<uint32_t>(bytes); w.data = static_cast<const uint8_t*>(data);
        *avail = w.hi - addr;
        return w.data + (addr - w.lo);
    }
    for (const Window& w : sNatWin)
        if (addr >= w.lo && addr < w.hi) { *avail = w.hi - addr; return w.data + (addr - w.lo); }
    void* base = nullptr;
    size_t bytes = 0;
    if (!port_native_window(reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)), &base, &bytes) || !base) return nullptr;
    Window& w = sNatWin[sNatNext++ & 3];
    w.lo = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base));
    w.hi = w.lo + static_cast<uint32_t>(bytes);
    w.data = static_cast<const uint8_t*>(base);
    *avail = w.hi - addr;
    return w.data + (addr - w.lo);
}

// ------------------------------------------------------------------ pixel-engine sync
void PeFinish() { port_irq_raise(PORT_IRQ_PE_FINISH); }
void PeToken(uint16_t token, bool interrupt) {
    reinterpret_cast<volatile uint16_t*>(__PortGXPERegs)[7] = token;
    if (interrupt) port_irq_raise(PORT_IRQ_PE_TOKEN);
}

// ------------------------------------------------------------------ back-end thread
// Owns the GL context. Frames (command buffers) are handed over through a short queue: the
// front end blocks only when `sMaxInFlight` frames are waiting, which is what paces a game
// that produces frames faster than they can be drawn.
std::mutex sMtx;
std::condition_variable sCv;
// crash36 (15 Nov): these were std::vector. The global operator new (JKRHeap.cpp gameAlloc) serves a game thread outside a PortHostAllocScope from the game's CURRENT heap,
// which in play is the scene's heap, and the game thread pushes here: port_hrw_undrawn_begin / port_hrw_fresh_draw -> HrwSendAsk -> PushAskLocked -> sJobs.push_back, with no
// scope. When that push was the one that made the vector grow, the queue's storage moved into the scene's heap and died with the scene: the next scene built its models over
// it (the dumps: joint matrices and J3DMatPacket / J3DShapePacket around sJobs's buffer, sJobs.front() 0 / 0x200 / garbage) and every later push wrote a CmdBuf* (a C-heap
// address) into one of the new scene's objects (the J3DShape whose display list was "an unregistered native physical-address source"). The fresh player made it common: a
// pair then has two tiny jobs (the plain ask, which waits at the queue's head for the partial draw's deadline, and the partial draw's job), so the queue reaches 5 entries
// (the growth from 4 to 8) in play, with the ask's push as the fifth. PortHostVector (port/host_vector.h, the sStaleDL crash of 6 Nov) allocates from the C heap on any
// thread. Jc36 below counts the pushes that would have moved the old vector into the game's heap ("crash36:" lines).
PortHostVector<vgx::CmdBuf*> sFree, sJobs;
PORT_HEAP_GUARD(sFree);
PORT_HEAP_GUARD(sJobs);
struct Jc36 {
    unsigned oldCap = 0;    // the capacity a std::vector would have now (it doubles: 1, 2, 4, 8 ...)
    unsigned game = 0;      // of its growths, those a game thread's push would have made: storage from the game's current heap
    unsigned gameCap = 0, gameSize = 0;  // the last such: the capacity grown to, the jobs queued
    unsigned logged = 0, reported = 0;
} sJc36;  // (sMtx)
// Before every sJobs.push_back, sMtx held.
inline void Jc36PushLocked() {
    const unsigned need = static_cast<unsigned>(sJobs.size()) + 1;
    if (need <= sJc36.oldCap) return;
    sJc36.oldCap = sJc36.oldCap ? sJc36.oldCap * 2 : 1;
    if (!port_use_game_heap()) return;
    ++sJc36.game;
    sJc36.gameCap = sJc36.oldCap;
    sJc36.gameSize = need;
}
unsigned sMaxInFlight = 1;
// How many frames the back end may hold at once (the one it draws and the ones waiting for it; sMaxInFlight + 1 buffers exist).
// Every frame that waits is a frame of delay between a press and the picture it causes.  Where the back end is the bottleneck (a
// busy scene at 30 fps) a waiting frame buys nothing, so the limit drops to one and the press shows up 1-2 frames sooner; where
// it is not (60 fps scenes) the queue evens out the frames' unequal times, and the full limit stays.  Guarded by sMtx.
unsigned sQueueLimit = 1;
bool sAdaptDepth = false;
std::atomic<uint64_t> sGateWaitN{0}, sGateWaitUs{0}, sGateWaitMaxUs{0};  // FrameSink waits (GXV_WAITLOG)
std::atomic<uint64_t> sGatePeekMaxUs{0};  // (smooth25) the longest FrameSink wait since port_vgx_gate_peek_take

// ---- endwait26 (GXV_ENDWAIT_LOG=1, default 0; vgx_endwait.h). The front end's record of each picture (the player thread writes one at the picture's end) and the
// report of a long game-thread wait at the end of the frame (port_vgx_endwait_line, below). Off: one flag read a picture and one a player sleep.
bool EwOn() {
    static const bool on = vgx::EnvInt("GXV_ENDWAIT_LOG", 0) != 0;
    return on;
}
vgx::endwait::Ring sEwRing;
vgx::endwait::StageClock sEwStage;
std::atomic_flag sEwLock = ATOMIC_FLAG_INIT;
int64_t sEwGateNs = 0;                 // the last FrameSink wait (player thread; FrameSink's census writes it)
int64_t sEwStart = 0, sEwIdleNs = 0;   // player: this picture's first parsed byte; the time asleep since then
bool sEwBaseValid = false;
struct EwBase { uint64_t decUs = 0, decN = 0, decBytes = 0, dlNs = 0, dlN = 0, drainNs = 0, settleNs = 0; } sEwBase;  // player: the cumulative counters at the picture's start
EwBase EwNow() {
    EwBase b;
    if (vgx_tex_decode_us) b.decUs = vgx_tex_decode_us();
    if (vgx_tex_decode_counts) vgx_tex_decode_counts(&b.decN, &b.decBytes);
    uint64_t dl[2] = {0, 0};
    vgx::DlBuildCounters(dl);
    b.dlNs = dl[0]; b.dlN = dl[1];
    int64_t cpu = 0, wait = 0;
    uint32_t frames = 0;
    vgx::split::Counters(cpu, wait, frames);
    b.drainNs = wait > 0 ? static_cast<uint64_t>(wait) : 0;
    b.settleNs = vgx::fepar::SettleNs();
    return b;
}
inline uint32_t EwUs(uint64_t ns) { const uint64_t u = ns / 1000; return u > 0xFFFFFFF0u ? 0xFFFFFFF0u : static_cast<uint32_t>(u); }
inline uint32_t EwUsD(uint64_t a, uint64_t b) { return a >= b ? EwUs(a - b) : 0u; }
// The player's side, at the end of a picture's FrontPresent: `tDisp` is DisplayCopy's start, `tList` after the join with the finishing thread, `frame` the picture's number.
// ---- pace33 PATCH4: GXV_FE_LANE (default 1; 0 = off; port/fe_lane.h): the "fe-lane:" line, every 5 s. The game thread notes each drawn turn's start under its stamp (LaneTurn); the player
// times its picture (first byte, last sleep's end, display copy, join, hand-over, the queue wait inside it, its own CPU time). Off: one flag read at each of those points.
bool LaneOn() {
    static const bool on = vgx::EnvInt("GXV_FE_LANE", 1) != 0;
    return on;
}
struct LaneTurnNote { std::atomic<uint32_t> tag{0}; std::atomic<int64_t> ns{0}; std::atomic<uint32_t> gapUs{0}; };
LaneTurnNote sLaneTurn[8];
std::atomic<int64_t> sLaneDrawEndNs{0};   // (PATCH5) the game's draw of the current picture ended (the thread that draws writes it)
int64_t sLaneFirstNs = 0, sLaneBusyFromNs = 0, sLaneIdleNs = 0, sLaneGateNs = 0, sLaneCpuUs = 0, sLaneWindowNs = 0;   // player thread
port::felane::Census sLane;
// game thread, a drawn turn's start
void LaneTurn(uint32_t stamp16, int64_t nowNs) {
    static int64_t last = 0;
    LaneTurnNote& n = sLaneTurn[stamp16 & 7u];
    n.tag.store(0, std::memory_order_relaxed);
    n.ns.store(nowNs, std::memory_order_relaxed);
    const int64_t gap = last ? nowNs - last : 0;
    n.gapUs.store(gap > 0 && gap < 1000000000ll ? static_cast<uint32_t>(gap / 1000) : 0u, std::memory_order_relaxed);
    n.tag.store(0x10000u | (stamp16 & 0xFFFFu), std::memory_order_release);
    last = nowNs;
}
// player thread, at the end of a picture's FrontPresent. stamp: the picture's present stamp (0: none, not counted).
void LanePictureDone(uint32_t stamp, int64_t tDisp, int64_t tList) {
    const int64_t now = port_host_time_ns();
    const int64_t cpu = port_thread_cpu_us();
    if ((stamp & 0xC0000000u) == 0x80000000u && sLaneFirstNs && (!port_screen_quiet || port_screen_quiet() == 0)) {
        const LaneTurnNote& n = sLaneTurn[stamp & 7u];
        const int64_t t0 = n.ns.load(std::memory_order_relaxed);
        if (n.tag.load(std::memory_order_acquire) == (0x10000u | (stamp & 0xFFFFu)) && t0 > 0 && tDisp >= t0 && now - t0 < 500000000ll) {
            auto us = [](int64_t ns) { return ns > 0 ? static_cast<uint32_t>(ns / 1000) : 0u; };
            port::felane::Pic p;
            // (PATCH5: the split of PATCH4 took "the player's last sleep" as the end of the game's part, but the player does not sleep again once the draw's bytes flow (console 2730:
            //  "game 3.9 ms, backlog 9.1" with a 6.5 ms draw): the backlog held the game's own draw. The game thread now notes when its draw ended (port_vgx_lane_draw_end, from
            //  GameSystem::portDrawEnd): game = turn start -> draw end; front end behind = draw end -> the player reaches the display copy.)
            const int64_t drawEnd = sLaneDrawEndNs.load(std::memory_order_relaxed);
            const int64_t busyFrom = drawEnd > t0 && drawEnd < tDisp ? drawEnd : (drawEnd >= tDisp ? tDisp : (sLaneBusyFromNs > t0 ? sLaneBusyFromNs : t0));
            const int64_t gate = sLaneGateNs > 0 ? sLaneGateNs : 0;
            const int64_t tail = tDisp > busyFrom ? tDisp - busyFrom : 0;
            p.us[port::felane::kGame] = us(tDisp - tail - t0);
            p.us[port::felane::kTail] = us(tail);
            p.us[port::felane::kJoin] = us(tList - tDisp);
            p.us[port::felane::kPass] = us(now - tList - gate);
            p.us[port::felane::kQueue] = us(gate);
            const int64_t wall = now - sLaneFirstNs - sLaneIdleNs, onCpu = (cpu - sLaneCpuUs) * 1000;
            p.lostUs = us(wall - onCpu);
            p.turnGapUs = n.gapUs.load(std::memory_order_relaxed);
            sLane.Add(p);
        }
    }
    sLaneFirstNs = 0; sLaneBusyFromNs = now; sLaneIdleNs = 0; sLaneGateNs = 0; sLaneCpuUs = cpu;
    if (!sLaneWindowNs) sLaneWindowNs = now;
    if (now - sLaneWindowNs >= 5000000000ll) {
        if (sLane.Count() >= 20 && port_log_summary) {
            static char line[1024];   // (player thread only)
            port::felane::Format(line, sizeof line, sLane.Summarise(), (now - sLaneWindowNs) / 1e9);
            port_log_summary("%s", line);
        }
        sLane.Clear();
        sLaneWindowNs = now;
    }
}
void EwPictureDone(int64_t tDisp, int64_t tList, uint64_t frame, const vgx::stats::Frame& fs) {
    const int64_t now = port_host_time_ns();
    const EwBase b = EwNow();
    if (sEwStart && sEwBaseValid) {
        vgx::endwait::Pic p;
        p.frame = frame;
        p.startNs = sEwStart;
        p.endNs = now;
        p.idleUs = EwUs(static_cast<uint64_t>(sEwIdleNs > 0 ? sEwIdleNs : 0));
        p.decodeUs = EwUsD(b.decUs, sEwBase.decUs);
        p.decodeN = static_cast<uint32_t>(b.decN - sEwBase.decN);
        p.decodeKiB = static_cast<uint32_t>((b.decBytes - sEwBase.decBytes) / 1024u);
        p.dlUs = EwUs(b.dlNs >= sEwBase.dlNs ? b.dlNs - sEwBase.dlNs : 0);
        p.dlN = static_cast<uint32_t>(b.dlN - sEwBase.dlN);
        p.joinUs = EwUs(static_cast<uint64_t>(tList - tDisp > 0 ? tList - tDisp : 0));
        p.drainUs = EwUsD(b.drainNs, sEwBase.drainNs);
        p.settleUs = EwUsD(b.settleNs, sEwBase.settleNs);
        const uint64_t gate = static_cast<uint64_t>(sEwGateNs > 0 ? sEwGateNs : 0);
        p.gateUs = EwUs(gate);
        const uint64_t present = static_cast<uint64_t>(now - tList > 0 ? now - tList : 0);
        p.passUs = EwUs(present > gate ? present - gate : 0);
        p.upN = fs.texUploads;
        p.upKiB = static_cast<uint32_t>(fs.texUploadBytes / 1024u);
        p.draws = fs.drawCalls;
        while (sEwLock.test_and_set(std::memory_order_acquire)) {}
        sEwRing.Push(p);
        sEwStage.Seen(port_scene_key ? port_scene_key() : 0, port_screen_quiet ? port_screen_quiet() : 0, now);
        sEwLock.clear(std::memory_order_release);
    }
    sEwGateNs = 0;
    sEwStart = now;  // (the next picture starts here; the player's sleeps from now on count as its idle time)
    sEwIdleNs = 0;
    sEwBase = b;
    sEwBaseValid = true;
}
// GXV_LATENCY24_DEPTH=n (default 0 = off): at most n frames in the back end's queue (the one it executes and n - 1 waiting), whatever the adaptation
// above allows: a waiting frame is a frame of lag. The most frames queued since the last port_vgx_queue_max_take() (the "latency:" line).
unsigned sDepthCap = 0;
// GXV_FIF=n (strk27, default 0 = off; 2..4): the display-end frames-in-flight gate (port/fif.h, from Strikers' MSC_LAT_DEPTH): the game thread hands a picture over only when at most n
// pictures are between the hand-over and the display's flip (counted at the display callback, not at the end of the back end), waiting at most three refreshes. The "fif:" census line says what it did.
port::fif::Gate sFif;
std::atomic<unsigned long long> sFifShown{0};
unsigned sFifResyncs = 0;   // (PATCH3; game thread, under sMtx)
unsigned sFifLogAt = 0;
std::atomic<unsigned> sQueueMax{0};
bool sWaitLog = false;     // GXV_WAITLOG, read once in port_gx_select_backend
bool sFixedDepth = false;  // GXV_FIXED_DEPTH, read once in port_gx_select_backend
// Where the back end is, for the stall report: 0 waiting for a frame, 1 executing it, 2 after it (the counters, the picture
// copy), and when it got there (host ns).
std::atomic<int> sBackStage{0};
std::atomic<int64_t> sBackSince{0};
bool sSkipWhenBehind = false;
bool sSkipAlways = false;   // MP3 (harness): GXV_SKIP_ALL=1 (desktop, default off): no frame is rasterised unless it is captured (see RenderMain)
bool sStarted = false, sStartOk = false, sThreadMade = false;
pthread_t sThread;
// capture request (desktop)
std::string sCapPath;
bool sCapWanted = false, sCapDone = false;
unsigned long long sCapFrame = 0;

#if defined(__vita__)
port::settings::HoldGate sSettingsRenderHold;
void ServiceSettingsHold(std::unique_lock<std::mutex>& lock) {
    const unsigned generation = sSettingsRenderHold.Requested();
    if (!generation || !sJobs.empty()) return;
    lock.unlock();
    const bool finished = port_vgx_vita_settings_finish(0) != 0;
    if (finished) {
        // GPU waits cannot be cancelled. A timed-out request must resume its
        // scene, not park later while the guest has already returned to play.
        if (sSettingsRenderHold.Acknowledge(generation)) {
            lock.lock();
            while (sSettingsRenderHold.Requested() == generation)
                sCv.wait_for(lock, std::chrono::milliseconds(2));
            lock.unlock();
        }
        port_vgx_vita_settings_resume();
    } else {
        sSettingsRenderHold.Cancel(generation);
    }
    lock.lock();
}
#endif
// ---- GXV_HRW (hrw26, vgx_hrw.h): the render thread keeps the buffer of a picture the game opened a pair with, and executes it a second time for the pair's
// second picture with the camera change on its projection blocks. The game asks for the second picture with port_hrw_undrawn_begin (below), at the turn after the
// pair's first; the ask travels as a tiny job (no commands) behind the first picture's job in the render queue, so the order is the pictures' order whichever of
// the two threads is ahead. The tiny jobs come from their own few, do not count against the queue's limit (FrameSink's wait), and are never blocked on.
PortHostVector<vgx::CmdBuf*> sHrwSpare;   // tiny jobs not in use (sMtx; crash36: host memory, see sJobs)
PORT_HEAP_GUARD(sHrwSpare);
unsigned sHrwTiny = 0;                  // tiny jobs in sJobs (sMtx)
uint32_t sHrwQueued = 0;                // the pair number of the last picture FrameSink queued with one (sMtx)
struct HrwAsk { bool valid = false; uint32_t id = 0, stamp = 0; float d[12] = {}; } sHrwAsk;  // an ask that came before its picture's job (sMtx)
// render-side counters, read by the game thread's log: 0 asks queued, 1 replays run, 2..9 drops by vgx::hrw::WhyName, 10 verify runs, 11 verify bad, 12 kept pictures, 13 kept but unusable
// (hrwfeel36) a pair's second picture lost: game side (the pair was opened and the second picture not asked for) or render side (dropped on its way)
static double HrwFeelNowMs() { return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0; }
static void HrwFeelLost(int side) {
    std::lock_guard<std::mutex> lk(port::hrwfeel::GlobalMutex());
    port::hrwfeel::Global().Lost(HrwFeelNowMs(), side);
}
std::atomic<uint64_t> sHrwRt[16];
// hrw27 (render side, read by the game thread's log): 0 partial draws received, 1 composed, 2 refused, 3..9 refusals by vgx::hrw::RefusalName (reason - 1 + 3), 10 draws removed, 11 draws inserted, 12 fresh draws left out (no owner in common),
// 13 compose+undo microseconds, 14 composite buffers that failed validation, 15 verify-fresh draws compared, 16 ... of them off by more than 0.25 px, 17 verify-fresh owners with a different draw count, 18 verify-fresh compose checks that failed,
// 19 verify-fresh runs, 20 sum of the probe differences (1/1000 px), 21 largest probe difference (1/1000 px), 22 resource-only partial draws (no replay), 23 owners seen in the partial draws (sum)
std::atomic<uint64_t> sHrwFr[25];  // (replay36: [24] partial draws that came with a picture's commands)
// warp32 PATCH23, GXV_HRW_FRESH_DEADLINE_MS10 (tenths of a ms, default 90 = 9.0 ms; 0 = off, the ask travels on the partial draw as before; mode 5 with the fresh player):
// the second picture is never late because of the fresh player. The undrawn turn sends the pair's PLAIN ask at once (as a pair without the fresh player does) and notes when
// the partial draw must have reached the render thread: the ask's time + this. The render thread, coming to the plain ask: the partial draw's job is already queued behind
// it -> the plain ask is dropped and the partial draw's job is the second picture (as before); not yet -> it waits for it until the deadline, no longer; still not there ->
// the plain re-projection (mode 4's box) goes out now and the partial draw, when it comes, is resource commands only (uploads, shaders, frees; no picture; its job goes
// back to its own pool as every partial-draw job does). c139-c142 on the console: with the ask on the partial draw (after the undrawn turn's update) Honeyhive showed about
// 3 late pictures in 10 s at latency level 3 (75-80 ms against 70 without the fresh player).
struct HrwProv { uint32_t id = 0; int64_t deadlineNs = 0; uint8_t state = 0; };  // state: 0 free, 1 the partial draw is expected, 2 none comes (no draw this turn), 3 the plain picture went out
HrwProv sHrwProv[8];   // (sMtx)
unsigned sHrwProvNext = 0;
std::atomic<uint32_t> sHrwDl[6];  // [0] partial draw in time, [1] missed its deadline, [2] of [1] its job came later (resources only), [3] no partial draw that pair, [4] waits, [5] us waited
long HrwFreshDeadline10() {
    static const long v = std::clamp<long>(vgx::EnvInt("GXV_HRW_FRESH_DEADLINE_MS10", 90), 0, 300);  // (review 14 Nov: at most 30 ms, so the 8-entry ring cannot wrap during a wait)  // (the emulator's flip-books need 0: its turns take 100+ ms)
    return v;
}
HrwProv* HrwProvFindLocked(uint32_t id) {
    for (HrwProv& e : sHrwProv) if (e.state && e.id == id) return &e;
    return nullptr;
}
// mariolag31, GXV_HRW_MARIOTIME (port/mario_time.h; default 0): no second picture with Mario's old pixels. Render-side counters, read by the game thread's census:
// [0] deadline missed -> stale (the plain warp went out), [1] deadline missed -> held, [2] not merged -> stale, [3] not merged -> held, [4] merged
std::atomic<uint32_t> sHrwMt[5];
std::atomic<bool> sHrwMtLimited{false};   // lost49: HoldLimit::stale, for the render thread's holds (deadline missed / not merged)
bool MarioTimeLimitKey() { static const bool on = vgx::EnvInt("GXV_HRW_MARIOTIME_LIMIT", 1) != 0; return on; }
int MarioTimeMode() {
    static const int m = port::mariotime::ClampMode(vgx::EnvInt("GXV_HRW_MARIOTIME", 0));
    return m;
}
bool MarioTimeCensus() {
    static const bool on = MarioTimeMode() != 0 || vgx::EnvInt("GXV_HRW_MARIOTIME_CENSUS", 0) != 0;
    return on;
}
bool sHrwFreshOn = false;  // GXV_HRW_FRESH on and a mode that keeps the picture (set once, in EnsureThread's first call and HrwFreshInit)
PortHostVector<vgx::CmdBuf*> sHrwFreshSpare;   // (crash36: host memory, see sJobs) the partial draw's jobs not in use (sMtx): the front end's buffer is copied into one (FrameSink); its capacity stays here
PORT_HEAP_GUARD(sHrwFreshSpare);  // (heap36: the fourth list of crash36 is checked by GXV_HEAP_GUARD too)
// gov5, GXV_HRW_GOV5 (Performance column of mode_keys.h: 1; code default 0 = mode 5 pairs every settled stage, as before): the governor drives mode 5 too. "60 Assist" is a step down from full 60:
// a stage starts on plain 60 (no pairs, the rhythm of the plain target), the pairs come only where plain 60 cannot be held and the GPU ladder has nothing (more) to give, and go when plain 60 would hold.
bool Gov5Key() { static const bool on = vgx::EnvInt("GXV_HRW_GOV5", 0) != 0; return on; }
bool HrwForceOn() { static const bool on = vgx::EnvInt("GXV_HRW_FORCE", 0) != 0; return on; }  // (hrw29, emulator / testing: mode 1 engages at once in every settled stage, whatever the governor would say)
std::atomic<int> sHrwDisabled{0};  // (hrw29) 1 once HRW switched itself off (HrwDisable): the game then makes no more pairs, partial draws or asks; what is queued runs out
std::atomic<uint64_t> sHrwRtLoops{0};  // (hrw30) jobs the render thread has finished: the progress FrameSink's partial-draw wait watches
unsigned sHrwFreshExtra = 0;       // (hrw29, sMtx) partial-draw jobs made beyond the two reserved ones because none was free
// hrw29: HRW switches itself off for good (this run) with a reason in the log, instead of hanging or killing the game. Any thread.
std::atomic<int> sHrwFreshOff{0};  // (warp32 PATCH16) the partial draw is given up for this run (mode 5 goes on with the plain warp); set once, with a log line
void HrwFreshGiveUp(const char* why) {
    if (sHrwFreshOff.exchange(1)) return;
    if (port_log_summary) port_log_summary("hrw: the fresh player draw is switched off for this run (%s): pairs go on with the plain warp", why);
}
void HrwDisable(const char* why) {
    if (sHrwDisabled.exchange(1)) return;
    if (port_log_summary) port_log_summary("hrw: DISABLED: %s", why);
}
struct HrwFreshAsk { bool valid = false; uint32_t id = 0, stamp = 0; float d[12] = {}; } sHrwFreshAsk;  // the pair's second-picture parameters, waiting for the partial draw's buffer (sMtx)
namespace hrwrt {
vgx::CmdBuf* g_ret = nullptr;          // the kept buffer (render thread only from here)
uint32_t g_retId = 0;
bool g_retOk = false;
vgx::hrw::Plan g_plan;
vgx::hrw::Scratch g_scr;
uint64_t g_hash = 0;
int Mode() { return vgx::HrwMode(); }
vgx::hrw::Undo g_undo;
vgx::hrw::OwnerProbes g_probesFresh, g_probesFull;
bool g_probesValid = false;
uint32_t g_probesPair = 0;
bool KeepOn() { return (Mode() == vgx::hrw::kOn || Mode() == vgx::hrw::kVerifyStill || Mode() == vgx::hrw::kVerifyFresh || Mode() == vgx::hrw::kWarp) && g_ret != nullptr; }  // (mode 5 keeps the first picture's buffer only with GXV_HRW_WARP_TRUTH=1: the true redraw for the stills)
bool g_truthRun = false;  // (hrw31) this ask is a true redraw (the warp picture was only dumped)
// Every offset a draw of the buffer names lies inside the buffer's arenas (a composite buffer that fails this is never executed).
uint32_t ValidateBuf(const vgx::CmdBuf& b) {
    uint32_t bad = 0;
    const size_t nd = b.draws.size();
    for (const vgx::Cmd& c : b.cmds) {
        if (c.type != vgx::CMD_DRAW) continue;
        if (c.arg >= nd) { ++bad; continue; }
        const vgx::DrawRec& d = b.draws[c.arg];
        for (int i = 0; i < vgx::UB_COUNT; ++i)
            if (d.ublock[i] != vgx::kNoBlock && (d.ublock[i] > b.uni.size() || b.uni.size() - d.ublock[i] < 4)) { ++bad; break; }
        if (d.vbuf == 0) {
            if (d.idxOffset > b.idx.size() || b.idx.size() - d.idxOffset < d.idxCount) ++bad;
            if (!b.vtxChunks.empty() && d.vchunk >= b.vtxChunks.size()) ++bad;
            else {
                const vgx::VtxLayout lay = vgx::MakeLayout(d.layoutMask);
                const size_t start = (b.vtxChunks.empty() ? 0 : b.vtxChunks[d.vchunk]) + static_cast<size_t>(d.vtxFirst) * lay.stride + d.vtxOffset;
                if (start > b.vtx.size() || b.vtx.size() - start < static_cast<size_t>(d.vtxCount) * lay.stride) ++bad;
            }
        }
    }
    return bad;
}
// replay36: the commands of `b` that name a record `b` does not hold (0: it can be executed). A partial draw's job is a COPY of the front end's buffer and a copy can leave a
// vector out (c168, console, Honeyhive: its command list came with a CMD_COPY_TEX and its copyTex did not: ExecuteFrame read record 0 of an empty vector). A few dozen commands.
uint32_t DanglingCmds(const vgx::CmdBuf& b) {
    uint32_t bad = 0;
    for (const vgx::Cmd& c : b.cmds) {
        size_t n = 0;
        switch (c.type) {
        case vgx::CMD_DRAW: n = b.draws.size(); break;
        case vgx::CMD_TEX_UPLOAD: n = b.uploads.size(); break;
        case vgx::CMD_SHADER: n = b.shaders.size(); break;
        case vgx::CMD_COPY_TEX: n = b.copyTex.size(); break;
        case vgx::CMD_COPY_DISP: n = b.copyDisp.size(); break;
        case vgx::CMD_SET_TARGET: n = b.targets.size(); break;
        case vgx::CMD_BUF_UPLOAD: n = b.bufUploads.size(); break;
        case vgx::CMD_PASS_BEGIN: n = b.passes.size(); break;
        case vgx::CMD_VARIANT: n = b.variants.size(); break;
        default: continue;  // (frees, programs, the present: the argument is not an index)
        }
        if (c.arg >= n) ++bad;
    }
    return bad;
}
// FrameSink / port_hrw_undrawn_begin, with sMtx held: the tiny job for pair `id`, or a counted drop.
void PushAskLocked(uint32_t id, uint32_t stamp, const float d[12]) {
    if (sHrwSpare.empty() || sHrwTiny >= 3) { sHrwRt[2 + 0].fetch_add(1, std::memory_order_relaxed); HrwFeelLost(port::hrwfeel::kRender); return; }
    vgx::CmdBuf* r = sHrwSpare.back();
    sHrwSpare.pop_back();
    r->clear();
    r->hrwReplayId = id;
    r->presentStamp = stamp;
    r->presentAllowed = true;
    std::memcpy(r->hrwD, d, sizeof r->hrwD);
    Jc36PushLocked();
    sJobs.push_back(r);
    ++sHrwTiny;
    sHrwRt[0].fetch_add(1, std::memory_order_relaxed);
}
// The render thread, before an ordinary buffer: the frees the kept buffer owed are made now (this buffer is later than the pair).
void BeforeOrdinary(const vgx::CmdBuf& job) {
    if (vgx::backend::HrwDeferredCount()) vgx::backend::HrwFlushDeferred();
    // verify-fresh: the picture after a partial draw (same turn, same state) is the full path the partial draw is compared with
    if (g_probesValid) {
        g_probesValid = false;
        vgx::hrw::CollectProbes(job, g_probesFull);
        const vgx::hrw::CompareResult r = vgx::hrw::CompareProbes(g_probesFresh, g_probesFull);
        sHrwFr[19].fetch_add(1, std::memory_order_relaxed);
        sHrwFr[15].fetch_add(r.drawsCompared, std::memory_order_relaxed);
        sHrwFr[16].fetch_add(r.drawsBad, std::memory_order_relaxed);
        sHrwFr[17].fetch_add(r.keysBadCount, std::memory_order_relaxed);
        sHrwFr[20].fetch_add(static_cast<uint64_t>(r.sumPx * 1000.0), std::memory_order_relaxed);
        const uint64_t mx = static_cast<uint64_t>(r.maxPx * 1000.0);
        uint64_t seen = sHrwFr[21].load(std::memory_order_relaxed);
        while (mx > seen && !sHrwFr[21].compare_exchange_weak(seen, mx, std::memory_order_relaxed)) {}
        static unsigned logged = 0;
        if ((r.drawsBad || r.keysBadCount) && logged++ < 12 && port_log_summary)
            port_log_summary("hrw: verify-fresh pair %u: %u owners, %u with a different number of draws (first: owner %u phase %u: %u draws fresh, %u in the full picture), %u draws compared, %u more than 0.25 px apart (largest %.2f px, mean %.3f px over %u points)",
                             g_probesPair, r.keys, r.keysBadCount, static_cast<unsigned>(r.badKey >> 8), static_cast<unsigned>(r.badKey & 0xFF), r.badKeyA, r.badKeyB, r.drawsCompared, r.drawsBad, r.maxPx, r.points ? r.sumPx / r.points : 0.0, r.points);
    }
}
// After an ordinary buffer was executed (frees listed if it was to be kept): keep it, or let it go.
void Retain(vgx::CmdBuf& job) {
    sHrwRt[12].fetch_add(1, std::memory_order_relaxed);
    vgx::hrw::Hazard h;
    const bool scanOk = vgx::hrw::Scan(job, h, g_scr);
    const bool planOk = scanOk && g_plan.Collect(job);
    g_retOk = false;
    g_retId = 0;
    if (!scanOk || !planOk) {
        sHrwRt[13].fetch_add(1, std::memory_order_relaxed);
        sHrwRt[2 + (!scanOk ? 3 : 4)].fetch_add(1, std::memory_order_relaxed); HrwFeelLost(port::hrwfeel::kRender);
        static unsigned logged = 0;
        if (logged++ < 8 && port_log_summary)
            port_log_summary("hrw: picture %u cannot be replayed (%s): set-target %u, texture re-upload after use %u, after free %u, buffer re-upload after use %u, after free %u, id range %u, display copies %u; projection plan: %u blocks, conflict %d, bad range %u",
                             job.hrwId, !scanOk ? "hazard" : "projection blocks", h.setTarget, h.texReuploadUsed, h.texReuseAfterFree, h.bufReuploadUsed, h.bufReuseAfterFree, h.idRange,
                             h.dispCopies, static_cast<unsigned>(g_plan.refs.size()), g_plan.conflict ? 1 : 0, g_plan.badRange);
        vgx::backend::HrwFlushDeferred();  // (nothing will replay it: its frees are due)
        return;
    }
    if (!job.presentAllowed) { sHrwRt[2 + 5].fetch_add(1, std::memory_order_relaxed); HrwFeelLost(port::hrwfeel::kRender); vgx::backend::HrwFlushDeferred(); return; }
    std::swap(*g_ret, job);  // (the kept buffer is the executed one; the job now holds the one kept before, which clear() empties)
    g_retId = g_ret->hrwId;
    g_retOk = true;
    if (Mode() == vgx::hrw::kVerifyStill) g_hash = vgx::hrw::HashBuf(*g_ret);
}
// A tiny job came up: the kept buffer ready to execute again, or null (counted).
bool g_warpFresh = false;  // (PATCH15: the warp pair being replayed came with a partial draw)
int g_freshTrace = 0;      // (PATCH18: the first six such pairs are traced in the log)
bool g_freshTraceOpen = false;
vgx::CmdBuf* PrepareReplayInner(const vgx::CmdBuf& ask);
vgx::CmdBuf* PrepareReplay(const vgx::CmdBuf& ask) {
    if (ask.hrwFresh) {
        // the partial draw's resource commands run whatever becomes of the replay: the front end's caches believe they have
        sHrwFr[0].fetch_add(1, std::memory_order_relaxed);
        {   // hrw30: what the partial draw holds (the first 12): why "no fresh draws"?
            static unsigned shown = 0;
            if (shown < 12 && port_log_summary) {
                unsigned nd = 0, owned = 0, other = 0;
                for (const vgx::Cmd& c : ask.cmds) {
                    if (c.type == vgx::CMD_DRAW && c.arg < ask.draws.size()) { ++nd; if (ask.draws[c.arg].owner) ++owned; }
                    else ++other;
                }
                ++shown;
                port_log_summary("hrw: partial draw of pair %u: %u commands (%u draws, %u of them owned, %u other), %u texture uploads, %u shaders, presentable %d",
                                 ask.hrwReplayId, static_cast<unsigned>(ask.cmds.size()), nd, owned, other, static_cast<unsigned>(ask.uploads.size()), static_cast<unsigned>(ask.shaders.size()), ask.presentAllowed ? 1 : 0);
            }
        }
        vgx::backend::HrwRunResources(ask);
        bool late = false;
        {   // (warp32 PATCH23: its pair's second picture already went out as the plain re-projection)
            std::lock_guard<std::mutex> lk(sMtx);
            if (HrwProv* e = HrwProvFindLocked(ask.hrwReplayId)) {
                late = e->state == 3;
                e->state = 0;
            }
        }
        if (late) {
            sHrwDl[2].fetch_add(1, std::memory_order_relaxed);
            vgx::backend::HrwFlushDeferred();  // (nothing replays: the frees it listed are due)
            return nullptr;
        }
    }
    vgx::CmdBuf* r = PrepareReplayInner(ask);
    if (!r && ask.hrwFresh) {
        sHrwFr[22].fetch_add(1, std::memory_order_relaxed);
        vgx::backend::HrwFlushDeferred();  // (nothing will replay: the frees the partial draw listed are due)
    }
    return r;
}
vgx::CmdBuf* PrepareReplayInner(const vgx::CmdBuf& ask) {
    if (ask.hrwFresh && Mode() == vgx::hrw::kVerifyFresh) {
        // verify-fresh: no second picture. The partial draw's owners are probed for the comparison with the picture that follows, and the merge into the last kept buffer is tried and undone
        vgx::hrw::CollectProbes(ask, g_probesFresh);
        g_probesValid = true;
        g_probesPair = ask.hrwReplayId;
        if (g_ret && g_retOk) {
            const uint64_t h0 = vgx::hrw::HashBuf(*g_ret);
            vgx::hrw::ComposeStats cs;
            const int rc = vgx::hrw::Compose(*g_ret, ask, g_undo, cs, g_scr);
            if (rc == vgx::hrw::kComposeOk) {
                if (ValidateBuf(*g_ret) != 0) sHrwFr[18].fetch_add(1, std::memory_order_relaxed);
                vgx::hrw::Uncompose(*g_ret, g_undo);
                if (vgx::hrw::HashBuf(*g_ret) != h0) sHrwFr[18].fetch_add(1, std::memory_order_relaxed);
                sHrwFr[1].fetch_add(1, std::memory_order_relaxed);
                sHrwFr[10].fetch_add(cs.removed, std::memory_order_relaxed);
                sHrwFr[11].fetch_add(cs.inserted, std::memory_order_relaxed);
            } else {
                sHrwFr[2].fetch_add(1, std::memory_order_relaxed);
                sHrwFr[2 + rc].fetch_add(1, std::memory_order_relaxed);
            }
            sHrwFr[23].fetch_add(cs.freshKeys, std::memory_order_relaxed);
        }
        return nullptr;
    }
    g_truthRun = false;
    if (Mode() == vgx::hrw::kWarp) {
        // hrw31: the second picture is the first one's finished image re-projected (vgx_backend_gl.cpp "namespace warp31"); with GXV_HRW_WARP_TRUTH=1 it is only dumped and the true redraw (mode 1's replay) is shown
        if (!ask.presentAllowed) { sHrwRt[2 + 5].fetch_add(1, std::memory_order_relaxed); HrwFeelLost(port::hrwfeel::kRender); return nullptr; }
        const bool truthAvail = g_ret && g_retOk && g_retId == ask.hrwReplayId;
        bool truthRun = false;
        vgx::CmdBuf* wb = const_cast<vgx::CmdBuf*>(vgx::backend::WarpPrepare(ask.hrwD, ask.hrwReplayId, truthAvail, &truthRun));
        if (!truthRun) {
            // (PATCH18) a partial draw with no draw in it is nothing to the render side: the pair is prepared and finished exactly as a plain ask's (no call into the
            // fresh path, no flag), apart from the frees it may list. c131: the emulator died right after "partial draw of pair 1: 0 commands".
            bool freshDraws = false;
            if (ask.hrwFresh)
                for (const vgx::Cmd& c : ask.cmds) if (c.type == vgx::CMD_DRAW) { freshDraws = true; break; }
            g_warpFresh = wb && ask.hrwFresh && !ask.cmds.empty();  // (PATCH15: the partial draw's listed frees are due after the replay)
            // (replay36: the ask itself is executed below, so every command of it must name a record it holds; one that does not is never executed: the plain warp, counted as a bad composite)
            const uint32_t dangling = wb && freshDraws ? DanglingCmds(ask) : 0u;
            if (dangling) {
                sHrwFr[14].fetch_add(1, std::memory_order_relaxed);
                static unsigned logged = 0;
                if (logged++ < 4 && port_log_summary)
                    port_log_summary("hrw: pair %u: the partial draw is not executed (%u of its %u commands name a record it does not hold): the plain re-projection (replay36)",
                                     ask.hrwReplayId, dangling, static_cast<unsigned>(ask.cmds.size()));
            } else
            if (wb && freshDraws) {
                wb = const_cast<vgx::CmdBuf*>(vgx::backend::WarpFresh(const_cast<vgx::CmdBuf*>(&ask)));
                if (wb == &ask) sHrwFr[1].fetch_add(1, std::memory_order_relaxed);  // ("with a fresh partial draw merged")
            }
            if (wb && freshDraws && MarioTimeCensus()) {   // (mariolag31: his draws came; were they merged? if not and he is in the picture, B would show his old pixels)
                if (wb == &ask) sHrwMt[4].fetch_add(1, std::memory_order_relaxed);
                else if (vgx::backend::WarpHasPlayer()) {
                    const bool hold = port::mariotime::RenderHold(MarioTimeMode(), sHrwMtLimited.load(std::memory_order_relaxed));
                    sHrwMt[hold ? 3 : 2].fetch_add(1, std::memory_order_relaxed);
                    if (hold) {   // (GXV_HRW_MARIOTIME: no second picture: the real first picture stays)
                        vgx::backend::WarpDrop();
                        g_warpFresh = false;
                        wb = nullptr;
                    }
                }
            }
            if (ask.hrwFresh && g_freshTrace < 6 && port_log_summary) {  // (PATCH18: where the first pairs with a partial draw get to; a second line follows when the picture is done)
                ++g_freshTrace;
                g_freshTraceOpen = true;
                port_log_summary("hrw: fresh trace %d | pair %u: partial draw with %s; second picture: %s", g_freshTrace, ask.hrwReplayId, freshDraws ? "draws" : "NO draw",
                                 !wb ? "none (the first picture stays)" : wb == &ask ? "the warp + his fresh draws" : "the plain warp");
            }
            if (wb) wb->presentStamp = ask.presentStamp;
            return wb;
        }
        g_truthRun = true;
    }
    if (!g_ret || !g_retOk) { sHrwRt[2 + 1].fetch_add(1, std::memory_order_relaxed); HrwFeelLost(port::hrwfeel::kRender); return nullptr; }
    if (g_retId != ask.hrwReplayId) { sHrwRt[2 + 2].fetch_add(1, std::memory_order_relaxed); HrwFeelLost(port::hrwfeel::kRender); return nullptr; }
    if (!ask.presentAllowed) { sHrwRt[2 + 5].fetch_add(1, std::memory_order_relaxed); HrwFeelLost(port::hrwfeel::kRender); return nullptr; }
    float d[12];
    std::memcpy(d, ask.hrwD, sizeof d);
    if (Mode() == vgx::hrw::kVerifyStill) {
        vgx::hrw::Ident(d);  // verify-still: B is A again
        sHrwRt[10].fetch_add(1, std::memory_order_relaxed);
        if (vgx::hrw::HashBuf(*g_ret) != g_hash) sHrwRt[11].fetch_add(1, std::memory_order_relaxed);
    }
    if (ask.hrwFresh && (Mode() == vgx::hrw::kOn)) {
        // the pair's partial draw replaces the retained draws of its owners (Mario ...) by the fresh ones, drawn with the second picture's own camera: the composite is executed
        const int64_t t0 = port_host_time_ns();
        vgx::hrw::ComposeStats cs;
        const int rc = vgx::hrw::Compose(*g_ret, ask, g_undo, cs, g_scr);
        if (rc == vgx::hrw::kComposeOk && ValidateBuf(*g_ret) != 0) {
            sHrwFr[14].fetch_add(1, std::memory_order_relaxed);
            vgx::hrw::Uncompose(*g_ret, g_undo);
        } else if (rc != vgx::hrw::kComposeOk) {
            sHrwFr[2].fetch_add(1, std::memory_order_relaxed);
            sHrwFr[2 + rc].fetch_add(1, std::memory_order_relaxed);
            static unsigned logged = 0;
            if (logged++ < 8 && port_log_summary) port_log_summary("hrw: pair %u: the partial draw is not merged (%s): its owners are drawn as retained this time (fresh owners %u, with a retained draw %u)", ask.hrwReplayId, vgx::hrw::RefusalName(rc), cs.freshKeys, cs.matchedKeys);
        } else {
            sHrwFr[1].fetch_add(1, std::memory_order_relaxed);
            sHrwFr[10].fetch_add(cs.removed, std::memory_order_relaxed);
            sHrwFr[11].fetch_add(cs.inserted, std::memory_order_relaxed);
            sHrwFr[12].fetch_add(cs.orphanDraws, std::memory_order_relaxed);
            sHrwFr[23].fetch_add(cs.freshKeys, std::memory_order_relaxed);
        }
        sHrwFr[13].fetch_add(static_cast<uint64_t>((port_host_time_ns() - t0) / 1000), std::memory_order_relaxed);
    }
    g_plan.Apply(*g_ret, d);
    g_ret->presentStamp = ask.presentStamp;
    return g_ret;
}
void FinishReplay() {
    if (Mode() == vgx::hrw::kWarp && !g_truthRun) {
        sHrwRt[1].fetch_add(1, std::memory_order_relaxed);
        if (g_warpFresh) { g_warpFresh = false; vgx::backend::HrwFlushDeferred(); }
        if (g_freshTraceOpen) { g_freshTraceOpen = false; if (port_log_summary) port_log_summary("hrw: fresh trace %d | second picture executed and finished", g_freshTrace); }
        return;
    }  // (hrw31: the warp picture has nothing of the kept buffer to put back)
    const int64_t t0 = g_undo.active ? port_host_time_ns() : 0;
    vgx::hrw::Uncompose(*g_ret, g_undo);  // (the kept buffer is its own again)
    if (t0) sHrwFr[13].fetch_add(static_cast<uint64_t>((port_host_time_ns() - t0) / 1000), std::memory_order_relaxed);
    g_plan.Restore(*g_ret);
    if (Mode() == vgx::hrw::kVerifyStill) {
        const uint64_t h = vgx::hrw::HashBuf(*g_ret);
        if (h != g_hash) { sHrwRt[11].fetch_add(1, std::memory_order_relaxed); g_hash = h; }  // (counted once: the next replay compares with what the buffer holds now)
    }
    sHrwRt[1].fetch_add(1, std::memory_order_relaxed);
    vgx::backend::HrwFlushDeferred();  // (the pair is over: the frees A owed)
}
} // namespace hrwrt

void ExecuteJob(const vgx::CmdBuf& job, bool fixed, bool replay = false) {
#if defined(__vita__)
    if (fixed) port_vgx_vita_set_present_allowed(job.presentAllowed);
    if (port_vgx_present_sync_mode()) port_vgx_vita_set_present_stamp(job.presentStamp);  // (GXV_PRESENT_SYNC: this picture's turn)
#endif
    if (replay) vgx::backend::HrwReplay(job);  // (GXV_HRW: the kept buffer again)
    else vgx::backend::Execute(job);
#if defined(__vita__)
    // CopyDisp may be deferred/invalid or splash-owned. Only the host knows if this job swapped.
    if (fixed) port_vgx_vita_finish_unpresented_frame();
    port_vgx_vita_job_end((job.presentStamp & 0xC0000000u) == 0xC0000000u ? 1 : 0);
#endif
}
unsigned long long CompleteJob() {
    const unsigned long long shown = sFramesShown.fetch_add(1) + 1;
    port_input_latency_frame_done(shown);
    return shown;
}

void RenderMain() {
    PORT_QEMU_THREAD("vgx-render");
    port_thread_class(3);
    bool ok;
#if defined(__vita__)
    ok = port_vgx_vita_start() != 0;
#else
    ok = vgx::host::CreateContext();
#endif
    if (ok) {
        int w = 0, h = 0;
        if (const char* rs = getenv("GXV_RENDER_SIZE")) sscanf(rs, "%dx%d", &w, &h);
        ok = vgx::backend::Init(w, h);
#if defined(__vita__)
        if (!ok) port_vgx_vita_backend_failed();  // (the graphics themselves started: without this the screen would still say "Starting")
#endif
    }
    {
        std::lock_guard<std::mutex> lk(sMtx);
        sStarted = true;
        sStartOk = ok;
    }
    sCv.notify_all();
    if (!ok) return;
#if defined(__vita__)
    port_vgx_vita_ready();
#endif
    for (;;) {
        std::unique_lock<std::mutex> lk(sMtx);
#if defined(__vita__)
        // While the loading screen is up the wait has a timeout: the picture moves although no frame comes.
        // GXV_WARM_IDLE=1: the deferred shader warm-up is worked off here too, whenever no frame is waiting. While the game loads a frame
        // comes about every 70 ms and this thread is idle in between, but the warm-up reads the shader cache from the card and the game
        // loads from the same card: on the console the game's own loading took 22 s instead of 12 s and the start was no shorter
        // (26 s against 24-27 s), so by default only the slice at the start of each frame is done.
        static const bool warmIdleOn = getenv("GXV_WARM_IDLE") && getenv("GXV_WARM_IDLE")[0] == '1';
        while (sJobs.empty()) {
            ServiceSettingsHold(lk);
            const bool warm = warmIdleOn && vgx::backend::WarmPending() > 0 && !vgx::backend::WarmCapacityYielded();
            // GXV_STAGEWARM_IDLE=1 (default 0): while the game's loading thread builds a stage, its recorded programs are linked here,
            // between frames - links only, no draw and no delete (vgx_backend_gl.cpp, StageWarmIdle); the rest stays in the frames' slices.
            const bool stage = port_scene_loading_active && port_scene_loading_active() && vgx::backend::StageWarmIdleWanted();
            if (!port_vgx_vita_splash_active()) {
                // (a time limit, not an endless wait: Vita3K's emulation of the kernel's condition variables sometimes loses a
                // wake-up, and with the queue cut to one frame the back end waits for every frame; the render stood still for good
                // in three emulator runs, stall report: "waiting for a frame" while a frame was queued. The console has not shown it; this costs it nothing.)
                if (sCv.wait_for(lk, std::chrono::milliseconds(warm || stage ? 1 : 20), [] { return !sJobs.empty(); })) break;
                if (warm || stage) {
                    lk.unlock();
                    if (stage) vgx::backend::StageWarmIdle();
                    if (warm) vgx::backend::WarmIdle();
                    lk.lock();
                }
                continue;
            }
            if (sCv.wait_for(lk, std::chrono::milliseconds(warm || stage ? 1 : 20), [] { return !sJobs.empty(); })) break;
            lk.unlock();
            if (warm || stage) {
                port_vgx_vita_idle_gap(80000u);
                if (stage) vgx::backend::StageWarmIdle();
                if (warm) vgx::backend::WarmIdle();
            } else {
                port_vgx_vita_idle();
            }
            lk.lock();
        }
#else
        sCv.wait(lk, [] { return !sJobs.empty(); });
#endif
        vgx::CmdBuf* job = sJobs.front();
        bool mtHold = false;  // (mariolag31, GXV_HRW_MARIOTIME: the partial draw missed its deadline: no second picture rather than his old pixels)
        if (job->hrwReplayId != 0 && !job->hrwFresh) {  // (warp32 PATCH23: a plain ask of a pair whose partial draw may still come; sMtx held)
            const uint32_t pid = job->hrwReplayId;
            HrwProv* e = HrwProvFindLocked(pid);
            if (e && e->state != 3) {
                const auto there = [pid]() -> vgx::CmdBuf* {
                    for (size_t i = 1; i < sJobs.size(); ++i) if (sJobs[i]->hrwFresh && sJobs[i]->hrwReplayId == pid) return sJobs[i];
                    return nullptr;
                };
                const int64_t w0 = port_host_time_ns();
                bool waited = false;
                while (e->state == 1 && !there()) {
                    const int64_t left = e->deadlineNs - port_host_time_ns();
                    if (left <= 0) break;
                    waited = true;
                    sCv.wait_for(lk, std::chrono::microseconds(std::min<int64_t>(left / 1000 + 1, 2000)));
                }
                if (waited) {
                    sHrwDl[4].fetch_add(1, std::memory_order_relaxed);
                    sHrwDl[5].fetch_add(static_cast<uint32_t>((port_host_time_ns() - w0) / 1000), std::memory_order_relaxed);
                }
                const vgx::CmdBuf* f = there();
                if (f && f->presentAllowed) {
                    // the partial draw is here: it is the second picture, this ask is dropped (its job back to the asks' pool); the entry goes when that job is prepared
                    sHrwDl[0].fetch_add(1, std::memory_order_relaxed);
                    sJobs.erase(sJobs.begin());
                    job->clear();
                    sHrwSpare.push_back(job);
                    if (sHrwTiny) --sHrwTiny;
                    lk.unlock();
                    sCv.notify_all();
                    continue;
                }
                if (e->state == 2) { sHrwDl[3].fetch_add(1, std::memory_order_relaxed); e->state = 0; }  // (no partial draw this pair: the plain picture, as always)
                else {
                    sHrwDl[1].fetch_add(1, std::memory_order_relaxed); e->state = 3;                     // (fresh missed its deadline: plain now, its job is resources only)
                    mtHold = port::mariotime::RenderHold(MarioTimeMode(), sHrwMtLimited.load(std::memory_order_relaxed));                               // (mariolag31: ... or, GXV_HRW_MARIOTIME, nothing: the real picture stays)
                    sHrwMt[mtHold ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        const bool cap = sCapWanted;
        // Desktop harness (software GL): when frames pile up, the older ones only have their
        // uploads, shaders, copies and clears executed, so that the game keeps its pace and
        // scripted input (which is timed by the clock) arrives at the same moments as in a
        // reference run. A frame that is captured is always drawn in full.
        const bool skip = (sSkipAlways || (sSkipWhenBehind && sJobs.size() > 1)) && !cap;   // (GXV_SKIP_ALL: every frame but a captured one)
        // GXV_INDCAP_PEEK (peek25): the buffer queued behind this one, if any (a pointer only; sMtx held, the buffer is complete and only this thread touches it from here on)
        const vgx::CmdBuf* const peekNext = vgx::indcappeek::Mode() != 0 && sJobs.size() > 1 ? sJobs[1] : nullptr;
        const bool peekAllowed = vgx::indcappeek::Mode() != 0 && !sSkipAlways && !sSkipWhenBehind && !cap;
        lk.unlock();
        // GXV_HRW: a tiny job asks for the kept buffer again; an ordinary one first makes the frees the kept buffer owed
        const bool hrwAsk = job->hrwReplayId != 0;
        const vgx::CmdBuf* exec = job;
        bool hrwKeep = false;
        if (hrwAsk) exec = mtHold ? nullptr : hrwrt::PrepareReplay(*job);
        else {
            hrwrt::BeforeOrdinary(*job);
            hrwKeep = job->hrwId != 0 && hrwrt::KeepOn();
            vgx::backend::HrwRetainRun(hrwKeep);
            vgx::backend::WarpArm(hrwrt::Mode() == vgx::hrw::kWarp && job->hrwId != 0, job->hrwId, job->hrwNoFresh);
        }
        const bool hrwSkip = exec == nullptr;
        if (peekAllowed && !hrwAsk && !hrwKeep) { // (GXV_INDCAP_PEEK: an ordinary buffer, executed in full, not kept for a replay)
            namespace ip = vgx::indcappeek;
            ip::Verify(*job, ip::g_shadow, ip::g_win);  // (the buffer that was the "next" one: its first use of the texture must be a whole write)
            if (job->hrwId == 0 && !job->hrwFresh) {
                ip::Decide(*job, peekNext, ip::Mode() == 1, ip::g_win, ip::g_shadow);
                char peekLine[900];
                if (ip::Line(peekLine, sizeof peekLine, ip::g_win, ip::g_tot) && port_log_summary) port_log_summary("%s", peekLine);
            }
        }
        vgx::backend::SetSkipDraws(skip);
        vgx::backend::RequestCapture(cap);
        const int64_t tExec = PORT_RUNTIME_DIAGNOSTICS && sPerfSeconds > 0 ? port_host_time_ns() : 0;
        const int64_t tAdapt = sAdaptDepth ? port_host_time_ns() : 0;
        const int64_t cpuAdaptBefore = sAdaptDepth && sFixedDepth ? port_thread_cpu_us() : 0;
        const int64_t tPace = PaceMode() ? port_host_time_ns() : 0;
        const int64_t paceCpuBefore = tPace ? port_thread_cpu_us() : 0;
        if (PORT_RUNTIME_DIAGNOSTICS) {
            sBackSince.store(port_host_time_ns(), std::memory_order_relaxed);
            sBackStage.store(1, std::memory_order_relaxed);
        }
        {
            vgx::workload::Scope execution(vgx::workload::BackendExecute);
            if (!hrwSkip) ExecuteJob(*exec, PaceMode() == 3, hrwAsk);
        }
        if (hrwAsk) { if (!hrwSkip) hrwrt::FinishReplay(); }
        else if (hrwKeep) { vgx::backend::HrwRetainRun(false); hrwrt::Retain(*job); }
        if (tPace) {
            const int64_t cpuNow = port_thread_cpu_us();
            const uint64_t elapsed = static_cast<uint64_t>(port_host_time_ns() - tPace) / 1000;
            // Display-callback waiting runs on another thread and can overlap this work; never subtract it.
            sPaceBackendUs.store(paceCpuBefore > 0 && cpuNow >= paceCpuBefore ? static_cast<uint64_t>(cpuNow - paceCpuBefore) : elapsed,
                                 std::memory_order_relaxed);
        }
        vgx::workload::Report();
        if (PORT_RUNTIME_DIAGNOSTICS) {
            sBackSince.store(port_host_time_ns(), std::memory_order_relaxed);
            sBackStage.store(2, std::memory_order_relaxed);
        }
        unsigned newLimit = 0;  // (0: unchanged)
        if (sAdaptDepth && !hrwSkip) {
            // The mean time of 24 frames' execution (port/queue_depth.h; a frame's time is capped, so a shader-compile stall does not
            // decide). GXV_FIXED_DEPTH=1 (default 0): with a display cap in force the thread's own CPU time is measured instead of its
            // wall time, because the wall time then includes the wait for the display queue and never gets under the cut-off.
            static port::depth::Adapter adapter;
            static unsigned changes = 0;
            static bool configured = false;
            if (!configured) {
                configured = true;
                adapter.Configure(sMaxInFlight + 1);
            }
            const int64_t wallNs = port_host_time_ns() - tAdapt;
            int64_t sample = wallNs;
            if (sFixedDepth && PaceMode() != 0 && cpuAdaptBefore > 0) {
                const int64_t cpuNow = port_thread_cpu_us();
                if (cpuNow >= cpuAdaptBefore) sample = (cpuNow - cpuAdaptBefore) * 1000;
            }
            newLimit = adapter.Feed(sample);
            if (newLimit && changes < 40) {
                ++changes;
                port_log("gx: the back end takes %.1f ms a frame (%s): %s", adapter.LastMean() / 1e6,
                         sample == wallNs ? "wall time" : "own CPU time",
                         adapter.Tight() ? "it is the bottleneck, so the queue is cut to one frame" : "it keeps up, so the queue is open again");
            }
        }
        const unsigned long long shown = hrwSkip ? sFramesShown.load() : CompleteJob();
        if (sPerfSeconds > 0 && !hrwSkip) {
            const int64_t execNs = PORT_RUNTIME_DIAGNOSTICS ? port_host_time_ns() - tExec : 0;
            const vgx::backend::Stats st = vgx::backend::GetStats();
            PerfFrame(execNs, st.drawCallsThisFrame);
            // A back-end frame of 45 ms or more (the game thread then waits for it, "gx-finish-wait"):
            // what it did that costs time. Counters are the changes since the previous frame.
            static vgx::backend::Stats prev;
            static unsigned slowLogged = 0;
            if (execNs >= 45000000 && slowLogged < 300) {
                ++slowLogged;
                port_log("back end: slow frame %.0f ms: %u shader compiles, %u program links (%.0f ms of shader work), %u KiB of textures, %u draws, %u textures live",
                         execNs / 1e6, st.shaderCompiles - prev.shaderCompiles, st.programLinks - prev.programLinks, st.shaderWorkMs - prev.shaderWorkMs,
                         static_cast<unsigned>((st.textureBytes - prev.textureBytes) / 1024), st.drawCallsThisFrame, st.textures);
            }
            prev = st;
        }
#if defined(__vita__)
        if (!hrwSkip) port_vgx_vita_frame_done();
#endif
        if (cap && !hrwAsk) {
            int w = 0, h = 0;
            const std::vector<uint8_t>& px = vgx::backend::LastDisplayCopy(w, h);
            if (w > 0 && h > 0) {
                // The copy is top row first; the PNG writer takes GL order (bottom row first).
                std::vector<uint8_t> flipped(px.size());
                const size_t row = static_cast<size_t>(w) * 4;
                for (int y = 0; y < h; ++y) {
                    memcpy(flipped.data() + static_cast<size_t>(y) * row, px.data() + static_cast<size_t>(h - 1 - y) * row, row);
                    for (int x = 0; x < w; ++x) flipped[static_cast<size_t>(y) * row + x * 4 + 3] = 255; // EFB alpha is not part of the picture
                }
                port_headless_write_png(sCapPath.c_str(), flipped.data(), w, h);
            }
        }
        // (PATCH16: clear() forgets what kind of job it was: the partial-draw jobs then went back to the plain asks' pool, their own pool ran dry after two pairs and
        // the game waited 3 s for a job that never came back: "hrw: DISABLED" two pairs into play, console and emulator, c130)
        const bool jobWasFresh = job->hrwFresh;
        job->clear();
        sHrwRtLoops.fetch_add(1, std::memory_order_relaxed);
        if (PORT_RUNTIME_DIAGNOSTICS) {
            sBackStage.store(0, std::memory_order_relaxed);
            sBackSince.store(port_host_time_ns(), std::memory_order_relaxed);
        }
        lk.lock();
        if (newLimit) sQueueLimit = newLimit;
        sJobs.erase(sJobs.begin());
        if (hrwAsk) {
            if (jobWasFresh && sHrwFreshSpare.size() >= 3) { delete job; --sHrwFreshExtra; }  // (hrw29: an extra job is let go once the pool is back to its size)
            else (jobWasFresh ? sHrwFreshSpare : sHrwSpare).push_back(job);
            if (sHrwTiny) --sHrwTiny;
        }
        else sFree.push_back(job);
        if (cap && !hrwAsk) { sCapWanted = false; sCapDone = true; sCapFrame = shown; }
        lk.unlock();
        sCv.notify_all();
    }
}

void* RenderEntry(void*) {
    RenderMain();
    return nullptr;
}

// A start-up failure that has its reason on the loading screen (the host's splash::Fail) stays on that screen: the game thread
// parks, and the player closes the game. Without it the process aborted within a moment and the reason was gone.
// GXV_BOOT_PARK=0 aborts as before. Read once. Only start-up failures that are already on screen reach the parking branch.
[[noreturn]] void GraphicsFailed(const char* why) {
#if defined(__vita__)
    static const bool park = vgx::EnvInt("GXV_BOOT_PARK", 1) != 0;
    if (vgx::bootwatch::ParkOnError(park, port_vgx_vita_error_shown() != 0)) {
        if (port_log_summary) port_log_summary("boot: %s; the reason is on the loading screen, which stays up (close the game)", why);
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
#endif
    port_fatal("vgx: %s", why);
}
constexpr const char* kNoContext = "no graphics context for the renderer's back end";

// Seconds the game's thread waits for the render thread's graphics before it says so on the loading screen. Read once.
// GXV_BOOT_WAIT_SEC=0: wait without a limit, as before.
double GraphicsWaitLimitSeconds() {
    static const double limit = static_cast<double>(vgx::EnvInt("GXV_BOOT_WAIT_SEC", 120));
    return limit;
}
void GraphicsWaitTimedOut(const char* where, double seconds) {
    if (port_log_summary) port_log_summary("boot: the render thread's graphics did not start within %.0f s (%s)", seconds, where);
#if defined(__vita__)
    port_vgx_vita_fail("The graphics did not start within two minutes.\nClose the game and start it again. If this repeats, send ux0:data/smg/log.txt.");
#endif
}

// wait: return when the render thread has its graphics up (otherwise at once: the front end can fill its frames meanwhile, they wait in the
// queue, and FrameSink watches for a thread that failed to start).
bool EnsureThread(bool wait = true) {
    std::unique_lock<std::mutex> lk(sMtx);
    if (!sThreadMade) {
        sThreadMade = true;
        // The frame buffers' arenas at their floors now, while the C heap is still in one piece (vgx_arena.h): they then need not
        // move again in play. The front end's own buffer circulates with them (FrameSink swaps it for a free one), and the
        // front-end split's arenas are reserved when it starts.
        const vgx::arena::Floors& floors = vgx::arena::Get();
        for (unsigned i = 0; i < sMaxInFlight + 1; ++i) {
            sFree.push_back(new vgx::CmdBuf());
            vgx::arena::Reserve(*sFree.back(), floors);
        }
        vgx::arena::Reserve(vgx::Pending(), floors);
        if (vgx::HrwMode() == vgx::hrw::kWarp) {  // hrw31: the tiny jobs; the kept buffer only for the stills' true redraw
            for (int i = 0; i < 4; ++i) sHrwSpare.push_back(new vgx::CmdBuf());
            if (vgx::EnvInt("GXV_HRW_WARP_FRESH", 0) != 0) {  // (warp32 PATCH15: the player's partial draw on the second picture: the partial-draw jobs, as the other modes make them)
                sHrwFreshOn = true;
                for (int i = 0; i < 2; ++i) {
                    vgx::CmdBuf* f = new vgx::CmdBuf();
                    f->cmds.reserve(4096);
                    f->draws.reserve(1024);
                    f->vtx.reserve(1u << 20);
                    f->idx.reserve(1u << 18);
                    f->uni.reserve(1u << 17);
                    sHrwFreshSpare.push_back(f);
                }
            }
            if (vgx::EnvInt("GXV_HRW_WARP_TRUTH", 0) != 0) {
                hrwrt::g_ret = new vgx::CmdBuf();
                vgx::arena::Reserve(*hrwrt::g_ret, floors);
            }
        }
        if (vgx::HrwMode() == vgx::hrw::kOn || vgx::HrwMode() == vgx::hrw::kVerifyStill || vgx::HrwMode() == vgx::hrw::kVerifyFresh) {  // GXV_HRW: the kept buffer (one more buffer's arenas) and the tiny jobs
            hrwrt::g_ret = new vgx::CmdBuf();
            vgx::arena::Reserve(*hrwrt::g_ret, floors);
            for (int i = 0; i < 4; ++i) sHrwSpare.push_back(new vgx::CmdBuf());
            sHrwFreshOn = vgx::HrwMode() != vgx::hrw::kCensus && vgx::EnvInt("GXV_HRW_FRESH", 1) != 0;
            // hrw27: the partial draw's jobs: two buffers with room for Mario's draws and a set of actors (the front end's buffer is copied into one, so that its own arenas never change hands)
            for (int i = 0; i < 2; ++i) {
                vgx::CmdBuf* f = new vgx::CmdBuf();
                f->cmds.reserve(4096);
                f->draws.reserve(1024);
                f->vtx.reserve(1u << 20);
                f->idx.reserve(1u << 18);
                f->uni.reserve(1u << 17);
                sHrwFreshSpare.push_back(f);
            }
        }
        // 4 MiB of stack: on the console the system's shader compiler runs on this thread.
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 4u << 20);
        if (pthread_create(&sThread, &attr, &RenderEntry, nullptr) != 0) {
            pthread_attr_destroy(&attr);
            return false;
        }
        pthread_attr_destroy(&attr);
    }
    if (!wait) return true;
    const double limit = GraphicsWaitLimitSeconds();
    if (limit <= 0.0) {
        sCv.wait(lk, [] { return sStarted; });
        return sStartOk;
    }
    // A bounded wait: after the limit the loading screen says so (the host's Fail) and the game's thread stays parked on it.
    if (!sCv.wait_for(lk, std::chrono::milliseconds(static_cast<long long>(limit * 1000.0)), [] { return sStarted; })) {
        lk.unlock();
        GraphicsWaitTimedOut("EnsureThread", limit);
        GraphicsFailed("the graphics did not start in time");
    }
    return sStartOk;
}

// The front end finished a frame: swap its buffer for an empty one and queue it.
void FrameSink(vgx::CmdBuf& buf) {
    std::unique_lock<std::mutex> lk(sMtx);
    if (buf.hrwFresh) {
        // GXV_HRW (hrw27): a pair's partial draw. It is not a picture: it is copied into one of the partial-draw jobs (so the front end keeps its own arenas), carries the pair's second-picture
        // parameters, and is queued behind the first picture like the plain ask, outside the queue's limit. Its resource commands must reach the render thread: if every job is in use the front end waits for one.
        // hrw30: the wait is bounded by the render thread's PROGRESS, not by a count (hrw29's "more than 6 extra jobs" disabled HRW in the emulator, where the render thread is simply slow: a
        // pair's replays queue up behind pictures that take 100+ ms). While the render thread finishes jobs we wait for a spare (as before); when it has finished none for 3 s, or HRW is
        // already off, one extra job is made (the partial draw's resource commands must not be lost) and, in the first case, HRW switches itself off.
        vgx::CmdBuf* r = nullptr;
        if (sHrwFreshSpare.empty()) {
            uint64_t seen = sHrwRtLoops.load(std::memory_order_relaxed);
            int64_t since = port_host_time_ns();
            while (sHrwFreshSpare.empty()) {
                sCv.wait_for(lk, std::chrono::milliseconds(10), [] { return !sHrwFreshSpare.empty(); });
                if (!sHrwFreshSpare.empty()) break;
                const uint64_t now = sHrwRtLoops.load(std::memory_order_relaxed);
                const int64_t t = port_host_time_ns();
                if (now != seen) { seen = now; since = t; }
                // (hrw31: a render thread that is INSIDE a job is alive, only slow (a starved emulator, a shader compile, a system stall on the console): it gets 20 s; one that is idle and finishes nothing gets 3 s)
                const int64_t stallNs = (PORT_RUNTIME_DIAGNOSTICS && sBackStage.load(std::memory_order_relaxed) == 1) ? 20000000000ll : 3000000000ll;
                if (sHrwDisabled.load(std::memory_order_relaxed) || t - since > stallNs) {
                    // (PATCH16: in mode 5 the partial draw is an extra: it is given up, the pairs go on)
                    if (vgx::HrwMode() == vgx::hrw::kWarp) HrwFreshGiveUp("the render thread finished no job for 3 s while the game waited for a partial draw's job");
                    else HrwDisable("the render thread finished no job for 3 s while the game waited for a partial draw's job");
                    r = new vgx::CmdBuf();
                    ++sHrwFreshExtra;
                    break;
                }
            }
        }
        if (!r) {
            r = sHrwFreshSpare.back();
            sHrwFreshSpare.pop_back();
        }
        r->clear();
        r->hrwReplayId = buf.hrwReplayId;
        r->hrwFresh = true;
        r->presentAllowed = false;  // (until the pair's parameters are on it)
        r->frame = buf.frame;
        // replay36: the job carries the draws and the resource commands of the partial draw and nothing else: copyTex, copyDisp, targets and passes are not copied (FrontFresh: "it is not a
        // picture"), so the commands that name them must not be either. The whole list was copied, and in mode 5 the job itself is executed (WarpFresh, c130): an EFB copy the game made
        // between the first picture's display copy and the partial draw's end (the undrawn turn's update runs before the draw since c142) arrived as a CMD_COPY_TEX without its record
        // (c168, console, Honeyhive: command 0 of 28; ExecuteFrame read copyTex[0] of an empty vector). Such a partial draw is resources only: with a picture's command among them its draws
        // cannot be told from that command's own (a capture's source), so they are left out too and the pair's second picture is the plain re-projection.
        {
            unsigned stray = 0, strayAt = 0;
            vgx::Cmd first{};
            for (const vgx::Cmd& c : buf.cmds) {
                if (c.type != vgx::CMD_COPY_TEX && c.type != vgx::CMD_COPY_DISP && c.type != vgx::CMD_PRESENT && c.type != vgx::CMD_SET_TARGET && c.type != vgx::CMD_PASS_BEGIN) continue;
                if (!stray++) { first = c; strayAt = static_cast<unsigned>(&c - buf.cmds.data()); }
            }
            if (!stray) r->cmds = buf.cmds;
            else {
                for (const vgx::Cmd& c : buf.cmds) if (vgx::hrw::SkipInReplay(c.type)) r->cmds.push_back(c);
                sHrwFr[24].fetch_add(1, std::memory_order_relaxed);
                static unsigned logged = 0;
                if (logged++ < 8 && port_log_summary) {
                    if (first.type == vgx::CMD_COPY_TEX && first.arg < buf.copyTex.size()) {
                        const vgx::CopyTexCmd& x = buf.copyTex[first.arg];
                        port_log_summary("hrw: pair %u: the partial draw came with %u picture commands among its %u (the first, at %u, an EFB copy of %d,%d %dx%d into a %ux%u texture, id %u, GX format 0x%02X, "
                                         "guest address 0x%08X, depth %d): its resource commands only, the second picture is the plain re-projection (replay36)",
                                         buf.hrwReplayId, stray, static_cast<unsigned>(buf.cmds.size()), strayAt, x.src.x, x.src.y, x.src.w, x.src.h, static_cast<unsigned>(x.dstW), static_cast<unsigned>(x.dstH),
                                         static_cast<unsigned>(x.texId), static_cast<unsigned>(x.gxFmt), static_cast<unsigned>(x.gxDest), x.depth ? 1 : 0);
                    } else {
                        port_log_summary("hrw: pair %u: the partial draw came with %u picture commands among its %u (the first, at %u, of type %u): its resource commands only, the second picture is the "
                                         "plain re-projection (replay36)", buf.hrwReplayId, stray, static_cast<unsigned>(buf.cmds.size()), strayAt, static_cast<unsigned>(first.type));
                    }
                }
            }
        }
        r->draws = buf.draws;
        r->vtx = buf.vtx;
        r->idx = buf.idx;
        r->uni = buf.uni;
        r->vtxChunks = buf.vtxChunks;
        r->variants = buf.variants;
        r->shaders = buf.shaders;
        r->uploads = std::move(buf.uploads);
        r->bufUploads = std::move(buf.bufUploads);
        if (sHrwFreshAsk.valid && sHrwFreshAsk.id == r->hrwReplayId) {
            r->presentStamp = sHrwFreshAsk.stamp;
            r->presentAllowed = true;
            std::memcpy(r->hrwD, sHrwFreshAsk.d, sizeof r->hrwD);
            sHrwFreshAsk.valid = false;
            if (!HrwProvFindLocked(r->hrwReplayId))  // (warp32 PATCH23: the pair's plain ask was queued and counted already)
            sHrwRt[0].fetch_add(1, std::memory_order_relaxed);  // (hrw30: an ask that travels on the partial draw is an ask queued too: the census said "asks queued 0" with replays running)
        }
        Jc36PushLocked();
        sJobs.push_back(r);
        ++sHrwTiny;
        lk.unlock();
        sCv.notify_all();
        return;
    }
    if (sFif.Enabled()) {   // (strk27 GXV_FIF: a picture counts from here to the display's flip; the wait is the game thread's, before its next turn reads the pad)
        const unsigned long long sent = sFramesQueued.load(std::memory_order_relaxed) + 1;
        auto shownNow = [] { return std::min(sFifShown.load(std::memory_order_relaxed), sFramesShown.load(std::memory_order_relaxed)); };
        const int64_t t0 = port_host_time_ns() / 1000;
        if (sFif.Want(sent, shownNow(), t0)) {
            const unsigned depth = sFif.Depth();
            // (PATCH3: GXV_FIF_WAIT_MS: the longest one wait; default one refresh with GXV_HRW (a longer wait there is a missed pair), 50 ms otherwise as before)
            static const int64_t waitUs = [] {
                const long ms = vgx::EnvInt("GXV_FIF_WAIT_MS", vgx::EnvInt("GXV_HRW", 0) != 0 ? 17 : 50);
                return static_cast<int64_t>(ms < 1 ? 1 : ms > 50 ? 50 : ms) * 1000;
            }();
            const bool ok = sCv.wait_for(lk, std::chrono::microseconds(waitUs), [&] { return sent - std::min<unsigned long long>(sent, shownNow()) <= depth; });
            sFif.Waited(!ok, port_host_time_ns() / 1000 - t0);
            if (!ok) {
                // (PATCH3: a wait that ran out with the back end idle is a count that fell behind, not a full pipeline: the display's count is taken up to the back end's, once, instead of
                //  thirty full waits before the gate gives up)
                const unsigned long long done = sFramesShown.load(std::memory_order_relaxed);
                if (sFifShown.load(std::memory_order_relaxed) < done && sJobs.size() - sHrwTiny == 0) { sFifShown.store(done, std::memory_order_relaxed); ++sFifResyncs; }
            }
        }
        const port::fif::Gate::Stats& st = sFif.GetStats();
        if (st.probes >= sFifLogAt + 600u) {
            sFifLogAt = st.probes;
            port_log_summary("fif: GXV_FIF=%u: %u probes, %u waited (%u still over after the wait), pictures in flight at the probe %.2f mean / %u max, stood aside %u times, switched off %u times, tried again %u times, count taken up %u times%s",
                             sFif.Depth(), st.probes, st.waits, st.stillOver, st.probes ? static_cast<double>(st.overSum) / st.probes : 0.0, st.overMax, st.aside, st.strikes, st.retries, sFifResyncs,
                             sFif.Off() ? " (off now)" : "");
        }
    }
    {
        vgx::workload::Scope observation(vgx::workload::FrontQueue);
        const double limit = GraphicsWaitLimitSeconds();
        const int64_t waitStart = port_host_time_ns();
        // GXV_WAITLOG=1 (default 0): how long the game thread waited here for the render thread, counted for the "waits:" line.
        struct GateCensus {
            int64_t start;
            int64_t always;  // (smooth25: the longest wait since the last peek, always on: the "hitch:" line's game-thread gate wait)
            ~GateCensus() {
                const int64_t end = port_host_time_ns();
                if (EwOn()) sEwGateNs += end - always;  // (endwait26)
                if (LaneOn()) sLaneGateNs += end - always;  // (pace33 PATCH4)
                if (end > always) {
                    const uint64_t peekUs = static_cast<uint64_t>(end - always) / 1000;
                    if (peekUs > sGatePeekMaxUs.load(std::memory_order_relaxed)) sGatePeekMaxUs.store(peekUs, std::memory_order_relaxed);  // (one writer)
                }
                if (!start) return;
                const uint64_t us = static_cast<uint64_t>(end - start) / 1000;
                sGateWaitN.fetch_add(1, std::memory_order_relaxed);
                sGateWaitUs.fetch_add(us, std::memory_order_relaxed);
                uint64_t seen = sGateWaitMaxUs.load(std::memory_order_relaxed);
                while (us > seen && !sGateWaitMaxUs.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {}
            }
        } census{sWaitLog ? waitStart : 0, waitStart};
        while (!sCv.wait_for(lk, std::chrono::milliseconds(20), [] {
            return !sFree.empty() && sJobs.size() - sHrwTiny < (sDepthCap && sDepthCap < sQueueLimit ? sDepthCap : sQueueLimit);
        })) {
            // (time limit: see RenderMain)
            if (sStarted && !sStartOk) {
                lk.unlock();
                GraphicsFailed(kNoContext);
            }
            // The queue is full because the render thread has not started its graphics, and has not for the limit: say so.
            if (!sStarted && limit > 0.0 && static_cast<double>(port_host_time_ns() - waitStart) / 1e9 >= limit) {
                lk.unlock();
                GraphicsWaitTimedOut("FrameSink", limit);
                GraphicsFailed("the graphics did not start in time");
            }
        }
    }
    vgx::CmdBuf* b = sFree.back();
    sFree.pop_back();
    std::swap(*b, buf);
    Jc36PushLocked();
    sJobs.push_back(b);
    if (b->hrwId) {  // GXV_HRW: the pair's second picture may have been asked for already
        if (sHrwAsk.valid && sHrwAsk.id != b->hrwId) { sHrwRt[2 + 2].fetch_add(1, std::memory_order_relaxed); HrwFeelLost(port::hrwfeel::kRender); sHrwAsk.valid = false; }
        sHrwQueued = b->hrwId;
        if (sHrwAsk.valid) { hrwrt::PushAskLocked(sHrwAsk.id, sHrwAsk.stamp, sHrwAsk.d); sHrwAsk.valid = false; }
    }
    if (sJobs.size() - sHrwTiny > sQueueMax.load(std::memory_order_relaxed)) sQueueMax.store(static_cast<unsigned>(sJobs.size() - sHrwTiny), std::memory_order_relaxed);
    sFramesQueued.fetch_add(1);
    lk.unlock();
    sCv.notify_all();
}

// A display copy is the end of the game's frame.
void DisplayCopy() {
    const bool ew = EwOn();  // (endwait26)
    const bool lane = LaneOn() && port_vgx_player();  // (pace33 PATCH4: only with the front end on its own thread)
    const int64_t ewDisp = ew || lane ? port_host_time_ns() : 0;
    const uint64_t ewFrame = ew ? vgx::CurrentFrame() : 0;
    const uint32_t laneStamp = lane ? vgx::Pending().presentStamp : 0u;
    vgx::split::FinishFrame(vgx::Pending()); // (front-end split: the frame's records join it, and count in the statistics read here)
    const int64_t ewList = ew || lane ? port_host_time_ns() : 0;
    const vgx::stats::Frame fs = vgx::stats::Cur();  // (FrontPresent resets it)
    vgx::FrontPresent(false, nullptr);
    if (ew) EwPictureDone(ewDisp, ewList, ewFrame, fs);
    if (lane) LanePictureDone(laneStamp, ewDisp, ewList);
    vgx::workload::FrontPicture();
    // (automatic quality's target mode: the front end is a stage of the picture's work, measured where it has a thread of its
    // own; on the game thread its work is already in the game's turns)
    if (port_autoq_front_picture && port_vgx_player()) port_autoq_front_picture();
    if (PORT_RUNTIME_DIAGNOSTICS && sPerfSeconds > 0) {
        sPlCpuUs.store(port_thread_cpu_us(), std::memory_order_relaxed);
        sPlRecords.fetch_add(fs.drawCalls, std::memory_order_relaxed);
        sPlPrims.fetch_add(fs.prims, std::memory_order_relaxed);
    }
    PORT_QEMU_FRAME_STATS(static_cast<uint32_t>(sFramesQueued.load()), fs.prims, fs.verts, fs.tris, fs.drawCalls, fs.streams);
    PORT_QEMU_MARK(static_cast<uint32_t>(sFramesQueued.load()));  // armhf/qemu harness: end of a game frame
    // GXV_STATS=<file>: the renderer's feature-usage report (what the game asked for that is
    // approximated or missing, texture and copy formats, cache counters), rewritten now and then.
    static const char* const statsPath = vgx::diag::Env("GXV_STATS");
    static unsigned n = 0;
    if (statsPath && *statsPath && (++n % 300) == 0) vgx::stats::Dump(statsPath);
}

#if !defined(__vita__)
// GXV_TEXDUMP=<folder> (desktop): every texture the renderer uploads as RGBA8, as
// <hash>_<w>x<h>_f<format>.png. The hash is the one the handheld's picture replacement keys on
// (run with GXV_DXT1=0 to see the compressed ones too).
void TexDump(const vgx::TexUpload& up, uint32_t gxFormat, uint64_t) {
    static const char* const dir = getenv("GXV_TEXDUMP");
    if (!dir || !*dir || up.format != vgx::TF_RGBA8 || up.levels.empty()) return;
    const uint32_t w = up.width, h = up.height;
    if (up.levels[0].size() < static_cast<size_t>(w) * h * 4) return;
    std::vector<uint8_t> flipped(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) memcpy(flipped.data() + static_cast<size_t>(y) * w * 4, up.levels[0].data() + static_cast<size_t>(h - 1 - y) * w * 4, static_cast<size_t>(w) * 4);
    char path[512];
    snprintf(path, sizeof path, "%s/%016llx_%ux%u_f%u.png", dir, static_cast<unsigned long long>(vgx::LastUploadHash()), w, h, gxFormat);
    port_headless_write_png(path, flipped.data(), static_cast<int>(w), static_cast<int>(h));
}
#endif

// The front-end split's finishing thread (vgx_fesplit.h): on the back end's core by default, which has CPU to spare
// in every scene measured, one priority step above the renderer's threads (Vita).
//   GXV_SPLIT_PRIO=<n>  (Vita) its scheduling priority: 0 (default) one step above the renderer's threads (a lower number; they
//                       run at the library's default unless GXV_PRIO says otherwise), 1 the same as theirs, 0x40-0xBF that one.
//                       Threads of one priority do not preempt each other, so at the back end's priority the finishing thread
//                       only runs while the back end waits (for a frame, for the GPU), and the player waits for it at the end
//                       of its frame whenever the back end was busy (in Vita3K, where the back end takes 200-500 ms a frame, the
//                       player waited 10-70 ms a frame). One step above, it runs as soon as it has work, in short bursts (a few
//                       dozen draws at a time) that delay the back end's submission by as much.
#if defined(__vita__)
extern "C" int sceKernelChangeThreadPriority(int thid, int priority);
extern "C" int sceKernelGetThreadCurrentPriority(void);
extern "C" int sceKernelGetThreadId(void);
extern "C" int sceKernelChangeThreadCpuAffinityMask(int thid, int mask);

// The finishing thread on the fourth core (GXV_CORE3_SPLIT, port/core_layout.h): the system core may give an application only part of its
// time, and the player waits for the finishing thread at every picture end (vgx_fesplit.cpp Drain: it takes over what the thread has not
// started, but waits while the thread is in the middle of a batch). So while the thread is there it writes one basic-log line every ~5 s
// (its CPU, the player's catch-up per picture: what to compare with a run without the switch), and port::cores::SplitGuard moves it back
// to its planned core (and says so) when the catch-up stays over GXV_CORE3_SPLIT_GUARD_MS (default 3 ms per picture; 0 never).
std::atomic<bool> sSplitOnCore3{false};
int sSplitCore3Slot = -1;  // (core3-38: the meter slot of "vgx-finish"; the thread registers it before it can be on core 3)
int sSplitPlannedMask = 0;  // (written before sSplitOnCore3, on the finishing thread, which is also the only reader)
void SplitCore3Tick() {
    {
        // (hitch30) the player's two waits that used to spin without a bound (vgx_fesplit.h SpinCounters): 0 = the reader at a picture's end, 1 = fepar's settle.
        // One line per 5 s while the waits happen; this tick runs about every 500 ms while the finishing thread is idle, on a stock console too (no fourth core).
        static int64_t spinAt = 0;
        static uint32_t spinLast = 0;
        const int64_t t = port_host_time_ns();
        if (t - spinAt >= 5000000000ll) {
            spinAt = t;
            uint32_t a[5] = {}, b[5] = {};
            vgx::split::SpinCounters(0, a);
            vgx::split::SpinCounters(1, b);
            if (a[0] + b[0] != spinLast && port_log_summary) {
                spinLast = a[0] + b[0];
                port_log_summary("spin: front-end reader %u waits, %u spins ran out, %u take-overs, longest %u us (window), %u us (ever); "
                                 "fepar settle %u waits, %u spins ran out, %u take-overs (the player ran the rest itself), longest %u us (window), %u us (ever); "
                                 "GXV_BOUND_SPIN_US=%u", a[0], a[1], a[2], a[3], a[4], b[0], b[1], b[2], b[3], b[4], vgx::split::BoundSpinUs());
            }
        }
    }
    if (!sSplitOnCore3.load(std::memory_order_acquire)) return;
    static port::cores::SplitGuard guard{static_cast<double>(vgx::EnvInt("GXV_CORE3_SPLIT_GUARD_MS", 3)), 0};
    static int64_t windowStart = 0, lastCpu = 0, lastWait = 0;
    static uint32_t lastFrames = 0, lastStolen = 0;
    const int64_t now = port_host_time_ns();
    int64_t cpu = 0, wait = 0;
    uint32_t frames = 0;
    vgx::split::Counters(cpu, wait, frames);
    const uint32_t stolen = vgx::split::StolenOps();
    if (!windowStart) {
        windowStart = now; lastCpu = cpu; lastWait = wait; lastFrames = frames; lastStolen = stolen;
        return;
    }
    if (now - windowStart < 5000000000ll) return;
    const unsigned pictures = frames - lastFrames;
    const double n = pictures ? pictures : 1.0, caughtUp = (wait - lastWait) / 1e6 / n;
    if (port_log_summary)
        port_log_summary("threads: core 3: finishing thread %u pictures in %.1f s, its cpu %.1f ms/picture; the player caught up with it for %.2f ms/picture, "
                         "doing %.0f of its operations/picture itself", pictures, (now - windowStart) / 1e9, (cpu - lastCpu) / 1e3 / n, caughtUp,
                         (stolen - lastStolen) / n);
    windowStart = now; lastCpu = cpu; lastWait = wait; lastFrames = frames; lastStolen = stolen;
    if (!guard.Feed(caughtUp, pictures)) return;
    sSplitOnCore3.store(false, std::memory_order_relaxed);
    port::core3::MarkOnCore3(sSplitCore3Slot, false);
    const int back = sSplitPlannedMask ? sSplitPlannedMask : port::cores::kHelpers;
    const int rc = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), back);
    if (port_log_summary)
        port_log_summary("threads: core 3: the player waited over %.1f ms/picture for the finishing thread twice in a row: it goes back to mask 0x%X (0x%X); "
                         "GXV_CORE3_SPLIT_GUARD_MS=0 keeps it on the fourth core", guard.limitMs, static_cast<unsigned>(back), static_cast<unsigned>(rc));
}
#endif
// core3-38: the finishing thread's hook (vgx_fesplit.cpp ThreadMain): 0 wake, 1 job boundary (may nap), 2 about to block. No clock read unless the thread is on core 3.
} // namespace
extern "C" void port_core3_finisher(int what) {
    static port::core3::Worker w(port::core3::RegisterThread("vgx-finish"));  // (only the finishing thread calls this)
    if (what == 0) w.Wake();
    else if (what == 1) w.Checkpoint();
    else w.Sleep();
}
namespace {
void SplitThreadInit(int core) {
    PORT_QEMU_THREAD("vgx-finish");
#if defined(__vita__)
    // GXV_CORE0_ALONE=1: a finishing thread asked onto core 0 (GXV_SPLIT_CORE=0) goes to core 1 instead; core 0 is the game's.
    // GXV_SPLIT_CORE0=1 (read once, default 0; an experiment): it goes to core 0 all the same, where the game thread leaves more than
    // half of every turn idle (r27: ~6.8 ms of 16.7), and the back end's core keeps the finishing thread's CPU. At its default priority
    // (one step above class 3, which the game's and the renderer's threads share: 0x8F against 0x90) it runs in short bursts as soon
    // as it has work, which the game thread waits out: the game's turn grows by about the finishing thread's CPU a picture. (Below the
    // game's priority it would be preempted while holding the reader and the player would wait for it at the end of its picture: do not
    // combine with a GXV_SPLIT_PRIO of 0x90 or more.)
    if (port_vita_core0_alone && port_vita_core0_alone() && !(core == 0 && vgx::EnvInt("GXV_SPLIT_CORE0", 0) != 0)) {
        port::cores::Switches alone;
        alone.core0Alone = true;
        core = port::cores::SplitCore(alone, core);
    }
    const int pin = core >= 0 && core <= 2 ? port_vita_pin_core(core) : 0;
    port_thread_class(3);
    long prio = vgx::EnvInt("GXV_SPLIT_PRIO", 0);
    if (prio == 0) prio = sceKernelGetThreadCurrentPriority() - 1; // (the renderer's threads have the class this thread has now)
    const int rc = prio >= 0x40 && prio <= 0xBF ? sceKernelChangeThreadPriority(0, static_cast<int>(prio)) : 0;
    // GXV_CORE3_SPLIT=1 (or GXV_CORE3=1) with a proven fourth core: the thread moves on to the system core, which frees core 1 for the
    // back end; refused or not honoured, it goes back to the core chosen above. SplitCore3Tick watches it there.
    const int planned = core >= 0 && core <= 2 && pin >= 0 ? port::cores::kUser0 << core : 0;
    const bool core3 = port_vita_thread_core3 && port_vita_thread_core3(3, planned) == port::cores::kSystem;
    if (core3) {
        sSplitPlannedMask = planned;
        port::core3::MarkOnCore3(sSplitCore3Slot = port::core3::RegisterThread("vgx-finish"), true);
        sSplitOnCore3.store(true, std::memory_order_release);
    }
    port_log("gx: front-end split: finishing thread on %s %d%s, priority 0x%X%s", core3 ? "the fourth core (planned core" : "core", core,
             core3 ? ")" : pin < 0 ? " (pinning refused)" : "", sceKernelGetThreadCurrentPriority(), rc < 0 ? " (GXV_SPLIT_PRIO refused)" : "");
#else
    (void)core;
    port_thread_class(3);
#endif
}

// GXV_FE_TWOCORE (vgx_twocore.h): the two-core front end's second worker places itself. Core 3 (the default): the fourth core, when
// the probe proved it (port_vita_core3_available; needs the CapUnlocker plug-in and a GXV_CORE3* part on), else nothing: on a user core it
// would only take the player's or the back end's time, and the switch stays off ("fetwo: the second worker could not have core ...").
// GXV_FE_TWOCORE_CORE=1|2 pins it to that user core instead. Priority GXV_FE_TWOCORE_PRIO (default 0xA0, twocore24; it was 0x93):
// below every thread whose work another thread waits for: the finishing thread (0x8F), the par workers (par-view / par-ptcl / par-anim:
// one step above the game's class, 0x9E: the game's draw waits WITHOUT a bound for a view job the par-view worker is in, par_async.h),
// the back end (0x9F, core 1) and the audio threads, so B only takes the time they leave and can starve none of them; when it is late
// the player does the segment itself. While B holds the shared tables' lock and the player waits for it, the player raises B to
// GXV_FE_TWOCORE_BOOST (default 0x8E) for the rest of that read (port_vgx2_boost); B gives it back at its next unlock.
#if defined(__vita__)
struct TwoCoreSelf {
    int Set(int mask) { return sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask); }
    int Get() { return sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId()); }
};
std::atomic<int> sTwoTid{-1};
int sTwoPrio = 0xA0, sTwoBoost = 0x8E;
} // namespace
// (vgx_twocore_impl.h) 1: the player raises B (its thread); 0: B gives the raise back (its own call)
extern "C" void port_vgx2_boost(int on) {
    const int tid = sTwoTid.load(std::memory_order_relaxed);
    if (tid < 0) return;
    if (on) sceKernelChangeThreadPriority(tid, sTwoBoost);
    else sceKernelChangeThreadPriority(0, sTwoPrio);
}
namespace {
#endif
int TwoCoreThreadInit(int core) {
    PORT_QEMU_THREAD("vgx-fe-b");
#if defined(__vita__)
    port_thread_class(3);
    bool placed = false;
    int mask = 0;
    if (core == 3) {
        if (port_vita_core3_available && port_vita_core3_available()) {
            TwoCoreSelf self;
            const port::cores::Placement p = port::cores::PlaceOnSystemCore(self, port::cores::kUser2);
            placed = p.onSystem;
            mask = p.mask;
        }
    } else if (core >= 1 && core <= 2) {
        placed = port_vita_pin_core(core) >= 0;
        mask = port::cores::kUser0 << core;
    }
    const long prio = vgx::EnvInt("GXV_FE_TWOCORE_PRIO", 0xA0);
    const int rc = placed && prio >= 0x40 && prio <= 0xBF ? sceKernelChangeThreadPriority(0, static_cast<int>(prio)) : 0;
    const long boost = vgx::EnvInt("GXV_FE_TWOCORE_BOOST", 0x8E);
    sTwoPrio = sceKernelGetThreadCurrentPriority();
    sTwoBoost = boost >= 0x40 && boost <= 0xBF ? static_cast<int>(boost) : sTwoPrio;
    if (placed) sTwoTid.store(sceKernelGetThreadId(), std::memory_order_relaxed);
    port_log("gx: two-core front end: second worker %s core %d (mask 0x%X), priority 0x%X%s, raised to 0x%X while the player waits for its lock",
             placed ? "on" : "could not have", core, static_cast<unsigned>(mask), sceKernelGetThreadCurrentPriority(),
             rc < 0 ? " (GXV_FE_TWOCORE_PRIO refused)" : "", static_cast<unsigned>(sTwoBoost));
    if (port_log_summary)
        port_log_summary("fetwo: second worker %s core %d, priority 0x%X (boost 0x%X)", placed ? "on" : "could not have", core,
                         static_cast<unsigned>(sTwoPrio), static_cast<unsigned>(sTwoBoost));
    return placed ? 1 : 0;
#else
    (void)core;
    port_thread_class(3);
    return 1;
#endif
}

void RegisterMemory() {
    // What the display-list and texture caches may keep decodes of: the console's two RAM
    // banks and the executable's own data (the game keeps some display lists and tables there).
    for (unsigned i = 0;; ++i) {
        void *base = nullptr, *alias = nullptr;
        uint32_t physical = 0, size = 0;
        int kind = 0;
        if (!port_memory_region(i, &base, &alias, &physical, &size, &kind)) break;
        if (kind <= 2) vgx::hle::RegisterGuestRam(base, size); // Mem1, Mem2, Image
    }
}

} // namespace

// The game's GX markers (PORT_GX_MARK_*, platform/include/port/compat.h) for the vgx front end, in the GX stream at the point
// the game calls them: a write of BP register 0xFF, which the console's GPU does not have (vgx_state.cpp WriteBP). Only the
// HUD's markers, and only with GXV_CHARAA, GXV_EDGE_AA or GXV_HUD_NATIVE on (vgx_draw.cpp, character / whole-picture edge smoothing: the HUD is never
// smoothed); otherwise nothing is written and the stream is the game's own, byte for byte. Game thread (the GX library's FIFO writer).
extern "C" uint32_t __PortArrValid;  // (GXV_ARRAYSKIP, port/array_skip.h)
#include "port/gx_dedup.h"            // (GXV_GX_DEDUP: a marker forgets the gate's shadow)
// GXV_BGLAYER=1 (bglayer25): the view (3x4 world-to-eye, projection scales) a layer picture's markers refer to, by slot (game thread, before the markers' draws).
extern "C" void port_vgx_bglayer_view(unsigned slot, const float* view12, float p00, float p11) { vgx::BgSetView(slot, view12, p00, p11); }
extern "C" void port_vgx_marker(uint32_t kind) {
    __PortArrValid = 0;  // (GXV_ARRAYSKIP: a marker's scope may put the front end's registers back; the array bases are written again after it)
    __PortGXDedupBump(PORT_GXDD_BUMP_ALL, PORT_GXDD_CAUSE_MARKER);  // (GXV_GX_DEDUP, port/gx_dedup.h: the same, for every register it knows)
    if ((kind & 0xFFu) == 10u) { // (PORT_GX_MARK_TRANSIENT_COPY + its stage tag in bits 8-23: only with GXV_INDCAP_SKIP, vgx_draw.cpp MarkTransientCopies)
        const bool cap = PORT_HOT_KNOB(bool, vgx::IndCapSkipMode() != 0);
        if (!cap) return;
        __PortGXFifo_u8(0x61);
        __PortGXFifo_u32((0xFFu << 24) | kind);
        return;
    }
    if ((kind & 0xFFu) >= 28u && (kind & 0xFFu) <= 31u) { // (PORT_GX_MARK_BGCAT 28: the census, bit 2 of GXV_BGLAYER; BG_BEGIN / BG_END / BG_REUSE 29..31: the layer cache, bit 1; vgx_bglayer.inc)
        if ((kind & 0xFFu) == 28u ? !PORT_HOT_KNOB(bool, (vgx::BgLayerMode() & 2) != 0) : !PORT_HOT_KNOB(bool, (vgx::BgLayerMode() & 1) != 0)) return;
        __PortGXFifo_u8(0x61);
        __PortGXFifo_u32((0xFFu << 24) | (kind & 0xFFFFFFu));
        return;
    }
    if ((kind & 0xFFu) == 35u || (kind & 0xFFu) == 36u) { // (PORT_GX_MARK_OWNER 35 / PORT_GX_MARK_FRESH_END 36, hrw27: only with GXV_HRW 1, 2 or 4 and GXV_HRW_FRESH on: the owner of the draws that follow / the end of a partial draw)
        if (!PORT_HOT_KNOB(bool, sHrwFreshOn)) return;
        __PortGXFifo_u8(0x61);
        __PortGXFifo_u32((0xFFu << 24) | (kind & 0xFFFFFFu));
        return;
    }
    if ((kind & 0xFFu) == 11u) { // (PORT_GX_MARK_PHASE + the phase in bits 8-15: only with GXV_GEOPASS, vgx_geopass.inc)
        const bool geo = PORT_HOT_KNOB(bool, vgx::GeoPassMode() != 0);
        if (!geo) return;
        __PortGXFifo_u8(0x61);
        __PortGXFifo_u32((0xFFu << 24) | (kind & 0xFFFFu));
        return;
    }
    if ((kind & 0xFFu) == 33u) { // (PORT_GX_MARK_STAGE_ENUM_DRAW + its arg in bits 8-23, precomp26: only with GXV_STAGE_ENUM on, vgx_draw.cpp StageEnumDrawMarker)
        if (!PORT_HOT_KNOB(bool, vgx::StageEnumMode() != 0)) return;
        __PortGXFifo_u8(0x61);
        __PortGXFifo_u32((0xFFu << 24) | (kind & 0xFFFFFFu));
        return;
    }
    if (kind == 14u || kind == 15u) { // (PORT_GX_MARK_WARM_BEGIN / _END, prewarm24: only with GXV_PREWARM or GXV_LAYOUTWARM on, vgx_draw.cpp WarmScopeMarker)
        if (!PORT_HOT_KNOB(bool, vgx::WarmScopeHonoured())) return;
        __PortGXFifo_u8(0x61);
        __PortGXFifo_u32((0xFFu << 24) | kind);
        return;
    }
    if ((kind & 0xFFu) == 19u || (kind & 0xFFu) == 20u) { // (PORT_GX_MARK_LAYOUT_BEGIN / _END + the census id in bits 8-23, layoutwarm24: only with GXV_LAYOUTWARM_LOG on)
        if (!PORT_HOT_KNOB(bool, vgx::LayoutCensusOn())) return;
        __PortGXFifo_u8(0x61);
        __PortGXFifo_u32((0xFFu << 24) | (kind & 0xFFFFFFu));
        return;
    }
    if ((kind & 0xFFu) == 32u) { // (PORT_GX_MARK_SILH_HINT + the hint in bits 8-15: GXV_SILH_GATE or GXV_SILH_OCC on. deadfix27: this branch was missing, so the marker fell to the HUD filter below and was
                                 // dropped on every console run: the renderer never saw a hint (silhgate "kept: hint says may show / none", silhocc "gated 0, unmatched = all results"))
        if (!PORT_HOT_KNOB(bool, vgx::SilhGateMode() != 0 || vgx::SilhOccMode() != 0)) return;
        __PortGXFifo_u8(0x61);
        __PortGXFifo_u32((0xFFu << 24) | (kind & 0xFFFFFFu));
        return;
    }
    if ((kind & 0xFFu) == 40u) { // (PORT_GX_MARK_BLOOM_BRACKET + the sub-kind in bits 8-15, bloom37: only with GXV_BLOOM_BRACKET on and GXV_HRW 0, vgx_draw.cpp BloomBracketMarker)
        if (!PORT_HOT_KNOB(bool, (vgx::BloomBracketMode() != 0 || vgx::TailFoldMode() == 3) && vgx::HrwMode() == 0)) return; // (GXV_TAILFOLD=3 needs the put-back's tag)
        __PortGXFifo_u8(0x61);
        __PortGXFifo_u32((0xFFu << 24) | (kind & 0xFFFFFFu));
        return;
    }
    if ((kind & 0xFFu) == 7u) { // PORT_GX_MARK_SCENE_DEPTH, before the game clears its 3D depth
        static const bool mask = vgx::EnvInt("GXV_HRW41_DEPTH", 0) != 0 || vgx::EnvInt("GXV_HRW41_BOXKEY", 0) != 0 ||
                                 (vgx::EnvInt("GXV_HRW36_PLANE_GATE", 0) != 0 && vgx::EnvInt("GXV_HRW36_PLANE_GATE_SRC", 1) == 1);
        static const unsigned sub = vgx::EnvInt("GXV_HRW41_MASK_AT", 0) != 0 ? 1u : 0u;
        if (!mask || !PORT_HOT_KNOB(bool, vgx::HrwMode() == 5) || ((kind >> 8) & 0xffu) != sub) return;
        __PortGXFifo_u8(0x61);
        __PortGXFifo_u32((0xffu << 24) | 7u);
        return;
    }
    if (vgx::hrwpair::IsMarker(kind)) {
        if (!PORT_HOT_KNOB(bool, vgx::HrwMode() == 1 || vgx::HrwMode() == 2 || vgx::HrwMode() == 5)) return;
        __PortGXFifo_u8(0x61);
        __PortGXFifo_u32((0xFFu << 24) | (kind & 0xFFFFFFu));
        return;
    }
    if (kind == 5u || kind == 6u) { // PORT_GX_MARK_PLAYER_BEGIN / _END: protect Mario/Luigi/MP puppet sub-meshes in AlMust()
        __PortGXFifo_u8(0x61);
        __PortGXFifo_u32((0xFFu << 24) | kind);
        return;
    }
    const bool wanted = PORT_HOT_KNOB(bool, vgx::CharaAAMode() != 0 || vgx::EdgeAAMode() != 0 || vgx::HudNativeMode() != 0 || vgx::HrwMode() != 0);  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
    if ((kind != 1u && kind != 2u && kind != 12u && kind != 13u) || !wanted) return; // (PORT_GX_MARK_HUD_BEGIN, _END; HUDN_BEGIN, _END: GXV_HUD_NATIVE's)
    if ((kind == 12u || kind == 13u) && !PORT_HOT_KNOB(bool, vgx::HudNativeMode() != 0)) return;
    __PortGXFifo_u8(0x61);
    __PortGXFifo_u32((0xFFu << 24) | kind);
}

// ---- mario24: "player-look:" (agents/mario24/REPORT.txt). The owner, 11 Nov: "Mario looks a little soft at 1.0x". A few seconds into every scene the game
// (MarioActor::portPlayerLook, decomp/src/Game/Player/MarioActorDraw.cpp) hands over what it knows of the player: his zoom, camera distance, size on the
// picture and depth. Three basic-log lines say what makes him sharp or soft on this console: which model he is drawn with, how many pixels tall, which
// of his textures are HD replacements with how many mip levels and which filter, the texture bias / anisotropy and every pass over the picture that
// touches him (backend::PlayerLookText), and where the game's depth-of-field ramp lies against his depth. Read only; nothing here changes a pixel.
extern "C" float port_pb_lod_scale(void) __attribute__((weak));
extern "C" float port_pb_anim_dist2(void) __attribute__((weak));
extern "C" float port_pb_shadow_clip(void) __attribute__((weak));
extern "C" int port_pb_budget_on(void) __attribute__((weak));
namespace {
std::atomic<int> sPlZ8{-1};                          // the player's depth, 0..255 (the depth-of-field ramp's scale), -1 unknown
std::atomic<int> sDofMin{-2}, sDofMax{-2};           // the ramp the blur last drew with (-1: the game's default; -2: it has not drawn)
std::atomic<float> sDofIntensity{0.0f};
const char* const kFiltName[6] = {"nearest", "bilinear", "nearest-mip", "mip-nearest", "trilinear-n", "trilinear"};
int PlayerDofOff() { return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_PLAYER_DOF"); return e && *e && atoi(e) == 0 ? 1 : 0; }()); }
} // namespace
extern "C" void port_player_look_z8(int z8) { sPlZ8.store(z8, std::memory_order_relaxed); }
extern "C" void port_player_look_dof(int minDist, int maxDist, float intensity) {
    sDofMin.store(minDist, std::memory_order_relaxed);
    sDofMax.store(maxDist, std::memory_order_relaxed);
    sDofIntensity.store(intensity, std::memory_order_relaxed);
}
// GXV_PLAYER_DOF=0 (default unset: the game's own ramp): the depth-of-field's ramp (Z8 levels, as the game draws it: 0 .. min no blur, min .. max rising to
// full) starts behind the player when his depth lies at or past its start, keeping its width. Returns 1 when it changed the ramp.
extern "C" int port_player_dof_clamp(int* pMin, int* pMax) {
    if (!PlayerDofOff()) return 0;
    const int z8 = sPlZ8.load(std::memory_order_relaxed);
    if (z8 < 0 || z8 > 250) return 0;
    const int start = z8 + 3;
    if (*pMin >= start) return 0; // (he is in front of it: nothing to do)
    const int width = std::max(*pMax - *pMin, 6);
    const int nmin = start, nmax = std::min(255, start + width);
    if (nmax <= nmin) return 0;
    static std::atomic<int> logged{0};
    if (logged.load(std::memory_order_relaxed) < 3 && port_log_summary) {
        logged.fetch_add(1, std::memory_order_relaxed);
        port_log_summary("player-look: GXV_PLAYER_DOF=0: the depth-of-field ramp %d..%d moved to %d..%d (the player's depth is %d of 255)", *pMin, *pMax, nmin, nmax, z8);
    }
    *pMin = nmin;
    *pMax = nmax;
    return 1;
}
extern "C" void port_player_look(float heightFrac, float camDist, float zoom, int z8, int isLuigi, int body) {
    if (!port_log_summary) return;
    const float lodScale = port_pb_lod_scale ? port_pb_lod_scale() : 1.0f;
    const float shadowClip = port_pb_shadow_clip ? port_pb_shadow_clip() : 0.0f;
    const float animDist = port_pb_anim_dist2 && port_pb_anim_dist2() > 0.0f ? std::sqrt(port_pb_anim_dist2()) : 0.0f;
    const bool pb = port_pb_budget_on && port_pb_budget_on() != 0;
    char rtText[48];
    vgx::PlayerTexFact f[vgx::kPlayerTexFactsMax];
    const int n = vgx::PlayerTexFacts(f, vgx::kPlayerTexFactsMax);
    char be[900];
    be[0] = 0;
    vgx::backend::PlayerLookText(be, sizeof be);
    // the render target's lines are in the back end's text; the picture height for the pixel figure is the 544-line screen, the render target's in it
    int rtH = 544;
    if (const char* x = std::strstr(be, "render target "); x) { int w = 0, h = 0; if (std::sscanf(x, "render target %dx%d", &w, &h) == 2 && h > 0) rtH = h; }
    std::snprintf(rtText, sizeof rtText, "%d-line render target", rtH);
    const int pxScreen = static_cast<int>(heightFrac * 544.0f + 0.5f), pxRt = static_cast<int>(heightFrac * static_cast<float>(rtH) + 0.5f);
    char dof[200];
    const int dMin = sDofMin.load(std::memory_order_relaxed), dMax = sDofMax.load(std::memory_order_relaxed);
    if (dMin == -2) std::snprintf(dof, sizeof dof, "depth of field has not drawn (not active here); player depth %d of 255", z8);
    else {
        const int mn = dMin >= 0 ? dMin : 242, mx = dMax >= 0 ? dMax : 248;
        std::snprintf(dof, sizeof dof, "depth of field DRAWS (ramp %d..%d of 255, intensity %.2f, GXV_DOF) and the player's depth is %d: %s", mn, mx,
                      static_cast<double>(sDofIntensity.load(std::memory_order_relaxed)), z8,
                      z8 >= mx ? "he is in the full blur" : (z8 > mn ? "he is IN the blur ramp" : "he is in front of it, sharp"));
    }
    port_log_summary("player-look: %s (body %d): ONE model, drawn at every distance (no LodCtrl: no Middle or Low model exists; perf-budget %s: lod x%.2f touches LodCtrl actors "
                     "only, never the player; its anim / shadow cuts (%.0f / %.0f units) and decor / fur never touch him); zoom %.1f, camera distance %.0f units, "
                     "about %d px tall on the %s and %d px on the 544-line screen (a 160-unit figure: an estimate); %s",
                     isLuigi ? "Luigi" : "Mario", body, pb ? "ON" : "off", static_cast<double>(lodScale), static_cast<double>(animDist), static_cast<double>(shadowClip),
                     static_cast<double>(zoom), static_cast<double>(camDist), pxRt, rtText, pxScreen, dof);
    if (n == 0) {
        port_log_summary("player-look: no player texture is known (no HD pack naming Mario's textures: GXV_HDTEX off or no pack file), so the per-texture switches "
                         "(GXV_PLAYER_LODBIAS, GXV_PLAYER_ANISO, GXV_PLAYER_MIPVIS) and the player's rectangle (GXV_EDGE_AA_PLAYER) have nothing to act on");
    } else {
        // largest first; at most 6 shown
        std::sort(f, f + n, [](const vgx::PlayerTexFact& a, const vgx::PlayerTexFact& b) { return static_cast<int>(a.w) * a.h > static_cast<int>(b.w) * b.h; });
        char list[700];
        int at = 0, hdN = 0;
        for (int i = 0; i < n; ++i) hdN += f[i].hd ? 1 : 0;
        for (int i = 0; i < n && i < 6 && at < static_cast<int>(sizeof list) - 120; ++i) {
            const float side = static_cast<float>(std::max(f[i].w, f[i].h));
            // if the texture spans about his height (the body atlas, roughly): the mip level the GPU reads is log2(texels / pixels)
            const float est = pxScreen > 0 ? std::log2(side / static_cast<float>(pxScreen)) : 0.0f;
            at += std::snprintf(list + at, sizeof list - static_cast<size_t>(at), "%s%ux%u%s %s%s lvl%u ~mip%.1f", i ? "; " : "", f[i].w, f[i].h,
                                f[i].hd ? " HD" : "", kFiltName[f[i].finalMin % 6], f[i].origMin != f[i].finalMin ? "*" : "", f[i].levels, static_cast<double>(est < 0.0f ? 0.0f : est));
        }
        port_log_summary("player-look: %d player textures (%d HD replacements, bound filter, * = not the game's own, lvl 0 = the back end makes the chain, ~mip = log2(side / "
                         "player px) assuming the texture spans his height, a model, not a measurement; GXV_PLAYER_MIPVIS=1 shows the real levels): %s", n, hdN, list);
    }
    port_log_summary("player-look: back end: %s", be);
}

// The front end's CPU time at its last display copy (perf.cpp's slow-turn line compares it across a turn).
extern "C" int64_t port_vgx_player_cpu_us(void) { return sPlCpuUs.load(std::memory_order_relaxed); }
extern "C" int port_vgx_active(void) { return sActive.load(std::memory_order_relaxed) ? 1 : 0; }
// Pictures the front end has handed to the back end that it has not finished drawing (the one being
// drawn included): the frame-skip rule in port/perf.cpp reads it.
extern "C" unsigned port_vgx_backend_pending(void) {
    const unsigned long long queued = sFramesQueued.load(std::memory_order_relaxed), shown = sFramesShown.load(std::memory_order_relaxed);
    return queued > shown ? static_cast<unsigned>(queued - shown) : 0u;
}
// pace25 (GXV_PENDING_HOLD25, perf.cpp): PresentSync::HoldAllowance, published by the display callback after each picture (one relaxed store), read by the game thread's skip rule.
static std::atomic<unsigned> sPendingAllow{0};
extern "C" void port_vgx_pending_allowance_set(unsigned n) { sPendingAllow.store(n, std::memory_order_relaxed); }
extern "C" unsigned port_vgx_pending_allowance(void) { return sPendingAllow.load(std::memory_order_relaxed); }
extern "C" unsigned port_vgx_pace_divisor(void) {
    return PaceMode() ? sPaceDivisor.load(std::memory_order_relaxed) : 0;
}
// The most frames the back end's queue held since the last call (GXV_LATENCY24's "latency:" line).
extern "C" unsigned port_vgx_queue_max_take(void) { return sQueueMax.exchange(0, std::memory_order_relaxed); }

// The FrameSink gate census since the last call (GXV_WAITLOG): waits, their total and longest, in us.
extern "C" uint64_t port_vgx_gate_peek_take(void) { return sGatePeekMaxUs.exchange(0, std::memory_order_relaxed); }
extern "C" int port_vgx_gate_wait_take(uint64_t* n, uint64_t* totalUs, uint64_t* maxUs) {
    *n = sGateWaitN.exchange(0, std::memory_order_relaxed);
    *totalUs = sGateWaitUs.exchange(0, std::memory_order_relaxed);
    *maxUs = sGateWaitMaxUs.exchange(0, std::memory_order_relaxed);
    return sWaitLog ? 1 : 0;
}
extern "C" uint64_t port_vgx_pace_wait_total_us(void) { return sPaceWaitUs.load(std::memory_order_relaxed); }
extern "C" void port_vgx_pace_waited(uint64_t us) { sPaceWaitUs.fetch_add(us, std::memory_order_relaxed); }
// pace26: the fixed 30's pictures, in the order they are drawn (game thread writes, the display callback reads): the turns each advanced and the display's counter at its turn.
static std::atomic<unsigned> sEvenW{0}, sEvenR{0};
static uint32_t sEvenSteps[64], sEvenVc[64];
extern "C" int port_vgx_even_take(uint32_t* steps, uint32_t* vcount) {
    unsigned r = sEvenR.load(std::memory_order_relaxed);
    const unsigned w = sEvenW.load(std::memory_order_acquire);
    if (r == w) return 0;
    if (w - r > 32) r = w - 1;  // (pictures that were never shown, a hidden frame or a reset: start again at the newest)
    *steps = sEvenSteps[r & 63];
    *vcount = sEvenVc[r & 63];
    sEvenR.store(r + 1, std::memory_order_relaxed);
    return 1;
}
extern "C" int port_vgx_pace_turn(int allowed) {
    const int mode = PaceMode();
    if (!mode) return -1;
    static port::pace::Controller pace = [mode] {
        port::pace::Controller c(mode == 2, mode == 3 ? port::pace::Policy::Fixed30 : port::pace::Policy::Adaptive);
        // GXV_SLIP (read in port/perf.cpp, which starts and ends the turns; default 0 = the slip never engages): Locked 30 only.
        if (mode == 3) c.UseSlip(&port::pace::GlobalSlip());
        if (mode == 3) {  // (smooth25, frame_pace.h SetPassOffer: default 0)
            const bool pass = vgx::EnvInt("GXV_PACE_PASSOFFER", 0) != 0;
            c.SetPassOffer(pass);
            if (pass) port_log("pace: GXV_PACE_PASSOFFER=1: a due turn the pacer leaves out passes its picture on to the next turn (a 3-vblank hold, not 4)");
        }
        if (mode == 3) {  // (pace26, frame_pace.h SetEvenQ: default 0 = today's rules)
            // GXV_EVEN26_Q: 0 off; 1 on with the back-end guard at 4 pictures; 2..6 on with the guard at that many. A due turn then draws whatever the backlog (below the guard) and the
            // late catch-up goes to the turn that is left out anyway: the 2-vblank rhythm is not broken by a busy back end or a late game thread.
            long q = vgx::EnvInt("GXV_EVEN26_Q", 0);
            if (q < 0 || q > 6) q = 0;
            if (q) {
                c.SetEvenQ(q == 1 ? 4u : static_cast<unsigned>(q));
                port_log("pace: GXV_EVEN26_Q=%ld: Quality's picture on every second turn is not left out for a back-end backlog under %ld pictures, and a late turn pays its catch-up on the other turn", q, q == 1 ? 4L : q);
            }
        }
        c.SetForgive(port::pace::StallForgiveNs(vgx::EnvInt("GXV_STALL_FORGIVE", 0)));
        c.SetDebtCap(port::pace::DebtCapNs(vgx::EnvInt("GXV_DEBT_CAP", 0)));  // (strk27: GXV_DEBT_CAP, perf.cpp)  // (GXV_STALL_FORGIVE, perf.cpp: a long gap between two turns' starts drops the debt)
        return c;
    }();
    const uint64_t forgivenBefore = pace.Forgiven();
    const unsigned before = pace.Divisor();
    const uint64_t backendUs = sPaceBackendUs.load(std::memory_order_relaxed);
    const uint64_t wholeUs = port_autoq_picture_work_us ? port_autoq_picture_work_us() : 0;
    if (mode == 3 && port::pace::GlobalSpeedGuard().Enabled()) {
        pace.SetOwns(port::pace::GlobalSpeedGuard().Owns());                              // (speed31: while the guard repays, the rules' own late catch-up stays off)
        pace.SetExtra(port::pace::GlobalSpeedGuard().WantExtra());                       // (speed30, GXV_SPEED_GUARD: one more turn without a picture for this one's picture)
    }
    const int decision = pace.Turn(static_cast<uint64_t>(port_host_time_ns() / 1000), allowed != 0,
                                   port_screen_quiet && port_screen_quiet() != 0, port_vgx_backend_pending(),
                                   backendUs, wholeUs);
    if (mode == 3 && pace.ExtraTaken()) port::pace::GlobalSpeedGuard().MarkExtra();
    sPaceDivisor.store(pace.Divisor(), std::memory_order_relaxed);
    if (pace.Forgiven() != forgivenBefore) {
        static unsigned logged = 0;
        if (logged++ < 20) {
            if (port_log_summary) port_log_summary("pace: stall forgiven (GXV_STALL_FORGIVE, fixed 30 / paced): the gap before this turn's start dropped the debt (%llu so far)", static_cast<unsigned long long>(pace.Forgiven()));
            else port_log("pace: stall forgiven (GXV_STALL_FORGIVE): the gap before this turn's start dropped the debt (%llu so far)", static_cast<unsigned long long>(pace.Forgiven()));
        }
    }
    if (mode == 3) {  // (pace26: the "even:" line needs the turns a picture advanced, which the fixed 30's display callback does not see: a ring, picture by picture)
        static unsigned since = 0;
        ++since;
        if (decision == 0 && pace.PresentAllowed()) {
            const unsigned w = sEvenW.load(std::memory_order_relaxed);
            sEvenSteps[w & 63] = since;
            sEvenVc[w & 63] = port_vgx_vita_vcount ? port_vgx_vita_vcount() : 0u;
            sEvenW.store(w + 1, std::memory_order_release);
            since = 0;
        }
    }
    if (mode == 3 && decision == 0) {
        // Same producer FIFO as the draw commands, never the CharaAA-filtered HUD hook.
        __PortGXFifo_u8(0x61);
        __PortGXFifo_u32((0xFFu << 24) | (pace.PresentAllowed() ? vgx::kPresentAllowedMarker : vgx::kPresentSuppressedMarker));
    }
    if (before != pace.Divisor()) {
        if (port_log_summary) port_log_summary("pace: quiet transition to %u vblanks per picture (simulation remains 60 Hz)", pace.Divisor());
        else port_log("pace: quiet transition to %u vblanks per picture (simulation remains 60 Hz)", pace.Divisor());
    }
    static uint64_t reported = 0;
    if (pace.Mismatches() != reported) {
        reported = pace.Mismatches();
        if (port_log_summary) port_log_summary("pace: cadence oracle mismatches %llu", static_cast<unsigned long long>(reported));
        else port_log("pace: cadence oracle mismatches %llu", static_cast<unsigned long long>(reported));
    }
    return decision;
}
// GXV_PRESENT_SYNC (frame_pace.h PresentSync; read here and in vgx_host_galaxy_vita.cpp, which shows the pictures): 0 off, 1 measure
// only (the "present:" line, pictures shown when ready as before), 2 pictures held to the vblank that matches their turn. Off on its
// own while GXV_PACE / GXV_FPS_LIMIT=30 own the display's callback. Called by port/perf.cpp at every turn's frame-skip decision (game
// thread): the turn's vblank, and for a turn that draws, that vblank (low 16 bits) into the GX stream ahead of its draws
// (vgx_state.cpp WriteBP 0xFF -> the picture's CmdBuf::presentStamp -> the host's present hook).
extern "C" int port_vgx_present_sync_mode(void) {
    return PORT_HOT_KNOB(int, [] {  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
        int m = static_cast<int>(vgx::EnvInt("GXV_PRESENT_SYNC", 0));
        if (m < 0 || m > 2) m = 0;
#if defined(__vita__)
        if (!port_vgx_vita_vcount) m = 0;
#else
        m = 0;  // (the display callback is the Vita host's)
#endif
        if (m && PaceMode() != 0) m = 0;
#if defined(__vita__)
        if (m != 2 && port_vgx_vita_vcount && PaceMode() == 0 && port::pace::Hrw5Pacing()) m = 2;  // (warp32 PATCH7: mode 5 needs it; frame_pace.h Hrw5Pacing)
#endif
        return m;
    }());
}
extern "C" void port_gxcap_turn(void);  // (gx_capture.cpp: GXV_GXCAPTURE_SELFTEST's turn point and the "gxcap:" line)
static std::atomic<uint32_t> sHrwTurnVblank{0};
extern "C" void port_vgx_present_turn(int drawn) {
    port_gxcap_turn();
#if defined(__vita__)
    if (!port_vgx_present_sync_mode()) return;
    static port::pace::TurnClock clock;
    const uint32_t vcount = port_vgx_vita_vcount();
    // GXV_STALL_FORGIVE (perf.cpp): after a stall the turn clock would stay kMaxBehind (4) vblanks behind the display for good (every later turn is one
    // vblank on), so each picture looks 4 vblanks late to PresentSync: "misses", and its latency L is raised for the next ~6 s. It takes the display's counter again instead.
    const int64_t forgiveNs = port::pace::StallForgiveNs(PORT_HOT_KNOB(int, static_cast<int>(vgx::EnvInt("GXV_STALL_FORGIVE", 0))));
    static int64_t lastTurnNs = 0;
    const int64_t nowNs = port_host_time_ns();
    if (forgiveNs && lastTurnNs && nowNs - lastTurnNs > forgiveNs) clock.Reset();
    lastTurnNs = nowNs;
    const uint32_t vblank = clock.Turn(vcount);
    sHrwTurnVblank.store(vblank, std::memory_order_relaxed);  // (GXV_HRW: the second picture of a pair is stamped with the vblank of the turn after the first's)
    if (drawn) {
        // GXV_LATENCY24: what the display callback needs to say how long the pad took to reach the screen (the picture of this turn shows what
        // the game's last pad read did; the read's sample time is the pad's)
        if (port_vgx_vita_note_turn) port_vgx_vita_note_turn(vblank, vcount, port_host_time_ns(), port_input_read_sample_ns ? port_input_read_sample_ns() : 0);
        if (LaneOn()) LaneTurn(vblank & 0xFFFFu, nowNs);   // (pace33 PATCH4: the "fe-lane:" line's turn start)
        // Same producer FIFO as the draw commands (as the fixed-30 marker above); the stamp rides in bits 8-23 of the BP value.
        __PortGXFifo_u8(0x61);
        __PortGXFifo_u32((0xFFu << 24) | ((vblank & 0xFFFFu) << 8) | vgx::kPresentStampMarker);
    }
#else
    (void)drawn;
#endif
}

// ---- GXV_HRW (hrw26): the game thread's side (GameSystem::frameLoop calls these four; vgx_hrw.h has the arithmetic).
//   port_hrw_turn_begin()          every turn, first thing
//   port_hrw_draw_begin()          a turn that draws, just before its draw: V_A = the camera the picture is drawn with; opens a pair (and, with GXV_HRW 1 / 2, marks the picture)
//   port_hrw_undrawn_begin()       a turn without a picture: if a pair is open, D = V_B * V_A^-1 with V_B the camera after the last turn's update; asks for the second picture
//   port_hrw_turn_end(drawn)       every turn, before the retrace wait: the turn's work time (the census; the governor's costs)
//   port_hrw_camera_cut()          (CameraDirector) an explicit camera change: the pair in flight is not re-projected
// GXV_HRW_ROT_DEG (default 4) / GXV_HRW_TRANS (default 600): a camera change larger than this between the pair's two cameras is not re-projected either (the guard band: the
// retained picture was culled against camera A, so a bigger change would show an empty edge).
// 0 off. 1 on. 2 verify-still: B = A again. 3 census: the log line only, nothing is marked, kept or replayed.
namespace {
std::atomic<uint32_t> sHrwCut{0};
std::atomic<int> sHrwStill{-1};   // (pace33: port_hrw_still)
port::hrwstage::Choice sHrwChoice;   // (pace33 PATCH6; game thread)
bool sHrwChoiceAuto = false;          // (emufix31, log only) the locked 30 in force comes from GXV_HRW_STAGE_AUTO's fallback, not from the stage's row / GXV_HRW_STAGE
struct HrwGame {
    int64_t turnT0 = 0;
    bool open = false;
    bool pairLive = false;             // (hrw29) a pair was opened by the last drawn turn and its undrawn turn has not yet ended: only such pairs enter the census sums
    uint32_t pairsDone = 0;            // (hrw29) pairs whose undrawn turn ended: the census's denominator
    uint32_t setKey = 0, settle = 0;   // (hrw29) the stage the settle count is for, and the drawn turns it has played with the screen open
    uint32_t offLogged = 0;
    uint32_t id = 0, cutAt = 0;
    float vA[12] = {};
    float plA[3] = {};                 // (warp32) the player's centre in the first picture's view space (plOk: there is one)
    bool plOk = false;
    double drawnMs = 0, undrawnMs = 0;
    bool engaged = true;               // the governor's verdict (mode 1); modes 2 and 3 are always on
    vgx::hrw::Governor gov;
    uint32_t govKey = 0;
    // census (5 s)
    int64_t logT0 = 0, secT0 = 0;
    uint32_t whyNotSettled = 0, whyStageOff = 0, whyNoCamera = 0;  // (PATCH16: drawn turns that opened no pair, by reason, for the "hrw: no pair" line)
    uint32_t whyStageAuto = 0;  // (emufix31: of whyStageOff, the turns whose locked 30 is GXV_HRW_STAGE_AUTO's fallback, not the row's)
    uint32_t pairs = 0, noPair = 0, cuts = 0, guarded = 0, asked = 0, fit = 0, guardDrops = 0, guardKept = 0;  // (guardDrops, hrw31: second pictures the game-speed guard gave up; guardKept, pace33 PATCH9: kept in mode 5, without the fresh player)
    double sumDrawn = 0, sumUndrawn = 0, sumPair = 0, maxPair = 0, sumRot = 0, maxRot = 0, maxTrans = 0;
    uint32_t rotBin[5] = {};           // < 0.1, < 0.5, < 1, < 2, >= 2 degrees
    uint32_t pictures = 0;             // (the second's) drawn turns
    double secDrawn = 0, secUndrawn = 0;
    uint32_t secDrawnN = 0, secUndrawnN = 0, secFeCpu = 0, secFePic = 0;
    uint32_t secPairsN = 0;            // (gov77) pairs opened in the governor's second (the denominator of the second pictures shown)
    uint32_t feCpu0 = 0, fePic0 = 0;
    uint64_t shown0 = 0;
    // hrw27: the partial draw
    bool freshPending = false;         // undrawn_begin has a pair's ask that port_hrw_fresh_draw sends (with the partial draw, or plain)
    uint32_t freshId = 0, freshStamp = 0;
    float freshD[12] = {};
    uint32_t freshDraws = 0, freshEmpty = 0;
    double freshMs = 0, freshMaxMs = 0;
    // warp32 PATCH22 (GXV_HRW_FRESH_AUTO, mode 5): the fresh player only while the pairs have room
    bool freshProv = false;            // (warp32 PATCH23) this pair's plain ask is out already (GXV_HRW_FRESH_DEADLINE_MS10)
    bool freshAutoOff = false;         // the rule has the fresh draw off (read when a pair's ask is made: a pair has it or has it not)
    bool pairNoFresh = false;          // frozen before the ordered A marker, never inferred from a late B
    uint32_t freshAutoHist = 0;        // the last 16 pairs, bit set = over the off threshold
    uint32_t freshAutoCalm = 0;        // pairs in a row under the on threshold while off
    uint32_t freshAutoOffN = 0, freshAutoOnN = 0, freshAutoWith = 0, freshAutoWithout = 0;  // the window's switches and pairs
    // mariolag31 (GXV_HRW_MARIOTIME, port/mario_time.h): the pair's fresh draw as the auto rule's cost mode reads it, the player's world centre at the drawn turn, the census
    bool pairFreshDrawn = false;
    double pairFreshMs = 0.0;
    float pwA[3] = {0.0f, 0.0f, 0.0f};
    port::mariotime::Census mt;
    port::mariotime::HoldLimit mtLimit;   // lost49: the holds are bounded (port/mario_time.h)
    int64_t boundaryApplyAt = 0;
    bool wantEngaged = true;           // the governor's verdict, applied at the next pair boundary (g)
    uint32_t deferredSwitch = 0;
    // hrw28: the picture mix (10 s) and the governor's status as last logged
    uint32_t cumPairs = 0, cumNoPair = 0, cumCuts = 0, cumGuard = 0, cumAsked = 0, cumFit = 0, cumA = 0, mixTick = 0;
    uint64_t mixRun0 = 0, mixMerged0 = 0, mixDrop0 = 0;
    int lastStatus = -1;
    bool qBlocked = false;
    bool secNonPlay = false;           // (even60: any turn in this 1 Hz window was in a load, opening camera, starter, movie or demo)
    vgx::hrw::Costs lastCosts;
    bool haveCosts = false;
} sHg;
int HrwEff() {
    if (sHrwDisabled.load(std::memory_order_relaxed)) return 0;
    static const int m = [] {
        int v = vgx::HrwMode();
        const int asked = v;
        const int sync = port_vgx_present_sync_mode(), pace = PaceMode();
        const long pattern = vgx::EnvInt("GXV_SKIP_PATTERN", 0), patternEff = pattern == 0 && port::pace::Hrw5Pacing() ? 2 : pattern;
        const bool nosync = vgx::EnvInt("GXV_HRW_NOSYNC", 0) != 0;
        const char* why = nullptr;
        if ((v == 1 || v == 2 || v == 5) && sync != 2 && !nosync) {
            if (port_log_summary) port_log_summary("hrw: GXV_HRW=%d needs GXV_PRESENT_SYNC=2 (the second picture is held to its turn's vblank); HRW is off (GXV_HRW_NOSYNC=1 overrides, for hosts)", v);
            v = 0;
#if defined(__vita__)
            why = !port_vgx_vita_vcount ? "this build has no vblank counter (port_vgx_vita_vcount): no present sync"
                : pace != 0 ? "a pacing mode owns the display (GXV_FPS_LIMIT=30 or GXV_PACE) and GXV_HRW_PACING_AUTO=0 keeps it"
                : "GXV_PRESENT_SYNC is not 2 and GXV_HRW_PACING_AUTO=0 keeps it";
#else
            why = "not a console build: no present sync here (GXV_HRW_NOSYNC=1 runs it without, for hosts and the emulator's tests)";
#endif
        }
        if (asked == 5 && port_log_summary) {  // (warp32 PATCH7: the start-up self-check: one line, engaged or the exact reason)
            if (v == 5)
                port_log_summary("hrw: mode 5 engaged%s: pacing %d (%s), present sync %d%s, turn rhythm GXV_SKIP_PATTERN=%ld%s%s", 
#if defined(__vita__)
                                 " on the console",
#else
                                 " (not a console build)",
#endif
                                 pace, pace == 0 ? "no cap, as mode 5 needs" : "A CAP OWNS THE DISPLAY", sync, nosync && sync != 2 ? " (GXV_HRW_NOSYNC=1: second pictures are not held to their vblank)" : "",
                                 patternEff, patternEff == 2 ? "" : " (NOT 2: pairs will not be first picture / second picture in turn)",
                                 port::pace::Hrw5Pacing() ? "; pacing chosen by mode 5 (GXV_HRW_PACING_AUTO)" : "; pacing as the switches say (GXV_HRW_PACING_AUTO=0)");
            else
                port_log_summary("hrw: mode 5 NOT engaged: %s (pacing %d, present sync %d, GXV_SKIP_PATTERN=%ld)", why ? why : "?", pace, sync, patternEff);
        }
        return v;
    }();
    return m;
}
// (14 Nov, owner on the console: the dome's galaxy map "flickers a bit, especially on the buttons") a menu the game draws over a live scene (the dome's galaxy
// selector, the star select) is not play: no pairs there, a plain picture every turn. The game calls port_hrw_menu_turn() on each such turn; HRW-only (the rest of
// the port's "quiet screen" rules are not touched, so no deferred work moves into these screens).
std::atomic<uint64_t> sHrwMenuFrame{0};
std::atomic<uint32_t> sHrwMenuTurns{0};
bool HrwMenuNow() {
    const uint64_t at = sHrwMenuFrame.load(std::memory_order_relaxed);
    const uint64_t now = sFramesQueued.load() + 1;   // (both sides + 1, so a note at frame 0 is not "none")
    return at != 0 && now >= at && now - at <= 3;
}
bool HrwPlay() {
    const uint32_t key = port_scene_key ? port_scene_key() : 0;
    const int q = port_screen_quiet ? port_screen_quiet() : 0;
    return key != 0 && q == 0 && !HrwMenuNow();
}
// even60: real gameplay only (never opening camera tours, scenario starters, movies, demos/cutscenes, loads, or pre-play flow legs).
bool Even60Play() {
    const bool loading = port_scene_loading_active && port_scene_loading_active() != 0;
    const int skipKind = port_skip_context_kind ? port_skip_context_kind() : 0;
    const int flight = port_popin_flight ? port_popin_flight() : 0;
    const int flowTurns = port_flow_play_turns ? port_flow_play_turns() : -2;
    return port::hrwmp::PlayActive(HrwPlay(), loading, skipKind, flight, flowTurns);
}
// hrw29: "in play": a real stage (not the title's FileSelect), the screen open, no scene being loaded, and GXV_HRW_SETTLE (default 90) drawn turns since the stage, the pause or the wipe began.
// Everything else (menus, loading, scene changes, pause, wipes, cut-scenes the game draws as a movie) runs with HRW out of the way: no pair, no marker, no partial draw, no ask.
// Called by the game thread once per drawn turn (draw_begin: it advances the count) and by the undrawn turn (undrawn_begin: it only tests, so a scene change between the pair's two turns drops the pair).
// stutter37 (15 Nov, port/hrw_settle.h; tests/hrw_settle_test.cpp): GXV_HRW_SETTLE_RESUME=<drawn turns> (default -1 = as GXV_HRW_SETTLE, every answer as before): the count
// when the same scene had already settled and nothing was loaded since (a pause, a closed fade). Before the pairs begin only every second turn is drawn, so Performance's
// GXV_HRW_SETTLE=200 is 6.7 s at 30 pictures a second, after every pause and fade. One "hrw: settle" line each time the short count let the pairs begin (the first 24).
bool HrwSettled(HrwGame& g, bool advance) {
    static const uint32_t fileSelect = [] { uint32_t h = 2166136261u; for (const char* c = "FileSelect"; *c; ++c) h = (h ^ static_cast<unsigned char>(*c)) * 16777619u; return h; }();
    static const unsigned need = static_cast<unsigned>(vgx::EnvInt("GXV_HRW_SETTLE", 90));
    static const long resume = vgx::EnvInt("GXV_HRW_SETTLE_RESUME", -1);
    static port::hrwsettle::Settle settle = [] { port::hrwsettle::Settle s; s.Configure(need, resume); return s; }();
    const uint32_t key = port_scene_key ? port_scene_key() : 0;
    const bool loading = port_scene_loading_active && port_scene_loading_active() != 0;
    const bool live = HrwPlay() && key != fileSelect && !loading;
    const bool ok = settle.Update(key, live, loading, advance);
    g.setKey = live ? key : 0;   // (kept for the reader: the scene the count is for, and the count)
    g.settle = settle.Count();
    if (advance) {
        static bool was = false;
        static unsigned logged = 0;
        if (ok && !was && settle.Resumed() && settle.Need() < need && logged < 24 && port_log_summary) {
            ++logged;
            port_log_summary("hrw: settle | GXV_HRW_SETTLE_RESUME %ld: pairs again %u drawn turns after the screen opened (the same scene, nothing loaded since it settled; GXV_HRW_SETTLE is %u) (%u so far)",
                             resume, settle.Need(), need, logged);
        }
        was = ok;
    }
    return ok;
}
}  // namespace
// (14 Nov) the game, once per turn while a selector menu is on screen (AstroDomeGalaxySelector, ScenarioSelectScene): see HrwMenuNow.
extern "C" void port_hrw_menu_turn(void) {
    sHrwMenuFrame.store(sFramesQueued.load() + 1, std::memory_order_relaxed);
    sHrwMenuTurns.fetch_add(1, std::memory_order_relaxed);
}
// hrw27: the game's cull frustum is widened by this factor (its half-width and half-height) while pairs are made, so that what the second picture's camera turns into was drawn in the first.
// GXV_HRW_CLIP_PAD in percent, default 120 (a 4 degree guard band at a 45 degree field of view needs about 119); 100 = as the game. 1 while HRW is off or idle.
// gov5: this mode is mode 5 with the governor on (and Performance). Game thread and the pacing decision (the same thread).
static bool Gov5(int m) { return m == vgx::hrw::kWarp && Gov5Key() && !HrwForceOn(); }
static double Gov5WarpMs() { static const double ms = static_cast<double>(vgx::EnvInt("GXV_HRW_GOV5_WARP_MS10", 20)) / 10.0; return ms; }
// mp30 (port/hrw_mp.h): the picture policy. GXV_HRW_EVEN60 (default 1; 0 = as before: plain 60 with its skips), GXV_HRW_ALLOW50 (default 0; 1 = env.txt escape hatch restoring old pacing).
static bool MpPolicyEnabled() {
    static const bool on = port::hrwmp::Enabled(vgx::EnvInt("GXV_HRW_EVEN60", 1), vgx::EnvInt("GXV_HRW_ALLOW50", 0));
    return on;
}
static port::hrwmp::Params MpParams() {
    port::hrwmp::Params p;
    p.on = MpPolicyEnabled();
    p.settlePlay = static_cast<int>(vgx::EnvInt("GXV_HRW_EVEN60_SETTLE_S", 8));
    p.trialFirst = static_cast<int>(std::clamp<long>(vgx::EnvInt("GXV_HRW_EVEN60_TRIAL_S", 10), 3, 600));
    p.trialCap = static_cast<int>(std::clamp<long>(vgx::EnvInt("GXV_HRW_EVEN60_TRIAL_CAP_S", 30), p.trialFirst, 600));
    p.gapSec = static_cast<double>(vgx::EnvInt("GXV_HRW_EVEN60_GAP_S", 6));
    p.blockRetrySec = static_cast<double>(vgx::EnvInt("GXV_HRW_EVEN60_BLOCK_RETRY_S", 30));
    p.dropBelowFps = static_cast<double>(vgx::EnvInt("GXV_HRW_EVEN60_DROP_FPS", 52));
    p.trialFps = static_cast<double>(vgx::EnvInt("GXV_HRW_EVEN60_TRIAL_FPS", 56));
    p.holdsFastReturn = static_cast<int>(std::clamp<long>(vgx::EnvInt("GXV_HRW_EVEN60_FAST_S", 3), 0, 60));
    p.assist = vgx::EnvInt("GXV_HRW_EVEN60_ASSIST", 0) != 0;   // (mp31: plain 60 <-> an even 30 only, by default)
    return p;
}
static port::hrwmp::Policy sMp;       // game thread
static bool sMpActive = false;        // the policy is in force (a session, mode 5 under the governor, Performance)
// even60 without 60 Assist (GXV_HRW=0, Performance, single player; GXV_HRW_EVEN60_NOASSIST default 1):
// runs plain 60 <-> even 30 via perf.cpp's 2-of-4 SkipPattern when 60 Assist is turned off in Settings.
static std::atomic<int> sEven60NoAssist30{0};
// 1 while single-player even60 is running plain 60 with 60 Assist disabled (GXV_HRW_EVEN60_ASSIST=0):
// pairs can never engage here, so GXV_HRW_GOV5_EXACT must not deadlock GXV_INDCAP_HALFRATE=2 on <57 fps stages.
static std::atomic<int32_t> sEven60PlainNoAssist{0};
static inline void SyncIndcapPlainNoAssist() {
    sEven60PlainNoAssist.store(sMpActive && (!MpParams().assist || port::lockstep::On()) && sMp.Mode() == port::hrwmp::kPlain60 ? 1 : 0, std::memory_order_relaxed);
}
static bool Even60NoAssistEligible(int m) {
    static const bool sw = vgx::EnvInt("GXV_HRW_EVEN60_NOASSIST", 1) != 0;
    static const bool otherCtl = vgx::EnvInt("GXV_STEADY30", 0) != 0 || vgx::EnvInt("GXV_SMOOTH60", 0) != 0 || vgx::EnvInt("GXV_RATE_AUTO", 0) != 0 ||
                                 vgx::EnvInt("GXV_SKIP_PATTERN", 0) != 0 || vgx::EnvInt("GXV_PACE25", 0) != 0 || vgx::EnvInt("GXV_EVEN26", 0) != 0;
    static const bool fixedCap = PaceMode() != 0 || vgx::EnvInt("GXV_FPS_LIMIT", 0) == 30;
    static std::atomic<int32_t>& knob = port::stageknob::LiveInt("GXV_HRW_STAGE", 0, false);
    const int eff = m != 0 ? m : vgx::HrwMode();
    return port::hrwmp::InForceNoAssist(MpPolicyEnabled(), sw, eff, port::stageknob::Quality(), knob.load(std::memory_order_relaxed), fixedCap, otherCtl);
}
static void Even60NoAssistSync(uint32_t key) {
    static uint32_t seenKey = 0;
    const bool nowOn = Even60NoAssistEligible(0);
    if (nowOn != sMpActive || (nowOn && key != seenKey)) {
        const bool starting = nowOn && !sMpActive;
        sMpActive = nowOn;
        if (nowOn) {
            port::hrwmp::Params p = MpParams();
            p.assist = false;   // no 60 Assist pairs when GXV_HRW=0
            sMp.Configure(p);
            sMp.Begin(static_cast<double>(port_host_time_ns()) / 1e9, port::hrwmp::kPlain60);
            sHg.secNonPlay = false;
            if (port_log_summary) {
                if (starting)
                    port_log_summary("even60 (no-assist): 60 Assist is off; plain 60 <-> even 30 active (GXV_HRW_EVEN60_NOASSIST=1, GXV_HRW_EVEN60=%ld, GXV_HRW_ALLOW50=%ld)",
                                     vgx::EnvInt("GXV_HRW_EVEN60", 1), vgx::EnvInt("GXV_HRW_ALLOW50", 0));
                port_log_summary("even60 (no-assist): stage %08x: %s: now %s", key, starting ? "start" : "new scene", port::hrwmp::ModeName(port::hrwmp::kPlain60));
            }
        } else if (port_log_summary) {
            port_log_summary("even60 (no-assist): not in force now");
        }
    }
    seenKey = key;
    sEven60NoAssist30.store(sMpActive && sMp.Mode() == port::hrwmp::kEven30 ? 1 : 0, std::memory_order_relaxed);
    SyncIndcapPlainNoAssist();
}
extern "C" int port_hrw_even30_noassist(void) { return sEven60NoAssist30.load(std::memory_order_relaxed); }
// gov5: the thresholds (owner, 21 Nov: only a step down; at or near 60, fluctuating, stay on plain 60). Engage: the plain rate under 55 fps in 5 of the last 7 seconds of PLAY (pause, loads, menus are not play and
// restart the window), after the settle (4 s) and after the GPU ladder had its chance (up to 12 s more, only while the stage is GPU-limited). Leave: in 4 of 5 seconds the game turn and front end are
// predicted under 17.5 ms and a first picture's GPU under 17 ms (plain 60 at >= ~57 fps), 8 s between changes, 6 changes a stage, a leave taken back within 20 s makes the next one 6 % stricter.
static vgx::hrw::Governor::Params Gov5Params() {
    vgx::hrw::Governor::Params p;
    p.warp = true;
    p.engageBelowFps = static_cast<double>(vgx::EnvInt("GXV_HRW_GOV5_BELOW", 55));
    p.settleSec = static_cast<double>(vgx::EnvInt("GXV_HRW_GOV5_SETTLE_S", 4));
    p.gapSec = static_cast<double>(vgx::EnvInt("GXV_HRW_GOV5_GAP_S", 8));
    p.maxChanges = static_cast<int>(vgx::EnvInt("GXV_HRW_GOV5_MAXCHANGES", 6));
    p.win = static_cast<int>(std::clamp<long>(vgx::EnvInt("GXV_HRW_GOV5_WIN", 12), 4, 16));  // (owner 21 Nov: only a SUSTAINED drop engages: 10 of the last 12 play seconds under 55, never one dip)
    p.needFit = static_cast<int>(std::clamp<long>(vgx::EnvInt("GXV_HRW_GOV5_NEED", 10), 1, p.win));
    p.needLeave = static_cast<int>(std::clamp<long>(vgx::EnvInt("GXV_HRW_GOV5_NEED_LEAVE", 9), 1, p.win));  // (owner 21 Nov: leaves on a SUSTAINED recovery too: plain 60 predicted to hold in 9 of the last 12 play seconds)
    p.gov77 = vgx::EnvInt("GXV_HRW_GOV77", 1) != 0;   // (gov77: Assist only where the game thread keeps plain 60 away and the GPU has room for the pair; B lost or the GPU over budget: plain at once, blocked for the stage)
    p.engageGameMs = static_cast<double>(vgx::EnvInt("GXV_HRW_GOV77_GAME_MS10", 170)) / 10.0;
    p.warpDefaultMs = Gov5WarpMs();
    p.bMinShown = static_cast<double>(vgx::EnvInt("GXV_HRW_GOV77_BSHOWN", 90)) / 100.0;
    p.bMinSecs = static_cast<int>(std::clamp<long>(vgx::EnvInt("GXV_HRW_GOV77_BSECS", 2), 1, 6));
    p.fix77 = vgx::EnvInt("GXV_GOV77_FIX", 0) != 0;   // (gov24: the "limited by the GPU" verdict from measured figures only; the engage rule is unchanged)
    p.gpuLimitMs = static_cast<double>(vgx::EnvInt("GXV_GOV77_GPU_MS10", 160)) / 10.0;
    p.pairPadMs = vgx::EnvInt("GXV_GOV77_PAIRPAD", 0) != 0 ? static_cast<double>(vgx::EnvInt("GXV_GOV77_PAIRPAD_MS10", 21)) / 10.0 : 0.0;   // (gov24: see Params::pairPadMs)
    p.leaveMs = static_cast<double>(vgx::EnvInt("GXV_HRW_GOV5_LEAVE_MS10", p.gov77 ? 155 : 175)) / 10.0;   // (gov77: under the engage limit, so a leave never meets its own engage)
    p.leaveGpuMs = static_cast<double>(vgx::EnvInt("GXV_HRW_GOV5_LEAVE_GPU_MS10", 170)) / 10.0;
    p.relapseSec = static_cast<double>(vgx::EnvInt("GXV_HRW_GOV5_RELAPSE_S", 20));
    p.ladderWaitSec = static_cast<double>(vgx::EnvInt("GXV_HRW_GOV5_LADDER_S", 12));
    return p;
}
// gov5: 1 while mode 5 runs on plain 60 on purpose (the governor has not engaged the pairs): the turn rhythm is the plain target's (every turn may draw), not the pairs' 2-of-4. perf.cpp (the skip pattern, the late-turn sleep).
extern "C" int port_hrw_plain60(void) {
    const int m = HrwEff();
    return Gov5(m) && !sHg.engaged && !sHrwChoice.PairsOff() ? 1 : 0;   // (the locked even 30 of GXV_HRW_STAGE / _AUTO keeps its rhythm)
}
extern "C" float port_hrw_clip_pad(void) {
    const int m = HrwEff();
    if (!m || m == 3 || m == 5 || (m == 1 && !sHg.engaged)) return 1.0f;  // (mode 5: the warp cannot show what the first picture did not draw: no wider cull)
    static const float pad = static_cast<float>(vgx::EnvInt("GXV_HRW_CLIP_PAD", 120)) / 100.0f;
    return pad;
}
extern "C" void port_hrw_camera_cut(void) { sHrwCut.fetch_add(1, std::memory_order_relaxed); }
// pace33 PATCH5: the game's draw of this turn's picture is over (GameSystem::portDrawEnd, whichever thread draws): the "fe-lane:" line's border between the game's part and the front end's.
extern "C" void port_vgx_lane_draw_end(void) {
    if (LaneOn()) sLaneDrawEndNs.store(port_host_time_ns(), std::memory_order_relaxed);
}
extern "C" int port_hrw_mode(void) { return HrwEff(); }
// vram24 (GXV_S68_PAIR_LAZY, default 0): seam68 makes the 60 Assist pair targets (~8-12 MiB of video memory) only once the governor has decided to engage (the latch below, set at the decision,
// keyed to the scene it was decided in, so another stage's load starts without). Any thread. 1 always when the switch is off, the governor does not drive mode 5, or HRW is forced.
static std::atomic<uint32_t> sPairKey{0};
static bool PairLazyOn() { static const bool on = vgx::EnvInt("GXV_S68_PAIR_LAZY", 0) != 0; return on; }
extern "C" int port_hrw_pair_wanted(void) {
    if (!PairLazyOn() || !Gov5(HrwEff()) || HrwForceOn()) return 1;
    const uint32_t k = sPairKey.load(std::memory_order_relaxed);
    return k != 0 && k == (port_scene_key ? port_scene_key() : 0u) ? 1 : 0;
}
extern "C" int port_vgx_pair_ready(void) __attribute__((weak));   // (vgx_backend_gl.cpp: the targets exist, or the switch is off)
// mariolag31 (GameScene.cpp portHrwFresh): GXV_HRW_MARIOTIME, and the census of the fresh draw's camera (moved: the camera of the update before the draws differed from the
// pose's turn's; snap: the pose's turn's camera was used). Game thread.
extern "C" int port_hrw_mariotime(void) { return HrwEff() == vgx::hrw::kWarp ? MarioTimeMode() : 0; }
extern "C" void port_hrw_mariotime_cam(int moved, double deg, double units, int snap) {
    if (!MarioTimeCensus()) return;
    port::mariotime::Census& C = sHg.mt;
    ++C.camPairs;
    if (moved) ++C.camMoved;
    if (snap) ++C.camSnap;
    if (deg > C.camDegMax) C.camDegMax = deg;
    if (units > C.camUnitsMax) C.camUnitsMax = units;
}
// pace33: the camera between the last pair's two turns: 1 it stood still (under 0.05 degrees and 2 units), 0 it moved, -1 not known (no pair: a cut, a cover, HRW off). Any thread.
extern "C" int port_hrw_still(void) { return sHrwStill.load(std::memory_order_relaxed); }
extern "C" void port_hrw_turn_begin(void) {
    const int m = HrwEff();
    if (!m && !Even60NoAssistEligible(m)) return;
    sHg.turnT0 = port_host_time_ns();
}
// GXV_HRW_CAPFIX (default 1; 0 = the old behaviour): the game-side half-rate reuses (PETARI_BLOOM_HALFRATE, _SHADOWTEX_HALFRATE, _CLIPCHAIN_HALFRATE, ImageEffectLocalUtil.cpp) count the
// pictures the game DRAWS (portPictureTick, once a drawn turn = the A pictures); a pair's second picture is not one. With pairs "the previous picture" is then 33.3 ms back and the
// reuse alternates 33 / 67 ms old. port_hrw_capfix_pairing() says: a pair was opened by this drawn turn or by the one before it, so nothing made on an earlier picture is to be reused.
// Game thread only. HRW off (HrwEff() == 0) or the key 0: always 0, the old rules run unchanged.
static uint64_t sHrwDrawNo = 0;
static vgx::hrwcapfix::Tracker sHrwCapPair;
extern "C" int port_hrw_capfix_pairing(void) {
    static const bool on = vgx::EnvInt("GXV_HRW_CAPFIX", 1) != 0;
    return on && HrwEff() != 0 && sHrwCapPair.Pairing(sHrwDrawNo, false, true) ? 1 : 0;
}
// GXV_HRW_GOV5_EXACT (gov49, default 1; 0 = the old behaviour; vgx_hrw_capfix.h SolidPlain): with the governor driving mode 5 the Indirect (water) capture's half rate (vgx_draw.cpp, the stage preset's
// indcap_half) may run only while plain 60 is PROVEN solid (GXV_HRW_GOV5_SOLID_S play seconds in a row, default 5, each at GXV_HRW_GOV5_SOLID_FPS or more, default 57, with the pairs off). While the pairs run
// the capture is fresh (CAPFIX); at plain 60 on a borderline spot the half rate showed the water one picture late on every second picture, and the changeover flickered. Now both sides show exact water,
// and a changeover never starts a half-rate phase: the count starts at 0 when the pairs stop. Any thread (the front end asks).
static vgx::hrwcapfix::SolidPlain sGov5Solid;          // game thread
static std::atomic<int32_t> sGov5SolidSecs{0};
static std::atomic<int32_t> sGov5Engaged{0};
extern "C" int port_hrw_indcap_exact(void) {
    static const bool on = vgx::EnvInt("GXV_HRW_GOV5_EXACT", 1) != 0;
    static const int need = static_cast<int>(std::clamp<long>(vgx::EnvInt("GXV_HRW_GOV5_SOLID_S", 5), 0, 120));
    if (!on) return 0;
    if (sEven60PlainNoAssist.load(std::memory_order_relaxed) != 0) return 0;
    vgx::hrwcapfix::SolidPlain sp;
    sp.secs = sGov5SolidSecs.load(std::memory_order_relaxed);
    return sp.Exact(Gov5(HrwEff()), sGov5Engaged.load(std::memory_order_relaxed) != 0, need) ? 1 : 0;
}
// dd24, GXV_DD_DYN=1 (default 0; vgx_directdisplay.h, vgx_backend_gl.cpp): the direct display runs while no pair is held. The game thread mirrors "the governor wants or has the pairs" here (the
// render thread's present hook reads it: port_hrw_dd_hold), and a pair is opened only after the render thread says the direct display has nothing in flight to the screen
// (vgx::backend::DirectDisplayDrained: held, no flip, every buffer given back), so a flipped buffer is never in the display's hands when a pair's first scene begins.
static std::atomic<int32_t> sDdHold{0};
static uint32_t sGovPaused = 0;   // (fix24) governor seconds not ticked while its engage waited
static uint32_t sDdWaits = 0, sDdWaitTurns = 0, sDdForced = 0;   // (game thread; the verify line) pairs delayed for the drain, the turns waited, pairs opened undrained after the longest wait
extern "C" int port_hrw_dd_hold(void) { return sDdHold.load(std::memory_order_relaxed); }
// (fix24) GXV_S68_PAIR_LAZY: while the governor wants the pairs but their targets are still to be made (a wait of up to minutes), no pair is imminent: the direct display is not held off for it.
static bool PairLazyWaiting(const HrwGame& g) { return g.wantEngaged && !g.engaged && PairLazyOn() && port_vgx_pair_ready && port_vgx_pair_ready() == 0; }
static void DdMirror(const HrwGame& g) { sDdHold.store(g.engaged || (g.wantEngaged && !PairLazyWaiting(g)) ? 1 : 0, std::memory_order_relaxed); }
extern "C" void port_hrw_draw_begin(void) {
    const int m = HrwEff();
    if (!m) {
        if (sMpActive || Even60NoAssistEligible(m)) Even60NoAssistSync(port_scene_key ? port_scene_key() : 0u);
        return;
    }
    ++sHrwDrawNo;
    HrwGame& g = sHg;
    g.open = false;
    DdMirror(g);
    if (port_hrw_set_clear) port_hrw_set_clear();  // (hrw28: the last pair's tags go whatever happened to it)
    // hrw28: HRW is a Performance-mode feature: in Quality (a locked even 30) it is off, and logged once per change
    if ((m == 1 || Gov5(m)) && port::stageknob::Quality()) {
        if (!g.qBlocked && port_log_summary) port_log_summary("hrw: stage %08x: disengaged because Quality mode is on (HRW is Performance-only)", port_scene_key ? port_scene_key() : 0u);
        g.qBlocked = true; g.engaged = g.wantEngaged = false;
        return;
    }
    if (g.qBlocked) { g.qBlocked = false; if (port_log_summary) port_log_summary("hrw: Performance mode again: the governor decides"); }
    // (g) a pair's first turn is the only place the governor's verdict takes effect, and never within a camera cut's shadow: nothing is created at the switch, a pair simply starts or does not
    if (g.engaged != g.wantEngaged) {
        const uint32_t cut = sHrwCut.load(std::memory_order_relaxed);
        static uint32_t cutSeen = 0;
        static int cutQuiet = 0;
        if (cut != cutSeen) { cutSeen = cut; cutQuiet = 6; }
        static const bool ddDyn = vgx::backend::DirectDisplayDynOn();
        static int ddWait = 0;
        if (cutQuiet > 0) { --cutQuiet; ++g.deferredSwitch; }
        else if (g.wantEngaged && PairLazyOn() && port_vgx_pair_ready && port_vgx_pair_ready() == 0) {   // (vram24 GXV_S68_PAIR_LAZY: the pair's targets are made behind a closed screen, never in play: Assist starts when they exist)
            ++g.deferredSwitch;
            static int64_t sayAt = 0;
            const int64_t t = port_host_time_ns();
            if (t - sayAt > 30000000000ll && port_log_summary) { sayAt = t; port_log_summary("hrw: vram24: the governor wants 60 Assist, waiting for its pair targets (made at the next closed screen: pause, wipe, load; GXV_S68_PAIR_LAZY)"); }
        }
        else if (ddDyn && g.wantEngaged && !vgx::backend::DirectDisplayDrained() && ddWait < 120) {   // (dd24) the direct display lets go of its buffers first (a few presents)
            if (ddWait++ == 0) ++sDdWaits;
            ++sDdWaitTurns;
        }
        else {
            if (ddDyn && g.wantEngaged && ddWait >= 120) ++sDdForced;
            if (ddDyn && g.wantEngaged && ddWait > 0 && port_log_summary)
                port_log_summary("hrw: dd-dyn: the pair opened after %d turns for the direct display to let go of its buffers (pairs delayed %u, opened undrained after the longest wait %u; governor seconds paused while waiting %u; GXV_DD_DYN)", ddWait, sDdWaits, sDdForced, sGovPaused);
            ddWait = 0;
            g.engaged = g.wantEngaged;
        }
    }
    DdMirror(g);
    if ((sGov5Engaged.load(std::memory_order_relaxed) != 0) != g.engaged) {   // (gov49: a changeover: the water stays exact, the proof of plain 60 starts again)
        sGov5Engaged.store(g.engaged ? 1 : 0, std::memory_order_relaxed);
        sGov5Solid.Reset(); sGov5SolidSecs.store(0, std::memory_order_relaxed);
        if (Gov5(m) && port_log_summary) port_log_summary("hrw: gov5 changeover: 60 Assist %s; the water's captures stay exact (no half rate) until plain 60 has held %ld s in a row (GXV_HRW_GOV5_EXACT)", g.engaged ? "ON" : "OFF", vgx::EnvInt("GXV_HRW_GOV5_SOLID_S", 5));
    }
    g.pairLive = false;
    const bool settled = HrwSettled(g, true);
    {   // pace33 PATCH6 (GXV_HRW_STAGE, a stage row's `reproj`; port/hrw_stage.h): this stage's choice, changed only behind a cover or at a new scene
        static std::atomic<int32_t>& knob = port::stageknob::LiveInt("GXV_HRW_STAGE", 0, false);
        static uint32_t seenKey = 0;
        const uint32_t key = port_scene_key ? port_scene_key() : 0;
        const bool covered = key != seenKey || !settled || (port_screen_quiet && port_screen_quiet() != 0);
        // g30 Wave 8: even60 stays active in multiplayer too (starting at plain 60, clean plain 60 <-> even 30 with assist=false,
        // eliminating the uneven ~48-52 fps motion stutter reported in live MP; GXV_HRW_ALLOW50=1 still opts back into uncapped rates).
        const bool lockNow = port::lockstep::On();
        const bool mpNow = port::hrwmp::InForce(MpPolicyEnabled(), Gov5(m), port::stageknob::Quality(), knob.load(std::memory_order_relaxed));
        static bool sMpReleased = false;   // (mp31: the turn the policy lets go: its choice is put back to what the mode says, at once)
        static bool lockWas = false;
        if (mpNow != sMpActive || (mpNow && (key != seenKey || lockNow != lockWas))) {
            lockWas = lockNow;
            const bool starting = mpNow && !sMpActive;
            sMpActive = mpNow;
            if (mpNow) {
                port::hrwmp::Params mpCfg = MpParams();
                if (lockNow) mpCfg.assist = false;   // MP: clean plain 60 <-> even 30 (zero uneven 50 fps, zero 60 Assist pair thrash)
                sMp.Configure(mpCfg);
                sMp.Begin(static_cast<double>(port_host_time_ns()) / 1e9, port::hrwmp::kPlain60);
                g.secNonPlay = false;
                g.wantEngaged = g.engaged = false;   // (no pair at the start; the governor is told with the next census)
                g.gov.External(static_cast<double>(port_host_time_ns()) / 1e9, false, "the 60 lock first");
                if (port_log_summary) {
                    if (starting) {
                        port_log_summary("even60: no uneven in-between rate: the picture is an even 60 (plain, or 60/30 with 60 Assist) or an even 30; every stage starts at the 60 lock; "
                                         "it falls only on sustained misses (native under %.0f fps in 8 of 10 quiet seconds, two windows in a row) and tries plain 60 again after %d s of 60-fps headroom or %d s at 30 (backed off x2 to %d s, reset on pass; "
                                         "GXV_HRW_EVEN60 %ld, GXV_HRW_ALLOW50 %ld); nothing is judged in opening cameras/cutscenes or before %d s of play and a quiet shader second",
                                         mpCfg.dropBelowFps, mpCfg.holdsFastReturn, mpCfg.trialFirst, mpCfg.trialCap, vgx::EnvInt("GXV_HRW_EVEN60", 1), vgx::EnvInt("GXV_HRW_ALLOW50", 0), mpCfg.settlePlay);
                    }
                    port_log_summary("even60: stage %08x: %s (%s): now %s", key, starting ? "start" : "new scene", lockNow ? "multiplayer" : "single player", port::hrwmp::ModeName(port::hrwmp::kPlain60));
                }
            } else {
                sMpReleased = true;
                if (port_log_summary)
                    port_log_summary("even60: not in force now (%s): the governor decides as before", "this stage's row, Quality mode, 60 Assist off or the switches say so");
            }
        }
        seenKey = key;
        SyncIndcapPlainNoAssist();
        const int was = sHrwChoice.Current();
        // pace33 PATCH9, GXV_HRW_STAGE_AUTO=1 (default; port/hrw_stage.h Auto): a stage whose row says nothing and whose "even:" windows stay bad with the fresh player already
        // off falls back to the locked even 30 for the rest of the stage; remembered for the session; a stage load tries the re-projected 60 again.
        int want = knob.load(std::memory_order_relaxed);
        bool autoCover = false;
        {
            static port::hrwstage::Auto sAuto;
            static const bool autoOn = [] {
                port::hrwstage::AutoConfig c;
                c.on = vgx::EnvInt("GXV_HRW_STAGE_AUTO", 1) != 0;
                c.score = static_cast<int>(vgx::EnvInt("GXV_HRW_STAGE_AUTO_SCORE", 75));
                const long w = vgx::EnvInt("GXV_HRW_STAGE_AUTO_WINDOWS", 2);
                c.windows = static_cast<int>(w < 1 ? 1 : w > 30 ? 30 : w);
                c.retryWindows = static_cast<int>(std::clamp<long>(vgx::EnvInt("GXV_HRW_STAGE_AUTO_RETRY", 0), 0, 360));   // (hrw41: the way back; port/hrw_stage.h)
                sAuto.Configure(c);
                return c.on;
            }();
            static const int64_t openNs = vgx::EnvInt("GXV_HRW_STAGE_AUTO_OPEN_S", 20) * 1000000000ll;
            static int64_t fellAt = 0;
            static uint32_t cutSeenA = 0;
            const uint32_t cutNow = sHrwCut.load(std::memory_order_relaxed);
            const bool eligible = autoOn && m == vgx::hrw::kWarp && want == 0 && !port::stageknob::Quality() && !(Gov5(m) && !g.engaged) && !sMpActive;   // (mp30: in a session the policy owns the choice)   // (gov5: the "even:" windows of a stage on plain 60 are no verdict on the pairs)
            // hrw41, GXV_HRW_STAGE_AUTO_RETRY_MS10 (tenths of a ms, default 300 = 30.0): "room for a pair" while the stage is at the locked 30: the game thread's mean drawn
            // turn + undrawn turn of the last second (the census's costs: the two turns a pair is made of) is under it. 0 = not asked (the windows' scores alone).
            static const double retryMs = static_cast<double>(std::clamp<long>(vgx::EnvInt("GXV_HRW_STAGE_AUTO_RETRY_MS10", 300), 0, 1000)) / 10.0;
            const double pairEst = g.haveCosts ? g.lastCosts.gameDrawnMs + g.lastCosts.gameUndrawnMs : 0.0;
            // Only count actual, uncovered fallback play. GPU and game costs are
            // concurrent budgets, not additive. Unknown timing is not headroom.
            const bool room = was == 1 && settled && !covered && (retryMs <= 0.0 ||
                (g.haveCosts && g.lastCosts.gameDrawnMs > 0 && g.lastCosts.gameUndrawnMs > 0 &&
                 pairEst < retryMs && g.lastCosts.gpuMs > 0 && g.lastCosts.gpuMs < retryMs));
            const port::hrwstage::Auto::Out ao = sAuto.Update(key, eligible, port::hrwstage::EvenWindow().load(std::memory_order_relaxed), !sHrwFreshOn || g.freshAutoOff, room);
            static bool retryCover = false;
            if (ao.newStage || ao.fell) retryCover = false;
            if (ao.retried) {
                retryCover = true;   // (the way back needs no cover: pairs simply begin, as they do after every settle)
                if (port_log_summary)
                    port_log_summary("hrw: stage %08x: auto (GXV_HRW_STAGE_AUTO_RETRY %ld): %d window(s) in a row at the locked even 30 with a score of %ld or more (the last %d) and room for a "
                                     "pair (game thread drawn + undrawn turn %.1f ms, under %.1f): the re-projected 60 is tried again; one bad window falls back (fallbacks in this visit: %d)",
                                     key, vgx::EnvInt("GXV_HRW_STAGE_AUTO_RETRY", 0), ao.wait, vgx::EnvInt("GXV_HRW_STAGE_AUTO_SCORE", 75), ao.score, pairEst, retryMs, ao.tries);
            }
            if (ao.newStage && ao.remembered && eligible && port_log_summary)
                port_log_summary("hrw: stage %08x: auto (GXV_HRW_STAGE_AUTO): this stage fell back to the locked even 30 earlier in the session; the re-projected 60 is tried again, one bad window "
                                 "(score under %ld) falls back", key, vgx::EnvInt("GXV_HRW_STAGE_AUTO_SCORE", 75));
            if (ao.fell) {
                fellAt = port_host_time_ns();
                if (port_log_summary)
                    port_log_summary("hrw: stage %08x: auto (GXV_HRW_STAGE_AUTO): %d window(s) in a row with an 'even:' score under %ld (the last %d) and no fresh player: this stage falls back to "
                                     "the locked even 30 at the next covered moment, camera cut or (after %ld s) still moment; remembered for the session (%u stages); way back (GXV_HRW_STAGE_AUTO_RETRY): %s%d window(s)",
                                     key, ao.bad, vgx::EnvInt("GXV_HRW_STAGE_AUTO_SCORE", 75), ao.score, vgx::EnvInt("GXV_HRW_STAGE_AUTO_OPEN_S", 20), sAuto.Remembered(), ao.wait ? "after " : "none, ", ao.wait);
            }
            sHrwChoiceAuto = sAuto.Fallback() && want == 0;  // (emufix31: the row / GXV_HRW_STAGE said nothing; the fallback decides)
            if (sAuto.Fallback()) {
                want = 1;
                if (was != 1)   // (not yet in force: a camera cut hides the change as well as a cover; after the wait a still camera does)
                    autoCover = cutNow != cutSeenA || (openNs > 0 && port_host_time_ns() - fellAt >= openNs && sHrwStill.load(std::memory_order_relaxed) == 1);
            } else if (retryCover) {
                if (was == 1 && want == 0) autoCover = true;
                else retryCover = false;
            }
            cutSeenA = cutNow;
        }
        if (sMpActive) {   // (mp30) the policy's even 30 is the stage's locked 30; the way back and the step down need no cover (a rhythm change: pictures, not game logic)
            want = sMp.Mode() == port::hrwmp::kEven30 ? 1 : 0;
            if (want != was) autoCover = true;
        } else if (sMpReleased) {   // (mp31) the row's own choice again, without waiting for a cover
            sMpReleased = false;
            if (want != was) autoCover = true;
        }
        if (sHrwChoice.Update(want, covered || autoCover)) {
            if (port_log_summary)
                port_log_summary("hrw: stage %08x: per-stage choice (GXV_HRW_STAGE / the row's reproj): %s (was: %s), taken %s; warp depth mode %ld; %u changes so far",
                                 key, port::hrwstage::Name(sHrwChoice.Current()), port::hrwstage::Name(was), covered ? "behind a covered screen" : sHrwChoice.Current() == 0 ? "in the open (auto: the way back, GXV_HRW_STAGE_AUTO_RETRY)" : "at a camera cut or a still moment (auto)", port::stageknob::Int("GXV_HRW_WARP_DEPTH", vgx::EnvInt("GXV_HRW_WARP_DEPTH", 0)),
                                 sHrwChoice.Changes());
        } else if (sHrwChoice.DeferredNow() && port_log_summary) {
            port_log_summary("hrw: stage %08x: per-stage choice: a new value (%s) arrived while the screen is open: it waits for the next covered screen (in force: %s)", key,
                             port::hrwstage::Name(sHrwChoice.Pending()), port::hrwstage::Name(sHrwChoice.Current()));
        }
    }
    if (!settled) { ++g.noPair; ++g.whyNotSettled; return; }
    if ((m == 1 || m == 5) && sHrwChoice.PairsOff()) { ++g.noPair; ++g.whyStageOff; if (sHrwChoiceAuto) ++g.whyStageAuto; return; }   // (a locked even 30 for this stage: no pair, no snapshot, no second picture)
    if ((m == 1 || Gov5(m)) && !g.engaged) return;
    if (!port_hrw_camera || !port_hrw_camera(g.vA)) { ++g.noPair; ++g.whyNoCamera; return; }
    {   // (warp32) the player's centre through the same view matrix, at the same moment
        float pw[3];
        g.plOk = m == 5 && port_hrw_player && port_hrw_player(pw);
        if (g.plOk)
            for (int r = 0; r < 3; ++r) g.plA[r] = g.vA[r * 4] * pw[0] + g.vA[r * 4 + 1] * pw[1] + g.vA[r * 4 + 2] * pw[2] + g.vA[r * 4 + 3];
        if (g.plOk) for (int r = 0; r < 3; ++r) g.pwA[r] = pw[r];   // (mariolag31: his move between the pair's two turns, for the census)
    }
    g.pairFreshDrawn = false;   // (mariolag31)
    g.pairFreshMs = 0.0;
    g.id = (g.id + 1) & 0xFFFFu;
    if (g.id == 0) g.id = 1;
    g.cutAt = sHrwCut.load(std::memory_order_relaxed);
    g.open = true;
    g.pairLive = true;
    (void)sHrwCapPair.Pairing(sHrwDrawNo, true, true); // (GXV_HRW_CAPFIX: this drawn turn opens a pair)
    ++g.pairs;
    ++g.secPairsN;
    static const bool freezePlain = vgx::EnvInt("GXV_HRW_PAIR_DEPTH", 1) != 0;
    g.pairNoFresh = freezePlain && m == vgx::hrw::kWarp &&
        (!sHrwFreshOn || g.freshAutoOff || sHrwFreshOff.load(std::memory_order_relaxed));
    if (m == 1 || m == 2 || m == 4 || m == 5) port_vgx_marker(vgx::hrwpair::Marker(g.id, g.pairNoFresh));
    if (port_hrw_scene_arm) port_hrw_scene_arm();
    if (port_hrw_set_begin && m != 2 && (m == 3 || sHrwFreshOn)) port_hrw_set_begin(g.id, m == 3 ? 0 : 1);  // (hrw28: the set; mode 3 counts only)
}
static void HrwSendAsk(uint32_t id, uint32_t stamp, const float* d) {
    std::unique_lock<std::mutex> lk(sMtx);
    if (sHrwQueued == id) hrwrt::PushAskLocked(id, stamp, d);
    else { sHrwAsk.valid = true; sHrwAsk.id = id; sHrwAsk.stamp = stamp; std::memcpy(sHrwAsk.d, d, 12 * sizeof(float)); }
    // crash36: this is the game thread's push. One line for each time it would have moved the old queue's storage into the game's current heap (at most 8; the scene's
    // destruction reports the count: port_vgx_scene_heaps_gone).
    const bool c36 = sJc36.game != sJc36.logged && sJc36.logged < 8;
    const unsigned c36n = sJc36.game, c36cap = sJc36.gameCap, c36size = sJc36.gameSize;
    if (c36) sJc36.logged = sJc36.game;
    lk.unlock();
    sCv.notify_all();
    if (c36 && port_log_summary)
        port_log_summary("crash36: the render job queue grew on the game thread (time %u: job %u of the queue, capacity %u): before crash36 this storage came from the scene's heap "
                         "and was gone at the next stage change (the library crash); it is host memory now", c36n, c36size, c36cap);
}
// (coordinator 14 Nov, GXV_LATE_TURN + the fresh player) the undrawn turn's plain ask goes out before GXV_LATE_TURN's sleep (port_late_turn_wait), so the partial
// draw's deadline (the ask's time + GXV_HRW_FRESH_DEADLINE_MS10) ran out during the sleep: console 2931a, Dreadnought, 150 of 150 second pictures went out plain.
// c150 set it again only after the sleep: too late, the render thread had already given up at 9 ms (console 2940d: reset on 59 of 450). Now port_late_turn_wait calls
// this BEFORE it sleeps with the planned sleep (the deadline moves to now + sleep + the budget) and again after it with 0 (now + the budget, for an oversleep); never
// earlier than it was. The render thread's wait re-reads the deadline every <= 2 ms under sMtx. Counted in sHrwDlShift (the "fresh deadline" line).
std::atomic<uint32_t> sHrwDlShift{0};
extern "C" void port_hrw_fresh_deadline_push(long long sleepNs) {
    HrwGame& g = sHg;
    if (!g.freshProv || HrwFreshDeadline10() <= 0) return;
    std::lock_guard<std::mutex> lk(sMtx);
    HrwProv* e = HrwProvFindLocked(g.freshId);
    if (!e || e->state != 1) return;
    const int64_t d = port_host_time_ns() + (sleepNs > 0 ? static_cast<int64_t>(sleepNs) : 0) + HrwFreshDeadline10() * 100000ll;
    if (d > e->deadlineNs) { e->deadlineNs = d; if (sleepNs > 0) sHrwDlShift.fetch_add(1, std::memory_order_relaxed); }
}
extern "C" void port_hrw_undrawn_begin(void) {
    const int m = HrwEff();
    HrwGame& g = sHg;
    sHrwStill.store(-1, std::memory_order_relaxed);   // (pace33: not known until this pair's camera change is)
    if (!m || !g.open) return;
    g.open = false;
    if (m != 4 && !HrwSettled(g, false)) { g.pairLive = false; ++g.cuts; HrwFeelLost(port::hrwfeel::kGame); return; }  // (hrw29: a scene change, pause or wipe between the pair's turns: no second picture)
    if (m == 4 && port_hrw_set_clear) port_hrw_set_clear();  // (verify-fresh: the tags of the drawn turn end with it)
    float vB[12], d[12];
    if (sHrwCut.load(std::memory_order_relaxed) != g.cutAt) { ++g.cuts; HrwFeelLost(port::hrwfeel::kGame); return; }
    if (!port_hrw_camera || !port_hrw_camera(vB) || !vgx::hrw::Delta(g.vA, vB, d)) { ++g.noPair; HrwFeelLost(port::hrwfeel::kGame); return; }
    float rot = 0, tr = 0;
    vgx::hrw::Motion(d, &rot, &tr);
    sHrwStill.store(rot < 0.05f && tr < 2.0f ? 1 : 0, std::memory_order_relaxed);   // (pace33: GXV_DRAIN33 gives a picture up at a still moment when there is one)
    g.sumRot += rot;
    if (rot > g.maxRot) g.maxRot = rot;
    if (tr > g.maxTrans) g.maxTrans = tr;
    ++g.rotBin[rot < 0.1f ? 0 : rot < 0.5f ? 1 : rot < 1.0f ? 2 : rot < 2.0f ? 3 : 4];
    static const bool rotMaxExplicit = [] { const char* e = std::getenv("GXV_HRW_ROT_DEG"); return e && *e; }();
    static const float rotMaxEnv = static_cast<float>(vgx::EnvInt("GXV_HRW_ROT_DEG", 4));
    const float rotMax = (!rotMaxExplicit && port_free_camera_turn_blend && port_free_camera_turn_blend() > 0.05f && rotMaxEnv < 4.6f)
        ? 4.6f : rotMaxEnv;
    static const float trMax = static_cast<float>(vgx::EnvInt("GXV_HRW_TRANS", 600));
    if (rot > rotMax || tr > trMax) { ++g.guarded; HrwFeelLost(port::hrwfeel::kGame); return; }
    if (m == 3) return;
    bool guardPair = false;
    if ((m == 1 || m == 5) && port::pace::GlobalSpeedGuard().Enabled() && port::pace::GlobalSpeedGuard().Owns()) {
        // hrw31 (owner, 13 Nov 12:48: keep game speed, drop a picture): the game-speed guard is repaying lost game time, so the pair's SECOND picture (the cheapest to give up) is not asked for; the guard's extra undrawn turns drop first pictures if that is not enough
        // pace33 PATCH9, GXV_HRW_GUARD_B=1 (default; 0: as hrw31): in mode 5 the second picture is made by the render thread from the first one's snapshot, so giving it up gives the
        // game thread nothing back (console 2892a, the Observatory: 76 of 145 pairs lost it, 48 shown a second, even 62/100, and the guard still needed its extra turns). It is kept;
        // only the fresh player's partial draw (game-thread time) is left out of such a pair. Mode 1 as before.
        static const bool keepB = vgx::EnvInt("GXV_HRW_GUARD_B", 1) != 0;
        if (m != vgx::hrw::kWarp || !keepB) { ++g.guardDrops; HrwFeelLost(port::hrwfeel::kGame); return; }
        ++g.guardKept;
        guardPair = true;
    }
    if (m == vgx::hrw::kWarp && MarioTimeCensus()) {
        // mariolag31 (GXV_HRW_MARIOTIME, port/mario_time.h): will this second picture show Mario's pose of its own turn (the fresh draw), or his old pixels moved by the box?
        // Modes 1 / 2: the latter is not shown: no ask (as over the guard band): the real first picture stays for both retraces. The census counts every case in any mode.
        port::mariotime::PairIn pin;
        pin.freshOn = sHrwFreshOn;
        pin.givenUp = sHrwFreshOff.load(std::memory_order_relaxed) != 0;
        pin.pairNoFresh = g.pairNoFresh;
        pin.guard = guardPair;
        pin.autoOff = g.freshAutoOff;
        const int why = port::mariotime::NoFresh(pin);
        float pw[3];
        const bool havePl = g.plOk && port_hrw_player && port_hrw_player(pw);
        // (in the picture: in front of the second picture's camera, as the back end's box asks: -z > 32)
        const bool inPic = havePl && -(vB[8] * pw[0] + vB[9] * pw[1] + vB[10] * pw[2] + vB[11]) > 32.0f;
        port::mariotime::Census& C = g.mt;
        ++C.pairs;
        if (why == port::mariotime::kFresh) {
            ++C.fresh;
            if (MarioTimeLimitKey() && inPic) { g.mtLimit.Note(false, true); sHrwMtLimited.store(g.mtLimit.stale, std::memory_order_relaxed); }   // (lost49: the way back)
        } else if (!inPic) ++C.noPlayer;
        else if ([&] {   // (lost49: the holds are bounded; a stage that cannot afford the fresh player would hold every second picture)
                     const bool want = port::mariotime::HoldInstead(MarioTimeMode(), why, inPic);
                     if (!MarioTimeLimitKey()) return want;
                     const uint32_t before = g.mtLimit.entered;
                     const bool hold = g.mtLimit.Note(want, why == port::mariotime::kFresh);
                     sHrwMtLimited.store(g.mtLimit.stale, std::memory_order_relaxed);
                     if (g.mtLimit.entered != before && port_log_summary)
                         port_log_summary("hrw: mariotime | lost49: %d of the last 32 pairs were held instead of his old pixels (nearly every pair lacks the fresh draw: the speed guard / the auto rule): "
                                          "no more holds, his old pixels moved by the box are shown instead, until 32 pairs in a row have the fresh draw (GXV_HRW_MARIOTIME_LIMIT 0: hold as before); times so far %u",
                                          __builtin_popcount(g.mtLimit.hist), g.mtLimit.entered);
                     return hold;
                 }()) {
            ++C.held[why];
            if (sHrwFreshOn) ++g.freshAutoWithout;  // (the "fresh auto" line's pairs stay comparable)
            HrwFeelLost(port::hrwfeel::kGame);
            return;
        } else {
            ++C.stale[why];
            double mv = 0.0;
            for (int r = 0; r < 3; ++r) mv += static_cast<double>(pw[r] - g.pwA[r]) * (pw[r] - g.pwA[r]);
            mv = std::sqrt(mv);
            C.staleMoveSum += mv;
            if (mv > C.staleMoveMax) C.staleMoveMax = mv;
        }
    }
    ++g.asked;
    uint32_t stamp = 0;
#if defined(__vita__)
    if (port_vgx_present_sync_mode()) stamp = 0x80000000u | 0x40000000u | (sHrwTurnVblank.load(std::memory_order_relaxed) & 0xFFFFu);   // (pace33: bit 30 = a pair's second picture, for GXV_FIF's count)
#endif
    if (m == 5) {  // (warp32: the player's centre in both pictures' view spaces, for the warp's focus plane and the steadiness figures; before the ask is queued)
        float pw[3], plB[3] = {0.0f, 0.0f, 0.0f};
        const bool ok = g.plOk && port_hrw_player && port_hrw_player(pw);
        if (ok)
            for (int r = 0; r < 3; ++r) plB[r] = vB[r * 4] * pw[0] + vB[r * 4 + 1] * pw[1] + vB[r * 4 + 2] * pw[2] + vB[r * 4 + 3];
        vgx::backend::WarpPlayer(g.id, ok, g.plA, plB);
        if (ok && port_hrw_player36) {  // (warp36: his feet, his up vector and the ground under him through the same view matrix, for the ground-plane warp and the shaped box; ~0.2 us)
            float w[12], v[12];
            const int bits = port_hrw_player36(w);
            if (bits) {
                for (int k = 0; k < 4; ++k)      // (k 0 feet and 2 ground are points; 1 up and 3 normal are directions)
                    for (int r = 0; r < 3; ++r)
                        v[k * 3 + r] = vB[r * 4] * w[k * 3] + vB[r * 4 + 1] * w[k * 3 + 1] + vB[r * 4 + 2] * w[k * 3 + 2] + ((k & 1) ? 0.0f : vB[r * 4 + 3]);
                vgx::backend::WarpPlayer36(g.id, bits, v, v + 3, v + 6, v + 9);
            }
        }
    }
    if (m == vgx::hrw::kWarp && sHrwFreshOn) ++(g.freshAutoOff || guardPair ? g.freshAutoWithout : g.freshAutoWith);  // (warp32 PATCH22)
    if (sHrwFreshOn && !g.pairNoFresh && m != 4 && !guardPair && !sHrwFreshOff.load(std::memory_order_relaxed) && !(m == vgx::hrw::kWarp && g.freshAutoOff)) {
        // hrw27: the ask waits for port_hrw_fresh_draw (GameSystem, right after this): it goes with the partial draw if one is made, plain otherwise
        g.freshPending = true; g.freshId = g.id; g.freshStamp = stamp;
        std::memcpy(g.freshD, d, sizeof d);
        g.freshProv = false;
        if (m == vgx::hrw::kWarp && HrwFreshDeadline10() > 0) {  // (warp32 PATCH23: the plain ask now, with the partial draw's deadline)
            {
                std::lock_guard<std::mutex> lk(sMtx);
                HrwProv& e = sHrwProv[sHrwProvNext++ & 7u];
                e.id = g.id; e.state = 1; e.deadlineNs = port_host_time_ns() + HrwFreshDeadline10() * 100000ll;
            }
            g.freshProv = true;
            HrwSendAsk(g.id, stamp, d);
        }
        return;
    }
    HrwSendAsk(g.id, stamp, d);
}
// warp32 PATCH22, GXV_HRW_FRESH_LATE=0|1 (default 1 since c142; mode 5 with the fresh player): GameSystem::frameLoop's undrawn turn makes only the player's view pass at its start
// (port_hrw_fresh_early: GameScene.cpp port_hrw_fresh_scene_view, no wait for the front end in draw-done mode 4) and calls port_hrw_fresh_draw after its update. The second
// picture's ask then leaves one update (4-6 ms) later than with 0.
extern "C" int port_hrw_fresh_late(void) {
    static const bool late = vgx::EnvInt("GXV_HRW_FRESH_LATE", 1) != 0;  // (default 1 since c142: console c139 Honeyhive fresh on 900/900 pairs at 0.35 ms vs 483/901 at 4.9 ms early)
    return late && sHrwFreshOn && HrwEff() == vgx::hrw::kWarp ? 1 : 0;
}
extern "C" void port_hrw_fresh_early(void) {
    if (sHg.freshPending && port_hrw_fresh_camera_take && MarioTimeCensus()) port_hrw_fresh_camera_take();   // (mariolag31: the camera of the pose's turn, before this turn's update moves it)
    if (sHg.freshPending && port_hrw_fresh_scene_view) port_hrw_fresh_scene_view();
}
// The pair's second picture, asked for with the partial draw: B's fresh Mario (and, with the significance set, the actors that matter), drawn with B's own camera before B's update.
// GXV_HRW_FRESH_BUDGET_MS limits what the game thread may spend on it; the render thread merges it into the replay (vgx_hrw.h Compose). Game thread, the undrawn turn, after port_hrw_undrawn_begin.
extern "C" void port_hrw_fresh_draw(void) {
    const int m = HrwEff();
    HrwGame& g = sHg;
    if (!m) return;
    if (m == 4) {
        // verify-fresh: on the drawn turn, the partial draw is made too (with the picture's own camera and state), then the full picture follows; the render side compares them
        // (called by GameScene::draw, first thing, in the picture the pair's marker opened: g.id)
        if (!g.open || !sHrwFreshOn || !port_hrw_fresh_scene) return;
        const uint32_t nid = g.id;
        const int64_t t0 = port_host_time_ns();
        { std::lock_guard<std::mutex> lk(sMtx); sHrwFreshAsk.valid = false; }
        const int n = port_hrw_fresh_scene(nid, port::getenv25::HrwPointer());
        if (n && port_hrw_fresh_finish) port_hrw_fresh_finish();
        const double ms = (port_host_time_ns() - t0) / 1e6;
        g.freshMs += ms; if (ms > g.freshMaxMs) g.freshMaxMs = ms;
        if (n) ++g.freshDraws; else ++g.freshEmpty;
        return;
    }
    if (!g.freshPending) return;
    g.freshPending = false;
    int n = 0;
    const int64_t t0 = port_host_time_ns();
    if (port_hrw_fresh_scene) {
        {
            std::lock_guard<std::mutex> lk(sMtx);
            sHrwFreshAsk.valid = true; sHrwFreshAsk.id = g.freshId; sHrwFreshAsk.stamp = g.freshStamp;
            std::memcpy(sHrwFreshAsk.d, g.freshD, sizeof sHrwFreshAsk.d);
        }
        n = port_hrw_fresh_scene(g.freshId, port::getenv25::HrwPointer());
        if (n && port_hrw_fresh_finish) port_hrw_fresh_finish();
    }
    const double ms = (port_host_time_ns() - t0) / 1e6;
    g.freshMs += ms; if (ms > g.freshMaxMs) g.freshMaxMs = ms;
    if (n) ++g.freshDraws; else ++g.freshEmpty;
    g.pairFreshDrawn = n != 0;   // (mariolag31: the cost mode of the auto rule reads what this pair's fresh draw cost)
    g.pairFreshMs = ms;
    if (!n && MarioTimeCensus()) ++g.mt.empty;
    if (!n) {
        std::lock_guard<std::mutex> lk(sMtx);
        sHrwFreshAsk.valid = false;
    }
    if (!n && g.freshProv) {  // (warp32 PATCH23: the plain ask is out already; the render thread need not wait for a partial draw)
        { std::lock_guard<std::mutex> lk(sMtx); if (HrwProv* e = HrwProvFindLocked(g.freshId)) { if (e->state == 1) e->state = 2; } }
        sCv.notify_all();
    } else
    if (!n) HrwSendAsk(g.freshId, g.freshStamp, g.freshD);
    g.freshProv = false;
    // (n != 0: the ask went on the partial draw's job when the front end handed it over; if that never happens (the stream had no draw) the pair has no second picture, counted as a missing ask by the render side)
}
// hrw28: the Performance governors' budgets while HRW is wanted (platform/src/port/perf.cpp FeGovConfig / GameGovConfig): 1 and the PAIR's budgets (front end per parsed picture, game thread per drawn turn) when HRW is pending,
// engaged, or declined only because the pair does not fit; 0 (their ordinary 16.7 ms budgets) otherwise: mode 0 / 2 / 3 / 4, Quality, or "the stage holds 60 natively".
extern "C" int port_hrw_pair_budgets(float* feMs, float* gameMs) {
    const int m = HrwEff();
    HrwGame& g = sHg;
    if (m != 1 || g.qBlocked || !g.haveCosts || port::stageknob::Quality()) return 0;
    const int st = g.gov.Status();
    if (st != vgx::hrw::kGovPending && st != vgx::hrw::kGovEngaged && st != vgx::hrw::kGovNoFit) return 0;
    vgx::hrw::Costs c = g.lastCosts;
    c.partialMs = 3.0;  // (Mario + the set's budget 2.5 ms: the partial draw is paid in the undrawn turn, so it is only subtracted from the drawn turn's budget as a safe side)
    const vgx::hrw::PairBudgetsOut b = vgx::hrw::PairBudgets(c);
    if (feMs) *feMs = static_cast<float>(b.feMs);
    if (gameMs) *gameMs = static_cast<float>(b.gameDrawnMs);
    return 1;
}
extern "C" void port_hrw_turn_end(int drawn) {
    const int m = HrwEff();
    if (!m) {
        if (!sMpActive && !Even60NoAssistEligible(m)) return;
        const uint32_t key = port_scene_key ? port_scene_key() : 0u;
        Even60NoAssistSync(key);
        HrwGame& g = sHg;
        const int64_t now = port_host_time_ns();
        const double ms = g.turnT0 ? (now - g.turnT0) / 1e6 : 0.0;
        g.secDrawn += drawn ? ms : 0; g.secDrawnN += drawn ? 1 : 0;
        g.secUndrawn += drawn ? 0 : ms; g.secUndrawnN += drawn ? 0 : 1;
        if (sMpActive && !Even60Play()) g.secNonPlay = true;
        if (!g.secT0) g.secT0 = now;
        if (now - g.secT0 >= 1000000000ll) {
            const double secs = (now - g.secT0) / 1e9;
            const double nativeFps = (g.secDrawnN + g.secUndrawnN) ? g.secDrawnN / secs : 0.0;
            const double gameDrawnMs = g.secDrawnN ? g.secDrawn / g.secDrawnN : 0.0;
            double feMs = 0.0, gpuMs = 0.0, pipeMs = 0.0;
            uint32_t cpu = 0, pic = 0, gpuUs = 0, pipeUs = 0;
            int low = 0;
            if (port_autoq_front_totals && port_autoq_front_totals(&cpu, &pic) && pic > g.fePic0 && g.fePic0) feMs = (cpu - g.feCpu0) / 1000.0 / (pic - g.fePic0);
            g.feCpu0 = cpu; g.fePic0 = pic;
            if (port_autoq_stages && port_autoq_stages(&gpuUs, &pipeUs, &low)) { gpuMs = gpuUs / 1000.0; pipeMs = pipeUs / 1000.0; }
            const bool secNonPlay = g.secNonPlay;
            g.secNonPlay = false;
            g.secT0 = now; g.secDrawn = g.secUndrawn = 0; g.secDrawnN = g.secUndrawnN = 0;
            if (sMpActive) {
                port::hrwmp::Sec ps;
                ps.play = Even60Play() && !secNonPlay;
                ps.nativeFps = nativeFps;
                ps.holdsVerdict = (sMp.Mode() == port::hrwmp::kEven30) && port::hrwmp::PlainCostsHold60(gameDrawnMs, feMs, gpuMs, pipeMs);
                static uint32_t lastC = 0, lastL = 0;
                static double lastShaderMs = 0.0;
                const vgx::backend::Stats bs = vgx::backend::GetStats();
                ps.quiet = port::hrwmp::ShaderQuiet(bs.shaderCompiles, lastC, bs.programLinks, lastL, bs.shaderWorkMs, lastShaderMs);
                lastC = bs.shaderCompiles; lastL = bs.programLinks; lastShaderMs = bs.shaderWorkMs;
                const int before = sMp.Mode();
                const port::hrwmp::Out mo = sMp.Tick(now / 1e9, ps);
                sEven60NoAssist30.store(sMp.Mode() == port::hrwmp::kEven30 ? 1 : 0, std::memory_order_relaxed);
                SyncIndcapPlainNoAssist();
                if (mo.trial == 1 && port_log_summary)
                    port_log_summary("even60 (no-assist): stage %08x: trial plain 60 starts after %d s at even 30 (%s; native %.1f fps)", key, mo.nextTrialSec, mo.why, nativeFps);
                else if (mo.trial == 2 && port_log_summary)
                    port_log_summary("even60 (no-assist): stage %08x: trial plain 60 kept (native %.1f fps)", key, nativeFps);
                else if (mo.trial == 3 && port_log_summary)
                    port_log_summary("even60 (no-assist): stage %08x: trial plain 60 back to 30 (native %.1f fps), next trial in %d s", key, nativeFps, mo.nextTrialSec);
                else if (mo.changed && port_log_summary)
                    port_log_summary("even60 (no-assist): stage %08x: %s -> %s because %s (native %.1f fps)", key, port::hrwmp::ModeName(before), port::hrwmp::ModeName(mo.mode), mo.why, nativeFps);
            }
        }
        return;
    }
    HrwGame& g = sHg;
    const int64_t now = port_host_time_ns();
    const double ms = g.turnT0 ? (now - g.turnT0) / 1e6 : 0.0;
    g.secDrawn += drawn ? ms : 0; g.secDrawnN += drawn ? 1 : 0;
    g.secUndrawn += drawn ? 0 : ms; g.secUndrawnN += drawn ? 0 : 1;
    if (sMpActive && !Even60Play()) g.secNonPlay = true;
    if (drawn) g.drawnMs = ms;
    else if (g.drawnMs > 0 && g.pairLive) {  // (hrw29: only a pair that was opened counts: the sums and the denominator are the same pairs)
        g.pairLive = false;
        ++g.pairsDone;
        const double pair = g.drawnMs + ms;
        g.sumDrawn += g.drawnMs; g.sumUndrawn += ms; g.sumPair += pair;
        if (pair > g.maxPair) g.maxPair = pair;
        if (pair <= 1000.0 / 30.0 * 0.97) ++g.fit;
        g.drawnMs = 0;
        if (m == vgx::hrw::kWarp && sHrwFreshOn) {  // (warp32 PATCH22)
            // GXV_HRW_FRESH_AUTO=1|0 (default 1): the fresh player costs the undrawn turn 0.7-3 ms (c135 on the console); where the pairs already fill their 33.3 ms (Beach Bowl:
            // 54.1 fps / even 89 with it, 57.7 / 100 without) it is left out. OFF when 3 of the last 16 pairs took more than GXV_HRW_FRESH_AUTO_OFF_MS10 (tenths of a ms, default 310 = 31.0) of game-thread
            // work; ON again after GXV_HRW_FRESH_AUTO_PAIRS (default 90 = 3 s) pairs in a row under GXV_HRW_FRESH_AUTO_ON_MS10 (default 270 = 27.0: room for what it costs). The switch
            // is read when a pair's ask is made, so a pair has the fresh player or the locked box, never half of each.
            static const bool autoOn = vgx::EnvInt("GXV_HRW_FRESH_AUTO", 1) != 0;
            static const double offMs = static_cast<double>(std::clamp<long>(vgx::EnvInt("GXV_HRW_FRESH_AUTO_OFF_MS10", 310), 100, 1000)) / 10.0;
            static const double onMs = static_cast<double>(std::clamp<long>(vgx::EnvInt("GXV_HRW_FRESH_AUTO_ON_MS10", 270), 100, 1000)) / 10.0;
            static const uint32_t calmPairs = static_cast<uint32_t>(std::clamp<long>(vgx::EnvInt("GXV_HRW_FRESH_AUTO_PAIRS", 90), 1, 100000));
            if (autoOn && MarioTimeMode() == port::mariotime::kHoldCost) {
                // mariolag31 (GXV_HRW_MARIOTIME=2, port/mario_time.h FreshAuto): a pair counts against the fresh player only when its draw is what takes (or would take) it over the limit
                static port::mariotime::FreshAuto sCost;
                static const bool cfg = [&] {
                    port::mariotime::AutoConfig c;
                    c.offMs = offMs; c.onMs = onMs; c.calmPairs = calmPairs; c.cost = true;
                    sCost.Configure(c);
                    return true;
                }();
                (void)cfg;
                // (the pair's WORK: GXV_LATE_TURN's sleep inside the undrawn turn is idle time it planned from the room the pair had, not a cost; the old rule counts it. Console
                //  r2-601 K0a, Honeyhive, while the old rule had the fresh player off: every undrawn turn slept 2.4-3.9 ms on purpose and still had 4.0-5.7 ms idle before its retrace)
                const double slept = port_late_turn_slept_us ? port_late_turn_slept_us() / 1000.0 : 0.0;
                const double work = g.drawnMs + std::max(0.0, ms - slept);
                sCost.Pair(work, g.pairFreshDrawn, g.pairFreshMs);
                if (sCost.Off() != g.freshAutoOff) {
                    g.freshAutoOff = sCost.Off();
                    ++(g.freshAutoOff ? g.freshAutoOffN : g.freshAutoOnN);
                    ++(g.freshAutoOff ? g.mt.autoOffN : g.mt.autoOnN);
                }
                g.mt.est = sCost.Est();
            } else
            if (autoOn) {
                g.freshAutoHist = ((g.freshAutoHist << 1) | (pair > offMs ? 1u : 0u)) & 0xFFFFu;
                if (!g.freshAutoOff) {
                    if (__builtin_popcount(g.freshAutoHist) >= 3) { g.freshAutoOff = true; g.freshAutoCalm = 0; ++g.freshAutoOffN; ++g.mt.autoOffN; }
                } else {
                    g.freshAutoCalm = pair < onMs ? g.freshAutoCalm + 1 : 0;
                    if (g.freshAutoCalm >= calmPairs) { g.freshAutoOff = false; g.freshAutoHist = 0; ++g.freshAutoOnN; ++g.mt.autoOnN; }
                }
            }
        }
        g.pairFreshDrawn = false;   // (mariolag31: read once, by this pair)
        g.pairFreshMs = 0.0;
    }
    if (!g.secT0) { g.secT0 = now; g.logT0 = now; }
    // once a second: the governor (mode 1) and its costs
    if (now - g.secT0 >= 1000000000ll) {
        const double secs = (now - g.secT0) / 1e9;
        vgx::hrw::GovSample sm;
        sm.play = HrwPlay();
        sm.nativeFps = (g.secDrawnN + g.secUndrawnN) ? g.secDrawnN / secs : 0;
        sm.costs.gameDrawnMs = g.secDrawnN ? g.secDrawn / g.secDrawnN : 0;
        sm.costs.gameUndrawnMs = g.secUndrawnN ? g.secUndrawn / g.secUndrawnN : 0;
        uint32_t cpu = 0, pic = 0, gpuUs = 0, pipeUs = 0;
        int low = 0;
        if (port_autoq_front_totals && port_autoq_front_totals(&cpu, &pic) && pic > g.fePic0 && g.fePic0) sm.costs.feMs = (cpu - g.feCpu0) / 1000.0 / (pic - g.fePic0);
        g.feCpu0 = cpu; g.fePic0 = pic;
        double pipeMs = 0.0;
        if (port_autoq_stages && port_autoq_stages(&gpuUs, &pipeUs, &low)) { sm.costs.gpuMs = gpuUs / 1000.0; pipeMs = pipeUs / 1000.0; }
        sm.costs.beMs = 7.5;  // (not measured here: the census prints the replay's own wall time from the back end when it runs)
        const uint64_t dropped = [] { uint64_t t = 0; for (int i = 2; i < 10; ++i) t += sHrwRt[i].load(std::memory_order_relaxed); return t; }();
        static uint64_t droppedPrev = 0, askedPrev = 0;
        sm.missPct = g.asked > askedPrev ? 100.0 * (dropped - droppedPrev) / (g.asked - askedPrev) : 0.0;
        droppedPrev = dropped; askedPrev = g.asked;
        {   // (gov77) the pairs' own second: the share of second pictures shown, and the GPU time of a whole pair (the picture mean x pictures per pair)
            static uint64_t runPrev = 0;
            const uint64_t run = sHrwRt[1].load(std::memory_order_relaxed);
            const uint32_t pairsSec = g.secPairsN;
            g.secPairsN = 0;
            sm.costs.bShown = -1;
            sm.costs.pairGpuMs = 0;
            if (g.engaged && pairsSec >= 10 && run >= runPrev) {
                sm.costs.bShown = std::min(1.0, static_cast<double>(run - runPrev) / pairsSec);
                if (sm.costs.gpuMs > 0) sm.costs.pairGpuMs = sm.costs.gpuMs * (1.0 + sm.costs.bShown);
            }
            runPrev = run;
        }
        g.lastCosts = sm.costs; g.haveCosts = true;
        const bool secNonPlay = g.secNonPlay;
        g.secNonPlay = false;
        g.secT0 = now; g.secDrawn = g.secUndrawn = 0; g.secDrawnN = g.secUndrawnN = 0;
        const uint32_t key = port_scene_key ? port_scene_key() : 0;
        if (key != g.govKey) { g.govKey = key; g.gov.StageBegin(now / 1e9); g.engaged = g.wantEngaged = (m != 1 && !Gov5(m)) || HrwForceOn() ? true : false; if (Gov5(m)) { g.gov.Configure(Gov5Params()); g.lastStatus = -1;
                static bool fixLogged = false;
                if (!fixLogged && port_log_summary) { fixLogged = true; if (vgx::EnvInt("GXV_GOV77_FIX", 0) != 0 || vgx::EnvInt("GXV_GOV77_PAIRPAD", 0) != 0) port_log_summary("hrw: gov77 fix: GXV_GOV77_FIX=%ld (the GPU-limited verdict needs a measured GPU time of at least %.1f ms and a measured game thread; neither over budget is its own verdict), GXV_GOV77_PAIRPAD=%ld (the engage test adds %.1f ms to the pair's game thread time)", vgx::EnvInt("GXV_GOV77_FIX", 0), static_cast<double>(vgx::EnvInt("GXV_GOV77_GPU_MS10", 160)) / 10.0, vgx::EnvInt("GXV_GOV77_PAIRPAD", 0), vgx::EnvInt("GXV_GOV77_PAIRPAD", 0) != 0 ? static_cast<double>(vgx::EnvInt("GXV_GOV77_PAIRPAD_MS10", 21)) / 10.0 : 0.0); } } }
        // (the governor only decides in mode 1; its first look needs the native rate, so a stage starts disengaged)
        if (Gov5(m)) {   // (the pair's second picture is a warp: its cost, and what the first picture would cost without the pairs, so that the governor can judge both ways)
            sm.costs.warpMs = Gov5WarpMs();
            if (sm.costs.gameUndrawnMs <= 0 && sm.costs.gameDrawnMs > 0) sm.costs.gameUndrawnMs = std::clamp(0.4 * sm.costs.gameDrawnMs, 4.0, 9.0);   // (plain 60 has no undrawn turn: Honeyhive 8.7 of 21.6)
            sm.ladderRoom = port_autoq_ladder_room && port_autoq_ladder_room() != 0;
            sm.engaged = g.engaged;
        }
        {   // (gov49) the proof of plain 60 for the water's half rate: once a second
            const bool before = port_hrw_indcap_exact() != 0;
            static const bool hitch = vgx::EnvInt("GXV_HRW_GOV5_HITCH", 0) != 0;   // (dd24: a hitch second does not take the proof of plain 60 back)
            static const int hitchNeed = static_cast<int>(std::clamp<long>(vgx::EnvInt("GXV_HRW_GOV5_SOLID_S", 5), 0, 120));
            static uint32_t dipsHeld = 0;
            if (hitch) {
                if (sGov5Solid.SecondHold(Gov5(m), g.engaged, sm.play && !g.qBlocked, sm.nativeFps, static_cast<double>(vgx::EnvInt("GXV_HRW_GOV5_SOLID_FPS", 57)), hitchNeed)) {
                    ++dipsHeld;
                    if (dipsHeld <= 8 && port_log_summary)
                        port_log_summary("hrw: gov5 water: a dip to %.1f native fps with the pairs off is ridden out, the captures stay at half rate (proof of plain 60 kept; %u dips so far; GXV_HRW_GOV5_HITCH)", sm.nativeFps, dipsHeld);
                }
            } else
            sGov5Solid.Second(Gov5(m), g.engaged, sm.play && !g.qBlocked, sm.nativeFps, static_cast<double>(vgx::EnvInt("GXV_HRW_GOV5_SOLID_FPS", 57)));
            sGov5SolidSecs.store(sGov5Solid.secs, std::memory_order_relaxed);
            const bool after = port_hrw_indcap_exact() != 0;
            if (Gov5(m) && !g.engaged && before != after && port_log_summary)
                port_log_summary("hrw: gov5 water: captures %s (plain 60 held %d s in a row, native %.1f fps)", after ? "exact (the half rate is off)" : "at half rate again (plain 60 is proven)", sGov5Solid.secs, sm.nativeFps);
        }
        DdMirror(g);
        if ((m == 1 || Gov5(m)) && !g.qBlocked && !HrwForceOn()) {
            // (fix24) While the engage it asked for is still deferred (the lazy pair targets, the drain, a camera cut's shadow) the governor is not ticked: its window would fill with samples taken without the pairs,
            // and its leave rules and maxChanges budget (Change counts every engage / leave) could be spent on a state the game never reached. It goes on when the engage has happened.
            // Only with GXV_S68_PAIR_LAZY or GXV_DD_DYN (the long waits); with both off the c51 behaviour is unchanged.
            static const bool govPause = PairLazyOn() || vgx::backend::DirectDisplayDynOn();
            const bool govDeferred = govPause && g.wantEngaged && !g.engaged;
            if (govDeferred) ++sGovPaused;
            port::hrwmp::Sec ps;
            if (sMpActive) {   // (mp30) compute play & shader-quiet before governor tick so opening cameras / shader loads never trip g.gov either
                ps.play = Even60Play() && !secNonPlay && !g.qBlocked;
                ps.nativeFps = sm.nativeFps;
                static uint32_t lastC = 0, lastL = 0;
                static double lastShaderMs = 0.0;
                const vgx::backend::Stats bs = vgx::backend::GetStats();
                ps.quiet = port::hrwmp::ShaderQuiet(bs.shaderCompiles, lastC, bs.programLinks, lastL, bs.shaderWorkMs, lastShaderMs);
                lastC = bs.shaderCompiles; lastL = bs.programLinks; lastShaderMs = bs.shaderWorkMs;
            }
            const bool mpEven = sMpActive && sMp.Mode() == port::hrwmp::kEven30;   // (mp30: at the even 30 the policy decides, from the costs measured there; the governor's window would read a native 30)
            vgx::hrw::GovSample govSm = sm;
            if (sMpActive) {
                govSm.play = ps.play && ps.quiet && !sMp.Trialing() && (sMp.Mode() == port::hrwmp::kAssist || sMp.Settled());
            }
            const vgx::hrw::GovAction a = govDeferred || mpEven ? vgx::hrw::kGovNone : g.gov.Tick(now / 1e9, govSm);
            if (!sMpActive) {
                if (a == vgx::hrw::kGovEngage) { g.wantEngaged = true; sPairKey.store(key, std::memory_order_relaxed); }
                else if (a == vgx::hrw::kGovLeave) g.wantEngaged = false;
            }
            if (sMpActive) {   // (mp30) one decision a second, from the same figures; sMp solely owns g.wantEngaged while active
                static const vgx::hrw::FitParams fit = Gov5Params().fit;
                vgx::hrw::Costs pc = sm.costs;
                if (g.gov.LearnedWarpMs() > 0) pc.warpMs = g.gov.LearnedWarpMs();
                ps.fits = pc.gameDrawnMs > 0 && pc.gameUndrawnMs > 0 && pc.gpuMs > 0 && vgx::hrw::PairFit(pc, fit).All();   // (an unknown figure is never room)
                // At kEven30 g.gov.Tick() is paused so VerdictCode() is stale; use measured single-picture costs (every drawn picture is full-cost at kEven30), including pipeMs (backend/front/gpu bottleneck).
                ps.holdsVerdict = mpEven ? port::hrwmp::PlainCostsHold60(sm.costs.gameDrawnMs, sm.costs.feMs, sm.costs.gpuMs, pipeMs)
                                         : (g.gov.VerdictCode() == vgx::hrw::Governor::kVHolds);
                ps.blocked = g.gov.Blocked();
                ps.ev = a == vgx::hrw::kGovEngage ? port::hrwmp::kGovEngaged
                      : a == vgx::hrw::kGovLeave ? (g.gov.LeftBecausePlainHolds() ? port::hrwmp::kGovLeftPlainHolds : port::hrwmp::kGovLeftOther) : port::hrwmp::kGovNothing;
                const int before = sMp.Mode();
                const port::hrwmp::Out mo = sMp.Tick(now / 1e9, ps);
                SyncIndcapPlainNoAssist();
                if (mo.trial == 1 && port_log_summary)
                    port_log_summary("even60: stage %08x: trial plain 60 starts after %d s at an even 30 (%s; native %.1f fps there; the first second is ignored, then 3 of 4 seconds at %.0f or more keep it)",
                                     key, mo.nextTrialSec, mo.why, sm.nativeFps, MpParams().trialFps);
                else if (mo.trial == 2 && port_log_summary)
                    port_log_summary("even60: stage %08x: trial plain 60 kept (native %.1f fps; game thread drawn %.1f ms, front end %.1f, GPU %.1f ms a picture)", key, sm.nativeFps, sm.costs.gameDrawnMs, sm.costs.feMs, sm.costs.gpuMs);
                else if (mo.trial == 3 && port_log_summary)
                    port_log_summary("even60: stage %08x: trial plain 60 back to 30 (native %.1f fps), next trial in %d s", key, sm.nativeFps, mo.nextTrialSec);
                if (mo.unblock) {
                    g.gov.Unblock();
                    if (port_log_summary) port_log_summary("even60: stage %08x: %s", key, mo.why);
                }
                if (mo.changed) {
                    if (!mo.byGovernor) {   // (the policy's own: the governor is told, so that its engaged flag, window and gap agree with what is drawn)
                        g.gov.External(now / 1e9, mo.mode == port::hrwmp::kAssist, mo.why);
                    }
                    g.wantEngaged = mo.mode == port::hrwmp::kAssist;
                    if (g.wantEngaged) sPairKey.store(key, std::memory_order_relaxed);
                    DdMirror(g);
                    if (port_log_summary && mo.trial == 0)
                        port_log_summary("even60: stage %08x: %s -> %s because %s (native %.1f fps, game thread drawn %.1f ms + undrawn %.1f, front end %.1f, GPU %.1f ms a picture; pair %s)", key, port::hrwmp::ModeName(before),
                                         port::hrwmp::ModeName(mo.mode), mo.why, sm.nativeFps, sm.costs.gameDrawnMs, sm.costs.gameUndrawnMs, sm.costs.feMs, sm.costs.gpuMs, ps.fits ? "fits" : "does not fit");
                } else if (a != vgx::hrw::kGovNone) {
                    // Defensive sync: if the policy ignored a governor event (e.g. !ps.play / unsettled / trialing), keep governor and wantEngaged aligned with sMp.Mode().
                    g.gov.External(now / 1e9, sMp.Mode() == port::hrwmp::kAssist, "even60 keeps current mode");
                    g.wantEngaged = sMp.Mode() == port::hrwmp::kAssist;
                }
            }
            DdMirror(g);   // (dd24)
            // hrw28: one decision line per change: "hrw: stage X: engaged because ..." / "disengaged because ..."
            if (a != vgx::hrw::kGovNone && port_log_summary) port_log_summary("hrw: stage %08x: %s because %s", key, a == vgx::hrw::kGovEngage ? "engaged" : "disengaged", g.gov.Why());
            {   // (gov77) one line for every reason the governor stays where it is, at each change of the reason (and every 30 s while it stands): the decisions themselves are the engaged / disengaged lines above
                static int lastVerdict = -1;
                static int64_t lastVerdictAt = 0;
                const int vc = g.gov.VerdictCode();
                if (g.lastStatus == -1) lastVerdict = -1;   // (a stage begin)
                if (vc != vgx::hrw::Governor::kVNone && port_log_summary && (vc != lastVerdict || (now - lastVerdictAt > 30000000000ll && vc != vgx::hrw::Governor::kVAssistOn))) {
                    port_log_summary("hrw: gov77 stage %08x: %s (native %.1f fps, game thread drawn %.1f ms, front end %.1f ms, GPU %.1f ms a picture%s)", key, g.gov.VerdictWhy(), sm.nativeFps, sm.costs.gameDrawnMs, sm.costs.feMs, sm.costs.gpuMs,
                                     g.gov.LearnedWarpMs() > 0 ? ", second picture measured" : "");
                    lastVerdict = vc; lastVerdictAt = now;
                }
            }
            const int st = static_cast<int>(g.gov.Status());
            if (st != g.lastStatus) {
                g.lastStatus = st;
                if (port_log_summary && (st == vgx::hrw::kGovNotNeeded || st == vgx::hrw::kGovNoFit))
                    port_log_summary("hrw: stage %08x: not engaged because %s (native %.1f fps; pair limited by %s: game %.1f fe %.1f gpu %.1f ms)", key, st == vgx::hrw::kGovNotNeeded ? "the stage holds 60 natively" : "the pair does not fit in 33.3 ms", sm.nativeFps, g.gov.LastFit().Limit(), g.gov.LastFit().game, g.gov.LastFit().fe, g.gov.LastFit().gpu);
            }
        }
    }
    if (now - g.logT0 >= 5000000000ll && port_log_summary) {
        const double n = g.pairsDone ? g.pairsDone : 1;
        const uint32_t cnt = g.rotBin[0] + g.rotBin[1] + g.rotBin[2] + g.rotBin[3] + g.rotBin[4];
        port_log_summary("hrw: mode %d%s | pairs %u (no pair: %u, camera cuts %u, over the guard band %u, asked %u) | game thread per pair: drawn %.1f + undrawn %.1f = %.1f ms (max %.1f), %u of %u pairs under 32.3 ms | "
                         "camera change per pair: mean %.2f deg, max %.2f deg, max %.0f units, bins <0.1/<0.5/<1/<2/>=2 deg: %u/%u/%u/%u/%u of %u | "
                         "render: asks queued %llu, replays run %llu, dropped %llu (queue %llu, no buffer %llu, id %llu, hazard %llu, projection %llu, not presentable %llu), kept %llu (unusable %llu), verify runs %llu bad %llu",
                         m, m == 1 ? (g.engaged ? " ENGAGED" : " idle") : "", g.pairs, g.noPair, g.cuts, g.guarded, g.asked, g.sumDrawn / n, g.sumUndrawn / n, g.sumPair / n, g.maxPair, g.fit, g.pairsDone,
                         cnt ? g.sumRot / cnt : 0.0, g.maxRot, g.maxTrans, g.rotBin[0], g.rotBin[1], g.rotBin[2], g.rotBin[3], g.rotBin[4], cnt,
                         (unsigned long long)sHrwRt[0].load(), (unsigned long long)sHrwRt[1].load(),
                         (unsigned long long)(sHrwRt[2].load() + sHrwRt[3].load() + sHrwRt[4].load() + sHrwRt[5].load() + sHrwRt[6].load() + sHrwRt[7].load() + sHrwRt[8].load() + sHrwRt[9].load()),
                         (unsigned long long)sHrwRt[2].load(), (unsigned long long)sHrwRt[3].load(), (unsigned long long)sHrwRt[4].load(), (unsigned long long)sHrwRt[5].load(),
                         (unsigned long long)sHrwRt[6].load(), (unsigned long long)sHrwRt[7].load(), (unsigned long long)sHrwRt[12].load(), (unsigned long long)sHrwRt[13].load(),
                         (unsigned long long)sHrwRt[10].load(), (unsigned long long)sHrwRt[11].load());
        if (m == 5) {
            // hrw31: the warp census (render side counts, since the last line): second pictures asked / made by the warp, the others and why, the first pictures' snapshots, how much of the screen's edge the camera change exposes
            vgx::backend::WarpStats w;
            vgx::backend::WarpTakeStats(w, true);
            {   // the self-proving line (hrw31): once 120 second pictures were made by the warp, then every 600
                static uint64_t cumWarped = 0, cumFell = 0, nextProof = 120;
                static double cumEdgeMax = 0;
                cumWarped += w.warped;
                for (int r = 1; r < 16; ++r) cumFell += w.fell[r];
                if (w.edgeMax > cumEdgeMax) cumEdgeMax = w.edgeMax;
                if (cumWarped >= nextProof) {
                    port_log_summary("hrw: mode 5 PROOF: warped %llu, fallbacks %llu, worst edge error %.1f px", (unsigned long long)cumWarped, (unsigned long long)cumFell, cumEdgeMax);
                    nextProof = cumWarped + 600;
                }
            }
            char fb[256];
            int o = 0;
            fb[0] = 0;
            uint64_t fell = 0;
            for (int r = 1; r < 16; ++r)
                if (w.fell[r]) { fell += w.fell[r]; if (o < 200) o += snprintf(fb + o, sizeof fb - o, "%s%s %llu", o ? ", " : "", vgx::backend::WarpReasonName(r), (unsigned long long)w.fell[r]); }
            port_log_summary("hrw: warp | pairs %u (cuts %u, over the guard band %u, no pair %u, second pictures given up to the speed guard %u), asked %llu, warped B %llu, fell back %llu by reason %s | snapshots %llu (with depth %llu, depth mode %d, %dx%d) %.2f ms mean | "
                             "warp pass %.2f ms mean (CPU side) | edge pixels exposed (rotation only, at infinity) mean %.1f / max %.1f px | dumps %llu",
                             g.pairs, g.cuts, g.guarded, g.noPair, g.guardDrops, (unsigned long long)w.asked, (unsigned long long)w.warped, (unsigned long long)fell, o ? fb : "none",
                             (unsigned long long)w.snaps, (unsigned long long)w.snapsDepth, w.depthMode, w.w, w.h, w.snaps ? w.snapMs / w.snaps : 0.0,
                             w.warped ? w.drawMs / w.warped : 0.0, w.edgeN ? w.edgeSum / w.edgeN : 0.0, w.edgeMax, (unsigned long long)w.dumps);
            port_log_summary("hrw: warp edge | exposure per pair 0 px %llu, 1-8 %llu, 9-16 %llu, 17-24 %llu, 25-48 %llu, over 48 %llu | pairs shown as A again for the edge strip (GXV_HRW_EDGE_MAX_PX) %llu = %.1f %% of %llu asked",
                             (unsigned long long)w.edgeHist[0], (unsigned long long)w.edgeHist[1], (unsigned long long)w.edgeHist[2], (unsigned long long)w.edgeHist[3], (unsigned long long)w.edgeHist[4], (unsigned long long)w.edgeHist[5],
                             (unsigned long long)w.fell[10], w.asked ? 100.0 * (double)w.fell[10] / (double)w.asked : 0.0, (unsigned long long)w.asked);
        }
        if (g.guardKept || (g.guardDrops && HrwEff() == vgx::hrw::kWarp))  // (pace33 PATCH9)
            port_log_summary("hrw: guard | GXV_HRW_GUARD_B %ld: of %u pairs, second pictures asked while the game-speed guard repaid %u (re-projected, without the fresh player), given up to it %u",
                             vgx::EnvInt("GXV_HRW_GUARD_B", 1), g.pairs, g.guardKept, g.guardDrops);
        if (sHrwFreshOn) {
            auto F = [](int i) { return (unsigned long long)sHrwFr[i].load(); };
            const double nf = g.freshDraws ? g.freshDraws : 1;
            port_log_summary("hrw: fresh | partial draws %u (empty %u), game thread %.2f ms mean, %.2f max | render: received %llu, merged %llu, refused %llu (none %llu, no common owner %llu, no room %llu, chunks %llu, tex %llu, buf %llu, scheme %llu), "
                             "draws out %llu in %llu left out %llu, owners %llu, merge+undo %.2f ms/pair, bad composites %llu, resource-only %llu, with a picture's commands %llu (replay36) | governor switches deferred %u | scenes gone with it named %u (crash1114) | "
                             "verify-fresh: runs %llu, draws compared %llu, off by >0.25 px %llu, owners with another draw count %llu, mean %.3f px, max %.2f px, compose checks bad %llu",
                             g.freshDraws, g.freshEmpty, g.freshMs / nf, g.freshMaxMs, F(0), F(1), F(2), F(3), F(4), F(5), F(6), F(7), F(8), F(9), F(10), F(11), F(12), F(23),
                             F(1) ? F(13) / 1000.0 / F(1) : 0.0, F(14), F(22), F(24), g.deferredSwitch, port_hrw_scene_drops ? port_hrw_scene_drops() : 0u,
                             F(19), F(15), F(16), F(17), F(15) ? F(20) / 1000.0 / F(15) : 0.0, F(21) / 1000.0, F(18));
            g.freshDraws = g.freshEmpty = 0; g.freshMs = g.freshMaxMs = 0; g.deferredSwitch = 0;
            if (port_hrw_fresh_view_take) {  // (warp32 PATCH21)
                unsigned v[5] = {0, 0, 0, 0, 0};
                port_hrw_fresh_view_take(v);
                if (v[0] + v[1] + v[2])
                    port_log_summary("hrw: fresh view | partial draws whose player view pass was made first (it was left to the next picture's flush: GXV_HRW_FRESH_VIEW) %u, was already "
                                     "made this turn %u, was pending and left as the first picture's (GXV_HRW_FRESH_VIEW=0: the first picture's pose) %u | GXV_HRW_FRESH_LATE %d: passes made at "
                                     "the turn's start without a wait %u, late draws that still waited for the first picture's finish %u", v[0], v[1], v[2], port_hrw_fresh_late(), v[3], v[4]);
            }
            if (g.freshAutoWith + g.freshAutoWithout) {  // (warp32 PATCH22)
                port_log_summary("hrw: fresh auto | GXV_HRW_FRESH_AUTO %ld: pairs with the fresh player %u, without (the pairs had no room) %u; switched off %u time(s), on again %u; now %s",
                                 vgx::EnvInt("GXV_HRW_FRESH_AUTO", 1), g.freshAutoWith, g.freshAutoWithout, g.freshAutoOffN, g.freshAutoOnN, g.freshAutoOff ? "off" : "on");
                g.freshAutoWith = g.freshAutoWithout = g.freshAutoOffN = g.freshAutoOnN = 0;
            }
            if (MarioTimeCensus()) {   // (mariolag31, port/mario_time.h: what Mario's second pictures showed)
                port::mariotime::Census& C = g.mt;
                C.stale[port::mariotime::kDeadline] += sHrwMt[0].exchange(0, std::memory_order_relaxed);
                C.held[port::mariotime::kDeadline] += sHrwMt[1].exchange(0, std::memory_order_relaxed);
                C.stale[port::mariotime::kMerge] += sHrwMt[2].exchange(0, std::memory_order_relaxed);
                C.held[port::mariotime::kMerge] += sHrwMt[3].exchange(0, std::memory_order_relaxed);
                C.merged += sHrwMt[4].exchange(0, std::memory_order_relaxed);
                if (C.pairs) {
                    char line[1100];
                    port::mariotime::Format(line, sizeof line, MarioTimeMode(), C);
                    port_log_summary("%s", line);
                }
                const double est = C.est;
                C = port::mariotime::Census{};
                C.est = est;
            }
            {   // (warp32 PATCH23)
                unsigned v[6];
                for (int i = 0; i < 6; ++i) v[i] = sHrwDl[i].exchange(0, std::memory_order_relaxed);
                if (v[0] + v[1] + v[3])
                    port_log_summary("hrw: fresh deadline | GXV_HRW_FRESH_DEADLINE_MS10 %ld: second pictures with the partial draw in time %u; fresh missed its deadline %u (the plain "
                                     "re-projection went out; its partial draw came later, resources only, on %u); no partial draw that pair %u | the render thread waited for a "
                                     "partial draw on %u pairs, %.2f ms mean; deadlines set again after a GXV_LATE_TURN sleep %u", HrwFreshDeadline10(), v[0], v[1], v[2], v[3], v[4],
                                     v[4] ? v[5] / 1000.0 / v[4] : 0.0, static_cast<unsigned>(sHrwDlShift.exchange(0, std::memory_order_relaxed)));
            }
        }
        // hrw28: the significance set's census (5 s) and the picture mix (10 s)
        if (port_hrw_set_take_stats) {
            port::hrwset::PairStats ps;
            port_hrw_set_take_stats(&ps);
            if (ps.cands || ps.pairs || ps.hudN || ps.reasons[port::hrwset::kRsnViewDeferred]) {
                const double np = ps.pairs ? ps.pairs : 1;
                char rs[256];
                int o = 0;
                rs[0] = 0;
                for (int r = 1; r < port::hrwset::kRsnCount; ++r)
                    if (ps.reasons[r] && o < 200) o += snprintf(rs + o, sizeof rs - o, "%s%s %u", o ? ", " : "", port::hrwset::ReasonName(r), (unsigned)ps.reasons[r]);
                port_log_summary("hrw: set | per drawn turn: %.1f candidates, %.1f tagged; per pair: %.1f refreshed, %.1f stale (kept at the first picture's pose) | kept by reason (units, summed): %s | "
                                 "fresh %.2f ms mean / %.2f max of a %.2f ms budget, over budget %u of %u pairs | scan %.2f ms per drawn turn (%.0f actors) | "
                                 "Mario's move between pictures (what his shadow, silhouette and reflection lag by) px <1/<2/<4/<8/>=8: %u/%u/%u/%u/%u, max %.1f | HUD block %.2f ms mean / %.2f max (%u turns)",
                                 (double)ps.cands / (g.pairs ? g.pairs : 1), (double)ps.tagged / (g.pairs ? g.pairs : 1), (double)ps.refreshed / np, (double)ps.stale / np, o ? rs : "none",
                                 ps.fresh_ms / np, ps.fresh_max, ps.fresh_budget / np, (unsigned)ps.overBudget, (unsigned)ps.pairs,
                                 ps.scan_ms / (g.pairs ? g.pairs : 1), (double)ps.scanned / (g.pairs ? g.pairs : 1),
                                 (unsigned)ps.lagBins[0], (unsigned)ps.lagBins[1], (unsigned)ps.lagBins[2], (unsigned)ps.lagBins[3], (unsigned)ps.lagBins[4], ps.lagMax,
                                 ps.hudN ? ps.hud_ms / ps.hudN : 0.0, ps.hud_max, (unsigned)ps.hudN);
            }
        }
        g.cumPairs += g.pairsDone; g.cumNoPair += g.noPair; g.cumCuts += g.cuts; g.cumGuard += g.guarded; g.cumAsked += g.asked; g.cumFit += g.fit;
        g.cumA += g.pairs + g.noPair;  // (drawn turns that were first pictures or could not open a pair)
        if ((++g.mixTick & 1) == 0) {
            const uint64_t run = sHrwRt[1].load(), merged = sHrwFr[1].load();
            const uint64_t drop = [] { uint64_t t = 0; for (int i = 2; i < 10; ++i) t += sHrwRt[i].load(std::memory_order_relaxed); return t; }();
            const uint32_t slow = g.cumPairs > g.cumFit ? g.cumPairs - g.cumFit : 0;
            port_log_summary("hrw: pictures (10 s) | first pictures A %u | second pictures: re-projected %llu (with a fresh partial draw merged: %llu), fallback: camera cut %u, over the guard band %u, no camera / no pair %u, "
                             "dropped by the render side (queue / no buffer / id / hazard / projection / not presentable) %llu, pairs over 32.3 ms (too slow to gain) %u of %u",
                             g.cumA, (unsigned long long)(run - g.mixRun0), (unsigned long long)(merged - g.mixMerged0), g.cumCuts, g.cumGuard, g.cumNoPair, (unsigned long long)(drop - g.mixDrop0), slow, g.cumPairs);
            g.mixRun0 = run; g.mixMerged0 = merged; g.mixDrop0 = drop;
            if (vgx::EnvInt("GXV_HRW_FEEL", 1) != 0) {   // (hrwfeel36: the pair-by-pair figures of the 60 Assist decision rule; log only)
                port::hrwfeel::Feel::Report fr;
                { std::lock_guard<std::mutex> lk(port::hrwfeel::GlobalMutex()); fr = port::hrwfeel::Global().Take(HrwFeelNowMs()); }
                if (fr.shown + fr.lost) {
                    char fl[900];
                    const char* wn[port::hrwfeel::kWhy];
                    for (int i = 0; i < port::hrwfeel::kWhy; ++i) wn[i] = vgx::backend::WarpReasonName(i);
                    port::hrwfeel::Feel::Format(fl, sizeof fl, fr, wn);
                    port_log_summary("%s", fl);
                }
            }
            g.cumPairs = g.cumNoPair = g.cumCuts = g.cumGuard = g.cumAsked = g.cumFit = g.cumA = 0;
        }
        g.logT0 = now;
        if (g.noPair && port_log_summary) {  // (PATCH16: why the drawn turns of this window opened no pair)
            // (emufix31: the stage's locked 30 split by its source: the row / GXV_HRW_STAGE, or GXV_HRW_STAGE_AUTO's fallback (emu31c read the auto
            // fallback in Honeyhive on Vita3K as "the stage's row"; no compiled-in row sets reproj))
            port_log_summary("hrw: no pair on %u drawn turns: stage not in play or not settled yet (loading, menu, pause, wipe, GXV_HRW_SETTLE) %u, the stage's choice says no "
                             "re-projection %u (its row / GXV_HRW_STAGE %u, the GXV_HRW_STAGE_AUTO fallback %u), no game camera (demo, cut-scene, no scene) %u, other %u | fresh player draw: %s | selector-menu turns (galaxy map, star select) %u", g.noPair, g.whyNotSettled, g.whyStageOff,
                             g.whyStageOff - std::min(g.whyStageOff, g.whyStageAuto), g.whyStageAuto, g.whyNoCamera, g.noPair - std::min(g.noPair, g.whyNotSettled + g.whyStageOff + g.whyNoCamera),
                             !sHrwFreshOn ? "not asked for" : sHrwFreshOff.load(std::memory_order_relaxed) ? "given up for this run (see the earlier line)" : "on",
                             static_cast<unsigned>(sHrwMenuTurns.exchange(0, std::memory_order_relaxed)));
        }
        g.whyNotSettled = g.whyStageOff = g.whyNoCamera = g.whyStageAuto = 0;
        g.pairs = g.noPair = g.cuts = g.guarded = g.asked = g.fit = g.pairsDone = g.guardDrops = g.guardKept = 0;
        g.sumDrawn = g.sumUndrawn = g.sumPair = g.maxPair = g.sumRot = g.maxRot = g.maxTrans = 0;
        for (uint32_t& b : g.rotBin) b = 0;
    }
}
// Frames the game has finished (display copies), whichever renderer is selected.
#if defined(PORT_GX_VGX_ONLY)
// The build's only renderer (PS Vita): what the legacy recorder's files otherwise define.
extern "C" uint64_t port_gx_recording_frame(void) { return sFramesQueued.load(); }
extern "C" void port_gx_marker(uint32_t kind) { port_vgx_marker(kind); }
// (prewarm24) the renderer's GXV_PREWARM for the game's half (decomp JPAEmitterManager.cpp PortPrewarmGroup): the front end honours the warm-up
// scope's markers only while it is on, so the game draws inside a scope only then.
extern "C" int port_prewarm_mode(void) { return static_cast<int>(vgx::PrewarmMode()); }
// (precomp26) the renderer's GXV_STAGE_ENUM for the game's half (decomp Game/System/PortStageEnum.cpp); 0 where the renderer does not honour the scope
extern "C" int port_stage_enum_mode(void) { return static_cast<int>(vgx::StageEnumMode()); }
// (layoutwarm24) the renderer's GXV_LAYOUTWARM, and whether it honours the warm-up scope for it (the game opens a scope only then)
// (dive27 GXV_WATER_HALFCAP: 0 off, 1 on, "perf" on in Performance mode only; asked once when a stage's underwater filter is made, decomp Game/Screen/WaterCameraFilter.cpp)
extern "C" int port_perf_mode_60(void) __attribute__((weak));
extern "C" int port_water_halfcap_mode(void) {
    const char* e = getenv("GXV_WATER_HALFCAP");
    if (e == nullptr || *e == '\0') return 0;
    if (strcmp(e, "perf") == 0) return port_perf_mode_60 && port_perf_mode_60() != 0 ? 1 : 0;
    return atoi(e) != 0 ? 1 : 0;
}
extern "C" int port_rehearse_mode(void) { return vgx::RehearseMode() != 0 && vgx::WarmScopeHonoured() ? 1 : 0; }  // (dive27 GXV_REHEARSE)
extern "C" int port_layoutwarm_mode(void) { return vgx::LayoutWarmMode() != 0 && vgx::WarmScopeHonoured() ? 1 : 0; }
// The game's per-frame presentation hints (GameScene.cpp). One is of use on a console: bit 1
// of the flags word (PORT_GX_CAMERA_POINTER_UI) says the Wii pointer is driving a 2D menu
// (file select, pause, yes/no, the storybook, the galaxy domes); the pad code then lets the
// left stick move the pointer (vita/runtime/input.cpp).
static std::atomic<int64_t> sPointerMenuAt{0};
extern "C" void port_gx_camera(const float* camera) {
    uint32_t flags = 0;
    memcpy(&flags, &camera[25], sizeof flags);
    sPointerMenuAt.store((flags & 2u) ? port_host_time_ns() : 0, std::memory_order_relaxed);
}
extern "C" int mp_menu_wants_no_idle_pointer(void) __attribute__((weak));   // MP3 menus5: decomp/src/Game/MP/MpUiSystem.cpp (1 while a multiplayer menu is open). Weak: a link without decomp/Game/MP (host tests, vgx-only tools) has none = 0
extern "C" int port_pointer_on_menus(void) {
    const int64_t at = sPointerMenuAt.load(std::memory_order_relaxed);
    if (mp_menu_wants_no_idle_pointer && mp_menu_wants_no_idle_pointer()) return 0;   // MP3 menus5: our menus' pointer is the finger's alone (a touch keeps it three seconds)
    return at != 0 && port_host_time_ns() - at < 300000000; // (hints come with drawn frames: a few may be skipped)
}
extern "C" void port_gx_present_xfb(void*, int) {}
#else
extern "C" uint64_t port_gx_recording_frame(void);
#endif
extern "C" unsigned long long port_game_frames(void) {
    return sActive.load(std::memory_order_relaxed) ? sFramesQueued.load() : port_gx_recording_frame();
}
// strk27 GXV_FIF: a picture reached the display's flip (or a job that was never swapped is over): the display thread / the render thread tell the game thread's gate.
extern "C" void port_vgx_fif_retire(void) {
    if (!sFif.Enabled()) return;
    sFifShown.fetch_add(1, std::memory_order_relaxed);
    { std::lock_guard<std::mutex> g(sMtx); }   // (no wake-up is lost between the gate's check and its wait)
    sCv.notify_all();
}
extern "C" unsigned long long port_vgx_frames(void) { return sFramesShown.load(); }

// ------------------------------------------------------------------ player thread
// The front end (stream parser, vertex and texture decoding, draw merging) costs more than the
// game itself does per frame, so it gets a processor core of its own: the game thread writes
// GX commands into a staging segment and publishes its write position at the GX library's
// flush points (and every few dozen KiB in between); the player consumes behind it. The
// game's frame loop draws first and then runs its logic, and only waits for the "draw done"
// interrupt at the end of the frame, which the player raises when it reaches that command:
// the two overlap for most of a frame, as the console's CPU and GPU did. Everything the
// player reads from the game's memory is read when it parses the command that refers to it,
// so "draw done" still means the game may change that memory again.
//
// A publish is two atomic operations (the game flushes some 600 times a frame: every
// material's display list is recorded with the FIFO pointed elsewhere, and re-pointing it
// flushes). The player is only woken through the kernel when a synchronisation command or a
// good amount of bytes waits for it. There are two segments: the game moves to the other one
// when the player has caught up, so the positions never have to be changed under its feet.
namespace {
constexpr size_t kWakeBytes = 16u << 10;
std::atomic<bool> sPlayerOn{false};
uint8_t* sSegBase[2] = {nullptr, nullptr};
size_t sSegSize = 0;
// epoch << 32 | bytes published in segment (epoch & 1). Written by the game thread.
std::atomic<uint64_t> sPubState{0};
// Written by the player: the bytes it has consumed, then epoch << 32 | the published position
// it has worked up to. When the second equals sPubState the player has nothing to do, and the
// first is stable.
std::atomic<uint32_t> sPlRd{0};
std::atomic<uint64_t> sPlDone{0};
std::atomic<bool> sPlSleeping{false};
std::mutex sPlMtx;
std::condition_variable sPlCv;
pthread_t sPlThread;
// polish23 (8 Nov): GXV_COPY_DROP (read once; default 0 until the console check; the count line is written either way). A scene's
// destruction (GameSystemSceneController::exeDestroyScene, game thread, after the front end has drawn the scene's last picture) hands
// over its heaps' ranges; the front end, before the first command published after that, counts the EFB-copy textures made into them
// (vgx_texcache.cpp CopyTexturesInRange) and with the switch frees them. The request is stored before anything of the next scene is
// published, and the publication's release / acquire orders it before the commands that follow.
std::mutex sCopyDropMtx;
std::atomic<unsigned> sCopyDropSeq{0};
unsigned sCopyDropDone = 0;  // (front-end thread)
const void* sCopyDropRange[2][2] = {};
bool CopyDropOn() {
    static const bool on = vgx::EnvInt("GXV_COPY_DROP", 0) != 0;
    return on;
}
// heap23b (read once; default 0 here, 1 from vita/vgx-game/entry.cpp): 0 the state-list store keeps every scene's entries (until its own
// clear at 16,384), 1 the entries of lists in a destroyed scene's heaps are freed with it, 2 every entry is freed at a scene's destruction.
int SlSceneDropMode() {
    static const int mode = [] {
        const long v = vgx::EnvInt("GXV_SL_SCENEDROP", 0);
        return v == 1 || v == 2 ? static_cast<int>(v) : 0;
    }();
    return mode;
}
// On the thread that runs the front end, before it parses newly published bytes.
inline void CopyDropService() {
    const unsigned seq = sCopyDropSeq.load(std::memory_order_acquire);
    if (__builtin_expect(seq == sCopyDropDone, 1)) return;
    const void* r[2][2];
    {
        std::lock_guard<std::mutex> lk(sCopyDropMtx);
        std::memcpy(r, sCopyDropRange, sizeof r);
        sCopyDropDone = sCopyDropSeq.load(std::memory_order_relaxed);
    }
    vgx::backend::NoteSceneGone(); // (bugs26: GXV_RUNG_SKIPRS_SETTLE)
    const bool drop = CopyDropOn();
    const unsigned before = vgx::CopyTextureCount();
    unsigned inScene = 0;
    for (int i = 0; i < 2; ++i) inScene += vgx::CopyTexturesInRange(r[i][0], r[i][1], drop);
    // heap23b (GXV_SL_SCENEDROP, vgx_state.h StateListsSceneGone): the state-list store's entries of lists in the destroyed scene's heaps
    // are freed here (1, default), every entry (2), or none (0: as before; only counted). Appended to the line below.
    const void* lo[2] = {r[0][0], r[1][0]};
    const void* hi[2] = {r[0][1], r[1][1]};
    const vgx::StateListStore sl = vgx::StateListsSceneGone(lo, hi, 2, SlSceneDropMode());
    size_t dlLists = 0, dlBypass = 0;
    vgx::DlCacheListCounts(dlLists, dlBypass);
    // grow24 (GXV_TEX_SCENEDROP, vgx_texcache.cpp TextureCacheSceneGone): the guest textures of the destroyed heaps (1), and those no frame of
    // the scene bound (2), are freed with the scene; the count is made with the switch off too. The texture-id table: ids ever made, free
    // (released and waiting out their quarantine) and handed out again; the back end's table slots (g_tex) as it last had them.
    const vgx::TexSceneDrop td = vgx::TextureCacheSceneGone(lo, hi, 2, vgx::TexSceneDropMode());
    vgx::TexMagSceneEnd(); // (GXV_TEXMAG_CENSUS: writes the stage's file; nothing when off)
    if (port_log_summary)
        port_log_summary("vgx: scene destroyed: EFB-copy textures %u, %u of them made into its heaps (%s); texture ids made so far %u; "
                         "state lists %u kept (%u KiB), %u freed (%u KiB; GXV_SL_SCENEDROP=%d; %u store clears); display-list cache %u lists "
                         "(%u bypassed); texture cache %u entries; textures: %u in the destroyed heaps, %u idle outside them, %u freed (%u KiB; "
                         "GXV_TEX_SCENEDROP=%d) and %u entries without a texture erased, %u kept (%u KiB of texels held); texture ids: %u made, %u free, "
                         "%u handed out again, back-end table %u slots; age %d: %u aged out, %u over the caps, %u pictures in play%s; "
                         "refetched after a free: same address %u (%u in play, %u this scene), same bytes elsewhere %u (%u in play, %u this scene), adopted %u", before, inScene,
                         drop ? "freed, GXV_COPY_DROP=1" : "kept, GXV_COPY_DROP=0", static_cast<unsigned>(vgx::TextureIdsMade()), sl.entries,
                         static_cast<unsigned>(sl.bytes / 1024u), sl.dropped, static_cast<unsigned>(sl.droppedBytes / 1024u), SlSceneDropMode(),
                         static_cast<unsigned>(sl.clears), static_cast<unsigned>(dlLists), static_cast<unsigned>(dlBypass),
                         td.cacheBefore, td.inRange, td.idleOutside, td.freed, static_cast<unsigned>(td.freedBytes / 1024u), td.mode,
                         td.freedNoTexture, td.cacheAfter, static_cast<unsigned>(td.liveBytes / 1024u), td.idsMade, td.idsFree, td.idsReused,
                         vgx::backend::TextureSlots(), td.age, td.aged, td.freedCap, td.playPics, td.shortScene ? " (a short scene: not counted idle)" : "",
                         td.refAddr, td.refAddrLive, td.refAddrScene, td.refContent, td.refContentLive, td.refContentScene, td.adopted);
    // hdtex24: the HD pack's own line (at most one a scene, none without a pack or when nothing happened): proves from log.txt that textures were substituted
    if (port_log_summary) {
        char hdLine[640];
        if (vgx::hdtex::SceneLine(hdLine, sizeof hdLine)) port_log_summary("%s", hdLine);
    }
}
// ARM harness, PETARI_VCLOCK_FE=1: the player's unfinished work holds the virtual clock (port_vclock_external_*), so the game
// sees the front end as infinitely fast and plays the same frames whatever the front end costs (instruction-count A/B runs
// otherwise land in different scenes: a slower front end made the game skip retraces). sFeBusy: the clock is held.
std::atomic<bool> sFeBusy{false};
inline bool VclockFe() {
#if defined(PORT_ARMHF_HARNESS)
    static const bool on = port_vclock_enabled() && getenv("PETARI_VCLOCK_FE") && atoi(getenv("PETARI_VCLOCK_FE")) != 0;
    return on;
#else
    return false;
#endif
}
inline void FeHold() { if (VclockFe() && !sFeBusy.exchange(true)) port_vclock_external_begin(); }
inline void FeRelease() { if (VclockFe() && sFeBusy.exchange(false)) port_vclock_external_end(); }
// game thread only
uint32_t sGmEpoch = 0;
size_t sGmPubOff = 0, sGmWakeOff = 0;
// GXV_SEG_IDLE (see port_vgx_publish, flag 4): segment changes made at the game's idle points, and the ones made after a full segment,
// with the game thread's time in those waits; reported on the basic log every ~10 s. Game thread only.
struct SegCounters { uint32_t idle = 0, full = 0; int64_t fullNs = 0; uint32_t pre = 0, preMiss = 0; int64_t preNs = 0, preMaxNs = 0; };  // (pre*: pace33 PATCH9, port/seg_prewait.h)
port::segpre::State sSegPre;
std::atomic<uint64_t> sPlProg{0};   // (pace33 PATCH9) the player's read position after its last batch: epoch << 32 | offset; read by the game thread's pre-wait rule only
SegCounters sSeg, sSegReported;
int64_t sSegReportAt = 0;

void* PlayerMain(void*) {
    PORT_THREAD_NAME("vgx-player");  // (the "cores-5s:" lines name it)
#if defined(__vita__)
    port_vita_pin_core(2);
#endif
    port_thread_class(3);
    uint32_t epoch = 0;
    size_t rd = 0, seen = 0;
    for (;;) {
        const uint64_t st = sPubState.load(std::memory_order_acquire);
        const uint32_t e = static_cast<uint32_t>(st >> 32);
        const size_t pub = static_cast<uint32_t>(st);
        if (e != epoch) { // the game moved on to the other segment (what was left of this one went with it)
            // (GXV_FE_PARALLEL, vgx_fepar.h: decodes the finishing thread has still to make may read the segment the game left; it writes
            // that one again only after it has moved back to it, which needs this thread to have caught up with the new one first)
            vgx::fepar::SettleSources();
            epoch = e;
            rd = seen = 0;
        }
        if (pub == seen) {
            sPlRd.store(static_cast<uint32_t>(rd), std::memory_order_release);
            sPlDone.store((static_cast<uint64_t>(epoch) << 32) | seen, std::memory_order_release);
            if (VclockFe()) {
                FeRelease();
                if (sPubState.load(std::memory_order_acquire) != st) { FeHold(); continue; } // (published meanwhile)
            }
            // More usually follows at once while the game is drawing: look again a few
            // times before going to sleep (a wake-up costs the game thread a kernel call).
            bool more = false;
            for (int i = 0; i < 400 && !more; ++i) more = sPubState.load(std::memory_order_acquire) != st;
            if (more) continue;
            sPlSleeping.store(true);
            if (sPubState.load() != st) { sPlSleeping.store(false); continue; }
            const int64_t ewSleep0 = EwOn() && sEwStart ? port_host_time_ns() : 0;  // (endwait26: sleeping inside a picture is idle time, not the picture's work)
            const int64_t laneSleep0 = LaneOn() ? port_host_time_ns() : 0;
            {
                std::unique_lock<std::mutex> lk(sPlMtx);
                // (the time limit bounds how long bytes that were published without a wake-up can wait)
                sPlCv.wait_for(lk, std::chrono::milliseconds(2), [&] { return sPubState.load() != st; });
            }
            if (ewSleep0) sEwIdleNs += port_host_time_ns() - ewSleep0;
            if (laneSleep0) {   // (pace33 PATCH4: the picture's idle time; the front end is busy again from here)
                const int64_t t = port_host_time_ns();
                if (sLaneFirstNs) sLaneIdleNs += t - laneSleep0;
                sLaneBusyFromNs = t;
            }
            sPlSleeping.store(false);
            continue;
        }
        seen = pub;
        CopyDropService();  // (polish23, GXV_COPY_DROP: a destroyed scene's copy textures, before the bytes published after it)
        if (EwOn() && !sEwStart) sEwStart = port_host_time_ns();  // (endwait26: the picture's first parsed byte)
        if (LaneOn() && !sLaneFirstNs) sLaneFirstNs = port_host_time_ns();  // (pace33 PATCH4)
        {
            vgx::workload::Scope observation(vgx::workload::FrontSample, vgx::workload::SampleProcess());
            rd += vgx::ProcessStream(sSegBase[epoch & 1] + rd, static_cast<uint32_t>(pub - rd), false);
        }
        sPlProg.store((static_cast<uint64_t>(epoch) << 32) | rd, std::memory_order_relaxed);
    }
    return nullptr;
}

inline void WakePlayer() {
    // Pair with the player's sleeping-store/publication-load: release publication
    // alone does not order this store-to-load handshake on ARM.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (sPlSleeping.load()) {
        std::lock_guard<std::mutex> lk(sPlMtx);
        sPlCv.notify_one();
    }
}
} // namespace

extern "C" int port_vgx_player(void) { return sPlayerOn.load(std::memory_order_relaxed) ? 1 : 0; }

#if defined(__vita__)
extern "C" void __PortGXFifoFlush(void);
extern "C" void port_vgx_settings_release(unsigned generation) {
    sSettingsRenderHold.Cancel(generation);
    sCv.notify_all();
}

extern "C" int port_vgx_settings_hold(unsigned generation, unsigned timeoutMs) {
    if (!generation) return 0;
    const int64_t deadline = port_host_time_ns() + int64_t(timeoutMs) * 1000000;
    __PortGXFifoFlush();
    const uint64_t published = sPubState.load(std::memory_order_acquire);
    while (sPlayerOn.load() && sPlDone.load(std::memory_order_acquire) != published) {
        if (port_host_time_ns() >= deadline) return 0;
        WakePlayer();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    if (!sSettingsRenderHold.Begin(generation)) return 0;
    sCv.notify_all();
    while (!sSettingsRenderHold.Acknowledged(generation)) {
        if (sSettingsRenderHold.Requested() != generation || port_host_time_ns() >= deadline) {
            port_vgx_settings_release(generation);
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return 1;
}
#endif

extern "C" void port_vgx_stream_setup(uint8_t* seg0, uint8_t* seg1, size_t size) {
    sSegBase[0] = seg0;
    sSegBase[1] = seg1;
    sSegSize = size;
}

// GXV_SEG_IDLE=<KiB> (default 128; 0: off, as before): the game changes segments at its idle points as well.
//
// The other segment is only taken while the player has done everything published (the bytes it has not parsed yet, an incomplete
// command at most, are copied to the start of the other one). The test for it above (full || past half) is made right after a publish
// that brought new bytes, which the player cannot have parsed yet, so in practice a segment was only ever changed when it was full,
// and a full segment makes the game thread wait until the player has parsed everything up to its end. That happens in the middle of
// the game's draw, while the front end is a good part of a picture behind it: the lit Comet Observatory (r47, r49) shows about 2.5
// of these waits a second, 13-25 ms each (the "gx-publish-in-turn" row of GXV_WORKLOAD_PROBE): ~40 ms a second, 4 % of the game
// thread, on the core that limits the picture rate, and a late picture each time.
// With this switch the game calls port_vgx_publish with flag 4 at the start of every turn (__PortGXFifoIdlePoint, gx_fifo.cpp; from
// GameSystem::frameLoop). If the segment holds at least <KiB>, and the player has done everything published before this call (it is
// idle then: it reads nothing until sPubState changes, which only this thread writes), the segment is changed exactly as after a full
// wait: what the player has not parsed (sPlRd up to the write position: the incomplete command, and any bytes not yet published) goes
// to the start of the other segment, the epoch moves on, and the player starts there. The player is caught up at most turn starts
// (it finishes a picture before the game's next drawn turn), so the segments change every picture or two and never fill; when the
// player stays behind for a whole segment (~2 MiB, about ten pictures), the full wait is made as before. The command stream the
// player parses is the same bytes in the same order (only where they lie changes), so pictures are identical.
static size_t SegIdleBytes() {
    return PORT_HOT_KNOB(size_t, [] {  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
        const long kib = vgx::EnvInt("GXV_SEG_IDLE", 128);
        return kib > 0 ? static_cast<size_t>(kib) << 10 : 0;
    }());
}
extern "C" int port_vgx_seg_idle(void) { return SegIdleBytes() != 0 && sPlayerOn.load(std::memory_order_relaxed) ? 1 : 0; }
// Moves on to the other segment, the player being idle with its read position at sPlRd; returns where writing continues.
static uint8_t* SegSwitch(uint8_t* base, size_t off) {
    const size_t rd = sPlRd.load(std::memory_order_acquire);
    const size_t left = off - rd;
    uint8_t* const next = sSegBase[(sGmEpoch + 1) & 1];
    if (left) memcpy(next, base + rd, left);
    ++sGmEpoch;
    sGmPubOff = sGmWakeOff = left;
    if (left) FeHold();
    sPubState.store((static_cast<uint64_t>(sGmEpoch) << 32) | left, std::memory_order_release);
    return next + left;
}
static void SegReport() {
    if (!PORT_LOGGING || !port_log_summary) return;
    const int64_t now = port_host_time_ns();
    if (!sSegReportAt) { sSegReportAt = now; sSegReported = sSeg; return; }
    if (now - sSegReportAt < 10000000000ll) return;
    const SegCounters d{sSeg.idle - sSegReported.idle, sSeg.full - sSegReported.full, sSeg.fullNs - sSegReported.fullNs};
    if (d.idle || d.full)
        port_log_summary("stream: %.0f s: %u segment changes at the game's idle points (GXV_SEG_IDLE=%u KiB), %u after a full segment (the game waited %.0f ms in all)",
                         (now - sSegReportAt) / 1e9, d.idle, static_cast<unsigned>(SegIdleBytes() >> 10), d.full, d.fullNs / 1e6);
    if (sSeg.pre != sSegReported.pre || sSeg.preMiss != sSegReported.preMiss)   // (pace33 PATCH9)
        port_log_summary("stream: pre-wait (GXV_SEG_PREWAIT_US=%ld): %u segment changes after a short wait at a turn start, %u waits given up; the game waited %.1f ms in all, %.1f ms at most",
                         vgx::EnvInt("GXV_SEG_PREWAIT_US", 4000), sSeg.pre - sSegReported.pre, sSeg.preMiss - sSegReported.preMiss, (sSeg.preNs - sSegReported.preNs) / 1e6, sSeg.preMaxNs / 1e6);
    sSeg.preMaxNs = 0;
    sSegReportAt = now;
    sSegReported = sSeg;
}

// ---- endwait26 (GXV_ENDWAIT_LOG): the game thread's side of the report. port_vgx_endwait_begin just before the wait notes where the stream's and the back end's sums stand;
// port_vgx_endwait_line after a wait of GXV_ENDWAIT_MS (40) or more writes the line. Game thread only (sSeg is its).
namespace {
SegCounters sEwSeg0;
vgx::backend::EndWaitCum sEwBe0;
bool sEwSegValid = false;
}  // namespace
extern "C" void port_vgx_endwait_begin(void) {
    if (!EwOn()) return;
    sEwSeg0 = sSeg;
    vgx::backend::EndWaitCumulative(sEwBe0);
    sEwSegValid = true;
}
extern "C" size_t port_vgx_endwait_line(int64_t t0, int64_t t1, int where, char* out, size_t cap) {
    if (!EwOn() || !out || cap < 64) return 0;
    vgx::endwait::Inputs in;
    in.where = where;
    in.waitStartNs = t0;
    in.waitEndNs = t1;
    in.waitMs = (t1 - t0) / 1e6;
    while (sEwLock.test_and_set(std::memory_order_acquire)) {}
    const bool allowed = sEwStage.LineAllowed();
    in.stageKey = sEwStage.key;
    in.sinceStartS = sEwStage.startNs ? (t1 - sEwStage.startNs) / 1e9 : 0.0;
    in.sinceOpenS = sEwStage.openNs ? (t1 - sEwStage.openNs) / 1e9 : -1.0;
    in.nPics = sEwRing.Overlapping(t0 - 2000000, t1 + 2000000, in.pics, vgx::endwait::kShow);
    sEwLock.clear(std::memory_order_release);
    if (!allowed) return 0;
    if (sEwSegValid) {
        in.stream.full = sSeg.full - sEwSeg0.full;
        in.stream.fullMs = (sSeg.fullNs - sEwSeg0.fullNs) / 1e6;
    }
    {
        std::lock_guard<std::mutex> lk(sMtx);
        in.be.jobs = static_cast<unsigned>(sJobs.size());
        in.be.limit = sDepthCap && sDepthCap < sQueueLimit ? sDepthCap : sQueueLimit;
        in.be.freeBufs = static_cast<unsigned>(sFree.size());
    }
    in.be.stage = sBackStage.load(std::memory_order_relaxed);
    in.be.sinceMs = (port_host_time_ns() - sBackSince.load(std::memory_order_relaxed)) / 1e6;
    if (sEwSegValid) {
        vgx::backend::EndWaitCum now;
        vgx::backend::EndWaitCumulative(now);
        const vgx::backend::EndWaitCum& a = sEwBe0;
        vgx::endwait::BeDone& d = in.done;
        d.pictures = now.frames - a.frames; d.execMs = now.execMs - a.execMs; d.targetMs = now.targetMs - a.targetMs; d.texMs = now.texMs - a.texMs;
        d.sceneMs = now.sceneMs - a.sceneMs; d.finishMs = now.finishMs - a.finishMs; d.dispMs = now.dispMs - a.dispMs; d.presentMs = now.presentMs - a.presentMs;
        d.drawMs = now.drawMs - a.drawMs; d.endMs = now.endMs - a.endMs; d.reserveMs = now.reserveMs - a.reserveMs; d.targetsMade = now.targets - a.targets;
        d.texN = now.texN - a.texN; d.texKiB = (now.texBytes - a.texBytes) / 1024.0;
        in.haveBeDone = true;
    }
    sEwSegValid = false;
    if (in.nPics) vgx::backend::EndWaitTag(in.pics[in.nPics - 1].frame);  // (the picture that released the wait: its back-end line follows)
    return vgx::endwait::Format(out, cap, in);
}

// flags: 1 the segment is full (the call waits for the player and moves to the other segment),
// 2 the bytes end with a synchronisation command (draw done, draw-sync token),
// 4 an idle point of the game's (GXV_SEG_IDLE, above): the other segment is taken now if the player is idle and this one is used enough.
// Returns where writing continues: wr, or a position in the other segment.
extern "C" uint8_t* port_vgx_publish(uint8_t* wr, int flags) {
    uint8_t* const base = sSegBase[sGmEpoch & 1];
    const size_t off = static_cast<size_t>(wr - base);
    if ((flags & 4) && !(flags & 1)) {
        SegReport();
        const size_t limit = SegIdleBytes();
        // (the player has done everything published before this call: what it stored last, in this epoch, is sGmPubOff)
        if (limit && off >= limit && sPlDone.load(std::memory_order_acquire) == ((static_cast<uint64_t>(sGmEpoch) << 32) | sGmPubOff)) {
            ++sSeg.idle;
            uint8_t* const at = SegSwitch(base, off);
            if (at != sSegBase[sGmEpoch & 1]) WakePlayer(); // (bytes moved over, not all of them parsed or published yet: the player may be asleep)
            return at;
        }
        // pace33 PATCH9 (port/seg_prewait.h): the segment is about to fill and the player is nearly done: a short wait now instead of the full wait in the middle of the draw
        static const port::segpre::Config pc = [] {
            port::segpre::Config c;
            c.segSize = sSegSize;
            c.idleLimit = SegIdleBytes();
            const long us = vgx::EnvInt("GXV_SEG_PREWAIT_US", 4000);
            c.waitNs = us <= 0 ? 0 : (us > 12000 ? 12000 : us) * 1000ll;
            const long kib = vgx::EnvInt("GXV_SEG_PREWAIT_KIB", 64);
            c.remLimit = static_cast<size_t>(kib < 1 ? 1 : kib > 1024 ? 1024 : kib) << 10;
            return c;
        }();
        if (pc.waitNs) {
            const uint64_t prog = sPlProg.load(std::memory_order_relaxed);
            const size_t prd = static_cast<uint32_t>(prog);
            const bool known = static_cast<uint32_t>(prog >> 32) == sGmEpoch && prd <= sGmPubOff;
            if (port::segpre::Want(sSegPre, pc, sGmEpoch, off, known, known ? sGmPubOff - prd : 0)) {
                const uint64_t want = (static_cast<uint64_t>(sGmEpoch) << 32) | sGmPubOff;
                const int64_t t0 = port_host_time_ns();
                bool done = false;
                for (;;) {
                    if (sPlDone.load(std::memory_order_acquire) == want) { done = true; break; }
                    if (port_host_time_ns() - t0 >= pc.waitNs) break;
                    WakePlayer();
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
                const int64_t dt = port_host_time_ns() - t0;
                sSeg.preNs += dt;
                if (dt > sSeg.preMaxNs) sSeg.preMaxNs = dt;
                if (done) {
                    ++sSeg.pre;
                    uint8_t* const at = SegSwitch(base, off);
                    if (at != sSegBase[sGmEpoch & 1]) WakePlayer();
                    return at;
                }
                ++sSeg.preMiss;
                port::segpre::Missed(sSegPre, sGmEpoch);
            }
        }
    }
    const uint64_t state = (static_cast<uint64_t>(sGmEpoch) << 32) | off;
    if (off != sGmPubOff) {
        sGmPubOff = off;
        FeHold(); // (before the bytes are visible: the player releases only after it has seen them)
        sPubState.store(state, std::memory_order_release);
    }
    const bool full = (flags & 1) != 0;
    if (full) {
        // Wait until the player has done everything published (what it leaves is a command
        // that is not complete yet).
        const int64_t t0 = PORT_RUNTIME_DIAGNOSTICS && sPerfSeconds > 0 ? port_host_time_ns() : 0;
        vgx::workload::Scope observation(vgx::workload::PublishInTurn);
        const int64_t tSeg = SegIdleBytes() ? port_host_time_ns() : 0; // (GXV_SEG_IDLE's report)
        while (sPlDone.load(std::memory_order_acquire) != state) {
            WakePlayer();
            // (real time, not port_host_sleep_ns: under the ARM harness's virtual clock that one
            // only returns when every game thread is idle, and this is a game thread waiting)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        if (PORT_RUNTIME_DIAGNOSTICS && sPerfSeconds > 0) sGmWaitNs.fetch_add(port_host_time_ns() - t0, std::memory_order_relaxed);
        if (tSeg) {
            ++sSeg.full;
            sSeg.fullNs += port_host_time_ns() - tSeg;
        }
    }
    // Move to the other segment when the player has caught up and this one is half used.
    if ((full || off > sSegSize / 2) && sPlDone.load(std::memory_order_acquire) == state) return SegSwitch(base, off);
    if ((flags & 2) || off - sGmWakeOff >= kWakeBytes) {
        sGmWakeOff = off;
        WakePlayer();
    }
    return wr;
}

// One line: where the renderer's host memory is (the frame buffers keep what their largest frame
// needed; the display-list cache keeps decoded vertices; the game's `new` is everything of it).
// Meant for the status log and the ends of a fast-forward; the figures are read without stopping
// the player, so they are approximate.
extern "C" unsigned port_host_new_live_kib(void);
extern "C" void port_vgx_diagnostic_csv_pause(int selecting) {
    if (!PORT_RUNTIME_DIAGNOSTICS || !selecting) return;
    vgx::stats::ExportFrameLog();
    vgx::backend::ExportCountLog();
}

extern "C" void port_vgx_census_log(const char* when) {
    if (!sActive.load(std::memory_order_relaxed)) return;
    size_t ring = 0, vtx = 0, other = 0;
    unsigned bufs = 0;
    {
        std::lock_guard<std::mutex> lk(sMtx);
        for (const vgx::CmdBuf* b : sFree) { ring += b->HostBytes(); vtx += b->vtx.capacity(); ++bufs; }
        for (const vgx::CmdBuf* b : sJobs) { ring += b->HostBytes(); vtx += b->vtx.capacity(); ++bufs; }
    }
    const size_t pending = vgx::Pending().HostBytes();
    other = ring - vtx;
    port_log_summary("vgx host memory %s: frame buffers %u KiB in %u (vertex arenas %u KiB, the rest %u KiB), the front end's own frame %u KiB, display-list cache %u KiB "
             "(+ %u KiB static in the back end), the game's `new` %u KiB",
             when, static_cast<unsigned>(ring / 1024), bufs, static_cast<unsigned>(vtx / 1024), static_cast<unsigned>(other / 1024),
             static_cast<unsigned>(pending / 1024), static_cast<unsigned>(vgx::DlCacheLiveBytes() / 1024),
             static_cast<unsigned>(vgx::DlCacheStaticBytes() / 1024), port_host_new_live_kib());
}

// A stall report: the hand-over state between the game thread, the player and the back end.
extern "C" void port_vgx_debug_dump(void) {
    if (!PORT_RUNTIME_DIAGNOSTICS) return;
    const uint64_t pub = sPubState.load(), done = sPlDone.load();
    size_t jobs = 0, freeBufs = 0;
    {
        std::lock_guard<std::mutex> lk(sMtx);
        jobs = sJobs.size();
        freeBufs = sFree.size();
    }
    static const char* const kStage[] = {"waiting for a frame", "executing a frame", "finishing a frame", "?"};
    const int stage = sBackStage.load();
    const double stageMs = (port_host_time_ns() - sBackSince.load()) / 1e6;
    port_log("vgx: published epoch %u offset %u; player done epoch %u offset %u, read %u, %s; frames queued %llu, shown %llu; back end: %u job(s) waiting, %u free buffer(s), queue limit %u, %s for %.0f ms",
             static_cast<unsigned>(pub >> 32), static_cast<unsigned>(pub), static_cast<unsigned>(done >> 32), static_cast<unsigned>(done), sPlRd.load(),
             sPlSleeping.load() ? "asleep" : "awake", sFramesQueued.load(), sFramesShown.load(), static_cast<unsigned>(jobs), static_cast<unsigned>(freeBufs),
             sQueueLimit, kStage[stage >= 0 && stage < 3 ? stage : 3], stageMs);
}

extern "C" size_t port_vgx_consume(const uint8_t* data, size_t len) {
    // (the caller holds a PortHostAllocScope: the front end allocates from the host heap)
    CopyDropService();  // (polish23: as in PlayerMain)
    vgx::workload::Scope observation(vgx::workload::FrontSample, vgx::workload::SampleProcess());
    return vgx::ProcessStream(data, static_cast<uint32_t>(len), false);
}

extern uint32_t gJ3DPortVer[] __attribute__((weak));  // (GXV_J3DNATIVE: decomp J3DPacket.cpp; absent in a build without the game)

// polish23 (GXV_COPY_DROP): game thread, a scene's destruction (see CopyDropService). Nothing to do before the renderer runs.
extern "C" void port_vgx_scene_heaps_gone(const void* lo0, const void* hi0, const void* lo1, const void* hi1) {
    if (!sActive.load(std::memory_order_relaxed)) return;
    {   // crash36 self-check: none of the job queue's storage may lie in the game's heaps when a scene's heaps go (it did: see sJobs)
        unsigned bad = 0, game = 0, was = 0, size = 0, cap = 0;
        const void* at = nullptr;
        {
            std::lock_guard<std::mutex> lk(sMtx);
            const void* const p[4] = {sJobs.data(), sFree.data(), sHrwSpare.data(), sHrwFreshSpare.data()};
            for (unsigned i = 0; i < 4; ++i)
                if (p[i] && port_is_game_heap_ptr(p[i])) { bad |= 1u << i; at = p[i]; }
            game = sJc36.game; was = sJc36.reported; sJc36.reported = game;
            size = static_cast<unsigned>(sJobs.size()); cap = static_cast<unsigned>(sJobs.capacity());
        }
        if (port_log_summary) {
            if (bad) port_log_summary("crash36: SELF-CHECK FAILED at a scene's destruction: job queue storage in the game's heaps (mask %u: 1 jobs, 2 free, 4 asks, 8 partial draws; %p)", bad, at);
            else if (game != was) port_log_summary("crash36: scene destroyed: the job queue's storage is host memory (%u jobs queued, capacity %u); pushes that would have put it in "
                                                   "the game's heap: %u so far, %u since the last scene (each was a crash waiting for the next stage change)", size, cap, game, game - was);
        }
    }
    std::lock_guard<std::mutex> lk(sCopyDropMtx);
    sCopyDropRange[0][0] = lo0;
    sCopyDropRange[0][1] = hi0;
    sCopyDropRange[1][0] = lo1;
    sCopyDropRange[1][1] = hi1;
    sCopyDropSeq.fetch_add(1, std::memory_order_release);
    if (gJ3DPortVer) __atomic_fetch_add(&gJ3DPortVer[0], 1u, __ATOMIC_RELAXED);  // (GXV_J3DNATIVE: J3DPortVer_Scene, decomp/include/JSystem/J3DGraphBase/J3DPortNative.hpp)
}

extern "C" void port_vgx_notify_write(const void* p, size_t n) {
    if (!sActive.load(std::memory_order_relaxed) || !n) return;
    vgx::hle::NotifyGuestWrite(p, static_cast<uint32_t>(n > 0xFFFFFFFFu ? 0xFFFFFFFFu : n));
}

extern "C" unsigned long long port_vgx_capture(const char* path, int) {
    PortHostAllocScope hostAlloc;  // (the harness calls this on a game thread; sCapPath outlives any scene's heap)
    std::unique_lock<std::mutex> lk(sMtx);
    sCapPath = path;
    sCapDone = false;
    sCapWanted = true;
    if (!sCv.wait_for(lk, std::chrono::seconds(5), [] { return sCapDone; })) {
        sCapWanted = false;
        return 0;
    }
    return sCapFrame;
}

extern "C" void port_gx_select_backend(void) {
#if !defined(PORT_GX_VGX_ONLY)
    const char* want = getenv("PETARI_BACKEND");
    if (!(want && !strcmp(want, "vgx"))) {
        port_gx_use_recorder_backend();
        return;
    }
#endif
    PortHostAllocScope hostAlloc;
#if defined(__vita__) || defined(PORT_ARMHF_HARNESS)
    const long inflightDefault = 2, skipDefault = 0;
#else
    const long inflightDefault = 8, skipDefault = 1;
#endif
    sMaxInFlight = static_cast<unsigned>(vgx::EnvInt("GXV_INFLIGHT", inflightDefault));
    if (sMaxInFlight < 1) sMaxInFlight = 1;
    if (sMaxInFlight > 16) sMaxInFlight = 16;
    sQueueLimit = sMaxInFlight + 1;
#if defined(__vita__)
    // (GXV_INFLIGHT set by hand: that number is kept; GXV_ADAPT_DEPTH=0 turns the adaptation off)
    sAdaptDepth = !getenv("GXV_INFLIGHT") && vgx::EnvInt("GXV_ADAPT_DEPTH", 1) != 0;
    sWaitLog = vgx::EnvInt("GXV_WAITLOG", 0) != 0;  // (the "waits:" line: where the game and render threads blocked)
    sFixedDepth = vgx::EnvInt("GXV_FIXED_DEPTH", 0) != 0;  // (the queue-depth adaptation measures its own CPU time under a display cap)
#endif
    {
        const long fif = vgx::EnvInt("GXV_FIF", 0);
        // (pace33: with GXV_HRW too. A pair's second picture is no hand-over of the game thread and is not counted on either side: its stamp carries bit 30 and the display callback does not retire it)
        if (fif >= 2 && fif <= 4) {
            sFif.Configure(static_cast<unsigned>(fif));
            port_log("fif: GXV_FIF=%ld: the game thread hands a picture over only when at most %ld pictures are between the hand-over and the display's flip (waits at most 50 ms)", fif, fif);
        } else if (fif) port_log("fif: GXV_FIF=%ld ignored (2..4 only)", fif);
        const long cap = vgx::EnvInt("GXV_LATENCY24_DEPTH", 0);
        sDepthCap = cap > 0 && cap < 16 ? static_cast<unsigned>(cap) : 0;
        if (sDepthCap) port_log("latency: GXV_LATENCY24_DEPTH=%u: at most %u frame(s) in the back end's queue", sDepthCap, sDepthCap);
    }
    sSkipWhenBehind = vgx::EnvInt("GXV_SKIP_BEHIND", skipDefault) != 0;
#if !defined(__vita__)
    sSkipAlways = vgx::EnvInt("GXV_SKIP_ALL", 0) != 0;
    if (sSkipAlways) port_log("gx: GXV_SKIP_ALL=1: frames are not rasterised (only captured ones are drawn)");
#endif
#if defined(__vita__)
    sPerfSeconds = PORT_LOGGING ? static_cast<int>(vgx::EnvInt("GXV_PERF", 5)) : 0;
    StartFreezeWatch();
#else
    sPerfSeconds = PORT_LOGGING ? static_cast<int>(vgx::EnvInt("GXV_PERF", 0)) : 0;
#endif
    vgx::SetTexturePatchHook(port_handheld_texture, port_handheld_texture_needs_rgba);
#if defined(__vita__)
    // vitaGL's start-up takes ~2 s of the render thread's time and nothing in the game's boot needs it: the thread is started and the
    // game goes on (GXV_RENDER_ASYNC=0: wait for it here). Fast paths/settings run first; heavy RAM pack + HD texture pack reads
    // overlap on this thread while vitaGL initializes on the render thread.
    port_vgx_vita_prepare();
    const bool asyncStart = vgx::EnvInt("GXV_RENDER_ASYNC", 1) != 0;
    if (!asyncStart) {
        port_vgx_vita_load_packs();
        vgx::hdtex::Install(&port_log);
    }
    if (!EnsureThread(!asyncStart)) GraphicsFailed(kNoContext);
    if (asyncStart) {
        port_log("gx: the render thread starts the graphics while the game boots");
        port_vgx_vita_load_packs();
        vgx::hdtex::Install(&port_log);
    }
#else
    // HD textures for Mario, the main characters and key items (GXV_HDTEX, vgx_hdtex.h).
    vgx::hdtex::Install(&port_log);
    if (const char* d = getenv("GXV_TEXDUMP"); d && *d) vgx::SetTextureDumpHook(TexDump);
    if (!EnsureThread()) GraphicsFailed(kNoContext);
#endif
    vgx::ResetState();
    vgx::GuestHooks hooks;
    hooks.resolve = &Resolve;
    hooks.peFinish = &PeFinish;
    hooks.peToken = &PeToken;
    hooks.displayCopy = &DisplayCopy;
    vgx::SetGuestHooks(hooks);
    vgx::SetFrameSink(&FrameSink);
    vgx::split::SetThreadInit(&SplitThreadInit);
    vgx::two::SetThreadInit(&TwoCoreThreadInit); // (GXV_FE_TWOCORE; nothing runs while it is 0)
#if defined(__vita__)
    vgx::split::SetThreadTick(&SplitCore3Tick);  // (does nothing unless the finishing thread went to the fourth core)
#endif
    if (const char* sp = vgx::diag::Env("GXV_STATS"); sp && *sp) vgx::stats::SetDetailed(true);
    // GXV_TRACK=0: no memory tracking (every display list decoded and every bound texture
    // hashed on each use): to tell a caching fault from a rendering one.
    if (vgx::EnvInt("GXV_TRACK", 1)) RegisterMemory();
    // The game's EFB: 640 x 456 (every render mode the game sets up uses that height).
    vgx::g.efbW = 640;
    vgx::g.efbH = static_cast<uint16_t>(vgx::EnvInt("GXV_EFB_HEIGHT", 456));
    vgx::QueueSetTarget();
    // GXV_PLAYER=0: the front end runs on the game thread, inside the GX flush (to tell a fault
    // of the hand-over from one of the renderer).
    if (vgx::EnvInt("GXV_PLAYER", 1)) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1u << 20);
        if (pthread_create(&sPlThread, &attr, &PlayerMain, nullptr) == 0) sPlayerOn.store(true);
        pthread_attr_destroy(&attr);
    }
    sActive.store(true);
    port_log("gx: vgx renderer selected (EFB %ux%u, %u frame(s) in flight%s, front end on %s)", vgx::g.efbW, vgx::g.efbH, sMaxInFlight,
             sAdaptDepth ? ", cut to one while the back end is the bottleneck" : "", sPlayerOn.load() ? "its own thread" : "the game thread");
}

#else // !PORT_GX_VGX
#include "vgx_galaxy.h"
extern "C" void port_gx_use_recorder_backend(void);
extern "C" void port_gx_select_backend(void) { port_gx_use_recorder_backend(); }
extern "C" int port_vgx_active(void) { return 0; }
extern "C" size_t port_vgx_consume(const uint8_t*, size_t len) { return len; }
extern "C" int port_vgx_player(void) { return 0; }
extern "C" int port_vgx_seg_idle(void) { return 0; }
extern "C" void port_vgx_stream_setup(uint8_t*, uint8_t*, size_t) {}
extern "C" uint8_t* port_vgx_publish(uint8_t* wr, int) { return wr; }
extern "C" void port_vgx_notify_write(const void*, size_t) {}
extern "C" unsigned long long port_vgx_capture(const char*, int) { return 0; }
extern "C" unsigned long long port_vgx_frames(void) { return 0; }
extern "C" void port_vgx_debug_dump(void) {}
extern "C" void port_vgx_marker(uint32_t) {}
extern "C" void port_vgx_bglayer_view(unsigned, const float*, float, float) {}
extern "C" uint64_t port_gx_recording_frame(void);
extern "C" unsigned long long port_game_frames(void) { return port_gx_recording_frame(); }
#endif

// See port/compat.h.
extern "C" int port_handheld(void) {
#if defined(__vita__) || defined(PORT_ARMHF_HARNESS)
    return 1;
#else
    static const int on = [] { const char* e = getenv("PETARI_HANDHELD"); return e && atoi(e) != 0 ? 1 : 0; }();
    return on;
#endif
}
