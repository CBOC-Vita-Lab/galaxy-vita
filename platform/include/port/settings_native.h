#pragma once
// Native in-game Settings: the MODEL (settings-native plan, 14 Nov, Layer A).
//
// A pure C++ layer over the existing port::settings Config / Document / SaveDocument / FileOps and controls::ControlApi. It is not a second
// settings model: it edits a COPY of the loaded Config and writes the whole Config back through SaveDocument, so every key the menu does not
// show (the technical ones, unknown keys, comments) stays as the file has it. No game (decomp) headers, no English text: a row and a value
// are named by a stable key (a short ASCII string) which the menu maps to its own text ids.
//
// Host: the platform fills a NativeHost (path, file functions, the Config the game started with, env.txt overrides, a relaunch function).
// Desktop/armhf: platform/src/settings/settings_native_host.cpp. Vita: the "settingsnative" block at the end of vita/vgx-game/settings_runtime.cpp.
// Tests install a fake with NativeHostSetForTest.
#include "port/settings_ui.h"   // Override, LanguageCatalog (types only)
#include <string>
#include <vector>

namespace port::settings {

// The rows of the menu (plan v3). Picture, Language and 60 Assist are restart rows; the rest are live (they apply at once). Safe Saves, Layout and the
// technical rows are gone from the menu (their values stay as the file has them).
// Assist60 (setnat31, "60 Assist"): Config::assist60, key assist60; shown under Picture only while Picture is Smooth Motion; "off" / "on", standard on (c181);
// env.txt's GXV_HRW overrides it (read from the mode-key snapshot, port/mode_keys.h UserHas). Appended last so the other rows keep their numbers.
// Allow50 (retired from Settings UI in Wave 4): enum kept for ABI stability; RowShown always returns false, NativeRetire forces Config::allow50 off,
// and ApplyStartup ignores it (GXV_HRW_ALLOW50 in env.txt remains as the escape hatch; legacy allow50=1 files still parse cleanly).
enum class NativeRow : uint8_t { Picture, Language, LessMotion, Tilt, TiltStrength, Pointer, CameraZoom, RightStick, InvertCamera, RearTouch, TapToJump, StarBitReach, Assist60, Allow50, CameraSpeed, Count };
constexpr unsigned kNativeRows = static_cast<unsigned>(NativeRow::Count);

// The live rows read and write the thing that uses them, through this table (the real one sits next to the readers: perf.cpp's port_zoom_*,
// port_rear_aim_*, port_stick_pointer_*, port_star_bit_reach_*; input.cpp; reduced_motion.h; tilt_gain.h). Values are the row's VALUE INDEX
// (see NativeModel::ValueIndex). Every function may be null.
struct NativeLiveOps {
    void* context;
    int (*get)(void*, NativeRow);                       // the value index now; -1: no reader here, so the row is hidden
    bool (*set)(void*, NativeRow, int index);           // apply at once; false: not applied (the change then waits for the next start)
    void (*commit)(void*);                              // write the rows kept in their own file (the pause panel's settings.ini); null: they save themselves
    bool (*applyBindings)(void*, const Config&, controls::ProfileId effective);   // swap the active buttons (Config.profiles[effective], the controller in use) at a frame boundary; false: they start at the next start
    int (*standard)(void*, NativeRow);                  // the standard value index of a row; -1 or null: the model's own (zoom 1.5, the rest 0, Tap to Jump on)
};
// The forwarding table over the live functions (all weak references, so a host test needs none; a missing one answers -1 / false and the row is hidden):
// perf.cpp (zoom, rear touch, right stick, Star Bit Reach, Invert Camera, Tap to Jump), reduced_motion.h, tilt_gain.h, input.cpp (Pointer) and
// control_bindings.cpp StageActiveMapping (the buttons). Their authors are agent D (setnat-pause); see port_*_enabled / port_*_set below.
const NativeLiveOps* NativeLiveOpsPort();

// What the platform gives the native menu. Every pointer outlives the process (or the fake that a test installs).
struct NativeHost {
    const char* path;                         // the player-settings.cfg ( .tmp, .bak and .untested go beside it)
    const FileOps* files;
    const Config* active;                     // the Config this run started with (the boot Config); null = the loaded file's
    const std::vector<Override>* overrides;   // what ApplyStartup reported (may be null)
    bool (*relaunch)();                       // start the game again; true is never seen on a console (the process is gone); false = it did not restart
    bool available;                           // false: the platform has no native Settings (Vita with GXV_SETTINGS=0): the menu shows no entry
    bool fixed30;                             // the driver can hold a locked 30: "Full Detail" is possible
    bool motion;                              // the console has a motion sensor: the Tilt row exists
    // Added with plan v3 (null / false = not there):
    const NativeLiveOps* live;
    bool (*relaunchReady)();                  // nothing is being written: no game save sequence running and no card write queued. The menu polls it every frame (never waits inside a call) before Relaunch()
    const char* (*systemLanguage)();          // the console's language as the disc names it ("english", "french" ...), or null (Vita: the system parameter; desktop: PETARI_SYSTEM_LANGUAGE only, never LANG)
    bool (*envPresent)(const char* key);      // the env.txt key was set at start (a snapshot taken before the menu's own choices were applied)
    bool externalPad;                         // a pad on a PS TV (or other external controller) is the one in use: Rear Touch is hidden
    // setfix31b (null = not there): while "Save and Start Again" waits for a save in flight. saveProgress counts finished save / card operations (it moves
    // while saving goes on); saveFailures counts the ones that failed; relaunchSafe: starting the game again during a save cannot damage the game save
    // (Safe Saves: the save is written to a temporary and swapped in, so a cut leaves the old save or the new one).
    unsigned (*saveProgress)() = nullptr;
    unsigned (*saveFailures)() = nullptr;
    bool (*relaunchSafe)() = nullptr;
};
// The platform's host (null when the platform has none), or the fake a test installed.
const NativeHost* NativeHostGet();
// The platform hook behind NativeHostGet: defined by settings_native_host.cpp (desktop) or settings_runtime.cpp (Vita); a weak null
// default lives in settings_native.cpp, so a host test needs neither.
const NativeHost* NativeHostPlatform();
// Tests: install a fake host (pass null to go back to the platform's). Not thread safe; call before the model is used.
void NativeHostSetForTest(const NativeHost*);

// Process-wide "the native Settings menu is open" flag (two editors guard): the LiveArea Settings request is dropped while it is set
// (port_settings_game_boundary). Set by the menu when it opens, cleared when it closes.
void NativeMenuOpenSet(bool open);
bool NativeMenuOpen();

// ---- start-up: the run's Config, before ApplyStartup (so env.txt, applied after, still wins) ----
// Start-up, in this order, before ApplyStartup (so env.txt, applied after, still wins): NativeBootCheck, then NativeRetire. NativeStartup does both.
// NativeRetire leaves every field the menu no longer shows exactly as the player's file has it (the 2.3 tuning: game speed, sharpness, HD textures, depth of
// field, core plan, renderer, logic overlap, material updates, far animation, picture rows) and forces Safe Saves ON (3.0: always on) and Allow50 OFF (retired from UI). The file is never touched.
// It also (1) records which row keys env.txt had set, with one getenv per key, BEFORE ApplyStartup sets GXV_MOUSE_AIM or GXV_REDUCED_MOTION itself
// (NativeEnvWasPresent; the keys are GXV_PICTURE_MODE, GXV_FPS_LIMIT, GXV_AUTOQ_TARGET (unless "30", the start-up default), PETARI_LANGUAGE, GXV_REDUCED_MOTION,
// GXV_TILT_STICK, GXV_TILT_GAIN, GXV_MOUSE_AIM, PETARI_ZOOM_FORCE, PETARI_RSTICK, PETARI_TOUCH_A, PETARI_STAR_BIT_REACH, and GXV_HRW from the mode-key snapshot), and (2) hands `systemLanguage`
// ("english" ...; null = none) to port_language_set_fallback (platform/src/dvd/dvd.cpp): the language the "System Language" choice means, used when the Config
// names no language and the disc has its texts. The Config's own language is not changed.
void NativeRetire(Config&, const char* systemLanguage = nullptr);
// The recovery marker: <settings path without .cfg>.untested, 3.0-owned, one line: "try N" or "hold".
//  - A native save that changes Picture or Language (or ends a hold on a non-standard pair) writes "try 0". A save of the standard pair removes the marker.
//  - NativeBootCheck at each start: "try N" with N >= 2 becomes "hold"; otherwise it is rewritten as "try N+1" and the start runs normally.
//  - NativeEntryReached() (the first MpEntry::fileSelectStep, which always runs) deletes a "try" marker.
//  - (setnat31) a change of 60 Assist counts as a Picture change here, and the standard pair includes 60 Assist on.
//  - "hold": EVERY start runs with the standard Picture (Smooth, 60 Assist on) and the disc's FIRST language (no fallback registered) and reports toast = true (one toast
//    per start). The settings file is never touched. The menu then shows the running values (NativeModel::Open); the player's next Save ends the hold
//    (the marker is removed when the saved pair is the standard one; picking Full Detail or a language again writes "try 0").
//  - A marker that cannot be read counts as "try 1". It cannot loop: hold never counts and never clears itself.
struct NativeBootResult { bool standard = false; bool toast = false; };
NativeBootResult NativeBootCheck(const NativeHost&, Config& runConfig);
void NativeEntryReached(const NativeHost&);
void NativeEntryReached();                       // the same on NativeHostGet(), once per process (the hook agent C calls)
std::string NativeMarkerPath(const char* settingsPath);
// All of it, for the Vita start-up (one line in settings_runtime.cpp, after the pad defaults, before ApplyStartup). Does nothing when the host is not available.
void NativeStartup(Config& runConfig);
bool NativeTakeBootToast();                      // true once after a recovery start (a hold start); the toast of every hold start is this
bool NativeStandardRun();                        // this run started in a hold: standard Picture, the disc's first language
bool NativeEnvWasPresent(const char* key);       // the snapshot of NativeRetire
void NativeBootStateClearForTest();

// The 3.0 menu side implements this (weak reference in settings_runtime.cpp): the LiveArea Settings tile of an OLD install (the Vita does not refresh a
// LiveArea) still sends "-settings", and it must open the NATIVE Settings, never the old software menu.
//   cold = 1: the game was started with -settings. Latch "open Settings at the entry screen" (the entry screen takes the latch). Return true: taken.
//   cold = 0: a running game got the request (game thread, at a frame boundary). Open from pause if that is possible (a single-player level, the pause
//             menu up, not in a session); otherwise refuse with a log line and the "can't" sound. Return true: handled (it is always handled).
// With the function absent (a build without the 3.0 menu) the old behaviour stays.
// The defining unit (decomp/src/Game/MP/MpUiSystem.cpp) defines SETTINGS_NATIVE_DEFINES_STRONG before its first include, so its definition is
// strong (T): a definition that inherits this weak declaration shows as W, which the post-link check (tools/vita_must_define.txt, T only) rejects.
#ifdef SETTINGS_NATIVE_DEFINES_STRONG
extern "C" bool port_settings_native_request(int cold);
#else
extern "C" bool port_settings_native_request(int cold) __attribute__((weak));
#endif

// Where the menu is open from, and what it may change there.
enum class NativeWhere : uint8_t { Entry, Pause, Session };
// Why a row cannot change now (None: it can).
enum class RowBlock : uint8_t { None, ReadOnly, Overridden, Unavailable, Together /* in a session: change it when not playing together */, StarRace };
// Why a Step did or did not change the value.
enum class StepResult : uint8_t {
    Changed,
    AtEnd,        // Tilt Strength, Camera Zoom, Star Bit Reach at their ends (they stop; the others wrap or toggle)
    Overridden,   // an env.txt key beats the file for this row
    Unavailable,  // this console cannot do it now (Full Detail without the fixed 30, no motion sensor, one disc language, no reader)
    ReadOnly,     // the file is from a newer game (schema > 1): nothing can be saved
    NotNow        // Together or StarRace (see RowBlock)
};
// What kind of row it is.
struct NativeRowInfo {
    bool live;                 // applies at once (previewed while the menu is open)
    bool restart;              // needs the next start (Picture, Language)
    bool shownEntry, shownPause, shownSession;
};
// The outcome of binding a button.
struct BindOutcome {
    enum Kind : uint8_t { Ok, Conflict, RequiredActionLost, Unavailable } kind = Ok;
    std::string conflictMove;   // Conflict: the id of the move that already uses the button
};
struct MoveInfo { std::string id; unsigned slots; bool required; };            // slots 1, or 2 (Spin)
struct ButtonChoice { std::string id; std::string label; uint64_t mask; };       // label is the API's own text: a fallback only

class NativeModel {
public:
    // Loads the file (LoadDocument) and copies the working Config from it. `languages`: the disc's language names (ReadLanguages().names);
    // may be empty. The host must outlive the model. The returned status is the load's (also LoadStatus()): Ok, Missing (defaults, editable),
    // Recovered (read from the .bak), Malformed/IoError/TooLarge (defaults, editable: a save then replaces the bad file), UnsupportedVersion (read-only).
    // The live rows are read through host.live. The file's Config is NOT changed by the pad defaults; the buttons shown and edited are those of Profile().
    StoreResult Open(const NativeHost& host, const std::vector<std::string>& languages);
    StoreResult LoadStatus() const { return loaded_; }
    bool LoadDamaged() const;     // Recovered, Malformed, IoError or TooLarge: the menu shows the one-time "couldn't be read" toast
    // The controller attached now (button picking and validation use it); default: the profile's nominal buttons.
    void SetController(const controls::ControllerCaps&, controls::ProfileId detected);
    // Where the menu is open from; in a session only the personal live set can change, and Star Bit Reach not in a Star Race.
    void SetWhere(NativeWhere, bool starRace = false);
    NativeWhere Where() const { return where_; }

    const Config& Working() const { return working_; }
    const Document& Loaded() const { return document_; }
    const Config& Boot() const { return boot_; }
    bool Editable() const { return document_.editable; }          // false: newer file, everything is read-only
    bool Available() const { return host_ && host_->available; }

    // ---- the rows ----
    static const char* RowKey(NativeRow);     // "picture", "language", "less_motion", "tilt", "tilt_strength", "pointer", "camera_zoom", "right_stick", "invert_camera", "rear_touch", "tap_to_jump", "star_bit_reach"
    static NativeRowInfo RowInfo(NativeRow);
    bool RowShown(NativeRow) const;           // false: Tilt without a motion sensor; Language with fewer than 2 disc languages; Invert Camera unless the right stick is the camera;
                                              // Rear Touch with an external pad; a live row the host has no reader for
    unsigned ValueCount(NativeRow) const;     // Picture 2, Language 1 + discs, switches 2, Tilt Strength 10, Camera Zoom 11 (1.0..2.0), Star Bit Reach 5
    unsigned ValueIndex(NativeRow) const;
    // Stable key of a value, never English: picture: "smooth" "detailed"; assist60: "off" "on"; language: "system" then "name" (use LanguageName); less_motion, tap_to_jump,
    // invert_camera, rear_touch: "off" "on"; tilt: "console" "stick"; tilt_strength: "1".."10"; pointer: "steady" "quick"; right_stick: "camera" "pointer";
    // camera_zoom: "1.0".."2.0"; star_bit_reach: "1".."5". (Index 0 is the first listed.)
    const char* ValueKey(NativeRow, unsigned index) const;
    const std::string& LanguageName(unsigned valueIndex) const;   // value index 1.. of the Language row; "" for 0 or out of range
    std::string SystemLanguageName() const;                // the disc language the console's language maps to; "" when the disc lacks it
    RowBlock Blocked(NativeRow) const;
    bool IsOverridden(NativeRow) const;       // an env.txt key set at start beats this row (by the key being PRESENT, not by its value)
    bool CanStep(NativeRow row) const { return Blocked(row) == RowBlock::None; }
    StepResult Step(NativeRow, int direction);        // +1 / -1; toggles ignore the sign, Language wraps, Tilt Strength / Zoom / Reach stop at the ends
    StepResult ResetRow(NativeRow);                   // the row's standard value (Tilt Strength 5, Camera Zoom 1.5 ...)
    // Every Settings row and the buttons of the controller in use back to standard (Picture Smooth, System Language, ..., Defaults()). Rows that cannot change now
    // (an env.txt key, a session) stay. Removed fields and unknown keys are not touched.
    void PutEverythingBack();

    // ---- the state ----
    bool Dirty() const;           // the working Config differs from the baseline (the loaded one), or a live row moved
    bool RestartNeeded() const;   // the SAVED Picture, 60 Assist (in Smooth Motion) or the effective Language differ from what is running. NOTHING else counts (Equal(saved, boot) would differ for nearly everyone after the retirement and the pad defaults).
    bool WouldNeedRestart() const;// the same for the working Picture and Language (what saving now would leave)
    bool NextStartOnly() const;   // a live row the host could not apply, or buttons it could not swap in, now saved or working: the band says "starts next time"
    bool InHold() const { return hold_; }   // this run started in a hold (see NativeBootCheck): the working Picture and Language are the running ones
    bool PictureOrLanguageChanged() const;   // working vs loaded: the leave question offers "Save and Start Again" at the entry screen
    // Validates both control profiles (and the attached controller's) as settings_ui.cpp Save() does, then SaveDocument. Ok = saved: the
    // loaded Document becomes the saved one, the live rows are committed (host.live->commit) and the buttons are handed to host.live->applyBindings.
    // A newer file returns UnsupportedVersion; a validation failure returns Malformed with its text. See the recovery marker in NativeBootCheck.
    // In a session (SetWhere) the document is serialized and handed to port_card_write_async (weak; os_alarm.cpp), so no card call is made on the game thread;
    // SaveQueued() tells which way the last Save went (the synchronous path is the fallback when the queue refuses or is absent). A queued save skips the .bak rotation.
    StoreResult Save();
    bool SaveQueued() const { return queued_; }
    void Revert();                // working = loaded, and every live preview is undone through the table
    // Restart: the menu asks RelaunchReady() every frame (it never waits inside a call), then Relaunch() (host->relaunch).
    bool RelaunchReady() const;   // host.relaunchReady (true when the host has none)
    unsigned SaveProgress() const;   // setfix31b: host.saveProgress (0 when the host has none)
    unsigned SaveFailures() const;   // host.saveFailures (0 when the host has none)
    bool RelaunchSafe() const;       // host.relaunchSafe (false when the host has none: then "Start Again Now" is never offered while a save is in flight)
    bool Relaunch() const;        // host->relaunch (false when there is none, or it returned)

    // ---- the buttons ----
    controls::ProfileId Profile() const;    // the profile of the controller in use: the file's, or PlayStation when ApplyPadDefaults would pick it (a pad on a PS TV)
    bool BindingsEnabled() const;           // not read-only, not in a session, remapping on
    std::vector<MoveInfo> Moves() const;    // every move of the control API, in its order
    std::string ButtonOf(const std::string& move, unsigned slot) const;   // the button id on that slot of the Profile(); "" = none
    std::vector<ButtonChoice> Buttons() const;                     // the buttons Profile() may use (the attached controller's, within the profile's)
    // bind: a button on a move's slot of Profile() (button "" clears a second slot: Spin's "None"). swap=true resolves a Conflict by swapping.
    BindOutcome Bind(const std::string& move, unsigned slot, const std::string& button, bool swap);
    void Defaults();                                               // Profile()'s buttons back to its defaults
    bool RemapAvailable() const;                                   // button changes exist in this build (GXV_REMAP not off)
    bool BindingsOverridden() const;                               // env.txt turns remapping off or the active mapping needs another controller
    bool BindingsChanged() const;                                  // the working buttons differ from the loaded ones

private:
    controls::ControllerCaps EditCaps() const;
    bool ExternalPad() const;
    bool IsLive(NativeRow) const;          // a row read and written through host.live
    std::string EffectiveLanguage(const Config&) const;
    int LiveGet(NativeRow) const;
    unsigned StandardIndex(NativeRow) const;
    void SetIndex(NativeRow, unsigned);    // writes working_ / the live copy, previews through the table
    const NativeHost* host_ = nullptr;
    Document document_;
    Config working_, boot_;
    StoreResult loaded_{StoreStatus::Missing, ""};
    std::vector<std::string> languages_;
    controls::ControllerCaps caps_;
    bool haveCaps_ = false;
    controls::ProfileId detected_ = controls::ProfileId::Handheld;
    NativeWhere where_ = NativeWhere::Entry;
    bool starRace_ = false;
    int live_[kNativeRows], liveBase_[kNativeRows];   // the value index of the rows read through the table (now, and when the menu opened / last saved)
    bool previewed_[kNativeRows] = {};                // a Config-backed live row was sent to the table since the last save / revert
    bool appliedLive_[kNativeRows] = {};              // ... and the table applied it
    bool bindingsPending_ = false;                    // saved buttons the table could not swap in
    bool queued_ = false, hold_ = false, holdRun_ = false;   // hold_: the marker holds (cleared by a save); holdRun_: this run started in a hold
    Config baseline_;                                 // what Dirty compares with: the loaded Config (in a hold: with the running Picture and Language)
};

}  // namespace port::settings

// The flag as a C function (the model's own use and tests).
extern "C" bool port_settings_native_menu_open(void);
