// Color8's u32 view is 0xRRGGBBAA whatever the host's byte order (decomp/include/Game/Util/Color.hpp, GXV_FIX_COLOR8).
// AutoEffectInfo reads the effect list's "#rrggbb" PrmColor / EnvColor with strtoul(...) * 256 and builds a Color8 from the u32;
// on a little-endian host that used to put the alpha byte in r. Usage: color8_byte_order <1|0> (the switch's value the run is under).
#include "Game/Util/Color.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_fail = 0;
#define CHECK(cond)                                                                                                                                  \
    do {                                                                                                                                             \
        if (!(cond)) {                                                                                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                                     \
            ++g_fail;                                                                                                                                \
        }                                                                                                                                            \
    } while (0)

// AutoEffectInfo's str2Color
static u32 Str2Color(const char* s) { return static_cast< u32 >(std::strtoul(s + 1, nullptr, 16) * 256); }

static void CheckBytes(const Color8& c, unsigned r, unsigned g, unsigned b, unsigned a, int line) {
    if (c.r != r || c.g != g || c.b != b || c.a != a) {
        std::fprintf(stderr, "FAIL line %d: got (%u,%u,%u,%u) want (%u,%u,%u,%u)\n", line, c.r, c.g, c.b, c.a, r, g, b, a);
        ++g_fail;
    }
}

int main(int argc, char** argv) {
    const bool fixed = argc > 1 && std::atoi(argv[1]) != 0;
    CHECK(Color8PackedIsRgba() == fixed);
    CHECK(sizeof(Color8) == 4);

    // The colours of the red ground-pound trail (HipDropBlur) and the black-hole death trail (DieBlackHole), and a few more rows of the list.
    struct Row { const char* hex; unsigned r, g, b; };
    static const Row rows[] = {{"#ff270f", 0xFF, 0x27, 0x0F}, {"#273fff", 0x27, 0x3F, 0xFF}, {"#ff1b00", 0xFF, 0x1B, 0x00},
                               {"#0000ff", 0x00, 0x00, 0xFF}, {"#00ff5a", 0x00, 0xFF, 0x5A}, {"#c8c8c8", 0xC8, 0xC8, 0xC8},
                               {"#00001e", 0x00, 0x00, 0x1E}, {"#ffff00", 0xFF, 0xFF, 0x00}, {"#780000", 0x78, 0x00, 0x00}};
    for (const Row& row : rows) {
        const Color8 c(Str2Color(row.hex));
        if (fixed) {
            CheckBytes(c, row.r, row.g, row.b, 0, __LINE__);
        } else {
            // the old layout: r is the low byte of the u32, which str2Color leaves zero
            CheckBytes(c, 0, row.b, row.g, row.r, __LINE__);
        }
    }

    // Round trip and byte order over a sweep of u32 values (a fixed-seed generator and the edge values).
    u32 x = 0x12345678u;
    for (int i = 0; i < 1000000; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5; // xorshift32
        u32 v = i < 5 ? (i == 0 ? 0u : i == 1 ? 0xFFFFFFFFu : i == 2 ? 0x000000FFu : i == 3 ? 0xFF000000u : 0x80808080u) : x;
        const Color8 c(v);
        if (fixed) {
            CHECK(c.r == (v >> 24) && c.g == ((v >> 16) & 0xFF) && c.b == ((v >> 8) & 0xFF) && c.a == (v & 0xFF));
            CHECK(static_cast< u32 >(c) == v);
            const GXColor gc = c;
            CHECK(gc.r == c.r && gc.g == c.g && gc.b == c.b && gc.a == c.a);
            // what the GX gather pipe gets from GXColor1u32(color): the big-endian bytes of the u32, r first
            unsigned char be[4] = {static_cast< unsigned char >(static_cast< u32 >(c) >> 24), static_cast< unsigned char >(static_cast< u32 >(c) >> 16),
                                   static_cast< unsigned char >(static_cast< u32 >(c) >> 8), static_cast< unsigned char >(static_cast< u32 >(c))};
            CHECK(be[0] == c.r && be[1] == c.g && be[2] == c.b && be[3] == c.a);
        } else {
            CHECK(c.mColor == v); // the old behaviour: the u32 is stored as is
        }
        if (g_fail) break;
    }

    // Call sites that build a Color8 from a u32 literal (the console's values are the 0xRRGGBBAA ones).
    if (fixed) {
        CheckBytes(Color8(128), 0, 0, 0, 128, __LINE__);               // ShadowSurfaceDrawer, ShadowSurfaceOval
        CheckBytes(Color8(0x00000080), 0, 0, 0, 0x80, __LINE__);       // DrawUtil
        CheckBytes(Color8(0xE0E0E0FFu), 0xE0, 0xE0, 0xE0, 0xFF, __LINE__); // BigBubbleDrawer
        CheckBytes(Color8(0xFF000000u), 0xFF, 0, 0, 0, __LINE__);
    }

    // Unaffected under both settings: the four-byte constructor, the default, copies, GXColor conversions.
    CheckBytes(Color8(1, 2, 3, 4), 1, 2, 3, 4, __LINE__);
    CheckBytes(Color8(), 255, 255, 255, 255, __LINE__);
    Color8 copy(10, 20, 30, 40);
    Color8 other;
    other = copy;
    CheckBytes(other, 10, 20, 30, 40, __LINE__);
    GXColor g = {9, 8, 7, 6};
    CheckBytes(Color8(g), 9, 8, 7, 6, __LINE__);
    CheckBytes(Color8(0u), 0, 0, 0, 0, __LINE__);
    Color8 z(7, 7, 7, 7);
    z.mColor = 0;
    CheckBytes(z, 0, 0, 0, 0, __LINE__);

    if (g_fail) {
        std::fprintf(stderr, "color8_byte_order: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("color8_byte_order (GXV_FIX_COLOR8=%d): ok\n", fixed ? 1 : 0);
    return 0;
}
