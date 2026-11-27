// Small crypto interface of the online transport, over a vendored Monocypher 4.0.3 (BSD-2-Clause / CC0, see third_party/monocypher/LICENCE.md).
// Everything the transport needs is here, so the implementation can move to mbedtls (np_https brings it) without touching the callers:
//   * AEAD: XChaCha20-Poly1305 (24-byte nonce, 16-byte tag), the construction of draft-irtf-cfrg-xchacha
//   * keyed hash: BLAKE2b (key derivation and the 16-byte MACs of the punch packets)
//   * constant-time compare, wipe
#ifndef NP_NET_CRYPTO_H
#define NP_NET_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

namespace NpNet {

const size_t kKeySize = 32, kNonceSize = 24, kMacSize = 16;

// out = BLAKE2b-256(key = `key`, msg = concatenation of up to 3 parts). Used as KDF and PRF.
void Kdf(uint8_t out[32], const uint8_t key[32], const void* a, size_t aLen, const void* b = 0, size_t bLen = 0, const void* c = 0, size_t cLen = 0);
// 16-byte MAC: first half of the keyed BLAKE2b-256 of `msg`.
void Mac16(uint8_t out[16], const uint8_t key[32], const void* msg, size_t len);
bool MacEqual(const uint8_t a[16], const uint8_t b[16]);

// ciphertext (len bytes) and tag (16) are written; `ad` is authenticated, not encrypted. In-place (plain == cipher) is allowed.
void AeadSeal(uint8_t* cipher, uint8_t tag[16], const uint8_t key[32], const uint8_t nonce[24], const void* ad, size_t adLen, const uint8_t* plain, size_t len);
// false when the tag does not verify (nothing is written to `plain` then).
bool AeadOpen(uint8_t* plain, const uint8_t key[32], const uint8_t nonce[24], const void* ad, size_t adLen, const uint8_t* cipher, size_t len, const uint8_t tag[16]);

void Wipe(void* p, size_t n);

}  // namespace NpNet

#endif
