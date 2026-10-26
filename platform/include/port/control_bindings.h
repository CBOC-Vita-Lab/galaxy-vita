#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace port::controls {
enum class ProfileId : uint8_t { Handheld, PlayStation };
struct Binding { std::string action; std::array<std::string, 2> buttons; };
struct ProfileMapping { std::vector<Binding> bindings; };
struct ActionInfo { const char* id; const char* label; bool required; uint8_t maxBindings; };
struct ButtonInfo { const char* id; const char* label; uint64_t mask; };
struct ControllerCaps {
    uint64_t availableButtons = 0;
    bool connected = false, frontTouch = false, rearTouch = false, motion = false, tiltFallback = false;
};
enum class BindResult : uint8_t { Applied, Conflict, Unavailable, RequiredActionLost };
struct ControlApi {
    const ActionInfo* actions; size_t actionCount;
    const ButtonInfo* buttons; size_t buttonCount;
    ProfileMapping (*defaults)(ProfileId);
    bool (*validate)(const ProfileMapping&, ProfileId, const ControllerCaps&, std::string& error);
    BindResult (*bind)(ProfileMapping&, const char* action, unsigned slot, const char* button,
                      bool swap, const ControllerCaps&, std::string& conflictAction);
};

// Stable normalized bits, never raw platform SDK masks.
enum Button : uint64_t {
    Cross = 1ull << 0, Circle = 1ull << 1, Square = 1ull << 2, Triangle = 1ull << 3,
    L1 = 1ull << 4, R1 = 1ull << 5, L2 = 1ull << 6, R2 = 1ull << 7,
    L3 = 1ull << 8, R3 = 1ull << 9, Start = 1ull << 10, Select = 1ull << 11,
    Up = 1ull << 12, Down = 1ull << 13, Left = 1ull << 14, Right = 1ull << 15
};
enum class Action : uint8_t {
    Jump, StarBits, Spin, Crouch, CameraCentre, Pause, Minus,
    CameraUp, CameraDown, CameraLeft, CameraRight, Count
};
constexpr size_t ActionCount = static_cast<size_t>(Action::Count);
struct CompiledMapping {
    std::array<uint64_t, ActionCount> masks{};
    std::array<uint8_t, ActionCount> primary{};
    ProfileId profile = ProfileId::Handheld;
};
struct Translation { uint32_t buttons = 0; bool spin = false; };
const ControlApi& GetControlApi();
ControllerCaps NominalCaps(ProfileId);
bool Compile(const ProfileMapping&, ProfileId, const ControllerCaps&, CompiledMapping&, std::string& error);
Translation Translate(const CompiledMapping&, uint64_t held);
const ButtonInfo* PrimaryButton(const CompiledMapping&, Action);
// Called once before input/game initialization. The immutable copy also drives prompts.
bool SetActiveMapping(const ProfileMapping&, ProfileId, const ControllerCaps&, std::string& error);
const CompiledMapping& ActiveMapping();
void FreezeActiveCapabilities(const ControllerCaps&);
const ControllerCaps& ActiveCapabilities();
bool RemapEnabled();
} // namespace port::controls
