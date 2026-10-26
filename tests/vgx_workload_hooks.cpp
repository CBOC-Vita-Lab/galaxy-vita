// Actual quiet-player frame/phase hooks, using deterministic host clocks.
#include "../platform/src/port/perf.cpp"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

static int64_t Wall = 1000000000, Cpu = 1000;
static unsigned WallReads = 0, CpuReads = 0;
extern "C" int64_t port_host_time_ns() { ++WallReads; return Wall; }
extern "C" int64_t port_thread_cpu_us() { ++CpuReads; return Cpu; }
extern "C" void port_log_summary(const char*, ...) {}
extern "C" void port_log(const char*, ...) {}
extern "C" int port_handheld() { return 1; }
extern "C" unsigned port_vgx_backend_pending() { return 0; }
// Optional peer-package hooks remain disabled in this standalone observer fixture.
// Definitions also let Mach-O link the fixture after pacing/autoq are combined.
extern "C" int port_vgx_pace_turn(int) { return -1; }
extern "C" void port_autoq_game_begin() {}
extern "C" void port_autoq_game_work_done() {}
extern "C" void port_settings_panel_turn(int) {}
namespace MR { const char* getCurrentStageName() { return ""; } }
static void require(bool yes, const char* why) { if (!yes) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
int main(int argc, char**) {
    const bool on = argc > 1;
    if (on) setenv("GXV_WORKLOAD_PROBE", "1", 1);
    port_perf_frame_begin();
    Wall += 1000000; Cpu += 500;
    port_game_progress_checkpoint(PORT_GAME_PHASE_GX_FINISH_WAIT);
    Wall += 2000000; Cpu += 100;
    port_game_progress_checkpoint(PORT_GAME_PHASE_AUDIO_MESSAGE_WAIT); // A foreign progress lane must not end a main-lane phase.
    Wall += 1000000; Cpu += 100;
    port_game_progress_checkpoint(PORT_GAME_PHASE_CALC_ANIM);
    Wall += 1000000; Cpu += 200;
    port_perf_frame_work_done(); port_perf_frame_work_done();
    int64_t work = -1, cpu = -1, maximum = -1; uint32_t turns = 0;
    port_perf_game_totals(&work, &turns, &cpu, &maximum);
    require(work == 0 && cpu == 0 && maximum == 0, "probe cannot modify legacy quiet diagnostics' unmeasured timing totals");
    require(turns == 2, "probe does not change the existing turn count contract");
    using namespace vgx::workload;
    if (on) {
        const auto t = Counters[Turn].Read(), f = Counters[FinishInTurn].Read();
        require(t.calls == 1 && t.wallUs == 5000 && t.cpuUs == 900, "production frame hooks measure one matched turn, not a duplicate done");
        require(f.calls == 1 && f.wallUs == 3000 && f.cpuUs == 200, "production phase dispatch measures only the bounded main-thread finish phase");
    } else require(WallReads == 0 && CpuReads == 0 && Counters[Turn].Read().calls == 0, "production quiet hooks remain clock-free by default");
    std::printf("PASS: production workload frame/phase hooks %s\n", on ? "on" : "off");
}
