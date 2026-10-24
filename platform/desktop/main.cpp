// Native Linux diagnostic runner. Captures use the real GX/GLES pipeline;
// its timings are not evidence of physical-Vita performance.
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <ucontext.h>
#endif

#include <atomic>
#include <thread>

#include "port/input.h"
#include "port/input_script.h"
#include "port/port.h"
#include "port/qemu_hooks.h"
#include "port/vclock.h"

extern "C" void port_boot(const char*, const char*);
extern "C" unsigned long long port_headless_render(const char*, int);
extern "C" void port_debug_dump_threads(void);
extern "C" void port_game_progress_report(const char* why);
extern "C" void port_vgx_debug_dump(void);
extern "C" void port_audio_capture_finish(void);
extern "C" unsigned long long port_game_frames(void);

// PETARI_GAME_CLOCK=1: scripted input, screenshots and the run length follow the game's own
// frame count (frames / 59.94) instead of the wall clock. A renderer that draws every frame in
// software makes the game lose frames now and then; with this clock a button press and a
// screenshot still land on the same game frame as in a run that never stalls, so pictures
// of two renderers can be compared.
static bool sGameClock;
static int64_t sStartNs;
static int64_t runMs() {
    if (sGameClock) return (int64_t)((double)port_game_frames() * (1000.0 / 59.94));
    return (port_host_time_ns() - sStartNs) / 1000000;
}

static volatile sig_atomic_t stopped;
static void stop(int) { stopped = 1; }

#if defined(__linux__)
static void crashAddress(int number, siginfo_t* info, void* opaque) {
    // Avoid allocation, stdio locks and unwinding on a faulting game thread.
    char message[2048];
    char* next = message;
    auto field = [&](const char* label, uintptr_t value) {
        while (*label) *next++ = *label++;
        *next++ = '0'; *next++ = 'x';
        for (int shift = int(sizeof(value) * 8) - 4; shift >= 0; shift -= 4)
            *next++ = "0123456789abcdef"[(value >> shift) & 15];
    };
    field("desktop crash signal=", number);
    field(" address=", reinterpret_cast<uintptr_t>(info->si_addr));
    const auto& state = static_cast<ucontext_t*>(opaque)->uc_mcontext;
#if defined(__aarch64__)
    field(" pc=", state.pc); field(" lr=", state.regs[30]);
    field(" sp=", state.sp); field(" x0=", state.regs[0]);
    field(" x1=", state.regs[1]); field(" x2=", state.regs[2]); field(" x3=", state.regs[3]);
#elif defined(__x86_64__)
    field(" pc=", state.gregs[REG_RIP]); field(" sp=", state.gregs[REG_RSP]);
#elif defined(__arm__)
    field(" pc=", state.arm_pc); field(" lr=", state.arm_lr); field(" sp=", state.arm_sp);
    field(" r0=", state.arm_r0); field(" r1=", state.arm_r1); field(" r2=", state.arm_r2); field(" r3=", state.arm_r3);
#if defined(PORT_ARMHF_HARNESS)
    // (harness) candidate return addresses on the stack, to find the caller of an abort / trap
    {
        const uint32_t* sp = reinterpret_cast<const uint32_t*>(state.arm_sp);
        int found = 0;
        for (int i = 0; i < 512 && found < 24; ++i) {
            const uint32_t w = sp[i];
            if (w >= 0x80000000u && w < 0x80500000u && (w & 1)) { field(" bt=", w); ++found; }
        }
    }
#endif
#endif
    *next++ = '\n';
    const ssize_t ignored = write(STDERR_FILENO, message, size_t(next - message));
    (void)ignored;
    _exit(128 + number);
}

static void installCrashDiagnostics() {
    struct sigaction action{};
    action.sa_sigaction = crashAddress;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (int number : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
        if (sigaction(number, &action, nullptr)) port_fatal("cannot install desktop crash diagnostics");
}
#endif

static void captureHostMaps(const char* directory, const char* phase) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/maps-%s.txt", directory, phase);
    FILE* source = fopen("/proc/self/maps", "r");
    FILE* target = fopen(path, "w");
    if (!source || !target) port_fatal("cannot capture host memory mappings: %s", path);
    char line[1024];
    while (fgets(line, sizeof(line), source)) fputs(line, target);
    const bool failed = ferror(source) || ferror(target);
    fclose(source);
    const int closed = fclose(target);
    if (failed || closed) port_fatal("cannot finish host memory mappings: %s", path);
}

static PortPadState neutralPad() {
    PortPadState pad{};
    pad.connected = 1;
    pad.pointerValid = 1;
    pad.pointerDist = 1.0f;
    return pad;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <cooked-disc> <isolated-run-dir> <seconds>\n", argv[0]);
        return 2;
    }
    const int seconds = atoi(argv[3]);
    if (seconds <= 0) return 2;
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
#if defined(__linux__)
    installCrashDiagnostics();
#endif
    char saves[1024], shots[1024], path[1200];
    snprintf(saves, sizeof(saves), "%s/saves", argv[2]);
    snprintf(shots, sizeof(shots), "%s/shots", argv[2]);
    if ((mkdir(argv[2], 0755) && access(argv[2], W_OK)) ||
        (mkdir(saves, 0755) && access(saves, W_OK)) ||
        (mkdir(shots, 0755) && access(shots, W_OK))) {
        perror("run directories");
        return 2;
    }
    snprintf(path, sizeof(path), "%s/game.log", argv[2]);
    port_log_file(path);
    captureHostMaps(argv[2], "before-boot");
    static PortInputEvent events[1024];
    const char* input = getenv("PETARI_INPUT");
    const int count = input ? portParseInputScript(input, events, 1024) : 0;
    const char* inputFile = getenv("PETARI_INPUT_FILE");
    PortPadState pad = neutralPad();
    port_input_set(0, &pad);
    const int64_t start = port_host_time_ns();
    sStartNs = start;
    sGameClock = getenv("PETARI_GAME_CLOCK") && atoi(getenv("PETARI_GAME_CLOCK")) != 0;
    char startText[32];
    snprintf(startText, sizeof(startText), "%lld", (long long)(start / 1000000));
    setenv("PETARI_T0_MS", startText, 1);
    // Shader compilation can block the render thread across an entire button
    // pulse. Input sampling must keep its own clock, as on the actual device.
    std::atomic<bool> sampling{true};
    std::thread inputThread([&] {
        PORT_QEMU_THREAD("host-input");
        PortInputEvent live[1024];
        char previousScript[16384]{};
        int liveCount = count;
        bool haveLive = false;
        int64_t nextRead = 0;
        while (sampling.load(std::memory_order_acquire)) {
            PortPadState state = neutralPad();
            const int elapsed = (int)runMs();
            const int wall = (int)((port_host_time_ns() - start) / 1000000);
            if (inputFile && wall >= nextRead) {
                nextRead = wall + 100;
                FILE* script = fopen(inputFile, "r");
                if (script) {
                    char spec[16384];
                    size_t bytes = fread(spec, 1, sizeof(spec) - 1, script);
                    bool read = !ferror(script);
                    fclose(script);
                    if (read) {
                        while (bytes && (spec[bytes - 1] == '\n' || spec[bytes - 1] == '\r' ||
                                         spec[bytes - 1] == ' ' || spec[bytes - 1] == '\t')) --bytes;
                        spec[bytes] = 0;
                        liveCount = portParseInputScript(spec, live, 1024);
                        if (!haveLive || strcmp(spec, previousScript)) {
                            port_log("desktop: input reload elapsed_ms=%d events=%d script=%s", elapsed, liveCount, spec);
                            snprintf(previousScript, sizeof(previousScript), "%s", spec);
                        }
                        haveLive = true;
                    }
                }
            }
            portApplyInputScript(haveLive ? live : events, haveLive ? liveCount : count, elapsed, &state);
            port_input_set(0, &state);
            port_host_sleep_ns(5000000);  // (virtual time under PETARI_VCLOCK=1, else nanosleep)
        }
    });
    PORT_QEMU_THREAD("host-main");
    port_boot(argv[1], saves);
    captureHostMaps(argv[2], "after-boot");

    const char* shotMs = getenv("PETARI_SHOT_MS");
    const int interval = shotMs ? atoi(shotMs) : 2000;
    if (interval < 100) port_fatal("PETARI_SHOT_MS must be at least 100");
    // PETARI_STALL_S=<n>: when no frame has been finished for n seconds of real time, the log gets
    // the main loop's phase, the renderer hand-over state and every game thread's stack (twice).
    if (const char* st = getenv("PETARI_STALL_S"); st && atoi(st) > 0) {
        const int limit = atoi(st);
        std::thread([limit] {
            unsigned long long seen = ~0ull;
            int quiet = 0, reports = 0;
            for (;;) {
                sleep(1);
                const unsigned long long now = port_game_frames();
                if (now != seen) { seen = now; quiet = 0; continue; }
                if (++quiet < limit || reports >= 2) continue;
                quiet = limit / 2;
                ++reports;
                port_game_progress_report("stall");
                port_vgx_debug_dump();
                port_debug_dump_threads();
            }
        }).detach();
    }
    int64_t nextShot = interval;
    unsigned shotsTaken = 0;
    unsigned long long lastFrame = 0;
    while (!stopped) {
        const int64_t elapsed = runMs();
        if (elapsed >= (int64_t)seconds * 1000) break;
        // (game clock: a run that has stopped producing frames still ends)
        if (sGameClock && (port_host_time_ns() - start) / 1000000 >= (int64_t)seconds * 3000 + 60000) break;
        if (elapsed >= nextShot) {
            snprintf(path, sizeof(path), "%s/frame_%04u.png", shots, shotsTaken);
            lastFrame = port_headless_render(path, 1);
            ++shotsTaken;
            nextShot = elapsed + interval;
        }
        port_host_sleep_ns(5000000);
    }
    sampling.store(false, std::memory_order_release);
    port_vclock_release();  // (virtual clock: the input thread can only finish once time advances)
    inputThread.join();
    captureHostMaps(argv[2], "terminal");
    port_audio_capture_finish();
    if (getenv("PETARI_THREADS")) port_debug_dump_threads();
    port_log("desktop: terminal elapsed_ms=%lld captures=%u last_recorded_frame=%llu; diagnostic, not Vita FPS",
             (long long)((port_host_time_ns() - start) / 1000000), shotsTaken, lastFrame);
    // Upstream has no coordinated game/IRQ shutdown yet. Exit only after
    // flushing evidence; detached OS threads must not run global destructors.
    fflush(nullptr);
    _exit(shotsTaken && lastFrame ? 0 : 1);
}
