// Frame-time statistics for the game thread and the renderer, logged every
// 10 s: always with PETARI_PERFLOG=1, otherwise only while the game runs
// below 58 fps (so the headset's log shows slowdowns).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <mutex>
#include <string>

#include "port/port.h"
#include "port/diagnostics.h"
#include "port/game_progress.h"
#include "port/vgx_coldtrace_control.h"
#include "port/frame_pace.h"
#include "port/hot_knob.h"
#include "../../vgx/src/vgx_workload.h"
#include "mtx_profile.h"

#if defined(__vita__) && PORT_LOGGING
#include <errno.h>
#endif

// (declared here and in the vgx glue rather than in port/port.h, which everything includes)
extern "C" int64_t port_thread_cpu_us(void);
extern "C" void port_perf_game_totals(int64_t* workNs, uint32_t* frames, int64_t* cpuUs, int64_t* maxWorkNs);
extern "C" void port_game_progress_report(const char* why);
extern "C" int port_handheld(void);
extern "C" int port_frameskip_turn(int allowed);
extern "C" uint32_t port_frameskip_count(void);
extern "C" unsigned port_vgx_backend_pending(void) __attribute__((weak));  // vgx_galaxy.cpp
extern "C" int port_vgx_pace_turn(int allowed) __attribute__((weak));  // -1: legacy cadence
extern "C" void port_vgx_present_turn(int drawn) __attribute__((weak));  // GXV_PRESENT_SYNC: the turn's vblank stamp (vgx_galaxy.cpp)

extern "C" int64_t port_vgx_player_cpu_us(void) __attribute__((weak));
extern "C" void port_vgx_diagnostic_csv_pause(int selecting) __attribute__((weak));
extern "C" void port_autoq_game_begin(void) __attribute__((weak));
extern "C" void port_autoq_game_work_done(void) __attribute__((weak));
extern "C" int port_autoq_stages(uint32_t* gpuUs, uint32_t* pipeUs, int* lowerable) __attribute__((weak));  // vgx_autoq.cpp (target 60)
extern "C" int port_screen_quiet(void) __attribute__((weak));  // the game: a pause menu or a closed fade

#include "phase_cpu.h"
#include "port/par_exec.h"  // GXV_PAR_VIEW (par13)

namespace {

struct Stat {
    double sum = 0.0, max = 0.0;
    unsigned count = 0;
    void add(double v) {
        sum += v;
        max = v > max ? v : max;
        count++;
    }
    double avg() const { return count ? sum / count : 0.0; }
};

std::mutex sLock;
// Running totals for port_perf_game_totals (the vgx glue's "perf:" line on handhelds).
std::atomic<int64_t> sTotWorkNs{0}, sTotCpuUs{0}, sWinMaxWorkNs{0};
std::atomic<uint32_t> sTotFrames{0}, sSkippedTurns{0};
Stat sWork, sRecord, sPrepare, sRenderCpu, sUpload, sGpuWait;
int64_t sPlayerCpuAtTurnStart = 0;  // Frame-boundary publication, not a turn-scoped CPU sample.
unsigned sFrames = 0, sRenderFrames = 0;
int64_t sFrameStart = 0, sFrameCpuStart = 0, sRecordThisFrame = 0, sWindowStart = 0;
std::atomic<uint32_t> sGameProgress{0};
std::atomic<uint32_t> sAudioProgress{0},sDspProgress{0};
std::atomic<uint32_t> sIrqProgress{0},sGpuProgress{0},sCpuOwner{0};
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(PORT_GAME_PHASE_COUNT<=64);
static_assert(PORT_IRQ_COUNT<64);
struct ProgressObserver {
    uint32_t word=0;
    int64_t lastPoll=0,lastChange=0,lastReport=0;
    unsigned reports=0;
    bool started=false;
} sProgressObserver;
constexpr const char* phaseNames[]={"unknown","begin-render","draw","end-render","update-reset","update-scene",
    "update-objects","update-home","update-error","update-dimming","update-nerve","audio-start",
    "audio-update-scene","audio-gate","audio-wait","audio-nerve","logo-request","logo-update-scene",
    "calc-anim","capture","end-frame","wait-retrace","frame-complete","audio-movement","audio-calc",
    "audio-mix","audio-objects","frame-begin","audio-resources","audio-jas-init","jas-track-pool",
    "jas-dvd-thread","jas-audio-thread","jas-stream-init","audio-jai-init","audio-pools","audio-section",
    "audio-archive","audio-construct","audio-scene-manager","audio-finish","audio-speaker","audio-names",
    "audio-ready","dsp-boot","dsp-buffer","dsp-channels","dsp-channel-pool","audio-dma-start","audio-message-wait",
    "frame-alarm-create","frame-alarm-set","frame-draw-done","frame-reset-sync","frame-alarm-cancel","frame-flush",
    "frame-retrace-count","gx-finish-flush","gx-finish-restore","gx-finish-wait",
    "end-gx-flush","combination-positive","combination-zero","combination-negative"};
static_assert(sizeof(phaseNames)/sizeof(*phaseNames)==PORT_GAME_PHASE_COUNT);
constexpr const char* gpuPhaseNames[]={"unknown","worker-wait","consume","peek","present","sdk-begin","sdk-begin-return",
    "sdk-end","sdk-end-return","sdk-finish","sdk-finish-return","sdk-heartbeat","sdk-heartbeat-return",
    "sdk-display-queue","sdk-display-queue-return","initialize","close","sdk-display-drain","sdk-display-drain-return"};
static_assert(PORT_GAME_GPU_COUNT<=64 && sizeof(gpuPhaseNames)/sizeof(*gpuPhaseNames)==PORT_GAME_GPU_COUNT);
// Read on first use: the VR app sets debug variables after the library loads.
bool alwaysLog() {
    return PORT_HOT_KNOB(bool, getenv("PETARI_PERFLOG") != nullptr);  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}

// Where the game thread's time went in the loop turn in progress (handhelds log a turn that took
// 45 ms or more with its three longest phases: the main loop's own checkpoints delimit them).
enum class TurnKind { Unclassified, DrawSelected, LateSkip, BacklogSkip, ForcedSkip, RequiredDraw, PacedSkip, Count };
constexpr const char* turnKinds[] = {"unclassified", "draw-selected", "late-skip", "backlog-skip", "forced-skip", "required-draw", "paced-skip"};
static_assert(sizeof(turnKinds) / sizeof(*turnKinds) == static_cast<unsigned>(TurnKind::Count));
struct TurnCost {
    unsigned turns = 0, cpuSamples = 0, bothReasons = 0, phaseTurns = 0;
    int64_t work = 0, sampledWork = 0, cpu = 0, draw = 0, anim = 0, audio = 0, finish = 0, record = 0;
} sTurnCosts[static_cast<unsigned>(TurnKind::Count)];
struct TurnPhases {
    int64_t last = 0;
    uint32_t phase = 0;
    int64_t ns[PORT_GAME_PHASE_COUNT] = {};
    TurnKind kind = TurnKind::Unclassified;
    bool bothSkipReasons = false;
} sTurn;
unsigned sSlowTurnsLogged = 0;
bool slowTurnLog() {
    return PORT_HOT_KNOB(bool, alwaysLog() || port_handheld() != 0);  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
void publish(std::atomic<uint32_t>& lane,uint32_t phase) {
    uint32_t old=lane.load(std::memory_order_relaxed);
    while (!lane.compare_exchange_weak(old,((old+64u)&~63u)|phase,std::memory_order_relaxed)) {}
}

void report(int64_t now) {
    double secs = (now - sWindowStart) / 1e9;
    double fps = sFrames / secs;
    const bool verbose = alwaysLog() || fps < 58.0;
    if (verbose) {
        port_log("perf: game %.1f fps, work %.1f ms avg %.1f max (GX recording %.1f avg); renderer %.1f fps, cpu %.1f ms avg %.1f max, "
                 "upload %.1f avg, prepare %.1f avg %.1f max",
                 fps, sWork.avg(), sWork.max, sRecord.avg(), sRenderFrames / secs, sRenderCpu.avg(), sRenderCpu.max, sUpload.avg(), sPrepare.avg(),
                 sPrepare.max);
        if (sGpuWait.count) {
            port_log("perf: GPU finished %.1f ms avg %.1f max after the render thread's submission", sGpuWait.avg(), sGpuWait.max);
        }
    }
    const bool skipped = sTurnCosts[static_cast<unsigned>(TurnKind::LateSkip)].turns ||
                         sTurnCosts[static_cast<unsigned>(TurnKind::BacklogSkip)].turns ||
                         sTurnCosts[static_cast<unsigned>(TurnKind::PacedSkip)].turns ||
                         sTurnCosts[static_cast<unsigned>(TurnKind::ForcedSkip)].turns;
    if (verbose || skipped) {
        // Policy-selected turns, not displayed pictures. Only sampledWork - cpu
        // is a matched off-CPU gap; phase/record columns are not additive.
        for (unsigned i = 0; i < static_cast<unsigned>(TurnKind::Count); ++i) {
            const TurnCost& c = sTurnCosts[i];
            if (!c.turns) continue;
            port_log("turn-cost: kind=%s turns=%u cpu_samples=%u both_reasons=%u phase_turns=%u work_us=%llu sampled_work_us=%llu cpu_us=%llu draw_us=%llu anim_us=%llu audio_objects_us=%llu finish_wait_us=%llu record_us=%llu",
                     turnKinds[i], c.turns, c.cpuSamples, c.bothReasons, c.phaseTurns,
                     static_cast<unsigned long long>(c.work / 1000), static_cast<unsigned long long>(c.sampledWork / 1000),
                     static_cast<unsigned long long>(c.cpu),
                     static_cast<unsigned long long>(c.draw / 1000), static_cast<unsigned long long>(c.anim / 1000),
                     static_cast<unsigned long long>(c.audio / 1000), static_cast<unsigned long long>(c.finish / 1000),
                     static_cast<unsigned long long>(c.record / 1000));
        }
    }
    for (TurnCost& c : sTurnCosts) c = TurnCost{};
    sWork = sRecord = sPrepare = sRenderCpu = sUpload = sGpuWait = Stat();
    sFrames = sRenderFrames = 0;
    sWindowStart = now;
}

}  // namespace

extern "C" {

#if (defined(__vita__) || (defined(PORT_WORKLOAD_GAME_BRIDGES) && PORT_WORKLOAD_GAME_BRIDGES)) && PORT_LOGGING
void port_workload_view_entry_begin(void) { vgx::workload::BeginViewEntry(); }
void port_workload_view_entry_end(void) { vgx::workload::EndViewEntry(); }
uint32_t port_workload_anim_begin(void) { return vgx::workload::BeginAnim(); }
void port_workload_anim_end(uint32_t ticket) { vgx::workload::EndAnim(ticket); }
uint32_t port_workload_anim_category_begin(unsigned domain, unsigned category) { return vgx::workload::BeginAnimCategory(domain, category); }
void port_workload_anim_category_end(uint32_t ticket) { vgx::workload::EndAnimCategory(ticket); }
uint32_t port_workload_anim_family_begin(unsigned family) { return vgx::workload::BeginAnimFamily(family); }
void port_workload_anim_family_end(uint32_t ticket) { vgx::workload::EndAnimFamily(ticket); }
void port_workload_anim_material_checkpoint(uint32_t ticket, unsigned phase) { vgx::workload::AnimMaterialCheckpoint(ticket, phase); }
uint32_t port_workload_anim_material_diff_context(void) { return vgx::workload::AnimMaterialDiffContext(); }
void port_workload_anim_material_diff_counts(uint32_t ticket, uint32_t scanned, uint32_t flagged, uint32_t calls) {
    vgx::workload::AnimMaterialDiffCounts(ticket, scanned, flagged, calls);
}
#endif

#if PORT_PHASE_CPU_COMPILED
void port_native_phase_cpu_checkpoint(uint32_t phase) {
    port::phasecpu::Checkpoint(phase);
}
#endif

void port_game_progress_checkpoint(uint32_t phase) {
    const bool mainLane = !(phase >= PORT_GAME_PHASE_AUDIO_RESOURCES && phase <= PORT_GAME_PHASE_AUDIO_MESSAGE_WAIT);
    if (mainLane) vgx::workload::FinishWait(phase == PORT_GAME_PHASE_GX_FINISH_WAIT);
#if defined(__vita__) && PORT_PHASE_CPU_AVAILABLE
    if (port::phasecpu::Enabled()) port::phasecpu::Checkpoint(phase);
#endif
    if (!PORT_RUNTIME_DIAGNOSTICS) return;
    if (!phase || phase>=PORT_GAME_PHASE_COUNT) return;
    auto& lane=phase>=PORT_GAME_PHASE_AUDIO_RESOURCES && phase<=PORT_GAME_PHASE_AUDIO_READY ? sAudioProgress :
        phase>=PORT_GAME_PHASE_DSP_BOOT && phase<=PORT_GAME_PHASE_AUDIO_MESSAGE_WAIT ? sDspProgress : sGameProgress;
    // One word makes the phase/generation coherent without a guest lock, clock,
    // allocation, log or foreign-thread read. Unsigned generation wraps.
    publish(lane,phase);
    if (&lane == &sGameProgress && slowTurnLog()) {  // (the main loop's lane: one thread)
        const int64_t now = port_host_time_ns();
        if (sTurn.last && sTurn.phase) sTurn.ns[sTurn.phase] += now - sTurn.last;
        sTurn.last = now;
        sTurn.phase = phase;
    }
}

void port_game_progress_irq_checkpoint(uint32_t irq,uint32_t active) {
    if (!PORT_RUNTIME_DIAGNOSTICS) return;
    if (irq<PORT_IRQ_COUNT && active<=1) publish(sIrqProgress,active ? irq+1 : 0);
}
void port_game_progress_gpu_checkpoint(uint32_t phase) {
    if (!PORT_RUNTIME_DIAGNOSTICS) return;
    if (phase && phase<PORT_GAME_GPU_COUNT) publish(sGpuProgress,phase);
}
void port_game_progress_cpu_checkpoint(uint32_t owner) {
    if (PORT_RUNTIME_DIAGNOSTICS) sCpuOwner.store(owner,std::memory_order_relaxed);
}
const char* port_game_progress_gpu_name(uint32_t phase) {
    return phase<PORT_GAME_GPU_COUNT ? gpuPhaseNames[phase] : gpuPhaseNames[0];
}

int port_game_progress_poll(int64_t now,PortGameProgress* sample) {
    if (!PORT_RUNTIME_DIAGNOSTICS) return 0;
    auto& observer=sProgressObserver;
    if (!sample || now<0 || observer.reports>=4 || (observer.started && now<observer.lastPoll)) return 0;
    const uint32_t word=sGameProgress.load(std::memory_order_relaxed);
    if (!word) return 0;
    if (!observer.started) {
        observer={word,now,now,now,0,true}; return 0;
    }
    observer.lastPoll=now;
    if (word!=observer.word) { observer.word=word; observer.lastChange=now; }
    if (now-observer.lastReport<5000000000ll) return 0;
    const uint32_t audio=sAudioProgress.load(std::memory_order_relaxed),dsp=sDspProgress.load(std::memory_order_relaxed);
    const uint32_t irq=sIrqProgress.load(std::memory_order_relaxed),gpu=sGpuProgress.load(std::memory_order_relaxed);
    *sample={word&63u,word>>6,now-observer.lastChange,audio&63u,audio>>6,dsp&63u,dsp>>6,
        irq&63u,irq>>6,gpu&63u,gpu>>6,sCpuOwner.load(std::memory_order_relaxed)};
    observer.lastReport=now; ++observer.reports;
    return 1;
}

// A stall report: where each lane last was (the main loop's phase, audio, DSP, the interrupt
// being handled, the renderer's last GX call) and who owns the game CPU. Any thread.
void port_game_progress_report(const char* why) {
    if (!PORT_RUNTIME_DIAGNOSTICS) return;
    const uint32_t word = sGameProgress.load(std::memory_order_relaxed);
    const uint32_t audio = sAudioProgress.load(std::memory_order_relaxed), dsp = sDspProgress.load(std::memory_order_relaxed);
    const uint32_t irq = sIrqProgress.load(std::memory_order_relaxed), gpu = sGpuProgress.load(std::memory_order_relaxed);
    port_log("progress (%s): main loop phase %s (#%u), audio %s (#%u), dsp %s (#%u), irq lane %u (#%u), gpu %s (#%u), cpu owner %08x", why,
             port_game_progress_name(word & 63u), word >> 6, port_game_progress_name(audio & 63u), audio >> 6, port_game_progress_name(dsp & 63u), dsp >> 6,
             irq & 63u, irq >> 6, port_game_progress_gpu_name(gpu & 63u), gpu >> 6, sCpuOwner.load(std::memory_order_relaxed));
}

const char* port_game_progress_name(uint32_t phase) {
    return phase<PORT_GAME_PHASE_COUNT ? phaseNames[phase] : phaseNames[0];
}

void port_perf_frame_begin(void) {
    if (port_autoq_game_begin) port_autoq_game_begin(); // target-only CPU sample, also in the quiet player build
    vgx::workload::BeginTurn();
#if PORT_MTX_PROFILE_COMPILED
    port_mtx_profile_work_begin();
#endif
#if PORT_PHASE_CPU_AVAILABLE
    port::phasecpu::Begin();
#endif
    if (!PORT_RUNTIME_DIAGNOSTICS) return;
    std::lock_guard<std::mutex> lock(sLock);
    int64_t now = port_host_time_ns();
    if (!sWindowStart) sWindowStart = now;
    sFrameStart = now;
    sFrameCpuStart = port_thread_cpu_us();
    sPlayerCpuAtTurnStart = port_vgx_player_cpu_us ? port_vgx_player_cpu_us() : 0;
    sRecordThisFrame = 0;
    sTurn.last = 0;
    sTurn.phase = 0;
    sTurn.kind = TurnKind::Unclassified;
    sTurn.bothSkipReasons = false;
    for (int64_t& v : sTurn.ns) v = 0;
}

static void SlipWorkDone();  // (below, with port_frameskip_turn)
static void SteadyWorkDone();

void port_perf_frame_work_done(void) {
    SlipWorkDone();  // GXV_SLIP: the turn's cost sample, taken before the retrace wait; nothing when off
    SteadyWorkDone();  // GXV_STEADY30: the same, for Performance mode's steady 30
#if PORT_PHASE_CPU_AVAILABLE
    port::phasecpu::End();
#endif
    if (port_autoq_game_work_done) port_autoq_game_work_done(); // ends before the retrace wait
    vgx::workload::EndTurn();
#if PORT_MTX_PROFILE_COMPILED
    port_mtx_profile_work_done();
#endif
    if (!PORT_RUNTIME_DIAGNOSTICS) {
        sTotFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::lock_guard<std::mutex> lock(sLock);
    int64_t now = port_host_time_ns();
    const int64_t cpuNow = port_thread_cpu_us();
    if (sFrameStart) {
        const int64_t work = now - sFrameStart;
        if (sTurn.last && sTurn.phase) {
            sTurn.ns[sTurn.phase] += now - sTurn.last;
            sTurn.last = now;
        }
        const bool cpuValid = sFrameCpuStart > 0 && cpuNow >= sFrameCpuStart;
        TurnCost& cost = sTurnCosts[static_cast<unsigned>(sTurn.kind)];
        ++cost.turns;
        cost.bothReasons += sTurn.bothSkipReasons;
        cost.work += work;
        if (cpuValid) { ++cost.cpuSamples; cost.sampledWork += work; cost.cpu += cpuNow - sFrameCpuStart; }
        cost.phaseTurns += sTurn.phase != 0;
        cost.draw += sTurn.ns[PORT_GAME_PHASE_DRAW];
        cost.anim += sTurn.ns[PORT_GAME_PHASE_CALC_ANIM];
        cost.audio += sTurn.ns[PORT_GAME_PHASE_AUDIO_OBJECTS];
        cost.finish += sTurn.ns[PORT_GAME_PHASE_GX_FINISH_WAIT];
        cost.record += sRecordThisFrame;
        if (work >= 45000000 && slowTurnLog() && sSlowTurnsLogged < 400) {
            uint32_t top[3] = {0, 0, 0};
            for (uint32_t i = 1; i < PORT_GAME_PHASE_COUNT; ++i) {
                for (int k = 0; k < 3; ++k) {
                    if (sTurn.ns[i] > sTurn.ns[top[k]]) {
                        for (int m = 2; m > k; --m) top[m] = top[m - 1];
                        top[k] = i;
                        break;
                    }
                }
            }
            ++sSlowTurnsLogged;
            const int64_t plCpu = port_vgx_player_cpu_us ? port_vgx_player_cpu_us() - sPlayerCpuAtTurnStart : 0;
            port_log("slow turn: %.0f ms (GX recording %.0f): %s %.0f, %s %.0f, %s %.0f%s", work / 1e6, sRecordThisFrame / 1e6, phaseNames[top[0]],
                     sTurn.ns[top[0]] / 1e6, phaseNames[top[1]], sTurn.ns[top[1]] / 1e6, phaseNames[top[2]], sTurn.ns[top[2]] / 1e6,
                     (port_vgx_player_cpu_us && sTurn.ns[PORT_GAME_PHASE_GX_FINISH_WAIT] >= 30000000)
                         ? (std::string("; front-end frame-boundary CPU counter advanced ") + std::to_string(plCpu / 1000) + " ms (not turn-scoped; includes idle spin)").c_str()
                         : "");
        }
        sTotWorkNs.fetch_add(work, std::memory_order_relaxed);
        sTotFrames.fetch_add(1, std::memory_order_relaxed);
        if (work > sWinMaxWorkNs.load(std::memory_order_relaxed)) sWinMaxWorkNs.store(work, std::memory_order_relaxed);
        // Zero is also the host CPU-query failure sentinel; never subtract it from lifetime CPU.
        if (cpuValid) sTotCpuUs.fetch_add(cpuNow - sFrameCpuStart, std::memory_order_relaxed);
        sWork.add((now - sFrameStart) / 1e6);
        sRecord.add(sRecordThisFrame / 1e6);
        sFrames++;
        sFrameStart = 0;
    }
    if (now - sWindowStart >= 10000000000ll) {
        report(now);
    }
}

// The game's main thread so far: time between the start of a frame and its wait for the
// retrace, frames, CPU time within those same work spans, and the longest frame since the
// previous call.
void port_perf_game_totals(int64_t* workNs, uint32_t* frames, int64_t* cpuUs, int64_t* maxWorkNs) {
    *workNs = sTotWorkNs.load(std::memory_order_relaxed);
    *frames = sTotFrames.load(std::memory_order_relaxed);
    *cpuUs = sTotCpuUs.load(std::memory_order_relaxed);
    *maxWorkNs = sWinMaxWorkNs.exchange(0, std::memory_order_relaxed);
}

#if !defined(__vita__)
// CPU time the calling thread has used, in microseconds (the Vita's is in vita/runtime/memory.cpp).
int64_t port_thread_cpu_us(void) {
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}
#endif

// Frame skipping (handhelds). The game runs its logic once per loop turn and has no notion
// of a slow frame: when a turn takes two display refreshes, everything moves at half speed.
// With PETARI_FRAMESKIP=N (default 3 on a handheld, 0 = off elsewhere) the frame loop asks
// here at the start of each turn whether to run its logic without drawing (0 no, 1 yes: the
// turn then takes its retrace like a drawn one, 2 yes: late, no retrace wait). Called by
// GameSystem::frameLoop on the game's main thread.
//   1. The turn starts more than half a refresh behind that clock (the game thread is the slow
//      part): skip, at most N turns in a row, to catch up.
//   2. The back end already has PETARI_PIPE_DEPTH (default 2, 0 = off) pictures it has not
//      finished (the GPU is the slow part): skip, at most PETARI_PIPE_RUN (default 6) in a row.
//      A picture sent now would only wait in the queue, adding to the delay between a button and
//      what it does on screen, and a turn that blocked on the queue would cost game speed; the
//      pictures that are sent come at the rate the GPU can draw them and the game keeps its pace.
// True while a turn runs without a picture (set by GameSystem::frameLoop around update() and calcAnim()): the game then leaves out
// what only the picture needs (the view and draw-list pass of GameScene::calcAnim). OFF by default: PETARI_SKIP_VIEWCALC=1 turns it on
// (forkB, 6 Nov: the Vita build sets it, vita/vgx-game/entry.cpp, with the player's pass kept every turn: PETARI_VIEW_PLAYER).
// The note below is from before PETARI_VIEW_PLAYER:
// Measured in the harness (festival plaza, 2 of every 3 turns skipped): 0.085 M instructions per skipped turn, 3 % of its cost,
// because the turn's work is almost all update() and calcAnim() of the objects; and MarioActor::calcViewAndEntry also runs
// LiveActor::calcAnim, decideShadowMode and the lighting, so leaving it out risks stale joint matrices for little gain.
// Camera zoom (handhelds). The game was framed for a television seen from across a room; on a 5-inch screen held at arm's
// length everything is small. The zoom Z (1.0-2.0) narrows the field of view of every perspective camera so the picture is Z
// times closer: tan(fovy/2) is divided by Z. CameraContext applies it where the game's field of view is set, so everything
// that reads the field of view (object clipping, the star pointer, particles, the blur effect) sees the same view as the
// projection does, and the narrower frustum also culls more.
//
// A user setting on a handheld: in the game's pause menu L lowers it and R raises it by 0.1 (platform/src/port/settings_panel.cpp draws the value under the menu,
// vita/runtime/input.cpp reads the buttons), and the choice is kept in <data folder>/settings.ini ("zoom = 1.5"; the folder
// that holds the saves' folder: ux0:data/smg). Where it comes from, in this order: that file; PETARI_ZOOM=Z in env.txt (the
// start value until a choice is made in the menu); 1.5 on a handheld (the maintainer: 1.15 at first, then "maybe 1.3", then on 17 Oct
// 17:11 "can we try 1.5", and at 17:15 "can that be a user scale ... 2.0 down to 1"), 1.0 elsewhere. It is held in tenths.
// A change shows when the game resumes (the camera is not updated while paused): CameraContext eases to it in 0.4 s.
namespace {
constexpr int kZoomMin = 10, kZoomMax = 20;
std::atomic<int> sZoomTenths{-1};   // -1: not read yet
std::atomic<unsigned> sZoomChange{0};  // counts the changes made in the menu
unsigned sZoomSavedChange = 0;         // (the count the file holds; touched by the game thread only)
unsigned sZoomIdle = 0;                // calls of port_zoom_menu since the last change
std::atomic<int> sZoomMenuSelecting{0};
std::string sZoomFile;                 // "" = no settings file (not a handheld, or no folder given)

int ZoomClamp(int t) { return t < kZoomMin ? kZoomMin : (t > kZoomMax ? kZoomMax : t); }
int ZoomTenths(double v) { return ZoomClamp(static_cast<int>(v * 10.0 + (v < 0 ? -0.5 : 0.5))); }

// "zoom = 1.5" in the file, or -1.
int ZoomFromFile() {
    if (sZoomFile.empty()) return -1;
    FILE* f = fopen(sZoomFile.c_str(), "r");
    if (!f) return -1;
    int t = -1;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (strncmp(p, "zoom", 4) != 0) continue;
        p += 4;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '=') continue;
        ++p;
        char* end = nullptr;
        const double v = strtod(p, &end);
        if (end != p && v >= 0.5 && v <= 4.0) t = ZoomTenths(v);
    }
    fclose(f);
    return t;
}

// "rear_aim = 1" in the same file: the rear touch pad aims the pointer; "stick_pointer = 1": the right stick moves the pointer, not the
// camera (vita/runtime/input.cpp). Off unless chosen: -1 = not read.
std::atomic<int> sRearAim{-1};
std::atomic<int> sStickPointer{-1};
int OptionFromFile(const char* key) {
    if (sZoomFile.empty()) return 0;
    FILE* f = fopen(sZoomFile.c_str(), "r");
    if (!f) return 0;
    int on = 0;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        const size_t keyLength = strlen(key);
        if (strncmp(p, key, keyLength) != 0) continue;
        p += keyLength;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '=') continue;
        ++p;
        while (*p == ' ' || *p == '\t') ++p;
        on = (*p == '1') ? 1 : 0;
    }
    fclose(f);
    return on;
}
int RearAimRead() {
    int v = sRearAim.load(std::memory_order_relaxed);
    if (v >= 0) return v;
    v = OptionFromFile("rear_aim");
    int expected = -1;
    if (sRearAim.compare_exchange_strong(expected, v)) port_log_summary("rear touch aiming: %s", v ? "on (from the settings file)" : "off");
    return sRearAim.load();
}
int StickPointerRead() {
    int v = sStickPointer.load(std::memory_order_relaxed);
    if (v >= 0) return v;
    v = OptionFromFile("stick_pointer");
    int expected = -1;
    if (sStickPointer.compare_exchange_strong(expected, v)) port_log_summary("right stick: %s", v ? "pointer (from the settings file)" : "camera");
    return sStickPointer.load();
}

// "star_bit_reach = 1" to 5 in the same file (pause menu: Select steps it): 1 is the original game; 2, 3, 4 widen the screen radius the
// pointer collects Star Bits in (1.5x, 2x, 3x, at any depth); 5 collects every Star Bit on screen without aiming
// (MR::isStarPointerPointingStarPieceAssist). From Producdevity's fork, 77119d3. -1 = not read. PETARI_STAR_BIT_REACH=1..5 (a test
// switch) forces a level whatever the file says.
std::atomic<int> sStarBitReach{-1};
constexpr int kStarBitReachLevels = 5;
int StarBitReachFromFile() {
    if (sZoomFile.empty()) return 1;
    FILE* f = fopen(sZoomFile.c_str(), "r");
    if (!f) return 1;
    int level = 1;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (strncmp(p, "star_bit_reach", 14) != 0) continue;
        p += 14;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '=') continue;
        ++p;
        while (*p == ' ' || *p == '\t') ++p;
        level = (*p >= '1' && *p <= '0' + kStarBitReachLevels) ? *p - '0' : 1;
    }
    fclose(f);
    return level;
}
int StarBitReachRead() {
    int v = sStarBitReach.load(std::memory_order_relaxed);
    if (v >= 0) return v;
    const char* force = getenv("PETARI_STAR_BIT_REACH");
    const bool forced = force && *force >= '1' && *force <= '0' + kStarBitReachLevels && force[1] == 0;
    v = forced ? *force - '0' : StarBitReachFromFile();
    int expected = -1;
    if (sStarBitReach.compare_exchange_strong(expected, v))
        port_log_summary("Star Bit reach: %d (%s)", v, forced ? "PETARI_STAR_BIT_REACH" : (v > 1 ? "from the settings file" : "the game's own"));
    return sStarBitReach.load();
}

void ZoomSave() {
    if (sZoomFile.empty()) return;
    const std::string tmp = sZoomFile + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    const int t = sZoomTenths.load();
    fprintf(f, "# Settings of the Vita port, changed in the game's pause menu (L and R: zoom; Triangle: rear touch aiming; Square: right stick;\n"
               "# Select: Star Bit reach, 1 the game's own to 5 every Star Bit on screen).\n"
               "zoom = %d.%d\nrear_aim = %d\nstick_pointer = %d\nstar_bit_reach = %d\n",
            t / 10, t % 10, RearAimRead(), StickPointerRead(), StarBitReachRead());
    const bool ok = fclose(f) == 0;
    if (ok) {
        remove(sZoomFile.c_str());  // (a rename onto an existing file fails on some of the console's file systems)
        if (rename(tmp.c_str(), sZoomFile.c_str()) != 0) {
            port_log_summary("zoom: the settings file %s could not be written", sZoomFile.c_str());
            return;
        }
    }
    sZoomSavedChange = sZoomChange.load();
    port_log_summary("zoom: %d.%d, rear touch aiming %s, right stick %s, Star Bit reach %d, saved in %s", t / 10, t % 10,
                     RearAimRead() ? "on" : "off", StickPointerRead() ? "pointer" : "camera", StarBitReachRead(), sZoomFile.c_str());
}

int ZoomRead() {
    int t = sZoomTenths.load(std::memory_order_relaxed);
    if (t >= 0) return t;
    // PETARI_ZOOM_FORCE=Z (a test switch): this zoom whatever the settings file says (the file is left as it is)
    if (const char* force = getenv("PETARI_ZOOM_FORCE"); force && *force) {
        t = ZoomTenths(atof(force));
        int expected = -1;
        if (sZoomTenths.compare_exchange_strong(expected, t)) port_log_summary("zoom: %d.%d (PETARI_ZOOM_FORCE)", t / 10, t % 10);
        return sZoomTenths.load();
    }
    t = ZoomFromFile();
    const bool fromFile = t >= 0;
    if (t < 0) {
        const char* e = getenv("PETARI_ZOOM");
        t = (e && *e) ? ZoomTenths(atof(e)) : (port_handheld() ? 15 : 10);
    }
    int expected = -1;
    if (sZoomTenths.compare_exchange_strong(expected, t)) {
        port_log_summary("zoom: %d.%d (%s)", t / 10, t % 10, fromFile ? "from the settings file" : "the default");
    }
    return sZoomTenths.load();
}
}  // namespace

// Called once by port_boot with the folder of the saves: the settings file is next to it.
extern "C" void port_zoom_set_root(const char* saveRoot) {
    sZoomFile.clear();
    if (!port_handheld() || !saveRoot || !*saveRoot) return;
    std::string r = saveRoot;
    while (r.size() > 1 && (r.back() == '/' || r.back() == '\\')) r.pop_back();
    const size_t cut = r.find_last_of("/\\");
    if (cut == std::string::npos) return;
    sZoomFile = r.substr(0, cut + 1) + "settings.ini";
    sZoomTenths.store(-1);  // (read again, with the file)
    sRearAim.store(-1);
    sStickPointer.store(-1);
    sStarBitReach.store(-1);
}

// Rear touch aiming (pause menu: Triangle toggles it; saved with the zoom). 1 = on.
extern "C" int port_rear_aim_enabled(void) { return port_handheld() ? RearAimRead() : 0; }
extern "C" int port_rear_aim_toggle(void) {
    const int now = RearAimRead() ? 0 : 1;
    sRearAim.store(now);
    sZoomChange.fetch_add(1);
    return now;
}

// The right stick (pause menu: Square switches it; saved with the zoom). 1 = it moves the pointer, 0 = it turns the camera.
extern "C" int port_stick_pointer_enabled(void) { return port_handheld() ? StickPointerRead() : 0; }
extern "C" int port_stick_pointer_toggle(void) {
    const int now = StickPointerRead() ? 0 : 1;
    sStickPointer.store(now);
    sZoomChange.fetch_add(1);
    return now;
}

// Star Bit reach (pause menu: Select steps through the levels; saved with the zoom). 1 = the original game; elsewhere than the handhelds
// always 1.
extern "C" int port_star_bit_reach(void) { return port_handheld() ? StarBitReachRead() : 1; }
extern "C" int port_star_bit_reach_next(void) {
    const int now = StarBitReachRead() % kStarBitReachLevels + 1;
    sStarBitReach.store(now);
    sZoomChange.fetch_add(1);
    return now;
}

extern "C" float port_camera_zoom(void) { return static_cast<float>(ZoomRead()) / 10.0f; }

// The menu: tenths of the zoom now (10..20).
extern "C" int port_zoom_tenths(void) { return ZoomRead(); }

// L / R in the pause menu (input thread): steps of 0.1. Returns the new value. A step past 1.0 or 2.0 counts as a limit hit
// (port_zoom_limit_hits: the settings panel plays the game's "can't" sound for it).
static std::atomic<unsigned> sZoomLimitHits{0};
extern "C" int port_zoom_adjust(int steps) {
    const int was = ZoomRead();
    const int now = ZoomClamp(was + steps);
    if (now != was) {
        sZoomTenths.store(now);
        sZoomChange.fetch_add(1);
    } else if (steps != 0) {
        sZoomLimitHits.fetch_add(1, std::memory_order_relaxed);
    }
    return now;
}
extern "C" unsigned port_zoom_limit_hits(void) { return sZoomLimitHits.load(std::memory_order_relaxed); }

// The settings panel under the pause menu (platform/src/port/settings_panel.cpp): its sounds and animation step once per turn of
// the game thread. (weak: the host tests build this file without it)
extern "C" void port_settings_panel_turn(int selecting) __attribute__((weak));

// The pause menu (game thread, once per turn): `selecting` is 1 while the buttons take input. The value is written to the file
// when the menu closes, and after three quarters of a second without a change, whichever comes first.
extern "C" void port_zoom_menu(int selecting) {
    const int was = sZoomMenuSelecting.exchange(selecting ? 1 : 0);
    const unsigned changes = sZoomChange.load();
    static unsigned lastChange = 0;
    if (changes != lastChange) {
        lastChange = changes;
        sZoomIdle = 0;
    } else if (sZoomIdle < 100000) {
        ++sZoomIdle;
    }
    if (changes != sZoomSavedChange && ((was && !selecting) || sZoomIdle >= 45)) ZoomSave();
    if (port_settings_panel_turn) port_settings_panel_turn(selecting);
#if VGX_COLDTRACE
    port_vgx_coldtrace_pause(selecting);
    if (port_vgx_coldtrace_export_pending()) return;
#endif
    if (PORT_RUNTIME_DIAGNOSTICS && port_vgx_diagnostic_csv_pause) port_vgx_diagnostic_csv_pause(selecting);
}
#if VGX_COLDTRACE
static_assert(PORT_VGX_COLDTRACE_CONTROL_VERSION==1);
extern "C" void port_vgx_coldtrace_pause_runtime_link_guard_v1() {}
#endif

// 1 while the pause menu waits for a choice and the zoom can be set with L and R.
extern "C" int port_zoom_menu_active(void) { return sZoomMenuSelecting.load(std::memory_order_relaxed) && port_handheld() ? 1 : 0; }

// The renderer's picture-size generation: the back end counts every change of its render size (vgx_backend_gl.cpp ApplyRung,
// the automatic quality's rungs, on the render thread); the game reads it (any thread). PETARI_PAUSE_FREEZE (GameScene.cpp)
// copies the 3D picture behind the pause menu again when it changes, so the copy always has the picture's size.
static std::atomic<uint32_t> sRenderSizeGen{0};
extern "C" void port_render_size_changed(void) { sRenderSizeGen.fetch_add(1, std::memory_order_relaxed); }
extern "C" uint32_t port_render_size_gen(void) { return sRenderSizeGen.load(std::memory_order_relaxed); }

static std::atomic<int> sTurnSkipped{0};
extern "C" void port_turn_skipped_set(int v) { sTurnSkipped.store(v, std::memory_order_relaxed); }
// The scene's key for the renderer's automatic quality (each stage remembers its own rung, vgx_autoq.h calm mode): a hash of
// the stage's name, refreshed on the game thread every turn of a game scene (GameScene::calcAnim asks port_turn_skipped),
// read by the back end. 0 = not known yet.
extern "C++" {  // (this part of the file is inside an extern "C" block)
namespace MR {
const char* getCurrentStageName() __attribute__((weak));  // (weak: the host tests build this file without the game)
}
}
static std::atomic<uint32_t> sSceneKey{0};
extern "C" uint32_t port_scene_key(void) { return sSceneKey.load(std::memory_order_relaxed); }
static uint32_t StageKeyOf(const char* name) {
    uint32_t h = 2166136261u;  // FNV-1a
    for (const char* c = name; c && *c; ++c) h = (h ^ static_cast<unsigned char>(*c)) * 16777619u;
    return name && *name ? h : 0;
}
static void SceneKeyRefresh() {
    if (!&MR::getCurrentStageName) return;
    sSceneKey.store(StageKeyOf(MR::getCurrentStageName()), std::memory_order_relaxed);
}
// The stage the game is loading (GXV_STAGEWARM in platform/vgx/src/vgx_backend_gl.cpp): GameSystemSceneController::initializeScene
// calls these on its loading thread, begin as soon as it knows the next stage (updateSceneControlInfo), end when the scene is built.
// The key has the form of port_scene_key and stays after the load (the same stage is then being played); 0 before the first load
// and for scenes without a stage. The renderer prepares that stage's programs while the screen is closed for the load.
static std::atomic<uint32_t> sLoadingKey{0};
static std::atomic<int> sLoadingActive{0};
extern "C" void port_scene_load_begin(const char* stage) {
    sLoadingKey.store(StageKeyOf(stage), std::memory_order_relaxed);
    sLoadingActive.store(1, std::memory_order_release);
}
extern "C" void port_scene_load_end(void) { sLoadingActive.store(0, std::memory_order_release); }
extern "C" uint32_t port_scene_loading_key(void) { return sLoadingKey.load(std::memory_order_relaxed); }
extern "C" int port_scene_loading_active(void) { return sLoadingActive.load(std::memory_order_acquire); }
// The raw flag (GXV_DL_LAZY, decomp/src/Game/LiveActor/ModelManager.cpp): materials of a turn without a picture are computed at
// the next turn that draws. GXV_DL_LAZY=0 turns it off (read once; default 1).
extern "C" int port_turn_skipped_raw(void) { return sTurnSkipped.load(std::memory_order_relaxed); }
// GXV_DL_LAZY=2 (the default): EVERY turn leaves the materials to the start of the next turn that draws, not only turns without a
// picture: a drawn turn's own materials were thrown away whenever the turn after it drew nothing (every other turn at 30 FPS), and the
// next drawn turn computes them from the same state anyway. 1: only turns without a picture (1.3 first candidate). 0: off.
extern "C" int port_dl_lazy_on(void) {
    return PORT_HOT_KNOB(int, [] {  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
        const char* e = getenv("GXV_DL_LAZY");
        if (e && *e == '0') return 0;
        if (e && *e == '1') return 1;
        return 2;
    }());
}
// GXV_ANIM_LOD (decomp/src/Game/LiveActor/LiveActor.cpp): the calc-anim category the game thread is executing (set by
// CategoryList::execute), and whether far models of it may compute their joints at the next drawn turn instead. Read once.
// (game3, 5 Nov) CategoryList::execute(MovementType) sets 0x100 | type (GXV_ANIM_PROF's movement rows): no calc-anim category
// matches it, so a model computed during a movement category is treated as before (never deferred).
static int sCalcAnimCategory = -1;
static int MoveProfLevel();             // (hivelogic: GXV_ANIM_PROF's category timers, at the end of this file)
static void MoveProfMark(int type);
extern "C" void port_anim_lod_set_category(int type) {
    sCalcAnimCategory = type;
    if (__builtin_expect(MoveProfLevel() != 0, 0)) MoveProfMark(type);
}
extern "C" int port_anim_lod_raw_category(void) { return sCalcAnimCategory; }
extern "C" int port_anim_lod_category(void) {
    // MR::CalcAnimType: 4 Environment, 5 MapObj, 6 NPC, 0x0B MapObjDecoration. Not collision objects, enemies, items, the player.
    switch (sCalcAnimCategory) {
    case 4: case 5: case 6: case 0x0B: return 1;
    default: return 0;
    }
}
// GXV_MTX_NOTIFY=1: the view pass's matrix buffers send the write notification of a data-cache store again (J3DModel.cpp). Default 0.
extern "C" int port_mtx_notify_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_MTX_NOTIFY"); return e && *e == '1' ? 1 : 0; }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
extern "C" float port_anim_lod_dist2(void) {
    return PORT_HOT_KNOB(float, [] {  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
        // (On by default again since 5 Nov: the deferral used to stop far actors' animation clocks, so their frame-synced one-time
        // effects fired every turn (lit Observatory: 545 effect draws a picture, 10 FPS); LiveActor.cpp keeps the clock running now
        // (GXV_ANIM_LOD_XLATCH). Console, zoom 1.0 lit Observatory: 27.5 FPS on against 24.0 off. GXV_ANIM_LOD=0: off.)
        const char* on = getenv("GXV_ANIM_LOD");
        if (on && *on == '0') return 0.0f;
        const char* e = getenv("GXV_ANIM_LOD_DIST");
        float d = e && *e ? static_cast<float>(atof(e)) : 3000.0f;
        if (!(d >= 500.0f)) d = 3000.0f;
        return d * d;
    }());
}
// GXV_ANIM_LOD_PDIST (game3, 5 Nov; default: the value of GXV_ANIM_LOD_DIST): the distance from the PLAYER beyond which a model may
// be deferred, apart from the camera's (GXV_ANIM_LOD_DIST). The player's distance is what keeps the game's logic as it was (sensors,
// talk ranges and attachments meet the player only near him); the camera's only decides how visible the lag of an effect or a
// shadow bound to a joint would be between two pictures. Read once; at least 500.
extern "C" float port_anim_lod_pdist2(void) {
    return PORT_HOT_KNOB(float, [] {  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
        const char* e = getenv("GXV_ANIM_LOD_PDIST");
        if (!(e && *e)) return port_anim_lod_dist2();
        float d = static_cast<float>(atof(e));
        if (!(d >= 500.0f)) d = 500.0f;
        return d * d;
    }());
}
// GXV_ANIM_LOD_OFFSCREEN (game3, 5 Nov; default 1): a model nearer the camera than GXV_ANIM_LOD_DIST is deferred like a far one when
// all of it is outside the view (the clipping frustum, with a margin) and it is as far from the player as GXV_ANIM_LOD_PDIST:
// nothing of it can be seen, and the next picture still computes it first (port_anim_flush_stale). 0: off.
extern "C" int port_anim_lod_offscreen(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_ANIM_LOD_OFFSCREEN"); return e && *e == '0' ? 0 : 1; }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
// GXV_ANIM_LOD_FREE (cpuhive, 6 Nov; default 1; decomp/src/Game/LiveActor/LiveActor.cpp AnimLodDefers): a model whose class marks
// its joints as read by nothing but the picture (LiveActor::mLogicFreeJoints: PlantMember, the flower groups' flowers and bushes) is
// computed once per picture by the drawn turn's flush, at any distance, instead of every turn. Exact (picture and game state). 0: off
// (only far models are deferred, as before).
extern "C" int port_anim_lod_free_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_ANIM_LOD_FREE"); return e && *e == '0' ? 0 : 1; }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
// GXV_ANIM_MEMO (game3, 5 Nov; default 1; decomp/src/Game/LiveActor/ModelManager.cpp): a model whose joint inputs (base matrix,
// scale, animation tracks, frames and weights) are those of its last computation, and whose joint matrices nobody changed since,
// keeps them instead of computing the same values again. Exact. Never for the player's categories (Mario's animators share joint
// state between their players). 0: off.
extern "C" int port_anim_memo_on(void) {
    const int on = PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_ANIM_MEMO"); return e && *e == '0' ? 0 : 1; }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
    if (!on) return 0;
    const int c = sCalcAnimCategory & 0xFF;  // (MR::CalcAnimType 9 Player, 0x0A PlayerDecoration; MovementType 0x25, 0x26)
    if (sCalcAnimCategory >= 0x100) return c == 0x25 || c == 0x26 ? 0 : 1;
    return sCalcAnimCategory == 9 || sCalcAnimCategory == 0x0A ? 0 : 1;
}
// GXV_LIVE_SNAP (draw7, 5 Nov; default 1; decomp/src/Game/LiveActor/LiveActor.cpp port_live_actor_snapshot): the sorted copy of the
// scene's actors that the drawn turn's flushes check their lists against is kept while the group's entries are unchanged. Exact. 0: off.
extern "C" int port_live_snap_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_LIVE_SNAP"); return e && *e == '0' ? 0 : 1; }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
// GXV_VIEW_KEEP (draw7, 5 Nov; default 0; decomp/src/Game/LiveActor/ModelManager.cpp PortViewKeepCalc): a model whose view pass
// inputs (the camera's view matrix, its base matrix and scale, its joint and envelope matrices and scale flags) are those of its last
// view pass, and whose current draw and normal matrices nobody touched since (J3DMtxBuffer's write serial), keeps them: no swap, no
// computation; the front end reads the same, unchanged half again. Exact. Pays when the camera holds still. 1: on.
extern "C" int port_view_keep_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_VIEW_KEEP"); return e && *e == '1' ? 1 : 0; }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
// GXV_PAR_VIEW (par13, 5 Nov; default 0; decomp/src/Game/LiveActor/ModelManager.cpp, platform/include/port/par_exec.h): the GameScene
// view pass's matrix arithmetic (draw, normal and billboard matrices of every plain J3DModel) is recorded during the pass and computed
// at its end by the game thread and a worker thread on another core; the game thread returns when every job is done. Exact (same
// code, same inputs; see ModelManager.cpp). 1: on.
// 2 (drawpar, 6 Nov): the drawn turn's view pass (port_view_flush, armed by GameSystem::frameLoop) has no join: its jobs are finished
// by a worker while the game thread draws, and each is made complete before anything reads it (J3DSys::setModelDrawMtx /
// setModelNrmMtx, NormalMapBase, J3DModel::calc / viewCalc) and in any case after draw() (port_par_view_retire). Exact by the same
// argument (see ModelManager.cpp). The other view passes (a turn without the deferred pass, the fast-forward) run as with 1. With it:
//   GXV_PAR_CORE=1|2|3    the worker's core (default 1 for mode 1: the back end's, about half idle in the lit Observatory; default 3
//                         for mode 2: the fourth core when it is proven (CapUnlocker), else core 1; 2 is the front end's)
//   GXV_PAR_SPLIT=0-99    0 (default): both threads take jobs from one counter; p: the worker starts on the first p % of the jobs,
//                         the game thread on the rest, and each takes from the other's part when its own is done
//   GXV_PAR_PRIO=0xNN     the worker's priority (default: one step above the game thread's, so above the back end's: see par_exec.h)
//   GXV_PAR_SPIN_US=40    how long the join spins before it sleeps
//   GXV_PAR_MIN=16        a pass with fewer jobs runs on the game thread alone (a wake costs more than it would save)
//   GXV_PAR_VIEW_MAX=2048 jobs per pass (more: computed at once, as before)
//   GXV_PAR_VERIFY=1      (diagnostic, costs a second computation of every job) each job is also computed at its place in the pass
//                         into scratch memory, and the result after the join is compared with it: "par-view: verify" counts mismatches
// GXV_PAR_ANIM: the joint computation's own switch (joint15), below port_par_run_batch.
// Every 5 s (basic log): "par-view: ..." with the batches, the jobs done by each side, the worker's time, its wake-up and the join waits;
// with 2, "par-view async: ..." with the jobs a picture, who computed them (the worker, the draw's touches, the retire), the touches
// that waited for the worker, and the retire's time.
// (the executor: platform/include/port/par_exec.h, included at the top of this file)
namespace {
int ParEnvInt(const char* name, int fallback, int lo, int hi) {
    const char* e = getenv(name);
    int v = e && *e ? static_cast<int>(strtol(e, nullptr, 0)) : fallback;
    return v < lo ? lo : (v > hi ? hi : v);
}
port::par::Executor& sParExec = *new port::par::Executor;  // (never destroyed: its worker waits on it until the process ends)
struct ParWindow {
    uint64_t t0 = 0;
    uint32_t batches = 0, parallel = 0, cancelled = 0, slept = 0, jobs = 0, byGame = 0, byWorker = 0, inlineRuns = 0, overflow = 0;
    uint64_t workerUs = 0, wakeUs = 0, joinUs = 0, wallUs = 0;
    uint32_t wakeMax = 0, joinMax = 0, wallMax = 0;
} sParWin;
}  // namespace
extern "C" int port_par_view_on(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_VIEW", 0, 0, 2));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
extern "C" unsigned port_par_view_max(void) {
    return PORT_HOT_KNOB(unsigned, ParEnvInt("GXV_PAR_VIEW_MAX", 2048, 64, 16384));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
extern "C" int port_par_verify_on(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_VERIFY", 0, 0, 1));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
// Runs a recorded batch (n items of fn) on the game thread and the worker; returns when all are done. inlineRuns / overflow: the
// jobs the walk itself already ran (a touch, a second job of the same buffer) or computed at once (the batch was full), for the log.
extern "C" void port_par_run_batch(void (*fn)(void*, unsigned), void* ctx, unsigned n, unsigned inlineRuns, unsigned overflow) {
    static const int core = ParEnvInt("GXV_PAR_CORE", 1, 1, 3);
    static const int split = ParEnvInt("GXV_PAR_SPLIT", 0, 0, 99);
    static const int minJobs = ParEnvInt("GXV_PAR_MIN", 16, 2, 100000);
    static const bool started = [] {
        const bool ok = sParExec.Start(core, ParEnvInt("GXV_PAR_PRIO", 0, 0, 0xFF), ParEnvInt("GXV_PAR_SPIN_US", 40, 0, 2000));
        port_log_summary("par-view: %s (GXV_PAR_VIEW=1; core %d, priority 0x%X, split %d, at least %d jobs a pass)",
                         ok ? "worker started" : "the worker could not be started: every pass runs on the game thread", sParExec.Core(),
                         static_cast<unsigned>(sParExec.Priority()), split, minJobs);
        return ok;
    }();
    const uint64_t t0 = port::par::NowUs();
    const port::par::BatchStats st =
        sParExec.Run(reinterpret_cast<port::par::ItemFn>(fn), ctx, n, started && n >= static_cast<unsigned>(minJobs), split);
    const uint64_t t1 = port::par::NowUs();
    ParWindow& w = sParWin;
    if (w.t0 == 0) w.t0 = t0;
    ++w.batches;
    w.parallel += st.parallel;
    w.cancelled += st.cancelled;
    w.slept += st.slept;
    w.jobs += n;
    w.byGame += st.byGame;
    w.byWorker += st.byWorker;
    w.inlineRuns += inlineRuns;
    w.overflow += overflow;
    w.workerUs += st.workerUs;
    w.wakeUs += st.wakeUs;
    w.joinUs += st.joinUs;
    const uint32_t wall = static_cast<uint32_t>(t1 - t0);
    w.wallUs += wall;
    if (st.wakeUs > w.wakeMax) w.wakeMax = st.wakeUs;
    if (st.joinUs > w.joinMax) w.joinMax = st.joinUs;
    if (wall > w.wallMax) w.wallMax = wall;
    if (t1 - w.t0 >= 5000000u) {
        const double b = w.batches ? static_cast<double>(w.batches) : 1.0;
        const double woke = (w.parallel > w.cancelled) ? static_cast<double>(w.parallel - w.cancelled) : 1.0;
        port_log_summary("par-view: %.1f s, %u passes (%u with the worker, %u cancelled before it woke, %u joins slept); jobs a pass %.1f: "
                         "game %.1f, worker %.1f, run in the walk %.2f, computed at once %.2f; a pass: run %.3f ms (max %.3f), worker %.3f ms, "
                         "join wait %.3f ms (max %.3f); worker wake-up %.0f us (max %u)",
                         (t1 - w.t0) / 1e6, w.batches, w.parallel, w.cancelled, w.slept, w.jobs / b, w.byGame / b, w.byWorker / b,
                         w.inlineRuns / b, w.overflow / b, w.wallUs / b / 1000.0, w.wallMax / 1000.0, w.workerUs / b / 1000.0,
                         w.joinUs / b / 1000.0, w.joinMax / 1000.0, w.wakeUs / woke, w.wakeMax);
        w = ParWindow{};
        w.t0 = t1;
    }
}
// GXV_PAR_ANIM (joint15, 5 Nov; default 0; decomp/src/Game/LiveActor/ModelManager.cpp, decomp/include/Game/Animation/XanimeSpecWalk.hpp,
// platform/include/port/par_spec.h): at the end of calc-anim, the next joint computation of every model that qualifies (GXV_ANIM_MEMO's
// rules: no callbacks, deformers, skinning, outside matrices or blends; not Mario) is predicted (its key: base matrix, scale, BCKs,
// frames, weights) and computed on a worker thread on another core into memory of its own, while the game thread draws and runs the
// next turn's movement. At the model's real calc the game thread compares the real key with the predicted one, word for word: equal
// (and the worker done) -> the result is copied in with the bookkeeping J3DModel::calc leaves; otherwise it computes as before. Exact.
//   GXV_PAR_ANIM=1            on (default 0)
//   GXV_PAR_ANIM_VERIFY=1     (diagnostic) a hit runs the original too and compares: "verify" counts in the line, MISMATCH lines
//   GXV_PAR_ANIM_CORE=1|2|3   the worker's core (default 1: the back end's, about half idle in the lit Observatory; 3: the fourth
//                             core when it is proven, else 1)
//   GXV_PAR_ANIM_PRIO=0xNN    the worker's priority (default: one step below the game thread's, so below the back end's: it only
//                             takes idle time; nothing waits for it)
//   GXV_PAR_ANIM_WAIT_US=300  how long the game thread waits for a job the worker is in the middle of before it computes it itself
//   GXV_PAR_ANIM_MAX=512      jobs a batch (models a turn)
//   GXV_PAR_ANIM_KB=1024      the worker's scratch store (joints, envelopes, core state of a batch)
// Every 5 s (basic log): "par-anim: ..." with the jobs, hits, misses, the worker's time and the game thread's time saved.
#include "port/par_spec.h"
#include "port/par_async.h"  // (GXV_PAR_VIEW=2: the job states, checked against par_spec.h's below)
namespace {
port::par::SpecWorker& sParSpec = *new port::par::SpecWorker;  // (never destroyed: its worker waits on it until the process ends)
struct ParAnimWindow {
    uint64_t t0 = 0;
    uint32_t batches = 0;
    uint64_t c[11] = {};  // (ModelManager.cpp's order: spec, hit, miss, not ready, busy, saved us, hit us, dispatch us, verified, verify bad,
                          // batches not made because the worker was still in the last one)
} sParAnimWin;
}  // namespace
extern "C" int port_par_anim_switch(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_ANIM", 0, 0, 1));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
extern "C" int port_par_anim_on(void) {
    if (!port_par_anim_switch()) return 0;
    const int c = sCalcAnimCategory & 0xFF;  // (as GXV_ANIM_MEMO: never the player's categories)
    if (sCalcAnimCategory >= 0x100) return c == 0x25 || c == 0x26 ? 0 : 1;
    return sCalcAnimCategory == 9 || sCalcAnimCategory == 0x0A ? 0 : 1;
}
// GXV_PAR_ANIM_LEAN (cpuhive, 6 Nov; default 1): the batch is made only for models whose jobs get used. A model whose last two jobs
// were never consulted (its next computation was kept by GXV_ANIM_MEMO, deferred by GXV_ANIM_LOD, or it was clipped) is left out for 8
// batches; one that does not qualify when its job is made (a blend in progress, no BCK, a tree the walk cannot take) for 32. Exact
// (which models are speculated on changes only who computes them). Saves the game thread's dispatch time (Honeyhive, 6 Nov: dispatch
// 0.14-0.30 ms a batch against 0.04-0.25 ms saved). 0: every listed model every batch, as before.
extern "C" int port_par_anim_lean_on(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_ANIM_LEAN", 1, 0, 1));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
extern "C" int port_par_anim_verify_on(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_ANIM_VERIFY", 0, 0, 1));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
extern "C" unsigned port_par_anim_max(void) {
    return PORT_HOT_KNOB(unsigned, ParEnvInt("GXV_PAR_ANIM_MAX", 512, 16, 4096));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
extern "C" unsigned port_par_anim_kb(void) {
    return PORT_HOT_KNOB(unsigned, ParEnvInt("GXV_PAR_ANIM_KB", 1024, 128, 16384));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
// Hands a batch to the worker (started at the first one); returns at once. 0: no worker (the jobs stay pending: every model is
// computed as before).
extern "C" int port_par_anim_run(void (*fn)(void*, unsigned), void* ctx, void* items, unsigned n) {
    static const bool started = [] {
        const int core = ParEnvInt("GXV_PAR_ANIM_CORE", 1, 1, 3);  // (3: the fourth core when proven, else 1)
        const bool ok = sParSpec.Start(core, ParEnvInt("GXV_PAR_ANIM_PRIO", 0, 0, 0xFF));
        port_log_summary("par-anim: %s (GXV_PAR_ANIM=1%s; core %d, priority 0x%X, at most %u jobs a batch, %u KiB scratch)",
                         ok ? "worker started" : "the worker could not be started: every model is computed as before",
                         port_par_anim_verify_on() ? ", VERIFY" : "", sParSpec.Core(), static_cast<unsigned>(sParSpec.Priority()), port_par_anim_max(),
                         port_par_anim_kb());
        return ok;
    }();
    if (!started) return 0;
    sParSpec.Dispatch(reinterpret_cast<port::par::ItemFn>(fn), ctx, static_cast<std::atomic<uint8_t>*>(items), n);
    return 1;
}
// The open batch ends: 1 at once unless the worker is in the middle of a job (0: it stops after it; try again at the next turn).
extern "C" int port_par_anim_try_quiesce(void) { return sParSpec.TryQuiesce() ? 1 : 0; }
// The same, waiting for that job (scene changes only).
extern "C" void port_par_anim_quiesce(void) { sParSpec.Quiesce(); }
// 0: the worker finished the job (its results are visible); 1: the game thread took it; 2: the worker is still in it.
extern "C" int port_par_anim_claim(void* item) {
    const uint32_t waitUs = PORT_HOT_KNOB(uint32_t, ParEnvInt("GXV_PAR_ANIM_WAIT_US", 300, 0, 5000));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
    switch (port::par::SpecWorker::Claim(*static_cast<std::atomic<uint8_t>*>(item), waitUs)) {
    case port::par::SpecWorker::kClaimDone: return 0;
    case port::par::SpecWorker::kClaimTaken: return 1;
    default: return 2;
    }
}
// GXV_PAR_VIEW=2 (drawpar, 6 Nov; decomp/src/Game/LiveActor/ModelManager.cpp, platform/include/port/par_async.h): the drawn turn's view
// jobs on a worker of their own (a SpecWorker: no join; the game thread claims what it reaches first). Core GXV_PAR_CORE (default 3,
// the fourth core when it is proven, else 1); priority GXV_PAR_PRIO (default one step ABOVE the game thread's: a draw that reaches a
// job the worker is in waits for it, so the worker must not be preempted by our own threads on its core).
static_assert(int(port::par::kJobPending) == int(port::par::async_detail::kPending) &&
                  int(port::par::kJobRunning) == int(port::par::async_detail::kRunning) &&
                  int(port::par::kJobDone) == int(port::par::async_detail::kDone) && int(port::par::kJobTaken) == int(port::par::async_detail::kTaken),
              "par_async.h's job states are par_spec.h's");
namespace {
port::par::SpecWorker& sParViewWorker = *new port::par::SpecWorker;  // (never destroyed: its worker waits on it until the process ends)
struct ParAsyncWindow {
    uint64_t t0 = 0;
    uint32_t pictures = 0;
    uint64_t c[8] = {};  // (ModelManager.cpp's order: jobs, run in the walk, computed at once, dispatched, by touches, at the retire,
                         // waits, wait polls)
    uint64_t retireUs = 0;
    uint32_t retireMax = 0;
} sParAsyncWin;
}  // namespace
extern "C" int port_par_view_async_run(void (*fn)(void*, unsigned), void* ctx, void* items, unsigned n) {
    static const bool started = [] {
        const int prio = ParEnvInt("GXV_PAR_PRIO", 0, 0, 0xFF);
        const bool ok = sParViewWorker.Start(ParEnvInt("GXV_PAR_CORE", 3, 1, 3), prio > 0 ? prio : -1, "smg_view_async");
        port_log_summary("par-view async: %s (GXV_PAR_VIEW=2; core %d, priority 0x%X)",
                         ok ? "worker started" : "the worker could not be started: the draw's touches and the retire compute every job",
                         sParViewWorker.Core(), static_cast<unsigned>(sParViewWorker.Priority()));
        return ok;
    }();
    if (!started) return 0;
    sParViewWorker.Dispatch(reinterpret_cast<port::par::ItemFn>(fn), ctx, static_cast<std::atomic<uint8_t>*>(items), n);
    return 1;
}
extern "C" void port_par_view_async_quiesce(void) { sParViewWorker.Quiesce(); }
// One picture's counts (at its retire), and the 5 s line.
extern "C" void port_par_view_async_report(const unsigned* counters, unsigned retireUs) {
    ParAsyncWindow& w = sParAsyncWin;
    const uint64_t now = port::par::NowUs();
    if (w.t0 == 0) w.t0 = now;
    ++w.pictures;
    for (int i = 0; i < 8; ++i) w.c[i] += counters[i];
    w.retireUs += retireUs;
    if (retireUs > w.retireMax) w.retireMax = retireUs;
    if (now - w.t0 < 5000000u) return;
    uint32_t busyUs = 0, workerJobs = 0, sleeps = 0;
    sParViewWorker.TakeStats(&busyUs, &workerJobs, &sleeps);
    const double p = w.pictures ? static_cast<double>(w.pictures) : 1.0;
    const uint64_t byOthers = w.c[4] + w.c[5];
    const uint64_t byWorker = w.c[3] > byOthers ? w.c[3] - byOthers : 0;
    port_log_summary("par-view async: %.1f s, %u pictures; jobs a picture %.1f (run in the walk %.2f, computed at once %.2f, handed out %.1f): "
                     "worker %.1f, taken by the draw's touches %.1f, at the retire %.1f; touches that waited for the worker %.2f a picture "
                     "(%.0f polls); retire %.3f ms (max %.3f); worker busy %.3f ms a picture (core %d, %u quiesce sleeps)",
                     (now - w.t0) / 1e6, w.pictures, w.c[0] / p, w.c[1] / p, w.c[2] / p, w.c[3] / p, byWorker / p, w.c[4] / p, w.c[5] / p,
                     w.c[6] / p, w.c[7] / p, w.retireUs / p / 1000.0, w.retireMax / 1000.0, busyUs / p / 1000.0, sParViewWorker.Core(), sleeps);
    w = ParAsyncWindow{};
    w.t0 = now;
}
// GXV_PAR_ANIM_BLEND (par2, 6 Nov; default 1 in the Vita player build): par-anim also speculates on models whose core is in a blend
// state (an interpolation after a change of animation: _1C < 1, _20 != 1, the freeze copy _29; track 0 without a BCK; a total weight
// of 0). The walk then reads the core's per-joint state (_0, _28, _50), copied into the job; a job is taken only when that state is
// byte for byte the core's at the real computation and the key (with the predicted _1C / _20 / _29) matches word for word. Exact
// (XanimeSpecWalk.hpp; host test tools/test_par_anim_walk.sh). 0: such models are computed as before.
extern "C" int port_par_anim_blend_on(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_ANIM_BLEND", 0, 0, 1));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
// GXV_PAR_FLUSH (par2, 6 Nov; DESIGN.md stage 2; default 1 in the Vita player build; needs GXV_PAR_ANIM=1): the drawn turn's far-model
// flush (port_anim_flush_stale) gets a batch of its own: the deferred models' joints are computed by a worker on the fourth core
// (GXV_PAR_FLUSH_CORE, default 3) from the end of the flush list, while the game thread waits for the front end and then computes from
// the start; each model takes the worker's result only on par-anim's word-for-word key match (ModelManager.cpp). Exact.
//   GXV_PAR_FLUSH_PRIO=0xNN  the worker's priority (default one step ABOVE the game thread's, as par-view async's: its jobs are read
//                            within the same few milliseconds)
// Every 5 s (basic log): "par-flush: ..." with the pictures, the deferred models, the jobs made and why others were not, the hits,
// misses, the jobs the game thread reached first, the worker's time and the game thread's time saved.
namespace {
port::par::SpecWorker& sParFlushWorker = *new port::par::SpecWorker;  // (never destroyed: its worker waits on it until the process ends)
struct ParFlushWindow {
    uint64_t t0 = 0;
    uint32_t pictures = 0;
    uint64_t c[15] = {};  // (ModelManager.cpp's order: built, hit, miss, not ready, busy, saved us, hit us, build us, covered by par-anim,
                          // kept by the memo, not qualifying, quiet, verified, verify bad, deferred models)
} sParFlushWin, sParLateWin;
}  // namespace
// GXV_PAR_ANIM_CB (par2; default 1 in the Vita player build): par-anim also takes models with a joint callback its walk performs
// itself (so far: NPCActor::calcJointScale, the NPCs' squash-and-stretch joint, e.g. every HoneyBee's "Center"); the callback's input
// (AnimScaleController::_C) and its controller are key words. Exact; 0: such models are computed as before.
extern "C" int port_par_anim_cb_on(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_ANIM_CB", 0, 0, 1));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
// GXV_PAR_LATE (par2; default 1 in the Vita player build; needs GXV_PAR_ANIM=1): calc-anim's own batch (ModelManager.cpp
// port_par_late_prepare), on GXV_PAR_FLUSH's worker; its 5 s line is "par-late: ..." (the par-flush line's fields; "deferred models"
// reads "candidates", "covered by par-anim" means par-anim's job had the same key).
extern "C" int port_par_late_on(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_LATE", 0, 0, 1));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
extern "C" int port_par_flush_on(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_FLUSH", 0, 0, 1));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
extern "C" int port_par_flush_run(void (*fn)(void*, unsigned), void* ctx, void* items, unsigned n) {
    static const bool started = [] {
        const int prio = ParEnvInt("GXV_PAR_FLUSH_PRIO", 0, 0, 0xFF);
        const bool ok = sParFlushWorker.Start(ParEnvInt("GXV_PAR_FLUSH_CORE", 3, 1, 3), prio > 0 ? prio : -1, "smg_flush_spec");
        port_log_summary("par-flush: %s (GXV_PAR_FLUSH=1%s; core %d, priority 0x%X)",
                         ok ? "worker started" : "the worker could not be started: the far models are computed as before",
                         port_par_anim_verify_on() ? ", VERIFY" : "", sParFlushWorker.Core(), static_cast<unsigned>(sParFlushWorker.Priority()));
        return ok;
    }();
    if (!started) return 0;
    sParFlushWorker.Dispatch(reinterpret_cast<port::par::ItemFn>(fn), ctx, static_cast<std::atomic<uint8_t>*>(items), n);
    return 1;
}
extern "C" void port_par_flush_quiesce(void) { sParFlushWorker.Quiesce(); }
extern "C" int port_par_flush_try_quiesce(void) { return sParFlushWorker.TryQuiesce() ? 1 : 0; }
// One picture's counts (port_par_flush_end), and the 5 s line.
extern "C" void port_par_flush_note(const unsigned* counts, unsigned n, int late) {
    ParFlushWindow& w = late ? sParLateWin : sParFlushWin;
    const uint64_t now = port::par::NowUs();
    if (w.t0 == 0) w.t0 = now;
    ++w.pictures;
    for (unsigned i = 0; i < n && i < 15; ++i) w.c[i] += counts[i];
    if (now - w.t0 < 5000000u) return;
    uint32_t workerUs = 0, workerJobs = 0, sleeps = 0;
    sParFlushWorker.TakeStats(&workerUs, &workerJobs, &sleeps);  // (both batches' worker time since the last line of either)
    if (late ? port_par_late_on() : port_par_flush_on()) {
        const double p = w.pictures ? static_cast<double>(w.pictures) : 1.0;
        port_log_summary("%s: %.1f s, %u %s; a %s: %s %.1f, jobs %.1f (not made: covered by par-anim %.1f, kept by the "
                         "memo %.1f, not qualifying %.1f, quiet %.1f); hits %.1f, misses %.1f, reached first by the game thread %.1f, busy %.2f; "
                         "worker %.3f ms (%u jobs); game thread: making %.3f ms, saved %.3f ms (walks %.3f - copies %.3f); verify: %llu checked, "
                         "%llu mismatches",
                         late ? "par-late" : "par-flush", (now - w.t0) / 1e6, w.pictures, late ? "calc-anims" : "pictures",
                         late ? "calc-anim" : "picture", late ? "candidates" : "deferred models", w.c[14] / p, w.c[0] / p, w.c[8] / p, w.c[9] / p, w.c[10] / p, w.c[11] / p, w.c[1] / p,
                         w.c[2] / p, w.c[3] / p, w.c[4] / p, workerUs / p / 1000.0, workerJobs, w.c[7] / p / 1000.0,
                         (static_cast<double>(w.c[5]) - static_cast<double>(w.c[6]) - static_cast<double>(w.c[7])) / p / 1000.0, w.c[5] / p / 1000.0,
                         w.c[6] / p / 1000.0, static_cast<unsigned long long>(w.c[12]), static_cast<unsigned long long>(w.c[13]));
    }
    w = ParFlushWindow{};
    w.t0 = now;
}
// One batch's counts (at the next batch's start).
extern "C" void port_par_anim_note(const unsigned* counts, unsigned n) {
    ParAnimWindow& w = sParAnimWin;
    const uint64_t now = port::par::NowUs();
    if (w.t0 == 0) w.t0 = now;
    ++w.batches;
    for (unsigned i = 0; i < n && i < 11; ++i) w.c[i] += counts[i];
    if (now - w.t0 < 5000000u) return;
    uint32_t workerUs = 0, workerJobs = 0, sleeps = 0;
    sParSpec.TakeStats(&workerUs, &workerJobs, &sleeps);
    const double secs = (now - w.t0) / 1e6, b = w.batches ? static_cast<double>(w.batches) : 1.0;
    const uint64_t spec = w.c[0], hit = w.c[1], miss = w.c[2], notReady = w.c[3], busy = w.c[4];
    const uint64_t consulted = hit + miss + notReady + busy;
    const double saved = (static_cast<double>(w.c[5]) - static_cast<double>(w.c[6])) / 1000.0;
    port_log_summary("par-anim: %.1f s, %u batches (%llu not made: the worker was still in the last one); a batch: speculated %.1f, hits %.1f (%.0f %% of the speculated), misses %.1f, not ready %.1f, "
                     "busy %.2f, not consulted %.1f; worker %.3f ms a batch (%u jobs, %.1f %% of its core, %u quiesce sleeps); game thread saved "
                     "%.3f ms a batch (walks %.3f - copies %.3f), dispatch %.3f ms a batch; verify: %llu checked, %llu mismatches",
                     secs, w.batches, static_cast<unsigned long long>(w.c[10]), spec / b, hit / b, spec ? 100.0 * static_cast<double>(hit) / static_cast<double>(spec) : 0.0, miss / b,
                     notReady / b, busy / b, spec > consulted ? static_cast<double>(spec - consulted) / b : 0.0, workerUs / b / 1000.0, workerJobs,
                     workerUs / (secs * 1e4), sleeps, saved / b, w.c[5] / b / 1000.0, w.c[6] / b / 1000.0, w.c[7] / b / 1000.0,
                     static_cast<unsigned long long>(w.c[8]), static_cast<unsigned long long>(w.c[9]));
    w = ParAnimWindow{};
    w.t0 = now;
}
extern "C" int port_turn_skipped(void) {
    SceneKeyRefresh();
    const bool on = PORT_HOT_KNOB(bool, [] {  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
        const char* e = getenv("PETARI_SKIP_VIEWCALC");
        return e && *e == '1';
    }());
    return on && sTurnSkipped.load(std::memory_order_relaxed);
}

// GXV_SLIP=1 (default 0): bounded slip, see port::pace::Slip (platform/include/port/frame_pace.h). GXV_SLIP_MAX=<percent> (default 15)
// is the largest slowdown of the game it may take instead of skipping pictures. Read once; 0 = off (then nothing below runs).
#if defined(__vita__) && PORT_LOGGING
static bool sSlipTrace = false;
#endif
static unsigned SlipMaxOnce() {
    static const unsigned max = [] {
#if defined(__vita__) && PORT_LOGGING
        const char* trace = getenv("GXV_SLIP_TRACE");
        sSlipTrace = trace && strcmp(trace, "1") == 0;
#endif
        if (vgx::EnvInt("GXV_SLIP", 0) == 0) {
#if defined(__vita__) && PORT_LOGGING
            if (sSlipTrace) {
                const int saved = errno;
                port_log_summary("pace: slip-config enabled=0 max_percent=0 trace=1");
                errno = saved;
            }
#endif
            return 0u;
        }
        const long m = vgx::EnvInt("GXV_SLIP_MAX", 15);
        const unsigned pct = m < 1 ? 1u : m > 50 ? 50u : static_cast<unsigned>(m);
        port::pace::GlobalSlip().Configure(pct);
#if defined(__vita__) && PORT_LOGGING
        if (sSlipTrace) {
            const int saved = errno;
            port_log_summary("pace: slip-config enabled=1 max_percent=%u trace=1", pct);
            errno = saved;
        }
#endif
        return pct;
    }();
    return max;
}
// (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h; SlipMaxOnce's guarded static still configures the slip exactly once)
static unsigned SlipMax() { return PORT_HOT_KNOB(unsigned, SlipMaxOnce()); }
static bool sSlipDrew = false;  // the last turn's picture decision (port_frameskip_turn), for the end of its work

// GXV_STEADY30=1 (default 0; the Vita's entry.cpp turns it on): Performance mode's steady 30, port::pace::SteadyCadence
// (platform/include/port/frame_pace.h). Only with the 60 target (GXV_AUTOQ_TARGET=60) and the "Up to 60" skip rule (no GXV_FPS_LIMIT=30,
// no GXV_PACE: those take the turn before the rule below). When the pictures stay under GXV_STEADY30_BELOW (default 38) a second for
// 3 s, and the GPU is not what holds them while the automatic quality controller could still make its work smaller, every second
// turn is offered a picture (an even 30, each picture 2 vblanks with GXV_PRESENT_SYNC=2); it is left when the pictures it estimates
// without it (the turns' own costs, and the front end's, renderer's and GPU's per picture) reach GXV_STEADY30_ABOVE (default 45) for 4 s.
// The basic log says when and why it engages or leaves ("pace: steady 30 ...").
static std::atomic<bool> sSteadyOn{false};
static port::pace::SteadyCadence& SteadyOnce() {
    static port::pace::SteadyCadence c;
    static const bool configured = [] {
        const bool on = vgx::EnvInt("GXV_STEADY30", 0) != 0 && vgx::EnvInt("GXV_AUTOQ_TARGET", 0) == 60 && vgx::EnvInt("GXV_FPS_LIMIT", 0) != 30;
        port::pace::SteadyCadence::Params p;
        const long below = vgx::EnvInt("GXV_STEADY30_BELOW", static_cast<long>(port::pace::kSteady30BelowFps)), above = vgx::EnvInt("GXV_STEADY30_ABOVE", static_cast<long>(port::pace::kSteady30AboveFps));
        p.belowFps = static_cast<double>(below < 20 ? 20 : below > 58 ? 58 : below);
        p.aboveFps = static_cast<double>(above < 31 ? 31 : above > 60 ? 60 : above);
        if (p.aboveFps < p.belowFps + 3.0) p.aboveFps = p.belowFps + 3.0;  // (a gap between the two, or it would flap)
        c.Configure(on, p);
        if (on)
            port_log_summary("pace: steady 30 on (GXV_STEADY30, Performance mode): under %.0f pictures a second for %.0f s, held by the CPU, the pictures "
                             "come every second turn; back to up to 60 when %.0f or more are estimated for %.0f s", p.belowFps, p.engageSec, p.aboveFps, p.releaseSec);
        return true;
    }();
    (void)configured;
    return c;
}
// (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h; SteadyOnce's guarded static still configures it exactly once)
static bool SteadyEnabled() { return PORT_HOT_KNOB(bool, SteadyOnce().Enabled()); }
extern "C" int port_pace_steady30_engaged(void) { return sSteadyOn.load(std::memory_order_relaxed) ? 1 : 0; }
static void SteadyLog(const port::pace::SteadyCadence& c) {
    const char* why = c.Engaged() ? "engaged" : "left";
    port_log_summary("pace: steady 30 %s: %.1f pictures a second in the last second; without it %.1f are estimated (game turns %.1f, "
                     "front end/renderer/GPU %.1f); GPU %.1f ms a picture, %s; next release blocked %.0f s",
                     why, c.RateFps(), c.EstimateFps(), c.GameFps(), c.PipeFps(), c.GpuUs() / 1000.0,
                     c.Lowerable() ? "a smaller picture still possible" : "nothing left to lower", c.BlockSec());
}
static int SteadyTurn(int allowed, int code) {
    port::pace::SteadyCadence& c = SteadyOnce();
    uint32_t gpuUs = 0, pipeUs = 0;
    int lowerable = 0;
    const bool known = port_autoq_stages && port_autoq_stages(&gpuUs, &pipeUs, &lowerable) != 0;
    c.Stages(known, gpuUs, pipeUs, lowerable != 0);
    const bool quiet = port_screen_quiet && port_screen_quiet() != 0;
    const bool offered = c.Begin(static_cast<uint64_t>(port_host_time_ns() / 1000), allowed != 0, quiet);
    if (allowed && !offered && code == 0) code = 1;  // (not offered: the turn keeps its retrace, as one the back end is too busy for)
    c.Decision(offered || !allowed, code == 0);
    return code;
}
static void SteadyWorkDone() {
    if (!SteadyEnabled()) return;
    port::pace::SteadyCadence& c = SteadyOnce();
    const unsigned before = c.Changes();
    c.End(static_cast<uint64_t>(port_host_time_ns() / 1000));
    if (c.Changes() != before) {
        sSteadyOn.store(c.Engaged(), std::memory_order_relaxed);
        SteadyLog(c);
    }
}

static int FrameskipDecide(int allowed) {
    if (PORT_RUNTIME_DIAGNOSTICS) sTurn.kind = allowed ? TurnKind::DrawSelected : TurnKind::RequiredDraw;
    // Testing (the harness has a virtual clock and never falls behind): PETARI_FORCE_SKIP=N leaves out N turns of every N+1.
    const int forced = PORT_HOT_KNOB(int, [] {  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
        const char* e = getenv("PETARI_FORCE_SKIP");
        return e && *e ? (atoi(e) < 0 ? 0 : (atoi(e) > 8 ? 8 : atoi(e))) : 0;
    }());
    if (forced > 0 && allowed) {
        static int phase = 0;
        phase = (phase + 1) % (forced + 1);
        if (!phase) return 0;
        sSkippedTurns.fetch_add(1, std::memory_order_relaxed);
        if (PORT_RUNTIME_DIAGNOSTICS) sTurn.kind = TurnKind::ForcedSkip;
        return 2;
    }
    if (port_vgx_pace_turn) {
        const int paced = port_vgx_pace_turn(allowed);
        if (paced >= 0) {
            if (paced != 0) {
                sSkippedTurns.fetch_add(1, std::memory_order_relaxed);
                if (PORT_RUNTIME_DIAGNOSTICS) sTurn.kind = paced == 2 ? TurnKind::LateSkip : TurnKind::PacedSkip;
            }
            return paced;
        }
    }
    const int maxRun = PORT_HOT_KNOB(int, [] {  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
        const char* e = getenv("PETARI_FRAMESKIP");
        if (e && *e) return atoi(e) < 0 ? 0 : (atoi(e) > 8 ? 8 : atoi(e));
        return port_handheld() ? 3 : 0;
    }());
    if (maxRun <= 0) return 0;
    const int pipeDepth = PORT_HOT_KNOB(int, [] {
        const char* e = getenv("PETARI_PIPE_DEPTH");
        return e && *e ? (atoi(e) < 0 ? 0 : (atoi(e) > 3 ? 3 : atoi(e))) : 2;
    }());
    const int maxPipeRun = PORT_HOT_KNOB(int, [] {
        const char* e = getenv("PETARI_PIPE_RUN");
        return e && *e ? (atoi(e) < 0 ? 0 : (atoi(e) > 12 ? 12 : atoi(e))) : 6;
    }());
    // GXV_SKIP_MAXSTEP=N (2-8; default 0 = off): a picture at least every N turns, the skip rules' two reasons counted together
    // (frame_pace.h LegacySkip): no picture stays on screen for more than N vblanks because of a skip run (r49, lit hub: 8-12 pictures
    // every 5 s advanced 4+ turns, the 77-93 ms "longest"). Every 5 s with forced pictures, one "pace: step cap" line counts them.
    static const int maxStep = [] {
        const int n = static_cast<int>(vgx::EnvInt("GXV_SKIP_MAXSTEP", 0));
        const int v = n >= 2 ? (n > 8 ? 8 : n) : 0;
        if (v) port_log_summary("pace: step cap on (GXV_SKIP_MAXSTEP=%d): a picture at least every %d game turns", v, v);
        return v;
    }();
    // The rule itself is port::pace::LegacySkip (platform/include/port/frame_pace.h), so that the host model runs the same code.
    static port::pace::LegacySkip rule(maxRun, pipeDepth, maxPipeRun, &port::pace::GlobalSlip(), maxStep);
    // GXV_SKIP_PATTERN=N (1-3; default 0 = off): only N of every 4 turns are offered a picture, in a fixed rhythm (3: 45 a second,
    // holds 1-1-2), port::pace::SkipPattern (frame_pace.h). The rule below still decides for the offered turns; a turn it leaves out
    // passes the offer to the next one. A turn that is not offered takes its retrace (code 1), as one the back end is too busy for.
    static port::pace::SkipPattern pattern(static_cast<int>(vgx::EnvInt("GXV_SKIP_PATTERN", 0)));
    const unsigned pending = pipeDepth > 0 && port_vgx_backend_pending ? port_vgx_backend_pending() : 0;
    port::pace::LegacySkip::Verdict v = rule.Turn(port_host_time_ns(), allowed != 0, pending);
    if (maxStep) {
        static uint32_t capped = 0, turns = 0;
        static int64_t since = 0;
        const int64_t now = port_host_time_ns();
        if (!since) since = now;
        capped += v.capped ? 1u : 0u;
        ++turns;
        if (now - since >= 5000000000ll) {
            if (capped)
                port_log_summary("pace: step cap: %u of %u turns in %.1f s drew a picture the skip rule would have left out (GXV_SKIP_MAXSTEP=%d)",
                                 capped, turns, (now - since) / 1e9, maxStep);
            capped = turns = 0;
            since = now;
        }
    }
    if (pattern.Enabled()) {
        const bool offered = pattern.Offer(allowed == 0);  // (a scene change is drawn at the normal pace and restarts the rhythm)
        if (allowed && !offered && v.code == 0) v.code = 1;
        pattern.Result(offered || !allowed, v.code == 0);
    }
    if (SteadyEnabled()) {
        const int before = v.code;
        v.code = SteadyTurn(allowed, v.code);
        if (v.code != before) v.behind = v.gpuBusy = false;  // (left out by the cadence: a paced skip, not a late or busy one)
    }
    if (v.code != 0) {
        sSkippedTurns.fetch_add(1, std::memory_order_relaxed);
        if (PORT_RUNTIME_DIAGNOSTICS) {
            sTurn.kind = v.behind ? TurnKind::LateSkip : v.gpuBusy ? TurnKind::BacklogSkip : TurnKind::PacedSkip;
            sTurn.bothSkipReasons = v.behind && v.gpuBusy;
        }
        // 2: late, catch up (the turn does not wait for its retrace: a wait would make every skipped turn last a
        // whole refresh, the debt would never be paid, and the title screen ran at a fixed 15 pictures a second
        // on the console); 1: not late, the turn takes its retrace.
    }
    return v.code;
}
int port_frameskip_turn(int allowed) {
    if (SlipMax()) port::pace::GlobalSlip().Begin(static_cast<uint64_t>(port_host_time_ns() / 1000), allowed != 0);
    const int decision = FrameskipDecide(allowed);
#if defined(__vita__) && PORT_LOGGING
    vgx::workload::AnimDrawDecision(decision);
#endif
    sSlipDrew = decision == 0;
    if (port_vgx_present_turn) port_vgx_present_turn(decision == 0 ? 1 : 0);  // (every turn: its clock; a drawn one: its stamp)
    return decision;
}
// The end of a turn's work (before the retrace wait): the slip's cost sample, and its log line when it engages or leaves.
static void SlipWorkDone() {
    if (!SlipMax()) return;
    port::pace::Slip& slip = port::pace::GlobalSlip();
    slip.End(static_cast<uint64_t>(port_host_time_ns() / 1000), sSlipDrew);
    static unsigned reported = 0;
    if (slip.Changes() != reported) {
        reported = slip.Changes();
        port_log("pace: slip %s: a pair of turns costs %.1f ms (%.1f pictures a second at %.0f %% game speed, cap %u %%); %.0f ms behind real time so far (not caught up)",
                 slip.Engaged() ? "engaged" : "left", slip.PairUs() / 1000.0, 60.0 * port::pace::Slip::kPeriodUs / slip.PairUs(),
                 slip.SpeedPercent(), slip.MaxPercent(), slip.LagUs() / 1000.0);
#if defined(__vita__) && PORT_LOGGING
        if (sSlipTrace) {
            const int saved = errno;
            port_log_summary("pace: slip-state engaged=%u changes=%u pair_us=%.1f speed_percent=%.1f max_percent=%u lag_us=%llu",
                             static_cast<unsigned>(slip.Engaged()), slip.Changes(), slip.PairUs(), slip.SpeedPercent(),
                             slip.MaxPercent(), static_cast<unsigned long long>(slip.LagUs()));
            errno = saved;
        }
#endif
    }
}
uint32_t port_frameskip_count(void) { return sSkippedTurns.load(std::memory_order_relaxed); }

// The title screen's logo has appeared (decomp/src/Game/Screen/TitleSequenceProduct.cpp): the end of the start-up, which the
// Vita's loading screen waits for. Logged once.
static std::atomic<int> sTitleVisible{0};
extern "C" void port_boot_title_visible(void) {
    if (sTitleVisible.exchange(1) == 0) port_log("boot: the title screen has appeared");
}
extern "C" int port_boot_title_is_visible(void) { return sTitleVisible.load(std::memory_order_relaxed); }

void port_perf_record(int64_t ns) {
    if (!PORT_RUNTIME_DIAGNOSTICS) return;
    std::lock_guard<std::mutex> lock(sLock);
    sRecordThisFrame += ns;
}

void port_perf_prepare(int64_t ns) {
    if (!PORT_RUNTIME_DIAGNOSTICS) return;
    std::lock_guard<std::mutex> lock(sLock);
    sPrepare.add(ns / 1e6);
}

void port_perf_gpu_wait(int64_t ns) {
    if (!PORT_RUNTIME_DIAGNOSTICS) return;
    std::lock_guard<std::mutex> lock(sLock);
    sGpuWait.add(ns / 1e6);
}

void port_perf_render(int64_t cpuNs, int64_t uploadNs) {
    if (!PORT_RUNTIME_DIAGNOSTICS) return;
    std::lock_guard<std::mutex> lock(sLock);
    sRenderCpu.add(cpuNs / 1e6);
    sUpload.add(uploadNs / 1e6);
    sRenderFrames++;
}

}  // extern "C"

// The thread that runs GameSystem::frameLoop (the game's main loop). The deferred work lists (GXV_ANIM_LOD's far models,
// GXV_DL_LAZY's material display lists) are filled and worked off by that thread only: a stage's actors are built on the
// loading thread (FunctionAsyncExecutorThread), whose calcAnim / updateMaterial during init must run at once as in the original.
// Listing them there (1) let the main loop's flush run updateDL / calcAnim on an actor whose init was not finished, and (2) let
// two guest threads push to the same vector (they switch at safe points, operator new among them): three stage-load crashes on
// the console, 4 Nov (MaterialAnmPlayerBase::beginDiff from port_dl_flush_stale; GameScene::init; findResMotion in init).
#include <pthread.h>
// (GXV_HOT_KNOBS, default 1: asked about three times a model a turn (ModelManager, LiveActor), so the acquire load of the flag, a
// `dmb ish` on the console, is a relaxed one, and the thread a relaxed atomic word: the only thread for which the answer is 1 is the
// one that marked itself, and it reads its own stores; any other thread reads the old value (none: no thread's pthread_self) or the
// marked one, and neither is itself, so the answer is 0 as before. 0: the acquire load as before.)
namespace { std::atomic<pthread_t> sFrameThread{}; std::atomic<bool> sFrameThreadKnown{false}; }
#if defined(__vita__)
// GXV_LC_FRAMETID (logiccut, 6 Nov; default 0): the same question answered with the kernel thread id. pthread_self() on the console
// is pthread_getspecific (pthread-embedded keeps the handle in a key): sceKernelGetTLSAddr behind two calls, then a walk of the
// thread's key list, about three times a model a turn. A thread's id is unique among the live threads and the frame thread (the
// game's main loop) lives for the whole run, so "my id is the frame thread's id" holds exactly when pthread_equal does. The id is
// stored before the flag, as the handle is, and read under the same flag.
#include <psp2/kernel/threadmgr.h>
namespace { std::atomic<int> sFrameThreadId{0}; }
static bool lcFrameTid() {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_LC_FRAMETID"); return e && *e ? atoi(e) : 0; }()) != 0;
}
#endif
extern "C" void port_mark_frame_thread(void) {
    if (!sFrameThreadKnown.load(std::memory_order_relaxed)) {
        sFrameThread.store(pthread_self(), std::memory_order_relaxed);
#if defined(__vita__)
        sFrameThreadId.store(sceKernelGetThreadId(), std::memory_order_relaxed);
#endif
        sFrameThreadKnown.store(true, std::memory_order_release);
    }
}
extern "C" int port_on_frame_thread(void) {
    const bool known = PORT_HOT_KNOB(bool, port::hotknob::Fenced()) ? sFrameThreadKnown.load(std::memory_order_acquire)
                                                                    : sFrameThreadKnown.load(std::memory_order_relaxed);
#if defined(__vita__)
    if (lcFrameTid()) {
        return known && sFrameThreadId.load(std::memory_order_relaxed) == sceKernelGetThreadId() ? 1 : 0;
    }
#endif
    return known && pthread_equal(sFrameThread.load(std::memory_order_relaxed), pthread_self()) ? 1 : 0;
}

// GXV_JPA_LOOK (logic12, 5 Nov; default 1; decomp/src/JSystem/JParticle/JPAResource.cpp): a particle's look functions (colour,
// alpha, scale, texture-index animation: what only the draw reads) run at the emitter's next draw instead of at every calc, so a turn
// that draws no picture skips them. Exact for every picture; game logic never reads them. 0: off (every calc runs them, as before).
extern "C" int port_jpa_look_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_JPA_LOOK"); return e && *e == '0' ? 0 : 1; }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}

// PETARI_RAY_NEAREST (forkB, 6 Nov; ported from Producdevity's fork, 1807939; default 1, exact; decomp/src/Game/Util/MapUtil.cpp,
// Game/Map/KCollision.cpp): MapUtil's first polygon on a line keeps only the nearest hit, so without a triangle filter each
// collision's walk through its cells ends at the first cell that starts beyond the nearest hit found (shadows, ground checks).
// 0: every line is walked to its end, as before. 2: both, compared ("rays:" lines in logging builds; 0 different expected).
// (One case differs by design: a line whose full walk filled the keeper's 32-hit list before reaching a part with a nearer hit; the
// short walk leaves room and finds that nearer hit. The fork's verify saw none in 494k searches.)
extern "C" int port_ray_nearest_mode(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("PETARI_RAY_NEAREST"); return e && *e ? atoi(e) : 1; }());  // (GXV_HOT_KNOBS)
}
#if PORT_LOGGING
static uint32_t sRaysCompared = 0, sRaysMismatching = 0;  // (PETARI_RAY_NEAREST=2; the game's threads, counts only)
#endif
extern "C" void port_ray_nearest_verified(int mismatching) {
#if PORT_LOGGING
    if (mismatching && sRaysMismatching++ < 20) port_log_summary("rays: a first polygon on a line differs with the nearest-hit end");
    if (++sRaysCompared >= 65536u) {
        port_log_summary("rays: %u first polygons on a line found with and without the nearest-hit end, %u different (total)", sRaysCompared,
                         sRaysMismatching);
        sRaysCompared = 0;
    }
#else
    (void)mismatching;
#endif
}

// Logic counters (logic12, 5 Nov; logging builds only): how often the per-turn systems that the GXV_ANIM_PROF rows lump into an
// actor's or a system object's time run. Counted always (an add per call or per batch), logged with GXV_ANIM_PROF=1 as one line
// every 300 turns, per turn:
//   logic-count: ptcl_calc=.. look_flushed=.. emitters=.. grav_calls=.. grav_visits=.. line_casts=.. ball_casts=.. shadow_casts=..
//                area_finds=.. area_tests=.. sensor_pairs=..
// 0 particles calculated (JPAResource::calc)  1 particles given their look at a draw (GXV_JPA_LOOK)  2 emitters calculated
// 3 PlanetGravityManager::calcTotalGravityVector calls  4 gravities it tried  5 CollisionCategorizedKeeper::checkStrikeLine
// 6 checkStrikeBall  7 ShadowController::updateProjection casts  8 AreaObjMgr::find_in calls  9 areas they tested
// 10 SensorHitChecker::movement sensor pairs looked at (upper bound: the groups' sizes multiplied)
#if PORT_LOGGING
static uint64_t sLogicCount[12];
#endif
extern "C" void port_logic_count(int what, unsigned n) {
#if PORT_LOGGING
    if (what >= 0 && what < 12) sLogicCount[what] += n;
#else
    (void)what; (void)n;
#endif
}

// GXV_ANIM_PROF=1 (game3, 5 Nov; default 0; logging builds only): which objects the game thread's calc-anim and movement lists
// spend their time on. NameObjCategoryList::execute times every object it runs (wall clock, 1 us ticks, unbiased over many calls)
// under the category CategoryList::execute marked (port_anim_lod_set_category: calc-anim types as they are, movement types as
// 0x100 | type); a LiveActor names the row by its model's resource name (ASCII), anything else by its NameObj name. Every 300 turns
// it logs the turn's totals, the drawn turns' flushes (GameSystem::frameLoop: GXV_ANIM_LOD's far models, GXV_DL_LAZY's materials,
// the left-out view pass), and the costliest rows:
//   anim-prof: turns=300 drawn=118 calc_us/turn=... move_us/turn=... deferred/turn=... memo_hits/turn=... flush_us/drawn anim=... dl=... view=...
//   anim-prof: calc #1 cat=NPC name=Tico calls/turn=24.0 us/turn=812.3 us/call=33.8 deferred/turn=10.2 memo/turn=0.0
// Rows are kept per scene (dropped with it: port_anim_prof_drop).
#if PORT_LOGGING
namespace {
struct ProfRow {
    uint32_t hash;    // 0: free
    int16_t domain;   // 0 calc-anim, 1 movement
    int16_t category;
    char name[28];
    uint32_t calls, deferred, memo;
    uint64_t ns;
};
constexpr unsigned kProfRows = 1024;
ProfRow sProfRows[kProfRows];
int sProfDepth = 0;
int64_t sProfT0 = 0;
const char* sProfActorName = nullptr;
uint32_t sProfDeferred = 0, sProfMemo = 0;  // (running counters, read around each object)
uint32_t sProfDeferred0 = 0, sProfMemo0 = 0;
uint32_t sProfTurns = 0, sProfDrawn = 0;
uint64_t sProfFlushNs[3] = {0, 0, 0};
bool sProfOverflow = false;
const char* const kProfCalcNames[] = {"ClippedMapParts", "Planet", "CollisionMapObj", "CollisionEnemy", "Environment", "MapObj", "NPC",
    "Ride", "Enemy", "Player", "PlayerDecoration", "MapObjDecoration", "MirrorMapObj", "Layout", "LayoutDecoration", "MovieSubtitles",
    "Item", "0x11", "0x12", "AnimParticle", "AnimParticleIgnorePause", "0x15"};
const char* const kProfMoveNames[] = {"0x00", "StopSceneDelayRequest", "Camera", "ScreenEffect", "ClippingDirector", "SensorHitChecker",
    "MsgSharedGroup", "0x07", "0x08", "AudCameraWatcher", "TalkDirector", "DemoDirector", "0x0C", "AreaObj", "Layout", "LayoutDecoration",
    "Movie", "WipeLayout", "LayoutOnPause", "MovieSubtitles", "0x14", "0x15", "MirrorCamera", "ImageEffect", "AudEffectDirector",
    "AudBgmConductor", "CameraCover", "SwitchWatcherHolder", "ClippedMapParts", "Planet", "CollisionMapObj", "CollisionEnemy",
    "CollisionDirector", "Environment", "MapObj", "MapObjDecoration", "Sky", "Player", "PlayerDecoration", "PlayerMessenger", "NPC",
    "Ride", "Enemy", "EnemyDecoration", "Item", "ShadowControllerHolder"};
const char* ProfCategoryName(int domain, int category) {
    if (domain == 0 && category >= 0 && category < static_cast<int>(sizeof(kProfCalcNames) / sizeof(*kProfCalcNames))) return kProfCalcNames[category];
    if (domain == 1 && category >= 0 && category < static_cast<int>(sizeof(kProfMoveNames) / sizeof(*kProfMoveNames))) return kProfMoveNames[category];
    return "?";
}
void ProfPrintable(char* out, size_t size, const char* in) {  // (NameObj names are often Shift-JIS: bytes outside ASCII as ~xx)
    size_t o = 0;
    for (const unsigned char* c = reinterpret_cast<const unsigned char*>(in ? in : "(null)"); *c && o + 4 < size; ++c) {
        if (*c > 0x20 && *c < 0x7F) {
            out[o++] = static_cast<char>(*c);
        } else {
            o += static_cast<size_t>(snprintf(out + o, size - o, "~%02x", *c));
        }
    }
    out[o < size ? o : size - 1] = 0;
}
}  // namespace
#endif

extern "C" int port_anim_prof_on(void) {
#if PORT_LOGGING
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_ANIM_PROF"); return e && *e == '1' ? 1 : 0; }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
#else
    return 0;
#endif
}
// Around one object of a category list (NameObjCategoryList::execute, PORT_ANIM_PROF_CALL). Only the outermost object is timed.
extern "C" void port_anim_prof_begin(void) {
#if PORT_LOGGING
    if (sProfDepth++ != 0) return;
    sProfActorName = nullptr;
    sProfDeferred0 = sProfDeferred;
    sProfMemo0 = sProfMemo;
    sProfT0 = port_host_time_ns();
#endif
}
extern "C" void port_anim_prof_end(const char* objName) {
#if PORT_LOGGING
    if (sProfDepth <= 0 || --sProfDepth != 0) return;
    const int64_t ns = port_host_time_ns() - sProfT0;
    const int raw = sCalcAnimCategory;
    if (raw < 0 || !port_on_frame_thread()) return;
    const int domain = raw >= 0x100 ? 1 : 0;
    const int category = raw & 0xFF;
    const char* name = sProfActorName ? sProfActorName : objName;
    uint32_t h = 2166136261u;  // FNV-1a over the category and the name's bytes (up to the row's length)
    h = (h ^ static_cast<uint32_t>(raw)) * 16777619u;
    for (const char* c = name ? name : ""; *c && c - name < 27; ++c) h = (h ^ static_cast<unsigned char>(*c)) * 16777619u;
    if (!h) h = 1;
    for (unsigned probe = 0; probe < 32; ++probe) {
        ProfRow& r = sProfRows[(h + probe) & (kProfRows - 1)];
        if (r.hash == 0) {
            r.hash = h;
            r.domain = static_cast<int16_t>(domain);
            r.category = static_cast<int16_t>(category);
            snprintf(r.name, sizeof(r.name), "%s", name ? name : "(null)");
        } else if (r.hash != h || r.domain != domain || r.category != category || strncmp(r.name, name ? name : "(null)", sizeof(r.name) - 1) != 0) {
            continue;
        }
        ++r.calls;
        r.ns += ns > 0 ? static_cast<uint64_t>(ns) : 0;
        r.deferred += sProfDeferred - sProfDeferred0;
        r.memo += sProfMemo - sProfMemo0;
        return;
    }
    sProfOverflow = true;
#else
    (void)objName;
#endif
}
// A LiveActor names the object being timed (the first one asked for wins: an actor's own model, not the parts it computes).
extern "C" void port_anim_prof_actor_name(const char* name) {
#if PORT_LOGGING
    if (sProfDepth > 0 && !sProfActorName) sProfActorName = name;
#else
    (void)name;
#endif
}
// 0: a calcAnim deferred by GXV_ANIM_LOD; 1: a joint computation kept by GXV_ANIM_MEMO.
extern "C" void port_anim_prof_count(int what) {
#if PORT_LOGGING
    if (what == 0) ++sProfDeferred; else ++sProfMemo;
#else
    (void)what;
#endif
}
extern "C" void port_draw_prof_stage(int stage, int64_t ns);
// GameSystem::frameLoop's drawn-turn flushes: 0 port_anim_flush_stale, 1 port_dl_flush_stale, 2 port_view_flush.
extern "C" void port_anim_prof_flush(int which, int64_t ns) {
#if PORT_LOGGING
    if (which >= 0 && which < 3 && ns > 0) sProfFlushNs[which] += static_cast<uint64_t>(ns);
    port_draw_prof_stage(6 + which, ns);  // (GXV_DRAW_PROF: the same three, below)
#else
    (void)which; (void)ns;
#endif
}
extern "C" void port_anim_prof_drop(void) {
#if PORT_LOGGING
    memset(sProfRows, 0, sizeof(sProfRows));
#endif
}
// The end of a turn (GameSystem::frameLoop). Every 300 turns: the log lines, then the counts start again.
static void MoveProfTurn(int drawn);  // (hivelogic, at the end of this file)
static void HlVerifyTurn();          // (hivelogic: GXV_HL_AREACUBE=2 / GXV_HL_SENSOR=2, at the end of this file)
extern "C" void port_anim_prof_turn(int drawn) {
    MoveProfTurn(drawn);
    HlVerifyTurn();
#if PORT_LOGGING
    if (!port_anim_prof_on()) return;
    ++sProfTurns;
    if (drawn) ++sProfDrawn;
    if (sProfTurns < 300) return;
    const double turns = sProfTurns, drawnTurns = sProfDrawn ? sProfDrawn : 1;
    uint64_t total[2] = {0, 0};
    uint32_t deferred = 0, memo = 0;
    for (const ProfRow& r : sProfRows) {
        if (!r.hash) continue;
        total[r.domain ? 1 : 0] += r.ns;
        deferred += r.deferred;
        memo += r.memo;
    }
    port_log_summary("anim-prof: turns=%u drawn=%u calc_us/turn=%.1f move_us/turn=%.1f deferred/turn=%.1f memo_hits/turn=%.1f flush_us/drawn anim=%.1f dl=%.1f view=%.1f%s",
             sProfTurns, sProfDrawn, total[0] / 1000.0 / turns, total[1] / 1000.0 / turns, deferred / turns, memo / turns,
             sProfFlushNs[0] / 1000.0 / drawnTurns, sProfFlushNs[1] / 1000.0 / drawnTurns, sProfFlushNs[2] / 1000.0 / drawnTurns,
             sProfOverflow ? " (rows full: some objects not counted)" : "");
    // Per category, then the costliest rows of each domain.
    for (int domain = 0; domain < 2; ++domain) {
        uint64_t catNs[256] = {};
        uint32_t catCalls[256] = {};
        for (const ProfRow& r : sProfRows) {
            if (r.hash && r.domain == domain) { catNs[r.category & 0xFF] += r.ns; catCalls[r.category & 0xFF] += r.calls; }
        }
        for (int c = 0; c < 256; ++c) {
            if (catNs[c] / turns >= 20000.0) {  // (categories of 20 us a turn or more)
                port_log_summary("anim-prof: %s-category %s calls/turn=%.1f us/turn=%.1f", domain ? "move" : "calc", ProfCategoryName(domain, c),
                         catCalls[c] / turns, catNs[c] / 1000.0 / turns);
            }
        }
        const ProfRow* shown[20] = {};
        for (int rank = 0; rank < (domain ? 12 : 20); ++rank) {
            const ProfRow* best = nullptr;
            for (const ProfRow& r : sProfRows) {
                if (!r.hash || r.domain != domain || !r.calls) continue;
                bool used = false;
                for (int k = 0; k < rank; ++k) used |= shown[k] == &r;
                if (!used && (!best || r.ns > best->ns)) best = &r;
            }
            if (!best) break;
            shown[rank] = best;
            char name[96];
            ProfPrintable(name, sizeof(name), best->name);
            port_log_summary("anim-prof: %s #%d cat=%s name=%s calls/turn=%.1f us/turn=%.1f us/call=%.1f deferred/turn=%.1f memo/turn=%.1f",
                     domain ? "move" : "calc", rank + 1, ProfCategoryName(domain, best->category), name, best->calls / turns,
                     best->ns / 1000.0 / turns, best->ns / 1000.0 / best->calls, best->deferred / turns, best->memo / turns);
        }
    }
    // (logic12: the per-turn system counts, port_logic_count above)
    port_log_summary("logic-count: per turn: ptcl_calc=%.1f look_flushed=%.1f emitters=%.1f grav_calls=%.1f grav_visits=%.1f line_casts=%.1f "
             "ball_casts=%.1f shadow_casts=%.1f area_finds=%.1f area_tests=%.1f sensor_pairs=%.1f",
             sLogicCount[0] / turns, sLogicCount[1] / turns, sLogicCount[2] / turns, sLogicCount[3] / turns, sLogicCount[4] / turns,
             sLogicCount[5] / turns, sLogicCount[6] / turns, sLogicCount[7] / turns, sLogicCount[8] / turns, sLogicCount[9] / turns,
             sLogicCount[10] / turns);
    memset(sLogicCount, 0, sizeof(sLogicCount));
    for (ProfRow& r : sProfRows) {
        if (r.hash) { r.calls = r.deferred = r.memo = 0; r.ns = 0; }
    }
    sProfTurns = sProfDrawn = 0;
    sProfFlushNs[0] = sProfFlushNs[1] = sProfFlushNs[2] = 0;
    sProfOverflow = false;
#else
    (void)drawn;
#endif
}

// GXV_DRAW_PROF=1 (draw7, 5 Nov; logging builds only, default 0): where the drawn turn's draw phase and its pre-picture flushes go.
// Rows (inclusive time of the outermost call; a list called inside another is in its parent's time):
//   exec-XX   CategoryList::execute(MR::DrawType 0xXX)            (decomp/include/Game/Scene/SceneFunction.hpp)
//   opa-XX    CategoryList::drawOpa(MR::DrawBufferType 0xXX)      xlu-XX  the same, drawXlu
//   entry-2d / entry-3d / entry-mirror                            CategoryList::entryDrawBuffer* (the view pass and its entry)
// Stages (timed whatever the nesting): draw (GameSystem::draw), scene (drawScene), seq (the sequence director's 2D), mirror, 3d,
// 2d (GameScene::drawMirror / draw3D / draw2D), flush-anim / flush-dl / flush-view (GameSystem::frameLoop's flushes).
// Every 300 drawn turns: one "draw-prof:" line with the stages per drawn turn, then the 24 costliest rows.
extern "C" int port_draw_prof_on(void) {
#if PORT_LOGGING
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_DRAW_PROF"); return e && *e == '1' ? 1 : 0; }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
#else
    return 0;
#endif
}
#if PORT_LOGGING
namespace {
constexpr int kDrawRows = 0x100;   // 0x00-0x5F exec, 0x60-0x8F opa, 0x90-0xBF xlu, 0xC0-0xC2 entry
constexpr int kDrawStages = 12;
uint64_t sDrawRowNs[kDrawRows];
uint32_t sDrawRowCalls[kDrawRows];
uint64_t sDrawStageNs[kDrawStages];
uint32_t sDrawTurns = 0;
int sDrawDepth = 0;
int64_t sDrawT0 = 0;
const char* const kDrawStageNames[kDrawStages] = {"draw", "scene", "seq", "mirror", "3d", "2d", "flush-anim", "flush-dl", "flush-view", "", "", ""};
}  // namespace
#endif
// row: (kind << 8) | index, kind 0 exec, 1 opa, 2 xlu, 3 entry (index 0 2D, 1 3D, 2 mirror). Game thread only.
extern "C" void port_draw_prof_begin(void) {
#if PORT_LOGGING
    if (sDrawDepth++ == 0) sDrawT0 = port_host_time_ns();
#endif
}
extern "C" void port_draw_prof_end(int row) {
#if PORT_LOGGING
    if (sDrawDepth <= 0 || --sDrawDepth != 0) return;
    const int kind = row >> 8, index = row & 0xFF;
    int slot = -1;
    if (kind == 0 && index < 0x60) slot = index;
    else if (kind == 1 && index < 0x30) slot = 0x60 + index;
    else if (kind == 2 && index < 0x30) slot = 0x90 + index;
    else if (kind == 3 && index < 3) slot = 0xC0 + index;
    if (slot < 0) return;
    const int64_t ns = port_host_time_ns() - sDrawT0;
    sDrawRowNs[slot] += ns > 0 ? static_cast<uint64_t>(ns) : 0;
    ++sDrawRowCalls[slot];
#else
    (void)row;
#endif
}
// stage: 0 draw, 1 scene, 2 seq, 3 mirror, 4 3d, 5 2d, 6-8 the flushes (anim, dl, view).
extern "C" void port_draw_prof_stage(int stage, int64_t ns) {
#if PORT_LOGGING
    if (stage >= 0 && stage < kDrawStages && ns > 0) sDrawStageNs[stage] += static_cast<uint64_t>(ns);
#else
    (void)stage; (void)ns;
#endif
}
// The end of a drawn turn (GameSystem::frameLoop).
extern "C" void port_draw_prof_turn(void) {
#if PORT_LOGGING
    if (!port_draw_prof_on() || ++sDrawTurns < 300) return;
    const double n = sDrawTurns;
    char line[512];
    int o = snprintf(line, sizeof line, "draw-prof: drawn=%u us/drawn:", sDrawTurns);
    for (int s = 0; s < kDrawStages && o < static_cast<int>(sizeof line) - 32; ++s) {
        if (*kDrawStageNames[s]) o += snprintf(line + o, sizeof line - o, " %s=%.1f", kDrawStageNames[s], sDrawStageNs[s] / 1000.0 / n);
    }
    uint64_t covered = 0;
    for (int r = 0; r < kDrawRows; ++r) covered += sDrawRowNs[r];
    if (o < static_cast<int>(sizeof line) - 32) snprintf(line + o, sizeof line - o, " rows=%.1f", covered / 1000.0 / n);
    port_log("%s", line);
    bool shown[kDrawRows] = {};
    for (int rank = 0; rank < 24; ++rank) {
        int best = -1;
        for (int r = 0; r < kDrawRows; ++r) {
            if (!shown[r] && sDrawRowCalls[r] && (best < 0 || sDrawRowNs[r] > sDrawRowNs[best])) best = r;
        }
        if (best < 0) break;
        shown[best] = true;
        const char* kind = best < 0x60 ? "exec" : best < 0x90 ? "opa" : best < 0xC0 ? "xlu" : "entry";
        const int index = best < 0x60 ? best : best < 0x90 ? best - 0x60 : best < 0xC0 ? best - 0x90 : best - 0xC0;
        port_log("draw-prof: #%d %s-%02X calls/drawn=%.2f us/drawn=%.1f us/call=%.1f", rank + 1, kind, index, sDrawRowCalls[best] / n,
                 sDrawRowNs[best] / 1000.0 / n, sDrawRowNs[best] / 1000.0 / sDrawRowCalls[best]);
    }
    memset(sDrawRowNs, 0, sizeof sDrawRowNs);
    memset(sDrawRowCalls, 0, sizeof sDrawRowCalls);
    memset(sDrawStageNs, 0, sizeof sDrawStageNs);
    sDrawTurns = 0;
#endif
}

// ---------------------------------------------------------------------------------------------------------------------------------
// hivelogic (6 Nov): exact update-scene cuts, and the timers that name the next ones.
//
// GXV_HL_SENSOR (default 1, exact): SensorHitChecker::movement tests its sensor pairs from compact per-group copies of the sensors
// that can take part (SensorHitChecker.cpp). 0: the original loops. 2: verify (both run every turn, the original's hit lists used).
extern "C" int port_hl_sensor_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_HL_SENSOR"); return e && *e == '0' ? 0 : e && *e == '2' ? 2 : 1; }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
// GXV_HL_AREACUBE (exact; 0 by default since r92, vita/vgx-game/entry.cpp, until a verify run passes): an area cube that follows
// nothing answers "outside" from a world box with margin before its full test (AreaForm.cpp). Read when a cube is set up (stage
// load). 0: every test is the full one. 2: verify (the box built, both answers computed at every call, the full one used).
extern "C" int port_hl_areacube_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_HL_AREACUBE"); return e && *e == '0' ? 0 : e && *e == '2' ? 2 : 1; }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}

// Verify modes (GXV_HL_AREACUBE=2, GXV_HL_SENSOR=2): counts, the first 8 mismatches of each kind logged with the area or actor name,
// and every 600 turns one line:
//   hl-verify: 600 turns: area-cube checks N (box said outside B), mismatches M, stale S (since start: ...); sensor turns T, sensors
//   compared C, mismatches M (since start: ...)
// A mismatch: the box said outside and the full test said inside (area), or a sensor's hit list differs from the original loops'
// (sensors). Stale: a cube's matrix or bounds changed after its box was built (a writer the audit missed; the box would be wrong).
// Every count must stay 0. Counts are relaxed atomics (isInVolume may run off the game thread).
namespace {
std::atomic<uint64_t> sHlAreaChecks{0}, sHlAreaOutside{0}, sHlAreaMis{0}, sHlAreaStale{0};
std::atomic<uint64_t> sHlSensTurns{0}, sHlSensCompared{0}, sHlSensMis{0};
std::atomic<int> sHlAreaLogged{0}, sHlStaleLogged{0}, sHlSensLogged{0};
uint64_t sHlLast[7];
uint32_t sHlTurns = 0;
}  // namespace
extern "C" void port_hl_verify_area(int outsideByBox, int insideByFull, int stale, const char* owner, const float* pos, const float* cubeTrans) {
    sHlAreaChecks.fetch_add(1, std::memory_order_relaxed);
    if (outsideByBox) sHlAreaOutside.fetch_add(1, std::memory_order_relaxed);
    if (stale) {
        sHlAreaStale.fetch_add(1, std::memory_order_relaxed);
        if (sHlStaleLogged.fetch_add(1, std::memory_order_relaxed) < 8) {
            port_log_summary("hl-verify: area-cube STALE \"%s\" at (%.1f, %.1f, %.1f): its matrix or bounds changed after its box was built",
                             owner ? owner : "?", cubeTrans[0], cubeTrans[1], cubeTrans[2]);
        }
    }
    if (outsideByBox && insideByFull) {
        sHlAreaMis.fetch_add(1, std::memory_order_relaxed);
        if (sHlAreaLogged.fetch_add(1, std::memory_order_relaxed) < 8) {
            port_log_summary("hl-verify: area-cube MISMATCH \"%s\" at (%.1f, %.1f, %.1f): point (%.3f, %.3f, %.3f) outside the box, inside by the full test",
                             owner ? owner : "?", cubeTrans[0], cubeTrans[1], cubeTrans[2], pos[0], pos[1], pos[2]);
        }
    }
}
extern "C" void port_hl_verify_sensor(unsigned compared, unsigned mismatched, const char* host, unsigned type, unsigned fastCount, unsigned origCount) {
    if (compared) {
        sHlSensTurns.fetch_add(1, std::memory_order_relaxed);
        sHlSensCompared.fetch_add(compared, std::memory_order_relaxed);
    }
    if (mismatched) {
        sHlSensMis.fetch_add(mismatched, std::memory_order_relaxed);
        if (sHlSensLogged.fetch_add(1, std::memory_order_relaxed) < 8) {
            port_log_summary("hl-verify: sensor MISMATCH host \"%s\" type 0x%X: %u hits by the copies, %u by the original loops",
                             host ? host : "?", type, fastCount, origCount);
        }
    }
}
static void HlVerifyTurn() {
    static const bool on = [] {
        const char* a = getenv("GXV_HL_AREACUBE");
        const char* b = getenv("GXV_HL_SENSOR");
        return (a && *a == '2') || (b && *b == '2');
    }();
    if (!on || ++sHlTurns < 600) return;
    const uint64_t now[7] = {sHlAreaChecks.load(std::memory_order_relaxed), sHlAreaOutside.load(std::memory_order_relaxed),
                             sHlAreaMis.load(std::memory_order_relaxed), sHlAreaStale.load(std::memory_order_relaxed),
                             sHlSensTurns.load(std::memory_order_relaxed), sHlSensCompared.load(std::memory_order_relaxed),
                             sHlSensMis.load(std::memory_order_relaxed)};
    port_log_summary("hl-verify: %u turns: area-cube checks %llu (box said outside %llu), mismatches %llu, stale %llu (since start: %llu checks, "
                     "%llu mismatches, %llu stale); sensor turns %llu, sensors compared %llu, mismatches %llu (since start: %llu compared, %llu mismatches)",
                     sHlTurns, (unsigned long long)(now[0] - sHlLast[0]), (unsigned long long)(now[1] - sHlLast[1]),
                     (unsigned long long)(now[2] - sHlLast[2]), (unsigned long long)(now[3] - sHlLast[3]), (unsigned long long)now[0],
                     (unsigned long long)now[2], (unsigned long long)now[3], (unsigned long long)(now[4] - sHlLast[4]),
                     (unsigned long long)(now[5] - sHlLast[5]), (unsigned long long)(now[6] - sHlLast[6]), (unsigned long long)now[5],
                     (unsigned long long)now[6]);
    memcpy(sHlLast, now, sizeof now);
    sHlTurns = 0;
}

// GXV_ANIM_PROF=1/2/3: the update-scene category timers (every build; lines in the basic log, log.txt).
//   2  every category list timed whole (CategoryList::execute marks it through port_anim_lod_set_category): two clock reads a list,
//      about fifty lists a turn, nothing per object. The cheapest way to see where update-scene goes.
//   3  2, plus the per-turn systems timed inclusive (port/move_prof.h PORT_MOVE_SYS: gravity queries, area lookups), about 1 us a
//      call more: read their shares, not the frame rate.
//   1  3, plus (logging builds) the per-object anim-prof rows as before.
// Every 600 turns (10 s at 60 turns a second), from the game thread:
//   move-prof: 600 turns (N drawn): update-scene lists X ms a turn, calc-anim lists Y, shadows Z; top: <list> ms (calls) ...
//   move-sys: a turn: gravity A ms (n calls), area lookups B ms (n calls)
// "update-scene lists" are the movement lists plus the collision objects' calcAnim lists run inside them (calc types 0-3); the
// shadow list (a movement type run in calc-anim) is shown on its own. Rows are game-thread wall time (preemption included).
// GXV_THREAD_CPU=1 (default 0; Vita): with the move-prof line, one "threads:" line per 10 s with the CPU each thread used by its core
// mask (vita/runtime/memory.cpp port_vita_thread_cpu_census): who else runs where the game thread runs.
extern "C" void port_vita_thread_cpu_census(void) __attribute__((weak));
int port_move_sys_flag = 0;
namespace {
struct MoveRow { uint64_t ns; uint32_t calls; };
MoveRow sMoveRows[128];            // [domain * 64 + type]: domain 0 calc-anim types, 1 movement types
int sMoveCur = -1;                 // the row being timed, -1 none
int64_t sMoveT0 = 0;
uint64_t sMoveSysNs[2];
uint32_t sMoveSysCalls[2];
int sMoveSysDepth[2];
int64_t sMoveSysT0[2];
uint32_t sMoveTurns = 0, sMoveDrawn = 0;
const char* const kMoveTypeNames[0x2E] = {"m00", "StopSceneDelay", "Camera", "ScreenEffect", "ClippingDirector", "SensorHitChecker",
    "MsgSharedGroup", "m07", "m08", "AudCameraWatcher", "TalkDirector", "DemoDirector", "m0C", "AreaObj", "Layout", "LayoutDecoration",
    "Movie", "WipeLayout", "LayoutOnPause", "MovieSubtitles", "m14", "m15", "MirrorCamera", "ImageEffect", "AudEffectDirector",
    "AudBgmConductor", "CameraCover", "SwitchWatcherHolder", "ClippedMapParts", "Planet", "CollisionMapObj", "CollisionEnemy",
    "CollisionDirector", "Environment", "MapObj", "MapObjDecoration", "Sky", "Player", "PlayerDecoration", "PlayerMessenger", "NPC",
    "Ride", "Enemy", "EnemyDecoration", "Item", "ShadowControllerHolder"};
const char* const kCalcTypeNames[4] = {"calc:ClippedMapParts", "calc:Planet", "calc:CollisionMapObj", "calc:CollisionEnemy"};
}  // namespace
static int MoveProfLevel() {
    // 0 off, 1 categories, 3 categories and systems
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_ANIM_PROF");
        if (!e || !*e) return 0;
        return *e == '2' ? 1 : (*e == '1' || *e == '3') ? 3 : 0;
    }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
static int MoveRowOf(int type) {
    if (type < 0) return -1;
    const int domain = type >= 0x100 ? 1 : 0, t = type & 0xFF;
    return t < 64 ? domain * 64 + t : -1;
}
static void MoveProfMark(int type) {
    if (!port_on_frame_thread()) return;
    const int64_t now = port_host_time_ns();
    if (sMoveCur >= 0) {  // (the list that ends, or a list that started inside another: the outer one is closed there)
        const int64_t ns = now - sMoveT0;
        sMoveRows[sMoveCur].ns += ns > 0 ? static_cast<uint64_t>(ns) : 0;
        ++sMoveRows[sMoveCur].calls;
        sMoveCur = -1;
    }
    const int row = MoveRowOf(type);
    if (row >= 0) {
        sMoveCur = row;
        sMoveT0 = now;
    }
}
extern "C" void port_move_sys_begin(int id) {
    if (id < 0 || id >= 2 || !port_on_frame_thread()) return;
    if (sMoveSysDepth[id]++ == 0) sMoveSysT0[id] = port_host_time_ns();
}
extern "C" void port_move_sys_end(int id) {
    if (id < 0 || id >= 2 || !port_on_frame_thread()) return;
    if (sMoveSysDepth[id] <= 0 || --sMoveSysDepth[id] != 0) return;
    const int64_t ns = port_host_time_ns() - sMoveSysT0[id];
    sMoveSysNs[id] += ns > 0 ? static_cast<uint64_t>(ns) : 0;
    ++sMoveSysCalls[id];
}
static void MoveProfTurn(int drawn) {
    const int level = MoveProfLevel();
    __atomic_store_n(&port_move_sys_flag, (level & 2) ? 1 : 0, __ATOMIC_RELAXED);
    static const bool threadCpu = [] { const char* e = getenv("GXV_THREAD_CPU"); return e && *e == '1'; }();
    if (!level && !threadCpu) return;
    ++sMoveTurns;
    if (drawn) ++sMoveDrawn;
    if (sMoveTurns < 600) return;
    const double turns = sMoveTurns;
    double scene = 0, calc = 0, shadow = 0;
    for (int r = 0; r < 128; ++r) {
        const double ms = sMoveRows[r].ns / 1e6 / turns;
        if (r == 64 + 0x2D) shadow += ms;
        else if (r >= 64 || r < 4) scene += ms;
        else calc += ms;
    }
    char line[640];
    int o = snprintf(line, sizeof line, "move-prof: %u turns (%u drawn): update-scene lists %.2f ms a turn, calc-anim lists %.2f, shadows %.2f; top:",
                     sMoveTurns, sMoveDrawn, scene, calc, shadow);
    bool shown[128] = {};
    for (int rank = 0; rank < 14 && o < static_cast<int>(sizeof line) - 48; ++rank) {
        int best = -1;
        for (int r = 0; r < 128; ++r) {
            if (r >= 4 && r < 64) continue;  // (the calc-anim phase's own lists: the calc-anim agent's; only their total is shown)
            if (!shown[r] && sMoveRows[r].calls && (best < 0 || sMoveRows[r].ns > sMoveRows[best].ns)) best = r;
        }
        if (best < 0 || sMoveRows[best].ns / 1e6 / turns < 0.01) break;
        shown[best] = true;
        const char* name = best < 4 ? kCalcTypeNames[best] : (best - 64) < 0x2E ? kMoveTypeNames[best - 64] : "?";
        o += snprintf(line + o, sizeof line - o, " %s %.2f (%.1f)", name, sMoveRows[best].ns / 1e6 / turns, sMoveRows[best].calls / turns);
    }
    if (level) port_log_summary("%s", line);
    if (level & 2) {
        port_log_summary("move-sys: a turn: gravity %.3f ms (%.1f calls), area lookups %.3f ms (%.1f calls) (inclusive, timers on: GXV_ANIM_PROF=%s)",
                         sMoveSysNs[0] / 1e6 / turns, sMoveSysCalls[0] / turns, sMoveSysNs[1] / 1e6 / turns, sMoveSysCalls[1] / turns,
                         getenv("GXV_ANIM_PROF") ? getenv("GXV_ANIM_PROF") : "?");
    }
    if (threadCpu && port_vita_thread_cpu_census) port_vita_thread_cpu_census();
    memset(sMoveRows, 0, sizeof sMoveRows);
    memset(sMoveSysNs, 0, sizeof sMoveSysNs);
    memset(sMoveSysCalls, 0, sizeof sMoveSysCalls);
    sMoveTurns = sMoveDrawn = 0;
}
