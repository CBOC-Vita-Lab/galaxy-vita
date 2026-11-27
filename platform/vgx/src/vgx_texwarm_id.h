// texwarm30 (GXV_TEXWARM_KEY=1, the default of the replay once GXV_TEXWARM is on): the pure parts of "find a stage's textures again at its next load by WHAT THEY ARE, not WHERE THEY LAY".
// Header only, standard library only, so the front end's texture cache (vgx_texcache.cpp) and the host test (tests/texwarm_id_model.cpp) share the code.
//
// WHY. The first book (vgx_texwarm.h) keyed an item by its guest ADDRESS and checked the bytes there. The console tour c90 (logs6/2111) shows what that gives: 1249 items planned,
// 462 replayed (37 %), 4648 tries "bytes differ": a stage's archives land at other addresses at every visit, so a book written at visit 1 finds its textures only where the heap
// happens to put them again. The misses were first used in play anyway (790 textures), and each of them was then noted under its NEW address, so the book also grew by a duplicate
// of every texture at every visit until its 160 items a stage were full of stale addresses (book 160 items, 119 planned, 57 found, in 6 of 22 stages).
//
// THE KEY. A texture is its bytes: (width, height, GX format, mip levels as the cache keys them, size in bytes, XXH3-64 of those bytes = the cache's own contentHash, taken at the first
// upload: no new work) plus a QUICK KEY, the XXH3-64 of the first min(256, level-0 size) bytes, that finds the candidate without hashing every texture of the stage. Where a
// texture lies is not part of the key. The replay gets the CANDIDATES from the game: at the cover every J3DModelData the stage holds is walked (decomp/src/Game/System/
// PortTexEnum.cpp) and each texture of each model reports (address, size, format, mip count, filter). A book item is found by (w, h, fmt, quick key) among them and CONFIRMED by the
// full hash of its bytes at the candidate's address, which is the very hash the cache takes before it uploads: what is uploaded is exactly what the game's first bind would upload.
//
// WHAT THIS HOLDS
//   Item     identity + the first-use time last visit (ms after the screen opened: the replay order) + a use count (what to evict).
//   Book     per stage up to kMaxItems items, same identity once; one text line an item, one file a stage (<= ~18 KiB: the asynchronous card writer's 20 KiB slot);
//            "texwarm 2". A "texwarm 1" file (address keyed) is still read: its items have no quick key (found by the slower scan) and no time.
//   CandIndex  the candidates of the stage being loaded, found by (w, h, fmt, quick key).
//   Sched    the order and pacing of the replay: most likely first, retry gaps, a wait for the walk that costs no miss, a per-picture budget (ms, KiB, items).
//   Census   the line the task asked for.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace vgx::texid {

constexpr unsigned kMaxItems = 224;         // items a stage keeps (a 224-line file is ~18 KiB)
constexpr unsigned kMaxStages = 96;
constexpr unsigned kMaxTotal = 8192;
constexpr double kWindowMs = 15000.0;       // textures first used in play within this long after the screen opened are recorded
constexpr uint32_t kMaxSize = 4u << 20;
constexpr unsigned kQuickBytes = 256;       // the quick key covers this many bytes of level 0 (less for a smaller texture)
constexpr unsigned kRetryGap = 24;          // pictures before a missed item is tried again
constexpr unsigned kMaxMisses = 6;
constexpr unsigned kNoCandCost = 3;         // a miss for "no candidate" counts this much (the candidate set does not change once the walk is over): two tries, not six
constexpr unsigned kMaxWaitPics = 1200;     // an item that waited this many pictures for the walk is treated as having no candidate (the walk never came)
constexpr unsigned kMaxScan = 12;           // an item without a quick key (a v1 book's) hashes at most this many same-shape candidates
constexpr uint32_t kFmtC4 = 8, kFmtC8 = 9, kFmtC14X2 = 10;
constexpr uint8_t kMaxSeen = 250;

struct Item {
    uint64_t hash = 0;       // XXH3-64 of `size` bytes: what the cache hashes before an upload (its contentHash)
    uint64_t qh = 0;         // XXH3-64 of the first min(kQuickBytes, level-0 bytes); 0 = not known (a v1 book's item)
    uint32_t size = 0;       // bytes of the texture with the levels the cache keys
    uint32_t tMs = 0;        // first used this long after the screen opened, last visits' earliest (the replay order); 0 = not known
    uint16_t w = 0, h = 0;
    uint8_t fmt = 0, levels = 1;
    uint8_t seen = 1;        // visits in which it was used (first-used in play or replayed): what an eviction keeps
};
// The same texture: everything but the times, the use count and the quick key (a v1 item has none).
inline bool SameId(const Item& a, const Item& b) { return a.hash == b.hash && a.size == b.size && a.w == b.w && a.h == b.h && a.fmt == b.fmt && a.levels == b.levels; }

inline bool Valid(const Item& it) {
    return it.w >= 1 && it.h >= 1 && it.w <= 1024 && it.h <= 1024 && it.size >= 8 && it.size <= kMaxSize && it.levels >= 1 && it.levels <= 12 && it.fmt <= 14 &&
           it.fmt != kFmtC4 && it.fmt != kFmtC8 && it.fmt != kFmtC14X2 && it.hash != 0;
}
inline bool InWindow(double nowMs, double openMs) { return openMs > 0.0 && nowMs >= openMs && nowMs - openMs <= kWindowMs; }
inline uint32_t MsSinceOpen(double nowMs, double openMs) {
    const double d = nowMs - openMs;
    return d <= 0.0 ? 0u : (d >= 4.0e9 ? 4000000000u : static_cast<uint32_t>(d));
}
// The length the quick key covers for a texture whose first level has `level0Bytes` bytes.
inline uint32_t QuickLen(uint32_t level0Bytes) { return level0Bytes < kQuickBytes ? level0Bytes : kQuickBytes; }
// The quick key from the raw hash: never 0 (0 means "not known" in a v1 item).
inline uint64_t QuickKey(uint64_t rawHash) { return rawHash | 1u; }

class Book {
public:
    // A texture first used in play (or confirmed by a replay when `replayed`). True when the stage's list changed (its file is then out of date).
    bool Note(uint32_t stage, const Item& in, bool replayed = false) {
        if (!stage || !Valid(in)) return false;
        auto f = m_.find(stage);
        if (f == m_.end()) {
            if (m_.size() >= kMaxStages || total_ >= kMaxTotal) return false;
            f = m_.emplace(stage, std::vector<Item>()).first;
        }
        std::vector<Item>& v = f->second;
        for (Item& o : v)
            if (SameId(o, in)) {
                bool ch = false;
                if (!o.qh && in.qh) { o.qh = in.qh; ch = true; }
                if (!replayed && in.tMs && (o.tMs == 0 || in.tMs < o.tMs)) { o.tMs = in.tMs; ch = true; }  // (the earliest use seen is the order)
                if (!parsing_ && o.seen < kMaxSeen) { ++o.seen; ch = true; }  // (a file read is not a visit: a v1 file's duplicates collapse)
                if (ch) Mark(stage);
                return ch;
            }
        if (v.size() >= kMaxItems) {  // full: a one-visit item gives way, a regular one does not
            size_t worst = v.size();
            for (size_t i = 0; i < v.size(); ++i)
                if (v[i].seen <= 1 && (worst == v.size() || v[i].tMs > v[worst].tMs)) worst = i;
            if (worst == v.size()) return false;
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(worst));
            --total_;
        }
        if (total_ >= kMaxTotal) return false;
        Item it = in;
        it.seen = 1;
        v.push_back(it);
        ++total_;
        Mark(stage);
        return true;
    }
    const std::vector<Item>* Items(uint32_t stage) const {
        const auto f = m_.find(stage);
        return f == m_.end() ? nullptr : &f->second;
    }
    size_t Stages() const { return m_.size(); }
    size_t Total() const { return total_; }
    bool Dirty() const { return dirty_; }
    void Clean() { dirty_ = false; }
    // The stages with changes since the last Clean for them: the card writer's unit is one stage's file.
    bool StageDirty(uint32_t s) const { const auto f = dirtyStages_.find(s); return f != dirtyStages_.end() && f->second; }
    void CleanStage(uint32_t s) { dirtyStages_[s] = false; }

    static const char* Magic() { return "texwarm 2"; }
    static const char* MagicV1() { return "texwarm 1"; }
    // "<stage hex> <size> <hash hex> <qh hex> <w> <h> <fmt> <levels> <tMs> <seen>" a line, in the order the replay will try them (earliest first use first).
    std::string SerializeStage(uint32_t stage) const {
        std::string s = Magic();
        s += "\n";
        const std::vector<Item>* v = Items(stage);
        if (!v) return s;
        std::vector<Item> o = *v;
        SortForReplay(o);
        char line[160];
        for (const Item& it : o) {
            std::snprintf(line, sizeof line, "%08x %u %016llx %016llx %u %u %u %u %u %u\n", static_cast<unsigned>(stage), it.size, static_cast<unsigned long long>(it.hash),
                          static_cast<unsigned long long>(it.qh), it.w, it.h, it.fmt, it.levels, it.tMs, it.seen);
            s += line;
        }
        return s;
    }
    std::string SerializeAll() const {
        std::string s = Magic();
        s += "\n";
        for (const auto& kv : m_) {
            const std::string one = SerializeStage(kv.first);
            s.append(one, std::strlen(Magic()) + 1, std::string::npos);
        }
        return s;
    }
    // Items taken. A wrong magic reads nothing (a file of another version is ignored, never half-understood); a bad line stops the parse (what was read stays); a last line cut short is dropped.
    // "texwarm 1" lines (stage addr size hash w h fmt levels): the address is dropped, the item has no quick key and no time.
    unsigned Parse(const char* text, size_t len) {
        const std::string t(text, len);
        size_t pos = t.find('\n');
        if (pos == std::string::npos) return 0;
        const int ver = t.compare(0, pos, Magic()) == 0 ? 2 : (t.compare(0, pos, MagicV1()) == 0 ? 1 : 0);
        if (!ver) return 0;
        unsigned taken = 0;
        ++pos;
        const bool wasDirty = dirty_;
        const std::map<uint32_t, bool> wasStages = dirtyStages_;
        parsing_ = true;
        while (pos < t.size()) {
            const size_t e = t.find('\n', pos);
            if (e == std::string::npos) break;
            const std::string line = t.substr(pos, e - pos);
            pos = e + 1;
            unsigned stage = 0, addr = 0, size = 0, w = 0, h = 0, fmt = 0, lv = 0, tms = 0, seen = 1;
            unsigned long long hash = 0, qh = 0;
            char tail = 0;
            if (ver == 2) {
                if (std::sscanf(line.c_str(), "%x %u %llx %llx %u %u %u %u %u %u%c", &stage, &size, &hash, &qh, &w, &h, &fmt, &lv, &tms, &seen, &tail) != 10) break;
            } else {
                if (std::sscanf(line.c_str(), "%x %x %u %llx %u %u %u %u%c", &stage, &addr, &size, &hash, &w, &h, &fmt, &lv, &tail) != 8) break;
            }
            if (w > 65535 || h > 65535 || fmt > 255 || lv > 255 || seen > 255) break;
            Item it;
            it.size = size; it.hash = hash; it.qh = qh; it.w = static_cast<uint16_t>(w); it.h = static_cast<uint16_t>(h);
            it.fmt = static_cast<uint8_t>(fmt); it.levels = static_cast<uint8_t>(lv); it.tMs = tms; it.seen = static_cast<uint8_t>(seen ? seen : 1);
            if (Note(stage, it)) ++taken;
        }
        parsing_ = false;
        dirty_ = wasDirty;                 // (what the file holds is clean; what was changed before the read stays changed)
        dirtyStages_ = wasStages;
        return taken;
    }
    // The replay's order: earliest first use first (unknown times last, in their book order), then the smaller texture (cheaper, and more of them fit a picture).
    static void SortForReplay(std::vector<Item>& v) {
        std::stable_sort(v.begin(), v.end(), [](const Item& a, const Item& b) {
            const uint32_t ta = a.tMs ? a.tMs : 0xFFFFFFFFu, tb = b.tMs ? b.tMs : 0xFFFFFFFFu;
            if (ta != tb) return ta < tb;
            return a.size < b.size;
        });
    }
private:
    void Mark(uint32_t s) { dirty_ = true; dirtyStages_[s] = true; }
    std::map<uint32_t, std::vector<Item>> m_;
    std::map<uint32_t, bool> dirtyStages_;
    size_t total_ = 0;
    bool dirty_ = false, parsing_ = false;
};

// One texture of a model the stage holds, as the game's walk reports it. `maxBytes` is what the model's own mip count says the data holds; `qh` is made by the front end.
struct Cand {
    uint32_t addr = 0;
    uint32_t maxBytes = 0;
    uint64_t qh = 0;
    uint32_t seq = 0;        // the walk's order (the stationed archives first, then the stage's: the start zone's models first)
    uint16_t w = 0, h = 0;
    uint8_t fmt = 0;
    uint8_t levels = 1;      // the levels the game's bind will key (from the model's filter, max lod and mip count)
    bool uploaded = false;   // the replay made it (once; a duplicate address is another candidate)
    uint64_t hashAt = 0;     // memo: the full hash over hashLen bytes
    uint32_t hashLen = 0;
};

enum Find : uint8_t { kFound = 0, kNoShape, kNoQuick, kBytesDiffer };

class CandIndex {
public:
    void Clear() { v_.clear(); by_.clear(); seen_.clear(); addrs_.clear(); walkDone_ = false; key_ = 0; }
    void Begin(uint32_t key) { Clear(); key_ = key; }
    uint32_t Key() const { return key_; }
    // False when the address was already there (a model shared by two holders).
    bool Add(const Cand& c) {
        if (v_.size() >= kMaxCands) return false;
        const uint64_t ak = (static_cast<uint64_t>(c.addr) << 32) | (static_cast<uint64_t>(c.w) << 16) | c.h;
        if (!seen_.emplace(ak ^ (static_cast<uint64_t>(c.fmt) << 59), 1).second) return false;
        v_.push_back(c);
        addrs_.emplace(c.addr, 1);
        by_.emplace(ShapeKey(c.w, c.h, c.fmt), static_cast<uint32_t>(v_.size() - 1));
        return true;
    }
    void SetWalkDone(bool d) { walkDone_ = d; }
    bool WalkDone() const { return walkDone_; }
    size_t Size() const { return v_.size(); }
    Cand& At(size_t i) { return v_[i]; }
    const Cand& At(size_t i) const { return v_[i]; }
    // The candidates an item can be: same shape and (when the item has a quick key) same quick key. At most `cap` indices, walk order. `scanned` counts the ones that had no
    // quick key to compare (an item without one).
    unsigned Shape(const Item& it, uint32_t* out, unsigned cap) const {
        unsigned n = 0;
        const auto r = by_.equal_range(ShapeKey(it.w, it.h, it.fmt));
        for (auto i = r.first; i != r.second && n < cap; ++i) {
            const Cand& c = v_[i->second];
            if (it.qh && c.qh != it.qh) continue;
            if (c.maxBytes < it.size) continue;  // (the model holds fewer bytes than the item's levels need: not this texture)
            out[n++] = i->second;
        }
        std::sort(out, out + n, [&](uint32_t a, uint32_t b) { return v_[a].seq < v_[b].seq; });
        return n;
    }
    unsigned ShapeCount(const Item& it) const { return static_cast<unsigned>(by_.count(ShapeKey(it.w, it.h, it.fmt))); }
    // Does any candidate lie at this address (a texture first used in play: was it enumerable?)
    bool HasAddr(uint32_t addr) const { return addrs_.find(addr) != addrs_.end(); }
    static constexpr size_t kMaxCands = 16384;
private:
    static uint64_t ShapeKey(unsigned w, unsigned h, unsigned fmt) { return (static_cast<uint64_t>(w) << 24) | (static_cast<uint64_t>(h) << 8) | fmt; }
    std::vector<Cand> v_;
    std::unordered_multimap<uint64_t, uint32_t> by_;
    std::unordered_map<uint64_t, int> seen_;
    std::unordered_map<uint32_t, int> addrs_;
    bool walkDone_ = false;
    uint32_t key_ = 0;
};

// The per-picture limit on warm-up work. All limits apply; one item is always allowed per picture (a picture with nothing done yet takes the next, however big).
struct Budget {
    double maxMs = 3.0;
    uint32_t maxKiB = 512;
    unsigned maxItems = 0;       // 0 = no limit on the count
    double usedMs = 0;
    uint32_t usedKiB = 0;
    unsigned items = 0;
    void NewPicture() { usedMs = 0; usedKiB = 0; items = 0; }
    bool Allow(uint32_t nextKiB) const {
        if (items == 0) return true;
        if (maxItems && items >= maxItems) return false;
        return usedMs < maxMs && usedKiB + nextKiB <= maxKiB;
    }
    void Spend(double ms, uint32_t kib) { usedMs += ms; usedKiB += kib; ++items; }
};

enum Outcome : uint8_t { kHit = 0, kAlready, kDropped, kWait, kMissNoCand, kMissBytes, kMissMemory };

// The replay of one stage's items in the order given (the caller sorted them, most likely first).
class Sched {
public:
    void Begin(size_t n) {
        st_.assign(n, State{});
        left_ = n;
        gaveUp_ = 0;
        waits_ = 0;
    }
    bool Active() const { return left_ > 0; }
    size_t Left() const { return left_; }
    // Items that can be tried at `picture` (not done, not waiting out a retry gap): what a closed-screen hold may wait for.
    size_t Due(uint64_t picture) const {
        size_t n = 0;
        for (const State& s : st_) n += (!s.done && s.nextTry <= picture) ? 1u : 0u;
        return n;
    }
    bool Next(uint64_t picture, size_t& idx) const {
        for (size_t i = 0; i < st_.size(); ++i)
            if (!st_[i].done && st_[i].nextTry <= picture) { idx = i; return true; }
        return false;
    }
    // `walkDone` says whether a "no candidate" can still change. Returns the outcome as recorded (a wait that ran out becomes a no-candidate miss).
    Outcome Result(size_t idx, Outcome o, uint64_t picture) {
        if (idx >= st_.size() || st_[idx].done) return o;
        State& s = st_[idx];
        if (o == kHit || o == kAlready || o == kDropped) { s.done = true; --left_; return o; }
        if (o == kWait) {
            ++waits_;
            if (++s.waits >= kMaxWaitPics) o = kMissNoCand;
            else { s.nextTry = picture + 1; return o; }
        }
        s.misses += (o == kMissNoCand) ? kNoCandCost : 1u;
        if (s.misses >= kMaxMisses) { s.done = true; --left_; ++gaveUp_; return o; }
        s.nextTry = picture + kRetryGap;
        return o;
    }
    unsigned GaveUp() const { return gaveUp_; }
    uint64_t Waits() const { return waits_; }
    // Mark everything left as not wanted any more (the stage's window is over).
    void Abandon() { for (State& s : st_) if (!s.done) { s.done = true; } left_ = 0; }
private:
    struct State { bool done = false; unsigned misses = 0, waits = 0; uint64_t nextTry = 0; };
    std::vector<State> st_;
    size_t left_ = 0;
    unsigned gaveUp_ = 0;
    uint64_t waits_ = 0;
};

struct Census {
    uint32_t stage = 0;
    unsigned bookN = 0;                                  // items in the stage's book at the start of the visit
    unsigned foundQuick = 0, foundScan = 0;              // found by identity (quick key + full hash), found by the slower scan (an item without a quick key)
    unsigned already = 0;                                // the texture was there (the stage before, or the same address twice)
    unsigned missNoCand = 0, missBytes = 0, missMem = 0, dropped = 0, gaveUp = 0;
    unsigned dupUploaded = 0;                            // extra addresses of the same bytes uploaded
    unsigned upClosed = 0, upOpen = 0;                   // uploaded by the replay while the screen was closed / in the first open pictures
    uint64_t upKiB = 0;
    double upMs = 0;                                     // front end ms (decode + convert + hand-over); the back end's upload is its own
    unsigned frames = 0;                                 // closed-screen pictures the replay worked in
    unsigned leftAtOpen = 0;                             // items still to do when the screen opened
    bool openSeen = false;
    unsigned enumUp = 0;                                 // first visit: candidates with no book item uploaded (GXV_TEXWARM_ENUM)
    uint64_t enumKiB = 0;
    unsigned enumBound = 0;                              // of them, bound by the game in the window (known at +20 s)
    unsigned cands = 0;                                  // candidates the walk reported
    unsigned firstP = 0;                                 // first-used in play (the cache decoded them for a first upload)
    uint64_t firstKiB = 0;
    double firstMs = 0;
    unsigned firstEnumerable = 0, firstNotEnumerable = 0, firstCI = 0;   // P by whether a loaded model's candidate lay at the address
    void Reset(uint32_t s) { *this = Census{}; stage = s; }
};

inline size_t Format(char* out, size_t cap, const Census& c, const char* when, int mode, unsigned left, bool walkDone) {
    const int n = std::snprintf(out, cap,
        "texwarm: stage %08x %s (GXV_TEXWARM=%d): book %u, found %u by identity, %u by hash scan, missed %u (no model holds it %u, bytes differ %u, memory %u, dropped %u; given up %u), "
        "already there %u; uploaded behind the cover %u in %.0f ms over %u frames (%llu KiB; first-open pictures %u; same bytes at a 2nd address %u), left at open %u, left now %u; "
        "walk %s %u candidates; first visit: %u unbooked candidates uploaded (%llu KiB, %u bound in the window); "
        "first-used in play %u (%llu KiB, %.0f ms): %u had a candidate, %u had none (made by code, a particle, a layout or a later archive), %u palette",
        static_cast<unsigned>(c.stage), when, mode, c.bookN, c.foundQuick, c.foundScan, c.missNoCand + c.missBytes + c.missMem + c.dropped, c.missNoCand, c.missBytes, c.missMem, c.dropped,
        c.gaveUp, c.already, c.upClosed, c.upMs, c.frames, static_cast<unsigned long long>(c.upKiB), c.upOpen, c.dupUploaded, c.leftAtOpen, left, walkDone ? "done" : "NOT done", c.cands,
        c.enumUp, static_cast<unsigned long long>(c.enumKiB), c.enumBound, c.firstP, static_cast<unsigned long long>(c.firstKiB), c.firstMs, c.firstEnumerable, c.firstNotEnumerable, c.firstCI);
    return n < 0 ? 0 : (static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1);
}

}  // namespace vgx::texid
