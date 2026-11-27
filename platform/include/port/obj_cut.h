// objcut (7 Nov): helpers of the exact per-turn game-logic cuts GXV_OC_LIGHTFIND and GXV_OC_CLIPFAST.
//
//  - port_area_gen counts the changes of any AreaObj's validity, wake state or follow matrix, and the registration of areas
//    (decomp/src/Game/AreaObj/LightAreaHolder.cpp defines it). A memo made in one generation is used only in that generation.
//  - PortOcDue(last): true about every 5 s, for the verify lines ("oc-lightfind: ..." / "oc-clip: ..."). The clock is read once
//    per 256 calls, so the counting paths pay a counter increment.
#pragma once

#include <cstdint>

extern "C" uint32_t port_area_gen;
extern "C" int64_t port_host_time_ns(void);

static inline void PortAreaBump() {
    ++port_area_gen;
}
// The field takes the value; the generation moves only when the value changes (writing the same value changes nothing).
template < class T > static inline void PortAreaSet(T& rField, T value) {
    if (rField != value) {
        rField = value;
        ++port_area_gen;
    }
}

// GXV_AREA_GRID (logic23, 8 Nov; AreaObj.cpp): port_area_geo_gen counts the changes of what the area grids and the manager-name memo
// are built from: a manager's array and its order (AreaObjMgr::entry, the sorts of CubeCameraMgr, ImageEffectAreaMgr and
// LightAreaHolder), a follow matrix (AreaObj::setFollowMtx), a cube's matrix and bounds (AreaFormCube::updateBoxParam) and a new area
// container (its managers). Validity and wake state are not in it: the lookups read them live.
extern "C" uint32_t port_area_geo_gen;
static inline void PortAreaGeoBump() {
    ++port_area_geo_gen;
}

static inline bool PortOcDue(int64_t& rLast, uint32_t& rTick) {
    if ((++rTick & 255u) != 0) {
        return false;
    }
    const int64_t now = port_host_time_ns();
    if (rLast == 0) {
        rLast = now;
        return false;
    }
    if (now - rLast < 5000000000ll) {
        return false;
    }
    rLast = now;
    return true;
}
