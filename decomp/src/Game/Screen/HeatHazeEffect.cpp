#include "Game/Screen/HeatHazeEffect.hpp"
#include "Game/AreaObj/HazeCube.hpp"
#include "Game/Util/AreaObjUtil.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"

HeatHazeEffect::HeatHazeEffect(const char* pName) : LiveActor(pName), _8C(1500.0f) {
}

void HeatHazeEffect::appear() {
    LiveActor::appear();
    MR::startBtk(this, "ShimmerBoard");
}

void HeatHazeEffect::control() {
    const TPos3f& rCameraMtx = MR::getCameraInvViewMtx();
    TVec3f direction;
    rCameraMtx.getZDir(direction);
    direction.negate();
    direction.mult(_8C);
    rCameraMtx.getTrans(mPosition);
    mPosition.add(direction);
    rCameraMtx.getEuler(mRotation);
    mRotation.mult(180.0f / PI);
    mScale.setAll< f32 >(_8C / 1000.0f);
}

HeatHazeDirector::HeatHazeDirector(const char* pName) : NameObj(pName) {
}

void HeatHazeEffect::init(const JMapInfoIter& rIter) {
    initModelManagerWithAnm("ShimmerBoard", nullptr, false);
    MR::connectToSceneIndirectMapObj(this);
    MR::invalidateClipping(this);
    makeActorDead();
}

void HeatHazeDirector::init(const JMapInfoIter& rIter) {
    mEffect = new HeatHazeEffect("\x97\x7a\x89\x8a");
    mEffect->initWithoutIter();

    MR::connectToSceneMapObjMovement(this);
}

#ifdef TARGET_PC
// boss23 (decomp/src/Game/Scene/GameScene.cpp): PETARI_BOSS_HAZE, the heat haze left out while a boss fight is on (1 / perf) or
// everywhere (2), a MINOR CHANGE (no shimmer); also counted for the "boss:" log line. 0 (default): as the game.
extern "C" int port_boss_haze_off(int hazeHere);
#endif

void HeatHazeDirector::movement() {
    HazeCube* pHazeCube = static_cast< HazeCube* >(MR::getAreaObj("HazeCube", *MR::getPlayerPos()));
#ifdef TARGET_PC
    if (port_boss_haze_off(pHazeCube != nullptr ? 1 : 0)) {
        pHazeCube = nullptr;
    }
#endif

    if (pHazeCube != nullptr) {
        if (MR::isDead(mEffect)) {
            mEffect->_8C = pHazeCube->mDistance;
            mEffect->appear();
        }
    } else if (!MR::isDead(mEffect)) {
        mEffect->kill();
    }
}
