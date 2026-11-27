// Galaxy 3.0 multiplayer (core2): session crypto (DESIGN.md section 10). Per-pair X25519 keys, sealed datagrams (XChaCha20-Poly1305),
// keyed BLAKE2b MACs, from the vendored Monocypher (net/src/online/third_party/monocypher, public domain / CC0 / BSD-2): compiled
// into the one translation unit that includes this header, with internal linkage (no clash with net/'s own copy, nothing more to link).
//
// Sealed datagram: 'G' 'S' proto u8, sender slot u8, counter u48 LE | ciphertext of the plain datagram | tag 16
//   key K(sender, receiver) (one key per pair); nonce = sender slot (0xFF while joining), "SMG3", counter u64 LE, 11 zero bytes (the
//   pair's two directions differ by the sender slot); additional data = the header.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef MONOCYPHER_H
#error "mp_seal.h: include it before anything that includes monocypher.h (its copy here must stay internal)"
#endif
namespace {
namespace mpc {
// (Monocypher's own C++ namespace switch: without it monocypher.h declares everything extern "C", which escapes the unnamed namespace)
#define MONOCYPHER_CPP_NAMESPACE m
#include "../../../net/src/online/third_party/monocypher/monocypher.c"
#undef MONOCYPHER_CPP_NAMESPACE
using namespace m;
}  // namespace mpc

namespace mpseal {
const size_t kHeader = 10, kTag = 16, kOverhead = kHeader + kTag;

struct KeyPair {
    uint8_t sk[32], pk[32];
};
inline void PublicKey(uint8_t pk[32], const uint8_t sk[32]) { mpc::crypto_x25519_public_key(pk, sk); }
inline void MakeKeyPair(KeyPair* kp, uint32_t (*rnd)()) {
    for (int i = 0; i < 32; i += 4) {
        const uint32_t v = rnd();
        memcpy(kp->sk + i, &v, 4);
    }
    mpc::crypto_x25519_public_key(kp->pk, kp->sk);
}
inline void Mac(uint8_t* out, size_t outLen, const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t len) {
    mpc::crypto_blake2b_keyed(out, outLen, key, keyLen, msg, len);
}
inline bool Equal16(const uint8_t a[16], const uint8_t b[16]) { return mpc::crypto_verify16(a, b) == 0; }
// K(a, b) = BLAKE2b-256(key = psk, "SMG3 pair" | X25519(sk, peerPk) | lower pk | higher pk | session id). False for a low-order peer key.
inline bool PairKey(const uint8_t sk[32], const uint8_t myPk[32], const uint8_t peerPk[32], const uint8_t psk[32], uint32_t sessionId,
                    uint8_t out[32]) {
    uint8_t shared[32];
    mpc::crypto_x25519(shared, sk, peerPk);
    uint8_t any = 0;
    for (int i = 0; i < 32; i++) any |= shared[i];
    if (!any) return false;
    uint8_t msg[9 + 32 + 64 + 4];
    memcpy(msg, "SMG3 pair", 9);
    memcpy(msg + 9, shared, 32);
    const bool myFirst = memcmp(myPk, peerPk, 32) < 0;
    memcpy(msg + 41, myFirst ? myPk : peerPk, 32);
    memcpy(msg + 73, myFirst ? peerPk : myPk, 32);
    memcpy(msg + 105, &sessionId, 4);
    mpc::crypto_blake2b_keyed(out, 32, psk, 32, msg, sizeof msg);
    mpc::crypto_wipe(shared, sizeof shared);
    mpc::crypto_wipe(msg, sizeof msg);
    return true;
}
inline void Nonce(uint8_t n[24], uint8_t from, uint64_t ctr) {
    memset(n, 0, 24);
    n[0] = from;
    memcpy(n + 1, "SMG3", 4);
    for (int i = 0; i < 8; i++) n[5 + i] = (uint8_t)(ctr >> (8 * i));
}
// out (cap >= len + kOverhead): the sealed datagram; returns its length (0: no room)
inline size_t Seal(const uint8_t key[32], uint8_t proto, uint8_t from, uint64_t ctr, const uint8_t* plain, size_t len, uint8_t* out,
                   size_t cap) {
    if (cap < len + kOverhead || ctr >= (1ull << 48)) return 0;
    out[0] = 'G';
    out[1] = 'S';
    out[2] = proto;
    out[3] = from;
    for (int i = 0; i < 6; i++) out[4 + i] = (uint8_t)(ctr >> (8 * i));
    uint8_t n[24];
    Nonce(n, from, ctr);
    mpc::crypto_aead_lock(out + kHeader, out + kHeader + len, key, n, out, kHeader, plain, len);
    return len + kOverhead;
}
inline uint64_t Counter(const uint8_t* in) {
    uint64_t c = 0;
    for (int i = 0; i < 6; i++) c |= (uint64_t)in[4 + i] << (8 * i);
    return c;
}
// plain (cap >= len - kOverhead): true when the tag is right
inline bool Open(const uint8_t key[32], const uint8_t* in, size_t len, uint8_t* plain, size_t* plainLen) {
    if (len < kOverhead + 1 || in[0] != 'G' || in[1] != 'S') return false;
    const size_t n = len - kOverhead;
    uint8_t nonce[24];
    Nonce(nonce, in[3], Counter(in));
    if (mpc::crypto_aead_unlock(plain, in + kHeader + n, key, nonce, in, kHeader, in + kHeader, n) != 0) return false;
    *plainLen = n;
    return true;
}
// replay window per pair and direction: counters must be new and within 64 behind the highest
struct Replay {
    uint64_t max, mask;
    bool check(uint64_t c) const { return c && (c > max || (max - c < 64 && !(mask & (1ull << (max - c))))); }
    void accept(uint64_t c) {
        if (c > max) {
            const uint64_t sh = c - max;
            mask = sh >= 64 ? 0 : mask << sh;
            mask |= 1;
            max = c;
        } else {
            mask |= 1ull << (max - c);
        }
    }
};
}  // namespace mpseal
}  // namespace
