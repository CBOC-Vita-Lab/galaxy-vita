#include "Game/Screen/BloomEffect.hpp"
#include "Game/Screen/CopyFilterNegater.hpp"
#include "Game/Screen/ImageEffectLocalUtil.hpp"
#include "Game/Screen/ImageEffectResource.hpp"
#include "Game/Screen/ImageEffectSystemHolder.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/Color.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "JSystem/JUtility/JUTVideo.hpp"
#include "revolution/gx/GXEnum.h"
#include "revolution/gx/GXFrameBuf.h"
#include "revolution/gx/GXStruct.h"
#include "revolution/gx/GXTev.h"
#include <JSystem/JUtility/JUTTexture.hpp>
#ifdef TARGET_PC
#include <cmath>
#include "port/compat.h"
#include "revolution/gx/GXGet.h"
#include "revolution/gx/GXTransform.h"
#endif

void BloomEffect_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
    (void)-1.0f;
}

namespace {
    struct RadAndOfs {
        /* 0x00 */ f32 _0;
        /* 0x04 */ f32 _4;
    };

    static const RadAndOfs sL1RadAndOfs[] = {
        {0.01f, 0.0f},
        {0.02f, 0.52f},
    };
    static const RadAndOfs sL2RadAndOfs[] = {
        {0.04f, 0.0f},
        {0.07f, 0.0f},
        {0.09f, 0.0f},
    };
    static const GXTexMtx sTexMtxID[] = {
        GX_TEXMTX0, GX_TEXMTX1, GX_TEXMTX2, GX_TEXMTX3, GX_TEXMTX4, GX_TEXMTX5, GX_TEXMTX6, GX_TEXMTX7,
    };
    static const u32 sL1RoundPoints = 6;
    static const u32 sL2RoundPoints = 12;
    // static const f32 sBloomIntensity = _;
    // static const f32 sMaskThreshold = _;
    // static const ... sBlurType = _;
    // static const s32 sFadeCounterMax = _;
};  // namespace

#ifdef TARGET_PC
// PETARI_BLOOM_SKIP (revisit23; the rule and why it is exact: ImageEffectLocalUtil.cpp): while set, postDraw's chain is "dry": every
// GX state call is made as before, but captures take no copy and draws send no vertex (the scene's put-back draw excepted).
namespace {
    bool sPortDry = false;
    u32 sChainCaptures = 0, sChainDraws = 0;  // (GXV_BLOOM_FAST's census: what this picture's chain sent; counters only, no GX)

    // ImageEffectLocalUtil::capture, or (dry) only the copy-source and destination state JUTTexture::capture would set.
    void bloomCapture(JUTTexture* pTexture, s32 divisions, s32 tile, GXTexFmt format, bool mipmap, u8 clear) {
        if (!sPortDry) {
            ImageEffectLocalUtil::capture(pTexture, divisions, tile, format, mipmap, clear);
            sChainCaptures += pTexture->getCaptureFlag() ? 1 : 0;
            return;
        }
        if (!pTexture->getCaptureFlag()) {
            return;
        }
        const s32 x = (tile % divisions) * MR::getFrameBufferWidth() / divisions;
        const s32 y = (tile / divisions) * MR::getFrameBufferHeight() / divisions;
        const u16 w = static_cast< u16 >(pTexture->getWidth());
        const u16 h = static_cast< u16 >(pTexture->getHeight());
        GXSetTexCopySrc(x, y, mipmap ? w * 2 : w, mipmap ? h * 2 : h);
        GXSetTexCopyDst(w, h, format, mipmap);
    }

    // GXV_BLOOM_BRACKET: the scene's put-back draw, between its markers (the vgx front end tags the draw; nothing is written with the switch off).
    void bloomPutBack(const BloomEffect* b) {
        ImageEffectLocalUtil::portBloomBracketMark(PORT_BB_PUT_BEGIN);
        b->drawTexture(b->_24, 1, 0, 0xFF, BloomEffect::DrawType_0);
        ImageEffectLocalUtil::portBloomBracketMark(PORT_BB_PUT_END);
    }

    // The black fill reaches every EFB pixel the chain reads: viewport and scissor cover the whole EFB.
    bool bloomFillWhole() {
        f32 vp[6];
        GXGetViewportv(vp);
        u32 left, top, wd, ht;
        GXGetScissor(&left, &top, &wd, &ht);
        const s32 w = MR::getFrameBufferWidth();
        const s32 h = MR::getScreenHeight();
        return vp[0] <= 0.0f && vp[1] <= 0.0f && vp[0] + vp[2] >= static_cast< f32 >(w) && vp[1] + vp[3] >= static_cast< f32 >(h) && left == 0 &&
               top == 0 && wd >= static_cast< u32 >(w) && ht >= static_cast< u32 >(h) && h >= MR::getFrameBufferHeight();
    }
}  // namespace
#define BLOOM_CAPTURE bloomCapture
#else
#define BLOOM_CAPTURE ImageEffectLocalUtil::capture
#endif

BloomEffect::BloomEffect(const char* pName) : ImageEffectBase(pName), _14(255), _18(1.0f / ::sL1RoundPoints), _1C(1.0f / ::sL2RoundPoints), _20(128) {
    MR::connectToSceneNormalBloom(this);
    MR::createImageEffectSystemHolder();
    MR::getImageEffectResource()->createBloomTexture();

    _24 = MR::getImageEffectResource()->_0;
    _28 = MR::getImageEffectResource()->_4;
    _2C = MR::getImageEffectResource()->_C;
    _30 = MR::getImageEffectResource()->_10;
    _34 = MR::getImageEffectResource()->_14;
    _38 = MR::getImageEffectResource()->_18;
    _3C = _2C;
    _40 = MR::getImageEffectResource()->_1C;
    _44 = MR::getImageEffectResource()->_20;
#ifdef TARGET_PC
    _50 = MR::getImageEffectResource()->_24;
#endif
    _48 = new Mtx[::sL1RoundPoints * ARRAY_SIZE(::sL1RadAndOfs)];
    _4C = new Mtx[::sL2RoundPoints * ARRAY_SIZE(::sL2RadAndOfs)];

    for (u32 i = 0; i < ARRAY_SIZE(::sL1RadAndOfs); i++) {
        initBlurMtx(&_48[i * ::sL1RoundPoints], ::sL1RoundPoints, ::sL1RadAndOfs[i]._0, ::sL1RadAndOfs[i]._4);
    }

    for (u32 i = 0; i < ARRAY_SIZE(::sL2RadAndOfs); i++) {
        initBlurMtx(&_4C[i * ::sL2RoundPoints], ::sL2RoundPoints, ::sL2RadAndOfs[i]._0, ::sL2RadAndOfs[i]._4);
    }

    PPCSync();
}

#ifdef TARGET_PC
// ---- GXV_BLOOM_FAST (bloom23, 8 Nov 2026): the glow chain with 4 texture scenes instead of 8 (a MINOR CHANGE)
// The original chain after the glow sources drew, as the vgx front end and back end make it at the console's defaults (each capture of
// an EFB tile ends an offscreen pass = a GXM scene; a second copy of a pass that converts or halves is one more scene; GXV_PASSHALF=2
// draws P1-P3 at half the render scale; host census through the production code, tools/test_bloom_fast_gl.sh):
//    _28 = EFB box-halved (copy scene) -> P0 tile 0 = _28 2:1 -> _2C (RGB565) + _40 (I8, a copy scene) -> P1 tile 2 = _2C x (I8 >
//    threshold) at half size -> _44 -> P2 tile 3 = 12-tap ring blur at half size -> _30 + _34 (halved: a copy scene) -> P3 tile 20 = 36-tap
//    ring blur at half size -> _38 -> P4 tile 7 = (_30 + _38) x intensity -> _3C; EFB = _24 (the scene put back) + _3C.
//    9 captures, 16 draws (preDraw's scene capture and black fill included), 8 texture scenes + the 2 EFB splits (_24, _28) a picture.
// The fast chain makes the same glow from the same sources at the same sizes (what P1-P3 have with GXV_PASSHALF=2: 80x57 EFB pixels):
//  1. _28 as before.
//  2. T (one pass, tile 20 = 80x57): the 4x4 box of _28 (4 bilinear reads at the 2x2 sub-block centres = P1's 2:1 read of the 160x114
//     tile 0) in colour, the same box of the luma in TEV alpha (each read 3 times with swap tables R/G/B -> A, konst weights 66/128/25 /
//     255 summed over the reads = the copy unit's 0.257/0.504/0.098), and the original's compare (I8 > threshold = luma > threshold - 16)
//     selecting the colour or black -> _34. (No P0 pass, no _2C, no I8 copy scene, no second pass.)
//  3. The 12-tap blur of _34 into tile 21 (80x57) -> _38.
//  4. The 36-tap blur of _38 into tile 20 (80x57) -> _34 (no halving copy: P3's 40x28 becomes 80x57).
//  5. The scene put back as before; the glow added in ONE draw that reads _38 and _34 (each x intensity) instead of the tile 7 pass, its
//     capture _3C and the draw of _3C.
//    5 captures, 12 draws, 4 texture scenes a picture; every pass reads a copy the chain made and is read by the EFB, so GXV_PASSHALF=2
//    leaves them at their size (already the halved one).
// What changes in the picture: the threshold is taken on the TEV luma of the box-averaged texels instead of the box of the I8 copy (pixels
// within ~1/255 of the threshold can flip), the wide blur is drawn at 80x57 instead of 40x28 (finer), the glow is upsampled once (x8)
// instead of through tile 7 (x2, then x4) and is not rounded to RGB565 on the way. PETARI_BLOOM_SKIP's dry chain and
// PETARI_BLOOM_HALFRATE's reuse (which adds _38 and _34 the same way) work as before.
//   GXV_BLOOM_FAST=0 (default) the original chain; 1 the fast chain in every mode; perf: in Performance mode only.
namespace {
    // drawTexture's quad without its state: tile `tile` of a `divisions` x `divisions` grid of the EFB, texture coordinates 0..1.
    void bloomQuad(s32 divisions, s32 tile) {
        const s32 row = tile / divisions;
        const s32 column = tile % divisions;
        const f32 width = static_cast< f32 >(MR::getFrameBufferWidth()) / divisions;
        const f32 height = static_cast< f32 >(MR::getFrameBufferHeight()) / divisions;
        const f32 left = column * width;
        const f32 right = (column + 1) * width;
        const f32 bottom = (row + 1) * height;
        const f32 top = row * height;
        if (sPortDry) {
            return;  // (PETARI_BLOOM_SKIP: state only)
        }
        ++sChainDraws;
        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
        GXPosition3f32(left, bottom, -1.0f);
        GXTexCoord2f32(0.0f, 1.0f);
        GXPosition3f32(left, top, -1.0f);
        GXTexCoord2f32(0.0f, 0.0f);
        GXPosition3f32(right, bottom, -1.0f);
        GXTexCoord2f32(1.0f, 1.0f);
        GXPosition3f32(right, top, -1.0f);
        GXTexCoord2f32(1.0f, 0.0f);
        GXEnd();
    }

    // Step 2: tile `tile` of the 8 x 8 grid (80x57) = the 4x4 box of _28 (320x228) where its luma passes the threshold, else black.
    // After initDraw (one texgen through GX_TEXMTX0 = identity, one stage).
    void bloomFastThreshold(JUTTexture* pHalf, u8 threshold, s32 tile) {
        static const GXTevSwapSel sSwap[3] = {GX_TEV_SWAP1, GX_TEV_SWAP2, GX_TEV_SWAP3};
        static const u32 sTexMtx[4] = {GX_TEXMTX1, GX_TEXMTX2, GX_TEXMTX3, GX_TEXMTX4};
        // konst alpha weights / 4 per read (K0 = 17, 16, 32, 6; K1.r = 7): R 17+16+17+16 = 66, G 4 x 32 = 128, B 6+6+6+7 = 25
        static const GXTevKAlphaSel sKSel[4][3] = {{GX_TEV_KASEL_K0_R, GX_TEV_KASEL_K0_B, GX_TEV_KASEL_K0_A},
                                                   {GX_TEV_KASEL_K0_G, GX_TEV_KASEL_K0_B, GX_TEV_KASEL_K0_A},
                                                   {GX_TEV_KASEL_K0_R, GX_TEV_KASEL_K0_B, GX_TEV_KASEL_K0_A},
                                                   {GX_TEV_KASEL_K0_G, GX_TEV_KASEL_K0_B, GX_TEV_KASEL_K1_R}};
        pHalf->load(GX_TEXMAP0);
        const u8 t = threshold > 16 ? threshold - 16 : 0;  // (I8 = luma + 16; below 16 every non-black texel passes, as I8 > threshold does)
        GXSetChanMatColor(GX_COLOR0A0, Color8(t, t, t, 255));
        GXSetTevKColor(GX_KCOLOR0, Color8(17, 16, 32, 6));
        GXSetTevKColor(GX_KCOLOR1, Color8(7, 0, 0, 0));
        GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_RED);
        GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_GREEN);
        GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_BLUE);
        // the four reads: the centres of the 2x2 sub-blocks of the output pixel's 4x4 block of _28 (one _28 texel off its centre)
        const f32 du = 1.0f / pHalf->getWidth(), dv = 1.0f / pHalf->getHeight();
        GXSetNumTexGens(4);
        for (int j = 0; j < 4; j++) {
            Mtx m;
            PSMTXTrans(m, (j & 1) ? du : -du, (j & 2) ? dv : -dv, 0.0f);
            GXLoadTexMtxImm(m, sTexMtx[j], GX_MTX2x4);
            GXSetTexCoordGen2(static_cast< GXTexCoordID >(j), GX_TG_MTX2x4, GX_TG_TEX0, sTexMtx[j], GX_FALSE, GX_PTIDENTITY);
        }
        GXSetNumTevStages(13);
        for (int j = 0; j < 4; j++) {
            for (int c = 0; c < 3; c++) {
                const int i = 3 * j + c;
                const GXTevStageID s = static_cast< GXTevStageID >(i);
                GXSetTevOrder(s, static_cast< GXTexCoordID >(j), GX_TEXMAP0, GX_COLOR0A0);
                GXSetTevSwapMode(s, GX_TEV_SWAP0, sSwap[c]);
                if (c == 0) {  // (colour: a quarter of the read)
                    GXSetTevKColorSel(s, GX_TEV_KCSEL_1_4);
                    GXSetTevColorIn(s, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, j == 0 ? GX_CC_ZERO : GX_CC_CPREV);
                } else {
                    GXSetTevColorIn(s, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
                }
                GXSetTevColorOp(s, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
                GXSetTevAlphaIn(s, GX_CA_ZERO, GX_CA_TEXA, GX_CA_KONST, i == 0 ? GX_CA_ZERO : GX_CA_APREV);
                GXSetTevAlphaOp(s, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
                GXSetTevKAlphaSel(s, sKSel[j][c]);
                GXSetTevDirect(s);
            }
        }
        GXSetTevOrder(GX_TEVSTAGE12, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevSwapMode(GX_TEVSTAGE12, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GXSetTevColorIn(GX_TEVSTAGE12, GX_CC_APREV, GX_CC_RASC, GX_CC_CPREV, GX_CC_ZERO);  // (luma > threshold ? colour : 0)
        GXSetTevColorOp(GX_TEVSTAGE12, GX_TEV_COMP_R8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevAlphaIn(GX_TEVSTAGE12, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GXSetTevAlphaOp(GX_TEVSTAGE12, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevDirect(GX_TEVSTAGE12);
        GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
        bloomQuad(8, tile);
        // (back to what initDraw left: J3D's swap tables, konst colours and selections; one texgen, one modulate stage)
        GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
        GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
        GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
        GXSetTevKColor(GX_KCOLOR0, Color8(255, 255, 255, 255));
        GXSetTevKColor(GX_KCOLOR1, Color8(255, 255, 255, 255));
        for (int i = 0; i < 13; i++) {
            GXSetTevSwapMode(static_cast< GXTevStageID >(i), GX_TEV_SWAP0, GX_TEV_SWAP0);
            GXSetTevKColorSel(static_cast< GXTevStageID >(i), GX_TEV_KCSEL_1_4);
            GXSetTevKAlphaSel(static_cast< GXTevStageID >(i), GX_TEV_KASEL_1);
        }
        GXSetNumTexGens(1);
        GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0, GX_FALSE, GX_PTIDENTITY);
        GXSetNumTevStages(1);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    }

    // Step 5: EFB += (L1 + L2) x intensity, one draw (blend ONE + ONE, as the original's last draw).
    void bloomFastGlow(JUTTexture* pFine, JUTTexture* pCoarse, u8 intensity) {
        pFine->load(GX_TEXMAP0);
        pCoarse->load(GX_TEXMAP1);
        GXSetChanMatColor(GX_COLOR0A0, Color8(intensity, intensity, intensity, 255));
        GXSetNumTevStages(2);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
        GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLOR0A0);
        GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_CPREV);
        GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GXSetTevDirect(GX_TEVSTAGE1);
        GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
        bloomQuad(1, 0);
        GXSetNumTevStages(1);  // (the one-stage state the original's last draw leaves)
    }
}  // namespace

// Steps 1-5 (postDraw's chain from the glow EFB on, GXV_BLOOM_FAST). The copy filter is off on entry, as in the original.
static void bloomPostDrawFast(const BloomEffect* b, GXRenderModeObj* pRenderObj) {
    BLOOM_CAPTURE(b->_28, 1, 0, GX_TF_RGB565, true, 0);
    b->initDraw();
    GXSetCopyClear(Color8(0, 0, 0, 0xFF), 0xFFFFFF);
    bloomFastThreshold(b->_28, b->_20, 20);
    BLOOM_CAPTURE(b->_34, 8, 20, GX_TF_RGB565, false, 0);
    b->blurTexture(b->_34, b->_48, 8, 21, 6, b->_18, 2, &::sL1RadAndOfs[0]._0);
    BLOOM_CAPTURE(b->_38, 8, 21, GX_TF_RGB565, false, 0);
    b->blurTexture(b->_38, b->_4C, 8, 20, 12, b->_1C, 3, &::sL2RadAndOfs[0]._0);
    BLOOM_CAPTURE(b->_34, 8, 20, GX_TF_RGB565, false, 0);
    GXSetCopyFilter(GX_FALSE, pRenderObj->sample_pattern, GX_TRUE, pRenderObj->vfilter);
    const bool dry = sPortDry;
    sPortDry = false;  // (the scene is put back in either case)
    bloomPutBack(b);
    sPortDry = dry;
    bloomFastGlow(b->_38, b->_34, static_cast< u8 >(b->_14 * b->get_10()));
    sPortDry = false;
}

// ---- GXV_BLOOM_FOLD (bloomfold25, 11 Nov 2026; the switch and the helpers: ImageEffectLocalUtil.cpp). It overrides GXV_BLOOM_FAST while it is not 0.
// GXV_BLOOM_FAST=1's chain (above) is 4 texture scenes: _28 (EFB box-halved, a copy scene) -> T (threshold of its 4x4 box, tile 20, 80x57) -> B1 (the
// 12-tap blur of T, tile 21) -> B2 (the 36-tap blur of B1, tile 20 again), the EFB adding I x (B1 + B2). B2 reads B1: that dependency is a scene.
//  GXV_BLOOM_FOLD=1 (3 texture scenes: _28, T, Z): B1 as before (the same 12 taps of T), and B2 computed from T instead of B1 in the SAME pass, so the
//    two blurs need no second scene. The target holds Z = B1 + B2' (one texture, the glow add reads one texture):
//      B2(q) = sum_j w2 B1(q + r_j) and B1(x) = clamp(sum_i w1 T(x + s_i)), the sums over the 36 taps r_j of the wide rings (_4C) and the 12 taps s_i of
//      the narrow rings (_48). B2' estimates B1 at each wide tap's place by TWO narrow taps (s_(7j mod 12) and s_((7j + 5) mod 12)): clamp(12 w1 (T(a) + T(b)) / 2),
//      at the original weight w2: a sample of the same 432-tap kernel (72 of its taps, 4 pairs a draw, 3 stages a pair). What differs from GXV_BLOOM_FAST=1:
//      the clamp of B1 (a sum of 12 taps) is taken on the mean of two, so a glow edge where B1 is within one blur radius of its clamp differs (host harness:
//      PSNR 36 dB, mean difference 1.1 levels, at most 58 on a glow edge; 98% of the light), and the kernel is sampled, so a source smaller than the narrow
//      ring radius (about 13 px) shows its wide glow a little grainier. Z = min(1, B1 + B2') adds as the original's two separate textures do when the fade
//      intensity is 1 (scene + B1 + B2 clamps at 1 either way); under a fade (intensity < 1) a saturated core is glow x I x 1 instead of x 2.
//      11 draws in the pass (2 for B1, 9 for B2'). An exact 1/255 match is not possible in one pass: B2 reads clamp(B1), a nonlinear function of 12 reads of T.
//  GXV_BLOOM_FOLD=2 (2 texture scenes: _28, P): ONE pass at 1/16 of the picture (40x28) of 18 positions, each 4 gated reads of _28 (bloomFastThreshold's
//    luma test, evaluated per read at the box's four bilinear centres; a source smaller than the 4x4 box still lights its reads): 6 positions on a ring of
//    radius sqrt(a) and 12 on a ring of radius sqrt(a + c), a and c the mean squared radii of the narrow and wide rings of the chain. Energy: the weights sum
//    to (the chain's gain) / 2 x 0.8, g1 (1 + g2) / 2 with g1 = 12 w1, g2 = 36 w2 (7.83 / 2 at the defaults), the glow add draws x 2: for a source that does
//    not saturate that is the chain's total glow (the kernel's integral is the chain's: B1 has gain g1, B2 gain g1 g2); the 0.8 is a calibration for the
//    clamps the chain applies to bright sources (the harness' sources: 107% of the reference with 0.88, 100-106% with 0.8). The inner ring takes the mass
//    share 1 / (1 + g2) of B1, the outer ring the rest (B2): the kernel's second moment about its centre (its extent) is the chain's, ring by ring. What the
//    player can see: the glow is softer and blockier (the 40x28 target is read back bilinearly, x16), a thin or small source shows 6 + 12 faint copies of
//    itself on the rings instead of a smooth halo, and the extent of the halo is as the chain's but its shape is two rings of taps, not a bell.
namespace {
    inline f32 foldTap(const Mtx* pM, u32 i, int axis) {
        return pM[i][axis][3];
    }
    inline u32 foldTilePx(s32 divisions, s32 tile, bool y) {
        const s32 span = y ? MR::getFrameBufferHeight() : MR::getFrameBufferWidth();
        return static_cast< u32 >((y ? tile / divisions : tile % divisions) * span / divisions);
    }

    // The pass of GXV_BLOOM_FOLD=1 into tile 21 (80x57): Z = B1 + B2' from T (b->_34).
    void bloomFoldBlurPass(const BloomEffect* b, bool dry) {
        using ImageEffectLocalUtil::FoldTap;
        const f32 l = static_cast< f32 >(foldTilePx(8, 21, false)), t = static_cast< f32 >(foldTilePx(8, 21, true));
        const f32 r = l + static_cast< f32 >(MR::getFrameBufferWidth()) / 8, bt = t + static_cast< f32 >(MR::getFrameBufferHeight()) / 8;
        const u8 w1 = static_cast< u8 >(b->_18 * 255.0f);
        const u8 w2 = static_cast< u8 >(b->_1C * 255.0f);
        FoldTap taps[8];
        u32 n = 0;
        bool blend = false;
        for (u32 i = 0; i < 12; i++) {  // (B1: the original's two rings of 6, weight _18 each)
            taps[n].du = foldTap(b->_48, i, 0);
            taps[n].dv = foldTap(b->_48, i, 1);
            taps[n].w = w1;
            n++;
            if (n == 8 || i == 11) {
                ImageEffectLocalUtil::portFoldSumTaps(b->_34, taps, n, false, 0, 1, blend, dry, l, t, r, bt);
                blend = true;
                n = 0;
            }
        }
        // clamp(12 w1 T) at the wide tap's place: the mean of two narrow taps, clamp(g1 x (T(a) + T(b)) / 2) = scale x K x (T(a) + T(b)) with K = g1 / (2 scale)
        const f32 g1 = 12.0f * w1 / 255.0f;
        const u32 scale = g1 <= 4.0f ? 2 : 4;
        const f32 k = g1 / (2.0f * scale) * 255.0f + 0.5f;
        const u8 preK = k > 255.0f ? 255 : static_cast< u8 >(k);
        u32 pairs = 0;
        for (u32 j = 0; j < 36; j++) {  // (B2': the wide rings, each tap's B1 estimated by two narrow taps)
            for (u32 h = 0; h < 2; h++) {
                const u32 sIdx = (7 * j + 5 * h) % 12;
                FoldTap& tp = taps[2 * pairs + h];
                tp.du = foldTap(b->_4C, j, 0) + foldTap(b->_48, sIdx, 0);
                tp.dv = foldTap(b->_4C, j, 1) + foldTap(b->_48, sIdx, 1);
                tp.w = w2;
            }
            pairs++;
            if (pairs == 4 || j == 35) {
                ImageEffectLocalUtil::portFoldPairTaps(b->_34, taps, pairs, preK, scale, true, dry, l, t, r, bt);
                pairs = 0;
            }
        }
    }

    // The pass of GXV_BLOOM_FOLD=2 into tile 15 of 16 (40x28): 18 positions of 4 gated reads of _28 each (the chain's two rings, its energy and second moment).
    void bloomReducedPass(const BloomEffect* b, bool dry) {
        using ImageEffectLocalUtil::FoldTap;
        const f32 w1f = static_cast< u8 >(b->_18 * 255.0f) / 255.0f, w2f = static_cast< u8 >(b->_1C * 255.0f) / 255.0f;
        const f32 g1 = 12.0f * w1f, g2 = 36.0f * w2f;
        // The kernel's integral g1 (1 + g2) is the chain's gain for a source that does not saturate; the chain clamps B1 (12 w1 T) and B2 and so gives a bright
        // source less: measured over the host harness' sources 0.80 of it (calibration, tests/bloom_fast_gl: 113% of the reference without it). The target holds half
        // the glow, the glow add draws x 2.
        const f32 total = g1 * (1.0f + g2) / 2.0f * 0.80f;
        const f32 shareA = 1.0f / (1.0f + g2);
        // the mean squared radii of the chain's rings (_48: two rings of 6, _4C: three of 12; the tables hold -r (cos, sin) with the aspect on x)
        const f32 invAspect = 1.0f / MR::getAspect();
        f32 a = 0.0f, c = 0.0f;
        for (u32 i = 0; i < 12; i++) {
            const f32 x = foldTap(b->_48, i, 0) / invAspect, y = foldTap(b->_48, i, 1);
            a += (x * x + y * y) / 12.0f;
        }
        for (u32 j = 0; j < 36; j++) {
            const f32 x = foldTap(b->_4C, j, 0) / invAspect, y = foldTap(b->_4C, j, 1);
            c += (x * x + y * y) / 36.0f;
        }
        const f32 rIn = sqrtf(a), rOut = sqrtf(a + c);
        const f32 wIn = total * shareA / 6.0f, wOut = total * (1.0f - shareA) / 12.0f;
        // The box's value is gated per read (the 4 bilinear reads of a box, each its own luma test, weight a quarter of the box's): a source smaller than the 8 px
        // box (about 6 px) still lights its reads, where gating the box's mean would lose it; for a source larger than the box it is the same sum.
        const u32 wInQ = static_cast< u32 >((wIn > 1.0f ? 1.0f : wIn) * 255.0f + 0.5f), wOutQ = static_cast< u32 >((wOut > 1.0f ? 1.0f : wOut) * 255.0f + 0.5f);
        const f32 l = static_cast< f32 >(foldTilePx(16, 15, false)), t = 0.0f;
        const f32 r = l + static_cast< f32 >(b->_50->getWidth()), bt = static_cast< f32 >(b->_50->getHeight());
        const f32 ru = 1.0f / b->_28->getWidth(), rv = 1.0f / b->_28->getHeight();
        bool blend = false;
        for (u32 k = 0; k < 18; k++) {
            const bool inner = k < 6;
            const f32 ang = inner ? 2.0f * 3.14159265f * k / 6.0f : 2.0f * 3.14159265f * (k - 6 + 0.5f) / 12.0f;
            const f32 rad = inner ? rIn : rOut;
            const u32 wq = inner ? wInQ : wOutQ;
            FoldTap taps[4];
            for (u32 j = 0; j < 4; j++) {
                taps[j].du = invAspect * rad * cosf(ang) + ((j & 1) ? ru : -ru);
                taps[j].dv = rad * sinf(ang) + ((j & 2) ? rv : -rv);
                taps[j].w = static_cast< u8 >((wq * (j + 1)) / 4 - (wq * j) / 4);  // (the box's weight in whole levels: the four add to wq)
            }
            ImageEffectLocalUtil::portFoldGatedTaps(b->_28, taps, 4, 0, b->_20, blend, dry, l, t, r, bt);
            blend = true;
        }
    }

    // Performance-only GXV_BLOOM_FOLD=3: reduced-tap symmetric 2D Gaussian gated bloom (9 draws x 4 taps = 36 gated reads instead of fold=2's
    // 18 draws / 72 reads; halves Melty Molten / Bowser bloom 3D fragment work while using the EXACT same 4-tap portFoldGatedTaps TEV shader key,
    // so zero shader re-harvest is required). Normalized Gaussian tap weights across the 4 concentric bands (center 4, inner 8, mid 8, outer 16)
    // sum identically to 1.0 and scale to match fold=2's total bloom energy.
    static constexpr u32 kFold3BlurDraws = 9;
    static constexpr u32 kFold3Taps = kFold3BlurDraws * 4;
    static constexpr u32 kFold3TotalDraws = kFold3BlurDraws + 3;  // preDraw black fill + 9 blur draws + put-back + glow add
    static u32 sFold3Pics = 0, sFold3Dry = 0;
    static bool sFold3Logged = false;

    int bloomFoldLevel(const BloomEffect* b) {
        int m = ImageEffectLocalUtil::portBloomFoldModeNormal();
        if (m >= 2 && b->_50 == nullptr) {
            m = 1;
        }
        return m;
    }

    void bloomFold3BandWeights(f32 rIn, f32 rOut, f32 outTapNorm[4]) {
        const f32 rMid = 0.5f * (rIn + rOut);
        const f32 rad[4] = {0.0f, rIn, rMid, rOut};
        const f32 cnt[4] = {4.0f, 8.0f, 8.0f, 16.0f};
        const f32 sigma = rOut > 1e-6f ? rOut : 1.0f;
        const f32 inv2s2 = 0.5f / (sigma * sigma);
        f32 raw[4];
        f32 sum = 0.0f;
        for (u32 band = 0; band < 4; band++) {
            raw[band] = expf(-(rad[band] * rad[band]) * inv2s2);
            sum += cnt[band] * raw[band];
        }
        const f32 invSum = sum > 0.0f ? 1.0f / sum : 1.0f / 36.0f;
        for (u32 band = 0; band < 4; band++) {
            outTapNorm[band] = raw[band] * invSum;
        }
    }

    void bloomFold3Pass(const BloomEffect* b, bool dry) {
        using ImageEffectLocalUtil::FoldTap;
        const f32 w1f = static_cast< u8 >(b->_18 * 255.0f) / 255.0f, w2f = static_cast< u8 >(b->_1C * 255.0f) / 255.0f;
        const f32 g1 = 12.0f * w1f, g2 = 36.0f * w2f;
        const f32 total = g1 * (1.0f + g2) / 2.0f * 0.80f;
        const f32 invAspect = 1.0f / MR::getAspect();
        f32 a = 0.0f, c = 0.0f;
        for (u32 i = 0; i < 12; i++) {
            const f32 x = foldTap(b->_48, i, 0) / invAspect, y = foldTap(b->_48, i, 1);
            a += (x * x + y * y) / 12.0f;
        }
        for (u32 j = 0; j < 36; j++) {
            const f32 x = foldTap(b->_4C, j, 0) / invAspect, y = foldTap(b->_4C, j, 1);
            c += (x * x + y * y) / 36.0f;
        }
        const f32 rIn = sqrtf(a), rOut = sqrtf(a + c), rMid = 0.5f * (rIn + rOut);
        f32 tapNorm[4];
        bloomFold3BandWeights(rIn, rOut, tapNorm);
        u32 drawQ[4];
        for (u32 band = 0; band < 4; band++) {
            const f32 wDraw = total * 4.0f * tapNorm[band];
            drawQ[band] = static_cast< u32 >((wDraw > 1.0f ? 1.0f : wDraw) * 255.0f + 0.5f);
        }
        const f32 l = static_cast< f32 >(foldTilePx(16, 15, false)), t = 0.0f;
        const f32 r = l + static_cast< f32 >(b->_50->getWidth()), bt = static_cast< f32 >(b->_50->getHeight());
        const f32 ru = 1.0f / b->_28->getWidth(), rv = 1.0f / b->_28->getHeight();
        bool blend = false;
        auto emitDraw = [&](const FoldTap taps[4]) {
            ImageEffectLocalUtil::portFoldGatedTaps(b->_28, taps, 4, 0, b->_20, blend, dry, l, t, r, bt);
            blend = true;
        };
        auto fillW = [](FoldTap taps[4], u32 wq) {
            for (u32 j = 0; j < 4; j++) {
                taps[j].w = static_cast< u8 >((wq * (j + 1)) / 4 - (wq * j) / 4);
            }
        };
        // Band 0 (1 draw): center 2x2 box
        {
            FoldTap taps[4];
            fillW(taps, drawQ[0]);
            for (u32 j = 0; j < 4; j++) {
                taps[j].du = (j & 1) ? ru : -ru;
                taps[j].dv = (j & 2) ? rv : -rv;
            }
            emitDraw(taps);
        }
        // Band 1 (2 draws): inner 8-gon at rIn (each draw 4-fold rotationally symmetric)
        for (u32 d = 0; d < 2; d++) {
            FoldTap taps[4];
            fillW(taps, drawQ[1]);
            for (u32 j = 0; j < 4; j++) {
                const f32 ang = 2.0f * 3.14159265f * static_cast< f32 >(d + 2 * j) / 8.0f;
                taps[j].du = invAspect * rIn * cosf(ang);
                taps[j].dv = rIn * sinf(ang);
            }
            emitDraw(taps);
        }
        // Band 2 (2 draws): mid 8-gon at 0.5*(rIn + rOut), half-step rotated
        for (u32 d = 0; d < 2; d++) {
            FoldTap taps[4];
            fillW(taps, drawQ[2]);
            for (u32 j = 0; j < 4; j++) {
                const f32 ang = 2.0f * 3.14159265f * (static_cast< f32 >(d + 2 * j) + 0.5f) / 8.0f;
                taps[j].du = invAspect * rMid * cosf(ang);
                taps[j].dv = rMid * sinf(ang);
            }
            emitDraw(taps);
        }
        // Band 3 (4 draws): outer 16-gon at rOut (each draw 4-fold rotationally symmetric)
        for (u32 d = 0; d < 4; d++) {
            FoldTap taps[4];
            fillW(taps, drawQ[3]);
            for (u32 j = 0; j < 4; j++) {
                const f32 ang = 2.0f * 3.14159265f * static_cast< f32 >(d + 4 * j) / 16.0f;
                taps[j].du = invAspect * rOut * cosf(ang);
                taps[j].dv = rOut * sinf(ang);
            }
            emitDraw(taps);
        }
        if (!ImageEffectLocalUtil::portRehearsing()) {
            if (!sFold3Logged) {
                sFold3Logged = true;
                const f32 normSum = 4.0f * tapNorm[0] + 8.0f * tapNorm[1] + 8.0f * tapNorm[2] + 16.0f * tapNorm[3];
                port_log_summary("bloom-fold: GXV_BLOOM_FOLD=3: reduced-tap symmetric Gaussian glow (1 blur pass, %u gated draws = %u taps, 2 texture scenes, %u total draws/pic; same 4-tap TEV key as fold=2; norm weight sum %.6f)",
                                 static_cast< unsigned >(kFold3BlurDraws), static_cast< unsigned >(kFold3Taps), static_cast< unsigned >(kFold3TotalDraws), static_cast< double >(normSum));
            }
            ++sFold3Pics;
            if (dry) {
                ++sFold3Dry;
            }
            if (sFold3Pics >= 600) {
                port_log_summary("bloom-fold3: %u pictures (%u dry): 1 blur pass/pic, %u gated draws/pic (%u taps), %u total draws/pic, 2 texture scenes",
                                 static_cast< unsigned >(sFold3Pics), static_cast< unsigned >(sFold3Dry), static_cast< unsigned >(kFold3BlurDraws),
                                 static_cast< unsigned >(kFold3Taps), static_cast< unsigned >(kFold3TotalDraws));
                sFold3Pics = 0;
                sFold3Dry = 0;
            }
        }
    }
}  // namespace

// GXV_BLOOM_FOLD = 1 / 2 / 3 (postDraw's chain from the glow EFB on). The copy filter is off on entry, as in the original.
static void bloomPostDrawFold(const BloomEffect* b, GXRenderModeObj* pRenderObj, int level) {
    const bool dry = sPortDry;
    BLOOM_CAPTURE(b->_28, 1, 0, GX_TF_RGB565, true, 0);
    b->initDraw();
    GXSetCopyClear(Color8(0, 0, 0, 0xFF), 0xFFFFFF);
    JUTTexture* pGlow;
    if (level == 1) {
        bloomFastThreshold(b->_28, b->_20, 20);
        BLOOM_CAPTURE(b->_34, 8, 20, GX_TF_RGB565, false, 0);
        bloomFoldBlurPass(b, dry);
        BLOOM_CAPTURE(b->_38, 8, 21, GX_TF_RGB565, false, 0);
        pGlow = b->_38;
    } else {
        if (level == 3) {
            bloomFold3Pass(b, dry);
        } else {
            bloomReducedPass(b, dry);
        }
        BLOOM_CAPTURE(b->_50, 16, 15, GX_TF_RGB565, false, 0);
        pGlow = b->_50;
    }
    ImageEffectLocalUtil::portFoldEnd();
    GXSetCopyFilter(GX_FALSE, pRenderObj->sample_pattern, GX_TRUE, pRenderObj->vfilter);
    sPortDry = false;  // (the scene is put back in either case)
    bloomPutBack(b);
    sPortDry = dry;
    ImageEffectLocalUtil::portFoldGlow(pGlow, static_cast< u8 >(b->_14 * b->get_10()), level >= 2, dry);
    ImageEffectLocalUtil::portFoldEnd();
    sPortDry = false;
}
#endif

void BloomEffect::preDraw() const {
#ifdef TARGET_PC
    if (!isSomething() && !ImageEffectLocalUtil::portRehearsing()) {
        return;
    }
#else
    if (!isSomething()) {
        return;
    }
#endif
#ifdef TARGET_PC
    if (ImageEffectLocalUtil::portBloomReuse(this)) {
        return;  // (PETARI_BLOOM_HALFRATE: no scene capture and no black fill; the glow models are left out too, SceneExecutor.cpp)
    }
    sChainCaptures = 1;  // (GXV_BLOOM_FAST's census: the scene capture and the black fill below)
    sChainDraws = 1;
#endif

#ifdef TARGET_PC
    ImageEffectLocalUtil::portBloomBracketMark(PORT_BB_CAPTURE);  // (GXV_BLOOM_BRACKET: the next copy is the scene capture)
#endif
    {
        CopyFilterNegater copyFilterNegator;
        ImageEffectLocalUtil::capture(_24, 1, 0, GX_TF_RGBA8, false, 0);
    }

#ifdef TARGET_PC
    ImageEffectLocalUtil::portBloomBracketMark(PORT_BB_FILL_BEGIN);
#endif
    MR::fillScreen(Color8(0, 0, 0, 0xFF).mGXColor);
#ifdef TARGET_PC
    ImageEffectLocalUtil::portBloomBracketMark(PORT_BB_FILL_END);
    ImageEffectLocalUtil::portBloomFillDone(this, bloomFillWhole());  // (PETARI_BLOOM_SKIP: what is drawn from here on is a glow source)
#endif
    MR::loadViewMtx();
    MR::loadProjectionMtx();
    GXSetClipMode(GX_CLIP_ENABLE);
}

void BloomEffect::postDraw() const {
#ifdef TARGET_PC
    if (!isSomething() && !ImageEffectLocalUtil::portRehearsing()) {
        return;
    }
#else
    if (!isSomething()) {
        return;
    }
#endif
#ifdef TARGET_PC
    if (ImageEffectLocalUtil::portBloomReuse(this)) {
        // PETARI_BLOOM_HALFRATE: the scene was not replaced by the black glow buffer (preDraw left out), so only the glow the
        // previous picture made (_3C) is added, as the last draw below adds it.
        GXRenderModeObj* pReuseRenderObj = JUTVideo::getManager()->getRenderMode();
        initDraw();
        GXSetCopyClear(Color8(0, 0, 0, 0xFF), 0xFFFFFF);  // (the copy state the full path leaves)
        GXSetCopyFilter(GX_FALSE, pReuseRenderObj->sample_pattern, GX_TRUE, pReuseRenderObj->vfilter);
        sPortDry = ImageEffectLocalUtil::portBloomGlowBlack(this);  // (PETARI_BLOOM_SKIP: the kept glow is black: adding it changes nothing)
        sChainCaptures = sChainDraws = 0;
        int reuseFold = bloomFoldLevel(this);
        if (reuseFold) {
            ImageEffectLocalUtil::portFoldGlow(reuseFold >= 2 ? _50 : _38, static_cast< u8 >(_14 * get_10()), reuseFold >= 2, sPortDry);  // (GXV_BLOOM_FOLD keeps its glow in _38 / _50)
            ImageEffectLocalUtil::portFoldEnd();
        } else if (ImageEffectLocalUtil::portBloomFastMode() == 1) {
            bloomFastGlow(_38, _34, static_cast< u8 >(_14 * get_10()));  // (GXV_BLOOM_FAST keeps its glow in _38 and _34, not in _3C)
        } else {
            drawTexture(_3C, 1, 0, 0xFF, DrawType_1);
        }
        ImageEffectLocalUtil::portBloomChainNote(this, sChainCaptures, sChainDraws, 2);
        sPortDry = false;
        return;
    }
    // (PETARI_BLOOM_SKIP: no glow source drew on the black: the chain below is made dry, its glow is black)
    // (and the scene's put-back draw below covers the whole EFB: viewport and scissor checked again, here)
    sPortDry = ImageEffectLocalUtil::portBloomNothingDrawn(this, bloomFillWhole());
    if (ImageEffectLocalUtil::portBloomBracketDead()) {
        ImageEffectLocalUtil::portBloomBracketMark(PORT_BB_DEAD);  // (GXV_BLOOM_BRACKET: the capture, the fill and the put-back restore what was there)
    }
    int foldMode = bloomFoldLevel(this);
    if (foldMode) {
        GXRenderModeObj* pFoldRenderObj = JUTVideo::getManager()->getRenderMode();
        GXSetCopyFilter(GX_FALSE, pFoldRenderObj->sample_pattern, GX_FALSE, pFoldRenderObj->vfilter);
        const bool dryFold = sPortDry;
        bloomPostDrawFold(this, pFoldRenderObj, foldMode);
        const u32 foldDraws = foldMode == 1 ? 11u : (foldMode == 3 ? kFold3TotalDraws : 21u);
        const int noteKind = dryFold ? 3 : (foldMode == 1 ? 8 : 9);
        ImageEffectLocalUtil::portBloomChainNote(this, sChainCaptures, foldDraws, noteKind);
        return;
    }
    if (ImageEffectLocalUtil::portBloomFastMode() == 1) {
        GXRenderModeObj* pFastRenderObj = JUTVideo::getManager()->getRenderMode();
        GXSetCopyFilter(GX_FALSE, pFastRenderObj->sample_pattern, GX_FALSE, pFastRenderObj->vfilter);
        const bool dryFast = sPortDry;
        bloomPostDrawFast(this, pFastRenderObj);
        ImageEffectLocalUtil::portBloomChainNote(this, sChainCaptures, sChainDraws, dryFast ? 3 : 1);
        return;
    }
    const bool dryNote = sPortDry;
#endif

    GXRenderModeObj* pRenderObj = JUTVideo::getManager()->getRenderMode();
    GXSetCopyFilter(GX_FALSE, pRenderObj->sample_pattern, GX_FALSE, pRenderObj->vfilter);

    BLOOM_CAPTURE(_28, 1, 0, GX_TF_RGB565, true, 0);
    initDraw();
    GXSetCopyClear(Color8(0, 0, 0, 0xFF), 0xFFFFFF);
    drawTexture(_28, 4, 0, 0xFF, DrawType_0);

    BLOOM_CAPTURE(_2C, 4, 0, GX_TF_RGB565, false, 0);
    BLOOM_CAPTURE(_40, 4, 0, GX_TF_I8, false, 0);
    drawTexture(_2C, 4, 2, 0xFF, DrawType_0);

    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_TEXC, GX_CC_RASC, GX_CC_ONE, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_COMP_R8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    drawTexture(_40, 4, 2, _20, DrawType_2);

    GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    BLOOM_CAPTURE(_44, 4, 2, GX_TF_RGB565, false, 0);
    blurTexture(_44, _48, 4, 3, 6, _18, 2, &::sL1RadAndOfs[0]._0);

    BLOOM_CAPTURE(_30, 4, 3, GX_TF_RGB565, false, 0);
    BLOOM_CAPTURE(_34, 4, 3, GX_TF_RGB565, true, 0);
    blurTexture(_34, _4C, 8, 20, 12, _1C, 3, &::sL2RadAndOfs[0]._0);

    BLOOM_CAPTURE(_38, 8, 20, GX_TF_RGB565, false, 0);
    const u8 intensity = _14 * get_10();
    drawTexture(_30, 4, 7, intensity, DrawType_0);
    drawTexture(_38, 4, 7, intensity, DrawType_1);

    BLOOM_CAPTURE(_3C, 4, 7, GX_TF_RGB565, false, 0);
    GXSetCopyFilter(GX_FALSE, pRenderObj->sample_pattern, GX_TRUE, pRenderObj->vfilter);

#ifdef TARGET_PC
    const bool dry = sPortDry;
    sPortDry = false;  // (the scene is put back in either case)
    bloomPutBack(this);
    sPortDry = dry;
    drawTexture(_3C, 1, 0, 0xFF, DrawType_1);
    sPortDry = false;
    ImageEffectLocalUtil::portBloomChainNote(this, sChainCaptures, sChainDraws, dryNote ? 3 : 0);
#else
    drawTexture(_24, 1, 0, 0xFF, DrawType_0);
    drawTexture(_3C, 1, 0, 0xFF, DrawType_1);
#endif
}

#ifdef TARGET_PC
// dive27 (GXV_REHEARSE, inside a warm-up scope behind the closed wipe): the real chain once - preDraw's scene capture, postDraw's captures and passes in the mode the stage uses - so the copy
// targets of every glow texture exist and the chain's programs are linked before the camera first goes under water. The scope records no draw (the picture is not touched; the glow
// intensity is _14 * _10 = 0 while the effect is off); the captures are real copies. The picture bookkeeping (reuse, skip, census) is left alone while portRehearsing().
void BloomEffect::portRehearse() const {
    ImageEffectLocalUtil::portRehearseSet(true);
    preDraw();
    postDraw();
    ImageEffectLocalUtil::portRehearseSet(false);
}
#endif

u8 BloomEffect::getIntensity1Default() const {
    return 42;
}

u8 BloomEffect::getIntensity2Default() const {
    return 21;
}

bool BloomEffect::isForEachModel() const {
    return true;
}

void BloomEffect::initDraw() const {
    MR::reinitGX();
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);

    Mtx44 projectionMtx;
    C_MTXOrtho(projectionMtx, 0.0f, MR::getFrameBufferHeight(), 0.0f, MR::getFrameBufferWidth(), -10000.0f, 10000.0f);
    GXSetProjection(projectionMtx, GX_ORTHOGRAPHIC);

    MR::loadViewMtxFor2DModel();
    GXClearVtxDesc();
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);

    TMtx34f posMtx;
    posMtx.identity();
    GXLoadPosMtxImm(posMtx.toMtxPtr(), GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);

    GXSetCullMode(GX_CULL_NONE);
    GXSetClipMode(GX_CLIP_ENABLE);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0, GX_FALSE, GX_PTIDENTITY);
    GXLoadTexMtxImm(posMtx.toMtxPtr(), GX_TEXMTX0, GX_MTX2x4);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GXSetNumIndStages(0);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, Color8(0, 0, 0, 0));
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetZCompLoc(GX_TRUE);
    GXSetDither(GX_FALSE);
}

void BloomEffect::drawTexture(JUTTexture* pTexture, s32 param2, s32 param3, u8 intensity, BLOOM_TEX_DRAW_TYPE param5) const {
    pTexture->load(GX_TEXMAP0);
    GXSetChanMatColor(GX_COLOR0A0, Color8(intensity, intensity, intensity, 255));

    switch (param5) {
    case DrawType_0:
        GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
        break;
    case DrawType_1:
        GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
        break;
    case DrawType_2:
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCCLR, GX_BL_ZERO, GX_LO_CLEAR);
        break;
    }

    s32 row = param3 / param2;
    s32 column = param3 % param2;
    f32 width = static_cast< f32 >(MR::getFrameBufferWidth()) / param2;
    f32 height = static_cast< f32 >(MR::getFrameBufferHeight()) / param2;
    f32 left = column * width;
    f32 right = (column + 1) * width;
    f32 bottom = (row + 1) * height;
    f32 top = row * height;

#ifdef TARGET_PC
    if (sPortDry) {
        return;  // (PETARI_BLOOM_SKIP: state only)
    }
    ++sChainDraws;  // (GXV_BLOOM_FAST's census)
#endif
    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
    {
        GXPosition3f32(left, bottom, -1.0f);
        GXTexCoord2f32(0.0f, 1.0f);

        GXPosition3f32(left, top, -1.0f);
        GXTexCoord2f32(0.0f, 0.0f);

        GXPosition3f32(right, bottom, -1.0f);
        GXTexCoord2f32(1.0f, 1.0f);

        GXPosition3f32(right, top, -1.0f);
        GXTexCoord2f32(1.0f, 0.0f);
    }
    GXEnd();
}

void BloomEffect::blurTexture(JUTTexture* pTexture, Mtx* pMtx, s32 param3, s32 param4, u32 param5, f32 param6, u32 param7, const f32* pParam8) const {
    initBlur(pTexture, 8, param6);

    for (s32 i = 0; i < param7; i++) {
        drawBlur(param3, param4, &pMtx[i * param5], 8, param5);
    }

    GXSetNumTexGens(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
}

void BloomEffect::initBlurMtx(Mtx* pMtx, u32 count, f32 radius, f32 offset) const {
    f32 invAspect = 1.0f / MR::getAspect();
    radius = -radius;

    for (u32 i = 0; i < count; i++) {
        PSMTXTrans(pMtx[i], invAspect * (radius * JMACosRadian(offset + i * JMath::TAngleConstant_< f32 >::RADIAN_DEG360() / count)),
                   radius * JMASinRadian(offset + i * JMath::TAngleConstant_< f32 >::RADIAN_DEG360() / count), 0.0f);
    }

    DCStoreRangeNoSync(pMtx, sizeof(Mtx) * count);
}

void BloomEffect::initBlur(JUTTexture* pTexture, u32 param2, f32 intensity) const {
    GXSetNumTexGens(param2);

    for (u32 i = 0; i < param2; i++) {
        GXSetTexCoordGen2(GXTexCoordID(i), GX_TG_MTX3x4, GX_TG_TEX0, ::sTexMtxID[i], GX_FALSE, GX_PTIDENTITY);
    }

    GXSetNumTevStages(param2);

    for (u32 i = 0; i < param2; i++) {
        GXSetTevOrder(GXTevStageID(i), GXTexCoordID(i), GX_TEXMAP0, GX_COLOR0A0);
        GXSetTevColorIn(GXTevStageID(i), GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_CPREV);
        GXSetTevColorOp(GXTevStageID(i), GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
        GXSetTevAlphaIn(GXTevStageID(i), GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
        GXSetTevAlphaOp(GXTevStageID(i), GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    }

    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
    GXSetTevColorOp(GXTevStageID(param2), GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetChanMatColor(GX_COLOR0A0, Color8(intensity * 255.0f, intensity * 255.0f, intensity * 255.0f, 255));
    pTexture->load(GX_TEXMAP0);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
}

void BloomEffect::drawBlur(s32 divisions, s32 tile, Mtx* pMtx, u32 stageCount, u32 matrixCount) const {
    GXSetArray(GX_TEX_MTX_ARRAY, pMtx, sizeof(Mtx));

    s32 row = tile / divisions;
    s32 column = tile % divisions;
    f32 width = static_cast< f32 >(MR::getFrameBufferWidth()) / divisions;
    f32 height = static_cast< f32 >(MR::getFrameBufferHeight()) / divisions;
    f32 left = column * width;
    f32 right = (column + 1) * width;
    f32 bottom = (row + 1) * height;
    f32 top = row * height;

    GXSetNumTexGens(stageCount);
    GXSetNumTevStages(stageCount);

    for (u32 i = 0; i < matrixCount; i += stageCount) {
        u32 remaining = matrixCount - i;
        if (remaining < stageCount) {
            GXSetNumTexGens(remaining);
            GXSetNumTevStages(remaining);
            stageCount = remaining;
        }

        for (u32 j = 0; j < stageCount; j++) {
            GXLoadTexMtxIndx(i + j, ::sTexMtxID[j], GX_MTX3x4);
        }

#ifdef TARGET_PC
        if (!sPortDry) {  // (PETARI_BLOOM_SKIP: state only)
        ++sChainDraws;    // (GXV_BLOOM_FAST's census)
#endif
        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
        GXPosition3f32(left, bottom, -1.0f);
        GXTexCoord2f32(0.0f, 1.0f);

        GXPosition3f32(left, top, -1.0f);
        GXTexCoord2f32(0.0f, 0.0f);

        GXPosition3f32(right, bottom, -1.0f);
        GXTexCoord2f32(1.0f, 1.0f);

        GXPosition3f32(right, top, -1.0f);
        GXTexCoord2f32(1.0f, 0.0f);
        GXEnd();
#ifdef TARGET_PC
        }
#endif

        if (i == 0) {
            GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
        }
    }
}
