#include "Game/AreaObj/AreaObjContainer.hpp"
#include "Game/AreaObj/AreaObj.hpp"
#include "Game/AreaObj/CubeCamera.hpp"
#include "Game/AreaObj/GlaringLightArea.hpp"
#include "Game/AreaObj/ImageEffectArea.hpp"
#include "Game/AreaObj/LightAreaHolder.hpp"
#include "Game/AreaObj/WarpCube.hpp"
#include "Game/AreaObj/WaterArea.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include <algorithm>
#include <cstring>
#ifdef TARGET_PC
#include "port/diagnostics.h"
#include "port/logic_prof.h"  // (GXV_LOGIC_PROF, logic23: the manager search timed as "area-manager")
#include "port/gt71.h"
#include "port/obj_cut.h"     // (GXV_AREA_GRID's manager-name memo: port_area_geo_gen)
extern "C" int port_area_grid_mode(void);  // (AreaObj.cpp)
extern "C" int port_on_frame_thread(void);
extern "C" uint32_t gPortAreaGridStats[10];
#endif

namespace {
    struct IsManagerName : std::binary_function< AreaObjMgr*, const char*, bool > {
        bool operator()(const AreaObjMgr* pManager, const char* pName) const {
            // "pName starts with the manager's name". (strstr(pName, mName) == pName says the same, but scans the whole
            // string for every manager that does not match: this runs for every area lookup of every actor, every frame.)
            const char* pPrefix = pManager->mName;
            while (*pPrefix != '\0') {
                if (*pName++ != *pPrefix++) {
                    return false;
                }
            }
            return true;
        }
    };

    template < typename T >
    AreaObjMgr* createManager(s32 numMax, const char* pName) {
        return new T(numMax, pName);
    }

    const Name2CreateManagerFunc cCreateTable[] = {
        {
            "SwitchArea",
            0x40,
            createManager< AreaObjMgr >,
        },
        {
            "RaceJudgeCube",
            0x40,
            createManager< AreaObjMgr >,
        },
        {
            "NinForceWindCube",
            0x40,
            createManager< AreaObjMgr >,
        },
        {
            "NinAbyssCube",
            0x40,
            createManager< AreaObjMgr >,
        },
        {
            "CubeCamera",
            0xA0,
            createManager< CubeCameraMgr >,
        },
        {
            "BindEndCube",
            0x40,
            createManager< AreaObjMgr >,
        },
        {
            "EffectCylinder",
            0x40,
            createManager< AreaObjMgr >,
        },
        {
            "DeathArea",
            0x40,
            createManager< AreaObjMgr >,
        },
        {
            "WarpCube",
            0x40,
            createManager< WarpCubeMgr >,
        },
        {
            "TripodBossStepStart",
            0x40,
            createManager< AreaObjMgr >,
        },
        {
            "Water",
            0x40,
            createManager< WaterAreaMgr >,
        },
        {
            "PlaneModeCube",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "PlaneCircularModeCube",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "PipeModeCube",
            0x4,
            createManager< AreaObjMgr >,
        },
        {
            "TowerModeCylinder",
            0x4,
            createManager< AreaObjMgr >,
        },
        {
            "ShadeCube",
            0x40,
            createManager< AreaObjMgr >,
        },
        {
            "PullBackCube",
            0x40,
            createManager< AreaObjMgr >,
        },
        {
            "PullBackCylinder",
            0x40,
            createManager< AreaObjMgr >,
        },
        {
            "RestartCube",
            0x40,
            createManager< AreaObjMgr >,
        },
        {
            "ChangeBgmCube",
            0x20,
            createManager< AreaObjMgr >,
        },
        {
            "BgmProhibitArea",
            0x4,
            createManager< AreaObjMgr >,
        },
        {
            "SoundEmitterCube",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "SoundEmitterSphere",
            0x8,
            createManager< AreaObjMgr >,
        },
        {
            "PlaneCollisionCube",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "ForbidTriangleJumpCube",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "ForbidWaterSearchCube",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "QuakeEffectArea",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "HazeCube",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "AudioEffectArea",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "BigBubbleGoalArea",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "SunLightArea",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "GlaringLightArea",
            0x40,
            createManager< GlaringLightAreaMgr >,
        },
        {
            "ViewGroupCtrlCube",
            0x40,
            createManager< AreaObjMgr >,
        },
        {
            "LensFlareArea",
            0x40,
            createManager< AreaObjMgr >,
        },
        {
            "CameraRepulsiveArea",
            0x80,
            createManager< AreaObjMgr >,
        },
        {
            "LightArea",
            0x80,
            createManager< LightAreaHolder >,
        },
        {
            "FallsCube",
            0x20,
            createManager< AreaObjMgr >,
        },
        {
            "MercatorCube",
            0x1,
            createManager< AreaObjMgr >,
        },
        {
            "AstroChangeStageCube",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "ImageEffectArea",
            0x20,
            createManager< ImageEffectAreaMgr >,
        },
        {
            "BlueStarGuidanceCube",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "TicoSeedGuidanceCube",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "MessageArea",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "SmokeEffectColorArea",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "BeeWallShortDistArea",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "ExtraWallCheckArea",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "ExtraWallCheckCylinder",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "SpinGuidanceCube",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "HipDropGuidanceCube",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "TamakoroMoveGuidanceCube",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "TamakoroJumpGuidanceCube",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "BigBubbleGuidanceCube",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "HeavySteeringCube",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "NonSleepCube",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "AreaMoveSphere",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "DodoryuClosedCylinder",
            0x8,
            createManager< AreaObjMgr >,
        },
        {
            "DashChargeCylinder",
            0x8,
            createManager< AreaObjMgr >,
        },
        {
            "PlayerSeArea",
            0x8,
            createManager< AreaObjMgr >,
        },
        {
            "RasterScrollCube",
            0x8,
            createManager< AreaObjMgr >,
        },
        {
            "OnimasuCube",
            0x20,
            createManager< AreaObjMgr >,
        },
        {
            "ForbidJumpCube",
            0x8,
            createManager< AreaObjMgr >,
        },
        {
            "CollisionArea",
            0x40,
            createManager< AreaObjMgr >,
        },
        {
            "AstroOverlookArea",
            0x8,
            createManager< AreaObjMgr >,
        },
        {
            "CelestrialSphere",
            0x4,
            createManager< AreaObjMgr >,
        },
        {
            "MirrorArea",
            0x10,
            createManager< AreaObjMgr >,
        },
        {
            "DarkMatterCube",
            0x40,
            createManager< AreaObjMgr >,
        },
        {
            "DarkMatterCylinder",
            0x20,
            createManager< AreaObjMgr >,
        },
    };
};  // namespace

AreaObjContainer::AreaObjContainer(const char* pName) : NameObj(pName) {
    mNumManagers = 0;
#ifdef TARGET_PC
    PortAreaGeoBump();  // (GXV_AREA_GRID: the manager-name memo is the old container's)
#endif
}

void AreaObjContainer::init(const JMapInfoIter& rIter) {
    for (u32 i = 0; i < ARRAY_SIZE(::cCreateTable); i++) {
        const Name2CreateManagerFunc* pName2CreateManagerFunc = &::cCreateTable[i];
        AreaObjMgr* mgr = pName2CreateManagerFunc->mCreateFunc(pName2CreateManagerFunc->_4, pName2CreateManagerFunc->mName);
        mgr->init(JMapInfoIter());
        s32 num = mNumManagers;
        mNumManagers++;
        mManagerArray[num] = mgr;
    }
}

#ifdef TARGET_PC
// GXV_AREA_GRID's manager-name memo (logic23, 8 Nov; on with GXV_AREA_GRID=1 or 2; exact): every named area lookup (MR::getAreaObj,
// MR::isInAreaObj, ...) first finds its manager by name: a walk over the ~60 managers, each a heap object, until one's name is a
// prefix of the asked name. The answer depends only on the asked text and on the managers' names and order, which are set when the
// container is initialised (init appends; nothing renames or reorders them). The memo keeps, per asked pointer, the text (up to 31
// characters), the container, the manager count and port_area_geo_gen; a lookup whose pointer, text, container, count and generation
// are the same gets the same index, read live from the array. A text that did not fit, or no manager found (the original's find_if
// would read past the array: left to the original), is not kept. GXV_AREA_GRID=2: every hit also searches; a different manager is
// counted ("area-grid: ... mismatches X / Y", Y) and the searched one used.
namespace {
struct MgrMemo {
    const AreaObjContainer* owner;
    const char* key;
    uint32_t gen;
    u32 num;
    u32 index;
    char text[32];
};
MgrMemo sMgrMemo[64];
}  // namespace
#endif

AreaObjMgr* AreaObjContainer::getManager(const char* pName) const {
#ifdef TARGET_PC
    PORT_LOGIC_SYS(PORT_LS_AREA_MGR);
    int mode = port_area_grid_mode();
    if (mode == 0 && ::port::gt71::On(::port::gt71::kArea)) {
        mode = 1;  // (gt71 AREA: the name memo alone, without the grid: the memo's own checks (pointer, text, container, count, generation) make it exact)
    }
    if (mode != 0 && port_on_frame_thread()) {  // (the memo is the game thread's: a scene's areas are made on the loading thread)
        gPortAreaGridStats[6]++;
        const uint32_t h = (static_cast< uint32_t >(reinterpret_cast< uintptr_t >(pName)) * 0x9E3779B1u) >> 26;
        MgrMemo& e = sMgrMemo[h];
#ifndef PORT_LGRID_NEG
        const bool hit = e.key == pName && e.owner == this && e.gen == port_area_geo_gen && e.num == mNumManagers &&
                         std::strncmp(e.text, pName, sizeof(e.text)) == 0;
#else
        const bool hit = e.key == pName && e.owner == this && e.gen == port_area_geo_gen && e.num == mNumManagers;  // (negative control)
#endif
        if (hit) {
            gPortAreaGridStats[7]++;
            if (mode == 1) {
                return mManagerArray[e.index];
            }
        }
        AreaObjMgr* const* pFound =
            std::find_if(mManagerArray, mManagerArray + mNumManagers, std::binder2nd< IsManagerName, const char* >(IsManagerName(), pName));
        const u32 index = static_cast< u32 >(pFound - mManagerArray);
        if (hit && index != e.index && gPortAreaGridStats[9]++ < 16) {
            port_log_summary("area-grid: manager memo MISMATCH for \"%.31s\": memo %u search %u", pName, e.index, index);
        }
        if (!hit && index < mNumManagers && std::strlen(pName) < sizeof(e.text)) {
            e.owner = this;
            e.key = pName;
            e.gen = port_area_geo_gen;
            e.num = mNumManagers;
            e.index = index;
            std::strncpy(e.text, pName, sizeof(e.text));
        }
        return *pFound;
    }
#endif
    return *std::find_if(mManagerArray, mManagerArray + mNumManagers, std::binder2nd< IsManagerName, const char* >(IsManagerName(), pName));
}

AreaObj* AreaObjContainer::getAreaObj(const char* pName, const TVec3f& rVec) const {
    return getManager(pName)->find_in(rVec);
}

namespace MR {
    AreaObjContainer* getAreaObjContainer() {
        return MR::getSceneObj< AreaObjContainer >(SceneObj_AreaObjContainer);
    }
};  // namespace MR
