// WP-T dependency: exercise the production DVD failure helpers and scheduler,
// not a model. Only the host error display and guest lifetime are test seams.
// Like os_irq.cpp, inclusion permits isolated boot-state setup. This does not
// execute DVDInit/data validation, VI/audio startup, or the Vita splash renderer.
#ifndef PORT_TEST_OS_SOURCE
#define PORT_TEST_OS_SOURCE "../platform/src/os/os_thread.cpp"
#endif
#include PORT_TEST_OS_SOURCE
#include <cstdarg>
#include <thread>

extern "C" void port_boot_data_park(void) __attribute__((weak));
extern "C" int port_boot_data_report(const char*) __attribute__((weak));

static std::atomic<unsigned> checks{0}, publication{0}, callbacks{0};
static std::atomic<unsigned> probes{0}, resumes{0}, hostFrames{0};
static std::atomic<bool> acceptDisplay{false}, continued{false}, stopDisplay{false};
enum Command { Probe, Resume, ExitFixture };
struct FixtureExit {};
static std::atomic<Command> command{Probe};
static constexpr const char* kError = "synthetic missing title stream; repair data and relaunch";

static void require(bool value, const char* message) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "Boot data park FAILED: %s\n", message);
        std::fflush(stderr);
        std::_Exit(1);
    }
}

extern "C" void port_log(const char*, ...) {}
extern "C" void port_log_summary(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    require(std::strstr(text, kError) != nullptr, "summary preserves actionable error");
    require(publication.exchange(1) == 0, "one error summary precedes host callback");
}
extern "C" int port_boot_data_error(const char* message) {
    require(std::strcmp(message, kError) == 0, "host callback receives original error");
    require(publication.exchange(2) == 1, "callback follows basic-log publication");
    ++callbacks;
    return acceptDisplay.load() ? 1 : 0;
}
extern "C" int64_t port_host_time_ns() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return int64_t(now.tv_sec) * 1000000000 + now.tv_nsec;
}
extern "C" void port_fatal(const char*, ...) { require(false, "unexpected production fatal"); std::abort(); }
extern "C" void* port_low_alloc(size_t) { require(false, "unexpected guest allocation"); return nullptr; }
extern "C" void __OSUnlockAllMutex(OSThread*) { require(false, "unexpected guest exit path"); }

static bool parked() {
    return __OSCurrentThread == nullptr && DefaultThread.state == OS_THREAD_STATE_READY &&
           DefaultThread.suspend == 1 && RunQueueBits == 0;
}

static void observeIrq() {
    if (tlsSelf != &DefaultThread || !sInIrq || sIntEnabled)
        std::fprintf(stderr, "IRQ context: guest=%d in_irq=%d enabled=%d command=%d\n",
                     tlsSelf == &DefaultThread, sInIrq, sIntEnabled, int(command.load()));
    require(tlsSelf == &DefaultThread && sInIrq && !sIntEnabled,
            "actual idle scheduler delivers IRQ on guest CPU with interrupts masked");
    // A producer can interrupt immediately before the next suspension. Only
    // acknowledge an observation once the actual scheduler has parked again.
    if (!parked()) return;
    require(publication.load() == 3 && !continued.load(),
            "accepted display publication completes before every park");
    switch (command.load()) {
    case Probe:
        ++probes;
        break;
    case Resume:
        require(OSResumeThread(&DefaultThread) == 1, "unexpected resume consumes one suspension");
        require(DefaultThread.suspend == 0 && RunQueueBits != 0,
                "actual scheduler makes the guest runnable after resume");
        ++resumes;
        break;
    case ExitFixture:
        // Deliberate test-only lifetime escape, caught outside the real park
        // call. No production recovery/exit path is inferred; no mutex is held.
        sInIrq = false;
        sIntEnabled = true;
        throw FixtureExit{};
    }
}

static void* guestMain(void*) {
    tlsSelf = &DefaultThread;
    __OSCurrentThread = &DefaultThread;
    sCpuOwner = &DefaultThread;
    sCurrentContext = &DefaultThread.context;
    DefaultThread.state = OS_THREAD_STATE_RUNNING;
    DefaultThread.priority = DefaultThread.base = 16;
    DefaultThread.suspend = 0;
    hostOf(&DefaultThread)->started = true;
    sIntEnabled = true;
    require(Reschedule == 0 && !sInIrq, "first-boot scheduling context permits suspension");
    require(port_boot_data_report(kError) == 1, "actual reporter confirms visible host acceptance");
    require(publication.exchange(3) == 2, "report returns only after host callback");
    try {
        port_boot_data_park();
    } catch (const FixtureExit&) {
        return nullptr;
    }
    continued.store(true);
    require(false, "terminal boot helper must not continue initialization");
    return nullptr;
}

template <class Predicate> static void await(Predicate done, const char* message) {
    const int64_t deadline = port_host_time_ns() + 2000000000;
    while (!done() && port_host_time_ns() < deadline) {
        port_irq_raise(PORT_IRQ_ALARM);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(done(), message);
}

static int64_t cpuNs(clockid_t clock) {
    timespec now{};
    require(clock_gettime(clock, &now) == 0, "guest CPU time is measurable");
    return int64_t(now.tv_sec) * 1000000000 + now.tv_nsec;
}

int main() {
    require(port_boot_data_park && port_boot_data_report, "WP-T production park/report helpers must be linked");
    setenv("GXV_BOOTDATA", "1", 1);
    require(port_boot_data_report(kError) == 0, "refused display is not reported as visible");
    require(publication.load() == 2 && callbacks.load() == 1, "refusal still publishes actionable diagnostic");
    publication.store(0);
    acceptDisplay.store(true);
    port_irq_set_handler(PORT_IRQ_ALARM, observeIrq);

    std::thread display([] {
        while (!stopDisplay.load()) {
            ++hostFrames;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    pthread_t guest;
    require(pthread_create(&guest, nullptr, guestMain, nullptr) == 0, "guest host thread starts");
    await([] { return probes.load() > 0; }, "initial suspension enters actual scheduler idle");
    clockid_t guestClock;
    require(pthread_getcpuclockid(guest, &guestClock) == 0, "guest CPU clock obtained");
    const int64_t cpuBefore = cpuNs(guestClock);
    const unsigned framesBefore = hostFrames.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const int64_t cpuUsed = cpuNs(guestClock) - cpuBefore;
    require(cpuUsed < 50000000, "park blocks rather than burning a CPU (under 50 ms CPU in 250 ms wall)");
    require(hostFrames.load() >= framesBefore + 10, "independent host display stand-in progresses while guest parks");

    command.store(Resume);
    await([] { return resumes.load() > 0; }, "test IRQ resumes the suspended guest");
    const unsigned before = probes.load();
    command.store(Probe);
    await([before] { return probes.load() > before; }, "unexpected resume returns to a fresh suspension");
    require(callbacks.load() == 2 && !continued.load(), "repark neither republishes error nor continues boot");
    command.store(ExitFixture);
    port_irq_raise(PORT_IRQ_ALARM);
    require(pthread_join(guest, nullptr) == 0, "test-only IRQ exit is joined cleanly");
    stopDisplay.store(true);
    display.join();
    std::printf("Actual DVD park/scheduler: %u checks, %u IRQ observations, %u resumes, %u host display ticks; "
                "parked guest CPU %.3f ms / 250 ms wall. Host fixture, not Vita UI proof.\n",
                checks.load(), probes.load(), resumes.load(), hostFrames.load(), cpuUsed / 1e6);
}
