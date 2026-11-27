#include <math.h>
#ifdef __GLIBC__
// GNU libc reserves these names; import it before the existing PPC wrappers.
#define __fabsf tev_test_ppc_fabsf
#define __fabs tev_test_ppc_fabs
#endif
#ifndef TEV_BURST_HEADER_ONLY
#include <JSystem/J3DGraphBase/J3DGD.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
void Burst_J3DGDSetTevColorS10(GXTevRegID,GXColorS10);
void Burst_J3DGDSetTevKColor(GXTevKColorID,GXColor);
GDLObj* __GDCurrentDL;
// Independent bytewise oracle: preserve one original color-level check and
// per-command publication, without using production BP macros or write helpers.
static void ReferenceBP(uint32_t value){
    GDLObj* dl=__GDCurrentDL;uint8_t* p=dl->ptr;
    p[0]=0x61;
    for(unsigned i=0;i<4;i++)p[1+i]=uint8_t(value>>(24-8*i));
    dl->ptr=p+5;
}
static void Original_J3DGDSetTevKColor(GXTevKColorID reg,GXColor c){
    const uint32_t id=uint32_t(reg)*2+0xe0;
    const uint32_t ra=uint32_t(c.r)|(uint32_t(c.a)<<12)|(1u<<23)|(id<<24);
    const uint32_t bg=uint32_t(c.b)|(uint32_t(c.g)<<12)|(1u<<23)|((id+1)<<24);
    GDOverflowCheck(10);ReferenceBP(ra);ReferenceBP(bg);
}
static void Original_J3DGDSetTevColorS10(GXTevRegID reg,GXColorS10 c){
    const uint32_t id=uint32_t(reg)*2+0xe0;
    const uint32_t ra=(uint32_t(c.r)&0x7ff)|((uint32_t(c.a)&0x7ff)<<12)|(id<<24);
    const uint32_t bg=(uint32_t(c.b)&0x7ff)|((uint32_t(c.g)&0x7ff)<<12)|((id+1)<<24);
    GDOverflowCheck(20);ReferenceBP(ra);ReferenceBP(bg);ReferenceBP(bg);ReferenceBP(bg);
}
static GDLObj *Redirect;
static unsigned CallbackMode;
static unsigned Callbacks;
static uint64_t Cases,Checks;
static void Need(bool ok,const char*why){++Checks;if(!ok){std::fprintf(stderr,"FAIL %s case=%llu\n",why,(unsigned long long)Cases);std::exit(1);}}
extern "C" void GDOverflowed(){
    ++Callbacks;
    if(CallbackMode==1)__GDCurrentDL=Redirect;
    if(CallbackMode==2)*__GDCurrentDL=*Redirect;
    if(CallbackMode==4)__GDCurrentDL->ptr+=5;
}
static void Case(bool isS,unsigned reg,GXColorS10 s,GXColor k,unsigned skew,unsigned overflow,bool padded=false){
    ++Cases;const unsigned bytes=isS?20:10,extent=skew+bytes+(overflow==4?5:0),allocated=extent+(padded?16:0);
    std::array<std::unique_ptr<uint8_t[]>,4> memory;
    for(auto&x:memory){x.reset(new uint8_t[allocated]);std::memset(x.get(),0xa5,allocated);}
    std::array<GDLObj,4> dl;
    for(unsigned i=0;i<4;i++)dl[i]={memory[i].get(),extent,memory[i].get()+skew,memory[i].get()+extent};
    if(overflow){dl[0].top=dl[0].ptr+bytes-1;dl[2].top=dl[2].ptr+bytes-1;}
    unsigned calls[2];
    for(unsigned mode=0;mode<2;mode++){
        __GDCurrentDL=&dl[mode*2];Redirect=&dl[mode*2+1];CallbackMode=overflow;Callbacks=0;
        if(isS){auto fn=mode?Burst_J3DGDSetTevColorS10:Original_J3DGDSetTevColorS10;fn(GXTevRegID(reg),s);}
        else{auto fn=mode?Burst_J3DGDSetTevKColor:Original_J3DGDSetTevKColor;fn(GXTevKColorID(reg),k);}
        calls[mode]=Callbacks;Need(__GDCurrentDL==&dl[mode*2+(overflow==1?1:0)],"current DL after callback");
    }
    Need(calls[0]==calls[1]&&calls[0]==unsigned(overflow!=0),"same original overflow check/callback count");
    for(unsigned i=0;i<2;i++){
        Need(std::memcmp(memory[i].get(),memory[i+2].get(),allocated)==0,"all payload and canary bytes exact");
        Need(dl[i].ptr-dl[i].start==dl[i+2].ptr-dl[i+2].start,"exact final cursor for both DLs");
        for(unsigned j=0;j<skew;j++)Need(memory[i][j]==0xa5&&memory[i+2][j]==0xa5,"prefix canary preserved");
        for(unsigned j=extent;j<allocated;j++)Need(memory[i][j]==0xa5&&memory[i+2][j]==0xa5,"trailing canary preserved");
    }
    const unsigned active=overflow==1?1:0;
    Need(dl[active].ptr==dl[active].start+extent,"exact fill, no extra output bytes");
}
int main(int argc,char**argv){
    if(argc>1){Case(true,1,GXColorS10{-32768,32767,-1,0},GXColor{1,2,3,4},3,true);return 0;}
    for(unsigned component=0;component<4;component++)for(unsigned value=0;value<65536;value++){
        GXColorS10 s{-1024,1023,-1,0};int16_t bits;uint16_t v=value;std::memcpy(&bits,&v,2);
        switch(component){case 0:s.r=bits;break;case 1:s.g=bits;break;case 2:s.b=bits;break;default:s.a=bits;}
        Case(true,value&3,s,GXColor{},value&15,false);
    }
    for(unsigned component=0;component<4;component++)for(unsigned value=0;value<256;value++){
        GXColor k{0,255,128,1};switch(component){case 0:k.r=value;break;case 1:k.g=value;break;case 2:k.b=value;break;default:k.a=value;}
        Case(false,value&3,GXColorS10{},k,value&15,false);
    }
    for(unsigned skew=0;skew<16;skew++)for(unsigned reg=0;reg<4;reg++)for(unsigned overflow:{0u,1u,2u,3u,4u})for(bool isS:{false,true})for(bool padded:{false,true})
        Case(isS,reg,GXColorS10{-32768,32767,-1024,1023},GXColor{0,255,128,1},skew,overflow,padded);
    const uint16_t representatives[]={0,1,2,255,256,1023,1024,2047,2048,4095,0x5555,0x7fff,0x8000,0xaaaa,0xfffe,0xffff};
    for(unsigned skew=0;skew<16;skew++)for(unsigned reg=0;reg<4;reg++)for(uint16_t value:representatives){
        int16_t bits,inverse;const uint16_t inv=~value;std::memcpy(&bits,&value,2);std::memcpy(&inverse,&inv,2);
        Case(true,reg,GXColorS10{bits,inverse,bits,inverse},GXColor{},skew,0,true);
        Case(false,reg,GXColorS10{},GXColor{uint8_t(value),uint8_t(value>>8),uint8_t(inv),uint8_t(inv>>8)},skew,0,true);
    }
    std::printf("PASS burst cases=%llu assertions=%llu\n",(unsigned long long)Cases,(unsigned long long)Checks);
}
#endif
