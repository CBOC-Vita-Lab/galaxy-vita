#define JPA_LIST_DEFER_INLINE
#define JPA_EMITTER_DEFER_INLINE
#define JSU_LIST_DEFER_INLINE
#include "JSystem/JParticle/JPAEmitterManager.hpp"
#include "JSystem/JKernel/JKRHeap.hpp"
#include "JSystem/JParticle/JPAEmitter.hpp"
#include "JSystem/JParticle/JPAParticle.hpp"
#include "JSystem/JParticle/JPAResourceManager.hpp"
#ifdef TARGET_PC
#include "JSystem/JParticle/JPAChildShape.hpp"
#include "JSystem/JParticle/JPADynamicsBlock.hpp"
#include "JSystem/JParticle/JPAEmitterCallBack.hpp"
#include "JSystem/JParticle/JPAResource.hpp"
#include <atomic>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "port/wait_bound.h"  // (hitch30: BoundedWait)
#include "port/host_vector.h"  // (PortHostVector, PortHostAllocScope: GXV_PAR_PTCL's storage never comes from a game heap)
#endif

JPAEmitterManager::JPAEmitterManager(u32 i_ptclNum, u32 i_emtrNum, JKRHeap* pHeap, u8 i_gidMax, u8 i_ridMax) {
    emtrNum = i_emtrNum;
    ptclNum = i_ptclNum;
    gidMax = i_gidMax;
    ridMax = i_ridMax;

    JPABaseEmitter* p_emtr_link = new (pHeap, 0) JPABaseEmitter[emtrNum];
#ifdef TARGET_PC
    mPortEmtrArr = p_emtr_link;
#endif

    for (u32 i = 0; i < emtrNum; i++)
        mFreeEmtrList.prepend(&p_emtr_link[i].mLink);

    JPANode< JPABaseParticle >* p_ptcl_node = new (pHeap, 0) JPANode< JPABaseParticle >[ptclNum];
#ifdef TARGET_PC
    mPortPtclArr = p_ptcl_node;
#endif
    for (u32 i = 0; i < ptclNum; i++)
        mPtclPool.push_back(&p_ptcl_node[i]);

    pEmtrUseList = new (pHeap, 0) JSUList< JPABaseEmitter >[gidMax];
    pResMgrAry = new (pHeap, 0) JPAResourceManager*[ridMax];
    for (int i = 0; i < ridMax; i++) {
        pResMgrAry[i] = NULL;
    }

    pWd = new (pHeap, 0) JPAEmitterWorkData();
}

JPABaseEmitter* JPAEmitterManager::createSimpleEmitterID(JGeometry::TVec3< f32 > const& pos, u16 resID, u8 group_id, u8 res_mgr_id,
                                                         JPAEmitterCallBack* emtrCB, JPAParticleCallBack* ptclCB) {
    JPAResource* pRes = pResMgrAry[res_mgr_id]->getResource(resID);

    if (pRes == NULL) {
    } else if (mFreeEmtrList.getNumLinks() == 0) {
    } else {
        JSULink< JPABaseEmitter >* pLink = mFreeEmtrList.getFirst();
        mFreeEmtrList.remove(pLink);
        pEmtrUseList[group_id].append(pLink);
        JPABaseEmitter* emtr = pLink->getObject();
        emtr->init(this, pRes);
        emtr->mpPtclPool = &mPtclPool;
        emtr->mpEmtrCallBack = emtrCB;
        emtr->mpPtclCallBack = ptclCB;
        emtr->mGroupID = group_id;
        emtr->mResMgrID = res_mgr_id;
        emtr->mGlobalTrs.set(pos);
        return emtr;
    }

    return NULL;
}

void JPAEmitterManager::calc(u8 group_id) {
    JSULink< JPABaseEmitter >* pNext = NULL;
    for (JSULink< JPABaseEmitter >* pLink = pEmtrUseList[group_id].getFirst(); pLink != pEmtrUseList[group_id].getEnd(); pLink = pNext) {
        pNext = pLink->getNext();

        JPABaseEmitter* emtr = pLink->getObject();

        if (emtr->mpRes->calc(pWd, emtr) && !emtr->checkStatus(0x200))
            forceDeleteEmitter(emtr);
    }
}

#ifdef TARGET_PC
void PortPrewarmGroup(JPAEmitterManager* mgr, u8 group);  // (GXV_PREWARM, at the end of this file)
void PortRehearseSlice();  // (dive27, GXV_REHEARSE: decomp/src/Game/System/PortRehearse.cpp; conditional effect paths drawn once in a warm-up scope behind the closed wipe)
void PortTexEnumSlice();    // (texwarm30, GXV_TEXWARM_KEY: decomp/src/Game/System/PortTexEnum.cpp; reports the textures of the stage's loaded models to the texture warm-up)
void PortStageEnumSlice();  // (precomp26, GXV_STAGE_ENUM: decomp/src/Game/System/PortStageEnum.cpp; the J3D materials of the stage's loaded models, as dry draws)
#endif
void JPAEmitterManager::draw(JPADrawInfo const* drawInfo, u8 group_id) {
    drawInfo->getCamMtx(pWd->mPosCamMtx);
    drawInfo->getPrjMtx(pWd->mPrjMtx);
    calcYBBCam();
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXEnableTexOffsets(GX_TEXCOORD0, GX_TRUE, GX_TRUE);
    GXEnableTexOffsets(GX_TEXCOORD1, GX_TRUE, GX_TRUE);
    GXEnableTexOffsets(GX_TEXCOORD2, GX_TRUE, GX_TRUE);
    GXSetCullMode(GX_CULL_NONE);
    GXSetCoPlanar(GX_FALSE);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
    GXSetVtxDesc(GX_VA_TEX0, GX_INDEX8);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_S8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumChans(0);
#ifdef TARGET_PC
    PortPrewarmGroup(this, group_id);  // (GXV_PREWARM: effects nobody has emitted yet, drawn once where the front end draws nothing; below)
#endif

    for (JSULink< JPABaseEmitter >* pLink = pEmtrUseList[group_id].getFirst(); pLink != pEmtrUseList[group_id].getEnd(); pLink = pLink->getNext()) {
        JPABaseEmitter* emtr = pLink->getObject();
        if (!emtr->checkStatus(0x04)) {
            pWd->mpResMgr = pResMgrAry[emtr->mResMgrID];
            emtr->mpRes->draw(pWd, emtr);
        }
    }
}

void JPAEmitterManager::forceDeleteEmitter(JPABaseEmitter* emtr) {
    emtr->deleteAllParticle();
    emtr->setStatus(0x300);
    pEmtrUseList[emtr->getGroupID()].remove(&emtr->mLink);
    mFreeEmtrList.prepend(&emtr->mLink);
}

void JPAEmitterManager::entryResourceManager(JPAResourceManager* resMgr, u8 resMgrID) {
    pResMgrAry[resMgrID] = resMgr;
}

#ifdef TARGET_PC
// ---------------------------------------------------------------------------------------------------------------------------------
// GXV_PAR_PTCL (parwork, 7 Nov; default 0; perf.cpp has the switches and the log line): ParticleCalcExecutor::movementNormal's nine
// JPAEmitterManager::calc(group) loops, with the emitters that do not depend on each other calculated on two cores. The result is the
// serial loops' result: every value of every emitter and particle, the order of the groups' emitter lists and of the free emitter list,
// the number of particles in the pool. (Which pool node a particle sits in may differ: a node is a slot, nothing reads which one.)
//
// What one emitter's calc (JPAResource::calc) reads and writes:
//  - the emitter itself: its fields, its random generator (mRndm: JPABaseEmitter's own; the manager's pWd->mRndm is read only by
//    JPABaseEmitter::init, which makes emitters, and calc makes none), its two particle lists and their particles;
//  - its resource (JPAResource: the field blocks' per-calc parameters and the fields' mAccel/mDir/..., written by initOpParam / prepare
//    and read by the same calc; the key blocks, the shapes, the dynamics block: read only). A resource is therefore the unit of
//    independence: all emitters of one resource are one chain, calculated in their list order by one thread;
//  - a work area (JPAEmitterWorkData). Every field the calc reads is written first within the same emitter's calc (calcWorkData_c, the
//    volume functions, JPACalcScale* / JPACalcClrIdx* before their reads: audited, see the REPORT), so a chain may use a work area of
//    its own; JPABaseEmitter::mPortWork makes createParticle / createChild use it instead of the manager's pWd;
//  - the particle pool (mPtclPool): popped when a particle is made, pushed when one dies. Taken under a spin lock while the threads run
//    (jpaPoolPop / jpaPoolPush). Which node a particle gets then depends on timing, which nothing observes; whether one is available
//    does not: calcNormal runs in parallel only when the pool holds at least as many nodes as ALL the emitters together could make
//    in this calc (PortPtclBound: a bound from each emitter's rate and child rate), so no pop can fail in any order;
//  - the emitter's callbacks. Parallel only when both are "pure" (portCalcKind() 1: MultiEmitterCallBack, whose execute writes only the
//    emitter and reads the host matrix, and MultiEmitterParticleCallBack's empty execute). Kind 2 (MarioEffect's follow table, the
//    particle callbacks that call game code) runs on the game thread, in the original relative order, on the manager's pWd, while the
//    other core does the parallel chains; they make and delete no emitter or particle. An unknown callback (kind 0) keeps the turn serial.
//  - JPAEmitterManager::forceDeleteEmitter (an emitter whose calc ends it): done afterwards, on the game thread, in the original order.
// Nothing else is written. The turn's other threads write no data a chain reads (the game thread itself is inside this call).
// ---------------------------------------------------------------------------------------------------------------------------------
extern "C" int port_par_ptcl_mode(void);  // perf.cpp: 0 off, 1 on, 2 on + verify (run both, compare, count)
extern "C" unsigned port_par_ptcl_min(void);
extern "C" unsigned long long port_par_ptcl_now(void);
// Runs mainFn(ctx) on the calling thread and fn(ctx, i) for i in [0, n) on it and the worker (the worker from 0 up, the calling thread
// from n-1 down, each item once); returns 1 when everything is done, 0 when there is no worker (nothing was run).
extern "C" int port_par_ptcl_run(void (*fn)(void*, unsigned), void (*mainFn)(void*), void* ctx, unsigned n);
// GXV_PAR_PTCL_OVERLAP (gamepar): port_par_ptcl_run in two halves (publish; then mainFn, the claims from the end, the join).
extern "C" int port_par_ptcl_begin(void (*fn)(void*, unsigned), void* ctx, unsigned n);
extern "C" void port_par_ptcl_finish(void (*fn)(void*, unsigned), void (*mainFn)(void*), void* ctx, unsigned n);
extern "C" void port_par_ptcl_note(const unsigned* counts, unsigned n);
extern "C" void port_par_ptcl_bad(unsigned emitter, unsigned offset, unsigned sizeA, unsigned sizeB);

bool gJpaPoolShared = false;
// GXV_PTCL_LOOKPAR (gamecpu23, 8 Nov; default 0; acts only in a parallel turn of GXV_PAR_PTCL): GXV_JPA_LOOK leaves the particles' look
// functions (colour, alpha, scale, texture index) to the next draw of each emitter, which runs them on the game thread inside draw(). In a
// parallel particle turn after which a picture is expected (see PortNextTurnDraws), the emitters of the PARALLEL chains run them in their
// calc instead, i.e. the way the original game does: the work is done by whichever thread runs the chain, mostly the particle worker on the
// fourth core, and the draw has nothing left to flush for them. Exact by construction: an emitter's calc with its look functions is the
// original's calc (GXV_JPA_LOOK off for that emitter for that calc). The marks are set before the turn's runs (and before GXV_PAR_PTCL=2's
// save, so its serial reference and its parallel run make the same choice) and gJpaLookPar is cleared when the turn ends.
// A wrong guess only costs time: the look functions run in a calc after which no picture follows (on the worker, mostly).
bool gJpaLookPar = false;
extern "C" int port_ptcl_lookpar_mode(void);
extern "C" int port_turn_skipped_raw(void);
namespace {
int sLookPrevDrawn = 1;
// A picture is expected after this turn unless this turn drew and the one before did not (the steady 30's alternation: the next turn
// draws nothing). Read once per particle turn (calcNormal runs once a turn).
bool PortNextTurnDraws() {
    const int cur = port_turn_skipped_raw() ? 0 : 1;
    const bool next = !(cur && !sLookPrevDrawn);
    sLookPrevDrawn = cur;
    return next;
}
bool sLookParThisTurn = false;
}  // namespace
static std::atomic< int > sJpaPoolLock{0};
static inline void JpaPoolLock() {
    while (sJpaPoolLock.exchange(1, std::memory_order_acquire) != 0) {
        // (hitch30: the holder, par-ptcl's worker, is a few instructions from the unlock unless it was preempted on this thread's core:
        // spin at most GXV_BOUND_SPIN_US, then nap so that it can run; census: "par-ptcl pool lock wait")
        port::par::BoundedWait([]() { return sJpaPoolLock.load(std::memory_order_relaxed) != 0; }, port::par::PoolWaitCensus(),
                               port::par::WaitBoundSpinUs(), port::par::WaitBoundNapUs());
    }
}
static inline void JpaPoolUnlock() {
    sJpaPoolLock.store(0, std::memory_order_release);
}
JPANode< JPABaseParticle >* jpaPoolPop(JPAList< JPABaseParticle >* pool) {
    if (!gJpaPoolShared) {
        return pool->pop_front();
    }
    JpaPoolLock();
    JPANode< JPABaseParticle >* n = pool->pop_front();
    JpaPoolUnlock();
    return n;
}
void jpaPoolPush(JPAList< JPABaseParticle >* pool, JPANode< JPABaseParticle >* node) {
    if (!gJpaPoolShared) {
        pool->push_front(node);
        return;
    }
    JpaPoolLock();
    pool->push_front(node);
    JpaPoolUnlock();
}

namespace {
const u32 kPortNone = 0xFFFFFFFFu;
// A turn's counts for perf.cpp's 5 s line (port_par_ptcl_note; perf.cpp reads them in this order).
enum {
    kCTurns,          // turns that reached calcNormal's switch on
    kCParallel,       // ... that ran on two cores
    kCEmitters,       // emitters in the nine groups
    kCParEmitters,    // ... in chains run by the cores (any)
    kCSerEmitters,    // ... run by the game thread beside them (kind 2 callbacks, or a resource shared with one)
    kCChains,         // resources
    kCItems,          // parallel chains
    kCParticles,      // particles alive at the start
    kCUsUsed,         // game thread's time in calcNormal, us
    kCUsSerialRef,    // verify: the serial reference run's time, us
    kCUnknownCb,      // fallbacks to the serial loops: an emitter has a callback of unknown kind
    kCPool,           // ... the pool could not be proven large enough
    kCSmall,          // ... too little work (or one chain and nothing beside it)
    kCNoWorker,       // ... no worker thread
    kCCap,            // ... more emitters than the manager has (cannot happen)
    kCVerified,       // verify: turns compared
    kCBad,            // verify: turns that differ
    kCVerifySkip,     // verify: parallel turns not compared (a kind 2 emitter callback would run twice: see below)
    kCCount
};
}  // namespace

struct JPAPortPar {
    struct Ent {
        JPABaseEmitter* emtr;
        JPAResource* res;
        u32 next;  // the next emitter of the chain (list order), kPortNone at the end
        u32 chain;
        u8 group;
        u8 par;  // both callbacks pure
        u8 del;  // the calc ended the emitter (done in the apply pass)
    };
    u32 cap = 0;
    Ent* ents = nullptr;
    JPAResource** chainRes = nullptr;
    u8* chainBad = nullptr;
    u32* chainHead = nullptr;
    u32* chainTail = nullptr;
    u32* chainCost = nullptr;
    u32* order = nullptr;  // item -> chain
    JPAEmitterWorkData* works = nullptr;
    u32 nEnts = 0, nChains = 0, nItems = 0;
    int runMode = 0;  // (the mode the parallel run was made in, after the verify-skip rule)
    JPAEmitterManager* mgr = nullptr;
    unsigned counts[kCCount] = {};
    // verify (host heap: see PortParMake)
    PortHostVector< unsigned char > saveEmtr, savePtcl, saveMgr, saveUse, saveWd;
    PortHostVector< unsigned char > capA, capB;
    PortHostVector< u32 > secA, secB;
};

namespace {

// Every JPAPortPar that exists (host heap; game thread only: made in calcNormal, dropped at a scene's destruction).
PortHostVector< JPAPortPar* > sPortPars;

// alodfix23 (8 Nov, the "star select -> stage" crash in every mode, console dumps flowP/flowQ-c15): ALL of a manager's GXV_PAR_PTCL storage
// comes from the HOST heap. Before, `new JPAPortPar` and `new JPAEmitterWorkData[cap]` went to the game's CURRENT heap (the global operator
// new, JKRHeap.cpp). The star select's EffectSystem (ScenarioSelectScene::init, made once at boot) makes its JPAPortPar on its first
// calcNormal, which runs on the game thread WHILE the loading thread builds the stage: the current heap is then that stage's scene heap
// (GameSystemSceneController::initializeScene). The star select keeps the pointer for the rest of the run, the scene heap goes with that
// stage, and the next stage places its own objects there. The next star select (and the GameScene's opening camera, which runs while the
// star select fades out: two scenes a turn) then memsets P->counts and stores nEnts / nChains / nItems / the turn's us into the new stage's
// objects. Console proof: in both dumps a director StarPiece's Spine::mExecutor (= P->nItems) and its EffectKeeper header (= P->counts)
// are zero with counts[kCUsUsed] = 7 / 8 us and counts[kCSmall] = 1 at exactly their offsets; the next movement ran exeFall with this = 0.
// With a garbage P->cap / P->ents (other layouts) the writes go anywhere. Same bug class as crash "A" (port/host_vector.h).
// A scene's own manager dies with its scene heap without a destructor: port_jpa_par_drop frees its storage at the scene's destruction.
JPAPortPar* PortParMake(JPAEmitterManager* mgr) {
#ifndef PORT_PTCL_NEG_HEAP
    PortHostAllocScope host;  // (the `new`s below; calloc is host anyway)
#endif
    JPAPortPar* P = new JPAPortPar;
    const u32 cap = mgr->emtrNum ? mgr->emtrNum : 1;
    P->cap = cap;
    P->mgr = mgr;
    P->ents = static_cast< JPAPortPar::Ent* >(calloc(cap, sizeof(JPAPortPar::Ent)));
    P->chainRes = static_cast< JPAResource** >(calloc(cap, sizeof(JPAResource*)));
    P->chainBad = static_cast< u8* >(calloc(cap, 1));
    P->chainHead = static_cast< u32* >(calloc(cap, sizeof(u32)));
    P->chainTail = static_cast< u32* >(calloc(cap, sizeof(u32)));
    P->chainCost = static_cast< u32* >(calloc(cap, sizeof(u32)));
    P->order = static_cast< u32* >(calloc(cap, sizeof(u32)));
    P->works = new JPAEmitterWorkData[cap];
    sPortPars.push_back(P);
    return P;
}

void PortParFree(JPAPortPar* P) {
    PortHostAllocScope host;
    free(P->ents);
    free(P->chainRes);
    free(P->chainBad);
    free(P->chainHead);
    free(P->chainTail);
    free(P->chainCost);
    free(P->order);
    delete[] P->works;
    delete P;
}

// An upper bound of the particles one calc of this emitter can pop from the pool (see the file comment): JPADynamicsBlock::create makes at
// most emitCount, JPABaseParticle::calc_p at most csp->getRate() children for each parent that is then alive.
u64 PortPtclBound(JPABaseEmitter* e) {
    JPAResource* r = e->mpRes;
    JPADynamicsBlock* dyn = r->getDyn();
    u64 create;
    if (e->checkFlag(JPADynFlag_FixedInterval)) {
        const u64 d = dyn->getDivNumber();
        create = dyn->getVolumeType() == 1 /* VOL_Sphere */ ? 4 * d * d + 2 : d;
    } else {
        f32 rr = dyn->getRateRndm();
        if (rr < 0.0f) {
            rr = -rr;
        }
        f32 v = e->mEmitCount + e->mRate * (rr + 1.0f);
        if (!(v > 0.0f)) {
            v = 0.0f;
        } else if (v > 1.0e9f) {
            v = 1.0e9f;
        }
        create = static_cast< u64 >(v) + 2;  // (+1 for FirstEmit's single particle, +1 for the rounding)
    }
    JPAChildShape* csp = r->getCsp();
    u64 rate = 0;
    if (csp != nullptr && csp->getRate() > 0) {
        rate = static_cast< u64 >(csp->getRate());
    }
    if (rate == 0) {
        return create;
    }
    // The parents that can make children in this calc: each alive base particle is calculated once, at age mAge + 1 (it dies instead when
    // that reaches its life), and makes its children only when canCreateChild says so for that age (the same arithmetic); the particles
    // this calc creates are counted as parents all (their age 0 may qualify). Exact for the old ones, an upper bound for the new.
    u64 parents = create;
    for (JPANode< JPABaseParticle >* n = e->mAlivePtclBase.getFirst(); n != e->mAlivePtclBase.getEnd(); n = n->getNext()) {
        const JPABaseParticle* p = n->getObject();
        const int age = p->mAge + 1;
        if (age >= p->mLifeTime) {
            continue;
        }
        const int time = age - static_cast< int >((p->mLifeTime - 1) * csp->getTiming());
        if (time >= 0 && time % (csp->getStep() + 1) == 0) {
            parents++;
        }
    }
    return create + parents * rate;
}

struct PortRunCtx {
    JPAPortPar* P;
};

// One chain, on a work area of its own (JPABaseEmitter::mPortWork makes the emitter's createParticle use it).
void PortChainFn(void* vctx, unsigned item) {
    JPAPortPar* P = static_cast< PortRunCtx* >(vctx)->P;
    const u32 c = P->order[item];
    JPAEmitterWorkData* w = &P->works[item];
    for (u32 i = P->chainHead[c]; i != kPortNone; i = P->ents[i].next) {
        JPAPortPar::Ent& e = P->ents[i];
        memset(static_cast< void* >(w), 0, sizeof(*w));
        e.del = (e.emtr->mpRes->calc(w, e.emtr) && !e.emtr->checkStatus(0x200)) ? 1 : 0;  // (JPAEmitterManager::calc's test)
    }
}

// The game thread's part: the emitters that must stay on it (kind 2 callbacks, or a resource shared with one), in the original order, on
// the manager's pWd as the serial loops do.
void PortMainFn(void* vctx) {
    JPAPortPar* P = static_cast< PortRunCtx* >(vctx)->P;
    for (u32 i = 0; i < P->nEnts; i++) {
        JPAPortPar::Ent& e = P->ents[i];
        if (!P->chainBad[e.chain]) {
            continue;
        }
        e.del = (e.emtr->mpRes->calc(P->mgr->pWd, e.emtr) && !e.emtr->checkStatus(0x200)) ? 1 : 0;
    }
}

// --- verify: save / restore everything the nine calcs touch, and a form of the result that does not depend on which pool node a
// particle sits in ---
void PortSave(JPAPortPar* P) {
    JPAEmitterManager* m = P->mgr;
    const unsigned char* a = reinterpret_cast< const unsigned char* >(m->mPortEmtrArr);
    P->saveEmtr.assign(a, a + sizeof(JPABaseEmitter) * m->emtrNum);
    const unsigned char* b = reinterpret_cast< const unsigned char* >(m->mPortPtclArr);
    P->savePtcl.assign(b, b + sizeof(JPANode< JPABaseParticle >) * m->ptclNum);
    const unsigned char* c = reinterpret_cast< const unsigned char* >(m);
    P->saveMgr.assign(c, c + sizeof(JPAEmitterManager));
    const unsigned char* d = reinterpret_cast< const unsigned char* >(m->pEmtrUseList);
    P->saveUse.assign(d, d + sizeof(JSUList< JPABaseEmitter >) * m->gidMax);
    const unsigned char* f = reinterpret_cast< const unsigned char* >(m->pWd);
    P->saveWd.assign(f, f + sizeof(JPAEmitterWorkData));
}
void PortRestore(JPAPortPar* P) {
    JPAEmitterManager* m = P->mgr;
    memcpy(static_cast< void* >(m->mPortEmtrArr), P->saveEmtr.data(), P->saveEmtr.size());
    memcpy(static_cast< void* >(m->mPortPtclArr), P->savePtcl.data(), P->savePtcl.size());
    memcpy(static_cast< void* >(m), P->saveMgr.data(), P->saveMgr.size());
    memcpy(static_cast< void* >(m->pEmtrUseList), P->saveUse.data(), P->saveUse.size());
    memcpy(static_cast< void* >(m->pWd), P->saveWd.data(), P->saveWd.size());
}
template < class T >
void Put(PortHostVector< unsigned char >& v, const T& x) {
    const unsigned char* p = reinterpret_cast< const unsigned char* >(&x);
    v.insert(v.end(), p, p + sizeof(T));
}
void PortCapture(JPAPortPar* P, PortHostVector< unsigned char >& v, PortHostVector< u32 >& sec) {
    JPAEmitterManager* m = P->mgr;
    v.clear();
    sec.clear();
    for (u32 g = 0; g < m->gidMax; g++) {
        u32 cnt = 0;
        for (JSULink< JPABaseEmitter >* l = m->pEmtrUseList[g].getFirst(); l != m->pEmtrUseList[g].getEnd(); l = l->getNext()) {
            cnt++;
        }
        Put(v, cnt);
        for (JSULink< JPABaseEmitter >* l = m->pEmtrUseList[g].getFirst(); l != m->pEmtrUseList[g].getEnd(); l = l->getNext()) {
            Put(v, static_cast< u32 >(l->getObject() - m->mPortEmtrArr));
        }
    }
    {
        u32 cnt = 0;
        for (JSULink< JPABaseEmitter >* l = m->mFreeEmtrList.getFirst(); l != m->mFreeEmtrList.getEnd(); l = l->getNext()) {
            cnt++;
        }
        Put(v, cnt);
        for (JSULink< JPABaseEmitter >* l = m->mFreeEmtrList.getFirst(); l != m->mFreeEmtrList.getEnd(); l = l->getNext()) {
            Put(v, static_cast< u32 >(l->getObject() - m->mPortEmtrArr));
        }
    }
    Put(v, m->mPtclPool.getNum());
    for (u32 i = 0; i < m->emtrNum; i++) {
        sec.push_back(static_cast< u32 >(v.size()));
        JPABaseEmitter& e = m->mPortEmtrArr[i];
        // (the emitter's bytes without the links and list heads, which hold pointers into the pool, and the work-area pointer)
        unsigned char buf[sizeof(JPABaseEmitter)];
        memcpy(buf, static_cast< const void* >(&e), sizeof(buf));
        memset(buf + (reinterpret_cast< unsigned char* >(&e.mLink) - reinterpret_cast< unsigned char* >(&e)), 0, sizeof(e.mLink));
        memset(buf + (reinterpret_cast< unsigned char* >(&e.mAlivePtclBase) - reinterpret_cast< unsigned char* >(&e)), 0, sizeof(e.mAlivePtclBase));
        memset(buf + (reinterpret_cast< unsigned char* >(&e.mAlivePtclChld) - reinterpret_cast< unsigned char* >(&e)), 0, sizeof(e.mAlivePtclChld));
        memset(buf + (reinterpret_cast< unsigned char* >(&e.mPortWork) - reinterpret_cast< unsigned char* >(&e)), 0, sizeof(e.mPortWork));
        v.insert(v.end(), buf, buf + sizeof(buf));
        for (int pass = 0; pass < 2; pass++) {
            JPAList< JPABaseParticle >& L = pass ? e.mAlivePtclChld : e.mAlivePtclBase;
            Put(v, L.getNum());
            for (JPANode< JPABaseParticle >* n = L.getFirst(); n != L.getEnd(); n = n->getNext()) {
                unsigned char tmp[0x97];  // (JPABaseParticle's fields end at 0x97; the byte at 0x94, mTexAnmIdx, is set by a look function or unused)
                memcpy(tmp, &n->mData, sizeof(tmp));
                tmp[0x94] = 0;
                if (pass == 1) {
                    // A child's mEnvClr.a (init_c leaves it to the pool node when colour is inherited) and mAnmRandom (init_c never sets it;
                    // only JPABaseShape's base-particle functions read it) are what the pool node held. Child drawing reads neither
                    // (JPARegistChildPrmEnv takes the colours from the child shape), and the parallel turn may hand a particle another
                    // node than the serial one would: the one difference allowed, and it is not visible.
                    tmp[0x93] = 0;
                    tmp[0x95] = 0;
                }
                v.insert(v.end(), tmp, tmp + sizeof(tmp));
            }
        }
    }
}

void PortCalcSerial9(JPAEmitterManager* m) {
    m->calc(0);
    m->calc(1);
    m->calc(2);
    m->calc(3);
    m->calc(4);
    m->calc(5);
    m->calc(6);
    m->calc(7);
    m->calc(8);
}

// The parallel turn, if its conditions hold (else the nine serial loops).
void PortCalcNormalTail(JPAEmitterManager* m, int mode, JPAPortPar* P);
// GXV_PAR_PTCL_OVERLAP (gamepar): with overlap, a parallel turn returns 1 after handing the chains to the worker (the rest is
// PortCalcNormalTail, at port_ptcl_overlap_finish) and a turn that is not parallel returns 2 having computed nothing (the serial loops run at
// port_ptcl_overlap_finish, after the shadows, where they run without overlap). Without overlap: 0, everything done.
PortRunCtx sOverlapCtx;
int PortCalcNormalImpl(JPAEmitterManager* m, int mode, JPAPortPar* P, bool overlap) {
    unsigned* C = P->counts;
    C[kCTurns]++;

    // 1. the emitters of the nine groups, in the serial loops' order
    u32 n = 0;
    bool unknown = false, overflow = false, kind2Emt = false;
    u64 bound = 0;
    u32 alive = 0;
    for (u32 g = 0; g < 9 && g < m->gidMax && !overflow; g++) {
        for (JSULink< JPABaseEmitter >* l = m->pEmtrUseList[g].getFirst(); l != m->pEmtrUseList[g].getEnd(); l = l->getNext()) {
            JPABaseEmitter* e = l->getObject();
            if (n >= P->cap) {
                overflow = true;
                break;
            }
            const int ek = e->mpEmtrCallBack != nullptr ? e->mpEmtrCallBack->portCalcKind() : 1;
            const int pk = e->mpPtclCallBack != nullptr ? e->mpPtclCallBack->portCalcKind() : 1;
            if (ek == 0 || pk == 0) {
                unknown = true;
            }
            kind2Emt |= ek == 2;
            JPAPortPar::Ent& en = P->ents[n];
            en.emtr = e;
            en.res = e->mpRes;
            en.next = kPortNone;
            en.group = static_cast< u8 >(g);
            en.par = (ek == 1 && pk == 1) ? 1 : 0;
            en.del = 0;
            alive += e->getParticleNumber();
            bound += PortPtclBound(e);
            n++;
        }
    }
    P->nEnts = n;
    C[kCEmitters] += n;
    C[kCParticles] += alive;
    if (overflow || unknown || bound > m->mPtclPool.getNum()) {
        C[overflow ? kCCap : unknown ? kCUnknownCb : kCPool]++;
        if (overlap) {
            return 2;
        }
        PortCalcSerial9(m);
        return 0;
    }

    // 2. chains (one per resource), their costs, the item order (cheapest first: the worker takes from the front, the game thread from
    // the back)
    u32 nCh = 0;
    for (u32 i = 0; i < n; i++) {
        JPAPortPar::Ent& en = P->ents[i];
#ifdef PORT_PTCL_NEG_RES
        JPAResource* const key = reinterpret_cast< JPAResource* >(en.emtr);  // (host negative control: a chain per emitter; the check must fail)
#else
        JPAResource* const key = en.res;
#endif
        u32 c = 0;
        for (; c < nCh; c++) {
            if (P->chainRes[c] == key) {
                break;
            }
        }
        if (c == nCh) {
            P->chainRes[c] = key;
            P->chainBad[c] = 0;
            P->chainHead[c] = P->chainTail[c] = kPortNone;
            P->chainCost[c] = 0;
            nCh++;
        }
        en.chain = c;
        if (P->chainTail[c] == kPortNone) {
            P->chainHead[c] = i;
        } else {
            P->ents[P->chainTail[c]].next = i;
        }
        P->chainTail[c] = i;
        P->chainBad[c] |= en.par ? 0 : 1;
        P->chainCost[c] += 8 + en.emtr->getParticleNumber();
    }
    u32 nItems = 0;
    u64 parCost = 0, serCost = 0;
    for (u32 c = 0; c < nCh; c++) {
        if (P->chainBad[c]) {
            serCost += P->chainCost[c];
        } else {
            parCost += P->chainCost[c];
            u32 k = nItems++;
            while (k > 0 && P->chainCost[P->order[k - 1]] > P->chainCost[c]) {  // (insertion sort, ascending)
                P->order[k] = P->order[k - 1];
                k--;
            }
            P->order[k] = c;
        }
    }
    P->nChains = nCh;
    P->nItems = nItems;
    if (nItems == 0 || (nItems == 1 && serCost == 0) || parCost + serCost < port_par_ptcl_min()) {
        C[kCSmall]++;
        if (overlap) {
            return 2;
        }
        PortCalcSerial9(m);
        return 0;
    }

    // GXV_PTCL_LOOKPAR: the parallel chains' emitters run their look functions in this turn's calc (marks before the verify's save)
    gJpaLookPar = false;
    if (sLookParThisTurn) {
        for (u32 i = 0; i < n; i++) {
            P->ents[i].emtr->mPortLookNow = P->chainBad[P->ents[i].chain] ? 0 : 1;
        }
        gJpaLookPar = true;
    }

    PortRunCtx ctx = {P};
    // (verify: the serial loops first, from the saved state, then the saved state again for the parallel run. Not when a kind 2 emitter
    // callback is in the turn: MarioEffect::execute edits its own table, so the second run would not start from the first's state.)
    if (mode == 2 && kind2Emt) {
        C[kCVerifySkip]++;
        mode = 1;
    }
    if (mode == 2) {
        PortSave(P);
        const unsigned long long ts = port_par_ptcl_now();
        PortCalcSerial9(m);
        C[kCUsSerialRef] += static_cast< unsigned >(port_par_ptcl_now() - ts);
        PortCapture(P, P->capA, P->secA);
        PortRestore(P);
    }

    // 3. the parallel run
    P->runMode = mode;
    if (overlap) {
        sOverlapCtx = ctx;
        gJpaPoolShared = true;
        if (port_par_ptcl_begin(&PortChainFn, &sOverlapCtx, nItems)) {
            return 1;  // (the worker has the chains; the game thread casts the shadows, then port_ptcl_overlap_finish)
        }
        gJpaPoolShared = false;
        gJpaLookPar = false;  // (GXV_PTCL_LOOKPAR: a serial turn as before)
        C[kCNoWorker]++;
        if (mode == 2) {
            PortRestore(P);
        }
        return 2;
    }
    gJpaPoolShared = true;
    const int ran = port_par_ptcl_run(&PortChainFn, &PortMainFn, &ctx, nItems);
    gJpaPoolShared = false;
    if (!ran) {
        // no worker: nothing was run
        gJpaLookPar = false;  // (GXV_PTCL_LOOKPAR: a serial turn as before)
        C[kCNoWorker]++;
        if (mode == 2) {
            PortRestore(P);
        }
        PortCalcSerial9(m);
        return 0;
    }
    PortCalcNormalTail(m, mode, P);
    return 0;
}

// After the join (both cores done, the worker out of the batch): the deletions in the serial order, the counts, the verify compare.
void PortCalcNormalTail(JPAEmitterManager* m, int mode, JPAPortPar* P) {
    unsigned* C = P->counts;
    const u32 n = P->nEnts;
    const u32 nCh = P->nChains;
    const u32 nItems = P->nItems;
    // 4. the deletions, in the serial loops' order
    for (u32 i = 0; i < n; i++) {
        if (P->ents[i].del) {
            m->forceDeleteEmitter(P->ents[i].emtr);
        }
    }
    u32 serEm = 0;
    for (u32 i = 0; i < n; i++) {
        serEm += P->chainBad[P->ents[i].chain] ? 1 : 0;
    }
    C[kCParallel]++;
    C[kCChains] += nCh;
    C[kCItems] += nItems;
    C[kCSerEmitters] += serEm;
    C[kCParEmitters] += n - serEm;
    if (mode == 2) {
        PortCapture(P, P->capB, P->secB);
        C[kCVerified]++;
        const bool same = P->capA.size() == P->capB.size() && memcmp(P->capA.data(), P->capB.data(), P->capA.size()) == 0;
        if (!same) {
            C[kCBad]++;
            size_t at = 0;
            const size_t lim = P->capA.size() < P->capB.size() ? P->capA.size() : P->capB.size();
            while (at < lim && P->capA[at] == P->capB[at]) {
                at++;
            }
            u32 emIdx = 0xFFFFFFFFu;
            for (u32 i = 0; i < P->secA.size(); i++) {
                if (P->secA[i] <= at) {
                    emIdx = i;
                }
            }
            port_par_ptcl_bad(emIdx, static_cast< unsigned >(emIdx != 0xFFFFFFFFu ? at - P->secA[emIdx] : at),
                              static_cast< unsigned >(P->capA.size()), static_cast< unsigned >(P->capB.size()));
            // (leave the serial result)
            PortRestore(P);
            PortCalcSerial9(m);
        }
    }
    gJpaLookPar = false;  // (GXV_PTCL_LOOKPAR: the turn is over)
}

// GXV_PAR_PTCL_OVERLAP: the calc handed out by the armed calcNormal, finished by port_ptcl_overlap_finish (game thread only).
bool sOverlapArmed = false;
JPAEmitterManager* sOverlapPending = nullptr;
int sOverlapKind = 0;  // 1 parallel (the worker has the chains), 2 serial (nothing computed yet)
unsigned long long sOverlapUs = 0;
}  // namespace

extern "C" int port_par_ptcl_overlap_on(void);
// SceneExecutor.cpp: on around the AnimParticle list when it runs before the shadow list.
extern "C" void port_ptcl_overlap_arm(int on) {
    sOverlapArmed = on != 0;
}
extern "C" void port_ptcl_overlap_finish(void) {
    JPAEmitterManager* m = sOverlapPending;
    if (m == nullptr) {
        return;
    }
    sOverlapPending = nullptr;
    const unsigned long long t0 = port_par_ptcl_now();
    JPAPortPar* P = m->mPortPar;
    if (sOverlapKind == 2) {
        PortCalcSerial9(m);
    } else {
        port_par_ptcl_finish(&PortChainFn, &PortMainFn, &sOverlapCtx, P->nItems);
        gJpaPoolShared = false;
        PortCalcNormalTail(m, P->runMode, P);
    }
    P->counts[kCUsUsed] = static_cast< unsigned >(sOverlapUs + (port_par_ptcl_now() - t0));
    port_par_ptcl_note(P->counts, kCCount);
}

// alodfix23: a scene's destruction (GameSystemSceneController::exeDestroyScene, game thread, before its heap goes): the GXV_PAR_PTCL
// storage of every manager that goes with it (`dies(manager)`: the manager lies in that scene's heap) is freed, the manager forgets it
// (its memory is still there at this point). Managers that outlive the scene (the star select's, made at boot) keep theirs. Returns the
// number freed.
extern "C" unsigned port_jpa_par_drop(int (*dies)(const void* manager)) {
    unsigned n = 0;
    for (size_t i = 0; i < sPortPars.size();) {
        JPAPortPar* P = sPortPars[i];
        if (dies == nullptr || !dies(P->mgr)) {
            ++i;
            continue;
        }
        if (sOverlapPending == P->mgr) {
            port_ptcl_overlap_finish();  // (cannot happen: the overlap is finished inside its turn; kept for safety)
        }
        if (P->mgr->mPortPar == P) {
            P->mgr->mPortPar = nullptr;
        }
        sPortPars[i] = sPortPars.back();
        sPortPars.pop_back();
        PortParFree(P);
        ++n;
    }
    return n;
}

// boss23 (decomp/src/Game/Scene/GameScene.cpp): the live particles and emitters at the start of the turn's calc, for the boss particle
// cut (PETARI_BOSS_PTCL: its factor is fixed here, one value for the whole calc, serial or parallel) and the "boss:" log line.
extern "C" void port_boss_ptcl_turn(u32 live, u32 emitters, u32 poolSize);
extern "C" float g_port_boss_ptcl_scale;
extern "C" float g_port_boss_ptcl_grow;
// flow23 (8 Nov): whether the calc about to run is the stage's own (ParticleCalcExecutor::movementNormal of the scene's effect system sets
// it to 1 before calling calcNormal, 0 for an effect system outside the scene: the star select's). The budget's and the boss cut's
// factors are the stage's only; another system's calc runs exactly as the game (GXV_PB_PTCL: decomp/src/Game/LiveActor/LodCtrl.cpp).
int gPortPtclStageCalc = 0;
extern "C" void port_pb_ptcl_turn(int stage);
extern "C" int port_cc_flag;  // (far30: bit 1 = GXV_DISTCENSUS)
extern "C" void port_dist_note_particles(float x, float y, float z, unsigned n);

void JPAEmitterManager::calcNormal() {
    const int stage = gPortPtclStageCalc;
    if (stage) {
        port_boss_ptcl_turn(ptclNum - mPtclPool.getNum(), static_cast< u32 >(getEmitterNumber()), ptclNum);
    } else {
        g_port_boss_ptcl_scale = 1.0f;  // (the boss log's counts stay the stage's)
        g_port_boss_ptcl_grow = 1.0f;
    }
    port_pb_ptcl_turn(stage);  // (GXV_PB_PTCL: this calc's factor, camera and player, before any chain is handed out)
    if (stage && __builtin_expect((port_cc_flag & 2) != 0, 0)) {
        // (far30, GXV_DISTCENSUS: the live particles of the stage's emitters by the emitter's distance from the camera; counts only)
        for (u32 g = 0; g < gidMax; ++g) {
            for (JSULink< JPABaseEmitter >* l = pEmtrUseList[g].getFirst(); l != pEmtrUseList[g].getEnd(); l = l->getNext()) {
                const JPABaseEmitter* em = l->getObject();
                port_dist_note_particles(em->mGlobalTrs.x, em->mGlobalTrs.y, em->mGlobalTrs.z, em->getParticleNumber());
            }
        }
    }
    const int mode = port_par_ptcl_mode();
    if (sOverlapPending != nullptr) {
        port_ptcl_overlap_finish();  // (cannot happen: one armed list per turn, finished after the shadows; kept for safety)
    }
    if (mode == 0) {
        PortCalcSerial9(this);
        return;
    }
    const unsigned long long t0 = port_par_ptcl_now();
    if (mPortPar == nullptr) {
        mPortPar = PortParMake(this);
    }
    JPAPortPar* P = mPortPar;
    memset(P->counts, 0, sizeof(P->counts));
    gJpaLookPar = false;
    sLookParThisTurn = port_ptcl_lookpar_mode() != 0 && PortNextTurnDraws();  // (GXV_PTCL_LOOKPAR)
    const int r = PortCalcNormalImpl(this, mode, P, sOverlapArmed);
    if (r != 0) {
        sOverlapPending = this;
        sOverlapKind = r;
        sOverlapUs = port_par_ptcl_now() - t0;
        return;
    }
    P->counts[kCUsUsed] = static_cast< unsigned >(port_par_ptcl_now() - t0);
    port_par_ptcl_note(P->counts, kCCount);
}
#endif

#ifdef TARGET_PC
// ---------------------------------------------------------------------------------------------------------------------------------
// prewarm24 (GXV_PREWARM, read once by the renderer: vgx_draw.cpp PrewarmMode; 0 off, the default): the shader programs of the effects a
// stage can show are made behind its closed screen, not at their first emission in play (a spin attack's hit effect in Honeyhive: 683 ms,
// two programs through the shader compiler).
//   Which effects: every (resource, draw group) pair the stage's actors set up (SingleEmitter::scanParticleEmitter, at each actor's
//   EffectKeeper set-up: the auto effects of its AutoEffectList entries, hit marks included) - mode 1; every resource of the resource
//   managers in every group the scene draws - mode 2 (the pack tour: no budget). One-time effects (hits, breaks, pick-ups: the ones a player
//   action shows first) go before continuous ones (those the opening already shows are linked by the stage's own list anyway).
//   How: in JPAEmitterManager::draw of the pair's group, after the manager's set-up and before the group's own emitters - so with the very GX
//   state the group's real emitters start from (projection, fog, the colour and alpha updates, the EFB format, the channel set-up) -, an
//   emitter of the resource (JPABaseEmitter::init, as createSimpleEmitterID does) with two particles (and two children when it has a child
//   shape: a stripe needs two) is drawn through JPAResource::draw, the real draw path, between PORT_GX_MARK_WARM_BEGIN and _END. The renderer's
//   front end resolves those draws as any other (the same keys: they are made of the registers the game set and of the draw's own vertices)
//   and records nothing; the back end links each program from its caches in its quiet slices (never in a frame in play), and the closed-screen
//   hold waits for them within GXV_PREWARM_MS.
//   What is put back afterwards, so that the game's own frame goes on exactly as without it: the GX library's state (GXData, which the scope's
//   GX calls change; its dirty flags make the next real draw send what it would have sent), the draw counter the bloom skip reads, the
//   manager's work area (its random generator: init and the particles' set-up draw from it), the particle pool (the particles go back to it).
//   The front end puts its own registers back at _END (vgx_draw.cpp WarmScopeMarker).
//   When: only in frames whose screen is closed (port_screen_quiet() == 2: a fade or wipe at least 90 % closed), and in mode 1 at most
//   GXV_PREWARM_FRAME_MS (default 4) of the game thread a frame; the rest goes on in the next closed frame. Never in play.
//   GXV_PREWARM_ALL=1 in mode 1: every resource, as mode 2, with the budget.
// ---------------------------------------------------------------------------------------------------------------------------------
extern "C" int port_prewarm_mode(void) __attribute__((weak));  // (vgx_galaxy.cpp: the renderer's GXV_PREWARM; 0 where it does not honour the scope)
extern "C" int port_screen_quiet(void) __attribute__((weak));
extern "C" uint32_t port_scene_loading_key(void) __attribute__((weak));
extern "C" unsigned long long port_par_ptcl_now(void);
extern "C" void port_gx_marker(unsigned int kind);
extern "C" u32 __PortGXDrawSerial;
extern "C" uint64_t port_gx_recording_frame(void) __attribute__((weak));
namespace {
enum { kPwGroups = 16, kPwWarmBegin = 14, kPwWarmEnd = 15 };
struct PortPrewarm {
    int mode = -1;
    bool all = false;
    bool census = false;  // (GXV_PREWARM=0 with GXV_PREWARM_LOG=1: the effects are registered and counted, nothing is drawn)
    uint32_t censusLogged = 0;
    unsigned long long frameUs = 4000;
    // (mode 1) the stage's pairs: resource user index << 8 | group, one-time ones first (two lists, then concatenated when a group is drawn)
    uint32_t key = 0;
    PortHostVector< u32 > once, cont;
    PortHostVector< u8 > doneMask;  // (per pair: 1 done or skipped) mode 1; (per resource of manager 0: bit per group) modes 2 and ALL
    PortHostVector< u16 > allDone;
    u16 groupsDrawn = 0;
    unsigned long long frame = ~0ull, frameSpentUs = 0;
    // counters (port_prewarm_game_counters)
    unsigned long long warmed = 0, skipped = 0, noDraw = 0, spentUs = 0, scopes = 0, frames = 0;
    bool dirtyOrder = true;
    PortHostVector< u32 > order;  // once + cont
};
PortPrewarm sPw;
bool PwOn() {
    if (sPw.mode < 0) {
        sPw.mode = port_prewarm_mode ? port_prewarm_mode() : 0;
        const char* a = getenv("GXV_PREWARM_ALL");
        sPw.all = sPw.mode == 2 || (a != NULL && atoi(a) != 0);
        const char* f = getenv("GXV_PREWARM_FRAME_MS");
        const int ms = f != NULL ? atoi(f) : 4;
        sPw.frameUs = static_cast< unsigned long long >(ms < 1 ? 1 : ms > 200 ? 200 : ms) * 1000ull;
        const char* l = getenv("GXV_PREWARM_LOG");
        sPw.census = sPw.mode == 0 && l != NULL && atoi(l) != 0;
    }
    return sPw.mode > 0;
}
// One resource drawn once inside a warm-up scope, in the current group's state. Returns 1 when its draw reached the GX library (a draw was
// sent), 0 when it drew nothing (a particle its draw function leaves out, an empty pool), -1 when it cannot be drawn at all.
int PwDrawOne(JPAEmitterManager* mgr, u8 resMgrId, JPAResource* res) {
    if (res == NULL || res->getBsp() == NULL || res->getDyn() == NULL)
        return -1;
    static JPABaseEmitter* sEmtr = NULL;
    if (sEmtr == NULL) {
        PortHostAllocScope scope;
        sEmtr = new JPABaseEmitter();
    }
    JPABaseEmitter* emtr = sEmtr;
    JPAEmitterWorkData* const wd = mgr->pWd;
    static JPAEmitterWorkData sSaved;
    static GXData sGx;
    memcpy(&sSaved, wd, sizeof sSaved);
    memcpy(&sGx, gx, sizeof sGx);
    const u32 serial0 = __PortGXDrawSerial;
    emtr->init(mgr, res);
    emtr->mpPtclPool = &mgr->mPtclPool;
    emtr->mpEmtrCallBack = NULL;
    emtr->mpPtclCallBack = NULL;
    emtr->mGroupID = 0;
    emtr->mResMgrID = resMgrId;
    emtr->mPortWork = NULL;
    emtr->mPortLookStale = 0;
    emtr->mPortLookByPar = 0;
    wd->mpEmtr = emtr;
    wd->mpRes = res;
    wd->mpResMgr = mgr->pResMgrAry[resMgrId];
    int made = 0;
    for (int i = 0; i < 2; ++i) {
        JPABaseParticle* p = emtr->createParticle();
        if (p == NULL)
            break;
        ++made;
        if (res->getCsp() != NULL)
            emtr->createChild(p);
    }
    int result = 0;
    if (made > 0) {
        port_gx_marker(kPwWarmBegin);
        res->draw(wd, emtr);
        port_gx_marker(kPwWarmEnd);
        result = __PortGXDrawSerial != serial0 ? 1 : 0;
    }
    emtr->deleteAllParticle();
    memcpy(gx, &sGx, sizeof sGx);
    __PortGXDrawSerial = serial0;
    memcpy(wd, &sSaved, sizeof sSaved);
    return result;
}
}  // namespace

// (EffectSystem.cpp / SingleEmitter.cpp) an effect a stage's actor set up: its resource user index and draw group. Game thread.
extern "C" void port_prewarm_note_effect(unsigned resUsrIdx, unsigned group, int oneTime) {
    if ((!PwOn() && !sPw.census) || sPw.all || group >= kPwGroups)
        return;
    const uint32_t key = port_scene_loading_key ? port_scene_loading_key() : 0;
    if (key != 0 && key != sPw.key) {  // (a new stage: its actors are set up while it loads)
        sPw.key = key;
        sPw.once.clear();
        sPw.cont.clear();
        sPw.doneMask.clear();
        sPw.order.clear();
        sPw.groupsDrawn = 0;
        sPw.warmed = sPw.skipped = sPw.noDraw = sPw.spentUs = sPw.scopes = sPw.frames = 0;
    }
    const u32 pair = (static_cast< u32 >(resUsrIdx & 0xFFFFu) << 8) | group;
    PortHostVector< u32 >& v = oneTime ? sPw.once : sPw.cont;
    for (u32 x : sPw.once)
        if (x == pair)
            return;
    for (u32 x : sPw.cont)
        if (x == pair)
            return;
    PortHostAllocScope scope;
    v.push_back(pair);
    sPw.dirtyOrder = true;
}

// (JPAEmitterManager::draw) a group's slice of the warm-up: pairs of this group not done yet, within the frame's budget.
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
extern "C" uint32_t port_scene_key(void) __attribute__((weak));
void PortPrewarmGroup(JPAEmitterManager* mgr, u8 group) {
    PortTexEnumSlice();    // (texwarm30: once a frame; nothing unless the renderer asked: GXV_TEXWARM>0 with GXV_TEXWARM_KEY=1)
    PortStageEnumSlice();  // (precomp26: once a frame, behind a closed screen only; it does nothing with GXV_STAGE_ENUM=0)
    PortRehearseSlice();   // (dive27: likewise; nothing with GXV_REHEARSE=0)
    if (!PwOn() && sPw.census && group < kPwGroups && port_log_summary && port_scene_key && port_screen_quiet) {
        // (the census: once per stage, at its first picture in play, what mode 1 would warm)
        sPw.groupsDrawn |= static_cast< u16 >(1u << group);
        const uint32_t key = port_scene_key();
        if (key != 0 && key != sPw.censusLogged && port_screen_quiet() == 0) {
            sPw.censusLogged = key;
            unsigned res = 0, groups[kPwGroups] = {};
            for (size_t i = 0; i < sPw.once.size() + sPw.cont.size(); ++i) {
                const u32 x = i < sPw.once.size() ? sPw.once[i] : sPw.cont[i - sPw.once.size()];
                ++groups[x & 0xFFu];
                bool dup = false;
                for (size_t j = 0; j < i && !dup; ++j)
                    dup = ((j < sPw.once.size() ? sPw.once[j] : sPw.cont[j - sPw.once.size()]) >> 8) == (x >> 8);
                if (!dup) ++res;
            }
            JPAResourceManager* rm = mgr->pResMgrAry[0];
            port_log_summary("prewarm census: stage %08x: %u effect resources in %u (resource, draw group) pairs set up by the stage's actors (%u one-time, %u "
                             "continuous; by group 0-8: %u %u %u %u %u %u %u %u %u; groups drawn so far %03x), of %u resources in the effect archive; "
                             "GXV_PREWARM=0: nothing warmed",
                             key, res, static_cast< unsigned >(sPw.once.size() + sPw.cont.size()), static_cast< unsigned >(sPw.once.size()),
                             static_cast< unsigned >(sPw.cont.size()), groups[0], groups[1], groups[2], groups[3], groups[4], groups[5], groups[6], groups[7],
                             groups[8], static_cast< unsigned >(sPw.groupsDrawn), rm != NULL ? static_cast< unsigned >(rm->mResNum) : 0u);
        }
        return;
    }
    if (!PwOn() || group >= kPwGroups)
        return;
    if (!port_screen_quiet || port_screen_quiet() != 2)
        return;  // (only behind a closed screen: never in play, never in the pause menu)
    sPw.groupsDrawn |= static_cast< u16 >(1u << group);
    const unsigned long long t0 = port_par_ptcl_now();
    const unsigned long long frame = port_gx_recording_frame ? port_gx_recording_frame() : 0;
    if (frame != sPw.frame) {
        sPw.frame = frame;
        sPw.frameSpentUs = 0;
    }
    const bool budget = sPw.mode == 1;
    if (budget && sPw.frameSpentUs >= sPw.frameUs)
        return;
    bool any = false;
    if (sPw.all) {
        JPAResourceManager* rm = mgr->pResMgrAry[0];
        if (rm == NULL)
            return;
        if (sPw.allDone.size() != rm->mResNum) {
            PortHostAllocScope scope;
            sPw.allDone.assign(rm->mResNum, 0);
        }
        for (u16 i = 0; i < rm->mResNum; ++i) {
            if (sPw.allDone[i] & (1u << group))
                continue;
            const int r = PwDrawOne(mgr, 0, rm->mpResArr[i]);
            sPw.allDone[i] |= static_cast< u16 >(1u << group);
            any = true;
            if (r > 0) ++sPw.warmed; else if (r == 0) ++sPw.noDraw; else ++sPw.skipped;
            if (budget && sPw.frameSpentUs + (port_par_ptcl_now() - t0) >= sPw.frameUs)
                break;
        }
    } else {
        if (sPw.dirtyOrder) {
            PortHostAllocScope scope;
            sPw.order.clear();
            for (u32 x : sPw.once) sPw.order.push_back(x);
            for (u32 x : sPw.cont) sPw.order.push_back(x);
            sPw.doneMask.resize(sPw.order.size(), 0);
            sPw.dirtyOrder = false;
        }
        JPAResourceManager* rm = mgr->pResMgrAry[0];
        for (size_t i = 0; i < sPw.order.size() && rm != NULL; ++i) {
            if (sPw.doneMask[i] || (sPw.order[i] & 0xFFu) != group)
                continue;
            const int r = PwDrawOne(mgr, 0, rm->getResource(static_cast< u16 >(sPw.order[i] >> 8)));
            sPw.doneMask[i] = 1;
            any = true;
            if (r > 0) ++sPw.warmed; else if (r == 0) ++sPw.noDraw; else ++sPw.skipped;
            if (budget && sPw.frameSpentUs + (port_par_ptcl_now() - t0) >= sPw.frameUs)
                break;
        }
    }
    if (any) {
        const unsigned long long us = port_par_ptcl_now() - t0;
        if (sPw.frameSpentUs == 0) ++sPw.frames;
        sPw.frameSpentUs += us;
        sPw.spentUs += us;
        ++sPw.scopes;
    }
}

// (the back end's StageWarmHold) effects not drawn in a scope yet, of the groups the scene has drawn since it was built.
extern "C" unsigned port_prewarm_pending(void) {
    if (!PwOn())
        return 0;
    unsigned n = 0;
    if (sPw.all) {
        for (u16 m : sPw.allDone)
            n += static_cast< unsigned >(__builtin_popcount(sPw.groupsDrawn & ~m));
        return sPw.allDone.empty() ? (sPw.groupsDrawn ? 1u : 0u) : n;
    }
    for (size_t i = 0; i < sPw.order.size() && i < sPw.doneMask.size(); ++i)
        if (!sPw.doneMask[i] && (sPw.groupsDrawn & (1u << (sPw.order[i] & 0xFFu))))
            ++n;
    return n;
}
// (the back end's "prewarm:" line) [0] resources registered, [1] (resource, group) pairs, [2] warmed (a draw was sent), [3] left, [4] skipped
// (no resource / shape) and drew nothing, [5] game-thread microseconds, [6] scopes, [7] frames with a slice
extern "C" void port_prewarm_game_counters(uint64_t out[8]) {
    unsigned long long pairs = sPw.all ? static_cast< unsigned long long >(sPw.allDone.size()) * __builtin_popcount(sPw.groupsDrawn) : sPw.order.size();
    out[0] = sPw.all ? sPw.allDone.size() : 0;
    if (!sPw.all) {
        unsigned long long res = 0;
        for (size_t i = 0; i < sPw.order.size(); ++i) {
            bool dup = false;
            for (size_t j = 0; j < i && !dup; ++j)
                dup = (sPw.order[j] >> 8) == (sPw.order[i] >> 8);
            if (!dup) ++res;
        }
        out[0] = res;
    }
    out[1] = pairs;
    out[2] = sPw.warmed;
    out[3] = port_prewarm_pending();
    out[4] = sPw.skipped + sPw.noDraw;
    out[5] = sPw.spentUs;
    out[6] = sPw.scopes;
    out[7] = sPw.frames;
}
#endif

#include "JSystem/JParticle/JPAListInline.hpp"

template JPAList< JPABaseParticle >::JPAList();
template JPANode< JPABaseParticle >::JPANode();
template JPANode< JPABaseParticle >::~JPANode();

#include "JSystem/JParticle/JPAEmitterInline.hpp"
#include "JSystem/JSupport/JSUListInline.hpp"
