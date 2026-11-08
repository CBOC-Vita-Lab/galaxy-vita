// Cached display-list geometry (player side).
//
// Nearly all of the game's geometry arrives as display lists in guest RAM
// that hold nothing but one or a few draw commands with indexed attributes,
// and that are called again every frame (often several times per frame, once
// per pass) with the same vertex descriptor and arrays: static meshes, and
// skinned characters whose vertices are static in model space (the skinning
// is done with the matrix palette). Decoding such a list means resolving
// every index against the guest arrays and converting big-endian fixed-point
// to floats, ~100 instructions per vertex even in the attribute-major decoder.
//
// An entry here keeps the decoded vertices of a list for one (vertex
// descriptor, vertex format, array bindings) combination. It is valid while
// the write-generation counters (vgx_guestmem.cpp) under the list bytes and
// under the array ranges the decode read have not moved; when they have, the
// content hash decides. Lists that keep changing are not cached for a while.
//
//   GXV_DLCACHE=0       off: every call is decoded
//   GXV_DLCACHE=verify  hash on every call and report an entry whose
//                           bytes changed without a write notification
//   GXV_DLCACHE_BOUNDS_GUARD=1  do not cache decodes with unreadable indexed elements (default off)
//   GXV_RIGIDMERGE_DLMAX=<n>    (with GXV_RIGIDMERGE) lists of up to n vertices without matrix indices stay in the arena, to join palette runs
//   GXV_RIGIDMERGE_DLMAX=learn  (or GXV_RIGIDMERGE_DLLEARN=1) the same for lists of up to GXV_RIGIDMERGE_DLLEARN_MAX vertices (default 512), but a list
//                               is copied only where that joins a run: see "learned copies" below
//   GXV_DRAWREPLAY=1    (default) the "draw replay" below: a call that repeats the previous picture's call at the same place in the
//                           picture goes to its cache entry without the lookups; 2: also hints the next call's entry; 0: off
// Counters: per frame in the CSV (dlHit, dlBuild, dlRehash, dlBypass), totals in vgx_stats.txt.
#include "vgx_workload.h"
#include "vgx_fesplit.h"
#include "vgx_guestmem.h"
#include "vgx_state.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <unordered_map>

namespace vgx {

namespace {

struct Seg {
    uint8_t prim;       // GX draw opcode (primitive type bits)
    uint8_t fmt;
    uint16_t stride;    // immutable plan layout, in the existing alignment gap
    uint32_t layoutMask;
    uint32_t count;
    uint32_t offset;    // into Entry::verts (bytes)
    uint32_t idxOffset; // source-stream bytes during build; static index-buffer elements once promoted
    uint32_t vtxFirst;  // once promoted: what the primitive's indices in the static buffer are offset by
    uint32_t posMtxMax; // highest position-matrix row its vertices name
};

// Consecutive primitives of a promoted list that share a vertex layout and a primitive kind:
// one index range of the static buffer, drawn with one record. The lists the game's models
// are made of hold hundreds of triangle strips of three to eight vertices each and nothing
// else, so nothing can change between those strips: the castle's 38 000 strips a frame are
// some 300 such runs (Galaxy).
struct Run {
    uint8_t primKind = 0;
    uint32_t layoutMask = 0;
    uint32_t vtxByteOffset = 0, vtxFirst = 0, vtxCount = 0;
    uint32_t idxOffset = 0, idxCount = 0;
    uint32_t posMtxMax = 0, prims = 0;
};

// Per indexed attribute (POS, NRM, CLR0, CLR1, TEX0..7): the array a decode read, the span of it, and the span's write generation.
struct ArrUse {
    const uint8_t* p = nullptr;
    uint32_t stride = 0;
    uint32_t lo = 0, hi = 0;
    uint64_t gen = 0;
};
struct Entry {
    // ---- what a hit reads (CallListBody, GXV_DRAWREPLAY), first and together: on the console a hit's time is mostly its cache
    // misses, and the fields it reads were spread over some fourteen cache lines of 32 bytes.
    // identity and policy
    const uint8_t* list = nullptr;
    uint32_t nbytes = 0;
    uint32_t idxMask = 0;          // indexed attributes that were read
    uint8_t fmtMask = 0;           // vertex formats the list's draws use
    uint8_t fails = 0;             // consecutive content changes
    bool raced = false;            // a write over what this reads arrived while it was being built: rebuild on next use
    bool promotable = false;       // the list as a whole is too large for the CPU-transform-and-merge path
    bool rigidCopy = false;        // GXV_RIGIDMERGE_DLMAX keeps it in the arena (counted as a copy for a palette run)
    bool copyable = false;         // (learned copies, GXV_RIGIDMERGE_DLLEARN, see below) the decoded vertices are kept after the promotion
    uint32_t rpStamp = 0;          // (GXV_DRAWREPLAY) moves when the entry is built again or stops being the first variant of its list
    uint32_t bufId = 0;            // static buffer in the back end (0: vertices are copied into the frame arena per use)
    uint32_t totalVerts = 0;
    uint64_t lastUse = 0;
    // validity
    uint64_t genList = 0;
    uint8_t vcd[24] = {};          // (21 used; whole words, to compare them as such)
    Entry* next = nullptr;         // other variants of the same list
    // content a hit emits
    std::vector<Run> runs;         // once promoted
    std::vector<RowBox> rowBoxes;  // (GXV_CHARAA) model-space bounds of the vertices per position-matrix row, [0] = all (RowBoxesOf)
    ArrUse arr[12];                // (the indexed attributes: idxMask >> 9)
    VtxAttrFmt vat[8][12] = {};    // rows of the formats in fmtMask (attributes POS..TEX7)
    // ---- the rest
    uint64_t hash = 0;
    std::vector<Seg> segs;
    std::vector<uint8_t> verts;    // decoded vertices (handed to the back end when promoted)
    std::vector<uint16_t> idx;     // only kept for GXV_HASH=2
    uint32_t bufBytes = 0;
    uint32_t ticket = 0;           // (GXV_SPLIT_COPY) its last copies into the arena: split::Settle before verts change or go
    uint32_t keptBytes = 0;        // (learned copies)
    uint32_t serial = 0;           // (staggers the probes; names the entry in the table of uses)
};

// Per list: its variants (same bytes, different vertex state / arrays) and the
// bookkeeping that notices a list whose bindings change every frame (meshes
// whose arrays come from a per-frame allocator): for those every call would
// build a new variant that is never used again.
struct ListInfo {
    Entry* head = nullptr;
    uint64_t lastNewVariant = 0; // frame
    uint8_t streak = 0;          // consecutive frames that needed a new variant
};
constexpr int kMaxVariants = 8;
constexpr uint64_t kRetainFrames = 600;
// The age entries are judged by is counted in frames that called a cached list, not in
// frames: the opening's picture book (41 s), the galaxy map and the staff roll switch the 3D
// scene off, and when it came back, every list of the castle had been "unused for 600 frames"
// and was decoded and uploaded again (about 25 frames' worth of front-end work in one go,
// then a tail of 20 frames: the hitch when the book ends).
uint64_t g_age = 0, g_ageFrame = ~0ull;
inline uint64_t AgeOf(uint64_t frame) {
    if (frame != g_ageFrame) { g_ageFrame = frame; ++g_age; }
    return g_age;
}
// A map from a display list's address to its bookkeeping. std::unordered_map divides by a
// prime bucket count on every lookup, a software divide on the console's CPU (the front end
// called it ~3000 times per frame in the festival plaza: 3% of its instructions). This is
// open addressing over a power-of-two table with a multiplicative hash. Values are separate
// allocations, so references to them stay valid when the table grows (the front caches below
// and the callers hold them).
template <class V>
class PtrMap {
public:
    PtrMap() = default;
    PtrMap(const PtrMap&) = delete;
    PtrMap& operator=(const PtrMap&) = delete;
    ~PtrMap() {
        for (Slot& s : slots_) if (Live(s.key)) delete s.val;
    }
    bool empty() const { return live_ == 0; }
    size_t size() const { return live_; }
    // (GXV_FE_PREFETCH) the slot a lookup of `key` reads first, or null
    const void* HomeSlot(const uint8_t* key) const { return slots_.empty() ? nullptr : &slots_[Home(key)]; }
    V* Find(const uint8_t* key) {
        if (slots_.empty()) return nullptr;
        for (size_t i = Home(key);; i = (i + 1) & mask_) {
            const uint8_t* k = slots_[i].key;
            if (k == key) return slots_[i].val;
            if (!k) return nullptr;
        }
    }
    V& operator[](const uint8_t* key) {
        if (V* v = Find(key)) return *v;
        if ((used_ + 1) * 4 > slots_.size() * 3) Grow();
        size_t i = Home(key);
        while (Live(slots_[i].key)) i = (i + 1) & mask_;
        if (!slots_[i].key) ++used_; // (an empty slot becomes used; a tombstone already counted)
        slots_[i] = Slot{key, new V()};
        ++live_;
        return *slots_[i].val;
    }
    void Erase(const uint8_t* key) {
        if (slots_.empty()) return;
        for (size_t i = Home(key);; i = (i + 1) & mask_) {
            if (slots_[i].key == key) { Kill(slots_[i]); return; }
            if (!slots_[i].key) return;
        }
    }
    // pred(key, value&) -> true erases the entry (it may change the value first).
    template <class F>
    void EraseIf(F pred) {
        for (Slot& s : slots_)
            if (Live(s.key) && pred(s.key, *s.val)) Kill(s);
    }
private:
    struct Slot { const uint8_t* key = nullptr; V* val = nullptr; };
    static const uint8_t* Tomb() { return reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(1)); }
    static bool Live(const uint8_t* k) { return k && k != Tomb(); }
    size_t Home(const uint8_t* key) const {
        return static_cast<size_t>((static_cast<uint32_t>(reinterpret_cast<uintptr_t>(key) >> 2) * 0x9E3779B1u) >> shift_) & mask_;
    }
    void Kill(Slot& s) {
        delete s.val;
        s = Slot{Tomb(), nullptr};
        --live_;
    }
    void Grow() {
        std::vector<Slot> old;
        old.swap(slots_);
        size_t cap = old.empty() ? 256 : old.size();
        if (live_ * 2 >= cap) cap *= 2; // (else only tombstones are cleared away)
        slots_.assign(cap, Slot{});
        mask_ = cap - 1;
        unsigned bits = 0;
        while ((size_t(1) << bits) < cap) ++bits;
        shift_ = 32 - bits;
        used_ = 0;
        for (Slot& s : old) {
            if (!Live(s.key)) continue;
            size_t i = Home(s.key);
            while (slots_[i].key) i = (i + 1) & mask_;
            slots_[i] = s;
            ++used_;
        }
    }
    std::vector<Slot> slots_;
    size_t mask_ = 0, live_ = 0, used_ = 0;
    unsigned shift_ = 32;
};

PtrMap<ListInfo> g_map;
// In front of g_map (see CacheFront in vgx_texcache.cpp): the list last looked up for each
// of 256 address classes. Emptied whenever an entry of g_map is erased.
struct MapFront { const uint8_t* list; ListInfo* li; };
MapFront g_mapFront[256] = {};
inline void MapFrontClear() {
    for (MapFront& f : g_mapFront) f.li = nullptr;
}
// (GXV_DRAWREPLAY, see "draw replay" below) moves whenever an entry is freed (evicted, unlinked; a list is put out of caching only
// together with that): a record of the trace names an entry, and is used only while no entry was freed since it was made.
uint32_t g_rpEpoch = 1;
inline void RpBump() { ++g_rpEpoch; }
struct Bypass {
    uint64_t until = 0;
    uint8_t level = 0; // each time a list proves volatile again it is left alone twice as long
};
uint64_t SetBypass(Bypass& b, uint64_t frame) {
    b.until = frame + (120ull << b.level);
    if (b.level < 4) ++b.level;
    return b.until;
}
PtrMap<Bypass> g_bypass; // lists not worth caching for now (state inside, volatile)
// (GXV_FE_SPLIT: why a call of CallListBody went the "list-other" way, counted per picture in the "list-other why" line of vgx_workload.h)
enum OtherWhy : unsigned { OwNotGuest, OwBypassStateReplay, OwBypassStream, OwRehashBypassSet, OwStreakBypassSet, OwUncacheableState, OwUncacheableStream, OwBuild, OwCount };
uint64_t g_otherWhy[OwCount] = {};

int g_mode = -1; // 0 off, 1 on, 2 verify
bool g_static = true;   // GXV_STATICBUF=0: never promote (decoded vertices are copied into the arena every time)
bool g_boundsGuard = false;
bool g_keepCopy = false; // GXV_HASH=2: keep the bytes of promoted entries for hashing
struct Counters {
    uint64_t hits = 0, builds = 0, rehashOk = 0, rehashChanged = 0, bypassed = 0, uncacheable = 0, staleNoNotify = 0, evicted = 0;
    uint64_t hitVerts = 0, builtVerts = 0, liveBytes = 0, staticBytes = 0, promoted = 0, staticBufs = 0;
    uint64_t weldLists = 0, weldIn = 0, weldOut = 0; // (GXV_DLWELD) promoted lists welded, their vertices before / after
} g_ctr;

// ---- learned copies (GXV_RIGIDMERGE_DLLEARN)
// Consecutive rigid draws that differ only in their matrix become one GL draw (vgx_draw.cpp, RigidJoin), but only draws whose vertices are in the frame's
// arena can: a cached list that became a static buffer cannot join. GXV_RIGIDMERGE_DLMAX=<n> keeps every list of up to n vertices in the arena (a copy of
// its vertices at each use), which costs a copy where no join follows. Here a list of at most N vertices is a static buffer as always, but its decoded
// vertices are kept as well, and a use copies them into the arena only when uses of the same list, after the same kind of draw (UseKey: the shaders and
// state of the draw before it, which learned list that was, and which registers were written since), joined a run on average often enough to pay for
// the copy (the join rate times GXV_RIGIDMERGE_DLLEARN_GAIN >= the bytes of the copy). How that is known:
//   - a use that was copied: the record stage says whether its first draw joined the record before it, and whether the next job joined it (the join log of
//     the frame, FinishCtx::joinLog, read at the frame's end: the same in every GXV_SPLIT mode);
//   - a use that stayed static: the record stage tests whether the draw would have joined, had it been a copy (FinishStatic: the same tests as RigidJoin,
//     with the previous draw counted as a copy too when its list can be copied); so a join that is there to find is found without trying.
// A copy that joined nothing puts that use of the list out of copying for 64 frames (twice as long each time, up to 512), and a list not
// marked is copied once in 64 frames anyway (staggered by list), in case the test missed something. (The answers are kept in a table of 8192 uses.)
uint32_t g_learnMax = 0;
uint64_t g_joinGain = 0;      // bytes of copying that one saved draw is worth (a GL draw ~20 us, a copy ~1 GB/s)
bool g_learnInit = false;
uint64_t g_keptBytes = 0, g_keptBudget = 0;
uint32_t g_entrySerial = 0;
struct LearnCounters {
    uint64_t copies = 0, copyBytes = 0, copyJoined = 0, copyWasted = 0, staticUses = 0, staticCouldJoin = 0, keptLists = 0;
} g_lc;
// What is known of the uses of the learned lists: by list (Entry::serial) and what came before the use (UseKey): how often the use joined a run (a running mean,
// 0..255), and, after a copy that bought nothing, the frame before which the use is not copied again (retryAt) and how many times in a row that happened.
// A table with room for a few thousand uses, two slots to a bucket; a use that finds its bucket taken takes a slot of it: what is lost is learned again.
struct CtxSlot { uint32_t tag = 0, retryAt = 0; uint8_t rate = 0, failLevel = 0; bool used = false; };
constexpr uint32_t kCtxSlots = 8192;
std::vector<CtxSlot> g_ctx;
CtxSlot* CtxFind(uint32_t serial, uint32_t key, bool create) {
    if (g_ctx.empty()) return nullptr;
    uint64_t h = (static_cast<uint64_t>(serial) << 32 | key) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
    const uint32_t tag = static_cast<uint32_t>(h >> 32) | 1u;
    CtxSlot* b = &g_ctx[(static_cast<uint32_t>(h) & (kCtxSlots - 1)) & ~1u];
    for (int k = 0; k < 2; ++k)
        if (b[k].used && b[k].tag == tag) return &b[k];
    if (!create) return nullptr;
    CtxSlot* v = !b[0].used ? &b[0] : (!b[1].used ? &b[1] : (b[0].rate <= b[1].rate ? &b[0] : &b[1]));
    *v = CtxSlot();
    v->tag = tag;
    return v;
}
struct Use { Entry* e; uint32_t j0, j1, key; uint8_t kind; }; // kind: 0 a copy before the promotion (the first use), 1 a static buffer, 2 a deliberate copy of a static buffer
std::vector<Use> g_uses; // this frame's uses of lists that can be copied

// Static buffer ids: small, reused, owner known (for GXV_HASH=2).
std::vector<uint32_t> g_freeIds;
std::vector<Entry*> g_bufOwner(1, nullptr);
uint32_t AllocBufId(Entry* e) {
    uint32_t id;
    if (!g_freeIds.empty()) { id = g_freeIds.back(); g_freeIds.pop_back(); }
    else { id = static_cast<uint32_t>(g_bufOwner.size()); g_bufOwner.push_back(nullptr); }
    g_bufOwner[id] = e;
    return id;
}
void ReleaseBuffer(Entry& e) {
    if (!e.bufId) return;
    Pending().cmds.push_back(Cmd{CMD_BUF_FREE, e.bufId});
    g_bufOwner[e.bufId] = nullptr;
    g_freeIds.push_back(e.bufId);
    g_ctr.staticBytes -= e.bufBytes;
    --g_ctr.staticBufs;
    e.bufId = 0;
    e.bufBytes = 0;
}
// GXV_DLWELD (default 1; 0: every vertex of every primitive is its own vertex of the static buffer, as before). The game's models are
// hundreds of short triangle strips and fans (three to eight vertices, see Run), and the same mesh vertex (the same indices into the
// position / normal / colour / texture arrays, so the same decoded bytes) comes up again in every strip that touches it: the decoded
// list holds it two to six times, and the vertex shader runs for every copy. Promote writes each distinct vertex of a run once and
// points the run's indices at it. Exact: the vertices are byte for byte the ones the unwelded buffer holds (same layout mask, same
// stride, same bytes, NaN patterns included), so every index reads the same attribute values and the vertex shader gives the same
// outputs for it; the primitives, their order and winding are the same, so the same triangles reach the rasteriser in the same order.
// A run's indices stay inside its own block of the buffer (vtxFirst .. vtxFirst + vtxCount, now the number of distinct vertices) and
// the runs are cut where they were (the unwelded counts), so records, merging and bounds are as before. With absolute indices the
// block's vertices are numbered from the buffer's start (all strides equal), with relative ones (the Vita build: a byte offset per
// draw, GXV_ABSIDX_DEFAULT=0) from the run's own byte offset. Not for GXV_HASH=2 (reference hashes of the buffer's bytes).
bool WeldOn() {
    static const bool on = EnvInt("GXV_DLWELD", 0) != 0;
    return on;
}
// The distinct vertices of one run: an open-addressing table of slots (+1) in the new buffer, emptied by a stamp per run.
struct Welder {
    std::vector<uint32_t> stamp, slot;
    uint32_t mask = 0, cur = 0;
    void Init(uint32_t maxVerts) {
        uint32_t cap = 256;
        while (cap < maxVerts * 2u) cap <<= 1;
        if (stamp.size() < cap) { stamp.assign(cap, 0); slot.assign(cap, 0); cur = 0; }
        mask = static_cast<uint32_t>(stamp.size()) - 1u; // (a larger table from an earlier list is fine: same hash, fewer collisions)
    }
    void NewRun() {
        if (++cur == 0) { std::fill(stamp.begin(), stamp.end(), 0u); cur = 1; }
    }
    // The slot (index among the run's distinct vertices, from 0) of the vertex `v` of `stride` bytes; the run's block starts `block`
    // bytes into `out`; the vertex is appended there if it is new.
    uint32_t Find(std::vector<uint8_t>& out, size_t block, const uint8_t* v, uint32_t stride) {
        uint32_t h = static_cast<uint32_t>(XXH3_64bits(v, stride)) & mask;
        for (;;) {
            if (stamp[h] != cur) {
                const uint32_t n = static_cast<uint32_t>((out.size() - block) / stride);
                out.insert(out.end(), v, v + stride);
                stamp[h] = cur;
                slot[h] = n;
                return n;
            }
            if (std::memcmp(out.data() + block + static_cast<size_t>(slot[h]) * stride, v, stride) == 0) return slot[h];
            h = (h + 1u) & mask;
        }
    }
};

// Moves the entry's geometry into a static buffer of the back end.
void Promote(Entry& e) {
    split::Settle(e.ticket); // (GXV_SPLIT_COPY: the vertices may move to the back end below)
    // With absolute indices every primitive of the buffer is addressed from the buffer's
    // start: that needs one stride for all of them and 16 bits to be enough. (Lists with
    // more than one primitive are rare in this game; one that does not qualify stays in
    // the per-frame arena.)
    const bool abs = AbsoluteIndices();
    const uint32_t stride = e.segs[0].stride;
    if (abs) {
        bool ok = e.totalVerts <= 0x10000;
        for (const Seg& sg : e.segs) ok = ok && sg.stride == stride;
        if (!ok) {
            e.promotable = false;
            g_keptBytes -= e.keptBytes; // (never a static buffer: nothing to learn)
            if (e.copyable) --g_lc.keptLists;
            e.keptBytes = 0;
            e.copyable = false;
            return;
        }
    }
    BufUpload up;
    up.bufId = e.bufId = AllocBufId(&e);
    e.runs.clear();
    const bool weld = !g_keepCopy && WeldOn();
    static Welder welder;
    static std::vector<uint16_t> slotOf;
    if (weld) {
        welder.Init(e.totalVerts);
        up.vtx.reserve(e.verts.size());
    }
    uint64_t weldedVerts = 0; // (welding: the distinct vertices written, over all runs)
    uint32_t runRaw = 0; // (welding: the run's vertices before welding; the runs are cut by these, as without it)
    size_t block = 0;    // (welding: where the run's block of distinct vertices starts in the new buffer, bytes)
    uint32_t base = 0;   // (welding: the number its first vertex has: absolute indices count from the buffer's start, relative ones from the block)
    for (Seg& sg : e.segs) {
        const uint32_t segStride = sg.stride;
        const uint8_t kind = (sg.prim == 0xA8 || sg.prim == 0xB0) ? PRIM_LINES : (sg.prim == 0xB8 ? PRIM_POINTS : PRIM_TRIANGLES);
        const uint32_t n = PrimIndices(sg.prim, sg.count, 0, nullptr);
        // Continue the current run, or start one: at a change of layout or primitive kind, and
        // where a run's vertices would no longer fit 16-bit indices counted from its start.
        Run* r = e.runs.empty() ? nullptr : &e.runs.back();
        const bool cut = weld ? (!r || r->primKind != kind || r->layoutMask != sg.layoutMask || runRaw + sg.count > 0xFFFF)
                              : (!r || r->primKind != kind || r->layoutMask != sg.layoutMask || r->vtxCount + sg.count > 0xFFFF ||
                                 sg.offset != r->vtxByteOffset + r->vtxCount * segStride);
        if (cut) {
            e.runs.emplace_back();
            r = &e.runs.back();
            r->primKind = kind;
            r->layoutMask = sg.layoutMask;
            r->idxOffset = static_cast<uint32_t>(up.idx.size());
            if (weld) {
                block = up.vtx.size(); // (the run's block of distinct vertices starts here)
                base = abs ? static_cast<uint32_t>(block / stride) : 0u;
                r->vtxByteOffset = static_cast<uint32_t>(block);
                r->vtxFirst = base;
                runRaw = 0;
                welder.NewRun();
            } else {
                r->vtxByteOffset = sg.offset;
                r->vtxFirst = abs ? sg.offset / stride : 0;
            }
        }
        sg.idxOffset = static_cast<uint32_t>(up.idx.size());
        up.idx.resize(up.idx.size() + n);
        if (weld) {
            // Indices relative to the primitive's first vertex, then through the slot of each of its vertices.
            sg.vtxFirst = base;
            uint16_t* const ix = up.idx.data() + sg.idxOffset;
            PrimIndices(sg.prim, sg.count, 0, ix);
            slotOf.resize(sg.count);
            const uint8_t* v = e.verts.data() + sg.offset;
            for (uint32_t i = 0; i < sg.count; ++i, v += segStride) slotOf[i] = static_cast<uint16_t>(base + welder.Find(up.vtx, block, v, segStride));
            for (uint32_t i = 0; i < n; ++i) ix[i] = slotOf[ix[i]];
            runRaw += sg.count;
            weldedVerts += (up.vtx.size() - block) / segStride - r->vtxCount;
            r->vtxCount = static_cast<uint32_t>((up.vtx.size() - block) / segStride);
        } else {
            // Indices count from the buffer's start (absolute) or from the run's first vertex.
            sg.vtxFirst = abs ? sg.offset / stride : (sg.offset - r->vtxByteOffset) / segStride;
            PrimIndices(sg.prim, sg.count, sg.vtxFirst, up.idx.data() + sg.idxOffset);
            r->vtxCount += sg.count;
        }
        r->idxCount += n;
        ++r->prims;
        if (sg.posMtxMax > r->posMtxMax) r->posMtxMax = sg.posMtxMax;
    }
    if (weld) {
        g_ctr.weldIn += e.totalVerts;
        g_ctr.weldOut += weldedVerts;
        ++g_ctr.weldLists;
        if (up.vtx.size() * 4 < up.vtx.capacity() * 3) up.vtx.shrink_to_fit();
        if ((g_ctr.weldLists & 255u) == 1u)
            VGX_LOG("dl weld: %llu lists, vertices %llu -> %llu (%.1f%%)\n", (unsigned long long)g_ctr.weldLists, (unsigned long long)g_ctr.weldIn,
                    (unsigned long long)g_ctr.weldOut, 100.0 * (double)g_ctr.weldOut / (double)(g_ctr.weldIn ? g_ctr.weldIn : 1));
    }
    e.bufBytes = static_cast<uint32_t>((weld ? up.vtx.size() : e.verts.size()) + up.idx.size() * 2);
    g_ctr.liveBytes -= e.verts.capacity();
    if (g_keepCopy) {
        up.vtx = e.verts;
        e.idx = up.idx;
        g_ctr.liveBytes += e.verts.capacity();
    } else if (e.copyable) {
        if (!weld) up.vtx = e.verts; // (GXV_RIGIDMERGE_DLLEARN: the entry keeps its vertices too, for the copies into the arena; welded: the buffer has its own)
        g_ctr.liveBytes += e.verts.capacity();
    } else if (weld) {
        e.verts = std::vector<uint8_t>(); // (the welded buffer is up.vtx; this entry is not copied from)
    } else {
        up.vtx = std::move(e.verts);
        e.verts = std::vector<uint8_t>();
    }
    g_ctr.staticBytes += e.bufBytes;
    ++g_ctr.promoted;
    ++g_ctr.staticBufs;
    stats::Frame& fr = stats::Cur();
    ++fr.bufUploads;
    fr.bufUploadBytes += e.bufBytes;
    CmdBuf& buf = Pending();
    buf.bufUploads.push_back(std::move(up));
    buf.cmds.push_back(Cmd{CMD_BUF_UPLOAD, static_cast<uint32_t>(buf.bufUploads.size() - 1)});
}

void InitSlow();
// (the test inline, the reading out of line: CallList and CallListBody ask for every list, ~3,000 calls a picture)
inline __attribute__((always_inline)) void Init() {
    if (__builtin_expect(g_mode >= 0, 1)) return;
    InitSlow();
}
__attribute__((noinline)) void InitSlow() {
    const char* m = std::getenv("GXV_DLCACHE");
    if (m && !std::strcmp(m, "verify")) g_mode = 2;
    else g_mode = (m && *m == '0') ? 0 : 1;
    g_static = EnvInt("GXV_STATICBUF", 1) != 0;
    // Default off: reject cached decodes with incompletely readable indexed data.
    g_boundsGuard = EnvInt("GXV_DLCACHE_BOUNDS_GUARD", 0) != 0;
    g_keepCopy = diag::Int("GXV_HASH", 0) == 2;
    // GXV_RIGIDMERGE_DLLEARN_MB (read once, default 6): the most the kept copies of the learned lists may take (megabytes).
    g_learnMax = g_static ? RigidMergeLearnMax() : 0u;
    g_keptBudget = static_cast<uint64_t>(EnvInt("GXV_RIGIDMERGE_DLLEARN_MB", 6)) << 20;
    // GXV_RIGIDMERGE_DLLEARN_GAIN (read once, default 20000): a list is copied while (how often its uses joined) x this is at least the bytes the copy moves:
    // the bytes one saved GL draw pays for (about 20 us of the console's CPU against a copy at about 1 GB/s). A very large value: any list that joins is copied.
    g_joinGain = static_cast<uint64_t>(EnvInt("GXV_RIGIDMERGE_DLLEARN_GAIN", 20000));
    if (g_learnMax) g_ctx.assign(kCtxSlots, CtxSlot());
}

// Walks a list that consists of draw commands (and padding) only. Fills
// e.segs / e.verts / spans; false if the list holds anything else.
bool Build(Entry& e, const uint8_t* p, uint32_t nbytes) {
    ++e.rpStamp; // (GXV_DRAWREPLAY: records of the entry as it was are void)
    const uint8_t* const list = p;
    const uint8_t* const end = p + nbytes;
    ReleaseBuffer(e);
    g_keptBytes -= e.keptBytes;
    if (e.copyable) --g_lc.keptLists;
    e.keptBytes = 0;
    e.copyable = false;
    e.rigidCopy = false;
    e.serial = ++g_entrySerial; // (a rebuilt list is a new one to the table of uses)
    e.segs.clear();
    e.verts.clear();
    e.idx.clear();
    e.totalVerts = 0;
    e.promotable = true;
    e.fmtMask = 0;
    e.idxMask = 0;
    e.genList = GuestWriteGeneration(list, nbytes);
    if (e.genList == kNotGuest) return false;
    // If a new generation was observed, its source write must precede the hash and decode.
    std::atomic_thread_fence(std::memory_order_acquire);
    e.hash = XXH3_64bits(list, nbytes);
    uint64_t vertexBytes = 0;
    const Plan* plans[8] = {};
    for (int i = 0; i < 12; ++i) { e.arr[i].lo = 0xFFFFFFFFu; e.arr[i].hi = 0; }
    while (p < end) {
        const uint8_t cmd = *p++;
        if (cmd == 0x00) continue;
        if (!(cmd & 0x80)) return false; // register loads, nested lists: not a pure geometry list
        if (end - p < 2) return false;
        const uint32_t count = be16(p);
        p += 2;
        const int fmt = cmd & 7;
        if (!plans[fmt]) plans[fmt] = &CurrentPlan(fmt);
        const Plan& plan = *plans[fmt];
        if (plan.streamSize == 0) return false;
        const uint64_t used = static_cast<uint64_t>(count) * plan.streamSize;
        if (used > static_cast<uint64_t>(end - p)) return false; // truncated: let the general parser deal with it
        if (count) {
            Seg s;
            s.prim = cmd & 0xF8;
            s.fmt = static_cast<uint8_t>(fmt);
            s.stride = plan.layout.stride;
            s.layoutMask = plan.layoutMask;
            s.count = count;
            s.offset = static_cast<uint32_t>(vertexBytes);
            s.idxOffset = static_cast<uint32_t>(p - list);
            s.vtxFirst = 0;
            if (count > 0xFFFF) e.promotable = false;
            vertexBytes += static_cast<size_t>(count) * plan.layout.stride;
            if (vertexBytes > 0xFFFFFFFFu) return false;
            bool complete = true;
            const ArraySpans sp = MeasureArraySpans(plan, p, count, g_boundsGuard ? &complete : nullptr);
            // Fallback-zero elements are absent from the spans, so they cannot safely define a cache entry.
            if (g_boundsGuard && !complete) return false;
            for (int a = 9; a <= 20; ++a)
                if (sp.mask & (1u << a)) {
                    e.idxMask |= 1u << a;
                    if (sp.lo[a - 9] < e.arr[a - 9].lo) e.arr[a - 9].lo = sp.lo[a - 9];
                    if (sp.hi[a - 9] > e.arr[a - 9].hi) e.arr[a - 9].hi = sp.hi[a - 9];
                }
            e.segs.push_back(s);
            e.totalVerts += count;
            if (!(e.fmtMask & (1u << fmt))) {
                e.fmtMask |= static_cast<uint8_t>(1u << fmt);
                std::memcpy(e.vat[fmt], &g.vat[fmt][9], sizeof e.vat[fmt]);
            }
        }
        p += used;
    }
    for (int a = 9; a <= 20; ++a)
        if (e.idxMask & (1u << a)) {
            const int i = a - 9;
            e.arr[i].p = g.arrays[a].data;
            e.arr[i].stride = g.arrays[a].stride;
            e.arr[i].gen = GuestWriteGeneration(e.arr[i].p + e.arr[i].lo, e.arr[i].hi - e.arr[i].lo);
            if (e.arr[i].gen == kNotGuest) return false;
        }
    std::atomic_thread_fence(std::memory_order_acquire);
    for (int a = 9; a <= 20; ++a)
        if (e.idxMask & (1u << a)) {
            const int i = a - 9;
            e.hash = XXH3_64bits_withSeed(e.arr[i].p + e.arr[i].lo, e.arr[i].hi - e.arr[i].lo, e.hash);
        }
    // The range pass already visited the indices. Decode once, into one allocation,
    // with the generations and hash describing bytes no newer than this decode.
    e.verts.resize(static_cast<size_t>(vertexBytes));
    e.rowBoxes.clear();
    for (Seg& s : e.segs) {
        DecodeVertices(*plans[s.fmt], list + s.idxOffset, s.count, e.verts.data() + s.offset, nullptr);
        s.posMtxMax = LastDecodedPosMtxMax();
        if (CharaAAMode() || EdgeAAMode() || StaticCullEnabled()) RowBoxesOf(e.verts.data() + s.offset, s.count, s.stride, e.rowBoxes); // (GXV_CHARAA, GXV_EDGE_AA's HUD bounds, GXV_STATICCULL)
    }
    // A list whose vertices all fit the CPU-transform path (a particle's quad, a glyph) stays
    // in the frame's arena, where draws that differ only in their matrix merge into one record;
    // anything larger becomes a static buffer the second time it is drawn unchanged.
    if (e.totalVerts <= SoftXformLimit()) e.promotable = false;
    // (GXV_RIGIDMERGE_DLMAX: a rigid list may stay in the arena, to join a palette run with the draws around it)
    if (!g.vcd[0] && e.totalVerts > SoftXformLimit() && e.totalVerts <= RigidMergeListMax()) e.rigidCopy = true;
    if (!g.vcd[0] && e.totalVerts <= RigidMergeListMax()) e.promotable = false;
    // (GXV_RIGIDMERGE_DLLEARN: a list that will be a static buffer keeps its vertices as well, within a budget)
    if (g_learnMax && !g.vcd[0] && e.promotable && e.totalVerts > SoftXformLimit() && e.totalVerts <= g_learnMax && !e.segs.empty() &&
        g_keptBytes + vertexBytes <= g_keptBudget) {
        e.copyable = true;
        e.keptBytes = static_cast<uint32_t>(vertexBytes);
        g_keptBytes += vertexBytes;
        ++g_lc.keptLists;
    }
    return !e.segs.empty();
}

uint64_t ContentHash(const Entry& e) {
    uint64_t h = XXH3_64bits(e.list, e.nbytes);
    for (int a = 9; a <= 20; ++a)
        if (e.idxMask & (1u << a)) h = XXH3_64bits_withSeed(e.arr[a - 9].p + e.arr[a - 9].lo, e.arr[a - 9].hi - e.arr[a - 9].lo, h);
    return h;
}

// Reads the generations of everything the entry depends on; false if some of
// it is not tracked guest RAM.
bool ReadGenerations(const Entry& e, uint64_t& genList, uint64_t genArr[12]) {
    genList = GuestWriteGeneration(e.list, e.nbytes);
    bool ok = genList != kNotGuest;
    for (uint32_t m = (e.idxMask >> 9) & 0xFFFu; m; m &= m - 1) { // (the indexed attributes only)
        const int i = __builtin_ctz(m);
        genArr[i] = GuestWriteGeneration(e.arr[i].p + e.arr[i].lo, e.arr[i].hi - e.arr[i].lo);
        ok = ok && genArr[i] != kNotGuest;
    }
    return ok;
}

bool SameState(const Entry& e) {
    if (!SameWords(e.vcd, g.vcd, 5) || e.vcd[20] != g.vcd[20]) return false;
    for (int f = 0; f < 8; ++f)
        if ((e.fmtMask & (1u << f)) && !SameWords(e.vat[f], &g.vat[f][9], static_cast<uint32_t>(sizeof e.vat[f] / 4))) return false;
    for (uint32_t m = (e.idxMask >> 9) & 0xFFFu; m; m &= m - 1) { // (the indexed attributes only)
        const int i = __builtin_ctz(m);
        const ArrayRef& ar = g.arrays[9 + i];
        if (e.arr[i].p != ar.data || e.arr[i].stride != ar.stride || e.arr[i].hi > ar.size) return false;
    }
    return true;
}

// What a use of a learned list is looked up by: the shaders and the render state of the draw before it (DlLastJobKey) and, when that draw was the previous learned
// list's, which list: so that a list at the head of a run and the same list in the middle of one (after itself) are two uses.
uint32_t g_prevSerial = 0, g_prevEndJob = ~0u;
inline uint32_t UseKey(const Entry&) {
    // (and which registers the state is built from were written since the last draw: a list that starts with another material than the draw before it
    // cannot join it, one that starts with the same can)
    const uint32_t dirty = (g.bpKeyDirty ? 1u : 0u) | (g.bpRsDirty ? 2u : 0u) | (g.xfKeyDirty ? 4u : 0u) | (g.xfRegDirty ? 8u : 0u) | (g.texDirty ? 16u : 0u);
    return DlLastJobKey() * 0x9E3779B1u + (g_prevEndJob == DlJobSeq() ? g_prevSerial * 0x85EBCA6Bu : 0u) + dirty * 0x27D4EB2Fu;
}

// Whether this use of a learned list copies its vertices into the arena: where the list, after a draw of this shader key, joined a run at its last use
// (the test of the static uses counts), unless its last copy joined nothing and is still being waited out; and once in 64 frames, staggered by list.
bool WantCopy(const Entry& e) {
    if (e.verts.empty()) return false;
    const uint32_t frame = static_cast<uint32_t>(CurrentFrame());
    const uint32_t key = UseKey(e);
    const bool probe = ((frame + e.serial) & 63u) == 0;
    if (const CtxSlot* c = CtxFind(e.serial, key, false)) {
        if (static_cast<int32_t>(frame - c->retryAt) < 0) return false;
        return probe || static_cast<uint64_t>(c->rate) * g_joinGain >= static_cast<uint64_t>(e.keptBytes) * 255u;
    }
    return probe;
}

void Emit(Entry& e, uint32_t nbytes) {
    stats::Frame& fr = stats::Cur();
    ++fr.streams;
    fr.streamBytes += nbytes;
    const bool learn = e.copyable && g_learnMax != 0;
    const bool copy = !e.bufId || (learn && WantCopy(e));
    const uint32_t j0 = DlJobSeq(), key = learn ? UseKey(e) : 0u;
    if (!copy) {
        const bool abs = AbsoluteIndices();
        if (learn) SetPendingStaticCopyable(true); // (the record stage tests whether a copy would have joined)
        for (const Run& r : e.runs) {
            SetPendingPosMtxMax(r.posMtxMax);
            if (!e.rowBoxes.empty()) SetPendingBounds(e.rowBoxes.data(), static_cast<uint32_t>(e.rowBoxes.size()));
            EmitStaticRun(r.primKind, r.layoutMask, r.vtxCount, r.idxCount, r.prims, e.bufId, abs ? 0 : r.vtxByteOffset, r.idxOffset, r.vtxFirst);
            SetPendingBounds(nullptr, 0);
            fr.primsDL += r.prims;
            fr.vertsDL += r.vtxCount;
        }
        if (learn) SetPendingStaticCopyable(false);
    } else {
        uint64_t bytes = 0;
        // (GXV_SPLIT_COPY: the finishing thread makes the copies, from e.verts, which stay until Settle(e.ticket); EmitDraw copies a
        // segment itself where the player reads its vertices)
        const bool deferred = split::CopiesVertices();
        for (const Seg& s : e.segs) {
            uint8_t* out = BeginVertices(s.layoutMask, s.count);
            if (!out) continue; // (BeginVertices' guard refused the arena: this segment is not drawn)
            const uint8_t* const src = e.verts.data() + s.offset;
            if (!deferred) std::memcpy(out, src, static_cast<size_t>(s.count) * s.stride);
            bytes += static_cast<uint64_t>(s.count) * s.stride;
            SetPendingPosMtxMax(s.posMtxMax);
            // (GXV_CHARAA: bounds from the vertices themselves, few by construction)
            EmitDraw(s.prim, s.layoutMask, s.count, 0xFFFFFFFFu, deferred ? src : nullptr);
            ++fr.primsDL;
            fr.vertsDL += s.count;
        }
        if (deferred) e.ticket = split::Issued();
        if (learn || e.rigidCopy) {
            ++g_lc.copies;
            g_lc.copyBytes += bytes;
        }
    }
    if (learn) {
        g_prevSerial = e.serial;
        g_prevEndJob = DlJobSeq();
    }
    if (learn && DlJobSeq() != j0) g_uses.push_back(Use{&e, j0, DlJobSeq(), key, static_cast<uint8_t>(!e.bufId ? 0 : (copy ? 2 : 1))});
}

void Free(Entry* e) {
    split::Settle(e->ticket); // (GXV_SPLIT_COPY)
    RpBump();
    for (Use& u : g_uses)
        if (u.e == e) u.e = nullptr;
    g_keptBytes -= e->keptBytes;
    if (e->copyable) --g_lc.keptLists;
    ReleaseBuffer(*e);
    g_ctr.liveBytes -= e->verts.capacity();
    delete e;
}

// Drops entries that were not used for a long time (called once in a while).
void Sweep(uint64_t frame, uint64_t age) {
    bool erasedAny = false;
    g_map.EraseIf([&](const uint8_t*, ListInfo& li) {
        Entry** link = &li.head;
        while (*link) {
            Entry* e = *link;
            if (age - e->lastUse > kRetainFrames) {
                *link = e->next;
                Free(e);
                ++g_ctr.evicted;
            } else {
                link = &e->next;
            }
        }
        if (li.head) return false;
        erasedAny = true;
        return true;
    });
    if (erasedAny) MapFrontClear();
    g_bypass.EraseIf([&](const uint8_t*, Bypass& b) { return b.until + kRetainFrames < frame; });
}

void FreeAll(ListInfo& li) {
    while (li.head) {
        Entry* e = li.head;
        li.head = e->next;
        Free(e);
        ++g_ctr.evicted;
    }
}
void Unlink(ListInfo& li, Entry* e) {
    Entry** link = &li.head;
    while (*link != e) link = &(*link)->next;
    *link = e->next;
    Free(e);
}

} // namespace

// The end of a frame: what the uses of the learned lists came to (see "learned copies" above). `log` has a byte per job of the frame.
void DlLearnFrameEnd(const std::vector<uint8_t>& log, uint64_t frame) {
    for (const Use& u : g_uses) {
        Entry* e = u.e;
        if (!e || u.j0 >= log.size()) continue;
        const bool joined = log[u.j0] != 0 || (u.j1 < log.size() && log[u.j1] != 0);
        CtxSlot* c = CtxFind(e->serial, u.key, true);
        if (!c) continue;
        const uint32_t now = static_cast<uint32_t>(frame);
        const bool blocked = c->used && static_cast<int32_t>(now - c->retryAt) < 0;
        if (!c->used) {
            c->used = true;
            c->rate = joined ? 255 : 0;
        } else if (u.kind == 2 || !blocked) {
            c->rate = static_cast<uint8_t>((c->rate * 3u + (joined ? 255u : 0u)) / 4u);
        }
        if (u.kind == 2) {
            if (joined) {
                ++g_lc.copyJoined;
                c->failLevel = 0;
            } else {
                ++g_lc.copyWasted;
                // (a copy that bought nothing and is not paying for itself: not again for a while)
                if (static_cast<uint64_t>(c->rate) * g_joinGain < static_cast<uint64_t>(e->keptBytes) * 255u || c->rate < 128) {
                    c->retryAt = static_cast<uint32_t>(now + (64u << c->failLevel));
                    if (c->failLevel < 3) ++c->failLevel;
                }
            }
        } else if (u.kind == 1) {
            ++g_lc.staticUses;
            if (joined) ++g_lc.staticCouldJoin;
        }
    }
    g_uses.clear();
    g_prevSerial = 0;
    g_prevEndJob = ~0u;
}

void DlLearnCounters(uint64_t out[8]) {
    out[0] = g_lc.copies;
    out[1] = g_lc.copyBytes;
    out[2] = g_lc.copyJoined;
    out[3] = g_lc.copyWasted;
    out[4] = g_lc.staticUses;
    out[5] = g_lc.staticCouldJoin;
    out[6] = g_keptBytes;
    out[7] = g_lc.keptLists;
}

bool StaticBufferData(uint32_t bufId, const uint8_t*& vtx, const uint16_t*& idx) {
    if (bufId >= g_bufOwner.size() || !g_bufOwner[bufId] || g_bufOwner[bufId]->verts.empty() || g_bufOwner[bufId]->idx.empty()) return false; // (no copy: or only the learned lists' vertices)
    vtx = g_bufOwner[bufId]->verts.data();
    idx = g_bufOwner[bufId]->idx.data();
    return true;
}

uint64_t DlCacheLiveBytes() { return g_ctr.liveBytes; }
// (GXV_FE_SPLIT: the branch counters, for the per-picture line of vgx_workload.h)
void DlCacheBranchCounts(uint64_t out[7]) {
    out[0] = g_ctr.hits; out[1] = g_ctr.rehashOk; out[2] = g_ctr.rehashChanged; out[3] = g_ctr.bypassed;
    out[4] = g_ctr.builds; out[5] = g_ctr.uncacheable; out[6] = g_ctr.promoted;
}
// (GXV_FE_SPLIT: the "list-other why" counters)
void DlOtherWhyCounts(uint64_t out[8]) { for (unsigned i = 0; i < OwCount; ++i) out[i] = g_otherWhy[i]; }
uint64_t DlCacheStaticBytes() { return g_ctr.staticBytes; }

void DlCacheReport(FILE* f) {
    std::fprintf(f,
                 "display-list cache: %llu hits (%llu verts), %llu builds (%llu verts), %llu re-validated by hash, %llu changed, %llu bypassed calls, "
                 "%llu uncacheable, %llu evicted, %llu changed-without-notification (verify mode), %.1f MB of decoded vertices live in the front end, "
                 "%llu promotions to static buffers, %llu static buffers / %.1f MB live in the back end\n",
                 (unsigned long long)g_ctr.hits, (unsigned long long)g_ctr.hitVerts, (unsigned long long)g_ctr.builds,
                 (unsigned long long)g_ctr.builtVerts, (unsigned long long)g_ctr.rehashOk, (unsigned long long)g_ctr.rehashChanged,
                 (unsigned long long)g_ctr.bypassed, (unsigned long long)g_ctr.uncacheable, (unsigned long long)g_ctr.evicted,
                 (unsigned long long)g_ctr.staleNoNotify, g_ctr.liveBytes / 1048576.0, (unsigned long long)g_ctr.promoted,
                 (unsigned long long)g_ctr.staticBufs, g_ctr.staticBytes / 1048576.0);
    if (g_ctr.weldLists)
        std::fprintf(f, "GXV_DLWELD: %llu promoted lists welded, vertices %llu -> %llu (%.1f%%)\n", (unsigned long long)g_ctr.weldLists,
                     (unsigned long long)g_ctr.weldIn, (unsigned long long)g_ctr.weldOut, 100.0 * (double)g_ctr.weldOut / (double)(g_ctr.weldIn ? g_ctr.weldIn : 1));
    if (g_learnMax)
        std::fprintf(f,
                     "learned copies (up to %u vertices): %llu copies (%.1f MB), %llu joined, %llu did not; %llu static uses, %llu of them could have joined; "
                     "%.1f MB of vertices kept for %llu lists\n",
                     g_learnMax, (unsigned long long)g_lc.copies, g_lc.copyBytes / 1048576.0, (unsigned long long)g_lc.copyJoined,
                     (unsigned long long)g_lc.copyWasted, (unsigned long long)g_lc.staticUses, (unsigned long long)g_lc.staticCouldJoin,
                     g_keptBytes / 1048576.0, (unsigned long long)g_lc.keptLists);
}

// (GXV_FE_PREFETCH stage 3, GXV_DRAWREPLAY=2) what SameState, ReadGenerations and Emit read of an entry: the head of the entry, the indexed
// attributes' array uses, and the first vertex format's row. Hints only (an entry the caller holds may be read: its fields are).
static void RpHintEntry(const Entry* e) {
    FePrefetchLines(e, 128, 4); // (the head, up to the array uses: about 100 bytes on the console)
    for (uint32_t m = (e->idxMask >> 9) & 0xFFFu; m; m &= m - 1) FePrefetch(&e->arr[__builtin_ctz(m)]);
    if (e->fmtMask) FePrefetchLines(&e->vat[__builtin_ctz(e->fmtMask)][0], sizeof e->vat[0], 2);
}
// (GXV_DRAWREPLAY=2) the same without reading the entry (a hint that waited for the entry would cost what it saves): its head, the
// positions' array use and the first vertex format's row.
static void RpHintHead(const Entry* e) {
    FePrefetchLines(e, 128, 4); // (the head, up to the array uses: about 100 bytes on the console)
    FePrefetchLines(&e->arr[0], sizeof e->arr[0], 2);
    FePrefetchLines(&e->vat[0][0], sizeof e->vat[0], 2);
}
static uint32_t TinyListBytes() {
    const uint32_t n = static_cast<uint32_t>(VGX_KNOB(static_cast<uint32_t>(EnvInt("GXV_DL_TINY", 64))));
    return n < 15 ? 15 : n;
}
// GXV_FE_PREFETCH (see "look-ahead" before ProcessStream in vgx_state.cpp): what CallList will read for `list`, hinted in stages
// as the call comes nearer. A tiny list is parsed from its bytes (or replayed: state list); a larger one is first tried as a state
// list (StateListPrefetch), then goes through the bypass map, the front, the list's bookkeeping and its first variant's entry,
// whose generation counters and runs the hit reads. Only the front and the entries it points at are read to get there (an entry
// of the front is valid while the front holds it: MapFrontClear empties it whenever one is erased).
void CallListPrefetch(const uint8_t* list, uint32_t nbytes, int stage) {
    Init();
    if (g_mode == 0 || nbytes <= TinyListBytes()) {
        if (stage == 1) FePrefetchLines(list, nbytes, 4);
        if (g_mode != 0) StateListPrefetch(list, nbytes, stage); // (padded tiny material lists: StateListCall)
        return;
    }
    StateListPrefetch(list, nbytes, stage);
    const MapFront& mf = g_mapFront[(reinterpret_cast<uintptr_t>(list) >> 5) & 255u];
    if (stage == 1) {
        FePrefetch(&mf);
        if (!g_bypass.empty()) FePrefetch(g_bypass.HomeSlot(list));
        return;
    }
    if (!(mf.list == list && mf.li)) return;
    if (stage == 2) {
        FePrefetch(mf.li);
        return;
    }
    const Entry* e = mf.li->head;
    if (!e) return;
    if (stage == 3) {
        // (what SameState, ReadGenerations and Emit read first: identity, the first vertex format's row, the indexed arrays' bindings,
        // spans and generations, the content vectors and the static buffer)
        RpHintEntry(e);
        return;
    }
    if (e->nbytes != nbytes) return;
    GuestWriteGenerationPrefetch(list, nbytes);
    for (uint32_t m = (e->idxMask >> 9) & 0xFFFu; m; m &= m - 1) {
        const int i = __builtin_ctz(m);
        if (e->arr[i].p && e->arr[i].hi > e->arr[i].lo) GuestWriteGenerationPrefetch(e->arr[i].p + e->arr[i].lo, e->arr[i].hi - e->arr[i].lo);
    }
    FePrefetchLines(e->runs.data(), static_cast<uint32_t>(e->runs.size() * sizeof(Run)), 4);
    FePrefetchLines(e->rowBoxes.data(), static_cast<uint32_t>(e->rowBoxes.size() * sizeof(RowBox)), 2);
}
// ------------------------------------------------------------------------------------------------ draw replay (GXV_DRAWREPLAY)
// A picture calls nearly the same lists in nearly the same order as the one before: the same models, shapes and materials, with new
// matrices. Each call of a list larger than a tiny one took the same way to its cache entry: the state-list front and table (a
// geometry list is looked up there first), the guest-RAM test, the bypass map, the map's front, the list's bookkeeping and its
// chain of variants; about a dozen pointer-chasing loads into tables the game's turn has pushed out of the caches, before the hit
// itself (the vertex state, the generation counters, the runs).
// Here every such call of a picture is written down in call order (the "trace": the list, its size, and the entry it was a hit of).
// The next picture walks the previous trace with a cursor: a call that is the next one of the trace (or one of the next eight, or,
// after objects came or went, wherever the list was last called) goes straight to that entry, and only the hit's own tests are made:
// the vertex state (SameState), the generations of the list and its arrays (ReadGenerations, unchanged), then the hit branch as it
// is (the age, the sweep, the counters, the promotion test, Emit). What the skipped lookups would have found is guaranteed by two
// marks: the record was made by a hit (or build) of the entry while it was the first variant of its list and not raced; the entry's
// rpStamp moves when it is built again or another variant is put before it, and g_rpEpoch moves when any entry is freed (a list is put
// out of caching, bypass, only together with freeing its entry). With both unchanged the list has the same first variant and no
// bypass, so the normal path's walk would stop at the entry exactly when SameState holds. A list the entry's generations say is
// unchanged holds the entry's draw commands, so the state-list fast hit, which the normal path tries first, would have declined it
// (a state-list entry of the same address was parsed from other bytes: state commands). Any doubt (no record, a mark moved, another
// vertex state, a moved generation, the verify mode) takes the normal path, which also makes the new record. The output is the normal path's.
// The trace's own cost: ~16 bytes appended per call, the compare at the cursor, and once a picture the swap of two vectors.
//   GXV_DRAWREPLAY=0  off;  1 (default) on;  2 on, and the next record's entry is hinted to the cache (PLD) as a call is replayed
namespace {
struct RpRec {
    const uint8_t* list;
    Entry* e;        // the entry the call was a hit (or build) of; null: the call went another way (state list, bypass, ...)
    uint32_t nbytes;
    uint32_t stamp;  // the entry's rpStamp when it was recorded
    uint32_t epoch;  // g_rpEpoch when it was recorded
};
std::vector<RpRec> g_rpOld, g_rpNew; // the previous picture's trace, this picture's so far
uint32_t g_rpCur = 0;                // the cursor in g_rpOld
uint64_t g_rpFrame = ~0ull;
constexpr uint32_t kRpWindow = 8, kRpMapBits = 12;
uint32_t g_rpMap[1u << kRpMapBits];  // list -> 1 + the index of its last call in g_rpOld (0: none); made at the first need of a picture
bool g_rpMapMade = false;
struct RpCounters { uint64_t hits = 0, misses = 0, realigned = 0, recorded = 0, calls = 0; } g_rp;
// (the call in progress: the entry the normal path ended at, for the record)
Entry* g_rpOutE = nullptr;
uint64_t g_lastSweep = 0; // (CallListBody's sweep schedule)
inline int RpMode() {
    const int m = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_DRAWREPLAY", 1))));
    return m;
}
inline uint32_t RpSlot(const uint8_t* list) {
    return (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(list) >> 5) * 2654435761u) >> (32 - kRpMapBits);
}
inline void RpFrame() {
    const uint64_t frame = CurrentFrame();
    if (frame == g_rpFrame) return;
    g_rpFrame = frame;
    g_rpOld.swap(g_rpNew);
    g_rpNew.clear();
    g_rpCur = 0;
    g_rpMapMade = false;
}
// The record of the previous trace this call repeats, or null. The cursor moves past it.
inline const RpRec* RpFind(const uint8_t* list, uint32_t nbytes) {
    const uint32_t n = static_cast<uint32_t>(g_rpOld.size());
    const uint32_t end = g_rpCur + kRpWindow < n ? g_rpCur + kRpWindow : n;
    for (uint32_t i = g_rpCur; i < end; ++i) {
        const RpRec& r = g_rpOld[i];
        if (r.list == list && r.nbytes == nbytes) {
            if (i != g_rpCur) ++g_rp.realigned;
            g_rpCur = i + 1;
            return &r;
        }
    }
    if (!n) return nullptr;
    if (!g_rpMapMade) {
        g_rpMapMade = true;
        std::memset(g_rpMap, 0, sizeof g_rpMap);
        for (uint32_t i = 0; i < n; ++i) g_rpMap[RpSlot(g_rpOld[i].list)] = i + 1;
    }
    const uint32_t k = g_rpMap[RpSlot(list)];
    if (k && g_rpOld[k - 1].list == list && g_rpOld[k - 1].nbytes == nbytes) {
        ++g_rp.realigned;
        g_rpCur = k;
        return &g_rpOld[k - 1];
    }
    return nullptr;
}
// The age and the sweep, as CallListBody has them before its lookup.
inline uint64_t AgeAndSweep(uint64_t frame) {
    const uint64_t age = AgeOf(frame);
    if (age - g_lastSweep >= 120) { g_lastSweep = age; Sweep(frame, age); }
    return age;
}
// The replay of a recorded hit: true when it was one (and is done), false when the normal path must run (nothing was changed but
// the age and the sweep, which the normal path does as the first thing after its lookups anyway, idempotently).
bool RpReplay(const RpRec& r, uint32_t nbytes, workload::FeScope& cls) {
    Entry& e = *r.e;
    if (FeCutHint()) {
        // (GXV_FECUT_HINT, vgx_state.h: what the hit reads after its tests, from the entry's head: the runs Emit walks, the bounds the
        // static cull and CharaNote read (separate blocks), and the list's generation counter, which ReadGenerations reads first)
        FePrefetchLines(e.runs.data(), static_cast<uint32_t>(e.runs.size() * sizeof(Run)), 4);
        FePrefetchLines(e.rowBoxes.data(), static_cast<uint32_t>(e.rowBoxes.size() * sizeof(RowBox)), 2);
        GuestWriteGenerationPrefetch(r.list, nbytes);
    }
    SyncVertexArrays(); // (as CallListBody before its lookup: SameState reads g.arrays)
    if (!SameState(e)) return false;
    uint64_t gl, ga[12] = {};
    if (!ReadGenerations(e, gl, ga)) return false;
    // (no acquire fence here with GXV_FE_KNOBS=1: a replayed hit reads no guest bytes after the generations, only the entry's own
    // decode; a miss goes the normal way, which reads the generations again and fences before it hashes or decodes guest memory)
    knob::HitFence();
    if (gl != e.genList) return false;
    for (uint32_t m = (e.idxMask >> 9) & 0xFFFu; m; m &= m - 1)
        if (ga[__builtin_ctz(m)] != e.arr[__builtin_ctz(m)].gen) return false;
    const uint64_t frame = CurrentFrame();
    const uint64_t age = AgeAndSweep(frame);
    if (r.epoch != g_rpEpoch) return false; // (the sweep freed something: the normal path decides)
    // ---- CallListBody's hit branch
    const bool afterChange = e.fails != 0;
    e.lastUse = age;
    e.fails = 0;
    ++g_ctr.hits;
    ++stats::Cur().dlHit;
    g_ctr.hitVerts += e.totalVerts;
    if (!afterChange && !e.bufId && e.promotable && g_static) Promote(e);
    if (cls.slot >= 0) cls.slot = workload::FeGeomHit;
    (void)nbytes;
    Emit(e, nbytes);
    return true;
}
} // namespace

void DrawReplayCounters(uint64_t out[4]) { out[0] = g_rp.hits; out[1] = g_rp.misses; out[2] = g_rp.realigned; out[3] = g_rp.recorded; }
void DrawReplayCounters5(uint64_t out[5]) { out[0] = g_rp.hits; out[1] = g_rp.misses; out[2] = g_rp.realigned; out[3] = g_rp.recorded; out[4] = g_rp.calls; }

static void CallListBody(const uint8_t* list, uint32_t nbytes, workload::FeScope& cls);
// (GXV_FXMERGE, vgx_state.cpp FxCall) whether CallList(list, nbytes) goes straight to ProcessStream(list, nbytes) after its Init and
// SyncVertexArrays: the tiny-list branch of CallListBody, for a list that is not a padded state list. The tests are CallList's and
// CallListBody's own, in their order: CallList takes CallListBody whenever the tiny branch's test holds.
bool CallListIsTinyStream(const uint8_t* list, uint32_t nbytes) {
    Init();
    if (!(g_mode == 0 || nbytes <= TinyListBytes() || TraceFrame())) return false;
    return !(g_mode != 0 && (nbytes == 32 || nbytes == 64) && (reinterpret_cast<uintptr_t>(list) & 31u) == 0 && list[0] == 0x61 && !TraceFrame());
}
void CallList(const uint8_t* list, uint32_t nbytes) {
    // (GXV_FE_SPLIT: the branch taken decides the slot; FeOther until one is known)
    workload::FeScope cls(workload::FeOther);
    Init();
    if (RpMode() == 0 || g_mode != 1 || nbytes <= TinyListBytes() || TraceFrame()) {
        CallListBody(list, nbytes, cls);
        return;
    }
    // ---- draw replay
    RpFrame();
    ++g_rp.calls;
    if (const RpRec* r = RpFind(list, nbytes)) {
        if (r->e && r->epoch == g_rpEpoch && r->e->rpStamp == r->stamp) {
            const RpRec rec = *r;
            if (RpMode() == 2 && g_rpCur < g_rpOld.size() && g_rpOld[g_rpCur].e && g_rpOld[g_rpCur].epoch == g_rpEpoch) RpHintHead(g_rpOld[g_rpCur].e);
            if (RpReplay(rec, nbytes, cls)) {
                ++g_rp.hits;
                g_rpNew.push_back(rec);
                return;
            }
            ++g_rp.misses;
        }
    }
    g_rpOutE = nullptr;
    CallListBody(list, nbytes, cls);
    // (a hit or build of the entry that is the first variant of its list, not raced: see the note above)
    g_rpNew.push_back(RpRec{list, g_rpOutE, nbytes, g_rpOutE ? g_rpOutE->rpStamp : 0u, g_rpEpoch});
    if (g_rpOutE) ++g_rp.recorded;
}
static void CallListBody(const uint8_t* list, uint32_t nbytes, workload::FeScope& cls) {
    Init();
    // (a list of a few dozen bytes, one particle quad for instance, is decoded faster than its
    // cache entry is checked)
    if (g_mode == 0 || nbytes <= TinyListBytes() || TraceFrame()) {
        if (cls.slot >= 0) cls.slot = workload::FeTiny;
        SyncVertexArrays();
        // Small padded material lists can reuse state ops; tiny draw lists still decode directly.
        if (g_mode != 0 && (nbytes == 32 || nbytes == 64) &&
            (reinterpret_cast<uintptr_t>(list) & 31u) == 0 && list[0] == 0x61 && !TraceFrame() &&
            StateListCall(list, nbytes)) return;
        ProcessStream(list, nbytes);
        return;
    }
    // A material list seen before and unchanged: replayed at once (the geometry cache never takes state lists).
    // (first: a hit is a list in guest RAM, its generation was read there; a replay reads no vertex array but through
    // the indexed loads, which bring their own arrays up to date)
    if (StateListFastHit(list, nbytes)) {
        if (cls.slot >= 0) cls.slot = workload::FeMatHit;
        return;
    }
    SyncVertexArrays();
    if (!IsGuestRam(list, nbytes)) {
        ++g_otherWhy[OwNotGuest];
        ProcessStream(list, nbytes);
        return;
    }
    const uint64_t frame = CurrentFrame();
    const uint64_t age = AgeOf(frame);
    if (age - g_lastSweep >= 120) { g_lastSweep = age; Sweep(frame, age); }
    if (!g_bypass.empty()) {
        const Bypass* b = g_bypass.Find(list);
        if (b) {
            if (frame < b->until) {
                ++g_ctr.bypassed;
                ++stats::Cur().dlBypass;
                if (!StateListCall(list, nbytes)) { ++g_otherWhy[OwBypassStream]; ProcessStream(list, nbytes); }  // (material lists: pre-parsed, vgx_state.cpp)
                else ++g_otherWhy[OwBypassStateReplay];
                return;
            }
            // expired: try caching again (the entry stays, it remembers the back-off level)
        }
    }
    MapFront& mf = g_mapFront[(reinterpret_cast<uintptr_t>(list) >> 5) & 255u];
    if (!(mf.li && mf.list == list)) {
        mf.li = &g_map[list];
        mf.list = list;
    }
    ListInfo& li = *mf.li;
    Entry* e = li.head;
    while (e && !(e->nbytes == nbytes && SameState(*e))) e = e->next;
    if (e && e->raced) {
        // Built while the game thread was flushing something: not trusted, build again.
    } else if (e) {
        uint64_t gl, ga[12] = {};
        bool tracked = ReadGenerations(*e, gl, ga);
        bool same = tracked && gl == e->genList;
        for (uint32_t m = (e->idxMask >> 9) & 0xFFFu; m && same; m &= m - 1) same = ga[__builtin_ctz(m)] == e->arr[__builtin_ctz(m)].gen;
        // The generations are read before any guest byte the hash (or a rebuild) reads: an acquire fence between them. A hit reads
        // none (it emits the entry's own decode), so with GXV_FE_KNOBS=1 it has no fence (a barrier on the console's processor).
        if (!same || g_mode == 2) std::atomic_thread_fence(std::memory_order_acquire);
        else knob::HitFence();
        if (same && g_mode == 2 && ContentHash(*e) != e->hash) {
            ++g_ctr.staleNoNotify;
            VGX_LOG_N(32, "dl cache: list %p (%u bytes, %u verts) changed without a write notification\n", (const void*)list, nbytes, e->totalVerts);
            same = false;
        }
        if (same) {
            const bool afterChange = e->fails != 0;
            e->lastUse = age;
            e->fails = 0;
            ++g_ctr.hits;
            ++stats::Cur().dlHit;
            g_ctr.hitVerts += e->totalVerts;
            // Used again unchanged: worth a static buffer (no per-frame copy, no index generation).
            if (!afterChange && !e->bufId && e->promotable && g_static) Promote(*e);
            if (cls.slot >= 0) cls.slot = workload::FeGeomHit;
            if (e == li.head) g_rpOutE = e; // (GXV_DRAWREPLAY: recorded)
            Emit(*e, nbytes);
            return;
        }
        if (tracked && ContentHash(*e) == e->hash) {
            // A write landed in the same 4 KiB as something this entry uses, but not on it.
            const bool afterChange = e->fails != 0;
            e->genList = gl;
            for (uint32_t m = (e->idxMask >> 9) & 0xFFFu; m; m &= m - 1) e->arr[__builtin_ctz(m)].gen = ga[__builtin_ctz(m)];
            e->lastUse = age;
            e->fails = 0;
            ++g_ctr.rehashOk;
            ++stats::Cur().dlRehash;
            g_ctr.hitVerts += e->totalVerts;
            // After a real edit, wait for another reuse instead of uploading on an alternating edit/noise cycle.
            if (!afterChange && !e->bufId && e->promotable && g_static) Promote(*e);
            if (e == li.head) g_rpOutE = e; // (GXV_DRAWREPLAY: recorded)
            Emit(*e, nbytes);
            return;
        }
        ++g_ctr.rehashChanged;
        if (++e->fails >= 3) {
            // Rewritten every time it is drawn (dynamic geometry): hashing and
            // storing it is wasted work. Decode directly for the next two seconds.
            SetBypass(g_bypass[list], frame);
            ++g_otherWhy[OwRehashBypassSet];
            Unlink(li, e);
            if (!li.head) { g_map.Erase(list); MapFrontClear(); }
            ProcessStream(list, nbytes);
            return;
        }
    } else {
        // A new variant. One every frame means the bindings never repeat.
        int n = 0;
        Entry* lru = nullptr;
        for (Entry* v = li.head; v; v = v->next) {
            ++n;
            if (!lru || v->lastUse < lru->lastUse) lru = v;
        }
        if (n) {
            if (li.lastNewVariant + 1 == frame) ++li.streak;
            else if (li.lastNewVariant != frame) li.streak = 1;
            li.lastNewVariant = frame;
            if (li.streak >= 4) {
                SetBypass(g_bypass[list], frame);
                ++g_otherWhy[OwStreakBypassSet];
                FreeAll(li);
                g_map.Erase(list);
                MapFrontClear();
                ++g_ctr.bypassed;
                ++stats::Cur().dlBypass;
                ProcessStream(list, nbytes);
                return;
            }
            if (n >= kMaxVariants) {
                Unlink(li, lru);
                ++g_ctr.evicted;
            }
        } else {
            li.lastNewVariant = frame;
            li.streak = 1;
        }
        if (li.head) ++li.head->rpStamp; // (GXV_DRAWREPLAY: no longer the first variant)
        e = new Entry();
        e->next = li.head;
        li.head = e;
    }
    // (Re)build. Snapshot each range before decoding it; the log also rejects
    // overlapping notifications completed while the build was in progress.
    const uint64_t epoch0 = GuestWriteEpoch();
    g_ctr.liveBytes -= e->verts.capacity();
    e->list = list;
    e->nbytes = nbytes;
    std::memcpy(e->vcd, g.vcd, 21);
    split::Settle(e->ticket); // (GXV_SPLIT_COPY: Build rewrites the vertices)
    const bool pure = Build(*e, list, nbytes);
    g_ctr.liveBytes += e->verts.capacity();
    const bool ok = pure;
    if (ok) {
        const uint64_t epoch1 = GuestWriteEpoch();
        e->raced = false;
        if (epoch1 != epoch0) {
            // Something was flushed meanwhile; it only matters if it was over what this read.
            HostRange used[13];
            int nUsed = 0;
            used[nUsed++] = {e->list, e->nbytes};
            for (int a = 9; a <= 20; ++a)
                if (e->idxMask & (1u << a)) used[nUsed++] = {e->arr[a - 9].p + e->arr[a - 9].lo, e->arr[a - 9].hi - e->arr[a - 9].lo};
            e->raced = GuestWritesTouch(epoch0, epoch1, used, nUsed);
        }
    }
    if (!ok) {
        // State commands inside, or data outside tracked guest RAM: the general
        // parser handles it, and the question is not asked again for a while.
        ++g_ctr.uncacheable;
        g_bypass[list].until = frame + 600;
        Unlink(li, e);
        if (!li.head) { g_map.Erase(list); MapFrontClear(); }
        if (!StateListCall(list, nbytes)) { ++g_otherWhy[OwUncacheableStream]; ProcessStream(list, nbytes); }
        else ++g_otherWhy[OwUncacheableState];
        return;
    }
    e->lastUse = age;
    ++g_otherWhy[OwBuild];
    ++g_ctr.builds;
    ++stats::Cur().dlBuild;
    g_ctr.builtVerts += e->totalVerts;
    if (e == li.head && !e->raced) g_rpOutE = e; // (GXV_DRAWREPLAY: recorded)
    Emit(*e, nbytes);
}

} // namespace vgx
