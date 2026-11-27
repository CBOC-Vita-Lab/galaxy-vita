// GXV_LAYOUTWARM end to end (tools/test_vgx_layoutwarm.sh): the game half. nw4r::lyt-shaped layouts - panes with REAL nw4r::lyt::Material objects
// (lyt_material.cpp: SetupGX, the production TEV / texgen / channel / alpha-compare / blend / indirect set-up of 0-3 textures with and without explicit
// TEV stages) drawn through the real detail::SetVertexFormat / detail::DrawQuad (lyt_common.cpp) in Picture::DrawSelf's sequence - go through the production
// GX units (GXTev, GXTransform, GXAttr, GXLight, GXPixel, GXTexture, GXBump, GXDisplayList, GXGeometry) and the production FIFO (platform/src/gx/gx_fifo.cpp)
// to the real front end (tests/vgx_prewarm_fe.cpp, which prints a canonical hash of every picture and checks (a)).
//   LW=0: "behind the closed screen" (the first LW_CLOSED frames) nothing is drawn; then, from frame LW_CLOSED + 1, each layout appears in a frame of its own
//   and draws its visible panes, with the alpha a pane has while it fades in (255, then partial, then 255) and vertex colours some panes have: the 2D pass's
//   real order - the 2D set-up (MR::drawInitFor2DModel's fog / update / Z state), a 3D-model-like draw that changes blend, TEV and Z state, the layout's
//   own preparation (MR::setupDrawForNW4RLayout), the panes (Pane::Draw's visibility test).
//   LW=1: in the closed frames, in the 2D pass's state right after its set-up, each layout (at most LW_SLICE a frame) is drawn once inside a warm-up scope
//   (PORT_GX_MARK_WARM_BEGIN / _END) as decomp Game/Screen/LayoutManager.cpp LwWarmOne does it: GXData saved, the markers, the layout's preparation, then
//   EVERY pane's DrawSelf (visible or not) in pass 1 (Material::SetupGX modulate false, alpha 255) and pass 2 (true, 128) through
//   nw4r::lyt::detail::gPortWarmPass, the GX state and draw counter put back.
//   LW_CENSUS=1: every in-play layout draw is bracketed by PORT_GX_MARK_LAYOUT_BEGIN / _END (kinds 19 / 20, the layout's id in bits 8-23).
// tools/test_vgx_layoutwarm.sh compares the pictures of LW=1 and LW=0 (b) and reads fe_report's (a): every program variant a real layout draw is
// recorded with after the closed frames was queued by a scope, and no shader is made after them.
//   vgx_layoutwarm [frames] [seed]
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
#include "nw4r/lyt/common.h"
#include "nw4r/lyt/layout.h"
#include "nw4r/lyt/material.h"

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

// ---- what nw4r::lyt::Material needs of the layout allocator
void* MEMAllocFromAllocator(MEMAllocator*, u32 n) { return std::calloc(1, n); }
void MEMFreeToAllocator(MEMAllocator*, void* p) { std::free(p); }
namespace nw4r { namespace lyt { MEMAllocator* Layout::mspAllocator = nullptr; } }

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
extern u32 __PortGXDrawSerial;
}

using namespace nw4r;
namespace {
uint64_t g_rng = 0x2545F4914F6CDD1Dull;
uint32_t Rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return static_cast<uint32_t>(g_rng >> 11);
}
uint32_t Rn(uint32_t n) { return Rnd() % n; }
void* Guest(uint32_t a) { return reinterpret_cast<void*>(static_cast<uintptr_t>(a)); }
void Marker(u32 kind) { GX_WRITE_U8(0x61); GX_WRITE_U32((0xFFu << 24) | kind); } // (vgx_galaxy.cpp port_vgx_marker's form)

uint32_t g_tex[3];
struct Pane {
    lyt::Material* mat = nullptr;
    ut::Color vtx[4];
    u8 alpha = 255;       // the pane's alpha when it is fully shown
    bool fades = false;   // it fades in: 90, then 255
    bool vertexColors = false;
    bool shownAtStart = false;
    int shownFrom = 0;    // the first in-play frame it is visible in
    u8 nTex = 0;
    float x = 0, y = 0, w = 0, h = 0;
};
struct Layout {
    std::vector<Pane> panes;
    int appearFrom = 0; // the first in-play frame the layout draws in
    bool warmed = false;
};
std::vector<Layout> g_layouts;
bool g_census = false;

lyt::Material* MakeMaterial() {
    lyt::Material* m = new lyt::Material();
    const int kind = static_cast<int>(Rn(7));
    const u8 texN = kind == 0 ? 0 : (kind == 1 || kind >= 4) ? 1 : (kind == 2 ? 2 : 3);
    const bool stages = kind >= 4;
    const u8 stageN = stages ? static_cast<u8>(1 + Rn(3)) : 0;
    const bool chan = Rn(2) == 0, matcol = Rn(3) == 0, alpc = Rn(3) == 0, blend = Rn(2) == 0, swap = stages && Rn(3) == 0;
    const bool indirect = stages && texN >= 1 && Rn(6) == 0;
    m->ReserveGXMem(texN + (indirect ? 1 : 0), static_cast<u8>(texN + 1), static_cast<u8>(texN + (indirect ? 1 : 0)), stageN, swap, indirect ? 1 : 0, indirect ? 1 : 0,
                    chan, matcol, alpc, blend);
    const u8 ntex = static_cast<u8>(texN + (indirect ? 1 : 0));
    m->SetTextureNum(ntex);
    for (u8 i = 0; i < ntex; ++i) {
        lyt::TexMap& t = m->GetTexMapAry()[i];
        t.SetImage(Guest(g_tex[Rn(3)]));
        t.SetSize(32, 32);
        const GXTexFmt fmts[3] = {GX_TF_I4, GX_TF_IA8, GX_TF_RGBA8};
        t.SetTexelFormat(fmts[Rn(3)]);
        t.SetWrapMode(Rn(2) ? GX_CLAMP : GX_REPEAT, Rn(2) ? GX_CLAMP : GX_REPEAT);
        t.SetFilter(GX_LINEAR, GX_LINEAR);
    }
    m->SetTexCoordGenNum(ntex);
    for (u8 i = 0; i < ntex; ++i)
        m->SetTexCoordGen(i, lyt::TexCoordGen(GX_TG_MTX2x4, static_cast<GXTexGenSrc>(GX_TG_TEX0 + i), Rn(4) == 0 ? GX_TEXMTX0 : GX_IDENTITY));
    for (int i = 0; i < 3; ++i) {
        GXColorS10 c = {static_cast<s16>(Rn(256)), static_cast<s16>(Rn(256)), static_cast<s16>(Rn(256)), static_cast<s16>(Rn(256))};
        if (i == 0 && Rn(2)) c = GXColorS10{0, 0, 0, 0};
        if (i == 1 && Rn(2)) c = GXColorS10{255, 255, 255, 255};
        m->SetTevColor(static_cast<u32>(i), c);
    }
    if (chan) m->SetChanCtrl(lyt::ChanCtrl(Rn(2) ? GX_SRC_VTX : GX_SRC_REG, Rn(2) ? GX_SRC_VTX : GX_SRC_REG));
    if (matcol) m->SetMatColor(Rn(2) ? ut::Color(0xFFFFFFFFu) : ut::Color(Rnd() | 0xFFu));
    if (alpc) m->SetAlphaCompare(lyt::AlphaCompare());
    if (blend) {
        const GXBlendFactor src[3] = {GX_BL_SRCALPHA, GX_BL_SRCALPHA, GX_BL_DSTCLR}, dst[3] = {GX_BL_INVSRCALPHA, GX_BL_ONE, GX_BL_ZERO};
        const int k = static_cast<int>(Rn(3));
        m->SetBlendMode(lyt::BlendMode(Rn(8) == 0 ? GX_BM_NONE : GX_BM_BLEND, src[k], dst[k], GX_LO_SET));
    }
    if (swap) {
        m->SetTevSwapMode(0, lyt::TevSwapMode(GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA));
        m->SetTevSwapMode(1, lyt::TevSwapMode(GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA));
        m->SetTevSwapMode(2, lyt::TevSwapMode(GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA));
        m->SetTevSwapMode(3, lyt::TevSwapMode(GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA));
    }
    if (stageN) {
        m->SetTevStageNum(stageN);
        for (u8 i = 0; i < stageN; ++i) {
            lyt::TevStage st;
            const bool tex = texN > 0 && Rn(4) != 0;
            st.SetOrder(tex ? GX_TEXCOORD0 : GX_TEXCOORD_NULL, tex ? GX_TEXMAP0 : GX_TEXMAP_NULL, GX_COLOR0A0, GX_TEV_SWAP0, GX_TEV_SWAP0);
            const GXTevColorArg ca[4] = {GX_CC_TEXC, GX_CC_RASC, GX_CC_C0, GX_CC_ONE};
            const GXTevAlphaArg aa[4] = {GX_CA_TEXA, GX_CA_RASA, GX_CA_A0, GX_CA_KONST};
            const GXTevColorArg prevC = i ? GX_CC_CPREV : GX_CC_ZERO;
            const GXTevAlphaArg prevA = i ? GX_CA_APREV : GX_CA_ZERO;
            st.SetColorIn(prevC, ca[Rn(4)], ca[Rn(4)], i ? GX_CC_ZERO : ca[Rn(4)]);
            st.SetAlphaIn(prevA, aa[Rn(3)], aa[Rn(3)], i ? GX_CA_ZERO : aa[Rn(3)]);
            st.SetColorOp(GX_TEV_ADD, GX_TB_ZERO, Rn(4) == 0 ? GX_CS_SCALE_2 : GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV, GX_TEV_KCSEL_K0);
            st.SetAlphaOp(GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV, GX_TEV_KASEL_K0_A);
            if (indirect && i == stageN - 1) st.SetIndirect(GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_NONE, GX_ITM_OFF, GX_ITW_OFF, GX_ITW_OFF, false, false, GX_ITBA_OFF);
            m->SetTevStage(i, st);
        }
    }
    if (indirect) {
        m->SetIndStageNum(1);
        m->SetIndStage(0, lyt::IndirectStage(GX_TEXCOORD0, static_cast<GXTexMapID>(ntex - 1), GX_ITS_1, GX_ITS_1));
    }
    return m;
}

void Build(unsigned closedFrames, unsigned frames) {
    uint8_t* ram = fe_ram();
    for (int t = 0; t < 3; ++t) {
        g_tex[t] = fe_alloc(4096);
        for (uint32_t i = 0; i < 4096; ++i) ram[g_tex[t] + i] = static_cast<uint8_t>(Rnd());
    }
    fe_written(0x1000, g_tex[2] + 4096 - 0x1000);
    std::vector<lyt::Material*> pool;
    for (int i = 0; i < 16; ++i) pool.push_back(MakeMaterial());
    const int layouts = 5 + static_cast<int>(Rn(5));
    for (int l = 0; l < layouts; ++l) {
        Layout L;
        L.appearFrom = static_cast<int>(closedFrames + 1 + Rn(frames > closedFrames + 2 ? frames - closedFrames - 1 : 1));
        const int panes = 3 + static_cast<int>(Rn(8));
        for (int p = 0; p < panes; ++p) {
            Pane pa;
            pa.mat = pool[Rn(static_cast<uint32_t>(pool.size()))];
            pa.nTex = static_cast<u8>(pa.mat->GetTextureNum() - (pa.mat->mGXMemNum.indStage ? 1 : 0));
            if (pa.nTex == 0) pa.nTex = 0;
            pa.vertexColors = Rn(3) == 0;
            for (int c = 0; c < 4; ++c) pa.vtx[c] = pa.vertexColors ? ut::Color(Rnd() | 0xFFu) : ut::Color(0xFFFFFFFFu);
            pa.fades = Rn(2) == 0;
            pa.alpha = Rn(5) == 0 ? static_cast<u8>(128 + Rn(100)) : 255;
            // (a pane shown only later in the layout's life: an "appear" animation, a counter that slides in: the layout draws without it first)
            pa.shownFrom = L.appearFrom + (Rn(3) == 0 ? static_cast<int>(Rn(6)) : 0);
            pa.x = static_cast<float>(Rn(400)) - 200; pa.y = static_cast<float>(Rn(300)) - 150; pa.w = 20 + static_cast<float>(Rn(200)); pa.h = 10 + static_cast<float>(Rn(100));
            L.panes.push_back(pa);
        }
        g_layouts.push_back(L);
    }
}

// MR::drawInitFor2DModel's state (the 2D pass's start), after whatever the 3D pass left
void Draw2DSetup() {
    const GXColor black = {0, 0, 0, 0};
    GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, black);
    GXSetZTexture(GX_ZT_DISABLE, GX_TF_Z8, 0);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetZCompLoc(GX_TRUE);
    GXSetDstAlpha(GX_DISABLE, 0);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetNumIndStages(0);
    const f32 proj[4][4] = {{2.0f / 608.0f, 0, 0, -1.0f}, {0, -2.0f / 456.0f, 0, 1.0f}, {0, 0, -0.001f, -1.0f}, {0, 0, 0, 1.0f}};
    GXSetProjection(proj, GX_ORTHOGRAPHIC);
}
// A 3D-model-like draw between the set-up and the layouts' draws (Model3DFor2D / the comet filter / the name plate): it sets blend, Z, TEV state and draws
void IntermediateDraw(unsigned frame) {
    if (Rn(2) == 0) return;
    GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_SET);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXSetAlphaCompare(GX_GREATER, 20, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetNumChans(1);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    (void)frame;
}
// MR::setupDrawForNW4RLayout
void LayoutPrepare() {
    const f32 proj[4][4] = {{1.0f / 304.0f, 0, 0, 0}, {0, 1.0f / 228.0f, 0, 0}, {0, 0, -0.001f, -1.0f}, {0, 0, 0, 1.0f}};
    GXSetProjection(proj, GX_ORTHOGRAPHIC);
    GXSetCullMode(GX_CULL_NONE);
    GXSetZMode(GX_FALSE, GX_NEVER, GX_FALSE);
}
// Picture::DrawSelf (lyt_picture.cpp), with the pane's alpha as Pane::CalculateMtx made it
void PaneDraw(const Pane& p, u8 alpha) {
    Mtx m = {{1, 0, 0, p.x}, {0, 1, 0, p.y}, {0, 0, 1, 0}};
    GXLoadPosMtxImm(m, GX_PNMTX0);   // (Pane::LoadMtx)
    GXSetCurrentMtx(GX_PNMTX0);
    if (p.mat->GetTexCoordGenCap() && p.mat->mGXMemNum.texSRT) {
        Mtx t = {{1, 0, 0.1f, 0}, {0, 1, 0.2f, 0}, {0, 0, 1, 0}};
        GXLoadTexMtxImm(t, GX_TEXMTX0, GX_MTX2x4);
    }
    ut::Color vtx[4] = {p.vtx[0], p.vtx[1], p.vtx[2], p.vtx[3]};
    const bool useVtx = p.mat->SetupGX(lyt::detail::IsModulateVertexColor(vtx, alpha), alpha);
    lyt::detail::SetVertexFormat(useVtx, p.nTex);
    math::VEC2 tc[3][4];
    for (int i = 0; i < 3; ++i) { tc[i][0] = math::VEC2(0, 0); tc[i][1] = math::VEC2(1, 0); tc[i][2] = math::VEC2(0, 1); tc[i][3] = math::VEC2(1, 1); }
    lyt::detail::DrawQuad(math::VEC2(p.x, p.y), lyt::Size(p.w, p.h), p.nTex, tc, useVtx ? vtx : NULL, alpha);
}
u8 AlphaAt(const Pane& p, int frame) {
    const int age = frame - p.shownFrom;
    if (!p.fades) return p.alpha;
    return age == 0 ? 40 : (age == 1 ? 90 : p.alpha);
}
// LayoutManager.cpp LwWarmOne
void WarmLayout(Layout& L) {
    static GXData saved;
    saved = gxData;
    const u32 serial = __PortGXDrawSerial;
    Marker(14);
    LayoutPrepare();
    for (int pass = 1; pass <= 2; ++pass) {
        lyt::detail::gPortWarmPass = pass;
        for (const Pane& p : L.panes) PaneDraw(p, p.alpha); // (DrawSelf of every pane, shown or not)
    }
    lyt::detail::gPortWarmPass = 0;
    Marker(15);
    gxData = saved;
    __PortGXDrawSerial = serial;
}

void Frame(unsigned frame, bool lw, unsigned closedFrames, unsigned slice) {
    Draw2DSetup();
    if (lw && frame <= closedFrames) {
        unsigned done = 0;
        for (Layout& L : g_layouts) {
            if (L.warmed || done >= slice) continue;
            WarmLayout(L);
            L.warmed = true;
            ++done;
        }
    }
    if (frame <= closedFrames) return;
    IntermediateDraw(frame);
    unsigned id = 0;
    for (Layout& L : g_layouts) {
        ++id;
        if (static_cast<int>(frame) < L.appearFrom) continue;
        if (g_census) Marker(19u | (id << 8));
        LayoutPrepare();
        for (const Pane& p : L.panes) {
            if (static_cast<int>(frame) < p.shownFrom) continue; // (Pane::Draw: not visible)
            PaneDraw(p, AlphaAt(p, static_cast<int>(frame)));
        }
        if (g_census) Marker(20u | (id << 8));
    }
    if (Rn(3) == 0) __PortGXFifoFlush();
}
} // namespace

int main(int argc, char** argv) {
    const unsigned frames = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 24;
    if (argc > 2) g_rng ^= static_cast<uint64_t>(std::atoll(argv[2])) * 0x9E3779B97F4A7C15ull;
    const char* e = std::getenv("LW");
    const bool lw = e && std::atoi(e) != 0;
    g_census = std::getenv("LW_CENSUS") && std::atoi(std::getenv("LW_CENSUS")) != 0;
    const unsigned closedFrames = std::getenv("LW_CLOSED") ? static_cast<unsigned>(std::atoi(std::getenv("LW_CLOSED"))) : 4;
    const unsigned slice = std::getenv("LW_SLICE") ? static_cast<unsigned>(std::atoi(std::getenv("LW_SLICE"))) : 3;
    setenv("PW", lw ? "1" : "0", 1); // (the front-end half's closed-frame count for its report)
    if (std::getenv("LW_CLOSED")) setenv("PW_CLOSED", std::getenv("LW_CLOSED"), 1);
    fe_init();
    port_gx_init();
    static GXFifoObj fifo;
    static uint8_t fifoMem[1 << 16];
    GXInitFifoBase(&fifo, fifoMem, sizeof fifoMem);
    GXSetCPUFifo(&fifo);
    GXSetGPFifo(&fifo);
    std::memset(&gxData, 0, sizeof gxData);
    gxData.vNumNot = 1;
    gxData.texRegionCallback = [](const GXTexObj*, GXTexMapID) -> GXTexRegion* { static GXTexRegion r; return &r; };
    gxData.bpSentNot = 1;
    Build(closedFrames, frames);
    for (unsigned f = 1; f <= frames; ++f) {
        Frame(f, lw, closedFrames, slice);
        __PortGXFifoFlush();
        fe_frame_end(f);
    }
    unsigned left = 0;
    for (const Layout& L : g_layouts) left += lw && !L.warmed ? 1u : 0u;
    std::fprintf(stderr, "layoutwarm test: %u frames, %zu layouts (%u not warmed in the %u closed frames)\n", frames, g_layouts.size(), left, closedFrames);
    return fe_report(lw ? closedFrames : 0) + (left ? 1 : 0);
}

// gx_capture.cpp reads/writes the PE register file; the console's hw_regs.c defines it.
extern "C" volatile u32 __PortGXPERegs[0x400];
extern "C" volatile u32 __PortGXPERegs[0x400] = {0};
