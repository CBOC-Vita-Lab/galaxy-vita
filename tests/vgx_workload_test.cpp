#include "vgx_workload.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static thread_local int64_t Wall = 1000000000, Cpu = 1000;
static std::atomic<unsigned> WallReads{0}, CpuReads{0};
static std::string Log;
extern "C" int64_t port_host_time_ns() { ++WallReads; return Wall; }
extern "C" int64_t port_thread_cpu_us() { ++CpuReads; return Cpu; }
extern "C" void port_log_summary(const char* fmt, ...) {
    char line[2048]; va_list ap; va_start(ap, fmt); std::vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    Log += line; Log += '\n';
}
static void require(bool yes, const char* why) { if (!yes) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
using namespace vgx::workload;
static void spend(int64_t wallUs, int64_t cpuUs) { Wall += wallUs * 1000; Cpu += cpuUs; }
static void off() {
    BeginTurn(); FinishWait(true); { Scope s(Dsp); spend(100, 60); } FinishWait(false); EndTurn();
    { Scope s(FrontSample, SampleProcess()); } FrontPicture(); CandidateRun(800); Picture(4, 3, 1, 2, 6, 2, 3, 1); Report();
    require(WallReads == 0 && CpuReads == 0 && Log.empty(), "default off never queries clocks or logs");
    require(Counters[Turn].Read().calls == 0 && Pictures.pictures == 0, "default off never records observations");
    setenv("GXV_WORKLOAD_PROBE", "1", 1);
    require(!Enabled(), "environment switch is read once, cannot enable after initialization");
}
static void timing() {
    BeginTurn();
    spend(200, 150);
    { Scope s(Dsp); spend(300, 180); }
    { Scope s(PublishInTurn); spend(400, 10); }
    FinishWait(true); spend(500, 20); FinishWait(false);
    EndTurn();
    const auto t = Counters[Turn].Read();
    require(t.calls == 1 && t.wallUs == 1400 && t.cpuUs == 360 && t.matchedWallUs == 1400, "whole-turn CPU and wall have identical boundaries");
    require(Counters[DspInTurn].Read().cpuUs == 180 && Counters[Dsp].Read().wallUs == 300, "DSP is explicitly a subset of the whole turn");
    require(Counters[PublishInTurn].Read().wallUs == 400 && Counters[FinishInTurn].Read().wallUs == 500, "separate publication/finish waiting spans");
    EndTurn();
    require(Counters[Turn].Read().calls == 1, "double done cannot duplicate a turn");
    std::thread worker([] { Scope s(Dsp); spend(900, 700); }); worker.join();
    require(Counters[Dsp].Read().calls == 2 && Counters[DspInTurn].Read().calls == 1, "helper DSP time cannot be charged to the game thread");
    { Scope s(PublishInTurn); spend(100, 50); }
    require(Counters[PublishInTurn].Read().calls == 1, "publication outside a turn is not charged to a game turn");
}
static void invalid() {
    Counter c;
    c.Add({1000, 100}, {11000, 105});
    c.Add({0, 100}, {10000, 200});
    c.Add({11000, 100}, {1000, 200});
    c.Add({1000, 0}, {11000, 900000});
    c.Add({1000, 100}, {11000, 0});
    c.Add({1000, 100}, {11000, 99});
    const auto t = c.Read();
    require(t.calls == 6 && t.wallSamples == 4 && t.wallUs == 40, "invalid/backwards wall samples do not underflow");
    require(t.cpuSamples == 1 && t.cpuUs == 5 && t.matchedWallUs == 10, "failed/backwards CPU samples never turn lifetime counters into CPU deltas");
    const Totals old{UINT32_MAX - 1, UINT32_MAX, UINT32_MAX - 5, UINT32_MAX, UINT32_MAX, UINT32_MAX - 2};
    const auto d = Delta({1, 0, 4, 0, 9, 6}, old);
    require(d.calls == 3 && d.wallSamples == 1 && d.wallUs == 10 && d.cpuSamples == 1 && d.matchedWallUs == 10 && d.cpuUs == 9,
            "lifetime counter wrap has well-defined unsigned report deltas");
}
static void concurrent() {
    Counter c;
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&] { for (unsigned n = 0; n < 10000; ++n) c.Add({1000, 100}, {11000, 103}); });
    for (auto& worker : workers) worker.join();
    const auto t = c.Read();
    require(t.calls == 80000 && t.wallSamples == 80000 && t.wallUs == 800000 && t.cpuSamples == 80000 && t.cpuUs == 240000,
            "concurrent producers retain every completed sample after joining");
}
static void sampling(unsigned stride) {
    unsigned selected = 0;
    const unsigned w = WallReads;
    for (unsigned n = 0; n < 1024; ++n) {
        Scope s(FrontSample, SampleProcess()); selected += n % stride == 0; spend(100, 60);
    }
    require(Counters[FrontSample].Read().calls == selected && WallReads - w == 2 * selected,
            "only selected ProcessStream scopes sample clocks");
    setenv("GXV_WORKLOAD_STRIDE", "999", 1);
    require(Stride() == stride, "stride is read once");
    FrontPicture(); spend(1000, 500); FrontPicture();
    require(Counters[FrontFrame].Read().cpuUs == 500 && Counters[FrontFrame].Read().wallUs == 1000,
            "frame-boundary span includes all calling-thread CPU and is separate from sampled scopes");
}
static void reporting() {
    Report();
    BeginTurn(); spend(1000, 700); EndTurn();
    CandidateRun(300); CandidateRun(800); Picture(100, 4, 1, 3, 7, 2, 8, 1);
    spend(4998999, 100); Report(); require(Log.empty(), "report is sparse before five seconds");
    spend(1, 0); Report();
    require(Log.find("work-turn calls=1 wall_samples=1 wall_us=1000 cpu_samples=1 matched_wall_us=1000 cpu_us=700") != std::string::npos,
            "report exposes sample counts and matched CPU/wall totals with explicit units");
    require(Log.find("draw_commands=100 max_draw_commands=100 texture_copy_commands=4") != std::string::npos,
            "census describes command counts and per-picture maxima");
    require(Log.find("fx_candidate_runs=2 vertices=1100 max_run_vertices=800 runs_ge400=1 runs_ge800=1") != std::string::npos,
            "FX candidate run distribution uses actual contiguous runs, not reduced-only runs");
    require(Log.find("NOT additive") != std::string::npos && Log.find("NOT GPU timing") != std::string::npos && Log.find("sampled-only") != std::string::npos,
            "report preserves overlap, sampling, and GPU measurement boundaries");
    Log.clear(); spend(5000000, 100); Report();
    require(Log.find("work-turn calls=0 wall_samples=0 wall_us=0") != std::string::npos && Log.find("max_draw_commands=0") != std::string::npos,
            "second report is a delta and resets maxima");
}
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string mode = argv[1];
    if (mode == "off") off();
    else {
        setenv("GXV_WORKLOAD_PROBE", "1", 1);
        if (mode == "timing") timing();
        else if (mode == "invalid") invalid();
        else if (mode == "concurrent") concurrent();
        else if (mode == "reporting") reporting();
        else if (mode == "sampling") sampling(argc > 2 ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)) : 32);
        else return 2;
    }
    std::printf("PASS: workload %s\n", mode.c_str());
}
