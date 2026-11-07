#include "revolution/gd/GDBase.h"
#ifdef TARGET_PC
#include <string.h>
#endif

GDLObj* __GDCurrentDL = NULL;
static GDOverflowCallback overflowcb = NULL;

void GDInitGDLObj(GDLObj* pDL, void* pStart, u32 length) {
    pDL->start = pStart;
    pDL->ptr = (u8*)pStart;
    pDL->top = (u8*)pStart + length;
    pDL->length = length;
}

void GDFlushCurrToMem() {
    DCFlushRange(__GDCurrentDL->start, __GDCurrentDL->length);
}

void GDPadCurr32() {
    u32 n = ((u32)__GDCurrentDL->ptr & 31);
    if (n) {
#ifdef TARGET_PC
        // (Producdevity's fork) One memset and one cursor store: byte by byte, -fno-strict-aliasing makes every store reload
        // __GDCurrentDL and its ptr. Same bytes; __GDWrite has no overflow check either.
        GDLObj* const dl = __GDCurrentDL;
        memset(dl->ptr, 0, 32 - n);
        dl->ptr += 32 - n;
#else
        for (; n < 32; n++) {
            __GDWrite(0);
        }
#endif
    }
}

void GDOverflowed() {
    if (overflowcb) {
        (*overflowcb)();
    }
}
