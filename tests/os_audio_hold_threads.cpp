// GXV_AUDIO_HOLD with real guest threads on the real scheduler (platform/src/os/os_thread.cpp): a "main" thread that computes for
// 10 ms (safe points all along) and then sleeps 5 ms waiting for a front-end interrupt, a priority-2 "audio" thread woken by AI and
// DSP interrupt handlers (as JASAudioThread is through its message queue), an AI clock raising every 17.5 ms and a DSP that answers
// every sub-frame release 0.2 ms later from a host thread. A DSP frame is 7 sub-frames, started by the AI interrupt.
// Checked for GXV_AUDIO_HOLD unset, 0, 8 and 12: every turn and every frame completes (no deadlock), no AI interrupt waits much longer
// than the limit (plus the host's scheduling noise), and with the hold the audio thread's runs move from the main thread's work into its
// sleeps.
// Prints the share of the audio runs that fell inside the main thread's work. Not Vita timing.
#include "../platform/src/os/os_thread.cpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

extern "C" void port_log(const char*, ...) {}
extern "C" int64_t port_host_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" void port_fatal(const char* f, ...) { std::fprintf(stderr, "fatal: %s\n", f); std::abort(); }
extern "C" void* port_low_alloc(size_t n) { return std::malloc(n); }
extern "C" void __OSUnlockAllMutex(OSThread*) {}

static void spinUntil(int64_t t, bool polls) {
    while (port_host_time_ns() < t) {
        if (polls) port_irq_poll();
    }
}

static OSThreadQueue gAudioQ, gFeQ;
static OSThread gAudio;
static std::atomic<int> gAiRaised{0}, gDspRaised{0};
static int gAiTaken = 0, gDspTaken = 0;   // (guest CPU only)
static std::atomic<int64_t> gAiRaiseTime{0};
static int64_t gAiWorst = 0;
static std::atomic<bool> gFeDone{false}, gStop{false}, gReleasePending{false};
static std::atomic<bool> gMainWorking{false};
static unsigned gRunsInWork = 0, gRunsElsewhere = 0, gFrames = 0, gSub = 0;
static bool gInFrame = false;
static unsigned gOwed = 0;  // DAC blocks whose DSP frame is not started yet (a frame in progress makes the next one wait, as JASDriver does)

static int64_t gAiHandlerWorst = 0;  // raise to handler: the scheduler's own part of the wait (the rest is the host's thread switch)
static void aiIrq() {
    const int64_t wait = port_host_time_ns() - gAiRaiseTime.load();
    if (wait > gAiHandlerWorst) gAiHandlerWorst = wait;
    ++gAiTaken;
    OSWakeupThread(&gAudioQ);
}
static void dspIrq() { ++gDspTaken; OSWakeupThread(&gAudioQ); }
static void feIrq() { gFeDone.store(true); OSWakeupThread(&gFeQ); }

static void release() {  // the last group's release: the DSP mixes the sub-frame on another core and mails back
    gReleasePending.store(true);
    port_audio_irq_urgent(gSub < 7 ? 1 : 0);
}

static void* audioMain(void*) {
    int ai = 0, dsp = 0;
    for (;;) {
        const BOOL en = OSDisableInterrupts();
        while (ai == gAiTaken && dsp == gDspTaken) OSSleepThread(&gAudioQ);
        const bool newAi = ai != gAiTaken, newDsp = dsp != gDspTaken;
        ai = gAiTaken;
        dsp = gDspTaken;
        OSRestoreInterrupts(en);
        (gMainWorking.load() ? gRunsInWork : gRunsElsewhere)++;
        spinUntil(port_host_time_ns() + 150000, true);  // the JAudio work of a wake-up
        if (newAi) {
            const int64_t wait = port_host_time_ns() - gAiRaiseTime.load();
            if (wait > gAiWorst) gAiWorst = wait;
            ++gOwed;
        }
        if (newDsp && gInFrame) {
            if (gSub < 7) { ++gSub; release(); }
            else { gInFrame = false; ++gFrames; }
        }
        if (!gInFrame && gOwed) { --gOwed; gInFrame = true; gSub = 1; release(); }
    }
    return nullptr;
}

static int gTurns = 0;
static void entry() {
    OSInitThreadQueue(&gAudioQ);
    OSInitThreadQueue(&gFeQ);
    port_irq_set_handler(PORT_IRQ_AI, aiIrq);
    port_irq_set_handler(PORT_IRQ_DSP, dspIrq);
    port_irq_set_handler(PORT_IRQ_PE_FINISH, feIrq);
    port_audio_irq_urgent(0);
    static char stack[0x4000];
    OSCreateThread(&gAudio, audioMain, nullptr, stack + sizeof stack, sizeof stack, 2, 1);
    OSResumeThread(&gAudio);
    for (gTurns = 0; gTurns < 120; ++gTurns) {
        gMainWorking.store(true);
        spinUntil(port_host_time_ns() + 10000000, true);
        gMainWorking.store(false);
        gFeDone.store(false);
        std::thread([] { std::this_thread::sleep_for(std::chrono::milliseconds(5)); port_irq_raise(PORT_IRQ_PE_FINISH); }).detach();
        const BOOL en = OSDisableInterrupts();
        while (!gFeDone.load()) OSSleepThread(&gFeQ);
        OSRestoreInterrupts(en);
    }
    gStop.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::printf("turns %d, frames %u, AI raised %d taken %d (worst wait %.1f ms, %.1f ms to the handler), audio runs: %u during the main thread's work, %u in its sleeps (%.0f %%)\n",
                gTurns, gFrames, gAiRaised.load(), gAiTaken, gAiWorst / 1e6, gAiHandlerWorst / 1e6, gRunsInWork, gRunsElsewhere,
                100.0 * gRunsInWork / (gRunsInWork + gRunsElsewhere ? gRunsInWork + gRunsElsewhere : 1));
    const int limitMs = sAudioHoldMs.load();
    bool ok = gFrames + 2 >= static_cast<unsigned>(gAiRaised.load()) && gAiTaken + 1 >= gAiRaised.load();
    // (the limit plus the host's own scheduling: on a loaded 2-CPU build machine the switch off already shows waits of up to ~7 ms)
    ok = ok && gAiHandlerWorst < (limitMs + 10) * 1000000ll;
    // with the hold, most of the audio thread's runs are in the main thread's sleeps (about 10-20 % in its work, against 55-70 % off)
    if (limitMs >= 4) ok = ok && gRunsInWork * 100 < 35 * (gRunsInWork + gRunsElsewhere);
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    std::_Exit(ok ? 0 : 1);
}

int main() {
    std::thread([] {  // the AI clock
        while (!gStop.load()) {
            std::this_thread::sleep_for(std::chrono::microseconds(17500));
            gAiRaiseTime.store(port_host_time_ns());
            gAiRaised.fetch_add(1);
            port_irq_raise(PORT_IRQ_AI);
        }
    }).detach();
    std::thread([] {  // the DSP mixer: a release is answered 0.2 ms later
        while (!gStop.load()) {
            if (gReleasePending.exchange(false)) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                gDspRaised.fetch_add(1);
                port_irq_raise(PORT_IRQ_DSP);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(20));
            }
        }
    }).detach();
    port_os_boot(entry);
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
