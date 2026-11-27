#pragma once
#include "port/control_bindings.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace port::settings {
constexpr size_t kDocumentLimit = 16384;
// Config::fpsLimit: 0 with Config::performance60 is the 60 mode (shown as "Performance"), 30 the 30 mode ("Quality"); kSteady30 (1.1's Steady 30) is read as 30.
constexpr int kSteady30 = -30;
enum class AutoQuality : uint8_t { Default, Enabled, Disabled };
// Picture and speed choices of the Display page (all optional keys of the file: absent means the default). Each maps to existing
// start-up switches in ApplyStartup. Anti-aliasing: Auto lets dynamic resolution use its multisampled rungs, Off removes them.
enum class AntiAliasing : uint8_t { Auto, Off };
enum class Sharpness : uint8_t { Auto, Off, Strong };
enum class EffectsQuality : uint8_t { Adaptive, Full };
enum class FarAnimation : uint8_t { Reduced, Full, Aggressive };
// Keys: atomic_saves, perf_mode, above_native, mario_first, anti_aliasing, picture_sharpness, hd_textures, effects_quality, depth_of_field, core_plan, gxm_renderer, logic_overlap, material_updates, far_animation, game_speed, motion_level,
// motion_source. (Every optional
// key sorts before "schema" and "reduced_motion" stays the last line of a fresh file, so a cut-short write is still caught as incomplete.)
// The Preset row is not stored: it is read from the rows a preset covers (Dynamic resolution, Sharper when possible, Prioritize
// Mario, Anti-aliasing, Sharpness, Effects quality, Depth of field); any other combination reads Custom.
enum class Preset : uint8_t { BestPicture, Balanced, Performance, Custom };
struct Config {
    uint32_t version = 1;
    // The Display page's Picture mode row (modes26, 12 Nov; ApplyStartup maps it to start-up switches). Two modes only:
    //   Performance (60)  fpsLimit 0, performance60 on (the default): 960x544 aiming at an even 60; the per-stage table's cuts apply (GXV_PICTURE_MODE=60);
    //   Quality (30)      fpsLimit 30, performance60 off: a locked, evenly paced 30 at 960x544 with every quality setting on and none of the 60 mode's cuts (GXV_PICTURE_MODE=30).
    // Files from before: perf_mode=1 reads Performance; fps_limit 30 / steady30 and the old default (fps_limit 0 without perf_mode) read Quality (ParseDocument); an
    // unknown fps_limit value reads Performance. The file keeps writing fps_limit and perf_mode (an older build reads them), so Mode30() is all a caller needs.
    // The rows Preset, Dynamic resolution, Sharper when possible, Prioritize Mario, Anti-aliasing and Effects quality are no longer shown; their keys stay in the file
    // (read and written as before) but ApplyStartup ignores them (the two modes fix what they chose).
    int fpsLimit = 0;
    bool performance60 = true;
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
    bool coreHelp = true;                  // core_plan: Auto (true) lets disc read-ahead and a movie decoder use the system core, and
                                           // puts vitaGL's collector and the audio mixer on core 1 (off the GX front end's core 2)
    // 1.3 speed rows (each maps to one start-up switch; all on by default, Off returns that part to 1.2's behaviour):
    bool gxmDirect = true;                 // gxm_renderer (direct | standard): GXV_GXM_DIRECT, one direct vitaGL call per scene draw
    bool logicOverlap = true;              // logic_overlap (1 | 0 | max): GXV_DRAWDONE_DEFER=2 (game logic overlaps the front end) vs 1
    bool overlapMax = true;                // logic_overlap=max: GXV_DRAWDONE_DEFER=3, turns without a picture never wait (needs logicOverlap)
    bool lazyMaterials = true;             // material_updates (per_picture | every_turn): GXV_DL_LAZY=2 vs 0
    FarAnimation farAnimation = FarAnimation::Reduced; // far_animation (reduced | full | aggressive): GXV_ANIM_LOD, GXV_ANIM_LOD_DIST
    // game_speed (full | smooth): Full keeps the game at 60 turns a second wherever the CPU allows, skipping pictures (GXV_SLIP=0,
    // PETARI_FRAMESKIP=5); Smooth is 1.2's trade (up to 20 % slower game for steadier pictures, GXV_SLIP=1, 3 skips in a row).
    bool fullSpeed = true;
    // atomic_saves (optional key, default 0; sorts before reduced_motion, the last key a complete file ends with): the single-player GameData.bin is written through a temporary file, checked, and swapped in, keeping the
    // previous save as GameData.bin.mpbak (platform/src/nand/nand.cpp). Off: the game's own in-place write. Multiplayer sessions always use the
    // safe write. Reaches the game as SMG_ATOMIC_SAVES at launch (an env.txt value overrides).
    bool safeSaves = false;
    // Tilt levels (Star Ball, ray surfing): how far the lean is scaled, on a 1 to 10 scale (5 is the game's own, unchanged steering, 1 half and
    // 10 double; platform/include/port/tilt_gain.h), and whether the left stick steers them instead of the console's motion sensor.
    // Optional keys motion_level and motion_source (sensor | stick); a 1.2 beta file's motion_gain (a percentage) is read as the nearest level.
    // A console without a motion sensor always steers by the stick.
    int tiltLevel = 5;
    bool tiltStick = false;
    // setnat31, Settings "60 Assist" (optional key assist60, 1 | 0; absent = 1): in Smooth Motion (the Performance mode) every other picture of the 60 is
    // estimated from the one before (GXV_HRW=5, the mode key table's Performance value, c181). 0 asks for GXV_HRW=0 at start-up: only real pictures,
    // aiming at an even 60 (the plain 60 target). Nothing changes in Full Detail. Read once at start-up (ApplyStartup); an env.txt GXV_HRW wins.
    // The default (1) is c181's behaviour until the owner's console A/B picks one. Older builds ignore the line (not a key they manage).
    bool assist60 = true;
    // mp30 (retired from Settings UI in Wave 4): optional key allow50 kept accepted in settings.cfg for backward compatibility,
    // forced off in NativeRetire and ignored by ApplyStartup; GXV_HRW_ALLOW50=1 in env.txt remains as the developer escape hatch.
    bool allow50 = false;
    std::array<controls::ProfileMapping, 2> profiles;
};
enum class StoreStatus : uint8_t { Ok, Missing, Recovered, Malformed, UnsupportedVersion, TooLarge, IoError };
// onlyChanged (the native Settings): a save writes only the keys whose value changed from `config` (the file as loaded). A line of the file is kept as it
// is when its value did not change; a key the file does not have is added only when it is a required key or its value is not the default (absent = default).
struct Document { Config config; std::string source; bool editable = true; bool onlyChanged = false; };
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
// The 30 mode (Quality): fpsLimit 30, or the old Quality state (neither Performance nor a lock) which reads as 30.
inline bool Mode30(const Config& c) { return c.fpsLimit != 0 || !c.performance60; }
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
// setfix31: the release stamp every writer puts on the file (the settings guard's comment line, port/settings_guard.h GuardStampLine), so a file this
// build wrote is never taken for a file of 2.2.1 or earlier at the next start. Set once at start-up, only when the guard found the file current (or
// migrated it, or there was none): a file the guard could not migrate stays unstamped, so the next start migrates it with its backup as before.
// SerializeDocument puts the line first when the text has no stamp of any release; a stamp already there (this release's or a newer one) is kept.
// Empty (the default, and every host tool): nothing is added.
void SetWriterStamp(const std::string& line);
const std::string& WriterStamp();
StoreResult LoadDocument(const char*, const controls::ControlApi&, const FileOps&, Document&);
StoreResult SaveDocument(const char*, const Document&, const Config&, const controls::ControlApi&, const FileOps&, Document&);
// Puts already-serialized text in place of the file: written complete and synced under a temporary name, read back and compared to `config`, the old file rotated to .bak.
// SaveDocument ends in this; the stale-settings guard (port/settings_guard.h) calls it directly. Same results as SaveDocument.
StoreResult ReplaceDocumentText(const char*, const std::string&, const Config&, const controls::ControlApi&, const FileOps&, Document&);
}
