// core2 stage 4: what the session crypto costs on this machine (platform/src/mp/mp_seal.h). Desktop numbers only; the Vita's are
// estimated in docs/MP-SESSION-SECURITY.md and measured on hardware with SMG_MP_CRYPTO_LOG=1 (one log line per session start).
//   g++ -std=c++20 -O2 -Iplatform/include tests/mp/seal_bench.cpp -o /tmp/seal_bench && /tmp/seal_bench
#include "../../platform/src/mp/mp_seal.h"
#include <chrono>
#include <stdio.h>
static uint32_t R() { static uint32_t x = 12345; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
static double Us(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count();
}
int main() {
    mpseal::KeyPair a, b;
    mpseal::MakeKeyPair(&a, R);
    mpseal::MakeKeyPair(&b, R);
    uint8_t psk[32] = {0}, k[32];
    auto t = std::chrono::steady_clock::now();
    const int nk = 200;
    for (int i = 0; i < nk; i++) mpseal::PairKey(a.sk, a.pk, b.pk, psk, 77u + (uint32_t)i, k);
    const double pairUs = Us(t) / nk;
    static uint8_t plain[1400], sealed[1500], out[1500];
    for (auto& c : plain) c = (uint8_t)R();
    const size_t sizes[] = {80, 300, 1200};
    printf("pair key (X25519 + BLAKE2b): %.1f us\n", pairUs);
    for (size_t sz : sizes) {
        const int n = 20000;
        size_t sn = 0, pl = 0;
        t = std::chrono::steady_clock::now();
        for (int i = 0; i < n; i++) sn = mpseal::Seal(k, 3, 1, (uint64_t)i + 1, plain, sz, sealed, sizeof sealed);
        const double sealUs = Us(t) / n;
        t = std::chrono::steady_clock::now();
        int okc = 0;
        for (int i = 0; i < n; i++) okc += mpseal::Open(k, sealed, sn, out, &pl) ? 1 : 0;
        const double openUs = Us(t) / n;
        printf("datagram %4zu B: seal %.2f us, open %.2f us (+%zu B on the wire)%s\n", sz, sealUs, openUs, sn - sz, okc == n ? "" : " OPEN FAILED");
    }
    return 0;
}
