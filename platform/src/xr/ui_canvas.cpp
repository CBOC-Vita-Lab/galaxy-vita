#include "ui_canvas.h"

#include <math.h>
#include <string.h>

#include <string>

#include "ui_font.h"

namespace ui {

namespace {

struct Font {
    const ui_font::Face* face = nullptr;
    std::vector<uint8_t> coverage;  // face->width x face->height
    const ui_font::Glyph* glyph(uint32_t code) const {
        if (code >= 32 && code < 127) {
            return &face->glyphs[code - 32];
        }
        for (int i = 95; i < face->count; i++) {
            if (face->glyphs[i].code == code) {
                return &face->glyphs[i];
            }
        }
        return &face->glyphs['?' - 32];
    }
};
Font sFonts[2];

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Next code point of UTF-8 text.
uint32_t nextCode(const char*& s) {
    uint8_t c = (uint8_t)*s++;
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    uint32_t code = c & (0x3F >> extra);
    for (int i = 0; i < extra && (*s & 0xC0) == 0x80; i++) {
        code = (code << 6) | ((uint8_t)*s++ & 0x3F);
    }
    return code;
}

// Signed distance shapes, anti-aliased over a pixel.
template <class Dist>
void fill(Canvas& cv, float x0, float y0, float x1, float y1, const Color& c, Dist dist) {
    int ix0 = (int)fmaxf(0.0f, floorf(x0 - 1.0f)), iy0 = (int)fmaxf(0.0f, floorf(y0 - 1.0f));
    int ix1 = (int)fminf((float)cv.w, ceilf(x1 + 1.0f)), iy1 = (int)fminf((float)cv.h, ceilf(y1 + 1.0f));
    for (int y = iy0; y < iy1; y++) {
        for (int x = ix0; x < ix1; x++) {
            float d = dist(x + 0.5f, y + 0.5f);
            float cov = fminf(1.0f, fmaxf(0.0f, 0.5f - d));
            if (cov > 0.0f) cv.blend(x, y, c, cov);
        }
    }
}

}  // namespace

void initFonts() {
    if (sFonts[0].face) {
        return;
    }
    for (int f = 0; f < 2; f++) {
        const ui_font::Face& face = ui_font::kFaces[f];
        std::vector<uint8_t> packed;
        uint32_t bits = 0;
        int nbits = 0;
        for (const char* p = face.atlas; *p; p++) {
            int v = base64Value(*p);
            if (v < 0) continue;
            bits = (bits << 6) | (uint32_t)v;
            nbits += 6;
            if (nbits >= 8) {
                nbits -= 8;
                packed.push_back((uint8_t)(bits >> nbits));
            }
        }
        Font& font = sFonts[f];
        font.face = &face;
        font.coverage.assign((size_t)face.width * face.height, 0);
        for (size_t i = 0; i < font.coverage.size(); i++) {
            uint8_t b = i / 2 < packed.size() ? packed[i / 2] : 0;
            font.coverage[i] = (uint8_t)(((i & 1) ? (b & 15) : (b >> 4)) * 17);
        }
    }
}

float textWidth(int font, const char* s) {
    float w = 0.0f;
    while (*s) {
        w += sFonts[font].glyph(nextCode(s))->advance * 0.25f;
    }
    return w;
}

void Canvas::init(int width, int height) {
    w = width;
    h = height;
    px.assign((size_t)w * h, 0);
}

void Canvas::blend(int x, int y, const Color& c, float coverage) {
    float a = c.a * coverage;
    if (a <= 0.0f) return;
    uint32_t& d = px[(size_t)y * w + x];
    float k = 1.0f - a;
    uint32_t r = (uint32_t)(c.r * a + (float)(d & 255) * k + 0.5f);
    uint32_t g = (uint32_t)(c.g * a + (float)((d >> 8) & 255) * k + 0.5f);
    uint32_t b = (uint32_t)(c.b * a + (float)((d >> 16) & 255) * k + 0.5f);
    uint32_t al = (uint32_t)(255.0f * a + (float)(d >> 24) * k + 0.5f);
    d = (r > 255 ? 255 : r) | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b) << 16 | (al > 255 ? 255 : al) << 24;
}

void Canvas::roundRect(float x0, float y0, float x1, float y1, float radius, const Color& c) {
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
    fill(*this, x0, y0, x1, y1, c, [=](float x, float y) {
        float qx = fabsf(x - cx) - hx + radius, qy = fabsf(y - cy) - hy + radius;
        float ox = fmaxf(qx, 0.0f), oy = fmaxf(qy, 0.0f);
        return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - radius;
    });
}

void Canvas::circle(float cx, float cy, float r, const Color& c) {
    fill(*this, cx - r, cy - r, cx + r, cy + r, c, [=](float x, float y) { return sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r; });
}

void Canvas::text(float x, float baseline, int font, const char* s, const Color& c, int align) {
    const Font& f = sFonts[font];
    float pen = x - (align == 1 ? textWidth(font, s) * 0.5f : align == 2 ? textWidth(font, s) : 0.0f);
    int by = (int)lroundf(baseline);
    while (*s) {
        const ui_font::Glyph* g = f.glyph(nextCode(s));
        int gx0 = (int)lroundf(pen) + g->left, gy0 = by - g->top;
        for (int gy = 0; gy < g->h; gy++) {
            int y = gy0 + gy;
            if (y < 0 || y >= h) continue;
            const uint8_t* row = &f.coverage[(size_t)(g->y + gy) * f.face->width + g->x];
            for (int gx = 0; gx < g->w; gx++) {
                int x = gx0 + gx;
                if (x >= 0 && x < w && row[gx]) blend(x, y, c, row[gx] / 255.0f);
            }
        }
        pen += g->advance * 0.25f;
    }
}

float Canvas::textWrapped(float x, float baseline, float width, float lineHeight, int font, const char* s, const Color& c) {
    std::string line;
    const char* p = s;
    while (*p) {
        // The next word, with the spaces before it.
        const char* start = p;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && *p != '\n') p++;
        std::string word(start, p);
        std::string trial = line + word;
        if (!line.empty() && textWidth(font, trial.c_str()) > width) {
            text(x, baseline, font, line.c_str(), c);
            baseline += lineHeight;
            size_t first = word.find_first_not_of(' ');
            line = first == std::string::npos ? std::string() : word.substr(first);
        } else {
            line = trial;
        }
        if (*p == '\n') {
            text(x, baseline, font, line.c_str(), c);
            baseline += lineHeight;
            line.clear();
            p++;
        }
    }
    if (!line.empty()) {
        text(x, baseline, font, line.c_str(), c);
        baseline += lineHeight;
    }
    return baseline;
}

}  // namespace ui
