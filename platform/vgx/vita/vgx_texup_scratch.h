// texup31 (17 Nov 2026): the fixed cost of one swizzled texture upload (vgx_host_galaxy_vita.cpp vgxvita_uploadSwizzled), as four small
// switches' worth of pure logic, free of sceGxm so that tests/texup31_scratch.cpp can run it on the host.
//
//   GXV_TEXUP_CENSUS=1   (default 0) split census: per upload the time in allocate / copy / transfer-issue / wait / setup, the formats and sizes.
//   GXV_TEXUP_LAZYFENCE  (default 0) 1: the swizzled upload does not wait for the transfer unit itself (sceGxmTransferFinish, once for every
//                        texture); it leaves its transfers pending as the compressed and generated-level uploads already do with
//                        GXV_VITA_UPLOAD_SYNC=2 (the back end fences before a scene ends, before the texture is freed or replaced, at the
//                        frame's end), and its linear scratch is returned at the next wait. Only with GXV_VITA_UPLOAD_SYNC=2 (the default).
//   GXV_TEXUP_SCRATCH    (default 0) 1: the linear scratch of an upload is taken from a small pool of reused blocks (power-of-two size classes,
//                        GXV_TEXUP_POOL_KIB of them at most, default 2048) instead of vglForceAllocAligned + vglFree for each texture.
//   GXV_TEXUP_VERIFY     (default 0) 1: before its scratch is given back, the swizzled chain is compared texel for texel with the linear chain
//                        it was made from (Morton order, vgx_swizzle_layout.h SwizzledIndex); a difference is counted and logged once.
//
// The pool holds blocks in three states: Free (in the pool, may be handed out), Busy (handed out, the transfers not issued yet or being
// waited for by the caller), Pending (transfers issued, the unit may still read it: given back by Flush, which the caller's wait makes).
// A block that does not fit the pool (too large, pool full with nothing Free) is a plain allocation, freed when released.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "vgx_swizzle_layout.h"

namespace vgx::texupx {

struct Config {
    int census = 0, lazy = 0, scratch = 0, verify = 0;
    uint32_t poolBytes = 2048u << 10;  // the most the pool keeps
    uint32_t pendingCap = 4096u << 10; // lazy: the most scratch bytes left pending before the caller waits
};

// ---- the verify check (texel for texel). lin: vitaGL's linear chain (row strides of the plan), swz: the swizzled chain.
// Returns true when every level of `plan` matches; *badLevel / *badTexel say where it first differs (0xFFFFFFFF when equal).
inline bool VerifyChain(const swz::Plan& p, uint32_t bpp, const uint8_t* lin, const uint8_t* swzData, uint32_t* badLevel, uint32_t* badTexel) {
    if (badLevel) *badLevel = 0xFFFFFFFFu;
    if (badTexel) *badTexel = 0xFFFFFFFFu;
    for (unsigned l = 0; l < p.levels; ++l) {
        const uint32_t w = p.w[l], h = p.h[l];
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* a = lin + p.linOff[l] + static_cast<size_t>(y) * p.linStride[l] + static_cast<size_t>(x) * bpp;
                const uint8_t* b = swzData + p.swzOff[l] + static_cast<size_t>(swz::SwizzledIndex(x, y, w, h)) * bpp;
                if (std::memcmp(a, b, bpp) != 0) {
                    if (badLevel) *badLevel = l;
                    if (badTexel) *badTexel = y * w + x;
                    return false;
                }
            }
    }
    return true;
}

// ---- the scratch pool
using AllocFn = void* (*)(uint32_t size);
using FreeFn = void (*)(void* p);
using WaitFn = void (*)(); // waits for the transfer unit and calls Flush() (the host's XferWait)
struct Hooks {
    AllocFn alloc = nullptr;
    FreeFn free = nullptr;
    WaitFn waitAll = nullptr;
};
struct VerifyMeta {
    bool set = false;
    swz::Plan plan;
    uint32_t bpp = 0;
    const uint8_t* swz = nullptr;
};
struct Stats {
    uint64_t acquired = 0, reused = 0, allocated = 0, plain = 0, flushes = 0, forcedWaits = 0, allocFailures = 0, verified = 0, verifyFails = 0;
};

class LinPool {
public:
    enum State : uint8_t { kFree, kBusy, kPending };
    struct Block {
        void* p = nullptr;
        uint32_t cap = 0;
        bool pooled = false;
        State st = kBusy;
        VerifyMeta meta;
    };
    LinPool() = default;
    LinPool(const Hooks& h, const Config& c) : hooks_(h), cfg_(c) {}
    void Setup(const Hooks& h, const Config& c) { hooks_ = h; cfg_ = c; }

    // A block of at least `size` bytes, Busy. nullptr: no memory even after waiting for the pending ones.
    void* Acquire(uint32_t size) {
        ++st_.acquired;
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (void* p = TryAcquire(size)) return p;
            if (pendingCount_ == 0) break;
            ++st_.forcedWaits;
            if (hooks_.waitAll) hooks_.waitAll(); // calls Flush()
            else Flush();
        }
        ++st_.allocFailures;
        return nullptr;
    }
    // Give a block back. deferred: transfers that read it are still pending (it becomes Free / is freed at the next Flush). Returns true when
    // the pending bytes are over cfg.pendingCap (the caller should wait now). meta: what to verify before it is given back (may be null).
    bool Release(void* p, bool deferred, const VerifyMeta* meta = nullptr) {
        Block* b = Find(p);
        if (!b) return false;
        if (meta) b->meta = *meta;
        if (deferred) {
            b->st = kPending;
            ++pendingCount_;
            pendingBytes_ += b->cap;
            return pendingBytes_ > cfg_.pendingCap;
        }
        Give(*b);
        Compact();
        return false;
    }
    // The transfer unit is idle: every Pending block is given back.
    void Flush() {
        ++st_.flushes;
        for (Block& b : blocks_)
            if (b.st == kPending) Give(b);
        pendingCount_ = 0;
        pendingBytes_ = 0;
        Compact();
    }
    // Free the pool's own blocks (the test's teardown; the game never calls it).
    void Drain() {
        Flush();
        for (Block& b : blocks_)
            if (b.p && hooks_.free) hooks_.free(b.p);
        blocks_.clear();
        poolTotal_ = 0;
    }
    uint32_t PendingBytes() const { return pendingBytes_; }
    uint32_t PendingCount() const { return pendingCount_; }
    uint32_t PoolBytes() const { return poolTotal_; }
    size_t Blocks() const { return blocks_.size(); }
    const Stats& St() const { return st_; }
    // Verification result sink: called with every block that carries a meta, as it is given back.
    using VerifyFn = void (*)(const Block& b, const void* lin);
    void SetVerify(VerifyFn f) { verify_ = f; }

private:
    static uint32_t ClassOf(uint32_t size) {
        uint32_t c = 16384;
        while (c < size) c <<= 1;
        return c;
    }
    void* TryAcquire(uint32_t size) {
        if (cfg_.scratch) {
            const uint32_t cls = ClassOf(size);
            if (cls <= cfg_.poolBytes / 2) {
                for (Block& b : blocks_)
                    if (b.pooled && b.st == kFree && b.cap >= size && b.cap <= cls * 2) { // (a block of this class or the next one up)
                        b.st = kBusy;
                        ++st_.reused;
                        return b.p;
                    }
                if (poolTotal_ + cls <= cfg_.poolBytes) {
                    void* p = hooks_.alloc ? hooks_.alloc(cls) : nullptr;
                    if (p) {
                        Block b;
                        b.p = p; b.cap = cls; b.pooled = true; b.st = kBusy;
                        blocks_.push_back(b);
                        poolTotal_ += cls;
                        ++st_.allocated;
                        return p;
                    }
                }
            }
        }
        void* p = hooks_.alloc ? hooks_.alloc(size) : nullptr;
        if (!p) return nullptr;
        Block b;
        b.p = p; b.cap = size; b.pooled = false; b.st = kBusy;
        blocks_.push_back(b);
        ++st_.plain;
        return p;
    }
    Block* Find(void* p) {
        for (Block& b : blocks_)
            if (b.p == p) return &b;
        return nullptr;
    }
    void Give(Block& b) {
        if (b.meta.set && verify_) verify_(b, b.p);
        b.meta = VerifyMeta{};
        if (b.pooled) {
            b.st = kFree;
        } else {
            if (hooks_.free) hooks_.free(b.p);
            b.p = nullptr; // (removed by Compact)
        }
    }
    void Compact() {
        size_t o = 0;
        for (size_t i = 0; i < blocks_.size(); ++i)
            if (blocks_[i].p) blocks_[o++] = blocks_[i];
        blocks_.resize(o);
    }
    Hooks hooks_;
    Config cfg_;
    std::vector<Block> blocks_;
    uint32_t poolTotal_ = 0, pendingBytes_ = 0, pendingCount_ = 0;
    Stats st_;
    VerifyFn verify_ = nullptr;
};

// ---- the census: where one swizzled upload's time goes (microseconds, cumulative)
enum Phase { kAlloc, kCopy, kXfer, kWait, kSetup, kPhases };
inline const char* PhaseName(int i) {
    static const char* n[kPhases] = {"alloc", "copy", "xfer-issue", "wait", "setup"};
    return n[i];
}
struct Census {
    uint64_t n = 0, bytes = 0, us[kPhases] = {}, kindN[6] = {}, mipped = 0, fallbacks = 0;
    uint64_t sizeN[7] = {}; // level-0 bytes: <=4K, <=16K, <=64K, <=256K, <=1M, >1M ... [6] unused slot kept for the Line's total
    uint64_t sizeUs[7] = {};
    static int SizeBucket(uint32_t b) { return b <= 4096 ? 0 : b <= 16384 ? 1 : b <= 65536 ? 2 : b <= 262144 ? 3 : b <= 1048576 ? 4 : 5; }
    void Note(int kind, uint32_t level0Bytes, bool mips, const uint64_t* phaseUs) {
        ++n;
        bytes += level0Bytes;
        if (kind >= 0 && kind < 6) ++kindN[kind];
        if (mips) ++mipped;
        uint64_t tot = 0;
        for (int i = 0; i < kPhases; ++i) { us[i] += phaseUs[i]; tot += phaseUs[i]; }
        const int s = SizeBucket(level0Bytes);
        ++sizeN[s];
        sizeUs[s] += tot;
    }
};
// "texup-census: swizzled uploads N (X KiB, M with mips): per upload alloc a, copy c, xfer-issue x, wait w, setup s = T us; by size ...; by kind ..."
inline int Line(char* out, size_t cap, const Census& c, const Stats* pool) {
    if (!c.n) return std::snprintf(out, cap, "texup-census: swizzled uploads 0");
    const double n = static_cast<double>(c.n);
    uint64_t tot = 0;
    for (int i = 0; i < kPhases; ++i) tot += c.us[i];
    int len = std::snprintf(out, cap, "texup-census: swizzled uploads %llu (%llu KiB, %llu with mips): per upload", static_cast<unsigned long long>(c.n),
                            static_cast<unsigned long long>(c.bytes >> 10), static_cast<unsigned long long>(c.mipped));
    for (int i = 0; i < kPhases && len > 0 && static_cast<size_t>(len) < cap; ++i)
        len += std::snprintf(out + len, cap - static_cast<size_t>(len), "%s %s %.0f us", i ? "," : "", PhaseName(i), static_cast<double>(c.us[i]) / n);
    static const char* sz[6] = {"<=4K", "<=16K", "<=64K", "<=256K", "<=1M", ">1M"};
    if (len > 0 && static_cast<size_t>(len) < cap) len += std::snprintf(out + len, cap - static_cast<size_t>(len), " = %.0f us; by level-0 size:", static_cast<double>(tot) / n);
    for (int i = 0; i < 6 && len > 0 && static_cast<size_t>(len) < cap; ++i)
        if (c.sizeN[i]) len += std::snprintf(out + len, cap - static_cast<size_t>(len), " %s %llu x %.0f us", sz[i], static_cast<unsigned long long>(c.sizeN[i]), static_cast<double>(c.sizeUs[i]) / static_cast<double>(c.sizeN[i]));
    static const char* kn[6] = {"RGBA8", "RGB565", "RGBA5551", "RGBA4444", "LA88", "I8"};
    if (len > 0 && static_cast<size_t>(len) < cap) len += std::snprintf(out + len, cap - static_cast<size_t>(len), "; by kind:");
    for (int i = 0; i < 6 && len > 0 && static_cast<size_t>(len) < cap; ++i)
        if (c.kindN[i]) len += std::snprintf(out + len, cap - static_cast<size_t>(len), " %s %llu", kn[i], static_cast<unsigned long long>(c.kindN[i]));
    if (pool && len > 0 && static_cast<size_t>(len) < cap)
        len += std::snprintf(out + len, cap - static_cast<size_t>(len), "; scratch: %llu taken, %llu reused, %llu pool blocks, %llu plain, %llu waits forced, %llu failures; verified %llu, differing %llu",
                             static_cast<unsigned long long>(pool->acquired), static_cast<unsigned long long>(pool->reused), static_cast<unsigned long long>(pool->allocated),
                             static_cast<unsigned long long>(pool->plain), static_cast<unsigned long long>(pool->forcedWaits), static_cast<unsigned long long>(pool->allocFailures),
                             static_cast<unsigned long long>(pool->verified), static_cast<unsigned long long>(pool->verifyFails));
    return len;
}

} // namespace vgx::texupx
