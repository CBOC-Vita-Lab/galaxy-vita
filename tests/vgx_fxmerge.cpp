// GXV_FXMERGE end to end (tools/test_vgx_fxmerge.sh): the game half. JParticle-shaped emitters are drawn through the production GX
// units (GXSetTevColor in GXTev.c, GXLoadPosMtxImm / GXLoadTexMtxImm in GXTransform.c, GXCallDisplayList and the gathering in
// GXDisplayList.c, __GXSendFlushPrim in GXGeometry.c) and the production FIFO (platform/src/gx/gx_fifo.cpp, included below: the
// switch, the staging buffer, __PortGXFifoWriteBytes, the flushes), with port_gx_fx_begin / port_gx_fx_end around every particle loop
// as JPAResource::drawP calls them. The stream goes to the real front end (tests/vgx_fxmerge_fe.cpp), which prints a hash of every
// picture. GXV_FXMERGE=1 must print what GXV_FXMERGE=0 prints.
//   vgx_fxmerge [frames] [seed]        FXT_FOREIGN=1: a stray GX write inside one particle loop (the gathering must notice it, log it
//                                      and stay off from then on)
// The emitters: 1-60 particles (some 300: a loop longer than one command holds), register 0 and / or 1 per particle, a 2x4 texture
// matrix for some, the clip flag's flush primitive (8 vertices, or 20 for a few emitters: then written to the FIFO, not gathered), dirty
// GX state left by the emitter's set-up for some (sent at the first call, as GXCallDisplayList sends it), the 8-vertex list for some, a
// list the front end does not take the short way for others (CallList's way), invisible particles (their colour set, no draw).
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../platform/vgx/src/vgx_common.h"
#include "../platform/src/gx/gpu.h"
#include "../platform/src/gx/vgx_galaxy.h"
#include "port/gx_fifo.h"
#include "port/heap_routing.h"
#include "port/port.h"
#include "revolution/gx.h"
#include "revolution/mtx.h"
#include "revolution/gx/GXRegs.h"

// ---- what gx_fifo.cpp needs (no player thread: every flush is parsed at once, as port_vgx_player() == 0 has it)
extern "C" int port_vgx_player() { return 0; }
extern "C" int port_vgx_seg_idle() { return 0; }
extern "C" int port_vgx_active() { return 1; }
extern "C" uint8_t* port_vgx_publish(uint8_t* p, int) { return p; }
extern "C" void port_vgx_stream_setup(uint8_t*, uint8_t*, size_t) {}
extern "C" void port_perf_record(int64_t) {}
extern "C" void* port_host_alloc(size_t n, int alignment) {
    void* p = nullptr;
    if (posix_memalign(&p, static_cast<size_t>(alignment), n) != 0) std::abort();
    return p;
}
extern "C" void port_fatal(const char*, ...) { std::abort(); }
extern "C" void port_host_alloc_scope_enter() {}
extern "C" void port_host_alloc_scope_leave() {}
extern "C" void port_irq_set_handler(int, void (*)()) {}
extern "C" void port_call_sdk_interrupt(int) {}
#include "../platform/src/gx/gx_fifo.cpp"

extern "C" {
uint8_t* fe_ram();
uint32_t fe_alloc(uint32_t bytes);
void fe_written(uint32_t addr, uint32_t bytes);
void fe_init();
void fe_frame_end(unsigned frame);
void fe_counters(unsigned long long out[6]);
static GXData gxData;
GXData* const __GXData = &gxData;
u32 port_host_to_phys(const void* p) { return static_cast<u32>(reinterpret_cast<uintptr_t>(p)); } // (the guest addresses of the fixture)
// The dirty state GXCallDisplayList sends before a call (the real one sets the generation mode, descriptors, texgens ... from gx): here
// a masked write of the gen-mode register with the value the emitters' set-up gives it (one texgen), with the library's bookkeeping,
// so that its place in the stream (a mask, then the write) is tested without changing what the draws are.
void __GXSetDirtyState(void) {
    const u32 d = gx->dirtyState;
    GX_WRITE_RA_REG(0xFE07FC3Fu);
    GX_WRITE_RA_REG(0x00000001u);
    if (d & 0xFFFFFF00u) gx->bpSentNot = GX_TRUE;
    gx->dirtyState = 0;
}
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
float Rf(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(Rnd() & 0xFFFFFF) / 16777216.0f; }
void* Guest(uint32_t a) { return reinterpret_cast<void*>(static_cast<uintptr_t>(a)); }

uint32_t g_dl, g_dlx, g_dlBp, g_pos, g_crd, g_tex;
struct Emitter { int n; bool env, texMtx, zTest, dirty, invis; int flush; int list; uint32_t prm, envc; };
std::vector<Emitter> g_em;

void Build() {
    uint8_t* ram = fe_ram();
    g_dl = fe_alloc(32);
    { const uint8_t b[] = {0x80, 0, 4, 0, 0, 1, 1, 2, 2, 3, 3}; std::memcpy(ram + g_dl, b, sizeof b); }
    g_dlx = fe_alloc(32);
    { const uint8_t b[] = {0x80, 0, 8, 0, 0, 1, 1, 2, 2, 3, 3, 4, 0, 5, 1, 6, 2, 7, 3}; std::memcpy(ram + g_dlx, b, sizeof b); }
    g_dlBp = fe_alloc(32); // (a BP write, then the quad: not one draw command, so CallList's way)
    { const uint8_t b[] = {0x61, 0xF3, 0x3F, 0x00, 0x00, 0x80, 0, 4, 0, 0, 1, 1, 2, 2, 3, 3}; std::memcpy(ram + g_dlBp, b, sizeof b); }
    g_pos = fe_alloc(8 * 3);
    { const int8_t q[8][3] = {{-25, 25, 0}, {25, 25, 0}, {25, -25, 0}, {-25, -25, 0}, {0, 50, 0}, {50, 0, 0}, {0, -50, 0}, {-50, 0, 0}};
      std::memcpy(ram + g_pos, q, sizeof q); }
    g_crd = fe_alloc(8 * 2);
    { const uint8_t t[16] = {0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 2, 0, 2, 1, 0, 1}; std::memcpy(ram + g_crd, t, sizeof t); }
    g_tex = fe_alloc(32 * 32);
    for (uint32_t i = 0; i < 32 * 32; ++i) ram[g_tex + i] = static_cast<uint8_t>(Rnd());
    fe_written(0x1000, g_tex + 32 * 32 - 0x1000);
    const int count = 12 + static_cast<int>(Rn(6));
    for (int i = 0; i < count; ++i) {
        Emitter e;
        e.n = Rn(8) == 0 ? 300 : 1 + static_cast<int>(Rn(60));
        e.env = Rn(2) == 0;
        e.texMtx = Rn(3) == 0;
        e.zTest = Rn(3) != 0;
        e.dirty = Rn(3) == 0;
        e.invis = Rn(4) == 0;
        e.flush = Rn(3) == 0 ? (Rn(4) == 0 ? 20 : 8) : 0;
        e.list = Rn(5) == 0 ? 1 : (Rn(6) == 0 ? 2 : 0);
        e.prm = Rnd() | 0xFFu;
        e.envc = Rnd();
        g_em.push_back(e);
    }
}

void Bp(u32 v) { GX_WRITE_RA_REG(v); }
void Cp(u32 reg, u32 v) { GX_WRITE_U8(0x08); GX_WRITE_U8(reg); GX_WRITE_U32(v); }
void Xf(u32 addr, u32 v) { GX_WRITE_U8(0x10); GX_WRITE_U32(addr); GX_WRITE_U32(v); }

// One picture: the set-up of JPAEmitterManager::draw, then per emitter its set-up and its particle loop.
void Frame(unsigned frame, bool gather, bool foreign) {
    Bp(0xC0000000u | (0xFu << 12) | (2u << 8) | (8u << 4) | 0xFu | (1u << 19));
    Bp(0xC1000000u | (7u << 13) | (4u << 10) | (1u << 7) | (7u << 4) | (1u << 19));
    Cp(0x50, 2u << 9);                                     // POS index8
    Cp(0x60, 2u);                                          // TEX0 index8
    Cp(0x70, 1u | (3u << 1) | (1u << 21) | (3u << 22));    // fmt 0: XYZ s8, ST s8
    Cp(0x80, 0);
    Cp(0x90, 0);
    Xf(0x1008, 1u << 4);
    Cp(0x30, 60u << 6);
    Xf(0x1018, 60u << 6);
    Xf(0x1009, 0);
    Cp(0xA0, g_pos); Cp(0xB0, 3);
    Cp(0xA4, g_crd); Cp(0xB4, 2);
    for (size_t k = 0; k < g_em.size(); ++k) {
        const Emitter& e = g_em[k];
        Bp(0xFE00FFE7u);
        Bp(0x41000000u | (e.zTest ? 0x0034A1u : 0x0000A1u | (1u << 8) | (1u << 5)));
        Bp(0x40000000u | (e.zTest ? 0x07u : 0x00u));
        Bp(0xF33F0000u);
        Bp(0xC0000000u | (0xFu << 12) | ((e.env ? 4u : 2u) << 8) | (8u << 4) | (e.env ? 2u : 0xFu) | (1u << 19));
        Bp(0x28000000u | 0x3C0u | (0x3C0u << 12));
        Bp(0x94000000u | (g_tex >> 5));
        Bp(0x88000000u | 31u | (31u << 10) | (1u << 20));
        Bp(0x80000195u);
        Bp(0x84000000u);
        Bp(0xFE07FC3Fu);
        Bp(0x00000001u);
        Xf(0x1040, 0x5280);
        Xf(0x1050, 0x3D);
        Xf(0x103F, 1);
        Cp(0x30, e.texMtx ? (30u << 6) : (60u << 6));
        Xf(0x1018, e.texMtx ? (30u << 6) : (60u << 6));
        // (what setPTev and the emitter's functions leave for the first call: the flush primitive's setting, maybe dirty state)
        gx->vNum = static_cast<u16>(e.flush);
        gx->vNumNot = !gx->vNum;
        gx->bpSentNot = GX_TRUE;
        gx->vLim = 2;
        gx->dirtyState = e.dirty ? 0x04000004u : 0u;
        const bool on = gather && port_gx_fx_begin();
        for (int i = 0; i < e.n; ++i) {
            const u8 a = static_cast<u8>((frame * 7 + static_cast<unsigned>(i) * 13) & 255);
            const GXColor prm = {static_cast<u8>(e.prm >> 24), static_cast<u8>(e.prm >> 16), static_cast<u8>(e.prm >> 8), a};
            GXSetTevColor(GX_TEVREG0, prm);
            if (e.env) {
                const GXColor env = {static_cast<u8>(e.envc >> 24), static_cast<u8>(e.envc >> 16), static_cast<u8>(i), 0};
                GXSetTevColor(GX_TEVREG1, env);
            }
            if (e.invis && (i % 3) == 1) continue; // (JPADrawBillboard returns for an invisible particle: its colour was set)
            if (e.texMtx) {
                const float age = static_cast<float>(i) * 0.25f + static_cast<float>(frame & 63) * 0.1f;
                Mtx t = {{1.0f, 0.0f, 0.5f + age * 0.01f, 0.0f}, {0.0f, 1.0f, age * 0.02f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}};
                GXLoadTexMtxImm(t, GX_TEXMTX0, GX_MTX2x4);
            }
            Mtx m = {};
            const float sx = Rf(0.5f, 3), sy = Rf(0.5f, 3);
            if (Rn(3) == 0) {
                const float c = Rf(-1, 1), s = Rf(-1, 1);
                m[0][0] = c * sx; m[0][1] = -s * sy; m[1][0] = s * sx; m[1][1] = c * sy;
            } else {
                m[0][0] = sx; m[1][1] = sy;
            }
            m[2][2] = 1.0f;
            m[0][3] = Rf(-30, 30); m[1][3] = Rf(-20, 20); m[2][3] = Rf(-200, -20);
            GXLoadPosMtxImm(m, GX_PNMTX0);
            static bool strayDone = false;
            if (foreign && on && i == 2 && !strayDone) { // (FXT_FOREIGN: a write the gathering does not take, in the first gathered loop)
                Bp(0xF33F0000u);
                strayDone = true;
            }
            GXCallDisplayList(Guest(e.list == 1 ? g_dlx : e.list == 2 ? g_dlBp : g_dl), 32);
        }
        if (on) port_gx_fx_end();
        gx->vNum = 0; // (GXSetMisc(GX_MT_XF_FLUSH, 0) after the loop)
        gx->vNumNot = 1;
        gx->bpSentNot = GX_TRUE;
        if (Rn(4) == 0) __PortGXFifoFlush(); // (GXFlush now and then: the stream is parsed in more pieces)
    }
}
} // namespace

int main(int argc, char** argv) {
    const unsigned frames = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 30;
    if (argc > 2) g_rng ^= static_cast<uint64_t>(std::atoll(argv[2])) * 0x9E3779B97F4A7C15ull;
    const bool foreign = std::getenv("FXT_FOREIGN") != nullptr;
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
    Build();
    const char* env = std::getenv("GXV_FXMERGE");
    const bool gather = env && std::atoi(env) == 1;
    for (unsigned f = 1; f <= frames; ++f) {
        Frame(f, gather, foreign);
        __PortGXFifoFlush();
        fe_frame_end(f);
    }
    unsigned long long fx[6];
    fe_counters(fx);
    std::fprintf(stderr, "fxmerge test: %u frames; particle commands %llu, items %llu, list calls %llu, short %llu, decodes reused %llu, through CallList %llu; "
                         "gathering %s\n", frames, fx[0], fx[1], fx[2], fx[3], fx[4], fx[5], gather ? (__PortGXFxMergeOk() ? "on" : "off at the end") : "not asked");
    return 0;
}
