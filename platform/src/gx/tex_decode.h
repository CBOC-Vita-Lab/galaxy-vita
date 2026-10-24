// GX texture formats -> RGBA8.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace gpu {

enum TexFormat : uint32_t {
    TF_I4 = 0x0,
    TF_I8 = 0x1,
    TF_IA4 = 0x2,
    TF_IA8 = 0x3,
    TF_RGB565 = 0x4,
    TF_RGB5A3 = 0x5,
    TF_RGBA8 = 0x6,
    TF_CI4 = 0x8,
    TF_CI8 = 0x9,
    TF_CI14X2 = 0xA,
    TF_CMPR = 0xE,
};

enum TlutFormat : uint32_t { TLUT_IA8 = 0, TLUT_RGB565 = 1, TLUT_RGB5A3 = 2 };

// Bytes used by a (width x height) level in format `fmt`, including tile padding.
size_t texLevelSize(uint32_t fmt, uint32_t width, uint32_t height);

// Decodes one level.  `src` is big-endian texture memory; `dst` receives
// width*height RGBA8 texels (row-major, top row first).  `tlut` points at
// the palette (big-endian u16 entries) for CI formats.
void decodeTexture(uint8_t* dst, const uint8_t* src, uint32_t fmt, uint32_t width, uint32_t height, const uint16_t* tlut, uint32_t tlutFmt);

}  // namespace gpu
