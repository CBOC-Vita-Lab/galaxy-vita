// GXV_PIX_ZERO / GXV_PIX_ATPASS (pix23, platform/vgx/src/vgx_draw.cpp "value-proven pixel cuts"): through the production GX front end and
// the production GLES back end on Mesa (no game data). tools/test_vgx_pixval_gl.sh runs it with the switches off, on (=1) and counting
// only (=2), under the plain generator and under the console's shader switches; every picture (all four channels) must be identical, and
// the counts must be what the frames hold.
//   Frame 1 (a 64x64 EFB, depth-tested floor quads written first):
//     a  blended (SRC_ALPHA / ONE_MINUS_SRC_ALPHA) quads whose colour and alpha are C0 with C0's alpha 0          -> left out (no-op)
//     b  the same with alpha 8 (it changes the picture)                                                          -> drawn
//     c  additive (ONE / ONE) quad with C0 = 0 everywhere, alpha written                                        -> left out
//     d  additive quad with C0 colour 0 and alpha 60, alpha written                                             -> drawn (alpha changes)
//     e  additive quad with C0 colour 0 and alpha 60, alpha not written                                         -> left out
//     f  a depth-writing quad with the alpha test GREATER 128 and alpha 100 (killed everywhere), in front of g -> left out
//     g  a quad behind f (shows that f wrote no depth)                                                          -> drawn
//     h  blended quad textured with an I8 texture of zeros (TEXA = 0)                                           -> left out
//     i  blended quad whose colour and alpha come from the rasterised colour, material alpha 0 (lighting off)    -> left out
//     j  the same with material alpha 90                                                                        -> drawn
//     p  blended quad whose colour and alpha are its vertex colours, alpha 0 at every corner                     -> left out
//     q  the same with one corner's alpha 40                                                                    -> drawn
//   Frame 2 (alpha-tested, depth-writing cut-out quads):
//     k  RGB565 texture (alpha 255 everywhere), alpha test GREATER 128 on TEXA                                   -> drawn without the test
//     l  I8 gradient texture (alpha = intensity 0..255), the same test: kills about half                         -> drawn with it
//     m  RGB565 texture, alpha test GREATER 128 OR LESS 10                                                        -> without the test
//     n  RGB565 texture, alpha test LESS 128 (kills every fragment)                                              -> left out (GXV_PIX_ZERO)
//     o  quads behind k..n (depth): what k..m wrote in depth shows
#include "vgx_state.h"
#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
void PixCounters(uint64_t out[8]);
}
namespace {
using namespace vgx;
alignas(32) unsigned char readDestination[32];
alignas(32) uint8_t texZero[64];      // I8 8x8, all 0
alignas(32) uint8_t texGrad[64];      // I8 8x8, 0..252
alignas(32) uint8_t tex565[128];      // RGB565 8x8, random colours (alpha 255)
unsigned failures = 0;
void Check(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
void XF(unsigned reg, unsigned value) { WriteXF(reg, 1, &value); }
void Viewport(int x, int y, int w, int h) {
    const uint32_t v[] = {f2u(w / 2.0f), f2u(-h / 2.0f), f2u(16777215.0f + 1), f2u(342.0f + x + w / 2.0f), f2u(342.0f + y + h / 2.0f), f2u(16777215.0f + 1)};
    WriteXF(0x101A, 6, v);
}
void Scissor(int x, int y, int w, int h) {
    BP(0x20, (342u + y) | ((342u + x) << 12));
    BP(0x21, (342u + y + h - 1) | ((342u + x + w - 1) << 12));
}
void Color(unsigned r, unsigned green, unsigned b, unsigned alpha) {
    BP(0xE2, r | alpha << 12);
    BP(0xE3, b | green << 12);
}
void AlphaTest(unsigned comp0, unsigned ref0, unsigned op = 0, unsigned comp1 = 7, unsigned ref1 = 0) {
    BP(0xF3, ref0 | (ref1 << 8) | (comp0 << 16) | (comp1 << 19) | (op << 22));
}
void Plain() { // no texture: colour and alpha = C0
    BP(0x00, 0);
    XF(0x103F, 0);
    XF(0x1009, 0);
    BP(0x28, 7u << 7);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    AlphaTest(7, 0);
}
// colour and alpha from the rasterised colour 0: one channel, lighting off, material from the register (alpha mat)
void Ras(uint32_t matRgba) {
    BP(0x00, 0);
    XF(0x103F, 0);
    XF(0x1009, 1);
    XF(0x100C, matRgba);
    XF(0x100E, 0); // (colour 0: material from the register, no lighting)
    XF(0x1010, 0); // (alpha 0: the same)
    BP(0x28, 0u << 7); // (stage 0: COLOR0A0)
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 10u | (1u << 19)); // d = RASC
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (5u << 4) | (1u << 19)); // d = RASA
    AlphaTest(7, 0);
}
// colour and alpha from the vertex colours (channel 0 off: the attribute itself)
void Vtx() {
    BP(0x00, 0);
    XF(0x103F, 0);
    XF(0x1009, 1);
    XF(0x100E, 1); // (colour 0: material from the vertex colour, no lighting)
    XF(0x1010, 1); // (alpha 0: the same)
    BP(0x28, 0u << 7);
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 10u | (1u << 19)); // d = RASC
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (5u << 4) | (1u << 19)); // d = RASA
    AlphaTest(7, 0);
}
// A quad with a colour per corner (RGBA bytes), layout pos + col0
void QuadCol(float l, float r, float b, float t, float depth, const uint8_t c[4][4]) {
    const float z = depth - 1;
    const float p[4][4] = {{l, b, z, 0}, {r, b, z, 0}, {r, t, z, 0}, {l, t, z, 0}};
    uint8_t v[4 * 20];
    for (int i = 0; i < 4; ++i) { std::memcpy(v + i * 20, p[i], 16); std::memcpy(v + i * 20 + 16, c[i], 4); }
    DrawDecoded(0x80, VL_COL0, v, 4);
}
void Tex(void* data, unsigned format, int w, int h) {
    auto& t = g.tex[0];
    t = TexSlot{};
    t.valid = true;
    t.obj.data = data;
    t.obj.width = static_cast<uint16_t>(w);
    t.obj.height = static_cast<uint16_t>(h);
    t.obj.format = format;
    t.obj.magFilter = 1;
    t.obj.minFilter = 1;
    g.bpTexStale = 0;
    g.texDirty = true;
    g.texSlotDirty = 1;
    XF(0x1009, 0);
    XF(0x103F, 1);
    XF(0x1040, 5u << 7);
    BP(0x00, 1);
    BP(0x28, (1u << 6) | (7u << 7));
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19)); // colour = TEXC
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (4u << 4) | (1u << 19)); // alpha = TEXA
}
void QuadRect(float l, float r, float b, float t, float depth, bool tex = false) {
    const float z = depth - 1;
    const float v[] = {l, b, z, 0, 0, 1, r, b, z, 0, 1, 1, r, t, z, 0, 1, 0, l, t, z, 0, 0, 0};
    if (tex) DrawDecoded(0x80, VL_TEX0, reinterpret_cast<const uint8_t*>(v), 4);
    else {
        float plain[16];
        for (int i = 0; i < 4; ++i) std::memcpy(plain + i * 4, v + i * 6, 16);
        DrawDecoded(0x80, 0, reinterpret_cast<const uint8_t*>(plain), 4);
    }
}
void Copy(void* dest, unsigned format, EfbRect r, bool clear) {
    g.texCopySrc = r;
    g.texCopyDstW = static_cast<uint16_t>(r.w);
    g.texCopyDstH = static_cast<uint16_t>(r.h);
    g.texCopyFmt = format;
    QueueCopyTex(dest, clear);
}
void ZMode(bool test, unsigned func, bool update) { BP(0x40, (test ? 1u : 0u) | (func << 1) | (update ? 1u << 4 : 0u)); }
void Blend(bool on, bool colour, bool alpha, unsigned dst = 0, unsigned src = 1) {
    BP(0x41, (on ? 1u : 0u) | (colour ? 1u << 3 : 0u) | (alpha ? 1u << 4 : 0u) | (dst << 5) | (src << 8));
}
void ClearColour(unsigned r, unsigned green, unsigned b, unsigned a, unsigned z24) {
    BP(0x4F, r | (a << 8));
    BP(0x50, b | (green << 8));
    BP(0x51, z24);
}
std::vector<uint8_t> ReadAndFlush() {
    Scissor(0, 0, 64, 64);
    Copy(readDestination, 6, EfbRect{0, 0, 64, 64}, false);
    const auto id = Pending().copyTex.back().texId;
    FrontPresent(false, nullptr);
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    Check(backend::DebugReadTexture(id, pixels, w, h) && w > 0 && h > 0, "picture read back");
    return pixels;
}
uint64_t Hash(const std::vector<uint8_t>& p) {
    uint64_t h = 14695981039346656037ull;
    for (uint8_t b : p) h = (h ^ b) * 1099511628211ull;
    return h;
}
void Floor() {
    Plain();
    Viewport(0, 0, 64, 64);
    Scissor(0, 0, 64, 64);
    ZMode(true, 7, true);
    Blend(false, true, true);
    ClearColour(10, 20, 120, 77, 0xFFFFFF);
    Copy(readDestination, 6, EfbRect{0, 0, 64, 64}, true); // (clears colour, alpha and depth)
    Color(40, 200, 90, 200);
    QuadRect(-1, 0, -1, 1, 0.6f);
    Color(220, 120, 30, 100);
    QuadRect(0, 1, -1, 1, 0.5f);
}
std::vector<uint8_t> Frame1() {
    Floor();
    ZMode(true, 3, false); // (tested against the floor, no depth written)
    // a: alpha 0, SRC_ALPHA / ONE_MINUS_SRC_ALPHA, colour and alpha written
    Blend(true, true, true, 5, 4);
    Color(250, 250, 250, 0);
    QuadRect(-0.9f, 0.1f, -0.9f, 0.1f, 0.3f);
    Color(0, 255, 0, 0);
    QuadRect(-0.2f, 0.8f, -0.5f, 0.6f, 0.3f);
    // b: alpha 8 (small, but it changes the picture: the EFB here is RGBA6)
    Color(250, 250, 250, 8);
    QuadRect(-0.6f, 0.4f, -0.3f, 0.9f, 0.3f);
    // c: ONE / ONE, everything 0
    Blend(true, true, true, 1, 1);
    Color(0, 0, 0, 0);
    QuadRect(-1, 1, -1, 1, 0.3f);
    // d: ONE / ONE, colour 0, alpha 60, alpha written
    Color(0, 0, 0, 60);
    QuadRect(-0.5f, 0.5f, -0.5f, 0.5f, 0.3f);
    // e: the same, alpha not written
    Blend(true, true, false, 1, 1);
    QuadRect(-1, 0.5f, -1, 0.5f, 0.3f);
    // f: depth-writing, alpha test GREATER 128, alpha 100: killed everywhere
    Blend(false, true, true);
    ZMode(true, 3, true);
    AlphaTest(4, 128);
    Color(255, 0, 255, 100);
    QuadRect(-0.7f, 0.7f, -0.7f, 0.7f, 0.1f);
    // g: behind f, in front of the floor
    AlphaTest(7, 0);
    Color(30, 30, 230, 255);
    QuadRect(-0.4f, 0.4f, -0.8f, 0.8f, 0.2f);
    // h: blended, TEXA of an all-zero I8 texture
    ZMode(true, 3, false);
    Blend(true, true, true, 5, 4);
    Tex(texZero, 1, 8, 8);
    QuadRect(-1, 1, -1, 1, 0.05f, true);
    // i: blended, colour / alpha from the rasterised colour, material alpha 0
    Ras(0xFF808000u);
    QuadRect(-0.8f, 0.9f, -0.9f, 0.3f, 0.05f);
    // j: material alpha 90
    Ras(0x20F0405Au);
    QuadRect(-0.3f, 0.5f, -0.2f, 0.95f, 0.05f);
    // p: vertex colours, alpha 0 at every corner (a layout pane faded out): the draw's own vertices prove it
    Vtx();
    const uint8_t zeroA[4][4] = {{255, 0, 0, 0}, {0, 255, 0, 0}, {0, 0, 255, 0}, {255, 255, 255, 0}};
    QuadCol(-0.95f, 0.6f, -0.6f, 0.95f, 0.05f, zeroA);
    // q: the same with one corner's alpha 40
    const uint8_t someA[4][4] = {{255, 0, 0, 0}, {0, 255, 0, 0}, {0, 0, 255, 40}, {255, 255, 255, 0}};
    QuadCol(-0.5f, 0.9f, -0.95f, 0.4f, 0.05f, someA);
    Plain();
    return ReadAndFlush();
}
std::vector<uint8_t> Frame2() {
    Floor();
    ZMode(true, 3, true);
    Blend(false, true, true);
    // k: RGB565, GREATER 128
    Tex(tex565, 4, 8, 8);
    AlphaTest(4, 128);
    QuadRect(-1, -0.2f, -1, 0.2f, 0.3f, true);
    // l: I8 gradient, GREATER 128
    Tex(texGrad, 1, 8, 8);
    AlphaTest(4, 128);
    QuadRect(-0.3f, 0.6f, -0.6f, 0.9f, 0.25f, true);
    // m: RGB565, GREATER 128 OR LESS 10
    Tex(tex565, 4, 8, 8);
    AlphaTest(4, 128, 1, 1, 10);
    QuadRect(0.1f, 1, -0.9f, 0.1f, 0.2f, true);
    // n: RGB565, LESS 128: no fragment survives
    AlphaTest(1, 128);
    QuadRect(-0.9f, 0.9f, 0.3f, 1, 0.1f, true);
    // o: behind them
    Plain();
    Color(250, 250, 30, 255);
    QuadRect(-1, 1, -0.5f, 0.5f, 0.35f);
    return ReadAndFlush();
}
}

int main() {
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(64, 64), "production backend init");
    if (failures) return 1;
    for (int i = 0; i < 64; ++i) { texZero[i] = 0; texGrad[i] = static_cast<uint8_t>(i * 4); }
    uint32_t s = 12345;
    for (int i = 0; i < 128; ++i) { s = s * 1103515245u + 12345u; tex565[i] = static_cast<uint8_t>(s >> 16); }
    ResetState();
    g.efbW = g.efbH = 64;
    QueueSetTarget();
    BP(0x43, 1); // GX_PF_RGBA6_Z24
    BP(0xF6, 4u);  // swap table 0 = r g b a (as GXInit leaves it): KSEL0 r 0, g 1;
    BP(0xF7, 14u); // KSEL1 b 2, a 3
    Scissor(0, 0, 64, 64);
    BP(0x59, 171u | (171u << 10));
    const uint32_t m[] = {f2u(1), 0, 0, 0, 0, f2u(1), 0, 0, 0, 0, f2u(1), 0};
    WriteXF(0, 12, m);
    const uint32_t proj[] = {f2u(1), 0, f2u(1), 0, f2u(1), 0, 1};
    WriteXF(0x1020, 7, proj);
    const auto p1 = Frame1();
    const auto p2 = Frame2();
    const auto p3 = Frame1(); // (again: the variants and decisions remembered)
    const auto p4 = Frame2();
    uint64_t c[8] = {};
    PixCounters(c);
    std::printf("frame1 hash=%016llx\nframe2 hash=%016llx\nframe3 hash=%016llx\nframe4 hash=%016llx\n", static_cast<unsigned long long>(Hash(p1)),
                static_cast<unsigned long long>(Hash(p2)), static_cast<unsigned long long>(Hash(p3)), static_cast<unsigned long long>(Hash(p4)));
    std::printf("counts draws=%llu kill=%llu blendnoop=%llu atpass=%llu made=%llu refused=%llu evals=%llu notok=%llu\n", (unsigned long long)c[0],
                (unsigned long long)c[1], (unsigned long long)c[2], (unsigned long long)c[3], (unsigned long long)c[4], (unsigned long long)c[5],
                (unsigned long long)c[6], (unsigned long long)c[7]);
    Check(Hash(p1) == Hash(p3) && Hash(p2) == Hash(p4), "the repeated frames are the same");
    auto mode = [](const char* n) { const char* e = std::getenv(n); return e ? std::atoi(e) : 0; };
    const int zm = mode("GXV_PIX_ZERO"), am = mode("GXV_PIX_ATPASS");
    if (zm == 3) {
        // (defaults48, the lean way: only the proof from a draw's own vertex colours: p in two frames; nothing is evaluated at the resolution)
        Check(c[1] == 0, "GXV_PIX_ZERO=3: no alpha-test decision");
        Check(c[2] == 2, "GXV_PIX_ZERO=3: two blend no-ops (p in two frames, from its vertex colours)");
        Check(c[6] == 2 || c[6] == 1, "GXV_PIX_ZERO=3: evaluations only for the re-checked draws");
    } else if (zm) {
        // frames 1 and 3: f (kill-all); a x2, c, e, h, i, p (blend no-op); frames 2 and 4: n (kill-all)
        Check(c[1] == 4, "GXV_PIX_ZERO: four draws whose alpha test fails everywhere (f, n in two frames each)");
        Check(c[2] == 14, "GXV_PIX_ZERO: fourteen blend no-ops (a, a, c, e, h, i, p in two frames)");
    } else {
        Check(c[1] == 0 && c[2] == 0, "GXV_PIX_ZERO off: nothing decided");
    }
    if (am) {
        // frames 2 and 4: k and m (n is left out by GXV_PIX_ZERO when it is on, else it is counted as neither: its test fails)
        Check(c[3] == 4, "GXV_PIX_ATPASS: k and m in two frames without the test");
        if (am == 1) Check(c[4] >= 1 && c[5] == 0, "GXV_PIX_ATPASS=1: a variant made, none refused");
        else Check(c[4] == 0, "GXV_PIX_ATPASS=2: counting only, no variant made");
    } else {
        Check(c[3] == 0 && c[4] == 0, "GXV_PIX_ATPASS off: nothing decided");
    }
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
