// Galaxy 3.0 multiplayer: the name filter, the game's copy of server/src/core/names.js (nameFilterReason). One list (mp_name_filter_data.inc,
// generated from server/src/core/name_filter.js), one normaliser, and a shared set of test names (server/test/fixtures/name_vectors.txt) that
// both implementations must agree on (tests/mp/name_filter_test.sh, server/test/core/svcfix2-names.test.js).
//
// A name is matched as it SOUNDS, not as typed: look-alike letters and digits are folded ("Sh1t", "Phuck", "Fvck"), separators dropped
// ("F.u.c.k"), repeats collapsed ("Fuuuck"). The game applies it to names that arrive from other consoles (a stranger's console on a local
// Wi-Fi can send anything and there is no service to ask) and to the player's own; a refused name is shown as the character's name, never
// as an error. Header-only, no allocation. The list is data: it is never printed or logged.
#ifndef MP_NAME_FILTER_H
#define MP_NAME_FILTER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mp_name_filter_data.inc"

namespace mp_nf {

enum Reason { OK = 0, FILTERED = 1, RESERVED = 2 };

static const int kMaxFold = 96;   // characters looked at (a name has at most 10; a hostile packet is cut here)

// next UTF-8 code point (invalid bytes: skipped as 0xFFFFFFFF)
inline uint32_t Next(const uint8_t* s, size_t len, size_t* i) {
    const uint8_t c = s[*i];
    if (c < 0x80) { ++*i; return c; }
    if (c >= 0xC2 && c < 0xE0 && *i + 1 < len && (s[*i + 1] & 0xC0) == 0x80) { const uint32_t v = uint32_t(c & 0x1F) << 6 | (s[*i + 1] & 0x3F); *i += 2; return v; }
    if (c >= 0xE0 && c < 0xF0 && *i + 2 < len && (s[*i + 1] & 0xC0) == 0x80 && (s[*i + 2] & 0xC0) == 0x80) {
        const uint32_t v = uint32_t(c & 0x0F) << 12 | uint32_t(s[*i + 1] & 0x3F) << 6 | (s[*i + 2] & 0x3F); *i += 3; return v;
    }
    ++*i;
    return 0xFFFFFFFFu;
}

inline char Wide(uint32_t c) {
    switch (c) {
    case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe4: case 0xe5: return 'a';
    case 0xe7: return 'c';
    case 0xe8: case 0xe9: case 0xea: case 0xeb: return 'e';
    case 0xec: case 0xed: case 0xee: case 0xef: return 'i';
    case 0xf1: return 'n';
    case 0xf2: case 0xf3: case 0xf4: case 0xf5: case 0xf6: return 'o';
    case 0xf9: case 0xfa: case 0xfb: case 0xfc: return 'u';
    case 0xfd: case 0xff: return 'y';
    case 0x430: return 'a'; case 0x435: return 'e'; case 0x43e: return 'o'; case 0x440: return 'p';
    case 0x441: return 'c'; case 0x445: return 'x'; case 0x443: return 'y'; case 0x43a: return 'k';
    default: return 0;
    }
}

// one code point -> the folded letter, or 0 (a separator or something unknown)
inline char FoldCp(uint32_t cp) {
    char c = 0;
    if (cp < 0x80) {
        c = char(cp);
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    } else {
        uint32_t lo = cp;
        if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) lo = cp + 0x20;
        else if (cp >= 0x410 && cp <= 0x42F) lo = cp + 0x20;
        c = Wide(lo);
    }
    if (!c) return 0;
    switch (c) {
    case '0': c = 'o'; break; case '1': c = 'l'; break; case '3': c = 'e'; break; case '4': c = 'a'; break; case '5': c = 's'; break;
    case '7': c = 't'; break; case '8': c = 'b'; break; case '9': c = 'g'; break; case '@': c = 'a'; break; case '$': c = 's'; break;
    case '!': c = 'l'; break; case '+': c = 't'; break; case '|': c = 'l'; break;
    default: break;
    }
    if (c < 'a' || c > 'z') return 0;
    return c == 'i' ? 'l' : c == 'v' ? 'u' : c == 'q' ? 'g' : c;
}

// the folded letters of utf8[0..len), separators dropped, "ph" -> "f"; returns the length
inline size_t Fold(const uint8_t* s, size_t len, char* out) {
    char tmp[kMaxFold + 1];
    size_t n = 0;
    for (size_t i = 0; i < len && s[i] && n < size_t(kMaxFold);) {
        const char c = FoldCp(Next(s, len, &i));
        if (c) tmp[n++] = c;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        if (tmp[i] == 'p' && i + 1 < n && tmp[i + 1] == 'h') { out[o++] = 'f'; ++i; }
        else out[o++] = tmp[i];
    }
    out[o] = 0;
    return o;
}
inline size_t FoldStr(const char* s, char* out) { return Fold(reinterpret_cast<const uint8_t*>(s), strlen(s), out); }

// runs longer than `keep` cut to `keep`
inline size_t Collapse(const char* in, size_t n, int keep, char* out) {
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && in[j] == in[i]) ++j;
        const size_t run = j - i;
        const size_t put = run > size_t(keep) ? size_t(keep) : run;
        for (size_t k = 0; k < put; ++k) out[o++] = in[i];
        i = j;
    }
    out[o] = 0;
    return o;
}
inline bool HasDouble(const char* e) { for (size_t i = 0; e[i] && e[i + 1]; ++i) if (e[i] == e[i + 1]) return true; return false; }

struct Views {
    char v[3][kMaxFold + 1];
    size_t n[3];
};
inline void MakeViews(const char* folded, size_t n, Views* w) {
    memcpy(w->v[0], folded, n + 1); w->n[0] = n;
    w->n[1] = Collapse(folded, n, 2, w->v[1]);
    w->n[2] = Collapse(folded, n, 1, w->v[2]);
}
inline bool Contains(const char* hay, size_t hn, const char* e, size_t en) {
    if (!en || en > hn) return false;
    for (size_t i = 0; i + en <= hn; ++i) if (!memcmp(hay + i, e, en)) return true;
    return false;
}
// entries with a doubled letter are never matched in the all-runs-cut view (view 2)
inline bool ContainsEntry(const Views& w, const char* e) {
    const size_t en = strlen(e); const bool dbl = HasDouble(e);
    for (int i = 0; i < 3; ++i) if ((i < 2 || !dbl) && Contains(w.v[i], w.n[i], e, en)) return true;
    return false;
}
inline bool EqualsEntry(const Views& w, const char* e) {
    const size_t en = strlen(e); const bool dbl = HasDouble(e);
    for (int i = 0; i < 3; ++i) if ((i < 2 || !dbl) && w.n[i] == en && !memcmp(w.v[i], e, en)) return true;
    return false;
}
inline bool StartsWith(const char* w, const char* p) { return !strncmp(w, p, strlen(p)); }

struct Word { char f[kMaxFold + 1]; size_t n; };

inline bool IsAsciiAlnum(uint32_t c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// 0 ok, 1 filtered, 2 reserved
inline int Reason(const char* utf8, size_t len) {
    const uint8_t* s = reinterpret_cast<const uint8_t*>(utf8);
    char whole[kMaxFold + 1];
    const size_t wn = Fold(s, len, whole);
    if (!wn) return OK;
    // the words: code points split at anything that is not ASCII alnum or >= 0xC0, and at a lower-to-upper ASCII case change
    enum { kMaxWords = 24 };
    Word words[kMaxWords * 2];
    int nw = 0;
    {
        uint8_t cur[kMaxFold * 3 + 4]; size_t cl = 0; uint32_t prev = 0;
        auto flush = [&]() {
            if (!cl) return;
            if (nw < kMaxWords * 2 - 1) {
                words[nw].n = Fold(cur, cl, words[nw].f);
                if (words[nw].n) {
                    // also without digits around the word
                    size_t b = 0, e = cl;
                    while (b < e && cur[b] >= '0' && cur[b] <= '9') ++b;
                    while (e > b && cur[e - 1] >= '0' && cur[e - 1] <= '9') --e;
                    const bool changed = (b != 0 || e != cl) && e > b;
                    ++nw;
                    if (changed) { words[nw].n = Fold(cur + b, e - b, words[nw].f); if (words[nw].n) ++nw; }
                }
            }
            cl = 0;
        };
        for (size_t i = 0; i < len && s[i];) {
            const size_t at = i;
            const uint32_t cp = Next(s, len, &i);
            if (cp == 0xFFFFFFFFu) { flush(); prev = 0; continue; }
            const bool wordChar = IsAsciiAlnum(cp) || cp >= 0xC0;
            if (!wordChar) { flush(); prev = 0; continue; }
            if (prev >= 'a' && prev <= 'z' && cp >= 'A' && cp <= 'Z') flush();
            if (cl + (i - at) < sizeof cur) { memcpy(cur + cl, s + at, i - at); cl += i - at; }
            prev = cp;
        }
        flush();
    }
    Views wv[kMaxWords * 2];
    for (int i = 0; i < nw; ++i) MakeViews(words[i].f, words[i].n, &wv[i]);
    Views whv;
    MakeViews(whole, wn, &whv);
    char folded[kMaxFold + 1], plural[kMaxFold + 2];
    // impersonation
    bool reserved = false;
    for (int e = 0; e < kNfReservedWordN && !reserved; ++e) {
        FoldStr(kNfReservedWord[e], folded);
        snprintf(plural, sizeof plural, "%ss", folded);
        for (int i = 0; i < nw && !reserved; ++i) reserved = EqualsEntry(wv[i], folded) || EqualsEntry(wv[i], plural);
    }
    for (int e = 0; e < kNfReservedPrefixN && !reserved; ++e) {
        FoldStr(kNfReservedPrefix[e], folded);
        for (int i = 0; i < nw && !reserved; ++i) reserved = StartsWith(words[i].f, folded);
    }
    for (int e = 0; e < kNfReservedExactN && !reserved; ++e) {
        FoldStr(kNfReservedExact[e], folded);
        reserved = !strcmp(whole, folded);
    }
    if (reserved) return RESERVED;
    for (int e = 0; e < kNfSubstringN; ++e) { FoldStr(kNfSubstring[e], folded); if (ContainsEntry(whv, folded)) return FILTERED; }
    for (int e = 0; e < kNfExactN; ++e) { FoldStr(kNfExact[e], folded); if (EqualsEntry(whv, folded)) return FILTERED; }
    for (int e = 0; e < kNfWordN; ++e) {
        FoldStr(kNfWord[e], folded);
        snprintf(plural, sizeof plural, "%ss", folded);
        for (int i = 0; i < nw; ++i) if (EqualsEntry(wv[i], folded) || EqualsEntry(wv[i], plural)) return FILTERED;
    }
    return OK;
}

}  // namespace mp_nf

#endif
