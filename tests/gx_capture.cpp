// GX capture host proof (tools/test_gx_capture.sh). The console's GX library (decomp RVL_SDK/gx, the production units) and the capture
// layer (platform/src/gx/gx_capture.cpp) with a FIFO that stands for the stream (tests/gx_capture_stubs.c). Every check runs the same
// sequence of game-like operations twice from the same GX state: directly (today) and through the layer; the stream bytes, every value
// the "game" read back (getters, the draw serial, peeks, return values) and the library's state at the end must be equal.
//   a  random and scripted sequences over all the entry points with a hook, immediate vertices and raw FIFO writes (J3D style), the
//      light dedup's scopes and LightFunction's memo, tokens whose "pass" is drawn at replay by the handler; captures begun and ended
//      at random points, replayed at once
//   b  every pointer argument the layer copies is overwritten right after the call (and again, with GXCAP_THREAD, while the replay runs)
//   c  getters: answered from the mirror; after a token unless the token keeps them (then abandon); mode 2 compares them
//   d  an abandon at every position of a sequence
//   e  GXCAP_THREAD=1: the replay on a second thread while the first rewrites the copied pointees (ThreadSanitizer build)
//   self-test: GXV_GXCAPTURE_SELFTEST=1, the layer cuts the run into captures itself (turn points at random)
// Mode (GXV_GXCAPTURE=1 / 2), GXV_GXCAPTURE_SYNC, GXV_GXCAPTURE_SELFTEST(_CALLS) come from the environment as on the console.
#define PORT_GXCAP_HOOKS 1
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include "revolution/types.h"
#include "revolution/gx.h"
#include "revolution/gx/GXRegs.h"
#include "port/gx_capture.h"
#include "port/gx_fifo.h"
#include "port/light_memo.h"

extern "C" {
void hx_fifo_reset(void);
const uint8_t* hx_stream(size_t* n);
uint8_t* hx_arena(void);
extern int hx_fifo_small, hx_retained, hx_bracket, hx_peek_const, hx_quiet;
extern volatile u32 __PortGXPERegs[0x400];
extern u32 __PortGXDrawSerial;
extern int __PortGXLightScope;
extern u32 __PortGXLightGen;
void port_gx_call_prologue(void);
u32 port_gx_draw_serial(void);
void __PortGXLightShadowDrop(void);
void GXSetViewportJitter(f32, f32, f32, f32, f32, f32, u32);
void port_gxcap_turn(void);
const char* port_gxcap_first_diff(void);
void port_gxcap_test_null(int on);
int port_gxcap_test_owner(void);
void port_gx_replay_arm(uintptr_t stackLo, uintptr_t stackSpan);
extern int hx_irq_foreign;
void hx_irq_main(void);
unsigned long long port_gxcap_test_fast_calls(void);
uint64_t port_gxcap_abandons_by(int id);
const char* port_gxcap_name(int id);
int port_on_frame_thread(void);
void port_log_summary(const char* fmt, ...);
}
static std::thread::id sMainThread;
// The trap test (GXCAP_TRAPTEST): what the draw pipeline's port_drawpipe_game_enter does for the layer: a GX call of the
// game thread beside a replay on another thread waits for that replay's end before it goes on.
static std::atomic<int> sTrapEnters{0}, sTrapReplayDone{0}, sTrapInToken{0}, sTrapForeignBodies{0};
extern "C" void port_drawpipe_game_enter(void) {
    if (std::this_thread::get_id() != sMainThread) return;  // (the replaying thread itself: nothing)
    sTrapEnters.fetch_add(1);
    while (!sTrapReplayDone.load()) std::this_thread::yield();
}
extern "C" int port_on_frame_thread(void) { return std::this_thread::get_id() == sMainThread ? 1 : 0; }
extern "C" void port_log_summary(const char* fmt, ...) {
    if (hx_quiet) return;
    va_list ap;
    va_start(ap, fmt);
    printf("log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

namespace {

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<uint32_t>(s >> 11);
    }
    uint32_t below(uint32_t n) { return n ? next() % n : 0; }
    f32 f() { return static_cast<f32>(static_cast<int32_t>(next() % 20001) - 10000) / 97.0f; }
    bool coin() { return next() & 1; }
};

// ---- the GX state a run starts from, and what is compared at the end
struct State {
    GXData gxd;
    uint8_t light[PORT_GXCAP_LIGHT_STATE_BYTES];
    u32 serial;
    u32 pe[16];
    void take() {
        memcpy(&gxd, __GXData, sizeof gxd);
        __PortGXLightStateSave(light);
        serial = __PortGXDrawSerial;
        for (int i = 0; i < 16; ++i) pe[i] = __PortGXPERegs[i];
    }
    void put() const {
        memcpy(__GXData, &gxd, sizeof gxd);
        __PortGXLightStateLoad(light);
        __PortGXDrawSerial = serial;
        for (int i = 0; i < 16; ++i) __PortGXPERegs[i] = pe[i];
    }
};
State sInit;

// The game-side buffers whose addresses the stream carries (stable between runs) and the ones passed by pointer and copied.
uint8_t* arena() { return hx_arena(); }
uint8_t* arenaAt(uint32_t k) { return arena() + 4096 + (k % 1024) * 1024; }

// The game's light memo (decomp/src/Game/Map/LightFunction.cpp, the same logic): scopes and loadActorLightInfo.
port::LightMemo sMemo;
int scopeEnter(int level, int mode) {
    if (mode < level) return 0;
    if (__PortGXLightScope > 0) {
        ++__PortGXLightScope;
        return 2;
    }
    __PortGXLightShadowDrop();
    ++__PortGXLightScope;
    return 1;
}
void scopeExit(int opened) {
    if (!opened) return;
    --__PortGXLightScope;
    if (opened != 1) return;
    __PortGXLightShadowDrop();
    port::LightMemoDrop(sMemo);
}
void loadLight(const f32 pos[3], GXColor c, GXLightID id) {
    GXLightObj lo;
    GXInitLightPos(&lo, pos[0], pos[1], pos[2]);
    GXInitLightDir(&lo, 0.0f, 0.0f, -1.0f);
    GXInitLightAttn(&lo, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    GXInitLightColor(&lo, c);
    GXLoadLightObjImm(&lo, id);
}
struct ActorLight {
    f32 p0[3], p1[3];
    u8 c0[4], c1[4], amb[4];
};
const ActorLight kActorLights[3] = {
    {{1, 2, 3}, {4, 5, 6}, {10, 20, 30, 255}, {40, 50, 60, 255}, {1, 2, 3, 4}},
    {{-1, 2, 3}, {4, -5, 6}, {11, 21, 31, 255}, {41, 51, 61, 255}, {5, 6, 7, 8}},
    {{9, 9, 9}, {8, 8, 8}, {12, 22, 32, 255}, {42, 52, 62, 255}, {9, 9, 9, 9}},
};
const f32 kView[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
void loadActorLightInfo(int which) {
    const ActorLight& a = kActorLights[which];
    bool memo = false;
    if (__PortGXLightScope > 0) {
        memo = true;
        if (port::LightMemoSame(sMemo, __PortGXLightGen, &a, sizeof a, kView, sizeof kView)) {
            GXSetChanAmbColor(GX_COLOR0A0, GXColor{a.amb[0], a.amb[1], a.amb[2], a.amb[3]});
            return;
        }
    }
    loadLight(a.p0, GXColor{a.c0[0], a.c0[1], a.c0[2], a.c0[3]}, GX_LIGHT0);
    loadLight(a.p1, GXColor{a.c1[0], a.c1[1], a.c1[2], a.c1[3]}, GX_LIGHT1);
    const f32 rim[3] = {0, 0, 0};
    loadLight(rim, GXColor{0, 0, 0, a.c0[3]}, GX_LIGHT2);
    GXSetChanAmbColor(GX_COLOR0A0, GXColor{a.amb[0], a.amb[1], a.amb[2], a.amb[3]});
    if (memo) port::LightMemoDone(sMemo, __PortGXLightGen);
}

// ---- operations
enum Kind {
    // recorded by value
    K_TEV, K_TEVK, K_TEVCOLOR, K_ALPHA, K_BLEND, K_ZMODE, K_VTXDESC, K_VTXFMT, K_ARRAY, K_TEXGEN, K_CHAN, K_CHANCOLOR, K_NUMS, K_CULL,
    K_CURMTX, K_IND, K_FOG, K_COPYSET, K_COPYTEX, K_COPYDISP, K_MISC, K_POKE, K_SYNCS, K_INVAL, K_LINE, K_CLIP, K_ZSCALE, K_SWAP,
    K_TEVS10, K_TEXSCALE, K_TEXMTXIDX, K_CALLDL, K_PROLOGUE, K_RETAINED, K_BRACKETED, K_FLUSH, K_PIXFMT, K_FIELD,
    // mirrored
    K_VIEWPORT, K_SCISSOR, K_PROJ, K_PROJV,
    // pointees copied
    K_POSMTX, K_NRMMTX, K_TEXMTX, K_INDMTX, K_FOGADJ, K_COPYFILTER, K_VATV, K_LIGHT, K_TEXOBJ, K_TLUT,
    // immediate mode and raw writes
    K_BEGIN, K_RAW, K_BIGRAW,
    // lights (LightFunction)
    K_SCOPE, K_ACTORLIGHT,
    // reads
    K_GETVP, K_GETPROJ, K_GETSCIS, K_PEEK, K_SERIAL, K_FX,
    // tokens and abandons
    K_TOKEN, K_ABANDON,
    K_COUNT
};
const Kind kPassKinds[] = {K_TEV, K_TEVK, K_TEVCOLOR, K_ALPHA, K_BLEND, K_ZMODE, K_VTXDESC, K_VTXFMT, K_ARRAY, K_TEXGEN, K_CHAN,
                           K_CHANCOLOR, K_NUMS, K_CULL, K_CURMTX, K_CALLDL, K_PROLOGUE, K_RETAINED, K_BRACKETED, K_POSMTX, K_NRMMTX,
                           K_TEXMTX, K_LIGHT, K_TEXOBJ, K_BEGIN, K_RAW, K_ACTORLIGHT, K_SWAP, K_TEVS10};

struct Op {
    uint8_t kind;
    uint32_t seed;
};

struct Ctx {
    std::vector<uint32_t> log;   // what the game read back
    bool mutate = false;          // overwrite copied pointees right after the call
    bool capturing = false;       // (tokens go through the layer)
    int lightMode = 2;            // GXV_LIGHT_DEDUP
    std::vector<int> scopes;      // open light scopes
    uint32_t keepAll = 0;
    // game-side buffers passed by pointer (copied by the layer)
    f32 mtx[3][4], proj[4][4], projv[7], ind[2][3];
    GXFogAdjTable fog;
    u8 pattern[12][2], vfilter[7];
    GXVtxAttrFmtList vat[6];
    GXLightObj lobj;
    GXTexObj tobj[4];
    GXTlutObj tlut;
};

void scribble(void* p, size_t n, uint32_t seed) {
    uint8_t* b = static_cast<uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(seed * 131 + i * 7 + 0x5A);
}

void execPass(uint32_t id);

void exec(const Op& op, Ctx& c) {
    Rng r(op.seed);
    switch (op.kind) {
    case K_TEV: {
        const GXTevStageID st = static_cast<GXTevStageID>(r.below(16));
        GXSetTevOrder(st, static_cast<GXTexCoordID>(r.coin() ? r.below(8) : 0xFF), static_cast<GXTexMapID>(r.coin() ? r.below(8) : 0xFF),
                      static_cast<GXChannelID>(r.coin() ? 4 : 0xFF));
        GXSetTevColorIn(st, static_cast<GXTevColorArg>(r.below(16)), static_cast<GXTevColorArg>(r.below(16)), static_cast<GXTevColorArg>(r.below(16)),
                        static_cast<GXTevColorArg>(r.below(16)));
        GXSetTevAlphaIn(st, static_cast<GXTevAlphaArg>(r.below(8)), static_cast<GXTevAlphaArg>(r.below(8)), static_cast<GXTevAlphaArg>(r.below(8)),
                        static_cast<GXTevAlphaArg>(r.below(8)));
        GXSetTevColorOp(st, static_cast<GXTevOp>(r.below(2)), static_cast<GXTevBias>(r.below(3)), static_cast<GXTevScale>(r.below(4)), r.coin(),
                        static_cast<GXTevRegID>(r.below(4)));
        GXSetTevAlphaOp(st, static_cast<GXTevOp>(r.below(2)), static_cast<GXTevBias>(r.below(3)), static_cast<GXTevScale>(r.below(4)), r.coin(),
                        static_cast<GXTevRegID>(r.below(4)));
        GXSetTevOp(static_cast<GXTevStageID>(r.below(16)), static_cast<GXTevMode>(r.below(5)));
        GXSetNumTevStages(static_cast<u8>(1 + r.below(16)));
        GXSetTevDirect(static_cast<GXTevStageID>(r.below(16)));
        break;
    }
    case K_TEVK:
        GXSetTevKColor(static_cast<GXTevKColorID>(r.below(4)), GXColor{(u8)r.next(), (u8)r.next(), (u8)r.next(), (u8)r.next()});
        GXSetTevKColorSel(static_cast<GXTevStageID>(r.below(16)), static_cast<GXTevKColorSel>(r.below(32)));
        GXSetTevKAlphaSel(static_cast<GXTevStageID>(r.below(16)), static_cast<GXTevKAlphaSel>(r.below(32)));
        break;
    case K_TEVCOLOR:
        GXSetTevColor(static_cast<GXTevRegID>(r.below(4)), GXColor{(u8)r.next(), (u8)r.next(), (u8)r.next(), (u8)r.next()});
        break;
    case K_TEVS10:
        GXSetTevColorS10(static_cast<GXTevRegID>(r.below(4)), GXColorS10{(s16)(r.below(2048) - 1024), (s16)r.below(1024), (s16)r.below(512), (s16)r.below(256)});
        break;
    case K_SWAP:
        GXSetTevSwapMode(static_cast<GXTevStageID>(r.below(16)), static_cast<GXTevSwapSel>(r.below(4)), static_cast<GXTevSwapSel>(r.below(4)));
        GXSetTevSwapModeTable(static_cast<GXTevSwapSel>(r.below(4)), static_cast<GXTevColorChan>(r.below(4)), static_cast<GXTevColorChan>(r.below(4)),
                              static_cast<GXTevColorChan>(r.below(4)), static_cast<GXTevColorChan>(r.below(4)));
        break;
    case K_ALPHA:
        GXSetAlphaCompare(static_cast<GXCompare>(r.below(8)), (u8)r.next(), static_cast<GXAlphaOp>(r.below(4)), static_cast<GXCompare>(r.below(8)), (u8)r.next());
        GXSetZCompLoc(r.coin());
        GXSetDstAlpha(r.coin(), (u8)r.next());
        break;
    case K_BLEND:
        GXSetBlendMode(static_cast<GXBlendMode>(r.below(4)), static_cast<GXBlendFactor>(r.below(8)), static_cast<GXBlendFactor>(r.below(8)),
                       static_cast<GXLogicOp>(r.below(16)));
        GXSetColorUpdate(r.coin());
        GXSetAlphaUpdate(r.coin());
        GXSetDither(r.coin());
        break;
    case K_ZMODE:
        GXSetZMode(r.coin(), static_cast<GXCompare>(r.below(8)), r.coin());
        GXSetZTexture(static_cast<GXZTexOp>(r.below(3)), r.coin() ? GX_TF_Z8 : GX_TF_Z24X8, r.below(1 << 20));
        break;
    case K_VTXDESC:
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        if (r.coin()) GXSetVtxDesc(GX_VA_CLR0, static_cast<GXAttrType>(1 + r.below(3)));
        if (r.coin()) GXSetVtxDesc(GX_VA_TEX0, static_cast<GXAttrType>(1 + r.below(3)));
        if (r.coin()) GXSetVtxDesc(GX_VA_NRM, static_cast<GXAttrType>(1 + r.below(3)));
        if (r.coin()) GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
        break;
    case K_VTXFMT:
        GXSetVtxAttrFmt(static_cast<GXVtxFmt>(r.below(8)), GX_VA_POS, static_cast<GXCompCnt>(r.below(2)), static_cast<GXCompType>(r.below(5)), (u8)r.below(16));
        GXSetVtxAttrFmt(static_cast<GXVtxFmt>(r.below(8)), GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        GXSetVtxAttrFmt(static_cast<GXVtxFmt>(r.below(8)), GX_VA_TEX0, GX_TEX_ST, static_cast<GXCompType>(r.below(5)), (u8)r.below(16));
        GXInvalidateVtxCache();
        break;
    case K_ARRAY:
        GXSetArray(static_cast<GXAttr>(GX_VA_POS + r.below(12)), arenaAt(r.next()), (u8)(4 + r.below(60)));
        break;
    case K_TEXGEN: {
        static const u32 mtx[] = {GX_TEXMTX0, GX_TEXMTX1, GX_TEXMTX5, GX_IDENTITY};
        GXSetTexCoordGen2(static_cast<GXTexCoordID>(r.below(8)), static_cast<GXTexGenType>(r.below(2)), static_cast<GXTexGenSrc>(GX_TG_TEX0 + r.below(4)),
                          mtx[r.below(4)], r.coin(), r.coin() ? GX_PTIDENTITY : GX_PTTEXMTX0);
        GXSetNumTexGens(static_cast<u8>(r.below(9)));
        break;
    }
    case K_CHAN:
        GXSetChanCtrl(static_cast<GXChannelID>(r.below(6)), r.coin(), static_cast<GXColorSrc>(r.below(2)), static_cast<GXColorSrc>(r.below(2)),
                      r.below(256), static_cast<GXDiffuseFn>(r.below(3)), static_cast<GXAttnFn>(r.below(3)));
        GXSetNumChans(static_cast<u8>(r.below(3)));
        break;
    case K_CHANCOLOR:
        GXSetChanAmbColor(static_cast<GXChannelID>(4 + r.below(2)), GXColor{(u8)r.next(), (u8)r.next(), (u8)r.next(), (u8)r.next()});
        GXSetChanMatColor(static_cast<GXChannelID>(4 + r.below(2)), GXColor{(u8)r.next(), (u8)r.next(), (u8)r.next(), (u8)r.next()});
        break;
    case K_NUMS:
        GXSetNumIndStages(static_cast<u8>(r.below(5)));
        GXSetCoPlanar(r.coin());
        break;
    case K_CULL:
        GXSetCullMode(static_cast<GXCullMode>(r.below(4)));
        break;
    case K_CURMTX:
        GXSetCurrentMtx(3 * r.below(10));
        break;
    case K_IND:
        GXSetIndTexOrder(static_cast<GXIndTexStageID>(r.below(4)), static_cast<GXTexCoordID>(r.below(8)), static_cast<GXTexMapID>(r.below(8)));
        GXSetIndTexCoordScale(static_cast<GXIndTexStageID>(r.below(4)), static_cast<GXIndTexScale>(r.below(9)), static_cast<GXIndTexScale>(r.below(9)));
        GXSetTevIndirect(static_cast<GXTevStageID>(r.below(16)), static_cast<GXIndTexStageID>(r.below(4)), static_cast<GXIndTexFormat>(r.below(4)),
                         static_cast<GXIndTexBiasSel>(r.below(8)), static_cast<GXIndTexMtxID>(r.below(4)), static_cast<GXIndTexWrap>(r.below(7)),
                         static_cast<GXIndTexWrap>(r.below(7)), r.coin(), r.coin(), static_cast<GXIndTexAlphaSel>(r.below(4)));
        GXSetTevIndWarp(static_cast<GXTevStageID>(r.below(16)), static_cast<GXIndTexStageID>(r.below(4)), r.coin(), r.coin(),
                        static_cast<GXIndTexMtxID>(1 + r.below(3)));
        break;
    case K_FOG:
        GXSetFog(static_cast<GXFogType>(r.coin() ? 2 : 0), r.f(), r.f() + 200.0f, 1.0f + r.below(10), 10000.0f, GXColor{(u8)r.next(), 1, 2, 3});
        break;
    case K_COPYSET:
        GXSetTexCopySrc((u16)r.below(320), (u16)r.below(240), (u16)(16 + r.below(320)), (u16)(16 + r.below(240)));
        GXSetTexCopyDst((u16)(16 + r.below(320)), (u16)(16 + r.below(240)), r.coin() ? GX_TF_RGBA8 : GX_TF_I8, r.coin());
        GXSetDispCopySrc(0, 0, 640, 456);
        GXSetDispCopyDst(640, 456);
        GXSetDispCopyGamma(static_cast<GXGamma>(r.below(3)));
        GXSetDispCopyFrame2Field(static_cast<GXCopyMode>(r.below(2)));
        GXSetCopyClamp(static_cast<GXFBClamp>(r.below(4)));
        GXSetCopyClear(GXColor{(u8)r.next(), 2, 3, 4}, r.below(0xFFFFFF));
        break;
    case K_COPYTEX:
        GXCopyTex(arenaAt(r.next()), r.coin());
        break;
    case K_COPYDISP:
        GXCopyDisp(arenaAt(r.next()), r.coin());
        break;
    case K_MISC:
        GXSetMisc(GX_MT_XF_FLUSH, r.coin() ? 0 : 8);
        break;
    case K_POKE:
        GXPokeAlphaMode(static_cast<GXCompare>(r.below(8)), (u8)r.next());
        GXPokeAlphaRead(static_cast<GXAlphaReadMode>(r.below(3)));
        GXPokeAlphaUpdate(r.coin());
        GXPokeBlendMode(static_cast<GXBlendMode>(r.below(4)), static_cast<GXBlendFactor>(r.below(8)), static_cast<GXBlendFactor>(r.below(8)),
                        static_cast<GXLogicOp>(r.below(16)));
        GXPokeColorUpdate(r.coin());
        GXPokeDither(r.coin());
        GXPokeDstAlpha(r.coin(), (u8)r.next());
        GXPokeZMode(r.coin(), static_cast<GXCompare>(r.below(8)), r.coin());
        break;
    case K_SYNCS:
        GXPixModeSync();
        GXTexModeSync();
        break;
    case K_INVAL:
        GXInvalidateTexAll();
        GXClearBoundingBox();
        break;
    case K_LINE:
        GXSetLineWidth((u8)r.next(), static_cast<GXTexOffset>(r.below(6)));
        GXSetPointSize((u8)r.next(), static_cast<GXTexOffset>(r.below(6)));
        GXEnableTexOffsets(static_cast<GXTexCoordID>(r.below(8)), r.coin(), r.coin());
        break;
    case K_CLIP:
        GXSetClipMode(static_cast<GXClipMode>(r.below(2)));
        GXSetScissorBoxOffset((s32)r.below(64) - 32, (s32)r.below(64) - 32);
        break;
    case K_ZSCALE:
        GXSetZScaleOffset(r.f() / 100.0f, r.f() / 100.0f);
        break;
    case K_TEXSCALE:
        GXSetTexCoordScaleManually(static_cast<GXTexCoordID>(r.below(8)), r.coin(), (u16)(1 + r.below(1024)), (u16)(1 + r.below(1024)));
        break;
    case K_TEXMTXIDX:
        GXLoadTexMtxIndx((u16)r.below(256), GX_TEXMTX0 + 3 * r.below(10), r.coin() ? GX_MTX2x4 : GX_MTX3x4);
        break;
    case K_CALLDL:
        GXCallDisplayList(arenaAt(r.next()), 32 * (1 + r.below(64)));
        break;
    case K_PROLOGUE:
        port_gx_call_prologue();
        break;
    case K_RETAINED:
        port_gx_call_retained(arenaAt(r.next()), 32 * (1 + r.below(8)), r.coin() ? 0 : r.next());
        break;
    case K_BRACKETED:
        port_gx_call_bracketed(arenaAt(r.next()), 32 * (1 + r.below(8)), r.next(), r.next(), r.next(), r.next(), r.next());
        break;
    case K_FLUSH:
        GXFlush();
        break;
    case K_PIXFMT:
        GXSetPixelFmt(static_cast<GXPixelFmt>(r.below(3)), static_cast<GXZFmt16>(r.below(4)));
        break;
    case K_FIELD:
        GXSetFieldMask(r.coin(), r.coin());
        GXSetFieldMode(r.coin(), r.coin());
        break;
    case K_VIEWPORT:
        if (r.coin()) GXSetViewport(r.f(), r.f(), 100 + r.f(), 100 + r.f(), 0.0f, 1.0f);
        else GXSetViewportJitter(r.f(), r.f(), 100 + r.f(), 100 + r.f(), 0.0f, 1.0f, r.below(2));
        break;
    case K_SCISSOR:
        GXSetScissor(r.below(640), r.below(480), 1 + r.below(640), 1 + r.below(480));
        break;
    case K_PROJ:
        for (int i = 0; i < 16; ++i) (&c.proj[0][0])[i] = r.f();
        GXSetProjection(c.proj, r.coin() ? GX_PERSPECTIVE : GX_ORTHOGRAPHIC);
        if (c.mutate) scribble(c.proj, sizeof c.proj, op.seed);
        break;
    case K_PROJV:
        c.projv[0] = r.coin() ? 0.0f : 1.0f;
        for (int i = 1; i < 7; ++i) c.projv[i] = r.f();
        GXSetProjectionv(c.projv);
        if (c.mutate) scribble(c.projv, sizeof c.projv, op.seed);
        break;
    case K_POSMTX:
    case K_NRMMTX:
    case K_TEXMTX:
        for (int i = 0; i < 12; ++i) (&c.mtx[0][0])[i] = r.f();
        if (op.kind == K_POSMTX) GXLoadPosMtxImm(c.mtx, 3 * r.below(10));
        else if (op.kind == K_NRMMTX) GXLoadNrmMtxImm(c.mtx, 3 * r.below(10));
        else GXLoadTexMtxImm(c.mtx, r.coin() ? GX_TEXMTX0 + 3 * r.below(10) : GX_PTTEXMTX0 + 3 * r.below(20), r.coin() ? GX_MTX2x4 : GX_MTX3x4);
        if (c.mutate) scribble(c.mtx, sizeof c.mtx, op.seed);
        break;
    case K_INDMTX:
        for (int i = 0; i < 6; ++i) (&c.ind[0][0])[i] = r.f() / 200.0f;
        GXSetIndTexMtx(static_cast<GXIndTexMtxID>(1 + r.below(3)), c.ind, static_cast<s8>(r.below(32) - 16));
        if (c.mutate) scribble(c.ind, sizeof c.ind, op.seed);
        break;
    case K_FOGADJ: {
        for (int i = 0; i < 10; ++i) c.fog.r[i] = (u16)r.below(4096);
        const GXBool en = r.coin();
        GXSetFogRangeAdj(en, (u16)r.below(640), r.coin() || en ? &c.fog : nullptr);
        if (c.mutate) scribble(&c.fog, sizeof c.fog, op.seed);
        break;
    }
    case K_COPYFILTER: {
        for (int i = 0; i < 24; ++i) (&c.pattern[0][0])[i] = (u8)r.below(12);
        for (int i = 0; i < 7; ++i) c.vfilter[i] = (u8)r.below(64);
        const GXBool aa = r.coin(), vf = r.coin();
        GXSetCopyFilter(aa, aa || r.coin() ? c.pattern : nullptr, vf, vf || r.coin() ? c.vfilter : nullptr);
        if (c.mutate) {
            scribble(c.pattern, sizeof c.pattern, op.seed);
            scribble(c.vfilter, sizeof c.vfilter, op.seed + 1);
        }
        break;
    }
    case K_VATV: {
        const int n = static_cast<int>(r.below(5));
        static const GXAttr attrs[] = {GX_VA_POS, GX_VA_NRM, GX_VA_CLR0, GX_VA_TEX0, GX_VA_TEX1};
        for (int i = 0; i < n; ++i) {
            c.vat[i].attr = attrs[i];
            c.vat[i].cnt = static_cast<GXCompCnt>(attrs[i] == GX_VA_CLR0 ? GX_CLR_RGBA : r.below(2));
            c.vat[i].type = static_cast<GXCompType>(attrs[i] == GX_VA_CLR0 ? GX_RGBA8 : r.below(5));
            c.vat[i].frac = (u8)r.below(16);
        }
        c.vat[n].attr = GX_VA_NULL;
        GXSetVtxAttrFmtv(static_cast<GXVtxFmt>(r.below(8)), c.vat);
        if (c.mutate) scribble(c.vat, sizeof c.vat, op.seed);
        break;
    }
    case K_LIGHT:
        GXInitLightPos(&c.lobj, r.f(), r.f(), r.f());
        GXInitLightDir(&c.lobj, r.f(), r.f(), r.f());
        GXInitLightAttn(&c.lobj, r.f(), r.f(), r.f(), r.f(), r.f(), r.f());
        GXInitLightColor(&c.lobj, GXColor{(u8)r.next(), (u8)r.next(), (u8)r.next(), 255});
        GXLoadLightObjImm(&c.lobj, static_cast<GXLightID>(1u << r.below(8)));
        if (c.mutate) scribble(&c.lobj, sizeof c.lobj, op.seed);
        break;
    case K_TEXOBJ: {
        GXTexObj& t = c.tobj[r.below(4)];
        static const GXTexFmt fmts[] = {GX_TF_I8, GX_TF_RGBA8, GX_TF_RGB565, GX_TF_CMPR};
        GXInitTexObj(&t, arenaAt(r.next()), (u16)(8 << r.below(5)), (u16)(8 << r.below(5)), fmts[r.below(4)], static_cast<GXTexWrapMode>(r.below(3)),
                     static_cast<GXTexWrapMode>(r.below(3)), r.coin());
        GXInitTexObjLOD(&t, static_cast<GXTexFilter>(r.below(6)), static_cast<GXTexFilter>(r.below(2)), 0.0f, (f32)r.below(8), r.f() / 100.0f, r.coin(),
                        r.coin(), static_cast<GXAnisotropy>(r.below(3)));
        GXLoadTexObj(&t, static_cast<GXTexMapID>(r.below(8)));
        c.log.push_back(reinterpret_cast<const u32*>(&t)[0]);  // (the game sees its object as the library left it: the register ids)
        c.log.push_back(reinterpret_cast<const u32*>(&t)[6]);
        if (c.mutate) scribble(&t, sizeof t, op.seed);
        break;
    }
    case K_TLUT: {
        GXInitTlutObj(&c.tlut, arenaAt(r.next()), static_cast<GXTlutFmt>(r.below(3)), (u16)(16 << r.below(4)));
        GXLoadTlut(&c.tlut, r.below(20));
        c.log.push_back(reinterpret_cast<const u32*>(&c.tlut)[0]);  // (the TMEM offset GXLoadTlut wrote into it)
        GXTexObj ci;
        GXInitTexObjCI(&ci, arenaAt(r.next()), 32, 32, GX_TF_C8, GX_CLAMP, GX_CLAMP, GX_FALSE, r.below(16));
        GXLoadTexObj(&ci, static_cast<GXTexMapID>(r.below(8)));
        if (c.mutate) scribble(&c.tlut, sizeof c.tlut, op.seed);
        break;
    }
    case K_BEGIN: {
        const u16 n = (u16)(1 + r.below(24));
        static const GXPrimitive prims[] = {GX_QUADS, GX_TRIANGLES, GX_TRIANGLESTRIP, GX_POINTS, GX_LINES};
        GXBegin(prims[r.below(5)], static_cast<GXVtxFmt>(r.below(8)), n);
        for (u16 i = 0; i < n; ++i) {
            GXPosition3f32(r.f(), r.f(), r.f());
            if (i & 1) GXColor4u8((u8)r.next(), (u8)r.next(), (u8)r.next(), 255);
            else GXColor1u32(r.next());
            GXTexCoord2f32(r.f(), r.f());
            if (i % 3 == 0) GXPosition1x8((u8)r.next());
        }
        GXEnd();
        break;
    }
    case K_RAW: {
        const int n = static_cast<int>(1 + r.below(12));
        for (int i = 0; i < n; ++i) {
            switch (r.below(5)) {
            case 0: __PortGXFifo_op_u8_u32(0x08, (u8)r.next(), r.next()); break;          // J3DFifoLoadCPCmd
            case 1: __PortGXFifo_op_u16_u16(0x20, (u16)r.next(), (u16)r.next()); break;   // J3DFifoLoadIndx
            case 2: GXCmd1u8(0x61); GXCmd1u32(r.next()); break;                           // a BP write (port_gx_marker)
            case 3: __PortGXFifo_op_u32_u32(0x10, r.next(), r.next()); break;              // an XF register write
            default: GXCmd1u16((u16)r.next()); GXCmd1f32(r.f()); break;
            }
        }
        break;
    }
    case K_BIGRAW: {  // (more than a capture chunk)
        GXBegin(GX_POINTS, GX_VTXFMT0, 0xFFFF);
        for (int i = 0; i < 9000; ++i) GXPosition3f32(r.f(), r.f(), r.f());
        break;
    }
    case K_SCOPE:
        if (!c.scopes.empty() && r.coin()) {
            scopeExit(c.scopes.back());
            c.scopes.pop_back();
        } else if (c.scopes.size() < 3) {
            c.scopes.push_back(scopeEnter(1 + (int)r.below(2), c.lightMode));
        }
        break;
    case K_ACTORLIGHT:
        loadActorLightInfo(static_cast<int>(r.below(3)));
        break;
    case K_GETVP: {
        f32 vp[6];
        GXGetViewportv(vp);
        for (f32 v : vp) {
            u32 w;
            memcpy(&w, &v, 4);
            c.log.push_back(w);
        }
        GXSetViewport(vp[0] + 1.0f, vp[1], vp[2], vp[3], vp[4], vp[5]);  // (as a game saving and changing it would: the stream depends on the answer)
        break;
    }
    case K_GETPROJ: {
        f32 p[7];
        GXGetProjectionv(p);
        for (f32 v : p) {
            u32 w;
            memcpy(&w, &v, 4);
            c.log.push_back(w);
        }
        p[2] += 0.5f;
        GXSetProjectionv(p);
        break;
    }
    case K_GETSCIS: {
        u32 a, b, w, h;
        GXGetScissor(&a, &b, &w, &h);
        c.log.push_back(a);
        c.log.push_back(b);
        c.log.push_back(w);
        c.log.push_back(h);
        GXSetScissor(a, b, w, h > 2 ? h - 1 : h);
        break;
    }
    case K_PEEK: {
        u32 v = 0, z = 0;
        GXPeekARGB((u16)r.below(640), (u16)r.below(480), &v);
        GXPeekZ((u16)r.below(640), (u16)r.below(480), &z);
        c.log.push_back(v);
        c.log.push_back(z);
        break;
    }
    case K_SERIAL: {
        const u32 s = port_gx_draw_serial();
        c.log.push_back(s);
        if (s & 1) GXSetCullMode(GX_CULL_BACK);  // (the bloom decides by it)
        break;
    }
    case K_FX:
        c.log.push_back(static_cast<u32>(port_gx_fx_begin ? port_gx_fx_begin() : 0));
        if (port_gx_fx_end) port_gx_fx_end();
        break;
    case K_TOKEN: {
        const uint32_t id = r.next() & 0xFFFF;
        if (c.capturing && port_gx_capture_open()) {  // (as the draw pipeline does: a token only into an open capture)
            if (r.below(3) == 0) port_gx_capture_token_keep(id, PORT_GXCAP_KEEP_VIEWPORT | PORT_GXCAP_KEEP_PROJECTION | PORT_GXCAP_KEEP_SCISSOR);
            else port_gx_capture_token(id);
        } else {
            execPass(id);
        }
        break;
    }
    case K_ABANDON:
        switch (r.below(5)) {
        case 0: c.log.push_back(GXSetDispCopyYScale(1.0f + r.below(2))); break;
        case 1: GXSetDrawDone(); break;
        case 2: port_gx_capture_abandon("test"); break;
        case 3: GXSetDrawSync((u16)r.next()); break;  // (abandons unless GXV_GXCAPTURE_SYNC=1)
        default: {
            const int keep = hx_peek_const;
            hx_peek_const = 0;  // (a renderer that reads the frame buffer: the peek abandons)
            u32 v = 0;
            GXPeekARGB(1, 2, &v);
            hx_peek_const = keep;
            c.log.push_back(v);
        }
        }
        break;
    }
}

// A token's pass (stands for a J3D pass: list calls, matrices, light set-ups inside a level-1 scope, raw writes; no viewport,
// projection or scissor change: the keep claims of K_TOKEN hold).
Ctx* sPassCtx = nullptr;
void execPass(uint32_t id) {
    Ctx local{};
    local.lightMode = sPassCtx ? sPassCtx->lightMode : 2;
    Rng r(id * 7919u + 17u);
    const int opened = scopeEnter(1, local.lightMode);
    const int n = static_cast<int>(4 + r.below(24));
    for (int i = 0; i < n; ++i) {
        Op op{static_cast<uint8_t>(kPassKinds[r.below(sizeof kPassKinds / sizeof kPassKinds[0])]), r.next()};
        exec(op, local);
    }
    scopeExit(opened);
}
void tokenHandler(uint32_t id, void*) { execPass(id); }

// ---- a run
struct Result {
    std::vector<uint8_t> stream;
    std::vector<uint32_t> log;
    State end;
};

void startRun(Ctx& c) {
    sInit.put();
    hx_fifo_reset();
    sMemo = port::LightMemo{0, 0, {}};
    memset(c.tobj, 0, sizeof c.tobj);
    __PortGXLightScope = 0;
}
Result finishRun(Ctx& c) {
    while (!c.scopes.empty()) {
        scopeExit(c.scopes.back());
        c.scopes.pop_back();
    }
    Result res;
    size_t n = 0;
    const uint8_t* s = hx_stream(&n);
    res.stream.assign(s, s + n);
    res.log = c.log;
    res.end.take();
    return res;
}

Result runDirect(const std::vector<Op>& ops, bool mutate) {
    Ctx c{};
    c.mutate = mutate;
    sPassCtx = &c;
    const int armed = __PortGXCap;
    __PortGXCap = 0;
    startRun(c);
    for (const Op& op : ops) exec(op, c);
    Result r = finishRun(c);
    __PortGXCap = armed;
    return r;
}

// Through the layer. `cuts`: where captures begin and end (op indices, alternating); `thread`: each replay on a second thread while
// this one overwrites the copied pointees.
Result runCaptured(const std::vector<Op>& ops, const std::vector<size_t>& cuts, bool mutate, bool thread, uint64_t* captures) {
    Ctx c{};
    c.mutate = mutate;
    c.capturing = true;
    sPassCtx = &c;
    startRun(c);
    size_t next = 0;
    bool open = false;
    for (size_t i = 0; i <= ops.size(); ++i) {
        while (next < cuts.size() && cuts[next] == i) {
            if (!open) {
                open = port_gx_capture_begin();
            } else {
                PortGxCapture* cap = port_gx_capture_end();
                open = false;
                if (cap) {
                    ++*captures;
                    if (thread) {
                        std::thread t([cap] { port_gx_replay(cap, tokenHandler, nullptr); });
                        // the game goes on with its own memory meanwhile: everything it passed by pointer and the layer copied
                        for (int k = 0; k < 200; ++k) {
                            scribble(c.mtx, sizeof c.mtx, k);
                            scribble(c.proj, sizeof c.proj, k);
                            scribble(&c.lobj, sizeof c.lobj, k);
                            scribble(c.tobj, sizeof c.tobj, k);
                            scribble(c.vat, sizeof c.vat, k);
                            scribble(&c.fog, sizeof c.fog, k);
                        }
                        t.join();
                    } else {
                        port_gx_replay(cap, tokenHandler, nullptr);
                    }
                }
            }
            ++next;
        }
        if (i < ops.size()) exec(ops[i], c);
    }
    if (open) {
        PortGxCapture* cap = port_gx_capture_end();
        if (cap) port_gx_replay(cap, tokenHandler, nullptr), ++*captures;
    }
    return finishRun(c);
}

// The self-test: the layer cuts the run itself; port_gxcap_turn at random op boundaries.
Result runSelftest(const std::vector<Op>& ops, uint32_t seed) {
    Ctx c{};
    c.capturing = false;  // (the self-test has no tokens: passes are drawn inline and captured as GX calls)
    sPassCtx = &c;
    startRun(c);
    Rng r(seed);
    port_gxcap_turn();
    for (const Op& op : ops) {
        if (r.below(8) == 0) port_gxcap_turn();
        exec(op, c);
    }
    port_gxcap_turn();
    __PortGXCap = 0;
    Result res = finishRun(c);
    return res;
}

int sFails = 0;
bool same(const Result& a, const Result& b, const char* what, uint64_t seed) {
    std::string why;
    if (a.stream != b.stream) {
        size_t i = 0;
        while (i < a.stream.size() && i < b.stream.size() && a.stream[i] == b.stream[i]) ++i;
        char buf[160];
        snprintf(buf, sizeof buf, "stream %zu vs %zu bytes, first difference at %zu", a.stream.size(), b.stream.size(), i);
        why = buf;
    } else if (a.log != b.log) {
        why = "what the game read back";
    } else if (memcmp(&a.end.gxd, &b.end.gxd, sizeof a.end.gxd) != 0) {
        size_t i = 0;
        const uint8_t* x = reinterpret_cast<const uint8_t*>(&a.end.gxd);
        const uint8_t* y = reinterpret_cast<const uint8_t*>(&b.end.gxd);
        while (x[i] == y[i]) ++i;
        char buf[96];
        snprintf(buf, sizeof buf, "GX state at the end (byte %zu of GXData)", i);
        why = buf;
    } else if (memcmp(a.end.light, b.end.light, 8 * 13 * 4 + 12) != 0) {  // (the counters after it are statistics)
        why = "light shadow at the end";
    } else if (a.end.serial != b.end.serial) {
        why = "draw serial at the end";
    } else if (memcmp(a.end.pe, b.end.pe, sizeof a.end.pe) != 0) {
        why = "PE registers at the end";
    }
    if (!why.empty()) {
        ++sFails;
        if (sFails <= 12) printf("FAIL %s seed %llu: %s\n", what, (unsigned long long)seed, why.c_str());
        return false;
    }
    return true;
}

std::vector<Op> randomOps(uint64_t seed, size_t n, bool withTokens, bool withAbandons, bool withBig) {
    Rng r(seed);
    std::vector<Op> ops;
    for (size_t i = 0; i < n; ++i) {
        uint8_t k;
        do {
            k = static_cast<uint8_t>(r.below(K_COUNT));
        } while ((!withTokens && k == K_TOKEN) || (!withAbandons && k == K_ABANDON) || (!withBig && k == K_BIGRAW) ||
                 (k == K_ABANDON && r.below(3)) || (k == K_BIGRAW && r.below(4)));
        ops.push_back(Op{k, r.next()});
    }
    return ops;
}
std::vector<size_t> randomCuts(uint64_t seed, size_t n) {
    Rng r(seed ^ 0xABCDEF);
    std::vector<size_t> cuts;
    size_t at = r.below(4);
    while (at <= n) {
        cuts.push_back(at);
        at += 1 + r.below(static_cast<uint32_t>(n / 2 + 1));
    }
    return cuts;
}

}  // namespace

int main(int argc, char** argv) {
    sMainThread = std::this_thread::get_id();
    const int seeds = argc > 1 ? atoi(argv[1]) : 60;
    const bool thread = getenv("GXCAP_THREAD") && atoi(getenv("GXCAP_THREAD"));
    const bool selftest = getenv("GXV_GXCAPTURE_SELFTEST") && atoi(getenv("GXV_GXCAPTURE_SELFTEST"));
    const bool neg = getenv("GXCAP_NEG") && atoi(getenv("GXCAP_NEG"));
    hx_quiet = getenv("GXCAP_LOG") ? 0 : 1;
    static u8 fifoMem[64];
    __PortGXFifoBatch = 1;  // (GXV_FIFO_BATCH=1, the console's default: the batched writers)
    hx_fifo_reset();
    port_gx_capture_init();
    GXInit(fifoMem, sizeof fifoMem);
    hx_fifo_reset();
    sInit.take();
    port_gx_capture_set_token_handler(tokenHandler, nullptr);
    const int mode = port_gx_capture_mode();
    printf("gx_capture test: mode %d%s%s, %d seeds\n", mode, selftest ? " self-test" : "", thread ? " replay-thread" : "", seeds);
    uint64_t captures = 0, checks = 0;

    if (neg) {
        // Negative control: the comparison sees a single changed argument.
        std::vector<Op> ops = randomOps(5, 200, true, false, false);
        Result a = runDirect(ops, false);
        ops[100].seed ^= 1;
        std::vector<size_t> cuts{0, ops.size()};
        Result b = runCaptured(ops, cuts, false, false, &captures);
        const bool differs = !same(a, b, "negative control (expected)", 5);
        printf(differs ? "negative control: the changed sequence is detected (PASS)\n" : "negative control: NOT detected (FAIL)\n");
        return differs ? 0 : 1;
    }

    if (getenv("GXCAP_TRAPTEST") && atoi(getenv("GXCAP_TRAPTEST"))) {
        // A replay on another thread beside this one, as the draw pipeline's mode 4 makes it (__PortGXCapTrap, __PortGXCap 3):
        //  - the replaying thread's own calls, GXSetDrawSync among them (GXV_GXCAPTURE_SYNC=1), make no guest-OS call: it is
        //    no guest thread (console, 11 Nov: the draw thread entered the guest OS from GXSetDrawSync's replay);
        //  - a GX call of this thread meanwhile goes to the layer and joins the replay first, also while the replaying thread
        //    is inside a call (the pass word is one for all threads: the hooks tell the threads apart by their stacks);
        //  - the same with the hooks armed by this thread before the other has begun (port_gx_replay_arm).
        // The control without the trap: the same replay makes the two interrupt calls of GXSetDrawSync on the other thread.
        hx_irq_main();
        int bad = 0;
        for (int round = 0; round < 3; ++round) {  // 0: no trap (control), 1: trap set by the replay, 2: armed before it
            hx_irq_foreign = 0;
            sTrapEnters = 0, sTrapReplayDone = 0, sTrapInToken = 0;
            if (!port_gx_capture_begin()) return printf("FAIL trap: no capture\n"), 1;
            for (int i = 0; i < 300; ++i) GXSetCullMode(static_cast<GXCullMode>(i & 3));
            GXSetDrawSync(0x1234);
            port_gx_capture_token(7);
            for (int i = 0; i < 300; ++i) GXSetCullMode(static_cast<GXCullMode>(i & 3));
            GXSetDrawSync(0x1235);
            PortGxCapture* cap = port_gx_capture_end();
            if (!cap) return printf("FAIL trap: the capture did not end (GXV_GXCAPTURE_SYNC=1 is needed)\n"), 1;
            const bool trap = round != 0;
            __PortGXCapTrap = trap ? 1 : 0;
            // the other thread: created first (its stack is then known), replaying when told
            std::atomic<int> go{0};
            std::atomic<uintptr_t> lo{0}, span{0};
            std::thread t([&] {
                pthread_attr_t attr;
                void* base = nullptr;
                size_t size = 0;
                pthread_getattr_np(pthread_self(), &attr);
                pthread_attr_getstack(&attr, &base, &size);
                pthread_attr_destroy(&attr);
                lo = reinterpret_cast<uintptr_t>(base);
                span = size;
                while (!go.load()) std::this_thread::yield();
                port_gx_replay(cap,
                               [](uint32_t, void* trapped) {
                                   GXSetCullMode(GX_CULL_BACK);  // (a pass's own GX call: straight into the library)
                                   sTrapInToken = 1;
                                   // (held inside the token until this thread's partner has tried its call)
                                   for (int k = 0; trapped && !sTrapEnters.load() && k < 2000000; ++k) std::this_thread::yield();
                               },
                               trap ? &go : nullptr);
                sTrapReplayDone = 1;
            });
            while (!span.load()) std::this_thread::yield();
            if (round == 2) {
                port_gx_replay_arm(lo, span);
                if (__PortGXCap != 3) ++bad, printf("FAIL trap: not armed\n");
            }
            go = 1;
            if (trap) {
                if (round == 1) while (!sTrapInToken.load()) std::this_thread::yield();  // (round 2: at once, armed or not begun)
                GXSetCullMode(GX_CULL_ALL);  // (this thread uses GX beside the replay)
                if (sTrapEnters.load() != 1) ++bad, printf("FAIL trap round %d: the call did not join the replay (%d)\n", round, sTrapEnters.load());
                if (!sTrapReplayDone.load()) ++bad, printf("FAIL trap round %d: the call ran before the replay's end\n", round);
            }
            t.join();
            if (__PortGXCap != 0 || __PortGXCapPass != 0) ++bad, printf("FAIL trap round %d: hooks left at %d / %d\n", round, __PortGXCap, __PortGXCapPass);
            if (trap && hx_irq_foreign != 0) ++bad, printf("FAIL trap round %d: %d guest-OS calls on the replaying thread\n", round, hx_irq_foreign);
            if (!trap && hx_irq_foreign != 4) ++bad, printf("FAIL trap control: %d guest-OS calls counted on the other thread, 4 expected\n", hx_irq_foreign);
            __PortGXCapTrap = 0;
            port_gx_replay_arm(0, 0);
        }
        printf("checks 3: a replay beside this thread (%d wrong)\n", bad);
        return bad ? 1 : 0;
    }
    if (getenv("GXCAP_OWNERTEST") && atoi(getenv("GXCAP_OWNERTEST"))) {
        // The cheap record path's thread test (A0: a compare of the stack address with a window of the recording thread's own
        // stack). It may say yes only for the recording thread: yes where the recording began and a few frames down, no from
        // another thread (with a small stack and a large one, wherever the allocator put them), no from deeper than the
        // window, and yes again on the way back. A "no" is not an error (the exact test decides then); a wrong "yes" would
        // record another thread's call.
        int bad = 0;
        if (port_gxcap_test_owner() != -1) ++bad, printf("FAIL owner: open before a capture\n");
        if (!port_gx_capture_begin()) return printf("FAIL owner: no capture\n"), 1;
        if (port_gxcap_test_owner() != 1) ++bad, printf("FAIL owner: the recording thread is not in its window\n");
        for (size_t stack : {size_t(16) << 10, size_t(64) << 10, size_t(1) << 20, size_t(8) << 20}) {
            for (int k = 0; k < 16; ++k) {
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setstacksize(&attr, stack);
                pthread_t t;
                int got = -2;
                if (pthread_create(&t, &attr, [](void* p) -> void* { *static_cast<int*>(p) = port_gxcap_test_owner(); return nullptr; }, &got) != 0)
                    return printf("FAIL owner: no thread\n"), 1;
                pthread_join(t, nullptr);
                pthread_attr_destroy(&attr);
                if (got != 0) ++bad, printf("FAIL owner: another thread (stack %u KiB) answered %d\n", static_cast<unsigned>(stack >> 10), got);
            }
        }
        struct Deep {
            __attribute__((noinline)) static int at(int depth, int want) {
                static volatile size_t frame = 16 << 10;  // (a size the compiler cannot see: the frame really is this deep)
                volatile char* const pad = static_cast<volatile char*>(__builtin_alloca(frame));
                pad[0] = pad[frame - 1] = static_cast<char>(depth);
                if (depth > 0) return at(depth - 1, want) + pad[0] * 0;
                return port_gxcap_test_owner() == want ? 0 : 1;
            }
        };
        if (Deep::at(4, 1)) ++bad, printf("FAIL owner: 64 KiB down is outside the window\n");
        if (Deep::at(40, 0)) ++bad, printf("FAIL owner: 640 KiB down is inside the window\n");
        if (port_gxcap_test_owner() != 1) ++bad, printf("FAIL owner: not in the window after coming back\n");
        GXSetCullMode(GX_CULL_BACK);
        PortGxCapture* cap = port_gx_capture_end();
        if (!cap) ++bad, printf("FAIL owner: the capture did not end\n");
        else port_gx_replay(cap, tokenHandler, nullptr);
        if (port_gxcap_test_owner() != -1) ++bad, printf("FAIL owner: open after the capture\n");
        // a recording begun on another thread has that thread's window, and this one is outside it
        std::thread([&bad] {
            if (!port_gx_capture_begin()) { ++bad; return; }
            if (port_gxcap_test_owner() != 1) ++bad, printf("FAIL owner: a second recording thread is not in its window\n");
            int got = -2;
            std::thread([&got] { got = port_gxcap_test_owner(); }).join();
            if (got != 0) ++bad, printf("FAIL owner: a third thread answered %d\n", got);
            if (PortGxCapture* c2 = port_gx_capture_end()) port_gx_replay(c2, tokenHandler, nullptr);
        }).join();
        printf("checks 70: the cheap path's thread test (%d wrong)\n", bad);
        return bad ? 1 : 0;
    }
    if (getenv("GXCAP_NULLTEST") && atoi(getenv("GXCAP_NULLTEST"))) {
        // The null recorder (GXV_GXCAPTURE_NULL): a capture in which every recorded call is counted and dropped. For sequences
        // without tokens and without calls that end a capture: nothing reaches the stream; what the game reads back while
        // "recording" is what it reads in a direct run; the calls counted are the calls a real recording records. With tokens,
        // abandons and oversized raw runs: it runs clean (the sanitizer builds), every capture ends or is abandoned.
        uint64_t clean = 0;
        for (int s = 0; s < seeds; ++s) {
            hx_fifo_small = s % 3 == 0;
            std::vector<Op> ops = randomOps(3000 + s, 300, false, false, s % 7 == 0);
            std::vector<size_t> whole{0, ops.size()};
            Result a = runDirect(ops, false);
            PortGxCaptureStats s0, s1, s2;
            port_gx_capture_stats(&s0);
            Result b = runCaptured(ops, whole, false, false, &captures);
            port_gx_capture_stats(&s1);
            port_gxcap_test_null(1);
            Result c = runCaptured(ops, whole, false, false, &captures);
            port_gxcap_test_null(0);
            port_gx_capture_stats(&s2);
            ++checks;
            if (!same(a, b, "null test: the real recording", s)) continue;
            if (s1.abandoned != s0.abandoned || s2.abandoned != s1.abandoned) continue;  // (a peek or an effect ended it: not this check's case)
            ++clean;
            if (!c.stream.empty()) {
                ++sFails;
                printf("FAIL null recorder seed %d: %u bytes reached the stream\n", s, static_cast<unsigned>(c.stream.size()));
            }
            if (c.log != a.log) {
                ++sFails;
                printf("FAIL null recorder seed %d: the game read back something else than in the direct run\n", s);
            }
            if (s2.calls - s1.calls != s1.calls - s0.calls) {
                ++sFails;
                printf("FAIL null recorder seed %d: %llu calls counted, the recording has %llu\n", s, (unsigned long long)(s2.calls - s1.calls),
                       (unsigned long long)(s1.calls - s0.calls));
            }
            if (s2.recordBytes != s1.recordBytes) {
                ++sFails;
                printf("FAIL null recorder seed %d: records were written\n", s);
            }
        }
        if (clean < static_cast<uint64_t>(seeds) / 4) {
            ++sFails;
            printf("FAIL null recorder: only %llu of %d sequences ran without an abandon\n", (unsigned long long)clean, seeds);
        }
        port_gxcap_test_null(1);
        for (int s = 0; s < seeds; ++s) {
            hx_fifo_small = s & 1;
            std::vector<Op> ops = randomOps(4000 + s, 300, true, true, s % 5 == 0);
            std::vector<size_t> cuts = randomCuts(s, ops.size());
            (void)runCaptured(ops, cuts, s & 1, thread, &captures);
            ++checks;
        }
        port_gxcap_test_null(0);
        printf("null recorder: %llu sequences without an abandon compared (stream empty, read-backs equal, calls counted)\n", (unsigned long long)clean);
    } else if (selftest) {
        for (int s = 0; s < seeds; ++s) {
            std::vector<Op> ops = randomOps(1000 + s, 400, true, true, s % 5 == 0);
            hx_fifo_small = s & 1;
            Result a = runDirect(ops, false);
            Result b = runSelftest(ops, 77 + s);
            same(a, b, "self-test", s);
            ++checks;
        }
    } else {
        // a, b, c: random sequences, random cuts, pointees overwritten (b) on odd seeds
        for (int s = 0; s < seeds; ++s) {
            hx_fifo_small = s % 3 == 0;
            hx_retained = s % 4 != 1;
            hx_bracket = s % 4 != 2;
            const bool mutate = s & 1;
            std::vector<Op> ops = randomOps(s, 300, true, true, s % 7 == 0);
            std::vector<size_t> cuts = randomCuts(s, ops.size());
            Result a = runDirect(ops, mutate);
            Result b = runCaptured(ops, cuts, mutate, thread, &captures);
            same(a, b, "random", s);
            ++checks;
            // one capture over the whole sequence
            std::vector<size_t> whole{0, ops.size()};
            Result c = runCaptured(ops, whole, mutate, thread, &captures);
            same(a, c, "whole", s);
            ++checks;
        }
        // scripted: every kind alone, with each other kind after it, in one capture
        for (int k = 0; k < K_COUNT; ++k) {
            for (int j = 0; j < K_COUNT; j += 3) {
                std::vector<Op> ops{Op{static_cast<uint8_t>(k), 11u + k}, Op{static_cast<uint8_t>(j), 29u + j}, Op{static_cast<uint8_t>(K_GETVP), 3},
                                    Op{static_cast<uint8_t>(k), 41u + k}, Op{static_cast<uint8_t>(K_SERIAL), 1}};
                std::vector<size_t> whole{0, ops.size()};
                Result a = runDirect(ops, true);
                Result b = runCaptured(ops, whole, true, thread, &captures);
                same(a, b, "scripted pair", k * 100 + j);
                ++checks;
            }
        }
        // d: an abandon at every position
        for (int s = 0; s < (seeds > 6 ? 6 : seeds); ++s) {
            std::vector<Op> base = randomOps(500 + s, 80, true, false, false);
            for (size_t at = 0; at <= base.size(); ++at) {
                std::vector<Op> ops = base;
                ops.insert(ops.begin() + static_cast<long>(at), Op{static_cast<uint8_t>(K_ABANDON), static_cast<uint32_t>(at * 31 + s)});
                std::vector<size_t> whole{0, ops.size()};
                Result a = runDirect(ops, true);
                Result b = runCaptured(ops, whole, true, thread, &captures);
                same(a, b, "abandon position", s * 1000 + at);
                ++checks;
            }
        }
    }
    PortGxCaptureStats st;
    port_gx_capture_stats(&st);
    printf("checks %llu, captures replayed %llu (begun %llu, refused %llu, ended %llu, abandoned %llu, verified %llu), calls %llu, raw %.1f KiB, "
           "records %.1f KiB, tokens %llu, getters %llu, verify differences %llu, foreign %llu, contract errors %llu\n",
           (unsigned long long)checks, (unsigned long long)st.replayed, (unsigned long long)st.begun, (unsigned long long)st.refused,
           (unsigned long long)st.ended, (unsigned long long)st.abandoned, (unsigned long long)st.verified, (unsigned long long)st.calls,
           st.rawBytes / 1024.0, st.recordBytes / 1024.0, (unsigned long long)st.tokens, (unsigned long long)st.getters,
           (unsigned long long)st.verifyDiffs, (unsigned long long)st.foreign, (unsigned long long)st.contractErrors);
    {
        // The cheap record path (A0) is what this run tested only if it took the calls: in a plain mode-1 run nearly all of them
        // (the rest: the first record of a chunk, the calls the general path keeps); none where it is switched off (verify,
        // GXV_GXCAPTURE_SLOW=1, the self-test with a call cap).
        const unsigned long long fast = port_gxcap_test_fast_calls();
        const bool slow = getenv("GXV_GXCAPTURE_SLOW") && atoi(getenv("GXV_GXCAPTURE_SLOW"));
        const bool cap = selftest && getenv("GXV_GXCAPTURE_SELFTEST_CALLS") && atoi(getenv("GXV_GXCAPTURE_SELFTEST_CALLS"));
        const bool off = mode != 1 || slow || cap;
        const bool mixed = getenv("GXCAP_NULLTEST") && atoi(getenv("GXCAP_NULLTEST"));  // (the null recorder on and off in turn)
        printf("cheap path: %llu of %llu calls%s\n", fast, (unsigned long long)st.calls, off ? " (off in this run)" : "");
        if (!mixed && (off ? fast != 0 : fast * 10 < st.calls * 7)) {
            printf("FAIL cheap path: %s\n", off ? "it recorded calls in a run that switches it off" : "it took under 7 calls in 10");
            return 1;
        }
    }
    printf("abandons by entry point:");
    for (int i = 0; i < PORT_GXCAP_ID_END; ++i)
        if (port_gxcap_abandons_by(i)) printf(" %s %llu", port_gxcap_name(i), (unsigned long long)port_gxcap_abandons_by(i));
    printf("\n");
    if (mode == 2 && st.verifyDiffs) {
        printf("FAIL verify: %llu differences, first: %s\n", (unsigned long long)st.verifyDiffs, port_gxcap_first_diff());
        ++sFails;
    }
    if (mode == 2 && !st.verified) {
        printf("FAIL verify: nothing compared\n");
        ++sFails;
    }
    if (st.contractErrors) {
        printf("FAIL contract errors %llu\n", (unsigned long long)st.contractErrors);
        ++sFails;
    }
    if (mode >= 1 && !st.begun) {
        printf("FAIL no capture began\n");
        ++sFails;
    }
    printf(sFails ? "FAIL (%d)\n" : "PASS\n", sFails);
    return sFails ? 1 : 0;
}
