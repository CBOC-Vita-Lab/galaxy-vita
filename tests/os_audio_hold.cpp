// GXV_AUDIO_HOLD: the game's main thread, running, holds the AI interrupt and a DSP frame's closing mail for its next sleep
// (platform/src/os/os_thread.cpp, port_irq_raise). The real scheduler file, as in tests/os_irq.cpp; the clock is the test's.
// Checked: off is the old delivery; a hold keeps only the audio bits (others are taken at once); a DSP frame in progress, a yield,
// another guest thread holding the CPU and the idle loop take them at once; the limit; interrupts disabled still mask everything;
// the counts of the audio log line.
// Build (tools/test_os_audio_hold.sh): clang++ or g++ -std=c++20 -ffunction-sections -Wl,--gc-sections, see that script.
#include "../platform/src/os/os_thread.cpp"
#include <cstdio>
#include <cstdlib>

extern "C" void port_log(const char*, ...) {}
static int64_t gNow = 1000000;  // ns
extern "C" int64_t port_host_time_ns() { return gNow; }
extern "C" void port_fatal(const char*, ...) { std::abort(); }
extern "C" void* port_low_alloc(size_t) { std::abort(); }
extern "C" void __OSUnlockAllMutex(OSThread*) { std::abort(); }

static unsigned aiCalls, dspCalls, viCalls, checks;
static void require(bool value, const char* message) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "audio hold FAILED: %s (ai %u dsp %u vi %u pending %08x)\n", message, aiCalls, dspCalls, viCalls, sPendingIrq.load());
        std::exit(1);
    }
}
static void ai() { require(sInIrq && !sIntEnabled, "handlers run masked on the virtual CPU"); ++aiCalls; }
static void dsp() { ++dspCalls; }
static void vi() { ++viCalls; }
static void polls(unsigned n) { for (unsigned i = 0; i < n; ++i) port_irq_poll(); }

int main() {
    tlsSelf = &DefaultThread;
    __OSCurrentThread = &DefaultThread;
    sCpuOwner = &DefaultThread;
    DefaultThread.state = OS_THREAD_STATE_RUNNING;
    DefaultThread.priority = 16;
    sIntEnabled = true;
    port_irq_set_handler(PORT_IRQ_AI, ai);
    port_irq_set_handler(PORT_IRQ_DSP, dsp);
    port_irq_set_handler(PORT_IRQ_VI, vi);

    // Off: taken at the first safe point, as before.
    port_irq_raise(PORT_IRQ_AI);
    port_irq_poll();
    require(aiCalls == 1 && sPendingIrq.load() == 0, "off: delivered at the next safe point");
    unsigned a, b, c, d;
    require(port_audio_hold_stats(&a, &b, &c, &d) == 0 && a + b + c + d == 0, "off: nothing counted");

    sAudioHoldMs.store(8);
    // Before the first DSP frame (boot handshakes) the scheduler treats the DSP as busy: nothing is held.
    port_irq_raise(PORT_IRQ_AI);
    port_irq_poll();
    require(aiCalls == 2, "no hold before the DSP has reported a finished frame");
    port_audio_hold_stats(&a, &b, &c, &d);
    require(c == 1 && a + b + d == 0, "counted: taken at once");
    port_audio_irq_urgent(0);  // between frames

    // A hold: the AI bit stays pending for the limit while the main thread runs.
    gNow = 10000000;
    port_irq_raise(PORT_IRQ_AI);
    polls(64);
    require(aiCalls == 2 && (sPendingIrq.load() & (1u << PORT_IRQ_AI)), "the running main thread holds the AI interrupt");
    // Another interrupt pending with it is taken now; the AI one stays.
    port_irq_raise(PORT_IRQ_VI);
    port_irq_poll();
    require(viCalls == 1 && aiCalls == 2 && sPendingIrq.load() == (1u << PORT_IRQ_AI), "other interrupts are not held");
    // Interrupts disabled: nothing at all, whatever the age.
    {
        const BOOL en = OSDisableInterrupts();
        gNow += 20000000;
        polls(16);
        require(aiCalls == 2, "masked: nothing delivered");
        gNow = 10000000 + 7000000;  // (back inside the limit for the next step)
        OSRestoreInterrupts(en);
    }
    require(aiCalls == 2, "re-enabling inside the limit keeps the hold");
    // The limit: at the first clock read (every 4th holding safe point) at or after raise + 8 ms.
    gNow = 10000000 + 8000000;
    polls(4);
    require(aiCalls == 3 && sPendingIrq.load() == 0, "held for the limit, then taken by the running main thread");
    port_audio_hold_stats(&a, &b, &c, &d);
    require(b == 1 && a == 0 && c == 0 && d == 1, "counted: one hold, ended at the limit");

    // A DSP frame in progress: its interrupts are taken at once, and an AI interrupt pending with them too.
    port_irq_raise(PORT_IRQ_AI);
    port_irq_poll();
    require(aiCalls == 3, "held again");
    port_audio_irq_urgent(1);
    port_irq_raise(PORT_IRQ_DSP);
    port_irq_poll();
    require(aiCalls == 4 && dspCalls == 1 && sPendingIrq.load() == 0, "a frame in progress: taken at once");
    port_audio_hold_stats(&a, &b, &c, &d);
    require(c == 1 && d == 1, "counted: taken at once");
    port_audio_irq_urgent(0);

    // A yield takes held audio interrupts (JASDriver::waitSubFrame).
    port_irq_raise(PORT_IRQ_DSP);
    polls(20);
    require(dspCalls == 1, "a frame's closing mail is held");
    OSYieldThread();
    require(dspCalls == 2 && sPendingIrq.load() == 0 && !sAudioHoldBypass.load(), "a yield takes it");
    require(DefaultThread.state == OS_THREAD_STATE_RUNNING && __OSCurrentThread == &DefaultThread, "the yield came back to the only thread");
    port_audio_hold_stats(&a, &b, &c, &d);
    require(c == 1 && d == 1 && a + b == 0, "counted: a hold ended by a yield");

    // Another guest thread holding the CPU (the main thread asleep) takes it at its first safe point.
    port_irq_raise(PORT_IRQ_AI);
    polls(3);
    require(aiCalls == 4, "held by the running main thread");
    static OSThread other;
    __OSCurrentThread = &other;
    port_irq_poll();
    require(aiCalls == 5 && sPendingIrq.load() == 0, "another guest thread takes it at once");
    __OSCurrentThread = &DefaultThread;
    port_audio_hold_stats(&a, &b, &c, &d);
    require(a == 1 && d == 1, "counted: a hold that ended in the main thread's sleep");

    // The idle loop (no thread runnable) takes everything pending.
    port_irq_raise(PORT_IRQ_DSP);
    port_irq_raise(PORT_IRQ_AI);
    polls(9);
    require(dspCalls == 2 && aiCalls == 5, "both held");
    runIrqHandlers();  // (what idleUntilRunnable does)
    require(dspCalls == 3 && aiCalls == 6 && sPendingIrq.load() == 0, "the idle loop takes them");

    // A raise whose time is not yet written (raised between the bit and its time) is held from the first clock read, not forever.
    sPendingIrq.fetch_or(1u << PORT_IRQ_AI);
    sAudioIrqSince.store(0);
    gNow = 50000000;
    polls(16);
    require(aiCalls == 6 && sAudioIrqSince.load() != 0, "an unknown raise time is taken from the first clock read");
    gNow = 50000000 + 8000000;
    polls(4);
    require(aiCalls == 7 && sPendingIrq.load() == 0, "and then held for the limit only");

    // No clock: never held blind.
    port_irq_raise(PORT_IRQ_AI);
    gNow = 0;
    polls(4);
    require(aiCalls == 8, "no clock: taken at the first clock read");

    std::printf("audio interrupt hold: %u checks passed\n", checks);
    return 0;
}
