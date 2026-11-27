// precomp26 (GXV_STAGE_ENUM): the pure parts of "enumerate the shader programs a stage CAN need from the materials of its loaded models, ahead of first draw".
// Header only, standard library only: the front end (vgx_draw.cpp StageEnumDrawMarker / EnumNote), the back end (vgx_backend_gl.cpp EnumFrame / RunEnumWarm) and
// the host tests (tests/stage_enum_model.cpp) share it.
//
// WHY. Console job 1786c (Dreadnought, 12 Nov): a launch star carried the owner to a part of the map no tour had drawn; three programs were in no stage list and no pack, the
// shader compiler stopped the picture for 4.35 s. The lists hold what automated tours drew in each stage's opening view. What a stage CAN draw is in the materials of
// the models its resource archives hold (J3DModelData material tables), not in what was drawn.
//
// HOW (three parts).
//   game      (decomp/src/Game/System/PortStageEnum.cpp) walks every loaded J3DModelData; for each material that has a display list and a shape it writes, between the
//             warm-up scope markers, exactly what a draw writes - the material's list, the shape's vertex description list, the current-matrix words - and one
//             PORT_GX_MARK_STAGE_ENUM_DRAW marker in place of the geometry.
//   front end the marker resolves the register state as a draw does (ResolveState: BuildFsKey / BuildVsKey / Lookup / BlendKey, the folding and twin variants of
//             the warm-up scope) for a vertex layout made from the shape's description, without texture uploads and without recording anything, and pushes one Rec per
//             variant (the shader keys' hashes, layout and blend bits: the same record the stage lists hold) into the Inbox below.
//   back end  drains the Inbox once a frame: counts what the stage's lists already had and what no cache has (census), and in mode 2 works the rest off in rank order:
//             a program whose shaders a cache has is linked while the program slots allow (behind the closed screen, then behind play in 1 ms slices), one whose shaders no
//             cache has goes to the shader worker one at a time (compile into the card's cache only), never to the render thread.
//
// seedrun31 A4: a harvest run (GXV_SEEDWALK=1) keeps what it enumerated as "can draw" records of the stage list (CanDrawWord: the draw, vgx::firstvisit::canDrawTag and
// the scenario's hint), for a variant no list of the stage holds whose two shaders a cache holds (KeepAsCanDraw: a record whose texts were never compiled would fail the
// seed gate's closure). The release never enumerates (GXV_STAGE_ENUM=0); it reads those records from the package and runs them as a tier of their own behind the queue
// (vgx_stage_first_visit.h CanDrawGate). The record's shader hashes are the registered keys' (the front end's EnumNote takes ShaderSource::keyHash of the ids the dry draw
// resolved through the draw's own code, PairKeys / VsOutPrune included): with GXV_VSOUT=1 they are the pruned vertex keys a real draw records, so a can-draw record
// and a draw record of the same draw are the same variant (vgx::firstvisit::Identity) and the queue keeps one.
#pragma once
#include "vgx_stage_first_visit.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace vgx::senum {

constexpr unsigned kMaxRecs = 12000;   // records kept per stage (the inbox and the back end's list)
constexpr double kHitWindowMs = 60000.0; // the hit-rate line comes this long after the screen opened

// The record the front end hands over: a draw variant as the stage lists hold it (key hashes of the two shaders, layout mask << 13 | blend key).
struct Rec {
    uint64_t vs = 0, fs = 0;
    uint32_t lb = 0;
    uint32_t stage = 0;   // port_scene_key() when it was made
    uint32_t seq = 0;     // the order the game walked in (models of the first loaded archives first): lower = likelier
    uint8_t flags = 0;    // bit 0: the variant is the unfolded twin of a folded draw (rarer), bit 1: a palette twin
};

// Front end -> back end. The front end runs on the player thread, the back end on the render thread.
class Inbox {
public:
    void Push(const Rec& r) {
        std::lock_guard<std::mutex> g(m_);
        if (v_.size() >= kMaxRecs) { ++dropped_; return; }
        v_.push_back(r);
        ++pushed_;
    }
    void Drain(std::vector<Rec>& out) {
        std::lock_guard<std::mutex> g(m_);
        out.insert(out.end(), v_.begin(), v_.end());
        v_.clear();
    }
    uint64_t Pushed() const { std::lock_guard<std::mutex> g(m_); return pushed_; }
    uint64_t Dropped() const { std::lock_guard<std::mutex> g(m_); return dropped_; }
    size_t Size() const { std::lock_guard<std::mutex> g(m_); return v_.size(); }
    void Clear() { std::lock_guard<std::mutex> g(m_); v_.clear(); }
private:
    mutable std::mutex m_;
    std::vector<Rec> v_;
    uint64_t pushed_ = 0, dropped_ = 0;
};

inline uint64_t Mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDull; x ^= x >> 33; x *= 0xC4CEB9FE1A85EC53ull; x ^= x >> 33;
    return x;
}
inline uint64_t RecKey(const Rec& r) { return Mix64(r.vs * 0x9E3779B97F4A7C15ull + Mix64(r.fs) + (static_cast<uint64_t>(r.lb) << 7)); }
inline uint64_t ProgKey(uint64_t vs, uint64_t fs) { return Mix64(vs * 0x9E3779B97F4A7C15ull + Mix64(fs)); }

// ---- what the back end does with one enumerated program (not in the stage's lists) ----
enum class Cache : uint8_t { Unknown, Hit, Miss };
enum class Act : uint8_t {
    Link,       // shaders cached, a slot is free: link it (WarmVariant, optional)
    HoldBack,   // the render thread must not link now (the worker holds the compiler, in play): try again later
    NoSlot,     // shaders cached, no free slot above the reserve: it stays in the cache, a draw links it (4-20 ms) if it is ever needed
    Ask,        // shaders in no cache: give the pair to the worker
    WaitWorker, // asked already / the worker has too much in flight: later
    NoWorker,   // shaders in no cache and no worker: nothing safe to do (never compile on the render thread for a guess)
    Done,       // linked already
};
struct ActIn {
    bool linked = false;
    Cache cache = Cache::Unknown;
    bool live = false;           // the screen is open (play)
    bool workerBusy = false;
    bool haveWorker = false;
    unsigned freeSlots = 0, reserve = 0;
    unsigned inFlight = 0, maxInFlight = 1;
    bool asked = false;
    bool pacedOut = false;       // in play: the gap since the last ask has not passed
};
inline Act Decide(const ActIn& in) {
    if (in.linked) return Act::Done;
    if (in.cache == Cache::Hit) {
        if (in.freeSlots <= in.reserve) return Act::NoSlot;
        if (in.live && in.workerBusy) return Act::HoldBack;
        return Act::Link;
    }
    if (!in.haveWorker) return Act::NoWorker;
    if (in.asked) return Act::WaitWorker;
    if (in.inFlight >= in.maxInFlight || in.pacedOut) return Act::WaitWorker;
    return Act::Ask;
}

// ---- the census of one stage ----
struct Census {
    uint32_t stage = 0;
    uint64_t records = 0;        // records taken from the inbox for this stage
    uint64_t stale = 0;          // records of another stage, dropped
    uint64_t unknownShader = 0;  // records whose shaders the back end never learned (dropped after a wait)
    uint64_t variants = 0;       // distinct (program, layout, blend)
    uint64_t programs = 0;       // distinct programs
    uint64_t inLists = 0;        // of the programs: in the stage's queue (its own, player, menu, shared lists)
    uint64_t probed = 0, cached = 0, uncached = 0; // of the programs not in the lists: shaders in a cache / in no cache (probed so far)
    uint64_t queued = 0, linked = 0, noSlot = 0, asked = 0, workerReady = 0, workerFailed = 0, noWorker = 0, holdBack = 0;
    double ms = 0;               // render-thread time of the back end's part
    uint32_t walked = 0, total = 0; // the game's walk: pairs written / found (last report)
};

// ---- the hit rate: what the stage's draws in play first used against what enumeration had predicted ----
// A "first use" is a program (or variant) a draw in play records for the first time in the stage. Three buckets: in the stage's lists (nothing new: the queue covers it),
// not in the lists but predicted by enumeration (what this switch saves), not in the lists and not predicted (what is still uncovered and can still stop the picture).
class HitTracker {
public:
    void Reset() { *this = HitTracker(); }
    void Predict(uint64_t progKey, uint64_t varKey) { pred_.insert(progKey); predV_.insert(varKey); }
    // Returns the bucket: 0 in the lists, 1 predicted, 2 not predicted. Counted once per program and once per variant.
    int NoteProgram(uint64_t progKey, bool inLists) {
        if (!seenP_.insert(progKey).second) return -1;
        if (inLists) { ++pList_; return 0; }
        if (pred_.count(progKey)) { ++pHit_; return 1; }
        ++pMiss_;
        return 2;
    }
    int NoteVariant(uint64_t varKey, bool progInLists) {
        if (!seenV_.insert(varKey).second) return -1;
        if (progInLists) { ++vList_; return 0; }
        if (predV_.count(varKey)) { ++vHit_; return 1; }
        ++vMiss_;
        return 2;
    }
    uint64_t pList() const { return pList_; } uint64_t pHit() const { return pHit_; } uint64_t pMiss() const { return pMiss_; }
    uint64_t vList() const { return vList_; } uint64_t vHit() const { return vHit_; } uint64_t vMiss() const { return vMiss_; }
    size_t Predicted() const { return pred_.size(); }
    // The hit rate on what the lists lacked, in percent; -1 when nothing was missing.
    static double Rate(uint64_t hit, uint64_t miss) { return (hit + miss) ? 100.0 * static_cast<double>(hit) / static_cast<double>(hit + miss) : -1.0; }
private:
    std::unordered_set<uint64_t> pred_, predV_, seenP_, seenV_;
    uint64_t pList_ = 0, pHit_ = 0, pMiss_ = 0, vList_ = 0, vHit_ = 0, vMiss_ = 0;
};

// ---- seedrun31 A4: what a harvest keeps ----
inline uint32_t CanDrawWord(uint32_t lb, uint32_t scenarioBits) {
    return (lb & firstvisit::drawMask) | firstvisit::canDrawTag | (scenarioBits & firstvisit::scenarioMask);
}
inline bool KeepAsCanDraw(bool harvest, bool inLists, bool cached) { return harvest && !inLists && cached; }

}  // namespace vgx::senum
