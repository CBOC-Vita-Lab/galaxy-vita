#include "port/settings_font.h"
#include "player_settings_font_fixture.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
using namespace port::settings;
using namespace settings_fixture;
int main(int argc,char** argv) {
    if(argc>1) {std::cerr<<"font fixture takes no arguments: "<<argv[0]<<"\n";return 2;}
    const char* error=nullptr;GameFont parsed;
    for(bool le:{false,true})for(unsigned format:{0u,1u,2u,3u})for(unsigned method:{0u,1u,2u}) {
        auto bytes=Font(le,format,method);assert(ParseGameFont(bytes.data(),bytes.size(),parsed,error));assert(parsed.valid()&&parsed.cellHeight==3);
        assert(parsed.find('A',false)&&parsed.find('A')->left==-1&&parsed.find('A')->advance==3);
        assert(parsed.find(0x1f680)==parsed.find('?',true));
        assert(parsed.sample(*parsed.find('A'),0,0)==(format==2?170:255));
        if(method==2)assert(parsed.find(0xe9,false)&&parsed.find(0x4e00,false));
        for(size_t cut=0;cut<bytes.size();++cut) {GameFont invalid;assert(!ParseGameFont(bytes.data(),cut,invalid,error)&&!invalid.valid());}
    }
    const auto font=Font();
    for(size_t field:{size_t(8),size_t(20),size_t(32),size_t(36),size_t(40),size_t(60),size_t(76)}) {
        auto bad=font;Put(bad,field,0xffffffff,4,false);assert(!ParseGameFont(bad.data(),bad.size(),parsed,error));
    }
    auto cycle=font;const unsigned cwdh=48+32+128,cmap=cwdh+28;Put(cycle,cmap+16,cmap+8,4,false);assert(!ParseGameFont(cycle.data(),cycle.size(),parsed,error));
    auto badWidth=font;badWidth[cwdh+17]=255;assert(!ParseGameFont(badWidth.data(),badWidth.size(),parsed,error));
    for(bool le:{false,true}) {
        auto archive=Archive(le);FontBundle bundle;assert(ParseGameFontArchive(archive.data(),archive.size(),bundle,error));assert(bundle.view().menu&&bundle.view().message);
        for(size_t cut=0;cut<archive.size();++cut)assert(!ParseGameFontArchive(archive.data(),cut,bundle,error));
        Put(archive,104,0x11000005,4,le);assert(!ParseGameFontArchive(archive.data(),archive.size(),bundle,error));
    }
    std::string text="A\xc3\xa9\xe4\xb8\x80\xf0\x9f\x9a\x80";size_t at=0;
    assert(NextFontCodepoint(text,at)=='A');assert(NextFontCodepoint(text,at)==0xe9);assert(NextFontCodepoint(text,at)==0x4e00);assert(NextFontCodepoint(text,at)==0x1f680&&at==text.size());
    for(const std::string invalid:{"\xc0\xaf","\xed\xa0\x80","\xf4\x90\x80\x80","\xe2"}) {at=0;assert(NextFontCodepoint(invalid,at)==0xfffd&&at>0);}
    std::cout<<"font: original/cooked RFNT+RARC, glyphs/widths, I4/I8/IA4/IA8, UTF-8 and malformed bounds passed\n";
}
