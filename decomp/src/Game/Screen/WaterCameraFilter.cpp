#include "Game/Screen/WaterCameraFilter.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Map/WaterAreaHolder.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Screen/MissLayout.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/MtxUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include <JSystem/JGeometry/TMatrix.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <JSystem/JUtility/JUTVideo.hpp>
#include <revolution/gx/GXBump.h>
#include <revolution/gx/GXEnum.h>
#include <revolution/gx/GXRegs.h>
#include <revolution/gx/GXStruct.h>
#include <revolution/gx/GXTransform.h>
#ifdef TARGET_PC
#include <stdlib.h>
#endif

namespace NrvWaterCameraFilter {
    NEW_NERVE(WaterCameraFilterNrvAir, WaterCameraFilter, Air);
    NEW_NERVE(WaterCameraFilterNrvAirToWater, WaterCameraFilter, AirToWater);
    NEW_NERVE(WaterCameraFilterNrvWater, WaterCameraFilter, Water);
    NEW_NERVE(WaterCameraFilterNrvWaterToAir, WaterCameraFilter, WaterToAir);
};  // namespace NrvWaterCameraFilter

namespace {
    s32 getFrameBufferWidth() {
        return MR::getFrameBufferWidth();
    }

    s32 getScreenHeight() {
        return MR::getScreenHeight();
    }
};  // namespace

static GXColor sDefaultColor;

WaterCameraFilter::WaterCameraFilter()
    : LiveActor("\x90\x85\x92\x86\x83\x4a\x83\x81\x83\x89\x83\x74\x83\x42\x83\x8b\x83\x5e\x81\x5b"), mWaterAlpha(), mWavePhase(), mWaterColor(::sDefaultColor), mScreenTex(), mFilterTex() {
}

void WaterCameraFilter::init(const JMapInfoIter& rIter) {
    MR::connectToScene(this, MR::MovementType_MapObj, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_WaterCameraFilter);
    MR::invalidateClipping(this);
    initNerve(GET_NERVE(WaterCameraFilter, WaterCameraFilterNrvAir));
    initScreenTex();
    JUTTexture* pFilterTex = new JUTTexture(MR::loadTexFromArc("WaterCameraFilter.arc", "WaterCameraFilter.bti"), 0);
    mFilterTex = pFilterTex;
    makeActorAppeared();
}

namespace {
    s32 getScreenTexHeight() {
        return MR::getScreenHeight();
    }
}  // namespace

#ifdef TARGET_PC
// dive27 (GXV_WATER_HALFCAP, default 0; PROPOSED 1 in Performance only): the underwater filter's screen capture at half size (480x272 from the 960x544 picture, box-filtered by the copy
// unit). What the filter draws is the capture warped by a 5 % indirect offset, tinted and blended: a soft low-frequency picture, read back bilinear at full size. Decided once when the
// stage's filter is made (the texture is made then): no switch-over in play, so nothing to fade; a stage loaded after a mode change picks the new value.
extern "C" int port_water_halfcap_mode(void) __attribute__((weak));
static bool WaterHalfCap() {
    if (port_water_halfcap_mode) {
        return port_water_halfcap_mode() != 0;
    }
    const char* e = getenv("GXV_WATER_HALFCAP");
    return e != nullptr && atoi(e) != 0;
}
#endif

void WaterCameraFilter::initScreenTex() {
    MR::CurrentHeapRestorer restorer(MR::getSceneHeapGDDR3());
#ifdef TARGET_PC
    if (WaterHalfCap()) {
        mHalfCap = true;
        mScreenTex = new JUTTexture(::getFrameBufferWidth() / 2, ::getScreenHeight() / 2, GX_TF_RGB565);
        return;
    }
#endif
    mScreenTex = new JUTTexture(::getFrameBufferWidth(), ::getScreenHeight(), GX_TF_RGB565);
}

void WaterCameraFilter::exeAirToWater() {
    if (!MR::isCameraInWater()) {
        setNerve(GET_NERVE(WaterCameraFilter, WaterCameraFilterNrvWaterToAir));
    } else {
        mWaterAlpha += 20.0f;
        if (MR::isDemoActive() || mWaterAlpha >= 255.0f) {
            mWaterAlpha = 255.0f;
            setNerve(GET_NERVE(WaterCameraFilter, WaterCameraFilterNrvWater));
        }
    }
}

void WaterCameraFilter::exeWaterToAir() {
    if (MR::isCameraInWater()) {
        setNerve(GET_NERVE(WaterCameraFilter, WaterCameraFilterNrvAirToWater));
    } else {
        mWaterAlpha -= 20.0f;
        if (MR::isDemoActive() || mWaterAlpha <= 0.0f) {
            mWaterAlpha = 0.0f;
            setNerve(GET_NERVE(WaterCameraFilter, WaterCameraFilterNrvAir));
        }
    }
}

static GXColor sWaterColor = {0x32, 0x32, 0, 0};

void WaterCameraFilter::control() {
    if (MR::isCameraInWater()) {
        mWavePhase += 0.5f;

        if (mWavePhase > 360.0f) {
            mWavePhase -= 360.0f;
        }

        f32 depth = MR::clamp(WaterAreaFunction::getCameraWaterDepth(), 0.0f, 3000.0f);
        Color8 color(MR::lerp(::sDefaultColor, ::sWaterColor, depth / 3000.0f));
        mWaterColor.mGXColor = color.mGXColor;
    }
}

void WaterCameraFilter::draw() const {
    if (isNerve(GET_NERVE(WaterCameraFilter, WaterCameraFilterNrvAir))) {
        return;
    }
#ifdef TARGET_PC
    // dive27 (exact): the effect alpha is 0 on the first frame of AirToWater (the nerve changes in the update before its first step adds 20). The blend is SRCALPHA / INVSRCALPHA
    // with alpha-compare "greater than 0" on that constant alpha, so a draw at alpha 0 writes no colour, no alpha and no depth (z write is off): the capture and the quad were wasted.
    // Nothing the draw would have loaded (projection, view) is read by a later draw type: each executor sets its own state.
    if (static_cast< u8 >(mWaterAlpha) == 0) {
        return;
    }
#endif
    drawBody();
}

#ifdef TARGET_PC
// dive27 (GXV_REHEARSE, decomp Game/System/PortRehearse.cpp): the whole draw, at a made-up alpha, inside the renderer's warm-up scope behind the closed wipe: the capture (the copy target
// of this texture's size and format is made then), the material (program, the filter texture's upload) and the quad. Everything it touches is restored by the driver (GXData) and the
// alpha here; the scope records no draw.
void WaterCameraFilter::rehearse() {
    const f32 alpha = mWaterAlpha;
    mWaterAlpha = 128.0f;
    drawBody();
    mWaterAlpha = alpha;
}
#endif

void WaterCameraFilter::drawBody() const {
    GXRenderModeObj* pRenderMode = JUTVideo::getManager()->getRenderMode();
    GXSetCopyFilter(GX_FALSE, pRenderMode->sample_pattern, GX_FALSE, pRenderMode->vfilter);
#ifdef TARGET_PC
    mScreenTex->capture(0, 0, GX_TF_RGB565, mHalfCap, 0);  // (GXV_WATER_HALFCAP: the source is twice the texture, box-filtered)
#else
    mScreenTex->capture(0, 0, GX_TF_RGB565, false, 0);
#endif
    GXSetCopyFilter(GX_FALSE, pRenderMode->sample_pattern, GX_TRUE, pRenderMode->vfilter);
    loadMaterial();
    TVec2f pos;
    pos.x = 0.0f;
    pos.y = 0.0f;
    TVec3f* pPlayerCenter = MR::getPlayerCenterPos();
    MR::calcScreenPosition(&pos, *pPlayerCenter);
    pos.x /= MR::getScreenWidth();
    pos.y /= MR::getScreenHeight();

    f32 halfSize = 0.5f;
    f32 centerU = 1.0f - pos.x;
    f32 centerV = 1.0f - pos.y;

    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
    {
        GXPosition2u16(0, 0);
        GXTexCoord2f32(centerU - halfSize, centerV - halfSize);
        GXTexCoord2f32(0.0f, 0.0f);

        GXPosition2u16(MR::getFrameBufferWidth(), 0);
        GXTexCoord2f32(centerU + halfSize, centerV - halfSize);
        GXTexCoord2f32(1.0f, 0.0f);

        GXPosition2u16(0, MR::getScreenHeight());
        GXTexCoord2f32(centerU - halfSize, centerV + halfSize);
        GXTexCoord2f32(0.0f, 1.0f);

        s32 height = MR::getScreenHeight();
        GXPosition2u16(MR::getFrameBufferWidth(), height);
        GXTexCoord2f32(centerU + halfSize, centerV + halfSize);
        GXTexCoord2f32(1.0f, 1.0f);
    }

    GXEnd();

    MR::loadProjectionMtx();
    MR::loadViewMtx();
}

void WaterCameraFilter::loadMaterial() const {
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_U16, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX1, GX_DIRECT);
    Mtx44 projection;
    s32 height = MR::getScreenHeight();
    C_MTXOrtho(projection, 0.0f, height, 0.0f, MR::getFrameBufferWidth(), -10000.0f, 10000.0f);
    GXSetProjection(projection, GX_ORTHOGRAPHIC);
    TMtx34f modelView;
    modelView.identity();
    GXLoadPosMtxImm(modelView, 0);
    GXSetCurrentMtx(0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(2);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_TEX0, 0x1E, 0, 0x7D);
    GXSetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX3x4, GX_TG_TEX1, 0x3C, 0, 0x7D);
    TVec2f screenPos;
    screenPos.x = 0.0f;
    screenPos.y = 0.0f;
    MR::calcScreenPosition(&screenPos, *MR::getPlayerCenterPos());
    screenPos.x /= MR::getScreenWidth();
    screenPos.y /= MR::getScreenHeight();
    TVec3f screenPosNorm;
    MR::calcNormalizedScreenPosition(&screenPosNorm, *MR::getPlayerCenterPos());
    TMtx34f textureMtx;
    textureMtx.identity();
    TPos3f translation;
    translation.identity();
    translation.setTrans(TVec3f(-0.5f, -0.5f, 0.0f));
    TPos3f rotation;
    rotation.identity();
    MR::makeMtxRotate(rotation, 0.0f, 0.0f, mWavePhase);
    MR::multMtx(textureMtx, translation, rotation);
    translation.setTrans(TVec3f(0.5f, 0.5f, 0.0f));
    MR::multMtx(textureMtx, textureMtx, translation);
    GXLoadTexMtxImm(textureMtx, 0x1E, GX_MTX3x4);
    mFilterTex->load(GX_TEXMAP0);
    mScreenTex->load(GX_TEXMAP1);
    GXSetNumIndStages(1);
    GXSetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP0);
    GXSetTevIndWarp(GX_TEVSTAGE0, GX_INDTEXSTAGE0, 1, 0, GX_ITM_0);
    f32 ind[2][3];
    ind[0][0] = 0.05f;
    ind[0][1] = 0.0f;
    ind[0][2] = 0.0f;
    ind[1][0] = 0.0f;
    ind[1][1] = 0.05f;
    ind[1][2] = 0.0f;
    GXSetIndTexMtx(GX_ITM_0, ind, 0);
    GXSetTevColor(GX_TEVREG0, Color8(mWaterColor.r, mWaterColor.g, mWaterColor.b, mWaterAlpha));
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, 1, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_A0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 1, GX_TEVPREV);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
    GXSetAlphaCompare(GX_GREATER, 0, GX_AOP_OR, GX_GREATER, 0);
    GXSetZMode(0, GX_ALWAYS, 0);
    GXSetZCompLoc(0);
    GXSetCullMode(GX_CULL_NONE);
    GXSetClipMode(GX_CLIP_ENABLE);
}

void WaterCameraFilter::exeWater() {
    if (!MR::isCameraInWater()) {
        setNerve(GET_NERVE(WaterCameraFilter, WaterCameraFilterNrvWaterToAir));
    }
}

void WaterCameraFilter::exeAir() {
    if (MR::isCameraInWater()) {
        setNerve(GET_NERVE(WaterCameraFilter, WaterCameraFilterNrvAirToWater));
    }
}
