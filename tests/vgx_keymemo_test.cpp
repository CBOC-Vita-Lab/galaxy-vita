// Host test of the shader-key memo table (platform/vgx/src/vgx_keymemo.h, used by the resolution cache in vgx_draw.cpp).
// The table must never change what a draw resolves to: a hit returns exactly what the miss path stored for that key, whatever the
// associativity, and only the number of misses differs. The model below is the cached path's loop (find, else compute and insert)
// with a pure function standing in for "build the keys and look the shaders up". Run through tools/test_vgx_keymemo.sh.
#include "vgx_keymemo.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <tuple>
#include <vector>

using vgx::KeyMemo;
using vgx::KeyMemoTable;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct K { uint64_t h; uint32_t mask; uint8_t spo; };
// the miss path: shader ids the registry would return for this key (any pure function will do)
static uint16_t FsOf(const K& k) { return static_cast<uint16_t>((k.h * 31u + k.mask * 7u + k.spo) % 600u); }
static uint16_t VsOf(const K& k) { return static_cast<uint16_t>((k.h * 17u + k.mask * 3u + k.spo * 5u + 1u) % 300u); }

struct Run { uint64_t hits = 0, misses = 0; bool exact = true; };

// The memo as the first version had it: direct-mapped, 1,024 entries.
struct OldMemo {
    KeyMemo e[1024];
    static uint32_t Slot(uint64_t h, uint32_t mask, uint8_t spo) {
        const uint32_t x = static_cast<uint32_t>(h) ^ static_cast<uint32_t>(h >> 32) ^ (mask * 0x9E3779B1u) ^ (spo * 0x7F4A7C15u);
        return (x ^ (x >> 15)) & 1023u;
    }
};
static Run RunOld(const std::vector<K>& trace) {
    OldMemo m;
    Run r;
    for (const K& k : trace) {
        KeyMemo& e = m.e[OldMemo::Slot(k.h, k.mask, k.spo)];
        uint16_t fs, vs;
        if (e.valid && e.h == k.h && e.vcdMtxMask == k.mask && e.spo == k.spo) { fs = e.fsId; vs = e.vsId; ++r.hits; }
        else {
            fs = FsOf(k); vs = VsOf(k);
            e = KeyMemo{};
            e.h = k.h; e.vcdMtxMask = k.mask; e.spo = k.spo; e.fsId = fs; e.vsId = vs; e.valid = 1;
            ++r.misses;
        }
        if (fs != FsOf(k) || vs != VsOf(k)) r.exact = false;
    }
    return r;
}
static Run RunNew(const std::vector<K>& trace, uint32_t ways) {
    static KeyMemoTable* t = new KeyMemoTable; // (large: not on the stack)
    *t = KeyMemoTable();
    Run r;
    for (const K& k : trace) {
        uint32_t slot = 0;
        uint16_t fs, vs;
        if (const KeyMemo* hit = t->Find(k.h, k.mask, k.spo, ways, slot)) { fs = hit->fsId; vs = hit->vsId; ++r.hits; }
        else {
            fs = FsOf(k); vs = VsOf(k);
            KeyMemo& e = t->Insert(k.h, k.mask, k.spo, ways, slot);
            e.fsId = fs; e.vsId = vs; e.valid = 1;
            ++r.misses;
        }
        if (fs != FsOf(k) || vs != VsOf(k)) r.exact = false;
    }
    return r;
}

static uint64_t rng = 0x243F6A8885A308D3ull;
static uint64_t Rand() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

// A frame-like trace: `live` distinct keys, drawn in a fixed order that repeats (as a picture repeats its materials), with a
// skew (a few keys used much more often) and some one-off keys.
static std::vector<K> FrameTrace(uint32_t live, uint32_t draws, uint32_t frames) {
    std::vector<K> keys;
    for (uint32_t i = 0; i < live; ++i) keys.push_back(K{Rand(), static_cast<uint32_t>(Rand() & 3u) ? 0u : static_cast<uint32_t>(Rand() & 0xFFFFu), static_cast<uint8_t>(Rand() & 1u)});
    std::vector<uint32_t> order;
    for (uint32_t d = 0; d < draws; ++d) order.push_back((Rand() & 1u) ? static_cast<uint32_t>(Rand() % (live / 8 + 1)) : static_cast<uint32_t>(Rand() % live));
    std::vector<K> t;
    for (uint32_t f = 0; f < frames; ++f)
        for (uint32_t i : order) t.push_back(keys[i]);
    return t;
}

int main() {
    // 1. A zero-initialised table has no entry: the key (0, 0, 0) must miss (it matches the cleared fields, not the valid flag).
    {
        KeyMemoTable t;
        uint32_t slot;
        CHECK(t.Find(0, 0, 0, 4, slot) == nullptr);
        CHECK(t.Find(0, 0, 0, 1, slot) == nullptr);
        KeyMemo& e = t.Insert(0, 0, 0, 4, slot);
        CHECK(!e.valid); // (the caller marks it valid once the ids are filled in, as the cached path does)
        e.valid = 1; e.fsId = 5; e.vsId = 6;
        const KeyMemo* h = t.Find(0, 0, 0, 4, slot);
        CHECK(h && h->fsId == 5 && h->vsId == 6 && &t.At(slot) == h);
    }
    // 2. Keys of one set: all `ways` of them live together; the next insertion evicts the oldest, nothing else.
    for (uint32_t ways = 1; ways <= 4; ++ways) {
        KeyMemoTable t;
        std::vector<K> same;
        for (uint64_t h = 1; same.size() < 6; ++h)
            if (KeyMemoTable::Set(h, 0, 0) == KeyMemoTable::Set(1, 0, 0)) same.push_back(K{h, 0, 0});
        uint32_t slot;
        for (uint32_t i = 0; i < ways; ++i) {
            KeyMemo& e = t.Insert(same[i].h, 0, 0, ways, slot);
            e.fsId = static_cast<uint16_t>(i); e.valid = 1;
        }
        for (uint32_t i = 0; i < ways; ++i) CHECK(t.Find(same[i].h, 0, 0, ways, slot) && t.At(slot).fsId == i);
        KeyMemo& e = t.Insert(same[ways].h, 0, 0, ways, slot);
        e.fsId = 99; e.valid = 1;
        CHECK(t.Find(same[0].h, 0, 0, ways, slot) == nullptr);                  // the oldest is gone
        for (uint32_t i = 1; i < ways; ++i) CHECK(t.Find(same[i].h, 0, 0, ways, slot) != nullptr);
        CHECK(t.Find(same[ways].h, 0, 0, ways, slot) != nullptr);
        // an entry of another set is untouched
        K other{1, 0, 0};
        for (uint64_t h = 1;; ++h) if (KeyMemoTable::Set(h, 0, 0) != KeyMemoTable::Set(1, 0, 0)) { other = K{h, 0, 0}; break; }
        t.Insert(other.h, 0, 0, ways, slot).valid = 1;
        CHECK(t.Find(other.h, 0, 0, ways, slot) != nullptr);
    }
    // 3. The fold id shares the entry: an eviction between two lookups is seen through the key stored in the slot (what
    //    FoldShaderCached checks), never through the slot number alone.
    {
        KeyMemoTable t;
        std::vector<K> same;
        for (uint64_t h = 1; same.size() < 5; ++h)
            if (KeyMemoTable::Set(h, 0, 0) == KeyMemoTable::Set(1, 0, 0)) same.push_back(K{h, 0, 0});
        uint32_t cur = 0, slot;
        for (int i = 0; i < 4; ++i) { KeyMemo& e = t.Insert(same[i].h, 0, 0, 4, slot); e.valid = 1; if (i == 0) cur = slot; }
        KeyMemo& e = t.Insert(same[4].h, 0, 0, 4, slot); // evicts key 0
        e.valid = 1;
        CHECK(slot == cur);
        CHECK(t.At(cur).h == same[4].h && t.At(cur).h != same[0].h);
    }
    // 4. Whatever the table does, every lookup resolves to the pure function's value; compare misses with the first version.
    const uint32_t lives[] = {40, 300, 900, 1500, 3000};
    for (uint32_t live : lives) {
        const std::vector<K> trace = FrameTrace(live, 4000, 6);
        const Run o = RunOld(trace);
        std::printf("live keys %4u: %7zu lookups, misses: first version (1024 direct) %6llu", live, trace.size(), static_cast<unsigned long long>(o.misses));
        CHECK(o.exact);
        uint64_t prev = ~0ull;
        for (uint32_t ways = 1; ways <= 4; ++ways) {
            const Run n = RunNew(trace, ways);
            std::printf(", %uw %6llu", ways, static_cast<unsigned long long>(n.misses));
            CHECK(n.exact);
            CHECK(n.hits + n.misses == trace.size());
            if (ways == 4) {
                CHECK(n.misses <= o.misses);
                if (live >= 300 && live <= 1500) CHECK(n.misses * 2 <= o.misses + 64); // the point of the change
            }
            (void)prev;
            prev = n.misses;
        }
        std::printf("\n");
    }
    // 5. Many random keys with repeats: exactness only.
    {
        std::vector<K> t;
        for (int i = 0; i < 200000; ++i) t.push_back(K{Rand() % 5000, static_cast<uint32_t>(Rand() % 3), static_cast<uint8_t>(Rand() % 2)});
        for (uint32_t ways = 1; ways <= 4; ++ways) { const Run n = RunNew(t, ways); CHECK(n.exact); CHECK(n.hits + n.misses == t.size()); }
        CHECK(RunOld(t).exact);
    }
    std::printf(failures ? "FAILED: %d\n" : "vgx_keymemo: all checks passed\n", failures);
    return failures ? 1 : 0;
}
