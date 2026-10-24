#include "gx_copy_pixels.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc,char** argv) {
    const bool filtered=argc==2 && !std::strcmp(argv[1],"filtered");
    if (argc>2 || (argc==2 && !filtered)) return 2;
    constexpr unsigned width=640,height=456,iterations=20;
    std::vector<unsigned char> source(width*height*4);
    for (size_t i=0;i<source.size();i+=4) {
        source[i]=0xAB; source[i+1]=0xCD; source[i+2]=0xEF; source[i+3]=0x97;
    }
    gpu::SpanCopy copy{};
    copy.efbWidth=width; copy.efbHeight=height; copy.pixelFormat=1;
    copy.words[2]=(width-1)|((height-1)<<10); copy.words[4]=width/16;
    copy.words[5]=256; copy.words[9]=(1u<<14)|3;
    copy.filter[0]=(21u<<12)|(22u<<18); copy.filter[1]=21;
    if (filtered) {
        copy.filter[0]=8u|(8u<<6)|(10u<<12)|(12u<<18);
        copy.filter[1]=10u|(8u<<6)|(8u<<12);
    }
    const gpu::GxCopyPlane plane{source.data(),0,0,width,height,width*4,source.size(),false};
    const unsigned char expected[]={0xAA,0xCF,0xEF,0x96};
    std::string error;
    const auto begin=std::chrono::steady_clock::now();
    for (unsigned iteration=0;iteration<iterations;++iteration) {
        gpu::GxCopyPixels pixels;
        if (gpu::produceGxCopyPixels(copy,plane,pixels,error)<0) {
            std::fprintf(stderr,"copy benchmark failed: %s\n",error.c_str()); return 1;
        }
        const auto texels=pixels.texels();
        if (texels.width!=width || texels.height!=height || texels.bytes!=source.size()) return 1;
        for (size_t i=0;i<texels.bytes;i+=4)
            if (std::memcmp(texels.data+i,expected,4)) return 1;
    }
    const auto elapsed=std::chrono::steady_clock::now()-begin;
    const double milliseconds=std::chrono::duration<double,std::milli>(elapsed).count()/iterations;
    std::printf("Actual 640x456 RGBA6 %s checked-copy round trip: %.3f ms/copy (%u copies, includes production, every-byte validation and destruction); host only, not Vita FPS\n",
        filtered ? "16/32/16-filtered" : "identity",milliseconds,iterations);
    if (milliseconds>1.5) {
        std::fprintf(stderr,"Pinned host copy budget exceeded: 1.5 ms/copy; not a physical Vita budget\n");
        return 1;
    }
}
