// GXV_JPA_LOOK (logic12, 5 Nov): host check that leaving a particle's look functions to the emitter's next draw gives every
// picture exactly the values the old way gives (JPAResource::calc / calc_p / calc_c / draw from decomp/src/JSystem/JParticle).
//
// The real JPA units run (JPAResource, JPAParticle, JPAEmitter, JPADynamicsBlock, JPA*Shape, JPAMath); GX and the texture are
// stubs. Each scenario runs twice from the same seed, once with the switch off and once on, through a few hundred turns with
// random pictures (none, one or two draws in a turn), emitter moves, rate changes and stopped calcs. The draw records what
// the picture reads of every particle (texture index, alpha, colours, scale, position); the two records must be identical.
// Scenarios cover parents with and without children, children that inherit their parent's scale, alpha and colour (the
// parent's look must then stay in the calc), and an emitter with a particle callback (left in the calc).
//
// Build and run: bash tools/test_jpa_look.sh
#include "JSystem/JParticle/JPABaseShape.hpp"
#include "JSystem/JParticle/JPAChildShape.hpp"
#include "JSystem/JParticle/JPADynamicsBlock.hpp"
#include "JSystem/JParticle/JPAEmitter.hpp"
#include "JSystem/JParticle/JPAEmitterManager.hpp"
#include "JSystem/JParticle/JPAExtraShape.hpp"
#include "JSystem/JParticle/JPAParticle.hpp"
#include "JSystem/JParticle/JPAResource.hpp"
#include "JSystem/JParticle/JPAResourceManager.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

// ---- the switch and the counter (platform/src/port/perf.cpp in the game) ----
static int gLook = 0;
extern "C" int port_jpa_look_on(void) {
    return gLook;
}
static unsigned long long gFlushed = 0;
extern "C" void port_logic_count(int what, unsigned n) {
    if (what == 1) gFlushed += n;
}

// ---- the look functions the resource lists hold (JPABaseShape.cpp, JPAExtraShape.cpp, JPAChildShape.cpp) ----
void JPACalcTexIdxRepeat(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcTexIdxNormal(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcAlphaAnm(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcAlphaFlickAnm(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcPrm(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcEnv(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcClrIdxRepeat(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcClrIdxReverse(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcColorCopy(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcScaleX(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcScaleY(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcScaleCopy(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcScaleAnmNormal(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcScaleAnmRepeatX(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcScaleAnmReverseY(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcChildAlphaOut(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcChildScaleOut(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcPrm(JPAEmitterWorkData*);
void JPACalcClrIdxRepeat(JPAEmitterWorkData*);

// ---- what the picture reads: the draw lists call this for every particle ----
struct Seen {
    int turn, child;
    u8 tex, alpha;
    GXColor prm, env;
    f32 sx, sy, px, py, pz;
};
static std::vector< Seen >* gSeen = nullptr;
static int gTurn = 0;
static void recordP(JPAEmitterWorkData*, JPABaseParticle* p) {
    gSeen->push_back(Seen{gTurn, 0, p->mTexAnmIdx, p->mPrmColorAlphaAnm, p->mPrmClr, p->mEnvClr, p->mParticleScaleX, p->mParticleScaleY,
                          p->mPosition.x, p->mPosition.y, p->mPosition.z});
}
static void recordC(JPAEmitterWorkData*, JPABaseParticle* p) {
    gSeen->push_back(Seen{gTurn, 1, p->mTexAnmIdx, p->mPrmColorAlphaAnm, p->mPrmClr, p->mEnvClr, p->mParticleScaleX, p->mParticleScaleY,
                          p->mPosition.x, p->mPosition.y, p->mPosition.z});
}

// A particle callback that reads and writes what the look functions write (it must keep seeing them as of the last calc).
struct TouchLook : JPAParticleCallBack {
    void execute(JPABaseEmitter*, JPABaseParticle* p) override {
        p->mPrmClr.r = static_cast< u8 >(p->mPrmClr.r + p->mPrmColorAlphaAnm);
        p->mParticleScaleY += 0.001f * p->mParticleScaleX;
    }
    void draw(JPABaseEmitter*, JPABaseParticle*) override {
    }
};

struct Scenario {
    const char* name;
    bool child;
    u32 childFlags;      // JPAChildShapeData::mFlags (0x10000 scale, 0x20000 alpha, 0x40000 colour inherited)
    bool globalColor;    // JPACalcColorCopy (emitter-level colour animation) instead of per-particle colour
    bool callback;
};

static GXColor gPrmTbl[64], gEnvTbl[64];
static u8 gTexTbl[16];
static u16 gTDB1[16];

static std::vector< Seen > run(const Scenario& sc, int look, unsigned seed) {
    gLook = look;
    srand(seed);
    std::vector< Seen > seen;
    gSeen = &seen;

    static u8 bsd[0x200], esd[0x100], csd[0x100], dyd[0x100];
    memset(bsd, 0, sizeof bsd);
    memset(esd, 0, sizeof esd);
    memset(csd, 0, sizeof csd);
    memset(dyd, 0, sizeof dyd);
    JPABaseShapeData* b = reinterpret_cast< JPABaseShapeData* >(bsd);
    b->mFlags = 0;  // (type 0; no texture-coordinate animation)
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

    // The lists as JPAResource::init builds them for such a resource (run from the last entry to the first).
    static JPAResource::ParticleFunc plist[16], clist[4], drawP[1], drawC[1];
    static JPAResource::EmitterFunc elist[4];
    int np = 0;
    plist[np++] = &JPACalcTexIdxRepeat;
    plist[np++] = &JPACalcAlphaFlickAnm;
    if (sc.globalColor) {
        plist[np++] = &JPACalcColorCopy;
    } else {
        plist[np++] = &JPACalcPrm;
        plist[np++] = &JPACalcEnv;
        plist[np++] = &JPACalcClrIdxReverse;
    }
    plist[np++] = &JPACalcScaleY;
    plist[np++] = &JPACalcScaleAnmReverseY;
    plist[np++] = &JPACalcScaleX;
    plist[np++] = &JPACalcScaleAnmRepeatX;
    int nc = 0;
    clist[nc++] = &JPACalcChildScaleOut;
    clist[nc++] = &JPACalcChildAlphaOut;
    int ne = 0;
    if (sc.globalColor) {
        elist[ne++] = &JPACalcPrm;
        elist[ne++] = &JPACalcClrIdxRepeat;
    }
    drawP[0] = &recordP;
    drawC[0] = &recordC;

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
    res.mpCalcEmitterFuncList = ne ? elist : nullptr;
    res.mpCalcEmitterFuncListNum = static_cast< u8 >(ne);
    res.mpDrawParticleFuncList = drawP;
    res.mpDrawParticleFuncListNum = 1;
    res.mpDrawParticleChildFuncList = drawC;
    res.mpDrawParticleChildFuncListNum = 1;
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
    TouchLook touch;
    emtr.mpPtclCallBack = sc.callback ? &touch : nullptr;
    emtr.mRndm.mSeed = seed;
    emtr.mGroupID = 0;
    emtr.mResMgrID = 0;
    emtr.mPortLookStale = 0;

    for (gTurn = 0; gTurn < 600; ++gTurn) {
        const int r = rand() % 100;
        if (r < 4) {
            emtr.setStatus(JPAEmtrStts_StopCalc);  // (a stopped calc for a while: the particles keep their last look)
        } else if (r < 12) {
            emtr.clearStatus(JPAEmtrStts_StopCalc);
        }
        if (r % 7 == 0) {
            emtr.mGlobalTrs.set(static_cast< f32 >(rand() % 200), static_cast< f32 >(rand() % 50), 3.0f);
        }
        if (r % 11 == 0) {
            emtr.mRate = 0.5f + (rand() % 30) / 10.0f;
        }
        res.calc(&work, &emtr);
        const int pictures = (rand() % 100) < 45 ? 1 + ((rand() % 100) < 10) : 0;
        for (int p = 0; p < pictures; ++p) {
            res.draw(&work, &emtr);
        }
    }
    gSeen = nullptr;
    return seen;
}

static bool sameSeen(const Seen& a, const Seen& b) {
    return a.turn == b.turn && a.child == b.child && a.tex == b.tex && a.alpha == b.alpha && memcmp(&a.prm, &b.prm, 4) == 0 &&
           memcmp(&a.env, &b.env, 4) == 0 && memcmp(&a.sx, &b.sx, 4) == 0 && memcmp(&a.sy, &b.sy, 4) == 0 && memcmp(&a.px, &b.px, 4) == 0 &&
           memcmp(&a.py, &b.py, 4) == 0 && memcmp(&a.pz, &b.pz, 4) == 0;
}

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
        {"parents only, own colour", false, 0, false, false},
        {"parents only, emitter colour", false, 0, true, false},
        {"parents and children, nothing inherited", true, 0x00000, false, false},
        {"children inherit scale", true, 0x10000, false, false},
        {"children inherit alpha and colour", true, 0x60000, true, false},
        {"particle callback", true, 0x00000, false, true},
    };
    int failures = 0;
    for (const Scenario& sc : scenarios) {
        for (unsigned seed = 1; seed <= 6; ++seed) {
            gFlushed = 0;
            const std::vector< Seen > off = run(sc, 0, seed * 7919u);
            const unsigned long long flushedOff = gFlushed;
            gFlushed = 0;
            const std::vector< Seen > on = run(sc, 1, seed * 7919u);
            size_t mismatch = off.size() == on.size() ? 0 : 1;
            for (size_t i = 0; i < off.size() && i < on.size(); ++i) {
                if (!sameSeen(off[i], on[i])) {
                    if (mismatch < 3) {
                        printf("  mismatch %s seed %u at #%zu turn %d child %d: tex %u/%u alpha %u/%u sx %g/%g\n", sc.name, seed, i,
                               off[i].turn, off[i].child, off[i].tex, on[i].tex, off[i].alpha, on[i].alpha, off[i].sx, on[i].sx);
                    }
                    ++mismatch;
                }
            }
            if (seed == 1) {
                printf("%-42s particles drawn %zu, given their look at a draw: off %llu on %llu\n", sc.name, on.size(), flushedOff, gFlushed);
            }
            if (mismatch) {
                printf("FAIL %s seed %u: %zu mismatches (%zu vs %zu records)\n", sc.name, seed, mismatch, off.size(), on.size());
                ++failures;
            }
        }
    }
    printf(failures ? "jpa_look: FAIL\n" : "jpa_look: PASS (identical pictures in every scenario)\n");
    return failures ? 1 : 0;
}
