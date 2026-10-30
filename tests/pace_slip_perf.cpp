// GXV_SLIP through the production perf.cpp ("Up to 60": port_frameskip_turn, the turn's work end, the log line), on a virtual clock.
// A CPU-bound stretch: every turn costs G ms, a turn that draws D ms more; the retrace wait is the port's (one-message queue).
//   tools/test_pace_slip_perf.sh   builds this once and runs it with GXV_SLIP unset (off) and GXV_SLIP=1 (cap 15, then 10)
// The switches are read once per process, so each setting is its own run: `pace_slip_perf <G ms> <D ms> <expect: off|slip|skip>`.
#define VITA_PERF_SOURCE "../platform/src/port/perf.cpp"
#include VITA_PERF_SOURCE
#include <cstdarg>
#include <string>
#undef vsnprintf

static int64_t wallNs = 1000000000;
static std::string slipLog;
static unsigned slipLines;
extern "C" int64_t port_host_time_ns() { return wallNs; }
extern "C" int64_t port_thread_cpu_us() { return wallNs / 1000; }
extern "C" int64_t port_vgx_player_cpu_us() { return 0; }
extern "C" unsigned port_vgx_backend_pending() { return 0; }
extern "C" int port_vgx_pace_turn(int) { return -1; }
extern "C" int port_handheld() { return 1; }
extern "C" void port_log_summary(const char*, ...) {}
extern "C" void port_autoq_game_begin() {}
extern "C" void port_autoq_game_work_done() {}
extern "C" void port_settings_panel_turn(int) {}
extern "C" void port_vgx_diagnostic_csv_pause(int) {}
namespace MR { const char* getCurrentStageName() { return ""; } }
extern "C" void port_log(const char* format, ...) {
    char line[2048]; va_list args; va_start(args, format);
    ::vsnprintf(line, sizeof(line), format, args); va_end(args);
    if (std::strstr(line, "pace: slip")) { slipLog += line; slipLog += '\n'; ++slipLines; }
}
static void require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

int main(int argc, char** argv) {
    require(argc == 4, "usage: pace_slip_perf <G ms> <D ms> <off|slip|skip>");
    const int64_t g = std::atoll(argv[1]) * 1000000, d = std::atoll(argv[2]) * 1000000;
    const std::string expect = argv[3];
    const int64_t vbl = 16683000;
    int64_t posted = wallNs / vbl, next = posted, msg = 0;
    bool full = false;
    auto post = [&](int64_t t) { for (const int64_t c = t / vbl; posted < c;) { ++posted; if (!full) { full = true; msg = posted; } } };
    const int64_t t0 = wallNs, tEnd = t0 + 30000000000ll, tMeasure = tEnd - 20000000000ll;
    int64_t tStart = 0;
    unsigned turns = 0, drawn = 0;
    while (wallNs < tEnd) {
        port_perf_frame_begin();
        const int dec = port_frameskip_turn(1);
        if (!tStart && wallNs >= tMeasure) { tStart = wallNs; turns = drawn = 0; }
        ++turns;
        wallNs += g;
        if (dec == 0) { ++drawn; wallNs += d; }
        port_perf_frame_work_done();
        if (dec != 2) {
            post(wallNs);
            for (;;) {
                if (!full) { wallNs = (posted + 1) * vbl; post(wallNs); }
                const int64_t m = msg;
                full = false;
                if (m >= next) { next = m + 1; break; }
            }
        } else post(wallNs);
    }
    const double secs = (wallNs - tStart) / 1e9, speed = turns * (vbl / 1e9) / secs, pictures = drawn / secs;
    std::printf("G %lld + D %lld ms, expect %s: game speed %.1f %%, %.1f pictures/s, slip log lines %u\n%s", static_cast<long long>(g / 1000000),
                static_cast<long long>(d / 1000000), expect.c_str(), speed * 100, pictures, slipLines, slipLog.c_str());
    if (expect == "slip") {
        const double pair = (2.0 * g + d) / 1e9, ideal = 2.0 * vbl / 1e9 / pair;  // (a short turn that ends before its retrace waits for it: a little under)
        require(speed <= ideal + 0.005 && speed >= ideal - 0.03 && pictures <= 30.0 * ideal + 0.5 && pictures >= 30.0 * ideal - 1.5, "slip: one picture a pair at the speed the pair allows");
        require(slipLines == 1 && slipLog.find("engaged") != std::string::npos, "one 'engaged' line");
    } else if (expect == "off") {
        require(speed > 0.99 && pictures < 28.0, "off: the skip rule holds the speed at fewer than 28 pictures a second");
        require(slipLines == 0, "off: no slip line");
    } else {
        require(slipLines == 0 && pictures < 28.0, "over the cap: the skip rule as before, no slip line");
    }
    std::printf("PASS\n");
    return 0;
}
