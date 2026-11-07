// GX TEV input wrap (GXV_FIX_TEVIN) through the production front end, shader generator and GLES back end (Mesa).
// GX reads the inputs A, B and C of a TEV stage as the low 8 bits of a (signed 11-bit) register value and the input D as is
// (then clamps). J3D materials and the BRK colour animation store such values (a Luma's body colour). A full-target quad is
// drawn with TEV register REG0 = (270, 100, -106, 255) and one stage reading it as B (out = B) or as D (out = D).
// Usage: vgx_tevin_gl <expect-switch 0|1>
#include "vgx_state.h"
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
}

int main(int argc, char** argv) {
    const bool expectOn = argc > 1 && std::atoi(argv[1]) == 1;
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(64, 64), "production backend init");
    if (failures) return 1;
    Check(FixTevIn() == expectOn, "GXV_FIX_TEVIN read once");
    // 270 & 255 = 14, 100, -106 & 255 = 150. Clamped (switch off, and always for D): 255, 100, 0.
    const Px wrapped = expectOn ? Px{14, 100, 150} : Px{255, 100, 0};
    const Px clamped = {255, 100, 0};
    for (int slot = 0; slot < 3; ++slot) {
        const Px p = Draw(slot, true, 270, 100, -106);
        std::printf("REG0 as %c: (%u,%u,%u)\n", "ABC"[slot], p.r, p.g, p.b);
        char name[96];
        std::snprintf(name, sizeof name, "input %c reads the low 8 bits of an out-of-range register%s", "ABC"[slot], expectOn ? "" : " (switch off: clamped)");
        Check(Is(p, wrapped.r, wrapped.g, wrapped.b), name);
    }
    const Px d = Draw(3, true, 270, 100, -106);
    std::printf("REG0 as D: (%u,%u,%u)\n", d.r, d.g, d.b);
    Check(Is(d, clamped.r, clamped.g, clamped.b), "input D is not wrapped (clamped)");
    // In range: identical with the switch on and off.
    const Px in = Draw(1, true, 200, 100, 30);
    Check(Is(in, 200, 100, 30), "in-range register unchanged");
    // The register rewritten in place between draws (no state reset): keys and key hash follow it.
    const Px p1 = Draw(1, false, 270, 100, -106), p2 = Draw(1, false, 200, 100, 30), p3 = Draw(1, false, 270, 100, -106), p4 = Draw(1, false, 40, 50, 60);
    std::printf("rewritten in place: (%u,%u,%u) (%u,%u,%u) (%u,%u,%u) (%u,%u,%u)\n", p1.r, p1.g, p1.b, p2.r, p2.g, p2.b, p3.r, p3.g, p3.b, p4.r, p4.g, p4.b);
    Check(Is(p1, wrapped.r, wrapped.g, wrapped.b) && Is(p2, 200, 100, 30) && Is(p3, wrapped.r, wrapped.g, wrapped.b) && Is(p4, 40, 50, 60),
          "register changed in place: wrapped, plain, wrapped, plain");
    Check(KeyHashOf(g) == g.keyHash, "incremental key hash equals KeyHashOf");
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
