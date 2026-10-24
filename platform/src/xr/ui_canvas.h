// A CPU canvas for the VR layer's own panels (the VR settings panel, the
// setup screen): anti-aliased rounded shapes and text from the baked glyph
// atlas in ui_font.h, into premultiplied RGBA8 pixels (R in the low byte,
// sRGB-encoded colours), rows top down.
#pragma once
#include <stdint.h>

#include <vector>

namespace ui {

enum { kFontLarge, kFontSmall };

struct Color {
    float r, g, b, a;  // 0..255, alpha 0..1
};
inline Color rgb(int r, int g, int b, float a = 1.0f) { return {(float)r, (float)g, (float)b, a}; }

// Decodes the glyph atlases; call once before drawing text.
void initFonts();
float textWidth(int font, const char* s);

struct Canvas {
    int w = 0, h = 0;
    std::vector<uint32_t> px;

    void init(int width, int height);
    void blend(int x, int y, const Color& c, float coverage);
    void roundRect(float x0, float y0, float x1, float y1, float radius, const Color& c);
    void circle(float cx, float cy, float r, const Color& c);
    // Text on a baseline; align 0 = left of x, 1 = centred on it, 2 = right.
    void text(float x, float baseline, int font, const char* s, const Color& c, int align = 0);
    // Text broken into lines at spaces to fit `width`, from the baseline
    // `baseline` down in steps of `lineHeight`; returns the baseline after
    // the last line.
    float textWrapped(float x, float baseline, float width, float lineHeight, int font, const char* s, const Color& c);
};

}  // namespace ui
