#include "Game/MP/MpSpectate.hpp"   // MP3 spectate
#include "port/mp_bridge.h"
#include "Game/LiveActor/LodCtrl.hpp"
#include "Game/LiveActor/ActorLightCtrl.hpp"
#include "Game/LiveActor/ClippingDirector.hpp"
#include "Game/LiveActor/ClippingJudge.hpp"
#include "Game/LiveActor/ModelObj.hpp"
#include "Game/Player/MarioHolder.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/ActorMovementUtil.hpp"
#include "Game/Util/ActorShadowUtil.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/JointUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/SequenceUtil.hpp"
#include <cstdio>

#ifdef TARGET_PC
#include "Game/Util/PlayerUtil.hpp"
#include "port/diagnostics.h"
#include "port/hot_knob.h"
#include <cstdlib>
#include <cmath>
extern "C" float port_pb_lod_scale(void);
#include "port/popin.h"
#include "port/popin_site.h"
#include "port/detail.h"
#include "Game/Util/ScreenUtil.hpp"
extern "C" float port_popin_zoom(void);  // (popin24, GXV_POPIN_ZOOM: this turn's zoom ratio, >= 1; 1 when off or not zoomed)
extern "C" void port_popin_event(int kind, int appear, const LiveActor* pActor, const TVec3f* pPos, float radius);  // (GXV_POPIN_LOG)
// (popin25, GXV_PB_DEMO / GXV_POPIN_LEAD, defined below with the census)
extern "C" float port_pb_lod_flight(float pbScale);
extern "C" float port_pb_scale(void);
extern "C" int port_pb_demo_off(void);
extern "C" float port_pb_lead(float limit);
extern "C" float port_pb_lod_distance(float distPlayer, const TVec3f* pPos);
namespace {
inline f32 PbLodDist(f32 d, f32 scale) {
    const f32 scaled = d * scale;
    const f32 floorD = d < 1000.0f ? d : 1000.0f;
    return scaled > floorD ? scaled : floorD;
}
}  // namespace
#endif

#ifdef TARGET_PC
// ---- GXV_CAPREAD (water23 round 2, diagnostic; default 0): which LOD of a model with "IndDummy" (screen-capture) materials is shown
// The console's capread lines show J3D draws that sample the "Indirect" screen capture BEFORE the picture's own capture (they read the
// previous picture's): the opaque and the planets' LOD buffers (Planet, PlanetLow, MapObj) are drawn before DrawType_CaptureScreenIndirect,
// the Indirect sub-models after it. Such a draw sees its own output of the last picture (feedback). This names the LOD controllers whose
// models have such materials, the model each one shows, the distance, and the view group's forcing (ViewGroupCtrlCube: arg1 1 = High,
// 2 = Middle, 3 = Low, 4 = hidden): "capread-lod:" basic-log lines at the first update and at every change of the shown model.
namespace {
int PortCapReadOn() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_CAPREAD");
        return e && *e && *e != '0' ? 1 : 0;
    }());
}
bool PortHasIndDummy(const LiveActor* pActor) {
    return pActor != nullptr && MR::getJ3DModelData(pActor) != nullptr && MR::isExistIndirectTexture(pActor);
}
const char* PortLodName(const LodCtrl* pCtrl, const LiveActor* pShown) {
    if (pShown == nullptr) {
        return "none";
    }
    if (pShown == pCtrl->mActor) {
        return "High";
    }
    if (pShown == reinterpret_cast< const LiveActor* >(pCtrl->_10)) {
        return "Middle";
    }
    if (pShown == reinterpret_cast< const LiveActor* >(pCtrl->_14)) {
        return "Low";
    }
    return "other";
}
}  // namespace
#endif

namespace {
    const f32 sDefaultDistanceToMiddle = 2000.0f;
    const f32 sDefaultDistanceToLow = 3000.0f;

};  // namespace

extern const bool def;

LodCtrl::LodCtrl(LiveActor* pActor, const JMapInfoIter& rIter) {
    _0 = ::sDefaultDistanceToMiddle;
    _4 = ::sDefaultDistanceToLow;
    _8 = pActor;
    mActor = pActor;
    _10 = nullptr;
    _14 = nullptr;
    _18 = 0;
    _19 = 0;
    _1A = 1;
    _1B = 0;
    mViewGroupID = -1;
    mActorLightCtrl = nullptr;
    _1C = &def;
    _20 = &def;
    _24 = &def;
    _28 = &def;
    MR::getClippingDirector()->entryLodCtrl(this, rIter);
    mActorLightCtrl = mActor->mActorLightCtrl;
}

void LodCtrl::offSyncShadowHost() {
    MR::offShadowVisibleSyncHostAll(mActor);
    _1A = 0;
}

void LodCtrl::appear() {
    MR::showModel(mActor);
    mActorLightCtrl = mActor->mActorLightCtrl;

    if (_10) {
        if (!MR::isDead(_10)) {
            _10->makeActorDead();
        }
    }

    if (_14) {
        if (!MR::isDead(_14)) {
            _14->makeActorDead();
        }
    }

    if (!_1A) {
        MR::offShadowVisibleSyncHostAll(mActor);
    }
}

void LodCtrl::kill() {
    MR::showModel(mActor);

    if (_10) {
        if (!MR::isDead(_10)) {
            _10->makeActorDead();
        }
    }

    if (_14) {
        if (!MR::isDead(_14)) {
            _14->makeActorDead();
        }
    }

    if (!_1A) {
        MR::onShadowVisibleSyncHostAll(mActor);
    }

    mActorLightCtrl = 0;
}

void LodCtrl::validate() {
    appear();
    _18 = 1;
}

void LodCtrl::invalidate() {
    kill();
    _18 = 0;
}

void LodCtrl::update() {
#ifdef TARGET_PC
    const LiveActor* const portPrev = _8;
#endif
    if (!MR::isDead(mActor) && _18) {
        bool hasLod = _10 || _14;
        if (!hasLod) {
            if (*_28) {
                hideAllModel();
            } else {
                showHighModel();
            }

            return;
        }

        f32 distance = calcDistanceToCamera();
#ifdef TARGET_PC
        const f32 distancePlayer = distance;  // (popin25: the game's own measure, for the census's "was it the game's swap")
        if (!_1B) {
            distance = port_pb_lod_distance(distance, &mActor->mPosition);  // (GXV_PB_DEMO: in a flight from the nearer of the player and the camera)
        }
#endif
        f32 lowDistance = _4;
        f32 middleDistance = _0;
#ifdef TARGET_PC
        const f32 popinOrigLow = lowDistance, popinOrigMiddle = middleDistance;  // (the game's own: GXV_POPIN_LOG tells port-made swaps from its own)
        f32 hystPct;
        {
            const f32 pbScale = port_pb_lod_flight(port_pb_lod_scale());  // (GXV_PB_LOD: 1 = as the game; popin25: GXV_PB_DEMO takes it back in a flight)
            if (pbScale < 1.0f) {
                lowDistance = PbLodDist(lowDistance, pbScale);
                middleDistance = PbLodDist(middleDistance, pbScale);
            }
            // GXV_POPIN_ZOOM: a model at a distance is drawn zoom times larger under the handheld zoom; the distances (the port's earlier
            // ones with 1, the game's own too with 2) are seen at the size they were at zoom 1.0.
            const f32 zoom = port_popin_zoom();
            if (zoom > 1.0f && (pbScale < 1.0f || port::popin::ZoomMode() >= 2)) {
                lowDistance *= zoom;
                middleDistance *= zoom;
            }
            hystPct = port::popin::HystPct();  // (GXV_POPIN_HYST: LodLevel)
        }
#endif
        if (*_28) {
            hideAllModel();
        } else if (*_1C) {
            showHighModel();
        } else if (_10 && *_20) {
            showMiddleModel();
        } else if (_14 && *_24) {
            showLowModel();
        } else
#ifdef TARGET_PC
        {
            const int shown = (_14 != nullptr && _8 == _14) ? 2 : ((_10 != nullptr && _8 == _10) ? 1 : 0);
            int portLevel = port::popin::LodLevel(distance, middleDistance, lowDistance, _10 != nullptr, _14 != nullptr, shown, hystPct);
            if (__builtin_expect(port::popin::GuardOn(port::popin::kFlLod), 0) && portLevel >= 0) {
                // popin31: GXV_PB_MINPX: a swap EARLIER than the game's (GXV_PB_LOD) is taken only while the model is under the floor on the picture; else the
                // game's own distances decide. GXV_PB_DEMO=2's release: a swap to a coarser model does not happen in the picture after a flight.
                PortLodGuard(portLevel, shown, distance, middleDistance, lowDistance, popinOrigMiddle, popinOrigLow, hystPct);
            }
            switch (portLevel) {
            case 0:
                showHighModel();
                break;
            case 1:
                showMiddleModel();
                break;
            case 2:
                showLowModel();
                break;
            default:
                break;
            }
        }
#else
        if (distance < middleDistance) {
            showHighModel();
        } else if (!_10 && distance < lowDistance) {
            showHighModel();
        } else if (_10 && distance < lowDistance) {
            showMiddleModel();
        } else if (_14) {
            showLowModel();
        }
#endif
#ifdef TARGET_PC
        if (_8 != portPrev && port::popin::LogOn()) {
            // GXV_POPIN_LOG: the swap, and what the game's own distances would show now (agree: its own swap; differ: the port made it)
            const int prevLevel = (_14 != nullptr && portPrev == _14) ? 2 : ((_10 != nullptr && portPrev == _10) ? 1 : 0);
            const int level = (_14 != nullptr && _8 == _14) ? 2 : ((_10 != nullptr && _8 == _10) ? 1 : 0);
            int origLevel = level;
            if (!*_28 && !*_1C && !(_10 && *_20) && !(_14 && *_24)) {
                const int lv = port::popin::LodLevel(distancePlayer, popinOrigMiddle, popinOrigLow, _10 != nullptr, _14 != nullptr, 0, 0.0f);
                origLevel = lv >= 0 ? lv : level;
            }
            port_popin_event(origLevel == level ? port::popin::kLodOrig : port::popin::kLodPort, level < prevLevel ? 1 : 0, mActor,
                             &mActor->mPosition, -1.0f);
        }
#endif

        if (_8 && _8 != mActor) {
            MR::copyTransRotateScale(mActor, _8);
        }
#ifdef TARGET_PC
        if (PortCapReadOn()) {
            if (mPortCapLog == 0) {
                mPortCapLog = (PortHasIndDummy(mActor) || PortHasIndDummy(_10) || PortHasIndDummy(_14)) ? 2 : 1;
            }
            if (mPortCapLog >= 2 && (_8 != portPrev || mPortCapLog == 2)) {
                mPortCapLog = 3;  // (the first line, then only changes)
                port_log_summary("capread-lod: %s shows %s (was %s) at %.0f (Middle from %.0f, Low from %.0f); view group %d %s forced "
                                 "High %d Middle %d Low %d hidden %d; IndDummy materials in High %d Middle %d Low %d (Middle / Low are drawn "
                                 "before the Indirect capture)",
                                 mActor->mName != nullptr ? mActor->mName : "?", PortLodName(this, _8), PortLodName(this, portPrev), distance,
                                 middleDistance, lowDistance, static_cast< int >(mViewGroupID), _1C == &def ? "(not registered)" : "", *_1C ? 1 : 0,
                                 *_20 ? 1 : 0, *_24 ? 1 : 0, *_28 ? 1 : 0, PortHasIndDummy(mActor) ? 1 : 0, PortHasIndDummy(_10) ? 1 : 0,
                                 PortHasIndDummy(_14) ? 1 : 0);
            }
        }
#endif
    }
}

#ifdef TARGET_PC
// popin31 (port/popin_site.h): the floor and the release for one LodCtrl's chosen level (only with GXV_PB_MINPX or GXV_PB_DEMO=2's release in force)
void LodCtrl::PortLodGuard(int& level, int shown, f32 distance, f32 middle, f32 low, f32 origMiddle, f32 origLow, f32 hystPct) {
    const bool hasM = _10 != nullptr, hasL = _14 != nullptr;
    const TVec3f& p = mActor->mPosition;
    f32 radius = -1.0f, dist = 0.0f;
    const auto size = [&]() {
        if (radius < 0.0f) {
            const f32 ext = port::popin::CachedExtent(mPbFloorExt, mPbFloorAt, mActor);
            radius = port::popin::SizeRadius(ext > 0.0f ? 0.0f : 300.0f, ext, 0.0f);
            const f32 dx = p.x - port_pb_floor.cam[0], dy = p.y - port_pb_floor.cam[1], dz = p.z - port_pb_floor.cam[2];
            dist = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
        }
    };
    if (port_pb_floor.kHeldK[port::popin::kFlLod] > 0.0f) {   // (popin31b: the LOD swap's own floor, GXV_PB_MINPX_LOD)
        // the level the game's own distances give (never nearer than the port's: the port's may be wider under the zoom)
        const int gameLevel = port::popin::LodLevel(distance, middle > origMiddle ? middle : origMiddle, low > origLow ? low : origLow, hasM, hasL, shown, hystPct);
        if (gameLevel >= 0 && level > gameLevel) {
            size();
            if (!port::popin::FloorMayCutK(port_pb_floor, port::popin::kFlLod, radius, dist, shown >= level)) {
                if (port::popin::LogOn()) port_pb_cut_kept(port::popin::kLodPort, 0);
                level = gameLevel;
            }
        }
    }
    if (port_pb_floor.releasing && level > shown) {
        size();
        if (port_pb_in_view(p.x, p.y, p.z, radius)) {
            port_pb_cut_kept(port::popin::kLodPort, 1);
            level = shown;
        }
    }
}
#endif

bool LodCtrl::isShowLowModel() const {
    return _14 != nullptr && _14 == _8;
}

void LodCtrl::setDistanceToMiddle(f32 dist) {
    _0 = dist;
}

f32 LodCtrl::calcDistanceToCamera() const {
    if (_1B) {
        return MR::calcCameraDistanceZ(mActor->mPosition);
    }

    // MP3 spectate: while a waiting player's view follows a teammate, the planets show the near models around HIM
    f32 mpDistance;
    // (optfix: per actor per frame: the inline mp_entered() bool first, so single player pays no call; mp_active() implies mp_entered())
    if (mp_entered() && mp_active() && MpSpectate::lodDistance(mActor->mPosition.x, mActor->mPosition.y, mActor->mPosition.z, &mpDistance)) {
        return mpDistance;
    }

    return MR::calcDistanceToPlayer(mActor);
}

void LodCtrl::setDistanceToLow(f32 dist) {
    _4 = dist;
}

void LodCtrl::setDistanceToMiddleAndLow(f32 mid_dist, f32 low_dist) {
    _0 = mid_dist;
    _4 = low_dist;
}

namespace {
    void LodFuntionCall(LodCtrl* pCtrl, void (*pFunc)(LiveActor*)) NO_INLINE {
        pFunc(pCtrl->mActor);

        if (pCtrl->_10) {
            pFunc(pCtrl->_10);
        }

        if (pCtrl->_14) {
            pFunc(pCtrl->_14);
        }
    }

    template < typename T >
    void LodFuntionCall(LodCtrl* pCtrl, void (*pFunc)(LiveActor*, T), T arg) NO_INLINE {
        pFunc(pCtrl->mActor, arg);

        if (pCtrl->_10) {
            pFunc(pCtrl->_10, arg);
        }

        if (pCtrl->_14) {
            pFunc(pCtrl->_14, arg);
        }
    }

    template void LodFuntionCall< f32 >(LodCtrl*, void (*)(LiveActor*, f32), f32);
}  // namespace

void LodCtrl::setClippingTypeSphereContainsModelBoundingBox(f32 bounds) {
    ::LodFuntionCall< f32 >(this, MR::setClippingTypeSphereContainsModelBoundingBox, bounds);
}

void LodCtrl::setFarClipping(f32 clip) {
    ::LodFuntionCall< f32 >(this, MR::setClippingFar, clip);
}

void LodCtrl::invalidateClipping() {
    ::LodFuntionCall(this, MR::invalidateClipping);
}

void LodCtrl::showHighModel() {
    if (MR::isHiddenModel(mActor)) {
        if (mActorLightCtrl) {
            mActor->mActorLightCtrl->copy(mActorLightCtrl);
        }

        mActorLightCtrl = mActor->mActorLightCtrl;
        MR::showModel(mActor);
    } else {
        if (_10) {
            if (!MR::isDead(_10)) {
                _10->makeActorDead();
            }
        }

        if (_14) {
            if (!MR::isDead(_14)) {
                _14->makeActorDead();
            }
        }
    }

    _8 = mActor;
}

void LodCtrl::showMiddleModel() {
    if (MR::isDead(_10)) {
        if (mActorLightCtrl) {
            _10->mActorLightCtrl->copy(mActorLightCtrl);
        }

        mActorLightCtrl = _10->mActorLightCtrl;
        _10->makeActorAppeared();
        MR::calcAnimDirect(mActor);
    } else {
        if (!MR::isHiddenModel(mActor)) {
            MR::hideModelAndOnCalcAnim(mActor);
        }

        if (_14) {
            if (!MR::isDead(_14)) {
                _14->makeActorDead();
            }
        }
    }

    _8 = _10;
}

void LodCtrl::showLowModel() {
    if (MR::isDead(_14)) {
        if (mActorLightCtrl) {
            _14->mActorLightCtrl->copy(mActorLightCtrl);
        }

        mActorLightCtrl = _14->mActorLightCtrl;
        _14->makeActorAppeared();
        MR::calcAnimDirect(mActor);
    } else {
        if (!MR::isHiddenModel(mActor)) {
            MR::hideModelAndOnCalcAnim(mActor);
        }

        if (_10 && !MR::isDead(_10)) {
            _10->makeActorDead();
        }
    }

    _8 = _14;
}

void LodCtrl::hideAllModel() {
    if (!MR::isHiddenModel(mActor)) {
        MR::hideModelAndOnCalcAnim(mActor);
    }

    if (_10) {
        if (!MR::isDead(_10)) {
            _10->makeActorDead();
        }
    }

    if (_14) {
        if (!MR::isDead(_14)) {
            _14->makeActorDead();
        }
    }

    _8 = 0;
}

void LodCtrl::setViewCtrlPtr(const bool* pHigh, const bool* pMiddle, const bool* pLow, const bool* pHidden) {
    _1C = pHigh;
    _20 = pMiddle;
    _24 = pLow;
    _28 = pHidden;
}

void LodCtrl::createLodModel(int drawBufferType, int movementType, int calcAnimType) {
    _10 = initLodModel(drawBufferType, movementType, calcAnimType, false);
    _14 = initLodModel(drawBufferType, movementType, calcAnimType, true);

    if (_10 || _14) {
        appear();
        _18 = 1;
    } else {
        kill();
        _18 = 0;
    }
}

void LodCtrl::syncMaterialAnimation() {
    if (_10) {
        MR::syncMaterialAnimation(_10, mActor);
    }

    if (_14) {
        MR::syncMaterialAnimation(_14, mActor);
    }
}

void LodCtrl::syncJointAnimation() {
    if (_10) {
        MR::syncJointAnimation(_10, mActor);
    }

    if (_14) {
        MR::syncJointAnimation(_14, mActor);
    }
}

void LodCtrl::initLightCtrl() {
    if (_10) {
        MR::initLightCtrl(_10);
    }

    if (_14) {
        MR::initLightCtrl(_14);
    }
}

ModelObj* LodCtrl::initLodModel(int drawBufferType, int movementType, int calcAnimType, bool isLowModel) const {
    const char* res = MR::getModelResName(mActor);
    const char* type = isLowModel ? "Low" : "Middle";
    char buf[0x100];
    snprintf(buf, sizeof(buf), "/ObjectData/%s%s.arc", res, type);

    if (!MR::isFileExist(buf, false)) {
        return nullptr;
    }

    const char* objName = isLowModel ? MR::createLowModelObjName(mActor) : MR::createMiddleModelObjName(mActor);
    snprintf(buf, sizeof(buf), "%s%s", res, type);
    ModelObj* obj = new ModelObj(objName, buf, mActor->getBaseMtx(), drawBufferType, movementType, calcAnimType, false);
    obj->initWithoutIter();
    obj->makeActorDead();
    MR::setClippingTypeSphereContainsModelBoundingBox(obj, 100.0f);
    MR::copyTransRotateScale(mActor, obj);
    return obj;
}

bool LodCtrlFunction::isExistLodLowModel(const char* pName) {
    char buf[0x100];
    snprintf(buf, sizeof(buf), "/ObjectData/%sLow.arc", pName);
    return MR::isFileExist(buf, false);
}

const char* LodCtrl_FORCE_MATCH_STRING() {
    return "/ObjectData/%sMiddle.arc";
}

const bool def = false;

#ifdef TARGET_PC
// ---- Performance-mode visual budget (platform/src/port/perf.cpp, GXV_PERF_BUDGET; budget, 8 Nov) ----
// GXV_PB_LOD (0.6 in Performance mode): the distances at which the Middle and the Low model take over are scaled (a model that has them
// only; collision, sensors and logic belong to the actor and are not touched). A distance is never pulled in below 1000 units (or below
// itself, when the game set it lower), so what stands near Mario or the camera keeps its High model.
extern "C" float port_pb_ptcl_factor(void);
extern "C" float port_pb_ptcl_dist2(void);
extern "C" float port_pb_release_ptcl_w(void);   // (popin31, below)
extern "C" float port_pb_ptcl_floor_d2(void);
extern "C" void port_popin_ptcl_note(int what, unsigned n);
extern "C" float port_detail_dist(int rule, float base);
extern "C" int port_pb_budget_on(void);
extern "C" int64_t port_host_time_ns(void);
#include <atomic>
#include <utility>
#include "Game/LiveActor/ModelManager.hpp"

// (2.3, 8 Nov) The player's position, or nullptr where none exists yet (star select, file select, title, loading: the MarioHolder can
// exist with no MarioActor). MR::getPlayerPos() itself never returns null, it reads through the actor; the port's own helpers ask here.
extern "C" const TVec3f* port_safe_player_pos(void) {
    if (!MR::isExistSceneObj(SceneObj_MarioHolder) || MR::getMarioHolder() == nullptr || MR::getMarioHolder()->getMarioActor() == nullptr) {
        return nullptr;
    }
    return MR::getPlayerPos();
}
extern "C" int port_safe_cam_exists(void) { return MR::isExistSceneObj(SceneObj_CameraContext) ? 1 : 0; }
// The squared distance from the camera / player (the callers' tests): helpers for the parts that live in other files.
extern "C" float port_pb_cam_dist2(float x, float y, float z) {
    // (2.3 test 10 crash, tester log 8 Nov: the star select (ScenarioSelectScene) runs particles before any camera exists; getCamPos()
    // read a null CameraContext. No camera known: treated as "near", nothing is thinned.)
    if (!MR::isExistSceneObj(SceneObj_CameraContext)) {
        return 0.0f;
    }
    const TVec3f cam = MR::getCamPos();
    const float dx = x - cam.x, dy = y - cam.y, dz = z - cam.z;
    return dx * dx + dy * dy + dz * dz;
}
extern "C" float port_pb_player_dist2(float x, float y, float z) {
    // (2.3 test 10 crash, tester dump 8 Nov: in the star select (ScenarioSelectScene) the MarioHolder exists but holds no MarioActor
    // yet, and MR::getPlayerPos() read through the null actor: JPADynamicsBlock::create -> here. getPlayerPos never returns null, so
    // the old check could not catch it. No player known: treated as "near", nothing is thinned or culled.)
    if (!MR::isExistSceneObj(SceneObj_MarioHolder) || MR::getMarioHolder() == nullptr || MR::getMarioHolder()->getMarioActor() == nullptr) {
        return 0.0f;
    }
    const TVec3f* p = MR::getPlayerPos();
    const float dx = x - p->x, dy = y - p->y, dz = z - p->z;
    return dx * dx + dy * dy + dz * dz;
}
// GXV_PB_PTCL: the factor an emitter's emission this turn is scaled by (1: as the game). JPADynamicsBlock::create asks it with the
// emitter's world position. Far from the camera AND from the player only; every emitter keeps emitting (a one-shot burst is thinned,
// not dropped), so no effect disappears.
// flow23 (8 Nov, the c10 star-select crash): the budget is the STAGE's. It is decided once per particle calc, on the game thread, at the
// start of JPAEmitterManager::calcNormal (port_pb_ptcl_turn): only for the stage's own effect system (the scene's ParticleCalcExecutor,
// the one with list adaptors), and only when the stage's camera and player both exist; their positions are taken then. Before, every
// emitter's create asked the scene objects itself, also for the star select's own effect system (ScenarioSelectScene: its emitters live
// in the star select's camera space, not the stage's) while the stage behind it was still being built on the loading thread, and from
// the GXV_PAR_PTCL worker on the fourth core. The calc moves neither the camera nor the player, so a stage's thinning is unchanged.
static float sPbPtclTurnFactor = 1.0f;  // (1: nothing thinned in this calc)
static TVec3f sPbPtclCam(0.0f, 0.0f, 0.0f);
static TVec3f sPbPtclPlayer(0.0f, 0.0f, 0.0f);
// GXV_SEAMLESS (seamless23): the stage camera's cuts, for the renderer's quality steps and the pacing's switch (a visible change is taken at a
// cut, where it is not seen: platform/vgx/src/vgx_backend_gl.cpp, platform/src/port/perf.cpp). A cut is a jump of the camera between two calcs
// of the stage's effects (once a game turn): its position moved more than GXV_SEAMLESS_CUT_DIST units (default 1000; Mario runs ~15-25 a turn,
// a launch star's flight ~100-200) or its view turned more than GXV_SEAMLESS_CUT_DEG degrees (default 35; the player's camera turns a few a
// turn). The count is read from other threads (relaxed). Only the game thread writes it; no camera, no stage: nothing is compared.
static uint32_t sCamCuts = 0;
static TVec3f sCutPos(0.0f, 0.0f, 0.0f), sCutDir(0.0f, 0.0f, 1.0f);
static bool sCutHave = false;
static void PortCameraCutTurn(int stage) {
    static bool configured = false;  // (game thread only: a plain flag, no guarded static)
    static float dist2 = 1000.0f * 1000.0f, minDot = 0.819f;
    if (!configured) {
        configured = true;
        const char* d = std::getenv("GXV_SEAMLESS_CUT_DIST");
        const char* a = std::getenv("GXV_SEAMLESS_CUT_DEG");
        if (d && *d && std::atof(d) > 0.0) dist2 = static_cast<float>(std::atof(d) * std::atof(d));
        if (a && *a && std::atof(a) > 0.0 && std::atof(a) < 180.0) minDot = static_cast<float>(std::cos(std::atof(a) * 3.14159265358979 / 180.0));
    }
    if (!stage) return; // (another effect system's calc: the star select's; the stage camera is not its)
    if (!port_safe_cam_exists()) {
        sCutHave = false;
        return;
    }
    const TVec3f pos = MR::getCamPos();
    const TVec3f dir = MR::getCamZdir();
    if (sCutHave) {
        const float dx = pos.x - sCutPos.x, dy = pos.y - sCutPos.y, dz = pos.z - sCutPos.z;
        const float dot = dir.x * sCutDir.x + dir.y * sCutDir.y + dir.z * sCutDir.z;
        const float len = std::sqrt((dir.x * dir.x + dir.y * dir.y + dir.z * dir.z) * (sCutDir.x * sCutDir.x + sCutDir.y * sCutDir.y + sCutDir.z * sCutDir.z));
        if (dx * dx + dy * dy + dz * dz > dist2 || (len > 0.0f && dot < minDot * len)) __atomic_add_fetch(&sCamCuts, 1u, __ATOMIC_RELAXED);
    }
    sCutPos = pos;
    sCutDir = dir;
    sCutHave = true;
}
extern "C" uint32_t port_camera_cut_count(void) { return __atomic_load_n(&sCamCuts, __ATOMIC_RELAXED); }
extern "C" void port_pb_ptcl_turn(int stage) {
    PortCameraCutTurn(stage);
    sPbPtclTurnFactor = 1.0f;
    float f = port_pb_ptcl_factor();
    if (!stage || f >= 1.0f || !port_safe_cam_exists() || port_pb_demo_off()) {  // (popin25: GXV_PB_DEMO=2: no thinning in a flight)
        return;
    }
    f = port::popin::RampedFactor(f, port_pb_release_ptcl_w());  // (popin31: GXV_PB_DEMO=2: after a flight the thinning comes back over a ramp; 1 = as before)
    if (!(f < 1.0f)) {
        return;
    }
    const TVec3f* p = port_safe_player_pos();
    if (p == nullptr) {
        return;  // (no player known: everything is "near", as before)
    }
    sPbPtclCam = MR::getCamPos();
    sPbPtclPlayer = *p;
    sPbPtclTurnFactor = f;
}
extern "C" float port_pb_ptcl_turn_factor(void) { return sPbPtclTurnFactor; }
extern "C" float port_pb_ptcl_scale_at(float x, float y, float z) {
    const float f = sPbPtclTurnFactor;
    if (f >= 1.0f) {
        return 1.0f;
    }
    const float zoom = port_pb_scale();  // (GXV_POPIN_ZOOM: 1 when off; popin25: times GXV_PB_DEMO's widening in a flight)
    const float hystPct = port::popin::HystPct();
    float dx = x - sPbPtclCam.x, dy = y - sPbPtclCam.y, dz = z - sPbPtclCam.z;
    const float camD2 = dx * dx + dy * dy + dz * dz;
    float camStart2 = port_pb_ptcl_dist2();
    {
        const float dScaled = port_detail_dist(4, std::sqrt(camStart2));  // (detail25: the level's camera distance; the player's 1500 is not scaled)
        camStart2 = dScaled * dScaled * zoom * zoom;
    }
    {
        const float lead = port_pb_lead(std::sqrt(camStart2));  // (popin25, GXV_POPIN_LEAD: a fast camera thins from farther; 0 = as before)
        if (lead > 0.0f) {
            camStart2 = (std::sqrt(camStart2) + lead) * (std::sqrt(camStart2) + lead);
        }
    }
    if (camD2 < camStart2) {
        return 1.0f;
    }
    if (__builtin_expect(port_pb_floor.kHeldK[port::popin::kFlParticle] > 0.0f, 0) && camD2 < port_pb_ptcl_floor_d2()) {
        if (port::popin::CensusOn()) port_popin_ptcl_note(3, 1);  // (popin31b: the particle floor kept an emitter: the census's price line)
        return 1.0f;  // (popin31, GXV_PB_MINPX: a particle of GXV_PB_MINPX_PTCL units would not be under the floor here; popin31b: GXV_PB_MINPX_PARTICLE)
    }
    const float pd = 1500.0f * zoom;
    dx = x - sPbPtclPlayer.x;
    dy = y - sPbPtclPlayer.y;
    dz = z - sPbPtclPlayer.z;
    const float plD2 = dx * dx + dy * dy + dz * dz;
    if (plD2 < pd * pd) {
        return 1.0f;
    }
    if (hystPct > 0.0f) {
        // GXV_POPIN_HYST: the thinning ramps in over 4 x popin-pct of each distance (the density of a far fountain does not step)
        const float camStart = std::sqrt(camStart2);
        const float a = port::popin::Ramp(std::sqrt(camD2), camStart, camStart * hystPct * 0.04f, f);
        const float b = port::popin::Ramp(std::sqrt(plD2), pd, pd * hystPct * 0.04f, f);
        const float r = a > b ? a : b;
        if (r < 1.0f && port::popin::CensusOn()) port_popin_ptcl_note(0, 1);
        return r;
    }
    if (port::popin::CensusOn()) port_popin_ptcl_note(0, 1);  // (popin31, the census: an emitter scaled down)
    return f;
}

// ---- popin24 (10 Nov): pop-in audit (platform/include/port/popin.h) ----
// The zoom ratio of this turn, set by port_popin_turn (once a turn, from ClippingActorHolder::movement, the game thread); the readers are
// the game thread's LodCtrl::update, the actor calcAnim, the shadow update and the fur draw.
static float sPopinZoom = 1.0f;
extern "C" float port_popin_zoom(void) { return sPopinZoom; }

// ---- popin25 (11 Nov): fly-overs ----
// What the camera and the game are doing this turn, set by port_popin_turn (game thread) and read by the budget's rules (the game thread; the
// particle calc's worker reads the two floats, a word each). With every popin25 switch off none of them differs from 1 / 0 / false.
static int sFlight = 0;           // port::popin::Flight: a demo, the opening camera ... (0: none); only classified with GXV_PB_DEMO / GXV_POPIN_CENSUS
static float sPbWiden = 1.0f;     // GXV_PB_DEMO=1 in a flight: the budget's distances are this many times wider
static bool sPbOff = false;       // GXV_PB_DEMO=2 in a flight: the budget is off this turn
static port::popin::CamTrack sCamTrack;  // the camera's speed in units a turn (GXV_POPIN_LEAD, the flight's "fast camera")
extern "C" int port_popin_flight(void) { return sFlight; }
// the distance scale of every budget rule: the handheld zoom (GXV_POPIN_ZOOM) times the flight's widening (GXV_PB_DEMO=1)
extern "C" float port_pb_scale(void) { return sPopinZoom * sPbWiden; }
extern "C" int port_pb_demo_off(void) { return sPbOff ? 1 : 0; }
// GXV_POPIN_LEAD: how far earlier than `limit` a rule appears something for the camera's speed (0 when off)
extern "C" float port_pb_lead(float limit) { return port::popin::LeadMargin(sCamTrack.speed, port::popin::LeadTurns(), limit); }
// GXV_POPIN_LEAD for a turning camera: the factor the clipping frustum's side tangents are multiplied by (ClippingJudge::calcViewingVolumeFovy; 1 = none)
extern "C" float port_popin_clip_pad(float fovyDeg) {
    const float p = port::popin::LeadFramePad(sCamTrack.turn, port::popin::LeadTurns(), fovyDeg, port::popin::LeadPadMax());
    return 1.0f + p;
}
// the same for the game's clipping spheres (ClippingActorHolder's loop, once a turn)
extern "C" float port_popin_clip_lead(void) { return port::popin::LeadClipMargin(sCamTrack.speed, port::popin::LeadTurns(), port::popin::LeadClipMax()); }
// LodCtrl: PB_LOD's factor in a flight (GXV_PB_DEMO: taken back toward 1; 2: off)
extern "C" float port_pb_lod_flight(float pbScale) {
    if (sFlight == 0 || port::popin::DemoMode() == 0) {
        return pbScale;
    }
    return port::popin::FlightLodScale(pbScale, port::popin::DemoWiden(), port::popin::DemoMode() >= 2);
}
// LodCtrl: the distance compared in a flight (GXV_PB_DEMO >= 1): the nearer of the player and the camera (depth along the view)
extern "C" float port_pb_lod_distance(float distPlayer, const TVec3f* pPos) {
    if (sFlight == 0 || port::popin::DemoMode() == 0 || !port_safe_cam_exists()) {
        return distPlayer;
    }
    return port::popin::FlightLodDistance(port::popin::DemoMode(), sFlight, distPlayer, MR::calcCameraDistanceZ(*pPos));
}
// The renderer's count of draws left out for a program that was not ready (platform/src/port/perf.cpp; the backend notes each one)
extern "C" uint32_t port_popin_render_skips_take(void);

// ---- popin31 (17 Nov): GXV_PB_MINPX (the invisibility floor) and GXV_PB_DEMO=2's release after a flight (port/popin.h, port/popin_site.h) ----
// port_pb_floor is written once a game turn by port_popin_turn (the game thread) and read by the cut sites (the game thread; the fur at the draw and the
// particle draw read a word or two of it). Both switches off: all zero, and every site's guard is one load (`guard`) that says no.
extern "C" {
port::popin::FloorTurn port_pb_floor;
uint32_t port_popin_turn_no = 0;
}
static port::popin::Release sRelease;
static uint32_t sReleaseHeld = 0;    // holds since the last turn (relaxed atomic adds: the fur's site runs at the draw)
static uint32_t sReleaseCuts = 0;    // the camera cut count at the last turn
static bool sReleaseCutsHave = false;
// The game's clipping frustum (the field of view the game judges with: never narrower than the zoomed picture). No judge: "in view" (careful).
extern "C" int port_pb_in_view(float x, float y, float z, float radius) {
    if (!MR::isExistSceneObj(SceneObj_ClippingDirector)) {
        return 1;
    }
    const ClippingJudge* pJudge = MR::getClippingJudge();
    if (pJudge == nullptr) {
        return 1;
    }
    const TVec3f c(x, y, z);
    return pJudge->mFrustum.mayIntersectBall3(c, radius) ? 1 : 0;
}
// The model's extent from the actor's origin (MR::calcModelBoundingRadius: the joints' boxes in their world matrices, so the scale and the pose are in it).
// 0: no model (the site takes its own fallback); 1e5: unreadable (the floor then never lets a cut take it until it is measured again).
extern "C" float port_pb_model_extent(const LiveActor* pActor) {
    if (pActor == nullptr || pActor->mModelManager == nullptr || MR::getJ3DModel(pActor) == nullptr || MR::getJointNum(pActor) <= 0) {
        return 0.0f;
    }
    f32 br = 0.0f;
    MR::calcModelBoundingRadius(&br, pActor);
    return (br > 0.0f && br < 100000.0f) ? br : 100000.0f;
}
// GXV_PB_DEMO=2: the weight of the particle thinning after a flight (0 in a flight, a ramp after it, 1 otherwise; perf.cpp's GXV_PTCL_THIN reads it too)
extern "C" float port_pb_release_ptcl_w(void) {
    if (port::popin::DemoMode() < 2) {
        return 1.0f;
    }
    return sRelease.PtclWeight(port::popin::ReleaseRampTurns());
}
// GXV_PB_MINPX for the particle thinning: the squared camera distance inside which an emitter is not thinned (0: no floor)
extern "C" float port_pb_ptcl_floor_d2(void) {
    return port::popin::FloorDist2K(port_pb_floor, port::popin::kFlParticle, port::popin::MinPxPtclRadius());   // (popin31b: the particles' own floor)
}
// (popin31, the census: particle thinning counts from the emitters' create and draw, other threads: relaxed atomics, drained by the census turn;
// popin31b: what 3 = an emitter the particle floor kept from thinning)
static std::atomic<uint32_t> sPopinPtcl[4];
extern "C" int port_popin_census_on(void) { return port::popin::CensusOn(); }
extern "C" void port_popin_ptcl_note(int what, unsigned n) {
    if (what >= 0 && what < 4) {
        sPopinPtcl[what].fetch_add(n, std::memory_order_relaxed);
    }
}
// popin31b: the census's price line. The timed work of the things a kind's floor kept (the shadow holder, the plant groups' control: the game thread; the
// fur's shell loop: the draw), and the kept shadows' ground probes the memo answered: relaxed atomics, drained once a turn by port_popin_turn.
static std::atomic<uint32_t> sFloorCostN[port::popin::kFloorKinds];
static std::atomic<uint64_t> sFloorCostNs[port::popin::kFloorKinds];
static std::atomic<uint32_t> sFloorMemo[2];  // (looked, answered)
extern "C" void port_pb_floor_cost(int fk, uint32_t ns) {
    if (fk >= 0 && fk < port::popin::kFloorKinds) {
        sFloorCostN[fk].fetch_add(1u, std::memory_order_relaxed);
        sFloorCostNs[fk].fetch_add(ns, std::memory_order_relaxed);
    }
}
extern "C" void port_pb_floor_shadow_memo(int hit) {
    sFloorMemo[0].fetch_add(1u, std::memory_order_relaxed);
    if (hit) {
        sFloorMemo[1].fetch_add(1u, std::memory_order_relaxed);
    }
}

namespace {
// the census: this 5 s window and the flight run in progress, both by cause (popin.h FlightCensus)
port::popin::FlightCensus sPopinWin, sPopinRun;
port::popin::FlightCensus sPopinWinFly;   // (popin31: the 5 s window's flight turns; sPopinWin keeps the play turns)
int sPopinGovLeft = 0;                    // (popin31: turns left in which a port-made pop is put down to a governor level change)
unsigned sPopinGovLast = 0;
bool sPopinGovHave = false;
int sPopinRunKind = 0;
int64_t sPopinRunT0 = 0;
port::popin::Census sPopinCensus;  // (popin24's 10 s census, GXV_POPIN_LOG without GXV_POPIN_CENSUS)
std::atomic_flag sPopinLock = ATOMIC_FLAG_INIT;
int64_t sPopinLast = 0;
bool sPopinAnnounced = false;
struct PopinGuard {
    PopinGuard() { while (sPopinLock.test_and_set(std::memory_order_acquire)) {} }
    ~PopinGuard() { sPopinLock.clear(std::memory_order_release); }
};

// What the game itself says is going on: the opening camera tour, the scenario starter's fly-in, the power star demo, any demo.
int PortDemoKind() {
    if (MR::isStageStateScenarioOpeningCamera()) {
        return port::popin::kFlightOpening;
    }
    if (MR::isExecScenarioStarter()) {
        return port::popin::kFlightStarter;
    }
    if (MR::isPowerStarGetDemoActive()) {
        return port::popin::kFlightStarGet;
    }
    if (MR::isExistSceneObj(SceneObj_DemoDirector) && MR::isDemoActive()) {
        return port::popin::kFlightDemo;
    }
    return 0;
}

void PrintTop(const char* tag, port::popin::Census& c) {
    port::popin::Event* top = c.top;
    for (int i = 0; i < c.nTop; ++i) {
        for (int j = i + 1; j < c.nTop; ++j) {
            if (top[j].size > top[i].size) {
                std::swap(top[i], top[j]);
            }
        }
        const float px = port::popin::PixelHeight(top[i].size, 544.0f);
        if (port::popin::CensusOn()) {
            port_log_summary("%s: %s \"%s\" screen %+.2f,%+.2f (y down, %s) about %.0f px tall at depth %.0f, flight %s", tag, port::popin::KindName(top[i].kind), top[i].name,
                             static_cast< double >(top[i].ndcX), static_cast< double >(top[i].ndcY), top[i].core ? "CENTRE" : "edge", static_cast< double >(px),
                             static_cast< double >(top[i].depth), port::popin::FlightName(top[i].flight));
        } else {
            port_log_summary("popin-top: %s %s \"%s\" screen %+.2f,%+.2f (y down) depth %.0f projected %.1f%% of the picture height%s", port::popin::KindName(top[i].kind),
                             top[i].appear ? "appears" : "disappears", top[i].name, static_cast< double >(top[i].ndcX), static_cast< double >(top[i].ndcY),
                             static_cast< double >(top[i].depth), static_cast< double >(top[i].size * 100.0f), top[i].core ? " (centre well inside)" : " (near the edge)");
        }
    }
}
}  // namespace

// One thing the port or the game made appear (appear 1) or disappear (0) at world position *pPos. radius < 0: the model's bounding
// radius (MR::calcModelBoundingRadius) times its scale, or 150 when it has none. Only with GXV_POPIN_LOG / GXV_POPIN_CENSUS.
static void PortPopinEvent(int kind, int appear, const LiveActor* pActor, const TVec3f* pPos, float radius, float unitRadius, float unitNearer);
extern "C" void port_popin_event(int kind, int appear, const LiveActor* pActor, const TVec3f* pPos, float radius) {
    PortPopinEvent(kind, appear, pActor, pPos, radius, -1.0f, 0.0f);
}
// popin31: the same for a thing whose ONE element is smaller than the whole (a flower group: unitRadius = a member's, unitNearer = how much nearer than the
// group's centre the nearest member may be). The census counts "at or above the floor" by the element.
extern "C" void port_popin_event_unit(int kind, int appear, const LiveActor* pActor, const TVec3f* pPos, float radius, float unitRadius, float unitNearer) {
    PortPopinEvent(kind, appear, pActor, pPos, radius, unitRadius, unitNearer);
}
static void PortPopinEvent(int kind, int appear, const LiveActor* pActor, const TVec3f* pPos, float radius, float unitRadius, float unitNearer) {
    if (!port::popin::LogOn() || pPos == nullptr || !port_safe_cam_exists()) {
        return;
    }
    port::popin::FlightCensus& win = sFlight != 0 ? sPopinWinFly : sPopinWin;
    if (port::popin::CensusOn() && pActor != nullptr && pActor->mModelManager == nullptr &&
        (kind == port::popin::kOrigClip || kind == port::popin::kOrigFar)) {
        // (popin25) a sound source, an area object ...: clipped and unclipped like any actor, nothing to see
        PopinGuard guard;
        ++win.noModel;
        if (sPopinRunKind != 0) {
            ++sPopinRun.noModel;
        }
        return;
    }
    port::popin::Event e;
    e.kind = kind;
    e.appear = appear != 0;
    e.flight = static_cast< uint8_t >(sFlight);
    e.gov = sPopinGovLeft > 0 ? 1 : 0;
    TVec3f ndc;
    ndc.zero();
    const bool on = MR::calcNormalizedScreenPosition(&ndc, *pPos);
    e.ndcX = ndc.x;
    e.ndcY = ndc.y;
    e.inFrame = on;
    e.core = on && std::fabs(ndc.x) < 0.8f && std::fabs(ndc.y) < 0.8f;
    e.depth = MR::calcCameraDistanceZ(*pPos);
    float r = radius;
    if (!(r > 0.0f)) {
        r = 150.0f;
        if (pActor != nullptr && pActor->mModelManager != nullptr) {
            // (popin31: MR::calcModelBoundingRadius measures the joints' boxes in their world matrices: the scale is already in it. popin25 multiplied by the
            // scale again, so a scaled-down thing was counted smaller than it is and a scaled-up one larger.)
            const f32 br = port_pb_model_extent(pActor);
            if (br > 0.0f && br < 100000.0f) {
                r = br;
            }
        }
    }
    e.size = port::popin::SizeFrac(r, e.depth, MR::getFovy());
    e.unit = unitRadius > 0.0f ? port::popin::SizeFrac(unitRadius, e.depth - unitNearer > 1.0f ? e.depth - unitNearer : 1.0f, MR::getFovy()) : -1.0f;
    if (pActor != nullptr && pActor->mName != nullptr) {
        snprintf(e.name, sizeof e.name, "%s", pActor->mName);
    }
    PopinGuard guard;
    sPopinCensus.Add(e);
    win.c.Add(e);
    if (sPopinRunKind != 0) {
        sPopinRun.c.Add(e);
    }
}
// popin31: a cut site kept a thing (byRelease 0: the floor; 1: the release after a flight). The release's count always (it ends the release); the census's
// only with GXV_POPIN_LOG / GXV_POPIN_CENSUS.
extern "C" void port_pb_cut_kept(int kind, int byRelease) {
    if (byRelease) {
        __atomic_add_fetch(&sReleaseHeld, 1u, __ATOMIC_RELAXED);
    }
    if (!port::popin::LogOn() || kind < 0 || kind >= port::popin::kKinds) {
        return;
    }
    PopinGuard guard;
    port::popin::FlightCensus& win = sFlight != 0 ? sPopinWinFly : sPopinWin;
    ++(byRelease ? win.releaseHeld : win.floorKept)[kind];
    if (sPopinRunKind != 0) {
        ++(byRelease ? sPopinRun.releaseHeld : sPopinRun.floorKept)[kind];
    }
}


// ================================================================ detail25: adaptive detail (platform/include/port/detail.h) ====================
// One detail level (0 wide .. 5 tight; 1 = today) from the governor (GXV_DETAIL_GOV: the game thread's drawn-turn time and the front end's
// picture time), a fixed level (GXV_DETAIL_LEVEL) or the stress cycle (GXV_DETAIL_CYCLE). The level moves the existing budget's camera distances
// (they slide, port::detail::kSlewStep a turn), and drives the plant thinning and the fur layers (each object changes unseen, or gradually).
extern "C" int port_autoq_game_cpu_totals(uint32_t* cpu, uint32_t* turns, uint32_t* faults) __attribute__((weak));
extern "C" int port_autoq_front_totals(uint32_t* cpu, uint32_t* pictures) __attribute__((weak));
extern "C" int port_scene_loading_active(void);
extern "C" float port_pb_fur_dist(void);
namespace {
struct DetailSt {
    int level = 1;
    float cur[port::detail::kRules] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    port::detail::Governor gov;
    bool init = false, freeze = false, cutTurn = false, camOk = false, havePlayer = false, haveTotals = false, inputsLogged = false;
    uint32_t turns = 0, cutsSeen = 0, pGc = 0, pGt = 0, pFc = 0, pFp = 0;
    TVec3f cam, player;
    int64_t lastNs = 0, startNs = 0, lastCensusNs = 0;
    double maxIntv = 0, prevMaxIntv = 0, sinceMax = 0, winGame = 0, winFe = 0, prevGame = 0, prevFe = 0, afterAt = -1.0, beforeMax = 0;
    int afterFrom = 0;
    uint32_t plantChanges = 0, furChanges = 0, plantChangesAtChange = 0;
    int plantBudget = 3, guardBudget = 8;
    // census (GXV_DETAIL_PLANT=2 / GXV_DETAIL_FUR=2)
    uint32_t flushes = 0, plantFlushed = 0, plantUnseen = 0, plantNear = 0, plantRemoved[port::detail::kLevels] = {}, plantAtState[3] = {};
    uint32_t furDraws = 0, furLayers = 0, furRemoved[port::detail::kLevels] = {};
};
DetailSt sDt;
void DetailLog(const char* why, int from, int to, double game, double fe, double at) {
    port_log_summary("detail: level %d -> %d: game %.1f ms, front end %.1f ms (%s); turn interval max before %.1f ms, at %.1f ms; plant members changed so far %u, fur changes %u", from, to,
                     game, fe, why, sDt.prevMaxIntv, at, sDt.plantChanges, sDt.furChanges);
    sDt.afterAt = static_cast< double >(port_host_time_ns() / 1000000) + 2000.0;
    sDt.afterFrom = from;
    sDt.beforeMax = sDt.prevMaxIntv;
    sDt.sinceMax = 0.0;
    sDt.plantChangesAtChange = sDt.plantChanges;
}
void PortDetailTurn(int flight) {
    using namespace port::detail;
    DetailSt& d = sDt;
    const int64_t nowNs = port_host_time_ns();
    const double nowMs = static_cast< double >(nowNs / 1000000);
    if (!d.init) {
        d.init = true;
        d.startNs = nowNs;
        d.level = StartLevel();
        d.gov.level = d.level;
        d.gov.hi = Hi();
        d.gov.lo = Lo();
        d.gov.dwell = DwellMs();
        d.gov.relaxMs = RelaxMs();
        if (FixedLevel() >= 0) {
            d.level = FixedLevel();
        }
        port_log_summary("detail: on: GXV_DETAIL_GOV=%d LEVEL=%d CYCLE=%.0f s PLANT=%d FUR=%d BUDGET=%d, start level %d, over %.1f ms / under %.1f ms, dwell %.1f s, relax hold %.1f s",
                         GovOn(), FixedLevel(), static_cast< double >(CycleSec()), PlantMode(), FurMode(), BudgetOn(), d.level, static_cast< double >(Hi()),
                         static_cast< double >(Lo()), DwellMs() * 0.001, RelaxMs() * 0.001);
    }
    if (d.lastNs != 0) {
        const double intv = static_cast< double >(nowNs - d.lastNs) * 1e-6;
        if (intv < 1000.0) {
            if (intv > d.maxIntv) d.maxIntv = intv;
            if (d.afterAt > 0.0 && intv > d.sinceMax) d.sinceMax = intv;
        }
    }
    d.lastNs = nowNs;
    ++d.turns;
    const uint32_t cuts = port_camera_cut_count();
    d.cutTurn = cuts != d.cutsSeen;
    d.cutsSeen = cuts;
    d.camOk = port_safe_cam_exists() != 0;
    if (d.camOk) d.cam = MR::getCamPos();
    const TVec3f* pl = port_safe_player_pos();
    d.havePlayer = pl != nullptr;
    if (pl != nullptr) d.player = *pl;
    d.freeze = flight != 0 || port_scene_loading_active() != 0 || MR::isWipeActive() || MR::isSystemWipeActive();
    // ---- the level
    if (!d.freeze) {
        if (CycleSec() > 0.0f) {
            const int nl = CycleLevel(static_cast< double >(nowNs - d.startNs) * 1e-6, static_cast< double >(CycleSec()) * 1000.0);
            if (nl != d.level) {
                DetailLog("GXV_DETAIL_CYCLE", d.level, nl, d.winGame, d.winFe, d.maxIntv);
                d.level = nl;
            }
        } else if (GovOn() && FixedLevel() < 0 && (d.turns % 30u) == 0) {
            uint32_t gc = 0, gt = 0, gf = 0, fc = 0, fp = 0;
            const bool ok = port_autoq_game_cpu_totals && port_autoq_front_totals && port_autoq_game_cpu_totals(&gc, &gt, &gf) != 0 &&
                            port_autoq_front_totals(&fc, &fp) != 0;
            if (!ok) {
                if (!d.inputsLogged) {
                    d.inputsLogged = true;
                    port_log_summary("detail: the governor has no inputs (the autoq's game and front end totals need GXV_AUTOQ_TARGET=60): level stays %d", d.level);
                }
            } else {
                if (d.haveTotals) {
                    const uint32_t dgc = gc - d.pGc, dgt = gt - d.pGt, dfc = fc - d.pFc, dfp = fp - d.pFp;
                    if (dgt >= 10 && dfp >= 5) {
                        const double mean = static_cast< double >(dgc) / dgt * 0.001, fe = static_cast< double >(dfc) / dfp * 0.001;
                        const double game = DrawnTurnMs(mean, static_cast< double >(dfp) / dgt, static_cast< double >(Undrawn()));
                        d.prevGame = d.winGame;
                        d.prevFe = d.winFe;
                        d.winGame = game;
                        d.winFe = fe;
                        int why = 0;
                        const int nl = d.gov.Update(nowMs, game, fe, false, &why);
                        d.prevMaxIntv = d.maxIntv;
                        if (why != 0) {
                            DetailLog(why == 1 ? "tightened" : "relaxed", d.level, nl, game, fe, d.maxIntv);
                            d.level = nl;
                        }
                        d.maxIntv = 0.0;
                    }
                }
                d.pGc = gc; d.pGt = gt; d.pFc = fc; d.pFp = fp;
                d.haveTotals = true;
            }
        } else if (FixedLevel() >= 0 && d.level != FixedLevel()) {
            d.level = FixedLevel();
        }
    } else if (GovOn()) {
        d.gov.Update(nowMs, 0.0, 0.0, true);
        d.haveTotals = false;  // (the window that spans a flight is not used)
        d.maxIntv = 0.0;
    }
    if (d.afterAt > 0.0 && nowMs >= d.afterAt) {
        port_log_summary("detail: level %d held 2.0 s: game %.1f ms, front end %.1f ms; turn interval max since the change %.1f ms (before it %.1f ms); plant members changed %u in these 2 s",
                         d.level, d.winGame, d.winFe, d.sinceMax, d.beforeMax, d.plantChanges - d.plantChangesAtChange);
        d.afterAt = -1.0;
    }
    // ---- the budget's multipliers slide toward the level's
    const Row& row = LevelRow(d.level);
    for (int r = 0; r < kRules; ++r) {
        const float tgt = BudgetOn() ? row.mult[r] : 1.0f;
        d.cur[r] = Slew(d.cur[r], tgt, kSlewStep);
    }
    // ---- the census
    if ((PlantMode() == 2 || FurMode() == 2) && nowNs - d.lastCensusNs >= 5000000000ll) {
        if (d.lastCensusNs != 0 && d.flushes != 0) {
            const double n = d.flushes;
            port_log_summary("detail-census: %.1f s, %u drawn turns: plant members posed or considered %.1f a turn (unseen %.1f, within 3300 of the player %.1f; at today's state 0/1/2: %.1f/%.1f/%.1f); "
                             "members left out a turn at level 0/1/2/3/4/5: %.1f/%.1f/%.1f/%.1f/%.1f/%.1f; fur: %.2f draws a turn, %.1f layers a turn, layers left out at level 0..5: %.1f/%.1f/%.1f/%.1f/%.1f/%.1f",
                             static_cast< double >(nowNs - d.lastCensusNs) * 1e-9, d.flushes, d.plantFlushed / n, d.plantUnseen / n, d.plantNear / n, d.plantAtState[0] / n, d.plantAtState[1] / n,
                             d.plantAtState[2] / n, d.plantRemoved[0] / n, d.plantRemoved[1] / n, d.plantRemoved[2] / n, d.plantRemoved[3] / n, d.plantRemoved[4] / n, d.plantRemoved[5] / n,
                             d.furDraws / n, d.furLayers / n, d.furRemoved[0] / n, d.furRemoved[1] / n, d.furRemoved[2] / n, d.furRemoved[3] / n, d.furRemoved[4] / n, d.furRemoved[5] / n);
        }
        d.lastCensusNs = nowNs;
        d.flushes = d.plantFlushed = d.plantUnseen = d.plantNear = d.furDraws = d.furLayers = 0;
        for (int i = 0; i < 3; ++i) d.plantAtState[i] = 0;
        for (int i = 0; i < kLevels; ++i) d.plantRemoved[i] = d.furRemoved[i] = 0;
    }
}
}  // namespace
// the camera-distance rule `rule` (port::detail::Rule) at the level: the base distance times the sliding multiplier, never below its floor
extern "C" float port_detail_dist(int rule, float base) {
    const float m = sDt.cur[rule];
    return m == 1.0f ? base : port::detail::Scaled(base, m, rule);
}
extern "C" int port_detail_level(void) { return sDt.level; }
extern "C" int port_detail_frozen(void) { return sDt.freeze ? 1 : 0; }
// the start of a drawn turn's flush: this picture's budgets of plant changes (a few; more on a camera cut)
extern "C" void port_detail_flush_begin(void) {
    sDt.plantBudget = sDt.cutTurn ? 16 : 3;
    sDt.guardBudget = 8;
    ++sDt.flushes;
}
// A plant member (a flower or a bush of a group) at the flush: 1 = leave it out of this picture (not posed, not drawn). `unseen`: it is outside
// the picture's view. Never within 3300 of the player (it is restored before the player arrives), never in a flight.
extern "C" int port_detail_plant_skip(LiveActor* a, int unseen) {
    using namespace port::detail;
    DetailSt& d = sDt;
    if (a->mDetailIdx == 0xFFFF || !d.camOk) {
        return 0;
    }
    const int mode = PlantMode();
    const float sc = port_pb_scale();
    const float dx = a->mPosition.x - d.cam.x, dy = a->mPosition.y - d.cam.y, dz = a->mPosition.z - d.cam.z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    bool near = false;
    if (d.havePlayer) {
        const float px = a->mPosition.x - d.player.x, py = a->mPosition.y - d.player.y, pz = a->mPosition.z - d.player.z;
        const float g = 3300.0f * sc;
        near = px * px + py * py + pz * pz <= g * g;
    } else {
        near = true;
    }
    const int idx = a->mDetailIdx;
    if (mode == 2) {
        ++d.plantFlushed;
        d.plantUnseen += unseen ? 1u : 0u;
        d.plantNear += near ? 1u : 0u;
        ++d.plantAtState[a->mDetailThin < 3 ? a->mDetailThin : 0];
        for (int l = 0; l < kLevels; ++l) {
            const Row& r = LevelRow(l);
            const int s = near ? 0 : ThinNext(0, dist, r.plantHalf * sc, r.plantQuarter * sc, 0.0f);
            d.plantRemoved[l] += KeepMember(idx, s) ? 0u : 1u;
        }
        return 0;
    }
    const Row& row = LevelRow(d.level);
    const int tgt = near ? 0 : ThinNext(a->mDetailTgt, dist, row.plantHalf * sc, row.plantQuarter * sc, HystPct());
    a->mDetailTgt = static_cast< u8 >(tgt);
    int cur = a->mDetailThin;
    if (cur != tgt) {
        if (a->mDetailWait < 255) ++a->mDetailWait;
        if (!d.freeze) {
            bool apply;
            if (near && tgt < cur) {
                apply = d.guardBudget > 0;  // (the player is coming: restore at once, a few a turn)
                if (apply) --d.guardBudget;
            } else {
                apply = MayApply(cur, tgt, unseen != 0, d.cutTurn, a->mDetailWait, FallbackPics(), d.plantBudget);
            }
            if (apply) {
                cur = tgt;
                a->mDetailThin = static_cast< u8 >(cur);
                a->mDetailWait = 0;
                ++d.plantChanges;
            }
        }
    } else {
        a->mDetailWait = 0;
    }
    return KeepMember(idx, cur) ? 0 : 1;
}
// Fur (FurCtrl::drawFur): the target state (0 full, 1 every 2nd layer, 2 few) for the planet at this position; the census at every level in dry mode.
extern "C" int port_detail_fur_target(float x, float y, float z, int prevTgt, int numLayers) {
    using namespace port::detail;
    DetailSt& d = sDt;
    if (!d.camOk) {
        return 0;
    }
    const float sc = port_pb_scale();
    const float dx = x - d.cam.x, dy = y - d.cam.y, dz = z - d.cam.z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    bool near = !d.havePlayer;
    if (d.havePlayer) {
        const float px = x - d.player.x, py = y - d.player.y, pz = z - d.player.z, g = 2500.0f * sc;
        near = px * px + py * py + pz * pz <= g * g;
    }
    const float pb = port_pb_fur_dist();
    auto bands = [&](int l, float* mid, float* far) {
        const Row& r = LevelRow(l);
        *mid = (r.furMid > 0.0f ? r.furMid : (pb > 0.0f ? Scaled(pb, r.mult[kFur], kFur) : 0.0f)) * sc;
        *far = r.furFar * sc;
    };
    if (FurMode() == 2) {
        ++d.furDraws;
        d.furLayers += static_cast< uint32_t >(numLayers);
        for (int l = 0; l < kLevels; ++l) {
            float mid, far;
            bands(l, &mid, &far);
            const int s = near ? 0 : ThinNext(0, dist, mid, far, 0.0f);
            d.furRemoved[l] += static_cast< uint32_t >(numLayers - FurKept(s, numLayers, FurFarLayers()));
        }
        return 0;
    }
    if (near) {
        return 0;
    }
    float mid, far;
    bands(d.level, &mid, &far);
    return ThinNext(prevTgt, dist, mid, far, HystPct());
}
extern "C" void port_detail_fur_changed(void) { ++sDt.furChanges; }

// Once a game turn (ClippingActorHolder::movement): this turn's zoom ratio, the flight state (GXV_PB_DEMO, GXV_POPIN_LEAD, the census) and
// the census lines.
// far3 / far30: is a flight on (the opening camera, the scenario starter, a demo, the star get)? While it is, the far field measures from the PLAYER and moves no horizon (perf.cpp holds the
// ramps, the governors and the ladder), so nothing appears or vanishes in bursts as the camera races past. Held for 60 turns after it ends (the player gets control a moment later).
extern "C" int port_ff_wants_flight(void);   // (perf.cpp: the far field or the Quality far steps are in use)
static int sFfFly = 0, sFfFlyTail = 0;
extern "C" int port_ff_flyover(void) { return sFfFly; }
extern "C" void port_anticipate_scene(int demo_kind, const float* player_pos);   // (anticip32: perf.cpp; returns at once unless GXV_ANTICIPATE)
extern "C" unsigned port_gov_levels(void);   // (popin31, perf.cpp: the front end's, the game thread's and the far-draw governor's levels, 8 bits each)
extern "C" void port_popin_turn(void) {
    const bool cam = port_safe_cam_exists();
    {   // (anticip32: what the game says is going on and where Mario is; read-only)
        const TVec3f* antPl = port_safe_player_pos();
        port_anticipate_scene(cam ? PortDemoKind() : 0, antPl != nullptr ? &antPl->x : nullptr);
    }
    if (cam && port_ff_wants_flight() != 0) {
        if (PortDemoKind() != 0) {
            sFfFlyTail = 60;
        } else if (sFfFlyTail > 0) {
            --sFfFlyTail;
        }
        sFfFly = sFfFlyTail > 0 ? 1 : 0;
    } else {
        sFfFly = 0;
        sFfFlyTail = 0;
    }
    if (port::popin::ZoomMode() >= 1 && cam) {
        sPopinZoom = port::popin::ZoomRatio(MR::getFovyForLogic(), MR::getFovy());
    } else {
        sPopinZoom = 1.0f;
    }
    // popin25: the flight state; nothing of it is computed unless one of its switches (or the census) is on
    const int demoMode = port::popin::DemoMode();
    float camSpeed = 0.0f, camPlayer = 0.0f;
    if (cam && (demoMode >= 1 || port::popin::LeadTurns() > 0.0f || port::popin::CensusOn() || port::detail::Active())) {
        const TVec3f c = MR::getCamPos();
        const float cp[3] = {c.x, c.y, c.z};
        sCamTrack.Update(cp, 1000.0f);  // (a jump of more than 1000 units in a turn is a cut, as GXV_SEAMLESS_CUT_DIST's default)
        {
            const TVec3f z = MR::getCamZdir();
            const float zd[3] = {z.x, z.y, z.z};
            sCamTrack.UpdateDir(zd);  // (a turn of more than 35 degrees in one is a cut, as GXV_SEAMLESS_CUT_DEG's default)
        }
        camSpeed = sCamTrack.speed;
        const TVec3f* pl = port_safe_player_pos();
        if (pl != nullptr) {
            const float dx = c.x - pl->x, dy = c.y - pl->y, dz = c.z - pl->z;
            camPlayer = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        sFlight = port::popin::ClassifyFlight(PortDemoKind(), camSpeed, camPlayer, port::popin::FlightSpeedMin(), port::popin::FlightFarCam());
    } else {
        sCamTrack.Reset();
        sFlight = 0;
    }
    sPbWiden = port::popin::FlightWiden(demoMode, sFlight, port::popin::DemoWiden());
    sPbOff = port::popin::BudgetOffInFlight(demoMode, sFlight);
    ++port_popin_turn_no;
    // popin31: GXV_PB_DEMO=2's release after a flight (a cut that would start on a thing in the picture waits; port/popin.h Release)
    if (demoMode >= 2) {
        const uint32_t cuts = port_camera_cut_count();
        const bool cutNow = sReleaseCutsHave && cuts != sReleaseCuts;
        sReleaseCuts = cuts;
        sReleaseCutsHave = true;
        sRelease.Turn(sPbOff, cutNow, __atomic_exchange_n(&sReleaseHeld, 0u, __ATOMIC_RELAXED), port::popin::ReleaseQuietTurns());
        port_pb_floor.releasing = sRelease.on ? 1 : 0;
    }
    // popin31: GXV_PB_MINPX, the floor's constants for this turn (the picture's zoomed field of view, the game's own for the corner's stretch).
    // popin31b: and each kind's (GXV_PB_MINPX_<KIND>; unset: GXV_PB_MINPX); the guard words per kind.
    {
        const float minPx = port::popin::MinPx();
        float kindPx[port::popin::kFloorKinds];
        bool anyKind = false;
        for (int fk = 0; fk < port::popin::kFloorKinds; ++fk) {
            kindPx[fk] = port::popin::MinPxKind(fk);
            anyKind = anyKind || kindPx[fk] > 0.0f;
        }
        static float sFovPic = -1.0f, sFovGame = -1.0f;  // (the constants are made again only when a field of view changed: the zoom's ease, a camera's own)
        static bool sFloorMade = false;                  // (... or after they were cleared: no camera, a stage change)
        if ((minPx > 0.0f || anyKind) && cam) {
            const float fovPic = MR::getFovy(), fovGame = MR::getFovyForLogic();
            if (fovPic != sFovPic || fovGame != sFovGame || !sFloorMade) {
                sFovPic = fovPic;
                sFovGame = fovGame;
                sFloorMade = true;
                const float k = 0.00872664626f;  // pi / 360
                const float tP = std::tan(fovPic * k), tG = std::tan(fovGame * k);
                port::popin::FloorSet(port_pb_floor, minPx, port::popin::MinPxBand(), port::popin::kPictureRows, tP, tG, port::popin::kPictureAspect);
                port::popin::FloorSetKinds(port_pb_floor, kindPx, port::popin::MinPxBand(), port::popin::kPictureRows, tP, tG, port::popin::kPictureAspect);
            }
            const TVec3f c = MR::getCamPos();
            port_pb_floor.cam[0] = c.x;
            port_pb_floor.cam[1] = c.y;
            port_pb_floor.cam[2] = c.z;
        } else {
            port_pb_floor.kHeld = port_pb_floor.kNew = 0.0f;
            port::popin::FloorClearKinds(port_pb_floor);
            sFloorMade = false;
        }
        port::popin::FloorGuards(port_pb_floor);
    }
    if (port::detail::Active()) {
        PortDetailTurn(sFlight);
    }
    if (!port::popin::LogOn()) {
        return;
    }
    PopinGuard guard;
    ++sPopinCensus.turns;
    const bool census = port::popin::CensusOn() != 0;
    if (census) {
        // popin31: the governors' levels (a change moves every distance at once: the port-made pops of the next kGovWindow turns are put down to it)
        const unsigned gov = port_gov_levels();
        const unsigned govFe = gov & 255u, govGame = (gov >> 8) & 255u, govCpu = (gov >> 16) & 255u;
        const unsigned govTop = govFe > govGame ? (govFe > govCpu ? govFe : govCpu) : (govGame > govCpu ? govGame : govCpu);
        const bool govChanged = sPopinGovHave && gov != sPopinGovLast;
        sPopinGovLast = gov;
        sPopinGovHave = true;
        if (govChanged) {
            sPopinGovLeft = port::popin::kGovWindow;
        } else if (sPopinGovLeft > 0) {
            --sPopinGovLeft;
        }
        // (popin31b: each kind's own floor for its pops; the game's own kinds against GXV_PB_MINPX, or when that is unset the smallest kind floor on)
        float kindPx[port::popin::kFloorKinds];
        float smallest = 0.0f;
        for (int fk = 0; fk < port::popin::kFloorKinds; ++fk) {
            kindPx[fk] = port::popin::MinPxKind(fk);
            if (kindPx[fk] > 0.0f && (!(smallest > 0.0f) || kindPx[fk] < smallest)) {
                smallest = kindPx[fk];
            }
        }
        const float gamePx = port::popin::MinPx() > 0.0f ? port::popin::MinPx() : smallest;
        const float floorFrac = gamePx / port::popin::kPictureRows;
        port::popin::FlightCensus* const spans[3] = {&sPopinWin, &sPopinWinFly, &sPopinRun};
        for (port::popin::FlightCensus* f : spans) {
            f->c.floorFrac = floorFrac;
            f->c.SetKindFloors(kindPx, port::popin::kPictureRows);
            f->govFe = govFe;
            f->govGame = govGame;
            f->govCpu = govCpu;
            if (govTop > f->govMax) f->govMax = govTop;
            if (govChanged) ++f->govChanges;
        }
        for (int i = 0; i < 4; ++i) {
            const uint32_t n = sPopinPtcl[i].exchange(0, std::memory_order_relaxed);
            port::popin::FlightCensus& w = sFlight != 0 ? sPopinWinFly : sPopinWin;
            uint32_t& a = i == 0 ? w.ptclScaled : (i == 1 ? w.ptclThinDraws : (i == 2 ? w.ptclLeftOut : w.ptclFloorKept));
            a += n;
            if (sPopinRunKind != 0) {
                uint32_t& b = i == 0 ? sPopinRun.ptclScaled : (i == 1 ? sPopinRun.ptclThinDraws : (i == 2 ? sPopinRun.ptclLeftOut : sPopinRun.ptclFloorKept));
                b += n;
            }
        }
        {   // popin31b: the price line's timed samples (this turn's window: play or flight)
            port::popin::FlightCensus& w = sFlight != 0 ? sPopinWinFly : sPopinWin;
            for (int fk = 0; fk < port::popin::kFloorKinds; ++fk) {
                const uint32_t n = sFloorCostN[fk].exchange(0, std::memory_order_relaxed);
                const uint64_t ns = sFloorCostNs[fk].exchange(0, std::memory_order_relaxed);
                w.costN[fk] += n;
                w.costNs[fk] += ns;
            }
            w.shadowMemoLook += sFloorMemo[0].exchange(0, std::memory_order_relaxed);
            w.shadowMemoHit += sFloorMemo[1].exchange(0, std::memory_order_relaxed);
        }
        // popin31: the window is split: play turns in sPopinWin, flight turns in sPopinWinFly (each its own line)
        port::popin::FlightCensus& win = sFlight != 0 ? sPopinWinFly : sPopinWin;
        ++win.turnsAll;
        if (sFlight != 0) {
            ++win.turnsFlight;
            win.lastFlight = sFlight;
        }
        if (port_pb_floor.releasing) {
            ++win.turnsRelease;
        }
        if (camSpeed > win.speedMax) {
            win.speedMax = camSpeed;
        }
        if (camPlayer > win.camPlayerMax) {
            win.camPlayerMax = camPlayer;
        }
        // a flight run: one per kind of flight, from its first turn to its last
        if (sFlight != sPopinRunKind) {
            const int64_t t = port_host_time_ns();
            if (sPopinRunKind != 0) {
                char line[1600];
                sPopinRun.renderSkips += port_popin_render_skips_take();
                sPopinRun.Format(line, sizeof line, static_cast< double >(t - sPopinRunT0) * 1e-9, port::popin::FlightName(sPopinRunKind));
                port_log_summary("popin25-run: %s ended; %s", port::popin::FlightName(sPopinRunKind), line);
                sPopinRun.Format31(line, sizeof line, static_cast< double >(t - sPopinRunT0) * 1e-9, port::popin::FlightName(sPopinRunKind), port::popin::MinPx(),
                                   port::popin::kPictureRows, kindPx);
                port_log_summary("%s", line);
                PrintTop("popin25-run-top", sPopinRun.c);
            }
            sPopinRunKind = sFlight;
            sPopinRunT0 = t;
            sPopinRun.Reset();
            if (sFlight != 0) {
                port_log_summary("popin25-run: %s starts (camera %.0f u/turn, %.0f from the player; GXV_PB_DEMO=%d widen %.2f lead %.0f turns)", port::popin::FlightName(sFlight),
                                 static_cast< double >(camSpeed), static_cast< double >(camPlayer), demoMode, static_cast< double >(port::popin::DemoWiden()),
                                 static_cast< double >(port::popin::LeadTurns()));
            }
        }
        if (sPopinRunKind != 0) {
            ++sPopinRun.turnsAll;
            ++sPopinRun.turnsFlight;
            if (camSpeed > sPopinRun.speedMax) {
                sPopinRun.speedMax = camSpeed;
            }
            if (camPlayer > sPopinRun.camPlayerMax) {
                sPopinRun.camPlayerMax = camPlayer;
            }
        }
    }
    if ((sPopinCensus.turns & (census ? 15u : 63u)) != 0) {
        return;
    }
    const int64_t now = port_host_time_ns();
    if (!sPopinAnnounced) {
        sPopinAnnounced = true;
        port_log_summary("popin24: GXV_POPIN_LOG=1 census on; GXV_POPIN_HYST=%.0f%% GXV_POPIN_ZOOM=%d (ratio now %.2f) GXV_POPIN_CLIPPAD=%.0f%%; perf budget %s",
                         static_cast< double >(port::popin::HystPct()), port::popin::ZoomMode(), static_cast< double >(sPopinZoom),
                         static_cast< double >((port::popin::ClipPadFactor() - 1.0f) * 100.0f), port_pb_budget_on() ? "ON" : "off");
        port_log_summary("popin25: GXV_POPIN_CENSUS=%d GXV_PB_DEMO=%d GXV_PB_DEMO_WIDEN=%.2f GXV_POPIN_LEAD=%.0f GXV_POPIN_LEAD_MAX=%.0f flight = a demo / the opening camera / the "
                         "starter / the star demo, or a camera >= %.0f u/turn and >= %.0f from the player",
                         census ? 1 : 0, demoMode, static_cast< double >(port::popin::DemoWiden()), static_cast< double >(port::popin::LeadTurns()),
                         static_cast< double >(port::popin::LeadClipMax()), static_cast< double >(port::popin::FlightSpeedMin()),
                         static_cast< double >(port::popin::FlightFarCam()));
        port_log_summary("popin31: GXV_PB_MINPX=%.1f (band %.0f%%, particles as %.0f-unit spheres; the floor's distance for a 100-unit radius now %.0f) GXV_PB_DEMO_RAMP=%d "
                         "GXV_PB_DEMO_QUIET=%d (the release after a flight: GXV_PB_DEMO=2 only)",
                         static_cast< double >(port::popin::MinPx()), static_cast< double >(port::popin::MinPxBand() * 100.0f),
                         static_cast< double >(port::popin::MinPxPtclRadius()), static_cast< double >(100.0f * port_pb_floor.kHeld),
                         port::popin::ReleaseRampTurns(), port::popin::ReleaseQuietTurns());
        port_log_summary("popin31b: the floor by kind (px; unset = GXV_PB_MINPX): shadow %.1f decor %.1f fur %.1f lod %.1f speck %.1f starbit %.1f particle %.1f",
                         static_cast< double >(port::popin::MinPxKind(port::popin::kFlShadow)), static_cast< double >(port::popin::MinPxKind(port::popin::kFlDecor)),
                         static_cast< double >(port::popin::MinPxKind(port::popin::kFlFur)), static_cast< double >(port::popin::MinPxKind(port::popin::kFlLod)),
                         static_cast< double >(port::popin::MinPxKind(port::popin::kFlSpeck)), static_cast< double >(port::popin::MinPxKind(port::popin::kFlStarBit)),
                         static_cast< double >(port::popin::MinPxKind(port::popin::kFlParticle)));
    }
    if (sPopinLast == 0) {
        sPopinLast = now;
        sPopinCensus.Reset();
        sPopinWin.Reset();
        sPopinWinFly.Reset();
        (void)port_popin_render_skips_take();
        return;
    }
    if (now - sPopinLast < (census ? 5000000000ll : 10000000000ll)) {
        return;
    }
    const double seconds = static_cast< double >(now - sPopinLast) * 1e-9;
    sPopinLast = now;
    char line[1600];
    if (census) {
        float kindPx[port::popin::kFloorKinds];  // (popin31b)
        bool anyFloor = false;
        for (int fk = 0; fk < port::popin::kFloorKinds; ++fk) {
            kindPx[fk] = port::popin::MinPxKind(fk);
            anyFloor = anyFloor || kindPx[fk] > 0.0f;
        }
        // popin31: play and flight apart (the renderer's skips are not split by turn: they go with the play line)
        const uint32_t skips = port_popin_render_skips_take();
        sPopinWin.renderSkips = skips;
        sPopinRun.renderSkips += skips;
        if (sPopinWin.turnsAll > 0) {
            const char* label = sPopinWin.turnsRelease > 0 ? "play, release after a flight" : "play";
            sPopinWin.Format(line, sizeof line, seconds, label);
            port_log_summary("%s", line);
            sPopinWin.Format31(line, sizeof line, seconds, label, port::popin::MinPx(), port::popin::kPictureRows, kindPx);
            port_log_summary("%s", line);
            if (anyFloor) {
                sPopinWin.Format31b(line, sizeof line, seconds, label, kindPx);  // (popin31b: the price of what each kind's floor gives back)
                port_log_summary("%s", line);
            }
            PrintTop("popin25-top", sPopinWin.c);
        }
        if (sPopinWinFly.turnsAll > 0) {
            const char* label = port::popin::FlightName(sPopinWinFly.lastFlight);
            sPopinWinFly.Format(line, sizeof line, seconds, label);
            port_log_summary("%s", line);
            sPopinWinFly.Format31(line, sizeof line, seconds, label, port::popin::MinPx(), port::popin::kPictureRows, kindPx);
            port_log_summary("%s", line);
            if (anyFloor) {
                sPopinWinFly.Format31b(line, sizeof line, seconds, label, kindPx);
                port_log_summary("%s", line);
            }
            PrintTop("popin25-top", sPopinWinFly.c);
        }
        sPopinWin.Reset();
        sPopinWinFly.Reset();
        sPopinCensus.Reset();
        return;
    }
    sPopinCensus.Format(line, sizeof line, seconds);
    port_log_summary("%s; zoom ratio %.2f", line, static_cast< double >(sPopinZoom));
    PrintTop("popin-top", sPopinCensus);
    sPopinCensus.Reset();
}
#endif
