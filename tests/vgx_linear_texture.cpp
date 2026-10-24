#include "vgx_frozen_linear.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
std::vector<uint8_t> storage;
const uint8_t* source=nullptr;
size_t sourceSize=0,copied=0,freed=0;
unsigned width=0,height=0,mips=0;
SceGxmTextureFormat format;
void Require(bool ok,const char* why) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); }
}
}
extern "C" {
void* color_table=nullptr;
void* gpu_alloc_mapped_for_gpu(size_t n) { storage.assign(n+32,0xCD); return storage.data(); }
void gpu_free_texture_data(texture* t) { Require(t->data==storage.data(),"replace only the previous owned allocation"); ++freed; t->data=nullptr; }
void vgl_fast_memcpy(void* dst,const void* src,size_t n) {
    const auto a=reinterpret_cast<uintptr_t>(src),b=reinterpret_cast<uintptr_t>(source);
    Require(a>=b && a-b<=sourceSize && n<=sourceSize-(a-b),"frozen allocator reads only packed input bytes");
    auto* d=static_cast<uint8_t*>(dst);
    Require(d>=storage.data() && size_t(d-storage.data())<=storage.size()-32 &&
            n<=storage.size()-32-size_t(d-storage.data()),"frozen allocator writes only its aligned allocation");
    std::memcpy(dst,src,n); copied+=n;
}
void vgl_memset(void* dst,int c,size_t n) { std::memset(dst,c,n); }
void vglInitLinearTexture(SceGxmTexture*,const void* p,SceGxmTextureFormat f,unsigned w,unsigned h,unsigned levels) {
    Require(p==storage.data(),"native descriptor references the current owned data"); width=w; height=h; format=f; mips=levels;
}
}

int main() {
    static_assert(SCE_GXM_TEXTURE_FORMAT_A8L8==SCE_GXM_TEXTURE_FORMAT_U8U8_GRRR,"SDK legacy LA88 swizzle identity");
    const unsigned dimensions[][2]={{1,1},{3,5},{7,5},{8,8},{9,9},{16,8},{127,65},{256,128}};
    for (const auto& size:dimensions) {
        texture tex{}; size_t allocated[2]={},bytes[2]={};
        for (unsigned compact=0;compact<2;++compact) {
            const unsigned bpp=compact?2:4,w=size[0],h=size[1],stride=((w+7)&~7u)*bpp;
            std::vector<uint8_t> input(size_t(w)*h*bpp);
            for (size_t i=0;i<input.size();++i) input[i]=(i*71+13)&255;
            source=input.data(); sourceSize=input.size(); copied=0;
            const auto before=freed;
            gpu_alloc_texture(w,h,compact?SCE_GXM_TEXTURE_FORMAT_A8L8:SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
                              input.data(),&tex,bpp,nullptr,nullptr,1);
            Require(freed==before+compact,"format replacement retires the previous allocation once");
            Require(storage.size()==size_t(stride)*h+32 && copied==input.size(),"native LA88 keeps two-byte storage and copies packed rows without expansion");
            Require(width==w && height==h && mips==1 && tex.mip_count==1 && tex.status==TEX_VALID && tex.palette_data==nullptr,
                    "native descriptor and texture bookkeeping preserve dimensions and one base level");
            Require(format==(compact?SCE_GXM_TEXTURE_FORMAT_A8L8:SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR),"exact requested native format reaches descriptor");
            for (unsigned y=0;y<h;++y) {
                Require(!std::memcmp(storage.data()+size_t(y)*stride,input.data()+size_t(y)*w*bpp,size_t(w)*bpp),"each aligned native row preserves exact input bytes");
                for (unsigned x=w*bpp;x<stride;++x) Require(storage[size_t(y)*stride+x]==0xCD,"row alignment padding is not read from source");
            }
            Require(std::all_of(storage.end()-32,storage.end(),[](uint8_t x){return x==0xCD;}),"native allocator preserves output guard bytes");
            allocated[compact]=storage.size()-32; bytes[compact]=copied;
        }
        Require(allocated[0]==allocated[1]*2 && bytes[0]==bytes[1]*2,"eligible native allocation and upload copies are exactly halved");
        gpu_free_texture_data(&tex);
    }
    // The unused slow read callback has a pre-existing signed-shift UB for A>127.
    // The fast-store tests above exercise all byte values without that callback.
    const uint8_t la[]={71,97}; uint8_t roundtrip[2]={};
    Require(read_la88(la)==0x61474747u,"frozen LA callback reads L then independent A");
    write_ra88(roundtrip,0x61474747u);
    Require(roundtrip[0]==71 && roundtrip[1]==97,"frozen RA callback emits L then independent A");
    std::puts("PASS: exact frozen native linear allocator, real SDK format enums, odd/even rows, LA channel callbacks, 50% bytes (SDK/heap doubles; no native GPU timing)");
}
