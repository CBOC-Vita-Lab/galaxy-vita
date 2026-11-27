// GXV_PAR_PTCL (parwork, 7 Nov): host check that JPAEmitterManager::calcNormal on two threads gives exactly the serial loops' result.
//
// The real JPA units run (JPAEmitterManager with its calcNormal, JPAResource, JPAParticle, JPAEmitter, JPADynamicsBlock, JPAFieldBlock,
// JPA*Shape, JPAMath); GX and the textures are stubs. Two worlds are built from the same event stream (spawns, moves, rate changes,
// stops, finite lives that end emitters, changes to the "host" data the emitter callbacks read): world S calcs with GXV_PAR_PTCL=0, world
// P with 1 (a real SpecWorker thread, the pool under its spin lock, the chains of one resource on one thread) or 2 (the in-code verify:
// each parallel turn is also run serially from a saved state and compared). After every turn the two worlds are dumped in a form that
// does not depend on which pool node a particle sits in, and compared: every emitter's bytes, its particles in list order, the groups'
// emitter lists, the free emitter list, the pool count.
//
// Scenarios: only pure callbacks; kind 2 callbacks whose results depend on the ORDER they run in (a shared counter), mixed with pure
// ones, also in a resource shared with them; an unknown callback (the turn must stay serial); a small pool (the bound must keep the
// turn serial or the pool never run dry); finite lives (emitters deleted by their calc).
// With -DPORT_PTCL_NEG_RES (chains per emitter instead of per resource) the same run must FAIL: field blocks are per-resource state.
//
// Build and run: bash tools/test_par_ptcl.sh [neg]
#include "JSystem/JKernel/JKRHeap.hpp"
#include "JSystem/JParticle/JPABaseShape.hpp"
#include "JSystem/JParticle/JPAChildShape.hpp"
#include "JSystem/JParticle/JPADynamicsBlock.hpp"
#include "JSystem/JParticle/JPAEmitter.hpp"
#include "JSystem/JParticle/JPAEmitterManager.hpp"
#include "JSystem/JParticle/JPAExtraShape.hpp"
#include "JSystem/JParticle/JPAFieldBlock.hpp"
#include "JSystem/JParticle/JPAParticle.hpp"
#include "JSystem/JParticle/JPAResource.hpp"
#include "JSystem/JParticle/JPAResourceManager.hpp"
#include "port/par_spec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

// ---- the game's hooks (platform/src/port/perf.cpp) ----
static int gMode = 0;
// (gamecpu23) PTCL_S_ORIG=1: world S runs the ORIGINAL calc (GXV_JPA_LOOK off: every look function in every calc), so P's deferred and
// worker-run look functions are compared with the original after every simulated picture. gWorldP: the world being calculated / drawn.
static int gWorldP = 0;
extern "C" int port_jpa_look_on(void) {
    return gWorldP || !getenv("PTCL_S_ORIG") ? 1 : 0;
}
extern "C" int port_jpa_look_pure_mode(void) {
    return (gWorldP || !getenv("PTCL_S_ORIG")) && getenv("PTCL_LOOKPURE") ? atoi(getenv("PTCL_LOOKPURE")) : 0;
}
extern "C" void port_logic_count(int, unsigned) {
}
// boss23 (PETARI_BOSS_PTCL, decomp/src/Game/Scene/GameScene.cpp): the turn's boss particle factor, 1 here (PTCL_BOSS=1: 0.5 every turn,
// like the budget's factor it must give the same parallel and serial worlds).
extern "C" {
float g_port_boss_ptcl_scale = 1.0f;
float g_port_boss_ptcl_grow = 1.0f;
}
extern "C" void port_boss_ptcl_turn(unsigned, unsigned, unsigned) {
    g_port_boss_ptcl_scale = getenv("PTCL_BOSS") ? 0.5f : 1.0f;
    g_port_boss_ptcl_grow = getenv("PTCL_BOSS") ? 1.41421356f : 1.0f;  // (round 2: PETARI_BOSS_PTCL_GROW's factor at 0.5)
}
// GXV_PB_PTCL (the visual budget, JPADynamicsBlock::create): PTCL_PB=1 thins emitters on one side of the world (x > 0) to half, as the
// budget does for far ones; it runs inside the chains (on the worker too), so the parallel worlds must still equal the serial ones.
extern "C" float port_pb_ptcl_factor(void) {
    return getenv("PTCL_PB") ? 0.5f : 1.0f;
}
extern "C" float port_pb_ptcl_scale_at(float x, float, float) {
    return getenv("PTCL_PB") && x > 0.0f ? 0.5f : 1.0f;
}
// flow23: the factor is decided per calc at calcNormal's start (LodCtrl.cpp port_pb_ptcl_turn), only for the stage's own calc
// (gPortPtclStageCalc, JPAEmitterManager.cpp; ParticleCalcExecutor sets it). Here every calc is a stage's; PTCL_NOSTAGE=1 runs them as
// another effect system's (the star select's): no budget or boss factor at all, and the worlds must still match.
extern int gPortPtclStageCalc;
static float gPbTurn = 1.0f;
extern "C" void port_pb_ptcl_turn(int stage) {
    gPbTurn = stage && getenv("PTCL_PB") ? 0.5f : 1.0f;
}
extern "C" float port_pb_ptcl_turn_factor(void) {
    return gPbTurn;
}
extern "C" int port_par_ptcl_mode(void) {
    return gMode;
}
// GXV_PTCL_LOOKPAR (gamecpu23): PTCL_LOOKPAR=1 turns it on in world P; PTCL_DRAWPAT=60 (every turn drawn, the default with LOOKPAR), 30 (every
// second turn), rand. A picture is simulated before every drawn turn in BOTH worlds (the draw's look flush of every emitter,
// port_jpa_look_flush_emitter) and the worlds are compared only then: between pictures P has run look functions S has left to the draw.
static int gSkippedCur = 0;
static unsigned long long gLookChecked = 0, gLookBad = 0, gLookByPar = 0, gLookStale = 0;
extern "C" int port_turn_skipped_raw(void) {
    return gSkippedCur;
}
extern "C" int port_ptcl_lookpar_mode(void) {
    return getenv("PTCL_LOOKPAR") ? atoi(getenv("PTCL_LOOKPAR")) : 0;
}
extern "C" void port_ptcl_lookpar_note(unsigned checked, unsigned bad) {
    gLookChecked += checked;
    gLookBad += bad;
}
extern "C" void port_jpa_look_flush_emitter(JPABaseEmitter* emtr, JPAEmitterWorkData* work);
extern "C" unsigned port_par_ptcl_min(void) {
    return 0;
}
extern "C" unsigned long long port_par_ptcl_now(void) {
    return port::par::NowUs();
}
static port::par::SpecWorker& gWorker = *new port::par::SpecWorker;  // (never destroyed: its worker waits on it until the process ends)
static std::atomic< uint8_t > gItems[1024];
// (the host build's worker is a std::thread, whose state comes from operator new; the console's is a kernel thread: not the game's heap)
extern "C" void port_host_alloc_scope_enter(void);
extern "C" void port_host_alloc_scope_leave(void);
static bool StartWorker() {
    port_host_alloc_scope_enter();
    const bool ok = gWorker.Start(1, 0, "ptcl_test");
    port_host_alloc_scope_leave();
    return ok;
}
extern "C" int port_par_ptcl_run(void (*fn)(void*, unsigned), void (*mainFn)(void*), void* ctx, unsigned n) {
    static bool started = StartWorker();
    (void)started;
    if (getenv("PTCL_INLINE")) {  // (debugging aid: the same chains and work areas, one thread, no pool lock needed)
        if (mainFn) mainFn(ctx);
        for (unsigned i = n; i-- > 0;) fn(ctx, i);
        return 1;
    }
    uint32_t wait = 0;
    return port::par::ForkJoin(gWorker, gItems, reinterpret_cast< port::par::ItemFn >(fn), mainFn, ctx, n, &wait) ? 1 : 0;
}
// GXV_PAR_PTCL_OVERLAP (gamepar): the two halves (publish; then mainFn, the claims, the join)
extern "C" int port_par_ptcl_begin(void (*fn)(void*, unsigned), void* ctx, unsigned n) {
    static bool started = StartWorker();
    (void)started;
    return port::par::ForkBegin(gWorker, gItems, reinterpret_cast< port::par::ItemFn >(fn), ctx, n) ? 1 : 0;
}
extern "C" void port_par_ptcl_finish(void (*fn)(void*, unsigned), void (*mainFn)(void*), void* ctx, unsigned n) {
    uint32_t wait = 0;
    port::par::ForkEnd(gWorker, gItems, reinterpret_cast< port::par::ItemFn >(fn), mainFn, ctx, n, &wait);
}
extern "C" void port_ptcl_overlap_arm(int on);
extern "C" void port_ptcl_overlap_finish(void);
static unsigned long long gTot[32];
extern "C" void port_par_ptcl_note(const unsigned* c, unsigned n) {
    for (unsigned i = 0; i < n && i < 32; i++) gTot[i] += c[i];
}
static unsigned gBadReports = 0;
extern "C" void port_par_ptcl_bad(unsigned emitter, unsigned offset, unsigned a, unsigned b) {
    if (gBadReports++ < 3) printf("  in-code verify: difference in emitter %d byte %u sizes %u/%u\n", static_cast< int >(emitter), offset, a, b);
}

// ---- stubs for what Overwrite.cpp / JPAResourceManager.cpp define (the check never reaches them) ----
void JPABaseEmitter::init(JPAEmitterManager*, JPAResource*) {
}
JPAResource* JPAResourceManager::getResource(u16) const {
    return nullptr;
}
void JPAEmitterManager::calcYBBCam() {
}
void JPAFieldAir::prepare(JPAEmitterWorkData*, JPAFieldBlock*) {
}
void JPAFieldVortex::prepare(JPAEmitterWorkData*, JPAFieldBlock*) {
}
void JPAFieldVortex::calc(JPAEmitterWorkData*, JPAFieldBlock*, JPABaseParticle*) {
}
void JPAFieldConvection::prepare(JPAEmitterWorkData*, JPAFieldBlock*) {
}
void JPAFieldSpin::prepare(JPAEmitterWorkData*, JPAFieldBlock*) {
}
void* operator new(size_t n, JKRHeap*, int) {
    return calloc(1, n);
}

// ---- alodfix23: the game's heap routing (decomp/src/JSystem/JKernel/JKRHeap.cpp gameAlloc, platform/src/os/os_thread.cpp) ----
// On the console a plain `new` on the game thread goes to the game's CURRENT heap unless a PortHostAllocScope is open. The star select's
// particle manager outlives the scene heap that was current at its first calc: its GXV_PAR_PTCL storage must be host memory. Here every
// plain `new` made by the game thread inside world P's calcNormal outside a host scope is counted as a game-heap allocation: there must be
// none (build with -DPORT_PTCL_NEG_HEAP, tools/test_par_ptcl.sh negheap: PortParMake without its scope, the check must FAIL).
static thread_local int tHostDepth = 0;
static bool gEmuGameCalc = false;  // (set on the main thread around world P's calcNormal)
static unsigned long long gGameHeapNews = 0;
extern "C" void port_host_alloc_scope_enter(void) {
    ++tHostDepth;
}
extern "C" void port_host_alloc_scope_leave(void) {
    --tHostDepth;
}
extern "C" void* port_host_alloc(size_t size, int align) {
    void* p = nullptr;
    if (align < static_cast< int >(sizeof(void*))) align = sizeof(void*);
    return posix_memalign(&p, static_cast< size_t >(align), size ? size : 1) ? nullptr : p;
}
extern "C" void port_host_free(void* p) {
    free(p);
}
extern "C" void port_host_alloc_failed(size_t, int) {
    abort();
}
static void* EmuGameNew(size_t n) {
    if (gEmuGameCalc && tHostDepth == 0) {
        ++gGameHeapNews;
        if (getenv("PTCL_HEAP_TRAP")) __builtin_trap();  // (to find the caller in a debugger)
    }
    void* p = malloc(n ? n : 1);
    if (p == nullptr) abort();
    return p;
}
void* operator new(size_t n) {
    return EmuGameNew(n);
}
void* operator new[](size_t n) {
    return EmuGameNew(n);
}
extern "C" unsigned port_jpa_par_drop(int (*dies)(const void* manager));
static const void* gDyingMgr = nullptr;
static int DyingMgr(const void* m) {
    return m == gDyingMgr ? 1 : 0;
}
void* operator new[](size_t n, JKRHeap*, int) {
    return calloc(1, n);
}

// ---- the look functions the lists hold ----
void JPACalcTexIdxRepeat(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcAlphaFlickAnm(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcPrm(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcEnv(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcClrIdxReverse(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcScaleX(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcScaleY(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcScaleAnmRepeatX(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcScaleAnmReverseY(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcChildScaleOut(JPAEmitterWorkData*, JPABaseParticle*);
void JPACalcChildAlphaOut(JPAEmitterWorkData*, JPABaseParticle*);

// ---- callbacks ----
static float gHost[64];  // (the host data the pure callback reads; changed only between turns)
static unsigned gOrder = 0;  // (state the kind 2 callbacks share: their results depend on the order they run in)
struct PureEmtCb : JPAEmitterCallBack {
    int idx;
    void execute(JPABaseEmitter* e) override {
        e->mGlobalTrs.set(gHost[idx & 63], gHost[(idx + 1) & 63], 2.0f);
        e->mGlobalScl.set(1.0f + gHost[(idx + 2) & 63] * 0.01f, 1.0f, 1.0f);
    }
    int portCalcKind() const override {
        return 1;
    }
};
struct SerialEmtCb : JPAEmitterCallBack {
    void execute(JPABaseEmitter* e) override {
        e->mGlobalTrs.x = static_cast< f32 >(++gOrder);
    }
    int portCalcKind() const override {
        return 2;
    }
};
struct PureNoopPtclCb : JPAParticleCallBack {
    int portCalcKind() const override {
        return 1;
    }
};
struct SerialPtclCb : JPAParticleCallBack {
    void execute(JPABaseEmitter*, JPABaseParticle* p) override {
        p->mOffsetPosition.x += 0.001f * static_cast< f32 >(++gOrder & 1023);
    }
    int portCalcKind() const override {
        return 2;
    }
};
struct UnknownPtclCb : JPAParticleCallBack {
    void execute(JPABaseEmitter*, JPABaseParticle* p) override {
        p->mOffsetPosition.y += 0.5f;
    }
};
static PureEmtCb gPureEmt[64];
static SerialEmtCb gSerialEmt;
static PureNoopPtclCb gPureP;
static SerialPtclCb gSerialP;
static UnknownPtclCb gUnknownP;

// ---- resources ----
static GXColor gPrmTbl[64], gEnvTbl[64];
static u8 gTexTbl[16];
static u16 gTDB1[16];
struct ResBuild {
    u8 bsd[0x200], esd[0x100], csd[0x100], dyd[0x100];
    u8 fd[3][0x48];
    JPABaseShape* bsp;
    JPAExtraShape* esp;
    JPAChildShape* csp;
    JPADynamicsBlock* dyn;
    JPAFieldBlock* fields[3];
    JPAResource res;
    JPAResource::ParticleFunc plist[16], clist[4];
    JPAResource::EmitterFunc elist[4];
};
static void noDraw(JPAEmitterWorkData*, JPABaseParticle*) {
}
static ResBuild* makeRes(int k) {
    ResBuild* r = new ResBuild;
    memset(r->bsd, 0, sizeof r->bsd);
    memset(r->esd, 0, sizeof r->esd);
    memset(r->csd, 0, sizeof r->csd);
    memset(r->dyd, 0, sizeof r->dyd);
    memset(r->fd, 0, sizeof r->fd);
    JPABaseShapeData* b = reinterpret_cast< JPABaseShapeData* >(r->bsd);
    b->mFlags = 0;
    b->mClrAnmFrmMax = 40;
    b->mTexAnmNum = 7;
    b->mBaseSizeX = b->mBaseSizeY = 10.0f;
    b->mAnmRndm = 13;
    b->mClrAnmRndmMask = 0x1F;
    b->mTexAnmRndmMask = 0x07;
    JPAExtraShapeData* e = reinterpret_cast< JPAExtraShapeData* >(r->esd);
    e->mFlags = 1 | 0x10000 | 0x20000;
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
    const bool child = (k % 3) != 0;
    JPAChildShapeData* c = reinterpret_cast< JPAChildShapeData* >(r->csd);
    c->mFlags = (k % 3) == 2 ? 0x70000 : 0;
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
    c->mRate = 1 + k % 3;
    c->mStep = 2;
    JPADynamicsBlockData* d = reinterpret_cast< JPADynamicsBlockData* >(r->dyd);
    d->mFlags = static_cast< u32 >((k % 7) << 8) | ((k % 4) == 3 ? 2u : 0u) | ((k % 5) == 2 ? 1u : 0u) | ((k % 2) ? 8u : 0u);
    d->mEmitterScl.set(1.0f, 1.0f, 1.0f);
    d->mEmitterDir.set(0.0f, 1.0f, 0.0f);
    d->mInitialVelOmni = 1.5f;
    d->mInitialVelAxis = 0.5f;
    d->mInitialVelRndm = 0.7f;
    d->mInitialVelDir = 2.0f;
    d->mSpread = 0.3f;
    d->mInitialVelRatio = 0.2f;
    d->mRate = 0.8f + 0.5f * (k % 4);
    d->mRateRndm = 0.5f;
    d->mLifeTimeRndm = 0.4f;
    d->mVolumeSweep = 1.0f;
    d->mVolumeMinRad = 0.2f;
    d->mAirResist = 0.98f;
    d->mMoment = 1.0f;
    d->mLifeTime = 30 + 3 * k;
    d->mVolumeSize = 30;
    d->mDivNumber = 2 + k % 2;
    d->mRateStep = k % 3 == 0 ? 1 : 0;
    // field blocks: gravity (shared per-resource state: mAccel), random, drag; one or two of them
    for (int f = 0; f < 3; f++) {
        u8* fd = r->fd[f];
        u32* flags = reinterpret_cast< u32* >(fd + 8);
        const u32 type = f == 0 ? 0u : (f == 1 ? 5u : 6u);
        *flags = type | (static_cast< u32 >(f % 3) << 8);
        reinterpret_cast< float* >(fd)[0xC / 4] = 0.0f;
        reinterpret_cast< float* >(fd)[0x10 / 4] = 0.0f;
        reinterpret_cast< float* >(fd)[0x14 / 4] = 0.0f;
        reinterpret_cast< float* >(fd)[0x18 / 4] = f == 0 ? 0.6f : 0.0f;  // (a direction the emitters' rotations change)
        reinterpret_cast< float* >(fd)[0x1C / 4] = -1.0f - 0.1f * k;
        reinterpret_cast< float* >(fd)[0x20 / 4] = f == 0 ? 0.3f : 0.0f;
        reinterpret_cast< float* >(fd)[0x24 / 4] = f == 2 ? 0.97f : 0.05f + 0.01f * k;
        fd[0x40] = 5;
        r->fields[f] = new JPAFieldBlock(fd, nullptr);
    }
    r->bsp = new JPABaseShape(r->bsd, nullptr);
    r->bsp->mpPrmClrAnmTbl = gPrmTbl;
    r->bsp->mpEnvClrAnmTbl = gEnvTbl;
    r->bsp->mpTexIdxAnimTbl = gTexTbl;
    r->esp = new JPAExtraShape(r->esd);
    r->csp = new JPAChildShape(r->csd);
    r->dyn = new JPADynamicsBlock(r->dyd);
    int np = 0, nc = 0;
    r->plist[np++] = &JPACalcTexIdxRepeat;
    r->plist[np++] = &JPACalcAlphaFlickAnm;
    r->plist[np++] = &JPACalcPrm;
    r->plist[np++] = &JPACalcEnv;
    r->plist[np++] = &JPACalcClrIdxReverse;
    r->plist[np++] = &JPACalcScaleY;
    r->plist[np++] = &JPACalcScaleAnmReverseY;
    r->plist[np++] = &JPACalcScaleX;
    r->plist[np++] = &JPACalcScaleAnmRepeatX;
    r->clist[nc++] = &JPACalcChildScaleOut;
    r->clist[nc++] = &JPACalcChildAlphaOut;
    JPAResource& res = r->res;
    res.mpBaseShape = r->bsp;
    res.mpExtraShape = r->esp;
    res.mpChildShape = child ? r->csp : nullptr;
    res.mpExTexShape = nullptr;
    res.mpDynamicsBlock = r->dyn;
    static JPAFieldBlock* fb[16][3];
    for (int f = 0; f < 3; f++) fb[k][f] = r->fields[f];
    res.mpFieldBlocks = fb[k];
    res.mFieldBlockNum = static_cast< u8 >(1 + k % 3);
    res.mpKeyBlocks = nullptr;
    res.mKeyBlockNum = 0;
    res.mpTDB1 = gTDB1;
    res.mTDB1Num = 16;
    res.mpCalcParticleFuncList = r->plist;
    res.mpCalcParticleFuncListNum = static_cast< u8 >(np);
    res.mpCalcParticleChildFuncList = child ? r->clist : nullptr;
    res.mpCalcParticleChildFuncListNum = static_cast< u8 >(child ? nc : 0);
    res.mpCalcEmitterFuncList = nullptr;
    res.mpCalcEmitterFuncListNum = 0;
    static JPAResource::ParticleFunc drawP[1] = {&noDraw};
    res.mpDrawParticleFuncList = drawP;
    res.mpDrawParticleFuncListNum = 1;
    res.mpDrawParticleChildFuncList = drawP;
    res.mpDrawParticleChildFuncListNum = 1;
    res.mpDrawEmitterFuncList = nullptr;
    res.mpDrawEmitterFuncListNum = 0;
    res.mpDrawEmitterChildFuncList = nullptr;
    res.mpDrawEmitterChildFuncListNum = 0;
    return r;
}

// ---- the worlds ----
struct World {
    JPAEmitterManager* mgr;
    JPAEmitterWorkData* wd;
};
static World makeWorld(ResBuild** rb, int nres, unsigned pool, unsigned emitters) {
    (void)rb;
    (void)nres;
    World w;
    w.mgr = new JPAEmitterManager(pool, emitters, nullptr, 9, 1);
    w.wd = w.mgr->pWd;
    return w;
}
struct Event {
    int kind;  // 0 spawn, 1 move, 2 rate, 3 stop/play emit, 4 finite life, 5 stop calc
    int a, b, c;
    float f;
};
// cbs: 0 none, 1 pure emitter cb + pure no-op ptcl cb (as every MultiEmitter effect has), 2 serial emitter cb, 3 serial ptcl cb, 4 unknown ptcl cb
static void applyEvent(World& w, ResBuild** rb, const Event& ev, int cbMix) {
    JPAEmitterManager* m = w.mgr;
    if (ev.kind == 0) {
        JSULink< JPABaseEmitter >* link = m->mFreeEmtrList.getFirst();
        if (link == m->mFreeEmtrList.getEnd()) return;
        const int g = ev.a % 9;
        JPAResource* res = &rb[ev.b]->res;
        m->mFreeEmtrList.remove(link);
        m->pEmtrUseList[g].append(link);
        JPABaseEmitter* e = link->getObject();
        JPADynamicsBlockData* d = reinterpret_cast< JPADynamicsBlockData* >(rb[ev.b]->dyd);
        e->mpEmtrMgr = m;
        e->mpRes = res;
        e->mpPtclPool = &m->mPtclPool;
        e->mLocalScl.set(1.0f, 1.0f, 1.0f);
        e->mLocalTrs.zero();
        // (emitters of one resource differ in direction, rotation and scale, so that a field block's per-calc state, written from the
        // emitter, is different for each: two of them running at once on one resource would show)
        e->mLocalDir.set(0.0f, 1.0f, 0.0f);
        e->mLocalDir.x = static_cast< f32 >((ev.c * 7) % 11) * 0.1f - 0.5f;
        e->mLocalDir.z = static_cast< f32 >((ev.c * 3) % 13) * 0.1f - 0.6f;
        e->mMaxFrame = 0;
        e->mRate = d->mRate;
        e->mVolumeSweep = 1.0f;
        e->mVolumeMinRad = 0.2f;
        e->mAwayFromCenterSpeed = d->mInitialVelOmni;
        e->mAwayFromAxisSpeed = d->mInitialVelAxis;
        e->mDirSpeed = d->mInitialVelDir;
        e->mSpread = d->mSpread;
        e->mRndmDirSpeed = d->mInitialVelRndm;
        e->mAirResist = d->mAirResist;
        e->mLocalRot.set(0, 0, 0);
        e->mLifeTime = d->mLifeTime;
        e->mVolumeSize = d->mVolumeSize;
        e->mRateStep = d->mRateStep;
        PSMTXRotRad(e->mGlobalRot, 'y', static_cast< f32 >(ev.c % 17) * 0.37f);
        e->mGlobalScl.set(1.0f + static_cast< f32 >(ev.c % 5) * 0.25f, 1.0f, 1.0f + static_cast< f32 >(ev.c % 3) * 0.5f);
        e->mGlobalTrs.set(static_cast< f32 >(ev.c % 100), 0.0f, 0.0f);
        e->mGlobalPScl.set(1.0f, 1.0f);
        e->mGlobalPrmClr = GXColor{255, 255, 255, 255};
        e->mGlobalEnvClr = GXColor{255, 255, 255, 255};
        e->mPrmClr = GXColor{128, 128, 128, 255};
        e->mEnvClr = GXColor{64, 64, 64, 255};
        e->mpUserWork = 0;
        e->mScaleOut = 1.0f;
        e->mEmitCount = 0.0f;
        e->mStatus = 0x30;
        e->mDrawTimes = 1;
        e->mTick = 0;
        e->mWaitTime = 0;
        e->mRateStepTimer = 0;
        e->mTexAnmIdx = 0;
        e->mRndm.mSeed = 12345u + static_cast< u32 >(ev.c) * 7919u;
        e->mGroupID = static_cast< u8 >(g);
        e->mResMgrID = 0;
        e->mPortLookStale = 0;
        e->mPortWork = nullptr;
        e->mpEmtrCallBack = nullptr;
        e->mpPtclCallBack = nullptr;
        int cb = cbMix == 0 ? 1 : (ev.c % 5);
        if (cbMix == 1) cb = (ev.c % 3) == 0 ? 2 : 1;                      // kind 2 emitter callbacks among pure ones
        if (cbMix == 2) cb = (ev.c % 4) == 0 ? 3 : 1;                      // kind 2 particle callbacks among pure ones
        if (cbMix == 3) cb = (ev.c % 9) == 0 ? 4 : 1;                      // an unknown callback now and then
        if (cbMix == 4) cb = 1;                                            // (small pool)
        if (cb == 1) {
            gPureEmt[ev.c & 63].idx = ev.c & 63;
            e->mpEmtrCallBack = &gPureEmt[ev.c & 63];
            e->mpPtclCallBack = &gPureP;
        } else if (cb == 2) {
            e->mpEmtrCallBack = &gSerialEmt;
            e->mpPtclCallBack = &gPureP;
        } else if (cb == 3) {
            e->mpPtclCallBack = &gSerialP;
        } else if (cb == 4) {
            e->mpPtclCallBack = &gUnknownP;
        }
        return;
    }
    // the others act on the n-th emitter in the use lists (the same one in both worlds)
    int n = ev.a;
    for (int g = 0; g < 9; g++) {
        for (JSULink< JPABaseEmitter >* l = m->pEmtrUseList[g].getFirst(); l != m->pEmtrUseList[g].getEnd(); l = l->getNext()) {
            if (n-- != 0) continue;
            JPABaseEmitter* e = l->getObject();
            switch (ev.kind) {
            case 1: e->mGlobalTrs.set(ev.f, ev.f * 0.5f, 3.0f); break;
            case 2: e->mRate = 0.2f + ev.f * 0.05f; break;
            case 3:
                if (ev.b & 1) e->setStatus(JPAEmtrStts_StopEmit);
                else e->clearStatus(JPAEmtrStts_StopEmit);
                break;
            case 4: e->mMaxFrame = 8 + (ev.b % 60); break;
            case 5:
                if (ev.b & 1) e->setStatus(JPAEmtrStts_StopCalc);
                else e->clearStatus(JPAEmtrStts_StopCalc);
                break;
            }
            return;
        }
    }
}

// A dump of a world that does not depend on which pool node a particle sits in.
static void put(std::vector< unsigned char >& v, const void* p, size_t n) {
    v.insert(v.end(), static_cast< const unsigned char* >(p), static_cast< const unsigned char* >(p) + n);
}
static std::vector< size_t > gSecs;
static void dump(World& w, std::vector< unsigned char >& v) {
    gSecs.clear();
    JPAEmitterManager* m = w.mgr;
    v.clear();
    for (u32 g = 0; g < 9; g++) {
        for (JSULink< JPABaseEmitter >* l = m->pEmtrUseList[g].getFirst(); l != m->pEmtrUseList[g].getEnd(); l = l->getNext()) {
            const u32 i = static_cast< u32 >(l->getObject() - m->mPortEmtrArr);
            put(v, &i, 4);
        }
        const u32 sep = 0xFFFFFFF0u + g;
        put(v, &sep, 4);
    }
    for (JSULink< JPABaseEmitter >* l = m->mFreeEmtrList.getFirst(); l != m->mFreeEmtrList.getEnd(); l = l->getNext()) {
        const u32 i = static_cast< u32 >(l->getObject() - m->mPortEmtrArr);
        put(v, &i, 4);
    }
    const u32 pn = m->mPtclPool.getNum();
    put(v, &pn, 4);
    for (u32 g = 0; g < 9; g++) {
      for (JSULink< JPABaseEmitter >* l = m->pEmtrUseList[g].getFirst(); l != m->pEmtrUseList[g].getEnd(); l = l->getNext()) {
        // (only the emitters in use: the others are as the allocator left them, different in the two worlds)
        JPABaseEmitter& e = *l->getObject();
        gSecs.push_back(v.size());
        unsigned char buf[sizeof(JPABaseEmitter)];
        memcpy(buf, static_cast< const void* >(&e), sizeof buf);
        auto zero = [&](void* p, size_t n) { memset(buf + (static_cast< unsigned char* >(p) - reinterpret_cast< unsigned char* >(&e)), 0, n); };
        zero(&e.mLink, sizeof e.mLink);
        zero(&e.mAlivePtclBase, sizeof e.mAlivePtclBase);
        zero(&e.mAlivePtclChld, sizeof e.mAlivePtclChld);
        zero(&e.mPortWork, sizeof e.mPortWork);
        zero(&e.mPortLookStale, 3);  // (mPortLookStale / Now / ByPar: compared after a simulated picture, where they legitimately differ)
        zero(&e.mpPtclPool, sizeof e.mpPtclPool);  // (these two point into the world: different in S and P)
        zero(&e.mpEmtrMgr, sizeof e.mpEmtrMgr);
        put(v, buf, sizeof buf);
        for (int pass = 0; pass < 2; pass++) {
            JPAList< JPABaseParticle >& L = pass ? e.mAlivePtclChld : e.mAlivePtclBase;
            const u32 cnt = L.getNum();
            put(v, &cnt, 4);
            for (JPANode< JPABaseParticle >* n = L.getFirst(); n != L.getEnd(); n = n->getNext()) {
                unsigned char tmp[0x97];
                memcpy(tmp, &n->mData, sizeof tmp);
                tmp[0x94] = 0;
                if (pass == 1) {
                    // A child's mEnvClr.a (init_c copies the parent's r g b when colour is inherited, and nothing sets a) and mAnmRandom
                    // (only JPABaseShape's base-particle functions read it) keep what the pool node held; child drawing never reads either
                    // (JPARegistChildPrmEnv takes the colours from the child shape), and which node a particle gets is the one thing the
                    // parallel path changes.
                    tmp[0x93] = 0;
                    tmp[0x95] = 0;
                }
                put(v, tmp, sizeof tmp);
            }
        }
      }
    }
}

struct Scenario {
    const char* name;
    int cbMix;
    unsigned pool;
    int mode;       // P's mode
    int maxEmit;
    bool expectParallel;
};

int main() {
    gPortPtclStageCalc = getenv("PTCL_NOSTAGE") ? 0 : 1;  // (flow23)
    for (int i = 0; i < 64; ++i) {
        gPrmTbl[i] = GXColor{static_cast< u8 >(i * 4), static_cast< u8 >(255 - i * 3), static_cast< u8 >(i * 7), static_cast< u8 >(100 + i)};
        gEnvTbl[i] = GXColor{static_cast< u8 >(i * 5), static_cast< u8 >(i * 2), static_cast< u8 >(255 - i), 255};
    }
    for (int i = 0; i < 16; ++i) {
        gTexTbl[i] = static_cast< u8 >((i * 5) % 16);
        gTDB1[i] = 0;
    }
    const int kRes = 9;
    ResBuild* rb[kRes];
    for (int k = 0; k < kRes; k++) rb[k] = makeRes(k);
    const Scenario scs[] = {
        {"pure callbacks, 9 resources", 0, 20000, 1, 40, true},
        {"pure callbacks, in-code verify", 0, 20000, 2, 40, true},
        {"kind 2 emitter callbacks among pure ones", 1, 20000, 1, 40, true},
        {"kind 2 particle callbacks among pure ones", 2, 20000, 1, 40, true},
        {"an unknown callback now and then", 3, 20000, 1, 40, false},
        {"small pool (bound must keep it exact)", 4, 700, 1, 40, false},
        {"mixed cb kinds 0-4", -1, 20000, 1, 48, false},
    };
    int failures = 0;
    for (const Scenario& sc : scs) {
        unsigned long long parallelTurns = 0, turns = 0;
        for (unsigned seed = 1; seed <= 4; seed++) {
            srand(seed * 7717u);
            for (int i = 0; i < 64; i++) gHost[i] = static_cast< float >(rand() % 100);
            gOrder = 0;
            World S = makeWorld(rb, kRes, sc.pool, sc.maxEmit);
            gOrder = 0;
            World P = makeWorld(rb, kRes, sc.pool, sc.maxEmit);
            std::vector< unsigned char > dS, dP;
            memset(gTot, 0, sizeof gTot);
            bool failed = false;
            for (int turn = 0; turn < 300 && !failed; turn++) {
                // the events of this turn (one stream, applied to both worlds)
                std::vector< Event > evs;
                const int nSpawn = (turn < 40) ? 2 : (rand() % 100 < 25 ? 1 : 0);
                for (int i = 0; i < nSpawn; i++) evs.push_back(Event{0, rand(), rand() % kRes, rand(), 0});
                for (int i = 0; i < 6; i++) evs.push_back(Event{1 + rand() % 5, rand() % 40, rand(), 0, static_cast< float >(rand() % 200)});
                float hostNew[64];
                for (int i = 0; i < 64; i++) hostNew[i] = rand() % 4 == 0 ? static_cast< float >(rand() % 100) : gHost[i];
                // order of two applications: the host data and the kind 2 counter are the same at each world's calc
                for (int wi = 0; wi < 2; wi++) {
                    World& W = wi ? P : S;
                    for (const Event& ev : evs) applyEvent(W, rb, ev, sc.cbMix < 0 ? (turn / 40) % 5 : sc.cbMix);
                }
                memcpy(gHost, hostNew, sizeof gHost);
                const char* pat = getenv("PTCL_DRAWPAT");
                auto drawn = [&](int t) {
                    if (!pat || !strcmp(pat, "60")) return true;
                    if (!strcmp(pat, "30")) return (t & 1) == 0;
                    return ((t * 2654435761u) >> 7) % 3 != 0;  // (rand: two of three, irregular)
                };
                const bool lookTest = port_ptcl_lookpar_mode() != 0 || getenv("PTCL_LOOKPURE") || getenv("PTCL_S_ORIG");
                gSkippedCur = drawn(turn) ? 0 : 1;
                gMode = 0;
                const unsigned o0 = gOrder;
                gWorldP = 0;
                S.mgr->calcNormal();
                gWorldP = 1;
                const unsigned o1 = gOrder;
                gOrder = o0;
                gMode = sc.mode;
                if (getenv("PTCL_OVERLAP")) {
                    // (gamepar: armed, as SceneExecutor runs it before the shadows; the calling thread does other work meanwhile, then joins)
                    port_ptcl_overlap_arm(1);
                    gEmuGameCalc = true;
                    P.mgr->calcNormal();
                    gEmuGameCalc = false;
                    port_ptcl_overlap_arm(0);
                    volatile float sink = 0.0f;
                    for (int k = 0; k < 20000 + (turn % 7) * 3000; k++) sink = sink + gHost[k & 63];  // (reads only: the "shadows")
                    port_ptcl_overlap_finish();
                } else {
                    gEmuGameCalc = true;
                    P.mgr->calcNormal();
                    gEmuGameCalc = false;
                }
                if (gOrder != o1) {
                    printf("  %s seed %u turn %d: kind 2 callbacks ran %u times in P, %u in S\n", sc.name, seed, turn, gOrder - o0, o1 - o0);
                    failed = true;
                }
                if (lookTest) {
                    for (u32 g = 0; g < 9; g++) {
                        for (JSULink< JPABaseEmitter >* l = P.mgr->pEmtrUseList[g].getFirst(); l != P.mgr->pEmtrUseList[g].getEnd(); l = l->getNext()) {
                            gLookByPar += l->getObject()->mPortLookByPar;
                            gLookStale += l->getObject()->mPortLookStale;
                        }
                    }
                    if (!drawn(turn + 1)) {
                        turns++;
                        continue;  // (no picture before the next calc: nothing to compare)
                    }
                    for (int wi = 0; wi < 2; wi++) {
                        World& W = wi ? P : S;
                        gWorldP = wi;
                        if (wi && getenv("PTCL_NEG_NOFLUSH")) continue;  // (negative control: P's picture without the look flush must FAIL)
                        for (u32 g = 0; g < 9; g++) {
                            for (JSULink< JPABaseEmitter >* l = W.mgr->pEmtrUseList[g].getFirst(); l != W.mgr->pEmtrUseList[g].getEnd(); l = l->getNext()) {
                                port_jpa_look_flush_emitter(l->getObject(), W.mgr->pWd);
                            }
                        }
                    }
                }
                dump(S, dS);
                dump(P, dP);
                if (dS != dP) {
                    size_t at = 0;
                    while (at < dS.size() && at < dP.size() && dS[at] == dP[at]) at++;
                    printf("  %s seed %u turn %d: worlds differ at byte %zu (sizes %zu/%zu)\n", sc.name, seed, turn, at, dS.size(), dP.size());
                    {
                        // which emitter section and which field
                        size_t sec = 0;
                        for (size_t i = 0; i < gSecs.size(); i++) if (gSecs[i] <= at) sec = i;
                        const size_t rel = gSecs.empty() ? at : at - gSecs[sec];
                        JPABaseEmitter* z = nullptr;
                        if (rel >= sizeof(JPABaseEmitter) + 4) {
                            u32 cnt0;
                            memcpy(&cnt0, &dS[gSecs[sec] + sizeof(JPABaseEmitter)], 4);
                            const size_t r2 = rel - sizeof(JPABaseEmitter) - 4;
                            if (r2 < cnt0 * 0x97) printf("    base particle %zu of %u, field offset 0x%zx (S byte %02x P byte %02x)\n", r2 / 0x97, cnt0, r2 % 0x97, dS[at], dP[at]);
                            else printf("    after base list (%u particles): child area, rel %zu\n", cnt0, r2 - cnt0 * 0x97);
                        } else printf("    emitter field offset %zu\n", rel);
                        printf("    section %zu offset %zu (sizeof emitter %zu; mStatus %zu mTick %zu mPrmClr %zu mEmitCount %zu mGlobalTrs %zu mAlivePtclBase %zu mRndm %zu)\n", sec, rel,
                               sizeof(JPABaseEmitter), (size_t)&z->mStatus, (size_t)&z->mTick, (size_t)&z->mPrmClr, (size_t)&z->mEmitCount, (size_t)&z->mGlobalTrs,
                               (size_t)&z->mAlivePtclBase, (size_t)&z->mRndm);
                    }
                    failed = true;
                }
                turns++;
            }
            parallelTurns += gTot[1];
            // alodfix23: a scene's destruction drops the storage of its manager only (port_jpa_par_drop), and the manager makes it again
            if (sc.mode != 0) {
                gDyingMgr = nullptr;
                const unsigned none = port_jpa_par_drop(&DyingMgr);
                gDyingMgr = S.mgr;  // (serial world: never made any)
                const unsigned noneS = port_jpa_par_drop(&DyingMgr);
                const bool had = P.mgr->mPortPar != nullptr;
                gDyingMgr = P.mgr;
                const unsigned one = port_jpa_par_drop(&DyingMgr);
                if (none != 0 || noneS != 0 || !had || one != 1 || P.mgr->mPortPar != nullptr) {
                    printf("FAIL %s seed %u: drop freed %u / %u / %u (want 0 / 0 / 1), storage before %d, after %p\n", sc.name, seed, none, noneS, one,
                           had ? 1 : 0, static_cast< void* >(P.mgr->mPortPar));
                    failures++;
                }
                gEmuGameCalc = true;
                P.mgr->calcNormal();  // (made again on the next calc, on the host heap)
                gEmuGameCalc = false;
                if (P.mgr->mPortPar == nullptr) {
                    printf("FAIL %s seed %u: no storage made again after the drop\n", sc.name, seed);
                    failures++;
                }
                if (port_jpa_par_drop(&DyingMgr) != 1) {
                    printf("FAIL %s seed %u: the remade storage was not registered\n", sc.name, seed);
                    failures++;
                }
            }
            if (failed) {
                printf("FAIL %s seed %u\n", sc.name, seed);
                failures++;
            }
            if (seed == 1) {
                printf("%-44s seed 1: %llu turns, %llu parallel, emitters/turn in chains %.1f, on game thread %.1f, fallbacks: unknown cb %llu pool %llu small %llu no worker %llu, verified %llu bad %llu, chains/turn %.1f\n",
                       sc.name, gTot[0], gTot[1], gTot[1] ? gTot[3] / double(gTot[1]) : 0.0, gTot[1] ? gTot[4] / double(gTot[1]) : 0.0, gTot[10],
                       gTot[11], gTot[12], gTot[13], gTot[15], gTot[16], gTot[1] ? gTot[5] / double(gTot[1]) : 0.0);
            }
            if (gTot[16]) {
                printf("FAIL %s seed %u: in-code verify reported %llu mismatches\n", sc.name, seed, gTot[16]);
                failures++;
            }
        }
        if (sc.expectParallel && parallelTurns == 0) {
            printf("FAIL %s: no turn ran in parallel (the check proves nothing)\n", sc.name);
            failures++;
        }
    }
    if (port_ptcl_lookpar_mode() != 0 || getenv("PTCL_LOOKPURE")) {
        printf("lookpar: emitter calcs with the look done in the calc %llu, left to the draw %llu; draw-time verify checked %llu particles, %llu differed\n",
               gLookByPar, gLookStale, gLookChecked, gLookBad);
        if (gLookByPar + gLookStale == 0) failures++;  // (nothing exercised: the check proves nothing)
        if (gLookBad) failures++;
    }
    printf("heap routing: %llu plain news from the game thread's calc outside a host scope (want 0)\n", gGameHeapNews);
    if (gGameHeapNews != 0) {
        printf("FAIL GXV_PAR_PTCL storage would come from the game's current heap (the star select -> stage crash, alodfix23)\n");
        failures++;
    }
    printf(failures ? "par_ptcl: FAIL (%d)\n" : "par_ptcl: PASS (the parallel worlds equal the serial ones in every scenario)\n", failures);
    return failures ? 1 : 0;
}
