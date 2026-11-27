// scenes26 (12 Nov 2026): the pure logic of the per-picture scene structure: the depth load/store decision table, the tail predictor
// (GXV_TAILDEPTH), the A/B block schedule of GXV_SCENES26_VERIFY and the per-scene census. No GL, no SDK: tests/vgx_scenes26_test.cpp drives it.
//
// Why: on the SGX543 (tile-based, deferred) every scene of a picture loads and stores its colour and depth at its boundaries (2.1 MB each way at
// 960x544 per surface). The EFB depth plan (GXV_DEPTHPLAN, vgx_backend_gl.cpp dplan) already drops most of that; what it cannot know is the
// NEXT picture: it must assume the next picture reads the depth the last scene leaves, so the last scene of every picture stores a depth that
// the next picture's first scene (a covering depth-writing quad, GXV_FIRSTCLEAR) overwrites whole without reading it. TailPredictor answers
// "will the next picture's start overwrite the depth whole" from the pictures seen so far, with a self-check one picture later.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace vgx {
namespace s26 {

// ---- the depth load / store decision of one EFB scene (the plan's rule, written once so it can be tested as a table) -------------------------------
// Facts of a scene that begins at plan position b and ends at the next position that leaves the EFB (e):
//   reads        some draw / copy in [b, e) reads the depth (a depth test other than ALWAYS, a depth copy)
//   writes       some draw / clear in [b, e) writes depth (including a partial clear)
//   full         a whole-depth overwrite (clear of all of it, a covering depth-writing quad with ALWAYS) is in [b, e)
//   readFirst    the first read comes before the first whole overwrite (or there is no whole overwrite)
//   liveAtEnd    a read comes after e before the next whole overwrite (the end of the picture counts as a read unless the tail is dropped)
struct DepthFacts {
    bool reads = false, writes = false, full = false, readFirst = false, liveAtEnd = true;
};
struct DepthNeed {
    bool load = false, store = false;
};
// The scene stores its depth only if it changes it and somebody reads it later; it loads when it reads before overwriting all of it, or when it
// stores without overwriting all of it (the rest of the surface must survive in memory).
inline DepthNeed DecideDepth(const DepthFacts& f) {
    DepthNeed n;
    n.store = (f.writes || f.full) && f.liveAtEnd;
    n.load = f.readFirst || (n.store && !f.full);
    return n;
}
// What the plan does with the answer: it only ever lowers what the default rule (load unless the scene begins with a clear of all depth; always store) asked for.
inline DepthNeed Lower(DepthNeed dflt, DepthNeed plan) {
    return DepthNeed{dflt.load && plan.load, dflt.store && plan.store};
}

// ---- the tail predictor ---------------------------------------------------------------------------------------------------------------------------
// The last EFB scene of a picture stores its depth because "the next picture may read it". The game's pictures all begin with the covering clear
// quad (MainLoopFramework::clearEfb: depth ALWAYS + write, whole EFB), so the picture's start is dead for depth: plan->StartDead(). After `run`
// planned pictures in a row that began dead, the predictor lets the picture being planned leave its tail store out. The next picture checks the
// prediction (Observe): a picture that does not begin dead after a dropped tail is a MISS (counted; the predictor then stays off for good, and
// that one picture may have read stale depth); a picture without a plan cannot check it (Unverified; the run starts again).
class TailPredictor {
public:
    explicit TailPredictor(uint32_t run = 120) : need_(run < 4 ? 4 : run) {}
    // At the start of planning a picture: did the previous picture drop its tail?  startDead: this picture's own start needs nothing of the old depth.
    // planned: this picture has a plan at all.
    void Observe(bool planned, bool startDead) {
        if (!planned) {
            if (dropped_) ++unverified_;
            dropped_ = false;
            run_ = 0;
            return;
        }
        if (dropped_ && !startDead) {
            ++misses_;
            off_ = true;
        }
        dropped_ = false;
        if (startDead) ++run_; else run_ = 0;
        ++pictures_;
    }
    // Whether the picture being planned leaves its tail store out (call after Observe for this picture).
    bool DropTail() {
        const bool d = !off_ && run_ >= need_;
        dropped_ = d;
        if (d) ++drops_;
        return d;
    }
    bool Off() const { return off_; }
    uint32_t Run() const { return run_; }
    uint64_t Drops() const { return drops_; }
    uint64_t Misses() const { return misses_; }
    uint64_t Unverified() const { return unverified_; }
    uint64_t Pictures() const { return pictures_; }
    void Reset() { run_ = 0; dropped_ = false; }
private:
    uint32_t need_;
    uint32_t run_ = 0;
    bool dropped_ = false, off_ = false;
    uint64_t drops_ = 0, misses_ = 0, unverified_ = 0, pictures_ = 0;
};

// ---- the A/B schedule (GXV_SCENES26_VERIFY=<block>) -----------------------------------------------------------------------------------------------
// Blocks of `block` pictures: ORIGINAL, NEW, ORIGINAL, NEW ... The last picture of each block is read back at its display copy. A cycle is the
// first three blocks (O, N, O2): mean / max |N - O| against the noise floor |O2 - O|.
enum Phase : int { P_ORIG = 0, P_NEW = 1 };
inline Phase PhaseOf(uint64_t picture, uint32_t block) { return ((picture / (block ? block : 1)) & 1u) ? P_NEW : P_ORIG; }
inline bool LastOfBlock(uint64_t picture, uint32_t block) { const uint32_t b = block ? block : 1; return picture % b == b - 1; }
// index of the shot a block is kept in: 0 O, 1 N, 2 O2, 3 N2 (blocks count mod 4)
inline unsigned ShotOf(uint64_t picture, uint32_t block) { return static_cast<unsigned>((picture / (block ? block : 1)) & 3u); }

// ---- the per-scene census -------------------------------------------------------------------------------------------------------------------------
// Per EFB scene index of a picture (0 = the picture's first): how often it began, with a depth load / store, and how often the plan found its colour
// dead at the start (a full overwrite of colour and alpha comes before any read: the colour load is a wasted read), and the loads / stores the plan and
// the tail rule left out. Cheap: a few integer adds a scene.
constexpr int kMaxScenes = 8;
struct SceneRow {
    uint64_t begins = 0, dLoads = 0, dStores = 0, colourDead = 0, colourSkipped = 0, dLoadsSaved = 0, dStoresSaved = 0, tailSaved = 0;
};
struct Census {
    SceneRow row[kMaxScenes];
    uint64_t pictures = 0, scenes = 0;
    int cur = 0;
    void BeginPicture() { cur = 0; ++pictures; }
    SceneRow& Scene() { return row[cur < kMaxScenes ? cur : kMaxScenes - 1]; }
    // One EFB scene begins: what the plan decided (default: what the rule without the plan would have asked).
    void Begin(bool load, bool store, bool loadDefault, bool storeDefault, bool tailDropped, bool colourDead, bool colourSkipped) {
        SceneRow& r = Scene();
        ++r.begins;
        ++scenes;
        r.dLoads += load;
        r.dStores += store;
        r.dLoadsSaved += (loadDefault && !load);
        r.dStoresSaved += (storeDefault && !store);
        r.tailSaved += tailDropped;
        r.colourDead += colourDead;
        r.colourSkipped += colourSkipped;
        ++cur;
    }
    // "scenes26: N pictures; a picture: E EFB scenes; scene k: begins b, depth load l% store s%, colour dead d% (skipped c%), saved: load, store (tail)"
    int Format(char* out, size_t cap) const {
        int n = std::snprintf(out, cap, "scenes26: %llu pictures, %.2f EFB scenes a picture; per EFB scene (index in the picture: share of pictures with it | depth load%% store%% | "
                              "colour dead at start%% (load skipped%%) | saved a picture: depth loads, depth stores, of those the tail):",
                              static_cast<unsigned long long>(pictures), pictures ? static_cast<double>(scenes) / static_cast<double>(pictures) : 0.0);
        const double p = pictures ? static_cast<double>(pictures) : 1.0;
        for (int i = 0; i < kMaxScenes && n > 0 && n < static_cast<int>(cap) - 220; ++i) {
            const SceneRow& r = row[i];
            if (!r.begins) continue;
            const double b = static_cast<double>(r.begins);
            n += std::snprintf(out + n, cap - static_cast<size_t>(n), " [%d: %.0f%% | %.0f%% %.0f%% | %.0f%% (%.0f%%) | %.2f %.2f (%.2f)]", i, 100.0 * b / p, 100.0 * static_cast<double>(r.dLoads) / b,
                               100.0 * static_cast<double>(r.dStores) / b, 100.0 * static_cast<double>(r.colourDead) / b, 100.0 * static_cast<double>(r.colourSkipped) / b,
                               static_cast<double>(r.dLoadsSaved) / p, static_cast<double>(r.dStoresSaved) / p, static_cast<double>(r.tailSaved) / p);
        }
        return n;
    }
    void Clear() { *this = Census{}; }
};

}  // namespace s26
}  // namespace vgx
