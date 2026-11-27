// GXV_UPD_LIVESET (update25, 11 Nov): a pointer set answered from an open-addressing table, for LiveActor.cpp's port_live_actor_valid.
// Built from the sorted snapshot it replaces the binary search over: the same members, one load for a hit. A header on its own so that
// tests/live_set.cpp runs the very code the game runs. Not thread safe (the game thread builds and asks).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace port {
template < class T, class Vec = std::vector< T* > >
struct PtrSet {
    Vec slots;
    unsigned shift = 32;  // 32 - log2(slots); 32: no table
    bool hasNull = false;

    static inline unsigned Slot(const T* p, unsigned sh) {
        return static_cast< unsigned >((static_cast< uint32_t >(reinterpret_cast< uintptr_t >(p) >> 3) * 0x9E3779B1u) >> sh);
    }
    void Clear() {
        slots.clear();
        hasNull = false;
        shift = 32;
    }
    bool Built() const { return shift != 32; }
    // The table for the `n` entries of `items` (any order, repeats allowed). At most half full: at least 2n slots, 16 or more, 2^24 at most
    // (a set of more than 2^23 pointers is not built: Built() stays false and the caller keeps its own search).
    template < class It >
    void Build(It items, size_t n) {
        Clear();
        if (n == 0 || n > (static_cast< size_t >(1) << 23)) {
            return;
        }
        unsigned bits = 4;
        while ((static_cast< size_t >(1) << bits) < n * 2) {
            ++bits;
        }
        slots.assign(static_cast< size_t >(1) << bits, nullptr);
        shift = 32 - bits;
        const unsigned mask = (1u << bits) - 1;
        for (size_t i = 0; i < n; ++i) {
            T* p = items[i];
            if (p == nullptr) {
                hasNull = true;
                continue;
            }
            unsigned k = Slot(p, shift);
            while (slots[k] != nullptr && slots[k] != p) {
                k = (k + 1) & mask;
            }
            slots[k] = p;
        }
    }
    // Only when Built().
    inline bool Has(const T* p) const {
        if (p == nullptr) {
            return hasNull;
        }
        const unsigned mask = (1u << (32 - shift)) - 1;
        unsigned k = Slot(p, shift);
        for (;;) {
            const T* q = slots[k];
            if (q == p) {
                return true;
            }
            if (q == nullptr) {
                return false;
            }
            k = (k + 1) & mask;
        }
    }
};
}  // namespace port
