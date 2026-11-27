// GXV_UPD_SNDNAME (update25, 11 Nov): a hash index over a table of (hash, name) entries that finds what a front-to-back scan finds: the FIRST
// entry at or after `start` whose hash equals the asked one and whose name passes `eq`. AudSoundNameConverter::getSoundID scanned from its
// category's first entry to the end of a table of thousands of names on every call (level sounds are asked for by name every turn, by dozens
// of actors); the chains below are in ascending entry order, so the first hit of a walk is the scan's first hit. Header only so that
// tests/name_index.cpp runs the code the game runs.
#pragma once
#include <cstdint>
#include <vector>

namespace port {
template < class Vec16 = std::vector< uint16_t > >
struct NameIndex {
    static constexpr uint16_t kEnd = 0xFFFF;
    Vec16 head, next;
    uint32_t mask = 0;
    int n = 0;
    bool built = false;

    // Entries 0 .. n-1 (n < 65535); hashOf(i) is entry i's hash. Chains by (hash & mask), ascending.
    template < class H >
    bool Build(int count, H hashOf) {
        built = false;
        n = 0;
        if (count <= 0 || count >= 0xFFFF) {
            return false;
        }
        uint32_t size = 16;
        while (size < static_cast< uint32_t >(count) * 2) {
            size <<= 1;
        }
        head.assign(size, kEnd);
        next.assign(static_cast< size_t >(count), kEnd);
        mask = size - 1;
        for (int i = count - 1; i >= 0; --i) {
            const uint32_t b = hashOf(i) & mask;
            next[static_cast< size_t >(i)] = head[b];
            head[b] = static_cast< uint16_t >(i);
        }
        n = count;
        built = true;
        return true;
    }
    // The first i in [start, n) with hashOf(i) == hash and eq(i); -1 if none.
    template < class H, class Eq >
    int Find(uint32_t hash, int start, H hashOf, Eq eq) const {
        for (uint16_t k = head[hash & mask]; k != kEnd; k = next[k]) {
            if (static_cast< int >(k) < start) {
                continue;
            }
            if (hashOf(static_cast< int >(k)) == hash && eq(static_cast< int >(k))) {
                return static_cast< int >(k);
            }
        }
        return -1;
    }
};
}  // namespace port
