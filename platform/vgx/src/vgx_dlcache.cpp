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
// Counters: per frame in the CSV (dlHit, dlBuild, dlRehash, dlBypass), totals in vgx_stats.txt.
#include "vgx_guestmem.h"
#include "vgx_state.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

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

struct Entry {
    // ---- identity
    const uint8_t* list = nullptr;
    uint32_t nbytes = 0;
    uint8_t vcd[24] = {};          // (21 used; whole words, to compare them as such)
    uint8_t fmtMask = 0;           // vertex formats the list's draws use
    VtxAttrFmt vat[8][12] = {};    // rows of those formats (attributes POS..TEX7)
    uint32_t idxMask = 0;          // indexed attributes that were read
    const uint8_t* arr[12] = {};
    uint32_t arrStride[12] = {};
    // ---- validity
    uint64_t genList = 0;
    uint64_t genArr[12] = {};
    uint32_t lo[12] = {}, hi[12] = {};
    uint64_t hash = 0;
    // ---- content
    std::vector<Seg> segs;
    std::vector<Run> runs;         // once promoted
    std::vector<uint8_t> verts;    // decoded vertices (handed to the back end when promoted)
    std::vector<uint16_t> idx;     // only kept for GXV_HASH=2
    std::vector<RowBox> rowBoxes;  // (GXV_CHARAA) model-space bounds of the vertices per position-matrix row, [0] = all (RowBoxesOf)
    uint32_t totalVerts = 0;
    uint32_t bufId = 0;            // static buffer in the back end (0: vertices are copied into the frame arena per use)
    uint32_t bufBytes = 0;
    bool promotable = false;       // the list as a whole is too large for the CPU-transform-and-merge path
    // ---- policy
    uint64_t lastUse = 0;
    uint8_t fails = 0;             // consecutive content changes
    bool raced = false;            // a write over what this reads arrived while it was being built: rebuild on next use
    Entry* next = nullptr;         // other variants of the same list
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

int g_mode = -1; // 0 off, 1 on, 2 verify
bool g_static = true;   // GXV_STATICBUF=0: never promote (decoded vertices are copied into the arena every time)
bool g_keepCopy = false; // GXV_HASH=2: keep the bytes of promoted entries for hashing
struct Counters {
    uint64_t hits = 0, builds = 0, rehashOk = 0, rehashChanged = 0, bypassed = 0, uncacheable = 0, staleNoNotify = 0, evicted = 0;
    uint64_t hitVerts = 0, builtVerts = 0, liveBytes = 0, staticBytes = 0, promoted = 0, staticBufs = 0;
} g_ctr;

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
// Moves the entry's geometry into a static buffer of the back end.
void Promote(Entry& e) {
    // With absolute indices every primitive of the buffer is addressed from the buffer's
    // start: that needs one stride for all of them and 16 bits to be enough. (Lists with
    // more than one primitive are rare in this game; one that does not qualify stays in
    // the per-frame arena.)
    const bool abs = AbsoluteIndices();
    const uint32_t stride = e.segs[0].stride;
    if (abs) {
        bool ok = e.totalVerts <= 0x10000;
        for (const Seg& sg : e.segs) ok = ok && sg.stride == stride;
        if (!ok) { e.promotable = false; return; }
    }
    BufUpload up;
    up.bufId = e.bufId = AllocBufId(&e);
    e.runs.clear();
    for (Seg& sg : e.segs) {
        const uint32_t segStride = sg.stride;
        const uint8_t kind = (sg.prim == 0xA8 || sg.prim == 0xB0) ? PRIM_LINES : (sg.prim == 0xB8 ? PRIM_POINTS : PRIM_TRIANGLES);
        const uint32_t n = PrimIndices(sg.prim, sg.count, 0, nullptr);
        // Continue the current run, or start one: at a change of layout or primitive kind, and
        // where a run's vertices would no longer fit 16-bit indices counted from its start.
        Run* r = e.runs.empty() ? nullptr : &e.runs.back();
        if (!r || r->primKind != kind || r->layoutMask != sg.layoutMask || r->vtxCount + sg.count > 0xFFFF ||
            sg.offset != r->vtxByteOffset + r->vtxCount * segStride) {
            e.runs.emplace_back();
            r = &e.runs.back();
            r->primKind = kind;
            r->layoutMask = sg.layoutMask;
            r->vtxByteOffset = sg.offset;
            r->vtxFirst = abs ? sg.offset / stride : 0;
            r->idxOffset = static_cast<uint32_t>(up.idx.size());
        }
        sg.idxOffset = static_cast<uint32_t>(up.idx.size());
        // Indices count from the buffer's start (absolute) or from the run's first vertex.
        sg.vtxFirst = abs ? sg.offset / stride : (sg.offset - r->vtxByteOffset) / segStride;
        up.idx.resize(up.idx.size() + n);
        PrimIndices(sg.prim, sg.count, sg.vtxFirst, up.idx.data() + sg.idxOffset);
        r->vtxCount += sg.count;
        r->idxCount += n;
        ++r->prims;
        if (sg.posMtxMax > r->posMtxMax) r->posMtxMax = sg.posMtxMax;
    }
    e.bufBytes = static_cast<uint32_t>(e.verts.size() + up.idx.size() * 2);
    g_ctr.liveBytes -= e.verts.capacity();
    if (g_keepCopy) {
        up.vtx = e.verts;
        e.idx = up.idx;
        g_ctr.liveBytes += e.verts.capacity();
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

void Init() {
    if (g_mode >= 0) return;
    const char* m = std::getenv("GXV_DLCACHE");
    if (m && !std::strcmp(m, "verify")) g_mode = 2;
    else g_mode = (m && *m == '0') ? 0 : 1;
    g_static = EnvInt("GXV_STATICBUF", 1) != 0;
    g_keepCopy = diag::Int("GXV_HASH", 0) == 2;
}

// Walks a list that consists of draw commands (and padding) only. Fills
// e.segs / e.verts / spans; false if the list holds anything else.
bool Build(Entry& e, const uint8_t* p, uint32_t nbytes) {
    const uint8_t* const list = p;
    const uint8_t* const end = p + nbytes;
    ReleaseBuffer(e);
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
    for (int i = 0; i < 12; ++i) { e.lo[i] = 0xFFFFFFFFu; e.hi[i] = 0; }
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
            const ArraySpans sp = MeasureArraySpans(plan, p, count);
            for (int a = 9; a <= 20; ++a)
                if (sp.mask & (1u << a)) {
                    e.idxMask |= 1u << a;
                    if (sp.lo[a - 9] < e.lo[a - 9]) e.lo[a - 9] = sp.lo[a - 9];
                    if (sp.hi[a - 9] > e.hi[a - 9]) e.hi[a - 9] = sp.hi[a - 9];
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
            e.arr[i] = g.arrays[a].data;
            e.arrStride[i] = g.arrays[a].stride;
            e.genArr[i] = GuestWriteGeneration(e.arr[i] + e.lo[i], e.hi[i] - e.lo[i]);
            if (e.genArr[i] == kNotGuest) return false;
        }
    std::atomic_thread_fence(std::memory_order_acquire);
    for (int a = 9; a <= 20; ++a)
        if (e.idxMask & (1u << a)) {
            const int i = a - 9;
            e.hash = XXH3_64bits_withSeed(e.arr[i] + e.lo[i], e.hi[i] - e.lo[i], e.hash);
        }
    // The range pass already visited the indices. Decode once, into one allocation,
    // with the generations and hash describing bytes no newer than this decode.
    e.verts.resize(static_cast<size_t>(vertexBytes));
    e.rowBoxes.clear();
    for (Seg& s : e.segs) {
        DecodeVertices(*plans[s.fmt], list + s.idxOffset, s.count, e.verts.data() + s.offset, nullptr);
        s.posMtxMax = LastDecodedPosMtxMax();
        if (CharaAAMode()) RowBoxesOf(e.verts.data() + s.offset, s.count, s.stride, e.rowBoxes);
    }
    // A list whose vertices all fit the CPU-transform path (a particle's quad, a glyph) stays
    // in the frame's arena, where draws that differ only in their matrix merge into one record;
    // anything larger becomes a static buffer the second time it is drawn unchanged.
    if (e.totalVerts <= SoftXformLimit()) e.promotable = false;
    return !e.segs.empty();
}

uint64_t ContentHash(const Entry& e) {
    uint64_t h = XXH3_64bits(e.list, e.nbytes);
    for (int a = 9; a <= 20; ++a)
        if (e.idxMask & (1u << a)) h = XXH3_64bits_withSeed(e.arr[a - 9] + e.lo[a - 9], e.hi[a - 9] - e.lo[a - 9], h);
    return h;
}

// Reads the generations of everything the entry depends on; false if some of
// it is not tracked guest RAM.
bool ReadGenerations(const Entry& e, uint64_t& genList, uint64_t genArr[12]) {
    genList = GuestWriteGeneration(e.list, e.nbytes);
    bool ok = genList != kNotGuest;
    for (uint32_t m = (e.idxMask >> 9) & 0xFFFu; m; m &= m - 1) { // (the indexed attributes only)
        const int i = __builtin_ctz(m);
        genArr[i] = GuestWriteGeneration(e.arr[i] + e.lo[i], e.hi[i] - e.lo[i]);
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
        if (e.arr[i] != ar.data || e.arrStride[i] != ar.stride || e.hi[i] > ar.size) return false;
    }
    return true;
}

void Emit(const Entry& e, uint32_t nbytes) {
    stats::Frame& fr = stats::Cur();
    ++fr.streams;
    fr.streamBytes += nbytes;
    if (e.bufId) {
        const bool abs = AbsoluteIndices();
        for (const Run& r : e.runs) {
            SetPendingPosMtxMax(r.posMtxMax);
            if (!e.rowBoxes.empty()) SetPendingBounds(e.rowBoxes.data(), static_cast<uint32_t>(e.rowBoxes.size()));
            EmitStaticRun(r.primKind, r.layoutMask, r.vtxCount, r.idxCount, r.prims, e.bufId, abs ? 0 : r.vtxByteOffset, r.idxOffset, r.vtxFirst);
            SetPendingBounds(nullptr, 0);
            fr.primsDL += r.prims;
            fr.vertsDL += r.vtxCount;
        }
        return;
    }
    for (const Seg& s : e.segs) {
        uint8_t* out = BeginVertices(s.layoutMask, s.count);
        if (!out) continue; // (BeginVertices' guard refused the arena: this segment is not drawn)
        std::memcpy(out, e.verts.data() + s.offset, static_cast<size_t>(s.count) * s.stride);
        SetPendingPosMtxMax(s.posMtxMax);
        EmitDraw(s.prim, s.layoutMask, s.count); // (GXV_CHARAA: bounds from the vertices themselves, few by construction)
        ++fr.primsDL;
        fr.vertsDL += s.count;
    }
}

void Free(Entry* e) {
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

bool StaticBufferData(uint32_t bufId, const uint8_t*& vtx, const uint16_t*& idx) {
    if (bufId >= g_bufOwner.size() || !g_bufOwner[bufId] || g_bufOwner[bufId]->verts.empty()) return false;
    vtx = g_bufOwner[bufId]->verts.data();
    idx = g_bufOwner[bufId]->idx.data();
    return true;
}

uint64_t DlCacheLiveBytes() { return g_ctr.liveBytes; }
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
}

static uint32_t TinyListBytes() {
    static const uint32_t n = static_cast<uint32_t>(EnvInt("GXV_DL_TINY", 64));
    return n < 15 ? 15 : n;
}
void CallList(const uint8_t* list, uint32_t nbytes) {
    Init();
    // (a list of a few dozen bytes, one particle quad for instance, is decoded faster than its
    // cache entry is checked)
    if (g_mode == 0 || nbytes <= TinyListBytes() || TraceFrame()) {
        SyncVertexArrays();
        ProcessStream(list, nbytes);
        return;
    }
    // A material list seen before and unchanged: replayed at once (the geometry cache never takes state lists).
    // (first: a hit is a list in guest RAM, its generation was read there; a replay reads no vertex array but through
    // the indexed loads, which bring their own arrays up to date)
    if (StateListFastHit(list, nbytes)) return;
    SyncVertexArrays();
    if (!IsGuestRam(list, nbytes)) {
        ProcessStream(list, nbytes);
        return;
    }
    const uint64_t frame = CurrentFrame();
    const uint64_t age = AgeOf(frame);
    {
        static uint64_t lastSweep = 0;
        if (age - lastSweep >= 120) { lastSweep = age; Sweep(frame, age); }
    }
    if (!g_bypass.empty()) {
        const Bypass* b = g_bypass.Find(list);
        if (b) {
            if (frame < b->until) {
                ++g_ctr.bypassed;
                ++stats::Cur().dlBypass;
                if (!StateListCall(list, nbytes)) ProcessStream(list, nbytes);  // (material lists: pre-parsed, vgx_state.cpp)
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
        std::atomic_thread_fence(std::memory_order_acquire);
        bool same = tracked && gl == e->genList;
        for (uint32_t m = (e->idxMask >> 9) & 0xFFFu; m && same; m &= m - 1) same = ga[__builtin_ctz(m)] == e->genArr[__builtin_ctz(m)];
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
            Emit(*e, nbytes);
            return;
        }
        if (tracked && ContentHash(*e) == e->hash) {
            // A write landed in the same 4 KiB as something this entry uses, but not on it.
            const bool afterChange = e->fails != 0;
            e->genList = gl;
            std::memcpy(e->genArr, ga, sizeof ga);
            e->lastUse = age;
            e->fails = 0;
            ++g_ctr.rehashOk;
            ++stats::Cur().dlRehash;
            g_ctr.hitVerts += e->totalVerts;
            // After a real edit, wait for another reuse instead of uploading on an alternating edit/noise cycle.
            if (!afterChange && !e->bufId && e->promotable && g_static) Promote(*e);
            Emit(*e, nbytes);
            return;
        }
        ++g_ctr.rehashChanged;
        if (++e->fails >= 3) {
            // Rewritten every time it is drawn (dynamic geometry): hashing and
            // storing it is wasted work. Decode directly for the next two seconds.
            SetBypass(g_bypass[list], frame);
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
                if (e->idxMask & (1u << a)) used[nUsed++] = {e->arr[a - 9] + e->lo[a - 9], e->hi[a - 9] - e->lo[a - 9]};
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
        if (!StateListCall(list, nbytes)) ProcessStream(list, nbytes);
        return;
    }
    e->lastUse = age;
    ++g_ctr.builds;
    ++stats::Cur().dlBuild;
    g_ctr.builtVerts += e->totalVerts;
    Emit(*e, nbytes);
}

} // namespace vgx
