#include "port/player_settings.h"
#include <cassert>
#include <iostream>
using namespace port;
int main() {
    const auto& api = controls::GetControlApi();
    const auto c = settings::DefaultConfig(api);
    assert(c.version == 1 && c.fpsLimit == 0 && c.language.empty());
    assert(c.autoQuality == settings::AutoQuality::Default && !c.reducedMotion);
    assert(c.profile == controls::ProfileId::Handheld);
    for (unsigned i=0; i<2; ++i) {
        std::string error;
        assert(api.validate(c.profiles[i], static_cast<controls::ProfileId>(i), controls::NominalCaps(static_cast<controls::ProfileId>(i)), error));
        assert(c.profiles[i].bindings.size() == api.actionCount);
        for (const auto& b : c.profiles[i].bindings) {
            assert(!b.buttons[0].empty());
            assert(b.action == "spin" || b.buttons[1].empty());
            assert(b.action != "menu_accept" && b.action != "menu_back");
        }
    }
    // Display page choices: the defaults are today's best picture; the preset is read from the covered rows, never stored.
    assert(c.aboveNative&&c.prioritizeMario&&c.antiAliasing==settings::AntiAliasing::Auto&&c.sharpness==settings::Sharpness::Auto&&c.hdTextures&&
           c.effects==settings::EffectsQuality::Adaptive&&c.depthOfField&&c.coreHelp&&c.tiltLevel==5&&!c.tiltStick);   // tilt: the game's own steering, from the sensor
    assert(settings::PresetOf(c)==settings::Preset::BestPicture);
    {
        const settings::Preset presets[]={settings::Preset::BestPicture,settings::Preset::Balanced,settings::Preset::Performance};
        for(const auto p:presets) {auto x=c;settings::ApplyPreset(x,p);assert(settings::PresetOf(x)==p);}
        auto balanced=c;settings::ApplyPreset(balanced,settings::Preset::Balanced);assert(!balanced.aboveNative&&balanced.antiAliasing==settings::AntiAliasing::Auto&&balanced.sharpness==settings::Sharpness::Auto&&balanced.depthOfField);
        auto perf=c;settings::ApplyPreset(perf,settings::Preset::Performance);assert(!perf.aboveNative&&perf.antiAliasing==settings::AntiAliasing::Off&&perf.sharpness==settings::Sharpness::Off&&!perf.depthOfField&&perf.prioritizeMario&&perf.effects==settings::EffectsQuality::Adaptive);
        // A preset touches only its covered rows.
        auto mixed=c;mixed.hdTextures=false;mixed.coreHelp=false;mixed.fpsLimit=30;mixed.reducedMotion=true;mixed.language="French";settings::ApplyPreset(mixed,settings::Preset::Performance);
        assert(!mixed.hdTextures&&!mixed.coreHelp&&mixed.fpsLimit==30&&mixed.reducedMotion&&mixed.language=="French"&&settings::PresetOf(mixed)==settings::Preset::Performance);
        // Any covered row edited reads Custom; "dynamic resolution on" (forced) and the default are the same choice.
        auto edited=c;edited.antiAliasing=settings::AntiAliasing::Off;assert(settings::PresetOf(edited)==settings::Preset::Custom);
        edited=c;edited.effects=settings::EffectsQuality::Full;assert(settings::PresetOf(edited)==settings::Preset::Custom);
        edited=c;edited.prioritizeMario=false;assert(settings::PresetOf(edited)==settings::Preset::Custom);
        edited=c;edited.autoQuality=settings::AutoQuality::Disabled;assert(settings::PresetOf(edited)==settings::Preset::Custom);
        edited=c;edited.autoQuality=settings::AutoQuality::Enabled;assert(settings::PresetOf(edited)==settings::Preset::BestPicture);
        edited=c;edited.hdTextures=false;edited.coreHelp=false;assert(settings::PresetOf(edited)==settings::Preset::BestPicture);   // not covered
        settings::ApplyPreset(edited,settings::Preset::Custom);assert(!edited.hdTextures);                                           // Custom changes nothing
    }
    settings::Document d;
    settings::Document original{c,"",true};auto full=c;full.fpsLimit=30;std::string serialized;
    assert(settings::SerializeDocument(original,full,api,serialized).status==settings::StoreStatus::Ok);
    assert(settings::ParseDocument(serialized, api, d).status == settings::StoreStatus::Ok);
    assert(d.config.fpsLimit == 30);
    {
        auto steady=serialized;steady.replace(steady.find("fps_limit=30"),12,"fps_limit=steady30");settings::Document s2;
        assert(settings::ParseDocument(steady,api,s2).status==settings::StoreStatus::Ok&&s2.config.fpsLimit==30); // 1.1's Steady 30 reads as the 1.2 locked 30
    }
    auto invalid=serialized;invalid.replace(invalid.find("fps_limit=30"),12,"fps_limit=60");
    assert(settings::ParseDocument(invalid, api, d).status == settings::StoreStatus::Malformed);
    auto copy=c; assert(settings::Equal(c,copy)); copy.language="French"; assert(!settings::Equal(c,copy));
    std::cout << "config: defaults, required actions, spin slots, fixed menu separation passed\n";
}
