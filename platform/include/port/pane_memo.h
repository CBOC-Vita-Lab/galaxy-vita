// GXV_LC_PANEMEMO (logiccut, 6 Nov): a memo of nw4r::lyt::Pane::FindPaneByName (decomp/src/nw4r/lyt/lyt_pane.cpp). Header-only, so
// that tests/lc_pane_memo.cpp runs this very code against the plain search.
//
// The HUD and the galaxy-map layouts look panes up by name every turn (MR::setLayoutScalePosAtPaneScaleTrans per dome icon,
// MR::setTextBoxFormatRecursive, MR::copyPaneTrans, MR::getPane ...): a depth-first walk over the layout's panes, a virtual call and
// a name compare per pane. The answer is a function of the root, the recursive flag, the first 16 characters of the name (up to its
// end: detail::EqualsResName compares 16 characters, strncmp-style) and of the names and child lists of the panes under the root.
// Names change only in Pane::SetName, child lists only in InsertChild / RemoveChild (AppendChild goes through InsertChild) and in a
// pane's destructor; each of those bumps the generation (Bump) and so retires every entry. A pane built at the address of a destroyed
// one finds nothing old: the destructor bumped.
// Single-threaded: layouts are built, changed and searched by the guest threads, which hold the one virtual CPU in turn and do not
// switch inside these functions (no safe point in them).
#pragma once

#include <cstdint>
#include <cstring>

namespace port {
namespace panememo {

struct Key {
    const void* root;
    uint32_t recursive;
    char name[16];  // the name's first 16 characters up to its end, zero-padded
};

inline void MakeKey(Key* k, const void* root, bool recursive, const char* name) {
    k->root = root;
    k->recursive = recursive ? 1u : 0u;
    size_t i = 0;
    for (; i < sizeof k->name && name[i] != '\0'; i++) k->name[i] = name[i];
    for (; i < sizeof k->name; i++) k->name[i] = '\0';
}

inline bool SameKey(const Key& a, const Key& b) {
    return a.root == b.root && a.recursive == b.recursive && std::memcmp(a.name, b.name, sizeof a.name) == 0;
}

inline uint32_t Hash(const Key& k) {
    uint32_t w[4];
    std::memcpy(w, k.name, sizeof w);
    uint32_t h = (uint32_t)(uintptr_t)k.root * 0x9E3779B1u ^ k.recursive;
    for (uint32_t x : w) h = (h ^ x) * 0x01000193u;
    return h ^ (h >> 15);
}

template <typename Pane, unsigned kSize = 128>
struct Memo {
    struct Entry {
        Key key;
        Pane* result;
        uint32_t gen;  // 0: empty
    };
    Entry entries[kSize] = {};
    uint32_t gen = 1;
    uint32_t hits = 0, misses = 0;

    void Bump() {
        if (++gen == 0) {  // (after 2^32 changes: forget everything, so no old entry can match again)
            std::memset(entries, 0, sizeof entries);
            gen = 1;
        }
    }
    // true: *pResult is what the search would answer
    bool Find(const Key& k, uint32_t h, Pane** pResult) {
        const Entry& e = entries[h & (kSize - 1)];
        if (e.gen != gen || !SameKey(e.key, k)) {
            misses++;
            return false;
        }
        hits++;
        *pResult = e.result;
        return true;
    }
    void Store(const Key& k, uint32_t h, Pane* result) {
        Entry& e = entries[h & (kSize - 1)];
        e.key = k;
        e.result = result;
        e.gen = gen;
    }
};

}  // namespace panememo
}  // namespace port
