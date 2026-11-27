// GXV_LODBIAS_SAMPLER (lodmask37) through the production front end, shader generator and GLES back end (Mesa): the picture of a biased draw with the bias in
// the shader (switch 0) and in the sampler (switch 1) must be pixel-identical for biases of -2, -1, -0.5 and +0.5 levels, and a 1/32 bias must stay on the shader
// path (the same pixels again). Derived from tests/vgx_lodbias_gl.cpp (the same fixture); tools/test_vgx_lodsampler_gl.sh runs both switches and compares the
// "RESULT" lines. Usage: vgx_lodsampler_gl <switch 0|1>
// A 64x64 I8 texture with seven authored mip levels, each a different constant, is drawn at two texels per pixel (LOD 1) by a
// quad in the left half of a 64x64 target, with texture-unit LOD biases from -2 to +2. Usage: vgx_lodbias_gl <expect-switch 0|1>
#include "vgx_fixes.h"
#include "vgx_state.h"
#include "vgx_texdecode.h"
#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}
namespace {
using namespace vgx;
constexpr uint32_t kTexAddr = 0x00100000; // fake console address of the texture
unsigned failures;
std::vector<uint8_t> g_tex;
void Check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
const uint8_t* Resolve(uint32_t addr, uint32_t* toEnd) {
    if (addr < kTexAddr || addr >= kTexAddr + g_tex.size()) return nullptr;
    if (toEnd) *toEnd = static_cast<uint32_t>(kTexAddr + g_tex.size() - addr);
    return g_tex.data() + (addr - kTexAddr);
}
const unsigned kLevelValue[7] = {20, 60, 100, 140, 180, 220, 250};
void MakeTexture() {
    g_tex.assign(TexDataSize(64, 64, 1, 7), 0);
    uint32_t off = 0, w = 64, h = 64;
    for (int l = 0; l < 7; ++l) {
        const uint32_t size = TexLevelSize(w, h, 1);
        std::memset(g_tex.data() + off, static_cast<int>(kLevelValue[l]), size);
        off += size;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
}
// Draw the quad with the texture unit's LOD bias set to `bias32` / 32 and read the pixel at (16, 32).
unsigned Sample(int bias32, bool mip = true, bool reset = true) {
    if (reset) ResetState();
    g.efbW = g.efbH = 64;
    QueueSetTarget();
    BP(0x43, 1);
    BP(0x20, 342u | (342u << 12)); BP(0x21, 405u | (405u << 12));
    BP(0x59, 171u | (171u << 10));
    const uint32_t m[] = {f2u(1), 0, 0, 0, 0, f2u(1), 0, 0, 0, 0, f2u(1), 0};
    WriteXF(0, 12, m);
    const uint32_t proj[] = {f2u(1), 0, f2u(1), 0, f2u(1), 0, 1};
    WriteXF(0x1020, 7, proj);
    const uint32_t vp[] = {f2u(32), f2u(-32), f2u(16777216), f2u(374), f2u(374), f2u(16777216)};
    WriteXF(0x101A, 6, vp);
    // one texture coordinate: texture matrix = identity (row 60), regular 2x4 from the first texture-coordinate attribute
    const uint32_t one = 1; WriteXF(0x103F, 1, &one);
    const uint32_t tg = 5u << 7; WriteXF(0x1040, 1, &tg);
    BP(0x00, 1);
    // texmap 0: wrap clamp, linear mag, linear/mip-linear min (hardware 6), the bias; mode1 min LOD 0, max LOD 6
    BP(0x80, 0u | (1u << 4) | ((mip ? 6u : 4u) << 5) | ((static_cast<unsigned>(bias32) & 0xFF) << 9));
    BP(0x84, (6u * 16u) << 8);
    BP(0x88, 63u | (63u << 10) | (1u << 20));  // 64x64, format I8
    BP(0x94, kTexAddr >> 5);
    // TEV stage 0: output the texture colour
    BP(0x28, (1u << 6) | (7u << 7));
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19));
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, 0); BP(0x41, (1u << 3) | (1u << 4));  // no depth test; colour and alpha update
    // the left half of the target, texture coordinates 0..1: 2 texels per pixel across, 1 down
    const float v[] = {-1, -1, -0.5f, 0, 0, 1,  0, -1, -0.5f, 0, 1, 1,  0, 1, -0.5f, 0, 1, 0,  -1, 1, -0.5f, 0, 0, 0};
    DrawDecoded(0x80, VL_TEX0, reinterpret_cast<const uint8_t*>(v), 4);
    g.texCopySrc = EfbRect{0, 0, 64, 64};
    g.texCopyDstW = g.texCopyDstH = 64;
    g.texCopyFmt = 6;
    alignas(32) static unsigned char dest[32];
    QueueCopyTex(dest, false);
    const auto id = Pending().copyTex.back().texId;
    FrontPresent(false, nullptr);
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    if (!backend::DebugReadTexture(id, pixels, w, h) || w != 64 || h != 64) { Check(false, "read actual EFB pixels"); return 0; }
    return pixels[(32 * 64 + 16) * 4];
}
bool Near(unsigned got, double want, double tol) { return got >= want - tol && got <= want + tol; }
}

int main(int argc, char** argv) {
    const int sw = argc > 1 ? std::atoi(argv[1]) : 0;
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(64, 64), "production backend init");
    if (failures) return 1;
    Check(FixLodBias(), "GXV_FIX_LODBIAS is on");
    Check(LodSamplerMode() == sw, "GXV_LODBIAS_SAMPLER read once");
    GuestHooks hooks;
    hooks.resolve = Resolve;
    SetGuestHooks(hooks);
    MakeTexture();
    const int biases[] = {-64, -32, -16, 16, 1, -2, 24};  // -2, -1, -0.5, +0.5 levels, then 1/32, -2/32 (not representable), +0.75
    std::printf("RESULT");
    for (int b : biases) std::printf(" %u", Sample(b));
    std::printf("\n");
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
