#include "vgx_texdecode.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void Require(bool ok,const char* why) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); }
}
}
int main() {
    const unsigned dimensions[][2]={{1,1},{3,5},{4,4},{7,8},{8,4},{8,8},{9,9},{16,8},{17,7},{128,128},{255,128},{256,128}};
    unsigned cases=0;
    for (unsigned fmt=0;fmt<4;++fmt) for (const auto& size:dimensions) for (unsigned seed=1;seed<=8;++seed) {
        const unsigned w=size[0],h=size[1];
        std::vector<uint8_t> source(vgx::TexLevelSize(w,h,fmt)+1),rgba(size_t(w)*h*4),compact(size_t(w)*h*2+62,0xCD);
        uint32_t random=seed*0x135B9A7u;
        for (auto& x:source) { random^=random<<13; random^=random>>17; random^=random<<5; x=random; }
        // Unaligned source/output exercise both scalar edge tiles and complete NEON tiles.
        vgx::DecodeTexture(source.data()+1,w,h,fmt,nullptr,0,0,rgba.data());
        Require(vgx::DecodeTextureLA88(source.data()+1,w,h,fmt,compact.data()+31),"eligible compact decode succeeds");
        for (size_t i=0;i<size_t(w)*h;++i) {
            const auto* la=compact.data()+31+i*2; const auto* reference=rgba.data()+i*4;
            Require(la[0]==reference[0] && la[0]==reference[1] && la[0]==reference[2] && la[1]==reference[3],
                    "scalar/NEON compact decode expands to exact RGBA for randomized tiles");
        }
        for (unsigned i=0;i<31;++i) Require(compact[i]==0xCD && compact[compact.size()-1-i]==0xCD,
                                           "tile edge/NEON writes stay within the requested output");
        ++cases;
    }
    std::array<uint8_t,64> guard; guard.fill(0xAF); const auto original=guard;
    Require(!vgx::DecodeTextureLA88(nullptr,1,1,0,guard.data()) &&
            !vgx::DecodeTextureLA88(guard.data(),1,1,4,guard.data()) &&
            !vgx::DecodeTextureLA88(guard.data(),0,1,0,guard.data()) &&
            !vgx::DecodeTextureLA88(guard.data(),1,0,0,guard.data()) &&
            !vgx::DecodeTextureLA88(guard.data(),1,1,0,nullptr) && guard==original,
            "invalid requests fail without touching destination bytes");
#if defined(__ARM_NEON)
    constexpr const char* path="NEON and scalar";
#else
    constexpr const char* path="scalar";
#endif
    std::printf("PASS: %u randomized exact compact decode cases, guards and invalid inputs (%s; not Vita timing)\n",cases,path);
}
