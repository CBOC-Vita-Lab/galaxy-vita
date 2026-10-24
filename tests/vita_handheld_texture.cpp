// Exercise the real patch matcher and pixels; generated artwork remains local.
#include "../platform/src/port/handheld_ui.cpp"
#include <array>
#include <cstdio>

extern "C" int port_handheld() { return 1; }

namespace {
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); }
}
void Check(uint64_t hash, unsigned w, unsigned h, bool match, bool enabled) {
    std::vector<uint8_t> pixels(size_t(w)*h*4,0xFD);
    const auto original=pixels;
    Require(port_handheld_texture_needs_rgba(hash,w,h)==(match && enabled),"eligibility preserves exact size/hash/enable boundaries");
    port_handheld_texture(hash,w,h,pixels.data());
    Require((pixels!=original)==(match && enabled),"only eligible replacements alter actual RGBA pixels");
}
}

int main(int argc,char** argv) {
    Require(argc==2,"expected enabled or disabled scenario");
    const bool enabled=std::strcmp(argv[1],"on")==0;
    setenv("PETARI_HANDHELD_UI",enabled?"1":"0",1);
    for (const auto& texture:kTextures) {
        Check(texture.hash,texture.size,texture.size,true,enabled);
        Check(texture.hash,texture.size+1,texture.size,false,enabled);
        std::vector<uint8_t> pixels(size_t(texture.size)*texture.size*4,0xFD);
        port_handheld_texture(texture.hash,texture.size,texture.size,pixels.data());
        if (enabled) Require(!std::memcmp(pixels.data(),texture.picture,pixels.size()),"whole texture replacement retains every authored color byte");
    }
    for (const auto& sheet:kSheets) {
        Check(sheet.hash,128,128,true,enabled);
        Check(sheet.hash,127,128,false,enabled);
        Check(sheet.hash,128,127,false,enabled);
    }
    const uint64_t animations[]={0x5c8ee03db0a44a08ull,0x8deeaa9e63cd26ffull,0x2e99253f110c8a84ull,
        0x86b80905932085d6ull,0x55a7b9fe04cfb1e0ull,0x24e5ac61034c6daeull,0x7c891ac94b93a354ull,
        0x720a2c4a8d40f663ull,0x11cb720a4576ec9bull,0x6f81bcf9cef94309ull,0x1f426fe7e5032b9dull,
        0x93eb50681f48acccull,0x4b5a247d08cc7680ull};
    for (const auto hash:animations) {
        Check(hash,128,128,true,enabled);
        Check(hash,128,64,false,enabled);
    }
    Check(0,128,128,false,enabled); Check(UINT64_MAX,48,48,false,enabled);
    std::printf("PASS: whole textures, glyph sheets, 13 animations, size boundaries and enable flag (%s)\n",argv[1]);
}
