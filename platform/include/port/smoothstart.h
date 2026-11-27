// openstall26 (12 Nov 2026): GXV_SMOOTHSTART, the one key for "the first 20 s after the screen opens must not hitch". Pure table + helpers (host test: tests/openstall26_model.cpp).
//
//   GXV_SMOOTHSTART=<0..2>   0 off (default: every member keeps its own switch), 1 the cheap measured set, 2 the same with more room. A member written in env.txt (even as 0) wins;
//                            the level only fills in members left unset (entry.cpp fans it out before the members' defaults, like GXV_ACROSS / GXV_CPULOD).
//   level 1: GXV_PREWARM_RES=2   render targets the stage made in play last time (and the generic 240x191 / 960x544 copy targets) are made behind the closed screen (handover26)
//            GXV_TEXWARM=1       textures first used in the stage's first 15 s are decoded + uploaded behind the closed screen (endwait26; this patch: not while the stage loads,
//                                a spending governor, and the closed screen is held while the list is worked off)
//            GXV_ENDWAIT_LOG=1   the log-only "endwait:" lines for game-thread waits over 40 ms (what the 130-360 ms is, if anything is left)
//            GXV_SMOOTHSTART_MS  400   the most closed-screen time the warm-up may add (the hold, see below) and the estimate it spends on uploads
//   level 2: GXV_TEXWARM=2       also the first 3 s of the open screen, 3 ms / 512 KiB a picture (what the closed budget could not take)
//            GXV_SMOOTHSTART_MS  600
// The hold: StageWarmHold (polled by the game while it waits to start the scene) answers "hold" while textures / targets of the plan are left, at most SMOOTHSTART_MS and not
// while nothing has moved for kStallMs; never where GXV_NO_MIDHOLD says a hold must not start (a restart in play). No game turn is run for it: the scene has not started.
// One census line a stage ("smoothstart:") at +20 s: Format() below.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdio.h>  // (unqualified snprintf: game units map it to port_snprintf, so std::snprintf does not exist there)
#include <cstdlib>
#include <cstring>

namespace port {
namespace smoothstart {

constexpr int kMaxLevel = 2;
constexpr double kStallMs = 200.0;          // nothing of the plan moved for this long: the hold is over
constexpr double kPriorMsPerKiB = 0.25;     // the back end's upload cost before any is seen: the slow c80 tours (1802/1803: 0.23-0.24; c79 and 1810: 0.015)

struct Member {
    const char* name;
    const char* v[kMaxLevel + 1];  // [0] unused
};
inline constexpr Member kMembers[] = {
    {"GXV_PREWARM_RES", {"", "2", "2"}},
    {"GXV_TEXWARM", {"", "1", "2"}},
    {"GXV_ENDWAIT_LOG", {"", "1", "1"}},
    {"GXV_SMOOTHSTART_MS", {"", "400", "600"}},
};
inline constexpr int kMemberCount = static_cast<int>(sizeof kMembers / sizeof kMembers[0]);

inline int ClampLevel(long v) { return v < 0 ? 0 : (v > kMaxLevel ? kMaxLevel : static_cast<int>(v)); }
// The members a level adds, for the env fan-out; `has(name)`: the environment sets it already.
template <class Has, class Emit>
inline int FanOut(int level, Has has, Emit emit) {
    int n = 0;
    if (level <= 0 || level > kMaxLevel) return 0;
    for (int i = 0; i < kMemberCount; ++i) {
        if (has(kMembers[i].name)) continue;
        emit(kMembers[i].name, kMembers[i].v[level]);
        ++n;
    }
    return n;
}

// The spending of the closed-screen warm-up: a ms cap per stage and a running estimate of what a KiB of texture costs the back end (uploads are the part that blocks the
// pipeline: c80 hand-over 66 uploads / 508 KiB / 186 ms in the picture that follows). The estimate is the back end's own totals, seen between two looks.
struct Governor {
    double capMs = 400.0;
    double usedMs = 0.0;
    double msPerKiB = kPriorMsPerKiB;
    bool seen = false;
    uint64_t lastKiB = 0;
    double lastMs = 0.0;
    void Reset(double cap) { capMs = cap < 0.0 ? 0.0 : cap; usedMs = 0.0; }
    // The back end's cumulative texture uploads (KiB, ms): when at least 64 KiB came since the last look, the estimate follows them (half old, half new; 0.005..1.5).
    void Observe(uint64_t totalKiB, double totalMs) {
        if (!seen) { seen = true; lastKiB = totalKiB; lastMs = totalMs; return; }
        if (totalKiB < lastKiB || totalMs < lastMs) { lastKiB = totalKiB; lastMs = totalMs; return; }
        const uint64_t dk = totalKiB - lastKiB;
        if (dk < 64) return;
        const double r = (totalMs - lastMs) / static_cast<double>(dk);
        msPerKiB = std::min(1.5, std::max(0.005, 0.5 * msPerKiB + 0.5 * r));
        lastKiB = totalKiB;
        lastMs = totalMs;
    }
    bool Exhausted() const { return usedMs >= capMs; }
    // KiB the cap still allows at the estimate (at least 1 while not exhausted: one item may always go).
    uint32_t KiBLeft() const {
        if (Exhausted()) return 0;
        const double k = (capMs - usedMs) / msPerKiB;
        return k < 1.0 ? 1u : (k > 4096.0 ? 4096u : static_cast<uint32_t>(k));
    }
    void Spend(uint32_t kib, double frontEndMs) { usedMs += static_cast<double>(kib) * msPerKiB + frontEndMs; }
};

// Whether the closed screen is held for the plan: items left, the cap, and progress.
inline bool HoldWanted(unsigned pending, double heldMs, double capMs, double sinceMoveMs) { return pending > 0 && capMs > 0.0 && heldMs < capMs && sinceMoveMs < kStallMs; }

// What the line says; filled by the back end (targets), the texture cache (textures) and the hitch25 stage clock (the closed phase).
struct Census {
    const char* stage = "?";
    int level = 0;
    double capMs = 0;
    // the closed phase
    double closedMs = 0;       // scene start -> screen open
    unsigned closedPics = 0;
    double holdMs = 0;         // of it the hold's
    unsigned holdPlan = 0;     // plan items left when it began
    const char* holdEnd = "none";
    // created behind the closed screen
    unsigned texClosed = 0;    // textures replayed
    unsigned long long texClosedKiB = 0;
    double texClosedMs = 0;    // front end ms
    unsigned texPlanned = 0, texMiss = 0, texGaveUp = 0;
    unsigned tgtMade = 0;
    double tgtMs = 0;
    // after it opened (first 20 s)
    unsigned texOpenReplay = 0;
    unsigned texFirst = 0;     // first used in play anyway
    unsigned long long texFirstKiB = 0;
    double texFirstMs = 0;
    unsigned tgtTaken = 0, tgtPlay = 0;
    double worstMs = 0, worstAtS = 0;
    const char* worstCause = "none";
    double msPerKiB = 0;
};

inline size_t Format(char* out, size_t cap, const Census& c) {
    const int n = snprintf(out, cap,
        "smoothstart: stage %s: GXV_SMOOTHSTART=%d: closed screen %.0f ms, %u pictures (the hold added %.0f ms of at most %.0f; %u plan items left at its start; ended: %s); "
        "created closed: textures %u (%llu KiB, %.0f ms front end; plan %u, missed %u, given up %u), targets %u (%.0f ms); "
        "created open (first 20 s): textures replayed %u, first used in play %u (%llu KiB, %.0f ms front end), targets made in play %u of which %u premade targets taken; "
        "worst open-screen picture %.0f ms at +%.1f s (%s); back end upload estimate %.3f ms/KiB",
        c.stage, c.level, c.closedMs, c.closedPics, c.holdMs, c.capMs, c.holdPlan, c.holdEnd, c.texClosed, c.texClosedKiB, c.texClosedMs, c.texPlanned, c.texMiss, c.texGaveUp,
        c.tgtMade, c.tgtMs, c.texOpenReplay, c.texFirst, c.texFirstKiB, c.texFirstMs, c.tgtPlay, c.tgtTaken, c.worstMs, c.worstAtS, c.worstCause, c.msPerKiB);
    return n < 0 ? 0 : (static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1);
}

}  // namespace smoothstart
}  // namespace port
