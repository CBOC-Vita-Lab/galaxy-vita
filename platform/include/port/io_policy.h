// disc26: the pure logic of the disc reader's play-time policies (platform/src/dvd/dvd.cpp uses it; tests/io_policy_test.cpp checks it).
// No I/O, no threads, no allocation: small tables and arithmetic, so that a host test can cover queue / read-ahead / prefetch decisions.
//
//   RaTable      the read-ahead windows: one window per recently read file (a stream, a wave bank, an archive read in pieces), so that two
//                sequences that alternate (the music stream and a wave bank) no longer evict each other's window.
//   PfBudget     the learned-prefetch cache's book-keeping: whole files, least recently used out first, never more than a byte cap.
//   ChunkPlan    how a long read is cut so that a more urgent request (a music stream) gets in between the pieces.
//   Verdict      the one word the "io:" line ends with.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace port::iopol {

// ------------------------------------------------------------------ read-ahead windows
// A window is "the bytes [offset, offset+len) of `entry` are in slot i's buffer". A file also has a sequence mark: where its last read
// ended. A read that starts exactly there is a continuation of a sequence (the next piece is worth reading ahead).
template <int N>
struct RaTable {
    struct Slot {
        uint32_t entry = ~0u;
        uint64_t offset = 0;
        size_t len = 0;
        uint64_t stamp = 0;  // last use
    };
    struct Mark {
        uint32_t entry = ~0u;
        uint64_t end = 0;
        uint64_t stamp = 0;
    };
    Slot slot[N];
    Mark mark[N];
    uint64_t clock = 0;

    // The slot that holds exactly [offset, offset+len) of `entry`, or -1.
    int Find(uint32_t entry, uint64_t offset, size_t len) {
        for (int i = 0; i < N; ++i)
            if (slot[i].entry == entry && slot[i].offset == offset && slot[i].len == len) { slot[i].stamp = ++clock; return i; }
        return -1;
    }
    // Record a finished read; true when it started where the file's previous read ended (a sequence).
    bool NoteRead(uint32_t entry, uint64_t offset, size_t len) {
        int m = -1, lru = 0;
        for (int i = 0; i < N; ++i) {
            if (mark[i].entry == entry) m = i;
            if (mark[i].stamp < mark[lru].stamp) lru = i;
        }
        bool seq = false;
        if (m >= 0) seq = mark[m].end == offset;
        else m = lru;
        mark[m] = {entry, offset + len, ++clock};
        return seq;
    }
    // The slot to fill for `entry`: its own window if it has one, else the least recently used.
    int SlotFor(uint32_t entry) {
        int lru = 0;
        for (int i = 0; i < N; ++i) {
            if (slot[i].entry == entry) return i;
            if (slot[i].stamp < slot[lru].stamp) lru = i;
        }
        return lru;
    }
    void Set(int i, uint32_t entry, uint64_t offset, size_t len) { slot[i] = {entry, offset, len, ++clock}; }
    void Clear(int i) { slot[i].entry = ~0u; slot[i].len = 0; }
    void ClearAll() { for (int i = 0; i < N; ++i) Clear(i); for (int i = 0; i < N; ++i) mark[i].entry = ~0u; }
};

// ------------------------------------------------------------------ prefetch budget
// Whole files. Admit() makes room by dropping the least recently used files; a file bigger than the cap is never admitted.
template <int N>
struct PfBudget {
    struct Item {
        uint32_t entry = ~0u;
        size_t size = 0;
        uint64_t stamp = 0;
    };
    Item item[N];
    size_t used = 0;
    uint64_t clock = 0;

    int Find(uint32_t entry) const {
        for (int i = 0; i < N; ++i) if (item[i].entry == entry) return i;
        return -1;
    }
    void Touch(int i) { item[i].stamp = ++clock; }
    // Returns the item index to fill (its buffer is free now), or -1 if `size` can not be held. `dropped` (at most N entries) receives the
    // files evicted to make room; *nDropped says how many.
    int Admit(uint32_t entry, size_t size, size_t cap, uint32_t* dropped, int* nDropped) {
        *nDropped = 0;
        if (size == 0 || size > cap || Find(entry) >= 0) return -1;
        for (;;) {
            int free = -1, lru = -1;
            for (int i = 0; i < N; ++i) {
                if (item[i].entry == ~0u) { if (free < 0) free = i; continue; }
                if (lru < 0 || item[i].stamp < item[lru].stamp) lru = i;
            }
            if (free >= 0 && used + size <= cap) { item[free] = {entry, size, ++clock}; used += size; return free; }
            if (lru < 0) return -1;
            used -= item[lru].size;
            dropped[(*nDropped)++] = item[lru].entry;
            item[lru] = Item{};
        }
    }
    void Drop(int i) { if (item[i].entry != ~0u) { used -= item[i].size; item[i] = Item{}; } }
};

// ------------------------------------------------------------------ chunk plan
// The next piece of a long read: `chunk` bytes at most (0 = no cutting: the whole rest at once).
inline size_t NextPiece(size_t remaining, size_t chunk) { return (chunk == 0 || remaining <= chunk) ? remaining : chunk; }
// May a request of priority `urgent` go in front of a read of priority `running` that is between two pieces? The SDK serves 0..3,
// lower is more urgent; only the audio streams (1) and below preempt, and only archive-class reads (>= 2).
inline bool MayInterleave(int urgent, int running) { return urgent <= 1 && running >= 2; }

// ------------------------------------------------------------------ the verdict word of the "io:" line
// worst play read (ms), reads of 20 ms or more, ms the game thread sat in disc waits, worst stream-read latency (ms).
inline const char* Verdict(double gameBlockedMs, double worstGameWaitMs, double worstStreamLatencyMs, unsigned slow50, unsigned slow20) {
    if (worstGameWaitMs >= 16.0 || gameBlockedMs >= 50.0) return "HITCH: the game thread waited for the card";
    if (worstStreamLatencyMs >= 250.0) return "AUDIO AT RISK: a stream read took 250 ms or more";
    if (slow50) return "SLOW READS (not waited for by the game thread)";
    if (slow20) return "slow reads below 50 ms";
    return "CLEAN";
}

}  // namespace port::iopol
