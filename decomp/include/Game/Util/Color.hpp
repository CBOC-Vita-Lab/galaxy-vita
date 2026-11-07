#pragma once

#include <revolution/gx.h>
#include <revolution/types.h>

#ifdef TARGET_PC
#include <stdlib.h>
#include "port/hot_knob.h"

// GXV_FIX_COLOR8 (read once, default 1; 0 = the old layout): the u32 view of a Color8 is 0xRRGGBBAA, as on the console, whatever the
// byte order of the host. The union below puts r in the first byte, which on a little-endian host is the LOW byte of the u32, so
// Color8(0xFF270F00) used to read as r = 0, g = 0x0F, b = 0x27, a = 0xFF (AutoEffectInfo's "#ff270f" colours: the red ground-pound and
// black-hole trails came out dark blue).
static inline bool Color8PackedIsRgba() {
    // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h; every Color8(u32) asks)
    return PORT_HOT_KNOB(bool, [] {
        const char* v = getenv("GXV_FIX_COLOR8"); // (as vgx::EnvInt: unset or empty = 1, else strtol, base 0)
        return (v != nullptr && *v != '\0') ? strtol(v, nullptr, 0) != 0 : true;
    }());
}
#endif

class Color8 {
public:
    Color8() : mColor(-1) {
    }

    Color8(GXColor color) {
        set(color);
    }

    Color8(u8 r, u8 g, u8 b, u8 a) {
        set(r, g, b, a);
    }

    Color8(u32 color) {
#ifdef TARGET_PC
        if (Color8PackedIsRgba()) {
            r = static_cast< u8 >(color >> 24);
            g = static_cast< u8 >(color >> 16);
            b = static_cast< u8 >(color >> 8);
            a = static_cast< u8 >(color);
            return;
        }
#endif
        mColor = color;
    }

    inline operator GXColor() const {
        return mGXColor;
    }

    inline operator u32() const {
#ifdef TARGET_PC
        if (Color8PackedIsRgba()) {
            return (static_cast< u32 >(r) << 24) | (static_cast< u32 >(g) << 16) | (static_cast< u32 >(b) << 8) | a;
        }
#endif
        return mColor;
    }

    inline const Color8& operator=(const GXColor& rOther) {
        mGXColor = rOther;
        return *this;
    }

    inline const Color8& operator=(const Color8& rOther) {
        r = rOther.r;
        g = rOther.g;
        b = rOther.b;
        a = rOther.a;
        return *this;
    }

    void set(u8 _r, u8 _g, u8 _b, u8 _a) {
        r = _r;
        g = _g;
        b = _b;
        a = _a;
    }

    void set(GXColor color) {
        GXColor* const pColor = &mGXColor;
        *pColor = color;
    }

    union {
        struct {
            u8 r;
            u8 g;
            u8 b;
            u8 a;
        };
        u32 mColor;
        GXColor mGXColor;
    };
};

class Color10 {
public:
    Color10() : r(0x3FF), g(0x3FF), b(0x3FF), a(0x3FF) {
    }

    operator GXColorS10() const {
        return mColor;
    }

    union {
        struct {
            u16 r;
            u16 g;
            u16 b;
            u16 a;
        };
        GXColorS10 mColor;
    };
};
