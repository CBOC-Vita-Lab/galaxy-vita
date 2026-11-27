// samp57 (22 Nov 2026): the pure part of the sampler's periodic binary dump (platform/src/port/sampler_vita.cpp): a fixed-size open-addressing histogram of
// (slot, kind, pc, lr) -> count, and the writer of ux0:data/smg/sampler-<n>.bin. No system calls, no allocation (the caller hands the storage over), so
// tests/sampler_hist_test.cpp runs the real code on the host. tools/sampler_report.py reads the file.
//
// File layout (little endian, all words uint32 unless noted):
//   BinHeader (kBinHeaderBytes)        then  nEntries x BinEntry (16 bytes)
//   BinHeader: magic "GXVSAMP1" (8 bytes), version (1), index (n of sampler-<n>.bin), periodUs, intervalMs (length of this interval), windowMs (since the window
//     opened, at the end of this interval), textBase / textSize (the running module's code segment: run-time addresses), anchorAddr (run-time address of
//     port_sampler_boot; the ELF's address of the same symbol gives the load offset), nEntries, dropped (samples that did not fit the table), runSamples,
//     waitSamples, failed (failed reads), pcWord, lrWord, then stage[48], anchorName[32], then kMaxSlots x BinSlot.
//   BinSlot (48 bytes): uid, used, stops (suspend..resume pairs), holdUs (total time from before the suspend to after the resume), holdMaxUs, fails, name[24].
//   BinEntry: pc, lr, meta (slot | kind << 8 | waitType << 16, as port/sampler_ring.h MakeTag), count.
//     kind 0 = running sample: pc / lr are code addresses. kind 1 = the thread was waiting: pc = the wait object's uid, lr = 0, waitType in meta.
#pragma once
#include "port/sampler_ring.h"

namespace port { namespace sampler {

struct BinSlot {
    uint32_t uid, used, stops, holdUs, holdMaxUs, fails;
    char name[24];
};
struct BinHeader {
    char magic[8];
    uint32_t version, index, periodUs, intervalMs, windowMs, textBase, textSize, anchorAddr, nEntries, dropped, runSamples, waitSamples, failed, pcWord, lrWord;
    char stage[48];
    char anchorName[32];
    BinSlot slots[kMaxSlots];
};
struct BinEntry { uint32_t pc, lr, meta, count; };
constexpr uint32_t kBinHeaderBytes = sizeof(BinHeader);
static_assert(sizeof(BinSlot) == 48, "BinSlot layout");
static_assert(sizeof(BinEntry) == 16, "BinEntry layout");
static_assert(sizeof(BinHeader) == 8 + 15 * 4 + 48 + 32 + kMaxSlots * 48, "BinHeader layout (no padding)");

class Hist {
public:
    void Init(BinEntry* storage, uint32_t cap) { t_ = storage; cap_ = cap; Clear(); }   // cap: a power of two
    void Clear() { if (t_) memset(t_, 0, sizeof(BinEntry) * cap_); used_ = 0; dropped_ = 0; }
    // count == 0 marks a free entry, so a stored entry always has count >= 1. A table 7/8 full takes no new keys (the sample is counted as dropped).
    void Add(uint32_t pc, uint32_t lr, uint32_t meta) {
        if (!cap_) return;
        uint32_t i = ((pc * 0x9e3779b1u) ^ (lr * 0x85ebca6bu) ^ (meta * 0xc2b2ae35u)) >> 7 & (cap_ - 1);
        for (uint32_t n = 0; n < 64; ++n, i = (i + 1) & (cap_ - 1)) {
            BinEntry& e = t_[i];
            if (e.count && e.pc == pc && e.lr == lr && e.meta == meta) { ++e.count; return; }
            if (!e.count) {
                if (used_ >= cap_ - cap_ / 8) break;
                e.pc = pc; e.lr = lr; e.meta = meta; e.count = 1; ++used_;
                return;
            }
        }
        ++dropped_;
    }
    uint32_t Used() const { return used_; }
    uint32_t Dropped() const { return dropped_; }
    const BinEntry* Data() const { return t_; }
    uint32_t Capacity() const { return cap_; }
private:
    BinEntry* t_ = nullptr;
    uint32_t cap_ = 0, used_ = 0, dropped_ = 0;
};

// Writes the header (nEntries and dropped filled in here) and the used entries. false on a short write.
inline bool WriteBin(FILE* f, BinHeader& h, const Hist& hist) {
    h.nEntries = hist.Used();
    h.dropped = hist.Dropped();
    if (fwrite(&h, sizeof h, 1, f) != 1) return false;
    uint32_t written = 0;
    for (uint32_t i = 0; i < hist.Capacity(); ++i)
        if (hist.Data()[i].count) { if (fwrite(&hist.Data()[i], sizeof(BinEntry), 1, f) != 1) return false; ++written; }
    return written == h.nEntries;
}

}}  // namespace port::sampler
