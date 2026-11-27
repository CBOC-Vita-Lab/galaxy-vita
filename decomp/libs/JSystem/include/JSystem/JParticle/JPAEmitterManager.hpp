#pragma once

#include "JSystem/JGeometry/TVec.hpp"
#include "JSystem/JParticle/JPADrawInfo.hpp"
#include "JSystem/JParticle/JPAList.hpp"
#include "JSystem/JSupport/JSUList.hpp"
#include <revolution/types.h>

class JPAEmitterCallBack;
class JPAParticleCallBack;
class JPABaseEmitter;
class JPAResourceManager;
class JPABaseParticle;
class JKRHeap;
struct JPAEmitterWorkData;

class JPAEmitterManager {
public:
    JPAEmitterManager(u32, u32, JKRHeap*, u8, u8);
    JPABaseEmitter* createSimpleEmitterID(JGeometry::TVec3< f32 > const&, u16, u8, u8, JPAEmitterCallBack*, JPAParticleCallBack*);
    void calc(u8);
#ifdef TARGET_PC
    // ParticleCalcExecutor::movementNormal's nine calcs (groups 0-8, in order); GXV_PAR_PTCL: the independent emitters on two cores.
    void calcNormal();
#endif
    void draw(JPADrawInfo const*, u8);
    void forceDeleteAllEmitter();
    void forceDeleteGroupEmitter(u8);
    void forceDeleteEmitter(JPABaseEmitter*);
    void entryResourceManager(JPAResourceManager*, u8);
    void clearResourceManager(u8);
    void calcYBBCam();
    JPAResourceManager* getResourceManager(u16 idx) const {
        return pResMgrAry[idx];
    }
    JPAResourceManager* getResourceManager(u8 res_mgr_id) const {
        return pResMgrAry[res_mgr_id];
    }
    int getEmitterNumber() const {
        return emtrNum - mFreeEmtrList.getNumLinks();
    }

public:
    /* 0x00 */ JSUList< JPABaseEmitter >* pEmtrUseList;
    /* 0x04 */ JSUList< JPABaseEmitter > mFreeEmtrList;
    /* 0x10 */ JPAList< JPABaseParticle > mPtclPool;
    /* 0x1C */ JPAResourceManager** pResMgrAry;
    /* 0x20 */ JPAEmitterWorkData* pWd;
    /* 0x24 */ u32 emtrNum;
    /* 0x28 */ u32 ptclNum;
    /* 0x2C */ u8 gidMax;
    /* 0x2D */ u8 ridMax;
#ifdef TARGET_PC
    // GXV_PAR_PTCL: the emitter and particle arrays made in the constructor (the verify mode saves and restores them) and the state of
    // calcNormal (JPAEmitterManager.cpp, made on the first parallel turn).
    JPABaseEmitter* mPortEmtrArr = nullptr;
    JPANode< JPABaseParticle >* mPortPtclArr = nullptr;
    struct JPAPortPar* mPortPar = nullptr;
#endif
};

#ifdef TARGET_PC
// GXV_PAR_PTCL: the particle pool is shared by the emitters of one calcNormal; while its threads run (gJpaPoolShared) a pop or a push
// takes a spin lock, otherwise these are the list's own pop_front / push_front.
extern bool gJpaPoolShared;
JPANode< JPABaseParticle >* jpaPoolPop(JPAList< JPABaseParticle >* pool);
void jpaPoolPush(JPAList< JPABaseParticle >* pool, JPANode< JPABaseParticle >* node);
#endif
