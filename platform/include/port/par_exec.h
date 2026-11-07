// GXV_PAR_* (par13, 5 Nov): a deterministic two-thread executor for the game thread's independent per-model work.
//
// The game thread (core 0) hands a batch of N independent items to one worker thread pinned to another core (GXV_PAR_CORE,
// default 1, where the back end leaves about half of the core idle in the lit Observatory) and works on the same batch itself;
// it returns only when every item is done (the join), so nothing can read a result early. Each item writes only its own
// outputs, so who computes which item never changes a byte: the results are those of the serial loop.
//
//   - one wake per batch (a semaphore signal) and one join (a spin of GXV_PAR_SPIN_US, then a sleep on a second semaphore);
//   - items are claimed with an atomic fetch-add (lock-free, no allocation per item or per batch);
//   - GXV_PAR_SPLIT = p (1-99): the worker starts on the first p % of the items, the game thread on the rest, and each side
//     steals from the other once its own part is done; 0 (default): one shared counter (fully dynamic);
//   - a worker that has not started by the time the game thread has claimed everything is cancelled (no wait for its wake-up);
//   - the worker's priority is one step above the game thread's (GXV_PAR_PRIO overrides it): on the console that is above the
//     back end's (both are made at the same priority), so a woken worker runs at once instead of waiting for the back end to
//     block (the join would wait for it). It runs at most one batch's share (well under a millisecond a picture) and sleeps
//     again, so the back end loses at most that much of its idle time; it never spins while there is no batch.
//
// Header-only (platform/include/port; included by platform/src/port/perf.cpp, which owns the one executor, and by the host test
// tests/par_exec_view.cpp): the Vita build's file lists stay as they are.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace port {
namespace par {

typedef void (*ItemFn)(void* ctx, uint32_t index);

inline uint64_t NowUs() {
#if defined(__vita__)
    return sceKernelGetProcessTimeWide();
#else
    return static_cast< uint64_t >(
        std::chrono::duration_cast< std::chrono::microseconds >(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

inline void CpuRelax() {
#if defined(__arm__) || defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#endif
}

#if defined(__vita__)
extern "C" int port_vita_core3_available(void) __attribute__((weak));  // (vita/runtime/memory.cpp: the fourth core, proven once)
#endif
// The kernel's affinity mask for a worker asked on `core` (drawpar, 6 Nov): 0-2 the user cores; 3 the fourth (system) core, only
// once it is proven for this process (CapUnlocker; port_vita_core3_available), otherwise core 1. *effective: the core asked for.
// (With core 3, a refused thread creation is retried on core 1 by the callers.)
inline int CoreMask(int core, int* effective) {
#if defined(__vita__)
    if (core == 3 && port_vita_core3_available && port_vita_core3_available()) {
        *effective = 3;
        return 0x80000;
    }
#endif
    if (core < 0 || core > 2) {
        core = 1;
    }
    *effective = core;
    return 0x10000 << core;  // SCE_KERNEL_CPU_MASK_USER_0..2
}

// A counting semaphore: the kernel's on the console, a mutex and a condition variable elsewhere.
class Sema {
public:
#if defined(__vita__)
    bool Init(const char* name) {
        id = sceKernelCreateSema(name, 0, 0, 0x7FFF, nullptr);
        return id >= 0;
    }
    void Signal() { sceKernelSignalSema(id, 1); }
    void Wait() { sceKernelWaitSema(id, 1, nullptr); }

private:
    SceUID id = -1;
#else
    bool Init(const char*) { return true; }
    void Signal() {
        std::lock_guard< std::mutex > lock(m);
        ++count;
        cv.notify_one();
    }
    void Wait() {
        std::unique_lock< std::mutex > lock(m);
        cv.wait(lock, [this] { return count > 0; });
        --count;
    }

private:
    std::mutex m;
    std::condition_variable cv;
    unsigned count = 0;
#endif
};

// What one batch did (the caller sums these for its log line).
struct BatchStats {
    uint32_t items = 0, byGame = 0, byWorker = 0;
    uint32_t workerUs = 0;   // the worker's time on the batch (from its start to its last item)
    uint32_t wakeUs = 0;     // from the signal to the worker's start (0: cancelled, or not woken)
    uint32_t joinUs = 0;     // the game thread's wait at the join (after its own last item)
    uint8_t cancelled = 0;   // the game thread claimed everything before the worker started: no wait for it
    uint8_t slept = 0;       // the join outlasted the spin and slept
    uint8_t parallel = 0;    // the worker was asked to help (0: run on the game thread only)
};

class Executor {
public:
    // Starts the worker (once). core: 0-3 (the worker's core; 3 falls back to 1 without the fourth core), priority: the console's thread priority (0: one step above the
    // calling thread's). false: no worker (every batch then runs on the calling thread alone).
    bool Start(int core, int priority, int spinUs) {
        if (started) {
            return ok;
        }
        started = true;
        spin = spinUs < 0 ? 0 : spinUs;
        if (!work.Init("smg_par_work") || !joined.Init("smg_par_join")) {
            return ok = false;
        }
#if defined(__vita__)
        if (priority <= 0) {
            const int mine = sceKernelGetThreadCurrentPriority();
            priority = mine > 0x41 ? mine - 1 : 0x9E;
        }
        const int mask = CoreMask(core, &effectiveCore);
        SceUID uid = sceKernelCreateThread("smg_par", &Executor::VitaEntry, priority, 32 * 1024, 0, mask, nullptr);
        if (uid < 0 && effectiveCore == 3) {
            uid = sceKernelCreateThread("smg_par", &Executor::VitaEntry, priority, 32 * 1024, 0, CoreMask(1, &effectiveCore), nullptr);
        }
        if (uid < 0) {
            return ok = false;
        }
        Executor* self = this;
        if (sceKernelStartThread(uid, sizeof(self), &self) < 0) {
            return ok = false;
        }
        threadPriority = priority;
#else
        (void)CoreMask(core, &effectiveCore);
        (void)priority;
        std::thread([this] { Loop(); }).detach();
#endif
        return ok = true;
    }
    bool Ready() const { return ok; }
    int Priority() const { return threadPriority; }
    int Core() const { return effectiveCore; }  // (the core the worker was put on: 3 only when the fourth core is proven)

    // Runs fn(ctx, i) for every i in [0, n) and returns when all are done. parallel = false (or no worker, or n < 2): the
    // calling thread runs them alone, in order. splitPercent: see the file comment.
    BatchStats Run(ItemFn fn, void* ctx, uint32_t n, bool parallel, int splitPercent) {
        BatchStats st;
        st.items = n;
        if (n == 0) {
            return st;
        }
        if (!parallel || !ok || n < 2) {
            for (uint32_t i = 0; i < n; ++i) {
                fn(ctx, i);
            }
            st.byGame = n;
            return st;
        }
        st.parallel = 1;
        // The batch, published by the release store of `state` below (the worker reads it after its acquire).
        bFn = fn;
        bCtx = ctx;
        bN = n;
        uint32_t k = n;  // items [0, k) start on the worker's counter, [k, n) on the game thread's
        if (splitPercent > 0 && splitPercent < 100) {
            k = static_cast< uint32_t >((static_cast< uint64_t >(n) * static_cast< uint32_t >(splitPercent)) / 100u);
        }
        bK = k;
        front.store(0, std::memory_order_relaxed);
        back.store(k, std::memory_order_relaxed);
        workerItems.store(0, std::memory_order_relaxed);
        workerUs.store(0, std::memory_order_relaxed);
        wakeUs.store(0, std::memory_order_relaxed);
        gen = (gen + 1) & 0x3FFFFFFFu;  // (the batch's number: 30 bits beside the 2-bit phase, never 0: 0 is "nobody sleeps")
        if (gen == 0) {
            gen = 1;
        }
        const uint32_t g = gen;
        armedAt = NowUs();
        state.store(Encode(g, kArmed), std::memory_order_release);
        work.Signal();

        // The game thread's part: its own range first (none in the shared mode), then whatever the worker has not claimed.
        uint32_t mine = 0;
        for (;;) {
            const uint32_t i = back.fetch_add(1, std::memory_order_relaxed);
            if (i >= n) {
                break;
            }
            fn(ctx, i);
            ++mine;
        }
        for (;;) {
            const uint32_t i = front.fetch_add(1, std::memory_order_relaxed);
            if (i >= k) {
                break;
            }
            fn(ctx, i);
            ++mine;
        }
        st.byGame = mine;

        // The join.
        uint32_t expected = Encode(g, kArmed);
        if (state.compare_exchange_strong(expected, Encode(g, kCancelled), std::memory_order_acq_rel)) {
            st.cancelled = 1;  // (the worker never started: it finds the batch cancelled when it wakes, and claims nothing)
        } else {
            const uint64_t t0 = NowUs();
            bool done = state.load(std::memory_order_acquire) == Encode(g, kDone);
            while (!done && NowUs() - t0 < static_cast< uint64_t >(spin)) {
                for (int k = 0; k < 32 && !done; ++k) {  // (the clock is a kernel call on the console: read it every 32 polls)
                    CpuRelax();
                    done = state.load(std::memory_order_acquire) == Encode(g, kDone);
                }
            }
            if (!done) {
                // The flag names the batch the game thread sleeps for: a worker still finishing an earlier batch cannot take it
                // (and so cannot send a signal that would end this join early).
                sleeping.store(g, std::memory_order_seq_cst);
                if (state.load(std::memory_order_seq_cst) == Encode(g, kDone)) {
                    // Done meanwhile: if the worker has not taken the flag, take it back (no signal will come); if it has, its
                    // signal is on the way and must be consumed so that it does not wake a later join.
                    uint32_t mineFlag = g;
                    if (!sleeping.compare_exchange_strong(mineFlag, 0, std::memory_order_seq_cst)) {
                        joined.Wait();
                    }
                } else {
                    joined.Wait();
                }
                st.slept = 1;
            }
            st.joinUs = static_cast< uint32_t >(NowUs() - t0);
            // (acquire: everything the worker wrote is visible; it touches nothing of this batch any more)
            (void)state.load(std::memory_order_acquire);
        }
        st.byWorker = workerItems.load(std::memory_order_relaxed);
        st.workerUs = workerUs.load(std::memory_order_relaxed);
        st.wakeUs = wakeUs.load(std::memory_order_relaxed);
        return st;
    }

private:
    enum : uint32_t { kIdle = 0, kArmed = 1, kRunning = 2, kDone = 3, kCancelled = 3 };
    static uint32_t Encode(uint32_t g, uint32_t phase) { return (g << 2) | phase; }

#if defined(__vita__)
    static int VitaEntry(SceSize, void* argp) {
        Executor* self = *static_cast< Executor** >(argp);
        self->Loop();
        return 0;
    }
#endif

    void Loop() {
        for (;;) {
            work.Wait();
            uint32_t s = state.load(std::memory_order_acquire);
            if ((s & 3u) != kArmed || !state.compare_exchange_strong(s, (s & ~3u) | kRunning, std::memory_order_acq_rel)) {
                continue;  // (cancelled, or a stale signal of an earlier batch)
            }
            const uint64_t t0 = NowUs();
            wakeUs.store(static_cast< uint32_t >(t0 - armedAt), std::memory_order_relaxed);
            const ItemFn fn = bFn;
            void* const ctx = bCtx;
            const uint32_t n = bN, k = bK;
            uint32_t mine = 0;
            for (;;) {
                const uint32_t i = front.fetch_add(1, std::memory_order_relaxed);
                if (i >= k) {
                    break;
                }
                fn(ctx, i);
                ++mine;
            }
            for (;;) {
                const uint32_t i = back.fetch_add(1, std::memory_order_relaxed);
                if (i >= n) {
                    break;
                }
                fn(ctx, i);
                ++mine;
            }
            workerItems.store(mine, std::memory_order_relaxed);
            workerUs.store(static_cast< uint32_t >(NowUs() - t0), std::memory_order_relaxed);
            state.store((s & ~3u) | kDone, std::memory_order_seq_cst);  // (release: the results and the counters above)
            uint32_t sleeper = s >> 2;  // (this batch's number: only a join of this batch is woken)
            if (sleeping.compare_exchange_strong(sleeper, 0, std::memory_order_seq_cst)) {
                joined.Signal();
            }
        }
    }

    Sema work, joined;
    bool started = false, ok = false;
    int spin = 30, threadPriority = 0, effectiveCore = 1;
    uint32_t gen = 0;
    uint64_t armedAt = 0;  // (written before the arming release store, read by the worker after its acquire)
    ItemFn bFn = nullptr;
    void* bCtx = nullptr;
    uint32_t bN = 0, bK = 0;
    alignas(64) std::atomic< uint32_t > state{0};
    alignas(64) std::atomic< uint32_t > front{0};
    alignas(64) std::atomic< uint32_t > back{0};
    alignas(64) std::atomic< uint32_t > sleeping{0};
    std::atomic< uint32_t > workerItems{0}, workerUs{0}, wakeUs{0};
};

}  // namespace par
}  // namespace port
