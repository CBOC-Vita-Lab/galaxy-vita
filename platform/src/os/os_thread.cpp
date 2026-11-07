// Wii OS threads on host threads, with the Broadway's single-core semantics.
//
// The scheduling logic (32 priority run queues, priority inheritance through
// mutex queues, the Reschedule counter) is carried over from the SDK's
// OSThread.c.  What changes is the context switch: every OSThread runs on its
// own host thread, and exactly one of them holds the "CPU" at a time.  A
// switch hands the CPU to the next thread and blocks the current host thread
// until the scheduler picks it again.
//
// Interrupts (alarms, VI retrace, DVD/audio completion, GPU tokens) are raised
// by host threads and delivered on the CPU at safe points: when interrupts get
// re-enabled, at explicit port_irq_poll() calls, and whenever the CPU idles.
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <unwind.h>
#include <string.h>
#include <time.h>
#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#endif

#include <malloc.h>
#if defined(__vita__)
#include "memory.h"
#endif

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "port/heap_routing.h"
#include "port/hot_knob.h"
#include "port/diagnostics.h"
#include "port/port.h"
#include "port/game_progress.h"
#include "port/qemu_hooks.h"
#include "port/vclock.h"
#include "revolution/os.h"

extern "C" {
OSThread* __OSCurrentThread;
OSThreadQueue __OSActiveThreadQueue;
volatile OSContext* __OSFPUContext;
u32 __OSFpscrEnableBits;
}

extern "C" void __OSUnlockAllMutex(OSThread* thread);
extern "C" OSPriority __OSGetEffectivePriority(OSThread* thread);

// ---------------------------------------------------------------------------
// Scheduler state (only touched by the thread holding the CPU)
// ---------------------------------------------------------------------------
static volatile u32 RunQueueBits;
static OSThreadQueue RunQueue[32];
static volatile BOOL RunQueueHint;
static volatile s32 Reschedule;
static OSThread DefaultThread;
static OSContext IdleContext;
static OSContext* sCurrentContext;

#define EnqueueTail(queue, thread, link)                                                                                                             \
    do {                                                                                                                                             \
        OSThread* __prev = (queue)->tail;                                                                                                            \
        if (__prev == NULL)                                                                                                                          \
            (queue)->head = (thread);                                                                                                                \
        else                                                                                                                                         \
            __prev->link.next = (thread);                                                                                                            \
        (thread)->link.prev = __prev;                                                                                                                \
        (thread)->link.next = NULL;                                                                                                                  \
        (queue)->tail = (thread);                                                                                                                    \
    } while (0)

#define EnqueuePrio(queue, thread, link)                                                                                                             \
    do {                                                                                                                                             \
        OSThread* __prev;                                                                                                                            \
        OSThread* __next;                                                                                                                            \
        for (__next = (queue)->head; __next && __next->priority <= thread->priority; __next = __next->link.next)                                     \
            ;                                                                                                                                        \
        if (__next == NULL)                                                                                                                          \
            EnqueueTail(queue, thread, link);                                                                                                        \
        else {                                                                                                                                       \
            (thread)->link.next = __next;                                                                                                            \
            __prev = __next->link.prev;                                                                                                              \
            __next->link.prev = (thread);                                                                                                            \
            (thread)->link.prev = __prev;                                                                                                            \
            if (__prev == NULL)                                                                                                                      \
                (queue)->head = (thread);                                                                                                            \
            else                                                                                                                                     \
                __prev->link.next = (thread);                                                                                                        \
        }                                                                                                                                            \
    } while (0)

#define DequeueItem(queue, thread, link)                                                                                                             \
    do {                                                                                                                                             \
        OSThread* __next = (thread)->link.next;                                                                                                      \
        OSThread* __prev = (thread)->link.prev;                                                                                                      \
        if (__next == NULL)                                                                                                                          \
            (queue)->tail = __prev;                                                                                                                  \
        else                                                                                                                                         \
            __next->link.prev = __prev;                                                                                                              \
        if (__prev == NULL)                                                                                                                          \
            (queue)->head = __next;                                                                                                                  \
        else                                                                                                                                         \
            __prev->link.next = __next;                                                                                                              \
    } while (0)

#define DequeueHead(queue, thread, link)                                                                                                             \
    do {                                                                                                                                             \
        OSThread* __next;                                                                                                                            \
        (thread) = (queue)->head;                                                                                                                    \
        __next = (thread)->link.next;                                                                                                            \
        if (__next == NULL)                                                                                                                          \
            (queue)->tail = NULL;                                                                                                                    \
        else                                                                                                                                         \
            __next->link.prev = NULL;                                                                                                                \
        (queue)->head = __next;                                                                                                                      \
    } while (0)

#define IsSuspended(suspendCount) (0 < (suspendCount))

// ---------------------------------------------------------------------------
// Host side of each OSThread
// ---------------------------------------------------------------------------
struct HostThread {
    OSThread* thread = nullptr;
    void* (*func)(void*) = nullptr;
    void* param = nullptr;
    std::condition_variable cv;
    bool started = false;
    bool cancelled = false;
    bool intEnabled = true;  // saved MSR[EE] while switched out
    size_t stackSize = 0;
    pthread_t pt{};          // host thread (for diagnostics)
    bool hasPt = false;
};

static constexpr int kMaxThreads = 64;
static HostThread sHost[kMaxThreads];
static OSThread* sHostOwner[kMaxThreads];

static std::mutex sCpuMutex;
static OSThread* sCpuOwner;  // guarded by sCpuMutex
static thread_local OSThread* tlsSelf;
static thread_local int tlsHostAllocDepth;

static HostThread* hostOf(OSThread* t) {
    for (int i = 0; i < kMaxThreads; i++) {
        if (sHostOwner[i] == t) {
            return &sHost[i];
        }
    }
    for (int i = 0; i < kMaxThreads; i++) {
        if (sHostOwner[i] == nullptr) {
            sHostOwner[i] = t;
            sHost[i].thread = t;
            sHost[i].func = nullptr;
            sHost[i].param = nullptr;
            sHost[i].started = false;
            sHost[i].cancelled = false;
            sHost[i].intEnabled = true;
            return &sHost[i];
        }
    }
    port_fatal("too many OSThreads");
}

// ---------------------------------------------------------------------------
// Virtual CPU interrupt state
// ---------------------------------------------------------------------------
static bool sIntEnabled = false;  // MSR[EE] of the running thread
static bool sInIrq = false;       // running interrupt handlers
static thread_local bool tlsDeliveringIrqs;
static std::atomic<u32> sPendingIrq{0};
static std::mutex sIdleMutex;
static std::condition_variable sIdleCv;
static PortIrqHandler sIrqHandlers[PORT_IRQ_COUNT];

extern "C" void port_irq_set_handler(int irq, PortIrqHandler handler) { sIrqHandlers[irq] = handler; }

// ---------------------------------------------------------------------------
// GXV_AUDIO_HOLD=<ms> (read once at boot; 0 or unset: off; 1-12 on; larger values are taken as 12)
//
// Every guest thread runs on core 0, one at a time, so whenever the game's audio thread (JASAudioThread, priority 2) works, the
// game's main thread (priority 16) waits for it.  The audio thread is woken by the AI interrupt (a DAC block every 17.5 ms:
// JASDriver::updateDac, which starts the next DSP frame and renders its first sub-frame) and by the DSP interrupt of each of the
// frame's 7 sub-frames (JASDriver::updateDSP, the release that hands the sub-frame to the mixer thread; the 7th closes the frame):
// about 8 wake-ups a DAC block, 6.7 a game turn, each two host thread switches on core 0 plus the JAudio work.  An interrupt is
// taken at the main thread's next safe point, wherever that is, so all of it comes out of the main thread's turn.  Yet the main
// thread also sleeps several milliseconds a turn on the guest CPU (GXWaitDrawDone waiting for the front end, the retrace wait),
// with core 0 idle.
// With the switch on, while the main thread runs, an AI interrupt and a DSP interrupt that does not continue a DSP frame (the mail of
// a frame's last sub-frame: its handling only closes the frame or starts the next one) are held.  They are taken as soon as the main
// thread sleeps (by the idle loop or another guest thread, as any interrupt is), when it yields (OSYieldThread: JASDriver::waitSubFrame
// polls with it), or once they have been held for the limit (at the 4th safe point after it at the latest).  The DSP interrupts in the middle of a frame are
// never held, so a frame's sub-frames still follow each other at once: a DSP frame is normally rendered whole in one of the main
// thread's waits, on the core it leaves idle.
// Nothing else changes: the same handlers run in the same order on the guest CPU, only later, and the game already sees interrupt delays
// of this size and more (a safe point can be 10-20 ms away, see aiClock in platform/src/audio/ai_dsp.cpp).  The limit stays far inside
// the audio's deadlines: an AI interrupt has about 39 ms to be answered (then a block is played as silence), and the DSP frame started at
// an AI interrupt is read two DAC blocks (35 ms) later; at 12 ms the frame's start and its closing mail are each at most 12 ms late.
// Off, the delivery is exactly as before.  Off as well under the armhf/qemu harness's virtual clock, which does not advance while the
// guest computes.
// ---------------------------------------------------------------------------
static constexpr u32 kAudioIrqBits = (1u << PORT_IRQ_AI) | (1u << PORT_IRQ_DSP);
static constexpr long kAudioHoldMaxMs = 12;
static std::atomic<int> sAudioHoldMs{0};           // the limit; 0: off (set by port_os_boot before any guest thread runs)
static std::atomic<int64_t> sAudioIrqSince{0};     // when the oldest pending audio interrupt was raised (0: none, or not known yet)
static std::atomic<int> sAudioUrgent{1};           // a DSP frame has sub-frames left to render: its interrupts are not held
static std::atomic<bool> sAudioHoldBypass{false};  // OSYieldThread: the audio interrupts are taken at once
static std::atomic<unsigned> sAudioHoldPolls{0};   // (the clock is read at every 4th safe point that holds)
static std::atomic<bool> sAudioHolding{false};     // the main thread holds audio interrupts now (for the counts)
// For the audio log line (port_audio_hold_stats): audio interrupts taken while the main thread slept or by another guest thread,
// taken by the running main thread at the limit, taken by it at once (a frame's sub-frames, a yield), and holds begun.
static std::atomic<u32> sHoldElsewhere{0}, sHoldExpired{0}, sHoldAtOnce{0}, sHoldBegun{0};
enum IrqTaker { kIrqElsewhere, kIrqExpired, kIrqAtOnce, kIrqHold };

// platform/src/audio/ai_dsp.cpp, on the guest CPU: 1 while a DSP sync frame has sub-frames left to render, else 0.
extern "C" void port_audio_irq_urgent(int urgent) { sAudioUrgent.store(urgent ? 1 : 0, std::memory_order_relaxed); }

// For the audio log line: the counts since the last call. Returns the limit in ms (0: off, nothing is counted).
extern "C" int port_audio_hold_stats(unsigned* elsewhere, unsigned* expired, unsigned* atOnce, unsigned* begun) {
    *elsewhere = sHoldElsewhere.exchange(0, std::memory_order_relaxed);
    *expired = sHoldExpired.exchange(0, std::memory_order_relaxed);
    *atOnce = sHoldAtOnce.exchange(0, std::memory_order_relaxed);
    *begun = sHoldBegun.exchange(0, std::memory_order_relaxed);
    return sAudioHoldMs.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// GXV_AUDIO_CPU1=1 (read once at boot; default off): the game's audio thread on a second virtual CPU, on core 1
//
// The game's audio thread (JASAudioThread, priority 2) is woken about 6.7 times a turn by the AI and DSP interrupts; each run (JASDriver::
// updateDac/updateDSP: the port commands, the 64 DSP channels, the sequence tick) took the guest CPU, i.e. core 0, from the main thread
// (about 1.8 ms a turn in the lit Observatory, where core 0 limits the frame rate).  The DSP mixing itself is on core 1 already
// (GXV_AUDIO_THREAD=1).  With this switch the audio thread may also run on a second virtual CPU ("CPU1", the audio thread's host thread,
// placed on core 1) at the same time as the main thread, but only under these rules, which keep every interleaving one the Wii can produce:
//  - Windows.  CPU1 runs only while the main thread holds CPU0 inside a window: GameSystem::frameLoop opens one around calcAnim() and
//    around draw() (port_audio_window).  Those phases do not touch JAudio's state outside the game's own interrupt-disabled sections
//    (the JAI managers' calc/mixOut and the sound starts and stops, all in update(), are where the main thread drives JAudio).  As a
//    guard, the JAI and JASTrack entry points the main thread uses call port_audio_touch (port/audio_cpu1.h): in a window it closes the
//    window (waiting for a CPU1 run in progress) before they touch anything, and counts it.
//  - Interrupt-disabled sections are exclusive, as on one CPU.  CPU0 publishes a gate (sCpu0Gate: the main thread in a window with MSR[EE]
//    set); CPU1 claims a run with sCpu1Run and goes ahead only if the gate is open (store-load on both sides, seq_cst: the two never
//    both pass).  CPU0 closes the gate before every critical section, before running interrupt handlers and at a window's end, and
//    waits for a CPU1 run in progress to end (the run ends when the audio thread blocks again).  So a CPU1 run is atomic with respect to
//    everything CPU0 does with interrupts disabled, the scheduler included, exactly as the audio thread's run was atomic with respect to
//    the main thread on the Wii; and the main thread's code that overlaps a run touches nothing the run touches.
//  - The audio interrupts (AI, DSP) are taken by CPU1 itself: while the main thread is in a window CPU0 leaves them pending, and the audio
//    host thread, waiting for a dispatch, claims CPU1 when one is pending and the gate is open, runs the AI/DSP handlers (the same
//    handlers, in the same order), and runs the audio thread if they readied it; before it goes idle it takes the audio interrupts raised
//    meanwhile, so a DSP frame's sub-frames follow each other on core 1.  Outside the windows the audio interrupts are taken by CPU0 as
//    before (GXV_AUDIO_HOLD still holds a frame's start, here up to 12 ms, for the next window or sleep).  Every other interrupt is CPU0's.
//  - The scheduler.  CPU1 never switches threads: a thread its run readies (the stream loader) waits for CPU0's next safe point (a
//    pseudo-interrupt makes CPU0 look); when the audio thread blocks on CPU1 it goes back to waiting for a dispatch, from either CPU (it
//    can be resumed on CPU0 the usual way, e.g. when the main thread unlocks a mutex it waits for).  OSGetCurrentThread, the interrupt
//    state and the irq context are per CPU.
// Off (the default) nothing changes: the gate stays shut, the hooks are one test of a global.
// GXV_AUDIO_CPU1 set (to 0 or 1), or GXV_AUDIO_STATS=1, adds a log line every 5 s: the audio thread's time on each core, CPU0's waits
// for CPU1, and the late/underrun counts (platform/src/audio/ai_dsp.cpp).  Off as well under the armhf/qemu harness's virtual clock.
// ---------------------------------------------------------------------------
// (the basic log; weak so the scheduler's host tests that do not provide it still link)
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak));
static bool sCpu1Mode = false;                         // the switch (set by port_os_boot before any guest thread runs)
static bool sAudioStats = false;                       // the 5 s line's counts (GXV_AUDIO_CPU1 set, or GXV_AUDIO_STATS=1)
static std::atomic<OSThread*> sAudioThread{nullptr};   // JASAudioThread's OSThread (port_audio_thread_mark)
static std::atomic<HostThread*> sAudioHost{nullptr};   // its host side
static std::atomic<bool> sCpu0Gate{false};            // CPU1 may run: the main thread holds CPU0 in a window, with MSR[EE] set
static std::atomic<bool> sCpu1Run{false};             // a CPU1 run is in progress (claimed and ended by the audio host thread)
static std::atomic<OSThread*> sCpu1Thread{nullptr};    // the thread on CPU1 during a run (set and cleared by that thread)
static std::atomic<bool> sCpu1Waiting{false};         // the audio host thread waits for a dispatch (to be told of a CPU1 chance)
static bool sCpu1IntEnabled = false;                   // CPU1's MSR[EE] (the audio host thread only)
static bool sCpu1InIrq = false;                        // CPU1 runs interrupt handlers (the audio host thread only)
static bool sGateMirror = false;                       // CPU0's copy of sCpu0Gate (CPU0 only: no atomic read on its hot paths)
static int sWin = 0;                                   // CPU0: the main thread is in a window
// GXV_HOT_KNOBS (default 1; read by port_os_boot before any guest thread runs; 0: the old seq_cst stores and loads): CPU0's gate
// changes, which every critical section in a window makes (thousands a turn), use one full fence each where the seq_cst operations
// cost two or three barriers more. See cpu0GateClose / cpu0GateOpen for why each is still correct.
static bool sGateFences = false;
extern "C" {
int port_audio_window_flag = 0;  // the same for the JAudio guards (port/audio_cpu1.h); __atomic accesses
}
static constexpr u32 kReschedBit = 1u << 31;           // pseudo-interrupt: CPU1 readied a thread, CPU0 should look at its run queue
static_assert(PORT_IRQ_COUNT < 31, "kReschedBit");

// The 5 s line's counts. CPU0 time by who held it: 0 the main thread, 1 the audio thread while the main thread was ready (the time it
// took from the main thread), 2 other guest threads, 3 idle (+ interrupts), 4 the audio thread while the main thread slept.
static struct {
    std::atomic<int64_t> cpu0Ns[5], cpu1Ns{0}, waitNs{0};
    std::atomic<u32> cpu0AudioRuns{0}, cpu1Runs{0}, cpu1Batches{0}, waits{0}, barrierWaits{0}, windows{0}, breaches{0}, gateCloses{0};
} sAudioCpu;
static int sCpu0Class = 3;        // CPU0 (whoever holds it; ordered by the CPU handoffs)
static int64_t sCpu0Since = 0;
static int64_t sCpu1RunStart = 0;  // the audio host thread only

static inline bool onCpu1() {  // the calling host thread is running a CPU1 run (cheap when no run is in progress)
    if (!sCpu1Run.load(std::memory_order_relaxed)) return false;
    OSThread* t = sCpu1Thread.load(std::memory_order_relaxed);
    return t != nullptr && t == tlsSelf;
}
static inline int cpu0ClassOf(OSThread* t) {
    if (t == nullptr) return 3;
    if (t == &DefaultThread) return 0;
    if (t != sAudioThread.load(std::memory_order_relaxed)) return 2;
    return DefaultThread.state == OS_THREAD_STATE_READY ? 1 : 4;
}
static void cpu0Account(int cls) {  // CPU0 passes to a holder of class `cls`
    if (!sAudioStats) return;
    const int64_t now = port_host_time_ns();
    if (sCpu0Since) sAudioCpu.cpu0Ns[sCpu0Class].fetch_add(now - sCpu0Since, std::memory_order_relaxed);
    if ((cls == 1 || cls == 4) && sCpu0Class != cls) sAudioCpu.cpu0AudioRuns.fetch_add(1, std::memory_order_relaxed);
    sCpu0Since = now;
    sCpu0Class = cls;
}

static void cpu1Notify() {  // tells the audio host thread (waiting for a dispatch) to look again
    HostThread* h = sAudioHost.load(std::memory_order_acquire);
    if (!h) return;
    std::lock_guard<std::mutex> lk(sCpuMutex);
    h->cv.notify_one();
}

// CPU0: returns when no CPU1 run is in progress (the gate is shut, so none can begin). The main thread is alone on core 0 and a run
// lasts a fraction of a millisecond: it spins, and sleeps a little only if the run is long (the host's scheduling, a test machine).
static void cpu0AwaitCpu1(bool barrier) {
    if (!sCpu1Run.load(std::memory_order_seq_cst)) return;
    const int64_t t0 = port_host_time_ns();
    bool told = false;
    for (unsigned n = 1; sCpu1Run.load(std::memory_order_acquire); n++) {
        if ((n & 255) == 0 && port_host_time_ns() - t0 > 2000000) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            // A run lasts well under a millisecond. Half a second means CPU1 is stuck: say where, once (the basic log), for the hang report.
            if (!told && port_log_summary && port_host_time_ns() - t0 > 500000000) {
                told = true;
                OSThread* t = sCpu1Thread.load(std::memory_order_relaxed);
                port_log_summary("audio cpu1: STUCK: the game thread has waited %.0f ms for a run on core 1 (thread %p state %d, Reschedule %d, "
                                 "pending 0x%x, in irq %d)", (port_host_time_ns() - t0) / 1e6, (void*)t, t ? (int)t->state : -1, (int)Reschedule,
                                 sPendingIrq.load(std::memory_order_relaxed), (int)sCpu1InIrq);
            }
        }
#if !defined(__vita__)
        else if ((n & 15) == 0) std::this_thread::yield();
#endif
    }
    if (sAudioStats) {
        sAudioCpu.waitNs.fetch_add(port_host_time_ns() - t0, std::memory_order_relaxed);
        sAudioCpu.waits.fetch_add(1, std::memory_order_relaxed);
        if (barrier) sAudioCpu.barrierWaits.fetch_add(1, std::memory_order_relaxed);
    }
}
static inline void cpu0GateClose(bool barrier = false) {  // CPU0, before a critical section, the handlers, or a window's end
    if (sAudioStats) sAudioCpu.gateCloses.fetch_add(1, std::memory_order_relaxed);
    sGateMirror = false;
    if (sGateFences) {
        // (GXV_HOT_KNOBS) The store-load pair against cpu1Claim (sCpu1Run = 1, then the gate read, both seq_cst) needs only a seq_cst
        // FENCE between this store and cpu0AwaitCpu1's read of sCpu1Run ([atomics.order]: if the fence precedes the claim's gate read
        // in the single order S, that read sees this store, so the claim backs off; otherwise the claim's sCpu1Run store precedes the
        // fence in S and our read after it sees that store or a later one, so we wait for the run). The store needs no release: a CPU1
        // that reads this `false` does nothing with it but back off. Saved: the barrier before the store (`dmb; str; dmb` -> `str; dmb`).
        sCpu0Gate.store(false, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    } else {
        sCpu0Gate.store(false, std::memory_order_seq_cst);
    }
    cpu0AwaitCpu1(barrier);
}
static void cpu0GateOpen() {  // CPU0: the main thread in a window has just set MSR[EE]
    sGateMirror = true;
    if (sGateFences) {
        // (GXV_HOT_KNOBS) The store is a release (CPU1, reading `true` with its seq_cst load, sees the guest state CPU0 left), then one
        // seq_cst FENCE orders it before the two reads, which need no barrier of their own: cpu1Wait's sCpu1Waiting store and its
        // reads of sPendingIrq and the gate are seq_cst, as is port_irq_raise's fetch_or, so if the waiter's gate read precedes the
        // fence in S (it saw the gate shut), its sCpu1Waiting store and the pending bits it saw precede it too and the reads below
        // see them (and notify); otherwise its gate read sees this store. cpu1Notify takes sCpuMutex, the wait's own lock.
        // Saved: two barriers (`dmb; str; dmb; ldr; dmb; ldr; dmb` -> `dmb; str; dmb; ldr; ldr`).
        sCpu0Gate.store(true, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if ((sPendingIrq.load(std::memory_order_relaxed) & kAudioIrqBits) && sCpu1Waiting.load(std::memory_order_relaxed)) cpu1Notify();
        return;
    }
    sCpu0Gate.store(true, std::memory_order_seq_cst);
    if ((sPendingIrq.load(std::memory_order_seq_cst) & kAudioIrqBits) && sCpu1Waiting.load(std::memory_order_seq_cst)) cpu1Notify();
}
static inline bool cpu0MayOpen() { return sWin && __OSCurrentThread == &DefaultThread; }

// GameSystem::frameLoop (the main thread, MSR[EE] set): 1 before calcAnim() and draw(), 0 after them.
extern "C" void port_audio_window(int open) {
    if (!sCpu1Mode || onCpu1() || __OSCurrentThread != &DefaultThread) return;
    if (open) {
        if (sAudioHost.load(std::memory_order_relaxed) == nullptr) return;  // (the audio thread is not running yet)
        if (sWin) return;
        sWin = 1;
        __atomic_store_n(&port_audio_window_flag, 1, __ATOMIC_RELAXED);
        if (sAudioStats) sAudioCpu.windows.fetch_add(1, std::memory_order_relaxed);
        if (sIntEnabled) cpu0GateOpen();
    } else {
        if (!sWin) return;
        sWin = 0;
        __atomic_store_n(&port_audio_window_flag, 0, __ATOMIC_RELAXED);
        if (sGateMirror) cpu0GateClose(true);
    }
}

// The JAudio guard (port/audio_cpu1.h, only called while port_audio_window_flag is set): the main thread is about to drive JAudio inside a
// window. The window ends here (after a CPU1 run in progress); the next one opens at the next port_audio_window(1).
extern "C" void port_audio_touch(void) {
    if (!sCpu1Mode || onCpu1() || !sWin || __OSCurrentThread != &DefaultThread) return;
    if (sAudioStats) sAudioCpu.breaches.fetch_add(1, std::memory_order_relaxed);
    port_audio_window(0);
}

// For the 5 s line (platform/src/audio/ai_dsp.cpp): ns[0..4] CPU0 (the main thread, the audio thread taking it from the ready main
// thread, other guest threads, idle, the audio thread while the main thread slept), ns[5] CPU1 (the audio thread), ns[6] CPU0 waiting
// for CPU1; n[0] audio runs on CPU0, n[1] on CPU1, n[2] audio interrupt batches taken on CPU1, n[3] CPU0 waits, n[4] of them at a
// window's end, n[5] windows, n[6] windows the JAudio guard closed, n[7] critical sections that shut the gate.
// The counts since the last call. Returns -1 when not counting, else the switch (0 or 1; 2: on, but the game's audio thread has not
// started, so nothing can run on CPU1).
extern "C" int port_audio_cpu_stats(int64_t ns[7], unsigned n[8]) {
    if (!sAudioStats) return -1;
    for (int i = 0; i < 5; i++) ns[i] = sAudioCpu.cpu0Ns[i].exchange(0, std::memory_order_relaxed);
    ns[5] = sAudioCpu.cpu1Ns.exchange(0, std::memory_order_relaxed);
    ns[6] = sAudioCpu.waitNs.exchange(0, std::memory_order_relaxed);
    n[0] = sAudioCpu.cpu0AudioRuns.exchange(0, std::memory_order_relaxed);
    n[1] = sAudioCpu.cpu1Runs.exchange(0, std::memory_order_relaxed);
    n[2] = sAudioCpu.cpu1Batches.exchange(0, std::memory_order_relaxed);
    n[3] = sAudioCpu.waits.exchange(0, std::memory_order_relaxed);
    n[4] = sAudioCpu.barrierWaits.exchange(0, std::memory_order_relaxed);
    n[5] = sAudioCpu.windows.exchange(0, std::memory_order_relaxed);
    n[6] = sAudioCpu.breaches.exchange(0, std::memory_order_relaxed);
    n[7] = sAudioCpu.gateCloses.exchange(0, std::memory_order_relaxed);
    return sCpu1Mode ? (sAudioHost.load(std::memory_order_relaxed) ? 1 : 2) : 0;
}

extern "C" void port_irq_raise(int irq) {
    sPendingIrq.fetch_or(1u << irq);
    if (((1u << irq) & kAudioIrqBits) && sAudioHoldMs.load(std::memory_order_relaxed) > 0 && sAudioIrqSince.load(std::memory_order_relaxed) == 0) {
        int64_t none = 0;
        sAudioIrqSince.compare_exchange_strong(none, port_host_time_ns(), std::memory_order_relaxed);
    }
    // (GXV_AUDIO_CPU1) the audio host thread waits for a dispatch: it may take this one on CPU1 (store-load against its own)
    if (((1u << irq) & kAudioIrqBits) && sCpu1Mode && sCpu1Waiting.load(std::memory_order_seq_cst)) cpu1Notify();
    std::lock_guard<std::mutex> lk(sIdleMutex);
    sIdleCv.notify_all();
}

// `keep`: the interrupts left pending (GXV_AUDIO_HOLD), `taker`: who takes them (for the counts).
static void runIrqHandlers(u32 keep = 0, IrqTaker taker = kIrqElsewhere) {
    // Called with the CPU held.  Handlers run with interrupts disabled and
    // may not switch threads (SelectThread checks sInIrq).
    bool savedEn = sIntEnabled;
    if (sGateMirror) cpu0GateClose();  // (GXV_AUDIO_CPU1) the handlers are a critical section: after a CPU1 run in progress
    sInIrq = true;
    sIntEnabled = false;
    if (!keep && sAudioHoldMs.load(std::memory_order_relaxed) > 0 && (sPendingIrq.load(std::memory_order_relaxed) & kAudioIrqBits)) {
        // (GXV_AUDIO_HOLD) cleared before the snapshot: a raise in between leaves an older time than its own, never a later one
        sAudioIrqSince.store(0, std::memory_order_relaxed);
        const bool held = sAudioHolding.exchange(false, std::memory_order_relaxed);
        if (taker == kIrqExpired) sHoldExpired.fetch_add(1, std::memory_order_relaxed);
        else if (taker == kIrqAtOnce) sHoldAtOnce.fetch_add(1, std::memory_order_relaxed);
        else if (held) sHoldElsewhere.fetch_add(1, std::memory_order_relaxed);  // (a hold that ended in a wait, not the audio thread's own raises)
    }
    // One atomic snapshot per safe point. A slow native display can receive
    // another VI while blocked; draining fresh bits forever starves game code.
    const u32 bits = keep ? (sPendingIrq.fetch_and(keep) & ~keep) : sPendingIrq.exchange(0);
    for (int i = 0; i < PORT_IRQ_COUNT; i++) {
        if ((bits & (1u << i)) && sIrqHandlers[i]) {
#if defined(__vita__)
            port_game_progress_irq_checkpoint(i,1);
#endif
            sIrqHandlers[i]();
#if defined(__vita__)
            port_game_progress_irq_checkpoint(i,0);
#endif
        }
    }
    sIntEnabled = savedEn;
    sInIrq = false;
    // (GXV_AUDIO_CPU1: the gate stays shut; the caller still reads the scheduler's state. deliverIrqs opens it again.)
}

extern "C" OSThread* SelectThread(BOOL yield);

// PETARI_GAPLOG=<ms>: a diagnostic for the sound.  The audio thread only runs when the thread holding the CPU reaches a safe point
// (this function), so a long stretch of work between two of them is a pause in the music.  Every stretch of at least <ms> is logged
// with the code addresses of the safe point that began it and the one that ended it (offsets from port_irq_poll, to be looked up in
// the ELF's symbol table with scripts/gap-report.py): the work in between is in or under the functions those two sites belong to.
// (The Vita's unwinder returns no frames, so there is no stack.)  Off unless the variable is set; the normal cost is one test of a global.
static constexpr bool kGapDiagnostics = PORT_RUNTIME_DIAGNOSTICS && PORT_LOGGING;
static int64_t sGapNs = kGapDiagnostics ? -1 : 0;  // -1: unread, 0: off
static int64_t sGapLast = 0;
static unsigned sGapLogged = 0;
static void* sGapSite;      // the caller of the safe point being passed
static void* sGapPrevSite;  // the caller of the one before it

// PETARI_GAPPROF=1: a flat profile of the guest CPU by stretch.  Every stretch between two safe points is added to its (begin, end)
// pair of sites; every 5 s the 30 pairs with the most time are logged ("prof:" lines, offsets from port_irq_poll; scripts/prof-report.py
// looks them up) and the table is cleared, so the lines show what each part of a run (start-up, the title load, a stage change) spent
// its time on.  Time the guest sleeps (the loader waiting for a disc read) lands on the pair that ends when it wakes.  No log lines for
// single stretches and no host-thread CPU query (that costs ~4 ms a frame); the cost is one clock read and a hash insert per safe point.
static bool sGapProf = false;
struct GapProfSlot {
    void* from;
    void* to;
    int64_t ns;
    uint32_t n;
};
static GapProfSlot sGapProfTab[2048];
static unsigned sGapProfUsed;
static int64_t sGapProfStart;
static int64_t sGapProfTotal;
static unsigned sGapProfLost;

static void gapProfDump(int64_t now) {
    const uintptr_t base = (uintptr_t)&port_irq_poll;
    GapProfSlot* top[30] = {};
    unsigned nTop = 0;
    for (GapProfSlot& g : sGapProfTab) {
        if (!g.n) continue;
        unsigned i = nTop < 30 ? nTop++ : 29;
        if (i == 29 && top[29] && top[29]->ns >= g.ns) continue;
        top[i] = &g;
        while (i > 0 && top[i - 1]->ns < top[i]->ns) {
            GapProfSlot* t = top[i];
            top[i] = top[i - 1];
            top[i - 1] = t;
            i--;
        }
    }
    port_log("prof: window %.1f s, %.0f ms in %u pairs (%u events not counted), port_irq_poll %p", (now - sGapProfStart) / 1e9, sGapProfTotal / 1e6, sGapProfUsed,
             sGapProfLost, (void*)base);
    for (unsigned i = 0; i < nTop; i++) {
        port_log("prof: %.1f ms %ux from %ld to %ld", top[i]->ns / 1e6, top[i]->n, (long)((uintptr_t)top[i]->from - base), (long)((uintptr_t)top[i]->to - base));
    }
    memset(sGapProfTab, 0, sizeof(sGapProfTab));
    sGapProfUsed = 0;
    sGapProfTotal = 0;
    sGapProfLost = 0;
    sGapProfStart = port_host_time_ns();
}

static void gapProfAdd(void* from, void* to, int64_t ns) {
    uintptr_t h = ((uintptr_t)from * 2654435761u) ^ ((uintptr_t)to * 40503u);
    for (unsigned probe = 0; probe < 16; probe++) {
        GapProfSlot& g = sGapProfTab[(h + probe) & 2047];
        if (g.n == 0) {
            g.from = from;
            g.to = to;
            sGapProfUsed++;
        } else if (g.from != from || g.to != to) {
            continue;
        }
        g.ns += ns;
        g.n++;
        sGapProfTotal += ns;
        return;
    }
    sGapProfLost++;
}

// PETARI_GAPLOG on the Vita: the CPU time the host thread holding the guest CPU used between two safe points, to tell work (the
// thread's own CPU time is about the whole stretch) from waiting or being pre-empted (it is far less).
#if defined(__vita__)
static uint64_t sGapCpuLast;
static int sGapCpuTid = -1;
static uint64_t gapHostCpuUs(int* tid) {
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    *tid = sceKernelGetThreadId();
    if (sceKernelGetThreadInfo(*tid, &info) != 0) return 0;
    return (uint64_t)info.runClocks;
}
#endif

static void gapNoteSlow() {
    if (!kGapDiagnostics) return;
    if (sGapNs < 0) {
        const char* e = getenv("PETARI_GAPLOG");
        sGapNs = e ? (int64_t)atoi(e) * 1000000 : 0;
        sGapLast = port_host_time_ns();
        const char* pr = getenv("PETARI_GAPPROF");
        if (pr && atoi(pr) > 0) {
            sGapProf = true;
            sGapProfStart = sGapLast;
            if (sGapNs == 0) {
                sGapNs = (int64_t)1 << 60;  // on, but no single stretch is long enough to log
            }
        }
        if (sGapNs == 0) {
            return;
        }
    }
    if (tlsSelf == nullptr) {
        return;
    }
    const int64_t now = port_host_time_ns();
    const int64_t gap = now - sGapLast;
    sGapLast = now;
    if (sGapProf) {
        gapProfAdd(sGapPrevSite, sGapSite, gap);
        if (now - sGapProfStart >= 5000000000ll) {
            gapProfDump(now);
            sGapLast = port_host_time_ns();  // (the logging is not part of the next stretch)
        }
        sGapPrevSite = sGapSite;
        return;
    }
#if defined(__vita__)
    int cpuTid = -1;
    const uint64_t cpuNow = gapHostCpuUs(&cpuTid);
    const bool sameThread = cpuTid == sGapCpuTid;
    const double cpuMs = sameThread ? (double)(cpuNow - sGapCpuLast) / 1e3 : -1.0;
    sGapCpuLast = cpuNow;
    sGapCpuTid = cpuTid;
#else
    const double cpuMs = -1.0;
#endif
    if (gap >= sGapNs && sGapLogged < 600) {
        sGapLogged++;
        const uintptr_t base = (uintptr_t)&port_irq_poll;
        port_log("gap: %.1f ms without a safe point, thread %p, from %ld to %ld (offsets from port_irq_poll %p) host thread cpu %.1f ms%s", gap / 1e6,
                 (void*)tlsSelf, (long)((uintptr_t)sGapPrevSite - base), (long)((uintptr_t)sGapSite - base), (void*)base, cpuMs,
                 cpuMs < 0.0 ? " (a different host thread began the stretch)" : "");
        sGapLast = port_host_time_ns();  // (the logging is not part of the next stretch)
    }
    sGapPrevSite = sGapSite;
}

// GXV_AUDIO_HOLD: the main thread, running, has audio interrupts pending at a safe point. Hold them (kIrqHold) or take them now
// (kIrqAtOnce: a DSP frame in progress, or a yield; kIrqExpired: held for the limit). Plain globals and relaxed atomics only, no
// thread_local (see deliverIrqs); the clock is read at every 4th safe point that holds (a syscall on the Vita).
static IrqTaker audioHold() {
    if (sAudioUrgent.load(std::memory_order_relaxed) || sAudioHoldBypass.load(std::memory_order_relaxed)) return kIrqAtOnce;
    if (!sAudioHolding.load(std::memory_order_relaxed)) {
        sAudioHolding.store(true, std::memory_order_relaxed);
        sHoldBegun.fetch_add(1, std::memory_order_relaxed);
    }
    const unsigned polls = sAudioHoldPolls.load(std::memory_order_relaxed);
    sAudioHoldPolls.store(polls + 1, std::memory_order_relaxed);
    if (polls & 3u) return kIrqHold;
    const int64_t now = port_host_time_ns();
    if (now <= 0) return kIrqExpired;  // (no clock: never hold blind)
    int64_t since = sAudioIrqSince.load(std::memory_order_relaxed);
    if (since == 0) {  // (raised between the bit and its time: from now)
        sAudioIrqSince.compare_exchange_strong(since, now, std::memory_order_relaxed);
        return kIrqHold;
    }
    return now - since >= sAudioHoldMs.load(std::memory_order_relaxed) * 1000000ll ? kIrqExpired : kIrqHold;
}

static void deliverIrqs() {
    if (__builtin_expect(sCpu1Mode, 0) && onCpu1()) return;  // (GXV_AUDIO_CPU1: CPU1 takes its interrupts before it goes idle)
    if (kGapDiagnostics && __builtin_expect(sGapNs != 0, 0)) {
        gapNoteSlow();
    }
    // The plain globals first: nearly always nothing is pending, and a thread_local read costs a call (emulated
    // thread-local storage on the Vita: __emutls_get_address, pthread_getspecific) on every interrupt-state change.
    const u32 pending = sPendingIrq.load(std::memory_order_relaxed);
    if (pending == 0 || sInIrq || !sIntEnabled) {
        return;
    }
    // GXV_AUDIO_HOLD: the main thread running holds the audio interrupts that can wait for its next sleep (see audioHold); any
    // other interrupt pending with them is taken now. (__OSCurrentThread is the guest thread holding the CPU; a host thread
    // that is not a guest thread returns below, as before.)
    u32 keep = 0;
    IrqTaker taker = kIrqElsewhere;
    if ((pending & kAudioIrqBits) && sGateMirror) {
        // (GXV_AUDIO_CPU1) the main thread in a window: the audio interrupts are left to CPU1 (see port_audio_window)
        if (!(pending & ~kAudioIrqBits)) return;
        keep = kAudioIrqBits;
    } else if ((pending & kAudioIrqBits) && sAudioHoldMs.load(std::memory_order_relaxed) > 0 && __OSCurrentThread == &DefaultThread) {
        taker = audioHold();
        if (taker == kIrqHold) {
            if (!(pending & ~kAudioIrqBits)) return;
            keep = kAudioIrqBits;
        }
    }
    if (tlsDeliveringIrqs || tlsSelf == nullptr) {
        return;
    }
    // Keep this caller's post-IRQ scheduling from recursively draining fresh
    // work. Another guest selected below has its own guard and can take IRQs.
    tlsDeliveringIrqs=true;
    runIrqHandlers(keep, taker);
    // Returning from an exception: switch if a handler readied a better thread.
    if (RunQueueHint) {
        BOOL en = OSDisableInterrupts();
        SelectThread(FALSE);
        OSRestoreInterrupts(en);
    }
    if (__builtin_expect(sCpu1Mode, 0) && !sGateMirror && sIntEnabled && cpu0MayOpen()) cpu0GateOpen();  // (shut by runIrqHandlers)
    tlsDeliveringIrqs=false;
}

// (for the diagnostics of other files under PETARI_GAPLOG: how long ago was the last safe point, and the threshold; 0 when the log is off)
extern "C" int64_t port_gap_age_ns(void) { return kGapDiagnostics && sGapNs > 0 ? port_host_time_ns() - sGapLast : 0; }
extern "C" int64_t port_gap_threshold_ns(void) { return kGapDiagnostics && sGapNs > 0 ? sGapNs : 0; }

extern "C" void port_irq_poll(void) {
    if (kGapDiagnostics && sGapNs && !(sCpu1Mode && onCpu1())) sGapSite = __builtin_return_address(0);
    deliverIrqs();
}
// (the same, for a caller that wants its own caller named: operator new, so a stretch ends at the code that allocated)
extern "C" void port_irq_poll_at(void* site) {
    if (kGapDiagnostics && sGapNs && !(sCpu1Mode && onCpu1())) sGapSite = site;
    deliverIrqs();
}

extern "C" {

// (GXV_AUDIO_CPU1) On CPU1 these only change CPU1's own MSR[EE]: the run already excludes CPU0's critical sections, and CPU1 takes no
// interrupts at its safe points.  On CPU0, in a window, disabling shuts the gate (waiting for a CPU1 run in progress) and enabling opens it.
BOOL OSDisableInterrupts(void) {
    if (__builtin_expect(sCpu1Mode, 0)) {
        if (onCpu1()) {
            const BOOL p = sCpu1IntEnabled;
            sCpu1IntEnabled = false;
            return p;
        }
    }
    BOOL prev = sIntEnabled;
    if (prev) {
        if (kGapDiagnostics && sGapNs) sGapSite = __builtin_return_address(0);
        deliverIrqs();  // a pending interrupt would have been taken before this point
    }
    // (GXV_AUDIO_CPU1) The gate is shut AFTER the interrupts are taken: deliverIrqs re-opens it on its way out (for the safe points,
    // where MSR[EE] stays set). Shut before it, as in r58-r60, a pending interrupt re-opened the gate just before MSR[EE] was cleared, so
    // the main thread entered this critical section with the gate open and CPU1 could run alongside it (the r60 load hang, see
    // tests/os_audio_cpu1_load.cpp).
    if (__builtin_expect(sCpu1Mode, 0) && sGateMirror) cpu0GateClose();
    sIntEnabled = false;
    return prev;
}

BOOL OSEnableInterrupts(void) {
    if (__builtin_expect(sCpu1Mode, 0) && onCpu1()) {
        const BOOL p = sCpu1IntEnabled;
        sCpu1IntEnabled = true;
        return p;
    }
    BOOL prev = sIntEnabled;
    if (kGapDiagnostics && sGapNs) sGapSite = __builtin_return_address(0);
    sIntEnabled = true;
    if (__builtin_expect(sCpu1Mode, 0) && !sGateMirror && cpu0MayOpen()) cpu0GateOpen();
    deliverIrqs();
    return prev;
}

BOOL OSRestoreInterrupts(BOOL level) {
    if (__builtin_expect(sCpu1Mode, 0) && onCpu1()) {
        const BOOL p = sCpu1IntEnabled;
        sCpu1IntEnabled = level ? true : false;
        return p;
    }
    BOOL prev = sIntEnabled;
    if (__builtin_expect(sCpu1Mode, 0) && !level && sGateMirror) cpu0GateClose();  // (a restore that disables)
    sIntEnabled = level ? true : false;
    if (sIntEnabled && !prev) {
        if (__builtin_expect(sCpu1Mode, 0) && cpu0MayOpen()) cpu0GateOpen();
        if (kGapDiagnostics && sGapNs) sGapSite = __builtin_return_address(0);
        deliverIrqs();
    }
    return prev;
}

}  // extern "C"

// ---------------------------------------------------------------------------
// Context switching
// ---------------------------------------------------------------------------
static void* hostThreadMain(void* arg);

static void startHostThread(OSThread* t, HostThread* h) {
    size_t stackSize = h->stackSize ? h->stackSize : (2u << 20);
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) port_fatal("pthread_attr_init failed");
#if defined(__vita__)
    // VitaSDK creates its own kernel stack; supplying an address only duplicated
    // the allocation. Preserve the native stack size, not the unused copy.
    if (pthread_attr_setstacksize(&attr, stackSize) != 0) port_fatal("pthread_attr_setstacksize failed");
#else
    void* stack = port_low_alloc(stackSize);
    if (pthread_attr_setstack(&attr, stack, stackSize) != 0) port_fatal("pthread_attr_setstack failed");
#endif
    pthread_t pt;
    if (pthread_create(&pt, &attr, hostThreadMain, t) != 0) {
        port_fatal("pthread_create failed");
    }
    pthread_attr_destroy(&attr);
    pthread_detach(pt);
    h->started = true;
}

static bool cpu1Wait(std::unique_lock<std::mutex>& lk, OSThread* self, HostThread* h);  // (GXV_AUDIO_CPU1, below)

// Waits (with sCpuMutex held via lk) until `self` owns the CPU again.  Returns true when the thread was given CPU1 instead
// (GXV_AUDIO_CPU1: only the audio thread).
static bool waitForCpu(std::unique_lock<std::mutex>& lk, OSThread* self, HostThread* h) {
    if (sCpu1Mode && h == sAudioHost.load(std::memory_order_relaxed)) {
        return cpu1Wait(lk, self, h);
    }
    h->cv.wait(lk, [&] { return sCpuOwner == self || h->cancelled; });
    if (h->cancelled && sCpuOwner != self) {
        lk.unlock();
        pthread_exit(nullptr);
    }
    return false;
}

// After waitForCpu: the per-CPU state of the CPU the thread got.
static void resumeAfterWait(OSThread* self, HostThread* h, bool cpu1) {
    if (cpu1) {
        sCpu1IntEnabled = h->intEnabled;  // (sCpu1Thread and the run were set up by the claim)
        return;
    }
    sIntEnabled = h->intEnabled;
    __OSCurrentThread = self;
    sCurrentContext = &self->context;
}

// Hands the CPU from the calling host thread to `next`.  Returns when the
// caller is scheduled again (immediately if next == caller), or never if the
// caller is exiting.
static void switchTo(OSThread* next, bool callerExiting) {
    OSThread* self = tlsSelf;
    cpu0Account(cpu0ClassOf(next));  // (the 5 s audio line only)
    __OSCurrentThread = next;
#if defined(__vita__)
    port_game_progress_cpu_checkpoint(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(next)));
#endif
    sCurrentContext = &next->context;
    if (next == self) {
        return;
    }
    HostThread* hn = hostOf(next);
    HostThread* hs = (self && !callerExiting) ? hostOf(self) : nullptr;
    std::unique_lock<std::mutex> lk(sCpuMutex);
    if (hs) {
        hs->intEnabled = sIntEnabled;
    }
    sCpuOwner = next;
    if (!hn->started) {
        startHostThread(next, hn);
    } else {
        hn->cv.notify_one();
    }
    if (callerExiting || hs == nullptr) {
        return;
    }
    const bool cpu1 = waitForCpu(lk, self, hs);
    lk.unlock();
    resumeAfterWait(self, hs, cpu1);
}

static void* hostThreadMain(void* arg) {
    OSThread* self = (OSThread*)arg;
    tlsSelf = self;
    HostThread* h = hostOf(self);
    h->pt = pthread_self();
    h->hasPt = true;
    PORT_QEMU_OSTHREAD(h->func, h->param);
    {
        std::unique_lock<std::mutex> lk(sCpuMutex);
        waitForCpu(lk, self, h);
    }
#if defined(__vita__)
    void* stack = nullptr; uint32_t stackSize = 0;
    const int result = port_vita_thread_stack(&stack, &stackSize);
    if (result < 0 || stackSize < h->stackSize) port_fatal("Vita native thread stack query failed: %08x size %u", (unsigned)result, stackSize);
    self->stackBase = (u8*)stack + stackSize;
    self->stackEnd = (u32*)stack;
    const int pinResult = port_vita_pin_core(0);  // only one guest thread runs at a time
    port_thread_class(3);
    if (PORT_RUNTIME_DIAGNOSTICS && PORT_LOGGING) {
        // Match the native CPU census to a guest entry symbol instead of guessing
        // which identically named pthread competes with game-main.
        port_log("thread-map: uid=0x%08x guest=%p entry=%p pin_rc=0x%08x stack=%p+%u anchor=%p",
                 static_cast<unsigned>(sceKernelGetThreadId()), static_cast<void*>(self),
                 reinterpret_cast<void*>(h->func), static_cast<unsigned>(pinResult), stack, stackSize,
                 reinterpret_cast<void*>(&hostThreadMain));
    }
#endif
    __OSCurrentThread = self;
    sCurrentContext = &self->context;
    sIntEnabled = true;  // new threads start with MSR[EE] set
    void* ret = h->func(h->param);
    OSExitThread(ret);
    return nullptr;
}

// Runs interrupt handlers while no thread is runnable.
static void idleUntilRunnable() {
    while (RunQueueBits == 0) {
        if (sCpu1Mode) cpu0AwaitCpu1(false);  // (GXV_AUDIO_CPU1: none can be in progress here, the gate is shut; for safety)
        if (port_vclock_enabled()) {
            // armhf/qemu harness: every OSThread is blocked, so virtual time may pass (port/vclock.h)
            port_vclock_idle_step([]() -> int { return sPendingIrq.load() != 0; });
            if (sPendingIrq.load() == 0) {
                std::unique_lock<std::mutex> lk(sIdleMutex);
                sIdleCv.wait_for(lk, std::chrono::microseconds(200), [] { return sPendingIrq.load() != 0; });
            }
            if (sPendingIrq.load() != 0) runIrqHandlers();
            continue;
        }
        {
            std::unique_lock<std::mutex> lk(sIdleMutex);
            sIdleCv.wait(lk, [] { return sPendingIrq.load() != 0; });
        }
        if (kGapDiagnostics && sGapNs > 0) {
            sGapLast = port_host_time_ns();  // (waiting is not a stretch of work)
        }
        runIrqHandlers();
    }
}

// ---------------------------------------------------------------------------
// SDK scheduler logic
// ---------------------------------------------------------------------------
static void SetRun(OSThread* thread) {
    thread->queue = &RunQueue[thread->priority];
    EnqueueTail(thread->queue, thread, link);
    RunQueueBits |= 1u << (31 - thread->priority);
    RunQueueHint = TRUE;
}

static void UnsetRun(OSThread* thread) {
    OSThreadQueue* queue = thread->queue;
    DequeueItem(queue, thread, link);
    if (queue->head == 0) {
        RunQueueBits &= ~(1u << (31 - thread->priority));
    }
    thread->queue = 0;
}

// ---------------------------------------------------------------------------
// GXV_AUDIO_CPU1: the second virtual CPU (see the comment at port_audio_window)
// ---------------------------------------------------------------------------
static bool sExiting;  // set by OSExitThread for the calling thread's switch

enum { kCpu1Got, kCpu1Done, kCpu1Shut, kCpu1Busy };

// CPU1 takes the audio interrupts raised so far (the AI and DSP handlers, in CPU0's order, with CPU1's MSR[EE] off). False: none.
static bool cpu1TakeAudioIrqs() {
    const u32 bits = sPendingIrq.fetch_and(~kAudioIrqBits) & kAudioIrqBits;
    if (!bits) return false;
    if (sAudioHoldMs.load(std::memory_order_relaxed) > 0) {  // (GXV_AUDIO_HOLD's bookkeeping, as runIrqHandlers)
        sAudioIrqSince.store(0, std::memory_order_relaxed);
        sAudioHolding.store(false, std::memory_order_relaxed);
    }
    if (sAudioStats) sAudioCpu.cpu1Batches.fetch_add(1, std::memory_order_relaxed);
    const bool savedEn = sCpu1IntEnabled;
    sCpu1InIrq = true;
    sCpu1IntEnabled = false;
    for (int i = 0; i < PORT_IRQ_COUNT; i++) {
        if ((bits & (1u << i)) && sIrqHandlers[i]) sIrqHandlers[i]();
    }
    sCpu1IntEnabled = savedEn;
    sCpu1InIrq = false;
    return true;
}

// The handlers readied the thread: it keeps (or gets) CPU1, as on the Wii it would have run before the main thread got the CPU back.
static bool cpu1Readied(OSThread* self) {
    if (self->state != OS_THREAD_STATE_READY || IsSuspended(self->suspend) || self->queue != &RunQueue[self->priority]) return false;
    UnsetRun(self);
    self->state = OS_THREAD_STATE_RUNNING;
    return true;
}

// Ends the CPU1 run (the audio host thread): from here CPU0 may enter its critical sections again.
static void cpu1Finish() {
    // A thread this run readied that would pre-empt the main thread: CPU0 looks at its run queue at its next safe point.
    const bool resched = RunQueueBits != 0 && (OSPriority)__cntlzw(RunQueueBits) < DefaultThread.priority;
    if (sAudioStats) {
        sAudioCpu.cpu1Ns.fetch_add(port_host_time_ns() - sCpu1RunStart, std::memory_order_relaxed);
        sAudioCpu.cpu1Runs.fetch_add(1, std::memory_order_relaxed);
    }
    sCpu1Thread.store(nullptr, std::memory_order_relaxed);
    sCpu1Run.store(false, std::memory_order_seq_cst);
    if (resched) port_irq_raise(31);  // (kReschedBit: no handler; deliverIrqs then reschedules)
}

// A chance seen by the waiting audio host thread: claim CPU1, take the audio interrupts, and keep CPU1 if they readied the thread.
static int cpu1Claim(OSThread* self) {
    sCpu1Run.store(true, std::memory_order_seq_cst);
    if (!sCpu0Gate.load(std::memory_order_seq_cst)) {  // CPU0 is in a critical section (or the window ended): it opens the gate again
        sCpu1Run.store(false, std::memory_order_seq_cst);
        return kCpu1Shut;
    }
    // Until sCpu1Run is cleared CPU0 is outside its critical sections and cannot enter one: the guest OS state is CPU1's.
    if (Reschedule > 0) {  // (the main thread has the scheduler disabled: nothing may be switched in)
        sCpu1Run.store(false, std::memory_order_seq_cst);
        return kCpu1Busy;
    }
    sCpu1Thread.store(self, std::memory_order_relaxed);
    if (sAudioStats) sCpu1RunStart = port_host_time_ns();
    cpu1TakeAudioIrqs();
    if (cpu1Readied(self)) return kCpu1Got;
    cpu1Finish();
    return kCpu1Done;
}

// waitForCpu for the audio thread: a dispatch on CPU0 (sCpuOwner, as for any thread) or a chance on CPU1 (an audio interrupt is pending
// and the gate is open: port_irq_raise and cpu0GateOpen tell it, store-load against sCpu1Waiting).
static bool cpu1Wait(std::unique_lock<std::mutex>& lk, OSThread* self, HostThread* h) {
    const auto chance = [] {
        return (sPendingIrq.load(std::memory_order_seq_cst) & kAudioIrqBits) != 0 && sCpu0Gate.load(std::memory_order_seq_cst);
    };
    for (;;) {
        sCpu1Waiting.store(true, std::memory_order_seq_cst);
        h->cv.wait(lk, [&] { return sCpuOwner == self || h->cancelled || chance(); });
        sCpu1Waiting.store(false, std::memory_order_seq_cst);
        if (sCpuOwner == self) return false;
        if (h->cancelled) {
            lk.unlock();
            pthread_exit(nullptr);
        }
        lk.unlock();
        const int r = cpu1Claim(self);
        lk.lock();
        if (r == kCpu1Got) return true;
        if (r == kCpu1Busy) h->cv.wait_for(lk, std::chrono::microseconds(200), [&] { return sCpuOwner == self || h->cancelled; });
    }
}

// SelectThread on CPU1: no switch while the thread runs (a better thread waits for CPU0); once it blocks, the interrupts raised during
// the run are taken first (unless CPU0 waits to enter a critical section), then CPU1 goes idle and the thread waits for its next
// dispatch, on either CPU.
static OSThread* cpu1Select() {
    // (No Reschedule test here: CPU1 never switches, it only goes idle when its thread blocks. A run cannot see Reschedule > 0 (it is
    // changed only inside CPU0's critical sections, and a claim refuses while it is set); were it to, returning 0 here would leave the
    // blocked thread looping in OSReceiveMessage on core 1 for ever, the r60 hang's second half.)
    if (sCpu1InIrq) return 0;
    OSThread* self = tlsSelf;
    if (self->state == OS_THREAD_STATE_RUNNING) return 0;
    const bool exiting = sExiting;
    sExiting = false;
    // (not while CPU0 waits at the gate: it gets in first, the interrupts wait for the next claim. A run so lasts one wake-up's work.)
    if (!exiting) {
        while (sCpu0Gate.load(std::memory_order_seq_cst) && cpu1TakeAudioIrqs()) {
            if (cpu1Readied(self)) return self;
        }
    }
    HostThread* h = sAudioHost.load(std::memory_order_relaxed);
    h->intEnabled = sCpu1IntEnabled;
    cpu1Finish();
    if (exiting) {  // (OSExitThread ends the host thread)
        sAudioHost.store(nullptr, std::memory_order_relaxed);
        return self;
    }
    std::unique_lock<std::mutex> lk(sCpuMutex);
    const bool cpu1 = waitForCpu(lk, self, h);
    lk.unlock();
    resumeAfterWait(self, h, cpu1);
    return self;
}

// JASAudioThread::run, first thing, on the audio thread (CPU0): this is the thread CPU1 may run.
// vita/runtime/audio.cpp: moves the calling thread to core 1 (GXV_AUDIO_THREAD_CORE) at priority 0x4F; returns its core mask after the
// change (< 0: refused) and its priority in *priority.
extern "C" int __attribute__((weak)) port_vita_audio_cpu1_boost(int* priority);
static const char* sCpu1Off = "GXV_AUDIO_CPU1 is not set";  // (port_os_boot: why the switch is off)
extern "C" void port_audio_thread_mark(void) {
    OSThread* t = __OSCurrentThread;
    if (t == nullptr || t == &DefaultThread || t != tlsSelf || sAudioThread.load(std::memory_order_relaxed)) return;
    sAudioThread.store(t, std::memory_order_release);
    sAudioHost.store(hostOf(t), std::memory_order_release);
    // The one start-up line saying whether GXV_AUDIO_CPU1 is in effect (the basic log: port_log does not reach the console's log.txt in a
    // build without runtime diagnostics, which is why r58 showed nothing).
    int priority = -1;
    const int mask = sCpu1Mode && port_vita_audio_cpu1_boost ? port_vita_audio_cpu1_boost(&priority) : -1;
    if (!port_log_summary) {
    } else if (sCpu1Mode) {
        port_log_summary("audio cpu1: ON (GXV_AUDIO_CPU1=1): the game's audio thread runs on a second virtual CPU while the game thread is in "
                         "calcAnim()/draw(); its host thread: core mask 0x%X, priority 0x%X%s; frame-start hold %d ms; \"audio cpu:\" every 5 s",
                         static_cast<unsigned>(mask), static_cast<unsigned>(priority),
                         !port_vita_audio_cpu1_boost ? " (no core placement on this platform)"
                         : mask < 0 ? " (NOT moved: it shares core 0 with the game thread, no gain)"
                         : (mask & 0x10000) ? " (core 0 included: little gain)" : "",
                         sAudioHoldMs.load(std::memory_order_relaxed));
    } else {
        port_log_summary("audio cpu1: off (%s)%s", sCpu1Off, sAudioStats ? "; \"audio cpu:\" every 5 s (the game thread's audio cost)" : "");
    }
}

extern "C" OSPriority __OSGetEffectivePriority(OSThread* thread) {
    OSPriority priority = thread->base;
    for (OSMutex* mutex = thread->queueMutex.head; mutex; mutex = mutex->link.next) {
        OSThread* blocked = mutex->queue.head;
        if (blocked != 0 && blocked->priority < priority) {
            priority = blocked->priority;
        }
    }
    return priority;
}

static OSThread* SetEffectivePriority(OSThread* thread, OSPriority priority) {
    switch (thread->state) {
    case OS_THREAD_STATE_READY:
        UnsetRun(thread);
        thread->priority = priority;
        SetRun(thread);
        break;
    case OS_THREAD_STATE_WAITING:
        DequeueItem(thread->queue, thread, link);
        thread->priority = priority;
        EnqueuePrio(thread->queue, thread, link);
        if (thread->mutex != 0) {
            return thread->mutex->thread;
        }
        break;
    case OS_THREAD_STATE_RUNNING:
        RunQueueHint = TRUE;
        thread->priority = priority;
        break;
    }
    return NULL;
}

static void UpdatePriority(OSThread* thread) {
    do {
        if (IsSuspended(thread->suspend)) {
            break;
        }
        OSPriority priority = __OSGetEffectivePriority(thread);
        if (thread->priority == priority) {
            break;
        }
        thread = SetEffectivePriority(thread, priority);
    } while (thread);
}

extern "C" void __OSPromoteThread(OSThread* thread, OSPriority priority) {
    do {
        if ((thread->suspend > 0) || thread->priority <= priority) {
            break;
        }
        thread = SetEffectivePriority(thread, priority);
    } while (thread);
}


extern "C" OSThread* SelectThread(BOOL yield) {
    if (__builtin_expect(sCpu1Mode, 0) && onCpu1()) {
        return cpu1Select();
    }
    if (Reschedule > 0 || sInIrq) {
        return 0;
    }
    OSThread* currentThread = __OSCurrentThread;
    if (currentThread != 0) {
        if (currentThread->state == OS_THREAD_STATE_RUNNING) {
            if (!yield) {
                OSPriority priority = __cntlzw(RunQueueBits);
                if (currentThread->priority <= priority) {
                    return 0;
                }
            }
            currentThread->state = OS_THREAD_STATE_READY;
            SetRun(currentThread);
        }
    }

    if (RunQueueBits == 0) {
        cpu0Account(3);  // (the 5 s audio line only)
        __OSCurrentThread = 0;
#if defined(__vita__)
        port_game_progress_cpu_checkpoint(0);
#endif
        sCurrentContext = &IdleContext;
        idleUntilRunnable();
    }

    RunQueueHint = FALSE;
    OSPriority priority = __cntlzw(RunQueueBits);
    OSThreadQueue* queue = &RunQueue[priority];
    OSThread* nextThread;
    DequeueHead(queue, nextThread, link);
    if (queue->head == 0) {
        RunQueueBits &= ~(1u << (31 - priority));
    }
    nextThread->queue = 0;
    nextThread->state = OS_THREAD_STATE_RUNNING;

    bool exiting = sExiting;
    sExiting = false;
    switchTo(nextThread, exiting);
    return nextThread;
}

static void __OSReschedule(void) {
    if (RunQueueHint) {
        SelectThread(FALSE);
    }
}

// ---------------------------------------------------------------------------
// Public thread API
// ---------------------------------------------------------------------------
extern "C" {

void OSInitThreadQueue(OSThreadQueue* queue) { queue->head = queue->tail = 0; }

OSThread* OSGetCurrentThread(void) {
    if (__builtin_expect(sCpu1Mode, 0) && onCpu1()) return tlsSelf;  // (GXV_AUDIO_CPU1: per CPU)
    return __OSCurrentThread;
}

BOOL OSIsThreadSuspended(OSThread* thread) { return (0 < thread->suspend) ? TRUE : FALSE; }

BOOL OSIsThreadTerminated(OSThread* thread) { return ((thread->state == OS_THREAD_STATE_MORIBUND) || (thread->state == 0)) ? TRUE : FALSE; }

static BOOL __OSIsThreadActive(OSThread* thread) {
    if (thread->state == 0) {
        return FALSE;
    }
    for (OSThread* active = __OSActiveThreadQueue.head; active; active = active->linkActive.next) {
        if (thread == active) {
            return TRUE;
        }
    }
    return FALSE;
}

s32 OSDisableScheduler(void) {
    BOOL enabled = OSDisableInterrupts();
    s32 count = Reschedule++;
    OSRestoreInterrupts(enabled);
    return count;
}

s32 OSEnableScheduler(void) {
    BOOL enabled = OSDisableInterrupts();
    s32 count = Reschedule--;
    OSRestoreInterrupts(enabled);
    return count;
}

// GXV_LC_YIELD (logiccut, 6 Nov; default 0): a yield that SelectThread(TRUE) would answer with the caller itself is finished here.
// MR::ProhibitSchedulerAndInterrupts yields on the way out, and DisplayListMaker::diff takes one per diffed material (hundreds a
// drawn turn); nearly always nothing of the main thread's priority or better is ready. SelectThread would then put the caller on the
// run queue, take it off again and call switchTo, which reads the emulated thread-local tlsSelf (a call into the TLS emulation on
// the console) before it finds next == self. The state left behind is the same as that path's: the caller RUNNING with queue 0 and
// both links 0, its priority's queue and RunQueueBits as before (it was alone in that queue), RunQueueHint FALSE,
// __OSCurrentThread and sCurrentContext the caller's, and the same accounting calls. The interrupts the yield takes
// (OSDisableInterrupts above, with the audio hold bypassed) are taken first, as before, so a thread they ready is seen.
static bool lcYieldFast() {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_LC_YIELD"); return e && *e ? atoi(e) : 0; }()) != 0;
}
static uint32_t sLcYieldCount, sLcYieldKept;  // (CPU0's guest threads only: one at a time)
static bool yieldKeepsCaller() {
    if (Reschedule > 0 || sInIrq || sExiting) return false;  // (SelectThread's own early answers: left to it)
    OSThread* const self = __OSCurrentThread;
    if (self == 0 || self->state != OS_THREAD_STATE_RUNNING) return false;
    if (RunQueueBits != 0 && (OSPriority)__cntlzw(RunQueueBits) <= self->priority) return false;  // a thread to switch to
    // (what SetRun, DequeueHead and switchTo(self) leave)
    self->link.prev = NULL;
    self->link.next = NULL;
    self->queue = 0;
    RunQueueHint = FALSE;
    cpu0Account(cpu0ClassOf(self));
#if defined(__vita__)
    port_game_progress_cpu_checkpoint(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)));
#endif
    sCurrentContext = &self->context;
    return true;
}

void OSYieldThread(void) {
    if (__builtin_expect(sCpu1Mode, 0) && onCpu1()) return;  // (GXV_AUDIO_CPU1: nothing else runs on CPU1)
    // (GXV_AUDIO_HOLD) a yield takes held audio interrupts: JASDriver::waitSubFrame yields until the audio thread has run a sub-frame
    sAudioHoldBypass.store(true, std::memory_order_relaxed);
    BOOL enabled = OSDisableInterrupts();
    sAudioHoldBypass.store(false, std::memory_order_relaxed);
    if (!lcYieldFast()) {
        SelectThread(TRUE);
    } else {
        if (yieldKeepsCaller()) {
            sLcYieldKept++;
        } else {
            SelectThread(TRUE);
        }
        if ((++sLcYieldCount & 0xFFFFu) == 0 && port_log_summary) {
            port_log_summary("logiccut: yield: %u of the last 65536 OSYieldThread calls kept the caller without SelectThread (GXV_LC_YIELD=1)",
                             sLcYieldKept);
            sLcYieldKept = 0;
        }
    }
    OSRestoreInterrupts(enabled);
}

BOOL OSCreateThread(OSThread* thread, void* (*func)(void*), void* param, void* stack, u32 stackSize, OSPriority priority, u16 attr) {
    if (priority < 0 || 31 < priority) {
        return FALSE;
    }
    thread->state = OS_THREAD_STATE_READY;
    thread->attr = (u16)(attr & 1);
    thread->priority = thread->base = priority;
    thread->suspend = 1;
    thread->value = (void*)-1;
    thread->mutex = NULL;
    OSInitThreadQueue(&thread->queueJoin);
    thread->queueMutex.head = thread->queueMutex.tail = NULL;
    memset(&thread->context, 0, sizeof(thread->context));
    thread->stackBase = (u8*)stack;
    thread->stackEnd = (u32*)((u8*)stack - stackSize);
    if (stack != NULL && stackSize >= 4) {
        *(thread->stackEnd) = 0xDEADBABE;
    }
    thread->error = 0;
    thread->specific[0] = thread->specific[1] = 0;

    HostThread* h = hostOf(thread);
    h->func = func;
    h->param = param;
    h->started = false;
    h->cancelled = false;
    h->intEnabled = true;
    // Host code (GPU decode, audio mixing) runs on game threads too, so give
    // every thread a generous host stack regardless of the game's request.
    h->stackSize = (size_t)(stackSize > 0x8000 ? 4u << 20 : 2u << 20);

    BOOL enabled = OSDisableInterrupts();
    EnqueueTail(&__OSActiveThreadQueue, thread, linkActive);
    OSRestoreInterrupts(enabled);
    return TRUE;
}

void OSExitThread(void* val) {
    BOOL enabled = OSDisableInterrupts();
    OSThread* currentThread = OSGetCurrentThread();
    if (currentThread->attr & 1) {
        DequeueItem(&__OSActiveThreadQueue, currentThread, linkActive);
        currentThread->state = 0;
    } else {
        currentThread->state = OS_THREAD_STATE_MORIBUND;
        currentThread->value = val;
    }
    __OSUnlockAllMutex(currentThread);
    OSWakeupThread(&currentThread->queueJoin);
    RunQueueHint = TRUE;
    // Release the host slot while we still own the CPU; after the switch below
    // another thread runs and may reuse it.
    if (currentThread == sAudioThread.load(std::memory_order_relaxed)) {
        if (!onCpu1()) sAudioHost.store(nullptr, std::memory_order_relaxed);  // (on CPU1, cpu1Select still needs it: cleared after)
        sAudioThread.store(nullptr, std::memory_order_relaxed);
    }
    for (int i = 0; i < kMaxThreads; i++) {
        if (sHostOwner[i] == currentThread) {
            sHostOwner[i] = nullptr;
        }
    }
    sExiting = true;
    SelectThread(FALSE);
    // The host thread for an exited OSThread ends here.
    (void)enabled;
    pthread_exit(nullptr);
}

void OSCancelThread(OSThread* thread) {
    BOOL enabled = OSDisableInterrupts();
    switch (thread->state) {
    case OS_THREAD_STATE_READY:
        if (!IsSuspended(thread->suspend)) {
            UnsetRun(thread);
        }
        break;
    case OS_THREAD_STATE_RUNNING:
        RunQueueHint = TRUE;
        break;
    case OS_THREAD_STATE_WAITING:
        DequeueItem(thread->queue, thread, link);
        thread->queue = NULL;
        if (!IsSuspended(thread->suspend) && thread->mutex) {
            UpdatePriority(thread->mutex->thread);
        }
        break;
    default:
        OSRestoreInterrupts(enabled);
        return;
    }
    if (thread->attr & 1) {
        DequeueItem(&__OSActiveThreadQueue, thread, linkActive);
        thread->state = 0;
    } else {
        thread->state = OS_THREAD_STATE_MORIBUND;
    }
    __OSUnlockAllMutex(thread);
    OSWakeupThread(&thread->queueJoin);

    if (thread == sAudioThread.load(std::memory_order_relaxed) && !onCpu1()) {  // (GXV_AUDIO_CPU1: forget it; its slot may be reused)
        sAudioThread.store(nullptr, std::memory_order_relaxed);
        sAudioHost.store(nullptr, std::memory_order_relaxed);
    }
    if (thread == OSGetCurrentThread()) {
        for (int i = 0; i < kMaxThreads; i++) {
            if (sHostOwner[i] == thread) {
                sHostOwner[i] = nullptr;
            }
        }
        sExiting = true;
        SelectThread(FALSE);
        pthread_exit(nullptr);
    }
    {
        // Release the host thread parked waiting for the CPU.
        HostThread* h = hostOf(thread);
        std::lock_guard<std::mutex> lk(sCpuMutex);
        h->cancelled = true;
        h->cv.notify_one();
        for (int i = 0; i < kMaxThreads; i++) {
            if (sHostOwner[i] == thread) {
                sHostOwner[i] = nullptr;
            }
        }
    }
    __OSReschedule();
    OSRestoreInterrupts(enabled);
}

BOOL OSJoinThread(OSThread* thread, void** val) {
    BOOL enabled = OSDisableInterrupts();
    if (!(thread->attr & 1) && thread->state != OS_THREAD_STATE_MORIBUND && thread->queueJoin.head == NULL) {
        OSSleepThread(&thread->queueJoin);
        if (!__OSIsThreadActive(thread)) {
            OSRestoreInterrupts(enabled);
            return FALSE;
        }
    }
    if (((volatile OSThread*)thread)->state == OS_THREAD_STATE_MORIBUND) {
        if (val) {
            *val = thread->value;
        }
        DequeueItem(&__OSActiveThreadQueue, thread, linkActive);
        thread->state = 0;
        OSRestoreInterrupts(enabled);
        return TRUE;
    }
    OSRestoreInterrupts(enabled);
    return FALSE;
}

void OSDetachThread(OSThread* thread) {
    BOOL enabled = OSDisableInterrupts();
    thread->attr |= 1;
    if (thread->state == OS_THREAD_STATE_MORIBUND) {
        DequeueItem(&__OSActiveThreadQueue, thread, linkActive);
        thread->state = 0;
    }
    OSWakeupThread(&thread->queueJoin);
    OSRestoreInterrupts(enabled);
}

s32 OSResumeThread(OSThread* thread) {
    BOOL enabled = OSDisableInterrupts();
    s32 suspendCount = thread->suspend--;
    if (thread->suspend < 0) {
        thread->suspend = 0;
    } else if (thread->suspend == 0) {
        switch (thread->state) {
        case OS_THREAD_STATE_READY:
            thread->priority = __OSGetEffectivePriority(thread);
            SetRun(thread);
            break;
        case OS_THREAD_STATE_WAITING:
            DequeueItem(thread->queue, thread, link);
            thread->priority = __OSGetEffectivePriority(thread);
            EnqueuePrio(thread->queue, thread, link);
            if (thread->mutex) {
                UpdatePriority(thread->mutex->thread);
            }
            break;
        }
        __OSReschedule();
    }
    OSRestoreInterrupts(enabled);
    return suspendCount;
}

s32 OSSuspendThread(OSThread* thread) {
    BOOL enabled = OSDisableInterrupts();
    s32 suspendCount = thread->suspend++;
    if (suspendCount == 0) {
        switch (thread->state) {
        case OS_THREAD_STATE_RUNNING:
            RunQueueHint = TRUE;
            thread->state = OS_THREAD_STATE_READY;
            break;
        case OS_THREAD_STATE_READY:
            UnsetRun(thread);
            break;
        case OS_THREAD_STATE_WAITING:
            DequeueItem(thread->queue, thread, link);
            thread->priority = 32;
            EnqueueTail(thread->queue, thread, link);
            if (thread->mutex) {
                UpdatePriority(thread->mutex->thread);
            }
            break;
        }
        __OSReschedule();
    }
    OSRestoreInterrupts(enabled);
    return suspendCount;
}

void OSSleepThread(OSThreadQueue* queue) {
    BOOL enabled = OSDisableInterrupts();
    OSThread* currentThread = OSGetCurrentThread();
    currentThread->state = OS_THREAD_STATE_WAITING;
    currentThread->queue = queue;
    EnqueuePrio(queue, currentThread, link);
    RunQueueHint = TRUE;
    __OSReschedule();
    OSRestoreInterrupts(enabled);
}

void OSWakeupThread(OSThreadQueue* queue) {
    BOOL enabled = OSDisableInterrupts();
    while (queue->head) {
        OSThread* thread;
        DequeueHead(queue, thread, link);
        thread->state = OS_THREAD_STATE_READY;
        if (!IsSuspended(thread->suspend)) {
            SetRun(thread);
        }
    }
    __OSReschedule();
    OSRestoreInterrupts(enabled);
}

BOOL OSSetThreadPriority(OSThread* thread, OSPriority priority) {
    if (priority < 0 || priority > 31) {
        return FALSE;
    }
    BOOL enabled = OSDisableInterrupts();
    if (thread->base != priority) {
        thread->base = priority;
        UpdatePriority(thread);
        __OSReschedule();
    }
    OSRestoreInterrupts(enabled);
    return TRUE;
}

OSPriority OSGetThreadPriority(OSThread* thread) { return thread->base; }

static void SleepAlarmHandler(OSAlarm* alarm, OSContext*) { OSResumeThread((OSThread*)OSGetAlarmUserData(alarm)); }

void OSSleepTicks(OSTime tick) {
    BOOL enabled = OSDisableInterrupts();
    OSThread* current = OSGetCurrentThread();
    if (current == NULL) {
        OSRestoreInterrupts(enabled);
        return;
    }
    OSAlarm sleepAlarm;
    OSCreateAlarm(&sleepAlarm);
    OSSetAlarmTag(&sleepAlarm, (u32)(uintptr_t)current);
    OSSetAlarmUserData(&sleepAlarm, (void*)current);
    OSSetAlarm(&sleepAlarm, tick, SleepAlarmHandler);
    OSSuspendThread(current);
    OSCancelAlarm(&sleepAlarm);
    OSRestoreInterrupts(enabled);
}

// ---------------------------------------------------------------------------
// Contexts (register state is meaningless on the host; keep the bookkeeping)
// ---------------------------------------------------------------------------
OSContext* OSGetCurrentContext(void) {
    if (__builtin_expect(sCpu1Mode, 0) && onCpu1()) return &tlsSelf->context;  // (GXV_AUDIO_CPU1: per CPU)
    return sCurrentContext ? sCurrentContext : &IdleContext;
}
void OSSetCurrentContext(OSContext* context) { sCurrentContext = context; }
void OSClearContext(OSContext* context) { (void)context; }
void OSFillFPUContext(OSContext* context) { (void)context; }
u32 OSGetStackPointer(void) { return (u32)(uintptr_t)__builtin_frame_address(0); }

// SDK interrupt controller hooks: the GX library registers PE handlers here.
static __OSInterruptHandler sSdkHandlers[32];
__OSInterruptHandler __OSSetInterruptHandler(__OSInterrupt interrupt, __OSInterruptHandler handler) {
    __OSInterruptHandler old = sSdkHandlers[interrupt & 31];
    sSdkHandlers[interrupt & 31] = handler;
    return old;
}
__OSInterruptHandler __OSGetInterruptHandler(s32 interrupt) { return sSdkHandlers[interrupt & 31]; }
OSInterruptMask __OSMaskInterrupts(OSInterruptMask mask) { return mask; }
OSInterruptMask __OSUnmaskInterrupts(OSInterruptMask mask) { return mask; }

}  // extern "C"

extern "C" void port_call_sdk_interrupt(int interrupt) {
    __OSInterruptHandler h = sSdkHandlers[interrupt & 31];
    if (h) {
        h((__OSInterrupt)interrupt, OSGetCurrentContext());
    }
}

// ---------------------------------------------------------------------------
// Heap routing (see port/heap_routing.h)
// ---------------------------------------------------------------------------
extern "C" int port_is_game_thread(void) { return tlsSelf != nullptr; }
extern "C" int port_owns_game_cpu(void) {
    if (!tlsSelf) return 0;
    if (sCpu1Mode && onCpu1()) return 1;  // (GXV_AUDIO_CPU1: the audio thread holds CPU1)
    std::lock_guard<std::mutex> lock(sCpuMutex);
    return sCpuOwner==tlsSelf;
}
extern "C" int port_use_game_heap(void) { return tlsSelf != nullptr && tlsHostAllocDepth == 0; }
// GXV_LC_MUTEX_FAST=1 (logiccut, 6 Nov; default 0 = as before): OSLockMutex / OSUnlockMutex / OSTryLockMutex (OSMutex.c) skip their
// diagnostic check that a guest thread is the caller (port_is_game_thread: an emulated thread-local read, a call into the TLS
// emulation on the console, once per lock and once per unlock). The check only logs, at most 4 lines a run; the mutex works the same.
extern "C" int port_lc_mutex_fast(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_LC_MUTEX_FAST"); return e && *e ? atoi(e) : 0; }());
}
extern "C" void port_host_alloc_scope_enter(void) { tlsHostAllocDepth++; }
extern "C" void port_host_alloc_scope_leave(void) { tlsHostAllocDepth--; }
extern "C" int port_is_game_heap_ptr(const void* p) { return port_is_game_memory(p); }

// GXV_HEAP_GUARD (port/heap_routing.h): a fixed table (no heap, nothing to construct, so registration works from any static
// initialiser); the check is for the game thread at a scene's destruction.
namespace {
struct HeapGuardEntry { const char* name; PortHeapGuardFn fn; void* context; };
constexpr unsigned kHeapGuardMax = 48;
HeapGuardEntry sHeapGuard[kHeapGuardMax];
std::atomic<unsigned> sHeapGuardCount{0};
bool heapGuardOn() {
    static const bool on = [] { const char* e = getenv("GXV_HEAP_GUARD"); return e && *e && *e != '0'; }();
    return on;
}
}  // namespace
extern "C" void port_heap_guard_register(const char* name, PortHeapGuardFn fn, void* context) {
    const unsigned i = sHeapGuardCount.fetch_add(1, std::memory_order_relaxed);
    if (i < kHeapGuardMax) sHeapGuard[i] = HeapGuardEntry{name, fn, context};  // (read only after start-up: no further synchronisation)
}
extern "C" void port_heap_guard_check(const char* when) {
    if (!heapGuardOn()) return;
    const unsigned n = sHeapGuardCount.load(std::memory_order_relaxed);
    unsigned bad = 0;
    for (unsigned i = 0; i < n && i < kHeapGuardMax; ++i) {
        const void* p = sHeapGuard[i].fn(sHeapGuard[i].context);
        if (p != nullptr && port_is_game_heap_ptr(p)) {
            ++bad;
            if (port_log_summary) port_log_summary("heap guard (%s): %s holds %p, inside a game heap: it will point into freed memory", when, sHeapGuard[i].name, p);
        }
    }
    if (port_log_summary) port_log_summary("heap guard (%s): %u registered storage(s) checked, %u in a game heap%s", when, n < kHeapGuardMax ? n : kHeapGuardMax, bad,
                                           n > kHeapGuardMax ? " (table full: some are not checked)" : "");
}
// The C heap's state, for the growth log and the exhaustion report. Vita: newlib's arena (a
// fixed block). Desktop (glibc, where the harness sets mmap_threshold=0, so every block is mapped):
// the heap plus the mapped blocks.
struct PortHeapStats { unsigned arenaKiB = 0, inUseKiB = 0, freeKiB = 0, pieces = 0, topKiB = 0; };
static PortHeapStats portHeapStats() {
    PortHeapStats r;
#if defined(__vita__)
    const struct mallinfo mi = mallinfo();
    r.arenaKiB = static_cast<unsigned>(mi.arena / 1024u);
    r.inUseKiB = static_cast<unsigned>(mi.uordblks / 1024u);
    r.freeKiB = static_cast<unsigned>(mi.fordblks / 1024u);
    r.pieces = static_cast<unsigned>(mi.ordblks);
    r.topKiB = static_cast<unsigned>(mi.keepcost / 1024u);
#elif defined(__GLIBC__)
    const struct mallinfo2 mi = mallinfo2();
    r.arenaKiB = static_cast<unsigned>((mi.arena + mi.hblkhd) / 1024u);
    r.inUseKiB = static_cast<unsigned>((mi.uordblks + mi.hblkhd) / 1024u);
    r.freeKiB = static_cast<unsigned>(mi.fordblks / 1024u);
    r.pieces = static_cast<unsigned>(mi.ordblks);
    r.topKiB = static_cast<unsigned>(mi.keepcost / 1024u);
#endif
    return r;
}

// Large host allocations are where the C heap's growth shows: the first time the heap's
// arena has grown by another 4 MiB, say so (with the request that did it).
static void portHeapMark(size_t request) {
    static std::atomic<unsigned> markKiB{0};
    static thread_local bool busy = false;
    if (busy) return;
    busy = true;
    const PortHeapStats hs = portHeapStats();
    unsigned prev = markKiB.load();
    if (hs.arenaKiB >= prev + 4096u && markKiB.compare_exchange_strong(prev, hs.arenaKiB))
        port_log_summary("host heap: arena %u KiB, in use %u KiB (a %lu KiB request)", hs.arenaKiB, hs.inUseKiB, static_cast<unsigned long>(request / 1024u));
    busy = false;
}

#if !defined(__vita__) && defined(__GLIBC__)
// GXV_HEAP_SITES=1 (desktop only, slow): which call sites hold the game's host `new` memory. Every
// allocation of 4 KiB or more records its call chain; port_host_heap_log prints the sites that
// hold the most. Addresses resolve with addr2line on build/desktop/game (the image is not PIE).
#include <execinfo.h>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>
namespace {
struct HeapSite { size_t live = 0, count = 0; void* pc[8] = {}; int depth = 0; };
std::mutex sSiteMtx;
std::map<std::vector<void*>, HeapSite>* sSites;
std::unordered_map<void*, std::pair<size_t, const std::vector<void*>*>>* sSiteOf;
bool siteTracking() {
    static const bool on = [] { const char* e = getenv("GXV_HEAP_SITES"); return e && *e && *e != '0'; }();
    return on;
}
// (the containers below allocate through the game's `new`, which comes back here: not while the table is being changed)
thread_local bool sSiteBusy = false;
struct SiteGuard {
    bool ok;
    SiteGuard() : ok(!sSiteBusy) { if (ok) { sSiteBusy = true; ++tlsHostAllocDepth; } }  // (on a game thread the tables are the host's, not a JKR heap's)
    ~SiteGuard() { if (ok) { --tlsHostAllocDepth; sSiteBusy = false; } }
};
void siteAlloc(void* p, size_t n) {
    SiteGuard guard;
    if (!guard.ok) return;
    void* bt[10];
    const int d = backtrace(bt, 10);
    std::lock_guard<std::mutex> lk(sSiteMtx);
    if (!sSites) { sSites = new std::map<std::vector<void*>, HeapSite>(); sSiteOf = new std::unordered_map<void*, std::pair<size_t, const std::vector<void*>*>>(); }
    std::vector<void*> key(bt + 2, bt + d);   // (without this function and port_host_alloc)
    auto it = sSites->emplace(key, HeapSite{}).first;
    it->second.live += n; ++it->second.count;
    (*sSiteOf)[p] = {n, &it->first};
}
void siteFree(void* p) {
    SiteGuard guard;
    if (!guard.ok) return;
    std::lock_guard<std::mutex> lk(sSiteMtx);
    if (!sSiteOf) return;
    auto it = sSiteOf->find(p);
    if (it == sSiteOf->end()) return;
    auto s = sSites->find(*it->second.second);
    if (s != sSites->end()) { s->second.live -= it->second.first; --s->second.count; }
    sSiteOf->erase(it);
}
void siteDump(const char* when) {
    SiteGuard guard;
    if (!guard.ok) return;
    std::lock_guard<std::mutex> lk(sSiteMtx);
    if (!sSites) return;
    std::vector<std::pair<size_t, const std::vector<void*>*>> top;
    size_t total = 0;
    for (auto& kv : *sSites) { top.emplace_back(kv.second.live, &kv.first); total += kv.second.live; }
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
    port_log("heap sites %s: %lu KiB held by allocations of 4 KiB or more, in %lu call chains; the biggest:", when, static_cast<unsigned long>(total / 1024), static_cast<unsigned long>(top.size()));
    for (size_t i = 0; i < top.size() && i < 14 && top[i].first >= 64 * 1024; ++i) {
        const auto& c = *top[i].second;
        char line[600];
        int n = snprintf(line, sizeof line, "  %lu KiB (%lu blocks):", static_cast<unsigned long>(top[i].first / 1024), static_cast<unsigned long>((*sSites)[c].count));
        for (size_t f = 0; f < c.size() && f < 6 && n < 560; ++f) n += snprintf(line + n, sizeof line - n, " %p", c[f]);
        port_log("%s", line);
    }
}
}
#endif

// Bytes the game's `new` has taken from the host heap and not given back (std containers of the
// renderer, platform bookkeeping: what is left of the C heap's use once the libc's, vitaGL's and
// the shader compiler's own blocks are taken away).
static std::atomic<size_t> sHostNewLive{0};
extern "C" unsigned port_host_new_live_kib(void) { return static_cast<unsigned>(sHostNewLive.load(std::memory_order_relaxed) / 1024u); }

extern "C" void port_host_heap_log(const char* when) {
    const PortHeapStats hs = portHeapStats();
    port_log("host heap %s: arena %u KiB, in use %u KiB, free %u KiB in %u pieces; the game's `new` holds %u KiB", when, hs.arenaKiB, hs.inUseKiB, hs.freeKiB,
             hs.pieces, port_host_new_live_kib());
#if !defined(__vita__) && defined(__GLIBC__)
    if (siteTracking()) siteDump(when);
#endif
}

// One line in the player's basic log: how full the C heap is (written every 30 s with the frame-rate lines).
extern "C" void port_host_heap_summary(void) {
    const PortHeapStats hs = portHeapStats();
    port_log_summary("memory: C heap in use %u KiB (arena %u KiB, free %u KiB in %u pieces); the game's `new` holds %u KiB", hs.inUseKiB, hs.arenaKiB,
                     hs.freeKiB, hs.pieces, port_host_new_live_kib());
}

extern "C" void port_host_alloc_failed(size_t size, int align) {
    const PortHeapStats hs = portHeapStats();
#if defined(__vita__)
    // (the player's basic log as well: the full log is off in player builds, and this is the last line before the abort)
    port_log_summary("FATAL: host heap exhausted: a request for %lu KiB (alignment %d) found no room; C heap arena %u KiB, in use %u KiB, free %u KiB in %u pieces, "
             "releasable at the top %u KiB; the game's `new` holds %u KiB",
             static_cast<unsigned long>(size / 1024u), align, hs.arenaKiB, hs.inUseKiB, hs.freeKiB, hs.pieces, hs.topKiB, port_host_new_live_kib());
    port_log_flush();
#else
    fprintf(stderr, "FATAL: host heap exhausted: a request for %zu KiB (alignment %d) found no room (in use %u KiB)\n", size / 1024u, align, hs.inUseKiB);
    fflush(stderr);
#endif
    abort();
}

extern "C" void* port_host_alloc(size_t size, int align) {
    if (align < 16) {
        align = 16;
    }
#if defined(__vita__)
    if (align & (align - 1)) return nullptr;
    void* p = memalign((size_t)align, size ? size : 1);
    if (p) sHostNewLive.fetch_add(malloc_usable_size(p), std::memory_order_relaxed);
    if (p && size >= 256u * 1024u) portHeapMark(size);
    return p;
#else
    void* p = nullptr;
    if (posix_memalign(&p, (size_t)align, size ? size : 1) != 0) {
        return nullptr;
    }
    sHostNewLive.fetch_add(malloc_usable_size(p), std::memory_order_relaxed);
    if (size >= 4096u && siteTracking()) siteAlloc(p, malloc_usable_size(p));
    if (size >= 256u * 1024u) portHeapMark(size);
    return p;
#endif
}
extern "C" void port_host_free(void* p) {
    if (p) sHostNewLive.fetch_sub(malloc_usable_size(p), std::memory_order_relaxed);
#if !defined(__vita__) && defined(__GLIBC__)
    if (p && siteTracking()) siteFree(p);
#endif
    free(p);
}

// ---------------------------------------------------------------------------
// Diagnostics: emulated thread states and native backtraces
// ---------------------------------------------------------------------------
#if !defined(__vita__)
static std::atomic<int> sBtDone;
static uintptr_t sBtPcs[40];
static int sBtCount;

static _Unwind_Reason_Code btCallback(struct _Unwind_Context* ctx, void*) {
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && sBtCount < 40) {
        sBtPcs[sBtCount++] = pc;
    }
    return _URC_NO_REASON;
}

static void btSignal(int) {
    sBtCount = 0;
    _Unwind_Backtrace(btCallback, nullptr);
    sBtDone.store(1);
}
#endif

extern "C" void port_debug_dump_threads(void) {
#if !defined(__vita__)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = btSignal;
    sigaction(SIGURG, &sa, nullptr);
#else
    // Vita pthreads cannot deliver foreign-thread signals. Preserve scheduler
    // state diagnostics without pretending an unsupported backtrace succeeded.
    port_log("threads: Vita cross-thread native backtraces unavailable (no pthread signal delivery)");
#endif
    port_log("threads: cpu owner %p, pending irqs %08x, int enabled %d", (void*)sCpuOwner, sPendingIrq.load(), (int)sIntEnabled);
    for (int i = 0; i < kMaxThreads; i++) {
        OSThread* t = sHostOwner[i];
        if (!t) {
            continue;
        }
        port_log("thread %p prio %d/%d state %d suspend %d queue %p mutex %p%s", (void*)t, t->priority, t->base, t->state, t->suspend,
                 (void*)t->queue, (void*)t->mutex, t == sCpuOwner ? " [CPU]" : "");
#if !defined(__vita__)
        HostThread& h = sHost[i];
        if (!h.hasPt) {
            continue;
        }
        sBtDone.store(0);
        if (pthread_kill(h.pt, SIGURG) != 0) {
            continue;
        }
        for (int w = 0; w < 200 && !sBtDone.load(); w++) {
            usleep(1000);
        }
        char line[1024];
        int n = 0;
        for (int k = 0; k < sBtCount && n < 1000; k++) {
            uintptr_t pc = sBtPcs[k];
            if (pc >= 0x98000000u && pc < 0xA0000000u) {
                n += snprintf(line + n, sizeof(line) - n, " g:%lx", (unsigned long)(pc - 0x98000000u));
            } else {
                n += snprintf(line + n, sizeof(line) - n, " %lx", (unsigned long)pc);
            }
        }
        port_log("  bt:%s", line);
#endif
    }
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
static void (*sBootEntry)(void);

static void* defaultThreadMain(void*) {
    OSThread* thread = &DefaultThread;
    tlsSelf = thread;
    hostOf(thread)->pt = pthread_self();
    hostOf(thread)->hasPt = true;
    PORT_QEMU_THREAD("game-main");
#if defined(__vita__)
    const SceUID nativeId = sceKernelGetThreadId();
    SceKernelThreadInfo beforePin{}, afterPin{};
    beforePin.size = sizeof(beforePin);
    afterPin.size = sizeof(afterPin);
    const int beforeResult = sceKernelGetThreadInfo(nativeId, &beforePin);
    const int pinResult = port_vita_pin_core(0);
    const int afterResult = sceKernelGetThreadInfo(nativeId, &afterPin);
    port_log("affinity: codex-pin1 role=game-main uid=0x%08x requested=0x10000 pin_rc=0x%08x "
             "before_rc=0x%08x before_mask=0x%08x after_rc=0x%08x after_mask=0x%08x",
             (unsigned)nativeId, (unsigned)pinResult, (unsigned)beforeResult, (unsigned)beforePin.currentCpuAffinityMask,
             (unsigned)afterResult, (unsigned)afterPin.currentCpuAffinityMask);
    void* stack = nullptr; uint32_t stackSize = 0;
    const int result = port_vita_thread_stack(&stack, &stackSize);
    if (result < 0 || stackSize < (16u << 20)) port_fatal("Vita main stack query failed: %08x size %u", (unsigned)result, stackSize);
    thread->stackBase = (u8*)stack + stackSize;
    thread->stackEnd = (u32*)stack;
    PORT_LOG("thread: Vita main kernel stack %p+%u", stack, stackSize);
    port_thread_class(3);
#endif
    __OSCurrentThread = thread;
    sCurrentContext = &thread->context;
    {
        std::lock_guard<std::mutex> lk(sCpuMutex);
        sCpuOwner = thread;
#if defined(__vita__)
        port_game_progress_cpu_checkpoint(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thread)));
#endif
    }
    sIntEnabled = true;
    sBootEntry();
    port_fatal("game main returned");
    return nullptr;
}

extern "C" void port_os_boot(void (*entry)(void)) {
    OSThread* thread = &DefaultThread;
    thread->state = OS_THREAD_STATE_RUNNING;
    thread->attr = 1;
    thread->priority = thread->base = 16;
    thread->suspend = 0;
    thread->value = (void*)-1;
    thread->mutex = NULL;
    OSInitThreadQueue(&thread->queueJoin);
    thread->queueMutex.head = thread->queueMutex.tail = NULL;
    __OSFPUContext = &thread->context;

    const size_t kMainStack = 16u << 20;
#if defined(__vita__)
    thread->stackBase = nullptr;
    thread->stackEnd = nullptr;
#else
    u8* stack = (u8*)port_low_alloc(kMainStack);
    thread->stackBase = stack + kMainStack;
    thread->stackEnd = (u32*)stack;
    *(thread->stackEnd) = 0xDEADBABE;
#endif

    RunQueueBits = 0;
    RunQueueHint = FALSE;
    for (int prio = 0; prio <= 31; prio++) {
        OSInitThreadQueue(&RunQueue[prio]);
    }
    OSInitThreadQueue(&__OSActiveThreadQueue);
    EnqueueTail(&__OSActiveThreadQueue, thread, linkActive);
    Reschedule = 0;

    HostThread* h = hostOf(thread);
    h->started = true;
    sBootEntry = entry;

    // GXV_HOT_KNOBS (see sGateFences): read once, before any guest thread runs (unset, empty or not 0: on).
    if (const char* e = getenv("GXV_HOT_KNOBS")) sGateFences = !(*e && strtol(e, nullptr, 0) == 0);
    else sGateFences = true;
    // GXV_AUDIO_CPU1 (see port_audio_window) and its log line: read once, before any guest thread runs.
    if (!port_vclock_enabled()) {
        const char* c = getenv("GXV_AUDIO_CPU1");
        const char* st = getenv("GXV_AUDIO_STATS");
        sCpu1Mode = c && atoi(c) == 1;
        sAudioStats = (c && *c) || (st && atoi(st) > 0);
        if (c && *c && !sCpu1Mode) sCpu1Off = "GXV_AUDIO_CPU1 is not 1";
    } else {
        sCpu1Off = "the armhf/qemu harness's virtual clock";
    }
    // GXV_AUDIO_HOLD (see port_irq_raise): read once, before any guest thread runs.
    if (const char* e = getenv("GXV_AUDIO_HOLD"); e && *e && !port_vclock_enabled()) {
        const long ms = strtol(e, nullptr, 10);
        if (ms > 0) {
            // (with GXV_AUDIO_CPU1 a frame's start waits up to the most, 12 ms, for the next window: one opens every turn)
            const long limit = sCpu1Mode ? kAudioHoldMaxMs : ms;
            sAudioHoldMs.store(static_cast<int>(limit < kAudioHoldMaxMs ? limit : kAudioHoldMaxMs), std::memory_order_relaxed);
            port_log("audio: the game thread holds audio interrupts up to %d ms for its next wait (GXV_AUDIO_HOLD)",
                     sAudioHoldMs.load(std::memory_order_relaxed));
        }
    }

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) port_fatal("pthread_attr_init failed");
#if defined(__vita__)
    if (pthread_attr_setstacksize(&attr, kMainStack) != 0) port_fatal("main pthread_attr_setstacksize failed");
#else
    if (pthread_attr_setstack(&attr, stack, kMainStack) != 0) port_fatal("main pthread_attr_setstack failed");
#endif
    pthread_t pt;
    if (pthread_create(&pt, &attr, defaultThreadMain, nullptr) != 0) {
        port_fatal("failed to start the game thread");
    }
    pthread_attr_destroy(&attr);
    pthread_detach(pt);
}
