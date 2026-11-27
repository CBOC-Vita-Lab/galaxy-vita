// Frame-time statistics for the game thread and the renderer, logged every
// 10 s: always with PETARI_PERFLOG=1, otherwise only while the game runs
// below 58 fps (so the headset's log shows slowdowns).
#include <stdio.h>
#include "port/core3_budget.h"  // (core3-38)
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <mutex>
#include <string>

#include "port/par_prio.h"
#include "port/port.h"
#include "port/diagnostics.h"
#include "port/game_progress.h"
#include "port/vgx_coldtrace_control.h"
#include "port/frame_pace.h"
#include "port/pace25.h"
#include "port/even26.h"
#include "port/speed_guard.h"  // (speed30: GXV_SPEED_GUARD, the "speed:" census)
#include "port/late_turn.h"    // (pace32: GXV_LATE_TURN)
#include "port/lockstep.h"     // (fmt32: port_net_session_lockstep, the logic-changing features stand off in a multiplayer session)
#include "port/lockstep_gate.h"  // (MP3 checks83: no clock-driven logic decision in a lockstep session)
#include "port/hot_knob.h"
#include "port/stage_knob.h"  // (stagepreset25)
#include "port/ab27.h"        // (ab27: Frozen(), the governors stand still while the A/B driver measures)
#include "port/cpulod.h"  // (fe26: GXV_FE_GOV combines with the umbrella table)
#include "port/fegov.h"
#include "port/lockfade.h"  // (q30lock26: a governor / lock level change is walked in, not switched)
#include "port/q30lock.h"   // (q30lock26: GXV_Q30_LOCK, the Quality mode's escalation ladder)
#include "port/detail.h"
#include "port/popin.h"  // (popin24)
#include "port/load_split.h"  // (cook24)
#include "port/hitch25.h"  // (hitch25)
#include "port/mp_gate.h"   // MP3 lodgate: state-affecting speed features are held while a multiplayer session is active
#include "port/mp_bridge.h"  // MP3: mp_active()
#include "../../vgx/src/vgx_workload.h"
#include "mtx_profile.h"

#if defined(__vita__) && PORT_LOGGING
#include <errno.h>
#endif

// (declared here and in the vgx glue rather than in port/port.h, which everything includes)
extern "C" void port_io_play_begin(const char* stage, uint32_t key) __attribute__((weak));  // (disc26: dvd.cpp, the "io:" line)
extern "C" void port_io_play_end(void) __attribute__((weak));
// sampler30 (platform/src/port/sampler_vita.cpp, GXV_SAMPLER=1): the stage boundaries the sampler's window follows. Two relaxed loads when it is off.
extern "C" void port_sampler_stage_loading(const char* stage) __attribute__((weak));
extern "C" void port_sampler_stage_playing(const char* stage) __attribute__((weak));
extern "C" void port_io_tick(void) __attribute__((weak));
extern "C" int64_t port_thread_cpu_us(void);
extern "C" void port_perf_game_totals(int64_t* workNs, uint32_t* frames, int64_t* cpuUs, int64_t* maxWorkNs);
extern "C" void port_game_progress_report(const char* why);
// hitch30: the card-write queue (platform/src/os/os_alarm.cpp, port/card_queue.h): weak, so a host build without it writes as before.
extern "C" int port_card_write_async(const char* path, int mode, const void* data, unsigned bytes) __attribute__((weak));
extern "C" void port_card_write_flush(unsigned waitMs) __attribute__((weak));
// GXV_CARD_ASYNC=0|1 (default 1): the files written while the player may have control (the Quality lock's memory, the settings file of the pause menu) go through the
// queue; 0 writes them on the calling thread as before.
static bool CardAsyncOn() {
    static const bool on = [] { const char* e = getenv("GXV_CARD_ASYNC"); return !(e && *e == '0') && port_card_write_async != nullptr; }();
    return on;
}
// MP3 (checks83): true in a lockstep multiplayer session; port_net_session_lockstep() is declared in port/mp_bridge.h (lodgate; strong, C linkage).
static inline bool PortLockstepOn() { return port_net_session_lockstep() != 0; }
extern "C" int port_handheld(void);
extern "C" int port_frameskip_turn(int allowed);
extern "C" uint32_t port_frameskip_count(void);
extern "C" unsigned port_vgx_backend_pending(void) __attribute__((weak));  // vgx_galaxy.cpp
extern "C" unsigned port_vgx_pending_allowance(void) __attribute__((weak));  // vgx_galaxy.cpp (pace25)
extern "C" int port_vgx_pace_turn(int allowed) __attribute__((weak));  // -1: legacy cadence
extern "C" int port_hrw_plain60(void) __attribute__((weak));   // (gov5; vgx_galaxy.cpp: 1 while mode 5's governor keeps the pairs off)
extern "C" int port_hrw_even30_noassist(void) __attribute__((weak));   // (even60 with 60 Assist off; vgx_galaxy.cpp: 1 while even 30 is active)
extern "C" void port_vgx_present_turn(int drawn) __attribute__((weak));  // GXV_PRESENT_SYNC: the turn's vblank stamp (vgx_galaxy.cpp)

extern "C" int64_t port_vgx_player_cpu_us(void) __attribute__((weak));
extern "C" void port_vgx_diagnostic_csv_pause(int selecting) __attribute__((weak));
extern "C" void port_autoq_game_begin(void) __attribute__((weak));
extern "C" void port_autoq_game_work_done(void) __attribute__((weak));
extern "C" int port_autoq_stages(uint32_t* gpuUs, uint32_t* pipeUs, int* lowerable) __attribute__((weak));  // vgx_autoq.cpp (target 60)
extern "C" int port_screen_quiet(void) __attribute__((weak));  // the game: a pause menu or a closed fade
extern "C" uint32_t port_camera_cut_count(void) __attribute__((weak));  // (GXV_SEAMLESS: decomp LodCtrl.cpp, +1 at each jump of the stage camera)

#include "phase_cpu.h"
#include "port/par_exec.h"  // GXV_PAR_VIEW (par13)
#include "port/par_dl.h"  // GXV_PAR_DL (gamedraw23)
#include "port/par25.h"  // GXV_PAR25 (par25: the net-benefit ledger and guard of the parallel workers)
#if !defined(__vita__)
#include <pthread.h>  // (port_par_dl_self)
#endif

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

// par25 (12 Nov 2026): GXV_PAR25=0|1|2 (default 0: nothing below does anything; platform/include/port/par25.h has the design). The workers' own
// 5 s lines post their window (Par25Post: game-thread ms replaced, game-thread ms it cost, mismatches); the Judge decides per worker; the verdicts
// are applied at the start of a turn (Par25Turn, from port_perf_frame_begin) and the workers' switches read them (Par25Allowed); every 5 s one line.
namespace {
struct Par25Glue {
    port::par25::Gate gate;                                      // what the workers read
    port::par25::Judge judge[port::par25::kWorkers];
    port::par25::Summary last[port::par25::kWorkers];
    unsigned postTick[port::par25::kWorkers] = {};
    bool posted[port::par25::kWorkers] = {};
    bool pending[port::par25::kWorkers] = {true, true, true, true, true, true};
    int64_t t0 = 0;
    double turns = 0;
    unsigned tickNo = 0;
} sPar25;
int Par25Env(const char* name, int fallback, int lo, int hi) {
    const char* e = getenv(name);
    int v = e && *e ? static_cast<int>(strtol(e, nullptr, 0)) : fallback;
    return v < lo ? lo : (v > hi ? hi : v);
}
int Par25Mode() { return PORT_HOT_KNOB(int, Par25Env("GXV_PAR25", 0, 0, 2)); }
port::par25::Params Par25Params() {
    port::par25::Params p;
    p.negMsPerSec = Par25Env("GXV_PAR25_NEG_MS", 3, 0, 1000);
    p.negWindows = Par25Env("GXV_PAR25_NEG_WINDOWS", 2, 1, 100);
    p.probeTicks = Par25Env("GXV_PAR25_PROBE", 12, 1, 1000);
    p.goodWindows = Par25Env("GXV_PAR25_GOOD", 3, 1, 100);
    return p;
}
}  // namespace
// The workers' switches: false while the guard (mode 1) has the worker off. Always true in mode 0 and 2.
static inline bool Par25Allowed(int w) { return sPar25.gate.Allowed(w); }
static void Par25Post(int w, const port::par25::Summary& s) {
    const int mode = Par25Mode();
    if (mode == 0 || w < 0 || w >= port::par25::kWorkers) return;
    Par25Glue& g = sPar25;
    g.last[w] = s;
    g.posted[w] = true;
    g.postTick[w] = g.tickNo;
    const bool changed = g.judge[w].Window(s, Par25Params());
    if (changed) {
        port_log_summary("par25: %s %s%s (window net %+.2f ms/s over %.0f jobs, mismatches %u%s)", port::par25::Name(w),
                         g.judge[w].st == port::par25::kLatched ? "LATCHED OFF for the session" : "OFF",
                         mode == 2 ? " (would be: observe mode gates nothing)" : "", port::par25::NetPerSec(s), s.jobs, s.mismatches,
                         s.bound ? "; the net is an upper bound" : "");
    }
    if (mode == 1 && port::par25::Gated(w)) g.pending[w] = g.judge[w].Allowed();
}
static void Par25Turn(void) {
    const int mode = Par25Mode();
    if (mode == 0) return;
    Par25Glue& g = sPar25;
    g.turns += 1.0;
    for (int w = 0; w < port::par25::kWorkers; ++w) {
        if (port::par25::Gated(w) && g.gate.Allowed(w) != g.pending[w]) g.gate.Set(w, g.pending[w]);  // (a verdict takes effect between turns)
    }
    const int64_t now = port_host_time_ns();
    if (g.t0 == 0) {
        g.t0 = now;
        return;
    }
    if (now - g.t0 < 5000000000ll) return;
    const double secs = static_cast<double>(now - g.t0) / 1e9;
    ++g.tickNo;
    const port::par25::Params p = Par25Params();
    bool fresh[port::par25::kWorkers];
    for (int w = 0; w < port::par25::kWorkers; ++w) {
        if (g.judge[w].Tick(p)) {
            if (mode == 1 && port::par25::Gated(w)) g.pending[w] = true;
            port_log_summary("par25: %s ON again on probation (it was off %s)", port::par25::Name(w), mode == 2 ? "in the would-be verdict" : "for the backoff");
        }
        fresh[w] = g.posted[w] && g.tickNo - g.postTick[w] <= 2;
    }
    static unsigned sLine = 0;
    const int every = Par25Env("GXV_PAR25_LOG", 1, 1, 1000);
    if (++sLine % static_cast<unsigned>(every) == 0) {
        char buf[900];
        port::par25::Format(buf, sizeof buf, secs, g.turns, mode, g.last, g.judge, fresh);
        port_log_summary("%s", buf);
    }
    g.t0 = now;
    g.turns = 0.0;
}

static void LockTurnBegin();  // (q30lock26, below)
static void LockTurnDone();
extern "C" int port_ff_flyover(void);   // (far30: LodCtrl.cpp)
static void LockPump();
static void AntTurn();              // (anticip32: GXV_ANTICIPATE, below, before the governors' fader)
static void AntSpikePost(int src);  // (anticip32: a detector fired: any thread; port/anticipate.h Src)
static void LateTurnBegin();     // (pace32: GXV_LATE_TURN, below, after the turn profiler)
static void LateTurnWorkDone();
static uint32_t sLateSleptUs = 0;   // what this turn slept on purpose before its update (game thread only): the wall-time meters of the turn's work leave it out
// mariolag31 (GXV_HRW_MARIOTIME=2, vgx_galaxy.cpp port_hrw_turn_end): the same, for the fresh player's auto rule, which reads a pair's work, not its GXV_LATE_TURN sleep.
extern "C" unsigned port_late_turn_slept_us(void) { return sLateSleptUs; }
static void GameGovBegin();  // (cpu2-26: GXV_GAME_GOV, below)
static void GameGovEnd();
// ---- fmt32 (13 Nov 2026): the lockstep hook (platform/include/port/lockstep.h) ------------------------------------------------------------------------------------------------
// The default: no session. The multiplayer side defines the same function without "weak" and returns 1 while its synchronised session runs.
// PETARI_LOCKSTEP_TEST=1 (a test switch, default 0, read once): this default answers 1, so that a single-player run can show what a session switches off ("lockstep:" line).
extern "C" __attribute__((weak)) int port_net_session_lockstep(void) {
    static const int test = vgx::EnvInt("PETARI_LOCKSTEP_TEST", 0) != 0 ? 1 : 0;
    return test;
}
// The start of every game turn: the hook is asked once and the answer kept for the whole turn. One "lockstep:" line when it changes and every 10 s while on.
static void LockstepTurnBegin() {
    const int on = port_net_session_lockstep() != 0 ? 1 : 0;
    static int64_t lineNs = 0;
    static unsigned turns = 0;
    if (__builtin_expect(on == port::lockstep::gTurn && !on, 1)) return;
    const int64_t now = port_host_time_ns();
    if (on != port::lockstep::gTurn) {
        __atomic_store_n(&port::lockstep::gTurn, on, __ATOMIC_RELAXED);
        if (on)
            port_log_summary("lockstep: session ON (port_net_session_lockstep() = 1): held off from this turn: far logic / actor LOD / idle rest, the far and off-screen pose rules, the budget's "
                             "decoration clip, shadow clip and particle thinning, the early decoration clip, the clip pads, the plants' rest, the lean undrawn turns, the "
                             "late turn, the skip's cover hold, the draw helper");
        else
            port_log_summary("lockstep: session OFF after %u turns: the performance features decide for themselves again", turns);
        turns = 0;
        lineNs = now;
    }
    if (on) {
        ++turns;
        if (now - lineNs >= 10000000000ll) {
            port_log_summary("lockstep: on for %u turns so far (the logic-changing performance features are held off)", turns);
            lineNs = now;
        }
    }
}
void port_perf_frame_begin(void) {
    LockstepTurnBegin();  // (fmt32: first, so that everything this turn decides sees the same answer)
    if (port_io_tick) port_io_tick();  // (disc26) the running "io:" line of a long stage
    Par25Turn();  // (GXV_PAR25: the verdicts take effect at the start of a turn; the 5 s line)
    if (port_autoq_game_begin) port_autoq_game_begin(); // target-only CPU sample, also in the quiet player build
    GameGovBegin();
    AntTurn();        // (anticip32: an announced heavy moment opens / closes its window before the fader looks at the level it is asked for)
    LockPump();       // (q30lock26: the lock's / governors' level change is walked one tick a turn)
    LateTurnBegin();  // (pace32: the turn that ended tells GXV_LATE_TURN how much idle it had)
    LockTurnBegin();
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

static std::atomic<int> sTurnSkipped{0};
// q30-28: GameSystem::frameLoop clears sTurnSkipped BEFORE port_perf_frame_work_done() in an undrawn turn (other readers need it that way), so the work-done readers (GameGovEnd,
// LockTurnDone) saw every turn as drawn: the Quality ladder's "game pair" was one turn, not drawn + undrawn (Honeyhive: p95 30 ms read, 41.3 ms real). The undrawn branch now says so
// just before the clear; the flag is consumed by the work-done call of the same turn (game thread only).
static std::atomic<int> sBookUndrawn{0};
static bool sWorkUndrawn = false;   // this turn's work is booked as an undrawn turn (set at the top of port_perf_frame_work_done)
extern "C" void port_turn_booked_undrawn(void) { sBookUndrawn.store(1, std::memory_order_relaxed); }
static void SlipWorkDone();  // (below, with port_frameskip_turn)
static void SteadyWorkDone();
static void Pace25WorkDone();

void port_perf_frame_work_done(void) {
    sWorkUndrawn = sTurnSkipped.load(std::memory_order_relaxed) != 0 || sBookUndrawn.exchange(0, std::memory_order_relaxed) != 0;
    LateTurnWorkDone();  // (pace32)
    SlipWorkDone();  // GXV_SLIP: the turn's cost sample, taken before the retrace wait; nothing when off
    SteadyWorkDone();  // GXV_STEADY30: the same, for Performance mode's steady 30
    Pace25WorkDone();  // GXV_PACE25: the same, for the even cadence
#if PORT_PHASE_CPU_AVAILABLE
    port::phasecpu::End();
#endif
    GameGovEnd();
    LockTurnDone();
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
// that reads the field of view (the star pointer, screen positions, particles, the blur effect) sees the same view as the
// projection does. The game's logic does not (zoomclip23, GXV_ZOOM_LOGIC_VIEW): the object clipping judges with the game's own
// field of view (CameraContext::mPortFovyGame, MR::getFovyForLogic), so nothing the original keeps running is frozen by the zoom;
// the actors outside the zoomed picture are still not drawn (GXV_ZOOM_DRAWCULL), and near a Pull Star the zoom eases to 1.0
// (GXV_ZOOM_PULLSTAR).
//
// A user setting on a handheld: in the game's pause menu L lowers it and R raises it by 0.1 (platform/src/port/settings_panel.cpp draws the value under the menu,
// vita/runtime/input.cpp reads the buttons), and the choice is kept in <data folder>/settings.ini ("zoom = 1.5"; the folder
// that holds the saves' folder: ux0:data/smg). Where it comes from, in this order: that file; PETARI_ZOOM=Z in env.txt (the
// start value until a choice is made in the menu); 1.5 on a handheld (the maintainer: 1.15 at first, then "maybe 1.3", then on 17 Oct
// 17:11 "can we try 1.5", and at 17:15 "can that be a user scale ... 2.0 down to 1"), 1.0 elsewhere. It is held in tenths.
// A change shows when the game resumes (the camera is not updated while paused): CameraContext eases to it in 0.4 s.
// (file scope, not inside the anonymous namespace below: there g++ sees it as an overload of port.h's declaration and "if (port_input_set_camera_inverted)" fails to compile; tools/test_pace24.sh)
extern "C" void port_input_set_camera_inverted(int) __attribute__((weak));   // (weak: the host tests build this file without wpad.cpp)
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
int OptionFromFile(const char* key, int fallback = 0) {
    if (sZoomFile.empty()) return fallback;
    FILE* f = fopen(sZoomFile.c_str(), "r");
    if (!f) return fallback;
    int on = fallback;
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
        if (*p == '1') on = 1;
        else if (*p == '0') on = 0;   // (anything else leaves the default: a damaged line must not turn a default-on option off)
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

// settingsnative: "tap_jump = 1" (default ON): a short tap on the front screen also presses A where it lands, a finger held still holds A
// (vita/runtime/input.cpp reads it as `touchA && port_tap_jump_enabled()`; PETARI_TOUCH_A=0 still turns it off whatever this says);
// "invert_camera = 1" (default off): the camera's turns swapped (port_input_set_camera_inverted, platform/src/input/wpad.cpp; applied when the
// folder is given, at start). -1 = not read.
std::atomic<int> sTapJump{-1};
std::atomic<int> sInvertCamera{-1};
int TapJumpRead() {
    int v = sTapJump.load(std::memory_order_relaxed);
    if (v >= 0) return v;
    v = OptionFromFile("tap_jump", 1);
    int expected = -1;
    if (sTapJump.compare_exchange_strong(expected, v)) port_log_summary("tap to jump: %s", v ? "on" : "off (from the settings file)");
    return sTapJump.load();
}
int InvertCameraRead() {
    int v = sInvertCamera.load(std::memory_order_relaxed);
    if (v >= 0) return v;
    v = OptionFromFile("invert_camera", 0);
    int expected = -1;
    if (sInvertCamera.compare_exchange_strong(expected, v)) port_log_summary("invert camera: %s", v ? "on (from the settings file)" : "off");
    return sInvertCamera.load();
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

// The text of settings.ini (settingsnative: the keys of the native Settings' live rows, written whole every time).
int ZoomFileText(char* text, size_t size) {
    const int t = sZoomTenths.load();
    return snprintf(text, size,
                    "# Settings of the Vita port, kept by the game's Settings (Camera Zoom, Right Stick, Rear Touch, Tap to Jump, Invert Camera, Star Bit Reach;\n"
                    "# L and R in the pause menu set the zoom). Star Bit reach: 1 the game's own to 5 every Star Bit on screen.\n"
                    "zoom = %d.%d\nrear_aim = %d\nstick_pointer = %d\nstar_bit_reach = %d\ntap_jump = %d\ninvert_camera = %d\n",
                    t / 10, t % 10, RearAimRead(), StickPointerRead(), StarBitReachRead(), TapJumpRead(), InvertCameraRead());
}

void ZoomSave() {
    if (sZoomFile.empty()) return;
    if (CardAsyncOn()) {
        // hitch30: the pause menu's settings file went out on the game thread when the menu closed (create + remove + rename: 0.4-0.6 s of the
        // first turns after the player's resume). Now the text is queued (4 KiB slot) and the queue's thread does the temporary file + rename.
        char text[768];
        const int t = sZoomTenths.load();
        const int len = ZoomFileText(text, sizeof text);
        if (len > 0 && len < static_cast<int>(sizeof text) && port_card_write_async(sZoomFile.c_str(), 0, text, static_cast<unsigned>(len))) {
            sZoomSavedChange = sZoomChange.load();
            port_log_summary("zoom: %d.%d, rear touch aiming %s, right stick %s, Star Bit reach %d, queued for %s (card-write thread)", t / 10, t % 10,
                             RearAimRead() ? "on" : "off", StickPointerRead() ? "pointer" : "camera", StarBitReachRead(), sZoomFile.c_str());
            return;
        }
        // (queue full: sZoomSavedChange stays as it was, so the next turn of the menu asks again)
        return;
    }
    const std::string tmp = sZoomFile + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    const int t = sZoomTenths.load();
    char text[768];
    const int len = ZoomFileText(text, sizeof text);
    if (len > 0 && len < static_cast<int>(sizeof text)) fwrite(text, 1, static_cast<size_t>(len), f);
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
    sTapJump.store(-1);
    sInvertCamera.store(-1);
    if (port_input_set_camera_inverted) port_input_set_camera_inverted(InvertCameraRead());   // settingsnative: Invert Camera is applied at start
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

// settingsnative: Tap to Jump (Settings > Controls; saved with the zoom). 1 = on (the default). Elsewhere than the handhelds: always on (no touch screen
// reads it there).
extern "C" int port_tap_jump_enabled(void) { return TapJumpRead(); }
extern "C" int port_tap_jump_set(int on) {
    const int now = on ? 1 : 0;
    if (TapJumpRead() != now) { sTapJump.store(now); sZoomChange.fetch_add(1); }
    return now;
}
// settingsnative: Invert Camera (Settings > Camera; saved with the zoom, applied at start through port_input_set_camera_inverted).
extern "C" int port_invert_camera_enabled(void) { return InvertCameraRead(); }
extern "C" int port_invert_camera_set(int on) {
    const int now = on ? 1 : 0;
    if (InvertCameraRead() != now) { sInvertCamera.store(now); sZoomChange.fetch_add(1); }
    if (port_input_set_camera_inverted) port_input_set_camera_inverted(now);
    return now;
}
// settingsnative: Right Stick / Rear Touch / Star Bit Reach set to a value (the toggles above flip or step; these go straight to it).
extern "C" int port_rear_aim_set(int on) {
    const int now = on ? 1 : 0;
    if (RearAimRead() != now) { sRearAim.store(now); sZoomChange.fetch_add(1); }
    return now;
}
extern "C" int port_stick_pointer_set(int on) {
    const int now = on ? 1 : 0;
    if (StickPointerRead() != now) { sStickPointer.store(now); sZoomChange.fetch_add(1); }
    return now;
}
extern "C" int port_star_bit_reach_set(int level) {
    const int now = level < 1 ? 1 : (level > kStarBitReachLevels ? kStarBitReachLevels : level);
    if (StarBitReachRead() != now) { sStarBitReach.store(now); sZoomChange.fetch_add(1); }
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

// settingsnative: write settings.ini now if a choice is waiting (the restart path: a change otherwise waits for 45 idle turns of the menu).
extern "C" void port_zoom_save_now(void) {
    if (sZoomChange.load() != sZoomSavedChange) ZoomSave();
}

// settingsnative: the pause panel's Triangle asks for the native Settings (input thread sets, the menu takes it on the game thread when the pause
// menu waits for a choice and no session is running). The request is only made while the hint is shown (port_settings_hint_visible).
static std::atomic<int> sSettingsRequest{0};
extern "C" int port_settings_native_available(void) __attribute__((weak));   // the native menu's (vita/runtime or desktop host): Settings can open here
extern "C" int port_settings_hint_visible(void) {
    if (!port_handheld()) return 0;
    if (PortLockstepOn()) return 0;   // (a session, or its 30 s rejoin: the session pill opens Settings there, not Triangle)
    return port_settings_native_available && port_settings_native_available() ? 1 : 0;
}
extern "C" void port_settings_open_request(void) { sSettingsRequest.store(1, std::memory_order_release); }
extern "C" int port_settings_open_take(void) { return sSettingsRequest.exchange(0, std::memory_order_acq_rel); }

// mppause: the multiplayer session menu (the pause screen that does not pause the game) shows the same zoom row. It uses the same flag as
// the pause menu (L / R set the zoom, the game does not get them) and says so with this one: there Triangle (Players) and Select
// (Messages) stay the menu's own buttons (no Settings hint). `on` = the session menu's first screen is up.
static std::atomic<int> sZoomSession{0};
extern "C" int port_zoom_menu_active(void);
extern "C" void port_zoom_menu_session(int on) {
    sZoomSession.store(on ? 1 : 0, std::memory_order_relaxed);
    port_zoom_menu(on);
}
extern "C" int port_zoom_menu_session_active(void) { return sZoomSession.load(std::memory_order_relaxed) && port_zoom_menu_active(); }
// The session menu may show the zoom row where the pause menu would (the handhelds).
extern "C" int port_zoom_menu_capable(void) { return port_handheld() ? 1 : 0; }

// 1 while the pause menu waits for a choice and the zoom can be set with L and R.
extern "C" int port_zoom_menu_active(void) { return sZoomMenuSelecting.load(std::memory_order_relaxed) && port_handheld() ? 1 : 0; }

// The renderer's picture-size generation: the back end counts every change of its render size (vgx_backend_gl.cpp ApplyRung,
// the automatic quality's rungs, on the render thread); the game reads it (any thread). PETARI_PAUSE_FREEZE (GameScene.cpp)
// copies the 3D picture behind the pause menu again when it changes, so the copy always has the picture's size.
static std::atomic<uint32_t> sRenderSizeGen{0};
extern "C" void port_render_size_changed(void) { sRenderSizeGen.fetch_add(1, std::memory_order_relaxed); }
extern "C" uint32_t port_render_size_gen(void) { return sRenderSizeGen.load(std::memory_order_relaxed); }

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
// (stagepreset25, GXV_STAGE_PRESETS) the stage being loaded by name and scenario, for the renderer's per-stage preset table: written by the loading thread
// (port_scene_load_scenario, then port_scene_load_begin), read by the back end under a sequence count (odd while it is being written).
static std::atomic<uint32_t> sLoadingNameSeq{0};
static char sLoadingName[48] = "";
static std::atomic<int> sLoadingScenario{-1};
extern "C" void port_scene_load_scenario(int scenario) { sLoadingScenario.store(scenario, std::memory_order_relaxed); }
extern "C" int port_scene_loading_stage(char* name, int cap, int* scenario) {
    if (!name || cap < 2) return 0;
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t a = sLoadingNameSeq.load(std::memory_order_acquire);
        if (a & 1u) continue;
        char tmp[sizeof sLoadingName];
        memcpy(tmp, sLoadingName, sizeof tmp);
        const int sc = sLoadingScenario.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (sLoadingNameSeq.load(std::memory_order_relaxed) != a) continue;
        tmp[sizeof tmp - 1] = 0;
        snprintf(name, static_cast<size_t>(cap), "%s", tmp);
        if (scenario) *scenario = sc;
        return tmp[0] ? 1 : 0;
    }
    return 0;
}
// (cook24, port/load_split.h) the "load-split: scene build" line: what the loading thread's wall time went to. Counters read at the begin and at
// the end of initializeScene (both run on the loading thread, so its own CPU time is the difference of two port_thread_cpu_us reads).
extern "C" void port_load_split_disc(PortDiscSplit* out) __attribute__((weak));
extern "C" int port_load_split_slowest(char* name, size_t size, double* ms) __attribute__((weak));
extern "C" void port_load_split_finish_waits(uint64_t* count, uint64_t* ns) __attribute__((weak));
extern "C" int64_t port_thread_cpu_us(void);
extern "C" int port_load_split_on(void) {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_LOAD_SPLIT");
        return e && *e == '0' ? 0 : 1;
    }());
}
static PortDiscSplit sLsDisc0;
static int64_t sLsT0Ns = 0, sLsCpu0Us = 0;
static uint64_t sLsWaitN0 = 0, sLsWaitNs0 = 0;
static char sLsStage[64] = "";
static std::atomic<int64_t> sLsLastEndNs{0};
extern "C" double port_load_split_since_build_end_ms(void) {
    const int64_t at = sLsLastEndNs.load(std::memory_order_relaxed);
    return at ? static_cast<double>(port_host_time_ns() - at) / 1e6 : -1.0;
}
// (layoutwarm24) counts the scenes the game has begun to build (port_scene_load_begin): what a scene's objects (layouts) were made in is gone when it moves
static std::atomic<uint32_t> sSceneSerial{0};
extern "C" uint32_t port_scene_serial(void) { return sSceneSerial.load(std::memory_order_relaxed); }
extern "C" void port_scene_load_begin(const char* stage) {
    if (port_sampler_stage_loading) port_sampler_stage_loading(stage);  // (sampler30)
    if (port_io_play_end) port_io_play_end();  // (disc26) the previous stage's play window closes with its "io:" line
    sSceneSerial.fetch_add(1, std::memory_order_relaxed);
    {  // (stagepreset25: the name, under the sequence count; the key and the active flag follow it, so the renderer sees the name once it sees the load)
        const uint32_t q = sLoadingNameSeq.load(std::memory_order_relaxed);
        sLoadingNameSeq.store(q + 1, std::memory_order_release);
        snprintf(sLoadingName, sizeof sLoadingName, "%s", stage ? stage : "");
        std::atomic_thread_fence(std::memory_order_release);
        sLoadingNameSeq.store(q + 2, std::memory_order_release);
    }
    sLoadingKey.store(StageKeyOf(stage), std::memory_order_relaxed);
    sLoadingActive.store(1, std::memory_order_release);
    snprintf(sLsStage, sizeof sLsStage, "%s", stage ? stage : "");
    sLsT0Ns = port_host_time_ns();
    sLsCpu0Us = port_thread_cpu_us();
    memset(&sLsDisc0, 0, sizeof sLsDisc0);
    if (port_load_split_disc) port_load_split_disc(&sLsDisc0);
    sLsWaitN0 = sLsWaitNs0 = 0;
    if (port_load_split_finish_waits) port_load_split_finish_waits(&sLsWaitN0, &sLsWaitNs0);
    if (port_load_split_slowest) { char n[8]; double ms; port_load_split_slowest(n, sizeof n, &ms); }  // (the slowest read of this build only)
}
static void LoadSplitSceneLine() {
    if (!port_load_split_on() || !port_load_split_disc || sLsT0Ns == 0) return;
    const int64_t wallNs = port_host_time_ns() - sLsT0Ns;
    const double cpuMs = static_cast<double>(port_thread_cpu_us() - sLsCpu0Us) / 1000.0;
    PortDiscSplit d1;
    port_load_split_disc(&d1);
    uint64_t waitN = 0, waitNs = 0;
    if (port_load_split_finish_waits) port_load_split_finish_waits(&waitN, &waitNs);
    // the loading thread's own disc waits: the sync reads of the game's folders (the audio streams are read by their own thread)
    double blockedMs = 0;
    for (int c = 0; c < PORT_DISC_CLASSES; ++c)
        if (c != PORT_DISC_AUDIO) blockedMs += static_cast<double>(d1.blockedNs[c] - sLsDisc0.blockedNs[c]) / 1e6;
    const double wallMs = static_cast<double>(wallNs) / 1e6, finishMs = static_cast<double>(waitNs - sLsWaitNs0) / 1e6;
    const double other = wallMs - cpuMs - blockedMs - finishMs;
    char disc[700];
    port::loadsplit::DiscText(disc, sizeof disc, sLsDisc0, d1);
    char slow[200] = "none";
    double slowMs = 0;
    if (port_load_split_slowest && port_load_split_slowest(slow, sizeof slow, &slowMs)) {}
    port_log_summary("load-split: scene build of %s (key %08x): wall %.2f s on the loading thread = own CPU %.2f s + disc waits %.2f s + front-end finish waits %.2f s "
                     "(%llu) + other %.2f s (preempted by the game thread, locks, sleeps; negative = the CPU time includes work the disc waits overlap); %s "
                     "Yaz0/Yay0 archives expanded %llu (%.0f ms, %.1f MiB); slowest single read %.0f ms: %s",
                     sLsStage, StageKeyOf(sLsStage), wallMs / 1000.0, cpuMs / 1000.0, blockedMs / 1000.0, finishMs / 1000.0,
                     static_cast<unsigned long long>(waitN - sLsWaitN0), other / 1000.0, disc,
                     static_cast<unsigned long long>(d1.decodes - sLsDisc0.decodes), static_cast<double>(d1.decodeNs - sLsDisc0.decodeNs) / 1e6,
                     static_cast<double>(d1.decodeBytes - sLsDisc0.decodeBytes) / 1048576.0, slowMs, slow);
    sLsLastEndNs.store(port_host_time_ns(), std::memory_order_relaxed);
}
extern "C" int port_pb_budget_on(void);
extern "C" float port_pb_lod_scale(void);
extern "C" float port_pb_decor_dist(void);
extern "C" float port_pb_ptcl_factor(void);
extern "C" float port_pb_shadow_clip(void);
extern "C" float port_pb_anim_dist2(void);
extern "C" float port_pb_fur_dist(void);
// popin25 (GXV_POPIN_CENSUS): draws the renderer left out because their shader program was not ready (a skipped draw IS a pop: the back end's
// Draw, vgx_backend_gl.cpp, notes each one; relaxed, it is a count). The census takes and clears it.
static std::atomic<uint32_t> sPopinRenderSkips{0};
extern "C" void port_popin_render_skip_note(void) { sPopinRenderSkips.fetch_add(1, std::memory_order_relaxed); }
extern "C" uint32_t port_popin_render_skips_take(void) { return sPopinRenderSkips.exchange(0, std::memory_order_relaxed); }
extern "C" void port_scene_load_end(void) {
    sLoadingActive.store(0, std::memory_order_release);
    if (port_sampler_stage_playing) port_sampler_stage_playing(sLsStage);  // (sampler30)
    LoadSplitSceneLine();
    if (port_io_play_begin) port_io_play_begin(sLsStage, StageKeyOf(sLsStage));  // (disc26) this stage's play window opens
    // (the Performance-mode visual budget, once per stage load: what is in force; 0 / 1.0 = that part is off)
    port_log_summary("perf-budget: %s lod=%.2f decor=%.0f ptcl=%.2f shadow=%.0f anim=%.0f fur=%.0f", port_pb_budget_on() ? "ON" : "off",
                     port_pb_lod_scale(), port_pb_decor_dist(), port_pb_ptcl_factor(), port_pb_shadow_clip(), sqrtf(port_pb_anim_dist2()),
                     port_pb_fur_dist());
    // (popin25: the fly-over switches in force too)
    port_log_summary("popin25: GXV_POPIN_CENSUS=%d GXV_PB_DEMO=%d GXV_PB_DEMO_WIDEN=%.2f GXV_POPIN_LEAD=%.0f GXV_POPIN_LEAD_MAX=%.0f GXV_PB_DEMO_SPEED=%.0f GXV_PB_DEMO_FARCAM=%.0f",
                     port::popin::CensusOn(), port::popin::DemoMode(), static_cast<double>(port::popin::DemoWiden()),
                     static_cast<double>(port::popin::LeadTurns()), static_cast<double>(port::popin::LeadClipMax()),
                     static_cast<double>(port::popin::FlightSpeedMin()), static_cast<double>(port::popin::FlightFarCam()));
    // (popin31: the invisibility floor and the release after a flight)
    port_log_summary("popin31: GXV_PB_MINPX=%.1f GXV_PB_MINPX_BAND=%.0f%% GXV_PB_MINPX_PTCL=%.0f GXV_PB_DEMO_RAMP=%d GXV_PB_DEMO_QUIET=%d",
                     static_cast<double>(port::popin::MinPx()), static_cast<double>(port::popin::MinPxBand() * 100.0f),
                     static_cast<double>(port::popin::MinPxPtclRadius()), port::popin::ReleaseRampTurns(), port::popin::ReleaseQuietTurns());
    // (popin31b: each kind's floor; unset = GXV_PB_MINPX's)
    port_log_summary("popin31b: GXV_PB_MINPX_SHADOW=%.1f _DECOR=%.1f _FUR=%.1f _LOD=%.1f _SPECK=%.1f _STARBIT=%.1f _PARTICLE=%.1f",
                     static_cast<double>(port::popin::MinPxKind(port::popin::kFlShadow)), static_cast<double>(port::popin::MinPxKind(port::popin::kFlDecor)),
                     static_cast<double>(port::popin::MinPxKind(port::popin::kFlFur)), static_cast<double>(port::popin::MinPxKind(port::popin::kFlLod)),
                     static_cast<double>(port::popin::MinPxKind(port::popin::kFlSpeck)), static_cast<double>(port::popin::MinPxKind(port::popin::kFlStarBit)),
                     static_cast<double>(port::popin::MinPxKind(port::popin::kFlParticle)));
    // (detail25, platform/include/port/detail.h: the adaptive detail switches in force, once per stage load)
    port_log_summary("detail25: GXV_DETAIL_GOV=%d LEVEL=%d CYCLE=%.0f PLANT=%d FUR=%d BUDGET=%d HI=%.1f LO=%.1f DWELL=%.1f RELAX=%.1f FALLBACK=%d pictures HYST=%.0f%% FUR_FAR=%d FUR_GAIN=%.2f",
                     port::detail::GovOn(), port::detail::FixedLevel(), static_cast<double>(port::detail::CycleSec()), port::detail::PlantMode(), port::detail::FurMode(),
                     port::detail::BudgetOn(), static_cast<double>(port::detail::Hi()), static_cast<double>(port::detail::Lo()), port::detail::DwellMs() * 0.001,
                     port::detail::RelaxMs() * 0.001, port::detail::FallbackPics(), static_cast<double>(port::detail::HystPct()), port::detail::FurFarLayers(),
                     static_cast<double>(port::detail::FurGainExp()));
    // (popin24, platform/include/port/popin.h: the pop-in switches in force, once per stage load)
    port_log_summary("popin24: GXV_POPIN_LOG=%d GXV_POPIN_HYST=%.0f%% GXV_POPIN_ZOOM=%d GXV_POPIN_CLIPPAD=%.0f%%", port::popin::LogOn(),
                     static_cast< double >(port::popin::HystPct()), port::popin::ZoomMode(),
                     static_cast< double >((port::popin::ClipPadFactor() - 1.0f) * 100.0f));
}
extern "C" uint32_t port_scene_loading_key(void) { return sLoadingKey.load(std::memory_order_relaxed); }
// (GXV_STAGEWARM_MENU) the star select / galaxy map (ScenarioSelectScene::draw) marks each drawn turn; the renderer records its programs
// under the menu list while the mark is recent (a few frames: the back end runs a little behind the game thread).
static std::atomic<int64_t> sMenuLayerNs{0};
extern "C" void port_menu_layer_mark(void) { sMenuLayerNs.store(port_host_time_ns(), std::memory_order_relaxed); }
extern "C" int port_menu_layer_recent(void) {
    const int64_t at = sMenuLayerNs.load(std::memory_order_relaxed);
    return at != 0 && port_host_time_ns() - at < 150000000 ? 1 : 0;
}
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
// ---- Performance-mode visual budget (budget, 8 Nov): tasteful reductions that buy CPU, FE and GPU time per picture in heavy galaxies.
// GXV_PERF_BUDGET: 1 on, 0 off. Unset: on in Performance mode (GXV_AUTOQ_TARGET=60 and no GXV_FPS_LIMIT=30), off in Quality and Locked 30.
// Each part has its own switch, read once; an unset one takes the Performance value below while the umbrella is on and means "off"
// while it is off; a value set in env.txt wins over the umbrella (0 = this part off, so each can be A/B'd alone). Gameplay is never
// touched: no collision, no sensors, nothing near the player (the parts that cull test the PLAYER's distance too).
//   GXV_PB_LOD         0.6   LodCtrl: the distances to the Middle / Low models times this (1 or 0 = as the game)
//   GXV_PB_DECOR       5000  flower / bush groups (PlantGroup) farther than this from the camera are clipped like the game's own far clip (0 off)
//   GXV_PB_PTCL        0.5   emission-rate factor of particle emitters farther than GXV_PB_PTCL_DIST (2500) from the camera and 1500 from the player
//   GXV_PB_SHADOW      2800  the shadows' far clip (camera depth) for every galaxy (the game has one only in the hub: 4000); never within 1200 of the player
//   GXV_PB_ANIM        2000  far map objects / NPCs / decorations (no collision): joints computed every second turn beyond this distance
//                            from both the camera and the player (the ANIM_LOD set; GXV_PB_ANIM_PDIST moves the player's)
//   GXV_PB_FUR         2500  fur (planets, not the player's): every second layer beyond this camera distance, the tip layer always drawn
// q30-28: the Quality ladder's steps 3f (far flower / bush groups, far shadows) and 3g (LOD distances): the Performance budget's parts at their mildest, swept in over seconds (the weight 0..1
// is the ramp of port/lockfade.h; the sweep moves the far limit from "nothing" to the step's value, so groups / shadows / models leave one by one from the far end, none at once).
// 0 outside the lock or while the step is off. The static value (the umbrella or env) still applies: the stricter of the two wins.
extern "C" float port_q30lock_budget_w(int which);
static constexpr float kQ30DecorFar = 7000.0f, kQ30ShadowFar = 4000.0f, kQ30LodScale = 0.80f, kQ30SweepFrom = 30000.0f;
static int PbUmbrella() {
    long ov;
    if (port::stageknob::Overridden("GXV_PERF_BUDGET", &ov)) return ov != 0 ? 1 : 0;  // (stagepreset25: the stage's value first)
    const char* e = getenv("GXV_PERF_BUDGET");
    if (e && *e) return strtol(e, nullptr, 0) != 0 ? 1 : 0;
    const char* t = getenv("GXV_AUTOQ_TARGET");
    const char* l = getenv("GXV_FPS_LIMIT");
    return (t && *t && strtol(t, nullptr, 0) == 60 && !(l && *l && strtol(l, nullptr, 0) == 30)) ? 1 : 0;
}
// The value of switch `name`: the env value when set, else `perf` when the umbrella is on, else `off`.
static float PbValue(const char* name, float perf, float off) {
    float ov;
    if (port::stageknob::Lookup(name, &ov)) return ov;  // (stagepreset25)
    const char* e = getenv(name);
    if (e && *e) return static_cast<float>(atof(e));
    return PbUmbrella() ? perf : off;
}
extern "C" int port_pb_budget_on(void) { return PORT_STAGE_KNOB(int, PbUmbrella()); }
// cpu2-26 (12 Nov): GXV_PB_LOD / _DECOR / _FUR / _PTCL are members of the GXV_CPULOD table (port/cpulod.h, perfOnly rows) and follow the governors' level.
// PbLevelValue: the static part (cached with the knob): a member nobody set on its own takes the stricter of the budget's value and the row of the stage's /
// environment's level. PbGovern: the dynamic part (one relaxed load a call): the stricter of that and the row of the governors' level. Both act only where the budget
// is on (Performance mode), never loosen a value, and are the identity at level 0.
extern "C" int port_fegov_level(void);
static float PbLevelValue(const char* name, int kind, float base) {
    float ov;
    const char* e = getenv(name);
    if (port::stageknob::Lookup(name, &ov) || (e && *e)) return base;  // (set on its own: wins over the level)
    float t;
    if (PbUmbrella() && port::cpulod::TableValue(port::cpulod::Level(), name, &t)) return port::cpulod::PbStricter(kind, base, t);
    return base;
}
static bool LockBusy();      // (recover30: the governors' fader, below)
static int LockFrom();
static int LockTo();
static float LockFrac();
static inline float PbGovern(const char* name, int kind, float v) {
    const int g = port_fegov_level();
    float t;
    if (__builtin_expect(g > 0, 0) && port_pb_budget_on() && port::cpulod::TableValue(g, name, &t)) {
        float r = port::cpulod::PbStricter(kind, v, t);
        // recover30: while a level change is walked in, the value moves with the walk's progress, between the value of the level it left and the value of the level it goes to (a distance
        // sweep, far end first, both ways), and not to the stricter end at the walk's start (a pop) or, on a give-back, only at its end (another one).
        if (LockBusy()) {
            float x, tf = v, tt = v;
            const int from = LockFrom(), to = LockTo();
            if (from > 0 && port::cpulod::TableValue(from, name, &x)) tf = port::cpulod::PbStricter(kind, v, x);
            if (to > 0 && port::cpulod::TableValue(to, name, &x)) tt = port::cpulod::PbStricter(kind, v, x);
            const bool onF = kind == 0 ? (tf > 0.0f && tf < 1.0f) : tf > 0.0f, onT = kind == 0 ? (tt > 0.0f && tt < 1.0f) : tt > 0.0f;
            if (onF && onT) r = tf + (tt - tf) * LockFrac();
        }
        return r;
    }
    return v;
}
extern "C" float port_pb_lod_scale(void) {
    const float b = PORT_STAGE_KNOB(float, [] {
        const float v = PbLevelValue("GXV_PB_LOD", 0, PbValue("GXV_PB_LOD", 0.6f, 1.0f));
        return (v > 0.2f && v < 1.0f) ? v : 1.0f;  // (below 0.2 would show the Low model at once: treated as off)
    }());
    float g = PbGovern("GXV_PB_LOD", 0, b);
    g = (g > 0.2f && g < 1.0f) ? g : 1.0f;
    const float w = port_q30lock_budget_w(1);   // (q30-28, step 3g)
    if (w > 0.0f) { const float q = 1.0f + (kQ30LodScale - 1.0f) * w; if (q < g) g = q; }
    const float w3i = port_q30lock_budget_w(3);   // (far3, step 3i: the budget's own 0.6, swept in)
    if (w3i > 0.0f) { const float q = port::farfield::qfar::LodScale(w3i); if (q < g) g = q; }
    return g;
}
// MP3 lodgate: one log line per held feature per session; the gate sites call port_mpgate_note on their multiplayer branch only (single player never gets here), and
// port_mpgate_idle (a bool read a turn, ActorTurnLod.cpp's turn start) re-arms the lines once the session is over.
static port::mpgate::SessionLog sMpGateLog;
extern "C" void port_mpgate_note(int feature) {
    if (!sMpGateLog.Due(feature, true)) return;
    port_log_summary("mpgate: %s held (%s) while a multiplayer session is held (active, or reconnecting): %s; the stored setting is untouched and resumes after the session",
                     port::mpgate::Name(feature), port::mpgate::Neutral(feature), port::mpgate::Why(feature));
}
extern "C" void port_mpgate_idle(void) { if (!port_net_session_lockstep()) sMpGateLog.Reset(); }   // (held, not active: a reconnect is the same session, its lines are not said again)
extern "C" float port_pb_decor_dist(void) {
    if (port_net_session_lockstep()) {   // MP3 lodgate: no far-decor clip in a session (PB_DECOR, Quality 3f / 3j, GXV_FARFIELD tier 3 all read this): plant groups keep their sensors and payouts
        port_mpgate_note(port::mpgate::kDecor);
        return port::mpgate::DecorHorizon(0.0f, true);
    }
    if (port::lockstep::On()) return 0.0f;   // (fmt32, c181: clipping a far group stops its movement: the game's logic, decided by this console's camera, mode and governors; the same session flag, latched per turn)
    const float b = PORT_STAGE_KNOB(float, [] { const float v = PbLevelValue("GXV_PB_DECOR", 1, PbValue("GXV_PB_DECOR", 5000.0f, 0.0f)); return v >= 1500.0f ? v : 0.0f; }());
    const float g = PbGovern("GXV_PB_DECOR", 1, b);
    float d = g >= 1500.0f ? g : 0.0f;
    const float w = port_q30lock_budget_w(0);   // (q30-28, step 3f)
    if (w > 0.0f) { const float q = kQ30SweepFrom + (kQ30DecorFar - kQ30SweepFrom) * w; if (d <= 0.0f || q < d) d = q; }
    const float w3j = port_q30lock_budget_w(4);   // (far3, step 3j)
    if (w3j > 0.0f) { const float q = port::farfield::qfar::Sweep(port::farfield::qfar::kQDecor, w3j); if (d <= 0.0f || q < d) d = q; }
    return d;
}
extern "C" float port_pb_ptcl_factor(void) {
    if (port::lockstep::On()) return 1.0f;   // (fmt32: an emitter that makes fewer particles can end on another turn, and the logic may wait for its end)
    const float b = PORT_STAGE_KNOB(float, [] { const float v = PbLevelValue("GXV_PB_PTCL", 0, PbValue("GXV_PB_PTCL", 0.5f, 1.0f)); return (v > 0.0f && v < 1.0f) ? v : 1.0f; }());
    const float g = PbGovern("GXV_PB_PTCL", 0, b);
    return (g > 0.0f && g < 1.0f) ? g : 1.0f;
}
extern "C" float port_pb_ptcl_dist2(void) {
    return PORT_STAGE_KNOB(float, [] {
        const char* e = getenv("GXV_PB_PTCL_DIST");
        float d = e && *e ? static_cast<float>(atof(e)) : 2500.0f;
        port::stageknob::Lookup("GXV_PB_PTCL_DIST", &d);
        if (!(d >= 800.0f)) d = 800.0f;
        return d * d;
    }());
}
extern "C" float port_pb_shadow_clip(void) {
    if (port::lockstep::On()) return 0.0f;   // (fmt32: a shadow that is not projected has no drop point for the logic that reads it)
    float d = PORT_STAGE_KNOB(float, [] { const float v = PbValue("GXV_PB_SHADOW", 2800.0f, 0.0f); return v >= 1500.0f ? v : 0.0f; }());
    if (port::cpulod::FarOn()) {   // far30: the governors' shadow rows (never applied before) and the 25 m floor of the static clip (a preset's 22 m)
        if (port::cpulod::StaticOn() && d > 0.0f && d < port::farfield::kFloor) d = port::farfield::kFloor;
        d = PbGovern("GXV_PB_SHADOW", 1, d);
    }
    const float w = port_q30lock_budget_w(0);   // (q30-28, step 3f)
    if (w > 0.0f) { const float q = kQ30SweepFrom + (kQ30ShadowFar - kQ30SweepFrom) * w; if (d <= 0.0f || q < d) d = q; }
    return d;
}
static float PbAnimBase2() {   // (the static GXV_PB_ANIM distance squared, cached with the stage knobs)
    return PORT_STAGE_KNOB(float, [] {
        float v = PbValue("GXV_PB_ANIM", 2000.0f, 0.0f);
        if (port::cpulod::FarOn() && port::cpulod::StaticOn() && v >= 800.0f && v < port::farfield::kFloor) v = port::farfield::kFloor;   // (far30: the static 20 m / a preset's 15 m to 25 m)
        return v >= 800.0f ? v * v : 0.0f;
    }());
}
extern "C" float port_pb_anim_dist2(void) {
    const float b = PbAnimBase2();
    const float w = port_q30lock_budget_w(2);   // (far3, step 3h: far poses every 2nd turn beyond a horizon swept in; the camera's AND the player's distance are tested, so nothing near either is touched)
    if (w > 0.0f) { const float q = port::farfield::qfar::Sweep(port::farfield::qfar::kQRate, w); if (b <= 0.0f || q * q < b) return q * q; }
    return b;
}
extern "C" float port_pb_anim_pdist2(void) {
    const float p = PORT_STAGE_KNOB(float, [] {
        const char* e = getenv("GXV_PB_ANIM_PDIST");
        float d = e && *e ? static_cast<float>(atof(e)) : 0.0f;
        port::stageknob::Lookup("GXV_PB_ANIM_PDIST", &d);
        if (!(d >= 800.0f)) d = sqrtf(PbAnimBase2());
        return d * d;
    }());
    if (p > 0.0f) return p;
    const float w = port_q30lock_budget_w(2);   // (far3: no static player distance: the step's own)
    if (w > 0.0f) { const float q = port::farfield::qfar::Sweep(port::farfield::qfar::kQRate, w); return q * q; }
    return 0.0f;
}
extern "C" float port_pb_fur_dist(void) {
    const float b = PORT_STAGE_KNOB(float, [] { const float v = PbLevelValue("GXV_PB_FUR", 1, PbValue("GXV_PB_FUR", 2500.0f, 0.0f)); return v >= 1000.0f ? v : 0.0f; }());
    const float g = PbGovern("GXV_PB_FUR", 1, b);
    return g >= 1000.0f ? g : 0.0f;
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
// GXV_PAR_PRIO_ALL=<0x40..0xBF> (sched24; read at each worker's start, default unset): the priority of every par worker whose own GXV_PAR_*_PRIO is left
// at 0. The defaults are one step above the thread that starts the worker (the game thread's 0x9F: 0x9E, the front end's finishing thread's too, on the same
// fourth core) and, for par-anim, one below it (0xA0). At equal priority the kernel rotates threads by time slice: a worker in the middle of a job, which
// the game thread spins for (ForkJoin: par-ptcl; the claim of par-anim / par-view), can be rotated out by the finishing thread, which has work for ~5 ms
// of every picture ("par-ptcl: ... max 2.4-5.6 ms, 2-3 stalls > 1.5 ms" in each 5 s of the Honeyhive logs). A value under 0x9E puts every worker above the
// finishing thread; the finishing thread then waits for a worker's burst (well under a millisecond a picture) instead of the game thread waiting for the
// finishing thread's slice.
static int ParPrio(const char* own, bool core3 = false) {
    int above = 0;
#if defined(__vita__) && !defined(PORT_ARMHF_HARNESS)
    {  // (GXV_PAR_PRIO=1, parprio31: two steps above the calling thread, the game's; the finishing thread is one step above the game's. The workers start on the game thread.)
        const int mine = sceKernelGetThreadCurrentPriority();
        above = mine > 0x43 ? mine - 2 : 0;
    }
#endif
    const int mode = ParEnvInt("GXV_PAR_PRIO", 0, 0, 0xFF);
    return port::par::ResolvePrioAbove(ParEnvInt(own, 0, 0, 0xFF), ParEnvInt("GXV_PAR_PRIO_ALL", 0, 0, 0xFF), core3 && mode > 0 && mode < 0x40, above);
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
        const bool ok = sParExec.Start(core, ParPrio("GXV_PAR_PRIO"), ParEnvInt("GXV_PAR_SPIN_US", 40, 0, 2000));
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
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_ANIM", 0, 0, 1)) && Par25Allowed(port::par25::kAnim);  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h; GXV_PAR25=1: the guard's verdict)
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
        const bool ok = sParSpec.Start(core, ParPrio("GXV_PAR_ANIM_PRIO", core == 3));  // (thr24: par-anim is a core-3 worker too: GXV_PAR_PRIO=1 puts it above the finishing thread as it does the others; it was left at 0xA0, below it)
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
        const int prio = ParPrio("GXV_PAR_PRIO", ParEnvInt("GXV_PAR_CORE", 3, 1, 3) == 3);
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
    {
        // par25 (report only): the worker's busy time is an upper bound of the walk it replaced; the retire is the part of the cost that was measured
        port::par25::Summary s;
        s.secs = (now - w.t0) / 1e6;
        s.turns = p;
        s.jobs = static_cast<double>(byWorker);
        s.workerMs = busyUs / 1000.0;
        s.grossMs = busyUs / 1000.0;
        s.costMs = w.retireUs / 1000.0;
        s.bound = true;
        s.judged = false;
        Par25Post(port::par25::kView, s);
    }
    {  // (hitch30) the draw's wait for a view job the worker is in: bounded spin, then naps
        char sb[320];
        port::par::AsyncWaitCensus().Line("par-view async touch wait", sb, sizeof sb);
        port_log_summary("%s (GXV_BOUND_SPIN_US=%u, nap %u us)", sb, port::par::WaitBoundSpinUs(), port::par::WaitBoundNapUs());
    }
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
    uint64_t c[40] = {};  // (ModelManager.cpp's order: built, hit, miss, not ready, busy, saved us, hit us, build us, covered by par-anim,
                          // kept by the memo, not qualifying, quiet, verified, verify bad, deferred models; GXV_PAR_PLANT: plant jobs,
                          // built by the worker, reached first by the game thread, waits for a build, their us, left out as off-screen;
                          // parplant23 (21..): plants seen, covered by par-anim, quiet, no record, the worker's refusals (memo, not
                          // qualifying, Softimage, scratch full), early batches made, ended unused, jobs built in those, plant batches,
                          // the worker's first job (us sum, batches), the game thread's first plant (us sum, batches))
} sParFlushWin, sParLateWin;
}  // namespace
// GXV_PAR_ANIM_CB (par2; default 1 in the Vita player build): par-anim also takes models with a joint callback its walk performs
// itself (so far: NPCActor::calcJointScale, the NPCs' squash-and-stretch joint, e.g. every HoneyBee's "Center"); the callback's input
// (AnimScaleController::_C) and its controller are key words. Exact; 0: such models are computed as before.
// GXV_PAR_ANIM_WHY (game24, 10 Nov; default 0): a census of the models the game thread computes in full in calc-anim (ModelManager.cpp PwRecord): why, ms, top actors.
extern "C" int port_par_anim_why_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_PAR_ANIM_WHY"); return e && *e == '1' ? 1 : 0; }());  // (GXV_HOT_KNOBS)
}
extern "C" int port_par_anim_cb_on(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_ANIM_CB", 0, 0, 1));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
// GXV_PAR_LATE (par2; default 1 in the Vita player build; needs GXV_PAR_ANIM=1): calc-anim's own batch (ModelManager.cpp
// port_par_late_prepare), on GXV_PAR_FLUSH's worker; its 5 s line is "par-late: ..." (the par-flush line's fields; "deferred models"
// reads "candidates", "covered by par-anim" means par-anim's job had the same key).
extern "C" int port_par_late_on(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_LATE", 0, 0, 2));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
// GXV_PAR_LATE=2 (gamecpu23, 8 Nov): calc-anim's batch gets only the HEAVY models: those with KoteBug's posture callback (GXV_PAR_ANIM_CB2)
// and those with at least GXV_PAR_LATE_MIN joints + weight envelopes (default 40). r87's GXV_PAR_LATE=1 made a job of every listed model
// and lost 0.2 ms a calc-anim: a job's making and taking cost about what a small model's joints cost. A heavy model's joints (Honeyhive's
// KoteBug: 113 us a computation profiled) cost several times its job. Exact by par-anim's key rule, like GXV_PAR_LATE=1.
extern "C" unsigned port_par_late_min(void) {
    return PORT_HOT_KNOB(unsigned, static_cast<unsigned>(ParEnvInt("GXV_PAR_LATE_MIN", 40, 0, 100000)));
}
// GXV_PAR_ANIM_CB2 (gamecpu23, 8 Nov; default 0; needs GXV_PAR_ANIM_CB): par-anim also takes KoteBug's joint callback (KoteBug::calcJoint
// through JointControlDelegator< KoteBug >: the "Center" joint turned by the actor's posture quaternion); the posture and the controller
// are key words, the walk performs it (XanimeSpecWalk.hpp kind 2). With GXV_PAR_LATE=2 its base matrix and scale are predicted by
// KoteBug::calcAndSetBaseMtx's own arithmetic. Exact by the key rule; console check: GXV_PAR_ANIM_VERIFY=1, 0 mismatches.
extern "C" int port_par_anim_cb2_on(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_ANIM_CB2", 0, 0, 1));
}
extern "C" int port_par_flush_on(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_FLUSH", 0, 0, 1));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
// GXV_PAR_PLANT (revisit23 round 2, 8 Nov; default 0; needs GXV_PAR_ANIM=1; ModelManager.cpp port_par_flush_prepare / ParFlushItem /
// port_par_plant_own, port/par_plant.h): the flower groups' plants (PlantMember; GXV_ANIM_LOD_FREE defers every one of them to the drawn
// turn's flush) get flush jobs that the WORKER makes (key and walk inputs from the plant's live state) and walks, on GXV_PAR_FLUSH's worker
// (core 3), from the end of the flush list while the game thread waits for the front end and then computes from the start. The game thread
// only lists them (GXV_PAR_FLUSH made each job on the game thread: r87, 0.8 ms a picture of making and nothing saved). Each plant's
// computation takes the job only on par-anim's word-for-word key rule (exact, as GXV_PAR_FLUSH / GXV_PAR_ANIM); the game thread never
// writes a plant the worker is reading (a phase byte per job). Independent of GXV_PAR_FLUSH (which, if on, still makes the other deferred
// models' jobs on the game thread in the same batch).
//   GXV_PAR_PLANT=0 (default) off; 1 on; 2 on + verify: every plant hit also runs the original walk and compares the joint matrices,
//   scale flags, envelopes and the core's joint state with the worker's ("par-plant: verify MISMATCH ..." lines; the original's result is
//   kept). GXV_PAR_PLANT_PAUSE_US (default 2000): a wait for the worker's job longer than that pauses the mode for 120 pictures.
// Every 5 s (basic log): "par-plant: ..." (the par-flush line's fields plus the plant counts and the worker's share of its core).
extern "C" unsigned port_par_plant_wait_max(void) __attribute__((weak));  // (ModelManager.cpp: the longest wait since the last call)
static unsigned ParPlantWaitMax() { return port_par_plant_wait_max ? port_par_plant_wait_max() : 0u; }
extern "C" int port_par_plant_on(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_PLANT", 0, 0, 2));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
// GXV_PAR_PLANT_EARLY (parplant23, 8 Nov; default 1; acts only with GXV_PAR_PLANT): where the plant batch is listed. 0: at the drawn turn's
// start, right before the flush (2.3 round 2: on the console the worker never got there first, the flush reaches the plants within
// microseconds); 1: at the end of the turn before a drawn turn, as predicted from the last two turns (port/par_plant.h PlantTurns; a
// batch whose next turn does not draw is ended before that turn's update(), counted "ended unused"); 2: at the end of every turn.
extern "C" int port_par_plant_early_mode(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_PLANT_EARLY", 1, 0, 2));
}
// GXV_PAR_PLANT_TAKE (parplant23; default 1; acts only with GXV_PAR_PLANT): par-anim's batch (made on the game thread at the end of
// calc-anim) leaves out the plants deferred to the next flush; GXV_PAR_PLANT's worker makes their jobs instead. 1: only at the end of a
// turn that lists the early batch (GXV_PAR_PLANT_EARLY predicts a drawn turn next; otherwise par-anim keeps them, as a fallback if the
// next turn draws after all); 2: always (at 30 pictures a second par-anim's jobs of the drawn turn's end are never read: the skipped
// turn's update() moves the plants on before the flush). 0: par-anim keeps them (the par-plant line's "covered by par-anim").
extern "C" int port_par_plant_take_on(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_PLANT_TAKE", 1, 0, 2));
}
extern "C" int port_par_flush_run(void (*fn)(void*, unsigned), void* ctx, void* items, unsigned n) {
    static const bool started = [] {
        const int prio = ParPrio("GXV_PAR_FLUSH_PRIO", ParEnvInt("GXV_PAR_FLUSH_CORE", 3, 1, 3) == 3);
        const bool ok = sParFlushWorker.Start(ParEnvInt("GXV_PAR_FLUSH_CORE", 3, 1, 3), prio > 0 ? prio : -1, "smg_flush_spec");
        port_log_summary("par-flush: %s (GXV_PAR_FLUSH=%d, GXV_PAR_PLANT=%d%s; core %d, priority 0x%X)",
                         ok ? "worker started" : "the worker could not be started: the far models are computed as before", port_par_flush_on(),
                         port_par_plant_on(), port_par_anim_verify_on() ? ", VERIFY" : "", sParFlushWorker.Core(),
                         static_cast<unsigned>(sParFlushWorker.Priority()));
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
    for (unsigned i = 0; i < n && i < sizeof(w.c) / sizeof(w.c[0]); ++i) w.c[i] += counts[i];
    if (now - w.t0 < 5000000u) return;
    uint32_t workerUs = 0, workerJobs = 0, sleeps = 0;
    sParFlushWorker.TakeStats(&workerUs, &workerJobs, &sleeps);  // (both batches' worker time since the last line of either)
    if (!late && port_par_plant_on()) {
        // (GXV_PAR_PLANT) saved = the worker's walks the game thread took minus its time taking them; the plants' making is the worker's
        const double p = w.pictures ? static_cast<double>(w.pictures) : 1.0;
        const double secs = (now - w.t0) / 1e6;
        port_log_summary("par-plant: %.1f s, %u pictures; a picture: plants listed %.1f (left out as off-screen %.1f, not qualifying %.1f), "
                         "made by the worker %.1f, reached first by the game thread %.1f; hits %.1f, misses %.1f, not ready %.1f, busy %.2f; "
                         "game thread saved %.3f ms (walks %.3f - takes %.3f), listing+other jobs %.3f ms, waits for a job %.2f (%.1f us, max %u us); "
                         "worker %.3f ms (%.1f %% of its core, %u jobs); verify (GXV_PAR_PLANT=2): %llu checked, %llu mismatches",
                         secs, w.pictures, w.c[15] / p, w.c[20] / p, w.c[10] / p, w.c[16] / p, w.c[17] / p, w.c[1] / p, w.c[2] / p, w.c[3] / p,
                         w.c[4] / p, (static_cast<double>(w.c[5]) - static_cast<double>(w.c[6])) / p / 1000.0, w.c[5] / p / 1000.0,
                         w.c[6] / p / 1000.0, w.c[7] / p / 1000.0, w.c[18] / p, w.c[18] ? static_cast<double>(w.c[19]) / w.c[18] : 0.0,
                         ParPlantWaitMax(), workerUs / p / 1000.0, secs > 0 ? workerUs / (secs * 1e4) : 0.0, workerJobs,
                         static_cast<unsigned long long>(w.c[12]), static_cast<unsigned long long>(w.c[13]));
        {  // (hitch30) the game thread's wait for a plant job the worker is building: bounded spin, then naps
            char sb[320];
            port::par::PlantWaitCensus().Line("par-plant build wait", sb, sizeof sb);
            port_log_summary("%s (GXV_BOUND_SPIN_US=%u, nap %u us)", sb, port::par::WaitBoundSpinUs(), port::par::WaitBoundNapUs());
        }
        // (parplant23) where the plants went, why the worker made no job, the early batches and the head start the worker had
        port_log_summary("par-plant: a picture: plants seen %.1f (covered by par-anim %.1f, quiet %.1f; deferred models %.1f, no record %.1f); "
                         "the worker's refusals: kept by the memo %.1f, not qualifying %.1f, Softimage init %.1f, scratch full %.1f; "
                         "early batches (GXV_PAR_PLANT_EARLY=%d): used %.2f, ended unused %.2f (%.1f jobs built for nothing); plant batches %.2f: "
                         "the worker's first job %.0f us after the listing (in %.0f %% of them), the game thread's first plant %.0f us after it",
                         w.c[21] / p, w.c[22] / p, w.c[23] / p, w.c[14] / p, w.c[24] / p, w.c[25] / p, w.c[26] / p, w.c[27] / p, w.c[28] / p,
                         port_par_plant_early_mode(), w.c[29] / p, w.c[30] / p, w.c[31] / p, w.c[32] / p,
                         w.c[34] ? static_cast<double>(w.c[33]) / w.c[34] : -1.0, w.c[32] ? 100.0 * w.c[34] / w.c[32] : 0.0,
                         w.c[36] ? static_cast<double>(w.c[35]) / w.c[36] : -1.0);
    }
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
    if (!late && (port_par_plant_on() || port_par_flush_on())) {
        // par25 (report only): saved = the walks the game thread took minus its copies and the making of the jobs it made
        port::par25::Summary s;
        s.secs = (now - w.t0) / 1e6;
        s.turns = static_cast<double>(w.pictures);
        s.jobs = workerJobs;
        s.workerMs = workerUs / 1000.0;
        s.grossMs = w.c[5] / 1000.0;
        s.costMs = (static_cast<double>(w.c[6]) + static_cast<double>(w.c[7])) / 1000.0;
        s.mismatches = static_cast<unsigned>(w.c[13]);
        s.judged = false;
        Par25Post(port_par_plant_on() ? port::par25::kPlant : port::par25::kFlush, s);
    }
    w = ParFlushWindow{};
    w.t0 = now;
}
// GXV_PAR_DL (gamedraw23, 8 Nov; default 0; decomp/src/Game/LiveActor/ModelManager.cpp "GXV_PAR_DL", port/par_dl.h, GameSystem::frameLoop):
// the drawn turn's material lists (port_dl_flush_stale) are made by a worker on the fourth core while the game thread makes the flush's view
// pass (port_view_flush, all but the player's); the game thread joins it before the draw and merges j3dSys. Exact (the proof is the comment
// in ModelManager.cpp). 0 off; 1 on; 2 on + verify (the serial flush again from the saved material state, compared entry by entry; the
// game keeps the serial result; slow).
//   GXV_PAR_DL_VIEW (default 1; acts only with GXV_PAR_DL): a drawn turn's calc-anim leaves its view pass (but the player's) to the flush,
//                       as a turn without a picture does, so there is a view pass to overlap at 60 pictures a second too.
//   GXV_PAR_DL_PLAYER (default 1): see port_par_dl_player_on below (the player's view pass after the join when the pending pass includes it).
//   GXV_PAR_DL_HELP (default 1): while the game thread waits at the join it runs the view jobs (GXV_PAR_VIEW=2) the view worker, on the
//                       same fourth core, has not reached (from the end): idle time put to use, so the draw's touches find them done.
//   GXV_PAR_DL_CORE (default 3: the fourth core when proven, else 1), GXV_PAR_DL_PRIO=0xNN (default one step above the game thread:
//                       the game thread waits for it at the join), GXV_PAR_DL_SPIN_US (default 3000: the join's spin before it sleeps).
// Every 5 s (basic log): "par-dl: ..." (below).
namespace {
port::par::OneShot& sParDlWorker = *new port::par::OneShot;  // (never destroyed: its worker waits on it until the process ends)
struct ParDlWindow {
    uint64_t t0 = 0;
    uint64_t c[21] = {};
    uint32_t turns = 0;
} sParDlWin;
}  // namespace
static int ParDlRawMode(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_DL", 0, 0, 2));  // (GXV_HOT_KNOBS)
}
extern "C" int port_par_dl_mode(void) {
    return Par25Allowed(port::par25::kDl) ? ParDlRawMode() : 0;  // (GXV_PAR25=1: the guard's verdict; between two drawn turns only)
}
// GXV_DRAWPIPE (drawpipe24, 10 Nov; default 0; platform/include/port/draw_pipe.h, decomp/src/Game/System/DrawBufferHolder.cpp,
// decomp/src/Game/Scene/SceneFunction.cpp port_drawpipe_point, GameSystem::frameLoop): the first pieces of a pipelined game thread.
// 0 off; 1 the J3D passes draw from the lists frozen at the hand-over (the draw itself stays on the game thread); 2 the census /
// verify run (pictures unchanged): what the J3D passes read is hashed at four points of one drawn turn in GXV_DRAWPIPE_SAMPLE
// (default 4) and the "drawpipe:" lines say which classes of it the draw, update, calc-anim and the flushes wrote.
// dpengine (11 Nov; platform/include/port/draw_thread.h): 3 the whole serial draw on the draw thread while the game thread waits at
// the hand-over (no gain: the thread, the GX owner hand-off and the guest-OS rules proven on the console); 4 stage A: the game
// thread records the draw's GX calls (port/gx_capture.h), the draw thread executes the recording and draws the J3D passes from
// the frozen frame while the game thread runs update(), joined before calcAnim(); 1 is now mode 4's data flow rehearsed on the
// game thread. 5 (stage B) is not built: it runs as 4 and says so. Every turn that cannot be pipelined is today's serial turn,
// counted by reason in the "drawpipe:" lines.
//   GXV_DRAWPIPE_CORE (default 1)        the draw thread's core, 0-3 (3 only with the fourth core proven, else 1)
//   GXV_DRAWPIPE_PRIO (default 0)        its priority: 0 = one step below the game thread's (0xA0 under the back end's render
//                                        thread and the game's audio thread at 0x9F, above the 0xBF background work); else
//                                        0x40..0xBF as given (0x9E = above the back end: an A/B, not the default, because the
//                                        back end's work is the previous picture's and must not be delayed by this one's)
//   GXV_DRAWPIPE_WATCHDOG_MS (200)       a join that waits longer logs once and turns the pipeline off for the session
//   GXV_DRAWPIPE_SLICE_US (1000)         the join's timed wait between two guest safe points
//   GXV_DRAWPIPE_SETTLE (8)              turns that must be pipelinable in a row before the first is pipelined
//   GXV_DRAWPIPE_STACK_KB (1024)         the draw thread's stack
//   GXV_DRAWPIPE_LOADCHECK (1)           1 = a turn is serial when a guest thread other than the main and the audio thread ran since
//                                        the last hand-over (a loading thread at work); 0 = that test off (only if the log's
//                                        "another guest thread ran" count shows a thread that runs in ordinary play)
//   GXV_DRAWPIPE_ALWAYS (0)              mode 3 only: 1 = every drawn turn's draw goes to the draw thread, whatever the state
//                                        (loading screens, scene changes, the pause menu, HOME, cutscenes): the second mode-3
//                                        run, for the boundaries the fall-back states otherwise keep away from the thread
extern "C" int port_pipe25_mode(void);
// (pipe27) GXV_PIPE25 > 0 with GXV_DRAWPIPE unset: the drawpipe's plumbing (the freeze at the hand-over, the turn's decision, the
// join) runs as mode 4, and decomp/src/Game/Scene/SceneFunction.cpp sends the draw to the late-draw engine instead of the capture
// layer's. GXV_DRAWPIPE set (1-5) wins: GXV_PIPE25 is then off (port_pipe25_active), with a line from the engine.
extern "C" int port_drawpipe_mode(void) {
    return PORT_HOT_KNOB(int, [] {
        const int m = ParEnvInt("GXV_DRAWPIPE", 0, 0, 5);
        return m != 0 ? m : (port_pipe25_mode() != 0 ? 4 : 0);
    }());  // (GXV_HOT_KNOBS)
}
extern "C" int port_pipe25_active(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_DRAWPIPE", 0, 0, 5) == 0 ? port_pipe25_mode() : 0);
}
extern "C" int port_drawpipe_core(void) { return PORT_HOT_KNOB(int, ParEnvInt("GXV_DRAWPIPE_CORE", 1, 0, 3)); }
extern "C" int port_drawpipe_prio(void) {
    return PORT_HOT_KNOB(int, [] {
        const int v = ParEnvInt("GXV_DRAWPIPE_PRIO", 0, 0, 0xBF);
        return v != 0 && v < 0x40 ? 0 : v;
    }());
}
extern "C" int port_drawpipe_watchdog_ms(void) { return PORT_HOT_KNOB(int, ParEnvInt("GXV_DRAWPIPE_WATCHDOG_MS", 200, 20, 60000)); }
extern "C" int port_drawpipe_slice_us(void) { return PORT_HOT_KNOB(int, ParEnvInt("GXV_DRAWPIPE_SLICE_US", 1000, 200, 20000)); }
extern "C" int port_drawpipe_settle(void) { return PORT_HOT_KNOB(int, ParEnvInt("GXV_DRAWPIPE_SETTLE", 8, 0, 3600)); }
extern "C" int port_drawpipe_stack_kb(void) { return PORT_HOT_KNOB(int, ParEnvInt("GXV_DRAWPIPE_STACK_KB", 1024, 256, 8192)); }
// GXV_PIPE25 (pipe25, 12 Nov; default 0; platform/include/port/pipe25.h): the late draw. 1 = the J3D walk of picture N on a worker beside turn
// N+1's update / calc from a snapshot; 2 = VERIFY (the walk on the game thread as today and on the worker, segments compared byte for byte,
// mismatches and fence violations counted). pipe27: wired (SceneFunction.cpp, DrawBufferHolder.cpp, port/draw_thread.h class Late).
//   GXV_PIPE25_CORE (1)           the helper's core, 0-3 (3 only with the fourth core proven, else 1)
//   GXV_PIPE25_PRIO (0)           its priority: 0 = the game thread's own (the back end's render thread and the audio thread sit there too);
//                                 0x40-0xBF explicit (0xA0 starved the draw thread of GXV_DRAWPIPE=4)
//   GXV_PIPE25_JOINLATE (0)       1 = the join after calcAnim(), not before it (pipe61): the overlap is update + calc-anim; see SceneFunction.cpp port_drawpipe_join_late
//   GXV_PIPE25_TAKEOVER_US (1500) a join that has waited this long makes the helper stop at its next pass boundary; the game thread does the rest
//   GXV_PIPE25_STACK_KB (1024)    the helper's stack
//   GXV_PIPE25_SCRATCH_KB (1024)  verify only: the buffer a pass is re-run into (a larger pass is left out of the compare, counted)
extern "C" int port_pipe25_mode(void) { return PORT_HOT_KNOB(int, ParEnvInt("GXV_PIPE25", 0, 0, 2)); }
extern "C" int port_pipe25_core(void) { return PORT_HOT_KNOB(int, ParEnvInt("GXV_PIPE25_CORE", 1, 0, 3)); }
extern "C" int port_pipe25_prio(void) {
    return PORT_HOT_KNOB(int, [] {
        const int v = ParEnvInt("GXV_PIPE25_PRIO", 0, 0, 0xBF);
        return v != 0 && v < 0x40 ? 0 : v;
    }());
}
// GXV_PIPE25_JOINLATE (pipe61, default 0): 1 = the helper is joined after calcAnim() (GameSystem::frameLoop), so that update() and calcAnim() both run beside it.
extern "C" int port_pipe25_joinlate(void) { return PORT_HOT_KNOB(int, ParEnvInt("GXV_PIPE25_JOINLATE", 0, 0, 1)); }
extern "C" int port_pipe25_takeover_us(void) { return PORT_HOT_KNOB(int, ParEnvInt("GXV_PIPE25_TAKEOVER_US", 1500, 200, 1000000)); }
extern "C" int port_pipe25_stack_kb(void) { return PORT_HOT_KNOB(int, ParEnvInt("GXV_PIPE25_STACK_KB", 1024, 256, 8192)); }
extern "C" int port_pipe25_scratch_kb(void) { return PORT_HOT_KNOB(int, ParEnvInt("GXV_PIPE25_SCRATCH_KB", 1024, 64, 16384)); }
extern "C" int port_drawpipe_always(void) { return PORT_HOT_KNOB(int, ParEnvInt("GXV_DRAWPIPE_ALWAYS", 0, 0, 1)); }
extern "C" int port_drawpipe_loadcheck(void) { return PORT_HOT_KNOB(int, ParEnvInt("GXV_DRAWPIPE_LOADCHECK", 1, 0, 1)); }
// (mode 4: during an overlap every hooked GX entry point asks the pipeline first, so that a GX call by the logic joins the draw
// thread before it runs. Needs the capture layer's owner trap, GX-HOOKS.diff; nothing without it. 0: not set.)
extern "C" int port_drawpipe_trap(void) { return PORT_HOT_KNOB(int, ParEnvInt("GXV_DRAWPIPE_TRAP", 1, 0, 1)); }
extern "C" int port_drawpipe_sample(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_DRAWPIPE_SAMPLE", 4, 1, 64));
}
extern "C" int port_par_dl_defer_view(void) {
    return PORT_HOT_KNOB(int, port_par_dl_mode() != 0 && ParEnvInt("GXV_PAR_DL_VIEW", 1, 0, 1) != 0 ? 1 : 0);
}
extern "C" int port_par_dl_start(void) {
    static const bool started = [] {
        const int prio = ParPrio("GXV_PAR_DL_PRIO", ParEnvInt("GXV_PAR_DL_CORE", 3, 1, 3) == 3);
        const bool ok = sParDlWorker.Start(ParEnvInt("GXV_PAR_DL_CORE", 3, 1, 3), prio > 0 ? prio : -1, "smg_dl_flush");
        port_log_summary("par-dl: %s (GXV_PAR_DL=%d, GXV_PAR_DL_VIEW=%d; core %d, priority 0x%X)",
                         ok ? "worker started" : "the worker could not be started: the material lists are made as before", port_par_dl_mode(),
                         port_par_dl_defer_view(), sParDlWorker.Core(), static_cast<unsigned>(sParDlWorker.Priority()));
        return ok;
    }();
    return started ? 1 : 0;
}
extern "C" void port_par_dl_dispatch(void (*fn)(void*), void* ctx) { sParDlWorker.Dispatch(fn, ctx); }
extern "C" int port_par_dl_take(void) { return sParDlWorker.Take() ? 1 : 0; }
extern "C" void port_par_dl_wait(unsigned* waitUs) {
    static const uint32_t spin = static_cast<uint32_t>(ParEnvInt("GXV_PAR_DL_SPIN_US", 3000, 0, 100000));
    uint32_t us = 0;
    sParDlWorker.Wait(spin, &us);
    *waitUs = us;
}
extern "C" void port_par_dl_reset(void) { sParDlWorker.Reset(); }
extern "C" int port_par_dl_done(void) { return sParDlWorker.Done() ? 1 : 0; }
// The worker's stack, [*lo, *hi) (0, 0 until its thread runs): ModelManager.cpp hands it to J3DSys.hpp's PortDlGameEnter.
extern "C" void port_par_dl_stack(unsigned long* lo, unsigned long* hi) {
    const uintptr_t top = sParDlWorker.StackTop();
    *lo = top ? static_cast<unsigned long>(top - port::par::OneShot::kStackSpan) : 0;
    *hi = top ? static_cast<unsigned long>(top) : 0;
}
// GXV_PAR_DL_PLAYER (round 2; default 1; acts only with GXV_PAR_DL): when the flush's pending view pass includes the player's (draw-done
// modes 3 / 4 with the front end behind at calc-anim: Settings' "Logic overlap: Max", the default), the flush overlaps the material lists
// with the rest and makes the player's part after the join. 0: such turns stay serial.
extern "C" int port_par_dl_player_on(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_DL_PLAYER", 1, 0, 1));
}
extern "C" int port_par_dl_help_on(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_DL_HELP", 1, 0, 1));
}
// This thread's id, never 0 (ModelManager.cpp: who runs the job, port_par_dl_game_enter). The kernel's thread id on the console (the call
// GXV_LC_FRAMETID uses for the frame thread); elsewhere a hash of the pthread id.
extern "C" unsigned port_par_dl_self(void) {
#if defined(__vita__)
    return static_cast<unsigned>(sceKernelGetThreadId()) | 0x80000000u;
#else
    const pthread_t t = pthread_self();
    unsigned h = 2166136261u;
    const unsigned char* b = reinterpret_cast<const unsigned char*>(&t);
    for (size_t i = 0; i < sizeof(t); ++i) h = (h ^ b[i]) * 16777619u;
    return h | 1u;
#endif
}
// One drawn turn's record (ModelManager.cpp port_par_dl_begin / _end: kParDlRecord fields), and the 5 s line. "core 0 saved" is the
// material lists' time that left the game thread: the worker's time on them minus what the game thread paid for the overlap (its wait at
// the join, the listing and dispatch, the merge). The turn-prof line (GXV_TURN_PROF=1) measures the same directly: its flushes [dl + view]
// per drawn turn against a run with GXV_PAR_DL=0.
extern "C" void port_par_dl_note(const unsigned* rec, unsigned n) {
    ParDlWindow& w = sParDlWin;
    const uint64_t now = port::par::NowUs();
    if (w.t0 == 0) w.t0 = now;
    ++w.turns;
    for (unsigned i = 0; i < n && i < 21; ++i) w.c[i] += rec[i];
    if (now - w.t0 < 5000000u) return;
    uint32_t busyUs = 0, jobs = 0, taken = 0, slept = 0, wakeUs = 0;
    sParDlWorker.TakeStats(&busyUs, &jobs, &taken, &slept, &wakeUs);
    const double o = w.c[0] ? static_cast<double>(w.c[0]) : 1.0;  // (per overlapped drawn turn)
    const double workerMs = busyUs / o / 1000.0;
    const double waitMs = w.c[5] / o / 1000.0, dispatchMs = w.c[9] / o / 1000.0, mergeMs = w.c[6] / o / 1000.0;
    // (a job the game thread ran itself saved nothing: its time is in neither the worker's nor the wait)
    char core[24];
    if (sParDlWorker.Ready()) {
        snprintf(core, sizeof core, "core %d", sParDlWorker.Core());
    } else {
        snprintf(core, sizeof core, "not started");  // (round 2: an idle worker is not "core 1")
    }
    port_log_summary("par-dl: %.1f s, %llu drawn turns overlapped (%llu with the player's view pass split off to after the join), %llu serial (no view pass "
                     "pending %llu, the player's included and not split %llu); an overlapped turn: entries %.1f, "
                     "lists updated %.1f; worker %.3f ms (%s, start %.0f us after the dispatch, %u jobs, %.1f %% of its core); game thread: "
                     "listing+dispatch %.3f ms, wait at the join %.3f ms (%u slept; view jobs run meanwhile %.1f), merge %.3f ms, section %.3f ms (view pass included); "
                     "core 0 saved %.3f ms a drawn turn (worker - wait - dispatch - merge); took the job itself %llu, finished early %llu; "
                     "j3dSys words from the worker %.1f, conflicts %llu; verify (GXV_PAR_DL=2): %llu entries checked, %llu differed "
                     "(%llu first of their model data), j3dSys words differed %llu, material states differed %llu",
                     (now - w.t0) / 1e6, static_cast<unsigned long long>(w.c[0]), static_cast<unsigned long long>(w.c[20]),
                     static_cast<unsigned long long>(w.c[1]), static_cast<unsigned long long>(w.c[18]), static_cast<unsigned long long>(w.c[19]),
                     w.c[2] / o, w.c[8] / o, workerMs, core, jobs ? static_cast<double>(wakeUs) / jobs : 0.0, jobs,
                     (now - w.t0) > 0 ? busyUs * 100.0 / static_cast<double>(now - w.t0) : 0.0, dispatchMs, waitMs, slept, w.c[17] / o, mergeMs,
                     w.c[7] / o / 1000.0, workerMs - waitMs - dispatchMs - mergeMs, static_cast<unsigned long long>(w.c[3]),
                     static_cast<unsigned long long>(w.c[4]), w.c[10] / o, static_cast<unsigned long long>(w.c[11]),
                     static_cast<unsigned long long>(w.c[12]), static_cast<unsigned long long>(w.c[13]), static_cast<unsigned long long>(w.c[14]),
                     static_cast<unsigned long long>(w.c[15]), static_cast<unsigned long long>(w.c[16]));
    (void)taken;
    if (w.c[0] != 0) {
        // par25: net = the worker's time (what the serial flush would have cost the game thread) - dispatch - wait - merge. A verify run's merge holds the verify's
        // second computation: not judged. A job the game thread took itself is in neither side (busyUs is the worker's).
        port::par25::Summary s;
        s.secs = (now - w.t0) / 1e6;
        s.turns = static_cast<double>(w.turns);
        s.jobs = jobs;
        s.workerMs = busyUs / 1000.0;
        s.grossMs = busyUs / 1000.0;
        s.costMs = (static_cast<double>(w.c[5]) + static_cast<double>(w.c[9]) + static_cast<double>(w.c[6])) / 1000.0;
        s.mismatches = static_cast<unsigned>(w.c[13] + w.c[15] + w.c[16]);
        s.judged = ParDlRawMode() != 2;
        Par25Post(port::par25::kDl, s);
    }
    w = ParDlWindow{};
    w.t0 = now;
}
// GXV_PAR_PTCL (parwork, 7 Nov; default 0 in perf.cpp; decomp/src/JSystem/JParticle/JPAEmitterManager.cpp calcNormal): the particle calc's
// emitters (ParticleCalcExecutor::movementNormal: the nine JPAEmitterManager::calc loops) that do not depend on each other are
// calculated by a worker on the fourth core (GXV_PAR_PTCL_CORE, default 3) and by the game thread, in one join; the emitters that
// call game code stay on the game thread. Exact: same particles and emitters, same order of deletions (the proof is the file comment
// there). The turn is serial, as before, when a callback is of an unknown kind, when the pool could not be proven to have a node for
// every particle the turn could make, or when there is too little work.
//   GXV_PAR_PTCL=0|1|2       0 off (default), 1 on, 2 on + verify: each such turn is run twice from the same saved state (the serial loops,
//                            then the parallel run) and the two results compared (every emitter's fields, its particles in order, the
//                            groups' lists, the free list, the pool count); a mismatch is logged ("par-ptcl: MISMATCH ...") and the
//                            serial result is kept. Costs a few ms a turn: for the console check only.
//   GXV_PAR_PTCL_MIN=96      a turn whose chains cost less (8 + particles per emitter) stays serial
//   GXV_PAR_PTCL_CORE=3      the worker's core;  GXV_PAR_PTCL_PRIO=0xNN  its priority (default one step above the game thread's)
// Every 5 s (basic log): "par-ptcl: ...". If the game thread waited more than 1.5 ms for the worker (it was kept off its core), the next
// 120 turns run serial.
namespace {
port::par::SpecWorker& sParPtclWorker = *new port::par::SpecWorker;  // (never destroyed: its worker waits on it until the process ends)
std::atomic<uint8_t> sParPtclItems[1024];
struct ParPtclWindow {
    uint64_t t0 = 0;
    uint64_t turns = 0, c[18] = {};
    uint64_t joinUs = 0, joinMax = 0, runs = 0, stalls = 0;
    uint64_t pauses = 0, pausedTurns = 0, pauseTurnsSet = 0;  // (parprio31 census)
} sParPtclWin;
int sParPtclPause = 0;  // (turns left that run the particle calc serially, see ParPtclPauseTurns)
}  // namespace
static int ParPtclRawMode(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_PTCL", 0, 0, 2));  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
extern "C" int port_par_ptcl_mode(void) {
    return Par25Allowed(port::par25::kPtcl) ? ParPtclRawMode() : 0;  // (GXV_PAR25=1: the guard's verdict; it changes between two turns only)
}
extern "C" unsigned port_par_ptcl_min(void) {
    return PORT_HOT_KNOB(unsigned, static_cast<unsigned>(ParEnvInt("GXV_PAR_PTCL_MIN", 96, 0, 100000)));
}
extern "C" unsigned long long port_par_ptcl_now(void) { return port::par::NowUs(); }
static unsigned ParPtclPauseTurns();
static unsigned ParPtclStallUs();
static bool ParPtclStarted() {
    static const bool started = [] {
        const int prio = ParPrio("GXV_PAR_PTCL_PRIO", ParEnvInt("GXV_PAR_PTCL_CORE", 3, 1, 3) == 3);
        const bool ok = sParPtclWorker.Start(ParEnvInt("GXV_PAR_PTCL_CORE", 3, 1, 3), prio > 0 ? prio : -1, "smg_ptcl_spec");
        port_log_summary("par-ptcl: %s (GXV_PAR_PTCL=%d, GXV_PAR_PTCL_OVERLAP=%d; core %d, priority 0x%X; a wait over %u us for the worker pauses it for %u turns: GXV_PAR_PTCL_STALL_US / _PAUSE)",
                         ok ? "worker started" : "the worker could not be started: the particle calc stays serial", port_par_ptcl_mode(),
                         ParEnvInt("GXV_PAR_PTCL_OVERLAP", 0, 0, 1), sParPtclWorker.Core(), static_cast<unsigned>(sParPtclWorker.Priority()),
                         ParPtclStallUs(), ParPtclPauseTurns());
        return ok;
    }();
    return started;
}
// GXV_PAR_PTCL_PAUSE (cpu60, 10 Nov; default 120 = as before) and GXV_PAR_PTCL_STALL_US (default 1500): a join the game thread waited more than
// STALL_US for (the worker was kept off its core) makes the next PAUSE turns run the particle calc serially. Console, Honeyhive 720x404
// (c25 750): 2-3 such waits (2.1-3.4 ms) in every 5 s left 273-283 of 300 turns "no worker (or paused)": a pause of 120 turns forgoes
// 120 x (the parallel saving) to avoid one wait of ~3 ms. Scheduling only: the particle results are the same either way (exact).
static unsigned ParPtclPauseTurns() {
    return PORT_HOT_KNOB(unsigned, static_cast<unsigned>(ParEnvInt("GXV_PAR_PTCL_PAUSE", 120, 0, 3000)));
}
static unsigned ParPtclStallUs() {
    return PORT_HOT_KNOB(unsigned, static_cast<unsigned>(ParEnvInt("GXV_PAR_PTCL_STALL_US", 1500, 100, 100000)));
}
// GXV_PAR_PAUSE (parprio31; 0 = GXV_PAR_PTCL_PAUSE's fixed pause as before, 1 = proportional to the wait, 2 = none; port/par_prio.h PauseTurns).
static int ParPtclPauseMode() {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_PAUSE", 0, 0, 2));
}
static void ParPtclJoined(uint32_t waitUs) {
    const uint64_t wait = waitUs;
    ParPtclWindow& w = sParPtclWin;
    w.joinUs += wait;
    ++w.runs;
    if (wait > w.joinMax) w.joinMax = wait;
    if (wait > ParPtclStallUs()) {
        ++w.stalls;
        const unsigned turns = port::par::PauseTurns(ParPtclPauseMode(), waitUs, ParPtclStallUs(), ParPtclPauseTurns(),
                                                     static_cast<unsigned>(ParEnvInt("GXV_PAR_PAUSE_SAVE_US", 250, 10, 100000)));
        if (turns) {
            ++w.pauses;
            w.pauseTurnsSet += turns;
            sParPtclPause = static_cast<int>(turns);
        }
    }
}
// GXV_PAR_PTCL_OVERLAP=1 (gamepar, 8 Nov; default 0; needs GXV_PAR_PTCL=1 or 2): the particle calc's parallel chains are handed to the
// worker BEFORE the shadow list (SceneExecutor.cpp executeCalcAnimList runs the AnimParticle list first, armed, then the shadows, then
// port_ptcl_overlap_finish): the game thread casts the shadows while the worker computes the particles, then joins as GXV_PAR_PTCL does.
// Exact: the shadow list and the particle calc share no data (JPAEmitterManager.cpp, the file comment); everything of the particle calc
// that is game code (kind 2 callbacks, the deletions) and every turn that is not parallel run after the shadows, in the original place.
extern "C" int port_par_ptcl_overlap_on(void) {
    return PORT_HOT_KNOB(int, ParEnvInt("GXV_PAR_PTCL_OVERLAP", 0, 0, 1)) != 0 && port_par_ptcl_mode() != 0 ? 1 : 0;
}
extern "C" int port_par_ptcl_begin(void (*fn)(void*, unsigned), void* ctx, unsigned n) {
    if (!ParPtclStarted() || n == 0 || n > 1024) return 0;
    if (sParPtclPause > 0) {
        --sParPtclPause;
        ++sParPtclWin.pausedTurns;
        return 0;
    }
    return port::par::ForkBegin(sParPtclWorker, sParPtclItems, reinterpret_cast<port::par::ItemFn>(fn), ctx, n) ? 1 : 0;
}
extern "C" void port_par_ptcl_finish(void (*fn)(void*, unsigned), void (*mainFn)(void*), void* ctx, unsigned n) {
    uint32_t waitUs = 0;
    port::par::ForkEnd(sParPtclWorker, sParPtclItems, reinterpret_cast<port::par::ItemFn>(fn), mainFn, ctx, n, &waitUs);
    ParPtclJoined(waitUs);
}
extern "C" int port_par_ptcl_run(void (*fn)(void*, unsigned), void (*mainFn)(void*), void* ctx, unsigned n) {
    if (!ParPtclStarted() || n == 0 || n > 1024) return 0;
    if (sParPtclPause > 0) {
        --sParPtclPause;
        ++sParPtclWin.pausedTurns;
        return 0;
    }
    uint32_t waitUs = 0;
    if (!port::par::ForkJoin(sParPtclWorker, sParPtclItems, reinterpret_cast<port::par::ItemFn>(fn), mainFn, ctx, n, &waitUs)) return 0;
    ParPtclJoined(waitUs);
    return 1;
}
extern "C" void port_par_ptcl_bad(unsigned emitter, unsigned offset, unsigned sizeA, unsigned sizeB) {
    static unsigned shown = 0;
    if (shown++ < 20)
        port_log_summary("par-ptcl: MISMATCH (serial result kept): first difference in emitter slot %d at byte %u of its section; sizes %u / %u",
                         static_cast<int>(emitter), offset, sizeA, sizeB);
}
// One turn's counts (JPAEmitterManager.cpp's enum), and the 5 s line.
extern "C" void port_par_ptcl_note(const unsigned* counts, unsigned n) {
    ParPtclWindow& w = sParPtclWin;
    const uint64_t now = port::par::NowUs();
    if (w.t0 == 0) w.t0 = now;
    ++w.turns;
    for (unsigned i = 0; i < n && i < 18; ++i) w.c[i] += counts[i];
    if (now - w.t0 < 5000000u) return;
    uint32_t workerUs = 0, workerJobs = 0, sleeps = 0;
    sParPtclWorker.TakeStats(&workerUs, &workerJobs, &sleeps);
    const double t = w.turns ? static_cast<double>(w.turns) : 1.0;
    const double par = w.c[1] ? static_cast<double>(w.c[1]) : 1.0;
    // c: 0 turns 1 parallel 2 emitters 3 par emitters 4 serial emitters 5 resources 6 chains 7 particles 8 us used 9 us serial ref
    //    10 unknown cb 11 pool 12 small 13 no worker 14 cap 15 verified 16 bad 17 verify skipped
    port_log_summary("par-ptcl: %.1f s, %u turns, %llu parallel (a parallel turn: emitters %.1f = %.1f in chains + %.1f on the game thread, "
                     "resources %.1f, parallel chains %.1f; a turn: particles %.0f); game thread in the calc %.3f ms a turn; "
                     "serial because: unknown callback %llu, pool bound %llu, too small %llu, no worker (or paused) %llu, over capacity %llu; "
                     "worker %.3f ms a turn (%u chains), game thread waited %.3f ms a parallel turn (max %llu us, %llu stalls > 1.5 ms); "
                     "verify: %llu checked (%llu not compared: a kind 2 emitter callback), %llu mismatches%s",
                     (now - w.t0) / 1e6, static_cast<unsigned>(w.turns), static_cast<unsigned long long>(w.c[1]), w.c[2] / par, w.c[3] / par,
                     w.c[4] / par, w.c[5] / par, w.c[6] / par, w.c[7] / t, w.c[8] / t / 1000.0, static_cast<unsigned long long>(w.c[10]),
                     static_cast<unsigned long long>(w.c[11]), static_cast<unsigned long long>(w.c[12]),
                     static_cast<unsigned long long>(w.c[13]), static_cast<unsigned long long>(w.c[14]), workerUs / t / 1000.0, workerJobs,
                     w.runs ? w.joinUs / static_cast<double>(w.runs) / 1000.0 : 0.0, static_cast<unsigned long long>(w.joinMax),
                     static_cast<unsigned long long>(w.stalls), static_cast<unsigned long long>(w.c[15]),
                     static_cast<unsigned long long>(w.c[17]), static_cast<unsigned long long>(w.c[16]), w.c[15] ? " (serial reference run costs a few ms a turn: verify only)" : "");
    port_log_summary("par-prio: GXV_PAR_PRIO=%d GXV_PAR_PAUSE=%d; worker core %d priority 0x%X; joins %llu, wait mean %.3f ms max %llu us, stalls > %u us %llu; "
                     "pauses %llu (%llu turns set, %llu skipped); the worker ran %llu of %u turns",
                     ParEnvInt("GXV_PAR_PRIO", 0, 0, 0xFF), ParPtclPauseMode(), sParPtclWorker.Core(), static_cast<unsigned>(sParPtclWorker.Priority()),
                     static_cast<unsigned long long>(w.runs), w.runs ? w.joinUs / static_cast<double>(w.runs) / 1000.0 : 0.0,
                     static_cast<unsigned long long>(w.joinMax), ParPtclStallUs(), static_cast<unsigned long long>(w.stalls),
                     static_cast<unsigned long long>(w.pauses), static_cast<unsigned long long>(w.pauseTurnsSet),
                     static_cast<unsigned long long>(w.pausedTurns), static_cast<unsigned long long>(w.runs), static_cast<unsigned>(w.turns));
    {  // (hitch30) the join's wait for a chain the worker is in: bounded spin, then naps
        char sb[320];
        port::par::SpecWaitCensus().Line("par-ptcl join wait", sb, sizeof sb);
        port_log_summary("%s (GXV_BOUND_SPIN_US=%u, nap %u us)", sb, port::par::WaitBoundSpinUs(), port::par::WaitBoundNapUs());
        port::par::PoolWaitCensus().Line("par-ptcl pool lock wait", sb, sizeof sb);
        port_log_summary("%s", sb);
    }
    {
        // par25: the worker's time is an UPPER BOUND of what the game thread would have spent on the same chains; the joins' waits are the cost
        port::par25::Summary s;
        s.secs = (now - w.t0) / 1e6;
        s.turns = t;
        s.jobs = workerJobs;
        s.workerMs = workerUs / 1000.0;
        s.grossMs = workerUs / 1000.0;
        s.costMs = w.joinUs / 1000.0;
        s.mismatches = static_cast<unsigned>(w.c[16]);
        s.bound = true;
        s.judged = ParPtclRawMode() != 2;
        Par25Post(port::par25::kPtcl, s);
    }
    w = ParPtclWindow{};
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
    {
        // par25: gross = the worker's time on the walks the game thread took (what the original would have cost it here); cost = the copies + the dispatch
        port::par25::Summary s;
        s.secs = secs;
        s.turns = b;
        s.jobs = workerJobs;
        s.workerMs = workerUs / 1000.0;
        s.grossMs = w.c[5] / 1000.0;
        s.costMs = (static_cast<double>(w.c[6]) + static_cast<double>(w.c[7])) / 1000.0;
        s.mismatches = static_cast<unsigned>(w.c[9]);
        s.judged = !port_par_anim_verify_on();  // (a verify run computes the original too: "saved" is 0 there)
        Par25Post(port::par25::kAnim, s);
    }
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
        // (GXV_PACE23: engages below 50, leaves at 57, frame_pace.h kPace23Steady30*; GXV_PACE60, the 60-or-an-even-30 rule of automatic quality's
        // GPU model, takes the same thresholds: its verdict "60 reachable" is the release at 57, and what it cannot reach is the uneven 38-50)
        const bool p23 = vgx::EnvInt("GXV_PACE23", 0) != 0 || vgx::EnvInt("GXV_PACE60", 0) != 0;
        const long below = vgx::EnvInt("GXV_STEADY30_BELOW", static_cast<long>(p23 ? port::pace::kPace23Steady30BelowFps : port::pace::kSteady30BelowFps)),
                   above = vgx::EnvInt("GXV_STEADY30_ABOVE", static_cast<long>(p23 ? port::pace::kPace23Steady30AboveFps : port::pace::kSteady30AboveFps));
        if (p23) { p.engageSec = 2.5; p.releaseSec = 3.0; }
        // (GXV_SEAMLESS, seamless23: the release to "up to 60" at a camera cut or a quiet moment, at most GXV_SEAMLESS_PACE_WAIT_MS later (15000);
        // after a release, 20 turns of uneven pictures engage again at once instead of a whole second (GXV_SEAMLESS_PACE_QUICK=<turns>, 0 off))
        if (vgx::EnvInt("GXV_SEAMLESS", 0) != 0) {
            p.cutRelease = vgx::EnvInt("GXV_SEAMLESS_PACE_CUT", 1) != 0;
            const long waitMs = vgx::EnvInt("GXV_SEAMLESS_PACE_WAIT_MS", 15000);
            p.cutWaitSec = static_cast<double>(waitMs < 0 ? 0 : waitMs > 600000 ? 600000 : waitMs) / 1000.0;
            const long quick = vgx::EnvInt("GXV_SEAMLESS_PACE_QUICK", 20);
            p.quickTurns = static_cast<unsigned>(quick < 0 ? 0 : quick > 59 ? 59 : quick);
        }
        // GXV_PACE24 (pace24, default 0): a calmer and quicker steady-30 rule that reads what the display really showed (frame_pace.h HoldMeter).
        //   engages below GXV_PACE24_BELOW (50) pictures a second when the screen's holds are uneven (GXV_PACE24_BREAKS, 7 breaks a second) for
        //   GXV_PACE24_ENGAGE_MS (1500); releases when 57+ are estimated for GXV_PACE24_RELEASE_MS (2000) at a camera cut or quiet moment (at most
        //   GXV_PACE24_CUTWAIT_MS, 8000, later), never within GXV_PACE24_DWELL_MS (8000) of an engage; a release the screen does not confirm within
        //   GXV_PACE24_QUICK (24) turns engages again and blocks the next one GXV_PACE24_BLOCK_S (10, doubling to GXV_PACE24_BLOCKMAX_S, 120).
        //   Decisions are made over 30-turn windows (GXV_PACE24_WINDOW). A release now also clears the window it was made in (without GXV_PACE24 the
        //   release at a cut was undone 7-13 ms later by the quick-engage check: c27 760 and 768, every cut release).
        const bool p24 = vgx::EnvInt("GXV_PACE24", 0) != 0;
        if (p24) {
            p.p24 = true;
            p.window = static_cast<unsigned>(vgx::EnvInt("GXV_PACE24_WINDOW", 30));
            p.breakFps = static_cast<double>(vgx::EnvInt("GXV_PACE24_BREAKS", 7));
            p.engageSec = static_cast<double>(vgx::EnvInt("GXV_PACE24_ENGAGE_MS", 1500)) / 1000.0;
            p.releaseSec = static_cast<double>(vgx::EnvInt("GXV_PACE24_RELEASE_MS", 2000)) / 1000.0;
            p.dwellSec = static_cast<double>(vgx::EnvInt("GXV_PACE24_DWELL_MS", 8000)) / 1000.0;
            p.blockSec = static_cast<double>(vgx::EnvInt("GXV_PACE24_BLOCK_S", 10));
            p.maxBlockSec = static_cast<double>(vgx::EnvInt("GXV_PACE24_BLOCKMAX_S", 120));
            p.confirmSec = static_cast<double>(vgx::EnvInt("GXV_PACE24_CONFIRM_S", 6));
            p.cutRelease = true;
            p.cutWaitSec = static_cast<double>(vgx::EnvInt("GXV_PACE24_CUTWAIT_MS", 8000)) / 1000.0;
            p.quickTurns = static_cast<unsigned>(std::min<long>(59, std::max<long>(8, vgx::EnvInt("GXV_PACE24_QUICK", 24))));
        }
        {
            const long b = p24 ? vgx::EnvInt("GXV_PACE24_BELOW", 50) : below;
            p.belowFps = static_cast<double>(b < 20 ? 20 : b > 58 ? 58 : b);
        }
        p.aboveFps = static_cast<double>(above < 31 ? 31 : above > 60 ? 60 : above);
        if (p24 && !std::getenv("GXV_STEADY30_ABOVE")) p.aboveFps = port::pace::kPace23Steady30AboveFps;
        if (p.aboveFps < p.belowFps + 3.0) p.aboveFps = p.belowFps + 3.0;  // (a gap between the two, or it would flap)
        c.Configure(on, p);
        if (p24) c.UseMeter(&port::pace::GlobalHoldMeter());
        if (on && p24)
            port_log_summary("pace24: GXV_PACE24 on: steady 30 engages under %.0f pictures a second with %.0f or more display breaks a second for %.1f s, over %u-turn windows; "
                             "released at 57+ estimated for %.1f s at a camera cut or quiet moment (<= %.0f s later), not within %.0f s of an engage; an unconfirmed release "
                             "engages again within %u turns and blocks the next one %.0f s (doubling to %.0f s); a release clears its window",
                             p.belowFps, p.breakFps, p.engageSec, p.window, p.releaseSec, p.cutWaitSec, p.dwellSec, p.quickTurns, p.blockSec, p.maxBlockSec);
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
// GXV_PACE24's "smooth:" census: how often the steady 30 changed state (the game thread counts, the display callback reads).
static std::atomic<uint32_t> sSteadyChanges{0};
// GXV_SMOOTH60 (policy26, port/smooth60.h): the per-stage controller owns the even 30. mode 1 forces it on, 0 forces it off (the 38 / 50 rules of GXV_STEADY30
// then decide nothing), -1 gives it back to those rules. Takes effect at the next turn; needs GXV_STEADY30=1 (entry.cpp sets it with GXV_SMOOTH60).
static bool RateWanted();   // (rate30: GXV_RATE_AUTO, below with the governors)
static std::atomic<int> sSteadyForce{-1};
extern "C" void port_pace_steady30_force(int mode) { if (RateWanted()) return; sSteadyForce.store(mode < 0 ? -1 : (mode > 0 ? 1 : 0), std::memory_order_relaxed); }
extern "C" uint32_t port_pace_steady30_changes(void) { return sSteadyChanges.load(std::memory_order_relaxed); }
// GXV_SEAMLESS_STRESS (the back end's stress mode): the other rhythm at the next turn.
static std::atomic<bool> sSteadyStress{false};
extern "C" void port_pace_stress_toggle(void) { sSteadyStress.store(true, std::memory_order_relaxed); }
static void SteadyLog(const port::pace::SteadyCadence& c) {
    const char* why = c.Engaged() ? "engaged" : (c.LastAtCut() ? "left (at a camera cut or a quiet moment)" : "left");
    if (c.QuickEngages() || c.ReleasesAtCut() || c.ReleasesTimedOut())
        port_log_summary("pace: steady 30 switches so far: %u releases at a cut or quiet moment, %u after the wait for one ran out, %u quick engages after a release",
                         c.ReleasesAtCut(), c.ReleasesTimedOut(), c.QuickEngages());
    port_log_summary("pace: steady 30 %s: %.1f pictures a second in the last second; without it %.1f are estimated (game turns %.1f, "
                     "front end/renderer/GPU %.1f); GPU %.1f ms a picture, %s; next release blocked %.0f s; display breaks %.1f a second",
                     why, c.RateFps(), c.EstimateFps(), c.GameFps(), c.PipeFps(), c.GpuUs() / 1000.0,
                     c.Lowerable() ? "a smaller picture still possible" : "nothing left to lower", c.BlockSec(), c.BreaksPerSec());
}
static void RateStep(port::pace::SteadyCadence& c, bool allowed, bool quiet, uint64_t nowUs);
static void RateNoteTurn(bool allowed, bool quiet, bool drawn, bool missed);
static void RateNoteGuardDrop();
static int SteadyTurn(int allowed, int code) {
    const int preCode = code;   // (rate30: the skip rule's verdict before the cadence: not 0 = it left the turn out as late or busy)
    port::pace::SteadyCadence& c = SteadyOnce();
    uint32_t gpuUs = 0, pipeUs = 0;
    int lowerable = 0;
    const bool known = port_autoq_stages && port_autoq_stages(&gpuUs, &pipeUs, &lowerable) != 0;
    c.Stages(known, gpuUs, pipeUs, lowerable != 0);
    const bool quiet = port_screen_quiet && port_screen_quiet() != 0;
    const uint64_t nowUs = static_cast<uint64_t>(port_host_time_ns() / 1000);
    if (port_camera_cut_count) { // (GXV_SEAMLESS: a release due waits for a camera cut)
        static uint32_t seen = 0;
        const uint32_t cuts = port_camera_cut_count();
        if (cuts != seen) { seen = cuts; c.NoteCut(nowUs); }
    }
    if (sSteadyStress.exchange(false, std::memory_order_relaxed)) {
        const unsigned before = c.Changes();
        c.ForceToggle(nowUs);
        if (c.Changes() != before) {
            sSteadyOn.store(c.Engaged(), std::memory_order_relaxed); sSteadyChanges.fetch_add(1, std::memory_order_relaxed);
            port_log_summary("pace: steady 30 %s by GXV_SEAMLESS_STRESS", c.Engaged() ? "engaged" : "left");
        }
    }
    {   // (GXV_SMOOTH60: a forced state is handed to the cadence at the first turn after the request)
        static int applied = -1;
        const int want = sSteadyForce.load(std::memory_order_relaxed);
        if (want != applied) {
            applied = want;
            const unsigned before = c.Changes();
            c.Force(want, nowUs);
            sSteadyOn.store(c.Engaged(), std::memory_order_relaxed);
            if (c.Changes() != before) sSteadyChanges.fetch_add(1, std::memory_order_relaxed);
            port_log_summary("pace: steady 30 %s by GXV_SMOOTH60 (%s)", c.Engaged() ? "engaged" : "off", want < 0 ? "rules back" : want ? "forced on" : "forced off");
        }
    }
    const bool rateOn = RateWanted();
    if (rateOn) RateStep(c, allowed != 0, quiet, nowUs);   // (rate30: GXV_RATE_AUTO owns the rhythm: its switch is made here, for THIS turn)
    const unsigned changes0 = c.Changes();
    const bool offered = c.Begin(nowUs, allowed != 0, quiet);
    if (c.Changes() != changes0) { // (a release made at a cut or quiet moment, in Begin)
        sSteadyOn.store(c.Engaged(), std::memory_order_relaxed); sSteadyChanges.fetch_add(1, std::memory_order_relaxed);
        SteadyLog(c);
    }
    if (allowed && !offered && code == 0) code = 1;  // (not offered: the turn keeps its retrace, as one the back end is too busy for)
    c.Decision(offered || !allowed, code == 0);
    if (rateOn) RateNoteTurn(allowed != 0, quiet, code == 0, preCode != 0);
    return code;
}
static void SteadyWorkDone() {
    if (!SteadyEnabled()) return;
    port::pace::SteadyCadence& c = SteadyOnce();
    const unsigned before = c.Changes();
    c.End(static_cast<uint64_t>(port_host_time_ns() / 1000) - sLateSleptUs);   // (pace32: a turn GXV_LATE_TURN started late on purpose costs its work, not its sleep)
    if (c.Changes() != before) {
        sSteadyOn.store(c.Engaged(), std::memory_order_relaxed); sSteadyChanges.fetch_add(1, std::memory_order_relaxed);
        SteadyLog(c);
    }
}

// GXV_PACE25 (pacer25; default 0 = the decisions below are exactly the legacy rule's; 1 = acts; 2 = census: decides and counts, the legacy rule still acts):
// the even cadence for work per picture between one and two refreshes (platform/include/port/pace25.h; the report: agents/pacer25/REPORT.txt). The legacy "Up to 60"
// rule leaves turns out when the back end holds >= 2 pictures it has not finished; that signal is late and quantised, so between about 1.2 and 1.4 refreshes of work a
// picture it draws 3-4 turns in a row and then leaves out two (holds 1-1-1-3, a 50 ms hold: Toy Time with the faster E / EU switches, c69). With GXV_PACE25=1 the
// turns that draw are a fixed even rhythm (1-2, 1-1-2, 1-1-1-2 ... chosen from the measured cost per picture, hold never above 2 while the work fits two refreshes),
// the back end's backlog is only a guard (GXV_PACE25_GUARD, default 3 pictures), and a scene that fits one refresh is not touched at all. Not with the steady 30 /
// GXV_SKIP_PATTERN (they own the rhythm). Logic turns stay 60 a second: the rhythm is never below what the game thread's own turn costs need.
// Sub-switches: GXV_PACE25_MARGIN (percent headroom on the slowest stage, default 3), GXV_PACE25_GUARD (3), GXV_PACE25_LOG_S (5; 0 = no line).
// Basic log: "pace25:" every GXV_PACE25_LOG_S seconds (level, cost estimate, the rhythm's holds, guard hits, turns decided differently from the legacy rule).
static port::pace::Pace25& Pace25Once() {
    static port::pace::Pace25 pace;
    static const bool configured = [] {
        port::pace::Pace25Config c;
        long mode = vgx::EnvInt("GXV_PACE25", 0);
        if (mode < 0 || mode > 2) mode = 0;
        if (mode && (vgx::EnvInt("GXV_SKIP_PATTERN", 0) != 0 || SteadyOnce().Enabled())) {
            port_log_summary("pace25: GXV_PACE25=%ld ignored: the steady 30 / GXV_SKIP_PATTERN own the rhythm", mode);
            mode = 0;
        }
        c.mode = static_cast<int>(mode);
        const long margin = vgx::EnvInt("GXV_PACE25_MARGIN", 3);
        c.marginPct = margin < 0 ? 0.0 : (margin > 30 ? 30.0 : static_cast<double>(margin));
        const long guard = vgx::EnvInt("GXV_PACE25_GUARD", 3);
        c.guardDepth = static_cast<unsigned>(guard < 2 ? 2 : (guard > 6 ? 6 : guard));
        pace.Configure(c);
        if (mode)
            port_log_summary("pace25: %s (GXV_PACE25=%ld): an even rhythm of pictures from the measured cost, margin %.0f %%, back-end guard at %u pictures",
                             mode == 2 ? "census, the legacy rule acts" : "on", mode, c.marginPct, c.guardDepth);
        return true;
    }();
    (void)configured;
    return pace;
}
static bool Pace25Enabled() { return PORT_HOT_KNOB(bool, Pace25Once().Enabled()); }
static void Pace25WorkDone() {
    if (!Pace25Enabled()) return;
    Pace25Once().WorkDone(port_host_time_ns() / 1000, sSlipDrew);
}
static int Pace25Turn(int allowed, const port::pace::LegacySkip::Verdict& v, unsigned pending) {
    port::pace::Pace25& pace = Pace25Once();
    uint32_t gpuUs = 0, pipeUs = 0;
    int lowerable = 0;
    if (!(port_autoq_stages && port_autoq_stages(&gpuUs, &pipeUs, &lowerable) != 0)) pipeUs = 0;
    port::pace::Pace25::Turn t{};
    t.nowUs = port_host_time_ns() / 1000;
    t.allowed = allowed != 0;
    t.quiet = port_screen_quiet && port_screen_quiet() != 0;
    t.legacy = v.code;
    t.legacyBehind = v.behind;
    t.pending = pending;
    t.pipeUs = pipeUs;
    const unsigned levelBefore = pace.Level();
    const int code = pace.Decide(t);
    static int64_t lastLog = 0;
    static const long logS = vgx::EnvInt("GXV_PACE25_LOG_S", 5);
    if (logS > 0 && (t.nowUs - lastLog >= logS * 1000000 || pace.Level() != levelBefore)) {  // (a change of level is logged at once)
        char line[640];
        port::pace::Pace25::Format(line, sizeof line, pace);
        port_log_summary("%s", line);
        pace.ClearStats();
        lastLog = t.nowUs;
    }
    return code;
}

// GXV_EVEN26 (pace26; 0 off = exactly the legacy rule, 1 acts, 2 census: decides and counts, the legacy rule acts): even frame pacing for Performance mode when the
// pipeline cannot take every turn (platform/include/port/even26.h; the report: new/pace26/REPORT.txt). A picture every `period` turns (a real number: holds are the
// floor and ceil of it, the long ones as far apart as they can be), the period from the slowest stage's cost (port_autoq_stages) and trimmed by the back end's busy verdicts,
// instead of the legacy rule's bang-bang on `pending` (1-1-1-3: 5-6 % of the tour's pictures held 3+ vblanks, 17-24 % in single windows; c79 logs). The legacy rule keeps its say
// when the game thread is behind the display's clock. Not while the steady 30 is engaged (it owns the rhythm then) or with GXV_SKIP_PATTERN / GXV_PACE25.
// Sub-switches: GXV_EVEN26_EVENT_AT (2: pending at a due turn that counts as busy; 1 = less latency (about 5 ms) for about 3 pictures a second, 3 = the opposite),
// GXV_EVEN26_GUARD (3), GXV_EVEN26_SNAP30 (1.85 turns; 0 = off), GXV_EVEN26_LOG_S (10; the "even26:" line, 0 = none). Logic turns stay 60 a second.
// ab27: the mode is read through the registry (port::stageknob::Int: a stage's or the A/B driver's override first, the environment else); Even26Enabled re-reads it when the registry's generation changes and
// configures the pacer again (its learned period starts again: stage_knob.h FlipClass 'w').
static long Even26ModeNow() {
    long mode = port::stageknob::Int("GXV_EVEN26", 0);
    if (mode < 0 || mode > 2) mode = 0;
    if (mode && (vgx::EnvInt("GXV_SKIP_PATTERN", 0) != 0 || vgx::EnvInt("GXV_PACE25", 0) != 0)) mode = 0;
    if (mode && vgx::EnvInt("GXV_FPS_LIMIT", 0) == 30) mode = 0;
    return mode;
}
static port::pace::Even26Config sEvenCfg;
static port::pace::Even26& Even26Once() {
    static port::pace::Even26 even;
    static const bool configured = [] {
        port::pace::Even26Config c;
        long mode = port::stageknob::Int("GXV_EVEN26", 0);
        if (mode < 0 || mode > 2) mode = 0;
        if (mode && (vgx::EnvInt("GXV_SKIP_PATTERN", 0) != 0 || vgx::EnvInt("GXV_PACE25", 0) != 0)) {
            port_log_summary("even26: GXV_EVEN26=%ld ignored: GXV_SKIP_PATTERN / GXV_PACE25 own the rhythm", mode);
            mode = 0;
        }
        if (mode && vgx::EnvInt("GXV_FPS_LIMIT", 0) == 30) {
            port_log_summary("even26: GXV_EVEN26=%ld ignored: the fixed 30 (Quality / Locked 30) has its own rule, GXV_EVEN26_Q", mode);
            mode = 0;
        }
        c.mode = static_cast<int>(mode);
        const long ev = vgx::EnvInt("GXV_EVEN26_EVENT_AT", 2), gd = vgx::EnvInt("GXV_EVEN26_GUARD", 3), sn = vgx::EnvInt("GXV_EVEN26_SNAP30", 185);
        c.eventAt = static_cast<unsigned>(ev < 1 ? 1 : (ev > 5 ? 5 : ev));
        c.guard = static_cast<unsigned>(gd < c.eventAt ? c.eventAt : (gd > 6 ? 6 : gd));
        c.snap30 = sn <= 0 ? 0.0 : sn / 100.0;  // (hundredths of a turn: 185 = 1.85)
        c.fit60 = vgx::EnvInt("GXV_EVEN26_FIT60", 0) != 0;   // (deadfix27: proposed default 1 for Performance; the "even26:" line says "fit-60 hold on")
        sEvenCfg = c;
        even.Configure(c);
        if (mode)
            port_log_summary("even26: %s (GXV_EVEN26=%ld): a picture every `period` turns from the slowest stage's cost and the back end's busy verdicts (at %u pending, guard %u, "
                             "an even 30 from a period of %.2f)", mode == 2 ? "census, the legacy rule acts" : "on", mode, c.eventAt, c.guard, c.snap30);
        return true;
    }();
    (void)configured;
    return even;
}
// (the expression runs once, and again after each registry Commit: a change of the mode configures the pacer anew)
static bool Even26Reread() {
    port::pace::Even26& e = Even26Once();
    const long mode = Even26ModeNow();
    if (mode != sEvenCfg.mode) {
        sEvenCfg.mode = static_cast<int>(mode);
        e.Configure(sEvenCfg);
        port_log_summary("even26: GXV_EVEN26 is now %ld (registry change; the learned period starts again)", mode);
    }
    return e.Enabled();
}
static bool Even26Enabled() { return PORT_STAGE_KNOB(bool, Even26Reread()); }
static int Even26Turn(int allowed, const port::pace::LegacySkip::Verdict& v, unsigned pending) {
    port::pace::Even26& even = Even26Once();
    uint32_t gpuUs = 0, pipeUs = 0;
    int lowerable = 0;
    if (!(port_autoq_stages && port_autoq_stages(&gpuUs, &pipeUs, &lowerable) != 0)) pipeUs = 0;
    port::pace::Even26::Turn t{};
    t.allowed = allowed != 0;
    // (the steady 30 owns the rhythm while it is engaged: the legacy verdict goes through and the period starts again from the hint afterwards)
    t.quiet = (port_screen_quiet && port_screen_quiet() != 0) || sSteadyOn.load(std::memory_order_relaxed);
    t.legacy = v.code;
    t.legacyBehind = v.behind;
    t.legacyBusy = v.gpuBusy && !v.behind;
    t.capped = v.capped;
    t.pending = pending;
    t.pipeUs = pipeUs;
    const int code = even.Decide(t);
    static int64_t lastLog = 0;
    static const long logS = vgx::EnvInt("GXV_EVEN26_LOG_S", 10);
    const int64_t nowUs = port_host_time_ns() / 1000;
    if (logS > 0 && nowUs - lastLog >= logS * 1000000) {
        if (lastLog) {
            char line[512];
            port::pace::Even26::Format(line, sizeof line, even);
            port_log_summary("%s", line);
        }
        even.ClearStats();
        lastLog = nowUs;
    }
    return code;
}

// The "even:" line's tail (vgx_host_galaxy_vita.cpp, display thread): which rule chose the pictures, and its state. Reads only atomics and the pacer's counters
// (the game thread writes them; a torn read of a double only misprints one log line).
extern "C" int port_pace_even_info(char* out, unsigned cap) {
    const bool steady = sSteadyOn.load(std::memory_order_relaxed);
    if (Even26Enabled() && Even26Once().Acting())
        return std::snprintf(out, cap, "pacer: GXV_EVEN26 (a picture every %.2f turns%s)%s", Even26Once().Period(), Even26Once().EffectivePeriod() != Even26Once().Period() ? ", rhythm snapped" : "",
                             steady ? ", steady 30 engaged (it owns the rhythm)" : "");
    return std::snprintf(out, cap, "pacer: %s%s", Even26Enabled() ? "GXV_EVEN26=2 census, legacy rule acts" : "legacy skip rule", steady ? ", steady 30 engaged" : "");
}

// ---- GXV_SPEED_GUARD (speed30; default 1 = on since 13 Nov, the owner's rule; 0 = off; platform/include/port/speed_guard.h): the game-speed guard, and its census, "speed:", which runs always (off or on).
// The turns run at 60 a second only while the work of the picture's turns fits the refreshes the rule gives it; when it does not (Quality: a pair of turns over 33.4 ms; Performance:
// a picture every two turns over 33.4 ms) the game clock falls behind the display's (frame_pace.h Slip even asks for it, up to GXV_SLIP_MAX 20 %): smooth pictures, slow motion, which
// fps and frame-time graphs cannot show. The guard keeps a leaky bucket of the game time lost against the display's clock (GXV_SPEED_GUARD_MS, default 30; leak 2 % of the wall time);
// over its budget, the picture about to be made is left out once more (an extra undrawn turn, no retrace wait: a three-turn picture) until the bucket is paid back.
// Quality: Controller::SetExtra in port_vgx_pace_turn (vgx_galaxy.cpp). Performance: the filter in FrameskipDecide below (after the steady 30), which also tells the skip rule and the
// steady 30's rhythm that the turn did not draw. Not counted, not acted on: loading / scene-change turns, covered screens (wipe, menu), the first GXV_SPEED_GUARD_GRACE_MS (3000) after
// them, and gaps over 80 ms between two turn starts. GXV_SPEED_GUARD_MAXSHARE=<percent> (1..100, default 100): at most that share of the pictures take an extra turn (25: one in four);
// GXV_SPEED_GUARD_EXTRA=1..2 (default 1): extra undrawn turns a picture may take (2 = a four-turn picture, a 67 ms hold); GXV_SPEED_GUARD_LEAK=<percent> (1..20, default 2): the share of the wall time the bucket forgives = the game speed the guard aims at is 100 minus this. Not with GXV_PACE25 / GXV_EVEN26 / GXV_SKIP_PATTERN in the
// Performance path (they own the rhythm: the guard stays off and says so).
// Census (every 5 s, and one line at each stage's end): "speed: game ran at NN.N % of real time over 5.0 s (lost M ms; K extra undrawn turns by the guard; budget B ms)".
// (speed31, GXV_SPEED_GUARD_SIGNAL, default 1; 0 = the cut systems do not read the guard) the guard's sustained repayment is a signal for the Quality ladder, the Performance governors (and so the far field)
// and the rate controller (the guard's extra turn is a missed turn there).
static bool SpeedGuardSignalOn() {
    static const bool on = vgx::EnvInt("GXV_SPEED_GUARD_SIGNAL", 1) != 0;
    return on;
}
static void SpeedGuardAct(uint64_t nowUs);   // (below, after the governors)
// (speed31) the skip rule's own left-out turns (late or back end busy) of the last 30 turns, one bit each (game thread only): Performance at 60 holds the game's speed with them, not with the guard;
// 8 of 30 is the rate controller's trigger and the governors' fast signal too (a picture left out is a fault to be cut away, whoever left it out).
static uint32_t sGuardMissBits = 0;
static void GuardMissNote(bool allowed, bool missed) {
    if (!allowed) { sGuardMissBits = 0; return; }
    sGuardMissBits = (sGuardMissBits << 1) | (missed ? 1u : 0u);
}
static bool GuardMissSustained() { return __builtin_popcount(sGuardMissBits & 0x3FFFFFFFu) >= 8; }
static port::pace::SpeedGuard& SpeedGuardOnce() {
    static port::pace::SpeedGuard& g = [] () -> port::pace::SpeedGuard& {
        port::pace::SpeedGuard& gg = port::pace::GlobalSpeedGuard();
        // (owner, 13 Nov 12:48: "keep game speed and drop a frame when needed ... weird if Mario feels slow motion": default 1 in both modes; 0 = the slip rule alone, as before)
        bool on = vgx::EnvInt("GXV_SPEED_GUARD", 1) != 0;
        // (hrw31: the half-rate-world modes run under GXV_SKIP_PATTERN, and the guard acts there: its extra undrawn turn drops a FIRST picture, and the pair's second picture is given up before that, port_hrw_undrawn_begin)
        const long hrwMode = vgx::EnvInt("GXV_HRW", 0);
        const bool hrwRhythm = hrwMode == 1 || hrwMode == 5;
        if (on && vgx::EnvInt("GXV_FPS_LIMIT", 0) != 30 && (vgx::EnvInt("GXV_PACE25", 0) != 0 || vgx::EnvInt("GXV_EVEN26", 0) != 0 || (vgx::EnvInt("GXV_SKIP_PATTERN", 0) != 0 && !hrwRhythm))) {
            port_log_summary("speed: GXV_SPEED_GUARD=1 ignored: GXV_PACE25 / GXV_EVEN26 / GXV_SKIP_PATTERN own the rhythm in this mode");
            on = false;
        }
        // (speed31) leak 0.5 % (GXV_SPEED_GUARD_LEAK_PM, per mille; GXV_SPEED_GUARD_LEAK in whole percent still wins when set), budget 20 ms: the model's table (REPORT.txt): 2 % could not reach 98 %
        long leakPm = vgx::EnvInt("GXV_SPEED_GUARD_LEAK_PM", 5);
        if (vgx::EnvInt("GXV_SPEED_GUARD_LEAK", 0) > 0) leakPm = vgx::EnvInt("GXV_SPEED_GUARD_LEAK", 0) * 10;
        port::pace::SpeedGuard::Config gc = port::pace::MakeSpeedGuardConfigPm(on, vgx::EnvInt("GXV_SPEED_GUARD_MS", 20), vgx::EnvInt("GXV_SPEED_GUARD_MAXSHARE", 100),
                                                                              vgx::EnvInt("GXV_SPEED_GUARD_EXTRA", 1), vgx::EnvInt("GXV_SPEED_GUARD_GRACE_MS", 3000), leakPm);
        gc.escalate = vgx::EnvInt("GXV_SPEED_GUARD_ESC", 1) != 0;                 // (speed31: 0 = speed30's one extra turn)
        gc.own = vgx::EnvInt("GXV_SPEED_GUARD_OWN", 1) != 0;                      // (0 = the rules' late catch-up keeps running beside the guard)
        gc.snap = vgx::EnvInt("GXV_SPEED_GUARD_SNAP", 1) != 0;                    // (0 = a fractional need is spread over k and k + 1 turns, never locked to one)
        gc.maxExtraEsc = static_cast<unsigned>(vgx::EnvInt("GXV_SPEED_GUARD_EXTRA_MAX", 8));
        gc.pressNs = static_cast<int64_t>(vgx::EnvInt("GXV_SPEED_GUARD_PRESS_MS", 300)) * 1000000;
        // pace32 (owner, 13 Nov 21:30): the slip band. GXV_SPEED_GUARD_SLIP=1 (default 0 = speed31 exactly): a measured loss of at most GXV_SPEED_GUARD_SLIP_PM (45 per mille) is slipped, not dropped,
        // for at most GXV_SPEED_GUARD_SLIP_S (10) seconds of allowance (refilled at 1 / GXV_SPEED_GUARD_SLIP_REFILL_DIV, 3, of real time while not slipping); needs the escalating guard.
        gc.slip = on && gc.escalate && vgx::EnvInt("GXV_SPEED_GUARD_SLIP", 0) != 0;
        gc.slipBandPermille = static_cast<unsigned>(std::min<long>(60, std::max<long>(5, vgx::EnvInt("GXV_SPEED_GUARD_SLIP_PM", 45))));
        gc.slipMaxNs = static_cast<int64_t>(std::min<long>(120, std::max<long>(1, vgx::EnvInt("GXV_SPEED_GUARD_SLIP_S", 10)))) * 1000000000ll;
        gc.slipRefillDiv = static_cast<unsigned>(std::min<long>(20, std::max<long>(1, vgx::EnvInt("GXV_SPEED_GUARD_SLIP_REFILL_DIV", 3))));
        gg.Configure(gc);
        if (gc.slip)
            port_log_summary("speed: slip band ON (GXV_SPEED_GUARD_SLIP=1): a measured loss of at most %u per mille of game speed is slipped (every picture stays evenly spaced) instead of dropping pictures, "
                             "for at most %.0f s of allowance (it refills at 1/%u of real time), then pictures are dropped as before; a slip is a pressure signal for the cuts",
                             gg.Cfg().slipBandPermille, gg.Cfg().slipMaxNs / 1e9, gg.Cfg().slipRefillDiv);
        if (on)
            port_log_summary("speed: guard ON (GXV_SPEED_GUARD=1): game time lost against the display's clock over %.0f ms is paid back with extra undrawn turns for the next pictures "
                             "(%s: at most %u extra turns a picture, at most %u %% of the pictures; the bucket forgives %u per mille of the wall time; the guard %s the late catch-up while it repays; "
                             "a whole number of turns a picture is %s; sustained drops are a signal for the cut systems: %s); loading, covered screens, the first %.1f s after them and gaps over 80 ms do not count",
                             gg.Cfg().budgetNs / 1e6, gg.Cfg().escalate ? "escalating: the number follows the learned need" : "one extra turn only",
                             gg.Cfg().escalate ? gg.Cfg().maxExtraEsc : gg.Cfg().maxExtra, gg.Cfg().maxSharePct, gg.Cfg().refillPermille, gg.Cfg().own ? "owns" : "leaves", gg.Cfg().snap ? "locked" : "not locked",
                             SpeedGuardSignalOn() ? "on" : "off", gg.Cfg().graceNs / 1e9);
        else
            port_log_summary("speed: guard off (census only; GXV_SPEED_GUARD=0 or a rhythm mode that owns the pacing)");
        return gg;
    }();
    return g;
}
extern "C" void port_q30lock_turn_gap(uint32_t gapUs, int counting);  // (recover30: below, the Quality ladder's slip meter)
// The start of every turn (port_frameskip_turn): the clock into the bucket and the census.
static void SpeedGuardTurnBegin(int allowed) {
    port::pace::SpeedGuard& g = SpeedGuardOnce();
    const int64_t now = port_host_time_ns();
    char line[640];
    static uint32_t lastKey = 0;
    static char stageName[56] = "?";
    const uint32_t key = port_scene_key();
    if (key != lastKey) {  // (the stage's end: its total, counted turns only)
        if (lastKey != 0 && g.Total().wallNs >= 2000000000ll) {
            port::pace::FormatSpeedLine2(line, sizeof line, stageName, " (stage end)", g.Total(), g.Enabled(), g.Cfg().budgetNs);   // (speed31: one grep, "game speed")
            port_log_summary("%s", line);
            if (port::pace::FormatSlipLine(line, sizeof line, stageName, " (stage end)", g.Total(), g) > 0) port_log_summary("%s", line);   // (pace32)
        }
        g.ClearTotal();
        lastKey = key;
        char nm[48];
        int sc = -1;
        if (port_scene_loading_stage(nm, sizeof nm, &sc) && nm[0]) snprintf(stageName, sizeof stageName, "%s%s%d", nm, sc >= 0 ? ":" : "", sc >= 0 ? sc : 0);
        else snprintf(stageName, sizeof stageName, "%08x", static_cast<unsigned>(key));
    }
    // (3.0 pacing rule, merge of c181 and 3.0: the speed guard counts and repays in a session too: pacing that keeps game time real-time is not held off, or a slow console would play in slow motion
    //  (the 14 Nov bug). c181's session hold on this test and on the filter below are removed TOGETHER; out of a session nothing differs: port::lockstep::On() was false there.)
    const bool quietNow = (port_screen_quiet && port_screen_quiet() != 0);
    g.BeginTurn(now, allowed != 0, quietNow);
    SpeedGuardAct(static_cast<uint64_t>(now / 1000));   // (speed31: the sustained drops are a signal for the cut systems)
    {   // (recover30: the turn gap into the Quality ladder's slip meter)
        static int64_t lastNs = 0;
        static bool lastCounting = false;
        const int64_t dt = lastNs ? now - lastNs : 0;
        port_q30lock_turn_gap(dt > 0 && dt < 1000000000ll ? static_cast<uint32_t>(dt / 1000) : 0u, lastCounting && dt > 0);
        lastNs = now;
        lastCounting = allowed != 0 && !quietNow;
    }
    static int64_t winStart = 0;
    if (!winStart) winStart = now;
    if (now - winStart >= 5000000000ll) {
        const port::pace::SpeedGuard::Census& w = g.Window();
        if (w.turns || w.wallNs || w.exclNs > 1000000000ll) {
            port::pace::FormatSpeedLine2(line, sizeof line, stageName, "", w, g.Enabled(), g.Cfg().budgetNs);
            port_log_summary("%s", line);
            if (port::pace::FormatSlipLine(line, sizeof line, stageName, "", w, g) > 0) port_log_summary("%s", line);   // (pace32: only in a window in which the band acted)
        }
        g.ClearWindow();
        winStart = now;
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
    // GXV_STALL_FORGIVE=<ms> (catchup25; default 0 = off): a turn that starts more than this long (80 ms .. 10 s) after the one before it is treated as on
    // time: the rule drops its "behind the display's clock" debt instead of paying it back with up to 8 turns without a picture (a jump of up to 4 turns
    // of game time after a stall of 80-250 ms; longer ones the rule's own 250 ms gap test already forgave). Same for the fixed-30 controller
    // (vgx_galaxy.cpp port_vgx_pace_turn) and the present clock (port_vgx_present_turn). Basic log: "pace: stall forgiven ..." per stall (20 at most).
    static const int64_t forgiveNs = port::pace::StallForgiveNs(vgx::EnvInt("GXV_STALL_FORGIVE", 0));
    static port::pace::LegacySkip rule(maxRun, pipeDepth, maxPipeRun, &port::pace::GlobalSlip(), maxStep, forgiveNs);
    // GXV_DEBT_CAP=<ms> (strk27; default 0 = 66.7 ms as before): the most time behind the display's clock the rule keeps (frame_pace.h DebtCapNs); the catch-up turns after a 33-80 ms turn.
    static const bool debtCapSet = [] {
        const long ms = vgx::EnvInt("GXV_DEBT_CAP", 0);
        const int64_t ns = port::pace::DebtCapNs(ms);
        rule.SetDebtCap(ns);
        if (ms > 0) port_log_summary("pace: debt cap on (GXV_DEBT_CAP=%ld): at most %.1f ms of lateness is paid back with turns without a picture (was 66.7)", ms, ns / 1e6);
        return ms > 0;
    }();
    (void)debtCapSet;
    // GXV_SKIP_PATTERN=N (1-3; default 0 = off): only N of every 4 turns are offered a picture, in a fixed rhythm (3: 45 a second,
    // holds 1-1-2), port::pace::SkipPattern (frame_pace.h). The rule below still decides for the offered turns; a turn it leaves out
    // passes the offer to the next one. A turn that is not offered takes its retrace (code 1), as one the back end is too busy for.
    // (warp32 PATCH7: GXV_HRW=5 with no pattern set runs the 2-of-4 rhythm its pairs need; frame_pace.h Hrw5Pacing)
    static port::pace::SkipPattern pattern([] { const int p = static_cast<int>(vgx::EnvInt("GXV_SKIP_PATTERN", 0)); return p == 0 && port::pace::Hrw5Pacing() ? 2 : p; }());
    unsigned pending = pipeDepth > 0 && port_vgx_backend_pending ? port_vgx_backend_pending() : 0;
    // GXV_PENDING_HOLD25 (pace25; default 0 = as before; GXV_PRESENT_SYNC=2 + GXV_LATENCY24): with the picture held to its slot L vblanks after its turn, about L - 2 pictures are with the back end
    // by design (display buffers, two-entry display queue), so at L 4-5 the rule's limit of 2 found the back end "busy" every turn and left one picture in five out for good: 48.0 fps with
    // the GPU at 15 ms and no late picture (night60 Beach Bowl, Good Egg). While every one of the last 32 pictures was on time at L (frame_pace.h PresentSync::HoldAllowance), L - 3 pictures
    // do not count as pending (limit L - 1). A late picture takes it away at once. Which pictures are drawn changes (pacing only: same game logic, same pixels). Every 5 s a "pace: pending hold" line.
    static const bool hold25 = vgx::EnvInt("GXV_PENDING_HOLD25", 0) != 0;
    if (hold25 && pending && port_vgx_pending_allowance) {
        static uint32_t guard25 = 0, turns25 = 0, applied25 = 0, saved25 = 0, allowSum25 = 0;
        static int64_t since25 = 0;
        const unsigned av25 = port_vgx_pending_allowance(), a = av25 & 0xFFu;
        guard25 = av25 >> 8;
        ++turns25;
        if (a) {
            ++applied25;
            allowSum25 += a;
            if (pending >= static_cast<unsigned>(pipeDepth) && (pending > a ? pending - a : 0) < static_cast<unsigned>(pipeDepth)) ++saved25;
            pending = pending > a ? pending - a : 0;
        }
        const int64_t now25 = port_host_time_ns();
        if (!since25) since25 = now25;
        if (now25 - since25 >= 5000000000ll) {
            port_log_summary("pace: pending hold (GXV_PENDING_HOLD25=1): %u turns with the back end holding a picture in %.1f s, the allowance applied in %u (mean %.1f pictures), %u turns whose pending count the allowance took under the limit (the busy-back-end skip not made), guard engaged %u (pace26, GXV_PENDING_HOLD25=2)",
                             turns25, (now25 - since25) / 1e9, applied25, applied25 ? static_cast<double>(allowSum25) / applied25 : 0.0, saved25, guard25);
            turns25 = applied25 = saved25 = allowSum25 = 0;
            since25 = now25;
        }
    }
    if (port::pace::GlobalSpeedGuard().Enabled()) rule.SetOwns(port::pace::GlobalSpeedGuard().Owns());   // (speed31: the guard repays while it is active: no late catch-up beside it)
    port::pace::LegacySkip::Verdict v = rule.Turn(port_host_time_ns(), allowed != 0, pending);
    if (port::pace::GlobalSpeedGuard().Enabled()) GuardMissNote(allowed != 0, v.behind || v.gpuBusy);   // (speed31)
    if (v.forgiven) {
        static unsigned logged = 0;
        if (logged++ < 20)
            port_log_summary("pace: stall forgiven (GXV_STALL_FORGIVE=%lld ms): a turn started %.0f ms after the one before; the %.0f ms of debt the skip rule would have paid back with turns without a picture is dropped (%llu so far)",
                             static_cast<long long>(forgiveNs / 1000000), v.gapNs / 1e6, v.debtNs / 1e6, static_cast<unsigned long long>(rule.Forgiven()));
    }
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
    bool patternOffered = false;
    bool noAssistOffered = false;
    // (gov5, GXV_HRW_GOV5: mode 5 on plain 60 (the pairs not engaged): the pattern stands down, every turn may draw, the legacy rule and the cadence decide as for the plain target)
    if (pattern.Enabled() && port_hrw_plain60 && port_hrw_plain60()) pattern.Park();
    else
    if (pattern.Enabled()) {
        const bool offered = pattern.Offer(allowed == 0);  // (a scene change is drawn at the normal pace and restarts the rhythm)
        if (allowed && !offered && v.code == 0) v.code = 1;
        patternOffered = offered || !allowed;
        pattern.Result(patternOffered, v.code == 0);
    }
    static port::pace::SkipPattern noAssistPattern(2);
    {   // even60 without 60 Assist (GXV_HRW=0, GXV_HRW_EVEN60_NOASSIST=1): 2-of-4 even-30 cadence when even60 chooses kEven30
        if (!pattern.Enabled() && !Pace25Enabled() && !Even26Enabled() && !SteadyEnabled() && port_hrw_even30_noassist) {
            if (port_hrw_even30_noassist() != 0) {
                const bool offered = noAssistPattern.Offer(allowed == 0);
                if (allowed && !offered && v.code == 0) v.code = 1;
                noAssistOffered = offered || !allowed;
                noAssistPattern.Result(noAssistOffered, v.code == 0);
            } else {
                noAssistPattern.Park();
            }
        }
    }
    if (Pace25Enabled()) v.code = Pace25Turn(allowed, v, pending);  // (GXV_PACE25: after the legacy rule and its cap, before the steady 30, which it excludes)
    if (Even26Enabled()) v.code = Even26Turn(allowed, v, pending);  // (GXV_EVEN26: the same place; not while the steady 30 is engaged)
    if (SteadyEnabled()) {
        const int before = v.code;
        v.code = SteadyTurn(allowed, v.code);
        if (v.code != before) v.behind = v.gpuBusy = false;  // (left out by the cadence: a paced skip, not a late or busy one)
    }
    if (port::pace::GlobalSpeedGuard().Enabled()) {  // (speed30, GXV_SPEED_GUARD: a picture that follows one undrawn turn is left out once more while the game time lost is over its budget; also in a session, see quietNow above)
        const int before = v.code;
        v.code = port::pace::GlobalSpeedGuard().Filter(v.code, allowed != 0);
        if (before == 0 && v.code != 0) {
            rule.ExtraTaken();                                   // (the step cap counts the turn; the slip's cadence gives its step back)
            if (patternOffered) pattern.Result(true, false);     // (wave8: pass the dropped offer to the next turn instead of a 4-turn 15 fps hold)
            if (noAssistOffered) noAssistPattern.Result(true, false);
            if (SteadyEnabled()) SteadyOnce().Decision(true, false);  // (the steady 30's rhythm: the next turn is offered again)
            v.behind = v.gpuBusy = false;
            if (RateWanted()) RateNoteGuardDrop();                    // (speed31: at 60 a picture the guard leaves out is a missed turn for the rate controller)
        }
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
// smooth25 (always on, one clock read and two relaxed atomics a turn): the gaps between the game thread's turn starts, for the "hitch:" line
// (platform/include/port/hitch24.h): a picture that came 500 ms after the one before is explained by a turn gap of 500 ms (the game thread's own
// time or a wait inside the turn) or by none (turns kept coming: the front end or the pacer). port_turn_gap_take returns the longest gap
// since the last call (us) and the turns counted, and starts over.
static std::atomic<int64_t> sTurnGapLastNs{0};
static std::atomic<uint32_t> sTurnGapMaxUs{0}, sTurnGapTurns{0};
extern "C" uint32_t port_turn_gap_take(uint32_t* turns) {
    if (turns) *turns = sTurnGapTurns.exchange(0, std::memory_order_relaxed);
    return sTurnGapMaxUs.exchange(0, std::memory_order_relaxed);
}
// (hitch25, GXV_HITCH25_LOG default 1, log only) A gap of GXV_HITCH25_TURN_MS (100) or more between two turn starts goes into a small ring with the screen's state
// at that moment and, when the turn profile ran (GXV_TURN_PROF=1 / GXV_FIRSTUSE_TURN=1), the largest part of the turn that just ended. The render thread takes the
// ring when it prints a stage's "hitch25:" line and decides there, from its own reveal time, which turns were behind the closed screen. GXV_HITCH25_TURN=1 also
// logs each such turn (at most 24 a stage). The turn profile's last turn is kept by port_turn_prof_end (game thread, the same thread as this).
static uint8_t sH25LastPart = port::hitch25::kPartNone;
static uint32_t sH25LastPartUs = 0, sH25LastTotalUs = 0;
static port::hitch25::Turn sH25Ring[port::hitch25::kTurnRing];
static unsigned sH25RingN = 0, sH25Dropped = 0;
static std::atomic_flag sH25Lock = ATOMIC_FLAG_INIT;
static bool Hitch25On() {
    static const bool on = [] { const char* e = getenv("GXV_HITCH25_LOG"); return !(e && *e == '0'); }();
    return on;
}
static uint32_t Hitch25TurnMs() {
    static const uint32_t ms = [] { const char* e = getenv("GXV_HITCH25_TURN_MS"); const int v = e && *e ? atoi(e) : 0; return static_cast<uint32_t>(v >= 20 ? v : port::hitch25::kSlowTurnMs); }();
    return ms;
}
extern "C" uint32_t port_hitch25_turns_take(port::hitch25::Turn* out, uint32_t cap, uint32_t* dropped) {
    while (sH25Lock.test_and_set(std::memory_order_acquire)) {}
    const uint32_t n = sH25RingN < cap ? sH25RingN : cap;
    for (uint32_t i = 0; i < n; ++i) out[i] = sH25Ring[i];
    if (dropped) *dropped = sH25Dropped + (sH25RingN - n);
    sH25RingN = 0;
    sH25Dropped = 0;
    sH25Lock.clear(std::memory_order_release);
    return n;
}
static void Hitch25SlowTurn(int64_t now, uint32_t gapUs) {
    port::hitch25::Turn t;
    t.endNs = now;
    t.gapUs = gapUs;
    const uint32_t key = port_scene_key();
    const int q = port_screen_quiet ? port_screen_quiet() : 0;
    t.state = (key == 0 || q == 1) ? port::hitch25::kMenu : q == 2 ? port::hitch25::kClosed : port::hitch25::kPlay;
    if (sH25LastPart != port::hitch25::kPartNone) {
        // the profile's last turn: its largest part, if the turn was most of the gap; else the gap was outside it (the retrace wait)
        const bool most = static_cast<uint64_t>(sH25LastTotalUs) * 2 >= gapUs;
        t.part = most ? sH25LastPart : static_cast<uint8_t>(port::hitch25::kPartOutside);
        t.partUs = most ? sH25LastPartUs : 0;
    }
    while (sH25Lock.test_and_set(std::memory_order_acquire)) {}
    if (sH25RingN < port::hitch25::kTurnRing) sH25Ring[sH25RingN++] = t;
    else ++sH25Dropped;
    sH25Lock.clear(std::memory_order_release);
    static const bool logEach = [] { const char* e = getenv("GXV_HITCH25_TURN"); return e && *e == '1'; }();
    if (logEach && port_log_summary) {
        static uint32_t forKey = 0;
        static unsigned lines = 0;
        if (key != forKey) { forKey = key; lines = 0; }
        if (lines++ < 24)
            port_log_summary("hitch25: game turn %u ms (%s; %s %u ms; stage key %08x)", gapUs / 1000,
                             t.state == port::hitch25::kClosed ? "screen closed by a wipe" : t.state == port::hitch25::kMenu ? "no stage / menu" : "screen open",
                             port::hitch25::PartName(t.part), t.partUs / 1000, key);
    }
}
// ---- endwait26 (GXV_ENDWAIT_LOG=1, default 0): the game thread's wait at the end of its frame, with the front end's and the back end's split of it (platform/vgx/src/vgx_endwait.h;
// the line is built in platform/src/gx/vgx_galaxy.cpp, which has the front end's per-picture record). Waits under GXV_ENDWAIT_MS (default 40) log nothing.
extern "C" void port_vgx_endwait_begin(void) __attribute__((weak));
extern "C" size_t port_vgx_endwait_line(int64_t t0, int64_t t1, int where, char* out, size_t cap) __attribute__((weak));
static bool EndWaitOn() {
    return PORT_HOT_KNOB(bool, [] { const char* e = getenv("GXV_ENDWAIT_LOG"); return e && *e == '1'; }());
}
extern "C" int64_t port_endwait_begin(void) {
    if (!EndWaitOn()) return 0;
    if (port_vgx_endwait_begin) port_vgx_endwait_begin();
    return port_host_time_ns();
}
extern "C" void port_endwait_end(int64_t t0, int where) {
    if (!t0) return;
    static const int64_t minNs = [] { const char* e = getenv("GXV_ENDWAIT_MS"); const int v = e && *e ? atoi(e) : 0; return (int64_t)(v >= 10 && v <= 2000 ? v : 40) * 1000000; }();
    const int64_t t1 = port_host_time_ns();
    if (t1 - t0 < minNs || !port_vgx_endwait_line || !port_log_summary) return;
    char text[2400];
    if (port_vgx_endwait_line(t0, t1, where, text, sizeof text)) port_log_summary("%s", text);
}
static void TurnGapNote() {
    const int64_t now = port_host_time_ns();
    const int64_t last = sTurnGapLastNs.exchange(now, std::memory_order_relaxed);
    sTurnGapTurns.fetch_add(1, std::memory_order_relaxed);
    if (last > 0 && now > last) {
        const int64_t us = (now - last) / 1000;
        const uint32_t v = us > 0xFFFFFFF0ll ? 0xFFFFFFF0u : static_cast<uint32_t>(us);
        if (v > sTurnGapMaxUs.load(std::memory_order_relaxed)) sTurnGapMaxUs.store(v, std::memory_order_relaxed);  // (one writer: the game thread)
        if (v >= Hitch25TurnMs() * 1000u && Hitch25On()) Hitch25SlowTurn(now, v);
    }
}
int port_frameskip_turn(int allowed) {
    TurnGapNote();
    SpeedGuardTurnBegin(allowed);
    if (SlipMax()) port::pace::GlobalSlip().Begin(static_cast<uint64_t>(port_host_time_ns() / 1000), allowed != 0);
    const int decision = FrameskipDecide(allowed);
    port::pace::GlobalSpeedGuard().EndDecision(decision);
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
// PETARI_BOOT (test harness) went straight into a stage and play began: there is no title, so the Vita's loading screen ends here
// (c104: with GXV_HRW=5 the warp's second pictures have no draws, and the 300-rich-pictures rule never ended it).
static std::atomic<int> sBootPlayBegan{0};
extern "C" void port_boot_play_began(void) { sBootPlayBegan.store(1, std::memory_order_relaxed); }
extern "C" int port_boot_play_has_begun(void) { return sBootPlayBegan.load(std::memory_order_relaxed); }

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
// GXV_DRAWPIPE=3 (dpengine; decomp/src/Game/Scene/SceneFunction.cpp): the draw thread draws the whole picture while the game thread
// waits at the hand-over; for that job it is "the frame thread" (the code under draw() that asks must get the answers it gets on
// the game thread). on = 1, on the draw thread before the draw: the mark names the caller; on = 0, after it: the game thread again.
// The game thread is asleep in between and every other thread's answer is "not me" for either value.
extern "C" void port_frame_thread_lend(int on) {
    static pthread_t sOwner;
#if defined(__vita__)
    static int sOwnerId;
#endif
    if (!sFrameThreadKnown.load(std::memory_order_relaxed)) {
        return;
    }
    if (on) {
        sOwner = sFrameThread.load(std::memory_order_relaxed);
        sFrameThread.store(pthread_self(), std::memory_order_relaxed);
#if defined(__vita__)
        sOwnerId = sFrameThreadId.load(std::memory_order_relaxed);
        sFrameThreadId.store(sceKernelGetThreadId(), std::memory_order_relaxed);
#endif
    } else {
        sFrameThread.store(sOwner, std::memory_order_relaxed);
#if defined(__vita__)
        sFrameThreadId.store(sOwnerId, std::memory_order_relaxed);
#endif
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
// Performance-mode visual budget (GXV_PERF_BUDGET), logged as "pb-count:" when it is on:
// 11 calcAnims left out (GXV_PB_ANIM)  12 flower/bush groups held clipped by the far distance (GXV_PB_DECOR), a count per turn
// 13 emitter emissions thinned (GXV_PB_PTCL)  14 shadows not projected or drawn because of the budget's far clip (GXV_PB_SHADOW)
// 15 fur shapes drawn thinned (GXV_PB_FUR)  16 fur layer draws saved
#if PORT_LOGGING
static uint64_t sLogicCount[20];
#endif
static uint64_t sFfPoseOut = 0;   // (far30: poses left out since the last far: line; game thread)
static uint32_t sFfIn = 0, sFfOut = 0;   // (far30: governor level moves toward / away from the cut, since the last far: line)
extern "C" void port_logic_count(int what, unsigned n) {
    if (what == 11) sFfPoseOut += n;
#if PORT_LOGGING
    if (what >= 0 && what < 20) sLogicCount[what] += n;
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
extern "C" void port_logic_prof_drop(void);  // (logic23: GXV_LOGIC_PROF's class rows, at the end of this file)
extern "C" void port_anim_prof_drop(void) {
    port_logic_prof_drop();
#if PORT_LOGGING
    memset(sProfRows, 0, sizeof(sProfRows));
#endif
}
// The end of a turn (GameSystem::frameLoop). Every 300 turns: the log lines, then the counts start again.
static void MoveProfTurn(int drawn);  // (hivelogic, at the end of this file)
static void HlVerifyTurn();          // (hivelogic: GXV_HL_AREACUBE=2 / GXV_HL_SENSOR=2, at the end of this file)
extern "C" void port_logic_prof_turn(int drawn);  // (logic23: GXV_LOGIC_PROF, at the end of this file)
extern "C" void port_dist_turn_end(int drawn);  // (far30: GXV_DISTCENSUS, ClippingActorHolder.cpp)
static void FarLineTurn();   // (far30: GXV_FARFIELD, the far: line every 5 s; below, after the governors' fader)
extern "C" void port_anim_prof_turn(int drawn) {
    port_dist_turn_end(drawn);
    FarLineTurn();
    MoveProfTurn(drawn);
    port_logic_prof_turn(drawn);
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
    if (port_pb_budget_on()) {
        port_log_summary("pb-count: per turn: anim_skipped=%.1f decor_groups_clipped=%.1f ptcl_thinned=%.1f shadows_culled=%.1f fur_thinned=%.1f fur_layers_saved=%.1f",
                 sLogicCount[11] / turns, sLogicCount[12] / turns, sLogicCount[13] / turns, sLogicCount[14] / turns, sLogicCount[15] / turns,
                 sLogicCount[16] / turns);
    }
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
    // 1: stages and the 24 costliest draw lists (every 300 drawn turns); 2 (flushcut, 7 Nov): stages and counts only, one basic-log line every 5 s
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_DRAW_PROF"); return e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0; }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
#else
    return 0;
#endif
}
// GXV_FLUSH_PREFETCH (flushcut, 7 Nov; default 1; LiveActor.cpp port_anim_flush_stale, ModelManager.cpp port_dl_flush_stale,
// DrawBufferExecuter.cpp calcViewAndEntry): the drawn turn's flushes and the view pass issue cache prefetches for the models a few steps
// ahead in their lists. A prefetch changes no value anywhere (PLD never faults, writes nothing); only when the data arrives. 0: none.
extern "C" int port_flush_prefetch_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_FLUSH_PREFETCH"); return e && *e == '0' ? 0 : 1; }());  // (GXV_HOT_KNOBS)
}
// GXV_DL_STATIC (flushcut, 7 Nov; default 0 until a verify run passes; ModelManager.cpp port_dl_flush_stale): a listed model whose
// materials cannot change (no material animation, no display-list maker, no texture matrix in any material) computed its materials
// once in full; the flush leaves out the repeats. 1: on. 2: verify (every repeat is computed in full and the bytes it writes are
// compared with what was there; "dl-static verify" in the draw-prof line, mismatches must stay 0).
extern "C" int port_dl_static_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_DL_STATIC"); return e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0; }());  // (GXV_HOT_KNOBS)
}
// GXV_DL_STATIC_FLAGFIX (cpu60, 10 Nov; default 1, only acts with GXV_DL_STATIC): the post-texture-matrix state a repeat is compared with is
// the model data's own (what calcMaterial sets from it), not the j3dSys flag the previously listed model happened to leave. 0: as flushcut
// wrote it (a list of mixed kinds then computes in full again and again).
extern "C" int port_dl_static_flagfix_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_DL_STATIC_FLAGFIX"); return e && *e == '0' ? 0 : 1; }());  // (GXV_HOT_KNOBS)
}
// GXV_DL_STATIC_TM (game24, 10 Nov; default 0; only acts with GXV_DL_STATIC): a plain (mode 0) texture matrix no longer keeps a model out of the
// repeat test; the model is left out only while the content of those matrices (and their texgen fields) is what the last full computation
// left (ModelManager.cpp DlStaticTmHash). GXV_DL_STATIC=2 with it verifies that by hash before/after a full recomputation.
extern "C" int port_dl_static_tm_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_DL_STATIC_TM"); return e && *e == '1' ? 1 : 0; }());  // (GXV_HOT_KNOBS)
}
// GXV_DL_STATIC_XANIM (gamecpu25, 12 Nov; default 0; only acts with GXV_DL_STATIC): the repeat test looks at getJ3DModel() instead of
// ModelManager::mModel, which is null for every model with a BCK (ModelManager.cpp, DlStaticModelOf). 2 = the same and the draw-prof line says so.
extern "C" int port_dl_static_xanim_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_DL_STATIC_XANIM"); return e && *e >= '1' && *e <= '9' ? 1 : 0; }());  // (GXV_HOT_KNOBS)
}
// GXV_PTCL_THIN (gamecpu25, 12 Nov; default 0; stage-settable ptcl_thin / ptcl_thin_d0 / ptcl_thin_d1; port/ptcl_thin.h): the fraction of the particles of an
// emitter far from the camera that is still drawn (0 = off; 0.35 = 35 % of them at D1 and beyond, falling linearly from 100 % at D0). Mode 2 (dry): counts only.
// (fe26: GXV_FE_GOV's level, platform/src/port/perf.cpp below; the governor's value of a member is the umbrella table's row of that level, and the cut takes the stricter of
// that and its own static value, port/fegov.h; level 0 returns the static value untouched)
extern "C" int port_fegov_level(void);
static int LockFrom();      // (q30lock26, below: the fader of port/lockfade.h; the cuts that are scalars are interpolated between the two ends of a walk)
static int LockTo();
static bool LockBusy();
static float LockFrac();
static float FeGovTableL(int lv, const char* name, float dflt) {
    float v = dflt;
    port::cpulod::TableValue(lv, name, &v);
    return v;
}
static float FeGovTable(const char* name, float dflt) { return FeGovTableL(port_fegov_level(), name, dflt); }
static float PtclThinStaticMin() {
    return PORT_STAGE_KNOB(float, [] { float v = 0.0f; const char* e = getenv("GXV_PTCL_THIN"); if (e && *e) v = static_cast<float>(atof(e)); port::stageknob::Lookup("GXV_PTCL_THIN", &v); return (v > 0.0f && v < 1.0f) ? v : 0.0f; }());
}
static float PtclKeepAt(int lv) {
    const float b = PtclThinStaticMin();
    return lv > 0 ? port::fegov::KeepMin(b, FeGovTableL(lv, "GXV_PTCL_THIN", 0.0f)) : b;
}
extern "C" int port_pb_demo_off(void);            // (popin31: LodCtrl.cpp; GXV_PB_DEMO=2 in a flight)
extern "C" float port_pb_release_ptcl_w(void);    // (popin31: LodCtrl.cpp; the thinning's weight after a flight, 1 = as before)
static float PtclThinMinRaw() {
    if (!LockBusy()) return PtclKeepAt(LockTo());
    // (q30lock26: while a level change is walked the kept fraction moves from one end's to the other's, monotonic, a few per mille a turn: the far particles fade out / in over the walk)
    const float a = PtclKeepAt(LockFrom()), z = PtclKeepAt(LockTo());
    const float ea = a > 0.0f ? a : 1.0f, ez = z > 0.0f ? z : 1.0f;
    const float r = ea + (ez - ea) * LockFrac();
    return r >= 0.999f ? 0.0f : r;
}
extern "C" float port_ptcl_thin_min(void) {
    const float r = PtclThinMinRaw();
    if (!(r > 0.0f)) return r;
    // popin31 (B1): GXV_PB_DEMO=2 turns the budget off in a flight; this thinning (a member of the same Performance budget, set by the stage table) did not ask. Off in a
    // flight, and after it back over the release's ramp (the far particles fade in, never all at once). GXV_PB_DEMO 0 / 1: the weight is 1 and nothing changes.
    if (port_pb_demo_off()) return 0.0f;
    const float w = port_pb_release_ptcl_w();
    if (w >= 1.0f) return r;
    const float m = port::popin::RampedFactor(r, w);
    return m >= 0.999f ? 0.0f : m;
}
extern "C" int port_ptcl_thin_dry(void) {
    return PORT_STAGE_KNOB(int, [] { const char* e = getenv("GXV_PTCL_THIN_DRY"); return e && *e == '1' ? 1 : 0; }());
}
extern "C" float port_ptcl_thin_d0(void) {
    const float b = PORT_STAGE_KNOB(float, [] { float v = 2500.0f; const char* e = getenv("GXV_PTCL_THIN_D0"); if (e && *e) v = static_cast<float>(atof(e)); port::stageknob::Lookup("GXV_PTCL_THIN_D0", &v); return v >= 300.0f ? v : 300.0f; }());
    const auto at = [&](int lv) { return lv > 0 ? port::fegov::DistFor(PtclThinStaticMin() > 0.0f ? 2 : 0, b, 2, FeGovTableL(lv, "GXV_PTCL_THIN_D0", b)) : b; };
    if (!LockBusy()) return at(LockTo());
    const float a = at(LockFrom()), z = at(LockTo());
    return a + (z - a) * LockFrac();
}
extern "C" float port_ptcl_thin_d1(void) {
    const float b = PORT_STAGE_KNOB(float, [] { float v = 7000.0f; const char* e = getenv("GXV_PTCL_THIN_D1"); if (e && *e) v = static_cast<float>(atof(e)); port::stageknob::Lookup("GXV_PTCL_THIN_D1", &v); return v >= 400.0f ? v : 400.0f; }());
    const auto at = [&](int lv) { return lv > 0 ? port::fegov::DistFor(PtclThinStaticMin() > 0.0f ? 2 : 0, b, 2, FeGovTableL(lv, "GXV_PTCL_THIN_D1", b)) : b; };
    if (!LockBusy()) return at(LockTo());
    const float a = at(LockFrom()), z = at(LockTo());
    return a + (z - a) * LockFrac();
}
// far30 (GXV_FARFIELD tier 4): the draw distance of small decoration and props, from the governors' depth (a walk between two levels is a straight line in depth, so the horizon sweeps with the
// walk, GXV_GOV_FADE_MS, never jumps). 0 = off (GXV_FARFIELD=0, or the depth has no draw tier yet).
static float FfDepth() { return port::farfield::DepthOf(LockFrom(), LockTo(), LockFrac(), LockBusy()); }
// far3 / far30: the far field is in use (Performance's GXV_FARFIELD, or the Quality ladder is armed): LodCtrl.cpp then classifies the flight each turn
static std::atomic<int> sQFarOn{0};   // (the Quality far steps are permitted in this stage: GXV_Q30_FAR=0 leaves it 0, and then nothing of the far field runs in Quality)
extern "C" int port_ff_wants_flight(void) { return (port::cpulod::FarOn() || sQFarOn.load(std::memory_order_relaxed) != 0) ? 1 : 0; }
// the spread (game units) of a plant group's own distance: a stable per-group hash decides, 0 = off (no far-field step on, so today's PB_DECOR is exactly as before)
extern "C" float port_ff_decor_spread(void) {
    if (port::cpulod::FarOn()) return 1000.0f;
    return port_q30lock_budget_w(4) > 0.0f ? 1000.0f : 0.0f;
}
// the height under which the draw tier may take an object, pixels of 544 (GXV_FARFIELD_PX, default 12: see ClippingActorHolder.cpp)
extern "C" float port_ff_draw_px(void) {
    return PORT_STAGE_KNOB(float, [] { float v = port::stageknob::Num("GXV_FARFIELD_PX", 12.0f); return v < 4.0f ? 4.0f : (v > 40.0f ? 40.0f : v); }());
}
extern "C" int port_ff_draw_last(void) { return port::cpulod::FarOn() && port::cpulod::LastOn() ? 1 : 0; }
extern "C" float port_ff_draw_horizon(void) {
    float h = 0.0f;
    if (port::cpulod::FarOn()) {
        const float p = port::farfield::Plan(FfDepth(), port_ff_draw_last() != 0).draw;
        h = p >= port::farfield::kOff ? 0.0f : p;
    }
    const float w = port_q30lock_budget_w(5);   // (far3, step 3k: the Quality ladder's faded draw distance for small decoration)
    if (w > 0.0f) { const float q = port::farfield::qfar::Sweep(port::farfield::qfar::kQDraw, w); if (h <= 0.0f || q < h) h = q; }
    return h;
}
// far3, step 3l: the ambient actors' half-pace distance (0 = off); read by ActorTurnLod.cpp
// (MP3 checks83: a function of measured frame times, so 0 in a lockstep session: consoles of different speed would run different logic)
extern "C" float port_ql_logic_limit(void) {
    if (PortLockstepOn() || port::lockstep::On()) return port::lockstep::QualityLogicLimit(0.0f, true);   // (fmt32 + checks83: the same session flag, read two ways)
    return port_q30lock_budget_w(6) > 0.0f ? port::farfield::qfar::Sweep(port::farfield::qfar::kQLogic, port_q30lock_budget_w(6)) : 0.0f;
}
extern "C" void port_ff_census(unsigned long long* held, unsigned long long* faded, unsigned long long* pops, float* popPx);   // (ClippingActorHolder.cpp)
static void FarLineTurn() {
#if PORT_LOGGING
    float qw = 0.0f;   // (the Quality far steps' weights 3h..3l)
    for (int i = 2; i < 7; ++i) qw += port_q30lock_budget_w(i);
    if (!port::cpulod::FarOn() && !(qw > 0.0f)) return;
    static uint64_t t0 = 0;
    const uint64_t now = port::q30lock::NowUs();
    if (!t0) { t0 = now; return; }
    if (now - t0 < 5000000ull) return;
    const uint64_t t0Prev = t0;
    t0 = now;
    unsigned long long held = 0, faded = 0, pops = 0;
    float popPx = 0.0f;
    port_ff_census(&held, &faded, &pops, &popPx);
    const int turns = 300;   // (a 5 s window at 60 turns a second; the sums are per window, the line says so)
    port::farfield::Census c;
    c.level = LockTo();
    if (port::cpulod::FarOn()) {
        c.depth = FfDepth();
        const port::farfield::Horizons h = port::farfield::Plan(c.depth, port_ff_draw_last() != 0);
        c.decor = h.decor; c.anim = h.rate; c.lod = h.lod; c.draw = h.draw; c.band = h.fadeBand; c.poseN = h.poseN;
    } else {   // Quality: depth = the sum of the steps' weights (0..5), the horizons are the steps' own
        namespace qf = port::farfield::qfar;
        const auto hz = [](float t, float w) { return w > 0.0f ? qf::Sweep(t, w) : port::farfield::kOff; };
        c.depth = qw;
        c.decor = hz(qf::kQDecor, port_q30lock_budget_w(4)); c.anim = hz(qf::kQRate, port_q30lock_budget_w(2));
        c.lod = qf::LodScale(port_q30lock_budget_w(3)); c.draw = hz(qf::kQDraw, port_q30lock_budget_w(5));
        c.band = port::farfield::kFadeBand; c.poseN = port_q30lock_budget_w(2) > 0.0f ? 2 : 1;
    }
    c.heldDraws = static_cast<uint32_t>(held / turns);
    c.fadedObjects = static_cast<uint32_t>(faded / turns);
    c.poseLeftOut = static_cast<uint32_t>(sFfPoseOut / turns);
    c.movedIn = sFfIn; c.movedOut = sFfOut;
    c.pops = static_cast<uint32_t>(pops); c.popPx = popPx; c.secs = static_cast<float>(now - t0Prev) / 1e6f;
    sFfPoseOut = 0; sFfIn = sFfOut = 0;
    char buf[320];
    port::farfield::Format(buf, sizeof buf, sLsStage, c);
    port_log_summary("%s", buf);
#endif
}
// GXV_DRAW_PREFETCH (game24, 10 Nov; default 0): J3DDrawBuffer::drawHead / drawTail ask for the next packet's first 64 bytes before drawing the current one.
extern "C" int port_draw_prefetch_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_DRAW_PREFETCH"); return e && *e == '1' ? 1 : 0; }());  // (GXV_HOT_KNOBS)
}
#if PORT_LOGGING
namespace {
constexpr int kDrawRows = 0x100;   // 0x00-0x5F exec, 0x60-0x8F opa, 0x90-0xBF xlu, 0xC0-0xC2 entry
constexpr int kDrawStages = 23;  // (20-22: gamecpu23 draw parts, port_draw_part_end)
uint64_t sDrawRowNs[kDrawRows];
uint32_t sDrawRowCalls[kDrawRows];
uint64_t sDrawStageNs[kDrawStages];
uint32_t sDrawTurns = 0;
int sDrawDepth = 0;
int64_t sDrawT0 = 0;
const char* const kDrawStageNames[kDrawStages] = {"draw", "scene", "seq", "mirror", "3d", "2d", "flush-anim", "flush-dl", "flush-view", "pointer", "tail",
                                                  "begin-render", "end-render", "settle", "turn", "3d-pre", "3d-opa", "3d-xlu", "3d-fx", "3d-post",
                                                  "shadows", "effects", "layouts"};
// flushcut counts (per flush, added up by the flushes; per drawn turn in the line): 0 far-model joints computed, 1 listed but left out
// (dead, clipped, hidden or no longer an actor), 2 material lists updated, 3 material lists left out by GXV_DL_STATIC, 4 listed but not an
// actor's model any more, 5 GXV_DL_STATIC=2 repeats compared, 6 GXV_DL_STATIC=2 repeats that differed, 7 view pass: models handed to a job
constexpr int kDrawCounts = 20;  // (19: game24, packets GXV_DRAW_PREFETCH asked ahead for)  // (8..16: cpu60, GXV_DL_STATIC's reasons, ModelManager.cpp DlWhy)
uint64_t sDrawCnt[kDrawCounts];
int64_t sDrawFrameT0 = 0;
int64_t sDrawWinT0 = 0;
uint32_t sDrawWinTurns = 0;
}  // namespace
#endif
// imp45 (22 Nov): GXV_DRAW_PROF_ROWS=1 (default 0; acts with GXV_DRAW_PROF=2): the per-list rows are timed in mode 2 as well and the 16 costliest go to the
// basic log after each 5 s line ("draw-prof: row #k <kind>-<index> ..."; kind exec = a draw type's category list, opa / xlu = a draw buffer's opaque /
// translucent pass, entry 0 2D, 1 3D, 2 mirror). Two clock reads a list: a profile arm, not a timing arm. The console build has no other way to say which
// lists the 5-7 ms of a heavy drawn turn's draw go to (the rank rows of mode 1 are port_log lines, which the player build does not write).
#if PORT_LOGGING
static int DrawProfRows() {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_DRAW_PROF_ROWS"); return e && *e == '1' ? 1 : 0; }());
}
static bool DrawProfRowsTimed() {
    const int m = port_draw_prof_on();
    return m == 1 || (m == 2 && DrawProfRows() != 0);
}
#endif
// row: (kind << 8) | index, kind 0 exec, 1 opa, 2 xlu, 3 entry (index 0 2D, 1 3D, 2 mirror). Game thread only.
extern "C" void port_draw_prof_begin(void) {
#if PORT_LOGGING
    if (!DrawProfRowsTimed()) return;  // (mode 2 without GXV_DRAW_PROF_ROWS: stages only, no per-list clock reads)
    if (sDrawDepth++ == 0) sDrawT0 = port_host_time_ns();
#endif
}
extern "C" void port_draw_prof_end(int row) {
#if PORT_LOGGING
    if (!DrawProfRowsTimed()) return;
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
// The start of a drawn turn's frame loop (GameSystem::frameLoop, before the skip decision): the "turn" stage runs from here to the end of calc-anim.
extern "C" void port_draw_prof_frame_begin(void) {
#if PORT_LOGGING
    if (port_draw_prof_on()) sDrawFrameT0 = port_host_time_ns();
#endif
}
// The drawn turn's flushes add up what they did (see kDrawCounts). Called once per flush, only while the profile is on.
extern "C" void port_draw_prof_count(int what, unsigned n) {
#if PORT_LOGGING
    if (what >= 0 && what < kDrawCounts) sDrawCnt[what] += n;
#else
    (void)what; (void)n;
#endif
}
// The end of a drawn turn (GameSystem::frameLoop).
// GXV_DRAW_PROF=2, every 5 s, one line in the basic log:
//   draw-prof: 5.0 s, 140 drawn turns; ms per drawn turn: turn 23.4 [begin-render .. calc-anim], draw 11.2 [scene 8.9 (mirror 0.0, 3d 7.1 (pre .. post), 2d 1.7),
//   seq 0.8, pointer .., tail ..], begin-render .., end-render .., settle .., flushes 5.3 [anim 1.9 dl 2.3 view 1.0]; per drawn turn: anim flush actors N (left out M), ...
extern "C" void port_draw_prof_turn(void) {
#if PORT_LOGGING
    const int mode = port_draw_prof_on();
    if (!mode) return;
    const int64_t now = port_host_time_ns();
    if (sDrawFrameT0 > 0 && now > sDrawFrameT0) sDrawStageNs[14] += static_cast<uint64_t>(now - sDrawFrameT0);
    ++sDrawTurns;
    if (mode == 2) {
        if (sDrawWinT0 == 0) { sDrawWinT0 = now; sDrawTurns = 0; memset(sDrawStageNs, 0, sizeof sDrawStageNs); memset(sDrawCnt, 0, sizeof sDrawCnt); return; }
        if (now - sDrawWinT0 < 5000000000LL) return;
    } else if (sDrawTurns < 300) {
        return;
    }
    const double n = sDrawTurns;
    const double secs = mode == 2 ? (now - sDrawWinT0) / 1e9 : 0.0;
    auto ms = [&](int s) { return sDrawStageNs[s] / 1e6 / n; };
    char line[1536];
    int o;
    if (mode == 2) {
        o = snprintf(line, sizeof line, "draw-prof: %.1f s, %u drawn turns (%.1f/s); ms per drawn turn: turn %.2f, draw %.2f [scene %.2f (mirror %.2f, 3d %.2f (pre %.2f opa %.2f xlu %.2f fx %.2f post %.2f), 2d %.2f), seq %.2f, pointer %.2f, tail %.2f], "
                     "begin-render %.2f, end-render %.2f, settle %.2f, flushes %.2f [anim %.2f dl %.2f view %.2f]; per drawn turn: far joints %.1f (left out %.1f), material lists %.1f "
                     "(left out by dl-static %.1f, not a model %.1f), view jobs %.1f",
                     secs, sDrawTurns, sDrawTurns / secs, ms(14), ms(0), ms(1), ms(3), ms(4), ms(15), ms(16), ms(17), ms(18), ms(19), ms(5), ms(2), ms(9), ms(10), ms(11), ms(12), ms(13),
                     ms(6) + ms(7) + ms(8), ms(6), ms(7), ms(8), sDrawCnt[0] / n, sDrawCnt[1] / n, sDrawCnt[2] / n, sDrawCnt[3] / n, sDrawCnt[4] / n, sDrawCnt[7] / n);
        if (o < static_cast<int>(sizeof line) - 96) {  // (gamecpu23: the draw parts, wherever they fall in the stages above)
            o += snprintf(line + o, sizeof line - o, "; draw parts: shadows %.2f, effects %.2f, layouts %.2f", ms(20), ms(21), ms(22));
        }
        if (port_draw_prefetch_on() != 0 && o < static_cast<int>(sizeof line) - 80) {
            o += snprintf(line + o, sizeof line - o, "; GXV_DRAW_PREFETCH: %.0f packets a turn asked ahead for", sDrawCnt[19] / n);
        }
        if (port_dl_static_on() == 2 && o < static_cast<int>(sizeof line) - 80) {
            o += snprintf(line + o, sizeof line - o, "; dl-static verify: %llu repeats compared, %llu differed", (unsigned long long)sDrawCnt[5], (unsigned long long)sDrawCnt[6]);
        }
        if (port_dl_static_on() != 0 && o < static_cast<int>(sizeof line) - 420) {  // (cpu60: why the material lists were or were not left out)
            snprintf(line + o, sizeof line - o, "; dl-static why a turn (GXV_DL_STATIC=%d, flagfix %d): left out %.1f, in full: first or flag changed %.1f, words differ %.1f, "
                     "maker %.1f, btk/brk/btp/bpk player %.1f, material animation %.1f, plain texture matrix only %.1f, other texture matrix %.1f, no data/shape %.1f; GXV_DL_STATIC_TM=%d: plain texture matrix content changed %.1f, left out with plain texture matrices %.1f",
                     port_dl_static_on(), port_dl_static_flagfix_on(), sDrawCnt[8] / n, sDrawCnt[9] / n, sDrawCnt[10] / n, sDrawCnt[11] / n, sDrawCnt[12] / n,
                     sDrawCnt[13] / n, sDrawCnt[14] / n, sDrawCnt[15] / n, sDrawCnt[16] / n, port_dl_static_tm_on(), sDrawCnt[17] / n, sDrawCnt[18] / n);
        }
        port_log_summary("%s", line);
        if (DrawProfRows() != 0) {  // (imp45: the costliest lists of the window, in the basic log)
            bool shownRow[kDrawRows] = {};
            uint64_t covered = 0;
            for (int r = 0; r < kDrawRows; ++r) covered += sDrawRowNs[r];
            port_log_summary("draw-prof: rows (GXV_DRAW_PROF_ROWS=1): the timed lists cover %.2f ms of a drawn turn", covered / 1e6 / n);
            for (int rank = 0; rank < 16; ++rank) {
                int best = -1;
                for (int r = 0; r < kDrawRows; ++r) {
                    if (!shownRow[r] && sDrawRowCalls[r] && (best < 0 || sDrawRowNs[r] > sDrawRowNs[best])) best = r;
                }
                if (best < 0) break;
                shownRow[best] = true;
                const char* kind = best < 0x60 ? "exec" : best < 0x90 ? "opa" : best < 0xC0 ? "xlu" : "entry";
                const int index = best < 0x60 ? best : best < 0x90 ? best - 0x60 : best < 0xC0 ? best - 0x90 : best - 0xC0;
                port_log_summary("draw-prof: row #%d %s-%02X calls/drawn=%.2f ms/drawn=%.3f us/call=%.1f", rank + 1, kind, index, sDrawRowCalls[best] / n,
                                 sDrawRowNs[best] / 1e6 / n, sDrawRowNs[best] / 1000.0 / sDrawRowCalls[best]);
            }
            memset(sDrawRowNs, 0, sizeof sDrawRowNs);
            memset(sDrawRowCalls, 0, sizeof sDrawRowCalls);
        }
        sDrawWinT0 = now;
        sDrawTurns = 0;
        memset(sDrawStageNs, 0, sizeof sDrawStageNs);
        memset(sDrawCnt, 0, sizeof sDrawCnt);
        return;
    }
    o = snprintf(line, sizeof line, "draw-prof: drawn=%u us/drawn:", sDrawTurns);
    for (int s = 0; s < kDrawStages && o < static_cast<int>(sizeof line) - 32; ++s) {
        if (*kDrawStageNames[s]) o += snprintf(line + o, sizeof line - o, " %s=%.1f", kDrawStageNames[s], sDrawStageNs[s] / 1000.0 / n);
    }
    uint64_t covered = 0;
    for (int r = 0; r < kDrawRows; ++r) covered += sDrawRowNs[r];
    if (o < static_cast<int>(sizeof line) - 32) snprintf(line + o, sizeof line - o, " rows=%.1f", covered / 1000.0 / n);
    port_log_summary("%s", line);
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
    memset(sDrawCnt, 0, sizeof sDrawCnt);
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
// GXV_SENSOR_BP (update23, 8 Nov; exact; port/sensor_bp.h): SensorHitChecker's pair loops (GXV_HL_SENSOR's copies, so only with it on)
// look only at the pairs that can hit, in the original order. 0: the plain loops. 1: on. 2: verify (the original loops run after, their
// lists are used; the "hl-verify:" line's sensor counts and "MISMATCH" lines are the check, as GXV_HL_SENSOR=2's).
extern "C" int port_sensor_bp_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_SENSOR_BP"); return e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0; }());  // (GXV_HOT_KNOBS)
}
// GXV_UPD_PREFETCH (update23, 8 Nov; exact: cache hints only, no value read for a result changes): the update's pointer-chasing loops
// (every category list's objects, the clipping judge's actors, the sensor checker's sensors and hosts) ask for the lines of the next
// objects while the current one runs. 0: off. 1: on.
extern "C" int port_upd_prefetch_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_UPD_PREFETCH"); return e && *e == '1' ? 1 : 0; }());  // (GXV_HOT_KNOBS)
}
// GXV_UPD_PREFETCH2 (update25, 11 Nov; exact: cache hints only): on top of GXV_UPD_PREFETCH (the next objects' own lines), the lines of the
// parts of the NEXT actor of a LiveActor category list that its movement reads first (model manager, nerve, sensor keeper, binder, effect
// keeper, sound object, light controller, shadow list), asked for from the pointers in its already-cached header, while the current one runs.
// 0: off. 1: on.
extern "C" int port_upd_prefetch2_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_UPD_PREFETCH2"); return e && *e == '1' ? 1 : 0; }());  // (GXV_HOT_KNOBS)
}
// GXV_UPD_PREFETCH3 (update26, 12 Nov; exact: cache hints only; decomp/src/Game/NameObj/NameObjCategoryList.cpp): the list loop runs three stages ahead
// (the object's lines 3 on, its parts' first lines 2 on, the blocks behind the parts 1 on: the J3D model and pose player, the sensor infos, the animation
// keeper's infos) for the movement AND the calcAnim lists of LiveActors. Needs GXV_UPD_PREFETCH=1 (its first stage). 0: off (default). 1: on.
// Census, about every 5 s (logging builds): "upd26: PREFETCH3: N hints for M objects a turn-list pass, K objects not in the snapshot" (counts per second).
extern "C" int port_upd_prefetch3_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_UPD_PREFETCH3"); return e && *e == '1' ? 1 : 0; }());  // (GXV_HOT_KNOBS)
}
namespace {
uint32_t sU26Hints = 0, sU26Objs = 0, sU26Unknown = 0, sU26Tick = 0;
int64_t sU26Last = 0;
}  // namespace
extern "C" void port_upd26_count(int what, unsigned n) {
    if (what == 0) {
        sU26Hints += n;
        ++sU26Objs;
    } else {
        sU26Unknown += n;
        return;
    }
#if PORT_LOGGING
    if ((++sU26Tick & 255u) == 0) {  // (the clock once per 256 objects)
        const int64_t now = port_host_time_ns();
        if (sU26Last == 0) {
            sU26Last = now;
        }
        if (now - sU26Last < 5000000000ll) {
            return;
        }
        sU26Last = now;
        port_log_summary("upd26: GXV_UPD_PREFETCH3: %u hints for %u objects, %u objects not in the snapshot (since the line before; %.1f hints an object)",
                         sU26Hints, sU26Objs, sU26Unknown, sU26Objs ? static_cast< double >(sU26Hints) / sU26Objs : 0.0);
        sU26Hints = sU26Objs = sU26Unknown = 0;
    }
#endif
}
// GXV_UPD_SNDNAME (update25, 11 Nov; exact; AudSoundNameConverter.cpp): a sound asked for by name found through a hash index instead of a scan of
// the name table from its category's first entry. 0 off, 1 on, 2 verify (both ways, the scan's answer used).
extern "C" int port_upd_sndname_mode(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_UPD_SNDNAME"); return e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0; }());  // (GXV_HOT_KNOBS)
}
// GXV_UPD_LIVESET (update25, 11 Nov; exact; LiveActor.cpp port_live_actor_valid): the flushes' "is it still one of the scene's actors"
// answered from a hash table of the sorted snapshot instead of a binary search. 0 off, 1 on, 2 verify (both answers, the search's used).
extern "C" int port_upd_liveset_mode(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_UPD_LIVESET"); return e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0; }());  // (GXV_HOT_KNOBS)
}
// GXV_HOTPROF_LIVESNAP (hotprof25, 11 Nov; exact; LiveActor.cpp port_live_actor_snapshot): "the scene's actor group is what it was at the last
// snapshot" from the groups' write count (NameObjGroup.cpp) instead of a walk of the ~2000 entries, 3.6 times a picture. 0 the walk
// (default until a verify run), 1 the count, 2 verify (both, the walk's answer used, "live-snap:" lines).
extern "C" int port_hotprof_livesnap_mode(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_HOTPROF_LIVESNAP"); return e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0; }());  // (GXV_HOT_KNOBS)
}
// GXV_HOTPROF_REQSCAN (hotprof25, 11 Nov; exact; NameObjExecuteHolder.cpp): the scans for objects asking to leave the movement or draw lists
// walk only the span of entries a request was made for since the last scan. 0 the whole array (default until a verify run), 1 the span, 2 verify.
extern "C" int port_hotprof_reqscan_mode(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_HOTPROF_REQSCAN"); return e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0; }());  // (GXV_HOT_KNOBS)
}
// GXV_HOTPROF_PTCLCULL (hotprof25, 11 Nov; picture-exact; JPAResource.cpp): a particle quad wholly outside one plane of the clip volume is
// not sent to the renderer (the front end's GXV_DYNCULL test, made before the quad is sent). 0 off (default), 1 on, 2 dry (counted only).
extern "C" int port_hotprof_ptclcull_mode(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_HOTPROF_PTCLCULL"); return e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0; }());  // (GXV_HOT_KNOBS)
}
// GXV_PTCL_PRE (perf32, 20 Nov; exact; JPAResource.cpp, port/ptcl_cull.h QuadPreOutside): with GXV_HOTPROF_PTCLCULL, a particle quad far outside the
// clip volume is answered from its centre instead of its four or eight vertices; the quads left out are the same. 0 off (default until a console
// A/B), 1 on, 2 verify (the kernel decides; "ptcl-cull: pre:" counts a centre answer the kernel contradicts: must be 0).
extern "C" int port_ptcl_pre_mode(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_PTCL_PRE"); return e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0; }());  // (GXV_HOT_KNOBS)
}
// GXV_HOTPROF_TEVCOL (hotprof25 round 2, 11 Nov; exact; decomp/src/RVL_SDK/gx/GXTev.c): GXSetTevColor's BG word written once, not three
// times, and inside a particle loop a word equal to the loop's last for its register not written. 0 off (default), 1 on, 2 verify
// (everything written, the words 1 would leave out announced to the front end, which checks them: "tevcol verify:").
extern "C" int port_hotprof_tevcol_mode(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_HOTPROF_TEVCOL"); return e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0; }());  // (GXV_HOT_KNOBS)
}
// GXV_HOTPROF_REINITGX (hotprof25 round 2, 11 Nov; exact; decomp/src/RVL_SDK/gx/GXTev.c port_gx_reinit_tev, J3DSys.cpp): J3DSys::reinitGX's
// reset of the 16 TEV stages (144 GX calls) done by one function that writes each register once with its final value. 0 off (default),
// 1 on, 2 verify (the calls as before; the library's state after them compared with what the one function makes: "reinitgx:" line).
extern "C" int port_hotprof_reinitgx_mode(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_HOTPROF_REINITGX"); return e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0; }());  // (GXV_HOT_KNOBS)
}
// GXV_HOTPROF_PTCLCENSUS (hotprof25 round 2; a log line only; JPAResource.cpp): with GXV_HOTPROF_PTCLCULL=1 or 2, a "ptcl-census:" line
// with every "ptcl-cull:" line: the particle resources with the most off-screen quads since the line before, by name. 0 off (default).
extern "C" int port_hotprof_ptclcensus_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_HOTPROF_PTCLCENSUS"); return e && *e == '1' ? 1 : 0; }());  // (GXV_HOT_KNOBS)
}
// GXV_HOTPROF_WPAD (hotprof25 round 2, 11 Nov; exact; decomp/src/Game/System/WPadAcceleration.cpp): while a remote's 128 acceleration
// samples are one finite value (a handheld that is not simulating a shake), the mean, the rotation outputs and the average distance are
// the known ones and the three history loops are not run. 0 off (default), 1 on, 2 verify (loops run and compared: "wpad-still:" line).
extern "C" int port_hotprof_wpad_mode(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_HOTPROF_WPAD"); return e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0; }());  // (GXV_HOT_KNOBS)
}
// GXV_HOTPROF_NOFLUSHSTRIP (hotprof25, 11 Nov; picture-exact; decomp/src/RVL_SDK/gx/GXGeometry.c __GXSendFlushPrim): the all-zero flush
// strips the GX library sends in front of particle and shadow primitives (372 a picture in the profiled run; the front end drops every
// one) are not written into the stream. 0 off (default), 1 on.
extern "C" int port_hotprof_noflushstrip_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_HOTPROF_NOFLUSHSTRIP"); return e && *e == '1' ? 1 : 0; }());  // (GXV_HOT_KNOBS)
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
        const char* c = getenv("GXV_SENSOR_BP");  // (update23: its verify reports through the same counters)
        return (a && *a == '2') || (b && *b == '2') || (c && *c == '2');
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

// ---------------------------------------------------------------------------------------------------------------------------------
// gamepar (8 Nov): the game thread's Performance-mode cuts (knobs) and a cheap per-category timing line (GXV_TURN_PROF).
#include "port/turn_prof.h"
//
// Performance mode is GXV_AUTOQ_TARGET=60 without the fixed 30 cap (as GXV_STEADY30 and GXV_PERF_BUDGET tell it); a change of picture mode
// relaunches the game, so it is read once. (The visual budget, GXV_PB_*, already halves far models' joints (GXV_PB_ANIM) and far-clips
// shadows (GXV_PB_SHADOW); the cuts here are the ones it does not make.) A "mode knob" is 0 (off), 1 (on in every mode), 2 (on + verify where the cut has one) or "perf" (the
// default in vita/vgx-game/entry.cpp): on in Performance mode only, off in Quality and Locked 30.
extern "C" int port_perf_mode_60(void) {
    return PORT_HOT_KNOB(int, [] {
        const char* t = getenv("GXV_AUTOQ_TARGET");
        const char* f = getenv("GXV_FPS_LIMIT");
        return (t && atoi(t) == 60 && !(f && atoi(f) == 30)) ? 1 : 0;
    }());
}
// ---------------------------------------------------------------------------------------------------------------------------------
// honey25 (12 Nov): GXV_CPUGOV, a governor for the far-draw cut (platform/include/port/cpugov.h has the rule and the knobs). The back end's
// floor report (vgx_backend_gl.cpp FloorReport, every 5 s) hands over its window; a stage preset's load resets; the game thread reads the level
// (ClippingActorHolder.cpp: the far-draw accessors combine it with the static values; one relaxed load a call, 0 when off).
#include "port/cpugov.h"
namespace {
std::atomic<int> sCpuGovLevel{0};
std::mutex sCpuGovLock;  // (the back end calls both functions; the lock only keeps a test harness honest)
port::cpugov::Governor sCpuGov;
unsigned sCpuGovWindows = 0;
long CpuGovKnob(const char* name, long dflt, long lo, long hi) {
    const long v = port::stageknob::Int(name, dflt);
    return v < lo ? lo : (v > hi ? hi : v);
}
port::cpugov::Config CpuGovConfig() {
    port::cpugov::Config c;
    c.maxLevel = static_cast<int>(CpuGovKnob("GXV_CPUGOV", 0, 0, port::cpugov::kMaxLevel));
    // Performance mode only (the owner's rule: Quality keeps its detail), unless GXV_CPUGOV_ANY=1
    if (c.maxLevel > 0 && !port_perf_mode_60() && CpuGovKnob("GXV_CPUGOV_ANY", 0, 0, 1) == 0) c.maxLevel = 0;
    c.latePct = static_cast<int>(CpuGovKnob("GXV_CPUGOV_LATE", 15, 1, 100));
    c.cpuPct = static_cast<int>(CpuGovKnob("GXV_CPUGOV_CPU", 50, 0, 100));
    c.calmPct = static_cast<int>(CpuGovKnob("GXV_CPUGOV_CALM", 3, 0, 50));
    c.upWindows = static_cast<int>(CpuGovKnob("GXV_CPUGOV_UP", 1, 1, 12));
    c.downWindows = static_cast<int>(CpuGovKnob("GXV_CPUGOV_DOWN", 6, 1, 120));
    return c;
}
}  // namespace
// ---------------------------------------------------------------------------------------------------------------------------------
// fe26 (12 Nov): GXV_FE_GOV, the front end's governor (platform/include/port/fegov.h has the rule and the knobs). The back end's floor report hands
// over the window's front-end CPU time; the governor's level is read by the cuts' accessors (ClippingActorHolder.cpp, port_ptcl_thin_* below) and by
// port_cpugov_level (the far-draw cut takes the larger of the two governors). One relaxed load a call; 0 when off.
#include "port/fegov.h"
#include "port/anticipate.h"
namespace {
constexpr float kGovRecoverDefault = 1.0f;   // (recover30: GXV_FE_GOV_RECOVER / GXV_GAME_GOV_RECOVER: the give-back follows the room; tests/recover30_gov_test.cpp shows no flapping with level costs 0.5x and 2x off the guess)
port::q30lock::Meter sFeMeter, sGameMeter;   // (recover30: the "recover:" census of the governors: taken = level above 0)
std::atomic<uint64_t> sGuardPressUs{0};   // (speed31: the last time the speed guard's repayment was sustained)
static bool GuardPressedRecently() { const uint64_t at = sGuardPressUs.load(std::memory_order_relaxed); return at != 0 && port::q30lock::NowUs() - at < 8000000u; }
std::atomic<int> sFeGovLevel{0};
std::atomic<int> sRateGive{0};     // (rate30: the even 30 stands and the governors give quality back: they judge against the 30 budgets)
std::atomic<int> sAntLevels{0};    // (anticip32: GXV_ANTICIPATE asks for this many cut levels on top of the governors' while an announced heavy moment lasts; 0 = none)
std::atomic<int> sAntWalkMs{0};    // (anticip32: ... and the fader walks them in this fast, so they are in place when the moment begins)
std::atomic<int> sGameGovLevel{0};  // (cpu2-26: GXV_GAME_GOV's level; the cuts read the larger of the two)
std::mutex sFeGovLock;
port::fegov::Governor sFeGov;
unsigned sFeGovWindows = 0;
uint32_t sFeGovUp = 0, sFeGovDown = 0;  // raises / lowerings since the stage's load
float sFeGovMeanSum = 0.0f;             // census: mean of the windows' means since the last summary line
unsigned sFeGovMeanN = 0;
float FeGovKnob(const char* name, float dflt, float lo, float hi) {
    const float v = port::stageknob::Num(name, dflt);
    return v < lo ? lo : (v > hi ? hi : v);
}
// pace32 (owner, 13 Nov 21:30: "try to create some overhead room so that we aren't right at the limits all the time"): GXV_HEADROOM_PCT=<70..100> (default 0 = off = today's targets exactly).
// Every controller that aims at a time budget aims at this share of the refresh (16.68 ms) or of the pair (33.37 ms) instead, when that is LOWER than its own target: the governors' budgets
// (GXV_FE_GOV_MS / GXV_GAME_GOV_MS, also the half-rate world's pair budgets), the Quality ladder's limits (GXV_Q30_LOCK_P95_MS / _GPU_MS). A key set by hand still wins (the value here is only the
// key's default). Quality comes back only on proof: the governors give a level back when the mean plus the level's learned gain fits under the LOWERED budget less the band, and the Quality
// ladder's "proof of headroom" slack (GXV_Q30_LOCK_PROBE_MS, 1 ms over the limit) is 0 while this is on. The rate controller's lanes (14.5 = 87 %, 30 = 90 %) are already under 90 %.
static long HeadroomPct() {
    static const long pct = [] { const long v = vgx::EnvInt("GXV_HEADROOM_PCT", 0); return v <= 0 ? 0 : v < 70 ? 70 : v > 100 ? 100 : v; }();
    return pct;
}
static float HeadroomCap(float dflt, bool pair) {
    const long pct = HeadroomPct();
    if (!pct) return dflt;
    const float cap = (pair ? 33.367f : 16.683f) * static_cast<float>(pct) / 100.0f;
    return cap < dflt ? cap : dflt;
}
// one line per stage load and governor: the proof that the target moved, and by how much
static void HeadroomLog(const char* who, float was, float now, bool pair) {
    if (!HeadroomPct()) return;
    port_log_summary("headroom: GXV_HEADROOM_PCT=%ld: %s aims at %.1f ms (was %.1f; %.1f %% of the %s's %.2f ms; %.1f ms kept free)", HeadroomPct(), who, now, was,
                     100.0 * now / (pair ? 33.367 : 16.683), pair ? "pair" : "refresh", pair ? 33.367 : 16.683, (pair ? 33.367 : 16.683) - now);
}
// q30lock26: the Quality (30) mode's lock ladder is wanted (GXV_Q30_LOCK, default 1 in Quality, set by port/quality30.h): the 5-second governors then stand down, the ladder supersedes them.
bool LockWanted() { return port::stageknob::Quality() && FeGovKnob("GXV_Q30_LOCK", 0, 0, 1) != 0; }
extern "C" int port_hrw_pair_budgets(float* feMs, float* gameMs) __attribute__((weak));  // (hrw28; vgx_galaxy.cpp: 1 while HRW is wanted: the governors aim at the pair's budgets)
// hrw28: the budget a governor aims at: the pair's while HRW is pending / engaged / declined for fit (it then raises the cpulod level only as far as the PAIR needs, and lowers it again, with
// its own hysteresis and the shared fader, as soon as the pair fits without it), else `dflt`. An explicit GXV_*_GOV_MS still wins (it is read through the same knob).
static float HrwPairBudget(bool game, float dflt) {
    float fe = 0, gm = 0;
    if (port_hrw_pair_budgets && port_hrw_pair_budgets(&fe, &gm)) return game ? gm : fe;
    return dflt;
}
// far30 (GXV_FARFIELD): the last resort (level 4) needs GXV_FARFIELD_LAST=1, else the governors stop at 3; the stage's opening level (GXV_FARFIELD_START, the stage table's row) is where the
// governors start, and the give-back rules walk them down from there.
static void FarFieldLimits(port::fegov::Config* c) {
    if (c->maxLevel > 3 && !(port::cpulod::FarOn() && port::cpulod::LastOn())) c->maxLevel = 3;
    int start = 0;
    if (c->maxLevel > 0 && port::cpulod::FarOn()) {
        start = static_cast<int>(port::stageknob::Num("GXV_FARFIELD_START", 0.0f));
        if (start < 0) start = 0;
        if (start > c->maxLevel) start = c->maxLevel;
    }
    c->startLevel = start;
}
// c161 (14 Nov, owner: the full far-cut step is "another rung on the ladder" before the 60 gives way): the stage's own static cpulod level already holds the rate cuts up to that level, so
// the governor's steps under it change nothing; its floor is that level less one (Performance only, and only when the cap allows a step above it). The first raise is then the one that adds
// the drawn cuts of the level above (model detail, plants, particles, shadows), and a give-back walks back to the floor, not through dead steps.
static void StaticFloor(port::fegov::Config* c) {
    if (c->maxLevel < 2 || port::stageknob::Quality()) return;
    const int s = port::cpulod::Level();
    if (s < 2) return;
    c->minLevel = s - 1 < c->maxLevel - 1 ? s - 1 : c->maxLevel - 1;
    if (c->startLevel < c->minLevel) c->startLevel = c->minLevel;
}
port::fegov::Config FeGovConfig() {
    port::fegov::Config c;
    c.maxLevel = static_cast<int>(FeGovKnob("GXV_FE_GOV", 0, 0, port::fegov::kMaxLevel));
    // Performance mode, or (q30-26) the Quality (30) mode at level 1 = the q level of the cpulod table (the barely-noticeable members only) with the 30 fps budget below; else GXV_FE_GOV_ANY=1
    // (q30lock26: not under the lock ladder, which supersedes it)
    if (c.maxLevel > 0 && port::stageknob::Quality()) c.maxLevel = LockWanted() ? 0 : (c.maxLevel > 1 ? 1 : c.maxLevel);
    else if (c.maxLevel > 0 && !port_perf_mode_60() && FeGovKnob("GXV_FE_GOV_ANY", 0, 0, 1) == 0) c.maxLevel = 0;
    {   // (pace32: GXV_HEADROOM_PCT lowers the default target; an explicit GXV_FE_GOV_MS still wins)
        const bool pair30 = port::stageknob::Quality() || sRateGive.load(std::memory_order_relaxed) != 0;
        const float was = pair30 ? port::fegov::kBudget30Ms : HrwPairBudget(false, 15.5f);
        const float aim = HeadroomCap(was, pair30 || was > 16.7f);
        c.budgetMs = FeGovKnob("GXV_FE_GOV_MS", aim, 6.0f, 40.0f);
        static float logged = -1.0f;
        if (c.maxLevel > 0 && c.budgetMs != logged) { logged = c.budgetMs; HeadroomLog("the front end's governor", was, c.budgetMs, pair30 || was > 16.7f); }
    }
    c.bandMs = FeGovKnob("GXV_FE_GOV_BAND", 0.7f, 0.0f, 10.0f);
    c.upWindows = static_cast<int>(FeGovKnob("GXV_FE_GOV_UP", 2, 1, 12));
    c.downWindows = static_cast<int>(FeGovKnob("GXV_FE_GOV_DOWN", 6, 1, 120));
    // recover30: the give-back follows the room (port/fegov.h); 0 = today's rule
    c.recover = FeGovKnob("GXV_FE_GOV_RECOVER", kGovRecoverDefault, 0, 1) != 0;
    c.downMinWindows = static_cast<int>(FeGovKnob("GXV_FE_GOV_DOWN_MIN", 2, 1, 120));
    c.fullMarginMs = FeGovKnob("GXV_FE_GOV_MARGIN", 3.0f, 0.5f, 20.0f);
    c.multi = FeGovKnob("GXV_FE_GOV_MULTI", 1, 0, 1) != 0;
    FarFieldLimits(&c);
    StaticFloor(&c);   // (c161)
    return c;
}
}  // namespace
// (cpu2-26) the level the cuts read: the larger of the front end's governor and the game thread's
static int GovMax() {
    const int a = sFeGovLevel.load(std::memory_order_relaxed), b = sGameGovLevel.load(std::memory_order_relaxed);
    return a > b ? a : b;
}
static int LockLevelNow();
static int LockLevelFor(uint32_t seed);
// The level the cuts read: the stricter of the two ends while a change is being walked in (q30lock26), else the governors' (or the lock's) level.
extern "C" int port_fegov_level(void) { return LockLevelNow(); }
// ... and the level THIS actor reads (seed: its address >> 4): the actors move to the new level one after the other over the walk, never all in one turn.
extern "C" int port_fegov_level_for(uint32_t seed) { return LockLevelFor(seed); }
extern "C" void port_fegov_reset(void) {
    std::lock_guard<std::mutex> lock(sFeGovLock);
    const int was = sFeGovLevel.load(std::memory_order_relaxed);
    {   // (recover30) the stage's end census, then a new stage's clock
        const uint64_t nowU = port::q30lock::NowUs();
        if (sFeMeter.Used()) { char l[200]; sFeMeter.Format(l, sizeof l, "fegov (level above 0), stage end", nowU); port_log_summary("%s", l); }
        sFeMeter.Start(nowU);
    }
    sFeGov.Configure(FeGovConfig());
    sFeGovLevel.store(sFeGov.level(), std::memory_order_relaxed);   // (far30: the stage's opening level; 0 unless GXV_FARFIELD_START)
    sFeGovUp = sFeGovDown = 0;
    sFeGovMeanSum = 0.0f;
    sFeGovMeanN = 0;
    if (was != 0) port_log_summary("fegov: reset at a stage load (was level %d)", was);
}
extern "C" void port_fegov_window(uint32_t pictures, uint32_t frontPics, uint64_t frontSumUs, uint32_t stalls, uint32_t quiet) {
    std::lock_guard<std::mutex> lock(sFeGovLock);
    if (port::ab::Frozen()) return;  // (ab27: GXV_AB pins the level while it measures)
    const port::fegov::Config c = FeGovConfig();
    sFeGov.Update(c);
    if (c.maxLevel <= 0) {
        if (sFeGovLevel.load(std::memory_order_relaxed) != 0) {
            sFeGovLevel.store(0, std::memory_order_relaxed);  // (switched off by a preset or a mode change: the cuts are let go at once)
            port_log_summary("fegov: off (GXV_FE_GOV 0 or not Performance mode): level 0");
        }
        return;
    }
    port::fegov::Window w;
    w.pictures = pictures; w.frontPics = frontPics; w.frontSumUs = frontSumUs; w.stalls = stalls; w.quiet = quiet;
    w.guard = GuardPressedRecently() || sAntLevels.load(std::memory_order_relaxed) != 0;   // (speed31) (anticip32: a window measured with the pre-scale in force is never calm: no give-back from it)
    const int before = sFeGov.level();
    const bool changed = sFeGov.Feed(w);
    const int after = sFeGov.level();
    sFeGovLevel.store(after, std::memory_order_relaxed);
    if (after > before) AntSpikePost(port::anticipate::kSrcGov);   // (anticip32: the place is remembered)
    if (sAntLevels.load(std::memory_order_relaxed) != 0) sFeGov.VoidPending();   // (anticip32: a gain measured with the pre-scale in force is not the level's)
    {   // (recover30)
        const uint64_t nowU = port::q30lock::NowUs();
        if (!sFeMeter.begin) sFeMeter.begin = nowU;
        if (before == 0 && after > 0) sFeMeter.Take(nowU);
        if (before > 0 && after == 0) sFeMeter.Give(nowU);
        if (sFeMeter.Used()) { char l[200]; sFeMeter.Format(l, sizeof l, "fegov (level above 0)", nowU); port_log_summary("%s", l); }
    }
    ++sFeGovWindows;
    if (sFeGov.last() != port::fegov::Verdict::Ignored) { sFeGovMeanSum += sFeGov.lastMean(); ++sFeGovMeanN; }
    if (changed) { if (after > before) ++sFeGovUp; else ++sFeGovDown; }
    static const char* const kVerdict[] = {"ignored", "hot", "calm", "middle"};
    const long every = static_cast<long>(FeGovKnob("GXV_FE_GOV_LOG", 6, 0, 1000));
    if (changed || (every > 0 && sFeGovWindows % static_cast<unsigned>(every) == 0)) {
        port_log_summary("fegov: %s level %d -> %d of %d; window %s: front end %.2f ms a picture (%u of %u pictures measured, budget %.1f, band %.1f); mean of the windows since the last line %.2f; "
                         "step gain L1 %.2f%s L2 %.2f%s ms; hot run %d of %d, calm run %d of %d; %u raises, %u lowerings since the stage load; %u stalls, %u quiet",
                         changed ? "CHANGE" : "steady", before, after, c.maxLevel, kVerdict[static_cast<int>(sFeGov.last())], static_cast<double>(port::fegov::MeanMs(w)), frontPics, pictures,
                         static_cast<double>(c.budgetMs), static_cast<double>(c.bandMs), sFeGovMeanN ? static_cast<double>(sFeGovMeanSum / sFeGovMeanN) : 0.0,
                         static_cast<double>(sFeGov.GainOf(1)), sFeGov.GainKnown(1) ? " (measured)" : " (assumed)", static_cast<double>(sFeGov.GainOf(2)),
                         sFeGov.GainKnown(2) ? " (measured)" : " (assumed)", sFeGov.hotRun(), c.upWindows, sFeGov.calmRun(), sFeGov.downNeed(), sFeGovUp, sFeGovDown, stalls, quiet);
        sFeGovMeanSum = 0.0f;
        sFeGovMeanN = 0;
    }
}
// ---------------------------------------------------------------------------------------------------------------------------------
// cpu2-26 (12 Nov): GXV_GAME_GOV, the GAME THREAD's governor. The same rule as fe26's (port/fegov.h Governor, one instance more), another measure: the game thread's own
// CPU ms a DRAWN turn, summed by GameGovBegin from consecutive turn-begin samples (revB: thread CPU time, so a wait for the front end does not count; see port::fegov::TurnCpuDelta) and taken
// at each 5-second floor window (port_gamegov_window, called by the back end next to port_fegov_window). Hot = the window's mean drawn turn above GXV_GAME_GOV_MS (15.5)
// for GXV_GAME_GOV_UP (2) windows: the level goes up one step; calm = mean + the measured gain of the level still fits budget - GXV_GAME_GOV_BAND (0.7) for
// GXV_GAME_GOV_DOWN (6) windows: one step down; backoff after a lowering that was followed by a hot window. The level it reaches is the one the cuts read
// (the larger of this and GXV_FE_GOV's: port_fegov_level), so one table (port/cpulod.h, levels 0..3) serves both and the cuts take the stricter of their static value and
// the level's row. Why a second governor and not CPUGOV: CPUGOV (honey25) counts LATE PICTURES blamed on the CPU and raises only the far-draw cut; it cannot tell
// 17 ms from 22 ms, learns no gain, and leaves every other member of the table alone. This one reads milliseconds and drives all of them. It does not replace CPUGOV: both
// may be on (the far-draw cut takes the largest level asked).
// GXV_GAME_GOV=<0..3> (0 off, default; stage key game_gov; Performance mode only unless GXV_GAME_GOV_ANY=1), _MS, _BAND, _UP, _DOWN, _LOG as for GXV_FE_GOV.
namespace {
std::mutex sGameGovLock;
port::fegov::Governor sGameGov;
std::atomic<int> sGgEnabled{0};          // read by the game thread every turn: the governor's max level > 0 (set by the config, never by the turn itself)
std::atomic<uint64_t> sGgUs{0}, sGgTurns{0};  // the window's drawn turns: CPU microseconds and count (game thread adds, the back end takes)
std::atomic<int64_t> sGgPrev{0};         // revB: the thread-CPU sample of the previous turn begin (the game thread writes it; a stage load zeroes it)
bool sGgPrevDrawn = false;               // game thread only: the turn that began at sGgPrev was a drawn one
unsigned sGameGovWindows = 0;
uint32_t sGameGovUp = 0, sGameGovDown = 0;
float sGameGovMeanSum = 0.0f;
unsigned sGameGovMeanN = 0;
port::fegov::Config GameGovConfig() {
    port::fegov::Config c;
    c.maxLevel = static_cast<int>(FeGovKnob("GXV_GAME_GOV", 0, 0, port::fegov::kMaxLevel));
    if (c.maxLevel > 0 && port::stageknob::Quality()) c.maxLevel = LockWanted() ? 0 : (c.maxLevel > 1 ? 1 : c.maxLevel);   // (q30lock26: 0 under the lock, which supersedes it) (q30-26: allowed in Quality at level 1 = the q level, the 30 fps budget)
    else if (c.maxLevel > 0 && !port_perf_mode_60() && FeGovKnob("GXV_GAME_GOV_ANY", 0, 0, 1) == 0) c.maxLevel = 0;
    {   // (pace32: as the front end's)
        const bool pair30 = port::stageknob::Quality() || sRateGive.load(std::memory_order_relaxed) != 0;
        const float was = pair30 ? port::fegov::kGameBudget30Ms : HrwPairBudget(true, 15.5f);
        const float aim = HeadroomCap(was, pair30 || was > 16.7f);
        c.budgetMs = FeGovKnob("GXV_GAME_GOV_MS", aim, 6.0f, 40.0f);
        static float logged = -1.0f;
        if (c.maxLevel > 0 && c.budgetMs != logged) { logged = c.budgetMs; HeadroomLog("the game thread's governor", was, c.budgetMs, pair30 || was > 16.7f); }
    }
    c.bandMs = FeGovKnob("GXV_GAME_GOV_BAND", 0.7f, 0.0f, 10.0f);
    c.upWindows = static_cast<int>(FeGovKnob("GXV_GAME_GOV_UP", 2, 1, 12));
    c.downWindows = static_cast<int>(FeGovKnob("GXV_GAME_GOV_DOWN", 6, 1, 120));
    c.recover = FeGovKnob("GXV_GAME_GOV_RECOVER", kGovRecoverDefault, 0, 1) != 0;   // (recover30)
    c.downMinWindows = static_cast<int>(FeGovKnob("GXV_GAME_GOV_DOWN_MIN", 2, 1, 120));
    c.fullMarginMs = FeGovKnob("GXV_GAME_GOV_MARGIN", 3.0f, 0.5f, 20.0f);
    c.multi = FeGovKnob("GXV_GAME_GOV_MULTI", 1, 0, 1) != 0;
    c.minPictures = 60;
    FarFieldLimits(&c);
    StaticFloor(&c);   // (c161)
    return c;
}
}  // namespace
static void GameGovBegin() {
    {   // core3-38: one line per stage load, 5 s after it: who was busy on the fourth core in the 10 s around it (cheap: one atomic load until a load is waiting)
        char l[640];
        if (port::core3::Poll(l, sizeof l)) port_log_summary("%s", l);
        // mp3c24: the online flag for the discretionary core-3 workers (GXV_MP_CORE3=1) and its line (session on, then every 10 s while online; the call is the same
        // either way: with the switch off it only logs the duty so the A/B has its baseline). One relaxed load while single player.
        if (port::core3::PollMp(mp_entered(), l, sizeof l)) port_log_summary("%s", l);
    }
    if (__builtin_expect(sGgEnabled.load(std::memory_order_relaxed) == 0, 1)) { sGgPrev.store(0, std::memory_order_relaxed); sGgPrevDrawn = false; return; }
    // revB: the turn that ended at this begin costs (this sample - the previous begin's): see port::fegov::TurnCpuDelta (the counter is exact only after a wait)
    const int64_t now = port_thread_cpu_us();
    int64_t us = 0;
    if (port::fegov::TurnCpuDelta(sGgPrev.load(std::memory_order_relaxed), now, sGgPrevDrawn, 100000, &us)) {
        sGgUs.fetch_add(static_cast<uint64_t>(us), std::memory_order_relaxed);
        sGgTurns.fetch_add(1, std::memory_order_relaxed);
    }
    sGgPrev.store(now, std::memory_order_relaxed);
    sGgPrevDrawn = false;
}
static void GameGovEnd() {
    // (only the turns that make a picture count: the undrawn ones are paid on top at 30, not at 60; the flag is read at the next begin)
    sGgPrevDrawn = sGgEnabled.load(std::memory_order_relaxed) != 0 && !sWorkUndrawn;
}
extern "C" void port_gamegov_reset(void) {
    if (port_card_write_flush) port_card_write_flush(0);  // (hitch30: a stage load: the queued card writes are done behind the cover, not later in play)
    std::lock_guard<std::mutex> lock(sGameGovLock);
    const int was = sGameGovLevel.load(std::memory_order_relaxed);
    {   // (recover30)
        const uint64_t nowU = port::q30lock::NowUs();
        if (sGameMeter.Used()) { char l[200]; sGameMeter.Format(l, sizeof l, "gamegov (level above 0), stage end", nowU); port_log_summary("%s", l); }
        sGameMeter.Start(nowU);
    }
    const port::fegov::Config c = GameGovConfig();
    sGameGov.Configure(c);
    sGameGovLevel.store(sGameGov.level(), std::memory_order_relaxed);
    sGgEnabled.store(c.maxLevel > 0 ? 1 : 0, std::memory_order_relaxed);
    sGgUs.store(0, std::memory_order_relaxed);
    sGgTurns.store(0, std::memory_order_relaxed);
    sGgPrev.store(0, std::memory_order_relaxed);  // (revB: the first turn after a load has no previous sample)
    sGameGovUp = sGameGovDown = 0;
    sGameGovMeanSum = 0.0f;
    sGameGovMeanN = 0;
    if (was != 0) port_log_summary("gamegov: reset at a stage load (was level %d)", was);
}
extern "C" void port_gamegov_window(uint32_t pictures, uint32_t stalls, uint32_t quiet) {
    std::lock_guard<std::mutex> lock(sGameGovLock);
    if (port::ab::Frozen()) return;  // (ab27)
    const port::fegov::Config c = GameGovConfig();
    sGameGov.Update(c);
    sGgEnabled.store(c.maxLevel > 0 ? 1 : 0, std::memory_order_relaxed);
    const uint64_t us = sGgUs.exchange(0, std::memory_order_relaxed), turns = sGgTurns.exchange(0, std::memory_order_relaxed);
    if (c.maxLevel <= 0) {
        if (sGameGovLevel.load(std::memory_order_relaxed) != 0) {
            sGameGovLevel.store(0, std::memory_order_relaxed);  // (switched off by a preset or a mode change: the cuts are let go at once)
            port_log_summary("gamegov: off (GXV_GAME_GOV 0 or not Performance mode): level 0");
        }
        return;
    }
    port::fegov::Window w;
    w.pictures = pictures; w.frontPics = static_cast<uint32_t>(turns); w.frontSumUs = us; w.stalls = stalls; w.quiet = quiet;  // (the Window's "front" fields carry the drawn turns here)
    w.guard = GuardPressedRecently() || sAntLevels.load(std::memory_order_relaxed) != 0;   // (speed31) (anticip32)
    const int before = sGameGov.level();
    const bool changed = sGameGov.Feed(w);
    const int after = sGameGov.level();
    sGameGovLevel.store(after, std::memory_order_relaxed);
    if (after > before) AntSpikePost(port::anticipate::kSrcGov);   // (anticip32)
    if (sAntLevels.load(std::memory_order_relaxed) != 0) sGameGov.VoidPending();
    {   // (recover30)
        const uint64_t nowU = port::q30lock::NowUs();
        if (!sGameMeter.begin) sGameMeter.begin = nowU;
        if (before == 0 && after > 0) sGameMeter.Take(nowU);
        if (before > 0 && after == 0) sGameMeter.Give(nowU);
        if (sGameMeter.Used()) { char l[200]; sGameMeter.Format(l, sizeof l, "gamegov (level above 0)", nowU); port_log_summary("%s", l); }
    }
    ++sGameGovWindows;
    if (sGameGov.last() != port::fegov::Verdict::Ignored) { sGameGovMeanSum += sGameGov.lastMean(); ++sGameGovMeanN; }
    if (changed) { if (after > before) ++sGameGovUp; else ++sGameGovDown; }
    static const char* const kVerdict[] = {"ignored", "hot", "calm", "middle"};
    const long every = static_cast<long>(FeGovKnob("GXV_GAME_GOV_LOG", 6, 0, 1000));
    if (changed || (every > 0 && sGameGovWindows % static_cast<unsigned>(every) == 0)) {
        port_log_summary("gamegov: %s level %d -> %d of %d (cuts read %d with the front end's); window %s: game thread %.2f ms a drawn turn (%u drawn turns, %u pictures, budget %.1f, band %.1f); "
                         "mean of the windows since the last line %.2f; step gain L1 %.2f%s L2 %.2f%s L3 %.2f%s ms; hot run %d of %d, calm run %d of %d; %u raises, %u lowerings since the stage load; %u stalls, %u quiet",
                         changed ? "CHANGE" : "steady", before, after, c.maxLevel, port_fegov_level(), kVerdict[static_cast<int>(sGameGov.last())], static_cast<double>(port::fegov::MeanMs(w)),
                         static_cast<unsigned>(turns), pictures, static_cast<double>(c.budgetMs), static_cast<double>(c.bandMs),
                         sGameGovMeanN ? static_cast<double>(sGameGovMeanSum / sGameGovMeanN) : 0.0,
                         static_cast<double>(sGameGov.GainOf(1)), sGameGov.GainKnown(1) ? " (measured)" : " (assumed)", static_cast<double>(sGameGov.GainOf(2)),
                         sGameGov.GainKnown(2) ? " (measured)" : " (assumed)", static_cast<double>(sGameGov.GainOf(3)), sGameGov.GainKnown(3) ? " (measured)" : " (assumed)",
                         sGameGov.hotRun(), c.upWindows, sGameGov.calmRun(), sGameGov.downNeed(), sGameGovUp, sGameGovDown, stalls, quiet);
        sGameGovMeanSum = 0.0f;
        sGameGovMeanN = 0;
    }
}
// ---------------------------------------------------------------------------------------------------------------------------------
// q30lock26 (12 Nov 2026): GXV_Q30_LOCK, the Quality (30) mode's lock ladder (port/q30lock.h has the rules; the steps 3a..3d act through the cpulod tables, 3b through the far-draw cut, 4 / 5 / 6 in
// the back end). The fader of port/lockfade.h is also what the Performance governors' level goes through, so that THEIR changes are walked in too (owner 12 Nov 14:47: a step is invisible).
// The turn feed (the wall time from a turn's begin to its work-done, drawn or not), the picture feed (autoq: GPU busy, front-end CPU) and the shown pictures' holds (the display callback) come
// in through the extern "C" functions below; the log lines are drained by whichever feed runs next.
namespace {
port::lockfade::Fader sLockFade;
port::lockfade::Weight sBudRamp[7];            // q30-28: steps 3f / 3g, swept in over 4 s (0..1)
port::lockfade::Weight sFarRamp;           // step 3b: the size under which a far speck is left out, ramped 0..1 over a second
std::atomic<int> sLockArmed{0};
port::q30lock::Ladder sLadder;
port::q30lock::Memory sLockMem;
std::mutex sLockMemLock;
bool sLockMemLoaded = false;
int64_t sLockTurnStartNs = 0;
std::atomic<int> sFarRampLevel{0};
std::atomic<float> sBudW[7];   // the game thread writes, any thread reads
const char* LockFilePath() {
    const char* e = getenv("GXV_Q30_LOCK_FILE");
    return e && *e ? e : "ux0:data/smg/q30lock.txt";
}
void LockMemLoad() {
    if (sLockMemLoaded) return;
    sLockMemLoaded = true;
    FILE* f = fopen(LockFilePath(), "rb");
    if (!f) { port_log_summary("q30lock: no list at %s yet (started anew)", LockFilePath()); return; }
    static char buf[8192];
    const size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    const int got = sLockMem.Parse(buf, n);
    port_log_summary("q30lock: %d stages read from %s", got, LockFilePath());
}
void LockMemSave() {
    if (!sLockMem.dirty) return;
    static char buf[8192];
    const int n = sLockMem.Format(buf, sizeof buf);
    if (CardAsyncOn()) {
        // hitch30: this runs on the thread that ends a picture (LockDrain, from port_q30lock_picture): create + remove + rename of the card file was
        // 0.4-0.6 s in play each time the ladder learned something. Queued now; the card-write thread does it. A full queue leaves the memory dirty:
        // the next picture asks again.
        if (n > 0 && port_card_write_async(LockFilePath(), 0, buf, static_cast<unsigned>(n))) sLockMem.dirty = false;
        return;
    }
    sLockMem.dirty = false;   // (a failed write is not tried again every picture; the next change asks again)
    const std::string tmp = std::string(LockFilePath()) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    const bool ok = fwrite(buf, 1, static_cast<size_t>(n), f) == static_cast<size_t>(n);
    const bool closed = fclose(f) == 0;
    if (ok && closed) { remove(LockFilePath()); rename(tmp.c_str(), LockFilePath()); }
}
void LockDrain() {
    char line[400];
    while (sLadder.PopLog(line, sizeof line)) port_log_summary("%s", line);
    while (sLadder.PopCensus(line, sizeof line)) port_log_summary("%s", line);   // (recover30: the stage's end)
    {   // (recover30: the census every 5 s, one line for each step that was taken)
        static uint64_t nextAt = 0;
        const uint64_t now = port::q30lock::NowUs();
        if (now >= nextAt) {
            nextAt = now + 5000000u;
            char cl[10][160];
            const int n = sLadder.Census(now, cl, 10);
            for (int i = 0; i < n; ++i) port_log_summary("%s", cl[i]);
        }
    }
    std::lock_guard<std::mutex> lk(sLockMemLock);
    LockMemSave();
}
}  // namespace
// ---------------------------------------------------------------------------------------------------------------------------------
// anticip32 (13 Nov 2026): GXV_ANTICIPATE (default 0 = nothing here runs; port/anticipate.h has the rules, tests/anticipate_test.cpp the model). The scaling engine's look-ahead: the game's
// hooks (launch star, sling star, boss music, power-up change: port_anticipate_event; demo starts and Mario's position: port_anticipate_scene from LodCtrl.cpp's once-a-turn hook; camera cuts:
// port_camera_cut_count) and the learned spike map open ONE window, and while it is open the EXISTING engine is asked for a little less far detail through its own actuators and fades:
//   Performance: the level the governors' fader walks is GovMax() + GXV_ANTICIPATE_DEPTH (1; never past level 3, never the last resort), walked in within the event's lead (at least 500 ms);
//   Quality:     the ladder's next 2 x depth least-visible steps (never 4 / 5 / 6) as an overlay on its published mask (q30lock.h O().extra): each step's own fade walks it in and out;
//   rate:        no up-switch inside the window; a down-switch to the even 30 AT the event when the map says this event ended in dropped pictures before (rate_auto.h Anticipate).
// When the window closes the request is withdrawn and the same faders walk the detail back (GXV_GOV_FADE_MS, the steps' own lengths). A governor window with the pre-scale in force is never
// calm, the ladder gives nothing back while its overlay is set: the engine's own give-back rules run after the moment, unchanged. Nothing here reads a time budget or changes a game value.
// Switches (read once): GXV_ANTICIPATE 0 / 1 (events + map) / 2 (only what the map has seen an overload at), GXV_ANTICIPATE_LEAD_MS (1000), GXV_ANTICIPATE_DEPTH (1), GXV_ANTICIPATE_TAIL_MS
// (1500), GXV_ANTICIPATE_MAX_MS (15000), GXV_SPIKEMAP 0 / 1, GXV_SPIKEMAP_CELL (2000 units), GXV_SPIKEMAP_SAVE 0 / 1 (GXV_SPIKEMAP_FILE, default ux0:data/smg/spikes.txt: a file of its own,
// read once, written by the card-write thread or behind a stage load), GXV_ANTICIPATE_TEST=<turns> (test only: a "test event" every that many turns of play).
// Lines: "anticipate: <event> at turn N: pre-scaled <what> (lead L ms)" at the start, the same with "; restored at turn M" at the end, "anticipate: summary: ..." every 10 s.
static int AntRate(uint64_t nowUs, uint64_t untilUs, bool drop);   // (below, with the rate controller)
namespace {
std::mutex sAntLock;                       // the engine: the game's hooks may run on a helper of the game thread
port::anticipate::Engine sAnt;
std::atomic<uint32_t> sAntSpikeBits{0};    // detectors on any thread post here; the game thread's turn takes them
std::atomic<uint64_t> sAntSpikeAt[port::anticipate::kSrcN];
uint32_t sAntTurn = 0, sAntKey = 0, sAntCuts = 0, sAntPlayTurns = 0;
float sAntPos[3] = {0, 0, 0};
uint32_t sAntPosTurn = 0;                  // the turn the position was handed over in (0: never)
int sAntDemo = 0;
char sAntWhat[200] = "nothing";
uint64_t sAntNextLog = 0, sAntStageAt = 0, sAntLogFrom = 0;
uint32_t sAntLadderMask = 0;
int sAntDepth = 1, sAntTestEvery = 0;
bool sAntSave = false, sAntLoaded = false;
// The module's clock is GAME TIME (turns x 16.683 ms), not the wall clock: the game's speed is held at real time on the console, and a run that is slower (the emulator) or drops turns of
// wall time still opens and closes every window after the same number of turns: the lines are the same on every run.
uint64_t AntClock() { return static_cast<uint64_t>(sAntTurn) * 16683u; }
int AntMode() {
    static const int mode = [] {
        const long m = vgx::EnvInt("GXV_ANTICIPATE", 0);
        if (m <= 0) return 0;
        port::anticipate::Params p;
        p.mode = m > 2 ? 2 : static_cast<int>(m);
        const auto clampL = [](const char* n, long d, long lo, long hi) { const long v = vgx::EnvInt(n, d); return v < lo ? lo : v > hi ? hi : v; };
        p.leadMs = static_cast<uint32_t>(clampL("GXV_ANTICIPATE_LEAD_MS", 1000, 200, 5000));
        p.tailMs = static_cast<uint32_t>(clampL("GXV_ANTICIPATE_TAIL_MS", 1500, 0, 10000));
        p.maxMs = static_cast<uint32_t>(clampL("GXV_ANTICIPATE_MAX_MS", 15000, 2000, 60000));
        p.map = vgx::EnvInt("GXV_SPIKEMAP", 0) != 0;
        p.cell = static_cast<float>(clampL("GXV_SPIKEMAP_CELL", 2000, 500, 20000));
        sAntDepth = static_cast<int>(clampL("GXV_ANTICIPATE_DEPTH", 1, 1, 3));
        sAntTestEvery = static_cast<int>(clampL("GXV_ANTICIPATE_TEST", 0, 0, 100000));
        sAntSave = p.map && vgx::EnvInt("GXV_SPIKEMAP_SAVE", 0) != 0;
        sAnt.Configure(p);
        port_log_summary("anticipate: GXV_ANTICIPATE=%d: a heavy moment the game announces (launch / sling star, boss music, power-up%s) pre-scales the far detail %d level%s (Performance) / %d steps "
                         "(Quality) through the engine's own faders and gives it back after; lead %u ms, tail %u ms, at most %u ms; spike map %s (cell %.0f units)%s%s",
                         p.mode, p.mode >= 2 ? ": only where the spike map has seen an overload" : "", sAntDepth, sAntDepth == 1 ? "" : "s", 2 * sAntDepth, static_cast<unsigned>(p.leadMs),
                         static_cast<unsigned>(p.tailMs), static_cast<unsigned>(p.maxMs), p.map ? "ON" : "off", static_cast<double>(p.cell), sAntSave ? ", kept on the card" : "",
                         sAntTestEvery ? ", TEST EVENTS ON" : "");
        return p.mode;
    }();
    return mode;
}
const char* AntFilePath() {
    const char* e = getenv("GXV_SPIKEMAP_FILE");
    return e && *e ? e : "ux0:data/smg/spikes.txt";
}
// (sAntLock held) the map from the card, once; a file of this feature only (never a save or a settings file)
void AntLoad() {
    if (sAntLoaded || !sAntSave) return;
    sAntLoaded = true;
    FILE* f = fopen(AntFilePath(), "rb");
    if (!f) { port_log_summary("anticipate: spike map: no file at %s yet (started anew)", AntFilePath()); return; }
    static char buf[12288];
    const size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    const int got = sAnt.Map().Parse(buf, n);
    port_log_summary("anticipate: spike map: %d cells read from %s", got, AntFilePath());
}
// (sAntLock held) cover: a stage load hides a write on this thread; in play only the card-write thread writes (a full queue leaves the map dirty: asked again at the next line)
void AntSaveMap(bool cover) {
    if (!sAntSave || !sAnt.Map().dirty) return;
    static char buf[12288];
    const int n = sAnt.Map().Format(buf, sizeof buf);
    if (n <= 0) return;
    if (CardAsyncOn()) {
        if (port_card_write_async(AntFilePath(), 0, buf, static_cast<unsigned>(n))) sAnt.Map().dirty = false;
        return;
    }
    if (!cover) return;
    sAnt.Map().dirty = false;
    const std::string tmp = std::string(AntFilePath()) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    const bool ok = fwrite(buf, 1, static_cast<size_t>(n), f) == static_cast<size_t>(n);
    const bool closed = fclose(f) == 0;
    if (ok && closed) { remove(AntFilePath()); rename(tmp.c_str(), AntFilePath()); }
}
// (sAntLock held) the governors' gain / cost measurements in flight are void: the cuts are about to move, or just moved, from outside
void AntVoidGovGains() {
    { std::lock_guard<std::mutex> lock(sFeGovLock); sFeGov.VoidPending(); }
    { std::lock_guard<std::mutex> lock(sGameGovLock); sGameGov.VoidPending(); }
}
// (sAntLock held) the request is withdrawn everywhere: the faders walk the detail back
void AntWithdraw() {
    if (sAntLevels.exchange(0, std::memory_order_relaxed) != 0) AntVoidGovGains();
    sAntWalkMs.store(0, std::memory_order_relaxed);
    port::q30lock::O().extra.store(0, std::memory_order_relaxed);
}
// (sAntLock held) a window opened: the engine is asked, and the line says what for
void AntOpen(uint64_t now) {
    const port::anticipate::Window& w = sAnt.Win();
    const int depth = sAntDepth + (w.deep ? 1 : 0);
    int o = 0;
    if (sLockArmed.load(std::memory_order_relaxed)) {   // Quality with the lock ladder
        const port::q30lock::StepMask set = sLadder.NextSteps(2 * depth);
        port::q30lock::O().extra.store(set, std::memory_order_relaxed);
        if (set) {
            o += snprintf(sAntWhat + o, sizeof sAntWhat - o, "Quality steps");
            for (int st = 0; st < port::q30lock::kStepN && o < static_cast<int>(sizeof sAntWhat) - 8; ++st)
                if (set >> st & 1u) o += snprintf(sAntWhat + o, sizeof sAntWhat - o, " %s", port::q30lock::StepLabel(st));
            o += snprintf(sAntWhat + o, sizeof sAntWhat - o, " (each step's own fade)");
        } else o += snprintf(sAntWhat + o, sizeof sAntWhat - o, "no Quality step (none left that is allowed)");
    } else {
        const int g = GovMax(), cap = port::stageknob::Quality() ? 1 : 3;
        const int to = g >= cap ? g : (g + depth > cap ? cap : g + depth);
        const long govMs = static_cast<long>(FeGovKnob("GXV_GOV_FADE_MS", 3000.0f, 500.0f, 10000.0f));
        long walk = w.leadMs ? static_cast<long>(w.leadMs) : static_cast<long>(sAnt.Cfg().leadMs);
        walk = walk < 500 ? 500 : walk > govMs ? govMs : walk;
        sAntWalkMs.store(static_cast<int>(walk), std::memory_order_relaxed);
        sAntLevels.store(depth, std::memory_order_relaxed);
        AntVoidGovGains();
        if (to > g) o += snprintf(sAntWhat + o, sizeof sAntWhat - o, "cut level %d -> %d (walked in over %ld ms)", g, to, walk);
        else o += snprintf(sAntWhat + o, sizeof sAntWhat - o, "nothing (cut level %d is the deepest the pre-scale may ask for)", g);
    }
    if (port_ff_flyover() != 0) o += snprintf(sAntWhat + o, sizeof sAntWhat - o, " [a flight is on: the engine moves no level until it ends]");
    const uint64_t real = port::q30lock::NowUs();
    const int r = AntRate(real, real + (w.endUs > now ? w.endUs - now : 0), w.rate);   // (the rate controller's clock is the wall clock)
    if (r == 2) o += snprintf(sAntWhat + o, sizeof sAntWhat - o, "; rate 60 -> even 30 at the event (the map: it ended in dropped pictures before)");
    else if (r == 1) o += snprintf(sAntWhat + o, sizeof sAntWhat - o, "; rate: no up-switch inside");
    char line[420];
    port::anticipate::Engine::FormatOpen(line, sizeof line, w, sAntWhat);
    port_log_summary("%s", line);
}
}  // namespace
static int AntLevel(int g) {
    const int n = sAntLevels.load(std::memory_order_relaxed);
    if (n <= 0) return g;
    const int cap = port::stageknob::Quality() ? 1 : 3;   // (never the last resort; Quality without its ladder has the q level only)
    return g >= cap ? g : (g + n > cap ? cap : g + n);
}
static void AntSpikePost(int src) {
    if (src < 0 || src >= port::anticipate::kSrcN || !AntMode()) return;
    const uint64_t now = port::q30lock::NowUs(), at = sAntSpikeAt[src].load(std::memory_order_relaxed);
    if (at != 0 && now - at < 1000000u) return;   // (a detector that fires every turn is one report a second)
    sAntSpikeAt[src].store(now, std::memory_order_relaxed);
    sAntSpikeBits.fetch_or(1u << src, std::memory_order_relaxed);
}
// The game's hooks (compat.h). kind: port::anticipate::Kind (1 launch star, 2 sling star, 4 boss battle, 5 power-up); the moment begins in about lead_ms and lasts about last_ms.
extern "C" void port_anticipate_event(int kind, int lead_ms, int last_ms) {
    if (!AntMode()) return;
    std::lock_guard<std::mutex> lk(sAntLock);
    sAnt.Event(AntClock(), sAntTurn, kind, static_cast<uint32_t>(lead_ms < 0 ? 0 : lead_ms), static_cast<uint32_t>(last_ms < 0 ? 0 : last_ms));
}
// LodCtrl.cpp, once a game turn of a stage scene (port_popin_turn): what the game says is going on (0 none, else a demo / the opening camera / the star get) and Mario's position (null: none).
extern "C" void port_anticipate_scene(int demo_kind, const float* player_pos) {
    if (!AntMode()) return;
    std::lock_guard<std::mutex> lk(sAntLock);
    if (player_pos) { sAntPos[0] = player_pos[0]; sAntPos[1] = player_pos[1]; sAntPos[2] = player_pos[2]; sAntPosTurn = sAntTurn ? sAntTurn : 1; }
    if (demo_kind != 0 && sAntDemo == 0) sAnt.Event(AntClock(), sAntTurn, port::anticipate::kDemo, 0, 3000);
    if (demo_kind == 0) ++sAntPlayTurns;
    sAntDemo = demo_kind;
}
// The game thread, at each turn's start, before the fader's tick.
static void AntTurn() {
    if (!AntMode()) return;
    const uint64_t real = port::q30lock::NowUs();
    std::lock_guard<std::mutex> lk(sAntLock);
    ++sAntTurn;
    const uint64_t now = AntClock();
    AntLoad();
    const uint32_t key = port_scene_key();
    if (key != sAntKey) {   // a stage (a scene) begins: the load is the cover, nothing is carried over
        sAntKey = key;
        char nm[48], stage[48];
        int sc = -1;
        if (port_scene_loading_stage(nm, sizeof nm, &sc) && nm[0]) snprintf(stage, sizeof stage, "%.34s%s%d", nm, sc >= 0 ? ":" : "", sc >= 0 ? sc : 0);
        else snprintf(stage, sizeof stage, "%08x", static_cast<unsigned>(key));
        AntWithdraw();
        sAnt.StageBegin(stage);
        sAntDemo = 0;
        sAntPosTurn = 0;
        sAntPlayTurns = 0;
        sAntStageAt = now;
        sAntLadderMask = port::q30lock::O().mask.load(std::memory_order_relaxed);
        sAntSpikeBits.store(0, std::memory_order_relaxed);
        AntSaveMap(true);
    }
    if (port_camera_cut_count) {
        const uint32_t cuts = port_camera_cut_count();
        if (cuts != sAntCuts) { sAntCuts = cuts; sAnt.Event(now, sAntTurn, port::anticipate::kCamCut, 0, 0); }
    }
    {   // the Quality ladder took a step of its own (not at the stage's start, where it only takes what the card remembers)
        const uint32_t m = port::q30lock::O().mask.load(std::memory_order_relaxed);
        if ((m & ~sAntLadderMask) != 0 && now - sAntStageAt > 8000000u) AntSpikePost(port::anticipate::kSrcLadder);
        sAntLadderMask = m;
    }
    const bool havePos = sAntPosTurn != 0 && sAntTurn - sAntPosTurn <= 2;
    sAnt.Turn(now, sAntTurn, havePos ? sAntPos : nullptr);   // (first: an overload is booked against the window and the place as they stand now)
    for (uint32_t bits = sAntSpikeBits.exchange(0, std::memory_order_relaxed), src = 0; bits; bits >>= 1, ++src) {
        if (!(bits & 1u)) continue;
        const int idx = sAnt.Spike(now, static_cast<int>(src));
        if (idx >= 0) {
            const port::anticipate::Cell& c = sAnt.Map().At(idx);
            port_log_summary("anticipate: spike map: %s at turn %u in %s: cell %d,%d,%d %s%s remembered (%d overload%s there%s%s)", port::anticipate::SrcName(static_cast<int>(src)), sAntTurn, c.stage,
                             static_cast<int>(c.x), static_cast<int>(c.y), static_cast<int>(c.z), c.kind ? "of the event " : "(a place)", c.kind ? port::anticipate::KindName(c.kind) : "",
                             static_cast<int>(c.hits), c.hits == 1 ? "" : "s", (c.flags & port::anticipate::kFlagRate) ? "; pictures were dropped" : "",
                             (c.flags & port::anticipate::kFlagDeep) ? "; one level more the next time" : "");
        }
    }
    if (sAntTestEvery > 0 && sAntDemo == 0 && havePos && sAntPlayTurns > 0 && sAntPlayTurns % static_cast<uint32_t>(sAntTestEvery) == 0) {
        ++sAntPlayTurns;   // (once per multiple: the count stands still when the game's hook does not run)
        sAnt.Event(now, sAntTurn, port::anticipate::kTest, 767, 3000);
    }
    port::anticipate::Window w;
    uint32_t at = 0;
    if (sAnt.TakeClosed(&w, &at)) {   // (a window that closed and one that opened in the same turn: the old line first)
        if (!sAnt.Win().open) AntWithdraw();
        char line[460];
        port::anticipate::Engine::FormatClose(line, sizeof line, w, sAntWhat, at);
        port_log_summary("%s", line);
    }
    if (sAnt.TakeOpened()) AntOpen(now);
    if (real >= sAntNextLog) {   // (every 10 s of the wall clock)
        if (sAntNextLog != 0) {
            char line[640];
            sAnt.FormatSummary(line, sizeof line, static_cast<double>(real - sAntLogFrom) / 1e6);
            port_log_summary("%s; now: %s, cut level %d, Quality overlay %04x", line, sAnt.Win().open ? "a window is OPEN" : "no window", LockLevelNow() & 0xFF,
                             static_cast<unsigned>(port::q30lock::O().extra.load(std::memory_order_relaxed)));
            AntSaveMap(false);
        } else sAntLogFrom = real;
        sAntNextLog = real + 10000000u;
    }
}
static int LockDesired() { return sLockArmed.load(std::memory_order_relaxed) ? port::q30lock::CpuLodLevel() : AntLevel(GovMax()); }   // (anticip32: Quality's overlay is in CpuLodLevel's mask)
static int LockLevelNow() { return sLockFade.Level(); }
static int LockLevelFor(uint32_t seed) { return sLockFade.LevelFor(seed); }
static int LockFrom() { return sLockFade.From(); }
static int LockTo() { return sLockFade.To(); }
static bool LockBusy() { return sLockFade.Busy(); }
static float LockFrac() { return sLockFade.Frac(); }
// The game thread, at each turn's start: a new level is begun as a walk (its length from the steps that changed, else 2 s for a governor), and one tick is taken. Never two walks at once: the
// ladder holds its next change until the walk (+ 0.5 s) is over, and a governor's windows are 5 s apart.
static void LockPump() {
    const int want = LockDesired();
    const int to = sLockFade.To();
    const bool fly = port_ff_flyover() != 0;   // (far30: nothing moves while a flight is on: no walk begins or ticks, no ramp moves, the ladder holds its decisions)
    sLadder.SetHold(fly);
    if (fly) return;
    if (want != to) {
        static const uint32_t govMs = static_cast<uint32_t>(FeGovKnob("GXV_GOV_FADE_MS", 3000.0f, 500.0f, 10000.0f));   // (recover30: a governor's level change is walked in this long, was 2000)
        uint32_t ms = govMs;
        if (port::cpulod::Coded(want) && port::cpulod::Coded(to)) {
            const int diff = port::cpulod::CodedSteps(want) ^ port::cpulod::CodedSteps(to);
            ms = 0;
            for (int s = 0; s < 5; ++s) if (diff >> s & 1) ms = ms > port::q30lock::kTransMs[s] ? ms : port::q30lock::kTransMs[s];
            if (!ms) ms = 100;
        } else if (const int a = sAntWalkMs.load(std::memory_order_relaxed); a > 0 && want > to && sAntLevels.load(std::memory_order_relaxed) > 0 && static_cast<uint32_t>(a) < ms) {
            ms = static_cast<uint32_t>(a);   // (anticip32: a pre-scale is walked in within its lead; the way back is the governors' own GXV_GOV_FADE_MS)
        }
        if (want > to) ++sFfIn; else ++sFfOut;   // (far30: the far: line counts the moves)
        sLockFade.Begin(want, static_cast<int>(ms * 60u / 1000u));
    }
    sLockFade.Tick();
    // step 3b's ramp: 1 second from nothing to the full size, and back
    sFarRamp.Target(sLockArmed.load(std::memory_order_relaxed) && port::q30lock::StepOn(port::q30lock::k3b) ? 1.0f : 0.0f);
    sFarRamp.Tick(1.0f / 60.0f);
    sFarRampLevel.store(sFarRamp.Value() > 0.0f ? 1 : 0, std::memory_order_relaxed);
    {   // (q30-28) 3f / 3g: 4 s from nothing to the full cut (the turns come 60 a second), and back
        const bool armed = sLockArmed.load(std::memory_order_relaxed) != 0;
        const int steps[7] = {port::q30lock::k3f, port::q30lock::k3g, port::q30lock::k3h, port::q30lock::k3i, port::q30lock::k3j, port::q30lock::k3k, port::q30lock::k3l};   // (far3: 2..6 = the far-field steps)
        for (int i = 0; i < 7; ++i) {
            sBudRamp[i].Target(armed && port::q30lock::StepOn(steps[i]) ? 1.0f : 0.0f);
            sBudRamp[i].Tick(1.0f / 240.0f);
            sBudW[i].store(sBudRamp[i].Value(), std::memory_order_relaxed);
        }
    }
}
static void LockTurnBegin() { sLockTurnStartNs = port_host_time_ns(); }
static void LockTurnDone() {
    if (!sLockArmed.load(std::memory_order_relaxed) || !sLockTurnStartNs) return;
    const int64_t us = (port_host_time_ns() - sLockTurnStartNs) / 1000;
    if (port::pace::GlobalSpeedGuard().Enabled() && port::pace::GlobalSpeedGuard().ExtraNow()) return;   // (speed31: an extra turn the speed guard took is no part of the game's pair: the pair is what a picture costs)
    sLadder.NoteTurn(port::q30lock::NowUs(), us < 0 ? 0u : static_cast<uint32_t>(us), !sWorkUndrawn);
}
extern "C" int port_q30lock_far_level(void) { return sFarRampLevel.load(std::memory_order_relaxed); }
extern "C" float port_q30lock_budget_w(int which) { return which >= 0 && which < 7 ? sBudW[which].load(std::memory_order_relaxed) : 0.0f; }
// the far-draw cut's size (lines) times this: the ramp of step 3b; 1 outside the lock
extern "C" float port_q30lock_far_ramp(void) { return sLockArmed.load(std::memory_order_relaxed) ? sFarRamp.Value() : 1.0f; }
// q30-28: the back end's picture GPU time without the display slot's wait (busy - the display scene's estimated wait, vgx_dispwait.h). The ladder compared "busy", which holds that wait,
// with its 31 ms limit: Dreadnought read 29.8 for 25.75, a 23-draw menu 32. One value, the newest; used by port_q30lock_picture while it is fresh (150 ms), else the caller's figure.
namespace {
std::atomic<uint32_t> sQ30GpuUs{0};
std::atomic<uint64_t> sQ30GpuAt{0};
std::atomic<uint64_t> sQ30GpuSum{0}, sQ30GpuWaitSum{0}, sQ30GpuN{0};
}  // namespace
extern "C" void port_q30lock_gpu_busy(uint32_t busyUs, uint32_t waitUs) {
    const uint32_t w = waitUs < busyUs ? waitUs : busyUs;
    sQ30GpuUs.store(busyUs - w, std::memory_order_relaxed);
    sQ30GpuAt.store(port::q30lock::NowUs(), std::memory_order_relaxed);
    sQ30GpuSum.fetch_add(busyUs, std::memory_order_relaxed);
    sQ30GpuWaitSum.fetch_add(w, std::memory_order_relaxed);
    sQ30GpuN.fetch_add(1, std::memory_order_relaxed);
}
extern "C" int port_vgx_efb_samples(void) __attribute__((weak));
extern "C" void port_q30lock_picture(uint32_t gpuUs, int gpuKnown, uint32_t frontUs, int frontKnown, int play) {
    if (port::ab::Frozen()) return;   // (ab27: the Quality ladder stands still while GXV_AB measures)
    if (!sLockArmed.load(std::memory_order_relaxed)) return;
    sLadder.SetMsaaActive(port_vgx_efb_samples && port_vgx_efb_samples() > 1);
    {
        const uint64_t now = port::q30lock::NowUs(), at = sQ30GpuAt.load(std::memory_order_relaxed);
        if (at != 0 && now - at < 150000u) { gpuUs = sQ30GpuUs.load(std::memory_order_relaxed); gpuKnown = 1; }
    }
    sLadder.NotePicture(port::q30lock::NowUs(), gpuUs, gpuKnown != 0, frontUs, frontKnown != 0, play != 0);
    LockDrain();
}
// recover30: the game's speed. SpeedGuardTurnBegin (above) calls this at every turn start with the wall time since the previous one and whether the turn counts (not a load, not a covered screen).
extern "C" void port_q30lock_turn_gap(uint32_t gapUs, int counting) {
    if (!sLockArmed.load(std::memory_order_relaxed)) return;
    sLadder.NoteSpeed(port::q30lock::NowUs(), gapUs, counting != 0);
}
extern "C" void port_q30lock_shown(unsigned hold) {
    if (!sLockArmed.load(std::memory_order_relaxed)) return;
    sLadder.NoteShown(port::q30lock::NowUs(), hold, true);
}
// the back end: the other size's render target exists and a natural cover is here (step 6 may be taken); it says 0 when it is not
extern "C" void port_q30lock_step6_ready(int ready) { sLadder.SetStep6Ready(ready != 0); }
// q30fin26: the back end opens / closes a step it can act on (5: the compositor target and programs are made and a layer is routed), and says each picture whether a natural cover is open
extern "C" void port_q30lock_allow(int step, int on) {
    if (!sLockArmed.load(std::memory_order_relaxed) || step < 0 || step >= port::q30lock::kStepN) return;
    sLadder.SetAllowed(step, on != 0);
}
extern "C" int port_q30lock_step_on(int step) { return step >= 0 && step < port::q30lock::kStepN && port::q30lock::StepOn(step) ? 1 : 0; }
extern "C" void port_q30lock_cover(int open) {
    if (!sLockArmed.load(std::memory_order_relaxed)) return;
    sLadder.NoteCover(port::q30lock::NowUs(), open != 0);
    LockDrain();
}
// A stage's load (the preset's switches are in force): the previous stage's census is written, the new stage starts from what the card remembers for it. `allowed`: the steps the back end can act on.
extern "C" void port_q30lock_stage(const char* name, int scenario, unsigned allowed) {
    const uint64_t now = port::q30lock::NowUs();
    if (!LockWanted()) {
        sQFarOn.store(0, std::memory_order_relaxed);
        if (sLockArmed.exchange(0)) { std::lock_guard<std::mutex> lk(sLockMemLock); sLadder.Shutdown(now, &sLockMem); port::q30lock::O().enabled.store(0); port::q30lock::O().mask.store(0); }
        LockDrain();
        sLockFade.Set(GovMax());
        for (int i = 0; i < 7; ++i) { sBudRamp[i].Set(0.0f); sBudW[i].store(0.0f, std::memory_order_relaxed); }
        return;
    }
    {
        std::lock_guard<std::mutex> lk(sLockMemLock);
        LockMemLoad();
        port::q30lock::Config c;
        {   // (pace32: GXV_HEADROOM_PCT lowers the ladder's limits; explicit keys still win)
            const float wasP = c.p95Ms, wasG = c.gpuMs;
            c.p95Ms = FeGovKnob("GXV_Q30_LOCK_P95_MS", HeadroomCap(c.p95Ms, true), 20.0f, 33.3f);
            c.gpuMs = FeGovKnob("GXV_Q30_LOCK_GPU_MS", HeadroomCap(c.gpuMs, true), 20.0f, 33.3f);
            if (HeadroomPct()) c.probeSlackMs = 0.0f;   // (quality comes back only when it fits under the limit; GXV_Q30_LOCK_PROBE_MS below still wins)
            HeadroomLog("the Quality ladder's game-pair / front-end limit", wasP, c.p95Ms, true);
            HeadroomLog("the Quality ladder's GPU limit", wasG, c.gpuMs, true);
        }
        c.gapSec = FeGovKnob("GXV_Q30_LOCK_GAP_S", c.gapSec, 1.0f, 600.0f);
        c.fastOverMs = FeGovKnob("GXV_Q30_LOCK_FAST_OVER_MS", c.fastOverMs, 0.0f, 20.0f);   // (q30-27: 0 = the old 1.5 s + 10 s timing)
        c.fastGapSec = FeGovKnob("GXV_Q30_LOCK_FAST_GAP_S", c.fastGapSec, 1.0f, 600.0f);
        c.fastConfirmSec = FeGovKnob("GXV_Q30_LOCK_FAST_CONFIRM_S", c.fastConfirmSec, 0.25f, 60.0f);
        c.holdSec = FeGovKnob("GXV_Q30_LOCK_HOLD_S", c.holdSec, 1.0f, 600.0f);
        c.calmSec = FeGovKnob("GXV_Q30_LOCK_CALM_S", c.calmSec, 1.0f, 600.0f);
        c.memory = FeGovKnob("GXV_Q30_LOCK_MEMORY", 1, 0, 1) != 0;
        c.guard = FeGovKnob("GXV_Q30_GUARD", 1, 0, 1) != 0 && SpeedGuardSignalOn();   // (speed31: the speed guard's dropped pictures are a threat; 0 = today's rules)
        c.guardConfirmSec = FeGovKnob("GXV_Q30_GUARD_CONFIRM_S", c.guardConfirmSec, 0.05f, 10.0f);
        c.guardMaxSteps = static_cast<int>(FeGovKnob("GXV_Q30_GUARD_STEPS", 4, 1, 8));
        c.allowed = static_cast<port::q30lock::StepMask>(allowed & 0x7FFFu);
        const unsigned maxStep = static_cast<unsigned>(FeGovKnob("GXV_Q30_LOCK_STEPS", 32767, 0, 32767));   // a test may cap the ladder (a bit mask of the steps)
        c.allowed = static_cast<port::q30lock::StepMask>(c.allowed & maxStep);
        if (FeGovKnob("GXV_Q30_FAR", 1, 0, 1) == 0) c.allowed = static_cast<port::q30lock::StepMask>(c.allowed & ~port::q30lock::kFarMask);
        sQFarOn.store((c.allowed & port::q30lock::kFarMask) != 0 ? 1 : 0, std::memory_order_relaxed);   // (far3: 0 = today's ladder, exactly)
        if (const char* sp = getenv("GXV_Q30_OPEN"); sp && *sp) { if (sp[0] == '-') c.openSpec[0] = 0; else snprintf(c.openSpec, sizeof c.openSpec, "%s", sp); }
        if (const char* sp = getenv("GXV_Q30_START"); sp && *sp) { if (sp[0] == '-') c.startSpec[0] = 0; else snprintf(c.startSpec, sizeof c.startSpec, "%s", sp); }
        c.proofSec = FeGovKnob("GXV_Q30_LOCK_PROOF_S", c.proofSec, 5.0f, 600.0f);   // (q30-28: no missed picture for this long = proof of headroom)
        c.probeSlackMs = FeGovKnob("GXV_Q30_LOCK_PROBE_MS", c.probeSlackMs, 0.0f, 5.0f);
        // recover30 (every key read once per stage load): see port/q30lock.h
        c.recover = FeGovKnob("GXV_Q30_RECOVER", c.recover ? 1 : 0, 0, 1) != 0;
        c.calmMinSec = FeGovKnob("GXV_Q30_CALM_MIN_S", c.calmMinSec, 0.5f, 600.0f);
        c.holdMinSec = FeGovKnob("GXV_Q30_HOLD_MIN_S", c.holdMinSec, 1.0f, 600.0f);
        c.gapMinSec = FeGovKnob("GXV_Q30_GAP_MIN_S", c.gapMinSec, 1.0f, 600.0f);
        c.proofMinSec = FeGovKnob("GXV_Q30_PROOF_MIN_S", c.proofMinSec, 2.0f, 600.0f);
        c.proofOverMs = FeGovKnob("GXV_Q30_PROOF_OVER_MS", c.proofOverMs, 0.5f, 20.0f);
        c.fullMarginMs = FeGovKnob("GXV_Q30_MARGIN_FULL_MS", c.fullMarginMs, 0.5f, 20.0f);
        c.multi = FeGovKnob("GXV_Q30_MULTI", c.multi ? 1 : 0, 0, 1) != 0;
        c.retakeSec = FeGovKnob("GXV_Q30_RETAKE_S", c.retakeSec, 5.0f, 600.0f);
        c.halveAfterSec = FeGovKnob("GXV_Q30_HALVE_S", c.halveAfterSec, 10.0f, 3600.0f);
        c.slipPct = FeGovKnob("GXV_Q30_SLIP_PCT", c.slipPct, 0.0f, 100.0f);
        c.slipSec = FeGovKnob("GXV_Q30_SLIP_S", c.slipSec, 0.25f, 60.0f);
        c.slipFastPct = FeGovKnob("GXV_Q30_SLIP_FAST_PCT", c.slipFastPct, 0.0f, 100.0f);
        sLadder.Configure(c);
        char key[64];
        if (scenario >= 0) snprintf(key, sizeof key, "%s:%d", name && *name ? name : "?", scenario);
        else snprintf(key, sizeof key, "%s", name && *name ? name : "?");
        sLadder.StageBegin(now, key, &sLockMem);
        port::q30lock::O().enabled.store(1);
        sLockArmed.store(1);
    }
    LockDrain();
    sLockFade.Set(port::q30lock::CpuLodLevel());   // (a stage's load is a cover: the remembered start set needs no walk)
    sFarRamp.Set(port::q30lock::StepOn(port::q30lock::k3b) ? 1.0f : 0.0f);
    {   // (q30-28) a remembered 3f / 3g is in force at the load (a cover): no sweep
        const int steps[7] = {port::q30lock::k3f, port::q30lock::k3g, port::q30lock::k3h, port::q30lock::k3i, port::q30lock::k3j, port::q30lock::k3k, port::q30lock::k3l};
        for (int i = 0; i < 7; ++i) { const float v = port::q30lock::StepOn(steps[i]) ? 1.0f : 0.0f; sBudRamp[i].Set(v); sBudW[i].store(v, std::memory_order_relaxed); }
    }
}
// the run ends: the census and the card
extern "C" void port_q30lock_shutdown(void) {
    if (!sLockArmed.exchange(0)) return;
    { std::lock_guard<std::mutex> lk(sLockMemLock); sLadder.Shutdown(port::q30lock::NowUs(), &sLockMem); }
    LockDrain();
}
extern "C" int port_q30lock_far_level(void);
// (the far-draw cut takes the larger of the two governors' levels; both 0 = off)
extern "C" int port_cpugov_level(void) {
    if (port::stageknob::Quality()) return port_q30lock_far_level();   // (q30-26: the far-draw cut is a sacrifice, Performance's: the Quality governors never ask for it; q30lock26: the ladder's step 3b does, at level 1, ramped in)
    const int a = sCpuGovLevel.load(std::memory_order_relaxed);
    const int f = port_fegov_level();  // (cpu2-26: the larger of the front end's and the game thread's governor)
    if (f == 0) return a;
    const int b = port::fegov::FarDrawLevel(f);
    return a > b ? a : b;
}
// ab27 (GXV_AB): the A/B driver freezes the three governors for the length of a plan, at `level` (GXV_AB_GOV_LEVEL, default 0: a feature's off arm then has no governor cut under it). The
// windows above return early while port::ab::Frozen(). Returns the levels they had (fe, game, cpu packed 8 bits each) for the log line.
extern "C" unsigned port_gov_freeze(int on, int level) {
    const unsigned was = static_cast<unsigned>(sFeGovLevel.load(std::memory_order_relaxed) & 255) | (static_cast<unsigned>(sGameGovLevel.load(std::memory_order_relaxed) & 255) << 8) |
                         (static_cast<unsigned>(sCpuGovLevel.load(std::memory_order_relaxed) & 255) << 16);
    port::ab::SetFrozen(on != 0);
    if (on) {
        sFeGovLevel.store(level, std::memory_order_relaxed);
        sGameGovLevel.store(level, std::memory_order_relaxed);
        sCpuGovLevel.store(level, std::memory_order_relaxed);
    }
    return was;
}
// popin31 (B0, the census line): the governors' levels, the front end's | the game thread's << 8 | the far-draw governor's << 16 (the level the far-speck cut reads)
extern "C" int port_cpugov_level(void);
extern "C" unsigned port_gov_levels(void) {
    const unsigned fe = static_cast<unsigned>(sFeGovLevel.load(std::memory_order_relaxed)) & 255u;
    const unsigned game = static_cast<unsigned>(sGameGovLevel.load(std::memory_order_relaxed)) & 255u;
    const unsigned cpu = static_cast<unsigned>(port_cpugov_level()) & 255u;
    return fe | (game << 8) | (cpu << 16);
}
// A stage's load (the preset's switches are in force): every run and the level start again.
extern "C" void port_cpugov_reset(void) {
    port::core3::StageLoad();  // (core3-38: the stage load that the "core3: stage load" line is about)
    std::lock_guard<std::mutex> lock(sCpuGovLock);
    const int was = sCpuGovLevel.load(std::memory_order_relaxed);
    sCpuGov.Configure(CpuGovConfig());
    sCpuGovLevel.store(0, std::memory_order_relaxed);
    if (was != 0) port_log_summary("cpugov: reset at a stage load (was level %d)", was);
}
// One 5-second floor window (the back end's FloorWindow fields). Logs a change, and a summary line every GXV_CPUGOV_LOG windows (6).
extern "C" void port_cpugov_window(uint32_t pictures, uint32_t late, uint32_t gpu, uint32_t game, uint32_t renderer, uint32_t front, uint32_t other,
                                   uint32_t unknown, uint32_t stalls, uint32_t quiet) {
    std::lock_guard<std::mutex> lock(sCpuGovLock);
    if (port::ab::Frozen()) return;  // (ab27)
    const port::cpugov::Config c = CpuGovConfig();
    sCpuGov.Update(c);
    if (c.maxLevel <= 0) {
        if (sCpuGovLevel.load(std::memory_order_relaxed) != 0) {
            sCpuGovLevel.store(0, std::memory_order_relaxed);  // (switched off by a preset or a mode change: the cut is let go at once)
            port_log_summary("cpugov: off (GXV_CPUGOV 0 or not Performance mode): level 0");
        }
        return;
    }
    port::cpugov::Window w;
    w.pictures = pictures; w.late = late; w.gpu = gpu; w.game = game; w.renderer = renderer; w.front = front; w.other = other; w.unknown = unknown;
    w.stalls = stalls; w.quiet = quiet;
    const int before = sCpuGov.level();
    const bool changed = sCpuGov.Feed(w);
    const int after = sCpuGov.level();
    sCpuGovLevel.store(after, std::memory_order_relaxed);
    ++sCpuGovWindows;
    static const char* const kVerdict[] = {"ignored", "hot", "calm", "middle"};
    const long every = CpuGovKnob("GXV_CPUGOV_LOG", 6, 0, 1000);
    if (changed || (every > 0 && sCpuGovWindows % static_cast<unsigned>(every) == 0)) {
        const port::cpugov::Level l = port::cpugov::LevelOf(after);
        port_log_summary("cpugov: %s level %d -> %d of %d (far draw: lines %.0f, kinds %d, hold %d, flush %d); window %s: %u pictures, %u late (GPU %u, game %u, renderer %u, front end %u, "
                         "none %u, unknown %u), %u stalls, %u quiet; calm run %d of %d, %u changes",
                         changed ? "CHANGE" : "steady", before, after, c.maxLevel, static_cast<double>(l.lines), static_cast<int>(l.kinds), static_cast<int>(l.hold),
                         static_cast<int>(l.flush), kVerdict[static_cast<int>(sCpuGov.last())], pictures, late, gpu, game, renderer, front, other, unknown, stalls, quiet,
                         sCpuGov.calmRun(), sCpuGov.downNeed(), sCpuGov.changes());
    }
}
static int GpModeKnob(const char* name) {
    const char* e = getenv(name);
    if (e == nullptr || *e == 0) return 0;
    if (*e == 'p' || *e == 'P') return port_perf_mode_60();  // ("perf")
    const int v = atoi(e);
    return v < 0 ? 0 : v > 2 ? 2 : v;
}
static float GpEnvFloat(const char* name, float fallback, float lo, float hi) {
    const char* e = getenv(name);
    if (e == nullptr || *e == 0) return fallback;
    const float v = static_cast<float>(atof(e));
    return v < lo ? lo : v > hi ? hi : v;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// rate30 (13 Nov 2026): GXV_RATE_AUTO (default 0 = today's behaviour exactly), the two-way rate controller of Performance mode (port/rate_auto.h has the rules and the model, tests/rate_auto_test.cpp).
// Two states, 60 and an even 30; the governors' cuts go to their maximum before the rate drops, the governors give quality back at 30 and the rate comes back to 60 when the predicted 60 fits.
// It needs GXV_STEADY30=1 (the even 30's machinery, port::pace::SteadyCadence): this controller forces that cadence on and off as GXV_SMOOTH60 does, so the cadence's own 38 / 50 rules decide nothing; GXV_SMOOTH60
// is ignored while GXV_RATE_AUTO is on (vgx_backend_gl.cpp s60::On), so the two never fight. The switch is only which turns are offered a picture: it is made at the start of a turn (SteadyTurn), the next picture is
// already on the new rhythm, no resource is made or freed. At the seam no picture is held more than 2 refreshes (SteadyCadence::Force: the first turn after it is offered; the turn after a drawn one is not),
// and no picture is added to the queue the skip rule's pipeDepth guard allows (the model: turn to picture is the same before, at and after).
// Keys (read once; the defaults are the model's): GXV_RATE_DOWN_N 30 / _DOWN_M 8 (missed turns of the last N), _SLOW_TURNS 180 / _SLOW_PCT 13, _SETTLE_S 3, _HOLD_TURNS 10, _STALL_MS 50, _BREAK_DOWN_MS 500,
// _LANE_MS 14.5, _LANE30_MS 30, _SPAN_S 1, _GPU_CREDIT 0.4, _MARGIN_MS 0.3, _FIT_HOLD_S 0.5, _FADE_S 3, _PROOF_S 1.5, _BREAK_UP_MS 1000, _RETRY_S 4, _RETRY_MAX_S 80, _FAIL_S 10, _HALVE_S 60,
// _GIVEBACK30 1 (the governors get the 30 budgets while the even 30 stands), _GAIN_ASSUME_MS 1.0 (the gain assumed for a cut level not yet measured), _LOG_S 5; a stage row's rate_pin (GXV_RATE_PIN) 1 = stay at 60, 2 = stay at 30.
#include "port/rate_auto.h"
namespace {
std::mutex sRateLock;
port::rate::Controller sRate;
bool sRateConfigured = false;
float sRateAssumeMs = 1.0f;
uint32_t sRateKey = 0;
char sRateStage[48] = "";
int64_t sRatePrevNowNs = 0, sRatePrevCpu = 0, sRateNextLog = 0, sRateCutNs = 0;
bool sRatePrevPlay = false, sRatePrevDrawn = false, sRatePrevMissed = false;
uint32_t sRateCutsSeen = 0;
uint64_t sRateGovAt = 0;
port::rate::Gov sRateGov;
int sRateLogS = 5;
}  // namespace
static bool RateWanted() {
    static const bool on = [] {
        const bool want = vgx::EnvInt("GXV_RATE_AUTO", 0) != 0;
        if (!want) return false;
        const bool ok = port_perf_mode_60() && vgx::EnvInt("GXV_STEADY30", 0) != 0;
        if (!ok) port_log_summary("rate: GXV_RATE_AUTO=1 ignored: it needs Performance mode (GXV_AUTOQ_TARGET=60, no fixed 30) and GXV_STEADY30=1 (the even 30's machinery)");
        return ok;
    }();
    return on;
}
static void RateConfigure() {
    if (sRateConfigured) return;
    sRateConfigured = true;
    port::rate::Params p;
    p.downN = static_cast<unsigned>(GpEnvFloat("GXV_RATE_DOWN_N", 30, 10, 60));
    p.downM = static_cast<unsigned>(GpEnvFloat("GXV_RATE_DOWN_M", 8, 2, 60));
    p.slowTurns = static_cast<unsigned>(GpEnvFloat("GXV_RATE_SLOW_TURNS", 180, 60, 180));
    p.slowMissPct = GpEnvFloat("GXV_RATE_SLOW_PCT", 13, 3, 60);
    p.settleSec = GpEnvFloat("GXV_RATE_SETTLE_S", 3, 0, 60);
    p.holdTurns = static_cast<unsigned>(GpEnvFloat("GXV_RATE_HOLD_TURNS", 10, 0, 60));
    p.stallMs = GpEnvFloat("GXV_RATE_STALL_MS", 50, 20, 500);
    p.breakWaitDownMs = GpEnvFloat("GXV_RATE_BREAK_DOWN_MS", 500, 0, 1000);
    p.laneLimitMs = GpEnvFloat("GXV_RATE_LANE_MS", 14.5f, 8, 16.6f);
    p.lane30LimitMs = GpEnvFloat("GXV_RATE_LANE30_MS", 30, 16, 33);
    p.laneSpanS = GpEnvFloat("GXV_RATE_SPAN_S", 1.0f, 0.5f, 5);
    p.gpuCredit = GpEnvFloat("GXV_RATE_GPU_CREDIT", 0.4f, 0, 1);
    p.predMarginMs = GpEnvFloat("GXV_RATE_MARGIN_MS", 0.3f, 0, 3);
    p.fitHoldS = GpEnvFloat("GXV_RATE_FIT_HOLD_S", 0.5f, 0, 10);
    p.fadeS = GpEnvFloat("GXV_RATE_FADE_S", 3, 0.5f, 10);
    p.proofS = GpEnvFloat("GXV_RATE_PROOF_S", 1.5f, 0.5f, 10);
    p.breakWaitUpMs = GpEnvFloat("GXV_RATE_BREAK_UP_MS", 1000, 0, 1000);
    p.retryS = GpEnvFloat("GXV_RATE_RETRY_S", 4, 1, 60);
    p.retryMaxS = GpEnvFloat("GXV_RATE_RETRY_MAX_S", 80, 4, 600);
    p.failS = GpEnvFloat("GXV_RATE_FAIL_S", 10, 1, 60);
    p.halveS = GpEnvFloat("GXV_RATE_HALVE_S", 60, 10, 600);
    p.giveback30 = GpEnvFloat("GXV_RATE_GIVEBACK30", 1, 0, 1) != 0.f;
    // pace32: GXV_RATE_STICKY=1 (default 0 = rate30 exactly): the rate class belongs to a stage SECTION. Down as before; up (30 -> 60) only after the lanes fitted for GXV_RATE_STICKY_PROOF_S (20) s
    // without a break AND only at a natural break (a camera cut, a closed screen, a load), however long that takes: never in the middle of play.
    p.sticky = GpEnvFloat("GXV_RATE_STICKY", 0, 0, 1) != 0.f;
    p.stickyProofS = GpEnvFloat("GXV_RATE_STICKY_PROOF_S", 20, 2, 600);
    if (p.sticky) port_log_summary("rate: GXV_RATE_STICKY=1: the even 30 is left only after %.0f s of proven fit and only at a camera cut / closed screen / load (never in the middle of play)", p.stickyProofS);
    sRateAssumeMs = GpEnvFloat("GXV_RATE_GAIN_ASSUME_MS", 1.0f, 0, 5);
    sRateLogS = static_cast<int>(GpEnvFloat("GXV_RATE_LOG_S", 5, 0, 600));
    sRate.Configure(p);
    port_log_summary("rate: GXV_RATE_AUTO on: 60 <-> an even 30, nothing in between. Down: %u of the last %u turns missed (or %.0f %% of %u), governors' cuts to their maximum first; "
                     "up: every lane (game thread drawn turn, front end, render thread, GPU) under %.1f ms predicted for %.1f s, cuts put back first, proved for %.1f s; a failed up doubles the wait (%.0f .. %.0f s); "
                     "a switch waits for a natural break (down %.0f ms, up %.0f ms at most); GXV_SMOOTH60 is ignored",
                     p.downM, p.downN, p.slowMissPct, p.slowTurns, p.laneLimitMs, p.fitHoldS, p.proofS, p.retryS, p.retryMaxS, p.breakWaitDownMs, p.breakWaitUpMs);
}
// the governors as the controller needs them (refreshed 4 times a second): at their maximum or not, and the gain of the levels still to take (learned, else assumed)
static void RateRefreshGov(uint64_t nowUs) {
    if (nowUs < sRateGovAt) return;
    sRateGovAt = nowUs + 250000u;
    port::rate::Gov g;
    g.atMax = true;
    {
        std::lock_guard<std::mutex> lock(sFeGovLock);
        const int mx = sFeGov.config().maxLevel;
        for (int l = sFeGov.level() + 1; l <= mx; ++l) { g.atMax = false; g.gainLeftFeMs += sFeGov.GainKnown(l) ? sFeGov.GainOf(l) : sRateAssumeMs; }
    }
    {
        std::lock_guard<std::mutex> lock(sGameGovLock);
        const int mx = sGameGov.config().maxLevel;
        for (int l = sGameGov.level() + 1; l <= mx; ++l) { g.atMax = false; g.gainLeftGameMs += sGameGov.GainKnown(l) ? sGameGov.GainOf(l) : sRateAssumeMs; }
    }
    sRateGov = g;
}
// the governors go to their maximum levels now (their own 3 s walk fades the cuts in; their own give-back rules bring them down again)
static void RateRaise(const char* why) {
    int fe = 0, gm = 0;
    {
        std::lock_guard<std::mutex> lock(sFeGovLock);
        const int before = sFeGov.level();
        if (sFeGov.config().maxLevel > 0 && sFeGov.ForceLevel(sFeGov.config().maxLevel)) {
            sFeGovLevel.store(sFeGov.level(), std::memory_order_relaxed);
            if (before == 0) sFeMeter.Take(port::q30lock::NowUs());
            ++sFeGovUp;
        }
        fe = sFeGov.level();
    }
    {
        std::lock_guard<std::mutex> lock(sGameGovLock);
        const int before = sGameGov.level();
        if (sGameGov.config().maxLevel > 0 && sGameGov.ForceLevel(sGameGov.config().maxLevel)) {
            sGameGovLevel.store(sGameGov.level(), std::memory_order_relaxed);
            if (before == 0) sGameMeter.Take(port::q30lock::NowUs());
            ++sGameGovUp;
        }
        gm = sGameGov.level();
    }
    sRateGovAt = 0;
    port_log_summary("rate: the governors' cuts to their maximum (front end level %d, game thread level %d): %s", fe, gm, why);
}
static void RateCensus(uint64_t nowUs, const char* tag) {
    char line[700];
    unsigned pend = port_vgx_backend_pending ? port_vgx_backend_pending() : 0;
    const double l = 1.0 + static_cast<double>(pend) * (sRate.Even30() ? 2.0 : 1.0);   // (an estimate: the pictures queued, each held one or two refreshes, and the turn's own)
    sRate.Format(line, sizeof line, sRateStage[0] ? sRateStage : "?", tag, l);
    port_log_summary("%s", line);
    (void)nowUs;
}
static void RateStep(port::pace::SteadyCadence& c, bool allowed, bool quiet, uint64_t nowUs) {
    std::lock_guard<std::mutex> lock(sRateLock);
    RateConfigure();
    const int64_t nowNs = port_host_time_ns();
    const uint32_t key = port_scene_key();
    if (key != sRateKey) {   // a stage (a scene) begins: the last one's census, a fresh controller
        if (sRate.Active() && sRateKey != 0 && (sRate.Sec60() + sRate.Sec30() > 2.0)) RateCensus(nowUs, " (stage end)");
        sRateKey = key;
        char nm[48];
        int sc = -1;
        if (port_scene_loading_stage(nm, sizeof nm, &sc) && nm[0]) snprintf(sRateStage, sizeof sRateStage, "%s%s%d", nm, sc >= 0 ? ":" : "", sc >= 0 ? sc : 0);
        else snprintf(sRateStage, sizeof sRateStage, "%08x", static_cast<unsigned>(key));
        const long pin = port::stageknob::Int("GXV_RATE_PIN", 0);
        sRate.StageBegin(nowUs, static_cast<int>(pin < 0 ? 0 : pin > 2 ? 2 : pin));
        sRatePrevPlay = false;
        sRateGovAt = 0;
        sRateNextLog = nowNs + static_cast<int64_t>(sRateLogS) * 1000000000ll;
        port_log_summary("rate: stage %s begins at 60%s", sRateStage, pin == 1 ? " (pinned to 60 by the stage row)" : pin == 2 ? " (pinned to an even 30 by the stage row)" : "");
    }
    if (port_camera_cut_count) {
        const uint32_t cuts = port_camera_cut_count();
        if (cuts != sRateCutsSeen) { sRateCutsSeen = cuts; sRateCutNs = nowNs; }
    }
    port::rate::Obs o;
    o.play = sRatePrevPlay;
    o.drawn = sRatePrevDrawn;
    o.missed = sRatePrevMissed && sRatePrevPlay;
    o.atBreak = quiet || !allowed || (sRateCutNs != 0 && nowNs - sRateCutNs <= 100000000ll);
    o.gapMs = sRatePrevNowNs ? static_cast<float>(static_cast<double>(nowNs - sRatePrevNowNs) / 1e6) : 0.f;
    {   // the game thread's CPU time of the turn that ended (the difference of two turn-BEGIN samples: the retrace wait costs none, see port::fegov::TurnCpuDelta)
        const int64_t cpu = port_thread_cpu_us();
        int64_t us = 0;
        o.gameMs = port::fegov::TurnCpuDelta(sRatePrevCpu, cpu, true, 100000, &us) ? static_cast<float>(us) / 1000.f : -1.f;
        sRatePrevCpu = cpu;
    }
    sRatePrevNowNs = nowNs;
    RateRefreshGov(nowUs);
    const bool was30 = sRate.Even30();
    const port::rate::Action a = sRate.Step(nowUs, o, sRateGov);
    if (sRate.TakeRaise()) RateRaise(was30 ? "put back before the rate goes up" : "tried before the rate drops");
    {
        float fe = 0, gm = 0;
        if (sRate.TakeLearned(&fe, &gm)) {
            { std::lock_guard<std::mutex> l2(sFeGovLock); sFeGov.LearnJump(fe); }
            { std::lock_guard<std::mutex> l2(sGameGovLock); sGameGov.LearnJump(gm); }
            port_log_summary("rate: the cuts' measured gain: front end %.2f ms, game thread %.2f ms (p95, handed to the governors as the learned gain)", static_cast<double>(fe), static_cast<double>(gm));
        }
    }
    sRateGive.store(sRate.Giveback30() ? 1 : 0, std::memory_order_relaxed);
    // the cadence follows the controller, whatever happened (idempotent)
    const bool want30 = sRate.Even30();
    if (c.Forced() < 0 || c.Engaged() != want30) {
        const unsigned before = c.Changes();
        c.Force(want30 ? 1 : 0, nowUs);
        sSteadyOn.store(c.Engaged(), std::memory_order_relaxed);
        if (c.Changes() != before) sSteadyChanges.fetch_add(1, std::memory_order_relaxed);
    }
    if (a == port::rate::kTo30 && port::stageknob::Int("GXV_RATE_PIN", 0) != 2) AntSpikePost(port::anticipate::kSrcRate);   // (anticip32; not the stage row's pin)
    if (a != port::rate::kNone) {
        float lg = 0, lf = 0, lr = 0, lb = 0;
        sRate.Lanes(&lg, &lf, &lr, &lb);
        port_log_summary("rate: %s: %s now %s (down %u, up %u, failed ups %u); lanes at the decision: game %.1f ms, front end %.1f, render %.1f, GPU %.1f",
                         sRateStage, a == port::rate::kTo30 ? "60 -> even 30" : "even 30 -> 60", sRate.Even30() ? "an even 30" : "60", sRate.Downs(), sRate.Ups(), sRate.FailedUps(),
                         static_cast<double>(lg), static_cast<double>(lf), static_cast<double>(lr), static_cast<double>(lb));
    }
    if (sRateLogS > 0 && nowNs >= sRateNextLog && sRate.Active()) {
        sRateNextLog = nowNs + static_cast<int64_t>(sRateLogS) * 1000000000ll;
        RateCensus(nowUs, "");
    }
}
// anticip32: the announced heavy moment reaches the rate controller (port/rate_auto.h Anticipate): no up-switch until `untilUs`; drop: to the even 30 now. 0 = no controller, 1 = held, 2 = switched.
static int AntRate(uint64_t nowUs, uint64_t untilUs, bool drop) {
    if (!RateWanted()) return 0;
    std::lock_guard<std::mutex> lock(sRateLock);
    if (!sRate.Active()) return 0;
    return sRate.Anticipate(nowUs, untilUs, drop) ? 2 : 1;
}
// What the turn that just began was decided to be: drawn, and whether the skip rule left it out for being late or the back end busy (the 60 state's "missed turn").
static void RateNoteTurn(bool allowed, bool quiet, bool drawn, bool missed) {
    std::lock_guard<std::mutex> lock(sRateLock);
    sRatePrevPlay = allowed && !quiet;
    sRatePrevDrawn = drawn;
    sRatePrevMissed = missed;
}
// speed31: the speed guard left this turn's picture out (an extra undrawn turn, code 2): a missed turn, not a drawn one (RateNoteTurn ran before the guard's filter).
static void RateNoteGuardDrop() {
    std::lock_guard<std::mutex> lock(sRateLock);
    sRatePrevDrawn = false;
    sRatePrevMissed = true;
}
// The back end, once a picture: the front end's, the render thread's and the GPU's time for it, in microseconds (0 / not known: -1 for a lane that was not measured).
extern "C" void port_rate_picture(uint32_t frontUs, int frontKnown, uint32_t renderUs, int renderKnown, uint32_t gpuUs, int gpuKnown, int play) {
    if (!RateWanted()) return;
    {   // (the GPU time without the display slot's wait, when the back end gave it just now: as the Quality ladder reads it)
        const uint64_t now = port::q30lock::NowUs(), at = sQ30GpuAt.load(std::memory_order_relaxed);
        if (at != 0 && now - at < 150000u) { gpuUs = sQ30GpuUs.load(std::memory_order_relaxed); gpuKnown = 1; }
    }
    std::lock_guard<std::mutex> lock(sRateLock);
    sRate.Picture(static_cast<uint64_t>(port_host_time_ns() / 1000), frontKnown ? static_cast<float>(frontUs) / 1000.f : -1.f, renderKnown ? static_cast<float>(renderUs) / 1000.f : -1.f,
                  gpuKnown ? static_cast<float>(gpuUs) / 1000.f : -1.f, play != 0);
}
// ---- speed31: the speed guard's dropped pictures as a signal for every cut system (GXV_SPEED_GUARD_SIGNAL, default 1; 0 = none reads the guard) ----------------------------------------------------
// The guard (port/speed_guard.h) repays lost game time with extra undrawn turns: a picture left out, which is a fault to be removed (the owner, 13 Nov: game speed stays, a dropped picture is the
// relief, the cuts remove the cause). Pressure() says when the repayment is SUSTAINED (the natural loss of a picture over the leak, a trimmed mean of five pictures, for 0.3 s): an isolated hitch has none.
//  - Quality: the ladder (q30lock.h NoteGuard): a threat like a missed picture, acted on after guardConfirmSec with the fast gap, several steps in one change when one is not enough; the proof of headroom is lost.
//  - Performance at 60 with GXV_RATE_AUTO: nothing here: the dropped picture is a missed turn for the rate controller (RateNoteGuardDrop), which tries the cuts and then goes to the even 30.
//  - Performance otherwise (and at the even 30): the governors (port/fegov.h Press): ONE level at once (two when the learned gain of one is under the need), at most one press every GXV_SPEED_GUARD_PRESS_GAP_MS
//    (3500: a fade is 3 s); the governor whose level is the lower is pressed (the cuts read the larger). The far field is these levels. A window with pressure in it is never calm (no give-back while the
//    guard still repays): the governors' own recover30 give-back rules and anti-flap apply after.
// Census: "speed: guard signal: ..." one line a press; the cuts are counted in the speed line ("cuts taken because of drops").
static void SpeedGuardAct(uint64_t nowUs) {
    port::pace::SpeedGuard& g = port::pace::GlobalSpeedGuard();
    if (!g.Enabled() || !SpeedGuardSignalOn()) return;
    port::pace::SpeedGuard::PressureInfo pr = g.Pressure();
    if (g.Slipping() && !pr.sustained) {   // (pace32: the slip band hides the drops, not the overload: the cuts read it as sustained pressure of the measured loss a picture)
        pr.sustained = true;
        pr.needMs = std::max(0.5, g.LossPermille() * 33.37 / 1000.0);
        pr.extrasPerPicture = g.EpsMs() > 0 ? pr.needMs / g.EpsMs() : 0.0;
    }
    if (pr.sustained) AntSpikePost(port::anticipate::kSrcGuard);   // (anticip32: pictures are being dropped here)
    if (sLockArmed.load(std::memory_order_relaxed)) {   // (Quality with the lock ladder: the ladder owns the cuts)
        sLadder.NoteGuard(port::q30lock::NowUs(), pr.sustained, static_cast<float>(pr.needMs));
        const uint32_t n = sLadder.TakeGuardCuts();
        for (uint32_t i = 0; i < n; ++i) g.NoteCut();
        return;
    }
    const bool missP = GuardMissSustained() && !sSteadyOn.load(std::memory_order_relaxed);   // (the skip rule's own left-out turns, not at the even 30 where the rhythm is by design)
    if (!pr.sustained && !missP) return;
    sGuardPressUs.store(port::q30lock::NowUs(), std::memory_order_relaxed);
    if (RateWanted()) {
        bool even30;
        { std::lock_guard<std::mutex> lk(sRateLock); even30 = sRate.Even30(); }
        if (!even30) return;   // (60: the rate controller's evidence)
    }
    static uint64_t lastPressUs = 0;
    static const uint64_t gapUs = static_cast<uint64_t>(vgx::EnvInt("GXV_SPEED_GUARD_PRESS_GAP_MS", 3500)) * 1000u;
    if (lastPressUs != 0 && nowUs - lastPressUs < gapUs) return;
    const int lf = sFeGovLevel.load(std::memory_order_relaxed), lg = sGameGovLevel.load(std::memory_order_relaxed);
    const float need = pr.sustained ? static_cast<float>(pr.needMs) : 1.0f;
    int took = 0, which = 0, from = 0, to = 0;
    for (int pass = 0; pass < 2 && !took; ++pass) {
        const bool game = pass == 0 ? lg <= lf : lg > lf;
        if (game) {
            std::lock_guard<std::mutex> lock(sGameGovLock);
            const int before = sGameGov.level();
            took = sGameGov.Press(need, 2);
            if (sAntLevels.load(std::memory_order_relaxed) != 0) sGameGov.VoidPending();   // (anticip32)
            if (took) { sGameGovLevel.store(sGameGov.level(), std::memory_order_relaxed); if (before == 0) sGameMeter.Take(port::q30lock::NowUs()); sGameGovUp += static_cast<uint32_t>(took); which = 1; from = before; to = sGameGov.level(); }
        } else {
            std::lock_guard<std::mutex> lock(sFeGovLock);
            const int before = sFeGov.level();
            took = sFeGov.Press(need, 2);
            if (sAntLevels.load(std::memory_order_relaxed) != 0) sFeGov.VoidPending();   // (anticip32)
            if (took) { sFeGovLevel.store(sFeGov.level(), std::memory_order_relaxed); if (before == 0) sFeMeter.Take(port::q30lock::NowUs()); sFeGovUp += static_cast<uint32_t>(took); which = 2; from = before; to = sFeGov.level(); }
        }
    }
    if (took) {
        lastPressUs = nowUs;
        for (int i = 0; i < took; ++i) g.NoteCut();
        port_log_summary("speed: guard signal: %s: %s governor level %d -> %d (the cuts read %d)",
                         pr.sustained ? "the guard repays the game thread's natural loss (see the next numbers)" : "8 of the last 30 turns were left out by the skip rule",
                         which == 1 ? "game thread" : "front end", from, to, GovMax());
        if (pr.sustained) port_log_summary("speed: guard signal: %.1f ms a picture repaid (%.1f extra turns a picture) for %.1f s", pr.needMs, pr.extrasPerPicture, pr.heldSec);
    }
}
// GXV_FLUSH_OFFSCREEN (LiveActor.cpp port_anim_flush_stale): a plant (mLogicFreeJoints: nothing but the picture reads its joints) whose
// whole bounding sphere, with a margin, is outside this picture's view pyramid gets no joint computation (and so no material update)
// for the picture; it is computed again, from the state of then, at the first picture that may show it. 0 / 1 / perf.
extern "C" int port_flush_offscreen_mode(void) { return PORT_HOT_KNOB(int, GpModeKnob("GXV_FLUSH_OFFSCREEN")); }
// GXV_PLANT_DRAWCULL (with GXV_FLUSH_OFFSCREEN on): such a plant is also not drawn in that picture (DrawBuffer.cpp: its packets are left out
// as a hidden shape's are; it puts no pixel in the picture). Saves the game thread's draw() and the front end's draws. 0 / 1 / perf.
extern "C" int port_plant_drawcull_mode(void) { return PORT_HOT_KNOB(int, GpModeKnob("GXV_PLANT_DRAWCULL")); }
// GXV_HR_PLANTMOVE (PlantGroup.cpp): a flower group's idle members (Wait nerve, past its first step, no light blend) skip their movement
// every second turn; the skipped turn's animation-clock step is replayed before the next movement, so the clock, the nerve logic and
// every picture's pose are those of the original. The group must be farther than GXV_HR_PLANT_PDIST (default 1500 units, beyond its
// touch sensor) from the player. 0 / 1 / perf.
extern "C" int port_hr_plantmove_mode(void) { return port::lockstep::On() ? (port_mpgate_note(port::mpgate::kPlantMove), 0) /* (fmt32: the rest is chosen by this console's mode and its own player's distance; 3.0 lodgate D7 held: the same rule, noted once per session) */ : PORT_HOT_KNOB(int, GpModeKnob("GXV_HR_PLANTMOVE")); }
// GXV_PTCL_LOOKPAR (gamecpu23, 8 Nov; JPAEmitterManager.cpp; acts only in GXV_PAR_PTCL's parallel turns): the parallel chains' emitters run
// their particles' look functions in the calc (on the particle worker, mostly) when a picture is expected next, instead of leaving them to
// the game thread's draw (GXV_JPA_LOOK). Exact (the original's calc). 0 / 1 / perf; 2 = on + verify at the draw ("ptcl-lookpar:" line).
extern "C" int port_ptcl_lookpar_mode(void) { return PORT_HOT_KNOB(int, GpModeKnob("GXV_PTCL_LOOKPAR")); }
// GXV_JPA_LOOK_PURE (gamecpu23; JPAResource.cpp): GXV_JPA_LOOK also for the emitters whose particle callback does nothing (every MultiEmitter
// effect's): their look functions run once a picture, at the draw, instead of every turn. 0 / 1 / perf; 2 = verify (as before + compared
// at the draw, "ptcl-lookpar:" line). Needs GXV_JPA_LOOK (default on).
extern "C" int port_jpa_look_pure_mode(void) { return PORT_HOT_KNOB(int, GpModeKnob("GXV_JPA_LOOK_PURE")); }
namespace {
std::atomic<uint32_t> sLookParChecked{0}, sLookParBad{0};
uint32_t sLookParLogged = 0;
}  // namespace
// (the game thread's draw only)
extern "C" void port_ptcl_lookpar_note(unsigned checked, unsigned bad) {
    const uint32_t c = sLookParChecked.fetch_add(checked, std::memory_order_relaxed) + checked;
    const uint32_t b = sLookParBad.fetch_add(bad, std::memory_order_relaxed) + bad;
    if (bad != 0 && sLookParLogged < 8) {
        ++sLookParLogged;
        port_log_summary("ptcl-lookpar: verify MISMATCH: %u of %u particles of an emitter differ between the worker's calc-time look and the draw-time look", bad, checked);
    }
    if (c >= 20000) {
        port_log_summary("ptcl-lookpar: verify: %u particles checked at the draw, %u differed (GXV_PTCL_LOOKPAR=2; must be 0)", c, b);
        sLookParChecked.store(0, std::memory_order_relaxed);
        sLookParBad.store(0, std::memory_order_relaxed);
    }
}
extern "C" float port_hr_plant_pdist(void) { return PORT_HOT_KNOB(float, GpEnvFloat("GXV_HR_PLANT_PDIST", 1500.0f, 0.0f, 100000.0f)); }
// GXV_TURN_PROF=1 (default 0; every build): one "turn-prof:" line in the basic log (log.txt) every 5 s that splits the game thread's turn
// into its categories, for the drawn and the undrawn turns, with the work of the cuts above. About 12 clock reads a turn (~30 us);
// no per-object timer. Buckets (laps from the start of GameSystem::frameLoop to the retrace wait):
//   fe-wait  the front-end waits (the drawn turn's draw-done settle, GameSystem::calcAnim's deferred draw-done wait)
//   flush    the drawn turn's anim / dl / view flushes            draw   beginRender + draw() + the view retire + endRender
//   update   GameSystem::update (movement lists etc.)              calc   calc-anim lists before the shadows
//   shadow   ShadowControllerHolder                                ptcl   AnimParticle (+ IgnorePause)
//   view     the rest of calc-anim (the view pass, the wipe)       other  capture, endFrame, the frame-begin bookkeeping
enum { kTpCat = 16 };  // (undrawn26: the calc-anim lists SceneFunction::executeCalcAnimList runs, in its order: 0..12, 13 the shadow list, 14 the particle list, 15 rest)
enum { kTpFeWait, kTpFlAnim, kTpFlDl, kTpFlView, kTpDraw, kTpUpdate, kTpCalc, kTpShadow, kTpPtcl, kTpView, kTpOther, kTpN };
enum { kTcPlantSkip, kTcPlantMoved, kTcFlushOff, kTcFlushDone, kTcN };
// (strikers2audit, GXV_TURN_PROF only) the "other" bucket of a drawn turn split by marks: port_turn_prof_mark(k) books the time since the last mark (or the
// turn's start) under k. The buckets above are not touched: "other" still holds all of it, the marks are a second, finer clock that only the extra
// "turn-prof: other of a drawn turn" line reads.
enum { kSubDecide, kSubPrepare, kSubCapture, kSubEndFrame, kSubTurnEnd, kSubN };
// GXV_TURN_PROF=2 / GXV_DRAW_PROF=2 (gamecpu23, 8 Nov): draw() split into parts (port_draw_part_begin / _end around the calls, GameScene.cpp):
// shadows (the volume, silhouette / fill, alpha and surface shadow lists), effects (the 3D effect lists, the bloom effects, the effects
// after the image effects), layouts (draw2D), 3d (draw3D whole), mirror (drawMirror). Two clock reads a part; ~10 parts a drawn turn.
enum { kDpShadow, kDpEffect, kDp2D, kDp3D, kDpMirror, kDpShv, kDpSil, kDpSha, kDpShs, kDpN };  // (cpu2-26: 5-8 = the shadow part split, nested in kDpShadow)
struct ShvCensus { uint64_t asked = 0, drawn = 0, bytes = 0, ns = 0; };  // (imp45: the shadow volume pass by drawer, port_shv_note)
static ShvCensus sShv;
namespace {
struct TurnProf {
    int64_t last = 0;
    int64_t turnT0 = 0;
    bool active = false;
    uint64_t ns[2][kTpN] = {};  // [drawn][bucket]
    uint64_t cnt[kTcN] = {};
    uint32_t turns[2] = {};
    uint64_t maxNs = 0;
    uint32_t over13 = 0;
    int64_t winT0 = 0;
    uint64_t cur[kTpN] = {};
    uint64_t part[kDpN] = {};  // (GXV_TURN_PROF=2, gamecpu23: the draw parts of the drawn turns, port_draw_part_end)
    uint64_t sub[kSubN] = {};  // (strikers2audit: the marks of the drawn turns' "other", port_turn_prof_mark)
    int64_t subLast = 0;
    uint64_t endFrameAtBegin = 0;  // (hitch25: sub[kSubEndFrame] when the turn began, so the turn's own end-frame time is known)
    // (undrawn26, GXV_TURN_PROF=2) the calc-anim lists, one by one: [drawn][list], and this turn's
    uint64_t catNs[2][kTpCat] = {};
    uint64_t catCur[kTpCat] = {};
    int64_t catLast = 0;
} sTp;
}  // namespace
// (firstuse24) GXV_FIRSTUSE_TURN=1 (default 0): the turn timer also runs without GXV_TURN_PROF (mode 3: no 5 s line) and a game turn in play that
// takes GXV_FIRSTUSE_TURN_MS (default 40) or more gets a "first-use: game thread turn" line with its categories and the time since the last pad
// input: the first run / jump of a stage, if it costs the game thread something, shows here. About 12 clock reads a turn (~30 us).
extern "C" int64_t port_player_input_age_ms(void) __attribute__((weak));  // platform/src/input/wpad.cpp
static bool FirstUseOn() {
    return PORT_HOT_KNOB(bool, [] { const char* e = getenv("GXV_FIRSTUSE_LOG"); return !(e && *e == '0'); }());
}
static bool FirstUseTurnOn() {
    return PORT_HOT_KNOB(bool, [] { const char* e = getenv("GXV_FIRSTUSE_TURN"); return e && *e == '1'; }());
}
// (firstuse24 r2, GXV_FIRSTUSE_PLAYONLY, default 1) "in play" is a real stage with the screen open: the title's file select is a stage with a scene key too
// (the name "FileSelect"), and its turns (a 40 ms draw every ~8 s: the stream's segment waits, not something a player did) used the line budget up before
// the first galaxy. 0 = as round 1. Every line budget below is per stage (the scene key), not per boot.
static bool FirstUseLive() {
    const uint32_t key = port_scene_key();
    if (key == 0 || (port_screen_quiet && port_screen_quiet() != 0)) return false;
    static const bool playOnly = [] { const char* e = getenv("GXV_FIRSTUSE_PLAYONLY"); return !(e && *e == '0'); }();
    static const uint32_t fileSelect = StageKeyOf("FileSelect");
    return !(playOnly && key == fileSelect);
}
static bool FirstUseBudget(std::atomic<unsigned>& used, std::atomic<uint32_t>& forKey, unsigned max) {
    const uint32_t key = port_scene_key();
    if (forKey.load(std::memory_order_relaxed) != key) {
        forKey.store(key, std::memory_order_relaxed);
        used.store(0, std::memory_order_relaxed);
    }
    return used.fetch_add(1, std::memory_order_relaxed) < max;
}
// The game thread's first-use timers (port/firstuse.h): the first start of a sound id, the first emit of an effect name.
extern "C" void port_firstuse_game(const char* kind, unsigned id, const char* name, int64_t ns) {
    if (!FirstUseOn() || !FirstUseLive()) return;
    static const int64_t minNs = [] { const char* e = getenv("GXV_FIRSTUSE_GAME_US"); return (e && *e ? (int64_t)atoi(e) : 1500) * 1000; }();
    if (ns < minNs) return;
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the kind, the id and the name: "seen before" is a small open-addressed table
    for (const char* c = kind; *c; ++c) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    h = (h ^ id) * 1099511628211ull;
    for (const char* c = name ? name : ""; *c; ++c) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    h |= 1;
    static std::atomic<uint64_t> seen[1024];
    for (unsigned i = 0, at = (unsigned)(h >> 7) & 1023u; i < 1024; ++i, at = (at + 1) & 1023u) {
        uint64_t cur = seen[at].load(std::memory_order_relaxed);
        if (cur == h) return;  // not the first time (it was timed before, and logged if slow)
        if (cur == 0 && seen[at].compare_exchange_strong(cur, h)) break;
        if (cur == h) return;
    }
    static std::atomic<unsigned> lines{0};
    static std::atomic<uint32_t> linesKey{0};
    if (!FirstUseBudget(lines, linesKey, 40)) return;  // (40 a stage)
    const int64_t age = port_player_input_age_ms ? port_player_input_age_ms() : -1;
    char since[48] = "no pad input yet";
    if (age >= 0) snprintf(since, sizeof since, "pad input %lld ms ago", (long long)age);
    port_log_summary("first-use: game thread: %s '%s' (id %08x) used for the first time in play: %.1f ms (%s)", kind, name ? name : "", id, ns / 1e6, since);
}
extern "C" int port_undrawn_lean_mode(void);
extern "C" int port_turn_prof_on(void) {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_TURN_PROF");
        const int mode = e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0;
        return mode ? mode : (FirstUseTurnOn() ? 3 : 0);
    }());
}
// pace33 PATCH5 (14 Nov 2026; console batches 4-5: at Beach Bowl two thirds of the pictures that reach the render thread late come with a game turn gap of 21-25 ms, while the 5 s means
// say a drawn turn's work is 11-15 ms): the LATE turns on their own. A turn is late when the next turn began 21..80 ms after it began (80+: a stall, hitch25's). Its buckets are added up
// apart from the window's, and each late turn is one of two kinds: its own work filled the gap (work 16.7 ms or more), or its work was shorter and the next turn still began late (the game
// thread was not woken or not run in time: the retrace wake-up, another guest thread, core 0). One more "turn-prof:" line per 5 s, only when there were late turns in play.
static struct TpLate {
    uint64_t prev[kTpN] = {};      // the turn that just ended: its buckets, its work
    uint64_t prevWork = 0;
    int64_t prevT0 = 0;
    int prevDrawn = -1;            // -1: nothing to judge (the first turn, not in play)
    uint64_t ns[2][kTpN] = {};     // late turns' buckets [drawn][bucket]
    uint32_t n[2] = {}, woke[2] = {};
    uint64_t gapNs[2] = {}, workNs[2] = {}, wokeLateNs[2] = {};
} sTpLate;
static void TpLateBegin(int64_t now) {
    TpLate& L = sTpLate;
    if (L.prevDrawn >= 0 && L.prevT0) {
        const int64_t gap = now - L.prevT0;
        if (gap >= 21000000ll && gap < 80000000ll) {
            const int d = L.prevDrawn;
            for (int b = 0; b < kTpN; ++b) L.ns[d][b] += L.prev[b];
            ++L.n[d];
            L.gapNs[d] += static_cast<uint64_t>(gap);
            L.workNs[d] += L.prevWork;
            if (L.prevWork < 16683350ull) { ++L.woke[d]; L.wokeLateNs[d] += static_cast<uint64_t>(gap) - 16683350ull; }
        }
    }
    L.prevDrawn = -1;
    L.prevT0 = now;
}
extern "C" void port_turn_prof_begin(void) {
    if (!port_turn_prof_on()) return;
    sTp.last = sTp.turnT0 = sTp.subLast = port_host_time_ns();
    TpLateBegin(sTp.turnT0);
    sTp.endFrameAtBegin = sTp.sub[kSubEndFrame];
    memset(sTp.cur, 0, sizeof sTp.cur);
    memset(sTp.catCur, 0, sizeof sTp.catCur);
    sTp.active = true;
}
extern "C" void port_turn_prof_lap(int bucket) {
    if (!sTp.active || bucket < 0 || bucket >= kTpN || !port_turn_prof_on()) return;
    const int64_t now = port_host_time_ns();
    if (now > sTp.last) sTp.cur[bucket] += static_cast<uint64_t>(now - sTp.last);
    sTp.last = now;
}
// ---- pace32 (13 Nov 2026): GXV_LATE_TURN (default 0 = off), "phase sync" for the even 30 (port/late_turn.h has the rule and the reasons; tests/pace32_test.cpp) ------------------------------------
// The undrawn turn of an even-30 pair (code 1: it keeps its retrace) sleeps FIRST and then reads the pad, so that it ends just before its retrace instead of finishing early and idling with a pad
// sample that grows stale: the picture the next turn draws shows a pad read that is fresher by the sleep. Eligible: code 1, the even 30 in force (Performance's steady 30 engaged, or the fixed 30
// of Quality), the screen not covered. The sleep is taken out of the wall-time meters of the turn's work (the Quality ladder's game pair, the steady 30's cost samples, the turn profiler).
// GXV_LATE_TURN_MARGIN_US (2500): the turn is planned to end this long before its retrace; GXV_LATE_TURN_MAX_US (12000). Line: "late-turn:" every 10 s while it runs.
namespace {
port::lateturn::Planner sLt;
bool sLtEligible = false;
uint32_t sLtSleptUs = 0;
int64_t sLtWorkDoneNs = 0, sLtBeginNs = 0, sLtLogNs = 0;
}  // namespace
extern "C" int port_hrw_mode(void) __attribute__((weak));   // (vgx_galaxy.cpp)
extern "C" void port_hrw_fresh_deadline_push(long long sleepNs) __attribute__((weak));   // (vgx_galaxy.cpp: GXV_LATE_TURN with the fresh player)
static bool LateTurnOn() {
    static const bool on = [] {
        port::lateturn::Config c;
        c.on = vgx::EnvInt("GXV_LATE_TURN", 0) != 0;
        c.marginUs = static_cast<uint32_t>(std::min<long>(8000, std::max<long>(500, vgx::EnvInt("GXV_LATE_TURN_MARGIN_US", 2500))));
        c.maxUs = static_cast<uint32_t>(std::min<long>(15000, std::max<long>(500, vgx::EnvInt("GXV_LATE_TURN_MAX_US", 12000))));
        sLt.Configure(c);
        if (c.on)
            port_log_summary("late-turn: GXV_LATE_TURN=1: at an even 30 the turn without a picture starts as late as its measured idle time allows (margin %.1f ms, at most %.1f ms), so the pad read "
                             "the next picture shows is fresher; a delayed turn that misses its retrace switches it off for 3 s and widens the margin",
                             c.marginUs / 1000.0, c.maxUs / 1000.0);
        return c.on;
    }();
    return on;
}
// GameSystem::frameLoop, an undrawn turn, after the pacing decision and before the update (which reads the pad). code: the decision (1 keeps its retrace, 2 does not).
extern "C" void port_late_turn_wait(int code) {
    if (!LateTurnOn() || port::lockstep::On()) return;   // (fmt32: no sleep before the pad read in a lockstep session: the session times the turn)
    static const bool fixed30 = vgx::EnvInt("GXV_FPS_LIMIT", 0) == 30;
    // (pace33: the half-rate world (GXV_HRW 1 / 5) is a two-turn rhythm too: its turn without a picture is eligible. The pair's second picture was asked for before this call
    //  (port_hrw_undrawn_begin), so the sleep delays only the update and its pad read.)
    const int hrw = port_hrw_mode ? port_hrw_mode() : 0;
    const bool rhythm = fixed30 || sSteadyOn.load(std::memory_order_relaxed) || hrw == 1 || (hrw == 5 && !(port_hrw_plain60 && port_hrw_plain60())) || (port_hrw_even30_noassist && port_hrw_even30_noassist() != 0);
    const bool quiet = port_screen_quiet && port_screen_quiet() != 0;
    static bool wasRhythm = false;
    if (!rhythm || quiet) {
        if (wasRhythm) sLt.Reset();   // (a covered screen, the even 30 left: the idle times measured are not the next rhythm's)
        wasRhythm = false;
        return;
    }
    wasRhythm = true;
    if (code != 1) return;
    sLtEligible = true;
    const uint32_t d = sLt.Plan();
    if (!d) return;
    if (port_hrw_fresh_deadline_push) port_hrw_fresh_deadline_push(static_cast<long long>(d) * 1000);   // (coordinator 14 Nov: before the sleep, so the render thread does not give up during it)
    const int64_t t0 = port_host_time_ns();
    port_host_sleep_ns(static_cast<int64_t>(d) * 1000);
    const int64_t slept = port_host_time_ns() - t0;
    if (slept <= 0) return;
    sLtSleptUs = sLateSleptUs = static_cast<uint32_t>(slept / 1000);
    if (sLockTurnStartNs) sLockTurnStartNs += slept;                                   // (the Quality ladder's game pair)
    if (sTp.active) { sTp.last += slept; sTp.subLast += slept; sTp.turnT0 += slept; }  // (the turn profiler: the sleep is in no bucket)
    if (port_hrw_fresh_deadline_push) port_hrw_fresh_deadline_push(0);   // (coordinator 14 Nov: and from the sleep's end, for an oversleep)
}
static void LateTurnWorkDone() {
    if (sLtEligible) sLtWorkDoneNs = port_host_time_ns();
}
static void LateTurnBegin() {
    if (!LateTurnOn()) return;
    const int64_t now = port_host_time_ns();
    if (sLtEligible && sLtWorkDoneNs && sLtBeginNs) {
        const int64_t idle = now - sLtWorkDoneNs, gap = now - sLtBeginNs;
        sLt.Result(idle > 0 && idle < 100000000ll ? static_cast<uint32_t>(idle / 1000) : 0u, sLtSleptUs, gap > 25025025ll);   // (late: the next turn began more than a refresh and a half after this one)
    }
    sLtEligible = false;
    sLtSleptUs = sLateSleptUs = 0;
    sLtWorkDoneNs = 0;
    sLtBeginNs = now;
    if (!sLtLogNs) sLtLogNs = now;
    if (now - sLtLogNs >= 10000000000ll) {
        if (sLt.GetStats().turns) {
            char line[512];
            port::lateturn::FormatLine(line, sizeof line, sLt, (now - sLtLogNs) / 1e9);
            port_log_summary("%s", line);
        }
        sLt.ClearStats();
        sLtLogNs = now;
    }
}
extern "C" void port_turn_prof_mark(int sub) {
    if (!sTp.active || !port_turn_prof_on()) return;
    const int64_t now = port_host_time_ns();
    if (sub >= 0 && sub < kSubN && now > sTp.subLast) sTp.sub[sub] += static_cast<uint64_t>(now - sTp.subLast);
    sTp.subLast = now;  // (sub < 0: only restarts the mark clock)
}
extern "C" void port_turn_prof_count(int what, unsigned n) {
    if (what >= 0 && what < kTcN && port_turn_prof_on()) sTp.cnt[what] += n;
}
// (undrawn26, GXV_TURN_PROF=2) port_turn_prof_cat_begin() starts the list clock; port_turn_prof_cat(k) books the time since the last call under list k.
extern "C" void port_turn_prof_cat_begin(void) {
    if (!sTp.active || port_turn_prof_on() != 2) return;
    sTp.catLast = port_host_time_ns();
}
extern "C" void port_turn_prof_cat(int k) {
    if (!sTp.active || k < 0 || k >= kTpCat || port_turn_prof_on() != 2) return;
    const int64_t now = port_host_time_ns();
    if (now > sTp.catLast) sTp.catCur[k] += static_cast<uint64_t>(now - sTp.catLast);
    sTp.catLast = now;
}
extern "C" void port_turn_prof_end(int drawn) {
    if (!sTp.active) return;
    port_turn_prof_lap(kTpOther);
    sTp.active = false;
    const int d = drawn ? 1 : 0;
    uint64_t work = 0;
    for (int b = 0; b < kTpN; ++b) {
        sTp.ns[d][b] += sTp.cur[b];
        if (b != kTpFeWait) work += sTp.cur[b];
    }
    for (int k = 0; k < kTpCat; ++k) sTp.catNs[d][k] += sTp.catCur[k];
    {  // (hitch25) the turn's largest part, for the slow-turn ring (a gap of 100 ms or more between turn starts names it): one pass over 11 numbers a turn
        const uint64_t endFrame = sTp.sub[kSubEndFrame] >= sTp.endFrameAtBegin ? sTp.sub[kSubEndFrame] - sTp.endFrameAtBegin : 0;
        const uint64_t part[port::hitch25::kPartN] = {sTp.cur[kTpUpdate], sTp.cur[kTpCalc], sTp.cur[kTpShadow], sTp.cur[kTpPtcl], sTp.cur[kTpView],
                                                      sTp.cur[kTpFlAnim] + sTp.cur[kTpFlDl] + sTp.cur[kTpFlView], sTp.cur[kTpDraw], sTp.cur[kTpFeWait],
                                                      d ? endFrame : 0, sTp.cur[kTpOther] > (d ? endFrame : 0) ? sTp.cur[kTpOther] - (d ? endFrame : 0) : 0, 0};
        unsigned best = 0;
        uint64_t total = 0;
        for (unsigned i = 0; i < port::hitch25::kPartN; ++i) {
            total += part[i];
            if (part[i] > part[best]) best = i;
        }
        sH25LastPart = static_cast<uint8_t>(best);
        sH25LastPartUs = static_cast<uint32_t>(part[best] / 1000);
        sH25LastTotalUs = static_cast<uint32_t>(total / 1000);
    }
    if (!port_screen_quiet || port_screen_quiet() == 0) {   // (PATCH5: judged at the next turn's begin)
        memcpy(sTpLate.prev, sTp.cur, sizeof sTpLate.prev);
        sTpLate.prevWork = work;
        sTpLate.prevDrawn = d;
    }
    ++sTp.turns[d];
    if (work > sTp.maxNs) sTp.maxNs = work;
    if (work > 13000000u) ++sTp.over13;
    const int64_t now = sTp.last;
    if (FirstUseOn() && FirstUseTurnOn() && FirstUseLive()) {  // (firstuse24) a turn of the game thread in play that took long: its categories and the pad
        static const uint64_t minNs = [] { const char* e = getenv("GXV_FIRSTUSE_TURN_MS"); return (uint64_t)(e && *e ? atoi(e) : 40) * 1000000ull; }();
        static int64_t winStart = 0;
        static unsigned lines = 0;
        static uint32_t linesKey = 0;
        if (now - winStart > 60000000000LL || linesKey != port_scene_key()) { winStart = now; lines = 0; linesKey = port_scene_key(); }  // (16 a stage, and a minute)
        if (work >= minNs && lines < 16) {
            ++lines;
            const int64_t age = port_player_input_age_ms ? port_player_input_age_ms() : -1;
            char since[48] = "no pad input yet";
            if (age >= 0) snprintf(since, sizeof since, "pad input %lld ms ago", (long long)age);
            auto ms = [&](int b) { return sTp.cur[b] / 1e6; };
            port_log_summary("first-use: game thread turn took %.0f ms in play (%s, %s): update %.1f + calc-anim %.1f + shadows %.1f + particles %.1f + view/rest %.1f + "
                             "flushes %.1f + draw %.1f + other %.1f; front-end waits %.1f",
                             work / 1e6, d ? "drawn" : "undrawn", since, ms(kTpUpdate), ms(kTpCalc), ms(kTpShadow), ms(kTpPtcl), ms(kTpView),
                             ms(kTpFlAnim) + ms(kTpFlDl) + ms(kTpFlView), ms(kTpDraw), ms(kTpOther), ms(kTpFeWait));
        }
    }
    if (sTp.winT0 == 0) {
        sTp.winT0 = now;
        return;
    }
    if (now - sTp.winT0 < 5000000000LL) return;
    if (port_turn_prof_on() == 3) {  // (firstuse24) no 5 s line: only the long turns above
        const int64_t w3 = now;
        sTp = TurnProf{};
        sTp.winT0 = w3;
        return;
    }
    const double secs = (now - sTp.winT0) / 1e9;
    const uint32_t nT = sTp.turns[0] + sTp.turns[1];
    const double t = nT ? nT : 1, td = sTp.turns[1] ? sTp.turns[1] : 1, tu = sTp.turns[0] ? sTp.turns[0] : 1;
    auto all = [&](int b) { return (sTp.ns[0][b] + sTp.ns[1][b]) / 1e6 / t; };
    auto dr = [&](int b) { return sTp.ns[1][b] / 1e6 / td; };
    auto un = [&](int b) { return sTp.ns[0][b] / 1e6 / tu; };
    double busyAll = 0, busyD = 0, busyU = 0;
    for (int b = 0; b < kTpN; ++b) {
        if (b == kTpFeWait) continue;
        busyAll += all(b);
        busyD += dr(b);
        busyU += un(b);
    }
    if (sTpLate.n[0] + sTpLate.n[1]) {   // (PATCH5)
        const TpLate& L = sTpLate;
        char part[2][300];
        for (int k = 0; k < 2; ++k) {
            const double n = L.n[k] ? L.n[k] : 1;
            auto lm = [&](int b) { return L.ns[k][b] / 1e6 / n; };
            snprintf(part[k], sizeof part[k], "%u (gap %.1f, work %.1f = update %.1f + calc-anim %.1f + shadows %.1f + particles %.1f + flushes %.1f + draw %.1f + other %.1f, front-end waits %.1f; "
                                              "%u with work under 16.7 ms, begun %.1f ms late)",
                     L.n[k], L.gapNs[k] / 1e6 / n, L.workNs[k] / 1e6 / n, lm(kTpUpdate), lm(kTpCalc), lm(kTpShadow), lm(kTpPtcl), lm(kTpFlAnim) + lm(kTpFlDl) + lm(kTpFlView), lm(kTpDraw),
                     lm(kTpOther) + lm(kTpView), lm(kTpFeWait), L.woke[k], L.woke[k] ? L.wokeLateNs[k] / 1e6 / L.woke[k] : 0.0);
        }
        port_log_summary("turn-prof: late turns in play (the next turn began 21-80 ms after this one; ms a turn): drawn %s; undrawn %s; compare with the window's means in the line below",
                         part[1], part[0]);
        const int64_t keepT0 = sTpLate.prevT0;
        const int keepDrawn = sTpLate.prevDrawn;
        const uint64_t keepWork = sTpLate.prevWork;
        uint64_t keep[kTpN];
        memcpy(keep, sTpLate.prev, sizeof keep);
        sTpLate = TpLate();
        sTpLate.prevT0 = keepT0; sTpLate.prevDrawn = keepDrawn; sTpLate.prevWork = keepWork;
        memcpy(sTpLate.prev, keep, sizeof keep);
    }
    port_log_summary("turn-prof: %.1f s, %u turns (%u drawn); ms a turn: busy %.2f (max %.2f; %u turns over 13 ms) = update %.2f + calc-anim %.2f + shadows %.2f "
                     "+ particles %.2f + view/rest %.2f + flushes %.2f + draw %.2f + other %.2f, front-end waits %.2f; a drawn turn: busy %.2f = update %.2f "
                     "+ calc-anim %.2f + shadows %.2f + particles %.2f + view/rest %.2f + flushes %.2f [anim %.2f dl %.2f view %.2f] + draw %.2f + other %.2f; "
                     "an undrawn turn: busy %.2f; cuts: plant moves skipped %.1f a turn (moved %.1f), plants off-screen (not posed%s) %.1f a drawn turn "
                     "(far models posed by the flush %.1f)",
                     secs, nT, sTp.turns[1], busyAll, sTp.maxNs / 1e6, sTp.over13, all(kTpUpdate), all(kTpCalc), all(kTpShadow), all(kTpPtcl), all(kTpView),
                     all(kTpFlAnim) + all(kTpFlDl) + all(kTpFlView), all(kTpDraw), all(kTpOther), all(kTpFeWait), busyD, dr(kTpUpdate), dr(kTpCalc), dr(kTpShadow),
                     dr(kTpPtcl), dr(kTpView), dr(kTpFlAnim) + dr(kTpFlDl) + dr(kTpFlView), dr(kTpFlAnim), dr(kTpFlDl), dr(kTpFlView), dr(kTpDraw), dr(kTpOther),
                     busyU, sTp.cnt[kTcPlantSkip] / t, sTp.cnt[kTcPlantMoved] / t, port_plant_drawcull_mode() ? ", not drawn" : "",
                     sTp.cnt[kTcFlushOff] / td, sTp.cnt[kTcFlushDone] / td);
    if (sTp.turns[0] != 0) {  // (undrawn26) the undrawn turn by bucket, and by calc-anim list with GXV_TURN_PROF=2
        port_log_summary("turn-prof: an undrawn turn (%u of %u) %.2f = update %.2f + calc-anim lists %.2f + shadows %.2f + particles %.2f + view/rest %.2f + other %.2f"
                         " (undrawn-lean %d: see the undrawn-lean line)",
                         sTp.turns[0], nT, busyU, un(kTpUpdate), un(kTpCalc), un(kTpShadow), un(kTpPtcl), un(kTpView), un(kTpOther), port_undrawn_lean_mode());
        if (port_turn_prof_on() == 2) {
            static const char* const kCatName[kTpCat] = {"Environment", "MapObj", "NPC", "Ride", "Enemy", "Player", "PlayerDeco", "MapObjDeco", "Item",
                                                         "Layout", "LayoutDeco", "0x12", "MirrorMapObj", "-", "-", "rest"};
            char row[512];
            size_t at = 0;
            row[0] = 0;
            for (int k = 0; k < kTpCat; ++k) {
                if (kCatName[k][0] == '-') continue;
                const double u = sTp.catNs[0][k] / 1e6 / tu, dd = sTp.catNs[1][k] / 1e6 / td;
                if (u < 0.005 && dd < 0.005) continue;
                const int n = snprintf(row + at, sizeof row - at, " %s %.2f/%.2f", kCatName[k], u, dd);
                if (n > 0 && at + static_cast<size_t>(n) < sizeof row) at += static_cast<size_t>(n);
            }
            port_log_summary("turn-prof: calc-anim lists ms an undrawn/drawn turn:%s", row);
        }
    }
    {  // (strikers2audit) where a drawn turn's "other" goes: the marks in GameSystem::frameLoop
        auto sb = [&](int k) { return sTp.sub[k] / 1e6 / td; };
        double markSum = 0;
        for (int k = 0; k < kSubN; ++k) markSum += sb(k);
        port_log_summary("turn-prof: other of a drawn turn %.2f = frame-skip decision (pacer, port_frameskip_turn) %.2f + flush prepare (the early plant batch if not listed at the last turn's end) %.2f "
                         "+ capture (screen preserver) %.2f + end frame (GXSetDrawDone, GXFlush, the segment hand-over) %.2f + turn end (next batch listing, perf / anim / draw stats) %.2f; "
                         "marked %.2f, the rest of other (laps around the marks) %.2f",
                         dr(kTpOther), sb(kSubDecide), sb(kSubPrepare), sb(kSubCapture), sb(kSubEndFrame), sb(kSubTurnEnd), markSum, dr(kTpOther) - markSum);
    }
    if (port_turn_prof_on() == 2) {
        auto pt = [&](int k) { return sTp.part[k] / 1e6 / td; };
        port_log_summary("turn-prof: draw parts a drawn turn (GXV_TURN_PROF=2): draw %.2f = mirror %.2f + 3d %.2f (shadows %.2f, effects %.2f, "
                         "the rest of the 3d: model lists, sky, captures, image effects %.2f) + 2d layouts %.2f + the rest (begin / end render, "
                         "sequences, pointer, the view retire) %.2f",
                         dr(kTpDraw), pt(kDpMirror), pt(kDp3D), pt(kDpShadow), pt(kDpEffect), pt(kDp3D) - pt(kDpShadow) - pt(kDpEffect), pt(kDp2D),
                         dr(kTpDraw) - pt(kDpMirror) - pt(kDp3D) - pt(kDp2D));
        // (cpu2-26) the shadows' 1.0 ms a drawn turn, which every Honeyhive window shows and no log explains: where it goes
        port_log_summary("turn-prof: shadows a drawn turn %.2f = volumes %.2f (ShadowVolume lists) + silhouette and fill %.2f (executeDrawSilhouetteAndFillShadow) + alpha shadow %.2f + shadow surface %.2f "
                         "+ the rest (the clock reads, DrawType_0x28) %.2f",
                         pt(kDpShadow), pt(kDpShv), pt(kDpSil), pt(kDpSha), pt(kDpShs), pt(kDpShadow) - pt(kDpShv) - pt(kDpSil) - pt(kDpSha) - pt(kDpShs));
        // (imp45) the volume pass by drawer: how many were asked, how many drew, what one costs and writes (ShadowVolumeDrawer::draw)
        port_log_summary("turn-prof: shadow volumes a drawn turn: %.1f drawers asked, %.1f drew; a drawn volume %.1f us (clock reads included) and %.0f stream bytes; "
                         "the drawers that did not draw and the list walk: %.2f ms",
                         sShv.asked / td, sShv.drawn / td, sShv.drawn ? sShv.ns / 1000.0 / sShv.drawn : 0.0, sShv.drawn ? static_cast<double>(sShv.bytes) / sShv.drawn : 0.0,
                         pt(kDpShv) - sShv.ns / 1e6 / td);
        sShv = ShvCensus{};
    }
    const int64_t w0 = now;
    sTp = TurnProf{};
    sTp.winT0 = w0;
}
namespace {
int64_t sDpT0[kDpN];
}  // namespace
// imp45 (22 Nov): GXV_AUDIO_WINDOW_WIDE (default 0; acts with GXV_AUDIO_CPU1=1): a drawn turn's audio window opens before the flushes
// (GameSystem::frameLoop), not at draw(). Scheduling only: which core the game's audio thread runs on; the guards end the window as before.
extern "C" int port_audio_window_wide_on(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_AUDIO_WINDOW_WIDE"); return e && *e == '1' ? 1 : 0; }());
}
extern "C" void port_shv_note(int drawn, unsigned bytes, long long ns) {  // (imp45: ShadowVolumeDrawer::draw, with a draw-part profile on; game thread)
    ++sShv.asked;
    if (drawn) {
        ++sShv.drawn;
        sShv.bytes += bytes;
        if (ns > 0) sShv.ns += static_cast<uint64_t>(ns);
    }
}
// 1 / 2: a draw-part profile is on (GXV_TURN_PROF=2, or GXV_DRAW_PROF=2 in a logging build).
extern "C" int port_draw_part_on(void) { return port_turn_prof_on() == 2 || port_draw_prof_on() == 2 ? 1 : 0; }
extern "C" int port_sc_turn;                      // (GXV_STREAM_CENSUS, port/stream_census.h: below)
extern "C" void port_stream_census_src_begin(int id);
extern "C" void port_stream_census_src_end(void);
extern "C" void port_draw_part_begin(int part) {
    if (__builtin_expect(port_sc_turn != 0, 0)) port_stream_census_src_begin(0x400 + part);  // (a counted picture: the part is a source)
    if (part < 0 || part >= kDpN || !port_draw_part_on()) return;
    sDpT0[part] = port_host_time_ns();
}
extern "C" void port_draw_part_end(int part) {
    if (__builtin_expect(port_sc_turn != 0, 0)) port_stream_census_src_end();
    if (part < 0 || part >= kDpN || sDpT0[part] == 0) return;
    const int64_t ns = port_host_time_ns() - sDpT0[part];
    sDpT0[part] = 0;
    if (ns <= 0) return;
    if (port_turn_prof_on() == 2) sTp.part[part] += static_cast<uint64_t>(ns);
    if (part <= kDp2D) port_draw_prof_stage(20 + part, ns);  // (GXV_DRAW_PROF=2: shadows, effects, layouts)
}

// ---------------------------------------------------------------------------------------------------------------------------------
// undrawn26 (12 Nov): GXV_UNDRAWN_LEAN, the lean undrawn turn (port/undrawn_lean.h has the rules, tests/undrawn_lean_test.cpp tests them).
//   GXV_UNDRAWN_LEAN=0 off (default) / 1 on / 2 verify. 2 runs every turn as the original and only asks, for each product mode 1 would leave out (a
//   far-or-not model's joints, a shadow cast), whether the game's logic read it before the next picture made it ("undrawn-lean: VIOLATION ...").
//   GXV_UNDRAWN_LEAN_JOINTS (default 1) / GXV_UNDRAWN_LEAN_SHADOWS (default 1): the two products, each off with 0.
//   GXV_UNDRAWN_LEAN_SHADOW_AGE (default 120): turns a shadow controller must have been asking before its cast is left out.
//   GXV_UNDRAWN_LEAN_HASH=N (default 0, works in every mode): one "undrawn-lean: hash" line every N game turns of a stage, the hash of the gameplay
//   state (actor positions, velocities, rotations, nerves, sensor positions, the random seed), so that two console runs (0 and 1) with the same
//   scripted input and PETARI_GAME_CLOCK can be diffed with the log lines alone.
//   Census: "undrawn-lean:" every 300 turns while the mode or the hash is on.
#include "port/undrawn_lean.h"
extern "C" uint64_t port_undrawn_state_hash(unsigned* actors);  // (LiveActor.cpp)
extern "C" int port_undrawn_lean_flag = 0;
namespace {
struct UlState {
    port::undrawn::Tracker t;
    bool prevUndrawn;
    bool thisUndrawn;
    const char* reader;  // the actor whose movement is running (a name, for the VIOLATION line)
    uint32_t printed[port::undrawn::kProducts];
    bool inited;
};
UlState sUl;
void UlInit() {
    if (sUl.inited) return;
    sUl.inited = true;
    sUl.t.Init();
}
int UlSub(const char* name, int def) {
    const char* e = getenv(name);
    return e && *e ? (*e == '0' ? 0 : 1) : def;
}
}  // namespace
extern "C" int port_undrawn_lean_mode(void) {
    return PORT_HOT_KNOB(int, port::undrawn::ParseMode(getenv("GXV_UNDRAWN_LEAN")));
}
static int UlHashEvery() {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_UNDRAWN_LEAN_HASH"); const int v = e && *e ? atoi(e) : 0; return v < 0 ? 0 : v > 100000 ? 100000 : v; }());
}
static int UlJoints() { return PORT_HOT_KNOB(int, UlSub("GXV_UNDRAWN_LEAN_JOINTS", 1)); }
static int UlShadows() { return PORT_HOT_KNOB(int, UlSub("GXV_UNDRAWN_LEAN_SHADOWS", 1)); }
static unsigned UlShadowAge() {
    return PORT_HOT_KNOB(unsigned, [] { const char* e = getenv("GXV_UNDRAWN_LEAN_SHADOW_AGE"); const int v = e && *e ? atoi(e) : 120; return v < 0 ? 0u : static_cast<unsigned>(v); }());
}
extern "C" int port_undrawn_lean_joints(void) { return port_undrawn_lean_mode() != 0 && UlJoints() != 0; }
extern "C" int port_undrawn_lean_shadows(void) { return port_undrawn_lean_mode() != 0 && UlShadows() != 0; }
// cpu34g (14 Nov 2026): GXV_PAIRPOSE, GXV_CLIP_PAIR: work the drawn turn of a drawn + undrawn pair makes only to have it overwritten.
// A picture is drawn at the START of a drawn turn, from the state the turn before left. In a pair (Performance's re-projected 60, an even 30) that turn is the
// undrawn one, so what the drawn turn's own update and calc-anim make for the picture (the joints of a model, the clipping verdicts) is made again by the undrawn
// turn before any picture uses it. On "the drawn turn after an undrawn one" (the next is then an undrawn one, as long as the pattern holds):
//   GXV_PAIRPOSE=1  LiveActor::calcAnim leaves out the joints of a model that is not the player's, has no collision, no camera controller, no joint callback, no
//                   other every-n-th rule and is farther than GXV_PAIRPOSE_PDIST (800) from the player; the undrawn turn poses it (never two left out in a row).
//                   2: also models with a joint callback. The picture is the same; a logic reader of a joint sees it change every second turn.
//   GXV_CLIP_PAIR=1 ClippingActorHolder / ClippingGroupHolder do not judge; the undrawn turn judges with its camera (the one the picture is drawn with).
//   GXV_PAIR_LAYOUT=1 LayoutActor::calcAnim (the Layout / LayoutDecoration lists) lays nothing out: the undrawn turn's calc-anim lays the HUD out for the picture.
// Both default 0. Never in a lockstep session, never in a turn that follows a drawn one (a plain 60 or 30 loses nothing). One "pairpose:" line every 600 turns.
extern "C" {
int port_pair_skip_turn = 0;  // 1: this turn is the drawn turn of a pair and a switch is on (written by the game thread at the turn's start)
}
extern "C" int port_pairpose_mode(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_PAIRPOSE"); const int v = e && *e ? atoi(e) : 0; return v < 0 ? 0 : v > 2 ? 2 : v; }());
}
extern "C" int port_clip_pair_mode(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_CLIP_PAIR"); return e && *e == '1' ? 1 : 0; }());
}
extern "C" int port_pair_layout_mode(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_PAIR_LAYOUT"); return e && *e == '1' ? 1 : 0; }());
}
extern "C" float port_pairpose_pdist2(void) {
    return PORT_HOT_KNOB(float, [] {
        const char* e = getenv("GXV_PAIRPOSE_PDIST");
        float d = e && *e ? static_cast<float>(atof(e)) : 800.0f;
        if (!(d >= 300.0f)) d = 300.0f;
        return d * d;
    }());
}
namespace {
struct PairCensus {
    bool prevUndrawn = false;
    uint32_t turns = 0, pairTurns = 0;
    uint32_t c[12] = {};  // 10 layouts whose calc-anim was left out; 0 poses left out, 1 posed after one left out, 2 near the player, 3 joint callback, 4 collision / camera, 5 another rule / family, 6 pair-set tag,
                          // 7 clip turns not judged, 8 clip actors not judged, 9 list or gate (demo, no player)
} sPair;
void PairTurnBegin(int undrawn) {
    const int pose = port_pairpose_mode(), clip = port_clip_pair_mode(), lay = port_pair_layout_mode();
    if ((pose == 0 && clip == 0 && lay == 0) || port::lockstep::On()) {
        port_pair_skip_turn = 0;
        sPair.prevUndrawn = undrawn != 0;
        return;
    }
    port_pair_skip_turn = (!undrawn && sPair.prevUndrawn) ? 1 : 0;
    sPair.prevUndrawn = undrawn != 0;
    ++sPair.turns;
    if (port_pair_skip_turn) ++sPair.pairTurns;
    if (sPair.turns >= 600) {
        const double n = sPair.pairTurns ? static_cast<double>(sPair.pairTurns) : 1.0;
        const uint32_t* c = sPair.c;
        port_log_summary("pairpose: GXV_PAIRPOSE=%d GXV_CLIP_PAIR=%d, %u turns, %u drawn turns of a pair; a pair: joint sets left out %.1f (posed in the undrawn turn %.1f); "
                         "posed as always: near the player %.1f, joint callback %.1f, collision / camera %.1f, another rule or family %.1f, pair-set tag %.1f, list / demo %.1f; "
                         "clipping not judged in %u turns (%.1f actors a pair); GXV_PAIR_LAYOUT=%d: layouts not laid out %.1f a pair",
                         pose, clip, sPair.turns, sPair.pairTurns, c[0] / n, c[1] / n, c[2] / n, c[3] / n, c[4] / n, c[5] / n, c[6] / n, c[9] / n, c[7], c[8] / n, lay, c[10] / n);
        sPair.turns = 0;
        sPair.pairTurns = 0;
        memset(sPair.c, 0, sizeof sPair.c);
    }
}
}  // namespace
extern "C" void port_pair_count(int what, unsigned n) {
    if (what >= 0 && what < 12) sPair.c[what] += n;
}
// ratecut31 (17 Nov 2026, plan-perf31 B3): GXV_POSE_PHASE / GXV_POSE_PHASE_CENSUS (port/pose_phase.h has the rule's text; LiveActor.cpp, ActorTurnLod.cpp and
// PlantGroup.cpp read the flag). port_pose_phase_skip = 1: this turn is a skip turn (drawn, after an undrawn one: what its calc-anim makes is made again by the
// next turn before any picture) and the mode is on; the every-n-th cuts leave poses out on such turns only. Both default 0.
#include "port/pose_phase.h"
extern "C" {
int port_pose_phase_skip = 0;
int port_pose_phase_census_flag = 0;
}
extern "C" int port_pose_phase_mode(void) { return PORT_HOT_KNOB(int, port::posephase::ParseMode(getenv("GXV_POSE_PHASE"))); }
namespace {
int PpCensusOn() { return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_POSE_PHASE_CENSUS"); return e && *e == '1' ? 1 : 0; }()); }
port::posephase::Phase sPpPhase;
port::posephase::Census sPp;
bool sPpDrawn = true;
std::atomic<uint32_t> sPpWAll{0}, sPpWOld{0}, sPpWSkip{0}, sPpWBroken{0}, sPpWTurns{0};   // (q30cush31: the census's figures for the current "q30sep:" window)
uint64_t sPpPrevAll = 0, sPpPrevOld = 0;
void PosePhaseLog(int mode) {
    using namespace port::posephase;
    char buf[1024];
    int at = snprintf(buf, sizeof buf, "pose-phase: GXV_POSE_PHASE=%d, %u turns, %u pictures, %u skip turns, %u broken pairs; pose age at draw max %d; per cut, decisions "
                      "the pictures showed [age 0 / 1 / 2 / 3+]:", mode, sPp.turns, sPp.pictures, sPp.skipTurns, sPp.broken, sPp.MaxAgeSeen());
    for (int k = 0; k < kKinds && at > 0 && at < static_cast<int>(sizeof buf) - 96; ++k) {
        if (sPp.SeenAll(k) == 0 && sPp.all[k][0] + sPp.all[k][1] + sPp.all[k][2] + sPp.all[k][3] == 0) continue;
        at += snprintf(buf + at, sizeof buf - at, " %s [%llu %llu %llu %llu]", KindName(k), static_cast<unsigned long long>(sPp.seen[k][0]),
                       static_cast<unsigned long long>(sPp.seen[k][1]), static_cast<unsigned long long>(sPp.seen[k][2]), static_cast<unsigned long long>(sPp.seen[k][3]));
    }
    if (at > 0 && at < static_cast<int>(sizeof buf) - 96)
        snprintf(buf + at, sizeof buf - at, "; pair set left out in a drawn turn %llu; star-bit flush rule %llu (plantmove: a step owed, its pose exact)",
                 static_cast<unsigned long long>(sPp.pairB[1] + sPp.pairB[2] + sPp.pairB[3]),
                 static_cast<unsigned long long>(sPp.flushSeen[1] + sPp.flushSeen[2] + sPp.flushSeen[3]));
    port_log_summary("%s", buf);
}
void PosePhaseTurnBegin(int undrawn) {
    const int mode = port_pose_phase_mode();
    const int census = PpCensusOn();
    port_pose_phase_census_flag = census;
    if (mode == 0 && census == 0) {
        port_pose_phase_skip = 0;
        return;
    }
    const bool drawn = undrawn == 0;
    const bool skip = sPpPhase.Begin(drawn);
    port_pose_phase_skip = mode != 0 && skip ? 1 : 0;
    sPpDrawn = drawn;
    if (census == 0) return;
    const bool brk = sPpPhase.Broken(drawn);
    sPp.TurnBegin(drawn, skip, brk);
    {   // (q30cush31: the window figures for the "q30sep:" line, the display thread takes them: decisions a picture showed, those at pose age >= 1 (the plants' owed steps are apart), skip turns, broken pairs)
        uint64_t all = 0, old = 0;
        for (int k = 0; k < port::posephase::kKinds; ++k) if (k != 6) { all += sPp.SeenAll(k); old += sPp.SeenOld(k); }
        if (all < sPpPrevAll || old < sPpPrevOld) { sPpPrevAll = 0; sPpPrevOld = 0; }
        sPpWAll.fetch_add(static_cast<uint32_t>(all - sPpPrevAll), std::memory_order_relaxed);
        sPpWOld.fetch_add(static_cast<uint32_t>(old - sPpPrevOld), std::memory_order_relaxed);
        sPpPrevAll = all; sPpPrevOld = old;
        if (skip) sPpWSkip.fetch_add(1, std::memory_order_relaxed);
        if (brk) sPpWBroken.fetch_add(1, std::memory_order_relaxed);
        sPpWTurns.fetch_add(1, std::memory_order_relaxed);
    }
    if (sPp.turns >= 600) {
        PosePhaseLog(mode);
        sPp.ClearTotals();
        sPpPrevAll = 0; sPpPrevOld = 0;
    }
}
}  // namespace
// q30cush31: the pose-phase census's figures since the last call (the "q30sep:" line, once a window). Returns 0 when GXV_POSE_PHASE_CENSUS is off (nothing counted).
extern "C" int port_pose_phase_window_take(unsigned* seenAll, unsigned* seenOld, unsigned* skipTurns, unsigned* broken, unsigned* turns, int* mode) {
    *seenAll = sPpWAll.exchange(0, std::memory_order_relaxed);
    *seenOld = sPpWOld.exchange(0, std::memory_order_relaxed);
    *skipTurns = sPpWSkip.exchange(0, std::memory_order_relaxed);
    *broken = sPpWBroken.exchange(0, std::memory_order_relaxed);
    *turns = sPpWTurns.exchange(0, std::memory_order_relaxed);
    *mode = port_pose_phase_mode();
    return PpCensusOn();
}
extern "C" void port_pose_phase_note(int kind, int age, int tagged) { sPp.Note(kind, age, tagged != 0, sPpDrawn); }
extern "C" void port_pose_phase_note_flush(int age) { sPp.NoteFlush(age); }
// The turn's start (GameSystem::frameLoop, every turn): the flag the cheap guards read, the stage's turn count.
extern "C" void port_bowl_pose_turn_begin(int drawn) __attribute__((weak));  // (bowl31, GXV_BOWL_POSE: decomp/src/Game/LiveActor/LiveActor.cpp, port/bowl_pose.h; weak: the host tests that link perf.cpp alone)
extern "C" void port_pose_blendfix_turn_begin(void) __attribute__((weak));  // (blendfix31, GXV_POSE_BLENDFIX: LiveActor.cpp, the "pose-blendfix:" line; weak as above)
extern "C" void port_pose_ondraw_turn_begin(int drawn) __attribute__((weak));  // (ondraw31, GXV_POSE_ONDRAW: LiveActor.cpp, port/pose_ondraw.h, the "pose-ondraw:" line; weak as above)
// perf32 (GXV_CPULOD_MEMO=2, port/cpulod.h): the verify counts, every 1,800 turns while the mode is not 0.
static void CpulodMemoTurn() {
    if (__builtin_expect(port::cpulod::MemoMode() == 0, 1)) return;
    static uint32_t sTurns = 0;
    if (++sTurns % 1800u != 0) return;
    port_log_summary("cpulod-memo: GXV_CPULOD_MEMO=%d: since the start %u table lookups checked both ways, %u differ", port::cpulod::MemoMode(),
                     static_cast<unsigned>(port::cpulod::g_memoChecks.load(std::memory_order_relaxed)), static_cast<unsigned>(port::cpulod::g_memoBad.load(std::memory_order_relaxed)));
}
extern "C" void port_undrawn_turn_begin(int undrawn) {
    CpulodMemoTurn();
    PairTurnBegin(undrawn);  // (cpu34g: GXV_PAIRPOSE / GXV_CLIP_PAIR, every turn of every path)
    PosePhaseTurnBegin(undrawn);  // (ratecut31: GXV_POSE_PHASE / _CENSUS, every turn of every path)
    if (port_bowl_pose_turn_begin) port_bowl_pose_turn_begin(undrawn == 0 ? 1 : 0);  // (bowl31: the same phase, the "bowl-pose:" line)
    if (port_pose_ondraw_turn_begin) port_pose_ondraw_turn_begin(undrawn == 0 ? 1 : 0);  // (ondraw31: the same phase, the "pose-ondraw:" line; before blendfix31's line, whose census it reads)
    if (port_pose_blendfix_turn_begin) port_pose_blendfix_turn_begin();  // (blendfix31: the census line)
    const int mode = port_undrawn_lean_mode();
    const int hash = UlHashEvery();
    port_undrawn_lean_flag = mode;
    if (!mode && !hash) return;
    UlInit();
    ++sUl.t.turn;
    sUl.thisUndrawn = undrawn != 0;
}
// A joint rule is in force for this turn: this turn has no picture, or the one before had none (Quality's 30: the pair's other turn; a plain
// 60 never defers, so it pays no flush for nothing).
extern "C" int port_undrawn_lean_turn_window(void) { return sUl.thisUndrawn || sUl.prevUndrawn; }
extern "C" void port_undrawn_set_reader(const char* name) { sUl.reader = name; }
// May this actor's joints be left out of the turn? (mode 1: yes = 1; mode 2: counted and marked, 0.)
extern "C" int port_undrawn_joint_decide(const void* model, const char* owner, int category, int collision, int camera, int callback, int stale, int onFrame, int posed) {
    const int mode = port_undrawn_lean_mode();
    if (!mode || !model || port::lockstep::On()) return 0;   // (fmt32: which turns have no picture is this console's own matter: nothing is left out of a session's turn)
    UlInit();
    port::undrawn::JointFacts f;
    f.category = category;
    f.collision = collision != 0;
    f.camera = camera != 0;
    f.exported = false;
    f.jointCallback = callback != 0;
    f.stale = stale != 0;
    f.onFrameThread = onFrame != 0;
    f.posedOnce = posed != 0;
    return sUl.t.JointDecide(mode, model, owner, f) ? 1 : 0;
}
static void UlReadLine(unsigned product, const void* obj, const char* what, const char* owner) {
    const int mode = port_undrawn_lean_mode();
    if (!sUl.t.Read(product, mode, obj)) return;
    if (sUl.printed[product]++ < 24) {
        const char* o = owner ? owner : static_cast<const char*>(sUl.t.book[product].Owner(obj));
        port_log_summary("undrawn-lean: %s %s of %s read by %s (turn %u)", mode == 2 ? "VIOLATION" : "LATE READ", what, o ? o : "?",
                         sUl.reader ? sUl.reader : "(outside an actor's movement)", sUl.t.turn);
    }
}
// The game's logic asks for a joint matrix of a model (MR::getJointMtx and the copyJoint* family, JointUtil.cpp; the model's mtx buffer copies, ModelUtil.cpp):
// from now on it is never left out, and if it is left out right now, that is counted (mode 2: VIOLATION).
extern "C" void port_undrawn_joint_touch(const void* model) {
    if (!model || !port_undrawn_lean_mode()) return;  // (the mode, not the turn's flag: stage inits run before the first turn)
    UlInit();
    UlReadLine(port::undrawn::kJoint, model, "joint matrices", nullptr);
}
// May this shadow controller's cast be left out of the turn? (mode 1: 1; mode 2: counted and marked, 0.)
extern "C" int port_undrawn_shadow_decide(const void* ctrl, const char* owner, int logicRead, unsigned collMode, unsigned gravMode, int pb) {
    const int mode = port_undrawn_lean_mode();
    if (!mode || !ctrl || !UlShadows() || port::lockstep::On()) return 0;   // (fmt32: every cast is made on its own turn in a lockstep session)
    UlInit();
    port::undrawn::ShadowFacts f;
    f.logicRead = logicRead != 0;
    f.collMode = collMode;
    f.gravMode = gravMode;
    f.pbShadow = pb != 0;
    f.age = 0;
    f.minAge = UlShadowAge();
    return sUl.t.ShadowDecide(mode, ctrl, owner, f) ? 1 : 0;
}
// The game's logic reads a shadow controller's cast (ActorShadowUtil.cpp's accessors). Read once: never left out again (the accessors that set
// mPbLogicRead are not all of them).
extern "C" void port_undrawn_shadow_read(const void* ctrl, const char* owner) {
    if (!ctrl || !port_undrawn_lean_mode()) return;
    UlInit();
    UlReadLine(port::undrawn::kShadow, ctrl, "shadow cast", owner);
}
// The scene is being destroyed (LiveActor.cpp port_live_set_forget): its models and controllers are gone, a new object may sit at one of their addresses.
extern "C" void port_undrawn_scene_drop(void) {
    if (!sUl.inited) return;
    sUl.t.Reset();
}
// Before a picture's draw (GameSystem::frameLoop, after the flushes): everything left out has been made; the products are clean.
extern "C" void port_undrawn_flush(void) {
    if (!port_undrawn_lean_mode()) return;
    UlInit();
    sUl.t.Flush();
}
// The turn's end (GameSystem::frameLoop, drawn or not): the hash line, the census line.
extern "C" void port_undrawn_turn_end(int drawn) {
    const int mode = port_undrawn_lean_mode();
    const int every = UlHashEvery();
    if (!mode && !every) return;
    UlInit();
    auto& c = sUl.t.c;
    sUl.prevUndrawn = !drawn;
    ++c.turns;
    if (!drawn) ++c.undrawnTurns;
    if (every > 0 && sUl.t.turn % static_cast<uint32_t>(every) == 0) {
        // (crash33) not while the loading thread builds a scene: the walk reads every actor of the group, and that thread is making them
        // (half-built actors, tables being filled). A diagnostic never reads another thread's objects under construction.
        if (port_scene_loading_active()) {
            port_log_summary("undrawn-lean: hash turn %u not taken: a scene is loading (the loading thread is building the actors)", sUl.t.turn);
        } else {
            unsigned n = 0;
            const uint64_t h = port_undrawn_state_hash(&n);
            port_log_summary("undrawn-lean: hash turn %u %016llx actors %u", sUl.t.turn, static_cast<unsigned long long>(h), n);
        }
    }
    if (mode && c.turns >= 300) {
        const double t = c.turns;
        const char* verb = mode == 2 ? "that mode 1 would skip" : "skipped";
        port_log_summary("undrawn-lean: mode %d (joints %d shadows %d), %u turns (%u undrawn), a turn: joint sets %s %.1f, shadow casts %s %.1f; %u flushes made %.1f joint sets "
                         "and %.1f casts each; joints refused for collision %u camera %u exported %u callback %u other %u; logic reads of a left-out product: %s joints %u shadows %u",
                         mode, UlJoints(), UlShadows(), c.turns, c.undrawnTurns, verb, (mode == 2 ? c.wouldSkip[1] : c.skipped[1]) / t, verb,
                         (mode == 2 ? c.wouldSkip[0] : c.skipped[0]) / t, c.flushes, c.flushes ? c.flushed[1] / double(c.flushes) : 0.0,
                         c.flushes ? c.flushed[0] / double(c.flushes) : 0.0, c.refused[0], c.refused[1], c.refused[2], c.refused[3], c.refused[4],
                         mode == 2 ? "VIOLATIONS" : "late", mode == 2 ? c.violations[1] : c.lateReads[1], mode == 2 ? c.violations[0] : c.lateReads[0]);
        memset(&c, 0, sizeof c);
    }
}
// logic23 (8 Nov): GXV_LOGIC_PROF, the update / calc-anim sub-profile by class and by system (port/logic_prof.h has the hooks and
// the system list). Every build; lines in log.txt every 300 turns:
//   logic-prof: 300 turns (D drawn), GXV_LOGIC_PROF=L, a turn: update phase U ms = lists M + outside the lists O; calc phase C ms =
//               calc-anim lists A + shadows S + particles P + outside the lists Q; timer reads R a turn at T us (taken off: ~X ms)
//   logic-prof: categories a turn: move <cat> ms (objects) ... | calc <cat> ms ...
//   logic-prof: update #k cat=<category> name=<model or NameObj name> vt=0x... ms/turn=.. calls/turn=.. us/call=.. (raw ms ..)
//   logic-prof: calc #k ...
//   logic-prof: systems a turn (inclusive): <system> ms (calls; in the player's lists ms) ...
//   logic-prof: movement phases a turn (GXV_LOGIC_PROF=2): ...
//   logic-prof: counts a turn: gravities visited .., areas tested .. (answered through the grid ..), collision parts looked at ..
//               (searched ..), gravity walks through the grid ..
// All ms are "corrected": the reads made inside a row and one for its own pair (half of each of its two reads falls inside), at the cost
// of one read measured at each window's start (256 reads in a row), are taken off.
#include "port/logic_prof.h"
extern "C" int port_anim_lod_raw_category(void);
int port_logic_prof_flag = 0;
int port_logic_prof_coarse = 0;
namespace {
int LogicProfLevel() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_LOGIC_PROF");
        const int v = e && *e ? atoi(e) : 0;
        return v < 0 ? 0 : v > 3 ? 3 : v;
    }());
}
int LogicProfTop() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_LOGIC_PROF_TOP");
        const int v = e && *e ? atoi(e) : (LogicProfLevel() == 3 ? 20 : 16);  // (update25: 20 rows with =3)
        return v < 1 ? 1 : v > 40 ? 40 : v;
    }());
}
// (game24) GXV_LOGIC_PROF=3: one turn in this many is a "deep" turn (everything of =2 on); the others run the coarse timers only.
int LogicProfSample() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_LOGIC_PROF_SAMPLE");
        const int v = e && *e ? atoi(e) : 4;
        return v < 1 ? 1 : v > 64 ? 64 : v;
    }());
}
struct LpClass {
    const void* vt;  // null: free
    int16_t domain;  // 0 calc-anim, 1 movement
    int16_t category;
    bool modelNamed;
    char name[31];
    char obj[23];  // (game24: the NameObj name of its first object)
    uint32_t calls;
    uint64_t ns, reads;
    // (update25, =3 / =2) where this class's time goes: LiveActor::movement's phases (PORT_LS_MV_FIRST..), time and inner timer reads
    uint64_t phNs[12], phReads[12];
};
constexpr unsigned kLpClasses = 1024;
LpClass sLpClass[kLpClasses];
bool sLpOverflow = false;
struct LpSys {
    uint64_t ns, nsPlayer, reads, readsPlayer;
    uint32_t calls;
    int depth;
    int64_t t0;
    uint64_t r0;
};
LpSys sLpSys[PORT_LS_N];
uint64_t sLpCount[PORT_LC_N];
uint64_t sLpCatNs[2][64], sLpCatReads[2][64];
uint32_t sLpCatCalls[2][64];
// (game24, =3) every turn's category lists timed whole: [deep turn][domain][category]; ns, calls, inner timer reads (+ the own pair)
uint64_t sLpCoNs[2][2][64], sLpCoReads[2][2][64];
uint32_t sLpCoCalls[2][2][64];
int sLpCoDepth = 0, sLpCoRaw = -1;
int64_t sLpCoT0 = 0;
uint64_t sLpCoR0 = 0;
uint32_t sLpDeepTurns = 0, sLpDeepDrawn = 0;
uint32_t sLpRng = 0x2545F491u;
uint64_t sLpReads = 0;  // every timer read of this profiler (the game thread's)
int sLpObjDepth = 0;
LpClass* sLpCur = nullptr;
int sLpCurRaw = -1;
int64_t sLpObjT0 = 0;
uint64_t sLpObjR0 = 0;
uint32_t sLpTurns = 0, sLpDrawn = 0;
double sLpReadNs = 0.0;  // the cost of one read, measured at the window's start
const char* const kCalcNamesLp[] = {"ClippedMapParts", "Planet", "CollisionMapObj", "CollisionEnemy", "Environment", "MapObj", "NPC", "Ride",
    "Enemy", "Player", "PlayerDecoration", "MapObjDecoration", "MirrorMapObj", "Layout", "LayoutDecoration", "MovieSubtitles", "Item", "0x11",
    "0x12", "AnimParticle", "AnimParticleIgnorePause", "0x15"};
inline int64_t LpNow() {
    ++sLpReads;
    return port_host_time_ns();
}
inline bool LpPlayerCategory(int raw) {
    return raw == (0x100 | 0x25) || raw == 9;  // (movement Player, calc-anim Player)
}
void LpCalibrate() {
    const int64_t t0 = port_host_time_ns();
    int64_t t = t0;
    for (int i = 0; i < 256; ++i) t = port_host_time_ns();
    sLpReadNs = t > t0 ? static_cast< double >(t - t0) / 256.0 : 0.0;
}
const char* LpCatName(int domain, int category) {
    if (domain == 1) return category >= 0 && category < 0x2E ? kMoveTypeNames[category] : "?";
    return category >= 0 && category < static_cast< int >(sizeof(kCalcNamesLp) / sizeof(*kCalcNamesLp)) ? kCalcNamesLp[category] : "?";
}
void LpPrintable(char* out, size_t size, const char* in) {  // (NameObj names are often Shift-JIS: bytes outside ASCII as ~xx)
    size_t o = 0;
    for (const unsigned char* c = reinterpret_cast< const unsigned char* >(in ? in : "(null)"); *c && o + 4 < size; ++c) {
        if (*c > 0x20 && *c < 0x7F) {
            out[o++] = static_cast< char >(*c);
        } else {
            o += static_cast< size_t >(snprintf(out + o, size - o, "~%02x", *c));
        }
    }
    out[o < size ? o : size - 1] = 0;
}
}  // namespace
extern "C" void port_logic_prof_obj_begin(const void* pObj, const char* pName) {
    if (sLpObjDepth++ != 0) return;
    sLpCur = nullptr;
    const int raw = port_anim_lod_raw_category();
    if (raw < 0 || pObj == nullptr || !port_on_frame_thread()) return;
    const void* vt = *static_cast< const void* const* >(pObj);
    const int domain = raw >= 0x100 ? 1 : 0;
    uint32_t h = static_cast< uint32_t >(reinterpret_cast< uintptr_t >(vt)) * 0x9E3779B1u;
    h ^= static_cast< uint32_t >(domain) * 0x85EBCA77u;
    for (unsigned probe = 0; probe < 64; ++probe) {
        LpClass& r = sLpClass[(h + probe) & (kLpClasses - 1)];
        if (r.vt == nullptr) {
            r.vt = vt;
            r.domain = static_cast< int16_t >(domain);
            r.category = static_cast< int16_t >(raw & 0xFF);
            r.modelNamed = false;
            snprintf(r.name, sizeof r.name, "%s", pName ? pName : "(null)");
            snprintf(r.obj, sizeof r.obj, "%s", pName ? pName : "(null)");
        } else if (r.vt != vt || r.domain != domain) {
            continue;
        }
        sLpCur = &r;
        sLpCurRaw = raw;
        sLpObjR0 = sLpReads;
        sLpObjT0 = LpNow();
        return;
    }
    sLpOverflow = true;
}
extern "C" void port_logic_prof_obj_end(void) {
    if (sLpObjDepth <= 0 || --sLpObjDepth != 0) return;
    LpClass* r = sLpCur;
    if (r == nullptr) return;
    sLpCur = nullptr;
    const int64_t ns = LpNow() - sLpObjT0;
    const uint64_t reads = sLpReads - sLpObjR0 - 1;  // (those made inside it, plus one for its own pair: half of each falls inside)
    ++r->calls;
    r->ns += ns > 0 ? static_cast< uint64_t >(ns) : 0;
    r->reads += reads;
    const int d = r->domain, c = sLpCurRaw & 63;
    sLpCatNs[d][c] += ns > 0 ? static_cast< uint64_t >(ns) : 0;
    sLpCatReads[d][c] += reads;
    ++sLpCatCalls[d][c];
}
extern "C" int port_logic_prof_obj_wants_name(void) {
    return sLpCur != nullptr && !sLpCur->modelNamed ? 1 : 0;
}
extern "C" void port_logic_prof_obj_name(const char* pModelName) {
    if (sLpCur == nullptr || sLpCur->modelNamed || pModelName == nullptr) return;
    snprintf(sLpCur->name, sizeof sLpCur->name, "%s", pModelName);
    sLpCur->modelNamed = true;
}
extern "C" void port_logic_prof_sys_begin(int id) {
    if (id < 0 || id >= PORT_LS_N || !port_on_frame_thread()) return;
    LpSys& s = sLpSys[id];
    if (s.depth++ != 0) return;
    s.r0 = sLpReads;
    s.t0 = LpNow();
}
extern "C" void port_logic_prof_sys_end(int id) {
    if (id < 0 || id >= PORT_LS_N || !port_on_frame_thread()) return;
    LpSys& s = sLpSys[id];
    if (s.depth <= 0 || --s.depth != 0) return;
    const int64_t ns = LpNow() - s.t0;
    const uint64_t v = ns > 0 ? static_cast< uint64_t >(ns) : 0;
    s.ns += v;
    s.reads += sLpReads - s.r0 - 1;  // (as for the objects: the inner reads, plus one for its own pair)
    ++s.calls;
    if (sLpCur != nullptr && LpPlayerCategory(sLpCurRaw)) {
        s.nsPlayer += v;
        s.readsPlayer += sLpReads - s.r0 - 1;
    }
    if (sLpCur != nullptr && id >= PORT_LS_MV_FIRST && id <= PORT_LS_MV_SHADOWREQ) {  // (update25: the phase under the class that ran it)
        sLpCur->phNs[id - PORT_LS_MV_FIRST] += v;
        sLpCur->phReads[id - PORT_LS_MV_FIRST] += sLpReads - s.r0 - 1;
    }
}
extern "C" void port_logic_prof_count(int id, unsigned n) {
    if (id >= 0 && id < PORT_LC_N) sLpCount[id] += n;
}
// (=2) Sizes a cross-model BCK cache: how many joint evaluations of a turn repeat an (animation, frame, joint) evaluated earlier in the
// same turn (models sharing an animation at the same frame: coins, star bits, flowers). A table of the turn's keys, direct-mapped.
namespace {
struct LpBckKey {
    const void* anm;
    uint32_t frame;
    uint32_t joint;
    uint32_t turn;
};
LpBckKey sLpBck[4096];
uint32_t sLpBckTurn = 1;
}  // namespace
extern "C" void port_logic_prof_bck(const void* pAnm, float frame, unsigned jointNo) {
    if (!port_on_frame_thread()) return;  // (par-anim's workers evaluate BCKs too)
    uint32_t fb;
    memcpy(&fb, &frame, sizeof fb);
    ++sLpCount[PORT_LC_BCK_CALLS];
    uint32_t h = static_cast< uint32_t >(reinterpret_cast< uintptr_t >(pAnm)) * 0x9E3779B1u;
    h = (h ^ fb) * 0x85EBCA77u;
    h = (h ^ jointNo) * 0xC2B2AE3Du;
    LpBckKey& k = sLpBck[h >> 20];
    if (k.turn == sLpBckTurn && k.anm == pAnm && k.frame == fb && k.joint == jointNo) {
        ++sLpCount[PORT_LC_BCK_REPEATS];
        return;
    }
    k.anm = pAnm;
    k.frame = fb;
    k.joint = jointNo;
    k.turn = sLpBckTurn;
}
// (update25, =3 on a sampled turn) the drawn turn's flushes, per owner class: kind 0 the material lists, kind 1 the far models' joints.
// Game thread only; the serial flush paths (GXV_PAR_DL / GXV_PAR_FLUSH workers make theirs elsewhere: those rows stay empty).
namespace {
struct LpFlush {
    const void* vt;  // null: free
    int16_t kind;
    char name[31];
    uint32_t calls, leftOut;
    uint64_t ns, reads;
};
constexpr unsigned kLpFlushes = 256;
LpFlush sLpFlush[kLpFlushes];
LpFlush* sLpFlCur = nullptr;
int sLpFlDepth = 0;
int64_t sLpFlT0 = 0;
uint64_t sLpFlR0 = 0;
}  // namespace
extern "C" void port_logic_prof_flush_begin(int kind, const void* pObj, const char* pName) {
    if (sLpFlDepth++ != 0) return;
    sLpFlCur = nullptr;
    if (pObj == nullptr || !port_on_frame_thread() || kind < 0 || kind > 1) return;
    const void* vt = *static_cast< const void* const* >(pObj);
    uint32_t h = static_cast< uint32_t >(reinterpret_cast< uintptr_t >(vt)) * 0x9E3779B1u;
    h ^= static_cast< uint32_t >(kind) * 0x85EBCA77u;
    for (unsigned probe = 0; probe < 32; ++probe) {
        LpFlush& r = sLpFlush[(h + probe) & (kLpFlushes - 1)];
        if (r.vt == nullptr) {
            r.vt = vt;
            r.kind = static_cast< int16_t >(kind);
            snprintf(r.name, sizeof r.name, "%s", pName ? pName : "(null)");
        } else if (r.vt != vt || r.kind != kind) {
            continue;
        }
        sLpFlCur = &r;
        sLpFlR0 = sLpReads;
        sLpFlT0 = LpNow();
        return;
    }
}
extern "C" void port_logic_prof_flush_end(int outcome) {
    if (sLpFlDepth <= 0 || --sLpFlDepth != 0) return;
    LpFlush* r = sLpFlCur;
    if (r == nullptr) return;
    sLpFlCur = nullptr;
    const int64_t ns = LpNow() - sLpFlT0;
    ++r->calls;
    if (outcome != 0) ++r->leftOut;
    r->ns += ns > 0 ? static_cast< uint64_t >(ns) : 0;
    r->reads += sLpReads - sLpFlR0 - 1;
}
// (game24, =3) a movement (0x100 | type) or calc-anim (type) category list, timed whole on every turn
extern "C" void port_logic_prof_cat_begin(int raw) {
    if (!port_on_frame_thread() || sLpCoDepth++ != 0) return;
    sLpCoRaw = raw;
    sLpCoR0 = sLpReads;
    sLpCoT0 = LpNow();
}
extern "C" void port_logic_prof_cat_end(void) {
    if (sLpCoDepth <= 0 || --sLpCoDepth != 0) return;
    const int64_t ns = LpNow() - sLpCoT0;
    const uint64_t reads = sLpReads - sLpCoR0 - 1;  // (as for the objects: the inner reads, plus one for its own pair)
    const int deep = port_logic_prof_flag != 0 ? 1 : 0, d = sLpCoRaw >= 0x100 ? 1 : 0, c = sLpCoRaw & 63;
    sLpCoNs[deep][d][c] += ns > 0 ? static_cast< uint64_t >(ns) : 0;
    sLpCoReads[deep][d][c] += reads;
    ++sLpCoCalls[deep][d][c];
}
namespace {
// The =3 lines of one window (every turn's coarse numbers, then the deep turns' notes). `plain` / `deep`: the turns of each kind.
void LpPrintSampled(uint32_t plain, uint32_t deep, uint32_t drawn, int sample, double rc) {
    const double turns = plain + deep;
    // ms a turn of a coarse category / system, over all the turns that ran it (reads taken off)
    auto catMs = [&](int d, int c, int kind) {  // kind 0 unsampled turns, 1 sampled turns, 2 all
        double ns = 0, reads = 0, n = 0;
        for (int k = 0; k < 2; ++k) {
            if (kind != 2 && kind != k) continue;
            ns += static_cast< double >(sLpCoNs[k][d][c]);
            reads += static_cast< double >(sLpCoReads[k][d][c]);
            n += k ? deep : plain;
        }
        const double v = n > 0 ? (ns - reads * rc) / 1e6 / n : 0.0;
        return v > 0 ? v : 0.0;
    };
    auto sysMs = [&](int id) {
        const LpSys& s = sLpSys[id];
        const double v = (static_cast< double >(s.ns) - static_cast< double >(s.reads) * rc) / 1e6 / turns;
        return v > 0 ? v : 0.0;
    };
    double listMs[2] = {0, 0}, listPlain[2] = {0, 0}, listDeep[2] = {0, 0};
    for (int d = 0; d < 2; ++d) {
        for (int c = 0; c < 64; ++c) {
            listMs[d] += catMs(d, c, 2);
            listPlain[d] += catMs(d, c, 0);
            listDeep[d] += catMs(d, c, 1);
        }
    }
    const double upMs = sysMs(PORT_LS_PH_UPDATE), cpMs = sysMs(PORT_LS_PH_CALC);
    const double shadowMs = catMs(1, 0x2D, 2), ptclMs = catMs(0, 0x13, 2) + catMs(0, 0x14, 2);
    const double mvLists = listMs[1], mvWhole = sysMs(PORT_LS_U_MOVELISTS);
    const double mlCtrl = sysMs(PORT_LS_U_ML_CTRL), mlReq = sysMs(PORT_LS_U_ML_REQ), mlCam = sysMs(PORT_LS_U_ML_CAMMTX);
    const double scene = sysMs(PORT_LS_U_SCENE), nerve = sysMs(PORT_LS_U_NERVE), audio = sysMs(PORT_LS_U_AUDIO), seq = sysMs(PORT_LS_U_SEQDIR);
    const double wipe = sysMs(PORT_LS_U_WIPE), reset = sysMs(PORT_LS_U_RESET), chg = sysMs(PORT_LS_U_SCENECHG), oh = sysMs(PORT_LS_U_OBJHOLDER);
    const double home = sysMs(PORT_LS_U_HOME);
    port_log_summary("logic-prof3: %.0f turns (%u drawn; %u sampled, 1 in %d), timer read %.2f us, %.0f reads a turn; a turn: update phase %.3f ms = "
                     "update's own parts [reset process %.3f, scene change check %.3f, ObjHolder::update %.3f (audio rhythm %.3f, pads %.3f, async %.3f, "
                     "star pointer %.3f), home/error/dimming %.3f] + updateNerve %.3f [= audio system %.3f + sequence director %.3f + system wipe %.3f + "
                     "scene update %.3f (= movement lists %.3f + the rest of the scene's update %.3f) + rest %.3f]; movement lists %.3f = category lists "
                     "%.3f + scene controller %.3f + requirements %.3f + camera matrix %.3f + rest %.3f; calc phase %.3f ms = calc-anim lists %.3f + "
                     "shadows %.3f + particles %.3f (list; its join below) + par-late start %.3f + particle join %.3f + par-anim dispatch %.3f + rest (the "
                     "system wipe's calc, the loadViewMtx, call overhead) %.3f",
                     turns, drawn, deep, sample, rc / 1000.0, sLpReads / turns, upMs, reset, chg, oh, sysMs(PORT_LS_U_OH_RHYTHM),
                     sysMs(PORT_LS_U_OH_WPAD), sysMs(PORT_LS_U_OH_ASYNC), sysMs(PORT_LS_U_OH_STARPTR), home, nerve, audio, seq, wipe, scene, mvWhole,
                     scene - mvWhole, nerve - audio - seq - wipe - scene, mvWhole, mvLists - shadowMs, mlCtrl, mlReq, mlCam,
                     mvWhole - mvLists - mlCtrl - mlReq - mlCam + shadowMs, cpMs, listMs[0] - ptclMs, shadowMs, ptclMs,
                     sysMs(PORT_LS_C_LATE), sysMs(PORT_LS_C_PTCLFIN), sysMs(PORT_LS_C_ANIMDISP),
                     cpMs - listMs[0] - shadowMs - sysMs(PORT_LS_C_LATE) - sysMs(PORT_LS_C_PTCLFIN) - sysMs(PORT_LS_C_ANIMDISP));
    port_log_summary("logic-prof3: update unexplained (update phase minus the parts above) %.3f ms; movement lists include shadows %.3f (the shadow "
                     "controllers' list); the lists of the sampled turns cost %.3f ms (corrected) against %.3f on the others: sampling inflation "
                     "x%.2f for the movement lists, x%.2f for the calc-anim lists",
                     upMs - (reset + chg + oh + home + nerve), shadowMs, listDeep[1], listPlain[1],
                     listPlain[1] > 0.001 ? listDeep[1] / listPlain[1] : 0.0, listPlain[0] > 0.001 ? listDeep[0] / listPlain[0] : 0.0);
    // every turn's category tables, costliest first (exact: whole-list timers, two reads a list)
    for (int d = 1; d >= 0; --d) {
        char line[760];
        int o = snprintf(line, sizeof line, "logic-prof3: %s categories a turn, every turn (ms, objects): ", d ? "movement" : "calc-anim");
        bool shown[64] = {};
        for (int rank = 0; rank < 16 && o < static_cast< int >(sizeof line) - 64; ++rank) {
            int best = -1;
            double bestMs = 0.004;
            for (int c = 0; c < 64; ++c) {
                const double v = catMs(d, c, 2);
                if (!shown[c] && v > bestMs) { best = c; bestMs = v; }
            }
            if (best < 0) break;
            shown[best] = true;
            double calls = 0;
            for (int k = 0; k < 2; ++k) calls += sLpCoCalls[k][d][best];
            o += snprintf(line + o, sizeof line - o, " %s %.3f (%.1f)", LpCatName(d, best), bestMs, calls / turns);
        }
        port_log_summary("%s", line);
    }
    // the sampled turns: the class table (corrected, and scaled to the real list cost of its category), systems, phases, counts
    const double dt = deep > 0 ? deep : 1;
    auto dms = [&](uint64_t ns, uint64_t reads) {
        const double v = (static_cast< double >(ns) - static_cast< double >(reads) * rc) / 1e6 / dt;
        return v > 0 ? v : 0.0;
    };
    // per category: the real cost of its list (unsampled turns) over the cost the sampled turns measured as the sum of its objects
    double scale[2][64];
    for (int d = 0; d < 2; ++d) {
        for (int c = 0; c < 64; ++c) {
            const double objs = dms(sLpCatNs[d][c], sLpCatReads[d][c]);
            const double real = catMs(d, c, 0);
            double f = objs > 0.001 && real > 0.0 ? real / objs : 1.0;
            scale[d][c] = f < 0.2 ? 0.2 : f > 1.6 ? 1.6 : f;
        }
    }
    const int top = LogicProfTop();
    for (int d = 1; d >= 0; --d) {
        const LpClass* shown[40] = {};
        for (int rank = 0; rank < top; ++rank) {
            const LpClass* best = nullptr;
            double bestMs = -1.0;
            for (const LpClass& r : sLpClass) {
                if (r.vt == nullptr || r.domain != d || r.calls == 0) continue;
                bool used = false;
                for (int k = 0; k < rank; ++k) used |= shown[k] == &r;
                const double v = dms(r.ns, r.reads) * scale[d][r.category & 63];
                if (!used && v > bestMs) { best = &r; bestMs = v; }
            }
            if (best == nullptr || bestMs < 0.004) break;
            shown[rank] = best;
            char name[96], obj[96];
            LpPrintable(name, sizeof name, best->name);
            LpPrintable(obj, sizeof obj, best->obj);
            const double raw = dms(best->ns, best->reads);
            port_log_summary("logic-prof3: %s class #%d cat=%s model=%s obj=%s vt=%p est ms/turn=%.3f (measured %.3f x %.2f), objects/turn=%.1f, us/object=%.1f",
                             d ? "movement" : "calc-anim", rank + 1, LpCatName(d, best->category), name, obj, best->vt, bestMs, raw,
                             scale[d][best->category & 63], best->calls / dt, best->calls > 0 ? bestMs * 1000.0 * dt / best->calls : 0.0);
            if (d == 1 && rank < 12) {
                // (update25) where this class's movement goes, by LiveActor::movement phase (sampled turns, corrected, unscaled; NameObj
                // classes that are not LiveActors, and Mario's own movement(), show none)
                static const char* const kPh[12] = {"anim-clock", "gravity", "sensor-messages", "nerve", "control", "binder", "effects",
                    "camera-ctrl", "light", "sensor-positions", "sound", "shadow-request"};
                char line[400];
                int o = snprintf(line, sizeof line, "logic-prof3: class #%d movement phases ms/turn (sampled turns, measured %.3f):", rank + 1, raw);
                double sum = 0.0;
                for (int k = 0; k < 12 && o < static_cast< int >(sizeof line) - 40; ++k) {
                    const double v = dms(best->phNs[k], best->phReads[k]);
                    sum += v;
                    if (v >= 0.002) o += snprintf(line + o, sizeof line - o, " %s %.3f", kPh[k], v);
                }
                snprintf(line + o, sizeof line - o, " (phases %.3f, the rest %.3f)", sum, raw > sum ? raw - sum : 0.0);
                port_log_summary("%s", line);
            }
        }
    }
    // (update25) the drawn turn's flushes per owner class: the material lists (dl) and the far models' joints (anim), on the sampled turns
    // that drew; ms and models per DRAWN sampled turn (the flush runs once before each picture)
    {
        const double df = sLpDeepDrawn > 0 ? sLpDeepDrawn : 1;
        for (int kind = 0; kind < 2; ++kind) {
            double totMs = 0, totCalls = 0, totOut = 0;
            for (const LpFlush& r : sLpFlush) {
                if (r.vt == nullptr || r.kind != kind || r.calls == 0) continue;
                totMs += (static_cast< double >(r.ns) - static_cast< double >(r.reads) * rc) / 1e6 / df;
                totCalls += r.calls / df;
                totOut += r.leftOut / df;
            }
            if (totCalls <= 0.0) continue;
            port_log_summary("logic-prof3: flush-%s per drawn turn (%u sampled drawn turns): %.3f ms timed over %.1f models, %.1f of them left out",
                             kind ? "anim (far models' joints + view)" : "dl (material lists)", sLpDeepDrawn, totMs, totCalls, totOut);
            const LpFlush* shown[40] = {};
            for (int rank = 0; rank < top; ++rank) {
                const LpFlush* best = nullptr;
                double bestMs = -1.0;
                for (const LpFlush& r : sLpFlush) {
                    if (r.vt == nullptr || r.kind != kind || r.calls == 0) continue;
                    bool used = false;
                    for (int k = 0; k < rank; ++k) used |= shown[k] == &r;
                    const double v = (static_cast< double >(r.ns) - static_cast< double >(r.reads) * rc) / 1e6 / df;
                    if (!used && v > bestMs) { best = &r; bestMs = v; }
                }
                if (best == nullptr || bestMs < 0.003) break;
                shown[rank] = best;
                char name[96];
                LpPrintable(name, sizeof name, best->name);
                port_log_summary("logic-prof3: flush-%s class #%d obj=%s vt=%p ms/drawn turn=%.3f models/drawn turn=%.1f (left out %.1f) us/model=%.1f",
                                 kind ? "anim" : "dl", rank + 1, name, best->vt, bestMs, best->calls / df, best->leftOut / df,
                                 best->calls > 0 ? bestMs * 1000.0 * df / best->calls : 0.0);
            }
        }
    }
    static const char* const kSysNames[PORT_LS_N] = {"gravity", "gravity-walk", "area-find", "area-manager", "collision", "binder", "actor-calcanim",
        "anim-clock", "gravity", "sensor-messages", "nerve", "control", "binder", "effects", "camera-ctrl", "light", "sensor-positions", "sound",
        "shadow-request", "update-phase", "calc-phase"};
    for (int part = 0; part < 2; ++part) {
        char line[900];
        int o = snprintf(line, sizeof line, part ? "logic-prof3: movement phases a turn (sampled turns; ms, calls; in the player's lists ms):"
                                                  : "logic-prof3: systems a turn (sampled turns; inclusive; ms, calls; in the player's lists ms):");
        const int lo = part ? PORT_LS_MV_FIRST : 0, hi = part ? PORT_LS_MV_SHADOWREQ + 1 : PORT_LS_MV_FIRST;
        for (int id = lo; id < hi && o < static_cast< int >(sizeof line) - 64; ++id) {
            const LpSys& s = sLpSys[id];
            // (these systems ran on the sampled turns only: their totals are over `deep` turns, not all of them)
            o += snprintf(line + o, sizeof line - o, " %s %.3f (%.1f; %.3f)", kSysNames[id], dms(s.ns, s.reads), s.calls / dt, dms(s.nsPlayer, s.readsPlayer));
        }
        port_log_summary("%s", line);
    }
    port_log_summary("logic-prof3: sound names asked for a turn (sampled turns): %.1f, the scan of the name table looks at %.0f entries a turn (%.0f a name)",
                     sLpCount[PORT_LC_SND_CALLS] / dt, sLpCount[PORT_LC_SND_SCAN] / dt,
                     sLpCount[PORT_LC_SND_CALLS] > 0 ? static_cast< double >(sLpCount[PORT_LC_SND_SCAN]) / static_cast< double >(sLpCount[PORT_LC_SND_CALLS]) : 0.0);
    port_log_summary("logic-prof3: counts a turn (sampled turns): gravities visited %.1f (walks through the grid %.1f), areas tested %.1f (lookups answered "
                     "through the grid %.1f), collision parts looked at %.1f (searched %.1f); BCK joint evaluations %.1f (repeating one of the same turn %.1f)",
                     sLpCount[PORT_LC_GRAV_VISITS] / dt, sLpCount[PORT_LC_GRAV_GRID] / dt, sLpCount[PORT_LC_AREA_TESTS] / dt,
                     sLpCount[PORT_LC_AREA_GRID] / dt, sLpCount[PORT_LC_COLL_PARTS] / dt, sLpCount[PORT_LC_COLL_SEARCH] / dt,
                     sLpCount[PORT_LC_BCK_CALLS] / dt, sLpCount[PORT_LC_BCK_REPEATS] / dt);
}
}  // namespace
extern "C" void port_logic_prof_turn(int drawn) {
    const int level = LogicProfLevel();
    const bool wasDeep = level < 3 || port_logic_prof_flag != 0;  // (the turn that has just ended)
    if (level == 3) {
        // the next turn: coarse timers always, everything else on one turn in `sample` (a random pick, so that nothing the game does every
        // few turns lines up with it)
        sLpRng ^= sLpRng << 13;
        sLpRng ^= sLpRng >> 17;
        sLpRng ^= sLpRng << 5;
        __atomic_store_n(&port_logic_prof_coarse, 2, __ATOMIC_RELAXED);
        __atomic_store_n(&port_logic_prof_flag, sLpRng % static_cast< uint32_t >(LogicProfSample()) == 0 ? 2 : 0, __ATOMIC_RELAXED);
    } else {
        __atomic_store_n(&port_logic_prof_coarse, level > 0 ? 1 : 0, __ATOMIC_RELAXED);
        __atomic_store_n(&port_logic_prof_flag, level, __ATOMIC_RELAXED);
    }
    if (level == 0) return;
    ++sLpBckTurn;
    if (sLpTurns == 0 && sLpReadNs == 0.0) LpCalibrate();
    ++sLpTurns;
    if (drawn) ++sLpDrawn;
    if (wasDeep) {
        ++sLpDeepTurns;
        if (drawn) ++sLpDeepDrawn;
    }
    if (sLpTurns < 300) return;
    if (level == 3) {
        LpPrintSampled(sLpTurns - sLpDeepTurns, sLpDeepTurns, sLpDrawn, LogicProfSample(), sLpReadNs);
        goto reset;
    }
    {
    const double turns = sLpTurns, rc = sLpReadNs;
    // corrected ms a turn: raw minus the reads made in the row (each ~one read's cost)
    auto ms = [&](uint64_t ns, uint64_t reads) {
        const double v = (static_cast< double >(ns) - static_cast< double >(reads) * rc) / 1e6 / turns;
        return v > 0 ? v : 0.0;
    };
    double lists[2] = {0, 0}, raw[2] = {0, 0};
    for (int d = 0; d < 2; ++d) {
        for (int c = 0; c < 64; ++c) {
            lists[d] += ms(sLpCatNs[d][c], sLpCatReads[d][c]);
            raw[d] += sLpCatNs[d][c] / 1e6 / turns;
        }
    }
    const LpSys& up = sLpSys[PORT_LS_PH_UPDATE];
    const LpSys& cp = sLpSys[PORT_LS_PH_CALC];
    const double upMs = ms(up.ns, up.reads), cpMs = ms(cp.ns, cp.reads);
    const double shadowMs = ms(sLpCatNs[1][0x2D], sLpCatReads[1][0x2D]);
    const double ptclMs = ms(sLpCatNs[0][0x13] + sLpCatNs[0][0x14], sLpCatReads[0][0x13] + sLpCatReads[0][0x14]);
    const double moveLists = lists[1] - shadowMs, calcLists = lists[0] - ptclMs;
    port_log_summary("logic-prof: %u turns (%u drawn), GXV_LOGIC_PROF=%d, a turn: update phase %.3f ms = lists %.3f + outside the lists %.3f; "
                     "calc phase %.3f ms = calc-anim lists %.3f + shadows %.3f + particles %.3f + outside the lists %.3f; timer reads %.0f a turn "
                     "at %.2f us (taken off: %.2f ms a turn; lists raw %.2f + %.2f)%s",
                     sLpTurns, sLpDrawn, level, upMs, moveLists, upMs - moveLists, cpMs, calcLists, shadowMs, ptclMs,
                     cpMs - calcLists - shadowMs - ptclMs, sLpReads / turns, rc / 1000.0, sLpReads * rc / 1e6 / turns, raw[1], raw[0],
                     sLpOverflow ? " (class rows full: some objects not counted)" : "");
    // categories, costliest first
    for (int d = 1; d >= 0; --d) {
        char line[640];
        int o = snprintf(line, sizeof line, "logic-prof: %s categories a turn (ms, objects):", d ? "update" : "calc-anim");
        bool shown[64] = {};
        for (int rank = 0; rank < 12 && o < static_cast< int >(sizeof line) - 64; ++rank) {
            int best = -1;
            double bestMs = 0.009;
            for (int c = 0; c < 64; ++c) {
                const double v = ms(sLpCatNs[d][c], sLpCatReads[d][c]);
                if (!shown[c] && v > bestMs) { best = c; bestMs = v; }
            }
            if (best < 0) break;
            shown[best] = true;
            o += snprintf(line + o, sizeof line - o, " %s %.2f (%.1f)", LpCatName(d, best), bestMs, sLpCatCalls[d][best] / turns);
        }
        port_log_summary("%s", line);
    }
    // classes, costliest first
    const int top = LogicProfTop();
    for (int d = 1; d >= 0; --d) {
        const LpClass* shown[40] = {};
        for (int rank = 0; rank < top; ++rank) {
            const LpClass* best = nullptr;
            double bestMs = -1.0;
            for (const LpClass& r : sLpClass) {
                if (r.vt == nullptr || r.domain != d || r.calls == 0) continue;
                bool used = false;
                for (int k = 0; k < rank; ++k) used |= shown[k] == &r;
                const double v = ms(r.ns, r.reads);
                if (!used && v > bestMs) { best = &r; bestMs = v; }
            }
            if (best == nullptr || bestMs < 0.005) break;
            shown[rank] = best;
            char name[96];
            LpPrintable(name, sizeof name, best->name);
            port_log_summary("logic-prof: %s #%d cat=%s name=%s vt=%p ms/turn=%.3f calls/turn=%.1f us/call=%.1f (raw ms/turn %.3f)", d ? "update" : "calc",
                             rank + 1, LpCatName(d, best->category), name, best->vt, bestMs, best->calls / turns, bestMs * 1000.0 * turns / best->calls,
                             best->ns / 1e6 / turns);
        }
    }
    // systems
    static const char* const kSysNames[PORT_LS_N] = {"gravity", "gravity-walk", "area-find", "area-manager", "collision", "binder", "actor-calcanim",
        "anim-clock", "gravity", "sensor-messages", "nerve", "control", "binder", "effects", "camera-ctrl", "light", "sensor-positions", "sound",
        "shadow-request", "update-phase", "calc-phase"};
    for (int part = 0; part < 2; ++part) {
        if (part == 1 && level < 2) break;
        char line[900];
        int o = snprintf(line, sizeof line, part ? "logic-prof: movement phases a turn (GXV_LOGIC_PROF=2; ms, calls; in the player's lists ms):"
                                                 : "logic-prof: systems a turn (inclusive; ms, calls; in the player's lists ms):");
        const int lo = part ? PORT_LS_MV_FIRST : 0, hi = part ? PORT_LS_MV_SHADOWREQ + 1 : PORT_LS_MV_FIRST;
        for (int id = lo; id < hi && o < static_cast< int >(sizeof line) - 64; ++id) {
            const LpSys& s = sLpSys[id];
            o += snprintf(line + o, sizeof line - o, " %s %.3f (%.1f; %.3f)", kSysNames[id], ms(s.ns, s.reads), s.calls / turns,
                          ms(s.nsPlayer, s.readsPlayer));
        }
        port_log_summary("%s", line);
    }
    port_log_summary("logic-prof: counts a turn: gravities visited %.1f (walks through the grid %.1f), areas tested %.1f (lookups answered "
                     "through the grid %.1f), collision parts looked at %.1f (searched %.1f); BCK joint evaluations %.1f (repeating one of "
                     "the same turn %.1f; GXV_LOGIC_PROF=2 only)",
                     sLpCount[PORT_LC_GRAV_VISITS] / turns, sLpCount[PORT_LC_GRAV_GRID] / turns, sLpCount[PORT_LC_AREA_TESTS] / turns,
                     sLpCount[PORT_LC_AREA_GRID] / turns, sLpCount[PORT_LC_COLL_PARTS] / turns, sLpCount[PORT_LC_COLL_SEARCH] / turns,
                     sLpCount[PORT_LC_BCK_CALLS] / turns, sLpCount[PORT_LC_BCK_REPEATS] / turns);
    }
reset:
    for (LpClass& r : sLpClass) {
        if (r.vt != nullptr) {
            r.calls = 0;
            r.ns = r.reads = 0;
            memset(r.phNs, 0, sizeof r.phNs);
            memset(r.phReads, 0, sizeof r.phReads);
        }
    }
    for (LpFlush& r : sLpFlush) {
        if (r.vt != nullptr) { r.calls = r.leftOut = 0; r.ns = r.reads = 0; }
    }
    for (LpSys& s : sLpSys) {
        s.ns = s.nsPlayer = s.reads = s.readsPlayer = 0;
        s.calls = 0;
    }
    memset(sLpCount, 0, sizeof sLpCount);
    memset(sLpCatNs, 0, sizeof sLpCatNs);
    memset(sLpCatReads, 0, sizeof sLpCatReads);
    memset(sLpCatCalls, 0, sizeof sLpCatCalls);
    memset(sLpCoNs, 0, sizeof sLpCoNs);
    memset(sLpCoReads, 0, sizeof sLpCoReads);
    memset(sLpCoCalls, 0, sizeof sLpCoCalls);
    sLpDeepTurns = sLpDeepDrawn = 0;
    sLpTurns = sLpDrawn = 0;
    sLpReads = 0;
    sLpOverflow = false;
    LpCalibrate();
}
// A new scene: the class rows go (their vtables stay valid, but the names were the old scene's objects').
extern "C" void port_logic_prof_drop(void) {
    memset(sLpClass, 0, sizeof sLpClass);
    memset(sLpFlush, 0, sizeof sLpFlush);
}

// ---- GXV_STREAM_CENSUS (lighter25, 11 Nov; default 0): the game's side of the stream census (port/stream_census.h, the front end's
// platform/vgx/src/vgx_census.inc). Every n-th picture: BEGIN after the display copy, the source of what follows, each J3D material
// packet's model with its size code. The markers are BP 0xFF writes (kind 21: BEGIN 0xFFFF / source id, 22: model, 23: model end), written
// only while the FIFO writes into the stream (not into a display list being recorded).
#include "port/stream_census.h"
#include "port/gx_fifo.h"
extern "C" int __PortGXFifoInStream(void);
int port_sc_turn = 0;
namespace {
int ScEvery() {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_STREAM_CENSUS"); const long v = e && *e ? strtol(e, nullptr, 0) : 0; return v > 0 ? static_cast<int>(v) : 0; }());
}
uint32_t sScPics = 0, sScSerial = 0;
int sScStack[32];
int sScDepth = 0;
// model data -> census id (1..4094; 0 free), names from ResourceHolder (made when the census is on at the model's load)
constexpr int kScSlots = 8192, kScIds = 4096;
const void** sScKey = nullptr;
uint16_t* sScId = nullptr;
char (*sScName)[28] = nullptr;
int sScNext = 1;
inline void ScMark(uint32_t kind, uint32_t payload) {
    if (!__PortGXFifoInStream()) return;
    __PortGXFifo_u8(0x61);
    __PortGXFifo_u32((0xFFu << 24) | ((payload & 0xFFFFu) << 8) | kind);
}
uint32_t ScSlot(const void* p) {
    uint32_t h = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p) >> 4) * 2654435761u;
    return h >> (32 - 13);
}
int ScIdOf(const void* p, const char* name) {
    if (!p) return kScIds - 1;
    if (!sScKey) {
        sScKey = static_cast<const void**>(calloc(kScSlots, sizeof(void*)));
        sScId = static_cast<uint16_t*>(calloc(kScSlots, sizeof(uint16_t)));
        sScName = static_cast<char (*)[28]>(calloc(kScIds, 28));
        if (!sScKey || !sScId || !sScName) return kScIds - 1;
    }
    uint32_t s = ScSlot(p);
    for (int n = 0; n < kScSlots; ++n, s = (s + 1) & (kScSlots - 1)) {
        if (sScKey[s] == p) {
            if (name && !sScName[sScId[s]][0]) snprintf(sScName[sScId[s]], 28, "%s", name);
            return sScId[s];
        }
        if (!sScKey[s]) {
            if (sScNext >= kScIds - 1) return kScIds - 1;  // (the table is full: "others")
            sScKey[s] = p;
            sScId[s] = static_cast<uint16_t>(sScNext++);
            snprintf(sScName[sScId[s]], 28, "%s", name ? name : "");
            return sScId[s];
        }
    }
    return kScIds - 1;
}
}  // namespace
// ---- GXV_ARRAYSKIP (lighter25; port/array_skip.h): the mode, the counts and the "arrayskip:" line (every 300 pictures while it is on)
#include "port/array_skip.h"
extern "C" int port_arrayskip_mode(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_ARRAYSKIP"); return e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0; }());
}
namespace {
uint64_t sAsWrites = 0, sAsSkipped = 0, sAsMarked = 0;
uint32_t sAsPics = 0;
}  // namespace
extern "C" void port_arrayskip_note(int skipped) {
    ++sAsWrites;
    if (skipped) ++sAsSkipped;
}
extern "C" void port_arrayskip_verify_mark(uint32_t reg, uint32_t value) {
    // (kinds 24..26: the register 0xA0..0xA2 and the value's high half; kind 27: the low half, at which the front end compares)
    if (!__PortGXFifoInStream()) return;
    __PortGXFifo_u8(0x61);
    __PortGXFifo_u32((0xFFu << 24) | ((value >> 16) << 8) | (24u + ((reg - 0xA0u) & 3u)));
    __PortGXFifo_u8(0x61);
    __PortGXFifo_u32((0xFFu << 24) | ((value & 0xFFFFu) << 8) | 27u);
    ++sAsMarked;
}
static void ArraySkipPicture() {
    const int mode = port_arrayskip_mode();
    if (mode == 0) return;
    if (++sAsPics < 300) return;
    port_log_summary("arrayskip: GXV_ARRAYSKIP=%d, %u pictures: per picture %.1f J3D array-base writes, %.1f of them the value the stream holds (%s)",
                     mode, sAsPics, static_cast<double>(sAsWrites) / sAsPics, static_cast<double>(sAsSkipped) / sAsPics,
                     mode == 1 ? "left out" : "sent behind a check: see the front end's \"arrayskip verify:\" line");
    sAsPics = 0;
    sAsWrites = sAsSkipped = sAsMarked = 0;
}
extern "C" void port_stream_census_copy(void) {
    ArraySkipPicture();
    port_sc_turn = 0;
    sScDepth = 0;
    const int n = ScEvery();
    if (n <= 0) return;
    if (++sScPics % static_cast<uint32_t>(n) != 0) return;
    if (!__PortGXFifoInStream()) return;
    ++sScSerial;
    port_sc_turn = 1;
    ScMark(21, 0xFFFF);
}
extern "C" void port_stream_census_src_begin(int id) {
    if (!port_sc_turn) return;
    if (sScDepth < 32) sScStack[sScDepth] = id;
    ++sScDepth;
    ScMark(21, static_cast<uint32_t>(id));
}
extern "C" void port_stream_census_src_end(void) {
    if (!port_sc_turn) return;
    if (sScDepth > 0) --sScDepth;
    ScMark(21, sScDepth > 0 && sScDepth <= 32 ? static_cast<uint32_t>(sScStack[sScDepth - 1]) : 0x7FFEu);
}
extern "C" int port_stream_census_judging(void) { return port_sc_turn; }
// (the stamp: the counted picture's serial and the size code 1..8 of the sphere's projected diameter in lines of a 456-line picture)
extern "C" unsigned port_stream_census_stamp(float radius, float dist, float tanHalfFovy) {
    unsigned code = 8;
    if (dist > radius && tanHalfFovy > 0.0f) {
        const float d = radius * 456.0f / (dist * tanHalfFovy);
        code = d < 1.0f ? 1 : d < 2.0f ? 2 : d < 4.0f ? 3 : d < 8.0f ? 4 : d < 16.0f ? 5 : d < 32.0f ? 6 : d < 64.0f ? 7 : 8;
    }
    return ((sScSerial & 0x0FFFFFFFu) << 4) | code;
}
// pModelData: the J3DModelData; stamp: the model's last stamp (0: none)
extern "C" void port_stream_census_model_begin2(const void* pModelData, unsigned stamp) {
    if (!port_sc_turn) return;
    const unsigned code = (stamp >> 4) == (sScSerial & 0x0FFFFFFFu) ? (stamp & 0xFu) : 0u;
    ScMark(22, static_cast<uint32_t>(ScIdOf(pModelData, nullptr)) | (code << 12));
}
extern "C" void port_stream_census_model_begin(const void* pModelData) { port_stream_census_model_begin2(pModelData, 0); }
extern "C" void port_stream_census_model_end(void) {
    if (port_sc_turn) ScMark(23, 0);
}
extern "C" void port_stream_census_model_note(const void* pModelData, const char* pName) {
    if (ScEvery() <= 0) return;
    ScIdOf(pModelData, pName);
}
// (the front end's lines name the models: vgx_census.inc)
extern "C" const char* port_stream_census_model_name(unsigned id) {
    if (!sScName || id >= static_cast<unsigned>(kScIds)) return nullptr;
    if (id == kScIds - 1) return "(others)";
    return sScName[id][0] ? sScName[id] : nullptr;
}
