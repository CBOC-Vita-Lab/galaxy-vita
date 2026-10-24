// The VR settings panel: a small panel beside the game's pause menu, drawn by
// the VR layer and worked with the pointer (aim the right controller, press
// A or the trigger).  It holds the settings of petari_vr.ini, on four tabs:
// the giant screen (its distance, the room around it, its stereoscopic 3D),
// the diorama (where Mario stands, how the world follows him), the picture
// (SpaceWarp, Super Resolution, sharpening, the render resolution, the
// display's refresh rate) and the game (its language, the camera's turns,
// skipping cutscenes).  A change applies at once (the paused scene behind
// the menu moves), except the two that are read at the start (the language,
// the clock levels), and the settings file is updated when the menu closes:
// only the lines of the settings changed here, so the others keep following
// the defaults.  In the headset the panel goes out as a compositor layer of
// its own (vr::uiLayer), sharp whatever the eye images' size.
//
// The panel is drawn on the CPU into a texture whenever something on it
// changes: rounded shapes with anti-aliased edges and text from the baked
// glyph atlas (ui_canvas.h).
#include <GLES3/gl32.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "port/heap_routing.h"
#include "port/port.h"
#include "ui_canvas.h"
#include "vr_renderer.h"

namespace {

using ui::Canvas;
using ui::Color;
using ui::kFontLarge;
using ui::kFontSmall;
using ui::rgb;

// ---------------------------------------------------------------------------
// The settings on the panel
// ---------------------------------------------------------------------------
enum Kind {
    kSwitch,   // on / off
    kStepper,  // - value +
    kSlider,   // - track +, the value above (two rows high)
};

struct Item {
    const char* key;    // the setting (vr::findSetting), and its line in the file
    const char* label;
    const char* hint;
    Kind kind;
    float min, max, step;  // what the panel offers (within the setting's own range)
    const char* format;    // of the value
    // Optional.  enabled: the setting has an effect as things are (dimmed
    // otherwise; it still works).  hintNow: a hint that depends on the state.
    // options: the values to step through, instead of min, max and step.
    // valueText: the value's text, instead of the format.  get, set and
    // fileText: a setting that is no number in the file (the language).
    bool (*enabled)();
    void (*hintNow)(char* out, size_t size);
    int (*options)(float* out, int max);
    void (*valueText)(float value, char* out, size_t size);
    float (*get)();
    void (*set)(float value);
    void (*fileText)(char* out, size_t size);
};

bool setting(const char* key) {
    const vr::Setting* s = vr::findSetting(key);
    return s && s->get() != 0.0f;
}
bool giantOn() { return setting("giant_screen"); }
bool stereoOn() { return giantOn() && setting("stereo_screen"); }
bool passthroughUsable() { return giantOn() && vr::passthroughAvailable(); }
bool sharpenOn() { return setting("sharpening"); }
bool severalLanguages() { return port_language_count() > 1; }

void hintScreenOnly(char* out, size_t size, const char* hint) { snprintf(out, size, "%s", giantOn() ? hint : "Only on the giant screen"); }
void hintPassthrough(char* out, size_t size) {
    if (!vr::passthroughAvailable()) {
        snprintf(out, size, "Not available on this headset");
    } else {
        hintScreenOnly(out, size, "Your room around the screen");
    }
}
void hintStereo(char* out, size_t size) { hintScreenOnly(out, size, "A picture for each eye"); }
void hintDiorama(char* out, size_t size) { snprintf(out, size, "%s", giantOn() ? "Used while the giant screen is off" : "How far away Mario stands"); }
void hintScale(char* out, size_t size) {
    if (giantOn()) {
        snprintf(out, size, "The screen's picture is at %.2f now", roundf(vr::screenPictureScale() * 100.0f) / 100.0f);
    } else {
        snprintf(out, size, "Now %.2f; 1.00 is Meta's standard", roundf(vr::renderScale() * 100.0f) / 100.0f);
    }
}
void hintInvert(char* out, size_t size) {
    snprintf(out, size, "%s", setting("invert_camera") ? "Stick right turns the view right" : "Stick right moves the camera right");
}
void hintLanguage(char* out, size_t size) {
    int running = port_language_running();
    if (!severalLanguages()) {
        snprintf(out, size, "The only one on this disc");
    } else if (running >= 0 && running != port_language_wanted()) {
        snprintf(out, size, "Restart the game to change it");
    } else {
        snprintf(out, size, "The game's texts");
    }
}

int refreshOptions(float* out, int max) {
    int n = vr::refreshRates(out, max);
    if (n == 0) {
        const float usual[] = {72.0f, 90.0f, 120.0f};
        for (float r : usual) {
            if (n < max) out[n++] = r;
        }
    }
    return n;
}

int languageOptions(float* out, int max) {
    int n = port_language_count();
    n = n < max ? n : max;
    for (int i = 0; i < n; i++) out[i] = (float)i;
    return n;
}
void languageText(float value, char* out, size_t size) {
    snprintf(out, size, "%s", port_language_name((int)lroundf(value)));
    out[0] = (char)toupper((unsigned char)out[0]);
}
float languageGet() { return (float)port_language_wanted(); }
void languageSet(float value) { port_language_set(port_language_name((int)lroundf(value))); }
void languageFile(char* out, size_t size) { snprintf(out, size, "%s", port_language_name(port_language_wanted())); }

void skipText(float value, char* out, size_t size) {
    if (value <= 0.0f) {
        snprintf(out, size, "Off");
    } else {
        snprintf(out, size, "%.2f s", value);
    }
}

const Item kScreenItems[] = {
    {"giant_screen", "Giant screen", "Play on a big screen, no diorama", kSwitch},
    {"screen_distance", "Screen distance", "How far away the giant screen is", kSlider, 2.5f, 10.0f, 0.5f, "%.1f m", giantOn},
    {"passthrough", "Passthrough", nullptr, kSwitch, 0, 0, 0, nullptr, passthroughUsable, hintPassthrough},
    {"stereo_screen", "Stereoscopic 3D", nullptr, kSwitch, 0, 0, 0, nullptr, giantOn, hintStereo},
    {"stereo_depth", "3D depth", "More brings the world out of the screen", kStepper, 0.25f, 3.0f, 0.25f, "%.2f", stereoOn},
    {"stereo_far", "3D far depth", "How far behind the screen the sky is", kStepper, 0.5f, 1.0f, 0.05f, "%.2f", stereoOn},
    {"stereo_resolution", "3D resolution", "Size of each eye's picture", kStepper, 0.5f, 1.0f, 0.05f, "%.2f", stereoOn},
};
const Item kDioramaItems[] = {
    {"diorama_distance", "Diorama distance", nullptr, kSlider, 0.6f, 4.0f, 0.1f, "%.1f m", nullptr, hintDiorama},
    {"diorama_height", "Height", "Mario below your eyes", kStepper, 0.0f, 1.6f, 0.05f, "%.2f m"},
    {"diorama_scale", "World size", "1.0 is the usual size", kStepper, 0.5f, 3.0f, 0.1f, "%.1f"},
    {"follow_smoothing", "Follow smoothing", "How gently the world follows Mario", kStepper, 0.0f, 0.5f, 0.02f, "%.2f s"},
    {"turn_smoothing", "Turn smoothing", "How gently it turns with gravity", kStepper, 0.0f, 1.5f, 0.05f, "%.2f s"},
    {"vignette", "Turn vignette", "Darker edges while the world turns", kStepper, 0.0f, 1.0f, 0.1f, "%.1f"},
    {"cutaway", "See through scenery", "Fade what hides Mario from you", kSwitch},
    {"turn_with_camera", "Turn with the camera", "The world swings with the game camera", kSwitch},
};
const Item kPictureItems[] = {
    {"space_warp", "Smooth motion", "SpaceWarp: 120 frames a second", kSwitch},
    {"super_resolution", "Super resolution", "Sharper upscaling by the headset", kSwitch},
    {"sharpening", "FidelityFX CAS", "Contrast adaptive sharpening", kSwitch},
    {"sharpening_strength", "CAS strength", "From 0 (least) to 1 (most)", kStepper, 0.0f, 1.0f, 0.1f, "%.1f", sharpenOn},
    {"min_resolution", "Lowest resolution", nullptr, kStepper, 0.5f, 1.25f, 0.05f, "%.2f", nullptr, hintScale},
    {"resolution", "Highest resolution", "Used while the GPU has time to spare", kStepper, 0.5f, 2.0f, 0.05f, "%.2f"},
    {"refresh_rate", "Refresh rate", "120 fits the game's 60 frames a second", kStepper, 0, 0, 0, "%.0f Hz", nullptr, nullptr, refreshOptions},
    {"high_clocks", "High clocks", "Faster CPU and GPU; from the next start", kSwitch},
};
const Item kGameItems[] = {
    {"language", "Language", nullptr, kStepper, 0, 0, 0, nullptr, severalLanguages, hintLanguage, languageOptions, languageText, languageGet,
     languageSet, languageFile},
    {"invert_camera", "Invert camera", nullptr, kSwitch, 0, 0, 0, nullptr, nullptr, hintInvert},
    {"skip_hold", "Hold A to skip", "Cutscenes and dialogues", kStepper, 0.0f, 3.0f, 0.25f, nullptr, nullptr, nullptr, nullptr, skipText},
};

struct Tab {
    const char* name;
    const Item* items;
    int count;
};
const int kMaxItems = 8;  // on a tab
#define TAB(name, items) {name, items, (int)(sizeof(items) / sizeof(items[0]))}
#define FITS(items) static_assert(sizeof(items) / sizeof(items[0]) <= kMaxItems, "too many settings on a tab")
const Tab kTabs[] = {TAB("Screen", kScreenItems), TAB("Diorama", kDioramaItems), TAB("Picture", kPictureItems), TAB("Game", kGameItems)};
FITS(kScreenItems);
FITS(kDioramaItems);
FITS(kPictureItems);
FITS(kGameItems);
#undef TAB
#undef FITS
const int kTabCount = (int)(sizeof(kTabs) / sizeof(kTabs[0]));

float itemValue(const Item& item) {
    if (item.get) return item.get();
    const vr::Setting* s = vr::findSetting(item.key);
    return s ? s->get() : 0.0f;
}

bool itemEnabled(const Item& item) { return !item.enabled || item.enabled(); }

void itemText(const Item& item, float value, char* out, size_t size) {
    if (item.valueText) {
        item.valueText(value, out, size);
    } else if (item.kind == kSwitch) {
        snprintf(out, size, "%s", value != 0.0f ? "On" : "Off");
    } else {
        snprintf(out, size, item.format ? item.format : "%g", value);
    }
}

void itemHint(const Item& item, char* out, size_t size) {
    if (item.hintNow) {
        item.hintNow(out, size);
    } else {
        snprintf(out, size, "%s", item.hint ? item.hint : "");
    }
}

// ---------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------
const int kTexW = 640, kTexH = 1300;
const float kWidthM = 0.42f;  // metres; height from the texture's aspect
// Beside the pause menu's buttons, in the space right of them on the HUD
// panel (see kHudCenter in vr_game.cpp), a little in front of it and turned
// towards the player.
const xm::Vec3 kCenter{0.60f, -0.12f, -0.88f};

// Layout, in texture pixels (top-down rows).  Under the title, the tabs;
// under them the tab's settings, one row each (a slider takes two).
const float kDefaultsX0 = 448.0f, kDefaultsX1 = 608.0f, kDefaultsY0 = 24.0f, kDefaultsY1 = 70.0f;
const float kTabsY0 = 92.0f, kTabsY1 = 148.0f, kTabsX0 = 24.0f, kTabW = (kTexW - 48.0f) / kTabCount;
const float kRowsTop = 172.0f, kRowH = 124.0f;
const float kControlY = 42.0f;  // a row's control, below the row's top
const float kButtonR = 30.0f;
const float kStepMinusX = 388.0f, kStepPlusX = 580.0f;
const float kSwitchX0 = 520.0f, kSwitchX1 = 608.0f, kSwitchR = 22.0f;
const float kSliderY = 166.0f, kSliderMinusX = 60.0f, kSliderPlusX = 580.0f, kTrackX0 = 116.0f, kTrackX1 = 524.0f;

// What the pointer is on.
enum Part { kNone, kDefaults, kTabButton, kToggle, kMinus, kPlus, kTrack };
struct Hit {
    Part part = kNone;
    int index = 0;  // the tab, or the item on the current tab
    bool operator==(const Hit& o) const { return part == o.part && index == o.index; }
    bool operator!=(const Hit& o) const { return !(*this == o); }
};

std::atomic<int> sMenuSelecting{0};
std::atomic<int64_t> sMenuReportedAt{0};

std::string sIniPath;
bool sReady = false;
GLuint sTex = 0;
Canvas sStatic, sCanvas;
std::vector<uint32_t> sUpload;
xm::Vec3 sRight, sUp, sNormal;
xm::Mat4 sModel;

int sTab = 0;
float sAlpha = 0.0f;
bool sShown = false;
int64_t sLastUpdateNs = 0;
Hit sHover, sPressed;
bool sClickDown = false, sOwnsClick = false, sTick = false;
int64_t sPressNs = 0, sRepeatNs = 0;
bool sDirty = true, sUnsaved = false;
int64_t sChangedNs = 0;
bool sPointerOnPanel = false;
float sPointerPx = 0.0f, sPointerPy = 0.0f;  // where it is, texture pixels
std::string sShownState;                     // the values and hints on the panel's picture
bool sChanged[kTabCount][kMaxItems];         // settings changed here since they were last saved

float heightM() { return kWidthM * kTexH / kTexW; }

void setupPose() {
    sNormal = xm::normalize(kCenter * -1.0f);  // facing the player's starting head position
    sRight = xm::normalize(xm::cross(xm::Vec3{0.0f, 1.0f, 0.0f}, sNormal));
    sUp = xm::cross(sNormal, sRight);
    xm::Mat4 m = xm::Mat4::identity();
    xm::Vec3 axes[3] = {sRight * kWidthM, sUp * heightM(), sNormal};
    for (int c = 0; c < 3; c++) {
        m.at(0, c) = axes[c].x;
        m.at(1, c) = axes[c].y;
        m.at(2, c) = axes[c].z;
    }
    m.at(0, 3) = kCenter.x;
    m.at(1, 3) = kCenter.y;
    m.at(2, 3) = kCenter.z;
    sModel = m;
}

// The top of item `index`'s row on the current tab.
float rowTop(int index) {
    float y = kRowsTop;
    for (int i = 0; i < index; i++) {
        y += kTabs[sTab].items[i].kind == kSlider ? 2.0f * kRowH : kRowH;
    }
    return y;
}

// The parts that never change: background and title.
void drawStatic() {
    Canvas& c = sStatic;
    c.init(kTexW, kTexH);
    c.roundRect(0, 0, kTexW, kTexH, 30.0f, rgb(78, 86, 106, 0.95f));
    c.roundRect(2, 2, kTexW - 2, kTexH - 2, 28.0f, rgb(22, 25, 33));
    c.text(32, 62, kFontLarge, "VR settings", rgb(255, 255, 255));
}

float sliderX(const Item& item, float value) { return kTrackX0 + (kTrackX1 - kTrackX0) * (value - item.min) / (item.max - item.min); }

// The values and hints the picture shows: it is drawn again when they differ.
std::string shownState() {
    std::string state;
    char text[96];
    const Tab& tab = kTabs[sTab];
    for (int i = 0; i < tab.count; i++) {
        const Item& item = tab.items[i];
        itemText(item, itemValue(item), text, sizeof(text));
        state += text;
        itemHint(item, text, sizeof(text));
        state += '|';
        state += text;
        state += itemEnabled(item) ? '+' : '-';
    }
    return state;
}

void drawPanel() {
    sCanvas.px = sStatic.px;
    Canvas& c = sCanvas;
    const Color accent = rgb(86, 160, 255);
    const Color bright = rgb(196, 202, 216), dim = rgb(140, 147, 163), white = rgb(255, 255, 255), line = rgb(58, 63, 78);
    const Tab& tab = kTabs[sTab];
    auto fill = [&](const Hit& h) { return sPressed == h ? accent : sHover == h ? rgb(84, 93, 116) : rgb(44, 49, 62); };

    // Defaults: the tab's settings back to their defaults.
    c.roundRect(kDefaultsX0, kDefaultsY0, kDefaultsX1, kDefaultsY1, 23.0f, fill({kDefaults, 0}));
    c.text((kDefaultsX0 + kDefaultsX1) * 0.5f, 56, kFontSmall, "Defaults", rgb(230, 234, 242), 1);

    // Tabs.
    for (int t = 0; t < kTabCount; t++) {
        float x0 = kTabsX0 + kTabW * t + 3.0f, x1 = kTabsX0 + kTabW * (t + 1) - 3.0f;
        Hit h{kTabButton, t};
        Color back = t == sTab ? accent : sPressed == h || sHover == h ? rgb(84, 93, 116) : rgb(44, 49, 62);
        c.roundRect(x0, kTabsY0, x1, kTabsY1, 20.0f, back);
        c.text((x0 + x1) * 0.5f, kTabsY0 + 38.0f, kFontSmall, kTabs[t].name, t == sTab ? white : bright, 1);
    }

    // A round - or + button.
    auto drawButton = [&](const Hit& h, float x, float y, bool plus, bool live) {
        const float bar = 22.0f, thick = 5.0f;
        Color sign = live ? rgb(240, 243, 248) : dim;
        c.circle(x, y, kButtonR, fill(h));
        c.roundRect(x - bar * 0.5f, y - thick * 0.5f, x + bar * 0.5f, y + thick * 0.5f, thick * 0.5f, sign);
        if (plus) {
            c.roundRect(x - thick * 0.5f, y - bar * 0.5f, x + thick * 0.5f, y + bar * 0.5f, thick * 0.5f, sign);
        }
    };

    char text[96];
    for (int i = 0; i < tab.count; i++) {
        const Item& item = tab.items[i];
        float top = rowTop(i);
        float value = itemValue(item);
        bool live = itemEnabled(item);
        if (i > 0) {
            c.roundRect(32, top - 1.0f, kTexW - 32, top + 1.0f, 1.0f, line);
        }
        c.text(32, top + kControlY + 10.0f, kFontSmall, item.label, live ? bright : dim);
        itemHint(item, text, sizeof(text));
        c.text(32, top + 96.0f, kFontSmall, text, dim);
        itemText(item, value, text, sizeof(text));
        float y = top + kControlY;
        if (item.kind == kSwitch) {
            Hit h{kToggle, i};
            bool on = value != 0.0f;
            bool active = sHover == h || sPressed == h;
            Color pill = on ? (live ? accent : rgb(52, 84, 128)) : active ? rgb(84, 93, 116) : line;
            c.roundRect(kSwitchX0, y - kSwitchR, kSwitchX1, y + kSwitchR, kSwitchR, pill);
            c.circle(on ? kSwitchX1 - kSwitchR : kSwitchX0 + kSwitchR, y, active ? 19.0f : 17.0f, live ? white : rgb(150, 156, 170));
            c.text(kSwitchX0 - 16, y + 10, kFontSmall, text, live ? rgb(230, 234, 242) : dim, 2);
        } else if (item.kind == kStepper) {
            drawButton({kMinus, i}, kStepMinusX, y, false, live);
            drawButton({kPlus, i}, kStepPlusX, y, true, live);
            c.text((kStepMinusX + kStepPlusX) * 0.5f, y + 10, kFontSmall, text, live ? white : dim, 1);
        } else {
            c.text(608, top + kControlY + 14.0f, kFontLarge, text, live ? white : dim, 2);
            float sy = top + kSliderY;
            drawButton({kMinus, i}, kSliderMinusX, sy, false, live);
            drawButton({kPlus, i}, kSliderPlusX, sy, true, live);
            // The track, the part up to the knob, and the knob; the default
            // as a tick under the track.
            float kx = sliderX(item, fminf(item.max, fmaxf(item.min, value)));
            c.roundRect(kTrackX0, sy - 6, kTrackX1, sy + 6, 6.0f, line);
            c.roundRect(kTrackX0, sy - 6, kx, sy + 6, 6.0f, live ? accent : rgb(52, 84, 128));
            if (const vr::Setting* s = vr::findSetting(item.key)) {
                float dx = sliderX(item, fminf(item.max, fmaxf(item.min, s->def)));
                c.roundRect(dx - 1.5f, sy + 14, dx + 1.5f, sy + 24, 1.5f, dim);
            }
            Hit h{kTrack, i};
            bool active = sHover == h || sPressed == h;
            if (active) {
                c.circle(kx, sy, 32.0f, rgb(86, 160, 255, 0.3f));
            }
            c.circle(kx, sy, active ? 22.0f : 19.0f, live ? white : rgb(150, 156, 170));
        }
    }
    sShownState = shownState();
}

void upload() {
    // Rows bottom up for GL.
    sUpload.resize((size_t)kTexW * kTexH);
    for (int y = 0; y < kTexH; y++) {
        memcpy(&sUpload[(size_t)y * kTexW], &sCanvas.px[(size_t)(kTexH - 1 - y) * kTexW], kTexW * 4);
    }
    glBindTexture(GL_TEXTURE_2D, sTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kTexW, kTexH, GL_RGBA, GL_UNSIGNED_BYTE, sUpload.data());
    glGenerateMipmap(GL_TEXTURE_2D);
}

// Writes `key = value` lines into the settings file, each replacing the
// key's line or appended; the other lines (comments included) stay as they
// are.
typedef std::vector<std::pair<std::string, std::string>> Lines;

void writeSettings(const std::string& path, const Lines& lines) {
    static std::mutex sWriteLock;
    std::lock_guard<std::mutex> lock(sWriteLock);
    std::string out;
    std::vector<bool> replaced(lines.size(), false);
    if (FILE* f = fopen(path.c_str(), "r")) {
        char line[1280];
        while (fgets(line, sizeof(line), f)) {
            const char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            bool taken = false;
            for (size_t i = 0; i < lines.size() && !taken; i++) {
                size_t n = lines[i].first.size();
                const char* after = p + n;
                if (strncmp(p, lines[i].first.c_str(), n) != 0) continue;
                while (*after == ' ' || *after == '\t') after++;
                if (!replaced[i] && *after == '=') {
                    out += lines[i].first + " = " + lines[i].second + "\n";
                    replaced[i] = true;
                    taken = true;
                }
            }
            if (!taken) {
                out += line;
                if (!out.empty() && out.back() != '\n') out += '\n';
            }
        }
        fclose(f);
    } else {
        out = "# GalaxyQuest VR settings (see docs/CONTROLS.md)\n";
    }
    for (size_t i = 0; i < lines.size(); i++) {
        if (!replaced[i]) {
            out += lines[i].first + " = " + lines[i].second + "\n";
        }
    }
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) {
        port_log("vr: cannot write %s", tmp.c_str());
        return;
    }
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        port_log("vr: cannot write %s", path.c_str());
        return;
    }
    for (const auto& l : lines) {
        port_log("vr: %s: %s = %s", path.c_str(), l.first.c_str(), l.second.c_str());
    }
}

void writeSettingsLater(const Lines& lines) {
    if (sIniPath.empty() || lines.empty()) return;
    std::string path = sIniPath;
    PortHostAllocScope hostAlloc;
    std::thread([path, lines] {
        PortHostAllocScope scope;
        writeSettings(path, lines);
    }).detach();
}

// The settings changed on the panel go to the file.
void save() {
    sUnsaved = false;
    Lines lines;
    char text[64];
    for (int t = 0; t < kTabCount; t++) {
        for (int i = 0; i < kTabs[t].count; i++) {
            if (!sChanged[t][i]) continue;
            sChanged[t][i] = false;
            const Item& item = kTabs[t].items[i];
            if (item.fileText) {
                item.fileText(text, sizeof(text));
            } else {
                snprintf(text, sizeof(text), "%g", itemValue(item));
            }
            lines.emplace_back(item.key, text);
        }
    }
    writeSettingsLater(lines);
}

// Gives item `index` of the current tab the value `value` (within what the
// panel offers).
void setItem(int index, float value) {
    const Item& item = kTabs[sTab].items[index];
    if (item.kind != kSwitch && !item.options) {
        value = fminf(item.max, fmaxf(item.min, roundf(value / item.step) * item.step));
    }
    if (fabsf(value - itemValue(item)) < 1e-4f) return;
    if (item.set) {
        item.set(value);
    } else if (const vr::Setting* s = vr::findSetting(item.key)) {
        s->set(fminf(s->max, fmaxf(s->min, value)));
    }
    char text[64];
    itemText(item, itemValue(item), text, sizeof(text));
    port_log("vr settings: %s = %s", item.key, text);
    sChanged[sTab][index] = true;
    sDirty = true;
    sUnsaved = true;
    sChangedNs = port_host_time_ns();
}

// A step down (-1) or up (1): along the item's options, or by its step.
void stepItem(int index, int dir) {
    const Item& item = kTabs[sTab].items[index];
    float value = itemValue(item);
    if (!item.options) {
        setItem(index, value + dir * item.step);
        return;
    }
    float options[16];
    int n = item.options(options, 16);
    if (n == 0) return;
    // The option nearest the value, then its neighbour.
    int at = 0;
    for (int i = 1; i < n; i++) {
        if (fabsf(options[i] - value) < fabsf(options[at] - value)) at = i;
    }
    at += dir;
    setItem(index, options[at < 0 ? 0 : at >= n ? n - 1 : at]);
}

void resetTab() {
    const Tab& tab = kTabs[sTab];
    for (int i = 0; i < tab.count; i++) {
        const Item& item = tab.items[i];
        if (item.get) {
            setItem(i, 0.0f);  // the language: the disc's own
        } else if (const vr::Setting* s = vr::findSetting(item.key)) {
            setItem(i, s->def);
        }
    }
}

void describe(const Hit& h, char* out, size_t size) {
    const Tab& tab = kTabs[sTab];
    switch (h.part) {
    case kDefaults:
        snprintf(out, size, "Defaults (%s)", tab.name);
        break;
    case kTabButton:
        snprintf(out, size, "the %s tab", kTabs[h.index].name);
        break;
    case kToggle:
        snprintf(out, size, "the %s switch", tab.items[h.index].label);
        break;
    case kMinus:
    case kPlus:
        snprintf(out, size, "%s %s", tab.items[h.index].label, h.part == kMinus ? "-" : "+");
        break;
    case kTrack:
        snprintf(out, size, "the %s slider", tab.items[h.index].label);
        break;
    default:
        snprintf(out, size, "nothing");
        break;
    }
}

Hit hitAt(float x, float y) {
    auto near = [](float x, float y, float cx, float cy, float r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; };
    if (x >= kDefaultsX0 - 8 && x <= kDefaultsX1 + 8 && y >= kDefaultsY0 - 8 && y <= kDefaultsY1 + 8) return {kDefaults, 0};
    if (y >= kTabsY0 - 6 && y <= kTabsY1 + 6 && x >= kTabsX0 && x < kTabsX0 + kTabW * kTabCount) {
        return {kTabButton, (int)((x - kTabsX0) / kTabW)};
    }
    const Tab& tab = kTabs[sTab];
    for (int i = 0; i < tab.count; i++) {
        const Item& item = tab.items[i];
        float top = rowTop(i);
        float cy = top + kControlY;
        if (item.kind == kSwitch) {
            if (x >= 24.0f && x <= kTexW - 16.0f && y >= top + 4.0f && y <= top + kRowH - 4.0f) return {kToggle, i};  // the whole row
        } else if (item.kind == kStepper) {
            if (near(x, y, kStepMinusX, cy, kButtonR + 10.0f)) return {kMinus, i};
            if (near(x, y, kStepPlusX, cy, kButtonR + 10.0f)) return {kPlus, i};
        } else {
            float sy = top + kSliderY;
            if (near(x, y, kSliderMinusX, sy, kButtonR + 10.0f)) return {kMinus, i};
            if (near(x, y, kSliderPlusX, sy, kButtonR + 10.0f)) return {kPlus, i};
            if (x >= kTrackX0 - 16.0f && x <= kTrackX1 + 16.0f && y >= sy - 34.0f && y <= sy + 34.0f) return {kTrack, i};
        }
    }
    return {};
}

// The stage point of a point of the panel's picture.
xm::Vec3 stagePoint(float px, float py) {
    return kCenter + sRight * ((px / kTexW - 0.5f) * kWidthM) + sUp * ((0.5f - py / kTexH) * heightM());
}

// PETARI_PANELLOG: where each control is in the room, for scripted aims
// (PETARI_XRSIM_AIM, PETARI_VRAIM).
void logControls() {
    auto log = [](const char* tab, const char* what, float px, float py) {
        xm::Vec3 p = stagePoint(px, py);
        port_log("vr settings: aim %s / %s: %.4f,%.4f,%.4f", tab, what, p.x, p.y, p.z);
    };
    log("any", "Defaults", (kDefaultsX0 + kDefaultsX1) * 0.5f, (kDefaultsY0 + kDefaultsY1) * 0.5f);
    int tabWas = sTab;
    for (int t = 0; t < kTabCount; t++) {
        log("any", kTabs[t].name, kTabsX0 + kTabW * (t + 0.5f), (kTabsY0 + kTabsY1) * 0.5f);
        sTab = t;
        for (int i = 0; i < kTabs[t].count; i++) {
            const Item& item = kTabs[t].items[i];
            float top = rowTop(i);
            char what[64];
            if (item.kind == kSwitch) {
                log(kTabs[t].name, item.key, (kSwitchX0 + kSwitchX1) * 0.5f, top + kControlY);
                continue;
            }
            float y = item.kind == kSlider ? top + kSliderY : top + kControlY;
            snprintf(what, sizeof(what), "%s -", item.key);
            log(kTabs[t].name, what, item.kind == kSlider ? kSliderMinusX : kStepMinusX, y);
            snprintf(what, sizeof(what), "%s +", item.key);
            log(kTabs[t].name, what, item.kind == kSlider ? kSliderPlusX : kStepPlusX, y);
        }
    }
    sTab = tabWas;
}

}  // namespace

extern "C" void port_vr_pause_menu(int selecting) {
    sMenuSelecting.store(selecting);
    sMenuReportedAt.store(port_host_time_ns());
}

namespace vr {

void saveGamePath(const std::string& folder) { writeSettingsLater({{"game_path", folder}}); }

void settingsInit(const char* iniPath) {
    sIniPath = iniPath ? iniPath : "";
    ui::initFonts();
    setupPose();
    drawStatic();
    sCanvas.init(kTexW, kTexH);
    glGenTextures(1, &sTex);
    glBindTexture(GL_TEXTURE_2D, sTex);
    int levels = 1 + (int)floorf(log2f((float)kTexW));
    glTexStorage2D(GL_TEXTURE_2D, levels, GL_RGBA8, kTexW, kTexH);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // The panel opens on the tab of the way the game is shown.
    sTab = giantOn() ? 0 : 1;
    if (getenv("PETARI_PANELLOG")) {
        logControls();
    }
    sReady = true;
}

float settingsPointer(xm::Vec3 origin, xm::Vec3 dir, bool clickDown) {
    int64_t now = port_host_time_ns();
    float dt = sLastUpdateNs ? fminf(0.1f, (now - sLastUpdateNs) / 1e9f) : 0.0f;
    sLastUpdateNs = now;
    // Shown while the pause menu takes input (reported each game frame).
    bool wanted = sReady && sMenuSelecting.load() && now - sMenuReportedAt.load() < 250000000;
    sAlpha = wanted ? fminf(1.0f, sAlpha + dt / 0.15f) : fmaxf(0.0f, sAlpha - dt / 0.12f);
    if (sShown && !wanted && sUnsaved) {
        save();  // the menu closed
    } else if (sUnsaved && now - sChangedNs > 3000000000ll) {
        save();  // or three seconds after the last change, in case it never does
    }
    if (wanted != sShown) {
        port_log("vr settings: panel %s", wanted ? "shown" : "hidden");
    }
    sShown = wanted;

    float hitT = 0.0f;
    float px = 0.0f, py = 0.0f;
    if (wanted) {
        float dn = xm::dot(dir, sNormal);
        if (dn < -1e-4f) {
            float t = xm::dot(kCenter - origin, sNormal) / dn;
            xm::Vec3 p = origin + dir * t - kCenter;
            float u = xm::dot(p, sRight) / kWidthM + 0.5f, v = xm::dot(p, sUp) / heightM() + 0.5f;
            if (t > 0.0f && u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
                hitT = t;
                px = u * kTexW;
                py = (1.0f - v) * kTexH;
            }
        }
    }
    Hit over = hitT > 0.0f ? hitAt(px, py) : Hit{};
    sPointerPx = px;
    sPointerPy = py;
    // What the panel shows follows the settings and what its hints tell (the
    // render scale, the display's refresh rates...).
    if (wanted && !sDirty && shownState() != sShownState) {
        sDirty = true;
    }
    if (over != sHover) {
        sHover = over;
        sDirty = true;
        if (over.part != kNone) sTick = true;
    }
    bool onPanel = hitT > 0.0f;
    if (onPanel != sPointerOnPanel) {
        sPointerOnPanel = onPanel;
        port_log("vr settings: pointer %s the panel", onPanel ? "on" : "off");
    }
    // A click (A or the trigger) made on the panel belongs to it until
    // released; one that started elsewhere stays the game's.
    if (clickDown && !sClickDown && hitT > 0.0f) {
        char what[96];
        describe(over, what, sizeof(what));
        port_log("vr settings: click on %s", what);
        sOwnsClick = true;
        sPressed = over;
        sPressNs = sRepeatNs = now;
        if (over.part == kTabButton) sTab = over.index;
        if (over.part == kDefaults) resetTab();
        if (over.part == kToggle) setItem(over.index, itemValue(kTabs[sTab].items[over.index]) != 0.0f ? 0.0f : 1.0f);
        if (over.part == kMinus) stepItem(over.index, -1);
        if (over.part == kPlus) stepItem(over.index, 1);
        sDirty = true;
    }
    if (sOwnsClick && clickDown) {
        if (sPressed.part == kTrack && hitT > 0.0f) {
            const Item& item = kTabs[sTab].items[sPressed.index];
            setItem(sPressed.index, item.min + (item.max - item.min) * (px - kTrackX0) / (kTrackX1 - kTrackX0));
        }
        // Held on - or +: repeats after a moment.
        if ((sPressed.part == kMinus || sPressed.part == kPlus) && over == sPressed && now - sPressNs > 450000000 && now - sRepeatNs > 130000000) {
            sRepeatNs = now;
            stepItem(sPressed.index, sPressed.part == kPlus ? 1 : -1);
        }
    }
    if (!clickDown) {
        if (sPressed.part != kNone) sDirty = true;
        sOwnsClick = false;
        sPressed = {};
    }
    sClickDown = clickDown;
    return hitT;
}

bool settingsOwnsClick() { return sOwnsClick; }

bool settingsShown() { return sReady && sAlpha > 0.0f; }

void settingsLayerSize(int* width, int* height) {
    *width = kTexW;
    *height = kTexH;
}

UiLayer settingsLayer() {
    UiLayer l;
    l.visible = settingsShown();
    l.changed = l.visible;  // a small image: redrawn each frame it is up (fade, reticle)
    xm::Mat4 basis = xm::Mat4::identity();
    xm::Vec3 axes[3] = {sRight, sUp, sNormal};
    for (int c = 0; c < 3; c++) {
        basis.at(0, c) = axes[c].x;
        basis.at(1, c) = axes[c].y;
        basis.at(2, c) = axes[c].z;
    }
    l.orientation = xm::quatFromMatrix(basis);
    l.position = kCenter;
    l.width = kWidthM;
    l.height = heightM();
    return l;
}

void settingsDrawLayer() {
    if (!sReady) return;
    if (sDirty) {
        drawPanel();
        upload();
        sDirty = false;
    }
    drawOverlayQuad(sTex, xm::scale(2.0f), sAlpha);
    // The laser's reticle is in the eye images, under this layer: the
    // panel shows where it points itself.
    if (sPointerOnPanel) {
        const float r = 13.0f;
        drawReticle2d(sPointerPx / kTexW * 2.0f - 1.0f, 1.0f - sPointerPy / kTexH * 2.0f, r / kTexW * 2.0f, r / kTexH * 2.0f);
    }
}

bool settingsTakeTick() {
    bool t = sTick;
    sTick = false;
    return t;
}

void settingsDraw(const xm::Mat4& viewProj) {
    if (!sReady || sAlpha <= 0.0f) return;
    if (sDirty) {
        drawPanel();
        upload();
        sDirty = false;
    }
    drawOverlayQuad(sTex, viewProj * sModel, sAlpha);
}

}  // namespace vr
