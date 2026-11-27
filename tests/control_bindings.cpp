#include "port/control_bindings.h"
#include <atomic>
#include <cassert>
#include <string>
#include <thread>
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
        // Star Bits is on Circle, except on the PlayStation profile with the pad defaults (GXV_PAD_DEFAULTS=1): R2, Circle unassigned.
        const bool padLayout = PadDefaultsEnabled() && id == ProfileId::PlayStation;
        const std::string star = padLayout ? "r2" : "circle";
        assert(map.bindings[1].action == "star_bits" && map.bindings[1].buttons[0] == star);
        if (padLayout) assert(api.bind(map, "jump", 0, "circle", false, caps, conflict) == BindResult::Applied && (map = before, true));
        assert(api.bind(map, "jump", 0, star.c_str(), false, caps, conflict) == BindResult::Conflict);
        assert(conflict == "star_bits" && map.bindings[0].buttons == before.bindings[0].buttons);
        assert(api.bind(map, "jump", 0, star.c_str(), true, caps, conflict) == BindResult::Applied);
        assert(map.bindings[0].buttons[0] == star && map.bindings[1].buttons[0] == "cross");
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
        // The Handheld defaults never change with the switch.
        if (id == ProfileId::Handheld) assert(api.defaults(id).bindings[1].buttons[0] == "circle");
    }
    // settingsnative: the staged swap. Nothing staged: nothing to commit. A staged mapping is invisible until the commit, a reference taken before
    // it stays valid and unchanged after it (every mapping is immutable and never freed), and the commit counts.
    {
        const auto& api2 = GetControlApi();
        assert(!CommitStagedMapping());
        const CompiledMapping& before = ActiveMapping();
        const uint64_t cross = Cross, circle = Circle;
        assert(before.profile == ProfileId::Handheld && before.masks[static_cast<size_t>(Action::Jump)] == cross);
        const uint32_t gen0 = MappingGeneration();
        auto map = api2.defaults(ProfileId::Handheld);
        std::string conflict, error;
        assert(api2.bind(map, "jump", 0, "circle", true, NominalCaps(ProfileId::Handheld), conflict) == BindResult::Applied);
        assert(StageActiveMapping(map, error));
        assert(ActiveMapping().masks[static_cast<size_t>(Action::Jump)] == cross && MappingGeneration() == gen0);   // not yet
        assert(CommitStagedMapping() && !CommitStagedMapping());                                                    // once
        assert(ActiveMapping().masks[static_cast<size_t>(Action::Jump)] == circle && ActiveMapping().masks[static_cast<size_t>(Action::StarBits)] == cross);
        assert(MappingGeneration() == gen0 + 1);
        assert(before.masks[static_cast<size_t>(Action::Jump)] == cross && &before != &ActiveMapping());            // the old one is untouched
        // Not valid for the controller in use (L2 on a handheld), or not complete: refused, nothing staged.
        auto bad = api2.defaults(ProfileId::Handheld);
        bad.bindings[0].buttons[0] = "l2";
        assert(!StageActiveMapping(bad, error) && !error.empty() && !CommitStagedMapping());
        bad = api2.defaults(ProfileId::Handheld); bad.bindings.pop_back();
        assert(!StageActiveMapping(bad, error) && !CommitStagedMapping());
        // A second stage before the commit replaces the first.
        assert(StageActiveMapping(api2.defaults(ProfileId::Handheld), error));
        assert(StageActiveMapping(map, error));
        assert(CommitStagedMapping() && ActiveMapping().masks[static_cast<size_t>(Action::Jump)] == circle && !CommitStagedMapping());
        // The profile does not change with a swap.
        assert(ActiveMapping().profile == ProfileId::Handheld);
        // A stager on another thread and the input thread committing and reading: every read is one whole mapping.
        const auto defaults2 = api2.defaults(ProfileId::Handheld);
        std::atomic<bool> stop{false};
        std::thread stager([&] { std::string e; for (int i = 0; i < 400; ++i) StageActiveMapping(i & 1 ? map : defaults2, e); stop = true; });
        while (!stop) {
            CommitStagedMapping();
            const CompiledMapping& m = ActiveMapping();
            const uint64_t jump = m.masks[static_cast<size_t>(Action::Jump)], star = m.masks[static_cast<size_t>(Action::StarBits)];
            assert((jump == cross && star == circle) || (jump == circle && star == cross));
        }
        stager.join();
    }
}
