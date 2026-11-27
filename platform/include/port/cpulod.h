// cpulod26 (12 Nov 2026): GXV_CPULOD, the umbrella for the CPU level-of-detail cuts, and the pure parts of the three new cuts. Host-tested by
// tests/cpulod.cpp. Everything defaults to 0 / off.
//
//   GXV_CPULOD=<0..3>   0 off (every member keeps its own switch), 1 mild, 2 strong, 3 strongest look-alikes (cpu2-26; meant for the governors, not as a static choice). A member switch that is set on its own (the environment,
//                       env.txt, or a stage preset line) always wins; the level only fills in the members left unset. In a stage preset line the key
//                       is `cpulod=N` (ValidateKv expands it); in an env file `GXV_CPULOD=N` (entry.cpp fans it out before the defaults are set,
//                       and the three new cuts read it themselves through port::cpulod::Num).
//
//   NEW CUTS (this file's clients: ModelManager.cpp, LiveActor.cpp, ClippingActorHolder.cpp):
//   GXV_MATLOD=<n>      (2..4) a model far from the camera AND the player (GXV_MATLOD_DIST, plus its radius) refreshes its material display list
//                       (texture-animation / colour-animation frames, the TEV colours) only every n-th turn; the other turns keep the last list.
//                       Joints, logic, the BCK clock, the frame counters of the material players (they advance in movement) are untouched.
//   GXV_FAMPOSE=<n>     (2..4) the actors of the marked families (star bits, coins, the three chasers, bees, the rolling rock; mFlFam) far from the
//                       camera AND the player (GXV_FAMPOSE_DIST) pose their joints every n-th turn. Unlike GXV_CHARPOSE there is no size limit (the
//                       Honeyhive draw census has nothing under 12 lines on the picture, so a size limit would take none of them); the movement guard is
//                       GXV_FAMPOSE_MOVE lines on the picture since the last pose (an actor that travels more is posed every turn).
//   GXV_PIECETHIN=<n>   (2..4) of the star bits (family 1) far from the camera AND the player (GXV_PIECETHIN_DIST) only one in n is drawn (the draw-only
//                       stamp of GXV_FARDRAW, so also left out of the pose and material flushes); movement, sensors, collecting by pointer unchanged.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdio.h>  // (unqualified snprintf: game units map it to port_snprintf, so std::snprintf does not exist there)
#include <cstdlib>
#include <cstring>
#include "stage_knob.h"
#include "cliprange.h"
#include "farfield.h"   // (far30: GXV_FARFIELD, the far-field horizons)
#include "pose_phase.h"  // (ratecut31: GXV_POSE_PHASE, SkipAt below)

namespace port {
namespace cpulod {

constexpr int kMaxLevel = 3;
// q30lock26 (12 Nov): in the Quality (30) mode with GXV_Q30_LOCK the level the cuts are given is a CODED level, port/q30lock.h CpuLodLevel: bit 8 (kLockFlag) set, bit 0 the baseline ('b' set = the q
// values), bits 1..4 the ladder's steps 3a..3d. A member's value there is its q value; a member with a qStep (1..4 = 3a..3d) takes `qv` while that step is in force (a q member takes qv instead of q).
constexpr int kLockFlag = 0x100;
inline bool Coded(int level) { return (level & kLockFlag) != 0; }
inline int CodedSteps(int level) { return (level >> 1) & 0x1F; }   // (q30fin26: 5 bits, steps 3a..3e)  // (cpu2-26: level 3 = the strongest look-alikes; the governors may use it, a static GXV_CPULOD=3 may too)

// One member of the umbrella: the switch's own name and its value at level 1 and 2. A member is applied only when nothing else set it.
struct Member {
    const char* name;
    float lv[kMaxLevel + 1];  // [0] unused
    bool existing;            // a switch of an earlier agent (entry.cpp sets its default, so the fan-out must come first); false: read through Num (or, for the
                              // GXV_PB_* members of cpu2-26, through perf.cpp's PbGovern: they act only where the Performance budget is on)
    bool perfOnly = false;    // cpu2-26: read by perf.cpp (PbGovern) from the level itself, so a preset line's `cpulod=N` does NOT expand it into a stage key (a stage key would
                              // act whether or not the budget is on)
    float q = 0;              // q30-26: this member's value in the QUALITY (30) mode's "q" level (0 = not a member of it). The q level is the barely-noticeable subset at its mildest values: a far
                              // model's material list / a far small character's or marked-family actor's joints refreshed every 2nd turn (beyond 3000 units from camera AND player, small on the picture,
                              // movement-guarded). Anything that thins, drops or lowers the resolution of something visible (far draw cuts, particle / star-bit thinning, far logic) is not in it.
    uint8_t qStep = 0;        // q30lock26: the ladder step (1..4 = 3a..3d) that switches this member to `qv`; 0 = none (the ladder never touches it)
    float qv = 0;             // q30lock26: the value while that step is in force
    uint8_t qStep2 = 0;       // q30fin26: a second step (5 = 3e, the game thread's strongest subtle member) that moves this member further: it takes `qv2` while that step is in force
    float qv2 = 0;
};
inline constexpr Member kMembers[] = {
    // cpucuts25 / cliprange25: the far speck cut and the characters' pose rule. FARDRAW_KINDS 31 = decoration, decoration with effects, enemies, map objects, NPCs.
    {"GXV_FARDRAW", {0, 8, 12, 16}, true},
    {"GXV_FARDRAW_KINDS", {0, 31, 31, 31}, true},
    {"GXV_FARDRAW_HOLD", {0, 20, 30, 45}, true},
    {"GXV_FARDRAW_FLUSH", {0, 1, 1, 1}, true},
    {"GXV_FARANIM", {0, 2, 3, 4}, false},   // (far30: far small decoration poses every n-th turn; the governors' level gives n, the phase spread by address; read through port_cr_faranim_n)
    {"GXV_CHARPOSE", {0, 2, 3, 4}, true, false, 2, 1, 3, 5, 4},
    {"GXV_CHARPOSE_LINES", {0, 12, 24, 40}, true, false, 12, 1, 18, 5, 24},
    // the Performance-mode budget's distances (they act only where GXV_PERF_BUDGET is on, as in every 60 mode env file)
    {"GXV_PB_ANIM", {0, 1600, 1200, 900}, true},
    {"GXV_PB_SHADOW", {0, 2200, 1800, 1500}, true},
    // gamecpu25: far particles drawn thinned
    {"GXV_PTCL_THIN", {0, 0.60f, 0.35f, 0.20f}, true, false, 0, 3, 0.60f},
    {"GXV_PTCL_THIN_D0", {0, 2500, 2000, 1500}, true, false, 0, 3, 2500},
    {"GXV_PTCL_THIN_D1", {0, 7000, 5000, 4000}, true, false, 0, 3, 7000},
    // cpulod26: the three new cuts (no entry.cpp default: unset means "the level decides")
    {"GXV_MATLOD", {0, 2, 3, 4}, false, false, 2, 1, 3, 5, 4},
    {"GXV_MATLOD_DIST", {0, 3000, 2200, 1800}, false, false, 3000, 0, 0, 5, 2500},   // (q30fin26: qStep 5 = 3e; recover30: from 2500 units = 25 m, the owner's floor for close items, was 2200)
    {"GXV_FAMPOSE", {0, 2, 3, 4}, false, false, 2, 1, 3, 5, 4},
    {"GXV_FAMPOSE_DIST", {0, 3000, 2200, 1800}, false, false, 3000, 0, 0, 5, 2500},
    {"GXV_FAMPOSE_MOVE", {0, 2, 3, 4}, false, false, 2, 1, 3, 5, 4},
    // cpu2-26: the rest of the Performance budget (the game's own LOD models, flower / bush groups, fur layers, far emitters' emission), graded by level. The budget's own value
    // (0.6, 5000, 2500, 0.5) is level 1, so level 1 changes nothing here; they act only where the budget is on, and never loosen a value set on its own.
    {"GXV_PB_LOD", {0, 0.60f, 0.45f, 0.30f}, false, true},
    {"GXV_PB_DECOR", {0, 5000, 4000, 3000}, false, true},
    {"GXV_PB_FUR", {0, 2500, 2000, 1500}, false, true},
    {"GXV_PB_PTCL", {0, 0.50f, 0.40f, 0.30f}, false, true},
    {"GXV_PIECETHIN", {0, 2, 2, 2}, false, false, 0, 4, 2},       // (owner 12 Nov 11:09: star bits may be thinned if hardly noticeable: only far ones, one in two; cpu2-26: level 3 reaches nearer, 3500)
    {"GXV_PIECETHIN_DIST", {0, 6000, 4500, 3500}, false, false, 0, 4, 6000},
};
inline constexpr int kMemberCount = static_cast<int>(sizeof kMembers / sizeof kMembers[0]);

inline int ClampLevel(long v) { return v < 0 ? 0 : (v > kMaxLevel ? kMaxLevel : static_cast<int>(v)); }
// The level in force: the stage's `cpulod`, else the environment's GXV_CPULOD.
inline int Level() { return ClampLevel(stageknob::Int("GXV_CPULOD", 0)); }
// The umbrella's value of a member at a level; false when the name is not a member or the level is 0. q30-26: in the Quality (30) mode ANY level above 0 is the "q" level: the member's q
// value, and a member with no q value (the visible cuts) answers false, as if the umbrella were off for it (the governors' steps above 1 add nothing there either).
// q30lock26: a member's value in the Quality mode at `level` (a legacy level 1..3 = the q value; a coded level = q, or qv while the member's step is in force). false: not applied.
inline bool QualityOf(const Member& m, int level, float* out) {
    if (Coded(level)) {
        const int steps = CodedSteps(level);
        if (m.qStep2 > 0 && ((steps >> (m.qStep2 - 1)) & 1)) { if (out) *out = m.qv2; return true; }   // (3e wins over 3a)
        if (m.qStep > 0 && ((steps >> (m.qStep - 1)) & 1)) { if (out) *out = m.qv; return true; }
        if (!(m.q > 0.0f)) return false;
        if (out) *out = m.q;
        return true;
    }
    if (!(m.q > 0.0f)) return false;
    if (out) *out = m.q;
    return true;
}
// far30: level 4 = the LAST RESORT (GXV_FARFIELD_LAST=1, port/farfield.h): the table has no row of its own for it; it is level 3's row without the 25 m floor.
constexpr int kLastResortLevel = 4;
inline bool LevelOk(int level) { return Coded(level) || (level > 0 && level <= kLastResortLevel); }
// far30: GXV_FARFIELD (Performance only: the Quality branch never reaches this). 0 (default): every row is exactly as before.
inline bool FarOn() { return PORT_STAGE_KNOB(int, stageknob::Int("GXV_FARFIELD", 0) != 0 ? 1 : 0) != 0; }          // (cached with the stage knobs: one load a call)
inline bool LastOn() { return PORT_STAGE_KNOB(int, stageknob::Int("GXV_FARFIELD_LAST", 0) != 0 ? 1 : 0) != 0; }
inline bool StaticOn() { return PORT_STAGE_KNOB(int, stageknob::Int("GXV_FARFIELD_STATIC", 0) != 0 ? 1 : 0) != 0; }   // the budget's own static reaches (PB_ANIM 20 m, a preset's 15 m / 22 m, the shadow clip) lifted to 25 m
// The Performance value of a member at a legacy level. With the far field on, the governors' rows 1..3 never reach inside 25 m (the distance members are floored; the shadow
// clip, the decoration distance, the LOD factor and the particle factor come from farfield::Plan), and level 4 (when allowed) is the old row 3 with no floor.
inline float PerfValue(const Member& m, int level) {
    const int row = level > kMaxLevel ? kMaxLevel : level;
    float v = m.lv[row];
    if (!FarOn()) return v;
    const bool last = level > kMaxLevel && LastOn();
    if (last) return v;   // the old reaches (9 .. 18 m): the only place they are still used
    const float depth = static_cast<float>(row);
    const farfield::Horizons h = farfield::Plan(depth, false);
    const char* n = m.name;
    if (std::strcmp(n, "GXV_PB_DECOR") == 0) return h.decor;
    if (std::strcmp(n, "GXV_PB_SHADOW") == 0) return h.shadow;
    if (std::strcmp(n, "GXV_PB_LOD") == 0) return h.lod;
    if (std::strcmp(n, "GXV_PB_PTCL") == 0) return h.ptcl;
    if (std::strcmp(n, "GXV_PB_ANIM") == 0 || std::strcmp(n, "GXV_MATLOD_DIST") == 0 || std::strcmp(n, "GXV_FAMPOSE_DIST") == 0 || std::strcmp(n, "GXV_PTCL_THIN_D0") == 0 ||
        std::strcmp(n, "GXV_PB_FUR") == 0)
        return v < farfield::kFloor ? farfield::kFloor : v;
    return v;
}
// perf32 (GXV_CPULOD_MEMO, 20 Nov 2026; default 0): TableValue without its string compares. The governors' and the Quality lock's accessors ask for a member by name
// for every actor they judge while a level is in force (the PGO profile: 76 calls a turn, 11 strcmp a call in the member search, and with the far field on up to 9 more
// in PerfValue): exactly the turns that are short of time. With the switch on
//   * the member is found through a 32-slot memo of the name's ADDRESS -> index; a hit is confirmed against the member's own name (the same pointer, else one strcmp), so
//     a slot overwritten by another name, or by another thread, can only miss; a miss runs the search below and stores what it found. The index is the search's (the
//     first member of that name), found or not found alike;
//   * PerfValue's chain of name compares is the member's class, worked out by the compiler from the same names in the same order (kFarClass).
// 0: the code as before. 1: on. 2: verify (both ways for every call, the old answer used, differences counted in g_memoBad / g_memoChecks: "cpulod-memo:" in perf.cpp).
enum : uint8_t { kFarNone = 0, kFarDecor, kFarShadow, kFarLod, kFarPtcl, kFarFloor };
constexpr bool CtEq(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
constexpr uint8_t FarClassOf(const char* n) {   // (PerfValue's chain, in its order)
    if (CtEq(n, "GXV_PB_DECOR")) return kFarDecor;
    if (CtEq(n, "GXV_PB_SHADOW")) return kFarShadow;
    if (CtEq(n, "GXV_PB_LOD")) return kFarLod;
    if (CtEq(n, "GXV_PB_PTCL")) return kFarPtcl;
    if (CtEq(n, "GXV_PB_ANIM") || CtEq(n, "GXV_MATLOD_DIST") || CtEq(n, "GXV_FAMPOSE_DIST") || CtEq(n, "GXV_PTCL_THIN_D0") || CtEq(n, "GXV_PB_FUR")) return kFarFloor;
    return kFarNone;
}
struct FarClassTable {
    uint8_t c[sizeof kMembers / sizeof kMembers[0]];
    constexpr FarClassTable() : c() {
        for (int i = 0; i < kMemberCount; ++i) c[i] = FarClassOf(kMembers[i].name);
    }
};
inline constexpr FarClassTable kFarClass{};
static_assert(kMemberCount < 255, "the memo keeps index + 1 in a byte");
inline float PerfValueClass(const Member& m, uint8_t cls, int level) {   // PerfValue with the member's class given
    const int row = level > kMaxLevel ? kMaxLevel : level;
    float v = m.lv[row];
    if (!FarOn()) return v;
    const bool last = level > kMaxLevel && LastOn();
    if (last) return v;
    const float depth = static_cast<float>(row);
    const farfield::Horizons h = farfield::Plan(depth, false);
    switch (cls) {
    case kFarDecor: return h.decor;
    case kFarShadow: return h.shadow;
    case kFarLod: return h.lod;
    case kFarPtcl: return h.ptcl;
    case kFarFloor: return v < farfield::kFloor ? farfield::kFloor : v;
    default: return v;
    }
}
inline int FindMember(const char* name) {   // the search: the first member of that name, -1 none
    for (int i = 0; i < kMemberCount; ++i)
        if (std::strcmp(kMembers[i].name, name) == 0) return i;
    return -1;
}
inline std::atomic<uint8_t> g_memoSlot[32] = {};   // index + 1 of the member last found for a name whose address falls in the slot; 0 none
inline int FindMemberMemo(const char* name) {
    const unsigned h = (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(name)) * 2654435761u) >> 27;
    const unsigned c = g_memoSlot[h].load(std::memory_order_relaxed);
    if (c != 0 && c <= static_cast<unsigned>(kMemberCount)) {
        const char* const mn = kMembers[c - 1].name;
        if (mn == name || std::strcmp(mn, name) == 0) return static_cast<int>(c) - 1;   // (names are unique: tests/perf32_cpulod_memo.cpp; so this is the search's index)
    }
    const int i = FindMember(name);
    if (i >= 0) g_memoSlot[h].store(static_cast<uint8_t>(i + 1), std::memory_order_relaxed);
    return i;
}
inline int MemoMode() {
    return PORT_STAGE_KNOB(int, [] { const char* e = std::getenv("GXV_CPULOD_MEMO"); return e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0; }());   // (with the stage knobs: read again at a stage change, like FarOn)
}
inline std::atomic<uint32_t> g_memoChecks{0}, g_memoBad{0};
inline bool TableValueOld(int level, const char* name, float* out) {
    if (!LevelOk(level)) return false;
    const bool quality = stageknob::Quality();
    for (int i = 0; i < kMemberCount; ++i)
        if (std::strcmp(kMembers[i].name, name) == 0) {
            if (quality) return QualityOf(kMembers[i], level, out);
            if (Coded(level)) return false;
            if (out) *out = PerfValue(kMembers[i], level);
            return true;
        }
    return false;
}
inline bool TableValueMemo(int level, const char* name, float* out) {
    if (!LevelOk(level)) return false;
    const bool quality = stageknob::Quality();
    const int i = FindMemberMemo(name);
    if (i < 0) return false;
    if (quality) return QualityOf(kMembers[i], level, out);
    if (Coded(level)) return false;
    if (out) *out = PerfValueClass(kMembers[i], kFarClass.c[i], level);
    return true;
}
inline bool TableValue(int level, const char* name, float* out) {
    const int mm = MemoMode();
    if (__builtin_expect(mm == 0, 1)) return TableValueOld(level, name, out);
    if (mm == 1) return TableValueMemo(level, name, out);
    float a = 0.0f, b = 0.0f;   // verify: both, the old answer used
    const bool ra = TableValueOld(level, name, &a), rb = TableValueMemo(level, name, &b);
    g_memoChecks.fetch_add(1, std::memory_order_relaxed);
    if (ra != rb || (ra && std::memcmp(&a, &b, sizeof a) != 0)) g_memoBad.fetch_add(1, std::memory_order_relaxed);
    if (ra && out) *out = a;
    return ra;
}
// q30-26: the value a member takes at `level` in the given mode (the pure form of TableValue, for the fan-outs and the tests). false: not applied.
inline bool ModeValue(bool quality, int level, const char* name, float* out) {
    if (!LevelOk(level)) return false;
    for (int i = 0; i < kMemberCount; ++i)
        if (std::strcmp(kMembers[i].name, name) == 0) {
            if (quality) return QualityOf(kMembers[i], level, out);
            if (Coded(level)) return false;
            if (out) *out = PerfValue(kMembers[i], level);
            return true;
        }
    return false;
}
// q30-26: the members entry.cpp's env fan-out wrote (by index), so that the Settings, once the picture mode is known, can rewrite them for Quality (ApplyQualityEnv). The fan-out comes before
// the mode is known (env.txt is read first), and the defaults of the members come after it.
// strk1118 (the Strikers port's g_defaulted[160] filled up on 17 Nov and later defaults were silently not undone): 32 -> 64 (23 members today, 36 %), the use is
// capped by a static_assert and a host test (tests/table_headroom.cpp fails above 75 %), and an index that does not fit is counted (g_fannedLost, shown in the
// quality30 census line) instead of ignored.
inline constexpr int kFannedCap = 64;
inline bool g_fanned[kFannedCap] = {};
inline int g_fannedLost = 0;
static_assert(kMemberCount <= kFannedCap, "g_fanned");
inline void NoteFanned(int i) { if (i >= 0 && i < kFannedCap) g_fanned[i] = true; else ++g_fannedLost; }
// A member's value: the stage's own key, else the environment's text (non-empty), else the umbrella's level, else `dflt`.
inline float Num(const char* name, float dflt) {
    float f;
    if (stageknob::Lookup(name, &f)) return f;
    const char* e = std::getenv(name);
    if (e && *e) return static_cast<float>(std::atof(e));
    if (TableValue(Level(), name, &f)) return f;
    return dflt;
}
inline int N(const char* name) {  // a skip count: 0 or 2..4
    const float v = Num(name, 0.0f);
    return v >= 2.0f ? (v > 4.0f ? 4 : static_cast<int>(v)) : 0;
}

// The preset line `cpulod=N`: the members the line did not name, with their values. `has(name)` says whether the line sets that switch itself.
template <class Has, class Emit>
inline void Expand(int level, Has has, Emit emit, bool quality = false) {
    if (level <= 0 || level > kMaxLevel) return;
    for (int i = 0; i < kMemberCount; ++i) {
        if (kMembers[i].perfOnly || has(kMembers[i].name)) continue;
        if (quality) { if (kMembers[i].q > 0.0f) emit(kMembers[i].name, kMembers[i].q); }   // (q30-26: Quality takes the q members only)
        else emit(kMembers[i].name, kMembers[i].lv[level]);
    }
}
// q30-26: the picture mode turned out to be Quality (30) after the env fan-out had written the Performance rows: rewrite every fanned member to its q value, or 0 (off) when it has none.
// `set(name, text)` writes the environment (overwrite). Returns how many members were rewritten.
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


// ---- cpu2-26: the Performance budget's own members (GXV_PB_LOD / _DECOR / _FUR / _PTCL), graded by level ----------------------------------------------------
// kind 0: a factor, 0 < f < 1 is on and smaller is stricter (PB_LOD, PB_PTCL); kind 1: a distance, 0 is off and smaller is stricter (PB_DECOR, PB_FUR).
// The stricter of the budget's own value `cur` and the table's row `tbl`; an "off" cur takes the row.
inline float PbStricter(int kind, float cur, float tbl) {
    const bool curOn = kind == 0 ? (cur > 0.0f && cur < 1.0f) : (cur > 0.0f);
    if (!curOn) return tbl;
    return tbl < cur ? tbl : cur;
}

// ---- the decision rules (pure) ----------------------------------------------------------------------------------------------------------
// Far enough from BOTH the camera and the player: squared distances against (D * zoom + radius)^2. zoom is the handheld zoom ratio (1 when none).
inline bool FarFromBoth(float camD2, float plD2, float dist, float zoom, float radius) {
    const float lim = dist * zoom + radius;
    return camD2 > lim * lim && plD2 > lim * lim;
}
// Skip rule shared by MATLOD and FAMPOSE: the count of turns left out since the last one made (0xFF fresh); at most n-1 in a row, phase by seed.
inline bool Skip(uint8_t& skips, int n, uint32_t seed) { return cliprange::FarAnimSkip(skips, n, seed); }
// ratecut31 (GXV_POSE_PHASE, port/pose_phase.h): the same rule made on the turn a picture shows: phaseMode 1 / 2 leave a pose out only on a skip turn (a drawn
// turn after an undrawn one), mode 1 also capping n at 2; phaseMode 0 is Skip itself (tests/pose_phase.cpp proves the identity).
inline bool SkipAt(uint8_t& skips, int n, uint32_t seed, int phaseMode, bool skipTurn) { return posephase::Cadence(skips, n, seed, phaseMode, skipTurn); }
// FAMPOSE's movement guard: an actor that moved more than `maxLines` lines on the picture since its last pose is posed now (and the count restarts).
// moved2: squared distance between the actor's position and the model's last base translation; dist: camera distance; tanHalf: tan(fov/2).
inline bool MovedTooFar(float moved2, float dist, float tanHalf, float maxLines) {
    const float unitsPerLine = (dist > 1.0f && tanHalf > 0.0f) ? 2.0f * dist * tanHalf / 456.0f : 0.0f;
    if (!(unitsPerLine > 0.0f)) return true;
    const float lim = maxLines * unitsPerLine;
    return moved2 > lim * lim;
}
// PIECETHIN: of the pieces beyond the distance, which are kept (drawn): a well-mixed per-actor hash, one in n. Stable for an actor.
inline uint32_t Mix(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
inline bool PieceKept(uint32_t seed, int n) { return n < 2 || (Mix(seed) % static_cast<uint32_t>(n)) == 0; }
// The hysteresis of the distance test: entering the far state needs 6 % more than staying in it.
inline float HystDist(float dist, bool was, float hyst) { return was ? dist : dist * (1.0f + hyst); }

}  // namespace cpulod
}  // namespace port
