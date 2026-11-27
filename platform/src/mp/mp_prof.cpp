// Galaxy 3.0 multiplayer: the cost counter behind port/mp_prof.h (owner: perf). The detailed counter is empty unless SMG_MP_PROF
// is defined; the lite counters (three zones, SMG_MP_PERFLOG=1) and the quality governor's inputs (perf3) are always there.
// Game thread only (the zones are timed where the game calls into multiplayer); plain counters, no locks, no heap.
#include "port/mp_prof.h"
#include "port/mp_bridge.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <pthread.h>
#include <time.h>
#endif

// ---- MP3 perf3: lite counters and the governor's inputs (always compiled) ---------------------------------------------------
extern "C" int port_pace_steady30_engaged(void) __attribute__((weak));     // port/perf.cpp: Performance mode's steady 30
extern "C" uint64_t port_autoq_picture_work_us(void) __attribute__((weak));  // vgx_autoq.cpp: a picture's work (target mode)

extern "C" {
int mp_perf_on = 0;
}
namespace {
uint32_t g_perfUs[3];   // since the last mp_perf_take: total, puppet calc, puppet draw

int EnvInt(const char* name, int fallback) {
    const char* e = getenv(name);
    return e && *e ? atoi(e) : fallback;
}
}  // namespace

#if !(defined(SMG_MP_PROF) && SMG_MP_PROF)
extern "C" uint64_t mp_prof_now_us(void) {
#ifdef __vita__
    return (uint64_t)sceKernelGetProcessTimeWide();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}
#endif

extern "C" void mp_perf_add(int zone, uint32_t us) {
    const int i = zone == MP_PROF_TOTAL ? 0 : zone == MP_PROF_PUPPET_CALC ? 1 : zone == MP_PROF_PUPPET_DRAW ? 2 : -1;
    if (i >= 0) g_perfUs[i] += us;
}

extern "C" void mp_perf_take(uint32_t out[3]) {
    for (int i = 0; i < 3; i++) {
        out[i] = g_perfUs[i];
        g_perfUs[i] = 0;
    }
}

extern "C" int mp_perflog_env(void) {
    static int on = -1;
    if (on < 0) {
        on = EnvInt("SMG_MP_PERFLOG", 0) == 1 ? 1 : 0;
        mp_perf_on = on;
    }
    return on;
}

// The picture rate the game aims at: 60 (16667 us), the locked-60 goal, unless a real 30 fps cap is engaged on purpose (GXV_FPS_LIMIT=30, or
// Performance mode's steady 30 pacing): then 30 is what the player gets and is meant to get. Never assumed otherwise (optfix: it used to
// say 33333 unless GXV_AUTOQ_TARGET=60). Read again at each call (the Settings menu changes the picture mode in play).
extern "C" uint32_t mp_frame_target_us(void) {
    bool thirty = EnvInt("GXV_FPS_LIMIT", 0) == 30;
    if (port_pace_steady30_engaged && port_pace_steady30_engaged()) thirty = true;
    return thirty ? 33333u : 16667u;
}

extern "C" uint32_t mp_frame_work_us(void) {
    const uint64_t w = port_autoq_picture_work_us ? port_autoq_picture_work_us() : 0;
    return w > 1000000u ? 1000000u : (uint32_t)w;
}

extern "C" int mp_quality_forced(void) {
    static int forced = -2;
    if (forced == -2) {
        const char* e = getenv("SMG_MP_QUALITY");
        forced = e && *e >= '0' && *e <= '4' && e[1] == 0 ? *e - '0' : -1;
    }
    return forced;
}

// optfix: the deepest level the automatic governor may use: 0 (default: it never changes the level, no visible quality is traded);
// SMG_MP_QUALITY_AUTO=1..4 allows steps down to that level (a testing switch).
extern "C" int mp_quality_auto_max(void) {
    static int mx = -1;
    if (mx < 0) {
        const char* e = getenv("SMG_MP_QUALITY_AUTO");
        mx = e && *e >= '0' && *e <= '4' && e[1] == 0 ? *e - '0' : 0;
    }
    return mx;
}

extern "C" int mp_test_slow_ms(int* belowLevel) {
#ifdef __vita__
    *belowLevel = 0;
    return 0;
#else
    static int ms = -1, below = 5;
    if (ms < 0) {
        const char* e = getenv("SMG_MP_TEST_SLOW_MS");
        ms = 0;
        if (e && *e) {
            char* end = nullptr;
            const long v = strtol(e, &end, 10);
            ms = v > 0 && v <= 500 ? (int)v : 0;
            if (end && *end == ':') {
                const long l = strtol(end + 1, nullptr, 10);
                below = l >= 1 && l <= 5 ? (int)l : 5;
            }
        }
    }
    *belowLevel = below;
    return ms;
#endif
}

extern "C" void mp_test_sleep_ms(int ms) {
#ifdef __vita__
    (void)ms;
#else
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
#endif
}

#if defined(SMG_MP_PROF) && SMG_MP_PROF

namespace {
const char* const kNames[MP_PROF_ZONES] = {"total", "net", "pup-upd", "pup-calc", "pup-draw", "world", "flow", "ui-upd", "ui-draw", "lockwait"};
uint64_t g_turnUs[MP_PROF_ZONES];   // this turn
uint64_t g_sumUs[MP_PROF_ZONES];    // this report window
uint64_t g_maxUs[MP_PROF_ZONES];
uint32_t g_counts[4];
uint32_t g_turns = 0, g_activeTurns = 0, g_every = 0;
bool g_haveGame = false;
#ifdef __vita__
int g_gameThread = 0;
int Self() { return sceKernelGetThreadId(); }
#else
pthread_t g_gameThread;
bool Same(pthread_t a) { return pthread_equal(a, pthread_self()) != 0; }
#endif

uint32_t Every() {
    if (!g_every) {
        const char* e = getenv("SMG_MP_PROF");
        const long n = e && *e ? atol(e) : 0;
        g_every = n >= 60 && n <= 600000 ? (uint32_t)n : 600;
    }
    return g_every;
}
}  // namespace

extern "C" uint64_t mp_prof_now_us(void) {
#ifdef __vita__
    return (uint64_t)sceKernelGetProcessTimeWide();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

extern "C" void mp_prof_add(int zone, uint64_t us) {
    if (zone >= 0 && zone < MP_PROF_ZONES) g_turnUs[zone] += us;
    if (mp_perf_on) mp_perf_add(zone, (uint32_t)us);   // (perf3: the lite counters too, for the SMG_MP_PERFLOG line)
}

extern "C" void mp_prof_count(int counter) {
    if (counter >= 0 && counter < 4) g_counts[counter]++;
}

extern "C" int mp_prof_is_game_thread(void) {
    if (!g_haveGame) return 0;
#ifdef __vita__
    return g_gameThread == Self();
#else
    return Same(g_gameThread);
#endif
}

extern "C" void mp_prof_turn(void) {
    if (!g_haveGame) {
        g_haveGame = true;
#ifdef __vita__
        g_gameThread = Self();
#else
        g_gameThread = pthread_self();
#endif
    }
    g_turns++;
    if (mp_active()) g_activeTurns++;
    for (int z = 0; z < MP_PROF_ZONES; z++) {
        g_sumUs[z] += g_turnUs[z];
        if (g_turnUs[z] > g_maxUs[z]) g_maxUs[z] = g_turnUs[z];
        g_turnUs[z] = 0;
    }
    if (g_turns < Every()) return;
    char line[480];
    int n = snprintf(line, sizeof line, "prof turns=%u active=%u", g_turns, g_activeTurns);
    for (int z = 0; z < MP_PROF_ZONES && n > 0 && n < (int)sizeof line; z++) {
        n += snprintf(line + n, sizeof line - (size_t)n, " | %s %.4f/%.3f", kNames[z], (double)g_sumUs[z] / 1000.0 / g_turns,
                      (double)g_maxUs[z] / 1000.0);
    }
    if (n > 0 && n < (int)sizeof line)
        snprintf(line + n, sizeof line - (size_t)n, " | locks/turn %.2f states %u calc %u skip %u", (double)g_counts[0] / g_turns,
                 g_counts[1], g_counts[2], g_counts[3]);
    mp_log("%s", line);
    memset(g_sumUs, 0, sizeof g_sumUs);
    memset(g_maxUs, 0, sizeof g_maxUs);
    memset(g_counts, 0, sizeof g_counts);
    g_turns = g_activeTurns = 0;
}
#endif
