#pragma once

#include <JSystem/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/JGeometry/TVec.hpp>

class J3DModelData;
class J3DAnmTransform;

class XtransformInfo {
public:
    XtransformInfo();

    /* 0x00 */ TVec3f _0;
    /* 0x0C */ TVec3f _C;
    /* 0x18 */ Quaternion mRotation;
};

class XjointTransform {
public:
    XjointTransform();

    void setLocalScale(f32 scale) {
        mScale.set(scale);
    }

    /* 0x00 */ J3DJoint* _0;
    /* 0x04 */ u16 _4;
    /* 0x08 */ TVec3f mScale;
    /* 0x14 */ TVec3f _14;
    /* 0x20 */ TVec3f _20;
    /* 0x2C */ TVec3f _2C;
    /* 0x38 */ TVec3f _38;
    /* 0x44 */ J3DTransformInfo _44;
    /* 0x64 */ MtxPtr _64;
    /* 0x68 */ u32 _68;
    /* 0x6C */ MtxPtr _6C;
};

class XjointInfo {
public:
    XjointInfo();

    /* 0x00 */ XtransformInfo _0;
    /* 0x28 */ XtransformInfo _28;
    /* 0x50 */ TVec3f _50;
    /* 0x5C */ f32 _5C;
    /* 0x60 */ f32 _60;
};

class XanimeTrack {
public:
    XanimeTrack() {
    }

    void init();
    void setFrame(f32 frame) {
        _8 = frame;
        _C = 1;
    }

    f32 getWeight() const {
        return mWeight;
    }

    /* 0x0 */ J3DAnmTransform* _0;
    /* 0x4 */ f32 mWeight;
    /* 0x8 */ f32 _8;
    /* 0xC */ u8 _C;
};

class XanimeCore : public J3DMtxCalc {
public:
    XanimeCore(u32, u32, u8);
    XanimeCore(u32, XanimeCore*);

    virtual ~XanimeCore();
    virtual void setWeight(u8, f32);
    virtual void init(const Vec&, const Mtx&);
    virtual void calc();

    void shareJointTransform(const XanimeCore*);
    void doFreeze();
    void setBck(u32, J3DAnmTransform*);
    void calcBlend(TVec3f*, TVec3f*);
    void calcSingle(TVec3f*, TVec3f*);
    void calcBlendSpecial();
    void calcScaleBlendMaya(const TVec3f&, const TVec3f&);
    void calcScaleBlendMayaNoTransform(const TVec3f&, const TVec3f&);
    void calcScaleBlendSI(const TVec3f&, const TVec3f&);
    void calcScaleBlendBasic(const TVec3f&, const TVec3f&);
    void calcScaleBlendSpecial();
    void freezeCopy(J3DModelData*, XanimeCore*, u32, u32);
    void initT(J3DModelData*);
    void reconfigJointTransform(J3DModelData*);
    void updateFrame();
    void fixT(TVec3f*);

    void enableJointTransform(J3DModelData*);

    void initMember(u32);

    XjointTransform* getJointTransform(u32 index) {
        if (mTransformList == nullptr) {
            return nullptr;
        }

        return &mTransformList[index];
    }

    /* 0x4 */ u8 _4;
    /* 0x5 */ u8 mTrackCount;
    /* 0x6 */ u8 _6;
    /* 0x8 */ u32 mJointCount;
    /* 0xC */ u16 _C;
    /* 0x10 */ XjointInfo* mJointList;
    /* 0x14 */ XjointTransform* mTransformList;
    /* 0x18 */ XanimeTrack* mTrackList;
    /* 0x1C */ f32 _1C;
    /* 0x20 */ f32 _20;
    /* 0x24 */ f32 mFrameRatio;
    /* 0x28 */ u8 _28;
    /* 0x29 */ u8 _29;

#ifdef TARGET_PC
    // blendfix31 (GXV_POSE_BLENDFIX, 17 Nov 2026; XanimeCore.cpp): the joint state (_0, _28, _50, _5C) of a computation that a pose cut left out.
    // A computation writes the last pose _28; the next change of animation copies it as the start of the blend (doFreeze -> updateFrame's _29 ->
    // calcSingle / calcBlend's _0 = _28), and a few computations read it again (calcBlend's _20 != 1, a track 0 without a resource or a total weight
    // of 0, the special modes _6 1 / 2). A cut that leaves a computation out (LiveActor::calcAnim's rules: PAIRPOSE, UNDRAWN_LEAN / ANIM_LOD, PB_ANIM,
    // FARANIM, CHARPOSE, FAMPOSE, far-logic 3, POSE_PHASE, STARBATCH_FAR, BOWL_POSE) records its inputs here (portLeaveOut: what XanimePlayer::calcAnm
    // and updateFrame would have used: the frame ratio, _1C, _20, the tracks), and the joint state it would have written is made from them, without
    // matrices, only when something is about to read it: a change of animation (doFreeze), a computation that reads the last pose (updateFrame),
    // another left-out computation that reads it (portLeaveOut), freezeCopy. Otherwise the next computation overwrites it all and nothing is made.
    enum { kPortOwedTracks = 4 };
    void portLeaveOut(f32 frameRatio);
    void portCatchUp(int why);
    bool portReadsLast() const;
    // ondraw31 (GXV_POSE_ONDRAW): the left-out computation itself, matrices included, from its record (updateFrame on the recorded inputs, then calc(ctx):
    // the model's joint tree walked with this core on the root, as XanimePlayer::calcAnm + J3DModel::calc walk it; the live inputs and the shared BCK
    // frames put back). For a first reader of the joints after the actor's own update ran. false: no intact record (an animation change caught it up).
    bool portRemake(void (*calc)(void*), void* ctx, J3DJoint* root);
    /* PC */ u8 mPortOwed;                              // 1: a left-out computation's joint state is owed (inputs below); 2: owed, and the next
                                                        // computation is that one itself (the flush: no catch-up then; a change before it still makes one)
    /* PC */ u8 mPortOwedTracks;                        // (mTrackCount at the time)
    /* PC */ u8 mPortShared;                            // its joint list is another core's too (XanimeCore(u32, XanimeCore*)): never owed
    /* PC */ u16 mPortCatchUps;                         // (counts the catch-ups that changed the core: a word of GXV_ANIM_MEMO / GXV_PAR_ANIM's key)
    /* PC */ f32 mPortOwedRatio, mPortOwed1C, mPortOwed20;
    /* PC */ XanimeTrack mPortOwedTrack[kPortOwedTracks];
#endif
};

#ifdef TARGET_PC
// blendfix31: the mode (LiveActor.cpp sets it from GXV_POSE_BLENDFIX at a left-out computation: 0 off, 1 the fix, 2 measure only: the owed joint state is
// made at a change of animation, compared with the stale one and thrown away, so the game runs as with 0), and the census (the "pose-blendfix:" line).
struct XanimePortBlendCensus {
    u32 leftOut;      // computations left out with their inputs recorded
    u32 atChange;     // made at a change of animation (doFreeze)
    u32 changed;      //   of which the blend start differed from the stale one (measured: census on, or mode 2)
    u32 atRead;       // made before a computation that reads the last pose
    u32 chained;      // made at a second left-out computation that reads the last pose
    u32 eager;        // made at once (more tracks than recorded)
    u32 atCopy;       // made before freezeCopy
    u32 paid;         // the drawn turn's flush made the left-out computation itself
    u32 cleared;      // overwritten by a computation that does not read it
    u32 shared;       // left out, but its joint list is shared (not recorded: as before)
    u32 noMemory;     // a catch-up without scratch memory (not made: as before)
    f32 maxDeg;       // the largest joint rotation difference at a blend start (degrees)
    f32 maxTrans;     // the largest joint translation difference at a blend start (model units)
    f64 sumDeg;       // (mean over the changed ones)
};
extern u8 gPortBlendFix;
extern u8 gPortBlendFixMeasure;
extern XanimePortBlendCensus gPortBlendCensus;
enum { kPortCatchChange = 0, kPortCatchRead = 1, kPortCatchChain = 2, kPortCatchEager = 3, kPortCatchCopy = 4 };
#endif
