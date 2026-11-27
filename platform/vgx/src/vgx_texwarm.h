// endwait26 (GXV_TEXWARM): the pure parts of "decode and upload the textures a stage's first seconds use while the screen is still closed". Header only, standard library
// only, so the front end's texture cache (vgx_texcache.cpp) and the host test (tests/texwarm_model.cpp) share the code.
//
// HONEST SCOPE (read new/endwait26/REPORT.txt first). The console logs say the front end's texture decode is 12-28 ms for the 5 s window that holds each 130-360 ms
// game-thread stall (stutter-5s "front-end texture decode"), and the back end's uploads 14-33 ms. Textures are a marker of "a lot of new things drawn for the first
// time" (r 0.85 with the stall), not the cost. This switch moves at most those ~20-30 ms behind the closed screen; it is built because it is cheap, safe and measurable
// (a census line says what it did), not because the evidence says it removes the stall.
//
// WHAT THIS HOLDS
//   Item     one texture a stage used for the first time in its first kWindowMs of play: where it lay in guest memory last time (addr), its size in bytes, its XXH3 hash,
//            width, height, GX format, mip levels as the cache keyed it. Colour-indexed formats are not recorded (their key holds the palette).
//   Book     per stage up to kMaxItems in the order first used; one line an item in a text file, read and written whole.
//   Replay   the stage's items, worked off a few a picture while the screen is closed (and, in mode 2, for the first open pictures too) under a per-picture budget. An item is
//            only used when guest memory at addr still holds the same bytes (size and hash match): then it is decoded and uploaded exactly as the first bind would have
//            (same key), otherwise it is a miss, tried again a few pictures later (the stage may still be loading), and given up after kMaxMisses.
//   Budget   the per-picture limit on warm-up work (ms and KiB).
//   Census   what a stage's pass did: recorded, replayed (hits, KiB, ms), misses by reason, textures first-used in play anyway, and (mode 2) the pictures whose first-use
//            decode went over the budget and the bytes beyond it: what a cap in play would have deferred.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace vgx::texwarm {

constexpr unsigned kMaxItems = 160;         // items a stage keeps
constexpr unsigned kMaxStages = 96;         // stages the book keeps
constexpr unsigned kMaxTotal = 4096;        // items in the whole book (a file of about 250 KiB)
constexpr double kWindowMs = 15000.0;       // textures first used in play within this long after the screen opened are recorded
constexpr uint32_t kMaxSize = 4u << 20;     // a single texture over this is not recorded
constexpr unsigned kRetryGap = 24;          // pictures before a missed item is tried again
constexpr unsigned kMaxMisses = 6;          // then it is given up
constexpr uint32_t kFmtC4 = 8, kFmtC8 = 9, kFmtC14X2 = 10;

struct Item {
    uint64_t hash = 0;
    uint32_t addr = 0, size = 0;            // guest address (low 32 bits of the host pointer), bytes of the texture with its levels
    uint16_t w = 0, h = 0;
    uint8_t fmt = 0, levels = 1;
    bool operator==(const Item& o) const { return addr == o.addr && w == o.w && h == o.h && fmt == o.fmt && levels == o.levels; }
};

inline bool Valid(const Item& it) {
    return it.w >= 1 && it.h >= 1 && it.w <= 1024 && it.h <= 1024 && it.size >= 32 && it.size <= kMaxSize && it.levels >= 1 && it.levels <= 12 && it.fmt <= 14 &&
           it.fmt != kFmtC4 && it.fmt != kFmtC8 && it.fmt != kFmtC14X2 && it.addr != 0 && it.hash != 0;
}
inline bool InWindow(double nowMs, double openMs) { return openMs > 0.0 && nowMs >= openMs && nowMs - openMs <= kWindowMs; }

class Book {
public:
    // A texture first used in play. True when it is new to the stage's list (the file is then out of date).
    bool Note(uint32_t stage, const Item& it) {
        if (!stage || !Valid(it) || total_ >= kMaxTotal) return false;
        auto f = m_.find(stage);
        if (f == m_.end()) {
            if (m_.size() >= kMaxStages) return false;
            f = m_.emplace(stage, std::vector<Item>()).first;
        }
        std::vector<Item>& v = f->second;
        for (Item& o : v)
            if (o == it) {
                if (o.hash != it.hash || o.size != it.size) { o.hash = it.hash; o.size = it.size; dirty_ = true; return true; }  // (the same place holds other bytes now)
                return false;
            }
        if (v.size() >= kMaxItems) return false;
        v.push_back(it);
        ++total_;
        dirty_ = true;
        return true;
    }
    const std::vector<Item>* Items(uint32_t stage) const {
        const auto f = m_.find(stage);
        return f == m_.end() ? nullptr : &f->second;
    }
    size_t Stages() const { return m_.size(); }
    size_t Total() const { return total_; }
    uint64_t KiB(uint32_t stage) const {
        const std::vector<Item>* v = Items(stage);
        uint64_t b = 0;
        if (v) for (const Item& it : *v) b += it.size;
        return b / 1024u;
    }
    bool Dirty() const { return dirty_; }
    void Clean() { dirty_ = false; }

    // "<stage hex> <addr hex> <size> <hash hex> <w> <h> <fmt> <levels>"; the first line is the magic.
    static const char* Magic() { return "texwarm 1"; }
    std::string Serialize() const {
        std::string s = Magic();
        s += "\n";
        char line[128];
        for (const auto& kv : m_)
            for (const Item& it : kv.second) {
                std::snprintf(line, sizeof line, "%08x %08x %u %016llx %u %u %u %u\n", static_cast<unsigned>(kv.first), it.addr, it.size,
                              static_cast<unsigned long long>(it.hash), it.w, it.h, it.fmt, it.levels);
                s += line;
            }
        return s;
    }
    // Returns the items taken; a wrong magic or a bad line stops the parse (what was read stays); a last line cut short is dropped.
    unsigned Parse(const char* text, size_t len) {
        const std::string t(text, len);
        size_t pos = t.find('\n');
        if (pos == std::string::npos || t.compare(0, pos, Magic()) != 0) return 0;
        unsigned taken = 0;
        ++pos;
        while (pos < t.size()) {
            const size_t e = t.find('\n', pos);
            if (e == std::string::npos) break;
            unsigned stage = 0, addr = 0, size = 0, w = 0, h = 0, fmt = 0, lv = 0;
            unsigned long long hash = 0;
            char tail = 0;
            const std::string line = t.substr(pos, e - pos);
            pos = e + 1;
            if (std::sscanf(line.c_str(), "%x %x %u %llx %u %u %u %u%c", &stage, &addr, &size, &hash, &w, &h, &fmt, &lv, &tail) != 8) break;
            if (w > 65535 || h > 65535 || fmt > 255 || lv > 255) break;
            Item it;
            it.addr = addr; it.size = size; it.hash = hash; it.w = static_cast<uint16_t>(w); it.h = static_cast<uint16_t>(h);
            it.fmt = static_cast<uint8_t>(fmt); it.levels = static_cast<uint8_t>(lv);
            if (Note(stage, it)) ++taken;
        }
        dirty_ = false;  // (what the file holds)
        return taken;
    }
private:
    std::map<uint32_t, std::vector<Item>> m_;
    size_t total_ = 0;
    bool dirty_ = false;
};

// The per-picture limit on warm-up work. Both limits apply; one item is always allowed per picture (a picture with nothing done yet takes the next, however big).
struct Budget {
    double maxMs = 3.0;
    uint32_t maxKiB = 512;
    double usedMs = 0;
    uint32_t usedKiB = 0;
    unsigned items = 0;
    void NewPicture() { usedMs = 0; usedKiB = 0; items = 0; }
    bool Allow(uint32_t nextKiB) const { return items == 0 || (usedMs < maxMs && usedKiB + nextKiB <= maxKiB); }
    void Spend(double ms, uint32_t kib) { usedMs += ms; usedKiB += kib; ++items; }
};

enum Outcome : uint8_t { kHit = 0, kAlready, kMissBytes, kMissMemory, kDropped };

// The replay of one stage's items: which to try next, retries of the ones that missed.
class Replay {
public:
    void Begin(const std::vector<Item>* items) {
        st_.clear();
        items_ = items;
        if (items) st_.assign(items->size(), State{});
        left_ = st_.size();
        gaveUp_ = 0;  // (texup27: the census counted every stage's given-up items since the start: "given up 104" on a stage with no book)
    }
    bool Active() const { return items_ && left_ > 0; }
    size_t Left() const { return left_; }
    // openstall26: the items that can be tried at `picture` (not done, not waiting for a retry): what a closed-screen hold may wait for. A miss is tried again kRetryGap pictures
    // later, so a list whose memory is gone (another layout) is not waited for.
    size_t Due(uint64_t picture) const {
        size_t n = 0;
        for (const State& s : st_) n += (!s.done && s.nextTry <= picture) ? 1u : 0u;
        return n;
    }
    // The next item due at `picture`, or false. Items are tried in the order they were first used.
    bool Next(uint64_t picture, size_t& idx) const {
        for (size_t i = 0; i < st_.size(); ++i)
            if (!st_[i].done && st_[i].nextTry <= picture) { idx = i; return true; }
        return false;
    }
    void Result(size_t idx, Outcome o, uint64_t picture) {
        if (idx >= st_.size() || st_[idx].done) return;
        State& s = st_[idx];
        if (o == kHit || o == kAlready || o == kDropped) { s.done = true; --left_; return; }
        if (++s.misses >= kMaxMisses) { s.done = true; --left_; ++gaveUp_; return; }
        s.nextTry = picture + kRetryGap;
    }
    unsigned GaveUp() const { return gaveUp_; }
private:
    struct State { bool done = false; unsigned misses = 0; uint64_t nextTry = 0; };
    const std::vector<Item>* items_ = nullptr;
    std::vector<State> st_;
    size_t left_ = 0;
    unsigned gaveUp_ = 0;
};

struct Census {
    uint32_t stage = 0;
    unsigned recorded = 0, planned = 0;
    unsigned hit = 0, already = 0, missBytes = 0, missMemory = 0, gaveUp = 0;
    unsigned replayClosed = 0, replayOpen = 0;       // hits made while the screen was closed / open
    uint64_t hitKiB = 0;
    double hitMs = 0;
    unsigned firstUseN = 0;                          // textures first used in play within the window anyway (not replayed)
    uint64_t firstUseKiB = 0;
    double firstUseMs = 0;
    unsigned picsOver = 0, deferN = 0;               // mode 2: pictures whose first-use decode went over the budget; the textures (not Mario's) beyond it
    uint64_t deferKiB = 0;
    double deferMs = 0, worstPicMs = 0;
    void Reset(uint32_t s) { *this = Census{}; stage = s; }
    // A texture decoded for a first upload in play. `ms` its decode time, `chara` Mario's or a key item's (never deferred), picMs the picture's first-use decode so far
    // (this one included), budget the per-picture limit.
    void FirstUse(uint32_t kib, double ms, bool chara, double picMsBefore, double budgetMs, bool& newOverPicture) {
        ++firstUseN; firstUseKiB += kib; firstUseMs += ms;
        const double after = picMsBefore + ms;
        if (after > worstPicMs) worstPicMs = after;
        newOverPicture = false;
        if (after > budgetMs) {
            if (picMsBefore <= budgetMs) { ++picsOver; newOverPicture = true; }
            if (!chara) { ++deferN; deferKiB += kib; deferMs += ms; }
        }
    }
};

inline size_t Format(char* out, size_t cap, const Census& c, const char* when, int mode, size_t bookItemsForStage) {
    const int n = std::snprintf(out, cap,
        "texwarm: stage %08x %s (GXV_TEXWARM=%d): book %u items for the stage (%u planned this visit); replayed behind the closed screen %u, in the first open pictures %u "
        "(%llu KiB, %.0f ms); already there %u; missed: bytes differ %u, memory not valid %u, given up %u; first-used in play anyway %u (%llu KiB, %.0f ms, worst picture %.1f ms); "
        "over the %s budget: %u pictures, %u textures not Mario's = %llu KiB, %.0f ms (what a cap in play would have deferred; none is deferred)",
        static_cast<unsigned>(c.stage), when, mode, static_cast<unsigned>(bookItemsForStage), c.planned, c.replayClosed, c.replayOpen, static_cast<unsigned long long>(c.hitKiB),
        c.hitMs, c.already, c.missBytes, c.missMemory, c.gaveUp, c.firstUseN, static_cast<unsigned long long>(c.firstUseKiB), c.firstUseMs, c.worstPicMs, "per-picture",
        c.picsOver, c.deferN, static_cast<unsigned long long>(c.deferKiB), c.deferMs);
    return n < 0 ? 0 : (static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1);
}

}  // namespace vgx::texwarm
