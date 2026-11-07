// GXV_SHADOWMEMO (shadowmemo, 6 Nov): the shadow projection ray cast (ShadowController::updateProjection) reused from the last
// turn when it would give the same answer. Used by decomp/src/Game/LiveActor/ShadowController.cpp; tested on the host by
// tests/shadowmemo_test.cpp (tools/test_shadowmemo.sh).
//
// shadowmemo2 (r89 console: Bowser's Star Reactor lost ~3 fps): changes are no longer found by walking every collision part each
// turn (cold memory: ~2 us a part on the console) but written where they happen (port/shadow_memo_hooks.h: CollisionParts'
// matrix, radius, on/off and KCL writers, the zone lists and zone spheres). A memo that is reused moves its log position to now,
// so it only ever looks at the changes made since its own last look (typically one turn's moving parts), each first through a
// float box prefilter. The walk below (Snapshot) remains for GXV_SHADOWMEMO_VERIFY's audit of the hooks and for the host test.
//
// The cast (MR::getFirstPolyOnLineToMap / ToWaterSurface -> CollisionCategorizedKeeper::checkStrikeLine) is a pure function of
//   - its arguments: start, direction (the drop position, direction, length and start offset folded in), category, parts filter;
//   - the keeper's zone lists (which zones, in which order, which parts in which order);
//   - each listed zone's culling sphere (mCenter, mRadius; zone 0 is never culled);
//   - each listed part's mBaseMatrix, mInvBaseMatrix, mServer (static KCL data), mHitSensor, _CC and _D8.
// A Snapshot keeps all of that (one per category) and is compared, in list order, once per ShadowControllerHolder run:
//   - a different list (a part or zone added, removed or moved) bumps the category's epoch: every memo of it is dropped;
//   - a changed zone sphere or part state is written to a ring log (old and new sphere, old and new _CC).
// A memo (start, direction, filter, category, epoch, log position) is kept when, for every log entry since it was made:
//   - part entry: the part is off (_CC false) or its sphere is surely outside the segment, both before and after;
//   - zone entry: its sphere is surely outside the segment both before and after, or surely touching it both before and after.
// A part that checkStrikeLine's own sphere tests would skip in both states adds no hit in either; a zone whose test answers the
// same keeps the same parts under test. So every hit, its order and the 32-hit cap are the same: the cast's answer is the same.
// "Surely" comes from Classify(): double precision with a margin of 1 + 1e-4 * (r + |C - A|_1 + |B - A|_1) units, far above
// the rounding of the float tests it stands for (a few ulps of those magnitudes). Anything not finite, a negative radius or a
// segment shorter than 1e-3 is "edge" (neither sure answer).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace port {
namespace shadowmemo {

enum { kOut = 0, kIn = 1, kEdge = 2 };

// The segment of one cast, as checkStrikeLine sees it: A = start, B = start + direction (float sum, as there).
struct Seg {
    double a[3], b[3];
    double lo[3], hi[3];  // its box, widened by its own part of the margin (pad)
    double pad;           // 1e-4 * (|A|_1 + |B - A|_1)
    double len1;          // |B - A|_1
    bool usable;          // finite and at least 1e-3 long
};

inline void MakeSeg(Seg* s, const float start[3], const float dir[3]) {
    float end[3];
    for (int i = 0; i < 3; ++i) end[i] = start[i] + dir[i];
    double a1 = 0, l1 = 0, l2 = 0;
    bool finite = true;
    for (int i = 0; i < 3; ++i) {
        s->a[i] = start[i];
        s->b[i] = end[i];
        finite = finite && std::isfinite(start[i]) && std::isfinite(end[i]) && std::isfinite(dir[i]);
        const double d = s->b[i] - s->a[i];
        a1 += std::fabs(s->a[i]);
        l1 += std::fabs(d);
        l2 += d * d;
    }
    s->len1 = l1;
    s->pad = 1e-4 * (a1 + l1);
    s->usable = finite && l2 >= 1e-6;
    for (int i = 0; i < 3; ++i) {
        s->lo[i] = (s->a[i] < s->b[i] ? s->a[i] : s->b[i]) - s->pad;
        s->hi[i] = (s->a[i] < s->b[i] ? s->b[i] : s->a[i]) + s->pad;
    }
}

// A sphere as written to the log, with its own part of the margin: 1 + 1e-4 * (r + |C|_1). (|C - A|_1 <= |C|_1 + |A|_1.)
struct Sphere {
    float c[3];
    float r;
    float pad;
    uint8_t ok;  // finite, r >= 0
};

inline void MakeSphere(Sphere* s, float x, float y, float z, float r) {
    s->c[0] = x;
    s->c[1] = y;
    s->c[2] = z;
    s->r = r;
    s->ok = std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(r) && r >= 0.0f;
    s->pad = s->ok ? static_cast<float>(1.0 + 1e-4 * (static_cast<double>(r) + std::fabs(x) + std::fabs(y) + std::fabs(z))) : 0.0f;
}

// Is the sphere surely clear of the segment (kOut), surely touching it (kIn), or too close to call (kEdge)?
// kOut means checkStrikeLine's tests (isSphereOverlappingWithBox && MR::checkHitSegmentSphere) fail; kIn means they pass.
inline int Classify(const Seg& s, const Sphere& sp) {
    if (!s.usable || !sp.ok) return kEdge;
    const double r = sp.r;
    const double m = static_cast<double>(sp.pad) + s.pad;  // >= 1 + 1e-4 * (r + |C - A|_1 + |B - A|_1)
    // Box first: a centre further than r + m from the segment's box on any axis is further than r + m from the segment.
    for (int i = 0; i < 3; ++i) {
        const double c = sp.c[i];
        if (c < s.lo[i] - r - m || c > s.hi[i] + r + m) return kOut;
    }
    double ab[3], ac[3];
    double abab = 0, acab = 0;
    for (int i = 0; i < 3; ++i) {
        ab[i] = s.b[i] - s.a[i];
        ac[i] = static_cast<double>(sp.c[i]) - s.a[i];
        abab += ab[i] * ab[i];
        acab += ac[i] * ab[i];
    }
    double t = acab / abab;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    double d2 = 0;
    for (int i = 0; i < 3; ++i) {
        const double q = ac[i] - t * ab[i];
        d2 += q * q;
    }
    const double out = r + m;
    if (d2 > out * out) return kOut;
    const double in = r - m;
    if (in > 0 && d2 < in * in) return kIn;
    return kEdge;
}

// One change seen between two snapshots, or written by a hook.
enum { kAllSlots = 0xFF };
struct Change {
    uint8_t slot;  // category slot (0: map, 1: water surface; kAllSlots: either, the hooks' changes)
    uint8_t zone;  // 1: a zone's culling sphere; 0: a part
    uint8_t oldOn, newOn;  // parts: _CC before / after
    Sphere was, now;
    float box[6];  // prefilter: lo xyz, hi xyz of both spheres widened by r + pad + 1 (rounded out), see Prefilter()
    const void* who;  // the part or zone (verify's audit)
};

// The float box that holds both spheres of a change with their whole own margin (r + pad), widened once more by 1 unit plus a
// relative 1e-6 to stand for the float rounding of these sums. A segment whose padded box misses it is surely clear of both
// (Classify's first test, which also widens by the segment's pad, rejects the same and more).
inline void SetBox(Change* c) {
    for (int i = 0; i < 3; ++i) {
        double lo = 1e30, hi = -1e30;
        const Sphere* sp[2] = {&c->was, &c->now};
        for (int k = 0; k < 2; ++k) {
            if (!sp[k]->ok) {
                lo = -1e30;  // (a sphere that is not finite: the box is everything)
                hi = 1e30;
                continue;
            }
            const double r = static_cast<double>(sp[k]->r) + sp[k]->pad;
            const double a = static_cast<double>(sp[k]->c[i]) - r, b = static_cast<double>(sp[k]->c[i]) + r;
            if (a < lo) lo = a;
            if (b > hi) hi = b;
        }
        c->box[i] = static_cast<float>(lo - 1.0 - 1e-6 * std::fabs(lo));
        c->box[3 + i] = static_cast<float>(hi + 1.0 + 1e-6 * std::fabs(hi));
    }
}

// No index (StillValid one by one).
struct NoIndex {
    uint32_t from, to;
    bool usable;
    template <class L>
    bool Clear(const L&, unsigned, const Seg&, const float*, const float*, uint32_t*) {
        return true;
    }
};

// Ring log of changes and the per-slot list epochs.
template <unsigned kCap>
struct Log {
    Change ring[kCap];
    uint32_t seq = 0;          // changes written so far
    uint32_t epoch[2] = {1, 1};  // bumped when a slot's lists change shape
    uint32_t written = 0;      // statistics: changes written

    void Push(const Change& c) {
        Change& e = ring[seq % kCap];
        e = c;
        SetBox(&e);
        ++seq;
        ++written;
    }

    // The segment's own padded box in float, rounded out the same way as the changes' boxes.
    static void SegBox(const Seg& s, float slo[3], float shi[3]) {
        for (int i = 0; i < 3; ++i) {
            slo[i] = static_cast<float>(s.lo[i] - 1e-6 * std::fabs(s.lo[i]));
            shi[i] = static_cast<float>(s.hi[i] + 1e-6 * std::fabs(s.hi[i]));
        }
    }
    // Can this change alter the answer of a cast on `s` (of slot `slot`)? Box prefilter first (both spheres surely clear).
    static bool Affects(const Change& c, unsigned slot, const Seg& s, const float slo[3], const float shi[3]) {
        if (c.slot != slot && c.slot != kAllSlots) return false;
        if (c.box[0] > shi[0] || c.box[3] < slo[0] || c.box[1] > shi[1] || c.box[4] < slo[1] || c.box[2] > shi[2] || c.box[5] < slo[2]) {
            return false;
        }
        if (c.zone) {
            const int a = Classify(s, c.was), b = Classify(s, c.now);
            return !((a == kOut && b == kOut) || (a == kIn && b == kIn));
        }
        return (c.oldOn && Classify(s, c.was) != kOut) || (c.newOn && Classify(s, c.now) != kOut);
    }

    // True if a cast made on `s` when the log stood at memoSeq (slot epoch memoEpoch) still gives the same answer.
    // More than maxScan changes since: false without looking (a cast is cheaper). *scanned: entries looked at (statistics).
    // index (optional): the changes from index->from on are looked up through it instead of one by one.
    template <class Index>
    bool StillValid(unsigned slot, const Seg& s, uint32_t memoSeq, uint32_t memoEpoch, uint32_t* scanned, uint32_t maxScan, Index* index) const {
        if (epoch[slot] != memoEpoch) return false;
        const uint32_t n = seq - memoSeq;
        if (n > kCap) return false;
        if (n == 0) return true;
        if (!s.usable) return false;
        float slo[3], shi[3];
        SegBox(s, slo, shi);
        // [memoSeq, linEnd): one by one; [linEnd, index->to): the index; [index->to, seq): one by one
        uint32_t linEnd = seq, tailFrom = seq;
        if (index != nullptr && index->usable && index->to - memoSeq <= n && index->from - memoSeq <= index->to - memoSeq) {
            linEnd = index->from;  // (memoSeq <= from <= to <= seq, in ring arithmetic)
            tailFrom = index->to;
        }
        const uint32_t linear = (linEnd - memoSeq) + (seq - tailFrom);
        if (linear > maxScan) return false;
        *scanned += linear;
        for (uint32_t i = memoSeq; i != linEnd; ++i) {
            if (Affects(ring[i % kCap], slot, s, slo, shi)) return false;
        }
        for (uint32_t i = tailFrom; i != seq; ++i) {
            if (Affects(ring[i % kCap], slot, s, slo, shi)) return false;
        }
        if (linEnd != seq && !index->Clear(*this, slot, s, slo, shi, scanned)) return false;
        return true;
    }
    bool StillValid(unsigned slot, const Seg& s, uint32_t memoSeq, uint32_t memoEpoch, uint32_t* scanned, uint32_t maxScan = kCap) const {
        return StillValid(slot, s, memoSeq, memoEpoch, scanned, maxScan, static_cast<NoIndex*>(nullptr));
    }
};

// A coarse 3D grid over one batch of changes (those written since the holder's last run), so a memo looks only at the changes
// whose box shares a cell with its segment's box instead of at all of them. Built once per holder run. Exact: two boxes that
// overlap share a point, and floor() puts that point in a cell both were filed under (both use the same float boxes). A box too
// big or too far for the grid goes to an overflow list that every query reads; a segment box too big reads the whole batch.
template <unsigned kCap>
struct BatchIndex {
    enum { kBuckets = 1024, kMaxIns = 4096, kMaxSpan = 4 };  // (span: cells per axis)
    static constexpr float kCell = 1024.0f;
    uint32_t from = 0, to = 0;
    bool usable = false;
    int32_t head[kBuckets];
    int32_t next[kMaxIns];
    uint32_t ent[kMaxIns];  // log seq of the change
    uint32_t nIns = 0;
    uint32_t over[kCap];
    uint32_t nOver = 0;
    uint32_t stamp[kCap];  // per ring slot: the last query that looked at it
    uint32_t query = 0;
    uint32_t built = 0;  // statistics

    static bool Range(float lo, float hi, int32_t* a, int32_t* b) {
        if (!(lo >= -1.0e7f && hi <= 1.0e7f)) return false;
        *a = static_cast<int32_t>(std::floor(lo / kCell));
        *b = static_cast<int32_t>(std::floor(hi / kCell));
        return *b - *a < kMaxSpan;
    }
    static uint32_t Hash(int32_t x, int32_t y, int32_t z) {
        return (static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ static_cast<uint32_t>(z) * 83492791u) % kBuckets;
    }

    void Build(const Log<kCap>& log, uint32_t batchFrom) {
        from = batchFrom;
        to = log.seq;
        nIns = nOver = 0;
        usable = to - from <= kCap;
        if (!usable) return;
        for (int i = 0; i < kBuckets; ++i) head[i] = -1;
        for (uint32_t q = from; q != to; ++q) {
            const Change& c = log.ring[q % kCap];
            int32_t a[3], b[3];
            const bool fits = Range(c.box[0], c.box[3], &a[0], &b[0]) && Range(c.box[1], c.box[4], &a[1], &b[1]) &&
                              Range(c.box[2], c.box[5], &a[2], &b[2]);
            const uint32_t cells = fits ? static_cast<uint32_t>((b[0] - a[0] + 1) * (b[1] - a[1] + 1) * (b[2] - a[2] + 1)) : 0;
            if (!fits || nIns + cells > kMaxIns) {
                over[nOver++] = q;
                continue;
            }
            for (int32_t x = a[0]; x <= b[0]; ++x)
                for (int32_t y = a[1]; y <= b[1]; ++y)
                    for (int32_t z = a[2]; z <= b[2]; ++z) {
                        const uint32_t h = Hash(x, y, z);
                        ent[nIns] = q;
                        next[nIns] = head[h];
                        head[h] = static_cast<int32_t>(nIns);
                        ++nIns;
                    }
        }
        ++built;
    }

    // Every change of the batch that can alter the answer is looked at (the others may be skipped): false if one does.
    bool Clear(const Log<kCap>& log, unsigned slot, const Seg& s, const float slo[3], const float shi[3], uint32_t* scanned) {
        for (uint32_t k = 0; k < nOver; ++k) {
            ++*scanned;
            if (Log<kCap>::Affects(log.ring[over[k] % kCap], slot, s, slo, shi)) return false;
        }
        int32_t a[3], b[3];
        const bool fits = Range(slo[0], shi[0], &a[0], &b[0]) && Range(slo[1], shi[1], &a[1], &b[1]) && Range(slo[2], shi[2], &a[2], &b[2]);
        if (!fits) {  // (a segment too long for the grid: the whole batch)
            for (uint32_t q = from; q != to; ++q) {
                ++*scanned;
                if (Log<kCap>::Affects(log.ring[q % kCap], slot, s, slo, shi)) return false;
            }
            return true;
        }
        if (++query == 0) {  // (stamps wrapped: start them again)
            std::memset(stamp, 0, sizeof stamp);
            query = 1;
        }
        for (int32_t x = a[0]; x <= b[0]; ++x)
            for (int32_t y = a[1]; y <= b[1]; ++y)
                for (int32_t z = a[2]; z <= b[2]; ++z) {
                    for (int32_t i = head[Hash(x, y, z)]; i >= 0; i = next[i]) {
                        const uint32_t q = ent[i];
                        if (stamp[q % kCap] == query) continue;
                        stamp[q % kCap] = query;
                        ++*scanned;
                        if (Log<kCap>::Affects(log.ring[q % kCap], slot, s, slo, shi)) return false;
                    }
                }
        return true;
    }
};

// What checkStrikeLine reads of one part. Compared bit for bit.
struct PartState {
    const void* parts;
    float base[12];
    float inv[12];
    const void* server;
    const void* sensor;
    float radius;  // _D8
    uint32_t on;   // _CC
};
// What it reads of one zone (zone 0: never culled, its sphere left at 0).
struct ZoneState {
    const void* zone;
    uint32_t count;
    float c[3];
    float r;
};

// One entry of a snapshot: a zone, then its parts.
struct SnapEntry {
    uint32_t isZone;
    union {
        PartState p;
        ZoneState z;
    };
};

// One category's lists as last seen, compared and updated in list order.
template <class Vec>  // Vec: a vector of SnapEntry (PortHostVector in the game, std::vector on the host)
struct Snapshot {
    typedef SnapEntry Entry;

    Vec entries;
    bool valid = false;
    uint32_t idx = 0;
    bool reshaped = false;
    unsigned slot = 0;

    void Reset() {
        entries.clear();
        valid = false;
    }
    void Begin(unsigned s) {
        slot = s;
        idx = 0;
        reshaped = !valid;
    }
    Entry* Next() {
        if (idx < entries.size()) return &entries[idx++];
        reshaped = true;
        entries.emplace_back();
        ++idx;
        std::memset(&entries.back(), 0, sizeof(Entry));
        return &entries.back();
    }
    template <class L>
    void Zone(L& log, const ZoneState& z) {
        Entry* e = Next();
        if (!e->isZone || e->z.zone != z.zone || e->z.count != z.count) {
            reshaped = true;
        } else if (std::memcmp(e->z.c, z.c, sizeof z.c + sizeof z.r) == 0) {
            return;  // (unchanged: nothing to write)
        } else if (!reshaped) {
            Change c;
            std::memset(&c, 0, sizeof c);
            c.slot = static_cast<uint8_t>(slot);
            c.zone = 1;
            c.who = z.zone;
            MakeSphere(&c.was, e->z.c[0], e->z.c[1], e->z.c[2], e->z.r);
            MakeSphere(&c.now, z.c[0], z.c[1], z.c[2], z.r);
            log.Push(c);
        }
        std::memset(e, 0, sizeof(Entry));
        e->isZone = 1;
        e->z = z;
    }
    template <class L>
    void Part(L& log, const PartState& p) {
        Entry* e = Next();
        if (e->isZone || e->p.parts != p.parts) {
            reshaped = true;
        } else if (std::memcmp(&e->p, &p, sizeof p) == 0) {
            return;  // (unchanged: nothing to write)
        } else if (!reshaped) {
            Change c;
            std::memset(&c, 0, sizeof c);
            c.slot = static_cast<uint8_t>(slot);
            c.zone = 0;
            c.who = p.parts;
            c.oldOn = e->p.on != 0;
            c.newOn = p.on != 0;
            MakeSphere(&c.was, e->p.base[3], e->p.base[7], e->p.base[11], e->p.radius);
            MakeSphere(&c.now, p.base[3], p.base[7], p.base[11], p.radius);
            log.Push(c);
        }
        std::memset(e, 0, sizeof(Entry));
        e->isZone = 0;
        e->p = p;
    }
    template <class L>
    void End(L& log) {
        if (idx != entries.size()) {
            reshaped = true;
            entries.resize(idx);
        }
        valid = true;
        if (reshaped) ++log.epoch[slot];
    }
};

}  // namespace shadowmemo
}  // namespace port
