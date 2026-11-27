#include "Game/Map/WaterAreaHolder.hpp"
#include "Game/Map/OceanBowl.hpp"
#include "Game/Map/OceanRing.hpp"
#include "Game/Map/OceanSphere.hpp"
#include "Game/Map/WaterInfo.hpp"
#include "Game/Map/WhirlPoolAccelerator.hpp"
#include "Game/MapObj/WhirlPool.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Screen/WaterCameraFilter.hpp"
#include "Game/Util/AreaObjUtil.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#ifdef TARGET_PC
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioHolder.hpp"
#include "Game/Util/PlayerUtil.hpp"
// dive27: the edges of "wet" for the console's first-use ledger (platform/vgx/src/vgx_backend_gl.cpp, "dive27:" lines): 1 camera wet, 2 camera dry, 3 Mario swimming, 4 Mario out of the
// water. The front end opens a 3 s window of counters (programs linked, targets made, textures uploaded, longest picture gap) at each. No effect on the game.
extern "C" void port_dive27_edge(unsigned kind) __attribute__((weak));
#endif

namespace {
    static const s32 sAreaNumMax = 8;

    WaterAreaHolder* getWaterAreaHolder() {
        return MR::getSceneObj< WaterAreaHolder >(SceneObj_WaterAreaHolder);
    }
};  // namespace

WaterAreaHolder::WaterAreaHolder()
    : NameObj("\x90\x85\x83\x47\x83\x8a\x83\x41\x95\xdb\x8e\x9d"), mNumOceanBowls(0), mOceanBowls(nullptr), mNumOceanRings(0), mOceanRings(nullptr), mNumOceanSpheres(0),
      mOceanSpheres(nullptr), mNumWhirlPools(0), mWhirlPools(nullptr), mNumWhirlPoolAccelerators(0), mWhirlPoolAccelerators(nullptr),
      mCamInWater(false), mCameraFilter(nullptr), mUseBloom(false) {
    mWaterInfo.clear();

    if (MR::isEqualStageName("HeavenlyBeachGalaxy") || MR::isEqualStageName("OceanRingGalaxy")) {
        MR::createNormalBloom();
        mUseBloom = true;
    }

    mOceanBowls = new OceanBowl*[::sAreaNumMax];
    mOceanRings = new OceanRing*[::sAreaNumMax];
    mOceanSpheres = new OceanSphere*[::sAreaNumMax];
    mWhirlPools = new WhirlPool*[::sAreaNumMax];
    mWhirlPoolAccelerators = new WhirlPoolAccelerator*[::sAreaNumMax];

    for (s32 idx = 0; idx < ::sAreaNumMax; idx++) {
        mOceanBowls[idx] = nullptr;
        mOceanRings[idx] = nullptr;
        mOceanSpheres[idx] = nullptr;
        mWhirlPools[idx] = nullptr;
        mWhirlPoolAccelerators[idx] = nullptr;
    }

    MR::connectToSceneScreenEffectMovement(this);
    mCameraFilter = new WaterCameraFilter();
    mCameraFilter->initWithoutIter();
}

void WaterAreaHolder::entryOceanBowl(OceanBowl* pOceanBowl) {
    mOceanBowls[mNumOceanBowls] = pOceanBowl;
    mNumOceanBowls++;
}

void WaterAreaHolder::entryOceanRing(OceanRing* pOceanRing) {
    mOceanRings[mNumOceanRings] = pOceanRing;
    mNumOceanRings++;
}

void WaterAreaHolder::entryOceanSphere(OceanSphere* pOceanSphere) {
    mOceanSpheres[mNumOceanSpheres] = pOceanSphere;
    mNumOceanSpheres++;
}

void WaterAreaHolder::entryWhirlPool(WhirlPool* pWhirlPool) {
    mWhirlPools[mNumWhirlPools] = pWhirlPool;
    mNumWhirlPools++;
}

void WaterAreaHolder::entryWhirlPoolAccelerator(WhirlPoolAccelerator* pWhirlPoolAccelerator) {
    mWhirlPoolAccelerators[mNumWhirlPoolAccelerators] = pWhirlPoolAccelerator;
    mNumWhirlPoolAccelerators++;
}

void WaterAreaHolder::movement() {
#ifdef TARGET_PC
    if (port_dive27_edge) {
        static bool sMarioWet = false;
        MarioHolder* pHolder = MR::isExistSceneObj(SceneObj_MarioHolder) ? MR::getMarioHolder() : nullptr;
        if (pHolder != nullptr && pHolder->getMarioActor() != nullptr && pHolder->getMarioActor()->getMario() != nullptr) {
            const bool wet = MR::isPlayerInWaterMode();
            if (wet != sMarioWet) {
                sMarioWet = wet;
                port_dive27_edge(wet ? 3u : 4u);
            }
        }
    }
#endif
    if (MR::getWaterAreaObj(&mWaterInfo, MR::getCamPos())) {
        if (!mCamInWater) {
#ifdef TARGET_PC
            if (port_dive27_edge) {
                port_dive27_edge(1u);
            }
#endif
            if (mUseBloom) {
                MR::turnOnNormalBloom();
                MR::setNormalBloomIntensity(255);
                MR::setNormalBloomThreshold(128);
                MR::setNormalBloomBlurIntensity1(40);
                MR::setNormalBloomBlurIntensity2(20);
            }
            mCamInWater = true;
        }
        MR::getWaterAreaInfo(&mWaterInfo, MR::getCamPos(), TVec3f(0.0f, -1.0f, 0.0f), false);
    } else {
        if (mCamInWater) {
            if (mUseBloom) {
                MR::setImageEffectControlAuto();
            }
            mCamInWater = false;
#ifdef TARGET_PC
            if (port_dive27_edge) {
                port_dive27_edge(2u);
            }
#endif
        }
    }
}

void WaterAreaFunction::entryOceanBowl(OceanBowl* pOceanBowl) {
    createWaterAreaHolder();
    ::getWaterAreaHolder()->entryOceanBowl(pOceanBowl);
}

void WaterAreaFunction::entryOceanRing(OceanRing* pOceanRing) {
    createWaterAreaHolder();
    ::getWaterAreaHolder()->entryOceanRing(pOceanRing);
}

void WaterAreaFunction::entryOceanSphere(OceanSphere* pOceanSphere) {
    createWaterAreaHolder();
    ::getWaterAreaHolder()->entryOceanSphere(pOceanSphere);
}

void WaterAreaFunction::entryWhirlPool(WhirlPool* pWhirlPool) {
    createWaterAreaHolder();
    ::getWaterAreaHolder()->entryWhirlPool(pWhirlPool);
}

void WaterAreaFunction::entryWhirlPoolAccelerator(WhirlPoolAccelerator* pWhirlPoolAccelerator) {
    createWaterAreaHolder();
    ::getWaterAreaHolder()->entryWhirlPoolAccelerator(pWhirlPoolAccelerator);
}

void WaterAreaFunction::createWaterAreaHolder() {
    MR::createSceneObj(SceneObj_WaterAreaHolder);
}

bool WaterAreaFunction::tryInOceanArea(const TVec3f& rPos, WaterInfo* pWaterInfo) {
    if (!MR::isExistSceneObj(SceneObj_WaterAreaHolder)) {
        return false;
    }

    OceanBowl* bowl;
    OceanRing* ring;
    OceanSphere* sphere;

    for (s32 idx = 0; idx < ::getWaterAreaHolder()->mNumOceanBowls; idx++) {
        bowl = ::getWaterAreaHolder()->mOceanBowls[idx];
        if (bowl->isInWater(rPos)) {
            pWaterInfo->mOceanBowl = bowl;
            return true;
        }
    }

    for (s32 idx = 0; idx < ::getWaterAreaHolder()->mNumOceanRings; idx++) {
        ring = ::getWaterAreaHolder()->mOceanRings[idx];
        if (ring->isInWater(rPos)) {
            pWaterInfo->mOceanRing = ring;
            return true;
        }
    }

    for (s32 idx = 0; idx < ::getWaterAreaHolder()->mNumOceanSpheres; idx++) {
        sphere = ::getWaterAreaHolder()->mOceanSpheres[idx];
        if (sphere->isInWater(rPos)) {
            pWaterInfo->mOceanSphere = sphere;
            return true;
        }
    }

    return false;
}

bool WaterAreaFunction::tryInWhirlPoolAccelerator(const TVec3f& rPos, TVec3f* pVel) {
    if (!MR::isExistSceneObj(SceneObj_WaterAreaHolder)) {
        return false;
    }

    for (s32 idx = 0; idx < ::getWaterAreaHolder()->mNumWhirlPoolAccelerators; idx++) {
        if (::getWaterAreaHolder()->mWhirlPoolAccelerators[idx]->calcInfo(rPos, pVel)) {
            return true;
        }
    }

    return false;
}

bool WaterAreaFunction::isCameraInWaterForCameraUtil() {
    if (MR::isExistSceneObj(SceneObj_WaterAreaHolder)) {
        if (::getWaterAreaHolder()->mCamInWater) {
            return true;
        }
    }

    return false;
}

#ifdef TARGET_PC
// dive27: this stage's normal bloom is the underwater one (WaterAreaHolder::mUseBloom): on only while the camera is wet.
bool WaterAreaFunction::isBloomForWater() {
    return MR::isExistSceneObj(SceneObj_WaterAreaHolder) && ::getWaterAreaHolder()->mUseBloom;
}
#endif

f32 WaterAreaFunction::getCameraWaterDepth() {
    if (MR::isExistSceneObj(SceneObj_WaterAreaHolder)) {
        return ::getWaterAreaHolder()->mWaterInfo.mCamWaterDepth;
    }

    return 0.0f;
}

WaterInfo* WaterAreaFunction::getCameraWaterInfo() {
    if (MR::isExistSceneObj(SceneObj_WaterAreaHolder)) {
        return &::getWaterAreaHolder()->mWaterInfo;
    }

    return nullptr;
}

void WaterAreaFunction::pauseOffWaterArea() {
    if (MR::isExistSceneObj(SceneObj_WaterAreaHolder)) {
        MR::requestMovementOn(::getWaterAreaHolder());
        MR::requestMovementOn(::getWaterAreaHolder()->mCameraFilter);
    }
}
