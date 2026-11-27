// GXV_FARLOGIC (farlogic25, 11 Nov 2026): the pure rules of the far-logic level (no game types; host-tested by tests/farlogic.cpp).
// The game side is in decomp/src/Game/LiveActor/ActorTurnLod.cpp (decision, dry counts, log, setter / getter), LiveActor.cpp (the
// calcAnim of a left-out turn, level 3's pose rule) and entry.cpp (setenv defaults, all 0).
//
// WHAT IT DOES (and what it does not). An actor in the "far state" runs its movement (and its calcAnim) on every second turn only. The left-
// out turn is NOT replayed: the game's logic is frame based (LiveActor::movement has no dt: velocities are per step, nerves count steps,
// the BCK clock advances one step per movement), so an actor that takes half the steps goes at HALF SPEED in its own time: walks, spins,
// bobs, timers and animation all run at half pace while it is far, and at full pace the moment it is not. That is the sacrifice. There is
// no generic compensated step (running the movement twice every second turn costs what it saves; a doubled velocity is not what the
// nerves, binder and effects do). GXV_ALOD (actorlod23) is the exact alternative: it replays every left-out step before the actor's next
// one, so nothing changes in time, and so it saves only the calcAnim, the sensor update and the shadow request (alodfix23 measured
// 0.05-0.1 ms in Honeyhive).
//
// LEVELS (port_farlogic_set / port_farlogic_level; GXV_FARLOGIC is the global default)
//   0  off.
//   1  "ambient" actors (pure decoration, and the star bit / coin / honeybee families) beyond 5000 units from the player AND the camera.
//   2  ambient and "mob" actors (kotebug, karipon, petit piranha, rolling rock) beyond 2500.
//   3  level 2, and characters (the Enemy and Item calc-anim lists) beyond 2500 are posed every other turn (the far-animation lever,
//      LiveActor.cpp PbAnimSkips), logic untouched.
// Never touched at any level: the player, anything with collision of its own (platforms, planets, doors), plants (their own cuts), actors with
// a stage switch, a rail, a camera controller, no model, or a hit sensor and no listed family (enemies and NPCs that can reach or shoot the
// player, switches, bosses: they stay at full rate), an actor with a sensor hit this turn, carried or carrying, and every actor in a demo,
// with the player dead or in a rush, with a mirror camera, or while the camera is not known.
#pragma once

#include <stdint.h>

namespace port {
namespace farlogic {

constexpr int kMaxLevel = 3;
constexpr float kNearFloor = 2000.0f;  // no level may act nearer than this to the player or the camera (units); a rule under it is "never"
constexpr float kHyst = 0.10f;         // an actor in the far state stays in it until it is 10 % of its limit nearer than the limit

// The families of the game's marked classes (the same ids as actorlod23's ActorTurnLod.cpp; 0 = none).
enum Family : uint8_t { kFamNone = 0, kFamStarPiece, kFamCoin, kFamKoteBug, kFamKaripon, kFamPackunPetit, kFamHoneyBee, kFamRock, kFamN };
constexpr int kBuckets = 9;  // census rows: 0 unlisted sensor actors, 1..7 the families, 8 decoration (no sensor)

enum Class : uint8_t { kNever = 0, kAmbient, kMob };
// Why an actor is "never" (census rows)
enum Why : uint8_t { kNvNone = 0, kNvPlayer, kNvPlant, kNvCollision, kNvBound, kNvSensor, kNvNoModel, kNvN };
inline const char* WhyName(int w) {
    static const char* const n[kNvN] = {"-", "player", "plant", "collision", "bound (switch/rail/camera/pointer)", "sensor (family not listed)", "no model"};
    return w >= 0 && w < kNvN ? n[w] : "?";
}

struct Facts {
    bool hasModel = false;
    bool collision = false;    // mCollisionParts
    bool sensors = false;      // a sensor keeper with at least one sensor
    bool stageSwitch = false;  // mStageSwitchCtrl
    bool rail = false;         // mRailRider
    bool camCtrl = false;      // mCameraCtrl
    bool pointer = false;      // mStarPointerTarget
    bool freeJoints = false;   // mLogicFreeJoints (flower / bush members)
    bool player = false;
    bool objective = false;    // far49: an objective by name (Penguin*, Kinopio*, Boss*, ...) or an actor no port cut may touch (mPortNoCut / mFarProt)
    uint8_t family = kFamNone;
};

// The class of an actor from what its own parts say. Conservative: anything that could be observed or that others depend on is "never".
// A listed family is allowed its pointer target (star bits are aimed at), effects and sound (every family has them).
inline Class Classify(const Facts& f, Why* why) {
    Why w = kNvNone;
    Class c = kNever;
    if (f.player) {
        w = kNvPlayer;
    } else if (f.objective) {
        w = kNvBound;  // (far49: the player deals with it: never at half pace, whatever its parts say)
    } else if (f.freeJoints) {
        w = kNvPlant;
    } else if (f.collision) {
        w = kNvCollision;
    } else if (f.stageSwitch || f.rail || f.camCtrl) {
        w = kNvBound;
    } else if (f.family != kFamNone && f.family < kFamN) {
        c = (f.family == kFamStarPiece || f.family == kFamCoin || f.family == kFamHoneyBee) ? kAmbient : kMob;
    } else if (f.sensors) {
        w = kNvSensor;
    } else if (!f.hasModel) {
        w = kNvNoModel;
    } else if (f.pointer) {
        w = kNvBound;
    } else {
        c = kAmbient;  // a model and nothing the game or the player could observe but its effects and sound
    }
    if (why != nullptr) {
        *why = w;
    }
    return c;
}
inline int BucketOf(const Facts& f) { return f.family != kFamNone && f.family < kFamN ? f.family : (f.sensors ? 0 : 8); }

// ---------------------------------------------------------------------------------------------------------------------------------
// the level table: the distance (from the player AND the camera) beyond which a class is in the far state; 0 = never at this level
struct Rule {
    float ambient;
    float mob;
    bool pose;  // level 3: characters beyond `ambient` are posed every other turn (LiveActor.cpp)
};
inline Rule RuleFor(int level) {
    switch (level) {
    case 1: return Rule{5000.0f, 0.0f, false};
    case 2: return Rule{2500.0f, 2500.0f, false};
    case 3: return Rule{2500.0f, 2500.0f, true};
    default: return Rule{0.0f, 0.0f, false};
    }
}
inline float BaseLimit(Class c, const Rule& r) {
    const float v = c == kAmbient ? r.ambient : c == kMob ? r.mob : 0.0f;
    return v >= kNearFloor ? v : 0.0f;  // (a limit under the floor is no limit)
}
inline int ClampLevel(int v) { return v < 0 ? 0 : v > kMaxLevel ? kMaxLevel : v; }
// MP3: the level that is acted on. While a multiplayer session is active the far-logic level is 0: far actors run at half pace by their distance
// from the LOCAL camera and player, which differs on every console, so the consoles' worlds would drift apart (a desync). The stored level is kept
// and is back the moment the session ends. Also the Quality step 3l's far logic (a level-1 rule from 100 m) is off in a session.
inline int LevelInSession(int level, bool mpActive) { return mpActive ? 0 : level; }

// The distance an actor must be beyond: the level's base, plus its model's radius and (a mob) its sensors' reach, plus what the player can
// cover in the 2 turns of one cycle (twice, as actorlod23's gate; `playerSpeed` is the faster of the player and the camera, so a camera flight
// widens it), all times the zoom ratio (a handheld zoom shows the far larger).
inline float Effective(float base, float radius, float reach, float playerSpeed, float zoom) {
    const float s = playerSpeed > 0.0f ? (playerSpeed < 1000.0f ? playerSpeed : 1000.0f) : 0.0f;
    const float z = zoom < 1.0f ? 1.0f : zoom > 4.0f ? 4.0f : zoom;
    const float r = radius > 0.0f ? radius : 0.0f;
    const float q = reach > 0.0f ? reach : 0.0f;
    return (base + r + q + 4.0f * s) * z;
}
// Hysteresis, on squared distances: the state enters beyond `limit` and leaves only inside limit * (1 - hyst). Idempotent for one position.
inline bool Beyond2(float d2, float limit, bool was, float hyst) {
    const float l = was ? limit * (1.0f - hyst) : limit;
    return d2 > l * l;
}
// The far state of one level: beyond for the player AND for the camera (each with its own hysteresis, the one state bit).
inline bool FarState(bool was, float playerD2, float camD2, float limit) {
    return limit > 0.0f && Beyond2(playerD2, limit, was, kHyst) && Beyond2(camD2, limit, was, kHyst);
}

// Half rate: an actor in the far state is left out on every second turn; no two left-out turns in a row, spread by the actor's address.
inline bool SkipThisTurn(uint32_t turn, uint32_t phase) { return ((turn + phase) & 1u) != 0; }
inline uint32_t PhaseOf(const void* p) {
    const uintptr_t a = reinterpret_cast< uintptr_t >(p);
    return static_cast< uint32_t >((a >> 4) ^ (a >> 9));
}
// The class is read from the actor's parts once, then every 32 turns (the phase from its address): `age` is the actor's byte.
inline bool ReadDue(uint8_t& age, uint32_t seed, bool fresh) {
    if (fresh) {
        age = static_cast< uint8_t >(seed & 31u);
        return true;
    }
    return ((++age) & 31u) == 0;
}

// level 3's pose rule: the calc-anim lists whose joints may be posed every other turn beyond the rule's distance: Enemy (8), Item (0x10);
// the budget's own categories (environment, map objects, NPCs, decorations) have their rule already.
inline bool PoseCategory(int rawCalcAnimType) { return rawCalcAnimType == 8 || rawCalcAnimType == 0x10; }

// The dry mode's figure: what half rate would save a turn, from the time of the actors in the far state (movement + calcAnim, summed over the
// timed turns): the half of their time, per timed turn.
inline double SavedMsPerTurn(uint64_t moveNs, uint64_t calcNs, uint32_t timedTurns) {
    return timedTurns == 0 ? 0.0 : 0.5 * (static_cast< double >(moveNs) + static_cast< double >(calcNs)) / 1.0e6 / static_cast< double >(timedTurns);
}

}  // namespace farlogic
}  // namespace port

// ------------------------------------------------------------------------------------------------- game-side entry points
// (ActorTurnLod.cpp; C linkage so the stage-preset registry and platform code can name them)
#ifdef __cplusplus
extern "C" {
#endif
// The level now in force (0..3). A cached int: one relaxed load, no allocation, any thread.
int port_farlogic_level(void);
// Sets the level (clamped to 0..3), any thread, no allocation. Takes effect on the game thread's next turn. A level above 0 after a stage's
// actors were made marks the existing ones on the first turn the scene runs (ActorTurnLod.cpp); the actors made later are marked when made.
void port_farlogic_set(int level);
// 1 when the dry mode (GXV_FARLOGIC_DRY=1) is on: nothing is changed, what levels 1, 2 and 3 would take is counted and timed.
int port_farlogic_dry(void);
#ifdef __cplusplus
}
#endif
