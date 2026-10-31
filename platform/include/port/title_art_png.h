#pragma once
// A small, dependency-free path from RGB pixels to an 8-bit palette PNG: a median-cut quantizer with Floyd-Steinberg
// dithering, and a PNG writer with its own deflate (LZ77 over a hash chain, dynamic Huffman blocks, stored blocks when
// those are smaller). The console's tools only accept palette PNGs for the bubble and LiveArea pictures, and the console
// SDK on the build machines has no zlib, so nothing here links against one.
//
// Memory: every buffer comes from malloc (never operator new, which is fatal on the console when the heap is short) and
// every function returns false instead of aborting when malloc says no. Peak use for a 960x544 picture is about 4 MiB.
// The reference of the colour handling is tools/handheld/make_livearea_title.py (Pillow's median cut).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace port::titleart {

// ------------------------------------------------------------------------------------------------ memory
template <class T> struct Mem {
    T* p = nullptr;
    size_t n = 0;
    Mem() = default;
    Mem(const Mem&) = delete;
    Mem& operator=(const Mem&) = delete;
    ~Mem() { std::free(p); }
    bool Alloc(size_t count) {  // uninitialised
        std::free(p);
        p = nullptr;
        n = 0;
        if (!count || count > SIZE_MAX / sizeof(T)) return false;
        p = static_cast<T*>(std::malloc(count * sizeof(T)));
        if (!p) return false;
        n = count;
        return true;
    }
    bool AllocZero(size_t count) {
        if (!Alloc(count)) return false;
        std::memset(p, 0, count * sizeof(T));
        return true;
    }
    T& operator[](size_t i) { return p[i]; }
    const T& operator[](size_t i) const { return p[i]; }
};

// ------------------------------------------------------------------------------------------------ quantizer
struct Palette {
    uint8_t rgb[256 * 3];
    int count = 0;
};

namespace detail {
struct Bin {
    uint32_t key;    // 6 bits per channel: r<<12 | g<<6 | b
    uint32_t count;
};
inline int KeyR(uint32_t k) { return int((k >> 12) & 63u); }
inline int KeyG(uint32_t k) { return int((k >> 6) & 63u); }
inline int KeyB(uint32_t k) { return int(k & 63u); }
inline uint32_t MakeKey(int r, int g, int b) { return (uint32_t(r >> 2) << 12) | (uint32_t(g >> 2) << 6) | uint32_t(b >> 2); }
// Weights of the channels in the colour distance (and in choosing the axis to split): the eye is least sensitive to blue.
constexpr int kWr = 3, kWg = 4, kWb = 2;
struct Box {
    uint32_t begin, end;  // range of bins
};
}  // namespace detail

// Builds a palette of at most `maxColours` (<= 256) from `rgb` (w*h*3 bytes). False only when memory is short.
inline bool BuildPalette(const uint8_t* rgb, size_t pixels, Palette& pal, uint32_t* table /* 262144 entries, scratch */, int maxColours = 256) {
    using namespace detail;
    std::memset(table, 0, sizeof(uint32_t) * 262144u);
    for (size_t i = 0; i < pixels; ++i) ++table[MakeKey(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2])];
    size_t distinct = 0;
    for (uint32_t k = 0; k < 262144u; ++k) distinct += table[k] != 0;
    if (!distinct) return false;
    Mem<Bin> bins;
    if (!bins.Alloc(distinct)) return false;
    size_t n = 0;
    for (uint32_t k = 0; k < 262144u; ++k)
        if (table[k]) bins[n++] = {k, table[k]};
    Box boxes[256];
    int boxCount = 1;
    boxes[0] = {0, uint32_t(n)};
    if (maxColours > 256) maxColours = 256;
    if (maxColours < 1) maxColours = 1;
    auto stats = [&](const Box& b, int& axis, int& range, uint64_t& total) {
        int lo[3] = {63, 63, 63}, hi[3] = {0, 0, 0};
        total = 0;
        for (uint32_t i = b.begin; i < b.end; ++i) {
            const int c[3] = {KeyR(bins[i].key), KeyG(bins[i].key), KeyB(bins[i].key)};
            for (int j = 0; j < 3; ++j) { lo[j] = std::min(lo[j], c[j]); hi[j] = std::max(hi[j], c[j]); }
            total += bins[i].count;
        }
        const int w[3] = {kWr, kWg, kWb};
        axis = 0;
        range = -1;
        for (int j = 0; j < 3; ++j)
            if ((hi[j] - lo[j]) * w[j] > range) { range = (hi[j] - lo[j]) * w[j]; axis = j; }
    };
    while (boxCount < maxColours) {
        // The box to split: the largest weighted spread times the square root of its pixel count (a pure population rule starves
        // the small, saturated colours of a logo; a pure spread rule spends the palette on single stray pixels).
        int best = -1, bestAxis = 0;
        double bestScore = 0;
        for (int i = 0; i < boxCount; ++i) {
            if (boxes[i].end - boxes[i].begin < 2) continue;
            int axis, range;
            uint64_t total;
            stats(boxes[i], axis, range, total);
            if (range <= 0) continue;
            double score = double(range) * std::sqrt(double(total));
            if (score > bestScore) { bestScore = score; best = i; bestAxis = axis; }
        }
        if (best < 0) break;
        Box b = boxes[best];
        auto channel = [&](const Bin& x) { return bestAxis == 0 ? KeyR(x.key) : (bestAxis == 1 ? KeyG(x.key) : KeyB(x.key)); };
        std::sort(bins.p + b.begin, bins.p + b.end, [&](const Bin& x, const Bin& y) {
            const int cx = channel(x), cy = channel(y);
            return cx != cy ? cx < cy : x.key < y.key;
        });
        uint64_t total = 0;
        for (uint32_t i = b.begin; i < b.end; ++i) total += bins[i].count;
        uint64_t acc = 0;
        uint32_t split = b.begin + 1;
        for (uint32_t i = b.begin; i + 1 < b.end; ++i) {
            acc += bins[i].count;
            split = i + 1;
            if (acc * 2 >= total) break;
        }
        boxes[best] = {b.begin, split};
        boxes[boxCount++] = {split, b.end};
    }
    pal.count = boxCount;
    for (int i = 0; i < boxCount; ++i) {
        uint64_t sum[3] = {0, 0, 0}, total = 0;
        for (uint32_t j = boxes[i].begin; j < boxes[i].end; ++j) {
            const uint64_t c = bins[j].count;
            sum[0] += c * ((uint32_t(KeyR(bins[j].key)) << 2) | 2u);
            sum[1] += c * ((uint32_t(KeyG(bins[j].key)) << 2) | 2u);
            sum[2] += c * ((uint32_t(KeyB(bins[j].key)) << 2) | 2u);
            total += c;
        }
        for (int c = 0; c < 3; ++c) pal.rgb[i * 3 + c] = uint8_t(std::min<uint64_t>(255, (sum[c] + total / 2) / total));
    }
    return true;
}

namespace detail {
// The nearest palette entry to a colour, remembered per 6-bit cell (262144 entries of scratch).
struct Nearest {
    const Palette& pal;
    uint32_t* table;
    Nearest(const Palette& p, uint32_t* t) : pal(p), table(t) { std::memset(table, 0xFF, sizeof(uint32_t) * 262144u); }
    uint8_t operator()(int r, int g, int b) {
        const uint32_t key = MakeKey(r, g, b);
        if (table[key] != 0xFFFFFFFFu) return uint8_t(table[key]);
        const int cr = KeyR(key) * 4 + 2, cg = KeyG(key) * 4 + 2, cb = KeyB(key) * 4 + 2;
        int bestI = 0;
        int bestD = 0x7fffffff;
        for (int i = 0; i < pal.count; ++i) {
            const int dr = cr - pal.rgb[i * 3], dg = cg - pal.rgb[i * 3 + 1], db = cb - pal.rgb[i * 3 + 2];
            const int d = kWr * dr * dr + kWg * dg * dg + kWb * db * db;
            if (d < bestD) { bestD = d; bestI = i; }
        }
        table[key] = uint32_t(bestI);
        return uint8_t(bestI);
    }
};
}  // namespace detail

// One refinement step: every palette entry becomes the mean of the actual pixels nearest to it (the median cut only knew 6-bit cells).
inline void RefinePalette(const uint8_t* rgb, size_t pixels, Palette& pal, uint32_t* table) {
    detail::Nearest nearest(pal, table);
    uint64_t sum[256][3] = {};
    uint32_t count[256] = {};
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t k = nearest(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
        sum[k][0] += rgb[i * 3]; sum[k][1] += rgb[i * 3 + 1]; sum[k][2] += rgb[i * 3 + 2];
        ++count[k];
    }
    for (int k = 0; k < pal.count; ++k)
        if (count[k])
            for (int c = 0; c < 3; ++c) pal.rgb[k * 3 + c] = uint8_t((sum[k][c] + count[k] / 2) / count[k]);
}

// Maps `rgb` to palette indices, with Floyd-Steinberg error diffusion when `dither` is set. `table` (262144 entries) is scratch.
inline bool MapToPalette(const uint8_t* rgb, int w, int h, const Palette& pal, uint8_t* idx, uint32_t* table, bool dither) {
    detail::Nearest nearest(pal, table);
    if (!dither) {
        for (size_t i = 0; i < size_t(w) * h; ++i) idx[i] = nearest(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
        return true;
    }
    Mem<int> err;  // two rows of w+2 pixels, 3 channels each, in 1/16 units
    if (!err.AllocZero(size_t(2) * (w + 2) * 3)) return false;
    int* cur = err.p;
    int* next = err.p + size_t(w + 2) * 3;
    for (int y = 0; y < h; ++y) {
        std::memset(next, 0, sizeof(int) * size_t(w + 2) * 3);
        for (int x = 0; x < w; ++x) {
            const uint8_t* s = rgb + (size_t(y) * w + x) * 3;
            int v[3];
            for (int c = 0; c < 3; ++c) {
                const int e = cur[(x + 1) * 3 + c];
                v[c] = std::clamp(int(s[c]) + (e >= 0 ? (e + 8) / 16 : -((-e + 8) / 16)), 0, 255);
            }
            const uint8_t i = nearest(v[0], v[1], v[2]);
            idx[size_t(y) * w + x] = i;
            for (int c = 0; c < 3; ++c) {
                const int e = v[c] - pal.rgb[i * 3 + c];
                cur[(x + 2) * 3 + c] += e * 7;
                next[x * 3 + c] += e * 3;
                next[(x + 1) * 3 + c] += e * 5;
                next[(x + 2) * 3 + c] += e;
            }
        }
        std::swap(cur, next);
    }
    return true;
}

inline bool Quantize(const uint8_t* rgb, int w, int h, uint8_t* idx, Palette& pal, bool dither = true) {
    Mem<uint32_t> table;
    if (!table.Alloc(262144u)) return false;
    if (!BuildPalette(rgb, size_t(w) * h, pal, table.p)) return false;
    RefinePalette(rgb, size_t(w) * h, pal, table.p);
    return MapToPalette(rgb, w, h, pal, idx, table.p, dither);
}

// ------------------------------------------------------------------------------------------------ deflate
namespace detail {
inline uint32_t Crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 255u] ^ (crc >> 8);
    return ~crc;
}
inline uint32_t Adler32(const uint8_t* data, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        const size_t chunk = std::min<size_t>(n, 5552);
        for (size_t i = 0; i < chunk; ++i) { a += data[i]; b += a; }
        a %= 65521u;
        b %= 65521u;
        data += chunk;
        n -= chunk;
    }
    return (b << 16) | a;
}

struct BitWriter {
    uint8_t* out;
    size_t cap, len = 0;
    uint64_t acc = 0;
    int bits = 0;
    bool overflow = false;
    BitWriter(uint8_t* o, size_t c) : out(o), cap(c) {}
    void Put(uint32_t value, int count) {  // LSB first
        acc |= uint64_t(value) << bits;
        bits += count;
        while (bits >= 8) { Byte(uint8_t(acc)); acc >>= 8; bits -= 8; }
    }
    void Byte(uint8_t b) {
        if (len < cap) out[len++] = b;
        else overflow = true;
    }
    void Align() {
        if (bits > 0) { Byte(uint8_t(acc)); acc = 0; bits = 0; }
    }
};

inline uint32_t ReverseBits(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; ++i) { r = (r << 1) | (v & 1u); v >>= 1; }
    return r;
}

// Code lengths (at most maxBits) for the symbols with a non-zero frequency; an exact prefix code (Kraft sum 1) when at least
// two symbols are used.
inline void HuffmanLengths(const uint32_t* freq, int n, int maxBits, uint8_t* lens) {
    std::memset(lens, 0, size_t(n));
    int used[320];
    int m = 0;
    for (int i = 0; i < n; ++i)
        if (freq[i]) used[m++] = i;
    if (m == 0) return;
    if (m == 1) { lens[used[0]] = 1; return; }
    std::sort(used, used + m, [&](int a, int b) { return freq[a] != freq[b] ? freq[a] < freq[b] : a < b; });
    uint64_t weight[640];
    int parent[640];
    for (int i = 0; i < m; ++i) weight[i] = freq[used[i]];
    int leaf = 0, node = m, made = m;
    auto take = [&]() {
        if (leaf < m && (node >= made || weight[leaf] <= weight[node])) return leaf++;
        return node++;
    };
    for (int t = 0; t < m - 1; ++t) {
        const int a = take(), b = take();
        weight[made] = weight[a] + weight[b];
        parent[a] = parent[b] = made;
        ++made;
    }
    int depth[640];
    depth[made - 1] = 0;
    for (int i = made - 2; i >= 0; --i) depth[i] = depth[parent[i]] + 1;
    for (int i = 0; i < m; ++i) lens[used[i]] = uint8_t(std::min(depth[i], 255));
    // Too long: clamp, then repair the Kraft sum by lengthening short codes, then fill any slack by shortening long ones.
    bool tooLong = false;
    for (int i = 0; i < m; ++i) tooLong |= lens[used[i]] > maxBits;
    if (!tooLong) return;
    for (int i = 0; i < m; ++i) lens[used[i]] = uint8_t(std::min<int>(lens[used[i]], maxBits));
    const uint64_t full = uint64_t(1) << maxBits;
    auto kraft = [&]() {
        uint64_t k = 0;
        for (int i = 0; i < m; ++i) k += uint64_t(1) << (maxBits - lens[used[i]]);
        return k;
    };
    uint64_t k = kraft();
    while (k > full) {
        int pick = -1;
        for (int i = 0; i < m; ++i)  // the longest code that can still grow (the rarest symbols are first in `used`)
            if (lens[used[i]] < maxBits && (pick < 0 || lens[used[i]] > lens[used[pick]])) pick = i;
        if (pick < 0) break;
        k -= uint64_t(1) << (maxBits - lens[used[pick]] - 1);
        ++lens[used[pick]];
    }
    for (bool changed = true; changed && k < full;) {
        changed = false;
        for (int i = m - 1; i >= 0 && k < full; --i) {  // the commonest symbols first
            const int l = lens[used[i]];
            if (l > 1 && k + (uint64_t(1) << (maxBits - l)) <= full) { --lens[used[i]]; k += uint64_t(1) << (maxBits - l); changed = true; }
        }
    }
}

inline void CanonicalCodes(const uint8_t* lens, int n, uint16_t* codes) {
    int count[16] = {0};
    for (int i = 0; i < n; ++i) ++count[lens[i]];
    count[0] = 0;
    uint32_t next[16];
    uint32_t code = 0;
    for (int b = 1; b < 16; ++b) { code = (code + count[b - 1]) << 1; next[b] = code; }
    for (int i = 0; i < n; ++i) codes[i] = lens[i] ? uint16_t(ReverseBits(next[lens[i]]++, lens[i])) : 0;
}

constexpr int kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr int kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
inline int LengthCode(int len) {
    int c = 28;
    while (kLenBase[c] > len) --c;
    return c;
}
inline int DistCode(int dist) {
    int c = 29;
    while (kDistBase[c] > dist) --c;
    return c;
}

constexpr int kBlockTokens = 16384;
// A token: a literal byte (bits 16..31 zero) or a match (length in bits 16..31, distance minus one in bits 0..15).
inline uint32_t MakeMatch(int len, int dist) { return (uint32_t(len) << 16) | uint32_t(dist - 1); }

// Writes one block for tokens[0..count) covering `span` input bytes starting at `start`.
inline void EmitBlock(BitWriter& bw, const uint32_t* tokens, int count, const uint8_t* in, size_t start, size_t span, bool last) {
    uint32_t lfreq[286] = {0}, dfreq[30] = {0};
    for (int i = 0; i < count; ++i) {
        const uint32_t t = tokens[i];
        if (t >> 16) { ++lfreq[257 + LengthCode(int(t >> 16))]; ++dfreq[DistCode(int(t & 0xffff) + 1)]; }
        else ++lfreq[t & 255];
    }
    lfreq[256] = 1;
    uint8_t llen[286], dlen[30];
    HuffmanLengths(lfreq, 286, 15, llen);
    HuffmanLengths(dfreq, 30, 15, dlen);
    int dUsed = 0;
    for (int i = 0; i < 30; ++i) dUsed += dlen[i] != 0;
    if (dUsed == 0) dlen[0] = 1;  // a block without matches still names one distance code
    int hlit = 286, hdist = 30;
    while (hlit > 257 && !llen[hlit - 1]) --hlit;
    while (hdist > 1 && !dlen[hdist - 1]) --hdist;
    // Run-length coding of the two length tables.
    uint8_t seq[286 + 30];
    int seqLen = 0;
    for (int i = 0; i < hlit; ++i) seq[seqLen++] = llen[i];
    for (int i = 0; i < hdist; ++i) seq[seqLen++] = dlen[i];
    uint8_t sym[286 + 30], extra[286 + 30];
    int symCount = 0;
    uint32_t cfreq[19] = {0};
    for (int i = 0; i < seqLen;) {
        int run = 1;
        while (i + run < seqLen && seq[i + run] == seq[i]) ++run;
        if (seq[i] == 0 && run >= 3) {
            const int r = std::min(run, 138);
            if (r >= 11) { sym[symCount] = 18; extra[symCount++] = uint8_t(r - 11); }
            else { sym[symCount] = 17; extra[symCount++] = uint8_t(r - 3); }
            ++cfreq[sym[symCount - 1]];
            i += r;
        } else if (seq[i] != 0 && run >= 4) {
            sym[symCount] = seq[i]; extra[symCount++] = 0; ++cfreq[seq[i]];
            int r = std::min(run - 1, 6);
            sym[symCount] = 16; extra[symCount++] = uint8_t(r - 3); ++cfreq[16];
            i += 1 + r;
        } else {
            sym[symCount] = seq[i]; extra[symCount++] = 0; ++cfreq[seq[i]];
            ++i;
        }
    }
    uint8_t clen[19];
    HuffmanLengths(cfreq, 19, 7, clen);
    {
        int used = 0, only = 0;
        for (int i = 0; i < 19; ++i)
            if (clen[i]) { ++used; only = i; }
        if (used == 1) clen[only == 0 ? 1 : 0] = 1;  // the code-length code must be complete: two codes of one bit
    }
    static const int order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int hclen = 19;
    while (hclen > 4 && !clen[order[hclen - 1]]) --hclen;
    // Cost in bits, to compare with a stored block.
    uint64_t bits = 3 + 14 + 3u * hclen;
    for (int i = 0; i < 19; ++i) bits += uint64_t(cfreq[i]) * (clen[i] + (i == 16 ? 2 : i == 17 ? 3 : i == 18 ? 7 : 0));
    for (int i = 0; i < 286; ++i) bits += uint64_t(lfreq[i]) * llen[i] + (i >= 257 ? uint64_t(lfreq[i]) * kLenExtra[i - 257] : 0);
    for (int i = 0; i < 30; ++i) bits += uint64_t(dfreq[i]) * (dlen[i] + kDistExtra[i]);
    const uint64_t storedBits = uint64_t(span) * 8 + 40u * ((span + 65534) / 65535) + 8;
    if (span && storedBits <= bits) {
        size_t done = 0;
        while (done < span) {
            const size_t chunk = std::min<size_t>(65535, span - done);
            const bool finalPiece = last && done + chunk == span;
            bw.Put(finalPiece ? 1u : 0u, 1);
            bw.Put(0, 2);
            bw.Align();
            bw.Byte(uint8_t(chunk)); bw.Byte(uint8_t(chunk >> 8));
            bw.Byte(uint8_t(~chunk)); bw.Byte(uint8_t((~chunk) >> 8));
            for (size_t i = 0; i < chunk; ++i) bw.Byte(in[start + done + i]);
            done += chunk;
        }
        return;
    }
    uint16_t lcode[286], dcode[30], ccode[19];
    CanonicalCodes(llen, 286, lcode);
    CanonicalCodes(dlen, 30, dcode);
    CanonicalCodes(clen, 19, ccode);
    bw.Put(last ? 1u : 0u, 1);
    bw.Put(2, 2);
    bw.Put(uint32_t(hlit - 257), 5);
    bw.Put(uint32_t(hdist - 1), 5);
    bw.Put(uint32_t(hclen - 4), 4);
    for (int i = 0; i < hclen; ++i) bw.Put(clen[order[i]], 3);
    for (int i = 0; i < symCount; ++i) {
        bw.Put(ccode[sym[i]], clen[sym[i]]);
        if (sym[i] == 16) bw.Put(extra[i], 2);
        else if (sym[i] == 17) bw.Put(extra[i], 3);
        else if (sym[i] == 18) bw.Put(extra[i], 7);
    }
    for (int i = 0; i < count; ++i) {
        const uint32_t t = tokens[i];
        if (t >> 16) {
            const int len = int(t >> 16), dist = int(t & 0xffff) + 1;
            const int lc = LengthCode(len), dc = DistCode(dist);
            bw.Put(lcode[257 + lc], llen[257 + lc]);
            if (kLenExtra[lc]) bw.Put(uint32_t(len - kLenBase[lc]), kLenExtra[lc]);
            bw.Put(dcode[dc], dlen[dc]);
            if (kDistExtra[dc]) bw.Put(uint32_t(dist - kDistBase[dc]), kDistExtra[dc]);
        } else {
            bw.Put(lcode[t & 255], llen[t & 255]);
        }
    }
    bw.Put(lcode[256], llen[256]);
}
}  // namespace detail

// Largest output of ZlibCompress for n input bytes.
inline size_t ZlibBound(size_t n) { return n + 5 * (n / 65535 + 2) + 16 + n / 8; }

// A zlib stream (RFC 1950) of `in`, written to `out` (capacity `cap`, at least ZlibBound(n)).
inline bool ZlibCompress(const uint8_t* in, size_t n, uint8_t* out, size_t cap, size_t& outLen) {
    using namespace detail;
    constexpr int kHash = 15, kWindow = 32768, kMaxChain = 32, kNice = 128;
    Mem<int32_t> head, prev;
    Mem<uint32_t> tokens;
    if (!head.Alloc(size_t(1) << kHash) || !prev.Alloc(kWindow) || !tokens.Alloc(kBlockTokens)) return false;
    std::fill(head.p, head.p + head.n, -1);
    BitWriter bw(out, cap);
    bw.Byte(0x78);
    bw.Byte(0x9C);
    auto hashAt = [&](size_t i) { return (uint32_t((in[i] | (in[i + 1] << 8) | (in[i + 2] << 16))) * 0x9E3779B1u) >> (32 - kHash); };
    auto insert = [&](size_t i) {
        if (i + 2 >= n) return;
        const uint32_t h = hashAt(i);
        prev[i & (kWindow - 1)] = head[h];
        head[h] = int32_t(i);
    };
    size_t pos = 0, blockStart = 0;
    int count = 0;
    if (n == 0) EmitBlock(bw, tokens.p, 0, in, 0, 0, true);
    while (pos < n) {
        int bestLen = 0, bestDist = 0;
        if (pos + 2 < n) {
            const uint32_t h = hashAt(pos);
            int32_t cand = head[h];
            const size_t maxLen = std::min<size_t>(258, n - pos);
            for (int chain = 0; chain < kMaxChain && cand >= 0; ++chain) {
                const size_t dist = pos - size_t(cand);
                if (dist > size_t(kWindow) || dist == 0) break;
                size_t l = 0;
                while (l < maxLen && in[size_t(cand) + l] == in[pos + l]) ++l;
                if (int(l) > bestLen) {
                    bestLen = int(l);
                    bestDist = int(dist);
                    if (l >= size_t(kNice) || l == maxLen) break;
                }
                const int32_t next = prev[size_t(cand) & (kWindow - 1)];
                if (next >= cand) break;  // (the chain can only go backwards; a stale slot ends it)
                cand = next;
            }
        }
        if (bestLen >= 3) {
            tokens[count++] = MakeMatch(bestLen, bestDist);
            for (int k = 0; k < bestLen; ++k) insert(pos + k);
            pos += bestLen;
        } else {
            tokens[count++] = in[pos];
            insert(pos);
            ++pos;
        }
        if (count == kBlockTokens || pos == n) {
            EmitBlock(bw, tokens.p, count, in, blockStart, pos - blockStart, pos == n);
            blockStart = pos;
            count = 0;
        }
    }
    bw.Align();
    const uint32_t adler = Adler32(in, n);
    for (int s = 24; s >= 0; s -= 8) bw.Byte(uint8_t(adler >> s));
    if (bw.overflow) return false;
    outLen = bw.len;
    return true;
}

// ------------------------------------------------------------------------------------------------ PNG
// An 8-bit palette PNG of `idx` (w*h palette indices). `out` receives the file; its capacity must be at least PngBound(w, h).
inline size_t PngBound(int w, int h) { return ZlibBound(size_t(w + 1) * h) + 768 + 128; }
inline bool EncodePng8(const uint8_t* idx, int w, int h, const Palette& pal, uint8_t* out, size_t cap, size_t& outLen) {
    using namespace detail;
    if (w <= 0 || h <= 0 || pal.count < 1 || cap < PngBound(w, h)) return false;
    const size_t rawLen = size_t(w + 1) * h;
    Mem<uint8_t> raw, packed;
    if (!raw.Alloc(rawLen) || !packed.Alloc(ZlibBound(rawLen))) return false;
    for (int y = 0; y < h; ++y) {
        raw[size_t(y) * (w + 1)] = 0;  // filter "none": palette indices gain nothing from the others
        std::memcpy(raw.p + size_t(y) * (w + 1) + 1, idx + size_t(y) * w, size_t(w));
    }
    size_t packedLen = 0;
    if (!ZlibCompress(raw.p, rawLen, packed.p, packed.n, packedLen)) return false;
    size_t o = 0;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::memcpy(out, sig, 8);
    o = 8;
    auto chunk = [&](const char* type, const uint8_t* data, size_t len) {
        out[o++] = uint8_t(len >> 24); out[o++] = uint8_t(len >> 16); out[o++] = uint8_t(len >> 8); out[o++] = uint8_t(len);
        const size_t typeAt = o;
        std::memcpy(out + o, type, 4);
        o += 4;
        if (len) { std::memcpy(out + o, data, len); o += len; }
        const uint32_t crc = Crc32(out + typeAt, 4 + len);
        out[o++] = uint8_t(crc >> 24); out[o++] = uint8_t(crc >> 16); out[o++] = uint8_t(crc >> 8); out[o++] = uint8_t(crc);
    };
    uint8_t ihdr[13] = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w), uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 3, 0, 0, 0};
    chunk("IHDR", ihdr, 13);
    chunk("PLTE", pal.rgb, size_t(pal.count) * 3);
    chunk("IDAT", packed.p, packedLen);
    chunk("IEND", nullptr, 0);
    outLen = o;
    return true;
}

}  // namespace port::titleart
