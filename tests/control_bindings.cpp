#include "port/control_bindings.h"
#include <cassert>
#include <string>
using namespace port::controls;
int main() {
    const auto& api = GetControlApi();
    assert(api.actionCount == 11 && api.buttonCount == 16);
    std::string error, conflict;
    for (auto id : {ProfileId::Handheld, ProfileId::PlayStation}) {
        auto map = api.defaults(id);
        const auto caps = NominalCaps(id);
        assert(api.validate(map, id, caps, error));
        CompiledMapping compiled;
        assert(Compile(map, id, caps, compiled, error));
        assert(Translate(compiled, Cross).buttons == 0x800);
        assert(Translate(compiled, R1).spin);
        auto before = map;
        assert(api.bind(map, "jump", 0, "circle", false, caps, conflict) == BindResult::Conflict);
        assert(conflict == "star_bits" && map.bindings[0].buttons == before.bindings[0].buttons);
        assert(api.bind(map, "jump", 0, "circle", true, caps, conflict) == BindResult::Applied);
        assert(map.bindings[0].buttons[0] == "circle" && map.bindings[1].buttons[0] == "cross");
        assert(api.validate(map, id, caps, error));
        assert(api.bind(map, "jump", 0, "", false, caps, conflict) == BindResult::RequiredActionLost);
        assert(api.bind(map, "jump", 1, "r3", false, caps, conflict) == BindResult::Unavailable);
        map.bindings[0].buttons[0] = "ps";
        assert(!api.validate(map, id, caps, error));
        map = api.defaults(id); map.bindings.pop_back();
        assert(!api.validate(map, id, caps, error));
        map = api.defaults(id); map.bindings[1].buttons[0] = "cross";
        assert(!api.validate(map, id, caps, error));
        map = api.defaults(id); map.bindings[0].buttons[0] = "l2";
        assert(api.validate(map, id, caps, error) == (id == ProfileId::PlayStation));
    }
}
