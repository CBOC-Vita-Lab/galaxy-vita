#include "../platform/src/port/handheld_ui.cpp"
#include "port/control_bindings.h"
#include <cassert>
#include <cstdio>
extern "C" int port_handheld() { return 1; }
extern "C" void port_host_alloc_scope_enter() {}
extern "C" void port_host_alloc_scope_leave() {}
extern "C" void port_log(const char*, ...) {}
using namespace port::controls;
static std::u16string message(const std::u16string& source) {
    return static_cast<const char16_t*>(port_handheld_message(source.c_str()));
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const bool remap = std::strcmp(argv[1], "on") == 0;
    setenv("GXV_REMAP", remap ? "1" : "0", 1);
    setenv("PETARI_HANDHELD_UI", "1", 1);
    auto map = GetControlApi().defaults(ProfileId::PlayStation);
    std::string error, conflict;
    assert(GetControlApi().bind(map, "jump", 0, "l2", false, NominalCaps(ProfileId::PlayStation), conflict) == BindResult::Applied);
    assert(GetControlApi().bind(map, "pause", 0, "triangle", true, NominalCaps(ProfileId::PlayStation), conflict) == BindResult::Applied);
    assert(GetControlApi().bind(map, "spin", 0, "r3", false, NominalCaps(ProfileId::PlayStation), conflict) == BindResult::Applied);
    assert(SetActiveMapping(map, ProfileId::PlayStation, NominalCaps(ProfileId::PlayStation), error));
    FreezeActiveCapabilities(NominalCaps(ProfileId::PlayStation));
    // Working-copy edits cannot alter runtime glyphs or cached text.
    map.bindings[0].buttons[0] = "cross";
    std::vector<uint8_t> pixels(128*128*4);
    port_handheld_texture(kSheets[0].hash,128,128,pixels.data());
    const auto* expected = remap ? kGlyphL2 : kGlyphCross;
    for (int y = 0; y < kGlyphH; ++y)
        assert(!std::memcmp(pixels.data()+((1+y)*128+36)*4, expected+y*kGlyphW*4,kGlyphW*4));
    for (const auto& texture : kTextures) {
        std::vector<uint8_t> p(texture.size*texture.size*4);
        port_handheld_texture(texture.hash,texture.size,texture.size,p.data());
        assert((std::memcmp(p.data(),texture.picture,p.size()) != 0) == remap);
    }
    const std::u16string a{0x1a,0x306,0}, b{0x1a,0x306,1}, remote{0x1a,0x306,3};
    const auto title = message(u"Press both " + a + u" and " + b + u".");
    assert(title.find(remap ? u"Triangle" : u"START or touch the screen") != std::u16string::npos);
    const auto point = message(u"Point your " + remote + u" at the screen.");
    assert(point.find(remap ? u"right stick" : u"Touch the screen") != std::u16string::npos);
    const auto tilt = message(u"Tilt your " + remote + u".");
    if (remap) assert(tilt.find(u"left stick") != std::u16string::npos && tilt.find(u"Tilt") == std::u16string::npos);
    const auto hold = message(u"Hold your " + remote + u" upright.");
    if (remap) assert(hold == u"Keep the left stick centred.");
    // Non-English words stay in their regional source; only their pictures change.
    const auto regional = u"Inclinez " + remote;
    assert(message(regional) == regional);
    std::puts("PASS: immutable active prompts, standalone textures and capability wording");
}
