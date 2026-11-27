// code5: the far-away invite's code and its sealed content (platform/src/mp/mp_invite.cpp).
//   g++ -std=c++20 -fsanitize=address,undefined -Iplatform/include -Iplatform/src/mp tests/mp/invite_code_test.cpp platform/src/mp/mp_invite.cpp -o /tmp/invite_code_test
#include "port/mp_invite.h"
#include "port/mp_direct_code.h"

#include <initializer_list>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
static uint32_t g_r = 12345;
static uint32_t R() { g_r ^= g_r << 13; g_r ^= g_r >> 17; g_r ^= g_r << 5; return g_r; }

int main() {
    char c[8];
    // typing: case, spaces, dashes, dots, underscores forgiven; the five characters out in capitals
    CHECK(mp_invite_normalize("kcbc4", c) == MP_INV_TYPED_OK && !strcmp(c, "KCBC4"));
    CHECK(mp_invite_normalize(" k c-b.c_4 ", c) == MP_INV_TYPED_OK && !strcmp(c, "KCBC4"));
    CHECK(mp_invite_normalize("KC BC4", c) == MP_INV_TYPED_OK && !strcmp(c, "KCBC4"));
    // what is refused, and why (no request is made for any of them)
    CHECK(mp_invite_normalize("", c) == MP_INV_TYPED_EMPTY && !c[0]);
    CHECK(mp_invite_normalize(" - ", c) == MP_INV_TYPED_EMPTY);
    CHECK(mp_invite_normalize("KCBC", c) == MP_INV_TYPED_LENGTH);
    CHECK(mp_invite_normalize("KCBC45", c) == MP_INV_TYPED_LENGTH);
    for (const char* bad : {"KCBC0", "KCBCO", "KCBCo", "KCBC1", "KCBCI", "KCBCl", "0CBC4", "1CBC4"}) CHECK(mp_invite_normalize(bad, c) == MP_INV_TYPED_LOOKALIKE);
    CHECK(mp_invite_normalize("KC#C4", c) == MP_INV_TYPED_CHARACTER);
    CHECK(mp_invite_normalize("KCBC\xC3\xA9", c) == MP_INV_TYPED_CHARACTER);
    // which path a typed code takes: five = invite, 15 / 19 = the long direct code (the hidden fallback), else neither
    CHECK(mp_invite_kind("kcbc4") == 5 && mp_invite_kind("K C B C 4") == 5 && mp_invite_kind("") == 0 && mp_invite_kind("KC#C4") == 0);
    CHECK(mp_invite_kind("K7QMD-4DXTA-RS2PQ") == 15 && mp_invite_kind("K7QMD-4DXTA-RS2PQ-9HXA") == 19);
    CHECK(mp_invite_kind("0CBC4") == 5);   // (look-alikes count: they are refused by name afterwards)
    // making codes: five characters of the alphabet, every character reachable, none out of it
    static const char A[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    int seen[31] = {0};
    for (int i = 0; i < 20000; i++) {
        CHECK(mp_invite_make(R, c) && strlen(c) == 5);
        for (int k = 0; k < 5; k++) { const char* p = strchr(A, c[k]); CHECK(p != 0); if (p) seen[p - A]++; }
    }
    for (int i = 0; i < 31; i++) CHECK(seen[i] > 100000 / 31 * 8 / 10 && seen[i] < 100000 / 31 * 12 / 10);   // (about uniform)
    CHECK(!mp_invite_make(0, c) && !mp_invite_make(R, 0));
    // sealing: 48 base64url characters, opens only under the same code, every character of it matters
    const uint8_t sec[5] = {0x12, 0x34, 0x56, 0x78, 0x0A};
    const uint8_t nonce[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    char blob[64];
    CHECK(mp_invite_seal("KCBC4", 0xC6336401u, 41100, sec, nonce, blob) == 48 && strlen(blob) == 48);
    for (int i = 0; i < 48; i++) CHECK((blob[i] >= 'A' && blob[i] <= 'Z') || (blob[i] >= 'a' && blob[i] <= 'z') || (blob[i] >= '0' && blob[i] <= '9') || blob[i] == '-' || blob[i] == '_');
    uint32_t ip = 0;
    uint16_t port = 0;
    uint8_t s2[5] = {0};
    CHECK(mp_invite_open("KCBC4", blob, &ip, &port, s2) == MP_CODE_OK && ip == 0xC6336401u && port == 41100 && !memcmp(sec, s2, 5));
    CHECK(mp_invite_open("KCBC5", blob, &ip, &port, s2) == MP_CODE_CHECK);                 // the wrong code opens nothing
    CHECK(mp_invite_open("WME6M", blob, &ip, &port, s2) == MP_CODE_CHECK);
    int flipped = 0, opened = 0;
    for (int i = 0; i < 48; i++) {
        char t[64];
        strcpy(t, blob);
        t[i] = t[i] == 'A' ? 'B' : 'A';
        if (t[i] == blob[i]) continue;
        flipped++;
        if (mp_invite_open("KCBC4", t, &ip, &port, s2) == MP_CODE_OK) opened++;
    }
    CHECK(flipped >= 47 && opened == 0);                                                    // a damaged copy never opens (the tag checks every bit)
    CHECK(mp_invite_open("KCBC4", "short", &ip, &port, s2) == MP_CODE_CHECK && mp_invite_open("KCBC4", "", &ip, &port, s2) == MP_CODE_CHECK);
    CHECK(mp_invite_open("kcbc4", blob, &ip, &port, s2) == MP_CODE_CHECK && mp_invite_open(0, blob, &ip, &port, s2) == MP_CODE_CHECK);   // (the caller normalizes)
    // the same content under another nonce is another text; the same nonce and code is the same text
    char blob2[64], blob3[64];
    const uint8_t nonce2[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    CHECK(mp_invite_seal("KCBC4", 0xC6336401u, 41100, sec, nonce2, blob2) == 48 && strcmp(blob, blob2) != 0);
    CHECK(mp_invite_seal("KCBC4", 0xC6336401u, 41100, sec, nonce, blob3) == 48 && !strcmp(blob, blob3));
    CHECK(mp_invite_open("KCBC4", blob2, &ip, &port, s2) == MP_CODE_OK);
    // no address, no port: refused to seal; a sealed unusable address opens as MP_CODE_ADDRESS (a forged answer from a hostile service cannot be one)
    CHECK(mp_invite_seal("KCBC4", 0xC6336401u, 0, sec, nonce, blob3) == 0);
    CHECK(mp_invite_seal("KCBC", 0xC6336401u, 41100, sec, nonce, blob3) == 0 && mp_invite_seal("kcbc4", 1, 41100, sec, nonce, blob3) == 0);
    CHECK(mp_invite_seal("KCBC4", 0, 41100, sec, nonce, blob3) == 48 && mp_invite_open("KCBC4", blob3, &ip, &port, s2) == MP_CODE_ADDRESS);
    CHECK(mp_invite_seal("KCBC4", 0xE0000001u, 41100, sec, nonce, blob3) == 48 && mp_invite_open("KCBC4", blob3, &ip, &port, s2) == MP_CODE_ADDRESS);
    // no plain address in the text (a search of the stored text for the host's address finds nothing)
    CHECK(!strstr(blob, "198.51") && !strstr(blob, "Gz") );
    // random sweep: every ip / port / secret round-trips under random codes
    for (int i = 0; i < 300; i++) {
        char code[8];
        mp_invite_make(R, code);
        uint8_t se[5], no[8];
        for (int k = 0; k < 5; k++) se[k] = (uint8_t)R();
        for (int k = 0; k < 8; k++) no[k] = (uint8_t)R();
        uint32_t a = R() | 0x01000000u;
        if ((a >> 28) == 0xE || a == 0xFFFFFFFFu) a = 0x0A000001u;
        const uint16_t p = (uint16_t)(1 + R() % 65535);
        char b[64];
        CHECK(mp_invite_seal(code, a, p, se, no, b) == 48);
        CHECK(mp_invite_open(code, b, &ip, &port, s2) == MP_CODE_OK && ip == a && port == p && !memcmp(se, s2, 5));
    }
    if (g_fail) { fprintf(stderr, "invite_code_test: %d failed\n", g_fail); return 1; }
    printf("invite_code_test: ok\n");
    return 0;
}
