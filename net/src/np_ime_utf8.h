// UTF-16 (the Vita keyboard's SceWChar16 buffer) to UTF-8, for the keyboard result (np_online_ime_vita.cpp). Header-only, no platform headers, so
// the host test (net/tests/ime_utf8_test.cpp, run by net/tests/run_online_tests.sh) runs the very code the Vita build uses.
// A high surrogate (D800..DBFF) followed by a low one (DC00..DFFF) becomes one 4-byte sequence; a lone surrogate (either kind, or a high one at the
// end of the text) is dropped, never written as an invalid 3-byte sequence. Output is always NUL-terminated and valid UTF-8; at most capacity-1 bytes.
#pragma once
#include <stddef.h>
#include <stdint.h>

static inline void NpUtf16ToUtf8(const uint16_t* in, char* out, size_t capacity) {
    if (!out || capacity == 0) return;
    size_t n = 0;
    for (size_t i = 0; in && in[i]; ++i) {
        unsigned v = in[i];
        if (v >= 0xD800 && v <= 0xDBFF) {
            const unsigned lo = in[i + 1];   // (in[i] is not NUL, so in[i + 1] is readable)
            if (lo < 0xDC00 || lo > 0xDFFF) continue;   // a lone high surrogate: dropped (the next unit is looked at on its own)
            v = 0x10000u + ((v - 0xD800u) << 10) + (lo - 0xDC00u);
            ++i;
        } else if (v >= 0xDC00 && v <= 0xDFFF) {
            continue;   // a lone low surrogate: dropped
        }
        const size_t need = v < 0x80 ? 1 : v < 0x800 ? 2 : v < 0x10000 ? 3 : 4;
        if (n + need + 1 > capacity) break;
        if (need == 1) out[n++] = char(v);
        else if (need == 2) { out[n++] = char(0xC0 | (v >> 6)); out[n++] = char(0x80 | (v & 0x3F)); }
        else if (need == 3) { out[n++] = char(0xE0 | (v >> 12)); out[n++] = char(0x80 | ((v >> 6) & 0x3F)); out[n++] = char(0x80 | (v & 0x3F)); }
        else {
            out[n++] = char(0xF0 | (v >> 18)); out[n++] = char(0x80 | ((v >> 12) & 0x3F));
            out[n++] = char(0x80 | ((v >> 6) & 0x3F)); out[n++] = char(0x80 | (v & 0x3F));
        }
    }
    out[n] = 0;
}
