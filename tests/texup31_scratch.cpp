// texup31: host test of platform/vgx/vita/vgx_texup_scratch.h (GXV_TEXUP_SCRATCH / _LAZYFENCE / _VERIFY / _CENSUS logic).
//   g++ -std=c++17 -O1 -Iplatform/vgx/vita tests/texup31_scratch.cpp -o /tmp/texup31 && /tmp/texup31
// tools/test_texup31.sh also builds five mutants of the header (a Flush that frees nothing, a Pending block handed out, verify in linear order, pending bytes
// not counted, a plain block not freed) and demands that this test FAIL on each.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>
#include "vgx_texup_scratch.h"

using namespace vgx::texupx;
namespace swz = vgx::swz;
namespace {
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { if (++g_fail <= 30) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } } while (0)

std::set<void*> g_live;
std::vector<uint32_t> g_allocSizes;
int g_waits = 0;
uint32_t g_failAfter = 0xFFFFFFFFu; // allocations after this many succeed no more
uint32_t g_allocCount = 0;
LinPool* g_pool = nullptr;
void* Alloc(uint32_t n) {
    if (g_allocCount++ >= g_failAfter) return nullptr;
    void* p = std::malloc(n);
    g_live.insert(p);
    g_allocSizes.push_back(n);
    return p;
}
void Free(void* p) { CHECK(g_live.count(p), "free of a block not live (double free?)"); g_live.erase(p); std::free(p); }
void Wait() { ++g_waits; g_pool->Flush(); }
int g_verifyCalls = 0, g_verifyBad = 0;
void VerifyCb(const LinPool::Block& b, const void* lin) {
    ++g_verifyCalls;
    uint32_t l, t;
    if (!VerifyChain(b.meta.plan, b.meta.bpp, static_cast<const uint8_t*>(lin), b.meta.swz, &l, &t)) ++g_verifyBad;
}
Hooks H() { Hooks h; h.alloc = Alloc; h.free = Free; h.waitAll = Wait; return h; }

// a swizzled chain made on the CPU from a linear chain of random texels (levels of the plan, strides of the plan)
void MakeChains(const swz::Plan& p, uint32_t bpp, std::vector<uint8_t>& lin, std::vector<uint8_t>& sw, uint32_t seed) {
    lin.assign(p.linBytes, 0xEE);
    sw.assign(p.swzBytes, 0);
    uint32_t x = seed;
    for (unsigned l = 0; l < p.levels; ++l)
        for (uint32_t y = 0; y < p.h[l]; ++y)
            for (uint32_t xx = 0; xx < p.w[l]; ++xx) {
                uint8_t px[4];
                for (uint32_t k = 0; k < bpp; ++k) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; px[k] = static_cast<uint8_t>(x); }
                std::memcpy(&lin[p.linOff[l] + y * p.linStride[l] + xx * bpp], px, bpp);
                std::memcpy(&sw[p.swzOff[l] + swz::SwizzledIndex(xx, y, p.w[l], p.h[l]) * bpp], px, bpp);
            }
}
} // namespace

int main() {
    // ---- 1. VerifyChain: equal chains pass, a one-byte difference anywhere is found, at the right level
    for (uint32_t bpp : {2u, 4u})
        for (uint32_t w : {8u, 16u, 64u})
            for (uint32_t h : {8u, 32u}) {
                for (int mips = 0; mips < 2; ++mips) {
                    const swz::Plan p = swz::MakePlan(w, h, bpp, mips != 0);
                    std::vector<uint8_t> lin, sw;
                    MakeChains(p, bpp, lin, sw, 1234u + w * 7 + h);
                    uint32_t lv, tx;
                    CHECK(VerifyChain(p, bpp, lin.data(), sw.data(), &lv, &tx), "equal chains differ %ux%u bpp %u", w, h, bpp);
                    for (unsigned l = 0; l < p.levels; ++l) {
                        std::vector<uint8_t> bad = sw;
                        bad[p.swzOff[l] + (p.w[l] * p.h[l] * bpp) / 2] ^= 0x40;
                        const bool same = VerifyChain(p, bpp, lin.data(), bad.data(), &lv, &tx);
                        CHECK(!same && lv == l, "difference at level %u not found (found: same=%d level=%u) %ux%u bpp %u", l, same, lv, w, h, bpp);
                    }
                    // a chain left in LINEAR order (the transfer unit's copy never ran) must differ
                    std::vector<uint8_t> unsw(p.swzBytes, 0);
                    for (unsigned l = 0; l < p.levels; ++l)
                        for (uint32_t y = 0; y < p.h[l]; ++y) std::memcpy(&unsw[p.swzOff[l] + y * p.w[l] * bpp], &lin[p.linOff[l] + y * p.linStride[l]], p.w[l] * bpp);
                    CHECK(!VerifyChain(p, bpp, lin.data(), unsw.data(), nullptr, nullptr), "a linear-order chain passed %ux%u bpp %u", w, h, bpp);
                }
            }

    // ---- 2. the pool, no switches: Acquire is a plain allocation, an immediate Release frees it
    {
        g_live.clear(); g_allocSizes.clear(); g_waits = 0; g_allocCount = 0; g_failAfter = 0xFFFFFFFFu;
        LinPool pool(H(), Config{});
        g_pool = &pool;
        void* a = pool.Acquire(40000);
        void* b = pool.Acquire(40000);
        CHECK(a && b && a != b, "two plain blocks");
        CHECK(g_live.size() == 2 && pool.St().plain == 2 && pool.St().reused == 0, "plain");
        pool.Release(a, false);
        pool.Release(b, false);
        CHECK(g_live.empty() && pool.Blocks() == 0, "plain blocks freed at release (live %zu)", g_live.size());
    }
    // ---- 3. scratch pool: reuse, size classes, a pool cap
    {
        g_live.clear(); g_allocSizes.clear(); g_waits = 0; g_allocCount = 0; g_failAfter = 0xFFFFFFFFu;
        Config c; c.scratch = 1; c.poolBytes = 1u << 20;
        LinPool pool(H(), c);
        g_pool = &pool;
        void* a = pool.Acquire(40000);       // class 64 KiB
        pool.Release(a, false);
        void* b = pool.Acquire(50000);       // the same class: reused
        CHECK(b == a && pool.St().reused == 1 && pool.St().allocated == 1, "reuse in a class (reused %llu allocated %llu)", (unsigned long long)pool.St().reused, (unsigned long long)pool.St().allocated);
        void* big = pool.Acquire(600000);    // > poolBytes / 2: not pooled
        CHECK(big && pool.St().plain == 1, "a block over half the pool is plain");
        pool.Release(big, false);
        CHECK(g_live.count(big) == 0, "the plain block is freed at release");
        pool.Release(b, false);
        CHECK(g_live.size() == 1 && pool.PoolBytes() == 65536, "the pooled block stays (live %zu, pool %u)", g_live.size(), pool.PoolBytes());
        pool.Drain();
        CHECK(g_live.empty(), "drain frees everything");
    }
    // ---- 4. lazy: a Pending block is not handed out before the wait; the wait gives it back; the cap forces a wait
    {
        g_live.clear(); g_allocSizes.clear(); g_waits = 0; g_allocCount = 0; g_failAfter = 0xFFFFFFFFu;
        Config c; c.scratch = 1; c.lazy = 1; c.poolBytes = 256u << 10; c.pendingCap = 300000;
        LinPool pool(H(), c);
        g_pool = &pool;
        void* a = pool.Acquire(60000); // 64 KiB class
        pool.Release(a, true);
        CHECK(pool.PendingCount() == 1 && pool.PendingBytes() == 65536, "pending counted");
        void* b = pool.Acquire(60000);
        CHECK(b != a, "a Pending block was handed out again before the wait");
        pool.Release(b, true);
        void* d = pool.Acquire(60000); // 128 KiB of 256 KiB used: a third fits
        pool.Release(d, true);
        void* e = pool.Acquire(60000); // a fourth fits (256 KiB), the fifth must force a wait
        pool.Release(e, true);
        CHECK(g_waits == 0, "no wait yet (%d)", g_waits);
        void* f = pool.Acquire(60000); // the pool is full and nothing is Free: a plain block, no wait
        CHECK(g_waits == 0 && f && pool.St().plain == 1 && pool.PendingCount() == 4, "a full pool gives a plain block (waits %d plain %llu)", g_waits, (unsigned long long)pool.St().plain);
        CHECK(pool.Release(f, true), "the fifth pending block (%u bytes) is over the cap of 300000: the caller waits", pool.PendingBytes());
        Wait();
        CHECK(pool.PendingCount() == 0 && pool.PendingBytes() == 0 && g_live.size() == 4, "after the wait nothing is pending, the plain block is freed, the pool's four stay (live %zu)", g_live.size());
        void* h = pool.Acquire(60000);
        CHECK(pool.St().reused == 1 && h, "a Free pooled block is reused after the wait");
        f = h;
        pool.Release(f, false);
        pool.Drain();
        CHECK(g_live.empty(), "everything freed");
    }
    // ---- 5. lazy without the pool: deferred plain blocks are freed at the wait; the cap asks for the wait
    {
        g_live.clear(); g_allocSizes.clear(); g_waits = 0; g_allocCount = 0; g_failAfter = 0xFFFFFFFFu;
        Config c; c.lazy = 1; c.pendingCap = 100000;
        LinPool pool(H(), c);
        g_pool = &pool;
        void* a = pool.Acquire(60000);
        CHECK(!pool.Release(a, true), "under the cap");
        void* b = pool.Acquire(60000);
        CHECK(pool.Release(b, true), "over the cap: the caller must wait (pending %u)", pool.PendingBytes());
        CHECK(g_live.size() == 2, "both still live before the wait");
        pool.Flush();
        CHECK(g_live.empty() && pool.PendingCount() == 0 && pool.PendingBytes() == 0, "the wait frees them (live %zu)", g_live.size());
    }
    // ---- 6. an allocation failure waits for the pending blocks and retries once
    {
        g_live.clear(); g_allocSizes.clear(); g_waits = 0; g_allocCount = 0; g_failAfter = 2;
        Config c; c.lazy = 1; c.pendingCap = 1u << 30;
        LinPool pool(H(), c);
        g_pool = &pool;
        void* a = pool.Acquire(1000); pool.Release(a, true);
        void* b = pool.Acquire(1000); pool.Release(b, true);
        void* d = pool.Acquire(1000);   // allocation 3 fails -> wait -> frees two -> retry (allocation index 3 fails again: the counter only grows)
        CHECK(g_waits == 1, "one forced wait (%d)", g_waits);
        CHECK(d == nullptr && pool.St().allocFailures == 1, "after the retry still no memory: null (failures %llu)", (unsigned long long)pool.St().allocFailures);
        g_failAfter = 0xFFFFFFFFu;
        void* e = pool.Acquire(1000);
        CHECK(e != nullptr, "memory again");
        pool.Release(e, false);
        pool.Drain();
        CHECK(g_live.empty(), "everything freed");
    }
    // ---- 7. verify: the callback sees a good chain good, a bad chain bad, deferred or not, before the block is given back
    {
        g_live.clear(); g_allocSizes.clear(); g_waits = 0; g_allocCount = 0; g_failAfter = 0xFFFFFFFFu; g_verifyCalls = g_verifyBad = 0;
        Config c; c.verify = 1; c.lazy = 1; c.scratch = 1; c.pendingCap = 1u << 30;
        LinPool pool(H(), c);
        pool.SetVerify(&VerifyCb);
        g_pool = &pool;
        const swz::Plan p = swz::MakePlan(16, 16, 4, true);
        std::vector<uint8_t> lin, sw;
        MakeChains(p, 4, lin, sw, 99);
        for (int i = 0; i < 4; ++i) {
            void* blk = pool.Acquire(p.linBytes);
            std::memcpy(blk, lin.data(), p.linBytes);
            std::vector<uint8_t>* use = &sw;
            std::vector<uint8_t> bad = sw;
            if (i == 2) { bad[p.swzOff[1] + 3] ^= 1; use = &bad; }
            VerifyMeta m; m.set = true; m.plan = p; m.bpp = 4; m.swz = use->data();
            pool.Release(blk, i % 2 == 0, &m); // two deferred, two immediate
            if (i % 2 == 0) pool.Flush();
        }
        CHECK(g_verifyCalls == 4 && g_verifyBad == 1, "verify calls %d bad %d (want 4, 1)", g_verifyCalls, g_verifyBad);
        pool.Drain();
    }
    // ---- 8. the census line
    {
        Census cen;
        uint64_t ph[kPhases] = {100, 200, 300, 400, 500};
        cen.Note(0, 65536, true, ph);
        cen.Note(4, 2048, false, ph);
        char line[900];
        Stats st; st.acquired = 5; st.reused = 3;
        const int n = Line(line, sizeof line, cen, &st);
        CHECK(n > 0 && std::strstr(line, "swizzled uploads 2") && std::strstr(line, "alloc 100 us") && std::strstr(line, "wait 400 us") && std::strstr(line, "= 1500 us") &&
              std::strstr(line, "<=4K 1 x 1500 us") && std::strstr(line, "<=64K 1 x 1500 us") && std::strstr(line, "RGBA8 1") && std::strstr(line, "LA88 1") &&
              std::strstr(line, "5 taken, 3 reused"), "census line: %s", line);
        Census none;
        Line(line, sizeof line, none, nullptr);
        CHECK(std::strcmp(line, "texup-census: swizzled uploads 0") == 0, "empty census line");
    }
    std::printf(g_fail ? "texup31 test: %d FAILED\n" : "texup31 test: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
