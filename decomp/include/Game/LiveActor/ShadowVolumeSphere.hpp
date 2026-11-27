#pragma once

#include "Game/LiveActor/ShadowVolumeModel.hpp"

class ShadowVolumeSphere : public ShadowVolumeModel {
public:
    ShadowVolumeSphere();

    virtual ~ShadowVolumeSphere();
    virtual void loadModelDrawMtx() const;
    virtual bool isDraw() const;

    void setRadius(f32);

#ifdef TARGET_PC
    // boss23 (PETARI_BOSS_PSHADOW): the shadow of a small fast projectile drawn as one flat disc in the volume pass (see the .cpp).
    virtual void draw() const override;
    void portDrawDisc() const;
#endif

    /* 0x20 */ f32 mRadius;
#ifdef TARGET_PC
    bool mPortProjectile = false;  // (set by MR::portMarkProjectileShadow for a boss's fireballs, bombs)
#endif
};