// Real Vita-conditional startup/scheduler; only kernel boundaries use Linux adapters.
#ifndef VITA_THREAD_SOURCE
#define VITA_THREAD_SOURCE "../platform/src/os/os_thread.cpp"
#endif
#include VITA_THREAD_SOURCE
#include <cstdarg>
#include <sched.h>
#include <sys/syscall.h>
#undef vsnprintf

static thread_local int pinnedCore = -1;
static int testCpu;
static bool requirePin;
static bool handoffMode;
static bool requireAffinityLog, failPin, failInfo;
static thread_local char affinityLog[1024];
static thread_local char workerLog[1024];
static thread_local unsigned nativeIdReads;
static bool requireWorkerLog, requireNoWorkerLog;
static unsigned iterations = 10000;
static unsigned mainTurns, workerTurns;
static std::atomic<unsigned> checks{0}, active{0};
static OSThread worker;
static unsigned char guestStack[0x4000];

static void require(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); _exit(1); }
}
extern "C" void port_log(const char* format, ...) {
    char* target = !std::strncmp(format, "affinity:", 9) ? affinityLog :
                   (!std::strncmp(format, "thread-map:", 11) ? workerLog : nullptr);
    if (!target) return;
    va_list args; va_start(args, format);
    ::vsnprintf(target, sizeof(affinityLog), format, args);
    va_end(args);
}
extern "C" void port_fatal(const char* format, ...) {
    va_list args; va_start(args, format); std::vfprintf(stderr, format, args); va_end(args);
    _exit(2);
}
extern "C" void* port_low_alloc(size_t) { std::abort(); }
extern "C" void __OSUnlockAllMutex(OSThread* t) {
    require(!t->queueMutex.head, "fixture exits without owned guest mutexes");
}
extern "C" void port_game_progress_cpu_checkpoint(uint32_t) {}
extern "C" void port_game_progress_irq_checkpoint(uint32_t, uint32_t) {}
extern "C" int64_t port_host_time_ns() {
    timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
extern "C" int sceKernelGetThreadId() { ++nativeIdReads; return int(syscall(SYS_gettid)); }
extern "C" int sceKernelGetThreadInfo(SceUID uid, SceKernelThreadInfo* info) {
    if (uid != sceKernelGetThreadId()) return -1;
    if (failInfo) return -5;
    info->currentCpuAffinityMask = pinnedCore == 0 ? 0x10000 : 0;
    info->currentPriority = 0x9f;
    timespec ts{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    info->runClocks = uint64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
    return 0;
}
extern "C" int port_vita_thread_stack(void** base, uint32_t* bytes) {
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr)) return -1;
    size_t size = 0;
    const int rc = pthread_attr_getstack(&attr, base, &size);
    pthread_attr_destroy(&attr);
    *bytes = uint32_t(size);
    return rc;
}
extern "C" int port_vita_pin_core(int core) {
    require(core == 0, "game startup requests the designated game core");
    if (failPin) return -22;
    cpu_set_t mask; CPU_ZERO(&mask); CPU_SET(testCpu, &mask);
    const int rc = pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
    if (!rc) pinnedCore = core;
    return rc ? -rc : 0;
}
extern "C" int port_vita_thread_class(int cls) {
    require(cls == 3, "guest startup applies the game priority class");
    return 0;
}
static void criticalTurn(unsigned& count) {
    require(active.fetch_add(1) == 0, "guest work never overlaps on two native threads");
    require(port_owns_game_cpu() && OSGetCurrentThread() == tlsSelf,
            "guest work belongs to the selected virtual CPU owner");
    ++count;
    require(active.fetch_sub(1) == 1, "guest critical section exits exactly once");
}
static void* workerEntry(void*) {
    require(pinnedCore == (failPin ? -1 : 0), "ordinary guest thread preserves the requested pin result");
    if (requireWorkerLog) {
        unsigned uid, pin, size;
        void *guest, *entry, *base, *anchor;
        require(std::sscanf(workerLog, "thread-map: uid=0x%x guest=%p entry=%p pin_rc=0x%x stack=%p+%u anchor=%p",
                &uid, &guest, &entry, &pin, &base, &size, &anchor) == 7,
                "guest worker log exposes native identity and its actual entry point");
        require(uid == unsigned(syscall(SYS_gettid)) && guest == &worker && entry == reinterpret_cast<void*>(&workerEntry),
                "native census UID maps to the correct guest worker and symbol");
        require(pin == (failPin ? unsigned(-22) : 0u) && base == worker.stackEnd && size >= (2u << 20),
                "identity report preserves pin failures and measured stack metadata");
        require(anchor == reinterpret_cast<void*>(&hostThreadMain),
                "worker identity includes a verified relocation anchor");
        require(nativeIdReads == 1, "worker identity samples the native ID only once at startup");
        std::printf("%s\n", workerLog);
    }
    if (requireNoWorkerLog) require(!workerLog[0] && nativeIdReads == 0,
            "quiet startup does not query or format worker identity diagnostics");
    for (unsigned i = 0; i < iterations; ++i) {
        criticalTurn(workerTurns);
        OSYieldThread();
    }
    return nullptr;
}
static void gameEntry() {
    std::printf("game entry: pin=%d, require_core0=%d\n", pinnedCore, requirePin);
    std::fflush(stdout);
    if (requirePin) require(pinnedCore == 0, "default game thread must be pinned before game entry");
    if (requireAffinityLog) {
        require(std::strstr(affinityLog, "role=game-main") && std::strstr(affinityLog, "requested=0x10000"),
                "startup log identifies the real game main and requested mask");
        require(std::strstr(affinityLog, failPin ? "pin_rc=0xffffffea" : "pin_rc=0x00000000"),
                "startup log reports the actual pin success or error");
        require(std::strstr(affinityLog, failInfo ? "before_rc=0xfffffffb" : "before_rc=0x00000000"),
                "startup log exposes a failed before query");
        require(std::strstr(affinityLog, failInfo ? "after_rc=0xfffffffb" : "after_rc=0x00000000"),
                "startup log exposes a failed after query");
        if (!failInfo)
            require(std::strstr(affinityLog, failPin ? "after_mask=0x00000000" : "after_mask=0x00010000"),
                    "startup log reads back the resulting kernel mask");
        std::printf("%s\n", affinityLog);
    }
    if (failPin) require(pinnedCore == -1, "a failed affinity call leaves the preexisting policy intact");
    if (handoffMode) {
        require(OSCreateThread(&worker, workerEntry, nullptr, guestStack + sizeof(guestStack),
                               sizeof(guestStack), 16, 0), "ordinary guest thread is created");
        const int64_t start = port_host_time_ns();
        OSResumeThread(&worker);
        for (unsigned i = 0; i < iterations; ++i) {
            criticalTurn(mainTurns);
            OSYieldThread();
        }
        require(OSJoinThread(&worker, nullptr), "guest worker exits and joins through real scheduler");
        const double elapsedMs = (port_host_time_ns() - start) / 1e6;
        require(mainTurns == iterations && workerTurns == iterations, "both guests make all requested progress");
        std::printf("handoffs: main=%u worker=%u elapsed_ms=%.3f checks=%u; Linux timing, not Vita\n",
                    mainTurns, workerTurns, elapsedMs, checks.load());
    }
    std::printf("PASS: real startup/scheduler audit (%u checks)\n", checks.load());
    std::fflush(stdout);
    _exit(0);
}
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--require-pin")) requirePin = true;
        if (!std::strcmp(argv[i], "--handoffs")) handoffMode = true;
        if (!std::strcmp(argv[i], "--require-log")) requireAffinityLog = true;
        if (!std::strcmp(argv[i], "--fail-pin")) failPin = true;
        if (!std::strcmp(argv[i], "--fail-info")) failInfo = true;
        if (!std::strcmp(argv[i], "--require-worker-log")) requireWorkerLog = handoffMode = true;
        if (!std::strcmp(argv[i], "--require-no-worker-log")) requireNoWorkerLog = handoffMode = true;
    }
    cpu_set_t available; CPU_ZERO(&available);
    require(sched_getaffinity(0, sizeof(available), &available) == 0, "Linux affinity adapter can inspect allowed CPUs");
    testCpu = -1;
    for (int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &available)) { testCpu = i; break; }
    require(testCpu >= 0, "Linux adapter has an allowed core");
    port_os_boot(gameEntry);
    for (;;) pause();
}
