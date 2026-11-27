// GXV_VTXPACK (cached display lists' static runs keep the GX source's compact fixed-point attributes; the 2^-k of each goes into the
// matrices that multiply it): the production GX front end (display-list cache included) and the production GLES back end on Mesa.
//
// Shapes in the game's vertex formats, all indexed (16-bit) through arrays in guest RAM: positions s16 >> 7, s8 >> 5, s16 whole numbers
// (with a matrix that scales them down), float with values that are no binary fraction (never packed); normals s16 >> 14, s8 >> 6;
// colour RGBA8; tex0 s16 >> 10, u16 >> 12, float; tex1 u8 >> 7, float binary fractions, s16 >> 3 / >> 15. They are drawn through a
// turning camera with materials that read every packed attribute every way the vertex shader can: lit (normals, a spot light), the
// material colour from the vertex, texgens from tex0 and tex1 through texture matrices, from the position (a, b, c, 1) projective, from
// the normal (a, b, 1, 1) with a normalised post matrix (dual texgen); rigid shapes (u_mv), two- and three-joint shapes (a matrix index
// per vertex: the small, short and full palettes), a shape with a texture-matrix index per vertex (never packed), lists with a
// triangle run and a line run (two runs; with absolute indices the list is then not packed at all), and small lists the CPU-transform
// path keeps in the frame's arena.
//   * the picture of every frame is read back and the hashes are folded together; the runner compares GXV_VTXPACK=1 with =0, bit for
//     bit, under the other front-end switches;
//   * the packing counters are printed: the runner checks that runs were packed and no matrix element was too small / no key refused.
// Usage: vgx_vtxpack_gl [frames]     (the switches are read from the environment)
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
alignas(4096) uint8_t ram[1 << 18];
unsigned failures = 0;
void Check(bool ok, const char* label) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
void BP(unsigned r, unsigned v) { WriteBP(r << 24 | v); }
void XF(unsigned r, unsigned v) { WriteXF(r, 1, &v); }
size_t g_records = 0, g_static = 0, g_packed = 0;
uint64_t g_recordSum = 0, g_packedSum = 0;
void Observe(CmdBuf& b) {
    g_records = b.draws.size();
    g_static = g_packed = 0;
    for (const DrawRec& d : b.draws) {
        g_static += d.vbuf != 0;
        g_packed += (d.layoutMask & VL_QALL) != 0;
    }
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
void Rows(unsigned base, unsigned row, const float* m, int n) {
    uint32_t w[16];
    for (int i = 0; i < n; ++i) w[i] = f2u(m[i]);
    WriteXF(base + row * 4, static_cast<uint32_t>(n), w);
}
void PosRows(unsigned row, const float m[12]) { Rows(0, row, m, 12); }
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
struct Cam { float ex, ey, ez, yaw, pitch; };
struct Placement { float cx, cy, cz, scale; };
// Position matrix rows `row`.. (and the normal rows of the same slot) = view * model of object `id` at p, the model scaled by `s2`
// beyond p.scale (the whole-number positions' shape: s2 = 1/64).
void LoadRows(unsigned row, int id, const Placement& p, const Cam& c, float s2 = 1.0f) {
    const float a = 0.37f * id + 0.1f, b = 0.23f * id - 0.3f;
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
    const float r[9] = {ca, -sa, 0, sa * cb, ca * cb, -sb, sa * sb, ca * sb, cb};
    const float cy = std::cos(c.yaw), sy = std::sin(c.yaw), cp = std::cos(c.pitch), sp = std::sin(c.pitch);
    const float v[9] = {cy, 0, -sy, sy * sp, cp, cy * sp, sy * cp, -sp, cy * cp};
    float vr[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) vr[i * 3 + j] = v[i * 3] * r[j] + v[i * 3 + 1] * r[3 + j] + v[i * 3 + 2] * r[6 + j];
    const float tx = p.cx - c.ex, ty = p.cy - c.ey, tz = p.cz - c.ez;
    const float s = p.scale * s2;
    float m[12];
    for (int i = 0; i < 3; ++i) {
        m[i * 4 + 0] = vr[i * 3 + 0] * s;
        m[i * 4 + 1] = vr[i * 3 + 1] * s;
        m[i * 4 + 2] = vr[i * 3 + 2] * s;
        m[i * 4 + 3] = v[i * 3] * tx + v[i * 3 + 1] * ty + v[i * 3 + 2] * tz;
    }
    PosRows(row, m);
    if (row % 3 == 0 && row / 3 < 10) NrmRows(row, vr);
}
void MatIndex(unsigned posRow) { XF(0x1018, (static_cast<unsigned>(60) << 6) | (57u << 12) | (54u << 18) | (51u << 24) | posRow); }
void Light() {
    XF(0x100A, 0x303030FFu);
    uint32_t w[16] = {};
    w[3] = 0xFFE0C0FFu;
    w[4] = f2u(1.0f);
    w[7] = f2u(1.0f);
    w[10] = f2u(0.3f); w[11] = f2u(0.4f); w[12] = f2u(3.0f);
    w[13] = f2u(0.0f); w[14] = f2u(0.0f); w[15] = f2u(-1.0f);
    WriteXF(0x600, 16, w);
    // light 1: a spot (cos attenuation), the second light of the lit channel
    uint32_t l[16] = {};
    l[3] = 0x80A0FFFFu;
    l[4] = f2u(-0.5f); l[5] = f2u(1.5f); l[6] = f2u(0.0f);
    l[7] = f2u(1.0f); l[8] = f2u(0.05f); l[9] = f2u(0.0f);
    l[10] = f2u(-1.0f); l[11] = f2u(1.0f); l[12] = f2u(1.0f);
    l[13] = f2u(0.6f); l[14] = f2u(-0.5f); l[15] = f2u(-0.6f);
    WriteXF(0x610, 16, l);
}
// Texture matrices: rows 60 (tex0's), 57 (tex1's), 54 (the projective one), 51 (the normal's); post matrix rows 0..2.
void TexMatrices(int f) {
    const float a = 0.05f * f;
    const float t60[12] = {0.75f * std::cos(a), -0.6f * std::sin(a), 0, 0.5f + 0.01f * f, 0.6f * std::sin(a), 0.75f * std::cos(a), 0, 0.25f, 0, 0, 1, 0};
    const float t57[12] = {1.3f, 0.2f, 0, -0.1f, -0.15f, 0.9f, 0, 0.3f, 0, 0, 1, 0};
    const float t54[12] = {0.4f, 0.1f, 0.3f, 0.5f, -0.2f, 0.45f, 0.1f, 0.5f, 0.05f, -0.03f, 0.2f, 1.0f};
    const float t51[12] = {0.5f, 0.1f, 0.2f, 0.5f, 0.0f, 0.5f, -0.3f, 0.5f, 0, 0, 1, 0};
    PosRows(60, t60); PosRows(57, t57); PosRows(54, t54); PosRows(51, t51);
    const float post[12] = {0.9f, 0.1f, 0.0f, 0.05f, -0.1f, 0.8f, 0.2f, 0.1f, 0.0f, 0.0f, 1.0f, 0.0f};
    Rows(0x500, 0, post, 12);
}
// Materials: 'L' lit with the material colour from the vertex (two lights, one a spot), tex0 through row 60 times tex1 through 57;
// 'E' lit, a projective texgen from the position (a, b, c, 1) through 54 and one from the normal through 51 and a normalised post matrix;
// 'U' unlit vertex colour times tex0 (row 60); 'P' unlit, texgen from the position (a, b, 1, 1).
void Material(char m) {
    const bool lit = m == 'L' || m == 'E' || m == 'C';
    const unsigned nTg = (m == 'C' || m == 'V') ? 0u : m == 'U' || m == 'P' ? 1u : 2u, nStages = nTg ? nTg : 1u;
    BP(0, nTg | (1u << 4) | ((nStages - 1) << 10));
    XF(0x1009, 1);
    XF(0x103F, nTg);
    XF(0x1012, m == 'E' ? 1u : 0u); // (dual texgen: the post matrices)
    if (m == 'L') { XF(0x1040, 5u << 7); XF(0x1041, 6u << 7); }
    else if (m == 'E') { XF(0x1040, (0u << 7) | (1u << 1) | (1u << 2)); XF(0x1041, 1u << 7); }
    else if (m == 'U') XF(0x1040, 5u << 7);
    else XF(0x1040, 0u << 7);
    XF(0x1050, 61); XF(0x1051, 0u | (1u << 8));
    XF(0x100C, m == 'E' ? 0xE0A0FF90u : 0xFFFFFFFFu);
    const unsigned matSrc = m == 'E' ? 0u : 1u;
    XF(0x100E, lit ? (matSrc | (1u << 1) | (1u << 2) | (1u << 3) | (2u << 7) | (1u << 9) | (1u << 10)) : matSrc);
    XF(0x1010, lit ? (matSrc | (1u << 1) | (1u << 2) | (2u << 7)) : matSrc);
    // stage 0: tex(coord 0) * ras ('C': ras alone); stage 1: prev * tex(coord 1)
    BP(0x28, (nTg ? (1u << 6) : 0u) | (0u << 3) | (nStages > 1 ? ((1u << 18) | (1u << 15)) : 0u));
    if (nTg) BP(0xC0, (15u << 12) | (8u << 8) | (10u << 4) | 15u | (1u << 19));
    else BP(0xC0, (10u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (5u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xC2, (15u << 12) | (0u << 8) | (8u << 4) | 15u | (1u << 19));
    BP(0xC3, (0u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0xF6, 0u | (1u << 2)); // (swap table 0 = r g b a: the colours, not their red alone)
    BP(0xF7, 2u | (3u << 2));
    BP(0x40, 1u | (3u << 1) | (1u << 4));
    BP(0x41, (1u << 3) | (1u << 4));
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
    t.obj.wrapS = t.obj.wrapT = 1;
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
void Put16(uint8_t* p, uint32_t v) { p[0] = static_cast<uint8_t>(v >> 8); p[1] = static_cast<uint8_t>(v); }
void Put32(uint8_t* p, uint32_t v) {
    for (unsigned i = 0; i != 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}
uint32_t g_ramTop = 0;
uint8_t* Take(uint32_t bytes) {
    uint8_t* p = ram + g_ramTop;
    g_ramTop = (g_ramTop + bytes + 63u) & ~63u;
    if (g_ramTop > sizeof ram) { std::fprintf(stderr, "guest RAM exhausted\n"); std::exit(2); }
    return p;
}

// ---- the vertex formats (VAT rows) and their component types
enum { U8 = 0, S8 = 1, U16 = 2, S16 = 3, F32 = 4 };
struct Fmt { int pos, posFrac, nrm, tex0, tex0Frac, tex1, tex1Frac; };
//                  pos       nrm  tex0         tex1
const Fmt kFmt[4] = {{S16, 7, S16, S16, 10, U8, 7},     // 0: the usual J3D shape
                     {S8, 5, S8, U16, 12, F32, 0},      // 1: small positions, s8 normals, float binary fractions for tex1
                     {F32, 0, S16, F32, 0, S16, 3},     // 2: float positions that are no binary fraction: positions and tex0 never packed
                     {S16, 0, S8, S16, 15, S16, 15}};   // 3: whole-number positions (k = 0), tiny texture coordinates
int Size(int type) { return type == F32 ? 4 : type >= U16 ? 2 : 1; }
void PutComp(uint8_t* p, int type, int frac, float v) {
    const double q = std::nearbyint(std::ldexp(static_cast<double>(v), frac));
    switch (type) {
    case U8: case S8: p[0] = static_cast<uint8_t>(static_cast<int>(q)); break;
    case U16: case S16: Put16(p, static_cast<uint32_t>(static_cast<int>(q)) & 0xFFFFu); break;
    default: Put32(p, f2u(v)); break;
    }
}

// ---- the lists: a bumpy patch of w x h vertices drawn as GX triangle strips of 3..8 vertices along its rows (the way J3D writes a
// shape), every attribute indexed with its own 16-bit index (the same number); options: a matrix index per vertex (joints), a
// texture-matrix index per vertex, a line strip at the end (a second run)
struct Mesh {
    int fmt = 0, n = 0;
    uint8_t* list = nullptr;
    uint32_t listBytes = 0;
    uint8_t *pos = nullptr, *nrm = nullptr, *clr = nullptr, *tex0 = nullptr, *tex1 = nullptr;
    uint32_t sPos = 0, sNrm = 0, sTex0 = 0, sTex1 = 0;
    int joints = 0; // 0: none; 2: rows 0 / 3; 3: rows 0 / 3 / 93 (the full palette)
    bool texIdx = false, lines = false;
};
void MakeMesh(Mesh& m, int fmt, int w, int h, int joints, bool texIdx, bool lines, unsigned seed) {
    const Fmt& F = kFmt[fmt];
    m.fmt = fmt;
    m.n = w * h;
    m.joints = joints;
    m.texIdx = texIdx;
    m.lines = lines;
    m.sPos = static_cast<uint32_t>(3 * Size(F.pos));
    m.sNrm = static_cast<uint32_t>(3 * Size(F.nrm));
    m.sTex0 = static_cast<uint32_t>(2 * Size(F.tex0));
    m.sTex1 = static_cast<uint32_t>(2 * Size(F.tex1));
    m.pos = Take(m.n * m.sPos); m.nrm = Take(m.n * m.sNrm); m.clr = Take(m.n * 4u); m.tex0 = Take(m.n * m.sTex0); m.tex1 = Take(m.n * m.sTex1);
    const float posScale = fmt == 3 ? 64.0f : 1.0f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            const float fx = -1.0f + 2.0f * x / (w - 1), fy = -1.0f + 2.0f * y / (h - 1);
            float z = 0.2f * std::sin(3.0f * fx + seed) * std::cos(2.0f * fy);
            float px = fx, py = fy;
            if (fmt == 2) { px = std::round(fx * 10.0f) * 0.1f; py = std::round(fy * 10.0f) * 0.1f; z = std::round(z * 10.0f) * 0.1f; } // (no binary fractions)
            const float P[3] = {px * posScale, py * posScale, z * posScale};
            for (int c = 0; c < 3; ++c) PutComp(m.pos + i * m.sPos + c * Size(F.pos), F.pos, F.posFrac, P[c]);
            const float nx = -0.6f * std::cos(3.0f * fx + seed) * std::cos(2.0f * fy), ny = 0.4f * std::sin(2.0f * fy), nz = 0.7f;
            const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
            const float N[3] = {nx / nl, ny / nl, nz / nl};
            const int nf = F.nrm == S8 ? 6 : 14;
            for (int c = 0; c < 3; ++c) PutComp(m.nrm + i * m.sNrm + c * Size(F.nrm), F.nrm, nf, N[c]);
            uint8_t* cl = m.clr + i * 4;
            cl[0] = static_cast<uint8_t>(128 + 120 * fx); cl[1] = static_cast<uint8_t>(128 + 120 * fy); cl[2] = static_cast<uint8_t>(200 - 40 * x % 160);
            cl[3] = 255;
            const float t0[2] = {fmt == 2 ? 0.3f * fx + 0.1f : (fmt == 3 ? 0.25f * fx : 1.5f * fx + 0.25f), fmt == 3 ? 0.25f * fy : 1.2f * fy};
            const float t1[2] = {0.5f + 0.45f * fx, 0.5f - 0.45f * fy};
            for (int c = 0; c < 2; ++c) {
                PutComp(m.tex0 + i * m.sTex0 + c * Size(F.tex0), F.tex0, F.tex0Frac, t0[c]);
                const float v1 = F.tex1 == F32 ? std::round(t1[c] * 256.0f) / 256.0f : (fmt == 3 ? 0.4f * t1[c] : t1[c]);
                PutComp(m.tex1 + i * m.sTex1 + c * Size(F.tex1), F.tex1, F.tex1Frac, v1);
            }
        }
    std::vector<uint8_t> l;
    unsigned r = 99 + seed;
    auto vertex = [&](unsigned ix) {
        if (joints) l.push_back(static_cast<uint8_t>(joints == 3 && ix % 5 == 0 ? 45 : (ix & 1) ? 0 : 3)); // PNMTXIDX
        if (texIdx) l.push_back(static_cast<uint8_t>(ix % 3 == 0 ? 57 : 60));                               // TEX0MTXIDX
        for (int a = 0; a < 5; ++a) { l.push_back(static_cast<uint8_t>(ix >> 8)); l.push_back(static_cast<uint8_t>(ix)); }
    };
    for (int y = 0; y + 1 < h; ++y) {
        std::vector<unsigned> row;
        for (int x = 0; x < w; ++x) { row.push_back(static_cast<unsigned>(y * w + x)); row.push_back(static_cast<unsigned>((y + 1) * w + x)); }
        for (size_t st = 0; st + 2 < row.size();) {
            r = r * 1103515245u + 12345u;
            size_t len = 3 + (r >> 8) % 6;
            if (st + len > row.size()) len = row.size() - st;
            l.push_back(static_cast<uint8_t>(0x98 | fmt));
            l.push_back(0);
            l.push_back(static_cast<uint8_t>(len));
            for (size_t k = 0; k < len; ++k) vertex(row[st + k]);
            st += len - 2;
        }
    }
    if (lines) {
        l.push_back(static_cast<uint8_t>(0xB0 | fmt)); // GX_LINESTRIP
        l.push_back(0);
        l.push_back(static_cast<uint8_t>(w));
        for (int x = 0; x < w; ++x) vertex(static_cast<unsigned>(x));
    }
    m.listBytes = static_cast<uint32_t>((l.size() + 31) & ~size_t(31));
    m.list = Take(m.listBytes);
    std::memset(m.list, 0, m.listBytes);
    std::memcpy(m.list, l.data(), l.size());
}
void DrawList(const Mesh& m) {
    SetVcd(0, m.joints ? 1 : 0);
    SetVcd(1, m.texIdx ? 1 : 0);
    g.arrays[9] = {m.pos, m.n * m.sPos, m.sPos, false};
    g.arrays[10] = {m.nrm, m.n * m.sNrm, m.sNrm, false};
    g.arrays[11] = {m.clr, m.n * 4u, 4, false};
    g.arrays[13] = {m.tex0, m.n * m.sTex0, m.sTex0, false};
    g.arrays[14] = {m.tex1, m.n * m.sTex1, m.sTex1, false};
    g.cpArrayStale = 0;
    CallList(m.list, m.listBytes);
    SetVcd(0, 0);
    SetVcd(1, 0);
}

struct Obj { char mat; int mesh; int id; Placement pl; };
} // namespace

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 24;
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(W, H), "production backend init");
    if (failures) return 1;
    ResetState(); g.efbW = W; g.efbH = H; QueueSetTarget(); BP(0x43, 1);
    const uint32_t proj[] = {f2u(1), 0, f2u(1), 0, f2u(-1), f2u(-1), 1};
    WriteXF(0x1020, 7, proj);
    Viewport(); Scissor(); Light(); TexMatrices(0);
    BP(0x4F, 30u | (255u << 8)); BP(0x50, (60u << 8) | 80u); BP(0x51, 0xFFFFFFu);
    for (int a : {9, 10, 11, 13, 14}) SetVcd(a, 3);
    for (int f = 0; f < 4; ++f) {
        const Fmt& F = kFmt[f];
        SetVat(f, 9, 1, F.pos, F.posFrac);
        SetVat(f, 10, 0, F.nrm, 0);
        SetVat(f, 11, 1, 5, 0);
        SetVat(f, 13, 1, F.tex0, F.tex0Frac);
        SetVat(f, 14, 1, F.tex1, F.tex1Frac);
    }
    // meshes: [fmt][size], joints, texture-matrix index, lines
    std::vector<Mesh> meshes(12);
    MakeMesh(meshes[0], 0, 12, 10, 0, false, false, 1);
    MakeMesh(meshes[1], 1, 10, 9, 0, false, false, 2);
    MakeMesh(meshes[2], 2, 11, 8, 0, false, false, 3);
    MakeMesh(meshes[3], 3, 9, 9, 0, false, false, 4);
    MakeMesh(meshes[4], 0, 10, 8, 2, false, false, 5);  // two joints (small palette)
    MakeMesh(meshes[5], 1, 9, 8, 3, false, false, 6);   // three joints, one row 45 (the full palette)
    MakeMesh(meshes[6], 0, 9, 7, 0, true, false, 7);    // texture-matrix index per vertex: never packed
    MakeMesh(meshes[7], 0, 10, 7, 0, false, true, 8);   // a triangle run and a line run
    MakeMesh(meshes[8], 3, 10, 7, 2, false, true, 9);   // the same with joints
    MakeMesh(meshes[9], 0, 3, 3, 0, false, false, 10);  // small: CPU-transformed (GXV_SOFTXFORM) or a static buffer
    MakeMesh(meshes[10], 1, 4, 3, 0, false, false, 11);
    MakeMesh(meshes[11], 2, 16, 12, 0, false, false, 12);
    hle::RegisterGuestRam(ram, sizeof ram);
    // a first picture: the texture
    const Cam still{0, 0, 0, 0, 0};
    BP(0x4F, 210u | (255u << 8)); BP(0x50, (190u << 8) | 140u); // (a bright clear for the texture's picture)
    Material('V'); BP(0x40, 1u | (7u << 1) | (1u << 4));
    Copy(3, true);
    Material('V');
    for (int i = 0; i < 6; ++i) {
        LoadRows(0, 100 + i, Placement{-0.6f + 0.25f * i, 0.4f - 0.15f * i, -1.3f, 0.3f}, still, meshes[i % 4].fmt == 3 ? 1.0f / 64.0f : 1.0f);
        MatIndex(0);
        DrawList(meshes[i % 4]);
    }
    BP(0x4F, 30u | (255u << 8)); BP(0x50, (60u << 8) | 80u);
    Copy(0, true);
    FrontPresent(false, nullptr);
    BindCopy(0);
    std::vector<Obj> scene;
    {
        unsigned r = 777;
        auto rnd = [&] { r = r * 1103515245u + 12345u; return ((r >> 8) & 0xFFFF) / 65535.0f; };
        const char mats[] = "LEUPLLEU";
        for (int i = 0; i < 40; ++i) {
            const float ang = 6.2831853f * rnd(), dist = 2.0f + 4.0f * rnd(), y = (rnd() - 0.5f) * 2.0f;
            scene.push_back(Obj{mats[i % 8], i % 12, i + 1, Placement{dist * std::sin(ang), y, -dist * std::cos(ang), 0.4f + 0.6f * rnd()}});
        }
    }
    if (const char* one = std::getenv("VTXPACK_ONE")) { // (debugging: one shape in front of the camera, material VTXPACK_MAT)
        const char* mat = std::getenv("VTXPACK_MAT");
        scene.assign(1, Obj{mat ? mat[0] : 'C', std::atoi(one), 1, Placement{0.0f, 0.0f, -2.5f, 1.0f}});
    }
    uint64_t hash = 14695981039346656037ull;
    std::vector<uint8_t> px;
    int detailed = 0;
    std::printf("records/static/packed/distinct colours per frame:");
    for (int f = 0; f < frames; ++f) {
        Perspective(f % 4 == 3 ? 0.7f : 1.1f, static_cast<float>(W) / H, 0.25f, 40.0f);
        TexMatrices(f);
        const Cam cam = std::getenv("VTXPACK_ONE") ? Cam{0, 0, 0, 0, 0} : Cam{0.02f * f, 0.0f, -0.03f * f, 0.21f * f, 0.25f * std::sin(0.5f * f)};
        char curMat = 0;
        for (const Obj& o : scene) {
            if (o.mat != curMat) { Material(o.mat); curMat = o.mat; }
            const Mesh& m = meshes[o.mesh];
            const float s2 = m.fmt == 3 ? 1.0f / 64.0f : 1.0f;
            LoadRows(0, o.id, o.pl, cam, s2);
            if (m.joints) {
                // (the joints: the same turn, moved and scaled a little against each other: a bent shape)
                LoadRows(3, o.id, Placement{o.pl.cx + 0.12f, o.pl.cy - 0.08f, o.pl.cz + 0.05f, o.pl.scale * 0.9f}, cam, s2);
                if (m.joints == 3) LoadRows(45, o.id, Placement{o.pl.cx - 0.1f, o.pl.cy + 0.1f, o.pl.cz, o.pl.scale * 1.1f}, cam, s2);
            }
            MatIndex(0);
            DrawList(m);
        }
        const uint32_t id = Copy(1, true);
        FrontPresent(false, nullptr);
        int pw = 0, ph = 0;
        if (!backend::DebugReadTexture(id, px, pw, ph) || pw != W || ph != H) { Check(false, "read the renderer's picture"); break; }
        hash = Fold(hash, px);
        std::printf(" %zu/%zu/%zu/%u", g_records, g_static, g_packed, Distinct(px));
        detailed += Distinct(px) > 40;
        g_recordSum += g_records;
        g_packedSum += g_packed;
        if (std::getenv("VTXPACK_PPM")) {
            char name[64];
            std::snprintf(name, sizeof name, "%s%02d.ppm", std::getenv("VTXPACK_PPM"), f);
            if (FILE* o = std::fopen(name, "wb")) {
                std::fprintf(o, "P6 %d %d 255\n", W, H);
                for (int i = 0; i < W * H; ++i) std::fwrite(&px[static_cast<size_t>(i) * 4], 1, 3, o);
                std::fclose(o);
            }
        }
    }
    std::printf("\n");
    uint64_t vp[7] = {}, vd[4] = {};
    VtxPackCounters(vp);
    VtxPackDrawCounters(vd);
    std::printf("packing: lists %llu, runs %llu of %llu (vertices %llu), buffer bytes %llu -> %llu, abs refusals %llu; records %llu, blocks scaled %llu, "
                "tiny %llu, refused %llu\n",
                (unsigned long long)vp[0], (unsigned long long)vp[1], (unsigned long long)vp[2], (unsigned long long)vp[3], (unsigned long long)vp[4],
                (unsigned long long)vp[5], (unsigned long long)vp[6], (unsigned long long)vd[0], (unsigned long long)vd[1], (unsigned long long)vd[2],
                (unsigned long long)vd[3]);
    Check(detailed * 2 >= frames, "most pictures have detail");
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    Check(vd[2] == 0 && vd[3] == 0, "no tiny matrix element, no refused key");
    std::printf("picture %dx%d hash %016llx\n", W, H, static_cast<unsigned long long>(hash));
    std::printf("records %llu\n", (unsigned long long)g_recordSum);
    std::printf("packedrecords %llu\n", (unsigned long long)g_packedSum);
    std::printf("packedruns %llu\n", (unsigned long long)vp[1]);
    return failures ? 1 : 0;
}
