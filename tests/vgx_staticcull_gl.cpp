// GXV_STATICCULL (static runs outside the clip volume are not drawn): the production GX front end (display-list cache included) and
// the production GLES back end on Mesa.
//
// Rigid objects are drawn from display lists in guest RAM (indexed position and normal, no matrix index), so that the lists become
// static buffers, through a camera that turns and moves frame after frame: objects in front of it, beside it (past each of the four
// side planes of the clip volume), behind the eye, straddling a plane, just inside and just outside one, through the near plane; in
// four materials (one textured from an EFB copy), the material often changing at a draw that is culled; two position-matrix slots
// (PNMTX0 / PNMTX1); some frames orthographic, some with a viewport smaller (only "behind the eye" may cull) or larger than the
// scissor. Each object is drawn as the game draws a shape: the material, the matrix, the list.
//   * the picture of every frame is read back and the hashes are folded together; the runner compares them across GXV_STATICCULL and
//     the other front-end switches: bit for bit;
//   * the records of every frame, and the culling counters, are printed: the runner checks that runs were culled and records saved.
// Usage: vgx_staticcull_gl [frames]     (the switches are read from the environment)
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
void StaticCullCounters(uint64_t out[3]); // (vgx_state.h; declared here too, so that the test also builds against a tree without the switch)
#ifndef VGX_NO_CULL23
void CullCounters23(uint64_t out[8]); // (ta23: GXV_STATICCULL_PAL / GXV_DYNCULL totals; -DVGX_NO_CULL23 for a tree without them)
void VcacheDrawCounters(uint64_t out[4]); // (ta23: GXV_DLVCACHE)
void ClusterCounters(uint64_t out[8]);    // (vertex25: GXV_CLUSTERCULL; -DVGX_NO_CLUSTER25 for a tree without it)
#endif
}
namespace {
using namespace vgx;
constexpr int W = 128, H = 96;
alignas(32) unsigned char destinations[4][32];
alignas(4096) uint8_t ram[1 << 21]; // (vertex25: 64 KiB before the big terrain sheets)
unsigned failures = 0;
void Check(bool ok, const char* label) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
void BP(unsigned r, unsigned v) { WriteBP(r << 24 | v); }
void XF(unsigned r, unsigned v) { WriteXF(r, 1, &v); }

size_t g_records = 0, g_static = 0;
uint64_t g_recordSum = 0;
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
void Viewport(float w = W, float h = H) {
    const uint32_t v[] = {f2u(w / 2.0f), f2u(-h / 2.0f), f2u(16777215.0f + 1), f2u(w / 2.0f + 342), f2u(h / 2.0f + 342), f2u(16777215.0f + 1)};
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
// GX projection registers (XF 0x1020..0x1026)
void Perspective(float fovY, float aspect, float n, float f) {
    const float t = 1.0f / std::tan(fovY * 0.5f);
    const uint32_t p[] = {f2u(t / aspect), 0, f2u(t), 0, f2u(-n / (f - n)), f2u(-(f * n) / (f - n)), 0};
    WriteXF(0x1020, 7, p);
}
void Ortho(float half, float n, float f) {
    const float s = 1.0f / half;
    const uint32_t p[] = {f2u(s), 0, f2u(s), 0, f2u(-1.0f / (f - n)), f2u(-f / (f - n)), 1};
    WriteXF(0x1020, 7, p);
}
// The camera: eye position and yaw / pitch (view = R^T * (p - eye)).
struct Cam { float ex, ey, ez, yaw, pitch; };
struct Placement { float cx, cy, cz, scale; };
// Position matrix slot `slot` (0: PNMTX0, 1: PNMTX1, rows 3..5) = view * model of object `id` at p.
void LoadMatrix(unsigned slot, int id, const Placement& p, const Cam& c) {
    const float a = 0.37f * id + 0.1f, b = 0.23f * id - 0.3f;
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
    const float r[9] = {ca, -sa, 0, sa * cb, ca * cb, -sb, sa * sb, ca * sb, cb}; // model rotation
    // view rotation: yaw about y, then pitch about x
    const float cy = std::cos(c.yaw), sy = std::sin(c.yaw), cp = std::cos(c.pitch), sp = std::sin(c.pitch);
    const float v[9] = {cy, 0, -sy, sy * sp, cp, cy * sp, sy * cp, -sp, cy * cp};
    float vr[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) vr[i * 3 + j] = v[i * 3] * r[j] + v[i * 3 + 1] * r[3 + j] + v[i * 3 + 2] * r[6 + j];
    const float tx = p.cx - c.ex, ty = p.cy - c.ey, tz = p.cz - c.ez;
    const float s = p.scale;
    float m[12];
    for (int i = 0; i < 3; ++i) {
        m[i * 4 + 0] = vr[i * 3 + 0] * s;
        m[i * 4 + 1] = vr[i * 3 + 1] * s;
        m[i * 4 + 2] = vr[i * 3 + 2] * s;
        m[i * 4 + 3] = v[i * 3] * tx + v[i * 3 + 1] * ty + v[i * 3 + 2] * tz;
    }
    PosRows(slot * 3, m);
    NrmRows(slot * 3, vr);
    XF(0x1018, (0x3CF3CF00u & ~0x3Fu) | (slot * 3));
}
void Light() {
    XF(0x100A, 0x303030FFu);
    uint32_t w[16] = {};
    w[3] = 0xFFE0C0FFu;
    w[4] = f2u(1.0f);
    w[7] = f2u(1.0f);
    w[10] = f2u(0.3f); w[11] = f2u(0.4f); w[12] = f2u(3.0f);
    WriteXF(0x600, 16, w);
    // (vertex25, GXV_VS_LTSKIP) STATICCULL_LIGHTS=1..3: a second light (1) next to light 0 in the channels' masks: 1 black, 2 rgb black with alpha 0x80, 3 live
    if (const char* e = std::getenv("STATICCULL_LIGHTS")) {
        const int k = std::atoi(e);
        uint32_t w1[16] = {};
        w1[3] = k == 1 ? 0u : k == 2 ? 0x00000080u : 0xC0A0FFFFu;
        w1[4] = f2u(1.0f); w1[7] = f2u(1.0f);
        w1[10] = f2u(-0.5f); w1[11] = f2u(0.2f); w1[12] = f2u(2.0f);
        WriteXF(0x610, 16, w1);
    }
}
bool LightsOn() { const char* e = std::getenv("STATICCULL_LIGHTS"); return e && *e && *e != '0'; }
void Material(char m) {
    const bool tex = m == 'C';
    BP(0, (tex ? 1u : 0u) | (1u << 4));
    XF(0x1009, 1);
    XF(0x103F, tex ? 1u : 0u);
    XF(0x1040, 1u << 7);
    XF(0x100C, m == 'B' ? 0xE0A0FF90u : m == 'D' ? 0x40C0FFFFu : 0xFFFFFFFFu);
    XF(0x100E, m == 'D' ? 0u : ((1u << 1) | (1u << 2) | (2u << 7) | (LightsOn() ? (1u << 3) : 0u)));
    XF(0x1010, LightsOn() && m != 'D' && std::atoi(std::getenv("STATICCULL_LIGHTS")) >= 2 ? ((1u << 1) | (1u << 2) | (2u << 7) | (1u << 3)) : 0u);
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
// (ta23, GXV_STATICCULL_PAL) the same disc with a direct position-matrix index per vertex: the centre and the odd rim vertices name
// rows 0..2 (PNMTX0), the even rim vertices rows 3..5 (PNMTX1): a "skinned" shape of two joints.
void MakeSkinMesh(Mesh& m, const Mesh& from) {
    m = from;
    m.listBytes = 3 + static_cast<uint32_t>(m.tris) * 3 * 5;
    m.list = Take(m.listBytes);
    uint8_t* p = m.list;
    *p++ = 0x90;
    *p++ = static_cast<uint8_t>(m.tris * 3 >> 8);
    *p++ = static_cast<uint8_t>(m.tris * 3);
    for (int i = 0; i < m.tris; ++i)
        for (int k = 0; k < 3; ++k) {
            const unsigned ix = k == 0 ? 0u : static_cast<unsigned>(1 + (i + k - 1) % m.tris);
            *p++ = static_cast<uint8_t>(ix == 0 || (ix & 1) ? 0 : 3); // PNMTXIDX
            *p++ = static_cast<uint8_t>(ix >> 8); *p++ = static_cast<uint8_t>(ix);
            *p++ = static_cast<uint8_t>(ix >> 8); *p++ = static_cast<uint8_t>(ix);
        }
}
Mesh g_skin[3];
// (ta23, GXV_DLVCACHE) a bumpy w x h grid drawn as GX triangle strips of 3..8 vertices along its rows, as J3D's exporter writes a
// shape: with GXV_DLWELD the strips share their vertices and the triangle order decides the vertex-cache hits
Mesh g_grid, g_big;
void MakeGrid(Mesh& m, int w, int h, bool coherent = false) {
    m.pos = Take(static_cast<uint32_t>(w * h) * 12);
    m.nrm = Take(static_cast<uint32_t>(w * h) * 12);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // (the upper half folded back over the lower one, a little above it and lit differently: where the two layers overlap
            // on the screen the depth test decides, or, without one, the order of the triangles)
            const bool fold = y >= h / 2;
            const float fx = -1.0f + 2.0f * x / (w - 1), fy0 = -1.0f + 2.0f * y / (h - 1), fy = fold ? -fy0 + 0.1f : fy0;
            const float z = 0.15f * std::sin(3.0f * fx) * std::cos(2.0f * fy) + (fold ? 0.12f + 0.3f * (fy0 - fy) * 0.2f : 0.0f);
            uint8_t* p = m.pos + (y * w + x) * 12;
            Put32(p, f2u(fx)); Put32(p + 4, f2u(fy)); Put32(p + 8, f2u(z));
            uint8_t* n = m.nrm + (y * w + x) * 12;
            Put32(n, f2u(fold ? 0.0f : -0.3f * fx)); Put32(n + 4, f2u(fold ? 0.0f : 0.2f * fy)); Put32(n + 8, f2u(fold ? -1.0f : 0.9f));
        }
    std::vector<uint8_t> l;
    unsigned r = 99;
    m.tris = 0;
    for (int yy = 0; yy + 1 < h; ++yy) {
        // (rows from the two layers in turn: the list's order and a vertex-cache order put them differently over each other)
        const int y = coherent ? yy : ((yy & 1) ? h - 2 - yy / 2 : yy / 2);
        std::vector<unsigned> row;
        for (int x = 0; x < w; ++x) { row.push_back(static_cast<unsigned>(y * w + x)); row.push_back(static_cast<unsigned>((y + 1) * w + x)); }
        for (size_t st = 0; st + 2 < row.size();) {
            r = r * 1103515245u + 12345u;
            size_t len = 3 + (r >> 8) % 6;
            if (st + len > row.size()) len = row.size() - st;
            l.push_back(0x98);
            l.push_back(0);
            l.push_back(static_cast<uint8_t>(len));
            for (size_t k = 0; k < len; ++k) {
                const unsigned ix = row[st + k];
                l.push_back(static_cast<uint8_t>(ix >> 8)); l.push_back(static_cast<uint8_t>(ix));
                l.push_back(static_cast<uint8_t>(ix >> 8)); l.push_back(static_cast<uint8_t>(ix));
            }
            m.tris += static_cast<int>(len) - 2;
            st += len - 2;
        }
    }
    m.listBytes = static_cast<uint32_t>((l.size() + 31) & ~size_t(31));
    m.list = Take(m.listBytes);
    std::memset(m.list, 0, m.listBytes);
    std::memcpy(m.list, l.data(), l.size());
    m.tris = w * h - 1; // (DrawList: the arrays' size is (tris + 1) * 12 bytes)
}
void DrawList(const Mesh& m) {
    const uint32_t bytes = static_cast<uint32_t>(m.tris + 1) * 12;
    g.arrays[9] = {m.pos, bytes, 12, false};
    g.arrays[10] = {m.nrm, bytes, 12, false};
    g.cpArrayStale = 0;
    CallList(m.list, m.listBytes);
}

// ---- the scene: objects all around the camera's path
struct Obj {
    char mat;
    int kind;  // 0: 132 vertices, 1: 210, 2: 1200, 3: 24 (CPU-transformed: never a static buffer with GXV_SOFTXFORM=128)
    int id;
    unsigned slot;
    Placement pl;
};
std::vector<Obj> Scene() {
    std::vector<Obj> s;
    unsigned r = 777;
    auto rnd = [&] { r = r * 1103515245u + 12345u; return ((r >> 8) & 0xFFFF) / 65535.0f; };
    const char mats[] = "AABCDABACCDBAAD";
    for (int i = 0; i < 96; ++i) {
        // a ring around the path (distance 2..9), heights from below to above the view, a few close to the eye
        const float ang = 6.2831853f * rnd(), dist = i % 9 == 0 ? 0.3f + 0.4f * rnd() : 2.0f + 7.0f * rnd();
        const float y = (rnd() - 0.5f) * (i % 5 == 0 ? 12.0f : 3.0f);
        const Placement p{dist * std::sin(ang), y, -dist * std::cos(ang), 0.3f + 0.9f * rnd()};
        s.push_back(Obj{mats[i % 15], i % 7 == 6 ? 3 : (i % 11 == 10 ? 2 : i % 2), i + 1, static_cast<unsigned>(i % 3 == 2), p});
    }
    return s;
}
Cam CameraAt(int f) {
    // turning on the spot, then walking forward and looking up and down; the last frames hug the objects
    Cam c{0, 0, 0, 0.26f * f, 0};
    if (f >= 12) { c.ez = -0.05f * (f - 12); c.ex = 0.04f * (f - 12); c.pitch = 0.35f * std::sin(0.7f * f); }
    return c;
}
} // namespace

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 30;
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
    MakeSkinMesh(g_skin[0], g_mesh[0]);
    MakeSkinMesh(g_skin[1], g_mesh[1]);
    MakeSkinMesh(g_skin[2], g_mesh[2]);
    MakeGrid(g_grid, 16, 12);
    MakeGrid(g_big, 70, 50, true); // (vertex25: 4 700 triangles in strip order along the rows: 14 000 indices, one run, about 73 clusters)
    hle::RegisterGuestRam(ram, sizeof ram);
    // a first picture with big overlapping shapes: the texture of the textured material
    const Cam still{0, 0, 0, 0, 0};
    Material('A'); BP(0x40, 1u | (7u << 1) | (1u << 4));
    Copy(3, true);
    for (int i = 0; i < 6; ++i) {
        Material(i & 1 ? 'B' : 'A');
        LoadMatrix(0, 100 + i, Placement{-0.6f + 0.25f * i, 0.4f - 0.15f * i, -0.3f, 0.45f}, still);
        DrawList(g_mesh[i & 1]);
    }
    Copy(0, true);
    FrontPresent(false, nullptr);
    BindCopy(0);
    const std::vector<Obj> scene = Scene();
    uint64_t hash = 14695981039346656037ull;
    std::vector<uint8_t> px;
    int detailed = 0;
    std::printf("records/distinct colours per frame:");
    for (int f = 0; f < frames; ++f) {
        const bool ortho = f % 10 == 7;
        if (ortho) Ortho(3.0f, 0.1f, 40.0f);
        else Perspective(f % 4 == 3 ? 0.6f : 1.1f, static_cast<float>(W) / H, 0.25f, 40.0f);
        // (some frames: a viewport smaller than the scissor, where only "behind the eye" may cull; some: a larger one)
        if (f % 10 == 3) Viewport(W * 0.75f, H * 0.75f);
        else if (f % 10 == 5) Viewport(W * 1.25f, H * 1.25f);
        else Viewport();
        const Cam cam = CameraAt(f);
        char cur = 0;
        for (const Obj& o : scene) {
            if (o.mat != cur) { Material(o.mat); cur = o.mat; }
            LoadMatrix(o.slot, o.id, o.pl, cam);
            DrawList(g_mesh[o.kind]);
        }
        // the same objects again, in the reverse order, each with its own material: a material change at every draw
        for (size_t i = scene.size(); i-- > 0;) {
            const Obj& o = scene[i];
            if (i % 4) continue;
            Material(o.mat == 'A' ? 'D' : 'A');
            LoadMatrix(1 - o.slot, o.id + 1000, Placement{o.pl.cx * 0.8f, o.pl.cy, o.pl.cz * 0.8f, o.pl.scale * 0.7f}, cam);
            DrawList(g_mesh[o.kind]);
        }
        // (ta23) two-joint shapes (a matrix index per vertex): the second joint's place a little off the first's, so that some
        // are wholly outside a plane, some straddle it with one joint, some are in view
        SetVcd(0, 1);
        for (size_t i = 0; i < scene.size(); i += 2) {
            const Obj& o = scene[i];
            Material(o.mat == 'C' ? 'B' : o.mat);
            const Placement b{o.pl.cx + (i % 6 == 0 ? 2.5f : 0.4f), o.pl.cy - 0.3f, o.pl.cz + 0.2f, o.pl.scale * 0.8f};
            LoadMatrix(0, o.id + 2000, Placement{o.pl.cx, o.pl.cy + 0.2f, o.pl.cz, o.pl.scale}, cam);
            LoadMatrix(1, o.id + 3000, b, cam);
            DrawList(g_skin[i % 3]);
        }
        SetVcd(0, 0);
        // (ta23) terrain patches of strips: depth-writing ('A'), depth-tested without writes ('B'), blended-like without a test ('D')
        for (int i = 0; i < 9; ++i) {
            Material(i % 3 == 0 ? 'A' : i % 3 == 1 ? 'B' : 'D');
            LoadMatrix(i & 1, 4000 + i, Placement{-1.5f + 0.4f * i, -0.5f + 0.1f * i, -2.5f - 0.3f * i, 0.9f}, cam);
            DrawList(g_grid);
        }
        // (vertex25, GXV_CLUSTERCULL) big terrain sheets, drawn in strip order, partly in view, in the three material classes
        for (int i = 0; i < 4; ++i) {
            Material(i % 3 == 0 ? 'A' : i % 3 == 1 ? 'B' : 'D');
            LoadMatrix(i & 1, 5000 + i, Placement{-2.5f + 1.7f * i, -0.2f * i, -2.0f - 1.1f * i, 3.2f - 0.4f * i}, cam);
            DrawList(g_big);
        }
        const uint32_t id = Copy(1, true);
        FrontPresent(false, nullptr);
        int pw = 0, ph = 0;
        if (!backend::DebugReadTexture(id, px, pw, ph) || pw != W || ph != H) { Check(false, "read the renderer's picture"); break; }
        hash = Fold(hash, px);
        std::printf(" %zu/%u", g_records, Distinct(px));
        detailed += Distinct(px) > 40;
        if (std::getenv("STATICCULL_PPM")) {
            char name[64];
            std::snprintf(name, sizeof name, "%s%02d.ppm", std::getenv("STATICCULL_PPM"), f);
            if (FILE* o = std::fopen(name, "wb")) {
                std::fprintf(o, "P6 %d %d 255\n", W, H);
                for (int i = 0; i < W * H; ++i) std::fwrite(&px[static_cast<size_t>(i) * 4], 1, 3, o);
                std::fclose(o);
            }
        }
        g_recordSum += g_records;
    }
    std::printf("\n");
    uint64_t c[3] = {};
    StaticCullCounters(c);
    std::printf("static runs tested %llu culled %llu (vertices %llu)\n", (unsigned long long)c[0], (unsigned long long)c[1], (unsigned long long)c[2]);
    uint64_t c23[8] = {}, vc[4] = {};
#ifndef VGX_NO_CULL23
    CullCounters23(c23);
    VcacheDrawCounters(vc);
#endif
    std::printf("vcache runs drawn reordered %llu of %llu (indices %llu of %llu)\n", (unsigned long long)vc[0], (unsigned long long)vc[1],
                (unsigned long long)vc[2], (unsigned long long)vc[3]);
    uint64_t cl[8] = {};
#if !defined(VGX_NO_CULL23) && !defined(VGX_NO_CLUSTER25)
    ClusterCounters(cl);
#endif
    std::printf("cluster runs: indices %llu, in culled clusters %llu, left out %llu (%llu runs), draws added %llu, no cluster in view %llu, range bad %llu, box bad %llu\n",
                (unsigned long long)cl[0], (unsigned long long)cl[1], (unsigned long long)cl[2], (unsigned long long)cl[7], (unsigned long long)cl[3], (unsigned long long)cl[6],
                (unsigned long long)cl[4], (unsigned long long)cl[5]);
    std::printf("pal runs tested %llu culled %llu (indices %llu); dynamic draws tested %llu culled %llu (indices %llu), not decoded %llu\n",
                (unsigned long long)c23[0], (unsigned long long)c23[1], (unsigned long long)c23[2], (unsigned long long)c23[3],
                (unsigned long long)c23[4], (unsigned long long)c23[5], (unsigned long long)c23[6]);
    Check(detailed * 2 >= frames, "most pictures have detail");
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    std::printf("picture %dx%d hash %016llx\n", W, H, static_cast<unsigned long long>(hash));
    std::printf("records %llu\n", (unsigned long long)g_recordSum);
    std::printf("culled %llu\n", (unsigned long long)c[1]);
    std::printf("palculled %llu\n", (unsigned long long)c23[1]);
    std::printf("dynculled %llu\n", (unsigned long long)c23[4]);
    std::printf("vcachetook %llu\n", (unsigned long long)vc[0]);
    std::printf("clleft %llu\n", (unsigned long long)cl[2]);
    std::printf("clbad %llu\n", (unsigned long long)(cl[4] + cl[5]));
    return failures ? 1 : 0;
}
