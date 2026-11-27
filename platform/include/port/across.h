// across26 (12 Nov 2026): GXV_ACROSS, the umbrella for the cuts that every stage can use (the costs the player pays in all or most stages: the silhouette's
// capture and fill, the indirect-effect captures, the shadow-volume alpha capture). Pure table + helpers, host-tested by tests/across_test.cpp.
//
//   GXV_ACROSS=<0..2>   0 off (default: every member keeps its own switch), 1 mild, 2 stronger. A member written on its own (env.txt, even as 0, or a stage preset
//                       line) always wins; the level fills in only the members left unset. In an env file `GXV_ACROSS=N` (entry.cpp fans it out before the
//                       members' defaults are set); in a stage preset line `across=N` (ValidateKv expands it into the stage-settable members it does not name).
//   Performance mode would use 1 or 2, Quality 0 (nothing is changed).
//
//   level 1:  GXV_SILH_GATE=1 + GXV_SILH_OCC=1   the silhouette's capture and fill are left out while Mario is in the open: the game's collision rays say so AND the
//                                                 occlusion query has seen no pixel of him behind anything for 12 pictures (needs one hit seen since start-up)
//             GXV_INDCAP_SKIP=1                  an indirect-effect screen capture nobody reads is not made (pixcuts25: exact; refreshed on first sight)
//   level 2:  GXV_SILH_OCC=2                     the same with a 5-picture hold and no proof needed
//             GXV_SHADOW_STENCIL=1               shadow volumes counted in the stencil instead of the alpha capture (bisect25: +2.5..+3.5 fps in 5 stages; the stencil
//                                                 plane costs 1 byte a pixel; windows that do not check out fall back to the capture)
//             GXV_FIRSTCLEAR=1, GXV_CLEARQ=1     (scenes26) the picture's first scene loads no depth, its clear quad is a clear
//             GXV_TAILDEPTH=1                    (scenes26) the last scene of a picture stores no depth once 120 pictures in a row began with a whole-depth overwrite
// Not in the table on purpose: GXV_DIRECT_DISPLAY (needs the vp28+ vitaGL patch), the glow/bloom switches (glow26 owns them), anything that changes shader text.
#pragma once

#include <cstdio>
#include <stdio.h>  // (unqualified snprintf: game units map it to port_snprintf, so std::snprintf does not exist there)
#include <cstdlib>
#include <cstring>
#include "stage_knob.h"

namespace port {
namespace across {

constexpr int kMaxLevel = 2;

struct Member {
    const char* name;
    float lv[kMaxLevel + 1];  // [0] unused
    float q = 0;              // q30-26: the member's value in the Quality (30) mode's "q" level (0 = not in it): the exact members (INDCAP_SKIP, CLEARQ) and the silhouette gate confirmed by the
                              // occlusion query at level 1 (SILH_GATE=1 + SILH_OCC=1: fail-closed; the outline can be missing for the pictures between Mario going behind collision-free
                              // foliage and the query's first hit). Not in it: the shadow stencil (never run on a console since the stencil43 fix) and FIRSTCLEAR / TAILDEPTH (depth plan, a vitaGL
                              // pool-reuse limit, never run on a console): promote them by giving them a q value once a console A/B shows the same picture.
};
inline constexpr Member kMembers[] = {
    {"GXV_SILH_GATE", {0, 1, 1}, 1},
    {"GXV_SILH_OCC", {0, 1, 2}, 1},
    {"GXV_INDCAP_SKIP", {0, 1, 1}, 1},
    {"GXV_SHADOW_STENCIL", {0, 0, 1}},
    // scenes26: the picture's first scene loads no depth and starts with the clear quad as a tile clear; the last scene stores no depth the next picture would only overwrite
    // (GXV_TAILDEPTH, vgx_scenes26.h). FIRSTCLEAR and CLEARQ have no stage key (read once at start): the env fan-out sets them, a preset line's `across=2` reaches only tail_depth.
    {"GXV_FIRSTCLEAR", {0, 0, 1}},
    {"GXV_CLEARQ", {0, 0, 1}, 1},
    {"GXV_TAILDEPTH", {0, 0, 1}},
};
inline constexpr int kMemberCount = static_cast<int>(sizeof kMembers / sizeof kMembers[0]);

inline int ClampLevel(long v) { return v < 0 ? 0 : (v > kMaxLevel ? kMaxLevel : static_cast<int>(v)); }
// The level in force: the stage's `across`, else the environment's GXV_ACROSS.
inline int Level() { return ClampLevel(stageknob::Int("GXV_ACROSS", 0)); }
inline bool TableValue(int level, const char* name, float* out) {
    if (level <= 0 || level > kMaxLevel) return false;
    for (int i = 0; i < kMemberCount; ++i)
        if (std::strcmp(kMembers[i].name, name) == 0) {
            if (stageknob::Quality()) {   // (q30-26: any level is the "q" level in Quality)
                if (!(kMembers[i].q > 0.0f)) return false;
                if (out) *out = kMembers[i].q;
                return true;
            }
            if (out) *out = kMembers[i].lv[level];
            return true;
        }
    return false;
}
// The members a level adds, for the env fan-out: the value as text only if it is not 0 (a 0 member is the default anyway). `has(name)`: the environment sets it.
template <class Has, class Emit>
inline int FanOut(int level, Has has, Emit emit, bool quality = false) {
    int n = 0;
    if (level <= 0 || level > kMaxLevel) return 0;
    for (int i = 0; i < kMemberCount; ++i) {
        if (has(kMembers[i].name)) continue;
        if (quality && !(kMembers[i].q > 0.0f)) continue;   // (q30-26: Quality takes the q members only)
        emit(kMembers[i].name, quality ? kMembers[i].q : kMembers[i].lv[level]);
        ++n;
    }
    return n;
}
// The preset line `across=N`: the members the line did not name. `has(name)` says whether the line sets that switch itself.
template <class Has, class Emit>
inline void Expand(int level, Has has, Emit emit, bool quality = false) {
    if (level <= 0 || level > kMaxLevel) return;
    for (int i = 0; i < kMemberCount; ++i) {
        const float v = quality ? kMembers[i].q : kMembers[i].lv[level];   // (q30-26: Quality takes the q members only)
        if (v != 0.0f && !has(kMembers[i].name)) emit(kMembers[i].name, v); // (a member that is 0 at this level is left to its own switch)
    }
}
// q30-26: the picture mode turned out to be Quality (30) after the env fan-out wrote the Performance rows. Rewrites the members the fan-out wrote (g_fanned) to their q value, or leaves them 0.
// strk1118: 16 -> 32 (7 members today, 22 %), static_assert + a host test at 75 % (tests/table_headroom.cpp), a lost index is counted (g_fannedLost) and shown in the quality30 census.
inline constexpr int kFannedCap = 32;
inline bool g_fanned[kFannedCap] = {};
inline int g_fannedLost = 0;
static_assert(kMemberCount <= kFannedCap, "g_fanned");
inline void NoteFanned(int i) { if (i >= 0 && i < kFannedCap) g_fanned[i] = true; else ++g_fannedLost; }
template <class Set>
inline int ApplyQualityEnv(Set set) {
    int n = 0;
    for (int i = 0; i < kMemberCount && i < kFannedCap; ++i) {
        if (!g_fanned[i]) continue;
        char b[32];
        snprintf(b, sizeof b, "%g", static_cast<double>(kMembers[i].q > 0.0f ? kMembers[i].q : 0.0f));
        set(kMembers[i].name, b);
        ++n;
    }
    return n;
}

}  // namespace across
}  // namespace port
