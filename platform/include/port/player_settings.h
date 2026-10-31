#pragma once
#include "port/control_bindings.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace port::settings {
constexpr size_t kDocumentLimit = 16384;
// Config::fpsLimit: 0 up to 60, 30 fixed 30, kSteady30 adaptive pacing with dynamic quality aimed at 30.
constexpr int kSteady30 = -30;
enum class AutoQuality : uint8_t { Default, Enabled, Disabled };
// Picture and speed choices of the Display page (all optional keys of the file: absent means the default). Each maps to existing
// start-up switches in ApplyStartup. Anti-aliasing: Auto lets dynamic resolution use its multisampled rungs, Off removes them.
enum class AntiAliasing : uint8_t { Auto, Off };
enum class Sharpness : uint8_t { Auto, Off, Strong };
enum class EffectsQuality : uint8_t { Adaptive, Full };
// Keys: above_native, mario_first, anti_aliasing, picture_sharpness, hd_textures, effects_quality, depth_of_field, core_plan, motion_level,
// motion_source. (Every optional
// key sorts before "schema" and "reduced_motion" stays the last line of a fresh file, so a cut-short write is still caught as incomplete.)
// The Preset row is not stored: it is read from the rows a preset covers (Dynamic resolution, Sharper when possible, Prioritize
// Mario, Anti-aliasing, Sharpness, Effects quality, Depth of field); any other combination reads Custom.
enum class Preset : uint8_t { BestPicture, Balanced, Performance, Custom };
struct Config {
    uint32_t version = 1;
    int fpsLimit = 0;
    AutoQuality autoQuality = AutoQuality::Default;
    controls::ProfileId profile = controls::ProfileId::Handheld;
    std::string language;
    bool reducedMotion = false;
    // Mouse-style Star Bit pointer aiming (vita/runtime/input.cpp, platform/include/port/pointer_aim.h): the right stick steers the
    // pointer by speed with acceleration. Off: the 1.0 linear stick-to-pointer-speed behaviour. Optional key mouse_aim.
    bool mouseAim = false;
    // The profile row was changed by hand in the Settings menu; until then a pad on a PS TV gets the PlayStation profile (see
    // ApplyPadDefaults). Optional key profile_chosen.
    bool profileChosen = false;
    bool aboveNative = true;               // Sharper when possible: dynamic resolution may climb above the game's 640x456 (keys: above_native)
    bool prioritizeMario = true;           // Mario-first ladder and Mario/Luigi edge smoothing (mario_first)
    AntiAliasing antiAliasing = AntiAliasing::Auto;  // anti_aliasing
    Sharpness sharpness = Sharpness::Auto;            // sharpness
    bool hdTextures = true;                // hd_textures (matters only when the HD pack is installed)
    EffectsQuality effects = EffectsQuality::Adaptive; // effects_quality
    bool depthOfField = true;              // depth_of_field: the game's own depth-of-field blur
    bool coreHelp = true;                  // core_plan: Auto (true) lets disc read-ahead and a movie decoder use the system core
    // Tilt levels (Star Ball, ray surfing): how far the lean is scaled, on a 1 to 10 scale (5 is the game's own, unchanged steering, 1 half and
    // 10 double; platform/include/port/tilt_gain.h), and whether the left stick steers them instead of the console's motion sensor.
    // Optional keys motion_level and motion_source (sensor | stick); a 1.2 beta file's motion_gain (a percentage) is read as the nearest level.
    // A console without a motion sensor always steers by the stick.
    int tiltLevel = 5;
    bool tiltStick = false;
    std::array<controls::ProfileMapping, 2> profiles;
};
enum class StoreStatus : uint8_t { Ok, Missing, Recovered, Malformed, UnsupportedVersion, TooLarge, IoError };
struct Document { Config config; std::string source; bool editable = true; };
struct StoreResult { StoreStatus status; std::string message; };
struct FileInfo { uint64_t size; bool regular; };
struct FileOps {
    void* context;
    int64_t (*openRead)(void*, const char*);
    int64_t (*openWrite)(void*, const char*);
    int64_t (*read)(void*, int64_t, void*, size_t);
    int64_t (*write)(void*, int64_t, const void*, size_t);
    int (*sync)(void*, int64_t);
    int (*close)(void*, int64_t);
    int (*stat)(void*, const char*, FileInfo&);
    int (*rename)(void*, const char*, const char*);
    int (*remove)(void*, const char*);
};
Config DefaultConfig(const controls::ControlApi&);
// The rows a preset covers, set together; the other settings are left alone.
void ApplyPreset(Config&, Preset);
Preset PresetOf(const Config&);
// With an external pad (PS TV: DS3 or DS4) connected and the profile never chosen by hand, a Handheld profile that still holds
// the default buttons is switched to the PlayStation profile, so the pad gets its own default layout. Returns true if it switched.
// Does nothing with GXV_PAD_DEFAULTS=0.
bool ApplyPadDefaults(Config&, bool externalPad);
bool Equal(const Config&, const Config&);
StoreResult ParseDocument(const std::string&, const controls::ControlApi&, Document&);
StoreResult SerializeDocument(const Document&, const Config&, const controls::ControlApi&, std::string&);
StoreResult LoadDocument(const char*, const controls::ControlApi&, const FileOps&, Document&);
StoreResult SaveDocument(const char*, const Document&, const Config&, const controls::ControlApi&, const FileOps&, Document&);
}
