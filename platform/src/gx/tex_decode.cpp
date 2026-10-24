// GX texture decoders.  Textures are stored in tiles (blocks) whose size
// depends on the texel size; all multi-byte values are big-endian.
#include "tex_decode.h"

#include <string.h>

namespace gpu {

namespace {

struct Block {
    uint32_t w, h, bytes;
};

Block blockOf(uint32_t fmt) {
    switch (fmt) {
    case TF_I4:
    case TF_CI4:
    case TF_CMPR:
        return {8, 8, 32};
    case TF_I8:
    case TF_IA4:
    case TF_CI8:
        return {8, 4, 32};
    case TF_IA8:
    case TF_RGB565:
    case TF_RGB5A3:
    case TF_CI14X2:
        return {4, 4, 32};
    case TF_RGBA8:
        return {4, 4, 64};
    default:
        return {4, 4, 32};
    }
}

inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

inline uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | (g << 8) | (b << 16) | (a << 24); }

inline uint32_t ext5(uint32_t v) { return (v << 3) | (v >> 2); }
inline uint32_t ext6(uint32_t v) { return (v << 2) | (v >> 4); }
inline uint32_t ext4(uint32_t v) { return (v << 4) | v; }
inline uint32_t ext3(uint32_t v) { return (v << 5) | (v << 2) | (v >> 1); }

inline uint32_t fromRGB565(uint16_t c) { return rgba(ext5(c >> 11), ext6((c >> 5) & 63), ext5(c & 31), 255); }

inline uint32_t fromRGB5A3(uint16_t c) {
    if (c & 0x8000) {
        return rgba(ext5((c >> 10) & 31), ext5((c >> 5) & 31), ext5(c & 31), 255);
    }
    return rgba(ext4((c >> 8) & 15), ext4((c >> 4) & 15), ext4(c & 15), ext3((c >> 12) & 7));
}

inline uint32_t fromIA8(uint16_t c) {
    uint32_t i = c & 0xFF, a = c >> 8;
    return rgba(i, i, i, a);
}

inline uint32_t fromTlut(uint16_t c, uint32_t tlutFmt) {
    switch (tlutFmt) {
    case TLUT_IA8:
        return fromIA8(c);
    case TLUT_RGB565:
        return fromRGB565(c);
    default:
        return fromRGB5A3(c);
    }
}

// Decodes one 4x4 CMPR (S3TC/DXT1-style) sub-block.
void decodeCmprSub(uint32_t* out, uint32_t stride, const uint8_t* src, uint32_t maxW, uint32_t maxH) {
    uint16_t c0 = be16(src), c1 = be16(src + 2);
    uint32_t pal[4];
    uint32_t r0 = ext5(c0 >> 11), g0 = ext6((c0 >> 5) & 63), b0 = ext5(c0 & 31);
    uint32_t r1 = ext5(c1 >> 11), g1 = ext6((c1 >> 5) & 63), b1 = ext5(c1 & 31);
    pal[0] = rgba(r0, g0, b0, 255);
    pal[1] = rgba(r1, g1, b1, 255);
    if (c0 > c1) {
        pal[2] = rgba((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255);
        pal[3] = rgba((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255);
    } else {
        pal[2] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
        pal[3] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 0);
    }
    for (uint32_t y = 0; y < 4 && y < maxH; y++) {
        uint8_t bits = src[4 + y];
        for (uint32_t x = 0; x < 4 && x < maxW; x++) {
            out[y * stride + x] = pal[(bits >> (6 - 2 * x)) & 3];
        }
    }
}

}  // namespace

size_t texLevelSize(uint32_t fmt, uint32_t width, uint32_t height) {
    Block b = blockOf(fmt);
    uint32_t bw = (width + b.w - 1) / b.w, bh = (height + b.h - 1) / b.h;
    return (size_t)bw * bh * b.bytes;
}

void decodeTexture(uint8_t* dstBytes, const uint8_t* src, uint32_t fmt, uint32_t width, uint32_t height, const uint16_t* tlut, uint32_t tlutFmt) {
    uint32_t* dst = (uint32_t*)dstBytes;
    Block b = blockOf(fmt);
    uint32_t bw = (width + b.w - 1) / b.w, bh = (height + b.h - 1) / b.h;
    const uint8_t* p = src;

    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++, p += b.bytes) {
            uint32_t x0 = bx * b.w, y0 = by * b.h;
            if (fmt == TF_CMPR) {
                for (uint32_t s = 0; s < 4; s++) {
                    uint32_t sx = x0 + (s & 1) * 4, sy = y0 + (s >> 1) * 4;
                    if (sx < width && sy < height) {
                        decodeCmprSub(dst + sy * width + sx, width, p + s * 8, width - sx, height - sy);
                    }
                }
                continue;
            }
            for (uint32_t ty = 0; ty < b.h; ty++) {
                uint32_t y = y0 + ty;
                for (uint32_t tx = 0; tx < b.w; tx++) {
                    uint32_t x = x0 + tx;
                    uint32_t i = ty * b.w + tx;
                    uint32_t c;
                    switch (fmt) {
                    case TF_I4: {
                        uint32_t v = (p[i >> 1] >> ((i & 1) ? 0 : 4)) & 15;
                        v = ext4(v);
                        c = rgba(v, v, v, v);
                        break;
                    }
                    case TF_I8: {
                        uint32_t v = p[i];
                        c = rgba(v, v, v, v);
                        break;
                    }
                    case TF_IA4: {
                        uint32_t v = p[i];
                        uint32_t in = ext4(v & 15), a = ext4(v >> 4);
                        c = rgba(in, in, in, a);
                        break;
                    }
                    case TF_IA8:
                        c = fromIA8(be16(p + i * 2));
                        break;
                    case TF_RGB565:
                        c = fromRGB565(be16(p + i * 2));
                        break;
                    case TF_RGB5A3:
                        c = fromRGB5A3(be16(p + i * 2));
                        break;
                    case TF_RGBA8: {
                        const uint8_t* ar = p + i * 2;
                        const uint8_t* gb = p + 32 + i * 2;
                        c = rgba(ar[1], gb[0], gb[1], ar[0]);
                        break;
                    }
                    case TF_CI4: {
                        uint32_t idx = (p[i >> 1] >> ((i & 1) ? 0 : 4)) & 15;
                        c = tlut ? fromTlut(be16((const uint8_t*)(tlut + idx)), tlutFmt) : rgba(idx * 17, idx * 17, idx * 17, 255);
                        break;
                    }
                    case TF_CI8: {
                        uint32_t idx = p[i];
                        c = tlut ? fromTlut(be16((const uint8_t*)(tlut + idx)), tlutFmt) : rgba(idx, idx, idx, 255);
                        break;
                    }
                    case TF_CI14X2: {
                        uint32_t idx = be16(p + i * 2) & 0x3FFF;
                        c = tlut ? fromTlut(be16((const uint8_t*)(tlut + idx)), tlutFmt) : rgba(255, 0, 255, 255);
                        break;
                    }
                    default:
                        c = rgba(255, 0, 255, 255);
                        break;
                    }
                    if (x < width && y < height) {
                        dst[y * width + x] = c;
                    }
                }
            }
        }
    }
}

}  // namespace gpu
