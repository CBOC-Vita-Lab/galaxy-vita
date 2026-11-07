// GXV_LC_YIELD host proof (logiccut, 6 Nov): the real scheduler (platform/src/os/os_thread.cpp) with real guest threads, the switch
// unset and set. The main thread (priority 16) runs a seeded script of yields (plain, inside MR::ProhibitSchedulerAndInterrupts' pattern,
// and nested so that the scheduler is still disabled), wake-ups of worker threads of priority 2, 10, 16 (round robin with the main
// thread) and 20 (runs only while the main thread sleeps), and sleeps. Every thread appends to one trace what it does, and after every
// yield the main thread appends the scheduler state the yield left (RunQueueBits, RunQueueHint, its queue and links, the current
// thread and context). The traces with and without the switch must be identical; the run prints a hash of the trace.
// Build and run: bash tools/test_lc_yield.sh
#include "../platform/src/os/os_thread.cpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" void port_log(const char*, ...) {}
extern "C" int64_t port_host_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" void port_fatal(const char* f, ...) { std::fprintf(stderr, "fatal: %s\n", f); std::abort(); }
extern "C" void* port_low_alloc(size_t n) { return std::malloc(n); }
extern "C" void __OSUnlockAllMutex(OSThread*) {}

enum { kWorkers = 4, kMain = kWorkers };
static const OSPriority kPrio[kWorkers] = {2, 10, 16, 20};
static OSThread gThread[kWorkers];
static OSThreadQueue gQ[kWorkers + 1];
static int gPending[kWorkers + 1];
static std::vector<uint32_t> gTrace;  // (guest threads only: one at a time)
static uint32_t gRng = 12345;
static uint32_t Rand() { gRng = gRng * 1664525u + 1013904223u; return gRng >> 8; }
static unsigned gYields = 0;
static bool gMainWaiting = false;

static void Trace(uint32_t a, uint32_t b) { gTrace.push_back(a); gTrace.push_back(b); }
static void Wake(int k) {
    gPending[k]++;
    OSWakeupThread(&gQ[k]);
}
static void WaitWork(int k) {
    const BOOL en = OSDisableInterrupts();
    while (gPending[k] == 0) OSSleepThread(&gQ[k]);
    gPending[k]--;
    OSRestoreInterrupts(en);
}
static void Snapshot(uint32_t tag) {
    OSThread* self = __OSCurrentThread;
    Trace(tag, (uint32_t)(self - &DefaultThread == 0 ? 100 : (self - gThread)));
    Trace(RunQueueBits, (uint32_t)RunQueueHint);
    Trace(self->queue != nullptr, (self->link.prev != nullptr) | ((self->link.next != nullptr) << 1));
    Trace(sCurrentContext == &self->context, (uint32_t)self->state);
}
static void ProhibitYield() {  // MR::ProhibitSchedulerAndInterrupts: constructor, then destructor
    const BOOL en = OSDisableInterrupts();
    OSDisableScheduler();
    OSEnableScheduler();
    OSYieldThread();
    gYields++;
    OSRestoreInterrupts(en);
}

static void* WorkerMain(void* p) {
    const int k = (int)(intptr_t)p;
    for (;;) {
        WaitWork(k);
        Trace(200 + k, gRng);
        switch (Rand() % 5) {
        case 0: ProhibitYield(); Snapshot(300 + k); break;
        case 1: OSYieldThread(); Snapshot(310 + k); break;
        case 2: if (k != 3) Wake(3); break;          // (the low-priority one runs later)
        case 3: if (!gMainWaiting) Wake(kMain); break;  // (a wake-up the main thread finds later)
        default: break;
        }
        if (k == 3 && gMainWaiting) {
            gMainWaiting = false;
            Wake(kMain);
        }
    }
    return nullptr;
}

static void entry() {
    static char stacks[kWorkers][0x4000];
    for (int k = 0; k < kWorkers; k++) {
        OSInitThreadQueue(&gQ[k]);
        OSCreateThread(&gThread[k], WorkerMain, (void*)(intptr_t)k, stacks[k] + sizeof stacks[k], sizeof stacks[k], kPrio[k], 1);
        OSResumeThread(&gThread[k]);
    }
    OSInitThreadQueue(&gQ[kMain]);
    for (int step = 0; step < 20000; step++) {
        Trace(100, step);
        switch (Rand() % 10) {
        case 0: case 1: ProhibitYield(); Snapshot(1); break;
        case 2: OSYieldThread(); gYields++; Snapshot(2); break;
        case 3: {  // nested: the yield runs with the scheduler still disabled
            const BOOL en = OSDisableInterrupts();
            OSDisableScheduler();
            ProhibitYield();
            OSEnableScheduler();
            OSRestoreInterrupts(en);
            Snapshot(3);
            break;
        }
        case 4: Wake((int)(Rand() % kWorkers)); break;
        case 5: {  // a worker readied while the scheduler is off, then the yield on the way out
            const BOOL en = OSDisableInterrupts();
            OSDisableScheduler();
            Wake((int)(Rand() % kWorkers));
            OSEnableScheduler();
            OSYieldThread();
            gYields++;
            OSRestoreInterrupts(en);
            Snapshot(5);
            break;
        }
        case 6: {  // sleep until the low-priority worker has run and woken the main thread
            gPending[kMain] = 0;
            gMainWaiting = true;
            Wake(3);
            WaitWork(kMain);
            break;
        }
        case 7: {  // states a yield must normalise as SelectThread does: a hint left set (a lower-priority thread was readied or the
                   // caller's priority changed) and stale links (left by an earlier wait queue; unused while the thread runs)
            if (Rand() & 1) RunQueueHint = TRUE;
            if (Rand() & 1) {
                DefaultThread.link.prev = &gThread[Rand() % kWorkers];
                DefaultThread.link.next = &gThread[Rand() % kWorkers];
            }
            ProhibitYield();
            Snapshot(7);
            break;
        }
        default: Snapshot(9); break;
        }
    }
    uint64_t h = 1469598103934665603ull;
    for (uint32_t v : gTrace) h = (h ^ v) * 1099511628211ull;
    std::printf("trace: %zu words, hash %016llx; %u yields, %u kept without SelectThread (GXV_LC_YIELD=%s)\n", gTrace.size(),
                (unsigned long long)h, gYields + 0, sLcYieldKept, getenv("GXV_LC_YIELD") ? getenv("GXV_LC_YIELD") : "unset");
    std::fflush(stdout);
    std::_Exit(0);
}

int main() {
    port_os_boot(entry);
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
