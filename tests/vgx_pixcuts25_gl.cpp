// pixcuts25: GXV_CLEARQ, GXV_TEVIN_PREWRAP and GXV_INDCAP_SKIP through the production front end and GLES back end on Mesa (no game data).
// tools/test_vgx_pixcuts25_gl.sh runs this binary with each switch off and on and compares the pictures (every frame prints
// "frameNN hash=<64-bit hash of all four channels> scenes=<GXM scenes>"; the script ignores the scene counts).
//
// Part A (frames 1-12, GXV_CLEARQ): MainLoopFramework::clearEfb as the game draws it: a 64 x 64 EFB, an orthographic projection in pixels (y down),
//   one TEV stage (C0 / A0 straight out), GX_ZT_REPLACE with a 4 x 4 Z24X8 texture of one value, z ALWAYS with update, no blend, GX_CULL_BACK, the quad
//   (0,0) (w,0) (w,h) (0,h) with a texture coordinate. Then the depth is read by two half-screen quads (LEQUAL and GEQUAL at 0.5) and a strip written
//   with another alpha, so the colour, the alpha and the depth of the clear are all in the picture. Frames that qualify (every colour byte 0 or 255,
//   depth exactly 0 or 1) are made by a clear with the switch on ("converted"); the rest must stay a quad and look the same.
// Part B (frames 13-14, GXV_TEVIN_PREWRAP): one stage reading TEV register REG0 as input A and as input D (out = A + D) with REG0 out of the 8-bit
//   range, then a second stage multiplying by a texture; four quadrants, REG0 rewritten in place between them.
// Part C (frames 15-26, GXV_INDCAP_SKIP): the game's marked "Indirect" capture: unread, read after it in the same picture, read before it by the next
//   picture's reader (the first-sight stale read) - see the script for the runs.
// GXV_CLEARQ_BREAK=1 (the back end clears the inverted colour) and GXV_PREWRAP_BREAK=1 (an unwrapped copy) are the negative controls.
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
unsigned failures = 0;
unsigned g_frame = 0;
alignas(32) unsigned char readDestination[32];
alignas(32) unsigned char indirectDestination[32];
alignas(32) unsigned char zTex0[64], zTex1[64], zTexHalf[64];
alignas(32) unsigned char grey[64];
void Check(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
long Env(const char* n, long d) { const char* v = std::getenv(n); return v ? std::atol(v) : d; }
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
void XF(unsigned reg, unsigned value) { WriteXF(reg, 1, &value); }
void Viewport() {
    const uint32_t w[] = {f2u(32), f2u(-32), f2u(16777215.0f + 1), f2u(374), f2u(374), f2u(16777215.0f + 1)};
    WriteXF(0x101A, 6, w);
}
void Scissor(int w, int h) {
    BP(0x20, 342u | (342u << 12));
    BP(0x21, (342u + h - 1) | ((342u + w - 1) << 12));
}
void OrthoClip() { // the vertices are clip coordinates
    const uint32_t proj[] = {f2u(1), 0, f2u(1), 0, f2u(1), 0, 1};
    WriteXF(0x1020, 7, proj);
}
void OrthoPixels() { // C_MTXOrtho(proj, 0, h, 0, w, 0, 1) for a 64 x 64 target (GX layout: p0 p1 p2 p3 p4 p5, type)
    const uint32_t proj[] = {f2u(2.0f / 64), f2u(-1.0f), f2u(-2.0f / 64), f2u(1.0f), f2u(-1.0f), f2u(0.0f), 1};
    WriteXF(0x1020, 7, proj);
}
void Color0(unsigned r, unsigned green, unsigned b, unsigned a) { // TEV register REG0 (C0 / A0)
    BP(0xE2, r | a << 12);
    BP(0xE3, b | green << 12);
}
void SetReg(unsigned idx, int r, int gr, int b, int a) { // signed 11-bit components
    BP(0xE0 + idx * 2, (static_cast<unsigned>(r) & 0x7FF) | ((static_cast<unsigned>(a) & 0x7FF) << 12));
    BP(0xE1 + idx * 2, (static_cast<unsigned>(b) & 0x7FF) | ((static_cast<unsigned>(gr) & 0x7FF) << 12));
}
// A one-stage C0 / A0 material (no texture), the genmode's cull bits as given.
void Plain(unsigned cull = 0) {
    BP(0x00, (cull << 14));
    XF(0x103F, 0);
    BP(0x28, 7u << 7);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19)); // alpha test ALWAYS
    BP(0xF5, 0);
    BP(0x40, 1 | (7u << 1) | (1u << 4));
    BP(0x41, (1u << 3) | (1u << 4));
    BP(0x42, 0);
}
void ClipQuad(float x0, float y0, float x1, float y1, float depth) {
    const float z = depth - 1;
    const float v[] = {x0, y0, z, 0, x1, y0, z, 0, x1, y1, z, 0, x0, y1, z, 0};
    DrawDecoded(0x80, 0, reinterpret_cast<const uint8_t*>(v), 4);
}
void Copy(void* dest, unsigned format) {
    g.texCopySrc = EfbRect{0, 0, 64, 64};
    g.texCopyDstW = g.texCopyDstH = 64;
    g.texCopyFmt = format;
    QueueCopyTex(dest, false);
}
uint64_t Hash(const std::vector<uint8_t>& p) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < p.size(); ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
// The picture of the frame: copy, flush, read back; prints the frame's line.
std::vector<uint8_t> Finish(const char* what) {
    Copy(readDestination, 6);
    const auto id = Pending().copyTex.back().texId;
    FrontPresent(false, nullptr);
    const unsigned scenes = backend::DebugLastFrameScenes();
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    Check(backend::DebugReadTexture(id, pixels, w, h) && w == 64 && h == 64, "picture read back");
    std::printf("frame%02u hash=%016llx scenes=%u  (%s)\n", ++g_frame, static_cast<unsigned long long>(Hash(pixels)), scenes, what);
    return pixels;
}
void BindTexture(void* data, unsigned format, unsigned w, unsigned h, unsigned tref) {
    auto& t = g.tex[0];
    t = TexSlot{};
    t.valid = true;
    t.obj.data = data;
    t.obj.width = static_cast<uint16_t>(w);
    t.obj.height = static_cast<uint16_t>(h);
    t.obj.format = format;
    g.bpTexStale = 0;
    g.texDirty = true;
    g.texSlotDirty = 1;
    XF(0x103F, 1);
    XF(0x1040, 5u << 7);
    (void)tref;
}
void SetupTarget() {
    Scissor(64, 64);
    Viewport();
    OrthoClip();
}
// Something for the clear to overwrite: colour, alpha and a near depth, over the whole EFB.
void Junk(unsigned seed) {
    SetupTarget();
    Plain();
    Color0(40 + seed * 17 % 200, 90 + seed * 29 % 150, 200 - seed * 13 % 100, 77);
    ClipQuad(-1, -1, 1, 1, 0.2f);
}
struct Clear {
    unsigned r = 0, g = 0, b = 0, a = 0;
    unsigned char* z = zTex1;   // the Z texture: zTex1 = 1.0, zTex0 = 0.0, zTexHalf = about 0.5
    bool dstAlpha = true;       // mUseAlpha: GXSetDstAlpha(TRUE, color.a), else the alpha is A0
    unsigned cull = 1;          // GX_CULL_BACK
    bool blend = false;
    int scissorW = 64;          // the scissor's width (64: all the EFB)
    bool useReg = false; int rr = 0, rg = 0, rb = 0, ra = 0; // C0 / A0 as signed values (out of 0..255 too)
    unsigned planes = 3;        // bit 0 colour update, bit 1 alpha update (DrawUtil::clearZBuffer writes neither)
    bool noDepth = false;       // depth test off (a colour-only fill)
    unsigned dstAlphaValue = 256; // 256: the colour's alpha (mUseAlpha); else the destination-alpha constant
};
// MainLoopFramework::clearEfb(0, 0, 64, 64, colour)
void ClearEfbQuad(const Clear& c) {
    OrthoPixels();
    Viewport();
    Scissor(c.scissorW, 64);
    BP(0x00, 1u | (c.cull << 14)); // one texgen, one stage
    XF(0x103F, 1);
    XF(0x1040, 5u << 7);
    BindTexture(c.z, 6, 4, 4, 0);
    BP(0x28, (1u << 6) | (7u << 7)); // TEXCOORD0, TEXMAP0, no colour channel
    if (c.useReg) SetReg(1, c.rr, c.rg, c.rb, c.ra); else Color0(c.r, c.g, c.b, c.a);
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 2u | (1u << 19));
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (1u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0xF5, c.noDepth ? 0u : (2u | (2u << 2))); // GX_ZT_REPLACE, Z24X8
    BP(0xF4, 0);
    BP(0x40, c.noDepth ? 0u : (1 | (7u << 1) | (1u << 4))); // z ALWAYS with update
    if (c.blend) BP(0x41, 1u | (1u << 3) | (1u << 4) | (4u << 8) | (5u << 5)); // blend src-alpha / inv src-alpha
    else BP(0x41, ((c.planes & 1u) ? (1u << 3) : 0u) | ((c.planes & 2u) ? (1u << 4) : 0u));
    BP(0x42, c.dstAlpha ? (0x100u | (c.dstAlphaValue < 256 ? c.dstAlphaValue : c.a)) : 0u);
    const float v[] = {0, 0, 0, 0, 0, 0, 64, 0, 0, 0, 1, 0, 64, 64, 0, 0, 1, 1, 0, 64, 0, 0, 0, 1};
    DrawDecoded(0x80, VL_TEX0, reinterpret_cast<const uint8_t*>(v), 4);
    OrthoClip();
    Viewport();
    Scissor(64, 64);
    BP(0xF5, 0);
}
// The read: the left half where the depth is <= 0.5, the right half where it is >= 0.5 (one of them draws, from the depth the clear left), and
// a strip at the bottom with another alpha.
void Readers() {
    Plain();
    BP(0x40, 1 | (3u << 1) | (1u << 4)); // LEQUAL
    Color0(220, 40, 40, 128);
    ClipQuad(-1, -0.5f, 0, 1, 0.5f);
    BP(0x40, 1 | (6u << 1) | (1u << 4)); // GEQUAL
    Color0(40, 40, 220, 128);
    ClipQuad(0, -0.5f, 1, 1, 0.5f);
    BP(0x40, 1 | (1u << 1)); // LESS, no update: only where the depth is still above the first readers' depth
    Color0(240, 240, 30, 90);
    ClipQuad(-0.25f, -1, 0.25f, -0.9f, 0.9f);
}
void FillZ(unsigned char* d, unsigned v24) {
    std::memset(d, 0, 64);
    for (int i = 0; i < 16; ++i) { d[i * 2 + 1] = static_cast<unsigned char>(v24 >> 16); d[32 + i * 2] = static_cast<unsigned char>(v24 >> 8); d[33 + i * 2] = static_cast<unsigned char>(v24); }
}
// ---- part B
void PrewrapQuadrant(float x0, float y0, float x1, float y1, int r, int gr, int b) {
    SetReg(1, r, gr, b, 255);
    ClipQuad(x0, y0, x1, y1, 0.5f);
}
struct Px { unsigned r, g, b; };
Px At(const std::vector<uint8_t>& p, int x, int y) {
    // (the read-back may be bottom-up: both rows are looked up by the caller where it matters)
    const uint8_t* q = &p[(static_cast<size_t>(y) * 64 + x) * 4];
    return {q[0], q[1], q[2]};
}
bool Near(unsigned got, unsigned want) { return got + 3 >= want && got <= want + 3; }
bool PxIs(Px p, unsigned r, unsigned gr, unsigned b) { return Near(p.r, r) && Near(p.g, gr) && Near(p.b, b); }
std::vector<uint8_t> PrewrapFrame(bool useTexture) {
    SetupTarget();
    Plain();
    // stage 0: out = A + D with A = D = REG0 (A reads the low 8 bits, D the whole value); stage 1: out = TEXC * PREV
    BP(0x00, (useTexture ? 1u : 0u) | ((useTexture ? 1u : 0u) << 10));
    BP(0x28, (7u << 7) | ((useTexture ? ((1u << 6) | (7u << 7)) : (7u << 7)) << 12));
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 2u | (1u << 19)); // A = C0, B = C = ZERO, D = C0
    BP(0xC1, (1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    if (useTexture) {
        BP(0xC2, (15u << 12) | (8u << 8) | (0u << 4) | 15u | (1u << 19)); // B = TEXC, C = CPREV
        BP(0xC3, (7u << 13) | (7u << 10) | (7u << 7) | (0u << 4) | (1u << 19)); // APREV
        BindTexture(grey, 1, 8, 8, 0);
    }
    PrewrapQuadrant(-1, -1, 0, 0, 300, -20, 100);
    PrewrapQuadrant(0, -1, 1, 0, 40, 60, 90);
    PrewrapQuadrant(-1, 0, 0, 1, 270, 50, -106);
    PrewrapQuadrant(0, 0, 1, 1, 300, -20, 100); // the first set again: the register rewritten in place
    return Finish(useTexture ? "REG0 as A and D, then x texture" : "REG0 as A and D");
}
// ---- part C
void MarkedCapture(unsigned stage = 0) {
    BP(0xFF, 10 | (stage << 8)); // PORT_GX_MARK_TRANSIENT_COPY
    Copy(indirectDestination, 4);
}
void ReaderOfCapture(float x0, float x1) {
    BindTexture(indirectDestination, 4, 64, 64, 0);
    BP(0x00, 1);
    BP(0x28, (1u << 6) | (7u << 7));
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19));
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (1u << 4) | (1u << 19));
    BP(0x40, 0);
    BP(0x41, (1u << 3) | (1u << 4));
    const float z = 0.5f - 1;
    const float v[] = {x0, -1, z, 0, 0, 1, x1, -1, z, 0, 1, 1, x1, 1, z, 0, 1, 0, x0, 1, z, 0, 0, 0};
    DrawDecoded(0x80, VL_TEX0, reinterpret_cast<const uint8_t*>(v), 4);
}
void Floor(unsigned r, unsigned gr, unsigned b) {
    SetupTarget();
    Plain();
    Color0(r, gr, b, 255);
    ClipQuad(-1, -1, 0, 1, 0.5f);
    Color0(b, r, gr, 255);
    ClipQuad(0, -1, 1, 1, 0.5f);
}
}

static uint32_t g_testCuts = 0;
extern "C" uint32_t port_camera_cut_count(void) { return g_testCuts; }
int main() {
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(64, 64), "production backend init");
    if (failures) return 1;
    ResetState();
    g.efbW = g.efbH = 64;
    QueueSetTarget();
    BP(0x43, 1);
    BP(0x20, 342u | (342u << 12));
    BP(0x21, 405u | (405u << 12));
    BP(0x59, 171u | (171u << 10));
    const uint32_t m[] = {f2u(1), 0, 0, 0, 0, f2u(1), 0, 0, 0, 0, f2u(1), 0};
    WriteXF(0, 12, m);
    OrthoClip();
    Viewport();
    FillZ(zTex1, 0xFFFFFF);
    FillZ(zTex0, 0);
    FillZ(zTexHalf, 0x7FFFFF);
    std::memset(grey, 0xFF, sizeof grey);
    const long clearq = Env("GXV_CLEARQ", 0);
    std::vector<std::vector<uint8_t>> pic;

    // ---- part A
    unsigned seed = 1;
    auto frame = [&](const Clear& c, const char* what) {
        Junk(seed++);
        ClearEfbQuad(c);
        Readers();
        pic.push_back(Finish(what));
    };
    { Clear c; frame(c, "black (0,0,0,0) z=1, dst alpha 0: the game's own clear"); }                                  // 1 converted
    { Clear c; c.r = c.g = c.b = c.a = 255; c.z = zTex0; frame(c, "white (255) z=0, dst alpha 255"); }               // 2 converted
    { Clear c; c.g = 255; c.a = 255; c.dstAlpha = false; frame(c, "(0,255,0) alpha from A0 = 255, no dst alpha"); }  // 3 converted
    { Clear c; c.r = 30; c.g = 200; frame(c, "colour bytes not 0/255 (a quad)"); }                                    // 4 not
    { Clear c; c.blend = true; c.a = 255; c.r = 255; frame(c, "blending on (a quad)"); }                              // 5 not
    { Clear c; c.scissorW = 48; frame(c, "scissor 48 wide (a quad, the rest keeps the last picture)"); }              // 6 not
    { Clear c; c.cull = 2; frame(c, "GX_CULL_FRONT: culled whole, nothing is cleared"); }                              // 7 not
    { Clear c; c.z = zTexHalf; frame(c, "z about 0.5 (a quad)"); }                                                    // 8 not
    { Clear c; c.dstAlpha = false; frame(c, "black, alpha from A0 = 0"); }                                            // 9 converted
    { Clear c; c.b = 255; frame(c, "blue, z=1, dst alpha 0 (alpha byte 0, b 255)"); }                                 // 10 converted
    { Clear c; c.b = 255; c.z = zTex0; frame(c, "blue, z=0"); }                                                       // 11 converted
    { Clear c; frame(c, "black z=1 again (steady state, a second picture)"); }                                        // 12 converted
    // arbitrary constant colours, alpha, depth-only and colour-only clears (frames 13-)
    { Clear c; c.r = 30; c.g = 30; c.b = 200; frame(c, "the game's own clear colour (30,30,200,0), dst alpha 0"); }
    static const unsigned spread[6][4] = {{1, 2, 3, 4}, {127, 128, 129, 130}, {254, 253, 252, 251}, {17, 34, 51, 68}, {100, 150, 200, 250}, {255, 254, 1, 0}};
    for (int i = 0; i < 6; ++i) {
        Clear c; c.r = spread[i][0]; c.g = spread[i][1]; c.b = spread[i][2]; c.a = spread[i][3]; c.dstAlpha = false; c.z = i & 1 ? zTex0 : zTex1;
        frame(c, "colour and alpha from the registers, any bytes");
    }
    { Clear c; c.r = 12; c.g = 200; c.b = 99; c.a = 33; c.dstAlphaValue = 77; frame(c, "colour (12,200,99), destination alpha constant 77"); }
    { Clear c; c.useReg = true; c.rr = 300; c.rg = -20; c.rb = 100; c.ra = 255; c.dstAlpha = false; frame(c, "registers outside 0..255 (input D, clamped)"); }
    { Clear c; c.planes = 0; frame(c, "depth-only clear (DrawUtil::clearZBuffer), z=1"); }
    { Clear c; c.planes = 0; c.z = zTex0; frame(c, "depth-only clear, z=0"); }
    { Clear c; c.r = 90; c.g = 10; c.b = 250; c.a = 20; c.noDepth = true; c.dstAlpha = false; frame(c, "colour-only fill (no depth test)"); }
    { Clear c; c.planes = 1; frame(c, "colour without alpha: no such clear, stays a quad"); } // not converted
    // sanity: the z=1 and z=0 pictures differ (the depth is read), so the test cannot pass on two blank pictures
    Check(Hash(pic[1]) != Hash(pic[0]), "the clear's colour and depth are in the picture");
    Check(Hash(pic[10]) != Hash(pic[9]), "z=1 and z=0 clears read differently");
    Check(Hash(pic[6]) != Hash(pic[0]), "a culled clear quad leaves the last picture's content");
    uint64_t fc[8] = {}, bc[8] = {};
    vgx::ClearQFrontCounters(fc);
    backend::ClearQBackCounters(bc);
    std::printf("clearq: front seen=%llu marked=%llu refused key=%llu state=%llu colour=%llu depth=%llu alpha=%llu extent=%llu; back met=%llu made=%llu "
                "refused count-only=%llu shape=%llu stencil=%llu pass=%llu extent=%llu planes=%llu\n",
                (unsigned long long)fc[0], (unsigned long long)fc[1], (unsigned long long)fc[2], (unsigned long long)fc[3], (unsigned long long)fc[4],
                (unsigned long long)fc[5], (unsigned long long)fc[6], (unsigned long long)fc[7], (unsigned long long)bc[0], (unsigned long long)bc[1],
                (unsigned long long)bc[2], (unsigned long long)bc[3], (unsigned long long)bc[4], (unsigned long long)bc[5], (unsigned long long)bc[6],
                (unsigned long long)bc[7]);
    { char cen[640]; vgx::ClearQFrontCensus(cen, sizeof cen); std::printf("clearq census: %s\n", cen); }
    std::printf("clearq %s\n", bc[1] > 0 ? "converted>0" : "converted=0");
    // converted: frames 1 2 3 4 9 10 11 12 (8), the game's colour (1), the spread (6), the destination-alpha constant (1), out-of-range registers (1),
    // two depth-only, one colour-only (3): 20; not: blend, scissor, cull front, z 0.5, colour without alpha
    if (clearq == 1 || clearq == 3) Check(bc[1] == (clearq == 1 ? 20u : 9u), "GXV_CLEARQ: the qualifying clear quads were made as clears, no other");
    if (clearq == 2) Check(bc[1] == 0 && fc[1] >= 20, "GXV_CLEARQ=2: recognised (counted) and none changed");
    if (clearq == 0) Check(bc[1] == 0 && fc[1] == 0, "GXV_CLEARQ=0: nothing recognised, nothing changed");

    // ---- part B
    {
        const uint64_t k0 = vgx::TevInPrewrapKeys();
        const auto a = PrewrapFrame(false);
        const auto b = PrewrapFrame(true);
        const uint64_t keys = vgx::TevInPrewrapKeys() - k0;
        std::printf("prewrap: keys with a wrapped-copy row=%llu %s\n", (unsigned long long)keys, keys > 0 ? "dual rows>0" : "dual rows=0");
        // out = A + D, A the low 8 bits of the register, D the register: (300,-20,100) -> (255, 216, 200); (40,60,90) -> (80,120,180);
        // (270,50,-106) -> (255, 100, 44). The read-back may be bottom-up, so the quadrants are looked up as a set.
        const Px want[4] = {{255, 216, 200}, {80, 120, 180}, {255, 100, 44}, {255, 216, 200}};
        const int cx[4] = {16, 48, 16, 48}, cy[4] = {16, 16, 48, 48};
        for (const auto* pp : {&a, &b}) {
            unsigned hit = 0;
            for (int q = 0; q < 4; ++q) {
                const Px p = At(*pp, cx[q], cy[q]);
                bool any = false;
                for (const Px& w : want) any |= PxIs(p, w.r, w.g, w.b);
                hit += any ? 1 : 0;
            }
            Check(hit == 4, pp == &a ? "out-of-range REG0 read as A and D: the low 8 bits for A, the whole value for D (no texture)"
                                    : "the same with a texture stage after it");
        }
        if (Env("GXV_TEVIN_PREWRAP", 0) == 1 && Env("GXV_PREWRAP_BREAK", 0) == 0) Check(keys > 0, "GXV_TEVIN_PREWRAP=1: the material got a wrapped-copy row");
        if (Env("GXV_TEVIN_PREWRAP", 0) == 0) Check(keys == 0, "GXV_TEVIN_PREWRAP=0: no wrapped-copy row, the key is unchanged");
    }

    // ---- part C: the capture
    {
        uint64_t c0[8], c[8];
        IndCapCounters(c0);
        // no reader at all: left out with the switch on (after the warm-up)
        for (int i = 0; i < 3; ++i) { Floor(40 + i * 20, 200, 90); MarkedCapture(); SetupTarget(); Plain(); Color0(250, 250, 10, 255); ClipQuad(-0.25f, -0.25f, 0.25f, 0.25f, 0.4f); Finish("capture, no reader"); }
        // the reader after the capture, same picture: always made
        for (int i = 0; i < 3; ++i) { Floor(200, 40 + i * 20, 120); MarkedCapture(); ReaderOfCapture(0.25f, 1); Finish("capture, then its reader (same picture)"); }
        // the reader BEFORE the capture (a reader of the previous picture's capture)
        for (int i = 0; i < 6; ++i) { Floor(10 + i * 30, 90, 220); ReaderOfCapture(-1, -0.5f); MarkedCapture(); Finish("reader before the capture (previous picture's)"); }
        IndCapCounters(c);
        std::printf("indcap: pictures=%llu marked=%llu left=%llu kept=%llu half=%llu stale=%llu held=%llu pre=%llu\n", (unsigned long long)(c[0] - c0[0]),
                    (unsigned long long)(c[1] - c0[1]), (unsigned long long)(c[2] - c0[2]), (unsigned long long)(c[3] - c0[3]), (unsigned long long)(c[4] - c0[4]),
                    (unsigned long long)(c[5] - c0[5]), (unsigned long long)(c[6] - c0[6]), (unsigned long long)(c[7] - c0[7]));
        uint64_t r[4] = {};
        IndCapRunCounters(r);
        std::printf("indcap-extra: refreshed=%llu warm-kept=%llu early-kept=%llu earlier-readers=%llu\n", (unsigned long long)r[0], (unsigned long long)r[1],
                    (unsigned long long)r[2], (unsigned long long)r[3]);
        if (Env("GXV_INDCAP_SKIP", 0) == 0) Check(c[1] - c0[1] == 0, "GXV_INDCAP_SKIP=0: no capture marked");
    }
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
