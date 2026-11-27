#pragma once

#include <JSystem/JParticle/JPAParticle.hpp>

class MultiEmitterParticleCallBack : public JPAParticleCallBack {
public:
    MultiEmitterParticleCallBack();

    virtual ~MultiEmitterParticleCallBack() {
    }
#ifdef TARGET_PC
    virtual int portCalcKind() const {
        return 1;  // (GXV_PAR_PTCL: this base class's execute is JPAParticleCallBack's empty one; the subclasses below say 2)
    }
#endif
};
