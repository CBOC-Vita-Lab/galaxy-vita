#include "Game/LiveActor/ShadowVolumeCylinder.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/MtxUtil.hpp"
#include <revolution/gx.h>

namespace {
    const f32 sModelScale = 100.0f;
}

ShadowVolumeCylinder::~ShadowVolumeCylinder() {
}

ShadowVolumeCylinder::ShadowVolumeCylinder() : ShadowVolumeModel("\x89\x65\x95\x60\x89\xe6[\x83\x7b\x83\x8a\x83\x85\x81\x5b\x83\x80\x89\x7e\x92\x8c]") {
    mRadius = 100.0f;
    initVolumeModel("ShadowVolumeCylinder");
}

void ShadowVolumeCylinder::setRadius(f32 radius) {
    mRadius = radius;
}

void ShadowVolumeCylinder::loadModelDrawMtx() const {
    ShadowController* controller = getController();
    TVec3f position;
    calcBaseDropPosition(&position);
    TVec3f direction;
    controller->getDropDir(&direction);
    TVec3f up(-direction);
    TPos3f mtx;
    MR::makeMtxUpNoSupportPos(&mtx, up, position);

    f32 radius = mRadius / ::sModelScale;
    if (controller->isFollowHostScale()) {
        radius *= controller->getHost()->mScale.x;
    }

    TVec3f scale(radius, calcBaseDropLength() / ::sModelScale, radius);
    MR::preScaleMtx(mtx, scale);
    PSMTXConcat(MR::getCameraViewMtx(), mtx, mtx);
    GXLoadPosMtxImm(mtx, 0);
}
