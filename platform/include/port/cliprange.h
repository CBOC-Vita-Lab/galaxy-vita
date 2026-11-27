// GXV_CLIPCENSUS / GXV_CLIPSCALE / GXV_FARANIM / GXV_FARDRAW (cliprange25, 11 Nov 2026): the pure parts (no game types, host-tested by
// tests/cliprange.cpp). Everything defaults to 0. The glue is in ClippingActorHolder.cpp (census pass, the clip and draw levers),
// NameObjCategoryList.cpp (the per-object census timing), LiveActor.cpp (the pose lever, the "posed" mark).
//
//   GXV_CLIPCENSUS=1  one line group per stage every ~5 s: actors alive / running / posed / drawn per turn, by distance band from the PLAYER
//                     (0-1000, 1000-2500, 2500-5000, 5000-10000, >10000 units) and by kind (what the actor's own parts say it is), and the
//                     summed time of movement and calcAnim per band / kind / list category.
//   GXV_CLIPCENSUS=2  1, plus the DRY counts of what each lever below would take at its current strength (nothing is changed).
//   GXV_CR_NEAR=2500  no lever touches an actor nearer than this to the player OR to the camera (units).
//   GXV_CLIPSCALE=<percent>      (<100) pure decoration is clipped (the game's own startClipped) beyond <percent> of its far-clip distance.
//   GXV_FARANIM=<n>              (2..4) far decoration poses its joints every n-th turn (extends GXV_PB_ANIM's every 2nd).
//   GXV_FARDRAW=<lines>          (1..64) small far pure decoration is not drawn (draw only; the game keeps running it).
#pragma once

#include <cstdint>

namespace port {
namespace cliprange {

// ---------------------------------------------------------------------------------------------------------------------------------
// distance bands from the player
constexpr int kBands = 5;
constexpr float kBandEdge[kBands - 1] = {1000.0f, 2500.0f, 5000.0f, 10000.0f};
inline int BandOf(float d2) {
    // d2: squared distance in game units
    if (d2 <= kBandEdge[0] * kBandEdge[0]) return 0;
    if (d2 <= kBandEdge[1] * kBandEdge[1]) return 1;
    if (d2 <= kBandEdge[2] * kBandEdge[2]) return 2;
    if (d2 <= kBandEdge[3] * kBandEdge[3]) return 3;
    return 4;
}
inline const char* BandName(int b) {
    static const char* const n[kBands] = {"0-1k", "1k-2.5k", "2.5k-5k", "5k-10k", ">10k"};
    return b >= 0 && b < kBands ? n[b] : "?";
}

// ---------------------------------------------------------------------------------------------------------------------------------
// kinds: what an actor's own parts say it is. Only kDecor is ever touched by a lever.
enum Kind {
    kPlant = 0,   // a flower / bush group member or group (mLogicFreeJoints, mPbDecor): has its own cuts (GXV_PB_DECOR, GXV_DETAIL_PLANT)
    kColl,        // has collision of its own the player (or anything) can stand on / hit: map parts, platforms, planets
    kSens,        // has hit sensors (enemies, collectibles, switches, NPCs with a talk sensor, the player): can interact
    kDecor,       // model, no collision, no sensors, nothing else the game could observe: PURE decoration
    kDecorFx,     // model, no collision, no sensors, but it owns effects / sound / a stage switch / a rail / a camera controller / a pointer target
    kNoModel,     // nothing to draw (controllers, area-like actors)
    kKinds
};
inline const char* KindName(int k) {
    static const char* const n[kKinds] = {"plant", "collision", "sensor", "decor", "decor+fx", "nomodel"};
    return k >= 0 && k < kKinds ? n[k] : "?";
}
struct Facts {
    bool hasModel = false;
    bool collision = false;   // mCollisionParts != nullptr
    bool sensors = false;     // mSensorKeeper != nullptr && sensor count > 0
    bool effects = false;     // mEffectKeeper != nullptr
    bool sound = false;       // mSoundObject != nullptr
    bool stageSwitch = false; // mStageSwitchCtrl != nullptr
    bool rail = false;        // mRailRider != nullptr
    bool camCtrl = false;     // mCameraCtrl != nullptr
    bool pointer = false;     // mStarPointerTarget != nullptr (the Wii pointer / star pointer can aim at it)
    bool freeJoints = false;  // mLogicFreeJoints
    bool pbDecor = false;     // ClippingActorInfo::mPbDecor
    bool player = false;
};
inline Kind Classify(const Facts& f) {
    if (f.freeJoints || f.pbDecor) return kPlant;
    if (f.collision) return kColl;
    if (f.sensors || f.player) return kSens;
    if (!f.hasModel) return kNoModel;
    if (f.effects || f.sound || f.stageSwitch || f.rail || f.camCtrl || f.pointer) return kDecorFx;
    return kDecor;
}
// The single predicate every lever asks. Conservative: when any part of an actor could be observed (by the player, an enemy, a switch, the ear,
// the camera) it is not pure decoration.
inline bool PureDecor(const Facts& f) { return Classify(f) == kDecor; }

// ---------------------------------------------------------------------------------------------------------------------------------
// projected size. The picture is 456 lines tall at the game's field of view; the diameter of a sphere of `radius` at `dist` is
// radius * 456 / (dist * tan(fovy / 2)) lines (the same figure GXV_TINYCULL uses).
constexpr float kPictureLines = 456.0f;
inline float ProjLines(float radius, float dist, float tanHalf) {
    return (dist > radius && tanHalf > 0.0f) ? radius * kPictureLines / (dist * tanHalf) : 1.0e9f;
}
// A stable per-actor number in [0, 1) (from its address; used to spread an actor's limit so that nothing leaves in a crowd).
inline float Jitter01(uintptr_t addr) {
    uint32_t h = static_cast<uint32_t>(addr >> 3) * 0x9E3779B1u;
    h ^= h >> 15;
    h *= 0x85EBCA77u;
    h ^= h >> 13;
    return static_cast<float>(h & 0xFFFFu) * (1.0f / 65536.0f);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// GXV_CLIPSCALE: the distance beyond which a pure decoration is clipped (the camera distance of the sphere's centre less its radius).
//   base      the game's far-clip distance for it (the level's, or the far plane's)
//   pct       GXV_CLIPSCALE (1..99; 100 or more: no change, the caller does not call)
//   jitter    Jitter01 of the actor: the percent is spread +-10 % (0.9 .. 1.1 of it) so that a crowd does not leave at one distance
//   lines     GXV_CLIPSCALE_LINES: an actor that is still over this many lines big at the scaled distance keeps going until it is not
//   nearD     GXV_CR_NEAR: never closer than nearD + radius
// The result is never above `base`, so the switch can only clip EARLIER than the game does, never later.
inline float ClipLimit(float base, float pct, float jitter, float radius, float tanHalf, float lines, float nearD) {
    float lim = base * pct * 0.01f * (0.9f + 0.2f * jitter);
    if (tanHalf > 0.0f && lines > 0.0f) {
        const float sizeLim = radius * kPictureLines / (tanHalf * lines);  // beyond this it is under `lines` lines
        if (sizeLim > lim) lim = sizeLim;
    }
    if (lim < nearD + radius) lim = nearD + radius;
    if (lim > base) lim = base;
    return lim;
}
// Hysteresis: once beyond the limit it stays beyond until it has come back by `hyst` (a fraction) of the limit. Idempotent for one position.
inline bool Beyond(float dist, float limit, bool was, float hyst) { return dist > (was ? limit * (1.0f - hyst) : limit); }

// ---------------------------------------------------------------------------------------------------------------------------------
// GXV_FARANIM: every n-th turn the joints are posed; the others keep the last pose. `skips` is the actor's count of left-out poses since
// the last one made; 0xFF = fresh (not far before). Returns true = leave this turn's pose out. The first far call is made; the count then
// starts at a per-actor phase (seed % n), so a crowd of actors does not pose on the same turn. At most n-1 poses in a row are left out.
inline bool FarAnimSkip(uint8_t& skips, int n, uint32_t seed) {
    if (n < 2) {
        skips = 0xFF;
        return false;
    }
    if (skips == 0xFF || skips >= n) {  // fresh, or n has been lowered since: make this pose
        skips = static_cast<uint8_t>(seed % static_cast<uint32_t>(n));
        return false;
    }
    if (skips >= n - 1) {
        skips = 0;
        return false;
    }
    ++skips;
    return true;
}
// A pose is made anyway when the actor has moved more than `maxLines` lines on the picture since it was last posed (a decoration that
// travels does not judder): moved2 = squared displacement (units) of the actor against the position its pose was made at.
inline bool MovedTooMuch(float moved2, float dist, float tanHalf, float maxLines) {
    if (!(dist > 1.0f) || !(tanHalf > 0.0f)) return true;
    const float unitsPerLine = 2.0f * dist * tanHalf / kPictureLines;
    const float lim = maxLines * unitsPerLine;
    return moved2 > lim * lim;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// GXV_FARDRAW: pure decoration that is under `lines` lines (diameter of its clipping sphere) is not drawn. Each actor has its own threshold
// between `lines` and `lines * (1 + rampPct / 100)` (from its jitter), so the models leave one by one as they shrink, never together.
inline float DrawThreshold(float lines, float rampPct, float jitter) { return lines * (1.0f + rampPct * 0.01f * (1.0f - jitter)); }
// Hysteresis: a culled actor returns once it is `hyst` (fraction) over its threshold.
inline bool DrawCulled(float sizeLines, float threshold, bool was, float hyst) { return sizeLines < (was ? threshold * (1.0f + hyst) : threshold); }

// ---------------------------------------------------------------------------------------------------------------------------------
// the census accumulator (sums over the window's turns; the log divides by turns)
constexpr int kMoveCats = 48;  // MR::MovementType 0 .. 0x2D
constexpr int kCalcCats = 24;  // MR::CalcAnimType 0 .. 0x15
constexpr int kSizeBuckets = 5;  // decor beyond the near limit, by diameter in lines: <2, 2-6, 6-14, 14-30, >=30
inline int SizeBucket(float lines) { return lines < 2.0f ? 0 : lines < 6.0f ? 1 : lines < 14.0f ? 2 : lines < 30.0f ? 3 : 4; }
inline const char* SizeBucketName(int b) {
    static const char* const n[kSizeBuckets] = {"<2", "2-6", "6-14", "14-30", ">=30"};
    return b >= 0 && b < kSizeBuckets ? n[b] : "?";
}
struct Cell {  // one (kind, band)
    uint64_t alive = 0, clipped = 0, drawn = 0, shapes = 0;      // the clip pass (once a turn)
    uint64_t mvListed = 0, mvRan = 0, mvNs = 0, mvListedT = 0;   // movement lists (T: listed on the turns whose time was read)
    uint64_t caListed = 0, caRan = 0, caPosed = 0, caNs = 0, caListedT = 0;  // calc-anim lists
};
struct CatRow {
    uint64_t listed = 0, ran = 0, posed = 0, ns = 0, listedT = 0;
};
struct Lever {  // DRY: what a lever would take of the actors that are not clipped by the game (per window sums)
    uint64_t actors = 0, shapes = 0;
};
struct Census {
    uint32_t turns = 0, pictures = 0;
    Cell cell[kKinds][kBands];
    CatRow mv[kMoveCats], ca[kCalcCats];
    uint64_t unknownListed = 0;                       // listed objects that are not (or not yet) in the live actor snapshot
    uint64_t farClass[8] = {};                        // pure decoration, not clipped by the game, by its far-clip level (0..7)
    uint64_t decFar[kSizeBuckets] = {}, decFarShapes[kSizeBuckets] = {};  // pure decoration beyond the near limit (player AND camera), by size
    Lever dryClip, dryDraw, dryAnim;
    void Reset() { *this = Census(); }
};

}  // namespace cliprange
}  // namespace port
