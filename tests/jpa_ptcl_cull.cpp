// GXV_HOTPROF_PTCLCULL (hotprof25, 11 Nov): host check of the particle quads left out on the game side (decomp/src/JSystem/JParticle/
// JPAResource.cpp port_jpa_ptcl_cull, platform/include/port/ptcl_cull.h). The real JPA units run (JPAResource with its particle loops,
// JPABaseShape with the quad functions JPADrawBillboard / JPADrawRotBillboard / JPADrawRotation and the projection-texture loads); GX is a
// recorder (tests/jpa_ptcl_cull_stubs.cpp): the registers a quad's picture depends on (position matrix 0, texture matrix 0, the TEV
// colours, the position array) and every list call with a copy of them.
// Each scenario runs from the same seed with the switch off, on and dry, a few hundred turns, a camera that turns and moves so that the
// emitter is in view, half in view, behind and beside it. Checked:
//   * on: the list calls are those of the off run with some left out, each of the others with exactly the same registers;
//   * every one left out is a quad the front end's own test drops: the reference here is vgx_draw.cpp's PointOutMask loop (copied, the
//     GXV_FE24_CLIP=0 form) over the vertices decoded from the array, through the clip volume vgx_draw.cpp's ClipNowOfCalc makes from the
//     REGISTERS the GX library writes for the projection, the viewport and the scissor (computed here as GXTransform.c computes them);
//   * the registers after every emitter draw are the same in the three runs (the owed loads at the end of a loop);
//   * dry: the same list calls as off; quads are left out at all, and an emitter with a particle callback is left alone.
// Build and run: bash tools/test_jpa_ptcl_cull.sh
#include "JSystem/JParticle/JPABaseShape.hpp"
#include "JSystem/JParticle/JPAChildShape.hpp"
#include "JSystem/JParticle/JPADynamicsBlock.hpp"
#include "JSystem/JParticle/JPAEmitter.hpp"
#include "JSystem/JParticle/JPAEmitterManager.hpp"
#include "JSystem/JParticle/JPAExtraShape.hpp"
#include "JSystem/JParticle/JPAParticle.hpp"
#include "JSystem/JParticle/JPAResource.hpp"
#include "JSystem/JParticle/JPAResourceManager.hpp"
#include <cstdarg>
#include <algorithm>
#include <cmath>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

// ---- the switches (platform/src/port/perf.cpp in the game) ----
static int gMode = 0;
extern "C" int port_hotprof_ptclcull_mode(void) {
    return gMode;
}
// (perf32: GXV_PTCL_PRE, the centre test; every scenario is run once more with it on, and once in its verify mode)
static int gPre = 0;
extern "C" int port_ptcl_pre_mode(void) {
    return gPre;
}
extern "C" void port_ptcl_pre_counts(unsigned long long out[2]);
extern "C" int port_jpa_look_on(void) {
    return 0;
}
extern "C" void port_logic_count(int, unsigned) {
}
// (JPA_TEST_CENSUS=1: GXV_HOTPROF_PTCLCENSUS on and its lines printed; the results must be the same: the census only counts)
extern "C" int port_hotprof_ptclcensus_on(void) {
    static const int on = getenv("JPA_TEST_CENSUS") != nullptr;
    return on;
}
extern "C" const char* port_particle_name(unsigned usrIdx) {
    return (usrIdx & 1) ? "OddResource" : nullptr;
}
extern "C" void port_log_summary(const char* fmt, ...) {
    if (getenv("JPA_TEST_CENSUS") == nullptr) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
// (other switches the JPA units ask about: all off; the particle pool as JPAEmitterManager.cpp's serial path has it)
bool gJpaLookPar = false;
extern "C" int port_ptcl_lookpar_mode(void) {
    return 0;
}
extern "C" int port_jpa_look_pure_mode(void) {
    return 0;
}
extern "C" void port_ptcl_lookpar_note(unsigned, unsigned) {
}
extern "C" float port_pb_ptcl_turn_factor(void) {
    return 1.0f;
}
extern "C" float port_pb_ptcl_scale_at(float, float, float) {
    return 1.0f;
}
extern "C" int port_gx_fx_begin(void) {  // (GXV_FXMERGE off: the loop's calls go out one by one)
    return 0;
}
extern "C" void port_gx_fx_end(void) {
}
extern "C" {
float g_port_boss_ptcl_scale = 1.0f;
float g_port_boss_ptcl_grow = 1.0f;
}
JPANode< JPABaseParticle >* jpaPoolPop(JPAList< JPABaseParticle >* pool) {
    return pool->pop_front();
}
void jpaPoolPush(JPAList< JPABaseParticle >* pool, JPANode< JPABaseParticle >* node) {
    pool->push_front(node);
}

// ---- the recorder (tests/jpa_ptcl_cull_stubs.cpp) ----
struct Regs {
    float pos0[12], tex0[12];
    unsigned tev[4];
    const unsigned char* posArray;
};
struct Call {
    Regs r;
    const unsigned char* list;
    unsigned nbytes;
    int turn;
    float proj[7];  // GXGetProjectionv at the call
};
extern "C" {
extern Regs gRegs;
extern float gProj[7], gVp[6];
extern unsigned gScis[4];
extern unsigned long long gGxCalls[3];
void jpa_cull_record(const Regs* r, const unsigned char* list, unsigned nbytes);
}
static std::vector< Call >* gCalls = nullptr;
static int gTurn = 0;
extern "C" void jpa_cull_record(const Regs* r, const unsigned char* list, unsigned nbytes) {
    Call k{*r, list, nbytes, gTurn, {}};
    memcpy(k.proj, gProj, sizeof k.proj);
    gCalls->push_back(k);
}

// ---- the functions the resource lists hold ----
void JPACalcTexIdxRepeat(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcAlphaFlickAnm(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcPrm(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcEnv(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcClrIdxReverse(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcScaleX(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcScaleY(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcScaleAnmRepeatX(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcScaleAnmReverseY(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcChildAlphaOut(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcChildScaleOut(JPAEmitterWorkData*, JPABaseParticle*);
void JPARegistPrmAlphaEnv(JPAEmitterWorkData*, JPABaseParticle*);
void JPADrawBillboard(JPAEmitterWorkData*, JPABaseParticle*);
void JPADrawRotBillboard(JPAEmitterWorkData*, JPABaseParticle*);
void JPADrawRotation(JPAEmitterWorkData*, JPABaseParticle*);
void JPADrawParticleCallBack(JPAEmitterWorkData*, JPABaseParticle*);

struct NoDraw : JPAParticleCallBack {
    void execute(JPABaseEmitter*, JPABaseParticle*) override {
    }
    void draw(JPABaseEmitter*, JPABaseParticle*) override {
    }
};

struct Scenario {
    const char* name;
    u32 baseFlags;   // JPABaseShapeData::mFlags: type (2 billboard, 7 rotation, 8 crossed rotation), 0x100000 projection texture, 0x200000 drawn from the last
    int quad;        // the parents' quad function: 0 JPADrawBillboard, 1 JPADrawRotBillboard, 2 JPADrawRotation
    bool child;
    u32 childFlags;
    bool callback;
    bool ortho;
    bool bigScissor;  // a scissor wider than the viewport: no side planes
};

static GXColor gPrmTbl[64], gEnvTbl[64];
static u8 gTexTbl[16];
static u16 gTDB1[16];
static std::vector< Regs >* gAfter = nullptr;

static void setCamera(JPAEmitterWorkData& work, const Scenario& sc) {
    // (a view matrix: a turn about y and x, then a move; the emitter wanders within ~200 of the origin)
    const f32 ay = (rand() % 6283) / 1000.0f, ax = ((rand() % 1200) - 600) / 1000.0f;
    Mtx ry, rx, t;
    PSMTXRotRad(ry, 'y', ay);
    PSMTXRotRad(rx, 'x', ax);
    PSMTXTrans(t, static_cast< f32 >((rand() % 400) - 200), static_cast< f32 >((rand() % 200) - 100), -static_cast< f32 >(rand() % 900));
    PSMTXConcat(rx, ry, work.mPosCamMtx);
    PSMTXConcat(t, work.mPosCamMtx, work.mPosCamMtx);
    PSMTXScale(work.mPrjMtx, 0.5f, 0.5f, 1.0f);
    // (the GX state: a perspective projection as C_MTXPerspective + GXSetProjection give it, or an orthographic one; viewport and scissor)
    const f32 cot = 1.0f / tanf(0.5f * (0.6f + (rand() % 60) / 100.0f)), aspect = 640.0f / 456.0f, n = 10.0f, f = 100000.0f;
    gProj[0] = sc.ortho ? 1.0f : 0.0f;
    gProj[1] = cot / aspect;
    gProj[2] = 0.0f;
    gProj[3] = cot;
    gProj[4] = 0.0f;
    gProj[5] = -n / (f - n);
    gProj[6] = -(f * n) / (f - n);
    gVp[0] = 0.0f;
    gVp[1] = 0.0f;
    gVp[2] = 640.0f;
    gVp[3] = 456.0f;
    gVp[4] = 0.0f;
    gVp[5] = 1.0f;
    gScis[0] = 0;
    gScis[1] = 0;
    gScis[2] = sc.bigScissor ? 700 : 640;
    gScis[3] = 456;
}

static void run(const Scenario& sc, int mode, unsigned seed, std::vector< Call >& calls, std::vector< Regs >& after) {
    gMode = mode;
    srand(seed);
    gCalls = &calls;
    gAfter = &after;
    memset(&gRegs, 0, sizeof gRegs);

    static u8 bsd[0x200], esd[0x100], csd[0x100], dyd[0x100];
    memset(bsd, 0, sizeof bsd);
    memset(esd, 0, sizeof esd);
    memset(csd, 0, sizeof csd);
    memset(dyd, 0, sizeof dyd);
    JPABaseShapeData* b = reinterpret_cast< JPABaseShapeData* >(bsd);
    b->mFlags = sc.baseFlags;
    b->mClrAnmFrmMax = 40;
    b->mTexAnmNum = 7;
    b->mBaseSizeX = b->mBaseSizeY = 10.0f;
    b->mAnmRndm = 13;
    b->mClrAnmRndmMask = 0x1F;
    b->mTexAnmRndmMask = 0x07;
    JPAExtraShapeData* e = reinterpret_cast< JPAExtraShapeData* >(esd);
    e->mFlags = 1 | 0x10000 | 0x20000;  // scale and alpha animation, alpha flicker
    e->mScaleInTiming = 0.2f;
    e->mScaleOutTiming = 0.7f;
    e->mScaleInValueX = 0.3f;
    e->mScaleOutValueX = 1.7f;
    e->mScaleInValueY = 0.5f;
    e->mScaleOutValueY = 0.2f;
    e->mScaleOutRandom = 0.4f;
    e->mScaleAnmCycleX = 9;
    e->mScaleAnmCycleY = 13;
    e->mAlphaInTiming = 0.15f;
    e->mAlphaOutTiming = 0.8f;
    e->mAlphaInValue = 0.1f;
    e->mAlphaBaseValue = 0.9f;
    e->mAlphaOutValue = 0.0f;
    e->mAlphaWaveFrequency = 0.3f;
    e->mAlphaWaveRandom = 0.5f;
    e->mAlphaWaveAmplitude = 0.6f;
    JPAChildShapeData* c = reinterpret_cast< JPAChildShapeData* >(csd);
    c->mFlags = sc.childFlags;
    c->mPosRndm = 5.0f;
    c->mBaseVel = 2.0f;
    c->mBaseVelRndm = 0.5f;
    c->mVelInfRate = 0.3f;
    c->mGravity = 0.2f;
    c->mScaleX = c->mScaleY = 1.0f;
    c->mInheritScale = 0.8f;
    c->mInheritAlpha = 0.7f;
    c->mInheritRGB = 0.6f;
    c->mPrmClr = GXColor{200, 100, 50, 255};
    c->mEnvClr = GXColor{10, 20, 30, 255};
    c->mTiming = 0.3f;
    c->mLife = 17;
    c->mRate = 2;
    c->mStep = 2;
    JPADynamicsBlockData* d = reinterpret_cast< JPADynamicsBlockData* >(dyd);
    d->mEmitterScl.set(1.0f, 1.0f, 1.0f);
    d->mEmitterDir.set(0.0f, 1.0f, 0.0f);
    d->mInitialVelOmni = 1.5f;
    d->mInitialVelRndm = 0.7f;
    d->mInitialVelDir = 2.0f;
    d->mSpread = 0.3f;
    d->mInitialVelRatio = 0.2f;
    d->mRate = 1.3f;
    d->mRateRndm = 0.5f;
    d->mLifeTimeRndm = 0.4f;
    d->mAirResist = 0.98f;
    d->mMoment = 1.0f;
    d->mLifeTime = 45;
    d->mVolumeSize = 30;
    d->mDivNumber = 3;
    d->mRateStep = 0;

    JPABaseShape bsp(bsd, nullptr);
    bsp.mpPrmClrAnmTbl = gPrmTbl;
    bsp.mpEnvClrAnmTbl = gEnvTbl;
    bsp.mpTexIdxAnimTbl = gTexTbl;
    JPAExtraShape esp(esd);
    JPAChildShape csp(csd);
    JPADynamicsBlock dyn(dyd);

    // The lists as JPAResource::init builds them (run from the last entry to the first): the look, and for the picture the colours, then
    // the quad (then the callback hook, when there is a callback).
    static JPAResource::ParticleFunc plist[16], clist[4], drawP[3], drawC[3];
    int np = 0;
    plist[np++] = &JPACalcTexIdxRepeat;
    plist[np++] = &JPACalcAlphaFlickAnm;
    plist[np++] = &JPACalcPrm;
    plist[np++] = &JPACalcEnv;
    plist[np++] = &JPACalcClrIdxReverse;
    plist[np++] = &JPACalcScaleY;
    plist[np++] = &JPACalcScaleAnmReverseY;
    plist[np++] = &JPACalcScaleX;
    plist[np++] = &JPACalcScaleAnmRepeatX;
    int nc = 0;
    clist[nc++] = &JPACalcChildScaleOut;
    clist[nc++] = &JPACalcChildAlphaOut;
    drawP[0] = sc.quad == 0 ? &JPADrawBillboard : sc.quad == 1 ? &JPADrawRotBillboard : &JPADrawRotation;
    drawP[1] = &JPARegistPrmAlphaEnv;
    drawC[0] = &JPADrawBillboard;
    drawC[1] = &JPARegistPrmAlphaEnv;

    JPAResource res;
    res.mpBaseShape = &bsp;
    res.mpExtraShape = &esp;
    res.mpChildShape = sc.child ? &csp : nullptr;
    res.mpExTexShape = nullptr;
    res.mpDynamicsBlock = &dyn;
    res.mpFieldBlocks = nullptr;
    res.mpKeyBlocks = nullptr;
    res.mpTDB1 = gTDB1;
    res.mFieldBlockNum = 0;
    res.mKeyBlockNum = 0;
    res.mTDB1Num = 16;
    res.mpCalcParticleFuncList = plist;
    res.mpCalcParticleFuncListNum = static_cast< u8 >(np);
    res.mpCalcParticleChildFuncList = sc.child ? clist : nullptr;
    res.mpCalcParticleChildFuncListNum = static_cast< u8 >(sc.child ? nc : 0);
    res.mpCalcEmitterFuncList = nullptr;
    res.mpCalcEmitterFuncListNum = 0;
    res.mpDrawParticleFuncList = drawP;
    res.mpDrawParticleFuncListNum = 2;
    res.mpDrawParticleChildFuncList = drawC;
    res.mpDrawParticleChildFuncListNum = 2;
    res.mpDrawEmitterFuncList = nullptr;
    res.mpDrawEmitterFuncListNum = 0;
    res.mpDrawEmitterChildFuncList = nullptr;
    res.mpDrawEmitterChildFuncListNum = 0;

    static JPAEmitterWorkData work;
    work = JPAEmitterWorkData();
    static JPATexture* texArr[16];
    static u8 texMem[16][sizeof(JPATexture)];
    for (int i = 0; i < 16; ++i) texArr[i] = reinterpret_cast< JPATexture* >(texMem[i]);
    static u8 rmMem[sizeof(JPAResourceManager)];
    JPAResourceManager* rm = reinterpret_cast< JPAResourceManager* >(rmMem);
    rm->mpTexArr = texArr;
    work.mpResMgr = rm;
    static u8 mgrMem[sizeof(JPAEmitterManager)];
    JPAEmitterManager* mgr = reinterpret_cast< JPAEmitterManager* >(mgrMem);
    mgr->pWd = &work;

    const int kPool = 600;
    static JPANode< JPABaseParticle > nodes[kPool];
    JPAList< JPABaseParticle > pool;
    for (int i = 0; i < kPool; ++i) pool.push_back(&nodes[i]);

    JPABaseEmitter emtr;
    // (the fields JPABaseEmitter::init, decomp/src/Game/System/Overwrite.cpp, sets)
    emtr.mpEmtrMgr = mgr;
    emtr.mpRes = &res;
    emtr.mpPtclPool = &pool;
    emtr.mLocalScl.set(1.0f, 1.0f, 1.0f);
    emtr.mLocalTrs.zero();
    emtr.mLocalDir.set(0.0f, 1.0f, 0.0f);
    emtr.mMaxFrame = 0;
    emtr.mRate = d->mRate;
    emtr.mVolumeSweep = 1.0f;
    emtr.mVolumeMinRad = 0.0f;
    emtr.mAwayFromCenterSpeed = d->mInitialVelOmni;
    emtr.mAwayFromAxisSpeed = 0.0f;
    emtr.mDirSpeed = d->mInitialVelDir;
    emtr.mSpread = d->mSpread;
    emtr.mRndmDirSpeed = d->mInitialVelRndm;
    emtr.mAirResist = d->mAirResist;
    emtr.mLocalRot.set(0, 0, 0);
    emtr.mLifeTime = d->mLifeTime;
    emtr.mVolumeSize = d->mVolumeSize;
    emtr.mRateStep = 0;
    PSMTXIdentity(emtr.mGlobalRot);
    emtr.mGlobalScl.set(1.0f, 1.0f, 1.0f);
    emtr.mGlobalTrs.zero();
    emtr.mGlobalPScl.set(1.0f, 1.0f);
    emtr.mGlobalPrmClr = GXColor{255, 255, 255, 255};
    emtr.mGlobalEnvClr = GXColor{255, 255, 255, 255};
    emtr.mPrmClr = GXColor{128, 128, 128, 255};
    emtr.mEnvClr = GXColor{64, 64, 64, 255};
    emtr.mpUserWork = 0;
    emtr.mScaleOut = 1.0f;
    emtr.mEmitCount = 0.0f;
    emtr.mStatus = 0x30;
    emtr.mDrawTimes = 1;
    emtr.mTick = 0;
    emtr.mWaitTime = 0;
    emtr.mRateStepTimer = 0;
    emtr.mTexAnmIdx = 0;
    emtr.mpEmtrCallBack = nullptr;
    NoDraw touch;
    emtr.mpPtclCallBack = sc.callback ? &touch : nullptr;
    emtr.mRndm.mSeed = seed;
    emtr.mGroupID = 0;
    emtr.mResMgrID = 0;
    emtr.mPortLookStale = 0;

    for (gTurn = 0; gTurn < 400; ++gTurn) {
        const int r = rand() % 100;
        if (r % 7 == 0) {
            emtr.mGlobalTrs.set(static_cast< f32 >(rand() % 200), static_cast< f32 >(rand() % 50), 3.0f);
        }
        if (r % 11 == 0) {
            emtr.mRate = 0.5f + (rand() % 30) / 10.0f;
        }
        res.calc(&work, &emtr);
        const int pictures = (rand() % 100) < 60 ? 1 + ((rand() % 100) < 10) : 0;
        for (int p = 0; p < pictures; ++p) {
            setCamera(work, sc);
            res.draw(&work, &emtr);
            after.push_back(gRegs);
        }
    }
    gCalls = nullptr;
    gAfter = nullptr;
}

// ---- the reference: what the front end does with such a list call (vgx_draw.cpp, base a558298) ----
namespace fe {
struct ClipNow {
    float p[6];
    bool ortho, sides;
    float kx, ky, pSum;
};
// ClipNowOfCalc, from the registers: XF 0x1A.. (viewport scale and offset as GXTransform.c __GXSetViewport computes them), XF 0x20.. (the
// projection), BP 0x20 / 0x21 (scissor corners + 342), BP 0x59 (the scissor offset: 342 / 2 in each field), the EFB's size.
static void ClipNowOfCalc(ClipNow& c, const float proj[7], const float vp[6], const unsigned scis[4], int efbW, int efbH) {
    for (int i = 0; i < 6; ++i) c.p[i] = proj[1 + i];
    c.ortho = proj[0] != 0.0f;
    constexpr float kGuard = 4.0f;
    const float vsx = vp[2] / 2.0f, vsy = -vp[3] / 2.0f, vox = vp[0] + (vp[2] / 2.0f) + 342.0f, voy = vp[1] + (vp[3] / 2.0f) + 342.0f;
    const int xoff = 171 * 2, yoff = 171 * 2;
    const int bx0 = static_cast< int >(scis[0]) + 342, by0 = static_cast< int >(scis[1]) + 342, bx1 = static_cast< int >(scis[0] + scis[2]) + 342 - 1,
              by1 = static_cast< int >(scis[1] + scis[3]) + 342 - 1;
    const float sx0 = std::max(0.0f, static_cast< float >(bx0 - xoff));
    const float sy0 = std::max(0.0f, static_cast< float >(by0 - yoff));
    const float sx1 = std::min(static_cast< float >(efbW), static_cast< float >(bx1 - xoff + 1));
    const float sy1 = std::min(static_cast< float >(efbH), static_cast< float >(by1 - yoff + 1));
    const float vpW = 2.0f * vsx, vpH = -2.0f * vsy, vpX = vox - vsx - static_cast< float >(xoff), vpY = voy + vsy - static_cast< float >(yoff);
    c.sides = vpW >= 16.0f && vpH >= 16.0f && vpW < 1e5f && vpH < 1e5f && sx0 >= vpX - kGuard && sx1 <= vpX + vpW + kGuard && sy0 >= vpY - kGuard &&
              sy1 <= vpY + vpH + kGuard;
    c.kx = 1.0f + 4.0f * kGuard / (c.sides ? vpW : 1.0f);
    c.ky = 1.0f + 4.0f * kGuard / (c.sides ? vpH : 1.0f);
    c.pSum = std::fabs(c.p[0]) + std::fabs(c.p[1]) + std::fabs(c.p[2]) + std::fabs(c.p[3]) + 1.0f;
}
static unsigned PointOutMask(const ClipNow& c, const float* const m[3], float x, float y, float z) {
    const float* const p = c.p;
    const float ex = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
    const float ey = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
    const float ez = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
    float cx, cy, cw, a;
    if (c.ortho) {
        cx = p[0] * ex + p[1]; cy = p[2] * ey + p[3]; cw = 1.0f;
        a = std::fabs(p[0] * ex) + std::fabs(p[1]) + std::fabs(p[2] * ey) + std::fabs(p[3]) + 1.0f;
    } else {
        cx = p[0] * ex + p[1] * ez; cy = p[2] * ey + p[3] * ez; cw = -ez;
        a = std::fabs(p[0] * ex) + std::fabs(p[1] * ez) + std::fabs(p[2] * ey) + std::fabs(p[3] * ez) + std::fabs(ez);
    }
    const float tx = cx - c.kx * cw, nx = -cx - c.kx * cw, ty = cy - c.ky * cw, ny = -cy - c.ky * cw;
    const float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
    float erow = 0.0f;
    for (int r = 0; r < 3; ++r)
        erow = std::max(erow, std::fabs(m[r][0]) * ax + std::fabs(m[r][1]) * ay + std::fabs(m[r][2]) * az + std::fabs(m[r][3]));
    const float margin = 2.0f * ((1e-4f * erow + 1e-6f) * c.pSum + 1e-4f * a) + 1e-6f;
    if (!(margin < 1e30f)) return 0;
    unsigned o = 0;
    if (c.sides) {
        if (tx > margin) o |= 1u;
        if (nx > margin) o |= 2u;
        if (ty > margin) o |= 4u;
        if (ny > margin) o |= 8u;
    }
    if (!c.ortho && -cw > margin) o |= 16u;
    return o;
}
// The list's vertices (GX quads: 0x80, a 16-bit count, then a position index and a texture index per vertex), decoded from the array of
// three signed bytes a vertex, through position matrix 0.
static bool CallOutside(const Call& k, const ClipNow& c) {
    const unsigned n = (static_cast< unsigned >(k.list[1]) << 8) | k.list[2];
    const float* m[3] = {k.r.pos0, k.r.pos0 + 4, k.r.pos0 + 8};
    unsigned all = 31u;
    for (unsigned i = 0; i < n && all; ++i) {
        const unsigned idx = k.list[3 + 2 * i];
        const signed char* v = reinterpret_cast< const signed char* >(k.r.posArray) + idx * 3;
        all &= PointOutMask(c, m, static_cast< float >(v[0]), static_cast< float >(v[1]), static_cast< float >(v[2]));
    }
    return all != 0;
}
}  // namespace fe

static bool sameRegs(const Regs& a, const Regs& b) {
    return memcmp(a.pos0, b.pos0, sizeof a.pos0) == 0 && memcmp(a.tex0, b.tex0, sizeof a.tex0) == 0 && memcmp(a.tev, b.tev, sizeof a.tev) == 0 &&
           a.posArray == b.posArray;
}
static bool sameCall(const Call& a, const Call& b) {
    return a.turn == b.turn && a.list == b.list && a.nbytes == b.nbytes && sameRegs(a.r, b.r) && memcmp(a.proj, b.proj, sizeof a.proj) == 0;
}

static unsigned long long gPreAnswered = 0;
int main() {
    for (int i = 0; i < 64; ++i) {
        gPrmTbl[i] = GXColor{static_cast< u8 >(i * 4), static_cast< u8 >(255 - i * 3), static_cast< u8 >(i * 7), static_cast< u8 >(100 + i)};
        gEnvTbl[i] = GXColor{static_cast< u8 >(i * 5), static_cast< u8 >(i * 2), static_cast< u8 >(255 - i), 255};
    }
    for (int i = 0; i < 16; ++i) {
        gTexTbl[i] = static_cast< u8 >((i * 5) % 16);
        gTDB1[i] = 0;
    }
    const Scenario scenarios[] = {
        {"billboards", 2, 0, false, 0, false, false, false},
        {"billboards, drawn from the last", 2 | 0x200000, 0, false, 0, false, false, false},
        {"rotated billboards with children", 2, 1, true, 0, false, false, false},
        {"rotation quads, projection texture", 7 | 0x100000, 2, false, 0, false, false, false},
        {"crossed rotation quads with children", 8, 2, true, 0x10000, false, false, false},
        {"projection texture animated, children first", 7 | 0x100000 | 0x1000000 | 0x400000, 2, true, 0, false, false, false},
        {"a scissor wider than the viewport", 2, 0, true, 0, false, false, true},
        {"orthographic projection (no test)", 2, 0, true, 0, false, true, false},
        {"particle callback (left alone)", 2, 0, true, 0, true, false, false},
    };
    int failures = 0;
    unsigned long long totalOff = 0, totalLeft = 0, feOutsideKept = 0, gxOff[3] = {}, gxOn[3] = {};
    for (const Scenario& sc : scenarios) {
        unsigned long long nOff = 0, nLeft = 0, nKeptOut = 0;
        for (unsigned seed = 1; seed <= 8; ++seed) {
            std::vector< Call > off, on, dry;
            std::vector< Regs > aOff, aOn, aDry;
            unsigned long long g0[3], g1[3], g2[3];
            memcpy(g0, gGxCalls, sizeof g0);
            run(sc, 0, seed * 7919u, off, aOff);
            memcpy(g1, gGxCalls, sizeof g1);
            run(sc, 1, seed * 7919u, on, aOn);
            memcpy(g2, gGxCalls, sizeof g2);
            run(sc, 2, seed * 7919u, dry, aDry);
            for (int k = 0; k < 3; ++k) {
                gxOff[k] += g1[k] - g0[k];
                gxOn[k] += g2[k] - g1[k];
            }
            size_t bad = 0;
            // (dry: the same calls)
            if (dry.size() != off.size()) ++bad;
            for (size_t i = 0; i < off.size() && i < dry.size(); ++i) if (!sameCall(off[i], dry[i])) ++bad;
            // (the registers after every emitter draw)
            if (aOff.size() != aOn.size() || aOff.size() != aDry.size()) ++bad;
            for (size_t i = 0; i < aOff.size() && i < aOn.size(); ++i) if (!sameRegs(aOff[i], aOn[i])) { if (bad < 3) printf("  registers after emitter draw %zu differ (%s seed %u)\n", i, sc.name, seed); ++bad; }
            // (on: the off calls with some left out, and each one left out is one the front end drops)
            size_t j = 0;
            for (size_t i = 0; i < off.size(); ++i) {
                const bool match = j < on.size() && sameCall(off[i], on[j]);
                fe::ClipNow c;
                const float vp[6] = {0.0f, 0.0f, 640.0f, 456.0f, 0.0f, 1.0f};
                const unsigned scis[4] = {0, 0, sc.bigScissor ? 700u : 640u, 456u};
                fe::ClipNowOfCalc(c, off[i].proj, vp, scis, 640, 456);
                const bool feOut = fe::CallOutside(off[i], c);
                if (match) {
                    ++j;
                    if (feOut) ++nKeptOut;
                } else {
                    ++nLeft;
                    if (!feOut) {
                        if (bad < 3) printf("  %s seed %u: call %zu (turn %d) is missing with the switch on, and the front end's test would draw it\n", sc.name, seed, i, off[i].turn);
                        ++bad;
                    }
                }
            }
            if (j != on.size()) { if (bad < 3) printf("  %s seed %u: %zu calls with the switch on are not calls of the off run\n", sc.name, seed, on.size() - j); ++bad; }
            {   // perf32, GXV_PTCL_PRE: the same calls and registers as the switch on without it; verify mode: the same again, and no centre answer the kernel contradicts
                std::vector< Call > pre, ver;
                std::vector< Regs > aPre, aVer;
                unsigned long long c0[2], c1[2], c2[2];
                port_ptcl_pre_counts(c0);
                gPre = 1;
                run(sc, 1, seed * 7919u, pre, aPre);
                port_ptcl_pre_counts(c1);
                gPre = 2;
                run(sc, 1, seed * 7919u, ver, aVer);
                port_ptcl_pre_counts(c2);
                gPre = 0;
                gPreAnswered += c1[0] - c0[0];
                if (c2[1] != c0[1]) { if (bad < 3) printf("  %s seed %u: GXV_PTCL_PRE=2 counted %llu centre answers the kernel contradicts\n", sc.name, seed, c2[1] - c0[1]); ++bad; }
                if (c2[0] - c1[0] != c1[0] - c0[0]) { if (bad < 3) printf("  %s seed %u: the verify run's centre answers (%llu) are not the on run's (%llu)\n", sc.name, seed, c2[0] - c1[0], c1[0] - c0[0]); ++bad; }
                if (pre.size() != on.size() || ver.size() != on.size() || aPre.size() != aOn.size() || aVer.size() != aOn.size()) { if (bad < 3) printf("  %s seed %u: GXV_PTCL_PRE changes the number of calls\n", sc.name, seed); ++bad; }
                for (size_t i = 0; i < on.size() && i < pre.size() && i < ver.size(); ++i) if (!sameCall(on[i], pre[i]) || !sameCall(on[i], ver[i])) { if (bad < 3) printf("  %s seed %u: call %zu differs with GXV_PTCL_PRE\n", sc.name, seed, i); ++bad; }
                for (size_t i = 0; i < aOn.size() && i < aPre.size() && i < aVer.size(); ++i) if (!sameRegs(aOn[i], aPre[i]) || !sameRegs(aOn[i], aVer[i])) { if (bad < 3) printf("  %s seed %u: registers after emitter draw %zu differ with GXV_PTCL_PRE\n", sc.name, seed, i); ++bad; }
            }
            nOff += off.size();
            if (bad) {
                printf("FAIL %s seed %u: %zu problems (%zu calls off, %zu on, %zu dry)\n", sc.name, seed, bad, off.size(), on.size(), dry.size());
                ++failures;
            }
        }
        printf("%-46s list calls %7llu, left out %7llu (%4.1f %%), drawn though the front end would drop them %llu\n", sc.name, nOff, nLeft,
               nOff ? 100.0 * nLeft / nOff : 0.0, nKeptOut);
        const bool mustLeave = !sc.ortho && !sc.callback;
        if (mustLeave && nLeft == 0) { printf("FAIL %s: nothing left out\n", sc.name); ++failures; }
        if (!mustLeave && nLeft != 0) { printf("FAIL %s: quads left out where none may be\n", sc.name); ++failures; }
        totalOff += nOff;
        totalLeft += nLeft;
        feOutsideKept += nKeptOut;
    }
    printf("%llu list calls, %llu left out, %llu drawn though the front end's test would drop them (callback / orthographic scenarios, a scissor wider than the viewport)\n", totalOff, totalLeft, feOutsideKept);
    printf("GX calls of the particle loops, off -> on: GXSetTevColor %llu -> %llu, matrix loads %llu -> %llu, list calls %llu -> %llu\n", gxOff[0], gxOn[0],
           gxOff[1], gxOn[1], gxOff[2], gxOn[2]);
    printf("GXV_PTCL_PRE: %llu of the %llu quads left out were answered by the centre test; calls and registers identical with it on and in verify mode\n", gPreAnswered, totalLeft);
    if (gPreAnswered == 0) { printf("FAIL: GXV_PTCL_PRE answered nothing (the switch did nothing)\n"); ++failures; }
    printf(failures ? "jpa_ptcl_cull: FAIL\n" : "jpa_ptcl_cull: PASS\n");
    return failures ? 1 : 0;
}
