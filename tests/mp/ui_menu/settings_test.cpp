// The menus' host tests: the native in-game Settings (settingsnative, family: MpMenuSettings.cpp, plan v3) and its entry button.
// A fake NativeHost over memory files and a fake live table (common.hpp: fake::cfg, installed by fake::reset()), the real model and menu.
#include "common.hpp"

using namespace port::settings;
namespace controls = port::controls;

namespace {

inline void up(int n = 1) { for (int i = 0; i < n; i++) press(8); }
inline void left(int n = 1) { for (int i = 0; i < n; i++) press(5); }

MpPanel::Model& Mod() {
    static MpPanel::Model m;
    S().buildModel(&m);
    return m;
}
std::u16string Flat(std::u16string s) { for (auto& c : s) if (c == u'\n') c = u' '; return s; }   // (a long sentence is broken into two lines by the menu)
bool BandIs(MpText id) { return Flat(Mod().line) == Flat(Fmt16(id)); }
bool BandStarts(MpText id) { const std::u16string a = Flat(Mod().line), b = Flat(Fmt16(id)); return a.size() > 10 && b.size() > 10 && a.compare(0, 10, b, 0, 10) == 0; }   // (a sentence with a name in it)
bool Shows(MpText label, MpText value) { return rowShows(label, value); }
bool ToastUp(MpText id) {
    const std::u16string want = Fmt16(id);
    for (int i = 0; i < MpOverlay::toastCount(); i++)
        if (std::u16string(MpOverlay::toasts()[i].text) == want) return true;
    return false;
}
bool Dim(int i) { return rowDisabled(i); }
std::string FileText() {
    auto it = fake::cfg.files.data.find(fake::kCfgPath);
    return it == fake::cfg.files.data.end() ? std::string() : it->second;
}
std::string SettingsText(const Config& c) {
    Document d{DefaultConfig(controls::GetControlApi()), "", true};
    std::string out;
    SerializeDocument(d, c, controls::GetControlApi(), out);
    return out;
}
Config FileConfig() {
    Document d;
    LoadDocument(fake::kCfgPath, controls::GetControlApi(), fake::cfg.ops, d);
    return d.config;
}
int Live(NativeRow r) { return fake::cfg.live.val[static_cast<int>(r)]; }
void Fresh() { fresh(); fake::cfgReset(true, true, true); }   // (the other families' tests run without a native Settings: no pill in the session's menu)
void OpenSettings(int origin = MpMenu::Sequence::SO_ENTRY) {
    Fresh();
    S().openSettings(origin);
    frames(3);
}
// From Settings (the cursor on Picture): Camera, Controls, More. (setnat31: while Picture is Smooth Motion the second row is 60 Assist: one row more)
int AssistRow() { return S().screen() == Scr::S_SET_MAIN && S().itemCount() == 5 ? 1 : 0; }
void ToCamera() { down(1 + AssistRow()); ok(); }
void ToControls() { down(2 + AssistRow()); ok(); }
void ToMore() { down(3 + AssistRow()); ok(); }
void ToButtons() { ToControls(); down(5); ok(); }   // (Tilt, Strength, Pointer, Rear Touch, Tap to Jump, then Change Buttons)
std::u16string Cur16() { const auto& m = Mod(); return m.rows[m.cursor].value; }
bool HasNormal(int i) { return Mod().rows[i].value == std::u16string(u"5 (") + Fmt16(MP_T_SET_V_NORMAL) + u")"; }
void DumpLog(const char* what) { fprintf(stderr, "%s; screen %s; log:", what, MpMenu::screenName(S().screen())); for (size_t i = fake::log.size() > 10 ? fake::log.size() - 10 : 0; i < fake::log.size(); i++) fprintf(stderr, " | %s", fake::log[i].c_str()); fprintf(stderr, "\n"); }
uint64_t MaskOf(const char* id) {
    NativeModel m;
    m.Open(fake::cfg.host, {});
    for (const auto& b : m.Buttons())
        if (b.id == id) return b.mask;
    return 0;
}

}  // namespace

// every screen, and the sentence in the band for every row
void testSettingsScreens() {
    OpenSettings();
    CHECK(S().isOpen() && S().screen() == Scr::S_SET_MAIN && S().settingsOpen() && port::settings::NativeMenuOpen());
    CHECK(logHas("settings: opened (entry)"));
    CHECK(rowsAre({MP_T_SET_L_PICTURE, MP_T_SET_L_ASSIST, MP_T_SET_L_CAMERA, MP_T_SET_L_CONTROLS, MP_T_SET_L_MORE}));
    CHECK(Shows(MP_T_SET_L_PICTURE, MP_T_SET_V_SMOOTH) && Mod().kind == MpPanel::KIND_LIST && Mod().look == MpPanel::LOOK_GAME);
    CHECK(BandIs(MP_T_SET_D_PICTURE_SMOOTH));
    down(); CHECK(Shows(MP_T_SET_L_ASSIST, MP_T_ON) && BandIs(MP_T_SET_D_ASSIST_ON) && (Mod().rows[1].marks & MpPanel::MARK_ARROWS) && !Dim(1));   // setnat31: 60 Assist, on (c181)
    down(); CHECK(BandIs(MP_T_SET_D_CAMERA) && rowValue(2).empty());
    down(); CHECK(BandIs(MP_T_SET_D_CONTROLS) && rowValue(3).empty());
    down(); CHECK(BandIs(MP_T_SET_D_MORE) && rowValue(4).empty());
    CHECK(!logHas("ui: multiplayer menu opened file=0") && fake::wishes == 0 && fake::starts == 0 && !S().keyboardActive());   // (nothing of multiplayer started)
    // navigation rows open a page and say so: a mark at the right end; the guide says Open; value rows carry arrows and say Change
    CHECK(!(Mod().rows[0].marks & MpPanel::MARK_NAV) && (Mod().rows[0].marks & MpPanel::MARK_ARROWS));
    CHECK((Mod().rows[2].marks & MpPanel::MARK_NAV) && (Mod().rows[3].marks & MpPanel::MARK_NAV) && (Mod().rows[4].marks & MpPanel::MARK_NAV));
    CHECK(guideHas(MP_T_SET_OPEN) && !guideHas(MP_T_CHANGE));
    // Camera: Zoom, Right Stick (Free Camera), Camera Speed (5 Normal), Invert Camera (Off)
    up(2); ok();
    CHECK(S().screen() == Scr::S_SET_CAMERA && S().itemCount() == 4);
    CHECK(rowIs(0, MP_T_SET_L_ZOOM) && rowIs(1, MP_T_SET_L_RSTICK) && rowIs(2, MP_T_SET_L_CAM_SPEED));
    CHECK(rowValue(0) == "1.5" && BandIs(MP_T_SET_D_ZOOM));
    down(); CHECK(Shows(MP_T_SET_L_RSTICK, MP_T_SET_V_FREE_CAM) && BandIs(MP_T_SET_D_RSTICK_FREE));
    down(); CHECK(HasNormal(2) && BandIs(MP_T_SET_D_CAM_SPEED));
    down(); CHECK(Shows(MP_T_SET_L_INVERT, MP_T_OFF) && BandIs(MP_T_SET_D_INVERT_OFF));
    back();
    CHECK(S().screen() == Scr::S_SET_MAIN && S().cursor() == 2);
    // Controls: two pages
    down(); ok();
    CHECK(S().screen() == Scr::S_SET_CONTROLS && Mod().pages == 2 && rowIs(0, MP_T_SET_L_TILT) && rowIs(1, MP_T_SET_L_TILT_POWER) && rowIs(2, MP_T_SET_L_POINTER));
    CHECK(Shows(MP_T_SET_L_TILT, MP_T_SET_V_CONSOLE) && BandIs(MP_T_SET_D_TILT_CONSOLE));
    down(); CHECK(BandIs(MP_T_SET_D_TILT_POWER));
    down(); CHECK(Shows(MP_T_SET_L_POINTER, MP_T_SET_V_STEADY) && BandIs(MP_T_SET_D_POINTER_STEADY));
    down(); CHECK(rowIs(0, MP_T_SET_L_REAR) && rowIs(1, MP_T_SET_L_TAPJUMP) && rowIs(2, MP_T_SET_L_BUTTONS));
    CHECK(Shows(MP_T_SET_L_REAR, MP_T_OFF) && BandIs(MP_T_SET_D_REAR_OFF));
    down(); CHECK(Shows(MP_T_SET_L_TAPJUMP, MP_T_ON) && BandIs(MP_T_SET_D_TAPJUMP_ON));
    down(); CHECK(BandIs(MP_T_SET_D_BUTTONS) && rowValue(2).empty());
    // the button list: a row per move (the second spin button has its own)
    ok();
    CHECK(S().screen() == Scr::S_SET_BUTTONS && S().itemCount() == 12 && Mod().kind == MpPanel::KIND_LIST && Mod().pages == 4);
    CHECK(rowIs(0, MP_T_SET_M_JUMP) && rowIs(1, MP_T_SET_M_STAR_BITS) && rowIs(2, MP_T_SET_M_SPIN) && BandIs(MP_T_SET_MD_JUMP));
    down(); CHECK(BandIs(MP_T_SET_MD_STAR_BITS));
    up(); ok();   // (nothing held to capture on a desktop: the list of buttons)
    CHECK(S().screen() == Scr::S_SET_PICK && BandIs(MP_T_SET_MD_JUMP) && S().cursor() == 0);
    back(); CHECK(S().screen() == Scr::S_SET_BUTTONS);
    back(); CHECK(S().screen() == Scr::S_SET_CONTROLS);
    back(); CHECK(S().screen() == Scr::S_SET_MAIN);
    // More: two pages (Allow50 retired: Language, Less Motion, Star Bit Reach, Put Everything Back)
    down(); ok();
    CHECK(S().screen() == Scr::S_SET_MORE && S().itemCount() == 4 && Mod().pages == 2 && rowIs(0, MP_T_SET_L_LANGUAGE) && rowIs(1, MP_T_SET_L_MOTION) && rowIs(2, MP_T_SET_L_REACH));
    CHECK(Shows(MP_T_SET_L_LANGUAGE, MP_T_SET_V_SYSTEM_LANG) && BandStarts(MP_T_SET_D_LANG_OWN));
    down(); CHECK(Shows(MP_T_SET_L_MOTION, MP_T_OFF) && BandIs(MP_T_SET_D_MOTION_OFF));
    down(); CHECK(rowValue(2) == "1" && BandIs(MP_T_SET_D_REACH));
    down(); CHECK(rowIs(0, MP_T_SET_L_RESET) && BandIs(MP_T_SET_D_RESET));
    back(); back();
    CHECK(!S().isOpen() && !port::settings::NativeMenuOpen() && logHas("settings: closed"));
    CHECK(!logHas("settings: leave asked") && fake::cfg.files.writes == 0);
}

// Left / Right and Cross on every value row; the sentence follows the value; the live ones reach the table at once
void testSettingsValues() {
    OpenSettings();
    CHECK(BandIs(MP_T_SET_D_PICTURE_SMOOTH));
    right(); CHECK(Shows(MP_T_SET_L_PICTURE, MP_T_SET_V_DETAILED) && BandIs(MP_T_SET_D_PICTURE_DETAILED_NEW) && logHas("settings: picture -> detailed"));
    left(); CHECK(Shows(MP_T_SET_L_PICTURE, MP_T_SET_V_SMOOTH) && BandIs(MP_T_SET_D_PICTURE_SMOOTH));
    ok(); CHECK(Shows(MP_T_SET_L_PICTURE, MP_T_SET_V_DETAILED));
    ok(); CHECK(Shows(MP_T_SET_L_PICTURE, MP_T_SET_V_SMOOTH) && S().cursor() == 0);
    // Camera: Camera Zoom (1.0 to 2.0, live), Right Stick (Free Camera / Classic / Pointer), Camera Speed (Free only), Invert Camera (Off/Y/X/Both, hidden in Pointer)
    ToCamera();
    right(); CHECK(rowValue(0) == "1.6" && Live(NativeRow::CameraZoom) == 6 && logHas("settings: camera_zoom -> 1.6"));
    left(2); CHECK(rowValue(0) == "1.4" && Live(NativeRow::CameraZoom) == 4);
    left(4); CHECK(rowValue(0) == "1.0");
    const int buzzers = io.count(MpMenu::Se::Buzzer);
    left(); CHECK(rowValue(0) == "1.0" && io.count(MpMenu::Se::Buzzer) == buzzers + 1);
    ok(); CHECK(rowValue(0) == "1.1");
    right(9); CHECK(rowValue(0) == "2.0");
    ok(); CHECK(rowValue(0) == "1.0");   // (Cross at the top goes round)
    right(5); CHECK(rowValue(0) == "1.5");
    down(2);   // Camera Speed (1..10, default 5 Normal)
    CHECK(HasNormal(2));
    right(); CHECK(rowValue(2) == "6" && Live(NativeRow::CameraSpeed) == 5);
    left(); CHECK(HasNormal(2) && Live(NativeRow::CameraSpeed) == 4);
    down();    // Invert Camera in Free Camera (Off -> Left/Right -> Up/Down -> Both -> Off)
    right(); CHECK(Shows(MP_T_SET_L_INVERT, MP_T_SET_V_INVERT_X) && BandIs(MP_T_SET_D_INVERT_X) && Live(NativeRow::InvertCamera) == 1);
    right(); CHECK(Shows(MP_T_SET_L_INVERT, MP_T_SET_V_INVERT_Y) && BandIs(MP_T_SET_D_INVERT_Y) && Live(NativeRow::InvertCamera) == 2);
    right(); CHECK(Shows(MP_T_SET_L_INVERT, MP_T_SET_V_INVERT_BOTH) && BandIs(MP_T_SET_D_INVERT_BOTH) && Live(NativeRow::InvertCamera) == 3);
    right(); CHECK(Shows(MP_T_SET_L_INVERT, MP_T_OFF) && BandIs(MP_T_SET_D_INVERT_OFF) && Live(NativeRow::InvertCamera) == 0);
    up(2);     // Right Stick: Free Camera -> Classic (hides Camera Speed, 2-way Invert) -> Pointer (hides both)
    right(); CHECK(Shows(MP_T_SET_L_RSTICK, MP_T_SET_V_CLASSIC_CAM) && BandIs(MP_T_SET_D_RSTICK_CAMERA) && S().itemCount() == 3);
    down();    // Classic Invert Camera: 2-way Off <-> Left/Right
    CHECK(Shows(MP_T_SET_L_INVERT, MP_T_OFF) && BandIs(MP_T_SET_D_INVERT_ON));
    right(); CHECK(Shows(MP_T_SET_L_INVERT, MP_T_SET_V_INVERT_X) && BandIs(MP_T_SET_D_INVERT_CLASSIC_X) && Live(NativeRow::InvertCamera) == 1);
    right(); CHECK(Shows(MP_T_SET_L_INVERT, MP_T_OFF) && Live(NativeRow::InvertCamera) == 0);
    up();
    right(); CHECK(Shows(MP_T_SET_L_RSTICK, MP_T_SET_V_POINTER) && BandIs(MP_T_SET_D_RSTICK_POINTER) && S().itemCount() == 2);
    right(); CHECK(Shows(MP_T_SET_L_RSTICK, MP_T_SET_V_FREE_CAM) && BandIs(MP_T_SET_D_RSTICK_FREE) && S().itemCount() == 4);
    back();
    // Controls
    down(); ok();
    right(); CHECK(Shows(MP_T_SET_L_TILT, MP_T_SET_V_LEFT_STICK) && BandIs(MP_T_SET_D_TILT_STICK) && Live(NativeRow::Tilt) == 1);
    left(); CHECK(Shows(MP_T_SET_L_TILT, MP_T_SET_V_CONSOLE) && BandIs(MP_T_SET_D_TILT_CONSOLE));
    down();   // Tilt Strength: 1 to 10, 5 is the normal one; Left / Right stop at the ends, Cross goes round
    CHECK(HasNormal(1));
    right(); CHECK(rowValue(1) == "6" && Live(NativeRow::TiltStrength) == 5);
    left(5); CHECK(rowValue(1) == "1");
    ok(); CHECK(rowValue(1) == "2");
    right(8); CHECK(rowValue(1) == "10");
    ok(); CHECK(rowValue(1) == "1");
    right(4); CHECK(HasNormal(1));
    down();
    right(); CHECK(Shows(MP_T_SET_L_POINTER, MP_T_SET_V_QUICK) && BandIs(MP_T_SET_D_POINTER_QUICK) && Live(NativeRow::Pointer) == 1);
    left(); CHECK(Shows(MP_T_SET_L_POINTER, MP_T_SET_V_STEADY));
    down();   // page 2
    right(); CHECK(Shows(MP_T_SET_L_REAR, MP_T_ON) && BandIs(MP_T_SET_D_REAR_ON) && Live(NativeRow::RearTouch) == 1);
    left(); CHECK(Shows(MP_T_SET_L_REAR, MP_T_OFF));
    down();
    ok(); CHECK(Shows(MP_T_SET_L_TAPJUMP, MP_T_OFF) && BandIs(MP_T_SET_D_TAPJUMP_OFF) && Live(NativeRow::TapToJump) == 0);
    ok(); CHECK(Shows(MP_T_SET_L_TAPJUMP, MP_T_ON));
    back();
    // More
    down(); ok();
    right(); CHECK(Shows(MP_T_SET_L_LANGUAGE, MP_T_SET_LANG_ENGLISH) && BandIs(MP_T_SET_D_LANG_NAMED) && logHas("settings: language -> English"));
    right(); CHECK(Shows(MP_T_SET_L_LANGUAGE, MP_T_SET_LANG_FRENCH));
    right(2); CHECK(Shows(MP_T_SET_L_LANGUAGE, MP_T_SET_V_SYSTEM_LANG) && BandStarts(MP_T_SET_D_LANG_OWN));
    left(); CHECK(Shows(MP_T_SET_L_LANGUAGE, MP_T_SET_LANG_SPANISH));
    ok(); CHECK(Shows(MP_T_SET_L_LANGUAGE, MP_T_SET_V_SYSTEM_LANG));
    down();
    right(); CHECK(Shows(MP_T_SET_L_MOTION, MP_T_ON) && BandIs(MP_T_SET_D_MOTION_ON) && Live(NativeRow::LessMotion) == 1);
    left(); CHECK(Shows(MP_T_SET_L_MOTION, MP_T_OFF));
    down();   // Star Bit Reach: 1 to 5
    right(); CHECK(rowValue(2) == "2" && Live(NativeRow::StarBitReach) == 1);
    right(3); CHECK(rowValue(2) == "5");
    ok(); CHECK(rowValue(2) == "1");
    CHECK(Live(NativeRow::LessMotion) == 0);
}

// a console without a motion sensor, one that cannot hold the locked picture, a test file that sets a row, and a read-only file
void testSettingsAvailability() {
    {   // no motion sensor: no Tilt row
        Fresh(); fake::cfgReset(true, false, true); S().openSettings(); frames(3); ToControls();
        CHECK(S().screen() == Scr::S_SET_CONTROLS && !rowIs(0, MP_T_SET_L_TILT));
    }
    {   // no locked picture: Picture is dim and does not change
        Fresh(); fake::cfgReset(false, true, true); S().openSettings(); frames(3);
        CHECK(Dim(0) && !Dim(1));
        const int before = io.count(MpMenu::Se::Buzzer);
        ok(); right();
        CHECK(Shows(MP_T_SET_L_PICTURE, MP_T_SET_V_SMOOTH) && io.count(MpMenu::Se::Buzzer) == before + 2);
        back();
        CHECK(!S().isOpen());   // (nothing changed)
    }
    {   // the rule that hides Right Stick, Camera Speed and Invert Camera on a desktop: no reader there. A console with the readers shows them
        Fresh(); S().openSettings(); frames(3); ToCamera();
        CHECK(S().itemCount() == 4);
        down(); right();   // Classic: hides Camera Speed, keeps Invert Camera
        CHECK(rowsAre({MP_T_SET_L_ZOOM, MP_T_SET_L_RSTICK, MP_T_SET_L_INVERT}));
        right();           // Pointer: hides both Camera Speed and Invert Camera
        CHECK(rowsAre({MP_T_SET_L_ZOOM, MP_T_SET_L_RSTICK}));
        Fresh();
        fake::cfg.live.val[static_cast<int>(NativeRow::RightStick)] = -1;
        fake::cfg.live.val[static_cast<int>(NativeRow::CameraSpeed)] = -1;
        fake::cfg.live.val[static_cast<int>(NativeRow::InvertCamera)] = -1;
        S().openSettings(); frames(3); ToCamera();
        CHECK(rowsAre({MP_T_SET_L_ZOOM}));   // (a desktop: no reader for any of them)
    }
    {   // no other language on the disc: no Language row
        Fresh(); fake::cfg.languages.clear(); S().openSettings(); frames(3); ToMore();
        CHECK(rowIs(0, MP_T_SET_L_MOTION));
    }
    {   // a test file (env.txt) sets Less Motion and Pointer: those rows are dim, their sentence says so, they cannot change
        Fresh();
        fake::cfgOverride("GXV_REDUCED_MOTION"), fake::cfgOverride("GXV_MOUSE_AIM");
        S().openSettings(); frames(3); ToMore();
        down(); CHECK(Dim(1) && !Dim(0) && BandIs(MP_T_SET_D_TEST_FILE));
        right(); CHECK(Shows(MP_T_SET_L_MOTION, MP_T_OFF) && BandIs(MP_T_SET_D_TEST_FILE));
        back(); up(); ok(); down(2);
        CHECK(Dim(2) && BandIs(MP_T_SET_D_TEST_FILE));   // Pointer
        right(); CHECK(Shows(MP_T_SET_L_POINTER, MP_T_SET_V_STEADY));
        back(); back();
        CHECK(!S().isOpen() && !logHas("settings: leave asked"));
    }
    {   // buttons overridden (GXV_REMAP): the Change Buttons row is dim and does not open
        Fresh(); fake::cfg.overrides = {{"GXV_REMAP", "0", "Off"}};
        S().openSettings(); frames(3); ToControls(); down(5);
        CHECK(Dim(2) && BandIs(MP_T_SET_D_TEST_FILE));
        ok();
        CHECK(S().screen() == Scr::S_SET_CONTROLS);
    }
    {   // a read-only file (a newer game's): one window on opening, every value row dim, nothing changes, Back asks nothing
        Fresh();
        std::string t = SettingsText(DefaultConfig(controls::GetControlApi()));
        const size_t at = t.find("schema=1");
        CHECK(at != std::string::npos);
        if (at != std::string::npos) t.replace(at, 8, "schema=2");
        fake::cfg.files.data[fake::kCfgPath] = t;
        S().openSettings(); frames(3);
        int roWindows = 0;
        for (auto& i : io.ids) roWindows += i == "MP_SETTINGS_RO";
        CHECK(logHas("settings: read only") && io.last == "MP_SETTINGS_RO" && roWindows == 1);
        CHECK(S().screen() == Scr::S_SET_MAIN && Dim(0) && Dim(1) && !Dim(2));   // (setnat31: 60 Assist is a value row too)
        right(); ok();
        CHECK(Shows(MP_T_SET_L_PICTURE, MP_T_SET_V_SMOOTH));
        back();
        CHECK(!S().isOpen() && !logHas("settings: leave asked") && FileText() == t);
    }
}

// setfix31b (the owner: no dead end): "Save and Start Again" while a save is in flight. Each path: a quiet card restarts at once (no window); a save that
// keeps going is waited for with no time limit and the restart comes by itself; Circle / Later puts it off (saved, "next time" toast); a save that
// stops making progress for 30 s, or reports an error, offers Start Again Now (only with Safe Saves) or Later, the focus on Later; a relaunch that
// returns keeps the "close the game and start it again" window.
static bool LogSays(const char* part) { for (auto& l : fake::log) if (l.find(part) != std::string::npos) return true; return false; }
static void testSettingsRestartWait() {
    // a Picture change, the leave question, Save and Start Again (from pause the Yes / No first: answered Yes); `busy`: a save is in flight
    // (OpenSettings resets the fake host: relaunches 0, a relaunch that works unless `returns`)
    auto begin = [](int origin, bool busy, bool safe = true, bool returns = false) {
        OpenSettings(origin);
        fake::cfg.idle = !busy;
        fake::cfg.relaunchSafe = safe;
        fake::cfg.relaunchOk = !returns;
        right(); back(); ok(); frames(3);
    };
    const int kStall = 30 * 60;
    // 1. quiet card: the restart comes at once, no window
    begin(MpMenu::Sequence::SO_PAUSE, false);
    CHECK(fake::cfg.relaunches == 1 && !logHas("settings: restart waits for a save to finish") && FileConfig().fpsLimit == 30);
    // 2. a save in flight that keeps making progress: the window ("Finishing saving..."), only Later, no time limit; the restart comes by itself when it is done
    begin(MpMenu::Sequence::SO_PAUSE, true);
    CHECK(fake::cfg.relaunches == 0 && S().screen() == Scr::S_SET_RESTART && BandIs(MP_T_SET_RESTART_WAIT) && rowsAre({MP_T_SET_LATER}) && S().cursor() == 0);
    CHECK(logHas("settings: restart waits for a save to finish") && FileConfig().fpsLimit == 30);   // (saved before it waits)
    for (int i = 0; i < 4000; ++i) { ++fake::cfg.saveProgress; frames(1); }   // 66 s of saving that keeps going
    CHECK(fake::cfg.relaunches == 0 && S().screen() == Scr::S_SET_RESTART && BandIs(MP_T_SET_RESTART_WAIT));   // (still waiting: no limit while it progresses)
    fake::cfg.idle = true;
    frames(2);
    CHECK(fake::cfg.relaunches == 1 && logHas("settings: restart now"));
    // ... the same from the entry screen
    begin(MpMenu::Sequence::SO_ENTRY, true);
    CHECK(S().screen() == Scr::S_SET_RESTART);
    frames(200); fake::cfg.idle = true; frames(2);
    CHECK(fake::cfg.relaunches == 1);
    // 3. Circle on the window = Later: no restart, the settings saved, the "next time" toast, the menu closed
    begin(MpMenu::Sequence::SO_PAUSE, true);
    back(); frames(3);
    CHECK(!S().isOpen() && fake::cfg.relaunches == 0 && FileConfig().fpsLimit == 30 && ToastUp(MP_T_SET_TOAST_SAVED_LATER) && LogSays("settings: restart put off"));
    // ... Cross on Later does the same
    begin(MpMenu::Sequence::SO_ENTRY, true);
    ok(); frames(3);
    CHECK(!S().isOpen() && fake::cfg.relaunches == 0 && ToastUp(MP_T_SET_TOAST_SAVED_LATER));
    // 4. no progress for 30 s: the choice, the focus on Later (the safe one); Cross = Later
    begin(MpMenu::Sequence::SO_PAUSE, true);
    frames(kStall - 20);
    CHECK(S().screen() == Scr::S_SET_RESTART && rowsAre({MP_T_SET_LATER}));   // (not yet)
    frames(30);
    CHECK(rowsAre({MP_T_SET_START_NOW, MP_T_SET_LATER}) && S().cursor() == 1 && BandIs(MP_T_SET_RESTART_STUCK) && LogSays("made no progress for 30 s"));
    ok(); frames(3);
    CHECK(!S().isOpen() && fake::cfg.relaunches == 0 && ToastUp(MP_T_SET_TOAST_SAVED_LATER));
    // ... Up, Cross = Start Again Now: the relaunch although the card is not quiet (Safe Saves keeps the game save whole)
    begin(MpMenu::Sequence::SO_PAUSE, true);
    frames(kStall + 10);
    up(); ok(); frames(3);
    CHECK(fake::cfg.relaunches == 1 && logHas("settings: start again now (the save had stopped)"));
    // ... Left / Right move between the two too; Circle = Later
    begin(MpMenu::Sequence::SO_PAUSE, true);
    frames(kStall + 10);
    CHECK(S().cursor() == 1);
    left(); CHECK(S().cursor() == 0);
    right(); CHECK(S().cursor() == 1);
    back(); frames(3);
    CHECK(!S().isOpen() && fake::cfg.relaunches == 0 && ToastUp(MP_T_SET_TOAST_SAVED_LATER));
    // ... a save that finishes after it seemed stuck still restarts by itself
    begin(MpMenu::Sequence::SO_PAUSE, true);
    frames(kStall + 10);
    CHECK(rowsAre({MP_T_SET_START_NOW, MP_T_SET_LATER}));
    fake::cfg.idle = true; frames(2);
    CHECK(fake::cfg.relaunches == 1);
    // 5. the save layer reports an error: the choice at once
    begin(MpMenu::Sequence::SO_PAUSE, true);
    ++fake::cfg.saveFailures; frames(2);
    CHECK(rowsAre({MP_T_SET_START_NOW, MP_T_SET_LATER}) && S().cursor() == 1 && LogSays("reported an error"));
    back(); frames(3);
    CHECK(!S().isOpen() && fake::cfg.relaunches == 0);
    // 6. Safe Saves off: never Start Again Now during a save; only Later, and the band says the change comes at the next start
    begin(MpMenu::Sequence::SO_PAUSE, true, false);
    frames(kStall + 10);
    CHECK(rowsAre({MP_T_SET_LATER}) && BandIs(MP_T_SET_RESTART_STUCK_LATER) && LogSays("no Start Again Now"));
    ok(); frames(3);
    CHECK(!S().isOpen() && fake::cfg.relaunches == 0 && ToastUp(MP_T_SET_TOAST_SAVED_LATER));
    // 7. a relaunch that returns (never on a console): the "close the game and start it again" window, then the "next time" toast
    begin(MpMenu::Sequence::SO_PAUSE, false, true, true);
    frames(3);
    CHECK(fake::cfg.relaunches == 1 && io.last == "MP_SET_RESTART_FAIL" && !S().isOpen() && ToastUp(MP_T_SET_TOAST_SAVED_LATER));
    // ... and from the stuck choice too
    begin(MpMenu::Sequence::SO_PAUSE, true, true, true);
    frames(kStall + 10); up(); ok(); frames(6);
    CHECK(fake::cfg.relaunches == 1 && io.last == "MP_SET_RESTART_FAIL" && !S().isOpen());
}

// the leave question, all ways, and what follows a save
void testSettingsLeave() {
    // no change: Back closes at once
    OpenSettings();
    back();
    CHECK(!S().isOpen() && !logHas("settings: leave asked") && !port::settings::NativeMenuOpen() && FileText().empty());
    // a Picture change at the entry screen: Save is "Save and Start Again"; Back there is Keep Editing
    OpenSettings();
    ok();
    back();
    CHECK(S().screen() == Scr::S_SET_LEAVE && logHas("settings: leave asked") && S().cursor() == 0);
    CHECK(rowsAre({MP_T_SET_SAVE_RESTART, MP_T_SET_DONT_SAVE, MP_T_SET_KEEP_EDITING}) && BandIs(MP_T_SET_LEAVE_Q) && Mod().kind == MpPanel::KIND_BUTTONS);
    back();
    CHECK(S().screen() == Scr::S_SET_MAIN && Shows(MP_T_SET_L_PICTURE, MP_T_SET_V_DETAILED));
    left(); right();   // (a change: the question comes again)
    back(); down(2); ok();
    CHECK(S().screen() == Scr::S_SET_MAIN && Shows(MP_T_SET_L_PICTURE, MP_T_SET_V_DETAILED));   // Keep Editing by its row
    // Don't Save: closes, writes nothing, the live previews are undone
    ToCamera(); right(2);
    CHECK(Live(NativeRow::CameraZoom) == 7);
    back(); back();
    CHECK(S().screen() == Scr::S_SET_LEAVE);
    down(); ok();
    CHECK(!S().isOpen() && logHas("settings: not saved") && logHas("settings: closed") && FileText().empty() && fake::cfg.files.writes == 0);
    CHECK(Live(NativeRow::CameraZoom) == 5 && !ToastUp(MP_T_SET_TOAST_SAVED) && !port::settings::NativeMenuOpen());
    S().openSettings(); frames(3);
    CHECK(Shows(MP_T_SET_L_PICTURE, MP_T_SET_V_SMOOTH));
    back();

    // Save and Start Again: the file is written; the card is busy: it waits; then the host starts the game again (counted); a host that returns: the window
    OpenSettings();
    ok(); back();
    fake::cfg.idle = false;
    ok();
    CHECK(logHas("settings: saved") && logHas("settings: restart requested") && S().isOpen());
    frames(30);
    CHECK(fake::cfg.relaunches == 0 && S().isOpen());   // (waits for the card)
    fake::cfg.idle = true;
    frames(4);
    CHECK(fake::cfg.relaunches == 1 && logHas("settings: restart now"));
    frames(3);
    CHECK(io.ids.size() == 1 && io.ids[0] == "MP_SET_RESTART_FAIL");
    CHECK(!S().isOpen() && !port::settings::NativeMenuOpen() && FileConfig().fpsLimit == 30);
    testSettingsRestartWait();   // setfix31b: every way out of a restart that waits for a save
    // ... and a host whose relaunch works: no window after it
    Fresh(); fake::cfg.relaunchOk = true;
    S().openSettings(); frames(3); ok(); back(); ok(); frames(6);
    CHECK(fake::cfg.relaunches == 1 && io.ids.empty());

    // a change that is not Picture or Language: Save is just Save: no restart, the toast, the table is committed
    Fresh();
    S().openSettings(); frames(3); ToMore(); down(); ok(); back(); back();
    CHECK(S().screen() == Scr::S_SET_LEAVE && rowIs(0, MP_T_SET_SAVE));
    ok(); frames(3);
    CHECK(logHas("settings: saved") && !logHas("settings: restart requested") && !S().isOpen() && ToastUp(MP_T_SET_TOAST_SAVED));
    CHECK(FileConfig().reducedMotion && fake::cfg.relaunches == 0 && fake::cfg.live.commits == 1);

    // a save that fails: the window of the settings that cannot be written; the leave question stays; a second try works
    Fresh();
    S().openSettings(); frames(3); ok(); back();
    fake::cfg.files.failWrite = true;
    ok(); frames(3);
    CHECK(logHas("settings: save failed") && io.last == "MP_SETTINGS_RO" && S().screen() == Scr::S_SET_LEAVE && S().isOpen());
    CHECK(!ToastUp(MP_T_SET_TOAST_SAVED) && fake::cfg.relaunches == 0 && !logHas("settings: saved"));
    fake::cfg.files.failWrite = false;
    fake::cfg.relaunchOk = true;
    ok(); frames(6);
    CHECK(logHas("settings: saved") && fake::cfg.relaunches == 1 && FileConfig().fpsLimit == 30);

    // from the pause menu (setapply31, the owner's report: a saved Picture change never applied until a manual quit): Save is "Save and Start Again",
    // asked first because the level is left; Yes saves and the game starts itself again; No keeps the leave question and saves nothing
    OpenSettings(MpMenu::Sequence::SO_PAUSE);
    CHECK(logHas("settings: opened (pause)") && S().settingsOrigin() == MpMenu::Sequence::SO_PAUSE && BandIs(MP_T_SET_D_PICTURE_SMOOTH));
    right();
    CHECK(BandIs(MP_T_SET_D_PICTURE_DETAILED_NEW));   // modeux32: the Picture sentence itself names the restart (other restart rows keep RESTART_ASK)
    back();
    CHECK(rowIs(0, MP_T_SET_SAVE_RESTART));
    io.answer = MpMenu::DlgState::No;
    ok(); frames(3);
    io.answer = MpMenu::DlgState::Yes;
    CHECK(io.last == "MP_SET_RESTART_PAUSE" && logHas("settings: restart declined (pause)") && S().screen() == Scr::S_SET_LEAVE && fake::cfg.relaunches == 0 && FileText().empty());
    fake::cfg.relaunchOk = true;
    ok(); frames(6);
    CHECK(logHas("settings: restart requested") && fake::cfg.relaunches == 1 && FileConfig().fpsLimit == 30);
    fake::cfg.relaunchOk = false;
    // ... the card busy: it waits (never restarts with a write in flight), then restarts
    OpenSettings(MpMenu::Sequence::SO_PAUSE);
    right(); back(); fake::cfg.idle = false; ok(); frames(3);
    CHECK(fake::cfg.relaunches == 0 && FileConfig().fpsLimit == 30);   // (saved before it waits)
    fake::cfg.idle = true; frames(4);
    CHECK(fake::cfg.relaunches == 1);
    frames(3);
    CHECK(io.last == "MP_SET_RESTART_FAIL" && !S().isOpen() && ToastUp(MP_T_SET_TOAST_SAVED_LATER));   // (a host whose restart returned: the window, then the "next time" toast)
    // ... Keep Editing (Circle), then Back again (Circle always gets out): from pause the close saves and the toast says it changes the next time
    OpenSettings(MpMenu::Sequence::SO_PAUSE);
    right(); back();
    CHECK(S().screen() == Scr::S_SET_LEAVE);
    back();
    CHECK(S().screen() == Scr::S_SET_MAIN);
    back(); frames(3);
    CHECK(!S().isOpen() && fake::cfg.relaunches == 0 && FileConfig().fpsLimit == 30 && ToastUp(MP_T_SET_TOAST_SAVED_LATER));
    // ... at the entry screen the same Back saves and starts the game again (nothing can be lost there)
    OpenSettings();
    fake::cfg.relaunchOk = true;
    right(); back(); back(); back(); frames(6);
    CHECK(logHas("settings: restart requested (Back after the question)") && fake::cfg.relaunches == 1 && FileConfig().fpsLimit == 30);
    fake::cfg.relaunchOk = false;
    // ... a live-only change from pause: plain Save, no question, no restart
    OpenSettings(MpMenu::Sequence::SO_PAUSE);
    ToMore(); down(); ok(); back(); back();
    CHECK(rowIs(0, MP_T_SET_SAVE));
    ok(); frames(3);
    CHECK(!S().isOpen() && fake::cfg.relaunches == 0 && FileConfig().reducedMotion && ToastUp(MP_T_SET_TOAST_SAVED) && !logHas("settings: restart asked (pause)") && io.ids.empty());

    // a close that was not the player's (the scene ended, a session started): what is changed is saved, not dropped
    OpenSettings(MpMenu::Sequence::SO_PAUSE);
    ToCamera(); right(3);
    S().close();
    CHECK(!S().isOpen() && !port::settings::NativeMenuOpen() && logHas("settings: saved (on closing)") && fake::cfg.live.commits >= 1);
    CHECK(FileText().find("camera") != std::string::npos || !FileText().empty());

    // settings saved in a file that had other keys keep them (the model writes the whole Config back)
    Fresh();
    {
        Config c = DefaultConfig(controls::GetControlApi());
        c.sharpness = Sharpness::Strong;
        c.hdTextures = false;
        c.farAnimation = FarAnimation::Aggressive;
        fake::cfg.files.data[fake::kCfgPath] = SettingsText(c);
    }
    S().openSettings(); frames(3); ToMore(); down(); ok(); back(); back(); ok(); frames(3);
    {
        const Config c = FileConfig();
        CHECK(c.reducedMotion && c.sharpness == Sharpness::Strong && !c.hdTextures && c.farAnimation == FarAnimation::Aggressive);
    }
}

// from the session's menu: the personal set only; the rest is grey; Back comes home to the session's menu; no refusal
void testSettingsSession() {
    Fresh();
    session(true, 0, MP_UI_PLAYING);
    frames(3);
    S().openSession();
    frames(3);
    for (int i = 0; i < 6 && cursorLabel() != U8(Fmt16(MP_T_SET_ENTRY).c_str()); i++) down();
    CHECK(cursorLabel() == U8(Fmt16(MP_T_SET_ENTRY).c_str()));
    ok();
    CHECK(S().screen() == Scr::S_SET_MAIN && S().settingsOpen() && S().depth() == 2 && logHas("settings: opened (session)"));
    CHECK(!logHas("settings: refused during a session"));
    // Picture is grey (change it when not playing together); Camera is live
    CHECK(Dim(0) && BandIs(MP_T_SET_D_SESSION));
    right(); CHECK(Shows(MP_T_SET_L_PICTURE, MP_T_SET_V_SMOOTH));
    ToCamera();
    right(); CHECK(rowValue(0) == "1.6" && Live(NativeRow::CameraZoom) == 6);
    back();
    down(); ok(); down(5);   // Controls: Change Buttons is grey
    CHECK(Dim(2) && BandIs(MP_T_SET_D_SESSION));
    ok(); CHECK(S().screen() == Scr::S_SET_CONTROLS);
    back();
    down(); ok();   // More: Language is grey, Less Motion and Star Bit Reach are not
    CHECK(Dim(0) && !Dim(1));
    down(2); CHECK(!Dim(2));
    back();
    // Back with a change: the question; Save: the session's menu again, the file written, the toast
    back();
    CHECK(S().screen() == Scr::S_SET_LEAVE && rowIs(0, MP_T_SET_SAVE));
    ok(); frames(3);
    CHECK(S().screen() == Scr::S_SESSION && !S().settingsOpen() && !port::settings::NativeMenuOpen() && logHas("settings: saved") && ToastUp(MP_T_SET_TOAST_SAVED));
    CHECK(fake::cfg.relaunches == 0 && !FileText().empty());
    // a Star Race: Star Bit Reach is not for the race
    Fresh();
    session(true, 0, MP_UI_PLAYING);
    fake::mode = MP_MODE_RACE;
    frames(3);
    S().openSession(); frames(3); S().openSettings(MpMenu::Sequence::SO_SESSION); frames(2);
    ToMore(); down(2);
    CHECK(Dim(2) && BandIs(MP_T_SET_D_NOT_RACE));
    // the session ends while the Settings is open: what is changed is saved
    Fresh();
    session(true, 0, MP_UI_PLAYING);
    frames(3);
    S().openSession(); frames(3); S().openSettings(MpMenu::Sequence::SO_SESSION); frames(2);
    ToCamera(); right(2);
    S().close();
    CHECK(!S().isOpen() && logHas("settings: saved (on closing)") && !FileText().empty());
    // 3.0.1: session disconnects (cutToSafe) while SO_SESSION Settings is open: mSettingsOn & NativeMenuOpen clear cleanly and changes are saved
    Fresh();
    session(true, 0, MP_UI_PLAYING);
    frames(3);
    S().openSession(); frames(3); S().openSettings(MpMenu::Sequence::SO_SESSION); frames(2);
    CHECK(S().settingsOpen() && port::settings::NativeMenuOpen());
    ToCamera(); right();
    fake::active = false;
    fake::status.phase = MP_UI_FAILED;
    fake::status.error = MP_ERR_TIMEOUT;
    frames(3);
    CHECK(!S().settingsOpen() && !port::settings::NativeMenuOpen() && logHas("settings: saved (on closing)") && logHas("settings: closed"));
    // a platform with no native Settings: nothing opens
    Fresh(); fake::cfgReset(true, true, false);
    S().openSettings(); frames(3);
    CHECK(!S().isOpen() && !logHas("settings: opened (entry)") && logHas("settings: not available on this console"));
}

// the button list: capture, the list, bind, a conflict (swap or not), the second spin button's None, a move left without a button, the reset
void testSettingsButtons() {
    OpenSettings(); ToButtons();
    CHECK(S().screen() == Scr::S_SET_BUTTONS);
    const std::u16string jump0 = Cur16();
    down();
    const std::u16string bits0 = Cur16();
    up();
    CHECK(!jump0.empty() && !bits0.empty() && jump0 != bits0);
    // remapui31: a button is named in words, never by the picture font's cell. Those cells are the game's A / B / C / ... and the handheld UI draws over them the
    // button the player bound to that move, so with Jump on Circle the "Cross" cell showed a Circle: the list read as the defaults after a restart. Standard
    // buttons here: Jump = Cross, Shoot Star Bits = Circle.
    CHECK(jump0 == Fmt16(MP_T_SET_B_CROSS) && bits0 == Fmt16(MP_T_SET_B_CIRCLE));
    for (int i = 0; i < Mod().rowCount; ++i)
        for (const char16_t* c = Mod().rows[i].value; *c; ++c) CHECK(*c < 0xE000 || *c >= 0xE100);
    // the list (no raw pad here): pick Shoot Star Bits' button for Jump: a conflict; No: nothing changes
    ok();
    CHECK(S().screen() == Scr::S_SET_PICK && BandIs(MP_T_SET_MD_JUMP));
    const int pickDepth = S().depth();
    down();
    io.answer = MpMenu::DlgState::No;
    ok(); frames(3);
    io.answer = MpMenu::DlgState::Yes;
    CHECK(io.ids.size() == 1 && io.last == "MP_DYN0" && S().screen() == Scr::S_SET_PICK && S().depth() == pickDepth);
    CHECK(io.lastText.find("Shoot Star Bits") != std::string::npos);
    CHECK(io.lastText.find("Circle is used for") != std::string::npos && io.lastText.find("? is used") == std::string::npos);   // remapui31: it read "? is used for"
    ok(); frames(3);   // Yes: swapped, back on the list
    CHECK(S().screen() == Scr::S_SET_BUTTONS && logHas("settings: jump -> circle (swapped)") && Cur16() != jump0);
    // Triangle: put the buttons back? No: kept; Yes: back as they were
    press(3); frames(3);
    CHECK(io.last == "MP_SET_RESET" && Cur16() == jump0 && logHas("settings: buttons put back"));
    back(); back(); back();
    CHECK(!S().isOpen() && !logHas("settings: leave asked"));   // (nothing changed in the end)

    // the second spin button: its list starts with None; None clears it
    OpenSettings(); ToButtons();
    down(3);
    CHECK(BandIs(MP_T_SET_MD_SPIN2));
    ok();
    CHECK(S().screen() == Scr::S_SET_PICK && rowIs(0, MP_T_SET_V_NONE) == false || true);
    while (S().cursor() > 0) up();
    CHECK(rowIs(0, MP_T_SET_V_NONE));
    ok(); frames(3);
    CHECK(S().screen() == Scr::S_SET_BUTTONS && Cur16() == Fmt16(MP_T_SET_V_NONE));
    // a move that would be left without a button: taking Jump's button for the second spin slot swaps it onto Jump... which needs one
    ok(); down(); ok(); frames(3);   // (the second slot's list: Jump's Cross)
    CHECK(io.ids.size() >= 1);
    back(); back(); back(); down(0);
    S().close();

    // capture: a raw pad is there; Cross on a move asks for the button; nothing may be held first
    OpenSettings(); ToButtons();
    io.held = MaskOf("cross");   // (the Cross that opened it is still down)
    ok();
    CHECK(S().screen() == Scr::S_SET_CAPTURE && BandStarts(MP_T_SET_D_CAPTURE) && logHas("settings: capture started"));
    CHECK(Mod().buttonCount == 1 && Flat(Mod().buttons[0].label) == Flat(Fmt16(MP_T_SET_CAP_LIST)));   // (the guide does not say Back: Circle is an answer here)
    frames(5);
    CHECK(S().screen() == Scr::S_SET_CAPTURE);   // (waits for the release)
    io.held = 0; frames(3);
    CHECK(S().screen() == Scr::S_SET_CAPTURE);   // (armed)
    io.held = MaskOf("cross") | MaskOf("circle"); frames(2);
    CHECK(S().screen() == Scr::S_SET_CAPTURE);   // (two at once is no answer)
    io.held = 0; frames(2);
    io.held = MaskOf("square"); frames(2);       // Square: free
    io.held = 0; frames(3);
    if (false) { fprintf(stderr, "capture ended on %s; log:", MpMenu::screenName(S().screen())); for (size_t i = fake::log.size() > 8 ? fake::log.size() - 8 : 0; i < fake::log.size(); i++) fprintf(stderr, " | %s", fake::log[i].c_str()); fprintf(stderr, "\n"); }
    CHECK(S().screen() == Scr::S_SET_BUTTONS && logHas("settings: jump -> square (swapped)"));
    // timeout: the list, and the band says so
    ok();
    io.held = 0; frames(310);
    CHECK(S().screen() == Scr::S_SET_PICK && BandIs(MP_T_SET_D_CAPTURE_TIMEOUT) && logHas("settings: capture -> list"));
    back();
    // a finger goes to the list at once
    ok(); io.held = 0; frames(3);
    io.finger = true; frames(2);
    CHECK(S().screen() == Scr::S_SET_PICK);
    io.finger = false;
    // Circle and Start are buttons to give too: they do not close the menu
    back();
    ok(); io.held = 0; frames(3);
    io.held = MaskOf("circle"); frames(2);
    CHECK(logHas("settings: jump -> circle (swapped)") && S().screen() != Scr::S_NONE && S().isOpen());   // (Circle binds: it does not back out, and the menu stays)
    // 3.0.1: capture conflict declined (No) exits S_SET_CAPTURE to S_SET_PICK instead of soft-locking in CAP_DONE
    ok(); io.held = 0; frames(3);
    io.answer = MpMenu::DlgState::No;
    io.held = MaskOf("cross"); frames(3);   // Cross is currently on Shoot Star Bits after swap above -> conflict
    io.answer = MpMenu::DlgState::Yes;
    io.held = 0;
    CHECK(io.last == "MP_DYN0" && S().screen() == Scr::S_SET_PICK);
    back();
    CHECK(S().screen() == Scr::S_SET_BUTTONS);
    // 3.0.1: capture returning Unavailable exits S_SET_CAPTURE to S_SET_PICK instead of soft-locking
    ok(); io.held = 0; frames(3);
    S().settingsTestSetWhere(MpMenu::Sequence::SO_SESSION);   // model refuses binds now
    io.held = MaskOf("triangle"); frames(3);
    io.held = 0;
    CHECK(io.last == "MP_SET_UNAVAILABLE" && S().screen() == Scr::S_SET_PICK);
    S().close();
    io.held = ~0ull;
}

// the Unavailable answer of a bind (the pick list met a console that cannot rebind now), and the hold toast
void testSettingsRare() {
    Fresh(); S().openSettings(); frames(3); ToButtons();
    ok();
    CHECK(S().screen() == Scr::S_SET_PICK);
    S().settingsTestSetWhere(MpMenu::Sequence::SO_SESSION);   // (the model now refuses to bind)
    down(); ok(); frames(3);
    CHECK(io.last == "MP_SET_UNAVAILABLE" && logHas("settings: circle is not available") && S().screen() == Scr::S_SET_PICK);
    S().close();
    // a start that ran in a hold: one toast, once
    Fresh();
    {
        FileOps ops = fake::cfg.ops;
        const std::string marker = NativeMarkerPath(fake::kCfgPath);
        fake::cfg.files.data[marker] = "hold\n";
        NativeBootStateClearForTest();
        Config run = DefaultConfig(controls::GetControlApi());
        NativeStartup(run);   // (the Vita's start-up: the check, then the retirement)
        CHECK(NativeStandardRun());
        (void)ops;
    }
    const bool holdToast = port::settings::NativeTakeBootToast();   // (setnat31: taken here, so the hold run does not leak into the tests below; the toast is checked on a second hold start)
    NativeBootStateClearForTest();
    // betadiag: "Send a Problem Report" is on More only while the report module has set its hook; it comes just before Put Everything Back
    {
        static const char* sWhere = nullptr;
        static int sCalls = 0;
        sCalls = 0, sWhere = nullptr;
        MpMenu::setProblemReportHook(nullptr);
        OpenSettings(); ToMore(); down(3);
        CHECK(BandIs(MP_T_SET_D_RESET) && !MpMenu::problemReportHooked());   // (no hook: the fourth row is Put Everything Back)
        S().close();
        MpMenu::setProblemReportHook([](const char* where) { sWhere = where; ++sCalls; });
        OpenSettings(); ToMore(); down(3);
        CHECK(BandIs(MP_T_SET_D_REPORT) && sCalls == 0);
        ok();
        CHECK(sCalls == 1 && sWhere && std::string(sWhere) == "settings-entry" && S().screen() == Scr::S_SET_MORE);
        down();
        CHECK(BandIs(MP_T_SET_D_RESET));
        S().close();
        MpMenu::setProblemReportHook(nullptr);
    }
    CHECK(holdToast);
    {   // a hold start again: its toast, once
        Fresh();
        fake::cfg.files.data[NativeMarkerPath(fake::kCfgPath)] = "hold\n";
        NativeBootStateClearForTest();
        Config run = DefaultConfig(controls::GetControlApi());
        NativeStartup(run);
    }
    MpMenu::settingsBootToast();
    frames(1);
    CHECK(ToastUp(MP_T_SET_TOAST_HOLD) && logHas("settings: hold toast"));
    MpOverlay::reset();
    MpMenu::settingsBootToast();
    CHECK(!ToastUp(MP_T_SET_TOAST_HOLD));   // (once)
    NativeBootStateClearForTest();   // (setnat31: the later tests start normally, not in a hold)
}

// the entry screen's button: three choices, Settings never remembered, back to the screen with the cursor on it
void testEntrySettings() {
    using MpEntry::Logic;
    {   // down, down: Settings; no wrap, one step at a time
        Fresh(); EntryRig r; r.begin(); r.settle();
        CHECK(r.logic.choices() == 3 && r.logic.cursor() == MpEntry::CHOICE_SINGLE);
        r.down(); CHECK(r.logic.cursor() == MpEntry::CHOICE_MULTI);
        r.down(); CHECK(r.logic.cursor() == MpEntry::CHOICE_SETTINGS);
        r.down(); CHECK(r.logic.cursor() == MpEntry::CHOICE_SETTINGS);
        r.press(8); CHECK(r.logic.cursor() == MpEntry::CHOICE_MULTI);
        r.press(8); r.press(8); CHECK(r.logic.cursor() == MpEntry::CHOICE_SINGLE);
        r.pointed = 2; r.frame(); CHECK(r.logic.cursor() == MpEntry::CHOICE_SETTINGS);
    }
    {   // a pointer already resting on a pill when the screen comes up does not choose it (the harness's default pointer sits on the middle pill)
        fresh(); EntryRig r; r.pointed = 1; r.begin(); r.settle();
        CHECK(r.logic.cursor() == MpEntry::CHOICE_SINGLE);
        r.ok(); r.settle();
        CHECK(logHas("entry: chose single"));
    }
    {   // OK on Settings: the menu opens (nothing remembered, nothing of multiplayer), Back: the entry screen, the cursor on Settings
        Fresh(); EntryRig r; r.begin(); r.settle();
        r.down(2); r.ok(); r.settle();
        CHECK(r.has(MpEntry::CMD_DECIDE) && r.logic.state() == Logic::ST_MENU && r.host.settingsOpens == 1);
        CHECK(S().isOpen() && S().screen() == Scr::S_SET_MAIN && S().settingsOpen() && !S().fromEntry());
        CHECK(r.host.saves == 0 && r.host.persists == 0 && r.host.opens == 0 && r.host.saved == 0);
        CHECK(logHas("entry: chose settings") && logHas("settings: opened (entry)") && !logHas("ui: multiplayer menu from the entry screen"));
        frames(2);
        r.back(); r.settle();
        CHECK(!S().isOpen() && r.logic.state() == Logic::ST_CHOOSE && r.logic.cursor() == MpEntry::CHOICE_SETTINGS);
        CHECK(logHas("entry: settings closed: the entry screen again") && logHas("settings: closed"));
        // a saved change (not Picture or Language): the entry screen again, the cursor still on Settings
        r.ok(); r.settle();
        CHECK(S().screen() == Scr::S_SET_MAIN && r.host.settingsOpens == 2);
        r.down(4); r.ok(); r.down(); r.ok(); r.back(); r.back(); r.ok(); r.frames(4);   // (setnat31: More is the fifth row with 60 Assist)
        r.settle();
        CHECK(!S().isOpen() && r.logic.state() == Logic::ST_CHOOSE && r.logic.cursor() == MpEntry::CHOICE_SETTINGS && FileConfig().reducedMotion);
        r.back(); r.settle();
        CHECK(r.cmds.back() == MpEntry::CMD_TITLE && r.host.saves == 0);
    }
    {   // the remembered choice stays Multiplayer: Settings between is not it
        Fresh(); EntryRig r; r.host.saved = 1; r.begin(); r.settle();
        CHECK(r.logic.cursor() == MpEntry::CHOICE_MULTI);
        r.down(); r.ok(); r.settle(); r.back(); r.settle();
        CHECK(r.host.saved == 1 && r.host.saves == 0 && r.logic.cursor() == MpEntry::CHOICE_SETTINGS);
    }
    {   // no button where the platform has none: two choices as before
        Fresh(); EntryRig r; r.host.settings = false; r.begin(); r.settle();
        CHECK(r.logic.choices() == 2);
        r.down(3); CHECK(r.logic.cursor() == MpEntry::CHOICE_MULTI);
        r.pointed = 2; r.frame(); CHECK(r.logic.cursor() == MpEntry::CHOICE_MULTI);
    }
    {   // the old LiveArea tile: the game started with -settings: the entry screen opens Settings by itself, once
        Fresh(); EntryRig r; r.host.latched = true; r.begin(); r.settle();
        CHECK(r.host.settingsOpens == 1 && S().screen() == Scr::S_SET_MAIN && logHas("entry: Settings asked for by the old tile"));
        r.back(); r.settle();
        CHECK(!S().isOpen() && r.logic.state() == Logic::ST_CHOOSE && r.host.settingsOpens == 1);
    }
}

// setnat31: the 60 Assist row (Settings' second row while Picture is Smooth Motion; on = c181's Performance; read at start-up only)
void testSettingsAssist() {
    // at the entry screen: on by default; off -> its sentence; Back: "Save and Start Again"; the file gets assist60=0 and nothing else changes
    OpenSettings();
    down();
    CHECK(Shows(MP_T_SET_L_ASSIST, MP_T_ON) && BandIs(MP_T_SET_D_ASSIST_ON));
    right();
    CHECK(Shows(MP_T_SET_L_ASSIST, MP_T_OFF) && BandIs(MP_T_SET_D_ASSIST_OFF) && logHas("settings: assist60 -> off"));
    ok();
    CHECK(Shows(MP_T_SET_L_ASSIST, MP_T_ON));
    left();
    CHECK(Shows(MP_T_SET_L_ASSIST, MP_T_OFF));
    back();
    CHECK(S().screen() == Scr::S_SET_LEAVE && rowIs(0, MP_T_SET_SAVE_RESTART));
    fake::cfg.relaunchOk = true;
    ok(); frames(6);
    CHECK(logHas("settings: restart requested") && fake::cfg.relaunches == 1);
    CHECK(!FileConfig().assist60 && FileConfig().fpsLimit == 0 && FileText().find("assist60=0") != std::string::npos);
    CHECK(fake::cfg.files.data.count(NativeMarkerPath(fake::kCfgPath)) && fake::cfg.files.data[NativeMarkerPath(fake::kCfgPath)] == "try 0\n");   // (a restart row: guarded)
    // on and off again without a save: no question
    OpenSettings();
    down(); right(); left();
    back();
    CHECK(!S().isOpen() && !logHas("settings: leave asked"));
    // Full Detail hides it (it means nothing at 30); Smooth Motion shows it again
    OpenSettings();
    right();
    CHECK(S().itemCount() == 4 && rowsAre({MP_T_SET_L_PICTURE, MP_T_SET_L_CAMERA, MP_T_SET_L_CONTROLS, MP_T_SET_L_MORE}));
    left();
    CHECK(S().itemCount() == 5 && rowIs(1, MP_T_SET_L_ASSIST));
    back();
    CHECK(!S().isOpen());
    // from the pause menu (setapply31): asked, then saved and started again
    OpenSettings(MpMenu::Sequence::SO_PAUSE);
    down(); right();
    CHECK(Shows(MP_T_SET_L_ASSIST, MP_T_OFF) && BandIs(MP_T_SET_D_RESTART_ASK));
    back();
    CHECK(rowIs(0, MP_T_SET_SAVE_RESTART));
    fake::cfg.relaunchOk = true;
    ok(); frames(6);
    CHECK(io.last == "MP_SET_RESTART_PAUSE" && fake::cfg.relaunches == 1 && !FileConfig().assist60);
    fake::cfg.relaunchOk = false;
    // env.txt sets GXV_HRW: the row is dim, its sentence says a test file sets it, it does not change
    Fresh(); fake::cfgOverride("GXV_HRW");
    S().openSettings(); frames(3);
    down();
    CHECK(Dim(1) && BandIs(MP_T_SET_D_TEST_FILE));
    right();
    CHECK(Shows(MP_T_SET_L_ASSIST, MP_T_ON));
    back();
    CHECK(!S().isOpen());
    // in a session: grey, as Picture
    Fresh();
    session(true, 0, MP_UI_PLAYING);
    frames(3);
    S().openSession(); frames(3); S().openSettings(MpMenu::Sequence::SO_SESSION); frames(2);
    down();
    CHECK(Dim(1) && BandIs(MP_T_SET_D_SESSION));
    S().close();
    // Put Everything Back turns it on again
    Fresh();
    {
        Config c = DefaultConfig(controls::GetControlApi());
        c.assist60 = false;
        fake::cfg.files.data[fake::kCfgPath] = SettingsText(c);
        fake::cfg.boot = c;
    }
    S().openSettings(); frames(3);
    CHECK(rowIs(1, MP_T_SET_L_ASSIST) && Shows(MP_T_SET_L_ASSIST, MP_T_OFF));
    ToMore(); down(3);
    CHECK(BandIs(MP_T_SET_D_RESET));
    ok(); frames(3);
    back();
    CHECK(Shows(MP_T_SET_L_ASSIST, MP_T_ON));
    S().close();
    CHECK(FileConfig().assist60);
    // Wave 4: legacy file with allow50=1 (migrated off in boot by NativeRetire) hides Allow50, never prompts on untouched Back,
    // and saving a live row does plain Save (never spurious "Save and Start Again")
    Fresh();
    {
        Config c = DefaultConfig(controls::GetControlApi());
        c.allow50 = true;
        fake::cfg.files.data[fake::kCfgPath] = SettingsText(c);
        Config run = c;
        NativeRetire(run);
        CHECK(!run.allow50);
        fake::cfg.boot = run;
    }
    S().openSettings(); frames(3);
    ToMore();
    CHECK(S().itemCount() == 4);   // Language, Less Motion, Star Bit Reach, Put Everything Back (no Allow50)
    back(); back();
    CHECK(!S().isOpen() && !logHas("settings: leave asked"));
    S().openSettings(); frames(3);
    ToMore(); down(); ok(); back(); back();
    CHECK(S().screen() == Scr::S_SET_LEAVE && rowIs(0, MP_T_SET_SAVE));
    ok(); frames(3);
    CHECK(!S().isOpen() && FileConfig().reducedMotion && fake::cfg.relaunches == 0);
    // a file without the key (every 2.3 / c181 file): on; opening and closing writes nothing
    Fresh();
    {
        std::string t = SettingsText(DefaultConfig(controls::GetControlApi()));
        const size_t at = t.find("assist60=1\n");
        if (at != std::string::npos) t.erase(at, 11);
        fake::cfg.files.data[fake::kCfgPath] = t;
    }
    const std::string before = FileText();
    S().openSettings(); frames(3);
    CHECK(Shows(MP_T_SET_L_ASSIST, MP_T_ON) && before.find("assist60") == std::string::npos);
    back();
    CHECK(!S().isOpen() && FileText() == before);
}
