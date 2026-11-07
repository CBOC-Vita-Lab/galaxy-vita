// GXV_PAR_ANIM (joint15, 5 Nov): an asynchronous worker for speculative per-model work (header-only; used by
// decomp/src/Game/LiveActor/ModelManager.cpp through platform/src/port/perf.cpp, and by the host test tests/par_anim_walk.cpp).
//
// Unlike par13's Executor (par_exec.h), the game thread does not wait for a batch: it publishes N jobs and goes on with its turn,
// and the worker (pinned to another core, below the game thread's priority by default) computes them into memory of their own
// while the game thread draws, waits for the display or runs the next turn's movement. Each job has a state the two threads agree
// on with one atomic exchange:
//     0 pending -> 1 running (the worker took it) -> 2 done (the worker's results are visible after an acquire load of the state)
//     0 pending -> 3 taken (the game thread got there first: the worker skips it, and the game thread computes as before)
// so a job is computed at most once and a result is read only after it is complete. The game thread's Claim(i) never waits for a
// job the worker has not started, and waits at most GXV_PAR_ANIM_WAIT_US for one it is in the middle of (then it computes it
// itself; the worker's copy is simply not used).
// TryQuiesce() ends the batch unless the worker is in the middle of a job (it is told to stop after it): the game thread never
// waits for the worker at a turn. Quiesce() also waits for that job (a sleep on a semaphore with the batch's number in the sleeping
// flag, as in par_exec.h): only a scene change uses it, so that the memory the jobs read (model data, BCK resources) is never freed
// under the worker. A new batch is made only once the last one is over.
#pragma once

#include "port/par_exec.h"  // (Sema, NowUs, CpuRelax)

#include <atomic>
#include <cstdint>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace port {
namespace par {

// The floating-point mode is per thread (ARM: FPSCR's flush-to-zero, default-NaN and rounding bits; x86: MXCSR's): the worker runs
// each batch in the mode of the thread that made it, so that a result is the same bytes on either thread even for subnormal values.
inline uint32_t FpModeGet() {
#if defined(__arm__) && defined(__ARM_FP)
    uint32_t v;
    __asm__ __volatile__("vmrs %0, fpscr" : "=r"(v));
    return v & 0x07C00000u;  // (AHP, DN, FZ, RMode)
#elif defined(__x86_64__) || defined(__i386__)
    return _mm_getcsr() & 0xFFC0u;  // (DAZ, the exception masks, rounding, FTZ)
#else
    return 0;
#endif
}
inline void FpModeSet(uint32_t mode) {
#if defined(__arm__) && defined(__ARM_FP)
    uint32_t v;
    __asm__ __volatile__("vmrs %0, fpscr" : "=r"(v));
    v = (v & ~0x07C00000u) | mode;
    __asm__ __volatile__("vmsr fpscr, %0" : : "r"(v));
#elif defined(__x86_64__) || defined(__i386__)
    _mm_setcsr((_mm_getcsr() & ~0xFFC0u) | mode);
#else
    (void)mode;
#endif
}

enum : uint8_t { kJobPending = 0, kJobRunning = 1, kJobDone = 2, kJobTaken = 3 };

class SpecWorker {
public:
    enum ClaimResult { kClaimDone, kClaimTaken, kClaimBusy };

    // Starts the worker (once). core 0-3 (3: the fourth core when proven, else 1). priority 0: one step below the calling thread's (the
    // console's priorities: a larger number is a lower priority); -1: one step above it. false: no worker (Dispatch then does nothing
    // and every job stays pending, so the game thread computes as before).
    bool Start(int core, int priority, const char* name = "smg_spec") {
        if (started) {
            return ok;
        }
        started = true;
        if (!work.Init("smg_spec_work") || !joined.Init("smg_spec_join")) {
            return ok = false;
        }
#if defined(__vita__)
        if (priority < 0) {
            const int mine = sceKernelGetThreadCurrentPriority();  // (-1: one step ABOVE the calling thread: GXV_PAR_VIEW=2's worker)
            priority = mine > 0x41 ? mine - 1 : 0x9E;
        } else if (priority == 0) {
            const int mine = sceKernelGetThreadCurrentPriority();
            priority = mine > 0 && mine < 0xBF ? mine + 1 : 0xA0;
        }
        const int mask = CoreMask(core, &effectiveCore);
        SceUID uid = sceKernelCreateThread(name, &SpecWorker::VitaEntry, priority, 64 * 1024, 0, mask, nullptr);
        if (uid < 0 && effectiveCore == 3) {
            uid = sceKernelCreateThread(name, &SpecWorker::VitaEntry, priority, 64 * 1024, 0, CoreMask(1, &effectiveCore), nullptr);
        }
        if (uid < 0) {
            return ok = false;
        }
        SpecWorker* self = this;
        if (sceKernelStartThread(uid, sizeof(self), &self) < 0) {
            return ok = false;
        }
        threadPriority = priority;
#else
        (void)CoreMask(core, &effectiveCore);
        (void)priority;
        (void)name;
        std::thread([this] { Loop(); }).detach();
#endif
        return ok = true;
    }
    bool Ready() const { return ok; }
    int Priority() const { return threadPriority; }
    int Core() const { return effectiveCore; }  // (the core the worker was put on: 3 only when the fourth core is proven)

    // Publishes jobs [0, n): fn(ctx, i) for each that the worker claims, in order. `items` holds n states, all kJobPending
    // (the caller resets them after Quiesce(), while no batch is open).
    void Dispatch(ItemFn fn, void* ctx, std::atomic< uint8_t >* items, uint32_t n) {
        Quiesce();
        if (!ok || n == 0) {
            return;
        }
        bFn = fn;
        bCtx = ctx;
        bItems = items;
        bN = n;
        bFpMode = FpModeGet();
        stop.store(0, std::memory_order_relaxed);
        gen = (gen + 1) & 0x3FFFFFFFu;
        if (gen == 0) {
            gen = 1;
        }
        open = true;
        state.store(Encode(gen, kArmed), std::memory_order_release);  // (publishes the batch and the reset states)
        work.Signal();
    }

    // The game thread, at the job's real place. kClaimDone: the worker finished it (its results are visible); kClaimTaken: the game
    // thread owns it now (the worker never starts it); kClaimBusy: the worker is still in it after waitUs (compute as before).
    static ClaimResult Claim(std::atomic< uint8_t >& item, uint32_t waitUs) {
        uint64_t t0 = 0;
        for (uint32_t polls = 0;; ++polls) {
            uint8_t v = item.load(std::memory_order_acquire);
            if (v == kJobDone) {
                return kClaimDone;
            }
            if (v == kJobTaken) {
                return kClaimTaken;
            }
            if (v == kJobPending) {
                if (item.compare_exchange_strong(v, kJobTaken, std::memory_order_acq_rel)) {
                    return kClaimTaken;
                }
                continue;
            }
            // (running: the worker is in it)
            if ((polls & 31u) == 0) {
                const uint64_t now = NowUs();
                if (t0 == 0) {
                    t0 = now;
                } else if (now - t0 >= waitUs) {
                    return kClaimBusy;
                }
            }
            CpuRelax();
        }
    }

    // Ends the open batch if the worker is not in it (or as soon as it is out of its current job, within a short spin): the jobs it
    // has not started are left alone from now on. true: the batch is over, and nothing of it (fn, ctx, items, what the jobs read) is
    // touched by the worker any more; false: the worker is still in a job (it stops after it), the batch stays open: call again.
    bool TryQuiesce() {
        if (!open) {
            return true;
        }
        stop.store(1, std::memory_order_relaxed);
        const uint32_t g = gen;
        uint32_t expected = Encode(g, kArmed);
        if (state.compare_exchange_strong(expected, Encode(g, kDone), std::memory_order_acq_rel)) {
            open = false;  // (the worker never started it: when it wakes it finds the batch over)
            return true;
        }
        bool done = state.load(std::memory_order_acquire) == Encode(g, kDone);
        for (int k = 0; k < 2000 && !done; ++k) {
            CpuRelax();
            done = state.load(std::memory_order_acquire) == Encode(g, kDone);
        }
        if (done) {
            open = false;
        }
        return done;
    }

    // Ends the open batch, waiting for the job the worker may be in (a sleep on a semaphore with the batch's number in the sleeping
    // flag, as in par_exec.h). Afterwards nothing of the batch is touched by the worker. (Scene changes: the jobs read model data.)
    void Quiesce() {
        if (TryQuiesce()) {
            return;
        }
        const uint32_t g = gen;
        sleeping.store(g, std::memory_order_seq_cst);
        if (state.load(std::memory_order_seq_cst) == Encode(g, kDone)) {
            uint32_t mine = g;
            if (!sleeping.compare_exchange_strong(mine, 0, std::memory_order_seq_cst)) {
                joined.Wait();  // (its signal is on the way: consume it)
            }
        } else {
            joined.Wait();
        }
        ++sleeps;
        (void)state.load(std::memory_order_acquire);
        open = false;
    }

    // The worker's busy time and jobs since the last call (the log line).
    void TakeStats(uint32_t* busyUs, uint32_t* jobs, uint32_t* quiesceSleeps) {
        *busyUs = workerUs.exchange(0, std::memory_order_relaxed);
        *jobs = workerJobs.exchange(0, std::memory_order_relaxed);
        *quiesceSleeps = sleeps;
        sleeps = 0;
    }

private:
    enum : uint32_t { kIdle = 0, kArmed = 1, kRunning = 2, kDone = 3 };
    static uint32_t Encode(uint32_t g, uint32_t phase) { return (g << 2) | phase; }

#if defined(__vita__)
    static int VitaEntry(SceSize, void* argp) {
        SpecWorker* self = *static_cast< SpecWorker** >(argp);
        self->Loop();
        return 0;
    }
#endif

    void Loop() {
        for (;;) {
            work.Wait();
            uint32_t s = state.load(std::memory_order_acquire);
            if ((s & 3u) != kArmed || !state.compare_exchange_strong(s, (s & ~3u) | kRunning, std::memory_order_acq_rel)) {
                continue;  // (quiesced before it started, or a stale signal)
            }
            const uint64_t t0 = NowUs();
            FpModeSet(bFpMode);  // (the dispatching thread's floating-point mode)
            const ItemFn fn = bFn;
            void* const ctx = bCtx;
            std::atomic< uint8_t >* const items = bItems;
            const uint32_t n = bN;
            uint32_t mine = 0;
            for (uint32_t i = 0; i < n && stop.load(std::memory_order_relaxed) == 0; ++i) {
                uint8_t v = kJobPending;
                if (!items[i].compare_exchange_strong(v, kJobRunning, std::memory_order_acq_rel)) {
                    continue;  // (taken by the game thread)
                }
                fn(ctx, i);
                items[i].store(kJobDone, std::memory_order_release);
                ++mine;
            }
            workerJobs.fetch_add(mine, std::memory_order_relaxed);
            workerUs.fetch_add(static_cast< uint32_t >(NowUs() - t0), std::memory_order_relaxed);
            state.store((s & ~3u) | kDone, std::memory_order_seq_cst);
            uint32_t sleeper = s >> 2;
            if (sleeping.compare_exchange_strong(sleeper, 0, std::memory_order_seq_cst)) {
                joined.Signal();
            }
        }
    }

    Sema work, joined;
    bool started = false, ok = false, open = false;
    int threadPriority = 0, effectiveCore = 1;
    uint32_t gen = 0, sleeps = 0;
    ItemFn bFn = nullptr;
    void* bCtx = nullptr;
    std::atomic< uint8_t >* bItems = nullptr;
    uint32_t bN = 0, bFpMode = 0;
    alignas(64) std::atomic< uint32_t > state{0};
    alignas(64) std::atomic< uint32_t > stop{0};
    alignas(64) std::atomic< uint32_t > sleeping{0};
    std::atomic< uint32_t > workerUs{0};  // (32-bit atomics only: lock-free on the Cortex-A9)
    std::atomic< uint32_t > workerJobs{0};
};

}  // namespace par
}  // namespace port
