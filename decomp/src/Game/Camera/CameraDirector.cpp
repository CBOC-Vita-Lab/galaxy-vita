#include "Game/Camera/CameraDirector.hpp"
#include "Game/Boss/BossStinkBug.hpp"
#include "Game/Camera/CameraCover.hpp"
#include "Game/Camera/CameraHolder.hpp"
#include "Game/Camera/CameraLocalUtil.hpp"
#include "Game/Camera/CameraMan.hpp"
#include "Game/Camera/CameraManEvent.hpp"
#include "Game/Camera/CameraManGame.hpp"
#include "Game/Camera/CameraManPause.hpp"
#include "Game/Camera/CameraManSubjective.hpp"
#include "Game/Camera/CameraParamChunk.hpp"
#include "Game/Camera/CameraParamChunkHolder.hpp"
#include "Game/Camera/CameraParamChunkID.hpp"
#include "Game/Camera/CameraPoseParam.hpp"
#include "Game/Camera/CameraRailHolder.hpp"
#include "Game/Camera/CameraRegisterHolder.hpp"
#include "Game/Camera/CameraRotChecker.hpp"
#include "Game/Camera/CameraShaker.hpp"
#include "Game/Camera/CameraTargetHolder.hpp"
#include "Game/Camera/CameraTargetMtx.hpp"
#include "Game/Camera/CameraViewInterpolator.hpp"
#include "Game/Camera/GameCameraCreator.hpp"
#include "Game/Camera/OnlyCamera.hpp"
#include "Game/LiveActor/ActorCameraInfo.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SequenceUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include "Game/MP/MpSpectate.hpp"  // MP3 spectate: L / R switch the watched player, no "can't" beep
#include "port/mp_bridge.h"  // MP3: D-pad down is the greeting in a session
#include <cstring>
#ifdef TARGET_PC
#include "Game/Map/HitInfo.hpp"
#include "Game/Util/AreaObjUtil.hpp"
#include "Game/Util/MapUtil.hpp"
#include "Game/Util/TriangleFilter.hpp"
#include "port/free_camera.h"
#include <cmath>
#include <cstdlib>
extern "C" void port_hrw_camera_cut(void);  // GXV_HRW (platform/src/gx/vgx_galaxy.cpp): an explicit camera change (event, reset, end of event): the pair in flight is not re-projected
extern "C" int port_zoom_menu_active(void) __attribute__((weak));
extern "C" bool port_settings_native_menu_open(void) __attribute__((weak));
extern "C" int mp_menu_wants_no_idle_pointer(void) __attribute__((weak));
extern "C" int port_turn_skipped_raw(void) __attribute__((weak));
#endif

void CameraDirector_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
    (void)0.5f;
    (void)2.0f;
    (void)MR::pi();
}

namespace {
    // static const s32 sDefaultBlendFrame =
    static const s32 sSubjectivePosInterpolateFrame = 0;
    static const s32 sSubjectiveRotInterpolateFrame = 10;
    static const s32 sSubjectiveInterpolateFrame = 20;
    // static const s32 sAnimCamBlendFrame =
    // static const f32 sAnimCamRate =
    static f32 sDefaultFovy = 45.0f;
    static const char* sTalkCameraName = "\x8b\xa4\x92\xca\x89\xef\x98\x62\x83\x4a\x83\x81\x83\x89";
    static const char* sStartAnimCameraName = "\x83\x58\x83\x5e\x81\x5b\x83\x67\x83\x41\x83\x6a\x83\x81\x83\x4a\x83\x81\x83\x89";
    static const char* sSubjectiveCameraName = "\x8e\xe5\x8a\xcf\x83\x4a\x83\x81\x83\x89";
    // static const s32 sTestAnimCameraName =
    // static const s32 sConvertSize =
    // static const s32 sConvertQuality =
    static s32 sUpdateCounter;
    // MP3 spectate: the event camera a waiting player's view of a teammate runs as (MpSpectate.cpp starts it, MpWatchTarget is its target)
    static const char* sMpWatchCameraName = "MpWatch";
    // static const s32 sAnimCameraName =
    // static const s32 sAnimCameraZoneID =
#ifdef TARGET_PC
    // freecam31: Gravity-aware right-stick Free Camera state (scoped to the active CameraDirector).
    struct FreeCamRuntime {
        port::freecam::State state;
        const CameraDirector* owner = nullptr;
        const CameraHolder* maskHolder = nullptr;
        uint64_t ineligibleMask = 0;
        uint64_t worldAnchoredMask = 0;
        const CameraParamChunk* prevChunk = nullptr;
        int64_t prevWallNs = 0;
        bool pendingHardCut = false;
        bool pendingParallelReset = false;
        bool lastEligible = false;
        unsigned logCounter = 0;
        float lastHitFrac = 2.0f;
        float lastDesiredLift = 0.0f;
        unsigned floorProbeTick = 0;
        int64_t raycastSumNs = 0;
        unsigned raycastTicks = 0;
        bool lastModified = false;
        float turnBlend = 0.0f;

        // Explicitly called from CameraDirector::CameraDirector() so deterministic JKRHeap address
        // reuse across stage loads never leaves a stale prevChunk or single-player/MP mask.
        void resetForNewDirector(const CameraDirector* dir) {
            owner = dir;
            maskHolder = nullptr;
            ineligibleMask = 0;
            worldAnchoredMask = 0;
            prevChunk = nullptr;
            prevWallNs = 0;
            pendingHardCut = false;
            pendingParallelReset = false;
            lastEligible = false;
            logCounter = 0;
            lastHitFrac = 2.0f;
            lastDesiredLift = 0.0f;
            floorProbeTick = 0;
            raycastSumNs = 0;
            raycastTicks = 0;
            lastModified = false;
            turnBlend = 0.0f;
            state.ResetHard();
        }

        void ensureMasks(const CameraHolder* holder) {
            if (holder == nullptr) {
                maskHolder = nullptr;
                ineligibleMask = ~0ull;
                worldAnchoredMask = 0;
                return;
            }
            if (maskHolder == holder) return;
            maskHolder = holder;
            ineligibleMask = 0;
            worldAnchoredMask = 0;
            // Cameras that MUST yield: 2D/2.5D, fixed-eye, cutscenes/demos, talk, death, black hole,
            // first-person, MP spectate, pointer-aim DPD, and MedianPlanet (reads getCameraInvViewMtx()
            // every frame, which would otherwise feed FreeCam output back into itself).
            static const char* const kBlocked[] = {
                "CAM_TYPE_2D_SLIDE",
                "CAM_TYPE_FRONT_AND_BACK",
                "CAM_TYPE_POINT_FIX",
                "CAM_TYPE_EYEPOS_FIX",
                "CAM_TYPE_EYEPOS_FIX_THERE",
                "CAM_TYPE_RAIL_DEMO",
                "CAM_TYPE_SPIRAL_DEMO",
                "CAM_TYPE_TALK",
                "CAM_TYPE_DEAD",
                "CAM_TYPE_BLACK_HOLE",
                "CAM_TYPE_ANIM",
                "CAM_TYPE_SUBJECTIVE",
                "CAM_TYPE_MP_WATCH",
                "CAM_TYPE_DPD",
                "CAM_TYPE_MEDIAN_PLANET",
            };
            for (const char* name : kBlocked) {
                const s32 idx = holder->getIndexOf(name);
                if (idx >= 0 && idx < 64) {
                    ineligibleMask |= (1ull << static_cast<unsigned>(idx));
                }
            }
            // Free-tracking cameras whose underlying basePos drifts or stays fixed on a sphere while
            // Mario moves: use parallel-transport world-anchored orbit so Mario never crosses an
            // invisible underlying camera pole.
            static const char* const kWorldAnchored[] = {
                "CAM_TYPE_FOLLOW",
                "CAM_TYPE_WONDER_PLANET",
                "CAM_TYPE_CUBE_PLANET",
                "CAM_TYPE_WATER_FOLLOW",
                "CAM_TYPE_WATER_PLANET",
                "CAM_TYPE_WATER_PLANET_BOSS",
                "CAM_TYPE_FOO_FIGHTER",
                "CAM_TYPE_FOO_FIGHTER_PLANET",
                "CAM_TYPE_GROUND",
                "CAM_TYPE_RACE_FOLLOW",
                "CAM_TYPE_TRIPOD_PLANET",
                "CAM_TYPE_INWARD_SPHERE",
                "CAM_TYPE_TRUNDLE",
            };
            for (const char* name : kWorldAnchored) {
                const s32 idx = holder->getIndexOf(name);
                if (idx >= 0 && idx < 64) {
                    worldAnchoredMask |= (1ull << static_cast<unsigned>(idx));
                }
            }
        }
    };

    static FreeCamRuntime sFreeCam;

    static bool freeCamLogOn() {
        static const bool on = [] {
            const char* e = std::getenv("GXV_FREE_CAM_LOG");
            return e && *e && std::atoi(e) != 0;
        }();
        return on;
    }

    static bool isFreeCamEligible(const CameraDirector* dir, bool* outWorldAnchored = nullptr) {
        if (outWorldAnchored) *outWorldAnchored = false;
        if (dir == nullptr || dir->mCameraManGame == nullptr || dir->mPoseParam1 == nullptr) return false;
        if (!MR::isEqualSceneName("Game") || MR::isStageFileSelect() ||
            MR::isEqualStageName("AstroDome") || MR::isEqualStageName("EpilogueDemoStage")) return false;
        if (dir->mIsStartCameraActive || dir->mIsSubjectiveCamera || dir->mIsStartSubjectiveCamera) return false;
        if (MR::isDemoActive() || MR::isEventCameraActive() || MR::isPlayerDead() ||
            MR::isPowerStarGetDemoActive() || MR::isStageStateScenarioOpeningCamera() ||
            MR::isExecScenarioStarter()) return false;
        if (dir->getCurrentCameraMan() != static_cast<const CameraMan*>(dir->mCameraManGame)) return false;
        if (port_zoom_menu_active && port_zoom_menu_active()) return false;
        if (port_settings_native_menu_open && port_settings_native_menu_open()) return false;
        if (mp_menu_wants_no_idle_pointer && mp_menu_wants_no_idle_pointer() != 0) return false;
        if (mp_dialog_active()) return false;
        const CameraManGame* man = dir->mCameraManGame;
        if (man->mCamera == nullptr || man->mHolder == nullptr) return false;
        const s32 camIdx = man->mHolder->getIndexOf(man->mCamera);
        if (camIdx < 0 || camIdx >= 64) return false;
        sFreeCam.ensureMasks(man->mHolder);
        const uint64_t bit = 1ull << static_cast<unsigned>(camIdx);
        if ((sFreeCam.ineligibleMask & bit) != 0) return false;
        // Read live roll from mCameraManGame (never stale mPoseParam1 before calcPose()).
        if (std::fabs(CameraLocalUtil::getRoll(man)) > 0.35f) return false;
        if (MR::isExistSceneObj(SceneObj_AreaObjContainer)) {
            if (const CameraTargetObj* target = dir->mTargetHolder ? dir->mTargetHolder->get() : nullptr) {
                const TVec3f& tpos = target->getPosition();
                if (MR::getAreaObj("PlaneModeCube", tpos) != nullptr ||
                    MR::getAreaObj("PlaneCircularModeCube", tpos) != nullptr ||
                    MR::getAreaObj("PipeModeCube", tpos) != nullptr) return false;
            }
        }
        if (outWorldAnchored) *outWorldAnchored = (sFreeCam.worldAnchoredMask & bit) != 0;
        return true;
    }

    static bool isFreeCamResetAvailable(const CameraDirector* dir) {
        if (!port_free_camera_enabled()) return false;
        return isFreeCamEligible(dir);
    }

    static port::freecam::Vec3 toFcVec(const TVec3f& v) { return {v.x, v.y, v.z}; }

    static void applyFreeCamera(CameraDirector* dir) {
        if (sFreeCam.owner != dir) sFreeCam.resetForNewDirector(dir);
        const bool enabled = port_free_camera_enabled() != 0;
        if (!enabled && sFreeCam.state.IsNeutral() && !sFreeCam.pendingHardCut) {
            sFreeCam.prevWallNs = 0;
            sFreeCam.lastModified = false;
            sFreeCam.turnBlend = 0.0f;
            return;
        }

        // Galaxy ALWAYS simulates CameraDirector::movement() at a fixed 60 logical turns/s
        // (even in 60 Assist HRW and Quality 30, where wall-time alternates ~5ms/~27ms per pair).
        // Always step with fixed dt = 1/60s to prevent HRW sawtooth judder; use wall-clock ONLY
        // to detect real pause/suspend gaps (> 200 ms) so stale velocity is zeroed on resume.
        const int64_t nowNs = port_host_time_ns();
        const bool resumePause = (sFreeCam.prevWallNs > 0 && nowNs > sFreeCam.prevWallNs && (nowNs - sFreeCam.prevWallNs) > 200000000ll);
        sFreeCam.prevWallNs = nowNs;

        bool worldAnchored = false;
        const bool eligible = isFreeCamEligible(dir, &worldAnchored);
        // Restrict subjective freeze strictly to live gameplay FP view (never carry into interrupting demos/events).
        const bool holdForSubjective = enabled &&
            (dir->mIsSubjectiveCamera || dir->mIsStartSubjectiveCamera) &&
            !MR::isDemoActive() && !MR::isEventCameraActive() &&
            dir->getCurrentCameraMan() == static_cast<const CameraMan*>(dir->mCameraManGame);
        float camX = 0.0f, camY = 0.0f;
        if (enabled && eligible) {
            port_input_cam_stick(&camX, &camY);
        }

        const CameraParamChunk* curChunk = (dir->mCameraManGame != nullptr) ? dir->mCameraManGame->mChunk : nullptr;
        const bool chunkChanged = (sFreeCam.prevChunk != nullptr && curChunk != sFreeCam.prevChunk);
        sFreeCam.prevChunk = curChunk;

        const bool hardCut = sFreeCam.pendingHardCut ||
            (dir->mViewInterpolator != nullptr && dir->mViewInterpolator->mIsForceCameraChange);
        sFreeCam.pendingHardCut = false;

        port::freecam::StepInput in;
        in.dt = 1.0f / 60.0f;
        in.stickX = camX;
        in.stickY = camY;
        in.enabled = enabled;
        in.eligible = eligible;
        in.hold = holdForSubjective;
        in.worldAnchored = worldAnchored;
        in.hardCut = hardCut;
        in.resumePause = resumePause;
        in.chunkChanged = chunkChanged;
        in.resetTrigger = enabled && eligible && CameraLocalUtil::testCameraPadTriggerReset();
        in.invertMask = port_input_camera_invert_mask();
        in.speedLevel = port_camera_speed();
        in.basePos = toFcVec(dir->mPoseParam1->mPos);
        in.baseWatch = toFcVec(dir->mPoseParam1->mWatchPos);
        in.baseUp = toFcVec(dir->mPoseParam1->mUpVec);
        in.watchUp = toFcVec(dir->mPoseParam1->mWatchUpVec);
        if (const CameraTargetObj* target = dir->mTargetHolder ? dir->mTargetHolder->get() : nullptr) {
            in.targetGravity = toFcVec(target->getGravityVector());
            in.targetFront = toFcVec(target->getFrontVec());
        }

        const port::freecam::StepOutput out = sFreeCam.state.Step(in);
        if (out.releasedNative) {
            sFreeCam.pendingParallelReset = true;
        }
        if (!out.modified || hardCut) {
            sFreeCam.lastHitFrac = 2.0f;
            sFreeCam.lastDesiredLift = 0.0f;
        }
        const bool activeOrbit = out.modified && out.needsLosCheck && !hardCut;
        sFreeCam.lastModified = activeOrbit;
        sFreeCam.turnBlend = activeOrbit ? sFreeCam.state.TurnBlend() : 0.0f;
        if (out.modified) {
            if (!out.needsLosCheck) sFreeCam.lastHitFrac = 2.0f;
            if (!out.needsFloorCheck) sFreeCam.lastDesiredLift = sFreeCam.state.floorLift;
            port::freecam::Vec3 finalPos = out.pos;
            port::freecam::Vec3 finalUp = out.up;
            // Skip map collision raycasts on undrawn/skipped turns (reuse cached hit/lift while still
            // advancing smooth exponential filters) to save 1-3 ms/turn on heavy stages like Observatory.
            const bool undrawnTurn = (port_turn_skipped_raw && port_turn_skipped_raw() != 0);
            const bool doLogTime = freeCamLogOn();
            const int64_t rcT0 = doLogTime ? port_host_time_ns() : 0;
            TriangleFilterFunc filter(&MR::isCameraCodeThrough);
            if (out.needsLosCheck && out.baseDist > 1.0f) {
                if (!undrawnTurn) {
                    float hitFrac = 2.0f;
                    const TVec3f watch(in.baseWatch.x, in.baseWatch.y, in.baseWatch.z);
                    const TVec3f ray(out.pos.x - in.baseWatch.x, out.pos.y - in.baseWatch.y, out.pos.z - in.baseWatch.z);
                    TVec3f hitPos;
                    Triangle tri;
                    if (MR::getFirstPolyOnLineToMap(&hitPos, &tri, watch, ray, nullptr, &filter)) {
                        const TVec3f toHit = hitPos - watch;
                        hitFrac = toHit.length() / out.baseDist;
                    }
                    sFreeCam.lastHitFrac = hitFrac;
                }
                finalPos = sFreeCam.state.ApplyCollisionPull(in.baseWatch, out.pos, sFreeCam.lastHitFrac, {0.0f, 0.0f, 0.0f}, sFreeCam.state.smoothUp, in.dt);
            }
            if (out.needsFloorCheck) {
                // Continuous downward floor probe + upward ceiling clamp (prevents near-plane/mBinder
                // floor clipping with zero 1-frame binary vertical pops and zero ceiling clipping).
                // Probe every drawn turn whenever near a floor, actively pitching, or recentering;
                // otherwise every 2nd drawn turn (30 Hz at 60 fps, 15 Hz at 30 fps) to keep heavy stages locked.
                const port::freecam::Vec3& up = sFreeCam.state.smoothUp;
                if (!undrawnTurn) {
                    const bool activeFloor = (sFreeCam.state.floorLift >= 0.5f || sFreeCam.lastDesiredLift >= 0.5f);
                    const bool pitching = (std::fabs(camY) > 0.05f || std::fabs(sFreeCam.state.velPitch) > 0.05f || sFreeCam.state.recentering);
                    const unsigned interval = (activeFloor || pitching) ? 1u : 2u;
                    if ((++sFreeCam.floorProbeTick % interval) == 0u) {
                        const float kProbeAbove = 60.0f;
                        const float kProbeLen = kProbeAbove + port::freecam::kFloorClearance;
                        const TVec3f probeStart(finalPos.x + up.x * kProbeAbove, finalPos.y + up.y * kProbeAbove, finalPos.z + up.z * kProbeAbove);
                        const TVec3f probeDown(-up.x * kProbeLen, -up.y * kProbeLen, -up.z * kProbeLen);
                        TVec3f floorHit;
                        Triangle floorTri;
                        float desiredLift = 0.0f;
                        if (MR::getFirstPolyOnLineToMap(&floorHit, &floorTri, probeStart, probeDown, nullptr, &filter)) {
                            const TVec3f* fn = floorTri.getNormal(0);
                            if (fn != nullptr && (fn->x * up.x + fn->y * up.y + fn->z * up.z) > 0.5f) {
                                const float heightAboveFloor =
                                    (finalPos.x - floorHit.x) * up.x +
                                    (finalPos.y - floorHit.y) * up.y +
                                    (finalPos.z - floorHit.z) * up.z;
                                if (heightAboveFloor < port::freecam::kFloorClearance) {
                                    desiredLift = port::freecam::kFloorClearance - heightAboveFloor;
                                    if (desiredLift > 1.0f) {
                                        const float kCeilMargin = 30.0f;
                                        const TVec3f ceilStart(finalPos.x, finalPos.y, finalPos.z);
                                        const TVec3f ceilRay(up.x * (desiredLift + kCeilMargin), up.y * (desiredLift + kCeilMargin), up.z * (desiredLift + kCeilMargin));
                                        TVec3f ceilHit;
                                        Triangle ceilTri;
                                        if (MR::getFirstPolyOnLineToMap(&ceilHit, &ceilTri, ceilStart, ceilRay, nullptr, &filter)) {
                                            const float ceilDist =
                                                (ceilHit.x - finalPos.x) * up.x +
                                                (ceilHit.y - finalPos.y) * up.y +
                                                (ceilHit.z - finalPos.z) * up.z;
                                            const float maxLift = ceilDist - kCeilMargin;
                                            desiredLift = maxLift > 0.0f ? (desiredLift < maxLift ? desiredLift : maxLift) : 0.0f;
                                        }
                                    }
                                }
                            }
                        }
                        sFreeCam.lastDesiredLift = desiredLift;
                    }
                }
                finalPos = sFreeCam.state.ApplyFloorLift(in.baseWatch, finalPos, up, sFreeCam.lastDesiredLift, in.dt, &finalUp);
            }
            if (doLogTime) {
                sFreeCam.raycastSumNs += (port_host_time_ns() - rcT0);
                ++sFreeCam.raycastTicks;
            }
            dir->mPoseParam1->mPos.set(finalPos.x, finalPos.y, finalPos.z);
            dir->mPoseParam1->mUpVec.set(finalUp.x, finalUp.y, finalUp.z);
        }

        if (freeCamLogOn()) {
            const bool stateFlipped = (eligible != sFreeCam.lastEligible);
            const bool activeStick = (std::fabs(camX) > 0.05f || std::fabs(camY) > 0.05f);
            if (stateFlipped || out.handledReset || ((activeStick || out.modified) && (++sFreeCam.logCounter % 30u == 1u))) {
                const char* camName = (dir->mCameraManGame && dir->mCameraManGame->mHolder && dir->mCameraManGame->mCamera)
                    ? dir->mCameraManGame->mHolder->getNameStrOf(dir->mCameraManGame->mHolder->getIndexOf(dir->mCameraManGame->mCamera))
                    : "none";
                const unsigned avgRcUs = sFreeCam.raycastTicks ? static_cast<unsigned>((sFreeCam.raycastSumNs / sFreeCam.raycastTicks) / 1000ll) : 0u;
                sFreeCam.raycastSumNs = 0;
                sFreeCam.raycastTicks = 0;
                port_log_summary("freecam: en=%d elig=%d anch=%d cam=%s stick=(%.2f,%.2f) yaw=%.1fdeg pitch=%.1fdeg pull=%.2f lift=%.0f rcUs=%u mod=%d rst=%d",
                                 enabled ? 1 : 0, eligible ? 1 : 0, sFreeCam.state.anchored ? 1 : 0,
                                 camName ? camName : "?", camX, camY,
                                 sFreeCam.state.yaw * (180.0f / port::freecam::kPi),
                                 sFreeCam.state.pitch * (180.0f / port::freecam::kPi),
                                 sFreeCam.state.pull, sFreeCam.state.floorLift, avgRcUs, out.modified ? 1 : 0, out.handledReset ? 1 : 0);
            }
        }
        sFreeCam.lastEligible = eligible;
    }
#endif
};  // namespace

#ifdef TARGET_PC
extern "C" int port_free_camera_owns_reset(void) {
    if (!port_free_camera_enabled()) return 0;
    if (!MR::isExistSceneObj(SceneObj_CameraDirector)) return 0;
    return isFreeCamEligible(MR::getCameraDirector()) ? 1 : 0;
}

extern "C" int port_free_camera_take_parallel_reset(void) {
    if (!sFreeCam.pendingParallelReset) return 0;
    sFreeCam.pendingParallelReset = false;
    return 1;
}

extern "C" int port_free_camera_active(void) {
    if (!MR::isExistSceneObj(SceneObj_CameraDirector)) return 0;
    return sFreeCam.lastModified ? 1 : 0;
}

extern "C" float port_free_camera_turn_blend(void) {
    if (!MR::isExistSceneObj(SceneObj_CameraDirector)) return 0.0f;
    return sFreeCam.turnBlend;
}

extern "C" float port_free_camera_clip_pad(float fovyDeg) {
    if (!MR::isExistSceneObj(SceneObj_CameraDirector) || !sFreeCam.lastModified) return 1.0f;
    return sFreeCam.state.ClipPadFactor(fovyDeg);
}
#endif

CameraDirector::CameraDirector(const char* pName) : NameObj(pName) {
    mTargetObj = nullptr;
    mStack = new CameraManStack();
    mOnlyCamera = new OnlyCamera("OnlyCamera");
    mPoseParam1 = new CameraPoseParam();
    mPoseParam2 = new CameraPoseParam();
    mHolder = new CameraHolder("\x83\x4a\x83\x81\x83\x89\x83\x7a\x83\x8b\x83\x5f\x81\x5b");
    mChunkHolder = new CameraParamChunkHolder(mHolder, "\x83\x70\x83\x89\x83\x81\x81\x5b\x83\x5e");
    mCameraCreator = new GameCameraCreator(mChunkHolder);
    mRailHolder = new CameraRailHolder("\x83\x4a\x83\x81\x83\x89\x83\x8c\x81\x5b\x83\x8b\x8a\xc7\x97\x9d");
    mRegisterHolder = new CameraRegisterHolder("\x83\x4a\x83\x81\x83\x89\x83\x8c\x83\x57\x83\x58\x83\x5e");
    mTargetHolder = new CameraTargetHolder();
    mShaker = new CameraShaker("\x90\x55\x93\xae");
    mViewInterpolator = new CameraViewInterpolator();
    mCover = new CameraCover("CameraCover");
    mRotChecker = new CameraRotChecker();
    mCameraManGame = new CameraManGame(mHolder, mChunkHolder, "\x83\x51\x81\x5b\x83\x80\x83\x4a\x83\x81\x83\x89\x83\x7d\x83\x93");
    mCameraManEvent = new CameraManEvent(mHolder, mChunkHolder, "\x83\x43\x83\x78\x83\x93\x83\x67\x83\x4a\x83\x81\x83\x89\x83\x7d\x83\x93");
    mCameraManPause = new CameraManPause("\x83\x7c\x81\x5b\x83\x59\x83\x4a\x83\x81\x83\x89\x83\x7d\x83\x93");
    mCameraManSubjective = new CameraManSubjective("\x8e\xe5\x8a\xcf\x83\x4a\x83\x81\x83\x89\x83\x7d\x83\x93");
    _58 = false;
    mEventNum = 0;
    mIsStartCameraActive = true;
    mStartTime = 0;
    mStartCameraCreated = false;
    mCameraTargetMtx = new CameraTargetMtx("\x83\x4a\x83\x81\x83\x89\x83\x5e\x81\x5b\x83\x51\x83\x62\x83\x67\x83\x5f\x83\x7e\x81\x5b");
    mRequestCameraManReset = false;
    mRequestCameraLocalOffsetReset = false;
    mIsSubjectiveCamera = false;
    mIsStartSubjectiveCamera = false;
    mSubjectiveFrame = 0;
    mNearZ = -100.0f;
    mIsSubjectiveCalced = false;
    _1F1 = true;
    mIsCameraNG = false;

    MR::connectToSceneCamera(this);
    push(mCameraManGame);
    mTargetMtx.identity();
    mCameraTargetMtx->setMtx(mTargetMtx);
    setInterpolation(0);
    mCameraManSubjective->owned(this);
    mViewMtx.identity();
    MR::createCenterScreenBlur();
#ifdef TARGET_PC
    sFreeCam.resetForNewDirector(this);
#endif
}

void CameraDirector::init(const JMapInfoIter& rIter) {
}

void CameraDirector::movement() {
    ::sUpdateCounter++;

    backLastMtx();
    mTargetHolder->movement();
    updateCameraMan();
    calcPose();
#ifdef TARGET_PC
    applyFreeCamera(this);
#endif
    createViewMtx();
    getCurrentCameraMan()->mMatrix.set(MR::getCameraInvViewMtx());
    mPoseParam2->copyFrom(*getCurrentCameraMan()->mPoseParam);
    calcSubjective();
    mShaker->movement();
    checkStartCondition();
    checkEndOfEventCamera();
    mRotChecker->update();

    mRequestCameraManReset = false;
    mRequestCameraLocalOffsetReset = false;
}

void CameraDirector::setTarget(CameraTargetObj* pTarget) {
    mTargetHolder->set(pTarget);
}

CameraTargetObj* CameraDirector::getTarget() {
    return mTargetHolder->get();
}

void CameraDirector::push(CameraMan* pMan) {
    if (mStack->mCount != 0) {
        getCurrentCameraMan()->deactivate(this);
    }

    mStack->mElements[mStack->mCount++] = pMan;

    pMan->owned(this);
    pMan->activate(this);
}

CameraMan* CameraDirector::pop() {
    CameraMan* man = mStack->mElements[--mStack->mCount];

    man->deactivate(this);
    man->released(this);

    if (mStack->mCount != 0) {
        CameraMan* newMan = getCurrentCameraMan();
        newMan->activate(this);
    }

    return man;
}

void CameraDirector::backLastMtx() {
    if (mIsSubjectiveCalced) {
        MR::setCameraViewMtx(mViewMtx, false, false, TVec3f(0.0f, 0.0f, 0.0f));

        TVec3f invTrans;
        MR::getCameraInvViewMtx().getTrans(invTrans);
    }
}

void CameraDirector::updateTarget() {
}

CameraMan* CameraDirector::getCurrentCameraMan() const {
    return mStack->mElements[mStack->mCount - 1];
}

void CameraDirector::updateCameraMan() {
    if (mRequestCameraManReset) {
        resetCameraMan();
    }

    if (mRequestCameraLocalOffsetReset) {
        CameraMan* man = getCurrentCameraMan();
        man->mRequestLOfsReset = true;
    }

    getCurrentCameraMan()->movement();
    controlCameraSE();
}

void CameraDirector::calcPose() {
    switchAntiOscillation();

    if (getCurrentCameraMan()->isCollisionOff()) {
        mViewInterpolator->mIsCollisionOff = true;
    }

    if (getCurrentCameraMan()->isCorrectingErpPositionOff()) {
        mViewInterpolator->mIsCorrectErpPositionOn = false;
    }

    if (getCurrentCameraMan()->isZeroFrameMoveOff()) {
        mOnlyCamera->mIsZeroFrameMoveOff = true;
    }

    mOnlyCamera->calcPose(getCurrentCameraMan());
    mPoseParam1->copyFrom(*mOnlyCamera->mPoseParam);
}

void CameraDirector::calcSubjective() {
    mViewMtx.set(MR::getCameraViewMtx());
    mIsSubjectiveCalced = true;

    if (MR::isDemoActive()) {
        MR::stopPlayerFpView();
    }

    if (mIsSubjectiveCamera) {
        mSubjectiveFrame++;
        if (mSubjectiveFrame > ::sSubjectiveInterpolateFrame) {
            mSubjectiveFrame = ::sSubjectiveInterpolateFrame;
        }
    } else {
        mSubjectiveFrame--;
        if (mSubjectiveFrame <= 0) {
            mSubjectiveFrame = 0;
            if (mIsStartSubjectiveCamera) {
                if (mNearZ >= 0.0f) {
                    MR::setNearZ(mNearZ);
                }

                mNearZ = -100.0f;
                MR::turnOnDOFInSubjective();
                mIsStartSubjectiveCamera = false;
            }
        }
    }

    if (!mIsStartSubjectiveCamera) {
        return;
    }

    TPos3f inv;
    inv.set(MR::getCameraInvViewMtx());
    mCameraManSubjective->calc();

    TPos3f viewMtx;
    calcViewMtxFromPoseParam(&viewMtx, mCameraManSubjective->mPoseParam);

    s32 posFrame = mSubjectiveFrame - ::sSubjectivePosInterpolateFrame;
    if (posFrame > ::sSubjectiveInterpolateFrame - ::sSubjectivePosInterpolateFrame) {
        posFrame = ::sSubjectiveInterpolateFrame - ::sSubjectivePosInterpolateFrame;
    }

    f32 posRate;
    if (mSubjectiveFrame >= ::sSubjectiveInterpolateFrame - ::sSubjectivePosInterpolateFrame) {
        posRate = 1.0f;
    } else {
        posRate = MR::cos(MR::pi() + posFrame * MR::pi() / (::sSubjectiveInterpolateFrame - ::sSubjectivePosInterpolateFrame)) * 0.5f + 0.5f;
    }

    const CameraPoseParam* param = mCameraManSubjective->mPoseParam;
    TVec3f localPos;
    inv.getTrans(localPos);

    TVec3f newPos = localPos * (1.0f - posRate) + param->mPos * posRate;

    s32 rotFrame = mSubjectiveFrame - ::sSubjectiveRotInterpolateFrame;
    if (rotFrame < 0) {
        rotFrame = 0;
    }

    f32 rotRate = MR::cos(MR::pi() + rotFrame * MR::pi() / (::sSubjectiveInterpolateFrame - ::sSubjectiveRotInterpolateFrame)) * 0.5f + 0.5f;

    TQuat4f invRot;
    inv.getQuat(invRot);
    TQuat4f viewRot;
    viewMtx.getQuat(viewRot);

    TQuat4f newRot;
    newRot.slerp(invRot, viewRot, rotRate);

    viewMtx.makeQuat(newRot);

    TVec3f front;
    viewMtx.getZDir(front);
    viewMtx.setTrans(newPos);
    viewMtx.invert(viewMtx);

    MR::setCameraViewMtx(viewMtx, false, false, TVec3f(0.0f, 0.0f, 0.0f));
}

bool CameraDirector::isInterpolationOff() {
    return getCurrentCameraMan()->isInterpolationOff() || mViewInterpolator->mIsForceCameraChange;
}

void CameraDirector::switchAntiOscillation() {
    if (isInterpolationOff()) {
        mViewInterpolator->mIsAntiOscillation = false;
    } else {
        mViewInterpolator->mIsAntiOscillation = true;
    }
}

void CameraDirector::createViewMtx() {
    TPos3f view;
    calcViewMtxFromPoseParam(&view, mPoseParam1);

    mViewInterpolator->updateCameraMtx(view, mPoseParam1->getWatchPos(), getTargetObj(), CameraLocalUtil::getFovy(getCurrentCameraMan()));
}

void CameraDirector::checkStartCondition() {
    if (mIsStartCameraActive && getCurrentCameraMan() == mCameraManGame && mStartTime++ > 30 && mTargetHolder->isMoving()) {
        mIsStartCameraActive = false;
        mCameraManGame->endStartPosCamera();
    }
}

void CameraDirector::startEvent(s32 zoneID, const char* pName, const CameraTargetArg& rTargetArg, s32 frame) {
#ifdef TARGET_PC
    port_hrw_camera_cut();
#endif
    mViewInterpolator->mIsInterpolationOff = false;
    removeEndEventAtLanding(zoneID, pName);

    if (getCurrentCameraMan() != mCameraManEvent) {
        if (mStack->mCount != 0) {
            if (getCurrentCameraMan() == mCameraManGame) {
                mCameraManEvent->mPoseParam->copyFrom(*mCameraManGame->mPoseParam);
                mCameraManEvent->mMatrix.set(MR::getCameraInvViewMtx());
            }
        }

        push(mCameraManEvent);
    }

    mCameraManEvent->start(zoneID, pName, rTargetArg, frame);
}

void CameraDirector::endEvent(s32 zoneID, const char* pName, bool resetView, s32 frame) {
#ifdef TARGET_PC
    port_hrw_camera_cut();
#endif
    if (getCurrentCameraMan() == mCameraManEvent) {
        mCameraManEvent->end(zoneID, pName, frame);

        if (!mCameraManEvent->isActive()) {
            pop();

            if (!isForceCameraChange() && resetView && getCurrentCameraMan() == mCameraManGame) {
                mCameraManGame->mPoseParam->copyFrom(*mPoseParam1);
                mCameraManGame->mMatrix.set(MR::getCameraInvViewMtx());
            }

            mViewInterpolator->mIsInterpolationOff = true;
        }
    }
}

void CameraDirector::endEventAtLanding(s32 zoneID, const char* pName, s32 frame) {
#ifdef TARGET_PC
    port_hrw_camera_cut();
#endif
    if (getCurrentCameraMan() == mCameraManEvent) {
        mEvents[mEventNum].mZoneID = zoneID;
        strcpy(mEvents[mEventNum].mName, pName);
        mEvents[mEventNum].mFrame = frame;
        mEventNum++;
    }
}

CameraParamChunkEvent* CameraDirector::getEventParameter(s32 zoneID, const char* pName) {
    CameraParamChunkID_Tmp chunkID = CameraParamChunkID_Tmp();
    chunkID.createEventID(zoneID, pName);

    return reinterpret_cast< CameraParamChunkEvent* >(mChunkHolder->getChunk(chunkID));
}

void CameraDirector::requestToResetCameraMan() {
    mRequestCameraManReset = true;
}

void CameraDirector::setInterpolation(u32 time) {
    mViewInterpolator->setInterpolation(time);

    if (time == 0 && !mIsStartCameraActive) {
        mViewInterpolator->mIsCollisionOff = true;
        mCover->cover(2);
    }
}

void CameraDirector::cover(u32 time) {
    mCover->cover(time);
}

void CameraDirector::closeCreatingCameraChunk() {
    mCameraCreator->scanArea();
    mCameraCreator->scanStartPos();
    createStartAnimCamera();
    createTalkCamera();
    createSubjectiveCamera();
    if (mp_active()) {   // MP3 spectate: single player declares nothing
        const s32 type = mHolder->getIndexOf("CAM_TYPE_MP_WATCH");
        if (type >= 0) {
            declareEvent(0, ::sMpWatchCameraName);
            CameraParamChunkEvent* chunk = getEventParameter(0, ::sMpWatchCameraName);
            if (chunk != nullptr) {
                chunk->setCameraType("CAM_TYPE_MP_WATCH", mHolder);
                chunk->_64 = true;
            }
        }
    }
    mCameraManGame->closeCreatingCameraChunk();
    mChunkHolder->loadCameraParameters();
    mChunkHolder->sort();
    mIsStartCameraActive = true;
    mCameraManGame->startStartPosCamera(false);
}

void CameraDirector::initCameraCodeCollection(const char* a1, s32 a2) {
    mCameraCreator->initCameraCodeCollection(a1, a2);
}

void CameraDirector::registerCameraCode(u32 code) {
    mCameraCreator->registerCameraCode(code);
}

void CameraDirector::termCameraCodeCollection() {
    mCameraCreator->termCameraCodeCollection();
}

void CameraDirector::declareEvent(s32 zoneID, const char* pName) {
    CameraParamChunkID_Tmp chunkID = CameraParamChunkID_Tmp();
    chunkID.createEventID(zoneID, pName);

    mChunkHolder->createChunk(chunkID, nullptr);
}

void CameraDirector::started() {
    mIsStartCameraActive = false;
    mCameraManGame->endStartPosCamera();
}

void CameraDirector::setTargetActor(const LiveActor* pActor) {
    mTargetHolder->set(pActor);
}

void CameraDirector::setTargetPlayer(const MarioActor* pActor) {
    mTargetHolder->set(pActor);
}

bool CameraDirector::isRotatingHard() const {
    return mRotChecker->mIsRotatingHard;
}

bool CameraDirector::isSubjectiveCamera() const {
    return mIsSubjectiveCamera;
}

bool CameraDirector::isEnableToControl() const {
    bool enable = false;

    if (!getCurrentCameraMan()->isSubjectiveCameraOff()) {
        bool change = true;
        bool equals = getCurrentCameraMan() == mCameraManEvent;

        if (equals && !isEventCameraActive(0, ::sSubjectiveCameraName)) {
            change = false;
        }

        if (change) {
            enable = true;
        }
    }

    return enable;
}

bool CameraDirector::isEnableToRoundLeft() const {
    return getCurrentCameraMan()->isEnableToRoundLeft();
}

bool CameraDirector::isEnableToRoundRight() const {
    return getCurrentCameraMan()->isEnableToRoundRight();
}

bool CameraDirector::isEnableToReset() const {
    return getCurrentCameraMan()->isEnableToReset();
}

bool CameraDirector::isEventCameraActive(s32 zoneID, const char* pName) const {
    if (getCurrentCameraMan() == mCameraManEvent) {
        return mCameraManEvent->isEventActive(zoneID, pName);
    }

    return false;
}

bool CameraDirector::isEventCameraActive() const {
    return getCurrentCameraMan() == mCameraManEvent;
}

void CameraDirector::startStartPosCamera(bool interpolate) {
    mIsStartCameraActive = true;
    mCameraManGame->startStartPosCamera(interpolate);
}

bool CameraDirector::isInterpolatingNearlyEnd() const {
    if (getCurrentCameraMan() == mCameraManEvent && mCameraManEvent->doesNextChunkHaveInterpolation()) {
        return false;
    }

    return mViewInterpolator->isInterpolatingNearlyEnd();
}

bool CameraDirector::isForceCameraChange() const {
    return mViewInterpolator->mIsForceCameraChange;
}

f32 CameraDirector::getDefaultFovy() const {
    return ::sDefaultFovy;
}

void CameraDirector::startStartAnimCamera() {
    if (mStartCameraCreated) {
        ActorCameraInfo info = ActorCameraInfo();

        MR::startEventCamera(&info, ::sStartAnimCameraName, CameraTargetArg(mCameraTargetMtx), 0);
    }
}

bool CameraDirector::isStartAnimCameraEnd() const {
    if (mStartCameraCreated) {
        return isAnimCameraEnd(0, ::sStartAnimCameraName);
    }

    return true;
}

u32 CameraDirector::getStartAnimCameraFrame() const {
    if (mStartCameraCreated) {
        return getAnimCameraFrame(0, ::sStartAnimCameraName);
    }

    return 0;
}

void CameraDirector::endStartAnimCamera() {
    ActorCameraInfo info = ActorCameraInfo();
    MR::endEventCamera(&info, ::sStartAnimCameraName, true, 0);
}

void CameraDirector::startTalkCamera(const TVec3f& rPosition, const TVec3f& rUp, f32 axisX, f32 axisY, s32 frame) {
    CameraParamChunkEvent* chunk = getEventParameter(0, ::sTalkCameraName);

    if (chunk != nullptr) {
        chunk->mGeneralParam->mWPoint.set(rPosition);
        chunk->mGeneralParam->mUp.set(rUp);

        CameraGeneralParam* generalParam = chunk->mGeneralParam;
        generalParam->mAxis.x = axisX;
        generalParam->mAxis.y = axisY;
        generalParam->mAxis.z = 0.0f;

        CameraTargetArg targetArg;
        MR::setCameraTargetToPlayer(&targetArg);
        startEvent(0, ::sTalkCameraName, targetArg, frame);
    }
}

void CameraDirector::endTalkCamera(bool resetView, s32 frame) {
    endEvent(0, ::sTalkCameraName, resetView, frame);
}

void CameraDirector::startSubjectiveCamera(s32 camType) {
    mIsStartCameraActive = false;
    mCameraManGame->endStartPosCamera();
    mIsSubjectiveCamera = true;

    if (!mIsStartSubjectiveCamera) {
        mIsStartSubjectiveCamera = true;
        mSubjectiveFrame = 0;

        mCameraManSubjective->activate(this);
        mNearZ = MR::getNearZ();

        MR::setNearZ(10.0f);
        MR::turnOffDOFInSubjective();
    }

    if (mSubjectiveFrame < ::sSubjectiveInterpolateFrame) {
        MR::startCenterScreenBlur(::sSubjectiveInterpolateFrame, 15.0f, 80, 5, 10);
    }
}

void CameraDirector::endSubjectiveCamera(s32 camType) {
    bool forceEnd = camType == 0 || camType == 1;  // type ?? or ForceClose

    if (mIsSubjectiveCamera == true) {
        mIsSubjectiveCamera = false;

        if (!forceEnd) {
            MR::startCenterScreenBlur(mSubjectiveFrame, 15.0f, 80, 5, 10);
        }
    }

    if (mIsStartSubjectiveCamera && forceEnd) {
        mSubjectiveFrame = 0;
    }
}

bool CameraDirector::isAnimCameraEnd(s32 zoneID, const char* pName) const {
    if (getCurrentCameraMan() == mCameraManEvent) {
        return mCameraManEvent->isAnimCameraEnd(zoneID, pName);
    }

    return true;
}

u32 CameraDirector::getAnimCameraFrame(s32 zoneID, const char* pName) const {
    if (getCurrentCameraMan() == mCameraManEvent) {
        return mCameraManEvent->getAnimCameraFrame(zoneID, pName);
    }

    return 0;
}

// getAnimCameraCurrentFrame

void CameraDirector::pauseOnAnimCamera(s32 zoneID, const char* pName) {
    if (getCurrentCameraMan() == mCameraManEvent) {
        mCameraManEvent->pauseOnAnimCamera(zoneID, pName);
    }
}

void CameraDirector::pauseOffAnimCamera(s32 zoneID, const char* pName) {
    if (getCurrentCameraMan() == mCameraManEvent) {
        mCameraManEvent->pauseOffAnimCamera(zoneID, pName);
    }
}

void CameraDirector::zoomInGameCamera() {
    mCameraManGame->zoomIn();
}

void CameraDirector::zoomOutGameCamera() {
    mCameraManGame->zoomOut();
}

void CameraDirector::checkEndOfEventCamera() {
    if (mEventNum != 0 && mTargetHolder->isOnGround()) {
        for (u32 i = 0; i < mEventNum; i++) {
            endEvent(mEvents[i].mZoneID, mEvents[i].mName, true, mEvents[i].mFrame);
        }

        mEventNum = 0;
    }
}

void CameraDirector::createTalkCamera() {
    declareEvent(0, ::sTalkCameraName);

    CameraParamChunkEvent* chunk = getEventParameter(0, ::sTalkCameraName);
    if (chunk != nullptr) {
        chunk->setCameraType("CAM_TYPE_TALK", mHolder);
        chunk->_64 = true;
    }
}

void CameraDirector::controlCameraSE() {
    mIsCameraNG = false;

    if (MR::isPlayerDead()) {
        return;
    }

    if (mp_active() && MpSpectate::watching()) {   // MP3 spectate: L / R / the pad switch the watched player, nothing is "not possible"
        return;
    }

    if (mIsSubjectiveCamera) {
        if (MR::testCorePadTriggerLeft(WPAD_CHAN0) || MR::testCorePadTriggerRight(WPAD_CHAN0) || MR::testFpViewStartTrigger()) {
            if (isPlayableCameraSE(false)) {
                MR::startSystemSE("SE_SY_CAMERA_NG");
                mIsCameraNG = true;
            }
        }

        return;
    }

    if (CameraLocalUtil::testCameraPadTriggerRoundLeft()) {
        if (isEnableToRoundLeft()) {
            getCurrentCameraMan()->roundLeft();
            if (isPlayableCameraSE(false)) {
                MR::startSystemSE("SE_SY_CAMERA_MOVE");
            }
        } else {
            if (isPlayableCameraSE(false)) {
                MR::startSystemSE("SE_SY_CAMERA_NG");
                mIsCameraNG = true;
            }
        }
    }

    if (CameraLocalUtil::testCameraPadTriggerRoundRight()) {
        if (isEnableToRoundRight()) {
            getCurrentCameraMan()->roundRight();
            if (isPlayableCameraSE(false)) {
                MR::startSystemSE("SE_SY_CAMERA_MOVE");
            }
        } else {
            if (isPlayableCameraSE(false)) {
                MR::startSystemSE("SE_SY_CAMERA_NG");
                mIsCameraNG = true;
            }
        }
    }

    if (CameraLocalUtil::testCameraPadTriggerReset()) {
#ifdef TARGET_PC
        const bool canReset = isEnableToReset() || isFreeCamResetAvailable(this);
#else
        const bool canReset = isEnableToReset();
#endif
        if (canReset) {
            if (isPlayableCameraSE(false)) {
                MR::startSystemSE("SE_SY_CAMERA_RESET");
                MR::startSystemSE("SE_SY_CAMERA_MOVE");
            }
        } else {
            if (isPlayableCameraSE(false)) {
                MR::startSystemSE("SE_SY_CAMERA_NG");
                mIsCameraNG = true;
            }
        }
    }

    if (MR::isPlayerInBind()) {
        if (MR::testFpViewStartTrigger()) {
            if (isPlayableCameraSE(false)) {
                MR::startSystemSE("SE_SY_CAMERA_NG");
                mIsCameraNG = true;
            }
        }
    }

    // MP3: in a session the D-pad down is the greeting (MpCoop), not a "can't" beep
    if (MR::testCorePadTriggerDown(WPAD_CHAN0) && !mp_active()) {
        if (isPlayableCameraSE(false)) {
            MR::startSystemSE("SE_SY_CAMERA_NG");
            mIsCameraNG = true;
        }
    }
}

void CameraDirector::removeEndEventAtLanding(s32 zoneID, const char* pName) {
    if (mEventNum == 0) {
        return;
    }

    for (s32 idx = 0; idx < mEventNum; idx++) {
        if (mEvents[idx].mZoneID == zoneID && strcmp(mEvents[idx].mName, pName) == 0) {
            if (mEventNum - 1 == idx) {
                mEventNum = 0;
                return;
            }

            mEvents[idx].mZoneID = mEvents[mEventNum - 1].mZoneID;
            strcpy(mEvents[idx].mName, mEvents[mEventNum - 1].mName);
            mEvents[idx].mFrame = mEvents[mEventNum - 1].mFrame;
            mEventNum--;
            return;
        }
    }
}

void CameraDirector::calcViewMtxFromPoseParam(TPos3f* pMtx, const CameraPoseParam* pParam) {
    TVec3f front = pParam->mWatchPos - pParam->mPos;
    MR::normalizeOrZero(&front);
    TVec3f side = pParam->mUpVec.cross(front);
    MR::normalizeOrZero(&side);
    TVec3f up = front.cross(side);
    MR::normalizeOrZero(&up);

    pMtx->setXDir(-side);
    pMtx->setYDir(up);
    pMtx->setZDir(-front);
    pMtx->setTrans(pParam->mPos);

    TPos3f rot;
    rot.makeRotate(TVec3f(0.0f, 0.0f, 1.0f), pParam->mRoll);
    pMtx->concat(*pMtx, rot);
}

bool CameraDirector::isPlayableCameraSE(bool checkSubjective) {
    if (MR::isDemoActive()) {
        return false;
    }

    if (MR::isPowerStarGetDemoActive()) {
        return false;
    }

    if (MR::isStageStateScenarioOpeningCamera()) {
        return false;
    }

    if (MR::isExecScenarioStarter()) {
        return false;
    }

    if (MR::isPlayerDead()) {
        return false;
    }

    if (MR::isEqualStageName("FileSelect")) {
        return false;
    }

    if (MR::isEqualStageName("EpilogueDemoStage")) {
        return false;
    }

    if (checkSubjective && mIsSubjectiveCamera) {
        return false;
    }

    return true;
}

void CameraDirector::resetCameraMan() {
#ifdef TARGET_PC
    port_hrw_camera_cut();
    sFreeCam.pendingHardCut = true;
#endif
    setInterpolation(0);

    CameraTargetObj* target1;
    CameraTargetObj* target2;
    CameraTargetObj* target3;
    CameraMan* man = getCurrentCameraMan();

    target1 = mTargetHolder->get();
    target2 = mTargetHolder->get();
    target3 = mTargetHolder->get();

    TVec3f newPos = target3->getPosition() - target2->getFrontVec() * 800.0f + target1->getUpVec() * 300.0f;

    CameraLocalUtil::setPos(man, newPos);
    CameraLocalUtil::setWatchPos(man, mTargetHolder->get()->getPosition());
    CameraLocalUtil::setUpVec(man, mTargetHolder->get()->getUpVec());
    CameraLocalUtil::setWatchUpVec(man, mTargetHolder->get()->getUpVec());

    man->deactivate(this);
    man->activate(this);

    mOnlyCamera->mIsResetting = true;
}

void CameraDirector::createStartAnimCamera() {
    void* data = nullptr;
    s32 size = 0;
    MR::getCurrentScenarioStartAnimCameraData(&data, &size);

    if (size > 0) {
        ActorCameraInfo info = ActorCameraInfo();
        MR::declareEventCameraAnim(&info, ::sStartAnimCameraName, data);
        mStartCameraCreated = true;
    }
}

void CameraDirector::createSubjectiveCamera() {
    declareEvent(0, ::sSubjectiveCameraName);

    CameraParamChunkEvent* chunk = getEventParameter(0, ::sSubjectiveCameraName);
    if (chunk != nullptr) {
        chunk->setCameraType("CAM_TYPE_SUBJECTIVE", mHolder);
        chunk->_64 = true;
    }
}

CameraDirector* MR::getCameraDirector() {
    return MR::getSceneObj< CameraDirector >(SceneObj_CameraDirector);
}
