#include "Game/System/DrawBufferExecuter.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/System/DrawBuffer.hpp"
#include "Game/Util/LightUtil.hpp"
#include <algorithm>

DrawBufferExecuter::DrawBufferExecuter(const char* pName, J3DModel* pModel, s32 a1)
    : mActors(nullptr), mMaxNumActors(0), mNumActors(0), mName(pName), mDrawBuffer(nullptr), mLightType(-1), mDrawBufferCount(0) {
    mDrawBuffer = new DrawBuffer(pModel);
#ifdef TARGET_PC
    mDrawBuffer->mPortName = pName;  // (GXV_REPLAY25: the census's family)
#endif
}

void DrawBufferExecuter::allocateActorListBuffer() {
    s32 count = mDrawBufferCount;
    if (count != 0) {
        mActors = new LiveActor*[count];
        mMaxNumActors = count;
        mDrawBuffer->init(mDrawBufferCount);
    }
}

void DrawBufferExecuter::add(LiveActor* pActor) {
    s32 lastActor = mNumActors;
    mNumActors++;
    mActors[lastActor] = pActor;
    mDrawBuffer->add(pActor);
}

void DrawBufferExecuter::remove(LiveActor* pActor) {
    LiveActor** it = std::find(mActors, mActors + mNumActors, pActor);
    mActors[it - mActors] = getActor(mNumActors - 1);
    mNumActors--;
    mDrawBuffer->remove(pActor);
}

void DrawBufferExecuter::findLightInfo(LiveActor* pActor) {
    MR::initActorLightInfoDrawBuffer(pActor, mDrawBuffer);
}

void DrawBufferExecuter::onExecuteLight(s32 lightType) {
    mLightType = lightType;
}
void DrawBufferExecuter::offExecuteLight() {
    mLightType = -1;
}

#ifdef TARGET_PC
extern "C" int port_flush_prefetch_on(void);  // (GXV_FLUSH_PREFETCH, flushcut: platform/src/port/perf.cpp)
extern "C" void port_prefetch_actor(const void* actor);
extern "C" void port_prefetch_model(const void* actor);
#endif

void DrawBufferExecuter::calcViewAndEntry() {
#ifdef TARGET_PC
    // The view pass walks every actor of every group; the far models' lines are cold (the turns of game logic since the last picture
    // ran in between). A prefetch changes no value: the actors a few steps ahead (they are in the group's active list, so live objects
    // the loop calls anyway), then their model manager and model.
    if (mNumActors > 8 && port_flush_prefetch_on()) {
        const s32 n = mNumActors;
        for (s32 k = 0; k < 6; ++k) {
            port_prefetch_actor(mActors[k]);
        }
        for (s32 i = 0; i < n; ++i) {
            if (i + 6 < n) {
                port_prefetch_actor(mActors[i + 6]);
            }
            if (i + 3 < n) {
                port_prefetch_model(mActors[i + 3]);
            }
            mActors[i]->calcViewAndEntry();
        }
        return;
    }
#endif
    std::for_each(&mActors[0], &mActors[mNumActors], std::mem_func(&LiveActor::calcViewAndEntry));
}

void DrawBufferExecuter::drawOpa() const {
    if (mLightType != -1) {
        MR::loadLight(mLightType);
    }
    mDrawBuffer->drawOpa();
}

void DrawBufferExecuter::drawXlu() const {
    if (mLightType != -1) {
        MR::loadLight(mLightType);
    }
    mDrawBuffer->drawXlu();
}
