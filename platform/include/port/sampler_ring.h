// sampler30 (16 Nov 2026): the pure part of the on-device sampling profiler (platform/src/port/sampler_vita.cpp): settings parsing, the fixed-size
// ring of samples, and the report writer. No system calls and no allocation here (the ring lives in storage the caller hands over), so a host test
// (tests/sampler_ring_test.cpp, tools/test_sampler.sh) runs the real code with fake samples.
//
// A sample is three words: pc, lr and a tag (slot | kind << 8 | waitType << 16). kind 0 = the thread was running (pc and lr read from its registers),
// kind 1 = it was not running (a wait: pc = the wait id, lr = 0, waitType in the tag). The ring keeps the NEWEST `cap` samples (the oldest are
// overwritten and counted as dropped), so a long window still ends with its last seconds.
//
// Report file (text, ux0:data/smg/sampler-<time>.txt), one record a line, fields separated by single spaces:
//   SMGSAMPLER 1
//   period_us N / window_ms N / reason TEXT / stage TEXT / pushed N / kept N / dropped N / ring N / pc_word I / lr_word I / anchor NAME 0xADDR
//   thread SLOT UID_HEX NAME FAILS          (one per slot used)
//   run SLOT N                               (samples with the thread running)
//   wait SLOT TYPE N                         (samples with the thread waiting, by wait type; the wait id is not kept)
//   pc SLOT 0xPC 0xLR N                      (running samples by pc / lr pair, most counts first within a slot)
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace port { namespace sampler {

constexpr int kMaxSlots = 12;
constexpr uint32_t kMinPeriodUs = 250, kMaxPeriodUs = 10000, kDefaultPeriodUs = 1000;
constexpr uint32_t kDefaultMaxSec = 120;   // (a window with no GXV_SAMPLER_SEC and no stage change ends here)

// GXV_SAMPLER_US: a number clamped to 250..10000; unset, empty or not a number = 1000.
inline uint32_t ParsePeriod(const char* s) {
    if (!s || !*s) return kDefaultPeriodUs;
    char* end = nullptr;
    const long v = strtol(s, &end, 10);
    if (end == s) return kDefaultPeriodUs;
    if (v < static_cast<long>(kMinPeriodUs)) return kMinPeriodUs;
    if (v > static_cast<long>(kMaxPeriodUs)) return kMaxPeriodUs;
    return static_cast<uint32_t>(v);
}

// A number of seconds (GXV_SAMPLER_SEC, GXV_SAMPLER_DELAY_SEC): 0 when unset or not a positive number; at most 3600.
inline uint32_t ParseSeconds(const char* s) {
    if (!s || !*s) return 0;
    char* end = nullptr;
    const long v = strtol(s, &end, 10);
    if (end == s || v <= 0) return 0;
    return v > 3600 ? 3600u : static_cast<uint32_t>(v);
}

// GXV_SAMPLER_SECS (fx59): the interval of the sampler-<n>.bin dumps. Unset, empty or not a number = 20; a number <= 0 (the plain "0") = 0 = NO dumps; else 1..3600.
inline uint32_t ParseDumpSecs(const char* s) {
    if (!s || !*s) return 20;
    char* end = nullptr;
    const long v = strtol(s, &end, 10);
    if (end == s) return 20;
    if (v <= 0) return 0;
    return v > 3600 ? 3600u : static_cast<uint32_t>(v);
}

// GXV_SAMPLER_THREADS: a comma list of game, guest, render, helper, or the name of a thread (a registered name or a kernel name). Unset or empty = game.
struct ThreadSel {
    bool game = false, guest = false, render = false, helper = false;
    int nExtra = 0;
    char extra[4][24] = {};
};
inline void ParseThreads(const char* s, ThreadSel& t) {
    t = ThreadSel();
    if (!s || !*s) { t.game = true; return; }
    while (*s) {
        while (*s == ',' || *s == ' ') ++s;
        const char* b = s;
        while (*s && *s != ',') ++s;
        const char* e = s;
        while (e > b && e[-1] == ' ') --e;
        const size_t n = static_cast<size_t>(e - b);
        if (!n) continue;
        if (n == 4 && !strncmp(b, "game", 4)) t.game = true;
        else if (n == 5 && !strncmp(b, "guest", 5)) t.guest = true;
        else if (n == 6 && !strncmp(b, "render", 6)) t.render = true;
        else if (n == 6 && !strncmp(b, "helper", 6)) t.helper = true;
        else if (t.nExtra < 4 && n < sizeof t.extra[0]) { memcpy(t.extra[t.nExtra], b, n); t.extra[t.nExtra][n] = 0; ++t.nExtra; }
    }
    if (!t.game && !t.guest && !t.render && !t.helper && !t.nExtra) t.game = true;
}

// Case-insensitive "stage name contains the filter" (an empty filter matches every stage).
inline bool StageMatches(const char* filter, const char* stage) {
    if (!filter || !*filter) return true;
    if (!stage) return false;
    const size_t n = strlen(filter);
    for (const char* p = stage; *p; ++p) {
        size_t i = 0;
        for (; i < n; ++i) {
            char a = p[i], b = filter[i];
            if (!a) return false;
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
            if (a != b) break;
        }
        if (i == n) return true;
    }
    return false;
}

struct Rec { uint32_t pc, lr, tag; };
enum Kind : uint32_t { kRun = 0, kWait = 1 };
inline uint32_t MakeTag(uint32_t slot, Kind kind, uint32_t waitType) { return (slot & 0xFFu) | (static_cast<uint32_t>(kind) << 8) | ((waitType & 0xFFFFu) << 16); }
inline uint32_t TagSlot(uint32_t tag) { return tag & 0xFFu; }
inline Kind TagKind(uint32_t tag) { return static_cast<Kind>((tag >> 8) & 1u); }
inline uint32_t TagWait(uint32_t tag) { return tag >> 16; }

// The ring: storage and capacity from the caller; Push never allocates and never blocks. One writer (the sampler thread).
class Ring {
public:
    void Init(Rec* storage, uint32_t cap) { buf_ = storage; cap_ = cap; pushed_ = 0; }
    void Push(uint32_t pc, uint32_t lr, uint32_t tag) {
        if (!cap_) return;
        Rec& r = buf_[pushed_ % cap_];
        r.pc = pc; r.lr = lr; r.tag = tag;
        ++pushed_;
    }
    uint64_t Pushed() const { return pushed_; }
    uint32_t Kept() const { return pushed_ < cap_ ? static_cast<uint32_t>(pushed_) : cap_; }
    uint64_t Dropped() const { return pushed_ - Kept(); }
    uint32_t Capacity() const { return cap_; }
    Rec* Data() { return buf_; }
    void Clear() { pushed_ = 0; }
private:
    Rec* buf_ = nullptr;
    uint32_t cap_ = 0;
    uint64_t pushed_ = 0;
};

struct SlotInfo { uint32_t uid = 0; char name[24] = {}; uint32_t fails = 0; bool used = false; };

struct Header {
    uint32_t periodUs = kDefaultPeriodUs, windowMs = 0;
    char reason[24] = {};     // why the window closed: seconds / stage-change / shutdown / failures
    char stage[64] = {};      // the stage that was playing when the window opened
    int pcWord = -1, lrWord = -1;
    char anchorName[32] = {};
    uint32_t anchorAddr = 0;
    SlotInfo slots[kMaxSlots];
};

struct SlotTotals { uint32_t run = 0, wait = 0; uint32_t waitByType[8] = {}; uint16_t waitTypes[8] = {}; int nWaitTypes = 0; };

inline bool RecLess(const Rec& a, const Rec& b) {
    if (a.tag != b.tag) return a.tag < b.tag;
    if (a.pc != b.pc) return a.pc < b.pc;
    return a.lr < b.lr;
}

// An in-place sort of the kept samples (insertion sort for small runs, otherwise heap sort: no recursion, no allocation, O(n log n)).
inline void SortRecs(Rec* r, uint32_t n) {
    if (n < 2) return;
    auto sift = [&](uint32_t root, uint32_t end) {
        for (;;) {
            uint32_t c = 2 * root + 1;
            if (c >= end) return;
            if (c + 1 < end && RecLess(r[c], r[c + 1])) ++c;
            if (!RecLess(r[root], r[c])) return;
            const Rec t = r[root]; r[root] = r[c]; r[c] = t;
            root = c;
        }
    };
    for (uint32_t i = n / 2; i-- > 0;) sift(i, n);
    for (uint32_t end = n - 1; end > 0; --end) {
        const Rec t = r[0]; r[0] = r[end]; r[end] = t;
        sift(0, end);
    }
}

// Per-slot totals from the (sorted or unsorted) kept samples. Wait types beyond 8 distinct per slot are folded into the last one.
inline void Totals(const Ring& ring, SlotTotals* out /* kMaxSlots */) {
    for (int i = 0; i < kMaxSlots; ++i) out[i] = SlotTotals();
    Ring& rr = const_cast<Ring&>(ring);
    const Rec* d = rr.Data();
    const uint32_t n = ring.Kept();
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t slot = TagSlot(d[i].tag);
        if (slot >= static_cast<uint32_t>(kMaxSlots)) continue;
        SlotTotals& t = out[slot];
        if (TagKind(d[i].tag) == kRun) { ++t.run; continue; }
        ++t.wait;
        const uint16_t wt = static_cast<uint16_t>(TagWait(d[i].tag));
        int k = 0;
        while (k < t.nWaitTypes && t.waitTypes[k] != wt) ++k;
        if (k == t.nWaitTypes) {
            if (t.nWaitTypes < 8) { t.waitTypes[t.nWaitTypes++] = wt; }
            else k = 7;
        }
        ++t.waitByType[k];
    }
}

// Sorts the kept samples (destroying their order) and writes the report. Returns the number of lines written.
inline int WriteReport(FILE* f, const Header& h, Ring& ring) {
    int lines = 0;
    const uint32_t n = ring.Kept();
    SortRecs(ring.Data(), n);
    SlotTotals tot[kMaxSlots];
    Totals(ring, tot);
    fprintf(f, "SMGSAMPLER 1\nperiod_us %u\nwindow_ms %u\nreason %s\nstage %s\npushed %llu\nkept %u\ndropped %llu\nring %u\npc_word %d\nlr_word %d\nanchor %s 0x%08x\n",
            h.periodUs, h.windowMs, h.reason[0] ? h.reason : "?", h.stage[0] ? h.stage : "?", static_cast<unsigned long long>(ring.Pushed()), n,
            static_cast<unsigned long long>(ring.Dropped()), ring.Capacity(), h.pcWord, h.lrWord, h.anchorName[0] ? h.anchorName : "-", h.anchorAddr);
    lines += 12;
    for (int s = 0; s < kMaxSlots; ++s) {
        if (!h.slots[s].used) continue;
        fprintf(f, "thread %d %08x %s %u\nrun %d %u\n", s, h.slots[s].uid, h.slots[s].name[0] ? h.slots[s].name : "?", h.slots[s].fails, s, tot[s].run);
        lines += 2;
        for (int k = 0; k < tot[s].nWaitTypes; ++k) { fprintf(f, "wait %d %u %u\n", s, tot[s].waitTypes[k], tot[s].waitByType[k]); ++lines; }
    }
    // pc / lr pairs: the sort groups equal (tag, pc, lr); within a slot the pairs follow in address order (symbolize.py sorts by count).
    uint32_t i = 0;
    while (i < n) {
        const Rec& a = ring.Data()[i];
        uint32_t j = i + 1;
        while (j < n && ring.Data()[j].tag == a.tag && ring.Data()[j].pc == a.pc && ring.Data()[j].lr == a.lr) ++j;
        if (TagKind(a.tag) == kRun) { fprintf(f, "pc %u 0x%08x 0x%08x %u\n", TagSlot(a.tag), a.pc, a.lr, j - i); ++lines; }
        i = j;
    }
    return lines;
}

// The one log.txt line: counts only (the functions come from tools/sampler/symbolize.py on a computer).
inline int FormatSummary(char* out, size_t cap, const Header& h, const Ring& ring, const char* file) {
    SlotTotals tot[kMaxSlots];
    Totals(ring, tot);
    int n = snprintf(out, cap, "sampler: %u ms window (%s), stage %s, period %u us, %u samples kept (%llu dropped):", h.windowMs, h.reason[0] ? h.reason : "?",
                     h.stage[0] ? h.stage : "?", h.periodUs, ring.Kept(), static_cast<unsigned long long>(ring.Dropped()));
    for (int s = 0; s < kMaxSlots && n > 0 && static_cast<size_t>(n) < cap; ++s) {
        if (!h.slots[s].used) continue;
        n += snprintf(out + n, cap - static_cast<size_t>(n), " %s run %u wait %u fail %u;", h.slots[s].name[0] ? h.slots[s].name : "?", tot[s].run, tot[s].wait, h.slots[s].fails);
    }
    if (n > 0 && static_cast<size_t>(n) < cap) n += snprintf(out + n, cap - static_cast<size_t>(n), " file %s", file ? file : "(none)");
    return n;
}

}}  // namespace port::sampler
