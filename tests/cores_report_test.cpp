// Host test of the "cores-5s:" lines (platform/include/port/cores_report.h): grouping by affinity mask, per-picture figures, merged names, the
// length of every line, and the probe line. Stub rows stand in for the console's thread table.
#include "port/cores_report.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace port::cores;

static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static ThreadRow Row(const char* name, unsigned mask, unsigned prio, unsigned long long us) {
    ThreadRow r;
    std::snprintf(r.name, sizeof r.name, "%s", name);
    r.mask = mask; r.priority = prio; r.cpuUs = us;
    return r;
}

int main() {
    // 5 s, 300 pictures: the game thread 17.7 ms a picture, the player 16.5, the finisher 4.9, a guest thread twice (merged), two par workers
    std::vector<ThreadRow> rows = {
        Row("game-main", 0x10000, 0x9F, 17700ull * 300), Row("guest", 0x10000, 0x9F, 300ull * 300), Row("guest", 0x10000, 0x9F, 200ull * 300),
        Row("vgx-render", 0x20000, 0x9F, 5000ull * 300), Row("jas-audio", 0x20000, 0x4F, 700ull * 300),
        Row("vgx-player", 0x40000, 0x9F, 16500ull * 300),
        Row("vgx-finish", 0x80000, 0x9E, 4900ull * 300), Row("smg_view_async", 0x80000, 0x9E, 450ull * 300), Row("smg_spec", 0x80000, 0xA0, 600ull * 300),
        Row("smg_tiny", 0x80000, 0x9E, 5ull * 300),
        Row("vi-clock", 0x60000, 0x50, 20ull * 300), Row("log-flush", 0, 0xBF, 100ull * 300)};
    std::vector<std::string> lines;
    FormatCoresReport(rows.data(), static_cast<int>(rows.size()), 5.0, 300, 301, [&](const char* l) { lines.push_back(l); });
    Require(lines.size() == 7, "a header and one line for each of core 0, 1, 2, 3, cores 1-2, unpinned");
    Require(lines[0].find("core 0 18.2 (109%)") != std::string::npos, "core 0 = 17.7 + 0.3 + 0.2 ms a picture, as a share of 16.7 ms");
    Require(lines[0].find("core 1 5.7 (34%)") != std::string::npos && lines[0].find("core 2 16.5 (99%)") != std::string::npos, "cores 1 and 2");
    Require(lines[0].find("core 3 6.0 (36%)") != std::string::npos, "core 3 = 4.9 + 0.45 + 0.6 + 0.005 = 5.955");
    Require(lines[0].find("cores 1-2 (floating) 0.0 (0%)") != std::string::npos || lines[0].find("cores 1-2 (floating) 0.1") != std::string::npos, "the floating clock");
    Require(lines[1].find("cores-5s core 0: game-main 17.70 [0x9F], guest 0.50 [0x9F]") == 0, "core 0 line: merged guest threads, sorted");
    bool sawFinisher = false, sawTiny = false, sawOthers = false;
    for (const auto& l : lines) {
        Require(l.size() < 1000, "every line is under 1000 characters");
        if (l.find("vgx-finish 4.90 [0x9E]") != std::string::npos) sawFinisher = true;
        if (l.find("smg_tiny") != std::string::npos) sawTiny = true;
        if (l.find("others 0.01") != std::string::npos) sawOthers = true;
    }
    Require(sawFinisher, "the finisher's 4.9 ms and priority are listed");
    Require(!sawTiny && sawOthers, "a thread under 0.02 ms a picture is folded into 'others'");
    // no picture shown: figures per turn
    lines.clear();
    FormatCoresReport(rows.data(), static_cast<int>(rows.size()), 5.0, 0, 300, [&](const char* l) { lines.push_back(l); });
    Require(!lines.empty() && lines[0].find("CPU ms a turn") != std::string::npos, "no pictures: per turn");
    // nothing: no lines
    lines.clear();
    FormatCoresReport(rows.data(), 0, 5.0, 300, 300, [&](const char* l) { lines.push_back(l); });
    Require(lines.empty(), "no rows, no lines");
    // many threads cannot overflow a line
    std::vector<ThreadRow> many;
    for (int i = 0; i < kReportMaxRows; ++i) { char nm[24]; std::snprintf(nm, sizeof nm, "a-long-thread-name-%02d", i); many.push_back(Row(nm, 0x80000, 0x9E, 900ull * 300)); }
    lines.clear();
    FormatCoresReport(many.data(), static_cast<int>(many.size()), 5.0, 300, 300, [&](const char* l) { lines.push_back(l); });
    for (const auto& l : lines) Require(l.size() < 1000, "64 busy threads on one core still fit a line");
    // the probe line
    ProbeWindow p[3];
    p[0].label = "core 3 @0x9E"; p[0].wakes = 2490; p[0].maxUs = 3400; p[0].over500 = 11; p[0].over1500 = 3; p[0].over3000 = 2;
    p[1].label = "core 3 @0x60"; p[1].wakes = 2490; p[1].maxUs = 300;
    p[2].label = "core 1 @0x9E"; p[2].wakes = 2490; p[2].maxUs = 900; p[2].over500 = 4;
    std::string probe;
    FormatProbeReport(p, 3, [&](const char* l) { probe = l; });
    Require(probe.find("core 3 @0x9E: 2490 wakes, max 3.40 ms, over 0.5 ms 11, over 1.5 ms 3, over 3 ms 2") != std::string::npos && probe.size() < 1000, "the probe line");
    std::puts("PASS: cores report: groups by mask, per-picture ms, merged names, line lengths, per-turn fallback, probe line");
    return 0;
}
