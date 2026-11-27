// hrwfeel36 (21 Nov 2026): the "hrw: feel |" line, the Vita number of the 60 Assist decision rule (shimmer/HRW-DESIGN.md S3d). Log only: it changes no picture.
// One event per pair: the second picture was shown (by the warp kind it was drawn with), or lost (A shown again) on the game side (camera cut, guard band, a scene
// change), on the render side (queue / buffer / id / not presentable) or by the warp's own fall-back reason. From the stream, per window:
//   shown B of pairs %, lost B a second (split), isolated losses (one lost pair between shown ones), bursts (3 or more lost within 1 s), the longest run of unbroken
//   shown pairs (s), warp kind pair counts (depth / plane / rotation only), the number of changes of kind between consecutive shown pairs, pairs drawn with point
//   reads because the camera moved under GXV_HRW_WARP_SNAP.
// Pure arithmetic (host test: tests/hrw_feel_test.cpp); the caller supplies the clock in ms and a lock where two threads feed it.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace port::hrwfeel {

enum Kind { kDepth = 0, kPlane = 1, kRot = 2, kKinds = 3 };
enum Side { kGame = 0, kRender = 1, kWarp = 2 };
constexpr int kWhy = 16;

class Feel {
public:
    void Shown(double nowMs, int kind) {
        if (kind < 0 || kind >= kKinds) kind = kRot;
        Begin(nowMs);
        ++shown_; ++kindN_[kind];
        if (lastKind_ >= 0 && lastKind_ != kind) ++changes_;
        lastKind_ = kind;
        if (lostRun_ == 1) ++isolated_;
        lostRun_ = 0;
        if (runStart_ < 0.0) runStart_ = nowMs;
        runLast_ = nowMs;
    }
    // side: kGame / kRender / kWarp; why: the warp's reason index (kWarp only)
    void Lost(double nowMs, int side, int why = 0) {
        Begin(nowMs);
        if (side < 0 || side >= 3) side = kRender;
        ++lost_; ++lostSide_[side];
        if (side == kWarp) ++why_[std::clamp(why, 0, kWhy - 1)];
        ++lostRun_;
        CloseRun();
        // burst: three lost pairs inside 1 s (counted once; the window restarts after it)
        recent_[nRecent_ % 3] = nowMs;
        ++nRecent_;
        if (nRecent_ >= 3 && nowMs - recent_[(nRecent_ - 3) % 3] <= 1000.0) { ++bursts_; nRecent_ = 0; }
    }
    void Snapped() { ++snapped_; }
    // the window's figures (nowMs - window start = seconds); then the window is cleared (the open run goes on, counted from the new window's start)
    struct Report {
        double seconds = 0, shownPct = 0, lostPerSec = 0, longestRunS = 0;
        unsigned shown = 0, lost = 0, lostSide[3] = {}, why[kWhy] = {}, isolated = 0, bursts = 0, kind[kKinds] = {}, changes = 0, snapped = 0;
    };
    Report Take(double nowMs) {
        Report r;
        r.seconds = started_ ? std::max((nowMs - t0_) / 1000.0, 0.001) : 0.0;
        r.shown = shown_; r.lost = lost_;
        for (int i = 0; i < 3; ++i) r.lostSide[i] = lostSide_[i];
        for (int i = 0; i < kWhy; ++i) r.why[i] = why_[i];
        r.isolated = isolated_;
        r.bursts = bursts_;
        for (int i = 0; i < kKinds; ++i) r.kind[i] = kindN_[i];
        r.changes = changes_; r.snapped = snapped_;
        const unsigned pairs = shown_ + lost_;
        r.shownPct = pairs ? 100.0 * shown_ / pairs : 0.0;
        r.lostPerSec = r.seconds > 0.0 ? lost_ / r.seconds : 0.0;
        double longest = longest_;
        if (runStart_ >= 0.0) longest = std::max(longest, runLast_ - std::max(runStart_, t0_));
        r.longestRunS = longest / 1000.0;
        shown_ = lost_ = isolated_ = bursts_ = changes_ = snapped_ = 0;
        for (int i = 0; i < 3; ++i) lostSide_[i] = 0;
        for (int i = 0; i < kWhy; ++i) why_[i] = 0;
        for (int i = 0; i < kKinds; ++i) kindN_[i] = 0;
        longest_ = 0.0;
        t0_ = nowMs;
        return r;
    }
    // "hrw: feel | ..." (whyNames: kWhy names or null)
    static int Format(char* out, size_t cap, const Report& r, const char* const* whyNames = nullptr) {
        char why[220]; int o = 0; why[0] = 0;
        for (int i = 1; i < kWhy && o < 180; ++i)
            if (r.why[i]) o += std::snprintf(why + o, sizeof why - static_cast<size_t>(o), "%s%s %u", o ? ", " : "", whyNames && whyNames[i] ? whyNames[i] : "?", r.why[i]);
        const unsigned pairs = r.shown + r.lost;
        return std::snprintf(out, cap,
            "hrw: feel | %.0f s: pairs %u, second pictures shown %.1f %%, lost %u (%.2f a second: game side %u, render side %u, warp %u [%s]) | isolated losses %u, bursts (3+ in 1 s) %u, "
            "longest unbroken run %.1f s | warp kind: depth %u, plane %u, rotation only %u, changes of kind %u (%.1f a minute) | snapped (point reads under GXV_HRW_WARP_SNAP) %u",
            r.seconds, pairs, r.shownPct, r.lost, r.lostPerSec, r.lostSide[kGame], r.lostSide[kRender], r.lostSide[kWarp], o ? why : "none", r.isolated, r.bursts, r.longestRunS,
            r.kind[kDepth], r.kind[kPlane], r.kind[kRot], r.changes, r.seconds > 0.0 ? r.changes * 60.0 / r.seconds : 0.0, r.snapped);
    }
private:
    void Begin(double nowMs) { if (!started_) { started_ = true; t0_ = nowMs; } }
    void CloseRun() {
        if (runStart_ >= 0.0) longest_ = std::max(longest_, runLast_ - std::max(runStart_, t0_));
        runStart_ = -1.0;
    }
    bool started_ = false;
    double t0_ = 0, runStart_ = -1.0, runLast_ = 0, longest_ = 0, recent_[3] = {};
    unsigned shown_ = 0, lost_ = 0, lostSide_[3] = {}, why_[kWhy] = {}, isolated_ = 0, bursts_ = 0, kindN_[kKinds] = {}, changes_ = 0, snapped_ = 0, lostRun_ = 0, nRecent_ = 0;
    int lastKind_ = -1;
};

// The one instance (render thread: the warp; game thread: pairs lost before the render side); the lock is taken 30 times a second at most.
inline Feel& Global() { static Feel f; return f; }
inline std::mutex& GlobalMutex() { static std::mutex m; return m; }

}  // namespace port::hrwfeel
