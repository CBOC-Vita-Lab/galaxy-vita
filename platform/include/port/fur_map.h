// The fur density map (FurDrawer::createFurMap), written the way the console's big-endian CPU left it in memory.
//
// Fur (Queen Bee, the Honeybees, ...) is drawn as shells that sample a 32x32 IA8 texture the game fills at run time:
// every texel starts as the 16-bit value 255 and random texels get (thickness << 8) | (255 - length). The original code stores
// those values as `u16`. On the console that leaves the high byte first in memory, which is how GX reads IA8 (byte 0 alpha,
// byte 1 intensity): alpha = thickness, intensity = 255 - length, and a texel without a strand has alpha 0, intensity 255.
// On a little-endian CPU the same `u16` store puts the low byte first, so the texture the renderer decodes had alpha and
// intensity swapped: no strand became alpha 255 / intensity 0 (every shell opaque and dark, which then passes the alpha test)
// and strands got their thickness as intensity. StoreU16 writes the two bytes in console order on any host.
//
// GXV_FIX_FURMAP (default 1, read once): 0 keeps the host-order `u16` store of the first ports.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "hot_knob.h"

namespace port {
namespace fur {

inline bool Enabled() {
    return PORT_HOT_KNOB(bool, [] {  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
        const char* v = std::getenv("GXV_FIX_FURMAP");  // unset, empty or not a number: stays on
        if (!v || !*v) return true;
        char* end = nullptr;
        const long parsed = std::strtol(v, &end, 0);
        return end == v || *end ? true : parsed != 0;
    }());
}

// Texel `index` of a 16-bit-per-texel image gets v, in console byte order (`bigEndian`) or in the host's.
inline void StoreU16(void* image, uint32_t index, uint16_t v, bool bigEndian) {
    uint8_t* p = static_cast<uint8_t*>(image) + static_cast<size_t>(index) * 2;
    if (bigEndian) {
        p[0] = static_cast<uint8_t>(v >> 8);
        p[1] = static_cast<uint8_t>(v & 0xFF);
    } else {
        std::memcpy(p, &v, 2);
    }
}

// createFurMap's loops, with the random number source and the byte order passed in. `rnd(0, 1)` is MR::getRandom(0.0f, 1.0f); the
// calls are made in the original order (y first, then x), so the same seed gives the same texels.
template <class Rand>
inline void BuildMap(void* image, uint32_t width, uint32_t height, const float density[4], const float thickness[4], const uint8_t lengthMap[4],
                     Rand rnd, bool bigEndian) {
    const uint32_t count = height * width;
    for (uint32_t i = 0; i < count; i++) StoreU16(image, i, 255, bigEndian);
    for (uint32_t type = 0; type < 4; type++) {
        for (uint32_t i = 0; i < static_cast<float>(height) * static_cast<float>(width) * density[type]; i++) {
            const float randY = rnd(0.0f, 1.0f);
            const float randX = rnd(0.0f, 1.0f);
            int32_t y = static_cast<int32_t>(static_cast<float>(height) * randY);
            int32_t x = static_cast<int32_t>(static_cast<float>(width) * randX);
            if (y == static_cast<int32_t>(height)) y = static_cast<int32_t>(height) - 1;
            if (x == static_cast<int32_t>(width)) x = static_cast<int32_t>(width) - 1;
            const uint16_t v = static_cast<uint16_t>((static_cast<uint16_t>(255.0f * thickness[type]) << 8) | static_cast<uint16_t>(255 - lengthMap[type]));
            StoreU16(image, static_cast<uint32_t>(x) + width * static_cast<uint32_t>(y), v, bigEndian);
        }
    }
}

}  // namespace fur
}  // namespace port
