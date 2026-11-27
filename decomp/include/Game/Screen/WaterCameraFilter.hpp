#pragma once

#include "Game/LiveActor/LiveActor.hpp"
#include "Game/Util/Color.hpp"

class JUTTexture;

class WaterCameraFilter : public LiveActor {
public:
    WaterCameraFilter();

    virtual void init(const JMapInfoIter& rIter);
    virtual void draw() const;
    virtual void control();

    void exeAirToWater();
    void exeWaterToAir();
    void exeWater();
    void exeAir();
    void loadMaterial() const;
#ifdef TARGET_PC
    void drawBody() const;  // (dive27: draw() without the nerve test and the alpha-0 skip)
    void rehearse();        // (dive27 GXV_REHEARSE: the real draw once, in a warm-up scope behind the closed wipe)
#endif

    inline void initScreenTex();

    /* 0x8C */ f32 mWaterAlpha;
    /* 0x90 */ f32 mWavePhase;
    /* 0x94 */ Color8 mWaterColor;
    /* 0x98 */ JUTTexture* mScreenTex;
    /* 0x9C */ JUTTexture* mFilterTex;
#ifdef TARGET_PC
    bool mHalfCap = false;  // (dive27 GXV_WATER_HALFCAP: mScreenTex is half the picture's size)
#endif
};
