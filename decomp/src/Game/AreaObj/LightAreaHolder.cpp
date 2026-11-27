#include "Game/AreaObj/LightAreaHolder.hpp"
#include "Game/AreaObj/LightArea.hpp"
#include "Game/Map/LightFunction.hpp"
#include "Game/Map/LightZoneDataHolder.hpp"
#include "Game/AreaObj/AreaForm.hpp"
#ifdef TARGET_PC
#include "port/diagnostics.h"
#include "port/hot_knob.h"
#include "port/obj_cut.h"
#include <cstdlib>
#include <cstring>
extern "C" int port_on_frame_thread(void);

// GXV_OC_LIGHTFIND (objcut, 7 Nov; default 1, exact): the light-area lookup of every actor with a light controller
// (ActorLightCtrl::update -> LightFunction::tryFindNewAreaLightID -> LightAreaHolder::tryFindLightID -> AreaObjMgr::find_in) is asked
// every turn at the actor's position, and a map object, plant, coin or standing NPC does not move: the walk over the holder's areas
// (a virtual isInVolume each, ~12 per lookup in Honeyhive) returns the same area every turn. The lookup is a pure function of the
// position and of the areas: each area's validity (mIsValid, _15, mIsAwake), its form (set at init; a follow matrix _4 moves it), the
// array and its order. With the position the same bit for bit and the area state unchanged (port_area_gen, below), the stored area
// pointer is what the walk returns.
//   Writers of that state, and where they move port_area_gen: AreaObj::validate / invalidate / awake / sleep (AreaObj.hpp, when the
//   value changes; stage switches and the sleep controllers call these), AreaObjFollower::update (_15), AreaObj::setFollowMtx,
//   AreaObjMgr::entry, LightAreaHolder::sort and the holder's constructor. A light area is a LightArea, which writes none of these
//   fields itself. A holder with any area that follows a matrix (form _4 set: its volume moves every turn) is never memoised.
//   0  off: the walk, as before.
//   1  on.
//   2  verify: every hit is also walked; a different area is a mismatch (counted, logged; the walk's answer is used).
// Game thread only. Logging builds write, about every 5 s, to log.txt:
//   oc-lightfind: lookups L hits H (P%) walks W mismatches X static-holder N
uint32_t port_area_gen = 1;

namespace {
int LightFindMode() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_OC_LIGHTFIND");
        return e && *e ? static_cast< int >(strtol(e, nullptr, 0)) : 0;
    }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}

struct LightFindEntry {
    uint32_t gen;  // 0: empty (port_area_gen starts at 1)
    const void* holder;
    uint32_t px, py, pz;
    AreaObj* result;
};
LightFindEntry sLightFind[256];
uint32_t sLfLookups, sLfHits, sLfWalks, sLfMismatches, sLfStaticOff, sLfTick;
int64_t sLfLast;
// "No area of this holder follows a matrix", worked out once per generation (a follow matrix is set through setFollowMtx, which bumps).
const void* sStaticHolder;
uint32_t sStaticGen;
bool sStaticValue;

inline uint32_t Bits(f32 v) {
    uint32_t b;
    memcpy(&b, &v, sizeof b);
    return b;
}
inline uint32_t LightFindSlot(uint32_t x, uint32_t y, uint32_t z) {
    uint32_t h = x * 0x9E3779B1u;
    h = (h ^ (h >> 15) ^ y) * 0x85EBCA77u;
    h = (h ^ (h >> 13) ^ z) * 0xC2B2AE3Du;
    return (h >> 24) & 255u;
}
void LightFindLog() {
#if PORT_LOGGING
    if (!PortOcDue(sLfLast, sLfTick)) return;
    port_log_summary("oc-lightfind: lookups %u hits %u (%.1f%%) walks %u mismatches %u static-holder-off %u", sLfLookups, sLfHits,
                     sLfLookups ? 100.0 * sLfHits / sLfLookups : 0.0, sLfWalks, sLfMismatches, sLfStaticOff);
    sLfLookups = sLfHits = sLfWalks = sLfStaticOff = 0;  // (mismatches stay: a total since boot)
#endif
}
}  // namespace

AreaObj* LightAreaHolder::portFindIn(const TVec3f& rPos) const {
    const int mode = LightFindMode();
    if (mode == 0 || !port_on_frame_thread()) {
        return find_in(rPos);
    }
    ++sLfLookups;
    LightFindLog();
    const uint32_t gen = port_area_gen;
    if (sStaticHolder != this || sStaticGen != gen) {
        bool isStatic = true;
        for (s32 i = 0; i < mArray.size(); i++) {
            if (mArray[i]->mForm == nullptr || mArray[i]->mForm->_4 != nullptr) {
                isStatic = false;
                break;
            }
        }
        sStaticHolder = this;
        sStaticGen = gen;
        sStaticValue = isStatic;
    }
    if (!sStaticValue) {
        ++sLfStaticOff;
        return find_in(rPos);
    }
    const uint32_t px = Bits(rPos.x), py = Bits(rPos.y), pz = Bits(rPos.z);
    LightFindEntry& e = sLightFind[LightFindSlot(px, py, pz)];
    const bool hit = e.gen == gen && e.holder == this && e.px == px && e.py == py && e.pz == pz;
    if (hit && mode == 1) {
        ++sLfHits;
        return e.result;
    }
    AreaObj* pArea = find_in(rPos);
    ++sLfWalks;
    if (hit) {  // (mode 2)
        ++sLfHits;
        if (e.result != pArea) {
#if PORT_LOGGING
            if (sLfMismatches < 16) {
                port_log_summary("oc-lightfind: MISMATCH at (%g %g %g): memo %p walk %p", rPos.x, rPos.y, rPos.z, static_cast< void* >(e.result),
                                 static_cast< void* >(pArea));
            }
#endif
            ++sLfMismatches;
        }
    }
    if (port_area_gen == gen) {  // (the walk changes nothing; checked anyway, so an entry never outlives its areas)
        e.gen = gen;
        e.holder = this;
        e.px = px;
        e.py = py;
        e.pz = pz;
        e.result = pArea;
    }
    return pArea;
}
#endif

LightAreaHolder::LightAreaHolder(s32 maxNum, const char* pName) : AreaObjMgr(maxNum, pName) {
#ifdef TARGET_PC
    PortAreaBump();  // (GXV_OC_LIGHTFIND: a new holder, a new scene's areas)
#endif
    LightFunction::registerLightAreaHolder(this);
}

bool LightAreaHolder::tryFindLightID(const TVec3f& rArea, ZoneLightID* pLightID) const {
#ifdef TARGET_PC
    AreaObj* pArea = portFindIn(rArea);  // (GXV_OC_LIGHTFIND)
#else
    AreaObj* pArea = find_in(rArea);
#endif

    if (pArea == nullptr) {
        if (pLightID->isOutOfArea()) {
            pLightID->clear();

            return false;
        } else {
            pLightID->clear();

            return true;
        }
    } else {
        const LightArea* pLightArea = static_cast< LightArea* >(pArea);

        if (pLightID->isTargetArea(pLightArea)) {
            return false;
        } else {
            pLightID->_0 = pLightArea->mPlacedZoneID;
            pLightID->mLightID = pLightArea->mObjArg0;

            return true;
        }
    }
}

void LightAreaHolder::initAfterPlacement() {
    sort();
}

void LightAreaHolder::sort() {
#ifdef TARGET_PC
    PortAreaBump();  // (GXV_OC_LIGHTFIND: the order of the areas)
    PortAreaGeoBump();  // (GXV_AREA_GRID: the order of the areas)
#endif
    for (s32 i = 0; i < mArray.size() - 1; i++) {
        AreaObj* pOriginal = mArray[i];
        AreaObj* pSelected = pOriginal;
        s32 selected = i;

        for (s32 j = i + 1; j < mArray.size(); j++) {
            AreaObj* pCandidate = mArray[j];

            if (pSelected->mObjArg1 > pCandidate->mObjArg1) {
                selected = j;
                pSelected = pCandidate;
            }
        }

        if (i != selected) {
            mArray[i] = pSelected;
            mArray[selected] = pOriginal;
        }
    }
}
