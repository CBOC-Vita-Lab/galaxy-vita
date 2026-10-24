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

#include "port/heap_routing.h"
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

extern "C" void port_irq_raise(int irq) {
    sPendingIrq.fetch_or(1u << irq);
    std::lock_guard<std::mutex> lk(sIdleMutex);
    sIdleCv.notify_all();
}

static void runIrqHandlers() {
    // Called with the CPU held.  Handlers run with interrupts disabled and
    // may not switch threads (SelectThread checks sInIrq).
    bool savedEn = sIntEnabled;
    sInIrq = true;
    sIntEnabled = false;
    // One atomic snapshot per safe point. A slow native display can receive
    // another VI while blocked; draining fresh bits forever starves game code.
    const u32 bits=sPendingIrq.exchange(0);
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

static void deliverIrqs() {
    if (kGapDiagnostics && __builtin_expect(sGapNs != 0, 0)) {
        gapNoteSlow();
    }
    // The plain globals first: nearly always nothing is pending, and a thread_local read costs a call (emulated
    // thread-local storage on the Vita: __emutls_get_address, pthread_getspecific) on every interrupt-state change.
    if (sPendingIrq.load(std::memory_order_relaxed) == 0 || sInIrq || !sIntEnabled || tlsDeliveringIrqs || tlsSelf == nullptr) {
        return;
    }
    // Keep this caller's post-IRQ scheduling from recursively draining fresh
    // work. Another guest selected below has its own guard and can take IRQs.
    tlsDeliveringIrqs=true;
    runIrqHandlers();
    // Returning from an exception: switch if a handler readied a better thread.
    if (RunQueueHint) {
        BOOL en = OSDisableInterrupts();
        SelectThread(FALSE);
        OSRestoreInterrupts(en);
    }
    tlsDeliveringIrqs=false;
}

// (for the diagnostics of other files under PETARI_GAPLOG: how long ago was the last safe point, and the threshold; 0 when the log is off)
extern "C" int64_t port_gap_age_ns(void) { return kGapDiagnostics && sGapNs > 0 ? port_host_time_ns() - sGapLast : 0; }
extern "C" int64_t port_gap_threshold_ns(void) { return kGapDiagnostics && sGapNs > 0 ? sGapNs : 0; }

extern "C" void port_irq_poll(void) {
    if (kGapDiagnostics && sGapNs) sGapSite = __builtin_return_address(0);
    deliverIrqs();
}
// (the same, for a caller that wants its own caller named: operator new, so a stretch ends at the code that allocated)
extern "C" void port_irq_poll_at(void* site) {
    if (kGapDiagnostics && sGapNs) sGapSite = site;
    deliverIrqs();
}

extern "C" {

BOOL OSDisableInterrupts(void) {
    BOOL prev = sIntEnabled;
    if (prev) {
        if (kGapDiagnostics && sGapNs) sGapSite = __builtin_return_address(0);
        deliverIrqs();  // a pending interrupt would have been taken before this point
    }
    sIntEnabled = false;
    return prev;
}

BOOL OSEnableInterrupts(void) {
    BOOL prev = sIntEnabled;
    if (kGapDiagnostics && sGapNs) sGapSite = __builtin_return_address(0);
    sIntEnabled = true;
    deliverIrqs();
    return prev;
}

BOOL OSRestoreInterrupts(BOOL level) {
    BOOL prev = sIntEnabled;
    sIntEnabled = level ? true : false;
    if (sIntEnabled && !prev) {
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

// Waits (with sCpuMutex held via lk) until `self` owns the CPU again.
static void waitForCpu(std::unique_lock<std::mutex>& lk, OSThread* self, HostThread* h) {
    h->cv.wait(lk, [&] { return sCpuOwner == self || h->cancelled; });
    if (h->cancelled && sCpuOwner != self) {
        lk.unlock();
        pthread_exit(nullptr);
    }
}

// Hands the CPU from the calling host thread to `next`.  Returns when the
// caller is scheduled again (immediately if next == caller), or never if the
// caller is exiting.
static void switchTo(OSThread* next, bool callerExiting) {
    OSThread* self = tlsSelf;
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
    waitForCpu(lk, self, hs);
    lk.unlock();
    sIntEnabled = hs->intEnabled;
    __OSCurrentThread = self;
    sCurrentContext = &self->context;
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

static bool sExiting;  // set by OSExitThread for the calling thread's switch

extern "C" OSThread* SelectThread(BOOL yield) {
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

OSThread* OSGetCurrentThread(void) { return __OSCurrentThread; }

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

void OSYieldThread(void) {
    BOOL enabled = OSDisableInterrupts();
    SelectThread(TRUE);
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
OSContext* OSGetCurrentContext(void) { return sCurrentContext ? sCurrentContext : &IdleContext; }
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
    std::lock_guard<std::mutex> lock(sCpuMutex);
    return sCpuOwner==tlsSelf;
}
extern "C" int port_use_game_heap(void) { return tlsSelf != nullptr && tlsHostAllocDepth == 0; }
extern "C" void port_host_alloc_scope_enter(void) { tlsHostAllocDepth++; }
extern "C" void port_host_alloc_scope_leave(void) { tlsHostAllocDepth--; }
extern "C" int port_is_game_heap_ptr(const void* p) { return port_is_game_memory(p); }
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
        port_log("host heap: arena %u KiB, in use %u KiB (a %lu KiB request)", hs.arenaKiB, hs.inUseKiB, static_cast<unsigned long>(request / 1024u));
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
    SiteGuard() : ok(!sSiteBusy) { if (ok) sSiteBusy = true; }
    ~SiteGuard() { if (ok) sSiteBusy = false; }
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

extern "C" void port_host_alloc_failed(size_t size, int align) {
    const PortHeapStats hs = portHeapStats();
#if defined(__vita__)
    port_log("FATAL: host heap exhausted: a request for %lu KiB (alignment %d) found no room; C heap arena %u KiB, in use %u KiB, free %u KiB in %u pieces, "
             "releasable at the top %u KiB",
             static_cast<unsigned long>(size / 1024u), align, hs.arenaKiB, hs.inUseKiB, hs.freeKiB, hs.pieces, hs.topKiB);
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
