// GXV_PAR_DL (gamedraw23, 8 Nov): the drawn turn's material-list flush (port_dl_flush_stale: every listed model's calcMaterial and
// differed lists) made on another core while the game thread makes the view pass (port_view_flush), joined before the draw.
// Header-only: decomp/src/Game/LiveActor/ModelManager.cpp uses it through platform/src/port/perf.cpp (the one worker), the host test
// tests/par_dl.cpp directly. The design and its proof are ModelManager.cpp's "GXV_PAR_DL" comment.
//
// OneShot: one job at a time that the game thread always joins (it is never left unfinished). States:
//     0 idle -> 1 armed (published) -> 2 running (the worker took it) -> 3 done (its writes visible after the acquire that saw 3)
//     1 armed -> 4 taken (the game thread got to the join, or needed the material path, before the worker woke: it runs the job itself)
// A job is therefore run exactly once, by one thread, and the game thread reads nothing of it before it is done.
#pragma once

#include "port/par_spec.h"  // (Sema, CoreMask, NowUs, CpuRelax, FpModeGet / FpModeSet)

#include <atomic>
#include <cstdint>
#include <cstring>

namespace port {
namespace par {

class OneShot {
public:
    enum : uint32_t { kIdle = 0, kArmed = 1, kRunning = 2, kDone = 3, kTaken = 4 };

    // core 0-3 (3: the fourth core when proven, else 1); priority -1: one step ABOVE the calling thread (the game thread waits for this
    // worker at the join, so nothing of ours on its core may preempt it), 0: one step below, >0: that priority. false: no worker.
    bool Start(int core, int priority, const char* name = "smg_dl") {
        if (started) {
            return ok;
        }
        started = true;
        if (!work.Init("smg_dl_work") || !joined.Init("smg_dl_join")) {
            return ok = false;
        }
#if defined(__vita__) && !defined(PORT_ARMHF_HARNESS)  // (armhf harness: Linux threads, no psp2 kernel headers)
        const int mine = sceKernelGetThreadCurrentPriority();
        if (priority < 0) {
            priority = mine > 0x41 ? mine - 1 : 0x9E;
        } else if (priority == 0) {
            priority = mine > 0 && mine < 0xBF ? mine + 1 : 0xA0;
        }
        const int mask = CoreMask(core, &effectiveCore);
        SceUID uid = sceKernelCreateThread(name, &OneShot::VitaEntry, priority, 128 * 1024, 0, mask, nullptr);
        if (uid < 0 && effectiveCore == 3) {
            uid = sceKernelCreateThread(name, &OneShot::VitaEntry, priority, 128 * 1024, 0, CoreMask(1, &effectiveCore), nullptr);
        }
        if (uid < 0) {
            return ok = false;
        }
        core3::MarkOnCore3(slot = core3::RegisterThread(name), effectiveCore == 3);  // (core3-38: before the thread starts: it reads the slot)
        OneShot* self = this;
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
    int Core() const { return effectiveCore; }
    // The worker's stack: its top (the frame address of its loop, 0 until the thread runs) and how far below it a job's frames can
    // reach. ModelManager.cpp tells the material path "this is the worker" by the stack pointer (J3DSys.hpp PortDlGameEnter): a compare
    // instead of a thread-id call on every way into the material path.
    uintptr_t StackTop() const { return stackTop.load(std::memory_order_acquire); }
    static constexpr uintptr_t kStackSpan =
#if defined(__vita__) && !defined(PORT_ARMHF_HARNESS)  // (armhf harness: Linux threads, no psp2 kernel headers)
        120 * 1024;  // (of the 128 KiB the thread is made with)
#else
        1024 * 1024;  // (of a host thread's 8 MiB)
#endif

    // Publishes fn(ctx). The previous job must have been joined (Take / Wait / Reset).
    void Dispatch(void (*fn)(void*), void* ctx) {
        jFn = fn;
        jCtx = ctx;
        jFpMode = FpModeGet();
        tDispatch = NowUs();
        state.store(kArmed, std::memory_order_release);  // (publishes the job and everything the game thread wrote before it)
        work.Signal();
    }
    // The game thread: true when the worker had not started the job; it is the caller's now (run it, then Reset()). false: the worker
    // has it (running or done): Wait() for it.
    bool Take() {
        uint32_t s = kArmed;
        if (state.compare_exchange_strong(s, kTaken, std::memory_order_acq_rel)) {
            ++taken;
            return true;
        }
        return false;
    }
    bool Done() const { return state.load(std::memory_order_acquire) == kDone; }
    // Waits until the worker's job is done (spins spinUs, then sleeps on a semaphore), then makes the worker idle. *waitUs: the wait.
    void Wait(uint32_t spinUs, uint32_t* waitUs) {
        const uint64_t t0 = NowUs();
        uint32_t polls = 0;
        while (state.load(std::memory_order_acquire) != kDone) {
            CpuRelax();
            if ((++polls & 63u) == 0 && NowUs() - t0 >= spinUs) {
                sleeping.store(1, std::memory_order_seq_cst);
                if (state.load(std::memory_order_seq_cst) == kDone) {
                    if (sleeping.exchange(0, std::memory_order_seq_cst) == 0) {
                        joined.Wait();  // (the worker saw the flag: its signal is on the way; consume it)
                    }
                } else {
                    joined.Wait();
                }
                ++sleeps;
                break;
            }
        }
        (void)state.load(std::memory_order_acquire);
        *waitUs = static_cast< uint32_t >(NowUs() - t0);
        state.store(kIdle, std::memory_order_relaxed);
    }
    // After the game thread ran a taken job itself.
    void Reset() { state.store(kIdle, std::memory_order_relaxed); }

    // The worker's busy time, its jobs, the jobs the game thread took, the joins that slept, and the worker's mean start delay.
    void TakeStats(uint32_t* busyUs, uint32_t* jobs, uint32_t* takenJobs, uint32_t* slept, uint32_t* wakeUs) {
        *busyUs = workerUs.exchange(0, std::memory_order_relaxed);
        *jobs = workerJobs.exchange(0, std::memory_order_relaxed);
        *wakeUs = workerWakeUs.exchange(0, std::memory_order_relaxed);
        *takenJobs = taken;
        *slept = sleeps;
        taken = sleeps = 0;
    }

private:
#if defined(__vita__) && !defined(PORT_ARMHF_HARNESS)  // (armhf harness: Linux threads, no psp2 kernel headers)
    static int VitaEntry(SceSize, void* argp) {
        OneShot* self = *static_cast< OneShot** >(argp);
        self->Loop();
        return 0;
    }
#endif
    void Loop() {
        stackTop.store(reinterpret_cast< uintptr_t >(__builtin_frame_address(0)) + 256, std::memory_order_release);
        core3::Worker c3(slot);  // (core3-38: one job is one call here, so on core 3 it is only metered, not cut)
        for (;;) {
            work.Wait();
            c3.Wake();
            uint32_t s = kArmed;
            if (!state.compare_exchange_strong(s, kRunning, std::memory_order_acq_rel)) {
                continue;  // (taken by the game thread before this wake)
            }
            const uint64_t t0 = NowUs();
            const uint64_t td = tDispatch;
            FpModeSet(jFpMode);  // (the game thread's floating-point mode: the same bytes as its own run)
            jFn(jCtx);
            c3.Sleep();
            const uint64_t t1 = NowUs();
            workerUs.fetch_add(static_cast< uint32_t >(t1 - t0), std::memory_order_relaxed);
            workerWakeUs.fetch_add(static_cast< uint32_t >(t0 > td ? t0 - td : 0), std::memory_order_relaxed);
            workerJobs.fetch_add(1, std::memory_order_relaxed);
            state.store(kDone, std::memory_order_seq_cst);
            if (sleeping.exchange(0, std::memory_order_seq_cst) == 1) {
                joined.Signal();
            }
        }
    }

    Sema work, joined;
    bool started = false, ok = false;
    int threadPriority = 0, effectiveCore = 1, slot = -1;
    uint32_t taken = 0, sleeps = 0;
    void (*jFn)(void*) = nullptr;
    void* jCtx = nullptr;
    uint32_t jFpMode = 0;
    uint64_t tDispatch = 0;
    alignas(64) std::atomic< uint32_t > state{kIdle};
    alignas(64) std::atomic< uint32_t > sleeping{0};
    std::atomic< uint32_t > workerUs{0}, workerJobs{0}, workerWakeUs{0};
    std::atomic< uintptr_t > stackTop{0};
};

// The J3DSys after the overlap (ModelManager.cpp): `live` is the game thread's j3dSys (the view pass wrote into it meanwhile), `pre` a copy
// of it made at the dispatch, `mine` the worker's private one (a copy of `pre` the material lists wrote into). In the serial order the
// material lists come first and the view pass second, so a word the worker changed takes the worker's value, unless the view pass changed
// it as well: then the view pass's value stands (counted in *conflicts; none is expected: the view pass writes only the view matrix, which
// the material lists only read). A word only the view pass changed keeps its value. *taken: the words taken from the worker.
inline void MergeWords(void* live, const void* pre, const void* mine, size_t bytes, unsigned* taken, unsigned* conflicts) {
    const size_t n = bytes / 4;
    uint8_t* l = static_cast< uint8_t* >(live);
    const uint8_t* p = static_cast< const uint8_t* >(pre);
    const uint8_t* m = static_cast< const uint8_t* >(mine);
    for (size_t i = 0; i < n; ++i) {
        uint32_t lw, pw, mw;
        std::memcpy(&lw, l + 4 * i, 4);
        std::memcpy(&pw, p + 4 * i, 4);
        std::memcpy(&mw, m + 4 * i, 4);
        if (mw != pw) {
            if (lw != pw) {
                ++*conflicts;
            } else {
                std::memcpy(l + 4 * i, &mw, 4);
                ++*taken;
            }
        }
    }
    for (size_t i = n * 4; i < bytes; ++i) {  // (a tail shorter than a word: bytewise, the same rule)
        if (m[i] != p[i]) {
            if (l[i] != p[i]) {
                ++*conflicts;
            } else {
                l[i] = m[i];
                ++*taken;
            }
        }
    }
}

}  // namespace par
}  // namespace port
