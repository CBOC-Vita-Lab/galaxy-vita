// The settings panel of the handheld ports, under the game's pause menu: what it draws, as plain data.
//
// settings_panel.cpp turns the plan into GX (shapes as coloured triangles, words and button pictures with the game's own fonts
// through nw4r::ut::CharWriter); tools/settings_panel_preview/ turns the same plan into a PNG on the host, so the layout can be
// looked at without a console. This file uses nothing of the game or the SDK.
//
// Coordinates: "square units" of the game's picture, y down: the height is the game's screen height (456), the width that
// height times the picture's aspect (810.7 for 16:9), so a unit is as wide as it is high on the screen.
#pragma once

#include <stdint.h>

namespace settings_panel {

struct Rgba { uint8_t r, g, b, a; };
struct Vertex { float x, y; Rgba c; };

enum Font : uint8_t { FONT_MESSAGE = 0, FONT_PICTURE = 1 };

// What the plan needs to know of a font: its line height and baseline (in the font's pixels) and the advance of a character.
struct FontMetrics {
    float height = 1.0f, baseline = 1.0f;
    void* context = nullptr;
    float (*advance)(void* context, uint16_t code) = nullptr;  // in the font's pixels
    bool (*has)(void* context, uint16_t code) = nullptr;       // the font has a picture for it
};

// One run of characters of one font, in one colour: the left of its first character at x, the top of its line at y, each
// character `scaleX` x `scaleY` of the font's pixels.
struct TextRun {
    Font font;
    float x, y, scaleX, scaleY;
    Rgba color;
    const char16_t* text;
    int length;
};

struct State {
    float width = 810.6667f, height = 456.0f;  // the picture, in square units
    int zoomTenths = 15;                       // 10..20
    bool rearAim = false;
    float knob = 0.0f;   // the On / Off switch's highlight, 0 = Off .. 1 = On (eased towards the setting)
    float pulse = 0.0f;  // 1 just after the zoom changed, easing to 0
    float alpha = 1.0f;  // the panel's fade-in
    bool compact = false;  // the Luigi letter button is at the bottom of the screen: no hint line, a lower panel
    int language = 0;    // index into the translations (LanguageIndex)
};

// The translations: English, French, Spanish, German, Italian, Dutch, Japanese. Others (and any text the font lacks) English.
int LanguageIndex(const char* gamePrefix);  // MR::getCurrentLanguagePrefix(): "UsEnglish", "EuFrench", "JpJapanese", ...
const char* LanguageName(int index);

constexpr int kMaxVertices = 6144;  // triangles: 3 to a triangle (the panel uses about 3,500)
constexpr int kMaxRuns = 24;

struct Plan {
    Vertex vertices[kMaxVertices];
    int vertexCount = 0;
    TextRun runs[kMaxRuns];
    int runCount = 0;
    // where it is, for the log and the checks (square units)
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool overflow = false;
};

// fonts[FONT_MESSAGE], fonts[FONT_PICTURE]
void Build(const State& state, const FontMetrics fonts[2], Plan* plan);

// The picture font's characters for the handheld's buttons (platform/src/port/handheld_ui.cpp draws them over the Wii ones).
constexpr uint16_t kPicTriangle = 0x32;  // the font's "C" button
constexpr uint16_t kPicL = 0x49;         // "Z"
constexpr uint16_t kPicR = 0x36;         // "2"

}  // namespace settings_panel
