#ifndef TPL_H
#define TPL_H

#include "revolution/gx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    u16 numEntries;
    u8 unpacked;
    u8 _4;
    GXTlutFmt format;
    PTR32(char) data;
} TPLClutHeader, *TPLClutHeaderPtr;

typedef struct {
    u16 height;
    u16 width;
    u32 format;
    PTR32(char) data;
    GXTexWrapMode wrapS;
    GXTexWrapMode wrapT;
    GXTexFilter minFilter;
    GXTexFilter magFilter;
    f32 LODBias;
    u8 edgeLODEnable;
    u8 minLOD;
    u8 maxLOD;
    u8 unpacked;
} TPLHeader, *TPLHeaderPtr;

typedef struct {
    PTR32(TPLHeader) textureHeader;
    PTR32(TPLClutHeader) CLUTHeader;
} TPLDescriptor, *TPLDescriptorPtr;

typedef struct {
    u32 versionNumber;
    u32 numDescriptors;
    PTR32(TPLDescriptor) descriptorArray;
} TPLPalette, *TPLPalettePtr;

void TPLBind(TPLPalettePtr);
TPLDescriptorPtr TPLGet(TPLPalettePtr, u32);

#ifdef __cplusplus
}
#endif

#endif // TPL_H
