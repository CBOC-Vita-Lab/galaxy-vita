#include "Game/Animation/XanimeCore.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/MtxUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp>
#include <JSystem/JMath/JMath.hpp>

TVec3f (TVec3f::* XanimeCore_FORCE_EMIT())(const TVec3f&) const {
    (void)1.0f;
    (void)0.0f;
    TVec3f* pVectors = new TVec3f[1];
    delete[] pVectors;
    return &TVec3f::operator-;
}

void XanimeTrack::init() {
    _0 = nullptr;
    mWeight = 0.0f;
    _C = 0;
    _8 = 0.0f;
}

XjointInfo::XjointInfo() {
    _0._0.set< f32 >(1.0f, 1.0f, 1.0f);
    _0._C.x = 0.0f;
    _0._C.y = 0.0f;
    _0._C.z = 0.0f;
    _0.mRotation.z = 0.0f;
    _0.mRotation.y = 0.0f;
    _0.mRotation.x = 0.0f;
    _0.mRotation.w = 1.0f;
    _28 = _0;
    _5C = 1.0f;
    _60 = 1.0f;
    _50.zero();
}

XjointTransform::XjointTransform() {
    _0 = nullptr;
    _4 = 0xFFFF;
    mScale.set(1.0f);
    _14.set(1.0f);
    _64 = nullptr;
    _6C = nullptr;
    _68 = 0;
    _20.x = 0.0f;
    _20.y = 0.0f;
    _20.z = 0.0f;
    _2C.x = 0.0f;
    _2C.y = 0.0f;
    _2C.z = 0.0f;
    _38.x = 0.0f;
    _38.y = 0.0f;
    _38.z = 0.0f;
    _44.mScale.x = 0.0f;
    _44.mScale.y = 0.0f;
    _44.mScale.z = 0.0f;
    _44.mRotation.x = 0;
    _44.mRotation.y = 0;
    _44.mRotation.z = 0;
    _44._12 = 0;
    _44.mTranslate.x = 0.0f;
    _44.mTranslate.y = 0.0f;
    _44.mTranslate.z = 0.0f;
}

void XanimeCore::shareJointTransform(const XanimeCore* pOther) {
    mTransformList = pOther->mTransformList;
}

void XanimeCore::enableJointTransform(J3DModelData* pModelData) {
    mTransformList = new XjointTransform[mJointCount];

    for (u32 i = 0; i < mJointCount; i++) {
        J3DJoint* pJoint = pModelData->getJointNodePointer(i);
        mTransformList[i]._0 = pJoint;
        mTransformList[i]._44 = pJoint->getTransformInfo();

        if (mTransformList[i]._4 == 0xFFFF) {
            for (u32 j = 0; j < mJointCount; j++) {
                if (mTransformList[j]._0 != nullptr) {
                    if (mTransformList[j]._0->getChild() == pJoint) {
                        mTransformList[i]._4 = j;
                        break;
                    }

                    if (mTransformList[j]._0->getYounger() == pJoint && mTransformList[j]._4 != 0xFFFF) {
                        mTransformList[i]._4 = mTransformList[j]._4;
                        break;
                    }
                }
            }
        }
    }
}

void XanimeCore::reconfigJointTransform(J3DModelData* pModelData) {
    for (u32 i = 0; i < mJointCount; i++) {
        J3DJoint* pJoint = pModelData->getJointNodePointer(i);
        mTransformList[i]._0 = pJoint;
        mTransformList[i]._44 = pJoint->getTransformInfo();
    }
}

void XanimeCore::initMember(u32 trackCount) {
    _1C = 1.0f;
    _20 = 1.0f;
    mFrameRatio = 0.0f;
    _28 = 0;
    _29 = 0;
    _6 = 0;
    _C = 0;
#ifdef TARGET_PC
    mPortOwed = 0;
    mPortOwedTracks = 0;
    mPortShared = 0;
    mPortCatchUps = 0;
    mPortOwedRatio = mPortOwed1C = mPortOwed20 = 0.0f;
#endif
    mTrackList = new XanimeTrack[trackCount];

    s32 curTrack = 0;

    while (curTrack < trackCount) {
        mTrackList[curTrack].init();
        curTrack++;
    }

    setWeight(0, 1.0f);
}

XanimeCore::XanimeCore(u32 trackCount, u32 jointCount, u8 a3) {
    _4 = a3;
    mTrackCount = trackCount;
    mJointCount = jointCount;
    mJointList = new XjointInfo[jointCount];
    mTransformList = nullptr;
    initMember(trackCount);
}

XanimeCore::XanimeCore(u32 trackCount, XanimeCore* pOtherCore) {
    mTrackCount = trackCount;
    mJointCount = pOtherCore->mJointCount;
    _4 = pOtherCore->_4;
    mJointList = pOtherCore->mJointList;
    mTransformList = pOtherCore->mTransformList;
    initMember(trackCount);
#ifdef TARGET_PC
    mPortShared = 1;  // (blendfix31: two cores write one joint list, in parts (MarioAnimator): a left-out computation's joint state is not made for them)
    pOtherCore->mPortShared = 1;
#endif
}

void XanimeCore::doFreeze() {
#ifdef TARGET_PC
    if (__builtin_expect(mPortOwed != 0, 0)) {
        portCatchUp(kPortCatchChange);  // (blendfix31: the blend starts from the pose the left-out computation would have made)
    }
#endif
    _28 = 1;
    _1C = 0.0f;
}

void XanimeCore::setBck(u32 index, J3DAnmTransform* pAnm) {
    mTrackList[index]._0 = pAnm;
    mTrackList[index].setFrame(0.0f);
    mTrackList[index]._C = 0;
}

void XanimeCore::setWeight(u8 idx, f32 weight) {
    mTrackList[idx].mWeight = weight;
}

void XanimeCore::calcBlend(TVec3f* pScale, TVec3f* pTranslate) {
    u16 joint = getJoint()->getJntNo();
    MtxPtr pMtx = getMtxBuffer()->getAnmMtx(joint);
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

    for (s32 i = 0; i < mTrackCount; i++) {
        if (mTrackList[i]._0 != nullptr) {
            totalWeight += mTrackList[i].mWeight;
        }
    }

    if (totalWeight == 0.0f) {
        *pScale = mJointList[joint]._28._0;
        *pTranslate = mJointList[joint]._28._C;
        PSMTXQuat(pMtx, &mJointList[joint]._28.mRotation);
        return;
    }

    inverseWeight = 1.0f / totalWeight;

    for (s32 i = 0; i < mTrackCount; i++) {
        XanimeTrack* pTrack = &mTrackList[i];

        if (pTrack->_0 != nullptr && pTrack->mWeight != 0.0f) {
            pTrack->_0->getTransform(joint, &transform);
            Quaternion trackRotation;
            JMAEulerToQuat(transform.mRotation.x, transform.mRotation.y, transform.mRotation.z, &trackRotation);

            weight = inverseWeight * pTrack->mWeight;
            *pScale += reinterpret_cast< const TVec3f& >(transform.mScale) * weight;
            *pTranslate += reinterpret_cast< const TVec3f& >(transform.mTranslate) * weight;
            accumulatedWeight += weight;
            JMAQuatLerp(&rotation, &trackRotation, weight / accumulatedWeight, &rotation);
        }
    }

    if (_29 != 0) {
        mJointList[joint]._0 = mJointList[joint]._28;
    }

    mJointList[joint]._50 = *pTranslate;
    f32 blend = _1C;

    if (blend < 1.0f) {
        const TVec3f* pFrozenScale = &mJointList[joint]._0._0;
        const TVec3f* pFrozenTranslate = &mJointList[joint]._0._C;
        XjointInfo& rJoint = mJointList[joint];
        *pScale = *pFrozenScale * (1.0f - blend) + *pScale * blend;
        *pTranslate = *pFrozenTranslate * (1.0f - blend) + *pTranslate * blend;
        JMAQuatLerp(&rJoint._0.mRotation, &rotation, blend, &rotation);
    }

    if (_20 != 1.0f) {
        TVec3f* pPreviousScale = &mJointList[joint]._28._0;
        TVec3f* pPreviousTranslate = &mJointList[joint]._28._C;
        XjointInfo& rJoint = mJointList[joint];
        *pScale = *pPreviousScale * (1.0f - _20) + *pScale * _20;
        *pTranslate = *pPreviousTranslate * (1.0f - _20) + *pTranslate * _20;
        JMAQuatLerp(&rJoint._28.mRotation, &rotation, _20, &rotation);
    }

    mJointList[joint]._28._0 = *pScale;
    mJointList[joint]._28._C = *pTranslate;
    mJointList[joint]._28.mRotation = rotation;
    PSMTXQuat(pMtx, &rotation);
}

void XanimeCore::calcSingle(TVec3f* pScale, TVec3f* pTranslate) {
    u16 joint = getJoint()->getJntNo();
    MtxPtr pMtx = getMtxBuffer()->getAnmMtx(joint);
    J3DTransformInfo transform;
    Quaternion rotation;

    if (mTrackList[0]._0 == nullptr) {
        *pScale = mJointList[joint]._28._0;
        *pTranslate = mJointList[joint]._28._C;
        PSMTXQuat(pMtx, &mJointList[joint]._28.mRotation);
        return;
    }

    mTrackList[0]._0->getTransform(joint, &transform);
    JMAEulerToQuat(transform.mRotation.x, transform.mRotation.y, transform.mRotation.z, &rotation);
    *pScale = reinterpret_cast< const TVec3f& >(transform.mScale);
    *pTranslate = reinterpret_cast< const TVec3f& >(transform.mTranslate);

    if (_29 != 0) {
        mJointList[joint]._0 = mJointList[joint]._28;
    }

    mJointList[joint]._50 = *pTranslate;
    f32 blend = _1C;

    if (blend < 1.0f) {
        XjointInfo* pJoints = mJointList;
        MR::vecBlend(pJoints[joint]._0._0, *pScale, pScale, blend);
        MR::vecBlend(pJoints[joint]._0._C, *pTranslate, pTranslate, blend);
        JMAQuatLerp(&pJoints[joint]._0.mRotation, &rotation, blend, &rotation);
    }

    mJointList[joint]._28._0 = *pScale;
    mJointList[joint]._28._C = *pTranslate;
    mJointList[joint]._28.mRotation = rotation;
    PSMTXQuat(pMtx, &rotation);
}

void XanimeCore::calcBlendSpecial() {
    u16 joint = getJoint()->getJntNo();
    J3DTransformInfo transform;
    Quaternion rotation;
    TVec3f scale(0.0f, 0.0f, 0.0f);
    TVec3f translate(0.0f, 0.0f, 0.0f);
    f32 inverseWeight;
    f32 weight;
    f32 accumulatedWeight = 0.0f;
    rotation.z = 0.0f;
    rotation.y = 0.0f;
    rotation.x = 0.0f;
    rotation.w = 1.0f;

    f32 totalWeight = 0.0f;

    for (s32 i = 0; i < mTrackCount; i++) {
        if (mTrackList[i]._0 != nullptr) {
            totalWeight += mTrackList[i].mWeight;
        }
    }

    if (totalWeight == 0.0f) {
        return;
    }

    inverseWeight = 1.0f / totalWeight;

    for (s32 i = 0; i < mTrackCount; i++) {
        XanimeTrack* pTrack = &mTrackList[i];

        if (pTrack->_0 != nullptr && pTrack->mWeight != 0.0f) {
            pTrack->_0->getTransform(joint, &transform);
            Quaternion trackRotation;
            JMAEulerToQuat(transform.mRotation.x, transform.mRotation.y, transform.mRotation.z, &trackRotation);

            weight = inverseWeight * pTrack->mWeight;
            scale += reinterpret_cast< const TVec3f& >(transform.mScale) * weight;
            translate += reinterpret_cast< const TVec3f& >(transform.mTranslate) * weight;
            accumulatedWeight += weight;
            JMAQuatLerp(&rotation, &trackRotation, weight / accumulatedWeight, &rotation);
        }
    }

    if (_29 != 0) {
        mJointList[joint]._0 = mJointList[joint]._28;
    }

    mJointList[joint]._50 = translate;
    f32 blend = _1C;

    if (mJointList[joint]._5C != 1.0f) {
        mJointList[joint]._5C += mJointList[joint]._60;
        mJointList[joint]._5C = MR::clamp(mJointList[joint]._5C, 0.0f, 1.0f);
        blend = mJointList[joint]._5C;
    }

    if (blend < 1.0f) {
        const TVec3f* pFrozenScale = &mJointList[joint]._0._0;
        const TVec3f* pFrozenTranslate = &mJointList[joint]._0._C;
        XjointInfo& rJoint = mJointList[joint];
        scale = *pFrozenScale * (1.0f - blend) + scale * blend;
        translate = *pFrozenTranslate * (1.0f - blend) + translate * blend;
        JMAQuatLerp(&rJoint._0.mRotation, &rotation, blend, &rotation);
    }

    if (_20 != 1.0f) {
        TVec3f* pPreviousScale = &mJointList[joint]._28._0;
        TVec3f* pPreviousTranslate = &mJointList[joint]._28._C;
        XjointInfo& rJoint = mJointList[joint];
        scale = *pPreviousScale * (1.0f - _20) + scale * _20;
        translate = *pPreviousTranslate * (1.0f - _20) + translate * _20;
        JMAQuatLerp(&rJoint._28.mRotation, &rotation, _20, &rotation);
    }

    mJointList[joint]._28._0 = scale;
    mJointList[joint]._28._C = translate;
    mJointList[joint]._28.mRotation = rotation;
}

void XanimeCore::updateFrame() {
#ifdef TARGET_PC
    if (__builtin_expect(mPortOwed != 0, 0)) {
        // (blendfix31: a computation starts while a left-out one's joint state is owed. It overwrites all of it unless it reads the last pose; the
        // freeze copy reads the same bytes either way: a change after the left-out computation made them already, at doFreeze)
        if (mPortOwed == 2) {
            mPortOwed = 0;  // (the drawn turn's flush makes the left-out computation itself, from the same state: LiveActor.cpp port_anim_flush_stale)
            ++gPortBlendCensus.paid;
        } else if (portReadsLast() && gPortBlendFix != 2) {
            portCatchUp(kPortCatchRead);
        } else {
            mPortOwed = 0;
            ++gPortBlendCensus.cleared;
        }
    }
#endif
    for (u32 i = 0; i < mTrackCount; i++) {
        if (mTrackList[i]._0 != nullptr) {
            if (mTrackList[i]._C != 0) {
                f32 frame = mTrackList[i]._8;
                mTrackList[i]._0->setFrame(frame * mTrackList[i]._0->getFrameMax());
            } else {
                mTrackList[i]._0->setFrame(mFrameRatio * mTrackList[i]._0->getFrameMax());
            }
        }
    }

    if (_28 != 0) {
        _28 = 0;
        _29 = 1;
    } else {
        _29 = 0;
    }
}

void XanimeCore::calcScaleBlendMaya(const TVec3f& rScale, const TVec3f& rTranslate) {
    J3DJoint* pJoint = getJoint();
    u16 joint = pJoint->getJntNo();
    J3DMtxBuffer* pMtxBuffer = getMtxBuffer();
    MtxPtr pMtx = pMtxBuffer->getAnmMtx(joint);

    if (mTransformList != nullptr) {
        if (mTransformList[joint]._64 != nullptr) {
            PSMTXConcat(pMtx, mTransformList[joint]._64, pMtx);
        }

        if (mTransformList[joint]._68 != 0) {
            PSMTXConcat(pMtx, reinterpret_cast< MtxPtr >(mTransformList[joint]._68), pMtx);
        }
    }

    if (mTransformList != nullptr && mTransformList[joint]._4 != 0xFFFF && mTransformList[mTransformList[joint]._4]._68 != 0) {
        Mtx inverse;
        PSMTXInverse(reinterpret_cast< MtxPtr >(mTransformList[mTransformList[joint]._4]._68), inverse);
        PSMTXConcat(inverse, pMtx, pMtx);
    }

    pMtx[0][3] = rTranslate.x;
    pMtx[1][3] = rTranslate.y;
    pMtx[2][3] = rTranslate.z;

    if (mTransformList != nullptr) {
        pMtx[0][3] += mTransformList[joint]._2C.x;
        pMtx[1][3] += mTransformList[joint]._2C.y;
        pMtx[2][3] += mTransformList[joint]._2C.z;
    }

    TVec3f scale(rScale);

    if (mTransformList != nullptr) {
        scale.x *= mTransformList[joint].mScale.x * mTransformList[joint]._14.x;
        scale.y *= mTransformList[joint].mScale.y * mTransformList[joint]._14.y;
        scale.z *= mTransformList[joint].mScale.z * mTransformList[joint]._14.z;
    }

    if (scale.x == 1.0f && scale.y == 1.0f && scale.z == 1.0f) {
        pMtxBuffer->setScaleFlag(joint, 1);
    } else {
        pMtxBuffer->setScaleFlag(joint, 0);
        JMAMTXApplyScale(pMtx, pMtx, scale.x, scale.y, scale.z);
    }

    if (mTransformList != nullptr && mTransformList[joint]._4 != 0xFFFF) {
        if (mTransformList[mTransformList[joint]._4].mScale.x != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(mTransformList[mTransformList[joint]._4].mScale.x);
            pMtx[0][0] *= inverseScale;
            pMtx[0][1] *= inverseScale;
            pMtx[0][2] *= inverseScale;
        }

        if (mTransformList[mTransformList[joint]._4].mScale.y != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(mTransformList[mTransformList[joint]._4].mScale.y);
            pMtx[1][0] *= inverseScale;
            pMtx[1][1] *= inverseScale;
            pMtx[1][2] *= inverseScale;
        }

        if (mTransformList[mTransformList[joint]._4].mScale.z != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(mTransformList[mTransformList[joint]._4].mScale.z);
            pMtx[2][0] *= inverseScale;
            pMtx[2][1] *= inverseScale;
            pMtx[2][2] *= inverseScale;
        }
    }

    if (pJoint->getScaleCompensate() == 1) {
        if (J3DSys::mParentS.x != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(J3DSys::mParentS.x);
            pMtx[0][0] *= inverseScale;
            pMtx[0][1] *= inverseScale;
            pMtx[0][2] *= inverseScale;
        }

        if (J3DSys::mParentS.y != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(J3DSys::mParentS.y);
            pMtx[1][0] *= inverseScale;
            pMtx[1][1] *= inverseScale;
            pMtx[1][2] *= inverseScale;
        }

        if (J3DSys::mParentS.z != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(J3DSys::mParentS.z);
            pMtx[2][0] *= inverseScale;
            pMtx[2][1] *= inverseScale;
            pMtx[2][2] *= inverseScale;
        }
    }

    PSMTXConcat(J3DSys::mCurrentMtx, pMtx, J3DSys::mCurrentMtx);

    if (mTransformList != nullptr && mTransformList[joint]._6C != nullptr) {
        TVec3f translate;
        MR::extractMtxTrans(J3DSys::mCurrentMtx, &translate);
        MR::setMtxTrans(J3DSys::mCurrentMtx, 0.0f, 0.0f, 0.0f);
        PSMTXConcat(mTransformList[joint]._6C, J3DSys::mCurrentMtx, J3DSys::mCurrentMtx);
        MR::setMtxTrans(J3DSys::mCurrentMtx, translate.x, translate.y, translate.z);
    }

    if (mTransformList != nullptr) {
        J3DSys::mCurrentMtx[0][3] += mTransformList[joint]._38.x;
        J3DSys::mCurrentMtx[1][3] += mTransformList[joint]._38.y;
        J3DSys::mCurrentMtx[2][3] += mTransformList[joint]._38.z;
    }

    PSMTXCopy(J3DSys::mCurrentMtx, pMtx);

    if (mTransformList != nullptr) {
        pMtx[0][3] += mTransformList[joint]._20.x;
        pMtx[1][3] += mTransformList[joint]._20.y;
        pMtx[2][3] += mTransformList[joint]._20.z;
    }

    J3DSys::mParentS.x = rScale.x;
    J3DSys::mParentS.y = rScale.y;
    J3DSys::mParentS.z = rScale.z;
}

void XanimeCore::calcScaleBlendMayaNoTransform(const TVec3f& rScale, const TVec3f& rTranslate) {
    J3DJoint* pJoint = getJoint();
    u16 joint = pJoint->getJntNo();
    J3DMtxBuffer* pMtxBuffer = getMtxBuffer();
    MtxPtr pMtx = pMtxBuffer->getAnmMtx(joint);

    pMtx[0][3] = rTranslate.x;
    pMtx[1][3] = rTranslate.y;
    pMtx[2][3] = rTranslate.z;

    TVec3f scale(rScale);

    if (scale.x == 1.0f && scale.y == 1.0f && scale.z == 1.0f) {
        pMtxBuffer->setScaleFlag(joint, 1);
    } else {
        pMtxBuffer->setScaleFlag(joint, 0);
        JMAMTXApplyScale(pMtx, pMtx, scale.x, scale.y, scale.z);
    }

    if (pJoint->getScaleCompensate() == 1) {
        if (J3DSys::mParentS.x != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(J3DSys::mParentS.x);
            pMtx[0][0] *= inverseScale;
            pMtx[0][1] *= inverseScale;
            pMtx[0][2] *= inverseScale;
        }

        if (J3DSys::mParentS.y != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(J3DSys::mParentS.y);
            pMtx[1][0] *= inverseScale;
            pMtx[1][1] *= inverseScale;
            pMtx[1][2] *= inverseScale;
        }

        if (J3DSys::mParentS.z != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(J3DSys::mParentS.z);
            pMtx[2][0] *= inverseScale;
            pMtx[2][1] *= inverseScale;
            pMtx[2][2] *= inverseScale;
        }
    }

    PSMTXConcat(J3DSys::mCurrentMtx, pMtx, J3DSys::mCurrentMtx);
    PSMTXCopy(J3DSys::mCurrentMtx, pMtx);

    J3DSys::mParentS.x = rScale.x;
    J3DSys::mParentS.y = rScale.y;
    J3DSys::mParentS.z = rScale.z;
}

void XanimeCore::calcScaleBlendSI(const TVec3f& rScale, const TVec3f& rTranslate) {
    Vec& rCurrentScale = J3DSys::mCurrentS;
    J3DJoint* pJoint = getJoint();
    u16 joint = pJoint->getJntNo();
    J3DMtxBuffer* pMtxBuffer = getMtxBuffer();
    MtxPtr pMtx = pMtxBuffer->getAnmMtx(joint);

    if (mTransformList != nullptr) {
        if (mTransformList[joint]._64 != nullptr) {
            PSMTXConcat(pMtx, mTransformList[joint]._64, pMtx);
        }

        if (mTransformList[joint]._68 != 0) {
            PSMTXConcat(pMtx, reinterpret_cast< MtxPtr >(mTransformList[joint]._68), pMtx);
        }
    }

    TVec3f scale(rScale);

    if (mTransformList != nullptr) {
        scale.x *= mTransformList[joint].mScale.x * mTransformList[joint]._14.x;
        scale.y *= mTransformList[joint].mScale.y * mTransformList[joint]._14.y;
        scale.z *= mTransformList[joint].mScale.z * mTransformList[joint]._14.z;
    }

    if (scale.x == 1.0f && scale.y == 1.0f && scale.z == 1.0f) {
        pMtxBuffer->setScaleFlag(joint, 1);
    } else {
        pMtxBuffer->setScaleFlag(joint, 0);
        JMAMTXApplyScale(pMtx, pMtx, scale.x, scale.y, scale.z);
    }

    if (mTransformList != nullptr && mTransformList[joint]._4 != 0xFFFF) {
        if (mTransformList[mTransformList[joint]._4].mScale.x != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(mTransformList[mTransformList[joint]._4].mScale.x);
            pMtx[0][0] *= inverseScale;
            pMtx[0][1] *= inverseScale;
            pMtx[0][2] *= inverseScale;
        }

        if (mTransformList[mTransformList[joint]._4].mScale.y != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(mTransformList[mTransformList[joint]._4].mScale.y);
            pMtx[1][0] *= inverseScale;
            pMtx[1][1] *= inverseScale;
            pMtx[1][2] *= inverseScale;
        }

        if (mTransformList[mTransformList[joint]._4].mScale.z != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(mTransformList[mTransformList[joint]._4].mScale.z);
            pMtx[2][0] *= inverseScale;
            pMtx[2][1] *= inverseScale;
            pMtx[2][2] *= inverseScale;
        }
    }

    if (pJoint->getScaleCompensate() == 1) {
        if (J3DSys::mParentS.x != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(J3DSys::mParentS.x);
            pMtx[0][0] *= inverseScale;
            pMtx[0][1] *= inverseScale;
            pMtx[0][2] *= inverseScale;
        }

        if (J3DSys::mParentS.y != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(J3DSys::mParentS.y);
            pMtx[1][0] *= inverseScale;
            pMtx[1][1] *= inverseScale;
            pMtx[1][2] *= inverseScale;
        }

        if (J3DSys::mParentS.z != 1.0f) {
            f32 inverseScale = JMath::fastReciprocal(J3DSys::mParentS.z);
            pMtx[2][0] *= inverseScale;
            pMtx[2][1] *= inverseScale;
            pMtx[2][2] *= inverseScale;
        }
    }

    pMtx[0][3] = rTranslate.x * rCurrentScale.x;
    pMtx[1][3] = rTranslate.y * rCurrentScale.y;
    pMtx[2][3] = rTranslate.z * rCurrentScale.z;

    if (mTransformList != nullptr) {
        pMtx[0][3] += mTransformList[joint]._2C.x;
        pMtx[1][3] += mTransformList[joint]._2C.y;
        pMtx[2][3] += mTransformList[joint]._2C.z;
    }

    PSMTXConcat(J3DSys::mCurrentMtx, pMtx, J3DSys::mCurrentMtx);

    rCurrentScale.x *= rScale.x;
    rCurrentScale.y *= rScale.y;
    rCurrentScale.z *= rScale.z;

    if (rCurrentScale.x == 1.0f && rCurrentScale.y == 1.0f && rCurrentScale.z == 1.0f) {
        pMtxBuffer->setScaleFlag(joint, 1);
        PSMTXCopy(J3DSys::mCurrentMtx, pMtx);
    } else {
        pMtxBuffer->setScaleFlag(joint, 0);
        JMAMTXApplyScale(J3DSys::mCurrentMtx, pMtx, rCurrentScale.x, rCurrentScale.y, rCurrentScale.z);
        pMtx[0][3] = J3DSys::mCurrentMtx[0][3];
        pMtx[1][3] = J3DSys::mCurrentMtx[1][3];
        pMtx[2][3] = J3DSys::mCurrentMtx[2][3];
    }

    if (mTransformList != nullptr) {
        pMtx[0][3] += mTransformList[joint]._20.x;
        pMtx[1][3] += mTransformList[joint]._20.y;
        pMtx[2][3] += mTransformList[joint]._20.z;
    }
}

void XanimeCore::calcScaleBlendBasic(const TVec3f& rScale, const TVec3f& rTranslate) {
    J3DJoint* pJoint = getJoint();
    u16 joint = pJoint->getJntNo();
    J3DMtxBuffer* pMtxBuffer = getMtxBuffer();
    MtxPtr pMtx = pMtxBuffer->getAnmMtx(joint);

    TVec3f& rCurrentScale = reinterpret_cast< TVec3f& >(J3DSys::mCurrentS);
    pMtx[0][3] = rTranslate.x;
    pMtx[1][3] = rTranslate.y;
    pMtx[2][3] = rTranslate.z;

    rCurrentScale.x *= rScale.x;
    rCurrentScale.y *= rScale.y;
    rCurrentScale.z *= rScale.z;

    if (rCurrentScale.x == 1.0f && rCurrentScale.y == 1.0f && rCurrentScale.z == 1.0f) {
        pMtxBuffer->setScaleFlag(joint, 1);
    } else {
        pMtxBuffer->setScaleFlag(joint, 0);
        JMAMTXApplyScale(pMtx, pMtx, rScale.x, rScale.y, rScale.z);
    }

    PSMTXConcat(J3DSys::mCurrentMtx, pMtx, J3DSys::mCurrentMtx);
    PSMTXCopy(J3DSys::mCurrentMtx, pMtx);
}

void XanimeCore::calcScaleBlendSpecial() {
    u16 joint = getJoint()->getJntNo();
    MtxPtr pMtx = getMtxBuffer()->getAnmMtx(joint);
    TVec3f scale;
    TVec3f translate;
    scale = mJointList[joint]._28._0;
    translate = mJointList[joint]._28._C;
    PSMTXQuat(pMtx, &mJointList[joint]._28.mRotation);
    calcScaleBlendMaya(scale, translate);
}

void XanimeCore::freezeCopy(J3DModelData* pModelData, XanimeCore* pOther, u32 joint, u32 duration) {
#ifdef TARGET_PC
    if (__builtin_expect((mPortOwed | pOther->mPortOwed) != 0, 0)) {
        // (blendfix31: this core's last pose is read and the other's frozen pose written: both as their left-out computations left them)
        if (mPortOwed != 0) portCatchUp(kPortCatchCopy);
        if (pOther->mPortOwed != 0) pOther->portCatchUp(kPortCatchCopy);
    }
#endif
    J3DJoint* pChild = pModelData->getJointNodePointer(joint)->getChild();

    if (pChild != nullptr) {
        freezeCopy(pModelData, pOther, pChild->getJntNo(), duration);

        while (true) {
            pChild = pChild->getYounger();
            if (pChild == nullptr) {
                break;
            }

            freezeCopy(pModelData, pOther, pChild->getJntNo(), duration);
        }
    }

    pOther->mJointList[joint]._0 = mJointList[joint]._28;
    pOther->mJointList[joint]._5C = 0.0f;
    f32 rate = 1.0f;

    if (duration != 0) {
        rate = 1.0f / duration;
    }

    pOther->mJointList[joint]._60 = rate;
}

void XanimeCore::initT(J3DModelData* pModelData) {
    for (u32 i = 0; i < mJointCount; i++) {
        const J3DTransformInfo& rTransform = pModelData->getJointNodePointer(i)->getTransformInfo();
        Quaternion rotation;
        JMAEulerToQuat(rTransform.mRotation.x, rTransform.mRotation.y, rTransform.mRotation.z, &rotation);
        mJointList[i]._0._0 = reinterpret_cast< const TVec3f& >(rTransform.mScale);
        mJointList[i]._0._C = reinterpret_cast< const TVec3f& >(rTransform.mTranslate);
        mJointList[i]._0.mRotation = rotation;
        mJointList[i]._28._0 = mJointList[i]._0._0;
        mJointList[i]._28._C = mJointList[i]._0._C;
        mJointList[i]._28.mRotation = mJointList[i]._0.mRotation;
    }
}

void XanimeCore::fixT(TVec3f* pTranslate) {
    u16 joint = getJoint()->getJntNo();
    J3DModelData* pModelData = j3dSys.getModel()->getModelData();

    if (joint != 0 && joint != _C) {
        *pTranslate = reinterpret_cast< const TVec3f& >(pModelData->getJointNodePointer(joint)->getTransformInfo().mTranslate);
    }
}

XanimeCore::~XanimeCore() {
}

XtransformInfo::XtransformInfo() {
    _0.zero();
    _C.zero();
    mRotation.z = 0.0f;
    mRotation.y = 0.0f;
    mRotation.x = 0.0f;
    mRotation.w = 1.0f;
}

void XanimeCore::calc() {
    j3dSys.mCurrentMtxCalc = this;

    if (_6 == 1) {
        calcBlendSpecial();
        return;
    }

    if (_6 == 2) {
        calcScaleBlendSpecial();
        return;
    }

    TVec3f scale;
    TVec3f translate;

    if (mTrackCount == 1) {
        calcSingle(&scale, &translate);
    } else {
        calcBlend(&scale, &translate);
    }

    if (_6 == 3) {
        fixT(&translate);
    }

    switch (_4) {
    case 1:
        calcScaleBlendSI(scale, translate);
        break;
    case 0:
        if (mTransformList == nullptr) {
            calcScaleBlendBasic(scale, translate);
            break;
        }
    case 2:
        if (mTransformList != nullptr) {
            calcScaleBlendMaya(scale, translate);
        } else {
            calcScaleBlendMayaNoTransform(scale, translate);
        }
        break;
    }
}

void XanimeCore::init(const Vec& rScale, const Mtx& rMtx) {
    switch (_4) {
    case 0:
    case 1:
    case 2:
        J3DMtxCalcJ3DSysInitMaya::init(rScale, rMtx);
        break;
    }
}

#ifdef TARGET_PC
// blendfix31 (GXV_POSE_BLENDFIX, 17 Nov 2026; tests/blendfix.cpp, tools/test_blendfix.sh): the joint state of a computation a pose cut left out, made when
// something is about to read it (XanimeCore.hpp). Exact by construction: the joint state a computation leaves (_0 on a freeze copy, _28, _50, _5C) is a
// function of the core's state before it and of its inputs: the freeze flag (updateFrame), the frame ratio (each track's frame on its resource),
// _1C, _20, _6 and the tracks (resource, weight, _8 / _C). The inputs are recorded when the computation is left out; the catch-up runs the game's own
// updateFrame, calcSingle / calcBlend / calcBlendSpecial on them, joint by joint, with J3DMtxCalc pointed at a scratch buffer for the matrices they
// write (the model's own matrices stay as they are: the cut's picture is unchanged), and puts the live inputs back. A catch-up is needed only if
// something reads that state before the next computation overwrites it: the next computation when it reads the last pose (portReadsLast), a change
// of animation (the freeze copy), freezeCopy; and a second left-out computation that reads the last pose is caught up through the first.
u8 gPortBlendFix = 0;
u8 gPortBlendFixMeasure = 0;
XanimePortBlendCensus gPortBlendCensus;

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
// (scratch for the matrices the joint functions write and, when measured, the joint state before; the game thread only in practice, but a
// second thread gets memory of its own rather than racing for this)
std::atomic< int > sPortCatchBusy{0};
void* sPortCatchMem = nullptr;
size_t sPortCatchCap = 0;
J3DMtxBuffer* PortCatchBuffer() {
    static J3DMtxBuffer* buf = new (malloc(sizeof(J3DMtxBuffer))) J3DMtxBuffer();
    return buf;
}
J3DJoint* PortCatchJoint() {
    static J3DJoint* joint = new (malloc(sizeof(J3DJoint))) J3DJoint();
    return joint;
}
f32 PortQuatDeg(const Quaternion& a, const Quaternion& b) {
    f32 d = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d) * 57.29578f;
}
}  // namespace

bool XanimeCore::portReadsLast() const {
    // (the next computation reads the joint state the last one wrote, other than through the freeze copy: calcBlendSpecial (_5C, _20),
    // calcScaleBlendSpecial (_28 only), calcSingle without a resource, calcBlend's _20 != 1 or a total weight of 0)
    if (_6 == 1 || _6 == 2) {
        return true;
    }
    if (mTrackCount == 1) {
        return mTrackList[0]._0 == nullptr;
    }
    if (_20 != 1.0f) {
        return true;
    }
    f32 total = 0.0f;
    for (s32 i = 0; i < mTrackCount; i++) {
        if (mTrackList[i]._0 != nullptr) {
            total += mTrackList[i].mWeight;
        }
    }
    return total == 0.0f;
}

void XanimeCore::portLeaveOut(f32 frameRatio) {
    if (mPortShared != 0) {
        ++gPortBlendCensus.shared;
        return;
    }
    if (mPortOwed != 0 && portReadsLast()) {
        // (this one reads what the earlier left-out one writes: that one is made first, from its own inputs)
        if (gPortBlendFix == 2) {
            mPortOwed = 0;  // (measure only: as before)
        } else {
            portCatchUp(kPortCatchChain);
        }
    }
    ++gPortBlendCensus.leftOut;
    mPortOwedRatio = frameRatio;
    mPortOwed1C = _1C;
    mPortOwed20 = _20;
    if (mTrackCount > kPortOwedTracks) {
        // (more tracks than recorded: made now, from the inputs it has now; measure only: nothing)
        mPortOwedTracks = 0;
        mPortOwed = 1;
        portCatchUp(kPortCatchEager);
        return;
    }
    mPortOwedTracks = mTrackCount;
    for (u32 t = 0; t < mTrackCount; t++) {
        mPortOwedTrack[t] = mTrackList[t];
    }
    mPortOwed = 1;
}

bool XanimeCore::portRemake(void (*calc)(void*), void* ctx, J3DJoint* root) {
    // (only an intact record: owed and not made since (1; 2 is the flush's own computation), every track recorded, a joint list of its own)
    if (mPortOwed != 1 || mPortOwedTracks == 0 || mPortOwedTracks != mTrackCount || mPortShared != 0 || root == nullptr) {
        return false;
    }
    const u32 nt = mPortOwedTracks;
    XanimeTrack live[kPortOwedTracks];
    f32 anmFrame[kPortOwedTracks];
    const f32 live1C = _1C, live20 = _20;
    for (u32 t = 0; t < nt; t++) {
        live[t] = mTrackList[t];
        mTrackList[t] = mPortOwedTrack[t];
    }
    mFrameRatio = mPortOwedRatio;  // (and left so, as the left-out computation leaves it: portCatchUp does the same; the next computation sets its own)
    _1C = mPortOwed1C;
    _20 = mPortOwed20;
    for (u32 t = 0; t < nt; t++) {
        anmFrame[t] = mTrackList[t]._0 != nullptr ? mTrackList[t]._0->getFrame() : 0.0f;
    }
    mPortOwed = 0;
    updateFrame();  // (the game's own: the frames onto the shared resources, the freeze flag as the left-out computation would have taken it)
    J3DMtxCalc* const savedCalc = root->mMtxCalc;
    root->mMtxCalc = this;
    calc(ctx);
    root->mMtxCalc = savedCalc;
    for (u32 t = nt; t-- > 0;) {
        if (mTrackList[t]._0 != nullptr) {
            mTrackList[t]._0->setFrame(anmFrame[t]);
        }
    }
    for (u32 t = 0; t < nt; t++) {
        mTrackList[t] = live[t];
    }
    _1C = live1C;
    _20 = live20;
    ++mPortCatchUps;  // (the core changed outside a computation: GXV_ANIM_MEMO / GXV_PAR_ANIM's key word)
    return true;
}

void XanimeCore::portCatchUp(int why) {
    mPortOwed = 0;
    const bool keep = gPortBlendFix == 2;  // (measure only: made, compared and put back)
    if (keep && why != kPortCatchChange) {
        return;
    }
    const bool measure = keep || (gPortBlendFixMeasure != 0 && why == kPortCatchChange);
    const u32 n = mJointCount;
    const size_t need = n * sizeof(Mtx) + mTrackCount * sizeof(f32) + (measure ? n * sizeof(XjointInfo) : 0);
    void* mem = nullptr;
    bool own = false;
    int expect = 0;
    if (sPortCatchBusy.compare_exchange_strong(expect, 1, std::memory_order_acquire)) {
        if (sPortCatchCap < need) {
            void* p = realloc(sPortCatchMem, need);
            if (p != nullptr) {
                sPortCatchMem = p;
                sPortCatchCap = need;
            }
        }
        mem = sPortCatchCap >= need ? sPortCatchMem : nullptr;
        if (mem == nullptr) {
            sPortCatchBusy.store(0, std::memory_order_release);
        }
    } else {
        mem = malloc(need);
        own = true;
    }
    if (mem == nullptr) {
        ++gPortBlendCensus.noMemory;
        return;
    }
    switch (why) {
    case kPortCatchChange: ++gPortBlendCensus.atChange; break;
    case kPortCatchRead: ++gPortBlendCensus.atRead; break;
    case kPortCatchChain: ++gPortBlendCensus.chained; break;
    case kPortCatchEager: ++gPortBlendCensus.eager; break;
    default: ++gPortBlendCensus.atCopy; break;
    }
    Mtx* mtx = static_cast< Mtx* >(mem);
    f32* anmFrame = reinterpret_cast< f32* >(static_cast< u8* >(mem) + n * sizeof(Mtx));
    XjointInfo* before = measure ? reinterpret_cast< XjointInfo* >(static_cast< u8* >(mem) + n * sizeof(Mtx) + mTrackCount * sizeof(f32)) : nullptr;
    if (before != nullptr) {
        memcpy(static_cast< void* >(before), mJointList, n * sizeof(XjointInfo));
    }
    // the inputs: the left-out computation's in, the live ones kept (eager: they are the live ones)
    const bool recorded = mPortOwedTracks != 0;
    const u32 nt = recorded ? mPortOwedTracks : 0;
    XanimeTrack live[kPortOwedTracks];
    const f32 liveRatio = mFrameRatio, live1C = _1C, live20 = _20;
    const u8 live28 = _28, live29 = _29;
    for (u32 t = 0; t < nt; t++) {
        live[t] = mTrackList[t];
        mTrackList[t] = mPortOwedTrack[t];
    }
    mFrameRatio = mPortOwedRatio;
    _1C = mPortOwed1C;
    _20 = mPortOwed20;
    // (the frames updateFrame sets on the shared resources: put back afterwards, as the next computation finds them)
    const u32 nf = mTrackCount;
    for (u32 t = 0; t < nf; t++) {
        anmFrame[t] = mTrackList[t]._0 != nullptr ? mTrackList[t]._0->getFrame() : 0.0f;
    }
    updateFrame();  // (the game's own: the frames, and the freeze flag as the left-out computation would have taken it; mPortOwed is 0)
    J3DMtxBuffer* const savedBuf = J3DMtxCalc::getMtxBuffer();
    J3DJoint* const savedJoint = J3DMtxCalc::getJoint();
    J3DMtxBuffer* const buf = PortCatchBuffer();
    J3DJoint* const joint = PortCatchJoint();
    buf->mpAnmMtx = mtx;
    J3DMtxCalc::setMtxBuffer(buf);
    for (u32 j = 0; j < n; j++) {
        joint->mJntNo = static_cast< u16 >(j);
        J3DMtxCalc::setJoint(joint);
        if (_6 == 1) {
            calcBlendSpecial();
        } else if (_6 != 2) {  // (calcScaleBlendSpecial writes no joint state)
            TVec3f scale;
            TVec3f translate;
            if (mTrackCount == 1) {
                calcSingle(&scale, &translate);
            } else {
                calcBlend(&scale, &translate);
            }
        }
    }
    J3DMtxCalc::setMtxBuffer(savedBuf);
    J3DMtxCalc::setJoint(savedJoint);
    for (u32 t = nf; t-- > 0;) {
        if (mTrackList[t]._0 != nullptr) {
            mTrackList[t]._0->setFrame(anmFrame[t]);
        }
    }
    for (u32 t = 0; t < nt; t++) {
        mTrackList[t] = live[t];
    }
    _1C = live1C;
    _20 = live20;
    // (the frame ratio as the left-out computation left it; a computation about to start (kPortCatchRead) has set its own already)
    mFrameRatio = why == kPortCatchRead ? liveRatio : mPortOwedRatio;
    if (measure) {
        f32 deg = 0.0f, tr = 0.0f;
        for (u32 j = 0; j < n; j++) {
            const XtransformInfo& a = before[j]._28;
            const XtransformInfo& b = mJointList[j]._28;
            const f32 d = PortQuatDeg(a.mRotation, b.mRotation);
            const f32 dx = a._C.x - b._C.x, dy = a._C.y - b._C.y, dz = a._C.z - b._C.z;
            const f32 t = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > deg) deg = d;
            if (t > tr) tr = t;
        }
        bool differs = false;
        for (u32 j = 0; j < n && !differs; j++) {
            differs = memcmp(&before[j]._28, &mJointList[j]._28, sizeof(XtransformInfo)) != 0;  // (the pose the freeze copies)
        }
        if (differs && why == kPortCatchChange) {
            ++gPortBlendCensus.changed;
            gPortBlendCensus.sumDeg += deg;
            if (deg > gPortBlendCensus.maxDeg) gPortBlendCensus.maxDeg = deg;
            if (tr > gPortBlendCensus.maxTrans) gPortBlendCensus.maxTrans = tr;
        }
    }
    if (keep) {
        memcpy(mJointList, static_cast< const void* >(before), n * sizeof(XjointInfo));
        _28 = live28;
        _29 = live29;
        mFrameRatio = liveRatio;
    } else {
        ++mPortCatchUps;
    }
    if (own) {
        free(mem);
    } else {
        sPortCatchBusy.store(0, std::memory_order_release);
    }
}
#endif
