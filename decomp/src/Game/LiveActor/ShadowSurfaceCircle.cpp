#include "Game/LiveActor/ShadowSurfaceCircle.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/LiveActor/ShadowController.hpp"
#include "Game/Util/DirectDraw.hpp"
#include <JSystem/JGeometry/TVec.hpp>

ShadowSurfaceCircle::~ShadowSurfaceCircle() {
}

ShadowSurfaceCircle::ShadowSurfaceCircle() : ShadowSurfaceDrawer("\x89\x65\x95\x60\x89\xe6[\x90\x85\x96\xca\x89\x7e]"), mRadius(100.0f) {
}

void ShadowSurfaceCircle::setRadius(f32 radius) {
    mRadius = radius;
}

void ShadowSurfaceCircle::draw() const {
    ShadowController* controller = getController();
    if (!controller->isProjected() || !controller->isDraw()) {
        return;
    }

    f32 radius = mRadius;
    if (controller->isFollowHostScale()) {
        radius *= controller->getHost()->mScale.x;
    }

    TVec3f pos, normal;
    controller->getProjectionPos(&pos);
    controller->getProjectionNormal(&normal);
    TDDraw::resetViewMtx();
    TDDraw::drawFillCircle(pos + normal * 1.0f, -normal, radius, 128, 20);
}
