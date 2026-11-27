// GXV_BLOOM_FAST host proof, the game's half (tools/test_bloom_fast_gl.sh). Compiled with the game's flags (TARGET_PC) and linked with
// the PRODUCTION decomp/src/Game/Screen/BloomEffect.cpp and ImageEffectLocalUtil.cpp (the base tree's or the patched tree's), the
// console's GX library (decomp RVL_SDK/gx) and, through tests/bloom_fast_gl_main.cpp, the production vgx front end and GLES back end on
// Mesa. Each picture: a 3D scene, BloomEffect::preDraw (scene capture, black fill), glow sources drawn on the black with the scene's
// depth (as the BloomModel draw buffers are), BloomEffect::postDraw (the glow chain, the scene put back, the glow added), the picture
// read back (an RGBA8 copy of the EFB), the display copy. What this file stands in for: JUTTexture (same GX calls as JUTTexture.cpp),
// JUTVideo's render mode (640x456 EFB, as the port's), MR::reinitGX (J3DSys::reinitGX's GX calls), MR::fillScreen (DrawUtil.cpp's),
// the camera's aspect (16:9) and projection, the BloomArea parameters (intensity 255, threshold 128, blur 42 / 21 = the defaults).
#include "Game/Camera/CameraDirector.hpp"
#include "Game/Map/WaterAreaHolder.hpp"
#include "Game/Screen/BloomEffect.hpp"
#include "Game/Screen/BloomEffectSimple.hpp"
#include "Game/Screen/ImageEffectLocalUtil.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
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
void hx_mark(int which);  // (bloom_fast_gl_main.cpp: the command count at the chain's start / end)
u32 port_gx_draw_serial(void);
void port_gx_marker(unsigned int) {}  // (the draw-bracket markers: not recorded here)
}

namespace {
constexpr int kW = 640, kH = 456;
GXRenderModeObj sMode;
alignas(16) unsigned char sVideo[512];  // (a JUTVideo: its vtable pointer, then mRenderObj; only getRenderMode / getEfbHeight are used)
}  // namespace
JUTVideo* JUTVideo::sManager = reinterpret_cast< JUTVideo* >(sVideo);

// ---- MR:: and J3D stand-ins (the GX calls of the real ones)
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
    void reinitGX() {  // (J3DSys::reinitGX + DrawUtil's GXSetAlphaUpdate(GX_FALSE))
        static u8 nullTex[32] __attribute__((aligned(32)));
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
        (void)nullTex;
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
        GXSetAlphaUpdate(GX_FALSE);
    }
    void fillScreen(const GXColor& rColor) {  // (DrawUtil.cpp fillScreenSetup + fillScreenArea)
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_U16, 0);
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        Mtx m;
        PSMTXIdentity(m);
        GXLoadPosMtxImm(m, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);
        Mtx44 projMtx;
        C_MTXOrtho(projMtx, 0.0f, kH, 0.0f, kW, -1.0f, 1.0f);
        GXSetProjection(projMtx, GX_ORTHOGRAPHIC);
        GXSetNumChans(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GXSetZMode(GX_FALSE, GX_LEQUAL, GX_FALSE);
        GXSetZCompLoc(GX_FALSE);
        GXSetCullMode(GX_CULL_NONE);
        GXSetDither(GX_FALSE);
        GXSetNumTexGens(0);
        GXSetNumTevStages(1);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
        GXSetTevColor(GX_TEVREG0, rColor);
        GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
        GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_A0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
        GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        GXPosition2u16(0, 0);
        GXPosition2u16(kW, 0);
        GXPosition2u16(kW, kH);
        GXPosition2u16(0, kH);
        GXEnd();
    }
    CameraDirector* getCameraDirector() {
        return nullptr;
    }
}  // namespace MR
// (dive27: ImageEffectLocalUtil.cpp asks; the scene here is not a water stage)
bool WaterAreaFunction::isBloomForWater() {
    return false;
}
bool CameraDirector::isRotatingHard() const {
    return false;
}

// ---- JUTTexture (JUTTexture.cpp's GX calls; the image in the guest arena)
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
JUTTexture* makeTexture(f32 scale, GXTexFmt fmt) {  // (ImageEffectResource::createTexture)
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

BloomEffect* sBloom = nullptr;
BloomEffectSimple* sSimple = nullptr;  // (BF_SIMPLE=1: the simple bloom of SimpleBloom areas, BloomEffectSimple::draw, instead)
u8* sReadback = nullptr;
u8* sXfb = nullptr;
int sThreshold = 128;

// position + colour vertices, no texture
void setupColourDraw(bool ortho, bool zTest, bool zWrite) {
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
    if (ortho) {
        C_MTXOrtho(p, 0.0f, kH, 0.0f, kW, 0.0f, 10000.0f);  // (pixels; z = -depth)
        GXSetProjection(p, GX_ORTHOGRAPHIC);
    } else {
        MR::loadProjectionMtx();
    }
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
inline void vtx(f32 x, f32 y, f32 depth, u8 r, u8 g, u8 b) {  // (ortho pixels, depth 0..10000 nearer = smaller)
    GXPosition3f32(x, y, -depth);
    GXColor4u8(r, g, b, 255);
}
void quad(f32 x0, f32 y0, f32 x1, f32 y1, f32 depth, u8 r, u8 g, u8 b) {
    GXBegin(GX_QUADS, GX_VTXFMT1, 4);
    vtx(x0, y0, depth, r, g, b);
    vtx(x1, y0, depth, r, g, b);
    vtx(x1, y1, depth, r, g, b);
    vtx(x0, y1, depth, r, g, b);
    GXEnd();
}
void gradQuad(f32 x0, f32 y0, f32 x1, f32 y1, f32 depth, const u8 c[4][3]) {
    GXBegin(GX_QUADS, GX_VTXFMT1, 4);
    vtx(x0, y0, depth, c[0][0], c[0][1], c[0][2]);
    vtx(x1, y0, depth, c[1][0], c[1][1], c[1][2]);
    vtx(x1, y1, depth, c[2][0], c[2][1], c[2][2]);
    vtx(x0, y1, depth, c[3][0], c[3][1], c[3][2]);
    GXEnd();
}
void disc(f32 cx, f32 cy, f32 rad, f32 depth, u8 r, u8 g, u8 b, u8 er, u8 eg, u8 eb) {  // (centre colour -> edge colour)
    const int seg = 48;
    GXBegin(GX_TRIANGLEFAN, GX_VTXFMT1, seg + 2);
    vtx(cx, cy, depth, r, g, b);
    for (int i = 0; i <= seg; i++) {
        const f32 a = 6.2831853f * i / seg;
        vtx(cx + rad * std::cos(a), cy + rad * std::sin(a), depth, er, eg, eb);
    }
    GXEnd();
}
u32 rnd(u32& s) {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
}

// The 3D scene: a sky gradient, ground, walls and boxes at several depths (what the glow is depth-tested against).
void drawScene(int n) {
    setupColourDraw(true, true, true);
    if (std::getenv("BF_ONE")) {  // (bloomfold25: a black scene, the one glow source alone)
        return;
    }
    const u8 sky[4][3] = {{40, 70, 140}, {60, 90, 170}, {150, 170, 200}, {120, 150, 190}};
    gradQuad(0, 0, kW, kH, 9000, sky);
    const u8 ground[4][3] = {{70, 110, 40}, {80, 120, 50}, {30, 60, 20}, {40, 70, 25}};
    gradQuad(0, 300, kW, kH, 5000, ground);
    u32 s = 1234u;
    for (int i = 0; i < 14; i++) {
        const f32 x = static_cast< f32 >(rnd(s) % 600), y = static_cast< f32 >(120 + rnd(s) % 300), w = static_cast< f32 >(20 + rnd(s) % 90),
                  h = static_cast< f32 >(20 + rnd(s) % 120);
        const f32 d = static_cast< f32 >(500 + rnd(s) % 4000);
        quad(x + (n % 7), y, x + w + (n % 7), y + h, d, 60 + rnd(s) % 160, 60 + rnd(s) % 160, 60 + rnd(s) % 160);
    }
    quad(250, 150, 330, 330, 800, 120, 90, 70);  // (a near pillar: hides part of a glow source)
}

// The glow sources, on the black fill with the scene's depth (BloomModel draws: depth test, no depth write).
void drawGlow(int n) {
    setupColourDraw(true, true, false);
    if (const char* one = std::getenv("BF_ONE")) {  // (bloomfold25: ONE source only, a disc of radius BF_ONE (pixels), away from the edges: the glow's energy and extent)
        disc(320, 228, static_cast< f32 >(std::atoi(one)), 400, 255, 235, 200, 255, 235, 200);
        return;
    }
    // a sun, partly off the top-left edge
    disc(30 + 3 * n, 25, 46, 8000, 255, 250, 225, 255, 200, 120);
    // a lamp behind the pillar (partly hidden)
    disc(300 + 2 * n, 220, 30, 1500, 255, 220, 140, 255, 160, 60);
    // star bits: small, saturated, some below the threshold
    u32 s = 99u + static_cast< u32 >(n);
    static const u8 cols[8][3] = {{255, 80, 80}, {80, 255, 80}, {120, 120, 255}, {255, 255, 255}, {255, 255, 90}, {90, 255, 255}, {200, 120, 255}, {130, 100, 90}};
    for (int i = 0; i < 28; i++) {
        const f32 x = static_cast< f32 >(rnd(s) % 620 + 10), y = static_cast< f32 >(rnd(s) % 430 + 10), sz = static_cast< f32 >(1 + rnd(s) % 5);
        const u8* c = cols[i % 8];
        quad(x, y, x + sz, y + sz, 400, c[0], c[1], c[2]);
    }
    // a thin bright line (a spider thread)
    GXBegin(GX_QUADS, GX_VTXFMT1, 4);
    vtx(80, 400, 300, 255, 210, 60);
    vtx(560, 260 + n, 300, 255, 210, 60);
    vtx(560, 261.5f + n, 300, 255, 210, 60);
    vtx(80, 401.5f, 300, 255, 210, 60);
    GXEnd();
    // a large dim surface around the threshold (luma ~ 120..150), and a black-to-white ramp across it
    // (BF_BRIGHT=1 leaves these two out: only sources well above or below the threshold, as most glow models are)
    if (!std::getenv("BF_BRIGHT")) {
        const u8 dim[4][3] = {{150, 120, 100}, {160, 140, 110}, {130, 120, 110}, {120, 110, 100}};
        gradQuad(420, 330, 600, 420, 450, dim);
        const u8 ramp[4][3] = {{0, 0, 0}, {255, 255, 255}, {255, 255, 255}, {0, 0, 0}};
        gradQuad(380, 90, 620, 110, 450, ramp);
    }
    // a bright box at the right edge (the glow's clamp at the screen edge)
    quad(624, 180, kW, 240, 600, 255, 140, 200);
}
}  // namespace

extern "C" void hg_init(int threshold) {
    sThreshold = threshold;
    hx_fifo_init();
    static u8 fifoDummy[64];
    GXInit(fifoDummy, sizeof fifoDummy);
    sMode = GXNtsc480IntDf;
    sMode.fbWidth = kW;
    sMode.efbHeight = kH;
    sMode.xfbHeight = kH;
    *reinterpret_cast< GXRenderModeObj** >(sVideo + sizeof(void*)) = &sMode;
    GXSetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);  // (the game's EFB format in play)
    GXSetCopyFilter(GX_FALSE, sMode.sample_pattern, GX_FALSE, sMode.vfilter);
    GXSetDispCopySrc(0, 0, kW, kH);
    GXSetDispCopyDst(kW, kH);
    sReadback = static_cast< u8* >(hx_guest_alloc(GXGetTexBufferSize(kW, kH, GX_TF_RGBA8, GX_FALSE, 1)));
    sXfb = static_cast< u8* >(hx_guest_alloc(kW * kH * 2));

    void* mem = std::calloc(1, sizeof(BloomEffect) + 64);
    sBloom = static_cast< BloomEffect* >(mem);
    sBloom->_C = true;
    sBloom->_D = true;
    sBloom->_10 = 1.0f;
    sBloom->_14 = 255;
    sBloom->_18 = 42 / 255.0f;
    sBloom->_1C = 21 / 255.0f;
    sBloom->_20 = static_cast< u8 >(sThreshold);
    sBloom->_24 = makeTexture(1.0f, GX_TF_RGBA8);
    sBloom->_28 = makeTexture(0.5f, GX_TF_RGB565);
    sBloom->_2C = makeTexture(0.25f, GX_TF_RGB565);
    sBloom->_30 = makeTexture(0.25f, GX_TF_RGB565);
    sBloom->_34 = makeTexture(0.125f, GX_TF_RGB565);
    sBloom->_38 = makeTexture(0.125f, GX_TF_RGB565);
    sBloom->_3C = sBloom->_2C;
    sBloom->_40 = makeTexture(0.25f, GX_TF_I8);
    sBloom->_44 = makeTexture(0.25f, GX_TF_RGB565);
    sBloom->_50 = makeTexture(0.0625f, GX_TF_RGB565);  // (GXV_BLOOM_FOLD=2's glow, ImageEffectResource::_24)
    sBloom->_48 = static_cast< Mtx* >(hx_guest_alloc(sizeof(Mtx) * 6 * 2));
    sBloom->_4C = static_cast< Mtx* >(hx_guest_alloc(sizeof(Mtx) * 12 * 3));
    // the simple bloom on the same ImageEffectResource textures (BloomEffectSimple's constructor), SimpleBloomArea's defaults
    void* smem = std::calloc(1, sizeof(BloomEffectSimple) + 64);
    sSimple = static_cast< BloomEffectSimple* >(smem);
    sSimple->_C = true;
    sSimple->_D = true;
    sSimple->_10 = 1.0f;
    sSimple->_14 = sBloom->_24;  // (resource _0)
    sSimple->_18 = sBloom->_28;  // (_4)
    sSimple->_1C = sBloom->_2C;  // (_C)
    sSimple->_20 = sBloom->_30;  // (_10)
    sSimple->_24 = sBloom->_34;  // (_14)
    sSimple->_28 = sBloom->_38;  // (_18)
    sSimple->_2C = sBloom->_40;  // (_1C)
    sSimple->mMaskFilter = std::getenv("BF_MASK") ? std::atoi(std::getenv("BF_MASK")) : 0;
    sSimple->mThreshold = static_cast< u8 >(sThreshold);
    sSimple->mFoldTex = sBloom->_50;
    sSimple->mIntensity = 76 / 255.0f;
    static const f32 l1[2][2] = {{0.01f, 0.0f}, {0.02f, 0.52f}};
    static const f32 l2[3][2] = {{0.04f, 0.0f}, {0.07f, 0.0f}, {0.09f, 0.0f}};
    for (int i = 0; i < 2; i++) {
        sBloom->initBlurMtx(&sBloom->_48[i * 6], 6, l1[i][0], l1[i][1]);
    }
    for (int i = 0; i < 3; i++) {
        sBloom->initBlurMtx(&sBloom->_4C[i * 12], 12, l2[i][0], l2[i][1]);
    }
}

// One picture. glow: 0 = no glow source drawn (PETARI_BLOOM_SKIP's case), 1 = the sources above.
extern "C" void hg_frame(int n, int glow) {
    ImageEffectLocalUtil::portPictureTick();  // (GameSystem::draw: a new picture)
    GXSetViewport(0.0f, 0.0f, kW, kH, 0.0f, 1.0f);
    GXSetScissor(0, 0, kW, kH);
    drawScene(n);
    MR::reinitGX();
    hx_fifo_flush();
    hx_mark(0);
    if (std::getenv("BF_SIMPLE")) {
        if (glow) {  // (the simple bloom has no glow sources of its own: it blooms the scene; "glow" here draws them into the scene)
            drawGlow(n);
            MR::reinitGX();
        }
        sSimple->BloomEffectSimple::draw();  // (not through the vtable: the object was not constructed)
    } else {
        sBloom->preDraw();
        if (glow) {
            drawGlow(n);
        }
        MR::reinitGX();
        sBloom->postDraw();
    }
    hx_fifo_flush();
    hx_mark(1);
    // the picture read back: an RGBA8 copy of the whole EFB, then the display copy (with the clear the next picture starts from)
    GXSetCopyFilter(GX_FALSE, sMode.sample_pattern, GX_FALSE, sMode.vfilter);
    if (!std::getenv("BF_NOREAD")) {
        GXSetTexCopySrc(0, 0, kW, kH);
        GXSetTexCopyDst(kW, kH, GX_TF_RGBA8, GX_FALSE);
        GXCopyTex(sReadback, GX_FALSE);
        GXPixModeSync();
    }
    GXSetCopyClear(GXColor{0, 0, 0, 255}, 0xFFFFFF);
    GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    GXCopyDisp(sXfb, GX_TRUE);
    hx_fifo_flush();
}
