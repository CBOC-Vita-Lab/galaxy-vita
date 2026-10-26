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
    settings::Document d;
    settings::Document original{c,"",true};auto full=c;full.fpsLimit=30;std::string serialized;
    assert(settings::SerializeDocument(original,full,api,serialized).status==settings::StoreStatus::Ok);
    assert(settings::ParseDocument(serialized, api, d).status == settings::StoreStatus::Ok);
    assert(d.config.fpsLimit == 30);
    {
        auto steady=serialized;steady.replace(steady.find("fps_limit=30"),12,"fps_limit=steady30");settings::Document s2;
        assert(settings::ParseDocument(steady,api,s2).status==settings::StoreStatus::Ok&&s2.config.fpsLimit==settings::kSteady30);
    }
    auto invalid=serialized;invalid.replace(invalid.find("fps_limit=30"),12,"fps_limit=60");
    assert(settings::ParseDocument(invalid, api, d).status == settings::StoreStatus::Malformed);
    auto copy=c; assert(settings::Equal(c,copy)); copy.language="French"; assert(!settings::Equal(c,copy));
    std::cout << "config: defaults, required actions, spin slots, fixed menu separation passed\n";
}
