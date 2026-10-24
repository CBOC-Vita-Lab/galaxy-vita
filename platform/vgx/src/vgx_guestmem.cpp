// Guest RAM bookkeeping for the caches: which host ranges are guest memory,
// and a write-generation counter per 4 KiB of it.
//
// The console's graphics processor read display lists, vertex arrays and
// textures from physical RAM, so a game has to flush the CPU data cache over
// anything it changed before drawing with it. The runtime's HLE of those
// cache calls (and of disc reads and similar bulk writes) reports the range
// here. A cached decode of guest data stays valid while the counters under it
// have not moved; when they have, the bytes are compared (hashed) again.
#include "vgx_guestmem.h"
#include "vgx_hle.h"

#include <atomic>
#include <memory>

namespace vgx {

namespace {
constexpr int kShift = 12;
struct Bank {
    const uint8_t* base = nullptr;
    uint32_t size = 0;
    std::unique_ptr<std::atomic<uint32_t>[]> gen;
};
constexpr int kMaxBanks = 16; // (Galaxy: two RAM banks and each loaded segment of the executable)
Bank g_banks[kMaxBanks];
int g_numBanks = 0;
std::atomic<uint64_t> g_epoch{0}; // bumped by every write notification

// The last notifications, numbered by the epoch they took (a writer claims its number first and fills
// the slot after: `stamp` is number + 1 once the slot holds it, 0 while it is being rewritten).
constexpr uint32_t kLogSize = 2048;
struct LogRec {
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> stamp{0};
    std::atomic<const uint8_t*> p{nullptr};
    std::atomic<uint32_t> n{0};
};
LogRec g_log[kLogSize];

inline const Bank* FindBank(const uint8_t* p, uint32_t n) {
    for (int i = 0; i < g_numBanks; ++i) {
        const Bank& b = g_banks[i];
        if (p >= b.base && static_cast<uint64_t>(p - b.base) + n <= b.size) return &b;
    }
    return nullptr;
}
// The front end's lookups (two thousand a picture: display lists, vertex arrays, textures) nearly all land in the bank the
// last one found: that one is tried first. (Only a hint: the range is checked as in FindBank.)
std::atomic<int> g_readerBank{0};
inline const Bank* FindBankReader(const uint8_t* p, uint32_t n) {
    const int h = g_readerBank.load(std::memory_order_relaxed);
    if (h < g_numBanks) {
        const Bank& b = g_banks[h];
        if (p >= b.base && static_cast<uint64_t>(p - b.base) + n <= b.size) return &b;
    }
    const Bank* b = FindBank(p, n);
    if (b) g_readerBank.store(static_cast<int>(b - g_banks), std::memory_order_relaxed);
    return b;
}
} // namespace

uint64_t GuestWriteGeneration(const void* ptr, uint32_t n) {
    if (n == 0) return kNotGuest;
    const uint8_t* p = static_cast<const uint8_t*>(ptr);
    const Bank* b = FindBankReader(p, n);
    if (!b) return kNotGuest;
    const uint32_t first = static_cast<uint32_t>(p - b->base) >> kShift, last = static_cast<uint32_t>(p - b->base + n - 1) >> kShift;
    uint64_t sum = 0;
    for (uint32_t i = first; i <= last; ++i) sum += b->gen[i].load(std::memory_order_relaxed);
    return sum; // counters only grow, so the sum changes whenever one of them does
}
bool IsGuestRam(const void* ptr, uint32_t n) { return FindBankReader(static_cast<const uint8_t*>(ptr), n ? n : 1) != nullptr; }
bool HaveGuestRam() { return g_numBanks != 0; }
uint64_t GuestWriteEpoch() { return g_epoch.load(std::memory_order_acquire); }

bool GuestWritesTouch(uint64_t from, uint64_t to, const HostRange* ranges, int count) {
    if (to <= from) return false;
    if (to - from > kLogSize / 2) return true; // some of them are gone by now
    for (uint64_t seq = from; seq < to; ++seq) {
        const LogRec& r = g_log[seq & (kLogSize - 1)];
        const uint64_t s1 = r.stamp.load(std::memory_order_acquire);
        if (s1 != seq + 1) return true;
        const uint8_t* p = r.p.load(std::memory_order_relaxed);
        const uint32_t n = r.n.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t s2 = r.stamp.load(std::memory_order_relaxed);
        if (s1 != seq + 1 || s2 != seq + 1) return true; // not filled yet (or already reused)
        const uint8_t* const wEnd = p + n;
        for (int i = 0; i < count; ++i) {
            const uint8_t* a = static_cast<const uint8_t*>(ranges[i].p);
            if (p < a + ranges[i].n && a < wEnd) return true;
        }
    }
    return false;
}

namespace hle {
void RegisterGuestRam(const void* hostBase, uint32_t size) {
    if (!hostBase || !size || g_numBanks >= kMaxBanks) return;
    Bank& b = g_banks[g_numBanks];
    b.base = static_cast<const uint8_t*>(hostBase);
    b.size = size;
    const size_t n = (static_cast<size_t>(size) >> kShift) + 1;
    b.gen.reset(new std::atomic<uint32_t>[n]);
    for (size_t i = 0; i < n; ++i) b.gen[i].store(1, std::memory_order_relaxed);
    ++g_numBanks; // published last: readers on the render thread start after initialisation
}
void NotifyGuestWrite(const void* hostPtr, uint32_t size) {
    if (!size) return;
    const uint8_t* p = static_cast<const uint8_t*>(hostPtr);
    const Bank* b = FindBank(p, size);
    if (!b) return;
    const uint32_t first = static_cast<uint32_t>(p - b->base) >> kShift, last = static_cast<uint32_t>(p - b->base + size - 1) >> kShift;
    // Pair with cache-reader acquire fences when they observe a new page generation.
    std::atomic_thread_fence(std::memory_order_release);
    for (uint32_t i = first; i <= last; ++i) b->gen[i].fetch_add(1, std::memory_order_relaxed);
    const uint64_t seq = g_epoch.fetch_add(1, std::memory_order_acq_rel);
    LogRec& r = g_log[seq & (kLogSize - 1)];
    // A lapped, paused writer must not corrupt a newer record. Missing records
    // invalidate conservatively; never block a notification waiting for a slot.
    if (!r.busy.test_and_set(std::memory_order_acquire)) {
        r.stamp.store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        r.p.store(p, std::memory_order_relaxed);
        r.n.store(size, std::memory_order_relaxed);
        r.stamp.store(seq + 1, std::memory_order_release);
        r.busy.clear(std::memory_order_release);
    }
}
} // namespace hle

} // namespace vgx
