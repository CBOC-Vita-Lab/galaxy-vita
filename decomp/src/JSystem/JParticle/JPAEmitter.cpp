#define JPA_LIST_DEFER_INLINE
#include "JSystem/JParticle/JPAEmitter.hpp"
#include "JSystem/JParticle/JPABaseShape.hpp"
#include "JSystem/JParticle/JPAEmitterManager.hpp"
#include "JSystem/JParticle/JPAParticle.hpp"
#include "JSystem/JParticle/JPAResourceManager.hpp"

JPAEmitterCallBack::~JPAEmitterCallBack() {
}

#ifdef TARGET_PC
// GXV_PAR_PTCL: a calc that runs on a work area of its own (mPortWork) makes its particles with that one, and the pool is taken
// through jpaPoolPop / jpaPoolPush (a spin lock only while the threads run). Serial: mPortWork is the manager's pWd, as before, and
// the pool is the list itself.
JPABaseParticle* JPABaseEmitter::createParticle() {
    JPAEmitterWorkData* const wd = mPortWork != nullptr ? mPortWork : mpEmtrMgr->pWd;
    JPANode< JPABaseParticle >* node = jpaPoolPop(mpPtclPool);  // (NULL when the pool is empty, as the getNum() test was)
    if (node != NULL) {
        mAlivePtclBase.push_front(node);
        mpRes->getDyn()->calc(wd);
        node->mData.init_p(wd);
        return &node->mData;
    }

    return NULL;
}

JPABaseParticle* JPABaseEmitter::createChild(JPABaseParticle* parent) {
    JPAEmitterWorkData* const wd = mPortWork != nullptr ? mPortWork : mpEmtrMgr->pWd;
    JPANode< JPABaseParticle >* node = jpaPoolPop(mpPtclPool);
    if (node != NULL) {
        mAlivePtclChld.push_front(node);
        node->mData.init_c(wd, parent);
        return &node->mData;
    }

    return NULL;
}
#else
JPABaseParticle* JPABaseEmitter::createParticle() {
    if (mpPtclPool->getNum() != 0) {
        JPANode< JPABaseParticle >* node = mpPtclPool->pop_front();
        mAlivePtclBase.push_front(node);
        mpRes->getDyn()->calc(mpEmtrMgr->pWd);
        node->mData.init_p(mpEmtrMgr->pWd);
        return &node->mData;
    }

    return NULL;
}

JPABaseParticle* JPABaseEmitter::createChild(JPABaseParticle* parent) {
    if (mpPtclPool->getNum() != 0) {
        JPANode< JPABaseParticle >* node = mpPtclPool->pop_front();
        mAlivePtclChld.push_front(node);
        node->mData.init_c(mpEmtrMgr->pWd, parent);
        return &node->mData;
    }

    return NULL;
}
#endif

void JPABaseEmitter::deleteAllParticle() {
#ifdef TARGET_PC
    while (mAlivePtclBase.getNum())
        jpaPoolPush(mpPtclPool, mAlivePtclBase.pop_back());
    while (mAlivePtclChld.getNum())
        jpaPoolPush(mpPtclPool, mAlivePtclChld.pop_back());
#else
    while (mAlivePtclBase.getNum())
        mpPtclPool->push_front(mAlivePtclBase.pop_back());
    while (mAlivePtclChld.getNum())
        mpPtclPool->push_front(mAlivePtclChld.pop_back());
#endif
}

bool JPABaseEmitter::processTillStartFrame() {
    JPADynamicsBlock* dyn = mpRes->getDyn();
    s16 startFrame = dyn->getStartFrame();

    if (mWaitTime >= startFrame)
        return true;

    if (!(mStatus & 2))
        mWaitTime++;

    return false;
}

bool JPABaseEmitter::processTermination() {
    if (checkStatus(0x100)) {
        return true;
    }

    if (mMaxFrame == 0) {
        return false;
    }
    if (mMaxFrame < 0) {
        setStatus(8);
        return getParticleNumber() == 0;
    }
    if (mTick >= mMaxFrame) {
        setStatus(8);
        if (checkStatus(0x40)) {
            return 0;
        }
        return getParticleNumber() == 0;
    }
    return false;
}

void JPABaseEmitter::calcEmitterGlobalPosition(JGeometry::TVec3< f32 >* dst) const {
    Mtx mtx;
    PSMTXScale(mtx, mGlobalScl.x, mGlobalScl.y, mGlobalScl.z);
    PSMTXConcat(mGlobalRot, mtx, mtx);
    mtx[0][3] = mGlobalTrs.x;
    mtx[1][3] = mGlobalTrs.y;
    mtx[2][3] = mGlobalTrs.z;
    PSMTXMultVec(mtx, mLocalTrs, *dst);
}

#include "JSystem/JParticle/JPAListInline.hpp"
