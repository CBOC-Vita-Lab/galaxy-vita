// Host test of the registration proof-of-work solver (platform/src/mp/mp_pow.h): SHA-256 known answers, the service's own rule
// (the same nonce as server/src/core/pow.js solve() for a fixed challenge, checked by tests/mp/pow_test.sh with node), and a speed figure.
//   tests/mp/pow_test.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mp_pow.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void Hex(const uint8_t* d, int n, char* out) {
    for (int i = 0; i < n; ++i) sprintf(out + 2 * i, "%02x", d[i]);
}

static double Now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

int main(int argc, char** argv) {
    // FIPS 180-2 examples, and the 55 / 56 / 64-byte padding edges
    struct { const char* in; const char* want; } kat[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    for (auto& k : kat) {
        uint8_t d[32];
        char h[65];
        mp_pow::Sha256(d, k.in, strlen(k.in));
        Hex(d, 32, h);
        CHECK(!strcmp(h, k.want));
    }
    // (argv: "kat <message>" prints the digest, for the node cross-check of every length 0..130)
    if (argc == 3 && !strcmp(argv[1], "kat")) {
        uint8_t d[32];
        char h[65];
        mp_pow::Sha256(d, argv[2], strlen(argv[2]));
        Hex(d, 32, h);
        printf("%s\n", h);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "solve")) {   // solve <challenge> <bits>: the nonce
        mp_pow::Solver s;
        if (!s.Start(argv[2], atoi(argv[3]))) return 2;
        while (s.Step(4096) == 0) {}
        printf("%s\n", s.nonce);
        return 0;
    }
    // refusals
    mp_pow::Solver s;
    CHECK(!s.Start("", 16));
    CHECK(!s.Start("x", 29));
    CHECK(mp_pow::kMaxBits == 17);   // the server (server/src/core/config.js POW_CLIENT_MAX_BITS) clamps to the same number
    CHECK(!s.Start("1762800000000.0123456789abcdef.16.0123456789abcdef", mp_pow::kMaxBits + 1));   // svcfix2: more than 17 bits is refused (about 10 s on a Vita, estimate)
    CHECK(s.Start("1762800000000.0123456789abcdef.16.0123456789abcdef", mp_pow::kMaxBits));
    CHECK(!s.Start(nullptr, 8));
    char longc[100];
    memset(longc, 'a', sizeof longc - 1);
    longc[99] = 0;
    CHECK(!s.Start(longc, 8));
    // a limit is a limit
    CHECK(s.Start("1762800000000.0123456789abcdef.16.0123456789abcdef", mp_pow::kMaxBits, 1000));
    int r = 0;
    for (int i = 0; i < 10 && r == 0; ++i) r = s.Step(200);
    CHECK(r == -1);

    // speed: tries per second of one core, over ~1 s
    CHECK(s.Start("1762800000000.0123456789abcdef.16.0123456789abcdef", mp_pow::kMaxBits));
    const double t0 = Now();
    uint64_t tries = 0;
    while (Now() - t0 < 1.0) { s.Step(20000); tries += 20000; }
    const double rate = double(tries) / (Now() - t0);
    printf("pow: %.0f tries/s on this machine (one core)\n", rate);
    printf("pow: expected tries 2^16 = 65536: %.1f ms here; 2^18: %.1f ms here\n", 65536.0 / rate * 1e3, 262144.0 / rate * 1e3);
    printf(g_fail ? "pow_test: FAILED %d\n" : "pow_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
