#pragma once

#include "Game/Effect/MultiEmitterParticleCallBack.hpp"
#include <revolution.h>

class SpinPullParticleCallBack : public MultiEmitterParticleCallBack {
public:
    SpinPullParticleCallBack();

    virtual void execute(JPABaseEmitter*, JPABaseParticle*);
#ifdef TARGET_PC
    virtual int portCalcKind() const {
        return 2;  // (GXV_PAR_PTCL: calls MR::calcPlayerSpinPullVelocity, game code: stays on the game thread)
    }
#endif

    f32 _4;
};

class MerameraParticleCallBack : public SpinPullParticleCallBack {
public:
    virtual void execute(JPABaseEmitter*, JPABaseParticle*);
};