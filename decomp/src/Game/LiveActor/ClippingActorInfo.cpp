#include "Game/LiveActor/ClippingActorInfo.hpp"
#include "Game/LiveActor/ClippingJudge.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/LiveActor/ViewGroupCtrl.hpp"
#include "Game/Util.hpp"

ClippingActorInfo::ClippingActorInfo(LiveActor* pActor) {
    mActor = pActor;
    _4 = 0;
    _8 = 0.0f;
    mInfo = 0;
    mFarClipLevel = 6;
    _12 = -1;
    _14 = 0;
    setTypeToSphere(300.0f, 0);
}

void ClippingActorInfo::judgeClipping() {
    if (isJudgedToClip()) {
        if (!MR::isClipped(mActor)) {
            mActor->startClipped();
        }
    } else {
        if (MR::isClipped(mActor)) {
            mActor->endClipped();
        }
    }
}

extern "C" int port_safe_cam_exists(void);  // (LodCtrl.cpp: no CameraContext yet in the star select / file select)
#ifdef TARGET_PC
extern "C" float port_pb_decor_dist(void);
extern "C" float port_detail_dist(int rule, float base);  // (detail25: the level's sliding camera distance)
extern "C" float port_pb_player_dist2(float x, float y, float z);
extern "C" float port_popin_zoom(void);  // (popin24: LodCtrl.cpp)
extern "C" float port_pb_scale(void);    // (popin25: LodCtrl.cpp; the zoom ratio times GXV_PB_DEMO's widening in a flight)
extern "C" int port_pb_demo_off(void);   // (popin25: GXV_PB_DEMO=2 in a flight: the budget is off)
extern "C" float port_pb_lead(float limit);  // (popin25: GXV_POPIN_LEAD: appear earlier for a fast camera)
#include "port/popin.h"
#include "port/popin_site.h"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/MapObj/PlantGroup.hpp"  // (popin31b: the group's mPortFloorKept, the census's price line)
// GXV_PB_DECOR: a decoration (mPbDecor: set only by PlantGroup::initAfterPlacement, so the actor IS a PlantGroup: ClippingActorHolder's
// C185 reveal static_casts on it; a new setter must keep that true) whose sphere is farther than the distance from the camera, and whose
// centre is farther than 2500 from the player, is clipped like the game's own far clip. 0: never.
extern "C" int port_ff_flyover(void);        // (far3: LodCtrl.cpp)
extern "C" const TVec3f* port_safe_player_pos(void);  // (LodCtrl.cpp)
extern "C" float port_ff_decor_spread(void); // (far3: perf.cpp; 0 = off)
float PortDecorScale(const ClippingActorInfo* pInfo);
bool PortPbDecorFar(const ClippingActorInfo* pInfo, float dist, float camX, float camY, float camZ) {
    const TVec3f& c = *pInfo->_4;
    // (popin24) GXV_POPIN_ZOOM: the distances of a zoomed picture are seen at the size they were at zoom 1.0 (1 when off);
    // GXV_POPIN_HYST: a group already held far returns popin-pct nearer than it was held (the state is idempotent for one position).
    if (port_pb_demo_off()) {
        pInfo->mPbFar = false;  // (popin25, GXV_PB_DEMO=2: no cut in a flight)
        static_cast< PlantGroup* >(pInfo->mActor)->mPortFloorKept = 0;
        return false;
    }
    const float zoom = port_pb_scale();  // (the zoom, popin25: times the flight's widening)
    const float hystPct = port::popin::HystPct();
    const bool was = pInfo->mPbFar;
    float limit = port_detail_dist(0, dist) * zoom + port_pb_lead(dist * zoom);  // (popin25: + GXV_POPIN_LEAD's margin, 0 when off)
    {   // far3 / far30: the far field decides a group ONCE, by a stable hash of the group: its own distance is the horizon +- the spread, so a crowd leaves one by one over the band and the same
        // flowers are always the missing ones (no flicker as the camera moves). In a flight the distance is the PLAYER's, not the flying camera's.
        const float spread = port_ff_decor_spread();
        if (spread > 0.0f) {
            const uint32_t h = static_cast< uint32_t >(reinterpret_cast< uintptr_t >(pInfo) >> 4) * 2654435761u;
            limit += ((static_cast< float >(h >> 8) / 16777216.0f) - 0.5f) * 2.0f * spread;
            if (port_ff_flyover() != 0) {
                const TVec3f* pl = port_safe_player_pos();
                if (pl != nullptr) {
                    camX = pl->x;
                    camY = pl->y;
                    camZ = pl->z;
                }
            }
        }
    }
    const float reach = (was ? limit - port::popin::Band(limit, hystPct) : limit) + pInfo->_8;
    const float dx = c.x - camX, dy = c.y - camY, dz = c.z - camZ;
    bool far = dx * dx + dy * dy + dz * dz > reach * reach;
    if (far) {
        const float pl = 2500.0f * zoom;
        const float plReach = was ? pl - port::popin::Band(pl, hystPct) : pl;
        far = port_pb_player_dist2(c.x, c.y, c.z) > plReach * plReach;
    }
    u8 kept = 0;  // (popin31b: the floor kept the group)
    if (far && __builtin_expect(port::popin::GuardOn(port::popin::kFlDecor), 0)) {
        // popin31: GXV_PB_MINPX: what pops is ONE member (a flower, a bush), at the group's nearest member, measured from the real camera (the far field may
        // have measured from the player in a flight); GXV_PB_DEMO=2's release: a group in the picture is not taken right after a flight.
        // popin31b: the plant groups' own floor (GXV_PB_MINPX_DECOR); a group the floor kept is marked for the census's price line
        float nearest = 0.0f;
        const float gx = c.x - port_pb_floor.cam[0], gy = c.y - port_pb_floor.cam[1], gz = c.z - port_pb_floor.cam[2];
        const float memberR = port::popin::PlantMember(__builtin_sqrtf(gx * gx + gy * gy + gz * gz), pInfo->_8, PortDecorScale(pInfo), &nearest);
        const int v = port::popin::CutVerdict(port::popin::kDecor, was, memberR, nearest, c.x, c.y, c.z, pInfo->_8);
        far = v == port::popin::kCutOk;
        kept = v == port::popin::kCutFloor ? 1 : 0;
    }
    static_cast< PlantGroup* >(pInfo->mActor)->mPortFloorKept = kept;
    pInfo->mPbFar = far;
    return far;
}
// popin31: the group's scale for its members' size (the largest axis; 1 when unknown)
float PortDecorScale(const ClippingActorInfo* pInfo) {
    const LiveActor* a = pInfo->mActor;
    if (a == nullptr) {
        return 1.0f;
    }
    const float sx = __builtin_fabsf(a->mScale.x), sy = __builtin_fabsf(a->mScale.y), sz = __builtin_fabsf(a->mScale.z);
    const float s = sx > sy ? (sx > sz ? sx : sz) : (sy > sz ? sy : sz);
    return s > 0.0f ? s : 1.0f;
}
#endif

bool ClippingActorInfo::isJudgedToClip() const {
    s32 farClipLevel = mFarClipLevel;
    if (_14->_0 == 1) {
        farClipLevel = 0;
    }

    f32 val = _8;
    ClippingJudge* judge = MR::getClippingJudge();
#ifdef TARGET_PC
    if (mPbDecor) {
        const float pbDist = port_pb_decor_dist();
        if (pbDist > 0.0f && port_safe_cam_exists()) {
            const TVec3f cam = MR::getCamPos();
            if (PortPbDecorFar(this, pbDist, cam.x, cam.y, cam.z)) {
                return true;
            }
        }
    }
#endif
    return judge->isJudgedToClipFrustum(*_4, val, farClipLevel);
}

bool ClippingActorInfo::isGroupClipping() const {
    return mInfo;
}

void ClippingActorInfo::setTypeToSphere(f32 radius, const TVec3f* pCenter) {
    _8 = radius;
    if (!pCenter) {
        _4 = &mActor->mPosition;
    } else {
        _4 = pCenter;
    }
}

void ClippingActorInfo::setGroupClippingNo(const JMapInfoIter& rIter) {
    mInfo = new JMapIdInfo(MR::createJMapIdInfoFromClippingGroupId(rIter));
}

ClippingActorInfoList::ClippingActorInfoList(int a1) {
    _0 = a1;
    _4 = 0;
    mClippingActorList = 0;

    mClippingActorList = new ClippingActorInfo*[a1];

    for (s32 i = 0; i < _0; i++) {
        mClippingActorList[i] = 0;
    }
}

void ClippingActorInfoList::add(ClippingActorInfo* pInfo) {
    mClippingActorList[_4] = pInfo;
    _4++;
}

ClippingActorInfo* ClippingActorInfoList::remove(LiveActor* pActor) {
    s32 index = 0;
    ClippingActorInfo* info = find(pActor, &index);
    s32 lastIndex = _4 - 1;
    mClippingActorList[index] = mClippingActorList[lastIndex];
    _4--;
    return info;
}

ClippingActorInfo* ClippingActorInfoList::find(const LiveActor* pActor, s32* pIndex) const {
    for (s32 i = 0; i < _4; i++) {
        ClippingActorInfo* inf = mClippingActorList[i];

        if (inf->mActor == pActor) {
            if (pIndex) {
                *pIndex = i;
            }

            return mClippingActorList[i];
        }
    }

    return mClippingActorList[0];
}

ClippingActorInfo* ClippingActorInfoList::findOrNone(const LiveActor* pActor) const {
    for (s32 i = _4 - 1; i >= 0; i--) {
        ClippingActorInfo* info = mClippingActorList[i];
        if (info->mActor == pActor) {
            return info;
        }
    }

    return nullptr;
}

bool ClippingActorInfoList::isInList(const LiveActor* pActor) const {
    for (s32 i = 0; i < _4; i++) {
        ClippingActorInfo* inf = mClippingActorList[i];
        if (inf->mActor == pActor) {
            return true;
        }
    }

    return false;
}
