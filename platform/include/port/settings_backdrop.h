#pragma once
// Optional Settings backdrop: the player's own title-screen capture (made by the game on a first run, see port/title_art.h), stored at half size.
#include <cstddef>
#include <cstdint>

namespace port::settings {
constexpr unsigned kBackdropSourceWidth = 960, kBackdropSourceHeight = 544;
constexpr unsigned kBackdropWidth = kBackdropSourceWidth / 2, kBackdropHeight = kBackdropSourceHeight / 2;
constexpr unsigned kBackdropChunkRows = 32;  // source rows read per file request
constexpr size_t kBackdropSourceBytes = size_t(kBackdropSourceWidth) * kBackdropSourceHeight * 4;
constexpr size_t kBackdropPixels = size_t(kBackdropWidth) * kBackdropHeight;
constexpr const char* kBackdropPath = "ux0:data/smg/art/title.rgba";

// Averages each 2x2 block of `rows` source rows (an even count starting at the even row `first`) of
// 960-wide RGBA8 into the 480x272 buffer `out`. The result is opaque ABGR (the menu's pixel order).
inline void DownsampleBackdropRows(const uint8_t* rgba, unsigned first, unsigned rows, uint32_t* out) {
    for (unsigned y = 0; y + 1 < rows; y += 2) {
        const uint8_t* top = rgba + size_t(y) * kBackdropSourceWidth * 4;
        const uint8_t* bottom = top + size_t(kBackdropSourceWidth) * 4;
        uint32_t* to = out + size_t((first + y) / 2) * kBackdropWidth;
        for (unsigned x = 0; x < kBackdropWidth; ++x) {
            const uint8_t* a = top + x * 8;
            const uint8_t* b = bottom + x * 8;
            uint32_t pixel = 0xff000000u;
            for (unsigned c = 0; c < 3; ++c)
                pixel |= uint32_t((a[c] + a[c + 4] + b[c] + b[c + 4] + 2) >> 2) << (8 * c);
            to[x] = pixel;
        }
    }
}
// The captured title has its "press start" prompt baked in. As the loading screen does, replace it with the
// clean starfield above it (the loading screen's 416..449 band, 224..735 columns, at half size).
inline void CleanBackdropPrompt(uint32_t* out) {
    for (unsigned y = 208; y < 225; ++y)
        for (unsigned x = 112; x < 368; ++x) out[y * kBackdropWidth + x] = out[(y - 29) * kBackdropWidth + x];
}
// Brightness (out of 256) the backdrop is drawn at on screen row `y`: dim everywhere so the light menu text
// reads clearly, darker again from the header down through the rows, easing back to the outer value at the bottom edge.
inline unsigned BackdropDim(unsigned y) {
    constexpr unsigned outer = 96, inner = 62;
    constexpr unsigned rampStart = 16, rampEnd = 48, tailStart = 490, tailEnd = 530;
    if (y <= rampStart) return outer;
    if (y < rampEnd) return outer - (outer - inner) * (y - rampStart) / (rampEnd - rampStart);
    if (y < tailStart) return inner;
    if (y < tailEnd) return inner + (outer - inner) * (y - tailStart) / (tailEnd - tailStart);
    return outer;
}
}
