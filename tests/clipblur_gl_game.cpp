// PETARI_CLIPCHAIN_ONEPASS host proof, the game's half (tools/test_clipblur_gl.sh). Compiled with the game's flags (TARGET_PC) and linked with the
// PRODUCTION decomp/src/Game/MapObj/FallOutFieldDraw.cpp and Game/Screen/ImageEffectLocalUtil.cpp (this tree's, and CB_BASE's for the reference),
// the console's GX library (decomp RVL_SDK/gx) and, through tests/clipblur_gl_main.cpp, the production vgx front end and GLES back end on Mesa.
// Each picture: a floor (what ClippedMapParts draws: colour, constant destination alpha 0), the clip volumes' net effect in the EFB's alpha (alpha-only
// discs / rectangles that add 4, as ClipArea::draw's front/back pass leaves it: tests/vgx_clipfield_pipeline_gl.cpp models the volumes themselves),
// FallOutFieldDraw::draw() (the actor's own method), the picture read back (an RGBA8 copy of the EFB), the display copy.
// What this file stands in for: JUTTexture (JUTTexture.cpp's GX calls), JUTVideo's render mode (640x456 EFB), MR::* (the GX calls of the real ones), the
// ScreenAlphaCapture textures (A8 mask 0.5 scale, the two 0.25 ones; I8, linear, clamped: ScreenAlphaCapture::createScreenAlpha), the clip area holder.
#include "Game/MapObj/FallOutFieldDraw.hpp"
#include "Game/Screen/ImageEffectLocalUtil.hpp"
#include "Game/Camera/CameraDirector.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "JSystem/JUtility/JUTTexture.hpp"
#include "JSystem/JUtility/JUTVideo.hpp"
#include <revolution/gx.h>
#include <revolution/mtx.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
void* hx_guest_alloc(u32 bytes);
void hx_fifo_init(void);
void hx_fifo_flush(void);
void hx_mark(int which);
}

namespace {
constexpr int kW = 640, kH = 456;
GXRenderModeObj sMode;
alignas(16) unsigned char sVideo[512];
JUTTexture* sTex[5] = {};
bool sActive = true;
}  // namespace
JUTVideo* JUTVideo::sManager = reinterpret_cast< JUTVideo* >(sVideo);

namespace MR {
    s32 getFrameBufferWidth() {
        return kW;
    }
    f32 getAspect() {
        return 16.0f / 9.0f;
    }
    bool isScreen16Per9() {
        return true;
    }
    void loadViewMtx() {
    }
    void loadProjectionMtx() {
        Mtx44 p;
        C_MTXPerspective(p, 45.0f, 16.0f / 9.0f, 1.0f, 10000.0f);
        GXSetProjection(p, GX_PERSPECTIVE);
    }
    void loadViewMtxFor2DModel() {
    }
    CameraDirector* getCameraDirector() {
        return nullptr;
    }
    const TVec3f getCamPos() {
        return TVec3f(0.0f, 0.0f, 0.0f);
    }
    TVec3f getCamZdir() {
        return TVec3f(0.0f, 0.0f, 1.0f);
    }
    // (FallOutFieldDraw's services)
    void createClipAreaHolder() {
    }
    void connectToScene(NameObj*, int, int, int, int) {
    }
    void createScreenAlphaSceneObj(s32, f32) {
    }
    bool isActiveClipArea() {
        return sActive;
    }
    JUTTexture* getScreenAlphaTexture(s32 i) {
        return sTex[i];
    }
    void loadScreenAlphaTexture(s32 i, GXTexMapID id) {
        sTex[i]->load(id);
    }
    void reinitGX() {  // (J3DSys::reinitGX + DrawUtil's GXSetAlphaUpdate(GX_FALSE): as tests/bloom_fast_gl_game.cpp)
        static u8* nullTexGuest = nullptr;
        if (!nullTexGuest) {
            nullTexGuest = static_cast< u8* >(hx_guest_alloc(64));
        }
        const GXColor black = {0, 0, 0, 0}, white = {255, 255, 255, 255};
        GXSetNumChans(0);
        GXSetNumTexGens(1);
        GXSetNumTevStages(1);
        GXSetNumIndStages(0);
        GXSetCullMode(GX_CULL_BACK);
        GXSetCoPlanar(GX_FALSE);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetChanAmbColor(GX_COLOR0A0, black);
        GXSetChanAmbColor(GX_COLOR1A1, black);
        GXSetChanMatColor(GX_COLOR0A0, white);
        GXSetChanMatColor(GX_COLOR1A1, white);
        GXSetCurrentMtx(GX_PNMTX0);
        for (int i = 0; i < 8; i++) {
            GXSetTexCoordGen2(GXTexCoordID(i), GX_TG_MTX2x4, GXTexGenSrc(GX_TG_TEX0 + i), GX_IDENTITY, GX_FALSE, 0x7D);
        }
        GXTexObj texObj;
        GXInitTexObj(&texObj, nullTexGuest, 4, 4, GX_TF_IA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        for (int i = 0; i < 8; i++) {
            GXLoadTexObj(&texObj, GXTexMapID(i));
        }
        for (int i = 0; i < 16; i++) {
            GXSetTevOrder(GXTevStageID(i), GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
        }
        GXSetTevColor(GX_TEVREG0, white);
        GXSetTevColor(GX_TEVREG1, white);
        GXSetTevColor(GX_TEVREG2, white);
        GXSetTevKColor(GX_KCOLOR0, white);
        GXSetTevKColor(GX_KCOLOR1, white);
        GXSetTevKColor(GX_KCOLOR2, white);
        GXSetTevKColor(GX_KCOLOR3, white);
        for (int i = 0; i < 16; i++) {
            GXSetTevColorIn(GXTevStageID(i), GX_CC_RASC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
            GXSetTevColorOp(GXTevStageID(i), GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GXSetTevAlphaIn(GXTevStageID(i), GX_CA_RASA, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
            GXSetTevAlphaOp(GXTevStageID(i), GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GXSetTevKColorSel(GXTevStageID(i), GX_TEV_KCSEL_1_4);
            GXSetTevKAlphaSel(GXTevStageID(i), GX_TEV_KASEL_1);
            GXSetTevSwapMode(GXTevStageID(i), GX_TEV_SWAP0, GX_TEV_SWAP0);
        }
        GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
        GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
        GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
        GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        for (int i = 0; i < 16; i++) {
            GXSetTevDirect(GXTevStageID(i));
        }
        GXSetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetDither(GX_TRUE);
        GXSetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, black);
        GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
        GXSetZCompLoc(GX_TRUE);
    }
}  // namespace MR
bool CameraDirector::isRotatingHard() const {
    return false;
}

void JUTTexture::initTexObj() {
    GXInitTexObj(&mObj, mImage, mTIMG->mWidth, mTIMG->mHeight, static_cast< GXTexFmt >(mTIMG->mFormat), static_cast< GXTexWrapMode >(mWrapS),
                 static_cast< GXTexWrapMode >(mWrapT), GX_FALSE);
    GXInitTexObjLOD(&mObj, static_cast< GXTexFilter >(mMinType), static_cast< GXTexFilter >(mMagType), mMinLod / 8.0f, mMaxLod / 8.0f, mLodBias / 100.0f,
                    mTIMG->mBiasClamp, mTIMG->mDoEdgeLod, static_cast< GXAnisotropy >(mTIMG->mMaxAnisotropy));
}
void JUTTexture::load(GXTexMapID id) {
    GXLoadTexObj(&mObj, id);
}
void JUTTexture::capture(int x, int y, GXTexFmt format, bool mipmap, GXBool clear) {
    if (mFlag & 0x1) {
        if (mipmap) {
            GXSetTexCopySrc(x, y, mTIMG->mWidth * 2, mTIMG->mHeight * 2);
        } else {
            GXSetTexCopySrc(x, y, mTIMG->mWidth, mTIMG->mHeight);
        }
        GXSetTexCopyDst(mTIMG->mWidth, mTIMG->mHeight, format, mipmap);
        GXCopyTex(mImage, clear);
        GXPixModeSync();
    }
}
JUTTexture::~JUTTexture() {
}

namespace {
JUTTexture* makeTexture(f32 scale, GXTexFmt fmt) {  // (ScreenAlphaCapture::createScreenAlpha: I8, clamped, linear)
    const int w = static_cast< int >(scale * kW), h = static_cast< int >(scale * kH);
    const u32 bytes = GXGetTexBufferSize(w, h, fmt, GX_FALSE, 1);
    ResTIMG* timg = static_cast< ResTIMG* >(hx_guest_alloc(bytes + sizeof(ResTIMG)));
    std::memset(timg, 0, sizeof(ResTIMG));
    timg->mFormat = fmt;
    timg->mWidth = w;
    timg->mHeight = h;
    timg->mWrapS = GX_CLAMP;
    timg->mWrapT = GX_CLAMP;
    timg->mMinType = GX_LINEAR;
    timg->mMagType = GX_LINEAR;
    timg->mImageNum = 1;
    timg->mImageDataOffset = sizeof(ResTIMG);
    JUTTexture* t = new JUTTexture();
    t->mTIMG = timg;
    t->mImage = reinterpret_cast< u8* >(timg) + sizeof(ResTIMG);
    t->mPalette = nullptr;
    t->mWrapS = GX_CLAMP;
    t->mWrapT = GX_CLAMP;
    t->mMinType = GX_LINEAR;
    t->mMagType = GX_LINEAR;
    t->mMinLod = 0;
    t->mMaxLod = 0;
    t->mLodBias = 0;
    t->setCaptureFlag(true);
    t->initTexObj();
    return t;
}

FallOutFieldDraw* sField = nullptr;
u8* sReadback = nullptr;
u8* sXfb = nullptr;
int sShape = 0;

void setupColourDraw(bool zTest, bool zWrite) {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    Mtx m;
    PSMTXIdentity(m);
    GXLoadPosMtxImm(m, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    Mtx44 p;
    C_MTXOrtho(p, 0.0f, kH, 0.0f, kW, 0.0f, 10000.0f);  // (pixels; z = -depth)
    GXSetProjection(p, GX_ORTHOGRAPHIC);
    GXSetViewport(0.0f, 0.0f, kW, kH, 0.0f, 1.0f);
    GXSetScissor(0, 0, kW, kH);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetNumIndStages(0);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetCullMode(GX_CULL_NONE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetZMode(zTest ? GX_TRUE : GX_FALSE, GX_LEQUAL, zWrite ? GX_TRUE : GX_FALSE);
    GXSetZCompLoc(GX_TRUE);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, GXColor{0, 0, 0, 0});
}
inline void vtx(f32 x, f32 y, f32 depth, u8 r, u8 g, u8 b, u8 a) {
    GXPosition3f32(x, y, -depth);
    GXColor4u8(r, g, b, a);
}
void quad(f32 x0, f32 y0, f32 x1, f32 y1, f32 depth, u8 r, u8 g, u8 b, u8 a = 255) {
    GXBegin(GX_QUADS, GX_VTXFMT1, 4);
    vtx(x0, y0, depth, r, g, b, a);
    vtx(x1, y0, depth, r, g, b, a);
    vtx(x1, y1, depth, r, g, b, a);
    vtx(x0, y1, depth, r, g, b, a);
    GXEnd();
}
void disc(f32 cx, f32 cy, f32 rad, u8 a) {
    const int seg = 96;
    GXBegin(GX_TRIANGLEFAN, GX_VTXFMT1, seg + 2);
    vtx(cx, cy, 100, 0, 0, 0, a);
    for (int i = 0; i <= seg; i++) {
        const f32 t = 6.2831853f * i / seg;
        vtx(cx + rad * std::cos(t), cy + rad * std::sin(t), 100, 0, 0, 0, a);
    }
    GXEnd();
}
void tri(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2, u8 a) {
    GXBegin(GX_TRIANGLES, GX_VTXFMT1, 3);
    vtx(x0, y0, 100, 0, 0, 0, a);
    vtx(x1, y1, 100, 0, 0, 0, a);
    vtx(x2, y2, 100, 0, 0, 0, a);
    GXEnd();
}
// The clip volumes' net effect in the alpha plane: +4 where a volume covers (alpha only, ONE + ONE, as ClipArea::draw's passes leave it).
// shapes: 0 none (empty), 1 disc, 2 rectangle, 3 diagonal half-plane, 4 slits, 5 blobs + an overlap (alpha 8), 6 disc moving with the picture number
void drawVolumes(int n) {
    if (sShape == 0) {
        return;
    }
    setupColourDraw(false, false);
    GXSetColorUpdate(GX_FALSE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetDstAlpha(GX_FALSE, 0);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_COPY);
    switch (sShape) {
    case 1: disc(320, 228, 120, 4); break;
    case 2: quad(150, 100, 470, 360, 100, 0, 0, 0, 4); break;
    case 3: tri(0, 160, 640, 384, 0, 456, 4); tri(640, 384, 640, 456, 0, 456, 4); break;
    case 4: quad(40, 220, 600, 226, 100, 0, 0, 0, 4); quad(300, 40, 305, 420, 100, 0, 0, 0, 4); break;
    case 5:
        disc(100, 100, 9, 4); disc(200, 300, 5, 4); disc(500, 120, 14, 4); disc(560, 380, 3, 4); disc(320, 228, 60, 4); disc(330, 238, 50, 4);
        break;
    case 6: disc(200.0f + 7.0f * n, 228, 110, 4); break;
    }
#ifndef CB_BASE
    if (sShape != 0) {
        ImageEffectLocalUtil::portClipVolumeDrawn();
    }
#endif
    GXSetColorUpdate(GX_TRUE);
}
void drawFloor(int n) {
    setupColourDraw(true, true);
    GXSetDstAlpha(GX_TRUE, 0);
    GXSetAlphaUpdate(GX_TRUE);
    const f32 s = static_cast< f32 >(n % 3);
    GXBegin(GX_QUADS, GX_VTXFMT1, 4);  // (a gradient floor)
    vtx(0, 0, 5000, 60, 120 + s, 60, 255);
    vtx(kW, 0, 5000, 100, 170, 90, 255);
    vtx(kW, kH, 5000, 40, 80, 160, 255);
    vtx(0, kH, 5000, 90, 90, 90, 255);
    GXEnd();
    quad(250, 150, 330, 330, 800, 200, 160, 90);  // (a pillar the glow is drawn over)
    quad(420, 60, 560, 130, 900, 230, 60, 60);
}
}  // namespace

extern "C" void hg_init(int shape) {
    sShape = shape;
    hx_fifo_init();
    static u8 fifoDummy[64];
    GXInit(fifoDummy, sizeof fifoDummy);
    sMode = GXNtsc480IntDf;
    sMode.fbWidth = kW;
    sMode.efbHeight = kH;
    sMode.xfbHeight = kH;
    *reinterpret_cast< GXRenderModeObj** >(sVideo + sizeof(void*)) = &sMode;
    GXSetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
    GXSetCopyFilter(GX_FALSE, sMode.sample_pattern, GX_FALSE, sMode.vfilter);
    GXSetDispCopySrc(0, 0, kW, kH);
    GXSetDispCopyDst(kW, kH);
    sReadback = static_cast< u8* >(hx_guest_alloc(GXGetTexBufferSize(kW, kH, GX_TF_RGBA8, GX_FALSE, 1)));
    sXfb = static_cast< u8* >(hx_guest_alloc(kW * kH * 2));
    sTex[2] = makeTexture(0.5f, GX_TF_I8);
    sTex[3] = makeTexture(0.25f, GX_TF_I8);
    sTex[4] = makeTexture(0.25f, GX_TF_I8);
    void* mem = std::calloc(1, sizeof(FallOutFieldDraw) + 64);
    sField = static_cast< FallOutFieldDraw* >(mem);
    sField->_C = std::getenv("CB_C") ? std::atoi(std::getenv("CB_C")) != 0 : false;
}

extern "C" void hg_frame(int n) {
    ImageEffectLocalUtil::portPictureTick();
    GXSetViewport(0.0f, 0.0f, kW, kH, 0.0f, 1.0f);
    GXSetScissor(0, 0, kW, kH);
    MR::reinitGX();
    drawFloor(n);
    drawVolumes(n);
    MR::reinitGX();
    hx_fifo_flush();
    hx_mark(0);
    if (!std::getenv("CB_NOFIELD")) {
        sField->FallOutFieldDraw::draw();
    }
    hx_fifo_flush();
    hx_mark(1);
    GXSetCopyFilter(GX_FALSE, sMode.sample_pattern, GX_FALSE, sMode.vfilter);
    if (!std::getenv("CB_NOREAD")) {
        GXSetTexCopySrc(0, 0, kW, kH);
        GXSetTexCopyDst(kW, kH, GX_TF_RGBA8, GX_FALSE);
        GXCopyTex(sReadback, GX_FALSE);
        GXPixModeSync();
    }
    GXSetCopyClear(GXColor{0, 0, 0, 0}, 0xFFFFFF);
    GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    GXCopyDisp(sXfb, GX_TRUE);
    hx_fifo_flush();
}
