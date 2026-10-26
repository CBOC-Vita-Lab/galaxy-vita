#include "port/control_bindings.h"
#include <cstdlib>
#include <cstring>

namespace port::controls {
namespace {
constexpr ActionInfo actions[] = {
    {"jump", "Jump / Confirm / Hold", true, 1}, {"star_bits", "Star Bits / Aim", true, 1},
    {"spin", "Spin", true, 2}, {"crouch", "Crouch", true, 1},
    {"camera_centre", "Camera centre", true, 1}, {"pause", "Pause / title confirm", true, 1},
    {"minus", "Minus / Recalibrate", true, 1}, {"camera_up", "Camera / menu up", true, 1},
    {"camera_down", "Camera / menu down", true, 1}, {"camera_left", "Camera / menu left", true, 1},
    {"camera_right", "Camera / menu right", true, 1}
};
constexpr ButtonInfo buttons[] = {
    {"cross", "Cross", Cross}, {"circle", "Circle", Circle}, {"square", "Square", Square},
    {"triangle", "Triangle", Triangle}, {"l1", "L", L1}, {"r1", "R", R1},
    {"l2", "L2", L2}, {"r2", "R2", R2}, {"l3", "L3", L3}, {"r3", "R3", R3},
    {"start", "Start", Start}, {"select", "Select", Select}, {"up", "Up", Up},
    {"down", "Down", Down}, {"left", "Left", Left}, {"right", "Right", Right}
};
constexpr const char* defaults[] = {"cross", "circle", "square", "l1", "triangle", "start", "select", "up", "down", "left", "right"};
int actionIndex(const std::string& id) {
    for (size_t i = 0; i < ActionCount; ++i) if (id == actions[i].id) return static_cast<int>(i);
    return -1;
}
int buttonIndex(const std::string& id) {
    for (size_t i = 0; i < 16; ++i) if (id == buttons[i].id) return static_cast<int>(i);
    return -1;
}
ProfileMapping makeDefaults(ProfileId) {
    ProfileMapping result;
    for (size_t i = 0; i < ActionCount; ++i)
        result.bindings.push_back({actions[i].id, {defaults[i], i == 2 ? "r1" : ""}});
    return result;
}
bool validate(const ProfileMapping& mapping, ProfileId profile, const ControllerCaps& caps, std::string& error) {
    error.clear();
    if (profile != ProfileId::Handheld && profile != ProfileId::PlayStation) { error = "Unknown profile"; return false; }
    if (mapping.bindings.size() != ActionCount) { error = "Every action requires a binding"; return false; }
    bool seen[ActionCount]{};
    uint64_t used = 0;
    const uint64_t available = caps.availableButtons & NominalCaps(profile).availableButtons;
    for (const auto& binding : mapping.bindings) {
        const int a = actionIndex(binding.action);
        if (a < 0 || seen[a]) { error = "Unknown or duplicate action"; return false; }
        seen[a] = true;
        if (binding.buttons[0].empty()) { error = "Every action requires a primary binding"; return false; }
        for (unsigned slot = 0; slot < 2; ++slot) {
            if (binding.buttons[slot].empty()) continue;
            const int b = buttonIndex(binding.buttons[slot]);
            if (slot >= actions[a].maxBindings || b < 0 || !(available & buttons[b].mask)) {
                error = "Unavailable button"; return false;
            }
            if (used & buttons[b].mask) { error = "Button already assigned"; return false; }
            used |= buttons[b].mask;
        }
    }
    return true;
}
BindResult bind(ProfileMapping& mapping, const char* action, unsigned slot, const char* button,
                bool swap, const ControllerCaps& caps, std::string& conflict) {
    conflict.clear();
    if (!action || !button) return BindResult::Unavailable;
    const int a = actionIndex(action), b = buttonIndex(button);
    if (a < 0 || slot >= actions[a].maxBindings || slot > 1) return BindResult::Unavailable;
    if (!*button && slot == 0) return BindResult::RequiredActionLost;
    if (*button && (b < 0 || !(caps.availableButtons & buttons[b].mask))) return BindResult::Unavailable;
    ProfileMapping candidate = mapping;
    Binding* target = nullptr;
    Binding* other = nullptr;
    unsigned otherSlot = 0;
    for (auto& entry : candidate.bindings) {
        if (entry.action == action) target = &entry;
        if (*button) for (unsigned s = 0; s < 2; ++s)
            if (entry.buttons[s] == button && !(entry.action == action && s == slot)) other = &entry, otherSlot = s;
    }
    if (!target) return BindResult::Unavailable;
    if (other) {
        conflict = other->action;
        if (!swap) return BindResult::Conflict;
        const std::string old = target->buttons[slot];
        if (old.empty() && otherSlot == 0) return BindResult::RequiredActionLost;
        other->buttons[otherSlot] = old;
    }
    target->buttons[slot] = button;
    // Caps, rather than connection status, define whether this edit is reachable.
    const ProfileId nominal = (caps.availableButtons & (L2 | R2 | L3 | R3)) ? ProfileId::PlayStation : ProfileId::Handheld;
    std::string error;
    if (!validate(candidate, nominal, caps, error)) return BindResult::RequiredActionLost;
    mapping = std::move(candidate);
    return BindResult::Applied;
}
CompiledMapping makeCompiledDefaults() {
    CompiledMapping compiled;
    std::string error;
    Compile(makeDefaults(ProfileId::Handheld), ProfileId::Handheld, NominalCaps(ProfileId::Handheld), compiled, error);
    return compiled;
}
CompiledMapping active;
bool activeSet = false;
ControllerCaps activeCaps;
bool capsSet = false;
}
ControllerCaps NominalCaps(ProfileId profile) {
    ControllerCaps caps;
    caps.availableButtons = 0xffff;
    caps.connected = true;
    if (profile == ProfileId::Handheld) {
        caps.availableButtons &= ~(L2 | R2 | L3 | R3);
        caps.frontTouch = caps.rearTouch = caps.motion = true;
    } else caps.tiltFallback = true;
    return caps;
}
const ControlApi& GetControlApi() {
    static const ControlApi api{actions, ActionCount, buttons, 16, makeDefaults, validate, bind};
    return api;
}
bool Compile(const ProfileMapping& map, ProfileId profile, const ControllerCaps& caps, CompiledMapping& out, std::string& error) {
    if (!validate(map, profile, caps, error)) return false;
    CompiledMapping next;
    next.profile = profile;
    for (const auto& entry : map.bindings) {
        const size_t a = static_cast<size_t>(actionIndex(entry.action));
        next.primary[a] = static_cast<uint8_t>(buttonIndex(entry.buttons[0]));
        for (const auto& button : entry.buttons) if (!button.empty()) next.masks[a] |= buttons[buttonIndex(button)].mask;
    }
    out = next;
    return true;
}
Translation Translate(const CompiledMapping& map, uint64_t held) {
    constexpr uint32_t targets[] = {0x0800, 0x0400, 0, 0x2000, 0x4000, 0x0010, 0x1000, 8, 4, 1, 2};
    Translation result;
    for (size_t a = 0; a < ActionCount; ++a) if (held & map.masks[a]) result.buttons |= targets[a];
    result.spin = (held & map.masks[static_cast<size_t>(Action::Spin)]) != 0;
    return result;
}
const ButtonInfo* PrimaryButton(const CompiledMapping& map, Action action) {
    const size_t a = static_cast<size_t>(action);
    if (a >= ActionCount || map.primary[a] >= 16) return nullptr;
    return &buttons[map.primary[a]];
}
bool SetActiveMapping(const ProfileMapping& map, ProfileId id, const ControllerCaps& caps, std::string& error) {
    if (activeSet) { error = "Active controls are immutable until restart"; return false; }
    if (!Compile(map, id, caps, active, error)) return false;
    activeSet = true;
    return true;
}
const CompiledMapping& ActiveMapping() {
    if (!activeSet) { active = makeCompiledDefaults(); activeSet = true; }
    return active;
}
void FreezeActiveCapabilities(const ControllerCaps& caps) {
    if (!capsSet) { activeCaps = caps; capsSet = true; }
}
const ControllerCaps& ActiveCapabilities() {
    if (!capsSet) FreezeActiveCapabilities(NominalCaps(ActiveMapping().profile));
    return activeCaps;
}
bool RemapEnabled() {
    static const bool enabled = [] { const char* e = std::getenv("GXV_REMAP"); return e && std::strcmp(e, "1") == 0; }();
    return enabled;
}
} // namespace port::controls
