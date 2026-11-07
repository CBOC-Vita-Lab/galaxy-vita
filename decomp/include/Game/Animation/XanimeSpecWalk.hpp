#pragma once
// GXV_PAR_ANIM (joint15, 5 Nov; decomp/src/Game/LiveActor/ModelManager.cpp, platform/include/port/par_spec.h): a reentrant copy of
// J3DModel::calc's joint walk (J3DJointTree::calc -> J3DJoint::recursiveCalc -> XanimeCore::calc -> calcSingle / calcBlend ->
// calcScaleBlendBasic / SI / MayaNoTransform, then J3DMtxBuffer::calcWeightEnvelopeMtx) for the models GXV_PAR_ANIM qualifies,
// so that a worker thread can compute a model's joints ahead of the game thread, into memory of its own.
//
// What the original reads from shared state, and what this copy takes instead (everything it reads is either passed in by value
// or does not change while a scene runs):
//   - J3DSys::mCurrentMtx / mCurrentS / mParentS (the running matrix and scales): a context of the walk's own (Ctx);
//   - J3DMtxCalc::mMtxBuffer / mJoint, J3DJoint::mCurrentMtxCalc: the output arrays and the joint are passed down;
//   - J3DJoint::mMtxCalc (the shared root joint's core, set by XanimePlayer::calcAnm): the core's parameters are in the Input;
//   - J3DAnmTransform::mFrame (set by XanimeCore::updateFrame on the SHARED BCK resource): each track's frame is in the Input,
//     and the key search is J3DAnmTransformKey::calcTransform(frame, ...), the function getTransform calls with mFrame;
//   - XanimeCore::mJointList (_28, _50 written per joint): written into Output::joint; the qualifying cores never read it here
//     (no blend in progress, no freeze copy, no null track, no zero total weight: ModelManager.cpp checks all of these);
//   - j3dSys.mModel (fixT's model data): the data's joint tree is in the Input.
// Read and unchanged while a scene runs: the joint tree (children, siblings, joint numbers, scale compensation, rest transforms),
// the BCK key tables, the envelope tables and inverse joint matrices, JMath's sine table. The worker writes only the Output.
//
// The arithmetic is the original's, statement for statement, through the same functions (JMAEulerToQuat, JMAQuatLerp, PSMTXQuat,
// JMAMTXApplyScale, PSMTXConcat, PSMTXCopy, the TVec3f operators, JMath::fastReciprocal, calcTransform, and for the envelopes the
// member J3DMtxBuffer::calcWeightEnvelopeMtx itself over a buffer whose arrays are the Output's): the same bytes. The host test
// tests/par_anim_walk.cpp compiles the real XanimeCore / J3DJoint / J3DJointTree / J3DAnimation / J3DMtxBuffer next to this
// file and compares them byte for byte; GXV_PAR_ANIM_VERIFY=1 does the same on the console.
//
// Only the branches the qualifying models take are here. A recursion over the children is kept; the siblings are a loop (the
// original recurses into mYounger after it has restored the state, so each sibling starts from the parent's state: the same).
//
// GXV_PAR_ANIM_BLEND (par2, 6 Nov): a job with Input::prev also takes the cores the plain walk refuses, whose result depends on the
// core's per-joint state as well: an interpolation in progress (_1C < 1: the frozen pose _0; calcBlend's _20 != 1: the last pose
// _28), the freeze copy (_29: _0 = _28 first), track 0 without a resource (calcSingle) and a total weight of 0 (calcBlend: the last
// pose again). That state (_0, _28, _50 of every joint) is read from Input::prev, a copy taken when the job is made, and the walk
// writes the whole of it into Output::full (what the original leaves in mJointList). ModelManager.cpp takes such a job only when
// the core's state at the model's real computation is byte for byte that copy (and the key, which then also has _29, matches), so the
// original would have read the same bytes. The arithmetic is calcSingle's / calcBlend's own, in their order, with the same
// functions (MR::vecBlend, JMAQuatLerp, the TVec3f operators).
//
// GXV_PAR_ANIM_CB (par2, 6 Nov): Input::cbKind 1 is a joint callback the walk performs itself: NPCActor::calcJointScale registered
// through JointControlDelegator< NPCActor > (JointController::staticCallBack). At timing 0 (right after the joint's own computation,
// J3DJoint::recursiveCalc) the original copies the joint's matrix into a TPos3f, scales its columns (MR::preScaleMtx by the
// actor's AnimScaleController::_C, passed in Input::cbScale), and copies the result into the joint matrix and J3DSys::mCurrentMtx;
// at timing 1 the delegator has no after-child function (nothing). The callback's unregistering is done by the caller (ModelManager.cpp).

#include "Game/Animation/XanimeCore.hpp"
#include <JSystem/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3DGraphAnimator/J3DJointTree.hpp>
#include <JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp>
#include <JSystem/J3DGraphAnimator/J3DMtxCalc.hpp>
#include <JSystem/J3DGraphBase/J3DSys.hpp>
#include <JSystem/JGeometry/TVec.hpp>
#include <JSystem/JMath/JMath.hpp>
#include "Game/Util/MathUtil.hpp"  // (MR::vecBlend: GXV_PAR_ANIM_BLEND)
#include "Game/Util/MtxUtil.hpp"   // (MR::preScaleMtx: GXV_PAR_ANIM_CB)
#include <JSystem/JGeometry/TMatrix.hpp>
#include <cstring>

namespace xspec {

enum { kMaxTracks = 8 };
// The basic matrix calculator's init (J3DModelData flags & 0xF, J3DModelLoader::readInformation): 0 Basic, 1 Softimage, 2 Maya.
enum { kInitBasic = 0, kInitSoftimage = 1, kInitMaya = 2 };

struct Track {
    const J3DAnmTransformKey* res;  // (null: no resource on this track)
    f32 weight;
    f32 frame;  // what the resource's mFrame holds when the original computes
};

// What XanimeCore::calc leaves in mJointList[joint]: _28 (scale, translation, rotation) and _50 (the track translation).
struct JointState {
    f32 t28[10];
    f32 t50[3];
};

// (GXV_PAR_ANIM_BLEND) mJointList[joint]'s _0 (the frozen pose), _28 (the last pose) and _50, before (Input::prev) or after
// (Output::full) the computation. 23 floats: XtransformInfo, XtransformInfo, TVec3f.
struct JointPrev {
    f32 t0[10];
    f32 t28[10];
    f32 t50[3];
};
inline XtransformInfo& Xt(f32* p) { return *reinterpret_cast< XtransformInfo* >(p); }  // (the project builds with -fno-strict-aliasing)
inline TVec3f& Tv(f32* p) { return *reinterpret_cast< TVec3f* >(p); }

struct Input {
    J3DJointTree* dataTree;  // the model data's tree: the walk's joints, and fixT's rest translations
    J3DJointTree* bufTree;   // the matrix buffer's tree: the envelopes (J3DMtxBuffer::calcWeightEnvelopeMtx)
    J3DJoint* root;
    Track track[kMaxTracks];
    u8 trackCount;  // XanimeCore::mTrackCount (1: calcSingle, else calcBlend)
    u8 type4;       // XanimeCore::_4 (0 Basic, 1 SI, 2 Maya; no transform list)
    u8 mode6;       // XanimeCore::_6 (0, or 3: fixT)
    u8 initKind;    // kInit*
    u8 doEnv;       // J3DModel::calcWeightEnvelopeMtx computes the envelopes
    u16 fixJoint;   // XanimeCore::_C
    u16 jointNum;
    f32 blend1C;    // XanimeCore::_1C (never < 1 here)
    f32 blend20;    // XanimeCore::_20 (always 1 here)
    Vec scale;      // the model's base scale
    Mtx base;       // and base matrix
    Vec parentS;    // J3DSys::mParentS when the original starts (read by the Softimage init's walk only)
    const JointPrev* prev;  // (GXV_PAR_ANIM_BLEND) null: the plain walk (the conditions above); else the core's joint state, jointNum
    u8 freeze29;            // (with prev) XanimeCore::_29 at the computation (updateFrame sets it from the freeze flag _28)
    u8 cbKind;              // (GXV_PAR_ANIM_CB) 0 none, 1 NPCActor::calcJointScale on joint cbJoint
    u16 cbJoint;
    Vec cbScale;            // (kind 1) AnimScaleController::_C
};

struct Output {
    Mtx* anm;          // jointNum
    u8* scaleFlag;     // jointNum
    JointState* joint; // jointNum (the plain walk)
    JointPrev* full;   // jointNum (Input::prev set: the whole joint state the original leaves)
    Mtx* env;          // envelopes (doEnv)
    u8* envScale;
    // The statics the original leaves behind (J3DModel::calc's bookkeeping, set by Apply).
    Mtx curMtx;
    Vec curS;
    Vec parentS;
    J3DJoint* lastJoint;  // J3DMtxCalc::mJoint
    u32 visited;
};

struct Ctx {
    const Input* in;
    Output* out;
    Mtx cur;      // J3DSys::mCurrentMtx
    Vec curS;     // J3DSys::mCurrentS
    Vec parentS;  // J3DSys::mParentS
    J3DJoint* last;
    u32 visited;
    bool bad;
};

inline void PutState(JointState& st, const TVec3f& scale, const TVec3f& translate, const Quaternion& rotation) {
    st.t28[0] = scale.x;
    st.t28[1] = scale.y;
    st.t28[2] = scale.z;
    st.t28[3] = translate.x;
    st.t28[4] = translate.y;
    st.t28[5] = translate.z;
    st.t28[6] = rotation.x;
    st.t28[7] = rotation.y;
    st.t28[8] = rotation.z;
    st.t28[9] = rotation.w;
}

// XanimeCore::calcSingle (track 0 has a resource; _29 == 0; _1C >= 1).
// (GXV_PAR_ANIM_BLEND) calcSingle with the core's joint state (Input::prev): every branch.
inline void CalcSingleFull(Ctx& c, u16 joint, TVec3f* pScale, TVec3f* pTranslate) {
    const Input& in = *c.in;
    MtxPtr pMtx = c.out->anm[joint];
    J3DTransformInfo transform;
    Quaternion rotation;
    JointPrev& o = c.out->full[joint];
    o = in.prev[joint];  // (what mJointList[joint] holds when the original reaches this joint)

    if (in.track[0].res == nullptr) {
        *pScale = Xt(o.t28)._0;
        *pTranslate = Xt(o.t28)._C;
        PSMTXQuat(pMtx, &Xt(o.t28).mRotation);
        return;
    }

    in.track[0].res->calcTransform(in.track[0].frame, joint, &transform);
    JMAEulerToQuat(transform.mRotation.x, transform.mRotation.y, transform.mRotation.z, &rotation);
    *pScale = reinterpret_cast< const TVec3f& >(transform.mScale);
    *pTranslate = reinterpret_cast< const TVec3f& >(transform.mTranslate);

    if (in.freeze29 != 0) {
        Xt(o.t0) = Xt(o.t28);
    }

    Tv(o.t50) = *pTranslate;
    f32 blend = in.blend1C;

    if (blend < 1.0f) {
        MR::vecBlend(Xt(o.t0)._0, *pScale, pScale, blend);
        MR::vecBlend(Xt(o.t0)._C, *pTranslate, pTranslate, blend);
        JMAQuatLerp(&Xt(o.t0).mRotation, &rotation, blend, &rotation);
    }

    Xt(o.t28)._0 = *pScale;
    Xt(o.t28)._C = *pTranslate;
    Xt(o.t28).mRotation = rotation;
    PSMTXQuat(pMtx, &rotation);
}

inline void CalcSingle(Ctx& c, u16 joint, TVec3f* pScale, TVec3f* pTranslate) {
    const Input& in = *c.in;
    MtxPtr pMtx = c.out->anm[joint];
    J3DTransformInfo transform;
    Quaternion rotation;

    in.track[0].res->calcTransform(in.track[0].frame, joint, &transform);
    JMAEulerToQuat(transform.mRotation.x, transform.mRotation.y, transform.mRotation.z, &rotation);
    *pScale = reinterpret_cast< const TVec3f& >(transform.mScale);
    *pTranslate = reinterpret_cast< const TVec3f& >(transform.mTranslate);

    JointState& st = c.out->joint[joint];
    st.t50[0] = pTranslate->x;
    st.t50[1] = pTranslate->y;
    st.t50[2] = pTranslate->z;
    PutState(st, *pScale, *pTranslate, rotation);
    PSMTXQuat(pMtx, &rotation);
}

// XanimeCore::calcBlend (the tracks' total weight is not 0; _29 == 0; _1C >= 1; _20 == 1).
inline void CalcBlend(Ctx& c, u16 joint, TVec3f* pScale, TVec3f* pTranslate) {
    const Input& in = *c.in;
    MtxPtr pMtx = c.out->anm[joint];
    J3DTransformInfo transform;
    Quaternion rotation;
    f32 inverseWeight;
    f32 weight;
    f32 accumulatedWeight = 0.0f;
    pScale->z = 0.0f;
    pScale->y = 0.0f;
    pScale->x = 0.0f;
    pTranslate->z = 0.0f;
    pTranslate->y = 0.0f;
    pTranslate->x = 0.0f;
    rotation.z = 0.0f;
    rotation.y = 0.0f;
    rotation.x = 0.0f;
    rotation.w = 1.0f;

    f32 totalWeight = 0.0f;

    for (s32 i = 0; i < in.trackCount; i++) {
        if (in.track[i].res != nullptr) {
            totalWeight += in.track[i].weight;
        }
    }

    inverseWeight = 1.0f / totalWeight;

    for (s32 i = 0; i < in.trackCount; i++) {
        const Track* pTrack = &in.track[i];

        if (pTrack->res != nullptr && pTrack->weight != 0.0f) {
            pTrack->res->calcTransform(pTrack->frame, joint, &transform);
            Quaternion trackRotation;
            JMAEulerToQuat(transform.mRotation.x, transform.mRotation.y, transform.mRotation.z, &trackRotation);

            weight = inverseWeight * pTrack->weight;
            *pScale += reinterpret_cast< const TVec3f& >(transform.mScale) * weight;
            *pTranslate += reinterpret_cast< const TVec3f& >(transform.mTranslate) * weight;
            accumulatedWeight += weight;
            JMAQuatLerp(&rotation, &trackRotation, weight / accumulatedWeight, &rotation);
        }
    }

    JointState& st = c.out->joint[joint];
    st.t50[0] = pTranslate->x;
    st.t50[1] = pTranslate->y;
    st.t50[2] = pTranslate->z;
    PutState(st, *pScale, *pTranslate, rotation);
    PSMTXQuat(pMtx, &rotation);
}

// (GXV_PAR_ANIM_BLEND) calcBlend with the core's joint state (Input::prev): every branch.
inline void CalcBlendFull(Ctx& c, u16 joint, TVec3f* pScale, TVec3f* pTranslate) {
    const Input& in = *c.in;
    MtxPtr pMtx = c.out->anm[joint];
    J3DTransformInfo transform;
    Quaternion rotation;
    f32 inverseWeight;
    f32 weight;
    f32 accumulatedWeight = 0.0f;
    JointPrev& o = c.out->full[joint];
    o = in.prev[joint];
    pScale->z = 0.0f;
    pScale->y = 0.0f;
    pScale->x = 0.0f;
    pTranslate->z = 0.0f;
    pTranslate->y = 0.0f;
    pTranslate->x = 0.0f;
    rotation.z = 0.0f;
    rotation.y = 0.0f;
    rotation.x = 0.0f;
    rotation.w = 1.0f;

    f32 totalWeight = 0.0f;

    for (s32 i = 0; i < in.trackCount; i++) {
        if (in.track[i].res != nullptr) {
            totalWeight += in.track[i].weight;
        }
    }

    if (totalWeight == 0.0f) {
        *pScale = Xt(o.t28)._0;
        *pTranslate = Xt(o.t28)._C;
        PSMTXQuat(pMtx, &Xt(o.t28).mRotation);
        return;
    }

    inverseWeight = 1.0f / totalWeight;

    for (s32 i = 0; i < in.trackCount; i++) {
        const Track* pTrack = &in.track[i];

        if (pTrack->res != nullptr && pTrack->weight != 0.0f) {
            pTrack->res->calcTransform(pTrack->frame, joint, &transform);
            Quaternion trackRotation;
            JMAEulerToQuat(transform.mRotation.x, transform.mRotation.y, transform.mRotation.z, &trackRotation);

            weight = inverseWeight * pTrack->weight;
            *pScale += reinterpret_cast< const TVec3f& >(transform.mScale) * weight;
            *pTranslate += reinterpret_cast< const TVec3f& >(transform.mTranslate) * weight;
            accumulatedWeight += weight;
            JMAQuatLerp(&rotation, &trackRotation, weight / accumulatedWeight, &rotation);
        }
    }

    if (in.freeze29 != 0) {
        Xt(o.t0) = Xt(o.t28);
    }

    Tv(o.t50) = *pTranslate;
    f32 blend = in.blend1C;

    if (blend < 1.0f) {
        const TVec3f* pFrozenScale = &Xt(o.t0)._0;
        const TVec3f* pFrozenTranslate = &Xt(o.t0)._C;
        *pScale = *pFrozenScale * (1.0f - blend) + *pScale * blend;
        *pTranslate = *pFrozenTranslate * (1.0f - blend) + *pTranslate * blend;
        JMAQuatLerp(&Xt(o.t0).mRotation, &rotation, blend, &rotation);
    }

    if (in.blend20 != 1.0f) {
        TVec3f* pPreviousScale = &Xt(o.t28)._0;
        TVec3f* pPreviousTranslate = &Xt(o.t28)._C;
        *pScale = *pPreviousScale * (1.0f - in.blend20) + *pScale * in.blend20;
        *pTranslate = *pPreviousTranslate * (1.0f - in.blend20) + *pTranslate * in.blend20;
        JMAQuatLerp(&Xt(o.t28).mRotation, &rotation, in.blend20, &rotation);
    }

    Xt(o.t28)._0 = *pScale;
    Xt(o.t28)._C = *pTranslate;
    Xt(o.t28).mRotation = rotation;
    PSMTXQuat(pMtx, &rotation);
}

// XanimeCore::calcScaleBlendMayaNoTransform.
inline void ScaleBlendMayaNoTransform(Ctx& c, J3DJoint* pJoint, u16 joint, const TVec3f& rScale, const TVec3f& rTranslate) {
    MtxPtr pMtx = c.out->anm[joint];

    pMtx[0][3] = rTranslate.x;
    pMtx[1][3] = rTranslate.y;
    pMtx[2][3] = rTranslate.z;

    TVec3f scale(rScale);

    if (scale.x == 1.0f && scale.y == 1.0f && scale.z == 1.0f) {
        c.out->scaleFlag[joint] = 1;
    } else {
        c.out->scaleFlag[joint] = 0;
        JMAMTXApplyScale(pMtx, pMtx, scale.x, scale.y, scale.z);
    }

    if (pJoint->getScaleCompensate() == 1) {
        if (c.parentS.x != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(c.parentS.x);
            pMtx[0][0] *= inverseScale;
            pMtx[0][1] *= inverseScale;
            pMtx[0][2] *= inverseScale;
        }

        if (c.parentS.y != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(c.parentS.y);
            pMtx[1][0] *= inverseScale;
            pMtx[1][1] *= inverseScale;
            pMtx[1][2] *= inverseScale;
        }

        if (c.parentS.z != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(c.parentS.z);
            pMtx[2][0] *= inverseScale;
            pMtx[2][1] *= inverseScale;
            pMtx[2][2] *= inverseScale;
        }
    }

    PSMTXConcat(c.cur, pMtx, c.cur);
    PSMTXCopy(c.cur, pMtx);

    c.parentS.x = rScale.x;
    c.parentS.y = rScale.y;
    c.parentS.z = rScale.z;
}

// XanimeCore::calcScaleBlendSI (no transform list).
inline void ScaleBlendSI(Ctx& c, J3DJoint* pJoint, u16 joint, const TVec3f& rScale, const TVec3f& rTranslate) {
    Vec& rCurrentScale = c.curS;
    MtxPtr pMtx = c.out->anm[joint];

    TVec3f scale(rScale);

    if (scale.x == 1.0f && scale.y == 1.0f && scale.z == 1.0f) {
        c.out->scaleFlag[joint] = 1;
    } else {
        c.out->scaleFlag[joint] = 0;
        JMAMTXApplyScale(pMtx, pMtx, scale.x, scale.y, scale.z);
    }

    if (pJoint->getScaleCompensate() == 1) {
        if (c.parentS.x != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(c.parentS.x);
            pMtx[0][0] *= inverseScale;
            pMtx[0][1] *= inverseScale;
            pMtx[0][2] *= inverseScale;
        }

        if (c.parentS.y != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(c.parentS.y);
            pMtx[1][0] *= inverseScale;
            pMtx[1][1] *= inverseScale;
            pMtx[1][2] *= inverseScale;
        }

        if (c.parentS.z != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(c.parentS.z);
            pMtx[2][0] *= inverseScale;
            pMtx[2][1] *= inverseScale;
            pMtx[2][2] *= inverseScale;
        }
    }

    pMtx[0][3] = rTranslate.x * rCurrentScale.x;
    pMtx[1][3] = rTranslate.y * rCurrentScale.y;
    pMtx[2][3] = rTranslate.z * rCurrentScale.z;

    PSMTXConcat(c.cur, pMtx, c.cur);

    rCurrentScale.x *= rScale.x;
    rCurrentScale.y *= rScale.y;
    rCurrentScale.z *= rScale.z;

    if (rCurrentScale.x == 1.0f && rCurrentScale.y == 1.0f && rCurrentScale.z == 1.0f) {
        c.out->scaleFlag[joint] = 1;
        PSMTXCopy(c.cur, pMtx);
    } else {
        c.out->scaleFlag[joint] = 0;
        JMAMTXApplyScale(c.cur, pMtx, rCurrentScale.x, rCurrentScale.y, rCurrentScale.z);
        pMtx[0][3] = c.cur[0][3];
        pMtx[1][3] = c.cur[1][3];
        pMtx[2][3] = c.cur[2][3];
    }
}

// XanimeCore::calcScaleBlendBasic.
inline void ScaleBlendBasic(Ctx& c, u16 joint, const TVec3f& rScale, const TVec3f& rTranslate) {
    MtxPtr pMtx = c.out->anm[joint];

    TVec3f& rCurrentScale = reinterpret_cast< TVec3f& >(c.curS);
    pMtx[0][3] = rTranslate.x;
    pMtx[1][3] = rTranslate.y;
    pMtx[2][3] = rTranslate.z;

    rCurrentScale.x *= rScale.x;
    rCurrentScale.y *= rScale.y;
    rCurrentScale.z *= rScale.z;

    if (rCurrentScale.x == 1.0f && rCurrentScale.y == 1.0f && rCurrentScale.z == 1.0f) {
        c.out->scaleFlag[joint] = 1;
    } else {
        c.out->scaleFlag[joint] = 0;
        JMAMTXApplyScale(pMtx, pMtx, rScale.x, rScale.y, rScale.z);
    }

    PSMTXConcat(c.cur, pMtx, c.cur);
    PSMTXCopy(c.cur, pMtx);
}

// XanimeCore::calc for _6 0 or 3 and no transform list.
inline void CalcJoint(Ctx& c, J3DJoint* pJoint) {
    const Input& in = *c.in;
    const u16 joint = pJoint->getJntNo();
    if (joint >= in.jointNum) {
        c.bad = true;  // (never for a tree ModelManager.cpp has checked; the result is not used)
        return;
    }
    TVec3f scale;
    TVec3f translate;

    if (in.prev != nullptr) {
        if (in.trackCount == 1) {
            CalcSingleFull(c, joint, &scale, &translate);
        } else {
            CalcBlendFull(c, joint, &scale, &translate);
        }
    } else if (in.trackCount == 1) {
        CalcSingle(c, joint, &scale, &translate);
    } else {
        CalcBlend(c, joint, &scale, &translate);
    }

    if (in.mode6 == 3) {
        // XanimeCore::fixT
        if (joint != 0 && joint != in.fixJoint) {
            translate = reinterpret_cast< const TVec3f& >(in.dataTree->getJointNodePointer(joint)->getTransformInfo().mTranslate);
        }
    }

    switch (in.type4) {
    case 1:
        ScaleBlendSI(c, pJoint, joint, scale, translate);
        break;
    case 0:
        ScaleBlendBasic(c, joint, scale, translate);
        break;
    case 2:
        ScaleBlendMayaNoTransform(c, pJoint, joint, scale, translate);
        break;
    }
}

// J3DJoint::recursiveCalc for one joint and its younger siblings (no callbacks, every joint computed by the root's core).
inline void Walk(Ctx& c, J3DJoint* first) {
    for (J3DJoint* pJoint = first; pJoint != nullptr; pJoint = pJoint->getYounger()) {
        Mtx currentMtx;
        Vec currentScale;
        Vec parentScale;
        PSMTXCopy(c.cur, currentMtx);
        currentScale = c.curS;
        parentScale = c.parentS;
        c.last = pJoint;
        ++c.visited;
        CalcJoint(c, pJoint);
        if (c.in->cbKind == 1 && pJoint->getJntNo() == c.in->cbJoint && pJoint->getJntNo() < c.in->jointNum) {
            // JointController::calcJointMatrixAndSetSystem -> NPCActor::calcJointScale (returns true)
            MtxPtr pJointMtx = c.out->anm[pJoint->getJntNo()];
            TPos3f mtx;
            PSMTXCopy(pJointMtx, mtx);
            MR::preScaleMtx(mtx.toMtxPtr(), reinterpret_cast< const TVec3f& >(c.in->cbScale));
            PSMTXCopy(mtx, pJointMtx);
            PSMTXCopy(mtx, c.cur);
        }
        if (pJoint->getChild() != nullptr) {
            Walk(c, pJoint->getChild());
        }
        PSMTXCopy(currentMtx, c.cur);
        c.curS = currentScale;
        c.parentS = parentScale;
    }
}

// J3DModel::calcAnmMtx + calcWeightEnvelopeMtx for a qualifying model, into `out`. false: something the checks should have kept
// out (the result must not be used).
inline bool Run(const Input& in, Output& out) {
    if (in.root == nullptr || in.trackCount == 0 || in.trackCount > kMaxTracks || in.type4 > 2 || (in.mode6 != 0 && in.mode6 != 3) || in.cbKind > 1 ||
        (in.prev == nullptr && (in.blend1C < 1.0f || in.blend20 != 1.0f || in.freeze29 != 0)) || (in.prev != nullptr && out.full == nullptr)) {
        return false;
    }
    Ctx c;
    c.in = &in;
    c.out = &out;
    c.last = nullptr;
    c.visited = 0;
    c.bad = false;
    // The basic calculator's init (J3DMtxCalcJ3DSysInitBasic / Maya / Softimage::init).
    switch (in.initKind) {
    case kInitBasic: {
        Vec unitScale = {1.0f, 1.0f, 1.0f};
        c.curS = in.scale;
        c.parentS = unitScale;
        JMAMTXApplyScale(in.base, c.cur, in.scale.x, in.scale.y, in.scale.z);
        break;
    }
    case kInitMaya:
        c.parentS = (Vec){1.0f, 1.0f, 1.0f};
        c.curS = in.scale;
        JMAMTXApplyScale(in.base, c.cur, in.scale.x, in.scale.y, in.scale.z);
        break;
    case kInitSoftimage:
        c.parentS = in.parentS;
        c.curS = in.scale;
        PSMTXCopy(in.base, c.cur);
        break;
    default:
        return false;
    }
    PSMTXCopy(c.cur, out.curMtx);  // (the root's recursiveCalc restores what the init left)
    out.curS = c.curS;
    out.parentS = c.parentS;

    if (in.track[0].res == nullptr && in.trackCount == 1 && in.prev == nullptr) {
        return false;
    }
    Walk(c, in.root);
    if (c.bad || c.visited != in.jointNum) {
        return false;
    }
    out.lastJoint = c.last;
    out.visited = c.visited;

    if (in.doEnv) {
        // J3DMtxBuffer::calcWeightEnvelopeMtx itself, over the Output's arrays (it reads its tree and those four pointers only).
        J3DMtxBuffer tmp;
        tmp.mJointTree = in.bufTree;
        tmp.mpAnmMtx = out.anm;
        tmp.mpScaleFlagArr = out.scaleFlag;
        tmp.mpWeightEvlpMtx = out.env;
        tmp.mpEvlpScaleFlagArr = out.envScale;
        tmp.calcWeightEnvelopeMtx();
    }
    return true;
}

// What a computed Output becomes on the game thread, at the place of the original J3DModel::calc's joint computation: the buffer's
// joint, scale-flag and envelope arrays, the core's per-joint state, and the statics the original leaves behind (the running
// matrix and scales as the root restored them, the matrix buffer, the last joint, the current calculators).
inline void Apply(const Output& out, u16 jointNum, u16 envNum, bool doEnv, J3DMtxBuffer* buf, XanimeCore* core, J3DMtxCalc* basic) {
    memcpy(buf->mpAnmMtx, out.anm, jointNum * sizeof(Mtx));
    memcpy(buf->mpScaleFlagArr, out.scaleFlag, jointNum);
    if (doEnv) {
        memcpy(buf->mpWeightEvlpMtx, out.env, envNum * sizeof(Mtx));
        memcpy(buf->mpEvlpScaleFlagArr, out.envScale, envNum);
    }
    if (out.full != nullptr) {
        for (u16 j = 0; j < jointNum; j++) {
            XjointInfo& ji = core->mJointList[j];
            memcpy(&ji._0, out.full[j].t0, sizeof(out.full[j].t0));
            memcpy(&ji._28, out.full[j].t28, sizeof(out.full[j].t28));
            memcpy(&ji._50, out.full[j].t50, sizeof(out.full[j].t50));
        }
    } else {
        for (u16 j = 0; j < jointNum; j++) {
            XjointInfo& ji = core->mJointList[j];
            memcpy(&ji._28, out.joint[j].t28, sizeof(out.joint[j].t28));
            memcpy(&ji._50, out.joint[j].t50, sizeof(out.joint[j].t50));
        }
    }
    PSMTXCopy(out.curMtx, J3DSys::mCurrentMtx);
    J3DSys::mCurrentS = out.curS;
    J3DSys::mParentS = out.parentS;
    J3DMtxCalc::setMtxBuffer(buf);
    J3DMtxCalc::setJoint(out.lastJoint);
    J3DJoint::mCurrentMtxCalc = basic;
    j3dSys.mCurrentMtxCalc = core;
}

// GXV_PAR_ANIM_VERIFY: the differences between an Output and what the original left (0: none). Bit 0 joints, 1 scale flags,
// 2 envelopes, 3 envelope scale flags, 4 core state, 5 statics.
inline u32 Compare(const Output& out, u16 jointNum, u16 envNum, bool doEnv, J3DMtxBuffer* buf, XanimeCore* core, J3DMtxCalc* basic) {
    u32 bad = 0;
    if (memcmp(buf->mpAnmMtx, out.anm, jointNum * sizeof(Mtx)) != 0) bad |= 1;
    if (memcmp(buf->mpScaleFlagArr, out.scaleFlag, jointNum) != 0) bad |= 2;
    if (doEnv) {
        if (memcmp(buf->mpWeightEvlpMtx, out.env, envNum * sizeof(Mtx)) != 0) bad |= 4;
        if (memcmp(buf->mpEvlpScaleFlagArr, out.envScale, envNum) != 0) bad |= 8;
    }
    for (u16 j = 0; j < jointNum; j++) {
        const XjointInfo& ji = core->mJointList[j];
        if (out.full != nullptr ? (memcmp(&ji._0, out.full[j].t0, sizeof(out.full[j].t0)) != 0 || memcmp(&ji._28, out.full[j].t28, sizeof(out.full[j].t28)) != 0 ||
                                   memcmp(&ji._50, out.full[j].t50, sizeof(out.full[j].t50)) != 0)
                                : (memcmp(&ji._28, out.joint[j].t28, sizeof(out.joint[j].t28)) != 0 || memcmp(&ji._50, out.joint[j].t50, sizeof(out.joint[j].t50)) != 0)) {
            bad |= 16;
            break;
        }
    }
    if (memcmp(J3DSys::mCurrentMtx, out.curMtx, sizeof(Mtx)) != 0 || memcmp(&J3DSys::mCurrentS, &out.curS, sizeof(Vec)) != 0 ||
        memcmp(&J3DSys::mParentS, &out.parentS, sizeof(Vec)) != 0 || J3DMtxCalc::getMtxBuffer() != buf || J3DMtxCalc::getJoint() != out.lastJoint ||
        J3DJoint::mCurrentMtxCalc != basic || j3dSys.mCurrentMtxCalc != core) {
        bad |= 32;
    }
    return bad;
}

// (GXV_PAR_ANIM_BLEND) the joint state as it is in mJointList now, into a JointPrev (the job's Input::prev), and the comparison
// ModelManager.cpp makes at the model's real computation (true: the bytes the walk was given are the core's).
inline void TakePrev(const XanimeCore* core, u16 jointNum, JointPrev* prev) {
    for (u16 j = 0; j < jointNum; j++) {
        const XjointInfo& ji = core->mJointList[j];
        memcpy(prev[j].t0, &ji._0, sizeof(prev[j].t0));
        memcpy(prev[j].t28, &ji._28, sizeof(prev[j].t28));
        memcpy(prev[j].t50, &ji._50, sizeof(prev[j].t50));
    }
}
inline bool SamePrev(const XanimeCore* core, u16 jointNum, const JointPrev* prev) {
    for (u16 j = 0; j < jointNum; j++) {
        const XjointInfo& ji = core->mJointList[j];
        if (memcmp(prev[j].t0, &ji._0, sizeof(prev[j].t0)) != 0 || memcmp(prev[j].t28, &ji._28, sizeof(prev[j].t28)) != 0 ||
            memcmp(prev[j].t50, &ji._50, sizeof(prev[j].t50)) != 0) {
            return false;
        }
    }
    return true;
}

static_assert(sizeof(XtransformInfo) == sizeof(f32) * 10, "XjointInfo::_28 layout");
static_assert(sizeof(JointPrev) == sizeof(XtransformInfo) * 2 + sizeof(TVec3f), "XjointInfo::_0, _28, _50 layout");

}  // namespace xspec
