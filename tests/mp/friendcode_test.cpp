// core2: the public player code parser of the online signal (net/src/online/np_signal.h SigPlayerCode): untrusted service text.
//   g++ -std=c++20 -fsanitize=address,undefined -Inet/src -Inet/src/online -Inet/include tests/mp/friendcode_test.cpp -o /tmp/friendcode_test
#include "online/np_signal.h"

#include <stdio.h>
#include <string.h>

using namespace NpNet;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
    char out[10];
    CHECK(SigPlayerCode("K7QM-4DXT", out) && !strcmp(out, "K7QM-4DXT"));
    const char* bad[] = {"", "K7QM4DXT", "K7QM-4DX", "K7QM-4DXTA", "k7qm-4dxt", "K7QM-4DX1", "I7QM-4DXT", "K7QM 4DXT", "K7QM--DXT",
                         "K7QM-4DXT\n", "<script>", "K7QM-4D\xC3\xA9"};
    for (const char* b : bad) {
        memset(out, 'Z', sizeof out);
        CHECK(!SigPlayerCode(b, out) && out[0] == 0);
    }
    CHECK(!SigPlayerCode(nullptr, out) && out[0] == 0);
    // fuzz: random bytes (up to 12) never write past out[9], and what passes is exactly the shape
    uint32_t r = 12345;
    int accepted = 0;
    for (int i = 0; i < 2000000; i++) {
        char in[13];
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        const int n = (int)(r % 13);
        for (int k = 0; k < n; k++) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            in[k] = (r & 1) ? "ABCDEFGHJKMNPQRSTUVWXYZ23456789-"[r % 32] : (char)(r >> 8);
        }
        in[n] = 0;
        char o[10];
        if (SigPlayerCode(in, o)) {
            accepted++;
            CHECK(strlen(o) == 9 && o[4] == '-' && !strcmp(o, in));
        } else {
            CHECK(o[0] == 0);
        }
    }
    printf("friendcode_test: %d of 2000000 random inputs accepted, %d failures\n", accepted, g_fail);
    return g_fail ? 1 : 0;
}
