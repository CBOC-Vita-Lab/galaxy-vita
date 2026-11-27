// q30-26 (12 Nov 2026): the Quality (30) mode's umbrella levels, applied once when the picture mode is known as 30.
//
// The Quality mode KEEPS the barely-noticeable ('b') class (stage_knob.h Desc::ex) so that it has headroom to hold a locked 30 in every stage while looking its best. The umbrellas get a "q" level
// there: cpulod.h (kMembers[].q: far models' material refresh, far small characters' and marked families' pose, every 2nd turn, beyond 3000 units, movement-guarded), across.h (the exact members and the
// silhouette gate confirmed by the occlusion query), glow26.h (kQ: GXV_IND_FLAT=1 and the exact light-volume cull only). The governors (GXV_FE_GOV / GXV_GAME_GOV) are allowed in Quality at level 1
// (= the q level) and aim at the 30 budget (perf.cpp). Far draw cuts, particle / star-bit thinning, far logic, low-resolution effects, bloom folds, 848x480 stay Performance's.
//
// entry.cpp reads env.txt and fans GXV_CPULOD / GXV_ACROSS out BEFORE the mode is known; this rewrites the members that fan-out wrote (cpulod::g_fanned, across::g_fanned) to their q values, sets the
// stage-knob mode flag (the tables, the governors and glow26::ClampLevel read it) and shields star bits and coins (GXV_PROTECT_SPRITES=1) unless env.txt says otherwise. Idempotent.
#pragma once

#include <cstdio>
#include <stdio.h>  // (unqualified snprintf: game units map it to port_snprintf, so std::snprintf does not exist there)
#include <cstdlib>
#include <cstring>
#include "across.h"
#include "cpulod.h"
#include "stage_knob.h"

namespace port {
namespace quality30 {

inline bool& Applied() { static bool a = false; return a; }
// get(key) -> the environment's text or null; put(key, text) overwrites. Returns the census text for the basic log in `text` (empty when nothing was needed) and true the first time.
template <class Get, class Put>
inline bool Apply(Get get, Put put, char* text, size_t cap) {
    if (Applied()) return false;
    Applied() = true;
    stageknob::SetQuality(true);
    const int a = cpulod::ApplyQualityEnv([&](const char* k, const char* v) { put(k, v); });
    const int b = across::ApplyQualityEnv([&](const char* k, const char* v) { put(k, v); });
    const char* sp = get("GXV_PROTECT_SPRITES");
    const bool shield = !sp || !*sp || std::strcmp(sp, "0") == 0;   // (entry.cpp's default is "0": only an explicit 1 or 2 is the user's)
    if (shield) put("GXV_PROTECT_SPRITES", "1");
    // q30lock26 (12 Nov): the lock ladder (port/q30lock.h) and the even-cadence rule it relies on (pace26's GXV_EVEN26_Q=4: a late turn pays its catch-up on the turn that is left out anyway) are the
    // Quality mode's defaults. Unset or entry.cpp's "0" means "not chosen"; a non-zero value of env.txt wins. GXV_Q30_LOCK=0 turns the ladder off (the governors then run as in q30-26).
    const char* lk = get("GXV_Q30_LOCK");
    const bool lockDefault = !lk || !*lk;
    if (lockDefault) put("GXV_Q30_LOCK", "1");
    const char* ev = get("GXV_EVEN26_Q");
    const bool evenDefault = !ev || !*ev || std::strcmp(ev, "0") == 0;
    if (evenDefault) put("GXV_EVEN26_Q", "4");
    if (text && cap) snprintf(text, cap, "quality30: Quality (30): umbrella q level: %d cpulod members and %d across members rewritten from the fan-out, star bits and coins shielded from the pose/material cuts: %s; lock ladder GXV_Q30_LOCK %s, GXV_EVEN26_Q %s",
                                   a, b, shield ? "yes" : "kept env.txt's value", lockDefault ? "1 (default)" : "from env.txt", evenDefault ? "4 (default)" : "from env.txt");
    // strk1118: an index the fan-out record could not hold is loud, not silent (cannot happen while the static_asserts in cpulod.h / across.h hold)
    if (text && cap && (cpulod::g_fannedLost || across::g_fannedLost)) {
        const size_t used = std::strlen(text);
        if (used + 1 < cap) snprintf(text + used, cap - used, "; WARNING: %d cpulod and %d across fan-out members were not recorded and will not be rewritten for Quality", cpulod::g_fannedLost, across::g_fannedLost);
    }
    return true;
}

}  // namespace quality30
}  // namespace port
