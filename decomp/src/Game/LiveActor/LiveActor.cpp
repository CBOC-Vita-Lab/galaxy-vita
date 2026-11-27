#include "Game/LiveActor/LiveActor.hpp"
#include "Game/AudioLib/AudAnmSoundObject.hpp"
#include "Game/LiveActor/ActorAnimKeeper.hpp"
#include "Game/LiveActor/ActorLightCtrl.hpp"
#include "Game/LiveActor/ActorPadAndCameraCtrl.hpp"
#include "Game/LiveActor/AllLiveActorGroup.hpp"
#include "Game/LiveActor/Binder.hpp"
#include "Game/LiveActor/ClippingDirector.hpp"
#include "Game/LiveActor/EffectKeeper.hpp"
#include "Game/LiveActor/HitSensor.hpp"
#include "Game/LiveActor/HitSensorKeeper.hpp"
extern "C" const TVec3f* port_safe_player_pos(void);  // (LodCtrl.cpp)
extern "C" int port_safe_cam_exists(void);  // (LodCtrl.cpp)
#ifdef TARGET_PC
#include "Game/LiveActor/HitSensorInfo.hpp"  // (GXV_ALOD: LiveActor::portMovementLite)
#include "port/actor_lod.h"                  // (GXV_ALOD, actorlod23: the update LOD of far, off-screen actors; ActorTurnLod.cpp)
#include "port/farlogic.h"                   // (GXV_FARLOGIC, farlogic25: the far-logic level; ActorTurnLod.cpp)
#include "port/lockstep.h"                   // (fmt32: the pose rules that go by this console's camera are off in a lockstep session)
#include "port/mp_gate.h"                    // MP3 lodgate: pose cuts are held for actors with a hit sensor in a session
extern "C" int port_farlogic_armed(void);
#endif
#include "Game/LiveActor/ModelManager.hpp"
#include "Game/LiveActor/RailRider.hpp"
#include "Game/LiveActor/ShadowController.hpp"
#include "Game/LiveActor/Spine.hpp"
#include "Game/MP/MpGame.hpp"  // MP3: shared enemy defeats
#include "Game/Map/StageSwitch.hpp"
#include "Game/NameObj/NameObjExecuteHolder.hpp"
#include "Game/Screen/StarPointerTarget.hpp"
#include "Game/Util/ActorMovementUtil.hpp"
#include "Game/Util/ActorSensorUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3DGraphAnimator/J3DJointTree.hpp>
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>

#ifdef TARGET_PC
#include "port/diagnostics.h"
#include "port/hot_knob.h"
#include "port/logic_prof.h"  // (GXV_LOGIC_PROF, logic23: the class rows are named by the model; =2 times movement's phases)
#include "port/gt71.h"  // (gt71)
// cliprange25 (ClippingActorHolder.cpp): GXV_CLIPCENSUS's marks (0 movement entered, 1 calcAnim entered, 2 joints posed) and GXV_FARANIM's knobs
extern "C" int port_cc_flag;
extern "C" void port_cc_mark(const void* pActor, int what);
extern "C" int port_cr_faranim_n(void);
extern "C" float port_cr_faranim_lines(void);
extern "C" int port_cr_faranim_sens(void);
extern "C" float port_cr_near(void);
extern "C" int port_cr_drawflush_on(void);          // (cpucuts25, GXV_FARDRAW_FLUSH: ClippingActorHolder.cpp)
extern "C" void port_cr_flush_count(unsigned n);
extern "C" void port_thinfade_apply(const void* actor, void* model);  // (q30fin26, GXV_THINFADE: ClippingActorHolder.cpp)
extern "C" int port_cp_n_for(uint32_t seed);  // (q30lock26: the level this actor is at while a governor / lock level change is walked in)
extern "C" float port_cp_lines_for(uint32_t seed);
extern "C" int port_cpulod_matlod_n_for(uint32_t seed);
extern "C" float port_cpulod_matlod_dist_for(uint32_t seed);
extern "C" int port_cpulod_fampose_n_for(uint32_t seed);
extern "C" float port_cpulod_fampose_dist_for(uint32_t seed);
extern "C" float port_cpulod_fampose_move_for(uint32_t seed);
extern "C" int port_cp_n(void);                      // (cpucuts25, GXV_CHARPOSE: ClippingActorHolder.cpp)
extern "C" float port_cp_lines(void);
extern "C" float port_cp_move(void);
extern "C" float port_cp_dist2(void);
// cpulod26 (ClippingActorHolder.cpp): GXV_MATLOD / GXV_FAMPOSE / GXV_PIECETHIN knobs and the census counters
extern "C" int port_cpulod_matlod_n(void);
extern "C" int port_protect_sprites(void);  // (cpu2-26, ClippingActorHolder.cpp: GXV_PROTECT_SPRITES)
extern "C" float port_cpulod_matlod_dist(void);
extern "C" int port_cpulod_fampose_n(void);
extern "C" float port_cpulod_fampose_dist(void);
extern "C" float port_cpulod_fampose_move(void);
extern "C" void port_cpulod_count(int what, unsigned n);
#include "port/cliprange.h"
#include "port/cpucuts.h"
#include "port/farprotect.h"
#include "port/cpulod.h"
#include "port/pose_phase.h"
// ratecut31 (perf.cpp, port/pose_phase.h): GXV_POSE_PHASE, the every-n-th-turn cuts below leave a pose out only on a skip turn (a drawn turn after an undrawn one,
// whose calc-anim the undrawn turn makes again before any picture); GXV_POSE_PHASE_CENSUS, the age of the pose each picture shows.
extern "C" int port_pose_phase_mode(void);
extern "C" int port_pose_phase_skip;          // (written at every turn's start; 0 when the mode is 0)
extern "C" int port_pose_phase_census_flag;
extern "C" void port_pose_phase_note(int kind, int age, int tagged);
extern "C" void port_pose_phase_note_flush(int age);
namespace {
// A model of 60 Assist's pair set (tagged in this turn's picture, PortHrwSet.cpp): the second picture draws it from this turn's pose.
inline bool PosePhaseTagged(const LiveActor* a) {
    const ModelManager* mm = a->mModelManager;
    return mm != nullptr && mm->mModel != nullptr && mm->mModel->mPortHrwTag != 0;
}
inline void PosePhaseNote(const LiveActor* a, int kind, int age) {
    if (__builtin_expect(port_pose_phase_census_flag != 0, 0)) {
        port_pose_phase_note(kind, age, PosePhaseTagged(a) ? 1 : 0);
    }
}
}  // namespace
// (GXV_LOGIC_PROF: a class row is named by its model's ASCII resource name, the first time one of its objects runs)
static inline void PortLogicProfName(const LiveActor* pActor) {
    if (__builtin_expect(port_logic_prof_flag != 0, 0) && pActor->mModelManager != nullptr && port_logic_prof_obj_wants_name()) {
        port_logic_prof_obj_name(MR::getModelResName(pActor));
    }
}
#if defined(__vita__) && PORT_LOGGING
extern "C" u32 port_workload_anim_family_begin(unsigned family);
extern "C" void port_workload_anim_family_end(u32 token);
#endif
#if PORT_LOGGING
// GXV_ANIM_PROF (platform/src/port/perf.cpp): the row of the object being timed is named by its model (ASCII resource name).
extern "C" int port_anim_prof_on(void);
extern "C" void port_anim_prof_actor_name(const char* name);
extern "C" void port_anim_prof_count(int what);
static inline void PortAnimProfName(const LiveActor* pActor) {
    if (::port::gt71::AnimProfOn() && pActor->mModelManager != nullptr) {
        port_anim_prof_actor_name(MR::getModelResName(pActor));
    }
}
#define PORT_ANIM_PROF_NAME(actor) PortAnimProfName(actor)
#define PORT_ANIM_PROF_COUNT(what) do { if (::port::gt71::AnimProfOn()) port_anim_prof_count(what); } while (0)
#else
#define PORT_ANIM_PROF_NAME(actor) ((void)0)
#define PORT_ANIM_PROF_COUNT(what) ((void)0)
#endif
#endif

void LiveActor_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
    (void)-1.0f;
}

LiveActor::LiveActor(const char* pName)
    : NameObj(pName), mPosition(0.0f, 0.0f, 0.0f), mRotation(0.0f, 0.0f, 0.0f), mScale(1.0f, 1.0f, 1.0f), mVelocity(0.0f, 0.0f, 0.0f),
      mGravity(0.0f, -1.0f, 0.0f), mModelManager(), mAnimKeeper(), mSpine(), mSensorKeeper(), mBinder(), mRailRider(), mEffectKeeper(),
      mSoundObject(), mShadowControllerList(), mCollisionParts(), mStageSwitchCtrl(), mStarPointerTarget(), mActorLightCtrl(), mCameraCtrl() {
#ifdef TARGET_PC
    if (port_farlogic_armed() != 0) {
        static_cast< NameObj* >(this)->mFlag |= PORT_NAMEOBJ_FLAG_ALOD;  // (GXV_FARLOGIC: movement goes through ActorTurnLod.cpp's entry)
    }
#endif
    MR::getAllLiveActorGroup()->registerActor(this);
    MR::getClippingDirector()->registerActor(this);
}

void LiveActor::init(const JMapInfoIter& rIter) {
}

void LiveActor::appear() {
    makeActorAppeared();
}

void LiveActor::makeActorAppeared() {
#ifdef TARGET_PC
    mAlodOwed = 0;  // (GXV_ALOD: steps owed from before are not a new life's)
    mFlState = 0;   // (GXV_FARLOGIC: nor its far state)
    if (mAlodCulled != 0) {
        PortAlodUncull(this);  // (nor its draw stamp)
    }
#endif
    if (mSensorKeeper != nullptr) {
        mSensorKeeper->validateBySystem();
    }

    if (MR::isClipped(this)) {
        endClipped();
    }

    mFlag.mIsDead = false;

    if (mCollisionParts != nullptr) {
        MR::validateCollisionParts(this);
    }

    MR::resetPosition(this);

    if (mActorLightCtrl != nullptr) {
        mActorLightCtrl->reset();
    }

    MR::tryUpdateHitSensorsAll(this);
    MR::addToClippingTarget(this);
    MR::connectToSceneTemporarily(this);

    if (!MR::isNoEntryDrawBuffer(this)) {
        MR::connectToDrawTemporarily(this);
    }
}

void LiveActor::kill() {
    makeActorDead();
}

// (merge of 3.0 on c181: a non-inlined helper, so that makeActorDead keeps c181's control flow and its PGO profile)
static void PortMpActorDead(LiveActor* pActor) __attribute__((noinline));
static void PortMpActorDead(LiveActor* pActor) {
    if (mp_active()) {
        MpWorldSync::onActorDead(pActor);
    }
}

void LiveActor::makeActorDead() {
    PortMpActorDead(this);  // MP3: shared world: a placed enemy dying here dies for everyone (checks before the actor is cleared); one call, the session test is inside (c181's control flow stays)
#ifdef TARGET_PC
    mAlodOwed = 0;  // (GXV_ALOD: a dead actor's movement stops at once; owed steps are not replayed into it)
    mFlState = 0;   // (GXV_FARLOGIC)
#endif
    mVelocity.zero();

    MR::clearHitSensors(this);

    if (mSensorKeeper != nullptr) {
        mSensorKeeper->invalidateBySystem();
    }

    if (getBinder() != nullptr) {
        mBinder->clear();
    }

    if (mEffectKeeper != nullptr) {
        mEffectKeeper->clear();
    }

    if (mCollisionParts != nullptr) {
        MR::invalidateCollisionParts(this);
    }

    mFlag.mIsDead = true;

    MR::removeFromClippingTarget(this);
    MR::disconnectToSceneTemporarily(this);
    MR::disconnectToDrawTemporarily(this);
}

#ifdef TARGET_PC
extern "C" int port_undrawn_lean_flag;
extern "C" void port_undrawn_set_reader(const char* name);
namespace {
// (GXV_UNDRAWN_LEAN) who is running, for the VIOLATION line of a read of a left-out product
struct PortUlReader {
    PortUlReader(const char* name) {
        if (port_undrawn_lean_flag) {
            port_undrawn_set_reader(name);
        }
    }
    ~PortUlReader() {
        if (port_undrawn_lean_flag) {
            port_undrawn_set_reader(nullptr);
        }
    }
};
}  // namespace
#endif
void LiveActor::movement() {
#ifdef TARGET_PC
    PortUlReader ulReader(mName);
    PORT_ANIM_PROF_NAME(this);
    PortLogicProfName(this);
    if (__builtin_expect(port_cc_flag != 0, 0)) {
        port_cc_mark(this, 0);  // (GXV_CLIPCENSUS)
    }
    if (__builtin_expect(port_logic_prof_flag >= 2, 0)) {
        portMovementPhased();  // (GXV_LOGIC_PROF=2: the same steps, each timed)
        return;
    }
#endif
    if (mModelManager != nullptr && !mFlag.mIsStoppedAnim) {
        mModelManager->update();

        if (mAnimKeeper != nullptr) {
            mAnimKeeper->update();
        }
    }

    if (MR::isCalcGravity(this)) {
        MR::calcGravity(this);
    }

    if (mSensorKeeper != nullptr) {
        mSensorKeeper->doObjCol();
    }

    if (mFlag.mIsDead) {
        return;
    }

    if (mSpine != nullptr) {
        mSpine->update();
    }

    if (mFlag.mIsDead) {
        return;
    }

    control();

    if (mFlag.mIsDead) {
        return;
    }

    updateBinder();

    if (mEffectKeeper != nullptr) {
        mEffectKeeper->update();
    }

    if (mCameraCtrl != nullptr) {
        mCameraCtrl->update();
    }

    if (mActorLightCtrl != nullptr) {
        MR::updateLightCtrl(this);
    }

    MR::tryUpdateHitSensorsAll(this);
    MR::actorSoundMovement(this);
    MR::requestCalcActorShadow(this);
}

#ifdef TARGET_PC
// GXV_LOGIC_PROF=2 (logic23, 8 Nov; port/logic_prof.h): movement() above, step for step and in the same order, with each phase timed
// as a "movement phase" system (two clock reads a phase). A diagnostic: keep it equal to movement() when that changes.
void LiveActor::portMovementPhased() {
    if (mModelManager != nullptr && !mFlag.mIsStoppedAnim) {
        PORT_LOGIC_PHASE(PORT_LS_MV_ANIMCLOCK, {
            mModelManager->update();

            if (mAnimKeeper != nullptr) {
                mAnimKeeper->update();
            }
        });
    }

    if (MR::isCalcGravity(this)) {
        PORT_LOGIC_PHASE(PORT_LS_MV_GRAVITY, MR::calcGravity(this));
    }

    if (mSensorKeeper != nullptr) {
        PORT_LOGIC_PHASE(PORT_LS_MV_OBJCOL, mSensorKeeper->doObjCol());
    }

    if (mFlag.mIsDead) {
        return;
    }

    if (mSpine != nullptr) {
        PORT_LOGIC_PHASE(PORT_LS_MV_NERVE, mSpine->update());
    }

    if (mFlag.mIsDead) {
        return;
    }

    PORT_LOGIC_PHASE(PORT_LS_MV_CONTROL, control());

    if (mFlag.mIsDead) {
        return;
    }

    PORT_LOGIC_PHASE(PORT_LS_MV_BINDER, updateBinder());

    if (mEffectKeeper != nullptr) {
        PORT_LOGIC_PHASE(PORT_LS_MV_EFFECT, mEffectKeeper->update());
    }

    if (mCameraCtrl != nullptr) {
        PORT_LOGIC_PHASE(PORT_LS_MV_CAMERA, mCameraCtrl->update());
    }

    if (mActorLightCtrl != nullptr) {
        PORT_LOGIC_PHASE(PORT_LS_MV_LIGHT, MR::updateLightCtrl(this));
    }

    PORT_LOGIC_PHASE(PORT_LS_MV_SENSORPOS, MR::tryUpdateHitSensorsAll(this));
    PORT_LOGIC_PHASE(PORT_LS_MV_SOUND, MR::actorSoundMovement(this));
    PORT_LOGIC_PHASE(PORT_LS_MV_SHADOWREQ, MR::requestCalcActorShadow(this));
}

// GXV_ALOD (actorlod23, 8 Nov; ActorTurnLod.cpp, port/actor_lod.h): the movement of a turn that was left out, replayed right before the
// actor's next movement (the catch-up). It is LiveActor::movement (the marked classes do not override it) less three things whose
// result the movement made right after it overwrites before anything reads them:
//  - doObjCol: the hit lists of the left-out turn were empty (an actor with a hit is never left out), so the original's call did
//    nothing. The lists the actor holds now are this turn's: they are hidden from the replayed step (counts set to 0 for its
//    duration, as they were then) and given back for the movement of this turn, which consumes them as the original did.
//  - tryUpdateHitSensorsAll: the sensor positions; the next movement sets them again before the sensor checker reads them.
//  - requestCalcActorShadow: a request flag; the next movement sets it again.
// Everything else runs as in movement(): the animation clock, gravity, the nerve (Spine), control(), the binder, effects, the
// camera and light controllers and the sound object (its BCK sound keys and level sounds see every step).
void LiveActor::portMovementLite() {
    // (the hit lists of this turn, hidden while the left-out turn's step runs)
    u16 savedCounts[16];
    s32 nSaved = 0;
    if (mSensorKeeper != nullptr) {
        nSaved = mSensorKeeper->mSensorInfosSize;  // (crash33: the sensors added; mSensorCount is the table's capacity, its unused entries are null)
        if (nSaved > 16) {
            nSaved = 16;  // (more sensors than any marked class has: the rest keep their lists; MR::portMarkActorLod refuses such actors)
        }
        for (s32 i = 0; i < nSaved; ++i) {
            HitSensor* hs = mSensorKeeper->mSensorInfos[i]->mSensor;
            savedCounts[i] = hs->mSensorCount;
            hs->mSensorCount = 0;
        }
    }
    if (mModelManager != nullptr && !mFlag.mIsStoppedAnim) {
        mModelManager->update();

        if (mAnimKeeper != nullptr) {
            mAnimKeeper->update();
        }
    }

    if (MR::isCalcGravity(this)) {
        MR::calcGravity(this);
    }

    if (!mFlag.mIsDead) {
        if (mSpine != nullptr) {
            mSpine->update();
        }

        if (!mFlag.mIsDead) {
            control();

            if (!mFlag.mIsDead) {
                updateBinder();

                if (mEffectKeeper != nullptr) {
                    mEffectKeeper->update();
                }

                if (mCameraCtrl != nullptr) {
                    mCameraCtrl->update();
                }

                if (mActorLightCtrl != nullptr) {
                    MR::updateLightCtrl(this);
                }

                MR::actorSoundMovement(this);
            }
        }
    }
    for (s32 i = 0; i < nSaved; ++i) {
        mSensorKeeper->mSensorInfos[i]->mSensor->mSensorCount = savedCounts[i];
    }
}

// GXV_ANIM_LOD (default 1; GXV_ANIM_LOD_DIST, default 3000 units): on a turn that draws no picture, a map object, NPC, decoration
// or environment model with no collision of its own and farther than the distance from both the player and the camera computes its
// joints and view only once per picture, just before it (port_anim_flush_stale): the picture is exact; what reads its joints between
// two pictures (an effect bound to a joint, a sensor, a part following a joint) sees them as of the last picture. Collision objects, enemies, items,
// the player and everything near keep every turn. The list is dropped with the scene (GameSystemSceneController).
extern "C" int port_turn_skipped_raw(void);
extern "C" int port_anim_lod_category(void);
extern "C" float port_anim_lod_dist2(void);
extern "C" float port_anim_lod_pdist2(void);
extern "C" int port_anim_lod_offscreen(void);
extern "C" int port_anim_lod_free_on(void);  // (GXV_ANIM_LOD_FREE: platform/src/port/perf.cpp)
extern "C" int port_view_pending(void);
#include "port/turn_prof.h"  // (gamepar: GXV_FLUSH_OFFSCREEN, GXV_PLANT_DRAWCULL, GXV_TURN_PROF)
#include "port/view_cone.h"   // (GXV_FLUSH_OFFSCREEN: the picture's view pyramid)
#include <cmath>              // (popin25: sqrt)
#include <JSystem/J3DGraphBase/J3DPacket.hpp>  // (J3DPortPicture: GXV_PLANT_DRAWCULL)
#include "Game/LiveActor/ClippingJudge.hpp"  // (zoomclip23: the picture's frustum, GXV_ANIM_LOD_OFFSCREEN)
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
extern "C" float port_pb_anim_dist2(void);   // (GXV_PB_ANIM, the Performance-mode visual budget: platform/src/port/perf.cpp)
extern "C" float port_pb_anim_pdist2(void);
extern "C" float port_detail_dist(int rule, float base);  // (detail25)
extern "C" int port_detail_plant_skip(LiveActor* a, int unseen);
extern "C" void port_detail_flush_begin(void);
#include "port/detail.h"
extern "C" float port_popin_zoom(void);  // (popin24: LodCtrl.cpp, GXV_POPIN_ZOOM)
extern "C" float port_pb_scale(void);    // (popin25: LodCtrl.cpp; the zoom ratio times GXV_PB_DEMO's widening in a flight)
extern "C" int port_pb_demo_off(void);   // (popin25: GXV_PB_DEMO=2 in a flight: the budget is off)
extern "C" float port_pb_lead(float limit);  // (popin25: GXV_POPIN_LEAD: appear earlier for a fast camera)
extern "C" void port_logic_count(int what, unsigned n);  // (platform/src/port/perf.cpp)
// GXV_ALOD=2 (census) and GXV_FARLOGIC_DRY=1: the calcAnim of a turn the mode would leave out is timed (ActorTurnLod.cpp port_alod_census_calc).
void port_alod_census_calc(LiveActor* a, int64_t ns);
extern "C" int port_farlogic_dry(void);
extern "C" int port_farlogic_pose_on(void);
extern "C" float port_farlogic_pose_dist2(void);
extern "C" int port_farlogic_pose_category(void);
extern "C" int port_farlogic_pose_candidate(const LiveActor* a);
extern "C" void port_farlogic_pose_census(int64_t ns);
// GXV_FARLOGIC_DRY, level 3: the calcAnim of an Enemy / Item beyond 2500 from the player and the camera (one in four turns timed).
struct PortFlPoseTimer {
    int64_t mT0 = 0;
    bool mOn = false;
    explicit PortFlPoseTimer(const LiveActor* a) {
        if (__builtin_expect(port_farlogic_dry() != 0, 0) && port_farlogic_pose_candidate(a) == 2) {
            mOn = true;
            mT0 = port_host_time_ns();
        }
    }
    ~PortFlPoseTimer() {
        if (mOn) {
            port_farlogic_pose_census(port_host_time_ns() - mT0);
        }
    }
};
struct PortAlodCalcTimer {
    LiveActor* mActor = nullptr;
    int64_t mT0 = 0;
    explicit PortAlodCalcTimer(LiveActor* a) {
        if (a->mAlodSkipTurn != 0 && a->mAlodSkipTurn == gPortAlodTurn && (port_alod_mode() == 2 || port_farlogic_dry() != 0)) {
            mActor = a;
            mT0 = port_host_time_ns();
        }
    }
    bool on() const { return mActor != nullptr; }
    ~PortAlodCalcTimer() {
        if (mActor != nullptr) {
            port_alod_census_calc(mActor, port_host_time_ns() - mT0);
        }
    }
};
#include <vector>
#include "port/host_vector.h"  // (PortHostVector: lists that outlive a scene keep host memory)
#include "port/live_set.h"     // (GXV_UPD_LIVESET: update25)
#include <algorithm>
#include <cstdlib>
#include "Game/Animation/XanimePlayer.hpp"
#include "Game/LiveActor/AllLiveActorGroup.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/ObjUtil.hpp"
LiveActor* gPortCalcAnimOwner = nullptr;
// The actors of the current scene (AllLiveActorGroup), sorted, taken at the start of a drawn turn: a listed pointer is used only if
// it is still one of them. Models can be freed inside a scene (the file select's), so a pointer kept from an earlier turn may be stale.
static PortHostVector< LiveActor* > sLiveSnapshot;  // (host heap: it outlives the scene; port/host_vector.h)
PORT_HEAP_GUARD(sLiveSnapshot);
// GXV_LIVE_SNAP (draw7, 5 Nov; default 1, platform/src/port/perf.cpp port_live_snap_on): the snapshot is taken twice per drawn turn
// (the far models' joints, then the materials) and the group hardly ever changes between pictures. The group's array as it was at
// the last sort is kept: when every entry (and their number) is the same, the sorted copy already is what the sort would make, and
// it stays. Exact (the same vector; a compare of the array instead of the copy and the sort). 0: sorted every time, as before.
extern "C" int port_live_snap_on(void);
static PortHostVector< LiveActor* > sLiveRaw;
PORT_HEAP_GUARD(sLiveRaw);
static bool sLiveRawValid = false;
// GXV_HOTPROF_LIVESNAP (hotprof25; below): the group object and the groups' write count sLiveRaw was copied or last found equal at.
extern "C" int port_hotprof_livesnap_mode(void);
extern unsigned gPortNameObjGroupEpoch;  // (NameObjGroup.cpp)
static const void* sLiveRawGroup = nullptr;
static unsigned sLiveRawEpoch = 0;
static unsigned sLiveSnapAsks = 0, sLiveSnapHits = 0, sLiveSnapBad = 0;
// GXV_UPD_LIVESET (update25; exact; default 0, platform/src/port/perf.cpp port_upd_liveset_mode): the membership test of the drawn turn's
// flushes and of par-anim's batches (port_live_actor_valid: ~2 asks per listed model, 400-600 a picture) was a binary search over ~2000
// sorted pointers: ~11 dependent loads and branches, mispredicted half of the time. The same set is kept in an open-addressing table
// (power of two slots, at most half full, one load for a hit): built with the sorted copy, so it answers for exactly those entries.
// 1: on. 2: verify (every ask is answered both ways; the binary search's answer is used; mismatches counted and logged).
extern "C" int port_upd_liveset_mode(void);
extern "C" int port_upd_prefetch2_on(void);  // (GXV_UPD_PREFETCH2 asks "is this one of the scene's actors" before it reads an object's pointers)
extern "C" int port_upd_prefetch3_on(void);  // (GXV_UPD_PREFETCH3 (update26) asks the same, so it needs the table too)
static port::PtrSet< LiveActor, PortHostVector< LiveActor* > > sLiveHash;  // (port/live_set.h, host heap)
PORT_HEAP_GUARD(sLiveHash.slots);
static unsigned sLiveSetAsks = 0, sLiveSetBad = 0;
static void LiveHashBuild() {
    sLiveHash.Build(sLiveSnapshot.data(), sLiveSnapshot.size());
}
void port_live_actor_snapshot() {
    if (!MR::isExistSceneObj(SceneObj_AllLiveActorGroup)) {
        sLiveSnapshot.clear();
        sLiveRawValid = false;
        sLiveHash.Clear();
        return;
    }
    AllLiveActorGroup* group = MR::getAllLiveActorGroup();
    const s32 n = group->getObjNum();
    if (port_live_snap_on()) {
        // GXV_HOTPROF_LIVESNAP (hotprof25, from the console's execution-count profile: 66,458 calls and 64.4 M entry compares in a 12 minute
        // run, i.e. 3.6 walks of the whole group (~970 actors on average, ~2000 in a big stage) a picture): the compare below is answered
        // from the groups' write count instead. sLiveRaw was the copy of this group's array when the count was sLiveRawEpoch; every write
        // of any NameObjGroup array moves the count (NameObjGroup.cpp: the constructor's initObjArray and registerObj are the only two), so
        // with the same group object and the same count the array is what it was: the walk would find every entry equal. A group made
        // anew at the same address moved the count in its constructor. 0: the walk, as before (default until a verify run). 1: the count. 2: verify (both;
        // the walk's answer is used; "live-snap:" lines count the asks and the disagreements).
        const int epochMode = port_hotprof_livesnap_mode();
        const bool epochSame = epochMode != 0 && sLiveRawValid && sLiveRawGroup == group && sLiveRawEpoch == gPortNameObjGroupEpoch &&
                               static_cast< s32 >(sLiveRaw.size()) == n;
        bool same;
        if (epochMode == 1 && epochSame) {
            same = true;
        } else {
            same = sLiveRawValid && static_cast< s32 >(sLiveRaw.size()) == n;
            for (s32 i = 0; same && i < n; ++i) {
                same = sLiveRaw[i] == group->getActor(i);
            }
            if (epochMode == 2) {
                ++sLiveSnapAsks;
                if (epochSame && !same) {
                    ++sLiveSnapBad;
                    if (sLiveSnapBad <= 8) {
                        port_log_summary("live-snap: MISMATCH: the write count says unchanged, the walk says changed (%d entries)", static_cast< int >(n));
                    }
                }
                if (epochSame) {
                    ++sLiveSnapHits;
                }
                if ((sLiveSnapAsks & 0x3FF) == 0) {
                    port_log_summary("live-snap: GXV_HOTPROF_LIVESNAP=2: %u asks, %u answered by the write count, %u mismatches", sLiveSnapAsks,
                                     sLiveSnapHits, sLiveSnapBad);
                }
            }
        }
        if (same) {
            sLiveRawGroup = group;  // (the copy equals this group's array as of this count)
            sLiveRawEpoch = gPortNameObjGroupEpoch;
            if (!sLiveHash.Built() && (port_upd_liveset_mode() != 0 || port_upd_prefetch2_on() != 0 || port_upd_prefetch3_on() != 0)) {
                LiveHashBuild();  // (the table is made from the same sorted copy; it follows it from here on)
            }
            return;  // (sLiveSnapshot is the sorted copy of exactly these entries)
        }
        sLiveRaw.resize(n > 0 ? static_cast< size_t >(n) : 0);
        for (s32 i = 0; i < n; ++i) {
            sLiveRaw[i] = group->getActor(i);
        }
        sLiveRawValid = true;
        sLiveRawGroup = group;
        sLiveRawEpoch = gPortNameObjGroupEpoch;
    } else {
        sLiveRawValid = false;
    }
    sLiveSnapshot.clear();
    sLiveSnapshot.reserve(n);
    for (s32 i = 0; i < n; ++i) {
        sLiveSnapshot.push_back(group->getActor(i));
    }
    std::sort(sLiveSnapshot.begin(), sLiveSnapshot.end());
    if (port_upd_liveset_mode() != 0 || port_upd_prefetch2_on() != 0 || port_upd_prefetch3_on() != 0) {
        LiveHashBuild();
    } else {
        sLiveHash.Clear();
    }
}
// The scene is being destroyed (GameSystemSceneController::exeDestroyScene, after the drop flushes): its actors are gone, so no table of them is kept
// (a new object at one of their addresses must not be taken for an actor), and the next snapshot is made from the new scene's group.
extern "C" void port_undrawn_scene_drop(void);  // (GXV_UNDRAWN_LEAN: its exported-joint set and left-out products are the scene's)
extern "C" void port_live_set_forget(void) {
    port_undrawn_scene_drop();
    sLiveHash.Clear();
    sLiveRawValid = false;
}
// GXV_UNDRAWN_LEAN_HASH (undrawn26, perf.cpp): the gameplay state of every live actor, in the group's order, for diffing two runs: position, velocity,
// rotation, the nerve and its step, the dead / clipped bits, every hit sensor's position, and the game's random seed. Pictures-only state (joints,
// materials, shadows, particles) is not in it.
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemObjHolder.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include "port/undrawn_lean.h"
extern "C" uint64_t port_undrawn_state_hash(unsigned* actors) {
    port::undrawn::Fnv64 h;
    unsigned count = 0;
    if (MR::isExistSceneObj(SceneObj_AllLiveActorGroup)) {
        AllLiveActorGroup* group = MR::getAllLiveActorGroup();
        const s32 n = group->getObjNum();
        for (s32 i = 0; i < n; ++i) {
            const LiveActor* a = group->getActor(i);
            if (a == nullptr) {
                continue;
            }
            ++count;
            h.F32(a->mPosition.x);
            h.F32(a->mPosition.y);
            h.F32(a->mPosition.z);
            h.F32(a->mVelocity.x);
            h.F32(a->mVelocity.y);
            h.F32(a->mVelocity.z);
            h.F32(a->mRotation.x);
            h.F32(a->mRotation.y);
            h.F32(a->mRotation.z);
            h.U32(a->mFlag.mIsDead ? 1u : 0u);
            h.U32(a->mFlag.mIsClipped ? 1u : 0u);
            if (a->mSpine != nullptr) {
                h.U32(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(a->mSpine->mCurrNerve) >> 2));
                h.U32(static_cast< uint32_t >(a->mSpine->mStep));
            }
            if (a->mSensorKeeper != nullptr && a->mSensorKeeper->mSensorInfos != nullptr) {
                // (crash33, 14 Nov: the three console deaths of c106 with GXV_UNDRAWN_LEAN_HASH=300, both dumps at this load.) mSensorCount is the
                // table's CAPACITY (HitSensorKeeper's constructor fills it with nulls); mSensorInfosSize is how many sensors were added. An actor
                // made for more sensors than it adds (Honeyhive has one), or one the loading thread is still building, has null entries.
                const HitSensorKeeper* sk = a->mSensorKeeper;
                const s32 sensors = sk->mSensorInfosSize < sk->mSensorCount ? sk->mSensorInfosSize : sk->mSensorCount;
                for (s32 k = 0; k < sensors; ++k) {
                    const HitSensorInfo* si = sk->mSensorInfos[k];
                    const HitSensor* sn = si != nullptr ? si->mSensor : nullptr;
                    if (sn == nullptr) {
                        continue;
                    }
                    h.F32(sn->mPosition.x);
                    h.F32(sn->mPosition.y);
                    h.F32(sn->mPosition.z);
                }
            }
        }
    }
    h.U32(SingletonHolder< GameSystem >::get()->mObjHolder->mRandom.mSeed);
    if (actors != nullptr) {
        *actors = count;
    }
    return h.h;
}
// GXV_UPD_PREFETCH2 (NameObjCategoryList.cpp): is this object one of the actors of the snapshot (so its LiveActor fields can be read)? A list
// can hold a NameObj that is not a LiveActor (FlashingCtrl, ...): its short block is not asked about. False while no table is built.
extern "C" int port_live_actor_known(const void* p) {
    return sLiveHash.Built() && sLiveHash.Has(static_cast< const LiveActor* >(p)) ? 1 : 0;
}
bool port_live_actor_valid(const LiveActor* p) {
    const int mode = port_upd_liveset_mode();
    if (mode == 0 || !sLiveHash.Built()) {
        return std::binary_search(sLiveSnapshot.begin(), sLiveSnapshot.end(), const_cast< LiveActor* >(p));
    }
    if (mode == 1) {
        return sLiveHash.Has(p);
    }
    const bool plain = std::binary_search(sLiveSnapshot.begin(), sLiveSnapshot.end(), const_cast< LiveActor* >(p));
    const bool fast = sLiveHash.Has(p);
    ++sLiveSetAsks;
    if (plain != fast) {
        ++sLiveSetBad;
        if (sLiveSetBad <= 8) {
            port_log_summary("live-set: MISMATCH asking for %p: binary search %d, table %d (%u entries)", static_cast< const void* >(p), plain ? 1 : 0,
                             fast ? 1 : 0, static_cast< unsigned >(sLiveSnapshot.size()));
        }
    }
    if ((sLiveSetAsks & 0xFFFFu) == 0) {
        port_log_summary("live-set: %u asks answered both ways, %u mismatches (GXV_UPD_LIVESET=2)", sLiveSetAsks, sLiveSetBad);
    }
    return plain;
}
extern "C" int port_on_frame_thread(void);
namespace {
PortHostVector< LiveActor* > sStaleAnim;  // (host heap: port/host_vector.h)
PORT_HEAP_GUARD(sStaleAnim);
float MaxAbsScale(const TVec3f& s) {
    const float x = s.x < 0.0f ? -s.x : s.x, y = s.y < 0.0f ? -s.y : s.y, z = s.z < 0.0f ? -s.z : s.z;
    return x > y ? (x > z ? x : z) : (y > z ? y : z);
}
bool AnimLodDefersFar(const LiveActor* pActor) {
    if (!port_on_frame_thread()) {
        return false;  // (an actor built on the loading thread is computed at once: perf.cpp, port_on_frame_thread)
    }
    // (any turn, drawn or not: the turn that draws next computes the model just before its picture, see port_anim_flush_stale)
    if (pActor->mCollisionParts != nullptr || !port_anim_lod_category()) {
        return false;
    }
    // GXV_ANIM_LOD_FREE (cpuhive, 6 Nov; default 1): a model whose class says nothing but the picture reads its joints (PlantMember:
    // the flowers and bushes of the flower groups) is computed once per picture at any distance. Exact: the picture is the flush's,
    // from the same state; nothing between two pictures reads what is left out (see PlantGroup.cpp).
    if (pActor->mLogicFreeJoints != 0 && port_anim_lod_free_on()) {
        return true;
    }
    if (port::lockstep::On()) {
        // (fmt32: the far and the off-screen deferral go by this console's camera and its own player's distance, and "nothing near the player reads the joints" holds for
        // one player only: with a second one nearby a sensor, an effect or a rider bound to a joint would read a pose one console made and the other did not)
        return false;
    }
    const float d2 = port_anim_lod_dist2();
    if (d2 <= 0.0f) {
        return false;
    }
    const TVec3f* player = port_safe_player_pos();
    const TVec3f& p = pActor->mPosition;
    // The player's distance keeps the game's logic (GXV_ANIM_LOD_PDIST; by default the same distance as the camera's, as before).
    if (player != nullptr) {
        const float px = p.x - player->x, py = p.y - player->y, pz = p.z - player->z;
        if (px * px + py * py + pz * pz <= port_anim_lod_pdist2()) {
            return false;
        }
    }
    if (!port_safe_cam_exists()) {
        return false;
    }
    const TVec3f cam = MR::getCamPos();
    const float cx = p.x - cam.x, cy = p.y - cam.y, cz = p.z - cam.z;
    const float c2 = cx * cx + cy * cy + cz * cz;
    if (c2 > d2) {
        return true;
    }
    // GXV_ANIM_LOD_OFFSCREEN (5 Nov): nearer the camera, but all of it outside the view. The radius is the model's own bounding
    // radius (MR::calcModelBoundingRadius, taken once after its first full computation: LiveActor::calcAnim) with a margin for the
    // limbs its animation moves; within 600 units plus that of the camera never (the clipping frustum's near plane is at 500).
    if (!port_anim_lod_offscreen() || !(pActor->mLodRadius > 0.0f)) {
        return false;
    }
    float r = pActor->mLodRadius * 1.25f * MaxAbsScale(pActor->mScale) + 100.0f;
    if (r < 300.0f) {
        r = 300.0f;
    }
    if (c2 <= (600.0f + r) * (600.0f + r)) {
        return false;
    }
    // (zoomclip23: the picture's frustum, the zoomed one while the handheld zoom is in force. Only the timing of the pose depends on it,
    // the flush computes a deferred model before its picture; the game's logic clipping judges with the original view, GXV_ZOOM_LOGIC_VIEW.)
    return MR::getClippingJudge()->isJudgedToClipPictureFrustum(p, r);
}
// GXV_UNDRAWN_LEAN (undrawn26, platform/src/port/perf.cpp, port/undrawn_lean.h): the joints of a model that nothing but the picture reads are made
// once, before the picture (port_anim_flush_stale, as the far models above), whatever the distance: for the turns around a turn without a picture.
// "Nothing but the picture reads": MR::getJointMtx and every other way the logic gets at a model's joint matrices (JointUtil.cpp, ModelUtil.cpp) have
// not been called for this model (an effect bound to a joint, a sensor, a part, a follower, a rider all get their matrix that way, at their init),
// the model has no collision parts, no camera controller, no joint callback (a callback can write game state while the joints are made), and it is
// not the player. GXV_UNDRAWN_LEAN=2 asks the same and computes as always, counting the reads that would have found the joints left out.
extern "C" int port_anim_lod_raw_category(void);  // (perf.cpp)
extern "C" int port_undrawn_lean_joints(void);
extern "C" int port_undrawn_lean_turn_window(void);
extern "C" int port_undrawn_joint_decide(const void* model, const char* owner, int category, int collision, int camera, int callback, int stale, int onFrame, int posed);
bool UndrawnLeanCallbacks(const LiveActor* pActor) {
    // A J3DJoint's callback is set on the model data's joint (shared by the instances of one model); the answer is kept per model data for 64 turns.
    J3DModelData* data = MR::getJ3DModelData(pActor);
    if (data == nullptr) {
        return true;
    }
    struct Ent {
        const void* data;
        u32 turn;
        bool cb;
    };
    static Ent sCache[256];
    Ent& e = sCache[(reinterpret_cast< uintptr_t >(data) >> 4) & 255];
    if (e.data != data || gPortAlodTurn - e.turn >= 64u) {
        bool cb = false;
        const u16 n = data->mJointTree.mJointNum;
        for (u16 i = 0; i < n && !cb; ++i) {
            const J3DJoint* j = data->mJointTree.mJointNodePointer[i];
            cb = j != nullptr && const_cast< J3DJoint* >(j)->getCallBack() != nullptr;
        }
        e.data = data;
        e.turn = gPortAlodTurn;
        e.cb = cb;
    }
    return e.cb;
}
bool AnimLodDefersRaw(const LiveActor* pActor) {
    // GXV_HRW (hrw28): the models of a pair's set (tagged in its first picture, PortHrwSet.cpp) are posed in the first turn's calc-anim, whatever their distance and whatever the lean turns do: the second
    // picture draws them from that pose. The tag is cleared at the start of the second turn, so the second turn's calc-anim defers them as it always did (nothing else changes).
    if (__builtin_expect(pActor->mModelManager != nullptr && pActor->mModelManager->mModel != nullptr && pActor->mModelManager->mModel->mPortHrwTag != 0, 0)) {
        return false;
    }
    if (AnimLodDefersFar(pActor)) {
        return true;
    }
    if (!port_undrawn_lean_joints() || !port_undrawn_lean_turn_window() || pActor->mModelManager == nullptr) {
        return false;
    }
    J3DModel* pModel = MR::getJ3DModel(pActor);
    if (pModel == nullptr) {
        return false;
    }
    // MP3 lodgate: a sensor actor is not deferred in a held session whatever this rule says, so it is not asked (port_undrawn_joint_decide books stats / the verify counts). No log line here:
    // nothing is known to be overruled (the rule is not asked); AnimLodDefers logs when it overrules a deferral that was decided.
    if (pActor->mSensorKeeper != nullptr && pActor->mSensorKeeper->mSensorCount > 0 && port_net_session_lockstep()) {
        return false;
    }
    return port_undrawn_joint_decide(pModel, pActor->mName, port_anim_lod_raw_category(), pActor->mCollisionParts != nullptr, pActor->mCameraCtrl != nullptr,
                                     UndrawnLeanCallbacks(pActor), pActor->mStaleAnim, port_on_frame_thread(), pActor->mCrPosed != 0) != 0;
}
// MP3 lodgate (D6): in a held session (active or reconnecting) an actor with a hit sensor is never deferred: the far rule and the lean rule decide by THIS console's camera,
// player distance and picture pacing, and a sensor that follows a joint would sit on a different pose here than on the other console. AnimLodDefers makes the session test only for an
// actor the rule would defer (a console outside a session pays nothing extra), and logs "far animation deferral held" only when it overrules such a deferral; AnimLodDefersRaw does its
// own session test for a sensor actor before the lean rule, which is not asked there.
// MP3 lodgate (D7): GXV_STARBATCH_FAR leaves a far star bit's pose out by the local camera's distance: not in a held session. c181's !port::lockstep::On() on the calcAnim line (the same session
// flag, latched per turn) already holds it, so there is no separate 3.0 test there (merge: the line is c181's exactly, which keeps its PGO profile).
bool AnimLodDefers(const LiveActor* pActor) {
    if (!AnimLodDefersRaw(pActor)) {
        return false;
    }
    if (!port_net_session_lockstep()) {
        return true;
    }
    if (!port::mpgate::AnimDeferAllowed(true, pActor->mSensorKeeper != nullptr ? pActor->mSensorKeeper->mSensorCount : 0)) {
        port_mpgate_note(port::mpgate::kAnimLod);
        return false;
    }
    return true;
}
// GXV_PB_ANIM (Performance-mode visual budget; 2000 units in Performance mode, 0 = off): at 60 pictures every turn draws, so the
// deferral above (once per picture) saves nothing there. A model of the same set (map objects, NPCs, decorations and the environment
// with no collision of their own) farther than the distance from BOTH the camera and the player makes its joints every second turn
// only (half the actors on each turn: the phase is taken from the actor's address); on the other turns it keeps its last pose and
// the view pass still runs with the new camera. The pose is at most one turn (1/60 s) old, the BCK clock keeps its pace (the latch is
// cleared as a made calcAnm clears it, see calcAnim), nothing near Mario is touched. The first call of an actor is always made.
// GXV_FARANIM=<n> (cliprange25; 2..4; default 0 = off): the budget's every-2nd-turn pose above is extended to every n-th for a model that is
// far from the camera AND the player (GXV_CR_NEAR, 2500, plus its own radius), SMALL on the picture (its model radius, x1.25, under GXV_FARANIM_LINES
// (30) lines of 456), with no collision of its own, no hit sensor (GXV_FARANIM_SENS=1: with; a sensor that follows a joint would see the old pose),
// no camera controller, not a flower / bush member (they have their own path) and that has not moved: its position against the base matrix of its
// last pose under half a line on the picture (a decoration that travels is posed every turn). The other turns keep the last pose (the BCK clock
// keeps its pace: PbAnimSkips' caller releases the latch as for the every-2nd rule), at most n-1 in a row, the phase spread by address. Returns -1: not
// eligible (the budget's own rule decides), 0: pose, 1: leave this turn's pose out.
int CrFarAnim(LiveActor* a, int n, float zoom) {
    if (a->mLogicFreeJoints != 0 || a->mCollisionParts != nullptr || a->mCameraCtrl != nullptr || !(a->mLodRadius > 0.0f) ||
        (!port_cr_faranim_sens() && a->mSensorKeeper != nullptr && a->mSensorKeeper->mSensorCount > 0)) {
        a->mCrSkip = 0xFF;
        return -1;
    }
    const TVec3f* player = port_safe_player_pos();
    J3DModel* pModel = MR::getJ3DModel(a);
    if (player == nullptr || pModel == nullptr) {
        a->mCrSkip = 0xFF;
        return -1;
    }
    const TVec3f cam = MR::getCamPos();
    const TVec3f& p = a->mPosition;
    const float r = a->mLodRadius * 1.25f * MaxAbsScale(a->mScale);
    const float nearD = port_cr_near() * zoom + r;
    const float cx = p.x - cam.x, cy = p.y - cam.y, cz = p.z - cam.z, px = p.x - player->x, py = p.y - player->y, pz = p.z - player->z;
    const float camD2 = cx * cx + cy * cy + cz * cz;
    if (!(camD2 > nearD * nearD) || !(px * px + py * py + pz * pz > nearD * nearD)) {
        a->mCrSkip = 0xFF;
        return -1;
    }
    static float sFov = -1.0f, sTan = 0.0f;  // (the field of view hardly ever changes: one tanf per change)
    const float fov = MR::getFovy();
    if (fov != sFov) {
        sFov = fov;
        sTan = std::tan(fov * 0.5f * 3.14159265f / 180.0f);
    }
    const float camD = std::sqrt(camD2);
    if (!(port::cliprange::ProjLines(r, camD, sTan) < port_cr_faranim_lines())) {
        a->mCrSkip = 0xFF;
        return -1;
    }
    const float dx = p.x - pModel->mBaseTransformMtx[0][3], dy = p.y - pModel->mBaseTransformMtx[1][3], dz = p.z - pModel->mBaseTransformMtx[2][3];
    if (port::cliprange::MovedTooMuch(dx * dx + dy * dy + dz * dz, camD, sTan, 0.5f)) {
        a->mCrSkip = 0;  // (posed now; the count starts again)
        PosePhaseNote(a, 1, 0);
        return 0;
    }
    // (ratecut31, GXV_POSE_PHASE: on the skip turn only; mode 0 is FarAnimSkip itself)
    const bool skip = port::posephase::Cadence(a->mCrSkip, n, static_cast< uint32_t >(reinterpret_cast< uintptr_t >(a) >> 4), port_pose_phase_mode(), port_pose_phase_skip != 0);
    if (skip) {
        port_logic_count(11, 1);  // (the budget's "calcAnims left out" counter)
    }
    PosePhaseNote(a, 1, skip ? a->mCrSkip : 0);
    return skip ? 1 : 0;
}
// GXV_CHARPOSE=<n> (cpucuts25; 2..4; default 0 = off): an Enemy or Item model (the calc-anim lists the budget leaves out) that is far from the camera AND the player
// (GXV_CHARPOSE_DIST, 2500, plus its radius), under GXV_CHARPOSE_LINES (12) lines on the picture (its bounding radius x1.25) and that has not moved more than
// GXV_CHARPOSE_MOVE (1) lines on the picture since its last pose (port::cpucuts::CharPoseSkip) is posed every n-th turn only. The other turns keep the last pose
// and the view pass still places it for the turn's camera; its movement, hit sensors, effects and animation clock are untouched (the caller releases the latch as
// the budget's every-2nd rule does: the frame advances by one on every movement, so the turn that poses shows the frame the game would show on that turn,
// tests/cpucuts.cpp ClockModel). A sensor that follows a joint reads a pose at most n-1 turns old, 2500+ units from Mario. Returns true: leave the pose out.
bool CpCharPose(LiveActor* a, int n, float zoom) {
    if (a->mLogicFreeJoints != 0 || a->mCollisionParts != nullptr || a->mCameraCtrl != nullptr || a->mCrPosed == 0) {
        a->mCrSkip = 0xFF;
        return false;
    }
    J3DModel* pModel = MR::getJ3DModel(a);
    const TVec3f* player = port_safe_player_pos();
    if (pModel == nullptr || player == nullptr) {
        a->mCrSkip = 0xFF;
        return false;
    }
    if (a->mLodRadius < 0.0f) {  // (taken once, from the joints a full computation made: as AnimLodTakeRadius)
        f32 r = 0.0f;
        MR::calcModelBoundingRadius(&r, a);
        const float s = MaxAbsScale(a->mScale);
        a->mLodRadius = (r > 0.0f && r < 100000.0f && s > 0.001f) ? r / s : 0.0f;
    }
    if (!(a->mLodRadius > 0.0f)) {
        a->mCrSkip = 0xFF;
        return false;
    }
    const TVec3f cam = MR::getCamPos();
    const TVec3f& p = a->mPosition;
    const float r = a->mLodRadius * 1.25f * MaxAbsScale(a->mScale);
    const float nearD = std::sqrt(port_cp_dist2()) * zoom + r;
    const float cx = p.x - cam.x, cy = p.y - cam.y, cz = p.z - cam.z, px = p.x - player->x, py = p.y - player->y, pz = p.z - player->z;
    const float camD2 = cx * cx + cy * cy + cz * cz;
    if (!(camD2 > nearD * nearD) || !(px * px + py * py + pz * pz > nearD * nearD)) {
        a->mCrSkip = 0xFF;
        return false;
    }
    static float sFov = -1.0f, sTan = 0.0f;
    const float fov = MR::getFovy();
    if (fov != sFov) {
        sFov = fov;
        sTan = std::tan(fov * 0.5f * 3.14159265f / 180.0f);
    }
    const float camD = std::sqrt(camD2);
    if (!(port::cliprange::ProjLines(r, camD, sTan) < port_cp_lines_for(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(a) >> 4)))) {
        a->mCrSkip = 0xFF;
        return false;
    }
    const float dx = p.x - pModel->mBaseTransformMtx[0][3], dy = p.y - pModel->mBaseTransformMtx[1][3], dz = p.z - pModel->mBaseTransformMtx[2][3];
    // (ratecut31, GXV_POSE_PHASE: on the skip turn only, the displacement gate first; mode 0 is CharPoseSkip itself)
    const bool skip = port::posephase::CharCadence(a->mCrSkip, n, static_cast< uint32_t >(reinterpret_cast< uintptr_t >(a) >> 4), dx * dx + dy * dy + dz * dz, camD, sTan,
                                                   port_cp_move(), port_pose_phase_mode(), port_pose_phase_skip != 0);
    if (skip) {
        port_logic_count(11, 1);  // (the budget's "calcAnims left out" counter)
    }
    PosePhaseNote(a, 2, skip ? a->mCrSkip : 0);
    return skip;
}
// GXV_FAMPOSE=<n> (cpulod26; 2..4; default 0 = off): an actor of a marked family (mFlFam: star bits, coins, KoteBug, Karikari, PackunPetit, bees, the rolling rock)
// that is far from the camera AND the player (GXV_FAMPOSE_DIST plus its radius, 1.25 x the model radius at its scale) and has not moved more than GXV_FAMPOSE_MOVE
// lines on the picture since its last pose is posed every n-th turn only (at most n-1 left out in a row, the phase by address). Its movement, sensors, effects and
// animation clock are untouched (the caller releases the latch as for the budget's every-2nd rule); the view pass still places it for the turn's camera. Unlike
// GXV_CHARPOSE there is no size limit. Never: collision parts, a camera controller, a model never posed, a mirror camera. Returns -1: not eligible (the other
// rules decide), 0: pose, 1: leave this turn's pose out.
int CpulodFamPose(LiveActor* a, int n, float zoom) {
    if (a->mFlFam == 1 || a->mFlFam == 2) {
        if (port_protect_sprites() >= 1) {
            return -1;  // (cpu2-26, GXV_PROTECT_SPRITES: star bits and coins are on the owner's protected list: never posed at a lower rate by this lever)
        }
    }
    if (a->mFlFam == 0 || a->mCrPosed == 0 || a->mCollisionParts != nullptr || a->mCameraCtrl != nullptr || a->mLogicFreeJoints != 0) {
        return -1;
    }
    J3DModel* pModel = MR::getJ3DModel(a);
    const TVec3f* player = port_safe_player_pos();
    if (pModel == nullptr || player == nullptr || !port_safe_cam_exists() || MR::isExistMirrorCamera()) {
        return -1;
    }
    if (a->mLodRadius < 0.0f) {  // (taken once, from the joints a full computation made: as CpCharPose)
        f32 r = 0.0f;
        MR::calcModelBoundingRadius(&r, a);
        const float s = MaxAbsScale(a->mScale);
        a->mLodRadius = (r > 0.0f && r < 100000.0f && s > 0.001f) ? r / s : 0.0f;
    }
    const TVec3f cam = MR::getCamPos();
    const TVec3f& p = a->mPosition;
    const float r = a->mLodRadius > 0.0f ? a->mLodRadius * 1.25f * MaxAbsScale(a->mScale) : 200.0f;
    const float cx = p.x - cam.x, cy = p.y - cam.y, cz = p.z - cam.z, px = p.x - player->x, py = p.y - player->y, pz = p.z - player->z;
    const float camD2 = cx * cx + cy * cy + cz * cz;
    if (!port::cpulod::FarFromBoth(camD2, px * px + py * py + pz * pz, port_cpulod_fampose_dist_for(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(a) >> 4)), zoom, r)) {
        a->mCrSkip = 0xFF;
        return -1;
    }
    port_cpulod_count(4, 1);
    static float sFov = -1.0f, sTan = 0.0f;
    const float fov = MR::getFovy();
    if (fov != sFov) {
        sFov = fov;
        sTan = std::tan(fov * 0.5f * 3.14159265f / 180.0f);
    }
    const float dx = p.x - pModel->mBaseTransformMtx[0][3], dy = p.y - pModel->mBaseTransformMtx[1][3], dz = p.z - pModel->mBaseTransformMtx[2][3];
    if (port::cpulod::MovedTooFar(dx * dx + dy * dy + dz * dz, std::sqrt(camD2), sTan, port_cpulod_fampose_move_for(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(a) >> 4)))) {
        a->mCrSkip = 0;  // (posed now; the count starts again)
        PosePhaseNote(a, 3, 0);
        return 0;
    }
    // (ratecut31, GXV_POSE_PHASE: on the skip turn only; mode 0 is cpulod::Skip itself)
    if (port::cpulod::SkipAt(a->mCrSkip, n, static_cast< uint32_t >(reinterpret_cast< uintptr_t >(a) >> 4), port_pose_phase_mode(), port_pose_phase_skip != 0)) {
        port_logic_count(11, 1);  // (the budget's "calcAnims left out" counter)
        port_cpulod_count(5, 1);
        PosePhaseNote(a, 3, a->mCrSkip);
        return 1;
    }
    PosePhaseNote(a, 3, 0);
    return 0;
}
}  // namespace
// GXV_MATLOD=<n> (cpulod26; 2..4; default 0 = off; ModelManager.cpp ModelManager::calcAnim): a model far from the camera AND the player (GXV_MATLOD_DIST plus its
// radius) refreshes its material display list (texture / colour animation frames, TEV colours) only every n-th turn; the frame counters of the material players
// advance in movement, so the next refresh shows the then-current frame. Joints, logic and sensors are untouched. Never: collision parts, a camera controller, a
// model whose list was never refreshed, a demo flight (GXV_PB_DEMO=2), a mirror camera. At most n-1 left out in a row. Returns true: leave this turn's refresh out.
bool port_cpulod_mat_skip(LiveActor* a, ModelManager* mm) {
    const int n = port_cpulod_matlod_n_for(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(a) >> 4));
    if (n >= 2 && (a->mFlFam == 1 || a->mFlFam == 2) && port_protect_sprites() >= 1) {
        mm->mCpDlSkip = 0xFF;  // (cpu2-26, GXV_PROTECT_SPRITES: star bits and coins keep their material refresh every turn)
        mm->mCpDlFar = 0;
        return false;
    }
    if (n < 2 || mm->mCpDlMade == 0 || a->mPortNoCut || a->mCollisionParts != nullptr || a->mCameraCtrl != nullptr || port_pb_demo_off() || MR::isExistMirrorCamera()) {
        mm->mCpDlSkip = 0xFF;
        mm->mCpDlFar = 0;
        return false;
    }
    const TVec3f* player = port_safe_player_pos();
    if (player == nullptr || !port_safe_cam_exists()) {
        mm->mCpDlSkip = 0xFF;
        mm->mCpDlFar = 0;
        return false;
    }
    const TVec3f cam = MR::getCamPos();
    const TVec3f& p = a->mPosition;
    const float r = a->mLodRadius > 0.0f ? a->mLodRadius * 1.25f * MaxAbsScale(a->mScale) : 200.0f;
    const float cx = p.x - cam.x, cy = p.y - cam.y, cz = p.z - cam.z, px = p.x - player->x, py = p.y - player->y, pz = p.z - player->z;
    if (!port::cpulod::FarFromBoth(cx * cx + cy * cy + cz * cz, px * px + py * py + pz * pz, port_cpulod_matlod_dist_for(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(a) >> 4)), port_pb_scale(), r)) {
        mm->mCpDlSkip = 0xFF;  // (near again: the next refresh is made at once)
        mm->mCpDlFar = 0;
        return false;
    }
    mm->mCpDlFar = 1;
    port_cpulod_count(0, 1);
    // ratecut31, GXV_POSE_PHASE (port/pose_phase.h): left out on the skip turn only (a model of 60 Assist's pair set never: its second picture draws this
    // turn's lists); mode 0 is cpulod::Skip itself. Census: a list still listed for the drawn turn's flush (mStaleDL) is made there, from the picture's state.
    const int ppMode = port_pose_phase_mode();
    if (ppMode != 0 && PosePhaseTagged(a)) {
        mm->mCpDlSkip = 0xFF;
        PosePhaseNote(a, 4, 0);
        return false;
    }
    if (port::cpulod::SkipAt(mm->mCpDlSkip, n, static_cast< uint32_t >(reinterpret_cast< uintptr_t >(a) >> 4), ppMode, port_pose_phase_skip != 0)) {
        port_cpulod_count(1, 1);
        PosePhaseNote(a, 4, mm->mStaleDL ? 0 : mm->mCpDlSkip);
        return true;
    }
    PosePhaseNote(a, 4, 0);
    return false;
}
#include "Game/Animation/XanimeCore.hpp"  // (blendfix31: the core's left-out record, gPortBlendFix)
namespace {
// blendfix31 (17 Nov 2026; Game/Animation/XanimeCore.hpp, tests/blendfix.cpp): GXV_POSE_BLENDFIX. A pose cut that leaves a calc-anim out left the
// core's last pose (_28) a turn or more old; a change of animation right after it started its blend from that old pose (XanimeCore::doFreeze ->
// the freeze copy), for every cut below and for every actor kind that changes animation. 1 (default, a bug fix of the shipped cuts): the left-out
// calcAnm's inputs are recorded in the core (XanimePlayer::portLeaveOut) and its joint state (no matrices) is made only when something reads it: a
// change of animation, a computation that reads the last pose, freezeCopy. 2: measure only (made at a change, compared with the old pose for the
// census, put back: the game runs as with 0). 0: as before (the latch released only). GXV_POSE_BLENDFIX_CENSUS=1 (or mode 2): the "pose-blendfix:"
// line every 600 turns.
int BlendFixMode() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_POSE_BLENDFIX");
        if (e == nullptr || e[0] == 0 || e[1] != 0) return 1;
        return e[0] == '0' ? 0 : e[0] == '2' ? 2 : 1;
    }());
}
// A pose cut leaves this actor's calcAnm out (every rule in LiveActor::calcAnim): the latch released as the made calcAnm releases it, and with
// GXV_POSE_BLENDFIX the inputs recorded.
inline void PortPoseLeftOut(XanimePlayer* xp) {
    const int mode = BlendFixMode();
    if (mode == 0) {
        xp->_88 = false;
        return;
    }
    gPortBlendFix = static_cast< u8 >(mode);
    xp->portLeaveOut();
}
}  // namespace
// The turn's start (perf.cpp port_undrawn_turn_begin): the census line.
extern "C" void port_pose_blendfix_turn_begin(void) {
    static u32 sTurns = 0;
    static const bool sCensus = [] {
        const char* e = getenv("GXV_POSE_BLENDFIX_CENSUS");
        return e != nullptr && e[0] == '1';
    }();
    const int mode = BlendFixMode();
    if (mode == 0 || !(sCensus || mode == 2)) {
        return;
    }
    gPortBlendFixMeasure = 1;
    if (++sTurns < 600) {
        return;
    }
    sTurns = 0;
    const XanimePortBlendCensus& c = gPortBlendCensus;
    port_log_summary("pose-blendfix: GXV_POSE_BLENDFIX=%d, 600 turns: left out %u (recorded); owed state made: at a change %u (blend start differed %u, "
                     "max %.2f deg, mean %.2f deg, max %.2f units), before a computation that reads it %u, chained %u, at once %u, freezeCopy %u; made by "
                     "the flush %u; overwritten unread %u; shared cores %u; no memory %u%s",
                     mode, c.leftOut, c.atChange, c.changed, static_cast< double >(c.maxDeg),
                     c.changed != 0 ? c.sumDeg / static_cast< double >(c.changed) : 0.0, static_cast< double >(c.maxTrans), c.atRead, c.chained, c.eager,
                     c.atCopy, c.paid, c.cleared, c.shared, c.noMemory, mode == 2 ? " (measure only: put back)" : "");
    memset(&gPortBlendCensus, 0, sizeof(gPortBlendCensus));
}
namespace {
// GXV_MP_DET_LOD (default 0 = off): deterministic MP lockstep narrow gate for strictly visual-only actors (mLogicFreeJoints != 0,
// proven never read by simulation/logic/collision/sensors/callbacks). Keeps all non-mLogicFreeJoints actors 100% blocked under lockstep.
bool MpDetLodOn() {
    return PORT_HOT_KNOB(bool, [] {
        const char* e = getenv("GXV_MP_DET_LOD");
        return e != nullptr && e[0] == '1' && e[1] == '\0';
    }());
}
struct MpDetLodCensus {
    u32 turns = 0;
    u32 pbAllowed = 0, pbSkipped = 0, pbBlocked = 0;
};
MpDetLodCensus sMpDetLod;

bool PbAnimSkipsInner(LiveActor* pActor) {
    if ((pActor->mFlFam == 1 || pActor->mFlFam == 2) && port_protect_sprites() >= 2) {
        pActor->mPbHalf = 0;  // (cpu2-26, GXV_PROTECT_SPRITES=2: star bits and coins are posed every turn whatever the budget says)
        return false;
    }
    if (pActor->mAlodRate > 1) {

        pActor->mPbHalf = 0;  // (GXV_ALOD: an actor moving at half or quarter rate computes its joints on each of its movements)
        return false;
    }
    // (popin24, GXV_POPIN_ZOOM: the budget's distance is the one seen at zoom 1.0; under the handheld zoom a far model is drawn larger, so
    // the distance grows with the zoom ratio; 1 when off)
    const float popinZoom = port_pb_scale();  // (popin25: times GXV_PB_DEMO's widening in a flight)
    // GXV_FARLOGIC level 3 (farlogic25): the Enemy and Item calc-anim lists, which the budget leaves out, join its every-second-turn pose
    // beyond GXV_FARLOGIC_POSE_DIST (2500) from the camera and the player (also where the budget itself is off). Logic is not touched.
    const bool flChar = port_farlogic_level() >= 3 && port_farlogic_pose_on() != 0 && !port_anim_lod_category() && port_farlogic_pose_category() != 0;
    // GXV_CHARPOSE (cpucuts25): the same two lists, posed every n-th turn with a size and a movement guard (CpCharPose), logic untouched, no far-logic level needed
    const int cpN = port_cp_n_for(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(pActor) >> 4));
    const bool cpChar = !flChar && cpN >= 2 && !port_anim_lod_category() && port_farlogic_pose_category() != 0;
    float d2 = port_pb_anim_dist2();
    if (flChar && port_farlogic_pose_dist2() > d2) {
        d2 = port_farlogic_pose_dist2();
    }
    if (cpChar && port_cp_dist2() > d2) {
        d2 = port_cp_dist2();
    }
    if (d2 > 0.0f) {
        const float dScaled = port_detail_dist(3, std::sqrt(d2));  // (detail25: the level's camera distance; the player's distance is not scaled)
        d2 = dScaled * dScaled * popinZoom * popinZoom;
        const float lead = port_pb_lead(std::sqrt(d2));  // (popin25, GXV_POPIN_LEAD: a fast camera has the pose made from farther; 0 = as before)
        if (lead > 0.0f) {
            d2 = (std::sqrt(d2) + lead) * (std::sqrt(d2) + lead);
        }
    }
    if (port_pb_demo_off()) {
        pActor->mPbHalf = 0;  // (popin25, GXV_PB_DEMO=2: every pose is made in a flight)
        return false;
    }
    const int ppMode = port_pose_phase_mode();  // (ratecut31, GXV_POSE_PHASE: the cuts below leave a pose out on the skip turn only; port/pose_phase.h)
    if (__builtin_expect(ppMode != 0, 0) && PosePhaseTagged(pActor)) {
        pActor->mPbHalf = 0;  // (a model of 60 Assist's pair set: its second picture draws this turn's pose, so it is posed every turn)
        pActor->mCrSkip = 0xFF;
        return false;
    }
    {
        const int fpN = port_cpulod_fampose_n_for(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(pActor) >> 4));  // (cpulod26, GXV_FAMPOSE: 0 = off; the families the other rules do not reach)
        if (__builtin_expect(fpN >= 2, 0) && !pActor->mStaleAnim && port_on_frame_thread()) {
            const int r = CpulodFamPose(pActor, fpN, popinZoom);
            if (r >= 0) {
                pActor->mPbHalf = 0;
                return r != 0;
            }
        }
    }
    if (d2 <= 0.0f || pActor->mStaleAnim || pActor->mCollisionParts != nullptr || !(port_anim_lod_category() || flChar || cpChar) || !port_on_frame_thread()) {
        return false;
    }
    const TVec3f& p = pActor->mPosition;
    if (!port_safe_cam_exists()) {
        return false;
    }
    const TVec3f cam = MR::getCamPos();
    const float cx = p.x - cam.x, cy = p.y - cam.y, cz = p.z - cam.z;
    bool isFar = cx * cx + cy * cy + cz * cz > d2;
    if (isFar) {
        const TVec3f* player = port_safe_player_pos();
        if (player == nullptr) {
            isFar = false;
        } else {
            const float px = p.x - player->x, py = p.y - player->y, pz = p.z - player->z;
            float pd2 = port_pb_anim_pdist2();
            if (flChar && port_farlogic_pose_dist2() > pd2) {
                pd2 = port_farlogic_pose_dist2();
            }
            if (cpChar && port_cp_dist2() > pd2) {
                pd2 = port_cp_dist2();
            }
            isFar = px * px + py * py + pz * pz > pd2 * popinZoom * popinZoom;
        }
    }
    if (!isFar) {
        pActor->mPbHalf = 0;
        return false;
    }
    if (cpChar) {
        return CpCharPose(pActor, cpN, popinZoom);  // (GXV_CHARPOSE: a character far away is posed every n-th turn or every turn, never the budget's fixed every-2nd)
    }
    {
        const int crN = port_cr_faranim_n();  // (GXV_FARANIM: 0 = off)
        if (__builtin_expect(crN >= 2, 0)) {
            const int r = CrFarAnim(pActor, crN, popinZoom);
            if (r >= 0) {
                return r != 0;
            }
        }
    }
    if (__builtin_expect(ppMode != 0 || port_pose_phase_census_flag != 0, 0)) {
        // (ratecut31, GXV_POSE_PHASE: the alternation on the skip turn: left out where the undrawn turn poses again before the picture; mode 0 is the switch below)
        const bool skip = port::posephase::Alternate(pActor->mPbHalf, static_cast< uint32_t >(reinterpret_cast< uintptr_t >(pActor) >> 4), ppMode, port_pose_phase_skip != 0);
        if (skip) {
            port_logic_count(11, 1);
        }
        PosePhaseNote(pActor, 0, skip ? 1 : 0);
        return skip;
    }
    switch (pActor->mPbHalf) {
    case 1:
        pActor->mPbHalf = 2;
        // Console A/B: restore only the skip owned by the level-3 pose rule.
        // Changing flChar above would also enable CHARPOSE's independent fallback.
        if (flChar && PORT_HOT_KNOB(bool, [] {
                const char* v = std::getenv("GXV_DIAG_FULL_FAR_POSE");
                return v && v[0] == '1' && v[1] == '\0';
            }())) {
#if PORT_LOGGING
            static bool reported = false;
            if (!reported) {
                reported = true;
                const char* model = pActor->mModelManager ? MR::getModelResName(pActor) : nullptr;
                port_log_summary("far-pose-test: restored first level-3 half-rate pose: %s", model ? model : "(unnamed)");
            }
#endif
            return false;
        }
        port_logic_count(11, 1);
        return true;
    case 2:
        pActor->mPbHalf = 1;
        return false;
    default:  // (first far call: made, then alternating, half the actors starting with a skip)
        pActor->mPbHalf = (reinterpret_cast< uintptr_t >(pActor) >> 4) & 1 ? 1 : 2;
        return false;
    }
}
bool PbAnimSkips(LiveActor* pActor) {
    // MP3 lodgate + fmt32: in a lockstep session every non-logic-free actor is posed on its own turn.
    // Behind default-off GXV_MP_DET_LOD=1, actors with mLogicFreeJoints != 0 (visual-only, never read by logic/sensors/collision/callbacks)
    // are allowed through the narrow visual pose gate; all non-mLogicFreeJoints actors remain 100% blocked.
    if (port::lockstep::On()) {
        if (!MpDetLodOn() || pActor->mLogicFreeJoints == 0) {
            port_mpgate_note(port::mpgate::kPose);
            if (MpDetLodOn()) {
                ++sMpDetLod.pbBlocked;
            }
            pActor->mPbHalf = 0;
            return false;
        }
        ++sMpDetLod.pbAllowed;
        const bool skipped = PbAnimSkipsInner(pActor);
        if (skipped) {
            ++sMpDetLod.pbSkipped;
        }
        return skipped;
    }
    return PbAnimSkipsInner(pActor);
}
// The radius GXV_ANIM_LOD_OFFSCREEN tests, once, from the joints a full computation has just made (at scale 1; 0: unusable).
void AnimLodTakeRadius(LiveActor* pActor) {
    if (pActor->mLodRadius >= 0.0f || !port_anim_lod_offscreen() || port_anim_lod_dist2() <= 0.0f || !port_anim_lod_category() ||
        pActor->mCollisionParts != nullptr || !port_on_frame_thread()) {
        return;
    }
    f32 r = 0.0f;
    MR::calcModelBoundingRadius(&r, pActor);
    const float s = MaxAbsScale(pActor->mScale);
    pActor->mLodRadius = (r > 0.0f && r < 100000.0f && s > 0.001f) ? r / s : 0.0f;
}
}  // namespace
// GXV_PAR_LATE (par2, ModelManager.cpp port_par_late_prepare): the base matrix LiveActor::calcAndSetBaseMtx (below) would set now, into
// out, without setting it (false: the actor is taken; its host's matrix is not predicted). Only a prediction: the key decides.
bool port_predict_default_base(const LiveActor* a, MtxPtr out) {
    if (MR::getTaken(a)) {
        return false;
    }
    TPos3f mtx;
    if (a->mRotation.x == 0.0f && a->mRotation.z == 0.0f) {
        MR::makeMtxTransRotateY(mtx, a);
    } else {
        MR::makeMtxTR(mtx, a);
    }
    PSMTXCopy(mtx.toMtxPtr(), out);
    return true;
}
// GXV_PAR_FLUSH (par2, ModelManager.cpp port_par_flush_prepare): the actors port_anim_flush_stale will compute, in its order.
LiveActor* const* port_anim_stale_items(size_t* n) {
    *n = sStaleAnim.size();
    return sStaleAnim.data();
}
// 1.3 (exact): the start of a drawn turn, after the deferred draw-done wait (the front end is past the last picture) and before
// beginRender. A listed far model gets the calcAnim and the view pass the last turn left out, from the same state (no update() has
// run since), so the picture shows it exactly as if it had been computed every turn; the turns in between computed nothing for it.
// The view pass needs the camera's view matrix in j3dSys, as executeCalcViewAndEntryList loads it for the 3D lists; it is restored.
// A model that died, was clipped or hidden since it was listed is skipped (it is not drawn). drop: the scene is going away.
// GXV_FLUSH_PREFETCH (flushcut, perf.cpp port_flush_prefetch_on): the lists of the drawn turn's flushes are long (Honeyhive: ~250 far
// models) and their models are not in any cache after the turns of game logic since the last picture. A prefetch changes no value; it
// starts the loads the loop will make a few steps later: the actor itself (from the list's pointer, never dereferenced here), and, for
// an entry the snapshot says is an actor of this scene, its model manager and model, so that the next steps find them in the cache.
extern "C" int port_flush_prefetch_on(void);
extern "C" void port_draw_prof_count(int what, unsigned n);
extern "C" int port_draw_prof_on(void);
static inline void PortPrefetchActor(const LiveActor* a) {
    const char* p = reinterpret_cast< const char* >(a);
    __builtin_prefetch(p, 0, 3);
    __builtin_prefetch(p + 32, 0, 3);
    __builtin_prefetch(p + 64, 0, 3);
    __builtin_prefetch(p + 96, 0, 3);
}
extern "C" void port_prefetch_actor(const void* actor) { PortPrefetchActor(static_cast< const LiveActor* >(actor)); }
extern "C" void port_prefetch_model(const void* actor) {
    // (an actor of this scene: the caller has checked it against the snapshot, or it is in an active draw-buffer list)
    const LiveActor* a = static_cast< const LiveActor* >(actor);
    const ModelManager* mm = a->mModelManager;
    if (mm == nullptr) {
        return;
    }
    __builtin_prefetch(mm, 0, 3);
    __builtin_prefetch(reinterpret_cast< const char* >(mm) + 32, 0, 3);
    const J3DModel* model = mm->mModel;
    if (model != nullptr) {
        const char* p = reinterpret_cast< const char* >(model);
        __builtin_prefetch(p, 0, 3);
        __builtin_prefetch(p + 32, 0, 3);
        __builtin_prefetch(p + 64, 0, 3);
        __builtin_prefetch(p + 96, 0, 3);
        __builtin_prefetch(model->mMtxBuffer, 0, 3);
    }
}
// gamepar (8 Nov): GXV_FLUSH_OFFSCREEN (exact for the picture and the game; port/turn_prof.h knobs, perf.cpp): a plant (mLogicFreeJoints)
// whose bounding sphere, grown by a margin, lies wholly outside this picture's view pyramid (the camera's view matrix in j3dSys, its fovy
// and aspect, tangents widened by 15 %; no near or far plane: port/view_cone.h, host test tools/test_view_cone.sh) shows nothing in the
// picture: its pose is not needed. Nothing else reads a plant's joints (PlantGroup.cpp), so it is not posed (no joints, no materials) and
// is computed again, from the state of then, at the first picture whose pyramid may hold it. Its view pass still runs (it stays placed).
// The latch rule is kept: a model left out clears mStaleXLatch as a computation would (XanimePlayer::calcAnm clears _88; the deferral
// already cleared it), so the animation clock runs on exactly as before.
// GXV_PLANT_DRAWCULL (with it): the same plant is also not drawn in that picture (DrawBuffer.cpp: left out as a hidden shape is).
namespace {
void PortViewConeMake(port::viewcone::Cone* c) {
    port::viewcone::Make(c, j3dSys.mViewMtx, MR::getFovy(), MR::getAspect());
}
float PortFlushRadius(LiveActor* a) {
    // (the model's bounding radius at scale 1, taken once from a full computation; 0: never left out as off-screen)
    if (a->mLodRadius < 0.0f) {
        return -1.0f;
    }
    if (!(a->mLodRadius > 0.0f)) {
        return -1.0f;
    }
    float r = a->mLodRadius * 1.5f * MaxAbsScale(a->mScale) + 150.0f;
    return r < 300.0f ? 300.0f : r;
}
void PortFlushTakeRadius(LiveActor* a) {
    if (a->mLodRadius >= 0.0f || a->mModelManager == nullptr) {
        return;
    }
    f32 r = 0.0f;
    MR::calcModelBoundingRadius(&r, a);
    const float sc = MaxAbsScale(a->mScale);
    a->mLodRadius = (r > 0.0f && r < 100000.0f && sc > 0.001f) ? r / sc : 0.0f;
}
}  // namespace
// GXV_PAR_PLANT (revisit23, ModelManager.cpp port_par_flush_prepare): the flush's GXV_FLUSH_OFFSCREEN test, made ahead, so that no job is
// made for a plant the flush will leave out. Only a saving of the worker's time: a job is used only if the flush computes the plant.
bool port_flush_plant_cone(port::viewcone::Cone* cone) {
    if (port_flush_offscreen_mode() == 0) {
        return false;
    }
    Mtx saved;
    PSMTXCopy(j3dSys.mViewMtx, saved);
    MR::loadViewMtx();
    PortViewConeMake(cone);
    PSMTXCopy(saved, j3dSys.mViewMtx);
    return true;
}
bool port_flush_plant_off(const port::viewcone::Cone& cone, LiveActor* a) {
    const float r = PortFlushRadius(a);
    return r > 0.0f && port::viewcone::Outside(cone, a->mPosition.x, a->mPosition.y, a->mPosition.z, r);
}
extern "C" void port_par_plant_own(LiveActor* a);  // (GXV_PAR_PLANT: ModelManager.cpp)
extern "C" int port_starbatch_far_on(void);                          // (GXV_STARBATCH_FAR: ActorTurnLod.cpp)
extern "C" int port_starbatch_far_skip(LiveActor* a, unsigned parity, int atFlush);  // (ratecut31: atFlush 1 = the picture's own pose, GXV_POSE_PHASE=1 never leaves it out there)
namespace {
// (GXV_BOWL_POSE, bowl31: below calcAnim's rules)
void BowlFlushBegin();
void BowlFlushEntry(LiveActor* a);
void BowlFlushEnd();
void BowlDrop();
// (GXV_POSE_ONDRAW, ondraw31: below bowl31's)
void OdFlushBegin();
int OdFlushEntry(LiveActor* a);
void OdFlushEnd();
void OdDrop();
bool OdRead(const J3DModel* model, int what);
void OdMakeJoints(LiveActor* a, bool restore);
}  // namespace
extern "C" void port_anim_flush_stale(int drop) {
    PortHostVector< LiveActor* > work;  // (taken out first: what runs below may list actors again)
    work.swap(sStaleAnim);
    PortHostVector< LiveActor* >& sStaleAnimNow = work;
    if (drop) {
        BowlDrop();  // (bowl31: the scene's windows go with it)
        OdDrop();    // (ondraw31: the same)
    } else {
        BowlFlushBegin();  // (bowl31: mode 2's windows end; mode 1's owed poses are in this list)
        OdFlushBegin();    // (ondraw31: the same)
    }
    if (!drop && !sStaleAnimNow.empty()) {
        port_live_actor_snapshot();
        const bool viewPending = port_view_pending() != 0;
        const bool prefetch = port_flush_prefetch_on() != 0;
        unsigned nComputed = 0, nLeftOut = 0, nOff = 0, nFarCull = 0, nSbFar = 0;
        const bool sbFarOn = port_starbatch_far_on() != 0;  // (starbatch26, GXV_STARBATCH_FAR)
        const bool farCullOn = port_cr_drawflush_on() != 0;  // (cpucuts25, GXV_FARDRAW_FLUSH)
        Mtx saved;
        PSMTXCopy(j3dSys.mViewMtx, saved);
        MR::loadViewMtx();
        const size_t count = sStaleAnimNow.size();
        // gamepar: GXV_FLUSH_OFFSCREEN / GXV_PLANT_DRAWCULL (above)
        const bool offOn = port_flush_offscreen_mode() != 0;
        const bool drawCull = offOn && port_plant_drawcull_mode() != 0;  // (GXV_PLANT_DRAWCULL: DrawBuffer.cpp)
        port::viewcone::Cone cone;
        cone.ok = false;
        // detail25 (GXV_DETAIL_PLANT): thinned members of far plant groups are left out like the off-screen ones (below); the budgets of this picture
        const bool thinOn = port::detail::PlantMode() != 0;
        unsigned nThin = 0;
        if (thinOn) {
            port_detail_flush_begin();
        }
        if (offOn || thinOn) {
            PortViewConeMake(&cone);
        }
        if (prefetch) {
            for (size_t k = 0; k < 6 && k < count; ++k) {
                PortPrefetchActor(sStaleAnimNow[k]);
            }
        }
        for (size_t i = 0; i < count; ++i) {
            if (prefetch) {
                if (i + 6 < count) {
                    PortPrefetchActor(sStaleAnimNow[i + 6]);
                }
                if (i + 3 < count && port_live_actor_valid(sStaleAnimNow[i + 3])) {
                    port_prefetch_model(sStaleAnimNow[i + 3]);
                }
            }
            LiveActor* a = sStaleAnimNow[i];
            if (!port_live_actor_valid(a)) {
                ++nLeftOut;
                continue;  // (no longer an actor of this scene: not touched)
            }
            int odMake = 0;  // (GXV_POSE_ONDRAW: 1 the owed pose from the skip turn's state, 3 made already (view only); 0 not this rule's / not drawn)
            if (__builtin_expect(a->mBowlPose >= 0x80 || (a->mBowlPose & 0x18) == 0x18, 0)) {
                if ((a->mBowlPose & 0x10) != 0) {
                    odMake = OdFlushEntry(a);  // (ondraw31: an owed pose; made even if its calc-anim was switched off since: the picture showed it)
                } else {
                    BowlFlushEntry(a);  // (GXV_BOWL_POSE: an owed pose; posed below as a far one is)
                }
                if (odMake == 3) {
                    if (!viewPending) {
                        a->calcViewAndEntry();  // (ondraw31: made in OdFlushEntry or for a reader in the window; its view pass for this picture)
                    }
                    continue;
                }
            }
            if (__builtin_expect(!a->mStaleAnim, 0)) {
                continue;  // (bowl31: an owed pose the next calc-anim made already; no other rule's entry is ever cleared before its flush)
            }
            port_par_plant_own(a);  // (GXV_PAR_PLANT: before anything of it is written; the worker may be reading it)
            a->mStaleAnim = false;
            if (a->mFlag.mIsDead || a->mFlag.mIsClipped || a->mFlag.mIsHiddenModel || (a->mFlag.mIsNoCalcAnim && odMake == 0)) {
                a->mStaleXLatch = false;
                ++nLeftOut;
                continue;
            }
            if (thinOn && a->mModelManager != nullptr && a->mLogicFreeJoints != 0 && a->mDetailIdx != 0xFFFF) {
                // (detail25) a member the level thins out: not posed, not drawn this picture (its pose is kept; it is posed again when it is restored).
                // A change is made unseen (outside the view), or after a wait, a few a turn (port_detail_plant_skip). Only a member posed before.
                bool unseen = false;
                if (cone.ok) {
                    const float r = PortFlushRadius(a);
                    unseen = r > 0.0f && port::viewcone::Outside(cone, a->mPosition.x, a->mPosition.y, a->mPosition.z, r);
                }
                if (port_detail_plant_skip(a, unseen ? 1 : 0) != 0 && a->mLodRadius >= 0.0f && a->mModelManager->mModel != nullptr) {
                    a->mModelManager->mModel->mPortCullPicture = J3DPortPicture() + 1;  // (the picture draw() makes next)
                    a->mStaleXLatch = false;
                    if (!viewPending) {
                        a->calcViewAndEntry();
                    }
                    ++nThin;
                    continue;
                }
            }
            if (__builtin_expect(farCullOn, 0) && a->mCrPosed != 0 && a->mLogicFreeJoints == 0 && a->mModelManager != nullptr && a->mModelManager->mModel != nullptr) {
                // (cpucuts25, GXV_FARDRAW_FLUSH) a far model GXV_FARDRAW stamped as not drawn in the picture draw() makes next, posed before: nobody sees its pose this
                // picture, so it is not posed and its material list is not made; it is posed in the picture it is not stamped in (the clock runs on, as the plants' path)
                const J3DModel* pFar = a->mModelManager->mModel;
                if (pFar->mPortFarCulled != 0 && pFar->mPortCullPicture == J3DPortPicture() + 1) {
                    a->mStaleXLatch = false;
                    if (!viewPending) {
                        a->calcViewAndEntry();
                    }
                    ++nFarCull;
                    continue;
                }
            }
            if (offOn && a->mModelManager != nullptr && a->mLogicFreeJoints != 0) {
                bool leave = false;
                {
                    const float r = PortFlushRadius(a);
                    if (r > 0.0f && port::viewcone::Outside(cone, a->mPosition.x, a->mPosition.y, a->mPosition.z, r)) {
                        leave = true;
                        ++nOff;
                        if (drawCull && a->mModelManager->mModel != nullptr) {
                            a->mModelManager->mModel->mPortCullPicture = J3DPortPicture() + 1;  // (the picture draw() makes next: port_j3d_picture_begin)
                        }
                    }
                }
                if (leave) {
                    a->mStaleXLatch = false;  // (the clock runs on as after a computation: the deferral cleared _88)
                    if (!viewPending) {
                        a->calcViewAndEntry();  // (placed and viewed this picture with the joints it has; not drawn with GXV_PLANT_DRAWCULL)
                    }
                    continue;
                }
            }
            if (__builtin_expect(sbFarOn, 0) && a->mFlFam == 1 && a->mPbHalf == 0 && a->mCrSkip == 0xFF && port_starbatch_far_skip(a, J3DPortPicture(), 1) != 0) {
                // (starbatch26, GXV_STARBATCH_FAR) a far, floating star bit keeps the pose of its last computation on this picture: the view pass places it
                a->mStaleXLatch = false;
                if (!viewPending) {
                    a->calcViewAndEntry();
                }
                ++nSbFar;
                continue;
            }
            const bool lpFlush = __builtin_expect(::port::LogicProfOn(2), 0);  // (update25, GXV_LOGIC_PROF=3 sampled turn: timed under its class)
            if (lpFlush) port_logic_prof_flush_begin(1, a, a->getName());
            ++nComputed;
            if (a->mStaleXLatch && a->mModelManager != nullptr && a->mModelManager->mXanimePlayer != nullptr) {
                a->mModelManager->mXanimePlayer->_88 = true;  // (the left-out calcAnm's frame, _84: see LiveActor::calcAnim)
            }
            if (a->mModelManager != nullptr && a->mModelManager->mXanimePlayer != nullptr && a->mModelManager->mXanimePlayer->mCore->mPortOwed != 0) {
                // (blendfix31: the computation below is the left-out one, made now from the same state (no update() since): its joint state is not made
                // twice; marked for its updateFrame, so that one left out after all stays owed)
                a->mModelManager->mXanimePlayer->mCore->mPortOwed = 2;
            }
            if (__builtin_expect(odMake != 0, 0)) {
                OdMakeJoints(a, false);  // (ondraw31: the base of the skip turn stands; the latch and the record were set above)
            } else {
            a->mStaleXLatch = false;
            a->calcAnim();          // (port_anim_lod_category() is -1 here: nothing defers)
            }
            if ((offOn || thinOn) && a->mLodRadius < 0.0f && a->mLogicFreeJoints != 0) {
                PortFlushTakeRadius(a);  // (gamepar: the radius GXV_FLUSH_OFFSCREEN tests, once, from this full computation)
            }
            if (!viewPending) {
                a->calcViewAndEntry();  // (else the whole view pass follows: port_view_flush)
            }
            if (lpFlush) port_logic_prof_flush_end(0);
        }
        (void)nThin;
        (void)nSbFar;
        if (nFarCull != 0) {
            port_cr_flush_count(nFarCull);
        }
        port_turn_prof_count(PORT_TC_FLUSH_OFF, nOff);
        port_turn_prof_count(PORT_TC_FLUSH_DONE, nComputed);
        PSMTXCopy(saved, j3dSys.mViewMtx);
        if (port_draw_prof_on()) {
            port_draw_prof_count(0, nComputed);
            port_draw_prof_count(1, nLeftOut);
        }
    } else if (drop && !sStaleAnimNow.empty()) {
        // The scene is about to be destroyed (its memory is still there): clear the marks of the actors still in it, so one that
        // outlives the scene is listed again and never stays without its view pass.
        port_live_actor_snapshot();
        for (size_t i = 0; i < sStaleAnimNow.size(); ++i) {
            if (port_live_actor_valid(sStaleAnimNow[i])) {
                sStaleAnimNow[i]->mStaleAnim = false;
            }
        }
    }
    if (!drop) {
        BowlFlushEnd();  // (bowl31: nothing owed is left for the picture)
        OdFlushEnd();    // (ondraw31: the same)
    }
    work.clear();
    if (sStaleAnim.empty() && sStaleAnim.capacity() < work.capacity()) {
        sStaleAnim.swap(work);
    }
}
// cpu34g (14 Nov 2026), GXV_PAIRPOSE (platform/src/port/perf.cpp has the rule's text and the "pairpose:" census): in the drawn turn of a drawn + undrawn pair the
// joints of a model are made only to be made again by the undrawn turn before the next picture. PairPoseLeavesOut answers whether this actor's are left out now.
#include "Game/Util/DemoUtil.hpp"
extern "C" int port_pair_skip_turn;
extern "C" int port_pairpose_mode(void);
extern "C" float port_pairpose_pdist2(void);
extern "C" void port_pair_count(int what, unsigned n);
namespace {
// The turn's gate, once a turn: a game scene with a player, no demo (a demo's camera may follow a joint: it would move every second turn only).
bool PairPoseGate() {
    static u32 sTurn = 0;
    static bool sOpen = false;
    if (sTurn != gPortAlodTurn) {
        sTurn = gPortAlodTurn;
        sOpen = MR::isExistSceneObj(SceneObj_DemoDirector) && MR::isExistSceneObj(SceneObj_MarioHolder) && !MR::isDemoActive() && port_safe_player_pos() != nullptr;
    }
    return sOpen;
}
bool PairPoseLeavesOut(LiveActor* a, int mode) {
    // (the lists whose models are not the player's and not collision objects: 4 Environment, 5 MapObj, 6 NPC, 0x0B MapObjDecoration, 8 Enemy, 0x10 Item; the flush and
    // every call outside a calc-anim list are posed as always)
    const int raw = port_anim_lod_raw_category();
    if (!(raw == 4 || raw == 5 || raw == 6 || raw == 0x0B || raw == 8 || raw == 0x10) || !port_on_frame_thread() || !PairPoseGate()) {
        port_pair_count(9, 1);
        return false;
    }
    if (a->mPairSkip != 0 || a->mStaleAnim || a->mCrPosed == 0) {
        return false;  // (never two in a row; listed for the flush; never posed yet)
    }
    if (a->mCollisionParts != nullptr || a->mCameraCtrl != nullptr) {
        port_pair_count(4, 1);
        return false;
    }
    // (star bits and coins have their own rules; a half-rate actor is posed on its own turns; a plant is the flush's; an every-n-th rule holds mCrSkip / mPbHalf is superseded
    // only for the budget's alternation, which this rule replaces with "the undrawn turn poses")
    if (a->mFlFam == 1 || a->mFlFam == 2 || a->mAlodRate > 1 || a->mLogicFreeJoints != 0 || a->mCrSkip != 0xFF) {
        port_pair_count(5, 1);
        return false;
    }
    const J3DModel* m = a->mModelManager->mModel;
    if (m != nullptr && m->mPortHrwTag != 0) {
        port_pair_count(6, 1);  // (GXV_HRW: a model of the pair's set is drawn in the second picture from this turn's pose)
        return false;
    }
    const TVec3f* player = port_safe_player_pos();
    if (player == nullptr) {
        return false;
    }
    const float px = a->mPosition.x - player->x, py = a->mPosition.y - player->y, pz = a->mPosition.z - player->z;
    if (!(px * px + py * py + pz * pz > port_pairpose_pdist2())) {
        port_pair_count(2, 1);
        return false;
    }
    if (mode < 2 && UndrawnLeanCallbacks(a)) {
        port_pair_count(3, 1);  // (a joint callback may keep state of its own: it would run at half pace)
        return false;
    }
    if (AnimLodDefersFar(a)) {
        port_pair_count(5, 1);  // (a far model is the flush's already, once a picture: nothing to save, and it stays listed for a picture that follows at once)
        return false;
    }
    return true;
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// bowl31 (17 Nov 2026, perf-inventory D4): GXV_BOWL_POSE, the pose of a Beach Bowl fish left out of the skip turn when nothing reads it before the next
// turn makes it again. port/bowl_pose.h has the rule's text and the pure parts (tests/bowl_pose.cpp, tools/test_bowl_pose.sh).
#include "port/bowl_pose.h"
#include "Game/Animation/XanimeCore.hpp"
#include "Game/Map/FishGroup.hpp"
namespace {
namespace bp = ::port::bowlpose;
struct BowlEntry {
    LiveActor* actor;
    J3DModel* model;
    uint32_t off;      // mode 2: the kept products in sBowlArena (joint matrices, base matrix, the core's last local transforms)
    uint16_t joints;
    uint16_t coreJoints;
    uint64_t hash0;    // mode 2: the state hash before the skip turn's calc-anim
};
PortHostVector< BowlEntry > sBowlList;  // the actors of the window (left out, or verified), from the skip turn to the next drawn turn's flush
PortHostVector< float > sBowlArena;
bp::Census sBowl;
port::posephase::Phase sBowlPhase;
bool sBowlSkip = false;
u32 sBowlTurnNo = 0;
unsigned sBowlPrinted = 0;
int BowlMode() {
    return PORT_HOT_KNOB(int, bp::ParseMode(getenv("GXV_BOWL_POSE")));
}
inline int BowlIdx(const LiveActor* a) {
    return bp::ClassOf(a->mBowlPose) == bp::kPenguin ? 1 : 0;
}
// The logic state of a marked actor: what the turn's movement reads and writes. The pose products (joint and base matrices, the core's per-joint
// transforms, the BCK latch _88 the cut releases as calcAnm does, the core's frame ratio and freeze-copy flag _29 that the next calcAnm sets before it reads
// them, Fish's basis memo) are not in it: those are what the cut leaves and the window's readers are compared against.
uint64_t BowlStateHash(const LiveActor* a) {
    bp::Hash h;
    h.Bytes(&a->mPosition, sizeof(TVec3f));
    h.Bytes(&a->mRotation, sizeof(TVec3f));
    h.Bytes(&a->mScale, sizeof(TVec3f));
    h.Bytes(&a->mVelocity, sizeof(TVec3f));
    h.Bytes(&a->mGravity, sizeof(TVec3f));
    h.Bytes(&a->mFlag, sizeof(LiveActorFlag));
    if (a->mSpine != nullptr) {
        h.U32(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(a->mSpine->mCurrNerve)));
        h.U32(static_cast< uint32_t >(a->mSpine->mStep));
    }
    const XanimePlayer* xp = a->mModelManager != nullptr ? a->mModelManager->mXanimePlayer : nullptr;
    if (xp != nullptr) {
        if (xp->_20 != nullptr) {
            h.F32(xp->_20->getFrame());
            h.F32(xp->_20->getRate());
            h.U32(xp->_20->getState());
        }
        h.F32(xp->_84);
        h.F32(xp->_08);
        h.F32(xp->_0C);
        h.Bytes(xp->mWeights, sizeof(xp->mWeights));
        h.U32(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(xp->mCurrentAnimation)));
        h.U32(static_cast< uint32_t >(xp->_78));
        h.U32((xp->_7C ? 1u : 0u) | (xp->_7E ? 2u : 0u) | (xp->_7F ? 4u : 0u) | (xp->_80 ? 8u : 0u));
        const XanimeCore* c = xp->mCore;
        if (c != nullptr) {
            h.F32(c->_1C);
            h.F32(c->_20);
            h.U32(c->_28);
            for (u32 t = 0; t < c->mTrackCount; ++t) {
                h.U32(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(c->mTrackList[t]._0)));
                h.F32(c->mTrackList[t].mWeight);
            }
        }
    }
    if (bp::ClassOf(a->mBowlPose) == bp::kFish) {
        const Fish* f = static_cast< const Fish* >(a);
        h.U32(static_cast< uint32_t >(f->mFishType));
        h.Bytes(&f->mFollowPos, sizeof(TVec3f));
        h.Bytes(&f->mFollowPosOffset, sizeof(TVec3f));
        h.Bytes(&f->mFront, sizeof(TVec3f));
        h.F32(f->mDistanceToApproach);
        h.U32(static_cast< uint32_t >(f->mTimeToSearchPlayer));
        h.U32(static_cast< uint32_t >(f->mStepToMove));
        h.U32(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(f->mEscapePos)));
    }
    h.U32(SingletonHolder< GameSystem >::get()->mObjHolder->mRandom.mSeed);
    return h.h;
}
void BowlMismatch(int cls, int kind, const LiveActor* a) {
    ++sBowl.mismatch[cls][kind];
    if (sBowlPrinted++ < 24) {
        static const char* const kKind[4] = {"the calc-anim changed the state hash", "a joint read saw the new pose", "a base-matrix read saw the new pose",
                                             "the next pose reads the last one (freeze / smoothing) and they differ"};
        port_log_summary("bowl-pose: MISMATCH %s of %s (%s, turn %u)", kKind[kind], a != nullptr && a->mName != nullptr ? a->mName : "?",
                         cls == 0 ? "fish" : "penguin", sBowlTurnNo);
    }
}
void BowlLate(int cls, const char* what, const LiveActor* a) {
    ++sBowl.late[cls];
    if (sBowlPrinted++ < 24) {
        port_log_summary("bowl-pose: LATE %s of %s while its pose was owed (%s, turn %u)", what, a != nullptr && a->mName != nullptr ? a->mName : "?",
                         cls == 0 ? "fish" : "penguin", sBowlTurnNo);
    }
}
BowlEntry* BowlFind(const void* actorOrModel) {
    for (size_t i = sBowlList.size(); i-- > 0;) {
        if (sBowlList[i].actor == actorOrModel || sBowlList[i].model == actorOrModel) {
            return &sBowlList[i];
        }
    }
    return nullptr;
}
// The window ends: the bits go, whoever ends it.
inline void BowlEnd(LiveActor* a) {
    a->mBowlPose &= bp::kClassMask;
    if (a->mModelManager != nullptr && a->mModelManager->mModel != nullptr) {
        a->mModelManager->mModel->mPortBowl &= static_cast< u8 >(~bp::kModelPend);
    }
}
// Mode 2: does the computation about to run read the core's last local transforms? Then compare the kept ones against the ones there now.
void BowlVerifyNext(LiveActor* a, const BowlEntry& e) {
    const XanimePlayer* xp = a->mModelManager != nullptr ? a->mModelManager->mXanimePlayer : nullptr;
    const XanimeCore* c = xp != nullptr ? xp->mCore : nullptr;
    if (c == nullptr || e.coreJoints == 0) {
        return;
    }
    const bool reads = c->_28 != 0 || c->_20 != 1.0f || (c->mTrackCount > 0 && c->mTrackList[0]._0 == nullptr);
    if (!reads) {
        return;
    }
    ++sBowl.late[BowlIdx(a)];
    const float* kept = sBowlArena.data() + e.off + e.joints * 12u + 12u;
    for (u16 j = 0; j < e.coreJoints; ++j) {
        if (memcmp(kept + j * (sizeof(XtransformInfo) / 4), &c->mJointList[j]._28, sizeof(XtransformInfo)) != 0) {
            BowlMismatch(BowlIdx(a), 3, a);
            return;
        }
    }
}
}  // namespace

extern "C" int port_bowl_pose_mode(void) { return BowlMode(); }
// GameSystem::frameLoop's turn start (perf.cpp port_undrawn_turn_begin): the phase, the census line.
extern "C" void port_bowl_pose_turn_begin(int drawn) {
    const int mode = BowlMode();
    if (mode == 0) {
        sBowlSkip = false;
        return;
    }
    ++sBowlTurnNo;
    sBowlSkip = sBowlPhase.Begin(drawn != 0);
    ++sBowl.turns;
    if (sBowlSkip) {
        ++sBowl.skipTurns;
    }
    if (sBowlPhase.Broken(drawn != 0)) {
        ++sBowl.broken;
    }
    if (sBowl.turns >= 600) {
        const bp::Census& c = sBowl;
        char ref[2][160];
        for (int k = 0; k < 2; ++k) {
            snprintf(ref[k], sizeof ref[k], "%u/%u/%u/%u/%u/%u/%u/%u", c.refused[k][0], c.refused[k][1], c.refused[k][2], c.refused[k][3], c.refused[k][4],
                     c.refused[k][5], c.refused[k][6], c.refused[k][7]);
        }
        port_log_summary("bowl-pose: GXV_BOWL_POSE=%d, %u turns, %u skip turns, %u broken pairs; fish: asked %u, %s %u, owed pose made by the next turn %u, by the flush %u, "
                         "not drawn %u, %s %u; penguins: asked %u, %s %u, next %u, flush %u, not drawn %u, %s %u; refused (turn/other-rule/picture/joints-read/callback/"
                         "parts/blend/class) fish %s penguins %s; mismatches (state/joints/base/next-pose) fish %u/%u/%u/%u penguins %u/%u/%u/%u; stale at draw %llu",
                         mode, c.turns, c.skipTurns, c.broken, c.asked[0], mode == 2 ? "would leave out" : "left out", c.leftOut[0], c.posedNext[0], c.posedFlush[0],
                         c.notPosed[0], mode == 2 ? "readers compared" : "LATE reads", c.late[0], c.asked[1], mode == 2 ? "would leave out" : "left out",
                         c.leftOut[1], c.posedNext[1], c.posedFlush[1], c.notPosed[1], mode == 2 ? "readers compared" : "LATE reads", c.late[1], ref[0], ref[1],
                         c.mismatch[0][0], c.mismatch[0][1], c.mismatch[0][2], c.mismatch[0][3], c.mismatch[1][0], c.mismatch[1][1], c.mismatch[1][2],
                         c.mismatch[1][3], static_cast< unsigned long long >(c.staleAtDraw));
        sBowl.Clear();
    }
}
// A read of an owed model's joint matrices (PortBowlPose.hpp: JointUtil.cpp, ModelUtil.cpp) or base matrix (LiveActor::getBaseMtx): what = 1 / 2.
extern "C" void port_bowl_pose_read(const J3DModel* model, int what) {
    if (OdRead(model, what)) {
        return;  // (GXV_POSE_ONDRAW, ondraw31: the window is that rule's)
    }
    BowlEntry* e = BowlFind(model);
    if (e == nullptr || e->actor == nullptr) {
        return;
    }
    LiveActor* a = e->actor;
    if ((a->mBowlPose & bp::kVerifyD) != 0) {
        return;  // (mode 2: the actor's own calc-anim, which the cut does not run at all)
    }
    const int cls = BowlIdx(a);
    if ((a->mBowlPose & bp::kPending) != 0) {
        BowlLate(cls, what == 1 ? "joint matrices read" : "base matrix read", a);
        return;
    }
    if ((a->mBowlPose & bp::kVerify) == 0) {
        return;
    }
    ++sBowl.late[cls];
    const float* kept = sBowlArena.data() + e->off;
    if (what == 1) {
        if (memcmp(kept, model->mMtxBuffer->mpAnmMtx, e->joints * sizeof(Mtx)) != 0) {
            BowlMismatch(cls, 1, a);
        }
    } else if (memcmp(kept + e->joints * 12u, model->mBaseTransformMtx, sizeof(Mtx)) != 0) {
        BowlMismatch(cls, 2, a);
    }
}
namespace {
// The drawn turn's flush (port_anim_flush_stale), before its picture: the windows still open end here. Mode 1's owed poses are in the flush's own list
// (posed by it below, or not drawn); mode 2's are counted. Then nothing may be owed: the census's "stale at draw".
void BowlFlushBegin() {
    if (sBowlList.empty()) {
        return;
    }
    port_live_actor_snapshot();
    for (BowlEntry& e : sBowlList) {
        if (e.actor == nullptr || !port_live_actor_valid(e.actor)) {
            e.actor = nullptr;
            continue;
        }
        LiveActor* a = e.actor;
        if ((a->mBowlPose & bp::kVerify) != 0) {
            const int cls = BowlIdx(a);
            const bool drawn = !(a->mFlag.mIsDead || a->mFlag.mIsClipped || a->mFlag.mIsHiddenModel || a->mFlag.mIsNoCalcAnim);
            ++(drawn ? sBowl.posedFlush[cls] : sBowl.notPosed[cls]);
            BowlEnd(a);
        }
    }
}
// In the flush's loop: an owed pose of mode 1 (listed by PortBowlPose), made by the loop now unless the actor is not drawn.
void BowlFlushEntry(LiveActor* a) {
    if ((a->mBowlPose & bp::kPending) == 0) {
        return;
    }
    const int cls = BowlIdx(a);
    const bool drawn = !(a->mFlag.mIsDead || a->mFlag.mIsClipped || a->mFlag.mIsHiddenModel || a->mFlag.mIsNoCalcAnim);
    ++(drawn ? sBowl.posedFlush[cls] : sBowl.notPosed[cls]);
    BowlEnd(a);
}
void BowlFlushEnd() {
    for (BowlEntry& e : sBowlList) {
        if (e.actor != nullptr && (e.actor->mBowlPose & (bp::kPending | bp::kVerify)) != 0) {
            ++sBowl.staleAtDraw;  // (by construction never: every owed pose was listed for the flush above)
            BowlEnd(e.actor);
        }
    }
    // (every window has ended: the next skip turn starts the list again)
    sBowlList.clear();
    sBowlArena.clear();
}
void BowlDrop() {
    sBowlList.clear();
    sBowlArena.clear();
}
// LiveActor::calcAnim of a marked actor. 0: as before; 1: this turn's pose is left out (mode 1); 2: the pose owed since the skip turn is made now, whatever
// the every-n-th rules say.
int PortBowlPose(LiveActor* a) {
    const int mode = BowlMode();
    if (mode == 0) {
        return 0;
    }
    const int cls = BowlIdx(a);
    if ((a->mBowlPose & bp::kPending) != 0) {
        // (the next turn's calc-anim: a freeze its movement started reads the last pose, which is the one before the skip turn's)
        const XanimePlayer* xp = a->mModelManager->mXanimePlayer;
        if (xp != nullptr && xp->mCore != nullptr && xp->mCore->_28 != 0) {
            BowlLate(cls, "animation change (freeze copy)", a);
        }
        ++sBowl.posedNext[cls];
        a->mStaleAnim = false;  // (the flush passes its entry by)
        a->mPbHalf = 0;
        BowlEnd(a);
        return 2;
    }
    if ((a->mBowlPose & bp::kVerify) != 0) {
        BowlEntry* e = BowlFind(a);
        a->mBowlPose &= static_cast< u8 >(~bp::kVerifyD);  // (the skip turn's own computation did not run to its end: another rule held it)
        if (e != nullptr) {
            BowlVerifyNext(a, *e);
        }
        ++sBowl.posedNext[cls];
        BowlEnd(a);
        return 0;
    }
    if (!sBowlSkip) {
        return 0;
    }
    ++sBowl.asked[cls];
    J3DModel* model = a->mModelManager->mModel;
    const XanimePlayer* xp = a->mModelManager->mXanimePlayer;
    const XanimeCore* core = xp != nullptr ? xp->mCore : nullptr;
    bp::Facts f;
    f.cls = bp::ClassOf(a->mBowlPose);
    f.skipTurn = !port::lockstep::On();
    const int listCat = port_anim_lod_raw_category();  // (a calc-anim list runs: not the flush, not a calcAnim called from a movement)
    f.gate = listCat >= 0 && listCat < 0x100 && port_on_frame_thread() && PairPoseGate();
    f.posedOnce = a->mCrPosed != 0 && model != nullptr;
    f.otherRule = a->mStaleAnim || a->mCrSkip != 0xFF || a->mFlFam != 0 || a->mAlodRate > 1 || a->mLogicFreeJoints != 0 || a->mPairSkip != 0;
    f.collision = a->mCollisionParts != nullptr;
    f.camera = a->mCameraCtrl != nullptr;
    f.hrwTag = model != nullptr && model->mPortHrwTag != 0;
    f.jointsRead = model != nullptr && (model->mPortBowl & bp::kModelRead) != 0;
    f.callback = f.posedOnce && UndrawnLeanCallbacks(a);
    f.parts = a->mSensorKeeper != nullptr || a->mEffectKeeper != nullptr || a->mShadowControllerList != nullptr || a->mStarPointerTarget != nullptr;
    f.blendReadsLast = core != nullptr && (core->_20 != 1.0f || core->_28 != 0 || (core->mTrackCount > 0 && core->mTrackList[0]._0 == nullptr));
    const int why = bp::Refuse(f, mode == 1);
    if (why >= 0) {
        ++sBowl.refused[cls][why];
        return 0;
    }
    ++sBowl.leftOut[cls];
    BowlEntry e;
    e.actor = a;
    e.model = model;
    e.off = 0;
    e.joints = 0;
    e.coreJoints = 0;
    e.hash0 = 0;
    if (mode == 2) {
        // (keep the products the cut would leave: the joint matrices, the base matrix, the core's last local transforms)
        ++sBowl.checks[cls];
        e.joints = model->mModelData->mJointTree.mJointNum;
        e.coreJoints = core != nullptr && core->mJointList != nullptr ? static_cast< u16 >(core->mJointCount) : 0;
        e.off = static_cast< uint32_t >(sBowlArena.size());
        const size_t perCore = sizeof(XtransformInfo) / 4;
        sBowlArena.resize(sBowlArena.size() + e.joints * 12u + 12u + e.coreJoints * perCore);
        float* p = sBowlArena.data() + e.off;
        memcpy(p, model->mMtxBuffer->mpAnmMtx, e.joints * sizeof(Mtx));
        memcpy(p + e.joints * 12u, model->mBaseTransformMtx, sizeof(Mtx));
        for (u16 j = 0; j < e.coreJoints; ++j) {
            memcpy(p + e.joints * 12u + 12u + j * perCore, &core->mJointList[j]._28, sizeof(XtransformInfo));
        }
        e.hash0 = BowlStateHash(a);
        a->mBowlPose |= bp::kVerify | bp::kVerifyD;
        model->mPortBowl |= bp::kModelPend;
        sBowlList.push_back(e);
        return 0;
    }
    // Mode 1: left out. The BCK clock is released as a made calcAnm releases it, the latch kept for the flush (as the far deferral keeps it); listed for the
    // flush (which also keeps the view pass for the picture: calcViewAndEntry passes a listed actor by).
    if (xp != nullptr) {
        a->mStaleXLatch = xp->_88;
        PortPoseLeftOut(a->mModelManager->mXanimePlayer);  // (blendfix31: the inputs recorded with GXV_POSE_BLENDFIX)
    }
    a->mStaleAnim = true;
    sStaleAnim.push_back(a);
    a->mBowlPose |= bp::kPending;
    model->mPortBowl |= bp::kModelPend;
    sBowlList.push_back(e);
    return 1;
}
// Mode 2, at the end of the skip turn's own calc-anim: it must not have changed the actor's logic state.
void PortBowlVerifyAfter(LiveActor* a) {
    a->mBowlPose &= static_cast< u8 >(~bp::kVerifyD);
    const BowlEntry* e = BowlFind(a);
    if (e != nullptr && BowlStateHash(a) != e->hash0) {
        BowlMismatch(BowlIdx(a), 0, a);
    }
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// ondraw31 (18 Nov 2026, plan-cpu31 idea 2): GXV_POSE_ONDRAW, bowl31's rule for every class of the calc-anim lists that are not the player's: on a skip turn
// an actor's joints (ModelManager::calcAnim) and view pass are left out when nothing reads them before the next turn makes them again; its base matrix is
// made as always. port/pose_ondraw.h has the rule's text and the pure parts (tests/ondraw.cpp, tools/test_ondraw.sh). The window bits are bowl31's
// (mBowlPose 0x80 / 0x40 / 0x20, J3DModel::mPortBowl 2) with 0x10 saying the window is this rule's; the read hook, the flush hooks and the verify are
// routed here by that bit.
#include "port/pose_ondraw.h"
#include "Game/Util/MutexHolder.hpp"
#include "Game/Animation/BtkPlayer.hpp"
#include "Game/Animation/BrkPlayer.hpp"
#include "Game/Animation/BtpPlayer.hpp"
#include "Game/Animation/BpkPlayer.hpp"
#include "Game/Animation/BvaPlayer.hpp"
extern "C" int port_dl_lazy_on(void);  // (perf.cpp: GXV_DL_LAZY, 2 by default)
int gPortOnDraw = 0;                 // the mode, written at each turn's start (calcAnim's gate: one load)
const LiveActor* gPortOdTimed = nullptr;  // (the census: the pose being timed)
namespace {
namespace od = ::port::ondraw;
struct OdEntry {
    LiveActor* actor;
    J3DModel* model;
    uint32_t off;        // mode 2: the kept joint matrices and the core's last local transforms in sOdArena
    uint16_t joints, coreJoints;
    uint32_t xhash;      // the inputs of the left-out computation (base, base scale, the BCK clock and latch, the core's inputs): the catch-up's test
    uint32_t bhash;      // the base matrix and base scale alone (the record's catch-up: XanimeCore::portRemake)
    uint32_t hash0;      // mode 2: the logic state before the skip turn's calc-anim (taken after the base when it is reached: PortOnDrawBaseMade)
    uint32_t hashPre;    // mode 2 (fable31fix): the logic state before the base too, kept to report what calcAndSetBaseMtx wrote as its own count
    uint8_t kind, made;  // made: mode 2, the skip turn's own computation ran to its end
    uint8_t remade;      // mode 1: a reader's catch-up made the joints from the record (the materials are still owed)
    J3DAnmBase* mres[5];  // the material and visibility players (BTK, BRK, BTP, BPK, BVA) as the left-out computation found them: resource and frame
    f32 mframe[5];
};
PortHostVector< OdEntry > sOdList;
PortHostVector< float > sOdArena;
od::Census sOd;
port::posephase::Phase sOdPhase;
bool sOdSkip = false;
u32 sOdTurnNo = 0, sOdPrinted = 0, sOdBlendLast = 0, sOdSample = 0;
int64_t sOdT0 = 0;
int OdMode() {
    return PORT_HOT_KNOB(int, od::ParseMode(getenv("GXV_POSE_ONDRAW")));
}
uint32_t OdBlendCatchUps() {
    const XanimePortBlendCensus& c = gPortBlendCensus;
    return c.atChange + c.atRead + c.chained + c.eager + c.atCopy;
}
// The material and visibility players of a model (ModelManager: BTK, BRK, BTP, BPK, BVA). Their frames move in update(), like the BCK clock: the left-out
// computation's are recorded with it, and the "unchanged" test covers them (an actor without a BCK has nothing else that moves).
enum { kOdPlayers = 5, kOdBva = 4 };
AnmPlayerBase* OdPlayer(const ModelManager* mm, int i) {
    switch (i) {
    case 0: return mm->mBtkPlayer;
    case 1: return mm->mBrkPlayer;
    case 2: return mm->mBtpPlayer;
    case 3: return mm->mBpkPlayer;
    default: return mm->mBvaPlayer;
    }
}
void OdHashPlayers(od::Hash& h, const ModelManager* mm) {
    for (int i = 0; i < kOdPlayers; ++i) {
        const AnmPlayerBase* p = OdPlayer(mm, i);
        if (p != nullptr) {
            h.U32(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(p->mAnmRes)));
            h.F32(p->mFrameCtrl.mFrame);
        }
    }
}
void OdRecordPlayers(const LiveActor* a, OdEntry& e) {
    for (int i = 0; i < kOdPlayers; ++i) {
        const AnmPlayerBase* p = OdPlayer(a->mModelManager, i);
        e.mres[i] = p != nullptr ? p->mAnmRes : nullptr;
        e.mframe[i] = p != nullptr ? p->mFrameCtrl.mFrame : 0.0f;
    }
}
// The players' resources are those of the left-out computation (players [from, to)); the frames may have moved since.
bool OdPlayersSame(const LiveActor* a, const OdEntry& e, int from, int to) {
    for (int i = from; i < to; ++i) {
        const AnmPlayerBase* p = OdPlayer(a->mModelManager, i);
        if ((p != nullptr ? p->mAnmRes : nullptr) != e.mres[i]) {
            return false;
        }
    }
    return true;
}
uint32_t OdBaseHash(const LiveActor* a) {
    od::Hash h;
    const J3DModel* m = a->mModelManager->mModel;
    h.Words(m->mBaseTransformMtx, sizeof(Mtx));
    h.Words(&m->mBaseScale, sizeof(Vec));
    return h.h;
}
// What the left-out computation would have used (a read's catch-up is exact while these are unchanged). The core's freeze flag is not in it: the made
// computation consumes it, the left-out one leaves it (blendfix31 makes either the same); an animation change also writes _1C and the tracks.
uint32_t OdXHash(const LiveActor* a) {
    od::Hash h;
    const J3DModel* m = a->mModelManager->mModel;
    h.Words(m->mBaseTransformMtx, sizeof(Mtx));
    h.Words(&m->mBaseScale, sizeof(Vec));
    OdHashPlayers(h, a->mModelManager);
    const XanimePlayer* xp = a->mModelManager->mXanimePlayer;
    if (xp != nullptr) {
        if (xp->_20 != nullptr) {
            h.F32(xp->_20->getFrame());
            h.F32(xp->_20->getRate());
            h.U32(xp->_20->getState());
        }
        h.F32(xp->_84);
        h.U32(xp->_88 ? 1u : 0u);
        h.U32(xp->_80 ? 1u : 0u);
        h.U32(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(xp->mCurrentAnimation)));
        const XanimeCore* c = xp->mCore;
        if (c != nullptr) {
            h.F32(c->_1C);
            h.F32(c->_20);
            h.U32(c->_6);
            for (u32 t = 0; t < c->mTrackCount; ++t) {
                h.U32(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(c->mTrackList[t]._0)));
                h.F32(c->mTrackList[t].mWeight);
                h.F32(c->mTrackList[t]._8);
                h.U32(c->mTrackList[t]._C);
            }
        }
    }
    return h.h;
}
// Mode 2: the logic state (what movement reads and writes). The pose products, the latch _88 (released by the cut as by calcAnm) and the core's freeze
// flags (consumed by the made computation, kept by blendfix31's record) are not in it.
uint32_t OdStateHash(const LiveActor* a) {
    od::Hash h;
    h.Words(&a->mPosition, sizeof(TVec3f));
    h.Words(&a->mRotation, sizeof(TVec3f));
    h.Words(&a->mScale, sizeof(TVec3f));
    h.Words(&a->mVelocity, sizeof(TVec3f));
    h.Words(&a->mGravity, sizeof(TVec3f));
    h.Words(&a->mFlag, sizeof(LiveActorFlag));
    if (a->mSpine != nullptr) {
        h.U32(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(a->mSpine->mCurrNerve)));
        h.U32(static_cast< uint32_t >(a->mSpine->mStep));
    }
    const XanimePlayer* xp = a->mModelManager != nullptr ? a->mModelManager->mXanimePlayer : nullptr;
    if (xp != nullptr) {
        if (xp->_20 != nullptr) {
            h.F32(xp->_20->getFrame());
            h.F32(xp->_20->getRate());
            h.U32(xp->_20->getState());
        }
        h.F32(xp->_84);
        h.F32(xp->_08);
        h.F32(xp->_0C);
        h.Words(xp->mWeights, sizeof(xp->mWeights));
        h.U32(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(xp->mCurrentAnimation)));
        h.U32(static_cast< uint32_t >(xp->_78));
        h.U32((xp->_7C ? 1u : 0u) | (xp->_7E ? 2u : 0u) | (xp->_7F ? 4u : 0u) | (xp->_80 ? 8u : 0u));
        const XanimeCore* c = xp->mCore;
        if (c != nullptr) {
            h.F32(c->_1C);
            h.F32(c->_20);
            for (u32 t = 0; t < c->mTrackCount; ++t) {
                h.U32(static_cast< uint32_t >(reinterpret_cast< uintptr_t >(c->mTrackList[t]._0)));
                h.F32(c->mTrackList[t].mWeight);
            }
        }
    }
    h.U32(SingletonHolder< GameSystem >::get()->mObjHolder->mRandom.mSeed);
    return h.h;
}
// (emufix31) since the start, never cleared: a MISMATCH line and the check that printed it can fall in two census windows (a joint read or the flush
// is a turn after its skip turn); the census line's "since the start" totals always hold both
uint32_t sOdTotChecks = 0, sOdTotMismatch[4] = {0, 0, 0, 0}, sOdTotBaseWrote = 0;
void OdMismatch(int kind, const LiveActor* a, int listKind) {
    ++sOd.mismatch[kind];
    ++sOdTotMismatch[kind];
    if (sOdPrinted++ < 24) {
        static const char* const kKind[4] = {"the calc-anim changed the logic state", "a joint read in the window would have seen the older pose",
                                             "the next computation reads the last pose and it differs", "the picture would be made from a later state (drift)"};
        // (a part, PartsModel / an NPC's goods: its host's name too; names are the game's Shift-JIS)
        const LiveActor* host = a != nullptr ? a->mPortHrwHost : nullptr;
        port_log_summary("pose-ondraw: MISMATCH %s of %s%s%s (%s, turn %u, in the census window that ends at turn %u)", kKind[kind],
                         a != nullptr && a->mName != nullptr ? a->mName : "?", host != nullptr ? ", a part of " : "",
                         host != nullptr ? (host->mName != nullptr ? host->mName : "?") : "", od::KindName(listKind), sOdTurnNo, (sOdTurnNo / 600u + 1u) * 600u);
    }
}
void OdLate(const LiveActor* a, int listKind) {
    ++sOd.late;
    if (sOdPrinted++ < 24) {
        port_log_summary("pose-ondraw: LATE joint read of %s while its pose was owed and its state had changed (%s, turn %u)",
                         a != nullptr && a->mName != nullptr ? a->mName : "?", od::KindName(listKind), sOdTurnNo);
    }
}
OdEntry* OdFind(const void* actorOrModel) {
    for (size_t i = sOdList.size(); i-- > 0;) {
        if (sOdList[i].actor == actorOrModel || sOdList[i].model == actorOrModel) {
            return &sOdList[i];
        }
    }
    return nullptr;
}
inline void OdEnd(LiveActor* a) {
    a->mBowlPose &= static_cast< u8 >(~od::kWinMask);
    if (a->mModelManager != nullptr && a->mModelManager->mModel != nullptr) {
        a->mModelManager->mModel->mPortBowl &= static_cast< u8 >(~bp::kModelPend);
    }
}
// The left-out joints, made now from the skip turn's state (the base and base scale of the skip turn stand; the latch as the left-out calcAnm found it;
// blendfix31's record is this computation itself).
void OdMakeJoints(LiveActor* a, bool restore) {
    XanimePlayer* xp = a->mModelManager->mXanimePlayer;
    if (restore && xp != nullptr) {
        if (a->mStaleXLatch) {
            xp->_88 = true;
        }
        if (xp->mCore != nullptr && xp->mCore->mPortOwed != 0) {
            xp->mCore->mPortOwed = 2;
        }
    }
    a->mStaleXLatch = false;
    LiveActor* const prev = gPortCalcAnimOwner;
    gPortCalcAnimOwner = a;
    a->mModelManager->calcAnim();
    gPortCalcAnimOwner = prev;
    a->mCrPosed = 1;
}
// The left-out joints from blendfix31's record (the actor's own update ran since the skip turn): the joint tree only, as ModelManager::calc walks it.
void OdModelCalc(void* m) {
    static_cast< J3DModel* >(m)->calc();
}
bool OdRemake(LiveActor* a) {
    J3DModel* model = a->mModelManager->mModel;
    XanimePlayer* xp = a->mModelManager->mXanimePlayer;
    bool ok = false;
    OSLockMutex(&MR::MutexHolder< 0 >::sMutex);
    if (xp == nullptr) {
        model->calc();  // (no skeletal animation: the joints follow the base and base scale alone)
        ok = true;
    } else if (xp->mCore != nullptr) {
        ok = xp->mCore->portRemake(&OdModelCalc, model, model->mModelData->mJointTree.mJointNodePointer[0]);
    }
    OSUnlockMutex(&MR::MutexHolder< 0 >::sMutex);
    return ok;
}
// The left-out computation's joints and its visibility animation (ModelManager::calc: the BVA is applied there, not with the materials), made now from the
// record after the actor's own update ran: exact while the base, the base scale and the BVA resource are those of the skip turn and the core's record
// is intact. false: nothing made.
bool OdJointsFromRecord(LiveActor* a, const OdEntry& e) {
    if (OdBaseHash(a) != e.bhash || !OdPlayersSame(a, e, kOdBva, kOdPlayers) || !OdRemake(a)) {
        return false;
    }
    BvaPlayer* bva = a->mModelManager->mBvaPlayer;
    if (bva != nullptr) {
        const f32 live = bva->mFrameCtrl.mFrame;
        bva->mFrameCtrl.mFrame = e.mframe[kOdBva];
        bva->calc();
        bva->mFrameCtrl.mFrame = live;
    }
    a->mStaleXLatch = false;
    return true;
}
// The materials the picture shows for a pose of the window made outside the next calc-anim, as the skip turn's ModelManager::calcAnim makes them:
// GXV_DL_LAZY=2 (the default) leaves every turn's materials to the drawn turn's material flush, from the frames then (this is that turn, before
// that flush: made now from the frames now); 0 / 1 make them on the skip turn (a drawn turn) from its frames (put back from the record, then
// restored; a stale list entry is passed by, as after calcAnim). false: a material resource changed since (lazy 0 / 1 only): made from now, drift.
bool OdMaterialsForPicture(LiveActor* a, const OdEntry& e) {
    ModelManager* mm = a->mModelManager;
    if (port_dl_lazy_on() == 2) {
        mm->updateDL(true);
        return true;
    }
    if (!OdPlayersSame(a, e, 0, kOdBva)) {
        mm->mStaleDL = false;
        mm->updateDL(true);
        return false;
    }
    f32 live[kOdBva];
    for (int i = 0; i < kOdBva; ++i) {
        AnmPlayerBase* p = OdPlayer(mm, i);
        if (p != nullptr) {
            live[i] = p->mFrameCtrl.mFrame;
            p->mFrameCtrl.mFrame = e.mframe[i];
        }
    }
    mm->mStaleDL = false;
    mm->updateDL(true);
    for (int i = 0; i < kOdBva; ++i) {
        AnmPlayerBase* p = OdPlayer(mm, i);
        if (p != nullptr) {
            p->mFrameCtrl.mFrame = live[i];
        }
    }
    return true;
}
// Mode 2, the next computation: does it read the core's last local transforms? blendfix31's catch-up makes them exactly (mode 1 needs it); without it,
// compare the kept ones (the cut's) against the ones there now (the made skip turn's).
void OdVerifyNext(LiveActor* a, const OdEntry& e) {
    const XanimePlayer* xp = a->mModelManager->mXanimePlayer;
    const XanimeCore* c = xp != nullptr ? xp->mCore : nullptr;
    if (c == nullptr || e.coreJoints == 0 || !e.made) {
        return;
    }
    if (!(c->_28 != 0 || c->portReadsLast()) || BlendFixMode() == 1) {
        return;
    }
    const float* kept = sOdArena.data() + e.off + e.joints * 12u;
    for (u16 j = 0; j < e.coreJoints; ++j) {
        if (memcmp(kept + j * (sizeof(XtransformInfo) / 4), &c->mJointList[j]._28, sizeof(XtransformInfo)) != 0) {
            OdMismatch(2, a, e.kind);
            return;
        }
    }
}
}  // namespace

namespace {
// A read of an owed model's joint matrices (port_bowl_pose_read, from JointUtil / ModelUtil). true: the model is this rule's.
bool OdRead(const J3DModel* model, int what) {
    if (sOdList.empty()) {
        return false;
    }
    OdEntry* e = OdFind(model);
    if (e == nullptr || e->actor == nullptr || (e->actor->mBowlPose & od::kWin) == 0) {
        return false;
    }
    LiveActor* a = e->actor;
    if (what != 1 || (a->mBowlPose & od::kVerifyD) != 0) {
        return true;  // (the base matrix is made on the skip turn: not owed; mode 2: the actor's own calc-anim)
    }
    if ((a->mBowlPose & od::kPending) != 0) {
        // mode 1: a reader that appears only now. Exact while the skip turn's inputs are unchanged (the whole computation), or, after the actor's own
        // update, from the record while the base stands; never from inside another model's computation (its joint callbacks: j3dSys is in use).
        bool made = false;
        if (port_on_frame_thread() && gPortCalcAnimOwner == nullptr) {
            if (OdXHash(a) == e->xhash) {
                OdMakeJoints(a, true);
                made = true;
            } else if (OdJointsFromRecord(a, *e)) {
                e->remade = 1;
                ++sOd.remade;
                made = true;
            }
        }
        if (!made) {
            OdLate(a, e->kind);
            return true;
        }
        a->mStaleAnim = false;  // (its view pass runs as any other's; the flush makes it again for a picture that follows at once)
        ++sOd.posedRead;
        a->mBowlPose = static_cast< u8 >((a->mBowlPose & ~od::kPending) | od::kMade);
        return true;
    }
    if ((a->mBowlPose & od::kVerify) != 0 && e->made) {
        // (mode 1 would make the joints here, exactly as the skip turn made them, while the inputs are unchanged, or from the record while the base
        // stands and no animation change has caught the record up (a change sets the freeze flag the made computation had consumed))
        const XanimePlayer* xp = a->mModelManager->mXanimePlayer;
        const bool recordIntact = xp == nullptr || (xp->mCore != nullptr && xp->mCore->_28 == 0);
        if (OdXHash(a) == e->xhash || (OdBaseHash(a) == e->bhash && recordIntact && OdPlayersSame(a, *e, kOdBva, kOdPlayers))) {
            ++sOd.posedRead;
            OdEnd(a);
        } else {
            ++sOd.late;
            if (memcmp(sOdArena.data() + e->off, model->mMtxBuffer->mpAnmMtx, e->joints * sizeof(Mtx)) != 0) {
                OdMismatch(1, a, e->kind);
            }
        }
    }
    return true;
}
// The drawn turn's flush, before its loop: snapshot, the actors gone, mode 2's windows (the picture check: a pose the next turn did not make).
void OdFlushBegin() {
    if (sOdList.empty()) {
        return;
    }
    port_live_actor_snapshot();
    for (OdEntry& e : sOdList) {
        if (e.actor == nullptr || !port_live_actor_valid(e.actor) || (e.actor->mBowlPose & od::kWin) == 0) {
            e.actor = nullptr;
            continue;
        }
        LiveActor* a = e.actor;
        if ((a->mBowlPose & od::kVerify) != 0) {
            const bool drawn = !(a->mFlag.mIsDead || a->mFlag.mIsClipped || a->mFlag.mIsHiddenModel);
            if (!drawn) {
                ++sOd.notPosed;
            } else if (e.made && a->mModelManager != nullptr && OdXHash(a) != e.xhash) {
                // (mode 1's flush: the joints, the visibility and the materials from the record when the base stands, no change caught it up and the
                // players' resources are the skip turn's (the materials' only under GXV_DL_LAZY 0 / 1); else from now: drift)
                const XanimePlayer* xp = a->mModelManager->mXanimePlayer;
                const bool recordIntact = xp == nullptr || (xp->mCore != nullptr && xp->mCore->_28 == 0);
                if (OdBaseHash(a) == e.bhash && recordIntact && OdPlayersSame(a, e, port_dl_lazy_on() == 2 ? kOdBva : 0, kOdPlayers)) {
                    ++sOd.flushRecord;
                    ++sOd.posedFlush;
                } else {
                    OdMismatch(3, a, e.kind);
                }
            } else {
                ++sOd.posedFlush;
            }
            OdEnd(a);
        }
    }
}
// In the flush's loop, an owed pose of mode 1. 1: the loop makes it now from the skip turn's state (no update since: the joints and materials exactly
// as the skip turn would have made them; the skip turn's base stands); 3: made here, the loop makes its view pass only; 0: not drawn (not made).
// Made here: an update ran since the skip turn and the next calc-anim did not (its calc-anim switched off in its own movement while shown): the
// joints and the visibility from blendfix31's record and the materials as the skip turn makes them (OdJointsFromRecord, OdMaterialsForPicture:
// exact, "from the record"), else, when the record was caught up by an animation change or the base was rewritten, the joints and materials from
// now ("drift", counted; mode 2's picture check reports it). The skip turn's base matrix stands either way (the logic's readers of the base are
// never touched).
int OdFlushEntry(LiveActor* a) {
    const bool drawn = !(a->mFlag.mIsDead || a->mFlag.mIsClipped || a->mFlag.mIsHiddenModel);
    if ((a->mBowlPose & od::kMade) != 0) {
        // (a reader had the joints made in the window and the next turn's calc-anim did not run: the picture shows them; their materials as the skip
        // turn makes them (a catch-up during the next turn left them to the material flush, or never made them) and their view pass are made here)
        const OdEntry* e = OdFind(a);
        OdEnd(a);
        if (!drawn) {
            return 0;
        }
        if (e != nullptr && !OdMaterialsForPicture(a, *e)) {
            ++sOd.drift;
        }
        return 3;
    }
    if ((a->mBowlPose & od::kPending) == 0) {
        return 0;
    }
    if (!drawn) {
        ++sOd.notPosed;
        OdEnd(a);
        return 0;
    }
    const OdEntry* e = OdFind(a);
    if (e != nullptr && OdXHash(a) == e->xhash) {
        ++sOd.posedFlush;
        OdEnd(a);
        return 1;
    }
    a->mStaleAnim = false;
    if (e != nullptr && OdJointsFromRecord(a, *e)) {
        ++sOd.flushRecord;
        ++sOd.posedFlush;
        if (!OdMaterialsForPicture(a, *e)) {
            ++sOd.drift;
        }
    } else {
        OdMakeJoints(a, false);
        ++sOd.drift;
    }
    OdEnd(a);
    return 3;
}
void OdFlushEnd() {
    for (OdEntry& e : sOdList) {
        if (e.actor != nullptr && (e.actor->mBowlPose & od::kWin) != 0) {
            ++sOd.staleAtDraw;  // (by construction never: every owed pose was listed for the flush)
            OdEnd(e.actor);
        }
    }
    sOdList.clear();
    sOdArena.clear();
}
void OdDrop() {
    sOdList.clear();
    sOdArena.clear();
}
// LiveActor::calcAnim (the actor has a model manager). 0: as before; 1: the joints are left out (mode 1); 2: the pose owed since the skip turn is made now,
// whatever the every-n-th rules say.
int PortOnDraw(LiveActor* a) {
    gPortOdTimed = nullptr;  // (a timed computation that did not reach its end: another rule deferred it)
    const u8 bits = a->mBowlPose;
    if ((bits & od::kWin) != 0) {
        if ((bits & od::kPending) != 0) {
            ++sOd.posedNext;
            a->mStaleAnim = false;  // (the flush passes its entry by)
            a->mPbHalf = 0;
            OdEnd(a);
            if ((++sOdSample & 7) == 0) {
                gPortOdTimed = a;  // (the census's price of a pose: this computation, timed to the end of calcAnim)
                sOdT0 = port_host_time_ns();
            }
            return 2;
        }
        if ((bits & od::kVerify) != 0) {
            a->mBowlPose &= static_cast< u8 >(~od::kVerifyD);
            if (const OdEntry* e = OdFind(a)) {
                OdVerifyNext(a, *e);
            }
            ++sOd.posedNext;
            OdEnd(a);
            return 0;
        }
        if ((bits & od::kMade) != 0) {
            a->mStaleAnim = false;
            OdEnd(a);
            return 2;  // (this turn's pose, whatever the every-n-th rules say, as after a left-out one)
        }
        return 0;
    }
    if (!sOdSkip) {
        return 0;
    }
    const int mode = gPortOnDraw;
    const bool timeRule = (++sOdSample & 15) == 0 && ::port::gt71::StatClockOn();  // (gt71 CLOCK: ruleNs only feeds the census line)
    const int64_t t0 = timeRule ? port_host_time_ns() : 0;
    ++sOd.ruleCalls;
    ModelManager* mm = a->mModelManager;
    J3DModel* model = mm->mModel;
    XanimePlayer* xp = mm->mXanimePlayer;
    const XanimeCore* core = xp != nullptr ? xp->mCore : nullptr;
    const int raw = port_anim_lod_raw_category();
    const int kind = od::KindOfList(raw);
    od::Facts f;
    f.skipTurn = !port::lockstep::On();
    f.gate = port_on_frame_thread() && PairPoseGate();
    f.list = kind >= 0;
    f.blendFixOn = BlendFixMode() == 1;
    f.posedOnce = a->mCrPosed != 0 && model != nullptr;
    f.otherRule = a->mStaleAnim || a->mCrSkip != 0xFF || a->mPbHalf != 0 || mm->mCpDlFar != 0 || mm->mCpDlSkip != 0xFF || a->mFlFam != 0 ||
                  a->mAlodFamily != 0 || a->mAlodRate > 1 || a->mLogicFreeJoints != 0 || a->mPairSkip != 0 || a->mDetailIdx != 0xFFFF;
    f.collision = a->mCollisionParts != nullptr;
    f.camera = a->mCameraCtrl != nullptr;
    f.hrwTag = model != nullptr && model->mPortHrwTag != 0;
    f.jointsRead = model != nullptr && (model->mPortBowl & bp::kModelRead) != 0;
    f.callback = false;
    f.sharedCore = core != nullptr && core->mPortShared != 0;
    f.readsLast = core != nullptr && (core->portReadsLast() || core->mTrackCount > XanimeCore::kPortOwedTracks);  // (more tracks than the record holds: made at once)
    int why = od::Refuse(f);
    // (the dearer facts only where they can decide: the far deferral is "other", the callbacks' own place is after "read")
    if ((why < 0 || why > od::kRefOther) && f.posedOnce && AnimLodDefersFar(a)) {
        f.otherRule = true;
        why = od::Refuse(f);
    }
    if ((why < 0 || why > od::kRefCallback) && f.posedOnce && UndrawnLeanCallbacks(a)) {
        f.callback = true;
        why = od::Refuse(f);
    }
    if (why >= 0) {
        ++sOd.refused[why];
        if (why > od::kRefList) {
            ++sOd.asked[kind];
        }
        if (timeRule) {
            sOd.ruleNs += static_cast< uint64_t >(port_host_time_ns() - t0);
            ++sOd.ruleN;
        }
        return 0;
    }
    ++sOd.asked[kind];
    ++sOd.leftOut[kind];
    OdEntry e;
    e.actor = a;
    e.model = model;
    e.off = 0;
    e.joints = 0;
    e.coreJoints = 0;
    e.xhash = 0;
    e.bhash = 0;
    e.remade = 0;
    e.hash0 = 0;
    e.hashPre = 0;
    e.kind = static_cast< uint8_t >(kind);
    e.made = 0;
    OdRecordPlayers(a, e);
    if (mode == 2) {
        // (nothing left out: the products the cut would not make are kept, readers in the window are compared; the skip turn's computation is timed)
        ++sOd.checks;
        ++sOdTotChecks;
        e.joints = model->mModelData->mJointTree.mJointNum;
        e.coreJoints = core != nullptr && core->mJointList != nullptr ? static_cast< u16 >(core->mJointCount) : 0;
        e.off = static_cast< uint32_t >(sOdArena.size());
        const size_t perCore = sizeof(XtransformInfo) / 4;
        sOdArena.resize(sOdArena.size() + e.joints * 12u + e.coreJoints * perCore);
        float* p = sOdArena.data() + e.off;
        memcpy(p, model->mMtxBuffer->mpAnmMtx, e.joints * sizeof(Mtx));
        for (u16 j = 0; j < e.coreJoints; ++j) {
            memcpy(p + e.joints * 12u + j * perCore, &core->mJointList[j]._28, sizeof(XtransformInfo));
        }
        e.hash0 = OdStateHash(a);
        e.hashPre = e.hash0;
        a->mBowlPose |= od::kWin | od::kVerify | od::kVerifyD;
        model->mPortBowl |= bp::kModelPend;
        sOdList.push_back(e);
        if (timeRule) {
            sOd.ruleNs += static_cast< uint64_t >(port_host_time_ns() - t0);
            ++sOd.ruleN;
        }
        gPortOdTimed = a;
        sOdT0 = port_host_time_ns();
        return 0;
    }
    // Mode 1: the base matrix as calcAnmMtx makes it (the class's own calcAndSetBaseMtx, once, as today); the joints left out: the BCK clock released
    // as a made calcAnm releases it and blendfix31's record taken, the latch kept for the flush; listed for the flush (which also keeps the view pass).
    model->setBaseScale(a->mScale);
    a->calcAndSetBaseMtx();
    if (xp != nullptr) {
        a->mStaleXLatch = xp->_88;
        PortPoseLeftOut(xp);
    }
    a->mStaleAnim = true;
    sStaleAnim.push_back(a);
    a->mBowlPose |= od::kWin | od::kPending;
    model->mPortBowl |= bp::kModelPend;
    e.xhash = OdXHash(a);
    e.bhash = OdBaseHash(a);
    sOdList.push_back(e);
    if (timeRule) {
        sOd.ruleNs += static_cast< uint64_t >(port_host_time_ns() - t0);
        ++sOd.ruleN;
    }
    return 1;
}
// Mode 2, in calcAnmMtx between the base (setBaseScale + the class's calcAndSetBaseMtx) and the joints (ModelManager::calcAnim): the logic state the
// verify compares is taken HERE, not before the base (emufix31). Mode 1 makes the base on the skip turn too (PortOnDraw), so what the base writes is not
// left out: PartsModel::calcAndSetBaseMtx sets mPosition from its host's joint (an NPC's goods, "\x83\x4f\x83\x62\x83\x59": Honeyhive's opening camera, emu31c),
// and the old point reported that as "the calc-anim changed the logic state". Only what mode 1 leaves out (the joints, the materials, the view pass) is
// compared now. (Not reached when another rule held the computation: the state before the base stands, which can only report more, never less.)
void PortOnDrawBaseMade(LiveActor* a) {
    if (OdEntry* e = OdFind(a)) {
        e->hash0 = OdStateHash(a);
        ++sOd.baseSplit;
    }
}
// The end of a calcAnim whose pose this rule times (mode 1: the owed pose made by the next turn; mode 2: the skip turn's own) or verifies (mode 2).
void PortOnDrawAfter(LiveActor* a) {
    if (gPortOdTimed == a) {
        gPortOdTimed = nullptr;
        sOd.poseNs += static_cast< uint64_t >(port_host_time_ns() - sOdT0);
        ++sOd.poseN;
    }
    if ((a->mBowlPose & (od::kWin | od::kVerifyD)) == (od::kWin | od::kVerifyD)) {
        a->mBowlPose &= static_cast< u8 >(~od::kVerifyD);
        if (OdEntry* e = OdFind(a)) {
            const uint32_t now = OdStateHash(a);
            if (now != e->hash0) {
                OdMismatch(0, a, e->kind);   // "logic state differs": what the calc-anim itself changed (the state after the base is the reference)
            } else if (now != e->hashPre) {
                // (fable31fix) "base-written state differs": the class's calcAndSetBaseMtx wrote logic state (an attachment's position from its host's joint). Mode 1 makes
                // the base too, so this is exact and NOT a mismatch; counted apart so the verify still shows what the base writes.
                ++sOd.baseWrote;
                ++sOdTotBaseWrote;
            }
            e->xhash = OdXHash(a);
            e->bhash = OdBaseHash(a);
            e->made = 1;
        }
    }
}
}  // namespace

extern "C" int port_pose_ondraw_mode(void) { return OdMode(); }
// GameSystem::frameLoop's turn start (perf.cpp port_undrawn_turn_begin): the phase, the census line.
extern "C" void port_pose_ondraw_turn_begin(int drawn) {
    if (MpDetLodOn() && port::lockstep::On()) {
        if (++sMpDetLod.turns >= 600) {
            port_log_summary("mp-det-lod: GXV_MP_DET_LOD=1, %u lockstep turns: PbAnimSkips allowed(freeJoints) %u skipped %u blocked(other) %u",
                             static_cast< unsigned >(sMpDetLod.turns), static_cast< unsigned >(sMpDetLod.pbAllowed),
                             static_cast< unsigned >(sMpDetLod.pbSkipped), static_cast< unsigned >(sMpDetLod.pbBlocked));
            memset(&sMpDetLod, 0, sizeof(sMpDetLod));
        }
    }
    const int mode = OdMode();
    gPortOnDraw = mode;
    gPortOdTimed = nullptr;
    if (mode == 0) {
        sOdSkip = false;
        return;
    }
    ++sOdTurnNo;
    sOdSkip = sOdPhase.Begin(drawn != 0);
    od::Census& c = sOd;
    ++c.turns;
    if (sOdSkip) {
        ++c.skipTurns;
    }
    if (sOdPhase.Broken(drawn != 0)) {
        ++c.broken;
    }
    const uint32_t bc = OdBlendCatchUps();
    c.blendCatch += bc >= sOdBlendLast ? bc - sOdBlendLast : bc;  // (its own census line may clear it)
    sOdBlendLast = bc;
    if (c.turns < 600) {
        return;
    }
    const double pose = c.poseN ? static_cast< double >(c.poseNs) / c.poseN / 1000.0 : 0.0;
    const double rule = c.ruleN ? static_cast< double >(c.ruleNs) / c.ruleN / 1000.0 : 0.0;
    port_log_summary("pose-ondraw: GXV_POSE_ONDRAW=%d, %u turns, %u skip turns, %u broken pairs; asked/%s: env %u/%u mapobj %u/%u npc %u/%u enemy %u/%u "
                     "deco %u/%u item %u/%u (all %u/%u); owed pose made by the next turn %u, the flush %u (from the record %u), a read %u (from the record %u), not drawn %u, drift %u; refused "
                     "(turn/list/blendfix/other/picture/read/callback/shared/blend) %u/%u/%u/%u/%u/%u/%u/%u/%u; blendfix catch-ups %u; LATE reads %u; "
                     "logic-state mismatches (state/joints/next-pose/picture) %u/%u/%u/%u of %u checked (%u from after the base); base-written state differs (not a mismatch: mode 1 makes the base too) %u, since the start %u; since the start: %u checked, mismatches %u/%u/%u/%u; "
                     "stale at draw %llu; pose %.1f us (%u timed), rule %.2f us; est. saved %.3f ms a pair",
                     mode, c.turns, c.skipTurns, c.broken, mode == 2 ? "would leave out" : "left out", c.asked[0], c.leftOut[0], c.asked[1], c.leftOut[1],
                     c.asked[2], c.leftOut[2], c.asked[3], c.leftOut[3], c.asked[4], c.leftOut[4], c.asked[5], c.leftOut[5], c.AskedAll(), c.LeftAll(),
                     c.posedNext, c.posedFlush, c.flushRecord, c.posedRead, c.remade, c.notPosed, c.drift, c.refused[0], c.refused[1], c.refused[2], c.refused[3], c.refused[4],
                     c.refused[5], c.refused[6], c.refused[7], c.refused[8], c.blendCatch, c.late, c.mismatch[0], c.mismatch[1], c.mismatch[2], c.mismatch[3],
                     c.checks, c.baseSplit, c.baseWrote, sOdTotBaseWrote, sOdTotChecks, sOdTotMismatch[0], sOdTotMismatch[1], sOdTotMismatch[2], sOdTotMismatch[3],
                     static_cast< unsigned long long >(c.staleAtDraw), pose, c.poseN, rule, c.SavedMsPerPair());
    c.Clear();
}
#endif

// far49 (GXV_FAR_PROTECT, default 1; port/farprotect.h): the actors the player deals with (objective names, a sensor actor with a stage switch / camera / pointer
// target / rail, an NPC with a hit sensor) are posed on every turn by every cut's rule: mPortNoCut, the path the galaxy map's miniatures take. Read at the first calc-anim
// (the actor's parts exist; the calc-anim list being run is the NPC list's or not), then never again.
#ifdef TARGET_PC
static void PortFarProtRead(LiveActor* a) {
    namespace fp = port::farprotect;
    a->mFarProt = 1;
    if (!fp::On()) {
        return;
    }
    fp::Facts f;
    f.name = a->mName;
    f.sensors = a->mSensorKeeper != nullptr && a->mSensorKeeper->mSensorCount > 0;
    f.stageSwitch = a->mStageSwitchCtrl != nullptr;
    f.camCtrl = a->mCameraCtrl != nullptr;
    f.pointer = a->mStarPointerTarget != nullptr;
    f.rail = a->mRailRider != nullptr;
    f.npcList = port_anim_lod_raw_category() == 6;
    f.family = a->mFlFam;
    if (fp::Protected(f)) {
        a->mFarProt = 2;
        a->mPortNoCut = true;
    }
}
#endif

void LiveActor::calcAnim() {
    if (mFlag.mIsNoCalcAnim) {
        return;
    }
#ifdef TARGET_PC
    // GXV_ALOD: the calcAnim of a turn whose movement was left out is left out too (the joints of the last movement stay; the actor
    // is off the picture, or a few pixels on it). Nothing moved, so the latch is as that movement's calcAnim left it; the catch-up
    // makes the clear this call would have made (port::alod::PayOwed). The view pass still runs (calcViewAndEntry): the model stays
    // placed for this turn's camera. GXV_ALOD=2 (census): nothing is left out; the call is timed.
    if (__builtin_expect(mFarProt == 0 && mModelManager != nullptr, 0)) {
        PortFarProtRead(this);  // (far49: once per actor, from its own parts; sets mPortNoCut for the actors the player deals with)
    }
    if (__builtin_expect(mPortNoCut, 0)) {
        // dome31 (bug fix, always on): the galaxy map's miniatures are posed every turn by no cut at all. The pointer is aimed at the pose
        // (MiniatureGalaxy's picking anchor is its base matrix, made by calcAndSetBaseMtx inside calcAnmMtx), and with cpulod=3 on the
        // AstroDome stage a FARANIM / CHARPOSE / FAMPOSE every-4th cut left the drawn miniature up to three turns behind its orbit.
        if (mStaleAnim) {
            mStaleAnim = false;  // (a pose owed from an earlier cut is made by this very call; the flush then finds nothing to do)
        }
        calcAnmMtx();
        mCrPosed = 1;
        if (mCollisionParts != nullptr) {
            MR::setCollisionMtx(this);
        }
        return;
    }
    PortAlodCalcTimer alodTimer(this);
    PortFlPoseTimer flPoseTimer(this);
    if (mAlodSkipTurn != 0 && mAlodSkipTurn == gPortAlodTurn) {
        mAlodSkipTurn = 0;
        if (!alodTimer.on()) {
            return;
        }
    }
    if (__builtin_expect(port_cc_flag != 0, 0)) {
        port_cc_mark(this, 1);  // (GXV_CLIPCENSUS)
    }
    PORT_ANIM_PROF_NAME(this);
    PortLogicProfName(this);
    // cpu34g, GXV_PAIRPOSE: the drawn turn of a pair leaves this model's joints out (the clock released as below); the turn after it poses it whatever the every-2nd rules say
    bool pairMade = false;
    // bowl31, GXV_BOWL_POSE (port/bowl_pose.h): a marked class's pose on the skip turn, left out when nothing reads it; the next calc-anim makes it whatever the
    // every-n-th rules say (as the pair rule's next turn does). One byte test for every other actor.
    // ondraw31, GXV_POSE_ONDRAW (port/pose_ondraw.h): the same for every class of the non-player lists, decided per actor at run time; it supersedes
    // bowl31's rule while it is on (an open window is closed by the rule that opened it). One load for every actor with both off.
    if (__builtin_expect((mBowlPose | gPortOnDraw) != 0, 0) && mModelManager != nullptr) {
        int bowl = 0;
        if ((mBowlPose & 0x10) != 0 || (gPortOnDraw != 0 && (mBowlPose & 0xE0) == 0)) {
            bowl = PortOnDraw(this);
        } else if (mBowlPose != 0) {
            bowl = PortBowlPose(this);
        }
        if (bowl == 1) {
            return;
        }
        pairMade = bowl == 2;
    }
    if (__builtin_expect(mModelManager != nullptr && (port_pair_skip_turn != 0 || mPairSkip != 0), 0)) {
        const int pairMode = port_pairpose_mode();
        if (port_pair_skip_turn != 0 && pairMode != 0 && !port::lockstep::On() && PairPoseLeavesOut(this, pairMode)) {
            mPairSkip = 1;
            port_pair_count(0, 1);
            if (mModelManager->mXanimePlayer != nullptr) {
                PortPoseLeftOut(mModelManager->mXanimePlayer);  // (blendfix31)
            }
            return;
        }
        if (mPairSkip != 0) {
            mPairSkip = 0;
            mPbHalf = 0;
            pairMade = true;
            port_pair_count(1, 1);
        }
    }
    if (!pairMade && mModelManager != nullptr && PbAnimSkips(this)) {
        // GXV_PB_ANIM: this turn's pose is left out. The BCK clock is released as a made calcAnm releases it (XanimePlayer::calcAnm
        // clears _88 and updateAfterMovement advances the frame only when it is clear), so the animation keeps its pace and the frame
        // the next made calcAnm poses is the one the game would pose then.
        if (mModelManager->mXanimePlayer != nullptr) {
            PortPoseLeftOut(mModelManager->mXanimePlayer);  // (blendfix31)
        }
        return;
    }
    // (revB: only when no other every-n-th-turn rule holds this bit: PB_ANIM's mPbHalf alternation or FAMPOSE / FARANIM / CHARPOSE's mCrSkip count. Two such rules
    // on one actor, each posing every second turn on its own phase, can be complementary: every turn is left out by one of them and the bit is never posed again.
    // With GXV_PROTECT_SPRITES=2, the default, PbAnimSkips leaves star bits alone and this is the only rule.)
    if (mModelManager != nullptr && mFlFam == 1 && !port::lockstep::On() && port_starbatch_far_on() != 0 && mPbHalf == 0 && mCrSkip == 0xFF && port_starbatch_far_skip(this, gPortAlodTurn, 0) != 0) {
        // GXV_STARBATCH_FAR (starbatch26): a far, floating star bit's pose on this turn is left out, as PbAnimSkips' above (the clock released the same way)
        if (mModelManager->mXanimePlayer != nullptr) {
            PortPoseLeftOut(mModelManager->mXanimePlayer);  // (blendfix31)
        }
        return;
    }
    if (mModelManager != nullptr && AnimLodDefers(this)) {
        PORT_ANIM_PROF_COUNT(0);
        // The BCK clock (XanimePlayer) is latched by calcAnim: XanimePlayer::updateAfterMovement advances the frame only if the
        // last calcAnm cleared _88 (the frame before the advance is kept in _84: calcAnm poses that frame, and checkPass reads it
        // while _88 is set). A left-out calcAnim that left _88 set froze the animation at one frame per PICTURE (5x slow at
        // 10 FPS), and every turn of the freeze answered checkPass for the same window again: each BCK-synced one-time effect
        // (SyncBckEffectChecker::isCreate via EffectKeeper::syncEffectBck), sound or nerve step tied to a frame fired once per
        // turn instead of once (lit Observatory of a 50-star file, 4 Nov: ~545 effect draws a picture instead of ~80, and a GPU
        // runaway from 37 to 10 FPS). The latch is cleared here as calcAnm clears it; whether calcAnm would have posed _84 is
        // kept, and port_anim_flush_stale sets the latch back for its calcAnm (_84 is unchanged: no update() runs between the
        // last calcAnim and the flush), so the picture's pose is the same frame as before. GXV_ANIM_LOD_XLATCH=0: the old way.
        const bool sXLatch = PORT_HOT_KNOB(bool, [] {  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
            const char* e = getenv("GXV_ANIM_LOD_XLATCH");
            return !(e && *e == '0');
        }());
        if (sXLatch && mModelManager->mXanimePlayer != nullptr) {
            mStaleXLatch = mModelManager->mXanimePlayer->_88;
            PortPoseLeftOut(mModelManager->mXanimePlayer);  // (blendfix31: the flush makes this very computation, see port_anim_flush_stale)
        }
        if (!mStaleAnim) {
            mStaleAnim = true;
            sStaleAnim.push_back(this);
        }
        return;
    }
    // (computed now; if it is still listed, from a farther turn, the flush computes it once more from the same state: it must
    // pose the frame this calcAnm poses, so the latch this calcAnm finds is kept for it as above)
    mStaleXLatch = mStaleAnim && mModelManager != nullptr && mModelManager->mXanimePlayer != nullptr && mModelManager->mXanimePlayer->_88;
#endif

#ifdef TARGET_PC
    if (__builtin_expect(port_cc_flag != 0, 0)) {
        port_cc_mark(this, 2);  // (GXV_CLIPCENSUS: its joints are posed now)
    }
    if (__builtin_expect(port_logic_prof_flag != 0, 0)) {
        PORT_LOGIC_SYS(PORT_LS_CALCANIM);  // (GXV_LOGIC_PROF: the joints / materials proper, without the list and the deferral tests)
        calcAnmMtx();
    } else {
        calcAnmMtx();
    }
    mCrPosed = 1;  // (cpucuts25: joints computed at least once)
#else
    calcAnmMtx();
#endif
#ifdef TARGET_PC
    if (mLodRadius < 0.0f && mModelManager != nullptr) {
        AnimLodTakeRadius(this);  // (GXV_ANIM_LOD_OFFSCREEN)
    }
    if (__builtin_expect(gPortOdTimed == this || (mBowlPose & 0x20) != 0, 0)) {
        if (gPortOdTimed == this || (mBowlPose & 0x10) != 0) {
            PortOnDrawAfter(this);  // (GXV_POSE_ONDRAW: the census's timing; =2: the skip turn's calc-anim changed nothing but the pose)
        } else {
            PortBowlVerifyAfter(this);  // (GXV_BOWL_POSE=2: the skip turn's calc-anim changed nothing but the pose)
        }
    }
#endif

    if (mCollisionParts == nullptr) {
        return;
    }

    MR::setCollisionMtx(this);
}

void LiveActor::calcAnmMtx() {
    if (mModelManager == nullptr) {
        return;
    }

    MR::getJ3DModel(this)->setBaseScale(mScale);
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    const u32 familyToken = port_workload_anim_family_begin(0);
#endif
    calcAndSetBaseMtx();
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    port_workload_anim_family_end(familyToken);
#endif
#ifdef TARGET_PC
    if (__builtin_expect((mBowlPose & 0x30) == 0x30, 0)) {
        PortOnDrawBaseMade(this);  // (GXV_POSE_ONDRAW=2: the skip turn's own computation; its base is made in mode 1 too, so the state is compared from here)
    }
    gPortCalcAnimOwner = this;  // (GXV_DL_LAZY lists only models of live actors, see ModelManager.cpp)
    mModelManager->calcAnim();
    gPortCalcAnimOwner = nullptr;
#else
    mModelManager->calcAnim();
#endif
}

void LiveActor::calcViewAndEntry() {
    if (mFlag.mIsNoCalcView) {
        return;
    }
#ifdef TARGET_PC
    if (mStaleAnim) {
        return;  // (GXV_ANIM_LOD: its view pass is made with its joints, before the next picture)
    }
#endif

    if (mModelManager == nullptr) {
        return;
    }

    if (mFlag.mIsNoCalcView) {
        return;
    }

#ifdef TARGET_PC
    {
        J3DModel* fadeModel = mModelManager->mModel;  // (q30fin26, GXV_THINFADE: a far item shrinking away / growing back has its base scale written before the view pass reads it)
        if (__builtin_expect(fadeModel != nullptr && fadeModel->mPortFade != 0, 0)) {
            port_thinfade_apply(this, fadeModel);
        }
    }
#endif
    mModelManager->calcView();
}

bool LiveActor::receiveMessage(u32 msg, HitSensor* pSender, HitSensor* pReceiver) {
    if (msg == ACTMES_PUSH) {
        return receiveMsgPush(pSender, pReceiver);
    }

    bool isMsgPlayerAttack = msg > ACTMES_PLAYER_ATTACK_START && msg < ACTMES_PLAYER_ATTACK_END;

    if (isMsgPlayerAttack) {
        return receiveMsgPlayerAttack(msg, pSender, pReceiver);
    }

    bool isMsgEnemyAttack = msg > ACTMES_ENEMY_ATTACK_START && msg < ACTMES_ENEMY_ATTACK_END;

    if (isMsgEnemyAttack) {
        return receiveMsgEnemyAttack(msg, pSender, pReceiver);
    }

    if (msg == ACTMES_TAKE) {
        return receiveMsgTake(pSender, pReceiver);
    }

    if (msg == ACTMES_TAKEN) {
        return receiveMsgTaken(pSender, pReceiver);
    }

    if (msg == ACTMES_THROW) {
        return receiveMsgThrow(pSender, pReceiver);
    }

    if (msg == ACTMES_APART) {
        return receiveMsgApart(pSender, pReceiver);
    }

    return receiveOtherMsg(msg, pSender, pReceiver);
}

void LiveActor::calcAndSetBaseMtx() {
    if (MR::getTaken(this)) {
        MR::setBaseTRMtx(this, MR::getTaken(this)->mHost->getTakingMtx());
    } else {
        TPos3f mtx;

        if (mRotation.x == 0.0f && mRotation.z == 0.0f) {
            MR::makeMtxTransRotateY(mtx, this);
        } else {
            MR::makeMtxTR(mtx, this);
        }

        MR::setBaseTRMtx(this, mtx);
    }
}

MtxPtr LiveActor::getTakingMtx() const {
    return getBaseMtx();
}

void LiveActor::setNerve(const Nerve* pNerve) {
    mSpine->setNerve(pNerve);
}

bool LiveActor::isNerve(const Nerve* pNerve) const {
    return mSpine->getCurrentNerve() == pNerve;
}

s32 LiveActor::getNerveStep() const {
    return mSpine->mStep;
}

HitSensor* LiveActor::getSensor(const char* pName) const {
    if (mSensorKeeper != nullptr) {
        return mSensorKeeper->getSensor(pName);
    }

    return nullptr;
}

MtxPtr LiveActor::getBaseMtx() const {
#ifdef TARGET_PC
    if (__builtin_expect((mBowlPose & 0xC0) != 0, 0) && (mBowlPose & 0x10) == 0 && MR::getJ3DModel(this) != nullptr) {  // (ondraw31: its base is made on the skip turn)
        port_bowl_pose_read(MR::getJ3DModel(this), 2);  // (GXV_BOWL_POSE: its base matrix read while its pose is owed: counted / compared)
    }
#endif
    if (MR::getJ3DModel(this) != nullptr) {
        return MR::getJ3DModel(this)->mBaseTransformMtx;
    }

    return nullptr;
}

void LiveActor::startClipped() {
    mFlag.mIsClipped = true;

    if (getSensorKeeper() != nullptr) {
        mSensorKeeper->invalidateBySystem();
    }

    if (mEffectKeeper != nullptr) {
        mEffectKeeper->stopEmitterOnClipped();
    }

    MR::disconnectToSceneTemporarily(this);

    if (MR::isNoEntryDrawBuffer(this)) {
        return;
    }

    MR::disconnectToDrawTemporarily(this);
}

void LiveActor::endClipped() {
    mFlag.mIsClipped = false;

    if (getSensorKeeper() != nullptr) {
        mSensorKeeper->validateBySystem();
        MR::updateHitSensorsAll(this);
    }

    if (mEffectKeeper != nullptr) {
        mEffectKeeper->playEmitterOffClipped();
    }

    MR::connectToSceneTemporarily(this);

    if (MR::isNoEntryDrawBuffer(this)) {
        return;
    }

    MR::connectToDrawTemporarily(this);
}

void LiveActor::initModelManagerWithAnm(const char* pModelName, const char* pAnimName, bool a3) {
    mModelManager = new ModelManager();
    mModelManager->init(pModelName, pAnimName, a3);

    MR::getJ3DModel(this)->setBaseScale(mScale);
    LiveActor::calcAndSetBaseMtx();
    MR::calcJ3DModel(this);

    mAnimKeeper = ActorAnimKeeper::tryCreate(this);
    mCameraCtrl = ActorPadAndCameraCtrl::tryCreate(mModelManager, &mPosition);
}

void LiveActor::initNerve(const Nerve* pNerve) {
    mSpine = new Spine(this, pNerve);
}

void LiveActor::initHitSensor(int numSensors) {
    mSensorKeeper = new HitSensorKeeper(numSensors);
}

void LiveActor::initBinder(f32 radius, f32 offsetY, u32 planeNum) {
    mBinder = new Binder(getBaseMtx(), &mPosition, &mGravity, radius, offsetY, planeNum);

    MR::onBind(this);

    if (mEffectKeeper != nullptr) {
        mEffectKeeper->setBinder(mBinder);
    }
}

void LiveActor::initRailRider(const JMapInfoIter& rIter) {
    mRailRider = new RailRider(rIter);
}

void LiveActor::initEffectKeeper(int a1, const char* a2, bool doSort) {
    mEffectKeeper = new EffectKeeper(getName(), MR::getModelResourceHolder(this), a1, a2);

    if (doSort) {
        mEffectKeeper->enableSort();
    }

    mEffectKeeper->init(this);

    if (mBinder != nullptr) {
        mEffectKeeper->setBinder(mBinder);
    }
}

void LiveActor::initSound(int param1, bool is2D) {
    if (!is2D) {
        mSoundObject = new AudAnmSoundObject(&mPosition, param1, MR::getCurrentHeap());
    } else {
        mSoundObject = new AudAnmSoundObject(nullptr, param1, MR::getCurrentHeap());
    }
}

void LiveActor::initShadowControllerList(u32 shadowNum) {
    mShadowControllerList = new ShadowControllerList(this, shadowNum);
}

void LiveActor::initActorCollisionParts(const char* pParam1, HitSensor* pParam2, ResourceHolder* pParam3, MtxPtr pParam4, bool param5, bool param6) {
    MR::CollisionScaleType scaleType;

    if (param6) {
        scaleType = MR::CollisionScaleType_NotUsingScale;
    } else {
        scaleType = MR::CollisionScaleType_Unk2;

        if (param5) {
            scaleType = MR::CollisionScaleType_AutoEqualScale;
        }
    }

    if (pParam3 != nullptr) {
        TPos3f mtx;

        if (pParam4 != nullptr) {
            mtx.set(pParam4);
        } else {
            MR::makeMtxTRS(mtx.toMtxPtr(), this);
        }

        mCollisionParts = MR::createCollisionPartsFromResourceHolder(pParam3, pParam1, pParam2, mtx, scaleType);
    } else if (pParam4 == nullptr) {
        mCollisionParts = MR::createCollisionPartsFromLiveActor(this, pParam1, pParam2, scaleType);
    } else {
        mCollisionParts = MR::createCollisionPartsFromLiveActor(this, pParam1, pParam2, pParam4, scaleType);
    }

    MR::invalidateCollisionParts(this);
}

void LiveActor::initStageSwitch(const JMapInfoIter& rIter) {
    mStageSwitchCtrl = MR::createStageSwitchCtrl(this, rIter);
}

void LiveActor::initActorStarPointerTarget(f32 radius, const TVec3f* pTrans, MtxPtr pMtx, TVec3f offset) {
    mStarPointerTarget = new StarPointerTarget(radius, pTrans, pMtx, offset);
}

void LiveActor::initActorLightCtrl() {
    mActorLightCtrl = new ActorLightCtrl(this);
}

void LiveActor::attackSensor(HitSensor* pSender, HitSensor* pReceiver) {
}

bool LiveActor::receiveMsgApart(HitSensor* pSender, HitSensor* pReceiver) {
    MR::setHitSensorApart(pSender, pReceiver);

    return true;
}

void LiveActor::addToSoundObjHolder() {
    mSoundObject->addToSoundObjHolder();
}

void LiveActor::updateBinder() {
    if (mBinder == nullptr) {
        mPosition += mVelocity;
    } else if (mFlag.mIsNoBind) {
        mPosition += mVelocity;
        mBinder->clear();
    } else {
        mPosition += getBinder()->bind(mVelocity);
    }
}
