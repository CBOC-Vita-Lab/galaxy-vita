// The checks are assert()s: a build with NDEBUG would pass without testing anything.
#ifdef NDEBUG
#error "tests/gd_write_driver.cpp needs assert(): build it without NDEBUG"
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <unistd.h>
#include "revolution/gd/GDBase.h"

extern "C" {
GDLObj* __GDCurrentDL;
void gd_raw_u16(u16);
void gd_raw_u32(u32);
void gd_checked_u16(u16);
void gd_checked_u32(u32);
void gd_raw_float(f32);
void gd_checked_float(f32);
void gd_raw_bp(u32);
void gd_raw_u8(u8);
}

static unsigned overflows;
static GDLObj* replacement;
static unsigned callbackMode;
extern "C" void GDOverflowed() {
    ++overflows;
    if (callbackMode == 2) return;
    if (callbackMode == 3) {  // the same object starts again at the beginning of its own buffer
        __GDCurrentDL->ptr = __GDCurrentDL->start;
        __GDCurrentDL->top = __GDCurrentDL->start + __GDCurrentDL->length;
        return;
    }
    assert(replacement);
    if (callbackMode == 1) *__GDCurrentDL = *replacement;
    else __GDCurrentDL = replacement;
}

static void verify_bytes() {
    alignas(64) u8 output[256], expected[256];
    for (unsigned alignment = 0; alignment < 64; ++alignment) {
        std::memset(output, 0xa5, sizeof(output));
        std::memset(expected, 0xa5, sizeof(expected));
        GDLObj list{output, sizeof(output), output + alignment, output + sizeof(output)};
        __GDCurrentDL = &list;
        gd_raw_u16(0x1234);
        gd_raw_u32(0x89abcdef);
        gd_checked_u16(0xfedc);
        gd_checked_u32(0x01234567);
        gd_raw_bp(0x76543210);
        const u8 wanted[] = {0x12,0x34,0x89,0xab,0xcd,0xef,0xfe,0xdc,0x01,0x23,0x45,0x67,0x61,0x76,0x54,0x32,0x10};
        std::memcpy(expected + alignment, wanted, sizeof(wanted));
        assert(list.ptr == output + alignment + sizeof(wanted));
        assert(!std::memcmp(output, expected, sizeof(output)));
    }
    uint32_t state = 0x9821abc7;
    const uint32_t special[] = {0,0x80000000,1,0x80000001,0x007fffff,0x00800000,
        0x3f800000,0x7f800000,0xff800000,0x7fc01234,0x7fa01234,0xffa01234,0xffffffff};
    for (unsigned i = 0; i < 20000; ++i) {
        state = state * 1664525 + 1013904223;
        uint32_t value = i < sizeof(special) / sizeof(*special) ? special[i] : state;
        float f;
        std::memcpy(&f, &value, sizeof(f));
        std::memset(output, 0xa5, sizeof(output));
        GDLObj list{output, sizeof(output), output + i % 64, output + sizeof(output)};
        __GDCurrentDL = &list;
        gd_raw_float(f);
        gd_checked_float(f);
        for (unsigned copy = 0; copy < 2; ++copy)
            for (unsigned byte = 0; byte < 4; ++byte)
                assert(output[i % 64 + copy * 4 + byte] == ((value >> (24 - 8 * byte)) & 0xff));
        assert(list.ptr == output + i % 64 + 8);
        assert(output[(i % 64 + 8)] == 0xa5);
        if (i % 64) assert(output[i % 64 - 1] == 0xa5);
    }
    assert(overflows == 0);
    for (unsigned value = 0; value < 65536; ++value) {
        std::memset(output, 0xa5, sizeof(output));
        const unsigned alignment = value % 64;
        GDLObj list{output, sizeof(output), output + alignment, output + sizeof(output)};
        __GDCurrentDL = &list;
        gd_raw_u16(value);
        gd_checked_u16(value);
        assert(output[alignment] == value / 256 && output[alignment + 1] == value % 256);
        assert(output[alignment + 2] == value / 256 && output[alignment + 3] == value % 256);
        assert(list.ptr == output + alignment + 4 && output[alignment + 4] == 0xa5);
        if (alignment) assert(output[alignment - 1] == 0xa5);
    }
}

static void verify_overflow() {
    alignas(64) u8 first[256], second[256];
    for (callbackMode = 0; callbackMode < 4; ++callbackMode)
      for (unsigned kind : {2u, 4u, 5u}) for (unsigned available = 0; available < 8; ++available) {
        const unsigned width = kind == 5 ? 4 : kind;  // 5: a checked f32 (GDWrite_f32), four bytes like a u32
        for (unsigned alignment = 0; alignment < 64; ++alignment) {
            std::memset(first, 0xa5, sizeof(first));
            std::memset(second, 0x5a, sizeof(second));
            GDLObj a{first, sizeof(first), first + alignment, first + alignment + available};
            GDLObj b{second, sizeof(second), second + alignment, second + sizeof(second)};
            __GDCurrentDL = &a;
            replacement = &b;
            overflows = 0;
            if (kind == 2) gd_checked_u16(0x1234);
            else if (kind == 4) gd_checked_u32(0x12345678);
            else { const uint32_t bits = 0x12345678; float f; std::memcpy(&f, &bits, 4); gd_checked_float(f); }
            const bool overflow = width > available;
            assert(overflows == unsigned(overflow));
            if (callbackMode == 3) {
                // reset in place: the bytes go to the start of the same buffer, the other buffer is untouched
                const u8* destination = overflow ? first : first + alignment;
                assert(__GDCurrentDL == &a && a.ptr == destination + width);
                assert(destination[0] == 0x12 && destination[1] == 0x34);
                if (width == 4) assert(destination[2] == 0x56 && destination[3] == 0x78);
                for (unsigned i = 0; i < sizeof(second); ++i) assert(second[i] == 0x5a);
                if (overflow) for (unsigned i = width; i < sizeof(first); ++i) assert(first[i] == 0xa5);
                continue;
            }
            const bool moved = overflow && callbackMode != 2;
            assert(__GDCurrentDL == (moved && callbackMode == 0 ? &b : &a));
            assert(__GDCurrentDL->ptr == (moved ? second : first) + alignment + width);
            const u8* destination = (moved ? second : first) + alignment;
            assert(destination[0] == 0x12 && destination[1] == 0x34);
            if (width == 4) assert(destination[2] == 0x56 && destination[3] == 0x78);
            const u8* untouched = moved ? first : second;
            for (unsigned i = 0; i < sizeof(first); ++i) assert(untouched[i] == (moved ? 0xa5 : 0x5a));
            if (moved && callbackMode == 0) assert(a.ptr == first + alignment);
            assert(destination[width] == (moved ? 0x5a : 0xa5));
            if (alignment) assert(destination[-1] == (moved ? 0x5a : 0xa5));
        }
    }
    overflows = 0;
    replacement = nullptr;
    callbackMode = 0;
}

static void verify_mixed_streams() {
    uint32_t state = 0x4123ba98;
    u8 output[4096], expected[4096];
    for (unsigned alignment = 0; alignment < 64; ++alignment) {
        std::memset(output, 0xa5, sizeof(output));
        std::memset(expected, 0xa5, sizeof(expected));
        GDLObj list{output, sizeof(output), output + alignment, output + sizeof(output)};
        __GDCurrentDL = &list;
        unsigned offset = alignment;
        for (unsigned i = 0; i < 512; ++i) {
            state = state * 1664525 + 1013904223;
            const unsigned kind = (state >> 12) % 8;
            unsigned width = 4;
            float f;
            std::memcpy(&f, &state, sizeof(f));
            switch (kind) {
            case 0: gd_raw_u8(state); width = 1; break;
            case 1: gd_raw_u16(state); width = 2; break;
            case 2: gd_checked_u16(state); width = 2; break;
            case 3: gd_raw_u32(state); break;
            case 4: gd_checked_u32(state); break;
            case 5: gd_raw_float(f); break;
            case 6: gd_checked_float(f); break;
            case 7: gd_raw_bp(state); expected[offset++] = 0x61; break;
            }
            for (unsigned byte = 0; byte < width; ++byte)
                expected[offset++] = state >> (8 * (width - 1 - byte));
            assert(list.ptr == output + offset);
        }
        assert(!std::memcmp(output, expected, sizeof(output)));
    }
}

int main() {
    verify_bytes();
    verify_overflow();
    verify_mixed_streams();
    static u8 bytes[40008];
    GDLObj list{bytes, sizeof(bytes), bytes, bytes + sizeof(bytes)};
    __GDCurrentDL = &list;
#if defined(PORT_GD_COUNT_TEST)
    syscall(0x7f00, 1u);
#endif
    for (unsigned i = 0; i < 10000; ++i) gd_raw_u32(i);
    list.ptr = bytes;
    for (unsigned i = 0; i < 10000; ++i) gd_checked_u32(i);
#if defined(PORT_GD_COUNT_TEST)
    syscall(0x7f00, 2u);
#endif
    assert(list.ptr == bytes + 40000 && overflows == 0);
    std::puts("PASS: real GD/J3D writers, big-endian bytes, float payloads, every alignment and overflow rerouting");
}
