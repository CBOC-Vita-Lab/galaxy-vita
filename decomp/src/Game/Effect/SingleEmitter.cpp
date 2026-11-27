#include "Game/Effect/SingleEmitter.hpp"
#include "Game/Effect/EffectSystem.hpp"
#include "Game/Effect/EffectSystemUtil.hpp"

SingleEmitter::SingleEmitter() {
    mEmitter = nullptr;
    _4 = 0;
    mGroupId = 0;
    mSpawn = SINGLE_EMITTER_UNDEF_SPAWN;
}

void SingleEmitter::init(u16 unk) {
    _4 = unk;
}

void SingleEmitter::deleteEmitter() {
    if (isValid()) {
        MR::Effect::deleteParticleEmitter(mEmitter);
    }
}

#ifdef TARGET_PC
// prewarm24 (GXV_PREWARM, decomp JPAEmitterManager.cpp): the effects a stage's actors set up, for the warm-up behind its closed screen
extern "C" void port_prewarm_note_effect(unsigned resUsrIdx, unsigned group, int oneTime) __attribute__((weak));
#endif

void SingleEmitter::scanParticleEmitter(EffectSystem* pSystem) {
    ParticleEmitter* emitter;
    if (mSpawn == SINGLE_EMITTER_UNDEF_SPAWN) {
        emitter = pSystem->createEmitter(_4, mGroupId, 0);
        mSpawn = emitter->isContinuousParticle() ? SINGLE_EMITTER_CONTINUOUS_SPAWN : SINGLE_EMITTER_ONE_TIME_SPAWN;
        pSystem->forceDeleteEmitter(emitter);
    }
#ifdef TARGET_PC
    if (port_prewarm_note_effect) {
        port_prewarm_note_effect(_4, mGroupId, mSpawn == SINGLE_EMITTER_ONE_TIME_SPAWN ? 1 : 0);
    }
#endif
}

bool SingleEmitter::isOneTime() const {
    if (mEmitter) {
        return !mEmitter->isContinuousParticle();
    } else {
        return !mSpawn;
    }
}

bool SingleEmitter::isValid() const {
    if (mEmitter == nullptr) {
        return false;
    } else {
        return mEmitter->mEmitter != nullptr;
    }
}

void SingleEmitter::link(ParticleEmitter* pEmitter) {
    mEmitter = pEmitter;
    MR::Effect::setLinkSingleEmitter(pEmitter, this);
}

void SingleEmitter::unlink() {
    MR::Effect::setLinkSingleEmitter(mEmitter, nullptr);
    mEmitter = nullptr;
}