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

#include <cstdarg>
// The basic log (GXV_SHADOW_STENCIL's "shadow-stencil:" line, every GXV_SHADOW_STENCIL_LOG_EVERY pictures): printed here as "LOG: ...".
extern "C" void port_log_summary(const char* fmt, ...) {
    if (!std::getenv("SF_STENCIL")) return;
    char line[1200];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (std::strncmp(line, "shadow-stencil", 14) == 0 || std::strncmp(line, "stencil classes", 15) == 0) std::printf("LOG: %s\n", line);
}
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
alignas(32) unsigned char alphaDest2[32];
alignas(32) unsigned char otherDest[32];
alignas(32) unsigned char cornerDest[32];
alignas(32) unsigned char readDestination[32];
alignas(4096) uint8_t ram[1 << 14]; // guest RAM for the display lists
unsigned failures = 0;
// GXV_SHADOW_STENCIL (pass24): SF_STENCIL=1 runs frames 17-31 too (the stencil cases) and leaves out what reads the alpha capture's texture
// (it is not made when the stencil replaces it): the lines are then "frame<N> hash=<picture> scenes=<GXM scenes> taken=<windows taken>".
bool StencilRun() { static const bool on = std::getenv("SF_STENCIL") && std::atoi(std::getenv("SF_STENCIL")) == 1; return on; }
// stencil43 (SF_STENCIL=3): frames 31-39, 50, 52, 53: the player's silhouette as the game draws it (constant destination alpha), marks and volumes in both orders,
// the thresholds, the silhouette gate's hand-over. SF_NOSIL=1 leaves every silhouette out (the twin of each picture).
bool Stencil43Run() { static const bool on = std::getenv("SF_STENCIL") && std::atoi(std::getenv("SF_STENCIL")) == 3; return on; }
bool NoSil() { static const bool on = std::getenv("SF_NOSIL") && std::atoi(std::getenv("SF_NOSIL")) == 1; return on; }
unsigned g_vAlpha = 0x40; // the volumes' constant alpha (the game's is 4)
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
// gpuab23: SF_TRIPLE=1 models the game's real frame (three XFBs: JUTXfb's mBufferNum 3): the display copy does not clear
// (copyXfb_triple: GXCopyDisp(.., GX_FALSE)); each picture starts with MainLoopFramework::clearEfb: a screen quad, colour = C0 with the
// clear colour (30, 30, 200, 0), GXSetDstAlpha(TRUE, 0), no blending, z ALWAYS with update, at the far depth, GX_CULL_BACK, its corners
// (0,0) (w,0) (w,h) (0,h) in a y-down orthographic projection: clockwise on the screen, a GX front face, so it is drawn.
// SF_CLEARCULL=2 (control): the same quad under GX_CULL_FRONT: culled whole, the EFB is NOT cleared and nothing may be taken as known.
bool Triple() {
    static const bool on = std::getenv("SF_TRIPLE") && std::atoi(std::getenv("SF_TRIPLE")) == 1;
    return on;
}
void ClearEfbQuad() {
    static const unsigned cull = std::getenv("SF_CLEARCULL") ? static_cast<unsigned>(std::atoi(std::getenv("SF_CLEARCULL"))) : 1u;
    Viewport(0, 0, kEfb, kEfb);
    Scissor(0, 0, kEfb, kEfb);
    Ortho();
    g_cull = cull;
    Plain();
    ZMode(true, 7, true); // ALWAYS, update
    BlendReg(kCU | kAU);
    DstAlpha(true, 0);
    Color(30, 30, 200, 0);
    // (top-left, top-right, bottom-right, bottom-left: clockwise as seen; clip y up, so "top" is y = +1)
    const float z = 1.0f - 1;
    const float v[] = {-1, 1, z, 0, 1, 1, z, 0, 1, -1, z, 0, -1, -1, z, 0};
    DrawDecoded(0x80, 0, reinterpret_cast<const uint8_t*>(v), 4);
    g_cull = 0;
    Plain();
    DstAlpha(false, 0);
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
int g_hint = 0; // (shadow25, SF_SILH=1: PORT_GX_MARK_SILH_HINT written before the capture; 0 none)
uint64_t g_stencilHash = 0, g_stencilBytes = 0;
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
    Color(0xC0, 0xC0, 0, g_vAlpha);
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
// stencil43: only the SUBTRACT pass of a volume (alpha -= a where the face passes the GEQUAL test): what a volume cut by a clip plane leaves.
void VolumeSubOnly(float x0, float x1, float y0, float y1, float z0, float z1) {
    Persp();
    DstAlpha(false, 0);
    ZMode(true, 6, false);
    Color(0xC0, 0xC0, 0, g_vAlpha);
    g_cull = 2;
    Plain();
    BlendReg(1u | (1u << 11) | kAU);
    Box(x0, x1, y0, y1, z0, z1);
    g_cull = 0;
    Plain();
}
// stencil43: the player's silhouette as the game draws it (J3DModelX display list 3, drawSilhouette): the TEV's alpha is ZERO, colour off, no blend, depth GEQUAL without a
// write, GXSetDstAlpha(TRUE, a) (a = 0x90 = 144 in the game): where it passes the depth test the EFB's alpha becomes exactly a. Stages: 1 (the game's), 2 = a second
// stage passing the first on (a shape the classes cannot place: alpha made of the stage chain, depth GEQUAL, no blend: "silhouette-shaped").
void SilhDst(float x0, float x1, float y0, float y1, float z0, float z1, unsigned a, int stages = 1) {
    if (NoSil()) return;
    Persp();
    Plain();
    if (stages == 2) {
        // alpha = A0 through two stages (the second passes the first on): a constant the one-stage test cannot place
        GenMode(0, 2);
        BP(0xC2, (15u << 12) | (15u << 8) | (15u << 4) | 0u | (1u << 19));      // colour = the previous stage
        BP(0xC3, (7u << 13) | (7u << 10) | (7u << 7) | (0u << 4) | (1u << 19)); // alpha = the previous stage
        DstAlpha(false, 0);
        Color(0, 0, 0, a);
    } else {
        BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19)); // alpha = ZERO (the game's list: the destination alpha is the constant)
        DstAlpha(true, a);
        Color(0, 0, 0, 0);
    }
    ZMode(true, 6, false);
    BlendReg(kAU);
    Box(x0, x1, y0, y1, z0, z1);
    DstAlpha(false, 0);
    Plain();
}
// The fill (MR::fillSilhouetteColor) after its capture.
void FillState(unsigned char* dest, bool rebind = true, unsigned a0 = 0x80, unsigned dstFactor = 5);
void CaptureAndFill(unsigned char* dest = alphaDest) {
    Viewport(0, 0, kEfb, kEfb);
    Scissor(0, 0, kEfb, kEfb);
    if (g_hint) BP(0xFF, 32u | (static_cast<unsigned>(g_hint) << 8)); // (PORT_GX_MARK_SILH_HINT)
    Copy(dest, 0x27, EfbRect{0, 0, kEfb, kEfb}, false); // GX_CTF_A8, no clear
    g_alphaTex = Pending().copyTex.back().texId;
    FillState(dest);
}
void FillState(unsigned char* dest, bool rebind, unsigned a0, unsigned dstFactor) {
    Ortho();
    g_cull = 0;
    BP(0x80, (1u << 4) | (4u << 5)); // (texture unit 0: clamp, bilinear)
    if (rebind) {
        auto& t = g.tex[0];
        t = TexSlot{};
        t.valid = true;
        t.obj.data = dest;
        t.obj.width = kEfb;
        t.obj.height = kEfb;
        t.obj.format = 1; // GX_TF_I8
        g.bpTexStale = 0;
        g.texDirty = true;
        g.texSlotDirty = 1;
    }
    XF(0x103F, 1);
    XF(0x1040, 5u << 7);
    GenMode(1, 1);
    BP(0x28, (1u << 6) | (7u << 7));
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));                                // colour = C0
    BP(0xC1, (4u << 13) | (6u << 10) | (1u << 7) | (7u << 4) | (3u << 16) | (1u << 19) | (3u << 20)); // alpha = TEXA > KONST (A8) ? A0 : 0
    BP(0xF6, 0x1Cu << 9);                                                                                // konst alpha K0_A
    BP(0xE0, (1u << 12) | (1u << 23));                                                                   // K0 = (0, 0, 0, 1)
    BP(0xE1, 1u << 23);
    Color(0, 0, 0, a0);
    BP(0xF3, (4u << 16) | (7u << 19)); // GREATER 0 AND ALWAYS
    ZMode(true, 7, false);
    DstAlpha(true, 0);
    BlendReg(1u | kCU | kAU | (dstFactor << 5) | (4u << 8));
    QuadRect(-1, 1, -1, 1, 0.5f, true);
    DstAlpha(false, 0);
    BlendReg(kCU);
    Plain();
}
std::vector<uint8_t> g_alphaPixels;
bool SilhRun() { static const bool on = std::getenv("SF_SILH") && std::atoi(std::getenv("SF_SILH")) == 1; return on; }
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
    QueueCopyDisp(!Triple()); // (SF_TRIPLE=1: the game's three-XFB path, MainLoopFramework::copyXfb_triple: the display copy does not clear)
    FrontPresent(false, nullptr);
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    Check(backend::DebugReadTexture(id, pixels, w, h) && w > 0 && h > 0, "picture read back");
    g_stencilHash = 0; g_stencilBytes = 0;
    if (std::getenv("SF_STENCILHASH")) { // (mf26) the EFB's stencil plane at the picture's end, hashed; printed with the frame line
        std::vector<uint8_t> st; int sw = 0, sh = 0;
        if (backend::DebugReadEfbStencil(st, sw, sh)) { g_stencilHash = Hash(st); for (uint8_t v : st) g_stencilBytes += v != 0; }
        else g_stencilHash = ~0ull;
    }
    g_alphaPixels.clear();
    if (!StencilRun() && !SilhRun() && !Stencil43Run()) Check(backend::DebugReadTexture(g_alphaTex, g_alphaPixels, w, h) && w > 0 && h > 0, "alpha capture read back");
    return pixels;
}
std::vector<uint8_t> Frame(int n) {
    if (Triple()) ClearEfbQuad();
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
    // pass24 (GXV_SHADOW_STENCIL): the stencil cases. g_vAlpha 4 is the game's volume alpha; the rest by what the picture checks.
    case 17: // overlapping volumes: the count is 2 and 3 where they overlap
        g_vAlpha = 4;
        Volume(-2.0f, 1.0f, -1.6f, 0.2f, -9.0f, -6.0f);
        Volume(-1.0f, 2.0f, -1.6f, 0.5f, -8.0f, -5.0f);
        Volume(-0.5f, 0.5f, -1.6f, 1.0f, -7.0f, -4.5f);
        break;
    case 18: // overlapping, one through the near plane
        g_vAlpha = 4;
        Volume(-2.0f, 0.5f, -1.6f, 0.2f, -3.0f, 0.3f);
        Volume(-1.0f, 1.0f, -1.6f, 0.0f, -8.0f, -4.0f);
        break;
    case 19: { // an alpha 0 write after the volumes (BeforeSilhouette's opaque list): a wall in front of part of them
        g_vAlpha = 4;
        Volume(-2.5f, 2.5f, -1.6f, 0.5f, -9.0f, -5.0f);
        Persp();
        Plain();
        DstAlpha(true, 0);
        ZMode(true, 3, true);
        BlendReg(kCU | kAU);
        Color(200, 120, 60, 255);
        const float a0[3] = {-0.5f, -1.0f, -4.0f}, a1[3] = {1.5f, -1.0f, -4.0f}, a2[3] = {1.5f, 1.0f, -4.0f}, a3[3] = {-0.5f, 1.0f, -4.0f};
        Quad3(a0, a1, a2, a3);
        break;
    }
    case 20: { // a volume, the player's silhouette over part of it, then an alpha 0 write over part of both
        g_vAlpha = 4;
        Volume(-2.0f, 0.5f, -1.6f, 0.2f, -9.0f, -6.0f);
        Persp();
        DstAlpha(false, 0);
        ZMode(true, 4, false);
        Color(0, 0, 0, 0x60);
        Plain();
        BlendReg(kAU);
        Box(-1.0f, 1.0f, -1.4f, -0.6f, -10.0f, -9.4f);
        Plain();
        DstAlpha(true, 0);
        ZMode(true, 3, true);
        BlendReg(kCU | kAU);
        Color(40, 200, 90, 255);
        const float b0[3] = {-0.2f, -1.0f, -7.0f}, b1[3] = {0.6f, -1.0f, -7.0f}, b2[3] = {0.6f, 0.4f, -7.0f}, b3[3] = {-0.2f, 0.4f, -7.0f};
        Quad3(b0, b1, b2, b3);
        break;
    }
    case 21:
        g_vAlpha = 4;
        for (int k = 0; k < 12; ++k) Volume(-3.0f + 0.4f * k, -2.2f + 0.4f * k, -1.6f, -0.2f, -9.0f - 0.2f * k, -6.0f);
        break;
    case 22: // 70 volumes: 70 x 4 > 255, so the alpha could saturate: left as it is
        g_vAlpha = 4;
        for (int k = 0; k < 70; ++k) Volume(-3.0f + 0.07f * k, -2.4f + 0.07f * k, -1.6f, -0.2f, -9.0f, -6.0f);
        break;
    case 30: // (a second window follows the first capture: see below)
        g_vAlpha = 4;
        Volume(-2.0f, 0.5f, -1.6f, 0.2f, -9.0f, -6.0f);
        break;
    case 23: { // a translucent particle that writes alpha in the middle of the window
        g_vAlpha = 4;
        Volume(-2.0f, 0.5f, -1.6f, 0.2f, -9.0f, -6.0f);
        Persp();
        Plain();
        DstAlpha(false, 0);
        ZMode(true, 3, false);
        BlendReg(1u | kCU | kAU | (5u << 5) | (4u << 8));
        Color(250, 240, 30, 90);
        const float c0[3] = {0.5f, -1.0f, -5.0f}, c1[3] = {2.0f, -1.0f, -5.0f}, c2[3] = {2.0f, 0.5f, -5.0f}, c3[3] = {0.5f, 0.5f, -5.0f};
        Quad3(c0, c1, c2, c3);
        break;
    }
    case 24: { // a volume after the silhouette: alpha would be X + a
        g_vAlpha = 4;
        Persp();
        DstAlpha(false, 0);
        ZMode(true, 4, false);
        Color(0, 0, 0, 0x60);
        Plain();
        BlendReg(kAU);
        Box(-1.0f, 1.0f, -1.4f, -0.6f, -10.0f, -9.4f);
        Volume(-2.0f, 0.5f, -1.6f, 0.2f, -9.0f, -6.0f);
        break;
    }
    case 25: // volumes of alpha 1: below the fill's threshold (2), the capture's fill would not darken them
        g_vAlpha = 1;
        Volume(-2.0f, 0.5f, -1.6f, 0.2f, -9.0f, -6.0f);
        break;
    case 26: // another reader of the capture (after the fill)
        g_vAlpha = 4;
        Volume(-2.0f, 0.5f, -1.6f, 0.2f, -9.0f, -6.0f);
        break;
    case 27: // a copy inside the window
        g_vAlpha = 4;
        Volume(-2.0f, 0.5f, -1.6f, 0.2f, -9.0f, -6.0f);
        Viewport(0, 0, kEfb, kEfb);
        Scissor(0, 0, kEfb, kEfb);
        Copy(otherDest, 6, EfbRect{0, 0, 16, 16}, false);
        break;
    case 28: { // an alpha-tested alpha 0 write after the volumes
        g_vAlpha = 4;
        Volume(-2.5f, 2.5f, -1.6f, 0.5f, -9.0f, -5.0f);
        Persp();
        Plain();
        BP(0xF3, (4u << 16) | (7u << 19)); // alpha test: A > ref0 (0), ALWAYS
        DstAlpha(true, 0);
        ZMode(true, 3, true);
        BlendReg(kCU | kAU);
        Color(200, 120, 60, 255);
        const float a0[3] = {-0.5f, -1.0f, -4.0f}, a1[3] = {1.5f, -1.0f, -4.0f}, a2[3] = {1.5f, 1.0f, -4.0f}, a3[3] = {-0.5f, 1.0f, -4.0f};
        Quad3(a0, a1, a2, a3);
        Plain();
        break;
    }
    // stencil43 (SF_STENCIL=3): the game's silhouette (a constant destination alpha) and the rules around it
    case 31: // the game's order: volumes (one over the silhouette's pixels), then the silhouette
        g_vAlpha = 4;
        Volume(-2.5f, -1.8f, -1.8f, -0.3f, -8.0f, -7.2f);
        Volume(-0.9f, 0.5f, -1.7f, -0.3f, -11.0f, -8.0f);
        SilhDst(-0.4f, 0.2f, -1.4f, -0.6f, -10.0f, -9.4f, 0x90);
        break;
    case 32: // a window with the silhouette only (no volume)
        SilhDst(-0.4f, 0.2f, -1.4f, -0.6f, -10.0f, -9.4f, 0x90);
        SilhDst(0.5f, 1.0f, -1.5f, -0.7f, -12.0f, -11.0f, 0x90);
        break;
    case 33: // a volume AFTER the mark (it adds and subtracts over the marked pixels)
        g_vAlpha = 4;
        SilhDst(-0.4f, 0.2f, -1.4f, -0.6f, -10.0f, -9.4f, 0x90);
        Volume(-0.8f, 0.6f, -1.7f, -0.3f, -11.0f, -8.5f);
        Volume(-2.5f, -1.8f, -1.8f, -0.3f, -8.0f, -7.2f);
        break;
    case 34: // a mark of 6, then two volumes of which only the SUBTRACT pass reaches the marked pixels (a volume cut by a clip plane or a frustum test loses one pass; the
             // draws are the volume drawer's own state): alpha 6 -> 2 -> 0 (not dark), the stencil would stay at 0x80 - 2: the bound refuses it, the control without it differs
        g_vAlpha = 4;
        SilhDst(-0.4f, 0.2f, -1.4f, -0.6f, -10.0f, -9.4f, 6);
        VolumeSubOnly(-2.0f, 2.0f, -3.0f, 1.0f, -30.0f, -20.0f);
        VolumeSubOnly(-2.5f, 2.5f, -3.0f, 1.0f, -31.0f, -19.0f);
        break;
    case 35: // a mark of exactly the fill's threshold (2): it darkens
        g_vAlpha = 4;
        Volume(-2.5f, -1.8f, -1.8f, -0.3f, -8.0f, -7.2f);
        SilhDst(-0.4f, 0.2f, -1.4f, -0.6f, -10.0f, -9.4f, 2);
        break;
    case 36: // a mark of 1: below the threshold, the fill does not darken it (the control with a wrong threshold marks it)
        g_vAlpha = 4;
        Volume(-2.5f, -1.8f, -1.8f, -0.3f, -8.0f, -7.2f);
        SilhDst(-0.4f, 0.2f, -1.4f, -0.6f, -10.0f, -9.4f, 1);
        break;
    case 37: { // volume, mark, then an alpha 0 write over part of both (order: mark before the reset)
        g_vAlpha = 4;
        Volume(-0.9f, 0.5f, -1.7f, -0.3f, -11.0f, -8.0f);
        SilhDst(-0.4f, 0.2f, -1.4f, -0.6f, -10.0f, -9.4f, 0x90);
        Persp();
        Plain();
        DstAlpha(true, 0);
        ZMode(true, 3, true);
        BlendReg(kCU | kAU);
        Color(40, 200, 90, 255);
        const float r0[3] = {-0.15f, -1.0f, -7.0f}, r1[3] = {0.45f, -1.0f, -7.0f}, r2[3] = {0.45f, 0.4f, -7.0f}, r3[3] = {-0.15f, 0.4f, -7.0f};
        Quad3(r0, r1, r2, r3);
        break;
    }
    case 38: case 39: case 50: // the gate's hand-over: volumes + (38 a silhouette-shaped writer no class places, 39 nothing, 50 the game's silhouette), hint 1 (cannot show)
        g_vAlpha = 4;
        g_hint = n == 39 ? 0 : 1;
        Volume(-2.5f, -1.8f, -1.8f, -0.3f, -8.0f, -7.2f);
        Volume(-0.9f, 0.5f, -1.7f, -0.3f, -11.0f, -8.0f);
        if (n == 38) SilhDst(-0.4f, 0.2f, -1.4f, -0.6f, -10.0f, -9.4f, 0x90, 2);
        if (n == 50) SilhDst(-0.4f, 0.2f, -1.4f, -0.6f, -10.0f, -9.4f, 0x90);
        break;
    case 52: // the silhouette alone, hint 1: the gate leaves the capture and the fill out (the picture is 53's)
        g_hint = 1;
        SilhDst(-0.4f, 0.2f, -1.4f, -0.6f, -10.0f, -9.4f, 0x90);
        break;
    case 53: break; // nothing in the alpha
    // shadow25 (GXV_SILH_GATE, SF_SILH=1): frames 40-45
    case 40: g_hint = 0; break;                                                    // nothing drawn into the alpha: gated exactly
    case 41: case 43: case 45: case 46: // the silhouette behind the floor (alpha written)
        g_hint = n == 41 ? 2 : n == 43 ? 1 : n == 45 ? 0 : 1;
        Persp(); DstAlpha(false, 0); ZMode(true, 4, false); Color(0, 0, 0, 0x60); Plain(); BlendReg(kAU);
        Box(-0.4f, 0.2f, -1.4f, -0.6f, -10.0f, -9.4f);
        if (n == 46) Volume(-2.5f, -1.8f, -1.8f, -0.3f, -8.0f, -7.2f);
        break;
    case 42: case 44: // the silhouette drawn in front of everything (no pixel passes), hint 1 / 2
        g_hint = n == 42 ? 1 : 2;
        Persp(); DstAlpha(false, 0); ZMode(true, 4, false); Color(0, 0, 0, 0x60); Plain(); BlendReg(kAU);
        Box(-0.4f, 0.2f, 0.0f, 0.8f, -10.0f, -9.4f);
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
    if (n == 26) FillState(alphaDest, false, 0x80, 1); // (a second fill: another reader of the capture's texture)
    if (n == 30) {
        // a second window in the same picture: an alpha-only quad over the whole EFB (alpha 0), a volume, its own capture and fill
        Plain();
        Ortho();
        ZMode(false, 7, false);
        BlendReg(kAU);
        DstAlpha(true, 0);
        QuadRect(-1, 1, -1, 1, 0.5f);
        Volume(0.2f, 1.8f, -1.6f, -0.2f, -9.0f, -6.0f);
        CaptureAndFill(alphaDest2);
    }
    g_vAlpha = 0x40;
    g_hint = 0;
    return Finish();
}
}

// gpucut23: SF_HOOK=1 installs a present hook that does what the console's does for the back end (the display drawn: the EFB's next scene is
// certainly a new one), so the depth plan (GXV_DEPTHPLAN, GXV_FIRSTCLEAR) decides the first EFB scene of every frame as on the console.
static bool FakePresent(unsigned, int, int, int, int) { return true; }
int main() {
    const int rw = std::getenv("SF_RW") ? std::atoi(std::getenv("SF_RW")) : kEfb;
    const int rh = std::getenv("SF_RH") ? std::atoi(std::getenv("SF_RH")) : kEfb;
    if (std::getenv("SF_HOOK") && std::atoi(std::getenv("SF_HOOK")) == 1) backend::SetPresentHook(FakePresent);
    Check(host::CreateContext(), "GLES context");
    // SF_RUNGS=1 (pass24): the render size follows the quality ladder (Init(0, 0)) and changes every frame (rungs 0, 2, 1, 2: the EFB's depth
    // surface is made again at each change); the stencil must survive it (the same windows taken, the same pictures).
    const bool rungs = std::getenv("SF_RUNGS") && std::atoi(std::getenv("SF_RUNGS")) == 1;
    Check(rungs ? backend::Init(0, 0) : backend::Init(rw, rh), "production backend init");
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
    if (SilhRun()) {
        for (int n = 40; n <= 46; ++n) {
            const auto p = Frame(n);
            std::printf("frame%d hash=%016llx scenes=%u\n", n, static_cast<unsigned long long>(Hash(p)), backend::DebugLastFrameScenes());
        }
        uint64_t sc[16] = {};
        SilhGateCounters(sc);
        std::printf("silhgate pictures=%llu exact=%llu hint=%llu keephint=%llu volume=%llu unknown=%llu fill=%llu reader=%llu shape=%llu nostart=%llu nowindow=%llu\n", (unsigned long long)sc[0], (unsigned long long)sc[1],
                    (unsigned long long)sc[2], (unsigned long long)sc[3], (unsigned long long)sc[4], (unsigned long long)sc[5], (unsigned long long)sc[6], (unsigned long long)sc[7],
                    (unsigned long long)sc[8], (unsigned long long)sc[9], (unsigned long long)sc[10]);
        Check(glGetError() == GL_NO_ERROR, "no GL error");
        return failures ? 1 : 0;
    }
    if (Stencil43Run()) {
        static const int list[] = {53, 31, 32, 33, 34, 35, 36, 37, 38, 39, 50, 52}; // (53 first: the back end says "stencil usable" from its first picture on)
        for (int n : list) {
            uint64_t s0[24] = {}, s1[24] = {};
            ShadowStencilCounters(s0);
            const auto p = Frame(n);
            ShadowStencilCounters(s1);
            if (const char* dir = std::getenv("SF_RAW")) { // (the pictures as raw RGBA, GL row order)
                char path[512];
                std::snprintf(path, sizeof path, "%s/frame%02d.rgba", dir, n);
                if (FILE* f = std::fopen(path, "wb")) { std::fwrite(p.data(), 1, p.size(), f); std::fclose(f); }
            }
            std::printf("frame%d hash=%016llx scenes=%u taken=%u", n, static_cast<unsigned long long>(Hash(p)), backend::DebugLastFrameScenes(), static_cast<unsigned>(s1[4] - s0[4]));
            for (int r = 2; r < 15; ++r)
                if (s1[4 + r] != s0[4 + r]) std::printf(" [%s]", ShadowStencilReasonName(r));
            std::printf("\n");
        }
        Check(glGetError() == GL_NO_ERROR, "no GL error");
        return failures ? 1 : 0;
    }
    uint64_t nonzero = 0;
    std::vector<unsigned> takenFrames;
    for (int n = 1; n <= (StencilRun() ? 30 : 16); ++n) {
        if (rungs) { static const int seq[4] = {0, 2, 1, 2}; backend::ForceQualityRung(seq[n % 4]); }
        g_staticBoundedFrame = 0;
        uint64_t s0[24] = {}, s1[24] = {};
        ShadowStencilCounters(s0);
        const auto p = Frame(n);
        ShadowStencilCounters(s1);
        if (n >= 13) g_staticBounded += g_staticBoundedFrame;
        if (const char* dir = std::getenv("SF_RAW")) { // (the pictures as raw RGBA, GL row order: tools/ side-by-side PNGs)
            char path[512];
            std::snprintf(path, sizeof path, "%s/frame%02d.rgba", dir, n);
            if (FILE* f = std::fopen(path, "wb")) { std::fwrite(p.data(), 1, p.size(), f); std::fclose(f); }
        }
        uint64_t nz = 0;
        for (size_t i = 3; i < g_alphaPixels.size(); i += 4) nz += g_alphaPixels[i] > 1;
        nonzero += nz;
        if (StencilRun()) {
            const unsigned taken = static_cast<unsigned>(s1[4] - s0[4]);
            if (taken) takenFrames.push_back(static_cast<unsigned>(n));
            std::printf("frame%d hash=%016llx scenes=%u taken=%u", n, static_cast<unsigned long long>(Hash(p)), backend::DebugLastFrameScenes(), taken);
            for (int r = 2; r < 15; ++r)
                if (s1[4 + r] != s0[4 + r]) std::printf(" [%s]", ShadowStencilReasonName(r));
            std::printf("\n");
            if (std::getenv("SF_STENCILHASH")) std::printf("stencil%d hash=%016llx nonzero=%llu\n", n, static_cast<unsigned long long>(g_stencilHash), static_cast<unsigned long long>(g_stencilBytes));
        } else {
            std::printf("frame%d hash=%016llx alpha=%016llx nonzero=%llu\n", n, static_cast<unsigned long long>(Hash(p)),
                        static_cast<unsigned long long>(Hash(g_alphaPixels)), static_cast<unsigned long long>(nz));
        }
    }
    uint64_t c[5];
    ShadowFillCounters(c);
    // GXV_SHADOW_RECT (shadowpx23): captures marked with their rectangle, and copied in part / whole by the back end
    uint64_t sr[6], sb[2];
    ShadowRectCounters(sr);
    backend::DebugShadowRect(sb);
    const int rectMode = ShadowRectMode();
    std::printf("rect mode=%d marked=%llu whole_box=%llu unknown=%llu shape=%llu area_permille=%llu zero=%llu copied_in_part=%llu copied_whole=%llu\n", rectMode,
                static_cast<unsigned long long>(sr[0]), static_cast<unsigned long long>(sr[1]), static_cast<unsigned long long>(sr[2]),
                static_cast<unsigned long long>(sr[3]), static_cast<unsigned long long>(sr[4]), static_cast<unsigned long long>(sr[5]),
                static_cast<unsigned long long>(sb[0]), static_cast<unsigned long long>(sb[1]));
    // (gpuab23, SF_TRIPLE=1: what is known of the EFB's alpha depends on the clear quad being taken: see the triple checks below)
    const char* softEnv = std::getenv("GXV_SOFTXFORM");
    const bool triNoSoft = softEnv && std::strcmp(softEnv, "0") == 0;
    const bool triCulled = std::getenv("SF_CLEARCULL") && std::atoi(std::getenv("SF_CLEARCULL")) == 2;
    const bool triKnown = ShadowCullCover() && !triCulled && !triNoSoft;
    if (StencilRun()) {
        // (the rectangle copies depend on captures this run leaves out, and on the render size staying the same: checked by the plain runs)
    } else if (rectMode == 1 && Triple() && !triKnown)
        Check(triNoSoft ? (sb[0] == 0 && sr[1] == sr[0]) : (sb[0] >= 1 && sr[1] == 11),
              "triple, N not emptied by the clear quad: captures whole until frame 12's covering sky (none at all without the CPU transform)");
    else if (rectMode == 1) Check(sb[0] >= 4 && sr[0] > 0, "GXV_SHADOW_RECT=1: alpha captures copied in part (frames after a capture of the same storage)");
    else Check(sb[0] == 0 && sb[1] == 0, "GXV_SHADOW_RECT off / 2: no capture copied by its rectangle");
    if (rectMode == 2) Check(sr[0] > 0, "GXV_SHADOW_RECT=2: the rectangles worked out (logged only)");
    if (StencilRun()) {
        const char* sw = std::getenv("GXV_SHADOW_STENCIL");
        const int mode = sw ? std::atoi(sw) : 0;
        uint64_t sc[24] = {};
        ShadowStencilCounters(sc);
        std::printf("stencil mode=%d pictures=%llu windows=%llu stale=%llu annotated=%llu taken_frames=", mode, static_cast<unsigned long long>(sc[0]),
                    static_cast<unsigned long long>(sc[1]), static_cast<unsigned long long>(sc[2]), static_cast<unsigned long long>(sc[3]));
        for (unsigned f : takenFrames) std::printf("%u,", f);
        std::printf("\n");
        Check(sc[2] == 0, "no stale read of a capture left out");
        if (mode == 0) Check(sc[0] == 0 && sc[1] == 0, "switch off: the pass does nothing");
        else if (std::getenv("SF_STENCIL_EXPECT") && std::atoi(std::getenv("SF_STENCIL_EXPECT")) == 1) {
            // (the vita profile, SF_TRIPLE=1: the clear quad begins each window) frames with a window that checks out
            static const unsigned want[] = {4, 5, 6, 7, 8, 11, 13, 14, 15, 16, 17, 18, 19, 20, 21, 24, 30}; // (24: stencil43 takes a volume after a mark when it cannot take the pixel under the threshold)
            bool same = takenFrames.size() == sizeof want / sizeof want[0];
            for (size_t i = 0; same && i < takenFrames.size(); ++i) same = takenFrames[i] == want[i];
            Check(mode == 2 ? takenFrames.size() == sizeof want / sizeof want[0] : same, "the windows that check out were taken (4-8, 11, 13-21, 24, 30)");
            // (volume after silhouette, reason 4, is met by tools/test_vgx_stencil43_gl.sh: frame 24 now passes the bound of a volume after a mark)
            Check(sc[4 + 3] >= 1 && sc[4 + 5] >= 1 && sc[4 + 6] >= 1 && sc[4 + 7] >= 1 && sc[4 + 8] >= 1 && sc[4 + 10] >= 1,
                  "each fall-back reason was met: unclassified, alpha value, too many layers, other reader, copy in the window, alpha-tested reset");
        }
        Check(glGetError() == GL_NO_ERROR, "no GL error");
        return failures ? 1 : 0;
    }
    const char* sfb = std::getenv("GXV_SHADOWFILL_BOUNDS");
    const bool cuts = (sfb ? std::atoi(sfb) != 0 : true) || rectMode == 1; // (the fill cuts: either switch)
    std::printf("cuts skipped=%llu scissored=%llu full=%llu fills=%llu fills_not_needed=%llu static_bounded=%u\n", static_cast<unsigned long long>(c[0]),
                static_cast<unsigned long long>(c[1]), static_cast<unsigned long long>(c[2]), static_cast<unsigned long long>(c[3]),
                static_cast<unsigned long long>(c[4]), g_staticBounded);
    Check(nonzero > 0, "the volumes put alpha on the screen (the fill has something to darken)");
    if (cuts && Triple()) {
        // (gpuab23: the game's frame. The clear quad is CPU-transformed (a 4-vertex draw) unless GXV_SOFTXFORM=0, then its pixels are known
        // and, drawn under GX_CULL_BACK and wound as a front face, it takes the screen out of N with GXV_SHADOW_CULLCOVER=1 (default):
        // frame 1 is known as well (left out), the rest as in the double-buffered model. Culled (SF_CLEARCULL=2), switch 0, or not
        // CPU-transformed: N is never emptied and every fill stays whole - which is what the console showed before the fix.)
        std::printf("triple: clear quad %s, GXV_SHADOW_CULLCOVER %d, soft %d: N %s\n", triCulled ? "culled (control)" : "drawn", ShadowCullCover() ? 1 : 0,
                    triNoSoft ? 0 : 1, triKnown ? "emptied by the clear quad" : "not emptied by the clear quad");
        if (triKnown) {
            Check(c[0] == 5, "triple: fill left out in frames 1, 2, 3, 10 (the clear quad's alpha 0) and 12");
            Check(c[1] == 8, "triple: fill scissored in frames 4, 7, 8, 9 and 13-16");
            Check(c[2] == 3, "triple: fill left whole in frames 5, 11 (near plane) and 6 (camera inside)");
        } else if (triNoSoft) {
            Check(c[0] == 0 && c[1] == 0 && c[2] == 16 && c[3] == 0, "triple without the CPU transform: nothing known, every fill whole (the console before the fix)");
        } else {
            // (only frame 12's own sky quad - drawn without culling and without a depth test - empties N; it carries over to 13-16)
            Check(c[0] == 1 && c[1] == 4 && c[2] == 11 && c[3] == 1, "triple, clear quad not taken: fills 1-11 whole (the console before the fix), 12 left out, 13-16 scissored");
        }
    } else if (cuts) {
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
    {
        uint64_t fc[2] = {};
        backend::DebugFirstClear(fc); // (gpucut23, GXV_FIRSTCLEAR)
        std::printf("firstclear: quads taken %llu, depth loads left out %llu\n", static_cast<unsigned long long>(fc[0]), static_cast<unsigned long long>(fc[1]));
    }
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
