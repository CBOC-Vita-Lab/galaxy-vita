// heap23 (9 Nov 2026): GXV_HEAP_BIG on the host. tools/test_heap_soak.sh builds and runs it.
//
// Part 1, the core (platform/include/port/host_big.h, the code the console runs) with a simulated page provider:
//   - blocks are whole pages, page-aligned, freed or cached; the cache takes a block within a quarter of the size and is bounded;
//   - the free-memory floor refuses a block (after giving the cache back) and the caller's fallback is used;
//   - the live table (open addressing, backward-shift deletion) agrees with a reference map through 200,000 random operations;
//   - a full table refuses instead of failing; four threads hammering it leave nothing mapped once the cache is dropped.
//
// Part 2, a soak of 100 stage cycles (the console's PETARI_FLOW loop of 7 galaxies) through a model of the console's C heap: one
// fixed 96 MiB block, best-fit with coalescing and a top chunk that grows by sbrk (what newlib's dlmalloc does, as address
// bookkeeping only). The workload is sized from the c20 soak's memory lines (690-soak-c20-log.txt):
//   start-up: ~29 MiB of small long-lived blocks (the port's and vitaGL's), the shader pack (2.4 MiB), the four frame buffers' arena
//             floors (4 x 6.9 MiB, made at start-up and never given back);
//   each stage entry: ~8 MiB of small stage blocks and ~8 MiB of stage blocks of 128 KiB - 2 MiB (decoded lists, upload copies,
//             particle work areas), half of those given back only during the next stage (the display-list cache's 600-frame
//             sweep); upload / decode temporaries of 128 KiB - 1.5 MiB that live a few steps while small blocks keep coming; on the
//             first visit of a galaxy ~0.6 MiB of long-lived small blocks (shaders, programs, warm-up records), capped at 10 MiB
//             (vitaGL's slots) with the oldest replaced; the texture-id tables growing ~150 ids a stage (40 B each, doubling);
//   play: 600 frames of small churn and a few uploads.
// Checked with GXV_HEAP_BIG=1 (blocks of 128 KiB and more outside the C heap from the first scene change, the console's 42 MiB of
// free main memory and its 16 MiB floor):
//   - no request fails in 100 cycles, and no block of 128 KiB or more falls back to the C heap;
//   - zero growth across loops: the C heap in use and the outside bytes at a scene's destroy are the same, loop after loop (after
//     the first loop has seen every galaxy), within 16 KiB a loop on average;
//   - the C heap's largest free piece stays at least HEAP_SOAK_MIN_LARGEST_KIB (default 8192) at every scene destroy.
// And the negative control, GXV_HEAP_BIG=0 (everything in the C heap, as c20): the same workload must run out of room (a request with
// no free piece large enough although megabytes are free in total) or end with the largest free piece under 1.4 MiB.
//   HEAP_SOAK_CYCLES=<n> (default 100), HEAP_SOAK_SEED=<n>, HEAP_SOAK_VERBOSE=1 (a line per scene destroy).
//
// Part 3 (heap23b), the c24 soak's slow growth: the same soak with the front end's state-list store modelled (entries keyed by list
// address, made at a list's first call; a galaxy's lists partly at other addresses on each visit), GXV_HEAP_BIG=1:
//   GXV_SL_SCENEDROP=0 (c24): the C heap grows loop over loop, hundreds of KiB a loop and shrinking, as c24's did;
//   GXV_SL_SCENEDROP=1: a scene's entries freed at its destroy: no growth from loop 4 on (<= 16 KiB a loop on average).
//   HEAP_SOAK_SL_CYCLES=<n> (default 56), HEAP_SOAK_SL_LISTS / _MOVED_PERMILLE / _POOL (the calibration), HEAP_SOAK_LONG_CYCLES=<n> (a long
//   run of each mode, printed only: the largest free piece and the growth over its last half).
#include "port/host_big.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

namespace {
unsigned g_checks = 0;
void Require(bool v, const char* what) {
    ++g_checks;
    if (!v) {
        std::fprintf(stderr, "heap_soak FAILED: %s\n", what);
        std::exit(1);
    }
}
long EnvL(const char* n, long d) {
    const char* e = std::getenv(n);
    return e && *e ? std::strtol(e, nullptr, 0) : d;
}

// ---------------------------------------------------------------- a page provider with a budget (the console's free main memory)
struct SimPages {
    size_t budget = 42u << 20;  // free main memory before any block
    std::atomic<size_t> mapped{0};
    std::atomic<uint64_t> maps{0}, unmaps{0};
    bool failNext = false;
    std::mutex liveMtx;
    std::set<void*> live;  // (what is still mapped when the provider goes: a soak that stops early leaves its blocks)
    ~SimPages() {
        for (void* p : live) std::free(p);
    }
    bool Map(size_t bytes, uintptr_t& base, int32_t& handle) {
        if (failNext) { failNext = false; return false; }
        if ((bytes & (port::bigheap::kPage - 1)) != 0) return false;
        if (mapped.load() + bytes > budget) return false;
        void* p = std::aligned_alloc(port::bigheap::kPage, bytes);
        if (!p) return false;
        std::memset(p, 0xA5, std::min<size_t>(bytes, 64));
        mapped += bytes;
        ++maps;
        {
            std::lock_guard<std::mutex> lk(liveMtx);
            live.insert(p);
        }
        base = reinterpret_cast<uintptr_t>(p);
        handle = 7;
        return true;
    }
    void Unmap(uintptr_t base, int32_t handle, size_t bytes) {
        Require(handle == 7, "provider: handle given back");
        mapped -= bytes;
        ++unmaps;
        {
            std::lock_guard<std::mutex> lk(liveMtx);
            live.erase(reinterpret_cast<void*>(base));
        }
        std::free(reinterpret_cast<void*>(base));
    }
    size_t FreeMemory() { return budget - mapped.load(); }
};
using Big = port::bigheap::BigHeap<SimPages>;

// ---------------------------------------------------------------- part 1
void TestCore() {
    {
        SimPages pages;
        Big big(pages);
        void* a = big.Alloc(200 * 1024 + 5, 64, 0);
        Require(a && (reinterpret_cast<uintptr_t>(a) & 4095) == 0, "a block is page-aligned");
        Require(big.SizeOf(a) == Big::RoundPage(200 * 1024 + 5), "its size is whole pages");
        {
            size_t got = 0;
            void* x = big.Alloc(300 * 1024, 16, 0, &got);
            Require(x && got == Big::RoundPage(300 * 1024), "the size is reported");
            big.Free(x, 8u << 20);
            void* y = big.Alloc(290 * 1024, 16, 0, &got);
            Require(y == x && got == Big::RoundPage(300 * 1024), "a reused block reports its own (larger) size");
            size_t freed = 0;
            Require(big.Free(y, 0, &freed) && freed == got, "and frees the same size");
        }
        std::memset(a, 1, 200 * 1024 + 5);
        Require(big.Alloc(1000, 8192, 0) == nullptr, "an alignment over a page is refused");
        size_t bytes = 0;
        Require(big.Free(a, 8u << 20, &bytes) && bytes == Big::RoundPage(200 * 1024 + 5), "freed into the cache");
        Require(big.Get().cacheBlocks == 1 && pages.mapped == bytes, "the cache keeps it mapped");
        void* b = big.Alloc(190 * 1024, 16, 0);
        Require(b == a && big.Get().reuses == 2, "a slightly smaller request reuses it");
        Require(big.Free(b, 8u << 20), "freed again");
        void* c = big.Alloc(100 * 1024, 16, 0);
        Require(c != a, "a request far smaller does not take a much larger cached block");
        Require(!big.Free(reinterpret_cast<char*>(c) + 16, 0), "a pointer that is not a block start is not ours");
        int local = 0;
        Require(!big.Free(&local, 0), "a foreign pointer is not ours");
        Require(big.Free(c, 0) && big.Get().cacheBlocks == 1, "cacheMax 0: given back at once");
        big.DropCache();
        Require(pages.mapped == 0 && big.Get().cacheBlocks == 0, "DropCache gives everything back");
    }
    {
        // the cache: bounded in bytes and count, oldest out first
        SimPages pages;
        Big big(pages);
        std::vector<void*> v;
        for (int i = 0; i < 12; ++i) v.push_back(big.Alloc((256 + 16 * i) * 1024, 16, 0));
        for (void* p : v) Require(big.Free(p, 2u << 20), "free");
        const port::bigheap::Stats s = big.Get();
        Require(s.cacheBytes <= (2u << 20) && s.cacheBlocks <= port::bigheap::kCacheCap, "the cache stays within its bounds");
        Require(pages.mapped == s.cacheBytes, "everything not cached is unmapped");
        big.DropCache();
        Require(pages.mapped == 0, "nothing left");
    }
    {
        // the floor: a block that would leave less free memory than the floor is refused, after the cache is given back
        SimPages pages;
        pages.budget = 10u << 20;
        Big big(pages);
        void* a = big.Alloc(5u << 20, 16, 4u << 20);
        Require(a != nullptr, "5 MiB of 10 with a 4 MiB floor");
        void* b = big.Alloc(2u << 20, 16, 4u << 20);
        Require(b == nullptr && big.Get().refusedFloor == 1, "2 MiB more would go under the floor: refused");
        void* c = big.Alloc(1u << 20, 16, 4u << 20);
        Require(c != nullptr, "1 MiB more still leaves 4");
        Require(big.Free(c, 8u << 20) && big.Get().cacheBlocks == 1, "cached");
        void* d = big.Alloc(2u << 20, 16, 4u << 20);
        Require(d == nullptr, "the cached MiB is given back but 2 MiB still go under the floor");
        Require(big.Get().cacheBlocks == 0, "the cache was given back to try");
        void* e = big.Alloc(2u << 20, 16, 1u << 20);
        Require(e != nullptr, "the last-resort floor (1 MiB) lets it through");
        pages.failNext = true;
        Require(big.Alloc(1u << 20, 16, 0) == nullptr && big.Get().mapFailures == 1, "a system refusal is a refusal, not a crash");
        big.Free(a, 0);
        big.Free(e, 0);
        Require(pages.mapped == 0, "all given back");
    }
    {
        // the table against a reference: random allocations and frees, including runs that collide
        SimPages pages;
        pages.budget = size_t(1) << 34;
        Big big(pages);
        std::map<void*, size_t> ref;
        std::mt19937 rng(1234);
        for (int op = 0; op < 200000; ++op) {
            const bool alloc = ref.size() < 20 || (ref.size() < 700 && (rng() & 1));
            if (alloc) {
                const size_t sz = 4096 + (rng() % 64) * 4096;
                void* p = big.Alloc(sz, 16, 0);
                if (ref.size() >= port::bigheap::kLiveCap * 3 / 4) { Require(p == nullptr, "a full table refuses"); continue; }
                Require(p != nullptr && !ref.count(p), "a new block");
                const size_t got = big.SizeOf(p);
                Require(got >= Big::RoundPage(sz) && got <= Big::RoundPage(sz) + Big::RoundPage(sz) / 4 + 65536, "its size (a reused block within a quarter)");
                ref[p] = got;
            } else {
                auto it = ref.begin();
                std::advance(it, rng() % ref.size());
                size_t bytes = 0;
                Require(big.Free(it->first, (rng() & 3) ? 0 : (4u << 20), &bytes) && bytes == it->second, "freed with its size");
                ref.erase(it);
            }
            if ((op & 1023) == 0)
                for (const auto& kv : ref) Require(big.SizeOf(kv.first) == kv.second, "every live block is found");
        }
        for (const auto& kv : ref) Require(big.Free(kv.first, 0), "free the rest");
        big.DropCache();
        Require(pages.mapped == 0 && big.Get().liveBlocks == 0, "table empty, nothing mapped");
        // fill the table up: 3/4 of the slots, then refusals
        std::vector<void*> v;
        for (unsigned i = 0; i < port::bigheap::kLiveCap; ++i) if (void* p = big.Alloc(4096, 16, 0)) v.push_back(p);
        Require(v.size() == port::bigheap::kLiveCap * 3 / 4 && big.Get().refusedFull > 0, "the table holds 3/4 of its slots");
        for (void* p : v) big.Free(p, 0);
        Require(pages.mapped == 0, "and gives them all back");
    }
    {
        // threads
        SimPages pages;
        pages.budget = size_t(1) << 34;
        Big big(pages);
        std::atomic<unsigned> bad{0};
        std::vector<std::thread> th;
        for (int t = 0; t < 4; ++t)
            th.emplace_back([&, t] {
                std::mt19937 rng(77 + t);
                std::vector<std::pair<void*, size_t>> mine;
                for (int op = 0; op < 20000; ++op) {
                    if (mine.size() < 30 && (mine.empty() || (rng() & 1))) {
                        const size_t sz = 128 * 1024 + (rng() % 32) * 8192;
                        if (void* p = big.Alloc(sz, 16, 0)) {
                            std::memset(p, t, 64);
                            mine.emplace_back(p, sz);
                        }
                    } else {
                        const size_t i = rng() % mine.size();
                        if (static_cast<unsigned char*>(mine[i].first)[63] != static_cast<unsigned char>(t)) ++bad;
                        if (!big.Free(mine[i].first, 4u << 20)) ++bad;
                        mine.erase(mine.begin() + static_cast<long>(i));
                    }
                }
                for (auto& m : mine) if (!big.Free(m.first, 4u << 20)) ++bad;
            });
        for (auto& t : th) t.join();
        big.DropCache();
        Require(bad == 0, "threads: every block intact and found");
        Require(pages.mapped == 0 && big.Get().liveBlocks == 0, "threads: nothing left mapped");
    }
    std::printf("core: %u checks PASS\n", g_checks);
}

// ---------------------------------------------------------------- part 2: the C heap model
// One fixed block (newlib's 96 MiB): free chunks by size (best fit, the lowest address among equals) and by address (coalescing);
// the top chunk grows by sbrk up to the block's end. A chunk has an 8-byte header and is a multiple of 8 bytes, at least 16.
class HeapModel {
public:
    explicit HeapModel(size_t cap) : cap_(cap) {}
    // offset of the user block, or SIZE_MAX
    size_t Alloc(size_t n) {
        size_t need = Chunk(n);
        auto it = bySize_.lower_bound({need, 0});
        size_t at;
        if (it != bySize_.end()) {
            at = it->second;
            const size_t sz = it->first;
            bySize_.erase(it);
            byAddr_.erase(at);
            if (sz - need >= 16) AddFree(at + need, sz - need);
            else need = sz;  // (a remainder too small for a chunk stays with the block)
        } else {
            // from the top (sbrk): a free chunk touching the top is merged first
            size_t start = brk_;
            if (!byAddr_.empty()) {
                auto last = std::prev(byAddr_.end());
                if (last->first + last->second == brk_) {
                    start = last->first;
                    bySize_.erase({last->second, last->first});
                    byAddr_.erase(last);
                }
            }
            if (start + need > cap_) {
                if (start != brk_) AddFree(start, brk_ - start);
                return SIZE_MAX;
            }
            at = start;
            brk_ = std::max(brk_, start + need);
        }
        used_[at] = need;
        inUse_ += need;
        return at;
    }
    void Free(size_t at) {
        auto u = used_.find(at);
        Require(u != used_.end(), "model: free of a block the model did not give");
        size_t sz = u->second;
        used_.erase(u);
        inUse_ -= sz;
        // coalesce
        auto next = byAddr_.find(at + sz);
        if (next != byAddr_.end()) {
            sz += next->second;
            bySize_.erase({next->second, next->first});
            byAddr_.erase(next);
        }
        auto prev = byAddr_.lower_bound(at);
        if (prev != byAddr_.begin()) {
            --prev;
            if (prev->first + prev->second == at) {
                at = prev->first;
                sz += prev->second;
                bySize_.erase({prev->second, prev->first});
                byAddr_.erase(prev);
            }
        }
        AddFree(at, sz);
    }
    size_t InUse() const { return inUse_; }
    size_t Arena() const { return brk_; }
    size_t FreeBytes() const { return brk_ - inUse_; }
    size_t Pieces() const { return byAddr_.size(); }
    // the largest free piece: the largest chunk, or the top one with the block's untouched end
    size_t Largest() const {
        size_t best = bySize_.empty() ? 0 : bySize_.rbegin()->first;
        size_t topRun = cap_ - brk_;
        if (!byAddr_.empty()) {
            auto last = std::prev(byAddr_.end());
            if (last->first + last->second == brk_) topRun += last->second;
        }
        return std::max(best, topRun);
    }

private:
    static size_t Chunk(size_t n) { return std::max<size_t>(16, (n + 8 + 7) & ~size_t(7)); }
    void AddFree(size_t at, size_t sz) {
        byAddr_[at] = sz;
        bySize_.insert({sz, at});
    }
    size_t cap_, brk_ = 0, inUse_ = 0;
    std::map<size_t, size_t> byAddr_;
    std::set<std::pair<size_t, size_t>> bySize_;
    std::map<size_t, size_t> used_;
};

// A block of the workload: in the C heap (offset) or outside (pointer).
struct Blk { size_t off = SIZE_MAX; void* out = nullptr; size_t size = 0; };

struct Soak {
    bool route;          // GXV_HEAP_BIG=1
    bool armed = false;  // from the first scene change
    size_t minBig = 128 * 1024, floor = 16u << 20, cache = 8u << 20;
    HeapModel heap{96u << 20};
    SimPages pages;
    Big big{pages};
    std::mt19937 rng;
    bool failed = false;
    size_t failSize = 0, failFree = 0, failPieces = 0, failLargest = 0;
    unsigned bigInHeap = 0;  // blocks of minBig or more that went to the C heap after arming

    unsigned seed;
    size_t idBytes = 0;  // the texture-id tables' blocks (the one known growth, reported apart)
    // heap23b: the front end's state-list store (vgx_state.cpp), keyed by list address. -1: not modelled (heap23's soak), 0: kept
    // across scenes (GXV_SL_SCENEDROP=0, c24), 1: a scene's entries freed at its destroy (GXV_SL_SCENEDROP=1)
    int slMode = -1;
    size_t slBytes = 0, slEntries = 0;
    Soak(bool r, unsigned sd, int sl = -1) : route(r), rng(sd), seed(sd), slMode(sl) {}

    bool Alloc(size_t size, Blk& b) {
        b = Blk{};
        b.size = size;
        if (route && armed && size >= minBig) {
            b.out = big.Alloc(size, 16, floor);
            if (b.out) return true;
        }
        b.off = heap.Alloc(size);
        if (b.off != SIZE_MAX) {
            if (route && armed && size >= minBig) ++bigInHeap;
            return true;
        }
        if (route) {
            b.out = big.Alloc(size, 16, 4u << 20);  // (the last resort: GXV_HEAP_BIG_LAST_MIB)
            if (b.out) return true;
        }
        if (!failed) {
            failed = true;
            failSize = size;
            failFree = heap.FreeBytes();
            failPieces = heap.Pieces();
            failLargest = heap.Largest();
        }
        return false;
    }
    void Free(Blk& b) {
        if (b.out) {
            Require(big.Free(b.out, cache), "soak: an outside block is found");
        } else if (b.off != SIZE_MAX) {
            heap.Free(b.off);
        }
        b = Blk{};
    }
    // small sizes: most of them tiny (map and list nodes, strings, records: 16-256 B, log-uniform), a quarter 256 B - 8 KiB, a few
    // 8 - 96 KiB (decoded small lists, vectors). (The console's C heap had ~45,000 free pieces averaging ~400 B.)
    size_t Small() {
        std::uniform_real_distribution<double> u(0, 1);
        const double r = u(rng);
        auto logu = [&](double lo, double hi) { return static_cast<size_t>(std::exp(std::log(lo) + (std::log(hi) - std::log(lo)) * u(rng))); };
        if (r < 0.70) return logu(16, 256);
        if (r < 0.96) return logu(256, 8192);
        return logu(8192, 96 * 1024);
    }
    size_t Between(size_t lo, size_t hi) { return lo + rng() % (hi - lo + 1); }
};

struct Point { unsigned cycle, galaxy, loop; size_t inUse, outside, largest, pieces, freeBytes, arena, idBytes, slBytes, slEntries; };

// One run of `cycles` stage entries. Returns the scene-destroy points.
std::vector<Point> RunSoak(Soak& s, unsigned cycles, bool verbose) {
    std::vector<Point> pts;
    std::vector<Blk> persistent, title;
    Blk b;
    // start-up: small long-lived blocks mixed with the title's and file select's (gone at the first scene change), the shader pack, then
    // the arena floors (4 frame buffers: vertices, indices, uniforms, records)
    for (size_t total = 0, other = 0; total < (25u << 20);) {
        if (!s.Alloc(s.Small(), b)) return pts;
        total += b.size;
        persistent.push_back(b);
        if (other < (8u << 20) && (s.rng() % 3) == 0) {
            if (!s.Alloc(s.Small(), b)) return pts;
            other += b.size;
            title.push_back(b);
        }
    }
    if (!s.Alloc(2486484, b)) return pts;
    persistent.push_back(b);
    for (int f = 0; f < 4; ++f)
        for (size_t sz : {size_t(6144) << 10, size_t(640) << 10, size_t(256) << 10, size_t(200) << 10, size_t(64) << 10}) {
            if (!s.Alloc(sz, b)) return pts;
            persistent.push_back(b);
        }
    for (Blk& t : title) s.Free(t);
    s.armed = true;  // GXV_HEAP_BIG=1 arms at the first scene destroyed

    std::deque<Blk> longLived;       // shaders, programs, warm-up records: made on a galaxy's first visit, capped (vitaGL's slots)
    size_t longBytes = 0;
    const size_t longCap = 10u << 20;
    // cache-lived small blocks: display-list cache entries, texture-cache records, memo nodes, map nodes ... made during a stage and
    // given back one to a few stages later (an LRU or a sweep): what cuts the free space into tens of thousands of pieces
    std::vector<std::pair<unsigned, Blk>> cacheLived;
    std::geometric_distribution<unsigned> stagesLived(0.4);
    std::vector<Blk> idTables(4);    // the texture-id tables (back end g_tex, front end ranges ...): grow by doubling
    std::vector<size_t> idCap(4, 0);
    const size_t idBytesPerId[4] = {32, 9, 12, 1};
    size_t ids = 0;
    std::vector<Blk> prevSweep;      // the last stage's blocks the display-list cache sweeps during this stage
    std::vector<Blk> expired;        // cache-lived blocks whose time is up, given back during the load
    std::vector<bool> seen(7, false);
    // heap23b: the state-list store. Every material list a scene calls gets an entry at its first call: the entry (alignas(32), 128 B
    // + the allocator's 8) and its four vectors (the list's bytes, its operations, words and register deltas: ~1.0 KiB together; the
    // c24 soak's loop-over-loop growth of the game's `new` against the other users is 8 : 1, as these sizes give). A galaxy's lists sit
    // at the same addresses on every visit except a share that lands elsewhere (asynchronous loads fill the scene heap in another
    // order); an entry at an address the store has stays (the same list or another one parsed into it). The persistent lists (Mario,
    // the HUD) are outside the scene heaps. Sizes and shares calibrated to c24 (the growth of loops 2, 3, 4: +1.38, +1.13, +0.89 MiB).
    struct SlEnt { Blk part[5]; };
    std::map<uint32_t, SlEnt> slStore;                       // address -> entry (addresses >= kSlScene are in the scene heaps)
    const uint32_t kSlScene = 1u << 20;
    const unsigned slLists = static_cast<unsigned>(EnvL("HEAP_SOAK_SL_LISTS", 1000));
    const unsigned slMoved = static_cast<unsigned>(EnvL("HEAP_SOAK_SL_MOVED_PERMILLE", 220));
    const unsigned slPool = static_cast<unsigned>(EnvL("HEAP_SOAK_SL_POOL", 12000));
    const unsigned slPersistent = 300;
    std::mt19937 slRng(s.seed ^ 0x51ED2701u), slSizes(s.seed ^ 0x2F6B9A13u); // (layout and sizes: separate streams, the same layouts in every mode)
    auto slNew = [&](uint32_t addr) -> bool {
        if (s.slMode < 0 || slStore.count(addr)) return true;
        SlEnt e;
        auto between = [&](size_t lo, size_t hi) { return lo + slSizes() % (hi - lo + 1); }; // (its own stream: the replay of the rest stays)
        const size_t sizes[5] = {136, between(96, 512), between(64, 512), between(32, 384), between(64, 400)};
        for (int k = 0; k < 5; ++k) {
            if (!s.Alloc(sizes[k], e.part[k])) return false;
            s.slBytes += e.part[k].size;
        }
        ++s.slEntries;
        slStore.emplace(addr, e);
        return true;
    };
    auto slFree = [&](SlEnt& e) {
        for (Blk& x : e.part) { s.slBytes -= x.size; s.Free(x); }
        --s.slEntries;
    };
    for (unsigned cycle = 0; cycle < cycles && !s.failed; ++cycle) {
        const unsigned galaxy = cycle % 7;
        // (each visit of a galaxy replays the same requests, as the console's loop replays the same stages: a difference between
        // loops at the same point is state carried over, not chance)
        s.rng.seed(s.seed * 7919u + galaxy);
        std::vector<Blk> stageSmall, stageBig;
        std::deque<Blk> temps;
        auto tempStep = [&](unsigned keep) {
            while (temps.size() > keep) { s.Free(temps.front()); temps.pop_front(); }
        };
        auto cacheNew = [&](unsigned from) -> bool {
            if (!s.Alloc(s.Small(), b)) return false;
            cacheLived.emplace_back(from + 1 + stagesLived(s.rng), b);
            return true;
        };
        // load: ~6 MiB small, ~3 MiB cache-lived, ~8 MiB of stage blocks, temporaries, (first visit) long-lived blocks; the last stage's
        // swept blocks and the expired cache-lived ones go back during the load
        const size_t smallTarget = (5u << 20) + (s.rng() % (2u << 20));
        const size_t cacheTarget = (2u << 20) + (s.rng() % (2u << 20));
        const size_t bigTarget = (6u << 20) + (s.rng() % (4u << 20));
        size_t smallNow = 0, bigNow = 0, cacheNow = 0;
        const size_t longNew = seen[galaxy] ? 0 : (400u << 10) + s.rng() % (400u << 10);
        size_t longNow = 0;
        unsigned step = 0;
        std::vector<uint32_t> slVisit; // (heap23b) this visit's list addresses: persistent ones, the galaxy's homes, a share moved
        size_t slNext = 0;
        if (s.slMode >= 0) {
            for (uint32_t i = 0; i < slPersistent; ++i) slVisit.push_back(i);
            const unsigned n = slLists + galaxy * 60;
            for (unsigned i = 0; i < n; ++i) {
                uint32_t a = kSlScene + i * 3 + (galaxy % 3); // (galaxies overlap in the scene heap)
                if (slRng() % 1000 < slMoved) a = kSlScene + 4000 + slRng() % slPool;
                slVisit.push_back(a);
            }
        }
        while (smallNow < smallTarget || bigNow < bigTarget || longNow < longNew || cacheNow < cacheTarget) {
            ++step;
            for (int k = 0; k < 6 && smallNow < smallTarget; ++k) {
                if (!s.Alloc(s.Small(), b)) return pts;
                smallNow += b.size;
                stageSmall.push_back(b);
            }
            for (int k = 0; k < 3 && cacheNow < cacheTarget; ++k) {
                if (!cacheNew(cycle)) return pts;
                cacheNow += cacheLived.back().second.size;
            }
            if (longNow < longNew) {
                if (!s.Alloc(s.Between(400, 6000), b)) return pts;
                longNow += b.size;
                longBytes += b.size;
                longLived.push_back(b);
                while (longBytes > longCap) { longBytes -= longLived.front().size; s.Free(longLived.front()); longLived.pop_front(); }
            }
            if (bigNow < bigTarget && (s.rng() % 6) == 0) {
                if (!s.Alloc(s.Between(128u << 10, 2u << 20), b)) return pts;
                bigNow += b.size;
                stageBig.push_back(b);
            }
            if ((s.rng() % 3) == 0) {  // an upload copy / decode buffer that lives a few steps
                if (!s.Alloc(s.Between(128u << 10, 1536u << 10), b)) return pts;
                temps.push_back(b);
                tempStep(3);
            }
            if (!prevSweep.empty() && (step % 4) == 0) { s.Free(prevSweep.back()); prevSweep.pop_back(); }
            for (int k = 0; k < 4 && !expired.empty(); ++k) { s.Free(expired.back()); expired.pop_back(); }
            for (int k = 0; k < 12 && slNext < slVisit.size(); ++k)  // (heap23b: the lists' first calls, during the load's first pictures)
                if (!slNew(slVisit[slNext++])) return pts;
        }
        while (slNext < slVisit.size())
            if (!slNew(slVisit[slNext++])) return pts;
        tempStep(0);
        for (Blk& p : prevSweep) s.Free(p);
        prevSweep.clear();
        for (Blk& e : expired) s.Free(e);
        expired.clear();
        seen[galaxy] = true;
        // the texture ids of this stage
        ids += 120 + s.rng() % 60;
        for (int t = 0; t < 4; ++t) {
            const size_t need = ids * idBytesPerId[t];
            if (need > idCap[t]) {
                size_t cap = std::max<size_t>(idCap[t] * 2, 1024);
                while (cap < need) cap *= 2;
                Blk nb;
                if (!s.Alloc(cap, nb)) return pts;
                s.Free(idTables[t]);
                idTables[t] = nb;
                idCap[t] = cap;
                s.idBytes = 0;  // (what the tables hold now: outside blocks are whole pages)
                for (int u = 0; u < 4; ++u) s.idBytes += idTables[u].out ? s.big.SizeOf(idTables[u].out) : idCap[u];  // (a reused block may be larger)
            }
        }
        // play: 600 frames, small churn every frame, cache-lived blocks now and then, an upload every 40 frames
        std::deque<Blk> churn;
        for (unsigned frame = 0; frame < 600; ++frame) {
            for (int k = 0; k < 3; ++k) {
                if (!s.Alloc(s.Small(), b)) return pts;
                churn.push_back(b);
            }
            while (churn.size() > 400) { s.Free(churn.front()); churn.pop_front(); }
            if ((frame % 2) == 0 && !cacheNew(cycle)) return pts;
            if ((frame % 40) == 0) {
                if (!s.Alloc(s.Between(128u << 10, 1024u << 10), b)) return pts;
                temps.push_back(b);
            }
            if ((frame % 40) == 5) tempStep(0);
        }
        for (Blk& c : churn) s.Free(c);
        tempStep(0);
        // the scene's destruction: its small blocks and half of its stage blocks now, the other half swept during the next stage; the
        // cache-lived blocks whose time is up go back during the next load
        for (Blk& x : stageSmall) s.Free(x);
        std::shuffle(stageBig.begin(), stageBig.end(), s.rng);
        for (size_t i = 0; i < stageBig.size(); ++i) {
            if (i % 2) prevSweep.push_back(stageBig[i]);
            else s.Free(stageBig[i]);
        }
        for (size_t i = 0; i < cacheLived.size();) {
            if (cacheLived[i].first <= cycle) {
                expired.push_back(cacheLived[i].second);
                cacheLived[i] = cacheLived.back();
                cacheLived.pop_back();
            } else {
                ++i;
            }
        }
        std::shuffle(expired.begin(), expired.end(), s.rng);
        if (s.slMode == 1) {  // (heap23b, GXV_SL_SCENEDROP=1: the front end frees the scene heaps' entries before the next scene's bytes)
            for (auto it = slStore.lower_bound(kSlScene); it != slStore.end();) { slFree(it->second); it = slStore.erase(it); }
        }
        s.big.DropCache();  // (port_heap_scene_destroyed)
        Point p{cycle, galaxy, cycle / 7, s.heap.InUse(), s.big.Get().liveBytes, s.heap.Largest(), s.heap.Pieces(), s.heap.FreeBytes(), s.heap.Arena(), s.idBytes,
                s.slBytes, s.slEntries};
        pts.push_back(p);
        if (verbose)
            std::printf("  cycle %3u galaxy %u: C heap in use %6zu KiB, arena %6zu, free %6zu KiB in %5zu pieces, largest %6zu KiB; outside %6zu KiB\n", cycle,
                        galaxy, p.inUse >> 10, p.arena >> 10, p.freeBytes >> 10, p.pieces, p.largest >> 10, p.outside >> 10);
    }
    return pts;
}

void TestSoak() {
    const unsigned cycles = static_cast<unsigned>(EnvL("HEAP_SOAK_CYCLES", 100));
    const unsigned seed = static_cast<unsigned>(EnvL("HEAP_SOAK_SEED", 2026));
    const bool verbose = EnvL("HEAP_SOAK_VERBOSE", 0) != 0;
    const size_t minLargest = static_cast<size_t>(EnvL("HEAP_SOAK_MIN_LARGEST_KIB", 8192)) << 10;

    // the negative control: everything in the C heap (c20)
    {
        Soak s(false, seed);
        if (verbose) std::printf("GXV_HEAP_BIG=0:\n");
        const std::vector<Point> pts = RunSoak(s, cycles, verbose);
        size_t worstLargest = SIZE_MAX;
        for (const Point& p : pts) worstLargest = std::min(worstLargest, p.largest);
        if (s.failed)
            std::printf("GXV_HEAP_BIG=0: ran out after %zu scene destroys: a request for %zu KiB found no room with %zu KiB free in %zu pieces (largest %zu KiB)\n",
                        pts.size(), s.failSize >> 10, s.failFree >> 10, s.failPieces, s.failLargest >> 10);
        else
            std::printf("GXV_HEAP_BIG=0: %zu cycles, the largest free piece fell to %zu KiB\n", pts.size(), worstLargest >> 10);
        Require(s.failed || worstLargest < (1400u << 10), "negative control: the C heap alone runs out of large pieces (as c20 did)");
        Require(!s.failed || s.failFree > s.failSize, "negative control: it ran out with more than the request free in total (fragmentation)");
    }
    // GXV_HEAP_BIG=1
    Soak s(true, seed);
    if (verbose) std::printf("GXV_HEAP_BIG=1:\n");
    const std::vector<Point> pts = RunSoak(s, cycles, verbose);
    Require(!s.failed, "GXV_HEAP_BIG=1: no request fails");
    Require(pts.size() == cycles, "GXV_HEAP_BIG=1: every cycle ran");
    Require(s.bigInHeap == 0, "GXV_HEAP_BIG=1: no block of 128 KiB or more fell back to the C heap");
    const port::bigheap::Stats bs = s.big.Get();
    Require(bs.refusedFloor == 0 && bs.mapFailures == 0 && bs.refusedFull == 0, "GXV_HEAP_BIG=1: the outside blocks were never refused");
    Require(bs.peakBytes + s.floor <= s.pages.budget, "GXV_HEAP_BIG=1: the floor was kept");
    size_t worstLargest = SIZE_MAX;
    for (const Point& p : pts) worstLargest = std::min(worstLargest, p.largest);
    Require(worstLargest >= minLargest, "GXV_HEAP_BIG=1: the largest free piece stays above the bound at every scene destroy");
    // zero growth across loops: the same galaxy's destroy point (the same requests replayed), loop 5 against the last loop (loop 1 met
    // every galaxy first, and the cache-lived blocks, a few stages' lifetimes each, have settled by then), the texture-id tables apart
    // (the one known growth: ids are never reused, ~150 a stage at ~54 bytes).
    const unsigned loops = cycles / 7, from = 5;
    double heapPerLoop = 0, outPerLoop = 0, idPerLoop = 0, otherPerLoop = 0;
    if (loops >= from + 3) {
        double dTotal = 0, dh = 0, dout = 0, did = 0;
        for (unsigned g = 0; g < 7; ++g) {
            const Point& a = pts[from * 7 + g];
            const Point& z = pts[(loops - 1) * 7 + g];
            dh += double(z.inUse) - double(a.inUse);
            dout += double(z.outside) - double(a.outside);
            did += double(z.idBytes) - double(a.idBytes);
        }
        const double n = 7.0 * (loops - 1 - from);
        dTotal = (dh + dout - did) / n;
        heapPerLoop = dh / n;
        outPerLoop = dout / n;
        idPerLoop = did / n;
        otherPerLoop = dTotal;
        // (the model's in-use counts whole chunks: a remainder under 16 bytes stays with its block, so the same requests can hold a few
        // KiB more or less depending on which pieces they found)
        Require(dTotal <= 8 * 1024, "GXV_HEAP_BIG=1: nothing but the texture-id tables grows loop over loop (C heap and outside together)");
        Require(heapPerLoop <= 16 * 1024, "GXV_HEAP_BIG=1: the C heap does not grow loop over loop");
        Require(idPerLoop <= 128 * 1024, "GXV_HEAP_BIG=1: the texture-id tables grow by at most 128 KiB a loop");
    }
    size_t maxPieces = 0, maxInUse = 0, maxOut = 0;
    for (const Point& p : pts) { maxPieces = std::max(maxPieces, p.pieces); maxInUse = std::max(maxInUse, p.inUse); maxOut = std::max(maxOut, p.outside); }
    if (loops < from + 3) std::printf("(growth across loops not measured: fewer than %u loops)\n", from + 3);
    std::printf("GXV_HEAP_BIG=1: %zu cycles, no failure; C heap at the scene destroys: in use up to %zu KiB, free pieces up to %zu, the largest free piece "
                ">= %zu KiB; outside: up to %zu KiB at a destroy, peak %zu KiB (%u blocks), %" PRIu64 " maps, %" PRIu64 " reuses; growth per loop (loops %u..%u): "
                "C heap %+.1f KiB, outside %+.1f KiB; the texture-id tables %+.1f KiB, everything else %+.2f KiB\n",
                pts.size(), maxInUse >> 10, maxPieces, worstLargest >> 10, maxOut >> 10, bs.peakBytes >> 10, bs.peakBlocks, bs.maps, bs.reuses, from + 1, loops,
                heapPerLoop / 1024, outPerLoop / 1024, idPerLoop / 1024, otherPerLoop / 1024);
}
// heap23b: the state-list store (GXV_SL_SCENEDROP), with GXV_HEAP_BIG=1 as on the console. Per loop (7 scene changes) the C heap's change
// at the same galaxy's destroy, averaged over the 7 galaxies, the texture-id tables apart:
//   GXV_SL_SCENEDROP=0 must grow as c24 did (loops 2 -> 3 and 3 -> 4: hundreds of KiB a loop, the growth shrinking: the addresses seen
//   fill in) and its free pieces with it;
//   GXV_SL_SCENEDROP=1 must not grow (<= 16 KiB a loop on average from loop 4 on, as heap23's soak without the store).
// HEAP_SOAK_SL_CYCLES (default 56 = 8 loops). HEAP_SOAK_LONG_CYCLES=<n> (default 0) also runs n cycles of each and prints the largest free
// piece and the growth over the last half (the estimate of how long a session can last).
struct LoopGrowth { std::vector<double> inUse, pieces; double largestFirst = 0, largestLast = 0; size_t slEntriesLast = 0, slBytesLast = 0; };
LoopGrowth Growth(const std::vector<Point>& pts) {
    LoopGrowth g;
    const unsigned loops = static_cast<unsigned>(pts.size() / 7);
    for (unsigned l = 1; l < loops; ++l) {
        double d = 0, dp = 0;
        for (unsigned k = 0; k < 7; ++k) {
            const Point& a = pts[(l - 1) * 7 + k];
            const Point& z = pts[l * 7 + k];
            d += (double(z.inUse) - double(a.inUse)) - (double(z.idBytes) - double(a.idBytes));
            dp += double(z.pieces) - double(a.pieces);
        }
        g.inUse.push_back(d / 7.0);
        g.pieces.push_back(dp / 7.0);
    }
    if (!pts.empty()) {
        g.largestFirst = double(pts[std::min<size_t>(7, pts.size() - 1)].largest);
        g.largestLast = double(pts.back().largest);
        g.slEntriesLast = pts.back().slEntries;
        g.slBytesLast = pts.back().slBytes;
    }
    return g;
}
void TestSlStore() {
    const unsigned cycles = static_cast<unsigned>(EnvL("HEAP_SOAK_SL_CYCLES", 56));
    const unsigned seed = static_cast<unsigned>(EnvL("HEAP_SOAK_SEED", 2026));
    const bool verbose = EnvL("HEAP_SOAK_VERBOSE", 0) != 0;
    LoopGrowth g[2];
    for (int mode = 0; mode < 2; ++mode) {
        Soak s(true, seed, mode);
        if (verbose) std::printf("GXV_HEAP_BIG=1 GXV_SL_SCENEDROP=%d:\n", mode);
        const std::vector<Point> pts = RunSoak(s, cycles, verbose);
        Require(!s.failed && pts.size() == cycles, "state-list store soak: no request fails");
        g[mode] = Growth(pts);
        std::printf("GXV_SL_SCENEDROP=%d: C heap change a loop (same galaxy, id tables apart):", mode);
        for (size_t l = 0; l < g[mode].inUse.size(); ++l) std::printf(" %+.0f", g[mode].inUse[l] / 1024);
        std::printf(" KiB; free pieces:");
        for (size_t l = 0; l < g[mode].pieces.size(); ++l) std::printf(" %+.0f", g[mode].pieces[l]);
        std::printf("; largest free piece %.0f -> %.0f KiB; store %zu entries (%zu KiB) at the last destroy\n", g[mode].largestFirst / 1024,
                    g[mode].largestLast / 1024, g[mode].slEntriesLast, g[mode].slBytesLast >> 10);
    }
    Require(g[0].inUse.size() >= 4 && g[1].inUse.size() >= 4, "state-list store soak: at least 5 loops");
    // (loop 1 -> 2 is index 0)
    Require(g[0].inUse[1] > 256 * 1024 && g[0].inUse[2] > 256 * 1024, "GXV_SL_SCENEDROP=0 grows loop over loop as c24 did (> 256 KiB a loop)");
    Require(g[0].inUse[2] < g[0].inUse[0], "GXV_SL_SCENEDROP=0: the growth shrinks as the addresses seen fill in (as c24's)");
    // (from loop 4 on: the cache-lived blocks, a few stages' lifetimes each, settle during the first loops, as in heap23's soak)
    double late = 0;
    for (size_t l = 3; l < g[1].inUse.size(); ++l) late += g[1].inUse[l];
    late /= double(g[1].inUse.size() - 3);
    std::printf("GXV_SL_SCENEDROP=1: %+.1f KiB a loop from loop 4 on; GXV_SL_SCENEDROP=0: %+.1f KiB a loop over loops 2..4\n", late / 1024,
                (g[0].inUse[1] + g[0].inUse[2] + g[0].inUse[3]) / 3 / 1024);
    Require(late <= 16 * 1024, "GXV_SL_SCENEDROP=1: the C heap does not grow loop over loop");
    const unsigned longCycles = static_cast<unsigned>(EnvL("HEAP_SOAK_LONG_CYCLES", 0));
    for (int mode = 0; longCycles >= 70 && mode < 2; ++mode) {
        Soak s(true, seed, mode);
        const std::vector<Point> pts = RunSoak(s, longCycles, false);
        const LoopGrowth lg = Growth(pts);
        double tail = 0;
        const size_t half = lg.inUse.size() / 2;
        for (size_t l = half; l < lg.inUse.size(); ++l) tail += lg.inUse[l];
        tail /= double(std::max<size_t>(1, lg.inUse.size() - half));
        size_t minLargest = SIZE_MAX, maxInUse = 0;
        for (const Point& p : pts) { minLargest = std::min(minLargest, p.largest); maxInUse = std::max(maxInUse, p.inUse); }
        std::printf("long soak GXV_SL_SCENEDROP=%d: %zu cycles%s; C heap in use up to %zu KiB; largest free piece %.0f KiB after loop 1, %.0f KiB at the end "
                    "(lowest %zu); growth over the last half %+.1f KiB a loop; store %zu entries (%zu KiB) at the end\n",
                    mode, pts.size(), s.failed ? " (a request FAILED)" : "", maxInUse >> 10, lg.largestFirst / 1024, lg.largestLast / 1024, minLargest >> 10,
                    tail / 1024, lg.slEntriesLast, lg.slBytesLast >> 10);
    }
}
}  // namespace

int main() {
    TestCore();
    TestSoak();
    TestSlStore();
    std::printf("heap_soak: %u checks PASS\n", g_checks);
    return 0;
}
