// MD5, SHA-1 and HMAC-SHA1 for the TURN client (RFC 8656 / 5389 long-term credentials: key = MD5(user ":" realm ":" password),
// MESSAGE-INTEGRITY = HMAC-SHA1). Small, dependency-free, public domain (see the notice in np_hash.cpp). They sit behind this interface so the
// implementation can move to mbedtls (np_https brings it) without touching the callers; none of this protects game data (that is np_crypto.h).
#ifndef NP_NET_HASH_H
#define NP_NET_HASH_H

#include <stddef.h>
#include <stdint.h>

namespace NpNet {

void Md5(uint8_t out[16], const void* data, size_t len);
void Sha1(uint8_t out[20], const void* data, size_t len);
void HmacSha1(uint8_t out[20], const void* key, size_t keyLen, const void* msg, size_t msgLen);

}  // namespace NpNet

#endif
