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
uint32_t vglRtLastGpuFrame(VgxRtGpuFrame* out) __attribute__((weak));
void vglRtGpuCounters(uint32_t* timed, uint32_t* untimed, uint32_t* late, uint32_t* pending) __attribute__((weak));
}
constexpr uint32_t kVglRtGpuTimers = 0x8; // VGL_RT_GPU_TIMERS
#endif

namespace vgx::autoq {

bool Target30Enabled() {
    // GXV_AUTOQ_TARGET=30 targets whole-picture throughput; 0 (default) keeps the old GPU-only policy and sampling off.
    static const bool enabled = EnvInt("GXV_AUTOQ_TARGET", 0) == 30;
    return enabled;
}

namespace {
// One writer (the game thread), bounded snapshot retries on the renderer. Keep
// the publication 32-bit so this does not need a 64-bit atomic lock on ARMv7.
std::atomic<uint32_t> g_gameSeq{0}, g_gameCpu{0}, g_gameTurns{0}, g_gameValid{0}, g_gameFaults{0};
std::atomic<uint32_t> g_pictureWorkUs{0}, g_pictureFaults{0};
int64_t g_gameCpuStart = 0;
static_assert(std::atomic<uint32_t>::is_always_lock_free);
}

extern "C" void port_autoq_game_begin() {
    if (!Target30Enabled()) return;
    g_gameCpuStart = port_thread_cpu_us ? port_thread_cpu_us() : 0;
}

extern "C" void port_autoq_game_work_done() {
    if (!Target30Enabled()) return;
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
    if (!Target30Enabled()) return 0;
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
    if (!Target30Enabled()) return 0;
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
    default: return "-";
    }
}

Controller::Controller(const Config& c) : cfg_(c) {
    cfg_.minRung = std::clamp(cfg_.minRung, cfg_.targetFps == 30 ? kEmergency : 0, kRungs - 1);
    cfg_.maxRung = std::clamp(cfg_.maxRung, std::max(0, cfg_.minRung), kRungs - 1);
    cfg_.startRung = std::clamp(cfg_.startRung, std::max(0, cfg_.minRung), cfg_.maxRung);
    if (cfg_.windowFrames < 2) cfg_.windowFrames = 2;
    rung_ = cfg_.startRung;
    last_.from = last_.to = rung_;
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
    boundFrames_ = 0;
}

void Controller::Block(int rung, uint64_t now) {
    double& len = blockLen_[rung + 1];
    len = len > 0 ? std::min(len * 2.0, cfg_.maxBlockSec) : cfg_.blockSec;
    blockUntil_[rung + 1] = now + static_cast<uint64_t>(len * 1e6);
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
    ResetWindow();
    ResetHolds();
}

void Controller::Remember(uint32_t key, int rung) {
    if (key == 0) return;
    for (SceneRung& m : sceneMem_)
        if (m.key == key) { m.rung = static_cast<int8_t>(rung); return; }
    sceneMem_[sceneNext_ % 32] = SceneRung{key, static_cast<int8_t>(rung)};
    ++sceneNext_;
}

int Controller::Recall(uint32_t key) const {
    if (key == 0) return kNoRung;
    for (const SceneRung& m : sceneMem_)
        if (m.key == key) return m.rung;
    return kNoRung;
}

void Controller::Limit(int highest, uint64_t now) {
    if (highest >= rung_) return;
    highest = std::max(highest, std::min(last_.from, rung_));
    for (int r = highest + 1; r <= cfg_.maxRung; ++r) Block(r, now);
    checking_ = false;
    rung_ = highest;
    last_.to = rung_;
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
        if (cfg_.targetFps == 30) g_pictureWorkUs.store(0, std::memory_order_release);
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
    const uint64_t interval = f.presentUs > prevPresent_ ? f.presentUs - prevPresent_ : 0;
    const uint64_t prevDone = prevDone_, prevRun = prevRun_;
    if (sinceChange_ < (1u << 30)) ++sinceChange_;
    const bool mayChange = sinceChange_ >= static_cast<unsigned>(cfg_.minChangeFrames) &&
        (cfg_.targetFps != 30 || changes_ == 0 || f.presentUs >= last_.atUs + 3000000u);
    const uint32_t gameRun = f.gameRunUs - prevGameRun_, gameTurns = f.gameTurns - prevGameTurns_;
    const bool gameKnown = f.gameKnown && prevGameKnown_ && f.gameFaults == prevGameFaults_;
    if (cfg_.targetFps == 30) {
        bool currentRoom = false;
        const bool backendKnown = f.backendRunUs > 0 && prevRun > 0 && f.backendRunUs >= prevRun &&
            f.backendRunUs <= INT64_MAX && prevRun <= INT64_MAX;
        const bool gpuKnown = f.timers ? f.busyKnown : !f.paced && f.gpuDoneUs != 0 && prevDone != 0;
        if (!gameKnown || !gameTurns || !backendKnown || !gpuKnown) {
            hintKnown_ = false;
            currentWorkUs_ = 0;
            g_pictureWorkUs.store(0, std::memory_order_release);
        } else {
            const uint64_t doneSpan = f.gpuDoneUs > prevDone ? f.gpuDoneUs - prevDone : 0;
            const uint64_t from = std::max(prevDone, f.startUs);
            const uint64_t fenceBusy = f.gpuDoneUs > from ? std::min(f.gpuDoneUs - from, doneSpan) : 0;
            const uint64_t gpu = f.timers ? f.gpuBusyUs : fenceBusy;
            const uint64_t game = uint64_t(gameRun) * 2 / gameTurns;
            const uint64_t backend = f.backendRunUs - prevRun;
            currentWorkUs_ = static_cast<uint32_t>(std::min<uint64_t>(1000000,
                std::max({gpu, backend, game})));
            // A known slow current picture must invalidate old headroom even when a quiet return skips evaluation.
            const uint32_t published = g_pictureWorkUs.load(std::memory_order_relaxed);
            if (published && currentWorkUs_ > published) g_pictureWorkUs.store(currentWorkUs_, std::memory_order_release);
            const int candidate = std::min(rung_ + std::max(1, pendingUp_), cfg_.maxRung);
            currentRoom = interval <= cfg_.budgetMs * 1030.0 &&
                std::max({double(game), double(backend), gpu * Cost(candidate) / Cost(rung_)}) <= cfg_.upTarget * cfg_.budgetMs * 1000.0;
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
        if (cfg_.targetFps == 30) g_pictureWorkUs.store(0, std::memory_order_relaxed);
        // A load or a shader compile: what came before says nothing about what comes next.
        ResetWindow();
        ResetHolds();
        checking_ = false;
        if (static_cast<double>(interval) > cfg_.longGapMs * 1000.0 && cfg_.calm) {
            // A load (calm mode): the rung of the scene left is remembered, the new scene starts at its own remembered
            // rung (or the start rung), applied now, while the load hides it. The same scene after a long gap (a
            // shader compile, the console suspended) keeps its rung and its blocks.
            const uint32_t from = scene_, to = f.scene;
            Remember(from, rung_);
            scene_ = to;
            if (to != 0 && to == from) return false;
            for (int i = 0; i <= kRungs; ++i) { blockUntil_[i] = 0; blockLen_[i] = 0; }
            belowBlockedUntil_ = 0;
            belowBlockLen_ = 0;
            int want = Recall(to);
            if (want == kNoRung) want = cfg_.startRung;
            want = std::clamp(want, cfg_.minRung, cfg_.maxRung);
            if (want != rung_) {
                SetRung(want, Why::NewScene, f.presentUs);
                return true;
            }
            return false;
        }
        if (static_cast<double>(interval) > cfg_.longGapMs * 1000.0) {
            // A new scene: nothing learnt about the old one holds, and its rung may be wrong for this one. Back to the
            // start rung (today's picture), from where the next windows decide again.
            for (int i = 0; i <= kRungs; ++i) { blockUntil_[i] = 0; blockLen_[i] = 0; }
            belowBlockedUntil_ = 0;
            belowBlockLen_ = 0;
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
        Remember(scene_, last_.why == Why::NewScene && sinceChange_ <= 60 ? last_.from : rung_);
        scene_ = f.scene;
        const int want = Recall(f.scene);
        if (last_.why == Why::NewScene && sinceChange_ <= 60 && want != kNoRung && want != rung_) {
            SetRung(std::clamp(want, cfg_.minRung, cfg_.maxRung), Why::NewScene, f.presentUs);
            return true;
        }
    }
    if (cfg_.calm && f.quiet) {
        // A quiet moment: not measured; a step up the last windows of play allowed is taken now.
        ResetWindow();
        overFrames_ = 0;
        if (pendingUndo_ >= 0 && mayChange) {
            const int to = pendingUndo_;
            pendingUndo_ = -1;
            pendingBelow_ = false;
            checking_ = false;
            SetRung(to, Why::NoGain, f.presentUs);
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
            checkSpan_ = 0;
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
    if (cfg_.targetFps == 30 && f.paced && !f.timers) {
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
    if (cfg_.targetFps == 30) {
        if (gameKnown) { gameRun_ += gameRun; gameTurns_ += gameTurns; }
        else gameKnown_ = false;
    }
    // Fast path: above today, a few frames in a row near the GPU's limit step down now.
    const int floorUp = std::max(kToday, cfg_.minRung);
    if (frameLoad >= 0.0) overFrames_ = frameLoad >= cfg_.saturated ? overFrames_ + 1 : 0;
    if (cfg_.targetFps != 30 && rung_ > floorUp && overFrames_ >= static_cast<unsigned>(cfg_.fastFrames) && mayChange) {
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
        if (cfg_.targetFps == 30) g_pictureWorkUs.store(0, std::memory_order_release);
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
    const bool target = cfg_.targetFps == 30;
    const double gpuMs = target ? static_cast<double>(busy_) / busyN_ / 1000.0 : 0.0;
    const double backendMs = target && runKnown_ ? static_cast<double>(run_) / frames_ / 1000.0 : -1.0;
    // Two 60 Hz game turns are required per 30 Hz picture. Use CPU work, not
    // retrace/pacing waits, and a maximum: all three pipeline stages overlap.
    const double gameMs = target && gameKnown_ && gameTurns_ ? static_cast<double>(gameRun_) * 2.0 / gameTurns_ / 1000.0 : -1.0;
    const bool wholeKnown = gameMs >= 0.0 && backendMs >= 0.0;
    winGameMs_ = gameMs;
    winBackendMs_ = backendMs;
    winWorkMs_ = std::max({gpuMs, gameMs, backendMs});
    if (target) {
        const uint32_t work = wholeKnown && hintKnown_ ? std::max(currentWorkUs_,
            static_cast<uint32_t>(std::min(winWorkMs_ * 1000.0, 1000000.0))) : 0;
        // Invalidate before changing stamp. The getter brackets work with stamp/fault reads, so it cannot pair old work with a new fault generation.
        g_pictureWorkUs.store(0, std::memory_order_release);
        g_pictureFaults.store(prevGameFaults_, std::memory_order_release);
        g_pictureWorkUs.store(work, std::memory_order_release);
    }
    const double measureMs = target ? winWorkMs_ : winIntervalMs_;
    if (target) winLoad_ = gpuMs / cfg_.budgetMs;
    ++windows_;

    if (checking_) {
        // A step below today's size is kept only if the frames got shorter.
        checkSpan_ += target ? measureMs * 1000.0 * frames_ : static_cast<double>(present_);
        checkFrames_ += frames_;
        if (checkSpan_ >= cfg_.checkSec * 1e6) {
            checking_ = false;
            const double after = checkSpan_ / 1000.0 / checkFrames_;
            // (undone only when the frames stayed about as long: when they got much longer the scene got heavier during
            // the check, which says nothing against the smaller picture; the Strikers port, cc302e6)
            if (after > checkBefore_ * (1.0 - cfg_.betterFrac) && after < checkBefore_ * 1.15) {
                belowBlockLen_ = belowBlockLen_ > 0 ? std::min(belowBlockLen_ * 2.0, cfg_.maxBlockSec) : cfg_.blockSec;
                belowBlockedUntil_ = now + static_cast<uint64_t>(belowBlockLen_ * 1e6);
                if (cfg_.calm) {
                    pendingUndo_ = checkFrom_; // (calm: back at the next quiet moment)
                } else {
                    SetRung(checkFrom_, Why::NoGain, now);
                    return true;
                }
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
    while (up < cfg_.maxUpStep && rung_ + up + 1 <= cfg_.maxRung && !Blocked(rung_ + up + 1, now) &&
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
        boundFrames_ += frames_;
    } else {
        boundT_ = 0;
        boundSpan_ = 0;
        boundFrames_ = 0;
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
    if (!checking_ && rung_ <= kToday && rung_ > cfg_.minRung && boundT_ >= cfg_.belowHoldSec && now >= belowBlockedUntil_ && pendingUndo_ < 0) {
        const int from = rung_;
        const double before = boundFrames_ ? boundSpan_ / 1000.0 / boundFrames_ : winIntervalMs_;
        if (cfg_.calm && (!target || boundT_ < 8.0)) {
            // (calm: a step below today only buys a little speed, so it waits for a quiet moment as a step up does)
            pendingBelow_ = true;
            belowAt_ = now;
            belowBefore_ = before;
            return false;
        }
        // Target mode prefers a quiet downstep too, but a persistent GPU miss
        // for 8 s may lower resolution in play rather than skip indefinitely.
        SetRung(rung_ - 1, Why::GpuBound, now);
        checking_ = true;
        checkFrom_ = from;
        checkBefore_ = before;
        checkSpan_ = 0;
        checkFrames_ = 0;
        return true;
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
        if (n != 0 && n != g_rtHead) {
            g_rtHead = n;
            // Target30 must not mistake dropped scene timings for GPU headroom.
            // Consume incomplete publications once, but leave their cost unknown.
            if (!Target30Enabled() || (g.scenes != 0 && g.timed == g.scenes)) {
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
