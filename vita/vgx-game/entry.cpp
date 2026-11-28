// The Vita game's host thread: data folder, log, environment file, input; then port_boot()
// (which starts the renderer's thread, vitaGL and the guest) and the pad-polling loop.
//
//   ux0:data/smg/game       the converted disc (sys/, files/); an installed copy of the owner's data
//   ux0:data/smg/saves      saves
//   ux0:data/smg/log.txt    the port's log (previous session: log.txt.prev); every [vgx-vita] line
//   ux0:data/smg/env.txt    NAME=value lines put into the environment before anything starts
//                           (GXV_* renderer switches, see platform/vgx/vita/vgx_host_galaxy_vita.cpp;
//                           the game's own PETARI_* debug hooks, e.g. PETARI_STAGE=HeavensDoorGalaxy:1; PETARI_FLOW=EggStarGalaxy:1
//                           enters through the star select as a player does, PETARI_FLOW_LOG=1 logs each step: "flow:" lines,
//                           decomp/src/Game/Scene/GameScene.cpp; both unset by default)
//   ux0:data/smg/input.txt  optional scripted pad input in the desktop harness's syntax
//                           (scripts/inputs/README.md: "<t>[-<t1>]:A|B|PX=0.3|SY=1", one entry per line, # comments),
//                           OR-ed over the real pad. t is wall-clock seconds since the start of this process, or with
//                           PETARI_GAME_CLOCK=1 game seconds (60 finished frames each), which keeps a script in step
//                           with a game that the emulator runs far below 60 fps. With PETARI_SCRIPT_ANCHOR_DRAWS=<n>
//                           (needs PETARI_GAME_CLOCK=1) t = 0 is the first picture with n or more draw calls (the
//                           title screen: 200), and nothing is scripted before: the scene loads take minutes there.
//                           route24: PETARI_SCRIPT_ANCHOR=play counts the script in game turns from the first turn of play in the stage
//                           PETARI_STAGE / PETARI_FLOW / PETARI_SCRIPT_STAGE name (restarts with every new scene; GameScene.cpp, end of
//                           file; PETARI_GAME_CLOCK is not needed); PETARI_SCRIPT_LOOP=<seconds> repeats the script with that period;
//                           PETARI_SCRIPT_LOG=1 logs "script:" lines (the file's grammar and these rules: agents/route24/FORMAT.md).
//   ux0:data/smg/shadercache  vitaGL's compiled-shader cache
#include <bits/gthr.h>
#include <malloc.h>
#include <psp2/io/stat.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>
#include <reent.h>

#include "../runtime/input.h"
#include "port/resume.h"
#include "settings_runtime.h"
#include "title_art_runtime.h"
#include "port/game_progress.h"
#include "port/input_script.h"
#include "port/port.h"
#include "port/diagnostics.h"
#include "port/bootio.h"
#include "port/cpulod.h"  // (cpulod26: GXV_CPULOD)
#include "port/across.h"  // (across26: GXV_ACROSS)
#include "port/keep_awake.h"  // (keepawake33: OLED-safe VitaOS ~15 s auto-suspend policy)
#include "port/quality30.h"  // (q30-26: the Quality mode's umbrella levels)
#include "port/smoothstart.h"  // (openstall26: GXV_SMOOTHSTART)
#include "port/quality_set.h"  // (gfx26: GXV_QUALITY_SET)
#include "port/mode_keys.h"  // (deadfix27: the per-mode key table)
#include "port/perf_defaults.h"  // (pace33 PATCH8)
#include "port/settings_launch.h"  // (gfx26: ApplyQualitySetFromEnv)
#include "port/sampler_vita.h"  // (sampler30: GXV_SAMPLER)

#define SMG_DATA "ux0:data/smg"
// stdio31 (vita/vgx-game/stdio_safe.cpp, the diagnostic build only; weak: absent from the player build, which discards console output)
extern "C" void port_console_configure(void) __attribute__((weak));
extern "C" void port_fd_guard_configure(const char* fcloseLock);   // (fd_guard.cpp: GXV_FCLOSE_LOCK, strk1118)
extern "C" void port_console_exclusive(void (*fn)(void*), void* arg) __attribute__((weak));

// ---- The environment lock (13 Nov 2026). newlib's getenv / setenv / unsetenv call __env_lock / __env_unlock around every walk and
// every change of the environment array, and the SDK's newlib leaves both EMPTY (two `bx lr`). setenv reallocs the array for every new
// name and then mallocs the new "NAME=value" string, which usually lands in the block the realloc just freed: a thread inside getenv at
// that moment walks string bytes as pointers. Seen in the emulator (c93-cenC): 262 invalid reads in _findenv_r within 2 ms, 0.2 s after
// env.txt was opened, at addresses that spell "GXV_", "PTCL", "_LOO" ...; on a console the first such read is a data abort, a death at
// boot (a control run died at about 1 launch in 10 with no cause found). Start-up sets several hundred names while other threads run.
// These two definitions replace the library's (this file is always in the link, so the library's envlock member is never pulled in).
// Recursive, as newlib requires (setenv calls the walk with the lock held). The owner is the thread's own TLS register (one instruction,
// no system call: getenv is called often); a waiter naps, it never spins on another thread's core.
namespace {
std::atomic<uintptr_t> sEnvLockOwner{0};
int sEnvLockDepth = 0;                         // only touched by the owner
std::atomic<uint32_t> sEnvLockWaits{0};        // times a thread found the lock held (the "envlock:" line)
std::atomic<int> sEnvLockBypass{0};            // the self-test's control arm only (GXV_ENVLOCK_SELFTEST=2)
inline uintptr_t EnvLockSelf() {
    void* p;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(p));   // TPIDRURO: this thread's TLS block, unique per thread, never 0
    return reinterpret_cast<uintptr_t>(p);
}
}  // namespace
extern "C" void __env_lock(struct _reent*) {
    if (sEnvLockBypass.load(std::memory_order_relaxed)) return;
    const uintptr_t me = EnvLockSelf();
    if (sEnvLockOwner.load(std::memory_order_acquire) == me) { ++sEnvLockDepth; return; }
    uintptr_t expected = 0;
    bool waited = false;
    for (unsigned tries = 0; !sEnvLockOwner.compare_exchange_weak(expected, me, std::memory_order_acquire, std::memory_order_relaxed); ++tries) {
        expected = 0;
        waited = true;
        if (tries >= 32) sceKernelDelayThread(50);
    }
    if (waited) sEnvLockWaits.fetch_add(1, std::memory_order_relaxed);
    sEnvLockDepth = 1;
}
extern "C" void __env_unlock(struct _reent*) {
    if (sEnvLockBypass.load(std::memory_order_relaxed)) return;
    if (sEnvLockOwner.load(std::memory_order_relaxed) != EnvLockSelf()) return;   // (not the holder: a bypassed lock that was switched back mid-call)
    if (--sEnvLockDepth <= 0) { sEnvLockDepth = 0; sEnvLockOwner.store(0, std::memory_order_release); }
}
extern "C" unsigned port_envlock_waits(void) { return sEnvLockWaits.load(std::memory_order_relaxed); }
// GXV_ENVLOCK_SELFTEST=1 (default 0): at start-up one helper thread calls getenv in a loop while this thread sets and changes 400 scratch
// names; with the lock the emulator shows no invalid access. =2: the same with the lock BYPASSED (the control arm: the emulator shows
// invalid reads in _findenv_r; on a console this arm can crash, it is for the emulator only). One "envlock:" line either way.
namespace {
std::atomic<int> sEnvTestRun{0};
int EnvLockTestThread(SceSize, void*) {
    unsigned n = 0;
    while (sEnvTestRun.load(std::memory_order_acquire)) {
        const char* v = std::getenv("GXV_ENVLOCK_PROBE_ZZ");   // (absent: every call walks the whole array)
        n += v ? 1u : 0u;
        if ((++n & 0x3FF) == 0) sceKernelDelayThread(1);
    }
    return 0;
}
void EnvLockSelfTest() {
    const char* e = std::getenv("GXV_ENVLOCK_SELFTEST");
    const int mode = e && *e ? std::atoi(e) : 0;
    if (mode <= 0) return;
    const uint32_t waits0 = sEnvLockWaits.load(std::memory_order_relaxed);
    if (mode == 2) sEnvLockBypass.store(1, std::memory_order_relaxed);
    sEnvTestRun.store(1, std::memory_order_release);
    const SceUID t = sceKernelCreateThread("smg_envtest", EnvLockTestThread, 0x10000100, 0x4000, 0, 0, nullptr);
    if (t >= 0) sceKernelStartThread(t, 0, nullptr);
    char name[40], value[40];
    for (int round = 0; round < 3; ++round)
        for (int i = 0; i < 400; ++i) {
            std::snprintf(name, sizeof name, "GXV_ENVLOCK_SCRATCH_%03d", i);
            const int len = 4 + round * 9;   // (longer each round: setenv allocates a new string)
            std::memset(value, 'a' + (i % 26), static_cast<size_t>(len));
            value[len] = 0;
            setenv(name, value, 1);
            if ((i & 15) == 0) sceKernelDelayThread(1);
        }
    for (int i = 0; i < 400; ++i) { std::snprintf(name, sizeof name, "GXV_ENVLOCK_SCRATCH_%03d", i); unsetenv(name); }
    sEnvTestRun.store(0, std::memory_order_release);
    if (t >= 0) { sceKernelWaitThreadEnd(t, nullptr, nullptr); sceKernelDeleteThread(t); }
    sEnvLockBypass.store(0, std::memory_order_relaxed);
    port_log_summary("envlock: self-test %s: 1200 sets and 400 removals against a getenv loop on another thread; the lock was found held %u times",
                     mode == 2 ? "with the lock BYPASSED (control arm)" : "with the lock", static_cast<unsigned>(sEnvLockWaits.load(std::memory_order_relaxed) - waits0));
}
}  // namespace

extern "C" void port_boot(const char*, const char*);
extern "C" int port_vita_pin_core(int core);
extern "C" int port_vita_thread_class(int cls);
extern "C" int port_vita_core0_alone(void);   // GXV_CORE0_ALONE=1: the game thread has core 0 to itself (port/core_layout.h)
extern "C" int port_vita_thread_helper(void);
extern "C" int port_vita_core3_available(void);  // the fourth core, proven once (vita/runtime/memory.cpp; port/core_layout.h)
extern "C" void port_vgx_vita_log_memory(const char* when);
extern "C" void port_vgx_vita_thread_report(void);
extern "C" void port_splash_begin(void); // the loading screen (platform/vgx/vita/vgx_host_galaxy_vita.cpp): up from the first moments, until the title has appeared
extern "C" int port_vgx_vita_error_shown(void);  // 1 when a fatal start-up error is shown on the loading screen
extern "C" int port_vgx_vita_boot_loading(void); // keepawake33: 1 while boot splash is actively loading (kDirect/kGl, !failed, !stalled)
extern "C" int port_vgx_shader_compiling(void);  // keepawake33: 1 while sync Compile/Link or async shader worker compile is active
extern "C" int port_settings_native_open(void);  // keepawake33: 1 while native in-game Settings UI is open (MpUiSystem.cpp)
extern "C" unsigned long long port_vgx_frames(void);
extern "C" unsigned long long port_vgx_vita_rich_frame(void);
extern "C" unsigned long long port_game_frames(void);
extern "C" void port_game_progress_report(const char* why);
extern "C" void port_vgx_debug_dump(void);
extern "C" void port_debug_dump_threads(void);
extern "C" void port_vita_hang_watch_start(void);  // (hang47: GXV_HANG_WATCH, vita/runtime/memory.cpp)
// movie47: when the movie decoder was last asked for a picture (platform/src/sdk/thp_video.cpp THPVideoDecode, the game thread); read by the pad thread.
static std::atomic<int64_t> g_movieAwakeNs{0};
static int64_t g_padWallNs = 0;  // (the pad thread only: the wall clock at its last pass, 0 = not known)
extern "C" void port_movie_awake_note(void) { g_movieAwakeNs.store(port_host_time_ns(), std::memory_order_relaxed); }
extern "C" void port_host_big_boot(void);  // (heap23: GXV_HEAP_BIG, platform/src/os/os_thread.cpp)
#if GXV_PGO_GENERATE
// (the instrumented build that records the PGO profile on the console, vita/pgo/vita_pgo.cpp; absent from every other build)
extern "C" void port_pgo_start(void);
extern "C" void port_pgo_at_exit(const char* why);
#endif

// The game's JKR heap globals, reached by symbol name (see ReportHeapFailure below).
extern void (*g_jkrErrorHandler)(void*, unsigned, int) __asm__("_ZN7JKRHeap13mErrorHandlerE");
extern void* g_jkrRootHeap __asm__("_ZN7JKRHeap9sRootHeapE");
extern void* g_jkrCurrentHeap __asm__("_ZN7JKRHeap12sCurrentHeapE");
// How many `new` requests the game's allocator sent to the host allocator because the current heap was a root heap (JKRHeap.cpp).
extern unsigned g_portRootAllocs, g_portRootAllocFirst, g_portRootAllocLast;

extern "C" void port_input_set_resampler(void (*fn)(void));
extern "C" void port_input_note_resample(void);
namespace {
void LoadEnvFile(const char* path) {
    bootio::Options patience;  // (bootio: a failed open that is not "no such file" is repeated; env.txt is normally absent, which costs nothing)
    patience.missingMs = 0;
    FILE* f = bootio::FopenRetry(path, "r", "env.txt", patience);
    if (!f) return;
    // (2 KiB: a PETARI_STAGE tour of every galaxy is about 1.1 KiB on one line; a longer line was cut and the cut name crashed the stage loader)
    static char line[2048];
    int n = 0, suspect = 0, noEq = 0, refused = 0;   // (strk31: noEq / refused, lines that were dropped without a word before)
    while (std::fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s == '#' || *s == '\n' || *s == '\r' || !*s) continue;
        char* eq = std::strchr(s, '=');
        if (!eq) { ++noEq; continue; }
        *eq = 0;
        char* v = eq + 1;
        for (char* e = v + std::strlen(v); e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); *--e = 0) {}
        // (audit30: three ways a hand-edited file used to lose a setting without a word: "NAME = 1" sets a variable called "NAME ", a UTF-8 byte-order mark
        // glues itself to the first name, and a line past 2047 bytes is cut and its rest parsed as a line of its own. Counted, one log line below; the settings still apply as before.)
        const size_t nameLen = std::strlen(s);
        if (nameLen && (s[nameLen - 1] == ' ' || s[nameLen - 1] == '\t')) ++suspect;
        else if (static_cast<unsigned char>(s[0]) == 0xEF || static_cast<unsigned char>(s[0]) >= 0x80) ++suspect;
        if (std::strlen(v) >= sizeof(line) - 2 - static_cast<size_t>(v - line)) ++suspect;
        if (setenv(s, v, 1) != 0) { ++refused; continue; }   // (an empty name: "=1")
        ++n;
    }
    std::fclose(f);
    port_log("env.txt: %d variables set", n);
    // (strk31: Strikers' staging tool dropped a NAME=VALUE without a word. Here every refusal is a line: a line with no '=' and a name the system refuses are counted and logged.)
    if (noEq || refused) port_log_summary("env.txt: WARNING %d line(s) without '=' and %d line(s) with an empty or refused name were ignored: those settings did not take effect", noEq, refused);
    if (suspect) port_log_summary("env.txt: WARNING %d line(s) look wrong (a space before '=', a byte-order mark, or a line over 2047 bytes): those settings did not take effect as written", suspect);
}

PortInputEvent g_script[1024];
int g_scriptCount = 0;

// GXV_LATENCY24_PAD (latency24): the game thread polls the pad once more right before its KPADRead (wpad.cpp port_input_set_resampler), so the state the game
// reads is the pad's now and not the main thread's last poll (up to ~6 ms old, ~3 ms on average). The same poll function and the same state as the main loop
// (which holds g_padMutex from its poll to its port_input_set); a poll under 1 ms after the last one is skipped (the sample is then as fresh as one can have).
std::mutex g_padMutex;
int64_t g_padPrevNs = 0;
void PadResample() {
    std::unique_lock<std::mutex> lk(g_padMutex, std::try_to_lock);
    if (!lk.owns_lock()) return;  // (the main thread is polling right now: its state is as fresh)
    const int64_t now = port_host_time_ns();
    if (now - g_padPrevNs < 1000000) return;
    PortPadState pad{};
    port_vita_input_poll(std::clamp(float(now - g_padPrevNs) / 1000000000.0f, 0.0f, 0.1f), &pad);
    g_padPrevNs = now;
    port_input_set(0, &pad);
    port_input_note_resample();
}
bool g_gameClock = false;
bool g_anchored = false; // PETARI_SCRIPT_ANCHOR_DRAWS set
bool g_scenePlay = false; // route24: PETARI_SCRIPT_ANCHOR=play (the script's t = 0 is the first turn of play in a scene)
int g_loopMs = 0;         // route24: PETARI_SCRIPT_LOOP seconds, in ms (0: no loop)
bool g_scriptLog = false; // route24: PETARI_SCRIPT_LOG
extern "C" unsigned long long port_script_scene_turn(void);  // GameScene.cpp: 0 = not started in this scene, else turns + 1
// The script file: entries one per line (or comma separated), '#' starts a comment line.
void LoadInputScript(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return;
    static char joined[16000];
    size_t used = 0;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s == '#' || *s == '\n' || *s == '\r' || !*s) continue;
        for (char* e = s + std::strlen(s); e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); *--e = 0) {}
        const int n = std::snprintf(joined + used, sizeof(joined) - used, "%s%s", used ? "," : "", s);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(joined) - used) break;
        used += static_cast<size_t>(n);
    }
    std::fclose(f);
    if (!used) return;
    g_scriptCount = portParseInputScript(joined, g_script, 1024);
    const char* gc = std::getenv("PETARI_GAME_CLOCK");
    g_gameClock = gc && std::atoi(gc) != 0;
    const char* an = std::getenv("PETARI_SCRIPT_ANCHOR_DRAWS");
    g_anchored = g_gameClock && an && std::atoi(an) > 0;
    const char* sa = std::getenv("PETARI_SCRIPT_ANCHOR");
    g_scenePlay = sa && std::strcmp(sa, "play") == 0;
    const char* lp = std::getenv("PETARI_SCRIPT_LOOP");
    g_loopMs = lp && std::atof(lp) > 0.0 ? static_cast<int>(std::atof(lp) * 1000.0 + 0.5) : 0;
    const char* sl = std::getenv("PETARI_SCRIPT_LOG");
    g_scriptLog = sl && std::atoi(sl) != 0;
    port_log("input script %s: %d entries, clock = %s%s, loop %d ms", path, g_scriptCount,
             g_scenePlay ? "game turns from the first turn of play in the scripted stage (PETARI_SCRIPT_ANCHOR=play)"
                         : g_gameClock ? "game frames (60 per second)" : "wall clock",
             g_scenePlay && g_anchored ? " (PETARI_SCRIPT_ANCHOR_DRAWS ignored)" : "", g_loopMs);
}

// Which game heap ran out?  HeapMemoryWatcher's JKR error callback panics without saying.  The entry loop below puts this
// reporter in front of the game's handler (JKRHeap::mErrorHandler, reached by its symbol name so no game header is needed):
// it logs the failed request and what the heap, the root heap and the current heap still have free, then lets the game panic.
void (*g_previousJkrHandler)(void*, unsigned, int) = nullptr;
int HeapVirtual(void* heap, int slot) { // ARM EABI: slots 0/1 destructors, 2 callAllDisposer ... 15 do_getFreeSize, 17 do_getTotalFreeSize
    void** vtable = *reinterpret_cast<void***>(heap);
    return reinterpret_cast<int (*)(void*)>(vtable[slot])(heap);
}
void ReportHeapFailure(void* heap, unsigned size, int align) {
    const uintptr_t* w = static_cast<const uintptr_t*>(heap);
    port_log("HEAP FULL: request 0x%x bytes align %d in heap %p; words %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", size, align,
             heap, (unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3], (unsigned)w[4], (unsigned)w[5], (unsigned)w[6], (unsigned)w[7],
             (unsigned)w[8], (unsigned)w[9], (unsigned)w[10], (unsigned)w[11]);
    SceKernelThreadInfo ti{};
    ti.size = sizeof(ti);
    sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti);
    port_log("HEAP FULL: in thread \"%s\" (id 0x%x); code addresses found on its stack (nearest first; resolve with nm -C):", ti.name, (unsigned)sceKernelGetThreadId());
    {
        const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(__builtin_frame_address(0));
        char line[256];
        int used = 0, found = 0;
        for (int i = 0; i < 1024 && found < 40; ++i) {
            const uintptr_t v = sp[i];
            if (v >= 0x81000100u && v < 0x81700000u) {
                used += std::snprintf(line + used, sizeof(line) - used, " %08x", (unsigned)v);
                if (++found % 8 == 0) { port_log("HEAP FULL:   %s", line); used = 0; }
            }
        }
        if (used) port_log("HEAP FULL:   %s", line);
    }
    void* const heaps[3] = {heap, g_jkrRootHeap, g_jkrCurrentHeap};
    const char* const names[3] = {"failed heap", "root heap", "current heap"};
    for (int i = 0; i < 3; ++i) {
        if (!heaps[i]) continue;
        port_log("HEAP FULL: %s %p: free 0x%x, total free 0x%x", names[i], heaps[i], (unsigned)HeapVirtual(heaps[i], 15), (unsigned)HeapVirtual(heaps[i], 17));
    }
    if (g_previousJkrHandler) g_previousJkrHandler(heap, size, align);
}
} // namespace


// PETARI_MEMBENCH=1: a memory test that needs no game (the game thread loses about half of its time to cache misses in the
// offline cache model; this measures what a miss costs on the console, and whether the page size matters). Random pointer
// chase through 32-byte lines (dense: latency against working set), through one line per 4 KiB page (the translation
// buffer: latency against page count), and block read / copy bandwidth, in the memory the game's arenas use (USER_RW)
// and in physically contiguous memory.
namespace membench {
uint64_t NowUs() { return sceKernelGetProcessTimeWide(); }
struct Block { SceUID uid = -1; uint8_t* base = nullptr; uint32_t size = 0; };
bool Alloc(const char* name, unsigned type, uint32_t size, Block& b) {
    b.uid = sceKernelAllocMemBlock(name, type, size, nullptr);
    if (b.uid < 0) { port_log("membench: %s: allocation of %u KiB failed (0x%X)", name, size / 1024, (unsigned)b.uid); return false; }
    void* base = nullptr;
    sceKernelGetMemBlockBase(b.uid, &base);
    b.base = static_cast<uint8_t*>(base);
    b.size = size;
    memset(b.base, 0, size);  // (touch every page once)
    return true;
}
// A single cycle through `nodes` nodes `spacing` bytes apart (Sattolo's shuffle); node i holds the address of its successor.
// With spacing >= 4096 the node's offset inside its page varies, so the lines do not all fall in one cache set.
double Chase(uint8_t* base, uint32_t nodes, uint32_t spacing, uint32_t steps) {
    std::vector<uint32_t> order(nodes);
    for (uint32_t i = 0; i < nodes; ++i) order[i] = i;
    uint32_t seed = 12345u;
    for (uint32_t i = nodes - 1; i > 0; --i) {
        seed = seed * 1664525u + 1013904223u;
        const uint32_t j = (seed >> 8) % i;
        std::swap(order[i], order[j]);
    }
    auto addr = [&](uint32_t idx) { return base + size_t(idx) * spacing + (spacing >= 4096 ? ((idx * 96u) & 4064u) : 0u); };
    for (uint32_t i = 0; i < nodes; ++i) *reinterpret_cast<uint8_t**>(addr(order[i])) = addr(order[(i + 1) % nodes]);
    uint8_t* p = addr(order[0]);
    for (uint32_t i = 0; i < nodes; ++i) p = *reinterpret_cast<uint8_t**>(p);  // (one lap to settle the caches)
    const uint64_t t0 = NowUs();
    for (uint32_t i = 0; i < steps; i += 8) {
        p = *reinterpret_cast<uint8_t**>(p); p = *reinterpret_cast<uint8_t**>(p); p = *reinterpret_cast<uint8_t**>(p); p = *reinterpret_cast<uint8_t**>(p);
        p = *reinterpret_cast<uint8_t**>(p); p = *reinterpret_cast<uint8_t**>(p); p = *reinterpret_cast<uint8_t**>(p); p = *reinterpret_cast<uint8_t**>(p);
    }
    const uint64_t t1 = NowUs();
    __asm__ volatile("" : : "r"(p) : "memory");  // (keeps the timed chase: the first version stored p in a pointer nobody read, the compiler removed the whole loop and every latency came out 0.0)
    return double(t1 - t0) * 1000.0 / steps;
}
void Run(const char* label, Block& b) {
    char line[512];
    int n = std::snprintf(line, sizeof line, "membench[%s] dense chase, KiB:ns", label);
    for (uint32_t kib = 8; kib <= 16384 && kib * 1024 <= b.size; kib *= 2) {
        const double ns = Chase(b.base, kib * 1024 / 32, 32, 4u << 20);
        n += std::snprintf(line + n, sizeof line - n, " %u:%.1f", kib, ns);
    }
    port_log("%s", line);
    n = std::snprintf(line, sizeof line, "membench[%s] one line per 4 KiB page, pages:ns", label);
    for (uint32_t pages = 8; pages <= 4096 && uint64_t(pages) * 4096 <= b.size; pages *= 2) {
        const double ns = Chase(b.base, pages, 4096, 4u << 20);
        n += std::snprintf(line + n, sizeof line - n, " %u:%.1f", pages, ns);
    }
    port_log("%s", line);
    // bandwidth: sum of 32-bit words over 8 MiB, and memcpy of 4 MiB, a few laps each
    const uint32_t span = std::min<uint32_t>(b.size / 2, 8u << 20);
    uint32_t* w = reinterpret_cast<uint32_t*>(b.base);
    uint32_t sum = 0;
    uint64_t t0 = NowUs();
    for (int lap = 0; lap < 8; ++lap)
        for (uint32_t i = 0; i < span / 4; i += 8) sum += w[i] + w[i + 1] + w[i + 2] + w[i + 3] + w[i + 4] + w[i + 5] + w[i + 6] + w[i + 7];
    uint64_t t1 = NowUs();
    const double readMBs = 8.0 * span / 1048576.0 / ((t1 - t0) / 1e6);
    t0 = NowUs();
    for (int lap = 0; lap < 8; ++lap) memcpy(b.base + span, b.base, span);
    t1 = NowUs();
    const double copyMBs = 8.0 * span / 1048576.0 / ((t1 - t0) / 1e6);
    port_log("membench[%s] read %.0f MiB/s, memcpy %.0f MiB/s (%u)", label, readMBs, copyMBs, sum & 1u);
}
void All() {
    port_log("membench: start (core clock %d MHz)", scePowerGetArmClockFrequency());
    struct Kind { const char* name; unsigned type; uint32_t size; } kinds[] = {
        {"USER_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 32u << 20},
        {"USER_MAIN_PHYCONT_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, 16u << 20},
        {"USER_RW again", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 32u << 20},
    };
    for (Kind& k : kinds) {
        Block b;
        if (!Alloc(k.name, k.type, k.size, b)) continue;
        Run(k.name, b);
        sceKernelFreeMemBlock(b.uid);
    }
    port_log("membench: done");
}
}  // namespace membench

// How fast the ARM core really runs. Under PSVshellPlus (a per-title profile gives 500 MHz) the system keeps answering 444,
// so the number to read is this one: a loop of one decrement-and-branch pair is one cycle per iteration on the Cortex-A9, so
// iterations per microsecond = MHz (VitAdhoc's console read 496 for a 500 MHz profile). Best of three, because anything that
// interrupts the loop only makes it look slower. About 12 ms in all; only used at start (and with GXV_CLOCK_PERIOD, below).
static unsigned MeasureArmMHz() {
    const uint32_t iterations = 2000000;
    double best = 0.0;
    for (int lap = 0; lap < 3; ++lap) {
        uint32_t n = iterations;
        const uint64_t t0 = sceKernelGetProcessTimeWide();
        __asm__ volatile("1: subs %0, %0, #1\n\tbne 1b" : "+r"(n) : : "cc");
        const uint64_t t1 = sceKernelGetProcessTimeWide();
        if (t1 > t0) best = std::max(best, static_cast<double>(iterations) / static_cast<double>(t1 - t0));
    }
    return static_cast<unsigned>(best + 0.5);
}

// One short lap (~1 ms): good to a few MHz, cheap enough to repeat while the game runs.
static unsigned MeasureArmMHzQuick() {
    double best = 0.0;
    for (int lap = 0; lap < 4; ++lap) {
        uint32_t n = 250000;
        const uint64_t t0 = sceKernelGetProcessTimeWide();
        __asm__ volatile("1: subs %0, %0, #1\n\tbne 1b" : "+r"(n) : : "cc");
        const uint64_t t1 = sceKernelGetProcessTimeWide();
        if (t1 > t0) best = std::max(best, 250000.0 / static_cast<double>(t1 - t0));
    }
    return static_cast<unsigned>(best + 0.5);
}
// The clock can change after start (a PSVshellPlus profile is applied when the game starts or resumes; the menu can change it
// at any time): a low-priority thread measures it every 30 s and logs a line when it moved by more than 10 MHz, and the first
// time. GXV_CLOCK_WATCH=0: off.
static std::atomic<bool> g_inPortBoot{false};         // keepawake33: true while port_boot() is running (covers GXV_VITA_SPLASH=0)
static std::atomic<bool> g_compileAwakeBoot{true};    // keepawake33: GXV_COMPILE_AWAKE snapshot before port_boot()
static int ClockWatchThread(SceSize, void*) {
    // A lap on a busy core can be cut by the game's threads and read low, so the clock is the best of 8 laps over ~2 s, and a
    // change is logged only when two such readings in a row agree on it.
    unsigned last = 0, pending = 0;
    for (;;) {
        if (!last) {
            // keepawake33: slice the initial 20 s wait into 1 s ticks while port_boot() runs so even
            // GXV_VITA_SPLASH=0 boots (e.g. GXV_VITA_SEED=copy) cannot hit VitaOS ~15.2 s auto-suspend.
            int waited = 0;
            while (waited < 20 || g_inPortBoot.load(std::memory_order_acquire)) {
                sceKernelDelayThread(1000 * 1000);
                ++waited;
                if (g_inPortBoot.load(std::memory_order_acquire) && g_compileAwakeBoot.load(std::memory_order_relaxed) &&
                    !port_vgx_vita_error_shown())
                    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
            }
        } else {
            sceKernelDelayThread(30 * 1000 * 1000);
        }
        unsigned mhz = 0;
        for (int i = 0; i < 8; ++i) { mhz = std::max(mhz, MeasureArmMHzQuick()); sceKernelDelayThread(250 * 1000); }
        const bool moved = !last || mhz > last + 15 || mhz + 15 < last;
        if (moved && (!last || (pending && (pending > mhz ? pending - mhz : mhz - pending) <= 15))) {
            port_log_summary("clocks: the ARM core measures %u MHz now (system reports %d)", mhz, scePowerGetArmClockFrequency());
            last = mhz; pending = 0;
        } else {
            pending = moved ? mhz : 0;
        }
    }
    return 0;
}
// 14 Nov (the NFS Most Wanted Vita 1.6 idea): GXV_FREEZE_BUTTON (default 0 since rel30; was 1): holding L + R + Select for 3 s closes the game with a core
// dump (ux0:data/psp2core-*.psp2dmp, the stacks of every thread) and a "FATAL: freeze button" line in the basic log, so a player can report a
// freeze with where each thread stood. A thread of its own at a low priority reads the pad every 100 ms (the game's own pad reads are not
// touched); a hang of the game, the front end or the renderer does not stop it. 0: no thread.
static int FreezeButtonThread(SceSize, void*) {
    const unsigned want = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT;
    unsigned heldMs = 0;
    for (;;) {
        sceKernelDelayThread(100 * 1000);
        SceCtrlData d{};
        if (sceCtrlPeekBufferPositive(0, &d, 1) <= 0) { heldMs = 0; continue; }
        heldMs = (d.buttons & want) == want ? heldMs + 100 : 0;
        if (heldMs >= 3000) {
            port_log_summary("FATAL: freeze button (L + R + Select held 3 s): the game closes with a core dump (ux0:data/psp2core-*.psp2dmp) for the report");
            port_log_flush();
            __builtin_trap();
        }
    }
    return 0;
}
// rel30 (25 Nov): default 0 in the release. L + R + Select must never be bound (AGENTS.md: the owner's rule; console plug-ins use that chord).
// GXV_FREEZE_BUTTON=1 in env.txt turns the freeze button on for a tester who wants it.
static void StartFreezeButton() {
    const char* e = std::getenv("GXV_FREEZE_BUTTON");
    if (!(e && *e == '1')) return;
    const SceUID t = sceKernelCreateThread("smg_freezebutton", FreezeButtonThread, 191, 0x2000, 0, 0, nullptr);
    if (t >= 0) sceKernelStartThread(t, 0, nullptr);
}
static void StartClockWatch() {
    const char* e = std::getenv("GXV_CLOCK_WATCH");
    if (e && *e == '0') return;
    const SceUID t = sceKernelCreateThread("smg_clockwatch", ClockWatchThread, 191, 0x4000, 0, 0, nullptr);
    if (t >= 0) sceKernelStartThread(t, 0, nullptr);
}

// GXV_CLOCK_KEEP (forkcheck23; from Producdevity's fork, a525f4b's KeepClocks; read once, default 0): the clocks the start-up
// requests got (0: not asked for, refused, or left to a clock plugin's profile). An application's clocks can be back at the
// console's defaults after it slept (the clock plugins check theirs again for this reason); the game would then run at the default
// clocks until it was restarted. With GXV_CLOCK_KEEP=1 the main loop compares the clocks the system reports with these every 5 s and asks again
// for any that dropped (CPU: 500, then 444 if 500 is refused, as at start); the first 8 times are logged. Only a clock this game
// asked for and got is kept, so a plugin's profile (the CPU not asked for) and a refused request are left alone. Default 0: the
// fork could not check a real sleep either; the clock watch's "clocks: the ARM core measures" lines show whether the clock drops.
static std::atomic<int> sKeptArm{0}, sKeptBus{0}, sKeptGpu{0}, sKeptXbar{0};
static void KeepClocks() {
    const int wantArm = sKeptArm.load(std::memory_order_relaxed), wantBus = sKeptBus.load(std::memory_order_relaxed);
    const int wantGpu = sKeptGpu.load(std::memory_order_relaxed), wantXbar = sKeptXbar.load(std::memory_order_relaxed);
    const int arm = scePowerGetArmClockFrequency(), bus = scePowerGetBusClockFrequency(), gpu = scePowerGetGpuClockFrequency(),
              xbar = scePowerGetGpuXbarClockFrequency();
    if (arm >= wantArm && bus >= wantBus && gpu >= wantGpu && xbar >= wantXbar) return;
    if (arm < wantArm && scePowerSetArmClockFrequency(wantArm) < 0 && wantArm > 444) scePowerSetArmClockFrequency(444);
    if (bus < wantBus) scePowerSetBusClockFrequency(wantBus);
    if (gpu < wantGpu) scePowerSetGpuClockFrequency(wantGpu);
    if (xbar < wantXbar) scePowerSetGpuXbarClockFrequency(wantXbar);
    static unsigned logged = 0;
    if (logged < 8) {
        ++logged;
        port_log_summary("clocks: were CPU %d MHz, bus %d MHz, GPU %d MHz, GPU xbar %d MHz (the game had %d, %d, %d, %d); asked again: now %d, %d, %d, %d "
                         "(GXV_CLOCK_KEEP)", arm, bus, gpu, xbar, wantArm, wantBus, wantGpu, wantXbar, scePowerGetArmClockFrequency(),
                         scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
    }
}
static bool ClockKeepOn() {
    static const bool on = [] { const char* e = std::getenv("GXV_CLOCK_KEEP"); return e && *e && std::atoi(e) != 0; }();
    return on;
}
static void NoteArmClockGot(int requested) {
    const int reported = scePowerGetArmClockFrequency();
    sKeptArm.store(reported >= requested ? requested : (reported >= 444 && requested > 444 ? 444 : 0), std::memory_order_relaxed);
}

// GXV_CLOCK_DEFER (forkB, 6 Nov; from Producdevity's fork, a525f4b; default 1): PSVshellPlus applies a title's profile as the game
// comes to the front, 0.25-0.45 s after the process starts (CPU, then GPU and crossbar, then a lock on the CPU). A game request that
// overlaps this can put the core back at 444 MHz and the lock keeps it there for the whole run (fork's console: 1 low launch in 40
// with the request at start, 0 in 92 deferred; a request forced into the window: 5 of 5 low). So the CPU request is made by a
// one-shot thread at 1 s of process time, and only when the core then measures below 470 MHz (no profile, or one that did not
// apply): 500, and 444 if 500 is refused. Bus, GPU and crossbar stay at start. GXV_CLOCK_DEFER=0: the CPU request at start as before.
static int DeferredArmClockRequest(SceSize, void*) {
    const uint64_t elapsed = sceKernelGetProcessTimeWide();
    if (elapsed < 1000000) sceKernelDelayThread(static_cast<SceUInt>(1000000 - elapsed));
    const unsigned pre = MeasureArmMHz();
    int arm = 0, fallback = 0;
    // GXV_STOCK_SIM=1 (13 Nov; test only, default 0): behave like a console with no clock/core plugin: ask for exactly 444 MHz even when a
    // plugin profile has the core faster, and keep everything off the fourth core (GXV_CORE3=0 is forced at start). Every console run so far
    // had ~494 MHz and four cores; players without the plugin get 444 MHz and three. The "clocks:" line shows what the core then measures.
    static const bool stockSim = [] { const char* e = std::getenv("GXV_STOCK_SIM"); return e && *e && std::atoi(e) != 0; }();
    if (stockSim) {
        arm = scePowerSetArmClockFrequency(444);
        port_log_summary("clocks: GXV_STOCK_SIM=1: asked for 444 MHz (rc=0x%08X); the ARM core measured %u MHz before and %u MHz now", static_cast<unsigned>(arm), pre,
                         MeasureArmMHzQuick());
        return sceKernelExitDeleteThread(0);
    }
    if (pre < 470) {
        arm = scePowerSetArmClockFrequency(500);
        if (arm < 0) fallback = scePowerSetArmClockFrequency(444);
        NoteArmClockGot(arm < 0 ? 444 : 500);  // (GXV_CLOCK_KEEP)
    }
    port_log_summary("clocks: 1 s into the process the ARM core measures %u MHz (%s; arm_rc=0x%08X fallback_rc=0x%08X); now %u MHz", pre,
                     pre < 470 ? "asked for 500" : "a profile is in place: no request", static_cast<unsigned>(arm),
                     static_cast<unsigned>(fallback), MeasureArmMHzQuick());
    return sceKernelExitDeleteThread(0);
}

// ---- 2.3: silent exits made visible. Before, an abort() (an assert, std::terminate, a library error) closed the game
// with no line in the log and no core dump (abort exits the process normally). Now it writes "FATAL: abort()" with the
// caller's address to the basic log and stops on an undefined instruction, so the system writes a psp2core dump with the
// stack (ux0:data/psp2core-*.psp2dmp) that a player can send. GXV_ABORT_TRAP=0: the old plain exit.
extern "C" void __real_abort(void) __attribute__((noreturn));
extern "C" __attribute__((noreturn, noinline)) void __wrap_abort(void) {
    void* caller = __builtin_return_address(0);
    port_log_summary("FATAL: abort() called from %p (game text at 0x81000000 in the ELF; the core dump that follows has the stack)", caller);
    port_log_flush();
#if GXV_PGO_GENERATE
    port_pgo_at_exit("abort");
#endif
    const char* trap = std::getenv("GXV_ABORT_TRAP");
    if (!trap || *trap != '0') __builtin_trap();
    __real_abort();
}
// vitaGL's collector failure and other fail-stops call sceKernelExitProcess(1) directly: log it, and stop with a dump
// when the code is not 0 (the game's own clean exits use 0).
extern "C" int __real_sceKernelExitProcess(int res);
extern "C" void port_card_write_flush(unsigned waitMs);  // (hitch30: platform/src/os/os_alarm.cpp, the card-write queue)
extern "C" int __wrap_sceKernelExitProcess(int res) {
    port_log_summary("%s: sceKernelExitProcess(%d) called from %p", res ? "FATAL" : "exit", res, __builtin_return_address(0));
    if (res == 0) port_card_write_flush(3000);  // (hitch30: a clean exit: what is queued for the card is written first)
    port_log_flush();
#if GXV_PGO_GENERATE
    port_pgo_at_exit(res ? "exit-error" : "exit");
#endif
    const char* trap = std::getenv("GXV_ABORT_TRAP");
    if (res != 0 && (!trap || *trap != '0')) __builtin_trap();
    return __real_sceKernelExitProcess(res);
}
static void Port23AtExit(void) {
    port_log_summary("exit: the game called exit() (not a crash by itself; the line before says why if anything did)");
    port_card_write_flush(3000);  // (hitch30)
    port_log_flush();
}
static void Port23Terminate() {
    port_log_summary("FATAL: std::terminate (an uncaught C++ exception or a broken noexcept)");
    port_log_flush();
    __builtin_trap();
}
extern "C" int port_thp_bench(const char* path);  // (platform/src/sdk/thp_video.cpp)

extern "C" int port_fast_tls_status(void) __attribute__((weak));
extern "C" void port_fast_tls_env(void) __attribute__((weak));
extern "C" const char* port_fast_tls_reason(void) __attribute__((weak));  // (imp45: which layout check failed)
extern "C" int port_fast_tls_delta(void) __attribute__((weak));           // (the slot's measured offset from the thread pointer)
extern "C" int port_vita_game_entry(int argc, char** argv) {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(SMG_DATA, 0777);
    sceIoMkdir(SMG_DATA "/saves", 0777);
    port_log_file(SMG_DATA "/log.txt");
    std::set_terminate(Port23Terminate);
    atexit(Port23AtExit);
    if (!PORT_RUNTIME_DIAGNOSTICS)
        port_log_summary("Super Mario Galaxy Vita, basic log: frame rate and hitches every 5 s, picture-size changes, settings, errors; built %s %s",
                         __DATE__, __TIME__);
    port_log("kind=native_galaxy_vgx; build %s %s", __DATE__, __TIME__);
    // libstdc++ locks its mutexes and condition variables only when pthread_cancel is linked (__gthread_active_p); the link line
    // requires it (vita/vgx-game/CMakeLists.txt). One line so that a log shows it.
    port_log_summary("threads: libstdc++ locking %s", __gthread_active_p() ? "active" : "INACTIVE (std::mutex does nothing)");
    LoadEnvFile(SMG_DATA "/env.txt");
    {   // tls40: vita/vgx-game/fast_tls.c (initialised by main before this thread did anything)
        if (port_fast_tls_env) port_fast_tls_env();
        const int st = port_fast_tls_status ? port_fast_tls_status() : -1;
        const int delta = port_fast_tls_delta ? port_fast_tls_delta() : 0;
        if (st == 3)
            port_log_summary("fasttls: off (layout check failed: %s; slot 0x89 at thread pointer %+d)", port_fast_tls_reason ? port_fast_tls_reason() : "?", delta);
        else
            port_log_summary("fasttls: %s (slot 0x89 at thread pointer %+d)", st == 1 ? "on (__getreent, pte_osTlsGetValue)" : st == 2 ? "off (GXV_FAST_TLS=0)" : "not linked", delta);
    }
    port_log_summary("envlock: active (getenv / setenv / unsetenv are serialized; the SDK's own lock is empty)");
    EnvLockSelfTest();   // (GXV_ENVLOCK_SELFTEST, default 0)
    port_sampler_boot();   // (sampler30: GXV_SAMPLER=1 starts the on-device sampling profiler; unset, this is one getenv)
    // gfx26 (12 Nov): GXV_QUALITY_SET=<0|1|2|auto> (platform/include/port/quality_set.h), the umbrella for the picture-quality switches. Default 0 (nothing
    // changes). 1 = Performance's crisp-at-near-zero-cost subset, 2 = Quality's best, auto = the Picture mode's own; GXV_QUALITY_SET_PROVEN=1 leaves out the
    // members that have not run on a console. A member written in env.txt always wins. The level is applied after the Settings start-up (the mode is
    // known there; settings_launch.cpp ApplyQualitySet) and overwrites the defaults set below for the members env.txt did not set, so the snapshot of
    // env.txt's keys is taken here, before any default. One "qualityset:" basic-log line says what was written and what was kept.
    port::qualityset::SnapshotUser([](const char* k) { return getenv(k); });
    port::modekeys::SnapshotUser([](const char* k) { return getenv(k); });   // (deadfix27: env.txt's keys of the per-mode table, before any default)
    port::perfdefaults::SnapshotUser([](const char* k) { return getenv(k); });   // (pace33 PATCH8: env.txt's umbrella keys and members, for the Performance mode's GXV_CPULOD / GXV_ACROSS defaults)
    // cpulod26 (12 Nov): GXV_CPULOD=<0..2> (default 0) is the one key for the CPU level-of-detail cuts (platform/include/port/cpulod.h). It fills in, with
    // overwrite 0, the members of the earlier cuts that the environment has not set (so it must come before their defaults below); the three new cuts
    // (GXV_MATLOD, GXV_FAMPOSE, GXV_PIECETHIN) read the level themselves. A member written in env.txt, even as 0, always wins. A "cpulod:" line says what it did.
    {
        const char* cl = getenv("GXV_CPULOD");
        const int lv = port::cpulod::ClampLevel(cl && *cl ? atol(cl) : 0);
        if (lv > 0) {
            int filled = 0;
            for (int i = 0; i < port::cpulod::kMemberCount; ++i) {
                const port::cpulod::Member& m = port::cpulod::kMembers[i];
                const char* cur = getenv(m.name);
                if (!m.existing || (cur && *cur)) continue;
                char buf[32];
                snprintf(buf, sizeof buf, "%g", static_cast< double >(m.lv[lv]));
                setenv(m.name, buf, 0);
                port::cpulod::NoteFanned(i);   // (q30-26: the Quality mode rewrites what this wrote)
                ++filled;
            }
            const char* fl = getenv("GXV_FARLOGIC");
            if (lv >= 2 && !(fl && *fl)) {
                setenv("GXV_FARLOGIC", "2", 0);  // (level 2 only: farlogic25's far ambient + chaser families at half pace, a time dilation)
                // (fmt32: far logic paces actors by their distance from THIS console's camera and player, so it must not run in a synchronised multiplayer session: port_farlogic_level()
                // answers 0 while port_net_session_lockstep() says a session runs, platform/include/port/lockstep.h; the value written here is kept for single player)
            }
            port_log_summary("cpulod: GXV_CPULOD=%d: %d switches of the earlier cuts filled in from the level%s (the new cuts read the level themselves)", lv, filled,
                             lv >= 2 && !(fl && *fl) ? ", GXV_FARLOGIC=2" : "");
        }
    }
    // across26 (12 Nov): GXV_ACROSS=<0..2> (default 0) is the one key for the cuts every stage can use (platform/include/port/across.h: the silhouette gate with its occlusion
    // query, the unread indirect captures, the shadow stencil). Like GXV_CPULOD it fills in, with overwrite 0, the members the environment has not set (so it comes before their
    // defaults below); a member written in env.txt, even as 0, always wins; a member whose value at the level is 0 is left alone. One "across:" line says what it wrote.
    {
        const char* al = getenv("GXV_ACROSS");
        const int lv = port::across::ClampLevel(al && *al ? atol(al) : 0);
        if (lv > 0) {
            char said[160] = "";
            int at = 0, filled = 0;
            port::across::FanOut(lv, [](const char* name) { const char* cur = getenv(name); return cur && *cur; },
                [&](const char* name, float v) {
                    if (v == 0.0f) return;
                    for (int mi = 0; mi < port::across::kMemberCount; ++mi) if (std::strcmp(port::across::kMembers[mi].name, name) == 0) port::across::NoteFanned(mi);   // (q30-26)
                    char b[32];
                    snprintf(b, sizeof b, "%g", static_cast< double >(v));
                    setenv(name, b, 0);
                    ++filled;
                    if (at < static_cast< int >(sizeof said) - 40) at += snprintf(said + at, sizeof said - at, " %s=%s", name, b);
                });
            port_log_summary("across: GXV_ACROSS=%d: %d switches filled in from the level:%s (a switch set in env.txt keeps its own value)", lv, filled, said);
        }
    }
    // dive27 (13 Nov): GXV_REHEARSE=<0..2> (default 0; PROPOSED 1 in both modes after one console run): every conditional effect path the stage can enter is drawn once in a warm-up scope behind the
    // loading wipe (platform/include/port/rehearse.h, decomp Game/System/PortRehearse.cpp: the underwater camera filter, the wet ocean forms, both glow chains; other first-use paths join the registry).
    // 2 also fills in GXV_PREWARM=1 and GXV_LAYOUTWARM=1 (effects and layouts, never run on the console yet) unless the environment has set them. Census: "rehearse:" a scene; "dive27:" lines (GXV_DIVE_LOG=1,
    // default 1) at every wet edge: what the 3 s after it paid in links, targets and uploads.
    // GXV_WATER_HALFCAP (0, 1, or perf = Performance only; default 0): the underwater filter's capture at half size (chosen when the stage's filter is made). GXV_WATER_BLOOMFOLD (0/1/2, default 0): the
    // fold level of the underwater normal bloom only (Beach Bowl, Ocean Ring). PROPOSED Performance: HALFCAP=perf, BLOOMFOLD=2; Quality: neither.
    {
        const char* rl = getenv("GXV_REHEARSE");
        if (rl && atol(rl) >= 2) {
            setenv("GXV_PREWARM", "1", 0);
            setenv("GXV_LAYOUTWARM", "1", 0);
        }
    }
    setenv("GXV_REHEARSE", "0", 0);
    setenv("GXV_DIVE_LOG", "1", 0);
    setenv("GXV_WATER_HALFCAP", "0", 0);
    setenv("GXV_WATER_BLOOMFOLD", "0", 0);
    // openstall26 (12 Nov): GXV_SMOOTHSTART=<0..2> (default 0) is the one key for the first 20 s after a stage's screen opens (platform/include/port/smoothstart.h): level 1 fills in
    // GXV_PREWARM_RES=2, GXV_TEXWARM=1, GXV_ENDWAIT_LOG=1 and GXV_SMOOTHSTART_MS=400, level 2 GXV_TEXWARM=2 and 600. Like GXV_ACROSS it only fills in members the environment has not set
    // (so it comes before their defaults below); a member written in env.txt, even as 0, always wins. One "smoothstart:" line a stage at +20 s.
    {
        const char* sl = getenv("GXV_SMOOTHSTART");
        const int lv = port::smoothstart::ClampLevel(sl && *sl ? atol(sl) : 0);
        if (lv > 0) {
            char said[160] = "";
            int at = 0, filled = 0;
            port::smoothstart::FanOut(lv, [](const char* name) { const char* cur = getenv(name); return cur && *cur; },
                [&](const char* name, const char* v) {
                    setenv(name, v, 0);
                    ++filled;
                    if (at < static_cast< int >(sizeof said) - 40) at += snprintf(said + at, sizeof said - at, " %s=%s", name, v);
                });
            port_log_summary("smoothstart: GXV_SMOOTHSTART=%d: %d switches filled in from the level:%s (a switch set in env.txt keeps its own value)", lv, filled, said);
        }
    }
#if GXV_PGO_GENERATE
    port_pgo_start();  // (GXV_PGO_DUMP_S from env.txt is read here)
#endif
    port_vita_hang_watch_start();  // (hang47: GXV_HANG_WATCH=<seconds>, default off; vita/runtime/memory.cpp)
    // Version 1.1 player defaults: Mario's eyes (final TEV byte wrap), the Settings menu and button
    // remapping, the truthful loading bar, and the NEON/faster audio mixing kernels incl. the resampler and AFC decoder
    // ported from Producdevity's fork (bit-identical PCM to the
    // scalar code; they run on the game thread). Set only when env.txt has not: NAME=0 turns one off.
    // Steady 30 (the fixed 30 ceiling, dynamic quality aimed at 30, full-size effects) is a Settings choice.
    for (const char* name : {"GXV_SETTINGS", "GXV_REMAP", "GXV_BOOTGUARD", "GXV_DSPNEON", "GXV_DSPFIR", "GXV_DSPRAMP",
                             "GXV_DSPRESAMPLE", "GXV_DSPAFC"})
        setenv(name, "1", 0);
    setenv("GXV_TEVWRAP", "2", 0);  // alpha-only wrap: the eye repair without wrapping RGB
    // forkcheck23 (8 Nov; from Producdevity's fork, 4c7d7e4): a THP movie keeps 6-8 frames of sound in hand, so a frame read late no
    // longer leaves a block of silence in the opening cutscenes (MoviePlayerSimple.cpp). PETARI_MOVIE_INHAND=0: one frame a turn as before.
    setenv("PETARI_MOVIE_INHAND", "1", 0);
    // GXV_CLOCK_KEEP (3.0.1 default 1; GXV_CLOCK_KEEP=0 disables): re-applies any app-requested clocks that VitaOS
    // dropped back to 333/111/111 MHz after sleep/suspend (leaves PSVshellPlus profiles and refused clocks untouched).
    setenv("GXV_CLOCK_KEEP", "1", 0);
    // bootio (11 Nov; platform/include/port/bootio.h): TEST ONLY. n = the first n opens of sys/fst.bin fail artificially, to show the retry on a console.
    setenv("GXV_BOOTIO_INJECT", "0", 0);
    // polish23 (8 Nov; vita/vgx-game/settings_runtime.cpp): the in-game Settings open. Its memory is reserved before the game is held, one
    // display buffer (2 MiB of video memory) when two do not fit, else the game's own front buffer (GXV_SETTINGS_FRONTBUF=0: never that);
    // a request that cannot open yet is tried again (1, 2, 4, then every 8 s) for GXV_SETTINGS_RETRY_S seconds; a system quit request
    // is honoured only after GXV_SETTINGS_QUIT_MS without any Settings request or parameter (GXV_SETTINGS_QUIT=0: never left that way).
    setenv("GXV_SETTINGS_FRONTBUF", "1", 0);
    setenv("GXV_SETTINGS_RETRY_S", "180", 0);
    setenv("GXV_SETTINGS_QUIT", "0", 0);
    setenv("GXV_SETTINGS_QUIT_MS", "750", 0);
    // polish23: GXV_COPY_DROP=1 frees the renderer's EFB-copy textures made into a destroyed scene's heaps (they otherwise pile up stage
    // after stage: video memory, and every picture-size change resamples them all). The "vgx: scene destroyed: EFB-copy textures" line
    // is written either way. heap23 (9 Nov): on by default (the review: the drop runs on the front end before the next scene's first
    // command, the back end defers the render texture's storage until the GPU is done (DeleteTex -> DeleteGpuTex) and detaches it from the
    // pooled objects; a copy made again at the same address gets a new texture). GXV_COPY_DROP=0: kept, as before.
    setenv("GXV_COPY_DROP", "1", 0);
    // heap23 (9 Nov; platform/include/port/host_big.h, platform/src/os/os_thread.cpp): the C heap was cut into ~45,000 pieces by stage
    // after stage of big blocks (decoded vertices, texture levels, upload copies ...) mixed with long-lived small ones, and a 1,359 KiB
    // `new` found no room with 15 MiB free (c20 soak, 7th stage). GXV_HEAP_BIG=1: from the first scene change on, a host `new` of
    // GXV_HEAP_BIG_KIB (128) KiB or more gets its own memory block outside the C heap while GXV_HEAP_BIG_FLOOR_MIB (16) MiB of main
    // memory stay free (else the C heap, as before); freed ones are kept up to GXV_HEAP_BIG_CACHE_MIB (8) MiB for reuse; a request the
    // C heap cannot meet is taken outside down to GXV_HEAP_BIG_LAST_MIB (4) before the heap is called exhausted. 2: from start-up.
    // 0: off (everything in the C heap, as before). A "heap: scene N destroyed" basic-log line at every scene change (with GXV_HEAP_BIG=0
    // too): the C heap, its largest free piece and each part's change since the last scene destroyed.
    setenv("GXV_HEAP_BIG", "1", 0);
    setenv("GXV_HEAP_BIG_KIB", "128", 0);
    setenv("GXV_HEAP_BIG_FLOOR_MIB", "16", 0);
    setenv("GXV_HEAP_BIG_CACHE_MIB", "8", 0);
    setenv("GXV_HEAP_BIG_LAST_MIB", "4", 0);
    // heap23b (9 Nov): the c24 30-minute soak's slow growth of the game's `new` (+0.9-1.4 MiB per loop of 7 galaxies) and of the C heap's
    // pieces. GXV_SL_SCENEDROP=1: the front end's state-list store (material lists parsed once and replayed, keyed by list address, and
    // otherwise only cleared at 16,384 entries) frees the entries of lists in a destroyed scene's heaps with that scene; 2: every entry;
    // 0: kept, as before. Exact (a freed entry is parsed again at its next call, as after the store's own clear). The "vgx: scene
    // destroyed:" line gains the store's entries and KiB kept and freed whatever the switch says.
    // Console re-soak on c27 (9 Nov): with 1 the store still grew ("state lists K kept" 1500 -> 3697 over 3 loops, +~1 MiB of C heap a
    // loop): most lists are outside the destroyed scene's two heaps. With 2 (every entry freed at a scene's destruction) the same soak is
    // flat: Tamakoro C heap in use 60170 / 60273 / 60212 / 60217 KiB over 4 loops, largest free piece 25.7-26.5 MiB. So 2.
    setenv("GXV_SL_SCENEDROP", "2", 0);
    // texage24 (10 Nov): GXV_TEX_SCENEDROP, the guest-texture cache at a scene's destruction (platform/vgx/src/vgx_texcache.cpp). 0: kept (the
    // c35 session slowdown: live textures 370 -> 1,590 over 15 visits of a two-stage loop); 1: only those in the destroyed heaps (freed
    // nothing there); 2: also every texture no frame of the scene bound (cured the slowdown, but 647 -> 1,986 in-play uploads in 18 minutes);
    // 3: by age (idle through GXV_TEX_SCENEDROP_AGE=2 scene destructions in a row) and capped (GXV_TEX_SCENEDROP_MAX=700 entries,
    // GXV_TEX_SCENEDROP_MAXKB=14336), a scene under GXV_TEX_SCENEDROP_MINFRAMES=30 pictures in play never ages anything. PROPOSED default 3,
    // pending the console A/B (REPORT of texage24); "vgx: scene destroyed:" and "tex-census-5s:" count the textures freed and decoded again.
    // GXV_TEX_ADOPT (default 0, not set here): a new entry whose bytes equal a live idle texture's takes that texture instead of an upload.
    setenv("GXV_TEX_SCENEDROP", "3", 0);
    // j3n1 (10 Nov): GXV_J3DNATIVE (platform/vgx/src/vgx_j3native.inc, decomp J3DPortNative.hpp): the game brackets each material / differed list
    // call with its versions and the front end can skip parsing it when they match (exact: probation, a scrub, a hash cross-check). 0: off,
    // as before (default, nothing is bracketed); 1: census only (the bracket is sent and the classes and front-end ms are logged, nothing
    // is skipped); 2: verify (every bracket is also checked both ways, within a budget); 3: on. j3n2: 4 stage-2 verify (chains of recorded
    // list calls; every register patch compared with the normal call), 5 stage 2 on (patches). Exact on the host tests; 3 and 5 measured
    // to cost more front-end instructions than they save on the host model, so 0 stays the default.
    setenv("GXV_J3DNATIVE", "0", 0);
    // drawpipe24 (10 Nov): GXV_DRAWPIPE (platform/include/port/draw_pipe.h): the first pieces of a pipelined game thread (the draw side of
    // a turn on another core beside the next update). 0: off, as before (default). 2: the census / verify run: pictures unchanged; one
    // drawn turn in GXV_DRAWPIPE_SAMPLE is hashed at four points and the "drawpipe:" lines name what the update that would overlap the
    // draw writes of what the J3D draw passes read, and count the draw lists that still read live game state. 1: the J3D passes draw
    // from the lists frozen at the hand-over (no gain yet: the draw stays on the game thread; run 2 first, "differed" must be 0).
    // dpengine (11 Nov): 3 = the whole serial draw on the draw thread while the game thread waits (no gain: proves the thread, the GX
    // owner hand-off and the guest-OS rules); 4 = stage A, the game thread records the draw's GX calls, the draw thread executes them
    // and draws the J3D passes from the frozen frame beside update() (needs GXV_GXCAPTURE=1 GXV_GXCAPTURE_SYNC=1 GXV_DRAWDONE_DEFER=4;
    // without them every turn is serial and the "drawpipe:" line says "configuration"); 1 = mode 4's data flow rehearsed on the game thread. Run 2, then 1,
    // then 3, then 4 (agents/dpengine/REPORT.txt). The thread: GXV_DRAWPIPE_CORE (1), GXV_DRAWPIPE_PRIO (0 = one step below the game
    // thread's: under the back end and the game's audio thread on core 1), GXV_DRAWPIPE_WATCHDOG_MS (a join longer than this turns the
    // pipeline off for the session), GXV_DRAWPIPE_SLICE_US (the join's timed wait), GXV_DRAWPIPE_SETTLE (turns that must qualify in a
    // row), GXV_DRAWPIPE_STACK_KB, GXV_DRAWPIPE_ALWAYS.
    setenv("GXV_DRAWPIPE", "0", 0);
    setenv("GXV_PIPE25", "0", 0);  // (pipe25/27: 1 = late draw on a worker, 2 = verify; needs GXV_DRAWDONE_DEFER=4, GXV_GXCAPTURE off, GXV_DRAWPIPE unset; platform/include/port/pipe25.h)
    setenv("GXV_PIPE25_CORE", "1", 0);
    setenv("GXV_PIPE25_PRIO", "0", 0);  // (0 = the game thread's own priority)
    setenv("GXV_PIPE25_JOINLATE", "0", 0);  // (pipe61: 1 = the helper is joined after calcAnim(); prove with GXV_PIPE25=2 first)
    setenv("GXV_PIPE25_TAKEOVER_US", "1500", 0);
    setenv("GXV_PIPE25_STACK_KB", "1024", 0);
    setenv("GXV_PIPE25_SCRATCH_KB", "1024", 0);
    setenv("GXV_DRAWPIPE_SAMPLE", "4", 0);
    setenv("GXV_DRAWPIPE_CORE", "1", 0);
    setenv("GXV_DRAWPIPE_PRIO", "0", 0);
    setenv("GXV_DRAWPIPE_WATCHDOG_MS", "200", 0);
    setenv("GXV_DRAWPIPE_SLICE_US", "1000", 0);
    setenv("GXV_DRAWPIPE_SETTLE", "8", 0);
    setenv("GXV_DRAWPIPE_STACK_KB", "1024", 0);
    setenv("GXV_DRAWPIPE_LOADCHECK", "1", 0);  // (0: a guest thread other than main / audio having run does not make the turn serial)
    setenv("GXV_DRAWPIPE_ALWAYS", "0", 0);  // (mode 3 only: 1 = every drawn turn's draw on the draw thread, whatever the game's state)
    setenv("GXV_DRAWPIPE_TRAP", "1", 0);  // (mode 4: the GX library's owner trap during an overlap, when the capture layer has it; 0 = not set)
    // dpcapture (10 Nov): GXV_GXCAPTURE (platform/include/port/gx_capture.h, platform/src/gx/gx_capture.cpp): the GX library's capture
    // and replay, the open draw lists' half of the pipelined game thread. 0: off (default; the library is today's). 1: on (captures
    // open only where the draw pipeline or the self-test asks). 2: verify (every recorded call also runs at once; the record is replayed
    // into a scratch stream and compared; the "gxcap:" line counts differences). GXV_GXCAPTURE_SELFTEST=1 (with 1 or 2): the layer cuts
    // the game thread's GX calls into captures itself and replays each at once: no gain, the same pictures. GXV_GXCAPTURE_SYNC=1:
    // GXSetDrawSync is recorded instead of ending the capture. GXV_GXCAPTURE_SELFTEST_CALLS=N: the self-test also cuts every N calls.
    setenv("GXV_GXCAPTURE", "0", 0);
    setenv("GXV_GXCAPTURE_SELFTEST", "0", 0);
    setenv("GXV_GXCAPTURE_SYNC", "0", 0);
    setenv("GXV_GXCAPTURE_SELFTEST_CALLS", "0", 0);
    // dpengine (12 Nov): GXV_GXCAPTURE_NULL=1 (with GXV_GXCAPTURE=1): the null recorder, a measurement run. Recorded calls are
    // counted and dropped, so THE PICTURES ARE WRONG ON PURPOSE; the game thread's time in the draw is then the draw lists' own
    // walk, the floor under any recorder (platform/src/gx/gx_capture.cpp).
    setenv("GXV_GXCAPTURE_NULL", "0", 0);
    // dpengine (12 Nov): GXV_GXCAPTURE_SLOW=1: the recorder as it was before the cheap record path (one out-of-line recorder a
    // call, the thread asked of the system each time). The same records and the same pictures either way: the switch is the A/B
    // for the cheap path's gain on the console, and the fallback if the cheap path is ever in doubt.
    setenv("GXV_GXCAPTURE_SLOW", "0", 0);
    // 1.2 repairs, on by default (each has an exact host test; NAME=0 in env.txt turns one off):
    // Ghostly Galaxy's proximity floor (divide-by-zero capture), the dome's Pull Star framing, zero
    // destination alpha, safe failures for EFB/render-target allocation and rebinding, the display-list
    // cache refusing half-readable decodes, the GPU timer ring guard, and no Settings entry mid-save.
    for (const char* name : {"GXV_CLIP_CAPTURE_FIX", "GXV_ASTRO_DOME_ZOOM_GUARD", "GXV_DSTALPHA_ZERO", "GXV_EFB_CREATE_GUARD",
                             "GXV_RT_ALLOC_GUARD", "GXV_EFB_REBIND_GUARD", "GXV_DLCACHE_BOUNDS_GUARD", "GXV_GPU_TIMER_GUARD",
                             "GXV_SETTINGS_SAVE_GUARD"})
        setenv(name, "1", 0);
    // 1.2 picture and speed defaults (NAME=0 in env.txt turns one off): GX-exact indirect add-previous, constant
    // destination alpha under blending, logic-op blends, white for a switched-off texture stage; no `discard` where the
    // alpha test cannot fail (keeps the GPU's hidden-surface removal); THP movie frames on the hardware JPEG decoder
    // (checked against the software decoder on the first picture, software on any failure); the frame clock at the
    // clocks' priority; one clean A press per touch in dialogue; Square/R shakes shorter than a game update kept.
    // Shaders whose text these change are prepared by the warm-up after the title and kept in the compiled cache.
    // (vram24: GXV_THPHW is NOT in this list any more: the hardware decoder rejects THP frames (0x80650010) and every movie falls back to software decode, while
    // the switch took 8 MiB of the physically contiguous pool away from vitaGL at every start. GXV_THPHW=1 in env.txt brings it back, for the decoder's own tests.)
    // Also: dedicated small programs for exact EFB copies, and dynamic quality returning to full resolution 20 s after a
    // busy stretch (not 120 s); one pass over the lights when a lit channel's colour and alpha share their light set.
    for (const char* name : {"GXV_FIX_INDPREV", "GXV_FIX_DSTA", "GXV_FIX_LOGICOP", "GXV_FIX_TEXOFF", "GXV_NODISCARD",
                             "GXV_VBLANK_PRIO", "GXV_TAP_CLEAN", "GXV_SHAKE_LATCH", "GXV_COPYVAR", "GXV_AUTOQ_RECOVER",
                             "GXV_LTMERGE", "GXV_PALROWS", "GXV_FIX_LODBIAS", "GXV_AUTHORED_I4_MIPS"})
        setenv(name, "1", 0);
    // Square/R spin as a 20-read swing pulse that waits out the game's 10-update A/B cooldown: a spin right after a jump comes at
    // once instead of 0.33 s late (from the swing's echo), and no phantom spin follows the spin interval (GAL-030).
    setenv("GXV_SHAKE_PULSE", "1", 0);
    // The HD pack's eye textures are used again: the white / stuck-closed eyes 1.0 players saw with them came from the clamped final
    // alpha, which GXV_TEVWRAP=2 now wraps as the Wii does (checked frame by frame in the Gateway wake-up close-up). GXV_HDTEX_EYES=0
    // keeps the game's own eye images.
    setenv("GXV_HDTEX_EYES", "1", 0);
    // GX lines and points at their GX width and size (spin sparkles, rain lines, depth-of-field strips), drawn as triangles.
    setenv("GXV_FIX_LINES", "1", 0);
    // Linear fog per vertex (ortho exact; perspective within 1/255 of the per-pixel curve in the host test): the per-pixel fog
    // the Strikers GameCube port measured at about 8 ms a frame becomes one multiply-add per pixel.
    setenv("GXV_FOGVTX", "2", 0);
    // 2.3, issue 6: the vertex-fog varying on a fixed register (vgx_shadergen.h FogBindMode). 2 = "v_fogt7" on TEXCOORD7 (free in every
    // fog shader of the 2.2 package list; the TEXCOORD path is the one every other varying already uses); 1 = "v_fogc" on FOG (needs
    // the console check that libshacccg takes FOGC); 0 = the 2.0-2.2 unbound "v_fog" whose slot depended on compile history. Only the
    // fog shaders' texts change (~210 of ~2,430), so the package's other compiled shaders, its pack and lists stay valid.
    setenv("GXV_FOG_BIND", "2", 0);
    // Stream vertex and index buffers in vitaGL's per-picture pool (no heap allocation or collector work per frame).
    setenv("GXV_VITA_SCRATCH", "1", 0);
    // Pad defaults for a PS TV (GAL-032): with a DS3 or DS4 on the PlayStation profile R2 shoots Star Bits, the right stick moves the
    // pointer, the D-pad is the camera. The Vita handheld's own controls are not touched. GXV_PAD_DEFAULTS=0 in env.txt gives the 1.1 pad layout.
    setenv("GXV_PAD_DEFAULTS", "1", 0);
    // The loading screen (the title picture with the loading bar) can never sit at 99 %: if the game has been running pictures in
    // the last loading stage for 60 s without the title logo being recognised, the screen fades out and the game's own picture shows.
    setenv("GXV_BOOT_TITLE_FALLBACK", "60", 0);
    // modes26 (12 Nov): the Picture mode has two choices, Performance (60, the default) and Quality (30) (port/player_settings.h Config, mapped by Settings at start-up,
    // platform/src/settings/settings_launch.cpp; docs/TECHNICAL.md "Version 2.3 Picture Modes"). Both fix the picture at 960x544 (GXV_AUTOQ_MIN = MAX = 6); 60 aims for GXV_AUTOQ_TARGET=60
    // with no cap and the stage table's cuts, 30 is the fixed 30 cap (GXV_FPS_LIMIT=30) with every cut off (GXV_PICTURE_MODE=30). The older notes below describe the rungs the modes no longer use.
    // The Picture mode's targets (port/player_settings.h Config, mapped by Settings at start-up, platform/src/settings/settings_launch.cpp):
    //  - Quality (the default) and Locked 30: dynamic quality budgets the whole picture's work (two game turns, the renderer, the GPU)
    //    against 33 ms under the fixed 30 cap (GXV_FPS_LIMIT=30): Quality starts at 960x544 and gives size away down to the game's
    //    smallest size (GXV_AUTOQ_LOCKED30_FLOOR=0, _START=6) only to hold 30; Locked 30 never goes below 960x544 (the backend's 6 / 7);
    //  - Performance: GXV_AUTOQ_TARGET=60 (one turn, 16.7 ms), no multisampling, the present's edge smoothing as the first step under GPU
    //    load (GXV_AUTOQ_EDGESTEP=1), sizes down to 608x346 (GXV_AUTOQ_MIN below) kept only when they show more pictures, and the steady 30
    //    below (GXV_STEADY30) where the CPU cannot come near 60.
    // Without Settings (GXV_SETTINGS=0) this 30 target with up-to-60 pictures is what runs.
    setenv("GXV_AUTOQ_TARGET", "30", 0);
    // Performance mode's steady 30 (port/frame_pace.h SteadyCadence; only with GXV_AUTOQ_TARGET=60 and no fixed cap): when the pictures stay
    // under 38 a second for 3 s and the GPU is not what holds them (or nothing is left to lower), every second turn draws, an even 30 (each
    // picture 2 vblanks with GXV_PRESENT_SYNC=2) instead of a 1-2-3 vblank mix; back to up to 60 once 45 or more are estimated for 4 s.
    // GXV_STEADY30=0 in env.txt: off (the plain "Up to 60" skip rule); GXV_STEADY30_BELOW / _ABOVE move the two thresholds.
    setenv("GXV_STEADY30", "1", 0);
    // Mario first: under load the floor protection gives up MSAA, then the sizes above native, then tries
    // cheaper background mip filtering, then half-size effects and blur, and only then a smaller
    // picture; while the picture is upscaled Mario and Luigi get their own edge smoothing.
    // 1.3: the cheaper background only (3), not half-size effects: on the console they cost more renderer time than they saved GPU
    // time. Mario's edge smoothing only below 640x456 (1), not at the game's own size: at 640x456 it softened him for nothing.
    setenv("GXV_AUTOQ_FXSTEP", "3", 0);
    setenv("GXV_CHARAA_AUTO", "1", 0);  // Mario/Luigi edge smoothing only while the picture is below 640x456
    // Anisotropic filtering as the top quality step (first given up under load); the HD pack's textures trilinear.
    setenv("GXV_AUTOQ_ANISO", "1", 0);
    setenv("GXV_HDTEX_TRILINEAR", "1", 0);
    // Half precision only in fragment shaders proven to stay within one 8-bit level of the float result.
    setenv("GXV_FS_HALF", "1", 0);
    // TEV inputs A, B, C read the low 8 bits of a colour register, as on the Wii (the Lumas were white without it).
    setenv("GXV_FIX_TEVIN", "1", 0);
    // Lost quality comes back sooner: a step up after 10 s of room in a row (not 20); a failed step still blocks itself.
    setenv("GXV_AUTOQ_RECOVER_SEC", "20", 0);  // (20: fewer in-play size changes, each a 100-170 ms freeze; the recovery after a wipe climbs in play now)
    // 1.3 play-time quality (vgx_autoq.h "Play-time quality"; GPU work only, no CPU on cores 1-2; NAME=0 in env.txt turns one off,
    // GXV_AUTOQ_ANISO_RUNG=8 keeps anisotropic filtering to the top rung as before):
    //  - quality never costs pictures: a larger size or anisotropic filtering only while the GPU stays within 85 % of the picture
    //    interval the CPU sets (at 47 FPS: 18 ms, not 70 % of 33 ms), and above 640x456 a GPU that sets the pace steps back;
    //  - fades, wipes and loads lead into unmeasured areas: no step up there (r27: 880x520 taken in a fade, then 11-12 FPS in the
    //    drop-in behind it), a size above 640x456 back to it while the screen is closed; the pause menu still steps up;
    //  - the front end's own late pictures do not end a stretch of room (r27: 592x422 kept for 2.5 minutes at 55 FPS);
    //  - anisotropic filtering (no frozen picture to switch, unlike a size) from 640x456 up, not only at the top rung.
    // A severe GPU overload in play (the user's r41 drop-in at zoom 1.0: 9 FPS for 15 s at 640x456 and no reaction, because pictures
    // over 100 ms were never measured) is acted on within about a second, in one change: cheaper background, half-size effects (=2,
    // only for such an overload: FXSTEP=3 leaves them out otherwise) and as small a picture as the GPU time says. 0 in env.txt: off.
    setenv("GXV_AUTOQ_OVERLOAD", "2", 0);
    // The lowest picture size is 544x388 (rung 0), not the emergency 480x340: at 480x340 Mario and the HUD look visibly soft on
    // the 960x544 screen. GXV_AUTOQ_MIN=-1 in env.txt allows 480x340 again.
    setenv("GXV_AUTOQ_MIN", "0", 0);
    // r49 console A/B (lit Observatory, zoom 1.0): the rungs in the screen's shape (720x404 for 640x456) cost nothing (27.3 fps
    // both) and show more detail across; the mip bias puts back the texture detail of a 960-wide picture at 60 % strength.
    setenv("GXV_WIDE_RUNGS", "1", 0);
    // Console A/B on c33 (10 Nov, Honeyhive 720x404 with GXV_TEVIN_CANON=2): 100 costs nothing (EFB 3D 7.05 against 6.98 ms); sharp24's
    // host metric: SSIM .8856 -> .8923 at 720, .8769 -> .8881 at 608. So 100.
    setenv("GXV_RUNG_LODBIAS", "100", 0);
    // (smooth23, 2.3) Smoother loads and size changes, same pictures (0 in env.txt: the 2.2 way):
    //  - GXV_SHADERPACK=1: the RAM shader pack (stages.pack, one read at start-up) answers vitaGL's compiled-shader lookups;
    //    GXV_SHADERPACK_SEED=1: the package's own pack (app0:shadercache/<key>/stages.pack) when the card's folder has none (since 2.1 it
    //    never has: the package's shaders are read in place). Each stage warm-up link opened two files on the memory card before.
    //  - GXV_RUNG_RSORDER / GXV_RUNG_PREMULTI / GXV_RUNG_RSFREE=1: a change of picture size resamples into premade textures first, has one
    //    premade texture for each texture it resamples, and never draws a resample through a pooled object the game used 8 times a frame
    //    (a scene there waited for the GPU: 25-45 ms of a 40-50 ms change).
    //  - GXV_STAGEWARM_REBUILD=1: resolve outstanding room/player variants as their actual shader sources register; no full queue replay.
    setenv("GXV_SHADERPACK", "1", 0);
    setenv("GXV_SHADERPACK_SEED", "1", 0);
    //  - (cook24) GXV_LOAD_SPLIT=1: the "load-split:" log lines (what a start / a stage load spent on the disc, the decoder, the waits and the
    //    shaders; counters only, no extra work). GXV_USERPACK=0: the user shader pack (user.pack.a/.b in the card's shader folder: what this card's
    //    play compiled, written ~30 s after the title in the idle card, read in one fread at the next start; GXV_USERPACK_MAX_KB=3072 its cap,
    //    GXV_USERPACK_DELAY_S=30). Default 0 until a console A/B has shown the saving (see agents/cook24/REPORT.txt).
    setenv("GXV_LOAD_SPLIT", "1", 0);
    //  - (disc26, 12 Nov) the card in play. GXV_DVD_CHUNK_KIB=512: a long read (an archive of MiB) is done in pieces and a music-stream read goes in
    //    between (before, it queued behind the whole archive: 305 ms measured); GXV_DVD_RA_SLOTS=4: one read-ahead window per recently read file;
    //    GXV_IO_LINE=1: the "io:" line per stage (reads in play by class, who waited, the verdict; the regression check); GXV_IO_LEDGER_FILE: the
    //    files a stage read while it PLAYED are appended there as prefetch-list lines (capped at 64 KiB); GXV_IO_PREFETCH_MB=0: 3 = the learned
    //    prefetch (the list GXV_IO_PREFETCH_LIST is read ahead while the card is idle; needs RAM: off until a console run shows the heap has it).
    setenv("GXV_DVD_CHUNK_KIB", "512", 0);
    setenv("GXV_DVD_RA_SLOTS", "4", 0);
    setenv("GXV_IO_LINE", "1", 0);
    setenv("GXV_IO_LEDGER_FILE", "ux0:data/smg/ioplay.txt", 0);
    setenv("GXV_IO_PREFETCH_LIST", "ux0:data/smg/ioplay.txt", 0);
    setenv("GXV_IO_PREFETCH_MB", "0", 0);
    setenv("GXV_USERPACK", "0", 0);
    // packrefill25 (12 Nov): GXV_PACK_MISSLOG=0: 1 = one "pack-miss: <v|f>/<HH>/<HASH>.gxp stage=.. play=.. t=.. n=.." line per distinct shader text the packs did not answer (the
    //    key list a shader-pack refill tour collects; also with GXV_SHADERPACK=0) and a "pack: hits N misses M compiled-in-play K" line every 60 s. Counters only.
    setenv("GXV_PACK_MISSLOG", "0", 0);
    setenv("GXV_RUNG_RSORDER", "1", 0);
    setenv("GXV_RUNG_PREMULTI", "1", 0);
    setenv("GXV_RUNG_RSFREE", "1", 0);
    setenv("GXV_STAGEWARM_REBUILD", "1", 0);
    // (stall23, 2.3) The opening camera after a galaxy load, same pictures (0 in env.txt: before):
    //  - GXV_STAGEWARM_OWNFIRST=1: the stage queue links the stage's own programs that are not linked yet before warm draws of linked ones.
    //  - GXV_STAGEWARM_HOLD_OWN_MS=12000: the closed screen is held past GXV_STAGEWARM_HOLD_MS (5 s) while those are still not linked (each
    //    may be a shader compiler run of 0.3-1.4 s that the opening camera would otherwise freeze for), at most 12 s from the hold's start.
    //  - GXV_WARM_LIVEGUARD=1: the generic warm-up never runs in a frame that began in play (a 484 ms compile while the wipe opened).
    //  - GXV_STUTTER_SPLIT=1 / GXV_STUTTER_WAITS=1 (log only): stutter lines for frames in play are not used up by the hold's quiet
    //    frames; "the rest" is split and the frame's shader compiler runs and GPU waits are given (vita/vgx-game/wait_wrap.cpp).
    setenv("GXV_STAGEWARM_OWNFIRST", "1", 0);
    setenv("GXV_STAGEWARM_HOLD_OWN_MS", "12000", 0);
    setenv("GXV_WARM_LIVEGUARD", "1", 0);
    setenv("GXV_STUTTER_SPLIT", "1", 0);
    setenv("GXV_STUTTER_WAITS", "1", 0);
    // (firstuse24, 2.4) The first run / jump of a stage (Mario's own actions), same pictures (0 in env.txt: before):
    //  - GXV_PLAYERWARM=1: the programs a draw has to link within GXV_PLAYERWARM_WINDOW_MS (6000) of a pad input are recorded in a
    //    "player" list (same file as the stage lists) and warmed in EVERY stage's queue after its own records; GXV_PLAYERWARM_HOLD=1: those
    //    not linked yet count as "own" for the closed-screen hold (GXV_STAGEWARM_HOLD_OWN_MS); GXV_PLAYERWARM_MAX=256 records at most.
    //  - GXV_FIRSTUSE_LOG=1 (log only): "first-use:" lines for what a stage still pays in play (programs, render targets, textures, lazily
    //    read disc data, the first start of a sound id / emit of an effect on the game thread); GXV_FIRSTUSE_TURN=0: with 1 also a line
    //    for every game turn in play of GXV_FIRSTUSE_TURN_MS (40) or more with its categories (the turn timer, ~30 us a turn).
    setenv("GXV_PLAYERWARM", "1", 0);
    setenv("GXV_PLAYERWARM_HOLD", "1", 0);
    // (round 2) GXV_PLAYERFILE=1: the player list is also written to vgx_stagewarm.bin.ply GXV_PLAYERFILE_DELAY_MS (1000) after a record was added (a thread of
    // its own, temp file + rename): an app closed in play keeps it. (GXV_PLAYERSEED is gone: since seedrun31 the old built-in seed is PLYR data that
    // tools/seedrun/stagetags.py legacy puts in the packaged list; fix31 removed the setting nothing read.) GXV_FIRSTUSE_PLAYONLY=1: the title's file select
    // is not "in play" for the first-use lines (budgets are per stage).
    setenv("GXV_PLAYERFILE", "1", 0);
    setenv("GXV_PLAYERFILE_DELAY_MS", "1000", 0);
    setenv("GXV_FIRSTUSE_PLAYONLY", "1", 0);
    setenv("GXV_POOLKEEP", "1", 0); // 1: pooled render targets do not age through a stage load / closed screen (no 40-50 ms re-make in play). c180 (15 Nov): was 0
                                    //  ("untested on console"); two console passes of c178 at Performance (3128b1/b2): 6 pooled objects kept through each load, no dumps
    setenv("GXV_FIRSTUSE_LOG", "1", 0);
    setenv("GXV_ARENA_SOFT", "1", 0);   // strk26: a vertex arena that cannot grow drops that one draw (counted, logged once) instead of aborting; pure crash guard
    setenv("GXV_DL_KEEP_MIB", "0", 0);  // strk26: 0 = off; N = keep at most N MiB of idle display-list cache, evicting the oldest first (GXV_DL_KEEP_LOG=1 prints "dl-keep:")
    setenv("GXV_NAND_SAFE", "0", 0);    // strk26: 1 = save files get a .bak/.wip pair and are restored after a torn write (off until proven on the console)
    setenv("GXV_FXWARM", "0", 0);       // strk26: 1 = compile the FX depth program at boot when FXHALF/FXLOWRES/glow26 is armed (off until proven)
    setenv("GXV_HITCH_LOG", "1", 0);   // hitch24b: a "hitch:" line per picture in play over GXV_HITCH_MS (40) and a "hitch-stage:" summary on leaving a stage (log only)
    setenv("GXV_STAGEPRIME", "0", 0);  // hitch24b: 1 = the closed-screen hold is armed again for every new stage and pooled targets are kept (implies GXV_POOLKEEP); proposed 1 after a console run
    setenv("GXV_FIRSTUSE_TURN", "0", 0);
    // smooth25 (11 Nov): all default 0 / on only as noted; the console A/B lines are in agents/smooth25/REPORT.txt.
    //  GXV_STAGEBUILD_CACHE=1: the stage queue's key maps and shared-program tally are kept between builds (the 78 ms "stage queue build" hitch at every stage change);
    //    GXV_STAGEBUILD_VERIFY=1 also runs the old loop and logs whether the results are equal.
    //  GXV_POOLPRIME=1: pooled render targets the stage's effects drew into in play (learned per session) are made behind the closed screen (and kept: GXV_POOLPRIME_KEEP=1800 presents).
    //  GXV_PACE_PASSOFFER=1: the fixed 30's skipped due turn passes its picture on to the next turn (a 3-vblank hold instead of 4).
    //  GXV_PACE_GATE=1 (on): the "pace:" line also exists in the as-ready path (Performance mode) and carries the longest hold, late counts, the in-play set and the run's p99.
    setenv("GXV_STAGEBUILD_CACHE", "0", 0);
    // hitch33 (14 Nov, console logs look1 / 2300-hh-ctl30 of c106): GXV_STAGEBUILD_DEFER=1 (on; 0 = before): the stage queue's build (116-153 ms on the render thread) no longer runs in the
    //  frame the next stage is announced while the old one is still on the screen (a 127-134 ms stop at every door / launch / file-select exit, 4 of 4 transitions in the two logs); it waits
    //  for the first frame that is not live (closed wipe, hold), at most GXV_STAGEBUILD_DEFER_MAX (180) frames. "hitch33: stage <key> left: VISIBLE frames ...: links N" per stage (target 0).
    setenv("GXV_STAGEBUILD_DEFER", "1", 0);
    // hitch33 part 2 (14 Nov; the owner: "some of the black screen washes were laggy", "loading can happen during screen wipes ... but they shouldn't stutter"): GXV_WIPE_GUARD=1
    //  (on; 0 = before): a frame whose wipe / fade is MOVING (closing or opening, also when 90 % closed or more) counts as live: no stage-queue slice, generic warm-up, queue build,
    //  eviction or target making runs in it; that work waits until the wipe stands still fully closed (the closed-screen hold keeps it there). "hitch33-wipe:" per stage left.
    setenv("GXV_WIPE_GUARD", "1", 0);
    // hitch33 4a (console job 2400: the Honeyhive tour jump's queue build ran "IN A VISIBLE FRAME" after 180 frames): GXV_LOAD_BLACK=1 (on; 0 = before; 2 = also the queue's slices):
    //  a frame during a stage load whose previous picture had at most GXV_LOAD_BLACK_DRAWS (8) draws is the load's black screen; the deferred queue build runs there and a load's
    //  frames do not count towards GXV_STAGEBUILD_DEFER_MAX. And WipeRing reports a ring that finished closing as closed (it reported it open: decomp WipeRing.cpp).
    setenv("GXV_LOAD_BLACK", "1", 0);
    // hitch33 5a (console job 2503: with the wipe guard on, the Observatory's opening wipe still had 2 frames over 50 ms, worst 193 ms: the stage's own first pictures, 263 texture
    //  uploads + 2 render targets + 216 vertex buffers, and a 602 ms first game turn): GXV_WIPE_OPEN_HOLD=1 (on; 0 = before): the first opening wipe after a stage load stays fully
    //  closed until GXV_WIPE_OPEN_CALM (3) light pictures of the scene in a row have been made behind it, at most GXV_WIPE_OPEN_CAP_MS (2000). "hitch33-open:" per hold.
    setenv("GXV_WIPE_OPEN_HOLD", "1", 0);
    // hitch33 6a: GXV_WIPE_OPEN_QUEUE=1 (on; 0 = 5a): the opening wipe's hold also waits (inside its cap) for a stage queue that was left to "behind play" (a hold suppressed by
    //  GXV_NO_MIDHOLD: 64 links, 570 ms in visible frames entering Honeyhive by a scene change, c118 batch 4). And a wipe that reappears fully closed after a few frames without
    //  a report is not counted as moving (console job 2650: the first picture behind the held wipe was listed as a visible 206 ms frame; it was not seen).
    setenv("GXV_WIPE_OPEN_QUEUE", "1", 0);
    // hitch33 9: GXV_WIPE_OPEN_NOWIPE=1 (on; 0 = 6a): a stage that begins without an opening wipe (an opening camera, a scenario starter, "begin without wipe") gets the black fade
    //  closed over its first pictures and opened two turns after the hold lets go (console job 2710, Peach's castle: 702 and 974 ms frames 4.5 s after the load, no hold).
    setenv("GXV_WIPE_OPEN_NOWIPE", "1", 0);
    // hitch33 10: GXV_SHADER_CACHE_TAG=<word> (unset = as before; no default written): this run's compiled shaders go to ux0:data/smg/shadercache/<key>-<word>, a folder of
    //  its own that starts empty; the card's <key> folder is neither read nor written, the package's pack and seed are used as always (vgx_host_galaxy_vita.cpp). For test
    //  runs that must meet compiles; set GXV_STAGEWARM_FILE and GXV_SHADERLIST to files of their own with it.
    // hitch33 7 (console 2605, Beach Bowl: 9 render targets made in play, 1016 ms, four in one 154 ms frame right after a death restart; the owner's c106 session made the same
    //  240x191 + 960x544 pair three times in the Observatory): two existing switches go on, both were off only for want of a console run.
    //  GXV_POOL_TRIM=1: an idle pooled render target is deleted only while video memory is short (under GXV_POOL_TRIM_MIB 26 free), one a present; before, 60 presents (1-2 s)
    //  without use deleted it and its next use made it again in play, 22-70 ms each. GXV_POOLPRIME=1 (set above): the sizes the session's effects asked for are made ahead in
    //  closed frames; hitch33 7 adds the stage load's black frames and the opening wipe's hold to those frames (a restart, a checkpoint, a scene change without a queue hold).
    //  "hitch33-targets: stage <key> left: render targets made in VISIBLE frames N" (target 0). 0 / 0 = before.
    setenv("GXV_POOL_TRIM", "1", 0);
    // hitch33 8: GXV_LOG_LEVEL=2 (every line, as before). 1 = the player's log: the periodic census lines (about 10 lines and 3 KiB a second in play, a card write every 100 ms)
    //  are not written; start-up, settings, stage, hitch, first-use, stutter and error lines stay (platform/src/os/os_alarm.cpp). Proposed 1 for the release package; test envs
    //  keep 2. It does not stop the censuses being computed and formatted: each has its own switch.
    // log30 (25 Nov, the owner: "keep the general public logging, remove heavy logging"): the release writes the player's log (1); the night runner's env files set 2.
    setenv("GXV_LOG_LEVEL", "1", 0);
    // hitch33 part 2b: GXV_MISS_STANDIN=1 (on; 0 = before; 2 = also opaque map pieces): a draw whose program waits for a compile (the gate's spacing, or the worker) is drawn with a linked
    //  look-alike program (same vertex shader and samplers) instead of being skipped: the owner's "screen flickered black for a few seconds" (120 draws skipped over 60 pictures in a row in
    //  the Observatory, 505 over 90 in the dome). "hitch33-standin:" per stage left. Also (no switch, only acts with the worker on): the render thread never links / compiles a program
    //  that shares a text with the pair whose cache file the worker is still writing (revA item 6), and the cache probe answers "not there yet" for it.
    setenv("GXV_MISS_STANDIN", "1", 0);
    setenv("GXV_STAGEBUILD_VERIFY", "0", 0);
    // hitch25 (12 Nov): GXV_HITCH25_LOG=1 (on, log only): one "hitch25: stage <name>: first visible picture at ..." line per stage (needs GXV_TURN_PROF=1 for the turns' parts);
    //  GXV_HITCH25_TURN=1: also a line per game turn of GXV_HITCH25_TURN_MS (100) or more, with the screen's state (at most 24 a stage).
    //  GXV_STAGEBUILD_SLICE=1 (default 0): the shared-program tally the stage queue's build needs is made again in slices of GXV_STAGEBUILD_SLICE_US (1500) while the stage lists are
    //    quiet (GXV_STAGEBUILD_SLICE_QUIET_MS 1500), so the build at the stage change (104-118 ms on the render thread, c76) finds it fresh. Implies GXV_STAGEBUILD_CACHE's maps.
    setenv("GXV_HITCH25_LOG", "1", 0);
    setenv("GXV_HITCH25_TURN", "0", 0);
    setenv("GXV_STAGEBUILD_SLICE", "0", 0);
    // hitch25 (12 Nov, second part): the "hitch25: stage <name>: queued programs linked in play: n (ms) ..." line is on with GXV_HITCH25_LOG.
    //  GXV_EVICT_BACKOFF=1 (on): after a program-map trim pass that deleted nothing, the next one waits 60 frames (c69/c74/c76: "reclaim" 1.5 ms a picture, median, in every in-play window: the
    //    map sits over its limit of 900 with every program spared, and each picture counted, collected and sorted it; c54, under the limit: 0.00). Timing only; 0 = as before.
    //  GXV_STAGEQ_ROOM=1 (default 0): a stage-queue entry of the head (own records, player list, hot shared programs) that is refused for want of a program slot makes room from the
    //    queue's filler (other stages' shared programs, then the menu's), at most GXV_STAGEQ_ROOM_MAX (192) programs a stage, in quiet frames; a queue that ends at the slot limit with
    //    its head linked no longer holds the closed screen (GXV_STAGEPRIME). The programs the draws in play linked one by one (56 in a 22-stage tour, 8-40 ms each) are linked behind the wipe.
    setenv("GXV_EVICT_BACKOFF", "1", 0);
    setenv("GXV_STAGEQ_ROOM", "1", 0);
    setenv("GXV_STAGEQ_ROOM_MAX", "192", 0);
    // dead26 (12 Nov): c83 console (logs 1805 vs 1807): ROOM added closed-screen holds of 2322 / 878 / 17 ms (the stage's unlinked own programs linked behind the wipe: 173 links = 860 ms at
    //  Gusty Garden; the deletes themselves cost 2 ms). GXV_STAGEQ_ROOM_HOLD_MS=300: once the room-making has acted for a stage, the hold ends after this long (0 = no cap) and the rest of the
    //  queue goes on behind play in the small slices of GXV_NO_MIDHOLD's path (GXV_MIDHOLD_SLICE_MS a frame). GXV_STAGEQ_ROOM_STALE=1: programs no list names are evicted first (repeated),
    //  the queue's filler only after that. A stage whose unlinked queue programs fit the free slots above the reserve switches the room-making off ("stage-room:" line).
    setenv("GXV_STAGEQ_ROOM_HOLD_MS", "300", 0);
    setenv("GXV_STAGEQ_ROOM_STALE", "1", 0);
    // nohitch26 (12 Nov): GXV_ASYNC_LINK (default 0; proposed 2 after one console tour): in play, a program a draw needs that is not linked is linked at most one a picture (inside
    //  GXV_ASYNC_LINK_MS 4 ms of the picture's links), the draw meanwhile uses a linked program of the same vertex shader and samplers (2) or is skipped (1). Mario, the HUD / 2D, render-to-texture
    //  and opaque map draws still link at once and are counted. One "asynclink: stage <name>: ..." line per stage. The link stays on the render thread (vitaGL / GXM are not thread-safe);
    //  GXV_ASYNC_SHADERS=1 (separate, default 0) moves the shader compiles of cache misses to the worker.
    setenv("GXV_ASYNC_LINK", "0", 0);
    // noprog26 (12 Nov, second shift): GXV_NO_PLAY_COMPILE_STOP (PROPOSED default 1; the back end reads 1 when unset): no shader compile and no progress picture in play. A text no cache has is not
    //  compiled at its CMD_SHADER in play (1786c, Dreadnought: 9 compiles = 2.4 s in one picture, then the branded "Preparing graphics" picture, 4.35 s stop); the worker (GXV_ASYNC_SHADERS' engine,
    //  on while this is on unless GXV_ASYNC_SHADERS=0) compiles it into the card cache, the draws that need it are skipped until it is, the link is a file read (cached links are not gated); without a
    //  worker, one compile per GXV_ASYNC_LINK_MISSGAP (30) pictures, vertex shader and fragment shader apart. Mario / HUD / 2D / render-to-texture / opaque map draws still compile at once but never
    //  show the picture. Lines: "playprogress: suppressed in play", "noprog: shader ..." (first few), "noprog: stage <name>: ..." (per stage left). 0 = exactly the code before.
    //  GXV_STAGE_PRECOMPILE (default 0): the stage queue's entries whose shaders no cache has are handed to the worker at the load and behind play (GXV_STAGE_PRECOMPILE_MS 2 ms a picture), the queue
    //  links them when ready; "precompile: stage <name>: ..." census.
    setenv("GXV_NO_PLAY_COMPILE_STOP", "1", 0);
    // revA (12 Nov): the compile worker stays OFF for the first console runs (lock contention, heap fallback pools and non-atomic cache writes are unproven); with it off a
    // shader in no cache is compiled on the render thread one per 30 pictures with its draw skipped: no progress picture, no multi-second stop. Set 1 to try the worker.
    // keys27 (13 Nov): GXV_KEYS27 (default 0; proposed 1): a variant program (rejectZero / alphaDead / atPass) that is not resident is drawn with its general form and the variant is linked / compiled later (the
    //  worker compiles it, one job at a time, never on the render thread in play); "keys27:" census every 10 s. The worker is off by revA's design (lock contention in vitaGL's compiler lock, heap fallback pools,
    //  non-atomic cache writes unproven): with GXV_KEYS27=1 and GXV_KEYS27_WORKER unset or 1 it is ON unless GXV_ASYNC_SHADERS is set; GXV_KEYS27_WORKER=0 keeps it off (siblings are then only learned).
    setenv("GXV_KEYS27", "0", 0);
    {
        const char* k27 = getenv("GXV_KEYS27");
        const char* k27w = getenv("GXV_KEYS27_WORKER");
        setenv("GXV_ASYNC_SHADERS", (k27 && k27[0] && k27[0] != '0' && !(k27w && k27w[0] == '0')) ? "1" : "0", 0);
    }
    setenv("GXV_STAGE_PRECOMPILE", "0", 0);
    // asyncsh31 (17 Nov): the compile worker's allocations come from newlib's heap only (vita/vgx-game/alloc_wrap.cpp, no switch: inert while the worker does not
    //  run), so the "heap fallback pools" part of revA's reason is closed in code; the worker stays off until the console A/B in reports/asyncsh31.md.
    //  GXV_UBERSHADER (default 0): 1 = census only, nothing drawn differently: one "first-use: ubershader census:" line per stage left (draws in play whose
    //  program needed the compiler, by "the fragment ubershader would have drawn it meanwhile" or not, and how many compiled on the render thread). Drawing with
    //  the ubershader is a later stage, gated on that census and a console compile / GPU-cost measurement (platform/vgx/src/vgx_ubershader.h).
    setenv("GXV_UBERSHADER", "0", 0);
    // menutrim25 (12 Nov): GXV_STAGEQ_MENUSPLIT (default 0): the "menu list" kept on the card (968 records in c76) is the union of everything drawn while the title, the star select or the pause
    //  menu was up, the game scene behind the star select's white fade-in and the frozen 3D scene of the pause menu's first pictures, so it added 600-680 programs to EVERY stage's queue
    //  (the cause of the queue's stall at the slot limit). =1: a stage's queue takes the "always" set of it: the 2D layouts (orthographic draws: HUD, pause menu, talk windows, star get,
    //  miss, game over; recorded in every frame now, with a class tag in bits 26..27 of the record's layout word) and the package's menu list's programs (title, file select, galaxy map,
    //  pause set); the scene classes stay in the list for the start-up queue; no 3D draw of play or of the pause menu is recorded in the list any more. Old (untagged) records are left out of
    //  the stage queues. =2: as 1 and one "menusplit:" line per stage build (queue before / after, programs left out by class) and per stage left (left-out programs that a draw linked
    //  afterwards: in play, in the pause menu, in a menu scene; the first few with their shader hashes).
    setenv("GXV_STAGEQ_MENUSPLIT", "1", 0);
    setenv("GXV_POOLPRIME", "1", 0); // (hitch33 7: was 0; see GXV_POOL_TRIM below)
    // handover26 (12 Nov): GXV_PREWARM_RES (default 0; PROPOSED 2 after one console tour): the render targets Mario's and the starter's first pictures ask for (c79 tours: a 240x191 target at the
    //  camera -> starter hand-over in 22 of 22 stages, 36-128 ms in the back end; a 960x544 copy target about 10 s in, 8 of 22 stages, 52-82 ms) are made in the stage's closed-screen frames (the opening
    //  wipe's quiet frames, a load, the hold) and handed to the first copy that asks. 1 = the built-in sizes (scaled from 960x544); 2 = also the targets this stage made in play in its first 15 s, recorded
    //  per stage in GXV_PREWARM_RES_FILE and replayed at its next load. GXV_PREWARM_RES_MAX (6 targets a stage), _MIB (16 colour MiB a stage), _RESERVE_MIB (28 MiB of video memory kept free),
    //  _KEEP (900 presents an unused premade target waits after its stage's last closed frame, then it is freed). Log: "prewarm-res:" two lines a stage (screen open; 20 s later with the worst stall).
    // c180 (15 Nov): default 2 (was 0). Console, c178 Performance, two passes with it (3128b1/b2) against four without (3124b, 3127c/d/f): the
    //  Beach Bowl dip ~15 s in (a 240x136 + 60x34 bloom/water chain made in play, "render targets made" 56-58 ms; lowest 5-s window 35.7-39.4 fps)
    //  is gone (lowest 57.2 and 56.5; 6 of 6 premade taken by first use); stage hitch totals at the four shared stages 1906 / 1718 ms vs 1917-2080.
    //  A first visit takes the shipped seeds (GXV_PREWARM_RES_SEED) and the generic sizes; later visits the recorded list.
    setenv("GXV_PREWARM_RES", "2", 0);
    setenv("GXV_PREWARM_RES_FILE", "ux0:data/smg/shadercache/vgx_prewarmres.txt", 0);
    setenv("GXV_PREWARM_RES_MAX", "6", 0);
    setenv("GXV_PREWARM_RES_MIB", "16", 0);
    setenv("GXV_PREWARM_RES_RESERVE_MIB", "28", 0);
    // stall27 (13 Nov): GXV_RES_ADOPT (default 1, acts only with GXV_PREWARM_RES >= 1): a first use takes an exact-size premade target (the copy's own texture adopts the premade object, a size change / the
    //  guard path / the rung change's resamples take the premade texture); c86 tour: "taken by first use" 0 or 1 of 2-4 in 22 of 22 stages. GXV_PREWARM_RES_KEEP 5400 (was 900): the picture-size steps
    //  come 24-32 s in (1,000+ presents) and found the premade 960x544 already freed. The census: the "prewarm-res: ... first 20 s" line ("premade N, taken by first use N (...), made in play N").
    setenv("GXV_RES_ADOPT", "1", 0);
    setenv("GXV_PREWARM_RES_SEED", "1", 0); // (stall27: the stage seed list shipped in vgx_prewarm_res.h: Beach Bowl / Ocean Ring / Koopa bloom and water chains; plan sizes use the runtime rounding: 212x168, not 212x169)
    setenv("GXV_PREWARM_RES_KEEP", "5400", 0);
    // endwait26 (12 Nov): GXV_ENDWAIT_LOG (default 0): for a game-thread end-of-frame wait over GXV_ENDWAIT_MS (40) one "endwait:" line (stage clock, the front end's pictures split into parse / texture
    //  decode / display-list builds / join / passes / queue gate, the queue, what the back end finished meanwhile) and, when the released picture has run, an "endwait-be:" line (target create, texture
    //  uploads, GPU finish wait, display queue ...). GXV_TEXWARM (default 0; PROPOSED 2 after a console run of the log): 1 = textures first used in a stage's first 15 s are recorded per stage
    //  (GXV_TEXWARM_FILE) and decoded + uploaded in its closed-screen frames next time; 2 = also in the first 3 s of the open screen, 3 ms / 512 KiB a picture. Census: "texwarm:" two lines a stage.
    setenv("GXV_ENDWAIT_LOG", "0", 0);
    setenv("GXV_TEXWARM", "0", 0);
    // openstall26 (12 Nov): GXV_SMOOTHSTART (default 0; PROPOSED 1 after a console pair, see above) and its closed-time cap in ms; GXV_STAGEBUILD_FAST (default 0; PROPOSED 1): the stage queue's
    //  shared-program pass merges from a list sorted once instead of walking and sorting all candidates (c80 1803: 99-114 ms a build with the tally fresh; VERIFY=1 checks the result).
    setenv("GXV_SMOOTHSTART", "0", 0);
    setenv("GXV_SMOOTHSTART_MS", "400", 0);
    setenv("GXV_STAGEBUILD_FAST", "0", 0);
    setenv("GXV_TEXWARM_FILE", "ux0:data/smg/shadercache/vgx_texwarm.txt", 0);
    setenv("GXV_PACE_PASSOFFER", "0", 0);
    setenv("GXV_PACE_GATE", "1", 0);
    // prewarm24 (10 Nov): GXV_PREWARM: the shader programs of the effects a stage's actors set up (hit marks, breaks, pick-ups, defeats) are made
    // behind its closed screen instead of at their first emission in play (Honeyhive spin-attack hit: 683 ms, two shader compiles). Each effect is
    // drawn once through JPAResource::draw in its draw group's real GX state between PORT_GX_MARK_WARM_BEGIN / _END; the front end resolves those
    // draws (same keys) and draws nothing, and puts its registers back; the back end links them in its quiet slices; the hold waits for them.
    // 0 off (as before), 1 on (GXV_PREWARM_MS=1500 of hold at most, GXV_PREWARM_FRAME_MS=4 of the game thread a closed frame), 2 the pack tour
    // (every resource of the effect archive in every draw group, no frame budget, hold up to GXV_PREWARM_TOUR_MS=60000). GXV_PREWARM_LOG=1: a
    // "prewarm:" line per stage (resources, variants, already linked / RAM pack / loose / COMPILED, ms). 0 until the console run.
    setenv("GXV_PREWARM", "0", 0);
    setenv("GXV_PREWARM_LOG", "0", 0);
    setenv("GXV_PREWARM_MS", "1500", 0);
    setenv("GXV_PREWARM_FRAME_MS", "4", 0);
    setenv("GXV_PREWARM_HOLD", "1", 0);
    // layoutwarm24 (11 Nov): GXV_LAYOUTWARM: the same warm-up for the 2D layouts a stage loads (pause menu, star-get, miss, game over, the HUD's counters
    // and meters, message windows, information windows, the wipes ...). While the screen is closed each loaded layout is drawn once through its own
    // panes' DrawSelf (every pane, shown or not; the material's two alpha / vertex-colour cases; one glyph a text box) between the warm-up scope
    // markers, in the 2D pass's real GX state; the front end resolves those draws (same keys, same textures) and draws nothing. GXV_LAYOUTWARM_FRAME_MS=4
    // of the game thread a closed frame; the hold waits for its variants and layouts within GXV_PREWARM_MS. 0 off, 1 on. GXV_LAYOUTWARM_LOG=1: the
    // census (log only): the layouts a stage loaded, and a "layoutwarm: first draw:" line for each layout's first draw in play with what the front end
    // had to make for it (variants not in the warm-up list, shaders, textures). 0 until the console run.
    setenv("GXV_LAYOUTWARM", "0", 0);
    setenv("GXV_LAYOUTWARM_LOG", "0", 0);
    setenv("GXV_LAYOUTWARM_FRAME_MS", "4", 0);
    // (shader23, 2.3) vitaGL has 2,048 shader slots and shader objects were never deleted (c20: 1,659 after 7 first-visit galaxies; a
    // longer session gets glCreateShader = 0 and loses those draws). GXV_SHADER_RECLAIM=1: at a scene change (screen closed) the shader
    // objects no linked program uses are given back, least recently used first, down to GXV_SHADER_RECLAIM_KEEP; above
    // GXV_SHADER_RECLAIM_HIGH programs of other stages are deleted for room too; in play only if glCreateShader finds no slot. Same
    // pictures (a given-back shader is compiled again from the same text when a program needs it: the RAM pack or the card's cache).
    // 0: as before. The per-scene "heap:" line shows the slots held.
    setenv("GXV_SHADER_RECLAIM", "1", 0);
    setenv("GXV_SHADER_RECLAIM_KEEP", "1024", 0);
    setenv("GXV_SHADER_RECLAIM_HIGH", "1408", 0);
    // r52 console A/B (lit Observatory, zoom 1.0): a picture at least every 3 turns cut the longest gap from ~82 to ~67 ms at the
    // same fps and a full 60 turns/s.
    setenv("GXV_SKIP_MAXSTEP", "3", 0);
    // catchup25 (GXV_STALL_FORGIVE=<ms>, 0 = off, 80 .. 10000): after a long turn (a shader compile, a long load step) the skip rules would pay the lost
    // time back with up to 8 turns without a picture (a jump of up to 4 turns of game time after a stall of 80-250 ms; the rules already forgave longer ones,
    // 250 ms and up, with their own gap test). With a threshold, a turn that starts more than that after the one before it counts as on time: no catch-up,
    // the stall costs the stall and play goes on at the normal pace (ordinary slow frames, under 80 ms, still keep the game at full speed by skipping).
    // Also the present clock's turn stamp (it stayed 4 vblanks behind after a stall: PresentSync "misses" and a higher latency for ~6 s). Basic log:
    // "pace: stall forgiven ...". Default 0 until the console A/B (recommended: 80).
    setenv("GXV_STALL_FORGIVE", "0", 0);
    // xskip27 (13 Nov; platform/include/port/xskip.h): the accepted skip of a galaxy's opening (X = Wii A: the intro camera's, the starter rail's, the generic fast-forward).
    //  GXV_XSKIP_LOG=1: the "xskip:" lines (accepted / cover / cut / picture / control / hold / one summary a stage entry; every line carries the renderer's queue depths).
    //  GXV_XSKIP=1: acts only while a skip is open: the black cover (cinema frame) is held up to GXV_XSKIP_HOLD_MS (400) while the stage's own programs / texture and target plan /
    //  warm scope have work left (stops after GXV_XSKIP_STALL_MS (150) without progress), the renderer drains with the closed-screen budget meanwhile, and the frame-skip rule
    //  treats the turns up to GXV_XSKIP_CALM_TURNS (24) after the cut as a scene change (no catch-up). GXV_XSKIP_FF_PREDICT=1: the generic fast-forward's 12 ms loop stops when
    //  the next update would not fit instead of overrunning. Nothing happens in a stage entry without a skip (the markers only).
    setenv("GXV_XSKIP_LOG", "1", 0);
    setenv("GXV_XSKIP", "1", 0);
    setenv("GXV_XSKIP_HOLD_MS", "400", 0);
    setenv("GXV_XSKIP_STALL_MS", "150", 0);
    setenv("GXV_XSKIP_CALM_TURNS", "24", 0);
    setenv("GXV_XSKIP_FF_PREDICT", "1", 0);
    // r54 console A/B: FXAA-style edge smoothing at the present (HUD untouched) cost no fps (27.5 vs 27.4, Locked 30) and
    // smooths the jagged edges; on in every mode.
    setenv("GXV_EDGE_AA", "1", 0);
    // r61 console A/B (GXV_GPU_SCENES): the one-pass present with reads at vertex-computed coordinates and half maths costs the
    // display scene 4.5 ms with edge smoothing (FXAA pass: 8.1 ms) and 2.3 ms without (sharpened upscale: 5.3 ms).
    setenv("GXV_PRESENT_FAST", "1", 0);
    // 2.3 upscale23 (host-tested, not yet on the console): the upscaler at the present (vgx_backend_gl.cpp "the upscaler"): a picture
    // rendered below 960x544 shown through a Catmull-Rom lobe from five non-dependent reads with an anti-ringing clamp on edges, in place
    // of the edge-smoothed present for that picture; 960x544 1:1 untouched. Off until a console A/B (GXV_UPSCALE=1 in env.txt;
    // GXV_UPSCALE_SPLIT=1 shows it on the left half only, GXV_UPSCALE_NEAR=4 lets Locked 30 use 848x480 through it). GXV_UPSCALE=3 (pace60):
    // on in Performance mode only (Quality and Locked 30 as 0), the candidate default for Performance after its A/B: there every picture is
    // enlarged (720x404 and below), the upscaler costs ~+1 ms over the edge-smoothed present it replaces and its edge step saves ~3 ms.
    // stagepreset25 (agents/stagepreset25/REPORT.txt): GXV_STAGE_PRESETS=0|1|2 (default 0 = off, bit-identical to a build without it; 1 = each stage gets its preset
    // when it loads; 2 = the preset each stage WOULD get is logged and nothing changes). The table is compiled in (platform/vgx/src/vgx_stagepreset.h) and the
    // file below, one line per stage, overrides it without a rebuild. With 1 the light upscaler is the default (the two sizes are 960x544 and 848x480 shown through
    // it; it only works below 960): env.txt still wins.
    // policy26 (agents/policy26/REPORT.txt): GXV_SMOOTH60=0|1 (default 0 = off, bit-identical; proposed default for 2.3: 1). The per-stage ladder "hold 60; else one step to 848x480; else
    // an even 30 at 960x544 with the quality put back" (platform/include/port/smooth60.h): the stage's preset row says where it begins (s60_start 0 = 60, 1 = 848x480, 2 = even 30) and what it may
    // step down to (s60_848, s60_30); rows `<Stage>@848` and `<Stage>@30` hold the fallback switch sets. Judged on the pictures a second shown: under GXV_SMOOTH60_848_FPS (54) for GXV_SMOOTH60_WINDOW_S (5) s
    // -> 848x480; then under GXV_SMOOTH60_30_FPS (50) -> the even 30. Only down inside a stage, never twice within GXV_SMOOTH60_GAP_S (20, at least), GXV_SMOOTH60_MAX_CHANGES (2) a stage, first
    // GXV_SMOOTH60_SETTLE_S (10) s of a stage or mode not judged; "smooth60:" log lines say every decision. With 1 the stage presets, the light upscaler and the steady-30 machinery (which the controller
    // then owns: its own 38 / 50 rules decide nothing) are on unless env.txt says otherwise; the table is the compiled-in one and the optional file (GXV_STAGE_PRESETS_FILE) with the 2.3 rows.
    setenv("GXV_SMOOTH60", "0", 0);
    if (const char* sm = getenv("GXV_SMOOTH60"); sm && atoi(sm) != 0) {
        setenv("GXV_STAGE_PRESETS", "1", 0);
        setenv("GXV_UPSCALE", "1", 0);
        setenv("GXV_UPSCALE_LITE", "1", 0);
        setenv("GXV_STEADY30", "1", 0);
    }
    // rate30 (13 Nov 2026, platform/include/port/rate_auto.h, tests/rate_auto_test.cpp): GXV_RATE_AUTO=0|1 (default 0 = today's behaviour exactly). Performance mode's two-way rate controller: 60 (a picture every turn)
    // or an even 30 (a picture every second turn, each held two refreshes, logic still 60 turns a second), NOTHING in between. Order of defence: the governors' cuts (GXV_FE_GOV / GXV_GAME_GOV, with GXV_FARFIELD) go to
    // their maximum first; only when 60 still cannot be held does the rate drop. DOWN: GXV_RATE_DOWN_M (8) of the last GXV_RATE_DOWN_N (30) turns missed (the skip rule left the picture out as late or the back end busy),
    // or GXV_RATE_SLOW_PCT (13) % of GXV_RATE_SLOW_TURNS (180); not in the first GXV_RATE_SETTLE_S (3) s of a stage; GXV_RATE_HOLD_TURNS (10) turns after a gap of GXV_RATE_STALL_MS (50) are not judged (a hitch's debt);
    // waits at most GXV_RATE_BREAK_DOWN_MS (500) for a camera cut / closed screen. UP: every lane (game thread's drawn turn, front end, render thread, GPU, p95 over GXV_RATE_SPAN_S, 1 s) predicted under GXV_RATE_LANE_MS (14.5)
    // less GXV_RATE_MARGIN_MS (0.3), less the cuts still to take (learned gain, else GXV_RATE_GAIN_ASSUME_MS, 1.0, a level; GXV_RATE_GPU_CREDIT, 0.4, of the front end's gain for the render thread and the GPU) for
    // GXV_RATE_FIT_HOLD_S (0.5); cuts put back first (GXV_RATE_FADE_S 3), proved GXV_RATE_PROOF_S (1.5) with them; waits at most GXV_RATE_BREAK_UP_MS (1000) for a break. The even 30 is held only where its own lanes fit
    // GXV_RATE_LANE30_MS (30). A failed up (back at 30 within GXV_RATE_FAIL_S, 10) doubles the wait before the next try (GXV_RATE_RETRY_S 4 .. GXV_RATE_RETRY_MAX_S 80) and the peak window of the prediction; GXV_RATE_HALVE_S (60)
    // at 60 halves it back. GXV_RATE_GIVEBACK30=1 (default): while at 30 the governors judge against the 30 budgets and give quality back by their own rules. GXV_RATE_LOG_S (5): the "rate:" census line. A stage row's
    // rate_pin (GXV_RATE_PIN) 1 keeps the stage at 60, 2 at an even 30. Needs Performance mode and GXV_STEADY30=1 (set below); GXV_SMOOTH60 is switched off (the one-way ladder would fight it; the back end ignores it too).
    setenv("GXV_RATE_AUTO", "0", 0);
    if (const char* ra = getenv("GXV_RATE_AUTO"); ra && atoi(ra) != 0) {
        setenv("GXV_SMOOTH60", "0", 1);
        setenv("GXV_STEADY30", "1", 1);
    }
    // pace32 (13 Nov 2026, the owner's decisions of 21:30: heavy stages "Locked even 30, lowest delay"; slightly over budget: "Whatever will feel the smoothest ... create some overhead room").
    // All default 0 = today's behaviour exactly; each has its own log line (tests/pace32_test.cpp is the host model).
    //  GXV_HEADROOM_PCT=<70..100> (0 off; proposed 90): the governors and the Quality ladder aim at this share of the refresh / the pair when that is lower than their own target ("headroom:" lines).
    //  GXV_LATE_TURN=1: at an even 30 the turn without a picture starts as late as its measured idle allows, so the pad read the next picture shows is fresher ("late-turn:" line every 10 s);
    //    GXV_LATE_TURN_MARGIN_US (2500), GXV_LATE_TURN_MAX_US (12000).
    //  GXV_SPEED_GUARD_SLIP=1: a measured loss of at most GXV_SPEED_GUARD_SLIP_PM (45 per mille) is slipped for at most GXV_SPEED_GUARD_SLIP_S (10) s instead of dropping pictures ("speed: slip band:").
    //  GXV_RATE_STICKY=1 (with GXV_RATE_AUTO=1): the even 30 is left only after GXV_RATE_STICKY_PROOF_S (20) s of fit and only at a camera cut / closed screen / load. A stage row's rate_pin=2
    //    (tools/presets/stage-presets-pace32.txt) locks a heavy stage to the even 30 from its load.
    setenv("GXV_HEADROOM_PCT", "0", 0);
    setenv("GXV_LATE_TURN", "0", 0);
    setenv("GXV_SPEED_GUARD_SLIP", "0", 0);
    setenv("GXV_RATE_STICKY", "0", 0);
    // pace33 (14 Nov 2026; console c111: the re-projected 60 showed 59.9 even pictures with 99-115 ms pad to flip, 33-50 ms of it pictures waiting in queues that a fixed cadence never empties).
    //  GXV_DRAIN33=1 (default 0; port/drain33.h, tests/pace33_test.cpp): a picture that stays behind its slot is given up once, at a still moment, so the queue shortens ("drain:" line every 10 s);
    //    GXV_DRAIN33_CONFIRM (30 pictures), _GAP_MS (1000), _STILL_MS (3000). It needs the latency rule that brings L down on evidence: GXV_LATENCY24 is set to 1 when it is 0 or unset.
    //  GXV_FIF now also acts with GXV_HRW (a pair's second picture is not counted); GXV_LATE_TURN now also acts in the half-rate world's rhythm.
    setenv("GXV_DRAIN33", "0", 0);
    if (const char* dr = getenv("GXV_DRAIN33"); dr && atoi(dr) != 0) {
        const char* l24 = getenv("GXV_LATENCY24");
        if (!l24 || atoi(l24) < 1) setenv("GXV_LATENCY24", "1", 1);
    }
    // anticip32 (13 Nov 2026, the owner 22:50: "anticipate when things are going to need to scale down, like when he gets launched across a level by a star or boss fights ... scale quick to
    // prevent it happening when the action is actually going on"). platform/include/port/anticipate.h, perf.cpp "anticip32", tests/anticipate_test.cpp. All default 0 = today's behaviour exactly.
    //  GXV_ANTICIPATE=1: a moment the game announces (launch star 0.77 s before the flight, sling star 0.35 s, boss music, power-up change, a demo the map knows) asks the EXISTING engine for a
    //    little less far detail before it begins and gives it back after: Performance GXV_ANTICIPATE_DEPTH (1) cut level(s) on top of the governors' through their fader, Quality the ladder's
    //    next 2 least-visible steps as an overlay, the rate controller makes no up-switch inside it. =2: only where the spike map has seen an overload. GXV_ANTICIPATE_LEAD_MS (1000): how far
    //    ahead of a learned place; GXV_ANTICIPATE_TAIL_MS (1500), GXV_ANTICIPATE_MAX_MS (15000). "anticipate:" lines (each event, and a summary every 10 s).
    //  GXV_SPIKEMAP=1 (with GXV_ANTICIPATE): where the speed guard, a rate drop, a governor raise or a ladder step fired is remembered per stage and scenario (a 20 m cell of Mario's position,
    //    or "this event at this place") and pre-scaled on the next visit; GXV_SPIKEMAP_SAVE=1 keeps the list in ux0:data/smg/spikes.txt (a file of its own; GXV_SPIKEMAP_FILE).
    for (const char* name : {"GXV_ANTICIPATE", "GXV_SPIKEMAP", "GXV_SPIKEMAP_SAVE"}) setenv(name, "0", 0);
    setenv("GXV_STAGE_PRESETS", "0", 0);
    setenv("GXV_STAGE_PRESETS_FILE", SMG_DATA "/stage-presets.txt", 0);
    if (const char* sp = getenv("GXV_STAGE_PRESETS"); sp && atoi(sp) == 1) {
        setenv("GXV_UPSCALE", "1", 0);
        setenv("GXV_UPSCALE_LITE", "1", 0);
    }
    setenv("GXV_UPSCALE", "0", 0);
    // sharp24 (all off by default, picture-changing only when set): GXV_UPSCALE_SHARP=auto picks the lobe strength per rung
    // (0.75 at 896x512 .. 2.0 at 664x376 and below; GXV_UPSCALE_AUTO_MAX caps it, percent). GXV_RUNG_LODBIAS_EXTRA=<eighths of a mip level>
    // adds extra negative texture LOD bias on every rung (EXTRA_AF is used while anisotropic filtering is on). GXV_ANISO_MINSIZE=<texels>
    // keeps small trilinear textures off the anisotropic path; GXV_ANISO_CENSUS=1 only logs the size classes.
    setenv("GXV_RUNG_LODBIAS_EXTRA", "0", 0);
    // mario24 (11 Nov, the owner: "Mario looks a little soft at 1.0x"; agents/mario24/REPORT.txt): the player's own picture switches, all off here so the
    // picture is exactly as before until a paused-frame A/B on the console. GXV_PLAYER_LODBIAS=<hundredths of a mip level, negative = sharper> (the player's
    // mipmapped textures only), GXV_PLAYER_ANISO=0|2|3 (his trilinear textures' filter value; unset = as the rest), GXV_EDGE_AA_PLAYER=0|1|2 and
    // GXV_PLAYER_SHARPEN=<pct> (the present's pass over his rectangle: 1 leaves it out of the edge smoothing, 2 smooths it lighter, 250 = its threshold
    // in GXV_EDGE_AA_PLAYER_T), GXV_PLAYER_DOF=0 (the depth-of-field ramp starts behind him; 1 = the game's own), GXV_PLAYER_MIPVIS=1 (diagnostic: his mip
    // levels as flat colours). The "player-look:" lines in log.txt (always on, a few seconds into each scene) say what the player's picture is made of.
    for (const char* name : {"GXV_PLAYER_LODBIAS", "GXV_EDGE_AA_PLAYER", "GXV_PLAYER_SHARPEN", "GXV_PLAYER_MIPVIS"}) setenv(name, "0", 0);
    setenv("GXV_EDGE_AA_PLAYER_T", "250", 0);
    // aa25 (11 Nov; agents/aa25/REPORT.txt): GXV_EDGE_DEPTH=1|2|3: the native-size edge pass blends only where the EFB's depth has a jump
    // (polygon silhouettes, leaf and fence edges), never texture detail (faces, emblems, text); 2 = branch form, 3 = mask tinted red (check).
    // GXV_EDGE_DEPTH_T (per mille relative depth jump, 20), GXV_EDGE_DEPTH_CT (colour threshold, 50). GXV_ANAMORPHIC=1: rungs 4 and 5 are
    // 752x544 and 848x544 (full height, same pixel counts), enlarged along x alone (the upscaler's 3-read form with GXV_UPSCALE). Off here.
    for (const char* name : {"GXV_EDGE_DEPTH", "GXV_ANAMORPHIC"}) setenv(name, "0", 0);
    setenv("GXV_EDGE_DEPTH_T", "20", 0);
    setenv("GXV_EDGE_DEPTH_CT", "50", 0);
    // upscale25 (agents/upscale25/REPORT.txt): GXV_UPSCALE_LITE=1|2, only with GXV_UPSCALE on: the budget forms of the upscaler (1 = three reads at +-(S,S)
    // with the Catmull-Rom lobe and a gated clamp; 2 = sharp bilinear, one dependent read). _S: read distance (percent of a texel, 75), _SB: steepening
    // (percent, 150), _K: lobe strength as a share of the rung's auto strength (percent; 0 = 80 with the clamp, 40 without), _CLAMP=0: 3-read form without the clamp. Off here: the picture is exactly as before.
    setenv("GXV_UPSCALE_LITE", "0", 0);
    setenv("GXV_UPSCALE_LITE_S", "75", 0);
    setenv("GXV_UPSCALE_LITE_SB", "150", 0);
    setenv("GXV_UPSCALE_LITE_K", "0", 0);
    setenv("GXV_UPSCALE_LITE_CLAMP", "1", 0);
    setenv("GXV_PLAYER_DOF", "1", 0);
    // Console A/B on c33 (10 Nov, Honeyhive): 4x anisotropic filtering measured free on the console: at 960x544 EFB 3D 11.05 against
    // 11.06 ms, at 720x404 7.01 / 7.00 (2x) against 6.98; the ground's fine detail (edge energy of the screenshots) +8-16 %. So it is on
    // for good in every mode (GXV_ANISO=4; the automatic quality's anisotropic step then has nothing to give up), small textures plain.
    setenv("GXV_ANISO", "4", 0);
    setenv("GXV_ANISO_MINSIZE", "128", 0);
    setenv("GXV_ANISO_CENSUS", "0", 0);
    // hdcensus: texture magnification census (measurement mode, 0 = off, nothing runs): N = one picture in N; files ux0:data/smg/texmag-<stage>.txt
    setenv("GXV_TEXMAG_CENSUS", "0", 0);
    // lighter25 (11 Nov): GXV_STREAM_CENSUS=<n> (census only, changes nothing drawn): every n-th picture, what the GX stream to the front end is
    // made of (bytes and commands by kind, register writes and how many changed nothing, list calls by what the front end did) and where it comes
    // from (draw lists, models with their projected size) with the player's time per source: "stream-census:" lines every GXV_STREAM_CENSUS_PRINT
    // (5) counted pictures. Game side platform/src/port/perf.cpp (port/stream_census.h), front end platform/vgx/src/vgx_census.inc. 0: off.
    setenv("GXV_STREAM_CENSUS", "0", 0);
    // lighter25: GXV_ARRAYSKIP=1 (exact; 2 = verify, the "arrayskip verify:" line must say 0 differing): J3D's per-shape array-base writes that the
    // stream already holds are left out at the source (port/array_skip.h). GXV_TINYCULL=<lines> (MINOR picture change; GXV_TINYCULL_DRY=1 counts
    // only): an actor whose clipping sphere projects under that many lines of a 456-line picture is not drawn (ClippingActorHolder.cpp).
    setenv("GXV_ARRAYSKIP", "0", 0);
    // gxdedup31 (18 Nov): GXV_GX_DEDUP (platform/include/port/gx_dedup.h; the decision and why it is exact: port/gx_dedup_core.h): the GX library's
    // and J3D's top-level register writes that the front end's register already holds (no BP mask pending, nothing between that could have
    // written it: list calls, markers, a new picture forget what is known) are not written. 0: off (default until the console verify);
    // 1: on; 2: verify (everything written; in front of each write =1 would leave out an XF probe the front end checks: "gxdedup verify:
    // N checked, M differing", M must be 0). GXV_GX_DEDUP_CENSUS=<n> (count only, any GXV_GX_DEDUP): "gxdedup-census:" lines every n
    // pictures (1: 300): writes by class and group, how many are repeats, which invalidations cost what, the call sites. Refused (stays 0)
    // with GXV_REPLAY25, GXV_PIPE25, GXV_GXCAPTURE, GXV_FXMERGE or GXV_DRAWPIPE on.
    // GXV_GX_DEDUP_CLASSES=1 (BP only) / 2 (CP and XF only) / 3 (both, the default): which writes go through the gate. The gate is in the
    // game only in a build made with PORT_GX_DEDUP_BUILD=1 (otherwise these switches log that and change nothing): its check costs every
    // register write a few instructions even when off, and on the host it costs the game thread more than it saves the front end
    // (reports/gxdedup31.md), so an A/B reads both threads.
    setenv("GXV_GX_DEDUP", "0", 0);
    setenv("GXV_GX_DEDUP_CENSUS", "0", 0);
    // fedl25 (12 Nov): GXV_NOOP_FILTER (platform/vgx/src/vgx_state.cpp NoopRun): the front end takes every repeat of a BP / CP array / matrix-index / XF write
    // in one loop (BPFAST's burst took only BP runs). 0: off (default); 1: on; 2: verify (the normal path runs, "noopfilter: N checked, M mismatching"
    // must say 0). Also the FE-split slot top-state-run (GXV_FE_SPLIT): the time of the top-level state runs, one scope per run.
    setenv("GXV_NOOP_FILTER", "0", 0);
    // fe2-26 (12 Nov, second shift): GXV_FE_STATEFAST (platform/vgx/src/vgx_state.cpp StateRun): one tight loop takes each run of top-level state commands (BP, CP, XF,
    // indexed matrix loads) with the general loop's per-command tests made once, the repeat tests inlined and the same write functions for a change; stops at the first
    // command it does not take (a BP register with an effect, a cut command, any other opcode). Host: -3.8 % of the front end's instructions on a console-weighted
    // stream (tools/test_vgx_fe2.sh), 25-60 instructions saved per repeated write. 0: off (default until the console verify); 1: on; 2: verify (the repeats it leaves out are made
    // the normal way and the state digest compared: "statefast: N checked, M mismatching", M must be 0). Not used with GXV_NOOP_FILTER (its own loop), GXV_BPFAST=0,
    // GXV_FE_TWOCORE or the J3D-native hooks. Log: "workload: statefast per picture" (with GXV_WORKLOAD_PROBE=1).
    setenv("GXV_FE_STATEFAST", "0", 0);
    // feplan31 (17 Nov): GXV_FEPLAN (platform/vgx/src/vgx_state.cpp "mixed display lists: exact plans"): a display list with state commands AND draws (or a CALL_DL
    // inside, or an opcode the parser stops at), which neither the state-list cache nor the geometry cache takes and which was parsed again on every call, is
    // replayed from a plan: its state sections as state lists (SlRun, register deltas), its draws made exactly as the parse makes them, in list order; a draw
    // whose vertex size changed sends the rest of the list to the parser. 0: off (default until the console verify); 1: on; 2: verify (diagnostics builds; the
    // parse runs for real and the plan is checked against it draw by draw, State before / after: "feplan: N plans checked, M mismatching", M must be 0).
    // GXV_FEPLAN_CENSUS=1 (count only, any build): "feplan-census:" lines every 300 pictures (=<n>: every n): the lists parsed again a picture by way and by
    // barrier, their ms, what a plan could take, the eight costliest lists.
    setenv("GXV_FEPLAN", "0", 0);
    setenv("GXV_FEPLAN_CENSUS", "0", 0);
    // tinylist31 (17 Nov): GXV_TINYLIST (platform/vgx/src/vgx_tinylist.inc): a tiny display list (GXV_DL_TINY, 64 bytes) that begins with a state command
    // (J3D's small differed lists: an XF material colour / texture matrix, 32 or 64 bytes) goes to the state-list cache and is replayed while its bytes are
    // unchanged, instead of being parsed on every call (before, only padded lists beginning with a BP write went there). 0: off (default; the host shows it
    // about cost-neutral, the console census decides); 1: on; 2: verify (diagnostics builds: parsed for real, the replay checked against it, State after
    // the call: "tinylist: N replays checked, M mismatching", M must be 0). GXV_TINYLIST_CENSUS=1 (count only, any build): "tinylist-census:" lines every
    // 300 pictures (=<n>: every n): CallListBody's tiny calls a picture by content (state only by first command, state+draw, particle quad, draws), by way,
    // by call site and by the stream census's source, with ms and the calls whose bytes are those of the address's last call; the costliest addresses.
    setenv("GXV_TINYLIST", "0", 0);
    setenv("GXV_TINYLIST_CENSUS", "0", 0);
    setenv("GXV_TINYCULL", "0", 0);
    // cliprange25 (platform/include/port/cliprange.h; ClippingActorHolder.cpp, LiveActor.cpp): GXV_CLIPCENSUS=1 logs "clipcensus:" lines every ~5 s per stage
    // (actors alive / running / posed / drawn by distance band from the player and by kind, time of movement and calcAnim; 2 = also the dry counts of the
    // levers). Levers, each acting only on pure decoration (no collision, no sensor, no effects / sound / switch / rail) beyond GXV_CR_NEAR (2500) from the
    // player AND the camera: GXV_CLIPSCALE=<percent> (clip earlier than the game's far distance), GXV_FARANIM=<n> (pose every n-th turn; 2..4),
    // GXV_FARDRAW=<lines> (draw-only skip under that many lines). All 0 = off.
    setenv("GXV_CLIPCENSUS", "0", 0);
    setenv("GXV_CLIPSCALE", "0", 0);
    setenv("GXV_FARANIM", "0", 0);
    setenv("GXV_FARDRAW", "0", 0);
    // cpucuts25 (platform/include/port/cpucuts.h; ClippingActorHolder.cpp, LiveActor.cpp; all default 0 / as before, all stage-settable): GXV_DRAWCENSUS=1 logs
    // "drawcensus:" lines every ~5 s (what the drawers get by camera band x projected size x class x family, with the est. ms of each size cut; changes nothing).
    // GXV_FARDRAW_KINDS=<mask> (default 1 = pure decoration; 2 decoration with effects, 4 enemies, 8 plain map objects, 16 NPCs / animals) widens what GXV_FARDRAW
    // may take; GXV_FARDRAW_HOLD=<turns> keeps a stamp at least that long; GXV_FARDRAW_FLUSH=1 leaves a stamped far model out of the pose / material-list flush;
    // GXV_CHARPOSE=<2..4> (+ _LINES 12, _MOVE 1, _DIST 2500) poses far small nearly still enemies and items every n-th turn (logic untouched).
    setenv("GXV_DRAWCENSUS", "0", 0);
    setenv("GXV_DISTCENSUS", "0", 0);  // (far30: the distance census, port/distcensus.h; counts only)
    setenv("GXV_FARDRAW_KINDS", "1", 0);
    setenv("GXV_FARDRAW_HOLD", "0", 0);
    setenv("GXV_FARDRAW_FLUSH", "0", 0);
    // prog32b: GXV_FARDRAW_OBJECTIVE=1 (a bug fix, on; 0 = off): the far-draw lever never takes an actor a star can depend on (bosses, switches, bunnies, Toads, Rosalina, Lumas,
    // anything with a stage switch / camera controller / pointer target; port/cpucuts.h ObjectiveName)
    setenv("GXV_FARDRAW_OBJECTIVE", "1", 0);
    setenv("GXV_CHARPOSE", "0", 0);
    setenv("GXV_CPULOD", "0", 0);  // (cpulod26)
    // honey25 (12 Nov; platform/include/port/cpugov.h): GXV_CPUGOV=<0..5> (default 0 = off; stage-settable as cpu_gov; Performance mode only unless GXV_CPUGOV_ANY=1): a governor
    // for the far-draw cut above. The back end's 5-second floor window (late pictures by the stage that made them late) raises a level when a window is late
    // (GXV_CPUGOV_LATE % of its pictures, 15) and the CPU's (game thread, front end, renderer: GXV_CPUGOV_CPU %, 50, of the late ones); a level is a larger far-draw
    // size limit in lines, more classes of far actor (pure decoration, + decoration with effects, + plain map objects, + NPCs / animals, + enemies), a stamp hold of 20
    // turns and the pose flush skip. A level goes down after GXV_CPUGOV_DOWN (6) calm windows (GXV_CPUGOV_CALM %, 3), twice as long again after a lowering that
    // was followed by lateness. The "cpugov:" line shows each change and a summary every GXV_CPUGOV_LOG (6) windows. Mario, collectibles, items, switches,
    // bosses, collision objects and anything within GXV_CR_NEAR of the camera or the player are outside the rule at every level.
    for (const char* name : {"GXV_CPUGOV", "GXV_CPUGOV_ANY"}) setenv(name, "0", 0);
    // fe26 (12 Nov; platform/include/port/fegov.h): GXV_FE_GOV=<0..2> (default 0 = off; stage-settable as fe_gov; Performance mode only unless GXV_FE_GOV_ANY=1): a governor that holds
    // the renderer's front end (core 2) under GXV_FE_GOV_MS (15.5) ms a picture by raising the CPU level-of-detail level (GXV_CPULOD's table, port/cpulod.h: far models' material
    // refresh, far family poses, far star-bit thinning, the far-draw cut, the character pose rule, far particle thinning; NOT the far-logic time dilation) one step after
    // GXV_FE_GOV_UP (2) hot 5-second windows, and lowering it after GXV_FE_GOV_DOWN (6) calm windows with a headroom of GXV_FE_GOV_BAND (0.7) ms beyond the measured gain of the step;
    // twice as long again after a lowering that was followed by a hot window. A static value (the stage's, env.txt's or GXV_CPULOD's) is never relaxed: the cut takes the stricter.
    // The "fegov:" line shows each change and a summary every GXV_FE_GOV_LOG (6) windows. Mario, the HUD and near objects are outside every one of these cuts.
    for (const char* name : {"GXV_FE_GOV", "GXV_FE_GOV_ANY"}) setenv(name, "0", 0);
    // cpu2-26 (12 Nov): GXV_GAME_GOV=<0..3> (default 0 = off; stage-settable as game_gov; Performance mode only unless GXV_GAME_GOV_ANY=1): the same governor on the GAME THREAD's own
    // CPU ms a drawn turn (budget GXV_GAME_GOV_MS 15.5, _BAND 0.7, _UP 2, _DOWN 6, _LOG 6). It drives the same level (GXV_CPULOD's table, now 0..3; level 3 = the strongest look-alikes,
    // including the Performance budget's own LOD / flower-group / fur / emitter members) and the cuts read the larger of this and GXV_FE_GOV's level. The "gamegov:" line shows each change.
    for (const char* name : {"GXV_GAME_GOV", "GXV_GAME_GOV_ANY"}) setenv(name, "0", 0);
    // cpu2-26: GXV_PROTECT_SPRITES=<0|1|2> (default 0 = as merged; the owner allowed far star bits thinned at 11:09): when 1, star bits and coins (the owner's protected list) are left out of the umbrella's pose and material levers (GXV_FAMPOSE, GXV_MATLOD);
    // 2 also leaves them out of the budget's every-second-turn pose (GXV_PB_ANIM); 0 = as before.
    // revB (12 Nov, owner's protected list 10:55): default 2. Star bits and coins reach every pose / material lever above otherwise; the only cut that may take a star bit is the
    // far one-in-two GXV_PIECETHIN (draw only, Floating star bits only), which this switch does not touch.
    setenv("GXV_PROTECT_SPRITES", "2", 0);
    // horizon25: GXV_HORIZON=1 (draw-only cull: an actor whose clipping sphere lies wholly in the shadow of a solid planet is not drawn; logic, sound,
    // shadow and effects untouched), 2 = census only (logs, culls nothing), 3 = verify (nothing culled; a magenta ball is drawn at each would-be-culled
    // actor's clipping sphere with the depth test: any magenta pixel on screen means the occluder is wrong). GXV_HORIZON_SHRINK (0.9: the inscribed
    // ball of the planet's collision is shrunk by it), GXV_HORIZON_MARGIN (0.03 rad), GXV_HORIZON_MINR (500: smallest occluder, game units).
    setenv("GXV_HORIZON", "0", 0);
    // bglayer25: GXV_BGLAYER (0 off; 2 census: the far-background layer idea counted, nothing changes: a "bglayer:" / "bglayer-cam:" line (game thread) and a "bglayer-fe:"
    // line (front end) every 5 s; needs GXV_DRAWPIPE=0 for the front end's by-category counts; 1 = the layer cache itself: the far background (sky, far decoration) drawn and cached
    // every GXV_BGLAYER_PERIOD (2) pictures, re-projected for the camera's turn in between; 3 = both). Knobs (read where used; defaults here for the record): GXV_BGLAYER_D (8000: the
    // distance from the camera and the player beyond which a model qualifies), _HYST (0.85), _ERRPX (2.5: re-render when the travel error exceeds it), _ROTPX (24: re-render when the turn exceeds it),
    // _MARGIN (0.03), _MINSHAPES (12: fewer shapes in the layer: no layer), _GRIDX/_GRIDY (16/9: the backdrop's grid), _D0 (0: the depth the camera travel is corrected at), _LOG_MS (5000).
    setenv("GXV_BGLAYER", "0", 0);
    // r63/r64 console: the game's audio thread on core 1 while the main thread animates and draws (GXV_AUDIO_CPU1; load hang
    // fixed in r63, 11 clean stage loads): core-0 audio 1.0 -> 0.2 ms a turn; lit Observatory at zoom 1.0 28.4 -> 29.7 fps with
    // the FE switch reads made barrier-free (GXV_FE_KNOBS).
    setenv("GXV_AUDIO_CPU1", "1", 0);
    // r55 console A/B (vitaGL 0015/0016 in the reader archive): uniform blocks written straight into vitaGL's buffers (+0.15 fps,
    // pictures identical) and one transfer wait per scene instead of per texture level (hub arrival frame 280 -> 250 ms).
    // r69 console A/B (lit Observatory, zoom 1.0, GXV_FE_SPLIT=2): shader keys sent once per id, compact material deltas and cache
    // hints from data just loaded (fecut, exact): the front end's list time 22.8 -> 20.6 ms a picture, 28.5 -> 28.9 fps.
    setenv("GXV_FECUT_KEYID", "1", 0);
    setenv("GXV_FECUT_SLHOT", "1", 0);
    setenv("GXV_FECUT_HINT", "1", 0);
    // cache24: software prefetch along the known future (retained chain, trace records, back end); bit 1 = on. 0 until the console A/B; GXV_CACHE24_DIST = calls ahead (default 2), GXV_CACHE24_DELTA = delta lines hinted.
    setenv("GXV_CACHE24", "0", 0);
    // r76 console A/B (lit Observatory, zoom 1.0, fourth core on): six exact game-thread cuts (logiccut): work-turn CPU 15.5 -> 15.2 ms
    // a turn, 29.6 -> 29.8 fps.
    // r76/r77 (6 Nov): the Strikers 1.3 shader trims (near-exact: clamps a range analysis proves inert, unit light attenuation), the
    // exact pixel-pass cuts (no-op draws, dead clears, scissor changes that cut nothing) and, with CapUnlocker, the fourth core for
    // the audio, DSP mix and front-end finishing threads (lit Observatory zoom 1.0: 29.6 -> 30.4 fps). The trims change shader texts:
    // the package's shader pack is re-seeded with them on.
    // GXV_FS_TRIM: r88 console showed speckled drop shadows / grass decals: the trim also moved some alpha-tested keys into half
    // precision (GXV_FS_HALF). Fixed in vgx_shadergen.cpp (fstrim, 6 Nov): half keys keep their full text, only float keys are trimmed.
    // r92 console bisect: GXV_FS_TRIM_PARTS=47 (every trim part except bit 16, the final clamp) shows no speckle; with the final clamp
    // left out the SGX's output conversion does not saturate values an ulp outside 0..1 (the speckled shadows / decals). So 47.
    setenv("GXV_FS_TRIM_PARTS", "47", 0);
    for (const char* name : {"GXV_FS_TRIM", "GXV_VS_TRIM", "GXV_PX_NOOP", "GXV_PX_DEADCLEAR", "GXV_PX_SCISSOR", "GXV_CORE3", "GXV_CORE3_SPLIT",
                             "GXV_CORE3_AUDIO"}) setenv(name, "1", 0);
    // gpuwater: no `discard` for an alpha test whose failing fragments the draw's blend makes no-ops (no depth write, destination
    // factor 1 for a zero source: Beach Bowl's sea, blended foliage / effects); a failing fragment writes vec4(0) instead. Exact (host
    // test: tools/test_vgx_discard_blend_gl.sh). New fragment shader keys (one per such shader): re-seed the shader pack with it on.
    setenv("GXV_DISCARD_BLEND", "1", 0);
    // seaind: an indirect lookup whose texgen nothing else reads takes its coordinate scale (a power of two) in the vertex shader, so the
    // lookup is a non-dependent texture read (the console's GPU prefetches those). Exact (host test: tools/test_vgx_indvs_gl.sh; Beach
    // Bowl's sea). New vertex / fragment shader keys for those pairs only: re-seed the shader pack with it on.
    setenv("GXV_IND_VS", "1", 0);
    // shader23 (8 Nov), 0 until the console A/B: GXV_TEVIN_CANON=1: one TEV register outside 0..255 no longer marks every key of the state
    // (no half precision, register wrap code per pixel): a key is marked only when it reads such a register's initial value or a wrap
    // could act (exact for initial reads, near-exact for values a stage made; no new shader texts). =2 also drops the wraps of stage-made
    // values proven in 0..1 from the marked keys' text (new texts: re-seed). GXV_FS_PROJ=1: projective texture reads as texture2DProj
    // (non-dependent reads; vertex and fragment texts change: re-seed).
    // canon24b (10 Nov): GXV_TEVIN_CANON=2 is the default. Console: Honeyhive 960x544 3D 22.8 -> 11.1 ms, Good Egg 21.7 -> 14.8 ms; proof over
    // 7,565 key/mask pairs of every shipped shader list: at most 1 colour level, in rare rounding flips. The shader lists are converted to the
    // mode-2 form at load (CanonListFsKey). The package's shader pack must be refilled with this default before a release (pack23 RUNBOOK,
    // with canon24b/tools/listnames.cpp).
    // g30 Wave 6 (Shader FPS Boost, inspired by Strikers 2.0 int32/shA + Galaxy's host-verified shader suite):
    //   GXV_FS_PROJ=1: true non-dependent hardware texture2DProj(u_texM, v_texN) with VS q==0 guard (Strikers shA: +6 fps / -1.03 ms GPU on Vita).
    //   GXV_VS_NOBRANCH=1 + GXV_VS_NOBRANCH_LIGHT=0: bit-exact branchless normalise, palette clamps, and FS_PROJ q==0 guard, while keeping
    //     per-light branches so Sony's Cg compiler stays <= 52 temp regs on multi-light VS (avoids the 85-reg SGX543/Vita3K regression).
    //   GXV_FS_HALF_FOG=1: unlocks GXV_FS_HALF=1 fp16 TEV ALU on vertex-fogged 3D world shaders via normal-safe half-exact FogHalfLanes + float fog mix.
    //   GXV_FS_REGS=1 + GXV_FS_ATEARLY=1: compacts u_tev[] rows (backend gather), elides constant TEV temps, sinks projective/indirect lookups, and
    //     evaluates texture reads + TEV alpha + discard in Phase 1 before TEV colour & fog (SGX543 implicit-LOD safe, Wave 3 combined path).
    //   GXV_FS_ATFAST=1: single compare/interval alpha test without *255+0.5+floor (works inside GXV_FS_ATEARLY+GXV_FS_REGS and rejectZero).
    //   GXV_FS_ADEAD=1 + GXV_FS_NOCOLOR=1: exact dead-code elimination of unused alpha (opaque) and unused colour+fog (colour-masked shadow/depth passes).
    //   GXV_FS_TEXREUSE=1 + GXV_TEVIN_PREWRAP=1 (below): exact elimination of duplicate stage texture fetches and per-pixel dual-wrap math.
    // All individually reversible to 0 in env.txt. Changes ProfileWord() -> refill shader seed pack (tools/pack24/refill_pack.py, RELEASE-3.0-CHECKLIST A.1-A.2).
    setenv("GXV_TEVIN_CANON", "2", 0); setenv("GXV_FS_PROJ", "1", 0);
    // shader23 round 2 (both exact; new shader texts / keys: re-seed): GXV_FS_FINCLAMP=1: the final clamp only for
    // a part not already in 0..1 bit for bit (last stage clamped by its text, wrapped, or alpha replaced / unused). GXV_FS_ADEAD=1: draws
    // whose render state uses nothing of the source alpha (alpha not written, no source-alpha blend factor) take an alpha-dead variant.
    setenv("GXV_FS_FINCLAMP", "1", 0); setenv("GXV_FS_ADEAD", "1", 0);
    setenv("GXV_VS_NOBRANCH", "1", 0); setenv("GXV_VS_NOBRANCH_LIGHT", "0", 0);
    setenv("GXV_FS_HALF_FOG", "1", 0); setenv("GXV_FS_NOCOLOR", "1", 0);
    setenv("GXV_FS_REGS", "1", 0); setenv("GXV_FS_ATEARLY", "1", 0);
    setenv("GXV_FS_ATFAST", "1", 0);
    // r79 console (lit Observatory, zoom 1.0, 960x544 pinned): the drawn turn's view pass handed to a worker (par-view async) and the
    // speculative joint computation (joint15), both on the fourth core when it is proven (else core 1): game-thread CPU 13.5 -> 12.4 ms a
    // turn; 960x544 holds 2-vblank pictures (147-151 of 150), GPU busy 32.5 ms.
    setenv("GXV_PAR_VIEW", "2", 0);
    setenv("GXV_PAR_CORE", "3", 0);
    setenv("GXV_PAR_ANIM", "1", 0);
    setenv("GXV_PAR_ANIM_CORE", "3", 0);
    for (const char* name : {"GXV_LC_NATIVE", "GXV_LC_KEYSEARCH", "GXV_LC_MUTEX_FAST", "GXV_LC_FRAMETID", "GXV_LC_YIELD", "GXV_LC_PANEMEMO"}) setenv(name, "1", 0);
    // cpuhive (6 Nov, Honeyhive): exact game-thread cuts. GXV_ANIM_LOD_FREE: the flower groups' plants compute their joints once per
    // picture (nothing else reads them); GXV_PAR_ANIM_LEAN: par-anim makes jobs only for models that use them (dispatch time).
    for (const char* name : {"GXV_ANIM_LOD_FREE", "GXV_PAR_ANIM_LEAN"}) setenv(name, "1", 0);
    // flushcut (7 Nov): GXV_FLUSH_PREFETCH: the drawn turn's flushes and the view pass prefetch the models a few steps ahead (no value
    // changes, only when the data arrives). GXV_DL_STATIC (exact by argument; 0 = off until a console verify run, GXV_DL_STATIC=2, shows 0
    // differences in the "dl-static verify" part of the GXV_DRAW_PROF=2 line): materials of models that cannot change are not computed again.
    // budget (8 Nov): the Performance-mode visual budget (platform/src/port/perf.cpp, "Performance-mode visual budget"): GXV_PERF_BUDGET is
    // not set here on purpose: unset, it is ON exactly when GXV_AUTOQ_TARGET=60 and there is no GXV_FPS_LIMIT=30 (Settings' Performance mode),
    // OFF in Quality and Locked 30. GXV_PERF_BUDGET=0/1 in env.txt forces it; each part has its own switch (GXV_PB_LOD, _DECOR, _PTCL,
    // _SHADOW, _ANIM, _FUR: a value, 0 = that part off) that wins over the umbrella.
    setenv("GXV_FLUSH_PREFETCH", "1", 0);
    setenv("GXV_DL_STATIC", "0", 0);
    // cpu60 (10 Nov): GXV_DL_STATIC's repeat test compared the post-texture-matrix flag with whatever the previously listed model left in
    // j3dSys; with GXV_DL_STATIC_FLAGFIX=1 (default; only acts with GXV_DL_STATIC) it uses the model data's own flag, which is what the full
    // computation uses. GXV_DRAW_PROF=2 with GXV_DL_STATIC=1/2 prints "dl-static why": which test refuses the models (maker, player, material
    // animation, plain / other texture matrix, ...). GXV_PAR_PTCL_PAUSE / _STALL_US: after a join the game thread waited STALL_US for, the
    // particle calc runs serially for PAUSE turns (120 / 1500 = as before; scheduling only, exact).
    setenv("GXV_DL_STATIC_FLAGFIX", "1", 0);
    // game24 (10 Nov): GXV_DL_STATIC_TM=1 (default 0; only acts with GXV_DL_STATIC) lets a model with plain (mode 0) texture matrices be left out
    // of the material-list flush while the content of those matrices (info, result, texgen fields) is what the last full computation left.
    // GXV_DL_STATIC=2 GXV_DL_STATIC_TM=1 verifies it (dl-static verify: differed must be 0). Host test: tools/test_dl_static_tm.sh.
    setenv("GXV_DL_STATIC_TM", "0", 0);
    // gamecpu25 (12 Nov): GXV_DL_STATIC_XANIM=1 (default 0; only acts with GXV_DL_STATIC) lets models with a BCK take part in the repeat test (they were all
    // refused as "no data": the test read ModelManager::mModel, null for them). Verify: GXV_DL_STATIC=2 GXV_DL_STATIC_XANIM=1 (dl-static verify: repeats compared > 0, differed 0).
    setenv("GXV_DL_STATIC_XANIM", "0", 0);
    // gamecpu25 (12 Nov): GXV_PTCL_THIN=<fraction> (default 0 = off): particles of emitters beyond GXV_PTCL_THIN_D0 (2500) from the camera are drawn thinned,
    // 100 % at D0 falling to <fraction> at GXV_PTCL_THIN_D1 (7000); GXV_PTCL_THIN_DRY=1 only counts (the "ptcl-thin:" line). Stage-settable: ptcl_thin, ptcl_thin_d0/_d1.
    setenv("GXV_PTCL_THIN", "0", 0);
    setenv("GXV_PTCL_THIN_DRY", "0", 0);
    // replay25 (12 Nov): GXV_REPLAY25 (decomp/src/Game/System/DrawBuffer.cpp, "replay25"): a rigid shape's draw (J3DShapePacket::drawFast) is replayed as
    // the bytes it wrote last time when every word they depend on is the same (the shape, its model's vertex arrays, the two matrix halves of the
    // view pass, the shape's flags, whether the vertex-format list is due); the matrices are not in the stream, so a moving rigid actor repeats too.
    // 0 = off until a console verify run: GXV_REPLAY25=2 draws everything for real and compares; the "replay25:" line must say "0 differed" and
    // "0 post-state wrong"; then 1. GXV_REPLAY25_KB (default 768) is the slot pool. Host test: tools/test_replay25.sh.
    setenv("GXV_REPLAY25", "0", 0);
    // game24: GXV_DRAW_PREFETCH=1 (default 0) asks for the next packet's first 64 bytes before J3DDrawBuffer::drawHead/drawTail draws the current
    // one (a cache hint, nothing else changes; GXV_DRAW_PROF=2 prints "packets a turn asked ahead for"). Exact by construction.
    setenv("GXV_DRAW_PREFETCH", "0", 0);
    // game24: GXV_PAR_ANIM_WHY=1 (default 0): a census line every ~5 s, "par-anim-why": the models the game thread computes in full in calc-anim
    // (not taken by the memo or GXV_PAR_ANIM), by the first refusing test, with ms a second and the top actors. Changes nothing but two clock reads.
    setenv("GXV_PAR_ANIM_WHY", "0", 0);
    setenv("GXV_PAR_PTCL_PAUSE", "120", 0);
    setenv("GXV_PAR_PTCL_STALL_US", "1500", 0);
    // dlcrash (8 Nov): the Bee Mushroom crash (2.0 .. 2.3). GXV_DL_LAZY_KEY: a deferred material-list refresh (GXV_DL_LAZY=2) runs only
    // while its model manager still points at the models it was asked for (Mario's one manager switches between his bodies:
    // decomp/src/Game/LiveActor/ModelManager.cpp). GXV_DL_GUARD: a differed list longer than its buffer is not stored ("FATAL: display
    // list overflow" line, J3DPacket.cpp) instead of overwriting the heap after it. 0 = as before (A/B only).
    setenv("GXV_DL_LAZY_KEY", "1", 0);
    setenv("GXV_DL_GUARD", "1", 0);
    // relfix24 (10 Nov): GXV_POWERUP_RETRY: a power-up pickup whose form models are missing is refused and the item stays in the world
    // (MorphItemObjNeo / BenefitItemInvincible ask MR::canChangePlayerItemStatus first); for the Bee, initBeeMario is tried again first
    // (twice at most per stage) in case its archives finished loading after Mario was made. 0 = no retry (the refusal stays).
    // (lead: 0 until a console run has exercised it: the retry builds the Bee models in the scene heap in the middle of play)
    setenv("GXV_POWERUP_RETRY", "0", 0);
    // relfix24 (10 Nov): GXV_TILT_AUTOSTICK: in the tilt levels (surfing, Star Ball ...) with "Tilt with: Sensor", a motion sensor that is present
    // but dead (raw readings frozen for GXV_TILT_AUTOSTICK_S = 4 s while the left stick is moved) or that does not answer a full left stick held
    // for 1.5 s hands the level to the left stick; never changes the saved setting. 0 = off. Rules: platform/include/port/tilt_autostick.h.
    setenv("GXV_TILT_AUTOSTICK", "1", 0);
    // retained (8 Nov): GXV_RETAINED: J3D shape calls carry a handle (J3DShapeDraw::draw -> GXDisplayList.c port_gx_call_retained); the
    // front end goes by it straight to the cache entry of the shape's last call, checked by the vertex-state CP words and the generations
    // kept with it (vgx_dlcache.cpp "retained shapes"). 0 = off until the console A/B; 2 = verify (the normal path draws, every would-be
    // hit is compared with it: "retained: MISMATCH" lines, and the "workload: retained per picture" line's mismatching count).
    setenv("GXV_RETAINED", "1", 0);
    // fedl25 (12 Nov): GXV_RT_INST: a retained record may also be made of a call that hit a variant of its list behind the first, and a shape drawn
    // with other vertex state than its record's (another instance of the model: arrays of its own) gets a record in a slot of its own, so the
    // instances stop evicting each other's (Honeyhive's "retained per picture": 31 calls a picture with no record, 2-15 failed). 0 = off (default)
    // until the console A/B; 1 = on; 2 = verify (every call takes the normal path; a record that would have been a hit must be a hit of the same
    // entry, else "retained: MISMATCH"; the vertex / index bytes that differ are counted). GXV_FE_CENSUS=1 now also prints "fe-census: bypass
    // detail" (why Build and StateListCall refused each bypassed list) and "fe-census: retained" (why calls had no record, which lists).
    setenv("GXV_RT_INST", "0", 0);
    // fe23 (8 Nov): GXV_DL_VARIANTS: the vertex states (variants) a display list may keep in the front end's list cache (8 = as before,
    // up to 32). Fur (FurCtrl::drawFur) draws one shape list once per layer with the layer's own position array: more states than 8 and
    // every call is a miss, the list is bypassed and decoded on every call (Honeyhive's "list-other", ~56 a picture at ~88 us). With 24
    // they are cache hits. Exact as the list cache is (GXV_DLCACHE=verify also checks the walk). GXV_FE_CENSUS=1: "fe-census:" lines
    // every 300 pictures (bypassed lists by cause and the costliest by address, the variant walk, the static runs that repeat the one
    // before = instanced models such as the flower groups' plants). Both read once.
    setenv("GXV_DL_VARIANTS", "24", 0);
    setenv("GXV_FE_CENSUS", "0", 0);
    // feparallel (8 Nov): GXV_FE_PARALLEL: streamed draws' vertex decodes go to the front-end split's finishing thread (the fourth core with
    // GXV_CORE3_SPLIT) as one more operation, wherever the player does not read the vertices itself (vgx_fepar.h). 0 = off until the console
    // A/B; 2 = verify (the player decodes as before, the finishing thread decodes again and compares: "fepar: MISMATCH" lines, the
    // "fepar:" summary's "mismatching" count). GXV_FE_PARALLEL_PROF=1: a line per picture.
    // c166 (15 Nov): OFF again. Console c163, the Comet Observatory (the front end is its limit), four runs of the same route: with 1 the front end finished
    // 18.2 ms (median) after the game's draw end and pad to flip was 98 ms; with 0, 16.5 ms and 82-98 ms (hitches 10 and 12 lines). The decodes it hands over
    // are tiny there (about 55 vertices a picture) and each costs a settle at the next draw-sync token that waits for the finishing thread's whole backlog
    // ("fepar: ... settles that waited": 0.6-0.7 ms a picture; the player took over in about 3 of 4); Beach Bowl paid 0.82 ms a picture for about 600.
    setenv("GXV_FE_PARALLEL", "0", 0);
    setenv("GXV_DYN_CENSUS", "0", 0); // ptcl24: 1 = a "dyn-census:" line every 10 s (dynamic draws per picture by primitive and vertex count, FE time)
    setenv("GXV_PALJOIN_CENSUS", "0", 0); // paljoin31: 1 = a "pal-census:" line every GXV_PALJOIN_CENSUS_MS (5000): how many skinned static draws one palette per model could join, by model, with their front-end time (log only)
    // streamc31: bit 2 (=2, or 3 with the line above) = the stream-decode census, "dyn-census decode:" lines (platform/vgx/src/vgx_deccen.h):
    // the stream draws' decode and EmitDraw time by class, whether each draw's inputs (stream bytes, indexed array ranges) are byte-identical
    // to the picture before, and what an exact compare-and-keep decode cache would save and cost. GXV_DYN_CENSUS_KB (default 1024) caps
    // each of its two copy arenas. Census only: nothing drawn changes. Costs about 8 clock reads a stream draw while on.
    setenv("GXV_FE24_CLIP", "0", 0); // fe24: 1 = clip-volume memo + early-out for a point inside; 2 = also verify every memo hit
    // fesplit23 (8 Nov): GXV_FE_TWOCORE: a two-core front end. A second instance of the front end (vgx_twocore_b*.cpp) on the fourth core
    // runs ahead of the player with a state-only scan and decodes contiguous segments of the picture (state, material lists, cache hits of
    // promoted lists = static J3D geometry) into the split's operations; the player splices them in at the segment's start and adopts B's
    // state at its end (vgx_twocore.h). Needs GXV_SPLIT=1 and a proven fourth core (else it stays off). 0 = off until the console A/B;
    // 2 = verify (segments made and compared at their starts and ends, never used: "fetwo: MISMATCH" lines and the "verify:" counts).
    // GXV_FE_TWOCORE_LOG=<n>: the "fetwo:" summary every n pictures (1: a line per picture with each core's FE CPU and the split points).
    // twocore24 (10 Nov): c42 froze with =1 (B spun at 0x93 above the par-view worker: std::this_thread::yield() is empty on the console; and
    // B's published end stayed in the other FIFO segment). Now every wait sleeps and is bounded, B runs at GXV_FE_TWOCORE_PRIO=0xA0 (below
    // the par workers, the finisher, the back end and audio; GXV_FE_TWOCORE_CORE=3 default, 1 to try), the switch turns itself off after
    // GXV_FE_TWOCORE_TRIP hard timeouts ("fetwo: ... turns itself off"), and GXV_SHADOW_RECT / GXV_SHADOWFILL_BOUNDS no longer refuse it.
    // The second "fetwo:" line says how many pictures went without B and why. agents/twocore24/REPORT.txt.
    // twocore25 (11 Nov): c49's verify mismatches ("resolved state fields 0x6") were GXV_DIRECT_J3D's deferred key copies on the player's side
    // (compared as bytes; not a difference); B now parses J3D's small differed lists (GXV_FE_TWOCORE_TINY=1) and publishes a segment only
    // once it has a draw; B runs only in play (GXV_FE_TWOCORE_SETTLE=60 pictures after play begins) and rests while it makes less than
    // GXV_FE_TWOCORE_MINSHARE=5 % of the bytes; no wait at the picture's end. Measured: even with B infinitely early, the console-shaped host
    // stream leaves the player ~80 % of its stream work: keep 0 (agents/twocore25/REPORT.txt).
    setenv("GXV_FE_TWOCORE", "0", 0);
    // hrw26 (12 Nov): GXV_HRW: half-rate world, full-rate camera. 0 off (proposed default; not enabled by default until measured on the console), 1 on (needs GXV_PRESENT_SYNC=2,
    // not GXV_FE_TWOCORE), 2 verify-still (the retained buffer replayed with D = identity must equal the first execution: "hrw:" verify runs / bad), 3 census only.
    // GXV_HRW_ROT_DEG=4 / GXV_HRW_TRANS=600: the guard band (a bigger camera change in a pair is not re-projected). Log: "hrw:" every 5 s.
    setenv("GXV_HRW", "0", 0);
    // hrw27 (12 Nov): with GXV_HRW 1 / 2 / 4. GXV_HRW=4 verify-fresh: the partial draw is also made in the drawn turn and compared with the full picture ("hrw: fresh" line: off by >0.25 px must be 0; no second pictures).
    // GXV_HRW_FRESH=1 (Mario drawn again on the undrawn turn with its camera; 0 = the plain replay), GXV_HRW_CLIP_PAD=120 (percent: the cull frustum while pairs are made; 100 = the game's),
    // GXV_HRW_POINTER=0 (the star pointer: reserved, not built), GXV_HRW_FRESH_MS=2.5 (the significance set's budget: host-tested only, not wired into the game's draw yet).
    setenv("GXV_HRW_FRESH", "1", 0);
    setenv("GXV_HRW_CLIP_PAD", "120", 0);
    setenv("GXV_HRW_POINTER", "0", 0);
    setenv("GXV_HRW_FRESH_MS", "2.5", 0);
    // hrw28 (13 Nov): the significance set is wired (PortHrwSet.cpp). GXV_HRW_SET=0 (proposed default; 1 = the set), GXV_HRW_SET_MAX=16 actors refreshed at most, GXV_HRW_SET_NEAR=900 (units: near Mario / the camera),
    // GXV_HRW_SET_MIN=1.0 (score under which an actor is not worth it), GXV_HRW_SET_SKIP=<name part> (an actor class kept retained, for triage). Log: "hrw: set |" every 5 s, "hrw: pictures" every 10 s.
    setenv("GXV_HRW_SET", "0", 0);  // (proposed default 0: Mario only, as hrw27, until the set has been through verify-fresh on the emulator and the console)
    setenv("GXV_HRW_SET_MAX", "16", 0);
    setenv("GXV_HRW_SET_NEAR", "900", 0);
    setenv("GXV_HRW_SET_MIN", "1.0", 0);
    // directj3d (8 Nov): GXV_DIRECT_J3D: a retained J3D shape's static runs resolve with the shape's key template (shader ids by key hash,
    // checked per run) and leave the two key copies into the resolver's state until something reads them (vgx_direct.h). 1 also turns
    // the retained records on (the handles: GXV_RETAINED's game side). 0 = off until the console A/B; 2 = verify (every templated run is
    // also resolved the normal way and compared: "direct: MISMATCH" lines, and the "workload: direct J3D" line's mismatching count).
    setenv("GXV_DIRECT_J3D", "1", 0);
    // mapobjcut (6 Nov, Honeyhive): GXV_MO_GRAVMEMO: a gravity query asked again at the same position while no gravity changed returns
    // the answer computed then (PlanetGravityManager.cpp; exact: every writer of a gravity's fields moves port_grav_gen). Console check:
    // GXV_MO_GRAVMEMO=2 must show "mismatches 0" in the "grav-memo:" lines.
    // (r92 console, Bowser 3, hand play: Mario jumped from a wall back to other ground. The memos that can touch gravity / collision /
    // areas were turned off until each passed a verify run in Bowser 3; all three did, so they are on again.)
    setenv("GXV_MO_GRAVMEMO", "1", 0);  // (r93 console verify, Bowser 3: 0 mismatches in ~260k queries with ~1000 gravity changes / 15 s)
    setenv("GXV_HL_AREACUBE", "1", 0);  // (r94 console verify, Bowser 3: 1.44M checks, 0 mismatches, 0 stale)
    // kcl (6 Nov): GXV_KCL_MEMO: one map-collision walk (sphere, line, area) passes the prisms it has already looked at in another
    // octree cell instead of testing them again (KCollision.cpp; exact: same hits, order, distances and flags; host test on 8 real
    // KCLs, 16M queries). 0: every prism tested in every cell, as before. 2 (kcl2): verify: both walks run, the game uses the original's
    // answer, and "kcl-verify:" log lines count checks and mismatches (must be 0) and spell out the first 8.
    setenv("GXV_KCL_MEMO", "1", 0);  // (r94 console verify, Bowser 3: 590k map queries, 0 mismatches)
    // par2 (6 Nov): exact, by par-anim's word-for-word key rule (host test tools/test_par_anim_walk.sh: blend and reverse runs, NEG_PREV).
    // GXV_PAR_ANIM_BLEND: par-anim also takes models in an interpolation / freeze copy (the core's joint state copied into the job and
    // compared at the take). GXV_PAR_FLUSH: the drawn turn's far-model flush gets its own batch on the fourth core (DESIGN.md stage 2).
    // Console check first: GXV_PAR_ANIM_VERIFY=1 must show 0 mismatches in the "par-anim" and "par-flush" lines.
    for (const char* name : {"GXV_PAR_ANIM_BLEND"}) setenv(name, "1", 0);
    // par2 (6 Nov, second patch): GXV_PAR_LATE: calc-anim's joint computations of par-anim's models get a batch at calc-anim's start (the
    // inputs final), on the fourth core, taken at each model's own computation on the same key rule. GXV_PAR_ANIM_CB: par-anim also takes
    // the NPCs' scale joint callback (NPCActor::calcJointScale: every HoneyBee). Exact; console check with GXV_PAR_ANIM_VERIFY=1 first.
    // r87 console (Honeyhive): GXV_PAR_LATE cost the game thread 0.2 ms a calc-anim and saved nothing; GXV_PAR_FLUSH made jobs for
    // 0.8 ms a picture and saved nothing. Both off; GXV_PAR_ANIM_CB stays on (par-anim exactness verified, 76007 checks, 0 mismatches).
    for (const char* name : {"GXV_PAR_ANIM_CB"}) setenv(name, "1", 0);
    // parwork (7 Nov): GXV_PAR_PTCL: the particle calc's independent emitters (every effect made through MultiEmitter) on the fourth core and
    // the game thread in one join, the rest of the emitters on the game thread beside it (JPAEmitterManager::calcNormal). Exact by the proof in
    // that file; default 0 until the console check: GXV_PAR_PTCL=2 must show 0 mismatches in the "par-ptcl" line, then 1.
    setenv("GXV_PAR_PTCL", "1", 0);
    // gamepar (8 Nov): the game thread toward 13 ms a turn in Performance mode (agents/gamepar/REPORT.txt).
    //  GXV_PAR_PTCL_OVERLAP=1: with GXV_PAR_PTCL on, the particle chains go to the worker before the shadow list and the game thread casts
    //    the shadows meanwhile (exact: they share no data; GXV_PAR_PTCL=2 still verifies every parallel turn).
    //  GXV_FLUSH_OFFSCREEN: plants wholly outside the picture's view are not posed (exact for the picture and the game); GXV_PLANT_DRAWCULL:
    //    nor drawn (left out as a hidden shape is; exact for the picture). Both "perf" until a console look in Honeyhive, then they can be 1.
    //  GXV_HR_PLANTMOVE (a minor change: light and sound only): far resting flower-group members move every second turn, their animation
    //    clock steps replayed (the same clock, poses and nerve logic). The far half-rate joints and the far shadow clip are the visual
    //    budget's (GXV_PB_ANIM, GXV_PB_SHADOW above). "perf": Performance mode only; 0 off, 1 every mode.
    //  GXV_TURN_PROF=1 in env.txt: the "turn-prof:" line (the game thread's turn by category, every 5 s).
    setenv("GXV_PAR_PTCL_OVERLAP", "1", 0);
    // gamecpu23 (8 Nov): GXV_PTCL_LOOKPAR: in GXV_PAR_PTCL's parallel turns before a picture, the parallel chains' emitters run their particles'
    //   look functions in the calc (the original's way) on the particle worker, instead of in the game thread's draw (GXV_JPA_LOOK's flush).
    //   Exact; 0 until the console check (=2: "ptcl-lookpar:" verify line must show 0 differed), then 1 together with GXV_PAR_PTCL=1.
    //   GXV_TURN_PROF=2 / GXV_DRAW_PROF=2 in env.txt: draw() split into mirror / 3d (shadows, effects) / 2d layouts (diagnostic).
    setenv("GXV_PTCL_LOOKPAR", "0", 0);
    //  GXV_JPA_LOOK_PURE: GXV_JPA_LOOK (look functions once a picture, at the draw) also for the effects whose particle callback does nothing
    //   (every MultiEmitter effect: without it GXV_JPA_LOOK covers ~37 of ~400 Honeyhive particles). Exact by GXV_JPA_LOOK's argument (the
    //   callback runs first and does nothing); 0 until the console verify (=2: "ptcl-lookpar:" lines, 0 differed).
    setenv("GXV_JPA_LOOK_PURE", "1", 0);
    //  GXV_PAR_ANIM_CB2: par-anim's walk also performs KoteBug's posture joint callback; GXV_PAR_LATE=2: calc-anim's batch on the fourth core
    //   for the HEAVY models only (KoteBug's, and >= GXV_PAR_LATE_MIN=40 joints + envelopes), where a job costs far less than the joints it
    //   saves (r87's GXV_PAR_LATE=1 took every model and lost 0.2 ms). Both exact (key rule); 0 until GXV_PAR_ANIM_VERIFY=1 shows 0 mismatches.
    setenv("GXV_PAR_ANIM_CB2", "0", 0);
    // revisit23 round 2 (8 Nov): GXV_PAR_PLANT: the drawn turn's flush of the flower groups' plants (GXV_ANIM_LOD_FREE) on the fourth core,
    // the jobs MADE by the worker (GXV_PAR_FLUSH made them on the game thread and saved nothing at r87); exact by par-anim's key rule.
    // 0 until the console check: GXV_PAR_PLANT=2 must show 0 mismatches in the "par-plant" line, then 1.
    setenv("GXV_PAR_PLANT", "0", 0);
    // parplant23 (8 Nov): the 472-verify run (GXV_PAR_PLANT=2) showed the worker never got to a plant first: listed right before the flush,
    // whose first entries are the plants, it had no head start. GXV_PAR_PLANT_EARLY=1: the batch is listed at the end of the turn before a
    // drawn turn (predicted; a batch whose turn does not draw ends before that turn's update()). GXV_PAR_PLANT_TAKE=1: par-anim leaves those
    // plants to it (their jobs no longer made on the game thread). Both act only when GXV_PAR_PLANT is on; =0 gives round 2's behaviour.
    for (const char* name : {"GXV_PAR_PLANT_EARLY", "GXV_PAR_PLANT_TAKE"}) setenv(name, "1", 0);
    for (const char* name : {"GXV_FLUSH_OFFSCREEN", "GXV_PLANT_DRAWCULL", "GXV_HR_PLANTMOVE"}) setenv(name, "perf", 0);
    // gamedraw23 (8 Nov): GXV_PAR_DL: the drawn turn's material lists (port_dl_flush_stale, ~2.5 ms in heavy Honeyhive) made by a worker on the
    // fourth core while the game thread makes the flush's view pass (all but the player's); joined before the draw, j3dSys merged. Exact (proof:
    // decomp/src/Game/LiveActor/ModelManager.cpp "GXV_PAR_DL"); GXV_PAR_DL_VIEW (default 1, acts only with it) leaves a drawn turn's view pass to
    // the flush too, so 60 pictures a second have one to overlap. 0 until the console verify: GXV_PAR_DL=2 must show 0 differed in the "par-dl:"
    // line (entries, j3dSys words, material states) and no "finished early" / conflicts; then 1.
    // Round 2 (job 500: 0 turns overlapped at 60 with Settings' "Logic overlap: Max" = GXV_DRAWDONE_DEFER=4): GXV_PAR_DL_PLAYER (default 1,
    // acts only with it) splits a pending pass that includes the player's: the rest overlapped, the player's part after the join.
    setenv("GXV_PAR_DL", "0", 0);
    // par25 (12 Nov): GXV_PAR25 (platform/include/port/par25.h): one "par25:" line every 5 s (jobs, worker ms, the game thread's net ms saved by each worker, mismatches)
    //  and a guard that switches par-anim / par-ptcl / par-dl off when their own measured net is negative (probation after 60 s, doubling) or when a verify
    //  mismatch appears. 0 = off (nothing posted, nothing gated); 1 = guard; 2 = observe (the line and the would-be verdicts, nothing gated: the first console run).
    //  GXV_PAR25_NEG_MS (3 ms/s = 0.05 ms a turn) / _NEG_WINDOWS (2) / _PROBE (12 ticks of 5 s) / _GOOD (3) / _LOG (1).
    setenv("GXV_PAR25", "0", 0);
    // actorlod23 (8 Nov): GXV_ALOD, the update LOD of far actors (a MINOR CHANGE; decomp/src/Game/LiveActor/ActorTurnLod.cpp): star bits,
    //   coins, kotebugs, karipons, petit piranhas, honeybees and rocks that are far from the player (GXV_ALOD_PDIST 2500 + reach), off the
    //   picture (or, with GXV_ALOD_TINY=px, a few pixels small on it), untouched this turn and outside any demo move every second turn
    //   (every fourth beyond GXV_ALOD_FAR4 5000); the turns left out are replayed in order before their next movement (the actor's own
    //   clock, nerve and body as the original), so only the ORDER against others changes. 0 off, 1 on, 2 census (nothing left out; the
    //   "alod:" lines time what 1 would save), "perf" = Performance mode only. 0 until the census and the visual check (REPORT, actorlod23).
    //   alodfix23 (8 Nov): back to 0. The console's Performance runs (quality23 482/483, flow 530) leave out 0.1-7 actors a turn, ~4.5
    //   star bits + 0.5 rocks steady in Honeyhive; their movement is replayed (10-60 us a step, no cheaper than the movement), so only
    //   their calcAnim / flush share is saved: ~0.05-0.1 ms of a ~14 ms turn. A Performance-only change must buy speed; this does not.
    //   (It did not cause the star select -> stage crash: that was GXV_PAR_PTCL's storage, JPAEmitterManager.cpp.)
    setenv("GXV_ALOD", "0", 0);
    // lightvol25 (12 Nov): GXV_LIGHTVOL, the light volumes' (VolumeModelDrawer, Dreadnought's 20 beams) list draw: bits 1 exact cull of a volume wholly off the
    //   screen / behind the eye, 2 pass-major groups of disjoint volumes (the same picture), 4 the LOD (nearest KEEP volumes, volumes under PX pixels fade out, the
    //   tint scaled smoothly; volumes within NEAR units are never reduced; a cut volume keeps its alpha clear, CLEAR=1), 8 the census line (per 300 pictures, what the
    //   options would do). 0 until the console's picture check (decomp/src/Game/LiveActor/VolumeModelDrawer.cpp, port/lightvol.h, tests/lightvol_test.cpp).
    setenv("GXV_LIGHTVOL", "0", 0);
    setenv("GXV_LIGHTVOL_KEEP", "12", 0);
    setenv("GXV_LIGHTVOL_PX", "2500", 0);
    setenv("GXV_LIGHTVOL_NEAR", "1500", 0);
    setenv("GXV_LIGHTVOL_CLEAR", "1", 0);
    // farlogic25 (11 Nov): GXV_FARLOGIC, the far-logic LEVEL (decomp/src/Game/LiveActor/ActorTurnLod.cpp, port/farlogic.h; per stage through
    //   port_farlogic_set(level)): 0 off; 1 pure decoration and the star bit / coin / honeybee families beyond 5000 units from the player AND the
    //   camera; 2 also kotebug / karipon / petit piranha / rock, beyond 2500; 3 level 2 plus the Enemy and Item calc-anim lists posed every
    //   other turn beyond GXV_FARLOGIC_POSE_DIST (2500). An actor in the far state runs its movement and calcAnim every second turn, NOT replayed:
    //   its own time goes at HALF SPEED while it is far (the logic has no dt), full speed again 10 % inside the limit. Never: the player, anything
    //   with collision, a stage switch, a rail, a camera controller, an unlisted actor with a hit sensor, a hit this turn, demos, rushes.
    //   GXV_FARLOGIC_DRY=1: nothing changes; the "farlogic: DRY" lines count and time what levels 1 and 2 (and 3's pose rule) would take.
    //   GXV_FARLOGIC_ARM=1: every LiveActor goes through the entry when made, so a level set at run time reaches them (else only actors made
    //   after the level is set, plus the existing ones marked once on the first turn after a set). All 0.
    setenv("GXV_FARLOGIC", "0", 0);
    setenv("GXV_FARLOGIC_DRY", "0", 0);
    setenv("GXV_FARLOGIC_ARM", "0", 0);
    setenv("GXV_FARLOGIC_POSE_DIST", "2500", 0);
    // idle25 (12 Nov): GXV_IDLE25, exact idle skipping (decomp/src/Game/LiveActor/ActorTurnLod.cpp, port/idle25.h): an actor whose movement() is
    //   provably a known micro-step this turn (a floating star bit, a waiting coin, a sleeping petit piranha) runs the micro-step and the parts of movement()
    //   that are not provable no-ops. No owed time, no change in what happens. 0 off, 1 on, 2 VERIFY (real movement + the prediction compared, mismatches logged).
    setenv("GXV_IDLE25", "0", 0);
    // undrawn26 (12 Nov): GXV_UNDRAWN_LEAN, the lean undrawn turn (platform/src/port/perf.cpp, port/undrawn_lean.h, ShadowController.cpp, LiveActor.cpp): the joints of a
    //   model that nothing but the picture reads (no MR::getJointMtx & co. ever asked, no collision, camera or joint callback) and the casts of a shadow that no logic reads
    //   are made once, before the picture, not in every turn. 0 off (default until a verify run is clean), 1 on, 2 VERIFY (everything runs as always; a read of a product
    //   mode 1 would have left out is logged "undrawn-lean: VIOLATION"). GXV_UNDRAWN_LEAN_JOINTS / _SHADOWS (default 1) switch the two products; GXV_UNDRAWN_LEAN_HASH=N
    //   logs a gameplay-state hash every N turns (any mode) for diffing runs.
    setenv("GXV_UNDRAWN_LEAN", "0", 0);
    // defaults49 (22 Nov, owner's eyes session on the Vita): 60 Assist's feel set, as he played it ("felt better with the 60 assist on"); these only act while
    // GXV_HRW pairs run (Performance's step-down), so they change nothing in Quality. Half-rate glow + shadow texture: -2.4 ms GPU at Beach Bowl; the sky flicker
    // he saw was the simple bloom's reuse adding the wrong texture at fold 2, fixed in sky49 (e9c60713); "=2 only with target 60", so Quality is unaffected.
    setenv("GXV_HRW41_DEPTH", "2", 0);
    setenv("GXV_HRW41_BOXKEY", "1", 0);
    setenv("GXV_HRW41_BUDGET", "0", 0);  // 23 Nov: the cheaper rotation/plane warp misplaced Mario, star bits and items (size pops, double Mario) on the console
    setenv("GXV_HRW_GUARD_B", "0", 0);  // 23 Nov: a pair the speed guard can't give a fresh Mario shows the real picture again, not a shifted old Mario
    setenv("GXV_HRW_MARIOTIME_LIMIT", "0", 0);
    setenv("GXV_HRW_WARP_SHARP", "40", 0);
    setenv("GXV_HRW_WARP_SNAP", "5", 0);
    setenv("GXV_HRW_HALO", "1", 0);
    setenv("PETARI_BLOOM_HALFRATE", "2", 0);
    setenv("PETARI_SHADOWTEX_HALFRATE", "2", 0);
    // defaults49: the model calc's material mutex without shutting the second CPU's gate (69647275); Vita c36e Beach / Honeyhive: taken from the game thread
    // 0.21 -> 0.10 / 0.28 -> 0.08 ms a turn, drawn turn -0.64 / -1.14 / -0.05 ms, fps +0.3..+0.6. Scheduling only.
    setenv("GXV_MUTEX_GATE_FAST", "1", 0);
    // cpu34g (14 Nov 2026): the drawn turn of a drawn + undrawn pair (platform/src/port/perf.cpp PairTurnBegin; LiveActor.cpp, ClippingActorHolder.cpp, ActorTurnLod.cpp).
    //   GXV_PAIRPOSE=1 (2: also models with a joint callback; _PDIST 800): joints the undrawn turn makes again before the picture are not made in the drawn turn.
    //   GXV_CLIP_PAIR=1: the clipping verdicts likewise. GXV_FARLOGIC_MIRROR=1: the far-logic level also acts in a stage with a mirror camera. All 0 until a console run.
    setenv("GXV_PAIRPOSE", "0", 0);
    setenv("GXV_CLIP_PAIR", "0", 0);
    setenv("GXV_PAIR_LAYOUT", "0", 0);  // (the HUD's layout likewise)
    // ratecut31 (17 Nov 2026, plan-perf31 B3; platform/include/port/pose_phase.h): GXV_POSE_PHASE, the every-n-th-turn pose / material / plant-movement cuts
    //   (GXV_PB_ANIM and far-logic level 3's every 2nd, GXV_FARANIM, GXV_CHARPOSE, GXV_FAMPOSE, GXV_MATLOD, GXV_STARBATCH_FAR, GXV_HR_PLANTMOVE) leave work out
    //   only on the drawn turn of a pair, which the undrawn turn makes again before the picture: no picture shows a pose a turn old. 1 = that, the every-3rd /
    //   4th capped at every 2nd and 60 Assist's pair set posed every turn (pose age at draw 0); 2 = the every-2nd members only; 0 = as before.
    //   GXV_POSE_PHASE_CENSUS=1: the "pose-phase:" line (the age of the pose each picture shows, per cut). Default 1 in both modes since defaults32 (mode_keys.h row; the Vita A/B of 19 Nov passed); this 0 is only the code default when no mode applies.
    setenv("GXV_POSE_PHASE", "0", 0);
    setenv("GXV_POSE_PHASE_CENSUS", "0", 0);
    // bowl31 (17 Nov 2026, perf-inventory D4; platform/include/port/bowl_pose.h, LiveActor.cpp PortBowlPose): GXV_BOWL_POSE, a Beach Bowl fish's pose is left out of the
    //   drawn turn of a pair (the same skip turn as GXV_POSE_PHASE) when nothing reads it before the next turn makes it again; a broken pair is posed by the flush
    //   before its picture (pose age at draw 0). 1 = the cut (fish), 2 = verify (nothing left out; state hash before / after and every reader in the window
    //   compared, fish and penguins; "bowl-pose: MISMATCH" lines), 0 = as before. The "bowl-pose:" line every 600 turns in modes 1 and 2. 0 until the console verify.
    setenv("GXV_BOWL_POSE", "0", 0);
    // ondraw31 (18 Nov 2026, plan-cpu31 idea 2; platform/include/port/pose_ondraw.h, LiveActor.cpp PortOnDraw): GXV_POSE_ONDRAW, bowl31's rule for every class
    //   of the non-player calc-anim lists (Environment, MapObj, NPC, Enemy, MapObjDecoration, Item): on the drawn turn of a pair (the skip turn) an actor's
    //   joints and view pass are left out when nothing reads them before the next turn makes them again, decided per actor at run time (joints never handed
    //   out, no joint callback, no collision / camera / 60 Assist pair-set model, no other rule holding it, not a shared core, not a lockstep session or a
    //   demo, GXV_POSE_BLENDFIX=1); the base matrix is made as always; a first reader in the window gets the joints made at once from the same state (or
    //   is counted LATE); the flush makes any owed pose before its picture. 1 = the cut, 2 = verify (nothing left out; state hash, joint reads, the next
    //   pose and the picture compared: "pose-ondraw: MISMATCH" lines), 0 = as before. The "pose-ondraw:" census line every 600 turns in modes 1 and 2
    //   (left out by list, refusals by reason, catch-ups, LATE reads, stale at draw, estimated ms saved a pair). 0 until the console verify and A/B.
    setenv("GXV_POSE_ONDRAW", "0", 0);
    // blendfix31 (17 Nov 2026; Game/Animation/XanimeCore.hpp, LiveActor.cpp PortPoseLeftOut, tests/blendfix.cpp): GXV_POSE_BLENDFIX, a bug fix of every pose cut
    //   (PAIRPOSE, UNDRAWN_LEAN / ANIM_LOD, PB_ANIM, FARANIM, CHARPOSE, FAMPOSE, far-logic 3, POSE_PHASE, STARBATCH_FAR, BOWL_POSE): a change of animation right
    //   after a left-out pose started its blend from a pose one turn (or more) old. 1 = the left-out computation's joint state (no matrices) is made from
    //   its recorded inputs when something reads it (a change of animation; a computation that reads the last pose); 2 = measure only (as 0, with the
    //   census); 0 = as before. GXV_POSE_BLENDFIX_CENSUS=1: the "pose-blendfix:" line every 600 turns (catch-ups made, how far the old blend start was off).
    setenv("GXV_POSE_BLENDFIX", "1", 0);
    setenv("GXV_POSE_BLENDFIX_CENSUS", "0", 0);
    setenv("GXV_FARLOGIC_MIRROR", "0", 0);
    // update23 (8 Nov): the update phase (agents/update23/REPORT.txt). Both exact.
    //  GXV_SENSOR_BP: SensorHitChecker's 14 pair loops (GXV_HL_SENSOR's copies) look only at the pairs whose sensors can touch along the
    //    axis of the widest spread (a sorted broad phase with a proven margin), in the original order: the same hit lists, same order.
    //    0: the plain loops; 2: verify (the original loops run after and are used; "hl-verify:" sensor mismatches must stay 0).
    //  GXV_UPD_PREFETCH: cache hints in the update's pointer-chasing loops (category lists, the clipping judge, the sensor collection).
    //    0: off. Nothing read for a result changes.
    for (const char* name : {"GXV_SENSOR_BP", "GXV_UPD_PREFETCH"}) setenv(name, "1", 0);
    // update25 (11 Nov): the update / flush phases (agents/update25/REPORT.txt). All exact, all 0 until their console A/B.
    //  GXV_UPD_PREFETCH2: the next actor's parts (model manager, nerve, sensor keeper, binder, effect keeper, sound object, light controller,
    //    shadow list) asked for ahead in the LiveActor movement lists (cache hints only). Needs GXV_UPD_PREFETCH=1.
    //  GXV_UPD_LIVESET: the flushes' "still one of the scene's actors" from a hash table, not a binary search. 2: verify ("live-set:").
    //  GXV_UPD_SNDNAME: a sound asked for by name found through a hash index, not a scan of the name table. 2: verify ("snd-name:").
    //  GXV_LOGIC_PROF=3 (env.txt): per-class phase rows, the drawn turn's flush rows per model class, the sound-name census, 20 class rows.
    setenv("GXV_UPD_PREFETCH2", "0", 0);
    // c180 (15 Nov): LIVESET and SNDNAME on (were 0). Console verify, c179 Performance (3130d): "live-set:" 26 lines, "snd-name:" 6 lines, all
    //  0 mismatches (the emulator gate too); an outside review (15 Nov) asked for the one run that settles them.
    for (const char* name : {"GXV_UPD_LIVESET", "GXV_UPD_SNDNAME"}) setenv(name, "1", 0);
    //  GXV_UPD_PREFETCH3 (update26): the list loop three stages ahead (object lines, parts' lines, the blocks behind the parts: J3D model, pose player,
    //    sensor infos, animation infos) for the movement AND calcAnim lists of LiveActors. Hints only, so exact; default 0 until a console A/B
    //    (the "update" and "calc-anim" ms of turn-prof, the "upd26:" line). Needs GXV_UPD_PREFETCH=1.
    setenv("GXV_UPD_PREFETCH3", "0", 0);
    // hotprof25 (11 Nov): cuts found in the console's execution-count profile (agents/pgorec: one 12 minute run, Honeyhive). All exact.
    //  GXV_HOTPROF_DYNCULL (vgx_draw.cpp, vgx_hotprof_dyncull.inc): GXV_DYNCULL's per-point test of rigid draws, the same answer for every
    //    draw, about a third of the time (hoisted row magnitudes, an early end, the margin bounded before it is computed). 1 on (host: 20 M
    //    draws against the old loop, 0 mismatches; pictures identical). 0: the old loop. 2: verify, "hotprof:" line every 10 s.
    //  GXV_HOTPROF_PTCLCULL (JPAResource.cpp, port/ptcl_cull.h): a particle quad wholly outside one plane of the clip volume is not sent by
    //    the game thread at all (the front end's own test, made before the quad's matrix and list call are written; 65 % of the dynamic
    //    draws of the profiled run were such quads, dropped by GXV_DYNCULL at the end of the front end's work). The picture is the same;
    //    the stream is shorter. 0 until the console A/B; 1 on; 2 dry (counted, all sent). "ptcl-cull:" line every 4096 particle loops.
    //  GXV_HOTPROF_LIVESNAP (LiveActor.cpp): "the scene's actor group is unchanged" from the groups' write count, not a walk of ~2000
    //    entries 3.6 times a picture. GXV_HOTPROF_REQSCAN (NameObjExecuteHolder.cpp): the scans for objects asking to leave the movement /
    //    draw lists walk the span asked for, not ~1100 entries twice in every second turn. Both: 0 until a verify run (=2: "live-snap:" /
    //    "req-scan:" lines, mismatches must be 0), then 1.
    //  GXV_HOTPROF_NOFLUSHSTRIP (GXGeometry.c __GXSendFlushPrim): the GX library's all-zero flush strips (a console hardware workaround, 372
    //    a picture in the profiled run, 43 % of the primitives the front end parses, every one dropped by it) are not written into the
    //    stream. Host: the front end's command buffers are the same without them. 0 until the console A/B; 1 on.
    // hotprof25 round 2 (11 Nov): the profile's items that sat in the GX library. All exact, all 0 until their console verify / A-B.
    //  GXV_HOTPROF_TEVCOL (GXTev.c): GXSetTevColor's BG word once instead of three times (42 % of the stream's BP commands are particles'
    //    TEV colours; half of them are these repeats), and inside a particle loop a word equal to the loop's last for its register not
    //    written. 2: verify (everything written; the front end checks each word 1 would leave out: "tevcol verify:" must say 0 differing).
    //  GXV_HOTPROF_REINITGX (GXTev.c, GXBump.c, J3DSys.cpp): J3DSys::reinitGX's reset of the 16 TEV stages (144 GX calls, 6.45 times a
    //    picture) as one pass that writes each register once. 2: verify (the calls as before; the library's state compared: "reinitgx:").
    //  GXV_HOTPROF_WPAD (WPadAcceleration.cpp): while a remote's 128 acceleration samples are one finite value (a handheld not simulating a
    //    shake) the mean, the rotation outputs and the average distance are the known ones; the three history loops (four objects a turn)
    //    are not run. 2: verify ("wpad-still:" line, mismatches must be 0).
    //  GXV_HOTPROF_BOXCULL (vgx_draw.cpp, vgx_hotprof_boxcull.inc): the box test of the static and cluster culls ends as soon as no plane
    //    is left that every corner so far is beyond; the same mask. 2: verify ("hotprof: GXV_HOTPROF_BOXCULL=2" line every 10 s).
    //  GXV_HOTPROF_PTCLCENSUS (JPAResource.cpp; a log line only, with GXV_HOTPROF_PTCLCULL=1 or 2): "ptcl-census:", the particle resources
    //    with the most off-screen quads since the line before, by name.
    for (const char* name : {"GXV_HOTPROF_TEVCOL", "GXV_HOTPROF_REINITGX", "GXV_HOTPROF_PTCLCENSUS"}) setenv(name, "0", 0);
    // c180 (15 Nov): WPAD, BOXCULL, LIVESNAP and REQSCAN on (were 0). Console verify, c179 Performance (3130d): "wpad-still:" 4 lines,
    //  "hotprof: GXV_HOTPROF_BOXCULL=2" 34, "live-snap:" 33, "req-scan:" 31, all 0 mismatches (the emulator gate too).
    for (const char* name : {"GXV_HOTPROF_WPAD", "GXV_HOTPROF_BOXCULL", "GXV_HOTPROF_LIVESNAP", "GXV_HOTPROF_REQSCAN"}) setenv(name, "1", 0);
    setenv("GXV_HOTPROF_DYNCULL", "1", 0);
    for (const char* name : {"GXV_HOTPROF_PTCLCULL", "GXV_HOTPROF_NOFLUSHSTRIP"}) setenv(name, "0", 0);
    // perf32 (20 Nov 2026; both exact, default 0 = unset until a console A/B; no default is written here: an unset name reads as 0):
    //  GXV_PTCL_PRE (JPAResource.cpp, port/ptcl_cull.h QuadPreOutside): with GXV_HOTPROF_PTCLCULL, a particle quad far outside the clip volume is answered from
    //    its centre, not from its four or eight vertices; the quads left out are the same ones. 1 on, 2 verify ("ptcl-cull: pre: ... 0 wrong").
    //  GXV_CPULOD_MEMO (port/cpulod.h TableValue): the governors' / the Quality lock's table lookups without their string compares. 1 on, 2 verify
    //    ("cpulod-memo: ... 0 differ").
    // logic23 (8 Nov): the update / calc-anim phases (agents/logic23/REPORT.txt).
    //  GXV_AREA_GRID: AreaObjMgr::find_in asks only the areas whose world box (GXV_HL_AREACUBE's) holds the point's grid cell, plus the
    //    areas without one, from the end as before; also a memo of the manager found by name (AreaObjContainer::getManager). Exact
    //    (the same area comes back). 0 until the console verify: =2 must show "mismatches 0 / 0" in the "area-grid:" lines, then 1.
    //  GXV_GRAV_GRID: a gravity query the memo misses walks only the gravities whose reach box holds the position's cell, plus the
    //    moving / unbounded ones, in the array's order, with the walk's own body. Exact. 0 until =2 shows "mismatches 0" ("grav-grid:").
    //  GXV_LOGIC_PROF=1 / 2 in env.txt: the "logic-prof:" lines (update and calc-anim by class and by system; 2 adds LiveActor::movement's
    //    phases). Diagnostic, unset = off.
    for (const char* name : {"GXV_AREA_GRID", "GXV_GRAV_GRID"}) setenv(name, "0", 0);
    // zoomclip23 (8 Nov): the handheld zoom (settings.ini "zoom", PETARI_ZOOM_FORCE) narrowed the field of view the game's object clipping
    //   judged with: actors a little outside the zoomed picture, but inside the original view, were frozen with their hit sensors off
    //   (coins, star bits, Launch / Pull Stars could not be touched or pointed at). GXV_ZOOM_LOGIC_VIEW: the clipping and every
    //   MR::isJudgedToClipFrustum test (and GXV_ALOD's off-picture test, the far-camera sound) use the game's own view, as at zoom 1.0;
    //   GXV_ZOOM_DRAWCULL: such an actor's model is still not drawn while it is outside the zoomed picture (the picture and its cost as
    //   2.2.1's); GXV_ZOOM_PULLSTAR (a MINOR CHANGE): while a Pull Star is in reach the zoom eases to 1.0 so it can be pointed at.
    //   0 = as 2.2.1 for each. (decomp/src/Game/Util/CameraUtil.cpp, LiveActor/ClippingActorHolder.cpp, Camera/CameraContext.cpp)
    for (const char* name : {"GXV_ZOOM_LOGIC_VIEW", "GXV_ZOOM_DRAWCULL", "GXV_ZOOM_PULLSTAR"}) setenv(name, "1", 0);
    // galmap23 (8 Nov): GXV_ZOOM_FIXEDCAM (a MINOR CHANGE): while the stage holds the game camera completely still (CAM_TYPE_POINT_FIX, a
    //   composed shot like a cutscene) the zoom eases to 1.0, and back after; 2 also covers the fixed-eye cameras that turn to follow
    //   Mario; 0 = as 2.2.1. (Cutscenes, event cameras such as the dome's galaxy-select map and the Launch Star flight, the whole
    //   AstroDome stage via GXV_ASTRO_DOME_ZOOM_GUARD above, file select and the star-select screen already keep zoom 1.0.)
    setenv("GXV_ZOOM_FIXEDCAM", "1", 0);
    // popin24 (10 Nov; platform/include/port/popin.h, agents/popin24/REPORT.txt): the pop-in audit. All four default 0 until the console A/B:
    //   GXV_POPIN_LOG=1       "popin:" / "popin-top:" / "popin-tex:" census lines every 10 s (what the port's switches and the game's own clipping made
    //                         appear or vanish, how many inside the picture, the largest projected size; the texture budget's evictions)
    //   GXV_POPIN_HYST=8      hysteresis of GXV_PB_LOD / _DECOR / _SHADOW / _FUR and of LodCtrl's own swap (8 % of the distance), the particle thinning ramps
    //   GXV_POPIN_ZOOM=1|2    the port's distances (1) and also the game's LodCtrl distances (2) grow with the handheld zoom ratio
    //   GXV_POPIN_CLIPPAD=10  the clipping frustum's sides moved out by 10 % (a CPU / draw cost for the extra ring of actors)
    for (const char* name : {"GXV_POPIN_LOG", "GXV_POPIN_HYST", "GXV_POPIN_ZOOM", "GXV_POPIN_CLIPPAD"}) setenv(name, "0", 0);
    // popin25 (fly-over pop-ins; every one 0 = as before):
    //   GXV_POPIN_CENSUS=1    the census by cause, per 5 s and per flight ("popin25:", "popin25-top:", "popin25-run:" lines)
    //   GXV_PB_DEMO=1|2       in a flight (a demo, the opening camera, the starter, the star demo, a fast camera far from the player): 1 widens the
    //                         budget's distances by GXV_PB_DEMO_WIDEN (1.5) and measures LOD from the nearer of the player and the camera; 2 = budget off
    //   GXV_POPIN_LEAD=6      a fast camera appears things earlier (budget distances and clipping spheres grow by speed x turns, capped)
    for (const char* name : {"GXV_POPIN_CENSUS", "GXV_PB_DEMO", "GXV_POPIN_LEAD"}) setenv(name, "0", 0);
    // detail25 (platform/include/port/detail.h; all default 0 = off): GXV_DETAIL_GOV=1 the governor (needs GXV_AUTOQ_TARGET=60); GXV_DETAIL_LEVEL=<0..5> a fixed
    // level (1 = today); GXV_DETAIL_CYCLE=<s> the stress cycle; GXV_DETAIL_PLANT / GXV_DETAIL_FUR = 1 on, 2 dry census. Tuning: GXV_DETAIL_HI / _LO / _DWELL /
    // _RELAX / _FALLBACK / _HYST / _FUR_GAIN / _FUR_FAR / _BUDGET / _START / _UNDRAWN (see the header).
    for (const char* name : {"GXV_DETAIL_GOV", "GXV_DETAIL_PLANT", "GXV_DETAIL_FUR", "GXV_DETAIL_CYCLE"}) setenv(name, "0", 0);
    // GXV_SHADOWMEMO (shadow projection memo, exact): r89 console saved 0.5-1.7 ms a turn in Honeyhive but cost Bowser 3 ~3 fps (moving
    // collision: change-log scans). Off until its adaptive gate lands.
    setenv("GXV_AC_PANEMTX", "1", 0);  // (animcut: layout pane matrices kept while unchanged; r89 console verify 236k checks, 0 differed)
    setenv("GXV_SHADOWMEMO", "1", 0);  // (r91: rebuilt with collision hooks + per-turn change grid; verify 0 mismatches, Honeyhive net ~0.9 ms a turn)
    setenv("GXV_UNI_SHADOW", "1", 0);
    setenv("GXV_VITA_UPLOAD_SYNC", "2", 0);
    setenv("GXV_AUTOQ_FREE", "85", 0);
    setenv("GXV_AUTOQ_WIPE", "1", 0);
    setenv("GXV_AUTOQ_LATEROOM", "1", 0);
    // r82 console (lit Observatory, zoom 1.0): pinned at 960x544 147-151 of 150 pictures came on time (GPU busy 32.5 ms), yet Quality stepped
    // to 784x440 on a ~102 % GPU window (Bowser 3 960 -> 784, Sea Slide 960 -> 720). Quality under the fixed cap now judges by the pictures
    // on time: a smaller size only after sustained misses blamed on the GPU, a climb trial back after 20 s of calm (vgx_autoq.h
    // Config::hold30; Locked 30 and Performance unchanged). GXV_AUTOQ_HOLD30=0 in env.txt: the GPU-busy rules as before.
    setenv("GXV_AUTOQ_HOLD30", "1", 0);
    // autoq (7 Nov): Performance mode only (target 60; the controller forces it off for Quality and Locked 30): a sustained burst of pictures
    // late for the GPU (8 of the last 12, every other stage fitting) steps down one size within ~0.3 s and again while they stay late, instead of
    // after 4+ s and a quiet moment (r100: Good Egg zoom 2.0 held 720x404 at 48.6 fps, Honeyhive 41.8). GXV_AUTOQ_FAST60=0: the old pace.
    setenv("GXV_AUTOQ_FAST60", "1", 0);
    // GXV_PACE23 (2.3, pace23 agent; platform/vgx/src/vgx_autoq.cpp "GXV_PACE23"): the frame-pacing rules: the GPU's load without the display
    // scene's waits, a late picture blamed on the GPU only when it held it, no size change the GPU decides for 6 s after a load, few and direct
    // size changes, the steady 30 below 50 pictures a second. Off until the console tour (tests: see the agent's REPORT); GXV_PACE23=1 turns it on.
    setenv("GXV_PACE23", "0", 0);
    // GXV_PACE60 (2.3, pace60 agent; vgx_autoq.h "pace60"): Performance mode's 60 or an even 30 decided by a GPU model from the scene timers'
    // split (TA the same at any size, the EFB/pass 3D falling with the pixels, the display scene's edge smoothing / upscaler as a step, the
    // measured overlap): 60 is chased (steps and size in one change) only when 608x348 with every step is predicted to hold it; otherwise the
    // steady 30 at 720x404 at once, no chase; the steady 30 takes GXV_PACE23's 50 / 57 thresholds. Target 60 only. Off until the console A/B
    // (GXV_PACE60=1 in env.txt; "pace60:" lines every 5 s).
    setenv("GXV_PACE60", "0", 0);
    // GXV_AUTOQ_QGUARD (2.3, quality23 agent; vgx_autoq.cpp "qguard"): the owner's rule, quality is given up only while it buys pictures.
    // Performance mode only (target 60; the controller forces it off for Quality and Locked 30): no verdict or step from loading, title, menu
    // or pause pictures (and 8 windows / 3 s of play after them); every reduction (fast60, overload, pace60, the slow rules) judged after 4 s of
    // play and restored when the pictures came no faster ("[autoq] quality: ... restored: bought no speed"); on a 30 cadence with 60 out of
    // reach everything that fits 33.3 ms comes back within ~2 s; target 60 under the fixed 30 cap is a 30 cadence. Console 483-c8P (cheaper
    // background and edge smoothing off for good at an even 30, GPU 20 of 33.3 ms) and 491-c9SF1 (fast60 720 -> 608 at an even 30).
    // GXV_AUTOQ_QGUARD=0 in env.txt: the old decisions (A/B).
    setenv("GXV_AUTOQ_QGUARD", "1", 0);
    // GXV_RT_DEFER (2.3, obscrash agent; the 2.2 Comet Observatory crash, platform/vgx/src/vgx_backend_gl.cpp "GXV_RT_DEFER"): a texture the
    // GPU draws into (render textures retired by a rung change, premade blanks, pooled objects' own colours, a copy texture whose size
    // changes) is deleted 4 presents after the back end lets it go, not at once: vitaGL frees an unsampled texture's storage immediately and
    // the GPU's last scene then wrote its pixels into vitaGL's video-memory heap. A bug fix; GXV_RT_DEFER=0 only to A/B the 2.2 behaviour.
    setenv("GXV_RT_DEFER", "4", 0);
    // GXV_SEAMLESS (2.3, seamless23; platform/vgx/src/vgx_backend_gl.cpp "GXV_SEAMLESS", vgx_autoq.h Config::seam, frame_pace.h cutRelease): the
    // owner's "no jumps, missed scenes or weird transitions when the quality changes". Every transition gets a "[seamless]" basic-log line with
    // its 4 pictures; a change of size that is not urgent waits (at most 30 pictures) until its render targets are made ahead, one a picture
    // where the picture has room; the objects of a size left are kept for a return (900 presents); a texture rewritten before it is read keeps
    // its picture until that write (never blank); edge smoothing and the upscaler's lobe fade over 20 pictures; quality-up steps and the
    // pacing's return to "up to 60" wait for a camera cut, a closed fade or the pause menu (at most 15 s). Reductions for late pictures are
    // never held. GXV_SEAMLESS=0: the 2.3 behaviour; each part has its own switch (GXV_SEAMLESS_PREP/_KEEPOLD/_LAZY/_RAMP/_CUT/_PACE_CUT).
    setenv("GXV_SEAMLESS", "1", 0);
    // gpu960 (6 Nov): GPU cuts at 960x544 for Quality's full size. GXV_EDGE_AA_HUDCUT: the edge pass leaves out the HUD rectangles it
    // redraws anyway (byte-identical). GXV_VGL_MASKKEEP: vitaGL (patches/0018, MASK_KEEP) skips the full-target scissor-mask quad of
    // scenes that do not load depth. GXV_EFBSWAP2: the shadow-alpha and indirect screen captures without an EFB scene split (exact;
    // r84 console: no freeze over 10 stage loads, but Good Egg fell from 30 to ~23 fps: its raw EFB copies need program variants linked
    // in play every launch and later conversions (renderer CPU late in 217 pictures); GXV_EFBSWAP2=0 gave 30 again, so it stays off).
    // GXV_PASSHALF=2: only the bloom chain's blur passes at half size.
    // gpucopy (6 Nov): the r84 cost was the draw-back's partial colour mask (vitaGL re-patched the copy program twice a picture and
    // linked a variant in play) and readers before the copy ("converted later"): GXV_EFBSWAP_FULLMASK / _PREDICT / _HOLD fix both
    // (code defaults on), so GXV_EFBSWAP2 is on again for the r87 console check (Good Egg must hold 30).
    // r95 console A/B (Beach Bowl + Bowser 3): with GXV_EFBSWAP2 and GXV_SHADOWFILL_BOUNDS on, 27.9 / 28.7 fps (renderer late); both off,
    // 29.5 / 30.0. Exact but a net cost on the SGX (draw-back quads, scissor-mask passes): off.
    for (const char* name : {"GXV_EDGE_AA_HUDCUT", "GXV_VGL_MASKKEEP"}) setenv(name, "1", 0);
    setenv("GXV_PASSHALF", "2", 0);
    // shadowfill (6 Nov): the silhouette / shadow-volume fill (a whole-EFB alpha capture and a full-screen alpha-tested quad every
    // picture) left out when no draw put alpha anywhere (the capture becomes a fill of 0, none at all while its texture holds 0) and
    // scissored to the volumes' projected pixels otherwise; byte-identical (tools/test_vgx_shadowfill_gl.sh). "shadowfill:" basic-log
    // line about once a minute. GXV_SHADOWFILL_BOUNDS=0 in env.txt: the full path every picture, as before.
    setenv("GXV_SHADOWFILL_BOUNDS", "0", 0);
    // gpugen26 (12 Nov): GXV_SHADOWFILL_SHRINK (needs GXV_SHADOWFILL_BOUNDS=1): the silhouette fill's four corners moved to the volumes' pixels instead of a scissor (no vitaGL
    // scissor-mask passes: the cost r95 found); same pixels. 0 off (not yet run on the console); 1 shrink, else the scissor; 2 shrink only. "shadowfill:" line: "shrink: N moved".
    setenv("GXV_SHADOWFILL_SHRINK", "0", 0);
    // gpugen26: GXV_SMOOTH60_MID=1 adds the 896x512 rung between 960x544 and 848x480 (GXV_SMOOTH60_896_FPS, 57); GXV_SMOOTH60_HUD=1 draws the 2D layer at 960x544 in the 848 / 896 modes.
    // Both 0: the controller is as policy26 had it.
    setenv("GXV_SMOOTH60_MID", "0", 0);
    setenv("GXV_SMOOTH60_HUD", "0", 0);
    setenv("GXV_SMOOTH60_896_FPS", "57", 0);
    // gpu23 (8 Nov): fewer GXM scenes a picture. GXV_INDCAP_SKIP: the game's "Indirect" screen capture left out when nothing in its
    // picture reads it (one EFB split less; exact, checked every picture: "indcap:" basic-log line, "stale reads" must stay 0; =2
    // counts only). The other three are MINOR CHANGES meant for Performance mode (=2: only with GXV_AUTOQ_TARGET=60): the bloom's glow
    // (PETARI_BLOOM_HALFRATE), the player's shadow texture (PETARI_SHADOWTEX_HALFRATE) and the indirect capture of a picture with
    // water or haze on screen (GXV_INDCAP_HALFRATE) made every other picture, the in-between picture reusing the last one. All off
    // until the console A/B (agents/gpu23/REPORT.txt).
    for (const char* name : {"PETARI_CLIPCHAIN_HALFRATE", "PETARI_CLIPCHAIN_ONEPASS", "PETARI_CLIPCHAIN_SKIPEMPTY", "GXV_INDCAP_SKIP", "GXV_INDCAP_HALFRATE", "GXV_INDCAP_CAMGATE", "GXV_INDCAP_VIS", "PETARI_BLOOM_HALFRATE", "PETARI_SHADOWTEX_HALFRATE"}) setenv(name, "0", 0);
    // prevcap25 (11 Nov): GXV_PREVCAP=<bitmask>: mid-picture EFB captures answered by the PREVIOUS picture (bit 1: the "Indirect" capture; the EFB's colour
    // buffer of the picture just shown is kept for the next picture, no extra copy). A MINOR CHANGE at best and visibly wrong for refraction (the reader samples
    // its own earlier output: agents/prevcap25/REPORT.txt): off, for console measurement only. GXV_PREVCAP_CENSUS=1: the "prevcap-census:" basic-log line, no change.
    for (const char* name : {"GXV_PREVCAP", "GXV_PREVCAP_CENSUS"}) setenv(name, "0", 0);
    // scenes25 (11 Nov): GXV_SCENES25=1: the "scenes25:" basic-log line every 5 s, every EFB split of the picture attributed to the capture class that made it
    // (indirect, shadow alpha, player's corner, clip mask / tiles, colour whole / half / tile, pass, other) with readers / dry counts per class (no picture change);
    // GXV_SCENES25_TRACE=<n>: the first n copies one line each. GXV_SCENES25_DRY=<mask> (1 corner, 2 whole-EFB 1:1 other than Indirect, 4 tiles / half copies):
    // captures no draw reads are left out by GXV_INDCAP_SKIP's rules (class E; "indcap: ... stale reads 0" must hold). Both off.
    for (const char* name : {"GXV_SCENES25", "GXV_SCENES25_DRY", "GXV_SCENES25_TRACE"}) setenv(name, "0", 0);
    // GXV_SCENE_PROBE=<n> (1..16; diagnostic, test-only): n empty 64x64 scenes at the start of every picture, to price one scene on the console (read gpu-scenes busy / TA / 3D at n = 0, 2, 4;
    // "scene-probe:" line). Never a play setting. Off.
    setenv("GXV_SCENE_PROBE", "0", 0);
    // revisit23 (8 Nov): PETARI_BLOOM_SKIP: the bloom's glow chain (8 captures, ~40 blur draws: up to 7 GXM scenes a picture) left out on
    // pictures where no glow source drew on its black fill (exact: the glow is then black; =2 counts only: "bloom-skip:" basic-log
    // line). C183: GXV_LA88_MIPS=1: eligible square generated I8/IA4/IA8 chains use LA88 after the covered-screen transfer self-test (rejection keeps RGBA8).
    // fix31 (owner 17 Nov: off until the console A/B, as c181): default 0, RGBA8 as c181; =1 is the A/B arm.
    setenv("PETARI_BLOOM_SKIP", "1", 0); setenv("GXV_LA88_MIPS", "0", 0);
    // bloom23 (8-9 Nov): GXV_BLOOM_FAST, the two bloom chains with fewer GXM scenes. 1 (a MINOR CHANGE): the normal bloom (BloomArea,
    // under water in Beach Bowl / Sea Slide, Kingfin) in 4 texture scenes instead of 8; the simple bloom (SimpleBloom areas: Beach Bowl above
    // water) in 5 texture scenes + 1 EFB split instead of 6 + 3. 2 (exact): only the simple bloom's unused capture left out (one scene).
    // perf = 1 in Performance mode only. 0 = the original chains. "bloom-fast:" basic-log lines (decomp BloomEffect.cpp, BloomEffectSimple.cpp).
    // Console A/B on c22 (9 Nov, 960x544): Beach Bowl above water (simple bloom) GPU busy 35.4 -> 34.3 ms at 1 (34.9 at 2), TA -2 ms;
    // the Kingfin fight (normal bloom) 41.5 -> 41.2 ms, TA -1.2 ms. Screenshots side by side show no visible difference. On in every mode.
    setenv("GXV_BLOOM_FAST", "1", 0);
    // bloomfold25 (11 Nov): GXV_BLOOM_FOLD, a further fold of both bloom chains (stage-settable, PORT_STAGE_KNOB bloom_fold). 0 (default, the chain
    // GXV_BLOOM_FAST says) / 1 the same blur shape in one pass fewer (normal 3 texture scenes, simple 3 + 0 EFB split instead of 5 + 1) / 2 a reduced
    // glow, one small pass at 40x28 with the chain's energy and extent (normal 2 texture scenes, simple 3). Neither level removes the glow. Off until the
    // console A/B (agents/bloomfold25/REPORT.txt). A new shader text? None: the passes use the TEV forms the shipped pack already has or compile at run time.
    setenv("GXV_BLOOM_FOLD", "0", 0);
    // gpu23b (8 Nov): GXV_SHADOWFETCH: the silhouette fill (MR::fillSilhouetteColor) reads the EFB's alpha in its own shader (a
    // __nativecolor Cg program, programmable blending; needs libvitaGL-reader-vp24.a, vitaGL patch 0025) instead of the whole-EFB A8
    // capture: no EFB scene split for it in any galaxy. Each fill key is calibrated on the console against the real path over a test
    // pattern (all bytes, all alphas) before it is used, and never used if it differs ("shadowfetch:" lines). A MINOR CHANGE at the
    // shadow mask's edges: the console's capture path samples between texels (the mask smeared by up to a pixel), the fetch reads each
    // pixel's own alpha (as the Wii); the calibration log counts those edge pixels. =2 calibrates and checks only. Off until the console
    // A/B (agents/gpu23b/REPORT3.txt).
    setenv("GXV_SHADOWFETCH", "0", 0);
    // gpu23b: GXV_IND_PRECOMP (a MINOR CHANGE, gpuwater's item 2.1): an indirect stage's coordinate with the texture size folded in per
    // draw (Beach Bowl's sea: a multiply, the bias add and a per-pixel divide fewer over 0.5-0.75 of the screen). Not bit-exact (the
    // coordinate moves by about one float step: the sea within one level; materials that compare or wrap the warped sample can flip
    // rare pixels). Needs the shader pack re-seeded for its new texts. Console: 3D -0.3..-0.7 ms at Beach Bowl in 6 of 6 pairs, no first-visit
    // hitch (IMPROVE45 / CONFIRM46). ON since 24 Nov: the owner accepted the near-exact change. GXV_IND_PRECOMP=0 turns it off.
    setenv("GXV_IND_PRECOMP", "1", 0);
    // pix23 (8 Nov): value-proven pixel cuts, both exact. GXV_PIX_ZERO: a draw whose own values leave every pixel as it was (its alpha
    // test fails for every fragment, or no depth write and a blend whose source term is exactly 0: alpha 0 under SRC_ALPHA /
    // ONE_MINUS_SRC_ALPHA, colour 0 under ONE / ONE, ...) is not drawn. GXV_PIX_ATPASS: a draw whose alpha test passes for every
    // fragment (the textures' alpha ranges are recorded at upload) is drawn without the test and its `discard` (the GPU's hidden-surface
    // removal keeps working; a new shader text per such key: compiled on first use until the pack is refilled). =2 counts only.
    // "pix23:" basic-log line every 10 s. Off until the console A/B (agents/pix23/REPORT.txt).
    // defaults48 (22 Nov): defaults46's GXV_PIX_ZERO=1 in Performance is taken back (port/mode_keys.h: the confirm pass at the same place, Honeyhive -3.2 / -3.5 fps,
    // drawn turn +0.9 ms). =3, the lean way, waits for its console A-B; the code default stays 0. GXV_PIX_ATPASS took 0 draws and makes new shader texts: it stays off.
    for (const char* name : {"GXV_PIX_ZERO", "GXV_PIX_ATPASS"}) setenv(name, "0", 0);
    // gpu960 (10 Nov): GXV_PIX_BLENDOFF (0 off; 1 exact: a blended draw whose source alpha is exactly 1.0, under SRC_ALPHA / ONE_MINUS_SRC_ALPHA, ONE / ZERO and the like,
    // is drawn unblended so the GPU's hidden-surface removal works for it; 2 also MINOR: alpha proven within GXV_PIX_BLENDOFF_LEVELS (tenths of a byte level, default 10)
    // of 1). GXV_PIX_CENSUS=1: a "gpu960:" log line with the coverage (screens, an upper bound) of the draws GXV_PIX_ATPASS / BLENDOFF can or nearly can take.
    // GXV_GPU_BISECT (DIAGNOSTIC, not exact: 1 opaque textured draws output the rasterised colour only, 2 they read no texture) to find where the world's pixel
    // time goes. All off by default (agents/gpu960/REPORT.txt).
    for (const char* name : {"GXV_PIX_BLENDOFF", "GXV_PIX_CENSUS", "GXV_GPU_BISECT"}) setenv(name, "0", 0);
    // scenes23 (8 Nov): fewer / cheaper GXM scene boundaries, all exact. GXV_HOIST_GRID: the player's shadow-corner copy
    // (CollisionShadow::drawAndCaptureTex) made from a target of the EFB's own size and pixel grid right after the silhouette capture,
    // instead of splitting the EFB's scene (Honeyhive: 4 -> 3 EFB scenes; same scene count, the split's full-size colour + depth store
    // and load replaced by a small pass); =2 counts only. GXV_VGL_VALIDREGION: that pass's scene begun with a GXM valid region (only
    // its tiles processed; vitaGL patch 0028, vp26). GXV_SCENES23_LOG: the "scenes23:" census line without the hoist. GXV_GXM_PROBE:
    // a start-up read-back of what GXM's colour load really reads (diagnostic). GXV_COLOR_LOADSKIP bit 16 (vp26): the scenes that begin
    // with a clear start with the clear's own colour as their tile load (exact by construction; the zero texture of bits 1 / 2 left a
    // band on the console). All off until the console A/B (agents/scenes23/REPORT.txt).
    for (const char* name : {"GXV_HOIST_GRID", "GXV_VGL_VALIDREGION", "GXV_SCENES23_LOG", "GXV_GXM_PROBE"}) setenv(name, "0", 0);
    // shadowpx23 (8 Nov): GXV_SHADOW_RECT: Mario's alpha shadow pass (the whole-EFB A8 capture + the silhouette fill, every picture)
    // limited to the screen rectangle where the shadow volumes / silhouette / corner can have put alpha: the capture's copy scene
    // draws only that box (plus what its own last capture may have left there) through a scissor (+ GXM valid region with
    // GXV_VGL_VALIDREGION=1), the rest of the texture keeps its 0s; the fill is cut as GXV_SHADOWFILL_BOUNDS cuts it (=1 turns those
    // cuts on too). Exact: the capture's texture is the whole copy bit for bit (tools/test_vgx_shadowfill_gl.sh, test_vgx_hoistgrid_gl.sh
    // with HG_EXTRA). =2: the rectangle worked out and logged only. "shadowrect:" / "shadowrect (back end):" basic-log lines.
    // GXV_FS_HALF_MINOR: a MINOR CHANGE (0 / 1 / perf): fragment shaders whose half-precision colour error is proven below 2 8-bit
    // levels (not 1) are made half too (alpha tests keep the strict bound); new shader texts for those keys only (pack refill).
    // Console A/B on c25 (9 Nov, Honeyhive 720x404, uncapped): GXV_SHADOW_RECT=1 with gpuab23's GXV_SHADOW_CULLCOVER fix scissors the
    // fill to ~26% of the EFB: 3D 18.95 -> 18.09 ms, TA 12.8 -> 10.7 ms, 52.2 -> 53.2 fps; exact, so on. GXV_FS_HALF_MINOR measured
    // nothing on c21 (705): stays off.
    // Console pairs 10 Nov (Honeyhive 608x348 uncapped; 3 runs on, 2 off): =1 saves 0.6 ms of GPU 3D (11.1 vs 11.7-11.8) and costs the
    // front end 1.3 ms (16.7-16.9 vs 15.4-15.6) and the game thread's drawn turn 0.8 ms (17.9 vs 17.0-17.2): the two threads at the
    // limit. Back to 0 until GXV_SHADOW_RECT_FE=1 (shadowfe24: the cheap bookkeeping) has shown on the console that =1 costs nothing there.
    setenv("GXV_SHADOW_RECT", "0", 0);
    // shadowfe24 (10 Nov): GXV_SHADOW_RECT_FE: the front-end cost of the alpha bounds GXV_SHADOW_RECT needs (a console pair, Honeyhive 608x348: front end
    // 16.74 ms with it, 15.35 without; the host fixture's callgrind with Honeyhive-like shadow volumes: ~270k instructions a picture, 75 % of them
    // SfProj::Point's double-precision corner transforms, every volume's two passes worked out twice). 1 = the same bounds (exact: same command
    // buffers) with a memo of the last bounds, repeated vertices skipped, shared products and no deferred-key copy for draws that cannot write alpha;
    // 2 = both ways compared per draw (host tests). 0 = as it was. "shadowrect-fe:" basic-log line. Default 0 until the console A/B.
    setenv("GXV_SHADOW_RECT_FE", "0", 0);
    // pass24 (10 Nov): GXV_SHADOW_STENCIL: the shadow volumes and Mario's silhouette are counted in the EFB's stencil (DEPTH24_STENCIL8: +1 byte a
    // pixel), and MR::fillSilhouetteColor's fill is drawn where the count is not 0: the whole-EFB A8 capture, its EFB scene split and the draw-back
    // quad are left out of every picture whose shadow window checks out (else that picture keeps the capture: "shadow-stencil:" line). 0 = today.
    // 2 = analyse and count only. Default 0 until the console A/B (tools/ in docs/ pass24 REPORT).
    setenv("GXV_SHADOW_STENCIL", "0", 0);
    // shadow25 (11 Nov): GXV_SILH_GATE: the silhouette / shadow-volume alpha capture and its fill quad (a whole-EFB copy scene + a full-screen pass, every
    // picture) are left out of a picture that holds no shadow volume when the player's silhouette cannot show (not drawn: exact; or the game's rays find no
    // map geometry between the camera and him, held 8 pictures: a minor change, misses map objects without collision). 0 = today, 1 on, 2 census only
    // ("silhgate:" line). GXV_SILH_RAYS (game side, 1-5 rays, default 4), GXV_SILH_HOLD (pictures kept after a hit, default 8).
    setenv("GXV_SILH_GATE", "0", 0);
    // across26 (12 Nov): GXV_SILH_OCC=<0..3> (default 0): the gate also needs an occlusion query's all-clear (vgx_silhocc.h): 1 hold 12 pictures and a hit seen once, 2 hold 5, 3 census
    // (queries run and are counted, no decision changes). Acts only with GXV_SILH_GATE=1 (census: GXV_SILH_GATE=2). GXV_SILH_OCC_HOLD=<pictures> and GXV_SILH_OCC_MINPX=<samples> tune it.
    // "silhocc:" line every 600 pictures with the gate's line. GXV_ACROSS (above) sets it.
    setenv("GXV_SILH_OCC", "0", 0);
    setenv("GXV_ACROSS", "0", 0);
    // shadow25 (11 Nov): GXV_COLSHADOW_STABLE: Mario's drop shadow (CollisionShadow) lists its ground triangles per logic turn with fixed thresholds; a triangle on
    // a threshold (small planet, far camera) or the cut at the 128 cap changes the set turn to turn: a blinking patch. 1 = stable selection (a band on the thresholds,
    // nearest to the drop point at the cap, a dead band on the box radius), 2 = 1 + the shadow polygons lifted 10 units for the depth test. 0 = today.
    // GXV_COLSHADOW_LOG=1: "colshadow:" census every 300 pictures (set changes while Mario and the camera are still = the flicker signature).
    // bugs26 (12 Nov): default 1. Console census of 12 Nov (logs 1786*-1788, 280 "colshadow:" lines): with 0, 62 of 142 lines show the set changing 3 or more
    // times while Mario and the camera were still (11,573 changes in all; the Dreadnought start: 10 <-> 12 polygons, box half-size 94..95, every turn); with 1,
    // 138 lines show 24 such changes in all (at most 2 in a line) and no picture-to-picture swing on the discs. Never less shadow than the game's larger state.
    setenv("GXV_COLSHADOW_STABLE", "1", 0);
    setenv("GXV_COLSHADOW_LOG", "0", 0);
    // dreadblend25 (12 Nov): GXV_COLSHADOW_CLIP: the drop shadow's per-triangle two-pass draw (draw3) shades whole collision triangles through a 160 x 160 texture whose
    // alpha is 0 outside the silhouette: 40 blended draws a picture on the Dreadnought (static coverage 1.3 screens) for a shadow of a few hundredths. 1 = each triangle is
    // cut to the texture's extent (+1 texel) and a triangle wholly outside is not drawn (the alpha test threw those fragments away: the same picture); 2 = census only
    // ("colshadow-clip:" line); 0 = today. Stage-settable (colshadow_clip). No shader text.
    setenv("GXV_COLSHADOW_CLIP", "0", 0);
    // dread25 (11 Nov): GXV_TINYRUN: a rigid static run whose model-space box projects to fewer than GXV_TINYRUN_PX (default 2.0) pixels squared of the GX viewport
    // is not drawn (vgx_tinyrun.h: the box's eight corners through the position matrix and the projection, not cut to the view; an unknown area keeps the run;
    // the first use of a list is never culled). 2 = the census only ("tinyrun25:" line every 10 s: runs and indices by size class). 0 = as before. Stage-settable
    // (tiny_run / tiny_px). GXV_HSR_ORDER / GXV_HSR_ORDER_TIES are stage-settable too (hsr_order / hsr_ties): no value changed here (unset = 0 / 1 as before).
    setenv("GXV_TINYRUN", "0", 0);
    setenv("GXV_TINYRUN_PX", "2", 0);
    // dread25 (12 Nov): GXV_ALPHATEST_PREPASS (platform/vgx/src/vgx_prepass.h): an alpha-tested draw as a depth-only first pass (the key's text up to its test, colour masked)
    // and a colour pass without `discard` (depth EQUAL, no depth write): 1 the pair at the draw's place; 2 also the first passes hoisted to the start of their run of plain
    // draws (a tested LEQUAL draw with an opaque draw before it); 3 census only ("atpp25:" line every 5 s); 4 / 5 as 1 / 2 but the colour pass keeps the alpha test (exact). 0 = as before. Stage-settable (alphatest_prepass).
    setenv("GXV_ALPHATEST_PREPASS", "0", 0);
    // gpu27 (13 Nov): GXV_DIAG_FETCHONLY=<hex fragment key>[,..] (DIAGNOSTIC, the picture is wrong while it runs): draws of the listed keys (the "fh" of the scene dump, e.g. Freezeflame
    // f9d97a8b) use a fetch-only shader, same blend / coverage / geometry: fill-bound vs arithmetic-bound. Unset = off. "diag-fetchonly:" line every 10 s.
    setenv("GXV_FS_HALF_MINOR", "0", 0);
    // gpuab23 (9 Nov): GXV_SHADOW_CULLCOVER (default 1; acts only while GXV_SHADOW_RECT or GXV_SHADOWFILL_BOUNDS is on): the c21 console run
    // marked every alpha capture "100.0% of the EFB" and cut no fill, because the game's per-picture EFB clear (MainLoopFramework::clearEfb,
    // three XFBs: the display copy never clears) is a GX_CULL_BACK quad and only cull-NONE quads emptied the alpha bookkeeping. 1 takes a
    // rectangle that faces the drawn side as covering; 0 = as c21. Exact (tools/test_vgx_shadowfill_gl.sh SF_TRIPLE cases).
    setenv("GXV_SHADOW_CULLCOVER", "1", 0);
    // gpucut23 (9 Nov), all 0 until the console A/B (agents/gpucut23/REPORT.txt):
    // GXV_FS_TEXREUSE=1: a TEV stage whose texture read repeats the read `tex` already holds (same map, coordinate, swap, LOD bias; never an
    //   indirect coordinate) reads nothing (Honeyhive's planet ground shaders, fs 1520 family, read u_tex0 at uv0 in both stages). Exact
    //   (host: byte-identical pictures, tools/test_vgx_texreuse_gl.sh); new texts for the keys it changes (11 of Honeyhive's 131): re-seed the
    //   shader pack with it on (with GXV_TEVIN_CANON=2).
    // GXV_FIRSTCLEAR=1: the picture's first EFB scene, which begins with the game's EFB clear quad (clearEfb: z ALWAYS with update over the
    //   whole EFB), loads no depth (GXV_DEPTHPLAN now decides that scene: the present hook's word is kept for it). Exact on the host
    //   (tools/test_vgx_firstclear_gl.sh, console depth memory modelled); known limit as GXV_DEPTH_SKIP=2's cleared scenes: a vitaGL
    //   uniform-pool rollover that has to wait re-begins the scene without a load.
    // GXV_PRESENT_PLAIN=perf (a MINOR CHANGE; 0 / 1 / perf): in Performance mode a picture drawn smaller than the screen is presented by the
    //   plain bilinear blit, without the unsharp mask (slightly softer; PSNR 43.3-43.8 dB against the sharpened present on c21 shots).
    setenv("GXV_FS_TEXREUSE", "1", 0);
    for (const char* name : {"GXV_FIRSTCLEAR", "GXV_PRESENT_PLAIN"}) setenv(name, "0", 0);
    // ta24 (10 Nov): GXV_BE_MERGE: adjacent draw commands with the same program, state, textures, uniform values and buffer whose index ranges follow
    //   each other are ONE draw call (1; exact on the host: tests/vgx_bemerge_gl.cpp, tools/test_vgx_bemerge_gl.sh; 2 = census only). The
    //   "be-merge:" basic-log line (every 300 pictures) says how many pairs of Honeyhive's draws qualify and what stops the rest.
    //   GXV_LIGHTBAKE=2: census only (the "lightbake:" line): how much lit geometry has the lighting inputs it had a picture ago (agents/ta24/REPORT.txt).
    // pixcuts25 (11 Nov), all 0 until the console A/B (agents/pixcuts25/REPORT.txt):
    // GXV_CLEARQ=1: the game's per-picture EFB clear quad (clearEfb: one TEV stage C0 / A0, constant Z texture, z ALWAYS with update, GX_CULL_BACK; fs "1172", 0.62-0.77 ms
    //   in the pixels25 dumps) is made by a glClear of colour, alpha and depth when the front end proves it writes one constant colour (bytes 0 / 255), alpha and depth
    //   (0 or 1) to the whole EFB. Exact on the host (tools/test_vgx_pixcuts25_gl.sh). With GXV_COLOR_LOADSKIP=17 (bits 1 + 16, vp26) the scene's tiles start with that
    //   colour as well. =2: count only ("clearq:" line). Stage-settable ("clearq"). No shader text changes.
    // GXV_TEVIN_PREWRAP=1 (needs GXV_FIX_TEVIN=1 and GXV_TEVIN_CANON=2): Mario's wrap shaders (fs 1259-1261, 1193-1195: vgx_w3(c0.rgb) per pixel on a UNIFORM that the shader also reads
    //   as D, which TEVIN_CANON=2's preWrap cannot take) read a wrapped copy of the register from four extra rows of the TEV block. NEW SHADER TEXTS for the keys it changes, only with
    //   the switch on: the old pack entries (switch off) are untouched and still found; refill the pack with the switch on before shipping it on.
    // GXV_INDCAP_REFRESH=<pictures> (U, only makes more captures): with GXV_INDCAP_SKIP=1, a capture nothing reads is still made when the last real one is this old
    //   (bounds the age of the one stale picture a stage's first reader sees). 0 off.
    setenv("GXV_TEVIN_PREWRAP", "1", 0);
    for (const char* name : {"GXV_CLEARQ", "GXV_INDCAP_REFRESH", "GXV_IND_FLAT"}) setenv(name, "0", 0);
    for (const char* name : {"GXV_BE_MERGE", "GXV_LIGHTBAKE"}) setenv(name, "0", 0);
    // scenes26 (12 Nov), all 0 until the console A/B (new/scenes26/REPORT.txt; platform/vgx/src/vgx_scenes26.h):
    // GXV_TAILDEPTH=1 (stage key tail_depth; needs GXV_FIRSTCLEAR=1 and GXV_DEPTHPLAN): the last EFB scene of a picture stores no depth once GXV_TAILDEPTH_RUN (120) pictures in a row
    //   began with a whole-depth overwrite (the next picture's first scene overwrites it unread); a picture that then does not begin so is a counted miss and switches the rule off.
    //   Same limit as GXV_FIRSTCLEAR: a uniform-pool rollover that has to wait re-begins the scene with the modes it began with. 2 = predictor only (counts, drops nothing).
    // GXV_SCENES26=1: "scenes26:" census line every 600 pictures (per EFB scene of a picture: depth load / store shares, colour dead at its start, what the plan and the tail rule
    //   left out); changes no picture. GXV_SCENES26_VERIFY=<block>: pictures alternate in blocks (ORIGINAL / NEW) and a "scenes26: verify" line gives the final pictures' mean / max difference.
    for (const char* name : {"GXV_TAILDEPTH", "GXV_SCENES26", "GXV_SCENES26_VERIFY"}) setenv(name, "0", 0);
    setenv("GXV_TAILDEPTH_RUN", "120", 0);
    // reflfur25 (12 Nov), all 0 until the console A/B (agents/reflfur25/REPORT.txt), all stage-settable (keys "mirror", "mirror_census", "fur25"):
    // GXV_MIRROR25 (platform/include/port/mirror25.h): Mario's reflection on glass / ice. 1: not calculated or drawn when the mirrored model is outside the frustum (exact);
    //   2: also ONE pass (the additive pass punches the floor's depth itself) instead of the game's depth-only pass + additive pass (not exact: overlaps of the mirrored model
    //   can add up to one more layer of the 25 % tint). GXV_MIRROR25_CENSUS=1: the "mirror25:" line every 300 pictures, also with level 0 (counts, changes nothing).
    // GXV_FUR25 (platform/include/port/fur25.h): fur shells by the fur's size on the screen (adjacent shells closer than GAP px merged: alpha merged exactly), at most one shell
    //   per STEP pictures with a hysteresis, full near the camera / the player. 1 conservative (0.75 px, full within 1500), 2 stronger (1.25 px, 1000). GXV_FURCENSUS=1: the
    //   "furcensus:" line every 5 s (fur objects and shells by distance and by thickness on the screen; also with the level 0).
    for (const char* name : {"GXV_MIRROR25", "GXV_MIRROR25_CENSUS", "GXV_FUR25", "GXV_FURCENSUS"}) setenv(name, "0", 0);
    // hsrorder25 (11 Nov): GXV_HSR_ORDER (platform/vgx/src/vgx_hsrorder.h): inside a scene, opaque draws before alpha-tested ones where the picture cannot
    //   change (the SGX543's hidden-surface removal needs opaque draws together; the game's order interleaves alpha-tested draws with them). 0 off; 1 reorder;
    //   2 census only ("hsr25:" lines every 5 s); 3 alternate original / reordered pictures (verify aid). GXV_HSR_ORDER_TIES=1: an alpha-tested draw with the same
    //   transform as a later opaque draw of its segment stays in place (exactness at equal depths). Tests that leave draws out on purpose, default empty (unset):
    //   GXV_DROP_CLASS=<letters of O T B D>, GXV_DROP_FS=<h:fragment-key-hex | fs id list>. The scene dump rows carry "fh" (fragment key) and "sest" (static coverage).
    for (const char* name : {"GXV_HSR_ORDER"}) setenv(name, "0", 0);
    setenv("GXV_HSR_ORDER_TIES", "1", 0);
    // geom26 (12 Nov): GXV_HSR_AUTO (platform/vgx/src/vgx_hsrauto.h; stage key hsr_auto; read only while GXV_HSR_ORDER is 0): the draw-order planner decided stage by stage. 1: for the
    //   first GXV_HSR_AUTO_PROBE (240) drawn pictures of a stage the planner runs on a copy (ties GXV_HSR_AUTO_TIES, default 7, the decal-aware rule) and counts the alpha-tested draws that sit
    //   before the last opaque draw of their scene; at GXV_HSR_AUTO_MINT (100 = 1.00 a picture) or more it is applied (inexact only where a decal lies on a curved/inner face, see gpu26),
    //   else it does not run at all (zero cost; probes again after GXV_HSR_AUTO_RECHECK, 7200, pictures). While applied it ends itself when it moves under GXV_HSR_AUTO_MINMOVED (50 = 0.5)
    //   draws a picture or costs over GXV_HSR_AUTO_MAXUS (400) microseconds a picture. 2: probe and report only. Keeps up to 12 MB of static geometry (GXV_HSR_GEO_KEEP_MB) for the proofs.
    //   Log: "hsr26: auto ..." (decisions) and the hsr25 lines every 5 s. Default 0 until a console A/B; the proposed stage rows are in agents/geom26.
    setenv("GXV_HSR_AUTO", "0", 0);
    setenv("GXV_AUTOQ_ANISO_RUNG", "2", 0);
    // When the CPU (not the GPU) is the limit and a pair of game turns just misses 33 ms, keep every second turn's picture and let
    // the game run up to 20 % slower (as the Wii did) instead of halving the pictures; beyond 20 % the old skipping returns.
    setenv("GXV_SLIP", "1", 0);
    setenv("GXV_SLIP_MAX", "20", 0);
    // Each picture shown on the vblank that matches its game turn (frame_pace.h PresentSync): with turns left out (Game speed Full,
    // ~45 pictures a second) the time on screen between two pictures is then the game time between them, so Mario's displayed
    // speed is even instead of jumping between double and half for a picture. Costs up to about a vblank of latency (every picture
    // waits for the slower ones' slot). GXV_PRESENT_SYNC=1 in env.txt: measure only (the "present:" line, pictures shown when ready
    // as before); 0: off. Off on its own under GXV_PACE / the fixed 30 cap.
    setenv("GXV_PRESENT_SYNC", "2", 0);
    // pace24 (10 Nov): GXV_PACE24=1: the steady-30 rule reads what the display really showed (frame_pace.h HoldMeter: pictures held for a different number of
    // vblanks than the one before are "breaks"), decides over 30-turn windows (engages under 50 pictures a second with 7+ breaks a second for 1.5 s, releases at
    // 57+ estimated for 2 s at a camera cut or a quiet moment, not within 8 s of an engage) and no longer undoes a release at a cut 7-13 ms later (c27 760/768: all
    // 8 cut releases were undone, each doubling the release block to 240 s). Default 0 until the console A/B; the sub-switches are listed in perf.cpp (GXV_PACE24_*).
    // GXV_SMOOTH_LOG (default 1, 0 = off) / GXV_SMOOTH_LOG_S (10): the "smooth:" line from the display callback: pictures, late, longest hold, holds 16/33/50/67+ ms,
    // breaks, pacer state changes. GXV_BOOT24_MENU_N / GXV_BOOT24_MENU_MS (default 0 = off): end the start-up menu-list warm-up queue after that many draw variants / ms.
    // Console A/B on c31 (10 Nov, Honeyhive, Performance defaults): with 0 the run sat on the steady 30 for most of the level (30.0 fps at
    // 720x404, re-engaged after every release); with 1 it ran 52-54 fps at 664x376 off the steady 30 (holds 16/33/50 ms ~476/44/12 per 10 s).
    // The owner's target is 60: 1.
    setenv("GXV_PACE24", "1", 0);
    // latency24 (10 Nov): GXV_LATENCY24 (default 0 = exactly the behaviour before): 1 = the present latency L comes down on evidence while pictures are held
    // (frame_pace.h Latency24; Good Egg at 60: L5 -> L3 in the model, 83 -> 50 ms pad to flip), 2 = also a picture whose slot the one before holds replaces it (catch-up).
    // GXV_LATENCY24_PAD (default 1, only with GXV_LATENCY24 >= 1): the game thread polls the pad once more right before its KPADRead (the poll thread's sample is up to 6 ms old).
    // GXV_LATENCY24_DEPTH (default 0 = as before): 1 / 2 / 3 caps the pictures the game thread may have in the front end / back end (GXV_INFLIGHT is 2).
    // GXV_LATENCY24_LOG (default 1, 0 = off) / GXV_LATENCY24_LOG_S (10): the "latency:" line: pad sample to flip (mean / 95th / max ms), pictures in flight, late pictures.
    // pacer25 (11 Nov): GXV_PACE25 (default 0 = exactly the behaviour before; 1 = on; 2 = census: decides and logs what it would do, the legacy rule acts): the even
    // cadence for work per picture between one and two refreshes (platform/include/port/pace25.h). With GXV_PRESENT_SYNC=2 the hold on screen is the number of turns a
    // picture advanced, and the legacy rule's back-end-backlog test (>= 2 pictures) is a late, quantised signal: between about 1.2 and 1.4 refreshes of work it draws
    // 3-4 turns in a row and leaves out two (holds 1-1-1-3, 50 ms; c69 Toy Time 67/27/5 % -> 73/5/22 % holds of 1/2/3+ with every part faster). On: the pictures
    // are a fixed even rhythm from the measured cost (never a hold over 2 while the work fits two refreshes), the backlog only a guard (GXV_PACE25_GUARD, 3), a scene
    // that fits one refresh is not touched; logic turns stay 60 a second. GXV_PACE25_MARGIN (3 %), GXV_PACE25_LOG_S (5, "pace25:" line). Console A/B: agents/pacer25/REPORT.txt.
    setenv("GXV_PACE25", "0", 0);
    // pace26 (12 Nov): GXV_EVEN26 (default 0 = exactly the behaviour before; 1 = on; 2 = census) the Bresenham pacer for Performance mode at 50-59 fps (platform/include/port/even26.h):
    // a picture every `period` turns (a real number from the slowest stage's cost, snapped to exactly 1 and exactly 2), the doubles spread evenly, the back end's busy verdict only trims it.
    // PROPOSED default 1 after one console A/B (model: ideal cadence 65 -> 85 %, missed moments 4.9 -> 0.4 a second, judder 0.24 -> 0.10, about -0.4 fps; an estimate, not measured).
    // GXV_EVEN26_Q (default 0; 2/3 = pending that counts as busy at the fixed 30's due turn) and GXV_EVEN26_QSLACK (0; 1..3 vblanks of room in the locked 30's slots, costs that latency once).
    // GXV_EVEN_LOG (default 1) / GXV_EVEN_LOG_S (10): the "even:" line, the smoothness score (ideal %, judder, latency, missed moments), in every mode.
    setenv("GXV_EVEN26", "0", 0);
    setenv("GXV_EVEN26_Q", "0", 0);
    setenv("GXV_EVEN26_QSLACK", "0", 0);
    setenv("GXV_EVEN_LOG", "1", 0);
    setenv("GXV_LATENCY24", "0", 0);
    setenv("GXV_LATENCY24_LOG", "1", 0);
    setenv("GXV_SMOOTH_LOG", "1", 0);
    setenv("GXV_BOOT24_MENU_N", "0", 0);
    setenv("GXV_BOOT24_MENU_MS", "0", 0);
    // Consecutive rigid draws that differ only in their matrix drawn as one, through the 10-matrix palette (GXV_RIGIDMERGE=1;
    // 2 = the 4-matrix palette). GXV_RIGIDMERGE_DLMAX (small cached lists joining runs; =learn copies only where it joins) stays a test switch.
    setenv("GXV_RIGIDMERGE", "1", 0);
    // fe23c (8 Nov): GXV_STATIC_INST: consecutive uses of one cached list's static run that differ only in their matrix (flower groups'
    // plants, other instanced models drawn by DrawBufferShapeDrawer) drawn as ONE GL draw from a 10x replicated copy of the run with the
    // GXV_RIGIDMERGE palette (vgx_draw.cpp "static instances", vgx_dlcache.cpp "instance buffers"). Exact as GXV_RIGIDMERGE is. Saves
    // back-end (core 1) and GPU per-draw work, not FE work. 0 = off until the console A/B; n >= 1 on (a list needs repeated uses in
    // max(2, n) pictures). GXV_STATIC_INST_VERTS (512) / GXV_STATIC_INST_KB (3072): per-list and total limits. "static-inst:" log line.
    setenv("GXV_STATIC_INST", "0", 0);
    // bowl32 (20 Nov): GXV_DL_DEDUP, lists whose decoded geometry (vertex and index bytes, run table) is equal share one static buffer (reference counted), and with
    // GXV_STATIC_INST one instance buffer, so that the uses of the same model by different actors are "the same run of the same buffer" for GXV_STATIC_INST and
    // GXV_BE_MERGE. Exact (same bytes drawn); the key is three 128-bit hashes. 0 = off until the console A/B, 1 = on, 2 = census ("dl-dedup:" line, nothing shared).
    setenv("GXV_DL_DEDUP", "0", 0);
    // starbatch25 (12 Nov): GXV_STARBATCH, the order a drawer's packets are sent in (decomp Game/System/DrawBuffer.cpp portSortOrder, JSystem J3DPortSort.hpp): in an opaque
    // depth-writing drawer, packets with the same differed list (star bits: one colour each, six colours) are sent adjacent, inside the stretch between two light loads, the first
    // packet of the stretch first and the last last. One differed list a colour instead of one a packet (game thread, front end), the state and the records of the front end
    // stay for the run, and GXV_RIGIDMERGE / GXV_STATIC_INST join the run's draws (back end, GPU). The same picture except at exactly equal depth. 0 = off; 1 = on; 2 = plan,
    // check and count, draw as before ("starbatch:" line every 300 pictures; verifies with mismatches = "plan checks failed" 0). GXV_STARBATCH_MIN (4): fewest packets of a stretch.
    setenv("GXV_STARBATCH", "0", 0);
    setenv("GXV_STARBATCH_MIN", "4", 0);
    // starbatch26 (12 Nov): GXV_STARBATCH=2 now also sends the planned order once more after the drawer's own (same picture) and compares, per packet, the stream bytes of the
    // shape draw and the differed list in force ("starbatch: census" line: VERIFY ... must say 0 differed; the loop times old / new when GXV_TURN_PROF is on).
    // GXV_STARBATCH_DL (0 off, 1 on, 2 verify; needs GXV_DL_STATIC): a star bit whose colour is what its last material-list refresh used leaves the refresh out (ModelManager.cpp).
    // GXV_STARBATCH_FAR=<metres> (0 = off; stage-settable): a floating star bit farther than this from the camera and the player is posed every second picture / turn only.
    setenv("GXV_STARBATCH_DL", "0", 0);
    setenv("GXV_STARBATCH_FAR", "0", 0);
    // Static runs of cached display lists (the map's shapes) whose bounds are outside the clip volume are not drawn: no resolution,
    // no job, no record, no GL draw; the picture is the same (vgx_draw.cpp "static runs outside the view"). 0 in env.txt: off.
    // The "ta23:" basic-log line (every 10 s) counts them.
    setenv("GXV_STATICCULL", "1", 0);
    // 2.3 ta23 (GPU vertex/TA work), all off here, for the console A/B (vgx_draw.cpp "static runs outside the view", vgx_dlcache.cpp):
    //   GXV_STATICCULL_PAL=1  the same exact cull for static runs with per-vertex matrices (skinned / multi-joint shapes), by the
    //                         list's per-row boxes;
    //   GXV_DYNCULL=N         dynamic (CPU-decoded) draws of >= N vertices whose vertices are all outside one clip plane: not drawn (exact);
    //   GXV_DLVCACHE=1        (with GXV_DLWELD=1) welded static runs also get a vertex-cache triangle order (Tipsify; 2 Forsyth), drawn only by opaque
    //                         depth-writing draws (a minor change at exact depth ties inside one shape; see vgx_dlcache.cpp).
    setenv("GXV_STATICCULL_PAL", "1", 0);
    setenv("GXV_DYNCULL", "1", 0);
    setenv("GXV_DLVCACHE", "0", 0);
    // vertex25 (11 Nov; platform/vgx/src/vgx_draw.cpp "clusters of a static run", vgx_dlcache.cpp BuildClusters, vgx_shadergen.cpp, vgx_backend_gl.cpp "vtxc"),
    // all off until the console A/B (agents/vertex25/REPORT.txt):
    //   GXV_CLUSTERCULL=1   a big rigid static triangle run is drawn as the ranges of its indices that have a cluster (about 64 consecutive triangles,
    //                       boxed at promotion) in view: head and tail trims always, an interior gap only above GXV_CLUSTERCULL_GAP (3000) indices;
    //                       exact (the whole-run cull's own test and margins per cluster). =2 dry (counts, draws whole), =3 on + checks. "vtx25:" line.
    //   GXV_VS_LTSKIP=1     lit vertex shaders skip the loop body of a light whose colour is exactly 0 (a uniform branch; exact for finite lights).
    //                       Changes every lit vertex shader's text: needs a pack re-seed to ship; for the A/B the new texts compile at run time.
    //   GXV_VTX_CENSUS=1    census only: the "vtx25-lit:" line (draws and indices by size, light loops, loops of colour 0).
    setenv("GXV_CLUSTERCULL", "0", 0);
    setenv("GXV_VS_LTSKIP", "0", 0);
    setenv("GXV_VTX_CENSUS", "0", 0);
    // lowres25 (11 Nov): GXV_FXLOWRES: a reduced-size buffer for the 3D picture's blended effects ("low-res particles"; a slightly noticeable change).
    //   =3  census only (no picture change): every 4th picture the depth-tested blended effect draws that would move (farther than GXV_FXLOWRES_NEAR = 2500
    //       game units from the camera plane; nearer ones are protected), their fill in screens and the estimated pixel time saved / spent: "fxlowres:" line.
    //   =1  the path: the largest run (GXV_FXLOWRES_RUNS=1) of such draws with at least 1.5 screens of fill (GXV_FXLOWRES_MIN=1500) is drawn into a target of
    //       half the width and height with the scene's depth and laid over the picture once (two more scenes a run); GXV_FXLOWRES_STATIC=1 also takes static
    //       translucent model parts that write no depth (distance not known: unprotected, an experiment); GXV_FXLOWRES_STEP=1: only while the controller's
    //       "effects at half size" step is on. =2 (half the width only) is not built.
    //   The two tours and the per-scene logs say particles are far too small on the screen to pay for the extra scenes (agents/lowres25/REPORT.txt): default 0.
    // GXV_FXLOWRES_CEILING=1..4 is a DIAGNOSTIC with a wrong picture on purpose: a class of blended 3D draws is not drawn (1 every candidate, 2 the static
    // translucent model parts without depth write, 3 the stream ones, 4 every depth-tested blended draw), so that one A/B of the pixel time says what the
    // class costs = the most a reduced-size buffer could ever take. Never a shipping value.
    setenv("GXV_FXLOWRES", "0", 0);
    setenv("GXV_FXLOWRES_CEILING", "0", 0);
    for (const char* name : {"GXV_FXLOWRES_STATIC", "GXV_FXLOWRES_K1", "GXV_FXLOWRES_K2", "GXV_FXLOWRES_K3", "GXV_FXLOWRES_K4"}) setenv(name, "0", 0); // (freeze25: stage keys fxlow_static / fxlow_k1..k4)
    // compositor25 (12 Nov): the effects compositor with an automatic choice of what it takes. GXV_FXLOWRES_AUTO=1: per stage, each blended shader key's fill (screens, overlap
    //   counted) is measured over a window of observed pictures and a key whose fill is at least GXV_FXLOWRES_AUTO_MILLI (750 = 0.75 screens), mostly far and not thin, and whose run
    //   pays for its two extra scenes (the budget rule), is routed to the reduced-size target for the rest of the stage ("fxauto:" log lines, one per decision); =2 census (the same choice
    //   is made and logged with the runs, scenes, fill and ms it would move; nothing changes). GXV_FXLOWRES_UPS=1: nearest-depth composite (removes the edge ring; needs the shader
    //   pack refilled or a runtime compile at the stage load); GXV_FXLOWRES_DIV=2|4 forces the size (0: half, quarter from 8 screens of fill); GXV_FXLOWRES_THIN=<tenths of a texel>
    //   (default 20 with AUTO): draws narrower than that stay at full size; GXV_FXLOWRES_ALPHA=1: draws that write alpha may move when nothing after them reads the EFB's alpha.
    //   AUTO also keeps a CPU copy of the static buffers (up to 2 MB each, GXV_FXLOWRES_KEEP_MB=10 in all) to measure static translucent parts. All default 0 (stage keys fxlow_auto ...).
    setenv("GXV_GLOW26", "0", 0); // glow26 (port/glow26.h): 0 off (Quality), 1 mild, 2 strong: raises the compositor, ind_flat, bloom_fold and lightvol; GXV_GLOW26_VERIFY=1|2 the A/B check
    for (const char* name : {"GXV_FXLOWRES_AUTO", "GXV_FXLOWRES_UPS", "GXV_FXLOWRES_DIV", "GXV_FXLOWRES_DEFER"}) setenv(name, "0", 0);
    // compfix26 (12 Nov): the compositor never routed on the console because the near limit (GXV_FXLOWRES_NEAR 2500 units, nearest vertex) is larger than the play camera's distance
    //   (~1300 units), so every glow sheet around Mario was "near". With AUTO a draw of at least GXV_FXLOWRES_NEAR_BIG thousandths of a screen (default 150) is a layer and is near only
    //   when its nearest vertex is under GXV_FXLOWRES_NEAR_MIN percent of the limit (default 25); NEAR_BIG=0 restores the old rule. GXV_FXLOWRES_FORCE=1|2 (default 0) bypasses the
    //   automatic choice for A/B runs (every measured key with >= GXV_FXLOWRES_FORCE_MILLI (50) thousandths of unblocked fill is routed at once, half / quarter size, no budget).
    //   With AUTO, FORCE or GXV_GLOW26 non-zero the log has "fxlowres: stage ...: level L, candidates N (by reject reason: ...), routed M keys ..." lines (every 600 pictures and at the stage's end).
    setenv("GXV_FXLOWRES_FORCE", "0", 0); // (THIN and ALPHA are left unset: their default follows AUTO)
    // compositor25 round 2: GXV_FXLOWRES_DEFER=1: the routed additive layers (ONE,ONE or SRC_ALPHA,ONE; no depth write) of a stretch between two sampled captures are drawn once, where the
    //   main depth is final (one depth copy, one composite); a later non-additive blended draw over them keeps them in place; =2 also crosses captures (an experiment: refractions then sample
    //   a picture without the layers). GXV_FXLOWRES_DEFER_TOL=<thousandths of a screen> of later non-additive fill that may overlap (0 exact). The size is a quarter only when >= 90 % of the
    //   fill is soft (textures cached at upload) and >= 4 screens. GXV_FXLOWRES_TRI=0 draws the nearest-depth composite as a quad again (default 1: one full-screen triangle); fallback for
    //   the depth-read-while-bound risk on the console: GXV_FXLOWRES_UPS=0.
    // geofeed23 (8 Nov): GXV_VTXPACK=1: cached lists' static runs keep the game's compact fixed-point attributes (positions 4 x s16,
    // normals 3 x s16 / s8, tex0/tex1 2 x s16) instead of floats, about half the bytes the GPU's vertex fetch reads; the 2^-k of each
    // goes into the matrices that multiply it (vgx_dlcache.cpp "packed static runs", vgx_draw.cpp VqScaleBlocks): bit-exact, no new
    // shader text. 0 = off until the console A/B. GXV_VTXFEED=1: the "vtxfeed:" basic-log line every 5 s (bytes fed a picture, packed
    // and as floats). Both read once.
    setenv("GXV_VTXPACK", "0", 0);
    // farlod32 (13 Nov): GXV_FACECULL: a rigid static run (a map shape) drawn with back- or front-face culling is drawn from a "view list":
    // only the triangles that can face the camera from where it is, with a compact copy of their vertices, in a static buffer of its own
    // (vgx_facecull.h; made again when the camera has moved GXV_FACECULL_MARGIN percent of its distance; budget GXV_FACECULL_BUDGET triangles
    // a picture; GXV_FACECULL_MIN indices; GXV_FACECULL_KB of kept copies). The left-out triangles are ones the GPU culls: the same picture.
    // 0 = off until the emulator's test picture (=3: the runs must vanish) and a console A/B; 2 = census only. "facecull:" log line.
    setenv("GXV_FACECULL", "0", 0);
    setenv("GXV_VTXFEED", "0", 0);
    // geopass23 (8 Nov, platform/vgx/src/vgx_geopass.inc), both off until the console A/B (agents/geopass23/REPORT.txt):
    //   GXV_OCCLCULL=1  depth-tested, unblended draws whose every point lies behind the inscribed ball of a closed occluder drawn earlier
    //                   in the same view and depth epoch (a planet's run: a closed, consistently wound surface) are not drawn (exact);
    //                   =2 counts only. "occlcull:" basic-log line every 10 s.
    //   GXV_GEOPASS=1   diagnostic: the "geopass:" line every 10 s (the picture's EFB scenes and what ended each, the GX draws and indices
    //                   of each game draw phase, geometry drawn twice, the occluder census); the game writes its phase markers only then.
    setenv("GXV_OCCLCULL", "0", 0);
    setenv("GXV_GEOPASS", "0", 0);
    // Frame pipeline (1.2): the game thread alone on core 0 (every other thread on cores 1-2); under a frame cap the queue depth
    // follows the back end's own CPU time, not its wall time (which includes the display wait and kept the queue at one frame,
    // so the game and the renderer stopped overlapping); a vblank wait that returned counts as a passed vblank.
    // The console DSP's mixing on its own thread off core 0 (bit-identical PCM); the end-of-turn draw-done wait moved to the
    // start of the next update (GXV_DRAWDONE_DEFER=1; 2 waits later still); disc read-ahead and one movie decoder on the
    // system core when the console allows it (GXV_CORE3_IO, silent fallback to cores 1-2).
    for (const char* name : {"GXV_CORE0_ALONE", "GXV_FIXED_DEPTH", "GXV_PACE_VBLANK_COUNT", "GXV_AUDIO_THREAD",
                             "GXV_CORE3_IO"}) setenv(name, "1", 0);
    // 1.3: the draw-done wait at the start of the next calcAnim() (2), so the game's logic (update) runs while the front end
    // finishes the picture: up to the whole update() a drawn turn (~6 ms at the Observatory) comes off the game thread's path.
    // GXV_DRAWDONE_DEFER=1 in env.txt gives 1.2's wait at the start of update().
    setenv("GXV_DRAWDONE_DEFER", "2", 0);
    // 2.3 (titlehang23, issue 3 "r110 does not leave the title in Performance mode"): a draw-done wait asked for by the LOADING thread
    // (a model's first view pass or list while a stage is built) no longer takes the game thread's deferred wait; it waits for the
    // outstanding finish on its own, by the count of GXSetDrawDone calls, so the game thread resetting the one-bit flag first cannot
    // starve it (at 60 pictures a second with every turn drawn the Observatory's load never finished: a black screen for good).
    // port/drawdone_other.h. GXV_DRAWDONE_OTHER=0 in env.txt: the shared wait of 2.2 / r110 (A/B only).
    setenv("GXV_DRAWDONE_OTHER", "1", 0);
    // 2.3 defaults (8 Nov, console 472-verify + 480..485 A/B, Honeyhive / Good Egg, Performance): every exactness check 0 mismatches
    // (sensors 553k, direct J3D 882k, clip 174k, light find, FE parallel 17.6k, particles 20k, fog self-test); the bundle took
    // Honeyhive TA 12.9 -> 10.4 ms, FE 22.0 -> 19.0 ms, game turn 14.1 -> 13.7 ms, Good Egg TA 9.1 -> 7.8 ms; screenshots identical.
    // Switches whose code default is 0 are turned on here (env.txt still wins). Shader-text changes (VSOUT, TEXVEC2, TEVIN_CANON,
    // FS_FINCLAMP) need the 2.3 shader refill in the package (agents/pack23 RUNBOOK) before release.
    setenv("GXV_LIGHT_DEDUP", "2", 0);
    setenv("GXV_DLWELD", "1", 0);  // 13 Nov: console A/B at Beach Bowl (c90, 960x544, geometry-bound): 39.0 fps with 1, 37.3 with 0 (the GPU's vertex work pays per distinct vertex). The "no gain" verdict came from stages that were not geometry-bound.
    setenv("GXV_VSOUT", "1", 0);
    setenv("GXV_TEXVEC2", "1", 0);
    setenv("GXV_OC_CLIPFAST", "1", 0);
    setenv("GXV_OC_LIGHTFIND", "1", 0);
    // forkB (6 Nov; after Producdevity's fork, ee487a3 "view pass before each picture"): a turn without a picture leaves its view and
    // draw-list pass to the flush before the next picture (GameScene::calcAnim, port_view_flush), except the player's, which keeps
    // its pass every turn (PETARI_VIEW_PLAYER=1: Mario's shadow mode, lighting fade, footprints). Only the last pass before a picture
    // is ever read. Fork's console: +0.6 to +2.5 fps (Beach Bowl, Toy Time, Honeyhive, Observatory), its hashes of every draw-buffer
    // actor's matrices equal with and without. PETARI_SKIP_VIEWCALC=0 in env.txt: the pass in every turn, as before.
    setenv("PETARI_SKIP_VIEWCALC", "1", 0);
    // 1.3: the DSP mixing thread on core 1 (the render thread's, which has room), not cores 1-2: core 2 is the front end's player,
    // the stage nearest its limit. GXV_AUDIO_THREAD_CORE=0 in env.txt: cores 1-2 as in 1.2.
    setenv("GXV_AUDIO_THREAD_CORE", "1", 0);
    // sched24 (10 Nov; placement and priority experiments, every one unset = as before; agents/sched24/REPORT.txt has the console lines):
    //   GXV_CORES_LOG (unset: on with GXV_TURN_PROF or GXV_WORKLOAD_PROBE) the "cores-5s:" lines, each thread's CPU a picture on each core;
    //   GXV_CORE_PROBE=1 wake-up lateness of probe threads on cores 1 and 3 ("cores-5s wake-lat:"); GXV_HELPER_CORE=1|2 / GXV_CLOCK_CORE=1|2 the helper /
    //   clock threads on that core alone (off the front end's core 2); GXV_PAR_PRIO_ALL=<0x40-0xBF> every par worker's priority (0x9C: above the
    //   front-end finishing thread, 0x9E, on the fourth core); GXV_AUDIO_CPU1_CORE=1|2|3 and GXV_AUDIO_PRIO the game's audio thread's core / priority.
    // The game's audio thread (JAudio's DSP frames, about 6.7 wake-ups a turn, all on core 0) works while the game thread sleeps
    // (the front-end wait, the retrace wait) instead of in the middle of its turn: the AI interrupt and a DSP frame's closing mail
    // wait up to 8 ms for the game thread's next sleep; a frame's sub-frames still follow each other at once (os_thread.cpp,
    // GXV_AUDIO_HOLD). The same handlers in the same order, only later. GXV_AUDIO_HOLD=0 in env.txt: taken at the next safe point.
    setenv("GXV_AUDIO_HOLD", "8", 0);
    // 1.3 exact work reductions, each checked against the original path by its oracle mode (=2) over full native game runs with
    // zero mismatches: 12-word position/texture matrix loads batched in the command stream (GXV_XFBURST), and no effect-candidate
    // scan in buffers that cannot have an effect pass (GXV_FXSCANFAST). NAME=0 in env.txt turns one off.
    for (const char* name : {"GXV_XFBURST", "GXV_FXSCANFAST"}) setenv(name, "1", 0);
    // 1.3, the GXM rebuild's first layer (vgx_backend_gl.cpp GxmDirectOn): a scene draw is one direct vitaGL call with one vertex
    // stream instead of a dozen attribute calls and a hashed vertex-program lookup. GXV_GXM_DIRECT=0 in env.txt: the 1.2 path.
    setenv("GXV_GXM_DIRECT", "1", 0);
    // 1.3: CPU-transformed (mergeable) draws only up to 32 vertices (was 128): measured on the console (Observatory route, 3 Nov) the
    // front end's CPU per picture went from 20.5 to 16.5 ms and the frame rate from 39.9 to 45.3 FPS; larger draws are transformed by
    // the vertex shader, as static geometry is. GXV_SOFTXFORM=128 in env.txt: the previous limit.
    setenv("GXV_SOFTXFORM", "32", 0);
    // 1.3: particles with a texture animation of their own (JPA's per-particle texture matrix, JPALoadCalcTexCrdMtxAnm) merge into one
    // draw like the others: a CPU-transformed draw's 2x4 texture matrix is applied to its coordinates by the front end and its block
    // holds identity rows (vgx_draw.cpp TexBakeMask; tools/test_vgx_texbake_gl.sh: identical pictures on Mesa, 116 -> 34 draws in its
    // particle scene). GXV_TEXBAKE=0 in env.txt: a draw per such particle, as before.
    setenv("GXV_TEXBAKE", "1", 0);
    // Game-thread exact work reductions (NAME=0 in env.txt turns one off): GX commands of several elements written as one batch
    // with one room check, the same bytes (GXV_FIFO_BATCH, port/gx_fifo.h, tools/test_fifo_batch.sh); the skinned models'
    // weight envelopes kept in NEON registers, the same arithmetic (GXV_J3D_NEON, J3DMtxBuffer.cpp, tools/test_j3d_envelope_neon.sh).
    for (const char* name : {"GXV_FIFO_BATCH", "GXV_J3D_NEON"}) setenv(name, "1", 0);
    // The Observatory's not yet powered base models (floors, central metal) are near black in the original until their Grand
    // Stars are back (AstroMapObj.cpp, PETARI_ASTRO_DEAD_LIGHT): on the Vita's screen they read as missing floors, so they are
    // held 30 % into their power-up colour animation: dim but visibly textured. PETARI_ASTRO_DEAD_LIGHT=0 in env.txt: the original look.
    setenv("PETARI_ASTRO_DEAD_LIGHT", "30", 0);
    // While the pause menu is up, the frozen 3D scene behind it is drawn from a copy of its picture taken as the menu opens (one
    // screen-sized rectangle) instead of the whole scene and its effects again every picture (GameScene.cpp pauseFreezeStep;
    // ~88 ms of GPU a picture, ~10 FPS, measured on 4 Nov). PETARI_PAUSE_FREEZE=0 in env.txt: the scene drawn every picture as
    // before; =2: only while no effect that moves in a pause is alive (exact).
    setenv("PETARI_PAUSE_FREEZE", "1", 0);
    // boss23 (GameScene.cpp, end): the boss-fight cuts, all MINOR CHANGES and off by default (unset is off too): PETARI_BOSS_PTCL (particle
    // emission thinned while a boss fight has more than PETARI_BOSS_PTCL_CAP particles alive), PETARI_BOSS_PTCL_GROW (the thinned storm's
    // particles larger so its area is kept), PETARI_BOSS_HAZE (no heat-haze shimmer in a boss fight), PETARI_BOSS_PSHADOW (boss projectiles'
    // sphere shadow volumes drawn as flat discs). "perf" in env.txt: on in Performance mode only; 1: in every mode. PETARI_BOSS_LOG=1: the
    // "boss:" lines of the boss tour.
    for (const char* name : {"PETARI_BOSS_PTCL", "PETARI_BOSS_HAZE", "PETARI_BOSS_PSHADOW", "PETARI_BOSS_PTCL_GROW"}) setenv(name, "0", 0);
    // gxm23 (vgx_backend_gl.cpp Upload, vgx_host_galaxy_vita.cpp vgxvita_uploadSwizzled): GXV_TEX_SWIZZLE=1 stores the uncompressed game
    // textures (RGBA8, the 16-bit formats, LA88; power-of-two 8..1024) SWIZZLED instead of vitaGL's linear layout, as the DXT1 ones
    // already are: same texels and mip levels, a cache-friendly layout for the GPU's texture reads. =2: also logs every texture made.
    // Off until a console A/B (GXV_GPU_SCENES=1, GXV_TEXFMT_LOG=30).
    setenv("GXV_TEX_SWIZZLE", "1", 0);
    // texup27 (platform/vgx/src/vgx_texsoft.h): GXV_TEXSOFT_FAST=1 makes the texture softness census (armed by GXV_FXLOWRES_AUTO or GXV_GLOW26 > 0) one pass over the texels with the
    // old verdict (host test tests/texsoft_model.cpp: 4,000 textures, 0 differences). Console logs: 3.1-4.5 ms an upload with the census armed, 0.35-0.5 without, flat in the bytes.
    // Safe: same verdict, no new state. "texup:" lines say whether it acted (soft census fast / OLD / off, uploads' split).
    setenv("GXV_TEXSOFT_FAST", "1", 0);
    // texcomp23 (8 Nov; platform/vgx/src/vgx_texcomp.cpp): GXV_TEX_COMPRESS=1|perf converts the large uncompressed colour textures (RGBA8,
    // RGB5A3, RGB565, palettes, CMPR with transparent texels; not I/IA, not 2D / indirect / small / changing ones) to DXT1 / DXT5 a few
    // pictures after their upload, on a background thread (the fourth core when proven), cached in ux0:data/smg/texcomp.idx/.dat; each
    // texture kept only above a quality floor (GXV_TEX_COMPRESS_PSNR, 32 dB). A MINOR CHANGE (lossy): 1 every mode, perf Performance only.
    // "tex-compress:" basic-log line. GXV_TEXCOMP_CENSUS=1 (diagnostic): "tex-census:" texture bytes read a picture. Off until the
    // console A/B (agents/texcomp23/REPORT.txt).
    setenv("GXV_TEX_COMPRESS", "0", 0);
    // automip25 (platform/vgx/src/vgx_automip.h, vgx_texcache.cpp "GXV_AUTOMIP"): GXV_AUTOMIP=1 gives a mip chain to textures the game declared without
    // one (single level, linear filter; opaque RGB565 / RGB5A3 / RGBA8 / CMPR, powers of two, >= 64 texels, not I/IA / palettes / HD pack / Mario) once
    // they have been drawn in 3D and in nothing else, and samples them with LINEAR_MIPMAP_NEAREST (GXV_AUTOMIP_FILTER=2: trilinear) at half a level
    // sharper (GXV_AUTOMIP_BIAS=-4): far surfaces stop sparkling. =2: census only ("automip-census" lines every 10 s). Extra memory capped by
    // GXV_AUTOMIP_KB (6144). Off until the census and a console A/B (agents/automip25/REPORT.txt).
    setenv("GXV_AUTOMIP", "0", 0);
    // hud24 (platform/vgx/src/vgx_backend_gl.cpp "namespace hudn"; the host's two-halved present hook, vgx_host_galaxy_vita.cpp): GXV_HUD_NATIVE=1 draws
    // the 2D layer (HUD, text, menus, the star pointer, layouts) at the panel's own 960x544 over the upscaled 3D picture, straight into the display's
    // scene (no extra scene), instead of in the rung-sized EFB. A picture that cannot (a 3D draw after the 2D, a capture of the 2D, a rung at the
    // panel's size, shader work) is drawn as before. =2: plan and count only. "hud-native:" basic-log line every 600 pictures. Off until a console A/B.
    setenv("GXV_HUD_NATIVE", "0", 0);
    // big60 (platform/vgx/src/vgx_prebuild_pick.h): GXV_RUNG_PREBUILD_START=1 makes the pieces of a size change (render targets, copy textures) be made ahead for the START rung of a stage
    // whose preset fixes its size (size=fixed row), at any distance below the current rung, in the quiet moments before the stage opens (star select, load). Default 0: the two rungs under the
    // current one, as before (right for 848x480 from 960x544, useless for a row that starts at 720x404). A stage that starts below 848x480 also needs `emergency=<rung>` in its row.
    setenv("GXV_RUNG_PREBUILD_START", "0", 0);
    // direct24 (vgx_directdisplay.h, vgx_backend_gl.cpp "namespace directd", vgx_host_galaxy_vita.cpp, vitaGL patch 0030 = libvitaGL-reader-vp28.a): GXV_DIRECT_DISPLAY=1
    // shows a picture that is a plain copy of the whole EFB (rung 6, 960x544, no sharpen / edge smoothing, the copy clears the whole EFB) by queueing the
    // EFB's own colour buffer to the display (the EFB's colour buffers are display textures with their own sync objects, rotated), with no present scene.
    // Every other picture, and every doubt, is the present as before. =2: the pictures as with 0 plus a timing probe of a 1:1 sceGxmTransferCopy (the
    // route not built). "direct-display:" basic-log line every GXV_DIRECT_DISPLAY_LOG (600) pictures. Off until a console A/B (agents/direct24/REPORT.txt).
    setenv("GXV_DIRECT_DISPLAY", "0", 0);
    // gpuheavy25 (11 Nov): GXV_AUTOQ_SHED=1 (target 60 only): the shed order as a table of levels, cheapest to the picture first: the edge pass (with direct display
    // at 960x544 when GXV_DIRECT_DISPLAY=1), the half-rate captures (GXV_AUTOQ_SHED_CUTS=1, armed by GXV_INDCAP_HALFRATE=2), then one rung at a time to 720x404;
    // every level judged by what it bought, the way back at a camera cut; GXV_AUTOQ_SHED_CYCLE=<s> walks every level for the stress run. "autoq-shed:" log lines.
    // Off until the console run in agents/gpuheavy25/REPORT.txt.
    setenv("GXV_AUTOQ_SHED", "0", 0);
    // slow24 (vgx_backend_gl.cpp "namespace slow24", vitaGL patch 0031 = libvitaGL-reader-vp29.a): the back end asks vitaGL for the free video memory on the
    // copy path (every GXV_EFBSWAP hand-over, the effects, the hoist...); vglMemFree is sceClibMspaceMallocStats, which walks every block of the CDRAM pool,
    // so each question costs more the more textures are alive (the session slowdown: copies 5.5 -> 24 ms a picture in Bowser's stage over 18 minutes).
    // =1: vitaGL's O(1) figure (vglMemFreeFast; needs vp29, else as 0); =2: asked once a picture. Pictures are the same in every mode (only buffer-keeping
    // decisions against 6+ MiB thresholds read it). Off until the console A/B; GXV_BE_PROF's "be-prof copies:" line shows the calls and their cost.
    // Console A/B 10 Nov (run 860, two-stage loop, 15 visits, no texture drop, 1,822 live video blocks): =1 keeps the copies at ~1.0 ms a
    // picture through every visit (=0: 6.2 -> 12.5 ms in Good Egg, 5.5 -> 24 in Bowser's stage) and the back end at 8.9 / 12.4 ms a picture
    // (was 13.9 -> 21 / 17 -> 35); each question costs 2 us. Default 1.
    setenv("GXV_SLOW24_VRAMFREE", "1", 0);
    port_host_big_boot();  // (heap23: GXV_HEAP_BIG=2 arms here; 1 at the first scene change)
    // This thread, which started with no restriction, leaves core 0 as soon as GXV_CORE0_ALONE is known.
    port_vita_thread_helper();
    LoadInputScript(SMG_DATA "/input.txt");
    {
        // Request 500 through the normal API; a supporting OC plugin may grant it or retain its manual policy.
        // Stock systems can refuse it: fall back to 444 only on failure. GXV_VITA_CLOCKS=0 leaves requests untouched.
        const char* e = std::getenv("GXV_VITA_CLOCKS");
        const bool requests = !(e && *e && std::atoi(e) == 0);
        int arm = 0, fallback = 0, bus = 0, gpu = 0, xbar = 0, xbarRequest = 166;
        unsigned preMHz = 0;
        bool fallbackTried = false;
        if (requests) {
            // A clock plugin (PSVshellPlus with a 500 MHz profile for SMGV00001) can already have the core at 500 while the
            // system refuses this request (0x802B0000): then a 444 request would at best do nothing and at worst take the
            // plugin's 500 away. So the core is measured first, and 444 is asked for only when it runs slower than that.
            preMHz = MeasureArmMHzQuick();
            const char* de = std::getenv("GXV_CLOCK_DEFER");
            SceUID deferred = -1;
            if (!(de && *de == '0')) {
                deferred = sceKernelCreateThread("smg_clockreq", DeferredArmClockRequest, 0x10000100, 0x4000, 0, 0, nullptr);
                if (deferred >= 0 && sceKernelStartThread(deferred, 0, nullptr) < 0) deferred = -1;
            }
            if (deferred < 0) {  // (GXV_CLOCK_DEFER=0, or no thread: the request at start, as before)
                arm = scePowerSetArmClockFrequency(500);
                if (arm < 0 && preMHz < 470) {
                    fallbackTried = true;
                    fallback = scePowerSetArmClockFrequency(444);
                }
                if (arm >= 0 || fallbackTried) NoteArmClockGot(arm < 0 ? 444 : 500);  // (GXV_CLOCK_KEEP)
            }
            bus = scePowerSetBusClockFrequency(222);
            gpu = scePowerSetGpuClockFrequency(222);
            // (the crossbar takes 83, 111 or 166 MHz: 222 is refused; 166 is the most, GXV_VITA_XBAR=111 measures the default)
            const char* xe = std::getenv("GXV_VITA_XBAR");
            xbarRequest = xe && *xe ? std::atoi(xe) : 166;
            xbar = scePowerSetGpuXbarClockFrequency(xbarRequest);
            // (GXV_CLOCK_KEEP: what the requests got)
            sKeptBus.store(bus >= 0 && scePowerGetBusClockFrequency() >= 222 ? 222 : 0, std::memory_order_relaxed);
            sKeptGpu.store(gpu >= 0 && scePowerGetGpuClockFrequency() >= 222 ? 222 : 0, std::memory_order_relaxed);
            sKeptXbar.store(xbar >= 0 && scePowerGetGpuXbarClockFrequency() >= xbarRequest ? xbarRequest : 0, std::memory_order_relaxed);
        }
        const int reportedArm = scePowerGetArmClockFrequency(), reportedBus = scePowerGetBusClockFrequency();
        const int reportedGpu = scePowerGetGpuClockFrequency(), reportedXbar = scePowerGetGpuXbarClockFrequency();
        if (requests) {
            port_log_summary("clocks: CPU request=500 arm_rc=0x%08X fallback444 fallback_tried=%u fallback_rc=0x%08X; bus request=222 bus_rc=0x%08X; GPU request=222 gpu_rc=0x%08X; xbar request=%d xbar_rc=0x%08X; reported CPU=%d reported bus=%d reported GPU=%d reported xbar=%d MHz (plugin-dependent API reports, not calibrated effective clocks)",
                             static_cast<unsigned>(arm), static_cast<unsigned>(fallbackTried), static_cast<unsigned>(fallback),
                             static_cast<unsigned>(bus), static_cast<unsigned>(gpu), xbarRequest, static_cast<unsigned>(xbar),
                             reportedArm, reportedBus, reportedGpu, reportedXbar);
        } else {
            port_log_summary("clocks: requests disabled (GXV_VITA_CLOCKS=0); reported CPU=%d reported bus=%d reported GPU=%d reported xbar=%d MHz (plugin-dependent API reports, not calibrated effective clocks)",
                             reportedArm, reportedBus, reportedGpu, reportedXbar);
        }
        port_log_summary("clocks: the ARM core measures %u MHz before the requests and %u MHz after them (496-500: a 500 MHz profile works; 440-444: it does not)",
                         preMHz, MeasureArmMHzQuick());
        StartClockWatch();
        StartFreezeButton();   // (14 Nov: L + R + Select for 3 s = a core dump)
        if (PORT_RUNTIME_DIAGNOSTICS) port_log("clocks: the ARM core measures %u MHz (the system reports %d; with a PSVshellPlus profile for %s the measured number is the one to read: 496-500 means the profile works, 440-444 means it does not)",
                 MeasureArmMHz(), scePowerGetArmClockFrequency(), "SMGV00001");
    }
    if (const char* mb = std::getenv("PETARI_MEMBENCH"); PORT_RUNTIME_DIAGNOSTICS && mb && *mb == '1') {
        membench::All();
        port_log_flush();
        sceKernelExitProcess(0);
        return 0;
    }
    if (const char* bench = std::getenv("PETARI_THP_BENCH"); PORT_RUNTIME_DIAGNOSTICS && bench && *bench) {
        // A movie decoder test that needs no game: plays the first pictures of a cooked movie through the decoder
        // pipeline at the movie rate, logs how many came late, and leaves (the console's cores are free then).
        // "path[@hz][;path[@hz]...]": each entry runs on its own pipeline restart (the pause between them resets it)
        std::string list(bench);
        size_t at = 0;
        while (at < list.size()) {
            size_t end = list.find(';', at);
            if (end == std::string::npos) end = list.size();
            std::string item = list.substr(at, end - at);
            at = end + 1;
            if (item.empty()) continue;
            const size_t atSign = item.find('@');
            if (atSign != std::string::npos) {
                setenv("PETARI_THP_BENCH_HZ", item.c_str() + atSign + 1, 1);
                item.resize(atSign);
            } else {
                unsetenv("PETARI_THP_BENCH_HZ");
            }
            port_log("THP bench: %s", item.c_str());
            port_thp_bench(item.c_str());
            sceKernelDelayThread(500000);  // (more than the pipeline's quarter-second pause: the next movie starts afresh)
        }
        port_log("THP bench: done");
        port_log_flush();
        sceKernelExitProcess(0);
        return 0;
    }
    if (port_settings_startup(argc, argv)) return 0;
    port_fd_guard_configure(getenv("GXV_FCLOSE_LOCK"));  // (strk1118: fclose under the fopen lock, default on)
    if (port_console_configure) port_console_configure();  // (stdio31: GXV_STDIO_SAFE, now that env.txt is in the environment; diagnostic build only)
    // gfx26: GXV_QUALITY_SET without Settings (GXV_SETTINGS=0 or a disabled menu; with Settings it was applied inside ApplyStartup and this is a no-op).
    if (port::settings::ApplyQualitySetFromEnv()) port_log_summary("%s", port::settings::QualitySetCensus().c_str());
    if (port::settings::ApplyModeKeysFromEnv()) {   // (deadfix27: the per-mode key table without Settings)
        port_log_summary("%s", port::settings::ModeKeysCensus().c_str());
        if (!port::settings::PerfDefaultsCensus().empty()) port_log_summary("%s", port::settings::PerfDefaultsCensus().c_str());   // (pace33 PATCH8)
    }
    // q30-26: the Quality (30) mode's umbrella levels when the mode is 30 by env.txt or the environment alone (with Settings, ApplyStartup did this and it is a no-op here).
    if (const char* pm = getenv("GXV_PICTURE_MODE"); pm && atoi(pm) == 30) {
        char q30[640];   // (strk1118: 320 -> 640 as settings_launch.cpp did in fmt1131; the census line is about 330 bytes and was cut)
        if (port::quality30::Apply([](const char* k) { return getenv(k); }, [](const char* k, const char* v) { setenv(k, v, 1); }, q30, sizeof q30)) port_log_summary("%s", q30);
    }
    // 1.3, the fourth core (port/core_layout.h): proven once, here, after the Settings start-up has put the Core plan row's choice
    // (GXV_CORE3=0 when Off) and env.txt into the environment, and before any thread that may ask for it starts. One basic-log line,
    // "clocks/cores: fourth core available ..." or "... not available (why)". Without the owner's CapUnlocker plug-in the probe thread
    // is refused at once and nothing moves. Defaults when proven: disc read-ahead, movie decoder 3 and the audio output thread;
    // GXV_CORE3=1 adds the DSP mixer and the front-end finishing thread (or GXV_CORE3_AUDIO=1, GXV_CORE3_SPLIT=1 one by one).
    port_vita_core3_available();
    port_splash_begin();
    if (const char* e = std::getenv("GXV_STDERR_FILE"); PORT_RUNTIME_DIAGNOSTICS && e && *e == '1') {
        // (written in batches, see port_log_file: a line-buffered file costs 12-15 ms per line on the card)
        // stdio31: the splash thread runs already and other threads may print: the reopen is made under the console lock (stdio_safe.cpp).
        auto reopen = [](void*) { std::freopen(SMG_DATA "/stderr.txt", "w", stderr); std::setvbuf(stderr, nullptr, _IOFBF, 16384); };
        if (port_console_exclusive) port_console_exclusive(reopen, nullptr);
        else reopen(nullptr);
    }
    port_vgx_vita_log_memory("at process start");
    if (!port_vita_input_init()) {
        port_log("native game input initialization failed; game boot prevented");
        return 1;
    }
    // port_boot: memory arenas, disc, GX + the renderer's thread (vitaGL start-up happens inside
    // port_gx_select_backend, so this call takes a few seconds), then the guest's own thread.
    // (this thread is the process's main thread, not the game's: the game thread is the one platform/src/os/os_thread.cpp makes and pins
    // to core 0. This one polls the pad at the clocks' priority below, where port_vita_thread_class(1) moves it to cores 1-2 as well.)
    if (!port_vita_core0_alone()) port_vita_pin_core(0);
    // This thread also polls the pad every few milliseconds.  At the main thread's own priority (0xA0, below every game thread at
    // 0x9F) it got no time at all while the game thread computed without stopping (a stage load: several seconds), so a
    // button pressed then was seen late or not at all.  It runs a hundred microseconds at a time: the clocks' priority.
    port_vita_thread_class(1);
    const bool compileAwakeBoot = [] {
        const char* e = std::getenv("GXV_COMPILE_AWAKE");
        return !(e && *e && std::atoi(e) == 0);
    }();
    g_compileAwakeBoot.store(compileAwakeBoot, std::memory_order_release);
    g_inPortBoot.store(true, std::memory_order_release);
    if (compileAwakeBoot) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT); // keepawake33: fresh window entering port_boot (also covers GXV_VITA_SPLASH=0)
    port_boot(SMG_DATA "/game", SMG_DATA "/saves");
    g_inPortBoot.store(false, std::memory_order_release);
    port_vgx_vita_log_memory("after port_boot");
    port_title_art_start();  // GXV_TITLEART: the player's own pictures, made from the title screen once it has been shown (title_art_runtime.cpp)
    int64_t previous = port_host_time_ns();
    g_padPrevNs = previous;
    {
        const char* l24 = std::getenv("GXV_LATENCY24");
        const char* l24pad = std::getenv("GXV_LATENCY24_PAD");
        // thr24 GXV_PAD_RESAMPLE=1 (default 0): the game thread's resample is registered whatever GXV_LATENCY24 said at this moment. Console night58 (Performance mode, GXV_LATENCY24=1 in the mode keys):
        // every "latency:" line reads "pad age at the game's read 3.3 ms mean, 7 max (600 reads, 0 resampled)": the resample never ran. One basic-log line says which way it went.
        const char* forcePad = std::getenv("GXV_PAD_RESAMPLE");
        const bool padForced = forcePad && std::atoi(forcePad) == 1 && !(l24pad && std::atoi(l24pad) == 0) && g_scriptCount == 0;
        const bool padOn = padForced || (l24 && std::atoi(l24) >= 1 && !(l24pad && std::atoi(l24pad) == 0) && g_scriptCount == 0);
        if (l24 && std::atoi(l24) >= 1) port_log("latency: GXV_LATENCY24=%s, game-thread pad resample %s", l24, padOn ? "on" : "off");
        port_log_summary("pad: game-thread resample %s (GXV_LATENCY24=%s, GXV_PAD_RESAMPLE=%s)", padOn ? "registered" : "not registered", l24 ? l24 : "unset", forcePad ? forcePad : "unset");
        if (padOn) port_input_set_resampler(PadResample);
    }
    const int64_t start = previous;
    int64_t lastReport = previous;
    for (;;) {
        if (port_settings_input_service()) {
            previous = lastReport = port_host_time_ns();
            g_padWallNs = 0;  // (movie47: the Settings menu held this loop: not a suspension)
            continue;
        }
        const int64_t now = port_host_time_ns();
        PortPadState pad{};
        std::unique_lock<std::mutex> padLock(g_padMutex);  // (held to port_input_set below: PadResample, on the game thread, polls the same state)
        port_vita_input_poll(std::clamp(float(now - g_padPrevNs) / 1000000000.0f, 0.0f, 0.1f), &pad);
        g_padPrevNs = now;
        previous = now;
        if (g_jkrErrorHandler && g_jkrErrorHandler != ReportHeapFailure) { // (the game installs its handler when it builds its heaps)
            g_previousJkrHandler = g_jkrErrorHandler;
            g_jkrErrorHandler = ReportHeapFailure;
        }
        bool scriptLive = false;
        if (g_scriptCount) {
            // (the game thread's finished frames are the game clock: a script then lands on the same game moment
            // whatever the emulator's speed)
            int ms = g_gameClock ? static_cast<int>(port_game_frames() * 1000ull / 60ull) : static_cast<int>((now - start) / 1000000);
            bool live = true;
            if (g_scenePlay) {
                // route24: turns of play in this scene; 0 until its first play turn (and in scenes that are not scripted)
                const unsigned long long turn = port_script_scene_turn();
                live = turn != 0;
                ms = portScriptTurnMs(turn);
            } else if (g_anchored) {
                const unsigned long long anchor = port_vgx_vita_rich_frame();
                live = anchor != 0;
                ms = live ? static_cast<int>((port_game_frames() - std::min(anchor, port_game_frames())) * 1000ull / 60ull) : 0;
            }
            if (live) ms = portScriptLoopMs(ms, g_loopMs);  // (route24: PETARI_SCRIPT_LOOP)
            if (g_scriptLog) {
                static bool wasLive = false;
                static int lastMs = 0, restarts = 0;
                if (live && (!wasLive || ms < lastMs)) {
                    port_log("script: t = 0 reached%s (start %d), %d entries", g_loopMs > 0 && wasLive ? " again (loop)" : "", ++restarts, g_scriptCount);
                } else if (!live && wasLive) {
                    port_log("script: clock stopped (the scene ended) after %d ms", lastMs);
                }
                wasLive = live;
                lastMs = ms;
            }
            if (live && g_anchored && !g_scenePlay) {
                // PETARI_WARP (a debug hook of GameScene) counts seconds from PETARI_T0_MS: with an anchored script that is the
                // script's own zero (the first title picture), so the same lines of a script and the warp land together.
                static bool t0Set = false;
                if (!t0Set) {
                    t0Set = true;
                    if (std::getenv("PETARI_WARP") || std::getenv("PETARI_SWITCH")) {
                        char t0[32];
                        std::snprintf(t0, sizeof(t0), "%lld", static_cast<long long>(now / 1000000));
                        setenv("PETARI_T0_MS", t0, 1);
                        port_log("script anchor: PETARI_T0_MS=%s (debug warps count from the first title picture)", t0);
                    }
                }
            }
            if (live) {
                pad.connected = 1;
                if (!pad.pointerValid) pad.pointerValid = 1; // the desktop harness's pointer sits at 0,0 unless an entry moves it
                portApplyInputScript(g_script, g_scriptCount, ms, &pad);
                scriptLive = true;
            }
        }
        port_input_set(0, &pad);
        padLock.unlock();
        {
            // movie47 + keepawake33 (ported from Strikers 2.0 int33): OLED-safe 1 Hz sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT)
            // preventing VitaOS's ~15.2-15.5 s unattended foreground auto-suspend during:
            //  - unattended test runs (GXV_KEEP_AWAKE=1, or unset with PETARI_BOOT / PETARI_MOVIE);
            //  - active scripted pad runs (scriptLive);
            //  - movie playback (GXV_MOVIE_AWAKE, default 1: THPVideoDecode ran within the last 2 s);
            //  - active boot loading, initial frames (< 2), or synchronous/async shader compilation (GXV_COMPILE_AWAKE, default 1).
            // Never ticks on a fatal error screen or while Settings is open (port_settings_input_service() continues above),
            // and never keeps idle players' consoles awake in menus, pause, or gameplay.
            static const int keepAwake = [] {
                const char* k = std::getenv("GXV_KEEP_AWAKE");
                const char* b = std::getenv("PETARI_BOOT");
                const char* m = std::getenv("PETARI_MOVIE");
                const bool test = (b && *b) || (m && *m);
                const int on = k && *k ? (std::atoi(k) != 0 ? 1 : 0) : (test ? 1 : 0);
                if (on) port_log_summary("keep-awake: on for the whole run (GXV_KEEP_AWAKE=%s%s): the console does not dim or go to standby without a press",
                                         k && *k ? k : "unset", k && *k ? "" : test ? ", PETARI_BOOT or PETARI_MOVIE set" : "");
                return on;
            }();
            static const bool movieAwake = [] { const char* e = std::getenv("GXV_MOVIE_AWAKE"); return !(e && *e && std::atoi(e) == 0); }();
            static const bool compileAwake = [] { const char* e = std::getenv("GXV_COMPILE_AWAKE"); return !(e && *e && std::atoi(e) == 0); }();
            const int64_t movieAt = g_movieAwakeNs.load(std::memory_order_relaxed);
            const bool inMovie = movieAwake && movieAt != 0 && now - movieAt < 2000000000ll;
            port::keepawake::State ka{};
            ka.fatalShown = port_vgx_vita_error_shown() != 0;
            ka.settingsOpen = port_settings_native_open() != 0;  // legacy overlay blocks in port_settings_input_service() above; native UI reports here
            ka.keepAwakeFlag = keepAwake != 0;
            ka.scriptLive = scriptLive;
            ka.inMovie = inMovie;
            ka.compileAwake = compileAwake;
            const int bootState = port_vgx_vita_boot_loading();
            ka.bootLoading = bootState > 0;
            ka.bootStalled = bootState < 0;
            ka.vgxFrames = port_vgx_frames();
            ka.shaderCompiling = port_vgx_shader_compiling() != 0;
            const port::keepawake::Reason why = port::keepawake::TickReason(ka);
            static int64_t lastKeepTick = 0;
            if (why != port::keepawake::Reason::None && now - lastKeepTick >= 1000000000ll) {
                lastKeepTick = now;
                sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
                static bool saidMovie = false, saidCompile = false;
                if (why == port::keepawake::Reason::Movie && !saidMovie) {
                    saidMovie = true;
                    port_log_summary("keep-awake: a movie is playing: the console is kept awake until it ends (GXV_MOVIE_AWAKE=1)");
                } else if (why == port::keepawake::Reason::BootOrCompile && !saidCompile && ka.shaderCompiling && ka.vgxFrames >= 2u) {
                    saidCompile = true;
                    port_log_summary("keep-awake: shader compilation active: the console is kept awake while shaders compile (GXV_COMPILE_AWAKE=1)");
                }
            }
            // The other side of the same thing: this loop runs every few milliseconds, so a gap of seconds between two passes with the
            // wall clock moving is the whole process standing still (the PS button, the system's standby). Said once it runs again.
            // (thr24 GXV_PAD_LEAN: the check is made every 100 ms instead of every pass: a gap of 3 s is as visible)
            static const bool padLeanOn = [] { const char* e = std::getenv("GXV_PAD_LEAN"); return e && *e && std::atoi(e) > 0; }();
            static int64_t lastWallCheck = 0;
            if (!padLeanOn || now - lastWallCheck >= 100000000ll) {
                lastWallCheck = now;
                struct timespec wall;
                clock_gettime(CLOCK_REALTIME, &wall);
                const int64_t wallNs = static_cast<int64_t>(wall.tv_sec) * 1000000000ll + wall.tv_nsec;
                const int64_t lastWallNs = g_padWallNs;
                if (lastWallNs != 0 && wallNs - lastWallNs > 3000000000ll)
                    port_log_summary("suspended: the pad thread did not run for %.1f s of wall time: the system had stopped the application (PS button, or automatic standby "
                                     "without a press: see keep-awake) or this thread got no core; game time and the log stood still",
                                     (wallNs - lastWallNs) / 1e9);
                g_padWallNs = wallNs;
            }
        }
        static int64_t lastClockCheck = start;
        static unsigned lastClockResumeGen = 0;
        const unsigned curResumeGen = port::ResumeGeneration() + port::ResumeAppGeneration();
        if (curResumeGen != lastClockResumeGen || now - lastClockCheck >= 5000000000ll) {  // (GXV_CLOCK_KEEP: clocks that dropped after sleep/LiveArea are asked for again immediately + every 5 s)
            lastClockResumeGen = curResumeGen;
            lastClockCheck = now;
            if (ClockKeepOn()) KeepClocks();
        }
        if (!PORT_RUNTIME_DIAGNOSTICS) {   // loglines44 (test only): GXV_THREADS_LOG=1 (unset by default: nothing changes): the per-thread CPU report alone, every 10 s, in the basic log
            static const bool threadsLog = [] { const char* e = getenv("GXV_THREADS_LOG"); return e != nullptr && *e != 0 && std::atoi(e) == 1; }();
            static int64_t lastThreadsLog = 0;
            if (threadsLog && now - lastThreadsLog >= 10000000000ll) {
                lastThreadsLog = now;
                port_vgx_vita_thread_report();
            }
        }
        if (PORT_RUNTIME_DIAGNOSTICS && now - lastReport >= 10000000000ll) { // every 10 s: where the game is, and the memory
            lastReport = now;
            // Twenty seconds without a finished frame: say where everything is (three times at most per stall).
            static unsigned long long stallFrames = ~0ull;
            static int stallTicks = 0;
            if (port_game_frames() == stallFrames) {
                ++stallTicks;
                if (stallTicks >= 2 && stallTicks <= 4) {
                    port_game_progress_report("no frame for 20 s");
                    port_vgx_debug_dump();
                    port_debug_dump_threads();
                }
            } else {
                stallFrames = port_game_frames();
                stallTicks = 0;
            }
            const struct mallinfo mi = mallinfo();
            port_log("status: game frames %llu, rendered %llu, C heap in use %u KiB (arena %u KiB)", port_game_frames(), port_vgx_frames(),
                     static_cast<unsigned>(mi.uordblks) / 1024u, static_cast<unsigned>(mi.arena) / 1024u);
            port_vgx_vita_log_memory("status");
            port_vgx_vita_thread_report();
            // (The game's JKR heap sizes used to be logged here; do_getFreeSize locks the heap's guest mutex from this non-guest thread,
            // and that stopped a run in Vita3K during the title load on 18 Oct.  The HEAP FULL reporter above still reads them, once,
            // when the game is about to stop anyway.)
            static unsigned reportedRootAllocs = 0;
            if (g_portRootAllocs != reportedRootAllocs) {  // plain counters kept by `new` (decomp/src/JSystem/JKernel/JKRHeap.cpp)
                reportedRootAllocs = g_portRootAllocs;
                port_log("heap: %u requests were served by the host allocator because the current heap was a root heap (first %u bytes, latest %u bytes)",
                         reportedRootAllocs, g_portRootAllocFirst, g_portRootAllocLast);
            }
        }
        // GXV_CLOCK_PERIOD=60 measures the core clock again every 60 s of play (to see whether the console throttles when it warms up;
        // it costs about 12 ms of core 0 each time, so it is off unless asked for)
        // (the profile's clocks are set by the shell when the game comes to the front, which may be a moment after the first
        // measurement at start-up: one more at 8 s, so that the log shows the clock the game really runs at)
        static bool lateMeasured = false;
        if (PORT_RUNTIME_DIAGNOSTICS && !lateMeasured && now - start >= 8000000000ll) {
            lateMeasured = true;
            port_log("clocks: the ARM core measures %u MHz (8 s into the run)", MeasureArmMHz());
        }
        static const long clockPeriod = [] { const char* e = std::getenv("GXV_CLOCK_PERIOD"); return e && *e ? std::atol(e) : 0l; }();
        static int64_t lastClock = start;
        if (PORT_RUNTIME_DIAGNOSTICS && clockPeriod > 0 && now - lastClock >= clockPeriod * 1000000000ll) {
            lastClock = now;
            port_log("clocks: the ARM core measures %u MHz (%lld s into the run)", MeasureArmMHz(), static_cast<long long>((now - start) / 1000000000ll));
        }
        port_host_sleep_ns(6000000);
    }
}
