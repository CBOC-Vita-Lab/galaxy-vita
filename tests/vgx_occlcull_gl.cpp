// GXV_OCCLCULL (draws hidden behind a closed occluder are not drawn) and GXV_GEOPASS (the census): the production GX front end
// (display-list cache included) and the production GLES back end on Mesa.
//
// A bumpy closed "planet" (an icosphere, 1280 triangles, from a display list in guest RAM: a static buffer from its second picture) is
// drawn first in each picture, with GX_CULL_BACK, followed by objects all around it: behind it (wholly hidden), half behind its rim,
// in front of it, beside it, INSIDE it between its inscribed ball and its surface, skinned (two joints) and CPU-transformed (dynamic)
// ones, in four materials (opaque depth-writing, depth-tested without writes and blended, textured from an EFB copy, no depth test).
// The camera orbits the planet, sometimes so close that the planet crosses the near plane, sometimes inside its bounding radius.
// Per picture also:
//   * a second planet wound the other way round (only its inner faces survive GX_CULL_BACK: it must not occlude: the objects inside it
//     show), a planet scaled unevenly, one drawn with an alpha test, an open bowl (not closed);
//   * on some pictures a copy that clears depth between the planet and the objects behind it, or a far "backdrop" draw with depth test
//     ALWAYS and writes over half the screen (the stored depth rises: the occluders must be forgotten), or objects drawn into a smaller
//     viewport with the same projection (another view: no occluder applies), or with the scissor cut down.
//   * the picture of every frame is read back and hashed; the runner compares across GXV_OCCLCULL 0 / 1 / 2 and other front-end switches.
// Usage: vgx_occlcull_gl [frames]     (switches from the environment)
#include "vgx_state.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"
#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void OcclCounters(uint64_t out[8]);
void CullCounters23(uint64_t out[8]);
}
namespace {
using namespace vgx;
constexpr int W = 256, H = 192;
alignas(32) unsigned char destinations[4][32];
alignas(4096) uint8_t ram[1 << 20];
unsigned failures = 0;
void Check(bool ok, const char* label) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
void BP(unsigned r, unsigned v) { WriteBP(r << 24 | v); }
// PORT_GX_MARK_PHASE as the game writes it with GXV_GEOPASS on (BP 0xFF, kind 11, the phase in bits 8-15): for the census only
void Phase(unsigned p) { WriteBP(0xFFu << 24 | (p << 8) | 11u); }
void XF(unsigned r, unsigned v) { WriteXF(r, 1, &v); }
size_t g_records = 0;
} // namespace
namespace vgx {
void SubmitToBackend(CmdBuf& b) {
    g_records = b.draws.size();
    backend::Execute(b);
}
} // namespace vgx
namespace {
void Viewport(float w = W, float h = H, float x = 0, float y = 0) {
    const uint32_t v[] = {f2u(w / 2.0f), f2u(-h / 2.0f), f2u(16777215.0f + 1), f2u(x + w / 2.0f + 342), f2u(y + h / 2.0f + 342), f2u(16777215.0f + 1)};
    WriteXF(0x101A, 6, v);
}
void Scissor(int x0 = 0, int y0 = 0, int x1 = W - 1, int y1 = H - 1) {
    BP(0x20, (static_cast<unsigned>(y0) + 342u) | ((static_cast<unsigned>(x0) + 342u) << 12));
    BP(0x21, (static_cast<unsigned>(y1) + 342u) | ((static_cast<unsigned>(x1) + 342u) << 12));
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
void Perspective(float fovY, float aspect, float n, float f) {
    const float t = 1.0f / std::tan(fovY * 0.5f);
    const uint32_t p[] = {f2u(t / aspect), 0, f2u(t), 0, f2u(-n / (f - n)), f2u(-(f * n) / (f - n)), 0};
    WriteXF(0x1020, 7, p);
}
// The camera: eye, yaw (about y), pitch (about x). view = R (p - eye).
struct Cam { float ex, ey, ez, yaw, pitch; };
// Position matrix slot `slot` = view * model, model = translation t * rotation(a, b) * scale (sx, sy, sz).
void LoadModel(unsigned slot, float a, float b, float sx, float sy, float sz, float tx, float ty, float tz, const Cam& c) {
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
    const float r[9] = {ca, -sa, 0, sa * cb, ca * cb, -sb, sa * sb, ca * sb, cb};
    const float cy = std::cos(c.yaw), sy0 = std::sin(c.yaw), cp = std::cos(c.pitch), sp = std::sin(c.pitch);
    const float v[9] = {cy, 0, -sy0, sy0 * sp, cp, cy * sp, sy0 * cp, -sp, cy * cp};
    float vr[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) vr[i * 3 + j] = v[i * 3] * r[j] + v[i * 3 + 1] * r[3 + j] + v[i * 3 + 2] * r[6 + j];
    const float dx = tx - c.ex, dy = ty - c.ey, dz = tz - c.ez;
    float m[12];
    const float s[3] = {sx, sy, sz};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m[i * 4 + j] = vr[i * 3 + j] * s[j];
        m[i * 4 + 3] = v[i * 3] * dx + v[i * 3 + 1] * dy + v[i * 3 + 2] * dz;
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
// Materials: A opaque depth-writing, B depth-tested without writes and blended, C textured (an EFB copy), D no depth test,
// P the planet (A, culling back faces), R the planet culling front faces (only its inner side drawn), Q the planet with an alpha test,
// Z a backdrop: depth test ALWAYS with writes, G depth test ALWAYS without writes (seen through everything).
void Material(char m) {
    const bool tex = m == 'C';
    const unsigned cull = (m == 'P' || m == 'Q') ? (1u << 14) : m == 'R' ? (2u << 14) : 0u; // (BP 0x00 bits 14-15: 1 cull back, 2 cull front)
    BP(0, (tex ? 1u : 0u) | (1u << 4) | cull);
    XF(0x1009, 1);
    XF(0x103F, tex ? 1u : 0u);
    XF(0x1040, 1u << 7);
    XF(0x100C, m == 'B' ? 0xE0A0FF90u : m == 'D' ? 0x40C0FFFFu : (m == 'P' || m == 'R') ? 0x60C060FFu : m == 'Q' ? 0xC08040FFu : m == 'Z' ? 0x2020A0FFu : 0xFFFFFFFFu);
    XF(0x100E, (m == 'D' || m == 'Z') ? 0u : ((1u << 1) | (1u << 2) | (2u << 7)));
    XF(0x1010, 0);
    BP(0x28, tex ? (1u << 6) : 0u);
    if (tex) BP(0xC0, (15u << 12) | (8u << 8) | (10u << 4) | 15u | (1u << 19));
    else BP(0xC0, (10u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (5u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    // (Q: alpha GREATER 0x20 AND ALWAYS: a test that can fail, so the shader discards)
    BP(0xF3, m == 'Q' ? (0x20u | (4u << 16) | (7u << 19)) : ((7u << 16) | (7u << 19)));
    BP(0x40, m == 'D' ? 0u : m == 'Z' ? (1u | (7u << 1) | (1u << 4)) : m == 'G' ? (1u | (7u << 1)) : (m == 'B' ? (1u | (3u << 1)) : (1u | (3u << 1) | (1u << 4))));
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
uint32_t g_ramTop = 0;
uint8_t* Take(uint32_t bytes) {
    uint8_t* p = ram + g_ramTop;
    g_ramTop = (g_ramTop + bytes + 63u) & ~63u;
    if (g_ramTop > sizeof ram) { std::fprintf(stderr, "test RAM exhausted\n"); std::exit(2); }
    return p;
}
struct Mesh {
    int verts = 0; // (the arrays' size, in vertices)
    uint8_t* list = nullptr;
    uint32_t listBytes = 0;
    uint8_t* pos = nullptr;
    uint8_t* nrm = nullptr;
};
void PutV(uint8_t* p, float x, float y, float z) { Put32(p, f2u(x)); Put32(p + 4, f2u(y)); Put32(p + 8, f2u(z)); }
// An indexed GX_TRIANGLES list (position and normal by the same 16-bit index), optionally with a matrix index per vertex.
void MakeList(Mesh& m, const std::vector<unsigned>& tri, const std::vector<unsigned char>* mtx = nullptr) {
    const uint32_t per = mtx ? 5u : 4u;
    m.listBytes = (3u + static_cast<uint32_t>(tri.size()) * per + 31u) & ~31u;
    m.list = Take(m.listBytes);
    std::memset(m.list, 0, m.listBytes);
    uint8_t* p = m.list;
    *p++ = 0x90;
    *p++ = static_cast<uint8_t>(tri.size() >> 8);
    *p++ = static_cast<uint8_t>(tri.size());
    for (unsigned ix : tri) {
        if (mtx) *p++ = (*mtx)[ix];
        *p++ = static_cast<uint8_t>(ix >> 8); *p++ = static_cast<uint8_t>(ix);
        *p++ = static_cast<uint8_t>(ix >> 8); *p++ = static_cast<uint8_t>(ix);
    }
}
// The planet: an icosphere subdivided `levels` times, pushed out by bumps; `flip` reverses every triangle's winding.
// `bowl`: only the triangles with y < 0.2 (an open surface).
void MakePlanet(Mesh& m, int levels, bool flip, bool bowl) {
    const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
    std::vector<float> v = {-1, t, 0, 1, t, 0, -1, -t, 0, 1, -t, 0, 0, -1, t, 0, 1, t, 0, -1, -t, 0, 1, -t, t, 0, -1, t, 0, 1, -t, 0, -1, -t, 0, 1};
    std::vector<unsigned> f = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                               3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
    for (size_t i = 0; i < v.size(); i += 3) {
        const float l = std::sqrt(v[i] * v[i] + v[i + 1] * v[i + 1] + v[i + 2] * v[i + 2]);
        v[i] /= l; v[i + 1] /= l; v[i + 2] /= l;
    }
    for (int lv = 0; lv < levels; ++lv) {
        std::map<std::pair<unsigned, unsigned>, unsigned> mid;
        auto midOf = [&](unsigned a, unsigned b) {
            const auto key = std::make_pair(std::min(a, b), std::max(a, b));
            auto it = mid.find(key);
            if (it != mid.end()) return it->second;
            float x = v[a * 3] + v[b * 3], y = v[a * 3 + 1] + v[b * 3 + 1], z = v[a * 3 + 2] + v[b * 3 + 2];
            const float l = std::sqrt(x * x + y * y + z * z);
            v.push_back(x / l); v.push_back(y / l); v.push_back(z / l);
            const unsigned id = static_cast<unsigned>(v.size() / 3 - 1);
            mid[key] = id;
            return id;
        };
        std::vector<unsigned> nf;
        for (size_t i = 0; i < f.size(); i += 3) {
            const unsigned a = f[i], b = f[i + 1], c = f[i + 2];
            const unsigned ab = midOf(a, b), bc = midOf(b, c), ca = midOf(c, a);
            const unsigned add[12] = {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca};
            nf.insert(nf.end(), add, add + 12);
        }
        f.swap(nf);
    }
    const int nv = static_cast<int>(v.size() / 3);
    m.verts = nv;
    m.pos = Take(static_cast<uint32_t>(nv) * 12);
    m.nrm = Take(static_cast<uint32_t>(nv) * 12);
    for (int i = 0; i < nv; ++i) {
        const float x = v[i * 3], y = v[i * 3 + 1], z = v[i * 3 + 2];
        const float r = 1.0f + 0.12f * std::sin(3.0f * x + 1.0f) * std::cos(2.0f * y) + 0.05f * std::sin(5.0f * z);
        PutV(m.pos + i * 12, x * r, y * r, z * r);
        PutV(m.nrm + i * 12, x, y, z);
    }
    std::vector<unsigned> tri;
    for (size_t i = 0; i < f.size(); i += 3) {
        if (bowl && (v[f[i] * 3 + 1] > 0.2f || v[f[i + 1] * 3 + 1] > 0.2f || v[f[i + 2] * 3 + 1] > 0.2f)) continue;
        // (the icosahedron's faces are wound with their right-hand normals outward)
        if (flip) { tri.push_back(f[i]); tri.push_back(f[i + 2]); tri.push_back(f[i + 1]); }
        else { tri.push_back(f[i]); tri.push_back(f[i + 1]); tri.push_back(f[i + 2]); }
    }
    MakeList(m, tri);
}
// A bumpy disc of `tris` triangles (the staticcull test's objects).
void MakeDisc(Mesh& m, int tris, bool skin) {
    const float pi = 3.14159265f;
    const int unique = tris + 1;
    m.verts = unique;
    m.pos = Take(static_cast<uint32_t>(unique) * 12);
    m.nrm = Take(static_cast<uint32_t>(unique) * 12);
    PutV(m.pos, 0.0f, 0.0f, 0.3f);
    PutV(m.nrm, 0.0f, 0.0f, 1.0f);
    std::vector<unsigned char> mtx(unique, 0);
    for (int i = 0; i < tris; ++i) {
        const float a = 2 * pi * i / tris;
        PutV(m.pos + (1 + i) * 12, std::cos(a), std::sin(a), 0.25f * std::cos(3 * a));
        PutV(m.nrm + (1 + i) * 12, 0.5f * std::cos(a), 0.5f * std::sin(a), 0.8f);
        mtx[1 + i] = static_cast<unsigned char>(((1 + i) & 1) ? 0 : 3);
    }
    std::vector<unsigned> tri;
    for (int i = 0; i < tris; ++i) { tri.push_back(0); tri.push_back(1 + i); tri.push_back(1 + (i + 1) % tris); }
    MakeList(m, tri, skin ? &mtx : nullptr);
}
void DrawList(const Mesh& m) {
    const uint32_t bytes = static_cast<uint32_t>(m.verts) * 12;
    g.arrays[9] = {m.pos, bytes, 12, false};
    g.arrays[10] = {m.nrm, bytes, 12, false};
    g.cpArrayStale = 0;
    CallList(m.list, m.listBytes);
}
Mesh g_planet, g_inverted, g_bowl, g_disc[4], g_skin[2];
struct Obj { char mat; int kind; float a, b, x, y, z, s; };
std::vector<Obj> Scene() {
    std::vector<Obj> s;
    unsigned r = 4242;
    auto rnd = [&] { r = r * 1103515245u + 12345u; return ((r >> 8) & 0xFFFF) / 65535.0f; };
    const char mats[] = "AAACABAGCDAAAB";
    for (int i = 0; i < 160; ++i) {
        // around the planet (radius ~3 at the origin): mostly on a shell of 3.4 .. 7, some inside it (1.8 .. 2.6, between the ball and
        // the surface: hidden by the surface, not by the ball), some close to its surface
        const float u = 6.2831853f * rnd(), w = std::acos(2.0f * rnd() - 1.0f);
        const float d = i % 9 == 0 ? 1.8f + 0.8f * rnd() : (i % 5 == 0 ? 3.1f + 0.4f * rnd() : 3.4f + 3.6f * rnd());
        s.push_back(Obj{mats[i % 14], i % 6 == 5 ? 3 : (i % 13 == 12 ? 2 : i % 2), 0.37f * i, 0.23f * i - 0.3f, d * std::sin(w) * std::cos(u), d * std::cos(w),
                        d * std::sin(w) * std::sin(u), 0.15f + 0.35f * rnd()});
    }
    // shells inside the planets, between the inscribed ball and the surface: inside the main planet (hidden by its surface) and inside
    // the inverted one (visible through it: only its inner faces are drawn)
    for (int i = 0; i < 40; ++i) {
        const float u = 6.2831853f * rnd(), w = std::acos(2.0f * rnd() - 1.0f);
        const bool inv = i % 2 == 1;
        const float d = inv ? 1.85f : 2.75f, cx = inv ? 9.0f : 0.0f, cy = inv ? 1.0f : 0.0f, cz = inv ? -6.0f : 0.0f;
        s.push_back(Obj{'A', i % 3 == 2 ? 3 : 0, 0.5f * i, 0.1f * i, cx + d * std::sin(w) * std::cos(u), cy + d * std::cos(w), cz + d * std::sin(w) * std::sin(u), 0.12f});
    }
    return s;
}
Cam CameraAt(int f) {
    // orbiting the planet at 9 .. 14, looking at it; frames 20-23 come close (the planet then crosses the near plane, or the eye is inside
    // its bounding radius), frames 24-27 look past it
    const float ang = 0.31f * f, el = 0.4f * std::sin(0.23f * f);
    float dist = 9.0f + 5.0f * (0.5f + 0.5f * std::sin(0.17f * f));
    if (f >= 20 && f < 24) dist = 3.2f + 0.25f * (f - 20);
    Cam c;
    c.ex = dist * std::cos(el) * std::sin(ang);
    c.ey = dist * std::sin(el);
    c.ez = dist * std::cos(el) * std::cos(ang);
    // looking at the origin: view forward = -z; yaw / pitch so that R (0 - eye) points down -z
    c.yaw = std::atan2(c.ex, c.ez);
    c.pitch = -std::atan2(c.ey, std::sqrt(c.ex * c.ex + c.ez * c.ez));
    if (f >= 24 && f < 28) c.yaw += 0.5f;
    return c;
}
void DrawObjects(const std::vector<Obj>& scene, const Cam& cam, int part, int parts) {
    char cur = 0;
    for (size_t i = 0; i < scene.size(); ++i) {
        if (static_cast<int>(i % parts) != part) continue;
        const Obj& o = scene[i];
        if (o.mat != cur) { Material(o.mat); cur = o.mat; }
        LoadModel(i % 3 == 2 ? 1 : 0, o.a, o.b, o.s, o.s, o.s, o.x, o.y, o.z, cam);
        DrawList(g_disc[o.kind]);
    }
}
} // namespace

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 32;
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(W, H), "production backend init");
    if (failures) return 1;
    ResetState(); g.efbW = W; g.efbH = H; QueueSetTarget(); BP(0x43, 1);
    Viewport(); Scissor(); Light();
    const float tm[12] = {0.5f, 0, 0, 0.5f, 0, 0.5f, 0, 0.5f, 0, 0, 0, 1};
    PosRows(60, tm);
    BP(0x4F, 30u | (255u << 8)); BP(0x50, (60u << 8) | 80u); BP(0x51, 0xFFFFFFu);
    SetVcd(9, 3);
    SetVcd(10, 3);
    SetVat(0, 9, 1, 4, 0);
    SetVat(0, 10, 0, 4, 0);
    // (GX front faces are clockwise on screen: a model whose outer side GX_CULL_BACK keeps has its right-hand normals pointing in, as
    // J3D's models have; the icosahedron below is wound the other way, so the planet is the flipped one)
    MakePlanet(g_planet, 3, true, false);
    MakePlanet(g_inverted, 3, false, false);
    MakePlanet(g_bowl, 3, true, true);
    MakeDisc(g_disc[0], 44, false);
    MakeDisc(g_disc[1], 70, false);
    MakeDisc(g_disc[2], 400, false);
    MakeDisc(g_disc[3], 8, false);
    MakeDisc(g_skin[0], 44, true);
    MakeDisc(g_skin[1], 70, true);
    hle::RegisterGuestRam(ram, sizeof ram);
    const Cam still{0, 0, 6, 0, 0};
    Perspective(1.0f, static_cast<float>(W) / H, 0.25f, 60.0f);
    Material('A');
    Copy(3, true);
    for (int i = 0; i < 6; ++i) {
        Material(i & 1 ? 'A' : 'D');
        LoadModel(0, 0.3f * i, 0.1f * i, 0.6f, 0.6f, 0.6f, -1.5f + 0.6f * i, 0.5f - 0.2f * i, 0.0f, still);
        DrawList(g_disc[i & 1]);
    }
    Copy(0, true);
    FrontPresent(false, nullptr);
    BindCopy(0);
    const std::vector<Obj> scene = Scene();
    uint64_t hash = 14695981039346656037ull;
    std::vector<uint8_t> px;
    int detailed = 0;
    uint64_t recordSum = 0;
    std::vector<uint64_t> frameHashes; // (each picture's own: the read-back must follow the camera, not hold a stale copy)
    std::printf("records/distinct colours per frame:");
    for (int f = 0; f < frames; ++f) {
        Perspective(f % 4 == 3 ? 0.7f : 1.1f, static_cast<float>(W) / H, f % 7 == 6 ? 1.0f : 0.25f, 60.0f);
        Viewport();
        Scissor();
        const Cam cam = CameraAt(f);
        // the planets: the closed one at the origin (scale 3, turning), the inverted one and an uneven one off to the sides
        Phase(2);
        // (every fourth picture the main planet culls its front faces: only its inside is drawn, and what is inside it shows)
        Material(f % 4 == 1 ? 'R' : 'P');
        LoadModel(0, 0.05f * f, 0.2f, 3.0f, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, cam);
        DrawList(g_planet);
        LoadModel(1, 0.1f, 0.03f * f, 2.0f, 2.0f, 2.0f, 9.0f, 1.0f, -6.0f, cam);
        DrawList(g_inverted);
        LoadModel(0, 0.2f, 0.1f, 2.5f, 1.5f, 2.5f, -9.0f, -1.0f, 5.0f, cam);
        DrawList(g_planet); // (uneven scale: refused)
        LoadModel(1, 0.4f + 0.1f * f, 0.3f, 2.0f, 2.0f, 2.0f, -6.0f, 4.0f, -9.0f, cam);
        DrawList(g_bowl);
        Material('Q');
        LoadModel(0, 0.2f, 0.1f, 2.0f, 2.0f, 2.0f, 6.0f, -5.0f, 8.0f, cam);
        DrawList(g_planet); // (alpha test: refused)
        // the objects, in parts, with the events between the parts
        Phase(3);
        DrawObjects(scene, cam, 0, 4);
        if (f % 5 == 1) {
            // a copy that clears depth (and colour) mid-picture: the objects after it are all visible again
            Material('A');
            Copy(2, true);
        }
        if (f % 5 == 2) {
            // a far backdrop with depth ALWAYS and writes over the lower half: depth rises there
            Material('Z');
            LoadModel(0, 0.0f, 0.0f, 30.0f, 30.0f, 30.0f, 0.0f, 0.0f, 0.0f, Cam{0, 0, 0, 0, 0});
            const float m[12] = {40, 0, 0, 0, 0, 20, 0, -18, 0, 0, 1, -50};
            PosRows(0, m);
            XF(0x1018, 0x3CF3CF00u & ~0x3Fu);
            DrawList(g_disc[1]);
        }
        DrawObjects(scene, cam, 1, 4);
        // objects inside the main planet straight behind its centre (beyond its inscribed ball, mostly inside its surface): hidden when the
        // planet culls its back faces, seen through its inside when it culls its front faces
        {
            float dx = -cam.ex, dy = -cam.ey, dz = -cam.ez;
            const float l = std::sqrt(dx * dx + dy * dy + dz * dz);
            dx /= l; dy /= l; dz /= l;
            float ux = dz, uy = 0.0f, uz = -dx; // (across the line of sight)
            const float ul = std::sqrt(ux * ux + uz * uz) + 1e-6f;
            ux /= ul; uz /= ul;
            const float vx = dy * uz - dz * uy, vy = dz * ux - dx * uz, vz = dx * uy - dy * ux;
            Material('A');
            for (int i = 0; i < 6; ++i) {
                const float a = 0.35f * static_cast<float>((i % 3) - 1), b = 0.35f * static_cast<float>(i / 3) - 0.17f, d = 2.6f + 0.1f * (i & 1);
                LoadModel(i & 1, 0.4f * i, 0.2f * f, 0.1f, 0.1f, 0.1f, d * dx + a * ux + b * vx, d * dy + a * uy + b * vy, d * dz + a * uz + b * vz, cam);
                DrawList(g_disc[i % 3 == 2 ? 3 : 0]);
            }
        }
        // skinned objects behind and around the planet (two joints: rows 0..2 and 3..5)
        Phase(7);
        SetVcd(0, 1);
        Material('A');
        for (size_t i = 0; i < scene.size(); i += 5) {
            const Obj& o = scene[i];
            LoadModel(0, o.a, o.b, o.s, o.s, o.s, o.x, o.y, o.z, cam);
            LoadModel(1, o.a + 0.3f, o.b, o.s * 0.8f, o.s * 0.8f, o.s * 0.8f, o.x * 1.05f + (i % 15 == 0 ? 1.5f : 0.1f), o.y + 0.1f, o.z * 1.05f, cam);
            DrawList(g_skin[i & 1]);
        }
        SetVcd(0, 0);
        if (f % 3 == 0) {
            // another view: a smaller viewport in the top left corner, the same projection: no occluder of the full view applies
            Viewport(W * 0.5f, H * 0.5f, 0.0f, 0.0f);
            DrawObjects(scene, cam, 2, 4);
            Viewport();
        } else if (f % 3 == 1) {
            // the scissor cut down: another view as well
            Scissor(8, 8, W - 9, H - 9);
            DrawObjects(scene, cam, 2, 4);
            Scissor();
        } else {
            DrawObjects(scene, cam, 2, 4);
        }
        Phase(15);
        DrawObjects(scene, cam, 3, 4);
        const uint32_t id = Copy(1, true);
        FrontPresent(false, nullptr);
        int pw = 0, ph = 0;
        if (!backend::DebugReadTexture(id, px, pw, ph) || pw != W || ph != H) { Check(false, "read the renderer's picture"); break; }
        hash = Fold(hash, px);
        frameHashes.push_back(Fold(14695981039346656037ull, px));
        std::printf(" %zu/%u", g_records, Distinct(px));
        detailed += Distinct(px) > 40;
        recordSum += g_records;
        if (std::getenv("OCCL_PPM")) {
            char name[96];
            std::snprintf(name, sizeof name, "%s%02d.ppm", std::getenv("OCCL_PPM"), f);
            if (FILE* o = std::fopen(name, "wb")) {
                std::fprintf(o, "P6 %d %d 255\n", W, H);
                for (int i = 0; i < W * H; ++i) std::fwrite(&px[static_cast<size_t>(i) * 4], 1, 3, o);
                std::fclose(o);
            }
        }
    }
    std::printf("\n");
    uint64_t oc[8] = {}, c23[8] = {};
    OcclCounters(oc);
    CullCounters23(c23);
    std::printf("occluders registered %llu; left out: static runs %llu (indices %llu), dynamic draws %llu (indices %llu); runs analysed %llu, closed %llu; "
                "epochs ended %llu\n",
                (unsigned long long)oc[0], (unsigned long long)oc[1], (unsigned long long)oc[2], (unsigned long long)oc[3], (unsigned long long)oc[4],
                (unsigned long long)oc[5], (unsigned long long)oc[6], (unsigned long long)oc[7]);
    Check(detailed * 2 >= frames, "most pictures have detail");
    {
        std::vector<uint64_t> u = frameHashes;
        std::sort(u.begin(), u.end());
        const size_t distinct = static_cast<size_t>(std::unique(u.begin(), u.end()) - u.begin());
        Check(distinct * 4 >= frameHashes.size() * 3, "the pictures differ from frame to frame (the read-back is not a stale copy)");
    }
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    std::printf("picture %dx%d hash %016llx\n", W, H, static_cast<unsigned long long>(hash));
    std::printf("records %llu\n", (unsigned long long)recordSum);
    std::printf("registered %llu\n", (unsigned long long)oc[0]);
    std::printf("occlstatic %llu\n", (unsigned long long)oc[1]);
    std::printf("occldyn %llu\n", (unsigned long long)oc[3]);
    std::printf("closed %llu\n", (unsigned long long)oc[6]);
    std::printf("breaks %llu\n", (unsigned long long)oc[7]);
    return failures ? 1 : 0;
}
