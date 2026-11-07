// The Vita game's host thread: data folder, log, environment file, input; then port_boot()
// (which starts the renderer's thread, vitaGL and the guest) and the pad-polling loop.
//
//   ux0:data/smg/game       the converted disc (sys/, files/); an installed copy of the owner's data
//   ux0:data/smg/saves      saves
//   ux0:data/smg/log.txt    the port's log (previous session: log.txt.prev); every [vgx-vita] line
//   ux0:data/smg/env.txt    NAME=value lines put into the environment before anything starts
//                           (GXV_* renderer switches, see platform/vgx/vita/vgx_host_galaxy_vita.cpp;
//                           the game's own PETARI_* debug hooks, e.g. PETARI_STAGE=HeavensDoorGalaxy:1)
//   ux0:data/smg/input.txt  optional scripted pad input in the desktop harness's syntax
//                           (scripts/inputs/README.md: "<t>[-<t1>]:A|B|PX=0.3|SY=1", one entry per line, # comments),
//                           OR-ed over the real pad. t is wall-clock seconds since the start of this process, or with
//                           PETARI_GAME_CLOCK=1 game seconds (60 finished frames each), which keeps a script in step
//                           with a game that the emulator runs far below 60 fps. With PETARI_SCRIPT_ANCHOR_DRAWS=<n>
//                           (needs PETARI_GAME_CLOCK=1) t = 0 is the first picture with n or more draw calls (the
//                           title screen: 200), and nothing is scripted before: the scene loads take minutes there.
//   ux0:data/smg/shadercache  vitaGL's compiled-shader cache
#include <bits/gthr.h>
#include <malloc.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../runtime/input.h"
#include "settings_runtime.h"
#include "title_art_runtime.h"
#include "port/game_progress.h"
#include "port/input_script.h"
#include "port/port.h"
#include "port/diagnostics.h"

#define SMG_DATA "ux0:data/smg"

extern "C" void port_boot(const char*, const char*);
extern "C" int port_vita_pin_core(int core);
extern "C" int port_vita_thread_class(int cls);
extern "C" int port_vita_core0_alone(void);   // GXV_CORE0_ALONE=1: the game thread has core 0 to itself (port/core_layout.h)
extern "C" int port_vita_thread_helper(void);
extern "C" int port_vita_core3_available(void);  // the fourth core, proven once (vita/runtime/memory.cpp; port/core_layout.h)
extern "C" void port_vgx_vita_log_memory(const char* when);
extern "C" void port_vgx_vita_thread_report(void);
extern "C" void port_splash_begin(void); // the loading screen (platform/vgx/vita/vgx_host_galaxy_vita.cpp): up from the first moments, until the title has appeared
extern "C" unsigned long long port_vgx_frames(void);
extern "C" unsigned long long port_vgx_vita_rich_frame(void);
extern "C" unsigned long long port_game_frames(void);
extern "C" void port_game_progress_report(const char* why);
extern "C" void port_vgx_debug_dump(void);
extern "C" void port_debug_dump_threads(void);

// The game's JKR heap globals, reached by symbol name (see ReportHeapFailure below).
extern void (*g_jkrErrorHandler)(void*, unsigned, int) __asm__("_ZN7JKRHeap13mErrorHandlerE");
extern void* g_jkrRootHeap __asm__("_ZN7JKRHeap9sRootHeapE");
extern void* g_jkrCurrentHeap __asm__("_ZN7JKRHeap12sCurrentHeapE");
// How many `new` requests the game's allocator sent to the host allocator because the current heap was a root heap (JKRHeap.cpp).
extern unsigned g_portRootAllocs, g_portRootAllocFirst, g_portRootAllocLast;

namespace {
void LoadEnvFile(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return;
    // (2 KiB: a PETARI_STAGE tour of every galaxy is about 1.1 KiB on one line; a longer line was cut and the cut name crashed the stage loader)
    static char line[2048];
    int n = 0;
    while (std::fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s == '#' || *s == '\n' || *s == '\r' || !*s) continue;
        char* eq = std::strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char* v = eq + 1;
        for (char* e = v + std::strlen(v); e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); *--e = 0) {}
        setenv(s, v, 1);
        ++n;
    }
    std::fclose(f);
    port_log("env.txt: %d variables set", n);
}

PortInputEvent g_script[1024];
int g_scriptCount = 0;
bool g_gameClock = false;
bool g_anchored = false; // PETARI_SCRIPT_ANCHOR_DRAWS set
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
    port_log("input script %s: %d entries, clock = %s", path, g_scriptCount, g_gameClock ? "game frames (60 per second)" : "wall clock");
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
static int ClockWatchThread(SceSize, void*) {
    // A lap on a busy core can be cut by the game's threads and read low, so the clock is the best of 8 laps over ~2 s, and a
    // change is logged only when two such readings in a row agree on it.
    unsigned last = 0, pending = 0;
    for (;;) {
        sceKernelDelayThread(last ? 30 * 1000 * 1000 : 20 * 1000 * 1000);
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
static void StartClockWatch() {
    const char* e = std::getenv("GXV_CLOCK_WATCH");
    if (e && *e == '0') return;
    const SceUID t = sceKernelCreateThread("smg_clockwatch", ClockWatchThread, 191, 0x4000, 0, 0, nullptr);
    if (t >= 0) sceKernelStartThread(t, 0, nullptr);
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
    if (pre < 470) {
        arm = scePowerSetArmClockFrequency(500);
        if (arm < 0) fallback = scePowerSetArmClockFrequency(444);
    }
    port_log_summary("clocks: 1 s into the process the ARM core measures %u MHz (%s; arm_rc=0x%08X fallback_rc=0x%08X); now %u MHz", pre,
                     pre < 470 ? "asked for 500" : "a profile is in place: no request", static_cast<unsigned>(arm),
                     static_cast<unsigned>(fallback), MeasureArmMHzQuick());
    return sceKernelExitDeleteThread(0);
}

extern "C" int port_thp_bench(const char* path);  // (platform/src/sdk/thp_video.cpp)

extern "C" int port_vita_game_entry(int argc, char** argv) {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(SMG_DATA, 0777);
    sceIoMkdir(SMG_DATA "/saves", 0777);
    port_log_file(SMG_DATA "/log.txt");
    if (!PORT_RUNTIME_DIAGNOSTICS)
        port_log_summary("Super Mario Galaxy Vita, basic log: frame rate and hitches every 5 s, picture-size changes, settings, errors; built %s %s",
                         __DATE__, __TIME__);
    port_log("kind=native_galaxy_vgx; build %s %s", __DATE__, __TIME__);
    // libstdc++ locks its mutexes and condition variables only when pthread_cancel is linked (__gthread_active_p); the link line
    // requires it (vita/vgx-game/CMakeLists.txt). One line so that a log shows it.
    port_log_summary("threads: libstdc++ locking %s", __gthread_active_p() ? "active" : "INACTIVE (std::mutex does nothing)");
    LoadEnvFile(SMG_DATA "/env.txt");
    // Version 1.1 player defaults: Mario's eyes (final TEV byte wrap), the Settings menu and button
    // remapping, the truthful loading bar, and the NEON/faster audio mixing kernels incl. the resampler and AFC decoder
    // ported from Producdevity's fork (bit-identical PCM to the
    // scalar code; they run on the game thread). Set only when env.txt has not: NAME=0 turns one off.
    // Steady 30 (the fixed 30 ceiling, dynamic quality aimed at 30, full-size effects) is a Settings choice.
    for (const char* name : {"GXV_SETTINGS", "GXV_REMAP", "GXV_BOOTGUARD", "GXV_DSPNEON", "GXV_DSPFIR", "GXV_DSPRAMP",
                             "GXV_DSPRESAMPLE", "GXV_DSPAFC"})
        setenv(name, "1", 0);
    setenv("GXV_TEVWRAP", "2", 0);  // alpha-only wrap: the eye repair without wrapping RGB
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
    // Also: dedicated small programs for exact EFB copies, and dynamic quality returning to full resolution 20 s after a
    // busy stretch (not 120 s); one pass over the lights when a lit channel's colour and alpha share their light set.
    for (const char* name : {"GXV_FIX_INDPREV", "GXV_FIX_DSTA", "GXV_FIX_LOGICOP", "GXV_FIX_TEXOFF", "GXV_NODISCARD", "GXV_THPHW",
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
    // Stream vertex and index buffers in vitaGL's per-picture pool (no heap allocation or collector work per frame).
    setenv("GXV_VITA_SCRATCH", "1", 0);
    // Pad defaults for a PS TV (GAL-032): with a DS3 or DS4 on the PlayStation profile R2 shoots Star Bits, the right stick moves the
    // pointer, the D-pad is the camera. The Vita handheld's own controls are not touched. GXV_PAD_DEFAULTS=0 in env.txt gives the 1.1 pad layout.
    setenv("GXV_PAD_DEFAULTS", "1", 0);
    // The loading screen (the title picture with the loading bar) can never sit at 99 %: if the game has been running pictures in
    // the last loading stage for 60 s without the title logo being recognised, the screen fades out and the game's own picture shows.
    setenv("GXV_BOOT_TITLE_FALLBACK", "60", 0);
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
    setenv("GXV_RUNG_LODBIAS", "60", 0);
    // r52 console A/B (lit Observatory, zoom 1.0): a picture at least every 3 turns cut the longest gap from ~82 to ~67 ms at the
    // same fps and a full 60 turns/s.
    setenv("GXV_SKIP_MAXSTEP", "3", 0);
    // r54 console A/B: FXAA-style edge smoothing at the present (HUD untouched) cost no fps (27.5 vs 27.4, Locked 30) and
    // smooths the jagged edges; on in every mode.
    setenv("GXV_EDGE_AA", "1", 0);
    // r61 console A/B (GXV_GPU_SCENES): the one-pass present with reads at vertex-computed coordinates and half maths costs the
    // display scene 4.5 ms with edge smoothing (FXAA pass: 8.1 ms) and 2.3 ms without (sharpened upscale: 5.3 ms).
    setenv("GXV_PRESENT_FAST", "1", 0);
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
    // Consecutive rigid draws that differ only in their matrix drawn as one, through the 10-matrix palette (GXV_RIGIDMERGE=1;
    // 2 = the 4-matrix palette). GXV_RIGIDMERGE_DLMAX (small cached lists joining runs; =learn copies only where it joins) stays a test switch.
    setenv("GXV_RIGIDMERGE", "1", 0);
    // Static runs of cached display lists (the map's shapes) whose bounds are outside the clip volume are not drawn: no resolution,
    // no job, no record, no GL draw; the picture is the same (vgx_draw.cpp "static runs outside the view"). 0 in env.txt: off.
    // The "perf: static cull" line counts them.
    setenv("GXV_STATICCULL", "1", 0);
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
    // forkB (6 Nov; after Producdevity's fork, ee487a3 "view pass before each picture"): a turn without a picture leaves its view and
    // draw-list pass to the flush before the next picture (GameScene::calcAnim, port_view_flush), except the player's, which keeps
    // its pass every turn (PETARI_VIEW_PLAYER=1: Mario's shadow mode, lighting fade, footprints). Only the last pass before a picture
    // is ever read. Fork's console: +0.6 to +2.5 fps (Beach Bowl, Toy Time, Honeyhive, Observatory), its hashes of every draw-buffer
    // actor's matrices equal with and without. PETARI_SKIP_VIEWCALC=0 in env.txt: the pass in every turn, as before.
    setenv("PETARI_SKIP_VIEWCALC", "1", 0);
    // 1.3: the DSP mixing thread on core 1 (the render thread's, which has room), not cores 1-2: core 2 is the front end's player,
    // the stage nearest its limit. GXV_AUDIO_THREAD_CORE=0 in env.txt: cores 1-2 as in 1.2.
    setenv("GXV_AUDIO_THREAD_CORE", "1", 0);
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
            }
            bus = scePowerSetBusClockFrequency(222);
            gpu = scePowerSetGpuClockFrequency(222);
            // (the crossbar takes 83, 111 or 166 MHz: 222 is refused; 166 is the most, GXV_VITA_XBAR=111 measures the default)
            const char* xe = std::getenv("GXV_VITA_XBAR");
            xbarRequest = xe && *xe ? std::atoi(xe) : 166;
            xbar = scePowerSetGpuXbarClockFrequency(xbarRequest);
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
    // 1.3, the fourth core (port/core_layout.h): proven once, here, after the Settings start-up has put the Core plan row's choice
    // (GXV_CORE3=0 when Off) and env.txt into the environment, and before any thread that may ask for it starts. One basic-log line,
    // "clocks/cores: fourth core available ..." or "... not available (why)". Without the owner's CapUnlocker plug-in the probe thread
    // is refused at once and nothing moves. Defaults when proven: disc read-ahead, movie decoder 3 and the audio output thread;
    // GXV_CORE3=1 adds the DSP mixer and the front-end finishing thread (or GXV_CORE3_AUDIO=1, GXV_CORE3_SPLIT=1 one by one).
    port_vita_core3_available();
    port_splash_begin();
    if (const char* e = std::getenv("GXV_STDERR_FILE"); PORT_RUNTIME_DIAGNOSTICS && e && *e == '1') { std::freopen(SMG_DATA "/stderr.txt", "w", stderr); std::setvbuf(stderr, nullptr, _IOFBF, 16384); /* (written in batches, see port_log_file: a line-buffered file costs 12-15 ms per line on the card) */ }
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
    port_boot(SMG_DATA "/game", SMG_DATA "/saves");
    port_vgx_vita_log_memory("after port_boot");
    port_title_art_start();  // GXV_TITLEART: the player's own pictures, made from the title screen once it has been shown (title_art_runtime.cpp)
    int64_t previous = port_host_time_ns();
    const int64_t start = previous;
    int64_t lastReport = previous;
    for (;;) {
        if (port_settings_input_service()) {
            previous = lastReport = port_host_time_ns();
            continue;
        }
        const int64_t now = port_host_time_ns();
        PortPadState pad{};
        port_vita_input_poll(std::clamp(float(now - previous) / 1000000000.0f, 0.0f, 0.1f), &pad);
        previous = now;
        if (g_jkrErrorHandler && g_jkrErrorHandler != ReportHeapFailure) { // (the game installs its handler when it builds its heaps)
            g_previousJkrHandler = g_jkrErrorHandler;
            g_jkrErrorHandler = ReportHeapFailure;
        }
        if (g_scriptCount) {
            // (the game thread's finished frames are the game clock: a script then lands on the same game moment
            // whatever the emulator's speed)
            int ms = g_gameClock ? static_cast<int>(port_game_frames() * 1000ull / 60ull) : static_cast<int>((now - start) / 1000000);
            bool live = true;
            if (g_anchored) {
                const unsigned long long anchor = port_vgx_vita_rich_frame();
                live = anchor != 0;
                ms = live ? static_cast<int>((port_game_frames() - std::min(anchor, port_game_frames())) * 1000ull / 60ull) : 0;
            }
            if (live && g_anchored) {
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
                // forkB (from Producdevity's fork, fdeeb00): scripted presses are not the system's input; with no tick the system
                // stops the app about 15 s after launch once the console's automatic standby time has passed on battery without a
                // real press. Only with an input script: players' consoles still go to standby as usual.
                static int64_t lastPowerTick = 0;
                if (now - lastPowerTick >= 1000000000ll) {
                    lastPowerTick = now;
                    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
                }
            }
        }
        port_input_set(0, &pad);
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
