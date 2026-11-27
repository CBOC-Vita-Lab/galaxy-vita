#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace port { namespace titlecloud {

// Both the Vita and its cooked BDL fields are little endian; read unaligned
// offsets without aliasing. No data from the game is embedded here.
inline uint16_t U16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
inline uint32_t U32(const uint8_t* p) {
    return uint32_t(U16(p)) | (uint32_t(U16(p + 2)) << 16);
}

// Called only for CometNearOrbitSky.bdl, before the J3D loader bakes sampler
// registers into its material lists. Change the resident sampler, not the texels.
inline bool BaseCloud(void* model, size_t size) {
    auto* p = static_cast<uint8_t*>(model);
    if (!p || size < 32 || std::memcmp(p, "2D3J4ldb", 8) || U32(p + 8) != size) return false;
    size_t block = 32;
    for (uint32_t i = 0; i < U32(p + 12) && size - block >= 8; ++i) {
        uint8_t* b = p + block;
        const size_t n = U32(b + 4);
        if (n < 8 || n > size - block) return false;
        if (!std::memcmp(b, "1XET", 4)) {
            if (n < 20) return false;
            const size_t count = U16(b + 8), tex = U32(b + 12), names = U32(b + 16);
            if (tex < 20 || tex > n || count > (n - tex)/32 ||
                names < 20 || names > n - 4 || U16(b + names) != count || count > (n - names - 4)/4) return false;
            for (size_t j = 0; j < count; ++j) {
                const size_t s = U16(b + names + 6 + 4*j);
                if (s > n - names || sizeof("Cloud01k") > n - names - s || std::memcmp(b + names + s, "Cloud01k", sizeof("Cloud01k"))) continue;
                uint8_t* h = b + tex + 32*j;
                if (h[0] != 14 || U16(h+2) != 256 || U16(h+4) != 256 || h[16] != 1 ||
                    h[20] != 5 || h[21] != 1 || h[22] != 0 || h[23] != 24 || h[24] != 4 || U16(h+26) != 200) return false;
                h[16] = 0; h[20] = 1; h[23] = 0; h[24] = 1; h[26] = h[27] = 0;
                return true;
            }
            return false;
        }
        block += n;
    }
    return false;
}

}}  // namespace port::titlecloud
