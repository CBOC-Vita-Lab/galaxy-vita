#include "memory.h"

#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/core_layout.h"
#include "port/cores_report.h"

#include <psp2/kernel/cpu.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr/thread.h>

namespace {

constexpr uint32_t page_size = 4096;
constexpr uint32_t image_alignment = 32;
constexpr int invalid_argument = static_cast<int>(SCE_KERNEL_ERROR_INVALID_ARGUMENT);
constexpr int illegal_address = static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
constexpr int illegal_size = static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_SIZE);

bool aligned(const void* base, uint32_t alignment) {
    return base && (reinterpret_cast<uintptr_t>(base) & (alignment - 1)) == 0;
}

bool native_range(const void* base, uint32_t size) {
    const uint64_t address = reinterpret_cast<uintptr_t>(base);
    return base && size && address <= UINT32_MAX &&
           address + uint64_t(size) <= UINT32_MAX;
}

bool valid_range(const void* base, uint32_t size, uint32_t alignment) {
    return aligned(base, alignment) && native_range(base, size);
}

extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));  // the basic log (os_alarm.cpp)
extern "C" void port_log(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));

// The thread-layout switches. Read at each use, not cached: this runs at thread start-up only, and the log writer thread starts
// (platform/src/os/os_alarm.cpp) before vita/vgx-game/entry.cpp has loaded env.txt, so a value cached at its first call would be stale.
port::cores::Switches CoreSwitches() {
    return port::cores::ReadSwitches([](const char* name) { return getenv(name); });
}

void BasicLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void BasicLog(const char* fmt, ...) {
    char text[200];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    if (port_log_summary) port_log_summary("%s", text);
    else if (port_log) port_log("%s", text);
}

// The calling thread's affinity mask, for port::cores::PlaceIoThread.
struct SelfKernel {
    int Set(int mask) { return sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask); }
    int Get() { return sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId()); }
};

// The fourth core's probe thread (port::cores::ProveSystemCore). It reports into statics, not the creator's stack: a thread that
// outlives the 200 ms wait must not write to a frame that is gone. It reads its core and mask, then spins kProofSpinUs by the
// process clock and compares its own CPU time with that: a core the system lends only in slices shows up as a small share.
volatile int g_proofCpu = -1, g_proofMask = 0, g_proofDone = 0;
volatile unsigned long long g_proofWallUs = 0, g_proofCpuUs = 0;
unsigned long long ThreadCpuUs() {
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    return sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0 ? 0 : static_cast<unsigned long long>(info.runClocks);
}
int ProofMain(SceSize, void*) {
    const int first = sceKernelGetCpuId();
    g_proofMask = sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId());
    const unsigned long long cpu0 = ThreadCpuUs(), wall0 = sceKernelGetProcessTimeWide();
    unsigned long long wall = wall0;
    while (wall - wall0 < port::cores::kProofSpinUs) wall = sceKernelGetProcessTimeWide();  // (the clock read is the work)
    g_proofCpuUs = ThreadCpuUs() - cpu0;
    g_proofWallUs = wall - wall0;
    const int last = sceKernelGetCpuId();
    g_proofCpu = first == last ? first : -1;
    g_proofDone = 1;
    return 0;
}
struct ProofApi {
    int Create(int mask) {
        g_proofDone = 0; g_proofCpu = -1; g_proofMask = 0; g_proofWallUs = 0; g_proofCpuUs = 0;  // (a retry starts clean)
        return sceKernelCreateThread("smg_core3_proof", &ProofMain, 0x10000100, 16 * 1024, 0, mask, nullptr);
    }
    int Change(int id, int mask) { return sceKernelChangeThreadCpuAffinityMask(id, mask); }
    int Start(int id) { return sceKernelStartThread(id, 0, nullptr); }
    int Wait(int id, unsigned timeoutUs) {
        SceUInt timeout = timeoutUs;
        const int rc = sceKernelWaitThreadEnd(id, nullptr, &timeout);
        return rc < 0 ? rc : 0;
    }
    void Delete(int id) { sceKernelDeleteThread(id); }
    void Sleep(unsigned us) { sceKernelDelayThread(us); }
    bool Reported() { return g_proofDone != 0; }
    int Cpu() { return g_proofCpu; }
    int Mask() { return g_proofMask; }
    unsigned long long SpinWallUs() { return g_proofWallUs; }
    unsigned long long SpinCpuUs() { return g_proofCpuUs; }
};

// What the basic log calls each part, for the "clocks/cores" and "threads: core 3" lines.
const char* Core3PartName(int part) {
    return part == 1 ? "audio output thread" : part == 2 ? "DSP mixer thread" : part == 3 ? "front-end finishing thread" : part == 4 ? "shader compile worker" : "thread";
}

} // namespace

extern "C" int port_vita_memory_alloc(const char* name, uint32_t size, PortVitaMemoryBlock* out) {
    if (!aligned(out, alignof(PortVitaMemoryBlock))) return invalid_argument;
    *out = {nullptr, 0, -1};
    if (!name) return invalid_argument;
    if (!size || size % page_size) return illegal_size;

    const SceUID uid = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, nullptr);
    if (uid < 0) return uid;
    out->uid = uid;

    void* base = nullptr;
    int result = sceKernelGetMemBlockBase(uid, &base);
    if (result >= 0 && !valid_range(base, size, page_size)) result = illegal_address;
    if (result < 0) {
        // Keep ownership recoverable if the SDK also rejects rollback.
        const int cleanup = port_vita_memory_free(out);
        return cleanup < 0 ? cleanup : result;
    }
    out->base = base;
    out->size = size;
    return 0;
}

extern "C" int port_vita_memory_free(PortVitaMemoryBlock* block) {
    if (!aligned(block, alignof(PortVitaMemoryBlock))) return invalid_argument;
    if (block->uid < 0) return static_cast<int>(SCE_KERNEL_ERROR_INVALID_UID);
    const int result = sceKernelFreeMemBlock(block->uid);
    if (result < 0) return result;
    *block = {nullptr, 0, -1};
    return 0;
}

extern "C" int port_vita_memory_image(PortVitaImageSegment* segments, uint32_t capacity, uint32_t* count) {
    if (!aligned(count, alignof(uint32_t))) return invalid_argument;
    *count = 0;
    if (!aligned(segments, alignof(PortVitaImageSegment)) || !capacity) return invalid_argument;

    const uintptr_t function_address = reinterpret_cast<uintptr_t>(&port_vita_memory_image) & ~uintptr_t(1);
    const SceUID uid = sceKernelGetModuleIdByAddr(reinterpret_cast<void*>(function_address));
    if (uid < 0) return uid;
    SceKernelModuleInfo info{};
    info.size = sizeof(info);
    const int result = sceKernelGetModuleInfo(uid, &info);
    if (result < 0) return result;

    constexpr uint32_t maximum_segments = sizeof(info.segments) / sizeof(info.segments[0]);
    PortVitaImageSegment found[maximum_segments]{};
    uint32_t found_count = 0;
    for (const SceKernelSegmentInfo& segment : info.segments) {
        if (!segment.memsz) continue;
        if (!valid_range(segment.vaddr, segment.memsz, image_alignment)) return illegal_address;
        found[found_count++] = {segment.vaddr, segment.memsz};
    }
    if (!found_count) return illegal_size;
    if (found_count > capacity) return static_cast<int>(SCE_KERNEL_ERROR_INVALID_ARGUMENT_SIZE);
    for (uint32_t i = 0; i < found_count; ++i) segments[i] = found[i];
    *count = found_count;
    return 0;
}

extern "C" int port_vita_memory_free_sizes(uint32_t* user, uint32_t* cdram, uint32_t* phycont) {
    if (!aligned(user, alignof(uint32_t)) || !aligned(cdram, alignof(uint32_t)) ||
        !aligned(phycont, alignof(uint32_t))) return invalid_argument;
    *user = *cdram = *phycont = 0;
    SceKernelFreeMemorySizeInfo info{};
    info.size = sizeof(info);
    const int result = sceKernelGetFreeMemorySize(&info);
    if (result < 0) return result;
    if (info.size_user < 0 || info.size_cdram < 0 || info.size_phycont < 0) return illegal_size;
    *user = static_cast<uint32_t>(info.size_user);
    *cdram = static_cast<uint32_t>(info.size_cdram);
    *phycont = static_cast<uint32_t>(info.size_phycont);
    return 0;
}

extern "C" int port_vita_memory_valid_range(const void* base, uint32_t size) {
    if (!native_range(base, size)) return 0;
    SceKernelMemBlockInfo info{};
    info.size = sizeof(info);
    if (sceKernelGetMemBlockInfoByRange(const_cast<void*>(base), size, &info) < 0) return 0;
    if (!valid_range(info.mappedBase, info.mappedSize, alignof(uint32_t))) return 0;
    const uint64_t begin = reinterpret_cast<uintptr_t>(base);
    const uint64_t mapped_begin = reinterpret_cast<uintptr_t>(info.mappedBase);
    return begin >= mapped_begin && begin + uint64_t(size) <= mapped_begin + uint64_t(info.mappedSize);
}

extern "C" int port_vita_pin_core(int core) {
    if (core < 0 || core > 2) return invalid_argument;
    return sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x10000 << core); // SCE_KERNEL_CPU_MASK_USER_0..2
}

// Priorities of the port's own threads. The C++ thread library (and pthread_create) gives every thread the lowest user
// priority, 0xBF, so the game, the renderer, the movie decoders, the disc reader and the clocks all took turns in equal slices;
// a movie decode that fills every core then delays the disc read and the audio clock. Lower numbers run first:
//   1 clocks (vertical-blank and OS timers: tiny, they must run when they are due)    0x50
//   2 disc reader (its copies of file data are short, the loads wait for them)        0x70
//   3 game, renderer front end and back end                                           0x90
//   4 background work (movie decoders): only the time the others leave                0xBF
// GXV_PRIO=<mask> chooses which of the classes 1-3 are applied (bit 0 = class 1, bit 1 = class 2, bit 2 = class 3; "keep" = 0 =
// every thread stays where the library put it). The default is class 1 only. Measured with the attack movie (three decoder
// threads, 40 ms of wall time per frame at the lowest priority): the disc reader at 0x70 makes the decoders 11 % slower
// (45 ms) and the full set 18 % (48 ms) although the file is then read four times faster; the clocks alone cost nothing
// measurable (42 ms). Game and renderer threads are made at 0x9F already, above the decoders.
extern "C" int port_vita_thread_class(int cls) {
    static const unsigned mask = [] {
        const char* e = getenv("GXV_PRIO");
        if (!e || !*e) return 1u;
        return !strcmp(e, "keep") ? 0u : static_cast<unsigned>(atoi(e));
    }();
    static const int kPriority[5] = {0xBF, 0x50, 0x70, 0x90, 0xBF};
    if (cls < 1 || cls > 4) return invalid_argument;
    // The clocks (1), the disc and host-job threads (2) and background work (4) keep off core 0, the game's: the system scheduler
    // otherwise places these unpinned threads anywhere, and at 0x50 / 0x70 they preempt the game thread there. In the Observatory
    // (18 Oct console log, thp48d) the game thread's turn ran 20-23 % longer by the clock than it computed; these threads and the
    // audio ones are the ones that can take that time. Cores 1 and 2 (back end and front end) have room. GXV_HELPER_CORES=0: as before.
    // (GXV_CORE0_ALONE=1 also moves them when GXV_HELPER_CORES=0 says otherwise: the game thread has core 0 to itself, see port/core_layout.h)
    // GXV_HELPER_CORE=1|2 (sched24, default 0 = cores 1 and 2): the helpers' mask is that core alone; GXV_CLOCK_CORE=1|2 the same for class 1 only
    // (port/core_layout.h). The front end's player is on core 2: what the system places there at 0x50 preempts it.
    if (const port::cores::Switches sw = CoreSwitches(); cls != 3) {
        const int helperMask = cls == 1 ? port::cores::ClockMask(sw) : port::cores::HelperMask(sw);
        if (helperMask) sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), helperMask);  // SCE_KERNEL_CPU_MASK_USER_1 | USER_2
    }
    if (cls <= 3 && !(mask & (1u << (cls - 1)))) return 0;
    return sceKernelChangeThreadPriority(0, kPriority[cls]);
}

// GXV_CORE0_ALONE=1 (default 0): 1 when the game thread is to have core 0 to itself (see port/core_layout.h).
extern "C" int port_vita_core0_alone(void) { return CoreSwitches().core0Alone ? 1 : 0; }

// A helper thread that has no class (log writer, freeze watch, an unraised frame clock): off core 0 under GXV_CORE0_ALONE=1, else untouched.
// Returns the mask applied, 0 when nothing was changed.
extern "C" int port_vita_thread_helper(void) {
    const int mask = port::cores::UnclassedMask(CoreSwitches());
    if (!mask) return 0;
    return sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask) >= 0 ? mask : 0;
}

// GXV_CORE3_IO=1 (default 0): delay-tolerant work asks for the system core. kind 0 the disc read-ahead worker, 1 a movie decoder (only
// index 2 asks). The thread tries the change on itself and keeps it only when the call succeeded and the mask reads back as asked;
// otherwise it is put back (cores 1-2) and nothing but one line in the basic log says so. Never called by the render, front-end or
// present threads, and nothing that runs here spins: the disc worker and the decoders sleep on condition variables.
// Returns the mask the thread has afterwards (0x80000 on the system core), 0 when the switch is off or the thread was left alone.
// 1.3: only once the fourth core is proven (port_vita_core3_available); without it the thread is not touched (it was already on its
// planned cores 1-2; before 1.3 it asked, was refused with 0x80028025 and went back).
extern "C" int port_vita_thread_io(int kind, int index) {
    static std::atomic<unsigned> reported{0};
    const port::cores::IoKind which = kind == 1 ? port::cores::IoKind::MovieDecoder : port::cores::IoKind::DiscReader;
    SelfKernel kernel;
    const port::cores::Switches switches = CoreSwitches();
    if (!port::cores::MayUseSystemCore(switches, which, index) || !port_vita_core3_available()) return 0;
    const port::cores::Placement p = port::cores::PlaceIoThread(kernel, switches, which, index);
    if (!p.tried) return 0;
    const unsigned bit = 1u << (kind == 1 ? 1 : 0);
    if (!(reported.fetch_or(bit) & bit)) {
        const char* what = kind == 1 ? "movie decoder 3" : "disc read-ahead worker";
        if (p.onSystem) BasicLog("threads: core 3 io: the %s runs on the system core (mask 0x%X)", what, static_cast<unsigned>(p.mask));
        else BasicLog("threads: core 3 io: the %s stays on cores 1-2 (change rc 0x%X, mask read back 0x%X)", what, static_cast<unsigned>(p.askRc),
                      static_cast<unsigned>(p.readBack));
    }
    return p.mask;
}

// The fourth core (port/core_layout.h, "The fourth core"): 1 when a probe thread proved that this process may run threads on the system
// core (mask 0x80000; the owner's CapUnlocker plug-in), else 0. Decided once per process, at the first call: vita/vgx-game/entry.cpp
// calls it right after the Settings start-up (so the Core plan row and env.txt are in the environment), and writes no line itself.
// Always one line in the basic log: "clocks/cores: fourth core available ..." or "... not available (why)". No probe when no part wants
// the core (GXV_CORE3=0, or every part switch 0).
extern "C" int port_vita_core3_available(void) {
    static const int proven = [] {
        const port::cores::Switches s = CoreSwitches();
        if (!port::cores::Core3Wanted(s)) {
            BasicLog("clocks/cores: fourth core not available (not probed: GXV_CORE3=0 or every GXV_CORE3_* part off)");
            return 0;
        }
        ProofApi api;
        int tries = 0;
        const port::cores::Proof r = port::cores::ProveSystemCoreRetry(api, &tries);
        if (r.proven) {
            BasicLog("clocks/cores: fourth core available (CPU %d, mask 0x%X, %u%% of a %u ms spin, probe %d of %d); used by:%s%s%s%s%s", r.cpu,
                     static_cast<unsigned>(r.mask), r.sharePct, port::cores::kProofSpinUs / 1000, tries, port::cores::kProofTries, s.core3Io ? (s.core3Disc ? " disc read-ahead and movie decoder 3;" : " movie decoder 3 (disc read-ahead stays on cores 1-2: GXV_CORE3_DISC=0);") : "",
                     s.core3Audio ? (s.core3Audio == 1 ? " audio output and DSP mixer;" : " audio output;") : "",
                     s.core3Split ? " front-end finishing thread;" : "", s.core3Shaders ? " shader compile worker;" : "", (s.core3Io || s.core3Audio || s.core3Split || s.core3Shaders) ? "" : " nothing");
        } else {
            BasicLog("clocks/cores: fourth core not available (%s; create 0x%X, move 0x%X, start 0x%X, wait 0x%X, cpu %d, mask 0x%X, share %u%%, %d probe(s)): nothing moves",
                     r.why, static_cast<unsigned>(r.createRc), static_cast<unsigned>(r.changeRc), static_cast<unsigned>(r.startRc),
                     static_cast<unsigned>(r.waitRc), r.cpu, static_cast<unsigned>(r.mask), r.sharePct, tries);
        }
        return r.proven ? 1 : 0;
    }();
    return proven;
}

// A thread of a fourth-core part (1 audio output, 2 DSP mixer, 3 front-end finishing thread; port::cores::Core3Part) asks for the system
// core on itself, when its switch is on and the core is proven. `planned`: the mask it has to go back to when the system refuses the core
// or does not honour the change (0: where it was). Returns the mask it has afterwards (0x80000 on the fourth core), or 0 when it did not
// ask (switch off, core not proven): then the caller's own placement stands. One basic-log line per part.
extern "C" int port_vita_thread_core3(int part, int planned) {
    static std::atomic<unsigned> reported{0};
    if (part < 1 || part > 4) return 0;
    const port::cores::Switches switches = CoreSwitches();
    // (the switch first: no probe for a part that is off)
    if (!port::cores::Core3Uses(switches, static_cast<port::cores::Core3Part>(part), true) || !port_vita_core3_available()) return 0;
    SelfKernel kernel;
    const port::cores::Placement p = port::cores::PlaceOnSystemCore(kernel, planned);
    const unsigned bit = 1u << part;
    if (!(reported.fetch_or(bit) & bit)) {
        if (p.onSystem) BasicLog("threads: core 3: the %s runs on the fourth core (mask 0x%X)", Core3PartName(part), static_cast<unsigned>(p.mask));
        else BasicLog("threads: core 3: the %s stays on its planned cores, mask 0x%X (change rc 0x%X, mask read back 0x%X)", Core3PartName(part),
                      static_cast<unsigned>(p.mask), static_cast<unsigned>(p.askRc), static_cast<unsigned>(p.readBack));
    }
    return p.mask;
}

// CPU time the calling thread has used, in microseconds.
extern "C" int64_t port_thread_cpu_us(void) {
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0) return 0;
    return static_cast<int64_t>(info.runClocks);
}

// GXV_THREAD_CPU=1 (hivelogic, 6 Nov; default 0): who uses the CPU where the game thread runs. Called every 600 turns by the game
// thread (platform/src/port/perf.cpp MoveProfTurn). One basic-log line: the CPU each thread used since the last call, summed by core
// mask, and the threads allowed on core 0 (mask 0 or with 0x10000) other than the caller, costliest first. runClocks is brought up to
// date only when a thread leaves its core, so a short window is uneven per call, but over 10 s the sums hold.
// (The per-phase "cpu-us" of the phase-cpu lines reads the same counter around each phase: CPU used after the thread last left its
// core lands in whichever later phase ends after its next switch. That is why draw shows more CPU than wall time and update-scene
// far less, and why "whole" wall minus CPU is not time taken by other threads.)
extern "C" void port_vita_thread_cpu_census(void) {
    struct Seen { SceUID uid; unsigned long long run; };
    static Seen before[128];
    static int beforeCount = 0;
    static unsigned long long lastUs = 0;
    Seen after[128];
    int afterCount = 0;
    struct Row { char name[28]; unsigned mask; unsigned long long us; };
    Row rows[8];
    int rowCount = 0;
    unsigned long long core0 = 0, core1 = 0, core2 = 0, core12 = 0, core3 = 0, any = 0, self = 0;
    const SceUID me = sceKernelGetThreadId();
    const unsigned long long nowUs = sceKernelGetProcessTimeWide();
    SceKernelThreadInfo info;
    int missesInRow = 0;
    for (SceUID uid = 0x40010001; uid < 0x40020000 && missesInRow < 500 && afterCount < 128; ++uid) {
        if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++missesInRow; continue; }
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(uid, &info) != 0) { ++missesInRow; continue; }
        missesInRow = 0;
        const unsigned long long run = static_cast<unsigned long long>(info.runClocks);
        after[afterCount++] = {uid, run};
        unsigned long long was = 0;
        bool known = false;
        for (int i = 0; i < beforeCount; ++i) {
            if (before[i].uid == uid) { was = before[i].run; known = true; break; }
        }
        if (lastUs == 0 || !known || run < was) continue;  // (the first call, or a new thread: only its starting value)
        const unsigned long long us = run - was;
        const unsigned mask = static_cast<unsigned>(info.currentCpuAffinityMask);
        if (uid == me) self += us;
        if (mask == 0x10000u) core0 += us;
        else if (mask == 0x20000u) core1 += us;
        else if (mask == 0x40000u) core2 += us;
        else if (mask == 0x60000u) core12 += us;
        else if (mask == 0x80000u) core3 += us;
        else any += us;
        if (uid != me && (mask == 0 || (mask & 0x10000u)) && us >= 1000) {
            int at = rowCount < 8 ? rowCount++ : -1;
            if (at < 0) {  // (keep the 8 costliest)
                at = 0;
                for (int i = 1; i < 8; ++i) if (rows[i].us < rows[at].us) at = i;
                if (rows[at].us >= us) continue;
            }
            snprintf(rows[at].name, sizeof(rows[at].name), "%s", info.name);
            rows[at].mask = mask;
            rows[at].us = us;
        }
    }
    if (lastUs != 0 && nowUs > lastUs) {
        char text[200];
        int o = snprintf(text, sizeof(text), "threads: %.1f s, CPU ms by core mask: core 0 only %llu (of it this game thread %llu), 1 %llu, 2 %llu, 1-2 %llu, 3 %llu, other %llu",
                         (nowUs - lastUs) / 1e6, core0 / 1000, self / 1000, core1 / 1000, core2 / 1000, core12 / 1000, core3 / 1000, any / 1000);
        (void)o;
        BasicLog("%s", text);
        for (int k = 0; k < rowCount; ++k) {  // (costliest first)
            int best = k;
            for (int i = k + 1; i < rowCount; ++i) if (rows[i].us > rows[best].us) best = i;
            const Row r = rows[best];
            rows[best] = rows[k];
            rows[k] = r;
            BasicLog("threads:   may run on core 0: \"%s\" mask 0x%X: %.1f ms", r.name, r.mask, r.us / 1000.0);
        }
    }
    memcpy(before, after, sizeof(Seen) * static_cast<size_t>(afterCount));
    beforeCount = afterCount;
    lastUs = nowUs;
}

// ---- sched24: who uses which core ("cores-5s:" lines, port/cores_report.h) and, with GXV_CORE_PROBE=1, how late a sleeping thread wakes on each core.
// Threads that the port makes with pthread_create or std::thread have the kernel name "pthread": each says who it is once, at its start
// (port_thread_register, from PORT_QEMU_THREAD / PORT_THREAD_NAME in port/qemu_hooks.h). Threads made with sceKernelCreateThread (the par workers, the
// audio output thread, the probe threads) and the system's own (display queue, collector) are listed by their kernel names.
namespace {
struct ThreadName { std::atomic<SceUID> uid{0}; char name[20] = {}; };
ThreadName g_names[48];
std::atomic<int> g_nameCount{0};

const char* NameOf(SceUID uid, const char* kernelName) {
    const int n = g_nameCount.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < 48; ++i)
        if (g_names[i].uid.load(std::memory_order_acquire) == uid) return g_names[i].name;
    return kernelName;
}

// The probes (GXV_CORE_PROBE=1). A probe sleeps kProbePeriodUs and counts how much later than that it ran again; the report reads and clears the counts.
constexpr unsigned kProbePeriodUs = 2000;
struct Probe {
    const char* label;
    int mask;
    int priority;
    std::atomic<unsigned> wakes{0}, over500{0}, over1500{0}, over3000{0}, maxUs{0};
};
Probe g_probes[3] = {{"core 3 @par workers' priority", 0x80000, 0}, {"core 3 @0x60", 0x80000, 0x60}, {"core 1 @par workers' priority", 0x20000, 0}};
Probe* g_made[3] = {};  // (the probes that started, in the order the report lists them)
int g_probeCount = 0;
bool g_probesMade = false;

// One basic-log line of any length the report makes (BasicLog above cuts at 200 characters; the log writer's own limit is 1100 with its time stamp).
void LogLine(const char* line) {
    if (port_log_summary) port_log_summary("%s", line);
    else if (port_log) port_log("%s", line);
}

int ProbeMain(SceSize, void* argp) {
    Probe* p = *static_cast<Probe**>(argp);
    for (;;) {
        const unsigned long long t0 = sceKernelGetProcessTimeWide();
        sceKernelDelayThread(kProbePeriodUs);
        const unsigned long long dt = sceKernelGetProcessTimeWide() - t0;
        const unsigned late = dt > kProbePeriodUs ? static_cast<unsigned>(dt - kProbePeriodUs) : 0u;
        p->wakes.fetch_add(1, std::memory_order_relaxed);
        if (late > 500) p->over500.fetch_add(1, std::memory_order_relaxed);
        if (late > 1500) p->over1500.fetch_add(1, std::memory_order_relaxed);
        if (late > 3000) p->over3000.fetch_add(1, std::memory_order_relaxed);
        unsigned m = p->maxUs.load(std::memory_order_relaxed);
        while (late > m && !p->maxUs.compare_exchange_weak(m, late, std::memory_order_relaxed)) {}
    }
    return 0;
}

// (from the report thread, once: the render thread, which has the par workers' own default priority one step above it)
void MakeProbes() {
    g_probesMade = true;
    const int parPriority = sceKernelGetThreadCurrentPriority() - 1;  // (what a par worker gets: one step above the thread that starts it)
    const bool core3 = port_vita_core3_available();
    for (int i = 0; i < 3; ++i) {
        Probe& p = g_probes[i];
        if (p.mask == 0x80000 && !core3) continue;
        const int priority = p.priority ? p.priority : parPriority;
        const SceUID uid = sceKernelCreateThread("smg_probe", &ProbeMain, priority, 16 * 1024, 0, p.mask, nullptr);
        Probe* self = &p;
        if (uid < 0 || sceKernelStartThread(uid, sizeof(self), &self) < 0) continue;
        g_made[g_probeCount++] = &p;
    }
    BasicLog("cores-5s: GXV_CORE_PROBE: %d probe thread(s) made (a 2 ms sleep each; core 3 %s)", g_probeCount, core3 ? "proven" : "not available");
}
}  // namespace

extern "C" void port_thread_register(const char* name) {
    if (!name) return;
    const SceUID me = sceKernelGetThreadId();
    const int known = g_nameCount.load(std::memory_order_acquire);
    for (int i = 0; i < known && i < 48; ++i) {
        if (g_names[i].uid.load(std::memory_order_acquire) == me) {  // (a thread that names itself again, a guest thread that is the audio thread: the later name stands)
            snprintf(g_names[i].name, sizeof(g_names[i].name), "%s", name);
            return;
        }
    }
    const int at = g_nameCount.fetch_add(1, std::memory_order_acq_rel);
    if (at >= 48) return;
    snprintf(g_names[at].name, sizeof(g_names[at].name), "%s", name);
    g_names[at].uid.store(sceKernelGetThreadId(), std::memory_order_release);
}

// sampler30 (platform/src/port/sampler_vita.cpp): the native thread ids that registered `name` (port_thread_register), at most `max`; the count.
extern "C" int port_thread_uids_by_name(const char* name, int* uids, int max) {
    if (!name || !uids || max <= 0) return 0;
    const int known = g_nameCount.load(std::memory_order_acquire);
    int n = 0;
    for (int i = 0; i < known && i < 48 && n < max; ++i) {
        const SceUID uid = g_names[i].uid.load(std::memory_order_acquire);
        if (uid > 0 && !strcmp(g_names[i].name, name)) uids[n++] = static_cast<int>(uid);
    }
    return n;
}

// Once per 5 s from the render thread's basic-log pass (platform/src/gx/vgx_galaxy.cpp PerfFrame, just after the "fps:" line): `seconds` and the
// pictures and game turns of that window. GXV_CORES_LOG: see port/cores_report.h.
extern "C" void port_vita_cores_report(double seconds, unsigned pictures, unsigned turns) {
    static const int mode = [] {
        const char* e = getenv("GXV_CORES_LOG");
        if (e && *e) return atoi(e) != 0 ? 1 : 0;
        const char* a = getenv("GXV_TURN_PROF");
        const char* b = getenv("GXV_WORKLOAD_PROBE");
        return (a && *a && atoi(a) != 0) || (b && *b && atoi(b) != 0) ? 1 : 0;
    }();
    if (!mode) return;
    static const bool probe = [] { const char* e = getenv("GXV_CORE_PROBE"); return e && *e && atoi(e) == 1; }();
    if (probe && !g_probesMade) MakeProbes();
    struct Seen { SceUID uid; unsigned long long run; };
    static Seen before[160];
    static int beforeCount = 0;
    static bool first = true;
    Seen after[160];
    int afterCount = 0;
    static port::cores::ThreadRow rows[port::cores::kReportMaxRows];
    int rowCount = 0;
    SceKernelThreadInfo info;
    int missesInRow = 0;
    // (the scan is ~700 cheap kernel calls: every 5 s on the render thread, which has room)
    for (SceUID uid = 0x40010001; uid < 0x40020000 && missesInRow < 500 && afterCount < 160; ++uid) {
        if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++missesInRow; continue; }
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(uid, &info) != 0) { ++missesInRow; continue; }
        missesInRow = 0;
        const unsigned long long run = static_cast<unsigned long long>(info.runClocks);
        after[afterCount++] = {uid, run};
        unsigned long long was = run;  // (a thread new in this window: nothing counted until the next one)
        for (int i = 0; i < beforeCount; ++i) {
            if (before[i].uid == uid) { was = before[i].run; break; }
        }
        if (first || run < was || rowCount >= port::cores::kReportMaxRows) continue;
        const unsigned long long us = run - was;
        if (us < 1000) continue;  // (under 1 ms in 5 s: not worth a name)
        port::cores::ThreadRow& r = rows[rowCount++];
        const char* name = NameOf(uid, info.name);
        if (!strcmp(name, "pthread")) snprintf(r.name, sizeof(r.name), "pthread-%04X", static_cast<unsigned>(uid) & 0xFFFFu);
        else snprintf(r.name, sizeof(r.name), "%s", name);
        r.mask = static_cast<unsigned>(info.currentCpuAffinityMask);
        r.priority = static_cast<unsigned>(info.currentPriority);
        r.cpuUs = us;
    }
    memcpy(before, after, sizeof(Seen) * static_cast<size_t>(afterCount));
    beforeCount = afterCount;
    if (first) { first = false; return; }
    port::cores::FormatCoresReport(rows, rowCount, seconds, pictures, turns, [](const char* line) { LogLine(line); });
    if (g_probeCount > 0) {
        port::cores::ProbeWindow w[3];
        for (int i = 0; i < g_probeCount && i < 3; ++i) {
            Probe& p = *g_made[i];
            w[i].label = p.label;
            w[i].wakes = p.wakes.exchange(0, std::memory_order_relaxed);
            w[i].over500 = p.over500.exchange(0, std::memory_order_relaxed);
            w[i].over1500 = p.over1500.exchange(0, std::memory_order_relaxed);
            w[i].over3000 = p.over3000.exchange(0, std::memory_order_relaxed);
            w[i].maxUs = p.maxUs.exchange(0, std::memory_order_relaxed);
        }
        port::cores::FormatProbeReport(w, g_probeCount < 3 ? g_probeCount : 3, [](const char* line) { LogLine(line); });
    }
}

extern "C" int port_vita_thread_stack(void** base, uint32_t* size) {
    const bool valid_base = aligned(base, alignof(void*));
    const bool valid_size = aligned(size, alignof(uint32_t));
    if (valid_base) *base = nullptr;
    if (valid_size) *size = 0;
    if (!valid_base || !valid_size) return invalid_argument;

    const SceUID uid = sceKernelGetThreadId();
    if (uid < 0) return uid;
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    const int result = sceKernelGetThreadInfo(uid, &info);
    if (result < 0) return result;
    if (info.stackSize <= 0) return illegal_size;
    const uint32_t stack_size = static_cast<uint32_t>(info.stackSize);
    if (!valid_range(info.stack, stack_size, alignof(uint32_t))) return illegal_address;
    *base = info.stack;
    *size = stack_size;
    return 0;
}

// ---- hang47: the hang watch (GXV_HANG_WATCH=<seconds>, default 0 = off; a test key).
//
// Night 35's movie rows (22 Nov): five of six runs ended with a log.txt that simply stops 0.4-1.4 s after play began, no crash dump, the
// application still quittable. log.txt cannot say which of three things happened, because all three end it the same way: the game stands
// still (a deadlock), the log's writer thread is stuck in a write to the card (the game goes on until the 96 KiB buffer is full, then
// every thread that logs waits for the writer), or the writer never gets a core. So this watch does not depend on the log:
//   - its own thread at the highest user priority, free to run on every core the process may use, no lock of the port taken;
//   - it writes with sceIo calls of its own to ux0:data/smg/hang.txt (appended; a run with no stall writes nothing);
//   - it reads only atomics and the kernel's thread table.
// A stall = no new picture queued by the game for the given seconds (after the first picture), or the log writer inside one write for
// that long, or the log writer's loop not turning for that long. The report: the counters, the guest scheduler as it stands
// (platform/src/os/os_thread.cpp port_os_hang_report), and every thread of the process with its kernel state, what it waits on, its
// priority, cores and the CPU it used in one second of the stall (a deadlock: all waiting, 0 ms; a spin: one thread near 1000 ms).
// The same lines go to log.txt when its writer is alive. At most three reports a run.
// GXV_HANG_DUMP=1 (default 0): after the first report the watch thread stops the process with an illegal instruction, so the system
// writes a crash dump (psp2core) that holds every thread's registers and stack: the exact place of each thread.
extern "C" unsigned long long port_game_frames(void) __attribute__((weak));
extern "C" int port_os_hang_report(char* out, int cap) __attribute__((weak));
extern "C" void port_log_health(unsigned* passes, unsigned* writes, unsigned* writeMs, unsigned* pendingBytes) __attribute__((weak));
extern "C" int port_is_paused(void) __attribute__((weak));
extern "C" int port_settings_active(void) __attribute__((weak));
extern "C" void port_hang_watch_note(const char* what) ;

#include <psp2/io/fcntl.h>
#include <psp2/rtc.h>

namespace {
int g_hangSeconds = 0;
bool g_hangDump = false;
std::atomic<const char*> g_hangNote{"-"};   // (the last place a thread said it was at: port_hang_watch_note, a string literal)
std::atomic<unsigned long long> g_hangNoteUs{0};

struct HangText {
    char text[24 * 1024];
    int len = 0;
    void Put(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (len >= static_cast<int>(sizeof text) - 2) return;
        va_list args;
        va_start(args, fmt);
        const int n = vsnprintf(text + len, sizeof text - static_cast<size_t>(len), fmt, args);
        va_end(args);
        if (n > 0) len = len + n < static_cast<int>(sizeof text) - 1 ? len + n : static_cast<int>(sizeof text) - 1;
    }
};
HangText g_hangText;   // (static: the watch thread's stack stays small)

const char* HangStatus(int status) {
    switch (status) {
    case 1: return "RUNNING";
    case 2: return "ready";
    case 4: return "standby";
    case 8: return "waiting";
    case 16: return "dormant";
    default: return "other";
    }
}

void HangReport(const char* why, unsigned long long frames, unsigned stalledMs) {
    HangText& t = g_hangText;
    t.len = 0;
    SceDateTime now{};
    sceRtcGetCurrentClockLocalTime(&now);
    unsigned passes = 0, writes = 0, writeMs = 0, pending = 0;
    if (port_log_health) port_log_health(&passes, &writes, &writeMs, &pending);
    t.Put("hang-watch: %02d:%02d:%02d %s: no progress for %u ms (GXV_HANG_WATCH=%d); pictures queued by the game %llu; log writer: %u passes, %u writes, "
          "%s, %u bytes pending; paused %d, settings menu %d; last note \"%s\" %llu ms ago\n",
          now.hour, now.minute, now.second, why, stalledMs, g_hangSeconds, frames, passes, writes,
          writeMs ? "INSIDE A WRITE" : "not writing", pending, port_is_paused ? port_is_paused() : -1, port_settings_active ? port_settings_active() : -1,
          g_hangNote.load(std::memory_order_relaxed),
          g_hangNoteUs.load(std::memory_order_relaxed) ? (sceKernelGetProcessTimeWide() - g_hangNoteUs.load(std::memory_order_relaxed)) / 1000 : 0ull);
    if (writeMs) t.Put("hang-watch: the log writer has been inside one write to the card for %u ms\n", writeMs);
    if (port_os_hang_report) {
        const int n = port_os_hang_report(t.text + t.len, static_cast<int>(sizeof t.text) - t.len - 1);
        if (n > 0) t.len += n;
        t.text[t.len] = 0;
    }
    // Every thread, twice one second apart: its state now and the CPU it used in that second.
    struct Seen { SceUID uid; unsigned long long run; };
    static Seen first[160];
    int firstCount = 0;
    SceKernelThreadInfo info;
    for (int pass = 0; pass < 2; ++pass) {
        int misses = 0;
        for (SceUID uid = 0x40010001; uid < 0x40020000 && misses < 500; ++uid) {
            if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++misses; continue; }
            memset(&info, 0, sizeof(info));
            info.size = sizeof(info);
            if (sceKernelGetThreadInfo(uid, &info) != 0) { ++misses; continue; }
            misses = 0;
            if (pass == 0) {
                if (firstCount < 160) first[firstCount++] = {uid, static_cast<unsigned long long>(info.runClocks)};
                continue;
            }
            long long us = -1;
            for (int i = 0; i < firstCount; ++i)
                if (first[i].uid == uid) { us = static_cast<long long>(static_cast<unsigned long long>(info.runClocks) - first[i].run); break; }
            t.Put("hang-watch thread 0x%08X \"%s\": %s (0x%X), waits on type 0x%X id 0x%08X, priority 0x%X, cores 0x%X (now %d, last %d), cpu in 1 s %.1f ms, entry %p\n",
                  static_cast<unsigned>(uid), NameOf(uid, info.name), HangStatus(info.status), static_cast<unsigned>(info.status), static_cast<unsigned>(info.waitType),
                  static_cast<unsigned>(info.waitId), static_cast<unsigned>(info.currentPriority), static_cast<unsigned>(info.currentCpuAffinityMask),
                  static_cast<int>(info.currentCpuId), static_cast<int>(info.lastExecutedCpuId), us < 0 ? -1.0 : us / 1000.0, reinterpret_cast<void*>(info.entry));
        }
        if (pass == 0) sceKernelDelayThread(1000000);
    }
    const unsigned long long after = port_game_frames ? port_game_frames() : 0;
    t.Put("hang-watch: one second later: pictures queued %llu (%s)%s\n", after, after != frames ? "MOVING AGAIN: a long stall, not a hang" : "still",
          g_hangDump ? "; GXV_HANG_DUMP=1: the process is stopped now for a crash dump" : "");
    // 1. the watch's own file, with its own calls (nothing of the log path)
    const SceUID fd = sceIoOpen("ux0:data/smg/hang.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, t.text, static_cast<SceSize>(t.len));
        sceIoClose(fd);
    }
    // 2. log.txt, only while its writer is not inside a write (a producer that finds the buffer full would wait for that writer for ever)
    unsigned writeMs2 = 0;
    if (port_log_health) port_log_health(nullptr, nullptr, &writeMs2, nullptr);
    if (port_log_summary && writeMs2 < 500) {
        char* line = t.text;
        while (line && *line) {
            char* end = strchr(line, '\n');
            if (end) *end = 0;
            port_log_summary("%s", line);
            line = end ? end + 1 : nullptr;
        }
    }
}

int HangWatchMain(SceSize, void*) {
    unsigned long long lastFrames = 0, lastMoveUs = sceKernelGetProcessTimeWide();
    unsigned lastPasses = 0;
    unsigned long long lastPassUs = lastMoveUs;
    int reports = 0;
    bool reported = false, armed = false;
    const unsigned long long limitUs = static_cast<unsigned long long>(g_hangSeconds) * 1000000ull;
    for (;;) {
        sceKernelDelayThread(250000);
        const unsigned long long nowUs = sceKernelGetProcessTimeWide();
        const unsigned long long frames = port_game_frames ? port_game_frames() : 0;
        if (frames != lastFrames) {
            if (reported && port_log_summary) port_log_summary("hang-watch: the game queues pictures again (%llu) after a stall of %llu ms", frames, (nowUs - lastMoveUs) / 1000);
            lastFrames = frames;
            lastMoveUs = nowUs;
            reported = false;
            armed = true;
        }
        unsigned passes = 0, writeMs = 0;
        if (port_log_health) port_log_health(&passes, nullptr, &writeMs, nullptr);
        if (passes != lastPasses) { lastPasses = passes; lastPassUs = nowUs; }
        const char* why = nullptr;
        unsigned stalledMs = 0;
        if (armed && !reported && nowUs - lastMoveUs >= limitUs) { why = "the game queued no picture"; stalledMs = static_cast<unsigned>((nowUs - lastMoveUs) / 1000); }
        else if (!reported && writeMs >= static_cast<unsigned>(g_hangSeconds) * 1000u) { why = "the log writer is inside one write"; stalledMs = writeMs; }
        else if (!reported && port_log_health && passes && nowUs - lastPassUs >= limitUs) { why = "the log writer's loop does not turn"; stalledMs = static_cast<unsigned>((nowUs - lastPassUs) / 1000); }
        if (!why || reports >= 3) continue;
        reported = true;
        ++reports;
        HangReport(why, frames, stalledMs);
        if (g_hangDump) {
            sceKernelDelayThread(300000);   // (the log writer's next pass, if it is alive)
            __builtin_trap();
        }
        lastPassUs = sceKernelGetProcessTimeWide();
    }
    return 0;
}
} // namespace

// A thread says where it is (a string literal; two relaxed stores): the hang report prints the last one with its age. Cheap enough for
// a few places a turn; not for inner loops.
extern "C" void port_hang_watch_note(const char* what) {
    if (!g_hangSeconds) return;
    g_hangNote.store(what, std::memory_order_relaxed);
    g_hangNoteUs.store(sceKernelGetProcessTimeWide(), std::memory_order_relaxed);
}

// Called once, after env.txt is read (vita/vgx-game/entry.cpp).
extern "C" void port_vita_hang_watch_start(void) {
    const char* e = getenv("GXV_HANG_WATCH");
    const long s = e && *e ? strtol(e, nullptr, 10) : 0;
    if (s <= 0) return;
    g_hangSeconds = static_cast<int>(s < 2 ? 2 : s > 600 ? 600 : s);
    const char* d = getenv("GXV_HANG_DUMP");
    g_hangDump = d && *d == '1';
    const SceUID th = sceKernelCreateThread("hang-watch", HangWatchMain, 0x40, 0x4000, 0, 0, nullptr);
    const int rc = th >= 0 ? sceKernelStartThread(th, 0, nullptr) : th;
    BasicLog("hang-watch: GXV_HANG_WATCH=%d: %s (rc 0x%X); a stall of that many seconds is reported in ux0:data/smg/hang.txt and here; GXV_HANG_DUMP=%d",
             g_hangSeconds, rc >= 0 ? "watching" : "THE THREAD COULD NOT BE MADE", static_cast<unsigned>(rc), g_hangDump ? 1 : 0);
}
