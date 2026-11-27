// cpucuts25 (11 Nov 2026): the pure parts of the CPU-limited-stage cuts (no game types; host-tested by tests/cpucuts.cpp). Everything defaults to 0.
// The glue is in ClippingActorHolder.cpp (draw census, the class gate and the hold of GXV_FARDRAW, the flush flag), LiveActor.cpp (the flush skip,
// the character pose rule), stage_knob.h (the stage-settable names), entry.cpp (setenv defaults).
//
//   GXV_DRAWCENSUS=1          "drawcensus:" lines every ~5 s: the actors / shapes the drawers get, by camera distance band x projected size, by class, and by
//                             family (the actor's name), with the estimated game-thread and front-end ms of each size cut (nothing is changed).
//   GXV_FARDRAW_KINDS=<mask>  which classes GXV_FARDRAW may take (default 1 = pure decoration only, as before): 1 pure decoration, 2 decoration with
//                             effects / sound / switch, 4 enemies, 8 map objects whose sensors are the plain map-object kinds, 16 NPCs / animals.
//                             Never: collision, collectibles, items, switches, launchers, bosses, anything with a sensor kind not listed above, Mario.
//   GXV_FARDRAW_HOLD=<turns>  a model stamped by GXV_FARDRAW is held for at least this many turns before it may come back (0 = as before; with the
//                             size hysteresis this stops a bobbing or turning model from flickering at its limit).
//   GXV_FARDRAW_FLUSH=1       a far model that GXV_FARDRAW stamped, listed for the drawn turn's pose flush and posed once before, is not posed (nor its
//                             material list made) for a picture it is not in: it is posed again the picture it comes back (the clock runs on).
//   GXV_CHARPOSE=<n>          (2..4) enemies and items far from the camera and the player, small on the picture and nearly still on it are posed every
//                             n-th turn (the pose of the other turns is the last one; movement, sensors, the animation clock are untouched).
#pragma once

#include <cstdint>
#include <cstring>

namespace port {
namespace cpucuts {

// ---------------------------------------------------------------------------------------------------------------------------------
// classes of an actor for the draw cut (bits). 0 = never.
enum ClassBit : uint8_t { kCbDecor = 1, kCbDecorFx = 2, kCbEnemy = 4, kCbMapObj = 8, kCbNpc = 16 };
constexpr uint8_t kCbAll = 31;
constexpr uint8_t kSensorKeep = 0x80;  // a sensor of a kind the cut never takes (collectibles, items, switches, bosses, launchers, ...)

// Hit-sensor types (decomp/include/Game/Util/ActorSensorUtil.hpp); only the ranges the cut takes are named here.
constexpr uint32_t kTypeNpc = 0x5, kTypeAnimal = 0x7;
constexpr uint32_t kTypeEnemyFirst = 0x14, kTypeEnemyLast = 0x2B;  // ATYPE_ENEMY .. ATYPE_WANWAN (the plain enemies; bosses are above)
constexpr uint32_t kTypePlayerAutoJump = 0x1F;                    // inside that range, but a launcher
constexpr uint32_t kTypeMapObj = 0x46, kTypeMapObjSimple = 0x47;
inline uint8_t ClassOfSensorType(uint32_t t) {
    if (t >= kTypeEnemyFirst && t <= kTypeEnemyLast && t != kTypePlayerAutoJump) return kCbEnemy;
    if (t == kTypeMapObj || t == kTypeMapObjSimple) return kCbMapObj;
    if (t == kTypeNpc || t == kTypeAnimal) return kCbNpc;
    return kSensorKeep;
}
// The class bits an actor with these sensor types needs (OR); kSensorKeep when any sensor is of a kind the cut never takes, 0 for no sensor.
inline uint8_t ClassOfSensors(const uint32_t* types, int n) {
    uint8_t c = 0;
    for (int i = 0; i < n; ++i) c |= ClassOfSensorType(types[i]);
    return c;
}
// The class an actor has for the cut. kind: the cliprange Kind as an int (plant 0, collision 1, sensor 2, decor 3, decor+fx 4, nomodel 5);
// sensorBits: ClassOfSensors for a sensor-kind actor. Returns the bits the actor NEEDS to be allowed (0 = never).
inline uint8_t ClassOfActor(int kind, uint8_t sensorBits) {
    switch (kind) {
        case 3: return kCbDecor;
        case 4: return kCbDecorFx;
        case 2: return (sensorBits == 0 || (sensorBits & ~static_cast<uint8_t>(kCbEnemy | kCbMapObj | kCbNpc)) != 0) ? 0 : sensorBits;  // only the three takeable sensor classes
        default: return 0;
    }
}
// prog32b (20 Nov 2026): OBJECTIVE ACTORS ARE NEVER TAKEN (GXV_FARDRAW_OBJECTIVE, default 1; 0 = the 3.0 candidate's old behaviour). The class gate above sorts by
// sensor type, and the plain Enemy / MapObj / Npc sensor types are shared by things a star can depend on: switches and cages (MapObj), bosses and the fights' own
// enemies (Enemy), bunnies, penguins, Toads, Rosalina, Lumas (Npc / Animal). With GXV_CPULOD=2 (Performance) the gate's mask is 31, so a far, small bunny or a
// far switch was a "speck" and was not drawn (draw only: its logic ran, but the player had nothing to head for). An actor is objective when
//   - it owns a stage switch, a camera controller or a star-pointer target (the engine's own signal that something is triggered by it or aimed at: a "defeat
//     them all" enemy, a switch listener, a cue), or, for a sensor actor, a rail (patrols and moving parts the player has to follow), or
//   - its name starts with one of the families below, or contains "Switch".
// The class is then 0 ("never"), the same as a collision or a collectible. tests/objective_cuts.cpp checks the rule against every actor class of the decomp.
inline bool NameStarts(const char* n, const char* p) {
    for (; *p; ++p, ++n)
        if (*n != *p) return false;
    return true;
}
inline bool ObjectiveName(const char* n) {
    if (n == nullptr) return false;
    static const char* const kPrefix[] = {"Boss", "Kameck", "Koopa", "DinoPackun", "Tripod", "Teresa", "Topman", "PowerStar", "GrandStar", "Kinopio", "Penguin",
                                          "Rabbit", "MoonRabbit", "Rosetta", "StrayTico", "Tico", "Luma", "PunchBox", "SwitchBox", "CrystalCage", "BreakableCage"};
    for (const char* p : kPrefix)
        if (NameStarts(n, p)) return true;
    for (const char* q = n; *q; ++q)
        if (NameStarts(q, "Switch")) return true;
    return false;
}
// The class for the cut with the objective rule applied. `bound`: a stage switch, a camera controller or a star-pointer target; `rail`: a rail rider.
inline uint8_t ClassOfActorGuarded(int kind, uint8_t sensorBits, bool bound, bool rail, const char* name, bool guard = true) {
    const uint8_t c = ClassOfActor(kind, sensorBits);
    if (!guard || c == 0) return c;
    if (bound || (kind == 2 && rail) || (kind == 2 && ObjectiveName(name))) return 0;
    return c;
}
// may the cut take an actor of class `cls` when `allowed` is GXV_FARDRAW_KINDS?
inline bool Allowed(uint8_t cls, unsigned allowed) { return cls != 0 && (cls & ~allowed) == 0; }

// ---------------------------------------------------------------------------------------------------------------------------------
// hold: the age of a stamp (1 = stamped this turn .. 255 saturating; 0 = not stamped). A stamp that wants to be released is kept until it is `hold` old.
inline uint8_t AgeNext(uint8_t age) { return age == 0 ? 1 : (age == 255 ? 255 : static_cast<uint8_t>(age + 1)); }
inline bool HoldKeeps(uint8_t age, int hold) { return age != 0 && hold > 0 && age < hold; }

// ---------------------------------------------------------------------------------------------------------------------------------
// the draw census
constexpr int kDcBands = 5;                        // camera distance: 0-1k, 1k-2.5k, 2.5k-5k, 5k-10k, >10k
constexpr int kDcSizes = 10;                       // lines: <2, 2-3, 3-4, 4-6, 6-8, 8-12, 12-16, 16-24, 24-32, >=32
constexpr float kDcSizeEdge[kDcSizes - 1] = {2, 3, 4, 6, 8, 12, 16, 24, 32};
constexpr int kDcClasses = 7;                      // 0 plant / other kinds, 1 decor, 2 decor+fx, 3 enemy, 4 map object, 5 npc, 6 never (collision, collectible, ...)
constexpr int kDcFamCap = 192;                     // family table (open addressing; the rest counts under "other")
constexpr int kDcFamName = 28;
inline int DcSize(float lines) {
    for (int i = 0; i < kDcSizes - 1; ++i)
        if (lines < kDcSizeEdge[i]) return i;
    return kDcSizes - 1;
}
inline const char* DcSizeName(int s) {
    static const char* const n[kDcSizes] = {"<2", "2-3", "3-4", "4-6", "6-8", "8-12", "12-16", "16-24", "24-32", ">=32"};
    return s >= 0 && s < kDcSizes ? n[s] : "?";
}
inline int DcBandOfCamera(float d2) {
    if (d2 <= 1000.0f * 1000.0f) return 0;
    if (d2 <= 2500.0f * 2500.0f) return 1;
    if (d2 <= 5000.0f * 5000.0f) return 2;
    if (d2 <= 10000.0f * 10000.0f) return 3;
    return 4;
}
inline const char* DcBandName(int b) {
    static const char* const n[kDcBands] = {"0-1k", "1k-2.5k", "2.5k-5k", "5k-10k", ">10k"};
    return b >= 0 && b < kDcBands ? n[b] : "?";
}
inline int DcClassIndex(uint8_t cls) {  // census row of a class (bits)
    switch (cls) {
        case kCbDecor: return 1;
        case kCbDecorFx: return 2;
        case kCbEnemy: return 3;
        case kCbMapObj: return 4;
        case kCbNpc: return 5;
        case 0: return 6;
        default: return (cls & kCbEnemy) ? 3 : ((cls & kCbMapObj) ? 4 : 5);  // a mixed class (several sensors): its first part
    }
}
inline const char* DcClassName(int c) {
    static const char* const n[kDcClasses] = {"other", "decor", "decor+fx", "enemy", "mapobj", "npc", "never"};
    return c >= 0 && c < kDcClasses ? n[c] : "?";
}
inline uint32_t DcHashName(const char* s) {
    uint32_t h = 2166136261u;
    for (; s && *s; ++s) h = (h ^ static_cast<unsigned char>(*s)) * 16777619u;
    return h ? h : 1u;
}
struct DcFam {
    uint32_t hash = 0;
    char name[kDcFamName] = {};
    uint8_t cls = 0;
    uint32_t actors = 0, shapes = 0;      // all drawn, any distance
    uint32_t farActors = 0, farShapes = 0;  // beyond the near limit from the camera AND the player
    uint32_t under8 = 0, under16 = 0;     // far shapes under 8 / 16 lines
};
struct DcCell {
    uint64_t actors = 0, shapes = 0;
};
struct DrawCensus {
    uint32_t turns = 0, pictures = 0;
    DcCell all[kDcBands][kDcSizes];                 // every drawn actor but the player, by camera band and size
    DcCell far[kDcClasses][kDcSizes];               // far actors (the cut's reach), by class and size
    uint64_t playerShapes = 0, drawnActors = 0, drawnShapes = 0, stamped = 0, stampedShapes = 0;
    DcFam fam[kDcFamCap];
    uint32_t famUsed = 0, famOverflowActors = 0, famOverflowShapes = 0;
    void Reset() { *this = DrawCensus(); }
    DcFam* Find(const char* name, bool create) {
        const uint32_t h = DcHashName(name);
        uint32_t i = h % kDcFamCap;
        for (int probe = 0; probe < kDcFamCap; ++probe, i = (i + 1) % kDcFamCap) {
            if (fam[i].hash == h) return &fam[i];
            if (fam[i].hash == 0) {
                if (!create) return nullptr;
                fam[i].hash = h;
                std::strncpy(fam[i].name, name ? name : "?", kDcFamName - 1);
                ++famUsed;
                return &fam[i];
            }
        }
        return nullptr;
    }
    // one drawn actor (not the player): its camera band, size (lines), class bits, shape count, and whether it is far (the cut's reach)
    void Add(const char* name, int band, float lines, uint8_t cls, uint32_t shapes, bool isFar) {
        const int s = DcSize(lines);
        ++drawnActors;
        drawnShapes += shapes;
        ++all[band][s].actors;
        all[band][s].shapes += shapes;
        DcFam* f = Find(name, true);
        if (f == nullptr) {
            ++famOverflowActors;
            famOverflowShapes += shapes;
        } else {
            f->cls = cls;
            ++f->actors;
            f->shapes += shapes;
            if (isFar) {
                ++f->farActors;
                f->farShapes += shapes;
                if (lines < 8.0f) f->under8 += shapes;
                if (lines < 16.0f) f->under16 += shapes;
            }
        }
        if (isFar) {
            ++far[DcClassIndex(cls)][s].actors;
            far[DcClassIndex(cls)][s].shapes += shapes;
        }
    }
    // far actors / shapes under `edgeIdx` buckets (i.e. under kDcSizeEdge[edgeIdx - 1] lines), of the classes in `mask` (bit per census class row)
    void Under(int edgeIdx, unsigned classRows, uint64_t* actors, uint64_t* shapes) const {
        uint64_t a = 0, sh = 0;
        for (int c = 0; c < kDcClasses; ++c) {
            if (!(classRows & (1u << c))) continue;
            for (int s = 0; s < edgeIdx && s < kDcSizes; ++s) {
                a += far[c][s].actors;
                sh += far[c][s].shapes;
            }
        }
        *actors = a;
        *shapes = sh;
    }
};
// the census rows a GXV_FARDRAW_KINDS mask takes
inline unsigned DcRowsOfMask(unsigned mask) {
    unsigned r = 0;
    if (mask & kCbDecor) r |= 1u << 1;
    if (mask & kCbDecorFx) r |= 1u << 2;
    if (mask & kCbEnemy) r |= 1u << 3;
    if (mask & kCbMapObj) r |= 1u << 4;
    if (mask & kCbNpc) r |= 1u << 5;
    return r;
}
// The estimate of what a cut saves, ms a drawn turn: shapes x draw us (game thread) + actors x flush us (a model left out of the pose / material-list
// flush; 0 when the flush skip is off), and shapes x front-end us (the other thread).
struct DcEst {
    double gameMs, feMs;
};
inline DcEst DcEstimate(uint64_t actors, uint64_t shapes, double usShape, double usActorFlush, double usShapeFe) {
    return {(static_cast<double>(shapes) * usShape + static_cast<double>(actors) * usActorFlush) * 1e-3, static_cast<double>(shapes) * usShapeFe * 1e-3};
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the animation clock under a skipped pose. A model of XanimePlayer's protocol (decomp/src/Game/Animation/XanimePlayer.cpp): updateAfterMovement
// advances the frame only while the latch (_88) is clear and sets it, remembering the frame it left in _84; calcAnm poses _84 (while the latch is set) and
// clears the latch. A calcAnim that is left out must clear the latch itself (LiveActor.cpp does), or the next movement would not advance. The test
// proves with this model that the frame advances by one per movement whatever the pose pattern, and that a posed turn shows the frame the game would
// have posed on that very turn.
struct ClockModel {
    double frame = 0.0, rate = 1.0, f84 = 0.0;
    bool latch = false;
    void Movement() {  // updateAfterMovement
        if (!latch) {
            const double was = frame;
            frame += rate;
            f84 = was;
            latch = true;
        }
    }
    double Pose() {  // calcAnm: the frame posed
        const double shown = latch ? f84 : frame;
        latch = false;
        return shown;
    }
    void LeftOut() { latch = false; }  // what LiveActor::calcAnim's left-out path does
};

// The pose rule for characters (GXV_CHARPOSE): pose this turn (false) or leave it out (true). The same cadence as GXV_FARANIM (cliprange.h FarAnimSkip)
// with a displacement gate in lines: an actor that moved more than maxLines on the picture since its last pose is posed.
inline bool CharPoseSkip(uint8_t& skips, int n, uint32_t seed, float moved2, float dist, float tanHalf, float maxLines) {
    if (n < 2) {
        skips = 0xFF;
        return false;
    }
    const float unitsPerLine = (dist > 1.0f && tanHalf > 0.0f) ? 2.0f * dist * tanHalf / 456.0f : 0.0f;
    const float lim = maxLines * unitsPerLine;
    if (!(unitsPerLine > 0.0f) || moved2 > lim * lim) {
        skips = 0;  // posed now; the count starts again
        return false;
    }
    if (skips == 0xFF || skips >= n) {
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

}  // namespace cpucuts
}  // namespace port
