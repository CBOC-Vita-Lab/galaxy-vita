#include "gx_copy_pixels.h"
#include <new>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace gpu {
namespace {
struct Sample { uint32_t c[4]{}; };
using GammaTable=std::array<unsigned char,256>;
GammaTable gammaTable(double exponent) {
    GammaTable table{};
    for (unsigned i=0;i<256;++i) table[i]=static_cast<unsigned char>(std::floor(std::pow(double(i)/255,exponent)*255+0.5));
    return table;
}
const GammaTable* gammaTable(unsigned selector) {
    if (!selector) return nullptr;
    if (selector==1) { static const auto table=gammaTable(1.0/1.7); return &table; }
    static const auto table=gammaTable(1.0/2.2); return &table;
}
uint32_t roundEvenQuarter(uint32_t value) {
    const uint32_t integer=value>>2,remainder=value&3;
    return integer+unsigned(remainder>2 || (remainder==2 && (integer&1)));
}
struct Sampling {
    const GxCopyPlane& plane;
    const GxCopyGeometry& geometry;
    unsigned scale,pixelFormat;
    bool clampTop,clampBottom;
    int32_t clamp(int32_t y) const {
        if (clampTop) y=std::max(y,int32_t(geometry.top));
        if (clampBottom) y=std::min(y,int32_t(geometry.top+geometry.sourceHeight-1));
        return y;
    }
    Sample sample(int32_t x,int32_t y) const {
        Sample sum;
        for (unsigned dy=0;dy<scale;++dy) for (unsigned dx=0;dx<scale;++dx) {
            const auto* p=plane.data+size_t(int64_t(clamp(y+int32_t(dy)))-plane.top)*plane.stride+
                size_t(int64_t(x)+dx-plane.left)*4;
            if (plane.depth) sum.c[0]+=(uint32_t(p[0])<<16)|(uint32_t(p[1])<<8)|p[2];
            else for (unsigned c=0;c<4;++c) sum.c[c]+=p[c];
        }
        if (plane.depth) {
            const uint32_t z=sum.c[0]/(scale*scale);
            return {{z>>16,(z>>8)&255,z&255,255}};
        }
        for (auto& c:sum.c) {
            c=scale==1 ? c : pixelFormat==0 ? c>>2 : roundEvenQuarter(c);
        }
        if (pixelFormat==1) for (auto& c:sum.c) c=(c&0xFC)|(c>>6);
        else {
            if (pixelFormat==2) {
                sum.c[0]=(sum.c[0]&0xF8)|(sum.c[0]>>5);
                sum.c[1]=(sum.c[1]&0xFC)|(sum.c[1]>>6);
                sum.c[2]=(sum.c[2]&0xF8)|(sum.c[2]>>5);
            }
            sum.c[3]=255;
        }
        return sum;
    }
};
}
int produceGxCopyPixels(const SpanCopy& copy,const GxCopyPlane& plane,GxCopyPixels& out,std::string& error) {
    error.clear();
    auto fail=[&](int code,const char* text) { error=text; return code; };
    if (out.mData) return fail(-1,"processed pixel output is not empty");
    GxCopyGeometry geometry;
    const bool xfb=(copy.words[9]&(1u<<14))!=0;
    const int described=xfb ? describeGxXfbCopy(copy,geometry,error) : describeGxTextureCopy(copy,geometry,error);
    if (described<0) return described;
    const uint32_t control=copy.words[9],format=copy.pixelFormat&7;
    if (control&0x3000) return fail(-1,"original field sampling requires a separate producer strategy");
    if (!xfb && (control&(1u<<10))) return fail(-1,"vertical-scaled copy requires the XFB/display producer strategy");
    if (plane.depth!=(!xfb && format==3)) return fail(-1,"copy pixel type does not match captured color/Z24 plane");
    const uint64_t rowBytes=uint64_t(plane.width)*4;
    if (!plane.data || !plane.width || !plane.height || rowBytes>plane.stride ||
        uint64_t(plane.height-1)*plane.stride+rowBytes>plane.bytes)
        return fail(-1,"captured point plane rows are unavailable");
    const unsigned scale=(control&(1u<<9)) ? 2 : 1;
    unsigned coefficients[3]={
        (copy.filter[0]&63)+((copy.filter[0]>>6)&63),
        ((copy.filter[0]>>12)&63)+((copy.filter[0]>>18)&63)+(copy.filter[1]&63),
        ((copy.filter[1]>>6)&63)+((copy.filter[1]>>12)&63)};
    // A8 has no RGB consumer. Requiring those filter neighbors would reject
    // valid alpha-only copies; the unused RGB output is deterministically zero.
    if (geometry.format==7) coefficients[0]=coefficients[1]=coefficients[2]=0;
    const Sampling sampling{plane,geometry,scale,format,bool(control&1),bool(control&2)};
    // Validate the union of all actually used sample footprints up front. No
    // fallback to capture edges: original clamp bits refer to the copy rectangle.
    const int32_t x0=int32_t(geometry.left),x1=x0+int32_t(geometry.paddedWidth*scale)-1;
    const int32_t y0=sampling.clamp(int32_t(geometry.top)-(coefficients[0] ? int32_t(scale) : 0));
    const int32_t y1=sampling.clamp(int32_t(geometry.top+geometry.paddedHeight*scale-1)+
        (coefficients[2] ? int32_t(scale) : 0));
    if (int64_t(x0)<plane.left || int64_t(x1)>=int64_t(plane.left)+plane.width ||
        int64_t(y0)<plane.top || int64_t(y1)>=int64_t(plane.top)+plane.height)
        return fail(-1,"capture lacks original padded/filter/half sample footprint");
    GxCopyPixels pixels;
    pixels.mWidth=geometry.paddedWidth; pixels.mHeight=geometry.paddedHeight;
    pixels.mData.reset(new(std::nothrow) unsigned char[size_t(pixels.mWidth)*pixels.mHeight*4]);
    if (!pixels.mData) return fail(-2,"processed pixel-buffer CPU OOM");
    const GammaTable* gamma=geometry.format==7 ? nullptr : gammaTable(unsigned((control>>7)&3));
    const bool yuv=!xfb && geometry.format!=7 && (control&(3u<<15))==(3u<<15);
    // Resolve full-rate color row footprints once, preserving all original
    // filter coefficients and precision without the general resampling loop.
    if (!plane.depth && scale==1 && geometry.format!=7 && !gamma && !yuv && (format==0 || format==1 || format==3)) {
        const bool identity=!coefficients[0] && coefficients[1]==64 && !coefficients[2];
        const auto row=[&](int32_t y) {
            return plane.data+size_t(int64_t(sampling.clamp(y))-plane.top)*plane.stride+
                size_t(int64_t(geometry.left)-plane.left)*4;
        };
        const auto point=[&](const unsigned char* source,unsigned char* destination) {
            uint32_t rgba; std::memcpy(&rgba,source,4);
            if (format==1) rgba=(rgba&0xFCFCFCFCu)|((rgba>>6)&0x03030303u);
            std::memcpy(destination,&rgba,4);
        };
        for (uint32_t y=0;y<pixels.mHeight;++y) {
            const int32_t sy=int32_t(geometry.top+y);
            const auto* source=row(sy);
            auto* destination=pixels.mData.get()+size_t(y)*pixels.mWidth*4;
            if (!identity) {
                const auto* previous=coefficients[0] ? row(sy-1) : nullptr;
                const auto* next=coefficients[2] ? row(sy+1) : nullptr;
                for (uint32_t x=0;x<pixels.mWidth;++x) {
                    const size_t offset=size_t(x)*4;
                    unsigned char current[4],before[4]{},after[4]{};
                    point(source+offset,current);
                    if (previous) point(previous+offset,before);
                    if (next) point(next+offset,after);
                    for (unsigned c=0;c<3;++c) {
                        const uint32_t filtered=((uint32_t(before[c])*coefficients[0]+uint32_t(current[c])*coefficients[1]+
                            uint32_t(after[c])*coefficients[2])>>6)&511;
                        destination[offset+c]=static_cast<unsigned char>(std::min(filtered,255u));
                    }
                    destination[offset+3]=format==1 ? current[3] : 255;
                }
            } else if (format==1) {
                for (uint32_t x=0;x<pixels.mWidth;++x) {
                    uint32_t rgba; std::memcpy(&rgba,source+size_t(x)*4,4);
                    rgba=(rgba&0xFCFCFCFCu)|((rgba>>6)&0x03030303u);
                    std::memcpy(destination+size_t(x)*4,&rgba,4);
                }
            } else {
                std::memcpy(destination,source,size_t(pixels.mWidth)*4);
                for (uint32_t x=0;x<pixels.mWidth;++x) destination[size_t(x)*4+3]=255;
            }
        }
        out=std::move(pixels); return 0;
    }
    for (uint32_t y=0;y<pixels.mHeight;++y) for (uint32_t x=0;x<pixels.mWidth;++x) {
        const int32_t sx=int32_t(geometry.left+x*scale),sy=int32_t(geometry.top+y*scale);
        Sample current=sampling.sample(sx,sy),previous,next;
        if (coefficients[0]) previous=sampling.sample(sx,sy-int32_t(scale));
        if (coefficients[2]) next=sampling.sample(sx,sy+int32_t(scale));
        for (unsigned c=0;c<3;++c) {
            const uint32_t filtered=((previous.c[c]*coefficients[0]+current.c[c]*coefficients[1]+next.c[c]*coefficients[2])>>6)&511;
            current.c[c]=std::min(filtered,255u);
            if (gamma) current.c[c]=(*gamma)[current.c[c]];
        }
        if (yuv) {
            const int r=int(current.c[0]),g=int(current.c[1]),b=int(current.c[2]);
            current.c[0]=uint32_t((66*r+129*g+25*b+4096+128)>>8);
            current.c[1]=uint32_t((-38*r-74*g+112*b+32768+128)>>8);
            current.c[2]=uint32_t((112*r-94*g-18*b+32768+128)>>8);
        }
        auto* p=pixels.mData.get()+(size_t(y)*pixels.mWidth+x)*4;
        for (unsigned c=0;c<4;++c) p[c]=static_cast<unsigned char>(current.c[c]);
    }
    out=std::move(pixels); return 0;
}
}
