// GXV_EDGE_AA, the front end's side (vgx_draw.cpp "whole-picture edge smoothing"): the production GX front end and GLES back end on
// Mesa (the harness of tests/vgx_staticcull_gl.cpp). Pictures of 3D objects, with HUD draws between the game's HUD markers (BP 0xFF,
// PORT_GX_MARK_HUD_BEGIN / END) or without; the display copy each frame hands to the back end is checked:
//   - no HUD: marked for smoothing, no HUD rectangle;
//   - one small orthographic HUD shape: one rectangle that holds the shape's projected disc (with the pad), nothing for the 3D draws;
//   - the same shape drawn outside the markers: no rectangle (only HUD draws are kept plain);
//   - a HUD shape over most of the picture: not marked (GXV_EDGE_AA_HUD, 50 %);
//   - 24 small HUD shapes apart: at most 16 rectangles, together holding every shape's centre;
//   - a HUD drawn, then cleared away by an EFB copy that clears it, before the display copy: no rectangle;
//   - GXV_EDGE_AA unset (the second run): never marked, no rectangle, and the GX stream's markers ignored.
// Usage: vgx_edgeaa_fe   (GXV_EDGE_AA from the environment)
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

std::vector<CopyDispCmd> g_disp; // the display copies of the last frame submitted
} // namespace
namespace vgx {
void SubmitToBackend(CmdBuf& b) {
    g_disp = b.copyDisp;
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


void HudBegin() { WriteBP((0xFFu << 24) | 1u); }
void HudEnd() { WriteBP((0xFFu << 24) | 2u); }
const Cam kStill{0, 0, 0, 0, 0};
// A disc of radius `r` (model units 1 x scale) centred at (cx, cy) in the orthographic view of half-size 3: on the 128x96 screen its
// centre is at (64 + cx / 3 * 64, 48 - cy / 3 * 48) and its radius about scale / 3 * 64 px across.
void Shape(int id, float cx, float cy, float scale) {
    LoadMatrix(0, id, Placement{cx, cy, -1.0f, scale}, kStill);
    DrawList(g_mesh[0]);
}
int ScreenX(float cx) { return static_cast<int>(64.0f + cx / 3.0f * 64.0f); }
int ScreenY(float cy) { return static_cast<int>(48.0f - cy / 3.0f * 48.0f); }
bool Holds(const EfbRect& r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
// One frame: the 3D objects, then `hud` (drawn between the markers or not), the display copy.
template <class F> const CopyDispCmd& Frame(F hud, bool markers) {
    Perspective(1.1f, static_cast<float>(W) / H, 0.25f, 40.0f);
    Material('A');
    for (int i = 0; i < 8; ++i) {
        LoadMatrix(0, 10 + i, Placement{-2.0f + 0.6f * i, 0.3f * (i % 3) - 0.3f, -4.0f, 0.5f}, kStill);
        DrawList(g_mesh[1]);
    }
    Ortho(3.0f, 0.1f, 40.0f);
    Material('B');
    if (markers) HudBegin();
    hud();
    if (markers) HudEnd();
    g.dispCopySrc = EfbRect{0, 0, W, H};
    QueueCopyDisp(true);
    FrontPresent(false, nullptr);
    static CopyDispCmd none;
    return g_disp.empty() ? none : g_disp.back();
}
} // namespace

int main() {
    const bool on = std::getenv("GXV_EDGE_AA") && std::atoi(std::getenv("GXV_EDGE_AA")) != 0;
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(W, H), "production backend init");
    if (failures) return 1;
    ResetState(); g.efbW = W; g.efbH = H; QueueSetTarget(); BP(0x43, 1);
    Viewport(); Scissor(); Light();
    BP(0x4F, 30u | (255u << 8)); BP(0x50, (60u << 8) | 80u); BP(0x51, 0xFFFFFFu);
    SetVcd(9, 3);
    SetVcd(10, 3);
    SetVat(0, 9, 1, 4, 0);
    SetVat(0, 10, 0, 4, 0);
    MakeMesh(g_mesh[0], 44);
    MakeMesh(g_mesh[1], 70);
    hle::RegisterGuestRam(ram, sizeof ram);
    char line[300];
    std::printf("GXV_EDGE_AA %s\n", on ? "on" : "off");
    {
        const CopyDispCmd& c = Frame([] {}, true);
        std::snprintf(line, sizeof line, "no HUD: marked %d, %d HUD rectangles (expected %d, 0)", c.edge, c.hudN, on);
        Check(c.edge == on && c.hudN == 0, line);
    }
    {
        const CopyDispCmd& c = Frame([] { Shape(200, -1.5f, 1.0f, 0.4f); }, true);
        const int x = ScreenX(-1.5f), y = ScreenY(1.0f);
        bool holds = c.hudN == 1 && Holds(c.hud[0], x, y) && Holds(c.hud[0], x - 7, y) && Holds(c.hud[0], x + 7, y) && c.hud[0].w < 40 && c.hud[0].h < 40;
        std::snprintf(line, sizeof line, "one HUD shape at (%d,%d): marked %d, %d rectangles, first (%d,%d %dx%d) (expected %d, %d, holding the shape, under 40x40)", x, y,
                      c.edge, c.hudN, c.hudN ? c.hud[0].x : 0, c.hudN ? c.hud[0].y : 0, c.hudN ? c.hud[0].w : 0, c.hudN ? c.hud[0].h : 0, on, on ? 1 : 0);
        Check(on ? c.edge && holds : !c.edge && c.hudN == 0, line);
    }
    {
        const CopyDispCmd& c = Frame([] { Shape(201, -1.5f, 1.0f, 0.4f); }, false);
        std::snprintf(line, sizeof line, "the same shape outside the markers: marked %d, %d rectangles (expected %d, 0)", c.edge, c.hudN, on);
        Check(c.edge == on && c.hudN == 0, line);
    }
    {
        const CopyDispCmd& c = Frame([] { Shape(202, 0.0f, 0.0f, 3.5f); }, true);
        std::snprintf(line, sizeof line, "a HUD shape over most of the picture: marked %d, %d rectangles (expected 0, 0)", c.edge, c.hudN);
        Check(!c.edge && c.hudN == 0, line);
    }
    {
        const CopyDispCmd& c = Frame([] {
            for (int i = 0; i < 24; ++i) Shape(300 + i, -2.6f + 0.9f * (i % 6), -2.2f + 1.3f * (i / 6), 0.05f);
        }, true);
        int held = 0;
        for (int i = 0; i < 24; ++i) {
            const int x = ScreenX(-2.6f + 0.9f * (i % 6)), y = ScreenY(-2.2f + 1.3f * (i / 6));
            for (int k = 0; k < c.hudN; ++k)
                if (Holds(c.hud[k], x, y)) { ++held; break; }
        }
        std::snprintf(line, sizeof line, "24 small HUD shapes: marked %d, %d rectangles holding %d of the 24 centres (expected %d, 1..16, %d)", c.edge, c.hudN, held, on, on ? 24 : 0);
        Check(on ? c.edge && c.hudN >= 1 && c.hudN <= 16 && held == 24 : !c.edge && c.hudN == 0, line);
    }
    {
        const CopyDispCmd& c = Frame([] {
            Shape(400, -1.5f, 1.0f, 0.4f);
            Copy(2, true); // (an offscreen picture: the HUD drawn into it is cleared away with it)
        }, true);
        std::snprintf(line, sizeof line, "a HUD cleared away by an EFB copy: marked %d, %d rectangles (expected %d, 0)", c.edge, c.hudN, on);
        Check(c.edge == on && c.hudN == 0, line);
    }
    uint64_t e[5] = {};
    EdgeCounters(e);
    std::printf("front-end counters: %llu copies, %llu marked, %llu HUD rectangles, %llu given up, %llu HUD draws measured\n", (unsigned long long)e[0],
                (unsigned long long)e[1], (unsigned long long)e[2], (unsigned long long)e[3], (unsigned long long)e[4]);
    Check(on ? e[0] == 6 && e[1] == 5 && e[3] == 1 : e[0] == 0 && e[1] == 0, "counters");
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    std::printf("%s (%u failed)\n", failures ? "SOME CHECKS FAILED" : "all checks passed", failures);
    return failures ? 1 : 0;
}
