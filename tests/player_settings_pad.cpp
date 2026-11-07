// Pad defaults (GAL-032) and the mouse-style aiming option (GAL-031) in the controls catalog and the settings store.
// Run with GXV_PAD_DEFAULTS unset (1.1 layout) and with GXV_PAD_DEFAULTS=1; tools/test_player_settings.sh pad does both.
#include "port/player_settings.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
using namespace port;
using controls::ProfileId;

static std::string without(std::string text, const char* key) {  // a file written before the key existed
    const auto at = text.find(std::string(key) + "=");
    assert(at != std::string::npos);
    text.erase(at, text.find('\n', at) + 1 - at);
    return text;
}
static std::string button(const controls::ProfileMapping& m, const char* action, unsigned slot = 0) {
    for (const auto& b : m.bindings) if (b.action == action) return b.buttons[slot];
    assert(false); return "";
}
int main() {
    const char* e = std::getenv("GXV_PAD_DEFAULTS");
    const bool on = e && !std::strcmp(e, "1");
    const auto& api = controls::GetControlApi();
    assert(controls::PadDefaultsEnabled() == on);
    const auto handheld = api.defaults(ProfileId::Handheld), ps = api.defaults(ProfileId::PlayStation);
    const auto legacy = controls::LegacyPlayStationDefaults();
    std::string error;

    // The Handheld defaults are the 1.0 ones whatever the switch says; the PlayStation defaults change only Star Bits.
    {
        const char* expectHandheld[] = {"cross", "circle", "square", "l1", "triangle", "start", "select", "up", "down", "left", "right"};
        assert(handheld.bindings.size() == 11 && ps.bindings.size() == 11 && legacy.bindings.size() == 11);
        for (size_t i = 0; i < 11; ++i) {
            assert(handheld.bindings[i].buttons[0] == expectHandheld[i] && legacy.bindings[i].buttons[0] == expectHandheld[i]);
            assert(handheld.bindings[i].buttons[1] == (i == 2 ? "r1" : "") && legacy.bindings[i].buttons[1] == handheld.bindings[i].buttons[1]);
            if (i == 1) continue;
            assert(ps.bindings[i].buttons == handheld.bindings[i].buttons);
        }
        assert(button(ps, "star_bits") == (on ? "r2" : "circle"));
        assert(controls::SameBindings(ps, legacy) == !on && controls::SameBindings(handheld, legacy));
        assert(api.validate(ps, ProfileId::PlayStation, controls::NominalCaps(ProfileId::PlayStation), error));
        // R2 does not exist on the handheld: the Handheld profile cannot hold it, so the two layouts cannot cross.
        auto h = handheld;
        assert(api.bind(h, "star_bits", 0, "r2", false, controls::NominalCaps(ProfileId::Handheld), error) == controls::BindResult::Unavailable);
        // The remap list on a pad offers R2 and the other buttons that only an external pad has.
        const auto caps = controls::NominalCaps(ProfileId::PlayStation);
        for (const char* id : {"l2", "r2", "l3", "r3"}) {
            bool listed = false;
            for (size_t i = 0; i < api.buttonCount; ++i) listed |= !std::strcmp(api.buttons[i].id, id) && (api.buttons[i].mask & caps.availableButtons);
            assert(listed);
        }
        // Compiled: R2 shoots, Circle does nothing on the pad defaults; the D-pad is the camera on both layouts.
        controls::CompiledMapping compiled;
        assert(controls::Compile(ps, ProfileId::PlayStation, caps, compiled, error));
        assert(controls::Translate(compiled, controls::R2).buttons == (on ? 0x400u : 0u));
        assert(controls::Translate(compiled, controls::Circle).buttons == (on ? 0u : 0x400u));
        assert(controls::Translate(compiled, controls::Left).buttons == 1 && controls::Translate(compiled, controls::Right).buttons == 2);
        assert(controls::Translate(compiled, controls::Down).buttons == 4 && controls::Translate(compiled, controls::Up).buttons == 8);
        assert(std::string(controls::PrimaryButton(compiled, controls::Action::StarBits)->id) == (on ? "r2" : "circle"));
    }

    // A file written by 1.1: no mouse_aim and no profile_chosen. Its PlayStation profile is the old default untouched.
    const auto base = settings::DefaultConfig(api);
    settings::Document blank{base, "", true};
    auto oldConfig = base; oldConfig.profiles[1] = legacy; oldConfig.profile = ProfileId::PlayStation; oldConfig.fpsLimit = 30;
    std::string full;
    assert(settings::SerializeDocument(blank, oldConfig, api, full).status == settings::StoreStatus::Ok);
    const std::string v11 = without(without(full, "mouse_aim"), "profile_chosen");
    {
        settings::Document d;
        assert(settings::ParseDocument(v11, api, d).status == settings::StoreStatus::Ok);
        assert(!d.config.mouseAim && !d.config.profileChosen && d.config.fpsLimit == 30 && d.config.profile == ProfileId::PlayStation);
        assert(button(d.config.profiles[1], "star_bits") == (on ? "r2" : "circle"));   // migrated to the pad default only with the switch
        assert(controls::SameBindings(d.config.profiles[0], handheld));
        // A 1.1 file with a customised PlayStation profile keeps it.
        auto custom = oldConfig; for (auto& b : custom.profiles[1].bindings) if (b.action == "crouch") b.buttons[0] = "l2";
        std::string text;
        assert(settings::SerializeDocument(blank, custom, api, text).status == settings::StoreStatus::Ok);
        text = without(without(text, "mouse_aim"), "profile_chosen");
        assert(settings::ParseDocument(text, api, d).status == settings::StoreStatus::Ok);
        assert(button(d.config.profiles[1], "crouch") == "l2" && button(d.config.profiles[1], "star_bits") == "circle");
        // Saving over the 1.1 file adds the two keys at the end, keeps the player's other lines, and then nothing is migrated again.
        settings::Document d11; assert(settings::ParseDocument("# mine\n" + v11, api, d11).status == settings::StoreStatus::Ok);
        auto edited = d11.config; edited.mouseAim = true;
        std::string saved;
        assert(settings::SerializeDocument(d11, edited, api, saved).status == settings::StoreStatus::Ok);
        assert(saved.compare(0, 7, "# mine\n") == 0 && saved.find("mouse_aim=1\n") != std::string::npos && saved.find("profile_chosen=0\n") != std::string::npos);
        settings::Document back; assert(settings::ParseDocument(saved, api, back).status == settings::StoreStatus::Ok);
        assert(back.config.mouseAim && settings::Equal(back.config, edited));
    }
    // A 1.2 file keeps what it says: Star Bits set back to Circle (the 1.1 default) by hand is not migrated.
    {
        auto choice = base; choice.profiles[1] = legacy; choice.profileChosen = true;
        std::string text; settings::Document d;
        assert(settings::SerializeDocument(blank, choice, api, text).status == settings::StoreStatus::Ok);
        assert(text.find("bind.playstation.star_bits=circle\n") != std::string::npos);
        assert(settings::ParseDocument(text, api, d).status == settings::StoreStatus::Ok);
        assert(button(d.config.profiles[1], "star_bits") == "circle" && d.config.profileChosen && settings::Equal(d.config, choice));
    }
    // The two new keys: stored, validated, optional, never duplicated.
    {
        auto c = base; c.mouseAim = true; c.profileChosen = true;
        std::string text; settings::Document d;
        assert(settings::SerializeDocument(blank, c, api, text).status == settings::StoreStatus::Ok);
        assert(text.find("mouse_aim=1\n") != std::string::npos && text.find("profile_chosen=1\n") != std::string::npos);
        assert(settings::ParseDocument(text, api, d).status == settings::StoreStatus::Ok && d.config.mouseAim && d.config.profileChosen && settings::Equal(d.config, c));
        auto bad = text; bad.replace(bad.find("mouse_aim=1"), 11, "mouse_aim=2");
        assert(settings::ParseDocument(bad, api, d).status == settings::StoreStatus::Malformed);
        bad = text; bad.replace(bad.find("profile_chosen=1"), 16, "profile_chosen=x");
        assert(settings::ParseDocument(bad, api, d).status == settings::StoreStatus::Malformed);
        assert(settings::ParseDocument(text + "mouse_aim=0\n", api, d).status == settings::StoreStatus::Malformed);
        // The required keys are still required: a file cut short is still caught.
        assert(settings::ParseDocument(without(text, "reduced_motion"), api, d).status == settings::StoreStatus::Malformed);
        assert(settings::ParseDocument(text.substr(0, text.size() / 2), api, d).status == settings::StoreStatus::Malformed);
        assert(!settings::Equal(c, base));
        auto m = base; m.mouseAim = true; assert(!settings::Equal(m, base));
        auto p = base; p.profileChosen = true; assert(!settings::Equal(p, base));
    }
    // Defaults: option off, profile not chosen.
    assert(!base.mouseAim && !base.profileChosen);
    // ApplyPadDefaults: a pad, the profile never chosen and the Handheld buttons untouched -> the PlayStation profile.
    {
        auto c = base;
        assert(settings::ApplyPadDefaults(c, true) == on && c.profile == (on ? ProfileId::PlayStation : ProfileId::Handheld));
        c = base; assert(!settings::ApplyPadDefaults(c, false) && c.profile == ProfileId::Handheld);       // the handheld's own controls
        c = base; c.profileChosen = true; assert(!settings::ApplyPadDefaults(c, true) && c.profile == ProfileId::Handheld);  // chosen by hand
        c = base; for (auto& b : c.profiles[0].bindings) if (b.action == "jump") b.buttons[0] = "square", c.profiles[0].bindings[2].buttons[0] = "cross";
        assert(!settings::ApplyPadDefaults(c, true) && c.profile == ProfileId::Handheld);                  // a customised Handheld profile stays
        c = base; c.profile = ProfileId::PlayStation; assert(!settings::ApplyPadDefaults(c, true));
        // The Settings menu marks the profile row as chosen (tests/player_settings_ui.cpp); a saved choice survives a restart.
        c = base; c.profileChosen = true; std::string text; settings::Document d;
        assert(settings::SerializeDocument(blank, c, api, text).status == settings::StoreStatus::Ok && settings::ParseDocument(text, api, d).status == settings::StoreStatus::Ok);
        assert(!settings::ApplyPadDefaults(d.config, true));
    }
    std::cout << "pad: defaults (" << (on ? "pad layout" : "1.1 layout") << "), 1.1 file migration, optional keys and profile choice passed\n";
}
