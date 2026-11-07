// GXV_AUDIO_CPU1 during a stage load (r60: one hang leaving the file select for the lit Observatory). The real scheduler file, the SDK's
// OSMessage.c and OSMutex.c, real host threads, ThreadSanitizer (tools/test_os_audio_cpu1.sh).
//
// On top of tests/os_audio_cpu1.cpp's steady model (the audio thread, its AI/DSP interrupts and mixer, a task thread), the load:
//  - a "loader" guest thread (priority 18, below the main thread) that reads "files" (it sleeps until a DVD interrupt a host drive
//    thread raises 0.3-2 ms later), decompresses (work with critical sections and the shared heap mutex, held with interrupts enabled),
//    loads JAudio resources (the JAudio guard, then the shared JAS state written as JASWaveArc/JAUSectionHeap do, in critical sections
//    and under the JASKernel-like mutex the audio thread also takes), creates and ends a helper guest thread per file, and posts each
//    file to the main thread;
//  - a "decomp" guest thread (priority 4, above the main thread: JKRDecomp) woken by the loader;
//  - the main thread opens and closes its windows every turn and, inside them, waits for the loader's files (it sleeps in its window),
//    disables the scheduler for a moment, suspends and resumes the loader, allocates under the heap mutex, and drives JAudio after the
//    guard; outside them it stops sounds with JAUSoundMgr::stopSync's pattern (JASDriver::waitSubFrame: yields until the audio thread
//    has rendered a sub-frame);
//  - the audio thread's task messages block when the task queue is full (as JASAramStream's loads do).
// Checked for the switch off and on: no hang (every turn and every file completes before the timeout), every DSP frame completes, the
// audio thread never overlaps the main thread's or the loader's unprotected JAudio writes, ThreadSanitizer clean.
#include "../platform/src/os/os_thread.cpp"
extern "C" {
#include "../decomp/src/RVL_SDK/os/OSMessage.c"
#include "../decomp/src/RVL_SDK/os/OSMutex.c"
}
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

extern "C" void port_log(const char*, ...) {}
extern "C" void port_log_summary(const char* f, ...) {
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

static void spinFor(int64_t ns, bool polls) {
    const int64_t end = port_host_time_ns() + ns;
    while (port_host_time_ns() < end) {
        if (polls) port_irq_poll();
    }
}

static struct {
    unsigned mute = 0, tempo = 0, ticks = 0, banks = 0;
    unsigned cmds[64] = {};
    unsigned nCmds = 0, done = 0;
} gJas;
static std::atomic<int> gUnprotected{0};
static std::atomic<unsigned> gViolations{0};
static unsigned gMainOwn[4096];
static unsigned gHeapWork = 0, gKernelWork = 0, gTaskWork = 0, gDecompWork = 0;

static OSMessage gAudioMsgs[16], gTaskMsgs[2], gFileMsgs[4], gDecompMsgs[4];
static OSMessageQueue gAudioMQ, gTaskMQ, gFileMQ, gDecompMQ;
static OSThreadQueue gRetraceQ, gDvdQ;
static OSMutex gHeapMutex, gKernelMutex;
static OSThread gAudio, gTask, gLoader, gDecomp, gHelper;
static std::atomic<int> gAiRaised{0};
static std::atomic<bool> gRelease{false}, gStop{false}, gVi{false}, gDvdDone{false};
static std::atomic<int> gDvdRequest{0};
static std::atomic<unsigned> gSubFrameCounter{0};
static unsigned gFrames = 0, gAiTaken = 0, gSub = 0, gOwed = 0, gRuns = 0, gFiles = 0, gHelpers = 0;
static bool gInFrame = false;
static std::atomic<int> gTurnsDone{0};

static void aiIrq() {
    ++gAiTaken;
    OSSendMessage(&gAudioMQ, (OSMessage)1, OS_MESSAGE_NOBLOCK);
}
static void dspIrq() { OSSendMessage(&gAudioMQ, (OSMessage)2, OS_MESSAGE_NOBLOCK); }
static void viIrq() {
    gVi.store(true);
    OSWakeupThread(&gRetraceQ);
}
static void dvdIrq() {
    gDvdDone.store(true);
    OSWakeupThread(&gDvdQ);
}

static void jasWrite() {  // a JAudio write without a critical section (JASTrack::mute, JAISeq params, a wave bank's state)
    gUnprotected.fetch_add(1);
    gJas.mute ^= 1;
    spinFor(10000, false);
    gJas.tempo++;
    gUnprotected.fetch_sub(1);
}

static void subFrame() {
    if (gUnprotected.load()) gViolations.fetch_add(1);
    {
        const BOOL en = OSDisableInterrupts();
        for (unsigned i = 0; i < gJas.nCmds; i++) gJas.done += gJas.cmds[i];
        gJas.nCmds = 0;
        OSRestoreInterrupts(en);
    }
    const int64_t end = port_host_time_ns() + 100000;
    while (port_host_time_ns() < end) {
        gJas.ticks += gJas.mute + gJas.tempo + gJas.banks;
        if ((gJas.ticks & 1023) == 0) port_irq_poll();
    }
    if (gUnprotected.load()) gViolations.fetch_add(1);
    ++gRuns;
    if (gRuns % 5 == 0) {  // the JASKernel command heap (the loader takes it too)
        OSLockMutex(&gKernelMutex);
        gKernelWork++;
        OSUnlockMutex(&gKernelMutex);
    }
    if (gRuns % 11 == 0) {  // a JKRHeap allocation (the main thread and the loader hold it with interrupts enabled)
        OSLockMutex(&gHeapMutex);
        gHeapWork++;
        OSUnlockMutex(&gHeapMutex);
    }
    if (gRuns % 3 == 0) OSSendMessage(&gTaskMQ, (OSMessage)1, OS_MESSAGE_BLOCK);  // the stream loader; blocks when its queue is full
    gSubFrameCounter.fetch_add(1);
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

static void* taskMain(void*) {  // slow: its 2-entry queue fills, and the audio thread's sends block
    for (;;) {
        OSMessage msg;
        OSReceiveMessage(&gTaskMQ, &msg, OS_MESSAGE_BLOCK);
        const BOOL en = OSDisableInterrupts();
        gTaskWork++;
        OSRestoreInterrupts(en);
        spinFor(150000, true);
    }
    return nullptr;
}

static void* decompMain(void*) {
    for (;;) {
        OSMessage msg;
        OSReceiveMessage(&gDecompMQ, &msg, OS_MESSAGE_BLOCK);
        const BOOL en = OSDisableInterrupts();
        gDecompWork++;
        OSRestoreInterrupts(en);
        spinFor(200000, true);
    }
    return nullptr;
}

static void* helperMain(void*) {  // a short-lived guest thread per file (a JKRTask / a scene loader's worker)
    const BOOL en = OSDisableInterrupts();
    gHelpers++;
    OSRestoreInterrupts(en);
    spinFor(50000, true);
    return nullptr;
}

static void* loaderMain(void*) {
    static char helperStack[0x4000];
    for (unsigned file = 0;; file++) {
        // a disc read: sleep until the DVD interrupt
        {
            const BOOL en = OSDisableInterrupts();
            gDvdDone.store(false);
            gDvdRequest.fetch_add(1);
            while (!gDvdDone.load()) OSSleepThread(&gDvdQ);
            OSRestoreInterrupts(en);
        }
        OSSendMessage(&gDecompMQ, (OSMessage)1, OS_MESSAGE_NOBLOCK);
        // decompress: heap mutex held with interrupts enabled, critical sections
        OSLockMutex(&gHeapMutex);
        gHeapWork++;
        spinFor(300000, true);
        OSUnlockMutex(&gHeapMutex);
        // a JAudio resource (JASWaveArc / JAUSectionHeap): the guard, a bank's state, the kernel heap
        if (port_audio_touch) port_audio_touch();
        {
            const BOOL en = OSDisableInterrupts();
            gJas.banks++;
            if (gJas.nCmds < 64) gJas.cmds[gJas.nCmds++] = 3;
            OSRestoreInterrupts(en);
        }
        OSLockMutex(&gKernelMutex);
        gKernelWork++;
        spinFor(40000, true);
        OSUnlockMutex(&gKernelMutex);
        // a helper thread, created, run and joined
        if (OSIsThreadTerminated(&gHelper) || gHelper.state == 0) {
            OSCreateThread(&gHelper, helperMain, nullptr, helperStack + sizeof helperStack, sizeof helperStack, 17, 0);
            OSResumeThread(&gHelper);
            OSJoinThread(&gHelper, nullptr);
        }
        {
            const BOOL en = OSDisableInterrupts();
            gFiles++;
            OSRestoreInterrupts(en);
        }
        OSSendMessage(&gFileMQ, (OSMessage)(intptr_t)(file + 1), OS_MESSAGE_BLOCK);
    }
    return nullptr;
}

static void entry() {
    OSInitMessageQueue(&gAudioMQ, gAudioMsgs, 16);
    OSInitMessageQueue(&gTaskMQ, gTaskMsgs, 2);
    OSInitMessageQueue(&gFileMQ, gFileMsgs, 4);
    OSInitMessageQueue(&gDecompMQ, gDecompMsgs, 4);
    OSInitThreadQueue(&gRetraceQ);
    OSInitThreadQueue(&gDvdQ);
    OSInitMutex(&gHeapMutex);
    OSInitMutex(&gKernelMutex);
    port_irq_set_handler(PORT_IRQ_AI, aiIrq);
    port_irq_set_handler(PORT_IRQ_DSP, dspIrq);
    port_irq_set_handler(PORT_IRQ_VI, viIrq);
    port_irq_set_handler(PORT_IRQ_DVD, dvdIrq);
    port_audio_irq_urgent(0);
    static char sA[0x4000], sT[0x4000], sL[0x4000], sD[0x4000];
    OSCreateThread(&gAudio, audioMain, nullptr, sA + sizeof sA, sizeof sA, 2, 1);
    OSCreateThread(&gTask, taskMain, nullptr, sT + sizeof sT, sizeof sT, 3, 1);
    OSCreateThread(&gDecomp, decompMain, nullptr, sD + sizeof sD, sizeof sD, 4, 1);
    OSCreateThread(&gLoader, loaderMain, nullptr, sL + sizeof sL, sizeof sL, 18, 1);
    OSResumeThread(&gAudio);
    OSResumeThread(&gTask);
    OSResumeThread(&gDecomp);
    OSResumeThread(&gLoader);
    unsigned filesSeen = 0;
    for (int turn = 0; turn < 200; ++turn) {
        // update(): JAI work, and one turn in 10 a stop with JAUSoundMgr::stopSync's wait (JASDriver::waitSubFrame)
        for (int i = 0; i < 4; i++) {
            spinFor(700000, true);
            jasWrite();
        }
        if (turn % 10 == 4) {
            const unsigned c = gSubFrameCounter.load();
            const int64_t limit = port_host_time_ns() + 200000000;
            while (gSubFrameCounter.load() == c && port_host_time_ns() < limit) OSYieldThread();
        }
        // calcAnim(), a window: own work, a file from the loader (it sleeps in the window when none is ready), the scheduler off for a
        // moment, the loader suspended and resumed, the heap mutex, one turn in 6 JAudio after the guard
        port_audio_window(1);
        for (int i = 0; i < 20; i++) {
            const int64_t end = port_host_time_ns() + 100000;
            unsigned k = i;
            while (port_host_time_ns() < end) gMainOwn[(k++ * 7) & 4095] += k;
            port_irq_poll();
            if (i == 3) {
                OSMessage m;
                if (turn % 3 == 0) {
                    OSReceiveMessage(&gFileMQ, &m, OS_MESSAGE_BLOCK);  // the scene waits for a file
                    filesSeen++;
                } else if (OSReceiveMessage(&gFileMQ, &m, OS_MESSAGE_NOBLOCK)) {
                    filesSeen++;
                }
            }
            if (i == 7) {
                OSDisableScheduler();
                spinFor(30000, true);
                OSEnableScheduler();
            }
            if (i == 9 && turn % 4 == 1) {
                OSSuspendThread(&gLoader);
                spinFor(20000, true);
                OSResumeThread(&gLoader);
            }
            if (i == 12) {
                OSLockMutex(&gHeapMutex);
                gHeapWork++;
                spinFor(20000, true);
                OSUnlockMutex(&gHeapMutex);
            }
            if (i == 15 && turn % 6 == 2) {
                port_audio_touch();
                jasWrite();
            }
        }
        port_audio_window(0);
        if (turn & 1) {  // draw(), a window: the front-end wait, then work
            port_audio_window(1);
            std::this_thread::sleep_for(std::chrono::microseconds(1500));
            spinFor(1500000, false);
            port_audio_window(0);
        }
        gTurnsDone.store(turn + 1);
        // the retrace, ahead of time only
        if (turn % 2 == 0) {
            gVi.store(false);
            const BOOL en = OSDisableInterrupts();
            while (!gVi.load()) OSSleepThread(&gRetraceQ);
            OSRestoreInterrupts(en);
        }
    }
    gStop.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    int64_t ns[7] = {};
    unsigned n[8] = {};
    const int mode = port_audio_cpu_stats(ns, n);
    std::printf("load model, GXV_AUDIO_CPU1=%d: turns %d, files %u (seen %u), helpers %u, DSP frames %u, AI raised %d taken %u; audio on CPU0 "
                "%.1f ms (+%.1f ms in sleeps), on CPU1 %.1f ms (%u runs); CPU0 waited %.1f ms (%u); %u windows (%u closed by the guard); "
                "violations %u; task %u decomp %u heap %u kernel %u\n",
                mode, gTurnsDone.load(), gFiles, filesSeen, gHelpers, gFrames, gAiRaised.load(), gAiTaken, ns[1] / 1e6, ns[4] / 1e6, ns[5] / 1e6,
                n[1], ns[6] / 1e6, n[3], n[5], n[6], gViolations.load(), gTaskWork, gDecompWork, gHeapWork, gKernelWork);
    bool ok = gTurnsDone.load() == 200 && filesSeen >= 60 && gViolations.load() == 0;
    ok = ok && gFrames + 4 >= static_cast<unsigned>(gAiRaised.load());
    if (mode == 1) ok = ok && n[1] > 0;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    std::_Exit(ok ? 0 : 1);
}

int main() {
    std::thread([] {  // the watchdog: a hang fails the test with the state it is in
        int last = -1;
        for (int s = 0; s < 90; s++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (gStop.load()) return;
            const int now = gTurnsDone.load();
            if (now == last && s > 3) {
                std::printf("HANG: no main-thread turn for 1 s at turn %d (CPU1 run %d, gate %d, pending 0x%x)\nFAIL\n", now, (int)sCpu1Run.load(),
                            (int)sCpu0Gate.load(), sPendingIrq.load());
                std::fflush(stdout);
                std::_Exit(2);
            }
            last = now;
        }
    }).detach();
    std::thread([] {  // AI clock
        while (!gStop.load()) {
            std::this_thread::sleep_for(std::chrono::microseconds(17500));
            gAiRaised.fetch_add(1);
            port_irq_raise(PORT_IRQ_AI);
        }
    }).detach();
    std::thread([] {  // DSP mixer
        while (!gStop.load()) {
            if (gRelease.exchange(false)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                port_irq_raise(PORT_IRQ_DSP);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(20));
            }
        }
    }).detach();
    std::thread([] {  // retrace
        while (!gStop.load()) {
            std::this_thread::sleep_for(std::chrono::microseconds(16667));
            port_irq_raise(PORT_IRQ_VI);
        }
    }).detach();
    std::thread([] {  // the drive: a read completes 0.3-2 ms after it is asked for
        int seen = 0;
        unsigned r = 1;
        while (!gStop.load()) {
            const int want = gDvdRequest.load();
            if (want != seen) {
                seen = want;
                r = r * 1103515245u + 12345u;
                std::this_thread::sleep_for(std::chrono::microseconds(300 + (r >> 16) % 1700));
                port_irq_raise(PORT_IRQ_DVD);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    }).detach();
    port_os_boot(entry);
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
