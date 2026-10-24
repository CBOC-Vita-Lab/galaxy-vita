#include "vgx_texdecode.h"
#include "vgx_common.h"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace vgx {

namespace {
#if defined(__ARM_NEON)
// Eight grey texels as RGBA (the same value four times) / as luminance-alpha.
inline void StoreGray4(uint8_t* d, uint8x8_t v) {
    uint8x8x4_t q;
    q.val[0] = q.val[1] = q.val[2] = q.val[3] = v;
    vst4_u8(d, q);
}
inline void StoreGray2(uint8_t* d, uint8x8_t v) {
    uint8x8x2_t q;
    q.val[0] = q.val[1] = v;
    vst2_u8(d, q);
}
#endif
struct TileInfo { uint32_t tw, th, bytes; }; // tile width/height in texels, bytes per tile
TileInfo Tile(uint32_t fmt) {
    switch (fmt) {
    case 0x0: case 0x8: return {8, 8, 32};  // I4, C4
    case 0x1: case 0x2: case 0x9: return {8, 4, 32}; // I8, IA4, C8
    case 0x3: case 0x4: case 0x5: case 0xA: return {4, 4, 32}; // IA8, RGB565, RGB5A3, C14X2
    case 0x6: return {4, 4, 64};  // RGBA8
    case 0xE: return {8, 8, 32};  // CMPR
    default: return {4, 4, 32};
    }
}
inline void Put(uint8_t* d, uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x >= w || y >= h) return;
    uint8_t* p = d + (static_cast<size_t>(y) * w + x) * 4;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}
inline void Dec565(uint16_t v, uint8_t* o) {
    const int r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
    o[0] = static_cast<uint8_t>((r << 3) | (r >> 2)); o[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
    o[2] = static_cast<uint8_t>((b << 3) | (b >> 2)); o[3] = 255;
}
inline void Dec5A3(uint16_t v, uint8_t* o) {
    if (v & 0x8000) {
        const int r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
        o[0] = static_cast<uint8_t>((r << 3) | (r >> 2)); o[1] = static_cast<uint8_t>((g << 3) | (g >> 2));
        o[2] = static_cast<uint8_t>((b << 3) | (b >> 2)); o[3] = 255;
    } else {
        const int a = (v >> 12) & 0x7, r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
        o[0] = static_cast<uint8_t>(r * 17); o[1] = static_cast<uint8_t>(g * 17); o[2] = static_cast<uint8_t>(b * 17);
        o[3] = static_cast<uint8_t>((a << 5) | (a << 2) | (a >> 1));
    }
}
inline void DecTlut(const uint8_t* tlut, uint32_t fmt, uint32_t entries, uint32_t idx, uint8_t* o) {
    if (!tlut || idx >= entries) { o[0] = o[1] = o[2] = 0; o[3] = 0; return; }
    const uint16_t v = be16(tlut + idx * 2);
    switch (fmt) {
    case 0: o[0] = o[1] = o[2] = static_cast<uint8_t>(v & 0xFF); o[3] = static_cast<uint8_t>(v >> 8); break; // IA8
    case 1: Dec565(v, o); break;
    default: Dec5A3(v, o); break;
    }
}
template <uint32_t fmt>
void DecodeLA88Tiles(const uint8_t* s, uint32_t w, uint32_t h, uint8_t* d) {
    const TileInfo t = Tile(fmt);
#if defined(__ARM_NEON)
    if constexpr (fmt != 1) {
        // Full tiles use vector stores; partial edge tiles stay bounded below.
        if (!(w & (t.tw - 1)) && !(h & (t.th - 1))) {
            const size_t stride = static_cast<size_t>(w) * 2;
            for (uint32_t ty = 0; ty < h; ty += t.th)
                for (uint32_t tx = 0; tx < w; tx += t.tw, s += t.bytes) {
                    uint8_t* row = d + static_cast<size_t>(ty) * stride + tx * 2;
                    if constexpr (fmt == 0) {
                        for (uint32_t y = 0; y < 8; y += 2) {
                            const uint8x8_t packed = vld1_u8(s + y * 4);
                            uint8x8_t hi = vshr_n_u8(packed, 4), lo = vand_u8(packed, vdup_n_u8(15));
                            hi = vorr_u8(hi, vshl_n_u8(hi, 4)); lo = vorr_u8(lo, vshl_n_u8(lo, 4));
                            const uint8x8x2_t pixels = vzip_u8(hi, lo);
                            StoreGray2(row + y * stride, pixels.val[0]);
                            StoreGray2(row + (y + 1) * stride, pixels.val[1]);
                        }
                    } else if constexpr (fmt == 2) {
                        for (uint32_t y = 0; y < 4; ++y) {
                            const uint8x8_t packed = vld1_u8(s + y * 8);
                            uint8x8x2_t pixels;
                            const uint8x8_t l = vand_u8(packed, vdup_n_u8(15)), a = vshr_n_u8(packed, 4);
                            pixels.val[0] = vorr_u8(l, vshl_n_u8(l, 4)); pixels.val[1] = vorr_u8(a, vshl_n_u8(a, 4));
                            vst2_u8(row + y * stride, pixels);
                        }
                    } else {
                        for (uint32_t y = 0; y < 4; ++y) vst1_u8(row + y * stride, vrev16_u8(vld1_u8(s + y * 8)));
                    }
                }
            return;
        }
    }
#endif
    for (uint32_t ty = 0; ty < h; ty += t.th)
        for (uint32_t tx = 0; tx < w; tx += t.tw, s += t.bytes) {
            const uint32_t rows = h - ty < t.th ? h - ty : t.th;
            const uint32_t columns = w - tx < t.tw ? w - tx : t.tw;
            for (uint32_t y = 0; y < rows; ++y) {
                uint8_t* out = d + (static_cast<size_t>(ty + y) * w + tx) * 2;
                if constexpr (fmt == 0) {
                    uint32_t x = 0;
                    for (; x + 1 < columns; x += 2, out += 4) {
                        const uint8_t packed = s[y * 4 + x / 2];
                        out[0] = out[1] = static_cast<uint8_t>((packed >> 4) * 17);
                        out[2] = out[3] = static_cast<uint8_t>((packed & 15) * 17);
                    }
                    if (x < columns) out[0] = out[1] = static_cast<uint8_t>((s[y * 4 + x / 2] >> 4) * 17);
                    continue;
                }
                for (uint32_t x = 0; x < columns; ++x, out += 2) {
                    const uint32_t p = y * t.tw + x;
                    uint8_t l, a;
                    if constexpr (fmt == 0) { l = static_cast<uint8_t>(((s[p / 2] >> ((p & 1) ? 0 : 4)) & 15) * 17); a = l; }
                    else if constexpr (fmt == 1) { l = a = s[p]; }
                    else if constexpr (fmt == 2) { l = static_cast<uint8_t>((s[p] & 15) * 17); a = static_cast<uint8_t>((s[p] >> 4) * 17); }
                    else { l = s[p * 2 + 1]; a = s[p * 2]; }
                    out[0] = l; out[1] = a;
                }
            }
        }
}
} // namespace

bool TexFormatIsCI(uint32_t fmt) { return fmt == 0x8 || fmt == 0x9 || fmt == 0xA; }

uint32_t TexLevelSize(uint32_t w, uint32_t h, uint32_t fmt) {
    const TileInfo t = Tile(fmt);
    return ((w + t.tw - 1) / t.tw) * ((h + t.th - 1) / t.th) * t.bytes;
}
uint32_t TexDataSize(uint32_t w, uint32_t h, uint32_t fmt, uint32_t levels) {
    uint32_t total = 0;
    for (uint32_t l = 0; l < levels; ++l) {
        total += TexLevelSize(w, h, fmt);
        if (w == 1 && h == 1) break;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total;
}

void DecodeTexture(const uint8_t* s, uint32_t w, uint32_t h, uint32_t fmt, const uint8_t* tlut, uint32_t tlutFmt,
                   uint32_t tlutEntries, uint8_t* d) {
    uint8_t c[4];
    switch (fmt) {
    case 0x0: // I4
        for (uint32_t ty = 0; ty < h; ty += 8)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 8; ++y)
                    for (uint32_t x = 0; x < 8; x += 2, ++s) {
                        const uint8_t a = static_cast<uint8_t>((*s >> 4) * 17), b = static_cast<uint8_t>((*s & 0xF) * 17);
                        Put(d, w, h, tx + x, ty + y, a, a, a, a);
                        Put(d, w, h, tx + x + 1, ty + y, b, b, b, b);
                    }
        break;
    case 0x1: // I8
#if defined(__ARM_NEON)
        if (!(w & 7) && !(h & 3)) {
            // (the movies' three planes, every frame: whole tiles, one vector store per row of a tile)
            const size_t stride = static_cast<size_t>(w) * 4;
            for (uint32_t ty = 0; ty < h; ty += 4) {
                uint8_t* row = d + static_cast<size_t>(ty) * stride;
                for (uint32_t tx = 0; tx < w; tx += 8, s += 32, row += 32) {
                    const uint8x8_t r0 = vld1_u8(s), r1 = vld1_u8(s + 8), r2 = vld1_u8(s + 16), r3 = vld1_u8(s + 24);
                    StoreGray4(row, r0);
                    StoreGray4(row + stride, r1);
                    StoreGray4(row + 2 * stride, r2);
                    StoreGray4(row + 3 * stride, r3);
                }
            }
            break;
        }
#endif
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 8; ++x, ++s) Put(d, w, h, tx + x, ty + y, *s, *s, *s, *s);
        break;
    case 0x2: // IA4
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 8; ++x, ++s) {
                        const uint8_t i = static_cast<uint8_t>((*s & 0xF) * 17), a = static_cast<uint8_t>((*s >> 4) * 17);
                        Put(d, w, h, tx + x, ty + y, i, i, i, a);
                    }
        break;
    case 0x3: // IA8
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 4)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x, s += 2) Put(d, w, h, tx + x, ty + y, s[1], s[1], s[1], s[0]);
        break;
    case 0x4: // RGB565
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 4)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x, s += 2) {
                        Dec565(be16(s), c);
                        Put(d, w, h, tx + x, ty + y, c[0], c[1], c[2], 255);
                    }
        break;
    case 0x5: // RGB5A3
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 4)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x, s += 2) {
                        Dec5A3(be16(s), c);
                        Put(d, w, h, tx + x, ty + y, c[0], c[1], c[2], c[3]);
                    }
        break;
    case 0x6: // RGBA8: 4x4 tiles, 32 bytes of AR pairs then 32 bytes of GB pairs
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 4, s += 64)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x) {
                        const uint8_t* ar = s + (y * 4 + x) * 2;
                        const uint8_t* gb = ar + 32;
                        Put(d, w, h, tx + x, ty + y, ar[1], gb[0], gb[1], ar[0]);
                    }
        break;
    case 0x8: // C4
        for (uint32_t ty = 0; ty < h; ty += 8)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 8; ++y)
                    for (uint32_t x = 0; x < 8; x += 2, ++s) {
                        DecTlut(tlut, tlutFmt, tlutEntries, *s >> 4, c);
                        Put(d, w, h, tx + x, ty + y, c[0], c[1], c[2], c[3]);
                        DecTlut(tlut, tlutFmt, tlutEntries, *s & 0xF, c);
                        Put(d, w, h, tx + x + 1, ty + y, c[0], c[1], c[2], c[3]);
                    }
        break;
    case 0x9: // C8
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 8; ++x, ++s) {
                        DecTlut(tlut, tlutFmt, tlutEntries, *s, c);
                        Put(d, w, h, tx + x, ty + y, c[0], c[1], c[2], c[3]);
                    }
        break;
    case 0xA: // C14X2
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 4)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x, s += 2) {
                        DecTlut(tlut, tlutFmt, tlutEntries, be16(s) & 0x3FFF, c);
                        Put(d, w, h, tx + x, ty + y, c[0], c[1], c[2], c[3]);
                    }
        break;
    case 0xE: // CMPR: 8x8 tiles of four DXT1 blocks
        for (uint32_t ty = 0; ty < h; ty += 8)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t sub = 0; sub < 4; ++sub, s += 8) {
                    const uint32_t bx = tx + (sub & 1) * 4, by = ty + (sub >> 1) * 4;
                    const uint16_t c0 = be16(s), c1 = be16(s + 2);
                    uint8_t pal[4][4];
                    Dec565(c0, pal[0]);
                    Dec565(c1, pal[1]);
                    if (c0 > c1) {
                        for (int k = 0; k < 3; ++k) {
                            pal[2][k] = static_cast<uint8_t>((pal[0][k] * 5 + pal[1][k] * 3) >> 3);
                            pal[3][k] = static_cast<uint8_t>((pal[1][k] * 5 + pal[0][k] * 3) >> 3);
                        }
                        pal[2][3] = pal[3][3] = 255;
                    } else {
                        for (int k = 0; k < 3; ++k) pal[2][k] = pal[3][k] = static_cast<uint8_t>((pal[0][k] + pal[1][k]) >> 1);
                        pal[2][3] = 255;
                        pal[3][3] = 0;
                    }
                    for (uint32_t y = 0; y < 4; ++y) {
                        const uint8_t rowBits = s[4 + y];
                        for (uint32_t x = 0; x < 4; ++x) {
                            const uint8_t* q = pal[(rowBits >> (6 - x * 2)) & 3];
                            Put(d, w, h, bx + x, by + y, q[0], q[1], q[2], q[3]);
                        }
                    }
                }
        break;
    default:
        for (uint32_t i = 0; i < w * h; ++i) { d[i * 4] = 255; d[i * 4 + 1] = 0; d[i * 4 + 2] = 255; d[i * 4 + 3] = 255; }
        break;
    }
}

bool DecodeTextureLA88(const uint8_t* s, uint32_t w, uint32_t h, uint32_t fmt, uint8_t* d) {
    if (fmt > 3 || !w || !h || !s || !d) return false;
#if defined(__ARM_NEON)
    if (fmt == 1 && !(w & 7) && !(h & 3)) {
        const size_t stride = static_cast<size_t>(w) * 2;
        for (uint32_t ty = 0; ty < h; ty += 4) {
            uint8_t* row = d + static_cast<size_t>(ty) * stride;
            for (uint32_t tx = 0; tx < w; tx += 8, s += 32, row += 16) {
                for (int r = 0; r < 4; ++r) {
                    const uint8x8_t v = vld1_u8(s + r * 8);
                    StoreGray2(row + r * stride, v);
                }
            }
        }
        return true;
    }
#endif
    switch (fmt) {
    case 0: DecodeLA88Tiles<0>(s, w, h, d); break;
    case 1: DecodeLA88Tiles<1>(s, w, h, d); break;
    case 2: DecodeLA88Tiles<2>(s, w, h, d); break;
    case 3: DecodeLA88Tiles<3>(s, w, h, d); break;
    }
    return true;
}

bool CmprToDxt1(const uint8_t* s, uint32_t w, uint32_t h, uint8_t* d, bool allowTransparent) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4; // DXT1 blocks
    const uint32_t tw = (w + 7) / 8;                   // CMPR tiles per row
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx, d += 8) {
            const uint8_t* b = s + ((static_cast<size_t>(by >> 1) * tw + (bx >> 1)) * 4 + (by & 1) * 2 + (bx & 1)) * 8;
            const uint32_t c0 = be16(b), c1 = be16(b + 2);
            d[0] = b[1]; d[1] = b[0]; d[2] = b[3]; d[3] = b[2];
            for (int y = 0; y < 4; ++y) {
                // GX puts the leftmost texel in the top two bits, DXT1 in the bottom two.
                const uint32_t r = b[4 + y];
                if (!allowTransparent && c0 <= c1 && ((r & (r >> 1)) & 0x55u)) return false; // entry 3 of a three-colour block
                d[4 + y] = static_cast<uint8_t>(((r & 0x03u) << 6) | ((r & 0x0Cu) << 2) | ((r & 0x30u) >> 2) | ((r & 0xC0u) >> 6));
            }
        }
    return true;
}

void EncodeDxt1(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* d) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx, d += 8) {
            uint8_t px[16][3];
            int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t sx = bx * 4 + x < w ? bx * 4 + x : w - 1, sy = by * 4 + y < h ? by * 4 + y : h - 1;
                    const uint8_t* p = rgba + (static_cast<size_t>(sy) * w + sx) * 4;
                    for (int k = 0; k < 3; ++k) {
                        px[y * 4 + x][k] = p[k];
                        if (p[k] < lo[k]) lo[k] = p[k];
                        if (p[k] > hi[k]) hi[k] = p[k];
                    }
                }
            const auto to565 = [](const int* c) { return static_cast<uint32_t>(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)); };
            uint32_t c0 = to565(hi), c1 = to565(lo);
            if (c0 < c1) { const uint32_t t = c0; c0 = c1; c1 = t; }
            d[0] = static_cast<uint8_t>(c0); d[1] = static_cast<uint8_t>(c0 >> 8);
            d[2] = static_cast<uint8_t>(c1); d[3] = static_cast<uint8_t>(c1 >> 8);
            if (c0 == c1) { d[4] = d[5] = d[6] = d[7] = 0; continue; } // (entry 0 of a three-colour block: opaque)
            uint8_t pal[4][4];
            Dec565(static_cast<uint16_t>(c0), pal[0]);
            Dec565(static_cast<uint16_t>(c1), pal[1]);
            for (int k = 0; k < 3; ++k) {
                pal[2][k] = static_cast<uint8_t>((2 * pal[0][k] + pal[1][k] + 1) / 3);
                pal[3][k] = static_cast<uint8_t>((pal[0][k] + 2 * pal[1][k] + 1) / 3);
            }
            for (uint32_t y = 0; y < 4; ++y) {
                uint32_t row = 0;
                for (uint32_t x = 0; x < 4; ++x) {
                    int best = 0, bestD = 1 << 30;
                    for (int e = 0; e < 4; ++e) {
                        int dist = 0;
                        for (int k = 0; k < 3; ++k) { const int df = px[y * 4 + x][k] - pal[e][k]; dist += df * df; }
                        if (dist < bestD) { bestD = dist; best = e; }
                    }
                    row |= static_cast<uint32_t>(best) << (x * 2);
                }
                d[4 + y] = static_cast<uint8_t>(row);
            }
        }
}

void Downsample(const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& dst, uint32_t& dw, uint32_t& dh) {
    dw = w > 1 ? w / 2 : 1;
    dh = h > 1 ? h / 2 : 1;
    dst.resize(static_cast<size_t>(dw) * dh * 4);
    for (uint32_t y = 0; y < dh; ++y)
        for (uint32_t x = 0; x < dw; ++x) {
            const uint32_t x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : w - 1;
            const uint32_t y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : h - 1;
            for (int k = 0; k < 4; ++k) {
                const uint32_t sum = src[(static_cast<size_t>(y0) * w + x0) * 4 + k] + src[(static_cast<size_t>(y0) * w + x1) * 4 + k] +
                                     src[(static_cast<size_t>(y1) * w + x0) * 4 + k] + src[(static_cast<size_t>(y1) * w + x1) * 4 + k];
                dst[(static_cast<size_t>(y) * dw + x) * 4 + k] = static_cast<uint8_t>((sum + 2) >> 2);
            }
        }
}

} // namespace vgx
