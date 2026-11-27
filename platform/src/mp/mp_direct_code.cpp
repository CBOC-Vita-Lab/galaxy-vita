// Galaxy 3.0 multiplayer (core2): the Play by Address spoken code (port/mp_direct_code.h). Pure, no allocation, 32-bit friendly
// (byte-array arithmetic: the Vita has no 128-bit integers).
#include "port/mp_direct_code.h"

#include <string.h>

namespace {
const char kAlpha[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";   // the room-code alphabet (31 characters; no I L O 0 1)
const int kBytes = 12;                                     // 96 bits: room for the 89-bit long form

int Value(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    const char* p = strchr(kAlpha, c);
    return c && p ? (int)(p - kAlpha) : -1;
}
bool LookAlike(char c) { return c == '0' || c == '1' || c == 'O' || c == 'o' || c == 'I' || c == 'i' || c == 'L' || c == 'l'; }
bool Separator(char c) { return c == ' ' || c == '-' || c == '.' || c == '_' || c == '\t'; }

void PutBits(uint8_t* n, int* at, uint64_t v, int bits) {
    for (int i = 0; i < bits; i++, (*at)++)
        if ((v >> i) & 1) n[*at / 8] |= (uint8_t)(1u << (*at % 8));
}
uint64_t GetBits(const uint8_t* n, int* at, int bits) {
    uint64_t v = 0;
    for (int i = 0; i < bits; i++, (*at)++)
        if (n[*at / 8] & (1u << (*at % 8))) v |= 1ull << i;
    return v;
}
// n /= 31, returns the remainder (little-endian bytes)
int DivMod31(uint8_t* n) {
    unsigned rem = 0;
    for (int i = kBytes - 1; i >= 0; i--) {
        const unsigned cur = (rem << 8) | n[i];
        n[i] = (uint8_t)(cur / 31);
        rem = cur % 31;
    }
    return (int)rem;
}
// n = n * 31 + d; false on overflow
bool MulAdd31(uint8_t* n, int d) {
    unsigned carry = (unsigned)d;
    for (int i = 0; i < kBytes; i++) {
        const unsigned cur = n[i] * 31u + carry;
        n[i] = (uint8_t)cur;
        carry = cur >> 8;
    }
    return carry == 0;
}
int Check(const int* v, int n) {
    unsigned s = 0;
    for (int i = 0; i < n; i++) s += (unsigned)(i + 1) * (unsigned)v[i];
    return (int)(s % 31);
}
}  // namespace

extern "C" int mp_direct_code_encode(uint32_t ip, uint16_t port, const uint8_t secret[MP_DIRECT_SECRET_BYTES], char* out, size_t cap) {
    if (!out || cap < MP_DIRECT_CODE_MAX || !secret) {
        if (out && cap) out[0] = 0;
        return 0;
    }
    const bool longForm = port != 0 && port != MP_DIRECT_DEFAULT_PORT;
    uint64_t sec = 0;
    for (int i = 0; i < MP_DIRECT_SECRET_BYTES; i++) sec |= (uint64_t)secret[i] << (8 * i);
    uint8_t n[kBytes];
    memset(n, 0, sizeof n);
    int at = 0;
    PutBits(n, &at, sec, longForm ? 40 : 36);
    PutBits(n, &at, ip, 32);
    if (longForm) PutBits(n, &at, port, 16);
    PutBits(n, &at, longForm ? 1 : 0, 1);
    const int payload = longForm ? 18 : 14;
    int v[19];
    for (int i = payload - 1; i >= 0; i--) v[i] = DivMod31(n);
    v[payload] = Check(v, payload);
    int w = 0;
    for (int i = 0; i <= payload; i++) {
        if (i && i % 5 == 0) out[w++] = '-';
        out[w++] = kAlpha[v[i]];
    }
    out[w] = 0;
    return w;
}

extern "C" int mp_direct_code_decode(const char* code, uint32_t* ip, uint16_t* port, uint8_t secret[MP_DIRECT_SECRET_BYTES]) {
    if (!code || !ip || !port || !secret) return MP_CODE_EMPTY;
    int v[20];
    int count = 0;
    bool any = false, lookalike = false, other = false;
    for (const char* p = code; *p; p++) {
        if (Separator(*p)) continue;
        any = true;
        const int x = Value(*p);
        if (x < 0) {
            if (LookAlike(*p)) lookalike = true;
            else other = true;
            continue;
        }
        if (count < 20) v[count] = x;
        count++;
    }
    if (!any) return MP_CODE_EMPTY;
    if (lookalike) return MP_CODE_LOOKALIKE;
    if (other) return MP_CODE_CHARACTER;
    if (count != 15 && count != 19) return MP_CODE_LENGTH;
    const int payload = count - 1;
    if (Check(v, payload) != v[payload]) return MP_CODE_CHECK;
    uint8_t n[kBytes];
    memset(n, 0, sizeof n);
    for (int i = 0; i < payload; i++)
        if (!MulAdd31(n, v[i])) return MP_CODE_CHECK;
    const bool longForm = count == 19;
    const int bits = longForm ? 89 : 69;
    for (int b = bits; b < kBytes * 8; b++)
        if (n[b / 8] & (1u << (b % 8))) return MP_CODE_CHECK;   // (a number larger than any code makes)
    int at = 0;
    const uint64_t sec = GetBits(n, &at, longForm ? 40 : 36);
    const uint32_t a = (uint32_t)GetBits(n, &at, 32);
    const uint16_t pt = longForm ? (uint16_t)GetBits(n, &at, 16) : (uint16_t)MP_DIRECT_DEFAULT_PORT;
    const uint64_t flag = GetBits(n, &at, 1);
    if ((flag != 0) != longForm) return MP_CODE_CHECK;
    if (a == 0 || a == 0xFFFFFFFFu || (a >> 28) == 0xE || pt == 0) return MP_CODE_ADDRESS;
    for (int i = 0; i < MP_DIRECT_SECRET_BYTES; i++) secret[i] = (uint8_t)(sec >> (8 * i));
    *ip = a;
    *port = pt;
    return MP_CODE_OK;
}
