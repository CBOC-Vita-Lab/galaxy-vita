// Galaxy 3.0 multiplayer (release3): "Save a report". See mp_report.h.
extern "C" void port_host_alloc_scope_enter(void) __attribute__((weak));
extern "C" void port_host_alloc_scope_leave(void) __attribute__((weak));
namespace {
struct MpHostAllocScope {
    MpHostAllocScope() { if (port_host_alloc_scope_enter) port_host_alloc_scope_enter(); }
    ~MpHostAllocScope() { if (port_host_alloc_scope_leave) port_host_alloc_scope_leave(); }
};
}  // namespace
#define MP_HOST_ALLOC MpHostAllocScope mpHostAllocScope_

#include "mp_report.h"

#include "mp_config.h"
#include "mp_posix_io.h"   // netfix B8b
#include "port/mp_proto.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern "C" uint32_t mp_core_fingerprint(void) __attribute__((weak));
extern "C" const char* mp_service_app_version(void) __attribute__((weak));
extern "C" void mp_log(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
const MpConfig* mp_config(void) __attribute__((weak));

namespace {

// ---- the scrubber --------------------------------------------------------------------------------------------------------------

struct Out {
    char* p;
    size_t cap;   // bytes available including the terminator
    size_t n;
    void put(char c) { if (n + 1 < cap) p[n++] = c; }
    void puts(const char* s) { while (*s) put(*s++); }
    void putn(const char* s, size_t k) { for (size_t i = 0; i < k; i++) put(s[i]); }
};

bool IsWordChar(unsigned char c) { return isalnum(c) || c == '_'; }
bool IsTokenChar(unsigned char c) { return isalnum(c) || c == ':' || c == '.' || c == '_' || c == '-' || c == '[' || c == ']'; }
bool IsHex(unsigned char c) { return isxdigit(c) != 0; }

// The output so far ends with `word` and a space ("... joining "): the next token is what that word introduces.
bool EndsWithWord(const Out& o, const char* word) {
    const size_t l = strlen(word);
    if (o.n < l + 1 || o.p[o.n - 1] != ' ' || memcmp(o.p + o.n - 1 - l, word, l) != 0) return false;
    return o.n == l + 1 || !IsWordChar(static_cast<unsigned char>(o.p[o.n - 2 - l]));
}

bool Starts(const char* s, size_t left, const char* lit) {
    const size_t k = strlen(lit);
    return left >= k && memcmp(s, lit, k) == 0;
}

// A friend code is "XXXX-XXXX" (8 characters of A-Z without I L O and 2-9, a hyphen in the middle: mp_service.cpp FriendCodeValid / kCodeAlphabet)
// and a room code 6 plain characters of the same set. The service's error codes are lower case words with underscores ("room_full").
// Used on its own, a plain 6-8 character run is too common to hide anywhere, so it is only hidden after a key (see ErrorCodeWord) or the
// words "joining" / "room" / "friend" / "code"; the hyphenated form is recognised anywhere.
bool CodeChar(char c) { return (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '9'); }
bool LooksLikeFriendCode(const char* s, size_t n) {   // XXXX-XXXX
    if (n != 9 || s[4] != '-') return false;
    for (size_t i = 0; i < n; i++)
        if (i != 4 && !CodeChar(s[i])) return false;
    return true;
}
bool LooksLikeCode(const char* s, size_t n) {   // 6-8 plain characters, or the hyphenated friend code
    if (LooksLikeFriendCode(s, n)) return true;
    if (n < 6 || n > 8) return false;
    for (size_t i = 0; i < n; i++)
        if (!CodeChar(s[i])) return false;
    return true;
}
// What `code=` may show: a service error word ("version_mismatch"). Everything else after `code=` (a friend or room code in any case or
// spelling, a number) is hidden: the test is inverted on purpose, a code format that changes must fail closed.
bool ErrorCodeWord(const char* s, size_t n) {
    if (n == 0 || n > 40 || !(s[0] >= 'a' && s[0] <= 'z')) return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) return false;
    return true;
}

// a.b.c.d with optional :port (parts 1-3 digits, port 1-5 digits), the whole token. Also "[a.b.c.d]:port" is handled by the caller (brackets stripped).
// A part may be a wildcard (x, X or *) when at least three parts are written: the log's own shortened public addresses ("81.12.x.x:41100",
// np_net.cpp AddrLog) and anything a person typed by hand are addresses too, the first two octets already narrow a person down.
bool IsIpv4Token(const char* s, size_t n) {
    size_t i = 0;
    int parts = 0, wild = 0, digitsParts = 0;
    for (;;) {
        if (i < n && (s[i] == 'x' || s[i] == 'X' || s[i] == '*')) { ++i; ++wild; }
        else {
            size_t d = 0;
            while (i < n && isdigit(static_cast<unsigned char>(s[i]))) { ++i; ++d; }
            if (d < 1 || d > 3) return false;
            ++digitsParts;
        }
        ++parts;
        if (parts == 4) break;
        if (i >= n || s[i] != '.') {
            if (wild && parts >= 3 && digitsParts >= 1) break;   // "81.12.x" (three written parts)
            return false;
        }
        ++i;
    }
    if (parts < 4 && !(wild && parts >= 3)) return false;
    if (wild == 0 && parts != 4) return false;
    if (i == n) return true;
    if (s[i] != ':') return false;
    ++i;
    size_t d = 0;
    while (i < n && isdigit(static_cast<unsigned char>(s[i]))) { ++i; ++d; }
    return d >= 1 && d <= 5 && i == n;
}

// IPv6 and MAC-like: hex digits and colons (and dots for the v4-in-v6 tail), "::" present or at least 5 colons. A clock time (12:34:56) has 2 colons.
bool IsIpv6Token(const char* s, size_t n) {
    int colons = 0;
    bool dbl = false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ':') { ++colons; if (i + 1 < n && s[i + 1] == ':') dbl = true; }
        else if (!IsHex(static_cast<unsigned char>(s[i])) && s[i] != '.') return false;
    }
    return colons >= 2 && (dbl || colons >= 5);
}

bool AllHex(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!IsHex(static_cast<unsigned char>(s[i]))) return false;
    return n > 0;
}

// A long random-looking token (a bearer token is 43 base64url characters, the console key 32 hex digits): letters and digits, 32 or more,
// only [A-Za-z0-9_-].
bool IsLongToken(const char* s, size_t n) {
    if (n < 32) return false;
    bool letter = false, digit = false;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (isalpha(c)) letter = true;
        else if (isdigit(c)) digit = true;
        else if (c != '_' && c != '-') return false;
    }
    return letter && digit;
}

bool IsBlobChar(unsigned char c) { return isalnum(c) || c == '+' || c == '/' || c == '=' || c == '_' || c == '-'; }

// The length of a base64 / base64url / hex blob that starts at s[0] (0 when it is not one): 32 or more of [A-Za-z0-9+/=_-] with a letter and a
// digit. Standard base64 uses '/' which paths use too, so at most two slashes are allowed in a blob (a path of four folders is not a secret).
size_t BlobLen(const char* s, size_t n) {
    size_t k = 0;
    int slashes = 0;
    bool letter = false, digit = false;
    while (k < n && IsBlobChar(static_cast<unsigned char>(s[k]))) {
        if (s[k] == '/') ++slashes;
        if (isalpha(static_cast<unsigned char>(s[k]))) letter = true;
        if (isdigit(static_cast<unsigned char>(s[k]))) digit = true;
        ++k;
    }
    if (k < 32 || slashes > 2 || !letter || !digit) return 0;
    return k;
}

// A host name without a scheme ("my-galaxy.example.org", "nas.lan:8443"): dotted labels whose last label is a known top-level domain. The list is
// on purpose short and common: file names (mp.cfg, GameData.bin), versions (3.0.0) and dotted words are never matched.
bool IsHostname(const char* s, size_t n) {
    static const char* const kTld[] = {"com", "net", "org", "io", "dev", "app", "gg", "tv", "me", "xyz", "info", "biz", "co", "uk", "de", "fr", "jp", "us",
                                       "ca", "au", "nl", "eu", "ru", "cn", "kr", "br", "it", "es", "se", "no", "fi", "pl", "ch", "at", "be", "cz", "nz",
                                       "local", "lan", "home", "internal", "localdomain", "cloud", "online", "site", "tech", "link", "page", "workers",
                                       "pages", "run", "sh", "ws", nullptr};
    size_t e = n;
    size_t pe = e;
    while (pe > 0 && isdigit(static_cast<unsigned char>(s[pe - 1]))) --pe;   // ":port"
    if (pe < e && pe > 0 && s[pe - 1] == ':' && e - pe <= 5) e = pe - 1;
    if (e < 4) return false;
    int labels = 0;
    size_t ls = 0;
    for (size_t i = 0; i <= e; i++) {
        if (i == e || s[i] == '.') {
            const size_t l = i - ls;
            if (l < 1 || l > 63 || s[ls] == '-' || s[i - 1] == '-') return false;
            ++labels;
            if (i < e) { ls = i + 1; continue; }
            if (labels < 2 || l < 2) return false;
            for (size_t q = ls; q < e; q++)
                if (!isalpha(static_cast<unsigned char>(s[q]))) return false;
            for (int t = 0; kTld[t]; t++)
                if (strlen(kTld[t]) == l && strncasecmp(s + ls, kTld[t], l) == 0) return true;
            return false;
        }
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (!(isalnum(c) || c == '-')) return false;
    }
    return false;
}

// Words that hide their value, whatever the spelling (key=value, key = value, key: value, "key":"value"; any case). Class A: the value goes unless
// it is empty or "-". Class B: a person's or a machine's name may stand there, but a small number (a slot, a count) is kept: "host=1" stays,
// "host=nas.lan" goes. `code` is special: only a service error word survives (ErrorCodeWord). For "name" the value runs to the next " key=" of the
// known set (or the end of the line): a player's name may contain spaces.
const char* const kHideKeys[] = {"name", "console", "friend", "room", "token", "key", "console_key", "secret", "pass", "password", nullptr};
const char* const kColonKeys[] = {"name", "console", "friend", "room", "token", "key", "console_key", "secret", "password", "code", nullptr};
const char* const kNumKeys[] = {"host", "peer", "player", "user", "ssid", "owner", "nick", "nickname", nullptr};
const char* const kNameStops[] = {"look", "rejoin", "players", "conn", "console", "session", "slot", "source", "host", "mode", "visibility", "proto",
                                  "fingerprint", "compatible", "epoch", "reason", "build", nullptr};

bool KeyIn(const char* const* list, const char* k, size_t kl) {
    for (int h = 0; list[h]; h++)
        if (strlen(list[h]) == kl && strncasecmp(k, list[h], kl) == 0) return true;
    return false;
}

// After "name=" at s[0..left): the length of the value to hide.
size_t NameValueLen(const char* s, size_t left) {
    for (size_t i = 0; i < left; i++) {
        if (s[i] != ' ') continue;
        for (int k = 0; kNameStops[k]; k++) {
            const size_t l = strlen(kNameStops[k]);
            if (Starts(s + i + 1, left - i - 1, kNameStops[k]) && i + 1 + l < left && s[i + 1 + l] == '=') return i;
        }
    }
    return left;
}

// A hidden-key pair at s[i]: key, optional closing quote, separator '=' or ':', value. Returns the new position (0: not one).
size_t ScrubKeyValue(const char* s, size_t n, size_t i, Out& o) {
    const bool quoted = s[i] == '"';   // JSON: "key": "value"
    const size_t ks = quoted ? i + 1 : i;
    if (ks >= n || !isalpha(static_cast<unsigned char>(s[ks]))) return 0;
    size_t k = ks;
    while (k < n && IsWordChar(static_cast<unsigned char>(s[k]))) ++k;
    size_t q = k;
    if (quoted) {
        if (q >= n || s[q] != '"') return 0;
        ++q;
    }
    size_t eq = q;
    while (eq < n && s[eq] == ' ') ++eq;
    if (eq >= n || (s[eq] != '=' && s[eq] != ':')) return 0;
    const bool colon = s[eq] == ':';
    if (colon && eq + 1 < n && s[eq + 1] == ':') return 0;   // "::"
    const size_t kl = k - ks;
    const bool isCode = kl == 4 && strncasecmp(s + ks, "code", 4) == 0;
    const bool classA = KeyIn(kHideKeys, s + ks, kl) || isCode;
    const bool classB = KeyIn(kNumKeys, s + ks, kl);
    if (!classA && !classB) return 0;
    if (colon && !quoted && !KeyIn(kColonKeys, s + ks, kl)) return 0;   // (a plain "host: x" is ordinary text)
    const bool isName = kl == 4 && strncasecmp(s + ks, "name", 4) == 0;
    size_t vs = eq + 1;
    if (colon || eq != q) while (vs < n && s[vs] == ' ') ++vs;   // "key: v" and "key = v"
    o.putn(s + i, vs - i);
    // the value: quoted (JSON: to the closing quote) or plain (to a space, comma, bracket)
    size_t vb = vs, ve = vs;
    const bool jq = vs < n && s[vs] == '"';
    if (jq) {
        vb = vs + 1;
        ve = vb;
        while (ve < n && s[ve] != '"') ve += (s[ve] == '\\' && ve + 1 < n) ? 2 : 1;
        if (ve > n) ve = n;
    } else {
        if (isName) ve = vs + NameValueLen(s + vs, n - vs);
        else while (ve < n && s[ve] != ' ' && s[ve] != ',' && s[ve] != ')' && s[ve] != '\t' && s[ve] != '}' && s[ve] != ']' && s[ve] != '"') ++ve;
        if (classB && !classA) {   // a name that may hold spaces, unless it is a small number
            bool num = ve - vs <= 3;
            for (size_t t = vs; t < ve && num; t++) if (!isdigit(static_cast<unsigned char>(s[t]))) num = false;
            if (!num && ve - vs > 0) ve = vs + NameValueLen(s + vs, n - vs);
        }
    }
    const size_t vl = ve - vb;
    bool keep = vl == 0 || (vl == 1 && s[vb] == '-');
    if (isCode && ErrorCodeWord(s + vb, vl)) keep = true;
    if (classB && !classA && vl <= 3) {
        bool num = true;
        for (size_t t = 0; t < vl; t++) if (!isdigit(static_cast<unsigned char>(s[vb + t]))) num = false;
        if (num) keep = true;
    }
    if (jq) o.put('"');
    if (keep) o.putn(s + vb, vl);
    else o.puts("<hidden>");
    if (jq) {
        if (ve < n) o.put('"');
        return ve < n ? ve + 1 : n;
    }
    return ve;
}

// A token's class: the replacement text ("<address>", "<secret>", "<code>", "<host>") or null.
const char* ClassifyToken(const char* core, size_t cl, const Out& o) {
    if (cl == 0) return nullptr;
    if (LooksLikeFriendCode(core, cl)) return "<code>";
    if (LooksLikeCode(core, cl) && (EndsWithWord(o, "joining") || EndsWithWord(o, "room") || EndsWithWord(o, "friend") || EndsWithWord(o, "code")))
        return "<code>";
    if (IsIpv4Token(core, cl) || IsIpv6Token(core, cl)) return "<address>";
    if (cl >= 32 && AllHex(core, cl)) return "<secret>";
    if (cl >= 10 && cl <= 14 && AllHex(core, 8) && core[8] == ':') {   // "0a000105:41100": the core's hex form of host:port
        bool digits = true;
        for (size_t q = 9; q < cl; q++) if (!isdigit(static_cast<unsigned char>(core[q]))) digits = false;
        if (digits) return "<address>";
    }
    if (cl == 8 && AllHex(core, cl) && o.n >= 8 && memcmp(o.p + o.n - 8, "address ", 8) == 0) return "<address>";
    if (cl >= 1 && (EndsWithWord(o, "Bearer") || EndsWithWord(o, "bearer"))) return "<secret>";
    if (IsLongToken(core, cl)) return "<secret>";
    if (IsHostname(core, cl)) return "<host>";
    return nullptr;
}

// An account number written after one of these words, with or without '=' or ':' ("login ok: profile 100003", "PID=4711", "uid: 99999"): the
// digits (4 or more; 4-12 is the real range, a longer run fails closed too) are hidden. Returns the new position (0: not one).
// Strikers found this leak on 18 Nov: its scrubber only knew "key=value" pairs.
const char* const kIdWords[] = {"profile", "pid", "account", "uid", "player", nullptr};
size_t ScrubIdNumber(const char* s, size_t n, size_t i, Out& o) {
    size_t k = i;
    while (k < n && IsWordChar(static_cast<unsigned char>(s[k]))) ++k;
    if (!KeyIn(kIdWords, s + i, k - i)) return 0;
    size_t p = k;
    while (p < n && s[p] == ' ') ++p;
    if (p < n && (s[p] == '=' || s[p] == ':') && !(s[p] == ':' && p + 1 < n && s[p + 1] == ':')) { ++p; while (p < n && s[p] == ' ') ++p; }
    size_t d = p;
    while (d < n && isdigit(static_cast<unsigned char>(s[d]))) ++d;
    if (d - p < 4) return 0;   // ("pid4711" is one word: never matches the list)
    if (d < n && isalpha(static_cast<unsigned char>(s[d]))) return 0;   // "uid 1234abcd" is something else
    o.putn(s + i, p - i);
    o.puts("<hidden>");
    return d;
}

void ScrubLineCore(const char* s, size_t n, Out& o) {
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const bool wordStart = i == 0 || !IsWordChar(static_cast<unsigned char>(s[i - 1]));
        // 0. profile / pid / account / uid / player followed by a number
        if (wordStart && isalpha(c)) {
            const size_t r = ScrubIdNumber(s, n, i, o);
            if (r) { i = r; continue; }
        }
        // 1. key=value, key: value, "key":"value"
        if (wordStart && (isalpha(c) || c == '"')) {
            const size_t r = ScrubKeyValue(s, n, i, o);
            if (r) { i = r; continue; }
        }
        // 2. URLs: the host is hidden, the rest of the token too (paths may carry room codes)
        if (c == 'h' && (Starts(s + i, n - i, "https://") || Starts(s + i, n - i, "http://"))) {
            const size_t sl = s[i + 4] == 's' ? 8 : 7;
            o.putn(s + i, sl);
            o.puts("<host>");
            i += sl;
            while (i < n && s[i] != ' ' && s[i] != '\t' && s[i] != ')' && s[i] != ',' && s[i] != '"') ++i;
            continue;
        }
        // 3. the user's folder name
        if (c == '/' || c == 'C') {
            static const char* const kHomes[] = {"/home/", "/Users/", "C:\\Users\\", "C:/Users/", nullptr};
            bool done = false;
            for (int h = 0; kHomes[h] && !done; h++) {
                if (Starts(s + i, n - i, kHomes[h])) {
                    const size_t hl = strlen(kHomes[h]);
                    o.putn(s + i, hl);
                    o.puts("<user>");
                    i += hl;
                    while (i < n && s[i] != '/' && s[i] != '\\' && s[i] != ' ' && s[i] != '\t' && s[i] != '"') ++i;
                    done = true;
                }
            }
            if (done) continue;
        }
        // 4a. "[addr]" and "[addr]:port"
        if (c == '[') {
            size_t e = i + 1;
            while (e < n && s[e] != ']' && s[e] != ' ' && e - i < 48) ++e;
            if (e < n && s[e] == ']' && (IsIpv6Token(s + i + 1, e - i - 1) || IsIpv4Token(s + i + 1, e - i - 1) || IsHostname(s + i + 1, e - i - 1) || LooksLikeFriendCode(s + i + 1, e - i - 1))) {
                ++e;
                if (e < n && s[e] == ':') {
                    size_t d = e + 1;
                    while (d < n && isdigit(static_cast<unsigned char>(s[d]))) ++d;
                    if (d > e + 1) e = d;
                }
                o.puts("<address>");
                i = e;
                continue;
            }
        }
        if (c == '[') { o.put('['); ++i; continue; }   // (not an address: the text inside is looked at on its own)
        // 3b. base64 / hex blobs of token length (they use '+' '/' '=' that tokens do not)
        if (IsBlobChar(c) && (i == 0 || !IsBlobChar(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '/')) {   // (also after each '/': a blob inside a path)
            const size_t bl = BlobLen(s + i, n - i);
            if (bl) { o.puts("<secret>"); i += bl; continue; }
        }
        // 4. tokens: addresses, ids, secrets
        if (IsTokenChar(c)) {
            size_t k = i;
            while (k < n && IsTokenChar(static_cast<unsigned char>(s[k]))) ++k;
            const char* t = s + i;
            size_t tl = k - i;
            // brackets around an address: "[::1]:41100" / "[1.2.3.4]"
            const char* core = t;
            size_t cl = tl;
            if (cl && core[0] == '[') { ++core; --cl; }
            while (cl && (core[cl - 1] == '.' || core[cl - 1] == ':' || core[cl - 1] == '-' || core[cl - 1] == ']')) --cl;   // a sentence's full stop or colon after the token
            const size_t trail = tl - (static_cast<size_t>(core - t) + cl);
            const char* repl = ClassifyToken(core, cl, o);
            size_t lead = 0;   // text kept in front of the hidden part: "peer:1.2.3.4:41100", "ip-10.0.0.5"
            if (!repl && cl <= 90) {
                for (size_t j = 1; j < cl && !repl; j++) {
                    if (core[j - 1] != ':' && core[j - 1] != '=' && core[j - 1] != '-' && core[j - 1] != '_') continue;
                    repl = ClassifyToken(core + j, cl - j, o);
                    if (repl) lead = j;
                }
            }
            // a blob glued to a key: "peer:dGhpcy+pcyBh..." (its '+' and '/' end the token above)
            bool found = false;
            if (!repl) {
                for (size_t j = 1; j < tl && !found; j++) {
                    if (t[j - 1] != ':' && t[j - 1] != '=') continue;
                    const size_t bl = BlobLen(t + j, n - (i + j));
                    if (bl) {
                        o.putn(t, j);
                        o.puts("<secret>");
                        i += j + bl;
                        found = true;
                        break;
                    }
                }
            }
            if (found) continue;
            // a code that is a path segment: ".../friends/KQ7M2XZA"
            if (!repl && i > 0 && s[i - 1] == '/' && LooksLikeCode(core, cl)) repl = "<code>";
            if (repl) {
                o.putn(t, static_cast<size_t>(core - t) + lead);
                o.puts(repl);
                o.putn(t + (tl - trail), trail);
            } else {
                o.putn(t, tl);
            }
            i = k;
            continue;
        }
        o.put(static_cast<char>(c));
        ++i;
    }
}

// beta diagnostics: e-mail addresses ("name@host.tld") become "<email>" before the line goes through the scrubber above.
bool IsMailLocal(unsigned char c) { return isalnum(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-'; }
bool IsMailDomain(unsigned char c) { return isalnum(c) || c == '.' || c == '-'; }

void ScrubLine(const char* s, size_t n, Out& o) {
    if (!memchr(s, '@', n)) { ScrubLineCore(s, n, o); return; }
    char* t = static_cast<char*>(malloc(n * 2 + 16));
    if (!t) { ScrubLineCore(s, n, o); return; }
    size_t tn = 0;
    size_t i = 0;
    while (i < n) {
        if (s[i] == '@') {
            size_t ls = tn;
            while (ls > 0 && IsMailLocal(static_cast<unsigned char>(t[ls - 1]))) --ls;
            size_t de = i + 1;
            while (de < n && IsMailDomain(static_cast<unsigned char>(s[de]))) ++de;
            while (de > i + 1 && (s[de - 1] == '.' || s[de - 1] == '-')) --de;   // a sentence's full stop after the address
            // the domain needs a dot and a last label of two or more letters
            size_t dot = de;
            while (dot > i + 1 && s[dot - 1] != '.') --dot;
            const bool tld = dot > i + 1 && de - dot >= 2;
            bool letters = tld;
            for (size_t q = dot; q < de && letters; q++) if (!isalpha(static_cast<unsigned char>(s[q]))) letters = false;
            if (tn > ls && letters) {
                tn = ls;
                memcpy(t + tn, "<email>", 7);
                tn += 7;
                i = de;
                continue;
            }
        }
        t[tn++] = s[i++];
    }
    ScrubLineCore(t, tn, o);
    free(t);
}

// ---- which log lines a report may carry ----------------------------------------------------------------------------------------
// A scrubber is a denylist: it removes what it knows is private and leaks whatever the next log line invents. So the report is built the other
// way round as well: a "[mp]" line is included only when its text starts with a prefix of kMpAllow (families of lines written by code that was
// read for this: they carry slots, counts, states, error words, galaxy names, and values the scrubber hides by key) and not with a prefix of
// kMpDeny (lines inside an allowed family that print another player's name or text). Every other "[mp]" line, and any family added to the code
// later, is left out until somebody adds it here. '#' in a prefix matches one or more digits. The network layer's own lines ([np-net], [net-https],
// [vlink*], [online]) never know a player's name: they print addresses and host names, which the scrubber removes; they are kept whole.
const char* const kMpAllow[] = {
    "ui: ", "flow ", "save ", "puppet ", "bubble ", "coop ", "race ", "world ", "starbits ", "service ", "direct: ", "join start", "join refused", "entry: ",
    "config: ", "team ", "spectate", "slot # joined", "slot # left", "slot # closed", "slot # send", "slot # says", "slot # resumed", "online: ", "link: ",
    "formation ", "hello ", "warm ", "upnp: ", "natpmp: ", "snapshot ", "net: ", "host ", "star ", "scan: ", "scan start", "head3d: ", "costar ",
    "conntest: ", "session ", "keyboard ", "helper ", "chat ", "settings ", "roster: slot # left", "roster: slot # joined", "quick match", "quality",
    "merge: ", "look ", "kicked slot=", "game ", "core ", "update: ", "threads: ", "suspend: ", "status ", "rejoin", "report ", "resume: ", "reconnect",
    "queue ", "poselog", "perf ", "prof ", "out of memory", "leaving", "joined ", "hosting ", "following ", "events lost", "dropped from", "cores: ",
    "catch-up", "boot ", "bad message", "rejected by host", "watch: ", "auto: host ", "auto: join by address", "diag: ", nullptr};
const char* const kMpDeny[] = {"ui: join room", "ui: toast", nullptr};

bool MatchPrefix(const char* s, size_t n, const char* pat) {
    size_t i = 0;
    for (const char* p = pat; *p; ++p) {
        if (*p == '#') {
            if (i >= n || !isdigit(static_cast<unsigned char>(s[i]))) return false;
            while (i < n && isdigit(static_cast<unsigned char>(s[i]))) ++i;
        } else {
            if (i >= n || s[i] != *p) return false;
            ++i;
        }
    }
    return true;
}

bool MpLineAllowed(const char* msg, size_t n) {
    for (int d = 0; kMpDeny[d]; d++)
        if (MatchPrefix(msg, n, kMpDeny[d])) return false;
    for (int a = 0; kMpAllow[a]; a++)
        if (MatchPrefix(msg, n, kMpAllow[a])) return true;
    return false;
}

// 1: keep, 0: not ours, -1: ours but not on the allowlist (counted in the report).
int ClassifyLogLine(const char* s, size_t n) {
    static const char* const kTags[] = {"[mp] ", "[np-net]", "[net-https]", "[vlink", "[online]", nullptr};
    for (int t = 0; kTags[t]; t++) {
        const size_t l = strlen(kTags[t]);
        for (size_t i = 0; i + l <= n; i++) {
            if (memcmp(s + i, kTags[t], l) != 0) continue;
            if (t > 0) return 1;
            return MpLineAllowed(s + i + l, n - i - l) ? 1 : -1;
        }
    }
    return 0;
}

}  // namespace

extern "C" size_t mp_scrub(const char* in, size_t n, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    Out o{out, cap, 0};
    if (in) {
        size_t i = 0;
        while (i < n) {
            size_t e = i;
            while (e < n && in[e] != '\n') ++e;
            ScrubLine(in + i, e - i, o);
            if (e < n) o.put('\n');
            i = e + 1;
        }
    }
    out[o.n] = 0;
    return o.n;
}

// beta diagnostics: the "log" of an uploaded problem report. The same filter (only the lines of kMpAllow and the network layer's own tags) and the
// same scrubber (mp_scrub, plus e-mail addresses) as the report file, over the previous session's log (prev, may be null) then this session's
// (cur). Both are the raw tail of a log file: `*IsTail` drops the first, probably partial, line. When the result is larger than cap - 1 bytes
// the OLDEST lines go (whole lines only), so the newest always stay. out is NUL terminated; returns the length (< cap).
extern "C" size_t mp_report_log_payload(const char* prev, size_t prevLen, bool prevIsTail, const char* cur, size_t curLen, bool curIsTail, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    const size_t tot = (prev ? prevLen : 0) + (cur ? curLen : 0);
    if (!tot) return 0;
    const size_t tcap = tot * 4 + 64;
    char* t = static_cast<char*>(malloc(tcap));
    if (!t) return 0;
    Out lo{t, tcap, 0};
    for (int pass = 0; pass < 2; pass++) {
        const char* s = pass == 0 ? prev : cur;
        size_t n = pass == 0 ? prevLen : curLen;
        const bool isTail = pass == 0 ? prevIsTail : curIsTail;
        if (!s || !n) continue;
        if (isTail) {
            const char* nl = static_cast<const char*>(memchr(s, '\n', n));
            if (!nl) continue;
            n -= static_cast<size_t>(nl + 1 - s);
            s = nl + 1;
        }
        size_t i = 0;
        while (i < n) {
            size_t e = i;
            while (e < n && s[e] != '\n') ++e;
            if (ClassifyLogLine(s + i, e - i) > 0) {
                ScrubLine(s + i, e - i, lo);
                lo.put('\n');
            }
            i = e + 1;
        }
    }
    size_t skip = 0;
    if (lo.n > cap - 1) {
        skip = lo.n - (cap - 1);
        while (skip < lo.n && t[skip - 1] != '\n') ++skip;   // (to a line start)
    }
    const size_t len = lo.n - skip;
    memcpy(out, t + skip, len);
    out[len] = 0;
    free(t);
    return len;
}

extern "C" size_t mp_report_build(char* out, size_t cap, const MpReportParts& p) {
    if (!out || cap == 0) return 0;
    Out o{out, cap, 0};
    char line[256];
    o.puts("Super Mario Galaxy Vita: multiplayer report\n");
    o.puts("Nothing here names you or your network: addresses, codes, names and keys are removed.\n\n");
    o.puts("[build]\n");
    snprintf(line, sizeof line, "version = %s\nbuild = %s\nfingerprint = %08x\nprotocol = %d\n\n", p.appVersion ? p.appVersion : "-",
             p.buildTag ? p.buildTag : "-", p.fingerprint, p.protocol);
    o.puts(line);
    o.puts("[settings]\n");
    if (p.settings) {
        char tmp[1024];
        mp_scrub(p.settings, strlen(p.settings), tmp, sizeof tmp);
        o.puts(tmp);
    }
    o.puts("\n[connection test]\n");
    if (p.conntest && p.conntestLen) {
        // (the whole text, scrubbed, into a scratch buffer on the stack-free heap path of the caller: here it is bounded by cap)
        const size_t room = o.cap > o.n + 8 ? o.cap - o.n - 8 : 0;
        char* t = static_cast<char*>(malloc(room + 1));
        if (t) {
            const size_t l = mp_scrub(p.conntest, p.conntestLen, t, room + 1);
            o.putn(t, l);
            if (l && t[l - 1] != '\n') o.put('\n');
            free(t);
        }
    } else {
        o.puts("none yet\n");
    }
    o.puts("\n[log: multiplayer and network lines, newest last]\n");
    // log lines: whole lines only, only ours, scrubbed; when everything does not fit the oldest lines go
    if (p.log && p.logLen) {
        const char* s = p.log;
        size_t n = p.logLen;
        if (p.logIsTail) {   // drop the first, probably partial, line
            const char* nl = static_cast<const char*>(memchr(s, '\n', n));
            if (!nl) { s += n; n = 0; } else { n -= static_cast<size_t>(nl + 1 - s); s = nl + 1; }
        }
        const size_t room = o.cap > o.n + 1 + 100 ? o.cap - o.n - 1 - 100 : 0;   // (100 bytes kept for the note on left-out lines)
        char* t = static_cast<char*>(malloc(n * 2 + 16));
        if (t && room) {
            Out lo{t, n * 2 + 16, 0};
            size_t leftOut = 0;
            size_t i = 0;
            while (i < n) {
                size_t e = i;
                while (e < n && s[e] != '\n') ++e;
                const int keep = ClassifyLogLine(s + i, e - i);
                if (keep > 0) {
                    ScrubLine(s + i, e - i, lo);
                    lo.put('\n');
                } else if (keep < 0) {
                    ++leftOut;
                }
                i = e + 1;
            }
            size_t skip = 0;
            if (lo.n > room) {   // keep the newest lines that fit: skip to a line start
                skip = lo.n - room;
                while (skip < lo.n && t[skip - 1] != '\n') ++skip;
            }
            o.putn(t + skip, lo.n - skip);
            if (leftOut) {
                char note[96];
                snprintf(note, sizeof note, "(%u more multiplayer log lines were left out: not on the report's list)\n", static_cast<unsigned>(leftOut));
                o.puts(note);
            }
        }
        free(t);
    }
    out[o.n] = 0;
    return o.n;
}

// ---- gathering on this console -------------------------------------------------------------------------------------------------

namespace {

const char* DataDir(char* buf, size_t cap) {
#ifdef __vita__
    snprintf(buf, cap, "ux0:data/smg");
#else
    const char* d = getenv("PETARI_DATA_DIR");
    snprintf(buf, cap, "%s", d && *d ? d : ".");
#endif
    return buf;
}

// The last `maxBytes` of a file. *start is true when it begins at the file's start. null (and *n = 0) when the file cannot be read.
char* ReadTail(const char* path, size_t maxBytes, size_t* n, bool* start) {
    *n = 0;
    *start = true;
    // netfix B8b: POSIX calls (the report is built on the helper thread beside the game's own file opens)
    long size = -1;
    char* b = static_cast<char*>(malloc(maxBytes + 1));
    if (!b) return nullptr;
    long got = mp_io_read(path, b, 0, 0, &size);   // (the size first)
    if (got < 0) { free(b); return nullptr; }
    long from = 0;
    if (size > static_cast<long>(maxBytes)) { from = size - static_cast<long>(maxBytes); *start = false; }
    got = size > 0 ? mp_io_read(path, b, maxBytes, from) : 0;
    if (got < 0) got = 0;
    b[got] = 0;
    *n = static_cast<size_t>(got);
    return b;
}

}  // namespace

extern "C" bool mp_report_save(const char* dir, char* path, size_t pathCap) {
    MP_HOST_ALLOC;
    char dd[200];
    if (!dir || !*dir) dir = DataDir(dd, sizeof dd);
    char file[256];
    snprintf(file, sizeof file, "%s/report.txt", dir);

    // settings: no name, no server address, no console key
    char settings[800];
    settings[0] = 0;
    if (mp_config) {
        const MpConfig* c = mp_config();
        snprintf(settings, sizeof settings,
                 "look = %d\nconnection = %d\nmode = %d\nmax_players = %d\ninteraction = %d\nshared_kills = %d\nname_tags = %d\nrivals = %d\n"
                 "race_wait = %d\nvisibility = %d\nrelay_only = %d\ndrop_in = %d\nfalls = %d\nstun = %d\ncustom_server = %s\n",
                 c->look, c->conn, c->mode, c->maxPlayers, c->interaction, c->sharedKills, c->nameTags, c->rivals, c->raceWaitSec, c->visibility,
                 c->relayOnly, c->dropIn, c->downed, c->stun, c->server[0] ? "yes" : "no");
    }
    char logPath[256], ctPath[256];
    snprintf(logPath, sizeof logPath, "%s/log.txt", dir);
    snprintf(ctPath, sizeof ctPath, "%s/mp/conntest.txt", dir);
    size_t logLen = 0, ctLen = 0;
    bool logStart = true, ctStart = true;
    char* log = ReadTail(logPath, MP_REPORT_LOG_KB * 1024, &logLen, &logStart);
    char* ct = ReadTail(ctPath, MP_REPORT_CONNTEST_KB * 1024, &ctLen, &ctStart);

    MpReportParts parts;
    memset(&parts, 0, sizeof parts);
    parts.appVersion = mp_service_app_version ? mp_service_app_version() : "3.0.0";
    parts.buildTag = __DATE__ " " __TIME__;
    parts.fingerprint = mp_core_fingerprint ? mp_core_fingerprint() : 0;
    parts.protocol = MP_PROTO_VERSION;
    parts.settings = settings;
    parts.log = log;
    parts.logLen = logLen;
    parts.logIsTail = !logStart;
    parts.conntest = ct;
    parts.conntestLen = ctLen;
    const size_t cap = (MP_REPORT_LOG_KB + MP_REPORT_CONNTEST_KB + 4) * 1024;
    char* text = static_cast<char*>(malloc(cap));
    bool ok = false;
    if (text) {
        const size_t n = mp_report_build(text, cap, parts);
        char tmp[270];
        snprintf(tmp, sizeof tmp, "%s.tmp", file);
        ok = mp_io_write(tmp, text, n);
        if (ok) {
            remove(file);   // (rename over an existing file is not allowed on every target)
            ok = rename(tmp, file) == 0;
        }
        if (!ok) remove(tmp);
        free(text);
    }
    free(log);
    free(ct);
    if (ok && path && pathCap) snprintf(path, pathCap, "%s", file);
    if (mp_log) mp_log("report %s", ok ? "saved" : "could not be written");
    return ok;
}
