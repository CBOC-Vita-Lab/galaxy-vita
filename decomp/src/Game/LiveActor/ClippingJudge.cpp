#include "Game/LiveActor/ClippingJudge.hpp"
#include "Game/LiveActor/ClippingDirector.hpp"
#include "Game/Util.hpp"
#ifdef TARGET_PC
#include "port/popin.h"  // (popin24, GXV_POPIN_CLIPPAD)
#include "port/lockstep.h"  // (fmt32)
extern "C" float port_hrw_clip_pad(void);  // (hrw27, GXV_HRW: vgx_galaxy.cpp; 1 = none)
extern "C" float port_popin_clip_pad(float fovyDeg);  // (popin25, GXV_POPIN_LEAD: LodCtrl.cpp; 1 = none)
#endif

void ClippingJudge_FORCE_MATCH_SDATA2() {
    0.0f;
}

ClippingJudge::ClippingJudge(const char* pName) : NameObj(pName), mFrustum() {
    for (s32 i = 0; i < 8; i++) {
        mClipDistances[i] = -1.0f;
    }

    mClipDistances[0] = 0.0f;
    mClipDistances[1] = 60000.0f;
    mClipDistances[2] = 50000.0f;
    mClipDistances[3] = 40000.0f;
    mClipDistances[4] = 30000.0f;
    mClipDistances[5] = 20000.0f;
    mClipDistances[6] = 10000.0f;
    mClipDistances[7] = 5000.0f;
}

void ClippingJudge::init(const JMapInfoIter& rIter) {
}

#if defined(__vita__) && defined(__arm__) && defined(GXV_SHARED_CLIP_PREP) && GXV_SHARED_CLIP_PREP
namespace {
// Preserve the original far-plane sequence; enablement requires target codegen verification.
__attribute__((noinline)) void calcFarPlaneLikeCalcViewingVolume(THex3f* pVolume, const TVec3f& front,
                                                                 const TVec3f& position, f32 farClip) {
    TVec3f back;
    back.negate(front);
    TVec3f planePosition;
    planePosition.scaleAdd(farClip, front, position);
    pVolume->mPlanes[5].set(back, planePosition);
}
}  // namespace
#endif

void ClippingJudge::movement() {
    calcViewingVolume(&mFrustum, MR::getFarZ());
#ifdef TARGET_PC
    // (zoomclip23) the picture's frustum, while the handheld zoom narrows the picture (see the header)
    {
        const f32 pictureFovy = MR::getFovy();
        mPortPictureZoomed = MR::getFovyForLogic() > pictureFovy;
        if (mPortPictureZoomed) {
            calcViewingVolumeFovy(&mPortPictureFrustum, MR::getFarZ(), pictureFovy);
        }
    }
#endif

#if defined(__vita__) && defined(__arm__) && defined(GXV_SHARED_CLIP_PREP) && GXV_SHARED_CLIP_PREP
    TPos3f cameraMtx;
    cameraMtx.setPositionFromLookAt(MR::getCameraViewMtx());
    TVec3f front;
    cameraMtx.getZDir(front);
    TVec3f position;
    cameraMtx.getTrans(position);
    for (u32 i = 1; i < 8; i++) {
        mClipFrustums[i].mPlanes[3] = mFrustum.mPlanes[3];
        mClipFrustums[i].mPlanes[1] = mFrustum.mPlanes[1];
        mClipFrustums[i].mPlanes[2] = mFrustum.mPlanes[2];
        mClipFrustums[i].mPlanes[0] = mFrustum.mPlanes[0];
        calcFarPlaneLikeCalcViewingVolume(&mClipFrustums[i], front, position, mClipDistances[i]);
        mClipFrustums[i].mPlanes[4] = mFrustum.mPlanes[4];
    }
#else
    for (u32 i = 1; i < 8; i++) {
        calcViewingVolume(&mClipFrustums[i], mClipDistances[i]);
    }
#endif
}

bool ClippingJudge::isJudgedToClipFrustum(const TVec3f& rVec, f32 a2) const {
    return !mFrustum.mayIntersectBall3(rVec, a2);
}

bool ClippingJudge::isJudgedToClipFrustum(const TVec3f& rVec, f32 a2, s32 index) const {
    return (!index) ? !mFrustum.mayIntersectBall3(rVec, a2) : !mClipFrustums[index].mayIntersectBall3(rVec, a2);
}

#ifdef TARGET_PC
// (zoomclip23) Outside the picture's frustum (the zoomed view; the game's own while no zoom is in force). Only for decisions that change
// what is drawn, never the game's logic.
bool ClippingJudge::isJudgedToClipPictureFrustum(const TVec3f& rVec, f32 radius) const {
    return mPortPictureZoomed ? !mPortPictureFrustum.mayIntersectBall3(rVec, radius) : !mFrustum.mayIntersectBall3(rVec, radius);
}

// The game's viewing volume: GXV_ZOOM_LOGIC_VIEW (MR::getFovyForLogic, CameraUtil.cpp) gives the game's own field of view.
void ClippingJudge::calcViewingVolume(THex3f* pVolume, f32 farClip) {
    calcViewingVolumeFovy(pVolume, farClip, MR::getFovyForLogic());
}

void ClippingJudge::calcViewingVolumeFovy(THex3f* pVolume, f32 farClip, f32 fovy) {
#else
void ClippingJudge::calcViewingVolume(THex3f* pVolume, f32 farClip) {
#endif
    f32 nearClip = 500.0f;
    if (MR::isSubjectiveCameraOnForObjClipping()) {
        nearClip = 100.0f;
    }

    f32 aspect = MR::getAspect();
#ifndef TARGET_PC
    f32 fovy = MR::getFovy();
#endif
    TPos3f cameraMtx;
    cameraMtx.setPositionFromLookAt(MR::getCameraViewMtx());
    f32 height = nearClip * static_cast< f32 >(tan(fovy * (JGeometry::TUtil< f32 >::PI() / 360.0f)));
#ifdef TARGET_PC
    // (fmt32: this is the frustum the game's own clipping judges with, and a clipped actor stops moving: in a lockstep session it is the game's, without the three pads, whose
    // size depends on this console's switches, on how fast its camera turns and on whether it is making picture pairs)
    if (!port::lockstep::On()) {
        height *= port::popin::ClipPadFactor();  // (popin24, GXV_POPIN_CLIPPAD: the side planes moved out; 1 = as the game)
        if (port::popin::LeadTurns() > 0.0f) {
            height *= port_popin_clip_pad(fovy);  // (popin25, GXV_POPIN_LEAD: a turning camera's sides, 1 when it is not turning)
        }
        height *= port_hrw_clip_pad();  // (hrw27, GXV_HRW: the second picture of a pair turns up to the guard band away from the first: what it will see is drawn in the first)
    }
#endif
    f32 width = aspect * height;
#ifdef TARGET_PC
    {
        // In VR, cull against what the headset actually sees.
        float vrMtx[12], tanX, tanY;
        if (port_vr_cull_view(vrMtx, &tanX, &tanY)) {
            nearClip = 20.0f;
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 4; c++) {
                    cameraMtx.mMtx[r][c] = vrMtx[r * 4 + c];
                }
            }
            width = nearClip * tanX;
            height = nearClip * tanY;
        }
    }
#endif

    TVec3f front;
    cameraMtx.getZDir(front);
    TVec3f position;
    TVec3f bottomRight(width, -height, nearClip);
    TVec3f topRight(width, height, nearClip);
    TVec3f topLeft(-width, height, nearClip);
    TVec3f bottomLeft(-width, -height, nearClip);
    cameraMtx.getTrans(position);
    cameraMtx.mult(topRight, topRight);
    cameraMtx.mult(topLeft, topLeft);
    cameraMtx.mult(bottomLeft, bottomLeft);
    cameraMtx.mult(bottomRight, bottomRight);

    pVolume->mPlanes[3].set(position, topLeft, topRight);
    pVolume->mPlanes[1].set(position, topRight, bottomRight);
    pVolume->mPlanes[2].set(position, bottomRight, bottomLeft);
    pVolume->mPlanes[0].set(position, bottomLeft, topLeft);

    TVec3f back;
    back.negate(front);
    TVec3f planePosition;
    planePosition.scaleAdd(farClip, front, position);
    pVolume->mPlanes[5].set(back, planePosition);
    planePosition.scaleAdd(nearClip, front, position);
    pVolume->mPlanes[4].set(front, planePosition);
}

namespace MR {
    ClippingJudge* getClippingJudge() {
        return getClippingDirector()->mJudge;
    }
}  // namespace MR

ClippingJudge::~ClippingJudge() {
}
