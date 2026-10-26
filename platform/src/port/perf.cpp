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
#include "../../vgx/src/vgx_workload.h"
#include "mtx_profile.h"

// (declared here and in the vgx glue rather than in port/port.h, which everything includes)
extern "C" int64_t port_thread_cpu_us(void);
extern "C" void port_perf_game_totals(int64_t* workNs, uint32_t* frames, int64_t* cpuUs, int64_t* maxWorkNs);
extern "C" void port_game_progress_report(const char* why);
extern "C" int port_handheld(void);
extern "C" int port_frameskip_turn(int allowed);
extern "C" uint32_t port_frameskip_count(void);
extern "C" unsigned port_vgx_backend_pending(void) __attribute__((weak));  // vgx_galaxy.cpp
extern "C" int port_vgx_pace_turn(int allowed) __attribute__((weak));  // -1: legacy cadence

extern "C" int64_t port_vgx_player_cpu_us(void) __attribute__((weak));
extern "C" void port_vgx_diagnostic_csv_pause(int selecting) __attribute__((weak));
extern "C" void port_autoq_game_begin(void) __attribute__((weak));
extern "C" void port_autoq_game_work_done(void) __attribute__((weak));

#include "phase_cpu.h"

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
    static const bool v = getenv("PETARI_PERFLOG") != nullptr;
    return v;
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
    static const bool v = alwaysLog() || port_handheld() != 0;
    return v;
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

#if PORT_PHASE_CPU_COMPILED
void port_native_phase_cpu_checkpoint(uint32_t phase) {
    port::phasecpu::Checkpoint(phase);
}
#endif

void port_game_progress_checkpoint(uint32_t phase) {
    const bool mainLane = !(phase >= PORT_GAME_PHASE_AUDIO_RESOURCES && phase <= PORT_GAME_PHASE_AUDIO_MESSAGE_WAIT);
    if (mainLane) vgx::workload::FinishWait(phase == PORT_GAME_PHASE_GX_FINISH_WAIT);
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
#if PORT_PHASE_CPU_COMPILED
    port::phasecpu::Begin();
#endif
}

void port_perf_frame_work_done(void) {
#if PORT_PHASE_CPU_COMPILED
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
// what only the picture needs (the view and draw-list pass of GameScene::calcAnim). OFF by default: PETARI_SKIP_VIEWCALC=1 turns it on.
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

// "rear_aim = 1" in the same file: the rear touch pad aims the pointer (vita/runtime/input.cpp). Off unless chosen: -1 = not read.
std::atomic<int> sRearAim{-1};
int RearAimFromFile() {
    if (sZoomFile.empty()) return 0;
    FILE* f = fopen(sZoomFile.c_str(), "r");
    if (!f) return 0;
    int on = 0;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (strncmp(p, "rear_aim", 8) != 0) continue;
        p += 8;
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
    v = RearAimFromFile();
    int expected = -1;
    if (sRearAim.compare_exchange_strong(expected, v)) port_log_summary("rear touch aiming: %s", v ? "on (from the settings file)" : "off");
    return sRearAim.load();
}

void ZoomSave() {
    if (sZoomFile.empty()) return;
    const std::string tmp = sZoomFile + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    const int t = sZoomTenths.load();
    fprintf(f, "# Settings of the Vita port, changed in the game's pause menu (L and R: zoom; Triangle: rear touch aiming).\nzoom = %d.%d\nrear_aim = %d\n",
            t / 10, t % 10, RearAimRead());
    const bool ok = fclose(f) == 0;
    if (ok) {
        remove(sZoomFile.c_str());  // (a rename onto an existing file fails on some of the console's file systems)
        if (rename(tmp.c_str(), sZoomFile.c_str()) != 0) {
            port_log_summary("zoom: the settings file %s could not be written", sZoomFile.c_str());
            return;
        }
    }
    sZoomSavedChange = sZoomChange.load();
    port_log_summary("zoom: %d.%d, rear touch aiming %s, saved in %s", t / 10, t % 10, RearAimRead() ? "on" : "off", sZoomFile.c_str());
}

int ZoomRead() {
    int t = sZoomTenths.load(std::memory_order_relaxed);
    if (t >= 0) return t;
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
}

// Rear touch aiming (pause menu: Triangle toggles it; saved with the zoom). 1 = on.
extern "C" int port_rear_aim_enabled(void) { return port_handheld() ? RearAimRead() : 0; }
extern "C" int port_rear_aim_toggle(void) {
    const int now = RearAimRead() ? 0 : 1;
    sRearAim.store(now);
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
static void SceneKeyRefresh() {
    if (!&MR::getCurrentStageName) return;
    const char* name = MR::getCurrentStageName();
    uint32_t h = 2166136261u;  // FNV-1a
    for (const char* c = name; c && *c; ++c) h = (h ^ static_cast<unsigned char>(*c)) * 16777619u;
    if (!name || !*name) h = 0;
    sSceneKey.store(h, std::memory_order_relaxed);
}
extern "C" int port_turn_skipped(void) {
    SceneKeyRefresh();
    static const bool on = [] {
        const char* e = getenv("PETARI_SKIP_VIEWCALC");
        return e && *e == '1';
    }();
    return on && sTurnSkipped.load(std::memory_order_relaxed);
}

int port_frameskip_turn(int allowed) {
    if (PORT_RUNTIME_DIAGNOSTICS) sTurn.kind = allowed ? TurnKind::DrawSelected : TurnKind::RequiredDraw;
    // Testing (the harness has a virtual clock and never falls behind): PETARI_FORCE_SKIP=N leaves out N turns of every N+1.
    static const int forced = [] {
        const char* e = getenv("PETARI_FORCE_SKIP");
        return e && *e ? (atoi(e) < 0 ? 0 : (atoi(e) > 8 ? 8 : atoi(e))) : 0;
    }();
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
    static const int maxRun = [] {
        const char* e = getenv("PETARI_FRAMESKIP");
        if (e && *e) return atoi(e) < 0 ? 0 : (atoi(e) > 8 ? 8 : atoi(e));
        return port_handheld() ? 3 : 0;
    }();
    if (maxRun <= 0) return 0;
    static const int pipeDepth = [] {
        const char* e = getenv("PETARI_PIPE_DEPTH");
        return e && *e ? (atoi(e) < 0 ? 0 : (atoi(e) > 3 ? 3 : atoi(e))) : 2;
    }();
    static const int maxPipeRun = [] {
        const char* e = getenv("PETARI_PIPE_RUN");
        return e && *e ? (atoi(e) < 0 ? 0 : (atoi(e) > 12 ? 12 : atoi(e))) : 6;
    }();
    static const int64_t period = 16683350;  // ns
    static int64_t last = 0, debt = 0;
    static int run = 0;
    const int64_t now = port_host_time_ns();
    const int64_t dt = last ? now - last : period;
    last = now;
    if (dt > 15 * period || !allowed) {  // a load, a pause, a scene change: nothing to catch up with
        debt = 0;
        run = 0;
        return 0;
    }
    debt += dt - period;
    if (debt < 0) debt = 0;
    if (debt > 4 * period) debt = 4 * period;
    const bool behind = debt >= period / 2 && run < maxRun;
    const bool gpuBusy = pipeDepth > 0 && port_vgx_backend_pending && port_vgx_backend_pending() >= static_cast<unsigned>(pipeDepth) && run < maxPipeRun;
    if (behind || gpuBusy) {
        ++run;
        sSkippedTurns.fetch_add(1, std::memory_order_relaxed);
        if (PORT_RUNTIME_DIAGNOSTICS) {
            sTurn.kind = behind ? TurnKind::LateSkip : TurnKind::BacklogSkip;
            sTurn.bothSkipReasons = behind && gpuBusy;
        }
        // 2: late, catch up (the turn does not wait for its retrace: a wait would make every skipped turn last a
        // whole refresh, the debt would never be paid, and the title screen ran at a fixed 15 pictures a second
        // on the console); 1: not late, the turn takes its retrace.
        return behind ? 2 : 1;
    }
    run = 0;
    return 0;
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
