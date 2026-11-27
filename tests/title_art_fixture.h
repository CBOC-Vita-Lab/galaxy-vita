#pragma once
// A made-up title screen for the host tests (no game picture): blue sky with stars, a colourful rectangular "logo" and two white
// text lines with dark outlines at the places the real title has them.
#include <algorithm>
#include <cstdint>
#include <vector>
#include "port/title_art.h"

namespace fixture {
constexpr int W = 960, H = 544;
struct Logo { int left = 230, right = 720, top = 50, bottom = 340; };
inline uint32_t Hash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
inline std::vector<uint8_t> MakeTitle(bool logo = true, bool text = true, Logo box = Logo(), uint32_t seed = 7) {
    std::vector<uint8_t> px(size_t(W) * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t* p = &px[(size_t(y) * W + x) * 4];
            int r = 12 + y / 30, g = 70 + y / 20, b = 130 + y / 10;
            const uint32_t h = Hash(uint32_t(y * W + x) ^ seed);
            if ((h & 255) < 3) { r = 200; g = 220; b = 255; }  // stars (about 1 pixel in 85)
            else r += int(h >> 24) % 5, g += int(h >> 20) % 5, b += int(h >> 16) % 5;
            p[0] = uint8_t(r); p[1] = uint8_t(g); p[2] = uint8_t(b); p[3] = 255;
        }
    if (logo)
        for (int y = box.top; y <= box.bottom; ++y)
            for (int x = box.left; x <= box.right; ++x) {
                uint8_t* p = &px[(size_t(y) * W + x) * 4];
                const int band = ((x - box.left) / 40) % 4;
                static const uint8_t colours[4][3] = {{235, 235, 225}, {40, 90, 220}, {240, 200, 40}, {60, 190, 70}};
                p[0] = colours[band][0]; p[1] = colours[band][1]; p[2] = colours[band][2];
            }
    if (text) {
        auto bar = [&](int x0, int x1, int y0, int y1) {
            for (int y = y0 - 2; y < y1 + 2; ++y)
                for (int x = x0 - 2; x < x1 + 2; ++x) {
                    uint8_t* p = &px[(size_t(y) * W + x) * 4];
                    const bool inside = x >= x0 && x < x1 && y >= y0 && y < y1;
                    p[0] = p[1] = p[2] = inside ? 250 : 25;
                }
        };
        for (int i = 0; i < 24; ++i) bar(245 + i * 20, 255 + i * 20, 422, 446);  // "Press START or touch the screen."
        for (int i = 0; i < 12; ++i) bar(385 + i * 15, 395 + i * 15, 486, 502);  // the copyright line
    }
    return px;
}

// A second made-up title, closer in shape to a real one (still no game picture): a vertical blue gradient sky with a cyan glow
// along the bottom, and a "wordmark" of rounded, outlined blocks of different heights and colours on two rows, the upper row
// arched, inside the same box as the plain logo; and the two text lines.
inline std::vector<uint8_t> MakeWordmarkTitle(uint32_t seed = 11) {
    auto px = MakeTitle(false, true, Logo(), seed);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t* p = &px[(size_t(y) * W + x) * 4];
            const bool text = y >= 418 && y < 506 && x >= 240 && x < 740 && (p[0] == 250 || p[0] == 25);
            if (text) continue;
            const int glow = y > 470 ? (y - 470) * 2 : 0;
            if (p[0] == 200 && p[2] == 255) continue;  // keep the stars
            p[0] = uint8_t(8 + y / 40 + glow / 6);
            p[1] = uint8_t(std::min(255, 40 + y / 6 + glow));
            p[2] = uint8_t(std::min(255, 110 + y / 4 + glow));
        }
    static const uint8_t colours[5][3] = {{250, 210, 40}, {230, 60, 50}, {60, 200, 80}, {245, 245, 235}, {250, 140, 30}};
    auto block = [&](int x0, int y0, int w, int h, int colour) {
        const int r = std::min(w, h) / 4;
        for (int y = y0 - 4; y < y0 + h + 4; ++y)
            for (int x = x0 - 4; x < x0 + w + 4; ++x) {
                const int cx = std::min(std::max(x, x0 + r), x0 + w - 1 - r), cy = std::min(std::max(y, y0 + r), y0 + h - 1 - r);
                const int d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                uint8_t* p = &px[(size_t(y) * W + x) * 4];
                if (d2 <= r * r) { p[0] = colours[colour][0]; p[1] = colours[colour][1]; p[2] = colours[colour][2]; }
                else if (d2 <= (r + 4) * (r + 4)) { p[0] = 70; p[1] = 30; p[2] = 20; }  // a dark outline
            }
    };
    for (int i = 0; i < 9; ++i) {  // upper row: arched
        const int x = 250 + i * 52, lift = (4 - i) * (4 - i) * 3;
        block(x, 60 + lift, 44, 110 - lift / 2, i % 5);
    }
    for (int i = 0; i < 6; ++i) block(300 + i * 62, 200, 54, 130, (i + 2) % 5);  // lower row: larger
    return px;
}
}  // namespace fixture
