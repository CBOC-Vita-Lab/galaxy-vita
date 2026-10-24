// THP movie frame decoder.  The SDK's THPDec.c is Broadway assembly (paired
// singles and the locked cache), so this is a portable replacement with the
// same contract: THPVideoDecode() takes one frame's video component and
// writes the Y, U and V planes straight into GX I8 textures (8x4 texel tiles
// of 32 bytes each) for THPGXYuv2RgbDraw().
//
// THP video is baseline JPEG with fixed 4:2:0 sampling, except that the
// entropy-coded data has no 0xFF00 byte stuffing and a restart interval only
// realigns to the next byte (there are no RSTn markers).  Error codes match
// the SDK's.
//
// Speed.  The movie of the opening (640x368, 60 pictures a second, 21 Mbit/s) needs a picture every 16.7 ms.
// On the Vita's 444 MHz Cortex-A9 the first (scalar, 64-bit bit buffer) version took about 20 million instructions
// a picture, three to four times what one core can give. This version
//   - reads bits with one unaligned 32-bit load and a shift per symbol (no 64-bit accumulator, no refill loop),
//     which is possible because the stream has no byte stuffing;
//   - dequantizes while it decodes (only the coefficients that exist), into a float block;
//   - runs the AAN IDCT on NEON (four lanes), straight into the 8x4 tiles of the output planes;
//   - can decode pictures on worker threads a picture or two ahead of the game (see "Pipeline"): the game thread
//     (and the audio mixer on its core) then only copies the finished planes.
#ifdef THP_STANDALONE  // host-side test build (tools/thp_test)
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t s16;
typedef int32_t s32;
typedef int BOOL;
#define TRUE 1
#else
#include "port/heap_routing.h"
#include "revolution.h"
#include "revolution/thp.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define THP_NEON 1
#endif

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__vita__) && !defined(THP_STANDALONE)
#include <psp2/kernel/threadmgr.h>
#endif

extern "C" void port_gpu_memory_written(void* addr, uint32_t size);
extern "C" int port_vita_pin_core(int core) __attribute__((weak));
extern "C" int port_vita_thread_class(int cls) __attribute__((weak));
extern "C" int64_t port_thread_cpu_us(void) __attribute__((weak));  // CPU time of the calling thread (the console)
extern "C" void port_log(const char* fmt, ...) __attribute__((weak));

namespace {

enum : s32 {
    kErrBadSyntax = 3,
    kErrBadPrecision = 10,
    kErrUnsupportedMarker = 11,
    kErrBadComponents = 12,
    kErrBadTable = 15,
    kErrBadSampling = 19,
    kErrNoInput = 25,
    kErrNoWork = 26,
    kErrNoOutput = 27,
};

// zigzag index -> natural (row-major) index.  Corrupt data can run past the end of a block; the tail keeps it inside.
alignas(16) const u8 kNaturalOrder[64 + 16] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,
};

// AAN IDCT scale factors: cos(k*pi/16) * sqrt(2) for k > 0.
const double kAanScale[8] = {1.0, 1.387039845, 1.306562965, 1.175875602, 1.0, 0.785694958, 0.541196100, 0.275899379};

struct Huffman {
    enum { kLookBits = 9 };
    u16 look[1 << kLookBits];  // (length << 8) | value for codes up to kLookBits long, 0 otherwise
    s32 maxCode[18];           // largest code of each length, -1 if there is none
    s32 valOffset[18];         // value index = code + valOffset[length]
    u8 values[256];
    u8 raw[16 + 256];          // the table as read (counts, values): an identical table is not rebuilt
    u32 rawSize;
};

struct Component {
    u8 quant, dc, ac;
    s32 pred;
};

struct Decoder {
    // Dequantization per table: natural order with the AAN scale and the IDCT's final 1/8 folded in (exact: a power
    // of two), and the same in zigzag order for the coefficient loop.
    alignas(16) float quantNat[4][64];
    alignas(16) float quantZig[4][64 + 16];
    u8 quantRaw[4][128];
    u32 quantRawSize[4];
    Huffman huff[4];  // [id * 2 + class], class 0 = DC, 1 = AC
    u8 validHuff;
    u8 validQuant;
    Component comp[3];
    u32 width, height;
    u32 restartInterval;
};

u32 read16(const u8* p) { return (u32)p[0] << 8 | p[1]; }

// ---------------------------------------------------------------------------
// Headers
// ---------------------------------------------------------------------------
s32 readQuant(Decoder& d, const u8*& c, const u8* end) {
    s32 length = (s32)read16(c) - 2;
    c += 2;
    while (length > 0 && c < end) {
        u8 pq = *c >> 4, id = *c & 3;
        c++;
        const u32 bytes = pq ? 128 : 64;
        length -= pq ? 129 : 65;
        if (!(d.validQuant & (1 << id)) || d.quantRawSize[id] != bytes || memcmp(d.quantRaw[id], c, bytes) != 0) {
            memcpy(d.quantRaw[id], c, bytes);
            d.quantRawSize[id] = bytes;
            float q[64];
            for (int i = 0; i < 64; i++) {
                q[kNaturalOrder[i]] = pq ? (float)read16(c + i * 2) : (float)c[i];
            }
            for (int row = 0, i = 0; row < 8; row++) {
                for (int col = 0; col < 8; col++, i++) {
                    d.quantNat[id][i] = (float)((double)q[i] * kAanScale[row] * kAanScale[col] * 0.125);
                }
            }
            for (int k = 0; k < 64 + 16; k++) {
                d.quantZig[id][k] = d.quantNat[id][kNaturalOrder[k]];
            }
        }
        c += bytes;
        d.validQuant |= 1 << id;
    }
    return 0;
}

s32 readHuffman(Decoder& d, const u8*& c, const u8* end) {
    s32 length = (s32)read16(c) - 2;
    c += 2;
    while (length > 0 && c + 17 <= end) {
        u8 cls = *c >> 4, id = *c & 15;
        c++;
        if (cls > 1 || id > 1) {
            return kErrBadTable;
        }
        Huffman& h = d.huff[id * 2 + cls];
        const u8* counts = c;
        s32 total = 0;
        for (int l = 0; l < 16; l++) {
            total += counts[l];
        }
        if (total > 256 || c + 16 + total > end) {
            return kErrBadTable;
        }
        const u32 rawSize = 16 + (u32)total;
        const bool same = (d.validHuff & (1 << (id * 2 + cls))) && h.rawSize == rawSize && memcmp(h.raw, counts, rawSize) == 0;
        c += 16;
        const u8* vals = c;
        c += total;
        length -= 17 + total;
        if (same) {
            continue;
        }
        memcpy(h.raw, counts, rawSize);
        h.rawSize = rawSize;
        memcpy(h.values, vals, (size_t)total);

        memset(h.look, 0, sizeof(h.look));
        s32 code = 0, k = 0;
        for (int l = 1; l <= 16; l++) {
            h.valOffset[l] = k - code;
            if (counts[l - 1] == 0) {
                h.maxCode[l] = -1;
            } else {
                for (int i = 0; i < counts[l - 1]; i++, code++, k++) {
                    if (l <= Huffman::kLookBits) {
                        int shift = Huffman::kLookBits - l;
                        for (int j = 0; j < (1 << shift); j++) {
                            h.look[(code << shift) | j] = (u16)(l << 8 | h.values[k]);
                        }
                    }
                }
                h.maxCode[l] = code - 1;
            }
            code <<= 1;
        }
        h.maxCode[17] = 0x7FFFFFFF;
        d.validHuff |= 1 << (id * 2 + cls);
    }
    return 0;
}

s32 readFrameHeader(Decoder& d, const u8*& c) {
    c += 2;
    if (*c++ != 8) {
        return kErrBadPrecision;
    }
    d.height = read16(c);
    d.width = read16(c + 2);
    c += 4;
    if (*c++ != 3) {
        return kErrBadComponents;
    }
    for (int i = 0; i < 3; i++) {
        c++;  // component id
        u8 sampling = *c++;
        if ((i == 0 && sampling != 0x22) || (i > 0 && sampling != 0x11)) {
            return kErrBadSampling;
        }
        d.comp[i].quant = *c++ & 3;
    }
    return 0;
}

s32 readScanHeader(Decoder& d, const u8*& c) {
    c += 2;
    if (*c++ != 3) {
        return kErrBadComponents;
    }
    for (int i = 0; i < 3; i++) {
        c++;  // component id
        u8 tables = *c++;
        d.comp[i].dc = (u8)((tables >> 4) & 1) * 2;
        d.comp[i].ac = (u8)(tables & 1) * 2 + 1;
        if (!(d.validHuff & (1 << d.comp[i].dc)) || !(d.validHuff & (1 << d.comp[i].ac))) {
            return kErrBadTable;
        }
        d.comp[i].pred = 0;
    }
    c += 3;  // spectral selection and approximation: always 0, 63, 0
    return 0;
}

// ---------------------------------------------------------------------------
// Entropy decoding.  The stream is read through a bit position: peek() returns the next 25 or more bits at the top of a
// 32-bit word with one unaligned load, a byte swap and a shift.
// ---------------------------------------------------------------------------
inline u32 peek(const u8* base, u32 pos) {
    u32 w;
    memcpy(&w, base + (pos >> 3), 4);
    return __builtin_bswap32(w) << (pos & 7);
}

inline s32 extend(u32 v, int s) { return v < (1u << (s - 1)) ? (s32)v - (1 << s) + 1 : (s32)v; }

// A symbol whose code is longer than the lookup table's: the canonical-code walk, from a window of 16 or more valid bits.
inline int slowSymbol(const Huffman& h, u32 w, int& len) {
    u32 code16 = w >> 16;
    for (int l = Huffman::kLookBits + 1; l <= 16; l++) {
        s32 code = (s32)(code16 >> (16 - l));
        if (code <= h.maxCode[l]) {
            len = l;
            return h.values[(code + h.valOffset[l]) & 0xFF];
        }
    }
    len = 16;  // corrupt data
    return 0;
}

// Decodes one block into `blk` (natural order, dequantized, 1/8 folded in; zero where nothing was coded) and returns how
// many entries of the zigzag sequence were consumed.
inline int decodeBlock(const u8* base, u32& pos, Component& c, const Decoder& d, float* blk) {
#ifdef THP_NEON
    {
        const float32x4_t z = vdupq_n_f32(0.0f);
        for (int i = 0; i < 64; i += 4) {
            vst1q_f32(blk + i, z);
        }
    }
#else
    memset(blk, 0, 64 * sizeof(float));
#endif
    const Huffman& dcTable = d.huff[c.dc];
    const float* qz = d.quantZig[c.quant];
    u32 w = peek(base, pos);
    {
        u16 e = dcTable.look[w >> (32 - Huffman::kLookBits)];
        int len, s;
        if (e) {
            len = e >> 8;
            s = e & 0xFF;
        } else {
            s = slowSymbol(dcTable, w, len);
        }
        pos += len;
        if (s) {
            s &= 15;
            u32 v = peek(base, pos) >> (32 - s);
            pos += s;
            c.pred += extend(v, s);
        }
    }
    blk[0] = (float)c.pred * qz[0];
    int k = 1;
    const Huffman& ac = d.huff[c.ac];
    while (k < 64) {
        w = peek(base, pos);
        u16 e = ac.look[w >> (32 - Huffman::kLookBits)];
        int len, rs;
        if (e) {
            len = e >> 8;
            rs = e & 0xFF;
        } else {
            rs = slowSymbol(ac, w, len);
        }
        int s = rs & 15;
        if (s) {
            k += rs >> 4;
            // code (at most 16 bits) and magnitude (at most 10): both from a fresh window when the code was long
            u32 v;
            if (e) {
                v = (w << len) >> (32 - s);
                pos += len + s;
            } else {
                pos += len;
                v = peek(base, pos) >> (32 - s);
                pos += s;
            }
            blk[kNaturalOrder[k]] = (float)extend(v, s) * qz[k];
            k++;
        } else {
            pos += len;
            if ((rs >> 4) == 15) {
                k += 16;
            } else {
                break;
            }
        }
    }
    return k;
}

// ---------------------------------------------------------------------------
// Inverse DCT (AAN, as in libjpeg's jidctflt.c, 1/8 already in the dequantization) into 8-bit pixels.
// ---------------------------------------------------------------------------
#ifdef THP_NEON

#define THP_IDCT_1D(i0, i1, i2, i3, i4, i5, i6, i7, o0, o1, o2, o3, o4, o5, o6, o7)                                        \
    {                                                                                                                      \
        float32x4_t t10 = vaddq_f32(i0, i4), t11 = vsubq_f32(i0, i4);                                                      \
        float32x4_t t13 = vaddq_f32(i2, i6);                                                                               \
        float32x4_t t12 = vsubq_f32(vmulq_n_f32(vsubq_f32(i2, i6), 1.414213562f), t13);                                    \
        float32x4_t t0 = vaddq_f32(t10, t13), t3 = vsubq_f32(t10, t13);                                                    \
        float32x4_t t1 = vaddq_f32(t11, t12), t2 = vsubq_f32(t11, t12);                                                    \
        float32x4_t z13 = vaddq_f32(i5, i3), z10 = vsubq_f32(i5, i3);                                                      \
        float32x4_t z11 = vaddq_f32(i1, i7), z12 = vsubq_f32(i1, i7);                                                      \
        float32x4_t t7 = vaddq_f32(z11, z13);                                                                              \
        float32x4_t u11 = vmulq_n_f32(vsubq_f32(z11, z13), 1.414213562f);                                                  \
        float32x4_t z5 = vmulq_n_f32(vaddq_f32(z10, z12), 1.847759065f);                                                   \
        float32x4_t u10 = vsubq_f32(vmulq_n_f32(z12, 1.082392200f), z5);                                                   \
        float32x4_t u12 = vaddq_f32(vmulq_n_f32(z10, -2.613125930f), z5);                                                  \
        float32x4_t t6 = vsubq_f32(u12, t7);                                                                               \
        float32x4_t t5 = vsubq_f32(u11, t6);                                                                               \
        float32x4_t t4 = vaddq_f32(u10, t5);                                                                               \
        o0 = vaddq_f32(t0, t7);                                                                                            \
        o7 = vsubq_f32(t0, t7);                                                                                            \
        o1 = vaddq_f32(t1, t6);                                                                                            \
        o6 = vsubq_f32(t1, t6);                                                                                            \
        o2 = vaddq_f32(t2, t5);                                                                                            \
        o5 = vsubq_f32(t2, t5);                                                                                            \
        o4 = vaddq_f32(t3, t4);                                                                                            \
        o3 = vsubq_f32(t3, t4);                                                                                            \
    }

inline void transpose4(float32x4_t& a, float32x4_t& b, float32x4_t& c, float32x4_t& d) {
    float32x4x2_t t0 = vtrnq_f32(a, b);
    float32x4x2_t t1 = vtrnq_f32(c, d);
    a = vcombine_f32(vget_low_f32(t0.val[0]), vget_low_f32(t1.val[0]));
    b = vcombine_f32(vget_low_f32(t0.val[1]), vget_low_f32(t1.val[1]));
    c = vcombine_f32(vget_high_f32(t0.val[0]), vget_high_f32(t1.val[0]));
    d = vcombine_f32(vget_high_f32(t0.val[1]), vget_high_f32(t1.val[1]));
}

// blk: 64 dequantized coefficients (row-major, 16-byte aligned).  rows[y] receive the pixel rows.
inline void idct8x8(const float* blk, uint8x8_t rows[8]) {
    float32x4_t a[8], b[8];  // columns 0-3 and 4-7 of each row
    for (int i = 0; i < 8; i++) {
        a[i] = vld1q_f32(blk + i * 8);
        b[i] = vld1q_f32(blk + i * 8 + 4);
    }
    float32x4_t ta[8], tb[8];
    THP_IDCT_1D(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], ta[0], ta[1], ta[2], ta[3], ta[4], ta[5], ta[6], ta[7]);
    THP_IDCT_1D(b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], tb[0], tb[1], tb[2], tb[3], tb[4], tb[5], tb[6], tb[7]);
    // ta[y]: columns 0-3 of row y, tb[y]: columns 4-7.  Transpose so a register holds one column for four rows.
    transpose4(ta[0], ta[1], ta[2], ta[3]);  // rows 0-3, columns 0-3: ta[u] = column u
    transpose4(ta[4], ta[5], ta[6], ta[7]);  // rows 4-7, columns 0-3
    transpose4(tb[0], tb[1], tb[2], tb[3]);  // rows 0-3, columns 4-7
    transpose4(tb[4], tb[5], tb[6], tb[7]);  // rows 4-7, columns 4-7
    float32x4_t r0[8], r1[8];  // [x]: pixel column x of rows 0-3 / rows 4-7
    THP_IDCT_1D(ta[0], ta[1], ta[2], ta[3], tb[0], tb[1], tb[2], tb[3], r0[0], r0[1], r0[2], r0[3], r0[4], r0[5], r0[6], r0[7]);
    THP_IDCT_1D(ta[4], ta[5], ta[6], ta[7], tb[4], tb[5], tb[6], tb[7], r1[0], r1[1], r1[2], r1[3], r1[4], r1[5], r1[6], r1[7]);
    const float32x4_t bias = vdupq_n_f32(128.5f);
    uint8x8_t c[8];
    for (int x = 0; x < 8; x++) {
        uint32x4_t lo = vcvtq_u32_f32(vaddq_f32(r0[x], bias));
        uint32x4_t hi = vcvtq_u32_f32(vaddq_f32(r1[x], bias));
        c[x] = vqmovn_u16(vcombine_u16(vqmovn_u32(lo), vqmovn_u32(hi)));
    }
    // c[x] holds pixel column x for rows 0-7: transpose the bytes into rows.
    uint8x8x2_t t0 = vtrn_u8(c[0], c[1]), t1 = vtrn_u8(c[2], c[3]), t2 = vtrn_u8(c[4], c[5]), t3 = vtrn_u8(c[6], c[7]);
    uint16x4x2_t u0 = vtrn_u16(vreinterpret_u16_u8(t0.val[0]), vreinterpret_u16_u8(t1.val[0]));
    uint16x4x2_t u1 = vtrn_u16(vreinterpret_u16_u8(t0.val[1]), vreinterpret_u16_u8(t1.val[1]));
    uint16x4x2_t u2 = vtrn_u16(vreinterpret_u16_u8(t2.val[0]), vreinterpret_u16_u8(t3.val[0]));
    uint16x4x2_t u3 = vtrn_u16(vreinterpret_u16_u8(t2.val[1]), vreinterpret_u16_u8(t3.val[1]));
    uint32x2x2_t r04 = vtrn_u32(vreinterpret_u32_u16(u0.val[0]), vreinterpret_u32_u16(u2.val[0]));
    uint32x2x2_t r26 = vtrn_u32(vreinterpret_u32_u16(u0.val[1]), vreinterpret_u32_u16(u2.val[1]));
    uint32x2x2_t r15 = vtrn_u32(vreinterpret_u32_u16(u1.val[0]), vreinterpret_u32_u16(u3.val[0]));
    uint32x2x2_t r37 = vtrn_u32(vreinterpret_u32_u16(u1.val[1]), vreinterpret_u32_u16(u3.val[1]));
    rows[0] = vreinterpret_u8_u32(r04.val[0]);
    rows[4] = vreinterpret_u8_u32(r04.val[1]);
    rows[2] = vreinterpret_u8_u32(r26.val[0]);
    rows[6] = vreinterpret_u8_u32(r26.val[1]);
    rows[1] = vreinterpret_u8_u32(r15.val[0]);
    rows[5] = vreinterpret_u8_u32(r15.val[1]);
    rows[3] = vreinterpret_u8_u32(r37.val[0]);
    rows[7] = vreinterpret_u8_u32(r37.val[1]);
}

inline void idctToTemp(const float* blk, int count, u8* out /* 8x8 */) {
    if (count <= 1) {
        float v = blk[0] + 128.5f;
        u8 p = v <= 0.0f ? 0 : v >= 255.0f ? 255 : (u8)v;
        memset(out, p, 64);
        return;
    }
    uint8x8_t rows[8];
    idct8x8(blk, rows);
    for (int y = 0; y < 8; y++) {
        vst1_u8(out + y * 8, rows[y]);
    }
}

#else  // portable version

inline u8 toPixel(float v) {
    v = v + 128.5f;
    return v <= 0.0f ? 0 : v >= 255.0f ? 255 : (u8)v;
}

void idctToTemp(const float* in0, int count, u8* out /* 8x8 */) {
    if (count <= 1) {
        memset(out, toPixel(in0[0]), 64);
        return;
    }
    float ws[64];
    for (int col = 0; col < 8; col++) {
        const float* in = in0 + col;
        float* w = ws + col;
        float tmp0 = in[0], tmp1 = in[16], tmp2 = in[32], tmp3 = in[48];
        float tmp10 = tmp0 + tmp2, tmp11 = tmp0 - tmp2;
        float tmp13 = tmp1 + tmp3, tmp12 = (tmp1 - tmp3) * 1.414213562f - tmp13;
        tmp0 = tmp10 + tmp13;
        tmp3 = tmp10 - tmp13;
        tmp1 = tmp11 + tmp12;
        tmp2 = tmp11 - tmp12;

        float tmp4 = in[8], tmp5 = in[24], tmp6 = in[40], tmp7 = in[56];
        float z13 = tmp6 + tmp5, z10 = tmp6 - tmp5, z11 = tmp4 + tmp7, z12 = tmp4 - tmp7;
        tmp7 = z11 + z13;
        tmp11 = (z11 - z13) * 1.414213562f;
        float z5 = (z10 + z12) * 1.847759065f;
        tmp10 = 1.082392200f * z12 - z5;
        tmp12 = -2.613125930f * z10 + z5;
        tmp6 = tmp12 - tmp7;
        tmp5 = tmp11 - tmp6;
        tmp4 = tmp10 + tmp5;

        w[0] = tmp0 + tmp7;
        w[56] = tmp0 - tmp7;
        w[8] = tmp1 + tmp6;
        w[48] = tmp1 - tmp6;
        w[16] = tmp2 + tmp5;
        w[40] = tmp2 - tmp5;
        w[32] = tmp3 + tmp4;
        w[24] = tmp3 - tmp4;
    }
    for (int row = 0; row < 8; row++) {
        const float* w = ws + row * 8;
        u8* o = out + row * 8;
        float tmp10 = w[0] + w[4], tmp11 = w[0] - w[4];
        float tmp13 = w[2] + w[6], tmp12 = (w[2] - w[6]) * 1.414213562f - tmp13;
        float tmp0 = tmp10 + tmp13, tmp3 = tmp10 - tmp13, tmp1 = tmp11 + tmp12, tmp2 = tmp11 - tmp12;

        float z13 = w[5] + w[3], z10 = w[5] - w[3], z11 = w[1] + w[7], z12 = w[1] - w[7];
        float tmp7 = z11 + z13;
        tmp11 = (z11 - z13) * 1.414213562f;
        float z5 = (z10 + z12) * 1.847759065f;
        tmp10 = 1.082392200f * z12 - z5;
        tmp12 = -2.613125930f * z10 + z5;
        float tmp6 = tmp12 - tmp7, tmp5 = tmp11 - tmp6, tmp4 = tmp10 + tmp5;

        o[0] = toPixel(tmp0 + tmp7);
        o[7] = toPixel(tmp0 - tmp7);
        o[1] = toPixel(tmp1 + tmp6);
        o[6] = toPixel(tmp1 - tmp6);
        o[2] = toPixel(tmp2 + tmp5);
        o[5] = toPixel(tmp2 - tmp5);
        o[4] = toPixel(tmp3 + tmp4);
        o[3] = toPixel(tmp3 - tmp4);
    }
}

#endif

// Writes an 8x8 block at (x, y) into an I8 texture of the given size, clipped (the picture's edge).
void storeBlockClipped(u8* plane, u32 width, u32 height, u32 x, u32 y, const u8* px) {
    u32 tilesPerRow = (width + 7) / 8;
    for (u32 r = 0; r < 8 && y + r < height; r++) {
        u32 row = y + r;
        memcpy(plane + ((size_t)(row >> 2) * tilesPerRow + (x >> 3)) * 32 + (row & 3) * 8, px + r * 8, 8);
    }
}

// Decodes the block coded next and stores it at (x, y) of the plane.  The block is whole (inside the picture) in nearly
// every case, then the NEON IDCT writes its two tiles directly.
struct PlaneOut {
    u8* plane;
    u32 width, height, tilesPerRow;
};

inline void storeBlock(const PlaneOut& o, u32 x, u32 y, const float* blk, int count) {
#ifdef THP_NEON
    if (x + 8 <= o.width && y + 8 <= o.height && count > 1) {
        uint8x8_t rows[8];
        idct8x8(blk, rows);
        u8* t0 = o.plane + ((size_t)(y >> 2) * o.tilesPerRow + (x >> 3)) * 32;
        u8* t1 = t0 + (size_t)o.tilesPerRow * 32;
        vst1_u8(t0, rows[0]);
        vst1_u8(t0 + 8, rows[1]);
        vst1_u8(t0 + 16, rows[2]);
        vst1_u8(t0 + 24, rows[3]);
        vst1_u8(t1, rows[4]);
        vst1_u8(t1 + 8, rows[5]);
        vst1_u8(t1 + 16, rows[6]);
        vst1_u8(t1 + 24, rows[7]);
        return;
    }
    if (x + 8 <= o.width && y + 8 <= o.height) {  // DC only: a flat tile pair
        float v = blk[0] + 128.5f;
        const uint8x8_t p = vdup_n_u8(v <= 0.0f ? 0 : v >= 255.0f ? 255 : (u8)v);
        u8* t0 = o.plane + ((size_t)(y >> 2) * o.tilesPerRow + (x >> 3)) * 32;
        u8* t1 = t0 + (size_t)o.tilesPerRow * 32;
        for (int i = 0; i < 32; i += 8) {
            vst1_u8(t0 + i, p);
            vst1_u8(t1 + i, p);
        }
        return;
    }
#endif
    alignas(8) u8 px[64];
    idctToTemp(blk, count, px);
    storeBlockClipped(o.plane, o.width, o.height, x, y, px);
}

s32 decodeScan(Decoder& d, const u8* data, u32 limit, u8* tileY, u8* tileU, u8* tileV) {
    alignas(16) float blk[64];
    u32 mcusX = (d.width + 15) / 16, mcusY = (d.height + 15) / 16;
    u32 cw = (d.width + 1) / 2, ch = (d.height + 1) / 2;
    const PlaneOut oy = {tileY, d.width, d.height, (d.width + 7) / 8};
    const PlaneOut ou = {tileU, cw, ch, (cw + 7) / 8};
    const PlaneOut ov = {tileV, cw, ch, (cw + 7) / 8};
    u32 untilRestart = d.restartInterval;
    u32 pos = 0;
    const u32 limitBits = limit * 8;
    for (u32 my = 0; my < mcusY; my++) {
        for (u32 mx = 0; mx < mcusX; mx++) {
            if (pos + 64 > limitBits) {
                return 0;  // ran out of data (corrupt or truncated picture): leave the rest as it is
            }
            for (u32 b = 0; b < 4; b++) {
                int n = decodeBlock(data, pos, d.comp[0], d, blk);
                storeBlock(oy, mx * 16 + (b & 1) * 8, my * 16 + (b >> 1) * 8, blk, n);
            }
            int n = decodeBlock(data, pos, d.comp[1], d, blk);
            storeBlock(ou, mx * 8, my * 8, blk, n);
            n = decodeBlock(data, pos, d.comp[2], d, blk);
            storeBlock(ov, mx * 8, my * 8, blk, n);

            if (d.restartInterval && --untilRestart == 0) {
                untilRestart = d.restartInterval;
                pos = (pos + 7) & ~7u;  // next byte boundary
                d.comp[0].pred = d.comp[1].pred = d.comp[2].pred = 0;
            }
        }
    }
    return 0;
}

// Bytes of the three planes for a picture of this size (I8 tiles of 8x4 texels).
inline u32 lumaBytes(u32 w, u32 h) { return ((w + 7) / 8) * 32 * ((h + 3) / 4); }
inline u32 chromaBytes(u32 w, u32 h) { return lumaBytes((w + 1) / 2, (h + 1) / 2); }

// One frame: headers, then the scan.  `limit` bounds the entropy-coded data.
s32 decodeFrame(Decoder& d, const void* file, u32 limit, u8* tileY, u8* tileU, u8* tileV) {
    // (validHuff / validQuant stay: a table identical to the one already built is not read again)
    d.restartInterval = 0;
    d.width = d.height = 0;
    const u8* start = (const u8*)file;
    const u8* c = start;
    const u8* end = c + limit;
    for (;;) {
        if (c >= end || *c++ != 0xFF) {
            return kErrBadSyntax;
        }
        while (c < end && *c == 0xFF) {
            c++;
        }
        if (c >= end) {
            return kErrBadSyntax;
        }
        u8 marker = *c++;
        s32 status = 0;
        switch (marker) {
        case 0xD8:  // SOI
            break;
        case 0xC0:  // SOF0
            status = readFrameHeader(d, c);
            break;
        case 0xC4:  // DHT
            status = readHuffman(d, c, end);
            break;
        case 0xDB:  // DQT
            status = readQuant(d, c, end);
            break;
        case 0xDD:  // DRI
            d.restartInterval = read16(c + 2);
            c += 4;
            break;
        case 0xDA:  // SOS: the entropy-coded data follows
            status = readScanHeader(d, c);
            if (status == 0) {
                if (!d.width || !d.height) {
                    return kErrBadSyntax;
                }
                return decodeScan(d, c, (u32)(end - c), tileY, tileU, tileV);
            }
            return status;
        default:
            if ((marker >= 0xE0 && marker <= 0xEF) || marker == 0xFE) {
                c += read16(c);
                break;
            }
            return kErrUnsupportedMarker;
        }
        if (status != 0) {
            return status;
        }
    }
}

Decoder sDec;  // the calling thread's own decoder (synchronous mode)

// ---------------------------------------------------------------------------
// Pipeline.  THPVideoDecode() is called once per game turn, on the game thread (which on the Vita is also the audio
// mixer's core).  With workers, a call copies the compressed frame into a ring slot, wakes a worker, and returns the
// NEWEST picture that is finished (copying it into the caller's planes): never a wait for a particular one, so the game
// does not slow down for the movie, and a slow decoder shows up as pictures that are a little older, or (when it cannot
// keep up at all) as pictures that are shown twice, never as a stutter of the game or its sound.  Pictures are
// independent (every frame is a JPEG), so any worker can take any slot, and the workers take the newest job first: when
// they are behind they spend their time on the pictures the game is about to ask for, not on ones nobody will show.
// ---------------------------------------------------------------------------
constexpr int kRing = 8;
constexpr int kMaxWorkers = 3;
constexpr u32 kFrameCap = 96 * 1024;  // upper bound of a compressed video frame when its size is not known (THP max here: 51 KB)

enum SlotState : int { kFree = 0, kQueued, kBusy, kDone, kSkipped };

struct Slot {
    std::vector<u8> in;
    u32 inSize = 0;
    std::vector<u8> y, u, v;
    u32 width = 0, height = 0;
    SlotState state = kFree;
    s32 status = 0;
    u64 seq = 0;
};

struct Pipeline {
    std::mutex mu;
    std::condition_variable work, done;
    Slot slots[kRing];
    int queue[kRing];
    int queued = 0;
    u64 calls = 0;      // pictures submitted since the last restart
    u64 base = 0;       // calls at the last restart
    bool quit = false;
    int workers = 0;
    std::thread threads[kMaxWorkers];
    Decoder decoders[kMaxWorkers];
    std::chrono::steady_clock::time_point lastCall;
    // statistics
    u64 shown = 0, repeated = 0, skipped = 0, waits = 0, decoded = 0, fresh = 0, lagSum = 0, dropped = 0;
    double waitMs = 0, decodeMs = 0, decodeCpuMs = 0;
    u64 lastShownSeq = 0;
    bool anyShown = false;
};

Pipeline* gPipe = nullptr;
std::mutex gPipeInitMu;
bool gPipeInitDone = false;
u32 gFrameSizeHint = 0;

int envInt(const char* name, int fallback) {
    const char* e = getenv(name);
    return e && *e ? atoi(e) : fallback;
}

void pinWorker(int index) {
#if defined(__vita__) && !defined(THP_STANDALONE)
    // Worker 0: the render core; worker 1: the auxiliary core; worker 2: the game core. While the movie plays there is no
    // 3D scene, so those cores have time; at the lowest priority the workers only take what the others leave.
    static const int kCores[kMaxWorkers] = {1, 2, 0};
    if (port_vita_pin_core) {
        port_vita_pin_core(kCores[index % kMaxWorkers]);
    }
    // (The thread library starts every thread at the lowest user priority, 0xBF, the same as the game and the renderer, so a
    // "lower than the game's" priority cannot be made by adding to it: the game's threads are raised instead, port_thread_class.)
    if (port_vita_thread_class) {
        port_vita_thread_class(4);
    }
#else
    (void)index;
#endif
}

void workerMain(Pipeline* p, int index) {
    pinWorker(index);
    Decoder& d = p->decoders[index];
    memset(&d, 0, sizeof(d));
    std::unique_lock<std::mutex> lock(p->mu);
    for (;;) {
        p->work.wait(lock, [p] { return p->quit || p->queued > 0; });
        if (p->quit) {
            return;
        }
        // Pictures nobody will show any more are dropped from the queue; of the rest the NEWEST goes first. Oldest first
        // looks fairer but collapses when the workers are behind: every picture is stale by the time its turn comes, all
        // are dropped, and almost nothing reaches the screen. Newest first spends every decode on a picture the game
        // is about to ask for, so a slow decoder shows a lower rate of fresh pictures instead of none.
        for (int i = 0; i < p->queued;) {
            Slot& old = p->slots[p->queue[i]];
            if (old.seq + (u64)(kRing - 2) < p->calls || (p->anyShown && old.seq < p->lastShownSeq)) {
                old.state = kSkipped;
                p->skipped++;
                p->queue[i] = p->queue[--p->queued];
                p->done.notify_all();
            } else {
                i++;
            }
        }
        if (p->queued == 0) {
            continue;
        }
        int best = 0;
        for (int i = 1; i < p->queued; i++) {
            if (p->slots[p->queue[i]].seq > p->slots[p->queue[best]].seq) best = i;
        }
        const int idx = p->queue[best];
        p->queue[best] = p->queue[--p->queued];
        Slot& s = p->slots[idx];
        s.state = kBusy;
        lock.unlock();
        // The planes are sized from the picture's own header (done on first use of the slot).
        s32 status = 0;
        double decodeMsLocal = 0, decodeCpuLocal = 0;
        static const int testDelayUs = envInt("PETARI_THP_TEST_DELAY_US", 0);  // (tests: a slow decoder)
        if (testDelayUs > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(testDelayUs));
        }
        {
            // width and height come from the SOF marker, read by decodeFrame; allocate to the largest the frame says
            // by scanning for it first.
            u32 w = 0, h = 0;
            const u8* c = s.in.data();
            const u8* end = c + s.inSize;
            while (c + 9 < end) {
                if (c[0] == 0xFF && c[1] == 0xC0) {
                    h = read16(c + 5);
                    w = read16(c + 7);
                    break;
                }
                c++;
            }
            if (w && h) {
                if (w != s.width || h != s.height || s.y.empty()) {
                    s.width = w;
                    s.height = h;
                    s.y.assign(lumaBytes(w, h), 0x10);
                    s.u.assign(chromaBytes(w, h), 0x80);
                    s.v.assign(chromaBytes(w, h), 0x80);
                }
                const auto d0 = std::chrono::steady_clock::now();
                const int64_t c0 = port_thread_cpu_us ? port_thread_cpu_us() : 0;
                status = decodeFrame(d, s.in.data(), s.inSize, s.y.data(), s.u.data(), s.v.data());
                decodeMsLocal = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - d0).count();
                if (port_thread_cpu_us) decodeCpuLocal = (double)(port_thread_cpu_us() - c0) / 1000.0;
            } else {
                status = kErrBadSyntax;
            }
        }
        lock.lock();
        p->decoded++;
        p->decodeMs += decodeMsLocal;
        p->decodeCpuMs += decodeCpuLocal;
        s.status = status;
        s.state = status == 0 ? kDone : kSkipped;
        p->done.notify_all();
    }
}

void initPipeline() {
    {
#if defined(__vita__) && !defined(THP_STANDALONE)
        const int defaultWorkers = 3;
#else
        const int defaultWorkers = 0;
#endif
        int n = envInt("PETARI_THP_WORKERS", defaultWorkers);
        if (n <= 0) {
            return;
        }
        if (n > kMaxWorkers) n = kMaxWorkers;
#ifndef THP_STANDALONE
        PortHostAllocScope hostHeap;  // (this runs on a game thread: the pipeline's memory is the host's, not a JKR heap's)
#endif
        Pipeline* p = new Pipeline();
        p->workers = n;
        for (int i = 0; i < kRing; i++) {
            p->slots[i].in.assign(kFrameCap + 64, 0);
        }
        p->lastCall = std::chrono::steady_clock::now();
        for (int i = 0; i < n; i++) {
            p->threads[i] = std::thread(workerMain, p, i);
        }
        if (port_log) port_log("THP: %d decoder thread(s)", n);
        gPipe = p;
    }
}

// (not std::call_once: the armhf test build links libstdc++'s emulated thread-local once state badly)
Pipeline* pipeline() {
    std::lock_guard<std::mutex> lock(gPipeInitMu);
    if (!gPipeInitDone) {
        gPipeInitDone = true;
        initPipeline();
    }
    return gPipe;
}

s32 decodePipelined(Pipeline* p, const void* file, void* tileY, void* tileU, void* tileV) {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    std::unique_lock<std::mutex> lock(p->mu);

    // A pause of more than a quarter second means a new movie (or a seek): start over, discarding what is in flight.
    if (std::chrono::duration<double>(now - p->lastCall).count() > 0.25) {
        for (;;) {
            bool busy = false;
            for (int i = 0; i < kRing; i++) busy |= p->slots[i].state == kBusy;
            if (!busy) break;
            p->done.wait_for(lock, std::chrono::milliseconds(20));
        }
        p->queued = 0;
        for (int i = 0; i < kRing; i++) p->slots[i].state = kFree;
        p->base = p->calls;
        p->anyShown = false;
    }
    p->lastCall = now;

    const u64 seq = p->calls++;
    Slot& s = p->slots[seq % kRing];
    if (s.state == kBusy) {
        // A worker is still on the picture of kRing turns ago (a long preemption): this turn's picture is not decoded.
        p->dropped++;
    } else {
        if (s.state == kQueued) {  // an old job nobody has started: cancel it
            for (int i = 0; i < p->queued; i++) {
                if (p->queue[i] == (int)(seq % kRing)) {
                    p->queue[i] = p->queue[--p->queued];
                    break;
                }
            }
        }
        u32 size = gFrameSizeHint ? gFrameSizeHint : kFrameCap;
        if (size > kFrameCap) size = kFrameCap;
        memcpy(s.in.data(), file, size);
        memset(s.in.data() + size, 0, 64);
        s.inSize = size;
        s.seq = seq;
        s.state = kQueued;
        p->queue[p->queued++] = (int)(seq % kRing);
        p->work.notify_one();
    }

    // The picture to show: the next one in order after the last one shown, if it is finished and not too old (a picture
    // that finished together with a newer one is not lost: every picture the decoder makes is shown, in order, while it
    // stays within kMaxLag turns); otherwise the newest finished one (the decoder fell behind: jump ahead); with nothing
    // new, the last one again.
    constexpr u64 kMaxLag = 4;
    auto choose = [&]() {
        int oldestNew = -1, newest = -1, same = -1;
        for (int i = 0; i < kRing; i++) {
            const Slot& t = p->slots[i];
            if (t.state != kDone || t.seq < p->base || t.seq > seq) continue;
            if (newest < 0 || t.seq > p->slots[newest].seq) newest = i;
            if (p->anyShown && t.seq == p->lastShownSeq) same = i;
            if ((!p->anyShown || t.seq > p->lastShownSeq) && (oldestNew < 0 || t.seq < p->slots[oldestNew].seq)) oldestNew = i;
        }
        if (oldestNew >= 0) return seq - p->slots[oldestNew].seq <= kMaxLag ? oldestNew : newest;
        return same >= 0 ? same : newest;
    };
    int use = choose();
    if (use < 0) {
        // Nothing finished yet (the first call of a movie): wait for the first picture.
        const auto t0 = Clock::now();
        p->done.wait_for(lock, std::chrono::milliseconds(500), [&] { return choose() >= 0; });
        p->waits++;
        p->waitMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        use = choose();
    }
    if (use < 0) {
        return kErrNoInput;  // the player retries on its next turn
    }
    const Slot& src = p->slots[use];
    if (p->anyShown && src.seq == p->lastShownSeq) {
        p->repeated++;
    } else {
        p->fresh++;
        p->lagSum += seq - src.seq;
        p->lastShownSeq = src.seq;
        p->anyShown = true;
    }
    p->shown++;
    lock.unlock();
    memcpy(tileY, src.y.data(), src.y.size());
    memcpy(tileU, src.u.data(), src.u.size());
    memcpy(tileV, src.v.data(), src.v.size());
    port_gpu_memory_written(tileY, (u32)src.y.size());
    port_gpu_memory_written(tileU, (u32)src.u.size());
    port_gpu_memory_written(tileV, (u32)src.v.size());
    if ((p->shown % 600) == 0 && port_log) {
        port_log("THP: %llu turns, %llu new pictures (on average %.1f turns old), %llu shown again (decoder behind), %llu not decoded in time, "
                 "%llu decoded (%.2f ms each, %.2f ms of processor time), %llu not submitted",
                 (unsigned long long)p->shown, (unsigned long long)p->fresh, p->fresh ? (double)p->lagSum / (double)p->fresh : 0.0,
                 (unsigned long long)p->repeated, (unsigned long long)p->skipped, (unsigned long long)p->decoded,
                 p->decoded ? p->decodeMs / (double)p->decoded : 0.0, p->decoded ? p->decodeCpuMs / (double)p->decoded : 0.0,
                 (unsigned long long)p->dropped);
    }
    return 0;
}

}  // namespace

extern "C" {

BOOL THPInit(void) { return TRUE; }

// The size of the video component about to be decoded (the movie player knows it; the SDK's decoder did not need it).
void port_thp_frame_size(u32 size) { gFrameSizeHint = size; }

// `file` is one frame's video component.  The THP player passes the size of
// the whole frame buffer only implicitly, so without a size hint the scan is bounded by the
// largest THP frame the SDK accepts.
s32 THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work) {
    if (!file) {
        return kErrNoInput;
    }
    if (!tileY || !tileU || !tileV) {
        return kErrNoOutput;
    }
    if (!work) {
        return kErrNoWork;
    }
    if (Pipeline* p = pipeline()) {
        return decodePipelined(p, file, tileY, tileU, tileV);
    }
    const u32 limit = gFrameSizeHint ? gFrameSizeHint : (4u << 20);
    s32 status = decodeFrame(sDec, file, limit, (u8*)tileY, (u8*)tileU, (u8*)tileV);
    if (status == 0) {
        // The three planes are textures the movie player draws with: tell the renderer they changed
        // (I8 tiles: the luma plane width x height, the chroma planes a quarter of that each).
        port_gpu_memory_written(tileY, lumaBytes(sDec.width, sDec.height));
        port_gpu_memory_written(tileU, chromaBytes(sDec.width, sDec.height));
        port_gpu_memory_written(tileV, chromaBytes(sDec.width, sDec.height));
    }
    return status;
}


// Console/harness benchmark of the pipeline (PETARI_THP_BENCH=<path of a cooked .thp>): plays the first
// PETARI_THP_BENCH_FRAMES (default 900) pictures through THPVideoDecode at PETARI_THP_BENCH_HZ (default
// 59.94), the way the movie player calls it, and says how many of them came late.  Returns the number of
// pictures that were repeated because the decoder was behind.
extern "C" int port_thp_bench(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        if (port_log) port_log("THP bench: cannot open %s", path);
        return -1;
    }
    u8 head[96];
    if (fread(head, 1, 96, f) != 96 || memcmp(head, "THP", 3) != 0) {
        fclose(f);
        if (port_log) port_log("THP bench: %s is not a cooked THP file", path);
        return -1;
    }
    auto rd32 = [](const u8* q) { return (u32)q[3] << 24 | (u32)q[2] << 16 | (u32)q[1] << 8 | q[0]; };
    const u32 numFrames = rd32(head + 0x14);
    u32 cur = rd32(head + 0x18);
    u32 pos = rd32(head + 0x28);
    int frames = envInt("PETARI_THP_BENCH_FRAMES", 900);
    if ((u32)frames > numFrames) frames = (int)numFrames;
    double hz = 59.94;
    if (const char* e = getenv("PETARI_THP_BENCH_HZ")) {
        if (atof(e) > 1.0) hz = atof(e);
    }
    std::vector<u8> buf(256 * 1024), y(640 * 368), u(320 * 184), v(320 * 184), work(4096);
    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now();
    const auto t0 = next;
    double worstMs = 0, sumMs = 0;
    int errors = 0;
    for (int i = 0; i < frames; i++) {
        fseek(f, (long)pos, SEEK_SET);
        if (cur > buf.size() || fread(buf.data(), 1, cur, f) != cur) break;
        const u32 nextSize = rd32(buf.data());
        const u32 vsz = rd32(buf.data() + 8);
        port_thp_frame_size(vsz);
        const auto c0 = Clock::now();
        const s32 st = THPVideoDecode(buf.data() + 16, y.data(), u.data(), v.data(), work.data());
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - c0).count();
        if (st != 0) errors++;
        sumMs += ms;
        if (ms > worstMs) worstMs = ms;
        pos += cur;
        cur = nextSize;
        next += period;
        std::this_thread::sleep_until(next);
    }
    fclose(f);
    const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
    int repeated = -1;
    if (gPipe) {
        std::lock_guard<std::mutex> lock(gPipe->mu);
        repeated = (int)gPipe->repeated;
        if (port_log)
            port_log("THP bench: %d turns in %.2f s (%.1f per second asked): %llu new pictures (on average %.1f turns old), %llu shown again, %llu "
                     "not decoded in time, %llu decoded (%.2f ms each, so %.1f pictures a second), %llu not submitted; THPVideoDecode took %.2f ms on "
                     "average, %.1f ms at worst, waited %.1f ms in all; %d errors",
                     frames, wall, hz, (unsigned long long)gPipe->fresh, gPipe->fresh ? (double)gPipe->lagSum / (double)gPipe->fresh : 0.0,
                     (unsigned long long)gPipe->repeated, (unsigned long long)gPipe->skipped, (unsigned long long)gPipe->decoded,
                     gPipe->decoded ? gPipe->decodeMs / (double)gPipe->decoded : 0.0, (double)gPipe->decoded / wall,
                     (unsigned long long)gPipe->dropped, sumMs / (frames ? frames : 1), worstMs, gPipe->waitMs, errors);
    } else if (port_log) {
        port_log("THP bench: %d pictures in %.2f s (%.1f per second asked) decoded inline: %.2f ms each on average, %.1f ms at worst; %d errors",
                 frames, wall, hz, sumMs / (frames ? frames : 1), worstMs, errors);
    }
    return repeated;
}

}  // extern "C"
