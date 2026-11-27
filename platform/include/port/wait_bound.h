// hitch30 (13 Nov 2026): a bounded wait for the game thread (header-only; host proof tests/wait_bound.cpp).
//
// The game thread ran at 0x9F; par-anim's workers run at 0xA0 (one step below), the others one step above, and two of them can share a core
// with the render thread (a stock console has three cores: GXV_STOCK_SIM=1; or the fourth core refused). A wait that only spins
// (while (state == running) CpuRelax();) for a worker that is not running then burns the game thread's whole time slice, or
// longer, and the picture is late: a frame hitch. BoundedWait spins for at most `spinUs` (default 100: the kernel's wake-up of a
// sleeping thread costs about that), then sleeps in naps of `napUs` (default 50), which gives the core to whoever the work is waiting
// for. The wait itself ends at the same moment as before (the state changes), so what is computed or drawn does not change.
// Where the waiting thread can do the work itself, the caller does that (par_exec's join, Claim, the finishing thread's Drain); the
// three waits here (plant build in progress, spec job running) are for work only the worker holds, so they nap.
//   GXV_BOUND_SPIN_US=<0..2000>   spin bound (default 100; 0 = the old pure spin, for A/B)
//   GXV_BOUND_NAP_US=<10..2000>   nap length after the spin (default 50)
// Census (WaitCensus, one per site; printed by a "spin:" line in the subsystem's 5 s summary): waits, spins that ran out, take-overs
// (counted by the caller where it can do the work itself; 0 for these sites), longest wait.
#pragma once
#include "port/par_exec.h"  // (NowUs, CpuRelax)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace port {
namespace par {

inline void NapUs(uint32_t us) {
#if defined(__vita__) && (!defined(PORT_ARMHF_HARNESS) || defined(PORT_ARMHF_VITA_SHIM))  // (armhf harness: Linux threads, no psp2 kernel headers; with the shim it takes the console's nap)
    sceKernelDelayThread(us);
#else
    std::this_thread::sleep_for(std::chrono::microseconds(us));
#endif
}

struct WaitCensus {
    std::atomic< uint32_t > waits{0};      // waits that had to wait at all
    std::atomic< uint32_t > spunOut{0};    // of those, the ones whose spin ran out (the rest ended inside the spin bound)
    std::atomic< uint32_t > takeOvers{0};  // the caller did the work itself instead (set by the caller)
    std::atomic< uint32_t > maxUs{0};      // longest wait since the last line (reset by Line)
    std::atomic< uint32_t > maxEverUs{0};
    std::atomic< uint32_t > totalUs{0};  // (32-bit atomics only: lock-free on the Cortex-A9; wraps after 71 minutes of waiting)
    void Note(uint32_t waitUs, bool out) {
        waits.fetch_add(1, std::memory_order_relaxed);
        if (out) {
            spunOut.fetch_add(1, std::memory_order_relaxed);
        }
        totalUs.fetch_add(waitUs, std::memory_order_relaxed);
        uint32_t m = maxUs.load(std::memory_order_relaxed);
        while (waitUs > m && !maxUs.compare_exchange_weak(m, waitUs, std::memory_order_relaxed)) {
        }
        uint32_t e = maxEverUs.load(std::memory_order_relaxed);
        while (waitUs > e && !maxEverUs.compare_exchange_weak(e, waitUs, std::memory_order_relaxed)) {
        }
    }
    // One census line for the 5 s summary ("spin: <name>: ..."); resets the window's longest wait. Returns the length.
    int Line(const char* name, char* buf, unsigned cap) {
        const unsigned w = waits.load(std::memory_order_relaxed), s = spunOut.load(std::memory_order_relaxed);
        const unsigned t = takeOvers.load(std::memory_order_relaxed), mx = maxUs.exchange(0, std::memory_order_relaxed);
        const unsigned e = maxEverUs.load(std::memory_order_relaxed), tot = totalUs.load(std::memory_order_relaxed);
        return snprintf(buf, cap, "spin: %s: %u waits, %u spins ran out, %u take-overs, longest %u us (window), %u us (ever), %u us waited in all", name, w,
                        s, t, mx, e, tot);
    }
};

inline uint32_t WaitEnv(const char* name, uint32_t fallback, uint32_t lo, uint32_t hi) {
    const char* e = getenv(name);
    long v = e && *e ? strtol(e, nullptr, 0) : static_cast< long >(fallback);
    if (v < static_cast< long >(lo)) {
        v = lo;
    }
    if (v > static_cast< long >(hi)) {
        v = hi;
    }
    return static_cast< uint32_t >(v);
}
inline uint32_t WaitBoundSpinUs() {
    static const uint32_t v = WaitEnv("GXV_BOUND_SPIN_US", 100, 0, 2000);
    return v;
}
inline uint32_t WaitBoundNapUs() {
    static const uint32_t v = WaitEnv("GXV_BOUND_NAP_US", 50, 10, 2000);
    return v;
}

// Waits while waiting() is true (it is evaluated first, so a false first answer costs one call and no clock read is made after it).
// spinUs == 0: the old pure spin. Returns the time waited in microseconds and notes it in `c`.
template < class Waiting >
inline uint32_t BoundedWait(Waiting waiting, WaitCensus& c, uint32_t spinUs, uint32_t napUs) {
    if (!waiting()) {
        return 0;
    }
    const uint64_t t0 = NowUs();
    bool out = false;
    uint32_t polls = 0;
    do {
        if (spinUs == 0 || !out) {
            CpuRelax();
            // (the clock is a kernel call on the console: read it every 16 polls)
            if (spinUs != 0 && (++polls & 15u) == 0 && NowUs() - t0 >= spinUs) {
                out = true;
            }
        } else {
            NapUs(napUs);
        }
    } while (waiting());
    const uint32_t us = static_cast< uint32_t >(NowUs() - t0);
    c.Note(us, out);
    return us;
}

inline WaitCensus& PlantWaitCensus() {
    static WaitCensus c;
    return c;
}
inline WaitCensus& AsyncWaitCensus() {  // (par_async.h Ensure: the draw's wait for a view job the worker is in)
    static WaitCensus c;
    return c;
}
inline WaitCensus& PoolWaitCensus() {  // (JPAEmitterManager.cpp's particle pool lock)
    static WaitCensus c;
    return c;
}
inline WaitCensus& SpecWaitCensus() {
    static WaitCensus c;
    return c;
}

}  // namespace par
}  // namespace port
