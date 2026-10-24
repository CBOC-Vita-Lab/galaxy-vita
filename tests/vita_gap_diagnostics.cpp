// Real safe points and IRQ state; only Vita kernel/clock boundaries are adapted.
#ifndef VITA_THREAD_SOURCE
#define VITA_THREAD_SOURCE "../platform/src/os/os_thread.cpp"
#endif
#include VITA_THREAD_SOURCE
#include <cstdarg>

static int64_t nowNs = 1000000000;
static unsigned clockReads, cpuQueries, nativeIds, envReads, logs, gapLogs, profileLogs, alarms, checks;
static void require(bool value, const char* why) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
extern "C" int64_t port_host_time_ns() { ++clockReads; nowNs += 1000000; return nowNs; }
extern "C" char* __real_getenv(const char*);
extern "C" char* __wrap_getenv(const char* name) {
    envReads += !std::strcmp(name, "PETARI_GAPLOG") || !std::strcmp(name, "PETARI_GAPPROF");
    return __real_getenv(name);
}
extern "C" void port_log(const char* format, ...) {
    ++logs;
    gapLogs += !std::strncmp(format, "gap:", 4);
    profileLogs += !std::strncmp(format, "prof:", 5);
}
extern "C" void port_fatal(const char*, ...) { std::abort(); }
extern "C" void* port_low_alloc(size_t) { std::abort(); }
extern "C" void __OSUnlockAllMutex(OSThread*) { std::abort(); }
extern "C" void port_game_progress_cpu_checkpoint(uint32_t) {}
extern "C" void port_game_progress_irq_checkpoint(uint32_t, uint32_t) {}
extern "C" int sceKernelGetThreadId() { ++nativeIds; return 17; }
extern "C" int sceKernelGetThreadInfo(SceUID uid, SceKernelThreadInfo* info) {
    require(uid == 17, "gap CPU query uses the actual calling native thread");
    ++cpuQueries;
    info->runClocks = nowNs / 1000;
    return 0;
}
extern "C" int port_vita_pin_core(int) { std::abort(); }
extern "C" int port_vita_thread_class(int) { std::abort(); }
extern "C" int port_vita_thread_stack(void**, uint32_t*) { std::abort(); }
static void alarm() {
    ++alarms;
    require(sInIrq && !sIntEnabled, "original IRQ handler still owns the masked virtual CPU");
    port_irq_poll(); // A nested safe point cannot reenter the handler.
}
int main(int argc, char** argv) {
    require(argc == 2, "one fresh-process configuration is required");
    const bool gap = !std::strcmp(argv[1], "gap") || !std::strcmp(argv[1], "both");
    const bool profile = !std::strcmp(argv[1], "profile") || !std::strcmp(argv[1], "both");
    require(gap || profile || !std::strcmp(argv[1], "off"), "fixture has a known configuration");
    unsetenv("PETARI_GAPLOG"); unsetenv("PETARI_GAPPROF");
    if (gap) setenv("PETARI_GAPLOG", "20", 1);
    if (profile) setenv("PETARI_GAPPROF", "1", 1);
    tlsSelf = &DefaultThread; __OSCurrentThread = &DefaultThread; sCpuOwner = &DefaultThread;
    DefaultThread.state = OS_THREAD_STATE_RUNNING; DefaultThread.priority = 0;
    sIntEnabled = true;
    port_irq_set_handler(PORT_IRQ_ALARM, alarm);
    for (unsigned i = 0; i < 1000; ++i) {
        if (i % 100 == 0) nowNs += 100000000;
        port_irq_poll();
        port_irq_poll_at(reinterpret_cast<void*>(uintptr_t(0x1234)));
        port_irq_raise(PORT_IRQ_ALARM);
        const BOOL enabled = OSDisableInterrupts();
        require(enabled && alarms == 2 * i + 1, "disabling interrupts preserves pending-IRQ delivery before masking");
        port_irq_raise(PORT_IRQ_ALARM);
        port_irq_poll();
        require(alarms == 2 * i + 1, "masked poll does not deliver a pending completion");
        OSRestoreInterrupts(enabled);
        require(alarms == 2 * i + 2, "restoring interrupts delivers the retained completion exactly once");
        OSEnableInterrupts();
        require(sIntEnabled && !sInIrq && !sPendingIrq.load(), "safe points return with original CPU/IRQ state");
    }
    const int64_t age = port_gap_age_ns(), threshold = port_gap_threshold_ns();
    if (!PORT_RUNTIME_DIAGNOSTICS || !PORT_LOGGING) {
        require(clockReads == 0 && cpuQueries == 0 && nativeIds == 0 && envReads == 0 && logs == 0,
                "disabled diagnostics cannot reactivate gap clocks, CPU queries, hashing or logs from saved environment");
        require(age == 0 && threshold == 0, "disabled gap helpers cannot trigger DVD timing work");
    } else if (gap || profile) {
        require(clockReads > 1000 && envReads == 2 && threshold > 0 && age >= 0, "explicit diagnostic builds retain requested gap timing");
        if (profile) require(profileLogs > 0 && cpuQueries == 0,
                "flat profiler still emits complete windows without per-safe-point thread-CPU queries");
        else require(gapLogs > 0 && cpuQueries > 0 && nativeIds == cpuQueries,
                "requested slow-gap logging retains native CPU attribution");
    } else {
        require(clockReads == 1 && envReads == 2 && cpuQueries == 0 && logs == 0 && age == 0 && threshold == 0,
                "diagnostic build without requested profiling samples only its original initialization clock");
    }
    std::printf("PASS: real safe points diagnostics=%d logging=%d mode=%s clock_reads=%u cpu_queries=%u env_reads=%u logs=%u alarms=%u (%u checks; not Vita timing)\n",
                PORT_RUNTIME_DIAGNOSTICS, PORT_LOGGING, argv[1], clockReads, cpuQueries, envReads, logs, alarms, checks);
}
