// gfx26 (12 Nov 2026): GXV_QUALITY_SET, the umbrella for the picture-quality switches. Pure logic: no game types, no clocks, no GL (host-tested by
// tests/quality_set.cpp). The glue is in platform/src/settings/settings_launch.cpp (ApplyStartup: the Picture mode is known there) and
// vita/vgx-game/entry.cpp (the snapshot of what env.txt set, and the fallback when Settings is off).
//
//   GXV_QUALITY_SET=<0|1|2|auto>
//     0     (code default; kDefaultAuto) nothing is touched: every member keeps its own switch and entry.cpp's default, the picture is exactly as before.
//     1     Performance's "crisp at near-zero cost" subset: members costing under 0.3 ms of GPU a picture (estimates; the console A/B decides).
//     2     Quality's best: level 1 plus the members that cost more or only pay at 960x544 (keeping the whole-picture edge smoothing under GPU load,
//           the geometry-only edge pass, sharper texture LOD with anisotropy, anisotropy on smaller textures, mip chains for textures declared without).
//     auto  the Picture mode's level: Quality 2, Locked 30 2, Performance 1.
//   GXV_QUALITY_SET_PROVEN=1   only the members that have run on a console (proven = true below); the others are left alone. For an A/B of the
//                              unproven members against the rest.
//
// Precedence (the same as GXV_CPULOD): a member that env.txt set itself, even to 0, always wins; the level fills in only the members env.txt left
// alone, and overwrites entry.cpp's and Settings' defaults for them. entry.cpp takes the snapshot of env.txt's keys right after env.txt is read
// (SnapshotUser), before any default is set; without a snapshot (a host test) every member already in the environment counts as the user's.
// A value "-" at a level means "no change from the default at this level".
#pragma once

#include <cstdint>
#include <cstdio>
#include <stdio.h>  // (unqualified snprintf: the Vita build maps it to port_snprintf with a macro, so std::snprintf does not exist there)
#include <cstdlib>
#include <cstring>
#include <string>

namespace port {
namespace qualityset {

enum class Mode : uint8_t { Quality, Locked30, Performance };
constexpr int kMaxLevel = 2;
// The level that applies when GXV_QUALITY_SET is not set at all. false: 0 (this patch changes nothing until it is switched on). The proposal for
// release is true (Quality 2, Locked 30 2, Performance 1) once the console A/B has passed: change this one constant.
constexpr bool kDefaultAuto = false;

struct Member {
    const char* key;
    const char* v1;       // the value at level 1 ("-": none)
    const char* v2;       // the value at level 2 ("-": none)
    bool proven;          // has run on a console in a release-candidate build (or is the shipped default restated)
    const char* what;     // one line for the census / report
};
// Order matters only for the census text. At most 32 members (the user mask is 32 bits).
inline constexpr Member kMembers[] = {
    // --- restated defaults: they make the set self-describing and keep it right if a default is ever changed
    {"GXV_EDGE_AA", "1", "1", true, "whole-picture edge smoothing in the present (5-read pass, HUD redrawn plain)"},
    {"GXV_EDGE_AA_HUDCUT", "1", "1", true, "the edge pass leaves the HUD rectangles out (exact)"},
    {"GXV_ANISO", "3", "3", true, "anisotropic filtering on trilinear textures (3 = the hardware's top mode; console: free at 960x544 and 720x404)"},
    {"GXV_HDTEX_TRILINEAR", "1", "1", true, "HD pack textures sampled trilinear"},
    {"GXV_RUNG_LODBIAS", "100", "100", true, "texture LOD bias of a sub-960 picture: the full share of log2(960 / width)"},
    // --- the one behaviour fix: the controller gave the whole-picture edge smoothing up first in Quality (20 of 22 stages in the locked-30 tour)
    {"GXV_AUTOQ_EDGESTEP", "-", "0", true, "edge smoothing is not the first thing given up under GPU load (the size gives way instead)"},
    {"GXV_PACE23_LOCKED_EDGE", "-", "0", true, "(only with GXV_PACE23=1) Locked 30 does not turn the edge step on by itself either"},
    // --- Mario and the HUD (zero or near-zero cost)
    {"GXV_HUD_NATIVE", "1", "1", false, "the 2D layer (HUD, text) drawn at the panel's 960x544 over an upscaled 3D picture (no extra scene)"},
    {"GXV_PLAYER_DOF", "0", "0", false, "the depth-of-field ramp starts behind Mario (he is never blurred by it)"},
    {"GXV_PLAYER_LODBIAS", "-25", "-50", false, "Mario's mipmapped textures read finer by 1/4 (set 1) or 1/2 (set 2) of a level (hundredths; negative = sharper)"},
    // --- Quality's extras
    {"GXV_EDGE_DEPTH", "-", "1", false, "at 960x544 the edge pass blends only at depth jumps (silhouettes, leaf and fence edges), not texture detail or text"},
    {"GXV_RUNG_LODBIAS_EXTRA_AF", "-", "2", false, "a further 1/4 level of texture LOD bias at every rung while anisotropic filtering is on (host-measured best: -0.25)"},
    {"GXV_ANISO_MINSIZE", "-", "64", false, "anisotropic filtering also on 64..127 texel textures (128 by default)"},
    {"GXV_AUTOMIP", "-", "1", false, "mip chains for opaque textures the game declared without (far ground and walls stop sparkling; at most 6 MiB)"},
};
constexpr int kMemberCount = static_cast<int>(sizeof kMembers / sizeof kMembers[0]);
static_assert(kMemberCount <= 32, "the user mask is 32 bits");

// "-" alone means no change (a value such as "-50" is a value).
inline bool NoChange(const char* v) { return v[0] == '-' && v[1] == 0; }

inline int AutoLevel(Mode m) { return m == Mode::Performance ? 1 : 2; }
inline const char* ModeName(Mode m) { return m == Mode::Performance ? "Performance" : m == Mode::Locked30 ? "Locked 30" : "Quality"; }

struct Parsed {
    int level = 0;
    bool fromEnv = false;  // the text came from GXV_QUALITY_SET
    bool bad = false;      // the text was not 0, 1, 2 or auto: treated as 0
};
// raw: the text of GXV_QUALITY_SET (null or empty: not set).
inline Parsed Resolve(const char* raw, Mode m) {
    Parsed p;
    if (!raw || !*raw) {
        p.level = kDefaultAuto ? AutoLevel(m) : 0;
        return p;
    }
    p.fromEnv = true;
    if (std::strcmp(raw, "auto") == 0) { p.level = AutoLevel(m); return p; }
    if ((raw[0] == '0' || raw[0] == '1' || raw[0] == '2') && raw[1] == 0) { p.level = raw[0] - '0'; return p; }
    p.bad = true;
    return p;
}

// ---- what env.txt set (taken once, before any default is set)
inline uint32_t& UserMask() { static uint32_t m = 0; return m; }
inline bool& Snapshotted() { static bool s = false; return s; }
// get(key) -> the environment's text or null. A key counts as the user's when it has any text.
template <class Get>
inline void SnapshotUser(Get get) {
    uint32_t m = 0;
    for (int i = 0; i < kMemberCount; ++i) {
        const char* v = get(kMembers[i].key);
        if (v && *v) m |= 1u << i;
    }
    UserMask() = m;
    Snapshotted() = true;
}
inline bool& Applied() { static bool a = false; return a; }

// fmt1131: the census text is a std::string (heap): nothing is cut and no 4 KiB array sits on a Vita thread's stack.
struct Text : std::string { operator const char*() const { return c_str(); } };
struct Result {
    int level = 0;
    int set = 0;        // members written
    int kept = 0;       // members left because env.txt set them
    int skipped = 0;    // members with no value at this level, or unproven ones left out by GXV_QUALITY_SET_PROVEN
    Text text;   // (fmt1131: std::string, see mode_keys.h: heap, unbounded, never cut)
};
// Applies a level. get(key) -> current text or null; put(key, value) writes with overwrite. userMask: bit i = member i was set by env.txt.
// provenOnly: leave the unproven members alone. The census text names every member written, kept and left.
template <class Get, class Put>
inline Result Apply(int level, Mode mode, const char* source, bool provenOnly, uint32_t userMask, Get get, Put put) {
    Result r;
    r.level = level < 0 ? 0 : (level > kMaxLevel ? kMaxLevel : level);
    auto add = [&](const char* s) { r.text.append(s); };
    char buf[160];
    snprintf(buf, sizeof buf, "qualityset: GXV_QUALITY_SET=%d (%s mode, %s%s)", r.level, ModeName(mode), source ? source : "?", provenOnly ? ", proven members only" : "");
    add(buf);
    if (r.level == 0) {
        add(": nothing touched");
        return r;
    }
    std::string keptList, leftList;   // (fmt1131: were 640 / 320 byte arrays that dropped the rest without a word)
    char tmp[256];
    add(": set");
    for (int i = 0; i < kMemberCount; ++i) {
        const Member& m = kMembers[i];
        const char* v = r.level == 1 ? m.v1 : m.v2;
        if (!v || NoChange(v)) { ++r.skipped; continue; }
        if (provenOnly && !m.proven) {
            ++r.skipped;
            leftList += ' '; leftList += m.key;
            continue;
        }
        if (userMask & (1u << i)) {
            ++r.kept;
            const char* cur = get(m.key);
            snprintf(tmp, sizeof tmp, " %s=%s(level %d would set %s)", m.key, cur ? cur : "?", r.level, v); keptList += tmp;
            continue;
        }
        put(m.key, v);
        ++r.set;
        char kv[96];
        snprintf(kv, sizeof kv, " %s=%s%s", m.key, v, m.proven ? "" : "*");
        add(kv);
    }
    snprintf(buf, sizeof buf, "; %d written, %d kept, %d not in this level or left out", r.set, r.kept, r.skipped);
    add(buf);
    if (!keptList.empty()) { add("; kept from env.txt:"); add(keptList.c_str()); }
    if (!leftList.empty()) { add("; left out (unproven):"); add(leftList.c_str()); }
    add(" (* = not yet run on a console)");
    return r;
}

}  // namespace qualityset
}  // namespace port
