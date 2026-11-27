#include "Game/Camera/CameraContext.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SystemUtil.hpp"

#ifdef TARGET_PC
#include "port/reduced_motion.h"
#include "Game/Camera/CameraDirector.hpp"
#include "Game/Camera/CameraHolder.hpp"
#include "Game/Camera/CameraManGame.hpp"
#include "Game/Demo/DemoDirector.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include "port/hot_knob.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
extern "C" float port_camera_zoom(void);
extern "C" void port_log(const char* fmt, ...);  // (platform/src/port/perf.cpp) PETARI_ZOOM: handhelds look a little closer

static long EnvInt(const char* name, long fallback) {
    const char* value = getenv(name);
    return value && *value ? strtol(value, nullptr, 0) : fallback;
}

// The zoom is for playing the game.  The logo, title and thank-you scenes and the file-select stage are composed against the whole
// frame (the title's 3D logo and its letterbox border), and a cutscene (a demo, or a camera the stage scripts: an "event camera")
// is framed by the game for the whole picture, so they all keep the original field of view.
static bool inGameScene() {
    GameSystem* pSystem = SingletonHolder< GameSystem >::get();
    if (pSystem == nullptr || pSystem->mSceneController == nullptr) {
        return false;
    }
    // GXV_ASTRO_DOME_ZOOM_GUARD=1 keeps the original view in the whole AstroDome stage (its fixed room cameras, the Pull Star lecture,
    // the galaxy-select map); read once. Off when unset here, but the Vita sets it to 1 (vita/vgx-game/entry.cpp, since 2.0).
    static const bool domeZoomGuard = EnvInt("GXV_ASTRO_DOME_ZOOM_GUARD", 0) == 1;
    return MR::isEqualSceneName("Game") && !MR::isStageFileSelect() &&
           !(domeZoomGuard && MR::isEqualStageName("AstroDome"));
}

static bool inCutscene() {
    if (MR::isExistSceneObj(SceneObj_DemoDirector) && MR::isDemoActive()) {
        return true;
    }
    return MR::isEventCameraActive();
}

// GXV_ZOOM_PULLSTAR (zoomclip23, 8 Nov; default 1; a MINOR CHANGE: the zoom the player chose is set aside for a moment). A Pull Star is
// grabbed by pointing at it, and the pointer can only reach what is in the picture. The game's cameras frame a Pull Star chain so the
// next star is in the picture at the original field of view; zoomed in, it often lies just past the edge and cannot be pointed at
// ("issues reaching stars when they're a bit out of screen"). While a Pull Star is pointable (Mario within its range), pointed at,
// pulling, or Mario is held by one (GCaptureTarget.cpp / GCapture.cpp call port_zoom_relax_request every turn), the zoom eases to 1.0
// as in a cutscene, and back to the chosen zoom 45 turns (0.75 s) after the last request. 0: the zoom stays.
extern uint32_t gPortAlodTurn;  // (ActorTurnLod.cpp: counts every turn, also a stopped one; SceneExecutor.cpp)
static uint32_t sRelaxUntilTurn = 0;
static bool sRelaxRequested = false;
static bool zoomRelaxOn() {
    return PORT_HOT_KNOB(bool, EnvInt("GXV_ZOOM_PULLSTAR", 1) != 0);
}
extern "C" void port_zoom_relax_request(void) {
    if (!zoomRelaxOn()) {
        return;
    }
    sRelaxUntilTurn = gPortAlodTurn + 45u;
    sRelaxRequested = true;
}
static bool inZoomRelax() {
    return sRelaxRequested && static_cast< int32_t >(sRelaxUntilTurn - gPortAlodTurn) > 0;
}

// GXV_ZOOM_FIXEDCAM (galmap23, 8 Nov; default 1 on the Vita, vita/vgx-game/entry.cpp; a MINOR CHANGE: the zoom the player chose is set
// aside while the stage holds the camera still). A game camera whose eye and target the stage sets (CAM_TYPE_POINT_FIX, the
// "complete fixed" shot) is a composed picture like a cutscene: the zoom crops it and Mario can walk out of the picture while the
// original keeps him in it. While the game camera man runs such a camera, the zoom eases to 1.0 over the same 24 updates as for a
// cutscene, and back when the stage hands over to a moving camera. 2: also the fixed-eye cameras that turn to follow Mario
// (CAM_TYPE_EYEPOS_FIX, CAM_TYPE_EYEPOS_FIX_THERE). 0: the zoom stays (2.2.1).
// Not needed for the menus, maps and cutscenes, which already keep the original view: the dome's galaxy-select map
// (AstroDomeCameraController) and the Launch Star flight (SpinDriverCamera, MultiEventCamera) are event cameras (inCutscene); the
// whole AstroDome stage is at 1.0 with GXV_ASTRO_DOME_ZOOM_GUARD (Vita default 1 since 2.0); the star-select screen has its own
// camera context made at boot, outside the game scene (1.0, never set again); file select and the non-game scenes are excluded;
// the pause-menu galaxy map is 2D.
static int fixedCamMode() {
    return PORT_HOT_KNOB(int, static_cast< int >(EnvInt("GXV_ZOOM_FIXEDCAM", 1)));
}
static bool inFramedGameCamera() {
    const int mode = fixedCamMode();
    if (mode <= 0 || !MR::isExistSceneObj(SceneObj_CameraDirector)) {
        return false;
    }
    const CameraDirector* pDirector = MR::getCameraDirector();
    if (pDirector == nullptr || pDirector->mCameraManGame == nullptr ||
        pDirector->getCurrentCameraMan() != static_cast< CameraMan* >(pDirector->mCameraManGame)) {
        return false;
    }
    const CameraManGame* pMan = pDirector->mCameraManGame;
    if (pMan->mCamera == nullptr || pMan->mHolder == nullptr) {
        return false;
    }
    // (the holder makes one camera per type, in its table's order: the indices are the same in every scene)
    static s32 sPointFix = -2, sEyeFix = -2, sEyeFixThere = -2;
    if (sPointFix == -2) {
        sPointFix = pMan->mHolder->getIndexOf("CAM_TYPE_POINT_FIX");
        sEyeFix = pMan->mHolder->getIndexOf("CAM_TYPE_EYEPOS_FIX");
        sEyeFixThere = pMan->mHolder->getIndexOf("CAM_TYPE_EYEPOS_FIX_THERE");
    }
    const s32 type = pMan->mHolder->getIndexOf(pMan->mCamera);
    if (type < 0) {
        return false;
    }
    return type == sPointFix || (mode >= 2 && (type == sEyeFix || type == sEyeFixThere));
}

// Why the zoom is set aside now (nullptr: it is not). Every reason eases (24 camera updates), none jumps.
static const char* zoomSetAsideReason() {
    if (inCutscene()) {
        return "cutscene / event camera";
    }
    if (inZoomRelax()) {
        return "Pull Star in reach";  // (GXV_ZOOM_PULLSTAR, see above)
    }
    if (inFramedGameCamera()) {
        return "fixed camera";  // (GXV_ZOOM_FIXEDCAM, see above)
    }
    return nullptr;
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
    const char* reason = nullptr;
    if (!inGameScene()) {
        target = 1.0f;
        jump = true;
        reason = "non-game scene / file select / dome";
    } else if (!init && (reason = zoomSetAsideReason()) != nullptr) {
        target = 1.0f;  // (a cutscene; GXV_ZOOM_PULLSTAR: a Pull Star is in reach; GXV_ZOOM_FIXEDCAM: a fixed camera; see above)
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
        port_log("[zoom] target %.2f (%s), now %.2f", target, target > 1.0f ? "play" : (reason != nullptr ? reason : "camera init"),
                 sZoomNow);
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
#ifdef TARGET_PC
    mPortFovyGame = 45.0f;
#endif
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
#ifdef TARGET_PC
    mPortFovyGame = fovy;
#endif
    updateProjectionMtx();
}

void CameraContext::setShakeOffset(f32 x, f32 y) {
    mShakeOffset.set(x, y);
    updateProjectionMtx();
}

void CameraContext::updateProjectionMtx() {
    mProjection.makePerspective(mFovy, getAspect(), mNearZ, mFarZ);

    TProj3f trans;
#ifdef TARGET_PC
    // Reduced motion (GXV_REDUCED_MOTION, port/reduced_motion.h): the shake in the projection is a quarter of its size. This is the one
    // projection the renderer, screen positions, the Star Pointer hit-test and the HUD marks all read, so the pointer hits what is on screen.
    trans.makeTrans(port::motion::ShakeScale(mShakeOffset.x), port::motion::ShakeScale(mShakeOffset.y));
#else
    trans.makeTrans(mShakeOffset.x, mShakeOffset.y);
#endif

    mProjection.concat(trans, mProjection);
}
