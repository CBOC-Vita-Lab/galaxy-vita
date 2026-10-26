// Optional coarse workload observer. Never waits for the GPU or changes a command.
#pragma once

#include <atomic>
#include <cstdint>
#include "vgx_common.h"

extern "C" int64_t port_host_time_ns(void) __attribute__((weak));
extern "C" int64_t port_thread_cpu_us(void) __attribute__((weak));
extern "C" void port_log_summary(const char*, ...) __attribute__((weak, format(printf, 1, 2)));

namespace vgx::workload {
inline bool Enabled() {
#if defined(PORT_LOGGING) && !PORT_LOGGING
    return false;
#else
    // GXV_WORKLOAD_PROBE=1: coarse CPU/wall/count observations in the basic log, also in quiet player builds; default off.
    static const bool on = EnvInt("GXV_WORKLOAD_PROBE", 0) == 1;
    return on && port_host_time_ns && port_thread_cpu_us && port_log_summary;
#endif
}
inline uint32_t Stride() {
    // GXV_WORKLOAD_STRIDE=1..1024: sample one ProcessStream call in this many; default 32, 1 measures observer overhead/full scopes.
    static const uint32_t stride = [] {
        const long n = EnvInt("GXV_WORKLOAD_STRIDE", 32);
        return static_cast<uint32_t>(n < 1 ? 1 : (n > 1024 ? 1024 : n));
    }();
    return stride;
}

enum Lane : unsigned { Turn, Dsp, DspInTurn, PublishInTurn, FinishInTurn, FrontSample, FrontFrame, FrontQueue, LaneCount };
inline constexpr const char* Names[] = {"work-turn", "dsp-all", "dsp-in-turn", "gx-publish-in-turn", "gx-finish-phase-in-turn",
                                      "fe-process-sampled", "fe-frame-including-idle", "fe-backend-queue"};
struct Reading { int64_t wall = 0, cpu = 0; };
struct Totals {
    uint32_t calls = 0, wallSamples = 0, wallUs = 0, cpuSamples = 0, matchedWallUs = 0, cpuUs = 0;
};
inline Totals Delta(const Totals& now, const Totals& old) {
    // Unsigned modular deltas tolerate lifetime counter wrap (report windows are five seconds).
    return {now.calls - old.calls, now.wallSamples - old.wallSamples, now.wallUs - old.wallUs,
            now.cpuSamples - old.cpuSamples, now.matchedWallUs - old.matchedWallUs, now.cpuUs - old.cpuUs};
}
struct Counter {
    std::atomic<uint32_t> calls{0}, wallSamples{0}, wallUs{0}, cpuSamples{0}, matchedWallUs{0}, cpuUs{0};
    void Add(Reading begin, Reading end) {
        calls.fetch_add(1, std::memory_order_relaxed);
        if (begin.wall <= 0 || end.wall < begin.wall || (end.wall - begin.wall) / 1000 > UINT32_MAX) return;
        const auto wall = static_cast<uint32_t>((end.wall - begin.wall) / 1000);
        wallSamples.fetch_add(1, std::memory_order_relaxed);
        wallUs.fetch_add(wall, std::memory_order_relaxed);
        if (begin.cpu <= 0 || end.cpu < begin.cpu || end.cpu - begin.cpu > UINT32_MAX) return;
        cpuSamples.fetch_add(1, std::memory_order_relaxed);
        matchedWallUs.fetch_add(wall, std::memory_order_relaxed);
        cpuUs.fetch_add(static_cast<uint32_t>(end.cpu - begin.cpu), std::memory_order_relaxed);
    }
    Totals Read() const {
        return {calls.load(std::memory_order_relaxed), wallSamples.load(std::memory_order_relaxed), wallUs.load(std::memory_order_relaxed),
                cpuSamples.load(std::memory_order_relaxed), matchedWallUs.load(std::memory_order_relaxed), cpuUs.load(std::memory_order_relaxed)};
    }
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);
inline Counter Counters[LaneCount];
inline std::atomic<uint32_t> ProcessCalls{0};
inline Reading ReadClock() { return {port_host_time_ns(), port_thread_cpu_us()}; }
struct ThreadState {
    bool inTurn = false, inFinish = false, frontStarted = false;
    uint32_t generation = 0, processCalls = 0;
    Reading turn, finish, front;
};
inline thread_local ThreadState Thread;

class Scope {
    Lane lane;
    bool active, inTurn = false;
    uint32_t generation = 0;
    Reading begin;
public:
    explicit Scope(Lane which, bool selected = true) : lane(which), active(selected && Enabled()) {
        if (active) {
            inTurn = Thread.inTurn;
            generation = Thread.generation;
            if ((lane == PublishInTurn || lane == FinishInTurn) && !inTurn) active = false;
            else begin = ReadClock();
        }
    }
    ~Scope() {
        if (!active) return;
        const Reading end = ReadClock();
        if ((lane == PublishInTurn || lane == FinishInTurn) && (!inTurn || !Thread.inTurn || generation != Thread.generation)) return;
        Counters[lane].Add(begin, end);
        if (lane == Dsp && inTurn && Thread.inTurn && generation == Thread.generation) Counters[DspInTurn].Add(begin, end);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
inline void BeginTurn() {
    if (!Enabled()) return;
    ++Thread.generation;
    Thread.inTurn = true;
    Thread.inFinish = false;
    Thread.turn = ReadClock();
}
inline void FinishWait(bool waiting) {
    if (!Enabled() || !Thread.inTurn || waiting == Thread.inFinish) return;
    if (waiting) Thread.finish = ReadClock();
    else Counters[FinishInTurn].Add(Thread.finish, ReadClock());
    Thread.inFinish = waiting;
}
inline void EndTurn() {
    if (!Enabled() || !Thread.inTurn) return;
    FinishWait(false);
    Counters[Turn].Add(Thread.turn, ReadClock());
    Thread.inTurn = false;
}
inline bool SampleProcess() {
    if (!Enabled()) return false;
    ProcessCalls.fetch_add(1, std::memory_order_relaxed);
    return Thread.processCalls++ % Stride() == 0;
}
inline void FrontPicture() {
    if (!Enabled()) return;
    const Reading now = ReadClock();
    if (Thread.frontStarted) Counters[FrontFrame].Add(Thread.front, now);
    Thread.front = now;
    Thread.frontStarted = true;
}

struct PictureCounts {
    uint32_t pictures = 0, drawCommands = 0, textureCopies = 0, displayCopies = 0, passBegins = 0;
    uint32_t backendScenes = 0, copyQuads = 0, fxCandidates = 0, reducedFxRuns = 0;
    uint32_t candidateRuns = 0, candidateVerts = 0, runs400 = 0, runs800 = 0;
};
// Render thread only: command/call census, not measured driver stores/reloads or GPU time.
inline PictureCounts Pictures, PreviousPictures;
inline uint32_t MaxDrawCommands = 0, MaxBackendScenes = 0, MaxCandidateVerts = 0;
inline void CandidateRun(uint32_t vertices) {
    if (!Enabled()) return;
    ++Pictures.candidateRuns;
    Pictures.candidateVerts += vertices;
    Pictures.runs400 += vertices >= 400;
    Pictures.runs800 += vertices >= 800;
    if (vertices > MaxCandidateVerts) MaxCandidateVerts = vertices;
}
inline void Picture(uint32_t draws, uint32_t copies, uint32_t displays, uint32_t passes,
                    uint32_t scenes, uint32_t quads, uint32_t candidates, uint32_t reducedRuns) {
    if (!Enabled()) return;
    ++Pictures.pictures;
    Pictures.drawCommands += draws; Pictures.textureCopies += copies; Pictures.displayCopies += displays; Pictures.passBegins += passes;
    Pictures.backendScenes += scenes; Pictures.copyQuads += quads; Pictures.fxCandidates += candidates; Pictures.reducedFxRuns += reducedRuns;
    if (draws > MaxDrawCommands) MaxDrawCommands = draws;
    if (scenes > MaxBackendScenes) MaxBackendScenes = scenes;
}
inline Totals Previous[LaneCount];
inline int64_t WindowStart = 0;
inline uint32_t PreviousProcessCalls = 0;
inline void Report() {
    if (!Enabled()) return;
    const int64_t now = port_host_time_ns();
    if (now <= 0) return;
    if (!WindowStart || now < WindowStart) {
        WindowStart = now;
        for (unsigned i = 0; i < LaneCount; ++i) Previous[i] = Counters[i].Read();
        PreviousPictures = Pictures;
        PreviousProcessCalls = ProcessCalls.load(std::memory_order_relaxed);
        MaxDrawCommands = MaxBackendScenes = MaxCandidateVerts = 0;
        return;
    }
    if (now - WindowStart < 5000000000ll) return;
    const uint32_t processCalls = ProcessCalls.load(std::memory_order_relaxed);
    port_log_summary("workload: %.2f s; stride=%u process_calls=%u; rolling asynchronous windows; subset/overlapping rows are NOT additive; CPU is calling-thread time, wall includes waits",
                     (now - WindowStart) / 1e9, Stride(), processCalls - PreviousProcessCalls);
    PreviousProcessCalls = processCalls;
    for (unsigned i = 0; i < LaneCount; ++i) {
        const Totals total = Counters[i].Read(), d = Delta(total, Previous[i]);
        Previous[i] = total;
        port_log_summary("workload: %s calls=%u wall_samples=%u wall_us=%u cpu_samples=%u matched_wall_us=%u cpu_us=%u",
                         Names[i], d.calls, d.wallSamples, d.wallUs, d.cpuSamples, d.matchedWallUs, d.cpuUs);
    }
    const PictureCounts p = Pictures, old = PreviousPictures;
    port_log_summary("workload: census pictures=%u draw_commands=%u max_draw_commands=%u texture_copy_commands=%u display_copy_commands=%u pass_begin_commands=%u backend_target_scenes=%u max_backend_target_scenes=%u copy_quads=%u fx_candidate_draws=%u reduced_fx_runs=%u",
                     p.pictures-old.pictures, p.drawCommands-old.drawCommands, MaxDrawCommands, p.textureCopies-old.textureCopies,
                     p.displayCopies-old.displayCopies, p.passBegins-old.passBegins, p.backendScenes-old.backendScenes, MaxBackendScenes,
                     p.copyQuads-old.copyQuads, p.fxCandidates-old.fxCandidates, p.reducedFxRuns-old.reducedFxRuns);
    port_log_summary("workload: fx_candidate_runs=%u vertices=%u max_run_vertices=%u runs_ge400=%u runs_ge800=%u; census is NOT GPU timing or measured GXM store/reload traffic; ProcessStream is sampled-only, not the entire front end",
                     p.candidateRuns-old.candidateRuns, p.candidateVerts-old.candidateVerts, MaxCandidateVerts, p.runs400-old.runs400, p.runs800-old.runs800);
    PreviousPictures = p;
    MaxDrawCommands = MaxBackendScenes = MaxCandidateVerts = 0;
    WindowStart = now;
}
} // namespace vgx::workload
