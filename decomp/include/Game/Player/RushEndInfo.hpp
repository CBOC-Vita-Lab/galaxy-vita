#pragma once

#include <JSystem/JGeometry/TVec.hpp>

class LiveActor;

class RushEndInfo {
public:
    RushEndInfo(LiveActor*, u32, const TVec3f&, bool, u32);

    /* 0x00 */ u32 _0;
    /* 0x04 */ u32 _4;
    /* 0x08 */ TVec3f _8;
    /* 0x14 */ bool _14;
    /* 0x18 */ u32 _18;
    /* 0x1C */ LiveActor* _1C;
    /* 0x20 */ union {
        u32 _20;
        // (MarioActorRush reads the damage type as (_20 >> 24) & 15. A bit-field's first member is its most significant bits on the
        // Wii and its least significant on a little-endian host, where mDamageType landed in bits 4..7: an enemy that let Mario go
        // with damage, PlayerUtil's endBindAndPlayerDamage / Flip, did none. Fix from Producdevity's fork, 74552f7.)
        struct {
#if defined(__MWERKS__) || defined(__BIG_ENDIAN__)
            u32 _0 : 4;
            u32 mDamageType : 4;
            u32 _8 : 24;
#else
            u32 _8 : 24;
            u32 mDamageType : 4;
            u32 _0 : 4;
#endif
        } mFlags;
    };
};
