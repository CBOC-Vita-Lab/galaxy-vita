#include "port/settings_font.h"
#include "player_settings_font_fixture.h"
#include "settings_font_allocation_fixture.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>

static bool forbidNew = false;
void* operator new(std::size_t bytes) {
    if (forbidNew) { std::fputs("FAIL: optional font decoder reached fatal operator new\n", stderr); std::abort(); }
    if (void* result = std::malloc(bytes ? bytes : 1)) return result;
    std::abort();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* data) noexcept { std::free(data); }
void operator delete[](void* data) noexcept { std::free(data); }

using namespace port::settings;
using namespace settings_fixture;
static std::vector<uint8_t> WithFonts(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b,bool le=false) {
    auto result=Archive(le);result.resize(192+a.size()+b.size());
    std::copy(a.begin(),a.end(),result.begin()+192);std::copy(b.begin(),b.end(),result.begin()+192+a.size());
    Put(result,4,static_cast<uint32_t>(result.size()),4,le);Put(result,16,static_cast<uint32_t>(a.size()+b.size()),4,le);
    Put(result,92,static_cast<uint32_t>(a.size()),4,le);Put(result,108,static_cast<uint32_t>(a.size()),4,le);Put(result,112,static_cast<uint32_t>(b.size()),4,le);return result;
}
static std::vector<uint8_t> LargeFont() {
    auto small=Font();constexpr size_t sheet=2048*2048/2,cwdh=80+sheet,cmap=cwdh+28;
    std::vector<uint8_t> result(small.size()+sheet-128,255);
    std::copy(small.begin(),small.begin()+80,result.begin());std::copy(small.begin()+208,small.end(),result.begin()+cwdh);
    Put(result,8,static_cast<uint32_t>(result.size()),4,false);Put(result,36,cwdh+8,4,false);Put(result,40,cmap+8,4,false);
    Put(result,52,32+sheet,4,false);Put(result,60,sheet,4,false);Put(result,72,2048,2,false);Put(result,74,2048,2,false);return result;
}
int main() {
    unsigned denied=0;
    for(bool le:{false,true})for(unsigned format:{0u,1u,2u,3u}) {
        const auto font=Font(le,format), archive=WithFonts(font,font,le);
        const unsigned allocations=format>=2?8:6;
        for(unsigned failure=0;failure<=allocations;++failure) {
            FontAllocationProbe probe;probe.failAt=failure;
            const char* error=nullptr;
            forbidNew=true;
            {
                FontBundle fonts;
                const bool parsed=ParseGameFontArchive(archive.data(),archive.size(),fonts,error,probe.allocator());
                assert(parsed==(failure==0));
                if(failure){assert(!fonts.menu.valid()&&!fonts.message.valid()&&error);++denied;}
                else assert(fonts.menu.valid()&&fonts.message.valid()&&probe.calls==allocations);
                assert(probe.peak<=kFontDecodeLimit);
            }
            assert(!probe.live);
            forbidNew=false;
        }
    }
    const auto large=LargeFont();
    for(bool alias:{false,true}) {
        auto archive=WithFonts(large,large);
        if(alias){archive.resize(192+large.size());Put(archive,4,static_cast<uint32_t>(archive.size()),4,false);Put(archive,108,0,4,false);}
        FontAllocationProbe probe;const char* error=nullptr;FontBundle fonts;
        forbidNew=true;
        assert(!ParseGameFontArchive(archive.data(),archive.size(),fonts,error,probe.allocator()));
        assert(!fonts.menu.valid()&&!fonts.message.valid()&&!probe.live&&probe.peak<=kFontDecodeLimit);
        forbidNew=false;
    }
    auto duplicate=Archive();Put(duplicate,92,0,4,false);Put(duplicate,104,0x11000005,4,false);
    auto cycle=Font();constexpr unsigned cmap=48+32+128+28;Put(cycle,cmap+16,cmap+8,4,false);
    FontBundle fonts;const char* error=nullptr;
    forbidNew=true;
    assert(!ParseGameFontArchive(duplicate.data(),duplicate.size(),fonts,error));
    assert(error&&!std::strcmp(error,"Ambiguous font archive entry"));
    GameFont rejected;assert(!ParseGameFont(cycle.data(),cycle.size(),rejected,error));
    forbidNew=false;
    std::printf("PASS: no-exception font decoder, fatal new forbidden, %u allocation failures, no leaks, combined/aliased budget and empty duplicate rejection\n",denied);
}
