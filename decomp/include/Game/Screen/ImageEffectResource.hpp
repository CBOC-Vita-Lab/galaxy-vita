#pragma once

#include <revolution/gx.h>

class JUTTexture;

class ImageEffectResource {
public:
    /// @brief Creates a new `ImageEffectResource`.
    ImageEffectResource();

    void createBloomTexture();
    void createBlurTexture();
    void createDOFTexture();
    JUTTexture* createTexture(f32, GXTexFmt);

    /* 0x00 */ JUTTexture* _0;
    /* 0x04 */ JUTTexture* _4;
    /* 0x08 */ JUTTexture* _8;
    /* 0x0C */ JUTTexture* _C;
    /* 0x10 */ JUTTexture* _10;
    /* 0x14 */ JUTTexture* _14;
    /* 0x18 */ JUTTexture* _18;
    /* 0x1C */ JUTTexture* _1C;
    /* 0x20 */ JUTTexture* _20;
#ifdef TARGET_PC
    JUTTexture* _24 = nullptr;  // GXV_BLOOM_FOLD=2 (bloomfold25): the reduced glow, 1/16 of the picture (40x28 at 640x456), RGB565
#endif
};
