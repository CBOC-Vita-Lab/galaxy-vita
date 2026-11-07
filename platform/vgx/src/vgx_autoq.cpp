// Automatic picture quality: the controller and the GPU clock (see vgx_autoq.h for the rules).
#include "vgx_autoq.h"
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
void port_log(const char* fmt, ...) __attribute__((weak));
void port_log_summary(const char* fmt, ...) __attribute__((weak)); // (platform/src/os/os_alarm.cpp: the basic log)
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
static_assert(std::atomic<uint32_t>::is_always_lock_free);
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

void Log(const char* fmt, ...) {
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

void LogSummary(const char* fmt, ...) {
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
    if (cfg_.targetFps != 60 || !cfg_.edgeStep) cfg_.edgeCheck = false; // (Performance's edge step only; Hold30's edge rule is its own)
    cfg_.cadenceUpFrac = std::clamp(cfg_.cadenceUpFrac, 0.5, 1.0);
    cfg_.cadenceReachFps = std::clamp(cfg_.cadenceReachFps, 31.0, 60.0);
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
}

void Controller::Block(int rung, uint64_t now) {
    double& len = blockLen_[rung + 1];
    len = len > 0 ? std::min(len * 2.0, cfg_.maxBlockSec) : cfg_.blockSec;
    blockUntil_[rung + 1] = now + static_cast<uint64_t>(len * 1e6);
    lastBlockAt_ = now;
}

void Controller::SetRung(int to, Why why, uint64_t now) {
    to = std::clamp(to, cfg_.minRung, cfg_.maxRung);
    if (to == rung_) return;
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
    // (Config::hold30: edge smoothing has its own rule at any rung, HoldEdgeRule, and is not one of the steps at today's rung)
    if (cfg_.edgeStep && !HoldEdgeRule() && !(steps_ & 4) && now >= onBlockedUntil_[2]) return 4;
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
void Controller::Climb(int to, uint64_t now) {
    const bool cadence = cfg_.cadenceUp && roomCadence_;
    if (cadence) to = std::min(to, std::max(rung_, kToday));
    const int from = rung_;
    SetRung(to, cadence ? Why::CadenceUp : Why::Headroom, now);
    if (cadence && rung_ > from) {
        cadTrialFrom_ = from;
        cadTrialUntil_ = now + static_cast<uint64_t>(cfg_.holdTrialSec * 1e6);
    }
}

void Controller::SetSteps(unsigned to, Why why, uint64_t now) {
    if (steps_ == to) return;
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
    onBlockedUntil_[0] = onBlockedUntil_[1] = onBlockedUntil_[2] = fxOffBlockedUntil_ = 0;
    onBlockLen_[0] = onBlockLen_[1] = onBlockLen_[2] = fxOffBlockLen_ = 0;
    anisoBlockedUntil_ = 0;
    anisoBlockLen_ = 0;
    // (Config::hold30: a new scene starts with the present's edge smoothing on, unmeasured, unblocked)
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
    if (aniso_) {
        // (no frozen picture of its own: it goes with the rest; switching it on again is blocked as after an overload)
        anisoBlockLen_ = anisoBlockLen_ > 0 ? std::min(anisoBlockLen_ * 2.0, cfg_.maxBlockSec) : cfg_.blockSec;
        anisoBlockedUntil_ = now + static_cast<uint64_t>(anisoBlockLen_ * 1e6);
        lastAniso_ = Change{rung_, rung_, Why::AnisoOff, meanGpuMs / cfg_.budgetMs, winCpu_, winIntervalMs_, now};
        aniso_ = false;
        ++anisoChanges_;
        anisoAt_ = now;
        meanGpuMs /= cfg_.anisoRatio;
    }
    const int from = rung_;
    // the rung the quality steps belong to (today's): a rung above it goes there first
    int to = std::min(rung_, FxRung());
    double predicted = meanGpuMs * Cost(to) / Cost(rung_);
    unsigned steps = to <= FxRung() ? steps_ : 0u;
    if (predicted > fit && cfg_.edgeStep && !HoldEdgeRule() && !(steps & 4u)) {
        steps |= 4u;
        predicted /= stepGain_[2] > 0.0 ? stepGain_[2] : cfg_.edgeRatio;
    }
    if (predicted > fit && cfg_.bgStep && !(steps & 2u)) {
        steps |= 2u;
        predicted /= stepGain_[1] > 0.0 ? stepGain_[1] : cfg_.bgRatio;
    }
    if (predicted > fit && (cfg_.fxStep || cfg_.overloadFx) && !(steps & 1u)) {
        steps |= 1u;
        predicted /= stepGain_[0] > 0.0 ? stepGain_[0] : cfg_.fxRatio;
    }
    const double perPixel = predicted;
    // (below today's rung only when steps below are not blocked: a step there that bought nothing was undone, and blocks the next)
    const int lowest = now < belowBlockedUntil_ ? std::min(rung_, FxRung()) : cfg_.minRung;
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

// One presented picture of the floor census (target mode). Quiet moments and stalls (loads, shader builds) are counted apart;
// a late picture is blamed on the largest stage that reached causeShare of the budget: the GPU's busy time, two game turns'
// CPU, the renderer's CPU, the front end's CPU (when it is measured). None that did, with the first three measured: something
// unmeasured (a wait, the pacing). A picture on Performance mode's steady 30 (cadence) is late only against 33.3 ms.
int Controller::FloorCount(uint64_t interval, bool quiet, bool measured, bool gpuKnown, bool gameKnown, bool backendKnown,
                           uint64_t gpu, uint64_t game, uint64_t backend, bool frontKnown, uint64_t front, bool cadence) {
    FloorWindow& w = floor_;
    if (quiet) { ++w.quiet; return -1; }
    if (static_cast<double>(interval) > cfg_.gapResetMs * 1000.0) { ++w.stalls; w.stallUs += interval; return -1; }
    ++w.pictures;
    w.spanUs += interval;
    const bool paced30 = cadence && cfg_.targetFps == 60;
    if (paced30) ++w.cadence;
    const double budgetMs = paced30 ? 1000.0 / 30.0 : cfg_.budgetMs;
    if (static_cast<double>(interval) <= budgetMs * 1030.0) return -1;
    ++w.late;
    w.lateUs += interval;
    w.worstUs = std::max(w.worstUs, interval);
    const double threshold = cfg_.causeShare * budgetMs * 1000.0;
    int cause = -1;
    double best = 0;
    auto consider = [&](bool known, uint64_t v, int id) {
        if (known && static_cast<double>(v) >= threshold && static_cast<double>(v) > best) { best = static_cast<double>(v); cause = id; }
    };
    consider(gpuKnown, gpu, 0);
    consider(gameKnown, game, 1);
    consider(backendKnown, backend, 2);
    consider(frontKnown, front, 3);
    if (cause == 0) ++w.gpu;
    else if (cause == 1) ++w.game;
    else if (cause == 2) ++w.renderer;
    else if (cause == 3) ++w.front;
    else if (measured) { ++w.other; return 4; }
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

bool Controller::Add(const FrameTimes& f) {
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
    if (cfg_.recover && cfg_.calm && lastBlockAt_ != 0 && f.presentUs >= lastBlockAt_ + static_cast<uint64_t>(cfg_.blockForgetSec * 1e6)) {
        // (GXV_AUTOQ_RECOVER: the blocks that ran out long ago do not count towards the next one's length)
        for (double& len : blockLen_) len = 0;
        belowBlockLen_ = 0;
        onBlockLen_[0] = onBlockLen_[1] = onBlockLen_[2] = fxOffBlockLen_ = anisoBlockLen_ = 0;
        edgeBlockLen_ = 0;
        lastBlockAt_ = 0;
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
    if (Target()) {
        bool currentRoom = false;
        const bool backendKnown = f.backendRunUs > 0 && prevRun > 0 && f.backendRunUs >= prevRun &&
            f.backendRunUs <= INT64_MAX && prevRun <= INT64_MAX;
        const bool gpuKnown = f.timers ? f.busyKnown : !f.paced && f.gpuDoneUs != 0 && prevDone != 0;
        {
            // (the floor census: the same three stages, whichever of them is known)
            const uint64_t doneSpan = f.gpuDoneUs > prevDone ? f.gpuDoneUs - prevDone : 0;
            const uint64_t from = std::max(prevDone, f.startUs);
            const uint64_t fenceBusy = f.gpuDoneUs > from ? std::min(f.gpuDoneUs - from, doneSpan) : 0;
            const bool gameOk = gameKnown && gameTurns != 0;
            const uint64_t gameUs = gameOk ? uint64_t(gameRun) * PictureTurns() / gameTurns : 0, backendUs = backendKnown ? f.backendRunUs - prevRun : 0;
            const int cause = FloorCount(interval, f.quiet, gameOk && backendKnown && gpuKnown, gpuKnown, gameOk, backendKnown,
                                         f.timers ? f.gpuBusyUs : fenceBusy, gameUs, backendUs, lastFrontUs_ != 0, lastFrontUs_, f.cadence30);
            // (Config::hold30: a picture late against the budget + 3 % is late for the GPU only when the census blames the GPU AND every
            // other measured stage fitted the budget + 3 %: then a smaller picture could have brought it on time. Late for the game's CPU
            // (or with the game over the budget as well: the GPU timer counts waits under the fixed cap) it says nothing about the size.)
            picLate_ = !f.quiet && static_cast<double>(interval) > cfg_.budgetMs * 1030.0;
            const double fits = cfg_.budgetMs * 1030.0;
            // (r84 console, Beach Bowl at zoom 2.0: with the GPU ~40 ms a picture and far behind, its timers stopped reporting, the census
            // said "not measured" for every late picture and nothing was ever blamed on the GPU: 25 fps at 960x544 for 25 s. A late
            // picture whose game and renderer CPU are known and fitted, with the GPU's own time unknown, is late for the GPU: nothing else
            // is left that could have made it late.)
            const bool gpuByElimination = !gpuKnown && gameOk && backendKnown;
            picLateGpu_ = picLate_ && (cause == 0 || gpuByElimination) && (!gameOk || static_cast<double>(gameUs) <= fits) &&
                (!backendKnown || static_cast<double>(backendUs) <= fits) && (lastFrontUs_ == 0 || static_cast<double>(lastFrontUs_) <= fits);
            // (Config::cadenceUp: a picture on the steady 30 that the census, judging it against 33.3 ms, blames on the GPU, or with the
            // GPU's own time unknown and nothing else measured that made it late: the size may have cost it)
            picCadLateGpu_ = cfg_.cadenceUp && f.cadence30 && (cause == 0 || (cause == 5 && gpuByElimination));
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
    if (static_cast<double>(interval) > cfg_.gapResetMs * 1000.0) {
        if (Target()) g_pictureWorkUs.store(0, std::memory_order_relaxed);
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
            Remember(from, Target() && pendingUndo_ >= 0 ? pendingUndo_ : rung_, Mem(), sceneFloor_);
            scene_ = to;
            if (to != 0 && to == from) {
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
            RecallFloor(to, f.presentUs);
            for (int i = 0; i <= kRungs; ++i) { blockUntil_[i] = 0; blockLen_[i] = 0; }
            belowBlockedUntil_ = 0;
            belowBlockLen_ = 0;
            ForgetFxBlocks();
            unsigned wantFx = 0;
            int want = Recall(to, &wantFx);
            if (want == kNoRung) want = cfg_.startRung;
            want = WipeCap(std::clamp(want, cfg_.minRung, cfg_.maxRung)); // (Config::wipe: a new scene starts at today's rung at most)
            const bool wantAniso = cfg_.aniso && (wantFx & kAnisoMem) != 0 && want == cfg_.maxRung;
            wantFx &= StepMask();
            if (want > FxRung()) wantFx = 0;
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
            for (int i = 0; i <= kRungs; ++i) { blockUntil_[i] = 0; blockLen_[i] = 0; }
            belowBlockedUntil_ = 0;
            belowBlockLen_ = 0;
            ForgetFxBlocks();
            SetSceneSteps(0, f.presentUs);
            SetSceneAniso(false, f.presentUs);
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
        Remember(scene_, last_.why == Why::NewScene && sinceChange_ <= 60 ? last_.from : rung_,
                 last_.why == Why::NewScene && sinceChange_ <= 60 ? steps_ : Mem(), sceneFloor_);
        scene_ = f.scene;
        RecallFloor(f.scene, f.presentUs);
        unsigned wantFx = 0;
        int want = Recall(f.scene, &wantFx);
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
        if (pendingBelow_ && mayChange && f.presentUs >= belowAt_ && static_cast<double>(f.presentUs - belowAt_) <= cfg_.roomMemorySec * 1e6 &&
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
            Climb(to, f.presentUs);
            return true;
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
    // (Config::overload: the GPU's own busy time a picture, known exactly with its timers)
    const bool gpuFrame = cfg_.overload && Target() && f.timers && f.busyKnown;
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
    if (frames_ < static_cast<unsigned>(cfg_.windowFrames)) return false;
    const bool changed = Evaluate(f.presentUs, mayChange);
    ResetWindow();
    return changed;
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
    winFrontMs_ = frontSeen;
    winGameMs_ = gameMs;
    winBackendMs_ = backendMs;
    winWorkMs_ = std::max(gpuMs, otherMs);
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
            if (!edgeTrial && after > fxBefore_ * (1.0 - need) && after < fxBefore_ * 1.15) {
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
            if (after > checkBefore_ * (1.0 - need) && after < checkBefore_ * 1.15) {
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
    const bool gpuBound = target && wholeKnown && gpuMs >= downMs * cfg_.downBelow &&
        (gpuMs >= otherMs * 1.05 || (cfg_.overload && gpuMs >= downMs * 1.1 && gpuMs >= std::max(backendMs, frontMs) * 1.05)) &&
        (winIntervalMs_ > downMs * 1.03 || winWorkMs_ > downMs);
    // (Config::hold30, Quality under the fixed cap: what the player sees decides, not the GPU's busy share: holdMissWindows hold windows in
    // a row with pictures late for the GPU, one within a climb's trial. It stands for the GPU-limited test of every step down below.)
    const bool hold30 = Hold30();
    const bool holdTrial = hold30 && HoldTrial(now);
    const bool edgeTrial = HoldEdgeRule() && holdEdge_ == false && now < edgeTrialUntil_; // (edge smoothing on again, on trial)
    const bool holdMiss = hold30 && missRun_ >= (holdTrial || edgeTrial ? 1 : cfg_.holdMissWindows);
    const bool over = hold30 ? holdMiss : target ? gpuBound : load >= cfg_.downAbove;
    const bool bound = hold30 ? holdMiss : target ? gpuBound : load >= cfg_.downBelow && cpu >= 0.0 && cpu <= cfg_.cpuMax;
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
        (gpuMs * Cost(cfg_.minRung) / Cost(rung_) >= reachMs || otherMs >= reachMs);
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
        const double ratio = std::max(stepGain_[idx] > 0.0 ? stepGain_[idx] : StepRatio(idx), 1.05);
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
        const double bestMs = std::max({gpuNow * Cost(cfg_.minRung) / Cost(rung_), otherMs, cfg_.budgetMs});
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
        const double pipeGpuMs = cfg_.cadenceUp && CadenceWindow() && rung_ > cfg_.minRung && !floorKnown
                                     ? gpuMs * Cost(cfg_.minRung) / Cost(rung_) : gpuMs;
        const double pipeMs = std::max({pipeGpuMs, backendMs, frontMs});
        const bool lowerable = NextStep(now) != 0 || trial_ != 0 || checking_ || pendingBelow_ ||
            (rung_ > cfg_.minRung && now >= belowBlockedUntil_ && !floorKnown && !cannotGain);
        g_stageGpuUs.store(wholeKnown ? static_cast<uint32_t>(std::min(gpuMs * 1000.0, 1e6)) : 0u, std::memory_order_relaxed);
        g_stagePipeUs.store(wholeKnown ? static_cast<uint32_t>(std::min(pipeMs * 1000.0, 1e6)) : 0u, std::memory_order_relaxed);
        g_stageLowerable.store(lowerable ? 1u : 0u, std::memory_order_relaxed);
    }
    if (!mayChange) return false; // (too soon after the last change: the holds go on counting)
    if (target && cfg_.calm && pendingUndo_ >= 0 && now >= undoDueAt_) {
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
            if ((hold30 ? over : overT_ >= cfg_.downHoldSec) || freeOverT_ >= cfg_.downHoldSec) {
                // The GPU is the limit: what means least to a player goes first, before any rung or step.
                anisoBlockLen_ = anisoBlockLen_ > 0 ? std::min(anisoBlockLen_ * 2.0, cfg_.maxBlockSec) : cfg_.blockSec;
                anisoBlockedUntil_ = now + static_cast<uint64_t>(anisoBlockLen_ * 1e6);
                lastBlockAt_ = now;
                SetAniso(false, Why::AnisoOff, now);
                return false;
            }
        } else if (anisoRoomT_ >= cfg_.anisoHoldSec && now >= anisoBlockedUntil_) {
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
        if (steps_ && (fxRoomT_ >= cfg_.fxOffHoldSec || (hold30 && calmRun_ >= cfg_.holdClimbWindows)) && now >= fxOffBlockedUntil_) {
            lastFxOffAt_ = now;
            SetSteps(steps_ & ~RecoveryStep(), Why::FxOff, now); // (half-size effects first, then the cheaper background)
            return false;
        }
    }
    const int floorUp = std::max(kToday, cfg_.minRung); // the lowest rung an overload above today goes to
    if (rung_ > floorUp && overT_ < cfg_.downHoldSec && freeOverT_ >= cfg_.freeDownSec) {
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
    if (rung_ > floorUp && overT_ >= cfg_.downHoldSec) {
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
    if (!checking_ && !fxFirst && !floorKnown && !cannotGain && rung_ <= kToday && rung_ > cfg_.minRung && boundT_ >= cfg_.belowHoldSec &&
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
        !checking_ && pendingUndo_ < 0 && !trial_ && !pendingBelow_) {
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
        if (gpuNow + save <= cfg_.holdClimbGpuFrac * cfg_.budgetMs) {
            SetHoldEdge(false, Why::HoldEdgeOn, now);
            edgeTrialUntil_ = now + static_cast<uint64_t>(cfg_.holdTrialSec * 1e6);
            return false;
        }
    }
    if (cfg_.recover && cfg_.calm) {
        // (GXV_AUTOQ_RECOVER: room for recoverHoldSec in a row, and not past what every window of it allowed)
        if (roomT_ >= cfg_.recoverHoldSec) {
            checking_ = false;
            Climb(rung_ + std::max(1, std::min(up, minUp_)), now);
            return true;
        }
        return false;
    }
    if (roomT_ >= (cfg_.calm ? cfg_.upLongHoldSec : cfg_.upHoldSec)) {
        checking_ = false;
        Climb(rung_ + up, now);
        return true;
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
    if (vglRtSetup && vglRtLastGpuFrame && !(te && *te == '0')) {
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
