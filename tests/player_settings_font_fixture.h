#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
namespace settings_fixture {
inline void Put(std::vector<uint8_t>& b,size_t at,uint32_t value,unsigned bytes,bool le) {for(unsigned i=0;i<bytes;++i)b.at(at+i)=static_cast<uint8_t>(value>>(8*(le?i:bytes-i-1)));}
inline void Tag(std::vector<uint8_t>& b,size_t at,const char* tag,bool le) {for(unsigned i=0;i<4;++i)b.at(at+i)=static_cast<uint8_t>(tag[le?3-i:i]);}
inline std::vector<uint8_t> Font(bool le=false,unsigned format=0,unsigned method=2) {
    const unsigned sheetBytes=format==0?128:format==3?512:256;
    const unsigned tglp=48,cwdh=tglp+32+sheetBytes,cmap=cwdh+28;
    const unsigned mapBytes=method==0?2:method==1?8:18;
    std::vector<uint8_t> b(cmap+20+mapBytes);
    Tag(b,0,"RFNT",le);Put(b,4,0xfeff,2,le);Put(b,6,0x104,2,le);Put(b,8,static_cast<uint32_t>(b.size()),4,le);Put(b,12,16,2,le);Put(b,14,4,2,le);
    Tag(b,16,"FINF",le);Put(b,20,32,4,le);b[24]=1;b[25]=4;Put(b,26,1,2,le);b[29]=3;b[30]=4;b[31]=1;
    Put(b,32,tglp+8,4,le);Put(b,36,cwdh+8,4,le);Put(b,40,cmap+8,4,le);b[44]=4;b[45]=4;b[46]=3;
    Tag(b,tglp,"TGLP",le);Put(b,tglp+4,32+sheetBytes,4,le);auto d=tglp+8;b[d]=3;b[d+1]=3;b[d+2]=3;b[d+3]=4;
    Put(b,d+4,sheetBytes,4,le);Put(b,d+8,1,2,le);Put(b,d+10,format,2,le);Put(b,d+12,4,2,le);Put(b,d+14,4,2,le);Put(b,d+16,16,2,le);Put(b,d+18,16,2,le);Put(b,d+20,tglp+32,4,le);
    std::fill(b.begin()+tglp+32,b.begin()+cwdh,format==2?0xa5:0xff);
    Tag(b,cwdh,"CWDH",le);Put(b,cwdh+4,28,4,le);Put(b,cwdh+8,0,2,le);Put(b,cwdh+10,3,2,le);
    for(unsigned i=0;i<4;++i){b[cwdh+16+i*3]=i==0?255:0;b[cwdh+17+i*3]=3;b[cwdh+18+i*3]=static_cast<uint8_t>(i+3);}
    Tag(b,cmap,"CMAP",le);Put(b,cmap+4,20+mapBytes,4,le);Put(b,cmap+8,method==2?0x3f:0x41,2,le);Put(b,cmap+10,method==2?0x4e00:0x44,2,le);Put(b,cmap+12,method,2,le);
    if(method==0)Put(b,cmap+20,0,2,le);
    else if(method==1)for(unsigned i=0;i<4;++i)Put(b,cmap+20+i*2,i==2?0xffff:i,2,le);
    else {Put(b,cmap+20,4,2,le);const unsigned cp[]={0x3f,0x41,0xe9,0x4e00};const unsigned glyph[]={1,0,2,3};for(unsigned i=0;i<4;++i){Put(b,cmap+22+i*4,cp[i],2,le);Put(b,cmap+24+i*4,glyph[i],2,le);}}
    return b;
}
inline std::vector<uint8_t> Archive(bool le=false) {
    const auto font=Font(le);constexpr char rawNames[]="font\0MenuFont64.brfnt\0MessageFont26.brfnt\0";const std::string names(rawNames,sizeof(rawNames)-1);
    const unsigned data=192;
    std::vector<uint8_t> b(data+font.size()*2);Tag(b,0,"RARC",le);Put(b,4,static_cast<uint32_t>(b.size()),4,le);Put(b,8,32,4,le);Put(b,12,data-32,4,le);Put(b,16,static_cast<uint32_t>(font.size()*2),4,le);
    Put(b,32,1,4,le);Put(b,36,32,4,le);Put(b,40,2,4,le);Put(b,44,48,4,le);Put(b,48,static_cast<uint32_t>(names.size()),4,le);Put(b,52,88,4,le);
    Put(b,68,0,4,le);Put(b,74,2,2,le);Put(b,76,0,4,le);std::memcpy(b.data()+120,names.data(),names.size());
    for(unsigned i=0;i<2;++i) {unsigned f=80+i*20;Put(b,f+4,0x11000000u+(i?22:5),4,le);Put(b,f+8,static_cast<uint32_t>(i*font.size()),4,le);Put(b,f+12,static_cast<uint32_t>(font.size()),4,le);std::memcpy(b.data()+data+i*font.size(),font.data(),font.size());}
    return b;
}
}
