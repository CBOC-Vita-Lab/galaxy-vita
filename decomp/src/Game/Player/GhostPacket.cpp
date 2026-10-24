#include "Game/Player/GhostPacket.hpp"
#include <JSystem/JGeometry/TVec.hpp>
#include <cstdio>

GhostPacket::GhostPacket(void* pData, u32 len) {
    mDataPtr = (u8*)pData;
    mCurOffs = 0;
    _C = len;
}

void GhostPacket::read(u8* pOut, u32 len) {
    for (int i = 0; i < len; i++) {
        *pOut = mDataPtr[mCurOffs];
        pOut++;
        mCurOffs++;
    }
}

#ifdef __MWERKS__
void GhostPacket::read(u32* pOut) {
    read((u8*)pOut, 4);
}

void GhostPacket::read(s16* pOut) {
    read((u8*)pOut, 2);
}
#else
// Ghost data stays big-endian on disk; assemble values byte by byte.
void GhostPacket::read(u32* pOut) {
    u8 b[4];
    read(b, 4);
    *pOut = (u32(b[0]) << 24) | (u32(b[1]) << 16) | (u32(b[2]) << 8) | u32(b[3]);
}

void GhostPacket::read(s16* pOut) {
    u8 b[2];
    read(b, 2);
    *pOut = s16((u16(b[0]) << 8) | u16(b[1]));
}
#endif

void GhostPacket::read(char** pOut) {
    char* v3 = (char*)&mDataPtr[mCurOffs];
    *pOut = (char*)v3;
    s32 offs = strlen(v3) + 1;
    mCurOffs += offs;
}

void GhostPacket::read(s8* pOut) {
    read((u8*)pOut, 1);
}

void GhostPacket::read(TVec3Sc* pOut) {
    read((u8*)&pOut->x, 1);
    read((u8*)&pOut->y, 1);
    read((u8*)&pOut->z, 1);
}

void GhostPacket::read(TVec3s* pOut) {
#ifdef __MWERKS__
    read((u8*)&pOut->x, 2);
    read((u8*)&pOut->y, 2);
    read((u8*)&pOut->z, 2);
#else
    read(&pOut->x);
    read(&pOut->y);
    read(&pOut->z);
#endif
}
