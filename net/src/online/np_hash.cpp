// MD5 (RFC 1321), SHA-1 (RFC 3174) and HMAC (RFC 2104) written for this project from the RFCs.
//
// Public domain notice: the author dedicates this file to the public domain (CC0 1.0). No warranty. Both hashes are broken as security
// primitives and are used here only because the TURN protocol (RFC 8656 / RFC 5389) mandates them for long-term credentials.
#include "np_hash.h"

#include <string.h>

namespace NpNet {
namespace {

inline uint32_t Rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

// ---- MD5 -------------------------------------------------------------------------------------------------------------------
const uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122,
    0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6,
    0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60,
    0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
const uint8_t kMd5S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                           4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

void Md5Block(uint32_t st[4], const uint8_t* p) {
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) m[i] = uint32_t(p[4 * i]) | uint32_t(p[4 * i + 1]) << 8 | uint32_t(p[4 * i + 2]) << 16 | uint32_t(p[4 * i + 3]) << 24;
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        const uint32_t t = d;
        d = c;
        c = b;
        b = b + Rol(a + f + kMd5K[i] + m[g], kMd5S[i]);
        a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

// ---- SHA-1 -----------------------------------------------------------------------------------------------------------------
void Sha1Block(uint32_t st[5], const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | uint32_t(p[4 * i + 3]);
    for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        const uint32_t t = Rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = Rol(b, 30); b = a; a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}

// Merkle-Damgard padding shared by both: message, 0x80, zeros, 64-bit bit length (little endian for MD5, big endian for SHA-1).
template <typename Block>
void Pad(uint32_t* st, const uint8_t* data, size_t len, bool bigEndianLength, Block block) {
    size_t i = 0;
    for (; i + 64 <= len; i += 64) block(st, data + i);
    uint8_t tail[128];
    const size_t rest = len - i;
    memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    const size_t total = rest + 1 + 8 <= 64 ? 64 : 128;
    memset(tail + rest + 1, 0, total - rest - 1);
    const uint64_t bits = uint64_t(len) * 8u;
    for (int k = 0; k < 8; ++k) tail[total - 8 + k] = uint8_t(bits >> (bigEndianLength ? 56 - 8 * k : 8 * k));
    for (size_t o = 0; o < total; o += 64) block(st, tail + o);
}

}  // namespace

void Md5(uint8_t out[16], const void* data, size_t len) {
    uint32_t st[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    Pad(st, static_cast<const uint8_t*>(data ? data : out), data ? len : 0, false, Md5Block);
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k) out[4 * i + k] = uint8_t(st[i] >> (8 * k));
}

void Sha1(uint8_t out[20], const void* data, size_t len) {
    uint32_t st[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    Pad(st, static_cast<const uint8_t*>(data ? data : out), data ? len : 0, true, Sha1Block);
    for (int i = 0; i < 5; ++i)
        for (int k = 0; k < 4; ++k) out[4 * i + k] = uint8_t(st[i] >> (24 - 8 * k));
}

void HmacSha1(uint8_t out[20], const void* key, size_t keyLen, const void* msg, size_t msgLen) {
    uint8_t k[64] = {0};
    if (keyLen > 64) Sha1(k, key, keyLen);
    else if (keyLen) memcpy(k, key, keyLen);
    // inner = SHA1((k ^ ipad) || msg): the message is copied next to the pad (messages here are one datagram)
    uint8_t buf[64 + 1500];
    uint8_t inner[20];
    uint8_t outer[64 + 20];
    if (msgLen > 1500) msgLen = 1500;
    for (int i = 0; i < 64; ++i) buf[i] = k[i] ^ 0x36;
    if (msgLen) memcpy(buf + 64, msg, msgLen);
    Sha1(inner, buf, 64 + msgLen);
    for (int i = 0; i < 64; ++i) outer[i] = k[i] ^ 0x5c;
    memcpy(outer + 64, inner, 20);
    Sha1(out, outer, sizeof outer);
}

}  // namespace NpNet
