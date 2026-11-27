// See np_crypto.h.
#include "np_crypto.h"
#include "np_net.h"

extern "C" {
#include "third_party/monocypher/monocypher.h"
}

#include <stdio.h>
#include <string.h>

#if defined(NP_TARGET_VITA)
#include <psp2/kernel/rng.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace NpNet {

void Kdf(uint8_t out[32], const uint8_t key[32], const void* a, size_t aLen, const void* b, size_t bLen, const void* c, size_t cLen) {
    crypto_blake2b_ctx ctx;
    crypto_blake2b_keyed_init(&ctx, 32, key, 32);
    if (a && aLen) crypto_blake2b_update(&ctx, static_cast<const uint8_t*>(a), aLen);
    if (b && bLen) crypto_blake2b_update(&ctx, static_cast<const uint8_t*>(b), bLen);
    if (c && cLen) crypto_blake2b_update(&ctx, static_cast<const uint8_t*>(c), cLen);
    crypto_blake2b_final(&ctx, out);
}

void Mac16(uint8_t out[16], const uint8_t key[32], const void* msg, size_t len) {
    uint8_t full[32];
    Kdf(full, key, msg, len);
    memcpy(out, full, 16);
    crypto_wipe(full, sizeof full);
}

bool MacEqual(const uint8_t a[16], const uint8_t b[16]) { return crypto_verify16(a, b) == 0; }

void AeadSeal(uint8_t* cipher, uint8_t tag[16], const uint8_t key[32], const uint8_t nonce[24], const void* ad, size_t adLen, const uint8_t* plain, size_t len) {
    crypto_aead_lock(cipher, tag, key, nonce, static_cast<const uint8_t*>(ad), adLen, plain, len);
}

bool AeadOpen(uint8_t* plain, const uint8_t key[32], const uint8_t nonce[24], const void* ad, size_t adLen, const uint8_t* cipher, size_t len, const uint8_t tag[16]) {
    return crypto_aead_unlock(plain, tag, key, nonce, static_cast<const uint8_t*>(ad), adLen, cipher, len) == 0;
}

void Wipe(void* p, size_t n) { crypto_wipe(static_cast<uint8_t*>(p), n); }

// ---- system randomness ----------------------------------------------------------------------------------------------------
const char* RandomFailureSentence() { return "This console's random number generator is not working, so a secure online link cannot be made."; }

bool SystemRandom(void*, void* out, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(out);
#if defined(NP_TARGET_VITA)
    // sceKernelGetRandomNumber fills at most 64 bytes per call (psp2/kernel/rng.h; np_https_sys.cpp FillRandom uses the same chunk)
    const size_t kChunk = 64;
    while (n) {
        const size_t k = n > kChunk ? kChunk : n;
        if (sceKernelGetRandomNumber(p, k) < 0) {
            // the console's RNG refused: this must never silently produce a predictable key
            fprintf(stderr, "[np-net] sceKernelGetRandomNumber failed\n");
            memset(out, 0, size_t(p - static_cast<uint8_t*>(out)) + n);
            return false;
        }
        p += k;
        n -= k;
    }
    return true;
#else
    static int fd = -1;
    if (fd < 0) fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    uint8_t* const start = p;
    const size_t total = n;
    while (n) {
        const ssize_t r = fd >= 0 ? read(fd, p, n) : -1;
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            fprintf(stderr, "[np-net] /dev/urandom unavailable\n");
            memset(start, 0, total);
            return false;
        }
        p += r;
        n -= size_t(r);
    }
    return true;
#endif
}

}  // namespace NpNet
