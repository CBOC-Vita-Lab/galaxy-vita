// hitch25 (GXV_HITCH25_LOG, default 1, log only): one "hitch25:" line for each stage, and the pure parts behind it. Header only, standard library only, so the
// game thread (platform/src/port/perf.cpp), the render thread (platform/vgx/src/vgx_backend_gl.cpp) and the host test (tests/hitch25_model.cpp) share the code.
//
// What a stage change looks like on the console (c76 tours, 22 stages): the game thread's longest turn of the first window is 250-650 ms, almost all of it in the
// turn's "end frame" part (the wait for the front end and the back end to take the stage's first pictures), and it lands while the screen is still closed by the
// stage's opening wipe; what the player sees is smaller and later (the scenario starter's two pictures about 8.5 s in, the stage queue's build when the star
// select opens). This line says it per stage, from numbers the renderer and the game thread already have:
//   hitch25: stage <name>: first visible picture at <ms> after the scene start (<n> pictures behind the closed screen), before the reveal: <n> holds >=3
//   refreshes (max <ms>), visible holds >=3 refreshes: first 5 s <n> (max <ms>), later <n> (max <ms>), causes: <cause> <n> (<ms>), ...; queue build <ms>;
//   game turns >=100 ms: before the reveal <n> (max <ms>: <part>; <k> at the star select, <m> behind the wipe), after it <n> (max <ms>: <part>)
// "Hold" is a picture interval of at least 2.5 refreshes (41.7 ms at 60 Hz: the hold the eye counts as three refreshes).
#pragma once
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "port/hitch24.h"

namespace port::hitch25 {

// ---- the game thread's slow turns -----------------------------------------------------------------------------------------------------------------
// A turn start-to-start gap of at least kSlowTurnMs. The game thread notes it (perf.cpp TurnGapNote, one compare a turn), the render thread takes the list.
constexpr unsigned kSlowTurnMs = 100;
constexpr unsigned kTurnRing = 16;
enum TurnState : uint8_t { kMenu = 0, kClosed = 1, kPlay = 2 };  // no stage key or the pause / star select menu; the screen 90 % closed by a wipe; a stage with the screen open
// the largest part of the turn, from the turn profile (GXV_TURN_PROF=1 or GXV_FIRSTUSE_TURN=1; 0xFF when the profile is off)
enum TurnPart : uint8_t { kPartUpdate, kPartCalc, kPartShadow, kPartPtcl, kPartView, kPartFlush, kPartDraw, kPartFeWait, kPartEndFrame, kPartOther, kPartOutside, kPartN, kPartNone = 0xFF };
inline const char* PartName(unsigned p) {
    static const char* const n[kPartN] = {"update", "calc-anim", "shadows", "particles", "view/rest", "flushes", "draw", "front-end wait", "end frame (wait for the pipeline)", "other", "outside the turn (retrace wait)"};
    return p < kPartN ? n[p] : "not timed";
}
struct Turn {
    int64_t endNs = 0;     // when the next turn began (hitch24::NowNs clock)
    uint32_t gapUs = 0;    // the gap: the turn and its retrace wait
    uint32_t partUs = 0;   // the largest part's time (0 when not timed)
    uint8_t state = kPlay;
    uint8_t part = kPartNone;
};

// ---- one stage ------------------------------------------------------------------------------------------------------------------------------------
inline bool IsHold(double intervalMs, double refreshMs = 16.683, double refreshes = 3.0) { return intervalMs >= (refreshes - 0.5) * refreshMs; }

// Appends to out[0..cap) at len (the text stays terminated, len stops at the end of the buffer).
inline void Append(char* out, size_t cap, int& len, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
inline void Append(char* out, size_t cap, int& len, const char* fmt, ...) {
    if (len < 0 || static_cast<size_t>(len) + 1 >= cap) return;
    va_list args;
    va_start(args, fmt);
    const int k = vsnprintf(out + len, cap - static_cast<size_t>(len), fmt, args);
    va_end(args);
    if (k > 0) len = static_cast<size_t>(len + k) < cap ? len + k : static_cast<int>(cap - 1);
}

struct Window {
    unsigned n = 0;
    double maxMs = 0, sumMs = 0;
    void Add(double ms) { ++n; sumMs += ms; if (ms > maxMs) maxMs = ms; }
};
struct Stage {
    char name[48] = "";
    uint32_t key = 0;
    double startMs = 0, openMs = 0;        // first picture of the stage's scene; first picture with the screen open (wall ms of the render thread's clock)
    int64_t openNs = 0;                    // the same instant on the hitch24::NowNs clock (turns are compared with it)
    unsigned beforeOpenPictures = 0;       // pictures the back end finished between the scene start and the reveal
    Window pre, first, later;              // holds before the reveal (star select / load), in the 5 s after it, after those
    unsigned byN[hitch24::kCauseN] = {};
    double byMs[hitch24::kCauseN] = {};
    double buildMs = 0;                    // the stage queue's build at the load start (what the render thread spent in StageBuild)
    unsigned unlisted = 0;
    bool any() const { return startMs > 0 || openMs > 0 || pre.n || first.n || later.n; }
    void Begin(const char* nm, uint32_t k, double buildMs_) {
        *this = Stage{};
        if (nm) snprintf(name, sizeof name, "%s", nm);
        key = k;
        buildMs = buildMs_;
    }
    void Seen(double nowMs, bool sceneKeyIsStage, bool screenLive, int64_t nowNs) {
        if (!sceneKeyIsStage) return;
        if (startMs == 0) startMs = nowMs;
        if (screenLive && openMs == 0) { openMs = nowMs; openNs = nowNs; }
        else if (!screenLive && openMs == 0) ++beforeOpenPictures;
    }
    void Hold(double nowMs, double intervalMs, hitch24::Cause c) {
        if (!IsHold(intervalMs)) return;
        if (openMs == 0) { pre.Add(intervalMs); return; }
        (nowMs - openMs < 5000.0 ? first : later).Add(intervalMs);
        ++byN[c];
        byMs[c] += intervalMs;
    }
    // The line, without the turns (TurnText gives that part).
    size_t Format(char* out, size_t cap, const char* turns) const {
        int len = snprintf(out, cap, "hitch25: stage %s: ", name[0] ? name : "?");
        if (len < 0 || static_cast<size_t>(len) >= cap) return cap ? cap - 1 : 0;
        if (openMs > 0 && startMs > 0) Append(out, cap, len, "first visible picture at %.0f ms after the scene start (%u pictures behind the closed screen)", openMs - startMs, beforeOpenPictures);
        else Append(out, cap, len, "no visible picture (the screen never opened for this stage)");
        Append(out, cap, len, ", before the reveal: %u holds >=3 refreshes (max %.0f ms)", pre.n, pre.maxMs);
        Append(out, cap, len, ", visible holds >=3 refreshes: first 5 s %u (max %.0f ms), later %u (max %.0f ms)", first.n, first.maxMs, later.n, later.maxMs);
        unsigned order[hitch24::kCauseN];
        unsigned m = 0;
        for (unsigned c = 0; c < hitch24::kCauseN; ++c) if (byN[c]) order[m++] = c;
        for (unsigned i = 1; i < m; ++i)
            for (unsigned j = i; j > 0 && byMs[order[j]] > byMs[order[j - 1]]; --j) { const unsigned t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
        Append(out, cap, len, ", causes:");
        if (!m) Append(out, cap, len, " none");
        for (unsigned i = 0; i < m && i < 4; ++i) Append(out, cap, len, "%s %s %u (%.0f ms)", i ? "," : "", hitch24::CauseName(static_cast<hitch24::Cause>(order[i])), byN[order[i]], byMs[order[i]]);
        if (buildMs > 0) Append(out, cap, len, "; queue build %.0f ms", buildMs);
        if (turns && turns[0]) Append(out, cap, len, "; %s", turns);
        return static_cast<size_t>(len) < cap ? static_cast<size_t>(len) : cap - 1;
    }
};

// "game turns >=100 ms: before the reveal 2 (max 360 ms: end frame (wait for the pipeline); 1 at the star select, 1 behind the wipe), after it 1 (max 178 ms: ...)"
// `reveal` is the stage's reveal on the turns' clock (0: never revealed: every turn is "before").
inline size_t TurnText(const Turn* t, unsigned n, int64_t revealNs, char* out, size_t cap) {
    unsigned nb = 0, na = 0, menu = 0, closed = 0;
    uint32_t maxB = 0, maxA = 0;
    uint8_t partB = kPartNone, partA = kPartNone;
    for (unsigned i = 0; i < n; ++i) {
        const bool after = revealNs != 0 && t[i].endNs >= revealNs;
        if (after) {
            ++na;
            if (t[i].gapUs >= maxA) { maxA = t[i].gapUs; partA = t[i].part; }
        } else {
            ++nb;
            (t[i].state == kClosed ? closed : menu) += 1;
            if (t[i].gapUs >= maxB) { maxB = t[i].gapUs; partB = t[i].part; }
        }
    }
    int len = snprintf(out, cap, "game turns >=%u ms: before the reveal %u (max %u ms: %s; %u at the menu / star select, %u behind the wipe), after it %u (max %u ms: %s)",
                            kSlowTurnMs, nb, maxB / 1000, PartName(partB), menu, closed, na, maxA / 1000, PartName(partA));
    return len < 0 ? 0 : (static_cast<size_t>(len) < cap ? static_cast<size_t>(len) : cap - 1);
}

}  // namespace port::hitch25
