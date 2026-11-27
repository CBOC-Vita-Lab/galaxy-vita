// GXV_GX_DEDUP end to end (tools/test_gx_dedup.sh): the game half. Random pictures drawn through the production GX units (GXTev.c, GXPixel.c,
// GXAttr.c, GXGeometry.c with its real dirty-state flush, GXLight.c, GXTransform.c, GXDisplayList.c, GXMisc.c) and the production FIFO
// (platform/src/gx/gx_fifo.cpp, included below, with the gate of platform/src/gx/gx_dedup.inc), plus J3D's direct writers as J3D makes them
// (__PortGXWriteCP for the array bases and matrix indices, __PortGXWriteXF2 for J3DShapeMtxIdx::load's XF half) and the list calls the
// game's callers vouch for (__PortGXDedupKeep: a material-like list of BP / XF writes, a geometry list of primitives) or not (a list with
// CP writes and the matrix index in it), a display list recorded with GXBeginDisplayList and called (the gate is not live while it is
// recorded), immediate draws, a masked gen-mode write (GXSetCoPlanar), markers' and pictures' invalidations. Values come from small sets,
// so most writes repeat. The stream goes to the real front end (tests/gx_dedup_fe.cpp), which prints a hash of every picture's command
// buffer and of the stream bytes it was handed.
//   gx_dedup_e2e [frames] [seed]    GXV_GX_DEDUP=0/1/2, GXV_GX_DEDUP_CENSUS=<n> as in the game
// Pass (the script): the picture lines of =1 and =2 and of the census are those of =0; the census's stream lines are those of =0 (it
// changes no byte); =2's front end counts no probe differing; =1's stream is shorter. GXDD_E2E_MUTANT=1 / 2 (a caller vouching wrongly
// for a list: the one with CP writes and the matrix index, a material-like one, as primitives only): =1's pictures or =2's verify must
// show it.
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
// (gx_fifo.cpp first: it owns the FIFO's globals, which port/gx_fifo.h would otherwise define weak in this unit as well)
#include "../platform/src/gx/gx_fifo.cpp"
#include "../platform/vgx/src/vgx_common.h"
#include "../platform/src/gx/gpu.h"
#include "../platform/src/gx/vgx_galaxy.h"
#include "port/gx_fifo.h"
#include "port/gx_dedup.h"
#include "port/heap_routing.h"
#include "port/port.h"
#include "revolution/gx.h"
#include "revolution/mtx.h"
#include "revolution/gx/GXRegs.h"

// ---- what gx_fifo.cpp needs (no player thread: every flush is parsed at once)
extern "C" int port_vgx_player() { return 0; }
extern "C" int port_vgx_seg_idle() { return 0; }
extern "C" int port_vgx_active() { return 1; }
extern "C" uint8_t* port_vgx_publish(uint8_t* p, int) { return p; }
extern "C" void port_vgx_stream_setup(uint8_t*, uint8_t*, size_t) {}
extern "C" void port_perf_record(int64_t) {}
// (the front end's arenas take their blocks from here and give them back with operator delete, as the game's operator new / delete pair
// does: blocks of up to the default alignment come from operator new; the FIFO's staging buffers, never freed, from posix_memalign)
extern "C" void* port_host_alloc(size_t n, int alignment) {
    if (alignment <= static_cast<int>(__STDCPP_DEFAULT_NEW_ALIGNMENT__)) return ::operator new(n);
    void* p = nullptr;
    if (posix_memalign(&p, static_cast<size_t>(alignment), n) != 0) std::abort();
    return p;
}
extern "C" void port_fatal(const char*, ...) { std::abort(); }
extern "C" void port_host_alloc_scope_enter() {}
extern "C" void port_host_alloc_scope_leave() {}
extern "C" void port_irq_set_handler(int, void (*)()) {}
extern "C" void port_call_sdk_interrupt(int) {}

extern "C" {
uint8_t* fe_ram();
uint32_t fe_alloc(uint32_t bytes);
void fe_written(uint32_t addr, uint32_t bytes);
void fe_init();
void fe_frame_end(unsigned frame);
void fe_counters(unsigned long long out[3]);
static GXData gxData;
GXData* const __GXData = &gxData;
u32 port_host_to_phys(const void* p) { // (guest addresses as pointers, as the fixture's lists are; a host pointer into the fixture's RAM too)
    const uintptr_t a = reinterpret_cast<uintptr_t>(p), r = reinterpret_cast<uintptr_t>(fe_ram());
    if (a >= r && a < r + (1u << 20)) return static_cast<u32>(a - r);
    return static_cast<u32>(a);
}
// (the OS calls the units make; nothing of them matters here)
BOOL OSDisableInterrupts(void) { return 0; }
BOOL OSRestoreInterrupts(BOOL) { return 0; }
void OSPanic(const char*, int, const char*, ...) { std::abort(); }
void OSReport(const char*, ...) {}
}

namespace {
uint64_t g_rng = 0x2545F4914F6CDD1Dull;
uint32_t Rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return static_cast<uint32_t>(g_rng >> 11);
}
uint32_t Rn(uint32_t n) { return Rnd() % n; }
int g_mutant = 0; // GXDD_E2E_MUTANT (the script's negative controls: a caller that vouches for a list wrongly)
void* Guest(uint32_t a) { return reinterpret_cast<void*>(static_cast<uintptr_t>(a)); }
void Put32(uint8_t* p, uint32_t v) { p[0] = static_cast<uint8_t>(v >> 24); p[1] = static_cast<uint8_t>(v >> 16); p[2] = static_cast<uint8_t>(v >> 8); p[3] = static_cast<uint8_t>(v); }

uint32_t g_pos, g_crd, g_geo, g_mat[2], g_odd, g_rec;
uint32_t g_matSize[2], g_oddSize;
// A list as the game's callers know theirs: bytes into the fixture's RAM
uint32_t MakeList(const std::vector<uint8_t>& b, uint32_t room) {
    const uint32_t a = fe_alloc(room);
    std::memset(fe_ram() + a, 0, room);
    std::memcpy(fe_ram() + a, b.data(), b.size());
    fe_written(a, room);
    return a;
}
void LBp(std::vector<uint8_t>& b, uint32_t rv) { b.push_back(0x61); uint8_t w[4]; Put32(w, rv); b.insert(b.end(), w, w + 4); }
void LCp(std::vector<uint8_t>& b, uint32_t reg, uint32_t v) { b.push_back(0x08); b.push_back(static_cast<uint8_t>(reg)); uint8_t w[4]; Put32(w, v); b.insert(b.end(), w, w + 4); }
void LXf(std::vector<uint8_t>& b, uint32_t addr, uint32_t v) {
    b.push_back(0x10);
    uint8_t w[4];
    Put32(w, addr);
    b.insert(b.end(), w, w + 4);
    Put32(w, v);
    b.insert(b.end(), w, w + 4);
}

void Build() {
    uint8_t* ram = fe_ram();
    g_pos = fe_alloc(8 * 3);
    { const int8_t q[8][3] = {{-25, 25, 0}, {25, 25, 0}, {25, -25, 0}, {-25, -25, 0}, {0, 50, 0}, {50, 0, 0}, {0, -50, 0}, {-50, 0, 0}};
      std::memcpy(ram + g_pos, q, sizeof q); }
    g_crd = fe_alloc(8 * 2);
    { const uint8_t t[16] = {0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 2, 0, 2, 1, 0, 1}; std::memcpy(ram + g_crd, t, sizeof t); }
    fe_written(0x1000, 0x100);
    // the geometry list: primitives only (a quad, then a second one), as J3D's shape lists and JParticle's quad lists are
    g_geo = MakeList({0x80, 0, 4, 0, 0, 1, 1, 2, 2, 3, 3, 0x80, 0, 4, 4, 0, 5, 1, 6, 2, 7, 3}, 64);
    // material-like lists: BP and XF writes, a mask used inside, no CP (what J3D's GD writers make)
    for (int k = 0; k < 2; ++k) {
        std::vector<uint8_t> b;
        LBp(b, 0xE0000000u | (k ? 0x0FF0FFu : 0x000080u));
        LBp(b, 0xE1000000u | 0x0000FFu);
        LBp(b, 0xE1000000u | 0x0000FFu);
        LBp(b, 0xC0000000u | (0xFu << 12) | (k ? 0x8Fu : 0x28Fu));
        LBp(b, 0xF3000000u | 0x3F0000u | (k ? 0x10u : 0u));
        LBp(b, 0xFE00FFE3u);
        LBp(b, 0x41000000u | (k ? 0x4A1u : 0x0A1u));
        LXf(b, 0x100C, k ? 0xFF8040FFu : 0x804020FFu);
        LXf(b, 0x1040, k ? 0x5280u : 0x5200u);
        g_matSize[k] = static_cast<uint32_t>((b.size() + 31) & ~size_t(31));
        g_mat[k] = MakeList(b, g_matSize[k]);
    }
    // a list nobody vouches for: CP writes, the matrix index (the front end's matrix-index cell), a TEV colour, then a quad
    {
        std::vector<uint8_t> b;
        LCp(b, 0xA5, 0x2000);
        LCp(b, 0x30, 3);
        LXf(b, 0x1018, 0);
        LBp(b, 0xE2000000u | 0x123456u);
        const uint8_t quad[] = {0x80, 0, 4, 0, 0, 1, 1, 2, 2, 3, 3};
        b.insert(b.end(), quad, quad + sizeof quad);
        g_oddSize = static_cast<uint32_t>((b.size() + 31) & ~size_t(31));
        g_odd = MakeList(b, g_oddSize);
    }
    g_rec = fe_alloc(512);
}

void Formats() {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
    GXSetVtxDesc(GX_VA_TEX0, GX_INDEX8);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_S8, 0);
    GXSetArray(GX_VA_POS, Guest(g_pos), 3);
    GXSetArray(GX_VA_TEX0, Guest(g_crd), 2);
}
void Matrices(unsigned frame) {
    for (int i = 0; i < 2; ++i) {
        Mtx m = {};
        m[0][0] = 1.0f + 0.25f * static_cast<float>(Rn(3));
        m[1][1] = 1.0f;
        m[2][2] = 1.0f;
        m[0][3] = static_cast<float>(static_cast<int>(Rn(5)) - 2) * 10.0f;
        m[1][3] = static_cast<float>(frame % 3) * 5.0f;
        m[2][3] = -100.0f - static_cast<float>(i) * 20.0f;
        GXLoadPosMtxImm(m, i ? GX_PNMTX1 : GX_PNMTX0);
    }
}
void CallKeep(uint32_t addr, uint32_t size, int keep) {
    __PortGXDedupKeep = static_cast<unsigned char>(keep);
    GXCallDisplayList(Guest(addr), size);
    __PortGXDedupKeep = 0;
}
// One object: the GX library's setters (most repeat), J3D's direct writes, a draw
void Object(unsigned frame) {
    static const GXColor kCol[3] = {{255, 128, 64, 255}, {16, 32, 48, 128}, {255, 255, 255, 255}};
    GXSetNumTevStages(1);
    GXSetNumChans(1);
    GXSetNumTexGens(Rn(4) == 0 ? 0 : 1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP_NULL, GX_COLOR0A0);
    if (Rn(2)) GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_C0, GX_CC_RASC, GX_CC_ZERO);
    else GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, Rn(2) ? GX_CA_A0 : GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevColor(GX_TEVREG0, kCol[Rn(3)]);
    if (Rn(2)) GXSetTevKColor(GX_KCOLOR0, kCol[Rn(2)]);
    GXSetTevKColorSel(GX_TEVSTAGE0, Rn(2) ? GX_TEV_KCSEL_K0 : GX_TEV_KCSEL_1);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, Rn(3) ? GX_ALWAYS : GX_GREATER, Rn(3) ? 0 : 16);
    if (Rn(2)) GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    else GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GXSetZMode(GX_TRUE, Rn(4) ? GX_LEQUAL : GX_ALWAYS, Rn(3) != 0);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    GXSetChanMatColor(GX_COLOR0A0, kCol[Rn(2)]);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
    if (Rn(6) == 0) GXSetCoPlanar(GX_FALSE); // (a masked gen-mode write)
    if (Rn(5) == 0) Formats();               // (the formats again: their own dirty flush)
    GXSetCurrentMtx(Rn(3) ? GX_PNMTX0 : GX_PNMTX1);
    if (Rn(3) == 0) Matrices(frame);
    const int how = static_cast<int>(Rn(10));
    if (how < 6) { // (as J3D draws a shape: the material list (BP / XF), the array bases, the matrix indices on CP and XF, the geometry list)
        if (Rn(2)) {
            const int k = static_cast<int>(Rn(2));
            CallKeep(g_mat[k], g_matSize[k], g_mutant == 2 ? PORT_GXDD_BUMP_PRIMS : PORT_GXDD_BUMP_NOCP); // (GXDD_E2E_MUTANT=2: vouched wrongly)
        }
        __PortGXWriteCP(0xA0, g_pos);
        __PortGXWriteCP(0xA4, g_crd);
        const uint32_t a = Rn(3) ? 0u : 3u;
        __PortGXWriteCP(0x30, a);
        __PortGXWriteCP(0x40, 0x00F3CF3Cu);
        __PortGXWriteXF2(0x1018, a, 0x00F3CF3Cu);
        CallKeep(g_geo, 64, PORT_GXDD_BUMP_PRIMS);
    } else if (how < 8) { // (immediate)
        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        for (u8 i = 0; i < 4; ++i) { GXPosition1x8(i); GXTexCoord1x8(i); }
        GXEnd();
    } else if (how == 8) { // (a list nobody vouches for; GXDD_E2E_MUTANT=1: the caller vouches wrongly, "primitives only")
        if (g_mutant == 1) CallKeep(g_odd, g_oddSize, PORT_GXDD_BUMP_PRIMS);
        else GXCallDisplayList(Guest(g_odd), g_oddSize);
    } else { // (a list recorded now and called: the gate is not live while the library writes into it)
        GXBeginDisplayList(fe_ram() + g_rec, 512);
        GXSetTevColor(GX_TEVREG0, kCol[Rn(3)]);
        GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
        GXSetArray(GX_VA_POS, Guest(g_pos), 3);
        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        for (u8 i = 0; i < 4; ++i) { GXPosition1x8(i); GXTexCoord1x8(i); }
        GXEnd();
        const u32 n = GXEndDisplayList();
        fe_written(g_rec, 512);
        if (n) GXCallDisplayList(Guest(g_rec), n);
    }
    if (Rn(12) == 0) __PortGXDedupBump(PORT_GXDD_BUMP_ALL, PORT_GXDD_CAUSE_MARKER); // (a port marker's invalidation, as port_vgx_marker)
}
void Frame(unsigned frame) {
    if (frame == 1) Formats();
    const int n = 6 + static_cast<int>(Rn(10));
    for (int k = 0; k < n; ++k) {
        Object(frame);
        if (Rn(5) == 0) __PortGXFifoFlush(); // (GXFlush now and then: the stream is parsed in more pieces)
    }
}
} // namespace

int main(int argc, char** argv) {
    const unsigned frames = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 30;
    if (argc > 2) g_rng ^= static_cast<uint64_t>(std::atoll(argv[2])) * 0x9E3779B97F4A7C15ull;
    g_mutant = std::getenv("GXDD_E2E_MUTANT") ? std::atoi(std::getenv("GXDD_E2E_MUTANT")) : 0;
    fe_init();
    port_gx_init();
    static GXFifoObj fifo;
    static uint8_t fifoMem[1 << 16];
    GXInitFifoBase(&fifo, fifoMem, sizeof fifoMem);
    GXSetCPUFifo(&fifo);
    GXSetGPFifo(&fifo); // (linked: the writes go into the staging buffer the front end reads)
    std::memset(&gxData, 0, sizeof gxData);
    gxData.vNumNot = 1;
    gxData.bpSentNot = 1;
    for (int i = 0; i < 16; ++i) gxData.tevc[i] = (0xC0u + 2u * static_cast<u32>(i)) << 24, gxData.teva[i] = (0xC1u + 2u * static_cast<u32>(i)) << 24;
    for (int i = 0; i < 8; ++i) gxData.tref[i] = (0x28u + static_cast<u32>(i)) << 24;
    for (int i = 0; i < 8; ++i) gxData.tevKsel[i] = (0xF6u + static_cast<u32>(i)) << 24;
    for (int i = 0; i < 8; ++i) gxData.suTs0[i] = (0x30u + 2u * static_cast<u32>(i)) << 24, gxData.suTs1[i] = (0x31u + 2u * static_cast<u32>(i)) << 24;
    gxData.iref = 0x27u << 24;
    gxData.lpSize = 0x22u << 24;
    gxData.suScis0 = 0x20u << 24;
    gxData.suScis1 = 0x21u << 24;
    gxData.cmode1 = 0x42u << 24;
    gxData.genMode = 0x00u << 24;
    gxData.cmode0 = 0x41u << 24;
    gxData.zmode = 0x40u << 24;
    gxData.peCtrl = 0x43u << 24;
    gxData.matIdxA = 0x3CF3CF00u;
    gxData.matIdxB = 0x00F3CF3Cu;
    Build();
    for (unsigned f = 1; f <= frames; ++f) {
        Frame(f);
        __PortGXDedupBump(PORT_GXDD_BUMP_ALL, PORT_GXDD_CAUSE_PICTURE); // (as GXCopyDisp: the picture's end)
        __PortGXFifoFlush();
        fe_frame_end(f);
    }
    unsigned long long v[3];
    fe_counters(v);
    std::fprintf(stderr, "gxdedup e2e: %u frames; GXV_GX_DEDUP=%d census %d; front end verify: %llu checked, %llu differing, %llu digests\n", frames,
                 sDdMode, sDdCensus, v[0], v[1], v[2]);
    return 0;
}
