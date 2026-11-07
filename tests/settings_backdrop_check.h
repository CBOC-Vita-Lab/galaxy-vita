#pragma once
// Legibility checks for the Settings backdrop, shared by the UI test and the preview tool.
#include "port/settings_backdrop.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace backdrop_check {
inline double Linear(unsigned c) {const double v=c/255.0;return v<=0.04045?v/12.92:std::pow((v+0.055)/1.055,2.4);}
inline double Luminance(unsigned r,unsigned g,unsigned b) {return 0.2126*Linear(r)+0.7152*Linear(g)+0.0722*Linear(b);}
inline double Luminance(uint32_t abgr) {return Luminance(abgr&255,(abgr>>8)&255,(abgr>>16)&255);}
inline double Contrast(uint32_t a,uint32_t b) {double x=Luminance(a),y=Luminance(b);if(x<y)std::swap(x,y);return (x+0.05)/(y+0.05);}
constexpr uint32_t RGB(unsigned r,unsigned g,unsigned b) {return 0xff000000u|(b<<16)|(g<<8)|r;}
constexpr uint32_t kTextColors[]={RGB(244,249,250),RGB(165,192,199),RGB(73,220,233),RGB(249,211,91),RGB(251,135,148)};
// The picture as drawn at screen pixel (x,y): nearest-neighbour scaled, then dimmed by the row's brightness.
inline uint32_t Expected(const uint32_t* backdrop,unsigned w,unsigned h,unsigned x,unsigned y) {
    const uint32_t p=backdrop[static_cast<size_t>(y*h/544)*w+x*w/960];const unsigned dim=port::settings::BackdropDim(y);
    return 0xff000000u|(((p&255)*dim>>8))|((((p>>8)&255)*dim>>8)<<8)|((((p>>16)&255)*dim>>8)<<16);
}
struct Result {size_t textPixels=0,failures=0;double worst=1e9;};
// Every frame pixel that is exactly a menu text colour (the opaque inside of a glyph) must have contrast of at
// least `minimum` against the picture behind it. Backgrounds under value boxes and focus bars are darker still.
inline Result CheckText(const std::vector<uint32_t>& frame,const uint32_t* backdrop,unsigned w,unsigned h,double minimum) {
    Result result;
    for(unsigned y=0;y<544;++y)for(unsigned x=0;x<960;++x) {
        const uint32_t pixel=frame[static_cast<size_t>(y)*960+x];bool text=false;
        for(uint32_t color:kTextColors)text|=pixel==color;
        if(!text)continue;
        ++result.textPixels;const double c=Contrast(pixel,Expected(backdrop,w,h,x,y));
        if(c<result.worst)result.worst=c;
        if(c<minimum)++result.failures;
    }
    return result;
}
}
