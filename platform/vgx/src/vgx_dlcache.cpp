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
#include "vgx_direct.h"
#include "vgx_vcache.h"
#include "vgx_facecull.h"
#include "vgx_dlkeep.h"
#include "vgx_twocore.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <map>
#include <atomic>
#include <cmath>
#include <unordered_map>

namespace vgx {

// (endwait26) display-list builds, cumulative: time (ns) and count; read by the front end's per-picture record (vgx_galaxy.cpp)
static std::atomic<uint64_t> g_dlBuildNs{0}, g_dlBuildN{0};
void DlBuildCounters(uint64_t out[2]) { out[0] = g_dlBuildNs.load(std::memory_order_relaxed); out[1] = g_dlBuildN.load(std::memory_order_relaxed); }

// (vgx_draw.cpp, GXV_FE_CENSUS: the static runs and those that repeat the run before them; see StaticRepeatCounters there)
void StaticRepeatCounters(uint64_t out[8]);
// (fedl25, GXV_FE_CENSUS: the retained records' reasons: failed tests [RfCount 5], calls with no record [3], calls that left none [7], GXV_RT_INST [3])
void RtReasonCounters(uint64_t out[20]);
// (and the six lists with the most calls that had no record, as text, cleared by the call; per `pics` pictures)
void RtNoneListText(char* out, uint32_t n, double pics, uint64_t* lost);

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
    uint8_t occ = 0; // (GXV_OCCLCULL / GXV_GEOPASS, in what was padding: 1 = Entry::occ[this run's index] holds its analysis)
    uint32_t layoutMask = 0;
    uint32_t vtxByteOffset = 0, vtxFirst = 0, vtxCount = 0;
    uint32_t idxOffset = 0, idxCount = 0;
    uint32_t posMtxMax = 0, prims = 0;
    uint32_t idxAlt = ~0u, idxAltCount = 0; // (GXV_DLVCACHE: the run's triangles reordered for the vertex cache, degenerate ones left out)
    uint32_t instBuf = 0; // (GXV_STATIC_INST: Entry::instBuf, here where a use reads it anyway)
    uint32_t vqK = 0;     // (GXV_VTXPACK: the packed run's shifts, see "packed static runs"; 0 with layoutMask & VL_QALL == 0)
    uint32_t clBeg = 0, clN = 0; // (GXV_CLUSTERCULL: this run's boxes are Entry::clusters[clBeg .. clBeg + clN); 0 clusters: none)
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
    // (GXV_DL_VARIANTS > 8, see "many variants" below: a 16-bit fold of the indexed arrays' addresses, in the bytes of padding before rpStamp,
    // so the entry's layout is unchanged; the variant walk skips an entry whose tag is not the registers' without reading its other lines)
    uint16_t stateTag = 0;
    uint32_t rpStamp = 0;         // (GXV_DRAWREPLAY) moves when the entry is built again or stops being the first variant of its list
    uint32_t bufId = 0;            // static buffer in the back end (0: vertices are copied into the frame arena per use)
    uint32_t totalVerts = 0;
    uint64_t lastUse = 0;
    // validity
    uint64_t genList = 0;
    uint8_t vcd[24] = {};          // (21 used; whole words, to compare them as such)
    Entry* next = nullptr;         // other variants of the same list
    // content a hit emits
    std::vector<Run> runs;         // once promoted
    std::vector<ta27::RunSet> t27; // (GXV_TA27: per run, in the run's index; empty with the switch off)
    std::vector<RowBox> clusters;  // (GXV_CLUSTERCULL: the rigid triangle runs' cluster boxes, see BuildClusters; empty with the switch off)
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
    // (GXV_STATIC_INST, see "instance buffers" below)
    uint32_t instBuf = 0;          // the instance buffer of the entry's one run (0: none)
    uint32_t instBytes = 0;
    uint32_t instFrame = 0;        // the last frame a use of the entry followed a use of it with no job between
    uint8_t instWant = 0;          // 0: not asked, 1: rebuilt for an instance buffer (made at the promotion), 2: not to be had (refused)
    uint8_t instSeen = 0;          // frames in which that happened (counts to the threshold)
    std::vector<OccShape> occ;     // (GXV_OCCLCULL / GXV_GEOPASS: per run, OcclAnalyse at the promotion; empty with both off)
    std::vector<facecull::Run> face; // (GXV_FACECULL: per run, what a view list is made from and the list's state; empty with the switch off or no eligible run)
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
// ---- many variants (GXV_DL_VARIANTS, fe23, 8 Nov 2026)
// A list drawn with more vertex states per picture than the variants it may keep (kMaxVariants) misses on every call that comes
// round to a state its least recently used variant was evicted for: one new variant every picture, and after four pictures in a row
// the list is put into the bypass map and decoded on every call (88 us a call in Honeyhive's profile, "list-other" / "bypassed->decode").
// The game draws such lists: fur (Game/Util/FurCtrl.cpp drawFur) draws the fur shape once per layer, each layer with its own position
// array (made once by FurShader at set-up, shared by every clone of the model), so the shape's lists have one vertex state per layer,
// plus the model's own; Honeyhive's HoneyBee and HoneyQueen (and their LOD models) and Bee Mario have fur.
//   GXV_DL_VARIANTS=<n> (read once; default 8 = as before; 8..32): the variants a list may keep. More variants only changes which of
//   the cache's own ways a call takes: a variant is a hit exactly when the normal walk would have found it (same bytes, SameState,
//   the generations), so a hit emits what a hit always emits; what the call did before (a decode with ProcessStream) and what it
//   does now (a cached entry, a static buffer) draw the same vertices with the same state (the cache's own exactness: GXV_DLCACHE).
//   With n > 8 the walk first compares an entry's stateTag (a fold of its indexed arrays' addresses) with the fold of the registers'
//   arrays for the entry's attributes: SameState compares each of those addresses, so a different tag means SameState is false and
//   the entry is passed over without reading its other cache lines. The first entry that passes SameState is the one taken, as before.
inline int VariantCap() {
    const int n = static_cast<int>(VGX_KNOB([] {
        const long v = EnvInt("GXV_DL_VARIANTS", kMaxVariants);
        return static_cast<int>(v < kMaxVariants ? kMaxVariants : v > 32 ? 32 : v);
    }()));
    return n;
}
uint64_t g_varVerify[2] = {}; // (GXV_DLCACHE=verify: variant walks checked, mismatching)
inline uint16_t FoldPtr(uintptr_t x) {
    uint64_t v = static_cast<uint64_t>(x);
    v ^= v >> 32;
    return static_cast<uint16_t>((v >> 5) ^ (v >> 21));
}
// (the fold of the arrays the indexed attributes in `m` (idxMask >> 9) name: of the entry's when it was built, of the registers' now)
inline uint16_t StateTagOf(const Entry& e) {
    uintptr_t x = 0;
    for (uint32_t m = (e.idxMask >> 9) & 0xFFFu; m; m &= m - 1) x ^= reinterpret_cast<uintptr_t>(e.arr[__builtin_ctz(m)].p);
    return FoldPtr(x);
}
inline uint16_t StateTagOfRegs(uint32_t m) {
    uintptr_t x = 0;
    for (; m; m &= m - 1) x ^= reinterpret_cast<uintptr_t>(g.arrays[9 + __builtin_ctz(m)].data);
    return FoldPtr(x);
}

// ---- census (GXV_FE_CENSUS=1, fe23): what the front end's list calls are, for sizing work on the console (default 0: nothing counted).
// Every 300 pictures, two or three "fe-census:" lines (port_log_summary, so in every build): the bypassed lists that were decoded, by
// the cause that put them there and the costliest of them by list; the variant walk (hits beyond the eighth variant, i.e. calls that
// GXV_DL_VARIANTS=8 would have decoded); and (vgx_draw.cpp, StaticRepeatCounters) the static runs that repeat the one drawn just before
// with the same shaders, render state and textures (the same model instanced: what batching such draws could take).
// (why a list was put into the bypass map: Bypass::why)
enum : uint8_t { BwNone, BwRewrittenList, BwRewrittenArrays, BwStreak, BwUncacheable, BwCount };
// (GXV_FE_CENSUS, fedl25: why the last Build refused a list, and the opcode when it was a command that is not a draw; one byte stored on a
// refusal, read by the census only. A refused list is not cached: it goes to StateListCall or ProcessStream every call.)
enum : uint8_t { BrOk, BrNotGuestList, BrNotDraw, BrCut, BrNoPlan, BrTruncated, BrVertBytes, BrBounds, BrArrNotGuest, BrNoSegs, BrCount };
uint8_t g_buildWhy = BrOk, g_buildCmd = 0;
inline bool CensusOn() { return VGX_KNOB(EnvInt("GXV_FE_CENSUS", 0) != 0) != 0; }
struct CensusList { const uint8_t* list = nullptr; uint32_t nbytes = 0, calls = 0; uint64_t ns = 0; uint8_t why = 0, variants = 0; };
struct Census {
    static constexpr uint32_t kSlots = 128;
    CensusList slot[kSlots];
    uint64_t bypassCalls[BwCount] = {}, bypassNs = 0, lost = 0;
    uint64_t walkHits = 0, deepHits = 0, walkSteps = 0;
    uint64_t bypassSub[BrCount] = {}, bypassSl[8] = {}, bypassBytes = 0, subCmd[256] = {}, slCmd[256] = {}; // (fedl25: by Build's refusal, by StateListCall's refusal, by the opcode)
    uint32_t deepest = 0, mostVariants = 0;
    uint64_t firstFrame = 0, lastFrame = 0;
} g_census;
CensusList* CensusSlot(const uint8_t* list) {
    uint32_t h = static_cast<uint32_t>((reinterpret_cast<uintptr_t>(list) >> 5) * 0x9E3779B1u) >> 25; // (7 bits: 128 slots)
    for (uint32_t k = 0; k < 8; ++k, h = (h + 1) & (Census::kSlots - 1)) {
        CensusList& s = g_census.slot[h];
        if (s.list == list) return &s;
        if (!s.list) { s.list = list; return &s; }
    }
    ++g_census.lost;
    return nullptr;
}
void CensusReport(uint64_t frame) {
    Census& c = g_census;
    c.lastFrame = frame;
    if (!c.firstFrame) { c.firstFrame = frame; return; }
    const uint64_t pics = frame - c.firstFrame;
    if (pics < 300) return;
    static uint64_t prevSr[8] = {};
    uint64_t sr[8];
    StaticRepeatCounters(sr);
    if (port_log_summary) {
        const double p = static_cast<double>(pics);
        uint64_t calls = 0;
        for (uint64_t n : c.bypassCalls) calls += n;
        port_log_summary("fe-census: %llu pictures; per picture: bypassed lists decoded %.1f (%.2f ms; put there by: variant streak %.1f, list rewritten %.1f, "
                         "arrays rewritten %.1f, state inside %.1f, not known %.1f); variant walk: hits %.1f, beyond the 8th variant %.1f (what GXV_DL_VARIANTS=8 "
                         "decodes), %.2f entries passed a hit, deepest %u, most variants of a list %u (GXV_DL_VARIANTS=%d)",
                         static_cast<unsigned long long>(pics), calls / p, c.bypassNs / 1e6 / p, c.bypassCalls[BwStreak] / p, c.bypassCalls[BwRewrittenList] / p,
                         c.bypassCalls[BwRewrittenArrays] / p, c.bypassCalls[BwUncacheable] / p, c.bypassCalls[BwNone] / p, c.walkHits / p, c.deepHits / p,
                         c.walkHits ? static_cast<double>(c.walkSteps) / static_cast<double>(c.walkHits) : 0.0, c.deepest, c.mostVariants, VariantCap());
        // (the six costliest bypassed lists: address, bytes, calls and ms per picture, us per call, cause, variants when put there)
        CensusList* top[6] = {};
        for (CensusList& s : c.slot) {
            if (!s.list || !s.calls) continue;
            for (int i = 0; i < 6; ++i) {
                if (!top[i] || s.ns > top[i]->ns) {
                    for (int k = 5; k > i; --k) top[k] = top[k - 1];
                    top[i] = &s;
                    break;
                }
            }
        }
        static const char* const kWhy[BwCount] = {"?", "list", "arrays", "streak", "state"};
        char line[640];
        int at = 0;
        for (int i = 0; i < 6 && top[i]; ++i) {
            const CensusList& s = *top[i];
            at += std::snprintf(line + at, sizeof line - at, " [%p %uB %.1f/pic %.2fms %.0fus %s v%u]", static_cast<const void*>(s.list), s.nbytes, s.calls / p,
                                s.ns / 1e6 / p, s.ns / 1e3 / s.calls, kWhy[s.why < BwCount ? s.why : 0], s.variants);
            if (at >= static_cast<int>(sizeof line) - 80) break;
        }
        port_log_summary("fe-census: costliest bypassed lists (address, bytes, calls a picture, ms a picture, us a call, cause, variants):%s%s", at ? line : " none",
                         c.lost ? " (table full: some not listed)" : "");

        // (fedl25: what the refusals were, per picture: Build's, then StateListCall's; the opcodes the list held; the retained records' reasons)
        {
            static const char* const kBr[BrCount] = {"ok", "list not guest RAM", "register load / call inside", "cut command", "no vertex plan", "truncated",
                                                    "vertex bytes", "indices beyond the array", "array not guest RAM", "no draw in it"};
            static const char* const kSl[8] = {"replayed", "GXV_STATELIST=0", "not guest RAM", "quiet after refusal", "quiet after churn", "refused now", "churn now", "raced"};
            char b1[400], b2[300], b3[200], b4[200];
            int a1 = 0, a2 = 0, a3 = 0, a4 = 0;
            b1[0] = b2[0] = b3[0] = b4[0] = 0;
            for (int i = 0; i < BrCount; ++i)
                if (c.bypassSub[i] && a1 < static_cast<int>(sizeof b1) - 60) a1 += std::snprintf(b1 + a1, sizeof b1 - a1, " %s %.1f;", kBr[i], c.bypassSub[i] / p);
            for (int i = 0; i < 8; ++i)
                if (c.bypassSl[i] && a2 < static_cast<int>(sizeof b2) - 60) a2 += std::snprintf(b2 + a2, sizeof b2 - a2, " %s %.1f;", kSl[i], c.bypassSl[i] / p);
            for (int i = 0; i < 256; ++i)
                if (c.subCmd[i] && a3 < static_cast<int>(sizeof b3) - 30) a3 += std::snprintf(b3 + a3, sizeof b3 - a3, " 0x%02X %.1f;", i, c.subCmd[i] / p);
            for (int i = 0; i < 256; ++i)
                if (c.slCmd[i] && a4 < static_cast<int>(sizeof b4) - 30) a4 += std::snprintf(b4 + a4, sizeof b4 - a4, " 0x%02X %.1f;", i, c.slCmd[i] / p);
            port_log_summary("fe-census: bypass detail per picture (%.1f lists, %.1f KB): Build refused:%s | StateListCall then refused:%s | first non-draw opcode in the list:%s | "
                             "opcode StateListCall's parse stopped at:%s",
                             calls / p, c.bypassBytes / 1024.0 / p, a1 ? b1 : " -", a2 ? b2 : " -", a3 ? b3 : " -", a4 ? b4 : " -");
            static uint64_t prevRt[20] = {};
            uint64_t rt[20];
            RtReasonCounters(rt);
            auto r = [&](int i) { return (rt[i] - prevRt[i]) / p; };
            port_log_summary("fe-census: retained records per picture: calls with no record %.1f (slot empty %.1f, another shape's %.1f, this handle for another list %.1f); "
                             "tests failed %.1f (entry marks %.1f, vertex state %.1f, list generation %.1f, array generation %.1f); normal-path calls that left no record "
                             "(no first-variant hit/build %.1f, raced %.1f, more than %u arrays %.1f, not one vertex format %.1f, CP words unknown %.1f, SameState %.1f) "
                             "and records made %.1f; GXV_RT_INST: instance slots used %.1f, hits there %.1f, records there %.1f",
                             r(5) + r(6) + r(7), r(5), r(6), r(7), r(1) + r(2) + r(3) + r(4), r(1), r(2), r(3), r(4), r(9), r(10), 6u, r(11), r(12), r(13), r(14), r(8), r(15), r(16), r(17));
            for (int i = 0; i < 20; ++i) prevRt[i] = rt[i];
            char nl[640];
            uint64_t nlost = 0;
            RtNoneListText(nl, sizeof nl, p, &nlost);
            port_log_summary("fe-census: retained: lists of the calls that had no record (list, bytes, handle, calls a picture, why none was made):%s%s", nl, nlost ? " (table full: some not listed)" : "");
        }
        auto d = [&](int i) { return (sr[i] - prevSr[i]) / p; };
        port_log_summary("fe-census: static runs per picture %.1f; repeating the run just before with the same shaders, render state and textures %.1f "
                         "(rigid %.1f, palette %.1f); runs of repeats: 2-3 draws %.1f, 4-7 %.1f, 8 or more %.1f (draws in them %.1f)",
                         d(0), d(1), d(2), d(1) - d(2), d(3), d(4), d(5), d(6));
    }
    for (int i = 0; i < 8; ++i) prevSr[i] = sr[i];
    const uint32_t keepMost = c.mostVariants;
    c = Census{};
    c.firstFrame = c.lastFrame = frame;
    c.mostVariants = keepMost;
}
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
    // f(key, value&) for every live entry (GXV_DL_KEEP_MIB's count of the entries that are due)
    template <class F>
    void ForEach(F f) {
        for (Slot& s : slots_)
            if (Live(s.key)) f(s.key, *s.val);
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
    uint8_t why = 0;      // (GXV_FE_CENSUS) why it was put here last: BwRewrittenList / BwRewrittenArrays / BwStreak / BwUncacheable
    uint8_t variants = 0; // (GXV_FE_CENSUS) the list's variants then (BwStreak: the cache was full when the streak ended)
    uint8_t sub = 0, subCmd = 0; // (GXV_FE_CENSUS, fedl25: BwUncacheable: Build's refusal (Br*) and the opcode it stopped at)
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
    // (GXV_DLVCACHE) runs reordered, their triangles, degenerate triangles left out, runs skipped (budget / no gain), and the modelled
    // vertex-shader runs (FIFO of the switch's size) of those runs: unwelded (every strip vertex), welded in the lists' order, reordered
    uint64_t vcRuns = 0, vcTris = 0, vcDegen = 0, vcSkipBudget = 0, vcSkipGain = 0, vcRaw = 0, vcWelded = 0, vcOut = 0;
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
void ReleaseInstBuffer(Entry& e);
void InstReport(uint64_t frame);
void ReleaseFace(Entry& e);
// ---- GXV_DL_DEDUP (bowl32, 20 Nov 2026; default 0): lists with the same decoded geometry share one static buffer.
// Beach Bowl draws ~170 instances of a few models a picture (palm fronds, bushes, ...), and the game gives every actor's copy of a model its own
// display list: each list was an entry with a static buffer of its own, so draws of the same mesh in the same state could not join (GXV_BE_MERGE
// "buffer/layout", GXV_STATIC_INST "not the same run of the same buffer"). Two entries whose buffer BYTES (vertices and indices as uploaded) and whose
// run tables are equal draw exactly the same triangles from either buffer, so the second one takes the first one's buffer id (reference counted;
// the buffer is freed with its last entry). The key is three 128-bit XXH3 hashes (vertices, indices, run table) and the sizes: a collision would need
// all three to collide at once (2^-128 a pair; not a byte compare, which the owner's bytes, already handed to the back end, could not give).
//   0 off   1 share   2 census only (nothing shared; the "dl-dedup:" line says what would be)
// With GXV_STATIC_INST the instance buffer is shared the same way (one per static buffer), so that a run of instances can span the entries.
// Not for GXV_HASH=2 copies, learned copies (GXV_RIGIDMERGE_DLLEARN) or GXV_FACECULL buffers.
int DedupMode() {
    static const int m = static_cast<int>(std::clamp<long>(EnvInt("GXV_DL_DEDUP", 0), 0, 2));
    return m;
}
struct DedupKey {
    uint64_t h[6] = {};
    uint32_t vtxBytes = 0, idxCount = 0, nRuns = 0;
    bool operator<(const DedupKey& o) const {
        for (int i = 0; i < 6; ++i)
            if (h[i] != o.h[i]) return h[i] < o.h[i];
        if (vtxBytes != o.vtxBytes) return vtxBytes < o.vtxBytes;
        if (idxCount != o.idxCount) return idxCount < o.idxCount;
        return nRuns < o.nRuns;
    }
};
struct DedupRec { uint32_t refs = 0; uint32_t bytes = 0; DedupKey key; };
std::map<DedupKey, uint32_t> g_dedupByKey;     // key -> static buffer id
std::vector<DedupRec> g_dedupRec;              // by static buffer id (refs 0: not a shared-capable buffer)
struct DedupCtr { uint64_t made = 0, hits = 0, hitBytes = 0, wouldHits = 0, wouldBytes = 0, instShared = 0; uint32_t liveShared = 0; } g_dd;
DedupRec* DedupRecOf(uint32_t id) { return id < g_dedupRec.size() && g_dedupRec[id].refs ? &g_dedupRec[id] : nullptr; }
void ReleaseBuffer(Entry& e) {
    ReleaseInstBuffer(e); // (GXV_STATIC_INST: it goes with the static buffer it was made from)
    ReleaseFace(e);       // (GXV_FACECULL: the runs' view lists and what they are made from, likewise)
    if (!e.bufId) return;
    if (DedupRec* d = DedupRecOf(e.bufId)) {
        if (d->refs > 1) { // (GXV_DL_DEDUP: other entries still draw from the buffer)
            --d->refs;
            if (d->refs == 1) --g_dd.liveShared;
            if (g_bufOwner[e.bufId] == &e) g_bufOwner[e.bufId] = nullptr;
            e.bufId = 0;
            e.bufBytes = 0;
            return;
        }
        g_dedupByKey.erase(d->key);
        g_ctr.staticBytes = g_ctr.staticBytes + e.bufBytes - d->bytes; // (the line below takes e.bufBytes: what the buffer holds is the record's bytes)
        d->refs = 0;
    }
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
// GXV_DLVCACHE (ta23; read once, default 0; needs GXV_DLWELD=1, without which every strip has vertices of its own and no order helps):
// each welded triangle run of a promoted list also gets its triangles in an order made for the GPU's post-transform vertex cache
// (vgx_vcache.h), with degenerate triangles (an index twice: zero area, no pixel) left out, appended after the list's own indices (the
// runs' blocks in the same order, so that consecutive runs stay consecutive in both). Each triangle keeps its indices in their order
// (winding, first vertex). EmitStaticRun takes the reordered block only for a draw whose picture does not depend on the order of its
// own triangles: no blending, depth test with LESS / LEQUAL / GREATER / GEQUAL, depth writes on (what is seen at a pixel is the nearest
// fragment whatever the order). The one exception, which makes it a minor change and not bit-exact: pixels where two triangles of the
// same draw give exactly the same depth (an intersection line or a coplanar overlap inside one shape) may take the other triangle's
// colour. Blended, depth-less and ALWAYS / EQUAL / NEQUAL draws keep the list's order.
//   0 off (as before); 1 Tipsify (Sander et al. 2007: ~0.08 us a triangle on a desktop core, ~0.4 on the console; the better order
//   on the strip meshes of tests/vgx_vcache.cpp); 2 Forsyth (2006: ~8x the cost).
// GXV_DLVCACHE_SIZE (default 16): the modelled FIFO's entries (4..32).
// GXV_DLVCACHE_BUDGET (default 16000): triangles reordered per picture at most (the work is on the front end's thread, at promotion,
// i.e. the second unchanged use of a list, mostly at an area's entry; a run beyond the budget keeps the list's order until the list is
// built again). A run is also left alone when the order gains less than 3% in the model and it has no degenerate triangle.
int VcacheMode() {
    static const int m = static_cast<int>(std::clamp<long>(EnvInt("GXV_DLVCACHE", 0), 0, 2));
    return m;
}
uint32_t VcacheSize() {
    static const uint32_t n = static_cast<uint32_t>(std::clamp<long>(EnvInt("GXV_DLVCACHE_SIZE", 16), 4, 32));
    return n;
}
uint32_t VcacheBudget() {
    static const uint32_t n = static_cast<uint32_t>(std::clamp<long>(EnvInt("GXV_DLVCACHE_BUDGET", 16000), 0, 1L << 30));
    return n;
}
uint64_t g_vcFrame = ~0ull;
uint32_t g_vcUsed = 0; // (triangles reordered in picture g_vcFrame)
// The reordered blocks of a promoted list's welded triangle runs (see above); raw[i]: run i's vertices before welding.
void VcacheRuns(Entry& e, BufUpload& up, const std::vector<uint32_t>& raw) {
    const uint32_t size = VcacheSize();
    static vcache::Scratch scratch;
    static std::vector<uint16_t> local, out;
    const uint64_t frame = CurrentFrame();
    if (frame != g_vcFrame) { g_vcFrame = frame; g_vcUsed = 0; }
    for (size_t ri = 0; ri < e.runs.size(); ++ri) {
        Run& r = e.runs[ri];
        if (r.primKind != PRIM_TRIANGLES || r.idxCount < 6 || r.vtxCount == 0) continue;
        const uint32_t tris = r.idxCount / 3;
        if (g_vcUsed + tris > VcacheBudget()) { ++g_ctr.vcSkipBudget; continue; }
        // (the run's indices, as numbers within its block; degenerate triangles out)
        local.clear();
        const uint16_t* ix = up.idx.data() + r.idxOffset;
        uint32_t degen = 0;
        bool bad = false;
        for (uint32_t t = 0; t < tris; ++t) {
            const uint32_t a = ix[t * 3] - r.vtxFirst, b = ix[t * 3 + 1] - r.vtxFirst, c = ix[t * 3 + 2] - r.vtxFirst;
            if (a >= r.vtxCount || b >= r.vtxCount || c >= r.vtxCount) { bad = true; break; }
            if (a == b || b == c || a == c) { ++degen; continue; }
            local.push_back(static_cast<uint16_t>(a)); local.push_back(static_cast<uint16_t>(b)); local.push_back(static_cast<uint16_t>(c));
        }
        if (bad || local.empty()) continue;
        g_vcUsed += tris;
        out.resize(local.size());
        const bool made = VcacheMode() == 2 ? vcache::Optimize(local.data(), static_cast<uint32_t>(local.size()), r.vtxCount, size, out.data(), scratch)
                                            : vcache::Tipsify(local.data(), static_cast<uint32_t>(local.size()), r.vtxCount, size, out.data(), scratch);
        if (!made) continue;
        const uint32_t before = vcache::FifoMisses(ix, r.idxCount, size), after = vcache::FifoMisses(out.data(), static_cast<uint32_t>(out.size()), size);
        if (!degen && after * 100u > before * 97u) { ++g_ctr.vcSkipGain; continue; }
        r.idxAlt = static_cast<uint32_t>(up.idx.size());
        r.idxAltCount = static_cast<uint32_t>(out.size());
        for (uint16_t v : out) up.idx.push_back(static_cast<uint16_t>(v + r.vtxFirst));
        ++g_ctr.vcRuns;
        g_ctr.vcTris += tris;
        g_ctr.vcDegen += degen;
        g_ctr.vcRaw += ri < raw.size() ? raw[ri] : 0u;
        g_ctr.vcWelded += before;
        g_ctr.vcOut += after;
    }
}
// The distinct vertices of one run: an open-addressing table of slots in the new buffer, emptied by a stamp per run.
// g30 Wave 5 (ported from Strikers 2.0 int28): 32-bit word-mix FNV-1a + finalizer, cap-sized mask (L1/L2 friendly),
// pre-sized cursor writes (zero vector::insert), and nextSlot counter (zero ARMv7 __aeabi_uidiv).
struct Welder {
    std::vector<uint32_t> stamp, slot;
    uint32_t mask = 0, cur = 0, nextSlot = 0;
    void Init(uint32_t maxVerts) {
        const uint32_t runMax = maxVerts > 0x10000u ? 0x10000u : maxVerts; // runs cut at 0xFFFF raw verts
        uint32_t cap = 256;
        while (cap < runMax * 2u) cap <<= 1;
        if (stamp.size() < cap) { stamp.assign(cap, 0); slot.assign(cap, 0); cur = 0; }
        mask = cap - 1u; // keep small lists inside L1/L2 instead of scattering across a huge table grown by one giant list
    }
    void NewRun() {
        if (++cur == 0) { std::fill(stamp.begin(), stamp.end(), 0u); cur = 1; }
        nextSlot = 0;
    }
    static inline uint32_t HashVert(const uint8_t* v, uint32_t stride) {
        uint32_t h = 2166136261u ^ stride;
        const uint32_t words = stride >> 2;
        for (uint32_t i = 0; i < words; ++i) {
            uint32_t w;
            std::memcpy(&w, v + (i << 2), 4);
            h ^= w;
            h *= 16777619u;
        }
        for (uint32_t i = words << 2; i < stride; ++i) {
            h ^= v[i];
            h *= 16777619u;
        }
        h ^= h >> 16;
        h *= 0x85ebca6bu;
        h ^= h >> 13;
        return h;
    }
    // The slot (index among the run's distinct vertices, from 0) of vertex `v` (`stride` bytes);
    // the run's block starts `block` bytes into `outData`; new vertices write at `outPos` (pre-sized buffer).
    uint32_t Find(uint8_t* outData, size_t& outPos, size_t block, const uint8_t* v, uint32_t stride) {
        uint32_t h = HashVert(v, stride) & mask;
        for (;;) {
            if (stamp[h] != cur) {
                const uint32_t n = nextSlot++;
                std::memcpy(outData + outPos, v, stride);
                outPos += stride;
                stamp[h] = cur;
                slot[h] = n;
                return n;
            }
            if (std::memcmp(outData + block + static_cast<size_t>(slot[h]) * stride, v, stride) == 0) return slot[h];
            h = (h + 1u) & mask;
        }
    }
};

// ---- view lists (GXV_FACECULL, farlod32, 13 Nov 2026; vgx_facecull.h has the method and why it is exact, vgx_draw.cpp "view lists" the use)
// Promote keeps, for each rigid triangle run of at least GXV_FACECULL_MIN indices (default 900), the run's triangles, one plane per
// triangle and a copy of its vertex block (the bytes the static buffer holds), while all such copies stay under GXV_FACECULL_KB
// (default 4096: about 14 bytes a triangle and the vertex block). The view list itself is a static buffer of its own made by the record
// side (FaceTake); it is released with the entry's buffer, and a released list's buffer id is freed four pictures later, so that no
// record of a picture that may still be replayed (GXV_HRW) names a buffer that was freed or handed out again.
uint32_t FaceMinIdx() {
    static const uint32_t n = static_cast<uint32_t>(std::clamp<long>(EnvInt("GXV_FACECULL_MIN", 900), 12, 1L << 24));
    return n;
}
uint64_t FaceBudgetBytes() {
    static const uint64_t n = static_cast<uint64_t>(std::clamp<long>(EnvInt("GXV_FACECULL_KB", 4096), 0, 1L << 20)) << 10;
    return n;
}
struct FaceTotals { uint64_t runs = 0, cpuBytes = 0, lists = 0, gpuBytes = 0, refusedBudget = 0, freeQueued = 0; } g_face;
struct FaceFree { uint32_t bufId; uint64_t frame; };
std::vector<FaceFree> g_faceFree;
void FaceDropList(facecull::Run& r) {
    if (r.bufId) {
        g_faceFree.push_back(FaceFree{r.bufId, CurrentFrame()});
        ++g_face.freeQueued;
        g_face.gpuBytes -= static_cast<uint64_t>(r.vtxKept) * r.stride + static_cast<uint64_t>(r.idxKept) * 2u;
        r.bufId = 0;
    }
    if (r.margin > 0.0f) --g_face.lists;
    r.margin = 0.0f;
    r.idxKept = r.vtxKept = 0;
}
void ReleaseFace(Entry& e) {
    for (facecull::Run& r : e.face) {
        if (r.planes.empty()) continue;
        FaceDropList(r);
        g_face.cpuBytes -= r.Bytes();
        --g_face.runs;
    }
    e.face = std::vector<facecull::Run>();
}
// Promote, step 1 (the buffer's vertices still floats: before GXV_VTXPACK): the triangles and planes of the eligible runs.
void FacePlanes(Entry& e, const std::vector<uint8_t>& vb, const std::vector<uint16_t>& idx) {
    static std::vector<uint16_t> local;
    for (size_t i = 0; i < e.runs.size(); ++i) {
        const Run& r = e.runs[i];
        if (r.primKind != PRIM_TRIANGLES || r.idxCount < FaceMinIdx() || r.idxCount % 3 != 0 || r.vtxCount < 3 || r.vtxCount > 0xFFFF) continue;
        if (static_cast<size_t>(r.idxOffset) + r.idxCount > idx.size()) continue;
        const uint32_t rs = MakeLayout(r.layoutMask).stride;
        if ((r.layoutMask & VL_QALL) || rs < 16 || static_cast<size_t>(r.vtxByteOffset) + static_cast<size_t>(r.vtxCount) * rs > vb.size()) continue;
        const uint64_t need = static_cast<uint64_t>(r.idxCount) * 2u + static_cast<uint64_t>(r.idxCount / 3) * sizeof(facecull::Plane) + static_cast<uint64_t>(r.vtxCount) * rs;
        if (g_face.cpuBytes + need > FaceBudgetBytes()) { ++g_face.refusedBudget; continue; }
        local.resize(r.idxCount);
        bool ok = true;
        for (uint32_t k = 0; k < r.idxCount && ok; ++k) {
            const uint32_t x = idx[r.idxOffset + k];
            ok = x >= r.vtxFirst && x - r.vtxFirst < r.vtxCount;
            local[k] = static_cast<uint16_t>(x - r.vtxFirst);
        }
        if (!ok) continue;
        if (e.face.empty()) e.face.resize(e.runs.size());
        facecull::Run& f = e.face[i];
        if (!facecull::BuildPlanes(vb.data() + r.vtxByteOffset, r.vtxCount, rs, local.data(), r.idxCount, f.planes, f.c, &f.r)) { f = facecull::Run(); continue; }
        f.idx.assign(local.begin(), local.end());
    }
}
// Promote, step 2 (the buffer as it is uploaded): the runs' vertex blocks.
void FaceVerts(Entry& e, const std::vector<uint8_t>& vb) {
    bool any = false;
    for (size_t i = 0; i < e.face.size() && i < e.runs.size(); ++i) {
        facecull::Run& f = e.face[i];
        if (f.planes.empty()) continue;
        const Run& r = e.runs[i];
        const uint32_t rs = MakeLayout(r.layoutMask).stride;
        if (static_cast<size_t>(r.vtxByteOffset) + static_cast<size_t>(r.vtxCount) * rs > vb.size()) { f = facecull::Run(); continue; }
        f.vtx.assign(vb.begin() + r.vtxByteOffset, vb.begin() + r.vtxByteOffset + static_cast<size_t>(r.vtxCount) * rs);
        f.stride = rs;
        f.vtxCount = r.vtxCount;
        g_face.cpuBytes += f.Bytes();
        ++g_face.runs;
        any = true;
    }
    if (!any) e.face = std::vector<facecull::Run>();
}

// ---- packed static runs (GXV_VTXPACK, geofeed23, 8 Nov 2026)
// The GPU's vertex (TA) phase reads every vertex of a static run from its buffer in the decoded layout (vgx_cmd.h): position 4 x f32,
// normal 3 x f32, colours 4 x u8, texture coordinates 2 x f32 each, 32-48 bytes a vertex. The game's own data is mostly fixed point:
// positions s16 (or s8) with a fraction shift, normals s16 >> 14 or s8 >> 6, texture coordinates s16 / u8 with a shift, and the decode
// gives v = q * 2^-k for whole numbers q (vgx_state.cpp AttrComp: float(q) * (1 / (1 << k)), exact). With the switch a run's buffer keeps
// q instead (VL_QPOS / VL_QNRM / VL_QNRM8 / VL_QTEX: 4 x s16, 3 x s16 or s8 padded to 4 bytes, 2 x s16), 16-24 bytes a vertex, and the
// GPU converts each q to the float of the same value (exact: |q| < 2^15). The factor 2^-k goes into the matrices instead
// (vgx_draw.cpp VqScaleBlocks): every use of such an attribute in the generated vertex shader is a dot product of it with matrix rows
// (eye = u_mv[0..2] . (x, y, z, 1), normal = u_mv[3..5] . n, the palette's rows the same way, a texgen's u_tm rows . its source), and
// (m * 2^-k) * q is the same real number as m * (q * 2^-k): both products round to the same float (a power of two scales without
// rounding), so every sum, the eye position, the normal, gl_Position and every texture coordinate are bit for bit those of the float
// buffer, and the vertex shader's TEXT is unchanged (no new shader, nothing to add to the shader pack). Exactness holds unless a scaled
// matrix element becomes subnormal (a nonzero |m| < 2^(k-126), i.e. below ~1e-30: counted, expected never; VtxPackDrawCounters).
// What is packed, run by run, decided from the decoded floats themselves (so it is exact whatever format the list had):
//   - an attribute whose every value is +0 or a whole multiple of 2^-k (the smallest k <= 30) within s16 (s8 for the 4-byte normal)
//     range: no -0, no subnormal, no infinity or NaN; the position's w (the matrix row) a whole number 0..32767, kept as it is;
//   - only triangle runs without texture-matrix indices (VL_TMI: a per-vertex texture matrix reads palette rows that a position scale
//     would also scale), and texture coordinates only for tex0 / tex1 (both, when the run has both);
//   - with absolute indices (GXV_ABSIDX=1) every run of the list must end with the same stride (else none is packed).
// Not packed: GXV_HASH=2 (reference copies), learned copies (GXV_RIGIDMERGE_DLLEARN), lists rebuilt for an instance buffer
// (GXV_STATIC_INST), CPU-transformed and every other arena draw.
//   GXV_VTXPACK=0 (default) off: buffers, records and blocks as before; 1 on. Read once.
bool PackSwitch() {
    static const bool on = EnvInt("GXV_VTXPACK", 0) == 1;
    return on;
}
// (written by the player thread only, read by the back end's report: relaxed)
std::atomic<uint64_t> g_vpLists{0}, g_vpRuns{0}, g_vpRunsSeen{0}, g_vpVerts{0}, g_vpBytesIn{0}, g_vpBytesOut{0}, g_vpAbsRefused{0};
inline void VpAdd(std::atomic<uint64_t>& a, uint64_t v) { a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); }
// The fraction bits of the float with bits u: the smallest f >= 0 for which value * 2^f is a whole number; false for -0, a subnormal,
// an infinity or a NaN.
inline bool FracBits(uint32_t u, int& f) {
    if (u == 0) { f = 0; return true; }
    const uint32_t e = (u >> 23) & 0xFFu;
    if (e == 0 || e == 0xFFu) return false; // (-0 has e == 0 too)
    const uint32_t m = (u & 0x7FFFFFu) | 0x800000u;
    f = 150 - static_cast<int>(e) - __builtin_ctz(m);
    if (f < 0) f = 0;
    return true;
}
// The smallest shift k (0..30) that makes each of `comps` floats at `off` of `n` vertices (`stride` bytes apart) a whole number in
// [lo, hi]; -1 when there is none.
int ShiftOf(const uint8_t* v, uint32_t n, uint32_t stride, uint32_t off, int comps, int lo, int hi) {
    int k = 0;
    for (uint32_t i = 0; i < n; ++i)
        for (int c = 0; c < comps; ++c) {
            uint32_t u;
            std::memcpy(&u, v + static_cast<size_t>(i) * stride + off + 4u * c, 4);
            int f;
            if (!FracBits(u, f) || f > 30) return -1;
            if (f > k) k = f;
        }
    for (uint32_t i = 0; i < n; ++i)
        for (int c = 0; c < comps; ++c) {
            float x;
            std::memcpy(&x, v + static_cast<size_t>(i) * stride + off + 4u * c, 4);
            const double q = std::ldexp(static_cast<double>(x), k);
            if (q < lo || q > hi) return -1;
        }
    return k;
}
// q = x * 2^k (a whole number by ShiftOf), checked back: float(q) * 2^-k must be x, bit for bit.
inline bool Quant(const uint8_t* s, int k, int32_t& q) {
    float x;
    std::memcpy(&x, s, 4);
    q = static_cast<int32_t>(std::ldexp(static_cast<double>(x), k));
    const float back = static_cast<float>(q) * std::ldexp(1.0f, -k);
    return std::memcmp(&back, &x, 4) == 0;
}
struct PackPlan { uint32_t flags = 0, vqK = 0; int kp = 0, kn = 0, kt[2] = {0, 0}; };
// What of run r (float vertices at v) can be packed.
PackPlan PlanRun(const Run& r, const uint8_t* v) {
    PackPlan p;
    if (r.primKind != PRIM_TRIANGLES || (r.layoutMask & (VL_TMI | VL_QALL)) || r.vtxCount == 0) return p;
    const VtxLayout L = MakeLayout(r.layoutMask);
    const uint32_t n = r.vtxCount, st = L.stride;
    // position: x, y, z; w (the matrix row) a whole number 0..32767 as it is
    const int kp = ShiftOf(v, n, st, 0, 3, -32768, 32767);
    bool wOk = kp >= 0;
    for (uint32_t i = 0; i < n && wOk; ++i) {
        uint32_t u;
        std::memcpy(&u, v + static_cast<size_t>(i) * st + 12, 4);
        int f;
        float w;
        std::memcpy(&w, &u, 4);
        wOk = FracBits(u, f) && f == 0 && !(u >> 31) && w <= 32767.0f;
    }
    if (wOk) { p.flags |= VL_QPOS; p.kp = kp; }
    if (r.layoutMask & VL_NRM) {
        const int k8 = ShiftOf(v, n, st, L.offNrm, 3, -128, 127);
        if (k8 >= 0) { p.flags |= VL_QNRM8; p.kn = k8; }
        else {
            const int k16 = ShiftOf(v, n, st, L.offNrm, 3, -32768, 32767);
            if (k16 >= 0) { p.flags |= VL_QNRM; p.kn = k16; }
        }
    }
    if (r.layoutMask & (VL_TEX0 | (VL_TEX0 << 1))) {
        bool ok = true;
        for (int t = 0; t < 2 && ok; ++t)
            if (r.layoutMask & (VL_TEX0 << t)) {
                p.kt[t] = ShiftOf(v, n, st, L.offTex[t], 2, -32768, 32767);
                ok = p.kt[t] >= 0;
            }
        if (ok) p.flags |= VL_QTEX;
        else p.kt[0] = p.kt[1] = 0;
    }
    p.vqK = static_cast<uint32_t>(p.kp) | (static_cast<uint32_t>(p.kn) << 5) | (static_cast<uint32_t>(p.kt[0]) << 10) | (static_cast<uint32_t>(p.kt[1]) << 15);
    return p;
}
// Writes the n vertices of layout `from` at s in the packed layout `to` (plan p) at d; false when a value does not convert back exactly.
bool PackVertices(const uint8_t* s, uint8_t* d, uint32_t n, const VtxLayout& from, const VtxLayout& to, const PackPlan& p) {
    const uint32_t m = from.mask;
    for (uint32_t i = 0; i < n; ++i, s += from.stride, d += to.stride) {
        std::memset(d, 0, to.stride);
        if (p.flags & VL_QPOS) {
            int16_t w[4];
            for (int c = 0; c < 3; ++c) {
                int32_t q;
                if (!Quant(s + 4 * c, p.kp, q)) return false;
                w[c] = static_cast<int16_t>(q);
            }
            int32_t row;
            if (!Quant(s + 12, 0, row)) return false;
            w[3] = static_cast<int16_t>(row);
            std::memcpy(d, w, 8);
        } else {
            std::memcpy(d, s, 16);
        }
        if (m & VL_NRM) {
            if (p.flags & (VL_QNRM | VL_QNRM8)) {
                for (int c = 0; c < 3; ++c) {
                    int32_t q;
                    if (!Quant(s + from.offNrm + 4 * c, p.kn, q)) return false;
                    if (p.flags & VL_QNRM8) d[to.offNrm + c] = static_cast<uint8_t>(static_cast<int8_t>(q));
                    else { const int16_t h = static_cast<int16_t>(q); std::memcpy(d + to.offNrm + 2 * c, &h, 2); }
                }
            } else {
                std::memcpy(d + to.offNrm, s + from.offNrm, 12);
            }
        }
        if (m & VL_COL0) std::memcpy(d + to.offCol0, s + from.offCol0, 4);
        if (m & VL_COL1) std::memcpy(d + to.offCol1, s + from.offCol1, 4);
        for (int t = 0; t < 8; ++t) {
            if (!(m & (VL_TEX0 << t))) continue;
            if (t < 2 && (p.flags & VL_QTEX)) {
                for (int c = 0; c < 2; ++c) {
                    int32_t q;
                    if (!Quant(s + from.offTex[t] + 4 * c, p.kt[t], q)) return false;
                    const int16_t h = static_cast<int16_t>(q);
                    std::memcpy(d + to.offTex[t] + 2 * c, &h, 2);
                }
            } else {
                std::memcpy(d + to.offTex[t], s + from.offTex[t], 8);
            }
        }
        if (m & VL_TMI) std::memcpy(d + to.offTmi, s + from.offTmi, 8);
    }
    return true;
}
// Packs what can be packed of the promoted entry's runs in up.vtx (float vertices, the runs' blocks at their vtxByteOffset). All or
// nothing: on any refusal up.vtx and the runs stay as they are.
void PackRuns(Entry& e, BufUpload& up) {
    static std::vector<PackPlan> plans;
    static std::vector<uint32_t> offs;
    static std::vector<uint8_t> out;
    plans.assign(e.runs.size(), PackPlan{});
    offs.assign(e.runs.size(), 0);
    bool any = false;
    for (size_t i = 0; i < e.runs.size(); ++i) {
        const Run& r = e.runs[i];
        const uint32_t st = MakeLayout(r.layoutMask).stride;
        if (static_cast<size_t>(r.vtxByteOffset) + static_cast<size_t>(r.vtxCount) * st > up.vtx.size()) return; // (not the blocks expected)
        VpAdd(g_vpRunsSeen, 1);
        plans[i] = PlanRun(r, up.vtx.data() + r.vtxByteOffset);
        any = any || plans[i].flags != 0;
    }
    if (!any) return;
    const bool abs = AbsoluteIndices();
    uint32_t absStride = 0;
    if (abs) {
        // (one stride for the whole buffer: every run packed the same way to the same stride, or none)
        absStride = MakeLayout(e.runs[0].layoutMask | plans[0].flags).stride;
        for (size_t i = 0; i < e.runs.size(); ++i)
            if (!plans[i].flags || MakeLayout(e.runs[i].layoutMask | plans[i].flags).stride != absStride) { VpAdd(g_vpAbsRefused, 1); return; }
    }
    out.clear();
    out.reserve(up.vtx.size());
    uint64_t verts = 0;
    for (size_t i = 0; i < e.runs.size(); ++i) {
        const Run& r = e.runs[i];
        const VtxLayout from = MakeLayout(r.layoutMask), to = MakeLayout(r.layoutMask | plans[i].flags);
        const uint8_t* const s = up.vtx.data() + r.vtxByteOffset;
        const size_t o = out.size();
        offs[i] = static_cast<uint32_t>(o);
        if (abs && (o % absStride != 0 || o / absStride != r.vtxFirst)) { VpAdd(g_vpAbsRefused, 1); return; } // (the numbering would move)
        if (!plans[i].flags) {
            out.insert(out.end(), s, s + static_cast<size_t>(r.vtxCount) * from.stride);
            continue;
        }
        out.resize(o + static_cast<size_t>(r.vtxCount) * to.stride);
        if (!PackVertices(s, out.data() + o, r.vtxCount, from, to, plans[i])) return;
        verts += r.vtxCount;
    }
    uint32_t runs = 0;
    for (size_t i = 0; i < e.runs.size(); ++i) {
        Run& r = e.runs[i];
        r.vtxByteOffset = offs[i];
        if (!plans[i].flags) continue;
        r.layoutMask |= plans[i].flags;
        r.vqK = plans[i].vqK;
        ++runs;
    }
    VpAdd(g_vpLists, 1);
    VpAdd(g_vpRuns, runs);
    VpAdd(g_vpVerts, verts);
    VpAdd(g_vpBytesIn, up.vtx.size());
    VpAdd(g_vpBytesOut, out.size());
    up.vtx.assign(out.begin(), out.end());
}

// Moves the entry's geometry into a static buffer of the back end.
void MakeInstBuffer(Entry& e, const BufUpload& up);
// GXV_CLUSTERCULL (vertex25): the boxes of consecutive triangles of each big rigid triangle run, worked out once when the list is promoted
// (vb: the buffer's float vertices, welded or not, as the run's indices address them). A run's indices are whole triangles, in the order the
// run draws them (the list's strips, expanded); a cluster is `per` consecutive indices (a multiple of 3), the last one what is left. The box
// holds the position of every vertex of its triangles (x, y, z of the decoded vertex, the very floats the vertex shader multiplies), so the
// triangles are inside the convex hull of the box's corners in any affine transform: vgx_draw.cpp "clusters of a static run" tests the box
// with BoxOutMask, as the whole run's box is tested. A run with a non-finite coordinate or an index outside the buffer gets no clusters.
// With GXV_CLUSTERCULL=3 every vertex of every triangle is looked up again and compared with its cluster's box (the counter "box bad").
void BuildClusters(Entry& e, const std::vector<uint8_t>& vb, const std::vector<uint16_t>& idx, bool abs) {
    const uint32_t T = ClusterTris(), minRun = ClusterMinRun();
    const bool check = ClusterMode() == 3;
    uint32_t nRuns = 0, bad = 0;
    for (Run& r : e.runs) {
        r.clBeg = 0;
        r.clN = 0;
        if (r.primKind != PRIM_TRIANGLES || r.idxCount < minRun || r.idxCount % 3 != 0 || r.idxOffset + r.idxCount > idx.size()) continue;
        const uint32_t rs = MakeLayout(r.layoutMask).stride;
        if (rs < 12) continue;
        const size_t base = abs ? 0 : r.vtxByteOffset;
        uint32_t per = T * 3;
        while (r.idxCount / per > 255) per += T * 3;
        const size_t first = e.clusters.size();
        bool ok = true;
        for (uint32_t s0 = 0; s0 < r.idxCount && ok; s0 += per) {
            const uint32_t s1 = std::min(r.idxCount, s0 + per);
            RowBox b{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}, s0, {}, 0};
            for (uint32_t i = s0; i < s1 && ok; ++i) {
                const size_t at = base + static_cast<size_t>(idx[r.idxOffset + i]) * rs;
                if (at + 12 > vb.size()) { ok = false; break; }
                float q[3];
                std::memcpy(q, vb.data() + at, sizeof q);
                if (!(std::isfinite(q[0]) && std::isfinite(q[1]) && std::isfinite(q[2]))) { ok = false; break; }
                for (int a = 0; a < 3; ++a) { b.mn[a] = std::min(b.mn[a], q[a]); b.mx[a] = std::max(b.mx[a], q[a]); }
            }
            if (ok) e.clusters.push_back(b);
        }
        if (!ok) { e.clusters.resize(first); continue; }
        r.clBeg = static_cast<uint32_t>(first);
        r.clN = static_cast<uint32_t>(e.clusters.size() - first);
        ++nRuns;
        if (check) {
            for (uint32_t c = 0; c < r.clN; ++c) {
                const RowBox& b = e.clusters[first + c];
                const uint32_t s0 = b.row, s1 = c + 1 < r.clN ? e.clusters[first + c + 1].row : r.idxCount;
                if (s0 % 3 != 0 || s1 <= s0 || s1 > r.idxCount || (c == 0 && s0 != 0)) ++bad;
                for (uint32_t i = s0; i < s1 && i < r.idxCount; ++i) {
                    float q[3];
                    std::memcpy(q, vb.data() + base + static_cast<size_t>(idx[r.idxOffset + i]) * rs, sizeof q);
                    for (int a = 0; a < 3; ++a) bad += (q[a] < b.mn[a] || q[a] > b.mx[a]) ? 1u : 0u;
                }
            }
        }
    }
    ClusterBuildNote(nRuns, static_cast<uint32_t>(e.clusters.size()), bad);
}

// GXV_TA27 (ta27, vgx_ta27.h): for each big rigid triangle run, the clusters of its triangles in the list's own order and after a Morton sort
// of the triangles, each with its box and normal cone, for the census in vgx_draw.cpp. Same conditions and vertex addressing as BuildClusters.
// GXV_TA27=2 also checks, per cluster, every vertex against the box and every triangle's unit normal against the cone ("box bad" / "cone bad").
void BuildTa27(Entry& e, const std::vector<uint8_t>& vb, const std::vector<uint16_t>& idx, bool abs) {
    const uint32_t T = Ta27Tris(), minRun = Ta27MinRun();
    const bool check = Ta27Mode() == 2;
    e.t27.assign(e.runs.size(), ta27::RunSet{});
    uint32_t nRuns = 0, nCl = 0, bad = 0;
    static std::vector<uint32_t> order;
    for (size_t ri = 0; ri < e.runs.size(); ++ri) {
        const Run& r = e.runs[ri];
        if (r.primKind != PRIM_TRIANGLES || r.idxCount < minRun || r.idxCount % 3 != 0 || r.idxOffset + r.idxCount > idx.size()) continue;
        const uint32_t rs = MakeLayout(r.layoutMask).stride;
        if (rs < 12) continue;
        const size_t base = abs ? 0 : r.vtxByteOffset;
        const uint16_t* ix = idx.data() + r.idxOffset;
        bool ok = true;
        for (uint32_t i = 0; i < r.idxCount && ok; ++i) ok = base + static_cast<size_t>(ix[i]) * rs + 12 <= vb.size();
        if (!ok) continue;
        float tmp[4][3];   // (tls40: stack locals, not thread_local: the lambda lives only inside this iteration; a thread_local is a call on the console)
        unsigned rot = 0;
        auto pos = [&](uint16_t i) -> const float* {
            float* d = tmp[rot++ & 3u];
            std::memcpy(d, vb.data() + base + static_cast<size_t>(i) * rs, 12); // (unaligned reads: memcpy)
            return d;
        };
        ta27::RunSet& rsw = e.t27[ri];
        if (!ta27::BuildClusters(ix, r.idxCount, T * 3, pos, nullptr, rsw.o)) { rsw = ta27::RunSet{}; continue; }
        ta27::MortonOrder(ix, r.idxCount, pos, order);
        if (!ta27::BuildClusters(ix, r.idxCount, T * 3, pos, order.data(), rsw.s)) { rsw = ta27::RunSet{}; continue; }
        ++nRuns;
        nCl += static_cast<uint32_t>(rsw.o.size() + rsw.s.size());
        if (check) {
            for (int pass = 0; pass < 2; ++pass) {
                const std::vector<ta27::Cl>& cl = pass ? rsw.s : rsw.o;
                for (size_t c = 0; c < cl.size(); ++c) {
                    const uint32_t s0 = cl[c].row, s1 = c + 1 < cl.size() ? cl[c + 1].row : r.idxCount;
                    for (uint32_t i = s0; i < s1; i += 3) {
                        const uint32_t tri = pass ? order[i / 3] : i / 3;
                        float q[3][3];
                        for (int k = 0; k < 3; ++k) std::memcpy(q[k], pos(ix[tri * 3 + k]), 12);
                        for (int k = 0; k < 3; ++k)
                            for (int a = 0; a < 3; ++a) bad += (q[k][a] < cl[c].mn[a] || q[k][a] > cl[c].mx[a]) ? 1u : 0u;
                        float e1[3], e2[3], nn[3];
                        for (int a = 0; a < 3; ++a) { e1[a] = q[1][a] - q[0][a]; e2[a] = q[2][a] - q[0][a]; }
                        ta27::Cross(e1, e2, nn);
                        const float len = std::sqrt(ta27::Dot(nn, nn));
                        if (len > 1e-20f && cl[c].cut > 0.05f && ta27::Dot(nn, cl[c].ax) / len < cl[c].cut - 1e-4f) ++bad;
                    }
                }
            }
        }
    }
    Ta27BuildNote(nRuns, nCl, bad);
}

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
    ReleaseFace(e); // (GXV_FACECULL: the runs are made again)
    BufUpload up;
    up.bufId = e.bufId = AllocBufId(&e);
    e.runs.clear();
    e.clusters.clear();
    e.t27.clear();
    const bool weld = !g_keepCopy && WeldOn();
    static Welder welder;
    static std::vector<uint16_t> slotOf;
    size_t outPos = 0;
    if (weld) {
        welder.Init(e.totalVerts);
        size_t maxVtxBytes = 0;
        for (const Seg& sg : e.segs) maxVtxBytes += static_cast<size_t>(sg.count) * sg.stride;
        up.vtx.resize(maxVtxBytes);
    }
    uint64_t weldedVerts = 0; // (welding: the distinct vertices written, over all runs)
    static std::vector<uint32_t> rawOf; // (GXV_DLVCACHE: each run's vertices before welding)
    rawOf.clear();
    uint32_t runRaw = 0; // (welding: the run's vertices before welding; the runs are cut by these, as without it)
    size_t block = 0;    // (welding: where the run's block of distinct vertices starts in the new buffer, bytes)
    uint32_t base = 0;   // (welding: the number its first vertex has: absolute indices count from the buffer's start, relative ones from the block)
    uint8_t* const outData = weld ? up.vtx.data() : nullptr;
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
                block = outPos; // (the run's block of distinct vertices starts here)
                base = abs ? static_cast<uint32_t>(weldedVerts) : 0u;
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
            for (uint32_t i = 0; i < sg.count; ++i, v += segStride) slotOf[i] = static_cast<uint16_t>(base + welder.Find(outData, outPos, block, v, segStride));
            for (uint32_t i = 0; i < n; ++i) ix[i] = slotOf[ix[i]];
            runRaw += sg.count;
            rawOf.resize(e.runs.size());
            rawOf.back() = runRaw;
            const uint32_t runUnique = welder.nextSlot;
            weldedVerts += runUnique - r->vtxCount;
            r->vtxCount = runUnique;
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
    if (weld) up.vtx.resize(outPos);
    if (weld && VcacheMode()) VcacheRuns(e, up, rawOf); // (GXV_DLVCACHE: after every run's own indices)
    if ((OcclCullMode() || GeoPassMode()) && !e.vcd[0]) {
        // (GXV_OCCLCULL: each rigid triangle run's surface; the vertices are the buffer's: welded into up.vtx, or still e.verts)
        const std::vector<uint8_t>& vb = weld ? up.vtx : e.verts;
        e.occ.assign(e.runs.size(), OccShape{});
        for (size_t i = 0; i < e.runs.size(); ++i) {
            Run& r = e.runs[i];
            if (r.primKind != PRIM_TRIANGLES) continue;
            const uint32_t rs = MakeLayout(r.layoutMask).stride;
            OcclAnalyse(e.occ[i], vb.data(), vb.size(), rs, abs ? 0u : r.vtxByteOffset, up.idx.data() + r.idxOffset, r.idxCount);
            r.occ = e.occ[i].analysed ? 1 : 0;
        }
    }
    if (ClusterMode() != 0 && StaticCullEnabled() && !e.vcd[0] && !g_keepCopy) BuildClusters(e, weld ? up.vtx : e.verts, up.idx, abs); // (GXV_CLUSTERCULL)
    if (Ta27Mode() != 0 && !e.vcd[0] && !g_keepCopy) BuildTa27(e, weld ? up.vtx : e.verts, up.idx, abs); // (GXV_TA27: census sets)
    const bool face = FaceCullMode() != 0 && !e.vcd[0] && !g_keepCopy && !e.copyable && e.instWant != 1; // (GXV_FACECULL: rigid runs of plain static lists)
    if (face) FacePlanes(e, weld ? up.vtx : e.verts, up.idx);
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
    // (GXV_VTXPACK: the runs' vertices in their compact form where that is exact; not for a buffer whose bytes are kept or copied as floats)
    if (PackSwitch() && !g_keepCopy && !e.copyable && e.instWant != 1) {
        PackRuns(e, up);
        e.bufBytes = static_cast<uint32_t>(up.vtx.size() + up.idx.size() * 2);
    }
    if (face && !e.face.empty()) FaceVerts(e, up.vtx); // (GXV_FACECULL: the vertex blocks as they are uploaded)
    // (GXV_DL_DEDUP: the same bytes and run table as a live buffer: that buffer is this entry's too)
    DedupKey ddKey;
    const bool ddOn = DedupMode() != 0 && !g_keepCopy && !e.copyable && !face && !up.vtx.empty() && !up.idx.empty();
    if (ddOn) {
        const XXH128_hash_t hv = XXH3_128bits(up.vtx.data(), up.vtx.size());
        const XXH128_hash_t hi = XXH3_128bits(up.idx.data(), up.idx.size() * sizeof(uint16_t));
        uint32_t rt[9];
        XXH128_hash_t hr = XXH3_128bits(nullptr, 0);
        for (const Run& r : e.runs) {
            rt[0] = r.primKind; rt[1] = r.layoutMask; rt[2] = r.vtxByteOffset; rt[3] = r.vtxFirst; rt[4] = r.vtxCount;
            rt[5] = r.idxOffset; rt[6] = r.idxCount; rt[7] = r.posMtxMax; rt[8] = r.prims;
            hr = XXH3_128bits_withSeed(rt, sizeof rt, hr.low64 ^ (hr.high64 * 0x9E3779B97F4A7C15ull));
        }
        ddKey.h[0] = hv.low64; ddKey.h[1] = hv.high64; ddKey.h[2] = hi.low64; ddKey.h[3] = hi.high64; ddKey.h[4] = hr.low64; ddKey.h[5] = hr.high64;
        ddKey.vtxBytes = static_cast<uint32_t>(up.vtx.size());
        ddKey.idxCount = static_cast<uint32_t>(up.idx.size());
        ddKey.nRuns = static_cast<uint32_t>(e.runs.size());
        const auto it = g_dedupByKey.find(ddKey);
        if (it != g_dedupByKey.end() && DedupRecOf(it->second)) {
            if (DedupMode() == 1) {
                const uint32_t sid = it->second;
                g_bufOwner[e.bufId] = nullptr; // (the id taken above was never sent to the back end)
                g_freeIds.push_back(e.bufId);
                e.bufId = sid;
                if (++g_dedupRec[sid].refs == 2) ++g_dd.liveShared;
                ++g_dd.hits;
                g_dd.hitBytes += e.bufBytes;
                if (e.instWant == 1) MakeInstBuffer(e, up); // (GXV_STATIC_INST: shares the buffer's instance buffer, or makes it from these bytes)
                return;
            }
            ++g_dd.wouldHits;
            g_dd.wouldBytes += e.bufBytes;
        }
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
    if (ddOn && g_dedupByKey.find(ddKey) == g_dedupByKey.end()) { // (mode 2 registers too: the census needs the keys; nothing is ever shared then)
        if (g_dedupRec.size() <= e.bufId) g_dedupRec.resize(static_cast<size_t>(e.bufId) + 64);
        g_dedupRec[e.bufId] = DedupRec{1, e.bufBytes, ddKey};
        g_dedupByKey.emplace(ddKey, e.bufId);
        ++g_dd.made;
    }
    if (e.instWant == 1) MakeInstBuffer(e, buf.bufUploads.back()); // (GXV_STATIC_INST: rebuilt for one)
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
    g_buildWhy = BrOk;
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
    if (e.genList == kNotGuest) { g_buildWhy = BrNotGuestList; return false; }
    // If a new generation was observed, its source write must precede the hash and decode.
    std::atomic_thread_fence(std::memory_order_acquire);
    e.hash = XXH3_64bits(list, nbytes);
    uint64_t vertexBytes = 0;
    const Plan* plans[8] = {};
    for (int i = 0; i < 12; ++i) { e.arr[i].lo = 0xFFFFFFFFu; e.arr[i].hi = 0; }
    while (p < end) {
        const uint8_t cmd = *p++;
        if (cmd == 0x00) continue;
        if (!(cmd & 0x80)) { g_buildWhy = BrNotDraw; g_buildCmd = cmd; return false; } // register loads, nested lists: not a pure geometry list
        if (end - p < 2) { g_buildWhy = BrCut; return false; }
        const uint32_t count = be16(p);
        p += 2;
        const int fmt = cmd & 7;
        if (!plans[fmt]) plans[fmt] = &CurrentPlan(fmt);
        const Plan& plan = *plans[fmt];
        if (plan.streamSize == 0) { g_buildWhy = BrNoPlan; return false; }
        const uint64_t used = static_cast<uint64_t>(count) * plan.streamSize;
        if (used > static_cast<uint64_t>(end - p)) { g_buildWhy = BrTruncated; return false; } // truncated: let the general parser deal with it
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
            if (vertexBytes > 0xFFFFFFFFu) { g_buildWhy = BrVertBytes; return false; }
            bool complete = true;
            const ArraySpans sp = MeasureArraySpans(plan, p, count, g_boundsGuard ? &complete : nullptr);
            // Fallback-zero elements are absent from the spans, so they cannot safely define a cache entry.
            if (g_boundsGuard && !complete) { g_buildWhy = BrBounds; return false; }
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
            if (e.arr[i].gen == kNotGuest) { g_buildWhy = BrArrNotGuest; return false; }
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
        if (CharaAAMode() || EdgeAAMode() || StaticCullEnabled() || OcclCullMode()) RowBoxesOf(e.verts.data() + s.offset, s.count, s.stride, e.rowBoxes); // (GXV_CHARAA, GXV_EDGE_AA's HUD bounds, GXV_STATICCULL, GXV_OCCLCULL)
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
    if (e.segs.empty()) g_buildWhy = BrNoSegs;
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

// ---- instance buffers (GXV_STATIC_INST, fe23c; the record stage's side is vgx_draw.cpp "static instances")
// A list whose static run is drawn several times in a row (nothing handed to the record stage between two uses: the plants of a flower
// group, drawn by DrawBufferShapeDrawer with one material list) in kInstFrames different frames is built once more (its entry is marked
// raced: the next use decodes it again and copies it into the arena, the use after promotes it), and the promotion then also makes an
// instance buffer: the run's vertices K times (K = StaticInstSlots(), the palette's slots), copy k's position w set to 3k (the palette
// row the palette twin shader reads; the run's own shader does not read w), and the run's indices K times, copy k's + k * the run's
// vertices, counted from the buffer's start (vtxFirst 0, vtxOffset 0: right with absolute and with relative indices). Uses of the entry
// then name it (SetPendingStaticInst) and the record stage may draw n consecutive uses as copies 0..n-1. The buffer goes with the
// static buffer (ReleaseBuffer: rebuilt, freed, swept). Only entries with one triangle run of at most GXV_STATIC_INST_VERTS vertices
// (default 512; K copies must fit 16-bit indices), and at most GXV_STATIC_INST_KB of instance buffers in all (default 3072).
//   GXV_STATIC_INST=<n> (read once): 0 off (default); n >= 1 on, n = the frames with repeated uses a list needs (1 and 2 = 2 frames).
// (GXV_DL_DEDUP) one instance buffer per static buffer: static buffer id -> instance buffer id, and per instance buffer: entries using it, bytes, parent
std::map<uint32_t, uint32_t> g_instOfBuf;
struct InstRec { uint32_t refs = 0, bytes = 0, parent = 0; };
std::vector<InstRec> g_instRec;
Entry* g_instPrevE = nullptr;     // the entry whose runs were emitted last, and DlJobSeq() after them
uint32_t g_instPrevEnd = ~0u;
uint32_t g_instPrevBuf = 0;       // (GXV_DL_DEDUP) the static buffer that entry used: the next entry's use of the same buffer follows it as an instance of the same mesh
uint64_t g_instBytes = 0;          // all instance buffers
struct InstCounters { uint64_t lists = 0, rebuilds = 0, refused = 0, made = 0; } g_ic;
uint32_t InstMaxVerts() { return static_cast<uint32_t>(VGX_KNOB(static_cast<uint32_t>(std::max<long>(3, EnvInt("GXV_STATIC_INST_VERTS", 512))))); }
uint64_t InstBudget() { return static_cast<uint64_t>(VGX_KNOB(static_cast<uint32_t>(std::max<long>(0, EnvInt("GXV_STATIC_INST_KB", 3072))))) << 10; }
uint32_t InstFrames() { return static_cast<uint32_t>(VGX_KNOB(static_cast<uint32_t>(std::max<long>(2, EnvInt("GXV_STATIC_INST", 0))))); }
// Whether the entry can have an instance buffer (and its size then).
bool InstFits(const Entry& e, uint64_t* bytes) {
    const uint32_t K = StaticInstSlots();
    if (!K || e.runs.size() != 1 || g_keepCopy) return false;
    const Run& r = e.runs[0];
    if (r.primKind != PRIM_TRIANGLES || r.vtxCount == 0 || r.vtxCount > InstMaxVerts() || static_cast<uint64_t>(r.vtxCount) * K > 0x10000u) return false;
    const uint64_t b = static_cast<uint64_t>(r.vtxCount) * MakeLayout(r.layoutMask).stride * K + static_cast<uint64_t>(r.idxCount) * 2u * K;
    if (bytes) *bytes = b;
    return g_instBytes + b <= InstBudget();
}
// A use of the entry (static buffer, not a learned copy) that follows a use of it with no job between: count it.
void InstNote(Entry& e) {
    if (e.instBuf || e.instWant) return;
    const uint32_t frame = static_cast<uint32_t>(CurrentFrame());
    if (e.instFrame == frame) return;
    e.instFrame = frame;
    if (++e.instSeen < InstFrames()) return;
    if (DedupMode() == 1 && e.bufId && DedupRecOf(e.bufId) && e.runs.size() == 1) {
        // (GXV_DL_DEDUP: another entry of this buffer has the instance buffer: the same bytes and run, so it is this entry's too, without a rebuild; the marks of the draw replay
        // and of the retained records move as for a rebuild)
        const auto f = g_instOfBuf.find(e.bufId);
        if (f != g_instOfBuf.end() && f->second < g_instRec.size() && g_instRec[f->second].refs) {
            e.instBuf = f->second;
            e.runs[0].instBuf = f->second;
            e.instBytes = 0;
            ++g_instRec[f->second].refs;
            ++g_dd.instShared;
            ++e.rpStamp;
            return;
        }
    }
    if (!InstFits(e, nullptr)) { e.instWant = 2; ++g_ic.refused; return; }
    // (built again: the next use decodes the list into the arena, the one after promotes it with the instance buffer; the marks of the
    // draw replay and of the retained records move, so that both take the normal path to the rebuild)
    e.instWant = 1;
    e.raced = true;
    ++e.rpStamp;
    ++g_ic.rebuilds;
}
void MakeInstBuffer(Entry& e, const BufUpload& up) {
    uint64_t bytes = 0;
    if (DedupMode() == 1 && e.bufId && DedupRecOf(e.bufId) && e.runs.size() == 1) {
        const auto f = g_instOfBuf.find(e.bufId);
        if (f != g_instOfBuf.end() && f->second < g_instRec.size() && g_instRec[f->second].refs) { // (the buffer's other entries made it: the same run, the same bytes)
            e.instBuf = f->second;
            e.runs[0].instBuf = f->second;
            e.instBytes = 0;
            ++g_instRec[f->second].refs;
            ++g_dd.instShared;
            return;
        }
    }
    if (!InstFits(e, &bytes)) { e.instWant = 2; ++g_ic.refused; return; }
    const uint32_t K = StaticInstSlots();
    const Run& r = e.runs[0];
    const uint32_t stride = MakeLayout(r.layoutMask).stride, V = r.vtxCount, I = r.idxCount;
    const uint32_t base = AbsoluteIndices() ? r.vtxFirst : 0u; // (what the run's indices count from)
    if (static_cast<size_t>(r.vtxByteOffset) + static_cast<size_t>(V) * stride > up.vtx.size() || static_cast<size_t>(r.idxOffset) + I > up.idx.size()) {
        e.instWant = 2; ++g_ic.refused; return;
    }
    for (uint32_t i = 0; i < I; ++i) {
        const uint32_t x = up.idx[r.idxOffset + i];
        if (x < base || x - base >= V) { e.instWant = 2; ++g_ic.refused; return; } // (an index outside the run's block: not replicable as it is)
    }
    BufUpload ib;
    ib.bufId = AllocBufId(&e);
    ib.vtx.resize(static_cast<size_t>(V) * stride * K);
    ib.idx.resize(static_cast<size_t>(I) * K);
    const uint8_t* const src = up.vtx.data() + r.vtxByteOffset;
    for (uint32_t k = 0; k < K; ++k) {
        uint8_t* d = ib.vtx.data() + static_cast<size_t>(k) * V * stride;
        std::memcpy(d, src, static_cast<size_t>(V) * stride);
        const float w = static_cast<float>(3 * k);
        for (uint32_t v = 0; v < V; ++v) std::memcpy(d + static_cast<size_t>(v) * stride + 12, &w, 4);
        uint16_t* ix = ib.idx.data() + static_cast<size_t>(k) * I;
        for (uint32_t i = 0; i < I; ++i) ix[i] = static_cast<uint16_t>(up.idx[r.idxOffset + i] - base + k * V);
    }
    e.instBuf = ib.bufId;
    e.runs[0].instBuf = ib.bufId;
    e.instBytes = static_cast<uint32_t>(bytes);
    if (DedupMode() == 1 && e.bufId && DedupRecOf(e.bufId)) {
        if (g_instRec.size() <= ib.bufId) g_instRec.resize(static_cast<size_t>(ib.bufId) + 64);
        g_instRec[ib.bufId] = InstRec{1, static_cast<uint32_t>(bytes), e.bufId};
        g_instOfBuf[e.bufId] = ib.bufId;
    }
    g_instBytes += bytes;
    ++g_ic.lists;
    ++g_ic.made;
    stats::Frame& fr = stats::Cur();
    ++fr.bufUploads;
    fr.bufUploadBytes += bytes;
    CmdBuf& buf = Pending();
    buf.bufUploads.push_back(std::move(ib));
    buf.cmds.push_back(Cmd{CMD_BUF_UPLOAD, static_cast<uint32_t>(buf.bufUploads.size() - 1)});
}
// Every fifth sweep (~600 pictures that called lists), one "static-inst:" line (port_log_summary, every build) while the switch is on.
void InstReport(uint64_t frame) {
    static uint64_t prevFrame = 0, prev[3] = {};
    static uint32_t sweeps = 0;
    if (++sweeps % 5 != 0 || !port_log_summary) return;
    uint64_t r[3];
    StaticInstCounters(r);
    const double p = frame > prevFrame ? static_cast<double>(frame - prevFrame) : 1.0;
    port_log_summary("static-inst: %llu pictures; lists with an instance buffer %llu (%.0f KB), made %llu, rebuilds asked %llu, lists refused %llu; per picture: "
                     "runs of instances %.1f, draws joined %.1f (GL draws saved), joins refused %.1f",
                     static_cast<unsigned long long>(frame - prevFrame), static_cast<unsigned long long>(g_ic.lists), g_instBytes / 1024.0, static_cast<unsigned long long>(g_ic.made),
                     static_cast<unsigned long long>(g_ic.rebuilds), static_cast<unsigned long long>(g_ic.refused), (r[0] - prev[0]) / p,
                     (r[1] - prev[1]) / p, (r[2] - prev[2]) / p);
    prevFrame = frame;
    for (int i = 0; i < 3; ++i) prev[i] = r[i];
}
void ReleaseInstBuffer(Entry& e) {
    if (!e.instBuf) return;
    if (e.instBuf < g_instRec.size() && g_instRec[e.instBuf].refs) { // (GXV_DL_DEDUP: shared by the entries of one static buffer)
        InstRec& r = g_instRec[e.instBuf];
        if (r.refs > 1) {
            --r.refs;
            if (g_bufOwner[e.instBuf] == &e) g_bufOwner[e.instBuf] = nullptr;
            e.instBuf = 0;
            if (!e.runs.empty()) e.runs[0].instBuf = 0;
            e.instBytes = 0;
            e.instWant = 1;
            return;
        }
        g_instOfBuf.erase(r.parent);
        e.instBytes = r.bytes; // (the last one pays what the buffer took)
        r.refs = 0;
    }
    Pending().cmds.push_back(Cmd{CMD_BUF_FREE, e.instBuf});
    g_bufOwner[e.instBuf] = nullptr;
    g_freeIds.push_back(e.instBuf);
    g_instBytes -= e.instBytes;
    --g_ic.lists;
    e.instBuf = 0;
    if (!e.runs.empty()) e.runs[0].instBuf = 0;
    e.instBytes = 0;
    e.instWant = 1; // (rebuilt or freed: a rebuild makes it again, while it fits)
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
        const bool inst = !learn && StaticInstSlots() != 0; // (GXV_STATIC_INST)
        if (inst && (g_instPrevE == &e || (g_instPrevBuf && g_instPrevBuf == e.bufId && DedupRecOf(e.bufId))) && g_instPrevEnd == DlJobSeq()) InstNote(e); // (the buffer's other entries count: GXV_DL_DEDUP)
        for (const Run& r : e.runs) {
            if (inst) SetPendingStaticInst(r.instBuf);
            SetPendingPosMtxMax(r.posMtxMax);
            if (!e.rowBoxes.empty()) SetPendingBounds(e.rowBoxes.data(), static_cast<uint32_t>(e.rowBoxes.size()));
            if (r.idxAlt != ~0u) SetPendingIdxAlt(r.idxAlt, r.idxAltCount); // (GXV_DLVCACHE)
            if (!e.face.empty() && !learn && !r.instBuf) { // (GXV_FACECULL: not a learned copy, not a run with an instance buffer)
                facecull::Run& fc = e.face[static_cast<size_t>(&r - e.runs.data())];
                if (!fc.vtx.empty()) SetPendingFace(&fc);
            }
            if (r.clN) SetPendingClusters(e.clusters.data() + r.clBeg, r.clN); // (GXV_CLUSTERCULL)
            if (!e.t27.empty()) SetPendingTa27(&e.t27[static_cast<size_t>(&r - e.runs.data())]); // (GXV_TA27)
            if (r.vqK) SetPendingVq(r.vqK); // (GXV_VTXPACK: the matrices the run's packed attributes are scaled through)
            if (r.occ) SetPendingOccluder(&e.occ[static_cast<size_t>(&r - e.runs.data())]); // (GXV_OCCLCULL / GXV_GEOPASS)
            EmitStaticRun(r.primKind, r.layoutMask, r.vtxCount, r.idxCount, r.prims, e.bufId, abs ? 0 : r.vtxByteOffset, r.idxOffset, r.vtxFirst);
            SetPendingBounds(nullptr, 0);
            fr.primsDL += r.prims;
            fr.vertsDL += r.vtxCount;
        }
        if (learn) SetPendingStaticCopyable(false);
        if (inst) {
            SetPendingStaticInst(0);
            g_instPrevE = &e;
            g_instPrevBuf = e.bufId;
            g_instPrevEnd = DlJobSeq();
        }
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
            g_dynCenList = 1; // (GXV_DYN_CENSUS: this draw is a cached list's)
            EmitDraw(s.prim, s.layoutMask, s.count, 0xFFFFFFFFu, deferred ? src : nullptr);
            g_dynCenList = 0;
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
    if (g_instPrevE == e) { g_instPrevE = nullptr; g_instPrevBuf = 0; } // (GXV_STATIC_INST)
    RpBump();
    for (Use& u : g_uses)
        if (u.e == e) u.e = nullptr;
    g_keptBytes -= e->keptBytes;
    if (e->copyable) --g_lc.keptLists;
    ReleaseBuffer(*e);
    g_ctr.liveBytes -= e->verts.capacity();
    delete e;
}

// ---- GXV_DL_KEEP_MIB (strk26; after the Strikers port's MSC_DL_KEEP) and the census of rebuilt lists
// Every entry unused for kRetainFrames use-frames is let go (Sweep), however much memory there is. The Strikers port measured what that costs a scene
// that comes back (a goal's celebration after 600+ pictures: 136 of the 151 lists built in one picture had been built and swept before, a picture
// of 4 times the front-end work). Galaxy returns to an area or an actor type the same way (a galaxy's other planets, a boss phase, a pipe back to
// the start). Nobody has counted it here, so the census does: the swept lists are remembered by address (4096 slots), and a build of a list that is in
// the table is a rebuild after a sweep (count, and how many pictures it had been gone).
//   GXV_DL_KEEP_MIB=<n> (default 0 = as before): the entries that are due are KEPT while they add up to no more than n MiB (decoded vertices in the heap
//                       plus the bytes of their static buffers); over the cap the oldest due entries go, at most 24 a sweep (a sweep is every 120 frames),
//                       never all at once. An entry kept is judged by its generations at its next call like any other: what is drawn does not change,
//                       only whether a list is decoded at the call or found.
//   GXV_DL_KEEP_LOG=1   (default 1; log only) the "dl-keep:" line in the basic log (every 5th sweep that let go or kept something): lists swept, kept,
//                       rebuilt after a sweep, the pictures they had been gone, the bytes due and the cap.
constexpr int kKeepEvictPerSweep = 24;
struct KeepStats {
    uint64_t swept = 0, keptNow = 0, keptBytes = 0, dueBytes = 0, rebuilt = 0, rebuiltGap = 0, rebuiltGapMax = 0, tableFull = 0, sweeps = 0, lastLogged = 0;
};
KeepStats g_keep;
struct SweptSlot { const uint8_t* list; uint64_t frame; };
constexpr size_t kSweptSlots = 4096;
SweptSlot g_sweptTab[kSweptSlots];
inline uint64_t KeepCapBytes() {
    static const uint64_t cap = [] { const long mb = EnvInt("GXV_DL_KEEP_MIB", 0); return mb > 0 ? static_cast<uint64_t>(mb) << 20 : 0ull; }();
    return cap;
}
inline bool KeepLogOn() {
    static const bool on = EnvInt("GXV_DL_KEEP_LOG", 1) != 0;
    return on;
}
inline size_t SweptIndex(const uint8_t* list) { return static_cast<size_t>((reinterpret_cast<uintptr_t>(list) >> 4) * 2654435761u) & (kSweptSlots - 1); }
void SweptNote(const uint8_t* list, uint64_t age) {
    for (size_t i = 0, k = SweptIndex(list); i < 8; ++i, k = (k + 1) & (kSweptSlots - 1)) {
        if (!g_sweptTab[k].list || g_sweptTab[k].list == list) { g_sweptTab[k] = SweptSlot{list, age}; return; }
    }
    ++g_keep.tableFull;
    g_sweptTab[SweptIndex(list)] = SweptSlot{list, age}; // (the oldest of 8 neighbours is not searched for: the census is a count, not a ledger)
}
// A list is being built (not a re-validation): was it swept?
void SweptBuilt(const uint8_t* list, uint64_t age) {
    for (size_t i = 0, k = SweptIndex(list); i < 8; ++i, k = (k + 1) & (kSweptSlots - 1)) {
        if (g_sweptTab[k].list == list) {
            const uint64_t gap = age - g_sweptTab[k].frame;
            ++g_keep.rebuilt;
            g_keep.rebuiltGap += gap;
            if (gap > g_keep.rebuiltGapMax) g_keep.rebuiltGapMax = gap;
            g_sweptTab[k].list = nullptr;
            return;
        }
        if (!g_sweptTab[k].list) return;
    }
}
inline uint64_t KeepEntryBytes(const Entry& e) {
    return static_cast<uint64_t>(e.verts.capacity()) + e.segs.capacity() * sizeof(Seg) + sizeof(Entry) + e.bufBytes;
}

// Drops entries that were not used for a long time (called once in a while).
void DedupReport() {
    static uint32_t sweeps = 0;
    if (++sweeps % 5 != 0 || !port_log_summary) return;
    port_log_summary("dl-dedup: GXV_DL_DEDUP=%d; shareable buffers made %llu, entries that took a live buffer %llu (%.0f KB not uploaded), census (mode 2) would have %llu (%.0f KB); "
                     "buffers now shared by 2 or more entries %u; instance buffers shared %llu; static buffers %llu (%.1f MB)",
                     DedupMode(), static_cast<unsigned long long>(g_dd.made), static_cast<unsigned long long>(g_dd.hits), g_dd.hitBytes / 1024.0,
                     static_cast<unsigned long long>(g_dd.wouldHits), g_dd.wouldBytes / 1024.0, g_dd.liveShared, static_cast<unsigned long long>(g_dd.instShared),
                     static_cast<unsigned long long>(g_ctr.staticBufs), g_ctr.staticBytes / 1048576.0);
}
void Sweep(uint64_t frame, uint64_t age) {
    if (StaticInstSlots()) InstReport(frame); // (GXV_STATIC_INST)
    if (DedupMode()) DedupReport(); // (GXV_DL_DEDUP)
    bool erasedAny = false;
    // (GXV_DL_KEEP_MIB: the due entries that stay: with a cap, the oldest are chosen to go when the due bytes are over it)
    std::vector<const Entry*> stay;
    const uint64_t cap = KeepCapBytes();
    uint64_t dueBytes = 0;
    if (cap) {
        std::vector<std::pair<uint64_t, const Entry*>> due;
        g_map.ForEach([&](const uint8_t*, ListInfo& li) {
            for (const Entry* e = li.head; e; e = e->next)
                if (age - e->lastUse > kRetainFrames) { due.emplace_back(e->lastUse, e); dueBytes += KeepEntryBytes(*e); }
        });
        std::sort(due.begin(), due.end(), [](const std::pair<uint64_t, const Entry*>& a, const std::pair<uint64_t, const Entry*>& b) { return a.first < b.first; });
        std::vector<dlkeep::Due> dueInfo;
        dueInfo.reserve(due.size());
        for (const auto& d : due) dueInfo.push_back(dlkeep::Due{d.first, KeepEntryBytes(*d.second)});
        const size_t goes = dlkeep::EvictCount(dueInfo.data(), dueInfo.size(), dueBytes, cap, static_cast<size_t>(kKeepEvictPerSweep));
        for (size_t i = goes; i < due.size(); ++i) stay.push_back(due[i].second);
        std::sort(stay.begin(), stay.end());
    }
    ++g_keep.sweeps;
    g_keep.keptNow = stay.size();
    g_keep.keptBytes = 0;
    for (const Entry* e : stay) g_keep.keptBytes += KeepEntryBytes(*e);
    g_keep.dueBytes = dueBytes;
    const bool census = KeepLogOn() || cap;
    g_map.EraseIf([&](const uint8_t* key, ListInfo& li) {
        Entry** link = &li.head;
        while (*link) {
            Entry* e = *link;
            if (age - e->lastUse > kRetainFrames && !(cap && std::binary_search(stay.begin(), stay.end(), static_cast<const Entry*>(e)))) {
                *link = e->next;
                if (census) { SweptNote(key, age); ++g_keep.swept; }
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
    if (census && port_log_summary && (g_keep.swept != 0 || cap) && g_keep.sweeps - g_keep.lastLogged >= 5 && (g_keep.swept || g_keep.rebuilt || g_keep.keptNow)) {
        g_keep.lastLogged = g_keep.sweeps;
        port_log_summary("dl-keep: GXV_DL_KEEP_MIB=%u: lists let go %u, kept past the age now %u (%u KiB), due bytes at the last sweep %u KiB; rebuilt after a sweep %u (gone %u pictures on average, longest %u); swept table full %u times",
                         static_cast<unsigned>(cap >> 20), static_cast<unsigned>(g_keep.swept), static_cast<unsigned>(g_keep.keptNow), static_cast<unsigned>(g_keep.keptBytes >> 10),
                         static_cast<unsigned>(g_keep.dueBytes >> 10), static_cast<unsigned>(g_keep.rebuilt),
                         static_cast<unsigned>(g_keep.rebuilt ? g_keep.rebuiltGap / g_keep.rebuilt : 0), static_cast<unsigned>(g_keep.rebuiltGapMax), static_cast<unsigned>(g_keep.tableFull));
    }
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

void DlDedupCounters(uint64_t out[8]) { out[0] = g_dd.made; out[1] = g_dd.hits; out[2] = g_dd.wouldHits; out[3] = g_dd.instShared; out[4] = g_dd.liveShared; out[5] = g_ctr.staticBufs; }
void DlStaticInstCounters(uint64_t out[5]) { out[0] = g_ic.lists; out[1] = g_instBytes; out[2] = g_ic.rebuilds; out[3] = g_ic.refused; out[4] = g_ic.made; } // (GXV_STATIC_INST)
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
// (heap23b: the scene-destroy census line) lists with entries now, and lists in the bypass table
void DlCacheListCounts(size_t& lists, size_t& bypassed) { lists = g_map.size(); bypassed = g_bypass.size(); }
// (GXV_FE_SPLIT: the branch counters, for the per-picture line of vgx_workload.h)
void DlCacheBranchCounts(uint64_t out[7]) {
    out[0] = g_ctr.hits; out[1] = g_ctr.rehashOk; out[2] = g_ctr.rehashChanged; out[3] = g_ctr.bypassed;
    out[4] = g_ctr.builds; out[5] = g_ctr.uncacheable; out[6] = g_ctr.promoted;
}
// (GXV_FE_SPLIT: the "list-other why" counters)
void DlOtherWhyCounts(uint64_t out[8]) { for (unsigned i = 0; i < OwCount; ++i) out[i] = g_otherWhy[i]; }
uint64_t DlCacheStaticBytes() { return g_ctr.staticBytes; }
void DlVcacheCounters(uint64_t out[6]) {
    out[0] = g_ctr.vcRuns; out[1] = g_ctr.vcTris; out[2] = g_ctr.vcRaw; out[3] = g_ctr.vcWelded; out[4] = g_ctr.vcOut; out[5] = g_ctr.vcSkipBudget;
}

// (GXV_DL_VARIANTS: the variant cap, and with GXV_DLCACHE=verify the tag walks checked against the plain walk and those that differed)
void DlVariantCounters(uint64_t out[3]) { out[0] = static_cast<uint64_t>(VariantCap()); out[1] = g_varVerify[0]; out[2] = g_varVerify[1]; }

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
    if (VariantCap() > kMaxVariants || g_varVerify[0])
        std::fprintf(f, "GXV_DL_VARIANTS=%d: variant walks checked against the plain walk (GXV_DLCACHE=verify) %llu, mismatching %llu\n", VariantCap(),
                     (unsigned long long)g_varVerify[0], (unsigned long long)g_varVerify[1]);
    if (g_ctr.weldLists)
        std::fprintf(f, "GXV_DLWELD: %llu promoted lists welded, vertices %llu -> %llu (%.1f%%)\n", (unsigned long long)g_ctr.weldLists,
                     (unsigned long long)g_ctr.weldIn, (unsigned long long)g_ctr.weldOut, 100.0 * (double)g_ctr.weldOut / (double)(g_ctr.weldIn ? g_ctr.weldIn : 1));
    if (VtxPackOn()) {
        uint64_t vp[7], vd[4];
        VtxPackCounters(vp);
        VtxPackDrawCounters(vd);
        std::fprintf(f, "GXV_VTXPACK: %llu lists packed, %llu of %llu runs (%llu vertices), their buffers %.1f -> %.1f MB; %llu lists refused for one stride "
                        "(GXV_ABSIDX=1); packed records %llu, blocks scaled %llu, tiny matrix elements %llu, keys refused %llu\n",
                     (unsigned long long)vp[0], (unsigned long long)vp[1], (unsigned long long)vp[2], (unsigned long long)vp[3], vp[4] / 1048576.0,
                     vp[5] / 1048576.0, (unsigned long long)vp[6], (unsigned long long)vd[0], (unsigned long long)vd[1], (unsigned long long)vd[2],
                     (unsigned long long)vd[3]);
    }
    if (g_ctr.vcRuns || g_ctr.vcSkipBudget || g_ctr.vcSkipGain)
        std::fprintf(f, "GXV_DLVCACHE=%d: %llu welded runs reordered (%llu triangles, %llu degenerate left out), %llu over the budget, %llu not worth it; "
                        "vertex shader runs (FIFO %u model) of those runs: unwelded %llu, welded %llu, reordered %llu (%.1f%% of unwelded)\n",
                     VcacheMode(), (unsigned long long)g_ctr.vcRuns, (unsigned long long)g_ctr.vcTris, (unsigned long long)g_ctr.vcDegen,
                     (unsigned long long)g_ctr.vcSkipBudget, (unsigned long long)g_ctr.vcSkipGain, VcacheSize(), (unsigned long long)g_ctr.vcRaw,
                     (unsigned long long)g_ctr.vcWelded, (unsigned long long)g_ctr.vcOut, 100.0 * (double)g_ctr.vcOut / (double)(g_ctr.vcRaw ? g_ctr.vcRaw : 1));
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
// GXV_RT_INST (fedl25, see "retained shapes"): 0 off (default); 1 on: a record may also be made of a call that hit a variant of the list other than
// the first (g_rtOutE, set where g_rpOutE is not), and a shape drawn with other vertex state than its record's (another instance of the model: its
// own arrays) gets a second record in a slot of its own; 2 verify (on, and GXV_RETAINED=2's check covers those records: the call always goes the
// normal way and a record that would have been a hit must be a hit of the same entry).
Entry* g_rtOutE = nullptr;
inline int RtInstMode() { return static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_RT_INST", 0)))); }
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
        const bool ahead = Cache24Pf(); // (GXV_CACHE24: the trace is read once from its start: hinted a few lines ahead of the loop)
        for (uint32_t i = 0; i < n; ++i) {
            if (ahead && (i & 1) == 0 && i + 8 < n) Pf24(&g_rpOld[i + 8]);
            g_rpMap[RpSlot(g_rpOld[i].list)] = i + 1;
        }
    }
    const uint32_t k = g_rpMap[RpSlot(list)];
    if (k && g_rpOld[k - 1].list == list && g_rpOld[k - 1].nbytes == nbytes) {
        ++g_rp.realigned;
        g_rpCur = k;
        return &g_rpOld[k - 1];
    }
    return nullptr;
}
// GXV_CACHE24: the previous picture's trace says which calls come next (the cursor is at the record of the next call). Hints, in three stages
// for the calls d + 2, d + 1 and d ahead (d = GXV_CACHE24_DIST), each from lines the stage before hinted a call ago: a call that was a cached hit
// (record's entry set, nothing freed since): the entry's head, then the runs and bounds;
// any other call (a material list, mostly): StateListAhead's three stages (front, entry, compiled delta). A record the picture no longer calls
// only hints lines nobody reads.
void TraceAhead() {
    const uint32_t n = static_cast<uint32_t>(g_rpOld.size()), c = g_rpCur, d = Cache24Dist();
    ++g_c24s.trAhead;
    if (c + d + 4 < n) Pf24(&g_rpOld[c + d + 4]); // (the trace itself, a line ahead of the reads)
    const bool slAhead = (Cache24() & 2) != 0;
    for (uint32_t stage = 1; stage <= 3; ++stage) {
        const uint32_t idx = c + d + 2 - stage; // stage 1: d + 2 ahead (record c + d + 1) ... stage 3: d ahead (record c + d - 1)
        if (idx >= n) continue;
        const RpRec& r = g_rpOld[idx];
        if (r.e) {
            if (r.epoch != g_rpEpoch) continue;
            if (stage == 2) { Pf24Lines(r.e, 128, 4); ++g_c24s.trGeo; }
            else if (stage == 3) {
                const Entry& e = *r.e; // (its head was hinted a call ago)
                Pf24Lines(e.runs.data(), static_cast<uint32_t>(e.runs.size() * sizeof(Run)), 4);
                Pf24Lines(e.rowBoxes.data(), static_cast<uint32_t>(e.rowBoxes.size() * sizeof(RowBox)), 2);
            }
        } else if (slAhead) { // (GXV_CACHE24 bit 2: the state-list stages; the model found them not worth their instructions, so they are optional)
            if (stage == 1) ++g_c24s.trSl;
            StateListAhead(r.list, static_cast<int>(stage));
        }
    }
}
// The age and the sweep, as CallListBody has them before its lookup.
inline uint64_t AgeAndSweep(uint64_t frame) {
    const uint64_t age = AgeOf(frame);
    if (age - g_lastSweep >= 120) { two::AGuard l; two::Bump(VGX2_EP_DL); g_lastSweep = age; Sweep(frame, age); } // (GXV_FE_TWOCORE: guard, epoch)
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
    if (e.fails) { two::AGuard l; e.fails = 0; } // (GXV_FE_TWOCORE: the second worker reads it; a store only where it changes)
    ++g_ctr.hits;
    ++stats::Cur().dlHit;
    g_ctr.hitVerts += e.totalVerts;
    if (!afterChange && !e.bufId && e.promotable && g_static) { two::AGuard l; Promote(e); }
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
    if (g_mode != 0 && (nbytes == 32 || nbytes == 64) && (reinterpret_cast<uintptr_t>(list) & 31u) == 0 && list[0] == 0x61 && !TraceFrame()) return false;
    // (GXV_TINYLIST: a list that begins with a state command goes to the state-list cache first)
    return !(TinyListMode() != 0 && g_mode != 0 && !TraceFrame() && TinyListCandidate(list[0]));
}
#if defined(VGX_CLONE_B)
// ------------------------------------------------------------------------------------------------ GXV_FE_TWOCORE: the second worker's lists
// (vgx_twocore.cpp) Its scan applies what a list does to the registers: a state list through its own state-list cache, any other list
// parsed with its draws left out (a list found to hold draws only is remembered with its write generation and skipped while that holds).
// In a segment it takes a state list as the player would (the state-list cache's replay is the parse's, exactly), or a hit of a
// promoted entry of the player's cache, read under the lock: the same tests as CallListBody's hit branch, then Emit's static branch
// from copies of the runs. Anything else ends the segment before the call (nothing has been changed then).
extern "C" uint64_t vgx2_b_list_state_cmds(void);
namespace {
// (a list of draws only, as it parsed with the vertex descriptor and the one vertex format it used: the parse, hence the list's skip, is
// a function of the bytes, the descriptor and that format)
struct PureList { const uint8_t* list = nullptr; uint32_t nbytes = 0; uint64_t gen = 0; uint8_t fmt = 0; uint8_t vcd[26] = {}; VtxAttrFmt vat[26] = {}; };
PureList* g_pure = nullptr; // (1024, made at B's first scan: nothing in the image while the switch is off)
inline PureList& PureSlot(const uint8_t* l) {
    if (!g_pure) g_pure = new PureList[1024];
    return g_pure[((static_cast<uint32_t>(reinterpret_cast<uintptr_t>(l)) >> 5) * 2654435761u) >> 22];
}
void ScanList(const uint8_t* list, uint32_t nbytes) {
    Init();
    if (StateListFastHit(list, nbytes)) return;
    SyncVertexArrays();
    const uint64_t gen = GuestWriteGeneration(list, nbytes);
    PureList& m = PureSlot(list);
    if (gen != kNotGuest && m.list == list && m.nbytes == nbytes && m.gen == gen && std::memcmp(m.vcd, g.vcd, sizeof m.vcd) == 0 &&
        std::memcmp(m.vat, g.vat[m.fmt], sizeof m.vat) == 0)
        return;
    if (StateListCall(list, nbytes)) return;
    const uint64_t before = vgx2_b_list_state_cmds();
    two::g_fmtSeen = 0;
    ProcessStream(list, nbytes, true);
    const uint32_t f = two::g_fmtSeen;
    if (gen != kNotGuest && vgx2_b_list_state_cmds() == before && f && !(f & (f - 1))) {
        m.list = list;
        m.nbytes = nbytes;
        m.gen = gen;
        m.fmt = static_cast<uint8_t>(__builtin_ctz(f));
        std::memcpy(m.vcd, g.vcd, sizeof m.vcd);
        std::memcpy(m.vat, g.vat[m.fmt], sizeof m.vat);
    }
}
// twocore25: a tiny list made of register writes only (BP but the triggers, CP, XF, indexed matrix loads, padding, the one-byte commands):
// J3D's small differed lists (J3DShapePacket::calcDifferedBufferSize: a material colour, 13 bytes, or a texture matrix, padded to 32 / 64
// bytes, starting with an XF write), one before nearly every shape. The player parses such a list inline (CallListBody's tiny branch:
// ProcessStream); B parses it the same way. Before, every one of them ended B's segment ("tiny"): on the console 4.3 of 5.2 segment ends a
// picture, and since a segment begins at a list call, most segments ended at their first command, empty.
bool TinyStateOnly(const uint8_t* l, uint32_t n) {
    const uint8_t* p = l;
    const uint8_t* const end = l + n;
    while (p < end) {
        const uint8_t c = *p;
        size_t len;
        if (c == 0x00 || c == 0x44 || c == 0x48) len = 1;
        else if (c == 0x08) len = 6;
        else if (c == 0x20 || c == 0x28 || c == 0x30 || c == 0x38) len = 5;
        else if (c == 0x61) {
            if (end - p < 5) return false;
            const uint8_t r = p[1];
            if (r == 0x45 || r == 0x47 || r == 0x48 || r == 0x52 || r == 0xFF) return false; // (draw done, tokens, EFB copies, markers)
            len = 5;
        } else if (c == 0x10) {
            if (end - p < 5) return false;
            len = 5u + ((((static_cast<uint32_t>(p[1]) << 8) | p[2]) & 0xFu) + 1u) * 4u;
        } else {
            return false; // (a draw, a call, an opcode the parse resynchronises on)
        }
        if (static_cast<size_t>(end - p) < len) return false;
        p += len;
    }
    return true;
}
extern "C" void vgx2_b_tiny_state(void); // (vgx_twocore_b.cpp: the log's count)
void DecodeList(const uint8_t* list, uint32_t nbytes) {
    Init();
    if (g_mode != 1 || TraceFrame() || CensusOn()) two::StopClean(1);
    if (nbytes <= TinyListBytes()) {
        // (CallListBody's tiny branch: a padded state list, else the player parses it)
        SyncVertexArrays();
        if ((nbytes == 32 || nbytes == 64) && (reinterpret_cast<uintptr_t>(list) & 31u) == 0 && list[0] == 0x61 && StateListCall(list, nbytes)) return;
        // (twocore25, GXV_FE_TWOCORE_TINY=1, default: register writes only: parsed as the player parses it; else a draw: the segment ends)
        if (two::TinyOn() && TinyStateOnly(list, nbytes)) {
            ProcessStream(list, nbytes, true);
            vgx2_b_tiny_state();
            return;
        }
        two::StopClean(2);
    }
    if (StateListFastHit(list, nbytes)) return;
    SyncVertexArrays();
    if (!IsGuestRam(list, nbytes)) two::StopClean(3);
    const uint64_t frame = CurrentFrame();
    const uint64_t age = vgx2_pic_age;
    PtrMap<ListInfo>& amap = *static_cast<PtrMap<ListInfo>*>(vgx2_dl_shared(0));
    PtrMap<Bypass>& abyp = *static_cast<PtrMap<Bypass>*>(vgx2_dl_shared(1));
    static std::vector<Run> runs;
    static std::vector<RowBox> boxes;
    uint32_t bufId = 0;
    bool drawnBefore = true;
    void* usedE = nullptr;
    int how = 0; // 0 stop, 1 hit, 2 bypassed, 3 (twocore24) B's copies need more room: made outside the lock, and the look-up again
    int why = 5;
    // (twocore24: nothing allocates while B holds the lock (an allocation can block): the copies' room is made outside it, the logs after it)
    void* rehashE = nullptr;
    uint64_t rehashGl = 0, rehashGa[12] = {};
    size_t needRuns = 0, needBoxes = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
    how = 0;
    why = 5;
    rehashE = nullptr;
    two::LockB();
    do {
        if (age - *static_cast<const uint64_t*>(vgx2_dl_shared(2)) >= 120) break; // (the player sweeps at this call)
        why = 6;
        if (!abyp.empty()) {
            const Bypass* b = abyp.Find(list);
            if (b && frame < b->until) { how = 2; break; }
        }
        ListInfo* li = amap.Find(list);
        if (!li) break;
        why = 7;
        Entry* e = li->head;
        const int cap = VariantCap();
        if (cap > kMaxVariants) {
            uint32_t tm = ~0u;
            uint16_t gt = 0;
            const bool tags = VGX_KNOB(EnvInt("GXV_DL_VARTAG", 1) != 0) != 0;
            for (; e; e = e->next) {
                if (e->nbytes != nbytes) continue;
                if (tags) {
                    const uint32_t m = (e->idxMask >> 9) & 0xFFFu;
                    if (m != tm) { tm = m; gt = StateTagOfRegs(m); }
                    if (e->stateTag != gt) continue;
                }
                if (SameState(*e)) break;
            }
        } else {
            while (e && !(e->nbytes == nbytes && SameState(*e))) e = e->next;
        }
        if (!e || e->raced) break;
        why = 8;
        uint64_t gl, ga[12] = {};
        const bool tracked = ReadGenerations(*e, gl, ga);
        bool same = tracked && gl == e->genList;
        for (uint32_t m = (e->idxMask >> 9) & 0xFFFu; m && same; m &= m - 1) same = ga[__builtin_ctz(m)] == e->arr[__builtin_ctz(m)].gen;
        if (!same && tracked) {
            // (CallListBody's "rehash": a write landed near what the entry reads, not on it. The entry's generations are stored by the
            // player when it takes the segment, as that branch stores them)
            std::atomic_thread_fence(std::memory_order_acquire);
            if (ContentHash(*e) != e->hash) break;
            why = 9;
            rehashE = e; // (two::NoteDlRehash after the lock)
            rehashGl = gl;
            std::memcpy(rehashGa, ga, sizeof rehashGa);
            same = true;
        }
        why = 9;
        if (!same || e->fails != 0 || !e->bufId || (e->copyable && g_learnMax != 0)) break; // (the player rebuilds, promotes or copies)
        if (runs.capacity() < e->runs.size() || boxes.capacity() < e->rowBoxes.size()) {
            needRuns = e->runs.size();
            needBoxes = e->rowBoxes.size();
            how = 3;
            break;
        }
        runs = e->runs;      // (in the room there: no allocation)
        boxes = e->rowBoxes;
        bufId = e->bufId;
        // (GXV_STATICCULL: the entry's "drawn once" flag, which the first use sets: set by an earlier use in this segment, or set here
        // and handed to the player, who stores it in the entry)
        if (!boxes.empty() && !boxes[0].drawn && two::DlDrawnHere(e)) boxes[0].drawn = 1;
        drawnBefore = boxes.empty() || boxes[0].drawn;
        usedE = e;
        how = 1;
    } while (false);
    vgx2_unlock_b();
    if (how != 3) break;
    runs.reserve(needRuns + 8);
    boxes.reserve(needBoxes + 8);
    }
    if (how == 3) two::StopClean(why); // (the entry kept growing: the player does the list)
    if (how == 1) {
        if (rehashE) two::NoteDlRehash(rehashE, rehashGl, rehashGa);
        two::NoteDlUse(usedE);
    }
    if (how == 2) {
        if (StateListCall(list, nbytes)) return;
        two::StopClean(4);
    }
    if (how != 1) two::StopClean(why);
    // ---- Emit's static branch (GXV_STATIC_INST and the learned copies are off: vgx_draw.cpp vgx2_a_draw_usable)
    stats::Frame& fr = stats::Cur();
    ++fr.streams;
    fr.streamBytes += nbytes;
    const bool abs = AbsoluteIndices();
    for (const Run& r : runs) {
        SetPendingPosMtxMax(r.posMtxMax);
        if (!boxes.empty()) SetPendingBounds(boxes.data(), static_cast<uint32_t>(boxes.size()));
        if (r.idxAlt != ~0u) SetPendingIdxAlt(r.idxAlt, r.idxAltCount); // (GXV_DLVCACHE)
        EmitStaticRun(r.primKind, r.layoutMask, r.vtxCount, r.idxCount, r.prims, bufId, abs ? 0 : r.vtxByteOffset, r.idxOffset, r.vtxFirst);
        SetPendingBounds(nullptr, 0);
        fr.primsDL += r.prims;
        fr.vertsDL += r.vtxCount;
    }
    if (!drawnBefore && boxes[0].drawn) two::NoteDlDrawn(usedE);
    two::NoteStaticRuns(static_cast<uint32_t>(runs.size()));
}
} // namespace
#endif
// GXV_J3DNATIVE (vgx_j3native.inc): whether CallList(list, nbytes) would send a state list to CallListBody's StateListFastHit, the call a
// bracket's skip replaces: the geometry cache is on, the list is not a tiny one, nothing traces this picture. (The draw replay's trace
// lookup before it finds no entry for a state list: the record is made with no entry, and a missing one is a plain miss.)
bool J3nListPathOk(uint32_t nbytes) {
    Init();
    return g_mode == 1 && nbytes > TinyListBytes() && !TraceFrame();
}
void CallList(const uint8_t* list, uint32_t nbytes) {
#if defined(VGX_CLONE_B)
    if (two::g_prescan) ScanList(list, nbytes);
    else DecodeList(list, nbytes);
    return;
#endif
    // (GXV_FE_SPLIT: the branch taken decides the slot; FeOther until one is known)
    workload::FeScope cls(workload::FeOther);
    Init();
    if (RpMode() == 0 || g_mode != 1 || nbytes <= TinyListBytes() || TraceFrame()) {
        g_rpOutE = nullptr; // (GXV_RETAINED reads it after the call: set by CallListBody's hits and builds only)
        CallListBody(list, nbytes, cls);
        return;
    }
    // ---- draw replay
    RpFrame();
    ++g_rp.calls;
    const RpRec* const found = RpFind(list, nbytes);
    if (Cache24Pf()) TraceAhead();
    if (const RpRec* r = found) {
        if (r->e && r->epoch == g_rpEpoch && r->e->rpStamp == r->stamp) {
            const RpRec rec = *r;
            if (RpMode() == 2 && g_rpCur < g_rpOld.size() && g_rpOld[g_rpCur].e && g_rpOld[g_rpCur].epoch == g_rpEpoch) RpHintHead(g_rpOld[g_rpCur].e);
            if (RpReplay(rec, nbytes, cls)) {
                ++g_rp.hits;
                g_rpNew.push_back(rec);
                g_rpOutE = rec.e; // (GXV_RETAINED: the entry the call was a hit of)
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
// (feplan31, GXV_FEPLAN_CENSUS) a list call's full parse, counted by the way it got here (vgx_state.h FePlanCensusParse); else ProcessStream
inline void ListParse(const uint8_t* list, uint32_t nbytes, uint8_t path) {
    if (__builtin_expect(FePlanCensusOn(), 0)) FePlanCensusParse(list, nbytes, path);
    else ProcessStream(list, nbytes);
}
// CallListBody's tiny branch (a list of at most GXV_DL_TINY bytes, or any list with the cache off or in a traced picture); returns the way it
// took (vgx_state.h kTw*, for GXV_TINYLIST_CENSUS).
static uint8_t TinyBranch(const uint8_t* list, uint32_t nbytes) {
    SyncVertexArrays();
    // Small padded material lists can reuse state ops; tiny draw lists still decode directly.
    if (g_mode != 0 && (nbytes == 32 || nbytes == 64) &&
        (reinterpret_cast<uintptr_t>(list) & 31u) == 0 && list[0] == 0x61 && !TraceFrame()) {
        if (StateListCall(list, nbytes)) return kTwPadReplay;
        ListParse(list, nbytes, nbytes <= TinyListBytes() ? 0 : 6);
        return kTwPadParsed;
    }
    // (tinylist31, GXV_TINYLIST: a list that begins with a state command, J3D's small differed lists, through the state-list cache too)
    if (const int tl = TinyListMode()) {
        if (g_mode != 0 && !TraceFrame() && TinyListCandidate(list[0])) {
            if (TinyListCall(list, nbytes, tl)) return tl == 2 ? kTwTlVerified : kTwTlReplay;
            ListParse(list, nbytes, nbytes <= TinyListBytes() ? 0 : 6);
            return kTwTlParsed;
        }
    }
    ListParse(list, nbytes, nbytes <= TinyListBytes() ? 0 : 6);
    return kTwParsed;
}
static void CallListBody(const uint8_t* list, uint32_t nbytes, workload::FeScope& cls) {
    Init();
    // (a list of a few dozen bytes, one particle quad for instance, is decoded faster than its
    // cache entry is checked)
    if (g_mode == 0 || nbytes <= TinyListBytes() || TraceFrame()) {
        if (cls.slot >= 0) cls.slot = workload::FeTiny;
        if (__builtin_expect(VGX_KNOB(static_cast<int>(EnvInt("GXV_TINYLIST_CENSUS", 0))) != 0, 0)) TinyListCensusCall(list, nbytes, &TinyBranch); // (GXV_TINYLIST_CENSUS: timed and sorted)
        else TinyBranch(list, nbytes);
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
        ListParse(list, nbytes, 1);
        return;
    }
    const uint64_t frame = CurrentFrame();
    const bool census = CensusOn();
    if (census && frame != g_census.lastFrame) CensusReport(frame);
    const uint64_t age = AgeOf(frame);
    if (age - g_lastSweep >= 120) { two::AGuard l; two::Bump(VGX2_EP_DL); g_lastSweep = age; Sweep(frame, age); }
    if (!g_bypass.empty()) {
        const Bypass* b = g_bypass.Find(list);
        if (b) {
            if (frame < b->until) {
                ++g_ctr.bypassed;
                ++stats::Cur().dlBypass;
                if (!StateListCall(list, nbytes)) {
                    ++g_otherWhy[OwBypassStream];
                    NoteBypassStream(nbytes);
                    if (census && port_host_time_ns) {
                        // (GXV_FE_CENSUS: the decode's time and cause, per list)
                        const uint8_t why = b->why < BwCount ? b->why : BwNone, variants = b->variants;
                        {   // (fedl25: why Build and StateListCall both refused this list)
                            uint8_t sc = 0;
                            const uint8_t sw = StateListWhyLast(&sc);
                            ++g_census.bypassSub[b->sub < BrCount ? b->sub : 0];
                            ++g_census.bypassSl[sw < 8 ? sw : 0];
                            g_census.bypassBytes += nbytes;
                            if (b->sub == BrNotDraw) ++g_census.subCmd[b->subCmd];
                            if (sw == 3 || sw == 5) ++g_census.slCmd[sc];
                        }
                        const uint8_t fpPath = why == BwUncacheable ? 2 : 3;
                        const int64_t t0 = port_host_time_ns();
                        ListParse(list, nbytes, fpPath); // (b may be gone now: the parse can call lists that change the map)
                        const uint64_t ns = static_cast<uint64_t>(port_host_time_ns() - t0);
                        ++g_census.bypassCalls[why];
                        g_census.bypassNs += ns;
                        if (CensusList* s = CensusSlot(list)) { s->nbytes = nbytes; ++s->calls; s->ns += ns; s->why = why; s->variants = variants; }
                    } else {
                        ListParse(list, nbytes, b->why == BwUncacheable ? 2 : 3);
                    }
                }  // (material lists: pre-parsed, vgx_state.cpp)
                else ++g_otherWhy[OwBypassStateReplay];
                return;
            }
            // expired: try caching again (the entry stays, it remembers the back-off level)
        }
    }
    MapFront& mf = g_mapFront[(reinterpret_cast<uintptr_t>(list) >> 5) & 255u];
    if (!(mf.li && mf.list == list)) {
        ListInfo* found = g_map.Find(list);
        if (!found) { two::AGuard l; found = &g_map[list]; } // (GXV_FE_TWOCORE: a new list in the map; the second worker finds no entry)
        mf.li = found;
        mf.list = list;
    }
    ListInfo& li = *mf.li;
    Entry* e = li.head;
    const int cap = VariantCap();
    if (cap > kMaxVariants || census) {
        // (GXV_DL_VARIANTS > 8, see "many variants": the same walk, an entry with another stateTag passed over without SameState; the
        // first entry that passes SameState is taken, as in the walk below. GXV_FE_CENSUS: how deep the walk went.)
        uint32_t tm = ~0u, depth = 0;
        uint16_t gt = 0;
        const bool tags = cap > kMaxVariants && VGX_KNOB(EnvInt("GXV_DL_VARTAG", 1) != 0) != 0; // (GXV_DL_VARTAG=0: the plain walk, for A/B)
        for (; e; e = e->next, ++depth) {
            if (e->nbytes != nbytes) continue;
            if (tags) {
                const uint32_t m = (e->idxMask >> 9) & 0xFFFu;
                if (m != tm) { tm = m; gt = StateTagOfRegs(m); }
                if (e->stateTag != gt) continue;
            }
            if (SameState(*e)) break;
        }
        if (tags && g_mode == 2) {
            // (GXV_DLCACHE=verify with GXV_DL_VARIANTS > 8: the plain walk must find the same entry)
            Entry* w = li.head;
            while (w && !(w->nbytes == nbytes && SameState(*w))) w = w->next;
            ++g_varVerify[0];
            if (w != e) {
                ++g_varVerify[1];
                VGX_LOG_N(32, "dl cache: MISMATCH variant walk: list %p (%u bytes): tag walk %p, plain walk %p\n", (const void*)list, nbytes, (void*)e, (void*)w);
            }
        }
        if (census && e) {
            ++g_census.walkHits;
            g_census.walkSteps += depth;
            if (depth >= static_cast<uint32_t>(kMaxVariants)) ++g_census.deepHits;
            if (depth > g_census.deepest) g_census.deepest = depth;
        }
    } else {
        while (e && !(e->nbytes == nbytes && SameState(*e))) e = e->next;
    }
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
            if (e->fails) { two::AGuard l; e->fails = 0; }
            ++g_ctr.hits;
            ++stats::Cur().dlHit;
            g_ctr.hitVerts += e->totalVerts;
            // Used again unchanged: worth a static buffer (no per-frame copy, no index generation).
            if (!afterChange && !e->bufId && e->promotable && g_static) { two::AGuard l; Promote(*e); }
            if (cls.slot >= 0) cls.slot = workload::FeGeomHit;
            if (e == li.head) g_rpOutE = e; // (GXV_DRAWREPLAY: recorded)
            else g_rtOutE = e;              // (GXV_RT_INST: a variant behind the first)
            Emit(*e, nbytes);
            return;
        }
        // (GXV_FE_TWOCORE: everything below changes the entry: under the lock. A change of its generations needs no epoch: the second
        // worker compares them with the guest's itself; what frees, rebuilds or bypasses it moves the epoch below)
        two::AGuard changing;
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
            else g_rtOutE = e;              // (GXV_RT_INST: a variant behind the first)
            Emit(*e, nbytes);
            return;
        }
        ++g_ctr.rehashChanged;
        if (++e->fails >= 3) {
            // Rewritten every time it is drawn (dynamic geometry): hashing and
            // storing it is wasted work. Decode directly for the next two seconds.
            two::Bump(VGX2_EP_DL);
            {
                Bypass& bp = g_bypass[list];
                SetBypass(bp, frame);
                bp.why = (tracked && gl != e->genList) ? BwRewrittenList : BwRewrittenArrays; // (GXV_FE_CENSUS)
                bp.variants = 0;
                for (const Entry* v = li.head; v; v = v->next) ++bp.variants;
            }
            ++g_otherWhy[OwRehashBypassSet];
            Unlink(li, e);
            if (!li.head) { g_map.Erase(list); MapFrontClear(); }
            ListParse(list, nbytes, 5);
            return;
        }
    } else {
        two::AGuard changing; // (GXV_FE_TWOCORE)
        two::Bump(VGX2_EP_DL);
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
                {
                    Bypass& bp = g_bypass[list];
                    SetBypass(bp, frame);
                    bp.why = BwStreak; // (GXV_FE_CENSUS)
                    bp.variants = static_cast<uint8_t>(n);
                }
                ++g_otherWhy[OwStreakBypassSet];
                FreeAll(li);
                g_map.Erase(list);
                MapFrontClear();
                ++g_ctr.bypassed;
                ++stats::Cur().dlBypass;
                ListParse(list, nbytes, 5);
                return;
            }
            if (census && static_cast<uint32_t>(n + 1) > g_census.mostVariants) g_census.mostVariants = static_cast<uint32_t>(n + 1);
            if (n >= cap) { // (GXV_DL_VARIANTS: kMaxVariants unless set)
                Unlink(li, lru);
                ++g_ctr.evicted;
            }
        } else {
            li.lastNewVariant = frame;
            li.streak = 1;
        }
        if (RtInstMode() != 0) { for (Entry* v = li.head; v; v = v->next) ++v->rpStamp; } // (GXV_RT_INST: a record of any variant is void: a new first one is walked before it)
        else if (li.head) ++li.head->rpStamp; // (GXV_DRAWREPLAY: no longer the first variant)
        e = new Entry();
        e->next = li.head;
        li.head = e;
    }
    // (Re)build. Snapshot each range before decoding it; the log also rejects
    // overlapping notifications completed while the build was in progress.
    two::AGuard building; // (GXV_FE_TWOCORE: the entry changes while the second worker may look at it)
    two::Bump(VGX2_EP_DL);
    const uint64_t epoch0 = GuestWriteEpoch();
    g_ctr.liveBytes -= e->verts.capacity();
    e->list = list;
    e->nbytes = nbytes;
    std::memcpy(e->vcd, g.vcd, 21);
    split::Settle(e->ticket); // (GXV_SPLIT_COPY: Build rewrites the vertices)
    // (endwait26, GXV_ENDWAIT_LOG) a display list decoded for the first time (or again): its time, for the front end's per-picture record
    static const bool ewOn = EnvInt("GXV_ENDWAIT_LOG", 0) != 0 && port_host_time_ns;
    const int64_t ewT0 = ewOn ? port_host_time_ns() : 0;
    const bool pure = Build(*e, list, nbytes);
    if (ewOn) {
        g_dlBuildNs.fetch_add(static_cast<uint64_t>(port_host_time_ns() - ewT0), std::memory_order_relaxed);
        g_dlBuildN.fetch_add(1, std::memory_order_relaxed);
    }
    e->stateTag = StateTagOf(*e); // (GXV_DL_VARIANTS: the walk's first test)
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
        g_bypass[list].why = BwUncacheable; // (GXV_FE_CENSUS)
        g_bypass[list].sub = g_buildWhy;
        g_bypass[list].subCmd = g_buildCmd;
        Unlink(li, e);
        if (!li.head) { g_map.Erase(list); MapFrontClear(); }
        if (!StateListCall(list, nbytes)) { ++g_otherWhy[OwUncacheableStream]; ListParse(list, nbytes, 4); }
        else ++g_otherWhy[OwUncacheableState];
        return;
    }
    e->lastUse = age;
    ++g_otherWhy[OwBuild];
    ++g_ctr.builds;
    if (g_keep.swept) SweptBuilt(list, age); // (GXV_DL_KEEP_LOG: a list swept before and built again)
    ++stats::Cur().dlBuild;
    g_ctr.builtVerts += e->totalVerts;
    if (e == li.head && !e->raced) g_rpOutE = e; // (GXV_DRAWREPLAY: recorded)
    Emit(*e, nbytes);
}

// ------------------------------------------------------------------------------------------------ retained shapes (GXV_RETAINED)
// The game's J3D shape draws (J3DShapeDraw::draw) send a handle, a number the shape got when it was made and that no other shape
// ever gets, in front of their CALL_DL (opcode 0x59, decomp/src/RVL_SDK/gx/GXDisplayList.c port_gx_call_retained). Here the handle
// names a record, kept from the shape's last call that the normal path took as a hit (or build) of a cache entry:
//   the list and its size, the entry, its rpStamp and g_rpEpoch then (the marks of the draw replay above: the entry is still the
//   list's first variant, not built again, and nothing was freed since), the vertex state then by the CP words it is decoded from
//   (the descriptor, the entry's one vertex format, its indexed arrays' bases and strides: what SameState compares, RtSameVertexState),
//   and the write generations of the list and of the spans of its arrays as the entry holds them, with the spans themselves.
// A call whose record matches (handle, list, size, both marks), whose CP words are the record's and whose generations are the
// record's goes straight to the hit branch (the same code as the normal path's: age, sweep, counters, promotion, Emit). That is what
// the normal path would have done: the generations unchanged mean the list's and the arrays' bytes are what the entry was made from
// (the entry keeps the generations it was last found unchanged with, and they only grow); the marks mean the list's first variant is
// this entry and the list is not bypassed (a list is put out of caching only together with freeing its entry); the vertex state
// equal means SameState holds; a list the entry's generations say is unchanged holds draw commands, so the state-list fast hit, which
// the normal path tries first, declines it (as for the draw replay). The game's handle only finds the record: every fact the call relies
// on is checked here, so a wrong or reused handle can only make a miss.
// What it saves against the draw replay: the trace lookup, and the entry's scattered lines that SameState and ReadGenerations read
// (the vertex descriptor, the format rows, the array uses: some six cache lines a call that the game's turn has pushed out); the
// record is one block, and the vertex state is read from the registers, which are in the cache (some 10-17 words compared, exactly:
// no hash). GXV_RETAINED=2 checks every would-be hit against SameState and the normal path.
//   GXV_RETAINED=0  (default) the handles are not sent (the game) and not used (here: a handle is then a plain CALL_DL)
//   GXV_RETAINED=1  on
//   GXV_RETAINED=2  verify: the call always goes the normal way (the picture is the normal path's); where the record would have been
//                   a hit, the normal path must have been a hit of the same entry and SameState must hold, else it is logged
namespace {
constexpr uint32_t kRtArr = 6;       // indexed attributes a record keeps (J3D: position, normal, a colour, texture coordinates)
constexpr uint32_t kRtSlots = 4096;  // records, by handle (direct-mapped: the handles are consecutive numbers)
// Per indexed attribute: the array's registers then (CP 0xA0+n / 0xB0+n as written), and the span the entry decoded with its generation.
struct RtArr { uint32_t base, stride; const uint8_t* p; uint32_t n; uint64_t gen; };
struct alignas(32) RtSlot { // (on the console: the head, the vertex-state words and three arrays in four cache lines)
    uint32_t handle = 0, nbytes = 0;
    const uint8_t* list = nullptr;
    Entry* e = nullptr;
    uint32_t stamp = 0, epoch = 0;
    uint32_t idxMask = 0;            // the entry's
    uint8_t fmt = 0, nArr = 0;       // the one vertex format the entry uses; the indexed attributes kept
    uint16_t nx1 = 0xFFFF;           // (GXV_CACHE24, in what was padding: the slot of the shape called after this one at its last call; 0xFFFF none)
    uint32_t vcd[2] = {};            // CP 0x50 / 0x60 as written then
    uint32_t vat[3] = {};            // CP 0x70+fmt / 0x80+fmt / 0x90+fmt as written then
    uint8_t inst = 0;                // (GXV_RT_INST, in what was padding: 1 = a second record of a shape, made for another instance's vertex state)
    uint64_t genList = 0;
    dj::Keys dj;                     // (GXV_DIRECT_J3D: the shape's key template, vgx_direct_dl.inc; before the arrays, in the lines a hit of a
                                     // shape with up to three indexed arrays reads anyway on the console)
    RtArr arr[kRtArr];
};
std::vector<RtSlot> g_rtSlots;
struct RtCounters { uint64_t calls = 0, hits = 0, none = 0, failed = 0, recorded = 0, checked = 0, bad = 0; } g_rt;
// (GXV_FE_CENSUS, fedl25: the reasons. A call with no record: its slot was never used / holds another handle / holds this handle for another list or
// size. A failed test: the entry's marks, the vertex state words, the list's generation, an array's generation. A normal-path call that left no record:
// it did not end at the first variant's hit or build, the entry was raced, more arrays than a record keeps, more or fewer than one vertex format, the
// CP words not known, SameState false. Plain counters, a byte store at each exit; printed by the census only.)
enum : uint8_t { RfNone, RfMarks, RfVertex, RfGenList, RfGenArr, RfCount };
enum : uint8_t { RnEmpty, RnOtherHandle, RnOtherList, RnCount };
enum : uint8_t { RrOk, RrNoEntry, RrRaced, RrArrays, RrFormats, RrRaw, RrState, RrCount };
uint8_t g_rtFailWhy = RfNone;
uint64_t g_rtFailN[RfCount] = {}, g_rtNoneN[RnCount] = {}, g_rtRecN[RrCount] = {};
// (the lists of the calls that had no record, by list: address, size, calls, the last reason: Rr*, or RrBypass when the normal path ended in the bypass map)
constexpr uint8_t RrBypass = RrCount;
struct RtNoneList { const uint8_t* list = nullptr; uint32_t nbytes = 0, calls = 0; uint8_t why = 0, handleLow = 0; uint32_t handle = 0; };
RtNoneList g_rtNoneList[64];
uint64_t g_rtNoneLost = 0;
uint64_t g_rtInst[3] = {}, g_rtDiffBytes = 0, g_rtDiffEntries = 0; // (GXV_RT_INST=2: vertex / index structure bytes that differ between the record's entry and the normal path's)  // (GXV_RT_INST: calls that took a slot other than the handle's own, of them hits, records made there)
// (GXV_DIRECT_J3D >= 1 with GXV_RETAINED=0: the records on, its templates are kept with them)
inline int RtModeBase() {
    return static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_RETAINED", 0) != 0 ? EnvInt("GXV_RETAINED", 0) : (dj::Mode() != 0 ? 1 : 0))));
}
// (GXV_RT_INST=2: the records are on and verified: every call goes the normal way, and a record that would have been a hit is compared with it)
inline int RtMode() {
    const int m = RtModeBase();
    return m == 1 && RtInstMode() == 2 ? 2 : m;
}
// The vertex state, by the registers it is decoded from. WriteCPFull sets each field of vcd[] / vat[] from exactly one of the raw CP words
// State::cpRaw keeps (vgx_state.h: "each of them sets its own fields completely"; SetVcd / SetVat have no other caller), and g.arrays[n]
// is made from cpBase[n] / cpStride[n] alone (BpArraySync, through the guest-memory map, which does not change). So with these words as they
// were when SameState held for the entry, SameState holds now: the same descriptor, format row and array bindings. (Exact, no hash: the
// words are compared; a word not known (cpRawValid) is not recorded.)
constexpr uint32_t kRtRawVcd = 1u << 0 | 1u << 1;
inline uint32_t RtRawFmt(uint32_t f) { return 1u << (2 + f) | 1u << (10 + f) | 1u << (18 + f); }
inline bool RtSameVertexState(const RtSlot& s) {
    const uint32_t f = s.fmt;
    if ((g.cpRawValid & (kRtRawVcd | RtRawFmt(f))) != (kRtRawVcd | RtRawFmt(f))) return false;
    if (g.cpRaw[0] != s.vcd[0] || g.cpRaw[1] != s.vcd[1] || g.cpRaw[2 + f] != s.vat[0] || g.cpRaw[10 + f] != s.vat[1] || g.cpRaw[18 + f] != s.vat[2])
        return false;
    uint32_t k = 0;
    for (uint32_t m = (s.idxMask >> 9) & 0xFFFu; m; m &= m - 1, ++k) {
        const int i = __builtin_ctz(m);
        if (g.cpBase[i] != s.arr[k].base || g.cpStride[i] != s.arr[k].stride) return false;
    }
    return true;
}
// (GXV_RT_INST) the slot of the shape's record for the vertex state now: the bases of the arrays the shape's own record reads pick it, so that the
// instances of a model that have arrays of their own each keep a record. A hint only: RtTest checks every word, whatever slot it is read from.
// (-DGXV_RT_INST_MUTANT=1 / 2: the test builds only: every instance of a shape in one slot; =1 also leaves the vertex-state test out for instance slots.)
inline uint32_t RtInstIdx(const RtSlot& p0) {
    uint32_t h = p0.handle * 0x9E3779B1u;
#if !defined(GXV_RT_INST_MUTANT)
    for (uint32_t m = (p0.idxMask >> 9) & 0xFFFu; m; m &= m - 1) {
        h = (h ^ g.cpBase[__builtin_ctz(m)]) * 0x85EBCA6Bu;
        h ^= h >> 15;
    }
#endif
    return (h >> 20) & (kRtSlots - 1);
}
// Whether the record is a hit now (nothing is changed but SyncVertexArrays, which the normal path makes first as well).
inline bool RtTest(const RtSlot& s, const uint8_t* list, uint32_t nbytes, uint32_t handle) {
    if (s.handle != handle || s.list != list || s.nbytes != nbytes || !s.e || s.epoch != g_rpEpoch || s.e->rpStamp != s.stamp) { g_rtFailWhy = RfMarks; return false; }
    if (!RtSameVertexState(s)) {
#if defined(GXV_RT_INST_MUTANT) && GXV_RT_INST_MUTANT == 1
        if (!s.inst)
#endif
        { g_rtFailWhy = RfVertex; return false; }
    }
    SyncVertexArrays();
    if (GuestWriteGeneration(list, nbytes) != s.genList) { g_rtFailWhy = RfGenList; return false; }
    for (uint32_t k = 0; k < s.nArr; ++k)
        if (GuestWriteGeneration(s.arr[k].p, s.arr[k].n) != s.arr[k].gen) { g_rtFailWhy = RfGenArr; return false; }
    return true;
}
// The record of the call the normal path just made, when it ended at a hit or build of the entry e (CallList's g_rpOutE).
uint8_t RtRecord(RtSlot& s, uint32_t handle, const uint8_t* list, uint32_t nbytes, Entry* e, uint8_t inst = 0) {
    s.handle = 0;
    if (!e || e->list != list || e->nbytes != nbytes) { ++g_rtRecN[RrNoEntry]; return RrNoEntry; }
    if (e->raced) { ++g_rtRecN[RrRaced]; return RrRaced; }
    const uint32_t arrMask = (e->idxMask >> 9) & 0xFFFu;
    if (static_cast<uint32_t>(__builtin_popcount(arrMask)) > kRtArr) { ++g_rtRecN[RrArrays]; return RrArrays; }
    if (__builtin_popcount(e->fmtMask) != 1) { ++g_rtRecN[RrFormats]; return RrFormats; }
    const uint32_t f = static_cast<uint32_t>(__builtin_ctz(e->fmtMask));
    if ((g.cpRawValid & (kRtRawVcd | RtRawFmt(f))) != (kRtRawVcd | RtRawFmt(f))) { ++g_rtRecN[RrRaw]; return RrRaw; }
    if (!SameState(*e)) { ++g_rtRecN[RrState]; return RrState; }
    ++g_rtRecN[RrOk];
    s.nbytes = nbytes;
    s.list = list;
    s.e = e;
    s.stamp = e->rpStamp;
    s.epoch = g_rpEpoch;
    s.idxMask = e->idxMask;
    s.fmt = static_cast<uint8_t>(f);
    s.vcd[0] = g.cpRaw[0];
    s.vcd[1] = g.cpRaw[1];
    s.vat[0] = g.cpRaw[2 + f];
    s.vat[1] = g.cpRaw[10 + f];
    s.vat[2] = g.cpRaw[18 + f];
    s.genList = e->genList;
    s.nArr = 0;
    for (uint32_t m = arrMask; m; m &= m - 1) {
        const int i = __builtin_ctz(m);
        const ArrUse& a = e->arr[i];
        s.arr[s.nArr++] = RtArr{g.cpBase[i], g.cpStride[i], a.p + a.lo, a.hi - a.lo, a.gen};
    }
    s.inst = inst;
    s.handle = handle;
    ++g_rt.recorded;
    if (inst) ++g_rtInst[2];
    return RrOk;
}
} // namespace

#include "vgx_direct_dl.inc" // GXV_DIRECT_J3D
bool RetainedOn() { return RtMode() != 0; }
void RetainedCounters(uint64_t out[7]) {
    out[0] = g_rt.calls; out[1] = g_rt.hits; out[2] = g_rt.none; out[3] = g_rt.failed; out[4] = g_rt.recorded; out[5] = g_rt.checked; out[6] = g_rt.bad;
}
void RtNoneListText(char* out, uint32_t n, double pics, uint64_t* lost) {
    static const char* const kRr[RrCount + 1] = {"made", "ended in no first-variant hit/build", "raced", "too many arrays", "not one vertex format", "CP words unknown", "SameState", "ended in the bypass map"};
    RtNoneList* top[6] = {};
    for (RtNoneList& t : g_rtNoneList) {
        if (!t.list || !t.calls) continue;
        for (int i = 0; i < 6; ++i)
            if (!top[i] || t.calls > top[i]->calls) { for (int k = 5; k > i; --k) top[k] = top[k - 1]; top[i] = &t; break; }
    }
    uint32_t at = 0;
    out[0] = 0;
    for (int i = 0; i < 6 && top[i]; ++i)
        if (at + 120 < n) at += static_cast<uint32_t>(std::snprintf(out + at, n - at, " [%p %uB h%u %.1f/pic: %s]", static_cast<const void*>(top[i]->list), top[i]->nbytes, top[i]->handle,
                                                                   top[i]->calls / pics, kRr[top[i]->why <= RrCount ? top[i]->why : 0]));
    if (!at) std::snprintf(out, n, " none");
    *lost = g_rtNoneLost;
    for (RtNoneList& t : g_rtNoneList) t = RtNoneList{};
    g_rtNoneLost = 0;
}
// (the census's order: [0] unused, [1..4] failed tests by reason, [5..7] no record by reason, [8] records made, [9..14] left no record by reason, [15..17] GXV_RT_INST)
void RtReasonCounters(uint64_t out[20]) {
    out[0] = 0;
    out[18] = g_rtDiffBytes;
    out[19] = g_rtDiffEntries;
    for (int i = 0; i < 4; ++i) out[1 + i] = g_rtFailN[RfMarks + i];
    for (int i = 0; i < 3; ++i) out[5 + i] = g_rtNoneN[i];
    out[8] = g_rtRecN[RrOk];
    for (int i = 0; i < 6; ++i) out[9 + i] = g_rtRecN[RrNoEntry + i];
    for (int i = 0; i < 3; ++i) out[15 + i] = g_rtInst[i];
}
// ---- GXV_CACHE24: hints along the shapes' call order (see vgx_state.h). A retained call learns that its slot follows the slot called just
// before it (nx1, a store only when it changes), and walks that chain d + 2 calls ahead (d = GXV_CACHE24_DIST): the slot's lines
// of the call d + 2 ahead, the entry's head of the call d + 1 ahead (read from the slot, which was hinted a call ago), and for the call d
// ahead what its test and hit read: the generation counters of its list and arrays (from its slot) and the entry's runs and bounds (from the
// entry's head, hinted a call ago). Every address comes from lines hinted at least one call before, or from this call's own slot; a wrong
// prediction (objects came or went) only hints lines nobody reads. Reads never fault: the slot indices are < kRtSlots, an entry is read
// only while its record's epoch is g_rpEpoch (nothing freed since, as RtTest's own read of s.e).
namespace {
RtSlot* g_rtPrev = nullptr;
bool g_rtRight = false; // the call in progress was the one its predecessor's slot predicted (its entry's lines were hinted a call ago)
// One hint per line of [p, p + 32 * n) for a 32-byte aligned p (a slot), or for an entry (not aligned: lines of the first 100 bytes, five hints).
inline void Pf24Slot(const void* p) {
    const uint8_t* q = static_cast<const uint8_t*>(p);
    g_c24Pf += 2;
#if defined(GXV_CACHE24_MODEL)
    gxv_model_touch(q); gxv_model_touch(q + 32);
#else
    __builtin_prefetch(q, 0, 3); __builtin_prefetch(q + 32, 0, 3);
#endif
}
inline void Pf24EntryHead(const void* p) {
    const uint8_t* q = static_cast<const uint8_t*>(p);
    g_c24Pf += 5;
#if defined(GXV_CACHE24_MODEL)
    gxv_model_touch(q); gxv_model_touch(q + 32); gxv_model_touch(q + 64); gxv_model_touch(q + 96); gxv_model_touch(q + 99);
#else
    __builtin_prefetch(q, 0, 3); __builtin_prefetch(q + 32, 0, 3); __builtin_prefetch(q + 64, 0, 3); __builtin_prefetch(q + 96, 0, 3); __builtin_prefetch(q + 99, 0, 3);
#endif
}
void RtAhead(RtSlot& s, uint32_t idx) {
    C24Stats& st = g_c24s;
    RtSlot* const prev = g_rtPrev;
    g_rtPrev = &s;
    g_rtRight = false;
    if (prev) {
        ++st.rtCalls;
        if (prev->nx1 == idx) { ++st.rtRight; g_rtRight = true; }
        else prev->nx1 = static_cast<uint16_t>(idx);
    }
    const uint32_t d = Cache24Dist();
    RtSlot* b = &s; // b: the call d ahead (its slot's lines were hinted a call ago: each hop reads a slot hinted earlier, or this call's own)
    for (uint32_t i = 0; i < d; ++i) {
        const uint32_t nx = b->nx1;
        if (nx >= kRtSlots) return;
        b = &g_rtSlots[nx];
    }
    const uint32_t n1 = b->nx1;
    if (n1 >= kRtSlots) return;
    RtSlot* const a = &g_rtSlots[n1]; // a: the call d + 1 ahead
    // the call d ahead: what RtTest and the hit read after the slot and the entry's head: the runs and the bounds
    if (b->handle && b->e && b->epoch == g_rpEpoch) {
        const Entry& e = *b->e;
        Pf24Lines(e.runs.data(), static_cast<uint32_t>(e.runs.size() * sizeof(Run)), 4);
        Pf24Lines(e.rowBoxes.data(), static_cast<uint32_t>(e.rowBoxes.size() * sizeof(RowBox)), 2);
    }
    // the call d + 1 ahead: its entry's head (list identity, stamp, last use, the runs' and bounds' vector headers), and the rest of its slot
    if (a->handle && a->e && a->epoch == g_rpEpoch) Pf24EntryHead(a->e);
    {
        const uint8_t* const lo = reinterpret_cast<const uint8_t*>(a) + 64;
        const uint8_t* const hi = reinterpret_cast<const uint8_t*>(&a->arr[a->nArr <= kRtArr ? a->nArr : kRtArr]);
        if (hi > lo) Pf24Lines(lo, static_cast<uint32_t>(hi - lo), 4);
    }
    // the call d + 2 ahead: its slot's first two lines (the head, the vertex-state words and the list's generation)
    const uint32_t n2 = a->nx1;
    if (n2 < kRtSlots) Pf24Slot(&g_rtSlots[n2]);
}
} // namespace
// GXV_J3DNATIVE=5 (vgx_j3native2.inc): CallListRetained's hit branch only. True: the call was a record hit and is done (Emit ran); false:
// nothing was done but RtTest's SyncVertexArrays and, when the record passed its tests, the age and sweep (both the normal path makes as
// well, idempotently): the caller makes the register file real and then calls CallListRetained.
bool CallListRetainedHit(const uint8_t* list, uint32_t nbytes, uint32_t handle) {
#if defined(VGX_CLONE_B)
    (void)list; (void)nbytes; (void)handle;
    return false;
#else
    Init();
    if (RtMode() != 1 || handle == 0 || g_mode != 1 || nbytes <= TinyListBytes() || TraceFrame() || g_rtSlots.empty()) return false;
    RtSlot& s = g_rtSlots[handle & (kRtSlots - 1)];
    if (s.handle != handle || s.list != list || s.nbytes != nbytes) return false;
    if (!RtTest(s, list, nbytes, handle)) return false;
    knob::HitFence();
    const uint64_t frame = CurrentFrame();
    const uint64_t age = AgeAndSweep(frame);
    if (s.epoch != g_rpEpoch) return false; // (the sweep freed something: the normal path)
    ++g_rt.calls;
    Entry& e = *s.e;
    workload::FeScope cls(workload::FeGeomHit);
    // ---- CallListBody's hit branch (as CallListRetained's)
    const bool afterChange = e.fails != 0;
    e.lastUse = age;
    if (e.fails) { two::AGuard l; e.fails = 0; }
    ++g_ctr.hits;
    ++stats::Cur().dlHit;
    g_ctr.hitVerts += e.totalVerts;
    if (!afterChange && !e.bufId && e.promotable && g_static) { two::AGuard l; Promote(e); }
    DjEmit(s, list, e, nbytes);
    ++g_rt.hits;
    return true;
#endif
}
void CallListRetained(const uint8_t* list, uint32_t nbytes, uint32_t handle) {
#if defined(VGX_CLONE_B)
    (void)handle; // (GXV_FE_TWOCORE: the second worker takes the normal path, which the records' hits are exact shortcuts of)
    CallList(list, nbytes);
    return;
#endif
    Init();
    const int mode = RtMode();
    if (mode == 0 || handle == 0 || g_mode != 1 || nbytes <= TinyListBytes() || TraceFrame()) {
        CallList(list, nbytes);
        return;
    }
    if (g_rtSlots.empty()) g_rtSlots.resize(kRtSlots);
    ++g_rt.calls;
    uint32_t slotIdx = handle & (kRtSlots - 1);
    // (GXV_RT_INST: the shape's own record is of the same shape and list but of other vertex state, so this call is of another instance of the
    // model (its arrays are its own): its record is the one in the slot its position array's base names. That slot may only be used if it is
    // empty, an instance record, or this shape's: a shape's own record is never given up for it.)
    bool instSlot = false;
    const int instMode = RtInstMode();
    if (instMode != 0) {
        const RtSlot& p0 = g_rtSlots[slotIdx];
        if (p0.handle == handle && p0.list == list && p0.nbytes == nbytes && !RtSameVertexState(p0)) {
            slotIdx = RtInstIdx(p0);
            instSlot = true;
            ++g_rtInst[0];
        }
    }
    RtSlot& s = g_rtSlots[slotIdx];
    const bool c24 = Cache24Pf();
    if (c24) RtAhead(s, slotIdx);
    const bool known = s.handle == handle && s.list == list && s.nbytes == nbytes;
    if (!known) {
        ++g_rt.none;
        ++g_rtNoneN[s.handle == 0 ? RnEmpty : s.handle != handle ? RnOtherHandle : RnOtherList];
    }
    if (known && mode == 1) {
        Entry& e = *s.e;
        if (FeCutHint() && s.e && s.epoch == g_rpEpoch && !(c24 && g_rtRight)) {
            // (GXV_FECUT_HINT: what the hit branch reads after the tests, as RpReplay hints it; not when GXV_CACHE24 hinted them a call ago)
            FePrefetchLines(e.runs.data(), static_cast<uint32_t>(e.runs.size() * sizeof(Run)), 4);
            FePrefetchLines(e.rowBoxes.data(), static_cast<uint32_t>(e.rowBoxes.size() * sizeof(RowBox)), 2);
        }
        if (RtTest(s, list, nbytes, handle)) {
            // (no acquire fence here with GXV_FE_KNOBS=1, as RpReplay: a hit reads no guest bytes after the generations)
            knob::HitFence();
            const uint64_t frame = CurrentFrame();
            const uint64_t age = AgeAndSweep(frame);
            if (s.epoch == g_rpEpoch) { // (the sweep freed nothing)
                workload::FeScope cls(workload::FeGeomHit);
                // ---- CallListBody's hit branch
                const bool afterChange = e.fails != 0;
                e.lastUse = age;
                if (e.fails) { two::AGuard l; e.fails = 0; }
                ++g_ctr.hits;
                ++stats::Cur().dlHit;
                g_ctr.hitVerts += e.totalVerts;
                if (!afterChange && !e.bufId && e.promotable && g_static) { two::AGuard l; Promote(e); }
                DjEmit(s, list, e, nbytes); // (Emit; GXV_DIRECT_J3D: with the shape's key template)
                ++g_rt.hits;
                if (instSlot) ++g_rtInst[1];
                return;
            }
        }
        ++g_rt.failed;
        ++g_rtFailN[g_rtFailWhy];
    }
    // ---- the normal path (the draw replay included), and the record of where it ended
    bool wouldHit = false, sameState = true;
    if (known && mode == 2) {
        wouldHit = RtTest(s, list, nbytes, handle);
        if (wouldHit) sameState = SameState(*s.e);
        else ++g_rt.failed;
    }
    Entry* const before = wouldHit ? s.e : nullptr;
    const uint32_t stampBefore = before ? before->rpStamp : 0u;
    g_rpOutE = nullptr;
    g_rtOutE = nullptr;
    const uint64_t byBefore = g_ctr.bypassed + g_ctr.uncacheable;
    CallList(list, nbytes);
    Entry* const out = g_rpOutE ? g_rpOutE : (instMode != 0 ? g_rtOutE : nullptr);
    if (mode == 2 && wouldHit) {
        ++g_rt.checked;
        // (the normal path's hit leaves the entry's stamp as it was; a build moves it)
        const bool ok = sameState && out == before && before->rpStamp == stampBefore;
        if (out && before && out != before) { // (another entry: the bytes its decode holds, and the primitives' index structure, compared)
            ++g_rtDiffEntries;
            const std::vector<uint8_t>& a = before->verts;
            const std::vector<uint8_t>& b = out->verts;
            const size_t n = std::min(a.size(), b.size());
            for (size_t i = 0; i < n; ++i) g_rtDiffBytes += a[i] != b[i];
            g_rtDiffBytes += (a.size() > b.size() ? a.size() - b.size() : b.size() - a.size());
            if (before->segs.size() != out->segs.size()) g_rtDiffBytes += 1;
            else
                for (size_t i = 0; i < before->segs.size(); ++i) {
                    const Seg& x = before->segs[i];
                    const Seg& y = out->segs[i];
                    g_rtDiffBytes += x.prim != y.prim || x.fmt != y.fmt || x.count != y.count || x.layoutMask != y.layoutMask || x.offset != y.offset;
                }
        }
        if (!ok) {
            ++g_rt.bad;
            VGX_LOG_N(32, "retained: MISMATCH handle %u list %p (%u bytes): the record would have been a hit of entry %p, the normal path %s%s\n", handle,
                      (const void*)list, nbytes, (const void*)before, out == before ? "hit it but built it again" : out ? "went to another entry" : "went another way",
                      sameState ? "" : " (and SameState does not hold)");
        }
        if ((g_rt.checked & 0x3FFFu) == 0)
            VGX_LOG_N(200, "retained: %llu hits checked, %llu mismatching\n", static_cast<unsigned long long>(g_rt.checked), static_cast<unsigned long long>(g_rt.bad));
    }
    if (instSlot && s.handle != 0 && !s.inst && s.handle != handle) return; // (a shape's own record is not given up for an instance's)
    const uint8_t rc = RtRecord(s, handle, list, nbytes, out, instSlot ? 1 : 0);
    if (!known && CensusOn()) { // (GXV_FE_CENSUS: which lists the calls with no record are, and why none was made)
        const uint8_t why = rc == RrNoEntry && g_ctr.bypassed + g_ctr.uncacheable != byBefore ? RrBypass : rc;
        RtNoneList* hit = nullptr;
        uint32_t h = static_cast<uint32_t>((reinterpret_cast<uintptr_t>(list) >> 5) * 0x9E3779B1u) >> 26; // (6 bits: 64 slots)
        for (uint32_t k = 0; k < 8 && !hit; ++k, h = (h + 1) & 63u) {
            RtNoneList& t = g_rtNoneList[h];
            if (t.list == list && t.handle == handle) hit = &t;
            else if (!t.list) { t.list = list; t.handle = handle; hit = &t; }
        }
        if (hit) { hit->nbytes = nbytes; ++hit->calls; hit->why = why; } else ++g_rtNoneLost;
    }
}

// GXV_FACECULL (see "view lists"): the record side's services (vgx_draw.cpp FaceTake). Front-end thread only.
// A new list for the run: its buffer is uploaded (vtx / idx are taken); the list it had goes.
void DlFaceInstall(facecull::Run& r, std::vector<uint8_t>& vtx, std::vector<uint16_t>& idx, const float eye[3], float margin, int side) {
    FaceDropList(r);
    r.idxKept = static_cast<uint32_t>(idx.size());
    r.vtxKept = r.stride ? static_cast<uint32_t>(vtx.size() / r.stride) : 0u;
    for (int a = 0; a < 3; ++a) r.eye[a] = eye[a];
    r.margin = margin;
    r.side = static_cast<int8_t>(side);
    r.builtFrame = CurrentFrame();
    ++g_face.lists;
    if (idx.empty() || vtx.empty()) return; // (nothing faces the camera, or a census: a list without a buffer)
    BufUpload ib;
    ib.bufId = AllocBufId(nullptr);
    ib.vtx.assign(vtx.begin(), vtx.end());
    ib.idx.assign(idx.begin(), idx.end());
    r.bufId = ib.bufId;
    const uint64_t bytes = ib.vtx.size() + ib.idx.size() * 2u;
    g_face.gpuBytes += bytes;
    stats::Frame& fr = stats::Cur();
    ++fr.bufUploads;
    fr.bufUploadBytes += static_cast<uint32_t>(bytes);
    CmdBuf& buf = Pending();
    buf.bufUploads.push_back(std::move(ib));
    buf.cmds.push_back(Cmd{CMD_BUF_UPLOAD, static_cast<uint32_t>(buf.bufUploads.size() - 1)});
}
void DlFaceDrop(facecull::Run& r) { FaceDropList(r); }
// Once a picture: the buffers of lists dropped four or more pictures ago are freed.
void DlFacePump(uint64_t frame) {
    if (g_faceFree.empty()) return;
    size_t keep = 0;
    for (size_t i = 0; i < g_faceFree.size(); ++i) {
        const FaceFree f = g_faceFree[i];
        if (frame >= f.frame && frame - f.frame < 4) { g_faceFree[keep++] = f; continue; }
        Pending().cmds.push_back(Cmd{CMD_BUF_FREE, f.bufId});
        if (f.bufId < g_bufOwner.size()) g_bufOwner[f.bufId] = nullptr;
        g_freeIds.push_back(f.bufId);
    }
    g_faceFree.resize(keep);
}
uint32_t DlFaceMinIdx() { return FaceMinIdx(); }
void DlFaceTotals(uint64_t out[6]) {
    out[0] = g_face.runs; out[1] = g_face.cpuBytes; out[2] = g_face.lists; out[3] = g_face.gpuBytes; out[4] = g_face.refusedBudget; out[5] = g_faceFree.size();
}

// GXV_VTXPACK (see "packed static runs")
bool VtxPackOn() { return PackSwitch(); }
void VtxPackCounters(uint64_t out[7]) {
    out[0] = g_vpLists.load(std::memory_order_relaxed);
    out[1] = g_vpRuns.load(std::memory_order_relaxed);
    out[2] = g_vpRunsSeen.load(std::memory_order_relaxed);
    out[3] = g_vpVerts.load(std::memory_order_relaxed);
    out[4] = g_vpBytesIn.load(std::memory_order_relaxed);
    out[5] = g_vpBytesOut.load(std::memory_order_relaxed);
    out[6] = g_vpAbsRefused.load(std::memory_order_relaxed);
}

// ---- GXV_FE_TWOCORE (vgx_twocore.cpp)
#if !defined(VGX_CLONE_B)
extern "C" void* vgx2_dl_shared(int which) {
    switch (which) {
    case 0: return &g_map;
    case 1: return &g_bypass;
    case 2: return &g_lastSweep;
    default: return nullptr;
    }
}
// (under the player's lock) an entry the second worker found unchanged by its contents: what the rehash branch stores
extern "C" void vgx2_a_dl_rehash(void* ep, uint64_t gl, const uint64_t* ga) {
    Entry& e = *static_cast<Entry*>(ep);
    e.genList = gl;
    for (uint32_t m = (e.idxMask >> 9) & 0xFFFu; m; m &= m - 1) e.arr[__builtin_ctz(m)].gen = ga[__builtin_ctz(m)];
    e.fails = 0;
    ++g_ctr.rehashOk;
}
// (under the player's lock) an entry the second worker used: what the hit branch's `lastUse = age` does
extern "C" void vgx2_a_dl_mark(void* e, uint64_t age) { static_cast<Entry*>(e)->lastUse = age; }
// (under the player's lock) GXV_STATICCULL: the first use of the entry since its build was the second worker's
extern "C" void vgx2_a_dl_drawn(void* e) {
    Entry& x = *static_cast<Entry*>(e);
    if (!x.rowBoxes.empty()) x.rowBoxes[0].drawn = 1;
}
// The age counter as it is after a call in `frame` (the second worker made the frame's first calls)
extern "C" void vgx2_a_dl_age(uint64_t age, uint64_t frame) {
    if (g_ageFrame != frame) { g_ageFrame = frame; g_age = age; }
}
// The age a call in `frame` will have
extern "C" uint64_t vgx2_a_dl_age_next(uint64_t frame) { return g_ageFrame == frame ? g_age : g_age + 1; }
#else
extern "C" void vgx2_b_dl_picture(void) { Init(); }
#endif
} // namespace vgx
