// GX capture and replay (drawpipe24 section 2.3a; the implementation and the reasons: platform/src/gx/gx_capture.cpp).
//
// A thread that owns GX opens a capture; from then its GX calls are recorded instead of executed (each entry point of the GX
// library, decomp/src/RVL_SDK/gx/*.c and gx_fifo.cpp, has a hook in its first line; the bytes the game writes into the FIFO itself,
// immediate vertices between GXBegin / GXEnd, J3DFifo / J3DGD writes, the port's markers, land in the capture as they are because
// the FIFO's write pointer points into the capture while it is open). Later the thread that owns GX then replays the record through
// the real library in the same order; the stream the front end receives is the one the calls would have produced (gx_capture.cpp
// "Why the replay is exact"). Any call the layer cannot record exactly ends the capture ("abandon"): what was recorded is replayed
// at once on the calling thread, the call runs for real, and the caller goes on serially.
//
// GXV_GXCAPTURE (read once): 0 off (no capture can open; the GX library is today's: one load and a not-taken branch at each entry
// point, the inline FIFO writers are untouched), 1 on, 2 verify (every call is also executed at once; at the end the record is
// replayed into a scratch stream from the GX state at the start and compared with the real bytes, and each mirrored getter with the
// real one: the first difference is logged by entry point; the caller sees an abandoned capture and goes on serially).
// GXV_GXCAPTURE_SELFTEST=1 (with GXV_GXCAPTURE 1 or 2): the game thread's GX calls are cut into captures by the layer itself
// (begun at the first GX call, ended and replayed at once on the same thread at every abandon-class call or after
// GXV_GXCAPTURE_SELFTEST_CALLS calls): no gain, the same pictures; the console's first test of the layer.
// GXV_GXCAPTURE_SYNC=1: GXSetDrawSync is recorded (its token reaches the front end, and so the game's callback, a few ms later than
// today); 0 (default): it abandons.
// Log (every 5 s while on): "gxcap: ...".
#pragma once

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- The interface the draw pipeline codes against
typedef struct PortGxCapture PortGxCapture;  // an opaque recorded run of GX calls

// The calling thread owns GX: from now its GX calls are recorded, not executed. false: capture unavailable now (mode 0, a capture
// already open, a replay running, a display list being recorded, a particle gathering open, the FIFO not feeding the stream).
bool port_gx_capture_begin(void);
// Records "token id happens here". If the capture was abandoned before this point the registered handler is called right away
// (the pass then runs where it would have run serially). In mode 2 the handler is called right away too (the pass runs now, for
// real, its bytes and the GX state after it are kept for the comparison).
void port_gx_capture_token(uint32_t id);
// Stop recording. nullptr: the capture was abandoned (everything recorded so far has already been executed in order) or it was a
// mode-2 capture (executed as it went): the caller simply continues serially.
PortGxCapture* port_gx_capture_end(void);
// On the thread that owns GX: execute the recorded calls in order, calling token() at each token; frees c.
void port_gx_replay(PortGxCapture* c, void (*token)(uint32_t id, void* ctx), void* ctx);

// ---- Additions
int port_gx_capture_mode(void);   // GXV_GXCAPTURE: 0, 1, 2
bool port_gx_capture_open(void);  // a capture is open on the calling thread
// The function that draws a token's pass when the layer has to do it itself (an abandon with tokens recorded, a token after an
// abandon, mode 2). Set it before the first token; the replay's token argument is used for port_gx_replay itself.
void port_gx_capture_set_token_handler(void (*token)(uint32_t id, void* ctx), void* ctx);
// As port_gx_capture_token, plus a claim that the token's pass leaves these mirrored states as they are (the mirror then keeps
// answering their getters after the token instead of abandoning). Mode 2 checks each claim after the pass and logs a wrong one.
// PORT_GXCAP_KEEP_SERIAL (dpengine's GX-HOOKS): the token's pass leaves the draw serial alone: no primitive, no display-list call,
// no frame buffer copy (a J3D pass whose buffer type had no drawer of its half at the hand-over; it loads lights at most), so the
// serial's mirror stays good across it. dpengine sets it on every such token. Mode 2 checks the claim like the others.
enum { PORT_GXCAP_KEEP_VIEWPORT = 1, PORT_GXCAP_KEEP_PROJECTION = 2, PORT_GXCAP_KEEP_SCISSOR = 4, PORT_GXCAP_KEEP_SERIAL = 8 };
void port_gx_capture_token_keep(uint32_t id, uint32_t keep);
// End the open capture of this thread the abandon way, for a reason outside the GX library (a writer about to change memory that
// recorded calls reference by pointer: port_gx_view_write_from, the material-list writers). No-op when none is open.
void port_gx_capture_abandon(const char* why);

typedef struct PortGxCaptureStats {
    uint64_t begun, refused, ended, abandoned, verified, replayed;
    uint64_t calls, rawBytes, recordBytes, tokens, getters;
    uint64_t verifyDiffs, foreign, contractErrors;
    uint64_t captureNs, replayNs;
} PortGxCaptureStats;
void port_gx_capture_stats(PortGxCaptureStats* out);  // totals since start
void port_gx_replay_arm(uintptr_t stackLo, uintptr_t stackSpan);  // (gx_capture.cpp; the draw pipeline, at the hand-over and after the join)

// ---- Hooks (the GX library's side; see gx_capture.cpp)
// __PortGXCap: 0 idle (the only value in mode 0), 1 a capture is open, 2 armed (GXV_GXCAPTURE_SELFTEST: the next GX call of the
// game thread opens one). __PortGXCapFifo: nonzero while the FIFO events (overflow, flush, the idle point) belong to the layer.
// (dpengine's GX-HOOKS) 3: a replay is running on the draw thread beside the game thread's update (set by port_gx_replay while
// __PortGXCapTrap is set, which the draw pipeline does for the length of an overlap). A hooked GX call on any thread then calls
// port_drawpipe_game_enter() first: on the game thread it joins the draw thread (the stream and the library's state are the
// replay's until then); on the draw thread itself it returns at once.
extern int __PortGXCap;
extern int __PortGXCapTrap;
extern int __PortGXCapPass;  // > 0: a call the layer runs for real is executing (its own hook and nested ones let it through)
// The replaying thread's stack while __PortGXCap is 3 (PORT_GXCAP_FOREIGN): [top - span + 1, top].
extern uintptr_t __PortGXCapReplayTop, __PortGXCapReplaySpan;
// (pipefix31) The late draw's owner trap (GXV_PIPE25) is three-way: the window above is the HELPER's stack and its calls pass; the game thread joins the helper;
// every other thread waits until no job is out. While another thread runs its real call (the layer's `through`) the hooks must let that very call and the ones
// it makes pass: the thread's whole stack is registered as an exempt window [ExTop - ExSpan + 1, ExTop] for the length of the call (one thread at a time).
extern uintptr_t __PortGXCapExTop, __PortGXCapExSpan;
// The late draw's trap, armed by the game thread at the hand-over: the helper's stack [lo, lo + span). 0: not armed (no trap flag, layer not idle, stack unknown).
int port_gx_replay_arm_trap(uintptr_t helperLo, uintptr_t helperSpan);
const char* port_gx_trap_selftest(uintptr_t helperLo, uintptr_t helperSpan);  // start-up check of the late draw's trap (gx_capture.cpp): nullptr when it behaves, else the reason
void* port_gx_trap_probe_start(void);  // start-up check: a thread that makes hooked calls (a getter and a setter) under the armed trap
int port_gx_trap_probe_wait(void* probe, uint32_t ms, int64_t* returnedAtNs);  // 1: it returned (when: port_host_time_ns); 0: not within ms; frees the probe
void port_gx_trap_stats(uint32_t* foreign, uint32_t* gameEntries, uint32_t* stuck, uint32_t* runaway);
extern int __PortGXCapFifo;
void port_gx_capture_init(void);                   // gx_fifo.cpp port_gx_init: reads the switches, logs them
int port_gxcap_fifo_pre(int event);                // 1: the event was the layer's (the caller returns); 0: run it, then call _post
void port_gxcap_fifo_post(int event);
int port_gxcap_stream_override(void);              // the scratch replay of mode 2 stands for the stream (gx_fifo.cpp's "Ok" tests)
enum { PORT_GXCAP_EV_OVERFLOW = 0, PORT_GXCAP_EV_FLUSH = 1, PORT_GXCAP_EV_IDLE = 2 };

#if defined(TARGET_PC)
#define PORT_GXCAP_LIKELY0(x) __builtin_expect((x) != 0, 0)
// A recorded entry point: while the layer is active the call goes to port_gxcap_<name>, which records it or runs this function
// for real (with __PortGXCapPass raised: the hook then lets it through). A tail call: with the layer idle the hook is one load and a
// branch not taken, and the function needs no stack frame for it (the console's code: REPORT.txt, "off mode").
// (dpengine, 12 Nov) __PortGXCapPass is one word for all threads: while a replay beside the game thread (__PortGXCap == 3) runs
// its calls for real, a GX call of ANOTHER thread must still go to the layer (it joins the replay first), or the two would be
// in the library at once. PORT_GXCAP_FOREIGN says so: true only with __PortGXCap == 3 and a caller whose stack is not the
// replaying thread's. It is asked only where the pass word alone used to decide, never with the layer idle, and it is a
// compare of the stack pointer, read as a register: no call and no frame, so the hook stays a tail call (a call here, or the
// address of a local, gave every hooked function a prologue on its idle path).
// (C only: the GX library's units, the only ones with these two hooks. A C++ unit that wants them must find its own way to
// the stack pointer: PORT_GXCAP_FOREIGN is not defined there, so that it cannot be left out unnoticed.)
#if defined(__cplusplus)
#elif defined(__arm__) || defined(__aarch64__)
register uintptr_t __PortGXCapSp __asm__("sp");
#define PORT_GXCAP_FOREIGN() (__PortGXCap == 3 && __PortGXCapReplayTop - __PortGXCapSp >= __PortGXCapReplaySpan && __PortGXCapExTop - __PortGXCapSp >= __PortGXCapExSpan)
#elif defined(__x86_64__)
register uintptr_t __PortGXCapSp __asm__("rsp");
#define PORT_GXCAP_FOREIGN() (__PortGXCap == 3 && __PortGXCapReplayTop - __PortGXCapSp >= __PortGXCapReplaySpan && __PortGXCapExTop - __PortGXCapSp >= __PortGXCapExSpan)
#else
#define PORT_GXCAP_FOREIGN() 0  /* (no stack pointer to read: as before, the pass word decides) */
#endif
#define PORT_GXCAP(name, args)                                                                                                      \
    do {                                                                                                                            \
        if (PORT_GXCAP_LIKELY0(__PortGXCap) && (!__PortGXCapPass || PORT_GXCAP_FOREIGN())) {                                         \
            port_gxcap_##name args;                                                                                                 \
            return;                                                                                                                 \
        }                                                                                                                           \
    } while (0)
// The same for an entry point that returns a value (the answer comes from the layer).
#define PORT_GXCAP_RET(name)                                                                                                        \
    do {                                                                                                                            \
        if (PORT_GXCAP_LIKELY0(__PortGXCap) && (!__PortGXCapPass || PORT_GXCAP_FOREIGN())) return port_gxcap_##name();               \
    } while (0)
// An entry point that ends the capture (what was recorded is replayed first), then runs for real.
#define PORT_GXCAP_ABANDON(name)                                                                                                    \
    do {                                                                                                                            \
        if (PORT_GXCAP_LIKELY0(__PortGXCap)) port_gxcap_abandon_entry(PORT_GXCAP_ID_##name);                                         \
    } while (0)
#else
#define PORT_GXCAP(name, args) ((void)0)
#define PORT_GXCAP_RET(name) ((void)0)
#define PORT_GXCAP_ABANDON(name) ((void)0)
#endif

// Every entry point with a hook. Kinds: R recorded (arguments by value; pointers kept as pointers: the library passes them on into
// the stream), M as R and the mirror follows it, P recorded with the pointee copied (the library reads it at call time), G answered
// from the mirror, A abandons, S special, T special and returns a value (gx_capture.cpp says what each does). INVENTORY.tsv (the work folder) has every entry point, these and the pure ones.
#define PORT_GXCAP_ABANDON_LIST(X)                                                                                                  \
    X(GXAbortFrame) X(GXBeginDisplayList) X(GXEndDisplayList) X(GXDrawDone) X(GXSetDrawDone) X(GXWaitDrawDone)                       \
    X(GXSetDrawSyncCallback) X(GXSetDrawDoneCallback) X(GXSetTexRegionCallback) X(GXSetTlutRegionCallback)                           \
    X(GXSetDispCopyYScale) X(GXResetWriteGatherPipe) X(GXLoadTexObjPreLoaded) X(GXInit) X(port_gx_wait_finish_other)                 \
    X(GXSetCPUFifo) X(GXSetGPFifo) X(GXGetCPUFifo) X(GXGetGPFifo) X(GXGetGPStatus) X(GXSetDrawSync_abandon) X(GXPeek_abandon)        \
    X(fifo_flush) X(fifo_idle) X(fifo_overflow) X(draw_serial_after_token) X(getter_after_token) X(fx_begin) X(texregion_custom)    \
    X(light_shadow_list) X(outside)

#if defined(PORT_GXCAP_HOOKS)
// (needs the GX types: the GX units include revolution/gx.h first)
#define PORT_GXCAP_RECORD_LIST(X)                                                                                                   \
    X(M, GXBegin, (GXPrimitive a0, GXVtxFmt a1, u16 a2), (a0, a1, a2))                                                              \
    X(M, GXCallDisplayList, (const void* a0, u32 a1), (a0, a1))                                                                     \
    X(R, GXClearBoundingBox, (void), ())                                                                                            \
    X(R, GXClearVtxDesc, (void), ())                                                                                                \
    X(M, GXCopyDisp, (void* a0, GXBool a1), (a0, a1))                                                                               \
    X(M, GXCopyTex, (void* a0, GXBool a1), (a0, a1))                                                                                \
    X(R, GXEnableTexOffsets, (GXTexCoordID a0, GXBool a1, GXBool a2), (a0, a1, a2))                                                 \
    X(R, GXFlush, (void), ())                                                                                                       \
    X(R, GXInvalidateTexAll, (void), ())                                                                                            \
    X(R, GXInvalidateVtxCache, (void), ())                                                                                          \
    X(R, GXLoadTexMtxIndx, (u16 a0, u32 a1, GXTexMtxType a2), (a0, a1, a2))                                                         \
    X(R, GXPixModeSync, (void), ())                                                                                                 \
    X(R, GXTexModeSync, (void), ())                                                                                                 \
    X(R, GXPokeAlphaMode, (GXCompare a0, u8 a1), (a0, a1))                                                                          \
    X(R, GXPokeAlphaRead, (GXAlphaReadMode a0), (a0))                                                                               \
    X(R, GXPokeAlphaUpdate, (GXBool a0), (a0))                                                                                      \
    X(R, GXPokeBlendMode, (GXBlendMode a0, GXBlendFactor a1, GXBlendFactor a2, GXLogicOp a3), (a0, a1, a2, a3))                     \
    X(R, GXPokeColorUpdate, (GXBool a0), (a0))                                                                                      \
    X(R, GXPokeDither, (GXBool a0), (a0))                                                                                           \
    X(R, GXPokeDstAlpha, (GXBool a0, u8 a1), (a0, a1))                                                                              \
    X(R, GXPokeZMode, (GXBool a0, GXCompare a1, GXBool a2), (a0, a1, a2))                                                           \
    X(R, GXSetAlphaCompare, (GXCompare a0, u8 a1, GXAlphaOp a2, GXCompare a3, u8 a4), (a0, a1, a2, a3, a4))                         \
    X(R, GXSetAlphaUpdate, (GXBool a0), (a0))                                                                                       \
    X(R, GXSetArray, (GXAttr a0, const void* a1, u8 a2), (a0, a1, a2))                                                              \
    X(R, GXSetBlendMode, (GXBlendMode a0, GXBlendFactor a1, GXBlendFactor a2, GXLogicOp a3), (a0, a1, a2, a3))                      \
    X(R, GXSetChanAmbColor, (GXChannelID a0, GXColor a1), (a0, a1))                                                                 \
    X(R, GXSetChanCtrl, (GXChannelID a0, GXBool a1, GXColorSrc a2, GXColorSrc a3, u32 a4, GXDiffuseFn a5, GXAttnFn a6),             \
      (a0, a1, a2, a3, a4, a5, a6))                                                                                                 \
    X(R, GXSetChanMatColor, (GXChannelID a0, GXColor a1), (a0, a1))                                                                 \
    X(R, GXSetClipMode, (GXClipMode a0), (a0))                                                                                      \
    X(R, GXSetCoPlanar, (GXBool a0), (a0))                                                                                          \
    X(R, GXSetColorUpdate, (GXBool a0), (a0))                                                                                       \
    X(R, GXSetCopyClamp, (GXFBClamp a0), (a0))                                                                                      \
    X(R, GXSetCopyClear, (GXColor a0, u32 a1), (a0, a1))                                                                            \
    X(R, GXSetCullMode, (GXCullMode a0), (a0))                                                                                      \
    X(R, GXSetCurrentMtx, (u32 a0), (a0))                                                                                           \
    X(R, GXSetDispCopyDst, (u16 a0, u16 a1), (a0, a1))                                                                              \
    X(R, GXSetDispCopyFrame2Field, (GXCopyMode a0), (a0))                                                                           \
    X(R, GXSetDispCopyGamma, (GXGamma a0), (a0))                                                                                    \
    X(R, GXSetDispCopySrc, (u16 a0, u16 a1, u16 a2, u16 a3), (a0, a1, a2, a3))                                                      \
    X(R, GXSetDither, (GXBool a0), (a0))                                                                                            \
    X(R, GXSetDstAlpha, (GXBool a0, u8 a1), (a0, a1))                                                                               \
    X(R, GXSetFieldMask, (GXBool a0, GXBool a1), (a0, a1))                                                                          \
    X(R, GXSetFieldMode, (GXBool a0, GXBool a1), (a0, a1))                                                                          \
    X(R, GXSetFog, (GXFogType a0, f32 a1, f32 a2, f32 a3, f32 a4, GXColor a5), (a0, a1, a2, a3, a4, a5))                             \
    X(R, GXSetIndTexCoordScale, (GXIndTexStageID a0, GXIndTexScale a1, GXIndTexScale a2), (a0, a1, a2))                             \
    X(R, GXSetIndTexOrder, (GXIndTexStageID a0, GXTexCoordID a1, GXTexMapID a2), (a0, a1, a2))                                      \
    X(R, GXSetLineWidth, (u8 a0, GXTexOffset a1), (a0, a1))                                                                         \
    X(R, GXSetMisc, (GXMiscToken a0, u32 a1), (a0, a1))                                                                             \
    X(R, GXSetNumChans, (u8 a0), (a0))                                                                                              \
    X(R, GXSetNumIndStages, (u8 a0), (a0))                                                                                          \
    X(R, GXSetNumTevStages, (u8 a0), (a0))                                                                                          \
    X(R, GXSetNumTexGens, (u8 a0), (a0))                                                                                            \
    X(R, GXSetPixelFmt, (GXPixelFmt a0, GXZFmt16 a1), (a0, a1))                                                                     \
    X(R, GXSetPointSize, (u8 a0, GXTexOffset a1), (a0, a1))                                                                         \
    X(M, GXSetScissor, (u32 a0, u32 a1, u32 a2, u32 a3), (a0, a1, a2, a3))                                                          \
    X(R, GXSetScissorBoxOffset, (s32 a0, s32 a1), (a0, a1))                                                                         \
    X(R, GXSetTevAlphaIn, (GXTevStageID a0, GXTevAlphaArg a1, GXTevAlphaArg a2, GXTevAlphaArg a3, GXTevAlphaArg a4),                \
      (a0, a1, a2, a3, a4))                                                                                                         \
    X(R, GXSetTevAlphaOp, (GXTevStageID a0, GXTevOp a1, GXTevBias a2, GXTevScale a3, GXBool a4, GXTevRegID a5),                     \
      (a0, a1, a2, a3, a4, a5))                                                                                                     \
    X(R, GXSetTevColor, (GXTevRegID a0, GXColor a1), (a0, a1))                                                                      \
    X(R, GXSetTevColorIn, (GXTevStageID a0, GXTevColorArg a1, GXTevColorArg a2, GXTevColorArg a3, GXTevColorArg a4),                \
      (a0, a1, a2, a3, a4))                                                                                                         \
    X(R, GXSetTevColorOp, (GXTevStageID a0, GXTevOp a1, GXTevBias a2, GXTevScale a3, GXBool a4, GXTevRegID a5),                     \
      (a0, a1, a2, a3, a4, a5))                                                                                                     \
    X(R, GXSetTevColorS10, (GXTevRegID a0, GXColorS10 a1), (a0, a1))                                                                \
    X(R, GXSetTevDirect, (GXTevStageID a0), (a0))                                                                                   \
    X(R, GXSetTevIndWarp, (GXTevStageID a0, GXIndTexStageID a1, GXBool a2, GXBool a3, GXIndTexMtxID a4), (a0, a1, a2, a3, a4))      \
    X(R, GXSetTevIndirect, (GXTevStageID a0, GXIndTexStageID a1, GXIndTexFormat a2, GXIndTexBiasSel a3, GXIndTexMtxID a4,           \
                            GXIndTexWrap a5, GXIndTexWrap a6, GXBool a7, GXBool a8, GXIndTexAlphaSel a9),                           \
      (a0, a1, a2, a3, a4, a5, a6, a7, a8, a9))                                                                                     \
    X(R, GXSetTevKAlphaSel, (GXTevStageID a0, GXTevKAlphaSel a1), (a0, a1))                                                         \
    X(R, GXSetTevKColor, (GXTevKColorID a0, GXColor a1), (a0, a1))                                                                  \
    X(R, GXSetTevKColorSel, (GXTevStageID a0, GXTevKColorSel a1), (a0, a1))                                                         \
    X(R, GXSetTevOp, (GXTevStageID a0, GXTevMode a1), (a0, a1))                                                                     \
    X(R, GXSetTevOrder, (GXTevStageID a0, GXTexCoordID a1, GXTexMapID a2, GXChannelID a3), (a0, a1, a2, a3))                        \
    X(R, GXSetTevSwapMode, (GXTevStageID a0, GXTevSwapSel a1, GXTevSwapSel a2), (a0, a1, a2))                                       \
    X(R, GXSetTevSwapModeTable, (GXTevSwapSel a0, GXTevColorChan a1, GXTevColorChan a2, GXTevColorChan a3, GXTevColorChan a4),      \
      (a0, a1, a2, a3, a4))                                                                                                         \
    X(R, GXSetTexCoordGen2, (GXTexCoordID a0, GXTexGenType a1, GXTexGenSrc a2, u32 a3, GXBool a4, u32 a5), (a0, a1, a2, a3, a4, a5)) \
    X(R, GXSetTexCoordScaleManually, (GXTexCoordID a0, GXBool a1, u16 a2, u16 a3), (a0, a1, a2, a3))                                \
    X(R, GXSetTexCopyDst, (u16 a0, u16 a1, GXTexFmt a2, GXBool a3), (a0, a1, a2, a3))                                               \
    X(R, GXSetTexCopySrc, (u16 a0, u16 a1, u16 a2, u16 a3), (a0, a1, a2, a3))                                                       \
    X(M, GXSetViewport, (f32 a0, f32 a1, f32 a2, f32 a3, f32 a4, f32 a5), (a0, a1, a2, a3, a4, a5))                                 \
    X(M, GXSetViewportJitter, (f32 a0, f32 a1, f32 a2, f32 a3, f32 a4, f32 a5, u32 a6), (a0, a1, a2, a3, a4, a5, a6))               \
    X(R, GXSetVtxAttrFmt, (GXVtxFmt a0, GXAttr a1, GXCompCnt a2, GXCompType a3, u8 a4), (a0, a1, a2, a3, a4))                       \
    X(R, GXSetVtxDesc, (GXAttr a0, GXAttrType a1), (a0, a1))                                                                        \
    X(R, GXSetZCompLoc, (GXBool a0), (a0))                                                                                          \
    X(R, GXSetZMode, (GXBool a0, GXCompare a1, GXBool a2), (a0, a1, a2))                                                            \
    X(R, GXSetZScaleOffset, (f32 a0, f32 a1), (a0, a1))                                                                             \
    X(R, GXSetZTexture, (GXZTexOp a0, GXTexFmt a1, u32 a2), (a0, a1, a2))                                                           \
    X(R, port_gx_call_prologue, (void), ())                                                                                         \
    X(M, port_gx_call_retained, (const void* a0, u32 a1, u32 a2), (a0, a1, a2))                                                     \
    X(M, port_gx_call_bracketed, (const void* a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6), (a0, a1, a2, a3, a4, a5, a6))   \
    X(R, __PortGXLightShadowDrop, (void), ())                                                                                       \
    X(P, GXLoadPosMtxImm, (const f32 a0[3][4], u32 a1), (a0, a1))                                                                   \
    X(P, GXLoadNrmMtxImm, (const f32 a0[3][4], u32 a1), (a0, a1))                                                                   \
    X(P, GXLoadTexMtxImm, (const f32 a0[][4], u32 a1, GXTexMtxType a2), (a0, a1, a2))                                               \
    X(P, GXSetProjection, (const f32 a0[4][4], GXProjectionType a1), (a0, a1))                                                      \
    X(P, GXSetProjectionv, (const f32* a0), (a0))                                                                                   \
    X(P, GXSetIndTexMtx, (GXIndTexMtxID a0, const f32 a1[2][3], s8 a2), (a0, a1, a2))                                               \
    X(P, GXSetFogRangeAdj, (GXBool a0, u16 a1, const GXFogAdjTable* a2), (a0, a1, a2))                                              \
    X(P, GXSetCopyFilter, (GXBool a0, const u8 a1[12][2], GXBool a2, const u8 a3[7]), (a0, a1, a2, a3))                             \
    X(P, GXSetVtxAttrFmtv, (GXVtxFmt a0, const GXVtxAttrFmtList* a1), (a0, a1))                                                     \
    X(P, GXLoadLightObjImm, (const GXLightObj* a0, GXLightID a1), (a0, a1))                                                         \
    X(P, GXLoadTexObj, (const GXTexObj* a0, GXTexMapID a1), (a0, a1))                                                               \
    X(P, GXLoadTlut, (const GXTlutObj* a0, u32 a1), (a0, a1))                                                                       \
    X(S, GXSetDrawSync, (u16 a0), (a0))                                                                                             \
    X(G, GXGetViewportv, (f32* a0), (a0))                                                                                           \
    X(G, GXGetProjectionv, (f32* a0), (a0))                                                                                         \
    X(G, GXGetScissor, (u32* a0, u32* a1, u32* a2, u32* a3), (a0, a1, a2, a3))                                                      \
    X(G, GXPeekARGB, (u16 a0, u16 a1, u32* a2), (a0, a1, a2))                                                                       \
    X(G, GXPeekZ, (u16 a0, u16 a1, u32* a2), (a0, a1, a2))                                                                          \
    X(T, port_gx_draw_serial, (void), ())                                                                                           \
    X(T, port_gx_fx_begin, (void), ())                                                                                              \
    X(S, port_gx_fx_end, (void), ())

// (cold where the GX library sees them: its entry points keep the call out of their straight path. Not in gx_capture.cpp itself
// (PORT_GXCAP_IMPL): while a capture is open the recorders run for every call and must be compiled to be fast, dpengine's A0.)
#if defined(PORT_GXCAP_IMPL)
#define PORT_GXCAP_DECLARE_R(n, p) int port_gxcap_##n p;
#else
#define PORT_GXCAP_DECLARE_R(n, p) int port_gxcap_##n p __attribute__((cold));
#endif
#define PORT_GXCAP_DECLARE_T(n, p)
#define PORT_GXCAP_DECLARE(k, n, p, a) PORT_GXCAP_DECLARE_##k(n, p)
#define PORT_GXCAP_DECLARE_M PORT_GXCAP_DECLARE_R
#define PORT_GXCAP_DECLARE_P PORT_GXCAP_DECLARE_R
#define PORT_GXCAP_DECLARE_G PORT_GXCAP_DECLARE_R
#define PORT_GXCAP_DECLARE_S PORT_GXCAP_DECLARE_R
PORT_GXCAP_RECORD_LIST(PORT_GXCAP_DECLARE)
#undef PORT_GXCAP_DECLARE
u32 port_gxcap_port_gx_draw_serial(void) __attribute__((cold));
int port_gxcap_port_gx_fx_begin(void) __attribute__((cold));
#endif

#define PORT_GXCAP_ENUM_R(k, n, p, a) PORT_GXCAP_ID_##n,
#define PORT_GXCAP_ENUM_A(n) PORT_GXCAP_ID_##n,
// (the abandon ids first: they are the same in every unit, with or without the GX types)
enum {
    PORT_GXCAP_ABANDON_LIST(PORT_GXCAP_ENUM_A)
    PORT_GXCAP_FIRST_RECORD,
#if defined(PORT_GXCAP_HOOKS)
    PORT_GXCAP_RECORD_LIST(PORT_GXCAP_ENUM_R)
#endif
    PORT_GXCAP_ID_END
};
#undef PORT_GXCAP_ENUM_R
#undef PORT_GXCAP_ENUM_A
void port_gxcap_abandon_entry(int id);

#if defined(TARGET_PC) && defined(PORT_GXCAP_HOOKS)
// GXTexture.c: what GXLoadTexObj / GXLoadTlut write back into the caller's object (the register ids of the map; the TLUT's TMEM
// offset), done at record time so that the game sees its objects as the serial call would leave them.
void __PortGXTexObjSetIds(GXTexObj* obj, GXTexMapID id);
int __PortGXTlutObjSetOffset(GXTlutObj* obj, u32 tlut_name);  // 0: the region callback is not the library's (abandon)
int __PortGXRegionDefault(void);  // 1: the texture and TLUT region callbacks are the ones GXInit set
// GXLight.c: the light dedup shadow (GXV_LIGHT_DEDUP), saved and restored around mode 2's scratch replay.
enum { PORT_GXCAP_LIGHT_STATE_BYTES = 8 * 13 * 4 + 24 };
void __PortGXLightStateSave(void* out);
void __PortGXLightStateLoad(const void* in);
#endif

#ifdef __cplusplus
}
#endif
