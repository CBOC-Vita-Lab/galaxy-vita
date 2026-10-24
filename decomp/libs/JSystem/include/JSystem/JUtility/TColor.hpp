#pragma once

#include "Inline.hpp"
#include <revolution/gx.h>

namespace JUtility {
    struct TColor : public GXColor {
    public:
        TColor(u8 r, u8 g, u8 b, u8 a);
        TColor() {
            set(0xffffffff);
        }

        TColor(u32 u32Color) {
            set(u32Color);
        }

        TColor(GXColor color) {
            set(color);
        }

        TColor& operator=(const TColor& rColor);

        void set(u8 cR, u8 cG, u8 cB, u8 cA) {
            r = cR;
            g = cG;
            b = cB;
            a = cA;
        }

        void set(u32 u32Color) {
#if defined(__MWERKS__) || defined(__BIG_ENDIAN__)
            *reinterpret_cast< u32* >(&r) = u32Color;
#else
            r = u8(u32Color >> 24);
            g = u8(u32Color >> 16);
            b = u8(u32Color >> 8);
            a = u8(u32Color);
#endif
        }

        operator u32() const {
            return toUInt32();
        }

        u32 toUInt32() const {
#if defined(__MWERKS__) || defined(__BIG_ENDIAN__)
            return *reinterpret_cast< const u32* >(&r);
#else
            return (u32(r) << 24) | (u32(g) << 16) | (u32(b) << 8) | u32(a);
#endif
        }

        void set(GXColor gxColor) {
            GXColor* temp = this;
            *temp = gxColor;
        }
    } ATTRIBUTE_ALIGN(4);
};  // namespace JUtility

inline JUtility::TColor::TColor(u8 r, u8 g, u8 b, u8 a) {
    this->r = r;
    this->g = g;
    this->b = b;
    this->a = a;
}
