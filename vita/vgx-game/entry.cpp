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
#include "port/game_progress.h"
#include "port/input_script.h"
#include "port/port.h"
#include "port/diagnostics.h"

#define SMG_DATA "ux0:data/smg"

extern "C" void port_boot(const char*, const char*);
extern "C" int port_vita_pin_core(int core);
extern "C" int port_vita_thread_class(int cls);
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
    char line[512];
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

extern "C" int port_thp_bench(const char* path);  // (platform/src/sdk/thp_video.cpp)

extern "C" int port_vita_game_entry() {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(SMG_DATA, 0777);
    sceIoMkdir(SMG_DATA "/saves", 0777);
    port_log_file(SMG_DATA "/log.txt");
    if (!PORT_RUNTIME_DIAGNOSTICS)
        port_log_summary("Super Mario Galaxy Vita, basic log: frame rate and hitches every 5 s, picture-size changes, settings, errors; built %s %s",
                         __DATE__, __TIME__);
    port_log("kind=native_galaxy_vgx; build %s %s", __DATE__, __TIME__);
    LoadEnvFile(SMG_DATA "/env.txt");
    LoadInputScript(SMG_DATA "/input.txt");
    {
        // The console starts an application at 333 MHz with the GPU at half speed: ask for the
        // most an application may have (GXV_VITA_CLOCKS=0 leaves the defaults, to measure them).
        const char* e = std::getenv("GXV_VITA_CLOCKS");
        int arm = 0, bus = 0, gpu = 0, xbar = 0;
        if (!(e && *e && std::atoi(e) == 0)) {
            arm = scePowerSetArmClockFrequency(444);
            bus = scePowerSetBusClockFrequency(222);
            gpu = scePowerSetGpuClockFrequency(222);
            // (the crossbar takes 83, 111 or 166 MHz: 222 is refused; 166 is the most, GXV_VITA_XBAR=111 measures the default)
            const char* xe = std::getenv("GXV_VITA_XBAR");
            xbar = scePowerSetGpuXbarClockFrequency(xe && *xe ? std::atoi(xe) : 166);
        }
        port_log("clocks: CPU %d MHz, bus %d MHz, GPU %d MHz, GPU xbar %d MHz (set calls returned 0x%X 0x%X 0x%X 0x%X)", scePowerGetArmClockFrequency(),
                 scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency(), static_cast<unsigned>(arm),
                 static_cast<unsigned>(bus), static_cast<unsigned>(gpu), static_cast<unsigned>(xbar));
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
    port_splash_begin();
    if (const char* e = std::getenv("GXV_STDERR_FILE"); PORT_RUNTIME_DIAGNOSTICS && e && *e == '1') { std::freopen(SMG_DATA "/stderr.txt", "w", stderr); std::setvbuf(stderr, nullptr, _IOFBF, 16384); /* (written in batches, see port_log_file: a line-buffered file costs 12-15 ms per line on the card) */ }
    port_vgx_vita_log_memory("at process start");
    if (!port_vita_input_init()) {
        port_log("native game input initialization failed; game boot prevented");
        return 1;
    }
    // port_boot: memory arenas, disc, GX + the renderer's thread (vitaGL start-up happens inside
    // port_gx_select_backend, so this call takes a few seconds), then the guest's own thread.
    port_vita_pin_core(0);  // this thread becomes the game's main thread
    // This thread also polls the pad every few milliseconds.  At the main thread's own priority (0xA0, below every game thread at
    // 0x9F) it got no time at all while the game thread computed without stopping (a stage load: several seconds), so a
    // button pressed then was seen late or not at all.  It runs a hundred microseconds at a time: the clocks' priority.
    port_vita_thread_class(1);
    port_boot(SMG_DATA "/game", SMG_DATA "/saves");
    port_vgx_vita_log_memory("after port_boot");
    int64_t previous = port_host_time_ns();
    const int64_t start = previous;
    int64_t lastReport = previous;
    for (;;) {
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
