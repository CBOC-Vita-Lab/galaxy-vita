// hot62 (22 Nov 2026): two exact memos found by the PGO block-count ranking (HOT62.md). Pure logic (no game types), host-tested by tests/hot62_memo_test.cpp.
//
// 1. ResIdxMemo (GXV_RESIDX_MEMO, default 1): ResTable::getResIndex is a linear scan of the table's hash words (310 words a call on average in the
//    PGO routes; a miss, "does this animation exist", scans the whole table). The answer depends only on (table identity, table array, count, hash)
//    and on the array's hash words, which are written once, by ResFileInfo::setName inside ResTable::add. Every ResTable constructor, newFileInfoTable and add
//    bumps one global generation; an entry is valid for the generation it was made in, the same table pointer, array pointer, count and hash, and (a hit with an
//    index) the word at the index still holds the hash. So a table is never answered from an older life of its address.
//    Scan order is kept: the memo stores what the scan returned (the FIRST index with the hash, or -1).
// 2. RailParamMemo (GXV_RAIL_MEMO, default 1): BezierRailPart::getParam(t) is a pure function of the part's 13 float words (start, three control vectors,
//    length) and t (an arc-length solve: up to 6 getLength calls of 21 square roots each). The memo keys on ALL 14 words (compared bitwise) and stores the
//    returned bits, so a hit returns exactly what the same inputs gave.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

namespace port {
namespace hot62 {

// ------------------------------------------------------------------ ResTable index memo
inline std::atomic<uint32_t>& ResGen() {
    static std::atomic<uint32_t> g{1};
    return g;
}
inline void ResBump() { ResGen().fetch_add(1, std::memory_order_seq_cst); }

struct ResEntry {
    const void* table;  // the ResTable
    const void* array;  // its mFileInfoTable
    uint32_t count, hash, gen;
    int32_t index;
};
enum { kResEntries = 512 };
struct ResMemo {
    ResEntry e[kResEntries];
    uint32_t hits = 0, misses = 0;
};
inline uint32_t ResSlot(const void* table, uint32_t hash) {
    const uint32_t a = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(table) >> 3) * 0x9E3779B1u;
    return ((a ^ (hash * 0x85EBCA6Bu)) * 0xC2B2AE35u) >> 23;  // 9 bits
}
// hashWord(i): the table's hash word i. Returns what the scan returns: the first i < count with hashWord(i) == hash, else -1.
template <class W>
inline int ResIndex(ResMemo& m, const void* table, const void* array, uint32_t count, uint32_t hash, W&& hashWord) {
    const uint32_t gen = ResGen().load(std::memory_order_acquire);  // (read BEFORE the scan: an entry is never newer than its scan)
    ResEntry& e = m.e[ResSlot(table, hash)];
    if (e.gen == gen && e.table == table && e.array == array && e.count == count && e.hash == hash &&
        (e.index < 0 || (static_cast<uint32_t>(e.index) < count && hashWord(static_cast<uint32_t>(e.index)) == hash))) {
        ++m.hits;
        return e.index;
    }
    ++m.misses;
    int r = -1;
    for (uint32_t i = 0; i < count; ++i) {
        if (hashWord(i) == hash) {
            r = static_cast<int>(i);
            break;
        }
    }
    e.table = table;
    e.array = array;
    e.count = count;
    e.hash = hash;
    e.gen = gen;
    e.index = r;
    return r;
}

// ------------------------------------------------------------------ rail parameter memo
enum { kRailWords = 14, kRailEntries = 64 };
struct RailEntry {
    uint32_t key[kRailWords];
    uint32_t result;
    uint8_t valid;
};
struct RailMemo {
    RailEntry e[kRailEntries];
    uint32_t hits = 0, misses = 0;
};
// words: the part's 13 float words as bits; t: the argument's bits. compute(): the original getParam, returns float bits.
template <class C>
inline uint32_t RailParam(RailMemo& m, const uint32_t* words13, uint32_t tbits, C&& compute) {
    uint32_t k[kRailWords];
    std::memcpy(k, words13, 13 * sizeof(uint32_t));
    k[13] = tbits;
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < kRailWords; ++i) h = (h ^ k[i]) * 0x01000193u;
    RailEntry& e = m.e[(h ^ (h >> 15)) & (kRailEntries - 1)];
    if (e.valid && std::memcmp(e.key, k, sizeof k) == 0) {
        ++m.hits;
        return e.result;
    }
    ++m.misses;
    const uint32_t r = compute();
    std::memcpy(e.key, k, sizeof k);
    e.result = r;
    e.valid = 1;
    return r;
}

}  // namespace hot62
}  // namespace port
