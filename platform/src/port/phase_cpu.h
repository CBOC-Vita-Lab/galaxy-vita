#pragma once

#if PORT_PHASE_CPU_COMPILED
#include <errno.h>
#include "../../vgx/src/vgx_common.h"

namespace port::phasecpu {

struct PreserveErrno {
    int saved = errno;
    ~PreserveErrno() { errno = saved; }
};

inline bool Enabled() {
    // Default-off native phase CPU/wall diagnostics. Read on first frame begin,
    // not library load or checkpoints: native startup
    // may set controls after loading. EnvInt retains its established prefix syntax.
    static const bool enabled = [] {
        PreserveErrno keep;
        return vgx::EnvInt("GXV_PHASECPU", 0) == 1;
    }();
    return enabled;
}

struct Stamp { int64_t wall = 0, cpu = 0; };
struct Stats {
    uint64_t intervals = 0, wallSamples = 0, wallNs = 0;
    uint64_t matched = 0, matchedWallNs = 0, cpuUs = 0;
    uint64_t invalidWall = 0, invalidCpu = 0;
    void Add(Stamp from, Stamp to) {
        ++intervals;
        const bool wallValid = from.wall > 0 && to.wall > 0 && to.wall >= from.wall;
        const bool cpuValid = from.cpu > 0 && to.cpu > 0 && to.cpu >= from.cpu;
        if (wallValid) { ++wallSamples; wallNs += uint64_t(to.wall - from.wall); }
        else ++invalidWall;
        if (!cpuValid) ++invalidCpu;
        if (wallValid && cpuValid) {
            ++matched;
            matchedWallNs += uint64_t(to.wall - from.wall);
            cpuUs += uint64_t(to.cpu - from.cpu);
        }
    }
    void Merge(const Stats& other) {
        intervals += other.intervals; wallSamples += other.wallSamples; wallNs += other.wallNs;
        matched += other.matched; matchedWallNs += other.matchedWallNs; cpuUs += other.cpuUs;
        invalidWall += other.invalidWall; invalidCpu += other.invalidCpu;
    }
};

struct State {
    bool active = false;
    uint32_t serial = 0, phase = 0;
    Stamp first, last;
    uint64_t touched = 0, completed = 0, abandoned = 0, orphanEnds = 0, unknown = 0;
    uint64_t wallFaults = 0, cpuFaults = 0, wallBackwards = 0, cpuBackwards = 0;
    Stats pending[PORT_GAME_PHASE_COUNT], phases[PORT_GAME_PHASE_COUNT], whole;
};
static_assert(PORT_GAME_PHASE_COUNT <= 64);
static thread_local State state;
static std::atomic<uint32_t> nextSerial{1};

inline Stamp Read(State& s) {
    const Stamp result{port_host_time_ns(), port_thread_cpu_us()};
    if (result.wall <= 0) ++s.wallFaults;
    if (result.cpu <= 0) ++s.cpuFaults;
    return result;
}

inline void CloseInterval(State& s, Stamp now) {
    if (s.last.wall > 0 && now.wall > 0 && now.wall < s.last.wall) ++s.wallBackwards;
    if (s.last.cpu > 0 && now.cpu > 0 && now.cpu < s.last.cpu) ++s.cpuBackwards;
    s.pending[s.phase].Add(s.last, now);
    s.touched |= uint64_t{1} << s.phase;
    // Never bridge an invalid endpoint to a later valid one.
    s.last = now;
}

inline void ReportRow(const State& s, const char* name, const Stats& v) {
    port_log_summary("phase-cpu cumulative thread=%u phase=%s intervals=%llu wall-samples=%llu wall-ns=%llu matched=%llu matched-wall-ns=%llu cpu-us=%llu invalid-wall=%llu invalid-cpu=%llu",
        s.serial, name, (unsigned long long)v.intervals, (unsigned long long)v.wallSamples,
        (unsigned long long)v.wallNs, (unsigned long long)v.matched, (unsigned long long)v.matchedWallNs,
        (unsigned long long)v.cpuUs, (unsigned long long)v.invalidWall, (unsigned long long)v.invalidCpu);
}

inline void Report(const State& s) {
    port_log_summary("phase-cpu cumulative thread=%u turns=%llu abandoned=%llu orphan-ends=%llu unknown=%llu wall-faults=%llu cpu-faults=%llu wall-backwards=%llu cpu-backwards=%llu",
        s.serial, (unsigned long long)s.completed, (unsigned long long)s.abandoned,
        (unsigned long long)s.orphanEnds, (unsigned long long)s.unknown,
        (unsigned long long)s.wallFaults, (unsigned long long)s.cpuFaults,
        (unsigned long long)s.wallBackwards, (unsigned long long)s.cpuBackwards);
    ReportRow(s, "whole", s.whole);
    for (uint32_t phase = 0; phase < PORT_GAME_PHASE_COUNT; ++phase)
        if (s.phases[phase].intervals)
            ReportRow(s, phase ? port_game_progress_name(phase) : "unclassified", s.phases[phase]);
}

inline void Begin() {
    PreserveErrno keep;
    if (!Enabled()) return;
    State& s = state;
    if (!s.serial) s.serial = nextSerial.fetch_add(1, std::memory_order_relaxed);
    if (s.active) ++s.abandoned;
    for (uint32_t phase = 0; phase < PORT_GAME_PHASE_COUNT; ++phase)
        if (s.touched & (uint64_t{1} << phase)) s.pending[phase] = Stats{};
    s.touched = 0;
    s.phase = 0;
    s.first = s.last = Read(s);
    s.active = true;
}

inline void Checkpoint(uint32_t phase) {
    State& s = state;
    if (!s.active) return;
    PreserveErrno keep;
    CloseInterval(s, Read(s));
    if (!phase || phase >= PORT_GAME_PHASE_COUNT) { ++s.unknown; phase = 0; }
    s.phase = phase;
}

inline void End() {
    State& s = state;
    if (!s.active) {
        if (s.serial) ++s.orphanEnds;
        return;
    }
    PreserveErrno keep;
    const Stamp now = Read(s);
    CloseInterval(s, now);
    s.active = false;
    s.whole.Add(s.first, now);
    for (uint32_t phase = 0; phase < PORT_GAME_PHASE_COUNT; ++phase)
        if (s.touched & (uint64_t{1} << phase)) s.phases[phase].Merge(s.pending[phase]);
    ++s.completed;
    // At most 66 integer-only lines, outside this completed turn's sampled span.
    if ((s.completed & 255u) == 0) Report(s);
}

} // namespace port::phasecpu
#endif
