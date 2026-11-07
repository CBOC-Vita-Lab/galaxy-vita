#if defined(TARGET_PC)
#define PORT_GX_FX_OWNER 1  // (this file holds __PortGXFx, port/gx_fifo.h)
#endif
#include "revolution/gx.h"
#include "revolution/gx/GXFifo.h"
#include "revolution/gx/GXRegs.h"
#include "revolution/os.h"
#include <stdio.h>
#include <mem.h>

static GXFifoObj DisplayListFifo ATTRIBUTE_ALIGN(32);
static GXFifoObj OldCPUFifo ATTRIBUTE_ALIGN(32);
static GXData __savedGXdata;

void GXBeginDisplayList(void *list, u32 size)
{
    __GXFifoObj* dlFifo = (__GXFifoObj*)&DisplayListFifo;
    if (gx->dirtyState) {
        __GXSetDirtyState();
    }

    if (gx->dlSaveContext) {
        memcpy(&__savedGXdata, gx, sizeof(*gx));
    }

    dlFifo->base = (u8*)list;
    dlFifo->top = (u8*)list + size - 4;
    dlFifo->size = size;
    dlFifo->count = 0;
    dlFifo->rdPtr = list;
    dlFifo->wrPtr = list;

    gx->inDispList   = GX_TRUE;

    GXFlush();
    GXGetCPUFifo(&OldCPUFifo);
    GXSetCPUFifo(&DisplayListFifo);
    GXResetWriteGatherPipe();
}

u32 GXEndDisplayList(void) {
    GXBool ov;
    BOOL enabled;
    u32 cpenable;

    GXFlush();
    GXGetCPUFifo(&DisplayListFifo);
    ov = GXGetFifoWrap(&DisplayListFifo);

    GXSetCPUFifo(&OldCPUFifo);

    if (gx->dlSaveContext)
    {
        enabled = OSDisableInterrupts();
        cpenable = gx->cpEnable;
        memcpy(gx, &__savedGXdata, sizeof(*gx));
        gx->cpEnable = cpenable;
        OSRestoreInterrupts(enabled);
    }

    gx->inDispList = GX_FALSE;
    
    if (!ov) {
        return GXGetFifoCount(&DisplayListFifo);
    }
    else
    {
        return 0;
    }
}

#if defined(TARGET_PC)
// GXV_FXMERGE (port/gx_fifo.h; the switch: gx_fifo.cpp __PortGXFxMergeOk): an emitter's particle loop gathered into port-private
// commands for the vgx front end (platform/vgx/src/vgx_state.cpp, FxMergeRun: the format, and why the pictures are the same).
// JPAResource::drawP / drawC call port_gx_fx_begin before the loop and port_gx_fx_end after it, for emitters whose particle functions
// make no GX calls but GXSetTevColor, GXLoadPosMtxImm, GXLoadTexMtxImm and GXCallDisplayList. While __PortGXFx is set those four
// put an item into sFx (the same register words they would have written) and keep the library's own state as they would. What
// GXCallDisplayList would have written to the FIFO before its call (the dirty state, a flush primitive of more than 16 vertices)
// is written there after sending what was gathered before it, so the order of everything is kept. A command goes out at the end of
// the loop, or when the next item would not fit. (Game thread only, as every GX call.)
u8 __PortGXFx;
static u8 sFx[3 + 8192];
static u32 sFxLen;  // payload bytes gathered
static u8* sFxAt;   // the FIFO's write position after the last command sent (or at the start): nothing else may write meanwhile

static u8* FxPut32(u8* p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
    return p + 4;
}

static void FxSend(void) {
    if (sFxLen != 0) {
        sFx[0] = 0x58;
        sFx[1] = (u8)(sFxLen >> 8);
        sFx[2] = (u8)sFxLen;
        __PortGXFifoWriteBytes(sFx, 3 + sFxLen);
        sFxLen = 0;
    }
    sFxAt = __PortGXFifoPtr;
}

// Room for an item of n bytes.
static u8* FxItem(u32 n) {
    u8* p;
    if (__PortGXFifoPtr != sFxAt) {
        __PortGXFxForeign();
        FxSend();
    }
    if (sFxLen + n > sizeof(sFx) - 3) {
        FxSend();
    }
    p = sFx + 3 + sFxLen;
    sFxLen += n;
    return p;
}

int port_gx_fx_begin(void) {
    if (__PortGXFx || !__PortGXFxMergeOk || !__PortGXFxMergeOk()) {
        return 0;
    }
    sFxLen = 0;
    sFxAt = __PortGXFifoPtr;
    __PortGXFx = 1;
    return 1;
}

void port_gx_fx_end(void) {
    if (!__PortGXFx) {
        return;
    }
    if (__PortGXFifoPtr != sFxAt) {
        __PortGXFxForeign();
    }
    __PortGXFx = 0;
    FxSend();
}

// GXSetTevColor's two BP words (RA, then BG, which it writes three times).
void __PortGXFxTevColor(u32 ra, u32 bg) {
    u8* p = FxItem(9);
    p[0] = 0x62;
    p = FxPut32(p + 1, ra);
    FxPut32(p, bg);
}

// An XF load of n words (GXLoadPosMtxImm, GXLoadTexMtxImm): its register word, then the words as the FIFO would carry them.
void __PortGXFxXF(u32 reg, const f32* words, u32 n) {
    u8* p = FxItem(5 + n * 4);
    u32 i;
    p[0] = 0x10;
    p = FxPut32(p + 1, reg);
    for (i = 0; i < n; i++) {
        u32 w;
        memcpy(&w, &words[i], 4);
        p = FxPut32(p, w);
    }
}

// GXCallDisplayList, gathered: its work before the call as it would be done, then the call.
static void FxCall(const void* list, u32 nbytes) {
    u8 flush = 0;
    u8* p;
    if (gx->dirtyState) {
        FxSend();
        __PortGXFx = 0;
        __GXSetDirtyState();
        __PortGXFx = 1;
        sFxAt = __PortGXFifoPtr;
    }
    if (!(*(u32*)(&gx->vNumNot))) {
        if (gx->vNum <= 16) {
            // (the all-zero strip __GXSendFlushPrim sends, as a count on the call: the front end drops such a strip)
            flush = (u8)gx->vNum;
            gx->bpSentNot = GX_TRUE;
        } else {
            FxSend();
            __PortGXFx = 0;
            __GXSendFlushPrim();
            __PortGXFx = 1;
            sFxAt = __PortGXFifoPtr;
        }
    }
    p = FxItem(10);
    p[0] = 0x40;
    p = FxPut32(p + 1, (u32)(uintptr_t)list);
    p = FxPut32(p, (u32)nbytes);
    p[0] = flush;
}
#endif

void GXCallDisplayList(const void *list, u32 nbytes) {
#if defined(TARGET_PC)
    if (__PortGXFx) {  // (GXV_FXMERGE: gathered, above)
        FxCall(list, nbytes);
        return;
    }
#endif
    if (gx->dirtyState)  {
        __GXSetDirtyState();
    }

    if (!(*(u32*)(&gx->vNumNot))) {
        __GXSendFlushPrim();
    }

#if defined(TARGET_PC)
    __PortGXFifo_op_u32_u32(0x40, (u32)list, (u32)nbytes);  // (the same 9 bytes, batched: GXV_FIFO_BATCH, port/gx_fifo.h)
#else
    GX_WRITE_U8(0x40);
    GX_WRITE_U32((u32)list);
    GX_WRITE_U32((u32)nbytes);
#endif
}

#if defined(TARGET_PC)
// (1.3 port) GXCallDisplayList's work before the call, for a call the game leaves out because its list would change nothing
// (GXV_J3D_DIFFSKIP, GXV_J3D_MATMERGE: decomp/src/Game/System/DrawBuffer.cpp): the state set since the last command is still sent
// at that point of the stream, as the call would have sent it.
void port_gx_call_prologue(void) {
    if (gx->dirtyState) {
        __GXSetDirtyState();
    }

    if (!(*(u32*)(&gx->vNumNot))) {
        __GXSendFlushPrim();
    }
}
#endif
