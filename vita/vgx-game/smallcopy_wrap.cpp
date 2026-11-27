// copy49 (22 Nov 2026), GXV_SMALLCOPY and GXV_COPY_CENSUS: small copies and fills without the system call.
// What reaches sceClibMemcpy in the game build (read from the SDK's libc.a and the link, not guessed):
//   memcpy           newlib's memcpy-stub.o is ONE instruction, a tail jump to sceClibMemcpy (so are __aeabi_memcpy*, which jump to memcpy)
//   sceClibMemcpy    also called directly by vitaGL's objects (it is an undefined symbol of libvitaGL.a)
//   memmove, memset  newlib's own C code (not the system call): a word loop with an alignment prologue
// So ONE link wrapper, -Wl,--wrap=sceClibMemcpy, sees every memcpy of the program (the game's, the port's, newlib's C++ library's, vitaGL's);
// memmove and memset are wrapped the same way. The compiler already expands a copy of a small CONSTANT size in place (clang: a few vld1 / vst1
// for up to ~64 bytes); what reaches the wrappers is the variable-size or larger rest.
// GXV_SMALLCOPY (default 0): 0 = every call goes on to the real function after one load and a compare; 1 = a copy or fill of at most
// GXV_SMALLCOPY_MAX bytes (default 256, 16..1024) is done inline (platform/include/port/smallcopy.h); 2 = as 1, and each such memcpy is also
// done by the system call into a scratch buffer and compared (a mismatch is counted, reported in the "smallcopy:" line, and repaired).
// GXV_SMALLCOPY_KINDS (default 7): bit 0 memcpy / sceClibMemcpy, bit 1 memmove, bit 2 memset, for an A/B of one kind.
// GXV_COPY_CENSUS=1 (default 0): counts the calls by kind and by size class, by thread and by call site (the return address; addr2line as
// for GXV_CLOCK_CENSUS), and the render thread writes one "copy-census:" line every 5 s. The counting costs a few atomic adds a call.
// Both read their variables at the first picture of the render thread (wait_wrap.cpp calls vgxvita_copyCensusPicture_v1 beside the clock
// census), so the first seconds of a start run with the real functions. The wrappers never call memcpy, memmove, memset or any stdio:
// this file is compiled with -fno-builtin (CMakeLists.txt) and the report is written only from the render thread's hook.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "port/smallcopy.h"

extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak));

extern "C" {
void* __real_sceClibMemcpy(void* dst, const void* src, size_t n);
void* __real_memmove(void* dst, const void* src, size_t n);
void* __real_memset(void* dst, int c, size_t n);
int __real_sceKernelFreeMemBlock(SceUID uid);
SceUID __real_sceKernelAllocMemBlock(const char* name, SceKernelMemBlockType type, SceSize size, SceKernelAllocMemBlockOpt* opt);
}

namespace smallcopy {
enum Kind : unsigned { kCopy, kMove, kSet, kKinds };
const char* const kKindName[kKinds] = {"memcpy", "memmove", "memset"};
constexpr unsigned kClasses = 9;   // 0-15, 16-31, 32-63, 64-127, 128-255, 256-511, 512-1023, 1K-4K, 4K and more
const char* const kClassName[kClasses] = {"<16", "16-31", "32-63", "64-127", "128-255", "256-511", "512-1K", "1K-4K", ">=4K"};
constexpr unsigned kThreads = 24, kSites = 512, kTop = 10;

// The thresholds the wrappers read: 0 = the real function. (relaxed loads: written once, by the render thread, at its first picture)
std::atomic<uint32_t> g_limit[kKinds];
std::atomic<uint32_t> g_slow{0};   // 1: the census or the verify mode is on (the wrappers take the slow path)
std::atomic<uint32_t> g_census{0}, g_verify{0};
std::atomic<uint32_t> g_mismatch{0}, g_verified{0}, g_verifySkipped{0};
std::atomic<uint32_t> g_firstBad[4];   // kind-less detail of the first mismatch: size, dst & 15, src & 15, byte index

struct ThreadSlot {
    std::atomic<uintptr_t> key{0};
    std::atomic<int> tid{0};
    std::atomic<uint32_t> n[kKinds][kClasses];
    std::atomic<uint32_t> inl{0};   // sc55: calls of this thread done inline (census only)
    char name[32] = {};
};
struct SiteSlot {
    std::atomic<uintptr_t> at{0};
    std::atomic<uint32_t> n{0};
};
ThreadSlot g_thread[kThreads];
SiteSlot g_site[kSites];
std::atomic<uint32_t> g_total[kKinds][kClasses];
std::atomic<uint32_t> g_otherThreads{0}, g_otherSites{0};
std::atomic<uint32_t> g_bytesSmall{0};   // bytes moved by calls of at most 1023 bytes (wraps at 4 G: a window of 5 s never reaches it)
int g_render = 0;
uint32_t g_pictures = 0;
uint64_t g_windowUs = 0;
uint64_t g_reportEveryUs = 5000000;

inline unsigned ClassOf(size_t n) {
    if (n < 16) return 0;
    if (n < 32) return 1;
    if (n < 64) return 2;
    if (n < 128) return 3;
    if (n < 256) return 4;
    if (n < 512) return 5;
    if (n < 1024) return 6;
    if (n < 4096) return 7;
    return 8;
}

// sc70: the per-thread key must NEVER be newlib's __getreent(). A thread's first __getreent() BUILDS its _reent (malloc + _REENT_INIT_PTR, which
// memsets it, under a mutex); that memset is a call of __wrap_memset, which counts, which asks for the key, which is the reent being built: a
// recursion (or a self-deadlock on the mutex) in every thread whose first newlib call arrives after the census was switched on (all the threads the
// game starts after the first picture: audio, readers, workers). The thread pointer register (TPIDRURO, one block a thread, set by the kernel before
// the first instruction) needs no call at all. (Host test: a thread_local address.)
inline uintptr_t ThreadKey() {
#if defined(__arm__)
    uintptr_t tp;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(tp));
    return tp;
#else
    static thread_local char here;
    return reinterpret_cast<uintptr_t>(&here);
#endif
}

void CountThread(Kind k, unsigned c) {
    const uintptr_t key = ThreadKey();
    unsigned h = static_cast<unsigned>(key >> 4) * 2654435761u;
    for (unsigned j = 0; j < kThreads; ++j) {
        ThreadSlot& s = g_thread[(h >> 8 ^ j) % kThreads];
        uintptr_t seen = s.key.load(std::memory_order_relaxed);
        if (seen == 0) {
            uintptr_t empty = 0;
            if (s.key.compare_exchange_strong(empty, key, std::memory_order_relaxed)) { s.tid.store(sceKernelGetThreadId(), std::memory_order_relaxed); seen = key; }
            else seen = empty;
        }
        if (seen == key) { s.n[k][c].fetch_add(1, std::memory_order_relaxed); return; }
    }
    g_otherThreads.fetch_add(1, std::memory_order_relaxed);
}
void CountInlineThread() {
    const uintptr_t key = ThreadKey();
    unsigned h = static_cast<unsigned>(key >> 4) * 2654435761u;
    for (unsigned j = 0; j < kThreads; ++j) {
        ThreadSlot& s = g_thread[(h >> 8 ^ j) % kThreads];
        if (s.key.load(std::memory_order_relaxed) == key) { s.inl.fetch_add(1, std::memory_order_relaxed); return; }
    }
}
void CountSite(uintptr_t at) {
    unsigned h = static_cast<unsigned>(at >> 1) * 2654435761u;
    for (unsigned j = 0; j < 8; ++j) {
        SiteSlot& s = g_site[(h + j) % kSites];
        uintptr_t seen = s.at.load(std::memory_order_relaxed);
        if (seen == 0) {
            uintptr_t empty = 0;
            if (s.at.compare_exchange_strong(empty, at, std::memory_order_relaxed)) seen = at;
            else seen = empty;
        }
        if (seen == at) { s.n.fetch_add(1, std::memory_order_relaxed); return; }
    }
    g_otherSites.fetch_add(1, std::memory_order_relaxed);
}
void Count(Kind k, size_t n, void* at) {
    const unsigned c = ClassOf(n);
    g_total[k][c].fetch_add(1, std::memory_order_relaxed);
    CountThread(k, c);
    if (c <= 6) { CountSite(reinterpret_cast<uintptr_t>(at)); g_bytesSmall.fetch_add(static_cast<uint32_t>(n), std::memory_order_relaxed); }
}

// sc55 (22 Nov 2026): the inline code only touches CACHED memory. Why: on the console copy49 made the HUD flicker and fade (Vita3K: 0 mismatches, so
// the bytes were right; the difference is the memory and the threads). The HUD is the part of a picture made of SMALL draws: their vertices and uniforms
// are copies of 16-256 bytes into vitaGL's GPU-mapped memory (CDRAM, USER_RW_UNCACHE: write-combined, not cached), which the big 3D copies (above 256 B)
// never reach with this switch. sceClibMemcpy is the system's own code for such memory; the inline NEON code stores 16 bytes unaligned (vst1.8) and
// writes the head and tail chunks twice, and leaves the write buffer undrained. Vita3K maps every guest page as plain host RAM, so it cannot show it.
// Every copy whose destination (or source, for a copy) is not cached memory goes to the real function, as before copy49. Where the memory is cached comes
// from sceKernelGetMemBlockInfoByAddr, remembered per 1 MiB granule when one block covers the granule, else per 4 KiB page (a direct-mapped table of
// words: one atomic load a lookup). Memory the call does not know (the stack, the module's data) is cached. A block freed through
// sceKernelFreeMemBlock (wrapped) clears the tables, so a later block at the same address is looked up again.
// GXV_SMALLCOPY_GPU=1 lifts the rule (the A/B that proves the cause on the console).
// fx59 (22 Nov 2026, review of c41 items 2, 3, 7):
//  - a FAILED sceKernelGetMemBlockInfoByAddr counts as NOT cached (the safe side: the real function does the copy), is counted (g_lookupFail) and
//    logged; the negative answer is remembered per page like any other;
//  - a start-up self-test (SelfTest) allocates a CDRAM block, asks about an INTERIOR address and expects "not cached"; if it fails the inline path is
//    switched off (every g_limit 0) and the line says so;
//  - sceKernelAllocMemBlock is wrapped too and clears the tables (frees outside the wrap, e.g. thread stacks, left stale entries);
//  - the epoch race is closed: a lookup stores only under a try-lock, only when no clear is in progress (g_busy 0) and the epoch is unchanged since the
//    lookup began; a clear holds the lock;
//  - a clear is done only if a lookup was ever made (switch off: the free / alloc wrappers cost one load);
//  - the page table is 2-way (a pair of slots a page index): two hot pages 16 MiB apart no longer evict each other. Page misses are counted and logged.
constexpr unsigned kGranBits = 20, kPageBits = 12, kPageSlots = 4096;
std::atomic<uint8_t> g_gran[1u << 12];          // 0 unknown, 1 cached, 2 not cached, 3 mixed (use the page table)
std::atomic<uint32_t> g_page[kPageSlots];                     // (page << 3) | 4 valid | 1 cached; slots 2k and 2k+1 are one set
std::atomic<uint32_t> g_epoch{0}, g_lookups{0}, g_pageMisses{0}, g_lookupFail{0}, g_clears{0};
std::atomic<uint32_t> g_busy{0};       // clears in progress (free / alloc wrappers between their first bump and their last)
std::atomic<uint32_t> g_lock{0};       // 1: a store or a clear holds the tables
std::atomic<uint32_t> g_everLookup{0}; // set by the first slow-path lookup: until then the wrappers clear nothing
std::atomic<uint32_t> g_allowGpu{0};
std::atomic<uint32_t> g_active{0};     // 1: GXV_SMALLCOPY > 0 (the hook reports the registry every 5 s)
std::atomic<uint32_t> g_selfTest{0};   // 0 not run, 1 pass, 2 FAIL, 3 could not run (no block)
uint32_t g_selfTestInfo[3] = {};       // type, memoryType, result of the query

inline bool IsCachedType(SceKernelMemBlockType t, int memoryType) {
    if (memoryType == SCE_KERNEL_MEMORY_TYPE_NORMAL_NC) return false;
    if (memoryType == SCE_KERNEL_MEMORY_TYPE_NORMAL) return true;
    return ((t >> 12) & 0xF) == 0xD || ((t >> 12) & 0xF) == 0x4;   // 0x...D060 cached, 0x...8060 not, 0x...4060 L1 write-back
}
inline bool TryLock() { uint32_t z = 0; return g_lock.compare_exchange_strong(z, 1, std::memory_order_acquire, std::memory_order_relaxed); }
inline void Unlock() { g_lock.store(0, std::memory_order_release); }
void PageStore(uint32_t page, uint32_t value) {   // (the lock is held) 2-way: same page, else an empty slot, else the way chosen by a page bit
    std::atomic<uint32_t>* set = &g_page[(page % (kPageSlots / 2)) * 2];
    for (unsigned w = 0; w < 2; ++w) if ((set[w].load(std::memory_order_relaxed) >> 3) == page && (set[w].load(std::memory_order_relaxed) & 4)) { set[w].store(value, std::memory_order_relaxed); return; }
    for (unsigned w = 0; w < 2; ++w) if (!(set[w].load(std::memory_order_relaxed) & 4)) { set[w].store(value, std::memory_order_relaxed); return; }
    set[(page / (kPageSlots / 2)) & 1].store(value, std::memory_order_relaxed);
}
// slow path: one system call per block / page, never made while the answer is known
bool CachedSlow(uintptr_t a) {
    g_everLookup.store(1, std::memory_order_relaxed);
    const uint32_t ep = g_epoch.load(std::memory_order_acquire);
    g_lookups.fetch_add(1, std::memory_order_relaxed);
    SceKernelMemBlockInfo info;
    info.size = sizeof info;
    const bool ok = sceKernelGetMemBlockInfoByAddr(reinterpret_cast<void*>(a), &info) >= 0;
    if (!ok) g_lookupFail.fetch_add(1, std::memory_order_relaxed);
    const bool cached = ok ? IsCachedType(info.type, info.memoryType) : false;   // a failure is NOT cached
    if (g_busy.load(std::memory_order_acquire) != 0 || !TryLock()) return cached;   // a clear is running (or a store): answer, remember nothing
    if (g_busy.load(std::memory_order_acquire) == 0 && g_epoch.load(std::memory_order_acquire) == ep) {
        const uintptr_t gran = a >> kGranBits;
        const uintptr_t lo = gran << kGranBits, hi = lo + (uintptr_t(1) << kGranBits);
        const bool whole = ok && reinterpret_cast<uintptr_t>(info.mappedBase) <= lo && reinterpret_cast<uintptr_t>(info.mappedBase) + info.mappedSize >= hi;
        if (whole) g_gran[gran & 4095].store(cached ? 1 : 2, std::memory_order_relaxed);
        else {
            const uint32_t page = static_cast<uint32_t>(a >> kPageBits) & 0x1FFFFFFFu;   // (the console's 32-bit addresses give 20 bits)
            PageStore(page, (page << 3) | 4 | (cached ? 1u : 0u));
            g_gran[gran & 4095].store(3, std::memory_order_relaxed);
        }
    }
    Unlock();
    return cached;
}
// forget everything learnt (a block was freed or allocated). Free / alloc wrappers: call = the real function, run between the two bumps.
template <class F> inline auto Invalidate(F&& call) -> decltype(call()) {
    g_busy.fetch_add(1, std::memory_order_acq_rel);    // (two atomic adds: a lookup that began meanwhile never stores)
    g_epoch.fetch_add(1, std::memory_order_acq_rel);
    auto r = call();
    if (g_everLookup.load(std::memory_order_acquire)) {   // (switch off: nothing was ever learnt, the 20 KB are not touched)
        for (unsigned spin = 0; !TryLock(); ++spin) if (spin > 2000) { sceKernelDelayThread(1); spin = 0; }
        for (unsigned i = 0; i < (1u << 12); ++i) g_gran[i].store(0, std::memory_order_relaxed);
        for (unsigned i = 0; i < kPageSlots; ++i) g_page[i].store(0, std::memory_order_relaxed);
        g_clears.fetch_add(1, std::memory_order_relaxed);
        Unlock();
    }
    g_epoch.fetch_add(1, std::memory_order_acq_rel);
    g_busy.fetch_sub(1, std::memory_order_acq_rel);
    return r;
}
inline bool CachedAt(uintptr_t a) {
    const uint8_t g = g_gran[(a >> kGranBits) & 4095].load(std::memory_order_relaxed);
    if (g == 1) return true;
    if (g == 2) return false;
    if (g == 3) {
        const uint32_t page = static_cast<uint32_t>(a >> kPageBits) & 0x1FFFFFFFu;   // (the console's 32-bit addresses give 20 bits)
        const std::atomic<uint32_t>* set = &g_page[(page % (kPageSlots / 2)) * 2];
        const uint32_t e0 = set[0].load(std::memory_order_relaxed), e1 = set[1].load(std::memory_order_relaxed);
        if ((e0 >> 3) == page && (e0 & 4)) return (e0 & 1) != 0;
        if ((e1 >> 3) == page && (e1 & 4)) return (e1 & 1) != 0;
        g_pageMisses.fetch_add(1, std::memory_order_relaxed);
    }
    return CachedSlow(a);
}
// the first and the last byte of the range (a block is at least a page, so two probes cover a range of at most 256 B... 1 KiB)
inline bool CachedRange(const void* p, size_t n) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (!CachedAt(a)) return false;
    const uintptr_t z = a + n - 1;
    return (z >> kPageBits) == (a >> kPageBits) || CachedAt(z);
}
// may the inline code do this copy / fill? (the caller has checked n - 1 < limit)
inline bool InlineOk(const void* d, const void* s, size_t n) {
    if (__builtin_expect(g_allowGpu.load(std::memory_order_relaxed) != 0, 0)) return true;
    return CachedRange(d, n) && (!s || CachedRange(s, n));
}
std::atomic<uint32_t> g_refusedUncached[kKinds];
std::atomic<uint32_t> g_inlined[kKinds];       // calls done inline (census only)

// memcpy verified: the system call fills a scratch buffer from the untouched source, the inline code does the real copy, the two are compared.
bool VerifiedCopy(void* d, const void* s, size_t n) {
    if (portsc::Overlaps(d, s, n)) { g_verifySkipped.fetch_add(1, std::memory_order_relaxed); return false; }
    alignas(16) uint8_t ref[portsc::kMaxLimit];
    __real_sceClibMemcpy(ref, s, n);
    if (!portsc::Copy(d, s, n)) return false;
    const uint8_t* got = static_cast<const uint8_t*>(d);
    for (size_t i = 0; i < n; ++i) {
        if (got[i] != ref[i]) {
            if (g_mismatch.fetch_add(1, std::memory_order_relaxed) == 0) {
                g_firstBad[0].store(static_cast<uint32_t>(n), std::memory_order_relaxed);
                g_firstBad[1].store(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(d) & 15), std::memory_order_relaxed);
                g_firstBad[2].store(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s) & 15), std::memory_order_relaxed);
                g_firstBad[3].store(static_cast<uint32_t>(i), std::memory_order_relaxed);
            }
            __real_sceClibMemcpy(d, ref, n);   // repair: the program gets the system call's result
            break;
        }
    }
    g_verified.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// start-up self-test: a CDRAM block is the known uncached memory. Ask about an interior address (what the wrappers do for a copy); the answer must be
// "not cached", else the lookup does not work as assumed and the inline path is switched off.
void SelfTest() {
    const SceUID uid = __real_sceKernelAllocMemBlock("galaxy_sc_selftest", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256 * 1024, nullptr);
    void* base = nullptr;
    if (uid < 0 || sceKernelGetMemBlockBase(uid, &base) < 0 || !base) { if (uid >= 0) __real_sceKernelFreeMemBlock(uid); g_selfTest.store(3, std::memory_order_relaxed); return; }
    SceKernelMemBlockInfo info;
    info.size = sizeof info;
    info.type = 0; info.memoryType = 0;
    const int r = sceKernelGetMemBlockInfoByAddr(static_cast<uint8_t*>(base) + 0x12345, &info);
    g_selfTestInfo[0] = info.type; g_selfTestInfo[1] = static_cast<uint32_t>(info.memoryType); g_selfTestInfo[2] = static_cast<uint32_t>(r);
    const bool pass = r >= 0 && !IsCachedType(info.type, info.memoryType);
    g_selfTest.store(pass ? 1 : 2, std::memory_order_relaxed);
    __real_sceKernelFreeMemBlock(uid);
}
void ReportRegistry() {   // (a line even without the census: the lookups, failures, page misses, clears)
    if (port_log_summary)
        port_log_summary("smallcopy: sc55/fx59 registry: lookups %u (failed %u = counted as NOT cached), page-misses %u, table clears %u, self-test %s", g_lookups.load(std::memory_order_relaxed),
                         g_lookupFail.load(std::memory_order_relaxed), g_pageMisses.load(std::memory_order_relaxed), g_clears.load(std::memory_order_relaxed),
                         g_selfTest.load(std::memory_order_relaxed) == 1 ? "PASS" : g_selfTest.load(std::memory_order_relaxed) == 2 ? "FAIL (inline path off)" : g_selfTest.load(std::memory_order_relaxed) == 3 ? "not run (no block; counted as FAIL, inline path off)" : "not run");
}

void Report(double seconds) {
    const double pic = g_pictures ? static_cast<double>(g_pictures) : 1.0;
    char line[2048];
    int at = 0;
#define PUT(...) do { if (at < static_cast<int>(sizeof line)) at += std::snprintf(line + at, sizeof line - at, __VA_ARGS__); } while (0)
    if (g_census.load(std::memory_order_relaxed)) {
        uint32_t tot[kKinds][kClasses], all = 0, upto256 = 0, upto1k = 0;
        for (unsigned k = 0; k < kKinds; ++k)
            for (unsigned c = 0; c < kClasses; ++c) {
                tot[k][c] = g_total[k][c].exchange(0, std::memory_order_relaxed);
                all += tot[k][c];
                if (c <= 4) upto256 += tot[k][c];
                if (c <= 6) upto1k += tot[k][c];
            }
        PUT("copy-census: %.1f s, %u pictures: calls a picture %.1f, of them under 256 B %.1f, under 1 KiB %.1f, small bytes a picture %.0f;", seconds,
            g_pictures, all / pic, upto256 / pic, upto1k / pic, g_bytesSmall.exchange(0, std::memory_order_relaxed) / pic);
        for (unsigned k = 0; k < kKinds; ++k) {
            PUT(" %s", kKindName[k]);
            for (unsigned c = 0; c < kClasses; ++c) if (tot[k][c]) PUT(" %s:%.1f", kClassName[c], tot[k][c] / pic);
            PUT("%s", ";");
        }
        {
            uint32_t inl = 0, ref = 0;
            for (unsigned k = 0; k < kKinds; ++k) { inl += g_inlined[k].exchange(0, std::memory_order_relaxed); ref += g_refusedUncached[k].exchange(0, std::memory_order_relaxed); }
            PUT(" sc55: done inline a picture %.1f (est. saved %.3f ms a picture at 160 ns a call), refused for non-cached memory %.1f, lookups %u (failed %u) page-misses %u;", inl / pic,
                inl / pic * 0.00016, ref / pic, g_lookups.load(std::memory_order_relaxed), g_lookupFail.load(std::memory_order_relaxed), g_pageMisses.load(std::memory_order_relaxed));
        }
        PUT(" by thread (name: calls a picture, of them under 256 B / under 1 KiB; inl = done inline, est. ms a picture saved at 160 ns):");
        for (unsigned t = 0; t < kThreads; ++t) {
            ThreadSlot& s = g_thread[t];
            const int tid = s.tid.load(std::memory_order_relaxed);
            uint32_t sum = 0, s256 = 0, s1k = 0;
            const uint32_t sinl = s.inl.exchange(0, std::memory_order_relaxed);
            for (unsigned k = 0; k < kKinds; ++k)
                for (unsigned c = 0; c < kClasses; ++c) {
                    const uint32_t v = s.n[k][c].exchange(0, std::memory_order_relaxed);
                    sum += v;
                    if (c <= 4) s256 += v;
                    if (c <= 6) s1k += v;
                }
            if (!tid || !(sum | sinl)) continue;
            if (!s.name[0]) {
                SceKernelThreadInfo info;
                info.size = sizeof info;
                if (sceKernelGetThreadInfo(tid, &info) >= 0) std::snprintf(s.name, sizeof s.name, "%s", info.name);
                else std::snprintf(s.name, sizeof s.name, "0x%x", static_cast<unsigned>(tid));
            }
            PUT(" %s%s %.1f/%.1f/%.1f inl %.1f (%.3f ms)", s.name, tid == g_render ? " (render)" : "", sum / pic, s256 / pic, s1k / pic, sinl / pic, sinl / pic * 0.00016);
        }
        const uint32_t oT = g_otherThreads.exchange(0, std::memory_order_relaxed);
        if (oT) PUT(" others %.1f", oT / pic);
        uintptr_t topAt[kTop] = {};
        uint32_t topN[kTop] = {};
        for (unsigned i = 0; i < kSites; ++i) {
            const uint32_t c = g_site[i].n.exchange(0, std::memory_order_relaxed);
            if (!c) continue;
            const uintptr_t a = g_site[i].at.load(std::memory_order_relaxed);
            unsigned j = kTop;
            while (j > 0 && topN[j - 1] < c) --j;
            if (j >= kTop) continue;
            for (unsigned m = kTop - 1; m > j; --m) { topN[m] = topN[m - 1]; topAt[m] = topAt[m - 1]; }
            topN[j] = c;
            topAt[j] = a;
        }
        PUT("; top sites under 1 KiB (addr2line; Report is at %p):", reinterpret_cast<void*>(&Report));
        for (unsigned j = 0; j < kTop && topN[j]; ++j) PUT(" %p %.1f", reinterpret_cast<void*>(topAt[j]), topN[j] / pic);
        const uint32_t oS = g_otherSites.exchange(0, std::memory_order_relaxed);
        if (oS) PUT(" (unplaced %.1f)", oS / pic);
        if (port_log_summary) port_log_summary("%s", line);
        else std::fprintf(stderr, "%s\n", line);
        at = 0;
    }
    if (g_verify.load(std::memory_order_relaxed)) {
        PUT("smallcopy: verify: %u copies compared so far, %u overlapping (left to the system call), %u MISMATCHES", g_verified.load(std::memory_order_relaxed),
            g_verifySkipped.load(std::memory_order_relaxed), g_mismatch.load(std::memory_order_relaxed));
        if (g_mismatch.load(std::memory_order_relaxed))
            PUT(" (first: %u bytes, dst&15 %u, src&15 %u, byte %u)", g_firstBad[0].load(std::memory_order_relaxed), g_firstBad[1].load(std::memory_order_relaxed),
                g_firstBad[2].load(std::memory_order_relaxed), g_firstBad[3].load(std::memory_order_relaxed));
        if (port_log_summary) port_log_summary("%s", line);
        else std::fprintf(stderr, "%s\n", line);
    }
    if (g_active.load(std::memory_order_relaxed) && !g_census.load(std::memory_order_relaxed)) ReportRegistry();
    g_pictures = 0;
#undef PUT
}

inline bool Slow() { return __builtin_expect(g_slow.load(std::memory_order_relaxed) != 0, 0); }
} // namespace smallcopy

extern "C" {
// The first call (the render thread's first picture) reads the settings; then, while the census or the verify mode is on, one line every 5 s.
void vgxvita_copyCensusPicture_v1(void) {
    using namespace smallcopy;
    static int asked = 0;
    if (__builtin_expect(!asked, 0)) {
        asked = 1;
        auto env = [](const char* name, int def) { const char* e = std::getenv(name); return e && *e ? std::atoi(e) : def; };
        const int mode = env("GXV_SMALLCOPY", 0), census = env("GXV_COPY_CENSUS", 0);
        int limit = env("GXV_SMALLCOPY_MAX", 256);
        if (limit < 16) limit = 16;
        if (limit > static_cast<int>(portsc::kMaxLimit)) limit = static_cast<int>(portsc::kMaxLimit);
        const int kinds = env("GXV_SMALLCOPY_KINDS", 7);
        g_allowGpu.store(env("GXV_SMALLCOPY_GPU", 0) > 0, std::memory_order_relaxed);
        g_render = sceKernelGetThreadId();
        g_windowUs = sceKernelGetProcessTimeWide();
        // sc70: the self-test (it allocates and frees a CDRAM block) runs BEFORE any limit, the census or the verify mode is published, so no other
        // thread is inside an inline copy, a lookup or a count while it runs; the settings go live at the end.
        if (mode > 0) {
            g_active.store(1, std::memory_order_release);
            SelfTest();
            // (fx66: "not run" (3: no free 256 KiB CDRAM block) proves nothing, so it counts as a FAIL: the inline path stays off)
            const unsigned st = g_selfTest.load(std::memory_order_relaxed);
            if ((st == 2 || st == 3) && !g_allowGpu.load(std::memory_order_relaxed)) {
                for (unsigned k = 0; k < kKinds; ++k) g_limit[k].store(0, std::memory_order_relaxed);   // the lookup does not tell CDRAM from RAM: no inline path
                if (port_log_summary) port_log_summary("smallcopy: SELF-TEST %s (CDRAM interior address: type 0x%08x memoryType 0x%x result 0x%x): inline path DISABLED", st == 3 ? "COULD NOT RUN (no free CDRAM block; counted as FAILED)" : "FAILED", g_selfTestInfo[0], g_selfTestInfo[1], g_selfTestInfo[2]);
            } else if (port_log_summary) port_log_summary("smallcopy: self-test %s (CDRAM interior address: type 0x%08x memoryType 0x%x result 0x%x)",
                                                         g_selfTest.load(std::memory_order_relaxed) == 1 ? "PASS" : "not run", g_selfTestInfo[0], g_selfTestInfo[1], g_selfTestInfo[2]);
        }
        {   // (a failed self-test above already zeroed the limits; publish the others now)
            const bool off = mode > 0 && !g_allowGpu.load(std::memory_order_relaxed) && g_selfTest.load(std::memory_order_relaxed) != 1;
            for (unsigned k = 0; k < kKinds; ++k) g_limit[k].store(!off && mode > 0 && ((kinds >> k) & 1) ? static_cast<uint32_t>(limit) : 0, std::memory_order_relaxed);
            g_census.store(census > 0, std::memory_order_relaxed);
            g_verify.store(mode == 2, std::memory_order_relaxed);
            g_slow.store(census > 0 || mode == 2, std::memory_order_release);
        }
        if ((mode > 0 || census > 0) && port_log_summary)
            port_log_summary("smallcopy: GXV_SMALLCOPY=%d (max %d B, kinds %d: 1 memcpy 2 memmove 4 memset), GXV_COPY_CENSUS=%d, GXV_SMALLCOPY_GPU=%d (0: cached memory only)", mode, limit, kinds, census,
                             static_cast<int>(g_allowGpu.load(std::memory_order_relaxed)));
    }
    if (!g_slow.load(std::memory_order_relaxed) && !g_active.load(std::memory_order_relaxed)) return;
    ++g_pictures;
    const uint64_t now = sceKernelGetProcessTimeWide();
    if (now - g_windowUs < g_reportEveryUs) return;
    Report(static_cast<double>(now - g_windowUs) / 1e6);
    g_windowUs = now;
}

// sc55: one decision for the three wrappers. The census counts what is done inline and what is refused for non-cached memory.
static inline bool Take(smallcopy::Kind k, void* d, const void* s, size_t n) {
    using namespace smallcopy;
    if (!(n - 1u < g_limit[k].load(std::memory_order_relaxed))) return false;
    if (!InlineOk(d, s, n)) {
        if (g_census.load(std::memory_order_relaxed)) g_refusedUncached[k].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}
static inline void NoteInline(smallcopy::Kind k) {
    using namespace smallcopy;
    if (g_census.load(std::memory_order_relaxed)) { g_inlined[k].fetch_add(1, std::memory_order_relaxed); CountInlineThread(); }
}
void* __wrap_sceClibMemcpy(void* d, const void* s, size_t n) {
    using namespace smallcopy;
    if (__builtin_expect(g_slow.load(std::memory_order_relaxed) != 0, 0)) {
        if (g_census.load(std::memory_order_relaxed)) Count(kCopy, n, __builtin_return_address(0));
        if (g_verify.load(std::memory_order_relaxed) && Take(kCopy, d, s, n)) {
            if (VerifiedCopy(d, s, n)) { NoteInline(kCopy); return d; }
            return __real_sceClibMemcpy(d, s, n);
        }
    }
    if (Take(kCopy, d, s, n) && portsc::Copy(d, s, n)) { if (__builtin_expect(Slow(), 0)) NoteInline(kCopy); return d; }
    return __real_sceClibMemcpy(d, s, n);
}
void* __wrap_memmove(void* d, const void* s, size_t n) {
    using namespace smallcopy;
    if (Slow() && g_census.load(std::memory_order_relaxed)) Count(kMove, n, __builtin_return_address(0));
    if (Take(kMove, d, s, n) && portsc::Copy(d, s, n)) { if (__builtin_expect(Slow(), 0)) NoteInline(kMove); return d; }   // (Copy handles every overlap up to 128 B; above that it declines an overlap)
    return __real_memmove(d, s, n);
}
void* __wrap_memset(void* d, int c, size_t n) {
    using namespace smallcopy;
    if (Slow() && g_census.load(std::memory_order_relaxed)) Count(kSet, n, __builtin_return_address(0));
    if (Take(kSet, d, nullptr, n)) { portsc::Fill(d, c, n); if (__builtin_expect(Slow(), 0)) NoteInline(kSet); return d; }
    return __real_memset(d, c, n);
}
// sc55 / fx59: a freed or allocated block may sit at an address learnt with another type: forget what was learnt (only if anything was ever learnt)
int __wrap_sceKernelFreeMemBlock(SceUID uid) {
    // sc70: with GXV_SMALLCOPY=0 (and the census alone) nothing was ever learnt: straight to the system call, no counter touched
    if (__builtin_expect(!smallcopy::g_active.load(std::memory_order_acquire), 1)) return __real_sceKernelFreeMemBlock(uid);
    return smallcopy::Invalidate([&] { return __real_sceKernelFreeMemBlock(uid); });
}
SceUID __wrap_sceKernelAllocMemBlock(const char* name, SceKernelMemBlockType type, SceSize size, SceKernelAllocMemBlockOpt* opt) {
    if (__builtin_expect(!smallcopy::g_active.load(std::memory_order_acquire), 1)) return __real_sceKernelAllocMemBlock(name, type, size, opt);
    return smallcopy::Invalidate([&] { return __real_sceKernelAllocMemBlock(name, type, size, opt); });
}
} // extern "C"
