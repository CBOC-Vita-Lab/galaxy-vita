#include "Game/MapObj/FallOutFieldDraw.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Map/StageSwitch.hpp"
#include "Game/MapObj/ClipAreaHolder.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Screen/ImageEffectLocalUtil.hpp"
#include "Game/Screen/ScreenAlphaCapture.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/Color.hpp"
#include "Game/Util/JMapUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#if defined(TARGET_PC)
#include <cstdlib>
#endif

void FallOutFieldDraw_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
    (void)0.5f;
    (void)0.25f;
}

namespace {
#if defined(TARGET_PC)
    long EnvInt(const char* name, long fallback) {
        const char* value = std::getenv(name);
        return (value && *value) ? std::strtol(value, nullptr, 0) : fallback;
    }
#endif

    inline void drawFillScreen(GXColor color) {
        GXSetTevColor(GX_TEVREG0, color);
        ImageEffectLocalUtil::sendTextureVertex(1, 0);
    }
}  // namespace

namespace {
    static GXTevOp sAlphaTevOperater = GX_TEV_COMP_A8_GT;
    static GXBlendMode sFillBlendMode = GX_BM_BLEND;
    static GXBlendFactor sFillSrcFactor = GX_BL_SRCALPHA;
    static GXBlendFactor sFillDstFactor = GX_BL_INVSRCALPHA;
    static GXLogicOp sFillLogicOp = GX_LO_NOOP;
    // static const s32 sBlurCount = _;
    // static const f32 sBlurRadius = _;
    // static const f32 sBlurIntensity = _;
    static Color8 sFillColor = Color8(0, 0, 0, 255);
    static Color8 sEdgeColor = Color8(0, 46, 200, 128);
};  // namespace

FallOutFieldDraw::FallOutFieldDraw(const char* pName) : NameObj(pName), _C() {
    MR::createClipAreaHolder();
    MR::connectToScene(this, MR::MovementType_None, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_FallOutFieldDraw);
    MR::createScreenAlphaSceneObj(2, 0.5f);
    MR::createScreenAlphaSceneObj(3, 0.25f);
    MR::createScreenAlphaSceneObj(4, 0.25f);
}

void FallOutFieldDraw::setUpFillScreen() const {
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevKColor(GX_KCOLOR0, Color8(0, 0, 0, 1));
    GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);

    GXTevAlphaArg alphaA = _C ? GX_CA_TEXA : GX_CA_KONST;
    GXTevAlphaArg alphaB = _C ? GX_CA_KONST : GX_CA_TEXA;

    GXSetTevAlphaIn(GX_TEVSTAGE0, alphaA, alphaB, GX_CA_A0, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, ::sAlphaTevOperater, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    GXSetZCompLoc(GX_FALSE);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetZScaleOffset(0.0f, 1.0f);
    GXSetBlendMode(::sFillBlendMode, ::sFillSrcFactor, ::sFillDstFactor, ::sFillLogicOp);
}

void FallOutFieldDraw::setUpEdgeAndClearAlpha() const {
    GXSetTevColor(GX_TEVREG1, ::sEdgeColor);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_C1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_A1, GX_CA_TEXA, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetDstAlpha(GX_TRUE, 0);
}

void FallOutFieldDraw::init(const JMapInfoIter& rIter) {
    if (!MR::isValidInfo(rIter)) {
        return;
    }

    bool arg0;
    MR::getJMapInfoArg0WithInit(rIter, &arg0);
    _C = arg0;

    if (MR::isExistStageSwitchAppear(rIter)) {
        MR::listenNameObjStageSwitchOnOffAppear(this, MR::createStageSwitchCtrl(this, rIter), MR::Functor(this, &FallOutFieldDraw::activate),
                                                MR::Functor(this, &FallOutFieldDraw::deactivate));
        MR::deactivateClipArea();
    } else {
        MR::activateClipArea();
    }
}

void FallOutFieldDraw::activate() {
    MR::activateClipArea();
}

void FallOutFieldDraw::deactivate() {
    MR::deactivateClipArea();
}

void FallOutFieldDraw::draw() const {
    if (!MR::isActiveClipArea()) {
        return;
    }

#if defined(TARGET_PC)
    // PETARI_CLIPCHAIN_ONEPASS / PETARI_CLIPCHAIN_SKIPEMPTY (clipblur25, ImageEffectLocalUtil.cpp): 0 / 0 (the default) is the code below as it was.
    const int onePass = ImageEffectLocalUtil::portClipOnePassMode();
    const bool emptyMask = ImageEffectLocalUtil::portClipSkipEmpty(_C != 0);
    ImageEffectLocalUtil::portClipNote(onePass != 0, emptyMask);
    ImageEffectLocalUtil::portClipSetDry(emptyMask);  // (no volume drew and the mask is the "inside" one: every GX state call is made, no capture, no vertex)
#endif
    ImageEffectLocalUtil::setupDrawTexture();

    JUTTexture* pScreenAlphaTexture2 = MR::getScreenAlphaTexture(2);
    JUTTexture* pScreenAlphaTexture3 = MR::getScreenAlphaTexture(3);
    JUTTexture* pScreenAlphaTexture4 = MR::getScreenAlphaTexture(4);

#if defined(TARGET_PC)
    // The original argument order divides by zero in capture(). Keep the
    // correction opt-in until the affected room has been verified on hardware.
    static const bool clipCaptureFix = EnvInt("GXV_CLIP_CAPTURE_FIX", 0) == 1;
    ImageEffectLocalUtil::capture(pScreenAlphaTexture2, clipCaptureFix ? 1 : 0, clipCaptureFix ? 0 : 1, GX_CTF_A8, true, 0);
#else
    ImageEffectLocalUtil::capture(pScreenAlphaTexture2, 0, 1, GX_CTF_A8, true, 0);
#endif

    GXSetTevKColor(GX_KCOLOR0, Color8(0, 0, 0, 1));
    GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
    GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0_A);
    GXSetTevColor(GX_TEVREG0, Color8(255, 255, 255, 255));

    GXTevColorArg colorA = _C ? GX_CC_TEXA : GX_CC_KONST;
    GXTevColorArg colorB = _C ? GX_CC_KONST : GX_CC_TEXA;

    GXSetTevColorIn(GX_TEVSTAGE0, colorA, colorB, GX_CC_ZERO, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_COMP_RGB8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

    GXTevAlphaArg alphaA = _C ? GX_CA_TEXA : GX_CA_KONST;
    GXTevAlphaArg alphaB = _C ? GX_CA_KONST : GX_CA_TEXA;

    GXSetTevAlphaIn(GX_TEVSTAGE0, alphaA, alphaB, GX_CA_A0, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_COMP_A8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetColorUpdate(GX_FALSE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetDstAlpha(GX_FALSE, 0);
    GXSetZScaleOffset(0.0f, 0.0f);

#if defined(TARGET_PC)
    // (PETARI_CLIPCHAIN_HALFRATE: the soft edge's chain left out on the second of two pictures while the camera is still; the edge below is drawn from
    // texture 4 as the previous picture made it. The chain writes only the EFB's alpha in its tiles (colour update is off), and the edge draw clears it.)
    const bool reuseChain = onePass != 1 && ImageEffectLocalUtil::portClipChainReuse(this);
    if (onePass == 2 && !reuseChain) {
        // (PETARI_CLIPCHAIN_ONEPASS=2: the tile the blur drew into gets the thresholded taps of the mask in one draw, no blend; tex3 and the quarter draw are gone)
        ImageEffectLocalUtil::portClipTapsSetup(pScreenAlphaTexture2, _C != 0, 0);
        GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
        ImageEffectLocalUtil::sendTextureVertex(4, 4);
        ImageEffectLocalUtil::portClipTapsEnd();
        ImageEffectLocalUtil::capture(pScreenAlphaTexture4, 4, 4, GX_CTF_A8, false, 0);
    }
    if (onePass == 0 && !reuseChain) {
#endif
    ImageEffectLocalUtil::drawTexture(pScreenAlphaTexture2, 2, 0, 255, ImageEffectLocalUtil::TexDrawType_0);
    ImageEffectLocalUtil::capture(pScreenAlphaTexture3, 2, 0, GX_CTF_A8, true, 0);

    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_TEXA, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_TEXA, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

    ImageEffectLocalUtil::blurTexture(pScreenAlphaTexture3, 4, 4, 5, 0.004f, 1.0f);
    ImageEffectLocalUtil::capture(pScreenAlphaTexture4, 4, 4, GX_CTF_A8, false, 0);
#if defined(TARGET_PC)
    }
#endif
#if defined(TARGET_PC)
    if (onePass == 1) {
        // (PETARI_CLIPCHAIN_ONEPASS=1: the edge draw reads the mask itself at K offsets; stage K multiplies by the edge colour's alpha as setUpEdgeAndClearAlpha does)
        const int taps = ImageEffectLocalUtil::portClipTapsSetup(pScreenAlphaTexture2, _C != 0, 1);
        const GXTevStageID last = static_cast< GXTevStageID >(GX_TEVSTAGE0 + taps);
        GXSetTevColor(GX_TEVREG1, ::sEdgeColor);
        GXSetTevOrder(last, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
        GXSetTevColorIn(last, GX_CC_C1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
        GXSetTevColorOp(last, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevAlphaIn(last, GX_CA_ZERO, GX_CA_A1, GX_CA_APREV, GX_CA_ZERO);
        GXSetTevAlphaOp(last, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetDstAlpha(GX_TRUE, 0);
        GXSetChanMatColor(GX_COLOR0A0, Color8(255, 255, 255, 255));
        GXSetChanAmbColor(GX_COLOR0A0, Color8(0, 0, 0, 255));
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        ImageEffectLocalUtil::sendTextureVertex(1, 0);
        ImageEffectLocalUtil::portClipTapsEnd();
    } else {
#endif
    setUpEdgeAndClearAlpha();
    ImageEffectLocalUtil::drawTexture(pScreenAlphaTexture4, 1, 0, 255, ImageEffectLocalUtil::TexDrawType_2);
#if defined(TARGET_PC)
    }
#endif
    MR::loadScreenAlphaTexture(2, GX_TEXMAP0);
    setUpFillScreen();

    ::drawFillScreen(::sFillColor);

    MR::loadViewMtx();
    MR::loadProjectionMtx();

    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetDstAlpha(GX_TRUE, 0);
    GXSetZScaleOffset(1.0f, 0.0f);
#if defined(TARGET_PC)
    ImageEffectLocalUtil::portClipSetDry(false);
#endif
}

namespace MR {
    NameObj* createFallOutFieldDraw(const char*) {
        return MR::createSceneObj(SceneObj_FallOutFieldDraw);
    }
};  // namespace MR
