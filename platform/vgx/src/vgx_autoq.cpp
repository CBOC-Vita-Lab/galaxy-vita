// Automatic picture quality: the controller and the GPU clock (see vgx_autoq.h for the rules).
#include "../../include/port/ab27.h"   // (ab27)
#include "vgx_autoq.h"
#include "vgx_shed.h"
#include "vgx_common.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>

extern "C" int64_t port_thread_cpu_us(void) __attribute__((weak));
extern "C" unsigned port_vgx_pace_divisor(void) __attribute__((weak));
extern "C" int port_pace_steady30_engaged(void) __attribute__((weak)); // port/perf.cpp: Performance mode's steady 30 (frame_pace.h SteadyCadence)
extern "C" void port_q30lock_picture(uint32_t gpuUs, int gpuKnown, uint32_t frontUs, int frontKnown, int play) __attribute__((weak)); // port/perf.cpp: the Quality lock ladder (port/q30lock.h)
extern "C" void port_rate_picture(uint32_t frontUs, int frontKnown, uint32_t renderUs, int renderKnown, uint32_t gpuUs, int gpuKnown, int play) __attribute__((weak)); // port/perf.cpp: GXV_RATE_AUTO (rate30), a picture's three lanes

#ifdef __VGX_VITA__
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <cstdio>
#include <cstdlib>
// vitaGL's fence (buffers.c, not static): every scene vitaGL ends, the display's included, is ended with it as the
// fragment notification and `value` one higher, so the GPU writes `value` to `*address` when that scene's last pixel is
// done. Weak: a vitaGL without it leaves the address null and there is no GPU clock.
extern "C" {
extern SceGxmNotification query_fence __attribute__((weak));
void port_log(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2))); // (platform/src/os/os_alarm.cpp: the basic log)
// GPU timers of a vitaGL built with GPU_TIMERS (tools/vitagl/gpu_timers.py; the build's include/vitaGL.h does not declare
// them, it is the deps' copy). Weak: they are there exactly when the library has them (its gxm.o then needs rt_gpu.o).
struct VgxRtGpuFrame {
    uint32_t busy_us, span_us, tail_us;
    uint16_t scenes, timed;
}; // (vitaGL's vglRtGpuFrame)
void vglRtSetup(uint32_t flags, int watcher_priority, int watcher_affinity) __attribute__((weak));
void vglRtSetRingGuard(uint32_t enabled) __attribute__((weak));
uint32_t vglRtRingDrops(void) __attribute__((weak));
uint32_t vglRtLastGpuFrame(VgxRtGpuFrame* out) __attribute__((weak));
uint32_t vglRtGpuFrameSize(void) __attribute__((weak)); // (a library that has it says how big its struct is: tools/vitagl/gpu_timers.py; vp30 and older do not)
void vglRtGpuCounters(uint32_t* timed, uint32_t* untimed, uint32_t* late, uint32_t* pending) __attribute__((weak));
}
constexpr uint32_t kVglRtGpuTimers = 0x8; // VGL_RT_GPU_TIMERS
#endif

namespace vgx::autoq {

int TargetFps() {
    // GXV_AUTOQ_TARGET=30 (Quality mode) or 60 (Performance mode) targets whole-picture throughput; anything else (0, the default)
    // keeps the old GPU-only policy and the game CPU sampling off.
    static const int fps = [] {
        const long v = EnvInt("GXV_AUTOQ_TARGET", 0);
        return v == 30 || v == 60 ? static_cast<int>(v) : 0;
    }();
    return fps;
}
bool TargetEnabled() { return TargetFps() != 0; }
bool Target30Enabled() { return TargetFps() == 30; }

namespace {
// One writer (the game thread), bounded snapshot retries on the renderer. Keep
// the publication 32-bit so this does not need a 64-bit atomic lock on ARMv7.
std::atomic<uint32_t> g_gameSeq{0}, g_gameCpu{0}, g_gameTurns{0}, g_gameValid{0}, g_gameFaults{0};
std::atomic<uint32_t> g_pictureWorkUs{0}, g_pictureFaults{0};
int64_t g_gameCpuStart = 0;
// The front end (the player thread: the GX stream's parse, the draws' building) at each picture it finished: its CPU time
// (low 32 bits of microseconds; wrap is intentional) and the pictures counted. One writer, a sequence like the game's.
std::atomic<uint32_t> g_feSeq{0}, g_feCpu{0}, g_fePictures{0};
// Target 60, for the pacing's steady 30 (frame_pace.h SteadyCadence, read on the game thread): the last window's GPU busy time a
// picture, the slowest of the stages that work a picture each on their own thread or unit (front end, renderer, GPU), and whether the
// controller can still make the GPU's work smaller (a step or a smaller size left). 0: not known. Relaxed: a hint a second old is fine.
std::atomic<uint32_t> g_stageGpuUs{0}, g_stagePipeUs{0}, g_stageLowerable{0};
std::atomic<uint32_t> g_ladderRoom{0};   // (gov5) the Performance ladder has a step to try (or one in proof)
// GXV_PACE23: the back end's scene timers (vgx_backend_gl.cpp gpuscenes) hand over every finished GPU picture's busy time with the display
// scene's wait for its buffer taken out (HonestBusyPublish), and the raw figure beside it. One writer (the back end's thread, in
// Execute) and one reader (the same thread, in FramePresented), so no more than relaxed atomics are needed.
std::atomic<uint32_t> g_honestSeq{0}, g_honestBusy{0}, g_honestRaw{0}, g_honestOn{0};
static_assert(std::atomic<uint32_t>::is_always_lock_free);
}

// GXV_PACE60: the scene timers' split of finished GPU pictures (SplitPublish / SplitNext), one writer and one reader (the back end's thread)
namespace {
constexpr unsigned kSplitRing = 16;
uint32_t g_split[kSplitRing][4] = {};
std::atomic<uint32_t> g_splitHead{0};
uint32_t g_splitTail = 0;
}
void SplitPublish(uint32_t taUs, uint32_t efbUs, uint32_t dispUs, uint32_t busyUs) {
    const uint32_t h = g_splitHead.load(std::memory_order_relaxed);
    if (h - g_splitTail >= kSplitRing) return; // (nobody takes them: drop the newest)
    uint32_t* e = g_split[h % kSplitRing];
    e[0] = taUs; e[1] = efbUs; e[2] = dispUs; e[3] = busyUs;
    g_splitHead.store(h + 1, std::memory_order_release);
}
bool SplitNext(uint32_t out[4]) {
    if (g_splitTail == g_splitHead.load(std::memory_order_acquire)) return false;
    const uint32_t* e = g_split[g_splitTail % kSplitRing];
    for (int i = 0; i < 4; ++i) out[i] = e[i];
    ++g_splitTail;
    return true;
}

void HonestBusyEnable(bool on) { g_honestOn.store(on ? 1u : 0u, std::memory_order_relaxed); }
bool HonestBusyEnabled() { return g_honestOn.load(std::memory_order_relaxed) != 0; }
void HonestBusyPublish(uint32_t busyUs, uint32_t rawUs) {
    g_honestBusy.store(busyUs, std::memory_order_relaxed);
    g_honestRaw.store(rawUs, std::memory_order_relaxed);
    g_honestSeq.fetch_add(1, std::memory_order_release);
}

// The front end finished a picture (its display copy), called on the front end's own thread only when it has one (with the
// front end on the game thread its work is in the game's turns already). One system call a picture, only in target mode.
extern "C" void port_autoq_front_picture() {
    if (!TargetEnabled() || !port_thread_cpu_us) return;
    const int64_t cpu = port_thread_cpu_us();
    if (cpu <= 0) return;
    g_feSeq.fetch_add(1, std::memory_order_acq_rel);
    std::atomic_thread_fence(std::memory_order_release);
    g_feCpu.store(static_cast<uint32_t>(cpu), std::memory_order_relaxed);
    g_fePictures.store(g_fePictures.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    g_feSeq.fetch_add(1, std::memory_order_release);
}

extern "C" int port_autoq_front_totals(uint32_t* cpu, uint32_t* pictures) {
    if (!TargetEnabled()) return 0;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        const uint32_t seq = g_feSeq.load(std::memory_order_acquire);
        if (seq & 1u) continue;
        const uint32_t c = g_feCpu.load(std::memory_order_relaxed), n = g_fePictures.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq != g_feSeq.load(std::memory_order_relaxed)) continue;
        *cpu = c;
        *pictures = n;
        return n != 0;
    }
    return 0;
}

// Target 60 (Performance mode): the stage hint of the last window for the pacing's steady 30 (port/perf.cpp). Returns 1 when known.
extern "C" int port_autoq_stages(uint32_t* gpuUs, uint32_t* pipeUs, int* lowerable) {
    if (TargetFps() != 60) return 0;
    const uint32_t g = g_stageGpuUs.load(std::memory_order_relaxed), p = g_stagePipeUs.load(std::memory_order_relaxed);
    if (!g || !p) return 0;
    *gpuUs = g;
    *pipeUs = p;
    *lowerable = g_stageLowerable.load(std::memory_order_relaxed) != 0;
    return 1;
}

// gov5: the game thread's governor of 60 Assist asks, once a second: is the GPU ladder still working on this stage (a step to try, or one on proof)? 1: it goes first. 0: nothing (more) from it.
extern "C" int port_autoq_ladder_room(void) { return g_ladderRoom.load(std::memory_order_relaxed) != 0; }

extern "C" void port_autoq_game_begin() {
    if (!TargetEnabled()) return;
    g_gameCpuStart = port_thread_cpu_us ? port_thread_cpu_us() : 0;
}

extern "C" void port_autoq_game_work_done() {
    if (!TargetEnabled()) return;
    const int64_t end = port_thread_cpu_us ? port_thread_cpu_us() : 0;
    const bool valid = g_gameCpuStart > 0 && end >= g_gameCpuStart;
    g_gameSeq.fetch_add(1, std::memory_order_acq_rel);
    // A reader observing a new relaxed payload synchronizes its acquire fence with this fence before checking seq again.
    std::atomic_thread_fence(std::memory_order_release);
    if (valid) {
        g_gameCpu.store(g_gameCpu.load(std::memory_order_relaxed) + static_cast<uint32_t>(end - g_gameCpuStart), std::memory_order_relaxed);
        g_gameTurns.store(g_gameTurns.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    } else {
        g_gameFaults.fetch_add(1, std::memory_order_release);
        g_pictureWorkUs.store(0, std::memory_order_release);
    }
    g_gameValid.store(valid, std::memory_order_relaxed);
    g_gameSeq.fetch_add(1, std::memory_order_release);
    g_gameCpuStart = 0;
}

extern "C" int port_autoq_game_cpu_totals(uint32_t* cpu, uint32_t* turns, uint32_t* faults) {
    if (!TargetEnabled()) return 0;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        const uint32_t seq = g_gameSeq.load(std::memory_order_acquire);
        if (seq & 1u) continue;
        const uint32_t c = g_gameCpu.load(std::memory_order_relaxed), t = g_gameTurns.load(std::memory_order_relaxed);
        const uint32_t valid = g_gameValid.load(std::memory_order_relaxed);
        const uint32_t failed = g_gameFaults.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq != g_gameSeq.load(std::memory_order_relaxed)) continue;
        *cpu = c;
        *turns = t;
        *faults = failed;
        return valid != 0;
    }
    return 0;
}

// Optional pacing hint: the 30 Hz feasibility cost, not the cost of three turns
// in a deliberately paced 20 Hz picture. Zero means this signal is unavailable.
extern "C" uint64_t port_autoq_picture_work_us() {
    if (!TargetEnabled()) return 0;
    const uint32_t faults = g_gameFaults.load(std::memory_order_acquire);
    const uint32_t stamp = g_pictureFaults.load(std::memory_order_acquire);
    const uint32_t work = g_pictureWorkUs.load(std::memory_order_acquire);
    // Reject a publication crossing a game-clock failure, including an older renderer window published after it.
    return faults == stamp && stamp == g_pictureFaults.load(std::memory_order_acquire) &&
        faults == g_gameFaults.load(std::memory_order_acquire) ? work : 0;
}

__attribute__((format(printf, 1, 2))) void Log(const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
#ifdef __VGX_VITA__
#if defined(PORT_RUNTIME_DIAGNOSTICS) && !PORT_RUNTIME_DIAGNOSTICS
    // (the player build: port_log writes nothing, but a change of the picture size belongs in its basic log)
    if (port_log_summary) {
        port_log_summary("%s", line);
        return;
    }
#endif
    if (port_log) {
        port_log("%s", line);
        return;
    }
#endif
    std::fprintf(stderr, "%s\n", line);
}

__attribute__((format(printf, 1, 2))) void LogSummary(const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
#ifdef __VGX_VITA__
    if (port_log_summary) {
        port_log_summary("%s", line);
        return;
    }
    if (port_log) {
        port_log("%s", line);
        return;
    }
#endif
    std::fprintf(stderr, "%s\n", line);
}

const char* WhyText(Why w) {
    switch (w) {
    case Why::LadderDown: return "the GPU is the limit (Performance ladder: cheaper picture, 896x512, 848x480, edge smoothing off)";
    case Why::LadderUp: return "the GPU has room at the next step up (Performance ladder, predicted from its measured time)";
    case Why::Headroom: return "the GPU has room";
    case Why::Overload: return "the GPU is near its limit";
    case Why::Saturated: return "the GPU is the limit";
    case Why::GpuBound: return "the GPU is the limit at today's size";
    case Why::NoGain: return "the smaller picture bought no speed";
    case Why::NewScene: return "a new scene (after a load) starts at today's size";
    case Why::Forced: return "forced";
    case Why::NoMemory: return "video memory is short";
    case Why::FxOn: return "the GPU is the limit at today's size: a cheaper picture before a smaller one";
    case Why::FxOff: return "room again: back at full quality";
    case Why::FxNoGain: return "the step bought no speed";
    case Why::AnisoOn: return "the GPU has room: anisotropic filtering on";
    case Why::AnisoOff: return "the GPU is the limit: anisotropic filtering goes first";
    case Why::Wipe: return "a fade, wipe or load leads into an unmeasured area: today's size at most there";
    case Why::FreeDown: return "the GPU sets the picture rate above today's size";
    case Why::Severe: return "the GPU is far over the budget in play (severe overload): what it needs at once";
    case Why::HoldMiss: return "pictures came late for the GPU in two windows in a row (Quality: judged by pictures on time)";
    case Why::HoldClimb: return "every picture on time with the GPU under the budget: a trial one size up";
    case Why::HoldBack: return "the size tried lost pictures to the GPU: back, and that size blocked";
    case Why::HoldEdgeOff: return "pictures came late for the GPU in two windows in a row: the present's edge smoothing goes before the size";
    case Why::HoldEdgeOn: return "every picture on time and the edge smoothing's measured cost fits: on again (a trial)";
    case Why::HoldEdgeBack: return "edge smoothing on again lost pictures to the GPU: off, and the next try blocked";
    case Why::CadenceUp: return "on the steady 30 (60 out of reach at any size) the GPU has room at 33.3 ms: one size up, on trial";
    case Why::CadenceBack: return "the size tried on the steady 30 lost pictures to the GPU: back, and that size blocked";
    case Why::ShedEdgeOff: return "shed: the edge pass goes first (the cheapest thing to lose; direct display with it at the native size)";
    case Why::ShedEdgeOn: return "shed: calm, the edge pass is back";
    case Why::ShedCutOn: return "shed: the half-rate captures";
    case Why::ShedCutOff: return "shed: calm, the captures at full rate again";
    case Why::ShedDown: return "shed: the first level of the shed order that fits the GPU's budget";
    case Why::ShedUp: return "shed: calm, one level back up the shed order";
    case Why::ShedBack: return "shed: the level bought too little or lost pictures: back";
    case Why::FastDown: return "pictures are late for the GPU in a sustained burst (Performance): one size down at once";
    case Why::Pace60: return "60 is reachable by the GPU model (pace60): the steps and size it needs, in one change";
    case Why::QgNoGain: return "the quality given up bought no speed (no faster pictures, or the GPU was not the limit): restored, and that step blocked for a while";
    case Why::QgRestore: return "60 is out of reach and the 30 cadence holds: every size and quality step that fits 33.3 ms restored, sharpest first";
    case Why::QgBack: return "the quality restored on the 30 cadence lost pictures to the GPU: back, and the next restore blocked";
    default: return "-";
    }
}

Controller::Controller(const Config& c) : cfg_(c) {
    if (cfg_.targetFps != 30 && cfg_.targetFps != 60) cfg_.targetFps = 0;
    if (cfg_.targetFps == 60) cfg_.budgetMs = std::min(cfg_.budgetMs, 1000.0 / 60.0); // (a picture every retrace: never the 30 budget)
    cfg_.minRung = std::clamp(cfg_.minRung, Target() ? kEmergency : 0, kRungs - 1);
    cfg_.maxRung = std::clamp(cfg_.maxRung, std::max(0, cfg_.minRung), kRungs - 1);
    cfg_.startRung = std::clamp(cfg_.startRung, std::max(0, cfg_.minRung), cfg_.maxRung);
    if (cfg_.windowFrames < 2) cfg_.windowFrames = 2;
    rung_ = cfg_.startRung;
    last_.from = last_.to = rung_;
    if (!Target()) cfg_.fxStep = cfg_.bgStep = cfg_.aniso = false; // (the steps belong to the whole-picture target)
    if (!Target()) { cfg_.freeFrac = 0.0; cfg_.lateRoom = false; } // (play-time quality, too)
    if (!Target() || !cfg_.calm) cfg_.wipe = false;
    if (!Target()) cfg_.overload = cfg_.overloadFx = false;
    if (!cfg_.overload) cfg_.overloadFx = false;
    if (cfg_.targetFps != 30) cfg_.hold30 = false; // (Quality mode's rule: the 30 target only)
    if (cfg_.targetFps != 60) cfg_.cadenceUp = false; // (Performance mode's steady 30 only)
    if (cfg_.targetFps != 60) cfg_.fast60 = false;    // (Performance mode's fast step down only: Quality (30) and Locked 30 never run it)
    cfg_.fast60Pics = std::clamp(cfg_.fast60Pics, 4, static_cast<int>(kFastRing));
    cfg_.fast60Late = std::clamp(cfg_.fast60Late, 1, cfg_.fast60Pics);
    cfg_.fast60ReachFps = std::clamp(cfg_.fast60ReachFps, 31.0, 60.0);
    if (cfg_.targetFps != 60 || !cfg_.edgeStep) cfg_.edgeCheck = false; // (Performance's edge step only; Hold30's edge rule is its own)
    cfg_.cadenceUpFrac = std::clamp(cfg_.cadenceUpFrac, 0.5, 1.0);
    cfg_.cadenceReachFps = std::clamp(cfg_.cadenceReachFps, 31.0, 60.0);
    cfg_.p23GpuMiss = std::clamp(cfg_.p23GpuMiss, 0.5, 1.2);
    cfg_.p23GameShare = std::clamp(cfg_.p23GameShare, 0.5, 1.0);
    cfg_.p23Scale = std::clamp(cfg_.p23Scale, 0.0, 1.0);
    cfg_.p23Fit = std::clamp(cfg_.p23Fit, 0.5, 1.0);
    cfg_.p23MaxChanges = std::clamp(cfg_.p23MaxChanges, 1, 8);
    if (!Target()) cfg_.pace23 = false;
    if (cfg_.targetFps != 60) cfg_.p60 = false; // (GXV_PACE60: Performance mode's question, 60 or an even 30)
    cfg_.p60PixShare = std::clamp(cfg_.p60PixShare, 0.0, 1.0);
    cfg_.p60FitIn = std::clamp(cfg_.p60FitIn, 0.5, 1.2);
    cfg_.p60FitOut = std::clamp(cfg_.p60FitOut, cfg_.p60FitIn, 1.3);
    cfg_.p60BgFrac = std::clamp(cfg_.p60BgFrac, 0.3, 1.0);
    cfg_.p60FxFrac = std::clamp(cfg_.p60FxFrac, 0.3, 1.0);
    cfg_.p60EdgeMs = std::clamp(cfg_.p60EdgeMs, 0.0, 10.0);
    cfg_.p60FxTaMs = std::clamp(cfg_.p60FxTaMs, 0.0, 10.0);
    cfg_.p60MinPics = std::clamp(cfg_.p60MinPics, 4, 600);
    if (cfg_.targetFps != 60) cfg_.qg = false; // (GXV_AUTOQ_QGUARD: Performance mode only; Quality and Locked 30 decide as before)
    if (!cfg_.qg) cfg_.cap30 = false;
    if (cfg_.targetFps != 60) cfg_.shed = false; // (GXV_AUTOQ_SHED: Performance mode only)
    cfg_.shedFit = std::clamp(cfg_.shedFit, 0.5, 1.0);
    cfg_.shedPixShare = std::clamp(cfg_.shedPixShare, 0.0, 1.0);
    cfg_.shedUpFrac = std::clamp(cfg_.shedUpFrac, 0.5, 1.0);
    cfg_.shedDwellSec = std::clamp(cfg_.shedDwellSec, 0.0, 60.0);
    ShedBuild();
    cfg_.qgPlayWindows = std::clamp(cfg_.qgPlayWindows, 0, 200);
    cfg_.qgPlaySec = std::clamp(cfg_.qgPlaySec, 0.0, 60.0);
    cfg_.qgNeedSec = std::clamp(cfg_.qgNeedSec, 0.0, 30.0);
    cfg_.qgReachHoldSec = std::clamp(cfg_.qgReachHoldSec, 0.0, 120.0);
    cfg_.qgTrialSec = std::clamp(cfg_.qgTrialSec, 0.5, 60.0);
    cfg_.qgRateFrac = std::clamp(cfg_.qgRateFrac, 0.0, 0.5);
    cfg_.qgBlockSec = std::clamp(cfg_.qgBlockSec, 1.0, 600.0);
    cfg_.qgRestoreSec = std::clamp(cfg_.qgRestoreSec, 0.4, 60.0);
    cfg_.qgGpuHoldFrac = std::clamp(cfg_.qgGpuHoldFrac, 1.1, 1000.0); // (1000: the rule off)
    cfg_.holdMissWindows = std::max(1, cfg_.holdMissWindows);
    cfg_.holdClimbWindows = std::max(1, cfg_.holdClimbWindows);
    if (cfg_.anisoRung != kNoRung) cfg_.anisoRung = std::clamp(cfg_.anisoRung, kToday, cfg_.maxRung);
    lastFx_.from = lastFx_.to = lastAniso_.from = lastAniso_.to = rung_;
}

void Controller::ResetWindow() {
    busy_ = span_ = present_ = run_ = 0;
    frames_ = busyN_ = 0;
    runKnown_ = true;
    gameRun_ = gameTurns_ = 0;
    frontRun_ = frontPictures_ = 0;
    cadenceFrames_ = 0;
    gameKnown_ = true;
    hintKnown_ = true;
    winPics_ = winLate_ = winLateGpu_ = 0;
    winCadLateGpu_ = 0;
}

void Controller::ResetHolds() {
    overT_ = boundT_ = roomT_ = 0;
    overFrames_ = 0;
    boundSpan_ = 0;
    boundIntervalSum_ = 0;
    boundFrames_ = 0;
    fxRoomT_ = 0;
    anisoRoomT_ = 0;
    freeOverT_ = 0;
    boundGpuSum_ = 0;
    boundGpuFrames_ = 0;
    // (Config::hold30: the hold window and its runs start again: what was measured before a change or a stall is not this picture's)
    holdT_ = holdGpuSum_ = 0;
    holdPics_ = holdLate_ = holdLateGpu_ = holdGpuN_ = 0;
    missRun_ = calmRun_ = 0;
    missGpuSum_ = 0;
    missGpuN_ = 0;
    shedCalmT_ = 0;
    fastN_ = 0; // (Config::fast60: the pictures before a change, a stall or a quiet moment are not those of the next size)
}

void Controller::Block(int rung, uint64_t now) {
    double& len = blockLen_[rung + 1];
    len = len > 0 ? std::min(len * 2.0, cfg_.maxBlockSec) : cfg_.blockSec;
    blockUntil_[rung + 1] = now + static_cast<uint64_t>(len * 1e6);
    lastBlockAt_ = now;
}

// Config::seam (GXV_SEAMLESS_CUT, seamless23): may a step that raises the visible quality be taken now? Yes at a camera cut, a closed fade or the
// pause menu (seamCutMemSec ago at most), or when such a step has been held for seamWaitSec (the wish counts as one while the rules keep asking,
// a 3 s pause in the asking starts it afresh). Reductions are never asked here.
bool Controller::SeamHold(uint64_t now) {
    if (!cfg_.seam) return false;
    if (seamLastCutUs_ != 0 && now >= seamLastCutUs_ && static_cast<double>(now - seamLastCutUs_) <= cfg_.seamCutMemSec * 1e6) {
        if (seamSince_) ++seamStats_.atCut;
        seamSince_ = 0;
        return false;
    }
    if (seamSince_ == 0 || now < seamLastAsk_ || now - seamLastAsk_ > 3000000u) seamSince_ = now;
    seamLastAsk_ = now;
    if (static_cast<double>(now - seamSince_) >= cfg_.seamWaitSec * 1e6) {
        ++seamStats_.timedOut;
        seamSince_ = 0;
        return false;
    }
    ++seamStats_.held;
    return true;
}

// Config::seam72 (GXV_SEAM72): a size step down taken in play waits for a quiet moment too; none for seam72WaitSec: the size is kept (true = hold).
bool Controller::SeamHoldDown(uint64_t now) {
    if (!cfg_.seam72) return false;
    if (seamLastCutUs_ != 0 && now >= seamLastCutUs_ && static_cast<double>(now - seamLastCutUs_) <= cfg_.seamCutMemSec * 1e6) {
        ++seam72Stats_.quiet;
        seam72Since_ = 0;
        return false;
    }
    if (seam72Since_ == 0 || now < seam72LastAsk_ || now - seam72LastAsk_ > 3000000u) seam72Since_ = now;
    seam72LastAsk_ = now;
    ++seam72Stats_.asked;
    if (static_cast<double>(now - seam72Since_) >= cfg_.seam72WaitSec * 1e6) {
        ++seam72Stats_.kept;
        seam72Since_ = now;
        Log("[seam72] size step down at rung %d NOT taken in play: no quiet moment for %.0f s; the size is kept", rung_, cfg_.seam72WaitSec);
    }
    return true;
}

void Controller::SetRung(int to, Why why, uint64_t now) {
    to = std::clamp(to, cfg_.minRung, cfg_.maxRung);
    if (presetActive_ && preset_.twoSize && to > cfg_.minRung && to < cfg_.maxRung) to = to > rung_ ? cfg_.maxRung : cfg_.minRung; // (stagepreset25: two sizes: no rung between)
    if (to == rung_) return;
    if (presetActive_ && preset_.noSizeUp && why != Why::NewScene && to < rung_) cfg_.maxRung = to; // (stagepreset25: size down only: the ceiling follows)
    if (QG() && to < rung_ && QgJudged(why)) QgNoteDown(now); // (Config::qg: a smaller size is judged by the pictures it buys)
    // (Config::hold30: every climb is a trial for holdTrialSec, judged by the pictures; any step down ends a trial)
    if (Hold30() && to > rung_ && why != Why::NewScene) {
        holdTrialFrom_ = rung_;
        holdTrialUntil_ = now + static_cast<uint64_t>(cfg_.holdTrialSec * 1e6);
    } else {
        holdTrialFrom_ = kNoRung;
        holdTrialUntil_ = 0;
    }
    edgeMeasuring_ = false; // (Config::hold30: an edge saving is measured at one size only)
    // (Config::cadenceUp: any change ends a climb's trial on the steady 30; Climb starts one after a climb it allowed)
    cadTrialFrom_ = kNoRung;
    cadTrialUntil_ = 0;
    cadTrialPics_ = cadTrialLate_ = 0;
    cadTrialMiss_ = false;
    last_ = Change{rung_, to, why, winLoad_, winCpu_, winIntervalMs_, now};
    rung_ = to;
    ++changes_;
    sinceChange_ = 0;
    if (P23() && why != Why::NewScene && why != Why::NoMemory) P23NoteChange(now);
    settleUntil_ = now + static_cast<uint64_t>(cfg_.settleSec * 1e6);
    pendingUp_ = 0;
    roomAt_ = 0;
    pendingBelow_ = false;
    if (why == Why::NewScene) pendingUndo_ = -1;
    if (why == Why::NewScene || (pathFrom_ != kNoRung && to >= pathFrom_)) pathFrom_ = kNoRung; // (the series of steps below is over)
    // (target mode: a change to the rung an undo was waiting for, or above it, has done the undo's work)
    if (Target() && pendingUndo_ >= 0 && to >= pendingUndo_) pendingUndo_ = -1;
    if (aniso_ && (why == Why::NewScene ? !AnisoRungOk(to) : !AnisoKeptAt(to))) {
        // (it belongs to the top rung: any other has it off; with Config::anisoRung any change of rung has it off, and a new scene
        // keeps what it remembered for a rung it may be on at)
        aniso_ = false;
        ++anisoChanges_;
        anisoAt_ = now;
        lastAniso_ = Change{last_.from, to, why, winLoad_, winCpu_, winIntervalMs_, now};
    }
    if (steps_ && to > FxRung() && why != Why::NewScene) {
        // (the steps belong to today's rung and below: a rung above has them off)
        steps_ = 0;
        trial_ = 0;
        ++fxChanges_;
        lastFx_ = Change{last_.from, to, Why::FxOff, winLoad_, winCpu_, winIntervalMs_, now};
    }
    ResetWindow();
    ResetHolds();
}

// The next step at today's rung (0: none left, or blocked): the present without edge smoothing first (Config::edgeStep: no texture
// or effect of the picture changes, only the present's filter), then the cheaper background, then half-size effects and blur.
unsigned Controller::NextStep(uint64_t now) const {
    if (presetActive_ && preset_.fx == 2) return 0; // (stagepreset25: the preset holds the effects)
    // (Config::hold30: edge smoothing has its own rule at any rung, HoldEdgeRule, and is not one of the steps at today's rung)
    if (cfg_.edgeStep && !HoldEdgeRule() && !ShedOn() && !(steps_ & 4) && now >= onBlockedUntil_[2]) return 4;
    if (cfg_.bgStep && !(steps_ & 2) && now >= onBlockedUntil_[1]) return 2;
    if (cfg_.fxStep && !(steps_ & 1) && now >= onBlockedUntil_[0]) return 1;
    return 0;
}

// The way back is the reverse: half-size effects off first, then the cheaper background, then edge smoothing on again.
unsigned Controller::RecoveryStep() const {
    return (steps_ & 1) ? 1u : (steps_ & 2) ? 2u : (steps_ & 4) ? 4u : 0u;
}

double Controller::DownBudgetMs() const {
    return CadenceWindow() ? std::max(cfg_.budgetMs, 1000.0 / 30.0) : cfg_.budgetMs;
}

// A step up the stretch of room allowed. When a window of that stretch had room only against the steady 30's 33.3 ms (Config::cadenceUp),
// the climb goes no higher than today's rung and is a trial for holdTrialSec: Evaluate takes it back, and blocks the rung tried, when its
// pictures come late for the GPU on the cadence.
bool Controller::Climb(int to, uint64_t now) {
    if (to > rung_ && ShedOwnsRung()) return false; // (Config::shed: the table's own way back, ShedUpStep)
    if (to > rung_ && SeamHold(now)) return false; // (Config::seam: a bigger size at a cut; the room goes on counting, the rule asks again)
    const bool cadence = cfg_.cadenceUp && roomCadence_;
    if (cadence) to = std::min(to, std::max(rung_, kToday));
    const int from = rung_;
    SetRung(to, cadence ? Why::CadenceUp : Why::Headroom, now);
    if (cadence && rung_ > from) {
        cadTrialFrom_ = from;
        cadTrialUntil_ = now + static_cast<uint64_t>(cfg_.holdTrialSec * 1e6);
    }
    return true;
}

void Controller::SetSteps(unsigned to, Why why, uint64_t now) {
    if (steps_ == to) return;
    if (QG() && (to & ~steps_) != 0 && QgJudged(why)) QgNoteDown(now); // (Config::qg: a step on is judged by the pictures it buys)
    lastFx_ = Change{rung_, rung_, why, winLoad_, winCpu_, winIntervalMs_, now};
    steps_ = to;
    ++fxChanges_;
    fxEver_ = true;
    lastFxAt_ = now;
    sinceChange_ = 0;
    settleUntil_ = now + static_cast<uint64_t>(cfg_.settleSec * 1e6);
    pendingUp_ = 0;
    minUp_ = 0;
    roomAt_ = 0;
    pendingBelow_ = false;
    trial_ = 0;
    ResetWindow();
    ResetHolds();
}

// Config::hold30 with edgeStep: the present's edge smoothing off or on again (no render target is made: the windows restart as for a step)
void Controller::SetHoldEdge(bool off, Why why, uint64_t now) {
    if (holdEdge_ == off) return;
    lastFx_ = Change{rung_, rung_, why, winLoad_, winCpu_, winIntervalMs_, now};
    holdEdge_ = off;
    ++fxChanges_;
    fxEver_ = true;
    lastFxAt_ = now;
    sinceChange_ = 0;
    settleUntil_ = now + static_cast<uint64_t>(cfg_.settleSec * 1e6);
    pendingUp_ = 0;
    minUp_ = 0;
    roomAt_ = 0;
    pendingBelow_ = false;
    ResetWindow();
    ResetHolds();
}

// A new scene starts with the steps as the scene last had them (off for a scene not seen): applied with the rung, in the load.
void Controller::SetSceneSteps(unsigned to, uint64_t now) {
    if (steps_ != to) {
        lastFx_ = Change{rung_, rung_, to > steps_ ? Why::FxOn : Why::FxOff, winLoad_, winCpu_, winIntervalMs_, now};
        steps_ = to;
        ++fxChanges_;
        ResetWindow();
        ResetHolds();
    }
    trial_ = 0;
    stepGain_[0] = stepGain_[1] = stepGain_[2] = 0;
}

// Anisotropic filtering on or off at the top rung. Nothing is made again for it (no render target), so the windows restart but
// no settling is needed beyond the first quarter second; the next change is at least 3 s away.
void Controller::SetAniso(bool to, Why why, uint64_t now) {
    if (aniso_ == to) return;
    if (QG() && !to && QgJudged(why)) QgNoteDown(now); // (Config::qg: anisotropic filtering off is judged like any reduction)
    lastAniso_ = Change{rung_, rung_, why, winLoad_, winCpu_, winIntervalMs_, now};
    aniso_ = to;
    ++anisoChanges_;
    anisoAt_ = now;
    // (diagnostics: its GPU cost, the windows of checkSec after it came on against the last window before)
    anisoMeasuring_ = to && Target() && lastGpuMs_ > 0.0;
    anisoBefore_ = lastGpuMs_;
    anisoSum_ = anisoT_ = 0;
    anisoFrames_ = 0;
    sinceChange_ = 0;
    settleUntil_ = now + static_cast<uint64_t>(cfg_.settleSec * 1e6);
    ResetWindow();
    ResetHolds();
}

// A new scene starts with it as the scene last had it (applied with the rung, in the load).
void Controller::SetSceneAniso(bool to, uint64_t now) {
    if (aniso_ == to) return;
    lastAniso_ = Change{rung_, rung_, Why::NewScene, winLoad_, winCpu_, winIntervalMs_, now};
    aniso_ = to;
    ++anisoChanges_;
    ResetWindow();
    ResetHolds();
}

void Controller::ForgetFxBlocks() {
    qgGpuNoGainN_ = 0;
    onBlockedUntil_[0] = onBlockedUntil_[1] = onBlockedUntil_[2] = fxOffBlockedUntil_ = 0;
    onBlockLen_[0] = onBlockLen_[1] = onBlockLen_[2] = fxOffBlockLen_ = 0;
    anisoBlockedUntil_ = 0;
    anisoBlockLen_ = 0;
    // (Config::qg: a new scene judges its reductions afresh)
    qgBlockLen_ = 0;
    qgAnisoKeepUntil_ = 0;
    qgRestoreBlockedUntil_ = 0;
    qgRestoreBlockLen_ = 0;
    // (Config::hold30: a new scene starts with the present's edge smoothing on, unmeasured, unblocked)
    if (ShedOn() && shedCut_) { shedCut_ = false; vgx::shed::g_cutGate.store(0, std::memory_order_relaxed); }
    shedJudge_ = -1;
    shedTrialFrom_ = kNoRung;
    for (double& l : shedBlockLen_) l = 0;
    for (uint64_t& b : shedBlockedUntil_) b = 0;
    holdEdge_ = edgeMeasuring_ = false;
    edgeSave_ = -1;
    edgeTrialUntil_ = edgeBlockedUntil_ = 0;
    edgeBlockLen_ = 0;
}

void Controller::Remember(uint32_t key, int rung, unsigned fx, int floor) {
    if (key == 0) return;
    for (SceneRung& m : sceneMem_)
        if (m.key == key) { m.rung = static_cast<int8_t>(rung); m.fx = static_cast<uint8_t>(fx); m.floor = static_cast<int8_t>(floor); return; }
    sceneMem_[sceneNext_ % 32] = SceneRung{key, static_cast<int8_t>(rung), static_cast<uint8_t>(fx), static_cast<int8_t>(floor)};
    ++sceneNext_;
}

int Controller::Recall(uint32_t key, unsigned* fx, int* floor) const {
    if (fx) *fx = 0;
    if (floor) *floor = kNoRung;
    if (key == 0) return kNoRung;
    for (const SceneRung& m : sceneMem_)
        if (m.key == key) {
            if (fx) *fx = m.fx;
            if (floor) *floor = m.floor;
            return m.rung;
        }
    return kNoRung;
}

// A scene starts with what an earlier visit learnt about steps below today: below the rung where one bought nothing, none is
// taken for maxBlockSec (target mode; elsewhere nothing is ever learnt).
void Controller::RecallFloor(uint32_t key, uint64_t now) {
    int floor = kNoRung;
    Recall(key, nullptr, &floor);
    sceneFloor_ = Target() ? floor : kNoRung;
    sceneFloorUntil_ = sceneFloor_ != kNoRung ? now + static_cast<uint64_t>(cfg_.maxBlockSec * 1e6) : 0;
}

// A step below today that bought nothing goes back (the quiet moment, or in target mode noGainUndoSec without one). In target
// mode the block on steps below counts from here: the smaller picture was on screen until now, and a block that ran out while
// waiting for a quiet moment let the next step down follow the undo within seconds.
void Controller::UndoBelow(uint64_t now) {
    const int to = pendingUndo_;
    pendingUndo_ = -1;
    pendingBelow_ = false;
    checking_ = false;
    SetRung(to, Why::NoGain, now);
    if (Target()) {
        belowBlockedUntil_ = now + static_cast<uint64_t>(belowBlockLen_ * 1e6);
        lastBlockAt_ = now;
    }
}

// A severe overload in play (Config::overload): everything the GPU time says is needed, in one change. Returns true when the rung changed.
bool Controller::SevereStep(uint64_t now, double meanGpuMs) {
    const double fit = cfg_.overloadFit * SevereBudgetMs();
    if (QG() && now < belowBlockedUntil_ && rung_ > FxRung()) return false; // (Config::qg: a size reduction that bought nothing: none for its block)
    if (aniso_) {
        if (QG()) QgNoteDown(now); // (Config::qg: judged with the rest of this change)
        // (no frozen picture of its own: it goes with the rest; switching it on again is blocked as after an overload)
        anisoBlockLen_ = anisoBlockLen_ > 0 ? std::min(anisoBlockLen_ * 2.0, cfg_.maxBlockSec) : cfg_.blockSec;
        anisoBlockedUntil_ = now + static_cast<uint64_t>(anisoBlockLen_ * 1e6);
        lastAniso_ = Change{rung_, rung_, Why::AnisoOff, meanGpuMs / cfg_.budgetMs, winCpu_, winIntervalMs_, now};
        aniso_ = false;
        ++anisoChanges_;
        anisoAt_ = now;
        meanGpuMs /= cfg_.anisoRatio;
    }
    if (ShedOn() && ShedFind() >= 0) { // (Config::shed: the table's level that fits, not a jump to today's rung)
        const double other = std::max({winGameMs_, winBackendMs_, winFrontMs_, 0.0});
        const int r = ShedDownStep(now, meanGpuMs, other, true);
        if (r != 0) { ovLastAt_ = now; return r == 1 || r == 2; }
    }
    const int from = rung_;
    // the rung the quality steps belong to (today's): a rung above it goes there first
    int to = std::min(rung_, FxRung());
    double predicted = meanGpuMs * Cost(to) / Cost(rung_);
    unsigned steps = to <= FxRung() ? steps_ : 0u;
    // (Config::qg: a step that bought no speed here before is not taken again while its block lasts)
    auto qgOk = [&](int idx) { return !QG() || now >= onBlockedUntil_[idx]; };
    if (predicted > fit && cfg_.edgeStep && !HoldEdgeRule() && !(steps & 4u) && qgOk(2)) {
        steps |= 4u;
        predicted /= stepGain_[2] > 0.0 ? stepGain_[2] : cfg_.edgeRatio;
    }
    if (predicted > fit && cfg_.bgStep && !(steps & 2u) && qgOk(1)) {
        steps |= 2u;
        predicted /= stepGain_[1] > 0.0 ? stepGain_[1] : cfg_.bgRatio;
    }
    if (predicted > fit && (cfg_.fxStep || cfg_.overloadFx) && !(steps & 1u) && qgOk(0)) {
        steps |= 1u;
        predicted /= stepGain_[0] > 0.0 ? stepGain_[0] : cfg_.fxRatio;
    }
    const double perPixel = predicted;
    // (below today's rung only when steps below are not blocked: a step there that bought nothing was undone, and blocks the next)
    // (Config::qg: a size reduction that bought nothing blocks every size step for its block, above today's too)
    const int lowest = QG() && now < belowBlockedUntil_ ? rung_ : now < belowBlockedUntil_ ? std::min(rung_, FxRung()) : cfg_.minRung;
    while (to > lowest && perPixel * Cost(to) / Cost(std::min(rung_, FxRung())) > fit) --to;
    if (to == rung_ && steps == steps_) return false; // (nothing left to give: the floor rung with every step on)
    ovLastAt_ = now;
    winLoad_ = meanGpuMs / cfg_.budgetMs;
    if (steps != steps_) {
        trial_ = 0;
        SetSteps(steps, Why::Severe, now);
    }
    if (to == rung_) return false;
    // (a step below today is judged like any other: kept when the GPU's time fell, undone otherwise; while the floor is missed any
    // real saving counts, missGainFrac)
    const int checkFrom = std::min(from, FxRung());
    if (from > FxRung()) Block(from, now);
    SetRung(to, Why::Severe, now);
    if (to < checkFrom) {
        checking_ = true;
        checkFrom_ = checkFrom;
        checkBefore_ = meanGpuMs;
        checkMissing_ = true;
        checkSpan_ = 0;
        checkT_ = 0;
        checkFrames_ = 0;
        pendingUndo_ = -1;
    } else {
        checking_ = false;
    }
    return true;
}

// ---- stagepreset25: a stage's preset (Controller::StagePreset in vgx_autoq.h). Called by the back end (the render thread, the thread that calls Add) when the
// game starts loading a stage. Only the frame of the rules changes here; the rung and the steps are put where a new scene already puts them (the two
// new-scene paths of Add call ApplyPresetStart and take the start rung), so nothing visible is resized and the scene left keeps the rung it really had.
void Controller::ApplyStagePreset(const StagePreset& p) {
    if (!presetSaved_) {
        savedMin_ = cfg_.minRung;
        savedMax_ = cfg_.maxRung;
        savedStart_ = cfg_.startRung;
        presetSaved_ = true;
    }
    const bool isDefault = p.start == kNoRung && p.minRung == kNoRung && p.maxRung == kNoRung && !p.fixedSize && !p.noSizeUp && !p.edgeOff && !p.cuts && p.fx == 0;
    int mn = p.minRung == kNoRung ? savedMin_ : p.minRung;
    int mx = p.maxRung == kNoRung ? savedMax_ : p.maxRung;
    mn = std::clamp(mn, Target() ? kEmergency : 0, kRungs - 1);
    mx = std::clamp(mx, std::max(0, mn), kRungs - 1);
    int st = p.start == kNoRung ? savedStart_ : p.start;
    st = std::clamp(st, std::max(0, mn), mx);
    if (p.fixedSize) mn = mx = st;
    preset_ = p;
    preset_.minRung = mn;
    preset_.maxRung = mx;
    preset_.start = st;
    presetActive_ = !isDefault;
    ladScene_ = ~0u; ladTrial_ = -1; ladDownT_ = ladRoomT_ = 0;   // (gpu896)
    glow_ = false; glowBlockedUntil_ = 0; glowBlockLen_ = 0;       // (halfglow38)
    if (isDefault) { mn = savedMin_; mx = savedMax_; st = savedStart_; }
    cfg_.minRung = mn;
    cfg_.maxRung = mx;
    cfg_.startRung = st;
    // what the old stage learnt (blocks, trials, a pending undo) is not this stage's: the same forgetting a new scene does
    checking_ = false;
    if (Target()) { pendingUndo_ = -1; pathFrom_ = kNoRung; }
    for (int i = 0; i <= kRungs; ++i) { blockUntil_[i] = 0; blockLen_[i] = 0; }
    belowBlockedUntil_ = 0;
    belowBlockLen_ = 0;
    ForgetFxBlocks();
    if (ShedOn()) {
        ShedBuild(); // (the table follows the frame: its top is the ceiling, its lowest rung the floor)
        shedTrialFrom_ = kNoRung;
        shedJudge_ = -1;
        shedCalmT_ = 0;
        for (int i = 0; i < 16; ++i) { shedBlockedUntil_[i] = 0; shedBlockLen_[i] = 0; }
    }
    Log("[autoq] stage preset %s: rungs %d..%d start %d%s%s%s, effects %s", presetActive_ ? "applied" : "cleared (the configuration's frame back)", cfg_.minRung, cfg_.maxRung,
        cfg_.startRung, preset_.fixedSize ? ", size fixed" : (preset_.noSizeUp ? ", size down only" : ""), !isDefault && preset_.edgeOff ? ", edge pass off" : "",
        !isDefault && preset_.cuts ? ", half-rate captures" : "", preset_.fx == 2 ? "held" : (preset_.fx == 1 ? "down only" : "free"));
}

// ---- gpu896 (21 Nov): the Performance ladder, GPU-limited stages only, two ways -----------------------------------------------------------------------------------
// Owner (21 Nov): "performance could even do one more step down in graphics ... and then lose the AA ... only when it's the gpu that needs it ... I want quick transitions and recovery".
// The levels, from the full picture: [cheaper background] -> [half-size effects and blur] -> 896x512 -> 848x480 -> [the present's edge smoothing off, at the floor] -> (the even 30 is
// the pacing's, as before: with the model saying 60 is out of reach `over` is false and nothing more is taken). A step down needs `over` (the GPU gate open and the GPU the limit:
// gpuBound, the same tests as every other size step) for ladDownHold_; the level chosen is the first whose predicted GPU time (the measured time scaled by pixels, Cost, and the steps'
// ratios) fits p23Fit of the budget, so a boss takes one change, not five. A step up needs the predicted GPU time of the next level up (measured / ratio, the same scaling) within
// ladUpMargin_ of the budget for ladUpHold_ (a few seconds, not the 20 s of the calm rules, and no wait for a camera cut: the change is blended by the back end), taking as many
// levels at once as that margin allows. Hysteresis: the dead band between the down test (GPU at ~95 % of the budget) and the up test (the NEXT level at <= 85 %, a level that costs more
// than the one left); and a step up that is undone within ladTrial_ blocks that entry (Block / onBlockedUntil_, blockSec doubling to maxBlockSec: the rules GXV_AUTOQ_RECOVER already uses).
// A stage that is CPU / game-thread / front-end limited never has `over`, so it never takes a step down; its levels come back up when their cost would fit.
namespace {
RungReadyFn g_rungReady = nullptr;
}
void SetRungReadyHook(RungReadyFn fn) { g_rungReady = fn; }
bool RungReadyNow(int rung) { return !g_rungReady || g_rungReady(rung); }
bool Controller::LadderOn() const { return presetActive_ && preset_.ladder && Target() && !ShedOn(); }
int Controller::LadderTable(LadLevel* t) const {
    int n = 0;
    const int mx = cfg_.maxRung, mn = std::min(cfg_.minRung, cfg_.maxRung);
    const bool hold = preset_.fx == 2;
    unsigned s = 0;
    bool glow = false;
    t[n++] = {mx, 0u, false};
    if (!hold && cfg_.bgStep) { s |= 2u; t[n++] = {mx, s, false}; }
    if (!hold && cfg_.fxStep) { s |= 1u; t[n++] = {mx, s, false}; }
    // halfglow38: after the cheap picture steps, before the first size; not once refused for the stage, not while the back end does not allow it
    if (!hold && cfg_.glowStep && glowAllowed_ && !(ladRefused_ & 0x200u)) { glow = true; t[n++] = {mx, s, true}; }
    for (int r = mx - 1; r >= mn && n < 8; --r) t[n++] = {r, s, glow};
    if (!hold && cfg_.edgeStep) { s |= 4u; t[n++] = {mn, s, glow}; }
    return n;
}
double Controller::LadderCost(const LadLevel& l) const {
    double c = Cost(l.rung);
    for (int idx = 0; idx < 3; ++idx)
        if (l.steps & (1u << (idx == 0 ? 0 : idx == 1 ? 1 : 2))) c /= std::max(stepGain_[idx] > 0.0 ? stepGain_[idx] : StepRatio(idx), 1.0);
    return c;
}
bool Controller::LadderEntryBlocked(const LadLevel* t, int m, uint64_t now) const {   // the change that reaches level m from m+1
    if (t[m].glow != t[m + 1].glow) return now < glowBlockedUntil_;
    if (t[m].rung != t[m + 1].rung) return Blocked(t[m].rung, now);
    const unsigned d = t[m + 1].steps & ~t[m].steps;
    for (int idx = 0; idx < 3; ++idx) if (d & (1u << idx)) return now < onBlockedUntil_[idx];
    return false;
}
void Controller::LadderBlockEntry(const LadLevel* t, int m, uint64_t now) {
    if (t[m].glow != t[m + 1].glow) {
        glowBlockLen_ = glowBlockLen_ > 0 ? std::min(glowBlockLen_ * 2.0, cfg_.maxBlockSec) : cfg_.blockSec;
        glowBlockedUntil_ = now + static_cast<uint64_t>(glowBlockLen_ * 1e6);
        lastBlockAt_ = now;
        return;
    }
    if (t[m].rung != t[m + 1].rung) { Block(t[m].rung, now); return; }
    const unsigned d = t[m + 1].steps & ~t[m].steps;
    for (int idx = 0; idx < 3; ++idx) if (d & (1u << idx)) {
        onBlockLen_[idx] = onBlockLen_[idx] > 0 ? std::min(onBlockLen_[idx] * 2.0, cfg_.maxBlockSec) : cfg_.blockSec;
        onBlockedUntil_[idx] = now + static_cast<uint64_t>(onBlockLen_[idx] * 1e6);
        lastBlockAt_ = now;
    }
}
// "Don't sacrifice graphics if it doesn't help" (owner, after console27): a size step or the edge step is only KEPT when it bought speed.
//  * Before: the predicted saving of the step from the GPU time's pixel-scaled share (the GXV_PACE60 model's split when it is ready: vertex TA, EFB 3D, display 3D; else p23Scale, 60 %).
//    Under ladMinGain_ (5 % of the GPU time, ~0.8 ms of 16.7: 896 at a 60 % pixel share predicts 7 %, 848 13 %) the step is not taken at all (a TA-bound or many-fixed-scenes stage: console27 Beach Bowl, 848x480 still 11.3 ms TA + 20.9 ms 3D).
//  * After: ladChkSec_ (1 s, ~5 windows, the settle 0.25 s not counted) of GPU busy time and picture interval. Kept only when the GPU time fell by at least ladKeepFrac_ (60 %) of the
//    predicted saving AND the pictures rose by 2 a second or reached the budget; else undone at once (the same seamless change back) and the step refused for the rest of the stage,
//    remembered per stage for the session (ladMem_). 60 % because the model's own error is about +-25 % and a step that gives less than that is not a visible trade; 2 fps because the pictures
//    come at whole retraces (a smaller GPU time that moves no picture is a sacrifice for nothing).
double Controller::LadderStepF(unsigned steps) const {
    double f = 1.0;
    for (int idx = 0; idx < 3; ++idx)
        if (steps & (1u << idx)) f /= std::max(stepGain_[idx] > 0.0 ? stepGain_[idx] : StepRatio(idx), 1.0);
    return f;
}
// GPU time at level b predicted from the time measured at level a
double Controller::LadderPredict(const LadLevel& a, const LadLevel& b, double gpuMs) const {
    double r = 1.0;
    if (a.rung != b.rung) {
        double pa = 0, pb = 0;
        if (P60Ready()) { pa = Pace60Predict(a.rung, a.steps); pb = Pace60Predict(b.rung, a.steps); }
        if (pa > 0.1 && pb > 0.0) r = pb / pa;
        else { const double sh = std::clamp(cfg_.p23Scale, 0.0, 1.0); r = (1.0 - sh) + sh * Cost(b.rung) / Cost(a.rung); }
    }
    return gpuMs * r * LadderStepF(b) / LadderStepF(a);
}
// an entry's bit in the refused mask: 1 << (rung of the size it reaches) for a size step, bit 8 for the edge step; 0 for the cheap steps (never refused)
unsigned Controller::LadderEntryBit(const LadLevel* t, int m) const {   // the change that reaches level m+1 from m
    if (t[m].glow != t[m + 1].glow) return 0x200u;
    if (t[m].rung != t[m + 1].rung) return 1u << t[m + 1].rung;
    return (t[m + 1].steps & ~t[m].steps & 4u) ? 0x100u : 0u;
}
// halfglow38: the glow step's own change (no render target, nothing to make: the game reads its two knobs again at the next picture): the windows start again as for a step.
void Controller::SetGlow(bool on, Why why, uint64_t now) {
    if (glow_ == on) return;
    lastFx_ = Change{rung_, rung_, why, winLoad_, winCpu_, winIntervalMs_, now};
    glow_ = on;
    ++fxChanges_;
    fxEver_ = true;
    lastFxAt_ = now;
    sinceChange_ = 0;
    settleUntil_ = now + static_cast<uint64_t>(cfg_.settleSec * 1e6);
    pendingUp_ = 0; minUp_ = 0; roomAt_ = 0; pendingBelow_ = false; trial_ = 0;
    ResetWindow();
    ResetHolds();
    Log("[autoq] ladder: half-rate glow + shadow texture now %s", on ? "on" : "off");
}
// gov5: does the ladder still have a step below the level it is at that is not refused (or one on proof)? What 60 Assist waits for before it takes the stage.
bool Controller::LadderRoomDown() const {
    if (!LadderOn()) return false;
    if (ladChk_.on) return true;
    LadLevel t[9];
    const int n = LadderTable(t);
    int cur = -1;
    for (int i = 0; i < n && cur < 0; ++i) if (t[i].rung == rung_ && t[i].steps == (steps_ & 7u) && t[i].glow == glow_) cur = i;
    for (int i = n - 1; i >= 0 && cur < 0; --i) if (t[i].rung == rung_ && (steps_ & ~t[i].steps & 7u) == 0 && (!glow_ || t[i].glow)) cur = i;
    for (int i = 0; i < n && cur < 0; ++i) if (t[i].rung == rung_) cur = i;
    if (cur < 0) cur = 0;
    return cur + 1 < n && !(LadderEntryBit(t, cur) & ladRefused_);
}
bool Controller::LadderStep(uint64_t now, double dt, bool mayChange, bool over, bool known, double gpuMs) {
    LadLevel t[9];
    if (ladScene_ != scene_) {
        ladScene_ = scene_; ladDownT_ = ladRoomT_ = 0; ladTrial_ = -1; ladChk_.on = false; ladEmaN_ = 0;
        ladRefused_ = 0;
        glow_ = false;   // (halfglow38: a new scene starts with the step off; the back end switches the knobs back at the poll)
        ladSceneRatio_ = 1.0f;
        for (int i = 0; i < 16; ++i) if (ladMemScene_[i] == scene_ && scene_ != 0) { ladRefused_ = ladMemMask_[i]; if (ladMemRatio_[i] > 0.0f) ladSceneRatio_ = ladMemRatio_[i]; }
        ladMinMs_ = [] { const char* e = std::getenv("GXV_AUTOQ_LADDER_MINMS"); return e && *e ? std::atof(e) : 1.0; }();
        ladSizeOver_ = [] { const char* e = std::getenv("GXV_AUTOQ_LADDER_SIZE_OVER"); return e && *e ? std::atof(e) : 1.10; }();
        ladRefuseAllSizes_ = [] { const char* e = std::getenv("GXV_AUTOQ_LADDER_REFUSE_ALL"); return !e || e[0] != '0'; }();
    }
    const int n = LadderTable(t);
    {   // (halfglow38: the step is no longer on offer (refused, HRW pairing, switch off): taken back at once, no proof needed for an undo)
        bool offered = false;
        for (int i = 0; i < n; ++i) offered = offered || t[i].glow;
        if (glow_ && !offered) SetGlow(false, Why::LadderUp, now);
    }
    int cur = -1;
    for (int i = 0; i < n && cur < 0; ++i) if (t[i].rung == rung_ && t[i].steps == (steps_ & 7u) && t[i].glow == glow_) cur = i;
    for (int i = n - 1; i >= 0 && cur < 0; --i) if (t[i].rung == rung_ && (steps_ & ~t[i].steps & 7u) == 0 && (!glow_ || t[i].glow)) cur = i;
    for (int i = 0; i < n && cur < 0; ++i) if (t[i].rung == rung_) cur = i;
    if (cur < 0) cur = 0;
    const double iv = winIntervalMs_;
    // the proof of the last step down
    if (ladChk_.on) {
        ladChk_.t += dt;
        ladChk_.gpu += gpuMs * frames_;
        ladChk_.iv += iv * frames_;
        ladChk_.frames += frames_;
        if (ladChk_.t >= ladChkSec_ && ladChk_.frames > 0) {
            ladChk_.on = false;
            const double after = ladChk_.gpu / ladChk_.frames, afterIv = ladChk_.iv / ladChk_.frames;
            const double gain = ladChk_.before - after, want = ladChk_.before - ladChk_.pred;
            const double fpsB = ladChk_.beforeIv > 0 ? 1000.0 / ladChk_.beforeIv : 0.0, fpsA = afterIv > 0 ? 1000.0 / afterIv : 0.0;
            const bool kept = gain >= ladKeepFrac_ * want && (fpsA - fpsB >= 2.0 || afterIv <= cfg_.budgetMs * 1.03);
            Log("[autoq] ladder: %s bought %.1f ms of GPU (predicted %.1f), pictures %.1f -> %.1f a second: %s", ladChk_.what, gain, want, fpsB, fpsA, kept ? "kept" : "undone, refused for this stage");
            if ((ladChk_.bits & 0xFFu) && want > 0.05) {   // (seam68: a size trial: its measured share of the prediction is the stage's from now on)
                const float ratio = static_cast<float>(std::clamp(gain / want, 0.0, 1.5));
                ladSceneRatio_ = std::max(0.02f, ratio);
                bool put = false;
                for (int i = 0; i < 16 && !put; ++i) if (ladMemScene_[i] == scene_ && scene_ != 0) { ladMemRatio_[i] = ladSceneRatio_; put = true; }
                for (int i = 0; i < 16 && !put; ++i) if (ladMemScene_[i] == 0 && scene_ != 0) { ladMemScene_[i] = scene_; ladMemMask_[i] = ladRefused_; ladMemRatio_[i] = ladSceneRatio_; put = true; }
            }
            if (!kept) {
                ladRefused_ |= ladChk_.bits;
                if (ladRefuseAllSizes_ && (ladChk_.bits & 0xFFu)) {   // (seam68: the GPU time does not fall with the pixels here: no other size is tried either, in this stage)
                    ladRefused_ |= 0xFFu;
                    Log("[autoq] ladder: no size step is tried again in this stage (the picture size did not buy its prediction)");
                }
                bool stored = false;
                for (int i = 0; i < 16 && !stored; ++i) if (ladMemScene_[i] == scene_) { ladMemMask_[i] = ladRefused_; stored = true; }
                for (int i = 0; i < 16 && !stored; ++i) if (ladMemScene_[i] == 0) { ladMemScene_[i] = scene_; ladMemMask_[i] = ladRefused_; stored = true; }
                if (!stored) { ladMemScene_[ladMemNext_ % 16] = scene_; ladMemMask_[ladMemNext_ % 16] = ladRefused_; ladMemRatio_[ladMemNext_ % 16] = ladSceneRatio_; ++ladMemNext_; }
                ++ladUndone_;
                const int back = ladChk_.fromLevel;
                if ((steps_ & 7u) != t[back].steps) SetSteps((steps_ & ~7u) | t[back].steps, Why::LadderUp, now);
                if (rung_ != t[back].rung) SetRung(t[back].rung, Why::LadderUp, now);
                SetGlow(t[back].glow, Why::LadderUp, now);
                ladDownT_ = ladRoomT_ = 0;
                ladTrial_ = -1;
                ladEmaN_ = 0;
                return true;
            }
            ladEmaN_ = 0;
        }
        return false;   // (nothing else is decided while a step is being proved)
    }
    if (known && gpuMs > 0.0) {   // (the before-values: a short average of the windows since the last change)
        ladEmaGpu_ = ladEmaN_ ? 0.6 * ladEmaGpu_ + 0.4 * gpuMs : gpuMs;
        ladEmaIv_ = ladEmaN_ ? 0.6 * ladEmaIv_ + 0.4 * iv : iv;
        ++ladEmaN_;
    }
    // seam49 (GXV_AUTOQ_LADDER_OTHERGATE, default 1): a size step is for a stage the GPU holds. When the game thread, the back end's CPU or the front end takes longer
    // than the GPU's busy time, the GPU's overload verdict (110 % of the budget on its own) is true but a smaller picture gains the pictures nothing: the owner's Beach Bowl
    // (GPU 18.4 ms, front end 21.6 ms, game thread 18.5 ms): 848x480 bought 0.6 ms of GPU and was undone 1.5 s later, two size changes and nine hitches for nothing.
    static const bool otherGate = [] { const char* e = std::getenv("GXV_AUTOQ_LADDER_OTHERGATE"); return !e || e[0] != '0'; }();
    if (otherGate && over && known && winOtherMs_ > 0.0 && gpuMs > 0.0 && gpuMs < winOtherMs_ * 1.05) {
        if (now - static_cast<uint64_t>(ladOtherLogAt_) > 10000000ull || ladOtherLogAt_ == 0) {
            ladOtherLogAt_ = static_cast<double>(now);
            Log("[autoq] ladder: no size step: the GPU's %.1f ms is not what holds the pictures (game thread, back end or front end %.1f ms)", gpuMs, winOtherMs_);
        }
        over = false;
    }
    ladDownT_ = over ? ladDownT_ + dt : 0.0;
    if (ladTrial_ >= 0 && now >= ladTrialUntil_) ladTrial_ = -1;
    if (over && ladTrial_ >= 0 && cur == ladTrial_ && cur + 1 < n) { for (int m = cur; m < std::min(ladTrialFrom_, n - 1); ++m) LadderBlockEntry(t, m, now); ladTrial_ = -1; }   // (a step up that lost pictures to the GPU: back, and blocked)
    const double budget = cfg_.budgetMs;
    if (over && ladDownT_ >= ladDownHold_ && mayChange && cur + 1 < n) {
        const double base = ladEmaN_ >= 2 ? ladEmaGpu_ : gpuMs;
        // how far down the stage may go: not through a refused entry
        int jmax = cur;
        while (jmax + 1 < n && !(LadderEntryBit(t, jmax) & ladRefused_)) ++jmax;
        // a size / edge entry whose predicted saving is too small is refused without being tried
        for (int m = cur; m < jmax; ++m) {
            const unsigned bit = LadderEntryBit(t, m);
            if (!bit) continue;
            const bool sizeEntry = (bit & 0xFFu) != 0;
            // seam68: a size entry's prediction is scaled by what the stage's earlier size trials really bought (ladSceneRatio_), and must be worth ladMinMs_ ms
            const double saved = (base - LadderPredict(t[m], t[m + 1], base)) * (sizeEntry ? static_cast<double>(ladSceneRatio_) : 1.0);
            if (sizeEntry && ladSizeOver_ > 0.0 && base < ladSizeOver_ * budget) {   // (a size is a visible trade: only for a GPU that is well over, the cheap steps and the 30 are for the margin)
                const uint64_t nowU = now;
                if (nowU - static_cast<uint64_t>(ladSeamLogAt_) > 10000000ull || ladSeamLogAt_ == 0) {
                    ladSeamLogAt_ = static_cast<double>(nowU);
                    Log("[autoq] ladder: smaller picture not taken: the GPU's %.1f ms is under %.0f %% of the %.1f ms budget (a size step is for a GPU well over it)", base, ladSizeOver_ * 100.0, budget);
                }
                jmax = m;
                break;
            }
            if (saved < ladMinGain_ * base || (sizeEntry && saved < ladMinMs_)) {
                Log("[autoq] ladder: %s not taken: predicted saving %.1f ms of %.1f (the GPU time does not scale with the pixels)", t[m + 1].glow != t[m].glow ? "half-rate glow + shadow texture" : t[m + 1].rung != t[m].rung ? "smaller picture" : "edge smoothing off", saved, base);
                ladRefused_ |= bit;
                for (int i = 0; i < 16; ++i) if (ladMemScene_[i] == scene_ || ladMemScene_[i] == 0) { ladMemScene_[i] = scene_; ladMemMask_[i] = ladRefused_; break; }
                jmax = m;
                break;
            }
        }
        if (jmax == cur) { ladDownT_ = 0; return false; }
        int j = cur + 1;
        while (j < jmax && LadderPredict(t[cur], t[j], base) > cfg_.p23Fit * budget) ++j;
        // halfglow38: the glow step is proved on its own: a jump that would carry it on to a size stops at its level (the cheap steps before it ride along)
        for (int m = cur + 1; m < j; ++m) if (t[m].glow && !t[m - 1].glow) { j = m; break; }
        // seam68: a size step lands only on a rung whose every piece is made ahead (the back end made them behind the closed screen); a shallower level, or the cheap steps, otherwise
        while (j > cur && t[j].rung != rung_ && !RungReadyNow(t[j].rung)) {
            if (ladSeamLogAt_ == 0 || now - static_cast<uint64_t>(ladSeamLogAt_) > 10000000ull) {
                ladSeamLogAt_ = static_cast<double>(now);
                Log("[autoq] ladder: rung %d not taken: its targets are not all made ahead (none is made in play)", t[j].rung);
            }
            --j;
        }
        if (j == cur) { ladDownT_ = 0; return false; }
        if (t[j].rung != rung_ && SeamHoldDown(now)) {
            // seam72: hold only the size change for a quiet moment; cheap same-rung steps (glow/edge/steps) still take effect now
            while (j > cur && t[j].rung != rung_) --j;
            if (j == cur) return false;
        }
        const int fromRung = rung_;
        unsigned bits = 0;
        for (int m = cur; m < j; ++m) bits |= LadderEntryBit(t, m);
        const double pred = LadderPredict(t[cur], t[j], base);
        ladDownT_ = ladRoomT_ = 0;
        ladTrial_ = -1;
        if ((steps_ & 7u) != t[j].steps) SetSteps((steps_ & ~7u) | t[j].steps, Why::LadderDown, now);
        if (rung_ != t[j].rung) SetRung(t[j].rung, Why::LadderDown, now);
        SetGlow(t[j].glow, Why::LadderDown, now);
        ++ladDowns_;
        if (bits) {   // (a size or the edge step is on trial)
            ladChk_ = LadChk{};
            ladChk_.on = true; ladChk_.bits = bits; ladChk_.fromLevel = cur; ladChk_.before = base; ladChk_.beforeIv = ladEmaN_ >= 2 ? ladEmaIv_ : iv; ladChk_.pred = pred;
            std::snprintf(ladChk_.what, sizeof ladChk_.what, "%s", bits == 0x200u ? "half-rate glow + shadow" : bits & 0x100u && !(bits & 0xFFu) ? "edge smoothing off" : t[j].rung == 5 ? "896x512" : t[j].rung == 4 ? "848x480" : "step");
        }
        ladEmaN_ = 0;
        return rung_ != fromRung;
    }
    // up: the next level (or more) predicted to fit
    bool room = false;
    int k = cur;
    if (!over && known && cur > 0 && gpuMs > 0.0) {
        while (k > 0 && !LadderEntryBlocked(t, k - 1, now) && LadderPredict(t[cur], t[k - 1], gpuMs) <= ladUpMargin_ * budget &&
               (t[k - 1].rung == rung_ || RungReadyNow(t[k - 1].rung))) --k;   // (seam68: only to a rung made ahead)
        room = k < cur;
    }
    ladRoomT_ = room ? ladRoomT_ + dt : 0.0;
    if (room && ladRoomT_ >= ladUpHold_ && mayChange) {
        const int fromRung = rung_;
        ladRoomT_ = ladDownT_ = 0;
        if (rung_ != t[k].rung) SetRung(t[k].rung, Why::LadderUp, now);
        if ((steps_ & 7u) != t[k].steps) SetSteps((steps_ & ~7u) | t[k].steps, Why::LadderUp, now);
        SetGlow(t[k].glow, Why::LadderUp, now);
        ladTrial_ = k;
        ladTrialFrom_ = cur;
        ladTrialUntil_ = now + static_cast<uint64_t>(ladTrialSec_ * 1e6);
        ++ladUps_;
        ladEmaN_ = 0;
        return rung_ != fromRung;
    }
    return false;
}

// A new scene (the load's gap, or the scene key): the preset's edge pass and captures from the first picture, or the configuration's back after a stage that forced them.
void Controller::ApplyPresetStart(uint64_t now) {
    if (!Target()) return;
    const bool wantEdge = presetActive_ && preset_.edgeOff && cfg_.edgeStep;
    const bool wantCuts = presetActive_ && preset_.cuts && cfg_.shedCuts;
    if (ShedOn()) {
        if (wantEdge != holdEdge_ && (wantEdge || presetForcedEdge_)) SetHoldEdge(wantEdge, wantEdge ? Why::ShedEdgeOff : Why::ShedEdgeOn, now);
        if (wantCuts != shedCut_ && (wantCuts || presetForcedCuts_)) {
            shedCut_ = wantCuts;
            vgx::shed::g_cutGate.store(wantCuts ? 1 : 0, std::memory_order_relaxed);
            lastFx_ = Change{rung_, rung_, wantCuts ? Why::ShedCutOn : Why::ShedCutOff, winLoad_, winCpu_, winIntervalMs_, now};
            ++fxChanges_;
        }
    }
    presetForcedEdge_ = wantEdge;
    presetForcedCuts_ = wantCuts;
}

// ---- GXV_AUTOQ_SHED (gpuheavy25): the shed order as a table of levels. See Config::shed in vgx_autoq.h.
// Levels run from the full picture down; a level is a row {rung, edge pass off, half-rate captures engaged}. The rows below the top keep every
// cheaper thing engaged (the table is cumulative), so moving down several rows at once takes everything in between. The edge pass and the
// captures are switched by the back end between two pictures (the edge pass fades, GXV_SEAMLESS_RAMP; the captures are a flag read by the front
// end); a rung goes through the back end's prepared size change (GXV_SEAMLESS_PREP). Direct display is not a state of its own: a picture is
// shown by flipping the EFB's buffer whenever the edge pass is off at the native size (vgx_directdisplay.h decides per picture).
void Controller::ShedBuild() {
    shedTab_.clear();
    if (!ShedOn()) return;
    const int top = cfg_.maxRung, low = std::max(FxRung(), cfg_.minRung);
    if (top < low) { cfg_.shed = false; return; }
    const bool e = cfg_.edgeStep;
    shedTab_.push_back({top, !e, false, Cost(top), 0.0, "full picture"});
    if (e) shedTab_.push_back({top, true, false, Cost(top), 0.0, "edge pass off"});
    if (cfg_.shedCuts) shedTab_.push_back({top, true, true, Cost(top), 0.0, "half-rate captures"});
    for (int r = top - 1; r >= low && shedTab_.size() < 15; --r) if (!(presetActive_ && preset_.twoSize && r != low)) shedTab_.push_back({r, true, cfg_.shedCuts, Cost(r), 0.0, "smaller picture"});
    if (cfg_.shedCuts) vgx::shed::g_cutGate.store(0, std::memory_order_relaxed); // (armed, not engaged)
}

int Controller::ShedFind() const {
    if (!ShedOn() || steps_ != 0) return -1;
    const bool eo = !cfg_.edgeStep || holdEdge_, cu = !cfg_.shedCuts || shedCut_;
    int best = -1;
    for (size_t i = 0; i < shedTab_.size(); ++i) {
        const ShedRow& r = shedTab_[i];
        if (r.rung != rung_) continue;
        if (best < 0) best = static_cast<int>(i);
        if (r.edgeOff <= eo && r.cut <= cu) best = static_cast<int>(i);
    }
    return best;
}

// The GPU's busy time (ms) at row `to` predicted from `g` measured in the state `from` (a row, or the actual state): the part that is the
// display's (edge pass, direct display) and the captures' is moved separately, the rest falls with the pixels by shedPixShare.
static double ShedPredictRows(const Config& c, double edgeMs, double cutMs, bool edgeA, bool dirA, const Controller::ShedRowView& A, bool edgeB, bool dirB,
                              const Controller::ShedRowView& B, double g) {
    double core = g;
    if (edgeA) core -= edgeMs;
    if (dirA) core += c.shedDirectMs;
    if (A.cut) core += cutMs;
    core += A.extraMs;
    core = std::max(core, 0.1 * g);
    double res = core * (1.0 - c.shedPixShare * (1.0 - B.cost / A.cost));
    if (edgeB) res += edgeMs;
    if (dirB) res -= c.shedDirectMs;
    if (B.cut) res -= cutMs;
    res -= B.extraMs;
    return std::max(res, 0.1 * g);
}

double Controller::ShedPredict(double g, int from, int to) const {
    if (from < 0 || to < 0 || from >= ShedLevels() || to >= ShedLevels() || from == to) return g;
    const ShedRow& A = shedTab_[static_cast<size_t>(from)];
    const ShedRow& B = shedTab_[static_cast<size_t>(to)];
    return ShedPredictRows(cfg_, ShedEdgeMs(), ShedCutMs(), ShedEdgeActive(A), ShedDirect(A), {A.cost, A.cut, A.extraMs}, ShedEdgeActive(B), ShedDirect(B),
                           {B.cost, B.cut, B.extraMs}, g);
}

double Controller::ShedPredictState(double g, int to) const {
    if (to < 0 || to >= ShedLevels()) return g;
    const bool eo = !cfg_.edgeStep || holdEdge_, cu = !cfg_.shedCuts || shedCut_;
    const ShedRow a{rung_, eo, cu, Cost(rung_), 0.0, ""};
    const ShedRow& B = shedTab_[static_cast<size_t>(to)];
    return ShedPredictRows(cfg_, ShedEdgeMs(), ShedCutMs(), ShedEdgeActive(a), ShedDirect(a), {a.cost, a.cut, a.extraMs}, ShedEdgeActive(B), ShedDirect(B),
                           {B.cost, B.cut, B.extraMs}, g);
}

void Controller::ShedBlock(int level, uint64_t now) {
    if (level < 0 || level >= 16) return;
    double& len = shedBlockLen_[level];
    len = len > 0 ? std::min(len * 2.0, cfg_.maxBlockSec) : cfg_.shedBlockSec;
    shedBlockedUntil_[level] = now + static_cast<uint64_t>(len * 1e6);
    lastBlockAt_ = now;
}

// Enter level `to`: the edge pass and the captures are flags (the back end picks them up before the next picture), the rung is a SetRung.
void Controller::ShedEnter(int to, Why why, uint64_t now, double gpuMs, double predicted, bool judge) {
    const int from = ShedFind();
    const ShedRow R = shedTab_[static_cast<size_t>(to)];
    const bool rungChange = R.rung != rung_;
    const bool edgeChange = cfg_.edgeStep && R.edgeOff != holdEdge_;
    const bool cutChange = cfg_.shedCuts && R.cut != shedCut_;
    ShedTx tx;
    tx.active = true;
    tx.from = from;
    tx.to = to;
    tx.what = R.name;
    tx.before = shedRingN_ ? shedRing_[(shedRingHead_ + 7) % 8] : 0.0;
    {
        double v[8];
        const unsigned n = std::min(shedRingN_, 8u);
        for (unsigned i = 0; i < n; ++i) v[i] = shedRing_[(shedRingHead_ + 8 - 1 - i) % 8];
        std::sort(v, v + n);
        tx.median = n ? v[n / 2] : 0.0;
    }
    Log("autoq-shed: level %d (%s) -> %d (%s): %s; GPU %.1f ms, predicted %.1f ms (aim %.1f); rung %d -> %d, edge pass %s, captures %s", from,
        from >= 0 ? shedTab_[static_cast<size_t>(from)].name : "-", to, R.name, WhyText(why), gpuMs, predicted, cfg_.shedFit * cfg_.budgetMs, rung_, R.rung,
        R.edgeOff ? "off" : "on", R.cut ? "half rate" : "full rate");
    if (edgeChange) SetHoldEdge(R.edgeOff, R.edgeOff ? Why::ShedEdgeOff : Why::ShedEdgeOn, now);
    if (cutChange) {
        shedCut_ = R.cut;
        vgx::shed::g_cutGate.store(R.cut ? 1 : 0, std::memory_order_relaxed);
        lastFx_ = Change{rung_, rung_, R.cut ? Why::ShedCutOn : Why::ShedCutOff, winLoad_, winCpu_, winIntervalMs_, now};
        ++fxChanges_;
        fxEver_ = true;
        lastFxAt_ = now;
        sinceChange_ = 0;
        settleUntil_ = now + static_cast<uint64_t>(cfg_.settleSec * 1e6);
        pendingUp_ = 0;
        minUp_ = 0;
        roomAt_ = 0;
        pendingBelow_ = false;
        ResetWindow();
        ResetHolds();
    }
    if (rungChange) SetRung(R.rung, why, now);
    ++shedChanges_;
    shedLastAt_ = now;
    shedTx_ = tx;
    shedTx_.n = 0;
    shedCalmT_ = 0;
    shedJudge_ = -1;
    if (judge && from >= 0) {
        shedJudge_ = from;
        shedJudgeTo_ = to;
        shedJudgeBefore_ = gpuMs;
        shedJudgePred_ = predicted;
        shedJudgeT_ = shedJudgeSum_ = 0;
        shedJudgeN_ = 0;
    }
}

// 0: the table does not own this state; 1: the rung changed; 2: another level; 3: handled, nothing to do (60 out of reach, or nothing saves enough)
int Controller::ShedDownStep(uint64_t now, double gpuMs, double otherMs, bool severe) {
    const int cur = ShedFind();
    if (cur < 0) return 0;
    if (presetActive_ && preset_.fx == 2) return 3; // (stagepreset25: the preset holds the ladder: nothing is given up or taken back)
    const int last = ShedLevels() - 1;
    // a level entered on the way up that lost pictures goes back at once, and is blocked
    if (shedTrialFrom_ != kNoRung && now < shedTrialUntil_ && cur < shedTrialFrom_) {
        const int back = shedTrialFrom_;
        ShedBlock(cur, now);
        shedTrialFrom_ = kNoRung;
        const bool rung = shedTab_[static_cast<size_t>(back)].rung != rung_;
        ShedEnter(back, Why::ShedBack, now, gpuMs, ShedPredictState(gpuMs, back), false);
        return rung ? 1 : 2;
    }
    if (!severe && shedLastAt_ != 0 && now < shedLastAt_ + static_cast<uint64_t>(cfg_.shedDwellSec * 1e6)) return 3; // (the minimum dwell)
    const double reachMs = cfg_.shedReach * cfg_.budgetMs; // (the last level must be predicted to bring the pictures on time: no step down for a 55)
    const double floorMs = ShedPredictState(gpuMs, last);
    if (std::max(floorMs, otherMs) > reachMs) return 3; // (60 out of reach: nothing is given up; the steady 30 is the way)
    const double aim = cfg_.shedFit * cfg_.budgetMs;
    const bool flagsDiffer = [&] {
        const ShedRow& r = shedTab_[static_cast<size_t>(cur)];
        return r.edgeOff != (!cfg_.edgeStep || holdEdge_) || r.cut != (!cfg_.shedCuts || shedCut_);
    }();
    int choice = -1;
    int widest = -1;
    for (int i = flagsDiffer ? cur : cur + 1; i <= last; ++i) {
        if (now < shedBlockedUntil_[i]) continue;
        const double pred = ShedPredictState(gpuMs, i);
        const double gain = gpuMs - pred;
        if (gain < cfg_.shedMinGainMs) continue; // (a level that saves almost nothing is passed over: 960 -> 896 loses direct display)
        widest = i;
        if (pred <= aim) { choice = i; break; }
    }
    if (choice < 0) choice = widest; // (as far as it goes)
    if (choice < 0) return 3;
    const bool rung = shedTab_[static_cast<size_t>(choice)].rung != rung_;
    const Why why = rung ? Why::ShedDown : shedTab_[static_cast<size_t>(choice)].cut && !shedCut_ ? Why::ShedCutOn : Why::ShedEdgeOff;
    if (rung) Block(rung_, now);
    checking_ = false;
    shedTrialFrom_ = kNoRung;
    ShedEnter(choice, why, now, gpuMs, ShedPredictState(gpuMs, choice), true);
    return rung ? 1 : 2;
}

// Judge the level just entered by what it really bought: kept, or undone and blocked.
void Controller::ShedJudgeAdd(uint64_t now, double gpuMs, double dt, uint32_t frames) {
    if (shedJudge_ < 0) return;
    if (ShedFind() != shedJudgeTo_) { shedJudge_ = -1; return; } // (something else moved the state: not ours to judge)
    shedJudgeT_ += dt;
    shedJudgeSum_ += gpuMs * frames;
    shedJudgeN_ += frames;
    if (shedJudgeT_ < cfg_.shedTrialSec || shedJudgeN_ == 0) return;
    const double after = shedJudgeSum_ / shedJudgeN_;
    const double bought = shedJudgeBefore_ - after, wanted = shedJudgeBefore_ - shedJudgePred_;
    const int from = shedJudge_, to = shedJudgeTo_;
    shedJudge_ = -1;
    const ShedRow& A = shedTab_[static_cast<size_t>(from)];
    const ShedRow& B = shedTab_[static_cast<size_t>(to)];
    if (bought >= cfg_.shedGainShare * wanted) {
        if (A.rung == B.rung && A.edgeOff != B.edgeOff && A.cut == B.cut) shedEdgeSave_ = std::max(0.0, bought);
        else if (A.rung == B.rung && A.edgeOff == B.edgeOff && A.cut != B.cut) shedCutSave_ = std::max(0.0, bought);
        Log("autoq-shed: level %d (%s) kept: GPU %.1f -> %.1f ms (predicted %.1f)", to, B.name, shedJudgeBefore_, after, shedJudgePred_);
        return;
    }
    Log("autoq-shed: level %d (%s) bought %.1f ms of the %.1f predicted (GPU %.1f -> %.1f): back to level %d, blocked", to, B.name, bought, wanted, shedJudgeBefore_, after, from);
    ShedBlock(to, now);
    ShedEnter(from, Why::ShedBack, now, after, shedJudgeBefore_, false);
}

// The way back: one level up (or two) when every picture has been on time for shedUpHoldSec and the GPU after it is predicted within shedUpFrac of the
// budget. A camera cut, a closed fade or the pause menu is waited for (SeamHold, Config::seam); the level is a trial for shedBackSec.
bool Controller::ShedUpStep(uint64_t now, double gpuMs, double otherMs, double intervalMs, double dt) {
    const int cur = ShedFind();
    if (cur <= 0 || shedJudge_ >= 0 || pendingUndo_ >= 0 || checking_ || trial_) { shedCalmT_ = 0; return false; }
    if (presetActive_ && preset_.fx >= 1) { shedCalmT_ = 0; return false; } // (stagepreset25: down only: a level given up stays given up until the stage ends)
    const double limit = cfg_.shedUpFrac * cfg_.budgetMs;
    int pick = -1;
    for (int j = std::max(0, cur - 2); j < cur; ++j) {
        if (now < shedBlockedUntil_[j]) continue;
        if (std::max(ShedPredictState(gpuMs, j), otherMs) <= limit) { pick = j; break; }
    }
    const bool onTime = intervalMs <= cfg_.budgetMs * 1.03;
    if (pick < 0 || !onTime) { shedCalmT_ = 0; return false; }
    shedCalmT_ += dt;
    if (shedCalmT_ < cfg_.shedUpHoldSec) return false;
    if (shedLastAt_ != 0 && now < shedLastAt_ + static_cast<uint64_t>(cfg_.shedDwellSec * 1e6)) return false;
    if (SeamHold(now)) return false; // (quality UP waits for a camera cut, a closed fade or the pause menu)
    const bool rung = shedTab_[static_cast<size_t>(pick)].rung != rung_;
    const Why why = rung ? Why::ShedUp : shedTab_[static_cast<size_t>(pick)].cut != shedCut_ ? Why::ShedCutOff : Why::ShedEdgeOn;
    const double pred = ShedPredictState(gpuMs, pick);
    ShedEnter(pick, why, now, gpuMs, pred, false);
    shedTrialFrom_ = cur;
    shedTrialUntil_ = now + static_cast<uint64_t>(cfg_.shedBackSec * 1e6);
    return true;
}

// GXV_AUTOQ_SHED_CYCLE: every shedCycleSec the next level of the table, up and down, whatever the load (the stress run).
bool Controller::ShedCycleStep(uint64_t now) {
    if (shedCycleNext_ == 0) { shedCycleNext_ = now + static_cast<uint64_t>(cfg_.shedCycleSec * 1e6); return false; }
    if (now < shedCycleNext_) return false;
    shedCycleNext_ = now + static_cast<uint64_t>(cfg_.shedCycleSec * 1e6);
    const int cur = ShedFind();
    if (cur < 0) return false;
    int to = cur + shedCycleDir_;
    if (to < 0 || to >= ShedLevels()) { shedCycleDir_ = -shedCycleDir_; to = cur + shedCycleDir_; }
    if (to < 0 || to >= ShedLevels() || to == cur) return false;
    shedTrialFrom_ = kNoRung;
    ShedEnter(to, to > cur ? Why::ShedDown : Why::ShedUp, now, lastGpuMs_, lastGpuMs_, false);
    return true;
}

// The picture intervals around a change, for the log: before, at the switch and the next three; a hitch is over 1.25 x the median before + 2 ms.
void Controller::ShedNotePicture(double intervalMs) {
    ShedTx& t = shedTx_;
    if (t.active && t.n < 4) {
        t.v[t.n++] = intervalMs;
        if (t.n == 4) {
            const bool hitch = std::max(t.v[0], std::max(t.v[1], std::max(t.v[2], t.v[3]))) > 1.25 * t.median + 2.0;
            ++shedTxCount_;
            if (hitch) ++shedHitches_;
            Log("autoq-shed: transition %u (level %d -> %d, %s): picture before %.1f ms (median of the last 8: %.1f), at the switch %.1f, next three %.1f %.1f %.1f; %s", shedTxCount_,
                t.from, t.to, t.what, t.before, t.median, t.v[0], t.v[1], t.v[2], t.v[3], hitch ? "HITCH" : "no hitch");
            t.active = false;
        }
        return;
    }
    shedRing_[shedRingHead_] = intervalMs;
    shedRingHead_ = (shedRingHead_ + 1) % 8;
    if (shedRingN_ < 8) ++shedRingN_;
}

bool Controller::TakeAnisoMeasure(double& beforeMs, double& afterMs) {
    if (!anisoMeasured_) return false;
    anisoMeasured_ = false;
    beforeMs = anisoBeforeMs_;
    afterMs = anisoAfterMs_;
    return true;
}

FloorWindow Controller::TakeFloor() {
    const FloorWindow w = floor_;
    floor_ = FloorWindow();
    return w;
}

// ---- GXV_PACE23 ----------------------------------------------------------------------------------------------------------------
// Why: the 2.2 console logs (crash-victusg-2.2) show the controller stepping down a Performance picture that the GPU did not hold.
//  - "GPU load 0 %" at 28.9 ms a picture and "late for the GPU 96" with a GPU of 6.5-15 ms: after a load the GPU timers report only some
//    pictures, and a late picture with the CPU stages measured and fitting was blamed on the GPU by elimination (the front end's CPU,
//    shader builds and texture uploads are not in those stages);
//  - the timers' busy time runs from a scene's submission to its completion, and the display scene's fragment stage waits for the
//    display to let go of its buffer: on a CPU-paced picture that wait is counted as GPU work (r57: "122 %" in a CPU-paced scene), so
//    the GPU looks like the limit exactly where a smaller picture cannot help;
//  - the census measured the game thread's CPU as one turn, but a picture the skip rule stretched over 2-3 turns is a game thread
//    saturated over all of them.
// The fixes, each behind GXV_PACE23 (Config::pace23): busy time without the display scene's wait (the back end's scene timers,
// HonestBusyPublish; falls back to the timers' own figure), a late picture is the GPU's only when the GPU's busy time alone reaches
// p23GpuMiss of the budget (below that it was waiting for a CPU-paced picture: a smaller size cannot bring it) and the game thread's span CPU
// is under p23GameShare of the interval, no blame by elimination unless the timers
// have been silent for p23ElimPictures pictures, no change decided by the GPU's load for p23SettleSec after a load or a stall, size changes
// p23GapSec apart and p23MaxChanges in p23WindowSec at most, and a step down goes where the GPU's time says in ONE change.
bool Controller::P23ChangeAllowed(uint64_t now, bool count, double gapScale) {
    const uint64_t gap = static_cast<uint64_t>(cfg_.p23GapSec * gapScale * 1e6), win = static_cast<uint64_t>(cfg_.p23WindowSec * 1e6);
    unsigned inWindow = 0;
    uint64_t newest = 0;
    for (unsigned i = 0; i < p23ChgN_; ++i) {
        const uint64_t t = p23Chg_[(p23ChgHead_ + 8 - 1 - i) % 8];
        if (i == 0) newest = t;
        if (now >= t && now - t < win) ++inWindow;
    }
    const bool ok = (p23ChgN_ == 0 || now < newest || now - newest >= gap) && inWindow < static_cast<unsigned>(cfg_.p23MaxChanges);
    if (!ok && count) ++p23_.rateHeld;
    return ok;
}

void Controller::P23NoteChange(uint64_t now) {
    p23Chg_[p23ChgHead_] = now;
    p23ChgHead_ = (p23ChgHead_ + 1) % 8;
    if (p23ChgN_ < 8) ++p23ChgN_;
}

// The GPU's time at size `to` from its time at `from`: only p23Scale of it falls with the pixels (console, Honeyhive r107, 720x404 ->
// 664x376: 25.3/22.3/21.4 -> 23.4/22.5 ms where the pixels alone predict 14 %: scenes, the display pass and the vertex work are per picture).
double Controller::P23Pred(double gpuMs, int from, int to) const {
    return gpuMs * ((1.0 - cfg_.p23Scale) + cfg_.p23Scale * Cost(to) / Cost(from));
}

// ------------------------------------------------------------------ pace60 (GXV_PACE60, Config::p60; vgx_autoq.h "pace60")
// The model: TA (vertex work: the same at any size), the EFB and pass scenes' 3D (falls with the pixels by p60PixShare), the display scene's
// 3D (the present: the same at any size, its edge smoothing or upscaler a step), and the overlap of TA and 3D measured now:
//   busy = max(TA, 3D) + k x min(TA, 3D),  k = (busy - max) / min of the smoothed values
// (a tile GPU runs a scene's 3D after its TA, the next scene's TA beside it: k = 0 is perfect overlap, 1 none). Console, Honeyhive 720x404:
// TA 12.7, 3D 18.9, busy 22.2 -> k = 0.26.
unsigned Controller::Pace60AllSteps() const {
    return (cfg_.edgeStep ? 4u : 0u) | (cfg_.bgStep ? 2u : 0u) | (cfg_.fxStep ? 1u : 0u);
}

void Controller::P60Parts(int rung, unsigned steps, double& ta, double& efb, double& disp) const {
    ta = mTa_;
    efb = mEfb_;
    disp = mDisp_;
    const unsigned now = mSteps_;
    const double a = cfg_.p60PixShare;
    if (mRung_ != kNoRung && rung != mRung_) efb *= (1.0 - a) + a * Cost(rung) / Cost(mRung_);
    if ((steps & 2u) && !(now & 2u)) efb *= cfg_.p60BgFrac;
    else if (!(steps & 2u) && (now & 2u)) efb /= cfg_.p60BgFrac;
    if ((steps & 1u) && !(now & 1u)) { efb *= cfg_.p60FxFrac; ta += cfg_.p60FxTaMs; }
    else if (!(steps & 1u) && (now & 1u)) { efb /= cfg_.p60FxFrac; ta = std::max(0.0, ta - cfg_.p60FxTaMs); }
    if ((steps & 4u) != (now & 4u)) {
        // (edge smoothing off: the display scene as measured without it, else less p60EdgeMs; on: as measured with it, else plus p60EdgeMs)
        if (steps & 4u) disp = mDispOff_ >= 0.0 ? mDispOff_ : std::max(0.3, disp - cfg_.p60EdgeMs);
        else disp = mDispOn_ >= 0.0 ? mDispOn_ : disp + cfg_.p60EdgeMs;
    }
}

double Controller::P60Busy(double ta, double efb, double disp) const {
    const double f0 = mEfb_ + mDisp_, hi0 = std::max(mTa_, f0), lo0 = std::min(mTa_, f0);
    const double k = lo0 > 0.1 ? std::clamp((mBusy_ - hi0) / lo0, 0.0, 1.0) : 1.0;
    const double f = efb + disp;
    return std::max(ta, f) + k * std::min(ta, f);
}

double Controller::Pace60Predict(int rung, unsigned steps) const {
    if (!p60_.ready) return 0.0;
    double ta, efb, disp;
    P60Parts(rung, steps, ta, efb, disp);
    return P60Busy(ta, efb, disp);
}

void Controller::AddSplit(uint32_t taUs, uint32_t efbUs, uint32_t dispUs, uint32_t busyUs) {
    if (!P60() || busyUs == 0 || busyUs > 250000u) return;
    // (Config::qg: the model is filled from play only: a loading screen's or a menu's split (the console: TA 1.5 ms while the stage loaded) is
    // not this scene's, and the pictures after it fill the model afresh)
    if (QG() && qgNonPlay_) { mGap_ = true; return; }
    ++p60_.pictures;
    const unsigned stepsNow = StepsNow();
    if (mGap_) {
        // (a load or a stall: what the model knew was another scene; the next pictures fill it afresh, the verdict stays until they have)
        mGap_ = false;
        mN_ = 0;
        shareFrom_ = kNoRung;
    }
    if (mRung_ != rung_ || mSteps_ != stepsNow) {
        // A change of size or step: the smoothed values are moved to the new state by the model (a measured share of the pixels needs
        // the raw EFB 3D at the new size: summed apart, see below), and the pictures still in flight from the old state are not taken.
        if (p60_.ready && mRung_ != kNoRung) {
            double ta, efb, disp;
            P60Parts(rung_, stepsNow, ta, efb, disp);
            const double busy = P60Busy(ta, efb, disp);
            if (rung_ != mRung_ && mSteps_ == stepsNow && mN_ >= static_cast<unsigned>(cfg_.p60MinPics)) {
                shareEfbBefore_ = mEfb_;
                shareFrom_ = mRung_;
            } else {
                shareFrom_ = kNoRung;
            }
            mTa_ = ta; mEfb_ = efb; mDisp_ = disp; mBusy_ = busy;
        }
        if ((mSteps_ & 4u) != (stepsNow & 4u)) mDispN_ = 0; // (the display scene's cost changed: its minimum anew)
        mRung_ = rung_;
        mSteps_ = stepsNow;
        mSkip_ = 3;
        shareSum_ = 0;
        shareN_ = 0;
    }
    if (mSkip_) { --mSkip_; return; }
    // the display scene's cost without a wait for the display: its recent minimum + 1/8 + 0.3 ms (the present's cost hardly varies; GXV_PACE23's
    // looser cap, + half + 0.5 ms, left ~2.5 ms of a 6 ms wait in the model's display cost)
    mDispRing_[mDispHead_] = dispUs;
    mDispHead_ = (mDispHead_ + 1) % 64u;
    if (mDispN_ < 64u) ++mDispN_;
    uint32_t low = dispUs;
    for (unsigned i = 0; i < mDispN_; ++i) low = std::min(low, mDispRing_[i]);
    const uint32_t cap = mDispN_ >= 8u ? low + low / 8u + 300u : dispUs;
    const uint32_t disp = std::min(dispUs, cap);
    const uint32_t wait = dispUs - disp;
    const double ta = taUs / 1000.0, efb = efbUs / 1000.0, dsp = disp / 1000.0;
    const double busy = (busyUs > wait ? busyUs - wait : 0u) / 1000.0;
    const double al = mN_ == 0 ? 1.0 : std::max(1.0 / (mN_ + 1.0), 1.0 / 24.0);
    mTa_ += al * (ta - mTa_);
    mEfb_ += al * (efb - mEfb_);
    mDisp_ += al * (dsp - mDisp_);
    mBusy_ += al * (std::max(busy, std::max(ta, efb + dsp)) - mBusy_); // (the busy time is never below either phase's own)
    double& side = (stepsNow & 4u) ? mDispOff_ : mDispOn_;
    side = side < 0.0 || mN_ == 0 ? dsp : side + (1.0 / 24.0) * (dsp - side);
    ++mN_;
    if (!p60_.ready && mN_ >= static_cast<unsigned>(cfg_.p60MinPics)) p60_.ready = true;
    p60_.taMs = mTa_;
    p60_.efbMs = mEfb_;
    p60_.dispMs = mDisp_;
    p60_.busyMs = mBusy_;
    {
        const double f = mEfb_ + mDisp_, lo = std::min(mTa_, f);
        p60_.overlap = lo > 0.1 ? std::clamp((mBusy_ - std::max(mTa_, f)) / lo, 0.0, 1.0) : 1.0;
    }
    p60_.edgeMs = mDispOn_ >= 0.0 && mDispOff_ >= 0.0 ? mDispOn_ - mDispOff_ : -1.0;
    // (diagnostics: the share of the EFB 3D that fell with the pixels at the last change of size alone, over 60 pictures at the new size)
    if (shareFrom_ != kNoRung) {
        shareSum_ += efb;
        if (++shareN_ >= 60u) {
            const double ratio = Cost(rung_) / Cost(shareFrom_);
            if (std::fabs(1.0 - ratio) > 0.05 && shareEfbBefore_ > 0.1) {
                p60_.shareMeasured = std::clamp((1.0 - shareSum_ / shareN_ / shareEfbBefore_) / (1.0 - ratio), -1.0, 2.0);
                p60_.shareFrom = shareFrom_;
                p60_.shareTo = rung_;
                Log("[autoq] pace60: the EFB and pass scenes' 3D went %.2f -> %.2f ms a picture from rung %d to %d (pixels x%.2f): %.0f %% of it fell with "
                    "the pixels (assumed %.0f %%, GXV_PACE60_PIXSHARE)", shareEfbBefore_, shareSum_ / shareN_, shareFrom_, rung_, ratio,
                    p60_.shareMeasured * 100.0, cfg_.p60PixShare * 100.0);
            }
            shareFrom_ = kNoRung;
        }
    }
}

// Reachable, off the steady 30, the GPU the limit for p60StepSec: the cheapest combination the model says fits p23Fit (92 %) of the budget, in
// one change: the steps first at this size in their order (edge smoothing, cheaper background, half-size effects), then the sizes down with
// every step. A size below is the existing series of steps below (judged by the GPU's time and the pictures, undone when it buys nothing).
int Controller::P60Step(uint64_t now, double gpuMs) {
    const double fit = cfg_.p23Fit * cfg_.budgetMs;
    const unsigned st = StepsNow();
    unsigned toSteps = st;
    int to = rung_;
    bool found = false;
    for (const unsigned bit : {4u, 2u, 1u}) {
        if (!(Pace60AllSteps() & bit) || (st & bit) || now < onBlockedUntil_[StepIdx(bit)]) continue;
        toSteps |= bit;
        if (Pace60Predict(rung_, toSteps) <= fit) { found = true; break; }
    }
    if (!found && now >= belowBlockedUntil_ && !(sceneFloor_ != kNoRung && rung_ <= sceneFloor_ && now < sceneFloorUntil_)) {
        for (int r = rung_ - 1; r >= cfg_.minRung; --r) {
            to = r;
            if (Pace60Predict(r, toSteps) <= fit) break;
        }
    }
    if (to == rung_ && toSteps == st) return 0;
    ++p60_.direct;
    const int from = rung_;
    if (toSteps != st) {
        fxBefore_ = gpuMs;
        fxBoundKept_ = 0;
        fxOffBlockLen_ = lastFxOffAt_ != 0 && now - lastFxOffAt_ < static_cast<uint64_t>(cfg_.fxRepeatSec * 1e6) && fxOffBlockLen_ > 0
            ? std::min(fxOffBlockLen_ * 2.0, cfg_.maxBlockSec) : cfg_.fxOffBlockSec;
        fxOffBlockedUntil_ = now + static_cast<uint64_t>(fxOffBlockLen_ * 1e6);
        SetSteps(toSteps, Why::Pace60, now);
    }
    if (to != from) {
        if (pathFrom_ == kNoRung) { pathFrom_ = from; pathInterval_ = winIntervalMs_; }
        for (int r = to + 1; r <= from; ++r) Block(r, now); // (the sizes left are too slow: no climb back for blockSec, doubling)
        SetRung(to, Why::Pace60, now);
        checking_ = true;
        checkFrom_ = from;
        checkBefore_ = gpuMs;
        checkMissing_ = true;
        checkSpan_ = 0;
        checkT_ = 0;
        checkFrames_ = 0;
        return 1;
    }
    return 2;
}

// The verdict, once a window: the floor rung with every step on, and the other stages, against the budget with hysteresis.
void Controller::P60Verdict(double dt, double otherMs) {
    if (!P60Ready() || mN_ < static_cast<unsigned>(cfg_.p60MinPics)) return;
    const double floor = P60Floor();
    if (QG()) {
        // (Config::qg: the other stages smoothed over qgNeedSec, seeded with the mean of the play that settled: one window's front end (the
        // console's menus swung it 12 -> 21 ms every few seconds) does not decide the verdict)
        if (qgNeedOther_ < 0.0) qgNeedOther_ = qgSettleOtherN_ ? qgSettleOtherSum_ / qgSettleOtherN_ : otherMs;
        else qgNeedOther_ += (cfg_.qgNeedSec > 0.0 ? std::min(1.0, dt / cfg_.qgNeedSec) : 1.0) * (otherMs - qgNeedOther_);
        otherMs = qgNeedOther_;
    }
    const double need = std::max(floor, otherMs);
    p60_.floorMs = floor;
    p60_.otherMs = otherMs;
    const double b = cfg_.budgetMs;
    bool want;
    if (mFirst_) {
        // (Config::cap30: under the fixed 30 cap no picture comes faster than 33.3 ms: 60 is never reachable, whatever the model)
        want = !cfg_.cap30 && need <= 0.5 * (cfg_.p60FitIn + cfg_.p60FitOut) * b;
        mFirst_ = false;
        reachT_ = 0;
        if (want != p60_.reach) {
            p60_.reach = want;
            ++p60_.verdicts;
        }
        Log("[autoq] pace60: first verdict: 60 %s%s (the floor rung %d with every step: GPU %.1f ms predicted; other stages %.1f ms; TA %.1f, 3D %.1f + display %.1f, busy %.1f, overlap %.2f)",
            p60_.reach ? "reachable" : "out of reach", cfg_.cap30 ? " (the fixed 30 cap)" : "", cfg_.minRung, floor, otherMs, mTa_, mEfb_, mDisp_, mBusy_, p60_.overlap);
        return;
    }
    want = cfg_.cap30 ? false : p60_.reach ? need <= cfg_.p60FitOut * b : need <= cfg_.p60FitIn * b;
    const uint64_t now = prevPresent_;
    if (p60LossAt_ && now > p60LossAt_ + static_cast<uint64_t>(cfg_.p60ForgetSec * 1e6)) { p60_.reachHoldSec = 0; p60LossAt_ = 0; }
    if (want == p60_.reach) { reachT_ = 0; return; }
    reachT_ += dt;
    // (out of reach: p60HoldSec; reachable again: as long as the last quick loss says, doubling: a scene whose GPU swings around the line
    // does not take the 60 path and give it back every few seconds)
    // (Config::qg: "reachable" must hold qgReachHoldSec at least: chasing 60 costs quality, losing it again costs a release of the steady 30)
    if (reachT_ < (want ? std::max({cfg_.p60HoldSec, p60_.reachHoldSec, QG() ? cfg_.qgReachHoldSec : 0.0}) : cfg_.p60HoldSec)) return;
    reachT_ = 0;
    if (want) {
        p60ReachAt_ = now;
    } else if (p60ReachAt_ && now < p60ReachAt_ + static_cast<uint64_t>(cfg_.p60RepeatSec * 1e6)) {
        p60_.reachHoldSec = std::min(std::max(p60_.reachHoldSec * 2.0, 2.0 * cfg_.p60HoldSec), cfg_.p60MaxHoldSec);
        p60LossAt_ = now;
    }
    p60_.reach = want;
    ++p60_.verdicts;
    Log("[autoq] pace60: 60 now %s at rung %d (the floor rung %d with every step: GPU %.1f ms predicted, other stages %.1f ms, against %.1f ms; TA %.1f, 3D %.1f + display %.1f, "
        "busy %.1f, overlap %.2f)", want ? "reachable" : "out of reach", rung_, cfg_.minRung, floor, otherMs, (want ? cfg_.p60FitIn : cfg_.p60FitOut) * b, mTa_, mEfb_,
        mDisp_, mBusy_, p60_.overlap);
}

// ------------------------------------------------------------------ qguard (GXV_AUTOQ_QGUARD, Config::qg; vgx_autoq.h)
// The owner's rule: "If giving up graphics doesn't help anything, we shouldn't be doing it." A reduction of the picture (a smaller size, half-size
// effects, the cheaper background, edge smoothing off, anisotropic filtering off) stays only while it buys pictures. The console (483-c8P,
// Honeyhive, target 60 under the fixed 30 cap, GXV_PACE60): the loading scene's split (TA 1.5 ms, the other stages 4 ms) said "60 reachable";
// in play pace60's direct step took the edge smoothing and the cheaper background off with 608x348 in one change; the size came back ("bought
// no speed") but nothing judged the steps, and every way back for them needs a picture every 16.7 ms (or the steady 30's cadence flag, which
// the fixed cap never sets): the rest of the run was an even 30 with the GPU at 20 ms of 33.3 and both steps on.
// Rules (all target 60 only):
//  - Not play (FrameTimes::loading / noStage, quiet, movie): nothing decided, the model not filled; after it, a load, or a new stage,
//    qgPlayWindows windows and qgPlaySec of play before any verdict or step (QgSettled). The verdict's other stages are smoothed.
//  - Every reduction opens (or, in a series, restarts) a trial with the state and the play before it (QgNoteDown). After qgTrialSec of play
//    it is kept only when the pictures came qgRateFrac faster and the GPU was the stage that limited them; otherwise all of it is restored
//    in one change, each part logged "quality: <step> restored: bought no speed (...)", and that step (or the steps below today) blocked
//    qgBlockSec, doubling (QgJudge).
//  - On a 30 cadence (the steady 30, or Config::cap30) with 60 out of reach, the cadence held, none late for the GPU: room for qgRestoreSec
//    restores in one change every size up to today's and every step the measured GPU time (pace60's model when filled) says fits
//    cadenceUpFrac of 33.3 ms, sharpest first: the size, half-size effects, the cheaper background, edge smoothing, anisotropic filtering
//    (QgRestore). If its pictures then come late for the GPU on the cadence within holdTrialSec, it goes back and the next restore is blocked
//    (qgBlockSec, doubling; QgBack).
void Controller::QgUnsettle() {
    qgPlayWins_ = 0;
    qgPlayT_ = 0;
    qgSettleOtherSum_ = 0;
    qgSettleOtherN_ = 0;
    qgNeedOther_ = -1;
    qgRoomT_ = 0;
    p60OverT_ = 0;
    reachT_ = 0;
    mFirst_ = true; // (pace60: the verdict is formed anew from the play that follows, at once and then with its hysteresis)
    mGap_ = true;   // (... from a model filled by that play only)
    ++qgStats_.unsettles;
}

bool Controller::QgJudged(Why w) {
    switch (w) {
    case Why::NewScene: case Why::Forced: case Why::NoMemory: case Why::NoGain: case Why::FxNoGain: case Why::Wipe: case Why::HoldBack:
    case Why::HoldEdgeBack: case Why::CadenceBack: case Why::QgNoGain: case Why::QgRestore: case Why::QgBack:
        return false; // (a new scene's state, video memory, and the undos and backs of the rules themselves)
    default:
        return true;
    }
}

void Controller::QgNoteDown(uint64_t now) {
    (void)now;
    if (qgInRevert_) return;
    QgTrial& q = qgTrial_;
    if (q.open && q.frames != 0 && q.t >= 0.5 * cfg_.qgTrialSec && q.ivBefore > 0.0 &&
        q.ivSum / q.frames <= q.ivBefore * (1.0 - cfg_.qgRateFrac)) {
        // (the reductions on trial already bought pictures: kept; this one is judged on its own, from the play since)
        ++qgStats_.kept;
        q.open = false;
    }
    if (!q.open) {
        // (the state and the play before the first reduction of a series: a series is judged as a whole against them)
        q.open = true;
        q.rung = rung_;
        q.steps = StepsNow();
        q.aniso = aniso_;
        // (the slower of the smoothed play and its last window: a reduction that answers a scene just got heavier is judged against that)
        q.ivBefore = qgRecent_ ? std::max(qgIv_, qgLastIv_) : winIntervalMs_;
        q.gpuBefore = qgRecent_ ? std::max(qgGpu_, qgLastGpu_) : lastGpuMs_;
        q.otherBefore = qgRecent_ ? qgOther_ : std::max(0.0, std::max({winGameMs_, winBackendMs_, FrontStage() ? winFrontMs_ : -1.0}));
        q.extends = 0;
        ++qgStats_.trials;
    }
    q.t = q.ivSum = q.gpuSum = q.otherSum = 0;
    q.frames = 0;
}

bool Controller::QgOutOfReach(double gpuMs, double otherMs) const {
    if (cfg_.cap30) return true; // (the fixed 30 cap: no picture comes faster than 33.3 ms)
    // (60, not the steady 30's old release at 45: Performance is 60 or an even 30; pace60's 57 a second, the steady 30's release under it)
    const double reachMs = 1000.0 / std::max(cfg_.cadenceReachFps, static_cast<double>(port::pace::kPace23Steady30AboveFps));
    if (P60Ready()) return !p60_.reach || otherMs >= reachMs;
    return gpuMs * Cost(cfg_.minRung) / Cost(rung_) >= reachMs || otherMs >= reachMs;
}

double Controller::QgPredict(int rung, unsigned steps, double gpuMs) const {
    if (P60Ready()) return Pace60Predict(rung, steps); // (pace60's model: measured TA, 3D, display, overlap; without the display's wait)
    double g = gpuMs * Cost(rung) / Cost(rung_);
    const unsigned now = StepsNow();
    for (const unsigned bit : {1u, 2u, 4u}) {
        const int idx = StepIdx(bit);
        const double ratio = std::max(stepGain_[idx] > 0.0 ? stepGain_[idx] : StepRatio(idx), 1.0); // (the measured saving, else the assumed)
        if ((now & bit) && !(steps & bit)) g *= ratio;
        else if (!(now & bit) && (steps & bit)) g /= ratio;
    }
    return g;
}

static const char* QgStepName(unsigned bit) {
    return bit == 1u ? "half-size effects and blur" : bit == 2u ? "full background mip filtering (cheaper background off)" : "edge smoothing";
}

int Controller::QgJudge(uint64_t now, double dt, bool mayChange, double gpuMs, double otherMs) {
    QgTrial& q = qgTrial_;
    if (!q.open) return 0;
    q.t += dt;
    q.ivSum += winIntervalMs_ * frames_;
    q.gpuSum += gpuMs * frames_;
    q.otherSum += std::max(0.0, otherMs) * frames_;
    q.frames += frames_;
    if (q.t < cfg_.qgTrialSec || !mayChange || q.frames == 0) return 0;
    {
        const double ivA = q.ivSum / q.frames, gpuA = q.gpuSum / q.frames, otherA = q.otherSum / q.frames;
        const bool fasterNow = q.ivBefore > 0.0 && ivA <= q.ivBefore * (1.0 - cfg_.qgRateFrac);
        // (not faster yet, judged again with fresh play from the same "before":
        //  - a GPU far over its budget needs a series: the step saved GPU time, the GPU is still the limit and over the budget it is judged
        //    against (33.3 ms on a 30 cadence): no picture yet, but the next step of the series may bring them (at most 4 times);
        //  - off the 30 cadence, the GPU was clearly the limit before and the step took GPU time away: the camera's noise may hide what it
        //    bought (console c17, Good Egg: 720x404 ~50 a second, 608x348 ~57); twice more before it is undone)
        const bool saved = gpuA <= q.gpuBefore * (1.0 - cfg_.qgRateFrac);
        const bool series = gpuA >= DownBudgetMs() * cfg_.downBelow && saved && gpuA >= otherA && q.extends < 4u;
        const bool noisy = !CadenceWindow() && saved && q.gpuBefore >= q.otherBefore * 1.05 && q.extends < 2u;
        if (!fasterNow && (series || noisy)) {
            ++q.extends;
            q.t = q.ivSum = q.gpuSum = q.otherSum = 0;
            q.frames = 0;
            return 0;
        }
    }
    q.open = false;
    // (what of the series is still given up: other rules may have undone parts of it already)
    const unsigned extra = steps_ & ~q.steps;
    const bool smaller = rung_ < q.rung;
    const bool anisoLost = q.aniso && !aniso_;
    if (!extra && !smaller && !anisoLost) return 0;
    const double ivAfter = q.ivSum / q.frames, gpuAfter = q.gpuSum / q.frames, otherAfter = q.otherSum / q.frames;
    const bool faster = q.ivBefore > 0.0 && ivAfter <= q.ivBefore * (1.0 - cfg_.qgRateFrac);
    const bool gpuLimit = q.gpuBefore >= q.otherBefore;
    // (a step that bought qgRateFrac more pictures is kept, whichever stage the census called the limit: console c17, Good Egg, the front end
    // beside the GPU's 18 ms made "the GPU was the limit" a coin flip and undid steps that bought ~7 a second)
    if (faster) {
        ++qgStats_.kept;
        return 0;
    }
    if (SeamHold(now)) { q.open = true; return 0; } // (Config::seam: the restore at a cut; the trial stays open and is judged again)
    ++qgStats_.reverts;
    // (the block: qgBlockSec when the GPU was the limit (a later try, in another view, may buy pictures: never a size pinned for minutes);
    // doubling to maxBlockSec when it was not (another stage holds the pictures: no step of the GPU's work can buy one))
    double blockSec = cfg_.qgBlockSec;
    if (gpuLimit && cfg_.qgStageBlock) {   // (gov77: each undone trial of the stage doubles the next block: the same two steps are not tried every 20 s)
        blockSec = std::min(cfg_.qgBlockSec * static_cast<double>(1u << std::min(qgGpuNoGainN_, 10u)), cfg_.maxBlockSec);
        ++qgGpuNoGainN_;
    }
    if (!gpuLimit) {
        qgBlockLen_ = qgBlockLen_ > 0.0 ? std::min(qgBlockLen_ * 2.0, cfg_.maxBlockSec) : cfg_.qgBlockSec;
        blockSec = qgBlockLen_;
    }
    const uint64_t until = now + static_cast<uint64_t>(blockSec * 1e6);
    char why[256];
    std::snprintf(why, sizeof why, "a picture every %.1f ms before, %.1f ms over the %.1f s after%s; GPU %.1f -> %.1f ms, other stages %.1f -> %.1f ms%s; "
                  "not taken again for %.0f s", q.ivBefore, ivAfter, q.t, faster ? "" : " (no faster)", q.gpuBefore, gpuAfter, q.otherBefore, otherAfter,
                  gpuLimit ? "" : " (the GPU was not the limit)", blockSec);
    for (const unsigned bit : {1u, 2u, 4u}) {
        if (!(extra & bit)) continue;
        const int idx = StepIdx(bit);
        LogSummary("[autoq] quality: %s restored: bought no speed (%s)", QgStepName(bit), why);
        onBlockLen_[idx] = std::max(onBlockLen_[idx], blockSec);
        onBlockedUntil_[idx] = std::max(onBlockedUntil_[idx], until);
    }
    if (smaller) {
        LogSummary("[autoq] quality: picture size (rung %d -> %d) restored: bought no speed (%s)", rung_, q.rung, why);
        belowBlockedUntil_ = std::max(belowBlockedUntil_, until);
        belowBlockLen_ = std::max(belowBlockLen_, blockSec);
        for (int r = rung_ + 1; r <= q.rung; ++r) blockUntil_[r + 1] = 0; // (the sizes it left were not too slow after all)
    }
    if (anisoLost) {
        LogSummary("[autoq] quality: anisotropic filtering restored: bought no speed (%s)", why);
        qgAnisoKeepUntil_ = until;
    }
    lastBlockAt_ = now;
    // (the other rules' judgements of the same reduction are moot now)
    checking_ = false;
    pendingUndo_ = -1;
    pendingBelow_ = false;
    pathFrom_ = kNoRung;
    trial_ = 0;
    qgInRevert_ = true;
    const int from = rung_;
    if (extra) SetSteps(steps_ & q.steps, Why::QgNoGain, now);
    if (smaller) SetRung(q.rung, Why::QgNoGain, now);
    if (anisoLost && AnisoRungOk(rung_)) SetAniso(true, Why::QgNoGain, now);
    qgInRevert_ = false;
    return rung_ != from ? 1 : 2;
}

int Controller::QgRestore(uint64_t now, double gpuMs, double otherMs) {
    // (a restore while the last one is still on trial goes on from it: the back is to the state before the first; the measured GPU time
    // at the size restored is what says whether the steps fit too)
    if (now < qgRestoreBlockedUntil_ || qgTrial_.open || trial_ || checking_ || pendingUndo_ >= 0 || qgBackMiss_) return 0;
    const double cadMs = std::max(cfg_.budgetMs, 1000.0 / 30.0), fitMs = cfg_.cadenceUpFrac * cadMs;
    if (otherMs > fitMs) return 0;
    auto fits = [&](double g) { return g <= fitMs; };
    // sharpest first: the size (today's at most: above it is the ladder's own climb), then the steps in the way back's order
    int toRung = rung_;
    unsigned toSteps = steps_;
    const int top = std::min(cfg_.maxRung, std::max(rung_, kToday));
    while (toRung < top && fits(QgPredict(toRung + 1, toSteps, gpuMs))) ++toRung;
    for (const unsigned bit : {1u, 2u, 4u})
        if ((toSteps & bit) && fits(QgPredict(toRung, toSteps & ~bit, gpuMs))) toSteps &= ~bit;
    const bool wantAniso = cfg_.aniso && AnisoRungOk(toRung) && toSteps == 0 && !(toRung == rung_ && aniso_) &&
                           fits(QgPredict(toRung, toSteps, gpuMs) * cfg_.anisoRatio);
    if (toRung == rung_ && toSteps == steps_ && !wantAniso) return 0;
    if (SeamHold(now)) return 0; // (Config::seam: the restore at a cut; the room stays, the rule asks again next window)
    const double pred = QgPredict(toRung, toSteps, gpuMs) * (wantAniso ? cfg_.anisoRatio : 1.0);
    char why[224];
    std::snprintf(why, sizeof why, "60 out of reach%s and the 30 cadence holds: GPU %.1f ms a picture now, %.1f ms predicted with it, within %.1f of 33.3 ms; "
                  "other stages %.1f ms", cfg_.cap30 ? " (the fixed 30 cap)" : "", gpuMs, pred, fitMs, otherMs);
    if (toRung != rung_) LogSummary("[autoq] quality: picture size (rung %d -> %d) restored: bought no speed (%s)", rung_, toRung, why);
    for (const unsigned bit : {1u, 2u, 4u})
        if ((steps_ & bit) && !(toSteps & bit)) LogSummary("[autoq] quality: %s restored: bought no speed (%s)", QgStepName(bit), why);
    if (wantAniso) LogSummary("[autoq] quality: anisotropic filtering restored: bought no speed (%s)", why);
    ++qgStats_.restores;
    if (qgBackRung_ == kNoRung) {
        qgBackRung_ = rung_;
        qgBackSteps_ = steps_;
    }
    qgBackUntil_ = now + static_cast<uint64_t>(cfg_.holdTrialSec * 1e6);
    qgBackMiss_ = false;
    qgRoomT_ = 0;
    const int from = rung_;
    if (toSteps != steps_) SetSteps(toSteps, Why::QgRestore, now);
    if (toRung != rung_) SetRung(toRung, Why::QgRestore, now);
    if (wantAniso && !aniso_) SetAniso(true, Why::QgRestore, now);
    if (qgBackRung_ == rung_ && qgBackSteps_ == steps_) qgBackRung_ = kNoRung; // (only anisotropic filtering: its own rule takes it off under load)
    return rung_ != from ? 1 : 2;
}

int Controller::QgBack(uint64_t now) {
    qgRestoreBlockLen_ = qgRestoreBlockLen_ > 0.0 ? std::min(qgRestoreBlockLen_ * 2.0, cfg_.maxBlockSec) : cfg_.qgBlockSec;
    qgRestoreBlockedUntil_ = now + static_cast<uint64_t>(qgRestoreBlockLen_ * 1e6);
    lastBlockAt_ = now;
    ++qgStats_.restoreBacks;
    LogSummary("[autoq] quality: the restore lost pictures to the GPU on the 30 cadence: back to rung %d with steps %u, the next restore not before %.0f s",
               qgBackRung_, qgBackSteps_, qgRestoreBlockLen_);
    const int from = rung_, toRung = qgBackRung_;
    const unsigned toSteps = qgBackSteps_;
    qgBackRung_ = kNoRung;
    qgBackMiss_ = false;
    checking_ = false;
    qgInRevert_ = true;
    if (toSteps != steps_) SetSteps(toSteps, Why::QgBack, now);
    if (toRung != rung_) SetRung(toRung, Why::QgBack, now);
    qgInRevert_ = false;
    return rung_ != from ? 1 : 2;
}

// One presented picture of the floor census (target mode). Quiet moments and stalls (loads, shader builds) are counted apart;
// a late picture is blamed on the largest stage that reached causeShare of the budget: the GPU's busy time, two game turns'
// CPU, the renderer's CPU, the front end's CPU (when it is measured). None that did, with the first three measured: something
// unmeasured (a wait, the pacing). A picture on Performance mode's steady 30 (cadence) is late only against 33.3 ms.
int Controller::FloorCount(uint64_t interval, bool quiet, bool measured, bool gpuKnown, bool gameKnown, bool backendKnown,
                           uint64_t gpu, uint64_t game, uint64_t backend, bool frontKnown, uint64_t front, bool cadence,
                           uint64_t gameSpanUs, uint32_t spanTurns) {
    FloorWindow& w = floor_;
    if (quiet) { ++w.quiet; return -1; }
    if (static_cast<double>(interval) > cfg_.gapResetMs * 1000.0) { ++w.stalls; w.stallUs += interval; return -1; }
    ++w.pictures;
    w.spanUs += interval;
    if (frontKnown && front != 0) { ++w.frontPics; w.frontSumUs += front; } // (fe26: GXV_FE_GOV's measure, whether or not the picture was late)
    const bool paced30 = cadence && cfg_.targetFps == 60;
    if (paced30) ++w.cadence;
    const double budgetMs = paced30 ? 1000.0 / 30.0 : cfg_.budgetMs;
    if (static_cast<double>(interval) <= budgetMs * 1030.0) return -1;
    ++w.late;
    w.lateUs += interval;
    w.worstUs = std::max(w.worstUs, interval);
    const double threshold = cfg_.causeShare * budgetMs * 1000.0;
    // (GXV_PACE23: the game thread's CPU over the picture's whole span, not one turn: a picture of 2-3 turns whose turns fill the interval
    // is the game thread's, whatever the one-turn figure and the GPU's busy time say)
    if (P23() && gameSpanUs != 0 && static_cast<double>(gameSpanUs) >= cfg_.p23GameShare * static_cast<double>(interval)) {
        ++w.game;
        ++w.p23GameSpan;
        return 1;
    }
    int cause = -1;
    double best = 0;
    auto consider = [&](bool known, uint64_t v, int id) {
        if (known && static_cast<double>(v) >= threshold && static_cast<double>(v) > best) { best = static_cast<double>(v); cause = id; }
    };
    consider(gpuKnown, gpu, 0);
    consider(gameKnown, game, 1);
    consider(backendKnown, backend, 2);
    consider(frontKnown, front, 3);
    // (GXV_PACE23: the GPU reached the census share of the budget but not the budget itself: alone it would have made the picture on time, so
    // it was waiting for the picture (CPU-paced), not holding it; the late picture goes to the stages the census cannot see)
    if (cause == 0 && P23() && static_cast<double>(gpu) < cfg_.p23GpuMiss * budgetMs * 1000.0) {
        ++w.p23CpuPaced;
        ++p23_.lowUtil;
        cause = -1;
    }
    if (cause == 0) ++w.gpu;
    else if (cause == 1) ++w.game;
    else if (cause == 2) ++w.renderer;
    else if (cause == 3) ++w.front;
    else if (measured) {
        ++w.other;
        // GXV_FLOOR_SPAN (default 1; census only): what "none of these" was. At the 60 target the game stage is ONE turn's CPU against
        // causeShare of the budget, but a picture the skip rule stretched over 2-3 turns (and every picture on the steady 30) has an
        // interval of several turns: the game thread's CPU over all of them is the true stage. r107/r108 Honeyhive with the profilers
        // (cpuhh10.log): 104 pictures of 47.6 ms, 2.8 turns each at 15.5 ms of game CPU a turn = 91 % of the interval, "none of these" 48.
        static const bool span = [] { const char* e = std::getenv("GXV_FLOOR_SPAN"); return !e || e[0] != '0'; }();
        if (span && spanTurns != 0 && gameSpanUs != 0) {
            if (spanTurns >= 2) ++w.otherMulti;
            if (static_cast<double>(gameSpanUs) >= 0.85 * static_cast<double>(interval)) {
                ++w.otherGameSpan;
                w.otherGameSpanUs += gameSpanUs;
                w.otherSpanUs += interval;
            }
        }
        return 4;
    }
    else { ++w.unknown; return 5; }
    return cause;
}

void Controller::Limit(int highest, uint64_t now) {
    if (highest >= rung_) return;
    highest = std::max(highest, std::min(last_.from, rung_));
    for (int r = highest + 1; r <= cfg_.maxRung; ++r) Block(r, now);
    checking_ = false;
    rung_ = highest;
    last_.to = rung_;
    if (aniso_ && !AnisoKeptAt(rung_)) { aniso_ = false; ++anisoChanges_; anisoAt_ = now; }
    sinceChange_ = 0;
    ResetWindow();
    ResetHolds();
}

void Controller::Drop(int to, uint64_t now) {
    to = std::clamp(to, cfg_.minRung, cfg_.maxRung);
    if (to >= rung_) return;
    for (int r = to + 1; r <= cfg_.maxRung; ++r) Block(r, now);
    checking_ = false;
    SetRung(to, Why::NoMemory, now);
}

void Controller::Refused(int back, int failed, uint64_t now) {
    back = std::clamp(back, cfg_.minRung, cfg_.maxRung);
    failed = std::clamp(failed, cfg_.minRung, cfg_.maxRung);
    Block(failed, now);
    if (failed > back)
        for (int r = failed + 1; r <= cfg_.maxRung; ++r) Block(r, now);
    checking_ = false;
    rung_ = failed; // (as if applied: SetRung records the way back)
    SetRung(back, Why::NoMemory, now);
}

bool Controller::Add(const FrameTimes& in) {
    if (port::ab::Frozen()) return false;   // (ab27: GXV_AB holds the picture size while it measures)
    // (Config::cap30: under the fixed 30 cap every picture is shown on a 30 cadence, exactly as on the steady 30: judged against 33.3 ms, never
    // a miss of 60, the way back against 33.3 ms. The console runs 480-483 had target 60 under Quality's cap and chased a 60 that cannot come.)
    FrameTimes f = in;
    if (QG() && cfg_.cap30) f.cadence30 = true;
    if (!havePrev_) {
        if (Target()) g_pictureWorkUs.store(0, std::memory_order_release);
        havePrev_ = true;
        prevDone_ = f.gpuDoneUs;
        prevPresent_ = f.presentUs;
        prevRun_ = f.backendRunUs;
        prevGameRun_ = f.gameRunUs;
        prevGameTurns_ = f.gameTurns;
        prevGameFaults_ = f.gameFaults;
        prevGameKnown_ = f.gameKnown;
        prevFrontRun_ = f.frontRunUs;
        prevFrontPictures_ = f.frontPictures;
        prevFrontKnown_ = f.frontKnown;
        return false;
    }
    if (ShedOn()) {
        ShedNotePicture(static_cast<double>(f.presentUs - prevPresent_) / 1000.0);
    }
    if ((cfg_.seam || cfg_.seam72) && (f.cut || f.quiet)) { // (Config::seam: a moment where a visible step is not seen)
        if (f.cut && !f.quiet) ++seamStats_.cuts;
        seamLastCutUs_ = f.presentUs;
    }
    if (cfg_.recover && cfg_.calm && lastBlockAt_ != 0 && f.presentUs >= lastBlockAt_ + static_cast<uint64_t>(cfg_.blockForgetSec * 1e6)) {
        // (GXV_AUTOQ_RECOVER: the blocks that ran out long ago do not count towards the next one's length)
        for (double& len : blockLen_) len = 0;
        belowBlockLen_ = 0;
        onBlockLen_[0] = onBlockLen_[1] = onBlockLen_[2] = fxOffBlockLen_ = anisoBlockLen_ = 0;
        edgeBlockLen_ = 0;
        lastBlockAt_ = 0;
    }
    if (QG()) {
        // (Config::qg: a picture that is not play (a loading screen, the title before the first stage, a menu layer, a quiet moment, a movie)
        // decides nothing; the first one of a stretch makes the play settle again, and pace60's verdict is formed anew after it)
        const bool nonPlay = f.quiet || f.movie || f.loading || f.noStage;
        if (nonPlay) {
            ++qgStats_.nonPlay;
            if (!qgNonPlay_) QgUnsettle();
        }
        qgNonPlay_ = nonPlay;
    }
    const uint64_t interval = f.presentUs > prevPresent_ ? f.presentUs - prevPresent_ : 0;
    const uint64_t prevDone = prevDone_, prevRun = prevRun_;
    if (sinceChange_ < (1u << 30)) ++sinceChange_;
    if (sinceLoad_ < (1u << 30)) ++sinceLoad_;
    const bool mayChange = sinceChange_ >= static_cast<unsigned>(cfg_.minChangeFrames) &&
        (!Target() || changes_ == 0 || f.presentUs >= last_.atUs + 3000000u) &&
        (!fxEver_ || f.presentUs >= lastFxAt_ + 3000000u) && (anisoAt_ == 0 || f.presentUs >= anisoAt_ + 3000000u);
    const uint32_t gameRun = f.gameRunUs - prevGameRun_, gameTurns = f.gameTurns - prevGameTurns_;
    const bool gameKnown = f.gameKnown && prevGameKnown_ && f.gameFaults == prevGameFaults_;
    // The front end's CPU per picture (target mode, when it stamps its pictures): the pictures it finished since the last present
    // and their CPU time; a present with none keeps the last known value for the per-picture tests.
    const bool frontPair = f.frontKnown && prevFrontKnown_;
    const uint32_t frontPictures = frontPair ? f.frontPictures - prevFrontPictures_ : 0, frontRun = frontPair ? f.frontRunUs - prevFrontRun_ : 0;
    if (!frontPair) lastFrontUs_ = 0;
    else if (frontPictures != 0 && frontPictures < 16) lastFrontUs_ = frontRun / frontPictures;
    // GXV_PACE23: the smoothed share of the picture interval the GPU was busy (busy time over the span since the last picture that carried
    // one, 7/8 per picture), and the settle window after a load or a stall
    bool p23Hold = false;
    if (P23()) {
        const bool stall = static_cast<double>(interval) > cfg_.gapResetMs * 1000.0;
        if (f.timers && f.busyKnown) { p23LastBusyUs_ = f.gpuBusyUs; p23BusyAge_ = 0; }
        else if (p23BusyAge_ < (1u << 30)) ++p23BusyAge_;
        if (stall) {
            p23SettleUntil_ = std::max(p23SettleUntil_, f.presentUs + static_cast<uint64_t>(cfg_.p23SettleSec * 1e6));
            p23UtilBusy_ = p23UtilSpan_ = p23PendSpan_ = 0;
            p23_.util = -1;
        } else if (!f.quiet && !f.movie) {
            p23PendSpan_ += static_cast<double>(interval);
            if (f.timers && f.busyKnown) {
                p23UtilBusy_ = p23UtilBusy_ * 0.875 + static_cast<double>(f.gpuBusyUs) * std::max<uint32_t>(1u, std::min<uint32_t>(f.gpuFrames, 4u));
                p23UtilSpan_ = p23UtilSpan_ * 0.875 + p23PendSpan_;
                p23PendSpan_ = 0;
            }
            p23_.util = p23UtilSpan_ > 0 ? p23UtilBusy_ / p23UtilSpan_ : -1.0;
        }
        p23Hold = f.presentUs < p23SettleUntil_;
        if (p23Hold && !stall && !f.quiet) ++p23_.settleHeld;
    }
    if (Target()) {
        bool currentRoom = false;
        const bool backendKnown = f.backendRunUs > 0 && prevRun > 0 && f.backendRunUs >= prevRun &&
            f.backendRunUs <= INT64_MAX && prevRun <= INT64_MAX;
        // (GXV_PACE23: a picture whose own busy time was not delivered yet uses the last one, if it is recent)
        const bool p23Recent = P23() && f.timers && !f.busyKnown && p23BusyAge_ <= 6;
        const bool gpuKnown = f.timers ? (f.busyKnown || p23Recent) : !f.paced && f.gpuDoneUs != 0 && prevDone != 0;
        {
            // (the floor census: the same three stages, whichever of them is known)
            const uint64_t doneSpan = f.gpuDoneUs > prevDone ? f.gpuDoneUs - prevDone : 0;
            const uint64_t from = std::max(prevDone, f.startUs);
            const uint64_t fenceBusy = f.gpuDoneUs > from ? std::min(f.gpuDoneUs - from, doneSpan) : 0;
            const uint64_t gpuStageUs = f.timers ? (p23Recent ? p23LastBusyUs_ : f.gpuBusyUs) : fenceBusy;
            const bool gameOk = gameKnown && gameTurns != 0;
            const uint64_t gameUs = gameOk ? uint64_t(gameRun) * PictureTurns() / gameTurns : 0, backendUs = backendKnown ? f.backendRunUs - prevRun : 0;
            const int cause = FloorCount(interval, f.quiet || (QG() && (f.loading || f.noStage)), gameOk && backendKnown && gpuKnown, gpuKnown, gameOk, backendKnown,
                                         gpuStageUs, gameUs, backendUs, lastFrontUs_ != 0, lastFrontUs_, f.cadence30,
                                         gameOk ? uint64_t(gameRun) : 0, gameOk ? gameTurns : 0);
            // (q30lock26: GXV_Q30_LOCK, the picture's measured times for the Quality lock ladder: its GPU busy time and the front end's CPU; a picture of play only. Nothing when the ladder is off.)
            if (port_q30lock_picture)
                port_q30lock_picture(static_cast<uint32_t>(std::min<uint64_t>(gpuStageUs, 4000000u)), gpuKnown ? 1 : 0, static_cast<uint32_t>(std::min<uint64_t>(lastFrontUs_, 4000000u)),
                                     lastFrontUs_ != 0 ? 1 : 0, (f.quiet || f.movie || f.loading || f.noStage) ? 0 : 1);
            // (rate30: GXV_RATE_AUTO: the same picture's three lanes, front end / render thread / GPU, for the rate controller; the render thread's is the back end's own run time for the picture)
            if (port_rate_picture)
                port_rate_picture(static_cast<uint32_t>(std::min<uint64_t>(lastFrontUs_, 4000000u)), lastFrontUs_ != 0 ? 1 : 0, static_cast<uint32_t>(std::min<uint64_t>(backendUs, 4000000u)), backendKnown ? 1 : 0,
                                  static_cast<uint32_t>(std::min<uint64_t>(gpuStageUs, 4000000u)), gpuKnown ? 1 : 0, (f.quiet || f.movie || f.loading || f.noStage) ? 0 : 1);
            // (Config::hold30: a picture late against the budget + 3 % is late for the GPU only when the census blames the GPU AND every
            // other measured stage fitted the budget + 3 %: then a smaller picture could have brought it on time. Late for the game's CPU
            // (or with the game over the budget as well: the GPU timer counts waits under the fixed cap) it says nothing about the size.)
            picLate_ = !f.quiet && static_cast<double>(interval) > cfg_.budgetMs * 1030.0;
            const double fits = cfg_.budgetMs * 1030.0;
            // (r84 console, Beach Bowl at zoom 2.0: with the GPU ~40 ms a picture and far behind, its timers stopped reporting, the census
            // said "not measured" for every late picture and nothing was ever blamed on the GPU: 25 fps at 960x544 for 25 s. A late
            // picture whose game and renderer CPU are known and fitted, with the GPU's own time unknown, is late for the GPU: nothing else
            // is left that could have made it late.)
            // (GXV_PACE23: only when the timers have been silent for many pictures, the picture is far behind and the front end is measured and
            // fits: after a load the timers skip pictures while shader builds and uploads make them late, and that was not the GPU)
            const bool gpuByElimination = P23() ? (f.timers && !gpuKnown && p23BusyAge_ >= static_cast<unsigned>(cfg_.p23ElimPictures) && gameOk && backendKnown &&
                                                   lastFrontUs_ != 0 && static_cast<double>(interval) > 1.4 * cfg_.budgetMs * 1000.0)
                                                : (!gpuKnown && gameOk && backendKnown);
            if (P23() && picLate_ && !gpuKnown && !gpuByElimination) ++p23_.noElim;
            // (GXV_PACE23: not the GPU's while the game thread's span CPU fills the interval, or inside a settle window)
            const bool p23Span = P23() && gameOk && static_cast<double>(gameRun) >= cfg_.p23GameShare * static_cast<double>(interval);
            if (p23Span && picLate_ && cause != 1) ++p23_.gameSpan;
            picLateGpu_ = picLate_ && (cause == 0 || gpuByElimination) && (!gameOk || static_cast<double>(gameUs) <= fits) &&
                (!backendKnown || static_cast<double>(backendUs) <= fits) && (lastFrontUs_ == 0 || static_cast<double>(lastFrontUs_) <= fits) &&
                !(P23() && (p23Span || p23Hold));
            // (Config::cadenceUp: a picture on the steady 30 that the census, judging it against 33.3 ms, blames on the GPU, or with the
            // GPU's own time unknown and nothing else measured that made it late: the size may have cost it)
            picCadLateGpu_ = (cfg_.cadenceUp || QG()) && f.cadence30 && (cause == 0 || (cause == 5 && gpuByElimination));
            // (Config::fast60: the picture goes into the ring of the last ones, with its GPU time and its largest other stage)
            if (Fast60())
                FastNote(f, interval, picLateGpu_, P23() ? gpuStageUs : (f.timers ? f.gpuBusyUs : fenceBusy), // (the 2.2 log's "GPU load 0 %": a picture with no busy time of its own counted as 0 ms)
                         std::max({gameUs, backendUs, FrontStage() ? uint64_t(lastFrontUs_) : uint64_t(0)}));
        }
        if (!gameKnown || !gameTurns || !backendKnown || !gpuKnown) {
            hintKnown_ = false;
            currentWorkUs_ = 0;
            g_pictureWorkUs.store(0, std::memory_order_release);
        } else {
            const uint64_t doneSpan = f.gpuDoneUs > prevDone ? f.gpuDoneUs - prevDone : 0;
            const uint64_t from = std::max(prevDone, f.startUs);
            const uint64_t fenceBusy = f.gpuDoneUs > from ? std::min(f.gpuDoneUs - from, doneSpan) : 0;
            const uint64_t gpu = f.timers ? f.gpuBusyUs : fenceBusy;
            // (the pacing hint is a 30 Hz feasibility cost in either target: two turns; the decisions take the target's own turns)
            const uint64_t game2 = uint64_t(gameRun) * 2 / gameTurns;
            const uint64_t game = uint64_t(gameRun) * PictureTurns() / gameTurns;
            const uint64_t backend = f.backendRunUs - prevRun;
            const uint64_t front = FrontStage() ? lastFrontUs_ : 0;
            currentWorkUs_ = static_cast<uint32_t>(std::min<uint64_t>(1000000,
                std::max({gpu, backend, game2, front})));
            // A known slow current picture must invalidate old headroom even when a quiet return skips evaluation.
            const uint32_t published = g_pictureWorkUs.load(std::memory_order_relaxed);
            if (published && currentWorkUs_ > published) g_pictureWorkUs.store(currentWorkUs_, std::memory_order_release);
            const int candidate = std::min(rung_ + std::max(1, pendingUp_), cfg_.maxRung);
            // (GXV_AUTOQ_RECOVER: a single frame ends the stretch of room only when it really missed, not when it was merely
            // above the margin the window means must keep: recoverFrameFrac against upTarget)
            const double frameFrac = cfg_.recover && cfg_.calm ? cfg_.recoverFrameFrac : cfg_.upTarget;
            // (Config::lateRoom: a late picture whose own work fits at the next rung was late for something else, the front end or a
            // wait, and does not end the stretch; the window means keep the cadence. A stall over gapResetMs restarts everything anyway.)
            const double other = std::max({double(game), double(backend), double(front)});
            currentRoom = (interval <= cfg_.budgetMs * 1030.0 || cfg_.lateRoom) &&
                std::max(other, gpu * Cost(candidate) / Cost(rung_)) <= frameFrac * cfg_.budgetMs * 1000.0;
            // (below today: another stage holds the pictures above the budget, and the GPU at the next size stays under it: the
            // smaller picture buys nothing, see freeClimb in Evaluate)
            if (!currentRoom && candidate <= kToday && other > cfg_.budgetMs * 1030.0 && gpu * Cost(candidate) / Cost(rung_) <= other)
                currentRoom = true;
            // (Config::cadenceUp: a picture on the steady 30 below today's rung is judged against 33.3 ms as the window will be: shown on
            // its cadence, its predicted work at the next rung (today's at most) within the margin (recoverFrameFrac with recovery, as
            // above), and 60 out of reach at any size; the window means decide the climb, see Evaluate)
            const double cadMs = std::max(cfg_.budgetMs, 1000.0 / 30.0), reachUs = 1e6 / cfg_.cadenceReachFps;
            if (!currentRoom && cfg_.cadenceUp && f.cadence30 && rung_ < kToday &&
                (gpu * Cost(cfg_.minRung) / Cost(rung_) >= reachUs || other >= reachUs) &&
                (interval <= cadMs * 1030.0 || cfg_.lateRoom) &&
                std::max(other, gpu * Cost(std::min(candidate, std::max(rung_, kToday))) / Cost(rung_)) <=
                    (cfg_.recover && cfg_.calm ? cfg_.recoverFrameFrac : cfg_.cadenceUpFrac) * cadMs * 1000.0)
                currentRoom = true;
        }
        if (!currentRoom) {
            pendingUp_ = 0;
            roomAt_ = 0;
            roomT_ = 0;
            minUp_ = 0;
        }
    }
    prevDone_ = f.gpuDoneUs;
    prevPresent_ = f.presentUs;
    prevRun_ = f.backendRunUs;
    prevGameRun_ = f.gameRunUs;
    prevGameTurns_ = f.gameTurns;
    prevGameFaults_ = f.gameFaults;
    prevGameKnown_ = f.gameKnown;
    prevFrontRun_ = f.frontRunUs;
    prevFrontPictures_ = f.frontPictures;
    prevFrontKnown_ = f.frontKnown;
    if (ShedOn() && cfg_.shedCycleSec > 0.0 && ShedCycleStep(f.presentUs)) return true; // (the stress cycle: after the bookkeeping, whatever the load)
    if (static_cast<double>(interval) > cfg_.gapResetMs * 1000.0) {
        if (Target()) g_pictureWorkUs.store(0, std::memory_order_relaxed);
        if (static_cast<double>(interval) > cfg_.longGapMs * 1000.0) mGap_ = true; // (GXV_PACE60: a load: the model fills afresh)
        if (static_cast<double>(interval) > cfg_.longGapMs * 1000.0) gate_.Clear(); // (s1117b: a load: the gate's pictures are another scene's)
        if (QG() && static_cast<double>(interval) > cfg_.longGapMs * 1000.0) QgUnsettle(); // (Config::qg: a load: the play settles again)
        // A load or a shader compile: what came before says nothing about what comes next.
        ResetWindow();
        ResetHolds();
        // (target mode: a check of a step below today goes on after a stall in the same scene. The change's own freeze, the new
        // render targets and the next slow frame, is often just over gapResetMs (0.26 s on the console), and a check dropped
        // there kept the smaller picture without a verdict: Beach Bowl went 640 -> 592 that way although the GPU time stayed.)
        if (!Target()) checking_ = false;
        if (static_cast<double>(interval) > cfg_.longGapMs * 1000.0 && cfg_.calm) {
            sinceLoad_ = 0;
            // A load (calm mode): the rung of the scene left is remembered, the new scene starts at its own remembered
            // rung (or the start rung), applied now, while the load hides it. The same scene after a long gap (a
            // shader compile, the console suspended) keeps its rung and its blocks.
            const uint32_t from = scene_, to = f.scene;
            // (target mode: a scene left before an undo for no gain was made is remembered at the rung the undo goes back to)
            // (Config::qg: and before a reduction still on trial, as it was before it: an unjudged reduction is not carried to the next visit)
            int memRung = Target() && pendingUndo_ >= 0 ? pendingUndo_ : rung_;
            unsigned memFx = Mem();
            if (QG() && qgTrial_.open) {
                memRung = std::max(memRung, qgTrial_.rung);
                memFx = (steps_ & qgTrial_.steps) | (aniso_ || qgTrial_.aniso ? kAnisoMem : 0u);
            }
            Remember(from, memRung, memFx, sceneFloor_);
            scene_ = to;
            if (to != 0 && to == from) {
                if (presetActive_) { // (stagepreset25: the same stage again (a retry, a restart): back to the stage's own start, while the load hides it)
                    ApplyPresetStart(f.presentUs);
                    SetSceneSteps(PresetSteps(), f.presentUs);
                    if (rung_ != cfg_.startRung) {
                        pendingUp_ = 0;
                        roomAt_ = 0;
                        checking_ = false;
                        SetRung(cfg_.startRung, Why::NewScene, f.presentUs);
                        return true;
                    }
                    return false;
                }
                // (Config::wipe: a load within the same stage leads into another part of it, a drop-in, a dome: not above today's rung,
                // changed now while the load hides it; the blocks of the stage stay)
                if (WipeCap(rung_) != rung_) {
                    pendingUp_ = 0;
                    roomAt_ = 0;
                    checking_ = false;
                    SetRung(WipeCap(rung_), Why::Wipe, f.presentUs);
                    return true;
                }
                return false;
            }
            checking_ = false;
            if (Target()) { pendingUndo_ = -1; pathFrom_ = kNoRung; } // (an undo learnt in the scene left: the new scene starts at its own rung)
            if (QG()) { qgTrial_.open = false; qgBackRung_ = kNoRung; qgRecent_ = false; } // (Config::qg: another scene: nothing on trial carries over)
            RecallFloor(to, f.presentUs);
            for (int i = 0; i <= kRungs; ++i) { blockUntil_[i] = 0; blockLen_[i] = 0; }
            belowBlockedUntil_ = 0;
            belowBlockLen_ = 0;
            ForgetFxBlocks();
            unsigned wantFx = 0;
            int want = Recall(to, &wantFx);
            if (want == kNoRung) want = cfg_.startRung;
            if (presetActive_) { want = cfg_.startRung; wantFx = PresetSteps(); } // (stagepreset25: the stage's own start, not a remembered rung)
            want = WipeCap(std::clamp(want, cfg_.minRung, cfg_.maxRung)); // (Config::wipe: a new scene starts at today's rung at most)
            const bool wantAniso = cfg_.aniso && (wantFx & kAnisoMem) != 0 && want == cfg_.maxRung;
            wantFx &= StepMask();
            if (want > FxRung() && !presetActive_) wantFx = 0;
            ApplyPresetStart(f.presentUs);
            if (want != rung_) {
                SetSceneSteps(wantFx, f.presentUs);
                SetSceneAniso(wantAniso, f.presentUs);
                SetRung(want, Why::NewScene, f.presentUs);
                return true;
            }
            SetSceneSteps(wantFx, f.presentUs);
            SetSceneAniso(wantAniso, f.presentUs);
            return false;
        }
        if (static_cast<double>(interval) > cfg_.longGapMs * 1000.0) {
            // A new scene: nothing learnt about the old one holds, and its rung may be wrong for this one. Back to the
            // start rung (today's picture), from where the next windows decide again.
            checking_ = false;
            if (Target()) { pendingUndo_ = -1; pathFrom_ = kNoRung; }
            if (QG()) { qgTrial_.open = false; qgBackRung_ = kNoRung; qgRecent_ = false; }
            for (int i = 0; i <= kRungs; ++i) { blockUntil_[i] = 0; blockLen_[i] = 0; }
            belowBlockedUntil_ = 0;
            belowBlockLen_ = 0;
            ForgetFxBlocks();
            SetSceneSteps(PresetSteps(), f.presentUs);
            SetSceneAniso(false, f.presentUs);
            ApplyPresetStart(f.presentUs);
            if (rung_ != cfg_.startRung) {
                SetRung(cfg_.startRung, Why::NewScene, f.presentUs);
                return true;
            }
        }
        return false;
    }
    if (cfg_.calm && f.scene != 0 && f.scene != scene_) {
        // The scene's key came after the load's gap (the game names the stage a few frames into it): the rung of the
        // scene left goes under the old key, and a remembered rung of the new one is taken while the scene is still
        // fading in (within 60 frames of the return to the start rung).
        {
            int memRung = last_.why == Why::NewScene && sinceChange_ <= 60 ? last_.from : rung_;
            unsigned memFx = last_.why == Why::NewScene && sinceChange_ <= 60 ? steps_ : Mem();
            if (QG() && qgTrial_.open) { // (Config::qg: as at a load: an unjudged reduction is not remembered)
                memRung = std::max(memRung, qgTrial_.rung);
                memFx = (memFx & (qgTrial_.steps | kAnisoMem)) | (qgTrial_.aniso ? kAnisoMem : 0u);
            }
            Remember(scene_, memRung, memFx, sceneFloor_);
        }
        if (QG()) { QgUnsettle(); qgTrial_.open = false; qgBackRung_ = kNoRung; qgRecent_ = false; } // (Config::qg: another stage: settle again)
        scene_ = f.scene;
        if (P23()) p23SettleUntil_ = std::max(p23SettleUntil_, f.presentUs + static_cast<uint64_t>(cfg_.p23SettleSec * 1e6));
        RecallFloor(f.scene, f.presentUs);
        unsigned wantFx = 0;
        int want = Recall(f.scene, &wantFx);
        if (presetActive_) { // (stagepreset25: the stage's own start, its steps, its edge pass and captures)
            ApplyPresetStart(f.presentUs);
            SetSceneSteps(PresetSteps(), f.presentUs);
            if (rung_ != cfg_.startRung) {
                SetRung(cfg_.startRung, Why::NewScene, f.presentUs);
                return true;
            }
        } else if (presetForcedEdge_ || presetForcedCuts_) {
            ApplyPresetStart(f.presentUs); // (the stage before forced them: the configuration's state back)
        }
        // (Config::newStage: a new stage keeps the rung of the stage before (its load may show pictures all along, with no gap), which may
        // be a heavy stage's small picture. A stage with nothing remembered starts at the start rung, as after any other load. On
        // the console a tour went Beach Bowl 608x346 -> Bonefin at 608x346 and 46 fps, for the whole stage.)
        if (cfg_.newStage && want == kNoRung && WipeCap(std::clamp(cfg_.startRung, cfg_.minRung, cfg_.maxRung)) != rung_) {
            const int to = WipeCap(std::clamp(cfg_.startRung, cfg_.minRung, cfg_.maxRung));
            checking_ = false;
            if (Target()) pendingUndo_ = -1;
            for (int i = 0; i <= kRungs; ++i) { blockUntil_[i] = 0; blockLen_[i] = 0; }
            belowBlockedUntil_ = 0;
            belowBlockLen_ = 0;
            ForgetFxBlocks();
            SetSceneSteps(0, f.presentUs);
            SetSceneAniso(false, f.presentUs);
            SetRung(to, Why::NewScene, f.presentUs);
            return true;
        }
        if (last_.why == Why::NewScene && sinceChange_ <= 60 && want != kNoRung && WipeCap(std::clamp(want, cfg_.minRung, cfg_.maxRung)) != rung_) {
            const int to = WipeCap(std::clamp(want, cfg_.minRung, cfg_.maxRung));
            SetSceneSteps(to <= FxRung() ? (wantFx & StepMask()) : 0u, f.presentUs);
            SetSceneAniso(cfg_.aniso && (wantFx & kAnisoMem) != 0 && to == cfg_.maxRung, f.presentUs);
            SetRung(to, Why::NewScene, f.presentUs);
            return true;
        }
    }
    if (cfg_.calm && f.movie) {
        // forkB (from Producdevity's fork, 2d0ef0c): a movie (THP, port_skip_movie_playing) is neither measured nor a moment to
        // change size: it is one quad, and its light GPU load read as room. At its closing fade, or after two minutes of it, the
        // opening area stepped from 544x388 to 720x480, overloaded the GPU the moment play came back and stepped down again through
        // four freezes of 120-260 ms (fork's console log, 5 Nov). The windows and any pending step are dropped, no step is taken.
        ResetWindow();
        ResetHolds();
        overFrames_ = 0;
        pendingUp_ = 0;
        roomAt_ = 0;
        roomT_ = 0;
        pendingBelow_ = false;
        return false;
    }
    if (QG() && (f.loading || f.noStage)) {
        // (Config::qg: a loading screen, a transition, the title before the first stage, a menu layer: not measured, nothing decided, nothing
        // pending kept; the console's loading scene (TA 1.5 ms, the other stages 4 ms) read "60 reachable" and took the quality away in play)
        ResetWindow();
        ResetHolds();
        overFrames_ = 0;
        pendingUp_ = 0;
        roomAt_ = 0;
        roomT_ = 0;
        minUp_ = 0;
        pendingBelow_ = false;
        p60OverT_ = 0;
        qgRoomT_ = 0;
        return false;
    }
    if (cfg_.calm && f.quiet) {
        // A quiet moment: not measured; a step up the last windows of play allowed is taken now.
        ResetWindow();
        overFrames_ = 0;
        if (cfg_.wipe && f.wipe) {
            // (Config::wipe: a closed fade or wipe opens on an area not measured yet: what the play before it showed is forgotten, no
            // step up is taken, and a rung above today's goes back to it now, while the screen is closed)
            pendingUp_ = 0;
            roomAt_ = 0;
            roomT_ = 0;
            minUp_ = 0;
            anisoRoomT_ = 0;
            if (WipeCap(rung_) != rung_ && sinceChange_ >= static_cast<unsigned>(cfg_.minChangeFrames)) {
                checking_ = false;
                SetRung(WipeCap(rung_), Why::Wipe, f.presentUs);
                return true;
            }
        }
        if (pendingUndo_ >= 0 && mayChange) {
            UndoBelow(f.presentUs);
            return true;
        }
        if (pendingBelow_ && mayChange && GpuGateOpen() && f.presentUs >= belowAt_ && static_cast<double>(f.presentUs - belowAt_) <= cfg_.roomMemorySec * 1e6 &&
            !checking_ && rung_ <= kToday && rung_ > cfg_.minRung) {
            const int from = rung_;
            pendingBelow_ = false;
            ++quietUps_;
            if (pathFrom_ == kNoRung) { pathFrom_ = from; pathInterval_ = belowInterval_; }
            SetRung(rung_ - 1, Why::GpuBound, f.presentUs);
            checking_ = true;
            checkFrom_ = from;
            checkBefore_ = belowBefore_;
            checkMissing_ = belowMissing_;
            checkSpan_ = 0;
            checkT_ = 0;
            checkFrames_ = 0;
            return true;
        }
        if (pendingUp_ > 0 && roomAt_ != 0 && !(cfg_.wipe && f.wipe) && f.presentUs >= roomAt_ && static_cast<double>(f.presentUs - roomAt_) <= cfg_.roomMemorySec * 1e6 && mayChange) {
            const int to = std::min(rung_ + pendingUp_, cfg_.maxRung);
            ++quietUps_;
            checking_ = false;
            return Climb(to, f.presentUs);
        }
        return false;
    }
    if (!f.timers && (f.gpuDoneUs == 0 || prevDone == 0)) return false;          // no GPU clock for this pair
    if (Target() && f.paced && !f.timers) {
        // The fence is sampled only after swap returns. A paced callback can
        // delay that observation beyond GPU completion: without exact timers,
        // keep the rung rather than mistake a deliberate wait for GPU work.
        ResetWindow();
        ResetHolds();
        pendingUp_ = 0;
        pendingBelow_ = false;
        g_pictureWorkUs.store(0, std::memory_order_relaxed);
        return false;
    }
    if (f.presentUs < settleUntil_) return false;                                // a change is settling
    // (s1117b, Config::gpuGate: this picture of play into the gate's ring; the clock must be the GPU timers: the fence clock's busy time holds
    // the back end's own waits, so it can not tell the GPU's work from a starved back end and the gate stays open for it, as before)
    if (cfg_.gpuGate && f.timers) GateNote(f, interval);
    const bool gateOpen = GpuGateOpen();
    // (Config::fast60, Performance: a sustained burst of pictures late for the GPU steps down now, not after the windows' holds; the
    // movie, the quiet moment, the stall and the settling are all behind us here, and the steady 30's pictures never enter the ring)
    if (Fast60() && !p23Hold && gateOpen && QgSettled() && !LadderOn()) { // (gpu896: the ladder owns the steps) // (Config::qg: not before the play has settled) (s1117b: and the GPU the limit, GpuGateOpen)
        if (const int r = FastStep(f.presentUs)) return r == 1;
    }
    // (Config::overload: the GPU's own busy time a picture, known exactly with its timers)
    const bool gpuFrame = cfg_.overload && Target() && f.timers && f.busyKnown && !LadderOn(); // (gpu896: no severe step beside the ladder)
    if (gpuFrame) {
        // A severe overload is acted on per picture, within overloadSec, not after windows of holds: pictures in a row whose GPU work
        // each reaches the budget, with a mean far over it.
        const double gms = static_cast<double>(f.gpuBusyUs) / 1000.0;
        // (Config::hold30: under the fixed cap the GPU timer counts waits as busy time: only a picture that came late for the GPU counts)
        if (gms >= SevereBudgetMs() && (!Hold30() || picLateGpu_)) {
            ovT_ += static_cast<double>(interval) / 1e6;
            ovGpuSum_ += gms;
            ++ovN_;
        } else {
            ovT_ = ovGpuSum_ = 0;
            ovN_ = 0;
        }
        const double mean = ovN_ ? ovGpuSum_ / ovN_ : 0.0;
        if (ovT_ >= cfg_.overloadSec && mean >= cfg_.overloadFrac * SevereBudgetMs() && sinceChange_ >= 6 &&
            !p23Hold && QgSettled() && (!P23() || P23ChangeAllowed(f.presentUs, true, 0.5)) &&
            (ovLastAt_ == 0 || f.presentUs >= ovLastAt_ + 1000000u) && (changes_ == 0 || f.presentUs >= last_.atUs + 1000000u)) {
            ovT_ = ovGpuSum_ = 0;
            ovN_ = 0;
            if (SevereStep(f.presentUs, mean)) return true;
        }
    }
    // One slow frame (a hitch): not counted. (Config::overload: unless the GPU was busy for most of it: that is the GPU's work, and at
    // 9 FPS every picture is over outlierMs, so no window would ever fill.)
    if (static_cast<double>(interval) > cfg_.outlierMs * 1000.0 && !(gpuFrame && static_cast<double>(f.gpuBusyUs) >= 0.75 * static_cast<double>(interval)))
        return false;
    double frameLoad = -1.0;
    if (f.timers) {
        if (f.busyKnown) {
            busy_ += f.gpuBusyUs;
            ++busyN_;
            if (interval) frameLoad = static_cast<double>(f.gpuBusyUs) / static_cast<double>(interval);
        }
    } else {
        const uint64_t doneSpan = f.gpuDoneUs > prevDone ? f.gpuDoneUs - prevDone : 0;
        const uint64_t from = std::max(prevDone, f.startUs);
        uint64_t busy = f.gpuDoneUs > from ? f.gpuDoneUs - from : 0;
        if (busy > doneSpan) busy = doneSpan;
        busy_ += busy;
        ++busyN_;
        span_ += doneSpan;
        if (doneSpan) frameLoad = static_cast<double>(busy) / static_cast<double>(doneSpan);
    }
    present_ += interval;
    ++frames_;
    if (Target()) {
        if (gameKnown) { gameRun_ += gameRun; gameTurns_ += gameTurns; }
        else gameKnown_ = false;
        if (frontPair && frontPictures < 16) { frontRun_ += frontRun; frontPictures_ += frontPictures; }
        if (f.cadence30) { ++cadenceFrames_; winCadLateGpu_ += picCadLateGpu_; }
        if (Hold30()) { ++winPics_; winLate_ += picLate_; winLateGpu_ += picLateGpu_; }
    }
    // Fast path: above today, a few frames in a row near the GPU's limit step down now.
    const int floorUp = std::max(kToday, cfg_.minRung);
    if (frameLoad >= 0.0) overFrames_ = frameLoad >= cfg_.saturated ? overFrames_ + 1 : 0;
    if (!Target() && rung_ > floorUp && overFrames_ >= static_cast<unsigned>(cfg_.fastFrames) && mayChange) {
        // One step (a short spike costs one rung); when the step before was one of these and did not help (the overload
        // lasts), as far down as this frame's load says. Changes are spaced (minChangeFrames), so one rung at a time would
        // take a second or more for each.
        int to = rung_ - 1;
        if (last_.why == Why::Saturated && last_.to == rung_)
            while (to > floorUp && frameLoad * Cost(to) / Cost(rung_) > cfg_.upTarget) --to;
        winLoad_ = frameLoad;
        winIntervalMs_ = static_cast<double>(interval) / 1000.0;
        Block(rung_, f.presentUs);
        checking_ = false;
        SetRung(to, Why::Saturated, f.presentUs);
        return true;
    }
    if (f.backendRunUs != 0 && prevRun != 0 && f.backendRunUs >= prevRun) run_ += f.backendRunUs - prevRun;
    else runKnown_ = false;
    // (Config::seam / Config::seam72: a step held for a cut is decided at the cut, from the window so far (4 pictures at least), once a cut)
    const bool seamEarly = ((cfg_.seam && seamSince_ != 0) || (cfg_.seam72 && seam72Since_ != 0)) && seamLastCutUs_ != 0 && seamEvalCut_ != seamLastCutUs_ && frames_ >= 4u &&
        f.presentUs >= seamLastCutUs_ && static_cast<double>(f.presentUs - seamLastCutUs_) <= cfg_.seamCutMemSec * 1e6;
    if (frames_ < static_cast<unsigned>(cfg_.windowFrames) && !seamEarly) return false;
    if (seamEarly) seamEvalCut_ = seamLastCutUs_;
    const bool changed = Evaluate(f.presentUs, mayChange && !p23Hold && (!P23() || P23ChangeAllowed(f.presentUs, false)));
    ResetWindow();
    return changed;
}

// s1117b (Config::gpuGate): a picture of play into the GPU gate's ring. Late is judged against the budget (against 33.3 ms for a picture on the
// steady 30, which is shown at that rate on purpose). A picture without a busy time of its own counts as late or not, but not in the busy share.
void Controller::GateNote(const FrameTimes& f, uint64_t interval) {
    const double budgetMs = f.cadence30 ? std::max(cfg_.budgetMs, 1000.0 / 30.0) : cfg_.budgetMs;
    const bool late = static_cast<double>(interval) > budgetMs * 1030.0;
    const uint32_t frames = std::max<uint32_t>(1u, std::min<uint32_t>(f.gpuFrames, 4u));
    const uint64_t periodUs = std::min<uint64_t>(interval, static_cast<uint64_t>(budgetMs * 1000.0));
    gate_.Add(periodUs, f.busyKnown ? static_cast<uint64_t>(f.gpuBusyUs) * frames : 0u, f.busyKnown, late);
    if (!GpuGateOpen()) ++gateClosedPics_;
}

// Config::fast60: a picture into the ring of the last ones. Not counted (and the ring starts again): a quiet moment, a movie, a stall,
// a picture on the steady 30 (the pacing's deliberate 30 is not a miss of 60). Not counted (the ring goes on): the settling after a change.
void Controller::FastNote(const FrameTimes& f, uint64_t interval, bool lateGpu, uint64_t gpuUs, uint64_t otherUs) {
    if (f.quiet || f.movie || f.cadence30 || static_cast<double>(interval) > cfg_.gapResetMs * 1000.0) { fastN_ = 0; return; }
    if (P23() && f.presentUs < p23SettleUntil_) { fastN_ = 0; return; } // (GXV_PACE23: the settle after a load: nothing counts)
    if (f.presentUs < settleUntil_) return;
    FastPic& p = fastRing_[fastHead_];
    p.intervalUs = static_cast<uint32_t>(std::min<uint64_t>(interval, 0xFFFFFFFFu));
    p.gpuUs = static_cast<uint32_t>(std::min<uint64_t>(gpuUs, 0xFFFFFFFFu));
    p.otherUs = static_cast<uint32_t>(std::min<uint64_t>(otherUs, 0xFFFFFFFFu));
    p.lateGpu = lateGpu;
    fastHead_ = (fastHead_ + 1) % kFastRing;
    if (fastN_ < kFastRing) ++fastN_;
}

// Config::fast60: a sustained burst (fast60Late of the last fast60Pics pictures late for the GPU, within fast60SpanSec) takes ONE step
// down, in the order of the other rules: anisotropic filtering, the present's edge smoothing (at today's rung), then a rung. A picture late
// for the CPU stages is not marked late for the GPU (picLateGpu_), so it never counts. Returns 0: nothing, 1: the rung changed, 2: a step or
// anisotropic filtering changed (the rung did not).
int Controller::FastStep(uint64_t now) {
    const unsigned k = static_cast<unsigned>(cfg_.fast60Pics);
    if (fastN_ < k) return 0;
    // (the means are those of the pictures late for the GPU: at the start of a burst the ring still holds pictures from before it)
    unsigned late = 0;
    double spanUs = 0, gpuUs = 0, otherUs = 0, lateIntervalUs = 0;
    for (unsigned i = 0; i < k; ++i) {
        const FastPic& p = fastRing_[(fastHead_ + kFastRing - 1 - i) % kFastRing];
        spanUs += p.intervalUs;
        if (!p.lateGpu) continue;
        ++late;
        lateIntervalUs += p.intervalUs;
        gpuUs += p.gpuUs;
        otherUs += p.otherUs;
    }
    if (late < static_cast<unsigned>(cfg_.fast60Late) || spanUs > cfg_.fast60SpanSec * 1e6) return 0;
    const uint64_t gap = static_cast<uint64_t>(cfg_.fast60GapSec * 1e6);
    if ((changes_ && now < last_.atUs + gap) || (fxEver_ && now < lastFxAt_ + gap) || (anisoAt_ != 0 && now < anisoAt_ + gap)) return 0;
    if (pendingUndo_ >= 0) return 0; // (a series that bought nothing is being undone)
    const double gpuMs = gpuUs / late / 1000.0, otherMs = otherUs / late / 1000.0, intervalMs = lateIntervalUs / late / 1000.0;
    // (Config::qg: pictures that came qgGpuHoldFrac slower than the GPU and the other stages, ALL of the last fast60Pics (a late picture alone is
    // always 2+ retraces: with "Up to 60" a 20 ms GPU gives a 1-2 retrace mix, ~22 ms on average), were held by something else (the fixed 30
    // cap, a wait, the pacing): no step buys one of them. Console 491-c9SF1: every picture 33.3 ms, GPU 19-20 ms, 720 -> 664 -> 608 by this rule.)
    if (QG() && spanUs / k / 1000.0 > cfg_.qgGpuHoldFrac * std::max(gpuMs, otherMs)) return 0;
    winLoad_ = gpuMs / cfg_.budgetMs;
    winIntervalMs_ = intervalMs;
    if (aniso_ && !(QG() && now < qgAnisoKeepUntil_)) { // (Config::qg: not while an earlier "off" that bought nothing is remembered)
        // (what means least goes first, as in the other rules: off, and on again blocked as after an overload)
        anisoBlockLen_ = anisoBlockLen_ > 0 ? std::min(anisoBlockLen_ * 2.0, cfg_.maxBlockSec) : cfg_.blockSec;
        anisoBlockedUntil_ = now + static_cast<uint64_t>(anisoBlockLen_ * 1e6);
        lastBlockAt_ = now;
        SetAniso(false, Why::AnisoOff, now);
        ++fastSteps_;
        return 2;
    }
    // (GXV_PACE60: 60 out of reach even at the floor rung with every step: nothing is given up for it, the steady 30 is the way)
    if (P60Holds()) return 0;
    if (ShedOn()) { // (Config::shed: the table's order; 0 here means the table does not own this state: the rules below)
        const int r = ShedDownStep(now, gpuMs, otherMs, false);
        if (r == 1) return 1;
        if (r == 2) return 2;
        if (r == 3) return 0;
    }
    if (ShedOn() && rung_ <= FxRung() && !cfg_.shedBelow) return 0; // (Config::shed: the table ends at today's rung; the older series below is off unless GXV_AUTOQ_SHED_BELOW=1)
    if (rung_ == FxRung() && cfg_.edgeStep && !HoldEdgeRule() && !(steps_ & 4) && !trial_ && now >= onBlockedUntil_[2]) {
        // (the present's edge smoothing off: no new render target, no freeze; judged by its own GPU-timed trial as in Evaluate)
        fxBefore_ = gpuMs;
        fxBoundKept_ = 0;
        fxOffBlockLen_ = lastFxOffAt_ != 0 && now - lastFxOffAt_ < static_cast<uint64_t>(cfg_.fxRepeatSec * 1e6) && fxOffBlockLen_ > 0
            ? std::min(fxOffBlockLen_ * 2.0, cfg_.maxBlockSec) : cfg_.fxOffBlockSec;
        fxOffBlockedUntil_ = now + static_cast<uint64_t>(fxOffBlockLen_ * 1e6);
        SetSteps(steps_ | 4u, Why::FxOn, now);
        trial_ = 4u;
        fxCheckT_ = fxCheckSum_ = 0;
        fxCheckFrames_ = 0;
        ++fastSteps_;
        return 2;
    }
    if (rung_ <= cfg_.minRung) return 0; // (the floor rung: the steady 30 is the way out, as before)
    // Not when even the floor rung cannot hold ~fast60ReachFps: its GPU time predicted by the pixels (the most a smaller size could save),
    // or another stage, is over 1000 / fast60ReachFps ms.
    const double floorGpuMs = P60Ready() ? P60Floor() : P23() ? P23Pred(gpuMs, rung_, cfg_.minRung) : gpuMs * Cost(cfg_.minRung) / Cost(rung_);
    if (std::max(floorGpuMs, otherMs) > 1000.0 / cfg_.fast60ReachFps) return 0;
    // (GXV_PACE23: every change of size costs 30-150 ms and the 2.2 crash was three of them in 2 s: spaced, rate limited)
    if (P23() && !P23ChangeAllowed(now, true)) return 0;
    const int from = rung_;
    // (GXV_PACE23: the smallest size whose predicted GPU time fits, in one change; at least one rung)
    auto direct = [&](int lowest) {
        int to = from - 1;
        // (GXV_PACE60: the largest size whose modelled GPU time, with the steps on now, fits p23Fit (92 %) of the budget)
        if (P60Ready()) while (to > lowest && Pace60Predict(to, StepsNow()) > cfg_.p23Fit * cfg_.budgetMs) --to;
        else while (P23() && to > lowest && P23Pred(gpuMs, from, to) > cfg_.p23Fit * cfg_.budgetMs) --to;
        if (to < from - 1) {
            ++p23_.direct;
            for (int r = to + 1; r < from; ++r) Block(r, now); // (the sizes passed over are as slow as the one left)
        }
        return to;
    };
    if (from > kToday) {
        if (QG() && now < belowBlockedUntil_) return 0; // (Config::qg: a size reduction that bought nothing: none for its block)
        // (above today's size: as an overload there, the rung left blocked (blockSec, doubling): no climb into a size just too slow)
        Block(from, now);
        checking_ = false;
        ++fastSteps_;
        SetRung(direct(std::max(kToday, cfg_.minRung)), Why::FastDown, now);
        return 1;
    }
    // At or below today's size it is a step of the existing series of steps below, judged as the others (the GPU's time after belowCheckSec,
    // the pictures, the scene's remembered floor, the blocks of a series that bought nothing): not when those rules say it would not gain.
    if (now < belowBlockedUntil_ || (sceneFloor_ != kNoRung && rung_ <= sceneFloor_ && now < sceneFloorUntil_)) return 0;
    if (std::max({floorGpuMs, otherMs, cfg_.budgetMs}) >= intervalMs * (1.0 - cfg_.pathRateFrac)) return 0;
    if (pathFrom_ == kNoRung) { pathFrom_ = from; pathInterval_ = intervalMs; }
    const bool series = checking_; // (a step while the last was still being judged: the judged start stays, the clock starts again)
    const int checkFrom = series ? checkFrom_ : from;
    const double before = series ? checkBefore_ : gpuMs;
    Block(from, now); // (the hysteresis: the size just left is too slow; no climb back into it for blockSec, doubling)
    ++fastSteps_;
    SetRung(direct(cfg_.minRung), Why::FastDown, now);
    checking_ = true;
    checkFrom_ = checkFrom;
    checkBefore_ = before;
    checkMissing_ = true;
    checkSpan_ = 0;
    checkT_ = 0;
    checkFrames_ = 0;
    return 1;
}

// Config::hold30: a decision window's pictures go into the hold window; one that has lasted holdWindowSec is judged: a miss (more than
// holdMissFrac of its pictures late for the GPU), calm (none late for the GPU, holdOnTimeFrac on time, the GPU's mean busy time within
// holdClimbGpuFrac of the budget), or neither; the runs of each in a row are what Evaluate decides on.
void Controller::HoldAccumulate(double dt, double gpuMs) {
    holdT_ += dt;
    holdPics_ += winPics_;
    holdLate_ += winLate_;
    holdLateGpu_ += winLateGpu_;
    holdGpuSum_ += gpuMs * frames_;
    holdGpuN_ += frames_;
    if (holdT_ < cfg_.holdWindowSec || holdPics_ == 0) return;
    HoldWindow w;
    w.pictures = holdPics_;
    w.late = holdLate_;
    w.lateGpu = holdLateGpu_;
    w.gpuMs = holdGpuN_ ? holdGpuSum_ / holdGpuN_ : 0.0;
    w.miss = static_cast<double>(w.lateGpu) > cfg_.holdMissFrac * w.pictures;
    w.calm = w.lateGpu == 0 && static_cast<double>(w.pictures - w.late) >= cfg_.holdOnTimeFrac * w.pictures &&
             w.gpuMs <= cfg_.holdClimbGpuFrac * cfg_.budgetMs;
    missRun_ = w.miss ? missRun_ + 1 : 0;
    calmRun_ = w.calm ? calmRun_ + 1 : 0;
    // (the GPU over the run of miss windows: what edge smoothing's saving is measured against)
    if (w.miss) { missGpuSum_ += w.gpuMs * holdGpuN_; missGpuN_ += holdGpuN_; }
    else { missGpuSum_ = 0; missGpuN_ = 0; }
    if (edgeMeasuring_ && holdEdge_ && holdGpuN_) {
        edgeMeasuring_ = false;
        edgeSave_ = std::max(0.0, edgeBefore_ - w.gpuMs);
    }
    lastHold_ = w;
    ++holdWindows_;
    holdT_ = holdGpuSum_ = 0;
    holdPics_ = holdLate_ = holdLateGpu_ = holdGpuN_ = 0;
}

bool Controller::Evaluate(uint64_t now, bool mayChange) {
    // (Config::hold30: a window whose GPU timers never reported, with pictures late for the GPU by elimination: the GPU is taken as busy
    // for the whole picture interval, so the window is judged instead of being skipped while the pictures stay late)
    if (present_ != 0 && busyN_ == 0 && frames_ != 0 && Target() && Hold30() && winLateGpu_ > 0) {
        busy_ = present_;
        busyN_ = frames_;
    }
    if (present_ == 0 || busyN_ == 0) {
        if (Target()) g_pictureWorkUs.store(0, std::memory_order_release);
        return false;
    }
    double load;
    if (span_ == 0) {
        // GPU timers: mean busy time / mean frame interval (frames the watcher had not finished by a present have no busy time)
        load = (static_cast<double>(busy_) / busyN_) / (static_cast<double>(present_) / frames_);
        if (load > 1.2) load = 1.2;
    } else {
        load = static_cast<double>(busy_) / static_cast<double>(span_);
    }
    const double cpu = runKnown_ ? static_cast<double>(run_) / static_cast<double>(present_) : -1.0;
    const double dt = static_cast<double>(present_) / 1e6;
    winLoad_ = load;
    winCpu_ = cpu;
    winIntervalMs_ = static_cast<double>(present_) / 1000.0 / frames_;
    const bool target = Target();
    const double gpuMs = target ? static_cast<double>(busy_) / busyN_ / 1000.0 : 0.0;
    const double backendMs = target && runKnown_ ? static_cast<double>(run_) / frames_ / 1000.0 : -1.0;
    // Two 60 Hz game turns are required per 30 Hz picture, one per 60 Hz picture. Use CPU work, not
    // retrace/pacing waits, and a maximum: all three pipeline stages overlap.
    const double gameMs = target && gameKnown_ && gameTurns_ ? static_cast<double>(gameRun_) * PictureTurns() / gameTurns_ / 1000.0 : -1.0;
    // (the pacing hint below stays a 30 Hz cost, two turns, whatever the target: the 20/30 pacing reads it)
    const double game2Ms = target && gameKnown_ && gameTurns_ ? static_cast<double>(gameRun_) * 2.0 / gameTurns_ / 1000.0 : -1.0;
    const bool wholeKnown = gameMs >= 0.0 && backendMs >= 0.0;
    // The front end's CPU per picture (-1: not known; it is then not a stage, as before it was measured)
    const double frontSeen = target && frontPictures_ ? static_cast<double>(frontRun_) / frontPictures_ / 1000.0 : -1.0;
    const double frontMs = FrontStage() ? frontSeen : -1.0;   // (Config::frontStage: a stage of the decisions in target 60 by default)
    const double otherMs = std::max({gameMs, backendMs, frontMs}); // the stages a smaller picture does not shorten
    winOtherMs_ = otherMs;
    winFrontMs_ = frontSeen;
    winGameMs_ = gameMs;
    winBackendMs_ = backendMs;
    winWorkMs_ = std::max(gpuMs, otherMs);
    // (Config::qg: the windows of play since the last load, new scene or stretch of pictures that were not play; until qgPlayWindows of them
    // and qgPlaySec have passed nothing is decided, and pace60's verdict waits. The recent play, smoothed over ~2 s, is what a reduction's
    // trial compares against.)
    bool qgSettled = true;
    if (QG() && target) {
        qgSettled = QgSettled();
        if (!qgSettled) {
            ++qgPlayWins_;
            qgPlayT_ += dt;
            if (otherMs >= 0.0) { qgSettleOtherSum_ += otherMs; ++qgSettleOtherN_; }
        }
        const double a = qgRecent_ ? std::min(1.0, dt / 2.0) : 1.0;
        qgIv_ += a * (winIntervalMs_ - qgIv_);
        qgGpu_ += a * (gpuMs - qgGpu_);
        qgOther_ += a * (std::max(0.0, otherMs) - qgOther_);
        qgLastIv_ = winIntervalMs_;
        qgLastGpu_ = gpuMs;
        qgRecent_ = true;
    }
    if (P60() && qgSettled) P60Verdict(dt, otherMs); // (GXV_PACE60: 60 reachable or not, with hysteresis)
    if (target) {
        const uint32_t work = wholeKnown && hintKnown_ ? std::max(currentWorkUs_,
            static_cast<uint32_t>(std::min(std::max({gpuMs, game2Ms, backendMs, frontMs}) * 1000.0, 1000000.0))) : 0;
        // Invalidate before changing stamp. The getter brackets work with stamp/fault reads, so it cannot pair old work with a new fault generation.
        g_pictureWorkUs.store(0, std::memory_order_release);
        g_pictureFaults.store(prevGameFaults_, std::memory_order_release);
        g_pictureWorkUs.store(work, std::memory_order_release);
    }
    const double measureMs = target ? winWorkMs_ : winIntervalMs_;
    if (target) winLoad_ = gpuMs / cfg_.budgetMs;
    ++windows_;
    const bool fxActive = StepsEnabled() && target;
    if (target) {
        // (diagnostics: anisotropic filtering's measured cost; nothing below reads it)
        if (anisoMeasuring_ && aniso_) {
            anisoSum_ += gpuMs * frames_;
            anisoFrames_ += frames_;
            anisoT_ += dt;
            if (anisoT_ >= cfg_.checkSec && anisoFrames_) {
                anisoMeasuring_ = false;
                anisoMeasured_ = true;
                anisoBeforeMs_ = anisoBefore_;
                anisoAfterMs_ = anisoSum_ / anisoFrames_;
            }
        } else {
            anisoMeasuring_ = false;
        }
        lastGpuMs_ = gpuMs;
        if (Hold30()) HoldAccumulate(dt, gpuMs);
    }
    // (Config::freeFrac: the share of the window's picture interval the GPU may be predicted to need after a step up; 0: no such limit)
    const double freeMs = target && cfg_.freeFrac > 0.0 ? cfg_.freeFrac * winIntervalMs_ : 1e30;

    if (trial_) {
        // This existing trial judges GPU busy time, not the full pipeline's CPU cost or displayed cadence.
        fxCheckT_ += dt;
        fxCheckSum_ += gpuMs * frames_;
        fxCheckFrames_ += frames_;
        // (Config::edgeCheck, target 60: the edge step's trial is judged over edgeCheckSec and kept for a fall of the smaller of half its
        // assumed saving and edgeBetterFrac (3 %); the console, Good Egg at 720x404 pinned: 20.9 -> 18.4 ms of GPU a picture without it, 12
        // %, yet a 2 s trial in play read 117 % -> 111 % of the budget through the camera's noise and threw it away as buying nothing)
        const bool edgeTrial = cfg_.edgeCheck && trial_ == 4u;
        if (fxCheckT_ >= (edgeTrial ? cfg_.edgeCheckSec : cfg_.checkSec) && fxCheckFrames_ > 0) {
            const unsigned bit = trial_;
            const int idx = StepIdx(bit);
            trial_ = 0;
            const double after = fxCheckSum_ / fxCheckFrames_;
            const double need = edgeTrial ? std::min(0.5 * (1.0 - 1.0 / std::max(cfg_.edgeRatio, 1.0)), cfg_.edgeBetterFrac)
                                          : cfg_.betterFrac;
            // (Config::edgeCheck: the edge step is not undone for buying too little: its saving is the present's own work, about 3 ms a
            // picture whatever the scene (console, Good Egg 720x404: 20.9 -> 18.4 ms), and in play the scene's own swings hide it (r99:
            // 118 % -> 115 % in Good Egg, 109 % -> 116 % in Bowser 3, each then undone, each scene stepping down to 608x348 with it on).
            // A measured fall is kept as its gain; otherwise the assumed edgeRatio stands, and it comes back on with room as the others.)
            // (Config::qg: the guard's trial judges every step by the pictures it buys (QgJudge); a GPU-time verdict here would undo a step
            // that bought pictures through the camera's noise: console c17, Good Egg)
            if (!QG() && !edgeTrial && after > fxBefore_ * (1.0 - need) && after < fxBefore_ * 1.15) {
                // (as for a step below today: when the frames got much longer the scene got heavier, which says nothing against the step)
                onBlockLen_[idx] = onBlockLen_[idx] > 0 ? std::min(onBlockLen_[idx] * 2.0, cfg_.maxBlockSec) : cfg_.blockSec;
                onBlockedUntil_[idx] = now + static_cast<uint64_t>(onBlockLen_[idx] * 1e6);
                lastBlockAt_ = now;
                stepGain_[idx] = 0;
                SetSteps(steps_ & ~bit, Why::FxNoGain, now);
                boundT_ = std::max(boundT_, fxBoundKept_); // (the GPU has been the limit all this time: the next step need not wait again)
                return false;
            }
            if (after > 0.0 && after < fxBefore_ * (1.0 - need)) stepGain_[idx] = std::clamp(fxBefore_ / after, 1.05, 3.0);
        }
    }

    if (checking_) {
        // A step below today's size is kept only if the frames got shorter (target mode: if the GPU's busy time per picture,
        // what a smaller picture is for, fell by a fair share of what the sizes predict; the other stages do not shrink with
        // the picture, and the frame interval is paced).
        checkT_ += dt;
        checkSpan_ += target ? gpuMs * 1000.0 * frames_ : static_cast<double>(present_);
        checkFrames_ += frames_;
        if (target ? checkT_ >= cfg_.belowCheckSec : checkSpan_ >= cfg_.checkSec * 1e6) {
            checking_ = false;
            const double after = checkSpan_ / 1000.0 / checkFrames_;
            double need = cfg_.betterFrac;
            // (target mode: while the floor was missed, any real saving of GPU time is kept, missGainFrac (3 %): in Beach Bowl each
            // size saves only 4-6 %, and together those steps are what holds 30 (the console, rc2: kept at 640x456 by the stricter
            // rule it showed 26 a second, with them 31). Once the floor is met, a step that only adds pictures above the target
            // must buy half of the saving the sizes predict.)
            if (target && rung_ < checkFrom_)
                need = checkMissing_ ? cfg_.missGainFrac : std::max(need, cfg_.gainShare * (1.0 - Cost(rung_) / Cost(checkFrom_)));
            // (undone only when the frames stayed about as long: when they got much longer the scene got heavier during
            // the check, which says nothing against the smaller picture; the Strikers port, cc302e6)
            // (Config::qg: as for the steps: a smaller size is judged by the pictures it buys, QgJudge; no GPU-time undo, no scene floor from it)
            if (QG()) {
                if (target && sceneFloor_ != kNoRung && checkFrom_ <= sceneFloor_ && after <= checkBefore_ * (1.0 - need)) { sceneFloor_ = kNoRung; sceneFloorUntil_ = 0; }
            } else if (after > checkBefore_ * (1.0 - need) && after < checkBefore_ * 1.15) {
                belowBlockLen_ = belowBlockLen_ > 0 ? std::min(belowBlockLen_ * 2.0, cfg_.maxBlockSec) : cfg_.blockSec;
                belowBlockedUntil_ = now + static_cast<uint64_t>(belowBlockLen_ * 1e6);
                lastBlockAt_ = now;
                // (the scene remembers it with its rung; a step undone while the floor was missed only when it saved nothing at all:
                // a saving lost in the noise of one check must not keep the scene above a size its floor needs on every visit)
                if (target && (!checkMissing_ || after > checkBefore_ * (1.0 - cfg_.nothingFrac))) sceneFloor_ = std::max(sceneFloor_, checkFrom_);
                // (target 60: a step that bought no GPU time ends its series of steps below; when the steps before it, kept while
                // the GPU was still over the budget, showed no more pictures either, the undo goes back to where the series began)
                if (cfg_.targetFps == 60 && pathFrom_ != kNoRung && checkFrames_ > 0 &&
                    checkT_ * 1000.0 / checkFrames_ > pathInterval_ * (1.0 - cfg_.pathRateFrac) && pathFrom_ > checkFrom_) {
                    sceneFloor_ = std::max(sceneFloor_, pathFrom_);
                    sceneFloorUntil_ = now + static_cast<uint64_t>(cfg_.maxBlockSec * 1e6); // (this visit too, not only the next)
                    checkFrom_ = pathFrom_;
                    pathFrom_ = kNoRung;
                }
                if (cfg_.calm) {
                    pendingUndo_ = checkFrom_; // (calm: back at the next quiet moment; target mode: within noGainUndoSec)
                    undoDueAt_ = now + static_cast<uint64_t>(cfg_.noGainUndoSec * 1e6);
                } else {
                    SetRung(checkFrom_, Why::NoGain, now);
                    return true;
                }
            } else {
                if (target && sceneFloor_ != kNoRung && checkFrom_ <= sceneFloor_) {
                    sceneFloor_ = kNoRung; // (a step from there bought speed this time: what the scene learnt before no longer holds)
                    sceneFloorUntil_ = 0;
                }
                if (cfg_.targetFps == 60 && pathFrom_ != kNoRung && checkFrames_ > 0) {
                    // Performance mode is for pictures: the series of steps below that this one belongs to must show more of them.
                    // A step that does not yet (pictures come at whole retraces: 20 ms of GPU and 17 ms both give 30 a second) is
                    // kept while the GPU is still over the budget and a smaller size is left; once it is not, or none is, and
                    // the pictures came no faster (pathRateFrac, 3 %), the whole series goes back to where it began: something
                    // else held the pictures (the front end, the game), and every size lost was for nothing.
                    const double afterInterval = checkT_ * 1000.0 / checkFrames_;
                    if (afterInterval <= pathInterval_ * (1.0 - cfg_.pathRateFrac)) {
                        pathFrom_ = kNoRung; // (more pictures: the series did its work; a later one is judged from there)
                    } else if (after < cfg_.budgetMs * cfg_.downBelow || rung_ <= cfg_.minRung) {
                        belowBlockLen_ = belowBlockLen_ > 0 ? std::min(belowBlockLen_ * 2.0, cfg_.maxBlockSec) : cfg_.blockSec;
                        belowBlockedUntil_ = now + static_cast<uint64_t>(belowBlockLen_ * 1e6);
                        lastBlockAt_ = now;
                        sceneFloor_ = std::max(sceneFloor_, pathFrom_);
                        // (a whole series that showed no more pictures is not tried again in this visit for maxBlockSec either: each try
                        // is several frozen pictures; a later visit recalls it too, RecallFloor)
                        sceneFloorUntil_ = now + static_cast<uint64_t>(cfg_.maxBlockSec * 1e6);
                        const int back = pathFrom_;
                        pathFrom_ = kNoRung;
                        if (cfg_.calm) {
                            pendingUndo_ = back;
                            undoDueAt_ = now + static_cast<uint64_t>(cfg_.noGainUndoSec * 1e6);
                        } else {
                            SetRung(back, Why::NoGain, now);
                            return true;
                        }
                    }
                }
            }
        }
    }

    // A paced 50 ms interval is not 50 ms of GPU work. Missed cadence is useful
    // only alongside actual GPU work near the 30 Hz budget and a CPU margin.
    // (Config::overload: a GPU over 110 % of the budget on its own misses the target whatever the game's estimate says: the game's CPU
    // time rises with the GPU's memory traffic, and the r41 drop-in had both over the budget)
    // (target 60: the front end is one of the stages when measured; and a window whose pictures the pacing showed on its steady 30, a
    // CPU-limited scene, is judged against 33.3 ms: a smaller picture is for a GPU that misses even that, DownBudgetMs)
    const double downMs = target ? DownBudgetMs() : cfg_.budgetMs;
    // (Config::qg, off a 30 cadence: not when the pictures came qgGpuHoldFrac slower than the GPU's own time and the other stages': something
    // else holds them (the fixed 30 cap, a wait, the pacing), and no smaller GPU work brings one more; console 491-c9SF1, Honeyhive: a picture
    // every 33.3 ms with the GPU at 19-20 ms, and fast60 / this rule took 720 -> 664 -> 608 for no picture)
    const bool qgNotGpuHeld = QG() && !CadenceWindow() && winIntervalMs_ > cfg_.qgGpuHoldFrac * std::max(gpuMs, otherMs);
    // (s1117b, Config::gpuGate: the windows' over / bound need the GPU to be what holds the pictures. For the target rules' own tests (the GPU's mean
    // busy time at 95 % of the budget or more, the severe overload's, the free-down's, the legacy fast path's 97 % load) the gate is open by what they
    // already demand, so they carry no AND of it; it bites where the verdict is by elimination: hold30's late pictures (the front end or a wait blamed on
    // the GPU) and fast60's, and a step held for a quiet moment.)
    const bool gateOpen = GpuGateOpen();
    const bool gpuBound = target && wholeKnown && gpuMs >= downMs * cfg_.downBelow &&
        (gpuMs >= otherMs * 1.05 || (cfg_.overload && gpuMs >= downMs * 1.1 && gpuMs >= std::max(backendMs, frontMs) * 1.05)) &&
        (winIntervalMs_ > downMs * 1.03 || winWorkMs_ > downMs) && !qgNotGpuHeld;
    // (Config::hold30, Quality under the fixed cap: what the player sees decides, not the GPU's busy share: holdMissWindows hold windows in
    // a row with pictures late for the GPU, one within a climb's trial. It stands for the GPU-limited test of every step down below.)
    const bool hold30 = Hold30();
    const bool holdTrial = hold30 && HoldTrial(now);
    const bool edgeTrial = HoldEdgeRule() && holdEdge_ == false && now < edgeTrialUntil_; // (edge smoothing on again, on trial)
    const bool holdMiss = hold30 && missRun_ >= (holdTrial || edgeTrial ? 1 : cfg_.holdMissWindows);
    // (GXV_PACE60, Config::p60NoChase: 60 out of reach by the model even at the floor rung with every step: off the steady 30 a GPU-limited
    // stretch at or below today's size takes nothing away, the steady 30 shows an even 30 at this size; on it, DownBudgetMs (33.3 ms) decides)
    const bool p60Hold = P60Holds() && cfg_.p60NoChase && !CadenceWindow() && rung_ <= kToday;
    if (p60Hold && gpuBound) ++p60_.heldSteps;
    const bool over = gateOpen && (hold30 ? holdMiss : target ? gpuBound && !p60Hold : load >= cfg_.downAbove);
    const bool bound = gateOpen && (hold30 ? holdMiss : target ? gpuBound && !p60Hold : load >= cfg_.downBelow && cpu >= 0.0 && cpu <= cfg_.cpuMax);
    g_ladderRoom.store(LadderRoomDown() ? 1u : 0u, std::memory_order_relaxed);   // (gov5: before the step, so the verdict covers the one about to be taken)
    if (LadderOn()) return LadderStep(now, dt, mayChange, bound, target && wholeKnown && hintKnown_, gpuMs); // (gpu896: the Performance ladder owns every size and quality step of this stage)
    if (!gateOpen && (hold30 ? holdMiss : target ? wholeKnown && gpuMs >= downMs * cfg_.downBelow : load >= cfg_.downAbove)) ++gateVetoes_;   // (stopped by the gate)
    // How far up the prediction allows (at most maxUpStep rungs at once, never past a blocked rung, and onto a rung that
    // has failed before but not past it: such a rung is tried again on its own).
    // (Calm mode: the GPU's busy time against the frame budget as well, so a frame the CPU made long does not hide it.)
    double upLoad = load;
    if (!target && cfg_.calm && winIntervalMs_ > 0.0) {
        const double busyMs = std::min(load, 1.2) * winIntervalMs_;
        upLoad = std::max(load, busyMs / std::min(winIntervalMs_, cfg_.budgetMs));
    }
    // (Config::cadenceUp, target 60: a window mostly on the steady 30 (as DownBudgetMs) at or below today's rung is also judged against the
    // 33.3 ms it is shown at for the way back: the cadence holds (the mean interval within 3 % of it), none of its pictures came late for
    // the GPU on it, and 60 is out of reach whatever the size (the floor rung's predicted GPU time, falling with the pixels, or the other
    // stages at 1000 / cadenceReachFps ms or more: the steady 30 is not released then, frame_pace.h kSteady30AboveFps), so a scene a
    // smaller picture could bring to 60 is not held at 30 by a larger one; the prediction and the other stages within cadenceUpFrac of 33.3
    // ms.)
    const double cadMs = std::max(cfg_.budgetMs, 1000.0 / 30.0), reachMs = 1000.0 / cfg_.cadenceReachFps;
    const bool cadenceRoom = cfg_.cadenceUp && target && CadenceWindow() && wholeKnown && hintKnown_ && rung_ <= kToday &&
        std::abs(winIntervalMs_ - cadMs) <= cadMs * 0.03 && winCadLateGpu_ == 0 &&
        (P60Ready() ? (!p60_.reach || otherMs >= reachMs) : (gpuMs * Cost(cfg_.minRung) / Cost(rung_) >= reachMs || otherMs >= reachMs));
    auto cadenceFits = [&](double gpuPredMs) { return cadenceRoom && std::max(otherMs, gpuPredMs) <= cfg_.cadenceUpFrac * cadMs; };
    int up = 0;
    // Unknown front-end/shared-core work may also miss the target. Do not add
    // GPU quality until actual cadence fits, even if measured stages look cheap.
    // (the step belongs to the bottom rungs: while it is on, the climb stops at the bottom ordinary rung; the step goes off first)
    const int upMax = fxActive && steps_ ? std::min(cfg_.maxRung, FxRung()) : cfg_.maxRung;
    // (target mode, below today: a free climb when another stage holds the pictures above the budget and the GPU at the next
    // size stays clearly under it (freeClimbFrac): the smaller picture buys no picture, so it is not kept; never above today)
    auto freeClimb = [&](int to) {
        return to <= kToday && wholeKnown && hintKnown_ && otherMs > cfg_.budgetMs * 1.03 &&
               gpuMs * Cost(to) / Cost(rung_) <= cfg_.freeClimbFrac * otherMs;
    };
    while (up < cfg_.maxUpStep && rung_ + up + 1 <= upMax && !Blocked(rung_ + up + 1, now) &&
           (target ? (wholeKnown && hintKnown_ && winIntervalMs_ <= cfg_.budgetMs * 1.03 &&
            std::max(otherMs, gpuMs * Cost(rung_ + up + 1) / Cost(rung_)) <= cfg_.upTarget * cfg_.budgetMs &&
            gpuMs * Cost(rung_ + up + 1) / Cost(rung_) <= freeMs) || freeClimb(rung_ + up + 1) :
            upLoad * Cost(rung_ + up + 1) / Cost(rung_) <= cfg_.upTarget)) {
        ++up;
        if (blockLen_[rung_ + up + 1] > 0) break;
    }
    // (Config::cadenceUp: where the rules above allow no more, the steady 30's room: never above today's rung, never past a blocked one,
    // onto a rung that failed before but not past it, as above)
    bool cadenceUp = false;
    if (cadenceRoom) {
        int cup = 0;
        const int cadMax = std::min(upMax, kToday);
        while (cup < cfg_.maxUpStep && rung_ + cup + 1 <= cadMax && !Blocked(rung_ + cup + 1, now) &&
               cadenceFits(gpuMs * Cost(rung_ + cup + 1) / Cost(rung_))) {
            ++cup;
            if (blockLen_[rung_ + cup + 1] > 0) break;
        }
        if (cup > up) {
            up = cup;
            cadenceUp = true;
        }
    }
    const bool room = up > 0;
    overT_ = over ? overT_ + dt : 0.0;
    roomT_ = room ? roomT_ + dt : 0.0;
    // (a stretch of room that one window owed to the steady 30 climbs as the steady 30's: today's rung at most, on trial, Climb)
    roomCadence_ = room && (roomT_ <= dt + 1e-9 ? cadenceUp : roomCadence_ || cadenceUp);
    if (bound) {
        boundT_ += dt;
        boundSpan_ += target ? measureMs * 1000.0 * frames_ : static_cast<double>(present_);
        boundIntervalSum_ += static_cast<double>(present_);
        boundFrames_ += frames_;
        boundGpuSum_ += gpuMs * frames_;
        boundGpuFrames_ += frames_;
    } else {
        boundT_ = 0;
        boundSpan_ = 0;
        boundIntervalSum_ = 0;
        boundFrames_ = 0;
        boundGpuSum_ = 0;
        boundGpuFrames_ = 0;
    }
    if (fxActive && steps_ && !trial_ && rung_ == FxRung()) {
        // Room for the last step to go off: the GPU's busy time as it would be without it (the measured saving, or the assumed
        // one) and the other stages must fit the margin, and the cadence must hold.
        const int idx = StepIdx(RecoveryStep());
        double ratio = std::max(stepGain_[idx] > 0.0 ? stepGain_[idx] : StepRatio(idx), 1.05);
        // (GXV_PACE60: and at least the step's modelled cost: with the upscaler the edge step is ~3 ms of the display scene, not 12 %)
        if (P60Ready()) {
            const double with = Pace60Predict(rung_, StepsNow()), without = Pace60Predict(rung_, StepsNow() & ~RecoveryStep());
            if (with > 0.1) ratio = std::max(ratio, without / with);
        }
        // (Config::cadenceUp: or the steady 30's room against 33.3 ms, cadenceFits)
        const bool offRoom = (wholeKnown && hintKnown_ && winIntervalMs_ <= cfg_.budgetMs * 1.03 &&
            std::max(otherMs, gpuMs * ratio) <= cfg_.upTarget * cfg_.budgetMs) || cadenceFits(gpuMs * ratio);
        fxRoomT_ = offRoom ? fxRoomT_ + dt : 0.0;
    } else {
        fxRoomT_ = 0.0;
    }
    // (Config::freeFrac: the GPU sets the pace, its busy time freeDownFrac of the picture interval or more)
    // (Config::hold30: not a reason to step down there: under the fixed cap the pace is the cap's, and the GPU timer counts its waits)
    const bool gpuPaces = target && !hold30 && cfg_.freeFrac > 0.0 && gpuMs >= cfg_.freeDownFrac * winIntervalMs_;
    freeOverT_ = gpuPaces ? freeOverT_ + dt : 0.0;
    if (cfg_.aniso && target && !aniso_ && AnisoRungOk(rung_) && !steps_ && !holdEdge_ && !checking_ && pendingUndo_ < 0 && !trial_) {
        // Room for anisotropic filtering at the top rung: the GPU's busy time as it would be with it (the assumed ratio), the other
        // stages, and the cadence, within the margin a step up keeps.
        const bool anisoRoom = wholeKnown && hintKnown_ && winIntervalMs_ <= cfg_.budgetMs * 1.03 &&
            std::max(otherMs, gpuMs * cfg_.anisoRatio) <= cfg_.upTarget * cfg_.budgetMs && gpuMs * cfg_.anisoRatio <= freeMs;
        anisoRoomT_ = anisoRoom ? anisoRoomT_ + dt : 0.0;
    } else {
        anisoRoomT_ = 0.0;
    }

    if (cadTrialFrom_ != kNoRung && !cadTrialMiss_) {
        // (Config::cadenceUp: a climb on the steady 30 on trial: its pictures on the cadence late for the GPU, more than holdMissFrac of
        // the trial's, or a window the GPU limits against 33.3 ms, and it goes back (below, once a change is allowed); a window off the
        // cadence says nothing about it)
        if (CadenceWindow()) {
            cadTrialPics_ += cadenceFrames_;
            cadTrialLate_ += winCadLateGpu_;
            if (gpuBound || cadTrialLate_ > cfg_.holdMissFrac * cadTrialPics_) cadTrialMiss_ = true;
        }
        if (!cadTrialMiss_ && now >= cadTrialUntil_) cadTrialFrom_ = kNoRung; // (passed: kept; later misses step down as any other)
    }

    if (cfg_.calm) {
        // (calm: room long enough is kept for the next quiet moment; a window without it, or an overload, forgets it)
        // (the smallest step any window of the unbroken stretch of room allowed: the camera's swings and the effects of
        // the whole stretch must fit, not only the last window's)
        if (room) minUp_ = roomT_ <= dt + 1e-9 ? up : std::min(minUp_, up);
        if (room && roomT_ >= cfg_.upQuietHoldSec) {
            pendingUp_ = minUp_;
            roomAt_ = now;
        } else if (!room) {
            pendingUp_ = 0;
            roomAt_ = 0;
        }
        if (!bound) pendingBelow_ = false;
    }
    // (target mode: and not below the rung where an earlier visit of this scene found that a step bought nothing)
    const bool floorKnown = target && sceneFloor_ != kNoRung && rung_ <= sceneFloor_ && now < sceneFloorUntil_;
    // (target 60: when even the smallest size, its GPU time falling with the pixels (the most it could), and the other stages as they
    // are would not let the pictures come pathRateFrac faster than now, no step below can show one more: none is taken. Throughput, not
    // whole retraces: with the "Up to 60" skip rule and GXV_PRESENT_SYNC a GPU at 24 ms a picture gives ~41 a second (holds of 1 and 2
    // vblanks), not 30, so a GPU over 16.7 ms at every size can still gain pictures from a smaller one.)
    bool cannotGain = false;
    if (cfg_.targetFps == 60 && rung_ <= kToday && rung_ > cfg_.minRung && boundFrames_) {
        const double gpuNow = boundGpuFrames_ ? boundGpuSum_ / boundGpuFrames_ : gpuMs;
        const double intervalNow = boundIntervalSum_ / 1000.0 / boundFrames_;
        const double floorMs = P60Ready() ? P60Floor() : P23() ? P23Pred(gpuNow, rung_, cfg_.minRung) : gpuNow * Cost(cfg_.minRung) / Cost(rung_);
        const double bestMs = std::max({floorMs, otherMs, cfg_.budgetMs});
        cannotGain = bestMs >= intervalNow * (1.0 - cfg_.pathRateFrac);
    }
    if (target) {
        // (the stage hint for the pacing's steady 30: the GPU, the slowest per-picture stage beside it, and whether anything is left to
        // make the GPU's work smaller; 0 = not known)
        // (Config::cadenceUp: on the steady 30 the sizes are judged against 33.3 ms and may climb back to today's, so the GPU at today's size
        // would keep the release estimate under its 45 a second for good once the CPU's own peak has passed (r99, Honeyhive 1.5x: engaged
        // at 27 game turns a second, then 30 at 720x404 for the rest of the run, where r98 ran ~54 at 608-720). The release is judged with
        // the GPU at the smallest size the controller may still take, its time falling with the pixels: leaving the cadence hands the scene
        // back to the 16.7 ms rules, which take the sizes down again if they must.)
        const bool pipeScaled = cfg_.cadenceUp && CadenceWindow() && rung_ > cfg_.minRung && !floorKnown;
        double pipeGpuMs = !pipeScaled ? gpuMs
                                 : cfg_.cadenceGpuScale >= 1.0 ? gpuMs * Cost(cfg_.minRung) / Cost(rung_)
                                                                : gpuMs * (1.0 - cfg_.cadenceGpuScale * (1.0 - Cost(cfg_.minRung) / Cost(rung_)));
        // (GXV_PACE60: on the steady 30 the release reads the model's verdict: reachable, the GPU at the floor rung with every step on (what
        // the 16.7 ms rules will take after the release); out of reach, a GPU just slower than the release rate, so the cadence is not left
        // for a 60 the controller will not chase (that release would come back within the confirm time and block the next for 30 s+))
        if (P60Ready() && CadenceWindow())
            pipeGpuMs = p60_.reach ? std::min(gpuMs, P60Floor()) : std::max(P60Floor(), 1000.0 / cfg_.cadenceReachFps * 1.02);
        const double pipeMs = std::max({pipeGpuMs, backendMs, frontMs});
        bool lowerable = NextStep(now) != 0 || trial_ != 0 || checking_ || pendingBelow_ ||
            (rung_ > cfg_.minRung && now >= belowBlockedUntil_ && !floorKnown && !cannotGain);
        // (GXV_PACE60: nothing the controller can lower brings 60: the GPU's limit is the steady 30's case)
        if (P60Holds()) lowerable = false;
        g_stageGpuUs.store(wholeKnown ? static_cast<uint32_t>(std::min(gpuMs * 1000.0, 1e6)) : 0u, std::memory_order_relaxed);
        g_stagePipeUs.store(wholeKnown ? static_cast<uint32_t>(std::min(pipeMs * 1000.0, 1e6)) : 0u, std::memory_order_relaxed);
        g_stageLowerable.store(lowerable ? 1u : 0u, std::memory_order_relaxed);
    }
    // (GXV_PACE60: reachable and the GPU the limit off the steady 30 for p60StepSec: the steps and the size the model says, in one change)
    // (and the model's own busy time, without the display scene's wait, over the budget too: without GXV_PACE23 the clock's figure has the wait)
    const bool p60Over = P60Ready() && p60_.reach && target && !CadenceWindow() && rung_ <= kToday && gpuBound && mBusy_ >= cfg_.budgetMs * cfg_.downBelow;
    p60OverT_ = p60Over ? p60OverT_ + dt : 0.0;
    if (QG() && target) {
        // (Config::qg) Not settled: the holds start again from play. Then: the reduction on trial is judged; a restore on trial that lost pictures
        // to the GPU goes back; on a 30 cadence with 60 out of reach, room for qgRestoreSec restores what fits 33.3 ms.
        if (!qgSettled) {
            ResetHolds();
            p60OverT_ = 0;
            qgRoomT_ = 0;
            return false;
        }
        const bool qgRoom = CadenceWindow() && wholeKnown && rung_ <= kToday && std::abs(winIntervalMs_ - cadMs) <= cadMs * 0.03 && winCadLateGpu_ == 0 &&
            !gpuBound && QgOutOfReach(gpuMs, otherMs);
        qgRoomT_ = qgRoom ? qgRoomT_ + dt : 0.0;
        if (qgBackRung_ != kNoRung && !qgBackMiss_) {
            if (CadenceWindow() && (gpuBound || static_cast<double>(winCadLateGpu_) > cfg_.holdMissFrac * cadenceFrames_)) qgBackMiss_ = true;
            else if (now >= qgBackUntil_) qgBackRung_ = kNoRung; // (passed: kept)
        }
        if (const int r = QgJudge(now, dt, mayChange, gpuMs, otherMs)) return r == 1;
        if (mayChange && qgBackMiss_ && qgBackRung_ != kNoRung) return QgBack(now) == 1;
        if (mayChange && qgRoomT_ >= cfg_.qgRestoreSec)
            if (const int r = QgRestore(now, gpuMs, otherMs)) return r == 1;
    }
    if (target && ShedOn()) { // (Config::shed: the judgement of a level just entered, then the way back)
        ShedJudgeAdd(now, gpuMs, dt, frames_);
        if (mayChange && ShedUpStep(now, gpuMs, otherMs, winIntervalMs_, dt)) return true;
    }
    if (!mayChange) return false; // (too soon after the last change: the holds go on counting)
    if (p60Over && p60OverT_ >= cfg_.p60StepSec && !trial_ && !checking_ && pendingUndo_ < 0 && cadTrialFrom_ == kNoRung &&
        (!P23() || P23ChangeAllowed(now, true))) {
        p60OverT_ = 0;
        if (const int done = P60Step(now, gpuMs)) return done == 1;
    }
    if (target && cfg_.calm && pendingUndo_ >= 0 && now >= undoDueAt_ && !SeamHold(now)) {
        // (target mode: a smaller picture that bought nothing is not kept for want of a quiet moment; nothing else moves first)
        UndoBelow(now);
        return true;
    }
    if (cadTrialMiss_ && cadTrialFrom_ != kNoRung && cadTrialFrom_ < rung_) {
        // (Config::cadenceUp: the size tried on the steady 30 lost pictures to the GPU: back to where it came from at once, before anything
        // else, and the rung tried blocked (blockSec, doubling to maxBlockSec), as a climb that overloaded)
        const int back = cadTrialFrom_;
        Block(rung_, now);
        checking_ = false;
        SetRung(back, Why::CadenceBack, now);
        return true;
    }
    if (cfg_.aniso && target && AnisoRungOk(rung_)) {
        if (aniso_) {
            // (Config::qg: not while an earlier "off" that bought nothing is remembered)
            if (((hold30 ? over : overT_ >= cfg_.downHoldSec) || freeOverT_ >= cfg_.downHoldSec) && !(QG() && now < qgAnisoKeepUntil_)) {
                // The GPU is the limit: what means least to a player goes first, before any rung or step.
                anisoBlockLen_ = anisoBlockLen_ > 0 ? std::min(anisoBlockLen_ * 2.0, cfg_.maxBlockSec) : cfg_.blockSec;
                anisoBlockedUntil_ = now + static_cast<uint64_t>(anisoBlockLen_ * 1e6);
                lastBlockAt_ = now;
                SetAniso(false, Why::AnisoOff, now);
                return false;
            }
        } else if (anisoRoomT_ >= cfg_.anisoHoldSec && now >= anisoBlockedUntil_ && !SeamHold(now)) {
            SetAniso(true, Why::AnisoOn, now);
            return false;
        }
    }
    const bool holdBack = holdTrial && holdTrialFrom_ != kNoRung && holdTrialFrom_ < rung_;
    if (HoldEdgeRule() && over && !holdEdge_ && !holdBack && !checking_ && pendingUndo_ < 0 && !trial_) {
        // (Config::hold30: a GPU miss takes the present's edge smoothing off first, at this size, before any step or smaller size; a miss
        // within its trial (back on) takes it off again and blocks the next try, doubling. Its saving is measured by the next full window.)
        const bool failed = now < edgeTrialUntil_;
        const double before = missGpuN_ ? missGpuSum_ / missGpuN_ : gpuMs;
        if (failed) {
            edgeBlockLen_ = edgeBlockLen_ > 0 ? std::min(edgeBlockLen_ * 2.0, cfg_.maxBlockSec) : cfg_.blockSec;
            edgeBlockedUntil_ = now + static_cast<uint64_t>(edgeBlockLen_ * 1e6);
            lastBlockAt_ = now;
        }
        edgeTrialUntil_ = 0;
        SetHoldEdge(true, failed ? Why::HoldEdgeBack : Why::HoldEdgeOff, now);
        edgeBefore_ = before;
        edgeMeasuring_ = true;
        return false;
    }
    if (fxActive && rung_ == FxRung() && !checking_ && pendingUndo_ < 0 && !trial_ && !aniso_) {
        const unsigned next = NextStep(now);
        if (next && boundT_ >= cfg_.fxHoldSec) {
            // GPU-limited at today's rung: the thing that means least to a player first.
            fxBefore_ = boundGpuFrames_ ? boundGpuSum_ / boundGpuFrames_ : gpuMs;
            fxBoundKept_ = boundT_;
            fxOffBlockLen_ = lastFxOffAt_ != 0 && now - lastFxOffAt_ < static_cast<uint64_t>(cfg_.fxRepeatSec * 1e6) && fxOffBlockLen_ > 0
                ? std::min(fxOffBlockLen_ * 2.0, cfg_.maxBlockSec) : cfg_.fxOffBlockSec;
            fxOffBlockedUntil_ = now + static_cast<uint64_t>(fxOffBlockLen_ * 1e6);
            SetSteps(steps_ | next, Why::FxOn, now);
            trial_ = next;
            fxCheckT_ = fxCheckSum_ = 0;
            fxCheckFrames_ = 0;
            return false;
        }
        // (Config::hold30: calm hold windows take the last step off too, as a climb trial would be: if pictures go late for the GPU again
        // the step comes back on, and the next switching off is blocked, doubling: fxOffBlockSec)
        if (steps_ && (fxRoomT_ >= cfg_.fxOffHoldSec || (hold30 && calmRun_ >= cfg_.holdClimbWindows)) && now >= fxOffBlockedUntil_ && !SeamHold(now)) {
            lastFxOffAt_ = now;
            SetSteps(steps_ & ~RecoveryStep(), Why::FxOff, now); // (half-size effects first, then the cheaper background)
            return false;
        }
    }
    const int floorUp = std::max(kToday, cfg_.minRung); // the lowest rung an overload above today goes to
    const bool qgSizeHeld = QG() && now < belowBlockedUntil_; // (Config::qg: a size reduction that bought nothing: none above today either)
    if (rung_ > floorUp && overT_ < cfg_.downHoldSec && freeOverT_ >= cfg_.freeDownSec && !qgSizeHeld && !ShedOwnsRung() && !SeamHold(now)) {
        // (Config::freeFrac: above today's size the GPU now sets the picture rate: one rung down, that rung blocked as after an overload)
        Block(rung_, now);
        checking_ = false;
        SetRung(rung_ - 1, Why::FreeDown, now);
        return true;
    }
    if (hold30 && over && (rung_ > floorUp || holdBack)) {
        // (Config::hold30: one rung at a time, the rung left blocked as after an overload; a climb that failed its trial goes back where it
        // came from)
        const int to = holdBack ? holdTrialFrom_ : rung_ - 1;
        Block(rung_, now);
        checking_ = false;
        SetRung(to, holdBack ? Why::HoldBack : Why::HoldMiss, now);
        return true;
    }
    if (rung_ > floorUp && overT_ >= cfg_.downHoldSec && !qgSizeHeld && !ShedOwnsRung()) {
        int to = rung_ - 1;
        const bool saturated = target ? gpuMs >= downMs : load >= cfg_.saturated;
        if (saturated && !cfg_.calm) to = floorUp; // (calm: as far as the load says, not straight to today)
        else
            while (to > floorUp && (target ? gpuMs / downMs : load) * Cost(to) / Cost(rung_) > cfg_.upTarget) --to;
        Block(rung_, now);
        checking_ = false;
        SetRung(to, saturated ? Why::Saturated : Why::Overload, now);
        return true;
    }
    // (quality steps precede the emergency rung: while one is still to come, or being judged, the rung stays)
    const bool fxFirst = fxActive && rung_ == FxRung() && (trial_ || NextStep(now));
    if (!checking_ && !fxFirst && !floorKnown && !cannotGain && rung_ <= kToday && rung_ > cfg_.minRung && boundT_ >= cfg_.belowHoldSec && !(ShedOn() && !cfg_.shedBelow) &&
        now >= belowBlockedUntil_ && pendingUndo_ < 0) {
        const int from = rung_;
        // (target mode: the GPU's mean busy time over the GPU-limited stretch, what the step is judged on)
        const double before = target ? (boundGpuFrames_ ? boundGpuSum_ / boundGpuFrames_ : gpuMs)
                                     : (boundFrames_ ? boundSpan_ / 1000.0 / boundFrames_ : winIntervalMs_);
        // (target mode: the floor is being missed: the pictures of the stretch came slower than the target, or the GPU alone
        // needs more than the budget)
        const double beforeInterval = boundFrames_ ? boundIntervalSum_ / 1000.0 / boundFrames_ : winIntervalMs_;
        const bool missing = target && (before > cfg_.budgetMs || beforeInterval > cfg_.budgetMs * 1.03);
        // (calm, GXV_AUTOQ_SEVERE, default on: a GPU-bound stretch that needs 1.8 times the frame budget or more for 3 s takes its
        // step below today in play: waiting for a quiet moment held a drop-in at 11-12 FPS for 15 s on the console, 4 Nov)
        static const bool severeStep = [] { const char* e = getenv("GXV_AUTOQ_SEVERE"); return !e || e[0] != '0'; }();
        const bool severe = severeStep && before >= 1.8 * cfg_.budgetMs && boundT_ >= 3.0;
        if (cfg_.calm && !severe && (!target || boundT_ < 8.0)) {
            // (calm: a step below today only buys a little speed, so it waits for a quiet moment as a step up does)
            pendingBelow_ = true;
            belowAt_ = now;
            belowBefore_ = before;
            belowMissing_ = missing;
            belowInterval_ = beforeInterval;
            return false;
        }
        // Target mode prefers a quiet downstep too, but a persistent GPU miss
        // for 8 s may lower resolution in play rather than skip indefinitely.
        if (pathFrom_ == kNoRung) { pathFrom_ = from; pathInterval_ = beforeInterval; }
        SetRung(rung_ - 1, Why::GpuBound, now);
        checking_ = true;
        checkFrom_ = from;
        checkBefore_ = before;
        checkMissing_ = missing;
        checkSpan_ = 0;
        checkT_ = 0;
        checkFrames_ = 0;
        return true;
    }
    if (hold30 && rung_ < cfg_.startRung && rung_ + 1 <= upMax && calmRun_ >= cfg_.holdClimbWindows && !Blocked(rung_ + 1, now) &&
        !checking_ && pendingUndo_ < 0 && !trial_ && !pendingBelow_ && !SeamHold(now)) {
        // (Config::hold30: below the start rung, every picture of holdClimbWindows hold windows came on time for the GPU with its busy time
        // under the budget: one rung up, on trial (SetRung); a miss within the trial comes back here blocked, so a failed climb is retried
        // only when that block ends: blockSec, doubling to maxBlockSec)
        checking_ = false;
        SetRung(rung_ + 1, Why::HoldClimb, now);
        return true;
    }
    if (HoldEdgeRule() && holdEdge_ && calmRun_ >= cfg_.holdClimbWindows && now >= edgeBlockedUntil_ && !checking_ && pendingUndo_ < 0 &&
        !trial_ && (rung_ >= cfg_.startRung || rung_ + 1 > upMax ||
                    (Blocked(rung_ + 1, now) && blockLen_[rung_ + 2] >= cfg_.maxBlockSec / 2.0))) {
        // (Config::hold30: edge smoothing on again, after the size (the reverse of the way down): at the start rung, at the highest rung
        // allowed, or below when climbing has failed again and again (the rung above blocked for maxBlockSec / 2 or longer, the third
        // failure), when the GPU now plus its measured saving (the assumed edgeRatio until measured) fits holdClimbGpuFrac of the budget;
        // a trial for holdTrialSec)
        const double gpuNow = lastHold_.gpuMs;
        const double save = edgeSave_ >= 0.0 ? edgeSave_ : gpuNow * (cfg_.edgeRatio - 1.0);
        if (gpuNow + save <= cfg_.holdClimbGpuFrac * cfg_.budgetMs && !SeamHold(now)) {
            SetHoldEdge(false, Why::HoldEdgeOn, now);
            edgeTrialUntil_ = now + static_cast<uint64_t>(cfg_.holdTrialSec * 1e6);
            return false;
        }
    }
    if (cfg_.recover && cfg_.calm) {
        // (GXV_AUTOQ_RECOVER: room for recoverHoldSec in a row, and not past what every window of it allowed)
        if (roomT_ >= cfg_.recoverHoldSec) {
            checking_ = false;
            return Climb(rung_ + std::max(1, std::min(up, minUp_)), now);
        }
        return false;
    }
    if (roomT_ >= (cfg_.calm ? cfg_.upLongHoldSec : cfg_.upHoldSec)) {
        checking_ = false;
        return Climb(rung_ + up, now);
    }
    return false;
}

// ------------------------------------------------------------------ GPU clock
namespace clock {
#ifdef __VGX_VITA__
namespace {
// One presented frame on its way through the GPU. The back end's thread writes an entry and publishes it with `head`;
// the watcher time-stamps `done` when the GPU's notification has passed the frame's fence; the back end takes finished
// entries in order (`tail`). Single producer, single consumer, plus the watcher between them.
struct Entry {
    uint64_t start = 0, present = 0, run = 0;
    uint32_t fence = 0;
    std::atomic<uint64_t> done{0};
    bool timers = false, known = false; // GPU timers clock: the busy time of the frame the GPU finished last (if a new one)
    uint32_t busy = 0;
    uint32_t gameCpu = 0, gameTurns = 0, gameFaults = 0;
    bool gameKnown = false;
    uint32_t frontCpu = 0, frontPictures = 0;
    bool frontKnown = false;
    bool cadence30 = false;
    bool paced = false;
    bool honest = false, honestKnown = false; // GXV_PACE23: the scene timers' busy time stands in for `busy`
    uint32_t honestBusy = 0, honestRaw = 0, honestFrames = 1;
};
constexpr unsigned kRing = 64;
Entry g_ring[kRing];
std::atomic<unsigned> g_head{0};
unsigned g_tail = 0;      // back end only
unsigned g_watch = 0;     // watcher only
uint64_t g_start = 0;     // the frame being executed
SceUID g_sema = -1;
volatile unsigned int* g_addr = nullptr;
long g_pollUs = 500;
bool g_timers = false;    // the GPU timers clock (vitaGL GPU_TIMERS) instead of the frame fence
bool g_timerRingGuard = false, g_timerRingDropReported = false;
uint32_t g_rtHead = 0;    // frames the GPU timers had finished at the last present
uint32_t g_honestSeen = 0; // GXV_PACE23: HonestBusyPublish's count at the last present

bool Reached(uint32_t fence) { return static_cast<int32_t>(*g_addr - fence) >= 0; } // (wrapping: it counts every scene since start-up)

int WatcherMain(SceSize, void*) {
    uint64_t lastTail = 0; // the previous frame: from its present to its last pixel
    for (;;) {
        if (g_watch == g_head.load(std::memory_order_acquire)) {
            SceUInt timeout = 100000;
            sceKernelWaitSema(g_sema, 1, &timeout);
            continue;
        }
        Entry& e = g_ring[g_watch % kRing];
        const uint32_t fence = e.fence;
        // First a sleep through three quarters of what the previous frame took after its present (a GPU-limited frame
        // is on the GPU for most of a frame after its present: dozens of polls otherwise), then a look every g_pollUs.
        // A frame not done after 2 s (the GPU is never that late with a frame; a vitaGL that stopped ending scenes with
        // this fence would be) is stamped as done anyway: the controller sees a 2 s gap, which restarts its windows,
        // and the watcher does not poll for ever.
        uint64_t now = sceKernelGetProcessTimeWide();
        const uint64_t wake = e.present + lastTail * 3 / 4;
        if (!Reached(fence) && wake > now + 1000u) sceKernelDelayThread(static_cast<SceUInt>(wake - now));
        const uint64_t since = sceKernelGetProcessTimeWide();
        while (!Reached(fence) && sceKernelGetProcessTimeWide() - since < 2000000u) sceKernelDelayThread(static_cast<SceUInt>(g_pollUs));
        now = sceKernelGetProcessTimeWide();
        e.done.store(now, std::memory_order_release);
        lastTail = now > e.present ? std::min<uint64_t>(now - e.present, 100000u) : 0;
        ++g_watch;
    }
    return 0;
}
} // namespace

uint64_t NowUs() { return sceKernelGetProcessTimeWide(); }

// GXV_AUTOQ_POLL_US=<n> (default 500): how often the watcher looks at the GPU's notification while a frame is on the GPU
// (it sleeps on a semaphore otherwise). The completion time is known to this resolution.
// The watcher runs on the back end's core (1) at priority 0x60, above the game's threads (0x90/0x9F) and below the clocks
// (0x50): it wakes for a few microseconds per poll.
// GXV_AUTOQ_TIMERS=0: the frame fence clock even with a vitaGL that has GPU timers (default 1: the timers when there are).
// The timers' watcher (vitaGL's thread "vgl_rt_gpu") also runs on core 1 at priority 0x60; it blocks in
// sceGxmNotificationWait once per scene, no polling.
bool Start() {
    const char* te = std::getenv("GXV_AUTOQ_TIMERS");
    // (vglRtLastGpuFrame copies the library's whole struct to the caller's: a library whose struct is larger than this file's mirror would write
    // past it. A library without vglRtGpuFrameSize is the one this file was written for (vp30: five fields, 16 bytes).)
    const bool frameFits = !vglRtGpuFrameSize || vglRtGpuFrameSize() == sizeof(VgxRtGpuFrame);
    if (!frameFits)
        Log("[autoq] GPU timers not used: the library's vglRtGpuFrame is %u bytes, this build's copy %u (the fence clock stays)", static_cast<unsigned>(vglRtGpuFrameSize()),
            static_cast<unsigned>(sizeof(VgxRtGpuFrame)));
    if (vglRtSetup && vglRtLastGpuFrame && frameFits && !(te && *te == '0')) {
        // GXV_GPU_TIMER_GUARD=1 prevents timer-ring overwrite and rejects timing after overflow; default off for A/B.
        static const bool guard = EnvInt("GXV_GPU_TIMER_GUARD", 0) != 0;
        g_timerRingGuard = guard && vglRtSetRingGuard && vglRtRingDrops;
        if (g_timerRingGuard) vglRtSetRingGuard(1);
        if (guard) Log("[autoq] GPU timer ring guard: %s", g_timerRingGuard ? "on" : "unavailable in this driver");
        vglRtSetup(kVglRtGpuTimers, 0x60, 0x20000);
        g_timers = true;
        Log("[autoq] GPU clock: vitaGL's GPU timers (a notification per scene, watcher on core 1)");
        return true;
    }
    if (&query_fence == nullptr || query_fence.address == nullptr) {
        Log("[autoq] no GPU clock: this vitaGL has no query_fence notification");
        return false;
    }
    g_addr = query_fence.address;
    if (const char* e = std::getenv("GXV_AUTOQ_POLL_US"); e && *e) g_pollUs = std::clamp(std::atol(e), 100L, 4000L);
    g_sema = sceKernelCreateSema("vgx_gpuclock", 0, 0, 1 << 20, nullptr);
    if (g_sema < 0) return false;
    const SceUID t = sceKernelCreateThread("vgx-gpuclock", &WatcherMain, 0x60, 16 * 1024, 0, 0x20000, nullptr);
    if (t < 0 || sceKernelStartThread(t, 0, nullptr) < 0) {
        Log("[autoq] no GPU clock: the watcher thread could not start (0x%X)", static_cast<unsigned>(t));
        return false;
    }
    Log("[autoq] GPU clock: vitaGL's scene notification, polled every %ld us on core 1", g_pollUs);
    return true;
}

void FrameBegin() { g_start = NowUs(); }

void FramePresented() {
    const unsigned h = g_head.load(std::memory_order_relaxed);
    if (h - g_tail >= kRing - 1) return; // (nobody is taking them: drop rather than overwrite)
    Entry& e = g_ring[h % kRing];
    e.start = g_start;
    e.present = NowUs();
    e.run = port_thread_cpu_us ? static_cast<uint64_t>(port_thread_cpu_us()) : 0;
    e.gameKnown = port_autoq_game_cpu_totals(&e.gameCpu, &e.gameTurns, &e.gameFaults) != 0;
    e.frontKnown = port_autoq_front_totals(&e.frontCpu, &e.frontPictures) != 0;
    e.cadence30 = port_pace_steady30_engaged && port_pace_steady30_engaged() != 0;
    e.paced = port_vgx_pace_divisor && port_vgx_pace_divisor() != 0;
    e.timers = g_timers;
    e.known = false;
    e.busy = 0;
    e.honest = e.honestKnown = false;
    e.honestFrames = 1;
    if (g_timers && HonestBusyEnabled()) {
        e.honest = true;
        const uint32_t hs = g_honestSeq.load(std::memory_order_acquire);
        if (hs != g_honestSeen) {
            e.honestKnown = true;
            e.honestBusy = g_honestBusy.load(std::memory_order_relaxed);
            e.honestRaw = g_honestRaw.load(std::memory_order_relaxed);
            e.honestFrames = std::min<uint32_t>(hs - g_honestSeen, 4u);
            g_honestSeen = hs;
        }
    }
    if (g_timers) {
        // The frame the GPU finished last (one or two presents back): its busy time goes with this present. A present
        // after which no new frame was finished carries none (the controller averages busy over the frames that have one).
        VgxRtGpuFrame g{};
        const uint32_t n = vglRtLastGpuFrame(&g);
        const bool dropped = g_timerRingGuard && vglRtRingDrops() != 0;
        if (dropped && !g_timerRingDropReported) {
            g_timerRingDropReported = true;
            Log("[autoq] GPU timer ring overflow: GPU fence protected; incomplete GPU timings ignored until restart");
        }
        if (n != 0 && n != g_rtHead) {
            g_rtHead = n;
            // Target30 must not mistake dropped scene timings for GPU headroom.
            // Consume incomplete publications once, but leave their cost unknown.
            if (!dropped && (!TargetEnabled() || (g.scenes != 0 && g.timed == g.scenes))) {
                e.known = true;
                e.busy = g.busy_us;
            }
        }
        e.done.store(e.present, std::memory_order_relaxed);
        g_head.store(h + 1, std::memory_order_release);
        return;
    }
    e.fence = query_fence.value; // (the display's scene was ended with this value inside the swap)
    e.done.store(0, std::memory_order_relaxed);
    g_head.store(h + 1, std::memory_order_release);
    sceKernelSignalSema(g_sema, 1);
}

void Counters(char* out, unsigned size) {
    out[0] = 0;
    if (g_timers && vglRtGpuCounters) {
        uint32_t timed = 0, untimed = 0, late = 0, pending = 0;
        vglRtGpuCounters(&timed, &untimed, &late, &pending);
        std::snprintf(out, size, "; GPU timers: %u scenes timed, %u untimed, %u already done when looked at, %u pending", static_cast<unsigned>(timed),
                      static_cast<unsigned>(untimed), static_cast<unsigned>(late), static_cast<unsigned>(pending));
    }
}

bool NextDone(FrameTimes& out) {
    if (g_tail == g_head.load(std::memory_order_acquire)) return false;
    Entry& e = g_ring[g_tail % kRing];
    const uint64_t done = e.done.load(std::memory_order_acquire);
    if (done == 0) return false;
    out.startUs = e.start;
    out.presentUs = e.present;
    out.gpuDoneUs = e.timers ? 0 : done;
    out.backendRunUs = e.run;
    out.gameRunUs = e.gameCpu;
    out.gameTurns = e.gameTurns;
    out.gameFaults = e.gameFaults;
    out.gameKnown = e.gameKnown;
    out.frontRunUs = e.frontCpu;
    out.frontPictures = e.frontPictures;
    out.frontKnown = e.frontKnown;
    out.cadence30 = e.cadence30;
    out.paced = e.paced;
    out.timers = e.timers;
    out.busyKnown = e.known;
    out.gpuBusyUs = e.busy;
    out.gpuRawUs = 0;
    out.gpuFrames = 1;
    if (e.honest) { // (GXV_PACE23: the corrected busy time replaces the timers' own; a picture it has none for carries none)
        out.busyKnown = e.honestKnown;
        out.gpuBusyUs = e.honestKnown ? e.honestBusy : 0;
        out.gpuRawUs = e.honestKnown ? e.honestRaw : 0;
        out.gpuFrames = e.honestFrames;
    }
    ++g_tail;
    return true;
}
#else
// No GPU clock off the console (the desktop harness's GL has no cheap equivalent wired up): the controller never moves.
uint64_t NowUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool Start() { return false; }
void FrameBegin() {}
void FramePresented() {}
bool NextDone(FrameTimes&) { return false; }
void Counters(char* out, unsigned) { out[0] = 0; }
#endif
} // namespace clock

} // namespace vgx::autoq
