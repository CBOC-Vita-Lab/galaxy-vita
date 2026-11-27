// core2: platform/src/mp/mp_seal.h: pair keys agree, seal / open round trip, every byte tampered fails, wrong key / direction fails,
// the replay window.  g++ -std=c++20 -fsanitize=address,undefined -Iplatform/include tests/mp/seal_test.cpp -o /tmp/seal_test
#include "../../platform/src/mp/mp_seal.h"
#include <stdio.h>
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
static uint32_t g_r = 7;
static uint32_t R() { g_r ^= g_r << 13; g_r ^= g_r >> 17; g_r ^= g_r << 5; return g_r; }
int main() {
    mpseal::KeyPair a, b, c;
    mpseal::MakeKeyPair(&a, R);
    mpseal::MakeKeyPair(&b, R);
    mpseal::MakeKeyPair(&c, R);
    uint8_t psk[32] = {0}, psk2[32] = {1}, kab[32], kba[32], kac[32], kab2[32];
    CHECK(mpseal::PairKey(a.sk, a.pk, b.pk, psk, 77, kab) && mpseal::PairKey(b.sk, b.pk, a.pk, psk, 77, kba) && !memcmp(kab, kba, 32));
    CHECK(mpseal::PairKey(a.sk, a.pk, c.pk, psk, 77, kac) && memcmp(kab, kac, 32));
    CHECK(mpseal::PairKey(a.sk, a.pk, b.pk, psk2, 77, kab2) && memcmp(kab, kab2, 32));   // the room secret changes the key
    uint8_t zero[32] = {0};
    CHECK(!mpseal::PairKey(a.sk, a.pk, zero, psk, 77, kab2));   // a low-order key is refused
    uint8_t msg[300], sealed[400], out[400];
    for (int i = 0; i < 300; i++) msg[i] = (uint8_t)R();
    const size_t n = mpseal::Seal(kab, 3, 1, 42, msg, sizeof msg, sealed, sizeof sealed);
    CHECK(n == sizeof msg + mpseal::kOverhead && mpseal::Counter(sealed) == 42);
    size_t pl = 0;
    CHECK(mpseal::Open(kab, sealed, n, out, &pl) && pl == sizeof msg && !memcmp(out, msg, pl));
    CHECK(!mpseal::Open(kac, sealed, n, out, &pl));    // another pair's key
    int tampered = 0;
    for (size_t i = 0; i < n; i++)
        for (int bit = 0; bit < 8; bit += 3) {
            uint8_t t[400];
            memcpy(t, sealed, n);
            t[i] ^= (uint8_t)(1 << bit);
            if (!mpseal::Open(kab, t, n, out, &pl)) tampered++;
            else CHECK(false);
        }
    CHECK(!mpseal::Open(kab, sealed, n - 1, out, &pl));
    mpseal::Replay rp = {0, 0};
    CHECK(rp.check(5)); rp.accept(5);
    CHECK(!rp.check(5) && rp.check(4) && rp.check(6) && !rp.check(0));
    rp.accept(100);
    CHECK(!rp.check(5) && !rp.check(100) && rp.check(99) && rp.check(37) && !rp.check(36));
    rp.accept(99);
    CHECK(!rp.check(99));
    printf("seal_test: %d tampered datagrams refused, %d failures\n", tampered, g_fail);
    return g_fail ? 1 : 0;
}
