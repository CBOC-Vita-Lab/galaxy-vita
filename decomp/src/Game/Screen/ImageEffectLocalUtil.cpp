#include "Game/Screen/ImageEffectLocalUtil.hpp"
#include "Game/Map/WaterAreaHolder.hpp"
#include "Game/NameObj/NameObjAdaptor.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Screen/BloomEffect.hpp"
#include "Game/Util/Color.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include <JSystem/JUtility/JUTTexture.hpp>
#ifdef TARGET_PC
#include "Game/Camera/CameraDirector.hpp"
#include "Game/Util/CameraUtil.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "port/stage_knob.h"
#include "port/glow26.h"
#include "port/compat.h"
#include <revolution/mtx.h>

namespace {
// (stagepreset25, port/stage_knob.h) The modes below are read once into a static; after a stage's preset is committed (the generation changed) each
// is read again, from the stage's value when it has one, else from the environment as before.
inline bool StageStale(u32& rSeen) {
    const u32 g = port::stageknob::Gen();
    if (rSeen == g) {
        return false;
    }
    rSeen = g;
    return true;
}
inline const char* StageEnv(const char* pName, char* pBuf, size_t size) {
    long v;
    if (port::stageknob::Overridden(pName, &v)) {
        snprintf(pBuf, size, "%ld", v);
        return pBuf;
    }
    return getenv(pName);
}
}  // namespace
extern "C" u32 port_render_size_gen(void);
extern "C" void port_log_summary(const char* fmt, ...);
#endif

namespace {
    void setTextureTrans(f32 x, f32 y) {
        TPos3f transMtx;
        transMtx.makeTrans(x, y, 0.0f);
        GXLoadTexMtxImm(transMtx.toMtxPtr(), GX_TEXMTX0, GX_MTX2x4);
    }
};  // namespace

void MR::connectToSceneImageEffect(NameObj* pObj) {
    MR::connectToScene(pObj, MR::MovementType_ImageEffect, MR::CalcAnimType_Environment, MR::DrawBufferType_None, MR::DrawType_ImageEffect);
}

void MR::connectToSceneImageEffectMovement(NameObj* pObj) {
    MR::connectToScene(pObj, MR::MovementType_ImageEffect, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_None);
}

void ImageEffectLocalUtil::capture(JUTTexture* pTexture, s32 divisions, s32 tile, GXTexFmt format, bool clear, u8 filter) {
#ifdef TARGET_PC
    if (ImageEffectLocalUtil::portClipDry()) {  // (PETARI_CLIPCHAIN_SKIPEMPTY: state only)
        return;
    }
    // (11 Nov: BloomEffectSimple can come here with divisions == 0 (Matter Splatter's start, Melty Molten and Flipswitch later in play).
    // The console's ARM division returns 0 there, so the capture is taken at the EFB's origin; a computer build trapped. Same result on both.)
    if (divisions <= 0) {
        pTexture->capture(0, 0, format, clear, filter);
        return;
    }
#endif
    pTexture->capture((tile % divisions) * MR::getFrameBufferWidth() / divisions, (tile / divisions) * MR::getFrameBufferHeight() / divisions, format,
                      clear, filter);
}

void ImageEffectLocalUtil::setupDrawTexture() {
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
    GXSetZTexture(GX_ZT_DISABLE, GX_TF_Z24X8, 0);
    GXSetNumIndStages(0);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetTevDirect(GX_TEVSTAGE1);
    GXSetTevDirect(GX_TEVSTAGE2);
    GXSetTevDirect(GX_TEVSTAGE3);
    GXSetTevDirect(GX_TEVSTAGE4);
    GXSetTevDirect(GX_TEVSTAGE5);
    GXSetTevDirect(GX_TEVSTAGE6);
    GXSetTevDirect(GX_TEVSTAGE7);
    GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, Color8(0, 0, 0, 0));
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetZCompLoc(GX_TRUE);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetDither(GX_FALSE);
}

void ImageEffectLocalUtil::drawTexture(JUTTexture* pTexture, s32 divisions, s32 tile, u8 intensity, ETexDrawType texDrawType) {
    pTexture->load(GX_TEXMAP0);

    if (texDrawType == TexDrawType_2) {
        GXSetChanMatColor(GX_COLOR0A0, Color8(255, 255, 255, intensity));
        GXSetChanAmbColor(GX_COLOR0A0, Color8(0, 0, 0, 255));
    } else {
        GXSetChanMatColor(GX_COLOR0A0, Color8(intensity, intensity, intensity, 255));
        GXSetChanAmbColor(GX_COLOR0A0, Color8(0, 0, 0, 255));
    }

    switch (texDrawType) {
    case TexDrawType_0:
        GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
        break;
    case TexDrawType_1:
        GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
        break;
    case TexDrawType_2:
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        break;
    case TexDrawType_3:
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCCLR, GX_BL_ZERO, GX_LO_CLEAR);
        break;
    }

    sendTextureVertex(divisions, tile);
}

void ImageEffectLocalUtil::sendTextureVertex(s32 divisions, s32 tile) {
#ifdef TARGET_PC
    if (ImageEffectLocalUtil::portClipDry()) {  // (PETARI_CLIPCHAIN_SKIPEMPTY: state only)
        return;
    }
#endif
    s32 row = tile / divisions;
    s32 column = tile % divisions;
    f32 width = static_cast< f32 >(MR::getFrameBufferWidth()) / divisions;
    f32 height = static_cast< f32 >(MR::getFrameBufferHeight()) / divisions;
    f32 left = column * width;
    f32 right = (column + 1) * width;
    f32 bottom = (row + 1) * height;
    f32 top = row * height;

    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
    GXPosition3f32(left, bottom, 0.0f);
    GXTexCoord2f32(0.0f, 1.0f);

    GXPosition3f32(left, top, 0.0f);
    GXTexCoord2f32(0.0f, 0.0f);

    GXPosition3f32(right, bottom, 0.0f);
    GXTexCoord2f32(1.0f, 1.0f);

    GXPosition3f32(right, top, 0.0f);
    GXTexCoord2f32(1.0f, 0.0f);
    GXEnd();
}

#pragma push
#pragma global_optimizer off
void ImageEffectLocalUtil::blurTexture(JUTTexture* pTexture, s32 divisions, s32 tile, u32 sampleCount, f32 radius, f32 intensity) {
    f32 aspect = MR::isScreen16Per9() ? 1.333f : 1.0f;
    f32 verticalRadius = radius * aspect;
    s32 sampleIntensity = 255.0f * intensity / sampleCount;

    for (u32 i = 0; i < sampleCount; i++) {
        f32 angle = 2.0f * (i * JGeometry::TUtil< f32 >::PI()) / sampleCount;
        ::setTextureTrans(radius * JMACosRadian(angle), verticalRadius * JMASinRadian(angle));
        ETexDrawType type;
        if (i == 0) {
            type = TexDrawType_0;
        } else {
            type = TexDrawType_1;
        }

        drawTexture(pTexture, divisions, tile, sampleIntensity, type);
    }

    ::setTextureTrans(0.0f, 0.0f);
}
#pragma pop

void MR::connectToSceneNormalBloom(BloomEffect* pBloomEffect) {
    MR::connectToScene(pBloomEffect, MR::MovementType_None, MR::CalcAnimType_Environment, MR::DrawBufferType_None, MR::DrawType_None);

    NameObjAdaptor* pPreDrawAdaptor = new NameObjAdaptor("BloomEffect::preDraw");

    pPreDrawAdaptor->connectToDraw(MR::Functor(pBloomEffect, &BloomEffect::preDraw));
    MR::connectToScene(pPreDrawAdaptor, MR::MovementType_None, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_BloomEffectPreDraw);

    NameObjAdaptor* pPostDrawAdaptor = new NameObjAdaptor("BloomEffect::postDraw");

    pPostDrawAdaptor->connectToDraw(MR::Functor(pBloomEffect, &BloomEffect::postDraw));
    MR::connectToScene(pPostDrawAdaptor, MR::MovementType_None, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_BloomEffectPostDraw);
}

#ifdef TARGET_PC
// ---- PETARI_BLOOM_HALFRATE (gpu23): the bloom's glow made every other picture (a MINOR CHANGE: the glow lags one picture)
// BloomEffectSimple::draw and BloomEffect::preDraw/postDraw make the glow from this picture's EFB through a chain of captures and
// small passes (each a GXM scene on the console: 6-9 scenes and ~2.5-4 ms of GPU a picture at 960x544, Beach Bowl's census), then
// add it to the picture. On the second of two pictures the chain is left out and the glow the first one made (BloomEffectSimple's
// _28, BloomEffect's _3C: render textures that keep their picture) is added again, with this picture's fade; BloomEffect then
// also leaves out its scene capture, its black fill and its glow models. The glow (not the scene) is one picture late on those
// pictures. The chain is always made: on the first picture an effect draws, after a picture without it, after a change of the
// render size (port_render_size_gen: the textures were resampled), while the camera turns hard (CameraDirector::isRotatingHard),
// and when two effects draw in one picture (a cross-fade).
//   PETARI_BLOOM_HALFRATE=0 (default) off: every picture makes its glow (the game as it was); 1 on in every mode; 2 on only in
//   Performance mode (GXV_AUTOQ_TARGET=60), the intended default once the console A/B is accepted.
// Log: "bloom-halfrate: N pictures with a bloom, R added the previous glow" (basic log, every 3600 pictures with a bloom).
namespace {
    u32 sBloomPic = 0, sBloomChainPic = 0, sBloomSizeGen = 0, sBloomDecidedPic = 0;
    const void* sBloomOwner = nullptr;
    const void* sBloomDecidedOwner = nullptr;
    bool sBloomDecision = false;
    u32 sBloomPictures = 0, sBloomReused = 0;
    int sBloomMode = -1;

    bool bloomHalfRateOn() {
        static u32 sSeen = 0;
        if (StageStale(sSeen)) {
            sBloomMode = -1;
        }
        if (sBloomMode < 0) {
            char sb[16];
            const char* e = StageEnv("PETARI_BLOOM_HALFRATE", sb, sizeof sb);
            int m = e && *e ? atoi(e) : 0;
            if (m == 2) {
                const char* t = getenv("GXV_AUTOQ_TARGET");
                m = t && atoi(t) == 60 ? 1 : 0;
            }
            sBloomMode = m == 1 ? 1 : 0;
            port_log_summary("bloom-halfrate: PETARI_BLOOM_HALFRATE=%s: %s", e && *e ? e : "0",
                             sBloomMode ? "the bloom's glow is made every other picture (minor change)" : "off");
        }
        return sBloomMode != 0;
    }
}  // namespace

void ImageEffectLocalUtil::portPictureTick() {
    ++sBloomPic;  // (every picture the game draws, the pause's frozen ones and a movie's included: "the previous picture" is exact)
}

namespace {
    bool sRehearsing = false;  // (dive27)
}
bool ImageEffectLocalUtil::portRehearsing() {
    return sRehearsing;
}
void ImageEffectLocalUtil::portRehearseSet(bool on) {
    sRehearsing = on;
}

// GXV_HRW_CAPFIX (vgx_galaxy.cpp): with 60 Assist's pairs the pictures this file counts (portPictureTick: drawn turns) are 33.3 ms apart, so a "reuse the previous picture's" rule would show
// something 33 ms old on one picture and 67 ms on the next. While a pair is open (this drawn turn or the last) the three half-rate rules below make their work for real. Weak: tests without galaxy.
extern "C" int port_hrw_capfix_pairing(void) __attribute__((weak));
namespace {
    u32 sHrwFresh[3] = {0, 0, 0};  // bloom, shadow texture, clip chain: reuses refused because of the pairing (since the last log line)
    bool hrwPairing() {
        return port_hrw_capfix_pairing && port_hrw_capfix_pairing() != 0;
    }
}  // namespace

bool ImageEffectLocalUtil::portBloomReuse(const void* pOwner) {
    if (sRehearsing) {
        return false;  // (dive27: the rehearsal makes the full chain and leaves the reuse history alone)
    }
    (void)portBloomFastMode();  // (GXV_BLOOM_FAST: its start line with the first bloom picture of either kind)
    if (sBloomDecidedPic == sBloomPic && sBloomDecidedOwner == pOwner) {
        return sBloomDecision;  // (preDraw and postDraw of one picture)
    }
    const u32 sizeGen = port_render_size_gen();
    bool reuse = false;
    if (bloomHalfRateOn() && pOwner == sBloomOwner && sBloomChainPic + 1 == sBloomPic && sizeGen == sBloomSizeGen) {
        reuse = !MR::getCameraDirector()->isRotatingHard();
        if (reuse && hrwPairing()) {
            reuse = false;  // (GXV_HRW_CAPFIX)
            ++sHrwFresh[0];
        }
    }
    if (!reuse) {
        sBloomOwner = pOwner;
        sBloomChainPic = sBloomPic;
        sBloomSizeGen = sizeGen;
    }
    sBloomDecidedPic = sBloomPic;
    sBloomDecidedOwner = pOwner;
    sBloomDecision = reuse;
    if (sBloomMode > 0) {
        ++sBloomPictures;
        sBloomReused += reuse ? 1 : 0;
        if (sBloomPictures >= 3600) {
            port_log_summary("bloom-halfrate: %u pictures with a bloom, %u added the previous glow (PETARI_BLOOM_HALFRATE), %u made fresh because HRW is pairing (GXV_HRW_CAPFIX)", (unsigned)sBloomPictures,
                             (unsigned)sBloomReused, (unsigned)sHrwFresh[0]);
            sBloomPictures = sBloomReused = 0;
            sHrwFresh[0] = 0;
        }
    }
    return reuse;
}

bool ImageEffectLocalUtil::portBloomReusingNow() {
    return sBloomDecidedPic == sBloomPic && sBloomDecision;
}

void ImageEffectLocalUtil::portBloomTexturesTouched() {
    sBloomOwner = nullptr;  // (the depth of field and the screen blur draw into ImageEffectResource's textures the glow is kept in)
}

u32 ImageEffectLocalUtil::portPictureSerial() {
    return sBloomPic;
}

// ---- PETARI_SHADOWTEX_HALFRATE (gpu23): the player's shadow texture drawn and captured every other picture (a MINOR CHANGE: the
// shape of Mario's projected shadow lags one picture on those pictures)
// CollisionShadow::drawAndCaptureTex draws Mario's simple model into the EFB's corner (608 - w, 0, w x h; 160 x 160 in play) and
// captures its alpha (GX_CTF_A8) for the shadow drawn right after. On the console that capture splits the EFB's scene every
// picture (EFB stored, a copy scene, EFB loaded: one GXM scene's fixed cost, ~0.5-0.9 ms of TA, plus the EFB's colour and depth
// traffic; PASSHOIST cannot take it: 160 EFB rows are not whole render rows at 404 or 544). On the second of two pictures the
// draw and the capture are left out and the texture keeps the previous picture's shadow; the corner's alpha is still cleared
// (clearAlphaBuffer, as always), so the EFB is as the full path leaves it. Always drawn: the first picture after one without it,
// after a change of the render size (the texture was resampled), and for any other CollisionShadow than the one drawn last.
//   PETARI_SHADOWTEX_HALFRATE=0 (default) off; 1 on in every mode; 2 on only in Performance mode (GXV_AUTOQ_TARGET=60).
// Log: "shadowtex-halfrate: N captures asked, R kept the previous picture's" (basic log, every 3600 asked).
namespace {
    u32 sShadowCapPic = 0xFFFFFFFFu, sShadowSizeGen = 0, sShadowAsked = 0, sShadowReused = 0;
    const void* sShadowOwner = nullptr;
    int sShadowMode = -1;
}  // namespace

bool ImageEffectLocalUtil::portShadowTexReuse(const void* pOwner) {
    static u32 sSeen = 0;
    if (StageStale(sSeen)) {
        sShadowMode = -1;
    }
    if (sShadowMode < 0) {
        char sb[16];
        const char* e = StageEnv("PETARI_SHADOWTEX_HALFRATE", sb, sizeof sb);
        int m = e && *e ? atoi(e) : 0;
        if (m == 2) {
            const char* t = getenv("GXV_AUTOQ_TARGET");
            m = t && atoi(t) == 60 ? 1 : 0;
        }
        sShadowMode = m == 1 ? 1 : 0;
        port_log_summary("shadowtex-halfrate: PETARI_SHADOWTEX_HALFRATE=%s: %s", e && *e ? e : "0",
                         sShadowMode ? "the player's shadow texture is drawn every other picture (minor change)" : "off");
    }
    if (!sShadowMode) {
        return false;
    }
    const u32 pic = sBloomPic;  // (the picture being drawn: portPictureTick ran before the scene)
    const u32 sizeGen = port_render_size_gen();
    bool reuse = pOwner == sShadowOwner && sShadowCapPic + 1 == pic && sizeGen == sShadowSizeGen;
    if (reuse && hrwPairing()) {
        reuse = false;  // (GXV_HRW_CAPFIX)
        ++sHrwFresh[1];
    }
    if (!reuse) {
        sShadowOwner = pOwner;
        sShadowCapPic = pic;
        sShadowSizeGen = sizeGen;
    }
    ++sShadowAsked;
    sShadowReused += reuse ? 1 : 0;
    if (sShadowAsked >= 3600) {
        port_log_summary("shadowtex-halfrate: %u captures asked, %u kept the previous picture's (PETARI_SHADOWTEX_HALFRATE), %u made fresh because HRW is pairing (GXV_HRW_CAPFIX)", (unsigned)sShadowAsked,
                         (unsigned)sShadowReused, (unsigned)sHrwFresh[1]);
        sShadowAsked = sShadowReused = 0;
        sHrwFresh[1] = 0;
    }
    return reuse;
}
// ---- PETARI_CLIPCHAIN_HALFRATE (heavyopt25, class U: unnoticeable): the clip field's soft edge made every other picture while the camera is still
// FallOutFieldDraw::draw (the dark-matter / fall-out "clip area" of Bowser's Dark Matter Plant and other stages) makes, every picture it is active:
// capture 2 (A8, whole EFB: the mask; handed over, no scene), then draws that mask into the EFB's quarter tile, captures it (3: a copy scene and an EFB
// split), blurs it with 5 passes into the sixteenth tile, captures that (4: a second copy scene and split), and draws capture 4 over the whole screen as the
// field's SOFT EDGE (blend by its alpha). The fill itself uses capture 2 of THIS picture. On the second of two pictures the quarter draw, capture 3, the blur and
// capture 4 are left out and the edge is drawn from the texture the previous picture made: the edge's soft band (a 1/16-scale blur, tens of pixels wide) is one
// picture late; the hard boundary (capture 2) is current. About 2 copy scenes + 2 EFB splits + 6 small draws a picture: est. -2 to -2.8 ms of 3D and -2.5 of TA
// at 960x544 in a stage where the clip area is on screen (Bowser's Dark Matter Plant: 12 scenes a picture). Always made: first picture, after a picture without,
// after a render-size change, another owner, a camera cut (port_camera_cut_count), the camera turning hard, or the camera having moved more than
// PETARI_CLIPCHAIN_CAMTOL game units (default 8, 480 units/s at 60) or turned more than PETARI_CLIPCHAIN_DIRTOL thousandths of a radian (default 4) since
// the picture the texture was made in. 0 (default) off; 1 on in every mode; 2 Performance mode only. Log: "clipchain-halfrate: N asked, R reused, B blocked by the camera".
extern "C" uint32_t port_camera_cut_count(void) __attribute__((weak));
namespace {
    int sClipMode = -1;
    u32 sClipMadePic = 0xFFFFFFFFu, sClipSizeGen = 0, sClipAsked = 0, sClipReused = 0, sClipBlocked = 0, sClipCuts = 0;
    const void* sClipOwner = nullptr;
    TVec3f sClipPos(0.0f, 0.0f, 0.0f), sClipZ(0.0f, 0.0f, 1.0f);
    f32 sClipTol = 8.0f, sClipDir = 0.004f;
}  // namespace

bool ImageEffectLocalUtil::portClipChainReuse(const void* pOwner) {
    static u32 sSeen = 0;
    if (StageStale(sSeen)) {
        sClipMode = -1;
    }
    if (sClipMode < 0) {
        char sb[16];
        const char* e = StageEnv("PETARI_CLIPCHAIN_HALFRATE", sb, sizeof sb);
        int m = e && *e ? atoi(e) : 0;
        if (m == 2) {
            const char* t = getenv("GXV_AUTOQ_TARGET");
            m = t && atoi(t) == 60 ? 1 : 0;
        }
        sClipMode = m == 1 ? 1 : 0;
        if (const char* v = getenv("PETARI_CLIPCHAIN_CAMTOL")) sClipTol = static_cast<f32>(atof(v));
        if (const char* v = getenv("PETARI_CLIPCHAIN_DIRTOL")) sClipDir = static_cast<f32>(atof(v)) * 0.001f;
        port_log_summary("clipchain-halfrate: PETARI_CLIPCHAIN_HALFRATE=%s: %s (camera tolerance %.1f units, %.4f rad)", e && *e ? e : "0",
                         sClipMode ? "the clip field's soft-edge chain is made every other picture while the camera is still (unnoticeable)" : "off", (double)sClipTol, (double)sClipDir);
    }
    if (!sClipMode) {
        return false;
    }
    const u32 pic = sBloomPic;
    const u32 sizeGen = port_render_size_gen();
    const u32 cuts = port_camera_cut_count ? port_camera_cut_count() : 0;
    const TVec3f pos = MR::getCamPos();
    const TVec3f z = MR::getCamZdir();
    bool reuse = pOwner == sClipOwner && sClipMadePic + 1 == pic && sizeGen == sClipSizeGen && cuts == sClipCuts;
    bool blocked = false;
    if (reuse) {
        const f32 dx = pos.x - sClipPos.x, dy = pos.y - sClipPos.y, dz = pos.z - sClipPos.z;
        const f32 dd = dx * dx + dy * dy + dz * dz;
        const f32 dot = z.x * sClipZ.x + z.y * sClipZ.y + z.z * sClipZ.z;
        const f32 ang2 = 2.0f * (1.0f - dot);  // (the angle squared for small turns)
        if (dd > sClipTol * sClipTol || ang2 > sClipDir * sClipDir || MR::getCameraDirector()->isRotatingHard()) {
            reuse = false;
            blocked = true;
        }
    }
    if (reuse && hrwPairing()) {
        reuse = false;  // (GXV_HRW_CAPFIX)
        ++sHrwFresh[2];
    }
    if (!reuse) {  // (made now: its camera is the reference)
        sClipOwner = pOwner;
        sClipMadePic = pic;
        sClipSizeGen = sizeGen;
        sClipCuts = cuts;
        sClipPos = pos;
        sClipZ = z;
    }
    ++sClipAsked;
    sClipReused += reuse ? 1 : 0;
    sClipBlocked += blocked ? 1 : 0;
    if (sClipAsked >= 3600) {
        port_log_summary("clipchain-halfrate: %u asked, %u reused the previous picture's edge, %u blocked by the camera (PETARI_CLIPCHAIN_HALFRATE), %u made fresh because HRW is pairing (GXV_HRW_CAPFIX)", (unsigned)sClipAsked,
                         (unsigned)sClipReused, (unsigned)sClipBlocked, (unsigned)sHrwFresh[2]);
        sClipAsked = sClipReused = sClipBlocked = 0;
        sHrwFresh[2] = 0;
    }
    return reuse;
}
// ---- PETARI_CLIPCHAIN_ONEPASS (clipblur25, class U: a player would barely notice): the clip field's soft edge from tex2 in ONE draw, not a chain of captures
// What FallOutFieldDraw::draw sends a picture (confirmed by reading the code and the renderer, see agents/clipblur25/REPORT.txt): the mask (tex2, A8, the whole EFB
// box-halved: 320x228), then the mask drawn 1:1 through a compare into the EFB's top-left quarter (alpha only: 1 GXM scene), a capture of that quarter box-halved
// into tex3 (160x114: a copy scene + an EFB split), the 5 blur taps of tex3 (five draws of the SAME source, additive, in one tile: ONE scene, not five), a
// capture of that tile into tex4 (a second copy scene + split), and the full-screen edge draw of tex4 (bilinear 4x). The five blur taps are already one pass
// (a ring of radius 0.004 texture units = 0.64 of a 160x114 texel, saturating sum); what costs the scenes is the two captures between the stages.
//   ONEPASS=1  (zero extra scenes): the edge draw reads tex2 itself, K times (TEV: K texture coordinates = K translated copies of one coordinate, K compare-
//              accumulate stages: alpha = clamp(gain / K * number of taps whose tex2 texel says "outside"), then * the edge colour's alpha as before). The quarter draw,
//              both captures and the blur are left out: 4 GXM scenes (2 EFB splits, 2 copy scenes) fewer a picture. K = PETARI_CLIPCHAIN_TAPS (4..8, default 6).
//  ONEPASS=2  (one capture): the K taps are drawn once into the same tile the blur used (alpha only, no blend), captured into tex4 as before, and the edge is
//              drawn from tex4 as before: the final stage (bilinear 4x of a 160x114 tile) is the game's own; 2 scenes (1 EFB split, 1 copy scene) fewer.
// Every tap is a compare of the texture's alpha with 1/255 (the compare the quarter draw and the fill make), so the soft edge lies where the hard edge (the fill) is.
// PETARI_CLIPCHAIN_RADIUS (EFB pixels), PETARI_CLIPCHAIN_ROT (radians), PETARI_CLIPCHAIN_GAIN: the ring (default: the fitted one for the mode and K) and the saturation (the five-tap chain adds five taps of a smooth
// texture and clamps; the one-pass kernel adds K thresholded taps and clamps at gain / K * count). Composes with PETARI_CLIPCHAIN_HALFRATE in mode 2 (the
// reuse skips the tile draw and the capture too) and replaces it in mode 1 (nothing left to reuse).
// PETARI_CLIPCHAIN_SKIPEMPTY=1 (exact): with no ClipArea volume drawn this picture the mask is all zero: with the "inside" sense (_C) the chain, the edge and the
// fill draw nothing (the edge only writes the alpha plane's 0, which the cleared EFB holds): their GX state calls are made, no vertex is sent, no capture is taken.
// Log: "clipchain-onepass: ..." at the first picture, then "N pictures, ..." every 3600.
extern "C" int64_t port_host_time_ns(void);
namespace {
    int sOnePass = -1, sSkipEmptyMode = -1;
    int sTapK = 6;
    f32 sTapRadius = 6.0f, sTapGain = 2.0f, sTapRot = 0.5235988f;  // (set from the fitted table below)
    bool sDry = false;
    u32 sVolumes = 0, sOpPictures = 0, sOpSkipped = 0, sOpEmptyRun = 0;
    int64_t sOpWindowNs = 0;
    u32 sOpTapDraws = 0, sOpModePictures = 0;  // (strikers2audit: the one-pass tap setups actually made, and the pictures the mode was asked for)

    void clipOnePassInit() {
        static u32 sSeen = 0;
        if (StageStale(sSeen)) {
            sOnePass = -1;
        }
        if (sOnePass >= 0) {
            return;
        }
        char sb[16], sc[16];
        const char* e = StageEnv("PETARI_CLIPCHAIN_ONEPASS", sb, sizeof sb);
        int m = e && *e ? atoi(e) : 0;
        sOnePass = m == 1 || m == 2 ? m : 0;
        if (const char* v = getenv("PETARI_CLIPCHAIN_TAPS")) {
            const int k = atoi(v);
            sTapK = k < 4 ? 4 : (k > 8 ? 8 : k);
        }
        // (the fitted ring per mode and tap count: radius in EFB pixels, rotation in radians, gain: tools/clipblur_model.py, minimising the mean difference of the
        // soft edge's alpha to the five-tap chain's on hard-edged shapes at several grid phases; K = 7 is interpolated, not fitted)
        static const f32 ring1[5][3] = {{6.00f, 0.785f, 2.00f}, {5.54f, 0.028f, 2.37f}, {5.88f, 0.729f, 2.09f}, {5.70f, 0.800f, 2.20f}, {5.55f, 0.895f, 2.27f}};
        static const f32 ring2[5][3] = {{5.92f, 0.884f, 2.05f}, {5.07f, 0.836f, 2.50f}, {4.97f, 0.815f, 2.83f}, {5.00f, 0.900f, 2.70f}, {5.04f, 0.965f, 2.53f}};
        const f32* r = (sOnePass == 2 ? ring2 : ring1)[sTapK - 4];
        sTapRadius = r[0];
        sTapRot = r[1];
        sTapGain = r[2];
        if (const char* v = getenv("PETARI_CLIPCHAIN_ROT")) sTapRot = static_cast<f32>(atof(v));
        if (const char* v = getenv("PETARI_CLIPCHAIN_RADIUS")) sTapRadius = static_cast<f32>(atof(v));
        if (const char* v = getenv("PETARI_CLIPCHAIN_GAIN")) sTapGain = static_cast<f32>(atof(v));
        const char* s = StageEnv("PETARI_CLIPCHAIN_SKIPEMPTY", sc, sizeof sc);
        sSkipEmptyMode = s && *s && atoi(s) != 0 ? 1 : 0;
        if (sOnePass == 0 && sSkipEmptyMode == 0) {
            return;  // (the default: no log line, no census)
        }
        port_log_summary("clipchain-onepass: PETARI_CLIPCHAIN_ONEPASS=%s: %s (taps %d, radius %.2f EFB px, gain %.2f); PETARI_CLIPCHAIN_SKIPEMPTY=%d", e && *e ? e : "0",
                         sOnePass == 1 ? "the soft edge from the mask in one draw (4 scenes fewer a picture)"
                         : sOnePass == 2 ? "the soft edge in one tile draw and one capture (2 scenes fewer a picture)"
                                         : "off",
                         sTapK, (double)sTapRadius, (double)sTapGain, sSkipEmptyMode);
    }
}  // namespace

int ImageEffectLocalUtil::portClipOnePassMode() {
    clipOnePassInit();
    return sOnePass;
}

void ImageEffectLocalUtil::portClipVolumeDrawn() {
    ++sVolumes;
}

bool ImageEffectLocalUtil::portClipSkipEmpty(bool inside) {
    clipOnePassInit();
    const bool empty = sVolumes == 0;
    sVolumes = 0;  // (the next picture's ClipAreas draw before its FallOutFieldDraw)
    return sSkipEmptyMode != 0 && empty && inside;
}

void ImageEffectLocalUtil::portClipSetDry(bool dry) {
    sDry = dry;
}

bool ImageEffectLocalUtil::portClipDry() {
    return sDry;
}

void ImageEffectLocalUtil::portClipNote(bool onePass, bool skipped) {
    if (sOnePass == 0 && sSkipEmptyMode == 0) {
        return;
    }
    ++sOpPictures;
    sOpSkipped += skipped ? 1 : 0;
    sOpModePictures += onePass ? 1 : 0;
    const int64_t opNow = port_host_time_ns();
    if (sOpWindowNs == 0) sOpWindowNs = opNow;
    if (sOpPictures >= 3600 || opNow - sOpWindowNs >= 60000000000LL) {  // (3600 clip pictures, or a minute: the line is there in a stage with few)
        // (strikers2audit: "passes it actually took": the one-pass taps drawn are counted where they are set up, portClipTapsSetup; a mode that is on with 0 here
        // does nothing, as the alpha-only rule of the Strikers port's half-scale option did)
        port_log_summary("clipchain-onepass: %u clip-field pictures, %u left out whole (no volume drawn: PETARI_CLIPCHAIN_SKIPEMPTY), mode %d asked on %u pictures, "
                         "%u one-pass tap draws made (%s)",
                         (unsigned)sOpPictures, (unsigned)sOpSkipped, sOnePass, (unsigned)sOpModePictures, (unsigned)sOpTapDraws,
                         sOnePass != 0 && sOpTapDraws == 0 && sOpModePictures != sOpSkipped ? "THE SWITCH DID NOTHING" : "ok");
        sOpPictures = sOpSkipped = sOpTapDraws = sOpModePictures = 0;
        sOpWindowNs = opNow;
    }
}

// K compare-accumulate stages over one texture (see above). extraStages: stages the caller adds after them (the edge's final multiply). Returns K.
// GX state after it: K texture coordinates (TEXMTX0 + 3 k: a translation by the tap's offset), stage k: alpha = (previous alpha) + (ref > tex ? reg0.a : 0)
// (for the "inside" sense: tex > ref), every stage's colour 0, reg0.a = round(255 gain / K). The caller draws, then calls portClipTapsEnd.
int ImageEffectLocalUtil::portClipTapsSetup(JUTTexture* pTexture, bool inside, int extraStages) {
    clipOnePassInit();
    ++sOpTapDraws;
    const int K = sTapK;
    const f32 w = static_cast< f32 >(MR::getFrameBufferWidth());
    const f32 h = static_cast< f32 >(MR::getFrameBufferHeight());
    pTexture->load(GX_TEXMAP0);
    GXSetNumTexGens(K);
    GXSetNumTevStages(K + extraStages);
    s32 weight = static_cast< s32 >(255.0f * sTapGain / K + 0.5f);
    weight = weight < 0 ? 0 : (weight > 255 ? 255 : weight);
    GXSetTevColor(GX_TEVREG0, Color8(255, 255, 255, static_cast< u8 >(weight)));
    GXSetTevKColor(GX_KCOLOR0, Color8(0, 0, 0, 1));
    for (int k = 0; k < K; k++) {
        const f32 angle = sTapRot + 2.0f * 3.14159265f * k / K;
        TPos3f transMtx;
        transMtx.makeTrans(sTapRadius * JMACosRadian(angle) / w, sTapRadius * JMASinRadian(angle) / h, 0.0f);
        GXLoadTexMtxImm(transMtx.toMtxPtr(), static_cast< GXTexMtx >(GX_TEXMTX0 + 3 * k), GX_MTX2x4);
        GXSetTexCoordGen2(static_cast< GXTexCoordID >(GX_TEXCOORD0 + k), GX_TG_MTX2x4, GX_TG_TEX0, static_cast< u32 >(GX_TEXMTX0 + 3 * k), GX_FALSE, GX_PTIDENTITY);
        const GXTevStageID stage = static_cast< GXTevStageID >(GX_TEVSTAGE0 + k);
        GXSetTevOrder(stage, static_cast< GXTexCoordID >(GX_TEXCOORD0 + k), GX_TEXMAP0, GX_COLOR0A0);
        GXSetTevKAlphaSel(stage, GX_TEV_KASEL_K0_A);
        GXSetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
        GXSetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevAlphaIn(stage, inside ? GX_CA_TEXA : GX_CA_KONST, inside ? GX_CA_KONST : GX_CA_TEXA, GX_CA_A0, k ? GX_CA_APREV : GX_CA_ZERO);
        GXSetTevAlphaOp(stage, GX_TEV_COMP_A8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
    return K;
}

void ImageEffectLocalUtil::portClipTapsEnd() {
    GXSetNumTexGens(1);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0, GX_FALSE, GX_PTIDENTITY);
    ::setTextureTrans(0.0f, 0.0f);
    GXSetTevColor(GX_TEVREG0, Color8(255, 255, 255, 255));
}
// ---- PETARI_BLOOM_SKIP (revisit23, 8 Nov 2026): BloomEffect's glow chain left out on pictures where no glow source drew (EXACT)
// BloomEffect (the "normal" bloom, glow made per model) works like this every picture it is on: preDraw captures the scene (_24,
// RGBA8) and fills the EFB with black; the glow sources draw on the black (SceneFunction::executeDrawImageEffect: the BloomModel draw
// buffers, DrawType_BloomModel, the bloom particles, the spider thread, the sea's bloom when the camera is under water); postDraw
// makes the glow from the EFB through 8 more captures and ~40 small blur draws (each capture is a GXM scene on the console:
// gpu23's census puts 7 of Beach Bowl's 9 copies a picture in this chain), puts the scene back (_24 over the whole EFB, no blend)
// and adds the glow (_3C, ONE + ONE). Wherever none of the sources is on screen, the chain blurs black: every capture is black,
// every blur of it is black (texel x colour sums, no bias term; the threshold compare of black is 0 for any threshold), and the
// glow added is 0. The picture is then exactly the scene put back.
//   So when NOTHING that can write the EFB was sent between the black fill and postDraw (port_gx_draw_serial: GXBegin, display-list
// calls, EFB copies; GXGeometry.c), and the fill and the put-back draw cover the whole EFB (viewport and scissor checked at both), the
// chain runs "dry": every GX state call of the chain is made exactly as before (so the GX state the later draws inherit is the same),
// but no capture is taken and no vertex is sent, except the scene's put-back draw. The EFB's colour (the scene), alpha and depth
// (the chain writes neither: alpha update and Z write are off in it, and none of its captures clears) are those of the full chain.
// What differs: the chain's own textures (_28 ... _3C) keep the older picture's contents instead of black. Nothing reads them
// before the next chain writes them, except PETARI_BLOOM_HALFRATE's reuse of _3C: that reuse then adds nothing
// (portBloomGlowBlack), which is what adding a black glow does.
//   PETARI_BLOOM_SKIP=0 (default) off: the game as it was; 1 on in every mode (exact); 2 census only: nothing is left out, the
//   pictures where it would be are counted.
// Log (basic log, every 600 chains, 10-20 s): "bloom-skip: N bloom chains, E with nothing drawn on the black (left out L | =2: would be),
// F with a fill that did not cover the EFB".
namespace {
    int sSkipMode = -1;
    u32 sSkipFillSerial = 0, sSkipFillPic = 0xFFFFFFFFu;
    const void* sSkipFillOwner = nullptr;
    bool sSkipFillWhole = false;
    const void* sGlowBlackOwner = nullptr;
    u32 sSkipChains = 0, sSkipEmpty = 0, sSkipLeft = 0, sSkipPartial = 0;

    int bloomSkipMode() {
        if (sSkipMode < 0) {
            const char* e = getenv("PETARI_BLOOM_SKIP");
            const int m = e && *e ? atoi(e) : 0;
            sSkipMode = m == 1 || m == 2 ? m : 0;
            port_log_summary("bloom-skip: PETARI_BLOOM_SKIP=%s: %s", e && *e ? e : "0",
                             sSkipMode == 1   ? "the glow chain is left out on pictures where no glow source drew (exact)"
                             : sSkipMode == 2 ? "census only (nothing left out)"
                                              : "off");
        }
        return sSkipMode;
    }
}  // namespace

extern "C" u32 port_gx_draw_serial(void);

// ---- GXV_BLOOM_BRACKET (bloom37; vgx_bloombracket.h has the front end's side). 0 (default) off: nothing below is written or changed; 1 on: a dead bracket (the same test as
// PETARI_BLOOM_SKIP's: nothing sent between the black fill and postDraw, fill and put-back cover the EFB) makes the chain dry whatever PETARI_BLOOM_SKIP says, and the game
// says "dead" to the front end, which leaves the capture, the fill and the put-back out when it can show that this is an identity; 2 count only (the markers are written, nothing
// else changes: the chain is dry only by PETARI_BLOOM_SKIP's rule). Not for the simple bloom (no dry chain there).
// Log (basic log, every 300 brackets): "bloom-bracket: ..." (vgx_draw.cpp BloomBracketPass).
namespace {
    int sBracketMode = -1;
    bool sBracketDead = false;
}
int ImageEffectLocalUtil::portBloomBracketMode() {
    if (sBracketMode < 0) {
        const char* e = getenv("GXV_BLOOM_BRACKET");
        const int m = e && *e ? atoi(e) : 0;
        sBracketMode = m == 1 || m == 2 ? m : 0;
        port_log_summary("bloom-bracket: GXV_BLOOM_BRACKET=%s: %s", e && *e ? e : "0",
                         sBracketMode == 1   ? "a bloom picture where no glow source drew leaves out the scene capture, the black fill and the put-back (exact)"
                         : sBracketMode == 2 ? "census only (nothing is left out)"
                                             : "off");
    }
    return sBracketMode;
}
// GXV_TAILFOLD=3 (tailfold39): the put-back draw is tagged for the front end, whatever GXV_BLOOM_BRACKET says; nothing else of the bracket changes (the "dead" logic stays with the bracket switch).
static bool portTailFoldMarks() {
    static int sMarks = -1;
    if (sMarks < 0) {
        const char* e = getenv("GXV_TAILFOLD");
        sMarks = e && *e && atoi(e) == 3 ? 1 : 0;
    }
    return sMarks != 0;
}
void ImageEffectLocalUtil::portBloomBracketMark(unsigned sub) {
    if (sRehearsing || (portBloomBracketMode() == 0 && !portTailFoldMarks())) {
        return;
    }
    port_gx_marker(PORT_GX_MARK_BLOOM_BRACKET | (sub << 8));
}
bool ImageEffectLocalUtil::portBloomBracketDead() {
    return sBracketDead;
}

void ImageEffectLocalUtil::portBloomFillDone(const void* pOwner, bool wholeEfb) {
    if (sRehearsing || !(bloomSkipMode() || portBloomBracketMode())) {
        return;
    }
    sSkipFillSerial = port_gx_draw_serial();
    sSkipFillPic = sBloomPic;
    sSkipFillOwner = pOwner;
    sSkipFillWhole = wholeEfb;
}

bool ImageEffectLocalUtil::portBloomNothingDrawn(const void* pOwner, bool wholeEfb) {
    const int mode = bloomSkipMode();
    const int bracket = portBloomBracketMode();
    sBracketDead = false;
    if (sRehearsing || (!mode && !bracket)) {
        return false;
    }
    const bool filled = sSkipFillPic == sBloomPic && sSkipFillOwner == pOwner;  // (this picture's preDraw of this effect made the fill)
    const bool empty = filled && port_gx_draw_serial() == sSkipFillSerial;
    const bool whole = sSkipFillWhole && wholeEfb;  // (the fill reached every pixel, and the put-back draw will)
    sBracketDead = bracket != 0 && empty && whole;
    const bool dry = (mode == 1 || bracket == 1) && empty && whole;
    sSkipFillPic = 0xFFFFFFFFu;
    sGlowBlackOwner = dry ? pOwner : nullptr;  // (the glow this chain leaves in _3C: black when dry, real otherwise)
    if (!mode) {
        return dry;  // (PETARI_BLOOM_SKIP's own census stays as it was: only GXV_BLOOM_BRACKET asked)
    }
    ++sSkipChains;
    sSkipEmpty += empty ? 1 : 0;
    sSkipLeft += dry ? 1 : 0;
    sSkipPartial += empty && !whole ? 1 : 0;
    if (sSkipChains >= 600) {
        port_log_summary("bloom-skip: %u bloom chains, %u with nothing drawn on the black (%s %u), %u with a fill that did not cover the EFB",
                         (unsigned)sSkipChains, (unsigned)sSkipEmpty, mode == 2 ? "PETARI_BLOOM_SKIP=2: would be left out" : "left out",
                         (unsigned)(mode == 2 ? sSkipEmpty - sSkipPartial : sSkipLeft), (unsigned)sSkipPartial);
        sSkipChains = sSkipEmpty = sSkipLeft = sSkipPartial = 0;
    }
    return dry;
}

bool ImageEffectLocalUtil::portBloomGlowBlack(const void* pOwner) {
    return pOwner != nullptr && sGlowBlackOwner == pOwner;
}

// ---- GXV_BLOOM_FAST (bloom23, 8-9 Nov 2026): the two bloom chains with fewer GXM scenes. The chains and what changes in the picture:
// BloomEffect.cpp (the normal bloom: BloomArea areas, Beach Bowl / Sea Slide with the camera under water, the Kingfin fight) and
// BloomEffectSimple.cpp (the simple bloom of SimpleBloom areas: Beach Bowl above water, ...). Host census (tools/test_bloom_fast_gl.sh,
// the console's default switches), texture scenes + EFB splits a picture:
//                          original          GXV_BLOOM_FAST=2 (exact)          GXV_BLOOM_FAST=1 (minor change)
//   normal bloom           8 + 2             8 + 2 (as the original)          4 + 2
//   simple bloom           6 + 3             5 + 3 (its unused capture out)   5 + 1
//   GXV_BLOOM_FAST=0 (default) the original chains; 1 the fast chains in every mode; perf: 1 in Performance mode only (port_perf_mode_60),
//   else 0; 2 (or "exact"): only the exact cut (the simple bloom's capture that nothing reads).
// Log: once, at the first bloom picture of either kind, "bloom-fast: GXV_BLOOM_FAST=x: ..."; then every 600 bloom pictures, or with the
// first bloom picture after 1800 pictures (30-60 s) since the window began, "bloom-fast: N bloom pictures in P pictures (GXV_BLOOM_FAST=m):
// normal F fast / O original (...) / R reused / D dry; simple S fast / E exact / T original / U reused". GXV_GPU_SCENES=1's
// "texture/pass: N scenes" is the console's own count.
namespace {
    int sFastMode = -1;
    int sFoldMode = -1;  // (GXV_BLOOM_FOLD, portBloomFoldMode below)
    // kinds: 0 normal original, 1 normal fast, 2 normal reused (HALFRATE), 3 normal dry (BLOOM_SKIP), 4 simple original, 5 simple fast,
    // 6 simple exact, 7 simple reused, 8 normal fold 1, 9 normal fold 2, 10 simple fold 1, 11 simple fold 2 (GXV_BLOOM_FOLD)
    u32 sNotePictures = 0, sNoteFirstPic = 0, sNoteKind[12], sNoteCaptures[12], sNoteDraws[12];  // (8-11: GXV_BLOOM_FOLD)
    const char* const sModeText[3] = {
        "the original bloom chains (normal: 9 captures, 16 draws, ~8 texture scenes + 2 EFB splits a picture; simple: 6 captures, 12 draws, "
        "~6 + 3)",
        "the fast bloom chains, a minor change (normal: 5 captures, 12 draws, ~4 texture scenes + 2 EFB splits; simple: 4 captures, 11 draws, ~5 + 1)",
        "the exact cut only (the simple bloom's unused capture left out: 5 captures, ~5 + 3; the normal bloom as it was)"};
}  // namespace

extern "C" int port_perf_mode_60(void);

int ImageEffectLocalUtil::portBloomFastMode() {
    static u32 sSeen = 0;
    if (StageStale(sSeen)) {
        sFastMode = -1;
    }
    if (sFastMode < 0) {
        char sb[16];
        const char* e = StageEnv("GXV_BLOOM_FAST", sb, sizeof sb);
        int m = 0;
        if (e && (*e == 'p' || *e == 'P')) {
            m = port_perf_mode_60() ? 1 : 0;  // ("perf")
        } else if (e && (*e == 'e' || *e == 'E')) {
            m = 2;  // ("exact")
        } else if (e && *e) {
            const int v = atoi(e);
            m = v == 1 || v == 2 ? v : 0;
        }
        sFastMode = m;
        port_log_summary("bloom-fast: GXV_BLOOM_FAST=%s: %s", e && *e ? e : "0", sModeText[m]);
    }
    return sFastMode;
}

// ---- GXV_BLOOM_FOLD (bloomfold25, 11 Nov 2026): the two bloom chains folded further, and a reduced glow for the heaviest stages.
// The switch is read through the stage knobs (GXV_BLOOM_FOLD in port/stage_knob.h: PORT_STAGE_KNOB row bloom_fold) and means, for BOTH bloom kinds
// (BloomEffect.cpp = normal bloom, BloomEffectSimple.cpp = simple bloom); it overrides GXV_BLOOM_FAST while it is not 0:
//   0 (default)  off: the chain GXV_BLOOM_FAST says (the shipped default is the fast chain)
//   1            folded chain: the glow of the original blur shape from fewer GXM scenes (below, and in the two files)
//   2            reduced glow: ONE small pass of gated taps replaces the blur chain; the glow's energy and extent are matched (below)
// Never a level without glow: every level makes the glow from the same sources and adds it to the picture.
// Why a GXM scene is only saved by folding draws, not by reordering them: a pass can read only what an EARLIER scene made (a texture cannot be drawn
// to and sampled at once), so the chain's depth (copy, threshold, blur 1, blur 2, add) is the scene count. The folds below cut the depth by making a
// pass read what an earlier scene made at MORE places: the TEV evaluates a tap's threshold where the original read the thresholded texture.
// The helpers draw a tile (an EFB rectangle) from taps of one texture. Each draw holds as many taps as the TEV has stages (16) and texture
// coordinate generators (8); the draws of one pass add into the target (blend ONE / ONE, as the original ring blurs do).
namespace {
    inline GXTevKColorSel foldKc(u32 slot) {  // slot 0..15 = konst colour slot % 4, channel slot / 4 (R, G, B, A)
        return static_cast< GXTevKColorSel >(0x10 + 4 * (slot / 4) + (slot % 4));
    }
    inline GXTevKAlphaSel foldKa(u32 slot) {
        return static_cast< GXTevKAlphaSel >(0x10 + 4 * (slot / 4) + (slot % 4));
    }
    void foldQuad(bool dry, f32 left, f32 top, f32 right, f32 bottom) {
        if (dry) {
            return;  // (PETARI_BLOOM_SKIP: state only)
        }
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
    // Texture coordinate i = the quad's coordinate + (du, dv)
    void foldOffsets(u32 i, f32 du, f32 dv) {
        Mtx m;
        PSMTXTrans(m, du, dv, 0.0f);
        const GXTexMtx id = static_cast< GXTexMtx >(GX_TEXMTX0 + 3 * (i + 1));  // (TEXMTX0 stays the identity initDraw loaded)
        GXLoadTexMtxImm(m, id, GX_MTX2x4);
        GXSetTexCoordGen2(static_cast< GXTexCoordID >(i), GX_TG_MTX2x4, GX_TG_TEX0, id, GX_FALSE, GX_PTIDENTITY);
    }
    void foldBlend(bool blend) {
        if (blend) {
            GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
        } else {
            GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
        }
    }
    void foldAlphaZero(GXTevStageID s) {
        GXSetTevAlphaIn(s, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
        GXSetTevAlphaOp(s, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
}  // namespace

int ImageEffectLocalUtil::portBloomFoldMode() {
    static u32 sSeen = 0;
    if (StageStale(sSeen)) {
        sFoldMode = -1;
    }
    if (sFoldMode < 0) {
        char sb[16];
        const char* e = StageEnv("GXV_BLOOM_FOLD", sb, sizeof sb);
        const int v0 = e && *e ? atoi(e) : 0;
        const int v0Clamped = (v0 == 1 || v0 == 2 || (v0 == 3 && !port::stageknob::Quality())) ? v0 : 0;
        char sg[16];
        const char* g = StageEnv("GXV_GLOW26", sg, sizeof sg);  // (glow26: level 1 raises the fold to 1, level 2 to 2; never lowers it)
        const int v = port::glow26::BloomFoldFloor(port::glow26::ClampLevel(g && *g ? atoi(g) : 0), v0Clamped);
        sFoldMode = (v >= 1 && v <= 3) ? v : 0;  // (never a value that removes the glow: anything else is 0)
        static const char* const sText[4] = {
            "off (the chain GXV_BLOOM_FAST says)",
            "folded chain, the glow of the original blur shape (normal bloom: copy + threshold + one blur pass = 3 texture scenes; simple bloom: 2 copies "
            "+ one pass = 3)",
            "reduced glow, one small pass of gated taps with the chain's energy and extent (normal bloom: copy + one pass = 2 texture scenes; simple "
            "bloom: 2 copies + one pass = 3)",
            "reduced-tap symmetric Gaussian glow (normal bloom: copy + 9-draw 36-tap pass = 2 texture scenes; simple bloom: 2 copies + one pass = 3)"};
        port_log_summary("bloom-fold: GXV_BLOOM_FOLD=%s: %s", e && *e ? e : "0", sText[sFoldMode]);
    }
    return sFoldMode;
}

// dive27 (GXV_WATER_BLOOMFOLD, default 0; PROPOSED 2 in Performance only): the NORMAL bloom's fold level (BloomEffect.cpp). In a stage whose normal bloom is the underwater one
// (WaterAreaHolder::mUseBloom: Beach Bowl, Ocean Ring) the level is raised to at least this one. There the normal bloom is on only while the camera is under water (the simple bloom of the
// dry beach keeps the stage's level), so the level is the same whenever the chain runs: no switch-over, no pop.
int ImageEffectLocalUtil::portBloomFoldModeNormal() {
    const int base = portBloomFoldMode();
    static const int sWaterFold = [] {
        const char* w = getenv("GXV_WATER_BLOOMFOLD");
        const int n = w && *w ? atoi(w) : 0;
        return n == 1 || n == 2 ? n : 0;
    }();
    if (sWaterFold > base && WaterAreaFunction::isBloomForWater()) {
        return sWaterFold;
    }
    return base;
}

// Sum of the taps of ONE texture over the rectangle (left, top, right, bottom) of the EFB, one draw of at most 8 taps (blend: add into the target, else
// the first draw of a pass). Tap i reads the texture at the quad's coordinate + (du, dv); its weight is w / 255 (the konst colour holds 8 bits, as the
// original ring blurs' per-tap intensity does). preClamp: the tap is first clamp(texel x 2 x preK / 255) (preScale 2 or 4: the original's clamped first
// blur, applied per tap), 2 stages a tap; else 1 stage a tap.
void ImageEffectLocalUtil::portFoldSumTaps(JUTTexture* pTex, const FoldTap* pTaps, u32 count, bool preClamp, u8 preK, u32 preScale, bool blend, bool dry,
                                           f32 left, f32 top, f32 right, f32 bottom) {
    if (count == 0 || count > 8) {
        return;
    }
    pTex->load(GX_TEXMAP0);
    u8 k[4][4] = {};
    for (u32 i = 0; i < count; i++) {
        k[i % 4][i / 4] = pTaps[i].w;
    }
    if (preClamp) {
        k[3][3] = preK;  // slot 15
    }
    for (u32 j = 0; j < 4; j++) {
        GXSetTevKColor(static_cast< GXTevKColorID >(GX_KCOLOR0 + j), Color8(k[j][0], k[j][1], k[j][2], k[j][3]));
    }
    GXSetTevColor(GX_TEVREG1, Color8(0, 0, 0, 0));
    GXSetNumTexGens(count);
    const u32 per = preClamp ? 2 : 1;
    GXSetNumTevStages(count * per);
    for (u32 i = 0; i < count; i++) {
        foldOffsets(i, pTaps[i].du, pTaps[i].dv);
        const GXTevStageID s = static_cast< GXTevStageID >(i * per);
        GXSetTevOrder(s, static_cast< GXTexCoordID >(i), GX_TEXMAP0, GX_COLOR_NULL);
        GXSetTevSwapMode(s, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GXSetTevDirect(s);
        foldAlphaZero(s);
        if (preClamp) {
            GXSetTevKColorSel(s, foldKc(15));
            GXSetTevColorIn(s, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
            GXSetTevColorOp(s, GX_TEV_ADD, GX_TB_ZERO, preScale == 4 ? GX_CS_SCALE_4 : GX_CS_SCALE_2, GX_TRUE, GX_TEVREG1);
            const GXTevStageID t = static_cast< GXTevStageID >(i * per + 1);
            GXSetTevOrder(t, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
            GXSetTevSwapMode(t, GX_TEV_SWAP0, GX_TEV_SWAP0);
            GXSetTevDirect(t);
            foldAlphaZero(t);
            GXSetTevKColorSel(t, foldKc(i));
            GXSetTevColorIn(t, GX_CC_ZERO, GX_CC_C1, GX_CC_KONST, i == 0 ? GX_CC_ZERO : GX_CC_CPREV);
            GXSetTevColorOp(t, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        } else {
            GXSetTevKColorSel(s, foldKc(i));
            GXSetTevColorIn(s, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, i == 0 ? GX_CC_ZERO : GX_CC_CPREV);
            GXSetTevColorOp(s, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        }
    }
    foldBlend(blend);
    foldQuad(dry, left, top, right, bottom);
}

// Pairs of taps: wide tap m = clamp(scale x K x (tex(a_m) + tex(b_m))) x w_m / 255 (K = preK / 255; the original's clamped first blur, estimated at the
// wide tap's place by the mean of two of its narrow taps; pTaps[2 m], pTaps[2 m + 1] are a_m, b_m, pTaps[2 m].w is w_m). 3 stages a pair, at most 4 pairs a draw.
void ImageEffectLocalUtil::portFoldPairTaps(JUTTexture* pTex, const FoldTap* pTaps, u32 pairs, u8 preK, u32 preScale, bool blend, bool dry, f32 left, f32 top,
                                            f32 right, f32 bottom) {
    if (pairs == 0 || pairs > 4) {
        return;
    }
    pTex->load(GX_TEXMAP0);
    u8 k[4][4] = {};
    for (u32 m = 0; m < pairs; m++) {
        k[m][0] = pTaps[2 * m].w;
    }
    k[3][3] = preK;  // slot 15
    for (u32 j = 0; j < 4; j++) {
        GXSetTevKColor(static_cast< GXTevKColorID >(GX_KCOLOR0 + j), Color8(k[j][0], k[j][1], k[j][2], k[j][3]));
    }
    GXSetTevColor(GX_TEVREG1, Color8(0, 0, 0, 0));
    GXSetTevColor(GX_TEVREG2, Color8(0, 0, 0, 0));
    GXSetNumTexGens(pairs * 2);
    GXSetNumTevStages(pairs * 3);
    for (u32 m = 0; m < pairs; m++) {
        foldOffsets(2 * m, pTaps[2 * m].du, pTaps[2 * m].dv);
        foldOffsets(2 * m + 1, pTaps[2 * m + 1].du, pTaps[2 * m + 1].dv);
        for (u32 c = 0; c < 3; c++) {
            const GXTevStageID s = static_cast< GXTevStageID >(m * 3 + c);
            if (c < 2) {
                GXSetTevOrder(s, static_cast< GXTexCoordID >(2 * m + c), GX_TEXMAP0, GX_COLOR_NULL);
            } else {
                GXSetTevOrder(s, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
            }
            GXSetTevSwapMode(s, GX_TEV_SWAP0, GX_TEV_SWAP0);
            GXSetTevDirect(s);
            foldAlphaZero(s);
            if (c == 0) {  // REG2 = K x tex(a)
                GXSetTevKColorSel(s, foldKc(15));
                GXSetTevColorIn(s, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
                GXSetTevColorOp(s, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG2);
            } else if (c == 1) {  // REG1 = clamp(scale x (REG2 + K x tex(b)))
                GXSetTevKColorSel(s, foldKc(15));
                GXSetTevColorIn(s, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_C2);
                GXSetTevColorOp(s, GX_TEV_ADD, GX_TB_ZERO, preScale == 4 ? GX_CS_SCALE_4 : GX_CS_SCALE_2, GX_TRUE, GX_TEVREG1);
            } else {  // PREV += REG1 x w
                GXSetTevKColorSel(s, foldKc(m));
                GXSetTevColorIn(s, GX_CC_ZERO, GX_CC_C1, GX_CC_KONST, m == 0 ? GX_CC_ZERO : GX_CC_CPREV);
                GXSetTevColorOp(s, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            }
        }
    }
    foldBlend(blend);
    foldQuad(dry, left, top, right, bottom);
}

// Gated taps of a texture that holds a picture (RGB): tap i is the texel colour where the texel's value (luma 66/128/25 of 255 = the I8 copy's
// 0.257 / 0.504 / 0.098, or R / G / B alone as the simple bloom's mask filters 1..3) exceeds the threshold, else black: the original's "colour x (I8 >
// threshold)", evaluated at the tap. The value is built in TEV alpha from three reads (swap tables R / G / B -> A), 4 stages a tap, at most 4 taps a
// draw; the sum goes into the target as portFoldSumTaps' does. threshold is the BloomArea / SimpleBloomArea threshold (0..255).
void ImageEffectLocalUtil::portFoldGatedTaps(JUTTexture* pTex, const FoldTap* pTaps, u32 count, s32 maskFilter, u8 threshold, bool blend, bool dry, f32 left,
                                             f32 top, f32 right, f32 bottom) {
    static const u8 sWeight[4][3] = {{66, 128, 25}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}};  // (I8, R8, G8, B8: the copy's channel)
    static const GXTevSwapSel sSwap[3] = {GX_TEV_SWAP1, GX_TEV_SWAP2, GX_TEV_SWAP3};
    static const GXTevKAlphaSel sKa[3] = {GX_TEV_KASEL_K0_R, GX_TEV_KASEL_K0_G, GX_TEV_KASEL_K0_B};
    if (count == 0 || count > 4) {
        return;
    }
    const s32 f = maskFilter >= 0 && maskFilter <= 3 ? maskFilter : 0;
    u8 t = threshold;
    if (f == 0) {
        t = threshold > 16 ? threshold - 16 : 0;  // (I8 = luma + 16; below 16 every non-black texel passes, as I8 > threshold does)
    }
    pTex->load(GX_TEXMAP0);
    GXSetChanMatColor(GX_COLOR0A0, Color8(t, t, t, 255));
    GXSetTevKColor(GX_KCOLOR0, Color8(sWeight[f][0], sWeight[f][1], sWeight[f][2], 0));
    u8 w[4] = {};
    for (u32 i = 0; i < count; i++) {
        w[i] = pTaps[i].w;
    }
    GXSetTevKColor(GX_KCOLOR1, Color8(w[0], w[1], w[2], w[3]));
    GXSetTevColor(GX_TEVREG0, Color8(0, 0, 0, 0));
    GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_RED);
    GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_GREEN);
    GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_BLUE);
    GXSetNumTexGens(count);
    GXSetNumTevStages(count * 4);
    for (u32 i = 0; i < count; i++) {
        foldOffsets(i, pTaps[i].du, pTaps[i].dv);
        for (u32 c = 0; c < 3; c++) {
            const GXTevStageID s = static_cast< GXTevStageID >(i * 4 + c);
            GXSetTevOrder(s, static_cast< GXTexCoordID >(i), GX_TEXMAP0, GX_COLOR_NULL);
            GXSetTevSwapMode(s, GX_TEV_SWAP0, sSwap[c]);
            GXSetTevDirect(s);
            if (c == 0) {  // (the tap's colour x its weight; the value starts here)
                GXSetTevKColorSel(s, foldKc(1 + 4 * i));  // (konst colour 1, channel i)
                GXSetTevColorIn(s, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
            } else {
                GXSetTevColorIn(s, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
            }
            GXSetTevColorOp(s, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GXSetTevAlphaIn(s, GX_CA_ZERO, GX_CA_TEXA, GX_CA_KONST, c == 0 ? GX_CA_ZERO : GX_CA_APREV);
            GXSetTevAlphaOp(s, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GXSetTevKAlphaSel(s, sKa[c]);
        }
        const GXTevStageID g = static_cast< GXTevStageID >(i * 4 + 3);  // (the compare: the sum so far + (value > threshold ? colour : 0))
        GXSetTevOrder(g, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevSwapMode(g, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GXSetTevDirect(g);
        GXSetTevColorIn(g, GX_CC_APREV, GX_CC_RASC, GX_CC_CPREV, GX_CC_C0);
        GXSetTevColorOp(g, GX_TEV_COMP_R8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, i + 1 == count ? GX_TEVPREV : GX_TEVREG0);
        GXSetTevAlphaIn(g, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GXSetTevAlphaOp(g, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
    foldBlend(blend);
    foldQuad(dry, left, top, right, bottom);
}

// One tap that holds the 4 x 4 box of a half-size picture (the original's threshold pass: 4 reads at the 2 x 2 sub-block centres, each a bilinear 2 x 2
// box), gated by the box's value as portFoldGatedTaps gates a read, times w / 255. 14 stages, one tap a draw. The tap's centre is the quad's coordinate +
// (du, dv). (This is bloomFastThreshold's evaluation moved to the tap.)
void ImageEffectLocalUtil::portFoldGatedBox(JUTTexture* pTex, const FoldTap& rTap, u8 threshold, bool blend, bool dry, f32 left, f32 top, f32 right,
                                            f32 bottom) {
    static const GXTevSwapSel sSwap[3] = {GX_TEV_SWAP1, GX_TEV_SWAP2, GX_TEV_SWAP3};
    // konst alpha weights / 4 per read (K0 = 17, 16, 32, 6; K1.r = 7): R 17+16+17+16 = 66, G 4 x 32 = 128, B 6+6+6+7 = 25
    static const GXTevKAlphaSel sKSel[4][3] = {{GX_TEV_KASEL_K0_R, GX_TEV_KASEL_K0_B, GX_TEV_KASEL_K0_A},
                                               {GX_TEV_KASEL_K0_G, GX_TEV_KASEL_K0_B, GX_TEV_KASEL_K0_A},
                                               {GX_TEV_KASEL_K0_R, GX_TEV_KASEL_K0_B, GX_TEV_KASEL_K0_A},
                                               {GX_TEV_KASEL_K0_G, GX_TEV_KASEL_K0_B, GX_TEV_KASEL_K1_R}};
    pTex->load(GX_TEXMAP0);
    const u8 t = threshold > 16 ? threshold - 16 : 0;
    GXSetChanMatColor(GX_COLOR0A0, Color8(t, t, t, 255));
    GXSetTevKColor(GX_KCOLOR0, Color8(17, 16, 32, 6));
    GXSetTevKColor(GX_KCOLOR1, Color8(7, 0, 0, 0));
    GXSetTevKColor(GX_KCOLOR2, Color8(rTap.w, 0, 0, 0));
    GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_RED);
    GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_GREEN);
    GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_BLUE);
    const f32 du = 1.0f / pTex->getWidth(), dv = 1.0f / pTex->getHeight();
    GXSetNumTexGens(4);
    for (u32 j = 0; j < 4; j++) {
        foldOffsets(j, rTap.du + ((j & 1) ? du : -du), rTap.dv + ((j & 2) ? dv : -dv));
    }
    GXSetNumTevStages(14);
    for (u32 j = 0; j < 4; j++) {
        for (u32 c = 0; c < 3; c++) {
            const u32 i = 3 * j + c;
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
    GXSetTevOrder(GX_TEVSTAGE13, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);  // (the tap's weight)
    GXSetTevSwapMode(GX_TEVSTAGE13, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXSetTevKColorSel(GX_TEVSTAGE13, GX_TEV_KCSEL_K2_R);
    GXSetTevColorIn(GX_TEVSTAGE13, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE13, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    foldAlphaZero(GX_TEVSTAGE13);
    GXSetTevDirect(GX_TEVSTAGE13);
    foldBlend(blend);
    foldQuad(dry, left, top, right, bottom);
}

// The glow add of the fold chains: EFB += texture x intensity (x 2 with scale2), one stage, blend ONE / ONE as the original's last draw.
void ImageEffectLocalUtil::portFoldGlow(JUTTexture* pTex, u8 intensity, bool scale2, bool dry) {
    pTex->load(GX_TEXMAP0);
    GXSetChanMatColor(GX_COLOR0A0, Color8(intensity, intensity, intensity, 255));
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, scale2 ? GX_CS_SCALE_2 : GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevDirect(GX_TEVSTAGE0);
    foldBlend(true);
    foldQuad(dry, 0.0f, 0.0f, static_cast< f32 >(MR::getFrameBufferWidth()), static_cast< f32 >(MR::getFrameBufferHeight()));
}

// Back to the state initDraw / setupDrawTexture leave (one texture coordinate, one modulate stage), and the TEV registers, konst colours and swap
// tables the fold helpers changed back to J3D's reinitGX values; the state later draws inherit is then the one the original chain leaves.
void ImageEffectLocalUtil::portFoldEnd() {
    const GXColor white = {255, 255, 255, 255};
    GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
    for (int i = 0; i < 4; i++) {
        GXSetTevKColor(static_cast< GXTevKColorID >(GX_KCOLOR0 + i), white);
    }
    GXSetTevColor(GX_TEVREG0, white);
    GXSetTevColor(GX_TEVREG1, white);
    for (int i = 0; i < 16; i++) {
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


void ImageEffectLocalUtil::portBloomChainNote(const void* pOwner, u32 captures, u32 draws, int kind) {
    (void)pOwner;
    if (sRehearsing || kind < 0 || kind > 11) {
        return;
    }
    if (sNotePictures == 0) {
        sNoteFirstPic = sBloomPic;
    }
    ++sNotePictures;
    if (kind >= 8) {
        port::glow26::g_count.bloomFolded.fetch_add(1, std::memory_order_relaxed);  // (compfix26: a bloom picture drawn by the GXV_BLOOM_FOLD chain)
    }
    ++sNoteKind[kind];
    sNoteCaptures[kind] += captures;
    sNoteDraws[kind] += draws;
    const u32 span = sBloomPic - sNoteFirstPic + 1;
    if (sNotePictures >= 600 || span >= 1800) {
        char part[12][48];
        for (int i = 0; i < 12; i++) {
            const u32 n = sNoteKind[i] ? sNoteKind[i] : 1;
            snprintf(part[i], sizeof part[i], "%u (%u.%u captures, %u.%u draws)", (unsigned)sNoteKind[i], (unsigned)(sNoteCaptures[i] / n),
                     (unsigned)(sNoteCaptures[i] * 10 / n % 10), (unsigned)(sNoteDraws[i] / n), (unsigned)(sNoteDraws[i] * 10 / n % 10));
        }
        port_log_summary("bloom-fast: %u bloom pictures in %u pictures (GXV_BLOOM_FAST=%d): normal bloom: fast %s, original %s, reused %u "
                         "(PETARI_BLOOM_HALFRATE), dry %u (PETARI_BLOOM_SKIP); simple bloom: fast %s, exact %s, original %s, reused %u; "
                         "GXV_BLOOM_FOLD=%d: normal fold1 %s, fold2 %s; simple fold1 %s, fold2 %s",
                         (unsigned)sNotePictures, (unsigned)span, sFastMode, part[1], part[0], (unsigned)sNoteKind[2], (unsigned)sNoteKind[3], part[5],
                         part[6], part[4], (unsigned)sNoteKind[7], sFoldMode, part[8], part[9], part[10], part[11]);
        sNotePictures = 0;
        for (int i = 0; i < 12; i++) {
            sNoteKind[i] = sNoteCaptures[i] = sNoteDraws[i] = 0;
        }
    }
}
#endif
