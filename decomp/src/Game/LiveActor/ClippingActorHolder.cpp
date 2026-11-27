#include "Game/LiveActor/ClippingActorHolder.hpp"
#include "Game/LiveActor/ClippingActorInfo.hpp"
#include "Game/LiveActor/ClippingGroupHolder.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/LiveActor/ViewGroupCtrl.hpp"
#include "Game/Util/JMapUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
extern "C" int port_safe_cam_exists(void);  // (LodCtrl.cpp)
#ifdef TARGET_PC
extern "C" void port_horizon_reset(void);  // (horizon25, below)
extern "C" void port_cr_init(void);        // (cliprange25, below)
#endif
#ifdef TARGET_PC
#include "Game/LiveActor/ClippingJudge.hpp"
#include "Game/MapObj/PlantGroup.hpp"
#include "port/diagnostics.h"
#include "port/hot_knob.h"
#include "port/stage_knob.h"  // (stagepreset25)
#include "port/obj_cut.h"
#include "Game/Util/CameraUtil.hpp"  // (GXV_PB_DECOR: MR::getCamPos)
#include "Game/LiveActor/ModelManager.hpp"
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphBase/J3DPacket.hpp>  // (J3DPortPicture: GXV_ZOOM_DRAWCULL)
#include "port/lockstep.h"  // (fmt32)
#include "port/popin.h"
#include "port/stream_census.h"  // (GXV_STREAM_CENSUS: the size of each judged actor's clipping sphere)
#include "Game/Player/MarioAccess.hpp"  // (GXV_TINYCULL: not the player)
#include "Game/Util/ModelUtil.hpp"
#include "port/horizon.h"  // (horizon25, GXV_HORIZON)
#include "port/cliprange.h"  // (cliprange25: GXV_CLIPCENSUS, GXV_CLIPSCALE, GXV_FARANIM, GXV_FARDRAW)
#include "port/piecefade.h"  // (q30fin26: GXV_THINFADE)
#include "port/cpucuts.h"   // (cpucuts25: GXV_DRAWCENSUS, GXV_FARDRAW_KINDS / _HOLD / _FLUSH)
#include "port/distcensus.h"  // (far30: GXV_DISTCENSUS)
#include <JSystem/J3DGraphBase/J3DShape.hpp>      // (far30: the indices of a model)
#include <JSystem/J3DGraphBase/J3DShapeDraw.hpp>
#include "port/cpugov.h"    // (honey25: GXV_CPUGOV, the far-draw cut's governor)
#include "port/cpulod.h"    // (cpulod26: GXV_CPULOD, GXV_MATLOD, GXV_FAMPOSE, GXV_PIECETHIN)
#include "port/popin_site.h"  // (popin31: GXV_PB_MINPX, GXV_PB_DEMO=2's release)
#include "port/fegov.h"     // (fe26: GXV_FE_GOV)
#include "Game/LiveActor/HitSensor.hpp"
#include "Game/LiveActor/HitSensorInfo.hpp"
#include "Game/LiveActor/HitSensorKeeper.hpp"
#include "Game/Map/CollisionParts.hpp"
#include "Game/Map/KCollision.hpp"
#include "Game/Util/DirectDraw.hpp"
#include "Game/Util/SceneUtil.hpp"
#include <revolution/gx/GXPixel.h>
#include <math.h>
#include <cstdlib>
#endif

namespace {
    static int sActorNumMax = 2560;
};  // namespace

ClippingActorHolder::ClippingActorHolder() : NameObj("\x83\x4e\x83\x8a\x83\x62\x83\x73\x83\x93\x83\x4f\x83\x41\x83\x4e\x83\x5e\x81\x5b\x95\xdb\x8e\x9d"), mActorNum(), _10(), _14(), _18(), _1C(), mViewGroupCtrl() {
    _10 = new ClippingActorInfoList(::sActorNumMax);
    _14 = new ClippingActorInfoList(::sActorNumMax);
    _18 = new ClippingActorInfoList(::sActorNumMax);
    _1C = new ClippingActorInfoList(::sActorNumMax);
    mViewGroupCtrl = new ViewGroupCtrl();
#ifdef TARGET_PC
    port_horizon_reset();  // (GXV_HORIZON: a new scene: the occluders of the last one are gone)
    port_cr_init();        // (GXV_CLIPCENSUS: the census flag the lists read)
#endif
}

#ifdef TARGET_PC
// GXV_OC_CLIPFAST (objcut, 7 Nov; default 1, exact): the judging loop below asked MR::getClippingJudge() (getSceneObjHolder, getObj,
// the director's mJudge: three calls) and ClippingJudge::isJudgedToClipFrustum (another translation unit) for each of the ~600 clipped
// or clippable actors of a turn. With the switch on, the judge is read once and the six-plane test (THex3f::mayIntersectBall3, the
// same inline code) runs in the loop on the same frustum, the same level and the same sphere; the start / end of clipping is called
// exactly when and in the order the original calls it, and the list length and the entry are read at each step as before
// (startClipped / endClipped can move actors between the lists). The frustums are made once a turn, before this loop
// (ClippingJudge::movement), and nothing in the loop writes them.
//   0  off: the original loop.   1  on.
//   2  verify: each decision is also taken by ClippingActorInfo::isJudgedToClip (the original); a difference is counted and logged,
//      the original's decision is used.
// Logging builds write, about every 5 s: oc-clip: judged J mismatches X
extern "C" float port_pb_decor_dist(void);
extern "C" int port_upd_prefetch_on(void);  // (GXV_UPD_PREFETCH, update23: platform/src/port/perf.cpp)
extern "C" void port_logic_count(int what, unsigned n);  // (platform/src/port/perf.cpp)
bool PortPbDecorFar(const ClippingActorInfo* pInfo, float dist, float camX, float camY, float camZ);  // (ClippingActorInfo.cpp, GXV_PB_DECOR)
extern "C" float port_popin_clip_lead(void);  // (popin25, GXV_POPIN_LEAD: LodCtrl.cpp; the clipping spheres' growth for a fast camera, 0 when off)
extern "C" void port_popin_turn(void);  // (popin24, LodCtrl.cpp: the zoom ratio of the turn, the GXV_POPIN_LOG census line)
extern "C" void port_popin_event(int kind, int appear, const LiveActor* pActor, const TVec3f* pPos, float radius);
namespace {
int ClipFastMode() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_OC_CLIPFAST");
        return e && *e ? static_cast< int >(strtol(e, nullptr, 0)) : 0;
    }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
uint32_t sClipJudged, sClipMismatches, sClipTick;
int64_t sClipLast;

// GXV_UPD_PREFETCH (update23, 8 Nov; exact: cache hints only). The judging loops below chase ~600 scattered objects a turn in heavy
// zones (the info, the sphere centre inside its actor, its view-group entry, the actor's flags), one cache miss after another. Ask for
// the info 4 entries ahead and, for the entry 2 ahead (its info asked for 2 steps ago), the lines the judge and isClipped read.
inline void ClipPrefetch(ClippingActorInfo* const* pList, s32 i, s32 n) {
    if (i + 4 < n) {
        __builtin_prefetch(pList[i + 4]);
    }
    if (i + 2 < n) {
        const ClippingActorInfo* pInfo = pList[i + 2];
        __builtin_prefetch(pInfo->_4);
        __builtin_prefetch(pInfo->_14);
        if (pInfo->mActor != nullptr) {
            __builtin_prefetch(&pInfo->mActor->mFlag);
        }
    }
}

// GXV_ZOOM_DRAWCULL (zoomclip23, 8 Nov; default 1; exact for the picture of 2.2.1's zoom). With GXV_ZOOM_LOGIC_VIEW (CameraUtil.cpp) the
// clipping above judges with the game's own field of view, so an actor in the original view but outside the zoomed picture keeps running
// (moves, its sensors stay on) as it does at zoom 1.0. Such an actor puts nothing in the picture: its clipping sphere is wholly outside
// the picture's frustum (ClippingJudge::isJudgedToClipPictureFrustum: the frustum the zoomed build clipped it with, same sphere, same
// far distance). Its model is then not drawn in the next picture (J3DModel::mPortCullPicture: DrawBuffer.cpp leaves its shapes out as
// hidden ones, gamepar's cull), so the picture costs what 2.2.1's did; the logic is the original's. Only while the zoom is in force and
// no mirror camera shows actors off the picture; not for the flower groups' plants (their flush has its own cull). The decision is made
// again every turn (the last turn before a picture decides); a stamp this cull made is cleared when the actor is back in the picture,
// clipped, or leaves the judged list. Its shadow and effects are still drawn (they may fall inside the picture).
// 0: such an actor is drawn. Logging builds write, about every 5 s while zoomed:
//   zoomclip: N turns, per turn K actors outside the zoomed picture kept running (clipped by 2.2.1), D of them not drawn
int ZoomDrawCullOn() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_ZOOM_DRAWCULL");
        return e && *e ? static_cast< int >(strtol(e, nullptr, 0)) : 1;
    }());
}
struct ZoomCullTurn {
    const ClippingJudge* judge;
    u32 next;      // the picture draw() makes next (J3DPortPicture() + 1)
    bool zoomed;   // the picture's frustum is narrower than the game's (counting)
    bool cull;     // and the cull is on
};
uint32_t sZoomTurns, sZoomKept, sZoomCulled, sZoomTick;
int64_t sZoomLast;

ZoomCullTurn ZoomCullBegin(const ClippingJudge* pJudge) {
    ZoomCullTurn t;
    t.judge = pJudge;
    t.next = J3DPortPicture() + 1;
    t.zoomed = pJudge->mPortPictureZoomed;
    t.cull = t.zoomed && ZoomDrawCullOn() != 0 && !MR::isExistMirrorCamera();
    if (t.zoomed) {
        ++sZoomTurns;
    }
    return t;
}

J3DModel* ZoomCullModel(const LiveActor* pActor) {
    return pActor->mModelManager != nullptr ? pActor->mModelManager->mModel : nullptr;
}

void ZoomUncull(ClippingActorInfo* pInfo, bool clipped = false) {
    if (!pInfo->mPortZoomCulled) {
        return;
    }
    pInfo->mPortZoomCulled = false;
    J3DModel* pModel = ZoomCullModel(pInfo->mActor);
    // Other clipping or an inactive LOD can still hide the actor when its zoom stamp expires.
    if (port::popin::LogOn() && !clipped && pModel && !MR::isClipped(pInfo->mActor) && !MR::isDead(pInfo->mActor) && !MR::isHiddenModel(pInfo->mActor)) {
        port_popin_event(port::popin::kZoomCull, 1, pInfo->mActor, pInfo->_4, pInfo->_8);
    }
    if (pModel != nullptr && pModel->mPortCullPicture == J3DPortPicture() + 1) {
        pModel->mPortCullPicture = 0;
    }
}

// After the actor's clipping was judged this turn (clipped: its state now).
void ZoomCullJudge(ClippingActorInfo* pInfo, bool clipped, const ZoomCullTurn& t) {
    if (!t.zoomed) {
        ZoomUncull(pInfo, clipped);
        return;
    }
    const LiveActor* pActor = pInfo->mActor;
    bool cull = false;
    if (!clipped && t.judge->isJudgedToClipPictureFrustum(*pInfo->_4, pInfo->_8)) {
        ++sZoomKept;
        J3DModel* pModel = ZoomCullModel(pActor);
        if (t.cull && pModel != nullptr && pActor->mLogicFreeJoints == 0) {
            pModel->mPortCullPicture = t.next;
            if (!pInfo->mPortZoomCulled && port::popin::LogOn() && !MR::isDead(pActor) && !MR::isHiddenModel(pActor)) {
                port_popin_event(port::popin::kZoomCull, 0, pActor, pInfo->_4, pInfo->_8);  // (GXV_POPIN_LOG: no longer drawn)
            }
            pInfo->mPortZoomCulled = true;
            ++sZoomCulled;
            cull = true;
        }
    }
    if (!cull) {
        ZoomUncull(pInfo, clipped);
    }
}

// GXV_TINYCULL=<lines> (lighter25, 11 Nov; default 0; a MINOR picture change): an actor whose clipping sphere (the one judged above, the
// game's own bound for its model; never smaller than what the model shows, or the game's frustum clipping would cut the model at the
// picture's edges) has a projected diameter under <lines> lines of a 456-line picture (the game's picture; the console's rung scales
// it, e.g. 348 / 456 at 608x348) is not drawn in the next picture: its model is stamped like GXV_ZOOM_DRAWCULL's (J3DModel::
// mPortCullPicture: the DrawBuffer drawers leave its shapes out as hidden ones). Its logic, animation, shadow and effects are as before
// (the shadow and the effects are drawn by their own drawers); only the model's few pixels go. Not for the player, not while a mirror
// camera shows actors, not for models whose joints only the picture reads (mLogicFreeJoints, as the zoom cull). A stamp it made is taken
// back when the actor is no longer that small or is clipped. Values: 0 off; 1..16 lines (2 = under two lines: at 608x348 about 1.5
// pixels). GXV_TINYCULL_DRY=1: judged and counted, never stamped. Logging builds write about every 5 s:
//   tinycull: N turns, per turn K actors under T lines, C of them not drawn (dry: 0), D drawn again; left out: player P, free joints F
float TinyCullLines() {
    return PORT_STAGE_KNOB(float, [] {  // (stagepreset25: stage-settable)
        const char* e = getenv("GXV_TINYCULL");
        float v = e && *e ? static_cast< float >(strtod(e, nullptr)) : 0.0f;
        float stageValue;
        if (port::stageknob::Lookup("GXV_TINYCULL", &stageValue)) {
            v = stageValue;
        }
        return v > 16.0f ? 16.0f : (v > 0.0f ? v : 0.0f);
    }());
}
bool TinyCullDry() {
    return PORT_HOT_KNOB(bool, [] {
        const char* e = getenv("GXV_TINYCULL_DRY");
        return e && *e == '1';
    }());
}
uint32_t sTinyTurns, sTinyUnder, sTinyCulled, sTinyBack, sTinyPlayer, sTinyFree, sTinyTick;
int64_t sTinyLast;
void TinyUncull(J3DModel* pModel, u32 next) {
    if (pModel == nullptr || pModel->mPortTinyCulled == 0) {
        return;
    }
    pModel->mPortTinyCulled = 0;
    if (pModel->mPortCullPicture == next) {
        pModel->mPortCullPicture = 0;
        ++sTinyBack;
    }
}
void TinyCullJudge(const LiveActor* pActor, const ClippingActorInfo* pInfo, J3DModel* pModel, float dist, float tanHalf, float lines, u32 next,
                   const LiveActor* pPlayer) {
    const float d = (dist > pInfo->_8 && tanHalf > 0.0f) ? pInfo->_8 * 456.0f / (dist * tanHalf) : 1.0e9f;
    if (d >= lines) {
        TinyUncull(pModel, next);
        return;
    }
    ++sTinyUnder;
    if (pActor == pPlayer) {
        ++sTinyPlayer;
        TinyUncull(pModel, next);
        return;
    }
    if (pActor->mLogicFreeJoints != 0) {
        ++sTinyFree;
        TinyUncull(pModel, next);
        return;
    }
    if (TinyCullDry()) {
        return;
    }
    pModel->mPortCullPicture = next;
    pModel->mPortTinyCulled = 1;
    ++sTinyCulled;
}
void TinyCullLog() {
#if PORT_LOGGING
    if (sTinyTurns != 0 && PortOcDue(sTinyLast, sTinyTick)) {
        port_log_summary("tinycull: %u turns, per turn %.1f actors under %.1f lines, %.1f of them not drawn%s, %.2f drawn again; left out: player %.2f, free joints %.2f",
                         sTinyTurns, static_cast< double >(sTinyUnder) / sTinyTurns, static_cast< double >(TinyCullLines()),
                         static_cast< double >(sTinyCulled) / sTinyTurns, TinyCullDry() ? " (GXV_TINYCULL_DRY: none)" : "",
                         static_cast< double >(sTinyBack) / sTinyTurns, static_cast< double >(sTinyPlayer) / sTinyTurns,
                         static_cast< double >(sTinyFree) / sTinyTurns);
        sTinyTurns = sTinyUnder = sTinyCulled = sTinyBack = sTinyPlayer = sTinyFree = 0;
    }
#endif
}

void ZoomCullLog() {
#if PORT_LOGGING
    if (sZoomTurns != 0 && PortOcDue(sZoomLast, sZoomTick)) {
        port_log_summary("zoomclip: %u turns, per turn %.1f actors outside the zoomed picture kept running (clipped by 2.2.1), %.1f of them "
                         "not drawn (GXV_ZOOM_DRAWCULL)",
                         sZoomTurns, static_cast< double >(sZoomKept) / sZoomTurns, static_cast< double >(sZoomCulled) / sZoomTurns);
        sZoomTurns = sZoomKept = sZoomCulled = 0;
    }
#endif
}
}  // namespace
#endif

#ifdef TARGET_PC
namespace {
// (popin25, the census) the game's own clipping, by what hides the actor: a FAR LEVEL (the level's far plane, 5000 .. 60000 from the camera
// plane, while the view's sides and the main far plane would show it) or the frustum's edge.
int PortOrigKind(const ClippingJudge* pJudge, const ClippingActorInfo* pInfo, s32 level) {
    return level != 0 && pJudge->mFrustum.mayIntersectBall3(*pInfo->_4, pInfo->_8) ? port::popin::kOrigFar : port::popin::kOrigClip;
}
}  // namespace
float PortDecorScale(const ClippingActorInfo* pInfo);  // (ClippingActorInfo.cpp)
extern "C" void port_popin_event_unit(int kind, int appear, const LiveActor* pActor, const TVec3f* pPos, float radius, float unitRadius, float unitNearer);
namespace {
// (popin31, the census) a clip event; a flower / bush group's carries its ONE member's size too (port::popin::PlantMember), what the floor measures
void PortClipEvent(int kind, int appear, const ClippingActorInfo* pInfo) {
    if (!pInfo->mPbDecor || !port_safe_cam_exists()) {
        port_popin_event(kind, appear, pInfo->mActor, pInfo->_4, pInfo->_8);
        return;
    }
    const TVec3f cam = MR::getCamPos();
    const TVec3f& c = *pInfo->_4;
    const float dx = c.x - cam.x, dy = c.y - cam.y, dz = c.z - cam.z;
    const float d = sqrtf(dx * dx + dy * dy + dz * dz);
    float nearest = 0.0f;
    const float memberR = port::popin::PlantMember(d, pInfo->_8, PortDecorScale(pInfo), &nearest);
    port_popin_event_unit(kind, appear, pInfo->mActor, pInfo->_4, pInfo->_8, memberR, d - nearest);
}
}  // namespace
#endif


#ifdef TARGET_PC
// ---------------------------------------------------------------------------------------------------------------------------------------
// cliprange25 (11 Nov; every switch default 0; the pure parts and their host test: port/cliprange.h, tests/cliprange.cpp)
//
// GXV_CLIPCENSUS=1: what a tighter clip could save. Every ~5 s, one group of "clipcensus:" lines per stage (logging builds): per turn, the actors
// alive (in the game's clipping lists: running, or clipped by the game), the actors whose movement ran (LiveActor::movement entered), whose calcAnim
// ran / posed joints, and that are drawn (not clipped, a model, not hidden, not stamped off), split into the distance bands from the PLAYER and into the
// kinds the actor's own parts give (port::cliprange::Kind), with the summed time of movement and calcAnim per band / kind / list category. Counts are
// every turn; the time is read on one turn in GXV_CLIPCENSUS_SAMPLE (4) and corrected by the cost of the clock reads (as GXV_LOGIC_PROF=3 does).
// GXV_CLIPCENSUS=2 adds the DRY counts of the levers below at their current strength (nothing is changed by them).
//
// The levers act only on PURE DECORATION (port::cliprange::PureDecor: a model; no collision of its own; no hit sensor; no effect keeper, sound object,
// stage switch, rail, camera controller or star-pointer target; not a flower / bush member; not the player), only beyond GXV_CR_NEAR (2500) from the player AND from the camera
// (the sphere's centre less its radius), only while a camera and the player exist, no mirror camera shows actors off the picture, and the popin25
// flight rule (GXV_PB_DEMO) is not holding the budget off. Zoom and the fast-camera lead of the budget (popin25) widen the distances as for PB_DECOR.
//   GXV_CLIPSCALE=<pct> (1..99): such an actor is clipped (the game's own startClipped: no movement, no pose, no draw) beyond pct % of its far-clip
//       distance (the level's, 5000 .. 60000, or the far plane's), spread +-10 % per actor (a crowd does not leave at one distance); an actor still over
//       GXV_CLIPSCALE_LINES (14) lines big at that distance keeps going until it is not (never later than the game's own limit).
//   GXV_FARDRAW=<lines> (1..64): such an actor whose clipping sphere is under <lines> lines (of 456) is not drawn (the draw-only stamp of GXV_TINYCULL /
//       zoom / horizon: movement, pose, shadow, effects as before); each actor's own threshold lies between lines and lines * (1 + GXV_FARDRAW_RAMP (50) %).
//   GXV_FARANIM=<n> (2..4): LiveActor.cpp (PbAnimSkips): far small decoration poses its joints every n-th turn.
// Hysteresis: GXV_CR_HYST (6 %). The class of an actor is read once and again every 32 turns. Needs GXV_OC_CLIPFAST=1 (the loop in movement()).
// Logging builds write, about every 5 s while a lever is on:  cliprange: ...   (counts a turn)
extern "C" {
int port_cc_flag = 0;  // bit 0: GXV_CLIPCENSUS != 0 (NameObjCategoryList.cpp: the timed list loop), bit 1: far30 GXV_DISTCENSUS (LiveActor.cpp reads the whole word: one load and a branch per object)
}
extern "C" const TVec3f* port_safe_player_pos(void);
extern "C" int port_pb_demo_off(void);
extern "C" int port_cpugov_level(void);  // (honey25, platform/src/port/perf.cpp: GXV_CPUGOV's level, 0 = off)
extern "C" int port_on_frame_thread(void);
extern "C" int port_anim_lod_raw_category(void);
extern "C" float port_pb_scale(void);
extern "C" float port_pb_lead(float limit);
bool port_live_actor_valid(const LiveActor*);
namespace {
using namespace port::cliprange;
int CcModeRaw() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_CLIPCENSUS");
        const int v = e && *e ? static_cast< int >(strtol(e, nullptr, 0)) : 0;
        return v < 0 ? 0 : (v > 2 ? 2 : v);
    }());
}
float CrEnvF(const char* pName, float def, float lo, float hi) {
    const char* e = getenv(pName);
    float v = e && *e ? static_cast< float >(strtod(e, nullptr)) : def;
    float stageValue;
    if (port::stageknob::Lookup(pName, &stageValue)) {  // (stagepreset25: the stage's value first)
        v = stageValue;
    }
    return v < lo ? lo : (v > hi ? hi : v);
}
float CrNear() { return PORT_STAGE_KNOB(float, CrEnvF("GXV_CR_NEAR", 2500.0f, 500.0f, 100000.0f)); }  // (cpucuts25: stage-settable)
float CrHyst() { return PORT_HOT_KNOB(float, CrEnvF("GXV_CR_HYST", 6.0f, 0.0f, 30.0f) * 0.01f); }
int CrClipPct() {
    return PORT_STAGE_KNOB(int, [] { const float v = CrEnvF("GXV_CLIPSCALE", 0.0f, 0.0f, 1000.0f); return v >= 1.0f && v < 100.0f ? static_cast< int >(v) : 0; }());
}
float CrClipLines() { return PORT_HOT_KNOB(float, CrEnvF("GXV_CLIPSCALE_LINES", 14.0f, 0.0f, 200.0f)); }
float CrDrawLinesStatic() {
    return PORT_STAGE_KNOB(float, [] { const float v = CrEnvF("GXV_FARDRAW", 0.0f, 0.0f, 1000.0f); return v >= 1.0f ? (v > 64.0f ? 64.0f : v) : 0.0f; }());
}
// honey25 (GXV_CPUGOV, port/cpugov.h): the far-draw rule's four inputs are the static values (above, and below) combined with the governor's level
// (platform/src/port/perf.cpp; 0 = off or calm: the static values exactly as they were, one relaxed load a call).
extern "C" float port_q30lock_far_ramp(void);  // (q30lock26: step 3b's ramp, 0..1 over a second; 1 outside the Quality lock)
float CrDrawLines() {
    const float b = CrDrawLinesStatic();
    const int g = port_cpugov_level();
    if (g <= 0) return b;
    const float lines = port::cpugov::Combine(b, 0, 0, 0, g).lines;
    return lines * port_q30lock_far_ramp();   // (the size under which a far speck may be left out grows from nothing to its full value over the ramp: the smallest go first)
}
float CrDrawRamp() { return PORT_STAGE_KNOB(float, CrEnvF("GXV_FARDRAW_RAMP", 50.0f, 0.0f, 300.0f)); }  // (cpucuts25: stage-settable)
// cpucuts25 (stage-settable; the defaults are the cliprange25 behaviour exactly): the classes GXV_FARDRAW may take (port::cpucuts::ClassBit; 1 = pure decoration only),
// the minimum age of a stamp before it may be released (turns), and whether the pose flush leaves a stamped model out.
int CrDrawKindsStatic() {
    return PORT_STAGE_KNOB(int, [] {
        const int v = static_cast< int >(CrEnvF("GXV_FARDRAW_KINDS", 1.0f, 0.0f, 255.0f)) & port::cpucuts::kCbAll;
        return v != 0 ? v : 1;
    }());
}
int CrDrawHoldStatic() { return PORT_STAGE_KNOB(int, static_cast< int >(CrEnvF("GXV_FARDRAW_HOLD", 0.0f, 0.0f, 240.0f))); }
int CrDrawFlushStatic() { return PORT_STAGE_KNOB(int, CrEnvF("GXV_FARDRAW_FLUSH", 0.0f, 0.0f, 1.0f) >= 1.0f ? 1 : 0); }
// (GXV_CPUGOV: with a level, the union of the classes, the longer hold and the flush skip; see port::cpugov::Combine)
int CrDrawKinds() {
    const int b = CrDrawKindsStatic();
    const int g = port_cpugov_level();
    return g > 0 ? port::cpugov::Combine(CrDrawLinesStatic(), b, 0, 0, g).kinds : b;
}
int CrDrawHold() {
    const int b = CrDrawHoldStatic();
    const int g = port_cpugov_level();
    return g > 0 ? port::cpugov::Combine(CrDrawLinesStatic(), 0, b, 0, g).hold : b;
}
int CrDrawFlush() {
    const int b = CrDrawFlushStatic();
    const int g = port_cpugov_level();
    return g > 0 ? port::cpugov::Combine(CrDrawLinesStatic(), 0, 0, b, g).flush : b;
}
int CcSample() { return PORT_HOT_KNOB(int, static_cast< int >(CrEnvF("GXV_CLIPCENSUS_SAMPLE", 4.0f, 1.0f, 64.0f))); }

Census gCc;
bool sCcTimed = false;       // this turn's object times are read
uint32_t sCcTimedTurns = 0;  // turns of the window that were timed
uint32_t sCcTurnNo = 0;
int64_t sCcWinT0 = 0;
uint32_t sCcWinPic0 = 0;
uint64_t sCcPassNs = 0, sCcPassTurns = 0;
int64_t sCcReadNs = 0;  // the cost of one clock read (measured at each window's start)
// the levers' own counts (window sums; per-turn means in the "cliprange:" line)
uint32_t sCrTurns = 0, sCrClipHeld = 0, sCrClipNew = 0, sCrClipBack = 0, sCrDrawHeld = 0, sCrDrawShapes = 0, sCrDrawNew = 0, sCrKindRead = 0, sCrDecor = 0;
uint32_t sCrDrawHeldOn = 0, sCrFlushSkip = 0;  // (cpucuts25) stamps kept by the hold; models left out of the pose flush
int64_t sCrLast = 0;
uint32_t sCrTick = 0;

// what the actor's own parts say it is
Facts CrFacts(const LiveActor* a, const ClippingActorInfo* pInfo, bool player) {
    Facts f;
    f.hasModel = a->mModelManager != nullptr && MR::getJ3DModel(a) != nullptr;
    f.collision = a->mCollisionParts != nullptr;
    f.sensors = a->mSensorKeeper != nullptr && a->mSensorKeeper->mSensorCount > 0;
    f.effects = a->mEffectKeeper != nullptr;
    f.sound = a->mSoundObject != nullptr;
    f.stageSwitch = a->mStageSwitchCtrl != nullptr;
    f.rail = a->mRailRider != nullptr;
    f.camCtrl = a->mCameraCtrl != nullptr;
    f.pointer = a->mStarPointerTarget != nullptr;
    f.freeJoints = a->mLogicFreeJoints != 0;
    f.pbDecor = pInfo != nullptr && pInfo->mPbDecor;
    f.player = player;
    return f;
}
// The sensor classes of an actor (cpucuts25, port::cpucuts::ClassOfSensors): what kinds of hit sensor it carries. More than 16 sensors or a sensor
// whose record is missing counts as a kind the cut never takes.
uint8_t CrSensorBits(const LiveActor* a) {
    const HitSensorKeeper* k = a->mSensorKeeper;
    if (k == nullptr || k->mSensorCount <= 0 || k->mSensorInfos == nullptr) {
        return 0;
    }
    if (k->mSensorCount > 16) {
        return port::cpucuts::kSensorKeep;
    }
    uint32_t types[16];
    int n = 0;
    for (s32 i = 0; i < k->mSensorCount; ++i) {
        const HitSensorInfo* si = k->mSensorInfos[i];
        types[n++] = (si != nullptr && si->mSensor != nullptr) ? si->mSensor->mType : 0xFFFFu;
    }
    return port::cpucuts::ClassOfSensors(types, n);
}
void CrReadClass(ClippingActorInfo* pInfo, const LiveActor* pPlayer) {
    const Facts facts = CrFacts(pInfo->mActor, pInfo, pInfo->mActor == pPlayer);
    pInfo->mCrKind = static_cast< u8 >(Classify(facts));
    // (prog32b: GXV_FARDRAW_OBJECTIVE, default 1: an actor a star can depend on is never a "speck": port/cpucuts.h ObjectiveName / ClassOfActorGuarded)
    static const bool objectiveGuard = CrEnvF("GXV_FARDRAW_OBJECTIVE", 1.0f, 0.0f, 1.0f) >= 1.0f;
    pInfo->mCrCls = port::cpucuts::ClassOfActorGuarded(pInfo->mCrKind, pInfo->mCrKind == kSens ? CrSensorBits(pInfo->mActor) : 0,
                                                       facts.stageSwitch || facts.camCtrl || facts.pointer, facts.rail, pInfo->mActor->getName(), objectiveGuard);
}
// The lever's cached class (a pointer chase per actor per turn is what this avoids: read once, then every 32 turns, the phase spread by address).
int CrKindOf(ClippingActorInfo* pInfo, const LiveActor* pPlayer) {
    if (pInfo->mCrKind == 0xFF) {
        pInfo->mCrAge = static_cast< u8 >((reinterpret_cast< uintptr_t >(pInfo) >> 4) & 31);
        CrReadClass(pInfo, pPlayer);
        ++sCrKindRead;
    } else if ((++pInfo->mCrAge & 31) == 0) {
        CrReadClass(pInfo, pPlayer);
        ++sCrKindRead;
    }
    return pInfo->mCrKind;
}
// May GXV_FARDRAW take this actor (its class is in GXV_FARDRAW_KINDS; the default 1 is "pure decoration", what the lever took before)?
inline bool CrDrawAllowed(ClippingActorInfo* pInfo, const LiveActor* pPlayer, unsigned kinds) {
    CrKindOf(pInfo, pPlayer);
    return port::cpucuts::Allowed(pInfo->mCrCls, kinds);
}

// The turn's environment for the levers and the census.
struct CrEnv {
    bool haveView = false;  // a camera and the player exist (census distances)
    bool lever = false;     // the levers may act this turn
    TVec3f cam{0.0f, 0.0f, 0.0f}, player{0.0f, 0.0f, 0.0f};
    float tanHalf = 0.0f, zoom = 1.0f, nearD = 2500.0f, hyst = 0.06f, farZ = 0.0f;
    int clipPct = 0;
    float clipLines = 14.0f, drawLines = 0.0f, drawRamp = 50.0f;
    const ClippingJudge* judge = nullptr;
    const LiveActor* playerActor = nullptr;
    u32 next = 0;
};
inline float D2(const TVec3f& a, const TVec3f& b) {
    const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return x * x + y * y + z * z;
}
// far30 (GXV_FARFIELD tier 4, the draw distance): perf.cpp's horizon for small decoration and props (game units, 0 = off), fed by the governors' depth through farfield::Plan.
extern "C" float port_ff_draw_horizon(void);
extern "C" int port_ff_draw_last(void);
extern "C" int port_ff_flyover(void);   // (far3: LodCtrl.cpp)
extern "C" float port_ff_draw_px(void);  // (perf.cpp: the height limit in pixels)
uint64_t sFfHeld = 0, sFfFaded = 0, sFfPop = 0;
float sFfPopPx = 0.0f;   // (the largest object that popped in the window, its height in pixels of 544)   // (census: draws held by the far-draw stamp; objects the draw distance wants gone, summed over the turns of a window)
CrEnv CrBegin(int cc, int clipPct, float drawLines, const ClippingJudge* pJudge, bool extraLever = false) {
    CrEnv e;
    const bool anyLever = clipPct > 0 || drawLines > 0.0f || extraLever;  // (cpulod26: GXV_PIECETHIN is a lever of its own)
    if (cc == 0 && !anyLever) {
        return e;
    }
    if (!port_safe_cam_exists()) {
        return e;
    }
    const TVec3f* pp = port_safe_player_pos();
    if (pp == nullptr) {
        return e;
    }
    e.haveView = true;
    e.cam = MR::getCamPos();
    e.player = *pp;
    e.tanHalf = tanf(MR::getFovy() * 0.5f * 3.14159265f / 180.0f);
    e.zoom = port_pb_scale();
    if (!(e.zoom > 0.1f)) {
        e.zoom = 1.0f;
    }
    e.nearD = CrNear();
    e.hyst = CrHyst();
    e.farZ = MR::getFarZ();
    e.clipPct = clipPct;
    e.clipLines = CrClipLines();
    e.drawLines = drawLines;
    e.drawRamp = CrDrawRamp();
    e.judge = pJudge;
    e.playerActor = reinterpret_cast< const LiveActor* >(MarioAccess::getPlayerActor());
    e.next = J3DPortPicture() + 1;
    e.lever = anyLever && !MR::isExistMirrorCamera() && !port_pb_demo_off();
    return e;
}
// the limit (camera distance of the sphere's centre less its radius) beyond which the clip lever holds the actor clipped
inline float CrClipLimitOf(const CrEnv& e, const ClippingActorInfo* pInfo, int level) {
    const float base = level == 0 ? e.farZ : e.judge->mClipDistances[level];
    const float lim = ClipLimit(base * e.zoom, static_cast< float >(e.clipPct), Jitter01(reinterpret_cast< uintptr_t >(pInfo->mActor)), pInfo->_8, e.tanHalf,
                                e.clipLines, e.nearD * e.zoom);
    return lim + port_pb_lead(lim);  // (popin25: a fast camera sees things appear earlier; 0 when off)
}
// the player's gate: at least near + radius from the player (hysteresis the other way: entering the lever's reach takes 6 % more than leaving it)
inline bool CrPlayerFar(const CrEnv& e, float plD, float radius, bool was) {
    return plD > e.nearD * e.zoom * (was ? 1.0f : 1.0f + e.hyst) + radius;
}
inline int CrLevel(const ClippingActorInfo* pInfo) {
    const int level = (pInfo->_14 != nullptr && pInfo->_14->_0 == 1) ? 0 : pInfo->mFarClipLevel;
    return level >= 0 && level < 8 ? level : 0;
}
// FARDRAW's stamp: taken back when this actor is no longer culled by the lever (never another cull's stamp)
void CrDrawRelease(ClippingActorInfo* pInfo, u32 next) {
    if (pInfo->mCrDraw == 0) {
        return;
    }
    pInfo->mCrDraw = 0;
    J3DModel* pModel = pInfo->mActor != nullptr ? MR::getJ3DModel(pInfo->mActor) : nullptr;
    if (pModel != nullptr) {
        pModel->mPortFarCulled = 0;  // (cpucuts25: the pose flush poses it again)
    }
    if (pModel != nullptr && pModel->mPortCullPicture == next && pModel->mPortTinyCulled == 0 && !pInfo->mPortZoomCulled && pInfo->mPortHorizon == 0) {
        pModel->mPortCullPicture = 0;
    }
}

// an actor leaves the judged list: both levers let go of it
void CrRelease(ClippingActorInfo* pInfo, u32 next) {
    CrDrawRelease(pInfo, next);
    pInfo->mCrClip = 0;
}

// ---- the census ----
const char* const kMvName[kMoveCats] = {"0x00", "StopSceneDelay", "Camera", "ScreenEffect", "ClippingDirector", "SensorHitChecker", "MsgSharedGroup", "0x07", "0x08",
    "AudCameraWatcher", "TalkDirector", "DemoDirector", "0x0C", "AreaObj", "Layout", "LayoutDecoration", "Movie", "WipeLayout", "LayoutOnPause", "MovieSubtitles",
    "0x14", "0x15", "MirrorCamera", "ImageEffect", "AudEffectDirector", "AudBgmConductor", "CameraCover", "SwitchWatcherHolder", "ClippedMapParts", "Planet",
    "CollisionMapObj", "CollisionEnemy", "CollisionDirector", "Environment", "MapObj", "MapObjDecoration", "Sky", "Player", "PlayerDecoration", "PlayerMessenger",
    "NPC", "Ride", "Enemy", "EnemyDecoration", "Item", "ShadowControllerHolder", "0x2E", "0x2F"};
const char* const kCaName[kCalcCats] = {"ClippedMapParts", "Planet", "CollisionMapObj", "CollisionEnemy", "Environment", "MapObj", "NPC", "Ride", "Enemy", "Player",
    "PlayerDecoration", "MapObjDecoration", "MirrorMapObj", "Layout", "LayoutDecoration", "MovieSubtitles", "Item", "0x11", "0x12", "AnimParticle",
    "AnimParticleIgnorePause", "0x15", "0x16", "0x17"};

// (the lists that hold LiveActors; the snapshot check in port_cc_obj_begin is what makes a cast safe)
bool CcLiveList(int raw) {
    if (raw >= 0x100) {
        const int t = raw - 0x100;
        return (t >= 0x1C && t <= 0x1F) || (t >= 0x21 && t <= 0x26) || (t >= 0x28 && t <= 0x2C);
    }
    return (raw >= 0 && raw <= 0xC) || raw == 0x10;
}
const NameObj* sCcObj = nullptr;
int sCcRaw = -1, sCcDepth = 0;
bool sCcMoved = false, sCcCalcIn = false, sCcPosed = false;

int CcKindOfActor(const LiveActor* a) {
    if (a->mCrKindA != 0xFF) {
        return a->mCrKindA;
    }
    return static_cast< int >(Classify(CrFacts(a, nullptr, a == reinterpret_cast< const LiveActor* >(MarioAccess::getPlayerActor()))));
}

void CcMeasureRead() {
    // the clock read costs (the console's is ~0.7 us): the rows are corrected by it
    const int64_t t0 = port_host_time_ns();
    int64_t x = 0;
    for (int i = 0; i < 64; ++i) {
        x += port_host_time_ns();
    }
    const int64_t t1 = port_host_time_ns();
    sCcReadNs = (t1 - t0) / 65;
    (void)x;
}

#if PORT_LOGGING
// the time of `listed` timed objects, the clock reads inside it taken off (each timed call carries about one read), in us per timed turn
double CcUs(uint64_t ns, uint64_t listedT, double timedTurns) {
    const double v = static_cast< double >(ns) - static_cast< double >(listedT) * static_cast< double >(sCcReadNs);
    return (v > 0.0 ? v : 0.0) / timedTurns * 1e-3;
}
#endif

void CcLog() {
#if PORT_LOGGING
    Census& c = gCc;
    if (c.turns == 0) {
        return;
    }
    const double n = static_cast< double >(c.turns);
    const double nt = sCcTimedTurns != 0 ? static_cast< double >(sCcTimedTurns) : 1.0;
    const double secs = static_cast< double >(port_host_time_ns() - sCcWinT0) * 1e-9;
    auto sumK = [&](uint64_t Cell::*m, int k) {
        uint64_t t = 0;
        for (int b = 0; b < kBands; ++b) t += c.cell[k][b].*m;
        return t;
    };
    auto sumAll = [&](uint64_t Cell::*m) {
        uint64_t t = 0;
        for (int k = 0; k < kKinds; ++k) t += sumK(m, k);
        return t;
    };
    port_log_summary("clipcensus: stage %s: %u turns (%u pictures) in %.1f s, time read on %u; a turn: alive %.0f (clipped by the game %.0f), movement ran %.0f of %.0f listed "
                     "(%.0f us), calcAnim entered %.0f posed %.0f of %.0f listed (%.0f us), drawn %.0f actors = %.0f shapes; not an actor %.1f; census pass %.3f ms a turn, clock read %.2f us",
                     MR::getCurrentStageName(), c.turns, c.pictures, secs, sCcTimedTurns, static_cast< double >(sumAll(&Cell::alive)) / n,
                     static_cast< double >(sumAll(&Cell::clipped)) / n, static_cast< double >(sumAll(&Cell::mvRan)) / n, static_cast< double >(sumAll(&Cell::mvListed)) / n,
                     CcUs(sumAll(&Cell::mvNs), sumAll(&Cell::mvListedT), nt), static_cast< double >(sumAll(&Cell::caRan)) / n,
                     static_cast< double >(sumAll(&Cell::caPosed)) / n, static_cast< double >(sumAll(&Cell::caListed)) / n,
                     CcUs(sumAll(&Cell::caNs), sumAll(&Cell::caListedT), nt), static_cast< double >(sumAll(&Cell::drawn)) / n,
                     static_cast< double >(sumAll(&Cell::shapes)) / n, static_cast< double >(c.unknownListed) / n,
                     sCcPassTurns ? static_cast< double >(sCcPassNs) / static_cast< double >(sCcPassTurns) * 1e-6 : 0.0, static_cast< double >(sCcReadNs) * 1e-3);
    char buf[640];
    for (int k = -1; k < kKinds; ++k) {  // one set of lines for all kinds (-1), then one per kind that has any actor
        if (k >= 0 && sumK(&Cell::alive, k) == 0 && sumK(&Cell::mvListed, k) == 0) {
            continue;
        }
        int o = snprintf(buf, sizeof buf, "clipcensus: %s by player distance %s|%s|%s|%s|%s, a turn:", k < 0 ? "ALL" : KindName(k), BandName(0), BandName(1), BandName(2),
                         BandName(3), BandName(4));
        auto put = [&](const char* name, uint64_t Cell::*m) {
            o += snprintf(buf + o, sizeof buf - static_cast< size_t >(o), " %s", name);
            for (int b = 0; b < kBands; ++b) {
                uint64_t t = 0;
                for (int kk = 0; kk < kKinds; ++kk) {
                    if (k < 0 || k == kk) t += c.cell[kk][b].*m;
                }
                o += snprintf(buf + o, sizeof buf - static_cast< size_t >(o), "%s%.1f", b ? "/" : " ", static_cast< double >(t) / n);
            }
        };
        put("alive", &Cell::alive);
        put("clipped", &Cell::clipped);
        put("moved", &Cell::mvRan);
        put("posed", &Cell::caPosed);
        put("drawn", &Cell::drawn);
        put("shapes", &Cell::shapes);
        port_log_summary("%s", buf);
        int o2 = snprintf(buf, sizeof buf, "clipcensus: %s us a turn (timed turns, clock reads taken off):", k < 0 ? "ALL" : KindName(k));
        for (int pass = 0; pass < 2; ++pass) {
            o2 += snprintf(buf + o2, sizeof buf - static_cast< size_t >(o2), " %s", pass == 0 ? "movement" : "calcAnim");
            for (int b = 0; b < kBands; ++b) {
                uint64_t ns = 0, listedT = 0;
                for (int kk = 0; kk < kKinds; ++kk) {
                    if (k < 0 || k == kk) {
                        ns += pass == 0 ? c.cell[kk][b].mvNs : c.cell[kk][b].caNs;
                        listedT += pass == 0 ? c.cell[kk][b].mvListedT : c.cell[kk][b].caListedT;
                    }
                }
                o2 += snprintf(buf + o2, sizeof buf - static_cast< size_t >(o2), "%s%.0f", b ? "/" : " ", CcUs(ns, listedT, nt));
            }
        }
        port_log_summary("%s", buf);
    }
    {
        int o = snprintf(buf, sizeof buf, "clipcensus: movement lists a turn (listed/ran, us):");
        for (int i = 0; i < kMoveCats && o < static_cast< int >(sizeof buf) - 70; ++i) {
            const CatRow& r = c.mv[i];
            if (r.listed != 0) {
                o += snprintf(buf + o, sizeof buf - static_cast< size_t >(o), " %s %.1f/%.1f %.0f;", kMvName[i], static_cast< double >(r.listed) / n,
                              static_cast< double >(r.ran) / n, CcUs(r.ns, r.listedT, nt));
            }
        }
        port_log_summary("%s", buf);
        o = snprintf(buf, sizeof buf, "clipcensus: calcAnim lists a turn (listed/entered/posed, us):");
        for (int i = 0; i < kCalcCats && o < static_cast< int >(sizeof buf) - 70; ++i) {
            const CatRow& r = c.ca[i];
            if (r.listed != 0) {
                o += snprintf(buf + o, sizeof buf - static_cast< size_t >(o), " %s %.1f/%.1f/%.1f %.0f;", kCaName[i], static_cast< double >(r.listed) / n,
                              static_cast< double >(r.ran) / n, static_cast< double >(r.posed) / n, CcUs(r.ns, r.listedT, nt));
            }
        }
        port_log_summary("%s", buf);
    }
    {
        int o = snprintf(buf, sizeof buf, "clipcensus: pure decoration the game has not clipped, a turn: by far-clip level 0..7:");
        for (int l = 0; l < 8; ++l) o += snprintf(buf + o, sizeof buf - static_cast< size_t >(o), "%s%.1f", l ? "/" : " ", static_cast< double >(c.farClass[l]) / n);
        o += snprintf(buf + o, sizeof buf - static_cast< size_t >(o), "; beyond %.0f from player AND camera, by diameter in lines:", static_cast< double >(CrNear()));
        for (int b = 0; b < kSizeBuckets; ++b) {
            o += snprintf(buf + o, sizeof buf - static_cast< size_t >(o), " %s: %.1f actors %.1f shapes;", SizeBucketName(b), static_cast< double >(c.decFar[b]) / n,
                          static_cast< double >(c.decFarShapes[b]) / n);
        }
        port_log_summary("%s", buf);
        if (CcModeRaw() >= 2) {
            port_log_summary("clipcensus: DRY (nothing changed) a turn: GXV_CLIPSCALE=%d would clip %.1f actors (%.1f shapes); GXV_FARDRAW=%.0f would not draw %.1f actors (%.1f shapes); "
                             "GXV_FARANIM candidates (pure decoration beyond near, under 30 lines, no collision) %.1f actors",
                             CrClipPct(), static_cast< double >(c.dryClip.actors) / n, static_cast< double >(c.dryClip.shapes) / n, static_cast< double >(CrDrawLines()),
                             static_cast< double >(c.dryDraw.actors) / n, static_cast< double >(c.dryDraw.shapes) / n, static_cast< double >(c.dryAnim.actors) / n);
        }
    }
#endif
}

// one turn's pass over a clipping list (run once ClippingActorHolder::movement has judged them)
void CcPassList(const ClippingActorInfoList* pList, const CrEnv& e, int cc) {
    Census& c = gCc;
    const float near2 = e.nearD * e.nearD;
    for (s32 i = 0; i < pList->_4; ++i) {
        ClippingActorInfo* pInfo = pList->mClippingActorList[i];
        LiveActor* a = pInfo != nullptr ? pInfo->mActor : nullptr;
        if (a == nullptr || pInfo->_4 == nullptr) {
            continue;
        }
        const TVec3f& pos = *pInfo->_4;
        const float pd2 = D2(pos, e.player);
        const int band = BandOf(pd2);
        const Facts f = CrFacts(a, pInfo, a == e.playerActor);
        const Kind kind = Classify(f);
        a->mCrKindA = static_cast< u8 >(kind);
        Cell& cell = c.cell[kind][band];
        ++cell.alive;
        if (MR::isClipped(a)) {
            ++cell.clipped;
            continue;
        }
        J3DModel* pModel = f.hasModel ? MR::getJ3DModel(a) : nullptr;
        u32 shapes = 0;
        if (pModel != nullptr && !MR::isHiddenModel(a) && pModel->mPortCullPicture != e.next) {
            const J3DModelData* pData = pModel->getModelData();
            shapes = pData != nullptr ? pData->getShapeNum() : 0;
            ++cell.drawn;
            cell.shapes += shapes;
        }
        if (kind != kDecor) {
            continue;
        }
        const int level = CrLevel(pInfo);
        ++c.farClass[level];
        const float cd2 = D2(pos, e.cam);
        if (!(pd2 > near2) || !(cd2 > near2)) {
            continue;
        }
        const float camD = sqrtf(cd2), plD = sqrtf(pd2);
        const float lines = ProjLines(pInfo->_8, camD, e.tanHalf);
        const int sb = SizeBucket(lines);
        ++c.decFar[sb];
        c.decFarShapes[sb] += shapes;
        if (cc >= 2) {
            if (CrClipPct() > 0 && camD - pInfo->_8 > CrClipLimitOf(e, pInfo, level) && CrPlayerFar(e, plD, pInfo->_8, false)) {
                ++c.dryClip.actors;
                c.dryClip.shapes += shapes;
            }
            const float dl = CrDrawLines();
            if (dl > 0.0f && camD > e.nearD + pInfo->_8 && CrPlayerFar(e, plD, pInfo->_8, false) &&
                DrawCulled(lines, DrawThreshold(dl, e.drawRamp, Jitter01(reinterpret_cast< uintptr_t >(a))), false, 0.0f)) {
                ++c.dryDraw.actors;
                c.dryDraw.shapes += shapes;
            }
            if (lines < 30.0f && a->mCollisionParts == nullptr) {
                ++c.dryAnim.actors;
            }
        }
    }
}

// ---- far30: GXV_DISTCENSUS=1 (default 0; counts only, no clock; port/distcensus.h says what each counter means and what is not counted) ----
// Decision points used, nothing added to the game's own loops: LiveActor::movement's and calcAnim's existing GXV_CLIPCENSUS marks (port_cc_mark: logic ran, joints posed; the
// pose flush's calcAnim reaches the same mark), the clipping lists pass of the draw census below (draws, indices, screen size), JPAEmitterManager::calcNormal (particles),
// ShadowControllerHolder::updateController (shadows). Distances are to the CAMERA (the actor's position; the emitter's origin).
port::distcensus::Table gDist, gDistWin, gDistStage;   // this turn's tallies / the 5 s window / the stage so far
port::distcensus::ModelIdx gDistMi;
TVec3f sDistCam(0.0f, 0.0f, 0.0f);
bool sDistCamOk = false;
float sDistTan = 0.0f;
int64_t sDistWinT0 = 0;
char sDistStage[48] = {};
int DcModeRaw();   // (defined with the draw census below)
int DistOnRaw() { return (DcModeRaw() & 2) != 0 ? 1 : 0; }
bool DistCam() {
    if (!sDistCamOk) {
        if (!port_safe_cam_exists()) {
            return false;
        }
        sDistCam = MR::getCamPos();
        sDistCamOk = true;
    }
    return true;
}
struct DistClsEnt {
    const LiveActor* a;
    uint8_t cls, age;
};
DistClsEnt sDistCls[2048];
port::distcensus::Class DistClassOf(const LiveActor* a, bool player) {
    DistClsEnt& e = sDistCls[(reinterpret_cast< uintptr_t >(a) >> 4) & 2047u];
    if (e.a == a && e.age < 40) {
        ++e.age;
        return static_cast< port::distcensus::Class >(e.cls);
    }
    const int kind = static_cast< int >(Classify(CrFacts(a, nullptr, player)));
    port::distcensus::Class sens = port::distcensus::kOther;
    if (kind == kSens) {
        const HitSensorKeeper* k = a->mSensorKeeper;
        uint32_t types[16];
        int n = 0;
        if (k != nullptr && k->mSensorCount > 0 && k->mSensorInfos != nullptr) {
            for (s32 i = 0; i < k->mSensorCount && n < 16; ++i) {
                const HitSensorInfo* si = k->mSensorInfos[i];
                types[n++] = (si != nullptr && si->mSensor != nullptr) ? si->mSensor->mType : 0xFFFFu;
            }
        }
        sens = port::distcensus::ClassOfSensorTypes(types, n);
    }
    const port::distcensus::Class c = port::distcensus::ClassOfKind(kind, sens, player);
    e.a = a;
    e.cls = static_cast< uint8_t >(c);
    e.age = 0;
    return c;
}
// the indices of a model data's shapes (a display-list walk, once a model data)
void DistModelIdx(const J3DModelData* pData, uint32_t* shapes, uint32_t* indices) {
    if (gDistMi.Find(reinterpret_cast< uintptr_t >(pData), shapes, indices)) {
        return;
    }
    uint32_t nSh = pData->getShapeNum(), idx = 0;
    for (uint32_t i = 0; i < nSh; ++i) {
        J3DShape* sh = pData->getShapeNodePointer(static_cast< u16 >(i));
        if (sh == nullptr || sh->getVtxDesc() == nullptr) {
            continue;
        }
        static const u32 kSize[] = {0, 1, 1, 2};
        u32 stride = 0;
        for (GXVtxDescList* d = sh->getVtxDesc(); d->attr != GX_VA_NULL; d++) {
            stride += kSize[static_cast< u32 >(d->type) & 3u];
        }
        for (u16 g = 0; g < static_cast< u16 >(sh->getMtxGroupNum()); ++g) {
            J3DShapeDraw* sd = sh->getShapeDraw(g);
            if (sd != nullptr && sd->getDisplayList() != nullptr) {
                idx += port::distcensus::DlIndices(sd->getDisplayList(), sd->getDisplayListSize(), stride, nullptr);
            }
        }
    }
    gDistMi.Put(reinterpret_cast< uintptr_t >(pData), nSh, idx);
    *shapes = nSh;
    *indices = idx;
}
// one drawn model (the draw census's pass): camera distance squared, radius of the clipping sphere
void DistDraw(const LiveActor* a, const J3DModelData* pData, float cd2, float radius, float tanHalf, bool player) {
    uint32_t shapes = 0, indices = 0;
    DistModelIdx(pData, &shapes, &indices);
    const int px = port::distcensus::SizeOf(port::distcensus::RadiusPx(radius, sqrtf(cd2), tanHalf));
    gDist.Draw(port::distcensus::BandOf(cd2), DistClassOf(a, player), px, shapes, indices);
}
void DistLog(bool stageEnd) {
#if PORT_LOGGING
    using namespace port::distcensus;
    Table& t = stageEnd ? gDistStage : gDistWin;
    if (t.turns == 0) {
        return;
    }
    char name[64];
    if (stageEnd) {
        snprintf(name, sizeof name, "%s(stage)", sDistStage[0] ? sDistStage : "?");
    } else {
        snprintf(name, sizeof name, "%s", sDistStage[0] ? sDistStage : "?");
    }
    char buf[1100];
    const double secs = sDistWinT0 != 0 ? static_cast< double >(port_host_time_ns() - sDistWinT0) * 1e-9 : 0.0;
    FormatWin(buf, sizeof buf, name, t, stageEnd ? 0.0 : secs);
    port_log_summary("%s", buf);
    for (int b = 0; b < port::distcensus::kBands; ++b) {
        FormatBand(buf, sizeof buf, name, b, t);
        port_log_summary("%s", buf);
    }
#else
    (void)stageEnd;
#endif
}
// the end of the stage (the next scene's holder is made): the stage's means, once
void DistStageEnd() {
    if (gDistWin.turns != 0) {
        gDistStage.AddTable(gDistWin);
        gDistWin.Clear();
    }
    if (gDistStage.turns >= 120) {
        DistLog(true);
    }
    gDistStage.Clear();
    gDist.Clear();
    sDistCamOk = false;
    sDistWinT0 = 0;
    sDistStage[0] = 0;
}
// the actor's (position -> camera) band; false: no camera
bool DistBandOfPos(const TVec3f& p, int* band) {
    if (!DistCam()) {
        return false;
    }
    *band = port::distcensus::BandOf(D2(p, sDistCam));
    return true;
}

// ---- cpucuts25: the draw census (GXV_DRAWCENSUS=1; nothing is changed) ----
// What the drawers are given, by the actor's distance band from the CAMERA, the projected size of its clipping sphere (lines of 456) and its class and family
// (its name), with the estimated cost of cutting everything under L lines: game thread = shapes x GXV_DRAWCENSUS_US (12) [+ actors x GXV_DRAWCENSUS_ACTUS
// (25) when GXV_FARDRAW_FLUSH leaves the model out of the flush], front end = shapes x GXV_DRAWCENSUS_FEUS (40). Every ~5 s, "drawcensus:" lines. Cost: one
// pass over the clipping lists a turn (about the clipcensus pass), so it is for a measuring run. Far = beyond GXV_CR_NEAR (+ radius) from the camera AND the player.
// bit 0: GXV_DRAWCENSUS; bit 1: GXV_DISTCENSUS (far30; the same pass over the clipping lists gives it the draws)
int DcModeRaw() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_DRAWCENSUS");
        const char* d = getenv("GXV_DISTCENSUS");
        return (e && *e && strtol(e, nullptr, 0) != 0 ? 1 : 0) | (d && *d && strtol(d, nullptr, 0) != 0 ? 2 : 0);
    }());
}
double DcUs(const char* name, double def) { return static_cast< double >(CrEnvF(name, static_cast< float >(def), 0.0f, 1000.0f)); }
port::cpucuts::DrawCensus gDc;
int sDcFlag = 0;
int64_t sDcWinT0 = 0;
uint32_t sDcPic0 = 0;
void port_dc_init() {
    sDcFlag = DcModeRaw();
    gDc.Reset();
    sDcWinT0 = 0;
}
void DcPassList(const ClippingActorInfoList* pList, const CrEnv& e) {
    const float near2 = e.nearD * e.zoom * e.nearD * e.zoom;
    for (s32 i = 0; i < pList->_4; ++i) {
        ClippingActorInfo* pInfo = pList->mClippingActorList[i];
        LiveActor* a = pInfo != nullptr ? pInfo->mActor : nullptr;
        if (a == nullptr || pInfo->_4 == nullptr || MR::isClipped(a)) {
            continue;
        }
        J3DModel* pModel = a->mModelManager != nullptr ? MR::getJ3DModel(a) : nullptr;
        if (pModel == nullptr || MR::isHiddenModel(a)) {
            continue;
        }
        const J3DModelData* pData = pModel->getModelData();
        const u32 shapes = pData != nullptr ? pData->getShapeNum() : 0;
        if (pModel->mPortCullPicture == e.next) {  // (stamped off by a cull: not given to the drawers)
            ++gDc.stamped;
            gDc.stampedShapes += shapes;
            continue;
        }
        if (a == e.playerActor) {
            if ((sDcFlag & 2) != 0 && pData != nullptr) {
                DistDraw(a, pData, D2(*pInfo->_4, e.cam), pInfo->_8, e.tanHalf, true);
            }
            if ((sDcFlag & 1) != 0) {
                gDc.playerShapes += shapes;
            }
            continue;
        }
        const TVec3f& pos = *pInfo->_4;
        const float cd2 = D2(pos, e.cam), pd2 = D2(pos, e.player);
        if ((sDcFlag & 2) != 0 && pData != nullptr) {
            DistDraw(a, pData, cd2, pInfo->_8, e.tanHalf, false);
        }
        if ((sDcFlag & 1) == 0) {
            continue;
        }
        const float reach = e.nearD * e.zoom + pInfo->_8;
        const bool isFar = cd2 > reach * reach && pd2 > reach * reach;
        CrKindOf(pInfo, e.playerActor);
        gDc.Add(a->getName(), port::cpucuts::DcBandOfCamera(cd2), ProjLines(pInfo->_8, sqrtf(cd2), e.tanHalf), pInfo->mCrCls, shapes, isFar);
    }
}
void DcLog() {
#if PORT_LOGGING
    using namespace port::cpucuts;
    const DrawCensus& c = gDc;
    if (c.turns == 0) {
        return;
    }
    const double n = static_cast< double >(c.turns);
    const double secs = static_cast< double >(port_host_time_ns() - sDcWinT0) * 1e-9;
    const double usS = DcUs("GXV_DRAWCENSUS_US", 12.0), usA = DcUs("GXV_DRAWCENSUS_ACTUS", 25.0), usF = DcUs("GXV_DRAWCENSUS_FEUS", 40.0);
    uint64_t farA = 0, farS = 0;
    for (int cl = 0; cl < kDcClasses; ++cl) {
        for (int s = 0; s < kDcSizes; ++s) {
            farA += c.far[cl][s].actors;
            farS += c.far[cl][s].shapes;
        }
    }
    port_log_summary("drawcensus: stage %s: %u turns (%u pictures) in %.1f s; a turn: given to the drawers %.1f actors = %.1f shapes (Mario %.1f shapes), stamped off by other cuts %.1f actors "
                     "(%.1f shapes); FAR (beyond %.0f from the camera and the player) %.1f actors = %.1f shapes (%.0f%% of the shapes); unit costs: game %.0f us a shape + %.0f us an actor "
                     "left out of the flush, front end %.0f us a shape",
                     MR::getCurrentStageName(), c.turns, c.pictures, secs, static_cast< double >(c.drawnActors) / n, static_cast< double >(c.drawnShapes) / n,
                     static_cast< double >(c.playerShapes) / n, static_cast< double >(c.stamped) / n, static_cast< double >(c.stampedShapes) / n, static_cast< double >(CrNear()),
                     static_cast< double >(farA) / n, static_cast< double >(farS) / n, c.drawnShapes != 0 ? 100.0 * static_cast< double >(farS) / static_cast< double >(c.drawnShapes) : 0.0,
                     usS, usA, usF);
    char buf[700];
    for (int b = 0; b < kDcBands; ++b) {  // camera band x size: actors/shapes a turn
        int o = snprintf(buf, sizeof buf, "drawcensus: camera %s, a turn, actors/shapes by lines:", DcBandName(b));
        bool any = false;
        for (int s = 0; s < kDcSizes && o < static_cast< int >(sizeof buf) - 40; ++s) {
            const DcCell& cell = c.all[b][s];
            any = any || cell.actors != 0;
            o += snprintf(buf + o, sizeof buf - static_cast< size_t >(o), " %s %.1f/%.1f;", DcSizeName(s), static_cast< double >(cell.actors) / n, static_cast< double >(cell.shapes) / n);
        }
        if (any) {
            port_log_summary("%s", buf);
        }
    }
    const unsigned mask = static_cast< unsigned >(CrDrawKinds());
    const unsigned maskRows = DcRowsOfMask(mask);
    for (int edge = 1; edge <= 8; ++edge) {  // cumulative: far actors / shapes under kDcSizeEdge[edge-1] lines
        int o = snprintf(buf, sizeof buf, "drawcensus: far, under %.0f lines, a turn, actors/shapes [est game ms (with flush skip), front end ms]:", static_cast< double >(kDcSizeEdge[edge - 1]));
        struct Sel { const char* name; unsigned rows; };
        const Sel sels[] = {{"all", 0xFFu}, {"KINDS", maskRows}, {"decor", 1u << 1}, {"decor+fx", 1u << 2}, {"enemy", 1u << 3}, {"mapobj", 1u << 4}, {"npc", 1u << 5}, {"never", 1u << 6}};
        for (const Sel& sel : sels) {
            uint64_t a = 0, s = 0;
            c.Under(edge, sel.rows, &a, &s);
            const DcEst e = DcEstimate(a, s, usS, 0.0, usF);
            const DcEst e2 = DcEstimate(a, s, usS, usA, usF);
            o += snprintf(buf + o, sizeof buf - static_cast< size_t >(o), " %s %.1f/%.1f [%.2f (%.2f), %.2f];", sel.name, static_cast< double >(a) / n, static_cast< double >(s) / n,
                          e.gameMs / n, e2.gameMs / n, e.feMs / n);
        }
        port_log_summary("%s", buf);
    }
    bool used[kDcFamCap] = {};
    int o = snprintf(buf, sizeof buf, "drawcensus: far families by shapes a turn (name[class] drawn actors/shapes; far actors/shapes, far shapes under 8 / 16 lines):");
    port_log_summary("%s", buf);
    for (int rank = 0; rank < 14; ++rank) {
        int best = -1;
        for (int i = 0; i < kDcFamCap; ++i) {
            if (!used[i] && c.fam[i].hash != 0 && (best < 0 || c.fam[i].farShapes > c.fam[best].farShapes)) {
                best = i;
            }
        }
        if (best < 0 || c.fam[best].farShapes == 0) {
            break;
        }
        used[best] = true;
        const DcFam& f = c.fam[best];
        port_log_summary("drawcensus:   %-26s [%s] drawn %.1f/%.1f far %.1f/%.1f under8 %.1f under16 %.1f", f.name, DcClassName(DcClassIndex(f.cls)), static_cast< double >(f.actors) / n,
                         static_cast< double >(f.shapes) / n, static_cast< double >(f.farActors) / n, static_cast< double >(f.farShapes) / n, static_cast< double >(f.under8) / n,
                         static_cast< double >(f.under16) / n);
    }
    (void)o;
    if (c.famOverflowActors != 0) {
        port_log_summary("drawcensus: family table full: %.1f actors / %.1f shapes a turn not named", static_cast< double >(c.famOverflowActors) / n, static_cast< double >(c.famOverflowShapes) / n);
    }
#endif
}
void DcTurn(const ClippingActorInfoList* l0, const ClippingActorInfoList* l1, const ClippingActorInfoList* l2) {
    const CrEnv e = CrBegin(1, 0, 0.0f, MR::getClippingJudge());
    if (!e.haveView) {
        return;
    }
    const int64_t t0 = port_host_time_ns();
    if (sDcWinT0 == 0) {
        sDcWinT0 = t0;
        sDcPic0 = J3DPortPicture();
    }
    DcPassList(l0, e);
    DcPassList(l1, e);
    DcPassList(l2, e);
    ++gDc.turns;
    if ((sDcFlag & 1) != 0 && (gDc.turns & 63u) == 0 && t0 - sDcWinT0 >= 5000000000ll) {
        gDc.pictures = J3DPortPicture() - sDcPic0;
        DcLog();
        gDc.Reset();
        sDcWinT0 = t0;
        sDcPic0 = J3DPortPicture();
    }
}

struct CcTurnGuard {
    const ClippingActorInfoList *l0, *l1, *l2;
    CcTurnGuard(const ClippingActorInfoList* a, const ClippingActorInfoList* b, const ClippingActorInfoList* c) : l0(a), l1(b), l2(c) {
        if (__builtin_expect((port_cc_flag & 1) != 0, 0)) {
            ++sCcTurnNo;
            sCcTimed = (sCcTurnNo % static_cast< uint32_t >(CcSample())) == 0;
        }
    }
    ~CcTurnGuard() {
        if (__builtin_expect(sDcFlag != 0, 0)) {
            DcTurn(l0, l1, l2);  // (cpucuts25, GXV_DRAWCENSUS)
        }
        if (__builtin_expect((port_cc_flag & 1) == 0, 1)) {
            return;
        }
        const int64_t t0 = port_host_time_ns();
        const int cc = CcModeRaw();
        const CrEnv e = CrBegin(cc, CrClipPct(), CrDrawLines(), MR::getClippingJudge());
        if (!e.haveView) {
            return;
        }
        if (sCcWinT0 == 0) {
            sCcWinT0 = t0;
            sCcWinPic0 = J3DPortPicture();
            CcMeasureRead();
        }
        CcPassList(l0, e, cc);
        CcPassList(l1, e, cc);
        CcPassList(l2, e, cc);
        ++gCc.turns;
        if (sCcTimed) {
            ++sCcTimedTurns;
        }
        sCcPassNs += static_cast< uint64_t >(port_host_time_ns() - t0);
        ++sCcPassTurns;
        if ((gCc.turns & 63u) == 0 && t0 - sCcWinT0 >= 5000000000ll) {
            gCc.pictures = J3DPortPicture() - sCcWinPic0;
            CcLog();
            gCc.Reset();
            sCcTimedTurns = 0;
            sCcPassNs = sCcPassTurns = 0;
            sCcWinT0 = t0;
            sCcWinPic0 = J3DPortPicture();
            CcMeasureRead();
        }
    }
};

// the levers' 5 s line
// q30fin26 (GXV_THINFADE, default 1; GXV_THINFADE_TICKS, default 24 = 0.4 s of game turns; 0 off): the far items GXV_PIECETHIN (star bits) and GXV_FARDRAW (specks) hide are SHRUNK away through their
// model's base scale (port/piecefade.h; the view pass in LiveActor::calcViewAndEntry writes it) and only then stamped "not drawn" as before; shown again they grow back. No shader, no material,
// no program. An item that stops being a candidate (near the player or the camera, moving, picked up) grows back 4x faster. Log (with the cliprange line): "thinfade: ...".
port::piecefade::Table sThinFade;
uint32_t sThinFadeTurn = 0;
int ThinFadeTicks() {
    return PORT_HOT_KNOB(int, [] {
        const char* on = getenv("GXV_THINFADE");
        if (on && *on && strtol(on, nullptr, 0) == 0) return 0;
        const char* e = getenv("GXV_THINFADE_TICKS");
        const long v = e && *e ? strtol(e, nullptr, 0) : 24;
        return static_cast< int >(v < 0 ? 0 : (v > 240 ? 240 : v));
    }());
}
void ThinFadeLog() {
#if PORT_LOGGING
    static uint32_t sTick = 0;
    static int64_t sLast = 0;
    if (!PortOcDue(sLast, sTick)) {
        return;
    }
    port::piecefade::Census c;
    sThinFade.Sample(c);
    port_log_summary("thinfade: GXV_THINFADE ticks %d: in progress %u (%u shrinking, %u growing), hidden %u; since start %llu begun, %llu fully hidden, %llu back to full size, %llu grown back fast (no longer far/resting), %llu table full (hidden at once); most live %u, most begun in one turn %u",
                     sThinFade.Ticks(), static_cast< unsigned >(c.shrinking + c.growing), c.shrinking, c.growing, c.hiddenNow, static_cast< unsigned long long >(c.begun),
                     static_cast< unsigned long long >(c.completedOut), static_cast< unsigned long long >(c.completedIn), static_cast< unsigned long long >(c.snapped),
                     static_cast< unsigned long long >(c.overflow), c.maxLive, c.maxStartsPerTurn);
#endif
}
void CrLog(const CrEnv& e) {
#if PORT_LOGGING
    if (sCrTurns == 0 || !PortOcDue(sCrLast, sCrTick)) {
        return;
    }
    const double n = static_cast< double >(sCrTurns);
    port_log_summary("cliprange: stage %s: %u turns; a turn: GXV_CLIPSCALE=%d held clipped %.1f actors (clipped now %.2f, let go %.2f); GXV_FARDRAW=%.0f not drawn %.1f actors "
                     "(%.1f shapes; new %.2f; kept by GXV_FARDRAW_HOLD=%d %.2f; kinds %d); left out of the pose flush %.1f (GXV_FARDRAW_FLUSH=%d); pure decoration seen %.1f; class reads %.1f; near %.0f hyst %.0f%% lever-active %d",
                     MR::getCurrentStageName(), sCrTurns, CrClipPct(), static_cast< double >(sCrClipHeld) / n,
                     static_cast< double >(sCrClipNew) / n, static_cast< double >(sCrClipBack) / n, static_cast< double >(CrDrawLines()), static_cast< double >(sCrDrawHeld) / n,
                     static_cast< double >(sCrDrawShapes) / n, static_cast< double >(sCrDrawNew) / n, CrDrawHold(), static_cast< double >(sCrDrawHeldOn) / n, CrDrawKinds(),
                     static_cast< double >(sCrFlushSkip) / n, CrDrawFlush(), static_cast< double >(sCrDecor) / n,
                     static_cast< double >(sCrKindRead) / n, static_cast< double >(e.nearD), static_cast< double >(e.hyst * 100.0f), e.lever ? 1 : 0);
    sCrTurns = sCrClipHeld = sCrClipNew = sCrClipBack = sCrDrawHeld = sCrDrawShapes = sCrDrawNew = sCrKindRead = sCrDecor = sCrDrawHeldOn = sCrFlushSkip = 0;
#endif
}
}  // namespace

// q30fin26 (GXV_THINFADE): the view pass (LiveActor::calcViewAndEntry, for a model marked mPortFade) writes the shrunk base scale; the finished entry restores it and clears the mark.
extern "C" void port_thinfade_apply(const void* actor, void* model) {
    J3DModel* m = static_cast< J3DModel* >(model);
    float s[3] = {m->mBaseScale.x, m->mBaseScale.y, m->mBaseScale.z};
    bool erased = false;
    if (sThinFade.Apply(reinterpret_cast< uintptr_t >(actor), s, &erased)) {
        m->mBaseScale.x = s[0];
        m->mBaseScale.y = s[1];
        m->mBaseScale.z = s[2];
    }
    if (erased) {
        m->mPortFade = 0;
    }
}
extern "C" int port_thinfade_live(void) { return sThinFade.Live(); }

// ---- NameObjCategoryList.cpp / LiveActor.cpp hooks (only when GXV_CLIPCENSUS is on) ----
extern "C" int64_t port_cc_obj_begin(const void* pObj) {
    if (sCcDepth++ != 0) {
        return 0;
    }
    sCcObj = nullptr;
    const int raw = port_anim_lod_raw_category();
    if (raw < 0 || pObj == nullptr || !port_on_frame_thread() || !CcLiveList(raw)) {
        return 0;
    }
    if (!port_live_actor_valid(static_cast< const LiveActor* >(static_cast< const NameObj* >(pObj)))) {
        ++gCc.unknownListed;
        return 0;
    }
    sCcObj = static_cast< const NameObj* >(pObj);
    sCcRaw = raw;
    sCcMoved = sCcCalcIn = sCcPosed = false;
    return sCcTimed ? (port_host_time_ns() | 1) : 1;
}
extern "C" void port_cc_obj_end(int64_t t0) {
    if (--sCcDepth != 0 || t0 == 0 || sCcObj == nullptr) {
        return;
    }
    const int64_t ns = (sCcTimed && t0 > 1) ? port_host_time_ns() - t0 : 0;
    const LiveActor* a = static_cast< const LiveActor* >(sCcObj);
    sCcObj = nullptr;
    const TVec3f* pp = port_safe_player_pos();
    const int band = pp != nullptr ? BandOf(D2(a->mPosition, *pp)) : 0;
    const int kind = CcKindOfActor(a);
    Cell& cell = gCc.cell[kind][band];
    const uint64_t timed = sCcTimed ? 1 : 0;
    if (sCcRaw >= 0x100) {
        const int i = sCcRaw - 0x100;
        CatRow& r = gCc.mv[i < kMoveCats ? i : kMoveCats - 1];
        ++r.listed; r.ran += sCcMoved ? 1 : 0; r.ns += static_cast< uint64_t >(ns); r.listedT += timed;
        ++cell.mvListed; cell.mvRan += sCcMoved ? 1 : 0; cell.mvNs += static_cast< uint64_t >(ns); cell.mvListedT += timed;
    } else {
        const int i = sCcRaw;
        CatRow& r = gCc.ca[i < kCalcCats ? i : kCalcCats - 1];
        ++r.listed; r.ran += sCcCalcIn ? 1 : 0; r.posed += sCcPosed ? 1 : 0; r.ns += static_cast< uint64_t >(ns); r.listedT += timed;
        ++cell.caListed; cell.caRan += sCcCalcIn ? 1 : 0; cell.caPosed += sCcPosed ? 1 : 0; cell.caNs += static_cast< uint64_t >(ns); cell.caListedT += timed;
    }
}
// what: 0 LiveActor::movement entered, 1 LiveActor::calcAnim entered, 2 its joints posed
extern "C" void port_cc_mark(const void* pActor, int what) {
    if ((port_cc_flag & 2) != 0 && (what == 0 || what == 2) && port_on_frame_thread()) {  // (far30, GXV_DISTCENSUS: the logic ran / the joints are posed)
        const LiveActor* la = static_cast< const LiveActor* >(pActor);
        int band;
        if (DistBandOfPos(la->mPosition, &band)) {
            const port::distcensus::Class c = DistClassOf(la, la == reinterpret_cast< const LiveActor* >(MarioAccess::getPlayerActor()));
            if (what == 0) {
                gDist.Logic(band, c);
            } else {
                gDist.Posed(band, c);
            }
        }
    }
    if (static_cast< const NameObj* >(static_cast< const LiveActor* >(pActor)) != sCcObj) {
        return;
    }
    if (what == 0) {
        sCcMoved = true;
    } else if (what == 1) {
        sCcCalcIn = true;
    } else {
        sCcPosed = true;
    }
}
extern "C" void port_ff_census(unsigned long long* held, unsigned long long* faded, unsigned long long* pops, float* popPx) {   // (far30: the far: line takes the sums of its window)
    *held = sFfHeld;
    *faded = sFfFaded;
    *pops = sFfPop;
    *popPx = sFfPopPx;
    sFfHeld = sFfFaded = sFfPop = 0;
    sFfPopPx = 0.0f;
}
// far30 (GXV_DISTCENSUS): particles (JPAEmitterManager::calcNormal) and shadows (ShadowControllerHolder::updateController) at their own decision points; the end of a turn (perf.cpp)
extern "C" void port_dist_note_particles(float x, float y, float z, unsigned n) {
    int band;
    const TVec3f p(x, y, z);
    if (DistBandOfPos(p, &band)) {
        gDist.Particles(band, n);
    }
}
extern "C" void port_dist_note_shadow(float x, float y, float z) {
    int band;
    const TVec3f p(x, y, z);
    if (DistBandOfPos(p, &band)) {
        gDist.Shadow(band);
    }
}
extern "C" void port_dist_turn_end(int drawn) {  // (GameSystem::frameLoop's turn end, via port_anim_prof_turn)
    if ((port_cc_flag & 2) == 0) {
        return;
    }
    if (sDistStage[0] == 0) {
        snprintf(sDistStage, sizeof sDistStage, "%s", MR::getCurrentStageName() != nullptr ? MR::getCurrentStageName() : "?");
    }
    gDistWin.AddTurn(gDist, drawn != 0);
    gDist.Clear();
    sDistCamOk = false;
    const int64_t now = port_host_time_ns();
    if (sDistWinT0 == 0) {
        sDistWinT0 = now;
    } else if (now - sDistWinT0 >= 5000000000ll) {
        DistLog(false);
        gDistStage.AddTable(gDistWin);
        gDistWin.Clear();
        sDistWinT0 = now;
    }
}
// (GXV_FARANIM, LiveActor.cpp PbAnimSkips)
static int CrFarAnimStatic() {
    return PORT_STAGE_KNOB(int, [] { const float v = CrEnvF("GXV_FARANIM", 0.0f, 0.0f, 100.0f); return v >= 2.0f ? (v > 4.0f ? 4 : static_cast< int >(v)) : 0; }());
}
extern "C" float port_cr_faranim_lines(void) { return PORT_HOT_KNOB(float, CrEnvF("GXV_FARANIM_LINES", 30.0f, 1.0f, 200.0f)); }
extern "C" int port_cr_faranim_sens(void) { return PORT_HOT_KNOB(int, static_cast< int >(CrEnvF("GXV_FARANIM_SENS", 0.0f, 0.0f, 1.0f))); }
extern "C" float port_cr_near(void) { return CrNear(); }
// (GXV_FARDRAW_FLUSH, cpucuts25: LiveActor.cpp port_anim_flush_stale)
extern "C" int port_cpulod_piece_n(void);
extern "C" int port_horizon_pose_on(void);  // (cpu2-26, GXV_HORIZON_POSE: below)
extern "C" int port_cr_drawflush_on(void) { return CrDrawFlush() || port_cpulod_piece_n() >= 2 || port_horizon_pose_on(); }  // (cpulod26: the thinned star bits are left out of the flushes too)
// (GXV_CHARPOSE, cpucuts25: LiveActor.cpp CpCharPose / PbAnimSkips; stage-settable)
// (fe26, GXV_FE_GOV: platform/src/port/perf.cpp's governor level, 0 = off or calm: every accessor below then returns its static value exactly as before, one relaxed load a call.
// With a level, a cut takes the stricter of its static value and the umbrella table's row of that level (port/fegov.h); the level never relaxes a static value.)
// (q30lock26, 12 Nov: a level change is walked in per actor, port/lockfade.h: the `_for(seed)` accessors read the level THIS actor is at, port_fegov_level_for; the plain ones the stricter of the two
// ends while a walk runs. A level is the governors' 0..3 or, in Quality with GXV_Q30_LOCK, the lock's coded level, port/cpulod.h.)
extern "C" int port_fegov_level(void);
extern "C" int port_fegov_level_for(uint32_t seed);
namespace {
float FgTableL(int lv, const char* name, float dflt) {
    float v = dflt;
    port::cpulod::TableValue(lv, name, &v);
    return v;
}
int FgNL(int lv, const char* name) {  // the governor's count for a member, 0 = none
    const float v = FgTableL(lv, name, 0.0f);
    return v >= 2.0f ? (v > 4.0f ? 4 : static_cast< int >(v)) : 0;
}
}  // namespace
static int CpNStatic() {
    return PORT_STAGE_KNOB(int, [] { const float v = CrEnvF("GXV_CHARPOSE", 0.0f, 0.0f, 100.0f); return v >= 2.0f ? (v > 4.0f ? 4 : static_cast< int >(v)) : 0; }());
}
static int CpNAt(int lv) {
    const int b = CpNStatic();
    return lv > 0 ? port::fegov::CountMax(b, FgNL(lv, "GXV_CHARPOSE")) : b;
}
static float CpLinesAt(int lv) {
    const float b = PORT_STAGE_KNOB(float, CrEnvF("GXV_CHARPOSE_LINES", 12.0f, 1.0f, 200.0f));
    return lv > 0 ? port::fegov::SizeFor(CpNStatic(), b, FgNL(lv, "GXV_CHARPOSE"), FgTableL(lv, "GXV_CHARPOSE_LINES", b)) : b;
}
// far30 (GXV_FARFIELD): GXV_FARANIM's count follows the governors' level (2 / 3 / 4 at levels 1 / 2 / 3, the cpulod table); the static value is the floor of it. Off (the default) the table row is
// not read: exactly the static value as before. The phase of each actor is its address (FarAnimSkip), the spread the cut has always had; farfield.h PoseNow is the same rule on ids.
extern "C" int port_cr_faranim_n(void) {
    const int b = CrFarAnimStatic();
    if (!port::cpulod::FarOn()) {
        return b;
    }
    const int g = port_fegov_level();
    return g > 0 ? port::fegov::CountMax(b, FgNL(g, "GXV_FARANIM")) : b;
}
extern "C" int port_cp_n(void) { return CpNAt(port_fegov_level()); }
extern "C" float port_cp_lines(void) { return CpLinesAt(port_fegov_level()); }
extern "C" int port_cp_n_for(uint32_t seed) { return CpNAt(port_fegov_level_for(seed)); }
extern "C" float port_cp_lines_for(uint32_t seed) { return CpLinesAt(port_fegov_level_for(seed)); }
extern "C" float port_cp_move(void) { return PORT_STAGE_KNOB(float, CrEnvF("GXV_CHARPOSE_MOVE", 1.0f, 0.1f, 20.0f)); }
extern "C" float port_cp_dist2(void) {
    return PORT_STAGE_KNOB(float, [] { const float d = CrEnvF("GXV_CHARPOSE_DIST", 2500.0f, 2000.0f, 100000.0f); return d * d; }());
}
// ---- cpulod26 (platform/include/port/cpulod.h): the knobs of the three new cuts (each reads its own switch, else GXV_CPULOD's level; all stage-settable) and the census ----
static int MatlodNStatic() { return PORT_STAGE_KNOB(int, port::cpulod::N("GXV_MATLOD")); }
static int FamposeNStatic() { return PORT_STAGE_KNOB(int, port::cpulod::N("GXV_FAMPOSE")); }
static int PieceNStatic() { return PORT_STAGE_KNOB(int, port::cpulod::N("GXV_PIECETHIN")); }
static int MatlodNAt(int lv) { const int b = MatlodNStatic(); return lv > 0 ? port::fegov::CountMax(b, FgNL(lv, "GXV_MATLOD")) : b; }
static float MatlodDistAt(int lv) {
    const float b = PORT_STAGE_KNOB(float, [] { const float d = port::cpulod::Num("GXV_MATLOD_DIST", 3000.0f); return d >= 1500.0f ? d : 1500.0f; }());
    return lv > 0 ? port::fegov::DistFor(MatlodNStatic(), b, FgNL(lv, "GXV_MATLOD"), FgTableL(lv, "GXV_MATLOD_DIST", b)) : b;
}
static int FamposeNAt(int lv) { const int b = FamposeNStatic(); return lv > 0 ? port::fegov::CountMax(b, FgNL(lv, "GXV_FAMPOSE")) : b; }
static float FamposeDistAt(int lv) {
    const float b = PORT_STAGE_KNOB(float, [] { const float d = port::cpulod::Num("GXV_FAMPOSE_DIST", 3000.0f); return d >= 1500.0f ? d : 1500.0f; }());
    return lv > 0 ? port::fegov::DistFor(FamposeNStatic(), b, FgNL(lv, "GXV_FAMPOSE"), FgTableL(lv, "GXV_FAMPOSE_DIST", b)) : b;
}
static float FamposeMoveAt(int lv) {
    const float b = PORT_STAGE_KNOB(float, [] { const float d = port::cpulod::Num("GXV_FAMPOSE_MOVE", 2.0f); return d >= 0.1f ? d : 0.1f; }());
    return lv > 0 ? port::fegov::SizeFor(FamposeNStatic(), b, FgNL(lv, "GXV_FAMPOSE"), FgTableL(lv, "GXV_FAMPOSE_MOVE", b)) : b;
}
static int PieceNAt(int lv) { const int b = PieceNStatic(); return lv > 0 ? port::fegov::CountMax(b, FgNL(lv, "GXV_PIECETHIN")) : b; }
static float PieceDistAt(int lv) {
    const float b = PORT_STAGE_KNOB(float, [] { const float d = port::cpulod::Num("GXV_PIECETHIN_DIST", 4500.0f); return d >= 1500.0f ? d : 1500.0f; }());
    return lv > 0 ? port::fegov::DistFor(PieceNStatic(), b, FgNL(lv, "GXV_PIECETHIN"), FgTableL(lv, "GXV_PIECETHIN_DIST", b)) : b;
}
extern "C" int port_cpulod_matlod_n(void) { return MatlodNAt(port_fegov_level()); }
extern "C" float port_cpulod_matlod_dist(void) { return MatlodDistAt(port_fegov_level()); }
extern "C" int port_cpulod_fampose_n(void) { return FamposeNAt(port_fegov_level()); }
extern "C" float port_cpulod_fampose_dist(void) { return FamposeDistAt(port_fegov_level()); }
extern "C" float port_cpulod_fampose_move(void) { return FamposeMoveAt(port_fegov_level()); }
extern "C" int port_cpulod_piece_n(void) { return PieceNAt(port_fegov_level()); }
extern "C" float port_cpulod_piece_dist(void) { return PieceDistAt(port_fegov_level()); }
extern "C" int port_cpulod_matlod_n_for(uint32_t seed) { return MatlodNAt(port_fegov_level_for(seed)); }
extern "C" float port_cpulod_matlod_dist_for(uint32_t seed) { return MatlodDistAt(port_fegov_level_for(seed)); }
extern "C" int port_cpulod_fampose_n_for(uint32_t seed) { return FamposeNAt(port_fegov_level_for(seed)); }
extern "C" float port_cpulod_fampose_dist_for(uint32_t seed) { return FamposeDistAt(port_fegov_level_for(seed)); }
extern "C" float port_cpulod_fampose_move_for(uint32_t seed) { return FamposeMoveAt(port_fegov_level_for(seed)); }
extern "C" int port_cpulod_piece_n_for(uint32_t seed) { return PieceNAt(port_fegov_level_for(seed)); }
// cpu2-26 (12 Nov): GXV_PROTECT_SPRITES (default 0 = as merged). The owner's protected list (12 Nov 10:55) names star bits and coins (never thinned, delayed or approximated); at 11:09 he allowed far star bits to be thinned when hardly
// noticeable, so the default does not change round one's behaviour; the switch is there to take them out again if a console run shows a spin that steps. 1: the cpulod
// umbrella's pose and material levers (GXV_FAMPOSE, GXV_MATLOD) leave families 1 (star bits) and 2 (coins) alone. 2: also the Performance budget's every-second-turn pose
// (GXV_PB_ANIM, which reaches them through the map-object category; round zero's rule, not changed unless asked). 0: as before this switch.
extern "C" int port_protect_sprites(void) {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("GXV_PROTECT_SPRITES"); return e && *e ? static_cast< int >(strtol(e, nullptr, 0)) : 0; }());
}
// revB (12 Nov): GXV_PIECETHIN takes a star bit only in its idle Floating nerve (StarPiece.cpp: spin, nothing pending). A bit that is moving (rail, thrown, falling, flying to the player),
// being collected, or changing nerve is always drawn: the owner allowed far thinning of the resting ones only. The nerve is read only for a bit that is already far enough.
extern "C" int port_idle25_starpiece_nerve(const void* pActor);
extern "C" int port_cpulod_level(void) { return PORT_STAGE_KNOB(int, port::cpulod::Level()); }
extern "C" int port_turn_prof_on(void);  // (perf.cpp: GXV_TURN_PROF)
extern "C" int port_cpulod_timer_on(void) { return port_turn_prof_on() != 0 && port_cpulod_matlod_n() >= 2; }
namespace {
// counters (game thread; ModelManager.cpp / LiveActor.cpp count through port_cpulod_count): 0 far models the material rule judged, 1 refreshes left out, 2 timed refreshes of far
// models, 3 their ns, 4 family actors far enough for the pose rule, 5 poses left out, 6 star bits far enough, 7 star bits not drawn, 8 their shapes
unsigned long long sClCnt[10] = {};
unsigned sClTurns = 0;
}  // namespace
extern "C" void port_cpulod_count(int what, unsigned n) { sClCnt[what < 0 || what > 9 ? 9 : what] += n; }
extern "C" void port_cpulod_dl_time(int64_t ns) {
    sClCnt[2] += 1;
    sClCnt[3] += ns > 0 ? static_cast< unsigned long long >(ns) : 0ull;
}
namespace {
// once a game turn (ClippingActorHolder::movement): every 300 turns one "cpulod:" line, when any of the three cuts is on
void CpulodTurn() {
    const int lv = port_cpulod_level(), mN = port_cpulod_matlod_n(), fN = port_cpulod_fampose_n(), pN = port_cpulod_piece_n();
    if (lv == 0 && mN == 0 && fN == 0 && pN == 0) {
        return;
    }
    if (++sClTurns < 300) {
        return;
    }
    const double t = static_cast< double >(sClTurns);
    const double perTurnUs = sClCnt[2] > 0 ? static_cast< double >(sClCnt[3]) / static_cast< double >(sClCnt[2]) / 1000.0 : 0.0;
    port_log_summary("cpulod: level %d, %u turns: GXV_MATLOD=%d (%.0f): far models judged %.1f a turn, refreshes left out %.1f a turn%s | GXV_FAMPOSE=%d (%.0f): far family actors %.1f, poses left out %.1f "
                     "| GXV_PIECETHIN=%d (%.0f): far star bits %.1f, not drawn %.1f (%.1f shapes)",
                     lv, sClTurns, mN, static_cast< double >(port_cpulod_matlod_dist()), static_cast< double >(sClCnt[0]) / t, static_cast< double >(sClCnt[1]) / t,
                     sClCnt[2] > 0 ? "" : (mN >= 2 ? " (set GXV_TURN_PROF=1 for the timer)" : ""), fN, static_cast< double >(port_cpulod_fampose_dist()),
                     static_cast< double >(sClCnt[4]) / t, static_cast< double >(sClCnt[5]) / t, pN, static_cast< double >(port_cpulod_piece_dist()),
                     static_cast< double >(sClCnt[6]) / t, static_cast< double >(sClCnt[7]) / t, static_cast< double >(sClCnt[8]) / t);
    if (sClCnt[2] > 0) {
        port_log_summary("cpulod: timer: a refresh of a far model took %.1f us (%.0f timed); left out %.1f a turn = est. %.2f ms a turn saved by GXV_MATLOD", perTurnUs,
                         static_cast< double >(sClCnt[2]), static_cast< double >(sClCnt[1]) / t, static_cast< double >(sClCnt[1]) / t * perTurnUs / 1000.0);
    }
    std::memset(sClCnt, 0, sizeof sClCnt);
    sClTurns = 0;
}
}  // namespace
extern "C" void port_cr_flush_count(unsigned n) { sCrFlushSkip += n; }
extern "C" void port_cr_init(void) {  // (a new scene: ClippingActorHolder's constructor)
    DistStageEnd();  // (far30: the stage that ends here, its means)
    __atomic_store_n(&port_cc_flag, (CcModeRaw() != 0 ? 1 : 0) | (DistOnRaw() != 0 ? 2 : 0), __ATOMIC_RELAXED);
    gCc.Reset();
    sCcWinT0 = 0;
    sCcTimedTurns = 0;
    sCcPassNs = sCcPassTurns = 0;
    sCrTurns = sCrClipHeld = sCrClipNew = sCrClipBack = sCrDrawHeld = sCrDrawShapes = sCrDrawNew = sCrKindRead = sCrDecor = sCrDrawHeldOn = sCrFlushSkip = 0;
    port_dc_init();
}
#endif  // TARGET_PC (cliprange25)

#ifdef TARGET_PC
// ---------------------------------------------------------------------------------------------------------------------------------------
// GXV_HORIZON (horizon25, 11 Nov; default 0). HORIZON CULLING: a stage is a few solid planets with objects standing all around them, and the
// game clips by frustum and distance only, so everything on the far side of the planet the camera looks at is posed, listed, parsed by the
// front end and vertex-processed by the GPU, then hidden by the planet. An OCCLUDER is a ball inside a planet's collision solid (port/horizon.h:
// the inscribed ball of the collision mesh around a centre whose winding number is 1, shrunk by GXV_HORIZON_SHRINK). An actor whose clipping
// sphere lies wholly in the ball's shadow seen from the camera (port::horizon::Hidden: beyond the tangent length and inside the cone, with a
// margin and hysteresis) cannot be seen: its MODEL is stamped not to be drawn in the next picture (J3DModel::mPortCullPicture, the draw-only
// path of GXV_ZOOM_DRAWCULL / GXV_TINYCULL). Its logic, movement, animation, sensors, sound, shadow and effects run as before: only the shapes
// go. Never: the player (and anything within 600 units of him: carried or riding things), the occluder planet itself, models whose joints only
// the picture reads (mLogicFreeJoints), group-clipped actors (not judged here), the actors of a mirror camera / VR picture, a planet that is
// dead, clipped or hidden (it is not drawn, so it hides nothing). Only planets whose collision is a closed outward solid around the centre
// are occluders; water / glass / ring / cage names are refused first. Values:
//   0 off.  1 on.  2 census: judged and counted, nothing stamped.  3 verify: nothing stamped; a flat magenta ball (the actor's clipping
//   sphere) is drawn with the depth test at each would-be-culled actor (Mario::draw -> port_horizon_draw_markers): a magenta pixel anywhere
//   means the planet does not hide that sphere, i.e. the occluder is wrong.
// GXV_HORIZON_SHRINK (0.9), GXV_HORIZON_MARGIN (0.03 rad enter; exit 1/3 of it), GXV_HORIZON_MINR (500: occluders under this world radius are
// not used). Log lines (logging builds):
//   horizon: stage S occluder ACCEPTED/rejected NAME ...   one per planet at load
//   horizon: stage S: N occluders (name r shrink ...), M rejected (name: reason ...)   first judged turn after a load
//   horizon: stage S: census N turns, occluders active A, per turn tested T would-cull K (shapes H) now-hidden Z; mode m   about every 5 s
namespace {
int HzMode() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_HORIZON");
        return e && *e ? static_cast< int >(strtol(e, nullptr, 0)) : 0;
    }());
}
float HzEnvF(const char* pName, float def, float lo, float hi) {
    const char* e = getenv(pName);
    float v = e && *e ? static_cast< float >(strtod(e, nullptr)) : def;
    return v < lo ? lo : (v > hi ? hi : v);
}
float HzShrink() {
    return PORT_HOT_KNOB(float, HzEnvF("GXV_HORIZON_SHRINK", 0.9f, 0.3f, 1.0f));
}
float HzMargin() {
    return PORT_HOT_KNOB(float, HzEnvF("GXV_HORIZON_MARGIN", 0.03f, 0.01f, 0.3f));
}
// cpu2-26 (GXV_HORIZON_POSE, default 0; acts only with GXV_HORIZON=1): a model the horizon hid is also left out of the drawn turn's pose flush (and its material list), exactly as a
// far-draw-stamped model is (GXV_FARDRAW_FLUSH's rule: nobody sees its pose this picture; posed again in the picture it is not hidden in). Needs port_cr_drawflush_on (the flush
// rule) so it is switched on together: HzPoseOn() also makes port_cr_drawflush_on true.
int HzPoseOn() {
    return PORT_HOT_KNOB(int, HzEnvF("GXV_HORIZON_POSE", 0.0f, 0.0f, 1.0f) >= 1.0f ? 1 : 0);
}
float HzMinR() {
    return PORT_HOT_KNOB(float, HzEnvF("GXV_HORIZON_MINR", 500.0f, 50.0f, 1.0e6f));
}

struct HzOcc {
    LiveActor* actor;
    CollisionParts* parts;
    float c[3];     // the ball's centre in the collision's space
    float rho;      // its radius there (inscribed distance * shrink)
    char name[32];
};
constexpr int kHzMaxOcc = 12;
constexpr int kHzCones = 4;
constexpr int kHzMarks = 96;
HzOcc sHzOcc[kHzMaxOcc];
int sHzOccN, sHzRejected;
bool sHzSummary;
char sHzRejText[200];
float sHzMark[kHzMarks][4];
int sHzMarkN;
float sHzPrevCam[3];
bool sHzHavePrev, sHzWarned;
uint32_t sHzTurns, sHzTested, sHzWould, sHzShapes, sHzActive, sHzNow;
int64_t sHzLast;
uint32_t sHzTick;

bool HzDenied(const char* pName) {
    static const char* const kWords[] = {"water", "ocean", "sea", "glass", "cage", "ring", "torus", "tube", "pipe", "cloud", "hollow", "bubble", "ice", "lava", "sand"};
    char low[64];
    size_t n = 0;
    for (; pName[n] != 0 && n < sizeof(low) - 1; n++) {
        const char ch = pName[n];
        low[n] = (ch >= 'A' && ch <= 'Z') ? static_cast< char >(ch + 32) : ch;
    }
    low[n] = 0;
    for (const char* w : kWords) {
        if (strstr(low, w) != nullptr) {
            return true;
        }
    }
    return false;
}

void HzScale(const CollisionParts* pParts, float* pMin) {
    const TPos3f& m = pParts->mBaseMatrix;
    float mn = 1.0e30f;
    for (int c = 0; c < 3; c++) {
        const float x = m.mMtx[0][c], y = m.mMtx[1][c], z = m.mMtx[2][c];
        const float l = sqrtf(x * x + y * y + z * z);
        mn = l < mn ? l : mn;
    }
    *pMin = mn;
}

struct HzTurn {
    int mode = 0;
    int nCones = 0;
    port::horizon::Cone cone[kHzCones];
    const LiveActor* occ[kHzCones];
    const LiveActor* player = nullptr;
    float pp[3] = {0, 0, 0};
    u32 next = 0;
    uint32_t tested = 0, would = 0, shapes = 0, now = 0;
};

void HzSummaryLine() {
#if PORT_LOGGING
    char line[420];
    int n = snprintf(line, sizeof(line), "horizon: stage %s: %d occluders (", MR::getCurrentStageName(), sHzOccN);
    for (int i = 0; i < sHzOccN && n < 250; i++) {
        float sc = 1.0f;
        HzScale(sHzOcc[i].parts, &sc);
        n += snprintf(line + n, sizeof(line) - n, "%s%s r=%.0f shrink %.2f", i ? ", " : "", sHzOcc[i].name, static_cast< double >(sHzOcc[i].rho * sc),
                      static_cast< double >(HzShrink()));
    }
    snprintf(line + n, sizeof(line) - n, "), %d rejected (%s)", sHzRejected, sHzRejText);
    port_log_summary("%s", line);
#endif
}

HzTurn HzBegin(int mode, const LiveActor* pPlayer) {
    HzTurn t;
    t.mode = mode;
    t.next = J3DPortPicture() + 1;
    sHzMarkN = 0;
    if (mode == 0 || !port_safe_cam_exists() || MR::isExistMirrorCamera()) {
        sHzHavePrev = false;
        return t;
    }
    {
        float a, b, c;
        float vr[12];
        if (port_vr_cull_view(vr, &a, &b) != 0) {
            (void)c;
            return t;  // (VR: the headset's view is not the camera judged here)
        }
    }
    const TVec3f cam = MR::getCamPos();
    const float E[3] = {cam.x, cam.y, cam.z};
    float dE = 0.0f;
    if (sHzHavePrev) {
        const float dx = E[0] - sHzPrevCam[0], dy = E[1] - sHzPrevCam[1], dz = E[2] - sHzPrevCam[2];
        dE = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    sHzPrevCam[0] = E[0]; sHzPrevCam[1] = E[1]; sHzPrevCam[2] = E[2];
    sHzHavePrev = true;
    if (sHzSummary && sHzOccN + sHzRejected > 0) {
        sHzSummary = false;
        HzSummaryLine();
    }
    t.player = pPlayer;
    if (pPlayer != nullptr) {
        t.pp[0] = pPlayer->mPosition.x; t.pp[1] = pPlayer->mPosition.y; t.pp[2] = pPlayer->mPosition.z;
    }
    const float margin = HzMargin();
    float alphas[kHzCones];
    for (int i = 0; i < sHzOccN; i++) {
        const HzOcc& o = sHzOcc[i];
        if (MR::isDead(o.actor) || MR::isClipped(o.actor) || MR::isHiddenModel(o.actor)) {
            continue;  // (not drawn: it hides nothing)
        }
        const TPos3f& m = o.parts->mBaseMatrix;
        float sc;
        HzScale(o.parts, &sc);
        const float C[3] = {m.mMtx[0][0] * o.c[0] + m.mMtx[0][1] * o.c[1] + m.mMtx[0][2] * o.c[2] + m.mMtx[0][3],
                            m.mMtx[1][0] * o.c[0] + m.mMtx[1][1] * o.c[1] + m.mMtx[1][2] * o.c[2] + m.mMtx[1][3],
                            m.mMtx[2][0] * o.c[0] + m.mMtx[2][1] * o.c[1] + m.mMtx[2][2] * o.c[2] + m.mMtx[2][3]};
        const float R = o.rho * sc;
        if (!(R >= HzMinR())) {
            continue;
        }
        const float dx = C[0] - E[0], dy = C[1] - E[1], dz = C[2] - E[2];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz);
        const float enter = margin + 2.0f * dE / (d - R > 100.0f ? d - R : 100.0f);  // (a camera that jumped: the margin grows with the jump)
        if (!(enter < 0.35f)) {
            continue;
        }
        const port::horizon::Cone k = port::horizon::MakeCone(E, C, R, enter, enter / 3.0f);
        if (!k.ok) {
            continue;
        }
        const float alpha = asinf(R / d);
        int at = t.nCones;
        if (at == kHzCones) {
            if (alpha <= alphas[kHzCones - 1]) {
                continue;
            }
            at = kHzCones - 1;
        } else {
            t.nCones++;
        }
        while (at > 0 && alphas[at - 1] < alpha) {  // (largest angular size first)
            alphas[at] = alphas[at - 1]; t.cone[at] = t.cone[at - 1]; t.occ[at] = t.occ[at - 1];
            at--;
        }
        alphas[at] = alpha; t.cone[at] = k; t.occ[at] = o.actor;
    }
    return t;
}

void HzRelease(ClippingActorInfo* pInfo, u32 next) {
    if (pInfo->mPortHorizon == 0) {
        return;
    }
    if (pInfo->mPortHorizon == 1 && pInfo->mActor != nullptr) {
        J3DModel* pModel = MR::getJ3DModel(pInfo->mActor);
        if (pModel != nullptr && pModel->mPortCullPicture == next && pModel->mPortTinyCulled == 0 && !pInfo->mPortZoomCulled) {
            pModel->mPortCullPicture = 0;
            pModel->mPortFarCulled = 0;  // (cpu2-26: GXV_HORIZON_POSE's mark goes with the stamp)
        }
    }
    pInfo->mPortHorizon = 0;
}

// After the actor's clipping was judged (and the zoom / tiny culls ran: this one writes last).
void HzJudge(ClippingActorInfo* pInfo, bool clip, HzTurn& t) {
    bool hide = false;
    LiveActor* pActor = pInfo->mActor;
    if (!clip && t.nCones > 0) {
        ++t.tested;
        if (pActor != t.player && pActor->mLogicFreeJoints == 0) {
            const TVec3f& c = *pInfo->_4;
            const float q = pInfo->_8 * 1.15f + 20.0f;  // (the clipping sphere, a little more)
            const float px = c.x - t.pp[0], py = c.y - t.pp[1], pz = c.z - t.pp[2];
            const float keep = q + 600.0f;
            if (t.player == nullptr || px * px + py * py + pz * pz > keep * keep) {
                const float P[3] = {c.x, c.y, c.z};
                const bool was = pInfo->mPortHorizon != 0;
                for (int i = 0; i < t.nCones && !hide; i++) {
                    hide = pActor != t.occ[i] && port::horizon::Hidden(t.cone[i], P, q, was);
                }
            }
        }
    }
    if (hide) {
        J3DModel* pModel = MR::getJ3DModel(pActor);
        if (pModel == nullptr) {
            hide = false;
        } else {
            ++t.would;
            const J3DModelData* pData = pModel->getModelData();
            t.shapes += pData != nullptr ? pData->getShapeNum() : 0;
            if (t.mode == 1) {
                pInfo->mPortHorizon = 1;
                pModel->mPortCullPicture = t.next;
                if (HzPoseOn()) {
                    pModel->mPortFarCulled = 1;  // (cpu2-26: the pose flush may leave the hidden model out: LiveActor.cpp port_anim_flush_stale)
                }
            } else {
                pInfo->mPortHorizon = 2;
                if (t.mode == 3 && sHzMarkN < kHzMarks) {
                    const TVec3f& c = *pInfo->_4;
                    float* mk = sHzMark[sHzMarkN++];
                    mk[0] = c.x; mk[1] = c.y; mk[2] = c.z; mk[3] = pInfo->_8;
                }
            }
            ++t.now;
        }
    }
    if (!hide) {
        HzRelease(pInfo, t.next);
    }
}

void HzTurnEnd(const HzTurn& t) {
    ++sHzTurns;
    sHzTested += t.tested; sHzWould += t.would; sHzShapes += t.shapes; sHzActive += static_cast< uint32_t >(t.nCones); sHzNow += t.now;
#if PORT_LOGGING
    if (PortOcDue(sHzLast, sHzTick) && sHzTurns != 0) {
        const double n = static_cast< double >(sHzTurns);
        port_log_summary("horizon: stage %s: census %u turns, occluders active %.1f of %d, per turn tested %.0f would-cull %.1f (shapes %.1f), mode %d%s",
                         MR::getCurrentStageName(), sHzTurns, static_cast< double >(sHzActive) / n, sHzOccN, static_cast< double >(sHzTested) / n,
                         static_cast< double >(sHzWould) / n, static_cast< double >(sHzShapes) / n, t.mode,
                         t.mode == 1 ? " (culled)" : (t.mode == 2 ? " (census: nothing culled)" : " (verify: magenta balls)"));
        sHzTurns = sHzTested = sHzWould = sHzShapes = sHzActive = sHzNow = 0;
    }
#endif
}
}  // namespace

extern "C" void port_horizon_reset(void) {
    sHzOccN = 0;
    sHzRejected = 0;
    sHzRejText[0] = 0;
    sHzSummary = false;
    sHzMarkN = 0;
    sHzHavePrev = false;
}

static void HzReject(const char* pName, const char* pWhy, int tris, double winding) {
    ++sHzRejected;
    const size_t used = strlen(sHzRejText);
    if (used + 60 < sizeof(sHzRejText)) {
        snprintf(sHzRejText + used, sizeof(sHzRejText) - used, "%s%s: %.32s", used ? ", " : "", pName, pWhy);
    }
#if PORT_LOGGING
    port_log_summary("horizon: stage %s: occluder rejected %s: %s (tris %d, winding %.2f)", MR::getCurrentStageName(), pName, pWhy, tris, winding);
#endif
    sHzSummary = true;
}

// Called by PlanetMap::init after its collision exists. The fit costs a few passes over the triangles (~10-50 ms for a big planet), at load.
extern "C" void port_horizon_register(LiveActor* pActor, const char* pModelName) {
    if (HzMode() == 0 || pActor->mCollisionParts == nullptr) {
        return;
    }
    const char* name = pModelName != nullptr ? pModelName : pActor->mName;
    if (HzDenied(name)) {
        HzReject(name, "name: water / glass / ring / cage style planet", 0, 0.0);
        return;
    }
    if (sHzOccN >= kHzMaxOcc) {
        HzReject(name, "occluder table full", 0, 0.0);
        return;
    }
    CollisionParts* pParts = pActor->mCollisionParts;
    KCollisionServer* pSrv = pParts->mServer;
    if (pSrv == nullptr || pSrv->mFile == nullptr) {
        return;
    }
    const int tris = pSrv->getTriangleNum();
    if (tris < 8 || tris > 60000) {
        HzReject(name, tris < 8 ? "too few triangles" : "too many triangles", tris, 0.0);
        return;
    }
    const port::horizon::Fit f = port::horizon::FitOccluder(tris, [pSrv](int i, double v[3][3], double n[3]) {
        const KC_PrismData* pPrism = pSrv->getPrismData(static_cast< u32 >(i));
        if (pSrv->isNearParallelNormal(pPrism)) {
            return false;
        }
        for (int k = 0; k < 3; k++) {
            const TVec3f p = pSrv->getPos(pPrism, k);
            v[k][0] = p.x; v[k][1] = p.y; v[k][2] = p.z;
        }
        const TVec3f* pN = pSrv->getFaceNormal(pPrism);
        n[0] = pN->x; n[1] = pN->y; n[2] = pN->z;
        return true;
    });
    if (!f.ok) {
        HzReject(name, f.why, tris, f.winding);
        return;
    }
    float sc;
    HzScale(pParts, &sc);
    const float rho = static_cast< float >(f.dmin) * HzShrink();
    if (!(rho * sc >= HzMinR())) {
        HzReject(name, "inscribed ball under GXV_HORIZON_MINR", tris, f.winding);
        return;
    }
    HzOcc& o = sHzOcc[sHzOccN++];
    o.actor = pActor;
    o.parts = pParts;
    o.c[0] = static_cast< float >(f.c[0]); o.c[1] = static_cast< float >(f.c[1]); o.c[2] = static_cast< float >(f.c[2]);
    o.rho = rho;
    snprintf(o.name, sizeof(o.name), "%s", name);
    sHzSummary = true;
#if PORT_LOGGING
    port_log_summary("horizon: stage %s: occluder ACCEPTED %s: r=%.0f (inscribed %.0f, shrink %.2f, scale %.2f), extent %.0f, tris %d, winding %.3f",
                     MR::getCurrentStageName(), name, static_cast< double >(rho * sc), f.dmin * sc, static_cast< double >(HzShrink()),
                     static_cast< double >(sc), f.extent * sc, tris, f.winding);
#endif
}

// GXV_HORIZON=3: the would-be-culled actors' clipping spheres in flat magenta, depth-tested against the picture (Mario::draw).
extern "C" void port_horizon_draw_markers() {
    if (sHzMarkN <= 0) {
        return;
    }
    TDDraw::cameraInit3D();
    TDDraw::setup(0, 0, 0);
    TDDraw::resetViewMtx();
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    for (int i = 0; i < sHzMarkN; i++) {
        TDDraw::drawSphere3D(TVec3f(sHzMark[i][0], sHzMark[i][1], sHzMark[i][2]), sHzMark[i][3], 0xFF00FFFF, 10);
    }
    TDDraw::close();
}
#endif

#ifdef TARGET_PC
extern "C" int port_pair_skip_turn;          // (cpu34g, perf.cpp)
extern "C" int port_clip_pair_mode(void);
extern "C" void port_pair_count(int what, unsigned n);
namespace {
bool sClipPairLast = false;     // the last turn was not judged
bool sClipPairSkipped = false;  // this turn was not judged (ClippingDirector leaves the groups out too)
}  // namespace
bool PortClipPairSkipped() { return sClipPairSkipped; }
#endif
void ClippingActorHolder::movement() {
    mViewGroupCtrl->update();

#ifdef TARGET_PC
    CpulodTurn();  // (cpulod26: the census line)
    CcTurnGuard ccGuard(_10, _1C, _14);  // (GXV_CLIPCENSUS: the pass over the lists runs when this function has judged them; off: one load and a branch)
    port_popin_turn();  // (popin24: GXV_POPIN_ZOOM's ratio for this turn; GXV_POPIN_LOG's census)
    // cpu34g (14 Nov 2026), GXV_CLIP_PAIR (perf.cpp): the drawn turn of a drawn + undrawn pair does not judge. The picture is drawn from the state the undrawn turn
    // leaves, and that turn judges every actor with its own camera, so every picture has verdicts as fresh as before; an actor's movement stops or resumes at most one
    // turn later (a camera cut made in the drawn turn is judged by the undrawn one, before its picture). Never two turns in a row. The picture stamps of the culls below
    // are written by the judging turn as before; their ages and fades count judged turns, so they last twice as long in a pair.
    sClipPairSkipped = false;
    if (__builtin_expect(port_pair_skip_turn != 0, 0) && port_clip_pair_mode() != 0 && !sClipPairLast && port_safe_cam_exists()) {
        sClipPairLast = true;
        sClipPairSkipped = true;
        port_pair_count(7, 1);
        port_pair_count(8, static_cast< unsigned >(_10->_4));
        return;
    }
    sClipPairLast = false;
    const int portMode = ClipFastMode();
    const ZoomCullTurn zoomTurn = ZoomCullBegin(MR::getClippingJudge());  // (GXV_ZOOM_DRAWCULL)
    if (portMode != 0) {
        const ClippingJudge* pJudge = MR::getClippingJudge();
        uint32_t judged = 0;
        const float pbDist = port_safe_cam_exists() ? port_pb_decor_dist() : 0.0f;  // (GXV_PB_DECOR; 0 = off; no camera yet: off)
        TVec3f pbCam(0.0f, 0.0f, 0.0f);
        if (pbDist > 0.0f) {
            pbCam = MR::getCamPos();
        }
        const bool prefetch = port_upd_prefetch_on() != 0;
        const float clipLead = port::lockstep::On() ? 0.0f : port_popin_clip_lead();  // (popin25, GXV_POPIN_LEAD: a fast camera sees things appear this much earlier; 0 = as the game; fmt32: as the game in a lockstep session)
        // (GXV_STREAM_CENSUS: while the next picture drawn is counted, each actor's model gets the projected size of its clipping sphere;
        // GXV_TINYCULL: a model whose sphere is smaller than that on the picture is not drawn in the next picture. Both need the camera.)
        const bool censusStamp = port_stream_census_judging() != 0 && port_safe_cam_exists();
        const float tinyLines = TinyCullLines();
        const bool tiny = tinyLines > 0.0f && port_safe_cam_exists() && !MR::isExistMirrorCamera() && !port_pb_demo_off();  // (popin31: GXV_PB_DEMO=2: off in a flight too)
        TVec3f scCam(0.0f, 0.0f, 0.0f);
        float scTan = 0.0f;
        if (censusStamp || tiny) {
            scCam = MR::getCamPos();
            scTan = tanf(MR::getFovy() * 0.5f * 3.14159265f / 180.0f);
        }
        const u32 tinyNext = J3DPortPicture() + 1;
        const LiveActor* const tinyPlayer = tiny ? reinterpret_cast< const LiveActor* >(MarioAccess::getPlayerActor()) : nullptr;
        if (tiny) {
            ++sTinyTurns;
        }
        const int crClipPct = CrClipPct();  // (cliprange25; both 0 by default: the hooks below are not entered)
        const float crDrawLines = CrDrawLines();
        const int pieceN = port_cpulod_piece_n();  // (cpulod26, GXV_PIECETHIN: 0 = off)
        // q30fin26 (GXV_THINFADE): while items are shrinking away or growing back the loop runs for them even if the cut itself has just been taken off (a step given back)
        const int fadeTicks = ThinFadeTicks();
        const bool fadeOn = fadeTicks > 0;
        if (fadeOn) {
            sThinFade.Config(fadeTicks, 4);
            ++sThinFadeTurn;
        }
        const bool fadeLive = fadeOn && sThinFade.Live() > 0;
        const float ffH = port_safe_cam_exists() ? port_ff_draw_horizon() : 0.0f;
        const bool fly = ffH > 0.0f && port_ff_flyover() != 0;
        const float ffPx = ffH > 0.0f ? port_ff_draw_px() : 0.0f;   // (far30, GXV_FARFIELD: the draw distance of small decoration; 0 = off)
        const CrEnv cr = CrBegin(0, crClipPct, crDrawLines, pJudge, pieceN >= 2 || fadeLive || ffH > 0.0f);
        {
            // popin31 (B1): GXV_PB_DEMO=2 stops the levers in a flight, but their stamps' marks (mCrDraw) stayed set, so after the flight a thing the flight showed
            // counted as "already cut" (no hysteresis, the minimum-age hold) and could be taken in the picture at once. At the flight's first turn every far-draw
            // stamp is let go (one pass over the list, once a flight). GXV_PB_DEMO 0 / 1: port_pb_demo_off is never 1, nothing runs.
            static bool sCrFlightLetGo = false;
            if (__builtin_expect(port_pb_demo_off() != 0, 0)) {
                if (!sCrFlightLetGo) {
                    sCrFlightLetGo = true;
                    const u32 next = J3DPortPicture() + 1;
                    for (s32 j = 0; j < _10->_4; j++) {
                        CrDrawRelease(_10->mClippingActorList[j], next);
                    }
                }
            } else {
                sCrFlightLetGo = false;
            }
        }
        const bool crClipOn = cr.lever && crClipPct > 0 && !port::lockstep::On();  // (fmt32: GXV_CLIPSCALE stops a decoration's movement by this console's camera: never in a lockstep session)
        const bool crDrawOn = cr.lever && (crDrawLines > 0.0f || ffH > 0.0f);
        const bool pieceOn = cr.lever && pieceN >= 2;
        const float pieceDist = (pieceOn || (fadeLive && cr.lever)) ? port_cpulod_piece_dist() : 0.0f;
        const unsigned crDrawKinds = crDrawOn ? static_cast< unsigned >(CrDrawKinds()) : 1u;  // (cpucuts25)
        const int crDrawHold = crDrawOn ? CrDrawHold() : 0;
        const int hzMode = HzMode();
        HzTurn hz = HzBegin(hzMode, hzMode != 0 ? reinterpret_cast< const LiveActor* >(MarioAccess::getPlayerActor()) : nullptr);
        for (s32 i = 0; i < _10->_4; i++) {
            if (prefetch) {
                ClipPrefetch(_10->mClippingActorList, i, _10->_4);
            }
            ClippingActorInfo* pInfo = _10->mClippingActorList[i];
            s32 level = pInfo->mFarClipLevel;
            if (pInfo->_14->_0 == 1) {
                level = 0;
            }
            const THex3f& rFrustum = (level == 0) ? pJudge->mFrustum : pJudge->mClipFrustums[level];
            bool clip = !rFrustum.mayIntersectBall3(*pInfo->_4, clipLead > 0.0f ? pInfo->_8 + clipLead : pInfo->_8);
            const bool frustumClip = clip;
            const bool wasPbFar = pInfo->mPbFar;
            bool pbFar = false;
            if (pbDist > 0.0f && pInfo->mPbDecor) {
                // (popin24: judged also while the frustum clips it, so the hysteresis state is current when it comes back into view)
                pbFar = PortPbDecorFar(pInfo, pbDist, pbCam.x, pbCam.y, pbCam.z);
                if (!clip && pbFar) {
                    clip = true;
                    port_logic_count(12, 1);  // (GXV_PB_DECOR)
                }
            }
            // GXV_CLIPSCALE (cliprange25): pure decoration far from the player and the camera is clipped earlier than the game does
            const bool wasCr = pInfo->mCrClip != 0;
            bool crHeld = false;
            if (crClipOn && !clip) {
                const TVec3f& cc = *pInfo->_4;
                const float cd2 = D2(cc, cr.cam);
                const float reach = cr.nearD * cr.zoom + pInfo->_8;
                if (cd2 > reach * reach && CrKindOf(pInfo, cr.playerActor) == kDecor) {
                    ++sCrDecor;
                    const float camD = sqrtf(cd2), plD = sqrtf(D2(cc, cr.player));
                    crHeld = Beyond(camD - pInfo->_8, CrClipLimitOf(cr, pInfo, CrLevel(pInfo)), wasCr, cr.hyst) && CrPlayerFar(cr, plD, pInfo->_8, wasCr);
                }
                pInfo->mCrClip = crHeld ? 1 : 0;
                if (crHeld) {
                    clip = true;
                    ++sCrClipHeld;
                }
            }
            if (portMode == 2) {
                const bool orig = pInfo->isJudgedToClip();
                if (orig != clip) {
                    ++sClipMismatches;
#if PORT_LOGGING
                    if (sClipMismatches <= 16) {
                        port_log_summary("oc-clip: MISMATCH level %d radius %g: loop %d original %d", static_cast< int >(level), pInfo->_8,
                                         clip ? 1 : 0, orig ? 1 : 0);
                    }
#endif
                    clip = orig;
                }
            }
            LiveActor* pActor = pInfo->mActor;
            if (clip) {
                if (!MR::isClipped(pActor)) {
                    pActor->startClipped();
                    if (crHeld) {
                        ++sCrClipNew;
                    }
                    if (port::popin::LogOn()) {
                        PortClipEvent(((pbFar || crHeld) && !frustumClip) ? port::popin::kDecor : PortOrigKind(pJudge, pInfo, level), 0, pInfo);
                    }
                }
            } else {
                if (MR::isClipped(pActor)) {
                    pActor->endClipped();
                    if (wasPbFar && !pbFar && pbDist > 0.0f && pInfo->mPbDecor) {
                        static_cast< PlantGroup* >(pActor)->portSplitDecorReveal(pbCam, tinyNext);
                    }
                    if (wasCr) {
                        ++sCrClipBack;
                    }
                    if (port::popin::LogOn()) {
                        PortClipEvent((wasPbFar || wasCr) ? port::popin::kDecor : PortOrigKind(pJudge, pInfo, level), 1, pInfo);
                    }
                }
            }
            ZoomCullJudge(pInfo, clip, zoomTurn);
            if ((censusStamp || tiny) && !clip) {
                J3DModel* pScModel = MR::getJ3DModel(pActor);  // (the model the picture draws: alodfix23)
                if (pScModel != nullptr) {
                    const TVec3f& c = *pInfo->_4;
                    const float dx = c.x - scCam.x, dy = c.y - scCam.y, dz = c.z - scCam.z;
                    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (censusStamp) {
                        pScModel->mPortCensusStamp = port_stream_census_stamp(pInfo->_8, dist, scTan);
                    }
                    if (tiny) {
                        TinyCullJudge(pActor, pInfo, pScModel, dist, scTan, tinyLines, tinyNext, tinyPlayer);
                    }
                }
            } else if (tiny && clip) {
                TinyUncull(MR::getJ3DModel(pActor), tinyNext);
            }
            if (hzMode != 0) {
                HzJudge(pInfo, clip, hz);  // (horizon25: it writes the stamp after the zoom and tiny culls)
            }
            if (crDrawOn || pieceOn || (fadeLive && cr.lever)) {
                // GXV_FARDRAW (cliprange25; writes last): small far pure decoration is not drawn in the next picture (the stamp of the culls above)
                // GXV_PIECETHIN (cpulod26): the same stamp for one in (n-1)/n of the far star bits (family 1), by distance only, never a size
                bool cull = false;
                J3DModel* pCrModel = nullptr;
                const bool pieceCand = (pieceOn || (fadeLive && cr.lever)) && pInfo->mActor != nullptr && pInfo->mActor->mFlFam == 1;
                if (!clip && pieceCand) {
                    const TVec3f& cc = *pInfo->_4;
                    const float cd2 = D2(cc, cr.cam), pd2 = D2(cc, cr.player);
                    const bool was = pInfo->mCrDraw != 0 || (fadeLive && sThinFade.Wants(reinterpret_cast< uintptr_t >(pInfo->mActor)));  // (a fading item is still "in": no 6 % hysteresis flip)
                    const float lim = port::cpulod::HystDist(pieceDist, was, cr.hyst) * cr.zoom + pInfo->_8;
                    if (cd2 > lim * lim && pd2 > lim * lim && pInfo->mActor != cr.playerActor && port_idle25_starpiece_nerve(pInfo->mActor) == 1) {
                        pCrModel = MR::getJ3DModel(pInfo->mActor);
                        if (pCrModel != nullptr) {
                            ++sClCnt[6];
                            {
                                const uint32_t pseed = static_cast< uint32_t >(reinterpret_cast< uintptr_t >(pInfo->mActor) >> 4);
                                cull = !port::cpulod::PieceKept(pseed, port_cpulod_piece_n_for(pseed));  // (q30lock26: the bit's own level: a walk thins them one after the other, never half in one picture)
                            }
                            if (cull) {
                                ++sClCnt[7];
                            }
                        }
                    }
                } else if (!clip && (crDrawOn || (fadeLive && cr.lever))) {
                    const TVec3f& cc = *pInfo->_4;
                    const float cd2 = D2(cc, cr.cam);
                    const float reach = cr.nearD * cr.zoom + pInfo->_8;
                    const bool kindOk = CrDrawAllowed(pInfo, cr.playerActor, crDrawKinds);
                    const bool ffOk = ffH > 0.0f && port::cpucuts::Allowed(pInfo->mCrCls, port::cpucuts::kCbDecor | port::cpucuts::kCbDecorFx);   // (far30: decoration and props only)
                    if (cd2 > reach * reach && (kindOk || ffOk)) {
                        pCrModel = MR::getJ3DModel(pInfo->mActor);
                        if (pCrModel != nullptr) {
                            const float camD = sqrtf(cd2), plD = sqrtf(D2(cc, cr.player));
                            const bool was = pInfo->mCrDraw != 0 || (fadeLive && sThinFade.Wants(reinterpret_cast< uintptr_t >(pInfo->mActor)));
                            const bool playerFar = CrPlayerFar(cr, plD, pInfo->_8, was);
                            cull = playerFar &&
                                   DrawCulled(ProjLines(pInfo->_8, camD, cr.tanHalf),
                                              DrawThreshold(crDrawLines, cr.drawRamp, Jitter01(reinterpret_cast< uintptr_t >(pInfo->mActor))), was, cr.hyst);
                            if (!cull && playerFar && port::cpucuts::HoldKeeps(pInfo->mCrDraw, crDrawHold)) {
                                cull = true;  // (cpucuts25, GXV_FARDRAW_HOLD: a stamp is kept for its minimum age while the actor is still far)
                                ++sCrDrawHeldOn;
                            }
                            if (ffOk && playerFar) {
                                // far30 (GXV_FARFIELD tier 4, Quality step 3k): REMOVAL IS THE LAST RESORT, so it takes only what is already tiny on the picture: an object whose whole height is under
                                // GXV_FARFIELD_PX (12) pixels of 544 (the census: nothing under 8 px is drawn at all, 8..32 px is where the far decoration lives; 12 is where a shape is a dot that a
                                // 20 m fade hides), beyond the draw horizon (its edge spread over the fade band by the object's own hash) and, as MayCut says, never inside 25 m of the camera or
                                // the player and never a protected class. It goes through THINFADE's shrink-fade (a few tenths of a second). Hysteresis: it returns 6 % nearer or 30 % bigger, and,
                                // having returned, stays for 4 s whatever its distance (no flicker at an edge). In a flight the distances are the PLAYER's (the camera races past; the player is where
                                // play will be): the set of removed objects is then the one play will start with, and does not change while the camera moves.
                                const float dView = fly ? plD : camD;
                                const float lines = ProjLines(pInfo->_8, dView, cr.tanHalf);
                                const float hPx = 2.0f * lines * (0.5f * 544.0f / 456.0f);
                                const float pxLimit = ffPx * (was ? 1.3f : 1.0f);
                                if (!was && pInfo->mFfDwell > 0) {
                                    --pInfo->mFfDwell;
                                } else if (hPx < pxLimit) {
                                    port::farfield::Subject fs;
                                    fs.camD = fly ? plD : camD;
                                    fs.plD = plD;
                                    fs.radius = pInfo->_8;
                                    fs.radiusPx = 0.5f * hPx;
                                    const float hi = ffH * cr.zoom + port::farfield::kFadeBand * Jitter01(reinterpret_cast< uintptr_t >(pInfo->mActor));
                                    const bool ffCull = port::farfield::MayCut(fs, hi, was, port_ff_draw_last() != 0);
                                    if (ffCull) {
                                        cull = true;
                                        ++sFfFaded;
                                    }
                                }
                                if (cull != was) {   // (the pop census: an object that changed between drawn and not drawn, and the biggest one's height)
                                    ++sFfPop;
                                    if (hPx > sFfPopPx) {
                                        sFfPopPx = hPx;
                                    }
                                    if (was && !cull) {
                                        pInfo->mFfDwell = 240;
                                    }
                                }
                            }
                            if (cull) {
                                ++sClCnt[7];
                            }
                        }
                    }
                } else if (!clip && (crDrawOn || (fadeLive && cr.lever))) {
                    const TVec3f& cc = *pInfo->_4;
                    const float cd2 = D2(cc, cr.cam);
                    const float reach = cr.nearD * cr.zoom + pInfo->_8;
                    const bool kindOk = CrDrawAllowed(pInfo, cr.playerActor, crDrawKinds);
                    const bool ffOk = ffH > 0.0f && port::cpucuts::Allowed(pInfo->mCrCls, port::cpucuts::kCbDecor | port::cpucuts::kCbDecorFx);   // (far30: decoration and props only)
                    if (cd2 > reach * reach && (kindOk || ffOk)) {
                        pCrModel = MR::getJ3DModel(pInfo->mActor);
                        if (pCrModel != nullptr) {
                            const float camD = sqrtf(cd2), plD = sqrtf(D2(cc, cr.player));
                            const bool was = pInfo->mCrDraw != 0 || (fadeLive && sThinFade.Wants(reinterpret_cast< uintptr_t >(pInfo->mActor)));
                            const bool playerFar = CrPlayerFar(cr, plD, pInfo->_8, was);
                            cull = playerFar &&
                                   DrawCulled(ProjLines(pInfo->_8, camD, cr.tanHalf),
                                              DrawThreshold(crDrawLines, cr.drawRamp, Jitter01(reinterpret_cast< uintptr_t >(pInfo->mActor))), was, cr.hyst);
                            if (!cull && playerFar && port::cpucuts::HoldKeeps(pInfo->mCrDraw, crDrawHold)) {
                                cull = true;  // (cpucuts25, GXV_FARDRAW_HOLD: a stamp is kept for its minimum age while the actor is still far)
                                ++sCrDrawHeldOn;
                            }
                            if (!cull && ffOk && playerFar) {
                                // far30 (GXV_FARFIELD tier 4): a SMALL decoration or prop beyond the draw distance (its own edge spread over the fade band, so a crowd leaves one by one, never in a ring)
                                // goes through the same fade as the other far cuts (GXV_THINFADE: it shrinks away over a few tenths of a second, then is not drawn). port::farfield::MayCut is the
                                // ordering rule: never inside 25 m of the camera or the player, never over 64 px, never a protected class (this branch only sees decoration).
                                const float lines = ProjLines(pInfo->_8, camD, cr.tanHalf);
                                if (lines < 24.0f) {
                                    port::farfield::Subject fs;
                                    fs.camD = camD;
                                    fs.plD = plD;
                                    fs.radius = pInfo->_8;
                                    fs.radiusPx = lines * (0.5f * 544.0f / 456.0f);
                                    const float hi = ffH * cr.zoom + port::farfield::kFadeBand * Jitter01(reinterpret_cast< uintptr_t >(pInfo->mActor));
                                    cull = port::farfield::MayCut(fs, hi, was, port_ff_draw_last() != 0);
                                    if (cull) {
                                        ++sFfFaded;
                                    }
                                }
                            }
                        }
                    }
                }
                if (cull && pCrModel != nullptr && __builtin_expect(port::popin::GuardOn(pieceCand ? port::popin::kFlStarBit : port::popin::kFlSpeck), 0)) {
                    // popin31: GXV_PB_MINPX: a far speck / a star bit is left out only while it is under the floor on the picture (its size: the larger of its clipping
                    // sphere and its model's extent; the minimum-age hold and the far field's own rule included); GXV_PB_DEMO=2's release after a flight.
                    // popin31b: their own floors, GXV_PB_MINPX_SPECK / GXV_PB_MINPX_STARBIT
                    const TVec3f& fc = *pInfo->_4;
                    const TVec3f& fp = pInfo->mActor->mPosition;
                    const float ox = fc.x - fp.x, oy = fc.y - fp.y, oz = fc.z - fp.z;
                    const float fr = port::popin::SizeRadius(pInfo->_8, port::popin::CachedExtent(pInfo->mPortFloorExt, pInfo->mPortFloorAt, pInfo->mActor),
                                                             sqrtf(ox * ox + oy * oy + oz * oz));
                    const float dx = fc.x - port_pb_floor.cam[0], dy = fc.y - port_pb_floor.cam[1], dz = fc.z - port_pb_floor.cam[2];
                    cull = port::popin::CutAllowed2(pieceCand ? port::popin::kPieceThin : port::popin::kFarSpeck, pInfo->mCrDraw != 0, fr, dx * dx + dy * dy + dz * dz,
                                                    fc.x, fc.y, fc.z);
                }
                if (fadeOn && !clip) {
                    // q30fin26 (GXV_THINFADE): the cut's wish goes through the fade; only an item shrunk to its floor is stamped "not drawn". An item that is no longer a candidate (pCrModel null) grows back fast.
                    const uintptr_t fkey = reinterpret_cast< uintptr_t >(pInfo->mActor);
                    if (pCrModel != nullptr && (cull || sThinFade.Live() > 0)) {
                        bool created = false;
                        const bool hidden = sThinFade.Judge(fkey, cull, true, sThinFadeTurn, &created);
                        if (created) {
                            pCrModel->mPortFade = 1;
                        }
                        cull = hidden;
                    } else if (pCrModel == nullptr && sThinFade.Live() > 0 && sThinFade.Find(fkey) != nullptr) {
                        sThinFade.Judge(fkey, false, false, sThinFadeTurn);
                    }
                }
                if (cull && (pCrModel->mPortCullPicture != cr.next || pInfo->mCrDraw != 0)) {  // (a stamp of another cull is left to it)
                    pCrModel->mPortCullPicture = cr.next;
                    sCrDrawNew += pInfo->mCrDraw == 0 ? 1u : 0u;
                    if (pInfo->mCrDraw == 0 && port::popin::LogOn()) {  // (popin31, the census: the far-speck / star-bit cut takes a thing)
                        port_popin_event(pieceCand ? port::popin::kPieceThin : port::popin::kFarSpeck, 0, pInfo->mActor, pInfo->_4, -1.0f);
                    }
                    pInfo->mCrDraw = port::cpucuts::AgeNext(pInfo->mCrDraw);  // (1 when new, then the age in turns; non-zero = stamped, as before)
                    pCrModel->mPortFarCulled = 1;                             // (cpucuts25: the pose flush may leave this model out)
                    ++sCrDrawHeld;
                    ++sFfHeld;
                    const J3DModelData* pData = pCrModel->getModelData();
                    sCrDrawShapes += pData != nullptr ? pData->getShapeNum() : 0;
                    if (pieceCand) {
                        sClCnt[8] += pData != nullptr ? pData->getShapeNum() : 0;  // (cpulod26 census)
                    }
                } else {
                    if (pInfo->mCrDraw != 0 && !clip && port::popin::LogOn()) {  // (popin31, the census: ... and gives it back in the picture's view)
                        port_popin_event(pInfo->mActor != nullptr && pInfo->mActor->mFlFam == 1 ? port::popin::kPieceThin : port::popin::kFarSpeck, 1, pInfo->mActor,
                                         pInfo->_4, -1.0f);
                    }
                    CrDrawRelease(pInfo, cr.next);
                }
            }
            ++judged;
        }
        if (hzMode != 0) {
            HzTurnEnd(hz);
        }
        if (crClipOn || crDrawOn || pieceOn) {
            ++sCrTurns;
            CrLog(cr);
        }
        if (fadeOn) {
            ThinFadeLog();
        }
        ZoomCullLog();
        TinyCullLog();
        sClipJudged += judged;
#if PORT_LOGGING
        if (PortOcDue(sClipLast, sClipTick)) {
            port_log_summary("oc-clip: judged %u mismatches %u", sClipJudged, sClipMismatches);
            sClipJudged = 0;  // (mismatches stay: a total since boot)
        }
#endif
        return;
    }
#endif

#ifdef TARGET_PC
    const bool prefetch = port_upd_prefetch_on() != 0;
#endif
    for (s32 i = 0; i < _10->_4; i++) {
#ifdef TARGET_PC
        if (prefetch) {
            ClipPrefetch(_10->mClippingActorList, i, _10->_4);
        }
        ClippingActorInfo* pInfo = _10->mClippingActorList[i];
        pInfo->judgeClipping();
        ZoomCullJudge(pInfo, MR::isClipped(pInfo->mActor), zoomTurn);
#else
        _10->mClippingActorList[i]->judgeClipping();
#endif
    }
#ifdef TARGET_PC
    ZoomCullLog();
#endif
}

void ClippingActorHolder::registerActor(LiveActor* pActor) {
    _18->add(new ClippingActorInfo(pActor));

    mActorNum++;
}

void ClippingActorHolder::initSystemInfo(LiveActor* pActor, const JMapInfoIter& rIter) {
    s32 viewGroupID = -1;

    if (MR::getJMapInfoViewGroupID(rIter, &viewGroupID)) {
        mViewGroupCtrl->initActorInfo(find(pActor), viewGroupID);
    }
}

void ClippingActorHolder::initViewGroupTable() {
    mViewGroupCtrl->startInitViewGroupTable();
    mViewGroupCtrl->initViewGroup(_10);
    mViewGroupCtrl->initViewGroup(_18);
    mViewGroupCtrl->initViewGroup(_1C);
    mViewGroupCtrl->initViewGroup(_14);
    mViewGroupCtrl->endInitViewGroupTable();
}

void ClippingActorHolder::entryLodCtrl(LodCtrl* pLodCtrl, const JMapInfoIter& rIter) {
    mViewGroupCtrl->entryLodCtrl(pLodCtrl, rIter);
}

void ClippingActorHolder::validateClipping(LiveActor* pActor) {
    pActor->mFlag.mIsInvalidClipping = false;

    ClippingActorInfo* pActorInfo = _14->remove(pActor);

    if (MR::isDead(pActor)) {
        _18->add(pActorInfo);
    } else if (pActorInfo->isGroupClipping()) {
        _1C->add(pActorInfo);
    } else {
        _10->add(pActorInfo);
    }
}

void ClippingActorHolder::invalidateClipping(LiveActor* pActor) {
    pActor->mFlag.mIsInvalidClipping = true;

    ClippingActorInfo* pActorInfo;

    if (MR::isDead(pActor)) {
        pActorInfo = _18->remove(pActor);
    } else if (_10->isInList(pActor)) {
        pActorInfo = _10->remove(pActor);
    } else {
        pActorInfo = _1C->remove(pActor);
    }

    _14->add(pActorInfo);
#ifdef TARGET_PC
    ZoomUncull(pActorInfo);  // (GXV_ZOOM_DRAWCULL: no longer judged)
    HzRelease(pActorInfo, J3DPortPicture() + 1);  // (GXV_HORIZON)
    CrRelease(pActorInfo, J3DPortPicture() + 1);  // (cliprange25)
#endif

    if (MR::isClipped(pActor)) {
        pActor->endClipped();
    }
}

void ClippingActorHolder::addToClippingTarget(LiveActor* pActor) {
    if (MR::isInvalidClipping(pActor) || _10->isInList(pActor) || _1C->isInList(pActor)) {
        return;
    }

    ClippingActorInfo* pActorInfo = _18->remove(pActor);

    if (pActorInfo->isGroupClipping()) {
        _1C->add(pActorInfo);
    } else {
        _10->add(pActorInfo);
    }
}

void ClippingActorHolder::removeFromClippingTarget(LiveActor* pActor) {
    if (MR::isInvalidClipping(pActor)) {
        return;
    }

    if (_18->isInList(pActor)) {
        return;
    }

    ClippingActorInfo* pActorInfo;

    if (_1C->isInList(pActor)) {
        pActorInfo = _1C->remove(pActor);
    } else {
        pActorInfo = _10->remove(pActor);
    }

    _18->add(pActorInfo);
#ifdef TARGET_PC
    ZoomUncull(pActorInfo);  // (GXV_ZOOM_DRAWCULL: no longer judged)
    HzRelease(pActorInfo, J3DPortPicture() + 1);  // (GXV_HORIZON)
    CrRelease(pActorInfo, J3DPortPicture() + 1);  // (cliprange25)
#endif
}

ClippingActorInfo* ClippingActorHolder::startGroupClipping(LiveActor* pActor, const JMapInfoIter& rIter) {
    ClippingActorInfo* pActorInfo = nullptr;

    if (_10->isInList(pActor)) {
        pActorInfo = _10->remove(pActor);

        _1C->add(pActorInfo);
#ifdef TARGET_PC
        ZoomUncull(pActorInfo);  // (GXV_ZOOM_DRAWCULL: judged by its group from now on)
        HzRelease(pActorInfo, J3DPortPicture() + 1);  // (GXV_HORIZON)
        CrRelease(pActorInfo, J3DPortPicture() + 1);  // (cliprange25)
#endif
    } else if (_18->isInList(pActor)) {
        pActorInfo = _18->find(pActor, 0);
    } else if (_14->isInList(pActor)) {
        pActorInfo = _14->remove(pActor);

        _1C->add(pActorInfo);
        pActor->endClipped();
    }

    pActorInfo->setGroupClippingNo(rIter);

    return pActorInfo;
}

void ClippingActorHolder::setTypeToSphere(LiveActor* pActor, f32 range, const TVec3f* pCenter) {
    find(pActor)->setTypeToSphere(range, pCenter);
}

void ClippingActorHolder::setFarClipLevel(LiveActor* pActor, s32 level) {
    find(pActor)->setFarClipLevel(level);
}

ClippingActorInfo* ClippingActorHolder::find(const LiveActor* pActor) const {
    ClippingActorInfo* pActorInfo;

    pActorInfo = _10->findOrNone(pActor);

    if (pActorInfo != nullptr) {
        return pActorInfo;
    }

    pActorInfo = _18->findOrNone(pActor);

    if (pActorInfo != nullptr) {
        return pActorInfo;
    }

    pActorInfo = _1C->findOrNone(pActor);

    if (pActorInfo != nullptr) {
        return pActorInfo;
    }

    return _14->find(pActor, 0);
}

#ifdef TARGET_PC
extern "C" int port_horizon_pose_on(void) { return HzMode() == 1 && HzPoseOn(); }  // (cpu2-26: the pose flush leaves out the hidden models of GXV_HORIZON=1)
#endif
