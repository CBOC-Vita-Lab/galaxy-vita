// Host test of np_ime_utf8.h (the keyboard result's UTF-16 to UTF-8). Run by net/tests/run_online_tests.sh; by hand from net/: clang++ -std=gnu++11 -Isrc -o /tmp/ime_utf8_test tests/ime_utf8_test.cpp
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "np_ime_utf8.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static std::string conv(const std::vector<uint16_t>& v, size_t cap = 64) {
    std::vector<uint16_t> in(v); in.push_back(0);
    std::vector<char> out(cap + 8, 0x7F);
    NpUtf16ToUtf8(in.data(), out.data(), cap);
    CHECK(out[strlen(out.data())] == 0);
    CHECK(strlen(out.data()) < cap);
    CHECK(out[cap] == 0x7F);   // (nothing written past capacity)
    return out.data();
}
// strict UTF-8 validity (no surrogates, no overlongs, <= U+10FFFF)
static bool validUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > s.size()) return false;
        unsigned cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
        for (int k = 1; k < n; ++k) { if ((s[i + k] & 0xC0) != 0x80) return false; cp = (cp << 6) | (s[i + k] & 0x3F); }
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += n;
    }
    return true;
}

int main() {
    CHECK(conv({'H', 'i'}) == "Hi");
    CHECK(conv({0x00E9}) == "\xC3\xA9");                      // e acute
    CHECK(conv({0x20AC}) == "\xE2\x82\xAC");                  // euro
    CHECK(conv({0xD83D, 0xDE00}) == "\xF0\x9F\x98\x80");      // U+1F600
    CHECK(conv({0xD800, 0xDC00}) == "\xF0\x90\x80\x80");      // U+10000
    CHECK(conv({0xDBFF, 0xDFFF}) == "\xF4\x8F\xBF\xBF");      // U+10FFFF
    CHECK(conv({'a', 0xD83D, 0xDE00, 'b'}) == "a\xF0\x9F\x98\x80" "b");
    CHECK(conv({0xD83D}) == "");                              // lone high at the end
    CHECK(conv({'a', 0xD83D, 'b'}) == "ab");                  // lone high before a normal unit
    CHECK(conv({0xDE00, 'x'}) == "x");                        // lone low
    CHECK(conv({0xD83D, 0xD83D, 0xDE00}) == "\xF0\x9F\x98\x80");   // lone high, then a pair
    CHECK(conv({0xDE00, 0xD83D}) == "");                      // low then high at the end
    // capacity: a 4-byte character that does not fit with the NUL is not written (whole characters only)
    CHECK(conv({'a', 0xD83D, 0xDE00}, 5) == "a");
    CHECK(conv({'a', 0xD83D, 0xDE00}, 6) == "a\xF0\x9F\x98\x80");
    CHECK(conv({'a', 'b', 'c'}, 1) == "");
    { char o[1] = {1}; NpUtf16ToUtf8(nullptr, o, 1); CHECK(o[0] == 0); }
    // every unit alone and in pairs: always valid UTF-8
    for (unsigned a = 0; a < 0x10000; a += (a >= 0xD7F0 && a < 0xE010) ? 1 : 7) {
        CHECK(validUtf8(conv({(uint16_t)a})));
        for (unsigned b : {0x41u, 0xD800u, 0xDBFFu, 0xDC00u, 0xDFFFu, 0x20ACu}) CHECK(validUtf8(conv({(uint16_t)a, (uint16_t)b})));
    }
    printf(fails ? "ime_utf8_test: %d FAILED\n" : "ime_utf8_test: all passed\n", fails);
    return fails ? 1 : 0;
}
