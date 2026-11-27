#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

static float Half(double x,int mode) {
    assert(x>=0 && x<=4);
    _Float16 h=static_cast<_Float16>(x);float nearest=static_cast<float>(h);
    uint16_t bits;std::memcpy(&bits,&h,2);
    if(mode==1 && nearest>x)--bits;
    if(mode==2 && nearest<x)++bits;
    if(mode==3 && (bits&0x7c00)==0)bits=0;
    std::memcpy(&h,&bits,2);return static_cast<float>(h);
}
static float Sum(const std::array<float,4>& a,int tree,int mode) {
    const auto add=[&](float x,float y){return Half(double(x)+double(y),mode);};
    switch(tree) {
    case 0:return add(add(add(a[0],a[1]),a[2]),a[3]);
    case 1:return add(add(a[0],add(a[1],a[2])),a[3]);
    case 2:return add(a[0],add(add(a[1],a[2]),a[3]));
    case 3:return add(a[0],add(a[1],add(a[2],a[3])));
    default:return add(add(a[0],a[1]),add(a[2],a[3]));
    }
}
static unsigned Quantize(float x){return unsigned(std::lround(std::clamp(x,0.0f,1.0f)*255.0f));}
int main() {
    // Universal directed-rounding envelope, not tied to one compiler association.
    constexpr double inputError=0x1p-11;
    constexpr double sumError=4*inputError+0x1p-10+2*0x1p-9;
    constexpr double floatEnvelope=0x1p-16;
    // Both candidate and control may have one final half output conversion before UNORM8.
    constexpr double bound=1.5*0.25*sumError+floatEnvelope+2*inputError;
    static_assert(bound<1.0/255.0);
    assert(Half(1.0+0x1p-24,2)==1.0f+0x1p-10f);
    uint32_t seed=0x0badcafe;
    const auto next=[&](){seed=seed*1664525u+1013904223u;return float(seed>>8)/16777215.0f;};
    const float strengths[]={0,0.05f,0.4f,0.8f,1,1.5f};
    double worst=0;unsigned worstByte=0;uint64_t cases=0,negative=0,saturated=0;
    for(unsigned sample=0;sample<24000;++sample) {
        std::array<float,5> raw;
        for(unsigned i=0;i<5;++i) {
            if(sample<256)raw[i]=float((sample+i*47)%256)/255.0f;
            else if(sample<512)raw[i]=float((sample>>(i%4))&1);
            else if(sample<768)raw[i]=std::ldexp(next(),-14);
            else raw[i]=next();
        }
        if(sample>=768 && sample<1024)raw[0]=float(sample-768)/255.0f;
        const float strength=strengths[sample%6];
        const double reference=double(raw[0])+(double(raw[0])-(double(raw[1])+raw[2]+raw[3]+raw[4])*0.25)*strength;
        negative+=reference<0;saturated+=reference>1;
        for(int mode=0;mode<4;++mode) {
            std::array<unsigned,4> order={0,1,2,3};
            do {
                std::array<float,4> a;for(unsigned i=0;i<4;++i)a[i]=Half(raw[order[i]+1],mode);
                const float center=raw[0];
                for(int tree=0;tree<5;++tree) {
                    const float sum=Sum(a,tree,mode);
                    const float outputs[]={center+(center-sum*0.25f)*strength,
                        std::fma(center-sum*0.25f,strength,center),
                        std::fma(-strength*0.25f,sum,center*(1.0f+strength))};
                    for(float output:outputs) {
                        const double error=std::abs(double(output)-reference);
                        unsigned byteError=unsigned(std::abs(int(Quantize(output))-int(Quantize(float(reference)))));
                        for(int finalMode=0;finalMode<3;++finalMode) {
                            const float packed=Half(std::clamp(double(output),0.0,1.0),finalMode);
                            const float control=Half(std::clamp(reference,0.0,1.0),finalMode==1?2:finalMode==2?1:0);
                            byteError=std::max(byteError,unsigned(std::abs(int(Quantize(packed))-int(Quantize(control)))));
                        }
                        worst=std::max(worst,error);worstByte=std::max(worstByte,byteError);++cases;
                        assert(std::isfinite(output)&&error<=bound&&byteError<=1);
                    }
                }
            } while(std::next_permutation(order.begin(),order.end()));
        }
    }
    assert(negative&&saturated);
    std::printf("PASS cases=%llu all120_neighbor_orders=1 strengths=0..1.5 exact_directed_add_witness=1 center=float half_modes=nearest/down/up/ftz full_correction_forms=3 final_pack=direct/nearest/opposed_directed bound=%.10f worst=%.10f max_rgba8_delta=%u negative=%llu saturated=%llu physical_vita=false\n",
        static_cast<unsigned long long>(cases),bound,worst,worstByte,
        static_cast<unsigned long long>(negative),static_cast<unsigned long long>(saturated));
}
