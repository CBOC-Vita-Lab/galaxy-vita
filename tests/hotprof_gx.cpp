// hotprof25 round 2 (11 Nov): host proof of GXV_HOTPROF_TEVCOL and GXV_HOTPROF_REINITGX (decomp/src/RVL_SDK/gx/GXTev.c, GXBump.c).
// The console's GX library (the production units, TARGET_PC) with the FIFO that stands for the stream (tests/gx_capture_stubs.c), and a
// model of the front end's register file that reads the stream in order: BP registers with the mask, the TEV colour / konst halves at BP
// 0xE0..0xE7 as vgx_state.cpp WriteTevColor keeps them, CP registers, XF memory, display lists called and read through, and at every
// quad drawn a copy of all of it. A game-like sequence (J3D-style raw BP writes into the FIFO, display lists that write TEV registers,
// every TEV setter with random arguments, J3DSys::reinitTevStages / reinitIndStages as J3DSys.cpp has them, and JParticle's particle loops:
// GXSetTevColor twice, GXLoadPosMtxImm, sometimes GXLoadTexMtxImm, GXCallDisplayList of a quad list, with and without GX_MT_XF_FLUSH) is
// run from the same GX state with the switches off and on:
//   TEVCOL 1    every quad is drawn with the same registers as with the switch off, the registers and the library's state at the end are the
//               same, and the stream is shorter (BG repeats and, in the loops, equal words were left out);
//   TEVCOL 2    the stream without its markers is byte for byte the stream of the switch off; every announced word is what the model's
//               register holds (checked > 0, differing 0);
//   REINITGX 1  the same quads, registers and library state as with the calls; the stream is shorter;
//   REINITGX 2  the stream is the one of the switch off; no mismatch is logged;
//   GX capture  (GXV_GXCAPTURE=1 or 2 in the environment) the same sequence recorded in captures and replayed: the same quads and
//               registers again, and in mode 2 the layer reports no difference between its replay and the direct bytes;
//   negative    a raw TEV colour write INSIDE a particle loop (what the scope's contract excludes): TEVCOL 1 must then draw a quad with
//               other registers in some seed, i.e. this test sees a memory that went stale.
//   tools/test_hotprof_gx.sh
#define PORT_GXCAP_HOOKS 1
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "revolution/types.h"
#include "revolution/gx.h"
#include "revolution/gx/GXRegs.h"
#include "port/gx_capture.h"
#include "port/gx_fifo.h"

extern "C" {
void hx_fifo_reset(void);
const uint8_t* hx_stream(size_t* n);
uint8_t* hx_arena(void);
extern int hx_fifo_small, hx_retained, hx_bracket, hx_peek_const, hx_quiet;
extern volatile u32 __PortGXPERegs[0x400];
extern u32 __PortGXDrawSerial;
void* port_phys_to_host(u32 a);
const char* port_gxcap_first_diff(void);
void port_gx_tevcol_scope(int on);
int port_gx_reinit_tev(void);
int port_gx_reinit_tevdirect(void);
void port_gx_reinit_tev_verify(int end);
}
static int gTev = 0, gReinit = 0;
static int gLogMismatch = 0;
extern "C" int port_hotprof_tevcol_mode(void) { return gTev; }
extern "C" int port_hotprof_reinitgx_mode(void) { return gReinit; }
extern "C" void port_drawpipe_game_enter(void) {}
extern "C" int port_on_frame_thread(void) { return 1; }
extern "C" void port_log_summary(const char* fmt, ...) {
    if (strstr(fmt, "MISMATCH")) ++gLogMismatch;
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

// ---- the GX state a run starts from, and the library's state compared at its end (as tests/gx_capture.cpp)
struct State {
    GXData gxd;
    uint8_t light[PORT_GXCAP_LIGHT_STATE_BYTES];
    u32 serial;
    void take() {
        memcpy(&gxd, __GXData, sizeof gxd);
        __PortGXLightStateSave(light);
        serial = __PortGXDrawSerial;
    }
    void put() const {
        memcpy(__GXData, &gxd, sizeof gxd);
        __PortGXLightStateLoad(light);
        __PortGXDrawSerial = serial;
    }
};
State sInit;

// ---- the model of the front end's registers
struct Fe {
    uint32_t bp[256];
    uint32_t bpMask;
    int tevReg[4][4], tevKonst[4][4];
    uint32_t tevRaw[16];
    uint32_t cp[256];
    uint32_t xf[0x1100];
    uint32_t idx[4];
};
const uint8_t* resolveList(uint32_t addr);
struct Parse {
    Fe fe;
    std::vector<Fe> quads;          // the registers at every quad drawn
    std::vector<uint8_t> plain;     // the stream without the verify markers
    uint64_t checked = 0, differing = 0, strips = 0, bpWrites = 0, errors = 0;
    bool haveHi = false;
    uint32_t hi = 0;
    Parse() { memset(&fe, 0, sizeof fe); fe.bpMask = 0x00FFFFFFu; }
    static int sext11(uint32_t v) { return (v & 0x400) ? static_cast<int>(v) - 0x800 : static_cast<int>(v); }
    void writeBp(uint32_t rv) {
        const uint32_t reg = rv >> 24, val = rv & 0x00FFFFFFu;
        ++bpWrites;
        if (reg == 0xFE) { fe.bpMask = val; return; }
        if (reg == 0xFF) {
            const uint32_t kind = val & 0xFFu, payload = (val >> 8) & 0xFFFFu;
            if (kind == 38) { hi = payload; haveHi = true; }
            else if (kind == 39 && haveHi) {
                haveHi = false;
                const uint32_t r = (hi >> 8) & 0xFFu, v = ((hi & 0xFFu) << 16) | payload;
                ++checked;
                const bool ok = r >= 0xE0 && r <= 0xE7 && fe.bpMask == 0x00FFFFFFu && fe.bp[r] == v && fe.tevRaw[((r - 0xE0) << 1) | ((v >> 23) & 1u)] == v;
                if (!ok) ++differing;
            }
            return;
        }
        const uint32_t nv = (fe.bp[reg] & ~fe.bpMask) | (val & fe.bpMask);
        fe.bpMask = 0x00FFFFFFu;
        if (reg >= 0xE0 && reg <= 0xE7) {
            // (vgx_state.cpp WriteTevColor: bit 23 names the konst colours; RA or BG half)
            const int i = (reg - 0xE0) >> 1;
            int* d = ((nv & 0x800000u) ? fe.tevKonst : fe.tevReg)[i];
            const int lo = sext11(nv & 0x7FF), hi2 = sext11((nv >> 12) & 0x7FF);
            if (reg & 1) { d[2] = lo; d[1] = hi2; }
            else { d[0] = lo; d[3] = hi2; }
            fe.tevRaw[((reg - 0xE0) << 1) | (nv >> 23)] = nv;
        }
        fe.bp[reg] = nv;
    }
    static uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
    static uint32_t be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }
    void run(const uint8_t* p, size_t n, bool top) {
        const uint8_t* const end = p + n;
        while (p < end) {
            const uint8_t* const at = p;
            const uint8_t cmd = *p++;
            bool marker = false;
            if (cmd == 0x00) {
            } else if (cmd == 0x61) {
                if (end - p < 4) { ++errors; return; }
                const uint32_t rv = be32(p);
                p += 4;
                marker = (rv >> 24) == 0xFF && ((rv & 0xFF) == 38 || (rv & 0xFF) == 39);
                writeBp(rv);
            } else if (cmd == 0x08) {
                if (end - p < 5) { ++errors; return; }
                fe.cp[p[0]] = be32(p + 1);
                p += 5;
            } else if (cmd == 0x10) {
                if (end - p < 4) { ++errors; return; }
                const uint32_t cnt = (be16(p) & 0xF) + 1, addr = be16(p + 2);
                p += 4;
                if (static_cast<size_t>(end - p) < cnt * 4) { ++errors; return; }
                for (uint32_t i = 0; i < cnt; ++i, p += 4)
                    if (addr + i < 0x1100) fe.xf[addr + i] = be32(p);
            } else if (cmd == 0x20 || cmd == 0x28 || cmd == 0x30 || cmd == 0x38) {
                if (end - p < 4) { ++errors; return; }
                fe.idx[(cmd - 0x20) >> 3] = be32(p);
                p += 4;
            } else if (cmd == 0x40) {
                if (end - p < 8) { ++errors; return; }
                const uint32_t addr = be32(p), size = be32(p + 4);
                p += 8;
                const uint8_t* list = resolveList(addr);  // (the call carries the list's address: its low 32 bits on a 64-bit host)
                if (!list) { ++errors; return; }
                run(list, size, false);
            } else if (cmd >= 0x80 && cmd < 0xC0) {
                // (this test draws with one vertex format: an 8-bit position index and an 8-bit texture coordinate index)
                if (end - p < 2) { ++errors; return; }
                const uint32_t count = be16(p);
                p += 2;
                if (static_cast<size_t>(end - p) < count * 2u) { ++errors; return; }
                bool zero = true;
                for (uint32_t i = 0; i < count * 2u; ++i) zero = zero && p[i] == 0;
                if ((cmd & 0xF8) == 0x98 && zero) ++strips;  // (the library's flush strip: the front end drops it)
                else quads.push_back(fe);
                p += count * 2u;
            } else {
                ++errors;
                return;
            }
            if (top && !marker) plain.insert(plain.end(), at, p);
        }
    }
};

// ---- the game-like sequence
uint8_t* gQuad;      // a quad list as JParticle's jpa_dl
uint8_t* gLists[6];  // display lists that write TEV registers (and may draw)
uint32_t gListLen[6];
void makeLists() {
    uint8_t* a = hx_arena();
    gQuad = a + 0x1000;
    static const uint8_t quad[32] = {0x80, 0x00, 0x04, 0x00, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03};
    memcpy(gQuad, quad, 32);
    Rng r(777);
    for (int i = 0; i < 6; ++i) {
        uint8_t* l = a + 0x2000 + i * 0x100;
        uint32_t n = 0;
        const int writes = 1 + static_cast<int>(r.below(6));
        for (int k = 0; k < writes; ++k) {
            // (a J3D material list's TEV colour / konst registers, or a TEV stage register)
            const uint32_t reg = r.below(3) ? 0xE0 + r.below(8) : 0xC0 + r.below(32);
            const uint32_t val = r.next() & 0x00FFFFFFu;
            l[n++] = 0x61; l[n++] = static_cast<uint8_t>(reg); l[n++] = static_cast<uint8_t>(val >> 16); l[n++] = static_cast<uint8_t>(val >> 8); l[n++] = static_cast<uint8_t>(val);
        }
        if (i & 1) { memcpy(l + n, quad, 11); n += 11; }
        while (n & 31) l[n++] = 0;
        gLists[i] = l;
        gListLen[i] = n;
    }
}
const uint8_t* resolveList(uint32_t addr) {
    if (addr == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(gQuad))) return gQuad;
    for (int i = 0; i < 6; ++i)
        if (addr == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(gLists[i]))) return gLists[i];
    return nullptr;
}
GXColor pal(Rng& r, int few) {
    // (few distinct colours, so that equal words follow each other as they do in an emitter's particles)
    static const GXColor k[6] = {{255, 255, 255, 255}, {255, 255, 255, 128}, {200, 100, 50, 255}, {200, 100, 50, 64}, {0, 0, 0, 0}, {10, 20, 30, 40}};
    if (few) return k[r.below(static_cast<uint32_t>(few))];
    return GXColor{static_cast<u8>(r.next()), static_cast<u8>(r.next()), static_cast<u8>(r.next()), static_cast<u8>(r.next())};
}
// (J3DSys::reinitTevStages and reinitIndStages, decomp/src/JSystem/J3DGraphBase/J3DSys.cpp, with the port's lines as they are there)
void reinitTevStages() {
    static const GXColor white = {255, 255, 255, 255};
    port_gx_reinit_tev_verify(0);
    const bool onePass = port_gx_reinit_tev() != 0;
    if (!onePass)
        for (int s = 0; s < 16; ++s) GXSetTevOrder(static_cast<GXTevStageID>(s), GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXSetTevColor(GX_TEVREG0, white);
    GXSetTevColor(GX_TEVREG1, white);
    GXSetTevColor(GX_TEVREG2, white);
    GXSetTevKColor(GX_KCOLOR0, white);
    GXSetTevKColor(GX_KCOLOR1, white);
    GXSetTevKColor(GX_KCOLOR2, white);
    GXSetTevKColor(GX_KCOLOR3, white);
    u32 numStages = onePass ? 0 : GX_MAX_TEVSTAGE;
    for (u32 i = 0; i < numStages; i++) {
        GXSetTevColorIn((GXTevStageID)i, GX_CC_RASC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
        GXSetTevColorOp((GXTevStageID)i, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevAlphaIn((GXTevStageID)i, GX_CA_RASA, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
        GXSetTevAlphaOp((GXTevStageID)i, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevKColorSel((GXTevStageID)i, GX_TEV_KCSEL_1_4);
        GXSetTevKAlphaSel((GXTevStageID)i, GX_TEV_KASEL_1);
        GXSetTevSwapMode((GXTevStageID)i, GX_TEV_SWAP0, GX_TEV_SWAP0);
    }
    port_gx_reinit_tev_verify(1);
    GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    if (!port_gx_reinit_tevdirect())
        for (u32 i = 0; i < GX_MAX_TEVSTAGE; i++) GXSetTevDirect((GXTevStageID)i);
}
void rawBp(uint32_t rv) {  // (J3DFifoLoadBPCmd: straight into the FIFO, behind the library's back)
    GXCmd1u8(0x61);
    GXCmd1u32(rv);
}
void outside(Rng& r) {
    switch (r.below(12)) {
    case 0: rawBp(((0xE0u + r.below(8)) << 24) | (r.next() & 0x00FFFFFFu)); break;
    case 1: GXSetTevColor(static_cast<GXTevRegID>(r.below(4)), pal(r, r.coin() ? 6 : 0)); break;
    case 2: GXSetTevColorS10(static_cast<GXTevRegID>(r.below(4)), GXColorS10{static_cast<s16>(r.below(2048)) - 1024, static_cast<s16>(r.below(2048)) - 1024, static_cast<s16>(r.below(2048)) - 1024, static_cast<s16>(r.below(2048)) - 1024}); break;
    case 3: GXSetTevKColor(static_cast<GXTevKColorID>(r.below(4)), pal(r, 0)); break;
    case 4: { const int i = static_cast<int>(r.below(6)); GXCallDisplayList(gLists[i], gListLen[i]); break; }
    case 5: reinitTevStages(); break;
    case 6: GXSetTevOrder(static_cast<GXTevStageID>(r.below(16)), static_cast<GXTexCoordID>(r.below(8)), static_cast<GXTexMapID>(r.below(8)), r.coin() ? GX_COLOR0A0 : GX_COLOR_NULL); break;
    case 7: GXSetTevColorIn(static_cast<GXTevStageID>(r.below(16)), static_cast<GXTevColorArg>(r.below(16)), static_cast<GXTevColorArg>(r.below(16)), static_cast<GXTevColorArg>(r.below(16)), static_cast<GXTevColorArg>(r.below(16))); break;
    case 8: GXSetTevAlphaOp(static_cast<GXTevStageID>(r.below(16)), static_cast<GXTevOp>(r.below(2)), static_cast<GXTevBias>(r.below(3)), static_cast<GXTevScale>(r.below(4)), r.coin(), static_cast<GXTevRegID>(r.below(4))); break;
    case 9: GXSetTevSwapMode(static_cast<GXTevStageID>(r.below(16)), static_cast<GXTevSwapSel>(r.below(4)), static_cast<GXTevSwapSel>(r.below(4))); break;
    case 10: GXSetTevKColorSel(static_cast<GXTevStageID>(r.below(16)), static_cast<GXTevKColorSel>(r.below(32))); GXSetTevKAlphaSel(static_cast<GXTevStageID>(r.below(16)), static_cast<GXTevKAlphaSel>(r.below(32))); break;
    default: rawBp(((0xC0u + r.below(32)) << 24) | (r.next() & 0x00FFFFFFu)); break;
    }
}
void particleLoop(Rng& r, bool foreignInside) {
    const bool flush = r.coin(), env = r.below(3) != 0, prj = r.below(4) == 0;
    const int few = 1 + static_cast<int>(r.below(4)), n = static_cast<int>(r.below(12));
    GXSetMisc(GX_MT_XF_FLUSH, flush ? 8 : 0);
    port_gx_tevcol_scope(1);
    for (int i = 0; i < n; ++i) {
        GXSetTevColor(GX_TEVREG0, pal(r, few));
        if (env) GXSetTevColor(GX_TEVREG1, pal(r, 1 + static_cast<int>(r.below(2))));
        if (foreignInside && r.below(3) == 0) rawBp(((0xE0u + r.below(4)) << 24) | (r.next() & 0x007FFFFFu));  // (negative control only)
        f32 m[3][4];
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 4; ++b) m[a][b] = r.f();
        if (r.below(8)) {  // (a quad the game leaves out sends colours without a draw: sometimes nothing follows the colours)
            GXLoadPosMtxImm(m, GX_PNMTX0);
            if (prj) GXLoadTexMtxImm(m, GX_TEXMTX0, GX_MTX3x4);
            GXCallDisplayList(gQuad, 32);
        }
    }
    port_gx_tevcol_scope(0);
    GXSetMisc(GX_MT_XF_FLUSH, 0);
}
static void tokenNone(uint32_t, void*) {}
// One run of the sequence of `seed` from the initial state. capture: cut into GX captures, each replayed at once.
void runSeq(uint64_t seed, bool capture, bool foreignInside, std::vector<uint8_t>& stream, State& endState) {
    sInit.put();
    hx_fifo_reset();
    hx_fifo_small = seed % 3 == 0;
    Rng r(seed);
    static const u8 pos[64] = {0}, crd[64] = {0};
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
    GXSetVtxDesc(GX_VA_TEX0, GX_INDEX8);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_S8, 0);
    GXSetArray(GX_VA_POS, hx_arena() + 0x800, 3);
    GXSetArray(GX_VA_TEX0, hx_arena() + 0x900, 2);
    (void)pos;
    (void)crd;
    const int steps = 40 + static_cast<int>(r.below(60));
    Rng rc(seed * 31 + 7);  // (where captures begin and end: a sequence of its own, so that the game-like sequence is the same)
    bool open = false;
    for (int s = 0; s < steps; ++s) {
        if (capture && !open && rc.below(3) == 0) open = port_gx_capture_begin();
        if (r.below(3) == 0) particleLoop(r, foreignInside);
        else outside(r);
        if (open && rc.below(4) == 0) {
            PortGxCapture* c = port_gx_capture_end();
            open = false;
            if (c) port_gx_replay(c, tokenNone, nullptr);
        }
    }
    if (open) {
        PortGxCapture* c = port_gx_capture_end();
        if (c) port_gx_replay(c, tokenNone, nullptr);
    }
    GXFlush();
    size_t n = 0;
    const uint8_t* p = hx_stream(&n);
    stream.assign(p, p + n);
    endState.take();
}
bool sameQuads(const Parse& a, const Parse& b) {
    if (a.quads.size() != b.quads.size()) return false;
    for (size_t i = 0; i < a.quads.size(); ++i)
        if (memcmp(&a.quads[i], &b.quads[i], sizeof(Fe)) != 0) return false;
    return memcmp(&a.fe, &b.fe, sizeof(Fe)) == 0;
}
}  // namespace

int main(int argc, char** argv) {
    const int seeds = argc > 1 ? atoi(argv[1]) : 200;
    hx_quiet = getenv("HOTPROF_LOG") ? 0 : 1;
    static u8 fifoMem[64];
    __PortGXFifoBatch = 1;
    hx_fifo_reset();
    port_gx_capture_init();
    GXInit(fifoMem, sizeof fifoMem);
    hx_fifo_reset();
    sInit.take();
    makeLists();
    const int capMode = port_gx_capture_mode();
    int bad = 0;
    unsigned long long quads = 0, bytes0 = 0, bytesTev = 0, bytesRe = 0, checked = 0, strips = 0, negSeen = 0, bp0 = 0, bpTev = 0, bpRe = 0;
    for (int s = 1; s <= seeds; ++s) {
        std::vector<uint8_t> s00, sx;
        State e00, ex;
        gTev = 0; gReinit = 0;
        runSeq(static_cast<uint64_t>(s), false, false, s00, e00);
        Parse p00;
        p00.run(s00.data(), s00.size(), true);
        if (p00.errors) { printf("FAIL seed %d: the model could not read the stream of the switches off\n", s); ++bad; continue; }
        quads += p00.quads.size();
        bytes0 += s00.size();
        strips += p00.strips;
        bp0 += p00.bpWrites;
        struct Case { int tev, reinit; bool cap; const char* name; };
        const Case cases[] = {{1, 0, false, "TEVCOL=1"}, {2, 0, false, "TEVCOL=2"}, {0, 1, false, "REINITGX=1"}, {0, 2, false, "REINITGX=2"}, {1, 1, false, "TEVCOL=1 REINITGX=1"},
                              {0, 0, true, "captures"}, {1, 1, true, "TEVCOL=1 REINITGX=1 in captures"}, {2, 2, true, "TEVCOL=2 REINITGX=2 in captures"}};
        for (const Case& c : cases) {
            if (c.cap && capMode == 0) continue;
            if (!c.cap && capMode != 0 && false) continue;
            gTev = c.tev; gReinit = c.reinit;
            gLogMismatch = 0;
            runSeq(static_cast<uint64_t>(s), c.cap, false, sx, ex);
            Parse px;
            px.run(sx.data(), sx.size(), true);
            const char* why = nullptr;
            if (px.errors) why = "the model could not read the stream";
            else if (!sameQuads(p00, px)) why = "a quad is drawn with other registers, or the registers at the end differ";
            else if (memcmp(&e00.gxd, &ex.gxd, sizeof(GXData)) != 0) why = "the GX library's state at the end differs";
            else if (px.differing) why = "an announced word is not what the register holds";
            else if (gLogMismatch) why = "a MISMATCH was logged";
            else if (!c.cap && (c.tev == 2 || c.tev == 0) && c.reinit != 1 && px.plain != s00) why = "the stream (without markers) is not the stream of the switches off";
            else if (c.cap && capMode == 2 && port_gxcap_first_diff() && *port_gxcap_first_diff()) why = "the capture layer's verify reports a difference";
            if (why) {
                if (bad < 12) printf("FAIL seed %d %s: %s (%u quads against %u, %u bytes against %u)\n", s, c.name, why, static_cast<unsigned>(px.quads.size()),
                                     static_cast<unsigned>(p00.quads.size()), static_cast<unsigned>(sx.size()), static_cast<unsigned>(s00.size()));
                ++bad;
            }
            if (!c.cap && c.tev == 1 && c.reinit == 0) { bytesTev += sx.size(); bpTev += px.bpWrites; }
            if (!c.cap && c.tev == 0 && c.reinit == 1) { bytesRe += sx.size(); bpRe += px.bpWrites; }
            if (!c.cap && c.tev == 2) checked += px.checked;
        }
        // (negative control: a raw TEV colour write inside the loops; with the switch off this is just another sequence)
        if (capMode == 0) {
            gTev = 0; gReinit = 0;
            runSeq(static_cast<uint64_t>(s), false, true, s00, e00);
            Parse n0;
            n0.run(s00.data(), s00.size(), true);
            gTev = 1;
            runSeq(static_cast<uint64_t>(s), false, true, sx, ex);
            Parse n1;
            n1.run(sx.data(), sx.size(), true);
            if (!sameQuads(n0, n1)) ++negSeen;
        }
    }
    gTev = 0; gReinit = 0;
    if (capMode == 0) {
        printf("%d sequences, %llu quads drawn, %llu flush strips; stream bytes: switches off %llu, TEVCOL=1 %llu, REINITGX=1 %llu; BP commands: %llu, %llu, %llu\n", seeds, quads,
               strips, bytes0, bytesTev, bytesRe, bp0, bpTev, bpRe);
        printf("TEVCOL=2: %llu announced words checked against the model's registers\n", checked);
        if (!(bytesTev < bytes0) || !(bytesRe < bytes0)) { printf("FAIL: a switch did not shorten the stream\n"); ++bad; }
        if (checked == 0) { printf("FAIL: TEVCOL=2 announced nothing\n"); ++bad; }
        printf("negative control (a raw TEV colour write inside a particle loop): %llu of %d sequences draw a quad with other registers under TEVCOL=1\n", negSeen, seeds);
        if (negSeen == 0) { printf("FAIL: the negative control was not seen\n"); ++bad; }
    } else {
        printf("%d sequences in GX captures (GXV_GXCAPTURE=%d), %llu quads drawn\n", seeds, capMode, quads);
    }
    printf(bad ? "hotprof_gx: FAIL (%d)\n" : "hotprof_gx: PASS\n", bad);
    return bad ? 1 : 0;
}
