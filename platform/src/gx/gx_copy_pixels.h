#pragma once
#include "gx_copy_bytes.h"

namespace gpu {
// Point samples in original EFB coordinates. Color is R/G/B/A8; depth is
// Z24 high/middle/low bytes, with alpha ignored. No tiled depth reinterpretation.
// The view must include every needed padded/filter/half neighbor. Unclamped
// missing samples reject, never silently edge-repeat or invent bus data.
struct GxCopyPlane {
    const unsigned char* data=nullptr;
    int32_t left=0,top=0;
    uint32_t width=0,height=0,stride=0;
    size_t bytes=0;
    bool depth=false;
};
class GxCopyPixels {
public:
    GxCopyPixels()=default;
    GxCopyTexels texels() const { return {mData.get(),mWidth,mHeight,mWidth*4,size_t(mWidth)*mHeight*4}; }
private:
    friend int produceGxCopyPixels(const SpanCopy&,const GxCopyPlane&,GxCopyPixels&,std::string&);
    std::unique_ptr<unsigned char[]> mData;
    uint32_t mWidth=0,mHeight=0;
};
// CPU producer for original copy processing, feeding the exact tiled texture
// or progressive linear XFB encoder. Source is borrowed and stable during the call; output owns all
// processed padded samples. Native capture/readback fidelity and physical
// hardware agreement are separate mandatory gates, not established by this API.
// Field/scaled XFB copies are not silently reinterpreted as progressive textures.
int produceGxCopyPixels(const SpanCopy&,const GxCopyPlane&,GxCopyPixels&,std::string& error);
}
