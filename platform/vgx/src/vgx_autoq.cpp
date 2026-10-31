// Automatic picture quality: the controller and the GPU clock (see vgx_autoq.h for the rules).
#include "vgx_autoq.h"
#include "vgx_common.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>

extern "C" int64_t port_thread_cpu_us(void) __attribute__((weak));
extern "C" unsigned port_vgx_pace_divisor(void) __attribute__((weak));

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
static_assert(std::atomic<uint32_t>::is_always_lock_free);
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
    case Why::AnisoOn: return "room at the top rung: anisotropic filtering on";
    case Why::AnisoOff: return "the GPU is the limit: anisotropic filtering goes first";
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
    lastFx_.from = lastFx_.to = lastAniso_.from = lastAniso_.to = rung_;
}

void Controller::ResetWindow() {
    busy_ = span_ = present_ = run_ = 0;
    frames_ = busyN_ = 0;
    runKnown_ = true;
    gameRun_ = gameTurns_ = 0;
    gameKnown_ = true;
    hintKnown_ = true;
}

void Controller::ResetHolds() {
    overT_ = boundT_ = roomT_ = 0;
    overFrames_ = 0;
    boundSpan_ = 0;
    boundIntervalSum_ = 0;
    boundFrames_ = 0;
    fxRoomT_ = 0;
    anisoRoomT_ = 0;
    boundGpuSum_ = 0;
    boundGpuFrames_ = 0;
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
    last_ = Change{rung_, to, why, winLoad_, winCpu_, winIntervalMs_, now};
    rung_ = to;
    ++changes_;
    sinceChange_ = 0;
    settleUntil_ = now + static_cast<uint64_t>(cfg_.settleSec * 1e6);
    pendingUp_ = 0;
    roomAt_ = 0;
    pendingBelow_ = false;
    if (why == Why::NewScene) pendingUndo_ = -1;
    // (target mode: a change to the rung an undo was waiting for, or above it, has done the undo's work)
    if (Target() && pendingUndo_ >= 0 && to >= pendingUndo_) pendingUndo_ = -1;
    if (aniso_ && to != cfg_.maxRung) {
        // (it belongs to the top rung: any other has it off)
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

// The next step to try at today's rung (0: none left, or blocked): half-size effects and blur first, then the cheaper background.
unsigned Controller::NextStep(uint64_t now) const {
    if (cfg_.fxStep && !(steps_ & 1) && now >= onBlockedUntil_[0]) return 1;
    if (cfg_.bgStep && !(steps_ & 2) && now >= onBlockedUntil_[1]) return 2;
    return 0;
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
    stepGain_[0] = stepGain_[1] = 0;
}

// Anisotropic filtering on or off at the top rung. Nothing is made again for it (no render target), so the windows restart but
// no settling is needed beyond the first quarter second; the next change is at least 3 s away.
void Controller::SetAniso(bool to, Why why, uint64_t now) {
    if (aniso_ == to) return;
    lastAniso_ = Change{rung_, rung_, why, winLoad_, winCpu_, winIntervalMs_, now};
    aniso_ = to;
    ++anisoChanges_;
    anisoAt_ = now;
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
    onBlockedUntil_[0] = onBlockedUntil_[1] = fxOffBlockedUntil_ = 0;
    onBlockLen_[0] = onBlockLen_[1] = fxOffBlockLen_ = 0;
    anisoBlockedUntil_ = 0;
    anisoBlockLen_ = 0;
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

FloorWindow Controller::TakeFloor() {
    const FloorWindow w = floor_;
    floor_ = FloorWindow();
    return w;
}

// One presented picture of the floor census (target mode). Quiet moments and stalls (loads, shader builds) are counted apart;
// a late picture is blamed on the largest stage that reached causeShare of the budget: the GPU's busy time, two game turns'
// CPU, the renderer's CPU. None that did, with all three measured: something unmeasured (the front end, a wait, the pacing).
void Controller::FloorCount(uint64_t interval, bool quiet, bool measured, bool gpuKnown, bool gameKnown, bool backendKnown,
                            uint64_t gpu, uint64_t game, uint64_t backend) {
    FloorWindow& w = floor_;
    if (quiet) { ++w.quiet; return; }
    if (static_cast<double>(interval) > cfg_.gapResetMs * 1000.0) { ++w.stalls; w.stallUs += interval; return; }
    ++w.pictures;
    w.spanUs += interval;
    if (static_cast<double>(interval) <= cfg_.budgetMs * 1030.0) return;
    ++w.late;
    w.lateUs += interval;
    w.worstUs = std::max(w.worstUs, interval);
    const double threshold = cfg_.causeShare * cfg_.budgetMs * 1000.0;
    int cause = -1;
    double best = 0;
    auto consider = [&](bool known, uint64_t v, int id) {
        if (known && static_cast<double>(v) >= threshold && static_cast<double>(v) > best) { best = static_cast<double>(v); cause = id; }
    };
    consider(gpuKnown, gpu, 0);
    consider(gameKnown, game, 1);
    consider(backendKnown, backend, 2);
    if (cause == 0) ++w.gpu;
    else if (cause == 1) ++w.game;
    else if (cause == 2) ++w.renderer;
    else if (measured) ++w.other;
    else ++w.unknown;
}

void Controller::Limit(int highest, uint64_t now) {
    if (highest >= rung_) return;
    highest = std::max(highest, std::min(last_.from, rung_));
    for (int r = highest + 1; r <= cfg_.maxRung; ++r) Block(r, now);
    checking_ = false;
    rung_ = highest;
    last_.to = rung_;
    if (aniso_ && rung_ != cfg_.maxRung) { aniso_ = false; ++anisoChanges_; anisoAt_ = now; }
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
        return false;
    }
    if (cfg_.recover && cfg_.calm && lastBlockAt_ != 0 && f.presentUs >= lastBlockAt_ + static_cast<uint64_t>(cfg_.blockForgetSec * 1e6)) {
        // (GXV_AUTOQ_RECOVER: the blocks that ran out long ago do not count towards the next one's length)
        for (double& len : blockLen_) len = 0;
        belowBlockLen_ = 0;
        onBlockLen_[0] = onBlockLen_[1] = fxOffBlockLen_ = anisoBlockLen_ = 0;
        lastBlockAt_ = 0;
    }
    const uint64_t interval = f.presentUs > prevPresent_ ? f.presentUs - prevPresent_ : 0;
    const uint64_t prevDone = prevDone_, prevRun = prevRun_;
    if (sinceChange_ < (1u << 30)) ++sinceChange_;
    const bool mayChange = sinceChange_ >= static_cast<unsigned>(cfg_.minChangeFrames) &&
        (!Target() || changes_ == 0 || f.presentUs >= last_.atUs + 3000000u) &&
        (!fxEver_ || f.presentUs >= lastFxAt_ + 3000000u) && (anisoAt_ == 0 || f.presentUs >= anisoAt_ + 3000000u);
    const uint32_t gameRun = f.gameRunUs - prevGameRun_, gameTurns = f.gameTurns - prevGameTurns_;
    const bool gameKnown = f.gameKnown && prevGameKnown_ && f.gameFaults == prevGameFaults_;
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
            FloorCount(interval, f.quiet, gameOk && backendKnown && gpuKnown, gpuKnown, gameOk, backendKnown,
                       f.timers ? f.gpuBusyUs : fenceBusy, gameOk ? uint64_t(gameRun) * PictureTurns() / gameTurns : 0,
                       backendKnown ? f.backendRunUs - prevRun : 0);
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
            currentWorkUs_ = static_cast<uint32_t>(std::min<uint64_t>(1000000,
                std::max({gpu, backend, game2})));
            // A known slow current picture must invalidate old headroom even when a quiet return skips evaluation.
            const uint32_t published = g_pictureWorkUs.load(std::memory_order_relaxed);
            if (published && currentWorkUs_ > published) g_pictureWorkUs.store(currentWorkUs_, std::memory_order_release);
            const int candidate = std::min(rung_ + std::max(1, pendingUp_), cfg_.maxRung);
            // (GXV_AUTOQ_RECOVER: a single frame ends the stretch of room only when it really missed, not when it was merely
            // above the margin the window means must keep: recoverFrameFrac against upTarget)
            const double frameFrac = cfg_.recover && cfg_.calm ? cfg_.recoverFrameFrac : cfg_.upTarget;
            currentRoom = interval <= cfg_.budgetMs * 1030.0 &&
                std::max({double(game), double(backend), gpu * Cost(candidate) / Cost(rung_)}) <= frameFrac * cfg_.budgetMs * 1000.0;
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
            // A load (calm mode): the rung of the scene left is remembered, the new scene starts at its own remembered
            // rung (or the start rung), applied now, while the load hides it. The same scene after a long gap (a
            // shader compile, the console suspended) keeps its rung and its blocks.
            const uint32_t from = scene_, to = f.scene;
            // (target mode: a scene left before an undo for no gain was made is remembered at the rung the undo goes back to)
            Remember(from, Target() && pendingUndo_ >= 0 ? pendingUndo_ : rung_, Mem(), sceneFloor_);
            scene_ = to;
            if (to != 0 && to == from) return false;
            checking_ = false;
            if (Target()) pendingUndo_ = -1; // (an undo learnt in the scene left: the new scene starts at its own rung)
            RecallFloor(to, f.presentUs);
            for (int i = 0; i <= kRungs; ++i) { blockUntil_[i] = 0; blockLen_[i] = 0; }
            belowBlockedUntil_ = 0;
            belowBlockLen_ = 0;
            ForgetFxBlocks();
            unsigned wantFx = 0;
            int want = Recall(to, &wantFx);
            if (want == kNoRung) want = cfg_.startRung;
            want = std::clamp(want, cfg_.minRung, cfg_.maxRung);
            const bool wantAniso = cfg_.aniso && (wantFx & 4u) != 0 && want == cfg_.maxRung;
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
            if (Target()) pendingUndo_ = -1;
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
        const int want = Recall(f.scene, &wantFx);
        if (last_.why == Why::NewScene && sinceChange_ <= 60 && want != kNoRung && want != rung_) {
            const int to = std::clamp(want, cfg_.minRung, cfg_.maxRung);
            SetSceneSteps(to <= FxRung() ? (wantFx & StepMask()) : 0u, f.presentUs);
            SetSceneAniso(cfg_.aniso && (wantFx & 4u) != 0 && to == cfg_.maxRung, f.presentUs);
            SetRung(to, Why::NewScene, f.presentUs);
            return true;
        }
    }
    if (cfg_.calm && f.quiet) {
        // A quiet moment: not measured; a step up the last windows of play allowed is taken now.
        ResetWindow();
        overFrames_ = 0;
        if (pendingUndo_ >= 0 && mayChange) {
            UndoBelow(f.presentUs);
            return true;
        }
        if (pendingBelow_ && mayChange && f.presentUs >= belowAt_ && static_cast<double>(f.presentUs - belowAt_) <= cfg_.roomMemorySec * 1e6 &&
            !checking_ && rung_ <= kToday && rung_ > cfg_.minRung) {
            const int from = rung_;
            pendingBelow_ = false;
            ++quietUps_;
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
        if (pendingUp_ > 0 && roomAt_ != 0 && f.presentUs >= roomAt_ && static_cast<double>(f.presentUs - roomAt_) <= cfg_.roomMemorySec * 1e6 && mayChange) {
            const int to = std::min(rung_ + pendingUp_, cfg_.maxRung);
            ++quietUps_;
            checking_ = false;
            SetRung(to, Why::Headroom, f.presentUs);
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
    if (static_cast<double>(interval) > cfg_.outlierMs * 1000.0) return false;   // one slow frame (a hitch): not counted
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

bool Controller::Evaluate(uint64_t now, bool mayChange) {
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
    winGameMs_ = gameMs;
    winBackendMs_ = backendMs;
    winWorkMs_ = std::max({gpuMs, gameMs, backendMs});
    if (target) {
        const uint32_t work = wholeKnown && hintKnown_ ? std::max(currentWorkUs_,
            static_cast<uint32_t>(std::min(std::max({gpuMs, game2Ms, backendMs}) * 1000.0, 1000000.0))) : 0;
        // Invalidate before changing stamp. The getter brackets work with stamp/fault reads, so it cannot pair old work with a new fault generation.
        g_pictureWorkUs.store(0, std::memory_order_release);
        g_pictureFaults.store(prevGameFaults_, std::memory_order_release);
        g_pictureWorkUs.store(work, std::memory_order_release);
    }
    const double measureMs = target ? winWorkMs_ : winIntervalMs_;
    if (target) winLoad_ = gpuMs / cfg_.budgetMs;
    ++windows_;
    const bool fxActive = StepsEnabled() && target;

    if (trial_) {
        // A step is kept only if the GPU's busy time got shorter (judged on the GPU alone: the steps touch nothing else).
        fxCheckT_ += dt;
        fxCheckSum_ += gpuMs * frames_;
        fxCheckFrames_ += frames_;
        if (fxCheckT_ >= cfg_.checkSec && fxCheckFrames_ > 0) {
            const unsigned bit = trial_;
            const int idx = static_cast<int>(bit) - 1;
            trial_ = 0;
            const double after = fxCheckSum_ / fxCheckFrames_;
            if (after > fxBefore_ * (1.0 - cfg_.betterFrac) && after < fxBefore_ * 1.15) {
                // (as for a step below today: when the frames got much longer the scene got heavier, which says nothing against the step)
                onBlockLen_[idx] = onBlockLen_[idx] > 0 ? std::min(onBlockLen_[idx] * 2.0, cfg_.maxBlockSec) : cfg_.blockSec;
                onBlockedUntil_[idx] = now + static_cast<uint64_t>(onBlockLen_[idx] * 1e6);
                lastBlockAt_ = now;
                stepGain_[idx] = 0;
                SetSteps(steps_ & ~bit, Why::FxNoGain, now);
                boundT_ = std::max(boundT_, fxBoundKept_); // (the GPU has been the limit all this time: the next step need not wait again)
                return false;
            }
            if (after > 0.0 && after < fxBefore_ * (1.0 - cfg_.betterFrac)) stepGain_[idx] = std::clamp(fxBefore_ / after, 1.05, 3.0);
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
                if (cfg_.calm) {
                    pendingUndo_ = checkFrom_; // (calm: back at the next quiet moment; target mode: within noGainUndoSec)
                    undoDueAt_ = now + static_cast<uint64_t>(cfg_.noGainUndoSec * 1e6);
                } else {
                    SetRung(checkFrom_, Why::NoGain, now);
                    return true;
                }
            } else if (target && sceneFloor_ != kNoRung && checkFrom_ <= sceneFloor_) {
                sceneFloor_ = kNoRung; // (a step from there bought speed this time: what the scene learnt before no longer holds)
                sceneFloorUntil_ = 0;
            }
        }
    }

    // A paced 50 ms interval is not 50 ms of GPU work. Missed cadence is useful
    // only alongside actual GPU work near the 30 Hz budget and a CPU margin.
    const bool gpuBound = target && wholeKnown && gpuMs >= cfg_.budgetMs * cfg_.downBelow &&
        gpuMs >= std::max(gameMs, backendMs) * 1.05 &&
        (winIntervalMs_ > cfg_.budgetMs * 1.03 || winWorkMs_ > cfg_.budgetMs);
    const bool over = target ? gpuBound : load >= cfg_.downAbove;
    const bool bound = target ? gpuBound : load >= cfg_.downBelow && cpu >= 0.0 && cpu <= cfg_.cpuMax;
    // How far up the prediction allows (at most maxUpStep rungs at once, never past a blocked rung, and onto a rung that
    // has failed before but not past it: such a rung is tried again on its own).
    // (Calm mode: the GPU's busy time against the frame budget as well, so a frame the CPU made long does not hide it.)
    double upLoad = load;
    if (!target && cfg_.calm && winIntervalMs_ > 0.0) {
        const double busyMs = std::min(load, 1.2) * winIntervalMs_;
        upLoad = std::max(load, busyMs / std::min(winIntervalMs_, cfg_.budgetMs));
    }
    int up = 0;
    // Unknown front-end/shared-core work may also miss the target. Do not add
    // GPU quality until actual cadence fits, even if measured stages look cheap.
    // (the step belongs to the bottom rungs: while it is on, the climb stops at the bottom ordinary rung; the step goes off first)
    const int upMax = fxActive && steps_ ? std::min(cfg_.maxRung, FxRung()) : cfg_.maxRung;
    while (up < cfg_.maxUpStep && rung_ + up + 1 <= upMax && !Blocked(rung_ + up + 1, now) &&
           (target ? wholeKnown && hintKnown_ && winIntervalMs_ <= cfg_.budgetMs * 1.03 &&
            std::max({gameMs, backendMs, gpuMs * Cost(rung_ + up + 1) / Cost(rung_)}) <= cfg_.upTarget * cfg_.budgetMs :
            upLoad * Cost(rung_ + up + 1) / Cost(rung_) <= cfg_.upTarget)) {
        ++up;
        if (blockLen_[rung_ + up + 1] > 0) break;
    }
    const bool room = up > 0;
    overT_ = over ? overT_ + dt : 0.0;
    roomT_ = room ? roomT_ + dt : 0.0;
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
        const int idx = (steps_ & 2) ? 1 : 0;
        const double ratio = std::max(stepGain_[idx] > 0.0 ? stepGain_[idx] : (idx ? cfg_.bgRatio : cfg_.fxRatio), 1.05);
        const bool offRoom = wholeKnown && hintKnown_ && winIntervalMs_ <= cfg_.budgetMs * 1.03 &&
            std::max({gameMs, backendMs, gpuMs * ratio}) <= cfg_.upTarget * cfg_.budgetMs;
        fxRoomT_ = offRoom ? fxRoomT_ + dt : 0.0;
    } else {
        fxRoomT_ = 0.0;
    }
    if (cfg_.aniso && target && !aniso_ && rung_ == cfg_.maxRung && !steps_ && !checking_ && pendingUndo_ < 0 && !trial_) {
        // Room for anisotropic filtering at the top rung: the GPU's busy time as it would be with it (the assumed ratio), the other
        // stages, and the cadence, within the margin a step up keeps.
        const bool anisoRoom = wholeKnown && hintKnown_ && winIntervalMs_ <= cfg_.budgetMs * 1.03 &&
            std::max({gameMs, backendMs, gpuMs * cfg_.anisoRatio}) <= cfg_.upTarget * cfg_.budgetMs;
        anisoRoomT_ = anisoRoom ? anisoRoomT_ + dt : 0.0;
    } else {
        anisoRoomT_ = 0.0;
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
    if (!mayChange) return false; // (too soon after the last change: the holds go on counting)
    if (target && cfg_.calm && pendingUndo_ >= 0 && now >= undoDueAt_) {
        // (target mode: a smaller picture that bought nothing is not kept for want of a quiet moment; nothing else moves first)
        UndoBelow(now);
        return true;
    }
    if (cfg_.aniso && target && rung_ == cfg_.maxRung) {
        if (aniso_) {
            if (overT_ >= cfg_.downHoldSec) {
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
        if (steps_ && fxRoomT_ >= cfg_.fxOffHoldSec && now >= fxOffBlockedUntil_) {
            lastFxOffAt_ = now;
            SetSteps(steps_ & ~((steps_ & 2) ? 2u : 1u), Why::FxOff, now); // (the cheaper background first, then the effects)
            return false;
        }
    }
    const int floorUp = std::max(kToday, cfg_.minRung); // the lowest rung an overload above today goes to
    if (rung_ > floorUp && overT_ >= cfg_.downHoldSec) {
        int to = rung_ - 1;
        const bool saturated = target ? gpuMs >= cfg_.budgetMs : load >= cfg_.saturated;
        if (saturated && !cfg_.calm) to = floorUp; // (calm: as far as the load says, not straight to today)
        else
            while (to > floorUp && (target ? gpuMs / cfg_.budgetMs : load) * Cost(to) / Cost(rung_) > cfg_.upTarget) --to;
        Block(rung_, now);
        checking_ = false;
        SetRung(to, saturated ? Why::Saturated : Why::Overload, now);
        return true;
    }
    // (the half-size step goes before the emergency rung: while it is still to come, or being judged, the rung stays)
    const bool fxFirst = fxActive && rung_ == FxRung() && (trial_ || NextStep(now));
    // (target mode: and not below the rung where an earlier visit of this scene found that a step bought nothing)
    const bool floorKnown = target && sceneFloor_ != kNoRung && rung_ <= sceneFloor_ && now < sceneFloorUntil_;
    if (!checking_ && !fxFirst && !floorKnown && rung_ <= kToday && rung_ > cfg_.minRung && boundT_ >= cfg_.belowHoldSec &&
        now >= belowBlockedUntil_ && pendingUndo_ < 0) {
        const int from = rung_;
        // (target mode: the GPU's mean busy time over the GPU-limited stretch, what the step is judged on)
        const double before = target ? (boundGpuFrames_ ? boundGpuSum_ / boundGpuFrames_ : gpuMs)
                                     : (boundFrames_ ? boundSpan_ / 1000.0 / boundFrames_ : winIntervalMs_);
        // (target mode: the floor is being missed: the pictures of the stretch came slower than the target, or the GPU alone
        // needs more than the budget)
        const double beforeInterval = boundFrames_ ? boundIntervalSum_ / 1000.0 / boundFrames_ : winIntervalMs_;
        const bool missing = target && (before > cfg_.budgetMs || beforeInterval > cfg_.budgetMs * 1.03);
        if (cfg_.calm && (!target || boundT_ < 8.0)) {
            // (calm: a step below today only buys a little speed, so it waits for a quiet moment as a step up does)
            pendingBelow_ = true;
            belowAt_ = now;
            belowBefore_ = before;
            belowMissing_ = missing;
            return false;
        }
        // Target mode prefers a quiet downstep too, but a persistent GPU miss
        // for 8 s may lower resolution in play rather than skip indefinitely.
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
    if (cfg_.recover && cfg_.calm) {
        // (GXV_AUTOQ_RECOVER: room for recoverHoldSec in a row, and not past what every window of it allowed)
        if (roomT_ >= cfg_.recoverHoldSec) {
            checking_ = false;
            SetRung(rung_ + std::max(1, std::min(up, minUp_)), Why::Headroom, now);
            return true;
        }
        return false;
    }
    if (roomT_ >= (cfg_.calm ? cfg_.upLongHoldSec : cfg_.upHoldSec)) {
        checking_ = false;
        SetRung(rung_ + up, Why::Headroom, now);
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
