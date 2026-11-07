// GXV_RIGIDMERGE_DLMAX=<n> / GXV_RIGIDMERGE_DLLEARN: rigid draws of cached display lists (static buffers) that join palette runs as copies in the frame's arena;
// the production GX front end (display-list cache included) and the production GLES back end on Mesa.
//
// Rigid objects are drawn from display lists in guest RAM (indexed position and normal, no matrix index) in runs of four materials, frame after frame, so
// that the lists become static buffers, and then (learned copies) are copied into the arena where that joins a run. Lists of 132 and 210 vertices (above
// GXV_SOFTXFORM, copyable), one of 1200 (above every cap), and one of 24 (always in the arena); isolated draws between other materials; a run longer
// than a palette; objects that overlap, so that the order of the depth and blend operations shows in the picture.
//   * the picture of every frame is read back and the hashes are folded together; the runner compares them across GXV_RIGIDMERGE_DLMAX / GXV_RIGIDMERGE_DLLEARN
//     values and the other front-end switches: bit for bit;
//   * the records of the last frame and the bytes copied are printed; the runner compares them (learned: as few records as the same cap with every list
//     copied, fewer bytes copied).
// Usage: vgx_dllearn_gl [frames]     (the switches are read from the environment)
#include "vgx_state.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"
#include <GLES2/gl2.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
}
namespace {
using namespace vgx;
constexpr int W = 128, H = 96;
alignas(32) unsigned char destinations[4][32];
alignas(4096) uint8_t ram[1 << 16];
unsigned failures = 0;
void Check(bool ok, const char* label) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
void BP(unsigned r, unsigned v) { WriteBP(r << 24 | v); }
void XF(unsigned r, unsigned v) { WriteXF(r, 1, &v); }

std::vector<size_t> g_frameRecords, g_frameStatic;
size_t g_records = 0, g_static = 0;
void Observe(CmdBuf& b) {
    g_records = b.draws.size();
    g_static = 0;
    for (const DrawRec& d : b.draws) g_static += d.vbuf != 0;
}
} // namespace
namespace vgx {
void SubmitToBackend(CmdBuf& b) {
    Observe(b);
    backend::Execute(b);
}
} // namespace vgx
namespace {
void Viewport() {
    const uint32_t v[] = {f2u(W / 2.0f), f2u(-H / 2.0f), f2u(16777215.0f + 1), f2u(W / 2.0f + 342), f2u(H / 2.0f + 342), f2u(16777215.0f + 1)};
    WriteXF(0x101A, 6, v);
}
void Scissor() {
    BP(0x20, 342u | (342u << 12));
    BP(0x21, (H - 1 + 342u) | ((W - 1 + 342u) << 12));
    BP(0x59, 171u | (171u << 10));
}
void PosRows(unsigned row, const float m[12]) {
    uint32_t w[12];
    for (int i = 0; i < 12; ++i) w[i] = f2u(m[i]);
    WriteXF(row * 4, 12, w);
}
void NrmRows(unsigned row, const float m[9]) {
    uint32_t w[9];
    for (int i = 0; i < 9; ++i) w[i] = f2u(m[i]);
    WriteXF(0x400 + row * 3, 9, w);
}
struct Placement { float cx, cy, cz, scale; };
void LoadMatrix(unsigned row, int id, const Placement& p) {
    const float a = 0.37f * id + 0.1f, b = 0.23f * id - 0.3f;
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
    const float r[9] = {ca, -sa, 0, sa * cb, ca * cb, -sb, sa * sb, ca * sb, cb};
    const float s = p.scale;
    const float m[12] = {r[0] * s, r[1] * s, r[2] * s, p.cx, r[3] * s, r[4] * s, r[5] * s, p.cy, r[6] * s, r[7] * s, r[8] * s, p.cz};
    PosRows(row, m);
    NrmRows(row, r);
}
void Light() {
    XF(0x100A, 0x303030FFu);
    uint32_t w[16] = {};
    w[3] = 0xFFE0C0FFu;
    w[4] = f2u(1.0f);
    w[7] = f2u(1.0f);
    w[10] = f2u(0.3f); w[11] = f2u(0.4f); w[12] = f2u(3.0f);
    WriteXF(0x600, 16, w);
}
void Material(char m) {
    const bool tex = m == 'C';
    BP(0, (tex ? 1u : 0u) | (1u << 4));
    XF(0x1009, 1);
    XF(0x103F, tex ? 1u : 0u);
    XF(0x1040, 1u << 7);
    XF(0x100C, m == 'B' ? 0xE0A0FF90u : m == 'D' ? 0x40C0FFFFu : 0xFFFFFFFFu);
    XF(0x100E, m == 'D' ? 0u : ((1u << 1) | (1u << 2) | (2u << 7)));
    XF(0x1010, 0);
    BP(0x28, tex ? (1u << 6) : 0u);
    if (tex) BP(0xC0, (15u << 12) | (8u << 8) | (10u << 4) | 15u | (1u << 19));
    else BP(0xC0, (10u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (5u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, m == 'D' ? 0u : (m == 'B' ? (1u | (3u << 1)) : (1u | (3u << 1) | (1u << 4))));
    BP(0x41, m == 'B' ? (1u | (1u << 3) | (1u << 4) | (4u << 8) | (5u << 5)) : ((1u << 3) | (1u << 4)));
    BP(0x42, 0);
}
uint32_t Copy(int index, bool clear) {
    g.texCopySrc = EfbRect{0, 0, W, H};
    g.texCopyDstW = W; g.texCopyDstH = H; g.texCopyFmt = 6;
    QueueCopyTex(destinations[index], clear);
    return Pending().copyTex.back().texId;
}
void BindCopy(int index) {
    BP(0x94, (reinterpret_cast<uintptr_t>(destinations[index]) >> 5) & 0xFFFFFF);
    BP(0x88, (W - 1) | ((H - 1) << 10) | (1u << 20));
    auto& t = g.tex[0]; t = TexSlot{}; t.valid = true;
    t.obj.data = destinations[index]; t.obj.width = W; t.obj.height = H; t.obj.format = 6;
    t.obj.minFilter = t.obj.magFilter = 1;
    g.bpTexStale = 0; g.texDirty = true; g.texSlotDirty = 1;
}
uint64_t Fold(uint64_t h, const std::vector<uint8_t>& p) {
    for (uint8_t b : p) h = (h ^ b) * 1099511628211ull;
    return h;
}
unsigned Distinct(const std::vector<uint8_t>& p) {
    std::map<uint32_t, int> seen;
    for (size_t i = 0; i + 3 < p.size(); i += 4) seen[p[i] | p[i + 1] << 8 | p[i + 2] << 16]++;
    return static_cast<unsigned>(seen.size());
}
void Put32(uint8_t* p, uint32_t v) {
    for (unsigned i = 0; i != 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}

// ---- the lists: a bumpy disc of `tris` triangles (3 * tris vertices), indexed position and normal (the same index), 16-bit indices
struct Mesh {
    int tris = 0;
    uint8_t* list = nullptr;
    uint32_t listBytes = 0;
    uint8_t* pos = nullptr;
    uint8_t* nrm = nullptr;
};
Mesh g_mesh[4];
uint32_t g_ramTop = 0;
uint8_t* Take(uint32_t bytes) {
    uint8_t* p = ram + g_ramTop;
    g_ramTop = (g_ramTop + bytes + 63u) & ~63u;
    return p;
}
void MakeMesh(Mesh& m, int tris) {
    const float pi = 3.14159265f;
    m.tris = tris;
    const int unique = tris + 1;
    m.pos = Take(static_cast<uint32_t>(unique) * 12);
    m.nrm = Take(static_cast<uint32_t>(unique) * 12);
    auto put = [](uint8_t* p, float x, float y, float z) {
        Put32(p, f2u(x)); Put32(p + 4, f2u(y)); Put32(p + 8, f2u(z));
    };
    put(m.pos, 0.0f, 0.0f, 0.3f);
    put(m.nrm, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < tris; ++i) {
        const float a = 2 * pi * i / tris;
        put(m.pos + (1 + i) * 12, std::cos(a), std::sin(a), 0.25f * std::cos(3 * a));
        put(m.nrm + (1 + i) * 12, 0.5f * std::cos(a), 0.5f * std::sin(a), 0.8f);
    }
    m.listBytes = 3 + static_cast<uint32_t>(tris) * 3 * 4;
    m.list = Take(m.listBytes);
    uint8_t* p = m.list;
    *p++ = 0x90; // GX_TRIANGLES, vertex format 0
    *p++ = static_cast<uint8_t>(tris * 3 >> 8);
    *p++ = static_cast<uint8_t>(tris * 3);
    for (int i = 0; i < tris; ++i)
        for (int k = 0; k < 3; ++k) {
            const unsigned ix = k == 0 ? 0u : static_cast<unsigned>(1 + (i + k - 1) % tris);
            *p++ = static_cast<uint8_t>(ix >> 8); *p++ = static_cast<uint8_t>(ix); // position
            *p++ = static_cast<uint8_t>(ix >> 8); *p++ = static_cast<uint8_t>(ix); // normal
        }
}
void DrawList(const Mesh& m) {
    const uint32_t bytes = static_cast<uint32_t>(m.tris + 1) * 12;
    g.arrays[9] = {m.pos, bytes, 12, false};
    g.arrays[10] = {m.nrm, bytes, 12, false};
    g.cpArrayStale = 0;
    CallList(m.list, m.listBytes);
}

// ---- the scene
struct Obj {
    char mat;
    int kind;  // 0: 132 vertices, 1: 210, 2: 1200 (above every cap), 3: 24 (CPU-transformed: always in the arena)
    int id;
    Placement pl;
};
std::vector<Obj> Scene() {
    std::vector<Obj> s;
    unsigned r = 4242;
    auto rnd = [&] { r = r * 1103515245u + 12345u; return ((r >> 8) & 0xFFFF) / 65535.0f; };
    int id = 1;
    auto add = [&](char mat, int n, int kind0, int kind1 = -1) {
        for (int i = 0; i < n; ++i) {
            Placement p{-0.8f + 1.6f * rnd(), -0.8f + 1.6f * rnd(), -0.25f - 0.5f * rnd(), 0.13f + 0.1f * rnd()};
            s.push_back(Obj{mat, (kind1 >= 0 && (i & 1)) ? kind1 : kind0, id++, p});
        }
    };
    add('A', 12, 0);        // longer than a palette
    add('B', 5, 1);
    add('A', 1, 2);         // above every cap: a static buffer between runs
    add('A', 5, 0, 1);      // two lists alternating, one run
    add('D', 1, 0);         // alone between two other materials
    add('A', 1, 0);
    add('D', 1, 1);
    add('C', 7, 0, 1);
    add('A', 4, 3);         // always in the arena
    add('A', 3, 1);
    add('A', 1, 2);
    add('B', 3, 0);
    add('A', 2, 0);
    add('D', 2, 3);
    return s;
}
} // namespace

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 24;
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(W, H), "production backend init");
    if (failures) return 1;
    ResetState(); g.efbW = W; g.efbH = H; QueueSetTarget(); BP(0x43, 1);
    const uint32_t proj[] = {f2u(1), 0, f2u(1), 0, f2u(-1), f2u(-1), 1};
    WriteXF(0x1020, 7, proj);
    Viewport(); Scissor(); Light();
    const float tm[12] = {0.5f, 0, 0, 0.5f, 0, 0.5f, 0, 0.5f, 0, 0, 0, 1};
    PosRows(60, tm);
    BP(0x4F, 30u | (255u << 8)); BP(0x50, (60u << 8) | 80u); BP(0x51, 0xFFFFFFu);
    SetVcd(9, 3);
    SetVcd(10, 3);
    SetVat(0, 9, 1, 4, 0);
    SetVat(0, 10, 0, 4, 0);
    MakeMesh(g_mesh[0], 44);
    MakeMesh(g_mesh[1], 70);
    MakeMesh(g_mesh[2], 400);
    MakeMesh(g_mesh[3], 8);
    hle::RegisterGuestRam(ram, sizeof ram);
    // a first picture with big overlapping shapes: the texture of the textured material
    Material('A'); BP(0x40, 1u | (7u << 1) | (1u << 4));
    Copy(3, true);
    for (int i = 0; i < 6; ++i) {
        Material(i & 1 ? 'B' : 'A');
        LoadMatrix(0, 100 + i, Placement{-0.6f + 0.25f * i, 0.4f - 0.15f * i, -0.3f, 0.45f});
        DrawList(g_mesh[i & 1]);
    }
    Copy(0, true);
    FrontPresent(false, nullptr);
    BindCopy(0);
    const std::vector<Obj> scene = Scene();
    uint64_t hash = 14695981039346656037ull;
    std::vector<uint8_t> px;
    for (int f = 0; f < frames; ++f) {
        char cur = 0;
        for (const Obj& o : scene) {
            if (o.mat != cur) { Material(o.mat); cur = o.mat; }
            LoadMatrix(0, o.id, o.pl);
            DrawList(g_mesh[o.kind]);
        }
        const uint32_t id = Copy(1, true);
        FrontPresent(false, nullptr);
        int pw = 0, ph = 0;
        if (!backend::DebugReadTexture(id, px, pw, ph) || pw != W || ph != H) { Check(false, "read the renderer's picture"); break; }
        hash = Fold(hash, px);
        g_frameRecords.push_back(g_records);
        g_frameStatic.push_back(g_static);
    }
    std::printf("frames %d, records per frame:", frames);
    for (size_t r : g_frameRecords) std::printf(" %zu", r);
    std::printf("\n");
    uint64_t c[8] = {};
    DlLearnCounters(c);
    std::printf("last frame: %zu records, %zu of them static\n", g_frameRecords.back(), g_frameStatic.back());
    std::printf("copies %llu bytes %llu joined %llu wasted %llu static-uses %llu could-join %llu kept-bytes %llu kept-lists %llu\n", (unsigned long long)c[0],
                (unsigned long long)c[1], (unsigned long long)c[2], (unsigned long long)c[3], (unsigned long long)c[4], (unsigned long long)c[5],
                (unsigned long long)c[6], (unsigned long long)c[7]);
    Check(Distinct(px) > 60, "the picture has detail");
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    std::printf("picture %dx%d hash %016llx\n", W, H, static_cast<unsigned long long>(hash));
    std::printf("records %zu\n", g_frameRecords.back());
    std::printf("copybytes %llu\n", (unsigned long long)c[1]);
    return failures ? 1 : 0;
}
