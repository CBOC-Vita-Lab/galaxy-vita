// Actual quiet-player frame/phase hooks, using deterministic host clocks.
#include "../platform/src/port/perf.cpp"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <string>
#include <vector>

#if defined(WORKLOAD_TEST_EMUTLS)
static std::atomic<unsigned> TlsLookups{0};
extern "C" void* __real___emutls_get_address(void*);
extern "C" void* __wrap___emutls_get_address(void* control) {
    ++TlsLookups;
    return __real___emutls_get_address(control);
}
#endif

static int64_t Wall = 1000000000, Cpu = 1000;
static unsigned WallReads = 0, CpuReads = 0;
static bool ClockChangesErrno = false;
extern "C" int64_t port_host_time_ns() { ++WallReads; if (ClockChangesErrno) errno = EDOM; return Wall; }
#if defined(__vita__)
extern "C" int64_t port_thread_cpu_us() { ++CpuReads; return Cpu; }
#endif
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

static std::vector<std::string> Calls;
static bool Movie = false, TimeUp = false, Skipped = false;
using u32 = std::uint32_t;
struct GameScene {
    bool isPlayMovie() const { return Movie; }
    bool isNerve(int) const { return TimeUp; }
    void calcAnim();
};
struct SceneFunction {
    static void executeCalcAnimList() { Calls.emplace_back("animation"); Wall += 3000000; Cpu += 1500; }
    static void executeCalcAnimListOnPlayingMovie() { Calls.emplace_back("movie"); Wall += 700000; Cpu += 300; }
    static void executeCalcViewAndEntryList() { Calls.emplace_back("view"); Wall += 400000; Cpu += 200; errno = E2BIG; }
};
namespace MR { constexpr int CalcAnimType_AnimParticleIgnorePause = 1; }
struct CategoryList { static void execute(int) { Calls.emplace_back("particles"); Wall += 50000; Cpu += 20; } };
static int TestTurnSkipped() { return Skipped; }
#define GET_NERVE(owner, name) 0
#define port_turn_skipped TestTurnSkipped
// (GameScene::calcAnim's GXV_DRAWDONE_DEFER 3/4 view deferral: off here, mode 0, so the skip guard alone decides as before)
static int TestDrawdoneMode() { return 0; }
static int TestTurnSkippedRaw() { return 0; }
static bool sViewPending = false;
#define port_gx_drawdone_defer_mode TestDrawdoneMode
#define port_turn_skipped_raw TestTurnSkippedRaw
#include "vgx_view_entry_fixture.inc"
#undef port_gx_drawdone_defer_mode
#undef port_turn_skipped_raw
#undef port_turn_skipped
#undef GET_NERVE

static void viewHooks(bool on) {
#if defined(__vita__) && PORT_LOGGING
    const bool hooked = on;
#else
    const bool hooked = false;
    (void)on;
#endif
    GameScene scene;
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        Movie = scenario == 1; Skipped = scenario == 2; TimeUp = scenario == 3;
        Calls.clear();
        port_perf_frame_begin();
        const unsigned wall = WallReads, cpu = CpuReads;
        ClockChangesErrno = true; errno = E2BIG;
        scene.calcAnim();
        ClockChangesErrno = false;
        require(errno == E2BIG, "real calcAnim hooks preserve callee errno");
        require(CpuReads == cpu, "real view/entry hooks never query CPU time");
        require(WallReads - wall == (hooked && !Skipped ? 2u : 0u), "only logging-enabled Vita view/entry receives wall endpoints");
        port_perf_frame_work_done();
        std::vector<std::string> expected{Movie ? "movie" : "animation"};
        if (!TimeUp) expected.emplace_back("particles");
        if (!Skipped) expected.emplace_back("view");
        require(Calls == expected, "actual calcAnim preserves animation, particle, skip and view ordering");
    }
    const auto t = vgx::workload::Counters[vgx::workload::GameViewEntry].Read();
    require(t.calls == (hooked ? 3u : 0u) && t.wallUs == (hooked ? 1200u : 0u) && t.cpuSamples == 0 && t.cpuUs == 0,
            "production C wrappers publish only the three real view spans, not animation work");
}
int main(int argc, char**) {
    const bool on = argc > 1;
    if (on) setenv("GXV_WORKLOAD_PROBE", "1", 1);
    if (argc > 2) {
        viewHooks(on);
        std::printf("PASS: non-Vita/silent actual GameScene has no view hooks\n");
        return 0;
    }
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
#if defined(WORKLOAD_TEST_EMUTLS)
    const unsigned lookups = TlsLookups;
#endif
    viewHooks(on);
#if defined(WORKLOAD_TEST_EMUTLS)
    if (!on) require(TlsLookups == lookups, "disabled production view hooks never look up emulated TLS");
#endif
    std::printf("PASS: production workload frame/phase hooks %s\n", on ? "on" : "off");
}
