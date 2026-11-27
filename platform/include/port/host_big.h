// heap23 (9 Nov 2026): large host `new` blocks outside the C heap (GXV_HEAP_BIG), the core shared by the console
// (platform/src/os/os_thread.cpp, sceKernelAllocMemBlock) and the host tests (tests/heap_soak.cpp, a simulated provider).
//
// Why. The console's C heap is newlib's dlmalloc in one fixed 96 MiB block. A stage cycle frees and allocates blocks of
// hundreds of KiB to a few MiB (decoded vertices, texture levels, upload copies, particle work areas, frame-arena steps) while
// small blocks that live much longer (the renderer's shaders and programs, vitaGL's objects, memo nodes, id tables) are made
// in between. The small ones settle in the holes the big ones leave, so after a few stages the free space is tens of
// thousands of small pieces: the c20 soak died entering its 7th stage with 15 MiB free in 45,483 pieces and no room for one
// 1,359 KiB `new` (FATAL: host heap exhausted). A best-fit allocator cannot avoid that; only keeping the two populations
// apart does.
//
// What. A `new` of at least `minBytes` gets its own block of whole pages from the provider (on the console a USER_RW
// memblock: cached main memory like the C heap's, page-mapped, so it needs no contiguous room anywhere). Freed blocks wait in
// a small cache (best fit within a quarter of the size) so a buffer freed and asked for again in the same stage costs no
// system call. A block is refused (the caller falls back to the C heap, as before) when the table is full, the alignment is
// over a page, or the system's free main memory would drop under `floorBytes`; the cache is given back first.
//
// No allocation inside (fixed tables), no exceptions; one mutex around the tables. The provider is a class with:
//   bool Map(size_t bytes, uintptr_t& base, int32_t& handle);   // whole pages, page-aligned base
//   void Unmap(uintptr_t base, int32_t handle, size_t bytes);
//   size_t FreeMemory();                                          // the system's free main memory (SIZE_MAX: unknown)
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace port {
namespace bigheap {

constexpr size_t kPage = 4096;
constexpr unsigned kLiveCap = 1024;  // live blocks (a power of two: open addressing)
constexpr unsigned kCacheCap = 8;    // freed blocks kept for reuse

struct Stats {
    unsigned liveBlocks = 0, cacheBlocks = 0, peakBlocks = 0;
    size_t liveBytes = 0, cacheBytes = 0, peakBytes = 0;
    uint64_t maps = 0, unmaps = 0, reuses = 0, refusedFloor = 0, refusedFull = 0, mapFailures = 0;
};

template <class Provider> class BigHeap {
public:
    // (constexpr: a global BigHeap is constant-initialised, usable by any static initialiser's `new`)
    constexpr explicit BigHeap(Provider& p) : prov_(p) {}

    // A block for `size` bytes aligned to `align`, or nullptr (the caller uses the C heap). `floorBytes`: the free main memory
    // that must be left to the system after the block is mapped (0: no limit).
    // `bytesOut`: the block's size (whole pages; a reused block may be up to a quarter larger than asked).
    void* Alloc(size_t size, size_t align, size_t floorBytes, size_t* bytesOut = nullptr) {
        if (align > kPage || size == 0 || size > (size_t(1) << 30)) return nullptr;
        const size_t bytes = RoundPage(size);
        std::lock_guard<std::mutex> lk(mtx_);
        if (st_.liveBlocks >= kLiveCap * 3 / 4) { ++st_.refusedFull; return nullptr; }
        // a freed block of about this size: best fit, at most a quarter (and 64 KiB) larger
        int best = -1;
        for (unsigned i = 0; i < kCacheCap; ++i) {
            const Slot& c = cache_[i];
            if (!c.base || c.bytes < bytes || c.bytes - bytes > bytes / 4 + 65536) continue;
            if (best < 0 || c.bytes < cache_[best].bytes) best = static_cast<int>(i);
        }
        Slot s{};
        if (best >= 0) {
            s = cache_[best];
            cache_[best] = Slot{};
            --st_.cacheBlocks;
            st_.cacheBytes -= s.bytes;
            ++st_.reuses;
        } else {
            if (!RoomFor(bytes, floorBytes)) { ++st_.refusedFloor; return nullptr; }
            uintptr_t base = 0;
            int32_t handle = 0;
            bool mapped = prov_.Map(bytes, base, handle) && base;
            if (!mapped && st_.cacheBlocks) {
                DropCacheLocked();  // (the system had no such block: give the cache back and try once more)
                base = 0;
                mapped = prov_.Map(bytes, base, handle) && base;
            }
            if (!mapped) { ++st_.mapFailures; return nullptr; }
            ++st_.maps;
            s.base = base;
            s.bytes = bytes;
            s.handle = handle;
        }
        Insert(s);
        ++st_.liveBlocks;
        st_.liveBytes += s.bytes;
        if (st_.liveBlocks > st_.peakBlocks) st_.peakBlocks = st_.liveBlocks;
        if (st_.liveBytes > st_.peakBytes) st_.peakBytes = st_.liveBytes;
        if (bytesOut) *bytesOut = s.bytes;
        return reinterpret_cast<void*>(s.base);
    }

    // Whether p is one of ours (then it is freed or cached and its size returned in *bytes). `cacheMax`: how many bytes of freed
    // blocks may wait for reuse (0: none).
    bool Free(void* p, size_t cacheMax, size_t* bytesOut = nullptr) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(p);
        if (!base || (base & (kPage - 1))) return false;  // (ours start a page: anything else is the C heap's, without the lock)
        Slot s{};
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!Remove(base, s)) return false;
            --st_.liveBlocks;
            st_.liveBytes -= s.bytes;
            if (bytesOut) *bytesOut = s.bytes;
            if (s.bytes <= cacheMax) {
                // room in the cache: the oldest blocks go first
                while (st_.cacheBlocks && (st_.cacheBlocks >= kCacheCap || st_.cacheBytes + s.bytes > cacheMax)) EvictOldestLocked();
                for (unsigned i = 0; i < kCacheCap; ++i) {
                    if (cache_[i].base) continue;
                    s.stamp = ++stamp_;
                    cache_[i] = s;
                    ++st_.cacheBlocks;
                    st_.cacheBytes += s.bytes;
                    return true;
                }
            }
            ++st_.unmaps;
        }
        prov_.Unmap(s.base, s.handle, s.bytes);
        return true;
    }

    // The bytes behind p if it is ours, else 0.
    size_t SizeOf(const void* p) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(p);
        if (!base || (base & (kPage - 1))) return 0;
        std::lock_guard<std::mutex> lk(mtx_);
        const int i = Find(base);
        return i < 0 ? 0 : live_[i].bytes;
    }

    // Give every cached block back (a scene's end; low memory).
    void DropCache() {
        std::lock_guard<std::mutex> lk(mtx_);
        DropCacheLocked();
    }

    Stats Get() {
        std::lock_guard<std::mutex> lk(mtx_);
        return st_;
    }

    static size_t RoundPage(size_t n) { return (n + kPage - 1) & ~(kPage - 1); }

private:
    struct Slot { uintptr_t base = 0; size_t bytes = 0; int32_t handle = 0; uint64_t stamp = 0; };

    bool RoomFor(size_t bytes, size_t floorBytes) {
        if (!floorBytes) return true;
        size_t free = prov_.FreeMemory();
        if (free == SIZE_MAX) return true;
        if (free >= bytes + floorBytes) return true;
        if (!st_.cacheBlocks) return false;
        DropCacheLocked();  // (cached blocks are the system's memory too)
        free = prov_.FreeMemory();
        return free != SIZE_MAX && free >= bytes + floorBytes;
    }
    void EvictOldestLocked() {
        int old = -1;
        for (unsigned i = 0; i < kCacheCap; ++i)
            if (cache_[i].base && (old < 0 || cache_[i].stamp < cache_[old].stamp)) old = static_cast<int>(i);
        if (old < 0) return;
        const Slot s = cache_[old];
        cache_[old] = Slot{};
        --st_.cacheBlocks;
        st_.cacheBytes -= s.bytes;
        ++st_.unmaps;
        prov_.Unmap(s.base, s.handle, s.bytes);  // (under the lock: rare, and the provider takes no lock of ours)
    }
    void DropCacheLocked() {
        while (st_.cacheBlocks) EvictOldestLocked();
    }

    static unsigned Hash(uintptr_t base) { return static_cast<unsigned>((base >> 12) * 2654435761u) & (kLiveCap - 1); }
    int Find(uintptr_t base) const {
        unsigned i = Hash(base);
        for (unsigned n = 0; n < kLiveCap; ++n, i = (i + 1) & (kLiveCap - 1)) {
            if (!live_[i].base) return -1;
            if (live_[i].base == base) return static_cast<int>(i);
        }
        return -1;
    }
    void Insert(const Slot& s) {
        unsigned i = Hash(s.base);
        while (live_[i].base) i = (i + 1) & (kLiveCap - 1);  // (never full: at most 3/4 used)
        live_[i] = s;
    }
    // Linear probing with backward-shift deletion: no tombstones, so lookups stay short however long the run.
    bool Remove(uintptr_t base, Slot& out) {
        int at = Find(base);
        if (at < 0) return false;
        out = live_[at];
        unsigned hole = static_cast<unsigned>(at);
        unsigned j = hole;
        for (;;) {
            j = (j + 1) & (kLiveCap - 1);
            if (!live_[j].base) break;
            const unsigned home = Hash(live_[j].base);
            // move j into the hole when its home is not within (hole, j]
            const bool between = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
            if (!between) {
                live_[hole] = live_[j];
                hole = j;
            }
        }
        live_[hole] = Slot{};
        return true;
    }

    Provider& prov_;
    std::mutex mtx_;
    Slot live_[kLiveCap];
    Slot cache_[kCacheCap];
    Stats st_;
    uint64_t stamp_ = 0;
};

}  // namespace bigheap
}  // namespace port
