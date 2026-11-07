// The player's per-picture work on a J3D-shaped scene (GXV_SLDELTA, GXV_RSMEMO: tools/test_vgx_fe_work.sh), through the real front end.
// The streams are shaped as the game's J3D packets make them (the command order and register sets of the material display lists of the
// Comet Observatory's models, tools/cook data, with values of its own): per material packet the material's list (texture images and
// modes, TEV order, masked texture-coordinate scales, TEV colours with the BG half written three times, konst colours, TEV stages,
// indirect stages, konst selections, masked indirect registers, fog, alpha compare, masked blend / pixel control / gen mode, texture
// matrices, texgens, channel colours and controls), its differed list (TEV colours, a texture matrix, a material colour) written again on
// every picture into the other of its two buffers (J3DDisplayListObj::swapBuffer), mostly with the same bytes, a few animated; per shape
// the vertex-format list (J3DShape::loadPreDrawSetting, only when it is another shape's), the position / normal matrix loads, the array
// bases, the geometry list. Textures are in guest RAM. Objects come and go and swap places.
// Prints a hash of every command buffer: runs with the switches on and off (and GXV_SPLIT=0 / 1) must print the same lines.
//   vgx_fe_work [frames] [seed]
//   FEW_PROFILE=1: no hashing or printing (callgrind: the player's instructions)
//   FEW_FX=<n>: n particle-like quads per picture (see g_fxCount)
//   FEW_JPA=<n>: n particles per picture as JParticle draws them (see g_jpaCount)
//   FEW_FXMERGE=1: those particles as the game's GX library sends them with GXV_FXMERGE=1 (one port-private command per emitter loop)
//   FEW_JPAVAR=1: more kinds of emitters among them (see g_jpaVar)
//   FEW_TRASH=<KiB>: after each picture, write that much of a buffer of its own (the game's turn and the back end pushing the front end's
//                    tables out of the shared cache between pictures; for a cache simulation: valgrind --tool=callgrind --cache-sim=yes)
#include "vgx_state.h"
#include "vgx_fesplit.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" int64_t port_host_time_ns() { static std::atomic<int64_t> n{1}; return n.fetch_add(1000); }
extern "C" int64_t port_thread_cpu_us() { static std::atomic<int64_t> n{1}; return n.fetch_add(1); }
extern "C" void port_log_summary(const char*, ...) {}
extern "C" void port_log(const char* fmt, ...) { // (FEW_LOG=1: the front end's log lines on stderr, e.g. GXV_SPLIT=2's verify summary)
    static const bool on = std::getenv("FEW_LOG") != nullptr;
    if (!on) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
} // namespace vgx

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
uint32_t F2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

constexpr uint32_t kRam = 8u << 20;
alignas(4096) uint8_t g_ram[kRam];
uint32_t g_brk = 0x1000;
uint32_t Alloc(uint32_t bytes) {
    const uint32_t a = g_brk;
    g_brk += (bytes + 31u) & ~31u;
    if (g_brk > (6u << 20)) { std::fprintf(stderr, "fixture: guest RAM exhausted\n"); std::exit(2); }
    return a;
}
const uint8_t* Resolve(uint32_t address, uint32_t* avail) {
    *avail = 0;
    address &= 0x3FFFFFFFu; // (cached / uncached mirrors)
    if (address == 0 || address >= kRam) return nullptr;
    *avail = kRam - address;
    return g_ram + address;
}

void Put32(uint8_t* p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i)); }
void PutLE(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); } // (vertex and matrix arrays are host data in this port: little-endian)
void Put16(uint8_t* p, uint32_t v) { p[0] = static_cast<uint8_t>(v >> 8); p[1] = static_cast<uint8_t>(v); }
struct Bytes : std::vector<uint8_t> {
    void U8(uint32_t v) { push_back(static_cast<uint8_t>(v)); }
    void U16(uint32_t v) { const size_t a = size(); resize(a + 2); Put16(data() + a, v); }
    void U32(uint32_t v) { const size_t a = size(); resize(a + 4); Put32(data() + a, v); }
    void BP(uint32_t reg, uint32_t value) { U8(0x61); U32(reg << 24 | (value & 0xFFFFFF)); }
    void Masked(uint32_t reg, uint32_t mask, uint32_t value) { BP(0xFE, mask); BP(reg, value); }
    void CP(uint32_t reg, uint32_t value) { U8(0x08); U8(reg); U32(value); }
    void XF(uint32_t addr, const std::vector<uint32_t>& w) { U8(0x10); U16(static_cast<uint32_t>(w.size() - 1)); U16(addr); for (uint32_t x : w) U32(x); }
    void Call(uint32_t addr, uint32_t size) { U8(0x40); U32(addr); U32(size); }
};
uint32_t Pad(size_t n) { return static_cast<uint32_t>((n + 31) & ~size_t(31)); }
void Store(uint32_t addr, const Bytes& b, uint32_t bytes) {
    if (b.size() > bytes) { std::fprintf(stderr, "fixture: list does not fit\n"); std::exit(2); }
    std::memset(g_ram + addr, 0, bytes);
    std::memcpy(g_ram + addr, b.data(), b.size());
    vgx::hle::NotifyGuestWrite(g_ram + addr, bytes);
}
struct List { uint32_t addr = 0, size = 0; };
// (the lists made again every picture live in memory of their own, as the game's heaps keep them apart from the model data: a
// write tracks 4 KiB pages)
uint32_t g_brkDyn = 6u << 20;
List NewList(const Bytes& b, uint32_t room = 0, bool dyn = false) {
    List l{0, Pad(b.size() + room)};
    if (dyn) { l.addr = g_brkDyn; g_brkDyn += l.size; } else l.addr = Alloc(l.size);
    Store(l.addr, b, l.size);
    return l;
}

// ---- textures: I8 images in guest RAM
constexpr int kTex = 24;
uint32_t g_tex[kTex], g_texW[kTex], g_texH[kTex];

// ---- a J3D material: the values its lists carry
struct Material {
    int texmaps = 1, stages = 1, texgens = 1;
    bool ind = false, texMtx = false;
    uint32_t tex[2] = {}, tevOrder = 0, tevReg[3][2] = {}, konst[4][2] = {}, stage[3][2] = {}, ksel[8] = {}, alpha = 0, blend = 0, zmode = 0,
             fog[6] = {}, matColor = 0, amb = 0, chan[4] = {};
    uint32_t mtx[16] = {};
    // the differed list's values (animated materials change them)
    uint32_t dReg[3][2] = {}, dMtx[12] = {}, dColor = 0;
    bool animated = false;
};
uint32_t Tc(int v) { return static_cast<uint32_t>(v) & 0x7FF; }
uint32_t TevRA(int r, int a, bool k) { return Tc(r) | (Tc(a) << 12) | (k ? 0x800000u : 0u); }
Material NewMaterial() {
    Material m;
    m.texmaps = 1 + (Rn(3) == 0);
    m.texgens = m.texmaps;
    m.stages = 1 + static_cast<int>(Rn(3));
    m.ind = Rn(4) == 0;
    m.texMtx = Rn(3) == 0;
    for (int t = 0; t < m.texmaps; ++t) m.tex[t] = Rn(kTex);
    m.tevOrder = 0x049040u | (Rn(2) << 3);
    const bool oor = Rn(5) == 0; // (TEV registers outside 0..255: GXV_FIX_TEVIN's State::tevOor)
    auto comp = [oor] { return oor && Rn(3) == 0 ? static_cast<int>(Rn(1024)) - 400 : static_cast<int>(Rn(256)); };
    for (int r = 0; r < 3; ++r) {
        m.tevReg[r][0] = TevRA(comp(), comp(), false);
        m.tevReg[r][1] = TevRA(comp(), comp(), false);
    }
    for (int k = 0; k < 4; ++k) {
        m.konst[k][0] = TevRA(static_cast<int>(Rn(256)), 255, true);
        m.konst[k][1] = TevRA(static_cast<int>(Rn(256)), static_cast<int>(Rn(256)), true);
    }
    for (int s = 0; s < 3; ++s) {
        // (colour: d c b a inputs from a small set, alpha likewise; scale / bias / clamp / register 0)
        const uint32_t ci[4] = {Rn(2) ? 0xFu : 8u, Rn(2) ? 8u : 0xAu, Rn(2) ? 0xEu : 0xFu, 0xFu};
        m.stage[s][0] = (ci[0] << 12) | (ci[1] << 8) | (ci[2] << 4) | ci[3] | (1u << 19);
        const uint32_t ai[4] = {Rn(2) ? 7u : 4u, Rn(2) ? 4u : 5u, 7u, 7u};
        m.stage[s][1] = (ai[0] << 13) | (ai[1] << 10) | (ai[2] << 7) | (ai[3] << 4) | (1u << 19);
    }
    for (int i = 0; i < 8; ++i) m.ksel[i] = 0xE338C4u | (i & 1 ? 0xAu : 0u) | (Rn(4) << 14);
    m.alpha = Rn(3) ? 0x770000u : (0x3F0000u | Rn(128));
    m.blend = Rn(3) ? 0x0034A1u : 0x0034A0u | (Rn(2) << 1);
    m.zmode = Rn(4) ? 0x17u : 0x07u;
    m.fog[0] = 0; m.fog[1] = 0x40000Fu; m.fog[2] = 1; m.fog[3] = 0; m.fog[4] = 0xFFFFFF; m.fog[5] = 0x156;
    m.matColor = Rnd() | 0xFFu;
    m.amb = Rnd() | 0xFFu;
    m.chan[0] = Rn(2) ? 0x701u : 0x702u; m.chan[1] = 0x202; m.chan[2] = 0x701; m.chan[3] = 0x400;
    // FEW_LIT=1: lit colour channels (lights 0-2, clamped diffuse, spot attenuation: the game's actor lights), so that GXV_VS_TRIM's light
    // facts reach the keys (the random light-memory writes below toggle them). No extra random numbers: unset, the scene is as before.
    if (std::getenv("FEW_LIT")) { m.chan[0] = (m.chan[0] & 1u) ? 0x71Fu : 0x71Eu; m.chan[2] = 0x71Fu; }
    for (int i = 0; i < 16; ++i) m.mtx[i] = F2u(Rf(-2, 2));
    for (int r = 0; r < 3; ++r) { m.dReg[r][0] = m.tevReg[r][0]; m.dReg[r][1] = m.tevReg[r][1]; }
    for (int i = 0; i < 12; ++i) m.dMtx[i] = F2u(Rf(-1, 1));
    m.dColor = m.matColor;
    m.animated = Rn(8) == 0;
    return m;
}
// The material's display list, in J3DMaterial::makeDisplayList_private's order (J3DTevBlock, J3DIndBlock, J3DPEBlock, gen mode,
// J3DTexGenBlock, J3DColorBlock, channel and texgen counts).
Bytes MaterialList(const Material& m) {
    Bytes b;
    for (int t = 0; t < m.texmaps; ++t) {
        const uint32_t ti = m.tex[t];
        b.BP(0x94 + t, g_tex[ti] >> 5);
        b.BP(0x88 + t, (g_texW[ti] - 1) | ((g_texH[ti] - 1) << 10) | (1u << 20)); // I8
        b.BP(0x80 + t, 0x000195u);
        b.BP(0x84 + t, 0);
    }
    b.BP(0x28, m.tevOrder);
    for (int t = 0; t < m.texgens; ++t) {
        b.Masked(0x30 + 2 * t, 0x03FFFF, g_texW[m.tex[t]] - 1);
        b.Masked(0x31 + 2 * t, 0x03FFFF, g_texH[m.tex[t]] - 1); // (the real lists mask once for both; the same writes)
    }
    for (int r = 0; r < 3; ++r) {
        b.BP(0xE2 + 2 * r, m.tevReg[r][0]);
        for (int k = 0; k < 3; ++k) b.BP(0xE3 + 2 * r, m.tevReg[r][1]); // (BG three times: the hardware's write delay)
    }
    for (int k = 0; k < 4; ++k) { b.BP(0xE0 + 2 * k, m.konst[k][0]); b.BP(0xE1 + 2 * k, m.konst[k][1]); }
    for (int s = 0; s < m.stages; ++s) {
        b.BP(0xC0 + 2 * s, m.stage[s][0]);
        b.BP(0xC1 + 2 * s, m.stage[s][1]);
        b.BP(0x10 + s, 0);
    }
    for (int i = 0; i < 8; ++i) b.BP(0xF6 + i, m.ksel[i]);
    if (m.ind) { b.BP(0x06, 0x200); b.BP(0x07, 0x100000); b.BP(0x08, 0x400000); b.BP(0x25, 0); }
    for (int i = 0; i < 3; ++i) { b.Masked(0x2E, 0x03FFFF, 0); b.BP(0x2F, 0); }
    b.BP(0x27, 0xFFFFC9);
    b.BP(0x0F, 2);
    for (int i = 0; i < 5; ++i) b.BP(0xEE + i, m.fog[i]);
    b.BP(0xE8, m.fog[5]);
    b.BP(0xF3, m.alpha);
    b.Masked(0x41, 0x00FFE7, m.blend);
    b.BP(0x40, m.zmode);
    b.Masked(0x43, 0x000040, 0x40);
    b.Masked(0x00, 0x07FC3F, static_cast<uint32_t>(m.texgens) | (1u << 4) | (static_cast<uint32_t>(m.stages - 1) << 10) | (m.ind ? 1u << 16 : 0u));
    if (m.texMtx) {
        b.XF(0x78, std::vector<uint32_t>(m.mtx, m.mtx + 8));
        b.XF(0x84, std::vector<uint32_t>(m.mtx + 8, m.mtx + 16));
    }
    std::vector<uint32_t> tg, pt;
    for (int t = 0; t < m.texgens; ++t) { tg.push_back(0x5280); pt.push_back(0x3D); }
    b.XF(0x1040, tg);
    b.XF(0x1050, pt);
    b.XF(0x100C, {m.matColor, m.matColor});
    b.XF(0x100A, {m.amb, 0});
    b.XF(0x100E, {m.chan[0], m.chan[1], m.chan[2], m.chan[3]});
    b.XF(0x1009, {1});
    b.XF(0x103F, {static_cast<uint32_t>(m.texgens)});
    return b;
}
// The differed list of the packet (J3DShapePacket's, made again from the animated values on every drawn turn).
Bytes DiffList(const Material& m) {
    Bytes b;
    for (int r = 0; r < 3; ++r) {
        b.BP(0xE2 + 2 * r, m.dReg[r][0]);
        for (int k = 0; k < 3; ++k) b.BP(0xE3 + 2 * r, m.dReg[r][1]);
    }
    b.XF(0x1D * 3 + 0, std::vector<uint32_t>(m.dMtx, m.dMtx + 12)); // (a texture matrix in position matrix memory, row 0x1D)
    b.XF(0x100C, {m.dColor});
    return b;
}
Bytes VtxFmt(int kind) {
    Bytes b;
    uint32_t vcd = 3u << 9 | 3u << 11; // POS, NRM index16
    if (kind == 1) vcd |= 1u;          // PNMTXIDX
    b.CP(0x50, vcd);
    b.CP(0x60, 3u); // TEX0 index16
    b.CP(0x70, 1u | (4u << 1) | (4u << 10) | (1u << 21) | (4u << 22));
    b.CP(0x80, 0);
    b.CP(0x90, 0);
    b.XF(0x1008, {1u | (1u << 2) | (1u << 4)});
    return b;
}
constexpr uint32_t kVerts = 200;
Bytes Geometry(int kind, uint32_t prims) {
    Bytes b;
    for (uint32_t p = 0; p < prims; ++p) {
        const uint32_t n = 4 + Rn(14);
        b.U8(0x98); // triangle strip, format 0
        b.U16(n);
        for (uint32_t i = 0; i < n; ++i) {
            if (kind == 1) b.U8(Rn(4) * 3);
            b.U16(Rn(kVerts));
            b.U16(Rn(kVerts));
            b.U16(Rn(kVerts));
        }
    }
    return b;
}

uint32_t g_pos = 0, g_nrm = 0, g_uv = 0, g_mtxArr = 0;

// ---- odd lists (FEW_ODD=0: none): state lists of random shape, to exercise every branch of the register-delta compiler and
// replay (GXV_SLDELTA): registers written again with the same or another value, behind masks, masks before TEV / texture registers or
// at the end, TEV halves outside 0..255, the texture units' LOD registers, XF registers, matrix indices through XF and CP, overlapping
// matrix-memory blocks, light memory, CP arrays and vertex formats, an unknown CP register, an indexed load, a PE-done write.
uint32_t OddVal(uint32_t reg, uint32_t salt) { return ((reg * 0x9E3779B1u) ^ (salt * 0x85EBCA6Bu)) & 0x00FFFFFFu; }
Bytes OddList() {
    static const uint8_t plain[] = {0x0F, 0x10, 0x11, 0x20, 0x21, 0x22, 0x25, 0x26, 0x27, 0x28, 0x30, 0x31, 0x32, 0x33, 0x40, 0x41, 0x42, 0x43,
                                    0x59, 0xC0, 0xC1, 0xC2, 0xC3, 0xEE, 0xEF, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF6, 0xF7, 0x06, 0x07, 0x08};
    static const uint8_t tex[] = {0x80, 0x81, 0x84, 0x85, 0x88, 0x94, 0xA0, 0xA4, 0xA8};
    static const uint16_t xfr[] = {0x1009, 0x100A, 0x100B, 0x100C, 0x100D, 0x100E, 0x100F, 0x1010, 0x1011, 0x1012, 0x1040, 0x1041, 0x1050, 0x1051, 0x103F, 0x101C};
    Bytes b;
    const uint32_t n = 4 + Rn(28);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t k = Rn(16);
        if (k < 4) {
            const uint32_t r = plain[Rn(sizeof plain)];
            uint32_t v = OddVal(r, Rn(2));
            if (r == 0x28) v &= 0x3C7C7u; // (TEV order: texture maps 0..3 or none)
            if (r == 0x41) v &= 0x00FFF7u;
            b.BP(r, v);
        } else if (k == 4) {
            const uint32_t r = plain[Rn(sizeof plain)];
            b.Masked(r, Rn(2) ? 0x00FF00u : 0x03FFFFu, OddVal(r, Rn(2)));
        } else if (k == 5 && Rn(4) == 0) {
            b.BP(0xFE, 0x000FFFu); // (a mask before a TEV or texture register: not compiled)
            b.BP(Rn(2) ? 0xE2 : 0x80, OddVal(0xE2, 0) & 0x7FF7FFu);
        } else if (k == 6) {
            const uint32_t r = tex[Rn(sizeof tex)];
            uint32_t v = OddVal(r, Rn(2));
            if (r == 0x94 || r == 0x88) v = r == 0x94 ? (g_tex[Rn(2)] >> 5) : ((g_texW[0] - 1) | ((g_texH[0] - 1) << 10) | (1u << 20));
            b.BP(r, v);
        } else if (k == 7 || k == 8) {
            const uint32_t r = 0xE0 + Rn(8);
            const int lo = static_cast<int>(Rn(3) == 0 ? Rn(2048) : Rn(256)), hi = static_cast<int>(Rn(3) == 0 ? Rn(2048) : Rn(256));
            b.BP(r, (static_cast<uint32_t>(lo) & 0x7FF) | ((static_cast<uint32_t>(hi) & 0x7FF) << 12) | (Rn(2) ? 0x800000u : 0u));
        } else if (k == 9) {
            const uint32_t a = xfr[Rn(sizeof xfr / sizeof xfr[0])], c = 1 + Rn(3);
            std::vector<uint32_t> w;
            for (uint32_t j = 0; j < c && a + j < 0x1058; ++j) w.push_back(a == 0x101C ? F2u(16777215.0f - static_cast<float>(Rn(2))) : (Rn(2) ? 0x701u : 0x202u) + j);
            b.XF(a, w);
        } else if (k == 10) {
            if (Rn(2)) b.XF(0x1018, {0x3CF3CF00u | Rn(3) * 3});
            else b.XF(0x1017, {0, 0x3CF3CF00u, 0x00F3CF3Cu});
        } else if (k == 11) {
            const uint32_t region = Rn(4);
            const uint32_t base = region == 0 ? 0x30 + Rn(0x10) : region == 1 ? 0x400 + Rn(0x20) : region == 2 ? 0x500 + Rn(0x30) : 0x600 + Rn(0x40);
            const uint32_t c = 1 + Rn(region == 3 ? 16 : 12);
            std::vector<uint32_t> w;
            for (uint32_t j = 0; j < c; ++j) w.push_back(F2u(static_cast<float>(Rn(3)) * 0.5f));
            b.XF(base, w);
        } else if (k == 12) {
            const uint32_t c = Rn(5);
            if (c == 0) b.CP(Rn(2) ? 0x30 : 0x40, Rn(2) ? 0u : 3u);
            else if (c == 1) b.CP(0xA0 + Rn(2), Rn(2) ? g_pos : g_nrm);
            else if (c == 2) b.CP(0xB0, 12);
            else if (c == 3) { const Bytes f = VtxFmt(static_cast<int>(Rn(2))); b.insert(b.end(), f.begin(), f.begin() + 6 * (1 + Rn(5))); }
            else if (Rn(4) == 0) b.CP(0x20, 1); // (a CP register the delta does not take)
        } else if (k == 13 && Rn(3) == 0) {
            const uint32_t c = Rn(3);
            if (c == 0) { b.U8(0x20); b.U16(Rn(16)); b.U16((11u << 12) | 0x30); } // (an indexed load: not compiled)
            else if (c == 1) b.BP(0x45, 0); // (PE done without the done bit: an effect register)
            else b.U8(0x44);
        }
    }
    if (Rn(16) == 0) b.BP(0xFE, 0x00FFFFu); // (a mask at the end: applies to the caller's next write)
    return b;
}
std::vector<List> g_odd;
bool g_oddOn = true;

struct Packet {
    int mat;
    List matList;
    List diff[2];
    int cur = 0;
    struct Shape { int kind; List geo; bool idxMtx; };
    std::vector<Shape> shapes;
    bool visible = true;
};
std::vector<Material> g_mats;
std::vector<Packet> g_pk;
List g_fmt[2];
// FEW_FX=<n> (default 0: none, and nothing else changes): n particle-like quads at the end of every picture, as the game's effects come:
// two blended materials (one depth-tested, one not), then per quad its position matrix, a TEV colour and a tiny display list of one
// quad (indexed position and texture coordinate, a direct colour: 27 bytes), so the dynamic-draw path (decode, CPU transform, fold,
// cover bounds, records) is measured too.
int g_fxCount = 0;
List g_fxMat[2], g_fxFmt;
std::vector<List> g_fxQuads;
// FEW_JPA=<n> (default 0: none, and nothing else changes): n particles per picture in the command order of JPAEmitterManager::draw,
// JPAResource::draw / setPTev / JPABaseShape::setGX and the per-particle functions (JPARegistPrmAlpha(Env), JPALoadCalcTexCrdMtxAnm,
// JPADrawBillboard / JPADrawRotBillboard): emitters of 1-40 particles, each with its immediate set-up (blend, z, alpha compare, TEV
// stage 0 colour / alpha inputs reading C0 / C1 and the texture, no colour channel, one texgen, its texture, the arrays jpa_pos / jpa_crd
// at the emitter's offsets), then per particle its TEV register 0 (and 1), for some emitters a 2x4 texture matrix (TEXMTX0), the position
// matrix (PNMTX0) and GXCallDisplayList(jpa_dl, 32): one quad, POS / TEX0 index8, s8 (VTXFMT0). Some emitters are depth-tested, some not
// (the cover bounds), some have the "clip on" flush primitive in front of every call (GX_MT_XF_FLUSH: an all-zero triangle strip).
int g_jpaCount = 0;
unsigned g_frame = 0;
List g_jpaDl, g_jpaPos, g_jpaCrd;
struct JpaEmitter { int n; int tex; bool zTest, env, texAnm, rot, flush, blendAdd; int posOff, crdOff; uint32_t prm, envc; int var; };
std::vector<JpaEmitter> g_jpaEm;
// FEW_FXMERGE=1 (default 0: the separate commands): the particles' calls as the game's GX library gathers them with GXV_FXMERGE=1
// (decomp/src/RVL_SDK/gx/GXDisplayList.c): per emitter loop one or more commands 0x58 (vgx_state.cpp FxMergeRun) holding the same
// TEV colour, XF and display-list call items, the flush primitive as a count on its call. The command buffers must be the same.
bool g_fxMerge = false;
// FEW_JPAVAR=1 (default 0: none, and nothing else changes): emitters of other kinds as well, so that every way of an item is taken:
// (var 1) the eight-vertex list of JPADrawRotation's jpa_dl_x; (2) a list with a BP write in front of the quad (CallList's way);
// (3) a 96-byte quad list (the geometry cache's way); (4) a post-transform matrix load (XF 0x500) per particle; (5) a 3x4 texture
// matrix with a texgen from the position (a projected texture: not transformed on the CPU); (6) GXSetTevColor of register 2 too;
// (7) a draw-only list made of two quads (not one draw: ProcessStream's way); (8) a command cut into several (the 8 KiB cap).
bool g_jpaVar = false;
List g_jpaDlX, g_jpaDlBp, g_jpaDlBig, g_jpaDl2;

void Build() {
    for (int t = 0; t < kTex; ++t) {
        g_texW[t] = 8u << Rn(3);
        g_texH[t] = 8u << Rn(3);
        const uint32_t bytes = g_texW[t] * g_texH[t];
        g_tex[t] = Alloc(bytes);
        for (uint32_t i = 0; i < bytes; ++i) g_ram[g_tex[t] + i] = static_cast<uint8_t>(Rnd());
        vgx::hle::NotifyGuestWrite(g_ram + g_tex[t], bytes);
    }
    g_pos = Alloc(kVerts * 12);
    g_nrm = Alloc(kVerts * 12);
    g_uv = Alloc(kVerts * 8);
    for (uint32_t i = 0; i < kVerts * 3; ++i) { PutLE(g_ram + g_pos + i * 4, F2u(Rf(-1, 1))); PutLE(g_ram + g_nrm + i * 4, F2u(Rf(-1, 1))); }
    for (uint32_t i = 0; i < kVerts * 2; ++i) PutLE(g_ram + g_uv + i * 4, F2u(Rf(0, 1)));
    vgx::hle::NotifyGuestWrite(g_ram + g_pos, kVerts * 12);
    vgx::hle::NotifyGuestWrite(g_ram + g_nrm, kVerts * 12);
    vgx::hle::NotifyGuestWrite(g_ram + g_uv, kVerts * 8);
    g_mtxArr = Alloc(16 * 48);
    for (uint32_t i = 0; i < 16 * 12; ++i) {
        const uint32_t k = i % 12;
        const float v = k == 3 ? Rf(-5, 5) : k == 7 ? Rf(-4, 4) : k == 11 ? Rf(-40, -6) : (k % 5) == 0 ? Rf(0.5f, 2) : Rf(-0.5f, 0.5f);
        PutLE(g_ram + g_mtxArr + i * 4, F2u(v));
    }
    vgx::hle::NotifyGuestWrite(g_ram + g_mtxArr, 16 * 48);
    for (int k = 0; k < 2; ++k) g_fmt[k] = NewList(VtxFmt(k), 0xC0 - Pad(VtxFmt(k).size()));
    for (int i = 0; i < 40; ++i) g_mats.push_back(NewMaterial());
    for (int i = 0; i < 12 && g_oddOn; ++i) g_odd.push_back(NewList(OddList(), 256));
    for (int p = 0; p < 110; ++p) {
        Packet k;
        k.mat = static_cast<int>(Rn(static_cast<uint32_t>(g_mats.size())));
        k.matList = NewList(MaterialList(g_mats[k.mat]));
        const Bytes d = DiffList(g_mats[k.mat]);
        k.diff[0] = NewList(d, 32, true);
        k.diff[1] = NewList(d, 32, true);
        const int n = 1 + static_cast<int>(Rn(3));
        for (int s = 0; s < n; ++s) {
            Packet::Shape sh;
            sh.kind = Rn(5) == 0 ? 1 : 0;
            sh.geo = NewList(Geometry(sh.kind, 6 + Rn(20)));
            sh.idxMtx = Rn(4) == 0;
            k.shapes.push_back(sh);
        }
        g_pk.push_back(k);
    }
    if (g_fxCount > 0) {
        for (int i = 0; i < 2; ++i) {
            Material m = NewMaterial();
            m.blend = 0x0034A1u;                // blend on
            m.zmode = i == 0 ? 0x07u : 0x00u;   // depth-tested without update; not depth-tested
            m.texMtx = false;
            g_mats.push_back(m);
            g_fxMat[i] = NewList(MaterialList(m));
        }
        Bytes v;
        v.CP(0x50, (2u << 9) | (1u << 13));  // POS index8, CLR0 direct
        v.CP(0x60, 2u);                      // TEX0 index8
        v.CP(0x71, 1u | (4u << 1) | (1u << 13) | (5u << 14) | (1u << 21) | (4u << 22)); // fmt 1: XYZ f32, RGBA8, ST f32
        v.CP(0x81, 0);
        v.CP(0x91, 0);
        v.XF(0x1008, {1u | (1u << 4)});
        g_fxFmt = NewList(v, 0xC0 - Pad(v.size()));
        for (int q = 0; q < 16; ++q) {
            Bytes b;
            b.U8(0x81); // quads, format 1
            b.U16(4);
            for (int k = 0; k < 4; ++k) { b.U8(Rn(kVerts)); b.U32(Rnd() | 0xFFu); b.U8(Rn(kVerts)); }
            g_fxQuads.push_back(NewList(b));
        }
    }
    if (g_jpaCount > 0) {
        Bytes dl;
        dl.U8(0x80); dl.U16(4);
        for (int k = 0; k < 4; ++k) { dl.U8(k); dl.U8(k); }
        g_jpaDl = NewList(dl);
        Bytes pos, crd;
        static const int8_t q[9][3] = {{0, 0, 0}, {50, 0, 0}, {50, -50, 0}, {0, -50, 0}, {-25, 0, 0}, {25, 0, 0}, {25, -50, 0}, {-25, -50, 0}, {-50, 0, 0}};
        for (int r = 0; r < 4; ++r)
            for (int i = 0; i < 9; ++i)
                for (int c = 0; c < 3; ++c) pos.U8(static_cast<uint8_t>(q[(i + r) % 9][c] + (c == 1 ? 25 * (r & 1) : 0)));
        g_jpaPos = NewList(pos);
        static const uint8_t t[16] = {0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 2, 0, 2, 1, 0, 1};
        for (uint8_t v : t) crd.U8(v);
        g_jpaCrd = NewList(crd);
        if (g_jpaVar) {
            Bytes x; // (jpa_dl_x: eight vertices, the second four from beyond the array's corners)
            x.U8(0x80); x.U16(8);
            for (int k = 0; k < 4; ++k) { x.U8(k); x.U8(k); }
            for (int k = 0; k < 4; ++k) { x.U8(0x48 + k); x.U8(k); }
            g_jpaDlX = NewList(x);
            Bytes b;
            b.BP(0xF3, 0x3F0000u);
            b.U8(0x80); b.U16(4);
            for (int k = 0; k < 4; ++k) { b.U8(k); b.U8(k); }
            g_jpaDlBp = NewList(b, 64 - Pad(b.size()) + 32);
            Bytes big;
            big.U8(0x80); big.U16(4);
            for (int k = 0; k < 4; ++k) { big.U8(k); big.U8(3 - k); }
            g_jpaDlBig = NewList(big, 96 - Pad(big.size()));
            Bytes two;
            for (int r = 0; r < 2; ++r) { two.U8(0x80); two.U16(4); for (int k = 0; k < 4; ++k) { two.U8((k + r) & 3); two.U8(k); } }
            g_jpaDl2 = NewList(two);
        }
        int left = g_jpaCount;
        while (left > 0) {
            JpaEmitter e;
            e.n = std::min(left, 1 + static_cast<int>(Rn(40)));
            left -= e.n;
            e.tex = static_cast<int>(Rn(kTex));
            e.zTest = Rn(3) != 0;
            e.env = Rn(2) == 0;
            e.texAnm = Rn(4) == 0;
            e.rot = Rn(3) == 0;
            e.flush = Rn(4) == 0;
            e.blendAdd = Rn(2) == 0;
            e.posOff = static_cast<int>(Rn(4)) * 27;
            e.crdOff = static_cast<int>(Rn(2)) * 8;
            e.prm = Rnd() | 0xFFu;
            e.envc = Rnd();
            e.var = g_jpaVar && Rn(2) == 0 ? 1 + static_cast<int>(Rn(8)) : 0;
            g_jpaEm.push_back(e);
        }
    }
}
// A particle loop's calls: as separate commands, or (FEW_FXMERGE=1) as the items of commands 0x58, written whenever the payload would
// pass `cap` bytes and at the loop's end (as GXDisplayList.c's gathering does).
struct JpaOut {
    Bytes& f;
    bool merged;
    uint32_t cap;
    Bytes items;
    JpaOut(Bytes& out, bool m, uint32_t c) : f(out), merged(m), cap(c) {}
    void Room(size_t n) { if (items.size() + n > cap) End(); }
    void Tev(uint32_t reg, uint32_t ra, uint32_t bg) {
        if (!merged) {
            f.BP(reg, ra);
            for (int k = 0; k < 3; ++k) f.BP(reg + 1, bg);
            return;
        }
        Room(9);
        items.U8(0x62);
        items.U32(reg << 24 | (ra & 0xFFFFFF));
        items.U32((reg + 1) << 24 | (bg & 0xFFFFFF));
    }
    void XF(uint32_t addr, const std::vector<uint32_t>& w) {
        if (!merged) { f.XF(addr, w); return; }
        Room(5 + w.size() * 4);
        items.U8(0x10); items.U16(static_cast<uint32_t>(w.size() - 1)); items.U16(addr);
        for (uint32_t x : w) items.U32(x);
    }
    void Call(uint32_t addr, uint32_t size, uint32_t flush) {
        if (!merged) {
            if (flush) { f.U8(0x98); f.U16(flush); for (uint32_t k = 0; k < flush * 2; ++k) f.U8(0); }
            f.Call(addr, size);
            return;
        }
        Room(10);
        items.U8(0x40); items.U32(addr); items.U32(size); items.U8(flush);
    }
    void End() {
        if (items.empty()) return;
        f.U8(0x58); f.U16(static_cast<uint32_t>(items.size()));
        f.insert(f.end(), items.begin(), items.end());
        items.clear();
    }
};
// One picture's particles (JPAEmitterManager::draw for one group).
void JpaFrame(Bytes& f, unsigned frame) {
    // (JPAEmitterManager::draw: TEV stage 0 ops, texture offsets, no culling, the vertex descriptor and formats, PNMTX0, no channels)
    f.BP(0xC0, (0xFu << 12) | (2u << 8) | (8u << 4) | 0xFu | (1u << 19));
    f.BP(0xC1, (7u << 13) | (4u << 10) | (1u << 7) | (7u << 4) | (1u << 19));
    f.CP(0x50, 2u << 9);   // POS index8
    f.CP(0x60, 2u);        // TEX0 index8
    f.CP(0x70, 1u | (3u << 1) | (1u << 21) | (3u << 22)); // fmt 0: XYZ s8, ST s8
    f.CP(0x80, 0);
    f.CP(0x90, 0);
    f.CP(0x71, 1u | (4u << 1) | (1u << 21) | (4u << 22)); // fmt 1: XYZ f32, ST f32
    f.XF(0x1008, {1u << 4});
    f.CP(0x30, 0 | (60u << 6));
    f.XF(0x1018, {0 | (60u << 6)});
    f.XF(0x1009, {0});
    for (const JpaEmitter& e : g_jpaEm) {
        // (JPABaseShape::setGX, JPAResource::setPTev, the texture load, the texgen)
        f.Masked(0x41, 0x00FFE7, e.blendAdd ? 0x0000A1u | (1u << 8) | (1u << 5) : 0x0034A1u);
        f.BP(0x40, e.zTest ? 0x07u : 0x00u);
        f.BP(0xF3, 0x3F0000u);
        f.BP(0xC0, (0xFu << 12) | ((e.env ? 4u : 2u) << 8) | (8u << 4) | (e.env ? 2u : 0xFu) | (1u << 19));
        f.BP(0xC1, (7u << 13) | (4u << 10) | (1u << 7) | (7u << 4) | (1u << 19));
        f.BP(0x10, 0);
        f.BP(0x11, 0);
        f.CP(0xA0, g_jpaPos.addr + static_cast<uint32_t>(e.posOff)); f.CP(0xB0, 3);
        f.CP(0xA4, g_jpaCrd.addr + static_cast<uint32_t>(e.crdOff)); f.CP(0xB4, 2);
        f.BP(0x28, 0x3C0u | (0x3C0u << 12));
        f.Masked(0x00, 0x07FC3F, 1u);
        const uint32_t ti = static_cast<uint32_t>(e.tex);
        f.BP(0x94, g_tex[ti] >> 5);
        f.BP(0x88, (g_texW[ti] - 1) | ((g_texH[ti] - 1) << 10) | (1u << 20));
        f.BP(0x80, 0x000195u);
        f.BP(0x84, 0);
        f.Masked(0x30, 0x03FFFF, g_texW[ti] - 1);
        f.Masked(0x31, 0x03FFFF, g_texH[ti] - 1);
        f.XF(0x1040, {0x5280});
        f.XF(0x1050, {0x3D});
        f.XF(0x103F, {1});
        f.CP(0x30, e.texAnm ? (30u << 6) : (60u << 6));
        f.XF(0x1018, {e.texAnm ? (30u << 6) : (60u << 6)});
        if (!e.texAnm) {
            // (JPARegistPrmEnv for the emitter)
            f.BP(0xE2, TevRA(static_cast<int>(e.prm >> 24), static_cast<int>(e.prm & 0xFF), false));
            for (int k = 0; k < 3; ++k) f.BP(0xE3, TevRA(static_cast<int>((e.prm >> 8) & 0xFF), static_cast<int>((e.prm >> 16) & 0xFF), false));
        }
        if (e.var == 5) { // (a projected texture: the texgen reads the position through TEXMTX0 3x4)
            f.XF(0x1040, {0x5000u | (1u << 1)});
            f.CP(0x30, 30u << 6);
            f.XF(0x1018, {30u << 6});
        }
        static const uint32_t capAll = std::getenv("FEW_FXCAP") ? static_cast<uint32_t>(std::atoi(std::getenv("FEW_FXCAP"))) : 8192u;
        JpaOut o(f, g_fxMerge, e.var == 8 ? 400u : capAll);
        for (int i = 0; i < e.n; ++i) {
            // (the particle's colour: its alpha fades with its age)
            const int a = static_cast<int>((frame * 7 + static_cast<unsigned>(i) * 13) & 255);
            o.Tev(0xE2, TevRA(static_cast<int>(e.prm >> 24), a, false), TevRA(static_cast<int>((e.prm >> 8) & 0xFF), static_cast<int>((e.prm >> 16) & 0xFF), false));
            if (e.env)
                o.Tev(0xE4, TevRA(static_cast<int>(e.envc >> 24), static_cast<int>(e.envc & 0xFF), false),
                      TevRA(static_cast<int>((e.envc >> 8) & 0xFF), static_cast<int>((e.envc >> 16) & 0xFF), false));
            if (e.var == 6) o.Tev(0xE6, TevRA(static_cast<int>(a / 2), 255, false), TevRA(static_cast<int>(i & 255), 7, false));
            if (e.texAnm) {
                const float age = static_cast<float>(i) * 0.25f + static_cast<float>(frame & 63) * 0.1f;
                o.XF(0x78, {F2u(1.0f), 0, F2u(0.5f + age * 0.01f), 0, 0, F2u(1.0f), F2u(age * 0.02f), 0});
            }
            if (e.var == 4) o.XF(0x500, {F2u(1.0f), 0, 0, F2u(static_cast<float>(i)), 0, F2u(1.0f), 0, 0, 0, 0, F2u(1.0f), 0});
            std::vector<uint32_t> w(12, 0);
            const float sx = Rf(0.5f, 3), sy = Rf(0.5f, 3);
            if (e.rot) {
                const float c = Rf(-1, 1), sn = Rf(-1, 1);
                w[0] = F2u(c * sx); w[1] = F2u(-sn * sy); w[4] = F2u(sn * sx); w[5] = F2u(c * sy);
            } else {
                w[0] = F2u(sx); w[5] = F2u(sy);
            }
            w[10] = F2u(1.0f);
            w[3] = F2u(Rf(-30, 30)); w[7] = F2u(Rf(-20, 20)); w[11] = F2u(Rf(-200, -20));
            if (e.var == 5) o.XF(0x78, {w[0], w[1], w[2], F2u(0.5f), w[4], w[5], w[6], F2u(0.5f), 0, 0, F2u(1.0f), 0});
            o.XF(0, w);
            const List& l = e.var == 1 ? g_jpaDlX : e.var == 2 ? g_jpaDlBp : e.var == 3 ? g_jpaDlBig : e.var == 7 ? g_jpaDl2 : g_jpaDl;
            o.Call(l.addr, l.size, e.flush ? 3u : 0u);
        }
        o.End();
        if (e.var == 5) {
            f.XF(0x1040, {0x5280});
            f.CP(0x30, 60u << 6);
            f.XF(0x1018, {60u << 6});
        }
    }
}

// Between pictures: the differed lists made again into their other buffer (same bytes, or the animated values), objects come and go.
void Mutate(unsigned frame) {
    for (Material& m : g_mats) {
        if (!m.animated) continue;
        m.dReg[0][0] = TevRA(static_cast<int>((frame * 3) & 255), 255, false);
        m.dColor = (m.dColor & ~0xFF00u) | ((frame & 0xFF) << 8);
    }
    for (Packet& k : g_pk) {
        k.cur ^= 1;
        Store(k.diff[k.cur].addr, DiffList(g_mats[k.mat]), k.diff[k.cur].size);
        if (Rn(60) == 0) k.visible = !k.visible;
    }
    if (Rn(3) == 0) {
        const size_t i = Rn(static_cast<uint32_t>(g_pk.size() - 1));
        std::swap(g_pk[i], g_pk[i + 1]);
    }
    for (List& o : g_odd) {
        if (Rn(20) == 0) {
            const Bytes b = OddList();
            if (Pad(b.size()) <= o.size) Store(o.addr, b, o.size);
        } else if (Rn(5) == 0) {
            std::vector<uint8_t> same(g_ram + o.addr, g_ram + o.addr + o.size);
            std::memcpy(g_ram + o.addr, same.data(), same.size());
            vgx::hle::NotifyGuestWrite(g_ram + o.addr, o.size); // (the same bytes written again)
        }
    }
    if (Rn(50) == 0) { // a material changed for good (and its list made again)
        Packet& k = g_pk[Rn(static_cast<uint32_t>(g_pk.size()))];
        g_mats[k.mat].tevReg[1][0] ^= 0x10;
        Store(k.matList.addr, MaterialList(g_mats[k.mat]), k.matList.size);
    }
}

Bytes Frame() {
    Bytes f;
    f.CP(0xAC, g_mtxArr);
    f.CP(0xBC, 48);
    f.XF(0x1020, {F2u(1.2f), 0, F2u(1.6f), 0, F2u(-1.0f), F2u(-0.2f), 0}); // (a perspective projection; the objects in front of the camera)
    int lastFmt = -1;
    size_t pi = 0;
    for (const Packet& k : g_pk) {
        ++pi;
        if (!k.visible) continue;
        if (g_oddOn && Rn(40) == 0) f.BP(0xFE, 0x00F0FFu); // (the material list's first write behind the caller's mask)
        f.Call(k.matList.addr, k.matList.size);
        if (!g_odd.empty() && Rn(6) == 0) {
            const List& o = g_odd[pi % g_odd.size()];
            f.Call(o.addr, o.size);
            f.Call(g_fmt[0].addr, g_fmt[0].size); // (it may have written the vertex format: the shapes' own again)
            lastFmt = 0;
            if (Rn(2)) f.Call(o.addr, o.size); // (and once more, from the state it left)
        }
        for (const Packet::Shape& sh : k.shapes) {
            f.Call(k.diff[k.cur].addr, k.diff[k.cur].size);
            if (sh.kind != lastFmt) { f.Call(g_fmt[sh.kind].addr, g_fmt[sh.kind].size); lastFmt = sh.kind; }
            if (!sh.idxMtx) {
                std::vector<uint32_t> w, n;
                for (int i = 0; i < 12; ++i) w.push_back(F2u((i % 5) == 0 ? Rf(0.5f, 2) : Rf(-0.5f, 0.5f)));
                w[3] = F2u(Rf(-5, 5)); w[7] = F2u(Rf(-4, 4)); w[11] = F2u(Rf(-40, -6));
                for (int i = 0; i < 9; ++i) n.push_back(F2u(Rf(-1, 1)));
                f.XF(0, w);
                f.XF(0x400, n);
            } else {
                f.U8(0x20); f.U16(Rn(16)); f.U16((11u << 12) | 0);
            }
            f.CP(0x30, 0);
            f.CP(0xA0, g_pos); f.CP(0xB0, 12);
            f.CP(0xA1, g_nrm); f.CP(0xB1, 12);
            f.CP(0xA4, g_uv); f.CP(0xB4, 8);
            f.Call(sh.geo.addr, sh.geo.size);
        }
    }
    for (int i = 0; i < g_fxCount; ++i) {
        if (i == 0 || i == g_fxCount / 2) {
            f.Call(g_fxMat[i == 0 ? 0 : 1].addr, g_fxMat[i == 0 ? 0 : 1].size);
            f.Call(g_fxFmt.addr, g_fxFmt.size);
            f.CP(0x30, 0);
            f.CP(0xA0, g_pos); f.CP(0xB0, 12);
            f.CP(0xA4, g_uv); f.CP(0xB4, 8);
        }
        std::vector<uint32_t> w;
        for (int k = 0; k < 12; ++k) w.push_back(F2u((k % 5) == 0 ? 1.0f : 0.0f));
        w[3] = F2u(Rf(-3, 3)); w[7] = F2u(Rf(-2, 2)); w[11] = F2u(Rf(-20, -8));
        f.XF(0, w);
        f.BP(0xE2, TevRA(static_cast<int>(Rn(256)), 255, false));
        f.BP(0xE3, TevRA(static_cast<int>(Rn(256)), static_cast<int>(Rn(256)), false));
        const List& q = g_fxQuads[static_cast<size_t>(i) % g_fxQuads.size()];
        f.Call(q.addr, q.size);
    }
    if (g_jpaCount > 0) JpaFrame(f, g_frame);
    return f;
}

bool g_quiet = false;
uint64_t g_h = 0;
void Mix(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) g_h = (g_h ^ b[i]) * 1099511628211ull;
}
void Observe(vgx::CmdBuf& b) {
    if (!g_quiet) {
        g_h = 14695981039346656037ull;
        for (const vgx::Cmd& c : b.cmds) { const uint32_t v[2] = {c.type, c.arg}; Mix(v, sizeof v); }
        for (const vgx::DrawRec& d : b.draws) {
            const uint32_t v[16] = {d.vsId, d.fsId, d.prim, d.texMask, d.cover, static_cast<uint32_t>(d.exX), static_cast<uint32_t>(d.exY),
                                    static_cast<uint32_t>(d.exW), static_cast<uint32_t>(d.exH), d.layoutMask, d.vbuf, d.vtxOffset, d.vtxCount, d.idxOffset,
                                    d.idxCount, d.vtxFirst};
            Mix(v, sizeof v);
            Mix(d.ublock, sizeof d.ublock);
            Mix(&d.rs, sizeof d.rs);
            Mix(d.tex, sizeof d.tex);
        }
        Mix(b.vtx.data(), b.vtx.size());
        Mix(b.idx.data(), b.idx.size() * 2);
        Mix(b.uni.data(), b.uni.size() * 4);
        for (const vgx::BufUpload& u : b.bufUploads) { Mix(u.vtx.data(), u.vtx.size()); Mix(u.idx.data(), u.idx.size() * 2); }
        for (const vgx::TexUpload& u : b.uploads) for (const auto& l : u.levels) Mix(l.data(), l.size());
        std::printf("frame %u: %016llx  %zu cmds %zu draws %zu vtx %zu idx %zu uni %zu uploads\n", g_frame, static_cast<unsigned long long>(g_h), b.cmds.size(),
                    b.draws.size(), b.vtx.size(), b.idx.size(), b.uni.size(), b.bufUploads.size());
    }
    vgx::backend::Execute(b);
}
} // namespace

namespace vgx { void SlDeltaCounters(uint64_t out[5]) __attribute__((weak)); } // (weak: the fixture also builds against sources without it)
namespace vgx { namespace split { void FxJoinCounters(uint64_t out[2]) __attribute__((weak)); } } // (GXV_FXJOIN; weak, as above)
namespace vgx { void FxMergeCounters(uint64_t out[6]) __attribute__((weak)); } // (GXV_FXMERGE; weak, as above)

int main(int argc, char** argv) {
    const unsigned frames = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 60;
    if (argc > 2) g_rng ^= static_cast<uint64_t>(std::atoll(argv[2])) * 0x9E3779B97F4A7C15ull;
    g_quiet = std::getenv("FEW_PROFILE") != nullptr;
    g_oddOn = !(std::getenv("FEW_ODD") && std::atoi(std::getenv("FEW_ODD")) == 0);
    g_fxCount = std::getenv("FEW_FX") ? std::atoi(std::getenv("FEW_FX")) : 0;
    g_jpaCount = std::getenv("FEW_JPA") ? std::atoi(std::getenv("FEW_JPA")) : 0;
    g_fxMerge = std::getenv("FEW_FXMERGE") && std::atoi(std::getenv("FEW_FXMERGE")) == 1;
    g_jpaVar = std::getenv("FEW_JPAVAR") && std::atoi(std::getenv("FEW_JPAVAR")) == 1;
    const size_t trashBytes = std::getenv("FEW_TRASH") ? static_cast<size_t>(std::atoi(std::getenv("FEW_TRASH"))) << 10 : 0;
    // (the PS Vita player's defaults that change the front end's work: vita/vgx-game/entry.cpp; the environment overrides them)
    for (const char* kv : {"GXV_FIX_TEVIN=1", "GXV_FS_HALF=1", "GXV_FIX_LODBIAS=1", "GXV_FOGVTX=2", "GXV_RIGIDMERGE=1", "GXV_STATICCULL=1", "GXV_SOFTXFORM=32",
                           "GXV_TEXBAKE=1", "GXV_PALROWS=1", "GXV_LTMERGE=1", "GXV_FIX_DSTA=1", "GXV_FIX_INDPREV=1", "GXV_FIX_LOGICOP=1", "GXV_FIX_TEXOFF=1",
                           "GXV_TEVWRAP=2", "GXV_DSTALPHA_ZERO=1", "GXV_NODISCARD=1"}) {
        std::string s(kv);
        const size_t eq = s.find('=');
        setenv(s.substr(0, eq).c_str(), s.substr(eq + 1).c_str(), 0);
    }
    vgx::hle::RegisterGuestRam(g_ram, kRam);
    vgx::GuestHooks hooks{};
    hooks.resolve = Resolve;
    vgx::SetGuestHooks(hooks);
    vgx::ResetState();
    vgx::SetFrameSink(&Observe);
    vgx::g.efbW = 640;
    vgx::g.efbH = 456;
    vgx::QueueSetTarget();
    Build();
    uint64_t bytes = 0;
    for (g_frame = 1; g_frame <= frames; ++g_frame) {
        Bytes f = Frame();
        size_t rd = 0, pub = 0;
        // (FEW_FXMERGE set, 0 or 1: the pieces' sizes from a sequence of their own, so that a stream of another length leaves the scene's
        // random values as they were)
        static const bool ownPieces = std::getenv("FEW_FXMERGE") != nullptr;
        static uint64_t pieceRng = 0x9E3779B97F4A7C15ull;
        auto pieceRn = [](uint32_t n) {
            if (!ownPieces) return Rn(n);
            pieceRng ^= pieceRng << 13;
            pieceRng ^= pieceRng >> 7;
            pieceRng ^= pieceRng << 17;
            return static_cast<uint32_t>(pieceRng >> 11) % n;
        };
        while (rd < f.size()) { // (as the player gets it: in pieces)
            pub = std::min(f.size(), pub + 2048 + pieceRn(4096));
            rd += vgx::ProcessStream(f.data() + rd, static_cast<uint32_t>(pub - rd), false);
            if (pub == f.size() && rd < f.size()) {
                const uint32_t n = vgx::ProcessStream(f.data() + rd, static_cast<uint32_t>(f.size() - rd), false);
                if (!n) { std::fprintf(stderr, "fixture: stream stuck\n"); return 2; }
                rd += n;
            }
        }
        bytes += f.size();
        vgx::g.dispCopySrc = vgx::EfbRect{0, 0, 640, 456};
        vgx::QueueCopyDisp(true);
        vgx::FrontPresent(false, nullptr);
        Mutate(g_frame);
        if (trashBytes) {
            static std::vector<uint8_t> trash(trashBytes);
            for (size_t i = 0; i < trash.size(); i += 32) trash[i] = static_cast<uint8_t>(trash[i] + g_frame);
        }
    }
    uint64_t sl[5] = {}, sd[5] = {}, sc[3] = {};
    vgx::StaticCullCounters(sc);
    std::fprintf(stderr, "fe-work fixture: static runs tested %llu, culled %llu\n", static_cast<unsigned long long>(sc[0]), static_cast<unsigned long long>(sc[1]));
    if (&vgx::split::FxJoinCounters) {
        uint64_t fj[2] = {};
        vgx::split::FxJoinCounters(fj);
        std::fprintf(stderr, "fe-work fixture: job runs (GXV_FXJOIN): %llu jobs in %llu runs\n", static_cast<unsigned long long>(fj[0]), static_cast<unsigned long long>(fj[1]));
    }
    if (&vgx::FxMergeCounters) {
        uint64_t fx[6] = {};
        vgx::FxMergeCounters(fx);
        std::fprintf(stderr, "fe-work fixture: particle commands (GXV_FXMERGE): %llu commands, %llu items, %llu list calls, %llu short, %llu decodes reused, %llu through CallList\n",
                     static_cast<unsigned long long>(fx[0]), static_cast<unsigned long long>(fx[1]), static_cast<unsigned long long>(fx[2]),
                     static_cast<unsigned long long>(fx[3]), static_cast<unsigned long long>(fx[4]), static_cast<unsigned long long>(fx[5]));
    }
    vgx::StateListCounters(sl);
    if (&vgx::SlDeltaCounters) vgx::SlDeltaCounters(sd);
    std::fprintf(stderr, "fe-work fixture: %u frames, %llu stream bytes; state lists: %llu replays, %llu parses; deltas %llu, checked %llu, mismatching %llu; lists compiled %llu, refused %llu\n", frames,
                 static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(sl[0]), static_cast<unsigned long long>(sl[1]),
                 static_cast<unsigned long long>(sd[0]), static_cast<unsigned long long>(sd[1]), static_cast<unsigned long long>(sd[2]),
                 static_cast<unsigned long long>(sd[3]), static_cast<unsigned long long>(sd[4]));
    return 0;
}
