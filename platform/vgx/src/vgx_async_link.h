// nohitch26 (GXV_ASYNC_LINK, default 0): no program is linked on the render path in play unless it must be. Pure, standard library only
// (the backend and tests/async_link_model.cpp share it).
//
// Why this exists. c79 tours (1787/1788, console, 12 Nov): a draw in play that needs a program that is not linked pays for the whole link on the render thread
// (glLinkProgram: a file read of the cached shaders, the GXM registration, the uniform queries; 8-25 ms when the compiled shader is in the cache, 50-300 ms when
// it is not). The links come in clusters (the scenario starter, about 8.5 s into a stage: 235 ms). glLinkProgram cannot move to a thread: vitaGL's state and the GXM shader
// patcher are the GL thread's (tools/vitagl/patches/0020 says so; only the compile into the cache files is off-thread, GXV_ASYNC_SHADERS). So the link is SLICED on the
// render thread instead: at most one link a picture when the picture has not spent its budget, a gap of pictures between links, and the draws of the programs that wait are
// stood in (mode 2) or skipped (modes 1, 2) meanwhile. What must not wait never does (the caller classifies the draw: Mario, HUD, 2D, render-to-texture, opaque map geometry).
//
//   mode 0 off (exactly the code before). 1: a draw whose program waits is skipped. 2: it is drawn with a stand-in program (same vertex shader, same samplers, no
//   larger TEV block) when one is linked, else skipped.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vgx::alink {

// Why a link happened on the render thread in play although the switch is on (the census's "sync links" list).
enum Sync : uint8_t {
    kSyncNone = 0,
    kSyncMustFbo,      // the draw goes into a texture (shadow map, blur chain, copy)
    kSyncMustFlat,     // 2D / HUD / screen-space draw (no depth test) or one that covers the viewport
    kSyncMustChara,    // Mario and the main characters (the HD pack's list)
    kSyncMustStatic,   // opaque cached geometry (the map's pieces)
    kSyncNoDraw,       // (not a sync link: an announcement was deferred) -- never counted as a link
    kSyncMissNoWorker, // not in the shader cache and no worker (GXV_ASYNC_SHADERS=0): the compile is on this thread; spaced by missGap pictures
    kSyncAged,         // waited longer than maxWaitPictures: the program is linked now whatever it costs
    kSyncN
};
inline const char* SyncName(unsigned s) {
    static const char* const n[kSyncN] = {"-", "render-to-texture draw", "2D/HUD/full-screen draw", "Mario/main character", "opaque map geometry", "announcement",
                                          "cache miss without a worker", "waited too long"};
    return s < kSyncN ? n[s] : "?";
}

struct Config {
    int mode = 0;                 // GXV_ASYNC_LINK
    double budgetMs = 4.0;        // GXV_ASYNC_LINK_MS: a link may start only while the picture's links so far took less than this
    unsigned gap = 1;             // GXV_ASYNC_LINK_GAP: pictures between two sliced links (1 = one a picture, 0 = only the ms budget limits them)
    unsigned missGap = 30;        // GXV_ASYNC_LINK_MISSGAP: pictures between two links of programs whose shaders are not cached and have no worker
    unsigned maxWait = 600;       // GXV_ASYNC_LINK_MAXWAIT: pictures a program may be deferred before it is linked anyway (0 = never)
    bool missOnly = false;        // noprog26 (GXV_NO_PLAY_COMPILE_STOP with GXV_ASYNC_LINK=0): only programs whose shaders no cache has are gated; cached links go as before, at once
};

enum class Verdict : uint8_t { Link, Defer };
enum Cache : uint8_t { kCacheUnknown = 0, kCacheHit, kCacheMiss };

struct Decision { Verdict v = Verdict::Link; Sync why = kSyncNone; bool sliced = false; };

// Per-stage census (one line when the stage is left).
struct Census {
    uint64_t deferredKeys = 0;     // distinct programs that waited at least once
    uint64_t deferredAsks = 0;     // times a draw (or announcement) was turned away
    uint64_t drawsSkipped = 0, drawsStoodIn = 0;
    uint64_t picturesAffected = 0; // pictures with at least one skipped or stood-in draw
    uint32_t runMax = 0;           // longest run of consecutive affected pictures
    uint32_t waitMax = 0;          // longest a program waited, in pictures, from its first deferral to its link
    uint64_t slicedLinks = 0;      // links done within the budget
    double slicedMs = 0, worstLinkMs = 0;
    uint64_t sync[kSyncN] = {};
    double syncMs[kSyncN] = {};
    uint64_t standInNone = 0;      // mode 2: no stand-in was linked, the draw was skipped
    uint64_t linksInPlay = 0;      // every link a draw made in play (sliced + sync)
    double linksInPlayMs = 0;
    int Format(char* out, size_t cap, const char* name, int mode) const {
        char sy[320] = "";
        size_t k = 0;
        for (unsigned s = 1; s < kSyncN; ++s) {
            if (!sync[s] || s == kSyncNoDraw) continue;
            const int n = std::snprintf(sy + k, sizeof sy - k, "%s%s %u (%.0f ms)", k ? ", " : "", SyncName(s), static_cast<unsigned>(sync[s]), syncMs[s]);
            if (n < 0 || static_cast<size_t>(n) >= sizeof sy - k) break;
            k += static_cast<size_t>(n);
        }
        return std::snprintf(out, cap,
            "asynclink: stage %s: mode %d: links deferred %u programs (%u asks); pictures with a draw skipped or stood in: %u total, %u in a row at most (%u draws skipped, %u stood in, %u with no stand-in); "
            "longest wait %u pictures; links in play %u (%.0f ms; %u sliced %.0f ms), worst link %.0f ms; synchronous links that still happened: %s",
            name, mode, static_cast<unsigned>(deferredKeys), static_cast<unsigned>(deferredAsks), static_cast<unsigned>(picturesAffected), runMax, static_cast<unsigned>(drawsSkipped),
            static_cast<unsigned>(drawsStoodIn), static_cast<unsigned>(standInNone), waitMax, static_cast<unsigned>(linksInPlay), linksInPlayMs, static_cast<unsigned>(slicedLinks),
            slicedMs, worstLinkMs, k ? sy : "none");
    }
};

class Gate {
public:
    Config cfg;
    Census c;

    // The first call of a picture (any serial that differs from the last) closes the previous one's run.
    void Picture(uint64_t serial) {
        if (serial == picture_) return;
        if (affectedThis_) {
            ++c.picturesAffected;
            ++run_;
            c.runMax = std::max(c.runMax, run_);
        } else {
            run_ = 0;
        }
        affectedThis_ = false;
        spentMs_ = 0;
        picture_ = serial;
    }
    // Is a program of this key waiting? (first deferral picture, 0 when none)
    // `must`: the caller's classification (kSyncMust*), kSyncNone when the draw may wait; kSyncNoDraw for an announcement.
    // `cache`: the probe's answer; `worker`: GXV_ASYNC_SHADERS has a worker that compiles misses (then a miss is the engine's business, not asked here).
    Decision Decide(uint32_t key, Sync must, Cache cache, bool worker) {
        Decision d;
        auto w = wait_.find(key);
        if (must != kSyncNone && must != kSyncNoDraw) { d.why = must; return d; } // linked now, counted by the caller as a synchronous link
        if (w != wait_.end() && cfg.maxWait && picture_ - w->second >= cfg.maxWait) { d.why = kSyncAged; return d; }
        if (cfg.missOnly && cache != kCacheMiss) return d; // (a cached link: as before the switch)
        const bool miss = cache == kCacheMiss && !worker;
        if (miss) {
            if (picture_ - lastMiss_ >= cfg.missGap || lastMiss_ == 0) {
                d.why = kSyncMissNoWorker;
                d.sliced = false;
                return d; // (the caller links it and calls Linked(); the spacing is kept by NoteMissLink)
            }
            return Defer(key);
        }
        if (spentMs_ < cfg.budgetMs && (lastLink_ == 0 || picture_ - lastLink_ >= cfg.gap)) {
            d.sliced = true;
            return d;
        }
        return Defer(key);
    }
    // A link of `key` took `ms` (the render thread's wall time). `sync` is the reason when it was not sliced.
    void Linked(uint32_t key, double ms, bool sliced, Sync why) {
        ++c.linksInPlay;
        c.linksInPlayMs += ms;
        c.worstLinkMs = std::max(c.worstLinkMs, ms);
        spentMs_ += ms;
        lastLink_ = picture_;
        if (why == kSyncMissNoWorker) lastMiss_ = picture_;
        if (sliced) { ++c.slicedLinks; c.slicedMs += ms; }
        else if (why != kSyncNone) { ++c.sync[why]; c.syncMs[why] += ms; }
        auto w = wait_.find(key);
        if (w != wait_.end()) {
            c.waitMax = std::max<uint32_t>(c.waitMax, static_cast<uint32_t>(picture_ - w->second));
            wait_.erase(w);
        }
    }
    // noprog26: a compile that was not a whole link (the first of a program's two shaders, without a worker) used this gap's slot.
    void MissSpent(double ms) { lastMiss_ = picture_; ++c.sync[kSyncMissNoWorker]; c.syncMs[kSyncMissNoWorker] += ms; }
    // A draw was turned away (skipped or stood in).
    void DrawSkipped(bool standIn, bool noStandIn) {
        affectedThis_ = true;
        if (standIn) ++c.drawsStoodIn; else ++c.drawsSkipped;
        if (noStandIn) ++c.standInNone;
    }
    size_t Waiting() const { return wait_.size(); }
    void Reset() { c = Census{}; wait_.clear(); run_ = 0; affectedThis_ = false; spentMs_ = 0; lastLink_ = lastMiss_ = 0; }
    // Close the stage: the current picture counts too.
    void Flush() {
        if (affectedThis_) { ++c.picturesAffected; ++run_; c.runMax = std::max(c.runMax, run_); affectedThis_ = false; }
    }
    uint64_t PictureNow() const { return picture_; }

private:
    Decision Defer(uint32_t key) {
        Decision d;
        d.v = Verdict::Defer;
        ++c.deferredAsks;
        if (wait_.emplace(key, picture_).second) ++c.deferredKeys;
        return d;
    }
    std::unordered_map<uint32_t, uint64_t> wait_; // key -> picture of the first deferral
    uint64_t picture_ = 0, lastLink_ = 0, lastMiss_ = 0;
    double spentMs_ = 0;
    bool affectedThis_ = false;
    uint32_t run_ = 0;
};

// Stand-ins: linked programs by vertex shader. A stand-in for (vs, texMask, nTev) is a linked program of the same vertex shader (same layout, same uniform blocks,
// same attributes) whose fragment shader samples exactly the same samplers and whose TEV block is not larger (the draw's block is cut for the wanted shader; the
// upload takes at most the program's own size). Closest TEV size first, then the most recently drawn.
struct StandInIndex {
    struct E { uint16_t fs; uint8_t texMask; uint8_t nTev; };
    std::unordered_map<uint16_t, std::vector<E>> byVs;
    void Add(uint16_t vs, uint16_t fs, uint8_t texMask, uint8_t nTev) {
        auto& v = byVs[vs];
        for (const E& e : v) if (e.fs == fs) return;
        v.push_back(E{fs, texMask, nTev});
    }
    void Remove(uint16_t vs, uint16_t fs) {
        auto it = byVs.find(vs);
        if (it == byVs.end()) return;
        auto& v = it->second;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const E& e) { return e.fs == fs; }), v.end());
    }
    // `usable(fs)` answers whether that program is linked and fine now and gives its last draw frame through *lastDraw (0 = never drawn); the caller
    // drops entries it finds gone. Returns the fragment shader id, or -1.
    template <class Usable> int Pick(uint16_t vs, uint16_t wantFs, uint8_t texMask, uint8_t nTev, Usable&& usable) const {
        const auto it = byVs.find(vs);
        if (it == byVs.end()) return -1;
        int best = -1;
        int bestGap = 1 << 30;
        uint64_t bestDraw = 0;
        for (const E& e : it->second) {
            if (e.fs == wantFs || e.texMask != texMask || e.nTev > nTev) continue;
            uint64_t last = 0;
            if (!usable(e.fs, &last)) continue;
            const int gap = static_cast<int>(nTev) - static_cast<int>(e.nTev);
            if (gap < bestGap || (gap == bestGap && last > bestDraw)) { best = e.fs; bestGap = gap; bestDraw = last; }
        }
        return best;
    }
};

} // namespace vgx::alink
