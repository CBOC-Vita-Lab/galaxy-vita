// Galaxy 3.0 multiplayer: the registration proof of work of the online service (server/src/core/pow.js, SMG-PROTOCOL.txt "Sign-up").
//
// Find a nonce (1..20 characters of [0-9a-z]) such that SHA-256(challenge ":" nonce) starts with `bits` zero bits. The solver runs in
// slices (Step) so the caller decides how much time one call may take: the menu calls it once per frame with a few milliseconds.
// SHA-256 is written out here (FIPS 180-4), scalar C++, no tables but the 64 round constants; one 64-byte block per try for every
// challenge the service issues (the message is at most 55 bytes: 13 + 1 + 16 + 1 + 2 + 1 + 16 + 1 + up to 8 nonce characters).
#ifndef MP_POW_H
#define MP_POW_H

#include <stdint.h>
#include <string.h>

namespace mp_pow {

// The most bits this client will work on: 2^17 tries on average. ESTIMATE, not measured on a console: scalar SHA-256 here costs about
// 2,000-3,000 cycles a try (this code on a 2020s x86 VM core: 1.0 million tries/s); a Vita core at 444 MHz is then about 150-200 thousand tries/s,
// and the menu gives the solver 6 ms of every 16 ms frame (mp_service.cpp kSolveBudgetMs), so roughly 60-75 thousand tries/s in effect. That makes
// 15 bits about 0.5 s, 17 bits about 2 s (95th percentile about 6 s), 18 bits about 4 s (95th about 12 s), 20 bits about 15 s. A player must never wait
// more than about 10 s: above this limit the caller shows the "online play is busy" line instead of grinding (svcfix2; it was 22 = minutes).
static const int kMaxBits = 17;

inline uint32_t Ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline void Sha256Block(uint32_t h[8], const uint8_t blk[64]) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(blk[4 * i]) << 24 | uint32_t(blk[4 * i + 1]) << 16 | uint32_t(blk[4 * i + 2]) << 8 | uint32_t(blk[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = hh + (Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        const uint32_t t2 = (Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

// SHA-256 of any message (out = 32 bytes, big-endian words).
inline void Sha256(uint8_t out[32], const void* data, size_t len) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t left = len;
    while (left >= 64) { Sha256Block(h, p); p += 64; left -= 64; }
    uint8_t blk[128];
    memset(blk, 0, sizeof blk);
    memcpy(blk, p, left);
    blk[left] = 0x80;
    const size_t total = left + 1 + 8 <= 64 ? 64 : 128;
    const uint64_t bitsLen = uint64_t(len) * 8;
    for (int i = 0; i < 8; ++i) blk[total - 1 - i] = uint8_t(bitsLen >> (8 * i));
    Sha256Block(h, blk);
    if (total == 128) Sha256Block(h, blk + 64);
    for (int i = 0; i < 8; ++i) { out[4 * i] = uint8_t(h[i] >> 24); out[4 * i + 1] = uint8_t(h[i] >> 16); out[4 * i + 2] = uint8_t(h[i] >> 8); out[4 * i + 3] = uint8_t(h[i]); }
}

inline int LeadingZeroBits(const uint8_t* d, int n) {
    int z = 0;
    for (int i = 0; i < n; ++i) {
        if (!d[i]) { z += 8; continue; }
        for (int b = 7; b >= 0 && !((d[i] >> b) & 1); --b) ++z;
        break;
    }
    return z;
}

// A solver for one challenge. Start(), then Step() until it returns 1 (nonce() is the answer) or -1 (gave up: limit reached).
struct Solver {
    char msg[128];
    size_t prefix = 0;      // strlen(challenge) + 1 (the ':')
    int bits = 0;
    uint64_t next = 0, limit = 0;
    char nonce[24] = {};

    // false when the challenge is unusable (empty, too long, or a difficulty this solver will not attempt)
    bool Start(const char* challenge, int wantBits, uint64_t maxTries = 1ull << 28) {
        const size_t n = challenge ? strlen(challenge) : 0;
        if (!n || n > 80 || wantBits < 0 || wantBits > kMaxBits) return false;
        memcpy(msg, challenge, n);
        msg[n] = ':';
        prefix = n + 1;
        bits = wantBits;
        next = 0;
        limit = maxTries;
        nonce[0] = 0;
        return true;
    }
    // Tries at most `tries` nonces. 1 found, 0 not yet, -1 gave up.
    int Step(uint32_t tries) {
        static const char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        for (uint32_t t = 0; t < tries; ++t) {
            if (next >= limit) return -1;
            // base 36, the same spelling as the service's reference solver (i.toString(36))
            char tmp[24];
            int k = 0;
            uint64_t v = next++;
            do { tmp[k++] = kDigits[v % 36]; v /= 36; } while (v && k < 20);
            size_t len = prefix;
            while (k) msg[len++] = tmp[--k];
            uint8_t d[32];
            Sha256(d, msg, len);
            if (LeadingZeroBits(d, 32) >= bits) {
                memcpy(nonce, msg + prefix, len - prefix);
                nonce[len - prefix] = 0;
                return 1;
            }
        }
        return 0;
    }
};

}  // namespace mp_pow

#endif
