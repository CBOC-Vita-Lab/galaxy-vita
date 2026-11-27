// Galaxy 3.0 multiplayer: the character select's 3D heads (owner: heads3d). See MpHead3D.hpp.
#include "port/mp_test_env.h"   // worldsync: desktop-only test switches (nothing on the console)
#include "Game/MP/MpHead3D.hpp"
#include "Game/MP/MpHead3DLook.hpp"
#include "Game/MP/MpHead3DMotion.hpp"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "Game/LiveActor/ActorLightCtrl.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/MP/MpPanelDraw.hpp"
#include "Game/MP/MpUi.hpp"
#include "Game/MP/MpGame.hpp"   // integ: MpPuppets::nameObjSlotsLeft
#include "Game/MP/MpDropInLogic.hpp"
#include "Game/NameObj/NameObjArchiveListCollector.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Player/J3DModelX.hpp"
#include "Game/System/ShapePacketUserData.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/JointUtil.hpp"
#include "Game/Util/LightUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include "nw4r/ut/CharWriter.h"
#include "nw4r/ut/Font.h"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/StarPointerUtil.hpp"
#include "port/mp_bridge.h"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/FileUtil.hpp"
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3DGraphBase/J3DSys.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <revolution/gx.h>
#include <revolution/mtx.h>

namespace MpHead3D {
namespace {

// ------------------------------------------------------------------ the heads
enum Kind : uint8_t { KIND_FILESELECT = 0, KIND_GHOST = 1 };
struct Def {
    const char* arc;
    Kind kind;
    bool yellow;   // Toad's head with the cap's spot read as yellow
};
// by MpLook (Mario, Luigi, Toad, Cosmic Mario, Yellow Toad, Cosmic Luigi)
const Def kDefs[kLooks] = {
    {"FileSelectDataMario", KIND_FILESELECT, false},   {"FileSelectDataLuigi", KIND_FILESELECT, false},
    {"FileSelectDataKinopio", KIND_FILESELECT, false}, {"GhostMario", KIND_GHOST, false},
    {"FileSelectDataKinopio", KIND_FILESELECT, true},  {"GhostLuigi", KIND_GHOST, false},
};

// The camera: a narrow perspective (the heads at the sides are seen a little from the side, as the file select's are), the heads at
// kDepth in front of it. One square unit of the picture at that depth is kDepth * 2 tan(fovy / 2) / 456 world units.
constexpr float kFovY = 24.0f, kNear = 100.0f, kFar = 20000.0f, kDepth = 2400.0f;
// The head fills this much of the layout's head box (MiiIcon's 100 units hold a head with a margin)
constexpr float kFill = 1.0f;
constexpr float kPi = 3.14159265f;
// The set reads as one: the heads' boxes are equally tall, Toad's round cap is wider and Mario's cap narrower than the rest (by MpLook)
constexpr float kBalance[kLooks] = {1.05f, 1.0f, 0.9f, 1.0f, 0.9f, 1.0f};
uint32_t sTick = 0;

// review2 #8: is the character's archive loaded (the Ghost archives only when the feature is on; a disc of another region may name
// the file select's differently)? Without it initModelManagerWithAnm would dereference a missing model: the head falls back to the 2D one.
// (desktop test: SMG_MP_TEST_HEAD_MISSING=<bits by MpLook> pretends those characters' archives are not there)
bool ArchiveThere(int look) {
    static const int sTestMissing = [] {
        const char* e = MP_TEST_GETENV("SMG_MP_TEST_HEAD_MISSING");
        return e && *e ? atoi(e) : 0;
    }();
    if (look < 0 || look >= kLooks || (sTestMissing & (1 << look))) return false;
    return MR::isLoadedObjectOrLayoutArchive(kDefs[look].arc);
}

class HeadActor : public LiveActor {
public:
    explicit HeadActor(int look) : LiveActor("MpHead3D"), mLook(look) { PSMTXIdentity(mBase); }
    virtual void init(const JMapInfoIter&);
    virtual void control() {}
    // (drawn by hand in the 2D pass: computed every frame it is shown, never deferred by the port's animation LOD)
    virtual void calcAnim() { calcAnmMtx(); }
    virtual void calcAndSetBaseMtx() { MR::setBaseTRMtx(this, mBase); }
    void measure();

    int mLook;
    Mtx mBase;
    TVec3f mCentre;   // the head's centre in model space (scale 1)
    float mHeight = 60.0f;
    uint32_t mHidden = 0;   // materials (by index) whose shapes are not the head's (GhostMario's body and hands)
    int mYellowMat = -1;    // Yellow Toad: the material that carries the cap
    bool mOk = false;
};

void HeadActor::init(const JMapInfoIter&) {
    const Def& d = kDefs[mLook];
    mOk = false;
    if (!ArchiveThere(mLook)) {
        mp_log("head3d: look=%d %s: archive not loaded, the 2D head is used", mLook, d.arc);
        return;
    }
    initModelManagerWithAnm(d.arc, nullptr, false);
    if (MR::getJ3DModel(this) == nullptr) {
        mp_log("head3d: look=%d %s: no model, the 2D head is used", mLook, d.arc);
        return;
    }
    // the light of the file select's NPC buffer (FileSelectModel's: the zone's strong light), named here: MR::initLightCtrl would ask
    // the draw buffer this actor is not in
    initActorLightCtrl();
    mActorLightCtrl->init(MR::LightType_Strong, false);
    mFlag.mIsDead = false;   // (never connected to the scene: run and drawn by MpHead3D only)
    if (d.kind == KIND_GHOST) {
        if (MR::isExistBtk(this, d.arc)) MR::startBtk(this, d.arc);   // GhostPlayer::appear: the shimmer
        if (MR::isExistBrk(this, d.arc)) MR::startBrk(this, d.arc);
    } else {
        MR::startAction(this, "normal");   // FileSelectModel::exeOpen
    }
    measure();
    mOk = true;
}

void HeadActor::measure() {
    J3DModel* model = MR::getJ3DModel(this);
    if (!model) return;
    J3DModelData* data = model->getModelData();
    const Def& d = kDefs[mLook];
    // the materials: GhostMario's body and hands are not drawn; Toad's cap material for the yellow spot
    JUTNameTab* names = data->getMaterialName();
    for (u16 i = 0; i < data->getMaterialNum() && i < 32; i++) {
        const char* n = names ? names->getName(i) : nullptr;
        if (!n) continue;
        if (d.kind == KIND_GHOST && (strncmp(n, "Body", 4) == 0 || strncmp(n, "Hand", 4) == 0)) mHidden |= 1u << i;
        if (d.yellow && strncmp(n, "BodyMat", 7) == 0) mYellowMat = i;
    }
    // a first pose, at the origin and scale 1 (the bind pose: these heads have no joint animation)
    PSMTXIdentity(mBase);
    mScale.set(1.0f, 1.0f, 1.0f);
    calcAnmMtx();
    if (d.kind == KIND_FILESELECT) {
        // one joint, the whole head: its bounding box
        J3DJoint* j = data->getJointNodePointer(0);
        const Vec& lo = *j->getMin();
        const Vec& hi = *j->getMax();
        if (hi.y > lo.y && hi.x > lo.x) {
            mCentre.set((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f);
            mHeight = hi.y - lo.y;
        } else {
            mCentre.set(0.0f, 30.0f, 0.0f);
            mHeight = 60.0f;
        }
    } else {
        // the head's shapes are bound to the Head joint (their boxes are its own space), the eyelids are skinned (their box is the
        // bind pose's, in model space): the head's middle is where it is on FileSelectDataMario from its eyes (5 up, 15.5 back),
        // the head as tall as that one
        mCentre.set(0.0f, 120.0f, 0.0f);
        mHeight = 66.0f;
        for (u16 i = 0; names && i < data->getMaterialNum(); i++) {
            const char* n = names->getName(i);
            J3DShape* sh = data->getMaterialNodePointer(i)->getShape();
            if (n && sh && strncmp(n, "EyeLid", 6) == 0 && sh->mMax.y > sh->mMin.y) {
                mCentre.set((sh->mMin.x + sh->mMax.x) * 0.5f, (sh->mMin.y + sh->mMax.y) * 0.5f + 5.0f, (sh->mMin.z + sh->mMax.z) * 0.5f - 15.5f);
                break;
            }
        }
    }
    // (once, at the file select's start: what the heads measure, for the picture's calibration)
    mp_log("head3d: look=%d %s centre=(%.1f %.1f %.1f) height=%.1f mats=%d hidden=%x yellowMat=%d", mLook, d.arc, mCentre.x, mCentre.y,
           mCentre.z, mHeight, (int)data->getMaterialNum(), (unsigned)mHidden, mYellowMat);
    for (u16 i = 0; i < data->getMaterialNum(); i++) {
        J3DMaterial* mat = data->getMaterialNodePointer(i);
        J3DShape* sh = mat ? mat->getShape() : nullptr;
        if (!mat) continue;
        mp_log("head3d:   mat %d %s stages=%d xlu=%d shape=(%.1f %.1f %.1f)-(%.1f %.1f %.1f)", (int)i, names ? names->getName(i) : "?",
               (int)mat->getTevStageNum(), mat->isDrawModeOpaTexEdge() ? 1 : 0, sh ? sh->mMin.x : 0.0f, sh ? sh->mMin.y : 0.0f,
               sh ? sh->mMin.z : 0.0f, sh ? sh->mMax.x : 0.0f, sh ? sh->mMax.y : 0.0f, sh ? sh->mMax.z : 0.0f);
        J3DTevBlock* tev = mat->getTevBlock();
        for (u32 k = 0; tev && k < mat->getTevStageNum() && k < 4; k++) {
            const J3DTevOrder* o = tev->getTevOrder(k);
            const u8* st = reinterpret_cast<const u8*>(tev->getTevStage(k));
            if (o && st)
                mp_log("head3d:     stage %u order=(%d %d %d) tev=%02x %02x %02x %02x %02x %02x %02x %02x", (unsigned)k, o->mTexCoord, o->mTexMap,
                       o->mColorChan, st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7]);
        }
    }
}

// ------------------------------------------------------------------ per place: FileSelectItem's motion (MpHead3DMotion.hpp)
namespace mo = MpHead3DMotion;
namespace lk = MpHead3DLook;
struct Slot {
    int look = -1;
    bool wanted = false, wasWanted = false;
    Spec spec;                 // what the last show() said
    float shown = 0.0f;        // 0..1: pops in when the head comes, shrinks away when it goes
    bool arrivingPrev = false;
    int shake = -1;            // frames since the "no" shake began (-1: none)
    mo::Motion motion;
    mo::Blink blink;
    // the star pointer's last position over the head (square units), for a flick
    float px = 0, py = 0;
    bool pointerOn = false;
    // where the head is this frame (set by Place): its centre in the heads' camera space, the world width of its box, the size factor
    float cx = 0, cy = 0, cz = -2400.0f, worldW = 100.0f, factor = 1.0f;
};

HeadActor* sActors[kLooks];
bool sAlive = false;   // the heads of the file select scene exist
Slot sSlots[kSlots];
float sW = 810.6667f, sH = 456.0f;   // the picture in square units (MpPanelDraw::screen)

// desktop tests (screenshots): SMG_MP_TEST_HEAD_TAKEN="1:1,2:2,0:0m" shows look:owner heads as taken by that slot (the "m": the local
// player's), SMG_MP_TEST_HEAD_POP=<n> makes the pop-in n times slower (to catch a frame of it)
struct TestHooks {
    int owner[kLooks];
    bool mine[kLooks];
    float popSlow = 1.0f;
    TestHooks() {
        for (int i = 0; i < kLooks; i++) owner[i] = -1, mine[i] = false;
        const char* e = MP_TEST_GETENV("SMG_MP_TEST_HEAD_TAKEN");
        while (e && *e) {
            const int look = atoi(e);
            const char* c = strchr(e, ':');
            if (!c) break;
            const int o = atoi(c + 1);
            c++;
            while (*c >= '0' && *c <= '9') c++;
            const bool m = *c == 'm';
            if (look >= 0 && look < kLooks && o >= 0 && o < lk::kMaxOwner) owner[look] = o, mine[look] = m;
            e = strchr(c, ',');
            if (e) e++;
        }
        const char* p = MP_TEST_GETENV("SMG_MP_TEST_HEAD_POP");
        if (p && *p && atof(p) >= 1.0) popSlow = static_cast<float>(atof(p));
    }
};
const TestHooks& Hooks() {
    static const TestHooks h;
    return h;
}

HeadActor* ActorOf(const Slot& s) {
    if (!sAlive || s.look < 0 || s.look >= kLooks) return nullptr;
    HeadActor* a = sActors[s.look];
    return a && a->mOk ? a : nullptr;
}

float WorldPerSquare() { return kDepth * 2.0f * tanf(kFovY * 0.5f * kPi / 180.0f) / sH; }

int OpenTime() { return MR::getRandom(static_cast<s32>(180), static_cast<s32>(300)); }

void Act(HeadActor* a, mo::BlinkAct act) {
    if (!a || kDefs[a->mLook].kind != KIND_FILESELECT) return;   // (FileSelectModel's eyes: the Cosmic heads have none)
    switch (act) {
    case mo::ACT_NONE: break;
    case mo::ACT_NORMAL: MR::startAction(a, "normal"); break;
    case mo::ACT_BLINK: MR::startAction(a, "blink"); break;
    case mo::ACT_CLOSE: MR::startAction(a, "close"); break;
    case mo::ACT_OPEN: MR::startAction(a, "open"); break;
    }
}

// The star pointer (touch on the Vita) flicks the head it moves across, as on the file select
void Flick(Slot& s) {
    const float radius = s.spec.size * 0.5f * s.motion.grow;
    const TVec2f* p = MR::isStarPointerInScreen(0) ? MR::getStarPointerScreenPosition(0) : nullptr;
    const float sw = static_cast<float>(MR::getScreenWidth()), sh = static_cast<float>(MR::getScreenHeight());
    if (!p || sw <= 0.0f || sh <= 0.0f) {
        s.pointerOn = false;
        return;
    }
    const float x = p->x * (sW / sw), y = p->y * (sH / sh);
    const float dx = x - s.spec.x, dy = y - s.spec.y;
    const bool over = dx * dx + dy * dy < radius * radius;
    if (over && s.pointerOn) mo::flick(s.motion, (x - s.px) * (sw / sW));   // (screen pixels, as FileSelectItem counts them)
    s.px = x, s.py = y;
    s.pointerOn = over;
}

lk::State StateOf(const Slot& s) {
    lk::State st;
    st.look = s.look;
    st.owner = s.spec.owner;
    st.mine = s.spec.mine;
    st.dimmed = s.spec.dimmed;
    return st;
}

void Step(Slot& s, HeadActor* a) {
    mo::point(s.motion, s.spec.selected);
    s.motion.front = s.spec.restFacing || s.spec.selected;   // (the file select's heads never rest; a pointed one faces the front)
    Flick(s);
    const bool turning = mo::step(s.motion);
    if (s.shake >= 0) s.shake = lk::shaking(s.shake + 1) ? s.shake + 1 : -1;
    if (kDefs[a->mLook].kind != KIND_FILESELECT) return;   // (the Cosmic heads have no eye animation)
    Act(a, mo::blinkStep(s.blink, s.motion.spin, MR::isBtpStopped(a), turning, OpenTime()));
}

// The head's base matrix for this frame: its centre at the place's point, turned, scaled (the camera is at the origin, looking down -z)
void Place(Slot& s, HeadActor* a, bool appearing) {
    const float k = WorldPerSquare();
    const float pop = mo::pop(s.shown, appearing);
    s.factor = s.motion.grow * pop;   // (the file select's whole 1.2 when pointed)
    const float scale = mo::modelScale(s.spec.size, kFill, k, a->mHeight, s.motion.grow, pop) * kBalance[a->mLook];
    const float yaw = s.motion.rotY + lk::shakeYaw(s.shake);
    const float tx = (s.spec.x - sW * 0.5f) * k, ty = (sH * 0.5f - s.spec.y) * k, tz = -kDepth;
    s.cx = tx, s.cy = ty, s.cz = tz;
    s.worldW = s.spec.size * k * s.factor;
    Mtx r;
    PSMTXRotRad(r, 'y', yaw * kPi / 180.0f);
    // [R | t - R (scale c)]: the centre lands on t whatever the turn
    const float cx = a->mCentre.x * scale, cy = a->mCentre.y * scale, cz = a->mCentre.z * scale;
    r[0][3] = tx - (r[0][0] * cx + r[0][1] * cy + r[0][2] * cz);
    r[1][3] = ty - (r[1][0] * cx + r[1][1] * cy + r[1][2] * cz);
    r[2][3] = tz - (r[2][0] * cx + r[2][1] * cy + r[2][2] * cz);
    PSMTXCopy(r, a->mBase);
    a->mScale.set(scale, scale, scale);
}

// ------------------------------------------------------------------ drawing
enum Pass : uint8_t { PASS_SOLID = 0, PASS_DEPTH, PASS_BLEND };
struct DrawCtx {
    const HeadActor* actor;
    J3DModelData* data;
    Pass pass;
    GXColor mul, add;   // PASS_BLEND: colour = colour x mul + add, alpha = mul.a
    bool boost;         // a Cosmic head that is not a ghost: its colours doubled (the shimmer's dark blue, its rim, its starry lines read)
};
DrawCtx sCtx;

// Yellow Toad: the cap's red spot reads as yellow, the skin and the white stay: the 2D head's mask (MpPanelDraw SetToadStages) on
// the head's own texture, after the material's stages (a stage's output to C1 / C2 is the pixel's own: the material's stages before
// have read the registers already):
//   n      C2 = clamp((1/2 - texture G) x 4)          the mask: the spot (G = 0) 1, the skin (G about 0.7), white and greys 0
//   n + 1  C1 = texture (R, R, B) x lighting           the yellow head (swap table 2), lit as the material lights it
//   n + 2  PREV = mix(PREV, C1, C2)
// The texture is the one the material's first stage samples.
float StarsPerHead();
bool CosmicOwn();
constexpr int kYellowStages = 3;
void AppendYellow(int n, const J3DMaterial* mat) {
    J3DMaterial* m = const_cast<J3DMaterial*>(mat);
    J3DTevOrder* order = m->getTevBlock() ? m->getTevBlock()->getTevOrder(0) : nullptr;
    const GXTexCoordID coord = order ? static_cast<GXTexCoordID>(order->mTexCoord) : GX_TEXCOORD0;
    const GXTexMapID map = order ? static_cast<GXTexMapID>(order->mTexMap) : GX_TEXMAP0;
    const GXTevStageID a = static_cast<GXTevStageID>(n), b = static_cast<GXTevStageID>(n + 1), c = static_cast<GXTevStageID>(n + 2);
    GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_RED, GX_CH_RED, GX_CH_BLUE, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
    GXSetTevOrder(a, coord, map, GX_COLOR_NULL);
    GXSetTevDirect(a);
    GXSetTevSwapMode(a, GX_TEV_SWAP0, GX_TEV_SWAP3);
    GXSetTevKColorSel(a, GX_TEV_KCSEL_1_2);
    GXSetTevColorIn(a, GX_CC_TEXC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST);
    GXSetTevColorOp(a, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_4, GX_TRUE, GX_TEVREG2);
    GXSetTevAlphaIn(a, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GXSetTevAlphaOp(a, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevOrder(b, coord, map, GX_COLOR0A0);
    GXSetTevDirect(b);
    GXSetTevSwapMode(b, GX_TEV_SWAP0, GX_TEV_SWAP2);
    GXSetTevColorIn(b, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
    GXSetTevColorOp(b, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG1);
    GXSetTevAlphaIn(b, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GXSetTevAlphaOp(b, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevOrder(c, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXSetTevDirect(c);
    GXSetTevSwapMode(c, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXSetTevColorIn(c, GX_CC_CPREV, GX_CC_C1, GX_CC_C2, GX_CC_ZERO);
    GXSetTevColorOp(c, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(c, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GXSetTevAlphaOp(c, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetNumTevStages(static_cast<u8>(n + kYellowStages));
}

// The Cosmic heads' own starry skin. The game's Cosmic materials (GhostMario) map their starfield through the VIEW (a projection of the scene
// camera: the stars slide over the figure as it moves); on a head a tenth of the picture wide that mapping puts the whole head on a couple of
// texels and it reads as a flat blue blob. Same textures (the materials maps 0 and 2: BackG, the starfield, and BackGStar, its glints), same
// scroll, mapped from the model's own positions so the stars stay with the head as it turns:
//   0  PREV = BackG (the material's own light channel is black: its light feeds only the rim, stage 3 of the game's material)     1  PREV += BackGStar (the glints)
constexpr int kCosmicStages = 2;
void AppendCosmic() {
    const float k = 1.0f / 60.0f * StarsPerHead();
    const float t = static_cast<float>(sTick) * 0.0016f;
    Mtx a = {{k, 0.0f, 0.0f, 0.5f + t}, {0.0f, k, 0.0f, 0.5f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    Mtx b = {{k * 2.3f, 0.0f, 0.0f, 0.1f}, {0.0f, k * 2.3f, 0.0f, 0.3f - t * 1.7f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    GXLoadTexMtxImm(a, GX_TEXMTX8, GX_MTX2x4);
    GXLoadTexMtxImm(b, GX_TEXMTX7, GX_MTX2x4);
    GXSetNumTexGens(2);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_POS, GX_TEXMTX8, GX_FALSE, GX_PTIDENTITY);
    GXSetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_POS, GX_TEXMTX7, GX_FALSE, GX_PTIDENTITY);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);   // (the material's channel is black here: its light is for the rim only)
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP2, GX_COLOR_NULL);
    GXSetTevDirect(GX_TEVSTAGE1);
    GXSetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_ONE, GX_CC_TEXC, GX_CC_CPREV);
    GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetNumTevStages(kCosmicStages);
}

// PREV = PREV x 2 (one stage after the material's: the Cosmic material alone reads as a flat dark blue at the menu's size)
void AppendBoost(int n) {
    const GXTevStageID a = static_cast<GXTevStageID>(n);
    GXSetTevOrder(a, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXSetTevDirect(a);
    GXSetTevSwapMode(a, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXSetTevColorIn(a, GX_CC_ZERO, GX_CC_CPREV, GX_CC_ONE, GX_CC_ZERO);
    GXSetTevColorOp(a, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(a, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GXSetTevAlphaOp(a, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetNumTevStages(static_cast<u8>(n + 1));
}

void AppendWash(int n, const GXColor& mul, const GXColor& add) {
    const GXTevStageID a = static_cast<GXTevStageID>(n), b = static_cast<GXTevStageID>(n + 1);
    GXSetTevKColor(GX_KCOLOR3, mul);
    GXSetTevKColor(GX_KCOLOR2, add);
    // n: PREV = PREV x K3; alpha = K3.a (the layout's alpha: the ghost's breathing, the menu's fade)
    GXSetTevOrder(a, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXSetTevDirect(a);
    GXSetTevSwapMode(a, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXSetTevKColorSel(a, GX_TEV_KCSEL_K3);
    GXSetTevKAlphaSel(a, GX_TEV_KASEL_K3_A);
    GXSetTevColorIn(a, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
    GXSetTevColorOp(a, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(a, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(a, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    // n + 1: PREV += K2 (the pale blue the ghost's colours are lifted to)
    GXSetTevOrder(b, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXSetTevDirect(b);
    GXSetTevSwapMode(b, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXSetTevKColorSel(b, GX_TEV_KCSEL_K2);
    GXSetTevColorIn(b, GX_CC_KONST, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
    GXSetTevColorOp(b, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(b, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GXSetTevAlphaOp(b, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetNumTevStages(static_cast<u8>(n + 2));
}

// The GX library writes some registers whole from its own copy of the state (GEN_MODE: texgens, channels, indirect and TEV stage
// counts, cull mode; TREF: two stages' orders; KSEL: two stages' konst selections), and the material's lists, raw register writes,
// never told it theirs: before a GX call of ours rewrites one of those, its copy is set to what the material holds, or the
// material's last stage (paired with our first one) and its counts would be replaced by stale values.
void SyncShadow(J3DMaterial* mat, int n) {
    if (J3DTexGenBlock* g = mat->getTexGenBlock()) GXSetNumTexGens(static_cast<u8>(g->getTexGenNum()));
    if (J3DColorBlock* c = mat->getColorBlock()) {
        GXSetNumChans(c->getColorChanNum());
        GXSetCullMode(static_cast<GXCullMode>(c->getCullMode()));
    }
    if (J3DIndBlock* ind = mat->getIndBlock()) GXSetNumIndStages(ind->getIndTexStageNum());
    J3DTevBlock* tev = mat->getTevBlock();
    if (tev && (n & 1) && n >= 1) {   // (our first stage shares TREF and KSEL with the material's last)
        const GXTevStageID last = static_cast<GXTevStageID>(n - 1);
        if (const J3DTevOrder* o = tev->getTevOrder(n - 1))
            GXSetTevOrder(last, static_cast<GXTexCoordID>(o->mTexCoord), static_cast<GXTexMapID>(o->mTexMap),
                          static_cast<GXChannelID>(o->mColorChan));
        GXSetTevKColorSel(last, static_cast<GXTevKColorSel>(tev->getTevKColorSel(n - 1)));
        GXSetTevKAlphaSel(last, static_cast<GXTevKAlphaSel>(tev->getTevKAlphaSel(n - 1)));
    }
}

// After a material's lists, just before its shape: what this pass changes
void Inject(u16 matIndex) {
    const DrawCtx& c = sCtx;
    J3DMaterial* mat = c.data->getMaterialNodePointer(matIndex);
    if (!mat) return;
    int n = mat->getTevStageNum();
    const bool yellow = c.actor->mYellowMat == static_cast<int>(matIndex);
    const bool boost = c.boost && c.pass != PASS_DEPTH && n + 1 + (c.pass == PASS_BLEND ? 2 : 0) <= 16;
    if (yellow || boost || c.pass == PASS_BLEND) SyncShadow(mat, n);
    if (yellow && n + kYellowStages + 2 <= 16) {
        AppendYellow(n, mat);
        n += kYellowStages;
    }
    if (boost) {
        if (CosmicOwn()) {
            AppendCosmic();
            n = kCosmicStages;
        } else {
            AppendBoost(n);
            n += 1;
        }
    }
    switch (c.pass) {
    case PASS_SOLID: break;
    case PASS_DEPTH:   // the nearest surface only: depth, no colour (so the ghost blends once, not its inside too)
        GXSetColorUpdate(GX_FALSE);
        GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
        break;
    case PASS_BLEND:
        if (n + 2 <= 16) AppendWash(n, c.mul, c.add);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GXSetZCompLoc(GX_TRUE);
        GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        GXSetColorUpdate(GX_TRUE);
        break;
    }
    GXSetAlphaUpdate(GX_FALSE);
}

void InjectX(void*, u16 matIndex) { Inject(matIndex); }

// A FileSelectData head (a plain J3DModel): every material's list, then its shape (DrawBufferShapeDrawer::draw for one model),
// opaque materials first
void DrawPlain(J3DModel* model) {
    J3DModelData* data = model->getModelData();
    j3dSys.setModel(model);
    if (model->checkFlag(J3DMdlFlag_SkinPosCpu)) j3dSys.onFlag(J3DSysFlag_SkinPosCpu);
    else j3dSys.offFlag(J3DSysFlag_SkinPosCpu);
    if (model->checkFlag(J3DMdlFlag_SkinNrmCpu)) j3dSys.onFlag(J3DSysFlag_SkinNrmCpu);
    else j3dSys.offFlag(J3DSysFlag_SkinNrmCpu);
    data->syncJ3DSysFlags();
    j3dSys.setTexture(data->getTexture());
    J3DShape::resetVcdVatCache();
    for (int xlu = 0; xlu < 2; xlu++) {
        for (u16 i = 0; i < data->getMaterialNum(); i++) {
            J3DMaterial* mat = data->getMaterialNodePointer(i);
            if (!mat || !mat->getShape() || mat->getShape()->checkFlag(1)) continue;
            if ((mat->isDrawModeOpaTexEdge() ? 1 : 0) != xlu) continue;
            J3DMatPacket* mp = model->getMatPacket(i);
            J3DShapePacket* sp = model->getShapePacket(mat->getShape()->getIndex());
            if (!mp || !sp || !mp->getDisplayListObj()) continue;
            j3dSys.setMatPacket(mp);
            mp->getDisplayListObj()->callDL();
            sp->getShape()->loadPreDrawSetting();
            if (ShapePacketUserData* u = MR::getJ3DShapePacketUserData(sp)) u->callDL();
            if (sp->getDisplayListObj()) sp->getDisplayListObj()->callDL();
            Inject(i);
            sp->drawFast();
        }
    }
    J3DShape::resetVcdVatCache();
}

// A GhostMario / GhostLuigi head (J3DModelX, as GhostPlayer::draw): the body's and hands' shapes hidden for this draw, the pass's
// state through the model's per-material hook
void DrawGhost(HeadActor* a, J3DModelX* model) {
    J3DModelData* data = model->getModelData();
    u32 saved[32];
    const int n = data->getMaterialNum() < 32 ? data->getMaterialNum() : 32;
    for (int i = 0; i < n; i++) {
        J3DShape* sh = data->getMaterialNodePointer(static_cast<u16>(i))->getShape();
        saved[i] = sh ? sh->mFlags : 0;
        if (sh && (a->mHidden & (1u << i))) sh->mFlags |= 1u;
    }
    void (*hook)(void*, u16) = model->_120;
    void* hookData = model->_128;
    model->_120 = InjectX;
    model->_128 = nullptr;
    model->mFlags.clear();
    model->directDraw(nullptr);
    model->_120 = hook;
    model->_128 = hookData;
    for (int i = 0; i < n; i++) {
        J3DShape* sh = data->getMaterialNodePointer(static_cast<u16>(i))->getShape();
        if (sh) sh->mFlags = saved[i];
    }
}

void DrawModel(HeadActor* a, Pass pass, const GXColor& mul, const GXColor& add, bool boost) {
    J3DModel* model = MR::getJ3DModel(a);
    sCtx.actor = a;
    sCtx.data = model->getModelData();
    sCtx.pass = pass;
    sCtx.mul = mul;
    sCtx.add = add;
    sCtx.boost = boost;
    if (kDefs[a->mLook].kind == KIND_GHOST) DrawGhost(a, static_cast<J3DModelX*>(model));
    else DrawPlain(model);
}

// The scene's depth is of no use to the heads: cleared to the far plane where they are drawn (no colour written)
void ClearDepth(float x0, float y0, float x1, float y1) {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumIndStages(0);
    GXSetNumTevStages(1);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GXSetColorUpdate(GX_FALSE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetZCompLoc(GX_TRUE);
    GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    GXSetCullMode(GX_CULL_NONE);
    Mtx id;
    PSMTXIdentity(id);
    GXLoadPosMtxImm(id, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    // the rectangle (square units) at nearly the far plane
    const float z = -kFar * 0.97f, k = -z * 2.0f * tanf(kFovY * 0.5f * kPi / 180.0f) / sH;
    const float ax = (x0 - sW * 0.5f) * k, bx = (x1 - sW * 0.5f) * k, ay = (sH * 0.5f - y0) * k, by = (sH * 0.5f - y1) * k;
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 6);
    GXPosition3f32(ax, ay, z), GXPosition3f32(bx, ay, z), GXPosition3f32(bx, by, z);
    GXPosition3f32(ax, ay, z), GXPosition3f32(bx, by, z), GXPosition3f32(ax, by, z);
    GXEnd();
    GXSetColorUpdate(GX_TRUE);
}


// The Cosmic heads' starry window. GhostMario's materials map their stars through the VIEW (J3DTexMtxMode_ViewProjmap): the game's
// ViewProjmapEffectMtxSetter hands them the scene camera's projection every frame, so Cosmic Mario is a window onto a starfield that
// slides over him as he moves. Here the camera is the heads' own and a head is a tenth of the picture wide: with the game's
// projection the whole head sits on a couple of texels, a flat blue blob (what the first heads looked like). Every such texture matrix
// gets an effect matrix of its own: the head's box covers kStarRepeats repeats of the main starfield layer (the glint layer keeps the
// game's own ratio to it), centred on the head so the stars stay with it as it turns and moves; the shimmer (the btk's scroll) runs
// as in the game.
constexpr float kStarRepeatsDefault = 0.5f;
// (desktop test: SMG_MP_TEST_COSMIC="<repeats>,<boost 0|1>" to tune the look in one run)
float StarRepeats() {
    static const float v = [] {
        const char* e = MP_TEST_GETENV("SMG_MP_TEST_COSMIC");
        return e && *e && atof(e) > 0.0 ? static_cast<float>(atof(e)) : kStarRepeatsDefault;
    }();
    return v;
}
float StarsPerHead() {
    static const float v = [] {
        const char* e = MP_TEST_GETENV("SMG_MP_TEST_STARS");
        return e && *e && atof(e) > 0.0 ? static_cast<float>(atof(e)) : 1.6f;
    }();
    return v;
}
bool CosmicOwn() {
    static const bool v = [] {
        const char* e = MP_TEST_GETENV("SMG_MP_TEST_STARS");
        return !(e && *e && atof(e) < 0.0);   // (negative: the game's own mapping, for comparison)
    }();
    return v;
}
bool CosmicBoost() {
    static const bool v = [] {
        const char* e = MP_TEST_GETENV("SMG_MP_TEST_COSMIC");
        const char* c = e ? strchr(e, ',') : nullptr;
        return c ? atoi(c + 1) != 0 : true;
    }();
    return v;
}
void CosmicWindow(HeadActor* a, const Slot& s) {
    J3DModel* model = MR::getJ3DModel(a);
    if (!model) return;
    J3DModelData* data = model->getModelData();
    float base = 0.0f;
    for (u16 i = 0; i < data->getMaterialNum(); i++) {
        J3DMaterial* mat = data->getMaterialNodePointer(i);
        if (!mat) continue;
        for (u32 j = 0; j < 8; j++) {
            J3DTexMtx* tm = mat->getTexMtx(j);
            if (!tm || (tm->getTexMtxInfo().mInfo & 0x3Fu) != J3DTexMtxMode_ViewProjmap) continue;
            J3DTexMtxInfo& info = tm->getTexMtxInfo();
            if (base <= 0.0f) base = info.mSRT.mScaleX;   // (the first one: the starfield layer)
            if (!(base > 0.0f)) return;
            const float q = -s.cz;
            const float gain = 2.0f * StarRepeats() * q / (base * (s.worldW > 1.0f ? s.worldW : 1.0f));
            Mtx e;
            e[0][0] = gain, e[0][1] = 0.0f, e[0][2] = gain * s.cx / q, e[0][3] = 0.0f;
            e[1][0] = 0.0f, e[1][1] = gain, e[1][2] = gain * s.cy / q, e[1][3] = 0.0f;
            e[2][0] = 0.0f, e[2][1] = 0.0f, e[2][2] = -1.0f, e[2][3] = 0.0f;
            tm->setEffectMtx(e);
        }
    }
}

}  // namespace

// ------------------------------------------------------------------ the badges
// The owner's badge ("2P"): a pill in the character's colour with a white rim and the message font's two letters with the soft shadow,
// the look of the panels' player plates (MpPanelPlan: "2P Luigi64"). Immediate triangles through MpPanelDraw::drawMini.
constexpr int kPillSeg = 6;   // segments per end cap
int PillOutline(float x0, float y0, float x1, float y1, float grow, float* px, float* py) {
    const float r = (y1 - y0) * 0.5f + grow;
    const float cy = (y0 + y1) * 0.5f;
    int n = 0;
    for (int cap = 0; cap < 2; cap++) {
        const float cxm = cap == 0 ? x1 - (y1 - y0) * 0.5f : x0 + (y1 - y0) * 0.5f;   // right cap first, then left
        const float a0 = cap == 0 ? -0.5f * kPi : 0.5f * kPi;
        for (int i = 0; i <= kPillSeg; i++) {
            const float a = a0 + kPi * static_cast<float>(i) / static_cast<float>(kPillSeg);
            px[n] = cxm + r * cosf(a);
            py[n] = cy + r * sinf(a);
            n++;
        }
    }
    return n;
}
MpPanel::Rgba Fade(MpPanel::Rgba c, float a) {
    float v = static_cast<float>(c.a) * a;
    v = v < 0 ? 0 : (v > 255 ? 255 : v);
    c.a = static_cast<uint8_t>(v + 0.5f);
    return c;
}
// One filled pill into `v` (triangles); the colour runs from `top` to `bottom`. Returns the vertex count written.
int FillPill(MpPanel::Vertex* v, int cap, float x0, float y0, float x1, float y1, float grow, MpPanel::Rgba top, MpPanel::Rgba bottom) {
    float px[2 * (kPillSeg + 1)], py[2 * (kPillSeg + 1)];
    const int n = PillOutline(x0, y0, x1, y1, grow, px, py);
    if (3 * n > cap) return 0;
    const float h = y1 - y0 + 2.0f * grow;
    auto at = [&](float x, float y) {
        float t = h > 0 ? (y - (y0 - grow)) / h : 0;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        auto m = [t](uint8_t a, uint8_t b) { return static_cast<uint8_t>(a + (b - a) * t + 0.5f); };
        return MpPanel::Vertex{x, y, {m(top.r, bottom.r), m(top.g, bottom.g), m(top.b, bottom.b), m(top.a, bottom.a)}};
    };
    const MpPanel::Vertex c = at((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
    int w = 0;
    for (int i = 0; i < n; i++) {
        v[w++] = c;
        v[w++] = at(px[i], py[i]);
        v[w++] = at(px[(i + 1) % n], py[(i + 1) % n]);
    }
    return w;
}

float FontAdvance(const nw4r::ut::Font* f, const char* s, float scale) {
    float w = 0;
    for (; *s; ++s) w += static_cast<float>(f->GetCharWidth(static_cast<uint16_t>(*s))) * scale;
    return w;
}

void DrawBadge(const lk::Badge& b) {
    if (!b.shown || b.alpha <= 0.01f) return;
    const nw4r::ut::Font* font = MR::getFontOnCurrentLanguage();
    if (!font) return;
    using MpPanel::Rgba;
    MpPanel::Vertex v[3 * 2 * (kPillSeg + 1) * 3];   // rim + body + the dark outline, each a fan of 2 * (kPillSeg + 1) triangles
    int n = 0;
    const Rgba edge = {b.edge.r, b.edge.g, b.edge.b, 255};
    const Rgba fill = {b.fill.r, b.fill.g, b.fill.b, 255};
    const Rgba fillLow = {static_cast<uint8_t>(b.fill.r * 7 / 10), static_cast<uint8_t>(b.fill.g * 7 / 10), static_cast<uint8_t>(b.fill.b * 7 / 10), 255};
    const Rgba white = {255, 255, 255, 235};
    const float ol = b.ring * 0.6f;   // the dark outline outside the white rim
    n += FillPill(v + n, static_cast<int>(sizeof v / sizeof v[0]) - n, b.x0, b.y0, b.x1, b.y1, b.ring + ol, Fade(edge, b.alpha * 0.9f), Fade(edge, b.alpha * 0.9f));
    n += FillPill(v + n, static_cast<int>(sizeof v / sizeof v[0]) - n, b.x0, b.y0, b.x1, b.y1, b.ring, Fade(white, b.alpha), Fade(white, b.alpha));
    n += FillPill(v + n, static_cast<int>(sizeof v / sizeof v[0]) - n, b.x0, b.y0, b.x1, b.y1, 0.0f, Fade(fill, b.alpha), Fade(fillLow, b.alpha));
    // the letters (the plates' words: white, the soft shadow): the middle of the capitals at the pill's middle
    const float height = static_cast<float>(font->GetHeight());
    const float scale = height > 0 ? b.textSize / height : 1.0f;
    const float w = FontAdvance(font, b.text, scale);
    const float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;
    const float top = cy - 17.4f * (height / 33.0f) * scale;
    char16_t text[3] = {static_cast<char16_t>(b.text[0]), static_cast<char16_t>(b.text[1]), 0};
    MpPanel::TextRun runs[2];
    runs[0] = {MpPanel::FONT_MESSAGE, cx - w * 0.5f + b.textSize * 0.07f, top + b.textSize * 0.1f, scale, scale, Fade(Rgba{6, 8, 40, 170}, b.alpha), 0, 2};
    runs[1] = {MpPanel::FONT_MESSAGE, cx - w * 0.5f, top, scale, scale, Fade(Rgba{255, 255, 255, 255}, b.alpha), 0, 2};
    MpPanelDraw::drawMini(v, n, runs, 2, text, 2);
}

// ------------------------------------------------------------------ the scene
// memfix: nothing is mounted or created for the file select scene any more (it was GhostMario + GhostLuigi, about 318 KB of archive,
// and six head actors, in every file select, for every player, even in single player). `createForFileSelect` only arms the module;
// the first character select that asks for a head (show) mounts the two Ghost archives into the scene heap and makes the actors,
// once per scene, when the scene heap has room. A failure leaves the layouts' own 2D heads (drawsSlot() is false).
void addArchives(void*) {}

namespace {
bool sArmed = false, sTried = false;

bool MountGhost(const char* arc) {
    if (MR::isLoadedObjectOrLayoutArchive(arc)) return true;
    char path[128];
    if (!MR::makeObjectArchiveFileNameFromPrefix(path, sizeof path, arc, false)) return false;
    return MR::mountArchive(path, MR::getSceneHeapNapa()) != nullptr && MR::isLoadedObjectOrLayoutArchive(arc);
}

void MakeActors() {
    sTried = true;
    JKRHeap* napa = MR::getSceneHeapNapa();
    const s32 before = napa ? napa->getTotalFreeSize() : 0;
    if (before < 0x400000) {
        mp_log("mem head3d REFUSED napa-free=%d: the character select keeps its 2D heads", (int)before);
        return;
    }
    if (!MpDropIn::objSlotsOk(MpPuppets::nameObjSlotsLeft())) {
        mp_log("mem head3d REFUSED: the scene's object table is nearly full: the character select keeps its 2D heads");
        return;
    }
    if (!MountGhost("GhostMario") || !MountGhost("GhostLuigi")) {
        mp_log("mem head3d archives not mounted: the character select keeps its 2D heads");
        return;
    }
    // integ: the heads belong to the SCENE heap (freed with the scene): this runs on the main thread mid-menu, where the current heap is
    // whatever the game left; without the switch they leaked on every file-select visit
    JKRHeap* previous = MR::getCurrentHeap();
    if (previous != napa) MR::becomeCurrentHeap(napa);
    int made = 0;
    for (int i = 0; i < kLooks; i++) {
        sActors[i] = nullptr;
        if (!ArchiveThere(i)) {   // (review2 #8: never a model manager on a missing archive; no actor at all: that character keeps its 2D head)
            mp_log("head3d: look=%d %s: archive not loaded, the 2D head is used", i, kDefs[i].arc);
            continue;
        }
        HeadActor* a = new HeadActor(i);
        a->initWithoutIter();
        sActors[i] = a;   // (ActorOf() hands out only actors whose init finished: mOk)
        if (a->mOk) made++;
    }
    if (previous != napa) MR::becomeCurrentHeap(previous);
    sAlive = true;
    mp_log("mem head3d heads made=%d of %d napa-free=%d->%d, %d places of %d bytes", made, kLooks, (int)before, (int)napa->getTotalFreeSize(), kSlots, static_cast<int>(sizeof(Slot)));
}
}  // namespace

void createForFileSelect() {
    sceneEnd();
    sArmed = MpUi::featureEnabled();
}

void sceneEnd() {
    sArmed = sTried = false;
    for (auto& a : sActors) a = nullptr;
    for (auto& s : sSlots) s = Slot();
    sAlive = false;
}

// ------------------------------------------------------------------ a frame
void beginFrame() {
    ++sTick;
    for (auto& s : sSlots) {
        s.wasWanted = s.wanted;
        s.wanted = false;
    }
}

void show(int slot, const Spec& in) {
    if (sArmed && !sTried && !sAlive) MakeActors();   // memfix: the first head the character select asks for
    if (!sAlive || slot < 0 || slot >= kSlots || in.look < 0 || in.look >= kLooks) return;
    Spec sp = in;
    // (desktop test: SMG_MP_TEST_HEAD_TAKEN)
    const TestHooks& th = Hooks();
    if (th.owner[sp.look] >= 0) sp.owner = th.owner[sp.look], sp.mine = th.mine[sp.look];
    if (!(sp.size > 0.0f)) return;
    Slot& s = sSlots[slot];
    bool fresh = false;
    if (s.look != sp.look) {   // (another character in this place: its own start)
        s = Slot();
        s.look = sp.look;
        s.motion.rotY = static_cast<float>((slot * 61) % 360);   // (each head its own angle: they do not turn in step)
        fresh = true;
    }
    if (sp.arriving && (!s.arrivingPrev || fresh)) {   // a player joined: the head pops in from nothing, with the game's sound
        s.shown = 0.0f;
        s.wasWanted = false;
        MR::startSystemSE("SE_SY_ITEM_APPEAR", -1, -1);
    }
    s.arrivingPrev = sp.arriving;
    s.wanted = !sp.leaving;
    s.spec = sp;
}

void shake(int slot) {
    if (!sAlive || slot < 0 || slot >= kSlots || sSlots[slot].look < 0) return;
    if (sSlots[slot].shake < 0) sSlots[slot].shake = 0;
}

void run() {
    if (!sAlive) return;
    const MpPanel::Screen scr = MpPanelDraw::screen();
    sW = scr.width, sH = scr.height;
    bool moved[kLooks] = {};
    const bool pressed = MR::testCorePadTriggerA(0);
    for (auto& s : sSlots) {
        HeadActor* a = ActorOf(s);
        if (!a) continue;
        if (s.wanted && !s.wasWanted && s.shown <= 0.0f) {   // the head comes: facing the front, the eyes open
            const float keep = s.motion.rotY;
            mo::reset(s.motion, s.spec.selected);
            s.motion.rotY = keep;
            mo::blinkEnter(s.blink, mo::BLINK_OPEN, OpenTime());
            Act(a, mo::ACT_NORMAL);
        }
        // choosing a head another player has: it shakes its head (the buzz is the menu's own)
        if (pressed && s.wanted && s.spec.selected && lk::tag(StateOf(s)) == lk::TAG_TAKEN && s.shake < 0) s.shake = 0;
        const float step = Hooks().popSlow;
        s.shown += s.wanted ? 1.0f / (14.0f * step) : -1.0f / (8.0f * step);
        s.shown = s.shown < 0.0f ? 0.0f : (s.shown > 1.0f ? 1.0f : s.shown);
        if (s.shown <= 0.0f) continue;
        Step(s, a);
        if (!moved[a->mLook]) {   // (the animations move once a frame however many places show this character)
            moved[a->mLook] = true;
            a->movement();
        }
    }
}

bool drawsSlot(int slot) {
    if (slot < 0 || slot >= kSlots) return false;
    return ActorOf(sSlots[slot]) != nullptr;   // (also after it shrank away: the layout's own head is never drawn over it)
}

bool anyShown() {
    if (!sAlive) return false;
    for (const auto& s : sSlots)
        if (s.shown > 0.0f && ActorOf(s)) return true;
    return false;
}

int debugScale(int slot) {
    if (slot < 0 || slot >= kSlots || !ActorOf(sSlots[slot]) || sSlots[slot].shown <= 0.0f) return 0;
    return static_cast<int>(sSlots[slot].factor * 100.0f + 0.5f);
}

void draw() {
    if (!anyShown()) return;
    // the area the heads take (with room for their growth and flicks): its depth cleared
    float x0 = sW, y0 = sH, x1 = 0.0f, y1 = 0.0f;
    for (const auto& s : sSlots) {
        if (s.shown <= 0.0f || !ActorOf(s)) continue;
        const float r = s.spec.size * 0.75f;
        x0 = s.spec.x - r < x0 ? s.spec.x - r : x0, x1 = s.spec.x + r > x1 ? s.spec.x + r : x1;
        y0 = s.spec.y - r < y0 ? s.spec.y - r : y0, y1 = s.spec.y + r > y1 ? s.spec.y + r : y1;
    }
    f32 savedProj[7];
    GXGetProjectionv(savedProj);   // (review2 #10: the 2D pass's projection comes back at the end)
    MR::reinitGX();
    Mtx44 proj;
    C_MTXPerspective(proj, kFovY, sW / sH, kNear, kFar);
    GXSetProjection(proj, GX_PERSPECTIVE);
    ClearDepth(x0, y0, x1, y1);
    Mtx savedView;
    PSMTXCopy(j3dSys.mViewMtx, savedView);
    PSMTXIdentity(j3dSys.mViewMtx);
    J3DShape::resetVcdVatCache();
    for (auto& s : sSlots) {
        HeadActor* a = ActorOf(s);
        if (!a || s.shown <= 0.0f) continue;
        // (placed here, not in run(): two places of one character share its model, each is put where it goes just before it is drawn)
        Place(s, a, s.wanted);
        a->calcAnim();
        J3DModel* model = MR::getJ3DModel(a);
        if (kDefs[a->mLook].kind == KIND_GHOST) {
            static_cast<J3DModelX*>(model)->viewCalc2();
            CosmicWindow(a, s);
        } else {
            model->viewCalc();
        }
        GXInvalidateVtxCache();
        // the light each is drawn with in its own scene: the file select's heads their actor light, the Cosmic ones the player's
        // (GhostPlayer draws in DrawType_Player, after SceneFunction loads the player light)
        if (kDefs[a->mLook].kind == KIND_GHOST) MR::loadLightPlayer();
        else MR::loadActorLight(a);
        const float fade = lk::alpha(StateOf(s), s.spec.alpha);
        const u8 alpha = static_cast<u8>(fade <= 0.0f ? 0 : (fade >= 1.0f ? 255 : fade * 255.0f + 0.5f));
        const bool cosmic = kDefs[a->mLook].kind == KIND_GHOST && CosmicBoost();
        if (alpha >= 250) {
            DrawModel(a, PASS_SOLID, GXColor{255, 255, 255, 255}, GXColor{0, 0, 0, 0}, cosmic);
            continue;
        }
        // dimmed, or fading with the menu: its depth, then its colour blended over
        const GXColor mul = {255, 255, 255, alpha};
        const GXColor add = {0, 0, 0, 0};
        DrawModel(a, PASS_DEPTH, mul, add, false);
        DrawModel(a, PASS_BLEND, mul, add, cosmic);
    }
    PSMTXCopy(savedView, j3dSys.mViewMtx);
    // the 2D pass's state again, the way the game's own 3D-in-2D draws end (MR::drawInitFor2DModel / clearZBuffer): the models left
    // TEV KCOLORs, swap tables, indirect and tev stages, a position matrix and the perspective projection behind. reinitGX puts back
    // the defaults of all of those (J3DSys::reinitTevStages: KCOLOR0-3, swap tables), the projection is the one saved above (the layouts
    // and MpPanelDraw::DrawPlan set the rest themselves)
    MR::reinitGX();
    GXSetProjectionv(savedProj);
    Mtx idMtx;
    PSMTXIdentity(idMtx);
    GXLoadPosMtxImm(idMtx, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    J3DShape::resetVcdVatCache();
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetZCompLoc(GX_TRUE);
    GXSetZMode(GX_FALSE, GX_NEVER, GX_FALSE);
    GXSetCullMode(GX_CULL_NONE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXSetNumIndStages(0);
    // the badges over the heads (all heads first: a grown head never covers a neighbour's badge)
    for (const auto& s : sSlots) {
        if (!ActorOf(s) || s.shown <= 0.0f) continue;
        const float f = mo::pop(s.shown, s.wanted) * (1.0f + (s.motion.grow - 1.0f));
        DrawBadge(lk::badge(StateOf(s), s.spec.x, s.spec.y, s.spec.size, f, s.spec.alpha));
    }
}

}  // namespace MpHead3D
