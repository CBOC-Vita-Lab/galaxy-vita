// The game-side cuts of the J3D state lists (GXV_J3D_DIFFKEEP, GXV_J3D_MATMERGE, GXV_J3D_DIFFSKIP: decomp J3DPacket.cpp,
// Game/System/DrawBuffer.cpp, J3DPortDL.hpp) on a scene shaped as Galaxy's draw buffers send it, through the real front end
// (tools/test_vgx_j3d_lists.sh).
//
// The stream is DrawBufferShapeDrawer::draw's, per drawer (a material of a model, for every actor of the model): the material's list
// (tests/vgx_fe_work.cpp's J3D-shaped generator: J3DMaterial::makeDisplayList_private's order and register sets), the first shape's
// vertex-format list when it is not the last one sent and its matrix index (J3DShape::loadPreDrawSetting), then per packet a light
// (some packets: XF light memory and a channel colour, as LightFunction::loadActorLightInfo), the packet's differed list (TEV colours
// and konst colours with the type bit, fog, a texture matrix in texture memory, the material colour: J3DMaterial::diff into
// J3DShapePacket's double buffer) and the shape (J3DShape::drawFast: array bases, matrix index, position / normal matrices, a texture
// matrix for some, the geometry list). Instances of a model mostly share their differed list's values (a stopped animation, fog),
// some have their own, some are animated; packets are hidden and shown, actors come and go.
//
// The cuts are made here by the fixture's own copy of the game's decisions (the predicates themselves are the game's header,
// J3DPortDL.hpp). Every mode must give the front end's command buffers of the base mode (J3D as before: every differed list made
// again into its other buffer on every picture, every list sent) frame for frame.
//   vgx_j3d_lists [frames] [seed]            prints a hash of every command buffer and, on stderr, the list calls per picture
//   JL_MODE=base|keep|merge|skip|all         the cuts made (default all)
//   JL_PROFILE=1                             no hashing or printing (callgrind: the player's instructions)
//   JL_BROKEN=1                              a wrong cut (a differed list left out after a shape's texture matrix): must change the buffers
#include "vgx_state.h"
#include "vgx_fesplit.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"

#include "JSystem/J3DGraphBase/J3DPortDL.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" int64_t port_host_time_ns() { static std::atomic<int64_t> n{1}; return n.fetch_add(1000); }
extern "C" int64_t port_thread_cpu_us() { static std::atomic<int64_t> n{1}; return n.fetch_add(1); }
extern "C" void port_log_summary(const char*, ...) {}
extern "C" void port_log(const char*, ...) {}

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
// (the scene's own random numbers, apart from the stream's: every mode builds and changes the same scene)
uint64_t g_srng = 0x9E3779B97F4A7C15ull;
uint32_t Sn(uint32_t n) {
    g_srng ^= g_srng << 13;
    g_srng ^= g_srng >> 7;
    g_srng ^= g_srng << 17;
    return static_cast<uint32_t>(g_srng >> 11) % n;
}

constexpr uint32_t kRam = 16u << 20;
alignas(4096) uint8_t g_ram[kRam];
uint32_t g_brk = 0x1000;
uint32_t Alloc(uint32_t bytes) {
    const uint32_t a = g_brk;
    g_brk += (bytes + 31u) & ~31u;
    if (g_brk > (8u << 20)) { std::fprintf(stderr, "fixture: guest RAM exhausted\n"); std::exit(2); }
    return a;
}
uint32_t g_brkDyn = 8u << 20; // (the lists made at run time: differed lists, merged lists)
uint32_t AllocDyn(uint32_t bytes) {
    const uint32_t a = g_brkDyn;
    g_brkDyn += (bytes + 31u) & ~31u;
    if (g_brkDyn > kRam) { std::fprintf(stderr, "fixture: guest RAM exhausted\n"); std::exit(2); }
    return a;
}
const uint8_t* Resolve(uint32_t address, uint32_t* avail) {
    *avail = 0;
    address &= 0x3FFFFFFFu;
    if (address == 0 || address >= kRam) return nullptr;
    *avail = kRam - address;
    return g_ram + address;
}

void Put32(uint8_t* p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i)); }
void PutLE(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
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
    void Pad32() { while (size() & 31) U8(0); }
};
uint32_t Pad(size_t n) { return static_cast<uint32_t>((n + 31) & ~size_t(31)); }

// ---- textures: I8 images in guest RAM
constexpr int kTex = 24;
uint32_t g_tex[kTex], g_texW[kTex], g_texH[kTex];

// ---- a material (tests/vgx_fe_work.cpp's)
struct Material {
    int texmaps = 1, stages = 1, texgens = 1;
    bool ind = false, texMtx = false;
    uint32_t tex[2] = {}, tevOrder = 0, tevReg[3][2] = {}, konst[4][2] = {}, stage[3][2] = {}, ksel[8] = {}, alpha = 0, blend = 0, zmode = 0,
             fog[6] = {}, matColor = 0, amb = 0, chan[4] = {};
    uint32_t mtx[16] = {};
};
uint32_t Tc(int v) { return static_cast<uint32_t>(v) & 0x7FF; }
uint32_t TevRA(int r, int a, bool k) { return Tc(r) | (Tc(a) << 12) | (k ? 0x800000u : 0u); }
Material NewMaterial() {
    Material m;
    m.texmaps = 1 + (Sn(3) == 0);
    m.texgens = m.texmaps;
    m.stages = 1 + static_cast<int>(Sn(3));
    m.ind = Sn(4) == 0;
    m.texMtx = Sn(3) == 0;
    for (int t = 0; t < m.texmaps; ++t) m.tex[t] = Sn(kTex);
    m.tevOrder = 0x049040u | (Sn(2) << 3);
    for (int r = 0; r < 3; ++r) { m.tevReg[r][0] = TevRA(static_cast<int>(Sn(256)), static_cast<int>(Sn(256)), false); m.tevReg[r][1] = TevRA(static_cast<int>(Sn(256)), static_cast<int>(Sn(256)), false); }
    for (int k = 0; k < 4; ++k) { m.konst[k][0] = TevRA(static_cast<int>(Sn(256)), 255, true); m.konst[k][1] = TevRA(static_cast<int>(Sn(256)), static_cast<int>(Sn(256)), true); }
    for (int s = 0; s < 3; ++s) {
        const uint32_t ci[4] = {Sn(2) ? 0xFu : 8u, Sn(2) ? 8u : 0xAu, Sn(2) ? 0xEu : 0xFu, 0xFu};
        m.stage[s][0] = (ci[0] << 12) | (ci[1] << 8) | (ci[2] << 4) | ci[3] | (1u << 19);
        const uint32_t ai[4] = {Sn(2) ? 7u : 4u, Sn(2) ? 4u : 5u, 7u, 7u};
        m.stage[s][1] = (ai[0] << 13) | (ai[1] << 10) | (ai[2] << 7) | (ai[3] << 4) | (1u << 19);
    }
    for (int i = 0; i < 8; ++i) m.ksel[i] = 0xE338C4u | (i & 1 ? 0xAu : 0u) | (Sn(4) << 14);
    m.alpha = Sn(3) ? 0x770000u : (0x3F0000u | Sn(128));
    m.blend = Sn(3) ? 0x0034A1u : 0x0034A0u | (Sn(2) << 1);
    m.zmode = Sn(4) ? 0x17u : 0x07u;
    m.fog[0] = 0; m.fog[1] = 0x40000Fu; m.fog[2] = 1; m.fog[3] = 0; m.fog[4] = 0xFFFFFF; m.fog[5] = 0x156;
    m.matColor = (Sn(1u << 24) << 8) | 0xFFu;
    m.amb = (Sn(1u << 24) << 8) | 0xFFu;
    m.chan[0] = Sn(2) ? 0x701u : 0x702u; m.chan[1] = 0x202; m.chan[2] = 0x701; m.chan[3] = 0x400;
    for (int i = 0; i < 16; ++i) m.mtx[i] = F2u(static_cast<float>(static_cast<int>(Sn(400)) - 200) / 100.0f);
    return m;
}
Bytes MaterialList(const Material& m) {
    Bytes b;
    for (int t = 0; t < m.texmaps; ++t) {
        const uint32_t ti = m.tex[t];
        b.BP(0x94 + t, g_tex[ti] >> 5);
        b.BP(0x88 + t, (g_texW[ti] - 1) | ((g_texH[ti] - 1) << 10) | (1u << 20));
        b.BP(0x80 + t, 0x000195u);
        b.BP(0x84 + t, 0);
    }
    b.BP(0x28, m.tevOrder);
    for (int t = 0; t < m.texgens; ++t) { b.Masked(0x30 + 2 * t, 0x03FFFF, g_texW[m.tex[t]] - 1); b.Masked(0x31 + 2 * t, 0x03FFFF, g_texH[m.tex[t]] - 1); }
    for (int r = 0; r < 3; ++r) { b.BP(0xE2 + 2 * r, m.tevReg[r][0]); for (int k = 0; k < 3; ++k) b.BP(0xE3 + 2 * r, m.tevReg[r][1]); }
    for (int k = 0; k < 4; ++k) { b.BP(0xE0 + 2 * k, m.konst[k][0]); b.BP(0xE1 + 2 * k, m.konst[k][1]); }
    for (int s = 0; s < m.stages; ++s) { b.BP(0xC0 + 2 * s, m.stage[s][0]); b.BP(0xC1 + 2 * s, m.stage[s][1]); b.BP(0x10 + s, 0); }
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
    if (m.texMtx) { b.XF(0x78, std::vector<uint32_t>(m.mtx, m.mtx + 8)); b.XF(0x84, std::vector<uint32_t>(m.mtx + 8, m.mtx + 16)); }
    std::vector<uint32_t> tg, pt;
    for (int t = 0; t < m.texgens; ++t) { tg.push_back(0x5280); pt.push_back(0x3D); }
    b.XF(0x1040, tg);
    b.XF(0x1050, pt);
    b.XF(0x100C, {m.matColor, m.matColor});
    b.XF(0x100A, {m.amb, 0});
    b.XF(0x100E, {m.chan[0], m.chan[1], m.chan[2], m.chan[3]});
    b.XF(0x1009, {1});
    b.XF(0x103F, {static_cast<uint32_t>(m.texgens)});
    b.Pad32(); // (GDPadCurr32)
    return b;
}
// The values a differed list carries (J3DMaterial::diff: J3DTevBlock::diff, J3DPEBlock fog, J3DTexGenBlock texture matrices,
// J3DColorBlock material colour).
struct DiffVals {
    uint32_t reg[3][2] = {}, konst[2] = {}, fog[4] = {}, color = 0, mtx[12] = {};
    bool hasFog = false, hasMtx = false;
};
Bytes DiffList(const DiffVals& d) {
    Bytes b;
    for (int r = 0; r < 3; ++r) { b.BP(0xE2 + 2 * r, d.reg[r][0]); for (int k = 0; k < 3; ++k) b.BP(0xE3 + 2 * r, d.reg[r][1]); }
    b.BP(0xE0, d.konst[0]);
    b.BP(0xE1, d.konst[1]);
    if (d.hasFog) for (int i = 0; i < 4; ++i) b.BP(0xEE + i, d.fog[i]);
    if (d.hasMtx) b.XF(0x78, std::vector<uint32_t>(d.mtx, d.mtx + 12)); // (GX_TEXMTX0)
    b.XF(0x100C, {d.color});
    b.Pad32();
    return b;
}
Bytes VtxFmt(int kind) {
    Bytes b;
    uint32_t vcd = 3u << 9 | 3u << 11;
    if (kind == 1) vcd |= 1u;
    b.CP(0x50, vcd);
    b.CP(0x60, 3u);
    b.CP(0x70, 1u | (4u << 1) | (4u << 10) | (1u << 21) | (4u << 22));
    b.CP(0x80, 0);
    b.CP(0x90, 0);
    b.XF(0x1008, {1u | (1u << 2) | (1u << 4)});
    b.Pad32();
    return b;
}
constexpr uint32_t kVerts = 200;
Bytes Geometry(int kind, uint32_t prims) {
    Bytes b;
    for (uint32_t p = 0; p < prims; ++p) {
        const uint32_t n = 4 + Sn(14);
        b.U8(0x98);
        b.U16(n);
        for (uint32_t i = 0; i < n; ++i) {
            if (kind == 1) b.U8(Sn(4) * 3);
            b.U16(Sn(kVerts));
            b.U16(Sn(kVerts));
            b.U16(Sn(kVerts));
        }
    }
    b.Pad32();
    return b;
}
uint32_t g_pos = 0, g_nrm = 0, g_uv = 0, g_mtxArr = 0;

struct List { uint32_t addr = 0, size = 0; };
List NewList(const Bytes& b) {
    List l{Alloc(Pad(b.size())), Pad(b.size())};
    std::memcpy(g_ram + l.addr, b.data(), b.size());
    vgx::hle::NotifyGuestWrite(g_ram + l.addr, l.size);
    return l;
}

// ---- the scene
// J3DDisplayListObj of a differed list: two buffers, the current one first.
struct DiffObj {
    uint32_t buf[2] = {}, maxSize = 0, size = 0;
};
struct Instance {
    DiffVals own;          // (own values, when not the model's shared ones)
    bool shared = true;    // the model's shared values (a stopped animation, fog)
    bool animated = false; // changed on every picture
    bool hidden = false;
    bool light = false;    // a light before its differed list
    bool texMtxShape = false; // the shape loads a texture matrix (J3DDifferedTexMtx)
    DiffObj diff;
    float m[12];
};
struct Drawer {
    int mat = 0;
    List matList;
    int fmt = 0;
    List geo;
    DiffVals shared;
    std::vector<Instance> inst;
    // the merged list (DrawBufferShapeDrawer's)
    uint32_t merged = 0, mergedCap = 0, mergedSize = 0, mergedMat = 0, mergedDiff = 0, lastCall = 0, quietUntil = 0, changes = 0;
};
std::vector<Material> g_mats;
std::vector<Drawer> g_dr;
List g_fmt[2];

DiffVals NewDiffVals() {
    DiffVals d;
    for (int r = 0; r < 3; ++r) { d.reg[r][0] = TevRA(static_cast<int>(Sn(256)), static_cast<int>(Sn(256)), false); d.reg[r][1] = TevRA(static_cast<int>(Sn(256)), static_cast<int>(Sn(256)), false); }
    d.konst[0] = TevRA(static_cast<int>(Sn(256)), 255, true);
    d.konst[1] = TevRA(static_cast<int>(Sn(256)), static_cast<int>(Sn(256)), true);
    d.hasFog = Sn(2) == 0;
    d.fog[0] = Sn(1u << 24); d.fog[1] = 0x40000Fu; d.fog[2] = Sn(1u << 24); d.fog[3] = 0;
    d.hasMtx = Sn(3) == 0;
    for (int i = 0; i < 12; ++i) d.mtx[i] = F2u(static_cast<float>(static_cast<int>(Sn(200)) - 100) / 100.0f);
    d.color = (Sn(1u << 24) << 8) | 0xFFu;
    return d;
}
const DiffVals& ValsOf(const Drawer& d, const Instance& i) { return i.shared ? d.shared : i.own; }

void Build() {
    for (int t = 0; t < kTex; ++t) {
        g_texW[t] = 8u << Sn(3);
        g_texH[t] = 8u << Sn(3);
        const uint32_t bytes = g_texW[t] * g_texH[t];
        g_tex[t] = Alloc(bytes);
        for (uint32_t i = 0; i < bytes; ++i) g_ram[g_tex[t] + i] = static_cast<uint8_t>(Sn(256));
        vgx::hle::NotifyGuestWrite(g_ram + g_tex[t], bytes);
    }
    g_pos = Alloc(kVerts * 12);
    g_nrm = Alloc(kVerts * 12);
    g_uv = Alloc(kVerts * 8);
    for (uint32_t i = 0; i < kVerts * 3; ++i) { PutLE(g_ram + g_pos + i * 4, F2u(static_cast<float>(static_cast<int>(Sn(200)) - 100) / 100.0f)); PutLE(g_ram + g_nrm + i * 4, F2u(static_cast<float>(static_cast<int>(Sn(200)) - 100) / 100.0f)); }
    for (uint32_t i = 0; i < kVerts * 2; ++i) PutLE(g_ram + g_uv + i * 4, F2u(static_cast<float>(Sn(100)) / 100.0f));
    vgx::hle::NotifyGuestWrite(g_ram + g_pos, kVerts * 12);
    vgx::hle::NotifyGuestWrite(g_ram + g_nrm, kVerts * 12);
    vgx::hle::NotifyGuestWrite(g_ram + g_uv, kVerts * 8);
    for (int k = 0; k < 2; ++k) g_fmt[k] = NewList(VtxFmt(k));
    for (int i = 0; i < 48; ++i) g_mats.push_back(NewMaterial());
    // (Galaxy's Observatory: map models of one actor and a shape per material, and actors of a few models, several of each)
    for (int i = 0; i < 90; ++i) {
        Drawer d;
        d.mat = i % static_cast<int>(g_mats.size());
        d.matList = NewList(MaterialList(g_mats[d.mat]));
        d.fmt = Sn(5) == 0 ? 1 : 0;
        d.geo = NewList(Geometry(d.fmt, 4 + Sn(14)));
        d.shared = NewDiffVals();
        const uint32_t n = Sn(10) < 7 ? 1 : 2 + Sn(6);
        for (uint32_t k = 0; k < n; ++k) {
            Instance in;
            in.shared = Sn(4) != 0;
            if (!in.shared) in.own = NewDiffVals();
            in.animated = Sn(10) == 0;
            if (in.animated) { in.shared = false; in.own = d.shared; }
            in.light = (k == 0) ? Sn(6) == 0 : Sn(12) == 0;
            in.texMtxShape = Sn(12) == 0;
            in.hidden = Sn(30) == 0;
            for (int j = 0; j < 12; ++j) in.m[j] = (j % 5) == 0 ? 1.0f : 0.0f;
            in.m[3] = static_cast<float>(static_cast<int>(Sn(10)) - 5);
            in.m[7] = static_cast<float>(static_cast<int>(Sn(8)) - 4);
            in.m[11] = -10.0f - static_cast<float>(Sn(30));
            const Bytes b = DiffList(ValsOf(d, in));
            in.diff.maxSize = Pad(b.size() + 64);
            in.diff.buf[0] = AllocDyn(in.diff.maxSize);
            in.diff.buf[1] = AllocDyn(in.diff.maxSize);
            d.inst.push_back(in);
        }
        if (i == 0) {
            // (one model for certain: four instances in one state with a texture matrix, the second's shape loading its own one, the
            // case where leaving a differed list out would be wrong: JL_BROKEN must show it)
            d.shared.hasMtx = true;
            d.inst.resize(1);
            d.inst[0].shared = true;
            d.inst[0].animated = false;
            d.inst[0].light = false;
            d.inst[0].hidden = false;
            d.inst[0].texMtxShape = false;
            const Bytes b = DiffList(d.shared);
            d.inst[0].diff.maxSize = Pad(b.size() + 64);
            d.inst[0].diff.buf[0] = AllocDyn(d.inst[0].diff.maxSize);
            d.inst[0].diff.buf[1] = AllocDyn(d.inst[0].diff.maxSize);
            for (int k = 1; k < 4; ++k) {
                Instance in = d.inst[0];
                in.texMtxShape = k == 1;
                in.diff.buf[0] = AllocDyn(in.diff.maxSize);
                in.diff.buf[1] = AllocDyn(in.diff.maxSize);
                in.m[3] = static_cast<float>(k);
                d.inst.push_back(in);
            }
        }
        g_dr.push_back(d);
    }
}

// ---- the modes
bool g_keep = false, g_merge = false, g_skip = false;
bool g_broken = false; // (JL_BROKEN=1: a differed list is also left out after a shape that loaded a texture matrix, which is wrong)
uint32_t g_picture = 1;
struct Counts { uint64_t matCalls = 0, merged = 0, diffSent = 0, diffSkipped = 0, diffMade = 0, diffKept = 0, mergeWrites = 0, listCalls = 0, brokenSkips = 0, mergeRefused = 0; } g_cnt;

// J3DMaterial::diff into the differed list's buffers: as before (always into the other buffer, which becomes the current one), or
// with GXV_J3D_DIFFKEEP (J3DDisplayListObj::endDiffDL: only when the bytes differ).
void MakeDiff(DiffObj& o, const Bytes& b) {
    ++g_cnt.diffMade;
    const uint32_t size = Pad(b.size());
    if (g_keep && size == o.size && std::memcmp(g_ram + o.buf[0], b.data(), size) == 0) { ++g_cnt.diffKept; return; }
    std::swap(o.buf[0], o.buf[1]);
    std::memset(g_ram + o.buf[0], 0, o.maxSize);
    std::memcpy(g_ram + o.buf[0], b.data(), b.size());
    o.size = size;
    vgx::hle::NotifyGuestWrite(g_ram + o.buf[0], o.maxSize);
}

// Between pictures: the game's turn (animations, the lists of every drawn model made again; actors hidden, shown, added, removed).
void Turn(unsigned frame) {
    for (Drawer& d : g_dr) {
        for (Instance& in : d.inst) {
            if (in.animated) {
                in.own.reg[0][0] = TevRA(static_cast<int>((frame * 3) & 255), 255, false);
                in.own.color = (in.own.color & ~0xFF00u) | ((frame & 0xFF) << 8);
            }
            if ((in.hidden ? Sn(10) == 0 : Sn(300) == 0) && &d != &g_dr[0]) in.hidden = !in.hidden; // (about 3 % hidden)
            MakeDiff(in.diff, DiffList(ValsOf(d, in)));
        }
        if (d.inst.size() > 1 && Sn(100) == 0 && &d != &g_dr[0]) std::swap(d.inst[0], d.inst[1 + Sn(static_cast<uint32_t>(d.inst.size() - 1))]); // (light sort)
        if (Sn(300) == 0) d.shared.color ^= 0x100; // (a model's shared state changes)
    }
}

// DrawBufferShapeDrawer::callMergedList.
bool CallMerged(Bytes& f, Drawer& d, const DiffObj& diff) {
    const uint32_t pic = g_picture;
    if (pic < d.quietUntil) return false;
    const uint8_t* mat = g_ram + d.matList.addr;
    const uint8_t* dl = g_ram + diff.buf[0];
    const uint32_t ms = d.matList.size, ds = diff.size;
    if (ms == 0 || ds == 0 || ((ms | ds) & 31) != 0) return false;
    const uint32_t room = ms + ds + 0x80;
    if (d.mergedMat == ms && d.mergedDiff == ds && std::memcmp(g_ram + d.merged + room, mat, ms) == 0 &&
        std::memcmp(g_ram + d.merged + room + ms, dl, ds) == 0) {
        d.changes = 0;
        d.lastCall = pic;
        f.Call(d.merged, d.mergedSize);
        ++g_cnt.listCalls;
        return true;
    }
    if (d.lastCall != 0 && d.lastCall + 1 >= pic) return false;
    if (++d.changes >= 3) { d.changes = 0; d.quietUntil = pic + 300; return false; }
    if (!J3DPortMergeable(J3DPortScanDL(mat, ms), J3DPortScanDL(dl, ds))) { d.quietUntil = pic + 600; return false; }
    if (!d.merged || d.mergedCap < room + ms + ds) { d.mergedCap = (room + ms + ds + 0x7F) & ~0x1Fu; d.merged = AllocDyn(d.mergedCap); }
    d.mergedMat = 0;
    const uint32_t size = J3DPortBuildMerged(mat, ms, dl, ds, g_ram + d.merged, room);
    if (size == 0) { d.quietUntil = pic + 600; ++g_cnt.mergeRefused; return false; }
    std::memcpy(g_ram + d.merged + room, mat, ms);
    std::memcpy(g_ram + d.merged + room + ms, dl, ds);
    d.mergedSize = size;
    d.mergedMat = ms;
    d.mergedDiff = ds;
    vgx::hle::NotifyGuestWrite(g_ram + d.merged, size);
    ++g_cnt.mergeWrites;
    return false;
}

Bytes Frame() {
    Bytes f;
    f.CP(0xAC, g_mtxArr);
    f.CP(0xBC, 48);
    f.XF(0x1020, {F2u(1.2f), 0, F2u(1.6f), 0, F2u(-1.0f), F2u(-0.2f), 0});
    for (size_t di = 0; di < g_dr.size(); ++di) {
        Drawer& d = g_dr[di];
        if (di % 30 == 0) f.CP(0x30, 0xFFFFFFFF); // (DrawBuffer::drawOpa: J3DShape::sOldVcdVatCmd = nullptr at a new buffer; here: a new model)
        // (drawPort / draw)
        Instance& first = d.inst[0];
        bool merged = false;
        if (g_merge && !first.light && !first.hidden) merged = CallMerged(f, d, first.diff);
        if (merged) ++g_cnt.merged;
        else { f.Call(d.matList.addr, d.matList.size); ++g_cnt.listCalls; }
        ++g_cnt.matCalls;
        // loadPreDrawSetting: the vertex-format list (J3D's cache is cleared per draw buffer: here, every drawer) and the matrix index
        f.Call(g_fmt[d.fmt].addr, g_fmt[d.fmt].size);
        ++g_cnt.listCalls;
        f.CP(0x30, 30u << 6); // (texture coordinate 0 through GX_TEXMTX0: the differed lists' texture matrix is read)
        f.CP(0x40, 0);
        f.XF(0x1018, {30u << 6, 0});
        uint32_t last = 0, lastSize = 0, lastFlags = 0;
        bool lastTex = false; // (JL_BROKEN)
        if (merged) { last = first.diff.buf[0]; lastSize = first.diff.size; }
        for (size_t k = 0; k < d.inst.size(); ++k) {
            Instance& in = d.inst[k];
            if (in.light) {
                // (LightFunction::loadActorLightInfo: a light object and the ambient colour)
                std::vector<uint32_t> lw;
                for (int j = 0; j < 16; ++j) lw.push_back(j < 3 ? 0 : F2u(static_cast<float>(j) * 0.25f));
                lw[3] = 0xFFEEDDCCu;
                f.XF(0x600, lw);
                f.XF(0x100A, {0x404040FFu});
                last = 0;
            }
            if (in.hidden) continue;
            const uint32_t bytes = in.diff.buf[0], size = in.diff.size;
            if (k == 0 && merged) {
            } else if (g_skip && last && size == lastSize && (bytes == last || std::memcmp(g_ram + bytes, g_ram + last, size) == 0) &&
                       J3DPortDiffSkippable(lastFlags ? lastFlags : (lastFlags = J3DPortScanDL(g_ram + last, lastSize)))) {
                ++g_cnt.diffSkipped;
                if (lastTex && ValsOf(d, in).hasMtx && !in.texMtxShape) ++g_cnt.brokenSkips;
            } else {
                lastTex = false;
                f.Call(bytes, size);
                ++g_cnt.listCalls;
                ++g_cnt.diffSent;
                if (bytes != last || size != lastSize) lastFlags = 0;
                last = bytes;
                lastSize = size;
            }
            // J3DShape::drawFast
            f.CP(0xA0, g_pos); f.CP(0xB0, 12);
            f.CP(0xA1, g_nrm); f.CP(0xB1, 12);
            f.CP(0xA4, g_uv); f.CP(0xB4, 8);
            if (in.texMtxShape) { // (J3DDifferedTexMtx::load: a texture matrix of its own)
                std::vector<uint32_t> t;
                for (int j = 0; j < 12; ++j) t.push_back(F2u(static_cast<float>(j) * 0.125f));
                f.XF(0x78, t);
            }
            std::vector<uint32_t> w(12), n;
            for (int j = 0; j < 12; ++j) w[j] = F2u(in.m[j]);
            for (int j = 0; j < 9; ++j) n.push_back(F2u(j % 4 == 0 ? 1.0f : 0.0f));
            f.XF(0, w);
            f.XF(0x400, n);
            f.Call(d.geo.addr, d.geo.size);
            ++g_cnt.listCalls;
            if (last && in.texMtxShape) {
                if (g_broken) lastTex = true; else last = 0;
            }
        }
    }
    return f;
}

bool g_quiet = false;
uint64_t g_h = 0;
unsigned g_frame = 0;
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

int main(int argc, char** argv) {
    const unsigned frames = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 60;
    if (argc > 2) { const uint64_t s = static_cast<uint64_t>(std::atoll(argv[2])); g_rng ^= s * 0x9E3779B97F4A7C15ull; g_srng ^= s * 0xC2B2AE3D27D4EB4Full; }
    g_quiet = std::getenv("JL_PROFILE") != nullptr;
    const std::string mode = std::getenv("JL_MODE") ? std::getenv("JL_MODE") : "all";
    g_keep = mode == "keep" || mode == "all";
    g_merge = mode == "merge" || mode == "all";
    g_skip = mode == "skip" || mode == "all";
    g_broken = std::getenv("JL_BROKEN") != nullptr;
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
    Turn(0);
    Counts c0;
    for (g_frame = 1; g_frame <= frames; ++g_frame) {
        ++g_picture;
        if (g_frame == frames / 2 + 1) c0 = g_cnt; // (the counters of the second half: the steady state)
        Bytes f = Frame();
        size_t rd = 0, pub = 0;
        while (rd < f.size()) {
            pub = std::min(f.size(), pub + 2048 + Rn(4096));
            rd += vgx::ProcessStream(f.data() + rd, static_cast<uint32_t>(pub - rd), false);
            if (pub == f.size() && rd < f.size()) {
                const uint32_t n = vgx::ProcessStream(f.data() + rd, static_cast<uint32_t>(f.size() - rd), false);
                if (!n) { std::fprintf(stderr, "fixture: stream stuck\n"); return 2; }
                rd += n;
            }
        }
        vgx::g.dispCopySrc = vgx::EfbRect{0, 0, 640, 456};
        vgx::QueueCopyDisp(true);
        vgx::FrontPresent(false, nullptr);
        Turn(g_frame);
    }
    const double n = static_cast<double>(frames - frames / 2);
    std::fprintf(stderr,
                 "j3d-lists %s: per picture (second half) list calls %.1f; material lists %.1f (merged %.1f); differed lists sent %.1f, not sent %.1f; "
                 "merged lists made %.2f (refused %.2f); differed lists made %.1f, kept %.1f\n",
                 mode.c_str(), (g_cnt.listCalls - c0.listCalls) / n, (g_cnt.matCalls - c0.matCalls) / n, (g_cnt.merged - c0.merged) / n,
                 (g_cnt.diffSent - c0.diffSent) / n, (g_cnt.diffSkipped - c0.diffSkipped) / n, (g_cnt.mergeWrites - c0.mergeWrites) / n,
                 (g_cnt.mergeRefused - c0.mergeRefused) / n, (g_cnt.diffMade - c0.diffMade) / n, (g_cnt.diffKept - c0.diffKept) / n);
    if (g_broken) std::fprintf(stderr, "j3d-lists: %llu wrong skips made (JL_BROKEN)\n", static_cast<unsigned long long>(g_cnt.brokenSkips));
    uint64_t sl[5] = {}, sf[3] = {};
    vgx::StateListCounters(sl);
    vgx::StateListFastCounters(sf);
    std::fprintf(stderr, "j3d-lists %s: front end state lists: %llu replays, %llu parses; fast hits front %llu table %llu, of them after a compare of the bytes %llu\n",
                 mode.c_str(), static_cast<unsigned long long>(sl[0]), static_cast<unsigned long long>(sl[1]), static_cast<unsigned long long>(sf[0]),
                 static_cast<unsigned long long>(sf[1]), static_cast<unsigned long long>(sf[2]));
    return 0;
}
