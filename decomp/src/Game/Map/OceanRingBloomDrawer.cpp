#include "Game/Map/OceanRingBloomDrawer.hpp"
#include "Game/Map/OceanRing.hpp"
#include "Game/Map/OceanRingDrawer.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ObjUtil.hpp"

OceanRingBloomDrawer::OceanRingBloomDrawer(OceanRing* pRing) : NameObj("\x83\x49\x81\x5b\x83\x56\x83\x83\x83\x93\x83\x8a\x83\x93\x83\x4f[\x83\x75\x83\x8b\x81\x5b\x83\x80\x95\x60\x89\xe6]") {
    mRing = pRing;
}

void OceanRingBloomDrawer::init(const JMapInfoIter& rIter) {
    MR::connectToScene(this, MR::MovementType_None, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_OceanBowlBloomDrawer);
}

void OceanRingBloomDrawer::draw() const {
    if (!MR::isValidDraw(mRing) || !MR::isCameraInWater()) {
        return;
    }

    mRing->mRingDrawer->drawBloom();
}

OceanRingBloomDrawer::~OceanRingBloomDrawer() {
}
