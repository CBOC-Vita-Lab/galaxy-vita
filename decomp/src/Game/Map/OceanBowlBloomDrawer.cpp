#include "Game/Map/OceanBowlBloomDrawer.hpp"
#include "Game/Map/OceanBowl.hpp"
#include "Game/Map/OceanBowlPoint.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include <revolution/gx/GXVert.h>

OceanBowlBloomDrawer::OceanBowlBloomDrawer(OceanBowl* pHost) : NameObj("\x83\x49\x81\x5b\x83\x56\x83\x83\x83\x93\x83\x7b\x83\x45\x83\x8b[\x83\x75\x83\x8b\x81\x5b\x83\x80\x95\x60\x89\xe6]"), mHost(pHost) {
}

void OceanBowlBloomDrawer::init(const JMapInfoIter&) {
    MR::connectToScene(this, MR::MovementType_None, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_OceanBowlBloomDrawer);
}

void OceanBowlBloomDrawer::draw() const {
    if (mHost->mIsClipped || !MR::isValidDraw(mHost) || !MR::isCameraInWater()) {
        return;
    }

    mHost->loadMaterialBloom();

    OceanBowlPoint* pPoint2;
    u16 zero = 0;
    u16 one = 1;
    for (s32 x = 0; x < 24; x++) {
        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 50);
        for (s32 y = 0; y < 25; y++) {
            pPoint2 = mHost->getPoint(x, y);
            OceanBowlPoint* pPoint = mHost->getPoint(x + 1, y);
            GXPosition3f32(pPoint->mVertexPosition.x, pPoint->mVertexPosition.y, pPoint->mVertexPosition.z);
            GXColor4u8(0xFF, 0xFF, 0xFF, mHost->getPoint(x + 1, y)->mAlpha);
            GXTexCoord2s16(zero, zero);

            GXPosition3f32(pPoint2->mVertexPosition.x, pPoint2->mVertexPosition.y, pPoint2->mVertexPosition.z);
            GXColor4u8(0xFF, 0xFF, 0xFF, mHost->getPoint(x, y)->mAlpha);
            GXTexCoord2s16(one, one);

            zero += 2;
            one += 2;
        }
        GXEnd();
    }
}
