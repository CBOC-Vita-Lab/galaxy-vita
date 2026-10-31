#pragma once
// A made-up title screen for the host tests (no game picture): blue sky with stars, a colourful rectangular "logo" and two white
// text lines with dark outlines at the places the real title has them.
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
}  // namespace fixture
