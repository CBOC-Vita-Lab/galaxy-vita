// GXV_PALROWS: the small matrix palette, through the production front end and the production GLES back end (Mesa).
//
// A skinned mesh (every vertex names its matrix: PNMTXIDX in the position's w) with a lit colour channel, so that both the
// position rows and the normal rows of the palette are read, is drawn in several groups whose highest matrix row differs
// (9, 12, 3, 0, 30, and 9 again with the registers unchanged). The picture is read back from the renderer and hashed.
//   * GXV_PALROWS=0 / unset / 2 (census only) / invalid: every palette draw uses the short (60 vec4) or full (96) palette as before;
//   * GXV_PALROWS=1: the draws whose highest row is at most 9 use the small palette (24 vec4), the others the same as before;
//   * the hash of the picture is printed; the runner compares it across the values of the switch and of the other front-end
//     switches (identical, bit for bit).
// Usage: vgx_palrows_gl [expected-mode: 0 off, 1 small palette on, 2 uniform packing off (GXV_UNIPACK=0: every palette is the full one)]
#include "vgx_state.h"
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
unsigned failures = 0;
void Check(bool ok, const char* label) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
void BP(unsigned r, unsigned v) { WriteBP(r << 24 | v); }
void XF(unsigned r, unsigned v) { WriteXF(r, 1, &v); }

// ---- what the back end is given: the palette size each draw's vertex shader declares, in order
std::map<uint16_t, const ShaderSource*> g_vs;
std::vector<int> g_pal;      // nPal of every draw that reads the palette
std::vector<int> g_palBlock; // floats of the palette block it refers to (from the arena offsets of the next palette draw)
void Observe(CmdBuf& b) {
    for (const Cmd& c : b.cmds)
        if (c.type == CMD_SHADER && c.arg < b.shaders.size() && !b.shaders[c.arg]->fragment) g_vs[b.shaders[c.arg]->id] = b.shaders[c.arg];
    for (const DrawRec& d : b.draws) {
        auto it = g_vs.find(d.vsId);
        if (it == g_vs.end() || !it->second->usesPalette) continue;
        g_pal.push_back(it->second->nPal);
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
void Viewport() {
    const uint32_t v[] = {f2u(W / 2.0f), f2u(-H / 2.0f), f2u(16777215.0f + 1), f2u(W / 2.0f + 342), f2u(H / 2.0f + 342), f2u(16777215.0f + 1)};
    WriteXF(0x101A, 6, v);
}
void Scissor() {
    BP(0x20, 342u | (342u << 12));
    BP(0x21, (H - 1 + 342u) | ((W - 1 + 342u) << 12));
    BP(0x59, 171u | (171u << 10));
}
void Row3(unsigned row, const float m[12]) {
    uint32_t w[12];
    for (int i = 0; i < 12; ++i) w[i] = f2u(m[i]);
    WriteXF(row * 4, 12, w);
}
void NrmRow3(unsigned row, const float m[9]) {
    uint32_t w[9];
    for (int i = 0; i < 9; ++i) w[i] = f2u(m[i]);
    WriteXF(0x400 + row * 3, 9, w);
}
// Matrix at row `row`: a rotation about z by `a` and about x by `b`, then a translation (tx, ty).
void Joint(unsigned row, float a, float b, float tx, float ty) {
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
    // R = Rx(b) * Rz(a)
    const float r[9] = {ca, -sa, 0, sa * cb, ca * cb, -sb, sa * sb, ca * sb, cb};
    const float m[12] = {r[0], r[1], r[2], tx, r[3], r[4], r[5], ty, r[6], r[7], r[8], 0};
    Row3(row, m);
    NrmRow3(row, r);
}
void Light() {
    XF(0x1009, 1);                      // one colour channel
    XF(0x100A, 0x303030FFu);            // ambient
    XF(0x100C, 0xFFFFFFFFu);            // material
    XF(0x100E, (1u << 1) | (1u << 2) | (2u << 7)); // lit, light 0, clamped diffuse
    XF(0x1010, 0);                      // alpha channel: material
    uint32_t w[16] = {};
    w[3] = 0xFFE0C0FFu;                 // colour
    w[4] = f2u(1.0f);                   // (cosine / distance attenuation: unused with attn off)
    w[7] = f2u(1.0f);
    w[10] = f2u(0.3f); w[11] = f2u(0.4f); w[12] = f2u(3.0f); // position
    WriteXF(0x600, 16, w);
}
void Pass() {
    BP(0, 1u << 4); // one colour channel, no texgens, one stage, no culling
    XF(0x103F, 0);
    BP(0x28, 0);    // stage 0: colour channel 0
    BP(0xC0, (10u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19)); // rasterised colour
    BP(0xC1, (5u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19)); // rasterised alpha
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, 0); BP(0x41, (1u << 3) | (1u << 4)); BP(0x42, 0); // no depth test, colour and alpha written
}
// A fan of `rows` slots: triangle k (k = 0..n-1) uses the matrix at row 3*k (or the rows in `idx`); the triangles are arranged
// around the origin so that each matrix moves its own to a different place on the screen.
struct Vtx { float x, y, z, idx, nx, ny, nz; };
void Skinned(const std::vector<int>& idx, float dx, float dy) {
    std::vector<Vtx> mesh;
    int mx = 0;
    for (int r : idx) mx = std::max(mx, r);
    SetPendingPosMtxMax(static_cast<uint32_t>(mx)); // (what the vertex decoder reports for the draw: the highest PNMTXIDX)
    for (size_t k = 0; k < idx.size(); ++k) {
        const float s = 0.16f;
        const float cx = dx, cy = dy;
        // three vertices of a triangle in model space; the joint matrix places it. The normals differ per vertex.
        const float z = -0.5f;
        const Vtx t[3] = {{cx - s, cy - s, z, static_cast<float>(idx[k]), -0.3f, -0.3f, 0.9f},
                          {cx + s, cy - s, z, static_cast<float>(idx[k]), 0.4f, -0.2f, 0.85f},
                          {cx, cy + s, z, static_cast<float>(idx[k]), 0.0f, 0.5f, 0.85f}};
        for (const Vtx& v : t) mesh.push_back(v);
    }
    DrawDecoded(0x90, VL_NRM, reinterpret_cast<const uint8_t*>(mesh.data()), static_cast<uint32_t>(mesh.size()));
}
uint32_t Copy(int index) {
    g.texCopySrc = EfbRect{0, 0, W, H};
    g.texCopyDstW = W; g.texCopyDstH = H; g.texCopyFmt = 6;
    QueueCopyTex(destinations[index], false);
    return Pending().copyTex.back().texId;
}
uint64_t Hash(const std::vector<uint8_t>& p) {
    uint64_t h = 14695981039346656037ull;
    for (uint8_t b : p) h = (h ^ b) * 1099511628211ull;
    return h;
}
unsigned Distinct(const std::vector<uint8_t>& p) {
    std::map<uint32_t, int> seen;
    for (size_t i = 0; i + 3 < p.size(); i += 4) seen[p[i] | p[i + 1] << 8 | p[i + 2] << 16]++;
    return static_cast<unsigned>(seen.size());
}
} // namespace

int main(int argc, char** argv) {
    const int expect = argc > 1 ? std::atoi(argv[1]) : 0;
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(W, H), "production backend init");
    if (failures) return 1;
    ResetState(); g.efbW = W; g.efbH = H; QueueSetTarget(); BP(0x43, 1);
    const uint32_t proj[] = {f2u(1), 0, f2u(1), 0, f2u(-1), f2u(-1), 1};
    WriteXF(0x1020, 7, proj);
    Viewport(); Scissor(); Pass(); Light();
    SetVcd(0, 1); // PNMTXIDX per vertex

    // Matrices at rows 0, 3, ..., 30 (rotations and translations, all different).
    for (unsigned k = 0; k <= 10; ++k) Joint(k * 3, 0.2f * k - 0.5f, 0.15f * k - 0.4f, 0.05f * k - 0.25f, 0.04f * k - 0.2f);
    // Clear to a background, then the groups (each at its own place; registers do not change between them).
    BP(0xE2, 20u | (30u << 12)); BP(0xE3, 40u | (255u << 12)); // (unused by the raster stage; kept for symmetry with other tests)
    struct Group { std::vector<int> rows; float dx, dy; };
    const Group groups[] = {
        {{0, 3, 6, 9}, -0.55f, 0.45f},        // highest 9: small palette with GXV_PALROWS=1
        {{0, 3, 6, 9, 12}, 0.0f, 0.45f},      // highest 12: short palette
        {{0, 3}, 0.55f, 0.45f},               // highest 3: small
        {{0}, -0.55f, 0.0f},                  // highest 0: small
        {{0, 9, 30}, 0.0f, 0.0f},             // highest 30: full palette
        {{3, 6, 9}, 0.55f, 0.0f},             // highest 9 again, same registers as group 1's: small
        {{27}, -0.55f, -0.45f},               // highest 27: short (the limit of the short palette)
        {{9, 6}, 0.0f, -0.45f},               // highest 9, order reversed
        {{0, 3, 6, 9}, 0.55f, -0.45f},        // the registers of group 1 once more, in a different place
    };
    std::vector<int> expectNormal, expectSmall, expectUnpacked;
    for (const Group& gr : groups) {
        int mx = 0;
        for (int r : gr.rows) mx = std::max(mx, r);
        Skinned(gr.rows, gr.dx, gr.dy);
        const int shortRows = 60, fullRows = 96, smallRows = 24;
        expectNormal.push_back(mx > 27 ? fullRows : shortRows);
        expectSmall.push_back(mx > 27 ? fullRows : mx <= 9 ? smallRows : shortRows);
        expectUnpacked.push_back(fullRows);
    }
    FrontPresent(false, nullptr);
    // (the draws may have merged or split; compare the sequence of distinct palette sizes in order)
    const uint32_t id = Copy(0);
    FrontPresent(false, nullptr);
    std::vector<uint8_t> px;
    int pw = 0, ph = 0;
    Check(backend::DebugReadTexture(id, px, pw, ph), "read the renderer's picture");
    Check(pw == W && ph == H, "picture size");
    std::printf("palette draws: %zu; sizes:", g_pal.size());
    for (int n : g_pal) std::printf(" %d", n);
    std::printf("\n");
    // Groups with the same palette size and registers are drawn as one record: compare the runs of equal sizes.
    auto runs = [](const std::vector<int>& v) {
        std::vector<int> r;
        for (int n : v) if (r.empty() || r.back() != n) r.push_back(n);
        return r;
    };
    const std::vector<int> want = runs(expect == 1 ? expectSmall : expect == 2 ? expectUnpacked : expectNormal), got = runs(g_pal);
    std::printf("expected runs:");
    for (int n : want) std::printf(" %d", n);
    std::printf("\n");
    const bool sizesOk = got == want;
    Check(sizesOk, expect == 1 ? "palette sizes: small (24) up to row 9, short (60) up to 27, full (96) above"
                  : expect == 2 ? "palette sizes: all full (96) without uniform packing" : "palette sizes: short (60) up to row 27, full (96) above");
    Check(Distinct(px) > 40, "the picture has detail (lit, shaded triangles)");
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    std::printf("picture %dx%d distinct colours %u hash %016llx\n", pw, ph, Distinct(px), static_cast<unsigned long long>(Hash(px)));
    return failures ? 1 : 0;
}
