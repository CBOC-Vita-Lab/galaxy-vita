// Exercise the actual production bridge definitions, not replacement hook bodies.
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>

static unsigned WallReads, CpuReads, Logs;
static int64_t Wall = 1000000000;
static int BridgeClock(clockid_t id, timespec* ts) {
    if (id != CLOCK_THREAD_CPUTIME_ID) std::abort();
    ++CpuReads;
    ts->tv_sec = 0; ts->tv_nsec = 123000;
    errno = EDOM;
    return 0;
}
#define clock_gettime BridgeClock
#include "../platform/src/port/perf.cpp"
#undef clock_gettime

extern "C" {
void port_workload_view_entry_begin();
void port_workload_view_entry_end();
uint32_t port_workload_anim_begin();
void port_workload_anim_end(uint32_t);
uint32_t port_workload_anim_category_begin(unsigned, unsigned);
void port_workload_anim_category_end(uint32_t);
uint32_t port_workload_anim_family_begin(unsigned);
void port_workload_anim_family_end(uint32_t);
void port_workload_anim_material_checkpoint(uint32_t, unsigned);
uint32_t port_workload_anim_material_diff_context();
void port_workload_anim_material_diff_counts(uint32_t, uint32_t, uint32_t, uint32_t);
int64_t port_host_time_ns() { ++WallReads; errno = EDOM; return Wall; }
void port_log(const char*, ...) { ++Logs; }
void port_log_summary(const char*, ...) { ++Logs; }
int port_handheld() { return 1; }
unsigned port_vgx_backend_pending() { return 0; }
int port_vgx_pace_turn(int) { return -1; }
void port_autoq_game_begin() {}
void port_autoq_game_work_done() {}
void port_settings_panel_turn(int) {}
// (tests31: the game-side and GX-FIFO symbols the perf unit has gained since this test was written; idle here)
void port_dist_turn_end(int) {}
void port_ff_census(unsigned long long*, unsigned long long*, unsigned long long*, float*) {}
int port_ff_flyover(void) { return 0; }
int port_pb_demo_off(void) { return 0; }
float port_pb_release_ptcl_w(void) { return 1.0f; }
uint64_t port_undrawn_state_hash(unsigned*) { return 0; }
int __PortGXFifoInStream(void) { return 0; }
uint8_t* __PortGXFifoPtr; uint8_t* __PortGXFifoLimit;
void __PortGXFifoOverflow(void) {}
}
void port_host_sleep_ns(int64_t) {}
namespace MR { const char* getCurrentStageName() { return ""; } }
#if defined(BRIDGE_TEST_EMUTLS)
static std::atomic<unsigned> TlsLookups{0};
extern "C" void* __real___emutls_get_address(void*);
extern "C" void* __wrap___emutls_get_address(void* p) {
    ++TlsLookups;
    return __real___emutls_get_address(p);
}
#endif
static void require(bool yes, const char* why) {
    if (!yes) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static void disabled() {
    using namespace vgx::workload;
    Thread.generation = 77; ViewEntry.generation = 66;
    AnimThread.owner = 55; AnimThread.parent = 44;
    const unsigned wall = WallReads, cpu = CpuReads, logs = Logs;
#if defined(BRIDGE_TEST_EMUTLS)
    const unsigned tls = TlsLookups;
#endif
    errno = E2BIG;
    port_workload_view_entry_begin(); port_workload_view_entry_end();
    require(port_workload_anim_begin() == 0, "disabled parent ticket is zero");
    port_workload_anim_end(123);
    require(port_workload_anim_category_begin(0, 6) == 0, "disabled category ticket is zero");
    port_workload_anim_category_end(123);
    require(port_workload_anim_family_begin(2) == 0, "disabled family ticket is zero");
    port_workload_anim_family_end(123);
    port_workload_anim_material_checkpoint(123, 3);
    require(port_workload_anim_material_diff_context() == 0, "disabled diff context is zero");
    port_workload_anim_material_diff_counts(123, 5, 2, 2);
    require(errno == E2BIG, "disabled bridges preserve errno");
    require(WallReads == wall && CpuReads == cpu && Logs == logs, "disabled bridges have no clock or log work");
#if defined(BRIDGE_TEST_EMUTLS)
    require(TlsLookups == tls, "disabled bridges never access emulated observer TLS");
#endif
    require(Thread.generation == 77 && ViewEntry.generation == 66 && AnimThread.owner == 55 && AnimThread.parent == 44,
            "disabled bridges leave observer state untouched");
    require(Counters[GameViewEntry].Read().calls == 0 && AnimCounts[AnimStarted] == 0 && MaterialDiffs[0] == 0,
            "disabled bridges publish no observations");
}
static void enabled() {
    using namespace vgx::workload;
    BeginTurn();
    const unsigned cpu = CpuReads;
    errno = E2BIG;
    port_workload_view_entry_begin(); Wall += 2000; port_workload_view_entry_end();
    require(errno == E2BIG && CpuReads == cpu, "enabled view forwarding preserves errno and avoids CPU clocks");
    require(Counters[GameViewEntry].Read().calls == 1 && Counters[GameViewEntry].Read().wallUs == 2,
            "actual bridges publish the measured view span");
    require(port_workload_anim_begin() == 0, "ARMHF bridge exports do not invent the Vita animation-turn lifecycle");
    // ARMHF does not acquire animation turns automatically; explicitly prepare
    // the existing observer lifecycle to verify forwarding, not full integration.
    AnimBeginTurn();
    errno = E2BIG;
    const uint32_t parent = port_workload_anim_begin();
    const uint32_t category = port_workload_anim_category_begin(0, 6);
    const uint32_t family = port_workload_anim_family_begin(2);
    require(parent && category && family, "bridges forward real parent/category/material tickets");
    for (unsigned phase : {1u, 2u, 4u, 3u}) {
        Wall += 1000; port_workload_anim_material_checkpoint(family, phase);
    }
    const uint32_t context = port_workload_anim_material_diff_context();
    require(context == family, "diff context preserves its owning family ticket");
    port_workload_anim_material_diff_counts(context, 5, 2, 2);
    Wall += 1000; port_workload_anim_material_checkpoint(family, 4);
    Wall += 1000; port_workload_anim_family_end(family);
    Wall += 1000; port_workload_anim_category_end(category);
    Wall += 1000; port_workload_anim_end(parent);
    AnimEndTurn();
    require(errno == E2BIG && CpuReads == cpu && Logs == 0, "animation forwarding preserves errno without CPU clocks or logs");
    require(AnimCounts[AnimComplete] == 1 && AnimCounters[AnimNpc].Read().calls == 1,
            "actual bridge ends publish one complete NPC parent");
    require(FamilyCounters[FamilyNpcMaterial].Read().calls == 1 && MaterialCounters[MaterialNpcDiff].Read().wallUs == 1,
            "family and material checkpoint arguments reach the real observer");
    require(MaterialDiffs[0] == 1 && MaterialDiffs[1] == 5 && MaterialDiffs[2] == 2 && MaterialDiffs[3] == 2,
            "all four material census arguments are forwarded unchanged");
}
int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "on") == 0) enabled();
    else disabled();
    const unsigned cpu = CpuReads;
    require(port_thread_cpu_us() == 123 && CpuReads == cpu + 1, "ARMHF retains the actual Linux CPU-clock implementation");
    std::puts("PASS: actual ARMHF workload bridges");
}
