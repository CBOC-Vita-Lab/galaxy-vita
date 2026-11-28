// Native in-game Settings: the model (see port/settings_native.h). No game headers, no English text.
#include "port/settings_native.h"
#include "port/tilt_gain.h"
#include "port/mode_keys.h"   // (setnat31: env.txt's GXV_HRW from the snapshot, NativeRetire)
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

extern "C" {   // perf.cpp (weak: a host test links none of them)
int port_handheld(void) __attribute__((weak));
int port_zoom_tenths(void) __attribute__((weak));
int port_zoom_adjust(int) __attribute__((weak));
int port_rear_aim_enabled(void) __attribute__((weak));
int port_rear_aim_toggle(void) __attribute__((weak));
int port_stick_pointer_enabled(void) __attribute__((weak));
int port_stick_pointer_toggle(void) __attribute__((weak));
int port_right_stick_mode(void) __attribute__((weak));
int port_right_stick_mode_set(int) __attribute__((weak));
int port_camera_speed(void) __attribute__((weak));
int port_camera_speed_set(int) __attribute__((weak));
int port_star_bit_reach(void) __attribute__((weak));
int port_star_bit_reach_next(void) __attribute__((weak));
int port_rear_aim_set(int) __attribute__((weak));
int port_stick_pointer_set(int) __attribute__((weak));
int port_star_bit_reach_set(int level) __attribute__((weak));
int port_invert_camera_enabled(void) __attribute__((weak));
int port_invert_camera_set(int) __attribute__((weak));
int port_tap_jump_enabled(void) __attribute__((weak));
int port_tap_jump_set(int) __attribute__((weak));
int port_less_motion_enabled(void) __attribute__((weak));
int port_less_motion_set(int) __attribute__((weak));
int port_tilt_stick_enabled(void) __attribute__((weak));
int port_tilt_stick_set(int) __attribute__((weak));
int port_pointer_quick_enabled(void) __attribute__((weak));     // input.cpp only: absent on the desktop
int port_pointer_quick_set(int) __attribute__((weak));
int port_tilt_level(void) __attribute__((weak));
int port_tilt_level_set(int level) __attribute__((weak));
int port_vita_input_external_pad(void) __attribute__((weak));   // vita/runtime/input.cpp: PS TV DS3/DS4 external pad default
void port_language_set_fallback(const char*) __attribute__((weak));   // dvd.cpp
int port_card_write_async(const char* path, int mode, const void* data, unsigned bytes) __attribute__((weak));   // os_alarm.cpp
}

namespace port::controls {
bool StageActiveMapping(const ProfileMapping&, std::string& err) __attribute__((weak));   // control_bindings.cpp (agent D)
}

namespace port::settings {
namespace {
std::atomic<bool> sMenuOpen{false};
const NativeHost* sTestHost = nullptr;
const std::string kNone;
constexpr const char* kRowKeys[kNativeRows] = {"picture", "language", "less_motion", "tilt", "tilt_strength", "pointer", "camera_zoom", "right_stick", "invert_camera", "rear_touch", "tap_to_jump", "star_bit_reach", "assist60", "allow50", "camera_speed"};
constexpr const char* kLevelKeys[tilt::kLevels] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
constexpr const char* kZoomKeys[11] = {"1.0", "1.1", "1.2", "1.3", "1.4", "1.5", "1.6", "1.7", "1.8", "1.9", "2.0"};
unsigned Idx(NativeRow r) { return static_cast<unsigned>(r); }
unsigned ProfileIndex(controls::ProfileId p) { return static_cast<unsigned>(p); }
// The env.txt keys that force each row (up to 3).
struct Keys { const char* k[3]; };
Keys RowEnvKeys(NativeRow row) {
    switch (row) {
        case NativeRow::Picture: return {{"GXV_PICTURE_MODE", "GXV_FPS_LIMIT", "GXV_AUTOQ_TARGET"}};
        case NativeRow::Language: return {{"PETARI_LANGUAGE", nullptr, nullptr}};
        case NativeRow::LessMotion: return {{"GXV_REDUCED_MOTION", nullptr, nullptr}};
        case NativeRow::Tilt: return {{"GXV_TILT_STICK", nullptr, nullptr}};
        case NativeRow::TiltStrength: return {{"GXV_TILT_GAIN", nullptr, nullptr}};
        case NativeRow::Pointer: return {{"GXV_MOUSE_AIM", nullptr, nullptr}};
        case NativeRow::CameraZoom: return {{"PETARI_ZOOM_FORCE", nullptr, nullptr}};
        case NativeRow::RightStick: return {{"PETARI_RSTICK", "GXV_FREE_CAM", nullptr}};
        case NativeRow::CameraSpeed: return {{"GXV_FREE_CAM_SPEED", nullptr, nullptr}};
        case NativeRow::TapToJump: return {{"PETARI_TOUCH_A", nullptr, nullptr}};
        case NativeRow::StarBitReach: return {{"PETARI_STAR_BIT_REACH", nullptr, nullptr}};
        case NativeRow::Assist60: return {{"GXV_HRW", nullptr, nullptr}};
        case NativeRow::Allow50: return {{"GXV_HRW_ALLOW50", nullptr, nullptr}};
        default: return {{nullptr, nullptr, nullptr}};
    }
}
bool HostBacked(NativeRow r) {
    switch (r) {
        case NativeRow::CameraZoom: case NativeRow::RightStick: case NativeRow::CameraSpeed: case NativeRow::InvertCamera: case NativeRow::RearTouch: case NativeRow::TapToJump: case NativeRow::StarBitReach: return true;
        default: return false;
    }
}
bool ConfigLive(NativeRow r) { return r == NativeRow::LessMotion || r == NativeRow::Tilt || r == NativeRow::TiltStrength || r == NativeRow::Pointer; }
bool SameNameNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
unsigned ConfigIndex(const Config& c, NativeRow r) {
    switch (r) {
        case NativeRow::LessMotion: return c.reducedMotion ? 1 : 0;
        case NativeRow::Tilt: return c.tiltStick ? 1 : 0;
        case NativeRow::TiltStrength: return tilt::ValidLevel(c.tiltLevel) ? static_cast<unsigned>(c.tiltLevel - 1) : tilt::kDefaultLevel - 1;
        case NativeRow::Pointer: return c.mouseAim ? 1 : 0;
        default: return 0;
    }
}
// ---- the recovery marker ----
bool ReadMarker(const NativeHost& h, const std::string& path, std::string& text) {
    if (!h.files) return false;
    FileInfo info{};
    if (h.files->stat(h.files->context, path.c_str(), info) < 0 || !info.regular) return false;
    int64_t fd = h.files->openRead(h.files->context, path.c_str());
    if (fd < 0) return false;
    char buf[64] = {};
    int64_t n = h.files->read(h.files->context, fd, buf, sizeof buf - 1);
    h.files->close(h.files->context, fd);
    text.assign(buf, n > 0 ? static_cast<size_t>(n) : 0);
    return true;
}
void WriteMarker(const NativeHost& h, const std::string& text) {
    if (!h.files || !h.path) return;
    const std::string path = NativeMarkerPath(h.path);
    int64_t fd = h.files->openWrite(h.files->context, path.c_str());
    if (fd < 0) return;
    h.files->write(h.files->context, fd, text.data(), text.size());
    h.files->sync(h.files->context, fd);
    h.files->close(h.files->context, fd);
}
void RemoveMarker(const NativeHost& h) { if (h.files && h.path) h.files->remove(h.files->context, NativeMarkerPath(h.path).c_str()); }
bool sBootToast = false, sStandardRun = false, sEntryDone = false;
std::vector<std::string> sEnvSeen;

// ---- the forwarding live table ----
int Flag(int (*f)(void)) { return f ? (f() ? 1 : 0) : -1; }
int PortGet(void*, NativeRow row) {
    const bool handheld = port_handheld && port_handheld();
    switch (row) {
        case NativeRow::CameraZoom: return port_zoom_tenths ? std::clamp(port_zoom_tenths() - 10, 0, 10) : -1;
        case NativeRow::RightStick:
            if (!handheld) return -1;
            if (port_right_stick_mode) return std::clamp(port_right_stick_mode(), 0, 2);
            return port_stick_pointer_enabled ? (port_stick_pointer_enabled() ? 2 : 0) : -1;
        case NativeRow::CameraSpeed: return handheld && port_camera_speed ? std::clamp(port_camera_speed() - 1, 0, 9) : -1;
        case NativeRow::RearTouch: return handheld ? Flag(port_rear_aim_enabled) : -1;
        case NativeRow::StarBitReach: return handheld && port_star_bit_reach ? std::clamp(port_star_bit_reach() - 1, 0, 4) : -1;
        case NativeRow::InvertCamera: return port_invert_camera_enabled ? std::clamp(port_invert_camera_enabled(), 0, 3) : -1;
        case NativeRow::TapToJump: return Flag(port_tap_jump_enabled);
        case NativeRow::LessMotion: return Flag(port_less_motion_enabled);
        case NativeRow::Tilt: return Flag(port_tilt_stick_enabled);
        case NativeRow::Pointer: return Flag(port_pointer_quick_enabled);
        case NativeRow::TiltStrength: return port_tilt_level ? std::clamp(port_tilt_level() - 1, 0, 9) : -1;
        default: return -1;
    }
}
bool PortSet(void* c, NativeRow row, int index) {
    if (index < 0 || PortGet(c, row) < 0) return false;
    switch (row) {
        case NativeRow::CameraZoom: if (!port_zoom_adjust) return false; port_zoom_adjust(index - PortGet(c, row)); break;
        case NativeRow::RightStick:
            if (port_right_stick_mode_set) port_right_stick_mode_set(index);
            else if (port_stick_pointer_set) port_stick_pointer_set(index == 2 ? 1 : 0);
            else if (PortGet(c, row) != index && port_stick_pointer_toggle) port_stick_pointer_toggle();
            break;
        case NativeRow::CameraSpeed: if (!port_camera_speed_set) return false; port_camera_speed_set(index + 1); break;
        case NativeRow::RearTouch: if (port_rear_aim_set) port_rear_aim_set(index); else if (PortGet(c, row) != index && port_rear_aim_toggle) port_rear_aim_toggle(); break;
        case NativeRow::StarBitReach:
            if (port_star_bit_reach_set) port_star_bit_reach_set(index + 1);
            else for (int i = 0; i < 5 && PortGet(c, row) != index && port_star_bit_reach_next; ++i) port_star_bit_reach_next();
            break;
        case NativeRow::InvertCamera: if (!port_invert_camera_set) return false; port_invert_camera_set(index); break;
        case NativeRow::TapToJump: if (!port_tap_jump_set) return false; port_tap_jump_set(index); break;
        case NativeRow::LessMotion: if (!port_less_motion_set) return false; port_less_motion_set(index); break;
        case NativeRow::Tilt: if (!port_tilt_stick_set) return false; port_tilt_stick_set(index); break;
        case NativeRow::Pointer: if (!port_pointer_quick_set) return false; port_pointer_quick_set(index); break;
        case NativeRow::TiltStrength: if (!port_tilt_level_set) return false; port_tilt_level_set(index + 1); break;
        default: return false;
    }
    return PortGet(c, row) == index;
}
bool PortApplyBindings(void*, const Config& c, controls::ProfileId effective) {
    std::string err;
    return controls::StageActiveMapping && controls::StageActiveMapping(c.profiles[static_cast<unsigned>(effective)], err);
}
int PortStandard(void*, NativeRow row) {
    if (row == NativeRow::CameraZoom) return port_handheld && port_handheld() ? 5 : 0;   // 1.5 on a handheld, 1.0 elsewhere (perf.cpp)
    if (row == NativeRow::RightStick) return port_vita_input_external_pad && port_vita_input_external_pad() ? 2 : 0;   // PS TV defaults to Pointer, handheld to Free Camera
    return -1;
}
const NativeLiveOps kPortOps{nullptr, PortGet, PortSet, nullptr, PortApplyBindings, PortStandard};
}  // namespace

const NativeLiveOps* NativeLiveOpsPort() { return &kPortOps; }
const NativeHost* __attribute__((weak)) NativeHostPlatform() { return nullptr; }
const NativeHost* NativeHostGet() { return sTestHost ? sTestHost : NativeHostPlatform(); }
void NativeHostSetForTest(const NativeHost* host) { sTestHost = host; }
void NativeMenuOpenSet(bool open) { sMenuOpen.store(open, std::memory_order_release); }
bool NativeMenuOpen() { return sMenuOpen.load(std::memory_order_acquire); }

// ---- start-up ----
void NativeRetire(Config& c, const char* systemLanguage) {
    // env.txt's keys, recorded now: ApplyStartup sets some of these itself afterwards
    sEnvSeen.clear();
    for (unsigned r = 0; r < kNativeRows; ++r)
        for (const char* key : RowEnvKeys(static_cast<NativeRow>(r)).k) {
            const char* v = key ? std::getenv(key) : nullptr;
            // (setnat31) GXV_HRW: entry.cpp sets its own default "0" before this runs; the mode-key snapshot (taken before any default) says what env.txt set
            if (key && !std::strcmp(key, "GXV_HRW")) { if (port::modekeys::UserHas(key, [](const char* k) { return std::getenv(k); })) sEnvSeen.emplace_back(key); continue; }
            if (!v) continue;
            if (!std::strcmp(key, "GXV_AUTOQ_TARGET") && !std::strcmp(v, "30")) continue;   // (entry.cpp's own start-up default)
            sEnvSeen.emplace_back(key);
        }
    // The owner's rule (15 Nov): 2.3's performance is finely tuned, so the settings of the player's file that the menu no longer shows (game speed, sharpness,
    // HD textures, depth of field, core plan, renderer, logic overlap, material updates, far animation, the picture rows) stay as the file has them and the run
    // uses them. Safe Saves is always on in 3.0; Allow50 is retired from the UI (migrated off; GXV_HRW_ALLOW50 in env.txt remains as escape hatch).
    c.safeSaves = true;
    c.allow50 = false;
    if (systemLanguage && *systemLanguage && port_language_set_fallback) port_language_set_fallback(systemLanguage);
}
std::string NativeMarkerPath(const char* path) {
    std::string p = path ? path : "";
    if (p.size() > 4 && p.compare(p.size() - 4, 4, ".cfg") == 0) p.resize(p.size() - 4);
    return p + ".untested";
}
NativeBootResult NativeBootCheck(const NativeHost& host, Config& run) {
    NativeBootResult out;
    if (!host.path || !host.files) return out;
    std::string text;
    if (!ReadMarker(host, NativeMarkerPath(host.path), text)) return out;
    bool hold = text.compare(0, 4, "hold") == 0;
    if (!hold) {
        int attempts = 1;   // (an unreadable marker counts as one attempt)
        if (text.compare(0, 4, "try ") == 0) { const int n = std::atoi(text.c_str() + 4); attempts = n < 0 ? 0 : n > 99 ? 99 : n; }
        if (attempts >= 2) { hold = true; WriteMarker(host, "hold\n"); }
        else WriteMarker(host, "try " + std::to_string(attempts + 1) + "\n");
    }
    if (hold) {
        run.fpsLimit = 0; run.performance60 = true; run.assist60 = true; run.allow50 = false; run.language.clear();   // the standard Picture (60 Assist on: c181); no language wanted and no fallback registered: the disc's first
        out.standard = true; out.toast = true;
    }
    return out;
}
void NativeEntryReached(const NativeHost& host) {
    std::string text;
    if (host.path && ReadMarker(host, NativeMarkerPath(host.path), text) && text.compare(0, 4, "hold") != 0) RemoveMarker(host);
}
void NativeEntryReached() {
    if (sEntryDone) return;
    const NativeHost* host = NativeHostGet();
    if (!host) return;
    sEntryDone = true;
    NativeEntryReached(*host);
}
void NativeStartup(Config& run) {
    const NativeHost* host = NativeHostGet();
    if (!host || !host->available) return;
    const auto res = NativeBootCheck(*host, run);
    sStandardRun = res.standard;
    if (res.toast) sBootToast = true;
    // in a hold no fallback is registered: the disc's first language
    NativeRetire(run, !res.standard && host->systemLanguage ? host->systemLanguage() : nullptr);
}
bool NativeTakeBootToast() { const bool t = sBootToast; sBootToast = false; return t; }
bool NativeStandardRun() { return sStandardRun; }
void NativeBootStateClearForTest() { sBootToast = sStandardRun = sEntryDone = false; sEnvSeen.clear(); }
bool NativeEnvWasPresent(const char* key) { return key && std::find(sEnvSeen.begin(), sEnvSeen.end(), key) != sEnvSeen.end(); }

// ---- the model ----
StoreResult NativeModel::Open(const NativeHost& host, const std::vector<std::string>& languages) {
    host_ = &host;
    languages_ = languages;
    loaded_ = {StoreStatus::IoError, "Settings storage unavailable"};
    document_ = {DefaultConfig(controls::GetControlApi()), "", true};
    if (host.path && host.files) loaded_ = LoadDocument(host.path, controls::GetControlApi(), *host.files, document_);
    document_.onlyChanged = true;   // a save changes only the keys that changed (Document::onlyChanged)
    baseline_ = document_.config;
    hold_ = holdRun_ = NativeStandardRun();
    if (hold_) {   // a recovery start: the menu shows what is running (the standard Picture, the disc's first language)
        baseline_.fpsLimit = 0; baseline_.performance60 = true; baseline_.assist60 = true; baseline_.allow50 = false;
        baseline_.language = languages_.size() >= 2 ? languages_[0] : std::string();
    }
    working_ = baseline_;
    boot_ = host.active ? *host.active : document_.config;
    queued_ = false;
    for (unsigned i = 0; i < kNativeRows; ++i) {
        live_[i] = liveBase_[i] = HostBacked(static_cast<NativeRow>(i)) ? LiveGet(static_cast<NativeRow>(i)) : -1;
        previewed_[i] = appliedLive_[i] = false;
    }
    bindingsPending_ = false;
    return loaded_;
}
bool NativeModel::LoadDamaged() const {
    switch (loaded_.status) { case StoreStatus::Recovered: case StoreStatus::Malformed: case StoreStatus::IoError: case StoreStatus::TooLarge: return true; default: return false; }
}
void NativeModel::SetController(const controls::ControllerCaps& caps, controls::ProfileId detected) { caps_ = caps; haveCaps_ = true; detected_ = detected; }
void NativeModel::SetWhere(NativeWhere w, bool starRace) { where_ = w; starRace_ = starRace; }
bool NativeModel::ExternalPad() const { return host_ && host_->externalPad; }
controls::ProfileId NativeModel::Profile() const {
    Config t = working_;
    ApplyPadDefaults(t, ExternalPad());
    return t.profile;
}
controls::ControllerCaps NativeModel::EditCaps() const {
    const auto profile = Profile();
    auto caps = controls::NominalCaps(profile);
    if (haveCaps_ && caps_.connected && detected_ == profile) { caps = caps_; caps.availableButtons &= controls::NominalCaps(profile).availableButtons; }
    return caps;
}
int NativeModel::LiveGet(NativeRow row) const { return host_ && host_->live && host_->live->get ? host_->live->get(host_->live->context, row) : -1; }

const char* NativeModel::RowKey(NativeRow row) { return Idx(row) < kNativeRows ? kRowKeys[Idx(row)] : ""; }
NativeRowInfo NativeModel::RowInfo(NativeRow row) {
    const bool restart = row == NativeRow::Picture || row == NativeRow::Language || row == NativeRow::Assist60 || row == NativeRow::Allow50;   // (60 Assist, mp30 Allow 50: read once at start-up, live switching not verified)
    return {!restart, restart, true, true, true};   // every row is shown everywhere; in a session the non-personal ones are greyed (Blocked)
}
bool NativeModel::IsLive(NativeRow row) const { return HostBacked(row); }
bool NativeModel::RowShown(NativeRow row) const {
    switch (row) {
        case NativeRow::Language: return languages_.size() >= 2;
        case NativeRow::Tilt: return host_ && host_->motion;
        case NativeRow::CameraSpeed: return live_[Idx(row)] >= 0 && live_[Idx(NativeRow::RightStick)] == 0;
        case NativeRow::InvertCamera: return live_[Idx(row)] >= 0 && (live_[Idx(NativeRow::RightStick)] == 0 || live_[Idx(NativeRow::RightStick)] == 1);
        case NativeRow::RearTouch: return live_[Idx(row)] >= 0 && !ExternalPad();
        case NativeRow::Assist60: return !Mode30(working_);   // (means something only in Smooth Motion)
        case NativeRow::Allow50: return false;                // (retired from UI: clean 2-mode Picture UX; GXV_HRW_ALLOW50 env.txt stays intact)
        default: return HostBacked(row) ? live_[Idx(row)] >= 0 : true;
    }
}
unsigned NativeModel::ValueCount(NativeRow row) const {
    switch (row) {
        case NativeRow::Language: return 1 + static_cast<unsigned>(languages_.size());
        case NativeRow::TiltStrength: case NativeRow::CameraSpeed: return tilt::kLevels;
        case NativeRow::CameraZoom: return 11;
        case NativeRow::RightStick: return 3;
        case NativeRow::InvertCamera: return live_[Idx(NativeRow::RightStick)] == 1 ? 2 : 4;   // Classic mode only has Left/Right (Off / Left-Right)
        case NativeRow::StarBitReach: return 5;
        default: return 2;
    }
}
unsigned NativeModel::ValueIndex(NativeRow row) const {
    switch (row) {
        case NativeRow::Picture: return Mode30(working_) ? 1 : 0;
        case NativeRow::Assist60: return working_.assist60 ? 1 : 0;
        case NativeRow::Allow50: return working_.allow50 ? 1 : 0;
        case NativeRow::Language: {
            auto at = std::find_if(languages_.begin(), languages_.end(), [&](const std::string& n) { return SameNameNoCase(n, working_.language); });
            return working_.language.empty() || at == languages_.end() ? 0 : static_cast<unsigned>(at - languages_.begin()) + 1;
        }
        case NativeRow::InvertCamera: {
            const int v = std::max(0, live_[Idx(row)]);
            return static_cast<unsigned>(live_[Idx(NativeRow::RightStick)] == 1 ? (v & 1) : (v & 3));
        }
        default: return HostBacked(row) ? static_cast<unsigned>(std::max(0, live_[Idx(row)])) : ConfigIndex(working_, row);
    }
}
const char* NativeModel::ValueKey(NativeRow row, unsigned i) const {
    if (i >= ValueCount(row)) return "";
    switch (row) {
        case NativeRow::Picture: return i ? "detailed" : "smooth";
        case NativeRow::Language: return i ? "name" : "system";
        case NativeRow::LessMotion: case NativeRow::TapToJump: case NativeRow::RearTouch: case NativeRow::Assist60: case NativeRow::Allow50: return i ? "on" : "off";
        case NativeRow::InvertCamera: { static constexpr const char* kInv[4] = {"off", "x", "y", "both"}; return kInv[i]; }
        case NativeRow::Tilt: return i ? "stick" : "console";
        case NativeRow::TiltStrength: case NativeRow::CameraSpeed: return kLevelKeys[i];
        case NativeRow::Pointer: return i ? "quick" : "steady";
        case NativeRow::RightStick: { static constexpr const char* kRs[3] = {"free", "camera", "pointer"}; return kRs[i]; }
        case NativeRow::CameraZoom: return kZoomKeys[i];
        case NativeRow::StarBitReach: return kLevelKeys[i];
        default: return "";
    }
}
const std::string& NativeModel::LanguageName(unsigned i) const { return i >= 1 && i <= languages_.size() ? languages_[i - 1] : kNone; }
std::string NativeModel::SystemLanguageName() const {
    const char* sys = host_ && host_->systemLanguage ? host_->systemLanguage() : nullptr;
    if (!sys || !*sys) return "";
    for (const auto& n : languages_) if (SameNameNoCase(n, sys)) return n;
    return "";
}
std::string NativeModel::EffectiveLanguage(const Config& c) const {
    if (c.language.empty()) return SystemLanguageName();
    for (const auto& n : languages_) if (SameNameNoCase(n, c.language)) return n;
    return languages_.empty() ? c.language : std::string();   // not on the disc: the disc's first
}
bool NativeModel::IsOverridden(NativeRow row) const {
    if (!host_) return false;
    for (const char* key : RowEnvKeys(row).k) {
        if (!key) continue;
        if (host_->envPresent && host_->envPresent(key)) return true;
        if (host_->overrides) for (const auto& o : *host_->overrides) if (o.key == key) return true;
    }
    return false;
}
RowBlock NativeModel::Blocked(NativeRow row) const {
    if (Idx(row) >= kNativeRows) return RowBlock::Unavailable;
    if (!Editable()) return RowBlock::ReadOnly;
    if (where_ == NativeWhere::Session) {
        if (row == NativeRow::Picture || row == NativeRow::Language || row == NativeRow::Assist60) return RowBlock::Together;
        if (row == NativeRow::StarBitReach && starRace_) return RowBlock::StarRace;
    }
    if (IsOverridden(row)) return RowBlock::Overridden;
    if (!host_) return RowBlock::Unavailable;
    if (!RowShown(row)) return RowBlock::Unavailable;
    if (row == NativeRow::Picture && !host_->fixed30 && !Mode30(working_)) return RowBlock::Unavailable;
    return RowBlock::None;
}
unsigned NativeModel::StandardIndex(NativeRow row) const {
    if (HostBacked(row) && host_ && host_->live && host_->live->standard) {
        const int s = host_->live->standard(host_->live->context, row);
        if (s >= 0) return static_cast<unsigned>(s);
    }
    switch (row) {
        case NativeRow::TiltStrength: case NativeRow::CameraSpeed: return tilt::kDefaultLevel - 1;
        case NativeRow::CameraZoom: return 5;
        case NativeRow::RightStick: return ExternalPad() ? 2 : 0;   // PS TV defaults to Pointer, handheld to Free Camera
        case NativeRow::TapToJump: return 1;
        case NativeRow::Assist60: return 1;   // (on: c181's Performance)
        default: return 0;
    }
}
void NativeModel::SetIndex(NativeRow row, unsigned i) {
    switch (row) {
        case NativeRow::Picture: working_.fpsLimit = i ? 30 : 0; working_.performance60 = !i; return;
        case NativeRow::Language: working_.language = i ? languages_[i - 1] : std::string(); return;
        case NativeRow::Assist60: working_.assist60 = i != 0; return;
        case NativeRow::Allow50: working_.allow50 = i != 0; return;
        case NativeRow::LessMotion: working_.reducedMotion = i != 0; break;
        case NativeRow::Tilt: working_.tiltStick = i != 0; break;
        case NativeRow::TiltStrength: working_.tiltLevel = static_cast<int>(i) + 1; break;
        case NativeRow::Pointer: working_.mouseAim = i != 0; break;
        default: return;
    }
    previewed_[Idx(row)] = true;   // a Config-backed live row: sent to its setter now
    appliedLive_[Idx(row)] = host_ && host_->live && host_->live->set && host_->live->set(host_->live->context, row, static_cast<int>(i));
}
StepResult NativeModel::Step(NativeRow row, int direction) {
    switch (Blocked(row)) {
        case RowBlock::None: break;
        case RowBlock::ReadOnly: return StepResult::ReadOnly;
        case RowBlock::Overridden: return StepResult::Overridden;
        case RowBlock::Together: case RowBlock::StarRace: return StepResult::NotNow;
        default: return StepResult::Unavailable;
    }
    const int dir = direction < 0 ? -1 : 1;
    const int n = static_cast<int>(ValueCount(row)), now = static_cast<int>(ValueIndex(row));
    int next;
    switch (row) {
        case NativeRow::Picture:
            // restates settings_ui.cpp Change(), RowRate: without the fixed 30 only Smooth exists (a saved Full Detail steps back to it)
            next = !host_->fixed30 ? 0 : 1 - now;
            break;
        case NativeRow::Language: case NativeRow::RightStick: next = (now + dir + n) % n; break;
        case NativeRow::InvertCamera:
            if (live_[Idx(NativeRow::RightStick)] == 1) {
                // Classic mode: toggle bit 0 (Left/Right) while preserving Free Camera's Up/Down bit 1.
                next = (std::max(0, live_[Idx(row)]) ^ 1) & 3;
            } else {
                next = (now + dir + n) % n;
            }
            break;
        case NativeRow::TiltStrength: case NativeRow::CameraSpeed: case NativeRow::CameraZoom: case NativeRow::StarBitReach:
            next = now + dir;
            if (next < 0 || next >= n) return StepResult::AtEnd;
            break;
        default: next = 1 - now; break;
    }
    if (HostBacked(row)) {
        if (!host_->live || !host_->live->set || !host_->live->set(host_->live->context, row, next)) return StepResult::Unavailable;
        live_[Idx(row)] = next;
        if (row == NativeRow::RightStick) {
            live_[Idx(NativeRow::InvertCamera)] = LiveGet(NativeRow::InvertCamera);
            live_[Idx(NativeRow::CameraSpeed)] = LiveGet(NativeRow::CameraSpeed);
        }
    } else SetIndex(row, static_cast<unsigned>(next));
    return StepResult::Changed;
}
StepResult NativeModel::ResetRow(NativeRow row) {
    const unsigned std_ = StandardIndex(row);
    switch (Blocked(row)) {
        case RowBlock::None: break;
        case RowBlock::ReadOnly: return StepResult::ReadOnly;
        case RowBlock::Overridden: return StepResult::Overridden;
        case RowBlock::Together: case RowBlock::StarRace: return StepResult::NotNow;
        default: return StepResult::Unavailable;
    }
    const bool alreadyStd = (row == NativeRow::InvertCamera)
        ? (std::max(0, live_[Idx(row)]) == static_cast<int>(std_))
        : (ValueIndex(row) == std_);
    if (alreadyStd) return StepResult::AtEnd;   // already standard
    if (HostBacked(row)) {
        if (!host_->live || !host_->live->set || !host_->live->set(host_->live->context, row, static_cast<int>(std_))) return StepResult::Unavailable;
        live_[Idx(row)] = static_cast<int>(std_);
        if (row == NativeRow::RightStick) {
            live_[Idx(NativeRow::InvertCamera)] = LiveGet(NativeRow::InvertCamera);
            live_[Idx(NativeRow::CameraSpeed)] = LiveGet(NativeRow::CameraSpeed);
        }
    } else SetIndex(row, std_);
    return StepResult::Changed;
}
void NativeModel::PutEverythingBack() {
    auto resetOne = [&](NativeRow row) {
        const bool differs = (row == NativeRow::InvertCamera)
            ? (std::max(0, live_[Idx(row)]) != static_cast<int>(StandardIndex(row)))
            : (ValueIndex(row) != StandardIndex(row));
        if (Blocked(row) == RowBlock::None && differs) ResetRow(row);
    };
    // Pre-reset CameraSpeed & InvertCamera before RightStick (handles PS TV where RightStick resets
    // from Free/Classic to Pointer and hides them), then reset all rows in enum order (handles
    // handheld where RightStick resets from Pointer to Free Camera and unhides them).
    resetOne(NativeRow::CameraSpeed);
    resetOne(NativeRow::InvertCamera);
    for (unsigned r = 0; r < kNativeRows; ++r) resetOne(static_cast<NativeRow>(r));
    if (Editable() && where_ != NativeWhere::Session && !BindingsOverridden()) Defaults();   // (Defaults is a no-op in a session or a read-only file)
}

bool NativeModel::Dirty() const {
    if (!Equal(working_, baseline_)) return true;
    for (unsigned i = 0; i < kNativeRows; ++i) if (HostBacked(static_cast<NativeRow>(i)) && live_[i] != liveBase_[i]) return true;
    return false;
}
namespace {
// What 60 Assist does in a Config: on only in Smooth Motion (in Full Detail the row means nothing, so a change of it alone needs no restart).
bool AssistRuns(const Config& c) { return !Mode30(c) && c.assist60; }
}  // namespace
bool NativeModel::PictureOrLanguageChanged() const { return Mode30(working_) != Mode30(baseline_) || working_.language != baseline_.language || AssistRuns(working_) != AssistRuns(baseline_); }
// Picture, Language and (setnat31) 60 Assist only: the restart rows, each compared field by field. What is running: the boot Config's Picture and 60 Assist,
// and its language (the console's, or in a hold the disc's first). Retired Allow50 is excluded so legacy allow50=1 files never cause spurious RestartNeeded().
bool NativeModel::RestartNeeded() const {
    const std::string running = holdRun_ ? (languages_.empty() ? std::string() : languages_[0]) : EffectiveLanguage(boot_);
    return Mode30(document_.config) != Mode30(boot_) || EffectiveLanguage(document_.config) != running || AssistRuns(document_.config) != AssistRuns(boot_);
}
bool NativeModel::WouldNeedRestart() const {
    const std::string running = holdRun_ ? (languages_.empty() ? std::string() : languages_[0]) : EffectiveLanguage(boot_);
    return Mode30(working_) != Mode30(boot_) || EffectiveLanguage(working_) != running || AssistRuns(working_) != AssistRuns(boot_);
}
bool NativeModel::NextStartOnly() const {
    for (unsigned r = 0; r < kNativeRows; ++r) {
        const auto row = static_cast<NativeRow>(r);
        if (ConfigLive(row) && ConfigIndex(working_, row) != ConfigIndex(boot_, row) && !appliedLive_[r]) return true;
    }
    return bindingsPending_ || (BindingsChanged() && !(host_ && host_->live && host_->live->applyBindings));
}
StoreResult NativeModel::Save() {
    if (!host_ || !host_->path || !host_->files) return {StoreStatus::IoError, "Settings storage unavailable"};
    if (!document_.editable) return {StoreStatus::UnsupportedVersion, "Newer settings file is read-only"};
    const auto& api = controls::GetControlApi();
    std::string error;
    for (unsigned i = 0; i < 2; ++i)
        if (!api.validate(working_.profiles[i], static_cast<controls::ProfileId>(i), controls::NominalCaps(static_cast<controls::ProfileId>(i)), error)) return {StoreStatus::Malformed, error};
    if (haveCaps_ && caps_.connected && detected_ == Profile() && !api.validate(working_.profiles[ProfileIndex(Profile())], Profile(), EditCaps(), error))
        return {StoreStatus::Malformed, error};
    const Config prev = document_.config;
    const bool bindingsMoved = BindingsChanged();
    StoreResult r{StoreStatus::IoError, ""};
    queued_ = false;
    if (where_ == NativeWhere::Session && port_card_write_async) {   // no card call on the game thread: serialize, hand the bytes to the card-write queue
        std::string text; Document parsed;
        if (SerializeDocument(document_, working_, api, text).status == StoreStatus::Ok && ParseDocument(text, api, parsed).status == StoreStatus::Ok &&
            port_card_write_async(host_->path, 0, text.data(), static_cast<unsigned>(text.size()))) {
            parsed.source = text; document_ = std::move(parsed); queued_ = true; r = {StoreStatus::Ok, "Saved"};
        }
    }
    if (!queued_) {
        Document saved;
        r = SaveDocument(host_->path, document_, working_, api, *host_->files, saved);
        if (r.status != StoreStatus::Ok) return r;
        document_ = std::move(saved);
    }
    document_.onlyChanged = true;
    baseline_ = document_.config;
    // the recovery marker (see NativeBootCheck): a save of the standard pair needs no guard; a changed pair (or a pair picked again after a hold) is untested until the entry screen shows
    std::string markerText; ReadMarker(*host_, NativeMarkerPath(host_->path), markerText);
    const bool wasHold = markerText.compare(0, 4, "hold") == 0;
    const bool standardPair = !Mode30(document_.config) && document_.config.assist60 && (document_.config.language.empty() || (hold_ && !languages_.empty() && document_.config.language == languages_[0]));
    const bool changed = Mode30(prev) != Mode30(document_.config) || prev.language != document_.config.language || AssistRuns(prev) != AssistRuns(document_.config);
    if (standardPair) RemoveMarker(*host_);
    else if (changed || wasHold) WriteMarker(*host_, "try 0\n");
    hold_ = false;
    for (unsigned i = 0; i < kNativeRows; ++i) { liveBase_[i] = live_[i]; previewed_[i] = false; }
    if (host_->live && host_->live->commit) host_->live->commit(host_->live->context);
    if (bindingsMoved) {
        const bool applied = host_->live && host_->live->applyBindings && host_->live->applyBindings(host_->live->context, document_.config, Profile());
        const auto p = ProfileIndex(Profile());
        bindingsPending_ = !applied && !controls::SameBindings(document_.config.profiles[p], boot_.profiles[p]);
    }
    return r;
}
void NativeModel::Revert() {
    working_ = baseline_;
    for (unsigned i = 0; i < kNativeRows; ++i) {
        const auto row = static_cast<NativeRow>(i);
        if (HostBacked(row)) {
            if (live_[i] != liveBase_[i]) { if (host_ && host_->live && host_->live->set) host_->live->set(host_->live->context, row, liveBase_[i]); live_[i] = liveBase_[i]; }
        } else if (previewed_[i]) {
            appliedLive_[i] = host_ && host_->live && host_->live->set && host_->live->set(host_->live->context, row, static_cast<int>(ConfigIndex(working_, row)));
            previewed_[i] = false;
        }
    }
}
bool NativeModel::RelaunchReady() const { return !host_ || !host_->relaunchReady || host_->relaunchReady(); }
unsigned NativeModel::SaveProgress() const { return host_ && host_->saveProgress ? host_->saveProgress() : 0; }
unsigned NativeModel::SaveFailures() const { return host_ && host_->saveFailures ? host_->saveFailures() : 0; }
bool NativeModel::RelaunchSafe() const { return host_ && host_->relaunchSafe && host_->relaunchSafe(); }
bool NativeModel::Relaunch() const { return host_ && host_->relaunch && host_->relaunch(); }

std::vector<MoveInfo> NativeModel::Moves() const {
    std::vector<MoveInfo> out;
    const auto& api = controls::GetControlApi();
    for (size_t i = 0; i < api.actionCount; ++i) out.push_back({api.actions[i].id, api.actions[i].maxBindings, api.actions[i].required});
    return out;
}
std::string NativeModel::ButtonOf(const std::string& move, unsigned slot) const {
    if (slot > 1) return "";
    for (const auto& b : working_.profiles[ProfileIndex(Profile())].bindings) if (b.action == move) return b.buttons[slot];
    return "";
}
std::vector<ButtonChoice> NativeModel::Buttons() const {
    std::vector<ButtonChoice> out;
    const auto& api = controls::GetControlApi();
    const auto caps = EditCaps();
    for (size_t i = 0; i < api.buttonCount; ++i)
        if (caps.availableButtons & api.buttons[i].mask) out.push_back({api.buttons[i].id, api.buttons[i].label, api.buttons[i].mask});
    return out;
}
bool NativeModel::BindingsEnabled() const { return Editable() && where_ != NativeWhere::Session && RemapAvailable(); }
BindOutcome NativeModel::Bind(const std::string& move, unsigned slot, const std::string& button, bool swap) {
    BindOutcome out;
    if (!Editable() || where_ == NativeWhere::Session) { out.kind = BindOutcome::Unavailable; return out; }
    const auto& api = controls::GetControlApi();
    const auto p = ProfileIndex(Profile());
    auto proposed = working_.profiles[p];
    std::string conflict;
    switch (api.bind(proposed, move.c_str(), slot, button.c_str(), swap, EditCaps(), conflict)) {
        case controls::BindResult::Applied: working_.profiles[p] = std::move(proposed); out.kind = BindOutcome::Ok; break;
        case controls::BindResult::Conflict: out.kind = BindOutcome::Conflict; out.conflictMove = conflict; break;
        case controls::BindResult::RequiredActionLost: out.kind = BindOutcome::RequiredActionLost; break;
        default: out.kind = BindOutcome::Unavailable; break;
    }
    return out;
}
void NativeModel::Defaults() {
    if (!Editable() || where_ == NativeWhere::Session) return;
    working_.profiles[ProfileIndex(Profile())] = controls::GetControlApi().defaults(Profile());
}
bool NativeModel::RemapAvailable() const { return controls::RemapEnabled(); }
bool NativeModel::BindingsOverridden() const {
    if (!host_) return false;
    if (!host_->overrides) return false;
    for (const auto& o : *host_->overrides) if (o.key == "GXV_REMAP" || o.key == "Controller") return true;
    return false;
}
bool NativeModel::BindingsChanged() const { return !controls::SameBindings(working_.profiles[0], document_.config.profiles[0]) || !controls::SameBindings(working_.profiles[1], document_.config.profiles[1]); }
}  // namespace port::settings

extern "C" bool port_settings_native_menu_open(void) { return port::settings::NativeMenuOpen(); }
