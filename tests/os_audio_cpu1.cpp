// GXV_AUDIO_CPU1 (platform/src/os/os_thread.cpp, port_audio_window): the game's audio thread on a second virtual CPU, with the real
// scheduler file, the SDK's own OSMessage.c and OSMutex.c, and real host threads.  Built with ThreadSanitizer by tools/test_os_audio_cpu1.sh.
//
// A model of the game's threads and of what they share:
//  - "main" (priority 16) runs 160 turns: update() (6 ms: polls, the JAI side writing the shared JAS state WITHOUT a critical section, as
//    JAISeq::mixOut_ does with JASTrack::mute/pause, plus a port command queued in a critical section, plus a heap mutex held with
//    interrupts enabled), then calcAnim() in a window (4 ms of its own work with polls, short critical sections, the heap mutex; one turn
//    in eight it drives JAudio there, after the guard port_audio_touch), on every other turn draw() in a window (a 3 ms host wait, as the
//    front-end wait, then work), then the retrace wait (it sleeps until a VI interrupt).
//  - "audio" (priority 2, JASAudioThread): marks itself, then takes messages from its queue as JASAudioThread::run does.  The AI handler
//    starts a DSP frame of 7 sub-frames; each sub-frame is 120 us of work on the shared JAS state plus the port commands (in a critical
//    section), then its release to the "mixer" host thread, which answers 100 us later with a DSP interrupt; one run in 16 locks the heap
//    mutex, one in 8 sends a message to "task".
//  - "task" (priority 3, a JASTaskThread): wakes on its queue and does a little work in a critical section.
//  - an OS alarm interrupt every 0.25 ms whose handler queues a port command (CPU0 takes it while CPU1 may be running).
// Checked, for the switch off and on: every DSP frame completes (no deadlock, no lost interrupt); the audio thread never runs while the
// main thread is between its unprotected writes to the shared state (an explicit flag, and ThreadSanitizer for every access); and with
// the switch on most of the audio thread's time moves off CPU0.  Prints the times. Not Vita timing.
#include "../platform/src/os/os_thread.cpp"
extern "C" {
#include "../decomp/src/RVL_SDK/os/OSMessage.c"
#include "../decomp/src/RVL_SDK/os/OSMutex.c"
}
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

extern "C" void port_log(const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    std::vprintf(f, ap);
    std::printf("\n");
    va_end(ap);
}
extern "C" void port_log_summary(const char* f, ...) {  // (the basic log: the "audio cpu1:" start-up line)
    va_list ap;
    va_start(ap, f);
    std::vprintf(f, ap);
    std::printf("\n");
    va_end(ap);
}
extern "C" int64_t port_host_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" void port_fatal(const char* f, ...) { std::fprintf(stderr, "fatal: %s\n", f); std::abort(); }
extern "C" void* port_low_alloc(size_t n) { return std::malloc(n); }
extern "C" int port_is_game_thread(void);

static void spinFor(int64_t ns, bool polls) {
    const int64_t end = port_host_time_ns() + ns;
    while (port_host_time_ns() < end) {
        if (polls) port_irq_poll();
    }
}

// The JAudio state the audio thread owns and the main thread's JAI side drives (plain memory: ThreadSanitizer sees every access).
static struct {
    unsigned mute = 0, pause = 0, tempo = 0;  // JASTrack fields JAISeq::mixOut_ writes without a critical section
    unsigned ticks = 0;                       // the audio thread's own
    unsigned cmds[64] = {};                   // JASPortCmd's list (critical sections on both sides)
    unsigned nCmds = 0, done = 0;
} gJas;
static std::atomic<int> gMainUnprotected{0};  // the main thread is between unprotected writes to gJas
static std::atomic<unsigned> gViolations{0};
static unsigned gMainOwn[4096];               // the main thread's own data (calc-anim)
static unsigned gTaskWork = 0;                // the task thread's (critical sections)

static OSMessage gAudioMsgs[16], gTaskMsgs[8];
static OSMessageQueue gAudioMQ, gTaskMQ;
static OSThreadQueue gRetraceQ;
static OSMutex gHeapMutex;
static unsigned gHeapWork = 0;                // under gHeapMutex
static OSThread gAudio, gTask;
static std::atomic<int> gAiRaised{0};
static std::atomic<bool> gRelease{false}, gStop{false}, gVi{false};
static unsigned gFrames = 0, gAiTaken = 0, gSub = 0, gOwed = 0;  // (guest only)
static bool gInFrame = false;
static unsigned gRuns = 0;
static std::atomic<int64_t> gAiRaiseTime{0};
static int64_t gAiWorst = 0;

static void aiIrq() {
    ++gAiTaken;
    const int64_t w = port_host_time_ns() - gAiRaiseTime.load();
    if (w > gAiWorst) gAiWorst = w;
    OSSendMessage(&gAudioMQ, (OSMessage)1, OS_MESSAGE_NOBLOCK);
}
static void dspIrq() { OSSendMessage(&gAudioMQ, (OSMessage)2, OS_MESSAGE_NOBLOCK); }
static void alarmIrq() {  // a timer's handler queues a port command (handlers are critical sections, as the audio thread's command list)
    if (gJas.nCmds < 64) gJas.cmds[gJas.nCmds++] = 2;
}
static void viIrq() {
    gVi.store(true);
    OSWakeupThread(&gRetraceQ);
}

static void subFrame() {  // JASDriver::updateDSP: the work, then the release
    if (gMainUnprotected.load()) gViolations.fetch_add(1);
    {
        const BOOL en = OSDisableInterrupts();  // JASPortCmd::execAllCommand
        for (unsigned i = 0; i < gJas.nCmds; i++) gJas.done += gJas.cmds[i];
        gJas.nCmds = 0;
        OSRestoreInterrupts(en);
    }
    const int64_t end = port_host_time_ns() + 120000;
    while (port_host_time_ns() < end) {
        gJas.ticks += gJas.mute + gJas.pause + gJas.tempo;
        if ((gJas.ticks & 1023) == 0) port_irq_poll();  // (operator new and OSGetTick are safe points)
    }
    if (gMainUnprotected.load()) gViolations.fetch_add(1);
    if (++gRuns % 16 == 0) {
        OSLockMutex(&gHeapMutex);  // a JKRHeap allocation
        gHeapWork++;
        OSUnlockMutex(&gHeapMutex);
    }
    if (gRuns % 8 == 0) OSSendMessage(&gTaskMQ, (OSMessage)1, OS_MESSAGE_NOBLOCK);  // the stream loader
    port_audio_irq_urgent(gSub < 7 ? 1 : 0);
    gRelease.store(true);
}

static void* audioMain(void*) {
    port_audio_thread_mark();
    for (;;) {
        OSMessage msg;
        OSReceiveMessage(&gAudioMQ, &msg, OS_MESSAGE_BLOCK);
        if ((intptr_t)msg == 1) {
            ++gOwed;
        } else if (gInFrame) {
            if (gSub < 7) {
                ++gSub;
                subFrame();
            } else {
                gInFrame = false;
                ++gFrames;
            }
        }
        if (!gInFrame && gOwed) {
            --gOwed;
            gInFrame = true;
            gSub = 1;
            subFrame();
        }
    }
    return nullptr;
}

static void* taskMain(void*) {
    for (;;) {
        OSMessage msg;
        OSReceiveMessage(&gTaskMQ, &msg, OS_MESSAGE_BLOCK);
        const BOOL en = OSDisableInterrupts();
        gTaskWork++;
        OSRestoreInterrupts(en);
    }
    return nullptr;
}

static void jaiCalc() {  // the main thread drives JAudio (update(), or after the guard in a window)
    gMainUnprotected.store(1);
    gJas.mute ^= 1;
    spinFor(20000, false);  // (no safe point between the writes: on one CPU nothing can come in between)
    gJas.pause ^= 1;
    gJas.tempo++;
    gMainUnprotected.store(0);
    const BOOL en = OSDisableInterrupts();  // JASPortCmd::addPortCmdOnce
    if (gJas.nCmds < 64) gJas.cmds[gJas.nCmds++] = 1;
    OSRestoreInterrupts(en);
}

static void heapOp() {  // a JKRHeap allocation: the mutex held with interrupts enabled
    OSLockMutex(&gHeapMutex);
    gHeapWork++;
    spinFor(30000, true);
    OSUnlockMutex(&gHeapMutex);
}

static int gTurns = 0;
static int64_t gUpdateNs = 8500000;  // TEST_BUSY=0: 4.8 ms (the main thread then sleeps several ms a turn)
static int64_t gWindowNs = 0, gTurnNs = 0;
static void entry() {
    OSInitMessageQueue(&gAudioMQ, gAudioMsgs, 16);
    OSInitMessageQueue(&gTaskMQ, gTaskMsgs, 8);
    OSInitThreadQueue(&gRetraceQ);
    OSInitMutex(&gHeapMutex);
    port_irq_set_handler(PORT_IRQ_AI, aiIrq);
    port_irq_set_handler(PORT_IRQ_DSP, dspIrq);
    port_irq_set_handler(PORT_IRQ_VI, viIrq);
    port_irq_set_handler(PORT_IRQ_ALARM, alarmIrq);
    port_audio_irq_urgent(0);
    static char stackA[0x4000], stackT[0x4000];
    OSCreateThread(&gAudio, audioMain, nullptr, stackA + sizeof stackA, sizeof stackA, 2, 1);
    OSCreateThread(&gTask, taskMain, nullptr, stackT + sizeof stackT, sizeof stackT, 3, 1);
    OSResumeThread(&gAudio);
    OSResumeThread(&gTask);
    int64_t prev = port_host_time_ns();
    if (const char* b = getenv("TEST_BUSY"); b && atoi(b) == 0) gUpdateNs = 4800000;
    for (gTurns = 0; gTurns < 160; ++gTurns) {
        const int64_t t0 = port_host_time_ns();
        // update(): the JAI side, heap use, polls
        for (int i = 0; i < 6; i++) {
            spinFor(gUpdateNs / 6, true);
            jaiCalc();
            if (i == 3) heapOp();
        }
        // calcAnim(), in a window
        const int64_t w0 = port_host_time_ns();
        port_audio_window(1);
        for (int i = 0; i < 40; i++) {
            const int64_t end = port_host_time_ns() + 100000;
            unsigned k = i;
            while (port_host_time_ns() < end) gMainOwn[(k++ * 7) & 4095] += k;
            port_irq_poll();
            if (i % 10 == 5) {
                const BOOL en = OSDisableInterrupts();
                gMainOwn[i] ^= 1;
                OSRestoreInterrupts(en);
            }
            if (i == 20) heapOp();
            if (i == 30 && gTurns % 8 == 3) {
                port_audio_touch();  // the guard: an actor started a sound from calcAnim
                jaiCalc();
            }
        }
        port_audio_window(0);
        // draw() every other turn, in a window: the front-end wait (a host wait), then GX work
        if (gTurns & 1) {
            port_audio_window(1);
            std::this_thread::sleep_for(std::chrono::microseconds(3000));
            spinFor(1000000, false);
            port_audio_window(0);
        }
        gWindowNs += port_host_time_ns() - w0;
        gTurnNs += port_host_time_ns() - t0;
        // the retrace wait, only for a turn ahead of the display's clock (frame skipping: a late turn goes on at once)
        prev += 16666667;
        if (port_host_time_ns() < prev) {
            gVi.store(false);
            const BOOL en = OSDisableInterrupts();
            while (!gVi.load() && port_host_time_ns() < prev) OSSleepThread(&gRetraceQ);
            OSRestoreInterrupts(en);
        }
    }
    gStop.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    int64_t ns[7] = {};
    unsigned n[8] = {};
    const int mode = port_audio_cpu_stats(ns, n);
    const double audio0 = ns[1] / 1e6, audioSleep = ns[4] / 1e6, audio1 = ns[5] / 1e6, wait = ns[6] / 1e6;
    std::printf("GXV_AUDIO_CPU1=%d: turns %d, DSP frames %u, AI raised %d taken %u (worst wait %.1f ms); audio thread on CPU0 %.1f ms taken from "
                "the main thread + %.1f ms in its sleeps (%u runs), on CPU1 %.1f ms (%u runs, %u interrupt batches); CPU0 waited %.1f ms (%u, %u "
                "at a window's end); %u windows, %u closed by the guard; %u gate closes; %u sub-frames; violations %u; heap %u task %u\n",
                mode, gTurns, gFrames, gAiRaised.load(), gAiTaken, gAiWorst / 1e6, audio0, audioSleep, n[0], audio1, n[1], n[2], wait, n[3],
                n[4], n[5], n[6], n[7], gRuns, gViolations.load(), gHeapWork, gTaskWork);
    std::printf("main thread's CPU0 time lost to the audio thread: %.1f ms (%.2f ms per turn)\n", audio0 + wait, (audio0 + wait) / gTurns);
    bool ok = gFrames + 4 >= static_cast<unsigned>(gAiRaised.load()) && gAiTaken + 2 >= static_cast<unsigned>(gAiRaised.load());
    ok = ok && gViolations.load() == 0 && gTaskWork > 0;
    ok = ok && gAiWorst < 40 * 1000000ll;  // (the hold's 12 ms plus the host's scheduling on a loaded build machine)
    if (mode == 1) {
        // the windows are ~45 % of the turn; frame starts wait for them: most of the audio thread's time is on CPU1
        // (GXV_AUDIO_HOLD=0: a frame's start is not held for the next window, so only the frames begun in one move: less, but some)
        ok = ok && audio1 > (sAudioHoldMs.load() > 0 ? 2.0 : 0.3) * (audio0 + wait) && n[6] > 0;
    } else {
        ok = ok && audio1 == 0.0 && n[1] == 0;
    }
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    std::_Exit(ok ? 0 : 1);
}

int main() {
    std::thread([] {  // the AI clock: a DAC block every 17.5 ms
        while (!gStop.load()) {
            std::this_thread::sleep_for(std::chrono::microseconds(17500));
            gAiRaiseTime.store(port_host_time_ns());
            gAiRaised.fetch_add(1);
            port_irq_raise(PORT_IRQ_AI);
        }
    }).detach();
    std::thread([] {  // the DSP mixer thread: a release is answered 100 us later
        while (!gStop.load()) {
            if (gRelease.exchange(false)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                port_irq_raise(PORT_IRQ_DSP);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(20));
            }
        }
    }).detach();
    std::thread([] {  // an OS alarm every 0.25 ms (a timer interrupt for CPU0 while CPU1 may be running)
        while (!gStop.load()) {
            std::this_thread::sleep_for(std::chrono::microseconds(250));
            port_irq_raise(PORT_IRQ_ALARM);
        }
    }).detach();
    std::thread([] {  // the retrace
        while (!gStop.load()) {
            std::this_thread::sleep_for(std::chrono::microseconds(16667));
            port_irq_raise(PORT_IRQ_VI);
        }
    }).detach();
    port_os_boot(entry);
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
