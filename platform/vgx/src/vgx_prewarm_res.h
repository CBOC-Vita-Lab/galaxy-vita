// handover26 (GXV_PREWARM_RES): the pure parts of "make the render targets a stage's first seconds need behind the closed screen". Header only, standard library
// only, so the back end (vgx_backend_gl.cpp) and the host test (tests/prewarm_res_model.cpp) share the code.
//
// WHY (console, 22-stage tours c79 K3 / RM): about 8 s into every stage, as the opening camera hands over to the scenario starter, the back end makes a 240x191
// render target (22 of 22 stages; 36-128 ms in the back end, "other"), and some stages a 960x544 copy target about 10 s in (8 of 22, 52-82 ms): sceGxm render
// targets and their depth buffers are made at the first scene (vitaGL gxm.c), 10-100 ms each, in a picture the player sees. The wish list of GXV_POOLPRIME needs a
// frame with the loading flag or the hold, which a normal stage change does not have (the wipe's closed frames are the only quiet ones), and it makes a pooled
// object with its own colour texture, not the copy texture the copy later asks for.
//
// WHAT THIS HOLDS
//   Item     one target: size, the picture size it was made for, kind (0 = a copy's texture on a pooled object, 1 = a pass's own colour (+ depth) object).
//   Book     per stage up to kMaxItems items in the order first made in play within kWindowMs of the screen opening; text file lines, read and written whole.
//   Plan     what to make for a stage: mode 2 = the stage's recorded items first, then the generic ones; mode 1 = the generic ones only; each size once (kind 0
//            and 1 are different objects), filtered to the current picture size, capped by count and by MiB.
//   Generic  the sizes every stage of the c79 tours made at the same moment, scaled from 960x544.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace vgx::prewarmres {

constexpr unsigned kMaxItems = 24;      // items a stage keeps (texwarm30: 8 -> 12: the step-up picture's targets are kept too; a plan still makes at most GXV_PREWARM_RES_MAX + kMaxAlt)
constexpr unsigned kMaxAlt = 3;         // texwarm30: items of the picture size the stage steps UP to, on top of the plan's own cap
constexpr unsigned kMaxStages = 96;     // stages the book keeps (a session visits 48 stage keys at most)
constexpr double kWindowMs = 15000.0;   // targets made in play within this long after the screen opened are recorded
constexpr double kCensusMs = 20000.0;   // the second census line, and the worst stall, cover this long
constexpr uint8_t kCopyTex = 0, kPassObj = 1;

struct Item {
    uint16_t w = 0, h = 0;          // pixels of the target
    uint16_t picW = 0, picH = 0;    // the picture (EFB) size it was made for: a target of another size is not this picture's
    uint8_t kind = kCopyTex;
    uint8_t depth = 0;              // kPassObj only: with a depth buffer
    bool operator==(const Item& o) const { return w == o.w && h == o.h && picW == o.picW && picH == o.picH && kind == o.kind; }
};

// The bytes a target takes: colour RGBA8 (rows to 8), depth 32-bit when wished.
inline uint64_t ItemBytes(const Item& it) {
    const uint64_t c = static_cast<uint64_t>((it.w + 7u) & ~7u) * it.h * 4u;
    const uint64_t d = it.depth ? static_cast<uint64_t>((it.w + 7u) & ~7u) * it.h * 4u : 0u;
    return c + d;
}

// The generic items for a picture: what every stage of the c79 tours made when Mario and the starter first drew (240x191 at 960x544 in 22 of 22 stages; the
// full-size copy target in 8 of 22), scaled to the picture size. Other sizes than 960x544 are scaled estimates (848x480 -> 212x169, 848x480): a wrong size is
// a few hundred KiB made for nothing and freed unused.
// (stall27) The runtime's own rounding (vgx_backend_gl.cpp Rnd: floor(v + 0.5)) and its copy size: a copy of dst x dst EFB units (the game's 160x160 copy of Mario and the starter) is
// Rnd(160 * picW / efbW) x Rnd(160 * picH / efbH): 240x191 at 960x544, 212x168 at 848x480 (the c86 plan said 212x169: an off-by-one premade is never taken).
inline int RndPx(double v) { return static_cast<int>(std::floor(v + 0.5)); }
inline unsigned Generic(unsigned picW, unsigned picH, Item* out, unsigned cap, unsigned efbW = 640, unsigned efbH = 456) {
    if (!picW || !picH || picW > 4096 || picH > 4096 || cap < 2 || !efbW || !efbH) return 0;
    const double sx = static_cast<double>(picW) / efbW, sy = static_cast<double>(picH) / efbH;
    unsigned n = 0;
    Item a;
    a.w = static_cast<uint16_t>(std::max(1, RndPx(160.0 * sx)));
    a.h = static_cast<uint16_t>(std::max(1, RndPx(160.0 * sy)));
    a.picW = static_cast<uint16_t>(picW);
    a.picH = static_cast<uint16_t>(picH);
    a.kind = kCopyTex;
    out[n++] = a;
    Item b = a;
    b.w = static_cast<uint16_t>(picW);
    b.h = static_cast<uint16_t>(picH);
    out[n++] = b;
    return n;
}

// (stall27) The seeded list shipped with the game: the targets the c86 tour (22 stages, first visits, empty books) made in play after the generic ones, as fractions of the picture
// (the water / bloom chain: 1/2, 1/4, 1/8 of the picture: 480x272, 240x136, 120x68 at 960x544; 424x240, 212x120 at 848x480), per stage name (hash of the name = the stage key).
// kind 0 a copy's texture, 1 a pass object with depth (BeginPass), 2 a pass object without depth (BeginFx). The learned list (mode 2) adds to it, the book's items win a size.
constexpr uint32_t Fnv(const char* s) { uint32_t h = 2166136261u; for (; *s; ++s) h = (h ^ static_cast<unsigned char>(*s)) * 16777619u; return h; }
struct SeedRow { uint32_t stage; uint8_t kind, den; };  // the target is the picture / den
constexpr SeedRow kSeed[] = {
    {Fnv("HeavenlyBeachGalaxy"), 0, 4}, {Fnv("HeavenlyBeachGalaxy"), 0, 2}, {Fnv("HeavenlyBeachGalaxy"), 1, 8},
    {Fnv("OceanRingGalaxy"), 0, 2}, {Fnv("OceanRingGalaxy"), 0, 4}, {Fnv("OceanRingGalaxy"), 1, 8},
    {Fnv("KoopaBattleVs1Galaxy"), 0, 2}, {Fnv("KoopaBattleVs1Galaxy"), 1, 8},
    {Fnv("KoopaBattleVs2Galaxy"), 0, 2}, {Fnv("KoopaBattleVs2Galaxy"), 0, 4},
    {Fnv("IceVolcanoGalaxy"), 2, 2},
};
inline unsigned Seeded(uint32_t stage, unsigned picW, unsigned picH, Item* out, unsigned cap) {
    unsigned n = 0;
    if (!picW || !picH || picW > 4096 || picH > 4096) return 0;
    for (const SeedRow& r : kSeed) {
        if (r.stage != stage || n >= cap) continue;
        Item it;
        it.w = static_cast<uint16_t>(std::max(1, RndPx(static_cast<double>(picW) / r.den)));
        it.h = static_cast<uint16_t>(std::max(1, RndPx(static_cast<double>(picH) / r.den)));
        it.picW = static_cast<uint16_t>(picW);
        it.picH = static_cast<uint16_t>(picH);
        it.kind = r.kind == 0 ? kCopyTex : kPassObj;
        it.depth = r.kind == 1 ? 1 : 0;
        out[n++] = it;
    }
    return n;
}

inline bool ValidItem(const Item& it) {
    return it.w >= 1 && it.h >= 1 && it.w <= 2048 && it.h <= 1024 && it.picW >= 1 && it.picH >= 1 && it.picW <= 4096 && it.picH <= 4096 && it.kind <= kPassObj && it.depth <= 1;
}

class Book {
public:
    // A target made in play. True when it is new to the stage's list (the file is then out of date).
    bool Note(uint32_t stage, const Item& it) {
        if (!stage || !ValidItem(it)) return false;
        auto f = m_.find(stage);
        if (f == m_.end()) {
            if (m_.size() >= kMaxStages) return false;
            f = m_.emplace(stage, std::vector<Item>()).first;
        }
        std::vector<Item>& v = f->second;
        for (Item& o : v)
            if (o == it) {
                if (it.depth && !o.depth) { o.depth = 1; dirty_ = true; return true; }
                return false;
            }
        if (v.size() >= kMaxItems) return false;
        v.push_back(it);
        dirty_ = true;
        return true;
    }
    const std::vector<Item>* Items(uint32_t stage) const {
        const auto f = m_.find(stage);
        return f == m_.end() ? nullptr : &f->second;
    }
    size_t Stages() const { return m_.size(); }
    size_t Total() const { size_t n = 0; for (const auto& kv : m_) n += kv.second.size(); return n; }
    bool Dirty() const { return dirty_; }
    void Clean() { dirty_ = false; }

    // One line an item: "<stage hex> <w> <h> <picW> <picH> <kind> <depth>"; the first line is the magic.
    static const char* Magic() { return "prewarm-res 1"; }
    std::string Serialize() const {
        std::string s = Magic();
        s += "\n";
        char line[96];
        for (const auto& kv : m_)
            for (const Item& it : kv.second) {
                std::snprintf(line, sizeof line, "%08x %u %u %u %u %u %u\n", static_cast<unsigned>(kv.first), it.w, it.h, it.picW, it.picH, it.kind, it.depth);
                s += line;
            }
        return s;
    }
    // Returns the items taken; a wrong magic or a bad line stops the parse (what was read stays), cut-short text is fine.
    unsigned Parse(const char* text, size_t len) {
        const std::string t(text, len);
        size_t pos = t.find('\n');
        if (pos == std::string::npos || t.compare(0, pos, Magic()) != 0) return 0;
        unsigned taken = 0;
        ++pos;
        while (pos < t.size()) {
            size_t e = t.find('\n', pos);
            if (e == std::string::npos) break; // (a line without its end: a write cut short)
            unsigned stage = 0, w = 0, h = 0, pw = 0, ph = 0, kind = 0, depth = 0;
            char tail = 0;
            const std::string line = t.substr(pos, e - pos);
            pos = e + 1;
            if (std::sscanf(line.c_str(), "%x %u %u %u %u %u %u%c", &stage, &w, &h, &pw, &ph, &kind, &depth, &tail) != 7) break;
            Item it;
            it.w = static_cast<uint16_t>(w); it.h = static_cast<uint16_t>(h); it.picW = static_cast<uint16_t>(pw); it.picH = static_cast<uint16_t>(ph);
            it.kind = static_cast<uint8_t>(kind); it.depth = static_cast<uint8_t>(depth);
            if (w > 65535 || h > 65535 || pw > 65535 || ph > 65535 || kind > 255 || depth > 255) break;
            if (Note(stage, it)) ++taken;
        }
        dirty_ = false; // (what the file holds)
        return taken;
    }

private:
    std::map<uint32_t, std::vector<Item>> m_;
    bool dirty_ = false;
};

struct Plan {
    Item item[kMaxItems + 4];
    unsigned n = 0, recorded = 0, generic = 0, seeded = 0, droppedBytes = 0, alt = 0, targeted = 0;   // alt: made for the picture size the stage steps up to (texwarm30)
};

// mode 1: generic only; 2: the stage's recorded items (this picture size only) first, then the generic ones. A size twice is made once; at most maxItems and maxBytes.
// texwarm30: altW x altH (0: none) is the picture size the stage steps UP to later (the controller's top rung, when the stage opens lower: the console tours step 848x480 -> 960x544 about 7.7 s
// after every stage opens, "4 -> 6 ... pooled objects made 1, premade textures taken 0": the plan was only for the size at the opening). Its recorded items and its generic ones (the copy
// target and the full-size texture) are added last, at most kMaxAlt of them on top of maxItems, under the same byte cap.
inline void BuildPlan(const Book* book, uint32_t stage, int mode, unsigned picW, unsigned picH, unsigned maxItems, uint64_t maxBytes, Plan& p, unsigned efbW = 640, unsigned efbH = 456, bool seed = false,
                      unsigned altW = 0, unsigned altH = 0, const Item* targeted = nullptr) {
    p = Plan{};
    if (mode < 1) return;
    if (maxItems > kMaxItems) maxItems = kMaxItems;
    uint64_t bytes = 0;
    unsigned limit = maxItems;
    auto add = [&](const Item& it, int rec) { // 1 recorded, 0 generic, 2 seeded, 3 for the step-up size
        if (p.n >= limit) return;
        for (unsigned i = 0; i < p.n; ++i)
            if (p.item[i] == it) { if (it.depth) p.item[i].depth = 1; return; }
        if (bytes + ItemBytes(it) > maxBytes) { ++p.droppedBytes; return; }
        bytes += ItemBytes(it);
        p.item[p.n++] = it;
        (rec == 1 ? p.recorded : (rec == 2 ? p.seeded : (rec == 3 ? p.alt : (rec == 4 ? p.targeted : p.generic))))++;
    };
    if (targeted && ValidItem(*targeted) && targeted->picW == picW && targeted->picH == picH) add(*targeted, 4);
    if (mode >= 2 && book) {
        const std::vector<Item>* v = book->Items(stage);
        if (v)
            for (const Item& it : *v)
                if (it.picW == picW && it.picH == picH) add(it, 1);
    }
    Item g[4];
    const unsigned gn = Generic(picW, picH, g, 4, efbW, efbH);
    for (unsigned i = 0; i < gn; ++i) add(g[i], 0);
    if (seed) { // (stall27: GXV_PREWARM_RES_SEED) after the generic ones: the stage's seeded targets (the water / bloom chain)
        Item sd[8];
        const unsigned sn = Seeded(stage, picW, picH, sd, 8);
        for (unsigned i = 0; i < sn; ++i) add(sd[i], 2);
    }
    if (altW && altH && altW <= 4096 && altH <= 4096 && (altW != picW || altH != picH)) {   // (texwarm30)
        limit = std::min(maxItems + kMaxAlt, kMaxItems + kMaxAlt);
        const unsigned before = p.n;
        if (mode >= 2 && book) {
            const std::vector<Item>* v = book->Items(stage);
            if (v)
                for (const Item& it : *v)
                    if (it.picW == altW && it.picH == altH && p.n - before < kMaxAlt) add(it, 3);
        }
        Item g2[4];
        const unsigned gn2 = Generic(altW, altH, g2, 4, efbW, efbH);
        for (unsigned i = 0; i < gn2 && p.n - before < kMaxAlt; ++i) add(g2[i], 3);
    }
}

// Is a target made in play at nowMs (the back end's clock) after the screen opened at openMs worth recording?
inline bool Recordable(double nowMs, double openMs) { return openMs > 0.0 && nowMs >= openMs && nowMs - openMs <= kWindowMs; }

// The worst stall of the census window: keep the largest interval, its time after the screen opened and its cause (a name the caller owns).
struct Worst {
    double ms = 0, atMs = 0;
    const char* cause = "";
    void Note(double intervalMs, double sinceOpenMs, const char* c) {
        if (sinceOpenMs < 0.0 || sinceOpenMs > kCensusMs) return;
        if (intervalMs > ms) { ms = intervalMs; atMs = sinceOpenMs; cause = c ? c : ""; }
    }
};

}  // namespace vgx::prewarmres
