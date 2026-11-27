// Galaxy 3.0 multiplayer (port side, owner: core): SipHash-2-4, a small keyed hash (Aumasson / Bernstein; the algorithm is published
// and free to implement; this is written from its description). Used to derive the per-session console id: the console's
// persistent secret (mp.cfg) never leaves the console, only a keyed hash of it and the session id does, so two sessions cannot be
// linked to one console by what the console sends.
#ifndef PORT_MP_SIPHASH_H
#define PORT_MP_SIPHASH_H

#include <stddef.h>
#include <stdint.h>

static inline uint64_t mp_sip_rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
static inline uint64_t mp_sip_le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

#define MP_SIPROUND(v0, v1, v2, v3)                                  \
    do {                                                             \
        v0 += v1; v1 = mp_sip_rotl(v1, 13); v1 ^= v0; v0 = mp_sip_rotl(v0, 32); \
        v2 += v3; v3 = mp_sip_rotl(v3, 16); v3 ^= v2;                \
        v0 += v3; v3 = mp_sip_rotl(v3, 21); v3 ^= v0;                \
        v2 += v1; v1 = mp_sip_rotl(v1, 17); v1 ^= v2; v2 = mp_sip_rotl(v2, 32); \
    } while (0)

// key: 16 bytes. Returns the 64-bit tag.
static inline uint64_t mp_siphash24(const uint8_t key[16], const uint8_t* data, size_t len) {
    const uint64_t k0 = mp_sip_le64(key), k1 = mp_sip_le64(key + 8);
    uint64_t v0 = k0 ^ 0x736f6d6570736575ull, v1 = k1 ^ 0x646f72616e646f6dull;
    uint64_t v2 = k0 ^ 0x6c7967656e657261ull, v3 = k1 ^ 0x7465646279746573ull;
    const uint8_t* end = data + (len - len % 8);
    for (; data != end; data += 8) {
        const uint64_t m = mp_sip_le64(data);
        v3 ^= m;
        MP_SIPROUND(v0, v1, v2, v3);
        MP_SIPROUND(v0, v1, v2, v3);
        v0 ^= m;
    }
    uint64_t b = (uint64_t)len << 56;
    for (size_t i = 0; i < len % 8; i++) b |= (uint64_t)data[i] << (8 * i);
    v3 ^= b;
    MP_SIPROUND(v0, v1, v2, v3);
    MP_SIPROUND(v0, v1, v2, v3);
    v0 ^= b;
    v2 ^= 0xff;
    for (int i = 0; i < 4; i++) MP_SIPROUND(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

// The id a console shows to one session: SipHash-2-4 (key = the console's persistent 128-bit secret) of a domain tag and the session
// id, folded to 32 bits, never 0.
static inline uint32_t mp_session_wire_id(const uint8_t key[16], uint32_t sessionId) {
    uint8_t m[12] = {'s', 'm', 'g', 'm', 'p', 'i', 'd', '1', (uint8_t)sessionId, (uint8_t)(sessionId >> 8), (uint8_t)(sessionId >> 16),
                     (uint8_t)(sessionId >> 24)};
    const uint64_t h = mp_siphash24(key, m, sizeof m);
    return (uint32_t)(h ^ (h >> 32)) | 1u;
}

#endif  // PORT_MP_SIPHASH_H
