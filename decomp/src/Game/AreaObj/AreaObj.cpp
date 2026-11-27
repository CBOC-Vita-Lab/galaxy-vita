#include "Game/AreaObj/AreaObj.hpp"
#include "Game/AreaObj/AreaForm.hpp"
#include "Game/AreaObj/AreaObjFollower.hpp"
#include "Game/Map/SleepControllerHolder.hpp"
#include "Game/Map/StageSwitch.hpp"
#include "Game/Util/AreaObjUtil.hpp"
#include "Game/Util/JMapUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include <algorithm>
#include <functional.hpp>
#ifdef TARGET_PC
extern "C" void port_logic_count(int what, unsigned n);  // (logic12: logging builds' per-turn counts, platform/src/port/perf.cpp)
#include "port/move_prof.h"  // (hivelogic: GXV_ANIM_PROF=1/3 times the area lookups, "move-sys:" in the basic log)
#include "port/diagnostics.h"
#include "port/hot_knob.h"
#include "port/logic_grid.h"  // (GXV_AREA_GRID, logic23)
#include "port/logic_prof.h"  // (GXV_LOGIC_PROF, logic23: "area-find" system, areas tested)
#include <cstdlib>
extern "C" int port_on_frame_thread(void);
#endif

AreaObj::AreaObj(int formType, const char* pName)
    : NameObj(pName), mFormType(formType), mIsValid(true), _15(true), mIsAwake(true), mObjArg0(-1), mObjArg1(-1), mObjArg2(-1), mObjArg3(-1),
      mObjArg4(-1), mObjArg5(-1), mObjArg6(-1), mObjArg7(-1), mSwitchCtrl() {
    switch (formType) {
    case AreaForm::Type_Cube1:
        mForm = new AreaFormCube(0);
        break;
    case AreaForm::Type_Cube2:
        mForm = new AreaFormCube(1);
        break;
    case AreaForm::Type_Sphere:
        mForm = new AreaFormSphere();
        break;
    case AreaForm::Type_Bowl:
        mForm = new AreaFormBowl();
        break;
    case AreaForm::Type_Cylinder:
        mForm = new AreaFormCylinder();
        break;
    }
}

void AreaObj::init(const JMapInfoIter& rIter) {
    mForm->init(rIter);
    MR::addBaseMatrixFollowerAreaObj(this, rIter);
    MR::getJMapInfoArg0WithInit(rIter, &mObjArg0);
    MR::getJMapInfoArg1WithInit(rIter, &mObjArg1);
    MR::getJMapInfoArg2WithInit(rIter, &mObjArg2);
    MR::getJMapInfoArg3WithInit(rIter, &mObjArg3);
    MR::getJMapInfoArg4WithInit(rIter, &mObjArg4);
    MR::getJMapInfoArg5WithInit(rIter, &mObjArg5);
    MR::getJMapInfoArg6WithInit(rIter, &mObjArg6);
    MR::getJMapInfoArg7WithInit(rIter, &mObjArg7);

    mSwitchCtrl = MR::createStageSwitchCtrl(this, rIter);

    if (mSwitchCtrl->isValidSwitchAppear()) {
        MR::listenNameObjStageSwitchOnOffAppear(this, mSwitchCtrl, MR::Functor(this, &AreaObj::validate), MR::Functor(this, &AreaObj::invalidate));
        mIsValid = false;
    }

    const char* name;

    if (MR::getObjectName(&name, rIter)) {
        setName(name);
    }
#ifdef TARGET_PC
    if (mFormType == AreaForm::Type_Cube1 || mFormType == AreaForm::Type_Cube2) {
        static_cast< AreaFormCube* >(mForm)->mPortOwner = mName;  // (GXV_HL_AREACUBE=2: the name in the verify log)
    }
#endif

    MR::getAreaObjManager(getManagerName())->entry(this);
    MR::getAreaObjManager(getManagerName());
    SleepControlFunc::addSleepControl(this, rIter);
}

bool AreaObj::isInVolume(const TVec3f& rPos) const {
    return isValid() && mForm->isInVolume(rPos);
}

void AreaObj::onSwitchA() {
    mSwitchCtrl->onSwitchA();
}

void AreaObj::offSwitchA() {
    mSwitchCtrl->offSwitchA();
}

bool AreaObj::isOnSwitchA() const {
    return mSwitchCtrl->isOnSwitchA();
}

bool AreaObj::isOnSwitchB() const {
    return mSwitchCtrl->isOnSwitchB();
}

bool AreaObj::isValidSwitchA() const {
    return mSwitchCtrl->isValidSwitchA();
}

bool AreaObj::isValidSwitchB() const {
    return mSwitchCtrl->isValidSwitchB();
}

void AreaObj::setFollowMtx(const TPos3f* pMtx) {
    mForm->_4 = const_cast< TPos3f* >(pMtx);
#ifdef TARGET_PC
    PortAreaBump();  // (GXV_OC_LIGHTFIND: a follow area moves; the holder is no longer static)
    PortAreaGeoBump();  // (GXV_AREA_GRID: the area is no longer static)
#endif
}

TPos3f* AreaObj::getFollowMtx() const {
    return mForm->_4;
}

AreaObjMgr::AreaObjMgr(s32 count, const char* pName) : NameObj(pName), mArray(), _18(count) {
}

void AreaObjMgr::entry(AreaObj* pAreaObj) {
    if (mArray.capacity() == 0) {
        mArray.init(_18);
    }

    mArray.push_back(pAreaObj);
#ifdef TARGET_PC
    PortAreaBump();  // (GXV_OC_LIGHTFIND)
    PortAreaGeoBump();  // (GXV_AREA_GRID)
#endif
}

#ifdef TARGET_PC
// GXV_AREA_GRID (logic23, 8 Nov; default 0; exact; port/logic_grid.h): find_in walks a manager's areas from the end, a virtual
// isInVolume each (two objects, the area and its form, mostly cache misses), until one holds the point: Honeyhive ~120 lookups and
// ~1480 area tests a turn, 0.45-0.6 ms profiled (move-sys). Most areas are cubes that follow nothing, and GXV_HL_AREACUBE (hivelogic)
// already keeps for each a world box that holds every point the cube can accept (proved there). The grid files those boxes into
// cells; a lookup walks, from the end, only the areas filed in the point's cell merged with the others (spheres, cylinders, bowls,
// follow cubes, cubes without a box), and asks each its own isInVolume, unchanged.
// Why exact:
//  - An area left out is a static cube whose box does not hold the point: AreaFormCube::isInVolume says no, so AreaObj::isInVolume
//    says no (isValid() && form), and so does every override (DeathArea, CubeCameraArea, BigBubbleCameraArea, BigBubbleSwitchArea,
//    AstroChangeStageCube: each returns false or AreaObj::isInVolume; 8 Nov audit, the only overrides in decomp/src). isInVolume has
//    no side effect (the overrides only read flags / switches; HL_AREACUBE=2's counter is a diagnostic), so a call left out changes
//    nothing. The walk's order (from the end) and its stop (the first area that holds the point) are kept: the same area comes back.
//  - The grid is rebuilt when port_area_geo_gen (port/obj_cut.h: entries, sorts, follow matrices, cube bounds, a new container) or
//    the array's length has changed; validity and wake state are read live by isInVolume.
//  - A point with a coordinate beyond 1e30 or not finite, a manager with fewer than 6 areas, no grid memory: the plain walk.
//   0  off.
//   1  on (also the manager-name memo in AreaObjContainer::getManager).
//   2  verify: every grid lookup also walks the original way (and every memo hit searches the managers); a different answer is
//      counted and logged, the original's is used. Lines about every 5 s (every build):
//      area-grid: L lookups, G through the grid (T of N areas tested), B builds, P plain; manager memo H of M; mismatches X / Y
uint32_t port_area_geo_gen = 1;
namespace {
struct AreaGridSlot {
    const AreaObjMgr* mgr;
    uint32_t gen;
    port::lgrid::Grid grid;
    port::lgrid::Box* boxes;
    uint8_t* isStatic;
    int cap;
};
AreaGridSlot sAreaGrids[64];
}  // namespace
uint32_t gPortAreaGridStats[10];  // lookups, grid, tested, all, builds, plain, memo lookups, memo hits, grid mismatches, memo mismatches
int64_t gPortAreaGridLast;
uint32_t gPortAreaGridTick;
extern "C" int port_area_grid_mode(void) {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_AREA_GRID");
        const int v = e && *e ? static_cast< int >(strtol(e, nullptr, 0)) : 0;
        return v < 0 ? 0 : v > 2 ? 2 : v;
    }());
}
extern "C" void port_area_grid_log(void) {
    uint32_t* st = gPortAreaGridStats;
    if (!PortOcDue(gPortAreaGridLast, gPortAreaGridTick)) return;
    port_log_summary("area-grid: %u lookups, %u through the grid (%u of %u areas tested), %u builds, %u plain; manager memo %u of %u; "
                     "mismatches %u / %u (GXV_AREA_GRID=%d)",
                     st[0], st[1], st[2], st[3], st[4], st[5], st[7], st[6], st[8], st[9], port_area_grid_mode());
    for (int i = 0; i < 8; i++) {
        st[i] = 0;  // (the mismatches stay: totals since boot)
    }
}

// The area find_in returns, through the grid: *pUsed = 0 when the grid cannot answer (the caller walks as before).
static AreaObj* PortAreaGridFind(const AreaObjMgr* pMgr, const TVec3f& rVec, int* pUsed) {
    *pUsed = 0;
    const s32 n = pMgr->mArray.size();
    if (n < 6 || n > 65535 || !port::lgrid::Finite3(rVec.x, rVec.y, rVec.z, 1e30f) || !port_on_frame_thread()) {
        return nullptr;
    }
    const uint32_t h = (static_cast< uint32_t >(reinterpret_cast< uintptr_t >(pMgr)) >> 3) * 0x9E3779B1u;
    AreaGridSlot* pSlot = nullptr;
    for (int probe = 0; probe < 8; probe++) {
        AreaGridSlot& r = sAreaGrids[(h >> 26) + probe & 63];
        if (r.mgr == pMgr || r.mgr == nullptr) {
            pSlot = &r;
            break;
        }
    }
    if (pSlot == nullptr) {
        // (the table is full of other managers: the oldest scene's slots are taken over by the generation check below)
        for (int probe = 0; probe < 8; probe++) {
            AreaGridSlot& r = sAreaGrids[(h >> 26) + probe & 63];
            if (r.gen != port_area_geo_gen) {
                pSlot = &r;
                break;
            }
        }
        if (pSlot == nullptr) {
            return nullptr;
        }
    }
    AreaGridSlot& slot = *pSlot;
    if (slot.mgr != pMgr || slot.gen != port_area_geo_gen || slot.grid.count != n) {
        slot.mgr = pMgr;
        slot.gen = port_area_geo_gen;
        ++gPortAreaGridStats[4];
        slot.grid.usable = false;
        slot.grid.count = n;
        if (slot.cap < n) {
            port::lgrid::Box* pBoxes = static_cast< port::lgrid::Box* >(std::realloc(slot.boxes, sizeof(port::lgrid::Box) * n));
            if (pBoxes != nullptr) {
                slot.boxes = pBoxes;
            }
            uint8_t* pStatic = static_cast< uint8_t* >(std::realloc(slot.isStatic, static_cast< size_t >(n)));
            if (pStatic != nullptr) {
                slot.isStatic = pStatic;
            }
            if (pBoxes == nullptr || pStatic == nullptr) {
                return nullptr;
            }
            slot.cap = n;
        }
        int nStatic = 0;
        for (s32 i = 0; i < n; i++) {
            const AreaObj* pArea = pMgr->mArray[i];
            slot.isStatic[i] = 0;
            if ((pArea->mFormType != AreaForm::Type_Cube1 && pArea->mFormType != AreaForm::Type_Cube2) || pArea->mForm == nullptr) {
                continue;
            }
            const AreaFormCube* pCube = static_cast< const AreaFormCube* >(pArea->mForm);
            if (pCube->_4 != nullptr || pCube->mPortRejectMode == 0) {
                continue;
            }
            for (int a = 0; a < 3; a++) {
                slot.boxes[i].lo[a] = pCube->mPortRejectLo[a];
                slot.boxes[i].hi[a] = pCube->mPortRejectHi[a];
#ifdef PORT_LGRID_NEG
                const f32 q = 0.125f * (slot.boxes[i].hi[a] - slot.boxes[i].lo[a]);  // (host test's negative control: too small)
                slot.boxes[i].lo[a] += q;
                slot.boxes[i].hi[a] -= q;
#endif
            }
            slot.isStatic[i] = 1;
            nStatic++;
        }
        if (nStatic >= 4) {
            slot.grid.Build(slot.boxes, slot.isStatic, n, 16, 8192);
        }
    }
    if (!slot.grid.usable) {
        return nullptr;
    }
    *pUsed = 1;
    const uint16_t* sb;
    const uint16_t* se;
    slot.grid.Cell(rVec.x, rVec.y, rVec.z, &sb, &se);
    AreaObj* pFound = nullptr;
    unsigned tested = 0;
    AreaObj* const* pArray = pMgr->mArray.begin();
    port::lgrid::ForEachDescending(slot.grid, sb, se, [&](int index) -> bool {
        ++tested;
        if (pArray[index]->isInVolume(rVec)) {
            pFound = pArray[index];
            return true;
        }
        return false;
    });
    gPortAreaGridStats[1]++;
    gPortAreaGridStats[2] += tested;
    gPortAreaGridStats[3] += static_cast< uint32_t >(n);
    PORT_LOGIC_COUNT(PORT_LC_AREA_GRID, 1);
    PORT_LOGIC_COUNT(PORT_LC_AREA_TESTS, tested);
    return pFound;
}
#endif

AreaObj* AreaObjMgr::find_in(const TVec3f& rVec) const {
#ifdef TARGET_PC
    PORT_MOVE_SYS(1);
    PORT_LOGIC_SYS(PORT_LS_AREA_FIND);
    port_logic_count(8, 1);
    port_logic_count(9, static_cast< unsigned >(mArray.size()));  // (an upper bound: it stops at the first hit from the end)
    const int gridMode = port_area_grid_mode();
    AreaObj* pGridFound = nullptr;
    int gridUsed = 0;
    if (gridMode != 0 && port_on_frame_thread()) {  // (the grids and the counters are the game thread's; others walk as before)
        ++gPortAreaGridStats[0];
        port_area_grid_log();
        pGridFound = PortAreaGridFind(this, rVec, &gridUsed);
        if (gridUsed && gridMode == 1) {
            return pGridFound;
        }
        if (!gridUsed) {
            ++gPortAreaGridStats[5];
        }
    }
    PORT_LOGIC_COUNT(PORT_LC_AREA_TESTS, static_cast< unsigned >(mArray.size()));  // (an upper bound, as above)
#endif
    AreaObj* const* Begin = mArray.begin();
    AreaObj* const* End = mArray.end();
    AreaObj* const* p = std::rfind_if(
        End - 1, Begin - 1,
        std::binder2nd< std::const_mem_fun1_t< bool, AreaObj, const TVec3f& >, const TVec3f& >(std::mem_func(&AreaObj::isInVolume), rVec));

    p = (p == Begin - 1 ? End : p);

#ifdef TARGET_PC
    AreaObj* const pWalkFound = p != mArray.end() ? *p : nullptr;
    if (gridUsed && pWalkFound != pGridFound) {  // (GXV_AREA_GRID=2)
        if (gPortAreaGridStats[8] < 16) {
            port_log_summary("area-grid: MISMATCH in %s (%d areas) at (%g %g %g): grid %p walk %p", mName, static_cast< int >(mArray.size()),
                             rVec.x, rVec.y, rVec.z, static_cast< void* >(pGridFound), static_cast< void* >(pWalkFound));
        }
        ++gPortAreaGridStats[8];
    }
#endif
    if (p != mArray.end()) {
        return *p;
    }

    return nullptr;
}
