#if defined(TARGET_PC)
#define PORT_GX_FX_OWNER 1  // (this file holds __PortGXFx, port/gx_fifo.h)
#endif
#include "revolution/gx.h"
#include "revolution/gx/GXFifo.h"
#include "revolution/gx/GXRegs.h"
#include "revolution/os.h"
#include <stdio.h>
#include <mem.h>
#include <string.h>
#define PORT_GXCAP_HOOKS 1  // (GX capture: the hook in each entry point, port/gx_capture.h)
#include "port/gx_capture.h"
#if defined(TARGET_PC)
#include "port/array_skip.h"  // (GXV_ARRAYSKIP: what J3D's array bases hold in the stream)
#include "port/gx_dedup.h"    // (GXV_GX_DEDUP: a list call forgets what its list may write)
uint32_t __PortArrValid = 0;
uint32_t __PortArrShadow[3];
int __PortArrKeep = 0;
#endif

static GXFifoObj DisplayListFifo ATTRIBUTE_ALIGN(32);
static GXFifoObj OldCPUFifo ATTRIBUTE_ALIGN(32);
static GXData __savedGXdata;

void GXBeginDisplayList(void *list, u32 size)
{
    PORT_GXCAP_ABANDON(GXBeginDisplayList);
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
    PORT_GXCAP_ABANDON(GXEndDisplayList);
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
        __PortGXDedupBump(PORT_GXDD_BUMP_ALL, PORT_GXDD_CAUSE_OTHER);  // (GXV_GX_DEDUP: TEV colours and list calls inside)
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
    PORT_GXCAP_RET(port_gx_fx_begin);
    if (__PortGXFx || !__PortGXFxMergeOk || !__PortGXFxMergeOk()) {
        return 0;
    }
    sFxLen = 0;
    sFxAt = __PortGXFifoPtr;
    __PortGXFx = 1;
    return 1;
}

void port_gx_fx_end(void) {
    PORT_GXCAP(port_gx_fx_end, ());
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

#if defined(TARGET_PC)
__attribute__((weak)) u32 __PortGXDrawSerial = 0;  // (PETARI_BLOOM_SKIP, GXGeometry.c; weak: one counter for the GX units linked)
#endif

#if defined(TARGET_PC)
// (j3n2, GXV_J3DNATIVE >= 1: J3DPacket.cpp sets the switch each picture and logs the table) the unbracketed emitters: list calls and
// immediate primitives the game sends without a GXV_J3DNATIVE bracket or a retained handle, counted by the caller's return address (the
// log gives it as an offset from GXCallDisplayList, to be symbolised with the console ELF: addr2line -f -C -e <elf> <GXCallDisplayList + offset>).
int __PortJ3nEmitOn = 0;
struct PortJ3nEmit { const void* ra; u32 kind, calls, amount; };
enum { kPortJ3nEmitSlots = 512 };
static struct PortJ3nEmit sPortJ3nEmit[kPortJ3nEmitSlots];
static u32 sPortJ3nEmitLost = 0;
void __PortJ3nEmitNote(u32 kind, const void* ra, u32 amount) {
    u32 h = (u32)(((uintptr_t)ra >> 2) * 0x9E3779B1u + kind) >> 23;  // (9 bits)
    for (int probe = 0; probe < 16; ++probe, h = (h + 1) & (kPortJ3nEmitSlots - 1)) {
        struct PortJ3nEmit* e = &sPortJ3nEmit[h];
        if (e->ra == ra && e->kind == kind) { ++e->calls; e->amount += amount; return; }
        if (e->ra == 0) { e->ra = ra; e->kind = kind; e->calls = 1; e->amount = amount; return; }
    }
    ++sPortJ3nEmitLost;
}
// The top `max` entries by calls into out (sorted, most first), then the table cleared: kind 0 a list call (amount: bytes), 1 GXBegin
// (amount: vertices). Returns how many; *lost: notes that found no slot.
int __PortJ3nEmitTake(struct PortJ3nEmit* out, int max, u32* lost) {
    int n = 0;
    for (int i = 0; i < kPortJ3nEmitSlots; ++i) {
        const struct PortJ3nEmit* e = &sPortJ3nEmit[i];
        int k;
        if (e->ra == 0) continue;
        if (n < max) k = n++;
        else if (e->calls > out[max - 1].calls) k = max - 1;
        else continue;
        out[k] = *e;
        for (; k > 0 && out[k - 1].calls < out[k].calls; --k) {
            struct PortJ3nEmit t = out[k];
            out[k] = out[k - 1];
            out[k - 1] = t;
        }
    }
    memset(sPortJ3nEmit, 0, sizeof(sPortJ3nEmit));
    *lost = sPortJ3nEmitLost;
    sPortJ3nEmitLost = 0;
    return n;
}
#endif

void GXCallDisplayList(const void *list, u32 nbytes) {
    PORT_GXCAP(GXCallDisplayList, (list, nbytes));
#if defined(TARGET_PC)
    if (!__PortArrKeep) __PortArrValid = 0;  // (GXV_ARRAYSKIP: a list may write the array bases; J3D's own CP-free lists keep them)
    ++__PortGXDrawSerial;
    if (__PortJ3nEmitOn) __PortJ3nEmitNote(0, __builtin_return_address(0), nbytes);
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
    // (GXV_GX_DEDUP, port/gx_dedup_core.h: what the list may have written is forgotten; the caller may vouch for less, __PortGXDedupKeep)
    __PortGXDedupBump(PORT_GXDD_KEPT(), PORT_GXDD_KEPT() == PORT_GXDD_BUMP_NOCP ? PORT_GXDD_CAUSE_MATLIST :
                                         PORT_GXDD_KEPT() == PORT_GXDD_BUMP_PRIMS ? PORT_GXDD_CAUSE_PRIMS : PORT_GXDD_CAUSE_LIST);
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
    PORT_GXCAP(port_gx_call_prologue, ());
    if (gx->dirtyState) {
        __GXSetDirtyState();
    }

    if (!(*(u32*)(&gx->vNumNot))) {
        __GXSendFlushPrim();
    }
}

// (2.3 port) GXV_RETAINED: GXCallDisplayList for a J3D shape's list (J3DShapeDraw::draw), with the shape's handle in front of the call:
// the port-private command 0x59 + u32 handle, then the call's 9 bytes, written as one run after the call's own work. The front end
// takes the two as one call (platform/vgx/src/vgx_state.cpp ProcessStream, vgx_dlcache.cpp "retained shapes"): the handle only finds
// the record of the shape's last call there; the list, its size and everything it reads are checked as before, so the call is the same
// call. Not while the particle gathering is on (GXV_FXMERGE: the call is then gathered as before), not while a display list is recorded
// and not with another renderer (__PortGXRetainedOk: the FIFO must feed the vgx front end), not without a handle: then GXCallDisplayList.
void port_gx_call_retained(const void* list, u32 nbytes, u32 handle) {
    PORT_GXCAP(port_gx_call_retained, (list, nbytes, handle));
    u8 cmd[14];
    ++__PortGXDrawSerial;  // (PETARI_BLOOM_SKIP; GXCallDisplayList below counts again: only "changed" matters)
    if (__PortGXFx || handle == 0 || !__PortGXRetainedOk || !__PortGXRetainedOk()) {
        GXCallDisplayList(list, nbytes);
        return;
    }
    if (gx->dirtyState) {
        __GXSetDirtyState();
    }

    if (!(*(u32*)(&gx->vNumNot))) {
        __GXSendFlushPrim();
    }
    cmd[0] = 0x59;
    FxPut32(cmd + 1, handle);
    cmd[5] = 0x40;
    FxPut32(cmd + 6, (u32)(uintptr_t)list);
    FxPut32(cmd + 10, (u32)nbytes);
    __PortGXFifoWriteBytes(cmd, sizeof(cmd));
    __PortGXDedupBump(PORT_GXDD_KEPT(), PORT_GXDD_KEPT() == PORT_GXDD_BUMP_NOCP ? PORT_GXDD_CAUSE_MATLIST :  // (GXV_GX_DEDUP: as GXCallDisplayList)
                                         PORT_GXDD_KEPT() == PORT_GXDD_BUMP_PRIMS ? PORT_GXDD_CAUSE_PRIMS : PORT_GXDD_CAUSE_LIST);
}

// (GXV_REPLAY25, port/gx_fifo.h; decomp/src/Game/System/DrawBuffer.cpp) 1 when the next GX call that sends something writes only its own
// bytes: no deferred state to send (the dirty-state flush every list call and every retained call starts with) and no primitive flush
// pending. Then the call's bytes are a function of its arguments alone, and a recorded copy of them is the call.
int port_gx_replay_clean(void) {
    return gx->dirtyState == 0 && *(u32*)(&gx->vNumNot) != 0;
}

// ---- GXV_PIPE25 (pipe26, 12 Nov; platform/include/port/pipe25.h, platform/src/gx/gx_fifo.cpp raw FIFO): the GX library's side of a pass that
// runs later, on another thread, from the state the serial draw would have had at its place in the stream.
//   __PortGXPipeSave / Load: the library's whole data (GXData) in and out of a buffer of __PortGXPipeBytes().
//   __PortGXPipeFirewall: what must hold at the instant a pass is taken out of the stream, on the game thread (when the token is made) and on
//   the thread that runs the pass (before and after it): the state the lazy setters left is sent (dirtyState == 0, no primitive flush
//   pending: port_gx_call_prologue, which every list call starts with), and every shadow of what the stream holds is forgotten, so that
//   nothing is left out on the strength of something a pass (or a list that ran without it) changed: the array bases (GXV_ARRAYSKIP),
//   the light memory (GXV_LIGHT_DEDUP: the generation moves, so the light memo of LightFunction misses too), the TEV colour words
//   (GXV_HOTPROF_TEVCOL). Those are all the shadows the port keeps (grep'd in decomp/src/RVL_SDK/gx and platform/src/gx: __PortArr*,
//   sLightShadow*, sTevCol*). Anything the front end keeps is of the stream as it arrives, in order, whichever thread wrote it.
u32 __PortGXPipeBytes(void) {
    return (u32)sizeof(*gx);
}
void __PortGXPipeSave(void* out) {
    memcpy(out, gx, sizeof(*gx));
}
void __PortGXPipeLoad(const void* in) {
    memcpy(gx, in, sizeof(*gx));
}
void port_gx_tevcol_drop(void) __attribute__((weak));  // (weak: a host test may link some of the GX units only)
void __PortGXLightShadowDrop(void) __attribute__((weak));
// (pipe27) the shadows alone, no bytes: after a late draw's passes were run, the shadows must not claim more than the stream holds
void __PortGXPipeShadowDrop(void) {
    __PortArrValid = 0;
    __PortGXDedupBump(PORT_GXDD_BUMP_ALL, PORT_GXDD_CAUSE_OTHER);  // (GXV_GX_DEDUP's shadow, port/gx_dedup.h)
    if (__PortGXLightShadowDrop) __PortGXLightShadowDrop();
    if (port_gx_tevcol_drop) port_gx_tevcol_drop();
}
void __PortGXPipeFirewall(void) {
    port_gx_call_prologue();
    __PortGXPipeShadowDrop();
}

// (GXV_J3DNATIVE, J3DPortNative.hpp; the front end: platform/vgx/src/vgx_j3native.inc) A material / differed list's call as a bracket:
// 0x5A, id, verA, verB, ext, flags (five u32), the call's 9 bytes (CALL_DL, as GXCallDisplayList writes it), 0x5B, one run after the
// call's own work. Not while the particle gathering is on, not while a display list is recorded and not with another renderer
// (__PortGXBracketOk): then GXCallDisplayList, which is the same call without the bracket.
void port_gx_call_bracketed(const void* list, u32 nbytes, u32 id, u32 verA, u32 verB, u32 ext, u32 flags) {
    PORT_GXCAP(port_gx_call_bracketed, (list, nbytes, id, verA, verB, ext, flags));
    u8 cmd[31];
    ++__PortGXDrawSerial;
    if (__PortGXFx || !__PortGXBracketOk || !__PortGXBracketOk()) {
        GXCallDisplayList(list, nbytes);
        return;
    }
    if (gx->dirtyState) {
        __GXSetDirtyState();
    }

    if (!(*(u32*)(&gx->vNumNot))) {
        __GXSendFlushPrim();
    }
    cmd[0] = 0x5A;
    FxPut32(cmd + 1, id);
    FxPut32(cmd + 5, verA);
    FxPut32(cmd + 9, verB);
    FxPut32(cmd + 13, ext);
    FxPut32(cmd + 17, flags);
    cmd[21] = 0x40;
    FxPut32(cmd + 22, (u32)(uintptr_t)list);
    FxPut32(cmd + 26, (u32)nbytes);
    cmd[30] = 0x5B;
    __PortGXFifoWriteBytes(cmd, sizeof(cmd));
    __PortGXDedupBump(PORT_GXDD_KEPT(), PORT_GXDD_KEPT() == PORT_GXDD_BUMP_NOCP ? PORT_GXDD_CAUSE_MATLIST :  // (GXV_GX_DEDUP: as GXCallDisplayList)
                                         PORT_GXDD_KEPT() == PORT_GXDD_BUMP_PRIMS ? PORT_GXDD_CAUSE_PRIMS : PORT_GXDD_CAUSE_LIST);
}
#endif
