// The loading screen: the player's own title-screen capture (ux0:data/smg/art/title.rgba, made by the game from its own running
// copy, see port/title_art.h) with the progress display where its start prompt was; until that capture exists (the first start,
// or a package that carries no game artwork) a dark starfield of our own drawing. Pixels are R, G, B, A bytes for both the direct
// display and GL upload.
#pragma once

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vgx_boot_guard.h"

#if defined(__vita__)
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#endif

namespace loading {

constexpr int W = 960, H = 544;

struct LoadingGlyph {
    int w, h, xo, yo, adv;
    unsigned off;
};
#include "loading_font.inc"

struct State {
    float progress = 0;
    const char* caption = "";
    int tip = 0;
    const char* footer = nullptr;
    const char* footer2 = nullptr;  // an optional second line under the footer (error texts)
    bool alert = false;
    float fade = 0;
    bool titleReady = false;
};

namespace detail {

inline uint32_t Pack(int r, int g, int b) {
    return 0xFF000000u | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
}

inline void Blend(uint32_t* pixel, int r, int g, int b, int alpha) {
    if (alpha <= 0) return;
    const uint32_t d = *pixel;
    const int dr = d & 255, dg = (d >> 8) & 255, db = (d >> 16) & 255;
    *pixel = Pack(dr + (((r - dr) * alpha) >> 8), dg + (((g - dg) * alpha) >> 8), db + (((b - db) * alpha) >> 8));
}

inline uint32_t* g_bg = nullptr;
constexpr size_t kBackgroundBytes = static_cast<size_t>(W) * H * 4;
constexpr const char* kBackgroundPath = "ux0:data/smg/art/title.rgba";

inline bool ReadBackground() {
    FILE* file = fopen(kBackgroundPath, "rb");
    if (!file) return false;
    bool valid = fseek(file, 0, SEEK_END) == 0 && ftell(file) == static_cast<long>(kBackgroundBytes) &&
                 fseek(file, 0, SEEK_SET) == 0;
    if (valid) valid = fread(g_bg, 1, kBackgroundBytes, file) == kBackgroundBytes && fgetc(file) == EOF && !ferror(file);
    if (fclose(file) != 0) valid = false;
    return valid;
}

// The picture used when there is no capture: a deep-blue gradient with a soft glow and a deterministic scatter of stars. No star
// falls in the band the progress bar, its caption and its footer use (x 176..783, y 384..483), so the text reads on a plain ground.
inline void FillNeutralBackground(uint32_t* bg) {
    for (int y = 0; y < H; ++y) {
        const int base_r = 3 + 6 * y / H, base_g = 8 + 15 * y / H, base_b = 24 + 30 * y / H;
        for (int x = 0; x < W; ++x) {
            const float dx = (x - 480) / 440.0f, dy = (y - 210) / 300.0f;
            const float d = dx * dx + dy * dy;
            const int glow = d < 1.0f ? static_cast<int>(26.0f * (1.0f - d) * (1.0f - d)) : 0;
            bg[y * W + x] = Pack(base_r + glow / 3, base_g + glow / 2, base_b + glow);
        }
    }
    uint32_t s = 0x2545F491u;
    auto next = [&s]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
    for (int i = 0; i < 520; ++i) {
        const int x = static_cast<int>(next() % W), y = static_cast<int>(next() % H);
        const int v = 110 + static_cast<int>(next() % 146);
        const unsigned kind = next() % 100;
        const int size = kind < 8 ? 2 : 1;
        if (x >= 176 - 4 && x < 784 + 4 && y >= 384 - 4 && y < 484 + 4) continue;
        if (x + size > W - 4 || y + size > H - 4 || x < 4 || y < 4) continue;
        for (int j = 0; j < size; ++j)
            for (int k = 0; k < size; ++k) bg[(y + j) * W + x + k] = Pack(v, v, v > 235 ? 255 : v + 20);
        if (kind >= 97) {  // a few brighter ones get a small cross
            for (int a = 1; a <= 3; ++a) {
                const int fade = v * (4 - a) / 5;
                bg[y * W + x - a] = Pack(fade, fade, fade + 15 > 255 ? 255 : fade + 15);
                bg[y * W + x + a] = Pack(fade, fade, fade + 15 > 255 ? 255 : fade + 15);
                bg[(y - a) * W + x] = Pack(fade, fade, fade + 15 > 255 ? 255 : fade + 15);
                bg[(y + a) * W + x] = Pack(fade, fade, fade + 15 > 255 ? 255 : fade + 15);
            }
        }
    }
}

inline void BuildBackground() {
    g_bg = static_cast<uint32_t*>(malloc(kBackgroundBytes));
    if (!g_bg) return;
    if (!ReadBackground()) {
        FillNeutralBackground(g_bg);
        return;
    }
    // Replace only the captured start prompt with nearby clean starfield, leaving the logo and
    // copyright pixel-identical. The live title restores its own prompt after the existing fade.
    for (int y = 416; y < 450; ++y)
        memcpy(g_bg + y * W + 224, g_bg + (y - 58) * W + 224, 512 * sizeof(uint32_t));
}

inline void DrawText(uint32_t* frame, const char* source, int left, int right, int y, bool centered,
                     int r, int g, int b) {
    if (!source || right <= left) return;
    char text[96];
    int count = 0, width = 0;
    while (*source && count < static_cast<int>(sizeof(text)) - 1) {
        const unsigned char raw = static_cast<unsigned char>(*source);
        const char c = raw >= 32 && raw < 127 ? static_cast<char>(raw) : '?';
        const int advance = kSmallGlyphs[c - 32].adv;
        if (width + advance > right - left) break;
        text[count++] = c;
        width += advance;
        ++source;
    }
    if (*source) {
        const int dots = 3 * kSmallGlyphs['.' - 32].adv;
        while (count && (width + dots > right - left || count > static_cast<int>(sizeof(text)) - 4))
            width -= kSmallGlyphs[text[--count] - 32].adv;
        if (width + dots <= right - left) {
            for (int i = 0; i < 3; ++i) text[count++] = '.';
            width += dots;
        }
    }
    text[count] = 0;
    int x = centered ? left + (right - left - width) / 2 : left;
    for (int k = 0; k < count; ++k) {
        const LoadingGlyph& glyph = kSmallGlyphs[text[k] - 32];
        for (int j = 0; j < glyph.h; ++j) {
            const int fy = y + glyph.yo + j;
            if (fy < 0 || fy >= H) continue;
            for (int i = 0; i < glyph.w; ++i) {
                const int fx = x + glyph.xo + i;
                if (fx < left || fx >= right || fx < 0 || fx >= W) continue;
                Blend(&frame[fy * W + fx], r, g, b, kSmallData[glyph.off + j * glyph.w + i]);
            }
        }
        x += glyph.adv;
    }
}

}  // namespace detail

inline bool Init() {
    if (!detail::g_bg) detail::BuildBackground();
    return detail::g_bg != nullptr;
}

inline void Release() {
    free(detail::g_bg);
    detail::g_bg = nullptr;
}

inline void Render(uint32_t* frame, double, const State& st) {
    using namespace detail;
    if (!g_bg || !frame) return;
    memcpy(frame, g_bg, kBackgroundBytes);
    float progress = !(st.progress >= 0.0f) ? 0.0f : (st.progress > 1.0f ? 1.0f : st.progress);
    if (vgx::bootguard::Enabled() && !st.titleReady && progress > 0.99f) progress = 0.99f;
    const int bx = 220, by = 426, bw = 520, bh = 14;
    const int fill = static_cast<int>((bw - 4) * progress + 0.5f);
    for (int y = by; y < by + bh; ++y) {
        for (int x = bx; x < bx + bw; ++x) {
            const bool border = x == bx || x == bx + bw - 1 || y == by || y == by + bh - 1;
            uint32_t colour = border ? Pack(96, 191, 220) : Pack(10, 48, 65);
            if (x >= bx + 2 && x < bx + 2 + fill && y > by + 1 && y < by + bh - 2)
                colour = st.alert ? Pack(230, 178, 130) : Pack(130, 228, 250);
            frame[y * W + x] = colour;
        }
    }
    char percent[5];
    const int p = static_cast<int>(progress * 100.0f + 0.5f);
    int n = 0;
    if (p >= 100) percent[n++] = '1';
    if (p >= 10) percent[n++] = static_cast<char>('0' + (p / 10) % 10);
    percent[n++] = static_cast<char>('0' + p % 10);
    percent[n++] = '%';
    percent[n] = 0;
    DrawText(frame, st.caption, bx, bx + bw - 64, 395, false, 235, 247, 255);
    DrawText(frame, percent, bx + bw - 52, bx + bw, 395, false, 174, 233, 250);
    DrawText(frame, st.footer, 192, 768, 448, true, st.alert ? 255 : 186, st.alert ? 200 : 219, st.alert ? 170 : 234);
    if (st.footer2) DrawText(frame, st.footer2, 192, 768, 466, true, st.alert ? 255 : 186, st.alert ? 200 : 219, st.alert ? 170 : 234);

    if (st.fade > 0.0f) {
        const float fade = st.fade > 1.0f ? 1.0f : st.fade;
        const uint32_t k = static_cast<uint32_t>((1.0f - fade) * 256.0f);
        for (int i = 0; i < W * H; ++i) {
            const uint32_t d = frame[i];
            frame[i] = 0xFF000000u | (((d & 0x00FF00FFu) * k >> 8) & 0x00FF00FFu) | (((d & 0x0000FF00u) * k >> 8) & 0x0000FF00u);
        }
    }
}

#if defined(__vita__)
// ---------------------------------------------------------------------------------------------
// Straight onto the display, with no graphics library: a block of video memory that
// sceDisplaySetFrameBuf scans. Used until vitaGL has started (it then takes the display over; the
// block is released once GL has put the same picture up).
namespace direct {

inline SceUID g_block = -1;
inline void* g_base = nullptr;

inline bool Open() {
    if (g_base) return true;
    // 960x544x4 = 2,088,960 bytes: video memory is allocated in 256 KiB units.
    g_block = sceKernelAllocMemBlock("smg_loading", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 8 * 256 * 1024, nullptr);
    if (g_block < 0) return false;
    if (sceKernelGetMemBlockBase(g_block, &g_base) < 0 || !g_base) {
        sceKernelFreeMemBlock(g_block);
        g_block = -1;
        g_base = nullptr;
        return false;
    }
    return true;
}

// Copies the frame to the block (starting just after a vertical blank, so the copy stays ahead of the
// scan-out) and tells the display to scan it.
inline void Show(const uint32_t* frame) {
    if (!g_base) return;
    sceDisplayWaitVblankStart();
    memcpy(g_base, frame, static_cast<size_t>(W) * H * 4);
    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    fb.base = g_base;
    fb.pitch = W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = W;
    fb.height = H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

inline void Close() {
    if (g_block >= 0) sceKernelFreeMemBlock(g_block);
    g_block = -1;
    g_base = nullptr;
}

}  // namespace direct
#endif

}  // namespace loading
