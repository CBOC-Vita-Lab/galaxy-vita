// GXV_FS_HALF=1 through the production front end, shader generator and GLES back end (Mesa): a draw whose TEV register is outside 0..255
// (the half precision analysis assumes registers in 0..1) gets the float shader of the same key, an in-range draw the half one. The picture of a PC
// is the same either way (the half types are floats there), so the shaders' text is what is checked, plus the pixels and the incremental key hash.
// Usage: vgx_fshalf_pipeline <expected GXV_FS_HALF 0|1>
#include "vgx_state.h"
#include "vgx_shadergen.h"
#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
std::string g_newFs; // text of the fragment shader the last draw registered ("" if none)
}
namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) {
    for (const ShaderSource* s : b.shaders)
        if (s && s->fragment) g_newFs = s->text;
    backend::Execute(b);
}
}
namespace {
using namespace vgx;
unsigned failures;
void Check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
// TEV colour register `idx` (0 = PREV .. 3 = REG2) from signed 11-bit components.
void SetReg(unsigned idx, int r, int gr, int b, int a) {
    BP(0xE0 + idx * 2, (static_cast<unsigned>(r) & 0x7FF) | ((static_cast<unsigned>(a) & 0x7FF) << 12));
    BP(0xE1 + idx * 2, (static_cast<unsigned>(b) & 0x7FF) | ((static_cast<unsigned>(gr) & 0x7FF) << 12));
}
struct Px { unsigned r, g, b; };
// Input of REG0 (selector 2) in position `slot`: 0 = A, 1 = B, 2 = C (the others zero / one so that out = that input), 3 = D.
Px Draw(int slot, bool reset, int r, int gr, int b) {
    g_newFs.clear();
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
    const uint32_t one = 1; WriteXF(0x103F, 1, &one);
    const uint32_t tg = 5u << 7; WriteXF(0x1040, 1, &tg);
    BP(0x00, 1);
    BP(0x28, 7u << 7);  // no texture, no rasterised colour
    // out = lerp(A, B, C) + D, A = zero, B = zero, C = one, D = zero, except the chosen slot reads REG0 (the others keep the identity)
    unsigned in[4] = {15, 15, 12, 15};          // A, B, C, D selectors (zero, zero, one, zero)
    if (slot == 1) in[1] = 2;                    // B = REG0: out = lerp(0, B, 1) = B
    else if (slot == 0) { in[0] = 2; in[1] = 12; in[2] = 15; } // A = REG0, B = one, C = zero: out = A
    else if (slot == 2) { in[0] = 15; in[1] = 12; in[2] = 2; } // C = REG0: out = lerp(0, 1, C) = C
    else in[3] = 2;                              // D = REG0
    if (slot != 3) in[3] = 15;
    BP(0xC0, (in[0] << 12) | (in[1] << 8) | (in[2] << 4) | in[3] | (1u << 19));
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, 0); BP(0x41, (1u << 3) | (1u << 4));
    SetReg(1, r, gr, b, 255);
    const float v[] = {-1, -1, -0.5f, 0, 0, 1,  1, -1, -0.5f, 0, 1, 1,  1, 1, -0.5f, 0, 1, 0,  -1, 1, -0.5f, 0, 0, 0};
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
    if (!backend::DebugReadTexture(id, pixels, w, h) || w != 64 || h != 64) { Check(false, "read actual EFB pixels"); return {0, 0, 0}; }
    const uint8_t* p = &pixels[(32 * 64 + 32) * 4];
    return {p[0], p[1], p[2]};
}
bool Near(unsigned got, unsigned want) { return got + 2 >= want && got <= want + 2; }
bool Is(Px p, unsigned r, unsigned gr, unsigned b) { return Near(p.r, r) && Near(p.g, gr) && Near(p.b, b); }
bool Half(const std::string& t) { return t.find("hvec4") != std::string::npos; }
}

int main(int argc, char** argv) {
    const long expect = argc > 1 ? std::atol(argv[1]) : 0;
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(64, 64), "production backend init");
    if (failures) return 1;
    Check(FsHalfMode() == expect, "GXV_FS_HALF read once");
    Check(!FixTevIn(), "GXV_FIX_TEVIN is off (the registers are tracked for GXV_FS_HALF alone)");
    Check(TevOorTracked() == (expect == 1), "registers tracked exactly when GXV_FS_HALF=1");
    // REG0 as input D: out = REG0 (a copy: its bound is one half rounding, so the key is eligible)
    const Px a = Draw(3, true, 200, 100, 30);
    const std::string fsA = g_newFs;
    Check(!fsA.empty() && Half(fsA) == (expect == 1), "in-range register: the half shader with GXV_FS_HALF=1, the float shader without");
    Check(Is(a, 200, 100, 30), "in-range picture");
    const Px b = Draw(3, false, 270, 100, -106);
    const std::string fsB = g_newFs;
    if (expect == 1) Check(!fsB.empty() && !Half(fsB) && fsB.find("vgx_w1") == std::string::npos, "register outside 0..255: a new shader, float, no wrap code");
    else Check(fsB.empty(), "switch off: the out-of-range draw is the same key and shader");
    Check(Is(b, 255, 100, 0), "out-of-range picture (D is clamped)");
    const Px c = Draw(3, false, 40, 50, 60);
    Check(g_newFs.empty() && Is(c, 40, 50, 60), "in range again: the first shader is used again (no new one) and the picture is right");
    const Px d = Draw(3, false, 270, 100, -106);
    Check(g_newFs.empty() && Is(d, 255, 100, 0), "out of range again: the second shader again");
    Check(KeyHashOf(g) == g.keyHash, "incremental key hash equals KeyHashOf");
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
