#pragma once
// pace33 PATCH8 (14 Nov 2026): the Performance mode's two umbrella defaults, GXV_CPULOD=2 and GXV_ACROSS=1, for a card that has only env-user.txt.
// Pure logic over get / put callbacks: tests/pace33_defaults_test.cpp runs this code.
//
// Why not a plain setenv default. The two umbrellas fan their members out at start-up, BEFORE the picture mode is known (entry.cpp), and each member then gets its own default.
// A default of 2 / 1 there would also reach the Quality mode (it would rewrite the fanned members to its q level: a change of Quality's picture). So:
//   - SnapshotUser, right after env.txt is read: which of the umbrella keys and their members the user wrote (anything env.txt names, even 0, is the user's);
//   - ApplyPerformance, once the mode is known to be Performance (settings_launch.cpp, beside the per-mode key table): for an umbrella the user did not write, the level is set
//     and every member the user did not write is overwritten with the level's value, exactly the values the start-up fan-out writes for GXV_CPULOD=2 / GXV_ACROSS=1 in env.txt.
// Quality never calls ApplyPerformance. GXV_CPULOD=0 / GXV_ACROSS=0 in env.txt switch each back.
#include "port/across.h"
#include "port/cpulod.h"
#include <cstdint>
#include <stdio.h>
#include <cstring>

namespace port {
namespace perfdefaults {

inline constexpr int kCpulodLevel = 2, kAcrossLevel = 1;
static_assert(cpulod::kMemberCount <= 64, "User::cpulodMembers is 64 bits");   // (strk1118: was a silent `i < 64` cut)
static_assert(across::kMemberCount <= 32, "User::acrossMembers is 32 bits");
struct User {
    bool snapshotted = false, cpulod = false, across = false, farlogic = false;
    uint64_t cpulodMembers = 0;
    uint32_t acrossMembers = 0;
};
inline User& U() { static User u; return u; }

template <class Get>
inline void SnapshotUser(Get get) {
    User u;
    auto set = [&](const char* k) { const char* v = get(k); return v && *v; };
    u.cpulod = set("GXV_CPULOD");
    u.across = set("GXV_ACROSS");
    u.farlogic = set("GXV_FARLOGIC");
    for (int i = 0; i < cpulod::kMemberCount && i < 64; ++i)
        if (set(cpulod::kMembers[i].name)) u.cpulodMembers |= 1ull << i;
    for (int i = 0; i < across::kMemberCount && i < 32; ++i)
        if (set(across::kMembers[i].name)) u.acrossMembers |= 1u << i;
    u.snapshotted = true;
    U() = u;
}

// Returns the number of switches written; `text` gets the census line ("perfdefaults: ...").
template <class Put>
inline int ApplyPerformance(Put put, char* text, size_t cap) {
    const User& u = U();
    int nc = 0, na = 0;
    char b[32];
    if (u.snapshotted && !u.cpulod) {
        put("GXV_CPULOD", "2");
        for (int i = 0; i < cpulod::kMemberCount && i < 64; ++i) {
            const cpulod::Member& m = cpulod::kMembers[i];
            if (!m.existing || ((u.cpulodMembers >> i) & 1ull)) continue;
            snprintf(b, sizeof b, "%g", static_cast<double>(m.lv[kCpulodLevel]));
            put(m.name, b);
            cpulod::NoteFanned(i);
            ++nc;
        }
        if (!u.farlogic) put("GXV_FARLOGIC", "2");
    }
    if (u.snapshotted && !u.across) {
        put("GXV_ACROSS", "1");
        for (int i = 0; i < across::kMemberCount && i < 32; ++i) {
            const float v = across::kMembers[i].lv[kAcrossLevel];
            if (v == 0.0f || ((u.acrossMembers >> i) & 1u)) continue;
            snprintf(b, sizeof b, "%g", static_cast<double>(v));
            put(across::kMembers[i].name, b);
            across::NoteFanned(i);
            ++na;
        }
    }
    if (text && cap)
        snprintf(text, cap, "perfdefaults: Performance mode: GXV_CPULOD %s (%d members written), GXV_ACROSS %s (%d members written)%s",
                 !u.snapshotted ? "left (no snapshot)" : u.cpulod ? "kept from env.txt" : "= 2 (the mode's default)", nc,
                 !u.snapshotted ? "left (no snapshot)" : u.across ? "kept from env.txt" : "= 1 (the mode's default)", na,
                 u.snapshotted && !u.cpulod && !u.farlogic ? ", GXV_FARLOGIC=2" : "");
    return nc + na;
}

}  // namespace perfdefaults
}  // namespace port
