// GXV_LIGHTVOL host proof, the game's half (tools/test_lightvol_gl.sh). Compiled with the game's flags (TARGET_PC) and linked with the PRODUCTION
// decomp/src/Game/LiveActor/VolumeModelDrawer.cpp (the drawer's own ctor, draw() and flushDeferred()), the console's GX library (decomp RVL_SDK/gx) and, through
// tests/lightvol_gl_main.cpp, the production vgx front end and GLES back end on Mesa. No game data.
// What this file stands in for: MR::* and NameObj (a camera, the GX state of DrawUtil's setupShadowVolumeDraw copied word for word), the model (a closed tapered box whose
// 12 triangles are drawn by drawSimpleModel's stand-in; the box the drawer reads comes through port_lightvol_box_hook).
// Each picture: a floor and pillars (colour, depth, alpha as the scene says), the volumes (every drawer's draw(), then SceneFunction's flushDeferred()), the picture read back
// as RGBA8. Scenes (LV_SCENE): 1 Dreadnought's shape (16 small beams in a ring + 4 large ones), 2 culls (wholly outside, behind the eye, across an edge, inside),
// 3 overlaps (pairs and a stack, in one place), 4 random 40 (some straddling the near plane), 5 as 1 with the floor's alpha 255 (the alpha plane is part of the picture).
// LV_NEG: 1 every volume pass by pass in one group (no disjointness test), 2 volumes whose box CENTRE is off the screen left out (the cut that is not exact), 3 as 2 but
// only the ones wholly behind the eye are kept out (control that the production cull's extra step is what keeps edge volumes).
#include "Game/LiveActor/VolumeModelDrawer.hpp"
#include "Game/System/ResourceHolder.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/Functor.hpp"
#include <revolution/gx.h>
#include <revolution/mtx.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
void* hx_guest_alloc(u32 bytes);
void hx_fifo_init(void);
void hx_fifo_flush(void);
}
extern "C" bool (*port_lightvol_box_hook)(const void* drawer, float* bmin, float* bmax);

namespace {
constexpr int kW = 640, kH = 456;
GXRenderModeObj sMode;
TVec3f sPlayer(0.0f, 0.0f, -300.0f);
TPos3f sView;
TProj3f sProj;
ResourceHolder* sHolder;
ResTable* sTable;
alignas(16) unsigned char sHolderBuf[256];
alignas(16) unsigned char sTableBuf[256];
alignas(16) unsigned char sModelTag[16];
u8* sReadback = nullptr;
u8* sXfb = nullptr;
int sScene = 1, sNeg = 0, sFlip = 0, sPassMask = 15;
float sMove[3] = {0, 0, 0};  // LV_MOVE=dx,dy,dz: the player's step per picture (the LOD's fade is seen over a walk)
}  // namespace

// ---- stand-ins -------------------------------------------------------------------------------------------------------------------------------------
NameObj::NameObj(const char*) {}
NameObj::~NameObj() {}
void NameObj::init(const JMapInfoIter&) {}
void NameObj::initAfterPlacement() {}
void NameObj::movement() {}
void NameObj::draw() const {}
void NameObj::calcAnim() {}
void NameObj::calcViewAndEntry() {}
void* ResTable::getRes(const char*) const { return sModelTag; }
void* operator new(unsigned long n, JKRHeap*, int) { return std::malloc(n); }

namespace MR {
void createSceneObj(int) {}
ResourceHolder* createAndAddResourceHolder(const char*) { return sHolder; }
void registerPreDrawFunction(const FunctorBase&, int) {}
TVec3f* getPlayerPos() { return &sPlayer; }
const TPos3f& getCameraViewMtx() { return sView; }
const TProj3f& getCameraProjectionMtx() { return sProj; }
void loadProjectionMtx() {
    GXSetProjection(sProj.mMtx, GX_PERSPECTIVE);
}
// DrawUtil.cpp's body
void setupShadowVolumeDraw() {
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetNumIndStages(0);
    GXSetNumTexGens(0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanMatColor(GX_COLOR0A0, Color8(0));
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_A0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetZCompLoc(GX_FALSE);
    GXSetZMode(GX_TRUE, GX_GEQUAL, GX_FALSE);
    GXSetClipMode(GX_CLIP_ENABLE);
}
}  // namespace MR

namespace {
// ---- the model: a closed tapered box, x z in [-1, 1] at y = 4 and [-0.5, 0.5] at y = 0 (a beam widening away from its source), 12 outward triangles
constexpr float kLo[3] = {-1.0f, 0.0f, -1.0f}, kHi[3] = {1.0f, 4.0f, 1.0f};
const float kV[8][3] = {{-0.5f, 0, -0.5f}, {0.5f, 0, -0.5f}, {0.5f, 0, 0.5f}, {-0.5f, 0, 0.5f}, {-1, 4, -1}, {1, 4, -1}, {1, 4, 1}, {-1, 4, 1}};
const int kT[12][3] = {{0, 2, 1}, {0, 3, 2},   // bottom (-y)
                       {4, 5, 6}, {4, 6, 7},   // top (+y)
                       {0, 1, 5}, {0, 5, 4},   // -z
                       {2, 3, 7}, {2, 7, 6},   // +z
                       {3, 0, 4}, {3, 4, 7},   // -x
                       {1, 2, 6}, {1, 6, 5}};  // +x
bool BoxHook(const void*, float* lo, float* hi) {
    for (int k = 0; k < 3; ++k) { lo[k] = kLo[k]; hi[k] = kHi[k]; }
    return true;
}

struct Vol {
    VolumeModelDrawer* d;
    Mtx m;  // model -> world (the camera is the identity: world = view); the drawer keeps a pointer to it
};
std::vector< Vol* > sVols;

void Place(float x, float y, float z, float scale, float yaw, float pitch, u8 r, u8 g, u8 b, u8 a) {
    Vol* v = static_cast< Vol* >(std::calloc(1, sizeof(Vol)));
    // rotate about z by `pitch` (tilt the beam's y axis in the x-y plane) then about y by `yaw`, scale, translate
    const float cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
    const float rz[3][3] = {{cp, -sp, 0}, {sp, cp, 0}, {0, 0, 1}};
    const float ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float s = 0;
            for (int k = 0; k < 3; ++k) s += ry[i][k] * rz[k][j];
            v->m[i][j] = s * scale;
        }
    v->m[0][3] = x; v->m[1][3] = y; v->m[2][3] = z;
    v->d = new VolumeModelDrawer("v", "Beam", reinterpret_cast< MtxPtr >(v->m));
    v->d->mColor = Color8(r, g, b, a);
    sVols.push_back(v);
}

unsigned Lcg(unsigned& s) { s = s * 1664525u + 1013904223u; return s >> 8; }
float Rnd(unsigned& s, float lo, float hi) { return lo + (hi - lo) * ((Lcg(s) & 0xFFFF) / 65535.0f); }

// (every beam points down, its far end below the floor at y = -120: the floor, the pillars and the wall are what its volume tints)
constexpr float kDown = 3.14159265f;
void BuildScene(int scene) {
    sVols.clear();
    unsigned s = 12345u + scene;
    if (scene == 1 || scene == 5) {
        for (int i = 0; i < 16; ++i) {  // BeamGoRoundPlanet's ring: small, far
            const float a = 6.2831853f * i / 16.0f;
            Place(260.0f * std::cos(a), 10.0f + 20.0f * std::sin(a), -1400.0f + 140.0f * std::cos(2 * a), 36.0f, a, kDown + 0.25f * std::sin(3 * a), 255, 169, 0, 140);
        }
        Place(-330, 300, -520, 110.0f, 0.3f, kDown - 0.1f, 255, 120, 20, 120);  // EyeBeamer's: large, nearer
        Place(380, 300, -640, 110.0f, -0.4f, kDown + 0.2f, 255, 120, 20, 120);
        Place(-120, 300, -380, 100.0f, 1.1f, kDown + 0.1f, 255, 120, 20, 120);
        Place(160, 300, -300, 100.0f, 2.0f, kDown - 0.1f, 255, 120, 20, 120);
    } else if (scene == 2) {
        Place(-9000, 100, -800, 60, 0.2f, kDown, 255, 100, 40, 160);   // wholly left of the screen
        Place(9000, 100, -800, 60, 0.2f, kDown, 255, 100, 40, 160);    // wholly right
        Place(0, 9000, -800, 60, 0.2f, kDown, 255, 100, 40, 160);      // wholly above
        Place(0, 100, 900, 60, 0.2f, kDown, 255, 100, 40, 160);        // wholly behind the eye
        Place(-100, 100, 500, 30, 1.0f, kDown, 255, 100, 40, 160);     // behind the eye
        Place(0, 100, 4000, 30, 0.0f, kDown, 255, 100, 40, 160);       // behind the eye, far
        Place(-405, 100, -500, 150, 0.0f, kDown, 100, 255, 60, 150);   // across the left edge (centre off the screen, a third of it on)
        Place(405, 100, -500, 150, 0.5f, kDown, 100, 255, 60, 150);    // across the right edge
        Place(0, 100, -300, 100, 0.0f, kDown, 100, 255, 60, 150);      // across the bottom edge (the floor under the camera)
        Place(0, 100, -700, 60, 0.4f, kDown, 60, 120, 255, 150);       // inside
        Place(-200, 100, -900, 50, 1.2f, kDown + 0.2f, 60, 120, 255, 150);
        Place(220, 100, -600, 45, 2.2f, kDown - 0.2f, 60, 120, 255, 150);
    } else if (scene == 3) {
        Place(0, 100, -700, 60, 0.3f, kDown, 255, 100, 40, 160);  // an overlapping pair
        Place(30, 100, -720, 60, 0.9f, kDown - 0.2f, 40, 255, 100, 160);
        Place(-300, 100, -800, 70, 0.0f, kDown, 255, 100, 40, 160);  // a stack of three
        Place(-290, 100, -790, 70, 0.0f, kDown, 100, 40, 255, 160);
        Place(-310, 100, -800, 70, 0.5f, kDown, 255, 255, 40, 160);
        Place(300, 100, -900, 40, 1.0f, kDown, 255, 100, 40, 160);  // a disjoint one
        Place(460, 100, -900, 40, 1.0f, kDown, 255, 100, 40, 160);  // and its neighbour at a gap
        Place(100, 150, -500, 55, 1.5f, kDown + 0.6f, 90, 200, 255, 100);  // crosses the pair
    } else if (scene == 4) {
        for (int i = 0; i < 40; ++i) {
            Place(Rnd(s, -900, 900), Rnd(s, 0, 300), Rnd(s, -1500, 250), Rnd(s, 25, 100), Rnd(s, 0, 6.28f), kDown + Rnd(s, -0.8f, 0.8f), static_cast< u8 >(Rnd(s, 60, 255)),
                  static_cast< u8 >(Rnd(s, 60, 255)), static_cast< u8 >(Rnd(s, 0, 255)), static_cast< u8 >(Rnd(s, 60, 200)));
        }
    }
}

// ---- the picture
void setupColourDraw() {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXLoadPosMtxImm(sView.mMtx, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetProjection(sProj.mMtx, GX_PERSPECTIVE);
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
    GXSetAlphaUpdate(GX_TRUE);
    GXSetDstAlpha(GX_FALSE, 0);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXSetZCompLoc(GX_TRUE);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, GXColor{0, 0, 0, 0});
}
inline void V(f32 x, f32 y, f32 z, u8 r, u8 g, u8 b, u8 a) {
    GXPosition3f32(x, y, z);
    GXColor4u8(r, g, b, a);
}
void Quad(const float p[4][3], u8 r, u8 g, u8 b, u8 a) {
    GXBegin(GX_QUADS, GX_VTXFMT1, 4);
    for (int i = 0; i < 4; ++i) V(p[i][0], p[i][1], p[i][2], r, g, b, a);
    GXEnd();
}
void DrawWorld(int scene) {
    setupColourDraw();
    const u8 fa = scene == 5 ? 255 : 0;
    // the floor, a plane under the camera, receding
    GXBegin(GX_QUADS, GX_VTXFMT1, 4);
    V(-3000, -120, -20, 70, 110, 70, fa);
    V(3000, -120, -20, 110, 150, 90, fa);
    V(3000, -120, -4000, 50, 70, 130, fa);
    V(-3000, -120, -4000, 90, 90, 90, fa);
    GXEnd();
    // a back wall and pillars (the volumes pass through them: the tint is only where the scene is INSIDE the volume)
    const float wall[4][3] = {{-3000, -120, -2200}, {3000, -120, -2200}, {3000, 900, -2200}, {-3000, 900, -2200}};
    Quad(wall, 60, 60, 90, fa);
    const float pillar1[4][3] = {{-50, -120, -450}, {50, -120, -450}, {50, 400, -450}, {-50, 400, -450}};
    Quad(pillar1, 200, 160, 90, 255);
    const float pillar2[4][3] = {{200, -120, -800}, {340, -120, -800}, {340, 300, -800}, {200, 300, -800}};
    Quad(pillar2, 230, 60, 60, 255);
    const float pillar3[4][3] = {{-420, -120, -650}, {-300, -120, -650}, {-300, 350, -650}, {-420, 350, -650}};
    Quad(pillar3, 90, 200, 90, 128);
}
// drawSimpleModel's stand-in: the 12 triangles in model space (the position matrix 0 the drawer loaded)
}  // namespace

namespace MR {
void drawSimpleModel(J3DModelData*) {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 36);
    for (int t = 0; t < 12; ++t) {
        const int* tri = kT[t];
        const int o[3] = {0, sFlip ? 2 : 1, sFlip ? 1 : 2};
        for (int k = 0; k < 3; ++k) GXPosition3f32(kV[tri[o[k]]][0], kV[tri[o[k]]][1], kV[tri[o[k]]][2]);
    }
    GXEnd();
}
}  // namespace MR

namespace {
// the controls' own passes (the same statements as the drawer's, written here: the test's picture must SEE a wrong grouping / a wrong cut)
void NaivePass(const Vol& v, int pass, GXColor c, bool skipDraw) {
    Mtx mv;
    PSMTXConcat(sView.mMtx, v.m, mv);
    GXLoadPosMtxImm(mv, 0);
    GXSetCurrentMtx(0);
    GXSetTevColor(GX_TEVREG0, c);
    switch (pass) {
    case 1:
        GXSetColorUpdate(0); GXSetAlphaUpdate(1); GXSetDstAlpha(1, 0); GXSetCullMode(GX_CULL_NONE); GXSetZMode(0, GX_ALWAYS, 0);
        GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
        break;
    case 2:
        GXSetDstAlpha(0, 0); GXSetZMode(1, GX_GEQUAL, 0); GXSetCullMode(GX_CULL_FRONT);
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ZERO, GX_LO_COPY);
        break;
    case 3:
        GXSetCullMode(GX_CULL_BACK);
        GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ZERO, GX_BL_ZERO, GX_LO_COPY);
        break;
    default:
        GXSetCullMode(GX_CULL_NONE);
        GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_ONE, GX_LO_COPY);
        GXSetColorUpdate(1); GXSetDstAlpha(1, 0); GXSetZMode(0, GX_ALWAYS, 0);
        break;
    }
    if (!skipDraw) MR::drawSimpleModel(nullptr);
}
bool CentreOnScreen(const Vol& v) {
    const float c[3] = {0.0f, 2.0f, 0.0f};
    const float x = v.m[0][0] * c[0] + v.m[0][1] * c[1] + v.m[0][2] * c[2] + v.m[0][3];
    const float y = v.m[1][0] * c[0] + v.m[1][1] * c[1] + v.m[1][2] * c[2] + v.m[1][3];
    const float z = v.m[2][0] * c[0] + v.m[2][1] * c[1] + v.m[2][2] * c[2] + v.m[2][3];
    if (z >= -1.0f) return false;
    const float nx = sProj.mMtx[0][0] * x / -z, ny = sProj.mMtx[1][1] * y / -z;
    return nx >= -1.0f && nx <= 1.0f && ny >= -1.0f && ny <= 1.0f;
}
}  // namespace

extern "C" void hg_init(int scene) {
    sScene = scene;
    sNeg = std::getenv("LV_NEG") ? std::atoi(std::getenv("LV_NEG")) : 0;
    sPassMask = std::getenv("LV_PASSES") ? std::atoi(std::getenv("LV_PASSES")) : 15;
    sFlip = std::getenv("LV_FLIP") ? std::atoi(std::getenv("LV_FLIP")) : 0;
    hx_fifo_init();
    static u8 fifoDummy[64];
    GXInit(fifoDummy, sizeof fifoDummy);
    sMode = GXNtsc480IntDf;
    sMode.fbWidth = kW;
    sMode.efbHeight = kH;
    sMode.xfbHeight = kH;
    GXSetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
    GXSetCopyFilter(GX_FALSE, sMode.sample_pattern, GX_FALSE, sMode.vfilter);
    GXSetDispCopySrc(0, 0, kW, kH);
    GXSetDispCopyDst(kW, kH);
    sReadback = static_cast< u8* >(hx_guest_alloc(GXGetTexBufferSize(kW, kH, GX_TF_RGBA8, GX_FALSE, 1)));
    sXfb = static_cast< u8* >(hx_guest_alloc(kW * kH * 2));
    PSMTXIdentity(sView.mMtx);
    C_MTXPerspective(sProj.mMtx, 45.0f, 16.0f / 9.0f, 1.0f, 10000.0f);
    std::memset(sHolderBuf, 0, sizeof sHolderBuf);
    std::memset(sTableBuf, 0, sizeof sTableBuf);
    sHolder = reinterpret_cast< ResourceHolder* >(sHolderBuf);
    sTable = reinterpret_cast< ResTable* >(sTableBuf);
    sHolder->mModelResTable = sTable;
    port_lightvol_box_hook = &BoxHook;
    BuildScene(scene);
    if (const char* p = std::getenv("LV_MOVE")) std::sscanf(p, "%f,%f,%f", &sMove[0], &sMove[1], &sMove[2]);
    if (const char* p = std::getenv("LV_PLAYER")) {  // "x,y,z"
        float x, y, z;
        if (std::sscanf(p, "%f,%f,%f", &x, &y, &z) == 3) sPlayer.set(x, y, z);
    }
}

extern "C" void hg_frame(int n) {
    if (n > 1) {
        sPlayer.x += sMove[0]; sPlayer.y += sMove[1]; sPlayer.z += sMove[2];
    }
    GXSetViewport(0.0f, 0.0f, kW, kH, 0.0f, 1.0f);
    GXSetScissor(0, 0, kW, kH);
    DrawWorld(sScene);
    // the category's turn: its pre-draw function, every owner's draw, then SceneFunction::executeDrawListXlu's flush
    MR::setupShadowVolumeDraw();
    VolumeModelDrawer::portBegin();
    if (sNeg == 0) {
        for (size_t i = 0; i < sVols.size(); ++i) sVols[i]->d->draw();
        VolumeModelDrawer::flushDeferred();
    } else {
        std::vector< const Vol* > use;
        for (size_t i = 0; i < sVols.size(); ++i) {
            if (sNeg == 2 && !CentreOnScreen(*sVols[i])) continue;
            use.push_back(sVols[i]);
        }
        for (int pass = 1; pass <= 4; ++pass)  // (NEG1: one group, no disjointness test; NEG2: the centre cut)
            for (size_t i = 0; i < use.size(); ++i) {
                NaivePass(*use[i], pass, use[i]->d->mColor, !(sPassMask >> (pass - 1) & 1));
            }
    }
    hx_fifo_flush();
    GXSetCopyFilter(GX_FALSE, sMode.sample_pattern, GX_FALSE, sMode.vfilter);
    GXSetTexCopySrc(0, 0, kW, kH);
    GXSetTexCopyDst(kW, kH, GX_TF_RGBA8, GX_FALSE);
    GXCopyTex(sReadback, GX_FALSE);
    GXPixModeSync();
    GXSetCopyClear(GXColor{0, 0, 0, 0}, 0xFFFFFF);
    GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    GXCopyDisp(sXfb, GX_TRUE);
    hx_fifo_flush();
}
