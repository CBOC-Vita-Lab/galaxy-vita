// core2: the Play by Address spoken code (platform/src/mp/mp_direct_code.cpp).
//   g++ -std=c++20 -fsanitize=address,undefined -Iplatform/include tests/mp/direct_code_test.cpp platform/src/mp/mp_direct_code.cpp -o /tmp/direct_code_test
#include "port/mp_direct_code.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
static uint32_t g_r = 99;
static uint32_t R() { g_r ^= g_r << 13; g_r ^= g_r >> 17; g_r ^= g_r << 5; return g_r; }

int main() {
    char c[MP_DIRECT_CODE_MAX];
    uint8_t s[5] = {0x12, 0x34, 0x56, 0x78, 0x0A}, s2[5];
    uint32_t ip;
    uint16_t port;
    CHECK(mp_direct_code_encode(0xC6336401u, MP_DIRECT_DEFAULT_PORT, s, c, sizeof c) == 17);
    printf("short code: %s\n", c);
    CHECK(mp_direct_code_decode(c, &ip, &port, s2) == MP_CODE_OK && ip == 0xC6336401u && port == MP_DIRECT_DEFAULT_PORT && !memcmp(s, s2, 5));
    CHECK(mp_direct_code_encode(0xC6336401u, 50123, s, c, sizeof c) == 22);
    printf("long code: %s\n", c);
    CHECK(mp_direct_code_decode(c, &ip, &port, s2) == MP_CODE_OK && port == 50123 && !memcmp(s, s2, 5));
    // read aloud: lower case, no dashes, spaces anywhere
    char t[64];
    mp_direct_code_encode(0x0A000002u, 0, s, c, sizeof c);
    int k = 0;
    for (const char* p = c; *p; p++) if (*p != '-') { t[k++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p); if (k % 3 == 0) t[k++] = ' '; }
    t[k] = 0;
    CHECK(mp_direct_code_decode(t, &ip, &port, s2) == MP_CODE_OK && ip == 0x0A000002u);
    // look-alikes, other characters, lengths, empty
    strcpy(t, c); t[0] = '0'; CHECK(mp_direct_code_decode(t, &ip, &port, s2) == MP_CODE_LOOKALIKE);
    strcpy(t, c); t[1] = 'l'; CHECK(mp_direct_code_decode(t, &ip, &port, s2) == MP_CODE_LOOKALIKE);
    strcpy(t, c); t[2] = '#'; CHECK(mp_direct_code_decode(t, &ip, &port, s2) == MP_CODE_CHARACTER);
    CHECK(mp_direct_code_decode("ABCDE-FGHJK", &ip, &port, s2) == MP_CODE_LENGTH);
    CHECK(mp_direct_code_decode(" - ", &ip, &port, s2) == MP_CODE_EMPTY);
    CHECK(mp_direct_code_decode("", &ip, &port, s2) == MP_CODE_EMPTY);
    // every single wrong character and every swap of neighbours is caught
    static const char A[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    int singles = 0, swaps = 0;
    for (int trial = 0; trial < 300; trial++) {
        for (int i = 0; i < 5; i++) s[i] = (uint8_t)R();
        const uint16_t pt = trial % 2 ? (uint16_t)(1 + R() % 65535) : 0;
        uint32_t a = R();
        if (a == 0 || a == 0xFFFFFFFFu || (a >> 28) == 0xE) a = 0x0A000001u;
        CHECK(mp_direct_code_encode(a, pt, s, c, sizeof c) > 0);
        CHECK(mp_direct_code_decode(c, &ip, &port, s2) == MP_CODE_OK && ip == a);
        for (int i = 0; c[i]; i++) {
            if (c[i] == '-') continue;
            for (int x = 0; x < 31; x++) {
                if (A[x] == c[i]) continue;
                strcpy(t, c); t[i] = A[x];
                CHECK(mp_direct_code_decode(t, &ip, &port, s2) != MP_CODE_OK);
                singles++;
            }
            int j = i + 1;
            if (c[j] == '-') j++;
            if (c[j] && c[j] != c[i]) {
                strcpy(t, c); t[i] = c[j]; t[j] = c[i];
                CHECK(mp_direct_code_decode(t, &ip, &port, s2) != MP_CODE_OK);
                swaps++;
            }
        }
    }
    // fuzz: random strings never crash, never write past the buffers
    for (int i = 0; i < 300000; i++) {
        char in[32];
        const int n = (int)(R() % 31);
        for (int j = 0; j < n; j++) in[j] = (R() & 1) ? A[R() % 31] : (char)(1 + R() % 255);
        in[n] = 0;
        mp_direct_code_decode(in, &ip, &port, s2);
    }
    CHECK(mp_direct_code_encode(1, 0, s, c, 10) == 0 && c[0] == 0);
    printf("direct_code_test: %d single-character errors and %d neighbour swaps all caught; %d failures\n", singles, swaps, g_fail);
    return g_fail ? 1 : 0;
}
