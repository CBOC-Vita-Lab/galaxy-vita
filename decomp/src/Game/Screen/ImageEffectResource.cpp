#include "Game/Screen/ImageEffectResource.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <JSystem/JUtility/JUTVideo.hpp>

ImageEffectResource::ImageEffectResource()
    : _0(nullptr), _4(nullptr), _8(nullptr), _C(nullptr), _10(nullptr), _14(nullptr), _18(nullptr), _1C(nullptr), _20(nullptr) {
}

void ImageEffectResource::createBloomTexture() {
    if (_0 == nullptr) {
        _0 = createTexture(1.0f, GX_TF_RGBA8);
    }

    if (_4 == nullptr) {
        _4 = createTexture(0.5f, GX_TF_RGB565);
    }

    if (_C == nullptr) {
        _C = createTexture(0.25f, GX_TF_RGB565);
    }

    if (_10 == nullptr) {
        _10 = createTexture(0.25f, GX_TF_RGB565);
    }

    if (_14 == nullptr) {
        _14 = createTexture(0.125f, GX_TF_RGB565);
    }

    if (_18 == nullptr) {
        _18 = createTexture(0.125f, GX_TF_RGB565);
    }

    if (_1C == nullptr) {
        _1C = createTexture(0.25f, GX_TF_I8);
    }

    if (_20 == nullptr) {
        _20 = createTexture(0.25f, GX_TF_RGB565);
    }

#ifdef TARGET_PC
    if (_24 == nullptr) {  // (GXV_BLOOM_FOLD=2's glow: 2 KB; made always, a stage's switch can change after the effect was made)
        _24 = createTexture(0.0625f, GX_TF_RGB565);
    }
#endif
}

void ImageEffectResource::createBlurTexture() {
    if (_0 == nullptr) {
        _0 = createTexture(1.0f, GX_TF_RGBA8);
    }

    if (_8 == nullptr) {
        _8 = createTexture(0.5f, GX_TF_I8);
    }
}

void ImageEffectResource::createDOFTexture() {
    if (_0 == nullptr) {
        _0 = createTexture(1.0f, GX_TF_RGBA8);
    }

    if (_4 == nullptr) {
        _4 = createTexture(0.5f, GX_TF_RGB565);
    }

    if (_C == nullptr) {
        _C = createTexture(0.25f, GX_TF_RGB565);
    }

    if (_10 == nullptr) {
        _10 = createTexture(0.25f, GX_TF_RGB565);
    }

    if (_8 == nullptr) {
        _8 = createTexture(0.5f, GX_TF_I8);
    }
}

JUTTexture* ImageEffectResource::createTexture(f32 scale, GXTexFmt format) {
    JUTTexture* pTexture;

    MR::CurrentHeapRestorer heapRestorer(MR::getSceneHeapGDDR3());

    pTexture = new JUTTexture(scale * (s32)JUTVideo::getManager()->getRenderMode()->fbWidth,
                              scale * (s32)JUTVideo::getManager()->getRenderMode()->efbHeight, format);
    pTexture->mWrapS = GX_CLAMP;
    pTexture->mWrapT = GX_CLAMP;
    pTexture->mMinType = GX_LINEAR;
    pTexture->mMagType = GX_LINEAR;

    return pTexture;
}
