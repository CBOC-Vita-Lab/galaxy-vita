// (From Producdevity's fork, 14cf5aa; public domain. Our writers (port/gd_write.h, forkA's GDPadCurr32) pass it: forkcheck23, 8 Nov.)
// The material writers that DisplayListMaker::diff runs every frame (J3DGDSetTevColorS10, J3DGDSetTevKColor) and
// GDPadCurr32, as built from decomp/src, against the console's byte-by-byte forms: same bytes, same cursor, at every
// alignment and when the overflow check moves the list (tools/test_gd_material_writers.sh).
#ifdef NDEBUG
#error "tests/gd_material_writers.cpp needs assert(): build it without NDEBUG"
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "JSystem/J3DGraphBase/J3DGD.hpp"

static GDLObj* replacement;
static unsigned callbackMode, overflows;
extern "C" void GDOverflowed() {
    ++overflows;
    if (callbackMode == 1) *__GDCurrentDL = *replacement;  // the same object continues in another buffer
    else __GDCurrentDL = replacement;                       // another object becomes current
}

// The original forms (GDBase.h without TARGET_PC, J3DGD.cpp before the cursor was kept in a register).
static void refByte(u8 v) { *__GDCurrentDL->ptr++ = v; }
static void refBP(u32 v) {
    refByte(0x61);
    refByte(v >> 24); refByte(v >> 16); refByte(v >> 8); refByte(v);
}
static void refKColor(GXTevKColorID reg, GXColor c) {
    // BP 0xE0 + 2n / 0xE1 + 2n: red (or blue) in bits 0-10, alpha (or green) in 12-22, bit 23 set for a konstant colour
    const u32 ra = u32(c.r) | u32(c.a) << 12 | 1u << 23 | u32(0xE0 + reg * 2) << 24;
    const u32 bg = u32(c.b) | u32(c.g) << 12 | 1u << 23 | u32(0xE1 + reg * 2) << 24;
    GDOverflowCheck(0xa);
    refBP(ra);
    refBP(bg);
}
static void refColorS10(GXTevRegID reg, GXColorS10 c) {
    const u32 ra = u32(c.r & 0x7FF) | u32(c.a & 0x7FF) << 12 | u32(0xE0 + reg * 2) << 24;
    const u32 bg = u32(c.b & 0x7FF) | u32(c.g & 0x7FF) << 12 | u32(0xE1 + reg * 2) << 24;
    GDOverflowCheck(0x14);
    refBP(ra);
    refBP(bg);
    refBP(bg);
    refBP(bg);
}
static void refPad32() {
    u32 n = ((u32)(uintptr_t)__GDCurrentDL->ptr & 31);
    if (n)
        for (; n < 32; n++) refByte(0);
}

static uint32_t sState = 0x2468ace1;
static uint32_t next() { return sState = sState * 1664525 + 1013904223; }

// Runs one writer twice, real and reference, from identical lists (available < 0: room enough) and compares the bytes
// of both buffers, the overflow callbacks, which list is current and where its cursor stands.
template <typename Real, typename Ref>
static void compare(Real real, Ref ref, unsigned alignment, int available, unsigned mode) {
    struct Side {
        alignas(64) u8 first[256], second[256];
        GDLObj list, other;
        unsigned overflows;
        bool switched;
        ptrdiff_t cursor;
    };
    static Side sides[2];
    for (int i = 0; i < 2; ++i) {
        Side& t = sides[i];
        std::memset(t.first, 0xa5, sizeof(t.first));
        std::memset(t.second, 0xa5, sizeof(t.second));
        t.list = {t.first, 256, t.first + alignment, available < 0 ? t.first + 256 : t.first + alignment + available};
        t.other = {t.second, 256, t.second + alignment, t.second + 256};
        __GDCurrentDL = &t.list;
        replacement = &t.other;
        callbackMode = mode;
        overflows = 0;
        if (i == 0) real(); else ref();
        t.overflows = overflows;
        t.switched = __GDCurrentDL != &t.list;
        t.cursor = __GDCurrentDL->ptr - __GDCurrentDL->start;
    }
    const Side &a = sides[0], &b = sides[1];
    assert(!std::memcmp(a.first, b.first, 256) && !std::memcmp(a.second, b.second, 256));
    assert(a.overflows == b.overflows && a.switched == b.switched && a.cursor == b.cursor);
    assert(a.list.ptr - a.list.start == b.list.ptr - b.list.start && (a.list.start == a.first) == (b.list.start == b.first));
}

int main() {
    unsigned cases = 0;
    for (unsigned i = 0; i < 4000; ++i) {
        const uint32_t x = next(), y = next();
        GXColor k = {u8(x), u8(x >> 8), u8(x >> 16), u8(x >> 24)};
        GXColorS10 s = {s16(x), s16(x >> 16), s16(y), s16(y >> 16)};  // the full s16 range: the writer keeps 11 bits
        if (i < 4) s = {s16(i & 1 ? -1 : 0x7FF), s16(i & 2 ? -1024 : 1023), s16(-2048), s16(2047)};
        const unsigned reg = y % 4, alignment = i % 64;
        for (unsigned mode = 0; mode < 2; ++mode)
            for (int available : {-1, 0, 4, 9, 10, 19, 20}) {
                compare([&] { J3DGDSetTevKColor(GXTevKColorID(reg), k); }, [&] { refKColor(GXTevKColorID(reg), k); }, alignment, available, mode);
                compare([&] { J3DGDSetTevColorS10(GXTevRegID(reg), s); }, [&] { refColorS10(GXTevRegID(reg), s); }, alignment, available, mode);
                cases += 2;
            }
        // several in a row, as a material's TEV block writes them
        compare([&] { for (unsigned r = 0; r < 4; ++r) { J3DGDSetTevColorS10(GXTevRegID(r), s); J3DGDSetTevKColor(GXTevKColorID(r), k); } },
                [&] { for (unsigned r = 0; r < 4; ++r) { refColorS10(GXTevRegID(r), s); refKColor(GXTevKColorID(r), k); } }, alignment, -1, 0);
        ++cases;
    }
    for (unsigned alignment = 0; alignment < 64; ++alignment)
        for (unsigned lead = 0; lead < 3; ++lead) {
            compare([&] { for (unsigned j = 0; j < lead; ++j) J3DGDWriteBPCmd(0x61000000u + j); GDPadCurr32(); },
                    [&] { for (unsigned j = 0; j < lead; ++j) refBP(0x61000000u + j); refPad32(); }, alignment, -1, 0);
            ++cases;
        }
    std::printf("PASS: J3DGDSetTevColorS10, J3DGDSetTevKColor, J3DGDWriteBPCmd and GDPadCurr32 write the console's bytes (%u cases)\n", cases);
}
