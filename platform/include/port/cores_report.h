// The "cores-5s:" basic-log lines (sched24): what each thread of the port used of each core in the last 5 s, per picture, so that one console run shows the
// whole balance of the four cores and an experiment on placement or priority can be read off it. Pure formatting and grouping, no kernel calls (the Vita
// runtime, vita/runtime/memory.cpp port_vita_cores_report, supplies the samples; tests/cores_report_test.cpp feeds it stubs).
//
//   GXV_CORES_LOG=1|0   (read once) the lines on / off. Unset: on when GXV_TURN_PROF or GXV_WORKLOAD_PROBE is set (the console base line has both), else off.
//   GXV_CORE_PROBE=1    (default 0) three tiny probe threads (core 3 at the par workers' priority, core 3 at 0x60, core 1 at the par workers' priority) sleep
//                       2 ms at a time and measure how late they wake; the "cores-5s wake-lat:" line says whether the fourth core is lent in slices and whether
//                       a thread of the par workers' priority waits behind the finishing thread. ~1500 wake-ups a second: not for a timing run.
//
// A thread's CPU is the kernel's run time (sceKernelGetThreadInfo runClocks, in microseconds), read by the thread that logs. The kernel brings it up to
// date when the thread leaves its core, so a thread that is running at the moment of the read is short by its current stretch: over 5 s the sums hold, one
// line alone does not (the same caveat as the "threads:" census lines of vita/runtime/memory.cpp). A thread's core is its affinity MASK: a thread that
// may use cores 1 and 2 is listed under "cores 1-2" (its CPU is on one of them, the kernel does not say which), one with no restriction under "unpinned".
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace port::cores {

constexpr int kReportMaxRows = 64;
constexpr double kReportPictureMs = 1000.0 / 60.0;  // the budget a picture has at 60 a second: the percent shown is of this

struct ThreadRow {
    char name[24] = {};
    unsigned mask = 0;       // the affinity mask: 0x10000 core 0, 0x20000 core 1, 0x40000 core 2, 0x80000 core 3, sums for several, 0 any
    unsigned priority = 0;   // the kernel's current priority (0x40 highest .. 0xBF lowest)
    unsigned long long cpuUs = 0;  // run time in the window
};

// 0-3 one core, 4 cores 1-2, 5 anything else (unpinned, core 0 with others, ...).
inline int ReportGroup(unsigned mask) {
    switch (mask) {
    case 0x10000u: return 0;
    case 0x20000u: return 1;
    case 0x40000u: return 2;
    case 0x80000u: return 3;
    case 0x60000u: return 4;
    default: return 5;
    }
}
inline const char* ReportGroupName(int g) {
    static const char* const names[6] = {"core 0", "core 1", "core 2", "core 3", "cores 1-2 (floating)", "unpinned or several"};
    return g >= 0 && g < 6 ? names[g] : "?";
}

// The lines for one window, handed to emit(const char*) one at a time (each under 1000 characters: the log writer cuts at 1100 with its time stamp).
// `rows` may hold several rows with one name (the game's guest threads): they are added up. Rows under 0.02 ms a picture are counted but not listed.
template <class Emit>
void FormatCoresReport(const ThreadRow* rows, int n, double seconds, unsigned pictures, unsigned turns, Emit emit) {
    if (n <= 0 || seconds <= 0) return;
    const double per = pictures ? pictures : (turns ? turns : 1);  // (per picture; per turn when none was shown)
    const bool byTurn = pictures == 0;
    double sum[6] = {};
    struct Merged { char name[24]; unsigned prio; double ms; };
    Merged merged[6][kReportMaxRows];
    int count[6] = {};
    for (int i = 0; i < n && i < kReportMaxRows; ++i) {
        const int g = ReportGroup(rows[i].mask);
        const double ms = rows[i].cpuUs / 1000.0 / per;
        sum[g] += ms;
        int at = -1;
        for (int k = 0; k < count[g]; ++k)
            if (!std::strncmp(merged[g][k].name, rows[i].name, sizeof merged[g][k].name)) { at = k; break; }
        if (at < 0) {
            at = count[g]++;
            std::snprintf(merged[g][at].name, sizeof merged[g][at].name, "%s", rows[i].name);
            merged[g][at].prio = rows[i].priority;
            merged[g][at].ms = 0;
        }
        merged[g][at].ms += ms;
        if (rows[i].priority < merged[g][at].prio) merged[g][at].prio = rows[i].priority;  // (the highest one a name had)
    }
    char line[1000];
    const char* unit = byTurn ? "turn" : "picture";
    int o = std::snprintf(line, sizeof line, "cores-5s: %.1f s, %u pictures, %u turns; CPU ms a %s (percent of 16.7 ms): ", seconds, pictures, turns, unit);
    for (int g = 0; g < 6; ++g) {
        if (g >= 4 && sum[g] < 0.005) continue;
        o += std::snprintf(line + o, o < static_cast<int>(sizeof line) ? sizeof line - o : 0, "%s%s %.1f (%.0f%%)", g ? ", " : "", ReportGroupName(g), sum[g],
                           sum[g] * 100.0 / kReportPictureMs);
        if (o >= static_cast<int>(sizeof line)) break;
    }
    emit(line);
    for (int g = 0; g < 6; ++g) {
        if (count[g] == 0) continue;
        std::sort(merged[g], merged[g] + count[g], [](const Merged& a, const Merged& b) { return a.ms > b.ms; });
        o = std::snprintf(line, sizeof line, "cores-5s %s:", ReportGroupName(g));
        int listed = 0;
        double rest = 0;
        for (int k = 0; k < count[g]; ++k) {
            if (merged[g][k].ms < 0.02 || o >= static_cast<int>(sizeof line) - 60) { rest += merged[g][k].ms; continue; }
            o += std::snprintf(line + o, sizeof line - o, "%s %s %.2f [0x%X]", listed ? "," : "", merged[g][k].name, merged[g][k].ms, merged[g][k].prio);
            ++listed;
        }
        if (rest >= 0.005) o += std::snprintf(line + o, o < static_cast<int>(sizeof line) ? sizeof line - o : 0, "%s others %.2f", listed ? "," : "", rest);
        emit(line);
    }
}

// The probe threads' lateness counts for one window (see GXV_CORE_PROBE above).
struct ProbeWindow {
    const char* label = "";
    unsigned wakes = 0, over500 = 0, over1500 = 0, over3000 = 0, maxUs = 0;
};
template <class Emit>
void FormatProbeReport(const ProbeWindow* p, int n, Emit emit) {
    char line[1000];
    int o = std::snprintf(line, sizeof line, "cores-5s wake-lat (GXV_CORE_PROBE: a 2 ms sleep, how late it ended):");
    for (int i = 0; i < n; ++i) {
        o += std::snprintf(line + o, o < static_cast<int>(sizeof line) ? sizeof line - o : 0, "%s %s: %u wakes, max %.2f ms, over 0.5 ms %u, over 1.5 ms %u, over 3 ms %u",
                           i ? ";" : "", p[i].label, p[i].wakes, p[i].maxUs / 1000.0, p[i].over500, p[i].over1500, p[i].over3000);
        if (o >= static_cast<int>(sizeof line)) break;
    }
    emit(line);
}

}  // namespace port::cores
