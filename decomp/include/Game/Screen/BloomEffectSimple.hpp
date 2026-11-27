#pragma once

#include "Game/Screen/ImageEffectBase.hpp"

class JUTTexture;

class BloomEffectSimple : public ImageEffectBase {
public:
    /// @brief Creates a new `BloomEffectSimple`.
    BloomEffectSimple();

    /// @brief Draws the `BloomEffectSimple` to the screen.
    virtual void draw() const;
#ifdef TARGET_PC
    void portRehearse() const;  // (dive27 GXV_REHEARSE)
#endif

    /* 0x14 */ JUTTexture* _14;
    /* 0x18 */ JUTTexture* _18;
    /* 0x1C */ JUTTexture* _1C;
    /* 0x20 */ JUTTexture* _20;
    /* 0x24 */ JUTTexture* _24;
    /* 0x28 */ JUTTexture* _28;
    /* 0x2C */ JUTTexture* _2C;
    /* 0x30 */ s32 mMaskFilter;
    /* 0x34 */ u8 mThreshold;
    /* 0x38 */ f32 mIntensity;
#ifdef TARGET_PC
    JUTTexture* mFoldTex = nullptr;  // GXV_BLOOM_FOLD=2 (bloomfold25): the reduced glow's texture, 1/16 of the picture
#endif
};
