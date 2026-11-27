// Stage-settable switches (stagepreset25): the registry of GXV_* / PETARI_* switches that a per-stage preset may change when a stage loads.
//
// Most switches are read once into a function-local static (EnvInt at first use, PORT_HOT_KNOB, VGX_KNOB) and never again. A switch is
// STAGE-SETTABLE when its read goes through this header:
//   - PORT_STAGE_KNOB(T, expr) is PORT_HOT_KNOB (port/hot_knob.h) whose word is also entered in a registry, so Commit() can clear every
//     such word and the next read works `expr` out again; `expr` reads its switch with port::stageknob::Int / Num, which answer the
//     stage's override first and the environment (the global default: entry.cpp's setenv defaults and env.txt) when there is none.
//   - a switch cached some other way (a static with a -1 "not read yet" mark, in the game's code) compares Gen() with the generation it
//     read at and reads again when it changed.
// With no override set, Int / Num return exactly what EnvInt / EnvI / atof of getenv returned before (same cases, same parsing), and a knob
// is worked out exactly once as before, so a build that never calls Set / Commit behaves as it did (GXV_STAGE_PRESETS=0).
//
// Order of an application (the back end, at a stage's load, screen closed): ClearAll(); Set() for every key; Commit(). A reader that
// worked a value out from the old table while Commit ran does not keep it (Init checks the generation after its store).
// Values are floats (exact for whole numbers up to 16 M): every settable switch is a small integer or a distance.
// The descriptor table kDesc is the list of what a preset may name: unknown or non-settable keys are refused with a log line.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdio.h>  // (unqualified snprintf: game units map it to port_snprintf)
#include <cstdlib>
#include <cstring>
#include "hot_knob.h"

namespace port {
namespace stageknob {

constexpr int kSlots = 96;
constexpr int kMaxKnobs = 512;

inline uint32_t Hash(const char* s) {
    uint32_t h = 2166136261u;  // FNV-1a
    for (; s && *s; ++s) h = (h ^ static_cast<unsigned char>(*s)) * 16777619u;
    return h ? h : 1u;
}
inline uint32_t FloatBits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
inline float BitsFloat(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

struct Table {
    std::atomic<uint32_t> hash[kSlots];  // 0 = free
    std::atomic<uint32_t> bits[kSlots];
    std::atomic<uint32_t> gen;
    std::atomic<uint32_t> count;
    std::atomic<uint32_t> knobN;  // registered knob words
    std::atomic<uint32_t>* knobA[kMaxKnobs];
    std::atomic<uint32_t>* knobB[kMaxKnobs];
    std::atomic<uint32_t> overflow;
};
inline Table g_t{};

inline uint32_t Gen() { return g_t.gen.load(std::memory_order_relaxed); }

// q30-26: the picture mode in force, for the umbrellas' tables and the governors (1 = Quality (30), 0 = Performance (60) or anything else). Set at start-up by the Settings (ApplyStartup), by
// the env file's GXV_PICTURE_MODE=30 (entry.cpp) and by the back end at every stage load (Resolve's pictureMode). One relaxed load.
inline std::atomic<int> g_quality{0};
inline bool Quality() { return g_quality.load(std::memory_order_relaxed) != 0; }
inline void SetQuality(bool on) { g_quality.store(on ? 1 : 0, std::memory_order_relaxed); }
inline uint32_t Count() { return g_t.count.load(std::memory_order_relaxed); }

inline bool Lookup(const char* name, float* out) {
    if (g_t.count.load(std::memory_order_relaxed) == 0) return false;
    const uint32_t h = Hash(name);
    for (int i = 0; i < kSlots; ++i)
        if (g_t.hash[i].load(std::memory_order_acquire) == h) {
            if (out) *out = BitsFloat(g_t.bits[i].load(std::memory_order_relaxed));
            return true;
        }
    return false;
}
// Sets (or replaces) the stage's value of `name`. False when the table is full. Takes effect at Commit().
inline bool Set(const char* name, float v) {
    const uint32_t h = Hash(name);
    int freeAt = -1;
    for (int i = 0; i < kSlots; ++i) {
        const uint32_t cur = g_t.hash[i].load(std::memory_order_relaxed);
        if (cur == h) { g_t.bits[i].store(FloatBits(v), std::memory_order_relaxed); return true; }
        if (cur == 0 && freeAt < 0) freeAt = i;
    }
    if (freeAt < 0) return false;
    g_t.bits[freeAt].store(FloatBits(v), std::memory_order_relaxed);
    g_t.hash[freeAt].store(h, std::memory_order_release);
    g_t.count.fetch_add(1, std::memory_order_relaxed);
    return true;
}
// Every stage override off: the switches read what the environment says again (after Commit).
inline void ClearAll() {
    for (int i = 0; i < kSlots; ++i) g_t.hash[i].store(0, std::memory_order_release);
    g_t.count.store(0, std::memory_order_relaxed);
}
// The environment's value (the global default) as EnvInt reads it: empty counts as not set.
inline long EnvLong(const char* name, long dflt) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::strtol(v, nullptr, 0) : dflt;
}
inline long Int(const char* name, long dflt) {
    float f;
    if (Lookup(name, &f)) return static_cast<long>(f);
    return EnvLong(name, dflt);
}
// atof of the environment's text (the way EnvF in port/detail.h and PbValue in perf.cpp read), the stage's value first.
inline float Num(const char* name, float dflt) {
    float f;
    if (Lookup(name, &f)) return f;
    const char* e = std::getenv(name);
    return (e && *e) ? static_cast<float>(std::atof(e)) : dflt;
}
// Whether the stage sets `name`, and to what (for a site that must keep its own parsing of the environment's text, e.g. "perf" / "exact").
inline bool Overridden(const char* name, long* v) {
    float f;
    if (!Lookup(name, &f)) return false;
    if (v) *v = static_cast<long>(f);
    return true;
}

namespace detail {
inline void Register(std::atomic<uint32_t>& a, std::atomic<uint32_t>& b, std::atomic<bool>& reg) {
    if (reg.exchange(true, std::memory_order_relaxed)) return;
    const uint32_t i = g_t.knobN.fetch_add(1, std::memory_order_relaxed);
    if (i >= static_cast<uint32_t>(kMaxKnobs)) { g_t.overflow.fetch_add(1, std::memory_order_relaxed); return; }
    g_t.knobA[i] = &a;
    g_t.knobB[i] = &b;
}
template <class T, class F>
__attribute__((noinline)) T Init(std::atomic<uint32_t>& k, std::atomic<uint32_t>& kf, std::atomic<bool>& reg, F f) {
    const uint32_t g0 = Gen();
    const T v = f();
    const hotknob::Bits<T> b = hotknob::ToBits<T>(v);
    if (b != hotknob::kUnset<T>) {
        Register(k, kf, reg);
        (hotknob::Fenced() ? kf : k).store(b, std::memory_order_relaxed);
        if (Gen() != g0) { k.store(hotknob::kUnset<T>, std::memory_order_relaxed); kf.store(hotknob::kUnset<T>, std::memory_order_relaxed); }
    }
    return v;
}
template <class T, class F>
inline __attribute__((always_inline)) T Read(std::atomic<uint32_t>& k, std::atomic<uint32_t>& kf, std::atomic<bool>& reg, F f) {
    const uint32_t v = k.load(std::memory_order_relaxed);
    if (__builtin_expect(v != hotknob::kUnset<T>, 1)) return hotknob::FromBits<T>(v);
    const uint32_t w = kf.load(std::memory_order_relaxed);
    if (w != hotknob::kUnset<T>) {
        std::atomic_thread_fence(std::memory_order_acquire);
        return hotknob::FromBits<T>(w);
    }
    return Init<T>(k, kf, reg, f);
}
}  // namespace detail

// ---- ab27: LIVE INTEGER KNOBS ------------------------------------------------------------------------------------------------------------------
// port_knob_int("GXV_X", dflt) (the global wrapper of LiveInt, at the end of this file) returns a reference to a live int that follows the registry: Set() + Commit() (a stage preset's apply, the A/B driver's flip, a governor)
// refresh it, so a site that keeps the reference reads the current value with one relaxed load, no re-evaluation, and a switch that used to be read once (a static const long
// x = EnvInt(...)) becomes flippable by replacing that line. A knob registers whether it is FLIP-SAFE (a pure per-picture / per-use decision: the value may change between two
// pictures with no state to rebuild) and an optional on-change callback (called on the thread that commits, after the value changed: invalidate a cache, reconfigure a helper).
constexpr int kMaxLive = 96;
struct LiveSlot {
    char name[48];
    std::atomic<int32_t> v{0};
    long dflt = 0;
    bool flipSafe = true;
    void (*onChange)(const char* name, long oldV, long newV) = nullptr;
};
inline LiveSlot g_live[kMaxLive];
inline std::atomic<uint32_t> g_liveN{0};
inline std::atomic<int> g_liveLock{0};
inline LiveSlot* LiveFind(const char* name) {
    const uint32_t n = g_liveN.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n && i < static_cast<uint32_t>(kMaxLive); ++i)
        if (std::strcmp(g_live[i].name, name) == 0) return &g_live[i];
    return nullptr;
}
inline LiveSlot* LiveGet(const char* name, long dflt, bool flipSafe) {
    if (LiveSlot* s = LiveFind(name)) return s;
    while (g_liveLock.exchange(1, std::memory_order_acquire)) {}
    LiveSlot* s = LiveFind(name);
    if (!s) {
        const uint32_t n = g_liveN.load(std::memory_order_relaxed);
        if (n < static_cast<uint32_t>(kMaxLive)) {
            s = &g_live[n];
            snprintf(s->name, sizeof s->name, "%s", name);
            s->dflt = dflt;
            s->flipSafe = flipSafe;
            s->v.store(static_cast<int32_t>(Int(name, dflt)), std::memory_order_relaxed);
            g_liveN.store(n + 1, std::memory_order_release);
        }
    }
    g_liveLock.store(0, std::memory_order_release);
    return s;
}
// A knob past the table (kMaxLive) answers from a spare that is never refreshed.
inline std::atomic<int32_t>& LiveInt(const char* name, long dflt, bool flipSafe = true) {
    static std::atomic<int32_t> spare{0};
    LiveSlot* s = LiveGet(name, dflt, flipSafe);
    if (!s) { spare.store(static_cast<int32_t>(Int(name, dflt)), std::memory_order_relaxed); return spare; }
    return s->v;
}
// The callback of a live knob (registers the knob when it is not there yet). Replaces an earlier one.
inline bool port_knob_on_change(const char* name, long dflt, void (*cb)(const char*, long, long)) {
    LiveSlot* s = LiveGet(name, dflt, true);
    if (!s) return false;
    s->onChange = cb;
    return true;
}
inline char FlipClassOfName(const char* name);
// Whether `name` may change in play: a live knob's own registration, else the descriptor's class (FlipClass: 'y' / 'w' yes, 'n' no); a name that is neither is not flippable.
inline bool FlipSafeName(const char* name) {
    if (LiveSlot* s = LiveFind(name)) return s->flipSafe;
    return FlipClassOfName(name) != 'n';
}
// The value a live knob has now, or `dflt` when it is not registered.
inline long LiveValue(const char* name, long dflt) {
    const LiveSlot* s = LiveFind(name);
    return s ? static_cast<long>(s->v.load(std::memory_order_relaxed)) : dflt;
}
// Called by Commit(): every live knob works its value out again from the table / environment; a changed one calls its callback. Returns how many changed.
inline uint32_t LiveRefresh() {
    uint32_t changed = 0;
    const uint32_t n = g_liveN.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n && i < static_cast<uint32_t>(kMaxLive); ++i) {
        LiveSlot& s = g_live[i];
        const int32_t nv = static_cast<int32_t>(Int(s.name, s.dflt)), ov = s.v.load(std::memory_order_relaxed);
        if (nv == ov) continue;
        s.v.store(nv, std::memory_order_relaxed);
        ++changed;
        if (s.onChange) s.onChange(s.name, ov, nv);
    }
    return changed;
}

// The generation changes at every Commit: a switch cached by hand reads again when it differs from the one it read at.
// Commit() (after ClearAll / Set): bump the generation and clear every registered knob word.
inline void Commit() {
    g_t.gen.fetch_add(1, std::memory_order_release);
    const uint32_t n = g_t.knobN.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n && i < static_cast<uint32_t>(kMaxKnobs); ++i) {
        g_t.knobA[i]->store(hotknob::kUnset<uint32_t>, std::memory_order_relaxed);
        g_t.knobB[i]->store(hotknob::kUnset<uint32_t>, std::memory_order_relaxed);
    }
    LiveRefresh();   // (ab27: the live integer knobs follow the new table)
}
inline uint32_t KnobsRegistered() { return g_t.knobN.load(std::memory_order_relaxed); }
inline uint32_t KnobOverflow() { return g_t.overflow.load(std::memory_order_relaxed); }

// ---- what a preset may name ---------------------------------------------------------------------------------------------------------
// cls: E exact, U unnoticeable, N noticeable (switchdoc25's classes). how: 'a' re-read at once at the next use, 'c' re-read at the next use of a
// cache that is made at a picture boundary (two threads settle within two pictures; applied behind the closed screen), 'g' a game-thread cache
// that reads again on a generation change. Every row's switch reads through Int / Num / PORT_STAGE_KNOB at its use site (tests/vgx_stagepreset_test.cpp
// checks that every row round-trips; the sites are listed in agents/stagepreset25/REPORT.txt).
struct Desc {
    const char* alias;  // the short key of a preset line
    const char* name;   // the switch
    float lo, hi;       // accepted range (values outside are refused, not clamped)
    char cls;
    char how;
    // The owner's rule (21:52): glow, bloom, heat haze and refraction may be reduced but never eliminated. A key that CAN eliminate one has offVal = the
    // value that does it and weakest = the weakest reduced form it is clamped to (ValidateKv, unless GXV_STAGE_PRESETS_ALLOW_OFF=1, a test override).
    // No key in the table below can eliminate one today (bloom_half / bloom_fast / bloom_fold / indcap_half / clip_half / clip_onepass / shadowtex_half only ever
    // reduce); the fields are for the keys the shader-simplification and haze work will add.
    float offVal = -1e9f, weakest = 0;
    // modes26 (12 Nov): the two picture modes (Performance 60 / Quality 30). ex says whether a preset line's value of this key is EXACT (bit-identical picture and game: nothing is
    // given up, so it applies in both modes) or a SACRIFICE (a cut the 60 mode may make; the 30 mode skips it): 'x' exact at every accepted value, 'p' exact for values up to exactMax
    // (lightvol 3: the cull and the pass-major groups; 4 and 8 are the LOD and the census), 's' a sacrifice (the default: a key nobody has proven exact is a cut). Decided from
    // new/policy26/LEDGER.tsv (the "player loses" column: "nothing (exact)" / "nothing (bit-identical)") and the class letter above ('E' with no ledger entry against it).
    char ex = 's';
    float exactMax = 0;
    // q30-26 (12 Nov): the third class, 'b' = BARELY NOTICEABLE (a player would not see it in normal play): the Quality (30) mode KEEPS it, so that it has headroom to hold a locked 30 everywhere
    // while looking its best. A 'b' key with levels may be kept only up to bMax (and from bMin up, for a key where a LOWER value is the stronger cut: a distance, a starting count): Quality clamps a
    // preset's value into [bMin, bMax] instead of dropping the key (the log line says "clamped for Quality"). Defaults: no clamp. The umbrellas (cpulod, across, glow26, fe_gov, game_gov) are 'b'
    // with bMax 1: in Quality ANY level means their "q" level, the table column of their barely-noticeable members (cpulod.h, across.h, glow26.h kQ).
    float bMin = -1e9f, bMax = 1e9f;
};
inline constexpr Desc kDesc[] = {
    // q30-26: ex 'b' rows (barely noticeable, kept by Quality; REPORT.txt of q30-26 has the reason of every row): the trailing bMin, bMax clamp a level. Every row without a letter is 's'.
    // ---- captures (the EFB splits scenes25 counted)
    {"indcap_skip", "GXV_INDCAP_SKIP", 0, 2, 'E', 'a', -1e9f, 0, 'x'},
    {"scenes_dry", "GXV_SCENES25_DRY", 0, 7, 'E', 'a', -1e9f, 0, 'x'},
    {"indcap_half", "GXV_INDCAP_HALFRATE", 0, 2, 'U', 'a'},
    {"indcap_vis", "GXV_INDCAP_VIS", 0, 2, 'E', 'a', -1e9f, 0, 'x'},   // indvis38: the Indirect capture left out when every reader is invisible (1; 2 counts only)
    {"indcap_refresh", "GXV_INDCAP_REFRESH", 0, 100000, 'U', 'a', -1e9f, 0, 'b', 0, -1e9f, 1e9f},
    {"clearq", "GXV_CLEARQ", 0, 3, 'E', 'a', -1e9f, 0, 'x'},
    {"silh_gate", "GXV_SILH_GATE", 0, 2, 'N', 'g', -1e9f, 0, 'b', 0, -1e9f, 2.0f},
    // ---- across26 (platform/include/port/across.h, vgx_silhocc.h): the silhouette gate confirmed by an occlusion query (needs silh_gate=1), and the umbrella level of the all-stage cuts (a preset line `across=N` is expanded into the members it does not name, ValidateKv)
    {"silh_occ", "GXV_SILH_OCC", 0, 3, 'N', 'g', -1e9f, 0, 'b', 0, -1e9f, 1.0f},
    {"across", "GXV_ACROSS", 0, 2, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 1.0f},
    {"tail_depth", "GXV_TAILDEPTH", 0, 2, 'U', 'a'},  // scenes26 (vgx_scenes26.h): the last EFB scene of a picture stores no depth when the pictures before began with a whole-depth overwrite (needs GXV_FIRSTCLEAR=1)
    {"shadowfill_shrink", "GXV_SHADOWFILL_SHRINK", 0, 2, 'N', 'g', -1e9f, 0, 'b', 0, -1e9f, 1e9f},
    {"silh_fuse", "GXV_SILH_FUSE", 0, 2, 'U', 'a'},  // silhfuse26 (vgx_silhfuse.h): the silhouette fill fused with the hand-over's draw-back quad (one full-screen pass less; exact against a round-to-nearest blender, measured by =2)
    {"colshadow_clip", "GXV_COLSHADOW_CLIP", 0, 2, 'N', 'g', -1e9f, 0, 'p', 1},
    {"ind_flat", "GXV_IND_FLAT", 0, 2, 'N', 'a'},
    // ---- reflfur25: Mario's reflection on glass / ice (port/mirror25.h): 1 skipped when the mirrored model is outside the frustum (exact), 2 also one pass instead of two; the fur layers (port/fur25.h)
    {"mirror", "GXV_MIRROR25", 0, 2, 'N', 'a', -1e9f, 0, 'p', 1},
    {"mirror_census", "GXV_MIRROR25_CENSUS", 0, 1, 'E', 'a', -1e9f, 0, 'x'},
    {"fur25", "GXV_FUR25", 0, 2, 'N', 'a'},
    // ---- reduced-size effect path (GXV_FXLOWRES=1, lowres25) and its stage-settable choice of draws (freeze25; vgx_fxkeys.h)
    {"fxlow", "GXV_FXLOWRES", 0, 3, 'N', 'a'},
    {"fxlow_static", "GXV_FXLOWRES_STATIC", 0, 1, 'N', 'a'},
    {"fxlow_min", "GXV_FXLOWRES_MIN", 0, 1000000, 'N', 'a'},
    {"fxlow_runs", "GXV_FXLOWRES_RUNS", 1, 64, 'N', 'a'},
    {"fxlow_k1", "GXV_FXLOWRES_K1", 0, 16777215, 'N', 'a'},
    {"fxlow_k2", "GXV_FXLOWRES_K2", 0, 16777215, 'N', 'a'},
    {"fxlow_k3", "GXV_FXLOWRES_K3", 0, 16777215, 'N', 'a'},
    {"fxlow_k4", "GXV_FXLOWRES_K4", 0, 16777215, 'N', 'a'},
    // ---- compositor25: the automatic choice of keys by fill (vgx_fxauto.h) and the quality switches of the compositor
    {"glow26", "GXV_GLOW26", 0, 2, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 1.0f},                  // glow26 (port/glow26.h): the cheap look-alike for soft effects, a level (0 off, 1 mild, 2 strong) that raises the compositor, ind_flat, bloom_fold and lightvol
    {"fxlow_auto", "GXV_FXLOWRES_AUTO", 0, 2, 'N', 'a'},
    {"fxlow_milli", "GXV_FXLOWRES_AUTO_MILLI", 50, 100000, 'N', 'a'},
    {"fxlow_win", "GXV_FXLOWRES_AUTO_WIN", 2, 1000, 'N', 'a'},
    {"fxlow_ups", "GXV_FXLOWRES_UPS", 0, 1, 'N', 'a'},
    {"fxlow_div", "GXV_FXLOWRES_DIV", 0, 4, 'N', 'a'},
    {"fxlow_thin", "GXV_FXLOWRES_THIN", 0, 200, 'N', 'a'},
    {"fxlow_alpha", "GXV_FXLOWRES_ALPHA", 0, 1, 'N', 'a'},
    {"fxlow_defer", "GXV_FXLOWRES_DEFER", 0, 2, 'N', 'a'},       // compositor25 round 2: the routed additive draws of a stretch in one run at the depth-final point (2: across captures)
    {"fxlow_defer_tol", "GXV_FXLOWRES_DEFER_TOL", 0, 1000, 'N', 'a'},
    {"fxlow_tri", "GXV_FXLOWRES_TRI", 0, 1, 'N', 'a'},
    {"indcap_camgate", "GXV_INDCAP_CAMGATE", 0, 1, 'U', 'a', -1e9f, 0, 'b', 0, -1e9f, 1e9f},
    {"clip_skipempty", "PETARI_CLIPCHAIN_SKIPEMPTY", 0, 1, 'E', 'g', -1e9f, 0, 'x'},
    {"clip_half", "PETARI_CLIPCHAIN_HALFRATE", 0, 2, 'U', 'g'},
    {"clip_onepass", "PETARI_CLIPCHAIN_ONEPASS", 0, 2, 'N', 'g'},
    {"bloom_half", "PETARI_BLOOM_HALFRATE", 0, 2, 'N', 'g'},
    {"bloom_fast", "GXV_BLOOM_FAST", 0, 2, 'N', 'g'},
    {"bloom_fold", "GXV_BLOOM_FOLD", 0, 3, 'N', 'g'},
    {"shadowtex_half", "PETARI_SHADOWTEX_HALFRATE", 0, 2, 'N', 'g'},
    {"hud_native", "GXV_HUD_NATIVE", 0, 2, 'N', 'c', -1e9f, 0, 'b', 0, -1e9f, 1e9f},
    // ---- draw order (hsrorder25's planner; dread25 made the pair stage-settable: read again at the next picture after a Commit)
    {"hsr_order", "GXV_HSR_ORDER", 0, 3, 'U', 'a', -1e9f, 0, 'b', 0, -1e9f, 2.0f},
    {"hsr_auto", "GXV_HSR_AUTO", 0, 2, 'U', 'a', -1e9f, 0, 'b', 0, -1e9f, 2.0f},         // geom26 (vgx_hsrauto.h): the planner decided per stage from what it draws (ties 7); 2 = probe and report only. Only while hsr_order is 0
    {"hsr_ties", "GXV_HSR_ORDER_TIES", 0, 7, 'E', 'a', -1e9f, 0, 'b', 0, 4.0f, 7.0f},   // (q30-26: 'b' in [4, 7]: 4 is the exact rule, 7 the decal-aware cut-out reorder (gpu26); 0..3 can flicker at exact depth ties and are raised to 4 in Quality)
    // ---- static runs that project to a few pixels (dread25, vgx_tinyrun.h; the limit in pixels squared of the GX viewport, 0 = never)
    {"tiny_run", "GXV_TINYRUN", 0, 2, 'N', 'a'},
    {"tiny_px", "GXV_TINYRUN_PX", 0, 64, 'N', 'a'},
    // ---- alpha-tested draws as a depth-only prepass + a colour pass without `discard` (dread25, vgx_prepass.h): 1 the pair at the draw's place (colour pass without the test), 2 the prepasses hoisted to the start of
    // their segment of plain draws, 3 census only, 4 / 5 as 1 / 2 with the test kept in the colour pass (exact; 1 and 2 differ where a killed fragment ties in depth with another surface)
    {"alphatest_prepass", "GXV_ALPHATEST_PREPASS", 0, 5, 'N', 'a', -1e9f, 0, 'b', 0, 3.0f, 5.0f},
    // ---- detail governor (port/detail.h)
    {"detail_level", "GXV_DETAIL_LEVEL", -1, 5, 'N', 'a'},
    {"detail_gov", "GXV_DETAIL_GOV", 0, 1, 'N', 'a'},
    {"detail_start", "GXV_DETAIL_START", 0, 5, 'N', 'a'},
    {"detail_plant", "GXV_DETAIL_PLANT", 0, 2, 'N', 'a'},
    {"detail_fur", "GXV_DETAIL_FUR", 0, 2, 'N', 'a'},
    // ---- performance budget distances (port/perf.cpp); 0 = that part off. GXV_PB_LOD and _PTCL are factors (0.6)
    {"pb", "GXV_PERF_BUDGET", 0, 1, 'N', 'a'},
    {"pb_lod", "GXV_PB_LOD", 0, 1, 'N', 'a'},
    {"pb_decor", "GXV_PB_DECOR", 0, 20000, 'N', 'a'},
    {"pb_ptcl", "GXV_PB_PTCL", 0, 1, 'N', 'a'},
    {"pb_shadow", "GXV_PB_SHADOW", 0, 20000, 'N', 'a'},
    {"pb_anim", "GXV_PB_ANIM", 0, 20000, 'N', 'a'},
    {"pb_fur", "GXV_PB_FUR", 0, 20000, 'N', 'a'},
    {"pb_ptcl_dist", "GXV_PB_PTCL_DIST", 800, 20000, 'N', 'a'},
    // ---- gamecpu25: particles drawn thinned by distance (port/ptcl_thin.h): fraction kept at D1 (0 = off), D0 / D1 in world units
    {"ptcl_thin", "GXV_PTCL_THIN", 0, 1, 'N', 'a'},
    {"ptcl_thin_d0", "GXV_PTCL_THIN_D0", 300, 40000, 'N', 'a'},
    {"ptcl_thin_d1", "GXV_PTCL_THIN_D1", 400, 40000, 'N', 'a'},
    {"pb_anim_pdist", "GXV_PB_ANIM_PDIST", 0, 20000, 'N', 'a'},
    // ---- far actors (decomp ClippingActorHolder.cpp)
    {"far_clip", "GXV_CLIPSCALE", 0, 99, 'N', 'a'},
    {"far_draw", "GXV_FARDRAW", 0, 64, 'N', 'a'},
    {"far_anim", "GXV_FARANIM", 0, 4, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 2.0f},
    // ---- far30 far field (cpulod.h / farfield.h): the Performance governors' far horizons; start = the level a stage begins at (3 = the nearest allowed outside the last resort)
    {"ff", "GXV_FARFIELD", 0, 1, 'N', 'a'},
    {"ff_start", "GXV_FARFIELD_START", 0, 4, 'N', 'a'},
    {"ff_px", "GXV_FARFIELD_PX", 4, 40, 'N', 'a'},
    {"ff_last", "GXV_FARFIELD_LAST", 0, 1, 'N', 'a'},
    {"ff_static", "GXV_FARFIELD_STATIC", 0, 1, 'N', 'a'},
    // ---- rate30 (port/rate_auto.h): a stage row may pin the stage to 60 (1) or to an even 30 (2); 0 = the controller decides. Read at the stage's begin.
    {"rate_pin", "GXV_RATE_PIN", 0, 2, 'N', 'a'},
    // ---- pace33 PATCH6 (port/hrw_stage.h): the per-stage choice under GXV_HRW=5. reproj: 0 as the mode says, 1 a locked even 30 (no pairs), 2 the re-projected 60, 3 as 1 with the row's Quality picture keys. warp_depth: the warp's
    // depth mode for the stage (0 rotation only, 2 focus plane, 3 hybrid, 4 rotation + player box; 1 depth-aware). Both take effect only behind a covered screen (a stage load).
    {"reproj", "GXV_HRW_STAGE", 0, 3, 'N', 'a'},
    {"warp_depth", "GXV_HRW_WARP_DEPTH", 0, 4, 'N', 'a'},
    // ---- starbatch26: floating star bits farther than this many metres (100 units) from the camera and the player are posed every second picture / turn (ActorTurnLod.cpp), 0 = off
    {"star_far", "GXV_STARBATCH_FAR", 0, 10000, 'N', 'a'},
    {"tiny_cull", "GXV_TINYCULL", 0, 16, 'N', 'a'},
    // ---- cpucuts25 (platform/include/port/cpucuts.h): far_draw's classes / hold / flush skip, the character pose rule. Their sites read through PORT_STAGE_KNOB.
    {"far_draw_kinds", "GXV_FARDRAW_KINDS", 0, 31, 'N', 'a'},
    {"far_draw_hold", "GXV_FARDRAW_HOLD", 0, 240, 'U', 'a'},
    {"far_draw_flush", "GXV_FARDRAW_FLUSH", 0, 1, 'N', 'a'},
    {"far_draw_ramp", "GXV_FARDRAW_RAMP", 0, 300, 'N', 'a'},
    // ---- honey25 (platform/include/port/cpugov.h): the far-draw cut's governor and its thresholds; read again every 5-second window (a stage preset's value acts at once)
    {"cpu_gov", "GXV_CPUGOV", 0, 5, 'N', 'a'},
    {"cpu_gov_late", "GXV_CPUGOV_LATE", 1, 100, 'N', 'a'},
    {"cpu_gov_cpu", "GXV_CPUGOV_CPU", 0, 100, 'N', 'a'},
    {"cpu_gov_calm", "GXV_CPUGOV_CALM", 0, 50, 'N', 'a'},
    {"cpu_gov_up", "GXV_CPUGOV_UP", 1, 12, 'N', 'a'},
    {"cpu_gov_down", "GXV_CPUGOV_DOWN", 1, 120, 'N', 'a'},
    // ---- fe26 (platform/include/port/fegov.h): the front end's governor (raises / lowers the cpulod level by the front end's ms a picture); read again every 5-second window
    {"fe_gov", "GXV_FE_GOV", 0, 3, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 1.0f},
    {"fe_gov_ms", "GXV_FE_GOV_MS", 6, 40, 'N', 'a'},
    {"fe_gov_band", "GXV_FE_GOV_BAND", 0, 10, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 1e9f},
    {"fe_gov_up", "GXV_FE_GOV_UP", 1, 12, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 1e9f},
    {"fe_gov_down", "GXV_FE_GOV_DOWN", 1, 120, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 1e9f},
    // ---- cpu2-26 (platform/include/port/fegov.h, perf.cpp GameGov*): the game thread's governor, the same rule on the drawn turn's CPU ms; read again every 5-second window
    {"game_gov", "GXV_GAME_GOV", 0, 3, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 1.0f},
    {"game_gov_ms", "GXV_GAME_GOV_MS", 6, 40, 'N', 'a'},
    {"game_gov_band", "GXV_GAME_GOV_BAND", 0, 10, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 1e9f},
    {"game_gov_up", "GXV_GAME_GOV_UP", 1, 12, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 1e9f},
    {"game_gov_down", "GXV_GAME_GOV_DOWN", 1, 120, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 1e9f},
    {"char_pose", "GXV_CHARPOSE", 0, 4, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 2.0f},
    {"char_pose_lines", "GXV_CHARPOSE_LINES", 1, 200, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 12.0f},
    {"char_pose_move", "GXV_CHARPOSE_MOVE", 0.1f, 20, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 1.0f},
    {"char_pose_dist", "GXV_CHARPOSE_DIST", 2000, 100000, 'N', 'a', -1e9f, 0, 'b', 0, 2500.0f, 1e9f},
    {"cr_near", "GXV_CR_NEAR", 500, 100000, 'N', 'a', -1e9f, 0, 'b', 0, 2500.0f, 1e9f},
    // ---- far actors' logic rate (actorlod23, decomp ActorTurnLod.cpp): mode 1 = every second turn, 2 = the census; read per actor turn through hot knobs
    {"alod", "GXV_ALOD", 0, 2, 'N', 'a'},
    {"alod_pdist", "GXV_ALOD_PDIST", 1000, 100000, 'N', 'a'},
    {"alod_rate", "GXV_ALOD_RATE", 1, 4, 'N', 'a'},
    {"alod_far4", "GXV_ALOD_FAR4", 0, 100000, 'N', 'a'},
    {"alod_tiny", "GXV_ALOD_TINY", 0, 64, 'N', 'a'},
    {"alod_tinydist", "GXV_ALOD_TINYDIST", 1500, 100000, 'N', 'a'},
    {"alod_families", "GXV_ALOD_FAMILIES", 0, 127, 'N', 'a'},
    // ---- lightvol25 (decomp VolumeModelDrawer.cpp, port/lightvol.h): the light volumes' cull (1, exact), pass-major groups (2, exact), LOD (4), census (8); read per picture
    {"lightvol", "GXV_LIGHTVOL", 0, 15, 'N', 'a', -1e9f, 0, 'p', 3},
    {"lightvol_keep", "GXV_LIGHTVOL_KEEP", 0, 64, 'N', 'a'},
    {"lightvol_px", "GXV_LIGHTVOL_PX", 0, 100000, 'N', 'a'},
    {"lightvol_near", "GXV_LIGHTVOL_NEAR", 0, 100000, 'N', 'a'},
    {"lightvol_clear", "GXV_LIGHTVOL_CLEAR", 0, 1, 'N', 'a'},
    // ---- cpulod26 (platform/include/port/cpulod.h): the umbrella level (a preset line `cpulod=N` is expanded into the members it does not name, ValidateKv) and the three new cuts
    {"cpulod", "GXV_CPULOD", 0, 3, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 1.0f},
    {"matlod", "GXV_MATLOD", 0, 4, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 2.0f},
    {"matlod_dist", "GXV_MATLOD_DIST", 1500, 100000, 'N', 'a', -1e9f, 0, 'b', 0, 3000.0f, 1e9f},
    {"fampose", "GXV_FAMPOSE", 0, 4, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 2.0f},
    {"fampose_dist", "GXV_FAMPOSE_DIST", 1500, 100000, 'N', 'a', -1e9f, 0, 'b', 0, 3000.0f, 1e9f},
    {"fampose_move", "GXV_FAMPOSE_MOVE", 0.1f, 20, 'N', 'a', -1e9f, 0, 'b', 0, -1e9f, 2.0f},
    {"piecethin", "GXV_PIECETHIN", 0, 4, 'N', 'a'},
    {"piecethin_dist", "GXV_PIECETHIN_DIST", 1500, 100000, 'N', 'a'},
    // ---- policy26 (GXV_SMOOTH60, port/smooth60.h): far-actor logic level of the stage (decomp ActorTurnLod.cpp port_farlogic_set: the backend carries it out at the stage
    // load; 0 off, 1 decoration / star bits / bees beyond 5000, 2 also kotebug / karipon / piranha / rock beyond 2500, 3 + far poses) and the per-stage ladder of the
    // smooth-60 controller: s60_start the mode the stage begins in (0 = 60 as the row says, 1 = 848x480, 2 = an even 30), s60_848 / s60_30 whether the controller may
    // step the stage down to 848x480 / to the even 30 when its pictures do not hold. Read by the back end from the stage's row, not by the game.
    // ---- ab27 (13 Nov): switches that were read once (a static) and now go through the registry, so the A/B driver can flip them in play: the pass hoist (PlanHoists, a live knob), the shadow fill's
    // bounds (read per draw through PORT_STAGE_KNOB), the even pacer (re-configured by a change callback; its learned period starts again, FlipClass 'w')
    {"passhoist", "GXV_PASSHOIST", 0, 2, 'N', 'a'},
    {"shadowfill_bounds", "GXV_SHADOWFILL_BOUNDS", 0, 1, 'N', 'a'},
    {"even26", "GXV_EVEN26", 0, 2, 'N', 'a'},
    {"far_logic", "GXV_FARLOGIC", 0, 3, 'N', 'a'},
    {"s60_start", "GXV_S60_START", 0, 3, 'N', 'a'},  // (gpugen26: 3 = begin at 896x512)
    {"s60_848", "GXV_S60_848", 0, 1, 'N', 'a'},
    {"s60_30", "GXV_S60_30", 0, 1, 'N', 'a'},
    // gpugen26: s60_896 whether the controller may use the middle rung 896x512 on this stage (default: what s60_848 says; needs GXV_SMOOTH60_MID=1), s60_hud whether its 848 / 896
    // modes draw the 2D layer at the screen's own size (hud_native=1 in the mode's spec; default: GXV_SMOOTH60_HUD). Read by the back end from the stage's row, not by the game.
    {"s60_896", "GXV_S60_896", 0, 1, 'N', 'a'},
    {"s60_hud", "GXV_S60_HUD", 0, 1, 'N', 'a'},
};
// Whether a preset line's value of `d` is exact (applies in the 30 mode too). Exact is a property of the key and its value, never of the stage.
inline bool IsExact(const Desc& d, float v) { return d.ex == 'x' || (d.ex == 'p' && v <= d.exactMax); }
// q30-26: what the Quality (30) mode does with a preset line's value `v` of `d`: 0 skipped (a sacrifice), 1 kept as it is, 2 kept but clamped (*out is the value to use): a 'b' key into [bMin, bMax], a 'p' key down to its exactMax.
inline int QualityValue(const Desc& d, float v, float* out) {
    if (out) *out = v;
    if (IsExact(d, v)) return 1;
    if (d.ex == 'p') { if (out) *out = d.exactMax; return 2; }   // (a partly exact key: the exact part is kept, lightvol 7 -> 3, mirror 2 -> 1, colshadow_clip 2 -> 1)
    if (d.ex != 'b') return 0;
    const float c = v > d.bMax ? d.bMax : (v < d.bMin ? d.bMin : v);
    if (out) *out = c;
    return c == v ? 1 : 2;
}
inline float ClampOff(const Desc& d, float v, bool allowOff, bool* clamped) {
    if (clamped) *clamped = false;
    if (!allowOff && d.offVal > -1e8f && v == d.offVal) { if (clamped) *clamped = true; return d.weakest; }
    return v;
}
inline constexpr int kDescCount = static_cast<int>(sizeof kDesc / sizeof kDesc[0]);
// ab27: may the A/B driver (or anything else) change this key in play, with no reload?  'y' yes: a pure per-picture / per-use decision, read at its next use (stage_knob how 'a' / 'g' / 'c').
// 'w' yes, but a retained state is rebuilt (a target, a program, a learned period): the first pictures after a flip are not the steady state (the driver adds GXV_AB_WARM_S to the settle).
// 'n' no: read by the back end from the stage's row at the load (s60_*), or a governor's own setting (the driver freezes the governors; name the cpulod / glow26 level instead).
inline char FlipClass(const Desc& d) {
    const char* a = d.alias;
    if (std::strncmp(a, "s60_", 4) == 0) return 'n';
    if (std::strncmp(a, "fe_gov", 6) == 0 || std::strncmp(a, "game_gov", 8) == 0 || std::strncmp(a, "cpu_gov", 7) == 0 || std::strncmp(a, "detail_", 7) == 0) return 'n';
    if (std::strncmp(a, "fxlow", 5) == 0 || std::strcmp(a, "hud_native") == 0 || std::strcmp(a, "hsr_auto") == 0 || std::strcmp(a, "even26") == 0 || std::strcmp(a, "far_logic") == 0) return 'w';
    return 'y';
}
// A preset key: the alias, or the switch's own name. Null when it is not stage-settable.
inline const Desc* FindDesc(const char* key) {
    for (int i = 0; i < kDescCount; ++i)
        if (std::strcmp(kDesc[i].alias, key) == 0 || std::strcmp(kDesc[i].name, key) == 0) return &kDesc[i];
    return nullptr;
}

inline char FlipClassOfName(const char* name) {
    const Desc* d = FindDesc(name);
    return d ? FlipClass(*d) : 'n';
}

}  // namespace stageknob
}  // namespace port

// Like PORT_HOT_KNOB (T at most 4 bytes) and registered for Commit(): `expr` reads its switches with port::stageknob::Int / Num.
#define PORT_STAGE_KNOB(T, ...) \
    ([]() -> T { \
        static_assert(sizeof(T) <= 4, "PORT_STAGE_KNOB: a word-sized value"); \
        static std::atomic<uint32_t> port_knob_{::port::hotknob::kUnset<T>}, port_knob_fenced_{::port::hotknob::kUnset<T>}; \
        static std::atomic<bool> port_knob_reg_{false}; \
        return ::port::stageknob::detail::Read<T>(port_knob_, port_knob_fenced_, port_knob_reg_, []() -> T { return static_cast<T>(__VA_ARGS__); }); \
    }())

// ab27: the registry's live knob under a global name (see "LIVE INTEGER KNOBS" in this header): port_knob_int("GXV_X", default) is a live int that Set() + Commit() refresh.
inline std::atomic<int32_t>& port_knob_int(const char* name, long dflt, bool flipSafe = true) { return ::port::stageknob::LiveInt(name, dflt, flipSafe); }
