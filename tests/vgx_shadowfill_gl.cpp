// GXV_SHADOWFILL_BOUNDS (vgx_draw.cpp, "shadow fill bounds"): the silhouette / shadow-volume fill (MR::fillSilhouetteColor: a whole-EFB
// A8 alpha capture and a full-screen alpha-tested quad that reads it) left out when no draw put alpha anywhere, and scissored to the
// shadow volumes' pixels otherwise, through the production GX front end and the production GLES back end on Mesa (no game data).
// tools/test_vgx_shadowfill_gl.sh runs it with the switch off and on at several render sizes; every picture (all four channels) and the
// alpha capture's texture must be identical, and with the switch on the expected cuts must have been made.
// Each frame is a game picture in the order GameScene::draw3D draws it: a sky quad (destination alpha the constant 0), a perspective
// floor and wall (opaque, destination alpha 0), the shadow volumes (colour off, alpha ONE / ONE with one face set culled, then SUBTRACT
// with the other, depth test GEQUAL), opaque draws over part of them (destination alpha 0), sometimes a "silhouette" (alpha written behind
// the floor), the capture (GX_CTF_A8 of the whole EFB) and the fill quad (one TEV stage TEXA > konst ? A0 : 0, alpha test GREATER 0,
// blended SRCALPHA / INVSRCALPHA, destination alpha 0), a translucent quad without alpha update; then the read-back and the display copy
// that clears to (0, 0, 0, 0).
//   Frame 1: no volume (nothing known yet: full)                           Frame 7: three volumes and a silhouette (scissored)
//   Frame 2: no volume (left out: the display copy's clear carried over)    Frame 8: a volume partly off the screen's edge (scissored)
//   Frame 3: no volume (left out, the capture not made at all)             Frame 9: a corner pass clearing alpha to 255 (scissored)
//   Frame 4: one small volume (scissored)                                   Frame 10: no volume (left out, capture a fill of 0)
//   Frame 5: a volume through the near plane (full)                         Frame 11: a volume through the near plane at the side (full)
//   Frame 6: the camera inside a volume (full)                              Frame 12: a fade writing alpha, then a sky covering it with 0 (left out)
//   Frames 13-16: a volume drawn from a display list in guest RAM (the geometry cache's static buffers: its bounds from the list's
//   bounds through the matrix), and in 15-16 also a two-matrix "skinned" silhouette list (per-vertex matrix rows, the palette) (scissored)
#include "vgx_state.h"
#include "vgx_hle.h"
#include <GLES2/gl2.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
unsigned g_staticBoundedFrame = 0;
void SubmitToBackend(CmdBuf& b) {
    for (const DrawRec& d : b.draws) g_staticBoundedFrame += d.vbuf != 0 && d.ab != 0;
    if (std::getenv("SF_DUMP")) {
        for (size_t i = 0; i < b.cmds.size(); ++i) {
            const Cmd& c = b.cmds[i];
            if (c.type == CMD_DRAW) {
                const DrawRec& d = b.draws[c.arg];
                std::printf("  %2zu draw: %u idx, vbuf %u, ex %d,%d %dx%d, sc %d,%d %dx%d, ab %u %d,%d %dx%d, blend %u cw %u aw %u dz %u tex %u\n", i, d.idxCount, d.vbuf,
                            d.exX, d.exY, d.exW, d.exH, d.rs.scX, d.rs.scY, d.rs.scW, d.rs.scH, d.ab, d.abX, d.abY, d.abW, d.abH, d.rs.blend, d.rs.colorWrite,
                            d.rs.alphaWrite, d.rs.dstAlphaZero, d.texMask ? d.tex[0].texId : 0u);
            } else if (c.type == CMD_COPY_TEX) {
                const CopyTexCmd& t = b.copyTex[c.arg];
                std::printf("  %2zu copy -> %u: src %d,%d %dx%d, fromPass %u, skip %u, fill %u, clear %u\n", i, t.texId, t.src.x, t.src.y, t.src.w, t.src.h, t.fromPass,
                            t.skip, t.fill, t.clear.enable);
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
constexpr int kEfb = 128;
void Put32(uint8_t* p, uint32_t v) {
    for (unsigned i = 0; i != 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}
alignas(32) unsigned char alphaDest[32];
alignas(32) unsigned char cornerDest[32];
alignas(32) unsigned char readDestination[32];
alignas(4096) uint8_t ram[1 << 14]; // guest RAM for the display lists
unsigned failures = 0;
unsigned g_staticBounded = 0; // records drawn from a static buffer whose pixels were worked out (DrawRec::ab)
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
unsigned g_cull = 0; // genmode cull bits (1 back, 2 front)
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
// Projection: orthographic identity (the vertices are clip coordinates) or a perspective camera at the origin looking down -z.
void Ortho() {
    const uint32_t proj[] = {f2u(1), 0, f2u(1), 0, f2u(1), 0, 1};
    WriteXF(0x1020, 7, proj);
}
void Persp() {
    const float n = 0.5f, f = 100.0f;
    const uint32_t proj[] = {f2u(1.4f), 0, f2u(1.4f), 0, f2u(-n / (f - n)), f2u(-f * n / (f - n)), 0};
    WriteXF(0x1020, 7, proj);
}
void Tri(std::vector<float>& v, const float* a, const float* b, const float* c) {
    for (const float* p : {a, b, c}) { v.push_back(p[0]); v.push_back(p[1]); v.push_back(p[2]); v.push_back(0); }
}
// An axis-aligned box (eye space), its faces wound the same way seen from outside.
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
// Display lists in guest RAM: a unit box (positions indexed 16-bit, one matrix: the current one), and a "skinned" one whose lower half
// uses matrix rows 3..5 and upper half rows 6..8 (a direct PNMTXIDX per vertex), as the game's skinned models are drawn.
uint8_t* g_boxPos = nullptr;
uint8_t* g_boxList = nullptr;
uint8_t* g_skinList = nullptr;
uint32_t g_boxListBytes = 0, g_skinListBytes = 0;
const int kBoxFaces[6][4] = {{4, 5, 6, 7}, {1, 0, 3, 2}, {0, 4, 7, 3}, {5, 1, 2, 6}, {7, 6, 2, 3}, {0, 1, 5, 4}};
void MakeLists() {
    uint32_t top = 0;
    auto take = [&](uint32_t n) { uint8_t* p = ram + top; top = (top + n + 63u) & ~63u; return p; };
    g_boxPos = take(8 * 12);
    for (int k = 0; k < 8; ++k) {
        Put32(g_boxPos + k * 12, f2u((k == 1 || k == 2 || k == 5 || k == 6) ? 1.0f : 0.0f));
        Put32(g_boxPos + k * 12 + 4, f2u((k == 2 || k == 3 || k == 6 || k == 7) ? 1.0f : 0.0f));
        Put32(g_boxPos + k * 12 + 8, f2u(k >= 4 ? 1.0f : 0.0f));
    }
    g_boxListBytes = 3 + 36 * 2;
    g_skinListBytes = 3 + 36 * 3;
    g_boxList = take(g_boxListBytes);
    g_skinList = take(g_skinListBytes);
    uint8_t* p = g_boxList;
    uint8_t* q = g_skinList;
    *p++ = 0x90; *p++ = 0; *p++ = 36; // GX_TRIANGLES, vertex format 0
    *q++ = 0x90; *q++ = 0; *q++ = 36;
    for (const auto& f : kBoxFaces)
        for (int t = 0; t < 2; ++t)
            for (int k = 0; k < 3; ++k) {
                const int ix = f[t == 0 ? k : (k == 0 ? 0 : k + 1)];
                *p++ = 0; *p++ = static_cast<uint8_t>(ix);
                *q++ = static_cast<uint8_t>((ix == 2 || ix == 3 || ix == 6 || ix == 7) ? 6 : 3); // (PNMTXIDX: the row)
                *q++ = 0; *q++ = static_cast<uint8_t>(ix);
            }
    hle::RegisterGuestRam(ram, sizeof ram);
}
// The rows of an affine matrix that maps the unit box onto [x0, x1] x [y0, y1] x [z0, z1].
void BoxRows(unsigned row, float x0, float x1, float y0, float y1, float z0, float z1) {
    const float m[12] = {x1 - x0, 0, 0, x0, 0, y1 - y0, 0, y0, 0, 0, z1 - z0, z0};
    uint32_t w[12];
    for (int i = 0; i < 12; ++i) w[i] = f2u(m[i]);
    WriteXF(row * 4, 12, w);
}
constexpr uint32_t kMatIdxBase = 0x3CF3CF00u & ~0x3Fu; // (texture matrices: identity)
void ListBox(bool skinned) {
    SetVcd(0, skinned ? 1 : 0); // PNMTXIDX direct
    SetVcd(9, 3);               // position, 16-bit index
    SetVat(0, 9, 1, 4, 0);      // xyz, f32
    g.arrays[9] = {g_boxPos, 8 * 12, 12, false};
    g.cpArrayStale = 0;
    if (skinned) CallList(g_skinList, g_skinListBytes);
    else CallList(g_boxList, g_boxListBytes);
    SetVcd(0, 0);
}
// A shadow volume drawn from the box list through matrix slot 1 (rows 3..5), then the matrix back to slot 0.
void ListVolume(float x0, float x1, float y0, float y1, float z0, float z1) {
    BoxRows(3, x0, x1, y0, y1, z0, z1);
    XF(0x1018, kMatIdxBase | 3);
    Persp();
    DstAlpha(false, 0);
    ZMode(true, 6, false);
    Color(0xC0, 0xC0, 0, 0x40);
    g_cull = 1;
    Plain();
    BlendReg(1u | kAU | (1u << 5) | (1u << 8));
    ListBox(false);
    g_cull = 2;
    Plain();
    BlendReg(1u | (1u << 11) | kAU);
    ListBox(false);
    g_cull = 0;
    Plain();
    XF(0x1018, kMatIdxBase);
}
// The skinned silhouette: the lower half of the box through rows 3..5, the upper through rows 6..8 (higher, wider and well to the right:
// a bound that took one matrix for every row would miss it by more than the margins).
void ListSilhouette(float x0, float x1, float y0, float y1, float z0, float z1) {
    BoxRows(3, x0, x1, y0, y1, z0, z1);
    BoxRows(6, x0 + 2.4f, x1 + 2.6f, y0 + 0.15f, y1 + 0.15f, z0, z1);
    Persp();
    DstAlpha(false, 0);
    ZMode(true, 4, false);
    Color(0, 0, 0, 0x60);
    Plain();
    BlendReg(kAU);
    ListBox(true);
}
// A quad in eye space (four corners).
void Quad3(const float a[3], const float b[3], const float c[3], const float d[3]) {
    std::vector<float> v;
    Tri(v, a, b, c);
    Tri(v, a, c, d);
    DrawDecoded(0x90, 0, reinterpret_cast<const uint8_t*>(v.data()), static_cast<uint32_t>(v.size() / 4));
}
// A quad from (l, b) to (r, t) in clip coordinates (Ortho), at depth d; textured: coordinates 0..1 (t = 0 at the top).
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
void ClearColour(unsigned r, unsigned green, unsigned b, unsigned a, unsigned z24) {
    BP(0x4F, r | (a << 8));
    BP(0x50, b | (green << 8));
    BP(0x51, z24);
}
uint32_t g_alphaTex = 0;
uint64_t Hash(const std::vector<uint8_t>& p) {
    uint64_t h = 14695981039346656037ull;
    for (uint8_t b : p) h = (h ^ b) * 1099511628211ull;
    return h;
}
// The scene: sky, floor, a wall (all opaque, destination alpha 0). coverSky: the sky quad is drawn without a depth test (a rectangle over
// the whole EFB: DrawRec::cover), after a full-screen fade that writes alpha; else it is depth-tested (LEQUAL at the far plane: not a
// cover), so what is known of alpha comes from the display copy's clear of the frame before.
void Scene(bool coverSky) {
    Viewport(0, 0, kEfb, kEfb);
    Scissor(0, 0, kEfb, kEfb);
    g_cull = 0;
    Plain();
    Ortho();
    if (coverSky) {
        DstAlpha(false, 0);
        ZMode(false, 7, false);
        BlendReg(1u | kCU | kAU | (5u << 5) | (4u << 8));
        Color(255, 255, 255, 90);
        QuadRect(-1, 1, -1, 1, 0.5f);
    }
    DstAlpha(true, 0);
    ZMode(coverSky ? false : true, 3, false);
    BlendReg(kCU | kAU);
    Color(60, 90, 200, 255);
    QuadRect(-1, 1, -1, 1, 1.0f);
    Persp();
    ZMode(true, 3, true);
    Color(120, 160, 90, 200);
    const float f0[3] = {-8, -1, -1}, f1[3] = {8, -1, -1}, f2[3] = {8, -1, -40}, f3[3] = {-8, -1, -40};
    Quad3(f0, f1, f2, f3);
    Color(170, 120, 100, 90);
    const float w0[3] = {1.5f, -1, -9}, w1[3] = {4, -1, -9}, w2[3] = {4, 2, -9}, w3[3] = {1.5f, 2, -9};
    Quad3(w0, w1, w2, w3);
}
// A shadow volume (ShadowVolumeDrawer::draw): colour off, alpha ONE / ONE with one face set culled, then SUBTRACT with the other.
void Volume(float x0, float x1, float y0, float y1, float z0, float z1) {
    Persp();
    DstAlpha(false, 0);
    ZMode(true, 6, false); // GEQUAL
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
}
// The fill (MR::fillSilhouetteColor) after its capture.
void CaptureAndFill() {
    Viewport(0, 0, kEfb, kEfb);
    Scissor(0, 0, kEfb, kEfb);
    Copy(alphaDest, 0x27, EfbRect{0, 0, kEfb, kEfb}, false); // GX_CTF_A8, no clear
    g_alphaTex = Pending().copyTex.back().texId;
    Ortho();
    g_cull = 0;
    BP(0x80, (1u << 4) | (4u << 5)); // (texture unit 0: clamp, bilinear)
    auto& t = g.tex[0];
    t = TexSlot{};
    t.valid = true;
    t.obj.data = alphaDest;
    t.obj.width = kEfb;
    t.obj.height = kEfb;
    t.obj.format = 1; // GX_TF_I8
    g.bpTexStale = 0;
    g.texDirty = true;
    g.texSlotDirty = 1;
    XF(0x103F, 1);
    XF(0x1040, 5u << 7);
    GenMode(1, 1);
    BP(0x28, (1u << 6) | (7u << 7));
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));                                // colour = C0
    BP(0xC1, (4u << 13) | (6u << 10) | (1u << 7) | (7u << 4) | (3u << 16) | (1u << 19) | (3u << 20)); // alpha = TEXA > KONST (A8) ? A0 : 0
    BP(0xF6, 0x1Cu << 9);                                                                                // konst alpha K0_A
    BP(0xE0, (1u << 12) | (1u << 23));                                                                   // K0 = (0, 0, 0, 1)
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
std::vector<uint8_t> g_alphaPixels;
std::vector<uint8_t> Finish() {
    // a translucent quad without alpha update
    Plain();
    Ortho();
    ZMode(false, 7, false);
    BlendReg(1u | kCU | (5u << 5) | (4u << 8));
    Color(250, 240, 30, 100);
    QuadRect(-0.3f, 0.6f, -0.8f, -0.2f, 0.5f);
    Scissor(0, 0, kEfb, kEfb);
    Copy(readDestination, 6, EfbRect{0, 0, kEfb, kEfb}, false);
    const auto id = Pending().copyTex.back().texId;
    // the display copy, clearing to (0, 0, 0, 0) and the far depth (MainLoopFramework::prepareCopyDisp: z and alpha updates on, as with
    // mUseAlpha, which LogoScene sets for the game's scenes; a copy clears only the planes the updates allow, MakeClear)
    ZMode(true, 3, true);
    BlendReg(kCU | kAU);
    ClearColour(0, 0, 0, 0, 0xFFFFFF);
    g.dispCopySrc = EfbRect{0, 0, kEfb, kEfb};
    QueueCopyDisp(true);
    FrontPresent(false, nullptr);
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    Check(backend::DebugReadTexture(id, pixels, w, h) && w > 0 && h > 0, "picture read back");
    g_alphaPixels.clear();
    Check(backend::DebugReadTexture(g_alphaTex, g_alphaPixels, w, h) && w > 0 && h > 0, "alpha capture read back");
    return pixels;
}
std::vector<uint8_t> Frame(int n) {
    Scene(n == 12);
    switch (n) {
    case 4: Volume(-1.5f, -0.9f, -1.6f, -0.4f, -7.0f, -6.4f); break; // one small volume
    case 5: Volume(-2.0f, 0.5f, -1.6f, 0.2f, -3.0f, 0.3f); break;     // through the near plane (z 0.3 behind the eye)
    case 6: Volume(-3.0f, 3.0f, -3.0f, 3.0f, -5.0f, 2.0f); break;     // the camera inside
    case 7:
        Volume(-2.5f, -1.8f, -1.8f, -0.3f, -8.0f, -7.2f);
        Volume(0.3f, 0.9f, -1.5f, -0.5f, -5.0f, -4.4f);
        Volume(2.0f, 2.6f, -1.6f, -0.6f, -14.0f, -13.0f);
        {
            // the player's silhouette (DrawType 0x28): alpha written where it is behind the floor (depth GREATER), colour off
            Persp();
            DstAlpha(false, 0);
            ZMode(true, 4, false);
            Color(0, 0, 0, 0x60);
            Plain();
            BlendReg(kAU);
            Box(-0.4f, 0.2f, -1.4f, -0.6f, -10.0f, -9.4f);
        }
        break;
    case 8: Volume(3.2f, 5.5f, -1.6f, -0.3f, -5.0f, -4.0f); break; // partly beyond the right edge
    case 9: {
        // a corner pass whose copy clears alpha to 255 (BloomEffect's copy clear), then a small volume
        Plain();
        Ortho();
        Viewport(0, 0, 32, 32);
        Scissor(0, 0, 32, 32);
        ZMode(false, 7, false);
        BlendReg(kCU | kAU);
        DstAlpha(false, 0);
        Color(200, 30, 30, 140);
        QuadRect(-1, 1, -1, 1, 0.5f);
        ClearColour(0, 0, 0, 255, 0xFFFFFF);
        Copy(cornerDest, 6, EfbRect{0, 0, 32, 32}, true);
        Viewport(0, 0, kEfb, kEfb);
        Scissor(0, 0, kEfb, kEfb);
        Volume(-1.0f, -0.4f, -1.6f, -0.4f, -6.0f, -5.4f);
        break;
    }
    // through the near plane at the side: its corners behind the eye project to the other side, so the hull of the projected
    // corners misses the near part (what the "w not safely above 0: not known" rule guards against)
    case 11: Volume(1.0f, 2.2f, -1.8f, -0.2f, -6.0f, 3.0f); break;
    case 13: case 14: case 15: case 16:
        ListVolume(-2.9f + 0.1f * n, -1.1f + 0.1f * n, -1.6f, -0.4f, -7.0f, -6.3f); // (wide: a bound from part of the box would miss it)
        if (n >= 15) ListSilhouette(0.2f, 0.8f, -1.5f, -0.6f, -8.0f, -7.4f);
        break;
    default: break;
    }
    // opaque draws over part of the volumes (destination alpha 0 again there)
    Persp();
    Plain();
    DstAlpha(true, 0);
    ZMode(true, 3, true);
    BlendReg(kCU | kAU);
    Color(90, 60, 40, 255);
    const float p0[3] = {-1.2f, -1.0f, -6.6f}, p1[3] = {-0.9f, -1.0f, -6.6f}, p2[3] = {-0.9f, 0.5f, -6.6f}, p3[3] = {-1.2f, 0.5f, -6.6f};
    Quad3(p0, p1, p2, p3);
    CaptureAndFill();
    return Finish();
}
}

int main() {
    const int rw = std::getenv("SF_RW") ? std::atoi(std::getenv("SF_RW")) : kEfb;
    const int rh = std::getenv("SF_RH") ? std::atoi(std::getenv("SF_RH")) : kEfb;
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(rw, rh), "production backend init");
    if (failures) return 1;
    ResetState();
    g.efbW = g.efbH = kEfb;
    QueueSetTarget();
    BP(0x43, 1); // GX_PF_RGBA6_Z24
    Scissor(0, 0, kEfb, kEfb);
    BP(0x59, 171u | (171u << 10));
    const uint32_t m[] = {f2u(1), 0, 0, 0, 0, f2u(1), 0, 0, 0, 0, f2u(1), 0};
    WriteXF(0, 12, m);
    Ortho();
    MakeLists();
    uint64_t nonzero = 0;
    for (int n = 1; n <= 16; ++n) {
        g_staticBoundedFrame = 0;
        const auto p = Frame(n);
        if (n >= 13) g_staticBounded += g_staticBoundedFrame;
        uint64_t nz = 0;
        for (size_t i = 3; i < g_alphaPixels.size(); i += 4) nz += g_alphaPixels[i] > 1;
        nonzero += nz;
        std::printf("frame%d hash=%016llx alpha=%016llx nonzero=%llu\n", n, static_cast<unsigned long long>(Hash(p)),
                    static_cast<unsigned long long>(Hash(g_alphaPixels)), static_cast<unsigned long long>(nz));
    }
    uint64_t c[5];
    ShadowFillCounters(c);
    std::printf("cuts skipped=%llu scissored=%llu full=%llu fills=%llu fills_not_needed=%llu static_bounded=%u\n", static_cast<unsigned long long>(c[0]),
                static_cast<unsigned long long>(c[1]), static_cast<unsigned long long>(c[2]), static_cast<unsigned long long>(c[3]),
                static_cast<unsigned long long>(c[4]), g_staticBounded);
    Check(nonzero > 0, "the volumes put alpha on the screen (the fill has something to darken)");
    if (ShadowFillOn()) {
        // (GXV_SOFTXFORM=0: the sky quad is not CPU-transformed, so its pixels, and with them DrawRec::cover, are not known: frame 12 is full;
        // lists this small become static buffers, whose pixels come from the lists' bounds)
        const char* soft = std::getenv("GXV_SOFTXFORM");
        const bool noSoft = soft && std::strcmp(soft, "0") == 0;
        Check(c[0] == (noSoft ? 3u : 4u), "fill left out in frames 2, 3, 10 (the display copy's clear carried over) and 12 (the sky covers a fade's alpha)");
        Check(c[1] == 8, "fill scissored in frames 4, 7, 8, 9 and 13-16");
        Check(c[2] == (noSoft ? 5u : 4u), "fill left whole in frames 1 (nothing known yet), 5 and 11 (volumes through the near plane) and 6 (the camera inside one)");
        Check(c[3] == (noSoft ? 3u : 4u) && c[4] == 1, "capture made a fill of 0 in frames 2, 3, 10 and 12; in frame 3 not even that (the texture held 0)");
        if (noSoft)
            Check(g_staticBounded >= 6, "frames 13-16: the display-list volumes and the skinned silhouette drawn from static buffers with their pixels known");
    } else {
        Check(c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0, "switch off: nothing cut");
    }
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
