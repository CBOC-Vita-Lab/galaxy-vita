#pragma once

#include "Game/LiveActor/LiveActorFlag.hpp"
#include "Game/NameObj/NameObj.hpp"
#include <JSystem/JGeometry/TVec.hpp>

class ActorAnimKeeper;
class ActorLightCtrl;
class ActorPadAndCameraCtrl;
class AudAnmSoundObject;
class Binder;
class CollisionParts;
class EffectKeeper;
class HitSensor;
class HitSensorKeeper;
class ModelManager;
class Nerve;
class RailRider;
class ResourceHolder;
class ShadowControllerList;
class Spine;
class StageSwitchCtrl;
class StarPointerTarget;

/// @brief The basis of a drawable actor that can contain states (see: Nerve)
class LiveActor : public NameObj {
public:
    /// @brief Creates a new `LiveActor`.
    /// @param pName A pointer to the null-terminated name of the actor.
    LiveActor(const char* pName);

    /// @brief Intializes the `LiveActor` while being placed into a scene.
    /// @param rIter A reference to an iterator over a `JMapInfo`.
    /* 0x0C */ virtual void init(const JMapInfoIter& rIter) override;

    /* 0x14 */ virtual void movement();
    /* 0x1C */ virtual void calcAnim();
    /* 0x20 */ virtual void calcViewAndEntry();
    /* 0x24 */ virtual void appear();
    /* 0x28 */ virtual void makeActorAppeared();
    /* 0x2C */ virtual void kill();
    /* 0x30 */ virtual void makeActorDead();
    /* 0x34 */ virtual bool receiveMessage(u32 msg, HitSensor* pSender, HitSensor* pReceiver);

    /// @brief Gets the base matrix of the model used for the actor.
    /// @returns A MtxPtr to the base matrix. NULL if there is no model present.
    /* 0x38 */ virtual MtxPtr getBaseMtx() const;
    /* 0x3C */ virtual MtxPtr getTakingMtx() const;
    /* 0x40 */ virtual void startClipped();
    /* 0x44 */ virtual void endClipped();

    /* 0x48 */ virtual void control() {
    }

    /// @brief Calculates and sets the base matrix of the actor.
    /* 0x4C */ virtual void calcAndSetBaseMtx();

    /* 0x50 */ virtual void updateHitSensor(HitSensor* pSensor) {
    }

    /* 0x54 */ virtual void attackSensor(HitSensor* pSender, HitSensor* pReceiver);

    /* 0x58 */ virtual bool receiveMsgPush(HitSensor* pSender, HitSensor* pReceiver) {
        return false;
    }

    /* 0x5C */ virtual bool receiveMsgPlayerAttack(u32 msg, HitSensor* pSender, HitSensor* pReceiver) {
        return false;
    }

    /* 0x60 */ virtual bool receiveMsgEnemyAttack(u32 msg, HitSensor* pSender, HitSensor* pReceiver) {
        return false;
    }

    /* 0x64 */ virtual bool receiveMsgTake(HitSensor* pSender, HitSensor* pReceiver) {
        return false;
    }

    /* 0x68 */ virtual bool receiveMsgTaken(HitSensor* pSender, HitSensor* pReceiver) {
        return false;
    }

    /* 0x6C */ virtual bool receiveMsgThrow(HitSensor* pSender, HitSensor* pReceiver) {
        return false;
    }

    /* 0x70 */ virtual bool receiveMsgApart(HitSensor* pSender, HitSensor* pReceiver);

    /* 0x74 */ virtual bool receiveOtherMsg(u32 msg, HitSensor* pSender, HitSensor* pReceiver) {
        return false;
    }

    HitSensorKeeper* getSensorKeeper() const {
        return mSensorKeeper;
    }

    Binder* getBinder() const {
        return mBinder;
    }

    const TVec3f* getGravity() const {
        return &mGravity;
    }

    const TVec3f* getVelocity() const {
        return &mVelocity;
    }

    const TVec3f* getPosition() const {
        return &mPosition;
    }

    TVec3f* editPosition() {
        return &mPosition;
    }

    void calcAnmMtx();
    void setNerve(const Nerve* pNerve);
    bool isNerve(const Nerve* pNerve) const;
    s32 getNerveStep() const;

    /// @brief Gets a sensor.
    /// @param pSensorName The name of the sensor to get.
    /// @returns The sensor that contains the name given. Returns the result of HitSensorKeeper::getSensor or NULL if there is no HitSensorKeeper
    /// created.
    HitSensor* getSensor(const char* pSensorName) const;
    void initModelManagerWithAnm(const char* pModelArcName, const char* pAnimArcName, bool);
    void initNerve(const Nerve*);
    void initHitSensor(int);
    void initBinder(f32, f32, u32);
    void initRailRider(const JMapInfoIter& rIter);
    void initEffectKeeper(int, const char*, bool);
    void initSound(int, bool);
    void initShadowControllerList(u32);
    void initActorCollisionParts(const char*, HitSensor*, ResourceHolder*, MtxPtr, bool, bool);
    void initStageSwitch(const JMapInfoIter& rIter);
    void initActorStarPointerTarget(f32, const TVec3f*, MtxPtr, TVec3f);
    void initActorLightCtrl();
    void addToSoundObjHolder();
    void updateBinder();

    /// @brief The three-dimensional location vector.
    /* 0x0C */ TVec3f mPosition;

    /// @brief The three-dimensional rotation vector, in degrees.
    /* 0x18 */ TVec3f mRotation;

    /// @brief The three-dimensional scale vector.
    /* 0x24 */ TVec3f mScale;

    /// @brief The three-dimensional velocity vector, in units per step.
    /* 0x30 */ TVec3f mVelocity;

    /// @brief The three-dimensional gravity unit vector.
    /* 0x3C */ TVec3f mGravity;

    /// @brief A pointer to a `ModelManager` instance, used for drawing a 3D model.
    /* 0x48 */ ModelManager* mModelManager;

    /// @brief A pointer to an `ActorAnimKeeper` instance, used for storing and playing model animations.
    /* 0x4C */ ActorAnimKeeper* mAnimKeeper;

    /* 0x50 */ Spine* mSpine;

    /// @brief A pointer to a `HitSensorKeeper` instance, used for storing `HitSensor` instances.
    /* 0x54 */ HitSensorKeeper* mSensorKeeper;

    /* 0x58 */ Binder* mBinder;

    /// @brief A pointer to a `RailRider` instance, used for allowing the actor to ride on paths.
    /* 0x5C */ RailRider* mRailRider;
    /* 0x60 */ EffectKeeper* mEffectKeeper;
    /* 0x64 */ AudAnmSoundObject* mSoundObject;

    /// @brief Flags relating to drawing, animation calculation, etc.
    /* 0x68 */ LiveActorFlag mFlag;

    /* 0x74 */ ShadowControllerList* mShadowControllerList;
    /* 0x78 */ CollisionParts* mCollisionParts;

    /// @brief A pointer to a `StageSwitchCtrl` instance, used for activating and deactivating switches.
    /* 0x7C */ StageSwitchCtrl* mStageSwitchCtrl;

    /* 0x80 */ StarPointerTarget* mStarPointerTarget;
    /* 0x84 */ ActorLightCtrl* mActorLightCtrl;
    /* 0x88 */ ActorPadAndCameraCtrl* mCameraCtrl;
#ifdef TARGET_PC
    bool mStaleAnim = false;  // (GXV_ANIM_LOD) joints not computed since a far turn without a picture; listed for the next drawn turn
    bool mPortNoCut = false;  // (dome31) never left out of a pose: every port pose / animation cut (pb_*, FARANIM, CHARPOSE, FAMPOSE, MATLOD, pair pose, ALOD pose, bowl, star-bit far, POSE_PHASE) skips it; the galaxy map's miniatures (their pose is what the pointer is aimed at)
    bool mStaleXLatch = false;  // (GXV_ANIM_LOD) the XanimePlayer's _88 as the last left-out calcAnim found it (LiveActor.cpp)
    LiveActor* mPortHrwHost = nullptr;  // (GXV_HRW, hrw28) the actor this one is a part of (PartsModel's host), else null: a unit of the half-rate world's set is a root and its parts
    u16 mDetailIdx = 0xFFFF;  // (detail25, GXV_DETAIL_PLANT) a plant member's index in its group (0xFFFF: not one)
    u8 mDetailThin = 0;       // the thinning state applied (0 full, 1 every 2nd member, 2 every 4th)
    u8 mDetailTgt = 0;        // the state the distance asks for (hysteresis)
    u8 mDetailWait = 0;       // pictures the change has waited for an unseen moment
    u8 mCrKindA = 0xFF;         // (GXV_CLIPCENSUS, cliprange25) port::cliprange::Kind as of the last census pass (0xFF: none yet)
    u8 mFarProt = 0;            // (far49, GXV_FAR_PROTECT) 0 not read yet, 1 no, 2 protected: mPortNoCut was set from the actor's own parts (LiveActor.cpp PortFarProtRead)
    u8 mCrSkip = 0xFF;          // (GXV_FARANIM, cliprange25) poses left out since the last one made (0xFF: not far / fresh)
    u8 mPbHalf = 0;             // (GXV_PB_ANIM, the Performance-mode visual budget) 0 none, 1: the next far calcAnim is left out, 2: it is made
    u8 mCrPosed = 0;            // (GXV_FARDRAW_FLUSH, cpucuts25) 1: its joints were computed at least once (a model never posed is never left out of the flush)
    u8 mLogicFreeJoints = 0;    // (GXV_ANIM_LOD_FREE) 1: nothing but the picture reads this model's joints (set by the class; in the
                                // padding before mLodRadius: the size is unchanged)
    u8 mPairSkip = 0;           // (GXV_PAIRPOSE, cpu34g) 1: the last calcAnim left the joints out as the drawn turn of a pair; the next one poses (in the same padding)
    u8 mBowlPose = 0;           // (GXV_BOWL_POSE, bowl31; port/bowl_pose.h) the class (set by Fish / the penguins) and the skip turn's window state (the last padding byte)
    f32 mLodRadius = -1.0f;     // (GXV_ANIM_LOD_OFFSCREEN) the model's bounding radius at scale 1 (<0: not known yet; 0: never off-screen)
    // GXV_ALOD (actorlod23: the update LOD of far, off-screen actors; decomp/src/Game/LiveActor/ActorTurnLod.cpp, port/actor_lod.h)
    u8 mAlodOwed = 0;            // movements left out and not yet replayed (at most the rate - 1)
    u8 mAlodRate = 0;            // the rate of the last decision (0 / 1: every turn, 2: half, 4: quarter)
    u8 mAlodFamily = 0;          // the family (MR::portMarkActorLod; 0: never left out)
    u8 mAlodCulled = 0;          // the model is stamped not to be drawn in a picture (GXV_ALOD_DRAWCULL)
    // GXV_FARLOGIC (farlogic25: the far-logic level; ActorTurnLod.cpp, port/farlogic.h)
    u8 mFlClass = 0xFF;          // port::farlogic::Class | Why << 2 as of the last read of the actor's parts (0xFF: not read yet)
    u8 mFlFam = 0;               // the family of its marked class (MR::portMarkActorLod records it whatever GXV_ALOD says; 0: none)
    u8 mFlState = 0;             // far state bits: 1 the level in force, 2 / 4 the dry mode's levels 1 / 2 (hysteresis), 8 timed this turn
    u8 mFlAge = 0;               // turns since the class was read (mod 32)
    u32 mAlodSkipTurn = 0;       // the turn (gPortAlodTurn) whose movement was left out: its calcAnim is left out too
    const void* mAlodNerve = nullptr;  // the nerve after its last movement (census: changed from outside while it stood behind)
    f32 mAlodReach = -1.0f;      // its sensors' reach (farthest offset + radius; <0: not known yet)
    void portMovementLite();     // a left-out turn's movement, replayed (LiveActor.cpp)
    void portMovementPhased();   // (GXV_LOGIC_PROF=2, logic23) movement() with each phase timed (LiveActor.cpp)
#endif
};
