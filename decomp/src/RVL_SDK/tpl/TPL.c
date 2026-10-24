#include "revolution/tpl.h"
#include "revolution/os.h"

#ifdef TARGET_PC
/* Offsets and pointers in the palette are 32-bit fields (PTR32). */
#define TPL_PTR(T, v) ((T)(uintptr_t)(v))

void TPLBind(TPLPalettePtr ptr) {
    u16 i;
    u32 base = (u32)(uintptr_t)ptr;
    TPLDescriptorPtr desc;

    if (ptr->versionNumber != 2142000) {
        OSPanic(__FILE__, 0x19, "invalid version number for texture palette");
    }

    ptr->descriptorArray = ptr->descriptorArray + base;
    desc = TPL_PTR(TPLDescriptorPtr, ptr->descriptorArray);

    for (i = 0; i < ptr->numDescriptors; i++) {
        if (desc[i].textureHeader) {
            TPLHeaderPtr th;
            desc[i].textureHeader = desc[i].textureHeader + base;
            th = TPL_PTR(TPLHeaderPtr, desc[i].textureHeader);

            if (!th->unpacked) {
                th->data = th->data + base;
                th->unpacked = 1;
            }
        }

        if (desc[i].CLUTHeader) {
            TPLClutHeaderPtr ch;
            desc[i].CLUTHeader = desc[i].CLUTHeader + base;
            ch = TPL_PTR(TPLClutHeaderPtr, desc[i].CLUTHeader);

            if (!ch->unpacked) {
                ch->data = ch->data + base;
                ch->unpacked = 1;
            }
        }
    }
}

TPLDescriptorPtr TPLGet(TPLPalettePtr ptr, u32 id) {
    id %= ptr->numDescriptors;
    return &TPL_PTR(TPLDescriptorPtr, ptr->descriptorArray)[id];
}
#else
void TPLBind(TPLPalettePtr ptr) {
    u16 i;

    if (ptr->versionNumber != 2142000) {
        OSPanic(__FILE__, 0x19, "invalid version number for texture palette");
    }

    ptr->descriptorArray = (TPLDescriptorPtr)(((u32)(ptr->descriptorArray)) + ((u32)ptr));

    for (i = 0; i < ptr->numDescriptors; i++) {
        if (ptr->descriptorArray[i].textureHeader) {
            ptr->descriptorArray[i].textureHeader = (TPLHeaderPtr)(((u32)(ptr->descriptorArray[i].textureHeader)) + ((u32)ptr));
        
            if (!ptr->descriptorArray[i].textureHeader->unpacked) {
                ptr->descriptorArray[i].textureHeader->data = (char*)((u32)(ptr->descriptorArray[i].textureHeader->data) + (u32)ptr);
                ptr->descriptorArray[i].textureHeader->unpacked = 1;
            }
        }

        if (ptr->descriptorArray[i].CLUTHeader) {
            ptr->descriptorArray[i].CLUTHeader = (TPLClutHeaderPtr)((u32)(ptr->descriptorArray[i].CLUTHeader) + (u32)ptr);

            if (!ptr->descriptorArray[i].CLUTHeader->unpacked) {
                ptr->descriptorArray[i].CLUTHeader->data = (char*)((u32)(ptr->descriptorArray[i].CLUTHeader->data) + (u32)ptr);
                ptr->descriptorArray[i].CLUTHeader->unpacked = 1;
            }
        }
    }
}

TPLDescriptorPtr TPLGet(TPLPalettePtr ptr, u32 id) {
    id %= ptr->numDescriptors;
    return &ptr->descriptorArray[id];
}
#endif
