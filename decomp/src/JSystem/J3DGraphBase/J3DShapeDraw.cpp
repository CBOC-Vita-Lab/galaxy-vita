#ifdef TARGET_PC
#include "port/array_skip.h"
#include "port/gx_dedup.h"
#endif
#include "JSystem/J3DGraphBase/J3DShapeDraw.hpp"
#include "JSystem/JKernel/JKRHeap.hpp"
#include <cstring>
#include <revolution/gx/GXDispList.h>
#include <stdint.h>

#ifdef TARGET_PC
// (2.3 port) GXV_RETAINED: every shape's geometry call goes to the front end with the shape's handle in front of it
// (decomp/src/RVL_SDK/gx/GXDisplayList.c port_gx_call_retained; the front end: platform/vgx/src/vgx_dlcache.cpp "retained shapes").
// The handle is a number taken when the shape is made (models may be loaded on another thread: atomic) and never given to another
// shape, so a record the front end keeps for it can never be another shape's; 0 is "none" (skipped when the count wraps). The
// front end checks everything the call depends on (the list, its size and generations, the vertex state, its cache entry): the handle
// only finds the record, it is not trusted for anything.
extern "C" void port_gx_call_retained(const void* list, u32 nbytes, u32 handle);
static u32 J3DPortNewShapeHandle() {
    static u32 sNext = 0;
    u32 h;
    do {
        h = __atomic_add_fetch(&sNext, 1, __ATOMIC_RELAXED);
    } while (h == 0);
    return h;
}
#endif

#if defined(__MWERKS__) || defined(__BIG_ENDIAN__)
#define DL_READ_U16(p) (*reinterpret_cast< u16* >(p))
#define DL_WRITE_U16(p, v) (*reinterpret_cast< u16* >(p) = (v))
#else
// Display lists keep the GPU's big-endian byte order.
#define DL_READ_U16(p) ((u16)(((p)[0] << 8) | (p)[1]))
#define DL_WRITE_U16(p, v) ((p)[0] = (u8)((v) >> 8), (p)[1] = (u8)(v))
#endif

u32 J3DShapeDraw::countVertex(u32 stride) {
    u32 count = 0;
    u8* dlStart = static_cast< u8* >(getDisplayList());

    for (u8* dl = dlStart; (dl - dlStart) < getDisplayListSize();) {
        u8 cmd = *dl;
        dl++;
        if (cmd != GX_TRIANGLEFAN && cmd != GX_TRIANGLESTRIP)
            break;
        int vtxNum = DL_READ_U16(dl);
        dl += 2;
        count += vtxNum;
        dl = static_cast< u8* >(dl) + stride * vtxNum;
    }

    return count;
}

void J3DShapeDraw::addTexMtxIndexInDL(u32 stride, u32 attrOffs, u32 valueBase) {
    u32 newSize = ALIGN_NEXT(countVertex(stride) + mDisplayListSize, 0x20);
    u8* newDLStart = new (0x20) u8[newSize];
    u8* oldDLStart = static_cast< u8* >(mDisplayList);
    u8* oldDL = oldDLStart;
    u8* newDL = newDLStart;

    for (; (oldDL - oldDLStart) < mDisplayListSize;) {
        // Copy command
        u8 cmd = *oldDL;
        oldDL++;
        *newDL++ = cmd;

        if (cmd != GX_TRIANGLEFAN && cmd != GX_TRIANGLESTRIP)
            break;

        // Copy count
        int vtxNum = DL_READ_U16(oldDL);
        oldDL += 2;
        DL_WRITE_U16(newDL, vtxNum);
        newDL += 2;

        for (int i = 0; i < vtxNum; i++) {
            u8* oldDLVtx = &oldDL[stride * i];
            u8 pnmtxidx = *oldDLVtx;
            memcpy(newDL, oldDLVtx, static_cast< int >(attrOffs));
            newDL += attrOffs;
            *newDL++ = valueBase + pnmtxidx;
            memcpy(newDL, oldDLVtx + attrOffs, stride - attrOffs);
            newDL += (stride - attrOffs);
        }

        oldDL = static_cast< u8* >(oldDL) + stride * vtxNum;
    }

    u32 realSize = ALIGN_NEXT(reinterpret_cast< uintptr_t >(newDL) - reinterpret_cast< uintptr_t >(newDLStart), 0x20);
    for (; (newDL - newDLStart) < newSize; newDL++)
        *newDL = 0;

    mDisplayListSize = realSize;
    mDisplayList = newDLStart;
    DCStoreRange(newDLStart, mDisplayListSize);
}

J3DShapeDraw::J3DShapeDraw(const u8* displayList, u32 displayListSize) {
    mDisplayList = const_cast< u8* >(displayList);
    mDisplayListSize = displayListSize;
#ifdef TARGET_PC
    mPortHandle = J3DPortNewShapeHandle();
#endif
}

void J3DShapeDraw::draw() const {
#ifdef TARGET_PC
    __PortArrKeep = 1;  // (GXV_ARRAYSKIP, port/array_skip.h: a shape's geometry list holds primitives only)
    PORT_GXDD_KEEP(PORT_GXDD_BUMP_PRIMS);  // (GXV_GX_DEDUP, port/gx_dedup_core.h: so nothing it knows is forgotten)
    port_gx_call_retained(mDisplayList, mDisplayListSize, mPortHandle);  // (GXV_RETAINED; otherwise exactly GXCallDisplayList)
    PORT_GXDD_KEEP(0);
    __PortArrKeep = 0;
#else
    GXCallDisplayList(mDisplayList, mDisplayListSize);
#endif
}

J3DShapeDraw::~J3DShapeDraw() {
}
