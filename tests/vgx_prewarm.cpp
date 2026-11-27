// GXV_PREWARM end to end (tools/test_vgx_prewarm.sh): the game half. JParticle-shaped effects ("resources": a one-quad list, the 8-vertex
// list, a stripe drawn with GXBegin, with their own blend, alpha compare, Z mode, TEV inputs, texture matrix) are drawn through the
// production GX units (GXTev.c, GXTransform.c, GXDisplayList.c, GXGeometry.c) and the production FIFO (platform/src/gx/gx_fifo.cpp) in
// draw groups whose set-up differs as JPAEmitterManager::draw's callers' does (colour / alpha updates, destination alpha, fog), with a
// J3D-like mesh after each group that sets only part of the state and draws with whatever the group left. The stream goes to the real
// front end (tests/vgx_prewarm_fe.cpp).
//   PW=1: in the first PW_CLOSED frames ("behind the closed screen", no effect is emitted yet), each group's set-up is followed by a warm-up
//   scope per resource of the group not warmed yet (at most PW_SLICE a group a frame: the slices go on in the next frame), as
//   decomp JPAEmitterManager.cpp PortPrewarmGroup does it: GXData saved, PORT_GX_MARK_WARM_BEGIN, the resource drawn with two particles,
//   PORT_GX_MARK_WARM_END, GXData and the draw counter put back. From frame PW_CLOSED + 1 on, the resources are emitted in play, each
//   from a frame of its own, with 1..80 particles (a stripe of more than 64 particles has more than GXV_SOFTXFORM vertices).
//   PW=0: the same frames without the scopes.
// The front-end half prints a canonical hash of every picture (shader ids by key, texture ids by content) and checks (a) every program
// variant a real draw is recorded with was queued by a warm-up scope, and no shader is made after the closed frames; tools/test_vgx_prewarm.sh
// compares the pictures of PW=1 and PW=0 (b).
//   vgx_prewarm [frames] [seed]
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
int fe_report(unsigned closedFrames);
static GXData gxData;
GXData* const __GXData = &gxData;
u32 port_host_to_phys(const void* p) { return static_cast<u32>(reinterpret_cast<uintptr_t>(p)); }
// (as tests/vgx_fxmerge.cpp: the dirty state GXCallDisplayList / GXBegin send, a masked gen-mode write with the library's bookkeeping)
void __GXSetDirtyState(void) {
    const u32 d = gx->dirtyState;
    GX_WRITE_RA_REG(0xFE07FC3Fu);
    GX_WRITE_RA_REG(0x00000001u);
    if (d & 0xFFFFFF00u) gx->bpSentNot = GX_TRUE;
    gx->dirtyState = 0;
}
extern u32 __PortGXDrawSerial;
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
void Bp(u32 v) { GX_WRITE_RA_REG(v); }
void Cp(u32 reg, u32 v) { GX_WRITE_U8(0x08); GX_WRITE_U8(reg); GX_WRITE_U32(v); }
void Xf(u32 addr, u32 v) { GX_WRITE_U8(0x10); GX_WRITE_U32(addr); GX_WRITE_U32(v); }
void Marker(u32 kind) { GX_WRITE_U8(0x61); GX_WRITE_U32((0xFFu << 24) | kind); } // (vgx_galaxy.cpp port_vgx_marker's form)

uint32_t g_dl, g_dlx, g_mesh, g_pos, g_crd, g_tex[3];
struct Res {
    int kind;         // 0 the one-quad list, 1 the 8-vertex list, 2 a stripe (GXBegin, direct F32 positions and coordinates)
    int blend;        // 0 none, 1 source alpha / inverse, 2 additive
    bool alphaTest, zTest, zUpd, env, texMtx;
    int tex, group, activeFrom, n;
    uint32_t prm, envc;
};
std::vector<Res> g_res;
constexpr int kGroups = 3;
bool g_warmed[64];

void Build(unsigned closedFrames, unsigned frames) {
    uint8_t* ram = fe_ram();
    g_dl = fe_alloc(32);
    { const uint8_t b[] = {0x80, 0, 4, 0, 0, 1, 1, 2, 2, 3, 3}; std::memcpy(ram + g_dl, b, sizeof b); }
    g_dlx = fe_alloc(32);
    { const uint8_t b[] = {0x80, 0, 8, 0, 0, 1, 1, 2, 2, 3, 3, 4, 0, 5, 1, 6, 2, 7, 3}; std::memcpy(ram + g_dlx, b, sizeof b); }
    g_mesh = fe_alloc(64); // (the mesh: five triangles of the 8 positions, 15 vertices: a count no effect's record has - the front-end half tells them apart by it)
    { uint8_t b[3 + 30] = {0x90, 0, 15};
      for (int i = 0; i < 15; ++i) { b[3 + 2 * i] = static_cast<uint8_t>((i * 3) % 8); b[4 + 2 * i] = static_cast<uint8_t>(i % 4); }
      std::memcpy(ram + g_mesh, b, sizeof b); }
    g_pos = fe_alloc(8 * 3);
    { const int8_t q[8][3] = {{-25, 25, 0}, {25, 25, 0}, {25, -25, 0}, {-25, -25, 0}, {0, 50, 0}, {50, 0, 0}, {0, -50, 0}, {-50, 0, 0}};
      std::memcpy(ram + g_pos, q, sizeof q); }
    g_crd = fe_alloc(8 * 2);
    { const uint8_t t[16] = {0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 2, 0, 2, 1, 0, 1}; std::memcpy(ram + g_crd, t, sizeof t); }
    for (int t = 0; t < 3; ++t) {
        g_tex[t] = fe_alloc(32 * 32);
        for (uint32_t i = 0; i < 32 * 32; ++i) ram[g_tex[t] + i] = static_cast<uint8_t>(Rnd());
    }
    fe_written(0x1000, g_tex[2] + 32 * 32 - 0x1000);
    const int count = 14 + static_cast<int>(Rn(8));
    for (int i = 0; i < count; ++i) {
        Res r;
        r.kind = Rn(4) == 0 ? 2 : (Rn(5) == 0 ? 1 : 0);
        r.blend = static_cast<int>(Rn(3));
        r.alphaTest = Rn(2) == 0;
        r.zTest = Rn(3) != 0;
        r.zUpd = Rn(4) == 0;
        r.env = Rn(2) == 0;
        r.texMtx = r.kind != 2 && Rn(3) == 0;
        r.tex = static_cast<int>(Rn(3));
        r.group = static_cast<int>(Rn(kGroups));
        r.activeFrom = static_cast<int>(closedFrames + 1 + Rn(frames > closedFrames + 2 ? frames - closedFrames - 1 : 1));
        r.n = r.kind == 2 ? (Rn(2) == 0 ? 2 + static_cast<int>(Rn(20)) : 65 + static_cast<int>(Rn(20))) : 1 + static_cast<int>(Rn(40));
        r.prm = Rnd() | 0xFFu;
        r.envc = Rnd();
        g_res.push_back(r);
    }
}

// JPAEmitterManager::draw's set-up, then the group's own differences (what its caller set before: the colour / alpha updates, the
// destination alpha, the fog)
void GroupSetup(int group) {
    Bp(0xC0000000u | (0xFu << 12) | (2u << 8) | (8u << 4) | 0xFu | (1u << 19));
    Bp(0xC1000000u | (7u << 13) | (4u << 10) | (1u << 7) | (7u << 4) | (1u << 19));
    Cp(0x50, 2u << 9);                                     // POS index8
    Cp(0x60, 2u);                                          // TEX0 index8
    Cp(0x70, 1u | (3u << 1) | (1u << 21) | (3u << 22));    // fmt 0: XYZ s8, ST s8
    Cp(0x71, 1u | (4u << 1) | (1u << 21) | (4u << 22));    // fmt 1: XYZ f32, ST f32
    Cp(0x80, 0);
    Cp(0x90, 0);
    Xf(0x1008, 1u << 4);
    Cp(0x30, 60u << 6);
    Xf(0x1018, 60u << 6);
    Xf(0x1009, 0);
    Cp(0xA0, g_pos); Cp(0xB0, 3);
    Cp(0xA4, g_crd); Cp(0xB4, 2);
    Bp(0x42000000u | (group == 1 ? (1u << 8) | 0x80u : 0u));                                  // destination alpha
    Bp(0xF1000000u | (group == 2 ? (2u << 21) : 0u));                                          // fog: linear in group 2
}
// The colour / alpha update bits of a group (GXSetColorUpdate / GXSetAlphaUpdate before the draw: kept by GXSetBlendMode)
u32 GroupUpdates(int group) { return group == 0 ? 0x18u : (group == 1 ? 0x08u : 0x18u); }

void ResSetup(const Res& r, int group) {
    u32 cm0 = GroupUpdates(group);
    if (r.blend == 1) cm0 |= 1u | (4u << 8) | (5u << 5);
    else if (r.blend == 2) cm0 |= 1u | (1u << 8) | (1u << 5);
    Bp(0xFE00FFE7u);
    Bp(0x41000000u | cm0);
    Bp(0x40000000u | (r.zTest ? 0x01u | (3u << 1) : 0u) | (r.zUpd ? 0x10u : 0u));
    Bp(r.alphaTest ? (0xF3000000u | (4u << 16) | (7u << 19) | 0x40u) : 0xF33F0000u);
    Bp(0xC0000000u | (0xFu << 12) | ((r.env ? 4u : 2u) << 8) | (8u << 4) | (r.env ? 2u : 0xFu) | (1u << 19));
    Bp(0x28000000u | 0x3C0u | (0x3C0u << 12));
    Bp(0x94000000u | (g_tex[r.tex] >> 5));
    Bp(0x88000000u | 31u | (31u << 10) | (1u << 20));
    Bp(0x80000195u);
    Bp(0x84000000u);
    Bp(0xFE07FC3Fu);
    Bp(0x00000001u);
    Xf(0x1040, 0x5280);
    Xf(0x1050, 0x3D);
    Xf(0x103F, 1);
    Cp(0x30, r.texMtx ? (30u << 6) : (60u << 6));
    Xf(0x1018, r.texMtx ? (30u << 6) : (60u << 6));
    if (r.kind == 2) { Cp(0x50, 1u << 9); Cp(0x60, 1u); } // (JPADrawStripe: GXSetVtxDesc POS / TEX0 GX_DIRECT)
    else { Cp(0x50, 2u << 9); Cp(0x60, 2u); }
    gx->vNum = 0;
    gx->vNumNot = 1;
    gx->bpSentNot = GX_TRUE;
    gx->vLim = 2;
}
void RandomMtx(Mtx m) {
    std::memset(m, 0, sizeof(Mtx));
    const float sx = Rf(0.5f, 3), sy = Rf(0.5f, 3);
    m[0][0] = sx; m[1][1] = sy; m[2][2] = 1.0f;
    m[0][3] = Rf(-30, 30); m[1][3] = Rf(-20, 20); m[2][3] = Rf(-200, -20);
}
// The resource's particles (JPAResource::drawP's loop, or JPADrawStripe for a stripe)
void ResDraw(const Res& r, int n, unsigned frame) {
    if (r.kind == 2) {
        Mtx m;
        RandomMtx(m);
        GXLoadPosMtxImm(m, GX_PNMTX0);
        const GXColor prm = {static_cast<u8>(r.prm >> 24), static_cast<u8>(r.prm >> 16), static_cast<u8>(r.prm >> 8), 200};
        GXSetTevColor(GX_TEVREG0, prm);
        if (r.env) { const GXColor env = {static_cast<u8>(r.envc >> 24), static_cast<u8>(r.envc >> 16), 9, 0}; GXSetTevColor(GX_TEVREG1, env); }
        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT1, static_cast<u16>(n * 2));
        for (int i = 0; i < n; ++i) {
            for (int side = 0; side < 2; ++side) {
                GX_WRITE_F32(Rf(-20, 20)); GX_WRITE_F32(static_cast<float>(i)); GX_WRITE_F32(Rf(-5, 5));
                GX_WRITE_F32(static_cast<float>(side)); GX_WRITE_F32(static_cast<float>(i) / static_cast<float>(n));
            }
        }
        return;
    }
    for (int i = 0; i < n; ++i) {
        const u8 a = static_cast<u8>((frame * 7 + static_cast<unsigned>(i) * 13) & 255);
        const GXColor prm = {static_cast<u8>(r.prm >> 24), static_cast<u8>(r.prm >> 16), static_cast<u8>(r.prm >> 8), a};
        GXSetTevColor(GX_TEVREG0, prm);
        if (r.env) {
            const GXColor env = {static_cast<u8>(r.envc >> 24), static_cast<u8>(r.envc >> 16), static_cast<u8>(i), 0};
            GXSetTevColor(GX_TEVREG1, env);
        }
        if (r.texMtx) {
            const float age = static_cast<float>(i) * 0.25f + static_cast<float>(frame & 63) * 0.1f;
            Mtx t = {{1.0f, 0.0f, 0.5f + age * 0.01f, 0.0f}, {0.0f, 1.0f, age * 0.02f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}};
            GXLoadTexMtxImm(t, GX_TEXMTX0, GX_MTX2x4);
        }
        Mtx m;
        RandomMtx(m);
        GXLoadPosMtxImm(m, GX_PNMTX0);
        GXCallDisplayList(Guest(r.kind == 1 ? g_dlx : g_dl), 32);
    }
}
// A J3D-like mesh after a group: in the closed frames it sets its TEV inputs and its texture only, and draws with the blend, Z mode, alpha
// compare, vertex descriptor and matrices the group's set-up (and, with PW=1, the scopes) left: what the scopes must not change, (b). In play
// it also sets its blend, Z mode and alpha compare (as a J3D material does), so that the effects emitted before it give it no key of theirs:
// (a) is about the effects' keys.
void Mesh(int group, bool inPlay) {
    if (inPlay) {
        Bp(0xFE00FFE7u);
        Bp(0x41000000u | GroupUpdates(group));
        Bp(0x40000000u | 0x01u | (3u << 1) | 0x10u);
        Bp(0xF33F0000u);
        Bp(0x28000000u | 0x340u | (0x3C0u << 12));
        Cp(0x30, 60u << 6);
        Xf(0x1018, 60u << 6);
    }
    Bp(0xC0000000u | (0xFu << 12) | (0xFu << 8) | (8u << 4) | (group == 1 ? 0xAu : 0xFu) | (1u << 19));
    Bp(0x94000000u | (g_tex[(group + 1) % 3] >> 5));
    Cp(0x50, 2u << 9);
    Cp(0x60, 2u);
    Mtx m;
    RandomMtx(m);
    GXLoadPosMtxImm(m, GX_PNMTX0);
    GXCallDisplayList(Guest(g_mesh), 64);
}

// One picture: per group its set-up, (PW) the warm-up scopes, the emitted resources, then the mesh.
void Frame(unsigned frame, bool pw, unsigned closedFrames, unsigned slice) {
    for (int group = 0; group < kGroups; ++group) {
        GroupSetup(group);
        // (an accident of the previous emitters: something none of them sets, left in a state the scopes' draws change)
        Bp(0x28000000u | (Rn(2) ? 0x3C0u : 0x340u) | (0x3C0u << 12));
        if (pw && frame <= closedFrames) {
            unsigned done = 0;
            for (size_t k = 0; k < g_res.size() && done < slice; ++k) {
                const Res& r = g_res[k];
                if (r.group != group || g_warmed[k]) continue;
                static GXData saved;
                saved = gxData;
                const u32 serial = __PortGXDrawSerial;
                const uint64_t rng = g_rng; // (the scope's random matrices must not move the frame's own)
                Marker(14);
                ResSetup(r, group);
                ResDraw(r, 2, frame);
                Marker(15);
                g_rng = rng;
                gxData = saved;
                __PortGXDrawSerial = serial;
                g_warmed[k] = true;
                ++done;
            }
        }
        for (size_t k = 0; k < g_res.size(); ++k) {
            const Res& r = g_res[k];
            if (r.group != group || static_cast<int>(frame) < r.activeFrom) continue;
            ResSetup(r, group);
            ResDraw(r, r.n, frame);
        }
        Mesh(group, frame > closedFrames);
        if (Rn(4) == 0) __PortGXFifoFlush();
    }
}
} // namespace

int main(int argc, char** argv) {
    const unsigned frames = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 24;
    if (argc > 2) g_rng ^= static_cast<uint64_t>(std::atoll(argv[2])) * 0x9E3779B97F4A7C15ull;
    const char* e = std::getenv("PW");
    const bool pw = e && std::atoi(e) != 0;
    const unsigned closedFrames = std::getenv("PW_CLOSED") ? static_cast<unsigned>(std::atoi(std::getenv("PW_CLOSED"))) : 4;
    const unsigned slice = std::getenv("PW_SLICE") ? static_cast<unsigned>(std::atoi(std::getenv("PW_SLICE"))) : 3;
    fe_init();
    port_gx_init();
    static GXFifoObj fifo;
    static uint8_t fifoMem[1 << 16];
    GXInitFifoBase(&fifo, fifoMem, sizeof fifoMem);
    GXSetCPUFifo(&fifo);
    GXSetGPFifo(&fifo);
    std::memset(&gxData, 0, sizeof gxData);
    gxData.vNumNot = 1;
    gxData.bpSentNot = 1;
    Build(closedFrames, frames);
    for (unsigned f = 1; f <= frames; ++f) {
        Frame(f, pw, closedFrames, slice);
        __PortGXFifoFlush();
        fe_frame_end(f);
    }
    unsigned left = 0;
    for (size_t k = 0; k < g_res.size(); ++k) left += pw && !g_warmed[k] ? 1u : 0u;
    std::fprintf(stderr, "prewarm test: %u frames, %zu resources (%u not warmed in the %u closed frames)\n", frames, g_res.size(), left, closedFrames);
    return fe_report(pw ? closedFrames : 0) + (left ? 1 : 0);
}
