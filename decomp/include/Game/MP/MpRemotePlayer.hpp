// Galaxy 3.0 multiplayer: puppets (owner: puppet). A puppet draws one player's character (MpLook) from STATE samples, or, for the
// "self puppet", from the local player's pose captured the same turn (every console plays with Mario's physics; a non-Mario
// character is drawn by a puppet on every console including the player's own). Created by MpPuppets::initScene().
//
//   MpPuppetBase    : pose (position, rotation, scale, gravity), visibility, interaction (sensors, BUMP), lifecycle.
//   MpRemotePlayer  : Mario / Luigi / Cosmic Mario / Cosmic Luigi with XanimePlayer and Mario's animation tables (GhostPlayer's method), drawn
//                     by MpPuppetDrawer in DrawType_Player.
//   MpToadPuppet    : Toad, red or yellow (Kinopio model) with Mario's state retargeted onto Toad's own animations (MpToadMap), drawn by the
//                     engine (NPC draw buffer).
#pragma once

#include "Game/LiveActor/LiveActor.hpp"
#include "Game/MP/MpPoseMath.hpp"

class HitSensor;
class J3DAnmTransform;
class J3DShape;
class XanimePlayer;

class MpPuppetBase : public LiveActor {
public:
    MpPuppetBase(const char* pName, int slot, u8 character, bool self);

    virtual void appear();
    virtual void control();
    virtual void calcAndSetBaseMtx();
    virtual void attackSensor(HitSensor* pSender, HitSensor* pReceiver);
    virtual bool receiveMsgPlayerAttack(u32 msg, HitSensor* pSender, HitSensor* pReceiver);
    virtual bool receiveMsgPush(HitSensor* pSender, HitSensor* pReceiver);
    virtual bool receiveMsgEnemyAttack(u32 msg, HitSensor* pSender, HitSensor* pReceiver);
    virtual bool receiveOtherMsg(u32 msg, HitSensor* pSender, HitSensor* pReceiver);

    virtual bool isDrawable() const;
    virtual bool usesDrawer() const { return false; }   // drawn by MpPuppetDrawer (else by the engine)
    virtual void drawPuppet() const {}
    virtual void warmDraw(int variant) {}   // MpWarm: one draw out of sight while dead (0 solid, 1 see-through); engine-drawn puppets: none
    void headPosition(TVec3f* pOut) const;
    void capPosition(TVec3f* pOut) const;   // touches: the top of the cap/head (the name tag hangs from here)
    // MP3 spectate: the puppet's frame this turn (the base rotation's up and facing columns): the watch camera's target reads it
    void getFrame(TVec3f* pUp, TVec3f* pFront) const {
        pUp->set(mRotMtx[0][1], mRotMtx[1][1], mRotMtx[2][1]);
        pFront->set(mRotMtx[0][2], mRotMtx[1][2], mRotMtx[2][2]);
    }
    bool isCulled() const { return mCulled; }   // (perf) off screen this turn: the drawer leaves it out
    bool isFar() const { return mFar; }         // MP3 lod2: beyond the far distance (hysteresis)
    void lodCenter(TVec3f* pOut) const;         // MP3 lod2: the body's middle where it is drawn (the distance the level of detail uses)
    int getSlot() const { return mSlot; }
    u8 getCharacter() const { return mCharacter; }
    bool isSelf() const { return mSelf; }
    // drop-in (MpPuppets): a spare puppet of a free character waits unbound (slot -1, dead) from the scene's init; a player who
    // joins mid-scene with that character gets it. Never for the self puppet.
    void bindSlot(int slot);
    void startArrival();                       // mid-scene arrival: grows (and fades) in at its first appearance
    bool isArriving() const { return mArrive != 0; }
    void setStandIn(bool on) { mStandIn = on; }   // drawn see-through: it stands in for a character this scene has no model of
    // MP3 formation: the drawn pose (model, shadow, name tag, speech balloon). step() sets it to the true pose (mPosition, the
    // STATE's rotation) every turn; MpFormation may then move it for some personal space. Sensors, bumps, the bubble: true pose.
    void setDrawPose(const TVec3f& rPos, const f32 rot[3][3]);
    void trueRotation(f32 out[3][3]) const;
    u16 poseFlags() const { return mFlags; }   // the last STATE's MpPoseFlags

protected:
    void initCommon(f32 sensorRadius, f32 sensorHeight, f32 shadowRadius);
    void step();   // sample + apply: remote puppets in control(), the self puppet at the start of calcAnim (after Mario's)
    virtual void applyAnimation(const MpPoseF& rPose) = 0;
    virtual void onHiddenChanged() {}
    virtual f32 headHeight() const { return 190.0f; }
    virtual f32 capHeight() const { return headHeight() - 30.0f; }   // touches: where the head really ends (Mario 160)
    virtual f32 modelScale() const { return 1.0f; }
    void updateInteraction();
    void setShadowMode(u8 mode);   // MP3 lod2: MpLod::ShadowMode: validates the volume or the round shadow
    // (perf) level of detail, asked at the start of calcAnim: false when this turn's joints can be left out (hidden, off screen,
    // no picture this turn, or far away on every other picture). Sets mCulled / mFar.
    bool lodWantsCalc();
    u8 arrivalAlpha() const;   // 255 = opaque (no arrival running)

    int mSlot;
    u8 mArrive;              // drop-in arrival: visible turns left (0 = none)
    bool mStandIn;           // bound as a family stand-in (see-through)
    u8 mCharacter;           // MpLook
    bool mSelf;
    bool mHidden;            // sample says not visible, the race setting hides rivals, or no sample
    bool mMirror;
    bool mSensorsOn;
    bool mShadowOn;
    bool mAnimated;          // a pose was applied and the model calculated since appear()
    bool mCulled;            // (perf) off screen or hidden at the last calcAnim: not drawn
    bool mFar;               // (perf) far from the camera (hysteresis): no shadow, joints on every other picture
    bool mStale;             // (perf) joints left out while culled: computed at the next picture whatever the distance
    bool mHasBlob;           // MP3 lod2: the round surface shadow exists beside the volume (SMG_MP_LOD2_SHADOW)
    bool mLightSkipped;      // MP3 lod2: the light control was not updated while the puppet was skipped: update it when it returns
    bool mLightInit;         // first light update after appear / bindSlot: snap immediately, then blend smoothly (update(false))
    u8 mShadowMode;          // MP3 lod2: MpLod::ShadowMode applied now
    u8 mForm;
    u16 mFlags;
    u16 mTrampleCooldown;
    u16 mSpinCooldown;
    bool mSpringOn;          // MP3 coop: the head's spring sensor ("jump", ATYPE_PLAYER_AUTO_JUMP) is valid: super jump
    u32 mRainbow;
    Mtx mRotMtx;
    TVec3f mDrawPos;         // MP3 formation: drawn pose (see setDrawPose)
    Mtx mDrawRotMtx;
};

class MpRemotePlayer : public MpPuppetBase {
public:
    MpRemotePlayer(int slot, u8 character, bool self);

    virtual void init(const JMapInfoIter& rIter);
    virtual void appear();
    virtual void calcAnim();
    virtual bool usesDrawer() const { return true; }
    virtual void drawPuppet() const;   // MpPuppetDrawer, inside DrawType_Player, after loadActorLight(this)
    virtual void warmDraw(int variant);

    enum { SHOW_SHAPES_MAX = 24 };

protected:
    virtual void applyAnimation(const MpPoseF& rPose);

private:
    void initAnimation();
    void setFrame(XanimePlayer* pPlayer, f32 frame) const;
    void buildDL() const;
    void calcPoseNow();   // the joints of the pose already applied (calcAnim after the level-of-detail test)
    bool blendRunning() const;   // MP3 lod2: a blend is running in the cores (the one model pass would not match the two)
    void checkPasses();          // MP3 lod2: SMG_MP_LOD2_CHECK=1: one pass against two, bit for bit
    bool isCosmic() const { return mp_character_is_cosmic(mCharacter); }
    bool usesLuigiSwap() const { return mp_character_luigi_swap(mCharacter); }

    bool mUpperActive;
    bool mCurSimple;
    bool mHasColorBtp;
    u8 mBtpForm;
    u32 mCurAnim;            // applied lower animation (hash), 0 = none yet
    u32 mCurUpper;
    u32 mMissHash;           // last unknown hash (logged once)
    u32 mTrackHash[4];
    J3DAnmTransform* mTrackRes[4];
    u16 mJointSpine1;
    u16 mJointPartsControl;
    XanimePlayer* mXanimePlayer;
    XanimePlayer* mXanimePlayerUpper;
    Mtx mMirrorMtx;
    // shapes of the body's Face0 / HandL0 / HandR0 joints: hidden in the shared model data by the local Mario (who draws separate
    // face and hand models); a puppet shows them for its own draw and restores the flags afterwards
    J3DShape* mShowShapes[SHOW_SHAPES_MAX];
    int mShowShapeCount;
    u8* mDL[2];
    mutable u32 mDLSize;
    mutable int mCurDL;
    mutable u64 mDLKey;      // (perf) what the current display list was built from (style, colours): rebuilt only on a change
    mutable bool mDLGhost;
    bool mWarm;              // MpWarm: this draw is the out-of-sight warm-up of a dead puppet
    bool mWarmGhost;         // ... with the see-through list
    mutable u8 mDLAlpha;     // alpha of the see-through list (Star Race ghost 0x80, a drop-in's fade-in ramp)
};

class MpToadPuppet : public MpPuppetBase {
public:
    MpToadPuppet(int slot, u8 character, bool self);   // MP_LOOK_TOAD (red) or MP_LOOK_YELLOW_TOAD

    virtual void init(const JMapInfoIter& rIter);
    virtual void appear();
    virtual void calcAnim();

protected:
    virtual void applyAnimation(const MpPoseF& rPose);
    virtual void onHiddenChanged();
    virtual f32 headHeight() const { return 125.0f; }
    virtual f32 capHeight() const { return 105.0f; }   // touches: Toads are shorter
    virtual f32 modelScale() const;

private:
    int mAnim;               // MpToadAnim, -1 none
    f32 mHop;
    const char* mClassBck;   // MP3 profile: the bck name mClass was worked out for (names are stable within a scene)
    int mClass;              // mp_toad_name_class(mClassBck)
};
