// GXV_ALOD (actorlod23, 8 Nov 2026): the update LOD of far, off-screen actors in Performance mode. Rules: port/actor_lod.h.
//
// WHO: LiveActors of a marked family (MR::portMarkActorLod, called at the end of the class's init). Only classes that run
// LiveActor::movement and LiveActor::calcAnim unchanged are marked, so the catch-up (LiveActor::portMovementLite) is their own
// movement. Families, from the Honeyhive logic profile (cpuhh10.log, heaviest window; move + calc us a turn, profiled):
//   1 starpiece (StarPiece)  28 x 16.9 move, calc deferred to the flush (~11 anim + ~7 dl a picture)
//   2 coin (Coin)            18 x 8.2 move + 12.2 calc
//   3 kotebug (KoteBug)       6 x 29.6 move + 112.8 calc
//   4 karipon (Karikari)      5 x ~29 calc + move
//   5 packunpetit             1 x 48 calc + move
//   6 honeybee (HoneyBee)     8 x 27.4 move + 13.5 calc
//   7 rock (Rock)             2 x 146 move
// Bosses and everything they carry, Mario, collision objects, plants (gamepar's GXV_HR_PLANTMOVE) and map parts are never marked.
//
// WHEN an actor's movement is left out (every condition, every turn; the decision is made at its own place in its list):
//  - GXV_ALOD on (1, or "perf" in Performance mode), the scene's camera of this turn known (after the camera and clipping lists),
//    no demo running, no mirror camera in the scene, the player present, alive and not in a "rush" (launch star, cannon, bubble...).
//  - the actor: alive, not clipped, no collision of its own, not carried and not carrying (sensor keeper taking / taken), no
//    camera controller, NO HIT in any of its sensors this turn (the sensor checker ran before the actors' lists).
//  - the player farther than GXV_ALOD_PDIST (2500) + the actor's reach (farthest sensor offset + radius, + its model's radius) +
//    2 x the player's speed x the rate: a player cannot reach it while it stands behind.
//  - outside the picture: its bounding sphere (model radius x scale x 1.5 + 150, at least 300) and the segment each of its shadows can
//    fall on (drop position, direction and length + start offset, widened by the same margin) wholly outside this turn's view pyramid
//    widened by 15 % (port/view_cone.h, gamepar's host-proved test); or, with GXV_ALOD_TINY = n,
//    on the picture but farther than GXV_ALOD_TINYDIST (3000) from the camera and under n pixels in radius (at 544 lines).
//  - rate: quarter (3 turns out of 4 left out) when off the picture and farther than GXV_ALOD_FAR4 (5000) from both camera and
//    player, else half; GXV_ALOD_RATE (4) caps it. The turns are spread by the actor's address.
// A turn that is left out: no movement, no calcAnim (LiveActor.cpp), no shadow request; the view pass runs (the model stays placed
// for this camera); its level sounds (JAUSoundObject::startLevelSound, lifetime 1) are kept alive as the movement would have; with
// GXV_ALOD_DRAWCULL (1) a model off the picture is also not drawn in this turn's picture (DrawBuffer.cpp's gamepar cull).
// The next movement replays the owed steps first (port::alod::PayOwed): exact for the actor's own state, see LiveActor.cpp.
//
// WHAT DIFFERS from the original (the minor changes; none can be seen in a picture: the actor is off it, or a few pixels small):
//  - Order: the owed step runs one to three turns late, just before the actor's next own step. What other actors and the player
//    did meanwhile, it sees as of now: a message, a switch or a nerve set from outside while it stood behind takes effect one to
//    three of its steps EARLIER in its own time (the census counts these as "nerve changes from outside").
//  - The shared random sequence (MR::getRandom) is drawn in another order: random choices differ from an unmodified run (the
//    game never replays a run; this only matters for bit-exact A/B of whole runs).
//  - Its sensors keep their last positions while it stands behind (the checker sees them up to 3 turns old: it is beyond the
//    player's reach by the gate, and any hit puts it back to every turn at once). Its shadow is cast every second or fourth turn.
//  - Its effects' emitters follow it on its own turns only (particles of an off-screen actor; can show at the picture's edge).
// GXV_ALOD=2 (census): nothing is left out. Each turn mode 1 would leave out is counted and its movement and calcAnim are timed:
// the "alod:" lines then give, per family, the ms a turn mode 1 saves (minus its replay), and how often it would be reordered.
//
// GXV_FARLOGIC (farlogic25, 11 Nov 2026), at the end of this file (rules: port/farlogic.h): the same per-actor entry (port_alod_movement) with
// a second, cheaper kind of left-out turn: no replay. An actor in the "far state" (beyond 5000 / 2500 units from the player AND the camera,
// hysteresis, a safe family or pure decoration, no hit, no demo) runs its movement and calcAnim on every second turn, so in its own time it
// goes at HALF SPEED (the logic is frame based and has no dt; see port/farlogic.h). GXV_ALOD above is the exact variant (replay: nothing
// changes in time, only the calcAnim / sensor update / shadow request are saved). An actor of ALOD's seven families belongs to ALOD while
// GXV_ALOD is on, and to GXV_FARLOGIC otherwise. Levels 0..3 per stage through port_farlogic_set(); GXV_FARLOGIC_DRY=1 counts and times what
// levels 1 and 2 would take, and what level 3's pose rule would take, and changes nothing.
#include "Game/LiveActor/LiveActor.hpp"
#include <atomic>
#include "Game/Animation/XanimePlayer.hpp"
#include "Game/AudioLib/AudAnmSoundObject.hpp"
#include "Game/LiveActor/HitSensor.hpp"
#include "Game/LiveActor/HitSensorInfo.hpp"
#include "Game/LiveActor/HitSensorKeeper.hpp"
#include "Game/LiveActor/ModelManager.hpp"
#include "Game/LiveActor/ShadowController.hpp"
#include "Game/LiveActor/Spine.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/LiveActor/AllLiveActorGroup.hpp"  // (GXV_FARLOGIC: marking the actors made before the level was set)
#include "port/farprotect.h"
#include "port/gt71.h"  // (gt71)
#include "port/mp_bridge.h"                    // MP3: port_net_session_lockstep() (far logic is off in a held session)
#include "port/mp_gate.h"                      // MP3 lodgate: the other state-affecting speed features held in a session (port_mpgate_note / _idle)
#include "Game/Player/MarioAccess.hpp"             // (GXV_FARLOGIC: never the player)
#include "Game/LiveActor/ActorLightCtrl.hpp"       // (GXV_IDLE25)
#include "Game/LiveActor/ActorPadAndCameraCtrl.hpp"  // (GXV_IDLE25)
#include "Game/LiveActor/Binder.hpp"               // (GXV_IDLE25)
#include "Game/LiveActor/EffectKeeper.hpp"         // (GXV_IDLE25)
#include "Game/Animation/AnmPlayer.hpp"            // (GXV_IDLE25)
#include "Game/Animation/BpkPlayer.hpp"            // (GXV_IDLE25)
#include "Game/Animation/BrkPlayer.hpp"
#include "Game/Animation/BtkPlayer.hpp"
#include "Game/Animation/BtpPlayer.hpp"
#include "Game/Animation/BvaPlayer.hpp"
#include "Game/Animation/XanimeCore.hpp"
#include "Game/Animation/XanimeResource.hpp"
#include "Game/Enemy/AnimScaleController.hpp"      // (GXV_IDLE25: the petit's scale controller)
#include "Game/Enemy/PackunPetit.hpp"              // (GXV_IDLE25)
#include "Game/MapObj/Coin.hpp"                    // (GXV_IDLE25)
#include "Game/MapObj/StarPiece.hpp"               // (GXV_IDLE25)
#include "Game/Util/ActorSensorUtil.hpp"           // (GXV_IDLE25)
#include "Game/Util/LiveActorUtil.hpp"             // (GXV_IDLE25)
#include "Game/Util/MathUtil.hpp"                  // (GXV_IDLE25: MR::repeatDegree)
#include "Game/Util/SoundUtil.hpp"                 // (GXV_IDLE25)
#include "Game/Util/StarPointerUtil.hpp"           // (GXV_IDLE25)
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3DGraphBase/J3DSys.hpp>
#include <JSystem/JAudio2/JAISound.hpp>
#include <JSystem/JAudio2/JAUSoundObject.hpp>

#ifdef TARGET_PC
#include "port/actor_lod.h"
#include "port/stage_knob.h"  // (stagepreset25)
#include "port/audio_cpu1.h"
#include "port/diagnostics.h"
#include "port/farlogic.h"
#include "port/pose_phase.h"  // (ratecut31: GXV_POSE_PHASE, GXV_STARBATCH_FAR on the skip turn)
#include "port/lockstep.h"  // (fmt32: GXV_ALOD, GXV_FARLOGIC and GXV_IDLE25 are off in a lockstep session)
#include "port/lockstep_gate.h"
#include "port/hot_knob.h"
#include "port/idle25.h"
#include "port/view_cone.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

extern "C" float port_ql_logic_limit(void);   // (far3: perf.cpp)
extern "C" const TVec3f* port_safe_player_pos(void);  // (LodCtrl.cpp)
extern "C" int port_perf_mode_60(void);       // (platform/src/port/perf.cpp: Performance mode)
extern "C" int port_on_frame_thread(void);    // (perf.cpp)
extern "C" int port_idle25_mode(void);        // (GXV_IDLE25, below; the nerve probes are in StarPiece.cpp, Coin.cpp, PackunPetit.cpp)
extern "C" int port_safe_cam_exists(void);  // (LodCtrl.cpp)
extern "C" int port_idle25_starpiece_nerve(const void*);
extern "C" int port_idle25_coin_nerve(const void*);
extern "C" int port_idle25_petit_nerve(const void*);
namespace {
void IdLogMaybe();  // (GXV_IDLE25: defined with the rest of its block before port_alod_movement)
}

uint32_t gPortAlodTurn = 1;

namespace {

int AlodEnvMode() {
    long stageValue;
    if (port::stageknob::Overridden("GXV_ALOD", &stageValue)) {  // (stagepreset25: the stage's value first)
        return stageValue < 0 ? 0 : stageValue > 2 ? 2 : static_cast< int >(stageValue);
    }
    const char* e = getenv("GXV_ALOD");
    if (e == nullptr || *e == 0) {
        return 0;
    }
    if (*e == 'p' || *e == 'P') {
        return port_perf_mode_60() ? 1 : 0;  // ("perf": on in Performance mode only)
    }
    const int v = atoi(e);
    return v < 0 ? 0 : v > 2 ? 2 : v;
}
float AlodEnvFloat(const char* name, float fallback, float lo, float hi) {
    float stageValue;
    if (port::stageknob::Lookup(name, &stageValue)) {
        return stageValue < lo ? lo : stageValue > hi ? hi : stageValue;
    }
    const char* e = getenv(name);
    if (e == nullptr || *e == 0) {
        return fallback;
    }
    const float v = static_cast< float >(atof(e));
    return v < lo ? lo : v > hi ? hi : v;
}
int AlodEnvInt(const char* name, int fallback, int lo, int hi) {
    float stageValue;
    if (port::stageknob::Lookup(name, &stageValue)) {
        const int sv = static_cast< int >(stageValue);
        return sv < lo ? lo : sv > hi ? hi : sv;
    }
    const char* e = getenv(name);
    if (e == nullptr || *e == 0) {
        return fallback;
    }
    const int v = static_cast< int >(strtol(e, nullptr, 0));
    return v < lo ? lo : v > hi ? hi : v;
}
float KnobPDist() { return PORT_STAGE_KNOB(float, AlodEnvFloat("GXV_ALOD_PDIST", 2500.0f, 1000.0f, 100000.0f)); }
int AlodRate() {
    const int v = AlodEnvInt("GXV_ALOD_RATE", 4, 1, 4);
    return v >= 4 ? 4 : v >= 2 ? 2 : 1;  // (1: never left out)
}
int KnobMaxRate() { return PORT_STAGE_KNOB(int, AlodRate()); }
float KnobFar4() { return PORT_STAGE_KNOB(float, AlodEnvFloat("GXV_ALOD_FAR4", 5000.0f, 0.0f, 100000.0f)); }
float KnobTinyPx() { return PORT_STAGE_KNOB(float, AlodEnvFloat("GXV_ALOD_TINY", 0.0f, 0.0f, 64.0f)); }
float KnobTinyDist() { return PORT_STAGE_KNOB(float, AlodEnvFloat("GXV_ALOD_TINYDIST", 3000.0f, 1500.0f, 100000.0f)); }
int KnobDrawCull() { return PORT_STAGE_KNOB(int, AlodEnvInt("GXV_ALOD_DRAWCULL", 1, 0, 1)); }
int KnobFamilies() { return PORT_STAGE_KNOB(int, AlodEnvInt("GXV_ALOD_FAMILIES", 0x7F, 0, 0x7F)); }

constexpr int kFam = 8;  // (0 unused, 1..7)
const char* const kFamName[kFam] = {"-", "starpiece", "coin", "kotebug", "karipon", "packunpetit", "honeybee", "rock"};

// why an actor of a marked family moved this turn (census)
enum { kWhyGate, kWhyBusy, kWhyContact, kWhyNear, kWhyOnScreen, kWhyCycle, kWhyN };

struct FamStats {
    uint32_t marked;
    uint64_t decisions, leftOut, half, quarter, replayed, nerveOutside;
    uint64_t why[kWhyN];
    uint64_t wouldMoveNs, wouldCalcNs, moveNs, moveCalls;  // (census: the left-out-to-be turns' movement / calcAnim; all marked movement)
};

struct Turn {
    uint32_t camTurn = 0;  // the turn the fields below are for
    bool open = false;     // the global gate
    port::viewcone::Cone cone{};
    float tanHalf = 0.0f;
    TVec3f player{0.0f, 0.0f, 0.0f};
    TVec3f cam{0.0f, 0.0f, 0.0f};
    float playerSpeed = 0.0f;
    float fastSpeed = 0.0f;  // (GXV_FARLOGIC) the faster of the player and the camera, units a turn (a camera flight or cut widens the limits)
    uint32_t gateWhy = 0;  // 1 demo, 2 mirror, 4 no player / dead / rush, 8 camera
};

Turn sTurn;
FamStats sFam[kFam];
uint64_t sGateClosedTurns = 0, sGateWhy[4] = {};
uint32_t sLogTurn0 = 0;
int64_t sLogT0 = 0;
uint64_t sReplayNs = 0, sReplaySteps = 0;  // (sampled: one actor-turn in 8 with a debt)
uint32_t sReplaySample = 0;
uint32_t sMoveSample = 0;

float MaxAbsScale(const TVec3f& s) {
    const float x = fabsf(s.x), y = fabsf(s.y), z = fabsf(s.z);
    return x > y ? (x > z ? x : z) : (y > z ? y : z);
}

// the actor's sensor reach (farthest sensor offset + radius), once
float Reach(LiveActor* a) {
    if (a->mAlodReach >= 0.0f) {
        return a->mAlodReach;
    }
    float r = 0.0f;
    if (a->mSensorKeeper != nullptr) {
        // (crash33: mSensorInfosSize = the sensors added; mSensorCount is the table's capacity, its unused entries are null)
        for (s32 i = 0; i < a->mSensorKeeper->mSensorInfosSize; ++i) {
            const HitSensorInfo* info = a->mSensorKeeper->mSensorInfos[i];
            const float off = sqrtf(info->_C.x * info->_C.x + info->_C.y * info->_C.y + info->_C.z * info->_C.z);
            const float reach = off + info->mSensor->mRadius;
            if (reach > r) {
                r = reach;
            }
        }
    }
    a->mAlodReach = r;
    return r;
}

// the model's radius at the actor's scale (bounding radius from the joints of a full computation, taken once; GXV_ANIM_LOD's
// mLodRadius is the same quantity and is shared). <0: not known (the actor then always moves).
float ModelRadius(LiveActor* a) {
    if (a->mLodRadius < 0.0f) {
        if (a->mModelManager == nullptr) {
            return -1.0f;
        }
        f32 r = 0.0f;
        MR::calcModelBoundingRadius(&r, a);
        const float s = MaxAbsScale(a->mScale);
        a->mLodRadius = (r > 0.0f && r < 100000.0f && s > 0.001f) ? r / s : 0.0f;
    }
    if (!(a->mLodRadius > 0.0f)) {
        return -1.0f;
    }
    return a->mLodRadius * MaxAbsScale(a->mScale);
}

// Its shadows, each a segment from its drop position along its drop direction (as last computed: ShadowController::getDropPos /
// getDropDir, the gravity or the fixed direction) over its drop length plus start offset; the shadow lies on that segment, at most
// the actor's own margin `r` wide. true: every segment's sphere is off the picture.
bool ShadowsOff(const LiveActor* a, float r) {
    const ShadowControllerList* list = a->mShadowControllerList;
    if (list == nullptr) {
        return true;
    }
    const u32 n = list->getControllerCount();
    for (u32 i = 0; i < n; ++i) {
        const ShadowController* c = list->getController(i);
        if (c == nullptr) {
            continue;
        }
        TVec3f pos, dir;
        c->getDropPos(&pos);
        c->getDropDir(&dir);
        const float len = c->getDropLength() + fabsf(c->mStartOffset);
        const float d2 = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;
        if (!(len >= 0.0f) || !(len < 100000.0f)) {
            return false;
        }
        bool off;
        if (d2 > 0.98f && d2 < 1.02f) {
            const float h = len * 0.5f;
            off = port::viewcone::Outside(sTurn.cone, pos.x + dir.x * h, pos.y + dir.y * h, pos.z + dir.z * h, h + r);
        } else {
            off = port::viewcone::Outside(sTurn.cone, pos.x, pos.y, pos.z, len + r);  // (no direction yet: the whole ball)
        }
        if (!off) {
            return false;
        }
    }
    return true;
}

// Level sounds (lifetime 1: JAUSoundObject::startLevelSound) die at the end of a turn that does not ask for them again. A turn that
// is left out asks for each one still alive, as the left-out movement would have.
void KeepLevelSounds(LiveActor* a) {
    JAUSoundObject* so = a->mSoundObject;
    if (so == nullptr || !so->mIsAllocated) {
        return;
    }
    PORT_AUDIO_TOUCH();  // (GXV_AUDIO_CPU1: movement is outside the audio window anyway, as startLevelSound's own refresh is)
    const int n = so->getNumHandles();
    for (int i = 0; i < n; ++i) {
        JAISound* s = so->getHandle(i)->getSound();
        if (s != nullptr && s->hasLifeTime()) {
            s->updateLifeTime(1);
        }
    }
}

// The rate this turn (1: it moves now; 2 / 4: it may be left out) and whether it is off the picture.
int Decide(LiveActor* a, FamStats& fs, bool* offScreen) {
    *offScreen = false;
    if (sTurn.camTurn != gPortAlodTurn || !sTurn.open) {
        ++fs.why[kWhyGate];
        return 1;
    }
    // (mStaleAnim: listed by GXV_ANIM_LOD for the next picture's flush, whose calcAnim must not be taken for this turn's: it moves)
    if (a->mFlag.mIsDead || a->mFlag.mIsClipped || a->mCollisionParts != nullptr || a->mCameraCtrl != nullptr || a->mModelManager == nullptr ||
        a->mStaleAnim || !port_on_frame_thread()) {
        ++fs.why[kWhyBusy];
        return 1;
    }
    if (a->mSensorKeeper != nullptr) {
        HitSensorKeeper* k = a->mSensorKeeper;
        if (k->mTaking != nullptr || k->mTaken != nullptr) {
            ++fs.why[kWhyBusy];
            return 1;
        }
        for (s32 i = 0; i < k->mSensorInfosSize; ++i) {  // (crash33: the sensors added, not the table's capacity)
            if (k->mSensorInfos[i]->mSensor->mSensorCount != 0) {
                ++fs.why[kWhyContact];
                return 1;
            }
        }
    }
    const float model = ModelRadius(a);
    if (model < 0.0f) {
        ++fs.why[kWhyBusy];
        return 1;
    }
    const int maxRate = KnobMaxRate();
    const TVec3f& p = a->mPosition;
    const float px = p.x - sTurn.player.x, py = p.y - sTurn.player.y, pz = p.z - sTurn.player.z;
    const float plyD2 = px * px + py * py + pz * pz;
    const float gate = port::alod::PlayerGate(KnobPDist(), Reach(a) + model, sTurn.playerSpeed, maxRate);
    if (!(plyD2 > gate * gate)) {
        ++fs.why[kWhyNear];
        return 1;
    }
    const float cx = p.x - sTurn.cam.x, cy = p.y - sTurn.cam.y, cz = p.z - sTurn.cam.z;
    const float camD2 = cx * cx + cy * cy + cz * cz;
    float r = model * 1.5f + 150.0f;
    if (r < 300.0f) {
        r = 300.0f;
    }
    bool off = port::viewcone::Outside(sTurn.cone, p.x, p.y, p.z, r) && ShadowsOff(a, r);
    if (!off) {
        const float tiny = KnobTinyPx();
        const float td = KnobTinyDist();
        if (!(tiny > 0.0f) || !(camD2 > td * td) || !(port::alod::PixelRadius(model * 1.5f, sqrtf(camD2), sTurn.tanHalf, 544.0f) < tiny)) {
            ++fs.why[kWhyOnScreen];
            return 1;
        }
    }
    *offScreen = off;
    return port::alod::ChooseRate(off, camD2, plyD2, KnobFar4(), maxRate);
}

struct OwedAdapter {
    LiveActor* a;
    int owed() const { return a->mAlodOwed; }
    void decOwed() { --a->mAlodOwed; }
    void liteStep() { a->portMovementLite(); }
    void latchAsCalcAnim() {
        // (that turn's calcAnim cleared the latch, unless the actor computes no animation: LiveActor::calcAnim returns at once)
        if (!a->mFlag.mIsNoCalcAnim && a->mModelManager != nullptr && a->mModelManager->mXanimePlayer != nullptr) {
            a->mModelManager->mXanimePlayer->_88 = false;
        }
    }
};

void LogMaybe() {
    if (sLogTurn0 == 0) {
        sLogTurn0 = gPortAlodTurn;
        sLogT0 = port_host_time_ns();
        return;
    }
    const uint32_t turns = gPortAlodTurn - sLogTurn0;
    if (turns < 300) {
        return;
    }
    const int64_t now = port_host_time_ns();
    const double secs = (now - sLogT0) / 1e9;
    const double t = turns;
    const int mode = port_alod_mode();
    uint64_t dec = 0, lo = 0, h = 0, q = 0, rep = 0, no = 0, why[kWhyN] = {};
    for (int f = 1; f < kFam; ++f) {
        dec += sFam[f].decisions;
        lo += sFam[f].leftOut;
        h += sFam[f].half;
        q += sFam[f].quarter;
        rep += sFam[f].replayed;
        no += sFam[f].nerveOutside;
        for (int w = 0; w < kWhyN; ++w) {
            why[w] += sFam[f].why[w];
        }
    }
    port_log_summary("alod: %.1f s, %u turns, GXV_ALOD=%d: a turn: %.1f actors decide, %.1f %s (half %.1f, quarter %.1f), %.1f owed steps "
                     "replayed (%.2f us a step, sampled); moved because: closed gate %.1f (closed %.0f %% of turns: demo %llu, mirror %llu, "
                     "player %llu, camera %llu), busy %.1f, sensor hit %.1f, player near %.1f, on screen %.1f, its turn of the cycle %.1f; "
                     "nerve set from outside while behind %.2f",
                     secs, turns, mode, dec / t, lo / t, mode == 2 ? "would be left out" : "left out", h / t, q / t, rep / t,
                     sReplaySteps ? (sReplayNs / 1e3) / sReplaySteps : 0.0, why[kWhyGate] / t, 100.0 * sGateClosedTurns / t,
                     (unsigned long long)sGateWhy[0], (unsigned long long)sGateWhy[1], (unsigned long long)sGateWhy[2],
                     (unsigned long long)sGateWhy[3], why[kWhyBusy] / t, why[kWhyContact] / t, why[kWhyNear] / t, why[kWhyOnScreen] / t,
                     why[kWhyCycle] / t, no / t);
    for (int f = 1; f < kFam; ++f) {
        const FamStats& s = sFam[f];
        if (s.marked == 0 && s.decisions == 0) {
            continue;
        }
        const double moveUs = s.moveCalls ? (s.moveNs / 1e3) / s.moveCalls : 0.0;
        if (mode == 2) {
            // what mode 1 would save a turn: the left-out turns' movement and calcAnim, less their replay (estimated at the
            // measured lite share of a movement: the replay is the movement less doObjCol, the sensor update and the shadow request)
            port_log_summary("alod: %s: %u marked since start; a turn: %.1f decide, %.1f would be left out (%.1f half, %.1f quarter); their movement %.3f ms "
                             "(%.1f us each; replayed later) + calcAnim %.3f ms (saved) ; all its movements %.1f us each; nerve set from "
                             "outside %.2f",
                             kFamName[f], s.marked, s.decisions / t, s.leftOut / t, s.half / t, s.quarter / t, s.wouldMoveNs / 1e6 / t,
                             s.leftOut ? (s.wouldMoveNs / 1e3) / s.leftOut : 0.0, s.wouldCalcNs / 1e6 / t, moveUs, s.nerveOutside / t);
        } else {
            port_log_summary("alod: %s: %u marked since start; a turn: %.1f decide, %.1f left out (%.1f half, %.1f quarter), %.1f replayed; moved because: "
                             "gate %.1f, busy %.1f, hit %.1f, near %.1f, on screen %.1f, cycle %.1f; its movements %.1f us each; nerve set "
                             "from outside %.2f",
                             kFamName[f], s.marked, s.decisions / t, s.leftOut / t, s.half / t, s.quarter / t, s.replayed / t,
                             s.why[kWhyGate] / t, s.why[kWhyBusy] / t, s.why[kWhyContact] / t, s.why[kWhyNear] / t, s.why[kWhyOnScreen] / t,
                             s.why[kWhyCycle] / t, moveUs, s.nerveOutside / t);
        }
    }
    for (int f = 0; f < kFam; ++f) {
        const uint32_t m = sFam[f].marked;
        memset(&sFam[f], 0, sizeof sFam[f]);
        sFam[f].marked = m;
    }
    sGateClosedTurns = 0;
    memset(sGateWhy, 0, sizeof sGateWhy);
    sReplayNs = sReplaySteps = 0;
    sLogTurn0 = gPortAlodTurn;
    sLogT0 = now;
}

}  // namespace

// (fmt32: 0 in a lockstep session: which actor stands behind is decided by this console's camera, and the catch-up draws the shared random sequence in another order)
extern "C" int port_alod_mode(void) { return port::lockstep::On() ? 0 : PORT_STAGE_KNOB(int, AlodEnvMode()); }

// ===================================================================================================================================
// GXV_FARLOGIC (farlogic25): the far-logic level. See the top of this file and port/farlogic.h.
// ===================================================================================================================================
extern "C" float port_pb_scale(void);              // (popin25: LodCtrl.cpp: the zoom ratio, times GXV_PB_DEMO's widening in a flight; >= 1)
extern "C" int port_anim_lod_raw_category(void);   // (perf.cpp: the calc-anim list being run)

namespace {
namespace pf = port::farlogic;

int sFlLevel = -1;         // the level in force (-1: the environment's default is not read yet); written by port_farlogic_set from any thread
int sFlNeedMark = 0;       // 1: the actors made before the level was set are marked on the first turn the scene runs
int sFlLogLevel = -1;      // the level the log last named

struct FlStats {
    uint64_t decisions = 0, gate = 0, busy = 0, contact = 0, never = 0, nearv = 0, inState = 0, skipped = 0;
    uint64_t nv[pf::kNvN] = {};
    // dry mode: [1] level 1, [2] level 2; actor-turns in the far state, and (timed turns only) their movement / calcAnim ns
    uint64_t in[3][pf::kBuckets] = {};
    uint64_t mvNs[3][pf::kBuckets] = {}, caNs[3][pf::kBuckets] = {};
    // dry mode: never-class actors beyond 5000 / 2500 from the player and the camera (plain distances, no radius): turns counted / movement ns (timed turns)
    uint64_t nvCnt[pf::kNvN][2] = {}, nvMvNs[pf::kNvN][2] = {};
    // dry mode, level 3: the Enemy / Item calc-anim calls beyond 2500 from both that no level's logic rule holds, and the timed ones
    uint64_t poseCount = 0, poseTimedCount = 0, poseNs = 0;
    uint32_t timedTurns = 0;
};
FlStats sFl;
uint32_t sFlTurn0 = 0;
TVec3f sFlPrevCam{0.0f, 0.0f, 0.0f};  // (the camera of the last turn the gate was open, and that turn)
uint32_t sFlPrevCamTurn = 0;
uint32_t sFlMirrorOpen = 0;  // (cpu34g, GXV_FARLOGIC_MIRROR: turns the gate stayed open with a mirror camera, since the last "farlogic:" line)
int64_t sFlT0 = 0, sFlReadNs = 0;  // (the cost of one clock read, measured at each window's start; taken off every timed span)

int FlEnvLevel() {
    const char* e = getenv("GXV_FARLOGIC");
    return pf::ClampLevel(e != nullptr && *e != 0 ? atoi(e) : 0);
}
int FlArm() { return PORT_HOT_KNOB(int, AlodEnvInt("GXV_FARLOGIC_ARM", 0, 0, 1)); }

void FlMeasureRead() {
    const int64_t t0 = port_host_time_ns();
    int64_t x = 0;
    for (int i = 0; i < 64; ++i) {
        x += port_host_time_ns();
    }
    const int64_t t1 = port_host_time_ns();
    sFlReadNs = (t1 - t0) / 65 + ((x == 0) ? 1 : 0);
    if (sFlReadNs < 0) {
        sFlReadNs = 0;
    }
}
uint64_t FlNs(int64_t ns) {
    ns -= sFlReadNs;
    return ns > 0 ? static_cast< uint64_t >(ns) : 0;
}

// The actor's class from its parts, read once and then every 32 turns (phase from its address).
void FlReadClass(LiveActor* a) {
    pf::Facts f;
    f.hasModel = a->mModelManager != nullptr && MR::getJ3DModel(a) != nullptr;
    f.collision = a->mCollisionParts != nullptr;
    f.sensors = a->mSensorKeeper != nullptr && a->mSensorKeeper->mSensorCount > 0;
    f.stageSwitch = a->mStageSwitchCtrl != nullptr;
    f.rail = a->mRailRider != nullptr;
    f.camCtrl = a->mCameraCtrl != nullptr;
    f.pointer = a->mStarPointerTarget != nullptr;
    f.freeJoints = a->mLogicFreeJoints != 0;
    f.player = a == reinterpret_cast< LiveActor* >(MarioAccess::getPlayerActor());
    f.objective = a->mPortNoCut || a->mFarProt == 2 || (port::farprotect::On() && port::cpucuts::ObjectiveName(a->mName));  // (far49)
    f.family = a->mFlFam;
    pf::Why why = pf::kNvNone;
    const pf::Class c = pf::Classify(f, &why);
    a->mFlClass = static_cast< u8 >(static_cast< u8 >(c) | (static_cast< u8 >(why) << 2));
}

// Back to full rate: the far state is cleared (hysteresis restarts), the joints are computed on every turn again (LiveActor.cpp PbAnimSkips).
inline void FlFull(LiveActor* a) {
    a->mFlState &= static_cast< u8 >(~7u);
    if (a->mAlodRate > 1) {
        a->mAlodRate = 1;
    }
}

// The actor's movement; with `ns` (dry mode, a timed turn) the time it took, less one clock read.
inline void FlRun(LiveActor* a, uint64_t* ns) {
    if (ns == nullptr) {
        a->movement();
        return;
    }
    const int64_t t0 = port_host_time_ns();
    a->movement();
    *ns = FlNs(port_host_time_ns() - t0);
}

// far3 (Quality step 3l, GXV_Q30_FAR): the ambient actors beyond this distance (game units; 0 = off, perf.cpp sweeps it in) run at half pace as farlogic level 1 does, but from 100 m and only
// for pure decoration and the honey bee family (never star bits, coins, enemies, anything with a hit sensor and no listed family: Classify already keeps those at full rate).
void FlMovement(LiveActor* a) {
    FlStats& fl = sFl;
    const bool dry = port_farlogic_dry() != 0;
    const int envLevel = dry ? 0 : port_farlogic_level();
    const float qLim = (dry || envLevel != 0 || port_net_session_lockstep()) ? 0.0f : port_ql_logic_limit();   // (MP3: no far logic of any kind in a session)
    const int level = envLevel != 0 ? envLevel : (qLim > 0.0f ? 1 : 0);
    const uint32_t turn = gPortAlodTurn;
    ++fl.decisions;
    // 1. the global gate: no camera or scene yet, a demo, a mirror camera, the player dead or in a rush
    if (sTurn.camTurn != turn || !sTurn.open) {
        ++fl.gate;
        FlFull(a);
        a->movement();
        return;
    }
    const bool timed = dry && (turn & 3u) == 0;
    const TVec3f& p = a->mPosition;
    const float px = p.x - sTurn.player.x, py = p.y - sTurn.player.y, pz = p.z - sTurn.player.z;
    const float cx = p.x - sTurn.cam.x, cy = p.y - sTurn.cam.y, cz = p.z - sTurn.cam.z;
    const float p2 = px * px + py * py + pz * pz, c2 = cx * cx + cy * cy + cz * cz;
    // 2. the class (a cached byte)
    if (pf::ReadDue(a->mFlAge, pf::PhaseOf(a), a->mFlClass == 0xFF)) {
        FlReadClass(a);
    }
    const pf::Class cls = static_cast< pf::Class >(a->mFlClass & 3u);
    const int why = a->mFlClass >> 2;
    if (cls == pf::kNever) {
        ++fl.never;
        ++fl.nv[why < pf::kNvN ? why : 0];
        FlFull(a);
        if (dry) {
            // what the never-class actors cost beyond 5000 / 2500 (the list of what is left on the table)
            const int farK = (p2 > 5000.0f * 5000.0f && c2 > 5000.0f * 5000.0f) ? 2 : (p2 > 2500.0f * 2500.0f && c2 > 2500.0f * 2500.0f) ? 1 : 0;
            if (farK != 0) {
                uint64_t ns = 0;
                FlRun(a, timed ? &ns : nullptr);
                const int w = why < pf::kNvN ? why : 0;
                for (int k = 0; k < farK; ++k) {  // (k 0: beyond 2500, k 1: beyond 5000; beyond 5000 is beyond 2500 too)
                    ++fl.nvCnt[w][k];
                    fl.nvMvNs[w][k] += ns;
                }
                return;
            }
        }
        a->movement();
        return;
    }
    // 3. what keeps an actor at full rate this turn (as GXV_ALOD's gate): dead, clipped, collision, camera controller, no model, listed
    //    for the picture's flush, carried or carrying, a hit in any sensor this turn
    if (a->mFlag.mIsDead || a->mFlag.mIsClipped || a->mCollisionParts != nullptr || a->mCameraCtrl != nullptr || a->mModelManager == nullptr ||
        a->mStaleAnim || !port_on_frame_thread()) {
        ++fl.busy;
        FlFull(a);
        a->movement();
        return;
    }
    if (a->mSensorKeeper != nullptr) {
        HitSensorKeeper* k = a->mSensorKeeper;
        bool hit = k->mTaking != nullptr || k->mTaken != nullptr;
        for (s32 i = 0; i < k->mSensorInfosSize && !hit; ++i) {  // (crash33: the sensors added, not the table's capacity)
            hit = k->mSensorInfos[i]->mSensor->mSensorCount != 0;
        }
        if (hit) {
            ++fl.contact;  // (any hit puts it back to every turn at once, and the far state starts again from outside)
            FlFull(a);
            a->movement();
            return;
        }
    }
    const float model = ModelRadius(a);
    if (model < 0.0f) {
        ++fl.busy;
        FlFull(a);
        a->movement();
        return;
    }
    const float reach = cls == pf::kMob ? Reach(a) : 0.0f;
    const float zoom = port_pb_scale();
    const int bucket = a->mFlFam != 0 && a->mFlFam < pf::kFamN ? a->mFlFam : 8;
    // 4. the far state of the level (hysteresis: one bit per level)
    auto eval = [&](int lvl, u8 bit) -> bool {
        float base = pf::BaseLimit(cls, pf::RuleFor(lvl));
        if (qLim > 0.0f) {   // (far3: the Quality step's own limit replaces level 1's, for ambient actors that are not collectibles)
            base = (cls == pf::kAmbient && a->mFlFam != pf::kFamStarPiece && a->mFlFam != pf::kFamCoin && qLim >= pf::kNearFloor) ? qLim : 0.0f;
        }
        const bool was = (a->mFlState & bit) != 0;
        const bool now = base > 0.0f && pf::FarState(was, p2, c2, pf::Effective(base, model, reach, sTurn.fastSpeed, zoom));
        a->mFlState = static_cast< u8 >(now ? (a->mFlState | bit) : (a->mFlState & ~bit));
        return now;
    };
    if (dry) {
        // 5a. dry: nothing is left out. Counted every turn, timed (movement here, calcAnim in LiveActor::calcAnim) on one turn in four.
        const bool l1 = eval(1, 2), l2 = eval(2, 4);
        if (l1) ++fl.in[1][bucket];
        if (l2) ++fl.in[2][bucket];
        if (!l1 && !l2) {
            ++fl.nearv;
            a->movement();
            return;
        }
        uint64_t ns = 0;
        FlRun(a, timed ? &ns : nullptr);
        if (timed) {
            if (l1) fl.mvNs[1][bucket] += ns;
            if (l2) fl.mvNs[2][bucket] += ns;
            a->mAlodSkipTurn = turn;  // (LiveActor::calcAnim times this turn's calcAnim and makes it: port_alod_census_calc)
        }
        return;
    }
    // 5b. live
    if (level == 0 || !eval(level, 1)) {
        FlFull(a);
        ++fl.nearv;
        a->movement();
        return;
    }
    ++fl.inState;
    a->mAlodRate = 2;  // (its joints are computed on each of its movements: PbAnimSkips leaves the actor alone)
    if (pf::SkipThisTurn(turn, pf::PhaseOf(a))) {
        // this turn is left out, NOT replayed: no movement, no calcAnim (LiveActor::calcAnim), the view pass runs. The actor's own time
        // runs at half speed while it is far.
        ++fl.skipped;
        a->mAlodSkipTurn = turn;
        KeepLevelSounds(a);
        return;
    }
    a->movement();
}

// The calcAnim time of a turn the dry mode times (LiveActor::calcAnim): into the rows of the levels the actor is in the far state of.
void FlCalcCensus(LiveActor* a, int64_t ns) {
    const uint64_t t = FlNs(ns);
    const int bucket = a->mFlFam != 0 && a->mFlFam < pf::kFamN ? a->mFlFam : 8;
    if ((a->mFlState & 2) != 0) {
        sFl.caNs[1][bucket] += t;
    }
    if ((a->mFlState & 4) != 0) {
        sFl.caNs[2][bucket] += t;
    }
}

void FlMarkAll() {
    AllLiveActorGroup* g = MR::getAllLiveActorGroup();
    if (g == nullptr) {
        return;
    }
    const s32 n = g->getObjNum();
    for (s32 i = 0; i < n; ++i) {
        LiveActor* a = g->getActor(i);
        if (a != nullptr) {
            static_cast< NameObj* >(a)->mFlag |= PORT_NAMEOBJ_FLAG_ALOD;
        }
    }
}

// MP3: said once per session, from the turn start (also when the far logic is otherwise not armed): the far logic is held at 0.
void FlMpGateLog() {
    static bool sMpGateLogged = false;
    if (!port_net_session_lockstep()) {   // (held: a reconnect keeps the session, the lines are not said again)
        sMpGateLogged = false;
        port_mpgate_idle();   // (MP3 lodgate: re-arms the other held features' lines, perf.cpp)
    } else if (!sMpGateLogged) {
        sMpGateLogged = true;
        port_log_summary("farlogic: held at level 0 while a multiplayer session is active (far actors would run at half pace by the local camera's distance: a desync); stored level %d, resumes after the session",
                         __atomic_load_n(&sFlLevel, __ATOMIC_RELAXED));
    }
}

void FlLogMaybe() {
    const int level = port_farlogic_level();
    const int dry = port_farlogic_dry();
    if (level != sFlLogLevel) {
        port_log_summary("farlogic: level %d now (was %d), dry %d, armed %d; GXV_ALOD %d", level, sFlLogLevel, dry, FlArm(), port_alod_mode());
        sFlLogLevel = level;
    }
    if (sFlTurn0 == 0) {
        sFlTurn0 = gPortAlodTurn;
        sFlT0 = port_host_time_ns();
        FlMeasureRead();
        return;
    }
    const uint32_t turns = gPortAlodTurn - sFlTurn0;
    if (turns < 300) {
        return;
    }
    const int64_t now = port_host_time_ns();
    const double secs = (now - sFlT0) / 1e9, t = turns;
    const FlStats& f = sFl;
    static const char* const kBucketName[pf::kBuckets] = {"sensor-unlisted", "starpiece", "coin", "kotebug", "karipon", "packunpetit", "honeybee", "rock", "decoration"};
    if (dry) {
        const double tt = f.timedTurns != 0 ? static_cast< double >(f.timedTurns) : 1.0;
        port_log_summary("farlogic: DRY (nothing changed) %.1f s, %u turns (%u timed): a turn: %.1f marked actors decide; full rate whatever the level: gate closed %.1f, "
                         "never-class %.1f, busy %.1f, sensor hit %.1f, not far %.1f; clock read %.2f us",
                         secs, turns, f.timedTurns, f.decisions / t, f.gate / t, f.never / t, f.busy / t, f.contact / t, f.nearv / t, sFlReadNs / 1e3);
        for (int lv = 1; lv <= 2; ++lv) {
            uint64_t n = 0, mv = 0, ca = 0;
            for (int b = 0; b < pf::kBuckets; ++b) {
                n += f.in[lv][b];
                mv += f.mvNs[lv][b];
                ca += f.caNs[lv][b];
            }
            port_log_summary("farlogic: DRY level %d (%s): a turn: %.1f actors in the far state; their movement %.3f ms + calcAnim %.3f ms; half rate would save %.3f ms",
                             lv, lv == 1 ? "ambient beyond 5000" : "ambient and mob beyond 2500", n / t, mv / 1e6 / tt, ca / 1e6 / tt,
                             pf::SavedMsPerTurn(mv, ca, f.timedTurns));
            for (int b = 0; b < pf::kBuckets; ++b) {
                if (f.in[lv][b] == 0) {
                    continue;
                }
                port_log_summary("farlogic: DRY level %d: %s: %.1f a turn, movement %.3f + calcAnim %.3f ms, would save %.3f ms", lv, kBucketName[b], f.in[lv][b] / t,
                                 f.mvNs[lv][b] / 1e6 / tt, f.caNs[lv][b] / 1e6 / tt, pf::SavedMsPerTurn(f.mvNs[lv][b], f.caNs[lv][b], f.timedTurns));
            }
        }
        const double perCall = f.poseTimedCount != 0 ? static_cast< double >(f.poseNs) / f.poseTimedCount / 1e3 : 0.0;
        port_log_summary("farlogic: DRY level 3 adds: Enemy / Item calcAnims beyond 2500 from player and camera, not in a far state: %.1f a turn at %.1f us each; "
                         "posed every other turn would save %.3f ms",
                         f.poseCount / t, perCall, 0.5 * perCall * (f.poseCount / t) / 1e3);
        for (int w = 1; w < pf::kNvN; ++w) {
            port_log_summary("farlogic: DRY full rate at every level: %s: %.1f actors a turn; beyond 2500: %.1f actors %.3f ms movement; beyond 5000: %.1f actors %.3f ms",
                             pf::WhyName(w), f.nv[w] / t, f.nvCnt[w][0] / t, f.nvMvNs[w][0] / 1e6 / tt, f.nvCnt[w][1] / t, f.nvMvNs[w][1] / 1e6 / tt);
        }
    } else {
        port_log_summary("farlogic: level %d, %.1f s, %u turns: a turn: %.1f marked actors decide: %.1f in the far state (%.1f left out, %.1f ran); full rate: gate closed %.1f, "
                         "never-class %.1f, busy %.1f, sensor hit %.1f, not far %.1f; GXV_FARLOGIC_MIRROR kept the gate open with a mirror camera in %u turns",
                         level, secs, turns, f.decisions / t, f.inState / t, f.skipped / t, (f.inState - f.skipped) / t, f.gate / t, f.never / t, f.busy / t,
                         f.contact / t, f.nearv / t, static_cast< unsigned >(sFlMirrorOpen));
    }
    sFlMirrorOpen = 0;
    sFl = FlStats();
    sFlTurn0 = gPortAlodTurn;
    sFlT0 = now;
    FlMeasureRead();
}
}  // namespace

extern "C" int port_farlogic_level(void) {
    if (port::lockstep::On()) {
        return 0;  // (fmt32: a far actor at half pace is a time dilation chosen by THIS console's camera and player: two consoles would simulate differently. The level set by the
                   // environment, GXV_CPULOD >= 2 or a stage key is kept and applies again after the session; an actor in the far state leaves it on its next movement, FlFull.)
    }
    int v = __atomic_load_n(&sFlLevel, __ATOMIC_RELAXED);
    if (__builtin_expect(v < 0, 0)) {
        int expected = -1;
        __atomic_compare_exchange_n(&sFlLevel, &expected, FlEnvLevel(), false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        v = __atomic_load_n(&sFlLevel, __ATOMIC_RELAXED);
    }
    return pf::LevelInSession(v, port_net_session_lockstep());   // MP3: 0 while a multiplayer session is active (desync gate); one bool read otherwise
}
extern "C" void port_farlogic_set(int level) {
    (void)port_farlogic_level();  // (the environment's default is read first, so it cannot overwrite this)
    __atomic_store_n(&sFlLevel, pf::ClampLevel(level), __ATOMIC_RELAXED);
    __atomic_store_n(&sFlNeedMark, 1, __ATOMIC_RELAXED);
}
extern "C" int port_farlogic_dry(void) { return PORT_HOT_KNOB(int, AlodEnvInt("GXV_FARLOGIC_DRY", 0, 0, 1)); }
// 1: LiveActors are marked for the entry above (when made, and, once after port_farlogic_set, the ones that exist): the level is above 0, the dry
// mode is on, or GXV_FARLOGIC_ARM=1 (a runtime change of the level then reaches actors made before it; the cost is a call a listed actor).
extern "C" int port_farlogic_armed(void) {
    const bool ql = port_ql_logic_limit() > 0.0f;   // (far3: the Quality step 3l is sweeping in or on: the actors made before it are marked once, as after port_farlogic_set)
    static bool sQlSeen = false;
    if (ql && !sQlSeen) {
        __atomic_store_n(&sFlNeedMark, 1, __ATOMIC_RELAXED);
    }
    sQlSeen = ql;
    return ql || port_farlogic_level() > 0 || port_farlogic_dry() != 0 || FlArm() != 0 || port_idle25_mode() != 0;
}
// Level 3 (LiveActor.cpp PbAnimSkips): characters beyond this distance (squared) from the camera and the player are posed every other turn.
extern "C" int port_farlogic_pose_on(void) { return port_farlogic_dry() == 0 && pf::RuleFor(port_farlogic_level()).pose ? 1 : 0; }
extern "C" float port_farlogic_pose_dist2(void) {
    return PORT_HOT_KNOB(float, [] { const float d = AlodEnvFloat("GXV_FARLOGIC_POSE_DIST", 2500.0f, 2000.0f, 100000.0f); return d * d; }());
}
extern "C" int port_farlogic_pose_category(void) { return pf::PoseCategory(port_anim_lod_raw_category()) ? 1 : 0; }
// Dry mode, level 3 (LiveActor::calcAnim): is this Enemy / Item calcAnim beyond 2500 from both, and not in a far state? 1 counted, 2 counted and timed.
extern "C" int port_farlogic_pose_candidate(const LiveActor* a) {
    if (!sTurn.open || sTurn.camTurn != gPortAlodTurn || !pf::PoseCategory(port_anim_lod_raw_category()) || a->mCollisionParts != nullptr || (a->mFlState & 4) != 0) {
        return 0;
    }
    const float d = sqrtf(port_farlogic_pose_dist2()) * port_pb_scale();
    const TVec3f& p = a->mPosition;
    const float px = p.x - sTurn.player.x, py = p.y - sTurn.player.y, pz = p.z - sTurn.player.z, cx = p.x - sTurn.cam.x, cy = p.y - sTurn.cam.y, cz = p.z - sTurn.cam.z;
    if (!(px * px + py * py + pz * pz > d * d) || !(cx * cx + cy * cy + cz * cz > d * d)) {
        return 0;
    }
    ++sFl.poseCount;
    return (gPortAlodTurn & 3u) == 0 ? 2 : 1;
}
extern "C" void port_farlogic_pose_census(int64_t ns) {
    sFl.poseNs += FlNs(ns);
    ++sFl.poseTimedCount;
}

extern "C" void port_alod_turn_begin(void) {
    ++gPortAlodTurn;
    if (gPortAlodTurn == 0) {
        gPortAlodTurn = 1;  // (0 means "no turn" in LiveActor::mAlodSkipTurn)
    }
    if (port_alod_mode() != 0) {
        LogMaybe();
    }
    FlMpGateLog();  // MP3: one bool read
    if (port_farlogic_armed() != 0) {
        FlLogMaybe();
    }
    if (port_idle25_mode() != 0) {
        IdLogMaybe();
    }
}

extern "C" void port_alod_camera_ready(void) {
    const bool flOn = port_farlogic_armed() != 0;  // (GXV_FARLOGIC: the same turn gate)
    if (port_alod_mode() == 0 && !flOn) {
        return;
    }
    Turn& t = sTurn;
    t.camTurn = gPortAlodTurn;
    t.open = false;
    t.gateWhy = 0;
    if (port_net_session_lockstep()) {   // MP3 lodgate: the gate stays closed in a session (every actor moves every turn, owed steps are paid at once): GXV_ALOD leaves turns out by the LOCAL view and distance
        port_mpgate_note(port::mpgate::kAlod);
        t.gateWhy |= 16;
        ++sGateClosedTurns;
        return;
    }
    // (2.3 console fix: this runs for every scene's movement list, also while the stationed archives load and on the title, where the
    // demo director, the player and the camera do not exist yet: MR::isDemoActive() dereferenced a missing DemoDirector there. The gate
    // stays closed until the game scene's objects exist.)
    if (!MR::isExistSceneObj(SceneObj_DemoDirector) || !MR::isExistSceneObj(SceneObj_MarioHolder)) {
        t.gateWhy |= 4;
        ++sGateClosedTurns;
        ++sGateWhy[2];
        return;
    }
    if (flOn && __atomic_exchange_n(&sFlNeedMark, 0, __ATOMIC_RELAXED) != 0) {
        FlMarkAll();  // (the level was set after some actors were made: the scene's objects exist, so the list is complete)
    }
    if (MR::isDemoActive()) {
        t.gateWhy |= 1;
    }
    if (MR::isExistMirrorCamera()) {
        // cpu34g (14 Nov 2026), GXV_FARLOGIC_MIRROR=1 (default 0): the mirror closes the gate for GXV_ALOD, whose rule is "off the picture" (a mirror shows what is off it).
        // GXV_FARLOGIC's rule is distance alone (beyond 2500 / 5000 from the player AND the camera, on the picture or not), and a mirror image is never nearer than the
        // thing itself, so with GXV_ALOD off the gate may stay open: Beach Bowl's gate was closed in 100 % of turns on the console (b10/2892c: "gate closed 74.3" of 74.3).
        static const bool sFlMirror = [] { const char* e = getenv("GXV_FARLOGIC_MIRROR"); return e != nullptr && *e == '1'; }();
        if (sFlMirror && flOn && port_alod_mode() == 0) {
            ++sFlMirrorOpen;
        } else {
            t.gateWhy |= 2;  // (a mirror shows actors that are off the picture)
        }
    }
    const TVec3f* player = port_safe_player_pos();
    if (player == nullptr || MR::isPlayerDead() || MR::isPlayerInRush()) {
        t.gateWhy |= 4;
    }
    // (zoomclip23, GXV_ZOOM_LOGIC_VIEW: "off the picture" is judged in the original game's view, never the narrower one of the handheld
    // zoom: an actor the original would show keeps its full rate. The tiny-size test below measures the size in the picture shown.)
    const float fovy = MR::getFovy();
    port::viewcone::Make(&t.cone, j3dSys.mViewMtx, MR::getFovyForLogic(), MR::getAspect());  // (MR::loadViewMtx: this turn's camera, after its list)
    if (!t.cone.ok) {
        t.gateWhy |= 8;
    }
    if (t.gateWhy != 0) {
        ++sGateClosedTurns;
        for (int b = 0; b < 4; ++b) {
            if (t.gateWhy & (1u << b)) {
                ++sGateWhy[b];
            }
        }
        return;
    }
    t.tanHalf = tanf(fovy * (3.14159265f / 360.0f));
    t.player = *player;
    const TVec3f* v = MR::getPlayerVelocity();
    t.playerSpeed = v != nullptr ? sqrtf(v->x * v->x + v->y * v->y + v->z * v->z) : 1000.0f;
    if (!(t.playerSpeed < 1e6f)) {
        t.playerSpeed = 1e6f;
    }
    t.cam = MR::getCamPos();
    t.fastSpeed = t.playerSpeed;
    if (flOn) {
        // (a camera that flies or cuts widens every far-state limit by 4 turns of its travel: nothing is left out toward which it is moving)
        const float dx = t.cam.x - sFlPrevCam.x, dy = t.cam.y - sFlPrevCam.y, dz = t.cam.z - sFlPrevCam.z;
        const float cs = sFlPrevCamTurn + 1 == gPortAlodTurn ? sqrtf(dx * dx + dy * dy + dz * dz) : 0.0f;
        sFlPrevCam = t.cam;
        sFlPrevCamTurn = gPortAlodTurn;
        if (cs > t.fastSpeed) {
            t.fastSpeed = cs;
        }
    }
    t.open = true;
    if (flOn && (gPortAlodTurn & 3u) == 0 && port_farlogic_dry() != 0) {
        ++sFl.timedTurns;  // (GXV_FARLOGIC_DRY: a turn whose movement and calcAnim are timed)
    }
}

// (alodfix23: the model the picture draws is ModelManager::getJ3DModel(), the XanimePlayer's for an animated model; mModel is null for
// those (every marked family: the console dumps show it for StarPiece), so the stamp was never set or taken back for them)
static J3DModel* AlodModel(LiveActor* a) {
    return a->mModelManager != nullptr ? a->mModelManager->getJ3DModel() : nullptr;
}

void PortAlodUncull(LiveActor* a) {
    if (J3DModel* m = AlodModel(a)) {
        m->mPortCullPicture = 0;
    }
    a->mAlodCulled = 0;
}

void port_alod_census_calc(LiveActor* a, int64_t ns) {
    if (a->mAlodFamily == 0 || port_alod_mode() != 2) {
        FlCalcCensus(a, ns);  // (GXV_FARLOGIC_DRY: an actor the far-logic entry timed)
        return;
    }
    const int f = a->mAlodFamily < kFam ? a->mAlodFamily : 0;
    if (ns > 0) {
        sFam[f].wouldCalcNs += static_cast< uint64_t >(ns);
    }
}

// ======================================================================================================================================================
// GXV_IDLE25 (idle25, 12 Nov 2026; rules and proofs: port/idle25.h, REPORT.txt): exact idle skipping.
//
// An actor of a registered family whose movement() is provably a known micro-step this turn does not run movement(): it runs the parts of it that are not
// provable no-ops, and the micro-step in place of Spine::update + control + the binder. Every turn, from the live fields: no owed time, no wake-up lag.
//   GXV_IDLE25=0 (default) off.  1 on.  2 VERIFY: movement() runs as always; when the predicate says idle, the actor's state vector before it is advanced by
//   the micro-step (the pure function the fast path applies), and after the real movement the two vectors are compared field by field (mismatches counted
//   per family and logged: the count must be 0). Nothing else changes in mode 2.
// movement() order (LiveActor.cpp): ModelManager::update; gravity; doObjCol; Spine::update (nerve, step + 1); control(); updateBinder; effects; camera ctrl;
// light ctrl; sensor positions; sound; shadow request. In the idle turn:
//   model update      skipped when every animation player is at its fixpoint (ModelQuiet), else run
//   gravity           the idle predicate needs mIsCalcGravity off
//   doObjCol          the idle predicate needs no hit on any sensor
//   nerve + control   the family's micro-step (port/idle25.h; the nerve is the idle nerve, not first, no change pending; its wake conditions are NOT met)
//   updateBinder      pos += vel, Binder::clear() (the predicate needs the binder null or off); this is the real updateBinder in that case
//   effects           skipped when the keeper has no sync-bck / attribute effect (what EffectKeeper::update looks at), else run
//   camera/light ctrl run when present
//   sensor positions  skipped for the star bit when both sensors already sit bit-exactly where an update would put them (offset 0, no joint), else run
//   sound, shadow request   run as in movement()
namespace {
namespace id = port::idle25;

int IdMode() {
    if (port::lockstep::On()) return 0;   // (fmt32: GXV_IDLE25 is off in a lockstep session; one load of a word otherwise)
    const int m = PORT_HOT_KNOB(int, AlodEnvInt("GXV_IDLE25", 0, 0, 2));
    if (m != 0 && port_net_session_lockstep()) {   // MP3 lodgate: off in a session: the idle predicate knows the local player and the local star pointer, not MpCoStar::sweeps or a teammate's bubble shot
        port_mpgate_note(port::mpgate::kIdle);
        return port::mpgate::IdleMode(m, true);
    }
    return m;
}

id::FamCensus sIdCen[id::kFamN];
uint32_t sIdLogTurn0 = 0;
int64_t sIdLogT0 = 0;
uint32_t sIdSample = 0;
uint32_t sIdBadLogged = 0;

struct IdCtx {
    bool active = false;  // star bit: tryGotJudge's reach test (player within 15000 and flag _1)
};

inline uint32_t FBits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
inline float BitsF(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// ---- the state vector (VERIFY) -----------------------------------------------------------------------------------------------------------------
enum {
    V_POS = 0, V_ROT = 3, V_VEL = 6, V_GRAV = 9, V_STEP = 12, V_CURR = 13, V_NEXT = 14, V_FLA = 15, V_FLB = 16, V_C8 = 17, V_DELAY = 18, V_SBF = 19,
    V_PETSTEP = 20, V_BIND = 21, V_SPOS = 22 /* 4 sensors x 3 */, V_SCNT = 34 /* 4 */, V_ANM = 38 /* 5 players x 3 (frame, rate, state) */, V_XAN = 53 /* 4 */,
    V_N = 57
};
struct IdVec {
    uint32_t w[V_N];
};
const char* VName(int i) {
    static const char* const axis[3] = {"x", "y", "z"};
    static char buf[8][24];
    static int k = 0;
    char* b = buf[k++ & 7];
    if (i < 12) {
        static const char* const grp[4] = {"pos", "rot", "vel", "grav"};
        snprintf(b, 24, "%s.%s", grp[i / 3], axis[i % 3]);
    } else if (i >= V_SPOS && i < V_SCNT) {
        snprintf(b, 24, "sensor%d.pos.%s", (i - V_SPOS) / 3, axis[(i - V_SPOS) % 3]);
    } else if (i >= V_SCNT && i < V_ANM) {
        snprintf(b, 24, "sensor%d.hits", i - V_SCNT);
    } else if (i >= V_ANM && i < V_XAN) {
        static const char* const pl[5] = {"btk", "brk", "btp", "bpk", "bva"};
        static const char* const fl[3] = {"frame", "rate", "state"};
        snprintf(b, 24, "%s.%s", pl[(i - V_ANM) / 3], fl[(i - V_ANM) % 3]);
    } else if (i >= V_XAN) {
        static const char* const xl[4] = {"xanime.latch|78|state", "xanime._08", "xanime.frame", "xanime._14"};
        snprintf(b, 24, "%s", xl[i - V_XAN]);
    } else {
        switch (i) {
        case V_STEP: return "nerve.step";
        case V_CURR: return "nerve.curr";
        case V_NEXT: return "nerve.next";
        case V_FLA: return "flags.dead|stopAnim|noCalcAnim|noCalcView";
        case V_FLB: return "flags.hidden|noBind|calcGravity|clipped|invalidClip";
        case V_C8: return "starpiece._C8";
        case V_DELAY: return "starpiece.gettableDelay";
        case V_SBF: return "starpiece.flags";
        case V_PETSTEP: return "petit.scaleStep";
        case V_BIND: return "binder.planes";
        default: snprintf(b, 24, "w%d", i);
        }
    }
    return b;
}
void PutF(IdVec& v, int i, float f) { v.w[i] = FBits(f); }
void PutVec3(IdVec& v, int i, const TVec3f& p) {
    PutF(v, i, p.x);
    PutF(v, i + 1, p.y);
    PutF(v, i + 2, p.z);
}
void PutAnm(IdVec& v, int i, const AnmPlayerBase* p) {
    if (p == nullptr || p->mAnmRes == nullptr) {
        return;
    }
    PutF(v, i, p->mFrameCtrl.getFrame());
    PutF(v, i + 1, p->mFrameCtrl.getRate());
    v.w[i + 2] = p->mFrameCtrl.getState();
}
void Capture(LiveActor* a, IdVec& v) {
    memset(&v, 0, sizeof v);
    PutVec3(v, V_POS, a->mPosition);
    PutVec3(v, V_ROT, a->mRotation);
    PutVec3(v, V_VEL, a->mVelocity);
    PutVec3(v, V_GRAV, a->mGravity);
    if (a->mSpine != nullptr) {
        v.w[V_STEP] = static_cast< uint32_t >(a->mSpine->mStep);
        v.w[V_CURR] = static_cast< uint32_t >(reinterpret_cast< uintptr_t >(a->mSpine->mCurrNerve));
        v.w[V_NEXT] = static_cast< uint32_t >(reinterpret_cast< uintptr_t >(a->mSpine->mNextNerve));
    }
    const LiveActorFlag& f = a->mFlag;
    v.w[V_FLA] = f.mIsDead | (f.mIsStoppedAnim << 1) | (f.mIsNoCalcAnim << 2) | (f.mIsNoCalcView << 3);
    v.w[V_FLB] = f.mIsHiddenModel | (f.mIsNoBind << 1) | (f.mIsCalcGravity << 2) | (f.mIsClipped << 3) | (f.mIsInvalidClipping << 4);
    if (a->mFlFam == id::kFamStarPiece) {
        const StarPiece* s = static_cast< const StarPiece* >(a);
        v.w[V_C8] = static_cast< uint32_t >(s->_C8);
        v.w[V_DELAY] = static_cast< uint32_t >(s->mGettableDelayCounter);
        uint8_t fb;
        memcpy(&fb, &s->mFlags, 1);
        v.w[V_SBF] = fb;
    } else if (a->mFlFam == id::kFamPetit) {
        const PackunPetit* p = static_cast< const PackunPetit* >(a);
        if (p->mScaleController != nullptr && p->mScaleController->mSpine != nullptr) {
            v.w[V_PETSTEP] = static_cast< uint32_t >(p->mScaleController->mSpine->mStep);
        }
    }
    if (a->mBinder != nullptr) {
        v.w[V_BIND] = a->mBinder->mPlaneNum;
    }
    if (a->mSensorKeeper != nullptr) {
        for (s32 i = 0; i < a->mSensorKeeper->mSensorInfosSize && i < 4; ++i) {
            const HitSensor* hs = a->mSensorKeeper->mSensorInfos[i]->mSensor;
            PutVec3(v, V_SPOS + 3 * i, hs->mPosition);
            v.w[V_SCNT + i] = hs->mSensorCount;
        }
    }
    if (a->mModelManager != nullptr) {
        const ModelManager* m = a->mModelManager;
        PutAnm(v, V_ANM + 0, m->mBtkPlayer);
        PutAnm(v, V_ANM + 3, m->mBrkPlayer);
        PutAnm(v, V_ANM + 6, m->mBtpPlayer);
        PutAnm(v, V_ANM + 9, m->mBpkPlayer);
        PutAnm(v, V_ANM + 12, m->mBvaPlayer);
        const XanimePlayer* x = m->mXanimePlayer;
        if (x != nullptr && x->_20 != nullptr) {
            v.w[V_XAN] = x->_88 | (static_cast< uint32_t >(x->_78) << 1) | (static_cast< uint32_t >(x->_20->getState()) << 24);
            PutF(v, V_XAN + 1, x->_08);
            PutF(v, V_XAN + 2, x->_20->getFrame());
            v.w[V_XAN + 3] = static_cast< uint32_t >(x->_20->_14);
        }
    }
}
// The generic part of the micro-step on a captured vector: the real updateBinder's `pos += vel`, and the nerve step.
void PredictCommon(IdVec& v) {
    for (int k = 0; k < 3; ++k) {
        v.w[V_POS + k] = FBits(BitsF(v.w[V_POS + k]) + BitsF(v.w[V_VEL + k]));
    }
    v.w[V_STEP] = static_cast< uint32_t >(static_cast< int32_t >(v.w[V_STEP]) + 1);
}

// ---- the quiet proofs ----------------------------------------------------------------------------------------------------------------------
bool AnmQuiet(const AnmPlayerBase* p) {
    if (p == nullptr || p->mAnmRes == nullptr) {
        return true;
    }
    const J3DFrameCtrl& c = p->mFrameCtrl;
    return id::FrameCtrlQuiet(c.getFrame(), c.getRate(), c.getState(), c.getStart(), c.getEnd());
}
// XanimePlayer::updateBeforeMovement + updateAfterMovement leave every field as it is.
bool XanimeQuiet(const XanimePlayer* x) {
    if (x == nullptr) {
        return true;
    }
    if (x->_68 != nullptr) {
        return false;  // prepareAnimation would start an animation
    }
    if (!x->_7C) {
        return true;  // both functions return before touching anything
    }
    const XanimeFrameCtrl* c = x->_20;
    if (c == nullptr || x->_78 != 0 || x->mCurrentAnimation == nullptr) {
        return false;
    }
    if ((c->getState() & 1) == 1 && x->_7E && (c->getAttribute() == 0 || c->getAttribute() == 3)) {
        return false;  // runDefaultAnimation
    }
    if ((c->getState() & 1) == 0 && !x->_88) {
        return false;  // the frame would advance
    }
    if (c->_14 != 0 || x->_08 != 1.0f || x->mCore == nullptr || x->mCore->_1C != x->_08 || x->mCore->_20 != x->_0C) {
        return false;  // the interpolation is still running
    }
    for (u32 i = 0; i < x->mCurrentAnimation->mBckTableVariant; ++i) {
        if (x->mCore->mTrackList[i].mWeight != x->mWeights[i]) {
            return false;
        }
    }
    return true;
}
bool ModelQuiet(const ModelManager* m) {
    return XanimeQuiet(m->mXanimePlayer) && AnmQuiet(m->mBtkPlayer) && AnmQuiet(m->mBrkPlayer) && AnmQuiet(m->mBtpPlayer) && AnmQuiet(m->mBpkPlayer) &&
           AnmQuiet(m->mBvaPlayer);
}
inline bool EffectsQuiet(const EffectKeeper* e) { return e == nullptr || (e->_20 == nullptr && !e->_30); }
inline bool BitsEqNoNegZero(float a, float b) { return FBits(a) == FBits(b) && FBits(a) != 0x80000000u; }
// The star bit's two sensors (offset 0, no joint): an update writes the host position.
bool StarSensorsAtHost(const LiveActor* a) {
    const HitSensorKeeper* k = a->mSensorKeeper;
    if (k == nullptr) {
        return true;
    }
    for (s32 i = 0; i < k->mSensorInfosSize; ++i) {
        const TVec3f& p = k->mSensorInfos[i]->mSensor->mPosition;
        if (!BitsEqNoNegZero(p.x, a->mPosition.x) || !BitsEqNoNegZero(p.y, a->mPosition.y) || !BitsEqNoNegZero(p.z, a->mPosition.z)) {
            return false;
        }
    }
    return true;
}

// ---- the registry: a family opts in with its idle predicate, its micro-step and the same micro-step on the state vector ------------------------------
struct IdFamDef {
    uint8_t fam;
    const char* name;
    id::Why (*idle)(LiveActor*, IdCtx&);
    void (*step)(LiveActor*, const IdCtx&);        // the nerve + control part of movement() for this turn, as the micro-step
    void (*predict)(IdVec&, const IdCtx&);         // the same, on a captured vector (VERIFY's expectation)
    bool starSensors;                              // sensor positions may be proved unchanged (StarSensorsAtHost)
};

// 1 StarPiece, nerve Floating: exeFloating (rotation, tryGotJudge) + control (the countdown).
id::Why SbIdle(LiveActor* a, IdCtx& c) {
    const int nerve = port_idle25_starpiece_nerve(a);
    if (nerve != 1) {
        return nerve == 2 ? id::kWhyRail : id::kWhyNerve;
    }
    StarPiece* s = static_cast< StarPiece* >(a);
    if (s->mFlags._2) {
        return id::kWhyFlag;  // exeFloating checks the map for sand every turn
    }
    const bool active = !(225000000.0f < (*MR::getPlayerCenterPos() - a->mPosition).squared() || !s->mFlags._1);
    const id::SbState st{a->mRotation.y, a->mSpine->mStep, s->_C8, s->mGettableDelayCounter};
    if (id::SbEventNext(st, active)) {
        return id::kWhyEvent;
    }
    if (id::SbPointerAsked(st, active) && (MR::portStarPointerReaches(a, 0) || MR::portStarPointerReaches(a, 1) || MR::portStarPointerAssistMayAct())) {
        return id::kWhyPointer;
    }
    c.active = active;
    return id::kIdle;
}
void SbStep(LiveActor* a, const IdCtx& c) {
    StarPiece* s = static_cast< StarPiece* >(a);
    a->mRotation.y += id::kSbTurn;
    MR::repeatDegree(&a->mRotation.y);
    s->_C8 = id::SbNextC8(s->_C8, c.active);
    a->mSpine->mStep += 1;
    if (s->mGettableDelayCounter >= 0) {
        s->mGettableDelayCounter -= 1;
    }
}
void SbPredict(IdVec& v, const IdCtx& c) {
    id::SbState st{BitsF(v.w[V_ROT + 1]), 0, static_cast< int32_t >(v.w[V_C8]), static_cast< int32_t >(v.w[V_DELAY])};
    id::SbAdvance(st, c.active);
    PutF(v, V_ROT + 1, st.rotY);
    v.w[V_C8] = static_cast< uint32_t >(st.c8);
    v.w[V_DELAY] = static_cast< uint32_t >(st.delay);
}

// 2 Coin, nerves Fix / FixHide / NonActive: nothing but the step counter.
id::Why CoinIdle(LiveActor* a, IdCtx&) {
    const int n = port_idle25_coin_nerve(a);
    if (n == 0) {
        return id::kWhyNerve;
    }
    if (n != 2 && static_cast< Coin* >(a)->_B9) {
        return id::kWhyFlag;  // exeFix / exeNonActive re-set the clipping range every step
    }
    return id::kIdle;
}
void CoinStep(LiveActor* a, const IdCtx&) { a->mSpine->mStep += 1; }
void CoinPredict(IdVec&, const IdCtx&) {}

// 5 PackunPetit, nerve NonActive: asleep. exeNonActive waits for the player within 5000; control counts the scale controller (Stop nerve) and asks the 2P pointer.
id::Why PetIdle(LiveActor* a, IdCtx&) {
    if (port_idle25_petit_nerve(a) == 0) {
        return id::kWhyNerve;
    }
    PackunPetit* p = static_cast< PackunPetit* >(a);
    if (p->mScaleController == nullptr || !p->mScaleController->portIsStopped()) {
        return id::kWhyNerve;
    }
    if (id::PetNear(a->mPosition.squared(*MR::getPlayerPos()))) {
        return id::kWhyNear;
    }
    if (MR::portStarPointerReaches(a, 1)) {
        return id::kWhyPointer;
    }
    return id::kIdle;
}
void PetStep(LiveActor* a, const IdCtx&) {
    a->mSpine->mStep += 1;
    static_cast< PackunPetit* >(a)->mScaleController->mSpine->mStep += 1;
}
void PetPredict(IdVec& v, const IdCtx&) { v.w[V_PETSTEP] = static_cast< uint32_t >(static_cast< int32_t >(v.w[V_PETSTEP]) + 1); }

const IdFamDef kIdFams[] = {
    {id::kFamStarPiece, "starpiece", SbIdle, SbStep, SbPredict, true},
    {id::kFamCoin, "coin", CoinIdle, CoinStep, CoinPredict, false},
    {id::kFamPetit, "packunpetit", PetIdle, PetStep, PetPredict, false},
};
const IdFamDef* IdFind(uint8_t fam) {
    for (const IdFamDef& d : kIdFams) {
        if (d.fam == fam) {
            return &d;
        }
    }
    return nullptr;
}

// What every family needs for the turn to be a micro-step, whatever its nerve (movement()'s generic parts).
id::Why IdCommon(LiveActor* a) {
    if (a->mFlag.mIsDead || a->mAlodOwed != 0 || a->mSpine == nullptr) {
        return id::kWhyOff;
    }
    const Spine* sp = a->mSpine;
    if (sp->mNextNerve != nullptr || sp->mStep < 1) {
        return id::kWhyNerve;
    }
    if (a->mFlag.mIsCalcGravity || (a->mBinder != nullptr && !a->mFlag.mIsNoBind)) {
        return id::kWhyGeom;
    }
    const HitSensorKeeper* k = a->mSensorKeeper;
    if (k != nullptr) {
        for (s32 i = 0; i < k->mSensorInfosSize; ++i) {
            if (k->mSensorInfos[i]->mSensor->mSensorCount != 0) {
                return id::kWhyHit;
            }
        }
    }
    return id::kIdle;
}

// The idle turn: movement() with the proved parts left out and the micro-step in place of the nerve, control and the binder.
void IdRun(LiveActor* a, const IdFamDef& d, const IdCtx& c) {
    ModelManager* mm = a->mModelManager;
    if (mm != nullptr && !a->mFlag.mIsStoppedAnim && !ModelQuiet(mm)) {
        mm->update();  // (ActorAnimKeeper::update is empty)
    }
    d.step(a, c);
    a->mPosition += a->mVelocity;  // updateBinder: the binder is null, or off (IdCommon)
    if (a->mBinder != nullptr) {
        a->mBinder->clear();
    }
    if (!EffectsQuiet(a->mEffectKeeper)) {
        a->mEffectKeeper->update();
    }
    if (a->mCameraCtrl != nullptr) {
        a->mCameraCtrl->update();
    }
    if (a->mActorLightCtrl != nullptr) {
        MR::updateLightCtrl(a);
    }
    if (!(d.starSensors && StarSensorsAtHost(a))) {
        MR::tryUpdateHitSensorsAll(a);
    }
    MR::actorSoundMovement(a);
    MR::requestCalcActorShadow(a);
}

void IdLogMaybe() {
    if (sIdLogTurn0 == 0) {
        sIdLogTurn0 = gPortAlodTurn;
        sIdLogT0 = port_host_time_ns();
        return;
    }
    const uint32_t turns = gPortAlodTurn - sIdLogTurn0;
    if (turns < 300) {
        return;
    }
    const int64_t now = port_host_time_ns();
    const double secs = (now - sIdLogT0) / 1e9, t = turns;
    const int mode = IdMode();
    double savedMs = 0.0;
    for (const IdFamDef& d : kIdFams) {
        const id::FamCensus& c = sIdCen[d.fam];
        if (c.decisions == 0) {
            continue;
        }
        const double idleUs = c.idleSamples ? c.idleNs / 1e3 / c.idleSamples : 0.0;
        const double realUs = c.realSamples ? c.realNs / 1e3 / c.realSamples : 0.0;
        const double saved = mode == 1 && c.idleSamples && c.realSamples ? c.idle * (realUs - idleUs) / 1e3 / t : 0.0;
        savedMs += saved;
        char why[200];
        int n = 0;
        why[0] = 0;
        for (int w = 1; w < id::kWhyN; ++w) {
            if (c.why[w] != 0 && n < 180) {
                n += snprintf(why + n, sizeof why - n, " %s %.1f", id::WhyName(w), c.why[w] / t);
            }
        }
        port_log_summary("idle25: %s: a turn: %.1f decide, %.1f %s (%.0f%%), %.2f woke; awake because:%s; cost an idle turn %.2f us, a real one %.2f us "
                         "(sampled %u/%u); saved ~%.3f ms a turn; VERIFY %llu checks, %llu mismatches",
                         d.name, c.decisions / t, c.idle / t, mode == 2 ? "predicted idle" : "idle (skipped)", c.decisions ? 100.0 * c.idle / c.decisions : 0.0,
                         c.wakes / t, why, idleUs, realUs, static_cast< unsigned >(c.idleSamples), static_cast< unsigned >(c.realSamples), saved,
                         static_cast< unsigned long long >(c.verifyChecks), static_cast< unsigned long long >(c.verifyBad));
    }
    port_log_summary("idle25: mode %d, %.1f s, %u turns: estimated saving %.3f ms a turn in all", mode, secs, turns, savedMs);
    for (int f = 0; f < id::kFamN; ++f) {
        sIdCen[f] = id::FamCensus();
    }
    sIdLogTurn0 = gPortAlodTurn;
    sIdLogT0 = now;
}

// The entry. Returns after the actor's movement of this turn was made (skipped as idle, or run by `real`).
// verifyfix31: set while GXV_IDLE25=2's compare runs the real movement: ALOD must not take a rest turn then (port::idle25::AlodSkipUnderVerify)
static bool sIdVerifyReal = false;
template < class Real >
void IdMovement(LiveActor* a, int mode, Real real) {
    const IdFamDef* d = IdFind(a->mFlFam);
    if (d == nullptr) {
        real();
        return;
    }
    id::FamCensus& cen = sIdCen[d->fam];
    ++cen.decisions;
    IdCtx ctx;
    id::Why why = IdCommon(a);
    if (why == id::kIdle) {
        why = d->idle(a, ctx);
    }
    const bool wasIdle = (a->mFlState & 0x10) != 0;
    const bool sample = (++sIdSample & 15) == 0 && ::port::gt71::StatClockOn();  // (gt71 CLOCK: the sampled timers only feed the census line)
    if (why != id::kIdle) {
        ++cen.why[why];
        if (wasIdle) {
            ++cen.wakes;
        }
        a->mFlState &= static_cast< u8 >(~0x10u);
        if (sample) {
            const int64_t t0 = port_host_time_ns();
            real();
            cen.realNs += static_cast< uint64_t >(port_host_time_ns() - t0);
            ++cen.realSamples;
        } else {
            real();
        }
        return;
    }
    ++cen.idle;
    a->mFlState |= 0x10;
    if (mode == 1) {
        if (sample) {
            const int64_t t0 = port_host_time_ns();
            IdRun(a, *d, ctx);
            cen.idleNs += static_cast< uint64_t >(port_host_time_ns() - t0);
            ++cen.idleSamples;
        } else {
            IdRun(a, *d, ctx);
        }
        return;
    }
    // VERIFY
    IdVec before, expect, after;
    Capture(a, before);
    expect = before;
    PredictCommon(expect);
    d->predict(expect, ctx);
    const bool modelQuiet = a->mModelManager == nullptr || a->mFlag.mIsStoppedAnim || ModelQuiet(a->mModelManager);
    const bool sensorsQuiet = d->starSensors && StarSensorsAtHost(a);
    sIdVerifyReal = true;
    real();
    sIdVerifyReal = false;
    Capture(a, after);
    // what the idle turn would have RUN (not skipped) is the real thing: nothing is predicted for it
    if (!modelQuiet) {
        for (int i = V_ANM; i < V_N; ++i) {
            expect.w[i] = after.w[i];
        }
    }
    if (!sensorsQuiet) {
        for (int i = V_SPOS; i < V_SCNT; ++i) {
            expect.w[i] = after.w[i];
        }
    }
    ++cen.verifyChecks;
    uint32_t bad = 0;
    int first = -1;
    for (int i = 0; i < V_N; ++i) {
        if (after.w[i] != expect.w[i]) {
            ++bad;
            if (first < 0) {
                first = i;
            }
        }
    }
    if (bad != 0) {
        ++cen.verifyBad;
        if (sIdBadLogged < 40) {
            ++sIdBadLogged;
            port_log_summary("idle25: VERIFY MISMATCH %s: %u fields differ, first %s: expected %08x, real %08x (before %08x); nerve step %d",
                             d->name, bad, VName(first), expect.w[first], after.w[first], before.w[first],
                             a->mSpine != nullptr ? static_cast< int >(a->mSpine->mStep) : -1);
        }
    }
}
}  // namespace

extern "C" int port_idle25_mode(void) { return IdMode(); }

// ================================================================================================================================
// GXV_STARBATCH_FAR=<metres> (starbatch26; default 0 = off; stage-settable; 1 metre = 100 units). A star bit in its Floating nerve (it is not being
// collected, thrown or flying; it stays where it is and only spins) that is farther than this from the camera AND from the player is posed (its
// joints, material list and so on: LiveActor::calcAnim, port_anim_flush_stale) on every second pose only; the other time it keeps the pose of the
// last one and the view pass places it for this camera (the calcViewAndEntry that follows). The parity is the picture's (flush) or the turn's (a pose
// made in the turn), spread by the actor's address. What it costs: the spin (15 degrees a turn) of a far star bit shows the angle of one pose
// earlier, at most a picture (about 27 degrees at 33 pictures a second; 15 at 60) old: a small spinning sprite-sized object beyond 30 m or more.
// Not for coins or anything else (their spin is a shared matrix or has other work), nor the first pose of an actor.
namespace {
std::atomic< unsigned > sSbFar[4];  // 0 asked, 1 left out, 2 too near / not floating (asked and refused)
inline float SbFarDist2() {
    return PORT_STAGE_KNOB(float, [] {
        const float m = AlodEnvFloat("GXV_STARBATCH_FAR", 0.0f, 0.0f, 10000.0f);
        return m * m * 10000.0f;  // (metres to units, squared)
    }());
}
}  // namespace
extern "C" int port_starbatch_far_on(void) { return SbFarDist2() > 0.0f ? 1 : 0; }
// ratecut31, GXV_POSE_PHASE (port/pose_phase.h, perf.cpp): in a turn (atFlush 0) a far star bit's pose is left out on the skip turn only (a drawn turn after an
// undrawn one: the undrawn turn poses it again before the picture), never for a model of 60 Assist's pair set; at the flush (atFlush 1: the picture's own pose)
// mode 1 never leaves it out, mode 2 keeps the picture parity. Mode 0: the parity rule as before. The census books each decision (kind 5).
extern "C" int port_pose_phase_mode(void);
extern "C" int port_pose_phase_skip;
extern "C" int port_pose_phase_census_flag;
extern "C" void port_pose_phase_note(int kind, int age, int tagged);
extern "C" void port_pose_phase_note_flush(int age);
extern "C" int port_starbatch_far_skip(LiveActor* a, unsigned parity, int atFlush) {
    const float d2 = SbFarDist2();
    if (d2 <= 0.0f || a->mFlFam != id::kFamStarPiece || a->mCrPosed == 0 || !port_safe_cam_exists()) {
        return 0;
    }
    const int ppMode = port_pose_phase_mode();
    if (ppMode != 0 && (atFlush ? !port::posephase::StarFlushAllowed(ppMode)
                                : (a->mModelManager != nullptr && a->mModelManager->mModel != nullptr && a->mModelManager->mModel->mPortHrwTag != 0))) {
        return 0;
    }
    ++sSbFar[0];
    const TVec3f* player = port_safe_player_pos();
    if (player == nullptr || port_idle25_starpiece_nerve(a) != 1 || a->mFlag.mIsDead || a->mCollisionParts != nullptr) {
        ++sSbFar[2];
        return 0;
    }
    const TVec3f cam = MR::getCamPos();
    const TVec3f& p = a->mPosition;
    const float cx = p.x - cam.x, cy = p.y - cam.y, cz = p.z - cam.z, px = p.x - player->x, py = p.y - player->y, pz = p.z - player->z;
    if (!(cx * cx + cy * cy + cz * cz > d2) || !(px * px + py * py + pz * pz > d2)) {
        ++sSbFar[2];
        return 0;
    }
    const bool skipTurn = port_pose_phase_skip != 0;
    if (atFlush ? (((parity + static_cast< unsigned >(reinterpret_cast< uintptr_t >(a) >> 4)) & 1u) == 0u)
                : !port::posephase::StarTurnSkip(parity, static_cast< uint32_t >(reinterpret_cast< uintptr_t >(a) >> 4), ppMode, skipTurn)) {
        if (__builtin_expect(port_pose_phase_census_flag != 0, 0) && !atFlush) {
            port_pose_phase_note(5, 0, 0);
        }
        return 0;  // (this one is posed)
    }
    ++sSbFar[1];
    if (__builtin_expect(port_pose_phase_census_flag != 0, 0)) {
        if (atFlush) {
            port_pose_phase_note_flush(2);  // (the picture's own pose kept from a picture before: two turns in a pair)
        } else {
            port_pose_phase_note(5, 1, a->mModelManager != nullptr && a->mModelManager->mModel != nullptr && a->mModelManager->mModel->mPortHrwTag != 0 ? 1 : 0);
        }
    }
    return 1;
}
extern "C" void port_starbatch_far_counts(unsigned* out) {
    for (int i = 0; i < 4; i++) {
        out[i] = sSbFar[i].load(std::memory_order_relaxed);
    }
}

static void AlodMovementInner(NameObj* pObj) {
    LiveActor* a = static_cast< LiveActor* >(pObj);
    const int mode = port_alod_mode();
    if (__builtin_expect(mode == 0 && a->mAlodOwed != 0 && a->mAlodRate > 1, 0)) {
        // (fmt32: the mode went to 0 under an actor that stood behind in mode 1 (its rate is above 1; the census mode's count is not a debt), i.e. a lockstep session began:
        // the steps it owes are made now, as before any movement of its own, and it is an ordinary actor from here on)
        OwedAdapter ad{a};
        port::alod::PayOwed(ad);
        a->mAlodRate = 0;
        if (a->mAlodCulled != 0) {
            PortAlodUncull(a);
        }
        if (a->mFlag.mIsDead) {
            a->mAlodNerve = nullptr;
            return;
        }
    }
    if (mode == 0 || a->mAlodFamily == 0) {
        if (port_farlogic_level() != 0 || port_farlogic_dry() != 0) {
            FlMovement(a);  // (GXV_FARLOGIC: the far-logic entry; an actor of ALOD's families belongs to ALOD while GXV_ALOD is on)
        } else {
            if ((a->mFlState & 7) != 0) {
                FlFull(a);  // (the level was lowered to 0: the far state ends, the joints are computed on every turn again)
            }
            a->movement();
        }
        return;
    }
    FamStats& fs = sFam[a->mAlodFamily < kFam ? a->mAlodFamily : 0];
    ++fs.decisions;
    bool off = false;
    const int rate = Decide(a, fs, &off);
    bool skip = rate > 1 && port::alod::SkipThisTurn(gPortAlodTurn, port::alod::PhaseOf(a), rate, a->mAlodOwed);
    skip = port::idle25::AlodSkipUnderVerify(skip, sIdVerifyReal);  // (IDLE25=2: the compare's reference is the original movement, never a rest turn)
    if (rate > 1 && !skip) {
        ++fs.why[kWhyCycle];
    }
    // (census: a nerve set from outside while the actor stood behind; mode 1 replays its owed steps under that nerve)
    if (a->mAlodOwed != 0 && a->mSpine != nullptr && a->mSpine->getCurrentNerve() != a->mAlodNerve) {
        ++fs.nerveOutside;
    }
    if (skip) {
        ++fs.leftOut;
        ++(rate >= 4 ? fs.quarter : fs.half);
    }
    if (mode == 2) {
        // census: the original movement every turn; mAlodOwed is the debt mode 1 would have (so its cycle is the same), never paid
        a->mAlodRate = 0;
        const int64_t t0 = port_host_time_ns();
        a->movement();
        const int64_t ns = port_host_time_ns() - t0;
        if (ns > 0) {
            fs.moveNs += static_cast< uint64_t >(ns);
            ++fs.moveCalls;
            if (skip) {
                fs.wouldMoveNs += static_cast< uint64_t >(ns);
            }
        }
        if (skip) {
            ++a->mAlodOwed;
            a->mAlodSkipTurn = gPortAlodTurn;  // (LiveActor::calcAnim times this turn's calcAnim, and makes it)
        } else {
            a->mAlodOwed = 0;
        }
        a->mAlodNerve = a->mSpine != nullptr ? a->mSpine->getCurrentNerve() : nullptr;
        return;
    }
    a->mAlodRate = static_cast< u8 >(rate > 1 ? rate : 1);
    if (a->mAlodCulled != 0 && !(skip && off)) {
        // (a stamp from a turn that drew no picture names the picture this turn draws: it must not hide the model now)
        PortAlodUncull(a);
    }
    if (skip) {
        ++a->mAlodOwed;
        a->mAlodSkipTurn = gPortAlodTurn;  // (LiveActor::calcAnim leaves this turn's calcAnim out)
        KeepLevelSounds(a);
        J3DModel* model = off && KnobDrawCull() ? AlodModel(a) : nullptr;
        if (model != nullptr) {
            model->mPortCullPicture = J3DPortPicture() + 1;  // (not drawn in the picture draw() makes next; DrawBuffer.cpp)
            a->mAlodCulled = 1;
        }
        return;
    }
    if (a->mAlodOwed != 0) {
        OwedAdapter ad{a};
        const bool sample = (++sReplaySample & 7) == 0 && ::port::gt71::StatClockOn();
        const int64_t t0 = sample ? port_host_time_ns() : 0;
        const int n = port::alod::PayOwed(ad);
        if (sample) {
            sReplayNs += static_cast< uint64_t >(port_host_time_ns() - t0);
            sReplaySteps += static_cast< uint64_t >(n);
        }
        fs.replayed += static_cast< uint64_t >(n);
        if (a->mFlag.mIsDead) {
            // (alodfix23) a replayed step ended its life (makeActorDead also cleared the rest of the debt): in the original that step was
            // made turns ago and the actor has not moved since (makeActorDead disconnects it from the lists). No movement now.
            a->mAlodNerve = nullptr;
            return;
        }
    }
    if ((++sMoveSample & 15) == 0 && ::port::gt71::StatClockOn()) {
        const int64_t t0 = port_host_time_ns();
        a->movement();
        fs.moveNs += static_cast< uint64_t >(port_host_time_ns() - t0) * 16u;  // (sampled one in 16)
        fs.moveCalls += 16;
    } else {
        a->movement();
    }
    a->mAlodNerve = a->mSpine != nullptr ? a->mSpine->getCurrentNerve() : nullptr;
}

void port_alod_movement(NameObj* pObj) {
    LiveActor* a = static_cast< LiveActor* >(pObj);
    if (a->mFlFam != 0) {
        const int im = IdMode();
        if (im != 0) {
            IdMovement(a, im, [&] { AlodMovementInner(pObj); });
            return;
        }
    }
    AlodMovementInner(pObj);
}

namespace MR {
// Marks an actor of a family (1..7, see the top) for GXV_ALOD, at the end of its class's init. Only when GXV_ALOD is on (so an
// actor costs nothing with it off: NameObj::executeMovement tests a bit it already loads), the family is in GXV_ALOD_FAMILIES, and
// the actor has at most 16 sensors (LiveActor::portMovementLite hides at most 16 hit lists).
void portMarkActorLod(LiveActor* pActor, int family) {
    if (family > 0 && family < kFam) {
        pActor->mFlFam = static_cast< u8 >(family);  // (GXV_FARLOGIC names the families whatever GXV_ALOD says)
    }
    if (port_alod_mode() == 0 || family <= 0 || family >= kFam || (KnobFamilies() & (1 << (family - 1))) == 0) {
        return;
    }
    if (pActor->mSensorKeeper != nullptr && pActor->mSensorKeeper->mSensorCount > 16) {
        return;
    }
    pActor->mAlodFamily = static_cast< u8 >(family);
    static_cast< NameObj* >(pActor)->mFlag |= PORT_NAMEOBJ_FLAG_ALOD;
    ++sFam[family].marked;
}
}  // namespace MR
#endif
