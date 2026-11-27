// Galaxy 3.0 multiplayer: the panels' layout and look (see MpPanelPlan.hpp). Pure code: no game headers, no SDK.
// Shapes and text placement follow platform/src/port/settings_panel_plan.cpp (the port's pause-menu panel), so both panels look
// like one family: the game's indigo windows, light rim, white words with a soft shadow, yellow for the selection.
#include "Game/MP/MpPanelPlan.hpp"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace MpPanel {
namespace {

// ------------------------------------------------------------------ colours
constexpr Rgba kWhite = {255, 255, 255, 255};
constexpr Rgba kShadow = {6, 8, 40, 170};
constexpr Rgba kYellow = {255, 226, 40, 255};
constexpr Rgba kGreen = {90, 220, 110, 255}, kRed = {240, 80, 70, 255}, kGrey = {120, 128, 170, 255};

constexpr float kPi = 3.14159265f;
constexpr int kPicPowerStar = 7;   // the picture font's Power Star (MpMsgText::PIC_STAR)

Rgba Fade(Rgba c, float a) {
    float v = c.a * a;
    v = v < 0 ? 0 : (v > 255 ? 255 : v);
    c.a = static_cast<uint8_t>(v + 0.5f);
    return c;
}
Rgba Mix(Rgba a, Rgba b, float t) {
    auto m = [t](uint8_t x, uint8_t y) { return static_cast<uint8_t>(x + (y - x) * t + 0.5f); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}
Rgba Clear(Rgba c) { c.a = 0; return c; }

int Len(const char16_t* s) {
    int n = 0;
    while (s && s[n]) ++n;
    return n;
}

// ------------------------------------------------------------------ shapes
struct Builder {
    PlanBase* plan;
    float alpha;
    void Tri(Vertex a, Vertex b, Vertex c) {
        if (plan->vertexCount + 3 > plan->maxVertices) { plan->overflow = true; return; }
        a.c = Fade(a.c, alpha), b.c = Fade(b.c, alpha), c.c = Fade(c.c, alpha);
        plan->vertices[plan->vertexCount++] = a;
        plan->vertices[plan->vertexCount++] = b;
        plan->vertices[plan->vertexCount++] = c;
    }
    void Quad(Vertex a, Vertex b, Vertex c, Vertex d) { Tri(a, b, c); Tri(a, c, d); }
    void Rect(float x0, float y0, float x1, float y1, Rgba c) { Quad({x0, y0, c}, {x1, y0, c}, {x1, y1, c}, {x0, y1, c}); }

    static int Outline(float x0, float y0, float x1, float y1, float r, float o, int seg, float* px, float* py) {
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
                px[n] = cx[k] + rr * cosf(a);
                py[n] = cy[k] + rr * sinf(a);
                n++;
            }
        }
        return n;
    }
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
    void Ring(float x0, float y0, float x1, float y1, float r, float o0, Rgba c0, float o1, Rgba c1, int seg = 6) {
        float ax[40], ay[40], bx[40], by[40];
        const int n = Outline(x0, y0, x1, y1, r, o0, seg, ax, ay);
        Outline(x0, y0, x1, y1, r, o1, seg, bx, by);
        for (int i = 0; i < n; i++) {
            const int j = (i + 1) % n;
            Quad({ax[i], ay[i], c0}, {bx[i], by[i], c1}, {bx[j], by[j], c1}, {ax[j], ay[j], c0});
        }
    }
    void Disc(float x, float y, float r, Rgba c, float soft = 0.9f) {
        Fill(x - r, y - r, x + r, y + r, r, c, c, 3);
        Ring(x - r, y - r, x + r, y + r, r, 0, c, soft, Clear(c), 3);
    }
    void Arrow(float x, float y, float dir, float w, float h, Rgba c) {  // pointing left (-1) / right (+1)
        Rgba soft = c;
        soft.a = static_cast<uint8_t>(c.a * 0.35f);
        const float g = 0.8f;
        Tri({x + dir * (w * 0.5f + g), y, soft}, {x - dir * (w * 0.5f + g * 0.5f), y - h * 0.5f - g, soft},
            {x - dir * (w * 0.5f + g * 0.5f), y + h * 0.5f + g, soft});
        Tri({x + dir * w * 0.5f, y, c}, {x - dir * w * 0.5f, y - h * 0.5f, c}, {x - dir * w * 0.5f, y + h * 0.5f, c});
    }
    void ArrowV(float x, float y, float dir, float w, float h, Rgba c) {  // pointing up (-1) / down (+1)
        Tri({x, y + dir * h * 0.5f, c}, {x - w * 0.5f, y - dir * h * 0.5f, c}, {x + w * 0.5f, y - dir * h * 0.5f, c});
    }
    void Line(float x0, float x1, float y, float t, Rgba c) {
        const float f = (x1 - x0) * 0.18f;
        const float xs[4] = {x0, x0 + f, x1 - f, x1};
        const Rgba cs[4] = {Clear(c), c, c, Clear(c)};
        for (int i = 0; i < 3; i++)
            Quad({xs[i], y - t * 0.5f, cs[i]}, {xs[i + 1], y - t * 0.5f, cs[i + 1]}, {xs[i + 1], y + t * 0.5f, cs[i + 1]},
                 {xs[i], y + t * 0.5f, cs[i]});
    }
    // A thick segment from a to b.
    void Seg(float ax, float ay, float bx, float by, float t, Rgba c) {
        float dx = bx - ax, dy = by - ay;
        const float l = sqrtf(dx * dx + dy * dy);
        if (l <= 0) return;
        dx /= l, dy /= l;
        const float nx = -dy * t * 0.5f, ny = dx * t * 0.5f;
        Quad({ax + nx, ay + ny, c}, {bx + nx, by + ny, c}, {bx - nx, by - ny, c}, {ax - nx, ay - ny, c});
    }
    void Star(float cx, float cy, float r, float turn, Rgba inner, Rgba outer) {
        float px[10], py[10];
        for (int i = 0; i < 10; i++) {
            const float a = (turn + i / 10.0f) * 2 * kPi - kPi * 0.5f;
            const float rr = (i & 1) ? r * 0.45f : r;
            px[i] = cx + rr * cosf(a), py[i] = cy + rr * sinf(a);
        }
        for (int i = 0; i < 10; i++) Tri({cx, cy, inner}, {px[i], py[i], outer}, {px[(i + 1) % 10], py[(i + 1) % 10], outer});
    }
    void Crown(float cx, float cy, float s, Rgba c) {
        const Rgba dark = {150, 100, 0, c.a};
        Rect(cx - s * 0.5f, cy + s * 0.05f, cx + s * 0.5f, cy + s * 0.35f, c);
        Tri({cx - s * 0.5f, cy + s * 0.08f, c}, {cx - s * 0.5f, cy - s * 0.35f, c}, {cx - s * 0.15f, cy + s * 0.08f, c});
        Tri({cx - s * 0.2f, cy + s * 0.08f, c}, {cx, cy - s * 0.42f, c}, {cx + s * 0.2f, cy + s * 0.08f, c});
        Tri({cx + s * 0.15f, cy + s * 0.08f, c}, {cx + s * 0.5f, cy - s * 0.35f, c}, {cx + s * 0.5f, cy + s * 0.08f, c});
        Rect(cx - s * 0.5f, cy + s * 0.22f, cx + s * 0.5f, cy + s * 0.27f, dark);
        Disc(cx, cy - s * 0.42f, s * 0.07f, c, 0.4f);
    }
    void Tick(float cx, float cy, float s, Rgba c) {
        Seg(cx - s * 0.45f, cy, cx - s * 0.12f, cy + s * 0.32f, s * 0.2f, c);
        Seg(cx - s * 0.18f, cy + s * 0.32f, cx + s * 0.48f, cy - s * 0.38f, s * 0.2f, c);
    }
    void Cross(float cx, float cy, float s, Rgba c) {
        Seg(cx - s * 0.38f, cy - s * 0.38f, cx + s * 0.38f, cy + s * 0.38f, s * 0.2f, c);
        Seg(cx - s * 0.38f, cy + s * 0.38f, cx + s * 0.38f, cy - s * 0.38f, s * 0.2f, c);
    }
    void Bars(float x, float cy, float h, int level, bool lag) {
        // four bars from x, growing; `level` lit
        const float w = h * 0.2f, gap = h * 0.1f;
        const Rgba on = lag ? kRed : (level >= 3 ? kGreen : (level == 2 ? kYellow : kRed));
        for (int i = 0; i < 4; i++) {
            const float bh = h * (0.35f + 0.65f * i / 3.0f);
            const float bx = x + i * (w + gap);
            Rect(bx, cy + h * 0.5f - bh, bx + w, cy + h * 0.5f, i < level ? on : Rgba{70, 80, 140, 200});
        }
    }
};

// ------------------------------------------------------------------ words
struct Writer {
    PlanBase* plan;
    const FontMetrics* fonts;
    float alpha;

    float CharW(char16_t c, float size, Font face = FONT_MESSAGE) const {
        if (c >= 0xE000 && c < 0xE100) return size * 1.08f;
        const FontMetrics& m = fonts[face];
        return (m.advance ? m.advance(m.context, c) : size * 0.5f) * size / m.height;
    }
    float Width(const char16_t* s, int n, float size, Font face = FONT_MESSAGE) const {
        float w = 0;
        for (int i = 0; i < n; i++) w += CharW(s[i], size, face);
        return w;
    }
    // The big display face (FONT_DISPLAY) for these characters, or the message font when the face is not there (no metrics) or
    // lacks one of them (never a blank or a "?").
    Font Face(const char16_t* s, int n, bool display) const {
        if (!display) return FONT_MESSAGE;
        const FontMetrics& m = fonts[FONT_DISPLAY];
        if (!m.has || !m.advance || !m.context || !(m.height > 1.0f)) return FONT_MESSAGE;
        for (int i = 0; i < n; i++)
            if (s[i] >= 0xE000 || !m.has(m.context, s[i])) return FONT_MESSAGE;
        return FONT_DISPLAY;
    }
    int Store(const char16_t* s, int n) {
        if (plan->textCount + n > plan->maxText) { plan->overflow = true; return -1; }
        memcpy(plan->text + plan->textCount, s, n * sizeof(char16_t));
        const int at = plan->textCount;
        plan->textCount += n;
        return at;
    }
    void Run(Font f, float x, float y, float sx, float sy, Rgba c, int start, int n) {
        if (start < 0 || n <= 0) return;
        if (plan->runCount >= plan->maxRuns) { plan->overflow = true; return; }
        plan->runs[plan->runCount++] = {f, x, y, sx, sy, Fade(c, alpha), start, n};
    }
    // `s` (n units; line breaks end it) at `size` square units of line height, the middle of its capitals at cy; align 0 left,
    // 1 centre, 2 right of x. Squeezed sideways to maxW (down to 78 %, then smaller). Pictures (U+E0xx) from the picture font.
    float Text(const char16_t* s, float x, float cy, float size, Rgba c, int align, float maxW, bool shadow = true, int n = -1, bool display = false) {
        if (n < 0) n = Len(s);
        for (int i = 0; i < n; i++)
            if (s[i] == u'\n') { n = i; break; }
        if (n <= 0) return 0;
        const Font face = Face(s, n, display);
        float w = Width(s, n, size, face), squeeze = 1.0f;
        if (maxW > 0 && w > maxW) {
            squeeze = maxW / w;
            if (squeeze < 0.78f) {
                size *= squeeze / 0.78f;
                squeeze = 0.78f;
                if (size < 9.0f) plan->textClipped = true;
            }
            w = Width(s, n, size, face) * squeeze;
        }
        const FontMetrics& m = fonts[face];
        const float scale = size / m.height;
        const float left = align == 0 ? x : (align == 1 ? x - w * 0.5f : x - w);
        // (the middle of the capitals: 17.4 of the message font's 33 rows; the display face by its baseline, the same proportion)
        const float mid = face == FONT_DISPLAY ? 17.4f / 26.0f * m.baseline : 17.4f * (m.height > 0 ? m.height / 33.0f : 1.0f);
        const float top = cy - mid * scale;
        const int at = Store(s, n);
        if (at < 0) return w;
        // runs of message characters, pictures one by one
        float px = left;
        int i = 0;
        while (i < n) {
            if (s[i] >= 0xE000 && s[i] < 0xE100) {
                Picture(static_cast<int>(s[i] - 0xE000), px + size * 1.08f * squeeze * 0.5f, cy, size * 1.12f);
                px += CharW(s[i], size) * squeeze;
                ++i;
                continue;
            }
            int j = i;
            float rw = 0;
            while (j < n && !(s[j] >= 0xE000 && s[j] < 0xE100)) rw += CharW(s[j], size, face), ++j;
            if (shadow) {
                Rgba sh = kShadow;
                sh.a = static_cast<uint8_t>(sh.a * (c.a / 255.0f));
                Run(face, px + size * 0.07f, top + size * 0.1f, scale * squeeze, scale, sh, at + i, j - i);
            }
            Run(face, px, top, scale * squeeze, scale, c, at + i, j - i);
            px += rw * squeeze;
            i = j;
        }
        return w;
    }
    // A picture of the picture font, `size` high, centred at (cx, cy) (the cell's picture is round 16, 16.5 of its 34 x 35).
    void Picture(int pic, float cx, float cy, float size) {
        const FontMetrics& m = fonts[FONT_PICTURE];
        const float scale = size / m.height;
        const char16_t code = static_cast<char16_t>(pictureCode(pic));
        const int at = Store(&code, 1);
        Run(FONT_PICTURE, cx - 15.0f * scale, cy - 16.5f * scale, scale, scale, kWhite, at, 1);
    }
};

void PlayerIcon(Builder& g, Writer& t, uint8_t icon, float cx, float cy, float r) {
    Rgba c;
    char16_t letter;
    switch (icon) {
    case ICON_MARIO: c = charColor(0), letter = u'M'; break;   // (touches: the one character table)
    case ICON_LUIGI: c = charColor(1), letter = u'L'; break;
    case ICON_TOAD: c = charColor(2), letter = u'T'; break;
    case ICON_COSMIC: c = charColor(3), letter = u'M'; break;
    case ICON_YELLOW_TOAD: c = charColor(4), letter = u'T'; break;
    case ICON_COSMIC_LUIGI: c = charColor(5), letter = u'L'; break;
    case ICON_DOT_ONLINE: g.Disc(cx, cy, r * 0.45f, kGreen); return;
    case ICON_DOT_ROOM: g.Disc(cx, cy, r * 0.45f, kYellow); return;
    case ICON_DOT_OFFLINE: g.Disc(cx, cy, r * 0.45f, kGrey); return;
    case ICON_FILE:
        g.Fill(cx - r * 0.7f, cy - r * 0.85f, cx + r * 0.7f, cy + r * 0.85f, r * 0.2f, Rgba{240, 244, 255, 240}, Rgba{190, 200, 240, 240}, 3);
        g.Rect(cx - r * 0.4f, cy - r * 0.35f, cx + r * 0.4f, cy - r * 0.22f, Rgba{60, 80, 170, 220});
        g.Rect(cx - r * 0.4f, cy - r * 0.02f, cx + r * 0.4f, cy + r * 0.11f, Rgba{60, 80, 170, 220});
        g.Rect(cx - r * 0.4f, cy + r * 0.31f, cx + r * 0.15f, cy + r * 0.44f, Rgba{60, 80, 170, 220});
        return;
    case ICON_SRC_WIFI: {   // flow: three arcs over a dot, in the window's blue
        const Rgba ink = {80, 110, 200, 255};
        g.Disc(cx, cy + r * 0.6f, r * 0.17f, ink, 0.5f);
        for (int k = 1; k <= 3; k++) {
            const float rr = r * 0.36f * k;
            for (int s = 0; s < 6; s++) {
                const float a0 = kPi * (1.25f + 0.5f * s / 6.0f), a1 = kPi * (1.25f + 0.5f * (s + 1) / 6.0f);
                g.Seg(cx + rr * cosf(a0), cy + r * 0.6f + rr * sinf(a0), cx + rr * cosf(a1), cy + r * 0.6f + rr * sinf(a1), r * 0.16f, ink);
            }
        }
        return;
    }
    case ICON_SRC_NEARBY: {   // flow: two handhelds side by side
        const Rgba ink = {80, 110, 200, 255}, screen = {200, 220, 255, 255};
        for (int k = -1; k <= 1; k += 2) {
            const float x = cx + k * r * 0.52f;
            g.Fill(x - r * 0.45f, cy - r * 0.32f, x + r * 0.45f, cy + r * 0.32f, r * 0.16f, ink, ink, 3);
            g.Rect(x - r * 0.24f, cy - r * 0.2f, x + r * 0.24f, cy + r * 0.18f, screen);
        }
        return;
    }
    case ICON_SRC_ONLINE: {   // flow: a globe (a disc, its equator and meridian)
        const Rgba ink = {80, 110, 200, 255}, light = {220, 232, 255, 255};
        g.Disc(cx, cy, r * 0.82f, ink, 0.6f);
        g.Disc(cx, cy, r * 0.66f, light, 0.4f);
        g.Seg(cx - r * 0.66f, cy, cx + r * 0.66f, cy, r * 0.12f, ink);
        g.Seg(cx, cy - r * 0.66f, cx, cy + r * 0.66f, r * 0.12f, ink);
        return;
    }
    case ICON_STAMP:
        g.Fill(cx - r, cy - r * 0.7f, cx + r, cy + r * 0.45f, r * 0.4f, kWhite, Rgba{214, 224, 255, 255}, 3);
        g.Tri({cx - r * 0.4f, cy + r * 0.3f, kWhite}, {cx - r * 0.55f, cy + r * 0.95f, kWhite}, {cx, cy + r * 0.3f, kWhite});
        return;
    default: return;
    }
    g.Disc(cx, cy, r + 1.2f, Rgba{255, 255, 255, 235});
    g.Disc(cx, cy, r, c);
    const char16_t s[2] = {letter, 0};
    t.Text(s, cx, cy + 0.5f, r * 1.45f, kWhite, 1, 0, false);
}

}  // namespace

void copy(char16_t* dst, int cap, const char16_t* src) {
    int n = 0;
    for (; src && src[n] && n < cap - 1; n++) dst[n] = src[n];
    if (cap > 0) dst[n] = 0;
}

bool copyChanged(char16_t* dst, int cap, const char16_t* src) {
    int n = 0;
    bool changed = false;
    for (; src && src[n] && n < cap - 1; n++) {
        if (dst[n] != src[n]) changed = true;
        dst[n] = src[n];
    }
    if (cap > 0) {
        if (dst[n] != 0) changed = true;
        dst[n] = 0;
    }
    return changed;
}

void clear(Model* m) {
    *m = Model();
    memset(m->title, 0, sizeof m->title);
    memset(m->subtitle, 0, sizeof m->subtitle);
    memset(m->tabs, 0, sizeof m->tabs);
    memset(m->bigLabel, 0, sizeof m->bigLabel);
    memset(m->big, 0, sizeof m->big);
    memset(m->rows, 0, sizeof m->rows);
    memset(m->status, 0, sizeof m->status);
    memset(m->hint, 0, sizeof m->hint);
    memset(m->buttons, 0, sizeof m->buttons);
    m->kind = KIND_LIST;
    m->alpha = 1.0f;
    m->cursor = -1;
    m->spinner = -1.0f;
    m->progress = -1.0f;
    for (Row& r : m->rows) r.ping = -1, r.owner = -1;
    for (int8_t& c : m->cursors) c = -1;
    m->localSlot = -1;
    memset(m->cells, 0, sizeof m->cells);   // chat
    m->cellCount = 0;
    m->cellCols = 3;
    memset(m->line, 0, sizeof m->line);     // flow look
    m->look = LOOK_PANEL;
    m->headCount = 0;
}

// memfix: the highest use of each plan (the tests check the plans' capacities against the biggest real screens). Three compares per
// compose call; the destructor notes what the call left in the plan (a call may return early).
namespace {
PlanPeak sPeak[PLAN_KINDS];
struct PeakNote {
    const PlanBase* plan;
    PlanKind kind;
    ~PeakNote() {
        PlanPeak& p = sPeak[kind];
        if (plan->vertexCount > p.vertices) p.vertices = plan->vertexCount;
        if (plan->runCount > p.runs) p.runs = plan->runCount;
        if (plan->textCount > p.text) p.text = plan->textCount;
    }
};
}  // namespace
PlanPeak planPeak(PlanKind kind) { return sPeak[kind]; }
void resetPlanPeaks() { for (PlanPeak& p : sPeak) p = PlanPeak{0, 0, 0}; }

PlanBase::PlanBase(Vertex* v, int nv, TextRun* r, int nr, char16_t* t, int nt) : vertices(v), maxVertices(nv), runs(r), maxRuns(nr), text(t), maxText(nt) {}

void begin(PlanBase* plan) {
    plan->vertexCount = 0;
    plan->runCount = 0;
    plan->textCount = 0;
    plan->overflow = false;
    plan->textClipped = false;
    plan->x0 = plan->y0 = plan->x1 = plan->y1 = 0;
    plan->hitCount = 0;   // chat
    plan->guideHitCount = 0;   // vitabtn
    plan->arrowHitCount = 0;   // settingsnative
}

namespace {
// uinative2: where the toasts start (square units, y down): the picture's top, or just under the title band of a menu composed this
// frame (on the real screen a toast sat over the session menu's title). beginScene puts it back; compose moves it down.
float sToastTop = 14.0f;
bool sNoPointer = false;   // uinative3: a native window is up over the menu
}  // namespace

void beginScene(Scene* scene) {
    sToastTop = 14.0f;
    scene->useCount = 0;
    scene->textCount = 0;
    scene->headCount = 0;
    scene->screenKey = 0;   // uinative: (no menu this frame: key 0, nothing decided)
    scene->decided = -1;
}

float layoutToSquareX(const Screen& s, float x) { return s.width * 0.5f + x * (s.width / 608.0f); }
float layoutToSquareY(const Screen& s, float y) { return s.height * 0.5f - y; }

namespace {

// ink of the game's message windows (SysInfoWindow's TxtConfirm: 80, 90, 130)
constexpr Rgba kInk = {80, 90, 130, 255};
constexpr Rgba kInkDim = {150, 158, 190, 255};
constexpr Rgba kInkValue = {50, 80, 175, 255};
constexpr Rgba kInkGold = {214, 150, 10, 255};
constexpr Rgba kRowSelTop = {255, 252, 226, 235}, kRowSelBottom = {255, 236, 168, 235};
constexpr Rgba kRowSelRim = {255, 196, 60, 230};
constexpr Rgba kRowRule = {120, 140, 200, 60};

int AddText(Scene* sc, const char16_t* a, const char16_t* sep = nullptr, const char16_t* b = nullptr) {
    if (sc->textCount >= kSceneTexts) return -1;
    char16_t* t = sc->texts[sc->textCount];
    int n = 0;
    for (const char16_t* p = a; p && *p && n < kSceneText - 1; ++p) t[n++] = *p;
    if (b && b[0]) {
        for (const char16_t* p = sep; p && *p && n < kSceneText - 1; ++p) t[n++] = *p;
        for (const char16_t* p = b; *p && n < kSceneText - 1; ++p) t[n++] = *p;
    }
    t[n] = 0;
    return sc->textCount++;
}

LayoutUse* AddUse(Scene* sc, UseKind kind, int slot) {
    if (sc->useCount >= kMaxUses) return nullptr;
    LayoutUse& u = sc->uses[sc->useCount++];
    u = LayoutUse();
    u.kind = kind;
    u.slot = static_cast<uint8_t>(slot);
    u.text = u.text2 = -1;
    u.alpha = 1.0f;
    return &u;
}

bool txtIsPic(char16_t c) { return c >= 0xE000 && c < 0xE100; }
bool Tabbed(const char16_t* v) { for (; *v; ++v) if (*v == u'\t') return true; return false; }
int Cells(const char16_t* v) { int n = 1; for (; *v; ++v) if (*v == u'\t') ++n; return n; }

bool IsHead(uint8_t icon) { return icon >= ICON_MARIO && icon <= ICON_COSMIC_LUIGI; }

// A character head (MiiIcon, 100 x 100 units) at square-unit position (x, y), `size` square units across.
bool Head(Scene* sc, const Screen& scr, uint8_t look, float x, float y, float size, float alpha, int limit) {
    if (sc->headCount >= limit) return false;
    LayoutUse* u = AddUse(sc, USE_HEAD, sc->headCount++);
    if (!u) return false;
    u->state = look;
    u->x = (x - scr.width * 0.5f) * (608.0f / scr.width);
    u->y = scr.height * 0.5f - y;
    u->w = size / 100.0f;
    u->alpha = alpha;
    return true;
}

// ------------------------------------------------------------------ uinative: the game's furniture, placed
// Square units (y down) -> the 608 x 456 layout's units (y up, origin in the middle).
float SqToLayoutX(const Screen& s, float x) { return (x - s.width * 0.5f) * (608.0f / s.width); }
float SqToLayoutY(const Screen& s, float y) { return s.height * 0.5f - y; }

// The Star Pointer's hand (DPDPointer), its fingertip at (x, y) square units; turn -90: the finger points right.
void Pointer(Scene* sc, const Screen& scr, float x, float y, float scale, float turn, float alpha) {
    if (sNoPointer) return;
    LayoutUse* u = AddUse(sc, USE_POINTER, 0);
    if (!u) return;
    u->x = SqToLayoutX(scr, x), u->y = SqToLayoutY(scr, y);
    u->w = scale, u->h = turn;
    u->alpha = alpha;
}

// The Mii select's arrows either side of a value: the tips at x0 and x1 (square units), centred on cy.
void ValueArrows(Scene* sc, const Screen& scr, float x0, float x1, float cy, float scale, float alpha) {
    LayoutUse* u = AddUse(sc, USE_ARROWS, 0);
    if (!u) return;
    const float l0 = SqToLayoutX(scr, x0), l1 = SqToLayoutX(scr, x1);
    u->x = (l0 + l1) * 0.5f, u->w = (l1 - l0) * 0.5f;
    u->y = SqToLayoutY(scr, cy);
    u->h = scale;
    u->alpha = alpha;
}

// The star list's page number between its round up / down buttons, in a column at x (square units) from y0 to y1.
void PageColumn(Scene* sc, const Screen& scr, const Model& m, float x, float y0, float y1, float alpha) {
    if (m.pages <= 1) return;
    LayoutUse* u = AddUse(sc, USE_SCROLL, 0);
    if (!u) return;
    const int page = m.page < 0 ? 0 : (m.page >= m.pages ? m.pages - 1 : m.page);
    char16_t num[12];
    int n = 0;
    if (page + 1 >= 10) num[n++] = static_cast<char16_t>(u'0' + (page + 1) / 10 % 10);
    num[n++] = static_cast<char16_t>(u'0' + (page + 1) % 10);
    num[n++] = u'/';
    if (m.pages >= 10) num[n++] = static_cast<char16_t>(u'0' + m.pages / 10 % 10);
    num[n++] = static_cast<char16_t>(u'0' + m.pages % 10);
    num[n] = 0;
    u->text = static_cast<int8_t>(AddText(sc, num));
    u->x = SqToLayoutX(scr, x);
    u->y = SqToLayoutY(scr, (y0 + y1) * 0.5f);
    u->h = (y1 - y0) * 0.5f - 34.0f;   // (the buttons' middles, 32 inside the column's ends)
    u->state = static_cast<uint8_t>((page > 0 ? 1 : 0) | (page + 1 < m.pages ? 2 : 0));
    u->alpha = alpha;
}

// The button guide ("[Cross] Select  [Circle] Back") on the lower band, centred, white with the game's shadow: a picture of the
// button then its word, a wide gap between two. Squeezed (smaller) only when a language's words do not fit the picture's width.
void Guide(Writer& t, const Model& m, const Screen& scr, PlanBase* plan) {
    if (m.buttonCount <= 0) return;
    float size = 22.0f;
    auto widthAt = [&](float s, float* widths) {
        float total = 0;
        for (int i = 0; i < m.buttonCount && i < 4; i++) {
            widths[i] = s * 1.45f + t.Width(m.buttons[i].label, Len(m.buttons[i].label), s);
            total += widths[i] + (i > 0 ? s * 1.6f : 0.0f);
        }
        return total;
    };
    float widths[4];
    float total = widthAt(size, widths);
    const float room = scr.width - 2.0f * 36.0f;
    if (total > room) {
        size *= room / total;
        total = widthAt(size, widths);
    }
    const float cy = layoutToSquareY(scr, kFootLine);
    float x = scr.width * 0.5f - total * 0.5f;
    plan->guideX0 = x, plan->guideX1 = x + total;
    for (int i = 0; i < m.buttonCount && i < 4; i++) {
        t.Picture(m.buttons[i].pic, x + size * 0.6f, cy, size * 1.4f);
        t.Text(m.buttons[i].label, x + size * 1.45f, cy, size, kWhite, 0, 0, true);
        // vitabtn: a touch on the picture or its word is that button (a finger is at least a 40-unit target: the box is padded)
        if (plan->guideHitCount < 4)
            plan->guideHits[plan->guideHitCount++] = {x - size * 0.4f, (cy - kGuideBandH * 0.5f < scr.height - kGuideBandH ? cy - kGuideBandH * 0.5f : scr.height - kGuideBandH), x + widths[i] + size * 0.4f, scr.height, m.buttons[i].pic};
        x += widths[i] + size * 1.6f;
    }
    plan->hintRuns = plan->runCount;
}

// The buttons inside an overlay's window (the results card's "Continue"), right-aligned at (right, cy), in the window's ink.
void WindowHints(Writer& t, const Model& m, float right, float cy, float size) {
    float total = 0, widths[4];
    for (int i = 0; i < m.buttonCount && i < 4; i++) {
        widths[i] = size * 1.2f + t.Width(m.buttons[i].label, Len(m.buttons[i].label), size) + size * 1.1f;
        total += widths[i];
    }
    float x = right - total;
    for (int i = 0; i < m.buttonCount && i < 4; i++) {
        t.Picture(m.buttons[i].pic, x + size * 0.5f, cy, size * 1.25f);
        t.Text(m.buttons[i].label, x + size * 1.2f, cy, size, kInk, 0, 0, false);
        x += widths[i];
    }
}

// The rows of a list inside a window whose inside is (x0, y0) - (x1, ...) in square units. Heads go to the scene; the selected
// row gets the Star Pointer's hand at its left and, for a value that changes with left / right, the Mii select's arrows.
void Rows(Builder& g, Writer& t, const Model& m, const Screen& scr, float x0, float y0, float x1, float stdH, float choiceH, Scene* sc, bool cursorFurniture) {
    float yy = y0;
    for (int i = 0; i < m.rowCount && i < kMaxRows; i++) {
        const Row& r = m.rows[i];
        const float rowH = (r.marks & MARK_CHOICE) ? choiceH : stdH;   // lobby: a card's live choices are kListRowH high whatever the others had to give
        const float lh = rowH < 48.0f ? rowH : 48.0f;   // uxfix #8: a taller row (67 to touch) keeps the words' size (27): it is the target that grows
        const float k = lh / 32.0f;   // (sizes drawn for 32-unit rows)
        const float ry0 = yy, ry1 = ry0 + rowH - 2.0f, cy = (ry0 + ry1) * 0.5f;
        yy += rowH;
        const bool sel = i == m.cursor;
        const bool dis = (r.marks & MARK_DISABLED) != 0;
        if (g.plan->hitCount < kMaxCells) g.plan->hits[g.plan->hitCount++] = {x0, ry0, x1, ry1 + 2.0f};   // vitabtn: the row answers a touch
        if (sel) {
            const float grow = 1.5f * m.pulse + ((r.marks & MARK_CHOICE) ? 2.5f : 0.0f);   // (a card's choice swells a little, as the big buttons do)
            g.Ring(x0 - grow, ry0 - grow, x1 + grow, ry1 + grow, rowH * 0.45f, 0.0f, Rgba{255, 210, 80, 110}, 4.0f, Rgba{255, 210, 80, 0}, 4);
            g.Fill(x0 - grow, ry0 - grow, x1 + grow, ry1 + grow, rowH * 0.45f, kRowSelTop, kRowSelBottom, 4);
            g.Ring(x0 - grow, ry0 - grow, x1 + grow, ry1 + grow, rowH * 0.45f, -1.6f, kRowSelRim, 0.0f, kRowSelRim, 4);
            if (cursorFurniture) Pointer(sc, scr, x0 - 2.0f - 4.0f * m.pulse, cy + 2.0f, 0.95f, -90.0f, m.alpha);
        } else if (r.marks & MARK_HEADER) {
            g.Fill(x0 + 2, ry0 + 1, x1 - 2, ry1 - 1, rowH * 0.4f, Rgba{255, 255, 255, 90}, Rgba{255, 255, 255, 60}, 3);
        } else if (i + 1 < m.rowCount && !(m.rows[i + 1].marks & MARK_HEADER) && i + 1 != m.cursor) {
            g.Line(x0 + 12, x1 - 12, ry1 + 1.0f, 1.0f, kRowRule);
        }
        float x = x0 + 16.0f * k;
        const Rgba ink = dis ? kInkDim : ((r.marks & MARK_YOU) ? kInkValue : kInk);
        const float size = lh * 0.62f;
        if (r.lead[0]) {
            t.Text(r.lead, x + 15.0f * k, cy, size * 0.95f, (r.marks & MARK_GOLD) ? kInkGold : kInkDim, 1, 36.0f * k, false);
            x += 38.0f * k;
        }
        if (IsHead(r.icon)) {
            if (!Head(sc, scr, static_cast<uint8_t>(r.icon - ICON_MARIO), x + 16.0f * k, cy, lh * 0.95f, m.alpha, kToastHeadBase))
                PlayerIcon(g, t, r.icon, x + 16.0f * k, cy, lh * 0.3f);
            x += 40.0f * k;
            // touches: the player's colour (the same one as the name tag and the pointer) as a pip before the name, dark rim round it
            const float bx = x - 3.0f * k, bh = lh * 0.28f;
            g.Fill(bx - 3.0f * k, cy - bh, bx + 3.0f * k, cy + bh, 3.0f * k, charEdge(r.icon - ICON_MARIO), charEdge(r.icon - ICON_MARIO), 3);
            g.Fill(bx - 2.0f * k, cy - bh + 1.0f, bx + 2.0f * k, cy + bh - 1.0f, 2.0f * k, charColor(r.icon - ICON_MARIO), charColor(r.icon - ICON_MARIO), 3);
            x += 6.0f * k;
        } else if (r.icon) {
            PlayerIcon(g, t, r.icon, x + 10.0f * k, cy, lh * 0.3f);
            x += 27.0f * k;
        }
        float rx = x1 - 14.0f * k;
        if (r.ping >= 0) {
            g.Bars(rx - 20.0f * k, cy, lh * 0.5f, r.ping, (r.marks & MARK_LAG) != 0);
            rx -= 30.0f * k;
        }
        if (r.marks & MARK_VERIFIED) {
            g.Tick(rx - 8.0f * k, cy, lh * 0.42f, Rgba{40, 170, 70, 255});
            rx -= 22.0f * k;
        } else if (r.marks & MARK_BAD) {
            g.Cross(rx - 8.0f * k, cy, rowH * 0.4f, Rgba{220, 60, 50, 255});
            rx -= 22.0f * k;
        }
        if (r.marks & MARK_NAV) {   // settingsnative: a page opens from this row
            t.Text(u">", rx - 4.0f * k, cy, size * 1.05f, dis ? kInkDim : kInkValue, 2, 0, false);
            rx -= 26.0f * k;
        }
        float valueW = 0;
        const bool arrows = (r.marks & MARK_ARROWS) != 0 && sel && (cursorFurniture || m.selectedArrows);
        if (r.value[0] && Tabbed(r.value)) {
            // columns (the star card's counts, the results card's time and series points): cells split at '\t', each a picture then its
            // number, picture left, number right. A cell with no picture is a text (a time): wider.
            const int cells = Cells(r.value);
            float cw[8];
            int nc = 0;
            for (const char16_t* p = r.value; nc < cells && nc < 8; nc++) {
                cw[nc] = (txtIsPic(*p) ? 64.0f : 100.0f) * k;
                while (*p && *p != u'\t') ++p;
                if (*p) ++p;
            }
            int c = 0;
            float behind = 0;
            for (int j = 0; j < nc; j++) behind += cw[j];
            float acc = 0;   // (the width of the cells to the right of this one)
            float cellRight[8];
            for (int j = nc - 1; j >= 0; j--) { cellRight[j] = rx - acc; acc += cw[j]; }
            for (const char16_t* p = r.value; c < nc; c++) {
                const char16_t* e = p;
                while (*e && *e != u'\t') ++e;
                const float right = cellRight[c], left = right - cw[c] + 6.0f;
                int skip = 0;
                if (txtIsPic(*p)) {
                    Writer pw{t.plan, t.fonts, t.alpha};
                    pw.Picture(*p - 0xE000, left + size * 0.55f, cy, size * 1.1f);
                    skip = 1;
                }
                t.Text(p + skip, right - 4.0f, cy, size * 0.95f, dis ? kInkDim : kInkValue, 2, 0, false, static_cast<int>(e - p) - skip);
                p = *e ? e + 1 : e;
            }
            valueW = behind;
        } else if (r.value[0]) {
            const float aw = 13.0f * k;   // (room for an arrow: the Mii select's, 0.42 of its 40 x 72)
            const float vx = arrows ? rx - aw - 6.0f : rx;
            valueW = t.Text(r.value, vx, cy, size * (arrows ? 0.84f : 0.92f), dis ? kInkDim : kInkValue, 2, (x1 - x0) * 0.46f, false);
            if (arrows) {
                const float nudge = 2.0f * m.pulse;
                const float ax0 = vx - valueW - 6.0f - aw - nudge, ax1 = rx + nudge;
                ValueArrows(sc, scr, ax0, ax1, cy, 0.42f * k * 0.75f, m.alpha);
                if (g.plan->arrowHitCount == 0) {   // settingsnative: a finger needs 40 units: the boxes are padded round the two arrows
                    const float cl = ax0 + aw * 0.5f, cr = ax1 - aw * 0.5f;
                    g.plan->arrowHits[0] = {cl - 20.0f, ry0, cl + 20.0f, ry1 + 2.0f};
                    g.plan->arrowHits[1] = {cr - 20.0f, ry0, cr + 20.0f, ry1 + 2.0f};
                    g.plan->arrowHitCount = 2;
                }
                valueW += 2.0f * aw + 12.0f;
            }
        }
        const float nameW = t.Text(r.label, x, cy, size, ink, 0, rx - valueW - x - ((r.marks & MARK_CROWN) ? 36.0f * k : 12.0f * k), false);
        if (r.marks & MARK_CROWN) g.Crown(x + nameW + 14.0f * k, cy - 1.0f, lh * 0.5f, Rgba{255, 205, 30, 255});
    }
}

}  // namespace

void compose(const Model& m, const Screen& scr, const FontMetrics fonts[FONT_COUNT], float bob, Scene* sc, PlanBase* plan) {
    PeakNote peakNote{plan, PLAN_MENU};
    Builder g{plan, m.alpha};
    Writer t{plan, fonts, m.alpha};
    const float W = scr.width, midX = W * 0.5f;
    const float float2 = sinf(bob * 2.0f * kPi) * 2.0f;   // the windows float a little, like the game's buttons
    const bool overlay = m.kind == KIND_WAITING || m.kind == KIND_RESULTS;
    const bool game = m.look == LOOK_GAME;   // flow look: the file select's look (no bands' words, the line, no hand)
    sNoPointer = m.dialogUp || game;
    sc->screenKey = m.screenKey;
    sc->decided = m.decided;
    // the content's room (layout y): under the title band (slimmer when the title stands alone), over the guide band
    const char16_t* bandLine = m.hint[0] ? m.hint : (m.kind == KIND_BUTTONS ? u"" : m.subtitle);
    if (m.kind == KIND_BUTTONS || m.kind == KIND_CHARS) bandLine = u"";   // uinative4: (the title and the mode in one line; no description line)
    if (m.kind != KIND_BUTTONS && m.hint[0] && m.subtitle[0]) bandLine = m.subtitle;   // (lists: the subtitle; the hint goes in the window)
    if (game) bandLine = u"";
    const bool bare = game || (m.kind == KIND_BUTTONS && m.title[0] == 0 && m.subtitle[0] == 0);   // (no title: no title band)
    const bool tight = (m.kind == KIND_BUTTONS && !bare) || m.kind == KIND_CHARS;   // uinative4: big-button screens: the title alone in a slimmer band (more room)
    const float top = overlay ? kContentTop : (bare ? (game && m.line[0] && ((m.kind == KIND_BUTTONS && m.rowCount <= 2) || (m.kind == KIND_LIST && m.rowCount <= 2)) ? kLookTop : kContentTopBare) : (tight ? kContentTopAlone + kBandTight : (bandLine[0] ? kContentTop : kContentTopAlone))),
                bottom = game && m.line[0] ? kLookBottom : (m.zoomRow ? kZoomRowTop : (m.buttonCount > 0 ? kContentBottom : -214.0f));   // (flow look: over the line)
    const bool lowLook = game && m.line[0] && ((m.kind == KIND_BUTTONS && m.rowCount <= 2) || (m.kind == KIND_LIST && m.rowCount <= 2 && !m.big[0] && m.tabCount == 0));   // (few buttons / rows with a line: the picture's middle)
    const float midL = lowLook ? kLookMid : (top + bottom) * 0.5f;

    // ---- the frame: the pause menu's dim background, its title band (title, scenario line) and its lower band (the guide)
    if (!overlay) {
        LayoutUse* f = AddUse(sc, USE_FRAME, 0);
        if (f) {
            f->alpha = m.alpha;
            f->text = static_cast<int8_t>(game ? AddText(sc, u"") : m.kind == KIND_BUTTONS ? AddText(sc, m.title, u" · ", m.subtitle) : AddText(sc, m.title));
            const char16_t* line = bandLine;
            f->text2 = static_cast<int8_t>(AddText(sc, line));
            f->state = static_cast<uint8_t>(kFrameDim | (m.buttonCount > 0 && !game ? kFrameFoot : 0) | (line[0] ? 0 : kFrameNoLine) | (bare ? kFrameNoTitle : 0) | (tight ? kFrameTight : 0));
            // the band's words at the pause menu's sizes (33 and 26.4 high, 2 units between characters), smaller only when wider
            // than the picture's room
            const float room = W - kBandMargin;   // (the widened text boxes, MpPanelDraw)
            auto boxWidth = [&](int text, float size) {
                if (text < 0) return 0.0f;
                const int n = Len(sc->texts[text]);
                return n > 0 ? t.Width(sc->texts[text], n, size) + 2.0f * (n - 1) : 0.0f;
            };
            const float tw = boxWidth(f->text, 33.0f), lw = boxWidth(f->text2, 26.4f);
            f->w = tw > room ? room / tw : 1.0f;
            f->h = lw > room ? room / lw : 1.0f;
            if (!bare && m.alpha > 0.05f)
                sToastTop = layoutToSquareY(scr, kBandBottom + (line[0] ? 0.0f : kBandShrink) + (tight ? kBandTight : 0.0f)) + 6.0f;   // (the toasts under the band)
        }
        Guide(t, m, scr, plan);
    }
    if (game && m.line[0] && !overlay) {
        // flow look: the screen's one question or line in the game's own bottom message
        LayoutUse* l = AddUse(sc, USE_LINE, 0);
        if (l) {
            l->text = static_cast<int8_t>(AddText(sc, m.line));
            l->x = 0.0f;
            l->y = kLineY;
            l->state = kLineCentred;   // (menus5)
            l->alpha = m.alpha;
        }
    }

    if (m.kind == KIND_HEADS) {
        // ---- flow look: the players as heads (the character select's heads, 3D where the file select's models are at hand), up to
        // four in a row across the upper part, each name under it in the character's colour; a crown by the first player's head. The big buttons (the rows after the heads) under them, as KIND_BUTTONS.
        const int heads = m.headCount < 4 ? (m.headCount < 0 ? 0 : m.headCount) : 4;
        const int nButtons = m.rowCount - heads;
        // (no button under them: the heads in the middle and bigger, as the first game pictures showed a lone row at the top)
        // (menus2: with the far-away code on the screen the heads shrink to a row at the top and the code is the biggest thing)
        const bool coded = m.big[0] != 0;
        // lobby: three big buttons under the heads (a lone player: Invite from Far Away, Play with Anyone, Look for Consoles) need 231 units to stay at
        // full size (>= 0.9): the lone head is small and close to the top then
        const bool packed = !coded && nButtons >= 3 && heads <= 1;
        const float hy = coded ? top - 36.0f : (packed ? top - 14.0f : (nButtons > 0 ? top - 50.0f : (top + bottom) * 0.5f + 20.0f));
        const float headScale = coded ? 0.55f : (packed ? 0.5f : (nButtons > 0 ? 0.8f : 1.1f));
        const float gapX = 145.0f;   // (four heads and their names inside the 4:3 middle: 608 units)
        for (int i = 0; i < heads && i < m.rowCount; i++) {
            const Row& r = m.rows[i];
            const float hx = (i - (heads - 1) * 0.5f) * gapX;
            const bool waiting = (r.marks & MARK_DISABLED) != 0;
            LayoutUse* u = AddUse(sc, USE_CHAR, r.owner >= 0 && r.owner < 4 ? r.owner : i);   // (owner: the player's own slot, kept)
            if (u) {
                const uint8_t look = r.icon >= ICON_MARIO ? static_cast<uint8_t>(r.icon - ICON_MARIO) : 0;
                u->state = static_cast<uint8_t>(look | kCharStill | (waiting ? kCharTaken : 0) | ((r.marks & MARK_ARRIVED) ? kCharArriving : 0));
                const float ph = (bob + i * 0.23f) * 2.0f * kPi;
                u->x = hx;
                u->y = hy + 4.0f * sinf(ph);   // (floating, each in its own phase)
                u->w = headScale;
                u->alpha3d = m.alpha;   // (heads: the 3D head's own alpha and place; no owner badge on a players' row: LOOK.md)
                u->y3d = hy;
                u->alpha = waiting ? m.alpha * (kGhostAlpha + kGhostPulse * sinf(ph)) : m.alpha;
            }
            const Rgba c = r.icon >= ICON_MARIO ? charColor(r.icon - ICON_MARIO) : kWhite;
            const float nameY = hy - 60.0f * headScale / 0.8f, nameSize = coded || packed ? 20.0f : (nButtons > 0 ? 22.0f : 26.0f);
            t.Text(r.label, layoutToSquareX(scr, hx), layoutToSquareY(scr, nameY), nameSize, waiting ? Rgba{170, 175, 200, 255} : c, 1, gapX - 16.0f);
            if ((r.marks & MARK_YOU) && r.value[0] && !waiting) {   // uxfix #9: the "You" mark: a small plate under your own name
                const float pw0 = t.Width(r.value, Len(r.value), nameSize * 0.72f) + 22.0f, pw = pw0 < gapX - 24.0f ? pw0 : gapX - 24.0f;
                const float py = layoutToSquareY(scr, nameY - 26.0f), px = layoutToSquareX(scr, hx);
                g.Fill(px - pw * 0.5f, py - 11.0f, px + pw * 0.5f, py + 11.0f, 11.0f, Rgba{c.r, c.g, c.b, 220}, Rgba{static_cast<uint8_t>(c.r * 0.7f), static_cast<uint8_t>(c.g * 0.7f), static_cast<uint8_t>(c.b * 0.7f), 220}, 4);
                g.Ring(px - pw * 0.5f, py - 11.0f, px + pw * 0.5f, py + 11.0f, 11.0f, -1.2f, Rgba{255, 255, 255, 200}, 0.0f, Rgba{255, 255, 255, 200}, 4);
                t.Text(r.value, px, py, nameSize * 0.72f, kWhite, 1, pw - 10.0f);
            }
            if (r.marks & MARK_CROWN) {   // (the first player: the one who starts: a crown before the name, as the lists show it)
                const float nw = t.Width(r.label, Len(r.label), nameSize);
                const float nwc = nw < gapX - 16.0f ? nw : gapX - 16.0f;
                g.Crown(layoutToSquareX(scr, hx) - nwc * 0.5f - 14.0f, layoutToSquareY(scr, nameY) - 1.0f, nameSize * 0.5f, Rgba{255, 205, 30, 255});
            }
        }
        // the far-away code (Model::big): the biggest thing on the screen, in the game's font and yellow, one group to a line (read aloud
        // a group at a time), centred in the room under the names
        float codeBottom = hy - (packed ? 46.0f : 74.0f);
        if (m.big[0]) {
            char16_t grp[6][24];
            int gl[6], ng = 0;
            {
                int from = 0;
                const int len = Len(m.big);
                for (int i = 0; i <= len && ng < 6; i++) {
                    if (i == len || m.big[i] == u'-' || m.big[i] == u' ') {
                        const int n = i - from < 23 ? i - from : 23;
                        if (n > 0) { memcpy(grp[ng], m.big + from, n * sizeof(char16_t)); grp[ng][n] = 0; gl[ng++] = n; }
                        from = i + 1;
                    }
                }
                if (ng == 1 && gl[0] > 6) {   // (a code with no dashes: groups of five)
                    char16_t whole[24];
                    const int n = gl[0];
                    memcpy(whole, grp[0], (n + 1) * sizeof(char16_t));
                    ng = 0;
                    for (int i = 0; i < n && ng < 6; i += 5) {
                        const int k = n - i < 5 ? n - i : 5;
                        memcpy(grp[ng], whole + i, k * sizeof(char16_t)); grp[ng][k] = 0; gl[ng++] = k;
                    }
                }
            }
            const int nBelow = m.rowCount - heads < 3 ? (m.rowCount - heads < 0 ? 0 : m.rowCount - heads) : 3;   // (a button that still shows keeps its room under the code)
            // B6b: under the names and the "You" plate (its foot is 52 under the name's line), then the caption when there is one
            bool youPlate = false;
            for (int i = 0; i < heads && i < m.rowCount; i++) youPlate = youPlate || ((m.rows[i].marks & MARK_YOU) && m.rows[i].value[0]);
            const float capH = m.bigLabel[0] ? 26.0f : 0.0f;
            // 13 Nov (A2): a one-row (5-character) code with the prompt band under it is centred between the caption's text and the band's
            // visible top (7 units under kLookBottom in the 13 Nov pictures: px 385 of 544); it stood 14 px high (the area ended at kLookBottom
            // + 8, and the big face's letters sit 0.06 of their size above the line's middle: compensated below). Several rows keep the old area.
            const bool oneRowUnderBand = game && m.line[0] && nBelow == 0 && ng == 1;
            float areaTop = hy - (youPlate ? 84.0f : 58.0f) - capH, areaBottom = bottom + 8.0f + 54.0f * nBelow;
            const float captionTop = areaTop;   // (the caption keeps its place; only the code's area changes)
            if (oneRowUnderBand) areaTop += capH > 0.0f ? 6.0f : 0.0f, areaBottom = kLookBottom - 7.0f;
            if (m.bigLabel[0]) t.Text(m.bigLabel, midX, layoutToSquareY(scr, captionTop + capH * 0.5f), 19.0f, kWhite, 1, (W < 608.0f ? W : 608.0f) - 40.0f);
            float step = (areaTop - areaBottom) / static_cast<float>(ng > 0 ? ng : 1);
            if (step > (ng == 1 ? 120.0f : 66.0f)) step = ng == 1 ? 120.0f : 66.0f;   // code5: a 5-character code is ONE row, as big as the room lets it be
            float size = step * 0.95f;
            const float room = (W < 608.0f ? W : 608.0f) - 40.0f;
            // (the code is drawn in the game's big menu face when it has every character of every row: sharper than the message font at 4x)
            bool display = true;
            for (int i = 0; i < ng; i++) display = display && t.Face(grp[i], gl[i], true) == FONT_DISPLAY;
            const Font face = display ? FONT_DISPLAY : FONT_MESSAGE;
            for (int i = 0; i < ng; i++) {   // (a group wider than the room: every line smaller alike)
                const float w = t.Width(grp[i], gl[i], size, face);
                if (w > room) size *= room / w;
            }
            const float first = areaTop - ((areaTop - areaBottom) - step * ng) * 0.5f - step * 0.5f;
            // (the display face's letters sit above the middle the plan assumes: one row 0.061 of its size; rows under the band: 7 units down, the
            // 13 Nov picture had 3 rows at px 200-352 between the caption (183) and the band (385): 8 px high of the middle)
            const float bias = oneRowUnderBand ? (display ? size * 0.061f : 0.0f) : (game && m.line[0] && nBelow == 0 ? 7.0f : 0.0f);
            for (int i = 0; i < ng; i++) t.Text(grp[i], midX, layoutToSquareY(scr, first - i * step - bias), size, kYellow, 1, room, true, gl[i], display);
            codeBottom = areaBottom;
        }
        // the big buttons under the heads (and the code)
        const int first = heads, n = m.rowCount - first < 3 ? m.rowCount - first : 3;
        const float btop = codeBottom;   // (under the names)
        const float room = btop - bottom - 10.0f;
        float scale = n > 0 ? room / (kButtonH * (1.15f + (1.0f + kButtonGap) * (n - 1))) : 1.0f;
        if (scale > 1.0f) scale = 1.0f;
        float spacing = n > 1 ? (room - kButtonH * 1.15f * scale) / (n - 1) : 0.0f;
        if (spacing > 96.0f * scale) spacing = 96.0f * scale;
        const float mid = (btop + bottom) * 0.5f;
        for (int k = 0; k < n; k++) {
            const int i = first + k;
            LayoutUse* b = AddUse(sc, USE_BUTTON, k);
            if (!b) break;
            const Row& r = m.rows[i];
            b->text = static_cast<int8_t>(AddText(sc, r.label));
            b->state = (r.marks & MARK_DISABLED) ? BUTTON_DISABLED : (i == m.cursor ? BUTTON_SELECTED : BUTTON_IDLE);
            const float by = mid + (n - 1) * spacing * 0.5f - k * spacing;
            b->x = 0.0f;
            b->y = by - 48.0f * scale;
            b->w = scale;
            b->alpha = m.alpha;
        }
        const float half = n > 0 ? (n - 1) * spacing * 0.5f + kButtonH * 0.5f * scale : 0.0f;
        plan->x0 = midX - 176.0f * scale, plan->x1 = midX + 176.0f * scale;
        plan->y0 = layoutToSquareY(scr, n > 0 ? mid + half : top);
        plan->y1 = layoutToSquareY(scr, n > 0 ? mid - half : bottom);
        return;
    }

    if (m.kind == KIND_CHARS) {
        // ---- six big floating character heads (MiiIcon, the file select's icon picker), two rows of three; the name under each,
        // the player who has it on a plate in its slot colour; a character another player has is its ghost
        const float colX[3] = {-180.0f, 0.0f, 180.0f};   // layout units
        // uinative2: heads 80 units across, the name and the owner's plate under each clear of the head (and of the head under
        // it): rows 137 apart, the lower plate just above the guide band (the real screen had "Luigi" on the chin)
        // uinative4: the slim band gives more room: heads 90 across (the selected one grows 1.1: 99), rows 164 apart, the name 66 and
        // the owner's plate 90 under the head's middle (the plate's foot at 101): a head that grows (the 3D heads) stays clear
        const float scale = 0.9f;
        const float rowY[2] = {midL + 102.0f, midL - 62.0f};
        for (int i = 0; i < m.rowCount && i < kMaxChars; i++) {
            const Row& r = m.rows[i];
            const float hx = colX[i % 3], hy = rowY[i / 3];
            LayoutUse* u = AddUse(sc, USE_CHAR, i);
            if (u) {
                u->state = static_cast<uint8_t>((r.icon - ICON_MARIO) | (i == m.cursor ? kCharSelected : 0) |
                                                ((r.marks & MARK_DISABLED) ? kCharTaken : 0));
                u->x = hx;
                u->y = hy;
                u->w = scale;
                u->alpha = m.alpha;
                // MP3 heads: the 3D head keeps its full colour whoever has it and wears the owner's badge (the ghost alpha and float below
                // are for the 2D layout head, which is what shows where no 3D heads exist)
                u->alpha3d = m.alpha;
                u->y3d = hy;
                u->owner = (r.owner >= 0 && r.owner < 4) ? r.owner : static_cast<int8_t>(-1);
                u->mine = u->owner >= 0 && u->owner == m.localSlot;
                if (r.marks & MARK_DISABLED) {
                    // the ghost of the character: faint, breathing, floating a little more (its own phase per head)
                    const float ph = (bob + i * 0.23f) * 2.0f * kPi;
                    u->alpha = m.alpha * (kGhostAlpha + kGhostPulse * sinf(ph));
                    u->y = hy + kGhostFloat * sinf(ph * 0.5f);
                }
            }
            const float cx = layoutToSquareX(scr, hx);
            const bool taken = (r.marks & MARK_DISABLED) != 0;
            {   // vitabtn: the head and its name answer a touch (a column is a third of the picture wide)
                const float hw = W / 6.0f - 6.0f, hy0 = layoutToSquareY(scr, hy + 56.0f), hy1 = layoutToSquareY(scr, hy - 78.0f);
                if (plan->hitCount < kMaxCells) plan->hits[plan->hitCount++] = {cx - hw, hy0, cx + hw, hy1};
            }
            t.Text(r.label, cx, layoutToSquareY(scr, hy - 66.0f), 22.0f, taken ? Rgba{170, 175, 200, 255} : (i == m.cursor ? kYellow : kWhite), 1,
                   W / 3.0f - 24.0f);
            if (r.owner >= 0 && r.owner < 4) {
                // the player who has it: "2P Luigi64" on a plate in the slot colour (each player's cursor)
                char16_t plate[kValue + 4] = {static_cast<char16_t>(u'1' + r.owner), u'P', u' ', 0};
                copy(plate + 3, kValue, r.value);
                float w = t.Width(plate, Len(plate), 17.0f) + 26.0f;
                if (w > W / 3.0f - 20.0f) w = W / 3.0f - 20.0f;
                const float oy = layoutToSquareY(scr, hy - 90.0f);
                const Rgba c = charColor(r.icon - ICON_MARIO);   // (touches: the plate wears the character's colour)
                g.Fill(cx - w * 0.5f, oy - 11, cx + w * 0.5f, oy + 11, 11.0f, Rgba{c.r, c.g, c.b, 220},
                       Rgba{static_cast<uint8_t>(c.r * 0.7f), static_cast<uint8_t>(c.g * 0.7f), static_cast<uint8_t>(c.b * 0.7f), 220}, 4);
                g.Ring(cx - w * 0.5f, oy - 11, cx + w * 0.5f, oy + 11, 11.0f, -1.2f, Rgba{255, 255, 255, 200}, 0.0f, Rgba{255, 255, 255, 200}, 4);
                t.Text(plate, cx, oy, 17.0f, kWhite, 1, w - 12.0f);
            }
        }
        // the local selection: the Star Pointer's hand at the head's lower right, fingertip toward it (as the file select's hand rests
        // on a file), nudging as it moves; clear of the face and of the name below (heads3d)
        if (m.cursor >= 0 && m.cursor < m.rowCount) {
            const float cx = layoutToSquareX(scr, colX[m.cursor % 3]) + 50.0f + 4.0f * m.pulse, cy = layoutToSquareY(scr, rowY[m.cursor / 3]) - 2.0f + 4.0f * m.pulse;
            Pointer(sc, scr, cx, cy, 1.0f, 0.0f, m.alpha);
        }
        if (m.spinner >= 0) g.Star(midX, layoutToSquareY(scr, rowY[1] - 100.0f) , 12.0f, m.spinner, Rgba{255, 250, 200, 255}, kYellow);
        plan->x0 = 20.0f, plan->x1 = W - 20.0f, plan->y0 = layoutToSquareY(scr, top), plan->y1 = layoutToSquareY(scr, bottom);
        return;
    }

    if (m.kind == KIND_GRID) {
        // ---- chat: the Messages page. A message window (SysInfoWindowMini's frame) holding a grid of phrase tiles, light like the
        // tabs; the selected one in the list's selection yellow, the Star Pointer's hand at its left; the wide "Write a Message"
        // tile under them when typing is allowed.
        const int cols = m.cellCols > 0 ? m.cellCols : 3;
        int gridCells = 0, wide = -1;
        for (int i = 0; i < m.cellCount && i < kMaxCells; i++) {
            if (m.cells[i].wide) wide = i;
            else gridCells++;
        }
        const int rows = (gridCells + cols - 1) / cols;
        const float gap = game ? 6.0f : 8.0f, ch = game ? 42.0f : 40.0f;   // (the file select's look: no band, so the five rows of tiles stand a little taller)
        const float cw = ((W - 128.0f < 640.0f ? W - 128.0f : 640.0f) - 40.0f - (cols - 1) * gap) / cols;
        const float innerW = cols * cw + (cols - 1) * gap;
        const float innerH = rows * (ch + gap) - gap + (wide >= 0 ? ch + gap + 4.0f : 0.0f);
        const float ww = innerW + 40.0f, wh = innerH + 32.0f;
        const float cyL = midL;
        LayoutUse* win = AddUse(sc, USE_WINDOW, 0);
        if (win) {
            win->x = 0.0f;
            win->y = cyL + float2;
            win->w = ww;
            win->h = wh;
            win->alpha = m.alpha;
        }
        const float wy0 = layoutToSquareY(scr, cyL + float2 + wh * 0.5f);
        plan->x0 = midX - ww * 0.5f, plan->x1 = midX + ww * 0.5f, plan->y0 = wy0, plan->y1 = wy0 + wh;
        const float gx0 = midX - innerW * 0.5f, gy0 = wy0 + 16.0f;
        int gi = 0;
        for (int i = 0; i < m.cellCount && i < kMaxCells; i++) {
            const Cell& c = m.cells[i];
            float x0, y0, x1, y1;
            if (c.wide) {
                x0 = gx0 + innerW * 0.18f, x1 = gx0 + innerW * 0.82f;
                y0 = gy0 + rows * (ch + gap) + 4.0f, y1 = y0 + ch;
            } else {
                const int r = gi / cols, k = gi % cols;
                x0 = gx0 + k * (cw + gap), x1 = x0 + cw;
                y0 = gy0 + r * (ch + gap), y1 = y0 + ch;
                gi++;
            }
            const bool sel = i == m.cursor;
            const float grow = sel ? 1.5f * m.pulse + (game ? 3.0f : 0.0f) : 0.0f;   // (the file select's look: the chosen tile swells as a big button does)
            if (sel) {
                g.Ring(x0 - grow, y0 - grow, x1 + grow, y1 + grow, ch * 0.45f, 0.0f, Rgba{255, 210, 80, 110}, 4.0f, Rgba{255, 210, 80, 0}, 4);
                g.Fill(x0 - grow, y0 - grow, x1 + grow, y1 + grow, ch * 0.45f, kRowSelTop, kRowSelBottom, 4);
                g.Ring(x0 - grow, y0 - grow, x1 + grow, y1 + grow, ch * 0.45f, -1.6f, kRowSelRim, 0.0f, kRowSelRim, 4);
                if (!game) Pointer(sc, scr, x0 + 4.0f - 3.0f * m.pulse, (y0 + y1) * 0.5f + 2.0f, 0.75f, -90.0f, m.alpha);
            } else {
                g.Fill(x0, y0, x1, y1, ch * 0.45f, Rgba{255, 255, 255, 120}, Rgba{235, 242, 255, 80}, 4);
                g.Ring(x0, y0, x1, y1, ch * 0.45f, -1.0f, Rgba{150, 170, 230, 110}, 0.0f, Rgba{150, 170, 230, 110}, 4);
            }
            float tx = (x0 + x1) * 0.5f;
            if (c.wide) {
                // a little speech balloon (in the ink's blue) before the words
                const float tw = t.Width(c.label, Len(c.label), 22.0f), bx = tx - tw * 0.5f - 6.0f, by = (y0 + y1) * 0.5f, r = 10.0f;
                const Rgba ink = Rgba{100, 130, 210, 255};
                g.Fill(bx - r, by - r * 0.75f, bx + r, by + r * 0.45f, r * 0.45f, ink, ink, 3);
                g.Tri({bx - r * 0.45f, by + r * 0.3f, ink}, {bx - r * 0.6f, by + r * 0.95f, ink}, {bx - r * 0.05f, by + r * 0.3f, ink});
                g.Rect(bx - r * 0.55f, by - r * 0.38f, bx + r * 0.55f, by - r * 0.24f, Rgba{255, 255, 255, 230});
                g.Rect(bx - r * 0.55f, by - r * 0.06f, bx + r * 0.25f, by + r * 0.08f, Rgba{255, 255, 255, 230});
                tx += 10.0f;
            }
            t.Text(c.label, tx, (y0 + y1) * 0.5f, 22.0f, kInk, 1, x1 - x0 - (c.wide ? 48.0f : 20.0f), false);
            if (plan->hitCount < kMaxCells) plan->hits[plan->hitCount++] = {x0, y0, x1, y1};
        }
        return;
    }

    if (m.kind == KIND_BUTTONS) {
        // ---- big buttons (RetryButton's upper button, at y 48 of its layout), centred in the content's room. Up to three at the
        // pause menu's own size; four and five smaller, so every one keeps a gap and the column stays clear of both bands.
        const int n = m.rowCount < kMaxButtons ? m.rowCount : kMaxButtons;
        if (n == 0) {   // uxfix #4: a wait in the file select's look: the line and the guide only, nothing to press in the middle
            plan->x0 = plan->x1 = midX, plan->y0 = plan->y1 = layoutToSquareY(scr, midL);
            return;
        }
        // uinative4: the pause menu's own rhythm: a button is 64 high x 1.1 = 70 units, the next one 96 below (a gap of 26: 37 % of
        // the height). Never less than kButtonGap (25 %) of the height between neighbours at rest; the selected one grows 1.15 and
        // keeps clear of its neighbours and both bands. Buttons keep their size (1.0) as far as the room allows (three: always).
        const float room = top - bottom - 10.0f;
        float scale = room / (kButtonH * (1.15f + (1.0f + kButtonGap) * (n - 1)));
        if (scale > 1.0f) scale = 1.0f;
        float spacing = n > 1 ? (room - kButtonH * 1.15f * scale) / (n - 1) : 0.0f;
        if (spacing > 96.0f * scale) spacing = 96.0f * scale;
        for (int i = 0; i < n; i++) {
            LayoutUse* b = AddUse(sc, USE_BUTTON, i);
            if (!b) break;
            const Row& r = m.rows[i];
            b->text = static_cast<int8_t>(AddText(sc, r.label, u": ", r.value));
            // a label wider than the button's text box (304 units at the button's 33-unit font, scaled 1.1) goes on two lines,
            // as the pause menu's "Return to the / Observatory"
            if (b->text >= 0) {
                char16_t* lab = sc->texts[b->text];
                const int len = Len(lab);
                if (t.Width(lab, len, 36.3f) + 2.2f * (len - 1) > 296.0f) {   // (the text box's 2 units between characters, scaled 1.1)
                    int best = -1;
                    for (int k = 1; k < len - 1; k++)
                        if (lab[k] == u' ' && (best < 0 || (k > len / 2 ? k - len / 2 : len / 2 - k) < (best > len / 2 ? best - len / 2 : len / 2 - best))) best = k;
                    if (best > 0) lab[best] = u'\n';
                }
            }
            b->state = (r.marks & MARK_DISABLED) ? BUTTON_DISABLED : (i == m.cursor ? BUTTON_SELECTED : BUTTON_IDLE);
            const float by = midL + (n - 1) * spacing * 0.5f - i * spacing;   // the button's middle
            b->x = 0.0f;
            b->y = by - 48.0f * scale;
            b->w = scale;
            b->alpha = m.alpha;
            if (i == m.cursor) {
                // the Star Pointer's hand points at it from the left, as if the pointer rested on it
                const float bx0 = midX - 176.0f * scale;
                Pointer(sc, scr, bx0 - 4.0f - 5.0f * m.pulse, layoutToSquareY(scr, by) + 3.0f, 1.1f * (scale > 0.85f ? 1.0f : 0.9f), -90.0f, m.alpha);
            }
        }
        const float half = (n - 1) * spacing * 0.5f + kButtonH * 0.5f * scale;
        plan->x0 = midX - 176.0f * scale, plan->x1 = midX + 176.0f * scale;
        plan->y0 = layoutToSquareY(scr, midL + half);
        plan->y1 = layoutToSquareY(scr, midL - half);
        return;
    }

    // ---- a window (SysInfoWindowMini's frame), sized to what it holds
    const bool busy = m.kind == KIND_BUSY;
    const bool list = m.kind == KIND_LIST;
    float rowH = (list && m.rowCount <= kListRows) || overlay ? kListRowH : (list ? 44.0f : 40.0f);   // (uxfix #8: a row to touch is 67 units; a report of more rows than a page (the Connection Test) stays slim)   // (overlays: the results card, the waiting screen, the star card)
    int nChoice = 0;   // lobby: a card's live choices (the results card): kListRowH high whatever the standings above have to give
    for (int i = 0; i < m.rowCount && i < kMaxRows; i++) nChoice += (m.rows[i].marks & MARK_CHOICE) ? 1 : 0;
    const float choiceH = nChoice > 0 ? kListRowH : rowH;
    float inner = 0;
    if (overlay && m.title[0]) inner += nChoice > 0 ? 36.0f : 44.0f;         // the title inside (the head start line has none)
    if (m.tabCount > 0) inner += 38.0f;
    if (m.big[0]) inner += m.bigLabel[0] ? 70.0f : 54.0f;
    if (busy) inner += 104.0f;
    else inner += (m.rowCount > 0 ? (m.rowCount - nChoice) * rowH + nChoice * choiceH : 0.0f) + (m.status[0] ? 38.0f : 0.0f);
    const bool windowHint = !game && m.hint[0] && m.subtitle[0] && !busy && !overlay;   // (flow look: the line says it)
    if (windowHint) inner += 30.0f;
    if (overlay && m.buttonCount > 0) inner += 34.0f;
    const float maxInner = overlay ? (nChoice > 0 ? 404.0f : 380.0f) : (top - bottom) - 34.0f - (game ? 26.0f : 10.0f);   // (uinative4: a margin of 5 more each side; the look: 10 and the window's float)
    if (list && !game && m.rowCount < kListRows && m.status[0] == 0 && inner < 2 * rowH) inner = 2 * rowH;   // (a short list keeps some body; in the game's look the window is its rows: no empty body under one row)
    if (list && m.rowCount == 0 && inner < 3 * 44.0f + (windowHint ? 30.0f : 0.0f)) inner = 3 * 44.0f + (windowHint ? 30.0f : 0.0f);
    if (list && rowH > 44.0f && inner > maxInner - 24.0f) {   // uxfix #8: no room for 67-unit rows with the rest: the old slim rows (never squeezed into the margin)
        inner -= m.rowCount * (rowH - 44.0f);
        rowH = 44.0f;
    }
    if (inner > maxInner) {
        const float rowsH = (m.rowCount - nChoice) * rowH;
        const float k = rowsH > 0 ? (rowsH - (inner - maxInner)) / rowsH : 1.0f;
        const float floorK = nChoice > 0 ? 0.4f : 0.65f;   // (the standings give way, the choices do not)
        rowH *= k > floorK ? k : floorK;   // (uxfix #8: 67 units where there is room; down to 44, the old row, where a status line / hint take it)
        inner = maxInner;
    }
    const bool paged = list && m.pages > 1;
    const float wideW = W - (paged ? 236.0f : 128.0f);
    const float ww = busy ? (W - 128.0f < 470.0f ? W - 128.0f : 470.0f) : (overlay ? (W - 96.0f < (m.width > 0 ? m.width : 560.0f) ? W - 96.0f : (m.width > 0 ? m.width : 560.0f)) : (wideW < 600.0f ? wideW : 600.0f));
    const float wh = inner + 34.0f;
    const float cyL = overlay ? m.lift : midL;   // the window's middle (layout y)
    LayoutUse* win = AddUse(sc, USE_WINDOW, overlay ? 1 : 0);
    const float wx0 = midX - ww * 0.5f, wx1 = midX + ww * 0.5f;
    if (win) {
        win->x = SqToLayoutX(scr, midX);
        win->y = cyL + float2;
        win->w = ww;   // (the frame's units are square: it is squeezed 0.75 across on a 16:9 picture, as the picture is stretched)
        win->h = wh;
        win->alpha = m.alpha;
    }
    const float wy0 = layoutToSquareY(scr, cyL + float2 + wh * 0.5f);
    plan->x0 = wx0, plan->x1 = wx1, plan->y0 = wy0, plan->y1 = wy0 + wh;
    float y = wy0 + 17.0f;
    const float ix0 = wx0 + 20.0f, ix1 = wx1 - 20.0f;

    if (overlay && m.title[0]) {
        t.Text(m.title, midX, y + (nChoice > 0 ? 14.0f : 18.0f), 27.0f, kInk, 1, ww - 60.0f, false);
        y += nChoice > 0 ? 36.0f : 44.0f;
    }
    if (m.tabCount > 0) {
        const int n = m.tabCount > 3 ? 3 : m.tabCount;
        const float tw = (ix1 - ix0) / n;
        for (int i = 0; i < n; i++) {
            const float tx0 = ix0 + i * tw + 4.0f, tx1 = tx0 + tw - 8.0f;
            const bool on = i == m.tab;
            if (on) {
                g.Fill(tx0, y + 2, tx1, y + 32, 15.0f, kRowSelTop, kRowSelBottom, 4);
                g.Ring(tx0, y + 2, tx1, y + 32, 15.0f, -1.4f, kRowSelRim, 0.0f, kRowSelRim, 4);
            } else {
                g.Fill(tx0, y + 2, tx1, y + 32, 15.0f, Rgba{255, 255, 255, 80}, Rgba{255, 255, 255, 50}, 4);
            }
            t.Text(m.tabs[i], (tx0 + tx1) * 0.5f, y + 17.0f, 21.0f, on ? kInk : kInkDim, 1, tx1 - tx0 - 16.0f, false);
        }
        y += 38.0f;
    }
    if (m.big[0]) {
        if (m.bigLabel[0]) {
            t.Text(m.bigLabel, midX, y + 8.0f, 18.0f, kInkDim, 1, ww - 60.0f, false);
            y += 16.0f;
        }
        const int n = Len(m.big);
        // (netx: an address is longer than a room code: the characters close up so it stays inside the window)
        const float step = n * 30.0f + 26.0f <= ww - 60.0f ? 30.0f : (ww - 86.0f) / static_cast<float>(n), bw = n * step + 26.0f;
        g.Fill(midX - bw * 0.5f, y + 2, midX + bw * 0.5f, y + 50, 12.0f, Rgba{255, 255, 255, 150}, Rgba{235, 242, 255, 150}, 4);
        g.Ring(midX - bw * 0.5f, y + 2, midX + bw * 0.5f, y + 50, 12.0f, -1.2f, Rgba{150, 170, 230, 200}, 0.0f, Rgba{150, 170, 230, 200}, 4);
        for (int i = 0; i < n; i++) t.Text(&m.big[i], midX + (i - (n - 1) * 0.5f) * step, y + 27.0f, step < 30.0f ? 38.0f * step / 30.0f : 38.0f, kInkValue, 1, 0, false, 1);
        y += 54.0f;
    }
    if (busy) {
        if (m.spinner >= 0) g.Star(midX, y + 28.0f, 21.0f, m.spinner, Rgba{255, 250, 200, 255}, Rgba{255, 200, 20, 255});
        if (m.status[0]) t.Text(m.status, midX, y + 70.0f, 24.0f, kInk, 1, ww - 60.0f, false);
        if (m.progress >= 0) {
            const float p = m.progress > 1 ? 1 : m.progress;
            const float bx0 = midX - 150, bx1 = midX + 150, by = y + 94.0f;
            g.Fill(bx0, by - 5, bx1, by + 5, 5, Rgba{120, 140, 200, 120}, Rgba{120, 140, 200, 120}, 3);
            if (p > 0.02f) g.Fill(bx0 + 1, by - 4, bx0 + 1 + (bx1 - bx0 - 2) * p, by + 4, 4, Rgba{255, 214, 40, 255}, Rgba{236, 160, 20, 255}, 3);
        }
        return;
    }
    const float listTop = y;
    Rows(g, t, m, scr, ix0, y, ix1, rowH, choiceH, sc, !game && (list || (overlay && m.cursor >= 0)));   // (race: the results card's choices have the hand; in the file select's look the highlight is the cursor)
    y += (m.rowCount - nChoice) * rowH + nChoice * choiceH;
    if (paged) PageColumn(sc, scr, m, wx1 + 48.0f, wy0 + 4.0f, wy0 + wh - 4.0f, m.alpha);
    else {
        // (a list that only scrolls, never pages: the game's small arrows)
        if (m.moreAbove) g.ArrowV(midX, listTop - 6.0f, -1, 20.0f, 10.0f, Rgba{240, 150, 20, 255});
        if (m.moreBelow) g.ArrowV(midX, y + 5.0f, 1, 20.0f, 10.0f, Rgba{240, 150, 20, 255});
    }
    if (m.status[0]) {
        const float sy = m.rowCount == 0 ? listTop + (3 * 44.0f) * 0.5f : y + 19.0f;
        const float ss = list ? 24.0f : 21.0f;
        if (m.spinner >= 0) g.Star(midX - t.Width(m.status, Len(m.status), ss) * 0.5f - 22.0f, sy, 11.0f, m.spinner, Rgba{255, 250, 200, 255}, Rgba{255, 200, 20, 255});
        t.Text(m.status, midX, sy, ss, (overlay && m.lift != 0.0f) ? kInkValue : kInkDim, 1, ww - 80.0f, false);   // (race: the head start line reads in the values' ink)
    }
    // the hint and (overlays) the buttons at the bottom of the window
    float by = wy0 + wh - 21.0f;
    if (overlay && m.buttonCount > 0) {
        WindowHints(t, m, ix1, by, 19.0f);
        by -= 32.0f;
    }
    if (windowHint) t.Text(m.hint, midX, by, 20.0f, kInkDim, 1, ww - 48.0f, false);   // (else the band shows it)
}

void composeToasts(const Toast* toasts, int count, const Screen& scr, const FontMetrics fonts[FONT_COUNT], Scene* sc, PlanBase* plan) {
    PeakNote peakNote{plan, PLAN_TOAST};
    const float midX = scr.width * 0.5f;
    float y = sToastTop;
    sc->headCount = kToastHeadBase;
    for (int i = 0; i < count && i < kMaxToasts; i++) {
        const Toast& to = toasts[i];
        if (to.alpha <= 0.01f) continue;
        Builder g{plan, to.alpha};
        Writer t{plan, fonts, to.alpha};
        const float size = 21.0f;   // (uinative: the message window's words, a little smaller)
        const float tw = t.Width(to.text, Len(to.text), size);
        const float iconW = to.icon ? 32.0f : 0.0f;
        float w = tw + iconW + 60.0f;
        if (w < 260.0f) w = 260.0f;
        if (w > scr.width - 96.0f) w = scr.width - 96.0f;
        const float drop = (1.0f - to.alpha) * -10.0f;
        const float h = 46.0f, cy = y + drop + h * 0.5f;
        LayoutUse* u = AddUse(sc, USE_WINDOW, 2 + i);
        if (u) {
            u->x = 0.0f;
            u->y = scr.height * 0.5f - cy;
            u->w = w;
            u->h = h;
            u->alpha = to.alpha;
        }
        float tx = midX - (tw + iconW) * 0.5f;
        if (to.icon) {
            if (!IsHead(to.icon) || !Head(sc, scr, static_cast<uint8_t>(to.icon - ICON_MARIO), tx + 12.0f, cy, 36.0f, to.alpha, kMaxHeads))
                PlayerIcon(g, t, to.icon, tx + 11.0f, cy, 11.0f);
            tx += iconW;
        }
        t.Text(to.text, tx, cy, size, kInk, 0, midX + w * 0.5f - tx - 20.0f, false);
        y += 52.0f;
    }
}

void composeRing(const Ring& ring, const Screen& scr, const FontMetrics fonts[FONT_COUNT], PlanBase* plan) {
    PeakNote peakNote{plan, PLAN_TOAST};
    if (ring.alpha <= 0.01f) return;
    const float a = ring.alpha > 1.0f ? 1.0f : ring.alpha;
    Builder g{plan, a};
    Writer t{plan, fonts, a};
    const float cx = scr.width * 0.5f, cy = scr.height * 0.5f + 6.0f;
    const float grow = 0.85f + 0.15f * a;   // (opens from a little smaller)
    const float tw = 208.0f, th = 44.0f;   // (uinative: bigger tiles, the phrases' words 22 high)
    const float dx[4] = {0.0f, 176.0f, 0.0f, -176.0f}, dy[4] = {-94.0f, 0.0f, 94.0f, 0.0f};
    // the centre: the Select picture on a dark disc
    g.Disc(cx, cy, 21.0f, Rgba{20, 24, 70, 150}, 2.0f);
    t.Picture(23, cx, cy, 26.0f);   // (picture 23: Select / -)
    for (int i = 0; i < 4; i++) {
        const bool sel = i == ring.picked;
        const float bounce = sel ? 1.0f + 0.08f * ring.pop : 1.0f;
        const float x = cx + dx[i] * grow, y = cy + dy[i] * grow;
        const float hw = tw * 0.5f * bounce, hh = th * 0.5f * bounce;
        const float x0 = x - hw, x1 = x + hw, y0 = y - hh, y1 = y + hh;
        g.Ring(x0, y0 + 3.0f, x1, y1 + 3.0f, th * 0.45f, 0.0f, Rgba{6, 8, 40, 90}, 4.0f, Rgba{6, 8, 40, 0}, 4);   // soft shadow
        if (sel) {
            g.Ring(x0, y0, x1, y1, th * 0.45f, 0.0f, Rgba{255, 210, 80, 140}, 5.0f, Rgba{255, 210, 80, 0}, 4);
            g.Fill(x0, y0, x1, y1, th * 0.45f, kRowSelTop, kRowSelBottom, 4);
            g.Ring(x0, y0, x1, y1, th * 0.45f, -1.6f, kRowSelRim, 0.0f, kRowSelRim, 4);
        } else {
            g.Fill(x0, y0, x1, y1, th * 0.45f, Rgba{246, 249, 255, 235}, Rgba{208, 222, 252, 235}, 4);
            g.Ring(x0, y0, x1, y1, th * 0.45f, -1.4f, Rgba{255, 255, 255, 240}, 0.0f, Rgba{255, 255, 255, 240}, 4);
        }
        // a little arrow on the side away from the centre: the direction to press
        const Rgba ac = Rgba{240, 150, 20, 255};
        if (i == 0) g.ArrowV(x, y0 - 7.0f, -1, 13.0f, 7.0f, ac);
        else if (i == 2) g.ArrowV(x, y1 + 7.0f, 1, 13.0f, 7.0f, ac);
        else g.Arrow(i == 1 ? x1 + 8.0f : x0 - 8.0f, y, i == 1 ? 1.0f : -1.0f, 7.0f, 12.0f, ac);
        t.Text(ring.label[i], x, y, 22.0f * bounce, kInk, 1, tw - 22.0f, false);
    }
}

int hitGuide(const PlanBase& plan, float x, float y) {
    for (int i = 0; i < plan.guideHitCount && i < 4; i++) {
        const PlanBase::GuideHit& h = plan.guideHits[i];
        if (x >= h.x0 && x <= h.x1 && y >= h.y0 && y <= h.y1) return h.pic;
    }
    return -1;
}

int hitArrow(const PlanBase& plan, float x, float y) {
    for (int i = 0; i < plan.arrowHitCount && i < 2; i++) {
        const PlanBase::Hit& h = plan.arrowHits[i];
        if (x >= h.x0 && x <= h.x1 && y >= h.y0 && y <= h.y1) return i == 0 ? -1 : 1;
    }
    return 0;
}

int hitCell(const PlanBase& plan, float x, float y) {
    for (int i = 0; i < plan.hitCount && i < kMaxCells; i++) {
        const PlanBase::Hit& h = plan.hits[i];
        if (x >= h.x0 && x <= h.x1 && y >= h.y0 && y <= h.y1) return i;
    }
    return -1;
}

void composeStrip(const Strip& strip, const Screen& scr, const FontMetrics fonts[FONT_COUNT], float bob, Scene* sc, PlanBase* plan) {
    PeakNote peakNote{plan, PLAN_STRIP};
    sc->headCount = kStripHeadBase;
    if (strip.appear <= 0.01f) return;
    const float ap = strip.appear > 1.0f ? 1.0f : strip.appear;
    const float ease = 1.0f - (1.0f - ap) * (1.0f - ap);
    const float slide = -50.0f * (1.0f - ease);   // (PlayerLeft's own appear: from 50 units to the left)
    if (strip.localLook >= 0) {   // touches: the player's own head where the counter's picture would show Mario
        // ingame: where the game's picture pane is this frame (its appear slide and its count-up bounce included), else its rest place
        const bool pane = strip.counterX >= 0.0f;
        const float hx = pane ? strip.counterX : layoutToSquareX(scr, kCounterHeadX + slide);
        const float hy = pane ? strip.counterY : layoutToSquareY(scr, kCounterHeadY);
        const float hs = kCounterHeadScale * 100.0f * (pane ? strip.counterScale : 1.0f);
        Head(sc, scr, static_cast<uint8_t>(strip.localLook), hx, hy, hs, ap, kStripHeadBase + 1 + kMaxStrip);
    }
    for (int i = 0; i < strip.count && i < kMaxStrip; i++) {
        const StripMember& m = strip.m[i];
        float alpha = ap, dy = 0.0f;
        uint8_t flags = 0;
        switch (m.state) {
        case PARTY_QUIET: alpha *= kQuietAlpha; break;
        case PARTY_DOWN: alpha *= kDownAlpha; flags = kHeadDark; dy = -3.0f; break;
        case PARTY_BUBBLE: flags = kCharTaken; dy = 3.0f * sinf((bob * 2.0f + i * 0.31f) * 2.0f * kPi) + 2.0f; break;   // (floats, a little higher)
        default: break;
        }
        if (m.away && m.state != PARTY_QUIET) alpha *= kAwayAlpha;
        // ingame: a join / a leave: the head grows in from a little below (or shrinks away), its neighbours glide to make room
        const float e = m.enter < 0.0f ? 0.0f : (m.enter > 1.0f ? 1.0f : m.enter);
        if (e <= 0.01f) continue;
        const float ee = 1.0f - (1.0f - e) * (1.0f - e);
        alpha *= ee;
        dy -= 14.0f * (1.0f - ee);
        const float lx = kStripX + (m.pos >= 0.0f ? m.pos : static_cast<float>(i)) * kStripStep + slide;
        const float hx = layoutToSquareX(scr, lx), hy = layoutToSquareY(scr, kStripY + dy);
        if (!Head(sc, scr, m.look, hx, hy, kStripScale * 100.0f * (0.5f + 0.5f * ee), alpha, kStripHeadBase + 1 + kMaxStrip)) continue;
        sc->uses[sc->useCount - 1].state |= flags;
        {
            // ingame: no colour pill (the head is the player) and no "zzz" (the dimmed head says "away"); the connection's three bars
            // only when it is weak or lost, nothing while it is good
            if (m.link != LINK_GOOD) {
                Builder g{plan, ap * ee};
                const bool blink = m.link == LINK_LOST && fmodf(bob * 2.0f, 1.0f) > 0.5f;
                const int lit = m.link == LINK_WEAK ? 2 : 1;
                const Rgba on = m.link == LINK_WEAK ? Rgba{255, 205, 70, 255} : Rgba{255, 90, 80, 255};
                const float bx = hx + 4.0f, by = hy + 17.0f;   // bottom right of the head, like a signal badge
                for (int k = 0; k < 3; k++) {
                    const float h = 4.0f + 3.0f * k, x0 = bx + k * 5.0f;
                    g.Rect(x0 - 1.0f, by - h - 1.0f, x0 + 4.0f, by + 1.0f, Rgba{15, 20, 50, 190});
                    g.Rect(x0, by - h, x0 + 3.0f, by, k < lit && !blink ? on : Rgba{90, 100, 150, 200});
                }
            }
        }
        if (m.flash >= 0.0f && m.flash < 1.0f) {
            // the Power Star pops up over the head (a little bounce), holds, then fades
            const float f = m.flash;
            const float pop = f < 0.18f ? f / 0.18f : 1.0f;
            const float size = 22.0f * (pop < 1.0f ? 1.0f + 0.5f * sinf(pop * kPi) : 1.0f) * (0.4f + 0.6f * pop);
            const float fade = f > 0.7f ? (1.0f - f) / 0.3f : 1.0f;
            Writer t{plan, fonts, ap * fade};
            t.Picture(kPicPowerStar, hx + 10.0f, hy - 17.0f - 5.0f * pop, size);
        }
    }
}

// ingame: the edge heads of off-screen players (MpNameTags::edgeModel) and the watch badge (MpSpectate::badgeModel)
void composeEdges(const Edge& edge, const Screen& scr, Scene* sc, PlanBase* plan) {
    sc->headCount = kEdgeHeadBase;
    for (int i = 0; i < edge.count && i < kMaxEdges; i++) {
        const EdgeHead& h = edge.e[i];
        if (h.alpha <= 0.01f || h.look >= kCharacters) continue;
        Builder g{plan, h.alpha};
        const float r = kEdgeHeadSize * 0.5f + 3.0f;
        // the pointer: a small triangle on the ring, toward the player
        const float tx = h.x + h.ax * (r + 7.0f), ty = h.y + h.ay * (r + 7.0f);
        const float bx = h.x + h.ax * (r - 1.0f), by = h.y + h.ay * (r - 1.0f);
        const float nx = -h.ay * 7.0f, ny = h.ax * 7.0f;
        g.Tri({tx, ty, charEdge(h.look)}, {bx + nx * 1.25f, by + ny * 1.25f, charEdge(h.look)}, {bx - nx * 1.25f, by - ny * 1.25f, charEdge(h.look)});
        g.Tri({tx - h.ax * 2.0f, ty - h.ay * 2.0f, charColor(h.look)}, {bx + nx, by + ny, charColor(h.look)}, {bx - nx, by - ny, charColor(h.look)});
        // the ring: a soft dark disc behind the head, the colour's rim
        g.Disc(h.x, h.y, r, Rgba{12, 16, 52, 120}, 2.0f);
        g.Ring(h.x - r, h.y - r, h.x + r, h.y + r, r, -2.6f, charColor(h.look), 0.0f, charColor(h.look), 4);
        g.Ring(h.x - r, h.y - r, h.x + r, h.y + r, r, 0.0f, charEdge(h.look), 1.2f, charEdge(h.look, 0), 4);
        Head(sc, scr, h.look, h.x, h.y, kEdgeHeadSize, h.alpha, kBadgeHead);
    }
}

void composeBadge(const Badge& b, const Screen& scr, const FontMetrics fonts[FONT_COUNT], Scene* sc, PlanBase* plan) {
    if (b.alpha <= 0.01f || b.look >= kCharacters) return;
    sc->headCount = kBadgeHead;
    const float x = layoutToSquareX(scr, kBadgeX), y = layoutToSquareY(scr, kBadgeY);
    const float r = kBadgeSize * 0.5f + 4.0f;
    Builder g{plan, b.alpha};
    g.Disc(x, y, r, Rgba{12, 16, 52, 130}, 2.5f);
    g.Ring(x - r, y - r, x + r, y + r, r, -3.2f, charColor(b.look), 0.0f, charColor(b.look), 4);
    g.Ring(x - r, y - r, x + r, y + r, r, 0.0f, charEdge(b.look), 1.4f, charEdge(b.look, 0), 4);
    Head(sc, scr, b.look, x, y, kBadgeSize, b.alpha, kBadgeHead + 1);
    if (b.canSwitch) {   // L and R (the picture font's Z and "2": the handheld pictures show L and R), either side of the ring
        Writer t{plan, fonts, b.alpha};
        t.Picture(25, x - r - 15.0f, y + 2.0f, 26.0f);
        t.Picture(6, x + r + 15.0f, y + 2.0f, 26.0f);
    }
}

void composeStarRow(const StarRow& row, const Screen& scr, const FontMetrics fonts[FONT_COUNT], Scene* sc, PlanBase* plan) {
    if (row.alpha <= 0.01f || row.count <= 0) return;
    sc->headCount = kStarRowBase;
    const int n = row.count > kStarRowMax ? kStarRowMax : row.count;
    // the faces' size: 1.0 (head 40) unless SMG_MP_STARROW_SCALE says otherwise (a look-and-feel knob for the owner's choice; 0.5 .. 2.5)
    static float sScale = -1.0f;
    if (sScale < 0.0f) {
        const char* e = getenv("SMG_MP_STARROW_SCALE");
        const float v = e ? static_cast<float>(atof(e)) : 1.0f;
        sScale = v >= 0.5f && v <= 2.5f ? v : 1.0f;
    }
    const float head = kStarRowHead * sScale, step = kStarRowStep * sScale;
    const float y = scr.height * kStarRowY;
    const float x0 = scr.width * 0.5f - (n - 1) * step * 0.5f;
    for (int i = 0; i < n; i++) {
        const uint8_t look = row.look[i] < kCharacters ? row.look[i] : 0;
        const float x = x0 + i * step;
        const float pop = row.alpha < 1.0f ? 0.85f + 0.15f * row.alpha : 1.0f;
        Head(sc, scr, look, x, y, head * pop, row.alpha, kStarRowBase + kStarRowMax);
        if (i == row.by) {
            Writer t{plan, fonts, row.alpha};
            t.Picture(kPicPowerStar, x + 13.0f * sScale, y - 19.0f * sScale, 22.0f * sScale);
        }
    }
}

void composePrompt(const Prompt& pr, const Screen& scr, const FontMetrics fonts[FONT_COUNT], PlanBase* plan) {
    if (pr.alpha <= 0.01f) return;
    (void)scr;
    Builder g{plan, pr.alpha};
    const float r = 15.0f;
    g.Disc(pr.x, pr.y + 2.0f, r + 1.0f, Rgba{10, 14, 50, 90}, 2.0f);   // soft shadow
    g.Fill(pr.x - r, pr.y - r, pr.x + r, pr.y + r, r, Rgba{255, 255, 255, 245}, Rgba{214, 224, 248, 245}, 4);
    g.Ring(pr.x - r, pr.y - r, pr.x + r, pr.y + r, r, -1.6f, Rgba{150, 165, 210, 255}, 0.0f, Rgba{150, 165, 210, 255}, 4);
    Writer t{plan, fonts, pr.alpha};
    t.Picture(0, pr.x, pr.y, 24.0f);   // (picture 0: the press button; the handheld build draws Cross)
}

}  // namespace MpPanel
