// The settings panel's layout and look (see settings_panel_plan.h). Nothing of the game or the SDK here.
//
// Where it goes (the game's PauseMenu layout, LayoutData/PauseMenu.arc: 608 x 456 layout units, y up, centred; on a 16:9
// picture a unit is 4/3 of a square unit wide): the two buttons are at y 24 and -73, 77 high with their shadow 6 lower, so the
// lower one ends at y -118; the dark band of the counters (PicStagebase1) starts at -123, with the star and lives counters at
// its left (they end 172 square units left of the middle) and the coin and Star Bit counters at its right (from 223 right of the
// middle, with four figures of Star Bits). The star list and the stage's name are at the top. The panel takes the middle of the
// counter band, 420 x 92 square units, from 122 to 214 below the middle of the picture, under the buttons and between the counters.
// The Luigi letter button (shown only while Luigi waits for a rescue) sits in the bottom middle from 178 below the middle down:
// then the panel is 54 high, without the hint line.
#include "settings_panel_plan.h"

#include <math.h>
#include <string.h>

namespace settings_panel {
namespace {

// ------------------------------------------------------------------ words
struct Words {
    const char* language;        // the end of the game's language prefix ("English" of "UsEnglish")
    const char16_t* zoom;        // the zoom row
    const char16_t* rear;        // the rear touch row
    const char16_t* on;
    const char16_t* off;
    const char16_t* hint;        // what rear touch aiming does
};
const Words kWords[] = {
    {"English", u"Camera Zoom", u"Rear Touch Aiming", u"On", u"Off",
     u"Slide a finger on the rear touch pad to move the Star Pointer."},
    {"French", u"Zoom de la caméra", u"Visée tactile arrière", u"Oui", u"Non",
     u"Faites glisser un doigt sur le pavé tactile arrière pour déplacer le pointeur."},
    {"Spanish", u"Zoom de la cámara", u"Apuntar con panel trasero", u"Sí", u"No",
     u"Desliza un dedo por el panel táctil trasero para mover el puntero."},
    {"German", u"Kamera-Zoom", u"Zielen mit Rück-Touchpad", u"An", u"Aus",
     u"Streiche mit einem Finger über das Rück-Touchpad, um den Pointer zu bewegen."},
    {"Italian", u"Zoom della telecamera", u"Mira col pannello posteriore", u"Sì", u"No",
     u"Fai scorrere un dito sul pannello posteriore per muovere il puntatore."},
    {"Dutch", u"Camerazoom", u"Richten met achterpaneel", u"Aan", u"Uit",
     u"Schuif met een vinger over het achterpaneel om de aanwijzer te bewegen."},
    {"Japanese", u"カメラズーム", u"背面タッチでねらう", u"オン", u"オフ",
     u"背面タッチパッドを指でなぞると、ポインターが動きます。"},
};
constexpr int kLanguages = sizeof(kWords) / sizeof(kWords[0]);

int Length(const char16_t* s) {
    int n = 0;
    while (s[n]) ++n;
    return n;
}

// ------------------------------------------------------------------ colours (the game's windows: deep indigo, a light rim, white
// words with a soft dark shadow, values in yellow)
constexpr Rgba kFillTop = {44, 58, 150, 214}, kFillBottom = {16, 22, 84, 226};
constexpr Rgba kRimInner = {120, 150, 255, 210}, kRimOuter = {226, 236, 255, 235};
constexpr Rgba kGlow = {0, 0, 24, 100};
constexpr Rgba kWhite = {255, 255, 255, 255};
constexpr Rgba kShadow = {6, 8, 40, 170};
constexpr Rgba kYellow = {255, 226, 40, 255};
constexpr Rgba kDim = {150, 162, 214, 255};
constexpr Rgba kHint = {196, 210, 255, 255};
constexpr Rgba kPipOff = {84, 100, 178, 230};
constexpr Rgba kTrack = {8, 12, 52, 170};
constexpr Rgba kTrackRim = {110, 132, 230, 150};
constexpr Rgba kKnobTop = {96, 128, 250, 240}, kKnobBottom = {48, 66, 196, 240};

Rgba Fade(Rgba c, float a) {
    float v = c.a * a;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    c.a = static_cast<uint8_t>(v + 0.5f);
    return c;
}
Rgba Mix(Rgba a, Rgba b, float t) {
    auto m = [t](uint8_t x, uint8_t y) { return static_cast<uint8_t>(x + (y - x) * t + 0.5f); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}

// ------------------------------------------------------------------ shapes, as triangles
constexpr float kPi = 3.14159265f;

struct Builder {
    Plan* plan;
    float alpha;
    void Tri(Vertex a, Vertex b, Vertex c) {
        if (plan->vertexCount + 3 > kMaxVertices) {
            plan->overflow = true;
            return;
        }
        a.c = Fade(a.c, alpha), b.c = Fade(b.c, alpha), c.c = Fade(c.c, alpha);
        plan->vertices[plan->vertexCount++] = a;
        plan->vertices[plan->vertexCount++] = b;
        plan->vertices[plan->vertexCount++] = c;
    }
    void Quad(Vertex a, Vertex b, Vertex c, Vertex d) {
        Tri(a, b, c);
        Tri(a, c, d);
    }

    // The outline of a rounded rectangle grown by `o` (negative: shrunk), clockwise from the left end of the top left corner.
    static int Outline(float x0, float y0, float x1, float y1, float r, float o, int seg, float* px, float* py) {
        // (a radius above half the height or width would make the outline cross itself)
        if (r > (y1 - y0) * 0.5f) r = (y1 - y0) * 0.5f;
        if (r > (x1 - x0) * 0.5f) r = (x1 - x0) * 0.5f;
        if (r < 0) r = 0;
        const float cx[4] = {x0 + r, x1 - r, x1 - r, x0 + r}, cy[4] = {y0 + r, y0 + r, y1 - r, y1 - r};
        const float rr = r + o > 0 ? r + o : 0;
        int n = 0;
        for (int k = 0; k < 4; k++) {
            const float a0 = kPi * (1.0f + 0.5f * k);
            for (int s = 0; s <= seg; s++) {
                const float a = a0 + 0.5f * kPi * s / seg;
                // (a corner of radius 0 with o > 0 is still round: the grown outline of a rectangle)
                px[n] = cx[k] + rr * cosf(a);
                py[n] = cy[k] + rr * sinf(a);
                n++;
            }
        }
        return n;
    }

    // Filled, the colour going from `top` at y0 to `bottom` at y1.
    void Fill(float x0, float y0, float x1, float y1, float r, Rgba top, Rgba bottom, int seg = 6) {
        float px[40], py[40];
        const int n = Outline(x0, y0, x1, y1, r, 0, seg, px, py);
        auto at = [&](float x, float y) {
            const float t = y1 > y0 ? (y - y0) / (y1 - y0) : 0;
            return Vertex{x, y, Mix(top, bottom, t < 0 ? 0 : (t > 1 ? 1 : t))};
        };
        const Vertex c = at((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
        for (int i = 0; i < n; i++) Tri(c, at(px[i], py[i]), at(px[(i + 1) % n], py[(i + 1) % n]));
    }

    // The band between the outline grown by `o0` (colour c0) and by `o1` (colour c1).
    void Ring(float x0, float y0, float x1, float y1, float r, float o0, Rgba c0, float o1, Rgba c1, int seg = 6) {
        float ax[40], ay[40], bx[40], by[40];
        const int n = Outline(x0, y0, x1, y1, r, o0, seg, ax, ay);
        Outline(x0, y0, x1, y1, r, o1, seg, bx, by);
        for (int i = 0; i < n; i++) {
            const int j = (i + 1) % n;
            Quad({ax[i], ay[i], c0}, {bx[i], by[i], c1}, {bx[j], by[j], c1}, {ax[j], ay[j], c0});
        }
    }

    // A disc with a soft edge (anti-aliased by a ring that fades out).
    void Disc(float x, float y, float r, Rgba c, float soft = 0.9f) {
        Fill(x - r, y - r, x + r, y + r, r, c, c, 3);
        Rgba clear = c;
        clear.a = 0;
        Ring(x - r, y - r, x + r, y + r, r, 0, c, soft, clear, 3);
    }

    // A small arrow head pointing left (dir -1) or right (+1), centred at (x, y), with a soft edge.
    void Arrow(float x, float y, float dir, float w, float h, Rgba c) {
        Rgba soft = c;
        soft.a = static_cast<uint8_t>(c.a * 0.35f);
        const float g = 0.8f;
        Tri({x + dir * (w * 0.5f + g), y, soft}, {x - dir * (w * 0.5f + g * 0.5f), y - h * 0.5f - g, soft},
            {x - dir * (w * 0.5f + g * 0.5f), y + h * 0.5f + g, soft});
        Tri({x + dir * w * 0.5f, y, c}, {x - dir * w * 0.5f, y - h * 0.5f, c}, {x - dir * w * 0.5f, y + h * 0.5f, c});
    }

    // A thin horizontal line that fades in and out at its ends.
    void Line(float x0, float x1, float y, float t, Rgba c) {
        Rgba clear = c;
        clear.a = 0;
        const float f = (x1 - x0) * 0.18f;
        const float xs[4] = {x0, x0 + f, x1 - f, x1};
        const Rgba cs[4] = {clear, c, c, clear};
        for (int i = 0; i < 3; i++)
            Quad({xs[i], y - t * 0.5f, cs[i]}, {xs[i + 1], y - t * 0.5f, cs[i + 1]}, {xs[i + 1], y + t * 0.5f, cs[i + 1]},
                 {xs[i], y + t * 0.5f, cs[i]});
    }
};

// ------------------------------------------------------------------ words, placed
struct Writer {
    Plan* plan;
    const FontMetrics* fonts;
    float alpha;

    float Width(Font f, const char16_t* s, int n, float size) const {
        const FontMetrics& m = fonts[f];
        float w = 0;
        for (int i = 0; i < n; i++) w += m.advance ? m.advance(m.context, s[i]) : 0;
        return w * size / m.height;
    }
    bool Has(Font f, const char16_t* s) const {
        const FontMetrics& m = fonts[f];
        if (!m.has) return true;
        for (; *s; ++s)
            if (*s >= 0x20 && !m.has(m.context, *s)) return false;
        return true;
    }
    void Run(Font f, float x, float y, float sx, float sy, Rgba c, const char16_t* s, int n) {
        if (plan->runCount >= kMaxRuns) {
            plan->overflow = true;
            return;
        }
        plan->runs[plan->runCount++] = {f, x, y, sx, sy, Fade(c, alpha), s, n};
    }
    // `s` at `size` square units of line height, its middle (of capital letters) at cy: left-aligned at x (align 0), centred on x
    // (align 1) or ending at x (align 2). Squeezed sideways to `maxW` if it is wider (down to 78 %, then smaller as a whole).
    // With a shadow below and to the right, as the game's words have. Returns the width.
    float Text(const char16_t* s, float x, float cy, float size, Rgba c, int align, float maxW, bool shadow = true) {
        const int n = Length(s);
        float w = Width(FONT_MESSAGE, s, n, size);
        float squeeze = 1.0f;
        if (maxW > 0 && w > maxW) {
            squeeze = maxW / w;
            if (squeeze < 0.78f) {
                size *= squeeze / 0.78f;
                squeeze = 0.78f;
            }
            w = Width(FONT_MESSAGE, s, n, size) * squeeze;
        }
        const FontMetrics& m = fonts[FONT_MESSAGE];
        const float scale = size / m.height;
        const float left = align == 0 ? x : (align == 1 ? x - w * 0.5f : x - w);
        // (in the game's message font, 33 pixels to a line, capitals span rows 8-25 of a glyph and small letters 13-25: a mixed
        // word looks centred with row 17.4 at cy)
        const float top = cy - 17.4f * (m.height > 0 ? m.height / 33.0f : 1.0f) * scale;
        if (shadow) {
            Rgba sh = kShadow;
            sh.a = static_cast<uint8_t>(sh.a * (c.a / 255.0f));
            Run(FONT_MESSAGE, left + size * 0.07f, top + size * 0.1f, scale * squeeze, scale, sh, s, n);
        }
        Run(FONT_MESSAGE, left, top, scale * squeeze, scale, c, s, n);
        return w;
    }
    // A button picture of the picture font, `size` square units high, centred at (cx, cy).
    void Button(uint16_t code, float cx, float cy, float size, const char16_t* storage) {
        const FontMetrics& m = fonts[FONT_PICTURE];
        const float scale = size / m.height;
        // (the button of a cell is drawn round the point 16, 16.5 of the 34 x 35 cell; a glyph starts one pixel left of its cell)
        (void)code;
        Run(FONT_PICTURE, cx - 15.0f * scale, cy - 16.5f * scale, scale, scale, kWhite, storage, 1);
    }
};

// The characters the runs point at must outlive the plan: static, one per button.
const char16_t kTriangleText[2] = {kPicTriangle, 0};
const char16_t kLText[2] = {kPicL, 0};
const char16_t kRText[2] = {kPicR, 0};

}  // namespace

int LanguageIndex(const char* prefix) {
    if (!prefix) return 0;
    const size_t n = strlen(prefix);
    for (int i = 0; i < kLanguages; i++) {
        const size_t k = strlen(kWords[i].language);
        if (n >= k && strcmp(prefix + n - k, kWords[i].language) == 0) return i;
    }
    return 0;
}
const char* LanguageName(int index) { return index >= 0 && index < kLanguages ? kWords[index].language : "English"; }

void Build(const State& st, const FontMetrics fonts[2], Plan* plan) {
    plan->vertexCount = 0;
    plan->runCount = 0;
    plan->overflow = false;
    Builder g{plan, st.alpha};
    Writer t{plan, fonts, st.alpha};

    // words of the language, or English where the font lacks a character
    const Words* words = &kWords[st.language >= 0 && st.language < kLanguages ? st.language : 0];
    if (!t.Has(FONT_MESSAGE, words->zoom) || !t.Has(FONT_MESSAGE, words->rear) || !t.Has(FONT_MESSAGE, words->on) ||
        !t.Has(FONT_MESSAGE, words->off) || !t.Has(FONT_MESSAGE, words->hint))
        words = &kWords[0];

    const float midX = st.width * 0.5f, midY = st.height * 0.5f;
    const float w = 420.0f;
    const float x0 = midX - w * 0.5f, x1 = midX + w * 0.5f;
    const float y0 = midY + 122.0f, y1 = midY + (st.compact ? 176.0f : 214.0f);
    // rows (middles), sizes
    const float rowZoom = y0 + (st.compact ? 14.0f : 17.0f);
    const float rowPips = y0 + 33.0f;
    const float rowLine = y0 + (st.compact ? 28.5f : 41.0f);
    const float rowRear = y0 + (st.compact ? 42.0f : 56.0f);
    const float rowHint = y0 + 78.0f;
    const float labelSize = st.compact ? 18.0f : 20.0f, icon = st.compact ? 21.0f : 24.0f;
    plan->x0 = x0, plan->y0 = y0, plan->x1 = x1, plan->y1 = y1;

    // the window: a soft dark glow round it, the indigo body with a sheen on its upper part, a light rim with a soft edge
    const float r = 14.0f;
    Rgba clear = kGlow;
    clear.a = 0;
    g.Ring(x0, y0, x1, y1, r, 0.5f, kGlow, 7.0f, clear);
    g.Fill(x0, y0, x1, y1, r, kFillTop, kFillBottom);
    g.Fill(x0 + 3, y0 + 2.5f, x1 - 3, y0 + (y1 - y0) * 0.42f, r - 3, Rgba{255, 255, 255, 30}, Rgba{255, 255, 255, 0});
    g.Ring(x0, y0, x1, y1, r, -2.2f, kRimInner, -0.2f, kRimOuter);
    Rgba rimClear = kRimOuter;
    rimClear.a = 0;
    g.Ring(x0, y0, x1, y1, r, -0.2f, kRimOuter, 1.0f, rimClear);
    g.Line(x0 + 18, x1 - 18, rowLine, 1.2f, Rgba{150, 172, 255, 70});

    // the columns: labels from the left, the controls between cl and cr
    const float labelX = x0 + 18.0f, labelMax = 168.0f;
    const float cl = x0 + 196.0f, cr = x1 - 18.0f;
    const float valueX = (cl + cr) * 0.5f;

    // ---- camera zoom:  label   [L]  ◂ 1.5× ▸  [R]   with the 11 steps as pips under the value
    t.Text(words->zoom, labelX, rowZoom, labelSize, kWhite, 0, labelMax);
    static char16_t value[8];
    const int tenths = st.zoomTenths < 10 ? 10 : (st.zoomTenths > 20 ? 20 : st.zoomTenths);
    value[0] = static_cast<char16_t>(u'0' + tenths / 10);
    value[1] = u'.';
    value[2] = static_cast<char16_t>(u'0' + tenths % 10);
    value[3] = u'×';
    value[4] = 0;
    const float pop = 1.0f + 0.14f * st.pulse;
    const float valueW = t.Text(value, valueX, rowZoom, labelSize * pop, kYellow, 1, 0);
    const float arrowGap = (valueW > 34.0f ? valueW : 34.0f) * 0.5f + 12.0f;
    g.Arrow(valueX - arrowGap, rowZoom, -1, 7.0f, 11.0f, tenths > 10 ? kYellow : Fade(kDim, 0.5f));
    g.Arrow(valueX + arrowGap, rowZoom, 1, 7.0f, 11.0f, tenths < 20 ? kYellow : Fade(kDim, 0.5f));
    t.Button(kPicL, cl + icon * 0.5f, rowZoom, icon, kLText);
    t.Button(kPicR, cr - icon * 0.5f, rowZoom, icon, kRText);
    if (!st.compact) {
        const float step = 12.0f;
        for (int i = 0; i <= 10; i++) {
            const float px = valueX + (i - 5) * step;
            const int at = tenths - 10;
            if (i == at) {
                Rgba halo = kYellow;
                halo.a = 70;
                g.Disc(px, rowPips, 6.6f + 1.2f * st.pulse, halo, 2.5f);
                g.Disc(px, rowPips, 4.6f, kWhite);
                g.Disc(px, rowPips, 3.4f, kYellow);
            } else {
                g.Disc(px, rowPips, i < at ? 3.0f : 2.6f, i < at ? Rgba{236, 196, 40, 240} : kPipOff);
            }
        }
    }

    // ---- rear touch aiming:  label   [△]  ( Off | On )
    t.Text(words->rear, labelX, rowRear, labelSize, kWhite, 0, labelMax);
    t.Button(kPicTriangle, cl + icon * 0.5f, rowRear, icon, kTriangleText);
    const float sx0 = cl + icon + 8.0f, sx1 = cr, sh = st.compact ? 19.0f : 22.0f;
    const float sy0 = rowRear - sh * 0.5f, sy1 = rowRear + sh * 0.5f;
    g.Fill(sx0, sy0, sx1, sy1, sh * 0.5f, kTrack, kTrack);
    Rgba trackClear = kTrackRim;
    trackClear.a = 0;
    g.Ring(sx0, sy0, sx1, sy1, sh * 0.5f, -1.2f, kTrackRim, 0.0f, kTrackRim);
    g.Ring(sx0, sy0, sx1, sy1, sh * 0.5f, 0.0f, kTrackRim, 0.8f, trackClear);
    const float half = (sx1 - sx0) * 0.5f;
    const float knob = st.knob < 0 ? 0 : (st.knob > 1 ? 1 : st.knob);
    const float kx0 = sx0 + 2.0f + knob * half, kx1 = kx0 + half - 4.0f;
    g.Fill(kx0, sy0 + 2.0f, kx1, sy1 - 2.0f, sh * 0.5f - 2.0f, kKnobTop, kKnobBottom);
    g.Ring(kx0, sy0 + 2.0f, kx1, sy1 - 2.0f, sh * 0.5f - 2.0f, -1.0f, Rgba{200, 214, 255, 200}, 0.0f, Rgba{236, 242, 255, 230});
    g.Ring(kx0, sy0 + 2.0f, kx1, sy1 - 2.0f, sh * 0.5f - 2.0f, 0.0f, Rgba{236, 242, 255, 230}, 0.8f, Rgba{236, 242, 255, 0});
    const float wordSize = labelSize * 0.9f;
    t.Text(words->off, sx0 + half * 0.5f, rowRear, wordSize, Mix(kWhite, kDim, knob), 1, half - 10.0f);
    t.Text(words->on, sx0 + half * 1.5f, rowRear, wordSize, Mix(kDim, kYellow, knob), 1, half - 10.0f);

    // ---- what it does
    if (!st.compact) t.Text(words->hint, midX, rowHint, 14.0f, kHint, 1, w - 36.0f);
}

}  // namespace settings_panel
