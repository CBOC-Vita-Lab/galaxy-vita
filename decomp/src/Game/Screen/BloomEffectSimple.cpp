#include "Game/Screen/BloomEffectSimple.hpp"
#include "Game/Screen/CopyFilterNegater.hpp"
#include "Game/Screen/ImageEffectLocalUtil.hpp"
#include "Game/Screen/ImageEffectResource.hpp"
#include "Game/Screen/ImageEffectSystemHolder.hpp"
#include "Game/Util/Color.hpp"
#include "Game/Util/DrawUtil.hpp"
#ifdef TARGET_PC
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include <JSystem/JUtility/JUTTexture.hpp>
#include <cmath>
#endif

void BloomEffectSimple_FORCE_MATCH_SDATA2() {
    (void)1.0f;
}

namespace {
    // static const s32 sLen = _;
    // static const s32 sDivNum = _;
};  // namespace

namespace {
    class TexSpec {
    public:
        TexSpec() {
        }

        void set(JUTTexture*, s32, s32, GXTexFmt);

        /* 0x00 */ JUTTexture* _0;
        /* 0x04 */ s32 _4;
        /* 0x08 */ s32 _8;
        /* 0x0C */ GXTexFmt _C;
    };
};  // namespace

BloomEffectSimple::BloomEffectSimple()
    : ImageEffectBase("\x8a\xc8\x88\xd5\x83\x75\x83\x8b\x81\x5b\x83\x80"), _14(), _18(), _1C(), _20(), _24(), _28(), _2C(), mMaskFilter(), mThreshold(205), mIntensity(0.3f) {
    MR::connectToSceneImageEffect(this);
    MR::createImageEffectSystemHolder();
    MR::getImageEffectResource()->createBloomTexture();

    _14 = MR::getImageEffectResource()->_0;
    _18 = MR::getImageEffectResource()->_4;
    _1C = MR::getImageEffectResource()->_C;
    _20 = MR::getImageEffectResource()->_10;
    _24 = MR::getImageEffectResource()->_14;
    _28 = MR::getImageEffectResource()->_18;
    _2C = MR::getImageEffectResource()->_1C;
#ifdef TARGET_PC
    mFoldTex = MR::getImageEffectResource()->_24;
#endif
}

#ifdef TARGET_PC
// ---- GXV_BLOOM_FAST (bloom23, 9 Nov 2026; the switch: ImageEffectLocalUtil.cpp). The simple bloom (SimpleBloom areas: Beach Bowl above
// water, ...) as the vgx front end and back end make it at the console's default switches (host census, tools/test_bloom_fast_gl.sh):
//    _20 = the EFB's tile 0 saved, _18 = the EFB box-halved (an EFB split, 2 copy scenes) -> tile 0 = _18 2:1 drawn IN the EFB (its old
//    contents are the picture's: not an offscreen pass) -> _1C (RGB565, read by nothing) + _2C (I8 / R8 / G8 / B8): an EFB split and 2
//    copy scenes -> tile 0 x (_2C > threshold) in the EFB -> _24 = its 2x2 box (EFB split, copy scene) -> 8-tap ring blur of _24 into the
//    8x8 grid's tile 0 (a direct pass) -> _28 -> _20 drawn back, _28 added.  6 captures, 12 draws, 6 texture scenes + 3 EFB splits.
//  GXV_BLOOM_FAST=2 (exact): the capture of _1C is left out (nothing reads it: this effect never samples _1C; BloomEffect's _2C / _3C and
//    DepthOfFieldBlur's _24, the same texture, write it before reading it, and PETARI_BLOOM_HALFRATE's reuse of BloomEffect's _3C needs
//    BloomEffect to have made the last chain). 5 captures, 5 texture scenes + 3 EFB splits; the picture is the same.
//  GXV_BLOOM_FAST=1 (a MINOR CHANGE): _20 and _18 as before; then ONE offscreen pass into tile 0: _18 2:1 (the original tile 0's value),
//    its luma (or its R / G / B for the other mask filters) in TEV alpha from three reads with the swap tables R/G/B -> A (konst 66, 128,
//    25 / 255 = the copy unit's 0.257 / 0.504 / 0.098), and the compare (I8 > threshold = luma > threshold - 16) keeping the colour or
//    black -> _1C (160x114); the 8-tap blur of _1C over the whole tile 0 (its first draw covers the pass before: both are offscreen passes
//    for the front end) -> _28 = its 2x2 box (80x57, a copy scene: the original's _28 = the blur of the box, blur and box swapped); _20
//    back, _28 added as before. 4 captures, 11 draws, 5 texture scenes + 1 EFB split (tile 0 is never drawn in the EFB; _20's draw covers
//    what the passes leave). What changes: the threshold on the TEV value of the unrounded texel instead of the I8 copy (pixels within
//    ~1/255 of the threshold can flip); the blur taps at 160x114 before the 2x2 box instead of at 80x57 after it.
namespace {
    void simpleFastThreshold(JUTTexture* pHalf, s32 maskFilter, u8 threshold) {
        static const GXTevSwapSel sSwap[3] = {GX_TEV_SWAP1, GX_TEV_SWAP2, GX_TEV_SWAP3};
        static const GXTevKAlphaSel sKSel[3] = {GX_TEV_KASEL_K0_R, GX_TEV_KASEL_K0_G, GX_TEV_KASEL_K0_B};
        static const u8 sWeight[4][3] = {{66, 128, 25}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}};  // (I8, R8, G8, B8: the copy's channel)
        const s32 f = maskFilter >= 0 && maskFilter <= 3 ? maskFilter : 0;
        u8 t = threshold;
        if (f == 0) {
            t = threshold > 16 ? threshold - 16 : 0;  // (I8 = luma + 16; below 16 every non-black texel passes, as I8 > threshold does)
        }
        pHalf->load(GX_TEXMAP0);
        GXSetChanMatColor(GX_COLOR0A0, Color8(t, t, t, 255));
        GXSetTevKColor(GX_KCOLOR0, Color8(sWeight[f][0], sWeight[f][1], sWeight[f][2], 0));
        GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_RED);
        GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_GREEN);
        GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_BLUE);
        GXSetNumTexGens(1);
        GXSetNumTevStages(4);
        for (int i = 0; i < 3; i++) {
            const GXTevStageID s = static_cast< GXTevStageID >(i);
            GXSetTevOrder(s, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
            GXSetTevSwapMode(s, GX_TEV_SWAP0, sSwap[i]);
            GXSetTevColorIn(s, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, i == 0 ? GX_CC_TEXC : GX_CC_CPREV);
            GXSetTevColorOp(s, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GXSetTevAlphaIn(s, GX_CA_ZERO, GX_CA_TEXA, GX_CA_KONST, i == 0 ? GX_CA_ZERO : GX_CA_APREV);
            GXSetTevAlphaOp(s, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GXSetTevKAlphaSel(s, sKSel[i]);
            GXSetTevDirect(s);
        }
        GXSetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevSwapMode(GX_TEVSTAGE3, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GXSetTevColorIn(GX_TEVSTAGE3, GX_CC_APREV, GX_CC_RASC, GX_CC_CPREV, GX_CC_ZERO);  // (value > threshold ? colour : 0)
        GXSetTevColorOp(GX_TEVSTAGE3, GX_TEV_COMP_R8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GXSetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevDirect(GX_TEVSTAGE3);
        GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
        ImageEffectLocalUtil::sendTextureVertex(4, 0);
        // (back to J3D's defaults, as reinitGX left them: swap tables, konst colour 0 and the alpha selections; one modulate stage)
        GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
        GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
        GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
        GXSetTevKColor(GX_KCOLOR0, Color8(255, 255, 255, 255));
        for (int i = 0; i < 3; i++) {
            GXSetTevSwapMode(static_cast< GXTevStageID >(i), GX_TEV_SWAP0, GX_TEV_SWAP0);
            GXSetTevKAlphaSel(static_cast< GXTevStageID >(i), GX_TEV_KASEL_1);
        }
        GXSetNumTevStages(1);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    }
}  // namespace
// ---- GXV_BLOOM_FOLD (bloomfold25, 11 Nov 2026; the switch and the helpers: ImageEffectLocalUtil.cpp). The fast chain above (GXV_BLOOM_FAST=1) is 5
// texture scenes + 1 EFB split: _20 (tile 0 saved), _18 (the EFB box-halved), the threshold pass (tile 0, 160x114, a pass), the 8-tap blur (tile 0, a pass,
// ending in a 2:1 copy into _28, a copy scene of its own). The fold makes the threshold and the blur ONE pass at 80x57 that ends in a plain 1:1 copy:
//  GXV_BLOOM_FOLD=1: Z(q) = 1/4 sum over the 4 sub-pixels s of the 2x2 block of the 160x114 grid at q, 1/8 sum over the 8 ring taps o_k of
//    gate(_18 at s + o_k): the fast chain's "blur of the thresholded half-size picture, then the 2x2 box" with the threshold evaluated at each tap
//    (the colour of the bilinear read of _18 where its value exceeds the threshold, as the threshold pass computes it) instead of read from a texture
//    of the thresholded picture. 32 gated taps = 8 draws, 4 taps each (about 440 thousand texel reads). Texture scenes: _20, _18, Z = 3 (5 before). The
//    threshold edge is taken where the tap reads, not interpolated from the four texels of the thresholded texture (pixels within one 160-grid pixel of
//    a threshold edge can differ); weight per tap 8/255 = (1/4)(31/255) x 1.032, the glow add's intensity is divided by 1.032.
//  GXV_BLOOM_FOLD=2: the same with ONE position per output texel at 40x28 (1/16 of the picture, ImageEffectResource::_24): the 8 ring taps alone, weight
//    31/255 as the chain's blur, no 2x2 box (the bilinear read-back of the 40x28 target, x16, smooths in its place). 8 gated taps = 2 draws. Same 3 texture
//    scenes, a fraction of the fill; the energy is the fast chain's (the same weights; the box and the read-back keep the mean), the glow is blockier.
namespace {
    void simpleFoldPass(const BloomEffectSimple* b, int level, f32& rGainFix) {
        using ImageEffectLocalUtil::FoldTap;
        const f32 fbW = static_cast< f32 >(MR::getFrameBufferWidth()), fbH = static_cast< f32 >(MR::getFrameBufferHeight());
        const f32 radius = 0.009f;
        const f32 vRadius = radius * (MR::isScreen16Per9() ? 1.333f : 1.0f);
        FoldTap taps[4];
        u32 n = 0;
        bool blend = false;
        f32 l = 0.0f, t = 0.0f, r, bt;
        JUTTexture* pOut;
        u32 subs;
        u8 w;
        if (level == 1) {
            r = fbW / 8;
            bt = fbH / 8;
            pOut = b->_28;
            subs = 4;
            w = 8;
            rGainFix = 8.0f / 7.75f;
        } else {
            pOut = b->mFoldTex;
            r = static_cast< f32 >(pOut->getWidth());  // (tile 0: the corner _20 keeps and puts back)
            bt = static_cast< f32 >(pOut->getHeight());
            subs = 1;
            w = static_cast< u8 >(255.0f / 8);
            rGainFix = 1.0f;
        }
        const f32 hx = 0.5f * 4.0f / fbW, hy = 0.5f * 4.0f / fbH;  // (half a pixel of the 160x114 grid, in picture coordinates)
        for (u32 sub = 0; sub < subs; sub++) {
            const f32 sx = level == 1 ? ((sub & 1) ? hx : -hx) : 0.0f, sy = level == 1 ? ((sub & 2) ? hy : -hy) : 0.0f;
            for (u32 k = 0; k < 8; k++) {
                const f32 ang = 2.0f * 3.14159265f * k / 8;
                taps[n].du = sx + radius * cosf(ang);
                taps[n].dv = sy + vRadius * sinf(ang);
                taps[n].w = w;
                n++;
                if (n == 4) {
                    ImageEffectLocalUtil::portFoldGatedTaps(b->_18, taps, n, b->mMaskFilter, b->mThreshold, blend, false, l, t, r, bt);
                    blend = true;
                    n = 0;
                }
            }
        }
        if (level == 1) {
            ImageEffectLocalUtil::capture(pOut, 8, 0, GX_TF_RGB565, false, 0);
        } else {
            ImageEffectLocalUtil::capture(pOut, 16, 0, GX_TF_RGB565, false, 0);
        }
    }
}  // namespace
#endif

#ifdef TARGET_PC
// dive27 (GXV_REHEARSE, inside a warm-up scope behind the closed wipe): the real chain once (see BloomEffect::portRehearse): the copy targets of its textures and its programs exist before the
// first SimpleBloom area is entered (Beach Bowl: the 240x136 / 480x272 / 120x68 targets were made in play 8-12 s in, 34 and 61 ms).
void BloomEffectSimple::portRehearse() const {
    ImageEffectLocalUtil::portRehearseSet(true);
    draw();
    ImageEffectLocalUtil::portRehearseSet(false);
}
#endif

#ifdef TARGET_PC
namespace {
    // sky49: where the last full chain of this effect kept its glow, so PETARI_BLOOM_HALFRATE's reuse adds the same texture the same way the
    // chain's own last draw does (0: _28 with drawTexture, as the original / fast / exact chains; 1: GXV_BLOOM_FOLD=1, _28 through portFoldGlow with
    // the gain fix; 2: GXV_BLOOM_FOLD=2, mFoldTex (_24) through portFoldGlow). Before sky49 the reuse always did the first, so with the fold at 2 (Beach
    // Bowl's Performance default) it added _28, which that chain never writes: the glow came and went every picture (the sky flickered at 30 Hz).
    int sSimpleGlowKind = 0;
    f32 sSimpleGlowGain = 1.0f;
}  // namespace
#endif

void BloomEffectSimple::draw() const {
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
        // PETARI_BLOOM_HALFRATE: the glow the previous picture made (_28), added as the last draw below adds it; the EFB's corner
        // is not used, so it is not restored either.
        MR::reinitGX();
        ImageEffectLocalUtil::setupDrawTexture();
        if (sSimpleGlowKind != 0 && (sSimpleGlowKind == 1 || mFoldTex != nullptr)) {
            const u8 foldReuseAlpha = 255.0f * mIntensity * get_10() / sSimpleGlowGain;
            ImageEffectLocalUtil::portFoldGlow(sSimpleGlowKind == 1 ? _28 : mFoldTex, foldReuseAlpha, false, false);
            ImageEffectLocalUtil::portFoldEnd();
        } else {
            const u8 reuseAlpha = 255.0f * mIntensity * get_10();
            ImageEffectLocalUtil::drawTexture(_28, 1, 0, reuseAlpha, ImageEffectLocalUtil::TexDrawType_1);
        }
        ImageEffectLocalUtil::portBloomChainNote(this, 0, 1, 7);
        return;
    }
    const int portFast = ImageEffectLocalUtil::portBloomFastMode();  // (GXV_BLOOM_FAST: 1 the fast chain, 2 the exact cut)
    const int portFold = ImageEffectLocalUtil::portBloomFoldMode();  // (GXV_BLOOM_FOLD: 1 the folded chain, 2/3 the reduced glow; overrides GXV_BLOOM_FAST)
    if (portFold == 1 || (portFold >= 2 && mFoldTex != nullptr)) {
        CopyFilterNegater foldNegator;
        ImageEffectLocalUtil::capture(_20, 4, 0, GX_TF_RGB565, false, 0);
        ImageEffectLocalUtil::capture(_18, 4, 0, GX_TF_RGB565, true, 0);
        MR::reinitGX();
        ImageEffectLocalUtil::setupDrawTexture();
        f32 gainFix = 1.0f;
        const int simpleFold = portFold == 1 ? 1 : 2;
        simpleFoldPass(this, simpleFold, gainFix);
        ImageEffectLocalUtil::portFoldEnd();
        ImageEffectLocalUtil::drawTexture(_20, 4, 0, 255, ImageEffectLocalUtil::TexDrawType_0);
        const u8 foldAlpha = 255.0f * mIntensity * get_10() / gainFix;
        sSimpleGlowKind = simpleFold;
        sSimpleGlowGain = gainFix;
        ImageEffectLocalUtil::portFoldGlow(simpleFold == 1 ? _28 : mFoldTex, foldAlpha, false, false);
        ImageEffectLocalUtil::portFoldEnd();
        ImageEffectLocalUtil::portBloomChainNote(this, 3, simpleFold == 1 ? 12 : 6, 9 + simpleFold);
        return;
    }
    if (portFast == 1) {
        CopyFilterNegater fastNegator;
        ImageEffectLocalUtil::capture(_20, 4, 0, GX_TF_RGB565, false, 0);
        ImageEffectLocalUtil::capture(_18, 4, 0, GX_TF_RGB565, true, 0);
        MR::reinitGX();
        ImageEffectLocalUtil::setupDrawTexture();
        simpleFastThreshold(_18, mMaskFilter, mThreshold);
        ImageEffectLocalUtil::capture(_1C, 4, 0, GX_TF_RGB565, false, 0);
        ImageEffectLocalUtil::blurTexture(_1C, 4, 0, 8, 0.009f, 1.0f);  // (over the whole tile 0: its first draw covers the pass above)
        ImageEffectLocalUtil::capture(_28, 4, 0, GX_TF_RGB565, true, 0);   // (its 2x2 box: 80x57, the original's _28)
        ImageEffectLocalUtil::drawTexture(_20, 4, 0, 255, ImageEffectLocalUtil::TexDrawType_0);
        sSimpleGlowKind = 0;
        const u8 fastAlpha = 255.0f * mIntensity * get_10();
        ImageEffectLocalUtil::drawTexture(_28, 1, 0, fastAlpha, ImageEffectLocalUtil::TexDrawType_1);
        ImageEffectLocalUtil::portBloomChainNote(this, 4, 11, 5);  // (4 captures, 11 draws)
        return;
    }
#endif

    CopyFilterNegater copyFilterNegator;

    ::TexSpec texSpec;
    texSpec.set(_20, 4, 0, GX_TF_RGB565);

    ImageEffectLocalUtil::capture(texSpec._0, texSpec._4, texSpec._8, texSpec._C, false, 0);
    ImageEffectLocalUtil::capture(_18, texSpec._4, texSpec._8, GX_TF_RGB565, true, 0);
    MR::reinitGX();
    ImageEffectLocalUtil::setupDrawTexture();
    ImageEffectLocalUtil::drawTexture(_18, 4, 0, 255, ImageEffectLocalUtil::TexDrawType_0);
#ifdef TARGET_PC
    if (portFast != 2) {  // (GXV_BLOOM_FAST=2: nothing reads _1C, see above)
        ImageEffectLocalUtil::capture(_1C, 4, 0, GX_TF_RGB565, false, 0);
    }
#else
    ImageEffectLocalUtil::capture(_1C, 4, 0, GX_TF_RGB565, false, 0);
#endif

    switch (mMaskFilter) {
    case 0:
        ImageEffectLocalUtil::capture(_2C, 4, 0, GX_TF_I8, false, 0);
        break;
    case 1:
        ImageEffectLocalUtil::capture(_2C, 4, 0, GX_CTF_R8, false, 0);
        break;
    case 2:
        ImageEffectLocalUtil::capture(_2C, 4, 0, GX_CTF_G8, false, 0);
        break;
    case 3:
        ImageEffectLocalUtil::capture(_2C, 4, 0, GX_CTF_B8, false, 0);
        break;
    }

    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_TEXC, GX_CC_RASC, GX_CC_ONE, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_COMP_R8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    ImageEffectLocalUtil::drawTexture(_2C, 4, 0, mThreshold, ImageEffectLocalUtil::TexDrawType_3);
    ImageEffectLocalUtil::capture(_24, 4, 0, GX_TF_RGB565, true, 0);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    ImageEffectLocalUtil::setupDrawTexture();
    texSpec.set(_24, 8, 0, GX_TF_RGB565);
    ImageEffectLocalUtil::blurTexture(texSpec._0, texSpec._4, texSpec._8, 8, 0.009f, 1.0f);
    ImageEffectLocalUtil::capture(_28, texSpec._4, texSpec._8, texSpec._C, false, 0);
    ImageEffectLocalUtil::drawTexture(_20, 4, 0, 255, ImageEffectLocalUtil::TexDrawType_0);
    const u8 alpha = 255.0f * mIntensity * get_10();
    ImageEffectLocalUtil::drawTexture(_28, 1, 0, alpha, ImageEffectLocalUtil::TexDrawType_1);
#ifdef TARGET_PC
    sSimpleGlowKind = 0;
    ImageEffectLocalUtil::portBloomChainNote(this, portFast == 2 ? 5 : 6, 12, portFast == 2 ? 6 : 4);
#endif
}

void ::TexSpec::set(JUTTexture* pParam1, s32 param2, s32 param3, GXTexFmt param4) {
    _0 = pParam1;
    _4 = param2;
    _8 = param3;
    _C = param4;
}
