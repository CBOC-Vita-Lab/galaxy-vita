#pragma once
#include "port/mtx_profile.h"

#if PORT_MTX_PROFILE_COMPILED
#include <atomic>
#include <cerrno>
#include <cstring>
#include "../../vgx/src/vgx_common.h"

namespace mtxprofile {
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
constexpr unsigned kThreads = 32, kOverflow = kThreads + 1;
constexpr unsigned kSlotMask = 63, kRepeat = 64, kWork = 128, kGuest = 256;
struct Counts {
    std::atomic<uint64_t> calls{0}, repeats{0}, singular{0}, work{0}, workRepeats{0}, workSingular{0}, guest{0};
};
Counts counts[kOverflow];
std::atomic<unsigned> assigned{0};
std::atomic<uint64_t> overflowThreads{0}, turns{0};
struct Thread {
    unsigned slot = 0;
    bool have = false, active = false;
    unsigned char previous[48]{};
};
thread_local Thread thread;

bool Enabled() {
    // -2 is a short first-use initialization, not a mutex or an allocation.
    static std::atomic<int> mode{-1};
    int value = mode.load(std::memory_order_acquire);
    if (value >= 0) return value == 1;
    if (value == -1 && mode.compare_exchange_strong(value, -2, std::memory_order_acq_rel)) {
        const int savedErrno = errno;
        // Default-off native inverse-call/repeat diagnostics; read once, never a cache.
        const bool on = vgx::EnvInt("GXV_MTXPROFILE", 0) == 1;
        errno = savedErrno;
        mode.store(on ? 1 : 0, std::memory_order_release);
        return on;
    }
    do { value = mode.load(std::memory_order_acquire); } while (value < 0);
    return value == 1;
}

unsigned Slot() {
    if (thread.slot) return thread.slot;
    unsigned n = assigned.load(std::memory_order_relaxed);
    while (n < kThreads) {
        if (assigned.compare_exchange_weak(n, n + 1, std::memory_order_relaxed)) return thread.slot = n + 1;
    }
    overflowThreads.fetch_add(1, std::memory_order_relaxed);
    return thread.slot = kOverflow;
}

void Report(uint64_t completedTurns) {
    port_log_summary("mtx-profile cumulative turns=%llu threads=%u overflow-threads=%llu",
                     static_cast<unsigned long long>(completedTurns), assigned.load(std::memory_order_relaxed),
                     static_cast<unsigned long long>(overflowThreads.load(std::memory_order_relaxed)));
    for (unsigned i = 0; i < kOverflow; ++i) {
        const Counts& c = counts[i];
        const uint64_t calls = c.calls.load(std::memory_order_relaxed);
        if (!calls) continue;
        // Rolling cumulative snapshots, not a cross-thread coherent transaction.
        const auto repeats = static_cast<unsigned long long>(c.repeats.load(std::memory_order_relaxed));
        const auto singular = static_cast<unsigned long long>(c.singular.load(std::memory_order_relaxed));
        const auto work = static_cast<unsigned long long>(c.work.load(std::memory_order_relaxed));
        const auto workRepeats = static_cast<unsigned long long>(c.workRepeats.load(std::memory_order_relaxed));
        const auto workSingular = static_cast<unsigned long long>(c.workSingular.load(std::memory_order_relaxed));
        const auto guest = static_cast<unsigned long long>(c.guest.load(std::memory_order_relaxed));
        if (i + 1 == kOverflow)
            port_log_summary("mtx-profile cumulative thread=overflow calls=%llu repeats=%llu singular=%llu work=%llu work-repeats=%llu work-singular=%llu guest=%llu",
                             static_cast<unsigned long long>(calls), repeats, singular, work, workRepeats, workSingular, guest);
        else
            port_log_summary("mtx-profile cumulative thread=%u calls=%llu repeats=%llu singular=%llu work=%llu work-repeats=%llu work-singular=%llu guest=%llu",
                             i + 1, static_cast<unsigned long long>(calls), repeats, singular, work, workRepeats, workSingular, guest);
    }
}
} // namespace mtxprofile

extern "C" unsigned port_mtx_profile_begin(const void* matrix) {
    using namespace mtxprofile;
    if (!Enabled()) return 0;
    unsigned token = Slot();
    if (thread.have && std::memcmp(thread.previous, matrix, 48) == 0) token |= kRepeat;
    std::memcpy(thread.previous, matrix, 48);
    thread.have = true;
    if (thread.active) token |= kWork;
    if (port_is_game_thread()) token |= kGuest;
    return token;
}

extern "C" void port_mtx_profile_end(unsigned token, unsigned result) {
    using namespace mtxprofile;
    Counts& c = counts[(token & kSlotMask) - 1];
    if (token & kRepeat) c.repeats.fetch_add(1, std::memory_order_relaxed);
    if (!result) c.singular.fetch_add(1, std::memory_order_relaxed);
    if (token & kWork) {
        c.work.fetch_add(1, std::memory_order_relaxed);
        if (token & kRepeat) c.workRepeats.fetch_add(1, std::memory_order_relaxed);
        if (!result) c.workSingular.fetch_add(1, std::memory_order_relaxed);
    }
    if (token & kGuest) c.guest.fetch_add(1, std::memory_order_relaxed);
    c.calls.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void port_mtx_profile_work_begin(void) {
    if (mtxprofile::Enabled()) mtxprofile::thread.active = true;
}

extern "C" void port_mtx_profile_work_done(void) {
    using namespace mtxprofile;
    if (!Enabled() || !thread.active) return;
    thread.active = false;
    const uint64_t n = turns.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & 255u) == 0) Report(n);
}
#endif
