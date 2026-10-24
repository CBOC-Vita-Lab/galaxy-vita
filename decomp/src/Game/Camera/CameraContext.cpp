#include "Game/Camera/CameraContext.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SystemUtil.hpp"

#ifdef TARGET_PC
#include "Game/Demo/DemoDirector.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include <math.h>
#include <stdlib.h>
extern "C" float port_camera_zoom(void);
extern "C" void port_log(const char* fmt, ...);  // (platform/src/port/perf.cpp) PETARI_ZOOM: handhelds look a little closer

// The zoom is for playing the game.  The logo, title and thank-you scenes and the file-select stage are composed against the whole
// frame (the title's 3D logo and its letterbox border), and a cutscene (a demo, or a camera the stage scripts: an "event camera")
// is framed by the game for the whole picture, so they all keep the original field of view.
static bool inGameScene() {
    GameSystem* pSystem = SingletonHolder< GameSystem >::get();
    if (pSystem == nullptr || pSystem->mSceneController == nullptr) {
        return false;
    }
    return MR::isEqualSceneName("Game") && !MR::isStageFileSelect();
}

static bool inCutscene() {
    if (MR::isExistSceneObj(SceneObj_DemoDirector) && MR::isDemoActive()) {
        return true;
    }
    return MR::isEventCameraActive();
}

// The zoom in force: Z in play, 1.0 in a cutscene, and in between for the 24 camera updates (0.4 s) after either begins, so the
// picture does not jump.  Not in the game scene: 1.0 at once.  `init`: the camera is being created (no camera director yet to ask).
static f32 sZoomNow = 1.0f;
static bool sZoomKnown = false;

// The field of view the projection (and everything that reads it back) uses: tan(fovy/2) divided by the zoom.
static f32 zoomedFovy(f32 fovy, bool init) {
    const f32 zoom = port_camera_zoom();
    if (!(fovy > 0.0f && fovy < 170.0f)) {
        return fovy;
    }
    if (!(zoom > 1.0f) && !(sZoomNow > 1.0f)) {
        // no zoom now and none asked for
        sZoomNow = 1.0f;
        sZoomKnown = true;
        return fovy;
    }
    f32 target = zoom;
    bool jump = init || !sZoomKnown;
    if (!inGameScene()) {
        target = 1.0f;
        jump = true;
    } else if (!init && inCutscene()) {
        target = 1.0f;
    }
    if (jump) {
        sZoomNow = target;
    } else {
        // (the whole way, 1.0 to the larger of the two zooms, takes 24 updates; the setting can change in the pause menu)
        f32 span = zoom > sZoomNow ? zoom : sZoomNow;
        span -= 1.0f;
        const f32 step = span > 0.02f ? span / 24.0f : 0.02f / 24.0f;
        if (sZoomNow < target) {
            sZoomNow = sZoomNow + step > target ? target : sZoomNow + step;
        } else if (sZoomNow > target) {
            sZoomNow = sZoomNow - step < target ? target : sZoomNow - step;
        }
    }
    sZoomKnown = true;
    static const bool logZoom = getenv("PETARI_ZOOM_LOG") != nullptr;
    static f32 sLogTarget = 0.0f;
    if (logZoom && sLogTarget != target) {
        sLogTarget = target;
        port_log("[zoom] target %.2f (%s), now %.2f", target, target > 1.0f ? "play" : "cutscene / non-game scene", sZoomNow);
    }
    if (sZoomNow > 1.0f) {
        return static_cast< f32 >(2.0 * atan(tan(fovy * (M_PI / 360.0)) / sZoomNow) * (180.0 / M_PI));
    }
    return fovy;
}
#else
static inline f32 zoomedFovy(f32 fovy, bool) {
    return fovy;
}
#endif

void CameraContext_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
    (void)-1.0f;
}

CameraContext::~CameraContext() {
}

CameraContext::CameraContext() : NameObj("\x83\x4a\x83\x81\x83\x89\x83\x52\x83\x93\x83\x65\x83\x4c\x83\x58\x83\x67") {
    initParams();
}

void CameraContext::initParams() {
    mView.setPositionFromLookAt(TVec3f(0.0f, 0.0f, 3000.0f), TVec3f(0.0f, 1.0f, 0.0f), TVec3f(0.0f, 0.0f, 0.0f));

    mViewInv.set(mView);
    mViewInv.invert(mViewInv);

    mFovy = zoomedFovy(45.0f, true);
    const f32 height = MR::getScreenHeight();
    const f32 width = MR::getScreenWidth();
    mAspectRatio = width / height;
    mNearZ = 100.0f;
    mFarZ = 800000.0f;

    mShakeOffset.zero();

    updateProjectionMtx();
}

const TPos3f& CameraContext::getViewMtx() const {
    return mView;
}

const TPos3f& CameraContext::getInvViewMtx() const {
    return mViewInv;
}

f32 CameraContext::getAspect() const {
    if (MR::isScreen16Per9()) {
        return 16.0f / 9.0f;
    } else {
        return static_cast< f32 >(MR::getScreenWidth()) / MR::getScreenHeight();
    }
}

void CameraContext::setViewMtx(const TPos3f& rMtx, bool b1, bool b2, const TVec3f& rVec) {
    mView.set(rMtx);
    mViewInv.invert(rMtx);
}

void CameraContext::setNearZ(f32 near) {
    mNearZ = near;
    updateProjectionMtx();
}

void CameraContext::setFarZ(f32 far) {
    mFarZ = far;
    updateProjectionMtx();
}

void CameraContext::setFovy(f32 fovy) {
    mFovy = zoomedFovy(fovy, false);
    updateProjectionMtx();
}

void CameraContext::setShakeOffset(f32 x, f32 y) {
    mShakeOffset.set(x, y);
    updateProjectionMtx();
}

void CameraContext::updateProjectionMtx() {
    mProjection.makePerspective(mFovy, getAspect(), mNearZ, mFarZ);

    TProj3f trans;
    trans.makeTrans(mShakeOffset.x, mShakeOffset.y);

    mProjection.concat(trans, mProjection);
}
