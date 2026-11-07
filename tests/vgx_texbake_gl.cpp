// GXV_TEXBAKE: the texture matrix of a CPU-transformed draw is applied to its texture coordinates by the front end, and the block holds
// identity rows, so that particles which differ in their texture matrix (JPA's per-particle texture animation) merge into one draw;
// the production GX front end and the production GLES back end on Mesa.
//
// Particles are drawn as the game draws them (JPADrawBillboard: a quad, its own position matrix in row 0, its own TEXMTX0 (row 30) for
// a 2x4 texgen from TEX0, its own TEV colour register 0 (register folding), additive or alpha blended), in emitters with:
//   A  a texture matrix of its own per particle (rotation, scale, scroll: the JPALoadCalcTexCrdMtxAnm case), additive;
//   B  the identity texture matrix (row 60), alpha blended;
//   P  as A, with a post-transform matrix (dual texgen) after the texture matrix;
//   S  two texgens that read TEX0 (JPAExTexShape's second coordinate): not baked;
//   T  as A, with the texture changing from particle to particle (JPALoadTexAnm per particle): baked, but no merge (another texture);
//   L  48 vertices a primitive: above GXV_SOFTXFORM=32 (the console's limit) transformed by the vertex shader and not baked; with
//      the host default 128 CPU-transformed and baked like A;
//   M  a texgen that reads TEX1, which the vertices lack: drawn with the vertex shader's transform (the fallback), picture unchanged.
// TB_MODE=exact: every matrix entry and coordinate is a short binary fraction, so that every product and sum is exact and the picture
// must be bit for bit the one without the switch. TB_MODE=random: arbitrary angles and scales (the CPU's and a GPU's dot products
// may round differently in the last place; on Mesa they do not: the front end sums in llvmpipe's order).
// Printed: the picture hash, the number of GL draws (records), the front end's bake counters.
// Usage: vgx_texbake_gl     (the switches are read from the environment; TB_SEED=<n> another scene; TB_RECS=1 lists the records)
#include "vgx_state.h"
#include <GLES2/gl2.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vgx {
bool LogEnabled() { return std::getenv("TB_LOG") != nullptr; }
void TexBakeCounters(uint64_t out[4]);
}
namespace {
using namespace vgx;
constexpr int W = 128, H = 96;
alignas(32) unsigned char destinations[2][32];
unsigned failures = 0;
void Check(bool ok, const char* label) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
void BP(unsigned r, unsigned v) { WriteBP(r << 24 | v); }
void XF(unsigned r, unsigned v) { WriteXF(r, 1, &v); }
void Rows(unsigned addr, const float* m, int n) {
    uint32_t w[12];
    for (int i = 0; i < n; ++i) w[i] = f2u(m[i]);
    WriteXF(addr, static_cast<uint32_t>(n), w);
}
void Viewport() {
    const uint32_t v[] = {f2u(W / 2.0f), f2u(-H / 2.0f), f2u(16777215.0f + 1), f2u(W / 2.0f + 342), f2u(H / 2.0f + 342), f2u(16777215.0f + 1)};
    WriteXF(0x101A, 6, v);
}
void Scissor() {
    BP(0x20, 342u | (342u << 12));
    BP(0x21, (H - 1 + 342u) | ((W - 1 + 342u) << 12));
    BP(0x59, 171u | (171u << 10));
}
// Two 16x16 RGBA8 textures of noise (GX tiled; any bytes are a valid picture), repeat wrap, linear filtering.
alignas(32) uint8_t g_tex[2][16 * 16 * 4];
void BindTex(int which) {
    auto& t = g.tex[0];
    t = TexSlot{};
    t.valid = true;
    t.obj.data = g_tex[which];
    t.obj.width = t.obj.height = 16;
    t.obj.format = 6;
    t.obj.wrapS = t.obj.wrapT = 1;
    t.obj.minFilter = t.obj.magFilter = 1;
    t.avail = sizeof g_tex[which];
    g.bpTexStale = 0; g.texDirty = true; g.texSlotDirty = 1;
}
// kind: A B P S T L M (see above)
void Material(char kind) {
    const unsigned nTex = kind == 'S' ? 2u : 1u;
    BP(0, nTex);                       // texgens, no colour channel, one TEV stage
    XF(0x1009, 0);
    XF(0x103F, nTex);
    XF(0x1040, (kind == 'M' ? 6u : 5u) << 7); // texgen 0: regular, 2x4, from TEX0 (M: TEX1)
    XF(0x1041, 5u << 7);              // texgen 1 (S): TEX0 too, through the identity row 60
    XF(0x1018, (30u << 6) | (60u << 12));
    XF(0x1012, kind == 'P' ? 1u : 0u); // dual texgen (post-transform matrix 0 for texgen 0)
    XF(0x1050, 0);
    XF(0x1051, 61);
    BP(0x28, (1u << 6) | (7u << 7));  // stage 0: texmap 0, texcoord 0, no raster colour
    BP(0xC0, (15u << 12) | (8u << 8) | (2u << 4) | 15u | (1u << 19)); // texture colour x C0
    BP(0xC1, (7u << 13) | (4u << 10) | (1u << 7) | (7u << 4) | (1u << 19)); // texture alpha x A0
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, 0);
    if (kind == 'B') BP(0x41, 1u | (1u << 3) | (4u << 8) | (5u << 5)); // alpha blend
    else BP(0x41, 1u | (1u << 3) | (4u << 8) | (1u << 5));             // additive
    BP(0x42, 0);
}
unsigned g_rnd = 4242;
float Rnd() { g_rnd = g_rnd * 1103515245u + 12345u; return ((g_rnd >> 8) & 0xFFFF) / 65536.0f; }
bool g_exact = true;
// A value in [lo, hi): exact mode, a multiple of 1/16.
float Val(float lo, float hi) {
    const float v = lo + (hi - lo) * Rnd();
    return g_exact ? std::floor(v * 16.0f) / 16.0f : v;
}
void TevC0(unsigned r, unsigned gr, unsigned b, unsigned a) {
    BP(0xE2, r | (a << 12));
    BP(0xE3, b | (gr << 12));
}
// One particle: position matrix (billboard: scale, translation), texture matrix, colour; a quad of `quads` quads (L: more).
void Particle(char kind, int i, int quads = 1) {
    const float sc = g_exact ? 0.125f : 0.09f + 0.06f * Rnd();
    const float pm[12] = {sc, 0, 0, Val(-0.85f, 0.85f), 0, sc, 0, Val(-0.85f, 0.85f), 0, 0, 1, -0.5f};
    Rows(0, pm, 12);
    if (kind == 'B') {
        // (identity: nothing loaded per particle)
    } else {
        // JPALoadCalcTexCrdMtxAnm: rotation, scale, translation
        float c, s;
        if (g_exact) { static const float cs[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}}; c = cs[i & 3][0]; s = cs[i & 3][1]; }
        else { const float a = 6.2831853f * Rnd(); c = std::cos(a); s = std::sin(a); }
        const float sx = g_exact ? 0.5f * (1 + (i % 3)) : 0.3f + 2.0f * Rnd(), sy = g_exact ? 0.25f * (1 + (i % 5)) : 0.3f + 2.0f * Rnd();
        const float tm[8] = {sx * c, -sx * s, 0, Val(-2, 2), sy * s, sy * c, 0, Val(-2, 2)};
        Rows(30 * 4, tm, 8);
    }
    if (kind == 'T') BindTex(i & 1);
    TevC0(64 + (i * 37) % 192, 64 + (i * 91) % 192, 64 + (i * 53) % 192, 128 + (i * 29) % 128);
    const uint32_t mask = VL_TEX0;
    const VtxLayout L = MakeLayout(mask);
    std::vector<uint8_t> v(static_cast<size_t>(4 * quads) * L.stride);
    static const float corner[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
    static const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int q = 0; q < quads; ++q)
        for (int k = 0; k < 4; ++k) {
            float* p = reinterpret_cast<float*>(v.data() + static_cast<size_t>(q * 4 + k) * L.stride);
            const float off = static_cast<float>(q) * 0.75f;
            p[0] = corner[k][0] + off; p[1] = corner[k][1] - off; p[2] = 0; p[3] = 0;
            float* t = reinterpret_cast<float*>(v.data() + static_cast<size_t>(q * 4 + k) * L.stride + L.offTex[0]);
            t[0] = uv[k][0] + (g_exact ? 0.5f * q : 0.37f * q);
            t[1] = uv[k][1];
        }
    DrawDecoded(0x80, mask, v.data(), static_cast<uint32_t>(4 * quads));
}
uint32_t Copy(int index, bool clear) {
    g.texCopySrc = EfbRect{0, 0, W, H};
    g.texCopyDstW = W; g.texCopyDstH = H; g.texCopyFmt = 6;
    QueueCopyTex(destinations[index], clear);
    return Pending().copyTex.back().texId;
}
size_t g_records = 0;
bool g_count = false;
} // namespace
namespace vgx {
void SubmitToBackend(CmdBuf& b) {
    if (g_count) g_records += b.draws.size();
    if (g_count && std::getenv("TB_RECS"))
        for (const DrawRec& d : b.draws) std::printf("rec: vs %u fs %u verts %u tex %u mv %u texm %u\n", d.vsId, d.fsId, d.vtxCount, d.tex[0].texId, d.ublock[UB_MV], d.ublock[UB_TEXM]);
    backend::Execute(b);
}
} // namespace vgx

int main(int, char**) {
    const char* md = std::getenv("TB_MODE");
    g_exact = !(md && std::string(md) == "random");
    if (const char* sd = std::getenv("TB_SEED")) g_rnd = static_cast<unsigned>(std::strtoul(sd, nullptr, 10));
    for (int t = 0; t < 2; ++t)
        for (size_t i = 0; i < sizeof g_tex[t]; ++i) g_tex[t][i] = static_cast<uint8_t>((i * 2654435761u + t * 977u) >> 13);
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(W, H), "production backend init");
    if (failures) return 1;
    ResetState(); g.efbW = W; g.efbH = H; QueueSetTarget(); BP(0x43, 1);
    const uint32_t proj[] = {f2u(1), 0, f2u(1), 0, f2u(-1), f2u(-1), 1};
    WriteXF(0x1020, 7, proj);
    Viewport(); Scissor();
    const float ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    Rows(60 * 4, ident, 12);
    const float post[12] = {0.5f, 0, 0, 0.25f, 0, 2.0f, 0, -0.5f, 0, 0, 1, 0}; // post-transform matrix 0 (P)
    Rows(0x500, post, 12);
    // clear to a grey (colour and depth)
    BP(0x4F, 40u | (255u << 8)); BP(0x50, (40u << 8) | 40u); BP(0x51, 0xFFFFFFu);
    Copy(0, true);
    FrontPresent(false, nullptr);
    g_count = true;
    BindTex(0);
    struct Em { char kind; int n; int quads; };
    const Em ems[] = {{'A', 40, 1}, {'B', 12, 1}, {'P', 16, 1}, {'S', 10, 1}, {'T', 10, 1}, {'A', 6, 1}, {'L', 4, 12}, {'M', 5, 1}, {'A', 24, 1}};
    int id = 0;
    for (const Em& e : ems) {
        Material(e.kind);
        BindTex(0);
        for (int i = 0; i < e.n; ++i) Particle(e.kind, id++, e.quads);
    }
    const uint32_t tex = Copy(1, false);
    FrontPresent(false, nullptr);
    g_count = false;
    std::vector<uint8_t> px;
    int pw = 0, ph = 0;
    Check(backend::DebugReadTexture(tex, px, pw, ph), "read the renderer's picture");
    Check(pw == W && ph == H, "picture size");
    unsigned distinct = 0;
    {
        std::vector<uint8_t> seen(1u << 18, 0);
        for (size_t i = 0; i + 3 < px.size(); i += 4) {
            const uint32_t k = ((px[i] >> 2) << 12) | ((px[i + 1] >> 2) << 6) | (px[i + 2] >> 2);
            if (!seen[k]) { seen[k] = 1; ++distinct; }
        }
    }
    Check(distinct > 200, "the picture has detail");
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    uint64_t h = 14695981039346656037ull;
    for (uint8_t b : px) h = (h ^ b) * 1099511628211ull;
    uint64_t c[4];
    TexBakeCounters(c);
    std::printf("bake: draws %llu, new matrices %llu, identity %llu, fallback %llu\n", (unsigned long long)c[0], (unsigned long long)c[1],
                (unsigned long long)c[2], (unsigned long long)c[3]);
    if (const char* dump = std::getenv("TB_DUMP")) {
        if (FILE* f = std::fopen(dump, "wb")) { std::fwrite(px.data(), 1, px.size(), f); std::fclose(f); }
    }
    std::printf("picture %dx%d distinct colours %u hash %016llx\n", pw, ph, distinct, static_cast<unsigned long long>(h));
    std::printf("records %zu\n", g_records);
    return failures ? 1 : 0;
}
