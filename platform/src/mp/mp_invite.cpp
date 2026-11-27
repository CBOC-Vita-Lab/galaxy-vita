// Galaxy 3.0 multiplayer (code5): the far-away invite's code and its sealed content (port/mp_invite.h). Pure, no allocation.
#include "port/mp_invite.h"

#include "mp_seal.h"   // (Monocypher, internal linkage; before anything that includes monocypher.h)
#include "port/mp_direct_code.h"

#include <string.h>

namespace {
const char kAlpha[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";   // the room-code alphabet: 31 characters
const int kAlphaLen = 31;
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
const int kPlain = 12;           // version 1 | ip 4 | port 2 | secret 5
const int kNonce = 8;
const int kTag = 16;
const int kBytes = kNonce + kPlain + kTag;   // 36 -> 48 characters

bool LookAlike(char c) { return c == '0' || c == '1' || c == 'O' || c == 'o' || c == 'I' || c == 'i' || c == 'L' || c == 'l'; }
bool Separator(char c) { return c == ' ' || c == '-' || c == '.' || c == '_' || c == '\t'; }
char Up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c; }
bool InAlpha(char c) { return c && strchr(kAlpha, c) != 0; }

// The key: BLAKE2b over the code, then many more rounds, so reading a copied database costs hours of work, not seconds (28.6 million codes
// is nothing against one hash; it is 4096 hashes each). One-off on a console (a few milliseconds).
void KeyFor(const char* code, uint8_t key[32]) {
    static const uint8_t kSalt[] = "SMG3 invite key v1";
    uint8_t h[32];
    mpseal::Mac(h, 32, kSalt, sizeof kSalt - 1, (const uint8_t*)code, MP_INVITE_LEN);
    for (int i = 0; i < 4096; i++) mpseal::Mac(h, 32, kSalt, sizeof kSalt - 1, h, 32);
    memcpy(key, h, 32);
    mpc::crypto_wipe(h, sizeof h);
}

void B64Put(const uint8_t* in, char* out) {   // 36 bytes -> 48 characters
    for (int i = 0, o = 0; i < kBytes; i += 3, o += 4) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o] = kB64[(v >> 18) & 63];
        out[o + 1] = kB64[(v >> 12) & 63];
        out[o + 2] = kB64[(v >> 6) & 63];
        out[o + 3] = kB64[v & 63];
    }
    out[48] = 0;
}
int B64Val(char c) {
    const char* p = c ? strchr(kB64, c) : 0;
    return p ? (int)(p - kB64) : -1;
}
bool B64Get(const char* in, uint8_t* out) {
    if (!in || strlen(in) != 48) return false;
    for (int i = 0, o = 0; i < 48; i += 4, o += 3) {
        const int a = B64Val(in[i]), b = B64Val(in[i + 1]), c = B64Val(in[i + 2]), d = B64Val(in[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return false;
        const uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
        out[o] = (uint8_t)(v >> 16);
        out[o + 1] = (uint8_t)(v >> 8);
        out[o + 2] = (uint8_t)v;
    }
    return true;
}
bool CodeOk(const char* code) {
    if (!code) return false;
    for (int i = 0; i < MP_INVITE_LEN; i++)
        if (!InAlpha(code[i])) return false;
    return code[MP_INVITE_LEN] == 0;
}
}  // namespace

extern "C" int mp_invite_normalize(const char* typed, char* out) {
    if (out) out[0] = 0;
    if (!typed || !out) return MP_INV_TYPED_EMPTY;
    char tmp[MP_INVITE_LEN + 1];
    int n = 0;
    bool any = false, look = false, other = false;
    for (const char* p = typed; *p; p++) {
        if (Separator(*p)) continue;
        any = true;
        const char u = Up(*p);
        if (InAlpha(u)) {
            if (n < MP_INVITE_LEN) tmp[n] = u;
            n++;
        } else if (LookAlike(*p)) look = true;
        else other = true;
    }
    if (!any) return MP_INV_TYPED_EMPTY;
    if (look) return MP_INV_TYPED_LOOKALIKE;
    if (other) return MP_INV_TYPED_CHARACTER;
    if (n != MP_INVITE_LEN) return MP_INV_TYPED_LENGTH;
    memcpy(out, tmp, MP_INVITE_LEN);
    out[MP_INVITE_LEN] = 0;
    return MP_INV_TYPED_OK;
}

extern "C" int mp_invite_kind(const char* typed) {
    if (!typed) return 0;
    int n = 0;
    for (const char* p = typed; *p; p++) {
        if (Separator(*p)) continue;
        if (!InAlpha(Up(*p)) && !LookAlike(*p)) return 0;
        n++;
    }
    return n;
}

extern "C" bool mp_invite_make(uint32_t (*rnd)(void), char* out) {
    if (!rnd || !out) return false;
    for (int i = 0; i < MP_INVITE_LEN; i++) {
        uint32_t v;
        do v = rnd(); while (v >= 0xFFFFFFFFu - (0xFFFFFFFFu % kAlphaLen));   // (no modulo bias)
        out[i] = kAlpha[v % kAlphaLen];
    }
    out[MP_INVITE_LEN] = 0;
    return true;
}

extern "C" int mp_invite_seal(const char* code, uint32_t ip, uint16_t port, const uint8_t* secret, const uint8_t* nonce8, char* out) {
    if (out) out[0] = 0;
    if (!CodeOk(code) || !secret || !nonce8 || !out || port == 0) return 0;
    uint8_t key[32], plain[kPlain], nonce[24], buf[kBytes];
    KeyFor(code, key);
    plain[0] = 1;
    for (int i = 0; i < 4; i++) plain[1 + i] = (uint8_t)(ip >> (8 * i));
    plain[5] = (uint8_t)port;
    plain[6] = (uint8_t)(port >> 8);
    memcpy(plain + 7, secret, 5);
    memset(nonce, 0, sizeof nonce);
    memcpy(nonce, nonce8, kNonce);
    memcpy(buf, nonce8, kNonce);
    // additional data: the code's length and the version, so a blob of another scheme never opens as this one
    static const uint8_t kAd[] = {'S', 'M', 'G', '3', 'i', 'n', 'v', 1};
    mpc::crypto_aead_lock(buf + kNonce, buf + kNonce + kPlain, key, nonce, kAd, sizeof kAd, plain, kPlain);
    B64Put(buf, out);
    mpc::crypto_wipe(key, sizeof key);
    mpc::crypto_wipe(plain, sizeof plain);
    return 48;
}

extern "C" int mp_invite_open(const char* code, const char* blob, uint32_t* ip, uint16_t* port, uint8_t* secret) {
    if (!CodeOk(code) || !blob || !ip || !port || !secret) return MP_CODE_CHECK;
    uint8_t buf[kBytes], key[32], plain[kPlain], nonce[24];
    if (!B64Get(blob, buf)) return MP_CODE_CHECK;
    KeyFor(code, key);
    memset(nonce, 0, sizeof nonce);
    memcpy(nonce, buf, kNonce);
    static const uint8_t kAd[] = {'S', 'M', 'G', '3', 'i', 'n', 'v', 1};
    const int bad = mpc::crypto_aead_unlock(plain, buf + kNonce + kPlain, key, nonce, kAd, sizeof kAd, buf + kNonce, kPlain);
    mpc::crypto_wipe(key, sizeof key);
    if (bad != 0 || plain[0] != 1) return MP_CODE_CHECK;
    uint32_t a = 0;
    for (int i = 0; i < 4; i++) a |= (uint32_t)plain[1 + i] << (8 * i);
    const uint16_t pt = (uint16_t)(plain[5] | (plain[6] << 8));
    if (a == 0 || a == 0xFFFFFFFFu || (a >> 28) == 0xE || pt == 0) return MP_CODE_ADDRESS;
    *ip = a;
    *port = pt;
    memcpy(secret, plain + 7, 5);
    mpc::crypto_wipe(plain, sizeof plain);
    return MP_CODE_OK;
}
