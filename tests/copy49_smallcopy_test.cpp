// copy49: host test of platform/include/port/smallcopy.h (the inline copy / fill) and of vita/vgx-game/smallcopy_wrap.cpp (the three link wrappers,
// the verify mode and the census) with stubs for the system calls. Every size 0..1100 (the whole range of the wrappers, and past it), source and
// destination alignments 0..31, buffers allocated to the exact size so that AddressSanitizer sees any byte read or written past the range, all
// overlaps of the same buffer for memmove, guard bytes around the destination checked. Build and run: tools/test_copy49.sh
#include <cstdio>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <atomic>
#include <random>
#include "port/smallcopy.h"
#include <psp2/kernel/sysmem.h>

extern "C" {
void* __wrap_sceClibMemcpy(void*, const void*, size_t);
void* __wrap_memmove(void*, const void*, size_t);
void* __wrap_memset(void*, int, size_t);
void vgxvita_copyCensusPicture_v1(void);
int __wrap_sceKernelFreeMemBlock(int);
int __wrap_sceKernelAllocMemBlock(const char*, unsigned, size_t, void*);
static unsigned g_realFree, g_realAlloc, g_allocType = 0x09408060;   // the self-test's CDRAM block (a test can make it look cached)
static uint8_t* g_selfBuf;
static void StubDropSelf();
static void StubAddSelf();
static int g_raceOnce;   // the stub changes the world during one lookup (a free racing a lookup)
int __real_sceKernelFreeMemBlock(int) { ++g_realFree; StubDropSelf(); return 0; }
int __real_sceKernelAllocMemBlock(const char*, unsigned t, size_t n, void*) { ++g_realAlloc; if (t == 0x09408060) { if (!g_selfBuf) g_selfBuf = static_cast<uint8_t*>(std::malloc(n)); StubAddSelf(); return 77; } return 78; }
int sceKernelGetMemBlockBase(int, void** b) { *b = g_selfBuf; return 0; }
int sceKernelDelayThread(unsigned) { return 0; }
// sc55 stub memory map: blocks the test declares; an address in none is "unknown" (the call fails)
struct StubBlock { uintptr_t base, size; unsigned type; };
static std::vector<StubBlock> g_stubBlocks;
static unsigned g_infoCalls;
static void StubAddSelf() { g_stubBlocks.push_back({reinterpret_cast<uintptr_t>(g_selfBuf), 256 * 1024, g_allocType}); }
static void StubDropSelf() { for (size_t i = 0; i < g_stubBlocks.size(); ++i) if (g_stubBlocks[i].base == reinterpret_cast<uintptr_t>(g_selfBuf)) { g_stubBlocks.erase(g_stubBlocks.begin() + i); return; } }
int sceKernelGetMemBlockInfoByAddr(void* a, SceKernelMemBlockInfo* i) {
    ++g_infoCalls;
    if (g_raceOnce == 1) {   // the answer is the OLD one, but a free (a block now of another type) completes before the lookup stores it
        g_raceOnce = 2;
        SceKernelMemBlockInfo old = {};
        const int r = sceKernelGetMemBlockInfoByAddr(a, &old);
        *i = old;
        g_stubBlocks[0].type = 0x0C208060;
        __wrap_sceKernelFreeMemBlock(1);
        g_raceOnce = 3;
        return r;
    }
    for (const StubBlock& b : g_stubBlocks)
        if (reinterpret_cast<uintptr_t>(a) >= b.base && reinterpret_cast<uintptr_t>(a) < b.base + b.size) {
            i->mappedBase = reinterpret_cast<void*>(b.base); i->mappedSize = b.size; i->type = b.type; i->memoryType = 0; return 0;
        }
    return -1;
}
// stubs: the "system calls"
static unsigned g_realCopy, g_realMove, g_realSet;
void* __real_sceClibMemcpy(void* d, const void* s, size_t n) { ++g_realCopy; return memmove(d, s, n); }
void* __real_memmove(void* d, const void* s, size_t n) { ++g_realMove; return memmove(d, s, n); }
void* __real_memset(void* d, int c, size_t n) { ++g_realSet; return memset(d, c, n); }
uint64_t sceKernelGetProcessTimeWide(void) { static uint64_t t = 0; return t += 1000000; }
int sceKernelGetThreadId(void) { return 0x1234; }
struct SceKernelThreadInfo;
int sceKernelGetThreadInfo(int, SceKernelThreadInfo*) { return -1; }
// sc70: newlib's first __getreent() of a thread BUILDS its _reent, which memsets it: model that, and count the calls (the wrappers must make none)
static unsigned g_getreentCalls, g_getreentDepthMax;
static thread_local int t_getreentDepth;
void* __wrap_memset(void*, int, size_t);
struct _reent* __getreent(void) {
    static char x; static char scratch[64];
    ++g_getreentCalls;
    if (++t_getreentDepth > g_getreentDepthMax) g_getreentDepthMax = t_getreentDepth;
    if (t_getreentDepth < 4) __wrap_memset(scratch, 0, sizeof scratch);   // (the real one would recurse until the stack ends)
    --t_getreentDepth;
    return reinterpret_cast<struct _reent*>(&x);
}
void port_log_summary(const char* fmt, ...) { (void)fmt; }
}

namespace smallcopy { extern std::atomic<uint32_t> g_clears, g_everLookup, g_lookupFail, g_lookups, g_pageMisses; }
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { if (g_fail++ < 20) { std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } } while (0)

static std::mt19937 rng(12345);

// copy with distinct buffers: the inline code, then the real memcpy as the reference
static void TestCopyDistinct(bool viaWrapper) {
    for (size_t n = 0; n <= 1100; ++n)
        for (unsigned so = 0; so < 32; so += (n < 40 ? 1 : 7)) for (unsigned dop = 0; dop < 32; dop += (n < 40 ? 1 : 5)) {
                        // exact allocation: the source ends right at the end of a malloc block
            uint8_t* sblk = static_cast<uint8_t*>(std::malloc(n + so ? n + so : 1));
            uint8_t* dblk = static_cast<uint8_t*>(std::malloc(n + dop ? n + dop : 1));
            uint8_t* s = sblk + so; uint8_t* d = dblk + dop;
            for (size_t i = 0; i < n; ++i) s[i] = static_cast<uint8_t>(rng());
            std::vector<uint8_t> before(dblk, dblk + dop);   // (uninitialised malloc bytes: only compared with themselves)
            std::vector<uint8_t> ref(s, s + n);
            void* r = nullptr;
            bool did = true;
            if (viaWrapper) r = __wrap_sceClibMemcpy(d, s, n);
            else { if (n >= 1) did = portsc::Copy(d, s, n); r = d; }
            CHECK(did && r == d, "copy n=%zu so=%u do=%u did=%d", n, so, dop, did);
            CHECK(n == 0 || std::memcmp(d, ref.data(), n) == 0, "copy content n=%zu so=%u do=%u", n, so, dop);
            CHECK(dop == 0 || std::memcmp(dblk, before.data(), dop) == 0, "copy wrote before the destination n=%zu", n);
            CHECK(n == 0 || std::memcmp(s, ref.data(), n) == 0, "copy changed the source n=%zu", n);
            std::free(sblk); std::free(dblk);
        }
}

// memmove / overlapping: one buffer, every relative offset
static void TestMove(bool viaWrapper) {
    for (size_t n = 1; n <= 1100; n += (n < 140 ? 1 : 13))
        for (int shift = -140; shift <= 140; shift += (std::abs(shift) < 20 || n < 40 ? 1 : 7)) {
            const size_t total = n + 300;
            uint8_t* blk = static_cast<uint8_t*>(std::malloc(total));
            for (size_t i = 0; i < total; ++i) blk[i] = static_cast<uint8_t>(rng());
            std::vector<uint8_t> ref(blk, blk + total);
            const size_t so = 150, dop = 150 + shift;
            std::memmove(ref.data() + dop, ref.data() + so, n);   // libc reference (bigger buffer: the whole range is inside)
            if (viaWrapper) __wrap_memmove(blk + dop, blk + so, n);
            else { if (!portsc::Copy(blk + dop, blk + so, n)) std::memmove(blk + dop, blk + so, n); }
            CHECK(std::memcmp(blk, ref.data(), total) == 0, "move n=%zu shift=%d", n, shift);
            std::free(blk);
        }
}

static void TestFill(bool viaWrapper) {
    for (size_t n = 0; n <= 1100; ++n)
        for (unsigned dop = 0; dop < 32; dop += (n < 40 ? 1 : 3)) {
            const int c = static_cast<int>(rng() & 0x1FF);   // (a value above 255: only the low byte counts)
            uint8_t* blk = static_cast<uint8_t*>(std::malloc(n + dop + 40));
            for (size_t i = 0; i < n + dop + 40; ++i) blk[i] = static_cast<uint8_t>(rng());
            std::vector<uint8_t> ref(blk, blk + n + dop + 40);
            std::memset(ref.data() + dop, c, n);
            if (viaWrapper) __wrap_memset(blk + dop, c, n);
            else if (n >= 1) portsc::Fill(blk + dop, c, n);
            CHECK(std::memcmp(blk, ref.data(), n + dop + 40) == 0, "fill n=%zu do=%u c=%d", n, dop, c);
            std::free(blk);
            // exactly sized (the end of the block is the end of the range)
            uint8_t* e = static_cast<uint8_t*>(std::malloc(n ? n : 1));
            if (viaWrapper) __wrap_memset(e, c, n); else if (n >= 1) portsc::Fill(e, c, n);
            for (size_t i = 0; i < n; ++i) if (e[i] != static_cast<uint8_t>(c)) { CHECK(false, "fill exact n=%zu", n); break; }
            std::free(e);
        }
}

// sc55: the wrappers must not do the copy inline into / out of non-cached memory, must do it inline in cached memory, and must forget a freed block.
static int RegistryTest() {
    constexpr size_t MB = 1u << 20;
    uint8_t* raw = static_cast<uint8_t*>(std::aligned_alloc(MB, 12 * MB));
    std::memset(raw, 0, 12 * MB);
    const uintptr_t R = reinterpret_cast<uintptr_t>(raw);
    // granule 0: one cached block over all of it; granule 1: one non-cached block over all of it; granule 2: mixed, pages 0-3 cached, 4-7 non-cached,
    // the rest unknown (cached); granule 3: a block of 2 granules, non-cached, starting 4 KiB into granule 2? (kept simple: it starts at granule 3)
    g_stubBlocks = {{R, MB, 0x0C20D060}, {R + MB, MB, 0x0C208060}, {R + 2 * MB, 4096 * 4, 0x0C20D060}, {R + 2 * MB + 4096 * 4, 4096 * 4, 0x09408060},
                    {R + 3 * MB, 2 * MB, 0x09408060}, {R + 10 * MB, 16384, 0x0C208060}};
    vgxvita_copyCensusPicture_v1();   // reads the env (GXV_SMALLCOPY=1, max 256); runs the self-test
    int bad = 0;
    // fx59 self-test: a CDRAM block was allocated and freed with the real calls, left the map as it was, and the inline path is still on
    if (g_realAlloc != 1 || g_realFree != 1 || g_stubBlocks.size() != 6) { std::printf("   FAIL selftest: alloc %u free %u blocks %zu\n", g_realAlloc, g_realFree, g_stubBlocks.size()); ++bad; }
    auto count = [&](bool useMemcpy, void* d, const void* s, size_t n) {
        g_realCopy = g_realSet = 0;
        if (useMemcpy) __wrap_sceClibMemcpy(d, s, n); else __wrap_memset(d, 7, n);
        return (useMemcpy ? g_realCopy : g_realSet) != 0;   // true: went to the real function
    };
    struct Case { size_t dOff, sOff; size_t n; bool expectReal; const char* what; bool srcOnly = false; };
    const Case cases[] = {
        {0 * MB + 100, 0 * MB + 5000, 40, false, "cached -> cached"},
        {1 * MB + 100, 0 * MB + 5000, 40, true, "cached -> non-cached (granule)"},
        {0 * MB + 100, 1 * MB + 5000, 40, true, "non-cached -> cached", true},
        {2 * MB + 100, 0 * MB + 5000, 40, false, "mixed granule, cached page"},
        {2 * MB + 4096 * 4 + 100, 0 * MB + 5000, 40, true, "mixed granule, non-cached page"},
        {2 * MB + 4096 * 4 - 16, 0 * MB + 5000, 40, true, "straddles a cached and a non-cached page"},
        {2 * MB + 4096 * 9, 0 * MB + 5000, 40, true, "unknown memory (lookup fails) is NOT cached (fx59)"},
        {3 * MB + 100, 0 * MB + 5000, 200, true, "CDRAM, 2-granule block"},
        {4 * MB + 100, 0 * MB + 5000, 200, true, "CDRAM, second granule of the block"},
        {5 * MB + 100, 0 * MB + 5000, 200, true, "after the block: unknown = not cached (fx59)"},
        {0 * MB + 100, 0 * MB + 5000, 300, true, "above the limit"},
    };
    for (const Case& c : cases) {
        for (int pass = 0; pass < 2; ++pass) {   // the second pass answers from the tables: no further call of the stub
            const unsigned before = g_infoCalls;
            const bool real = count(true, raw + c.dOff, raw + c.sOff, c.n);
            if (real != c.expectReal) { std::printf("   FAIL registry (copy) %s pass %d: real=%d\n", c.what, pass, real); ++bad; }
            if (pass == 1 && g_infoCalls != before) { std::printf("   FAIL registry %s: second pass called the stub\n", c.what); ++bad; }
        }
        const bool realSet = count(false, raw + c.dOff, nullptr, c.n);
        if (realSet != (c.expectReal && !c.srcOnly)) { std::printf("   FAIL registry (set) %s: real=%d\n", c.what, realSet); ++bad; }
    }
    // the freed block: granule 0 turns non-cached; without the free wrapper the table would still say cached
    g_stubBlocks[0].type = 0x0C208060;
    const bool stale = !count(true, raw + 100, raw + 5000, 40);
    __wrap_sceKernelFreeMemBlock(1);
    const bool fresh = count(true, raw + 100, raw + 5000, 40);
    if (!stale || !fresh) { std::printf("   FAIL registry: stale=%d fresh=%d (want 1 1)\n", stale, fresh); ++bad; }
    // fx59: the alloc wrapper clears too
    g_stubBlocks[0].type = 0x0C20D060;
    __wrap_sceKernelFreeMemBlock(1);
    const bool cachedAgain = !count(true, raw + 100, raw + 5000, 40);
    g_stubBlocks[0].type = 0x0C208060;
    const bool stale2 = !count(true, raw + 100, raw + 5000, 40);
    __wrap_sceKernelAllocMemBlock("x", 0x0C20D060, 4096, nullptr);
    const bool fresh2 = count(true, raw + 100, raw + 5000, 40);
    if (!cachedAgain || !stale2 || !fresh2) { std::printf("   FAIL alloc wrapper: %d %d %d (want 1 1 1)\n", cachedAgain, stale2, fresh2); ++bad; }
    // fx59: a free racing a lookup: the lookup answers with the old type but must not store it
    g_stubBlocks[0].type = 0x0C20D060;
    __wrap_sceKernelFreeMemBlock(1);
    g_raceOnce = 1;
    const bool first = count(false, raw + 100, nullptr, 40);   // old answer (cached) -> inline; world changed meanwhile
    const unsigned c0 = g_infoCalls;
    const bool second = count(false, raw + 100, nullptr, 40);   // must look again and see non-cached
    if (first || !second || g_infoCalls == c0) { std::printf("   FAIL race: first real %d second real %d (want 0 1), stub calls after %u\n", first, second, g_infoCalls - c0); ++bad; }
    g_raceOnce = 0;
    // fx59: failures are counted and remembered (no system call a copy for unknown memory)
    {
        const uint32_t f0 = smallcopy::g_lookupFail.load();
        const unsigned c1 = g_infoCalls;
        for (int i = 0; i < 5; ++i) count(true, raw + 8 * MB + 64 * i, raw + 5000, 40);   // unknown, beyond all blocks
        if (smallcopy::g_lookupFail.load() <= f0 || g_infoCalls - c1 > 2) { std::printf("   FAIL failure accounting: fail +%u, stub calls %u\n", smallcopy::g_lookupFail.load() - f0, g_infoCalls - c1); ++bad; }
    }
    // fx59: 2-way page table: two hot pages in one set (8 MiB apart) cost one stub call each, not a call a copy
    {
        g_stubBlocks[0].type = 0x0C20D060;
        __wrap_sceKernelFreeMemBlock(1);
        count(true, raw + 2 * MB + 100, raw + 5000, 40); count(true, raw + 10 * MB + 100, raw + 5000, 40);   // learn both (mixed granules 2 and 10)
        const unsigned c2 = g_infoCalls;
        bool a = true, b = false;
        for (int i = 0; i < 50; ++i) { a &= !count(true, raw + 2 * MB + 100, raw + 5000, 40); b |= !count(true, raw + 10 * MB + 100, raw + 5000, 40); }
        if (!a || b || g_infoCalls != c2) { std::printf("   FAIL 2-way: cached ok %d, non-cached went inline %d, stub calls %u (want 1 0 0)\n", a, b, g_infoCalls - c2); ++bad; }
    }
    // the copy is right whichever way it went
    for (size_t n = 1; n <= 256; ++n) {
        for (int i = 0; i < 300; ++i) raw[3 * MB + 1000 + i] = static_cast<uint8_t>(i * 7 + n);
        __wrap_sceClibMemcpy(raw + 4 * MB + 33, raw + 3 * MB + 1000, n);
        if (std::memcmp(raw + 4 * MB + 33, raw + 3 * MB + 1000, n)) { std::printf("   FAIL data n=%zu\n", n); ++bad; break; }
    }
    std::printf("registry test: %s (%d)\n", bad ? "FAILED" : "PASS", bad);
    std::free(raw);
    return bad;
}

// fx59: switch off = no clearing at all; self-test failure = inline path off
static int OffTest() {
    vgxvita_copyCensusPicture_v1();
    int bad = 0;
    for (int i = 0; i < 100; ++i) { __wrap_sceKernelFreeMemBlock(1); __wrap_sceKernelAllocMemBlock("x", 0x0C20D060, 4096, nullptr); }
    if (smallcopy::g_clears.load() || smallcopy::g_everLookup.load() || g_infoCalls || g_realFree != 100 || g_realAlloc != 100) { std::printf("   FAIL off: clears %u lookups %u stub %u free %u alloc %u\n", smallcopy::g_clears.load(), smallcopy::g_lookups.load(), g_infoCalls, g_realFree, g_realAlloc); ++bad; }
    uint8_t a[64] = {}, b[64] = {1};
    __wrap_sceClibMemcpy(a, b, 40);
    if (!g_realCopy || g_infoCalls) { std::printf("   FAIL off: a copy was not left to the real function\n"); ++bad; }
    std::printf("off test: %s (%d)\n", bad ? "FAILED" : "PASS", bad);
    return bad;
}
static int SelfTestFailTest() {
    g_allocType = 0x0C20D060;   // the "CDRAM" block looks cached: the lookup is not what the code assumes
    vgxvita_copyCensusPicture_v1();
    uint8_t* raw = static_cast<uint8_t*>(std::malloc(1 << 20));
    g_stubBlocks.push_back({reinterpret_cast<uintptr_t>(raw), 1 << 20, 0x0C20D060});
    g_realCopy = 0;
    __wrap_sceClibMemcpy(raw + 100, raw + 5000, 40);
    int bad = g_realCopy == 1 ? 0 : 1;
    if (bad) std::printf("   FAIL selftest-fail: a cached copy was still done inline\n");
    std::printf("self-test failure test: %s (%d)\n", bad ? "FAILED" : "PASS", bad);
    std::free(raw);
    return bad;
}

// sc70: GXV_SMALLCOPY=1 GXV_COPY_CENSUS=1 (the crashing console run): counting from several threads, the first call of each being a memset/memmove/memcpy,
// must never ask newlib for the thread's _reent (c46: __getreent -> reent init -> memset -> Count -> __getreent ...).
static void CensusThreadBody() {
    uint8_t a[64], b[64];
    __wrap_memset(a, 1, sizeof a);   // (first call of this thread)
    __wrap_sceClibMemcpy(b, a, 40);
    __wrap_memmove(b + 3, b, 30);
    CHECK(b[5] == 1, "census thread copy");
}
static int CensusReentTest() {
    vgxvita_copyCensusPicture_v1();
    g_getreentCalls = 0;
    CensusThreadBody();
    std::thread t1(CensusThreadBody), t2(CensusThreadBody);
    t1.join(); t2.join();
    CHECK(g_getreentCalls == 0, "the census asked newlib for a _reent %u times (depth %u): first-call recursion", g_getreentCalls, g_getreentDepthMax);
    std::printf("-- census without __getreent: %u calls, depth %u: %s\n", g_getreentCalls, g_getreentDepthMax, g_fail ? "FAILED" : "PASS");
    return g_fail ? 1 : 0;
}
int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "censusreent")) { setenv("GXV_SMALLCOPY", "1", 1); setenv("GXV_SMALLCOPY_MAX", "256", 1); setenv("GXV_COPY_CENSUS", "1", 1); return CensusReentTest(); }
    if (argc > 1 && !std::strcmp(argv[1], "off")) { setenv("GXV_SMALLCOPY", "0", 1); unsetenv("GXV_COPY_CENSUS"); return OffTest() ? 1 : 0; }
    if (argc > 1 && !std::strcmp(argv[1], "selftestfail")) { setenv("GXV_SMALLCOPY", "1", 1); setenv("GXV_SMALLCOPY_MAX", "256", 1); unsetenv("GXV_COPY_CENSUS"); return SelfTestFailTest() ? 1 : 0; }
    if (argc > 1 && !std::strcmp(argv[1], "registry")) {
        setenv("GXV_SMALLCOPY", "1", 1); setenv("GXV_SMALLCOPY_MAX", "256", 1); unsetenv("GXV_COPY_CENSUS");
        return RegistryTest() ? 1 : 0;
    }
    std::printf("-- the header alone\n");
    TestCopyDistinct(false); TestMove(false); TestFill(false);
    std::printf("   failures so far: %d\n", g_fail);
    // the wrappers: mode 2 (verify) + census, limit 1024; every kind
    std::printf("-- the wrappers, GXV_SMALLCOPY=2 GXV_SMALLCOPY_MAX=1024 GXV_COPY_CENSUS=1\n");
    setenv("GXV_SMALLCOPY", "2", 1); setenv("GXV_SMALLCOPY_MAX", "1024", 1); setenv("GXV_COPY_CENSUS", "1", 1);
    vgxvita_copyCensusPicture_v1();
    g_realCopy = g_realMove = g_realSet = 0;
    TestCopyDistinct(true); TestMove(true); TestFill(true);
    // (verify mode itself calls the real copy once a call, plus the calls of size 0 / above the limit)
    for (int i = 0; i < 8; ++i) vgxvita_copyCensusPicture_v1();   // each stub tick is 1 s: reports a line every 5 pictures
    std::printf("   failures so far: %d (real copy calls %u, move %u, set %u)\n", g_fail, g_realCopy, g_realMove, g_realSet);
    std::printf("%s\n", g_fail ? "FAILED" : "PASS");
    return g_fail ? 1 : 0;
}
