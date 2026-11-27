// streamc31 (17 Nov): GXV_DYN_CENSUS bit 2 (=2, or 3 with the old "dyn-census:" line): the stream-decode census. Census only: it changes
// no state, no draw, no command buffer (tools/test_vgx_deccen.sh checks that the fe-work fixture's command buffers are the same with it).
//
// The question (perf-inventory D3, Codex c181 report #5, Strikers ideas-3 item 3c): would an exact compare-and-keep cache of the stream
// draws' vertex decode pay? For each draw that DrawStreamPlan decodes (vgx_state.cpp: the top-level stream, the lists ProcessStream parses
// (tiny and bypassed), the particle commands), this records
//   - its decode time and the time of its EmitDraw (the part a decode cache can NOT remove: resolve, culls, records), by class;
//   - whether its inputs are byte-identical to the "same" draw of the picture before. The same draw: the same plan signature (the vertex
//     format as the plan reads it, the primitive, the vertex count, the decoded layout) and the same ordinal among the draws of that
//     signature in the picture (key A: the key an actual cache would look up). Inputs, per attribute: the attribute's bytes in the stream
//     (direct data, or its indices) and, for an indexed attribute, the array bytes the indices reach ([lowest, highest] element, the array's
//     stride and byte order). Compared with memcmp against a copy kept from the picture before (byte-exact, not a hash);
//   - whether the whole input (key B: a 64-bit hash of the signature, the stream bytes and the array ranges) was seen anywhere in the
//     picture before, or earlier in the same picture (a content-addressed cache's ceiling);
//   - the cost side of a compare-and-keep cache, timed: the memcmp of the inputs against the kept copy (every matched draw), the copy kept
//     for the next picture (every draw), the copy of the decoded vertices out of the cache (every identical draw).
// So the census prints, per picture: what an exact cache could save at most (decode time of the identical draws) and what it would cost.
// Classes: origin (top-level stream / inside a called list / a particle command) x shape (4-vertex all-direct quad, all-direct <=64
// vertices, all-direct more, indexed <=64, indexed more). In the game: JPA particles are one-quad lists with indexed arrays (list,
// indexed<=64); J2D / HUD and immediate drawing are top-level direct draws; CPU-deformed meshes are indexed draws whose position array
// changes. "In range" (Strikers' FASTIDX question): every index of the draw inside its array.
// Lines ("dyn-census decode:", with GXV_DYN_CENSUS_MS, default every 10 s; averages per picture; times with the clock's own cost taken off,
// measured at the first draw): the totals and the cache's economics, the per-attribute identity, then one line per origin with its classes.
// Memory: two arenas of at most GXV_DYN_CENSUS_KB (default 1024) KiB each for the kept inputs; a draw whose inputs do not fit (or are
// more than an eighth of the cap) is counted "over the cap" and not compared in the next picture.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>

namespace vgx {
namespace deccen {

enum Origin : uint8_t { kTop, kList, kFx, kOrigins };
enum Shape : uint8_t { kQuadDirect, kDirectSmall, kDirectBig, kIndexedSmall, kIndexedBig, kShapes };
enum Kind : uint8_t { kMtxIdx, kPos, kNrm, kClr, kTex, kKinds };
constexpr int kClasses = kOrigins * kShapes;
inline const char* OriginName(int o) { return o == kTop ? "top-level" : o == kList ? "in lists" : "particle commands"; }
inline const char* ShapeName(int s) {
    static const char* const n[kShapes] = {"quad-direct", "direct<=64", "direct>64", "indexed<=64", "indexed>64"};
    return n[s];
}
inline const char* KindName(int k) {
    static const char* const n[kKinds] = {"mtxidx", "pos", "nrm", "clr", "tex"};
    return n[k];
}
// (GX attribute numbers: 0 PNMTXIDX, 1-8 TEXnMTXIDX, 9 POS, 10 NRM, 11-12 CLR0/1, 13-20 TEX0-7)
inline int KindOf(uint32_t attr) { return attr <= 8 ? kMtxIdx : attr == 9 ? kPos : attr == 10 ? kNrm : attr <= 12 ? kClr : kTex; }
inline int ShapeOf(bool allDirect, uint32_t count) {
    if (allDirect) return count == 4 ? kQuadDirect : count <= 64 ? kDirectSmall : kDirectBig;
    return count <= 64 ? kIndexedSmall : kIndexedBig;
}

// One attribute of a draw, as the decode reads it.
struct AttrIn {
    uint8_t attr = 0;
    bool indexed = false;
    uint16_t srcOff = 0, srcSize = 0;  // its bytes in each vertex of the stream (direct data or the index)
    const uint8_t* arr = nullptr;      // indexed: the array bytes the indices reach (nullptr / 0: none, out of range)
    uint32_t arrLen = 0, stride = 0;
    bool le = false;
};
struct DrawIn {
    uint64_t sig = 0;                  // the plan signature (vgx_state.cpp DecCenSig): equal signatures decode equal inputs alike
    uint8_t origin = kTop;
    bool allDirect = false, inRange = true;
    uint32_t count = 0, streamSize = 0;
    const uint8_t* stream = nullptr;   // count * streamSize bytes
    int nAttr = 0;
    AttrIn a[21];
    const uint8_t* out = nullptr;      // the decoded vertices (nullptr: not made yet, GXV_FE_PARALLEL)
    uint32_t outBytes = 0;
    uint64_t decodeNs = 0, emitNs = 0; // raw clock differences, `decodeScopes` / `emitScopes` intervals
    uint32_t decodeScopes = 0, emitScopes = 0;
};

class Census {
public:
    using Clock = uint64_t (*)();
    explicit Census(Clock now, size_t capBytes = 1024u << 10) : now_(now), cap_(capBytes) {}

    void Note(const DrawIn& d) {
        if (!calibrated_) Calibrate();
        Cls& c = w_.c[d.origin * kShapes + ShapeOf(d.allDirect, d.count)];
        const uint64_t dec = Net(d.decodeNs, d.decodeScopes), emit = Net(d.emitNs, d.emitScopes);
        ++c.draws;
        c.verts += d.count;
        c.decNs += dec;
        c.emitNs += emit;
        bool anyIndexed = false;
        for (int i = 0; i < d.nAttr; ++i) anyIndexed |= d.a[i].indexed;
        if (anyIndexed) { ++c.indexed; if (d.inRange) ++c.inRange; }
        const uint32_t slen = d.count * d.streamSize;

        // ---- key B: the whole input, anywhere in the picture before / earlier in this one
        uint64_t h = XXH3_64bits_withSeed(d.stream, slen, d.sig);
        for (int i = 0; i < d.nAttr; ++i)
            if (d.a[i].indexed) h = XXH3_64bits_withSeed(d.a[i].arr, d.a[i].arr ? d.a[i].arrLen : 0, h ^ (static_cast<uint64_t>(d.a[i].stride) << 9) ^ (d.a[i].le ? 256u : 0u) ^ d.a[i].attr);
        if (prevH_.count(h)) { ++c.seenPrev; c.seenPrevDecNs += dec; }
        if (!curH_.insert(h).second) { ++c.repeat; c.repeatDecNs += dec; }

        // ---- key A: the signature and its ordinal in this picture
        const uint32_t ord = sigCount_[d.sig]++;
        const uint64_t key = d.sig ^ (0x9E3779B97F4A7C15ull * (ord + 1ull));
        const auto it = prev_.find(key);
        if (it != prev_.end() && it->second.sig == d.sig) {
            const Rec& r = it->second;
            ++c.matched;
            if (!r.kept) {
                ++w_.noKept;
            } else {
                // (the cache's compare, timed: the stream bytes, then each indexed attribute's array range)
                const uint8_t* const pb = prevBytes_.data();
                const uint64_t t0 = now_();
                bool streamSame = r.streamLen == slen && std::memcmp(pb + r.streamOff, d.stream, slen) == 0;
                bool arraysSame = true;
                for (int i = 0; i < d.nAttr && i < r.nAttr; ++i) {
                    const AttrIn& a = d.a[i];
                    const ARec& ra = prevAttr_[r.attr0 + i];
                    if (!a.indexed) continue;
                    const bool same = ra.arrLen == a.arrLen && ra.stride == a.stride && ra.le == a.le && (a.arrLen == 0 || std::memcmp(pb + ra.arrOff, a.arr, a.arrLen) == 0);
                    arraysSame &= same;
                }
                const uint64_t t1 = now_();
                w_.cmpNs += Net(t1 - t0, 1);
                if (r.nAttr != d.nAttr) arraysSame = false;
                const bool whole = streamSame && arraysSame;
                if (streamSame) ++c.stream;
                // ---- per attribute (not timed: the census's own work)
                bool nonPosSame = true, posSame = true, hasPos = false;
                for (int i = 0; i < d.nAttr && i < r.nAttr; ++i) {
                    const AttrIn& a = d.a[i];
                    const ARec& ra = prevAttr_[r.attr0 + i];
                    const int k = KindOf(a.attr);
                    bool same = SliceSame(pb + r.streamOff, d.stream, d.count, d.streamSize, a.srcOff, a.srcSize);
                    if (a.indexed) {
                        const bool arrSame = ra.arrLen == a.arrLen && ra.stride == a.stride && ra.le == a.le &&
                                             (a.arrLen == 0 || std::memcmp(pb + ra.arrOff, a.arr, a.arrLen) == 0);
                        ++w_.arrCmp[k];
                        if (arrSame) ++w_.arrSame[k];
                        if (arrSame && ra.base != a.arr) ++w_.arrMoved[k];
                        same = same && arrSame;
                    }
                    ++w_.kindCmp[k];
                    if (same) ++w_.kindSame[k];
                    if (k == kPos) { hasPos = true; posSame = same; }
                    else nonPosSame &= same;
                }
                if (whole) {
                    ++c.whole;
                    c.wholeDecNs += dec;
                    if (d.out && d.outBytes) {
                        // (a hit's copy of the kept vertices into the draw arena, timed)
                        if (scratch_.size() < d.outBytes) scratch_.resize(d.outBytes);
                        const uint64_t t2 = now_();
                        std::memcpy(scratch_.data(), d.out, d.outBytes);
                        const uint64_t t3 = now_();
                        w_.copyNs += Net(t3 - t2, 1);
                        w_.copyBytes += d.outBytes;
                    }
                } else if (hasPos && !posSame && nonPosSame && r.nAttr == d.nAttr) {
                    ++c.nonPos;
                    // (estimate: the decode makes one pass per attribute; the passes other than the position's)
                    c.nonPosDecEst += d.nAttr > 0 ? dec * static_cast<uint64_t>(d.nAttr - 1) / static_cast<uint64_t>(d.nAttr) : 0;
                }
            }
        }
        Keep(key, d, slen);
    }

    // The picture ends: this picture's records become the ones the next compares against.
    void EndPicture() {
        ++w_.pics;
        std::swap(prev_, cur_);
        cur_.clear();
        std::swap(prevBytes_, curBytes_);
        curBytes_.clear();
        std::swap(prevAttr_, curAttr_);
        curAttr_.clear();
        std::swap(prevH_, curH_);
        curH_.clear();
        sigCount_.clear();
    }

    // The lines for the window since the last call (then the window starts again). Returns the number of lines written.
    int Lines(char (*out)[1000], int maxLines) {
        int n = 0;
        const double pc = w_.pics ? static_cast<double>(w_.pics) : 1.0;
        Cls t{};
        for (const Cls& c : w_.c) t.Add(c);
        auto us = [&](uint64_t ns) { return static_cast<double>(ns) / 1000.0 / pc; };
        auto pf = [&](uint64_t v) { return static_cast<double>(v) / pc; };
        const uint64_t costNs = w_.cmpNs + w_.keepNs + w_.copyNs;
        if (n < maxLines)
            std::snprintf(out[n++], 1000,
                          "dyn-census decode: %llu pictures; a picture: stream draws %.1f (%.0f vertices), decode %.1f us, their EmitDraw %.1f us (clock %.3f us a read, "
                          "taken off); matched to the picture before %.1f; inputs identical: whole draws %.1f (decode %.1f us), only the position changed %.1f "
                          "(other attributes' decode, est. %.1f us); whole input seen in the picture before %.1f (decode %.1f us), repeated in the picture %.1f "
                          "(%.1f us); exact cache's costs: compare %.1f us, keep %.1f us (%.1f of %.1f KiB), copy out %.1f us -> ceiling saving %.1f us; over the cap %.1f, "
                          "not kept %.1f",
                          static_cast<unsigned long long>(w_.pics), pf(t.draws), pf(t.verts), us(t.decNs), us(t.emitNs), clockNs_ / 1000.0, pf(t.matched), pf(t.whole),
                          us(t.wholeDecNs), pf(t.nonPos), us(t.nonPosDecEst), pf(t.seenPrev), us(t.seenPrevDecNs), pf(t.repeat), us(t.repeatDecNs), us(w_.cmpNs),
                          us(w_.keepNs), pf(w_.keepBytes) / 1024.0, pf(w_.needBytes) / 1024.0, us(w_.copyNs), (static_cast<double>(t.wholeDecNs) - static_cast<double>(costNs)) / 1000.0 / pc,
                          pf(w_.overCap), pf(w_.noKept));
        if (n < maxLines) {
            int o = std::snprintf(out[n], 1000, "dyn-census decode: attributes of matched draws, identical / compared a picture (array ranges identical / compared, of them at another address):");
            for (int k = 0; k < kKinds && o < 900; ++k)
                o += std::snprintf(out[n] + o, 1000 - o, " %s %.1f/%.1f (%.1f/%.1f, %.1f)", KindName(k), pf(w_.kindSame[k]), pf(w_.kindCmp[k]), pf(w_.arrSame[k]), pf(w_.arrCmp[k]),
                                   pf(w_.arrMoved[k]));
            ++n;
        }
        for (int og = 0; og < kOrigins && n < maxLines; ++og) {
            int o = std::snprintf(out[n], 1000, "dyn-census decode: %s [draws/vertices/decode us/emit us/matched/whole identical/stream identical/only position changed/in range of indexed]:",
                                  OriginName(og));
            bool any = false;
            for (int s = 0; s < kShapes && o < 860; ++s) {
                const Cls& c = w_.c[og * kShapes + s];
                if (!c.draws) continue;
                any = true;
                o += std::snprintf(out[n] + o, 1000 - o, " %s=%.1f/%.0f/%.1f/%.1f/%.1f/%.1f/%.1f/%.1f/%.1f-of-%.1f", ShapeName(s), pf(c.draws), pf(c.verts), us(c.decNs),
                                   us(c.emitNs), pf(c.matched), pf(c.whole), pf(c.stream), pf(c.nonPos), pf(c.inRange), pf(c.indexed));
            }
            if (any) ++n;
        }
        w_ = Win{};
        return n;
    }

    // (the host test reads the window's counts directly)
    struct Cls {
        uint64_t draws = 0, verts = 0, decNs = 0, emitNs = 0, matched = 0, whole = 0, wholeDecNs = 0, stream = 0, nonPos = 0, nonPosDecEst = 0;
        uint64_t seenPrev = 0, seenPrevDecNs = 0, repeat = 0, repeatDecNs = 0, indexed = 0, inRange = 0;
        void Add(const Cls& o) {
            draws += o.draws; verts += o.verts; decNs += o.decNs; emitNs += o.emitNs; matched += o.matched; whole += o.whole; wholeDecNs += o.wholeDecNs;
            stream += o.stream; nonPos += o.nonPos; nonPosDecEst += o.nonPosDecEst; seenPrev += o.seenPrev; seenPrevDecNs += o.seenPrevDecNs;
            repeat += o.repeat; repeatDecNs += o.repeatDecNs; indexed += o.indexed; inRange += o.inRange;
        }
    };
    struct Win {
        uint64_t pics = 0;
        Cls c[kClasses] = {};
        uint64_t kindCmp[kKinds] = {}, kindSame[kKinds] = {}, arrCmp[kKinds] = {}, arrSame[kKinds] = {}, arrMoved[kKinds] = {};
        uint64_t cmpNs = 0, keepNs = 0, keepBytes = 0, needBytes = 0, copyNs = 0, copyBytes = 0, overCap = 0, noKept = 0;
    };
    const Win& Window() const { return w_; }
    Cls Total() const { Cls t{}; for (const Cls& c : w_.c) t.Add(c); return t; }
    double ClockNs() const { return clockNs_; }
    size_t KeptBytes() const { return prevBytes_.capacity() + curBytes_.capacity(); }

private:
    struct ARec { const uint8_t* base; uint32_t arrOff, arrLen, stride; bool le; };
    struct Rec { uint64_t sig; uint32_t streamOff, streamLen, attr0; int nAttr; bool kept; };

    static bool SliceSame(const uint8_t* a, const uint8_t* b, uint32_t count, uint32_t ss, uint32_t off, uint32_t size) {
        for (uint32_t v = 0; v < count; ++v)
            if (std::memcmp(a + static_cast<size_t>(v) * ss + off, b + static_cast<size_t>(v) * ss + off, size) != 0) return false;
        return true;
    }
    uint64_t Net(uint64_t ns, uint32_t scopes) const {
        const double off = clockNs_ * scopes;
        return static_cast<double>(ns) > off ? static_cast<uint64_t>(static_cast<double>(ns) - off) : 0u;
    }
    void Calibrate() {
        // (the cost of one clock read: the smallest difference of two reads in a row, of 64 tries)
        uint64_t best = ~0ull;
        for (int i = 0; i < 64; ++i) {
            const uint64_t a = now_(), b = now_();
            best = std::min<uint64_t>(best, b - a);
        }
        clockNs_ = static_cast<double>(best);
        calibrated_ = true;
    }
    void Keep(uint64_t key, const DrawIn& d, uint32_t slen) {
        Rec r{d.sig, 0, slen, static_cast<uint32_t>(curAttr_.size()), d.nAttr, false};
        size_t need = slen;
        for (int i = 0; i < d.nAttr; ++i) if (d.a[i].indexed) need += d.a[i].arrLen;
        w_.needBytes += need;
        const bool fits = need <= cap_ / 8 && curBytes_.size() + need <= cap_;
        if (fits && curBytes_.capacity() < cap_) curBytes_.reserve(cap_); // (once: the copies' time is the copy, not a growth)
        const uint64_t t0 = fits ? now_() : 0;
        if (fits) {
            r.streamOff = static_cast<uint32_t>(curBytes_.size());
            curBytes_.insert(curBytes_.end(), d.stream, d.stream + slen);
        }
        for (int i = 0; i < d.nAttr; ++i) {
            const AttrIn& a = d.a[i];
            ARec ra{a.arr, 0, a.indexed ? a.arrLen : 0, a.stride, a.le};
            if (fits && a.indexed && a.arrLen) {
                ra.arrOff = static_cast<uint32_t>(curBytes_.size());
                curBytes_.insert(curBytes_.end(), a.arr, a.arr + a.arrLen);
            }
            curAttr_.push_back(ra);
        }
        if (fits) {
            const uint64_t t1 = now_();
            w_.keepNs += Net(t1 - t0, 1);
            w_.keepBytes += need;
            r.kept = true;
        } else {
            ++w_.overCap;
        }
        cur_[key] = r; // (a key seen twice in one picture cannot happen: the ordinal makes it new)
    }

    Clock now_;
    size_t cap_;
    bool calibrated_ = false;
    double clockNs_ = 0.0;
    Win w_;
    std::unordered_map<uint64_t, Rec> prev_, cur_;
    std::unordered_map<uint64_t, uint32_t> sigCount_;
    std::unordered_set<uint64_t> prevH_, curH_;
    std::vector<uint8_t> prevBytes_, curBytes_, scratch_;
    std::vector<ARec> prevAttr_, curAttr_;
};

} // namespace deccen
} // namespace vgx
