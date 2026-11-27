// Galaxy 3.0: the menus, the native in-game Settings (settingsnative, agent C; plan v3, 15 Nov: share/settings-native-plan-v3-1115.md).
//
// A menu family on the multiplayer framework. It starts nothing of multiplayer (no network, no session, no radio): it edits a copy of the
// player's settings through the pure model (port/settings_native.h: NativeModel) and writes them back with Save. Three doors (origins):
//   SO_ENTRY    the entry screen (the game is not running): everything, and "Save and Start Again" for Picture and Language;
//   SO_PAUSE    the pause menu of a single-player level: everything; Picture and Language start the next time (never a restart in a level);
//   SO_SESSION  the session's menu: the personal set only (Camera, Controls, Less Motion, Star Bit Reach unless a Star Race); the rest is grey.
// Screens: S_SET_MAIN (Picture, Camera, Controls, More), S_SET_CAMERA, S_SET_CONTROLS, S_SET_MORE (list rows: "Name   Value" with arrows),
// S_SET_BUTTONS (a row per move), S_SET_CAPTURE ("Press the button you want"), S_SET_PICK (the buttons, the fallback), S_SET_LEAVE.
// Every row shows its value and the blue band shows its own plain sentence, which follows the CURRENT value. A row that cannot change now
// is dim; its sentence says why. Pure code, as the other families.
#include "MpMenuInternal.hpp"

#include <string>
#include <vector>

#include "port/settings_native.h"

extern "C" int port_language_count(void) __attribute__((weak));        // port/port.h (a host test may define them)
extern "C" const char* port_language_name(int index) __attribute__((weak));
extern "C" void port_zoom_save_now(void) __attribute__((weak));        // perf.cpp: a Camera Zoom change is written now (it otherwise waits for idle turns)
extern "C" void port_zoom_menu(int selecting) __attribute__((weak));   // perf.cpp: settings.ini is only written from here (every frame at the entry screen and in a session)

namespace MpMenu {
namespace {

using port::settings::BindOutcome;
using port::settings::NativeHost;
using port::settings::NativeModel;
using port::settings::NativeRow;
using port::settings::NativeWhere;
using port::settings::RowBlock;
using port::settings::StepResult;

struct MoveRow { std::string id; unsigned slot; };

// What a list row is.
enum Kind : uint8_t { R_VALUE, R_CAMERA, R_CONTROLS, R_MORE, R_BUTTONS, R_RESET_ALL, R_REPORT };

// betadiag: "Send a Problem Report" calls the report module's opener through this pointer (no-op, and the row hidden, while none is set).
void (*sReportHook)(const char* where) = nullptr;
struct Spec { Kind kind; NativeRow row; };

// How the capture goes (Fable C): wait until nothing is held, then the first frame with exactly one new button is the press, then wait for release.
enum CapPhase : uint8_t { CAP_RELEASE, CAP_ARMED, CAP_DONE };
constexpr int kCaptureFrames = 300;
// fix31 / setfix31b: "Save and Start Again" never starts the game again while a save is being written (LoadExec would cut the write). It waits for
// the card to be quiet with NO time limit while the save layer makes progress, behind a small window ("Finishing saving... the game starts again as
// soon as it's done.", Circle / Later puts the restart off: the settings stay saved, the change comes at the next start). Only when the save layer
// reports an error or makes no progress for kStallFrames (30 s at 60 a second) does the window offer "Start Again Now" (only when that cannot hurt
// the game save: Safe Saves, NativeHost::relaunchSafe) or "Later". (fix31 had a 600-frame limit and then a dead end: "close the game".)
constexpr int kStallFrames = 30 * 60;

// The one open Settings (the family's own state; the Sequence only knows "the Settings is open").
struct Set {
    NativeModel model;
    int origin = 0;
    std::vector<MoveRow> moves;                      // the button list's rows (a two-slot move has two)
    std::vector<port::settings::ButtonChoice> buttons;   // S_SET_PICK: the buttons the move may take (the second slot starts with "None")
    std::string pendMove, pendButton;                // a pick that met a conflict (the swap question)
    unsigned pendSlot = 0;
    bool discard = false;                            // Don't Save: closing saves nothing
    bool restartPath = false;                        // the leave question's Save is "Save and Start Again"
    bool asked = false;                              // the leave question was asked (and answered Keep Editing): the next Back leaves, saving
    int restarting = 0;                              // frames since "Save and Start Again" (0: not restarting)
    bool waitShown = false;                          // setfix31b: the "Finishing saving..." window is up (S_SET_RESTART)
    unsigned lastProgress = 0, failuresAtStart = 0;  // the save layer's counters when the wait began / last moved
    int stallFrames = 0;                             // frames with no progress
    bool timedOut = false;                           // the capture ran out: the list says so
    CapPhase cap = CAP_RELEASE;
    int capFrames = 0, capRow = 0;
    uint64_t capPrev = 0;
    bool savedToastDue = false;
    bool restartConfirmed = false;                   // setapply31: from pause, the player said Yes to "Start the game again now?"
};
Set sSet;

// setapply31 (the owner's report: "saved a setting, it wasn't applying it ... had to save and then manually quit"): a change that only takes effect
// at a start (Picture, 60 Assist, Language: read once at start-up; WouldNeedRestart) is never left waiting for a manual quit. The leave question's
// Save is "Save and Start Again" from the entry screen AND from the pause menu (there after a Yes / No: the level is left); the game then starts
// itself again (host relaunch: sceAppMgrLoadExec of our own eboot on the Vita). Never in a multiplayer session (those rows are grey there).
bool RestartOnSave(int origin, const NativeModel& m) { return origin != Sequence::SO_SESSION && m.WouldNeedRestart(); }
// setfix31b: the wait for a quiet card begins (the settings are saved already)
void BeginRestart() {
    sSet.restarting = 1;
    sSet.waitShown = false;
    sSet.lastProgress = sSet.model.SaveProgress();
    sSet.failuresAtStart = sSet.model.SaveFailures();
    sSet.stallFrames = 0;
}

constexpr int kMaxLanguages = 32;

// The leave screen's pills: I_SET0 + k.
enum : int { K_SAVE, K_DONT, K_KEEP, K_SESSION_PILL /* I_SESSION_SETTINGS: the session family's */, K_NOW, K_LATER };   // (K_NOW / K_LATER: S_SET_RESTART)
constexpr int Pill(int k) { return I_SET0 + k; }
bool IsSet(Scr s) { return s >= S_SET_MAIN && s <= S_SET_RESTART; }
bool IsListScreen(Scr s) { return s == S_SET_MAIN || s == S_SET_CAMERA || s == S_SET_CONTROLS || s == S_SET_MORE; }

// The rows of a list screen, in order, without the ones this console has no use for.
int SpecsOf(Scr s, Spec* out) {
    const NativeModel& m = sSet.model;
    int n = 0;
    auto value = [&](NativeRow r) { if (m.RowShown(r)) out[n++] = {R_VALUE, r}; };
    auto nav = [&](Kind k) { out[n++] = {k, NativeRow::Picture}; };
    switch (s) {
    case S_SET_MAIN:
        value(NativeRow::Picture), value(NativeRow::Assist60), nav(R_CAMERA), nav(R_CONTROLS), nav(R_MORE);   // (60 Assist: only while Picture is Smooth Motion)
        break;
    case S_SET_CAMERA:
        value(NativeRow::CameraZoom), value(NativeRow::RightStick), value(NativeRow::InvertCamera);
        break;
    case S_SET_CONTROLS:
        value(NativeRow::Tilt), value(NativeRow::TiltStrength), value(NativeRow::Pointer), value(NativeRow::RearTouch), value(NativeRow::TapToJump);
        nav(R_BUTTONS);
        break;
    case S_SET_MORE:
        value(NativeRow::Language), value(NativeRow::LessMotion), value(NativeRow::StarBitReach);   // (Allow50 retired: clean 2-mode Picture UX)
        if (sReportHook) out[n++] = {R_REPORT, NativeRow::Picture};   // (an action row, last but one)
        nav(R_RESET_ALL);
        break;
    default: break;
    }
    return n;
}
bool SpecAt(Scr s, int item, Spec* out) {
    if (item < I_LIST0 || !IsListScreen(s)) return false;
    Spec all[8];
    const int n = SpecsOf(s, all);
    const int k = item - I_LIST0;
    if (k < 0 || k >= n) return false;
    *out = all[k];
    return true;
}

MpText LabelOf(NativeRow r) {
    switch (r) {
    case NativeRow::Picture: return MP_T_SET_L_PICTURE;
    case NativeRow::Language: return MP_T_SET_L_LANGUAGE;
    case NativeRow::LessMotion: return MP_T_SET_L_MOTION;
    case NativeRow::Tilt: return MP_T_SET_L_TILT;
    case NativeRow::TiltStrength: return MP_T_SET_L_TILT_POWER;
    case NativeRow::Pointer: return MP_T_SET_L_POINTER;
    case NativeRow::CameraZoom: return MP_T_SET_L_ZOOM;
    case NativeRow::RightStick: return MP_T_SET_L_RSTICK;
    case NativeRow::InvertCamera: return MP_T_SET_L_INVERT;
    case NativeRow::RearTouch: return MP_T_SET_L_REAR;
    case NativeRow::TapToJump: return MP_T_SET_L_TAPJUMP;
    case NativeRow::Assist60: return MP_T_SET_L_ASSIST;
    case NativeRow::Allow50: return MP_T_SET_L_ALLOW50;
    default: return MP_T_SET_L_REACH;
    }
}

// The disc's language names as the Language row shows them: in their own language (one text each); a name this build does not know is
// shown as the disc has it.
bool LanguageText(const std::string& name, char16_t* out, int cap) {
    std::string n;
    for (char c : name) n += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    MpText id = MP_T_NONE;
    if (n == "english") id = MP_T_SET_LANG_ENGLISH;
    else if (n == "french" || n == "français" || n == "francais") id = MP_T_SET_LANG_FRENCH;
    else if (n == "spanish" || n == "español" || n == "espanol") id = MP_T_SET_LANG_SPANISH;
    else if (n == "german" || n == "deutsch") id = MP_T_SET_LANG_GERMAN;
    else if (n == "italian" || n == "italiano") id = MP_T_SET_LANG_ITALIAN;
    if (id != MP_T_NONE) {
        Text(out, cap, id);
        return true;
    }
    txt::utf8To16(out, cap, name.c_str());
    return false;
}

// A number with one decimal ("1.5") or without ("7").
void Number(char16_t* out, int cap, int tenths, bool decimal) {
    int n = 0;
    auto put = [&](char16_t c) { if (n < cap - 1) out[n++] = c; };
    if (decimal) {
        if (tenths >= 100) put(static_cast<char16_t>(u'0' + tenths / 100));
        put(static_cast<char16_t>(u'0' + (tenths / 10) % 10)), put(u'.'), put(static_cast<char16_t>(u'0' + tenths % 10));
    } else {
        if (tenths >= 10) put(static_cast<char16_t>(u'0' + tenths / 10));
        put(static_cast<char16_t>(u'0' + tenths % 10));
    }
    out[n] = 0;
}

void ValueText(const NativeModel& m, NativeRow row, char16_t* out, int cap) {
    const unsigned i = m.ValueIndex(row);
    switch (row) {
    case NativeRow::Picture: Text(out, cap, i ? MP_T_SET_V_DETAILED : MP_T_SET_V_SMOOTH); break;
    case NativeRow::Language:
        if (i == 0) Text(out, cap, MP_T_SET_V_SYSTEM_LANG);
        else LanguageText(m.LanguageName(i), out, cap);
        break;
    case NativeRow::LessMotion:
    case NativeRow::InvertCamera:
    case NativeRow::RearTouch:
    case NativeRow::TapToJump:
    case NativeRow::Assist60:
    case NativeRow::Allow50: Text(out, cap, i ? MP_T_ON : MP_T_OFF); break;
    case NativeRow::Tilt: Text(out, cap, i ? MP_T_SET_V_LEFT_STICK : MP_T_SET_V_CONSOLE); break;
    case NativeRow::TiltStrength:   // 1 to 10; 5 is the normal one
        Number(out, cap, static_cast<int>(i) + 1, false);
        if (i == 4) {   // "5 (Normal)": only the normal one carries the word
            char16_t w[24];
            Text(w, 24, MP_T_SET_V_NORMAL);
            int n = 0;
            while (out[n]) ++n;
            out[n++] = u' ', out[n++] = u'(';
            for (int k = 0; w[k] && n < cap - 2; ++k) out[n++] = w[k];
            out[n++] = u')', out[n] = 0;
        }
        break;
    case NativeRow::CameraZoom: Number(out, cap, 10 + static_cast<int>(i), true); break;   // 1.0 to 2.0
    case NativeRow::RightStick: Text(out, cap, i ? MP_T_SET_V_POINTER : MP_T_SET_V_CAMERA); break;
    case NativeRow::StarBitReach: Number(out, cap, static_cast<int>(i) + 1, false); break;
    default: Text(out, cap, i ? MP_T_SET_V_QUICK : MP_T_SET_V_STEADY); break;
    }
}

// The row's one sentence: it follows the current value; a row that cannot change says why.
MpText DescOf(const NativeModel& m, NativeRow row) {
    switch (m.Blocked(row)) {
    case RowBlock::Overridden: return MP_T_SET_D_TEST_FILE;
    case RowBlock::Together: return MP_T_SET_D_SESSION;
    case RowBlock::StarRace: return MP_T_SET_D_NOT_RACE;
    default: break;
    }
    const unsigned i = m.ValueIndex(row);
    const bool restartRow = row == NativeRow::Picture || row == NativeRow::Language || row == NativeRow::Assist60 || row == NativeRow::Allow50;   // (setnat31: 60 Assist too; mp30: Allow 50 fps)
    if (restartRow && m.InHold()) return MP_T_SET_D_HOLD;   // (this run started with the standard ones)
    if (restartRow && row != NativeRow::Picture && sSet.origin == Sequence::SO_PAUSE && m.WouldNeedRestart()) return MP_T_SET_D_RESTART_ASK;   // (setapply31: the game starts again when you leave; asked first)
    // modeux32: the Picture sentence names both modes (Performance aims for 60, Quality holds 30); once the choice differs from what is running it
    // says the restart in the same breath (the leave pill is "Save and Start Again"; from pause you are asked first). Nothing else about the row changes.
    const bool pictureMoved = row == NativeRow::Picture && (i != 0) != port::settings::Mode30(m.Boot());
    switch (row) {
    case NativeRow::Picture: return i ? (pictureMoved ? MP_T_SET_D_PICTURE_DETAILED_NEW : MP_T_SET_D_PICTURE_DETAILED) : (pictureMoved ? MP_T_SET_D_PICTURE_SMOOTH_NEW : MP_T_SET_D_PICTURE_SMOOTH);
    case NativeRow::Language: return i ? MP_T_SET_D_LANG_NAMED : MP_T_SET_D_LANG_OWN;
    case NativeRow::LessMotion: return i ? MP_T_SET_D_MOTION_ON : MP_T_SET_D_MOTION_OFF;
    case NativeRow::Tilt: return i ? MP_T_SET_D_TILT_STICK : MP_T_SET_D_TILT_CONSOLE;
    case NativeRow::TiltStrength: return MP_T_SET_D_TILT_POWER;
    case NativeRow::Pointer: return i ? MP_T_SET_D_POINTER_QUICK : MP_T_SET_D_POINTER_STEADY;
    case NativeRow::CameraZoom: return MP_T_SET_D_ZOOM;
    case NativeRow::RightStick: return i ? MP_T_SET_D_RSTICK_POINTER : MP_T_SET_D_RSTICK_CAMERA;
    case NativeRow::InvertCamera: return i ? MP_T_SET_D_INVERT_ON : MP_T_SET_D_INVERT_OFF;
    case NativeRow::RearTouch: return i ? MP_T_SET_D_REAR_ON : MP_T_SET_D_REAR_OFF;
    case NativeRow::TapToJump: return i ? MP_T_SET_D_TAPJUMP_ON : MP_T_SET_D_TAPJUMP_OFF;
    case NativeRow::Assist60: return i ? MP_T_SET_D_ASSIST_ON : MP_T_SET_D_ASSIST_OFF;
    case NativeRow::Allow50: return i ? MP_T_SET_D_ALLOW50_ON : MP_T_SET_D_ALLOW50_OFF;
    default: return MP_T_SET_D_REACH;
    }
}

// The moves of the button list: name and sentence (a two-slot move's second row has its own).
MpText MoveName(const std::string& id, unsigned slot) {
    if (id == "jump") return MP_T_SET_M_JUMP;
    if (id == "star_bits") return MP_T_SET_M_STAR_BITS;
    if (id == "spin") return slot ? MP_T_SET_M_SPIN2 : MP_T_SET_M_SPIN;
    if (id == "crouch") return MP_T_SET_M_CROUCH;
    if (id == "camera_centre") return MP_T_SET_M_CAMERA;
    if (id == "pause") return MP_T_SET_M_PAUSE;
    if (id == "minus") return MP_T_SET_M_AIM;
    if (id == "camera_up") return MP_T_SET_M_CAM_UP;
    if (id == "camera_down") return MP_T_SET_M_CAM_DOWN;
    if (id == "camera_left") return MP_T_SET_M_CAM_LEFT;
    return id == "camera_right" ? MP_T_SET_M_CAM_RIGHT : MP_T_NONE;
}
MpText MoveDesc(const std::string& id, unsigned slot) {
    if (id == "jump") return MP_T_SET_MD_JUMP;
    if (id == "star_bits") return MP_T_SET_MD_STAR_BITS;
    if (id == "spin") return slot ? MP_T_SET_MD_SPIN2 : MP_T_SET_MD_SPIN;
    if (id == "crouch") return MP_T_SET_MD_CROUCH;
    if (id == "camera_centre") return MP_T_SET_MD_CAMERA;
    if (id == "pause") return MP_T_SET_MD_PAUSE;
    if (id == "minus") return MP_T_SET_MD_AIM;
    if (id == "camera_up") return MP_T_SET_MD_CAM_UP;
    if (id == "camera_down") return MP_T_SET_MD_CAM_DOWN;
    if (id == "camera_left") return MP_T_SET_MD_CAM_LEFT;
    return id == "camera_right" ? MP_T_SET_MD_CAM_RIGHT : MP_T_NONE;
}

// A button of the control API as a player reads it: its short word ("Cross", "R", "L2", "D-pad Up"). remapui31: never the font's picture. The picture font's
// Cross / Circle / Triangle / Square / - / + / Z cells are the game's A / B / C / "1" / - / + / Z, and the handheld UI draws over them the button the player
// bound to that move (handheld_ui.cpp ResolveGlyph): with Jump on Circle the "Cross" cell shows a Circle. They name a move's button, not a button, so the list
// of buttons (and "X is used for Y") showed swapped pictures after a start, and a character the font did not have printed as "?".
void ButtonText(const std::string& id, char16_t* out, int cap) {
    if (id.empty()) {   // (a second slot with no button)
        Text(out, cap, MP_T_SET_V_NONE);
        return;
    }
    MpText t = MP_T_NONE;
    if (id == "cross") t = MP_T_SET_B_CROSS;
    else if (id == "circle") t = MP_T_SET_B_CIRCLE;
    else if (id == "square") t = MP_T_SET_B_SQUARE;
    else if (id == "triangle") t = MP_T_SET_B_TRIANGLE;
    else if (id == "l1") t = MP_T_SET_B_L;
    else if (id == "select") t = MP_T_SET_B_SELECT;
    else if (id == "start") t = MP_T_SET_B_START;
    else if (id == "r1") t = MP_T_SET_B_R;
    else if (id == "l2") t = MP_T_SET_B_L2;
    else if (id == "r2") t = MP_T_SET_B_R2;
    else if (id == "l3") t = MP_T_SET_B_L3;
    else if (id == "r3") t = MP_T_SET_B_R3;
    else if (id == "up") t = MP_T_SET_B_UP;
    else if (id == "down") t = MP_T_SET_B_DOWN;
    else if (id == "left") t = MP_T_SET_B_LEFT;
    else if (id == "right") t = MP_T_SET_B_RIGHT;
    if (t != MP_T_NONE) Text(out, cap, t);
    else txt::utf8To16(out, cap, id.c_str());
}
// The band is the game's text box: the box wraps at about 37 characters and shows two lines. A sentence longer than that breaks at its last space that fits.
void BreakLines(char16_t* s, int width = 34) {
    int start = 0, lastSpace = -1;
    for (int i = 0; s[i]; ++i) {
        if (s[i] == u'\n') { start = i + 1, lastSpace = -1; continue; }
        if (s[i] == u' ') lastSpace = i;
        if (i - start >= width && lastSpace > start) {
            s[lastSpace] = u'\n';
            start = lastSpace + 1, lastSpace = -1;
            i = start - 1;
        }
    }
}
void To8(const char16_t* in, char* out, int cap) {
    int n = 0;
    for (; *in && n < cap - 4; ++in) {
        const uint32_t c = *in;
        if (c < 0x80) out[n++] = static_cast<char>(c);
        else if (c < 0x800) out[n++] = static_cast<char>(0xC0 | (c >> 6)), out[n++] = static_cast<char>(0x80 | (c & 0x3F));
        else out[n++] = static_cast<char>(0xE0 | (c >> 12)), out[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F)), out[n++] = static_cast<char>(0x80 | (c & 0x3F));
    }
    out[n] = 0;
}

// A row can be changed now (not overridden, not read-only, not in a session, this console can do it).
bool Live(NativeRow row) { return sSet.model.CanStep(row); }
bool ButtonsLive() { return sSet.model.Editable() && sSet.model.RemapAvailable() && !sSet.model.BindingsOverridden() && sSet.model.BindingsEnabled(); }

void LogValue(NativeRow row) {
    const NativeModel& m = sSet.model;
    const unsigned i = m.ValueIndex(row);
    if (row == NativeRow::Language && i > 0) mp_log("settings: language -> %s", m.LanguageName(i).c_str());
    else mp_log("settings: %s -> %s", NativeModel::RowKey(row), m.ValueKey(row, i));
}

// One press on a value row. dir: -1 / +1. wrapEnd: Cross at the end of a range goes round to the other end (Left / Right stop).
bool StepRow(Io* io, NativeRow row, int dir, bool wrapEnd) {
    NativeModel& m = sSet.model;
    sSet.asked = false;   // (a change: the leave question may come again)
    StepResult r = m.Step(row, dir);
    if (r == StepResult::AtEnd && wrapEnd) {
        while (m.Step(row, -dir) == StepResult::Changed) {}
        r = StepResult::Changed;
    }
    if (r != StepResult::Changed) {
        if (io) io->sound(Se::Buzzer);
        return false;
    }
    if (io) io->sound(Se::Tab);
    LogValue(row);
    return true;
}
bool WrapsAtEnd(NativeRow r) { return r == NativeRow::TiltStrength || r == NativeRow::CameraZoom || r == NativeRow::StarBitReach; }

void Toast(MpText id) {
    char16_t t[96];
    txt::format(t, 96, id, Args());
    MpOverlay::toastText(t, 0);
}
void SavedToast() {
    const NativeModel& m = sSet.model;
    Toast(m.RestartNeeded() || m.NextStartOnly() ? MP_T_SET_TOAST_SAVED_LATER : MP_T_SET_TOAST_SAVED);   // (setapply31: from any door: a saved change that waits for a start says so)
}

// The buttons of the list: one row per move slot.
void BuildMoves() {
    sSet.moves.clear();
    for (const auto& mv : sSet.model.Moves())
        for (unsigned slot = 0; slot < mv.slots && slot < 2; ++slot) sSet.moves.push_back({mv.id, slot});
}
// The buttons a move's slot may take; the second slot also may have none.
void BuildButtons(const MoveRow& mv) {
    sSet.buttons.clear();
    if (mv.slot > 0) sSet.buttons.push_back({"", "", 0});
    for (const auto& b : sSet.model.Buttons()) sSet.buttons.push_back(b);
}

}  // namespace

// ------------------------------------------------------------------ opening and closing
int Sequence::settingsOrigin() const { return sSet.origin; }
void Sequence::settingsTestSwap() {
    if (!mSettingsOn) return;
    for (const MoveRow& mv : sSet.moves)
        if (mv.id == "jump" && mv.slot == 0) {
            const BindOutcome o = sSet.model.Bind(mv.id, mv.slot, "circle", true);
            (void)o;
            mp_log("settings: test swap jump -> circle");
            break;
        }
    finishSettings(true);
}
void Sequence::settingsTestSetWhere(int origin) { sSet.model.SetWhere(origin == SO_SESSION ? NativeWhere::Session : (origin == SO_PAUSE ? NativeWhere::Pause : NativeWhere::Entry)); }

// A start that ran in a hold (Picture and Language at the standard ones so the game could start) says so once, at the entry screen.
void setProblemReportHook(void (*fn)(const char* where)) { sReportHook = fn; }
bool problemReportHooked() { return sReportHook != nullptr; }

void settingsBootToast() {
    if (!port::settings::NativeTakeBootToast()) return;
    mp_log("settings: hold toast");
    Toast(MP_T_SET_TOAST_HOLD);
}

bool Sequence::openSettings(int origin) {
    const NativeHost* host = port::settings::NativeHostGet();
    if (!host || !host->available) {
        mp_log("settings: not available on this console");
        return false;
    }
    if (mSettingsOn) return false;
    sSet = Set();
    sSet.origin = origin;
    std::vector<std::string> languages;
    if (port_language_count && port_language_name) {
        const int n = port_language_count();
        for (int i = 0; i < n && i < kMaxLanguages; ++i) {
            const char* name = port_language_name(i);
            bool dup = false;
            for (const auto& l : languages) dup = dup || l == name;
            if (name && name[0] && !dup) languages.emplace_back(name);
        }
    }
    sSet.model.Open(*host, languages);
    sSet.model.SetWhere(origin == SO_SESSION ? NativeWhere::Session : (origin == SO_PAUSE ? NativeWhere::Pause : NativeWhere::Entry),
                        origin == SO_SESSION && mp_mode() == MP_MODE_RACE);
    BuildMoves();
    if (origin != SO_SESSION) {   // (the session's menu stays under it: Back comes home to it)
        mFromFileSelect = false;
        mFromEntry = false;
        mDepth = 0;
        mFade = 0;
    }
    mSettingsOn = true;
    port::settings::NativeMenuOpenSet(true);
    push(S_SET_MAIN);
    mp_log("settings: opened (%s)", origin == SO_ENTRY ? "entry" : (origin == SO_PAUSE ? "pause" : "session"));
    if (mIo) mIo->sound(Se::Open);
    if (!sSet.model.Editable()) {
        mp_log("settings: read only");
        say(P_SET_RO, MP_T_DLG_SETTINGS_RO);   // (once; the rows are dim and Back asks nothing)
    } else if (sSet.model.LoadDamaged()) {
        mp_log("settings: file damaged");
        Toast(MP_T_SET_TOAST_UNREADABLE);
    }
    return true;
}

// Every way out ends here: the menu is gone. Changes that are still unsaved are saved (a forced close: the session ended, a cut to safety;
// the player only loses them by choosing Don't Save).
void Sequence::settingsClosed() {
    if (!mSettingsOn) return;
    mSettingsOn = false;
    NativeModel& m = sSet.model;
    if (!sSet.discard && m.Editable() && m.Dirty()) {
        const port::settings::StoreResult r = m.Save();
        if (r.status == port::settings::StoreStatus::Ok) {
            mp_log("settings: saved (on closing)");
            if (port_zoom_save_now) port_zoom_save_now();
            SavedToast();
        } else {
            mp_log("settings: not saved (on closing)");
        }
    }
    port::settings::NativeMenuOpenSet(false);
    mp_log("settings: closed");
}

// ------------------------------------------------------------------ the family's hooks
bool Sequence::itemsSettings(Scr s, uint8_t param, int* out, int* np) const {
    int& n = *np;
    auto add = [&](int i) { if (n < kItems) out[n++] = i; };
    switch (s) {
    case S_SET_MAIN: case S_SET_CAMERA: case S_SET_CONTROLS: case S_SET_MORE:
        {
        Spec all[8];
        const int c = SpecsOf(s, all);
        for (int k = 0; k < c; ++k) add(I_LIST0 + k);
        break;
        }
    case S_SET_BUTTONS:
        for (int k = 0; k < static_cast<int>(sSet.moves.size()); ++k) add(I_LIST0 + k);
        break;
    case S_SET_PICK:
        for (int k = 0; k < static_cast<int>(sSet.buttons.size()); ++k) add(I_LIST0 + k);
        break;
    case S_SET_LEAVE:
        add(Pill(K_SAVE)), add(Pill(K_DONT)), add(Pill(K_KEEP));
        break;
    case S_SET_CAPTURE:
        break;
    case S_SET_RESTART:   // setfix31b: param 0: Later; param 1 (the save seems stuck): Start Again Now (only when safe) and Later
        if (param == 1 && sSet.model.RelaunchSafe()) add(Pill(K_NOW));
        add(Pill(K_LATER));
        break;
    default: return false;
    }
    (void)param;
    return true;
}

bool Sequence::chooseSettings(int item) {
    if (!mSettingsOn || mDepth == 0 || !IsSet(top().scr)) return false;
    Io* io = mIo;
    auto decide = [io] { if (io) io->sound(Se::Decide); };
    NativeModel& m = sSet.model;
    const Scr scr = top().scr;
    if (sSet.restarting > 0) {   // setfix31b: only the wait window's choices
        const int k = item >= I_SET0 && item < I_LIST0 ? item - I_SET0 : -1;
        if (scr == S_SET_RESTART && k == K_LATER) { decide(); cancelRestart(); }
        else if (scr == S_SET_RESTART && k == K_NOW && top().param == 1 && m.RelaunchSafe()) {
            decide();
            mp_log("settings: start again now (the save had stopped)");
            restartNow();
        }
        return true;
    }
    Spec sp;
    if (SpecAt(scr, item, &sp)) {
        switch (sp.kind) {
        case R_VALUE: StepRow(io, sp.row, 1, WrapsAtEnd(sp.row)); return true;
        case R_CAMERA: decide(); push(S_SET_CAMERA); return true;
        case R_CONTROLS: decide(); push(S_SET_CONTROLS); return true;
        case R_MORE: decide(); push(S_SET_MORE); return true;
        case R_BUTTONS:
            if (!ButtonsLive()) {
                if (io) io->sound(Se::Buzzer);
                return true;
            }
            decide();
            BuildMoves();
            push(S_SET_BUTTONS);
            return true;
        case R_REPORT:
            decide();
            if (sReportHook) sReportHook(sSet.origin == SO_ENTRY ? "settings-entry" : (sSet.origin == SO_PAUSE ? "settings-pause" : "settings-session"));
            return true;
        case R_RESET_ALL:
            decide();
            ask(P_SET_RESET_ALL, MP_T_SET_DLG_RESET_ALL, Args(), Dlg::YesNo);
            return true;
        }
        return true;
    }
    const int pill = item >= I_SET0 && item < I_LIST0 ? item - I_SET0 : -1;
    if (scr == S_SET_LEAVE && pill == K_SAVE) {
        decide();
        const bool again = RestartOnSave(sSet.origin, m);
        if (again && sSet.origin == SO_PAUSE && !sSet.restartConfirmed) {   // (setapply31: a start leaves the level: ask first; No keeps the leave question)
            mp_log("settings: restart asked (pause)");
            ask(P_SET_RESTART, MP_T_SET_DLG_RESTART_PAUSE, Args(), Dlg::YesNo);
            return true;
        }
        sSet.restartConfirmed = false;
        const port::settings::StoreResult r = m.Save();
        if (r.status != port::settings::StoreStatus::Ok) {
            mp_log("settings: save failed");
            if (io) io->sound(Se::Buzzer);
            say(P_SET_SAVE_FAIL, MP_T_DLG_SETTINGS_RO);   // (the leave question stays)
            return true;
        }
        mp_log("settings: saved");
        if (port_zoom_save_now) port_zoom_save_now();
        if (again) {   // "Save and Start Again": wait (up to kRestartFrames) for the card to be quiet, then start the game again; the tick does it
            mp_log("settings: restart requested");
            BeginRestart();
            return true;
        }
        finishSettings(true);
        return true;
    }
    if (scr == S_SET_LEAVE && pill == K_DONT) {
        decide();
        m.Revert();
        sSet.discard = true;
        mp_log("settings: not saved");
        finishSettings(false);
        return true;
    }
    if (scr == S_SET_LEAVE && pill == K_KEEP) {
        decide();
        pop();
        return true;
    }
    if (item < I_LIST0) return false;
    const int k = item - I_LIST0;
    if (scr == S_SET_BUTTONS) {
        if (k >= static_cast<int>(sSet.moves.size())) return true;
        if (!ButtonsLive()) {
            if (io) io->sound(Se::Buzzer);
            return true;
        }
        const MoveRow& mv = sSet.moves[static_cast<size_t>(k)];
        decide();
        sSet.timedOut = false;
        if (mv.slot == 0 && io && io->rawButtons() != ~0ull) {   // capture first: "Press the button you want"
            sSet.cap = CAP_RELEASE, sSet.capFrames = 0, sSet.capRow = k, sSet.capPrev = 0;
            push(S_SET_CAPTURE, static_cast<uint8_t>(k));
            mp_log("settings: capture started");
            return true;
        }
        openPick(k);
        return true;
    }
    if (scr == S_SET_PICK) {
        const int row0 = top().param;
        if (k < 0 || k >= static_cast<int>(sSet.buttons.size()) || row0 < 0 || row0 >= static_cast<int>(sSet.moves.size())) return true;
        if (bindTo(sSet.moves[static_cast<size_t>(row0)].id, sSet.moves[static_cast<size_t>(row0)].slot, sSet.buttons[static_cast<size_t>(k)].id)) pop();
        return true;
    }
    return false;
}

// The button list's fallback: the buttons of the controller in use, the move's own on the cursor.
void Sequence::openPick(int row) {
    const MoveRow& mv = sSet.moves[static_cast<size_t>(row)];
    BuildButtons(mv);
    push(S_SET_PICK, static_cast<uint8_t>(row));
    const std::string have = sSet.model.ButtonOf(mv.id, mv.slot);
    for (size_t j = 0; j < sSet.buttons.size(); ++j)
        if (sSet.buttons[j].id == have) top().cursor = static_cast<int8_t>(j), fitScroll(top());
}

// Put a button on a move's slot. true: done (it was bound). A conflict asks to swap; the others say why not.
bool Sequence::bindTo(const std::string& moveId, unsigned slot, const std::string& button) {
    Io* io = mIo;
    const MoveRow mv{moveId, slot};
    NativeModel& m = sSet.model;
    sSet.asked = false;
    const BindOutcome o = m.Bind(mv.id, mv.slot, button, false);
    switch (o.kind) {
    case BindOutcome::Ok:
        if (io) io->sound(Se::Decide);
        mp_log("settings: %s -> %s", mv.id.c_str(), button.empty() ? "none" : button.c_str());
        return true;
    case BindOutcome::Conflict:
        {
        sSet.pendMove = mv.id, sSet.pendSlot = mv.slot, sSet.pendButton = button;
        char16_t b16[24], v16[40];
        ButtonText(button, b16, 24);
        Text(v16, 40, MoveName(o.conflictMove, 0));
        for (const MoveRow& r : sSet.moves)   // (the move's name as its row shows it: the second spin button has its own)
            if (r.id == o.conflictMove && m.ButtonOf(r.id, r.slot) == button) Text(v16, 40, MoveName(r.id, r.slot));
        char b8[48], v8[96];
        To8(b16, b8, sizeof b8), To8(v16, v8, sizeof v8);
        Args a;
        a.str = b8, a.str2 = v8;
        mp_log("settings: %s would take %s from %s", mv.id.c_str(), button.c_str(), o.conflictMove.c_str());
        ask(P_SET_CONFLICT, MP_T_SET_DLG_CONFLICT, a, Dlg::YesNo);
        return false;
        }
    case BindOutcome::RequiredActionLost:
        if (io) io->sound(Se::Buzzer);
        mp_log("settings: %s would be left without a button", mv.id.c_str());
        say(P_SET_LOST, MP_T_SET_DLG_LOST);
        return false;
    default:
        if (io) io->sound(Se::Buzzer);
        mp_log("settings: %s is not available", button.c_str());
        say(P_SET_UNAVAILABLE, MP_T_SET_DLG_UNAVAILABLE);
        return false;
    }
}

// The menu is done (saved or not): the entry screen / the level again, or the session's menu.
void Sequence::finishSettings(bool toast) {
    const int origin = sSet.origin;
    if (origin == SO_SESSION) {
        popTo(S_SESSION);
        if (toast) SavedToast();
        settingsClosed();
    } else {
        if (toast) SavedToast();
        close();
    }
}

bool Sequence::dialogSettings(int purpose, bool yes, int param) {
    (void)param;
    NativeModel& m = sSet.model;
    switch (purpose) {
    case P_SET_RO:
    case P_SET_SAVE_FAIL:
    case P_SET_LOST:
    case P_SET_UNAVAILABLE:
        return true;
    case P_SET_RESTART:   // setapply31: from pause, "Start the game again now?" Yes: save and start again; No: the leave question again
        if (!yes) {
            mp_log("settings: restart declined (pause)");
            return true;
        }
        sSet.restartConfirmed = true;
        if (mDepth > 0 && top().scr == S_SET_LEAVE) chooseSettings(Pill(K_SAVE));
        else sSet.restartConfirmed = false;
        return true;
    case P_SET_RESTART_FAIL:   // (it did not start again: the settings are saved for the next start)
        sSet.restarting = 0;
        finishSettings(true);
        return true;
    case P_SET_CONFLICT:
        if (!yes) return true;   // (the pick screen again)
        {
            const BindOutcome o = m.Bind(sSet.pendMove, sSet.pendSlot, sSet.pendButton, true);
            if (o.kind == BindOutcome::Ok) {
                mp_log("settings: %s -> %s (swapped)", sSet.pendMove.c_str(), sSet.pendButton.c_str());
                if (mDepth > 0 && (top().scr == S_SET_PICK || top().scr == S_SET_CAPTURE)) pop();
            } else if (o.kind == BindOutcome::RequiredActionLost) {
                if (mIo) mIo->sound(Se::Buzzer);
                say(P_SET_LOST, MP_T_SET_DLG_LOST);
            } else {
                if (mIo) mIo->sound(Se::Buzzer);
                say(P_SET_UNAVAILABLE, MP_T_SET_DLG_UNAVAILABLE);
            }
        }
        return true;
    case P_SET_RESET:   // Triangle on the button list
        if (yes) {
            m.Defaults();
            mp_log("settings: buttons put back");
        }
        return true;
    case P_SET_RESET_ALL:   // More: Put Everything Back
        if (yes) {
            m.PutEverythingBack();
            mp_log("settings: everything put back");
        }
        return true;
    default: return false;
    }
}

bool Sequence::keyboardSettings(int purpose, const char* text) {
    (void)purpose, (void)text;
    return false;
}

// The held buttons that are new since the last frame: exactly one is the press.
static int OneBit(uint64_t bits) {
    if (!bits || (bits & (bits - 1))) return -1;
    int i = 0;
    while (!((bits >> i) & 1)) ++i;
    return i;
}

// setfix31b: start the game again now (the host's relaunch: never returns on a console). A relaunch that returns: the "close the game" window.
void Sequence::restartNow() {
    NativeModel& m = sSet.model;
    mp_log("settings: restart now");
    sSet.restarting = 0;
    if (!m.Relaunch()) {
        if (mDepth > 0 && top().scr == S_SET_RESTART) pop();
        say(P_SET_RESTART_FAIL, MP_T_SET_DLG_RESTART_FAIL);   // (a console never sees this: the process is gone)
    } else {
        finishSettings(false);
    }
}
// setfix31b: Later: no restart now; the settings are saved and the change comes at the next start (the "next time" toast says so)
void Sequence::cancelRestart() {
    mp_log("settings: restart put off: the change applies the next time the game starts");
    sSet.restarting = 0;
    if (mDepth > 0 && top().scr == S_SET_RESTART) pop();
    finishSettings(true);
}

bool Sequence::tickSettings(Scr scr, const Input& in, bool* stop) {
    (void)in;
    if (!IsSet(scr) || !mSettingsOn) return false;
    if (sSet.origin != SO_PAUSE && port_zoom_menu) port_zoom_menu(0);   // (settings.ini is only written from there: the pause menu calls it itself)
    NativeModel& m = sSet.model;
    if (sSet.restarting > 0) {   // "Save and Start Again": the card has to be quiet; then the host starts the game again
        ++sSet.restarting;
        if (m.RelaunchReady()) {
            restartNow();
            *stop = true;
            return true;
        }
        if (!sSet.waitShown) {   // setfix31b: a save is in flight: the small window, the restart comes by itself when it is done
            sSet.waitShown = true;
            push(S_SET_RESTART, 0);
            top().cursor = 0;
            mp_log("settings: restart waits for a save to finish");
            *stop = true;
            return true;
        }
        const unsigned progress = m.SaveProgress();
        if (progress != sSet.lastProgress) sSet.lastProgress = progress, sSet.stallFrames = 0;
        else ++sSet.stallFrames;
        const bool failed = m.SaveFailures() != sSet.failuresAtStart;
        if (mDepth > 0 && top().scr == S_SET_RESTART && top().param == 0 && (failed || sSet.stallFrames > kStallFrames)) {
            top().param = 1;
            top().cursor = static_cast<int8_t>(m.RelaunchSafe() ? 1 : 0);   // (on Later: the safe choice)
            mp_log("settings: the save %s; asking: start again now or later (%s)", failed ? "reported an error" : "made no progress for 30 s",
                   m.RelaunchSafe() ? "Safe Saves: starting again cannot damage the save" : "no Start Again Now: Safe Saves is off");
            if (mIo) mIo->sound(Se::Buzzer);
        }
        *stop = !(mDepth > 0 && top().scr == S_SET_RESTART);   // (the window's own buttons work)
        return true;
    }
    if (scr != S_SET_CAPTURE) return false;
    *stop = true;   // (every button is the player's answer, none of them works the menu)
    Io* io = mIo;
    const uint64_t held = io ? io->rawButtons() : ~0ull;
    const int row = sSet.capRow;
    auto toList = [&](bool timedOut) {
        sSet.timedOut = timedOut;
        mp_log("settings: capture -> list");
        pop();
        if (row >= 0 && row < static_cast<int>(sSet.moves.size())) openPick(row);
    };
    if (held == ~0ull || row < 0 || row >= static_cast<int>(sSet.moves.size())) { toList(false); return true; }
    if (io && io->touched()) { toList(false); return true; }
    switch (sSet.cap) {
    case CAP_RELEASE:   // nothing may be held yet (the Cross that opened this)
        if (held == 0) sSet.cap = CAP_ARMED, sSet.capFrames = 0, sSet.capPrev = 0;
        else if (++sSet.capFrames > kCaptureFrames) toList(true);
        break;
    case CAP_ARMED:
        {
        const uint64_t fresh = held & ~sSet.capPrev;
        sSet.capPrev = held;
        const int bit = OneBit(fresh);
        if (bit >= 0) {
            const uint64_t mask = 1ull << bit;
            std::string id;
            bool known = false;
            for (const auto& b : m.Buttons())
                if (b.mask == mask) id = b.id, known = true;
            if (!known) {   // (a button this controller's profile does not use)
                if (io) io->sound(Se::Buzzer);
                break;
            }
            const MoveRow mv = sSet.moves[static_cast<size_t>(row)];
            sSet.cap = CAP_DONE;
            sSet.capPrev = held;
            if (bindTo(mv.id, mv.slot, id)) sSet.capFrames = 0;   // bound: wait for the release, then go back
            else sSet.capFrames = -1;                 // a window is up (conflict / lost / unavailable)
            break;
        }
        if (++sSet.capFrames > kCaptureFrames) toList(true);
        break;
        }
    case CAP_DONE:
        if (sSet.capFrames < 0) break;   // (the window answers; a Yes pops this screen)
        if (held == 0) pop();
        break;
    }
    return true;
}

bool Sequence::labelSettings(int item, MpPanel::Row& r, const Frame& f) const {
    using namespace MpPanel;
    if (!mSettingsOn || !IsSet(f.scr)) return false;
    const NativeModel& m = sSet.model;
    Spec sp;
    if (SpecAt(f.scr, item, &sp)) {
        switch (sp.kind) {
        case R_VALUE:
            Text(r.label, kLabel, LabelOf(sp.row));
            ValueText(m, sp.row, r.value, kValue);
            r.marks |= MARK_ARROWS;
            if (!Live(sp.row)) r.marks |= MARK_DISABLED;
            break;
        case R_CAMERA: Text(r.label, kLabel, MP_T_SET_L_CAMERA), r.marks |= MARK_NAV; break;
        case R_CONTROLS: Text(r.label, kLabel, MP_T_SET_L_CONTROLS), r.marks |= MARK_NAV; break;
        case R_MORE: Text(r.label, kLabel, MP_T_SET_L_MORE), r.marks |= MARK_NAV; break;
        case R_BUTTONS:
            Text(r.label, kLabel, MP_T_SET_L_BUTTONS);
            r.marks |= MARK_NAV;
            if (!ButtonsLive()) r.marks |= MARK_DISABLED;
            break;
        case R_RESET_ALL: Text(r.label, kLabel, MP_T_SET_L_RESET); break;
        case R_REPORT: Text(r.label, kLabel, MP_T_SET_L_REPORT); break;
        }
        return true;
    }
    const int pill = item >= I_SET0 && item < I_LIST0 ? item - I_SET0 : -1;
    if (f.scr == S_SET_RESTART) {   // setfix31b
        Text(r.label, kLabel, pill == K_NOW ? MP_T_SET_START_NOW : MP_T_SET_LATER);
        return true;
    }
    if (f.scr == S_SET_LEAVE) {
        const bool again = RestartOnSave(sSet.origin, m);
        Text(r.label, kLabel, pill == K_SAVE ? (again ? MP_T_SET_SAVE_RESTART : MP_T_SET_SAVE) : (pill == K_DONT ? MP_T_SET_DONT_SAVE : MP_T_SET_KEEP_EDITING));
        return true;
    }
    if (item < I_LIST0) return false;
    const int k = item - I_LIST0;
    if (f.scr == S_SET_BUTTONS && k < static_cast<int>(sSet.moves.size())) {
        const MoveRow& mv = sSet.moves[static_cast<size_t>(k)];
        Text(r.label, kLabel, MoveName(mv.id, mv.slot));
        ButtonText(m.ButtonOf(mv.id, mv.slot), r.value, kValue);
        if (!ButtonsLive()) r.marks |= MARK_DISABLED;
        return true;
    }
    if (f.scr == S_SET_PICK && k < static_cast<int>(sSet.buttons.size())) {
        ButtonText(sSet.buttons[static_cast<size_t>(k)].id, r.label, kLabel);
        return true;
    }
    return false;
}

bool Sequence::modelSettings(MpPanel::Model* m, const Frame& f, int cursorItem, int count, const int* it) const {
    using namespace MpPanel;
    if (!mSettingsOn || !IsSet(f.scr)) return false;
    (void)count, (void)it;
    auto button = [m](int pic, MpText label) { ModelButton(m, pic, label); };
    const NativeModel& nm = sSet.model;
    MpText band = MP_T_NONE;
    Args bandArgs;
    char arg1[96] = "";
    switch (f.scr) {
    case S_SET_MAIN:
    case S_SET_CAMERA:
    case S_SET_CONTROLS:
    case S_SET_MORE:
        {
        m->kind = KIND_LIST;
        Spec sp;
        bool value = false;
        if (SpecAt(f.scr, cursorItem, &sp)) {
            switch (sp.kind) {
            case R_VALUE:
                band = DescOf(nm, sp.row), value = true;
                if (band == MP_T_SET_D_LANG_OWN) {   // "The game uses your console's language: French."
                    char16_t name[40];
                    std::string sys = nm.SystemLanguageName();
                    if (sys.empty() && nm.ValueCount(NativeRow::Language) > 1) sys = nm.LanguageName(1);   // (no console language known here: the disc's first)
                    LanguageText(sys, name, 40);
                    To8(name, arg1, sizeof arg1);
                    bandArgs.str = arg1;
                }
                break;
            case R_CAMERA: band = MP_T_SET_D_CAMERA; break;
            case R_CONTROLS: band = MP_T_SET_D_CONTROLS; break;
            case R_MORE: band = MP_T_SET_D_MORE; break;
            case R_BUTTONS: band = ButtonsLive() ? MP_T_SET_D_BUTTONS : (nm.Where() == NativeWhere::Session ? MP_T_SET_D_SESSION : MP_T_SET_D_TEST_FILE); break;
            case R_RESET_ALL: band = MP_T_SET_D_RESET; break;
            case R_REPORT: band = MP_T_SET_D_REPORT; break;
            }
        }
        if (band == MP_T_NONE) band = MP_T_SET_BAND;
        const bool nav = SpecAt(f.scr, cursorItem, &sp) && (sp.kind == R_CAMERA || sp.kind == R_CONTROLS || sp.kind == R_MORE || sp.kind == R_BUTTONS);
        button(txt::PIC_CROSS, value ? MP_T_CHANGE : (nav ? MP_T_SET_OPEN : MP_T_SELECT)), button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
        }
    case S_SET_BUTTONS:
    case S_SET_PICK:
        {
        m->kind = KIND_LIST;
        const int k = cursorItem - I_LIST0;
        if (f.scr == S_SET_BUTTONS) {
            if (!ButtonsLive()) band = nm.Where() == NativeWhere::Session ? MP_T_SET_D_SESSION : MP_T_SET_D_TEST_FILE;
            else if (k >= 0 && k < static_cast<int>(sSet.moves.size())) band = nm.NextStartOnly() ? MP_T_SET_D_BUTTONS_NEXT : MoveDesc(sSet.moves[static_cast<size_t>(k)].id, sSet.moves[static_cast<size_t>(k)].slot);
            button(txt::PIC_CROSS, MP_T_CHANGE), button(txt::PIC_TRIANGLE, MP_T_SET_RESET), button(txt::PIC_CIRCLE, MP_T_BACK);
        } else {
            const int row0 = f.param;   // (the move being changed: its sentence stays in the band)
            if (sSet.timedOut) band = MP_T_SET_D_CAPTURE_TIMEOUT;
            else if (row0 >= 0 && row0 < static_cast<int>(sSet.moves.size())) band = MoveDesc(sSet.moves[static_cast<size_t>(row0)].id, sSet.moves[static_cast<size_t>(row0)].slot);
            button(txt::PIC_CROSS, MP_T_SELECT), button(txt::PIC_CIRCLE, MP_T_BACK);
        }
        break;
        }
    case S_SET_CAPTURE:
        m->kind = KIND_BUTTONS;
        band = MP_T_SET_D_CAPTURE;
        if (f.param < sSet.moves.size()) {   // "Press the button you want for Jump."
            char16_t name[40];
            Text(name, 40, MoveName(sSet.moves[f.param].id, sSet.moves[f.param].slot));
            To8(name, arg1, sizeof arg1);
            bandArgs.str = arg1;
        }
        button(txt::PIC_HAND, MP_T_SET_CAP_LIST);   // (every button, Circle too, is the answer: only a touch or the wait gives the list)
        break;
    case S_SET_LEAVE:
        m->kind = KIND_BUTTONS;
        band = MP_T_SET_LEAVE_Q;
        button(txt::PIC_CROSS, MP_T_SELECT), button(txt::PIC_CIRCLE, MP_T_SET_KEEP_EDITING);
        break;
    case S_SET_RESTART:   // setfix31b: "Finishing saving..." / the save seems stuck
        m->kind = KIND_BUTTONS;
        band = f.param == 0 ? MP_T_SET_RESTART_WAIT : (nm.RelaunchSafe() ? MP_T_SET_RESTART_STUCK : MP_T_SET_RESTART_STUCK_LATER);
        if (f.param == 1 && nm.RelaunchSafe()) button(txt::PIC_CROSS, MP_T_SELECT);
        button(txt::PIC_CIRCLE, MP_T_SET_LATER);
        break;
    default: return false;
    }
    m->look = LOOK_GAME;
    m->selectedArrows = true;
    if (band != MP_T_NONE) txt::format(m->line, 96, band, bandArgs), BreakLines(m->line);
    return true;
}

// Left / Right on a value row change it (before the core's own changeValue); Triangle on the button list asks to put the buttons back.
bool Sequence::pressSettings(Scr scr, const Input& in, int item, int count, int stage) {
    if (!mSettingsOn || !IsSet(scr)) return false;
    if (sSet.restarting > 0) {   // setfix31b: on the wait window Left / Right move between its two choices too; nothing else works the menu meanwhile
        if (scr == S_SET_RESTART && stage == 0 && (in.up || in.down || in.left || in.right)) moveCursor(in.up || in.left ? -1 : 1, count);
        return true;
    }
    if (stage == 0) {
        if (!(in.left || in.right)) return false;   // (Up / Down: the cursor)
        Spec sp;
        if (SpecAt(scr, item, &sp) && sp.kind == R_VALUE) StepRow(mIo, sp.row, in.left ? -1 : 1, false);
        return true;
    }
    if (stage == 1 && in.tri && scr == S_SET_BUTTONS) {
        if (!ButtonsLive()) {
            if (mIo) mIo->sound(Se::Buzzer);
            return true;
        }
        if (mIo) mIo->sound(Se::Decide);
        ask(P_SET_RESET, MP_T_SET_DLG_RESET, Args(), Dlg::YesNo);
        return true;
    }
    return false;
}

// Back: on Settings with changes, the leave question (Back there is Keep Editing); a capture is answered by every button, so it never gets here.
bool Sequence::leaveSettings(Scr scr, uint8_t param, int phase) {
    (void)param;
    if (mSettingsOn && sSet.restarting > 0 && IsSet(scr)) {   // setfix31b: Circle on the wait window is Later; nothing else backs out meanwhile
        if (phase == 0 && scr == S_SET_RESTART) {
            if (mIo) mIo->sound(Se::Cancel);
            cancelRestart();
        }
        return true;
    }
    if (!mSettingsOn || phase != 0 || scr != S_SET_MAIN) return false;
    if (!sSet.model.Editable() || !sSet.model.Dirty()) return false;
    if (sSet.asked) {   // (asked once: Back again leaves, as the close saves; Circle pressed again and again always gets out)
        // setapply31: at the entry screen a change that needs a start is saved AND the game starts again (nothing can be lost there); from pause the
        // close saves it and the toast says it changes the next time the game starts (the player has already said no to leaving the level now).
        if (sSet.origin == SO_ENTRY && RestartOnSave(sSet.origin, sSet.model)) {
            const port::settings::StoreResult r = sSet.model.Save();
            if (r.status != port::settings::StoreStatus::Ok) return false;   // (the close tries again, and says nothing worse)
            mp_log("settings: saved");
            mp_log("settings: restart requested (Back after the question)");
            if (port_zoom_save_now) port_zoom_save_now();
            BeginRestart();
            return true;
        }
        return false;
    }
    sSet.asked = true;
    mp_log("settings: leave asked");
    if (mIo) mIo->sound(Se::Decide);
    push(S_SET_LEAVE);
    return true;
}

}  // namespace MpMenu
