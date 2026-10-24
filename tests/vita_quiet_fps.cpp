#include "../platform/src/gx/vgx_galaxy.cpp"
#include <cstdarg>
#undef vsnprintf

static int64_t nowNs = 1000000000;
static uint32_t turns = 100, skips = 80;
static unsigned cpuQueries, clockQueries, diagnosticLogs, summaries;
static std::string summary;
extern "C" int64_t port_host_time_ns() { ++clockQueries; return nowNs; }
extern "C" int64_t port_thread_cpu_us() { ++cpuQueries; return 0; }
extern "C" uint32_t port_frameskip_count() { return skips; }
extern "C" void port_perf_game_totals(int64_t* work, uint32_t* count, int64_t* cpu, int64_t* max) {
    *work = *cpu = *max = 0;
    *count = turns;
}
extern "C" void port_log(const char*, ...) { ++diagnosticLogs; }
extern "C" void port_log_summary(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    ::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    summary = buffer;
    ++summaries;
}
static void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static void expect(double shown, double pace, unsigned skipped) {
    double seconds = 0, pictures = 0, game = 0;
    unsigned omitted = 0;
    require(std::sscanf(summary.c_str(), "fps: %lf s: shown %lf fps; game %lf turns/s; %u skipped",
                        &seconds, &pictures, &game, &omitted) == 4, "quiet summary is available");
    require(seconds == 5.0 && pictures == shown && game == pace && omitted == skipped,
            "summary must use matching window deltas, excluding pre-window activity");
    require(cpuQueries == 0 && diagnosticLogs == 0, "quiet summaries must not sample CPU or emit verbose reports");
}
int main() {
    sPerfSeconds = 5;
    if (!PORT_LOGGING) {
        for (int i = 0; i < 1000; ++i) { nowNs += 1000000000; PerfFrame(0, 0); }
        require(clockQueries == 0 && cpuQueries == 0 && summaries == 0 && diagnosticLogs == 0,
                "silent frame reporting must do no timing or logging work");
        std::puts("PASS: silent frames bypass all reporting work");
        return 0;
    }
    sFramesShown.store(20);
    PerfFrame(0, 0);
    require(summaries == 0, "first snapshot establishes a baseline without a bogus rate");
    nowNs += 4999999999;
    PerfFrame(0, 0);
    require(summaries == 0, "no summary before the reporting interval");
    ++nowNs;
    turns = 400; skips = 320; sFramesShown.store(80);
    PerfFrame(0, 0);
    expect(12.0, 60.0, 240);
    nowNs += 5000000000;
    turns = 650; skips = 520; sFramesShown.store(130);
    PerfFrame(0, 0);
    expect(10.0, 50.0, 200);
    sPerf = {};
    turns = 0xfffffff0u; skips = 0xfffffff0u; sFramesShown.store(200);
    PerfFrame(0, 0);
    nowNs += 5000000000;
    turns = 16; skips = 6; sFramesShown.store(210);
    PerfFrame(0, 0);
    expect(2.0, 6.4, 22);
    require(summaries == 3, "one summary per completed reporting interval");
    std::puts("PASS: quiet FPS windows, warm-up exclusion, CPU-query suppression, and counter wrap");
}
