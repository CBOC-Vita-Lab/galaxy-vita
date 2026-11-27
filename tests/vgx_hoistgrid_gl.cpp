// GXV_HOIST_GRID (vgx_backend_gl.cpp, "GXV_HOIST_GRID"): the player's shadow corner copy hoisted on the EFB's own pixel grid, through the
// production GX front end and the production GLES back end on Mesa (no game data). Each frame is a game picture in the order
// GameScene::draw3D draws it, at the game's EFB size (640x456):
//   the world (sky, floor, wall; alpha shadow volumes, a silhouette), the whole-EFB A8 capture and the silhouette fill that reads it
//   (MR::fillSilhouetteColor), then CollisionShadow::drawAndCaptureTex: a "model" drawn twice into the EFB's top-right 160x160 corner
//   (a wide viewport first, then the corner's own, scissor one pixel inside, no depth test, colour and alpha written), a disc that
//   multiplies the corner's alpha (GX_BL_DSTALPHA / GX_BL_ZERO, alpha only), the A8 copy of the corner; clearAlphaBuffer (the corner's
//   alpha to 0); the shadow drawn from the corner's texture; more world; the "Indirect" RGB565 capture read by a quad at shifted
//   coordinates; the picture read back; the display copy.
// Frame 1 is not hoisted (its new shaders' commands come between the copies: only draws may), frame 2 puts a shadow volume over the corner before the capture (the corner's alpha depends on the EFB), frame 4 draws the model with
// a depth test (the hoist must be refused: the EFB's depth is not in the pass), frame 6 draws the model twice as large (the margin's
// pixels), frame 8 leaves the disc out; in frames 9 and 10 the capture is RGBA8 (no conversion), so that GXV_EFBSWAP hands the EFB's
// buffer over (the hoist is then made between the EFB's scene end and the draw-back quad); frame 9 has a new fill shader (no hoist).
// HG_NOCAPTURE=1 (imp45, tools/test_vgx_cornerpass_gl.sh): the capture and the fill are left out, as GXV_SHADOW_STENCIL leaves the picture: the corner copy then has no
// first copy, and GXV_CORNERPASS=1 must make it from a pass at the picture's start (frames 1, 2 and 4 excepted: rectangle not yet known, alpha over the corner, depth test).
// tools/test_vgx_hoistgrid_gl.sh runs it with GXV_HOIST_GRID=0 / 1 / 2, GXV_EFBSWAP2 0 / 1 and several render sizes: every picture and
// every corner texture (all four channels) must be identical, and with =1 the EFB must have one scene less in every frame but 4.
#include "vgx_state.h"
#include <GLES2/gl2.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) {
    if (std::getenv("HG_DUMP")) {
        for (size_t i = 0; i < b.cmds.size(); ++i) {
            const Cmd& c = b.cmds[i];
            if (c.type == CMD_DRAW) {
                const DrawRec& d = b.draws[c.arg];
                std::printf("  %2zu draw: %u idx, ex %d,%d %dx%d, sc %d,%d %dx%d, vp %.1f,%.1f %.1fx%.1f, blend %u cw %u aw %u zt %u tex %u\n", i, d.idxCount, d.exX, d.exY,
                            d.exW, d.exH, d.rs.scX, d.rs.scY, d.rs.scW, d.rs.scH, d.rs.vpX, d.rs.vpY, d.rs.vpW, d.rs.vpH, d.rs.blend, d.rs.colorWrite, d.rs.alphaWrite,
                            d.rs.depthTest, d.texMask ? d.tex[0].texId : 0u);
            } else if (c.type == CMD_COPY_TEX) {
                const CopyTexCmd& t = b.copyTex[c.arg];
                std::printf("  %2zu copy -> %u: src %d,%d %dx%d, dst %ux%u, fromPass %u, skip %u, fill %u, clear %u, swapOk %u\n", i, t.texId, t.src.x, t.src.y, t.src.w,
                            t.src.h, t.dstW, t.dstH, t.fromPass, t.skip, t.fill, t.clear.enable, t.swapOk);
            } else {
                std::printf("  %2zu command %u\n", i, static_cast<unsigned>(c.type));
            }
        }
    }
    backend::Execute(b);
}
}
namespace {
using namespace vgx;
constexpr int kEfbW = 640, kEfbH = 456, kCorner = 160;
// (the front end finds a copy's texture by the guest range the copy covers: destinations far enough apart that no copy's range holds another's)
alignas(32) unsigned char alphaDest[1 << 20];
alignas(32) unsigned char cornerDest[1 << 20];
alignas(32) unsigned char indDest[1 << 20];
alignas(32) unsigned char readDestination[1 << 20];
unsigned failures = 0;
void Check(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
void XF(unsigned reg, unsigned value) { WriteXF(reg, 1, &value); }
void Viewport(float x, float y, float w, float h) {
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
unsigned g_cull = 0;
void GenMode(unsigned texGens, unsigned stages) { BP(0x00, texGens | ((stages - 1) << 10) | (g_cull << 14)); }
void Plain() { // no texture: colour and alpha = C0
    GenMode(0, 1);
    XF(0x103F, 0);
    BP(0x28, 7u << 7);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
}
void ZMode(bool test, unsigned func, bool update) { BP(0x40, (test ? 1u : 0u) | (func << 1) | (update ? 1u << 4 : 0u)); }
void BlendReg(unsigned v) { BP(0x41, v); }
constexpr unsigned kCU = 1u << 3, kAU = 1u << 4;
void DstAlpha(bool on, unsigned a) { BP(0x42, (on ? 0x100u : 0u) | a); }
void Ortho() {
    const uint32_t proj[] = {f2u(1), 0, f2u(1), 0, f2u(1), 0, 1};
    WriteXF(0x1020, 7, proj);
}
void Persp() {
    const float n = 0.5f, f = 100.0f;
    const uint32_t proj[] = {f2u(1.4f), 0, f2u(1.4f * kEfbW / kEfbH), 0, f2u(-n / (f - n)), f2u(-f * n / (f - n)), 0};
    WriteXF(0x1020, 7, proj);
}
void Tri(std::vector<float>& v, const float* a, const float* b, const float* c) {
    for (const float* p : {a, b, c}) { v.push_back(p[0]); v.push_back(p[1]); v.push_back(p[2]); v.push_back(0); }
}
void Box(float x0, float x1, float y0, float y1, float z0, float z1) {
    const float c[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}, {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
    static const int f[6][4] = {{4, 5, 6, 7}, {1, 0, 3, 2}, {0, 4, 7, 3}, {5, 1, 2, 6}, {7, 6, 2, 3}, {0, 1, 5, 4}};
    std::vector<float> v;
    for (const auto& q : f) {
        Tri(v, c[q[0]], c[q[1]], c[q[2]]);
        Tri(v, c[q[0]], c[q[2]], c[q[3]]);
    }
    DrawDecoded(0x90, 0, reinterpret_cast<const uint8_t*>(v.data()), static_cast<uint32_t>(v.size() / 4));
}
void Quad3(const float a[3], const float b[3], const float c[3], const float d[3]) {
    std::vector<float> v;
    Tri(v, a, b, c);
    Tri(v, a, c, d);
    DrawDecoded(0x90, 0, reinterpret_cast<const uint8_t*>(v.data()), static_cast<uint32_t>(v.size() / 4));
}
// A quad from (l, b) to (r, t) in clip coordinates (Ortho), at depth d; textured: coordinates u0..u1 x v0..v1 (v0 at the top).
void QuadRect(float l, float r, float b, float t, float depth, bool tex = false, float u0 = 0, float u1 = 1, float v0 = 0, float v1 = 1) {
    const float z = depth - 1;
    const float v[] = {l, b, z, 0, u0, v1, r, b, z, 0, u1, v1, r, t, z, 0, u1, v0, l, t, z, 0, u0, v0};
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
void ClearColour(unsigned r, unsigned green, unsigned b, unsigned a, unsigned z24) {
    BP(0x4F, r | (a << 8));
    BP(0x50, b | (green << 8));
    BP(0x51, z24);
}
// Texture unit 0 from a copy's destination (clamped, bilinear). (Only the slot is set, not the BP image registers: the script runs with
// GXV_TEXMEMO=0, whose memo keys on those registers.)
void BindCopy(void* data, int w, int h, unsigned format) {
    BP(0x80, (1u << 4) | (4u << 5));
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
    GenMode(1, 1);
    BP(0x28, (1u << 6) | (7u << 7));
}
uint64_t Hash(const std::vector<uint8_t>& p) {
    uint64_t h = 14695981039346656037ull;
    for (uint8_t b : p) h = (h ^ b) * 1099511628211ull;
    return h;
}
void Full() {
    Viewport(0, 0, kEfbW, kEfbH);
    Scissor(0, 0, kEfbW, kEfbH);
}
void World(int n) {
    Full();
    g_cull = 0;
    Plain();
    Ortho();
    DstAlpha(true, 0);
    ZMode(true, 3, false);
    BlendReg(kCU | kAU);
    Color(60, 90, 200, 255);
    if (std::getenv("HG_STENCIL")) { // (fix47: the picture's start as the game makes it, MainLoopFramework::clearEfb: a culled quad, depth ALWAYS + update, alpha 0: a shadow window begins)
        g_cull = 1;
        Plain();
        ZMode(true, 7, true);
        Color(60, 90, 200, 0);
        const float cq[] = {-1, 1, 0, 0, 1, 1, 0, 0, 1, -1, 0, 0, -1, -1, 0, 0}; // (clockwise as seen, as tests/vgx_shadowfill_gl.cpp ClearEfbQuad)
        DrawDecoded(0x80, 0, reinterpret_cast<const uint8_t*>(cq), 4);
        g_cull = 0;
        Plain();
        ZMode(true, 3, false);
    } else
    QuadRect(-1, 1, -1, 1, 1.0f);
    Persp();
    ZMode(true, 3, true);
    Color(120, 160, 90, 200);
    const float f0[3] = {-8, -1, -1}, f1[3] = {8, -1, -1}, f2[3] = {8, -1, -40}, f3[3] = {-8, -1, -40};
    Quad3(f0, f1, f2, f3);
    Color(170, 120, 100, 90);
    const float w0[3] = {1.5f, -1, -9}, w1[3] = {4, -1, -9}, w2[3] = {4, 2.6f, -9}, w3[3] = {1.5f, 2.6f, -9};
    Quad3(w0, w1, w2, w3);
    // shadow volumes (alpha +/- 0x40), one over the corner in frame 2
    auto volume = [](float x0, float x1, float y0, float y1, float z0, float z1) {
        Persp();
        DstAlpha(false, 0);
        ZMode(true, 6, false);
        Color(0xC0, 0xC0, 0, 0x40);
        g_cull = 1;
        Plain();
        BlendReg(1u | kAU | (1u << 5) | (1u << 8));
        Box(x0, x1, y0, y1, z0, z1);
        g_cull = 2;
        Plain();
        BlendReg(1u | (1u << 11) | kAU);
        Box(x0, x1, y0, y1, z0, z1);
        g_cull = 0;
        Plain();
    };
    volume(-1.5f + 0.05f * n, -0.7f + 0.05f * n, -1.6f, -0.3f, -7.0f, -6.2f);
    // (fix47, HG_STENCIL=1: the console's picture - a shadow volume's alpha lies over the corner in every frame but 3 (frame 7: two of them); the silhouette fill takes it away)
    const bool overCorner = std::getenv("HG_STENCIL") ? n != 3 : n == 2;
    if (overCorner) volume(2.0f, 3.9f, 0.8f, 3.4f, -10.5f, -8.5f); // (behind the wall: alpha in the top-right corner)
    if (std::getenv("HG_STENCIL") && n == 7) volume(2.6f, 3.6f, 1.2f, 3.0f, -10.0f, -9.2f);
    // the silhouette: alpha written where it is behind the floor (depth GREATER), colour off
    Persp();
    DstAlpha(false, 0);
    ZMode(true, 4, false);
    Color(0, 0, 0, 0x60);
    Plain();
    BlendReg(kAU);
    Box(-0.4f, 0.2f, -1.4f, -0.6f, -10.0f, -9.4f);
}
void CaptureAndFill(int n) {
    Full();
    // (frames 9, 10: an RGBA8 capture, no conversion: GXV_EFBSWAP hands the EFB's buffer over, and the hoist is made before its draw-back)
    Copy(alphaDest, n >= 9 ? 6 : 0x27, EfbRect{0, 0, kEfbW, kEfbH}, false); // GX_CTF_A8 (or GX_TF_RGBA8), no clear
    Ortho();
    g_cull = 0;
    BindCopy(alphaDest, kEfbW, kEfbH, n >= 9 ? 6 : 1);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));                                // colour = C0
    if (n >= 9) BP(0xC1, (4u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19)); // (frames 9, 10: alpha = TEXA)
    else BP(0xC1, (4u << 13) | (6u << 10) | (1u << 7) | (7u << 4) | (3u << 16) | (1u << 19) | (3u << 20)); // alpha = TEXA > KONST (A8) ? A0 : 0
    BP(0xF6, 0x1Cu << 9);
    BP(0xE0, (1u << 12) | (1u << 23));
    BP(0xE1, 1u << 23);
    Color(0, 0, 0, 0x80);
    BP(0xF3, (4u << 16) | (7u << 19)); // GREATER 0 AND ALWAYS
    ZMode(true, 7, false);
    DstAlpha(true, 0);
    BlendReg(1u | kCU | kAU | (5u << 5) | (4u << 8));
    QuadRect(-1, 1, -1, 1, 0.5f, true);
    DstAlpha(false, 0);
    BlendReg(kCU);
    Plain();
}
// A star of `points` triangles around the centre, rotated by `a`, radius r (clip units), in the current viewport.
void Star(float a, float r, unsigned points) {
    std::vector<float> v;
    for (unsigned k = 0; k < points; ++k) {
        const float t0 = a + 6.2831853f * k / points, t1 = t0 + 6.2831853f / points * 0.45f;
        const float c[3] = {0.05f, -0.03f, -0.5f};
        const float p0[3] = {r * std::cos(t0), r * std::sin(t0), -0.5f};
        const float p1[3] = {0.45f * r * std::cos(t1), 0.45f * r * std::sin(t1), -0.5f};
        Tri(v, c, p0, p1);
    }
    DrawDecoded(0x90, 0, reinterpret_cast<const uint8_t*>(v.data()), static_cast<uint32_t>(v.size() / 4));
}
void Disc(float r) {
    std::vector<float> v;
    const unsigned seg = 16;
    for (unsigned k = 0; k < seg; ++k) {
        const float t0 = 6.2831853f * k / seg, t1 = 6.2831853f * (k + 1) / seg;
        const float c[3] = {0, 0, -0.5f}, p0[3] = {r * std::cos(t0), r * std::sin(t0), -0.5f}, p1[3] = {r * std::cos(t1), r * std::sin(t1), -0.5f};
        Tri(v, c, p0, p1);
    }
    DrawDecoded(0x90, 0, reinterpret_cast<const uint8_t*>(v.data()), static_cast<uint32_t>(v.size() / 4));
}
// CollisionShadow::drawAndCaptureTex and clearAlphaBuffer, then the shadow drawn from the corner's texture.
void Corner(int n) {
    const float left = 608 - kCorner;
    if (std::getenv("HG_EARLYREAD") && n >= 2) { // (ss24, GXV_CORNERPASS_ALT: the shadow quad of the PREVIOUS capture is drawn before this picture's capture, as the game does: a draw reads the corner's texture before the copy)
        Full();
        Ortho();
        BindCopy(cornerDest, kCorner, kCorner, 1);
        BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
        BP(0xC1, (4u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
        BP(0xF3, (7u << 16) | (7u << 19));
        Color(240, 230, 20, 255);
        ZMode(false, 7, false);
        BlendReg(1u | kCU | (5u << 5) | (4u << 8));
        QuadRect(-0.9f, -0.2f, 0.3f, 0.9f, 0.5f, true);
        Plain();
    }
    Ortho();
    Plain();
    DstAlpha(false, 0);
    ZMode(n == 4, 3, false); // (frame 4: a depth test: the EFB's depth is not in the pass: no hoist)
    BlendReg(kCU | kAU);
    const float a = 0.4f * n, r = n == 6 ? 1.6f : 0.75f;
    Viewport(608 - (kCorner + 32), -32.0f, 64.0f + kCorner, 64.0f + kCorner);
    Scissor(static_cast<int>(left) + 1, 1, kCorner - 2, kCorner - 2);
    Color(200, 60, 30, 0xF0);
    Star(a, r * 0.8f, 5);
    Viewport(left, 0, kCorner, kCorner);
    Color(30, 200, 60, 0xFF);
    Star(-a, r, 7);
    if (n != 8) { // the disc: alpha *= 0xC0 / 255 (src DSTALPHA, dst ZERO, alpha only)
        ZMode(false, 7, false);
        Color(255, 255, 255, 0xC0);
        BlendReg(1u | kAU | (0u << 5) | (6u << 8));
        Disc(0.95f);
    }
    Copy(cornerDest, 0x27, EfbRect{static_cast<int16_t>(left), 0, kCorner, kCorner}, false); // the corner as A8
    // clearAlphaBuffer: the corner's alpha to 0
    Full();
    Viewport(left, 0, kCorner, kCorner);
    ZMode(false, 7, false);
    BlendReg(kAU);
    Color(0, 0, 0, 0);
    QuadRect(-1, 1, -1, 1, 0.5f);
    Full();
    // the shadow: a quad over the floor that darkens by the corner's texture
    BindCopy(cornerDest, kCorner, kCorner, 1);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19)); // colour = C0
    BP(0xC1, (4u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19)); // alpha = TEXA
    BP(0xF3, (7u << 16) | (7u << 19));
    Color(20, 10, 40, 255);
    ZMode(false, 7, false);
    BlendReg(1u | kCU | (5u << 5) | (4u << 8));
    QuadRect(-0.6f, 0.2f, -0.9f, -0.1f, 0.5f, true);
    Plain();
}
void IndirectAndFinish(int n, std::vector<uint8_t>& picture, std::vector<uint8_t>& corner) {
    // more world after the shadow
    Full();
    Persp();
    Plain();
    DstAlpha(true, 0);
    ZMode(true, 3, true);
    BlendReg(kCU | kAU);
    Color(90, 60, 40, 255);
    const float p0[3] = {-1.2f, -1.0f, -6.6f}, p1[3] = {-0.9f, -1.0f, -6.6f}, p2[3] = {-0.9f, 0.5f + 0.02f * n, -6.6f}, p3[3] = {-1.2f, 0.5f + 0.02f * n, -6.6f};
    Quad3(p0, p1, p2, p3);
    // the "Indirect" capture (RGB565) and a reader at shifted coordinates
    Copy(indDest, 4, EfbRect{0, 0, kEfbW, kEfbH}, false);
    Ortho();
    BindCopy(indDest, kEfbW, kEfbH, 4);
    BP(0xC0, (8u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19)); // colour = TEXC
    BP(0xC1, (1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    Color(0, 0, 0, 255);
    ZMode(false, 7, false);
    BlendReg(kCU);
    QuadRect(0.3f, 0.9f, -0.95f, -0.35f, 0.5f, true, 0.52f, 0.83f, 0.61f, 0.93f);
    Plain();
    // a translucent quad without alpha update, then the read-back
    BlendReg(1u | kCU | (5u << 5) | (4u << 8));
    Color(250, 240, 30, 100);
    QuadRect(-0.3f, 0.6f, -0.8f, -0.2f, 0.5f);
    Full();
    Copy(readDestination, 6, EfbRect{0, 0, kEfbW, kEfbH}, false);
    const auto id = Pending().copyTex.back().texId;
    uint32_t cornerId = 0;
    for (const auto& t : Pending().copyTex)
        if (t.src.w == kCorner && t.src.h == kCorner) cornerId = t.texId;
    ZMode(true, 3, true);
    BlendReg(kCU | kAU);
    ClearColour(0, 0, 0, 0, 0xFFFFFF);
    g.dispCopySrc = EfbRect{0, 0, kEfbW, kEfbH};
    QueueCopyDisp(true);
    FrontPresent(false, nullptr);
    int w = 0, h = 0;
    Check(backend::DebugReadTexture(id, picture, w, h) && w > 0 && h > 0, "picture read back");
    Check(cornerId != 0 && backend::DebugReadTexture(cornerId, corner, w, h) && w > 0 && h > 0, "corner texture read back");
}
}

// HG_SKIPHOOK=1: stand-ins for the console's vglColorLoadSkip / vglColorLoadSkipColor that only count what the back end asks for
// (GXV_COLOR_LOADSKIP: with bit 16 the scenes that begin with a clear must be asked for their clear's colour, never the zero texture).
unsigned g_skipZero = 0, g_skipColour = 0, g_skipBadColour = 0, g_skipDisarm = 0;
bool FakeSkip(bool arm) {
    if (arm) ++g_skipZero; else ++g_skipDisarm;
    return true;
}
bool FakeSkipColour(bool arm, uint32_t rgba) {
    if (!arm) { ++g_skipDisarm; return true; }
    ++g_skipColour;
    for (int i = 0; i < 4; ++i) {
        const uint32_t b = (rgba >> (8 * i)) & 0xFFu;
        if (b != 0 && b != 0xFFu) ++g_skipBadColour;
    }
    return true;
}
bool FakePresent(unsigned, int, int, int, int) { return true; } // (as the console's: the display drawn, the EFB's next scene a new one)
int main() {
    if (std::getenv("HG_SKIPHOOK")) {
        backend::SetPresentHook(FakePresent);
        backend::SetColorSkipHook(FakeSkip);
        backend::SetColorSkipColorHook(FakeSkipColour);
    }
    const int rw = std::getenv("HG_RW") ? std::atoi(std::getenv("HG_RW")) : 720;
    const int rh = std::getenv("HG_RH") ? std::atoi(std::getenv("HG_RH")) : 404;
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(rw, rh), "production backend init");
    if (failures) return 1;
    ResetState();
    g.efbW = kEfbW;
    g.efbH = kEfbH;
    QueueSetTarget();
    BP(0x43, 1); // GX_PF_RGBA6_Z24
    Scissor(0, 0, kEfbW, kEfbH);
    BP(0x59, 171u | (171u << 10));
    const uint32_t m[] = {f2u(1), 0, 0, 0, 0, f2u(1), 0, 0, 0, 0, f2u(1), 0};
    WriteXF(0, 12, m);
    Ortho();
    unsigned sceneKinds[5] = {};
    for (int n = 1; n <= 10; ++n) {
        World(n);
        if (!std::getenv("HG_NOCAPTURE")) CaptureAndFill(n); // (imp45, GXV_CORNERPASS: the picture as GXV_SHADOW_STENCIL leaves it, no whole-EFB alpha capture before the corner)
        Corner(n);
        std::vector<uint8_t> picture, corner;
        IndirectAndFinish(n, picture, corner);
        backend::DebugLastFrameSceneKinds(sceneKinds);
        if (const char* dump = std::getenv("HG_DUMPDIR")) { // (raw RGBA of the corner texture and the picture, for a pixel diff)
            char path[512];
            std::snprintf(path, sizeof path, "%s-f%d-corner.rgba", dump, n);
            if (FILE* f = std::fopen(path, "wb")) { std::fwrite(corner.data(), 1, corner.size(), f); std::fclose(f); }
            std::snprintf(path, sizeof path, "%s-f%d-picture.rgba", dump, n);
            if (FILE* f = std::fopen(path, "wb")) { std::fwrite(picture.data(), 1, picture.size(), f); std::fclose(f); }
        }
        uint64_t nz = 0;
        for (size_t i = 3; i < corner.size(); i += 4) nz += corner[i] != 0;
        std::printf("frame%d scenes=%u efb=%u copy=%u pass=%u hoist=%u hash=%016llx corner=%016llx cornerNonzero=%llu\n", n, backend::DebugLastFrameScenes(), sceneKinds[0],
                    sceneKinds[1], sceneKinds[2], sceneKinds[3], static_cast<unsigned long long>(Hash(picture)), static_cast<unsigned long long>(Hash(corner)),
                    static_cast<unsigned long long>(nz));
    }
    std::printf("hoistgrids=%u efbswaps=%u cornerpasses=%u\n", backend::DebugHoistGrids(), backend::DebugEfbSwaps(), backend::DebugCornerPasses());
    { // (fix47: the front end's corner bookkeeping and the fill bounds' totals: GXV_CORNERPASS alone must see the copy and cut nothing)
        uint64_t fe[6] = {}, sf[5] = {};
        CornerPassFeCounters(fe);
        ShadowFillCounters(sf);
        std::printf("cornerfe seen=%llu marked=%llu\n", static_cast<unsigned long long>(fe[0]), static_cast<unsigned long long>(fe[1]));
        uint64_t ff[2] = {}, sc[24] = {};
        CornerPassFeFill(ff);
        ShadowStencilCounters(sc);
        std::printf("cornerfill marked=%llu fills=%llu\n", static_cast<unsigned long long>(ff[0]), static_cast<unsigned long long>(ff[1]));
        std::printf("stencil windows=%llu taken=%llu", static_cast<unsigned long long>(sc[1]), static_cast<unsigned long long>(sc[4]));
        for (int r = 1; r < 15; ++r)
            if (sc[4 + r]) std::printf(" [%s %llu]", ShadowStencilReasonName(r), static_cast<unsigned long long>(sc[4 + r]));
        std::printf("\n");
        std::printf("shadowfill skipped=%llu scissored=%llu full=%llu fills=%llu fillsKept=%llu\n", static_cast<unsigned long long>(sf[0]), static_cast<unsigned long long>(sf[1]),
                    static_cast<unsigned long long>(sf[2]), static_cast<unsigned long long>(sf[3]), static_cast<unsigned long long>(sf[4]));
    }
    { // (GXV_SHADOW_RECT, shadowpx23: the alpha captures copied by their rectangle / whole)
        uint64_t sb[2];
        backend::DebugShadowRect(sb);
        std::printf("shadowrect copied_in_part=%llu copied_whole=%llu\n", static_cast<unsigned long long>(sb[0]), static_cast<unsigned long long>(sb[1]));
    }
    if (std::getenv("HG_SKIPHOOK"))
        std::printf("skiphook zero=%u colour=%u badcolour=%u disarm=%u\n", g_skipZero, g_skipColour, g_skipBadColour, g_skipDisarm);
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
