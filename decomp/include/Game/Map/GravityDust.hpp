#pragma once

#include "Game/Effect/MultiEmitterParticleCallBack.hpp"

class LiveActor;

class GravityDustParticleCallBack : public MultiEmitterParticleCallBack {
public:
    GravityDustParticleCallBack(const LiveActor* pActor);

    virtual ~GravityDustParticleCallBack();

    virtual void execute(JPABaseEmitter* pEmitter, JPABaseParticle* pParticle);
#ifdef TARGET_PC
    virtual int portCalcKind() const {
        return 2;  // (GXV_PAR_PTCL: calls MR::calcGravityVectorOrZero, game code: stays on the game thread)
    }
#endif

    /* 0x04 */ const LiveActor* mActor;
};
