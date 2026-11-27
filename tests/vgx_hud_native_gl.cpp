// GXV_HUD_NATIVE (hud24; vgx_backend_gl.cpp "namespace hudn"): the 2D layer drawn at the display's own resolution over the upscaled 3D picture,
// through the production GX front end and the production GLES back end on Mesa (no game data). The present hook is the console's in shape:
// two halves (PresentPhase 1: the upscale into the display, SetHudNativeTarget; 2: the swap), the display a bottom-row-first 960x544 target.
// Each scenario is a game picture at the game's EFB size (640x456): a 3D world (World), then 2D draws between the HUD markers (a panel,
// pixel text, a disc and a star, culled triangles), then the display copy; run twice, the second picture is written to HN_OUT/<name>.rgba
// (960x544 RGBA8, top row first) with its scene count and hook calls in <name>.txt. tools/test_vgx_hud_native_gl.sh runs it with
// GXV_HUD_NATIVE=0 / 1 / 2 at render sizes 720x404 and 960x544 and tests/vgx_hud_native_check.py compares the pictures.
#include "vgx_state.h"
#include <GLES2/gl2.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}
namespace {
using namespace vgx;
constexpr int kEfbW = 640, kEfbH = 456, kScreenW = 960, kScreenH = 544;
alignas(32) unsigned char copyDest[1 << 21];
unsigned failures = 0;
void Check(bool ok, const char* name) {
    if (!ok) std::printf("FAIL: %s\n", name);
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
void Plain() {
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
constexpr unsigned kAlphaBlend = 1u | kCU | (5u << 5) | (4u << 8);          // SRC_ALPHA, INV_SRC_ALPHA
constexpr unsigned kDstAlphaBlend = 1u | kCU | (0u << 5) | (6u << 8);       // DST_ALPHA, ZERO
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
void Quad3(const float a[3], const float b[3], const float c[3], const float d[3]) {
    std::vector<float> v;
    Tri(v, a, b, c);
    Tri(v, a, c, d);
    DrawDecoded(0x90, 0, reinterpret_cast<const uint8_t*>(v.data()), static_cast<uint32_t>(v.size() / 4));
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
// A rectangle in EFB pixels from the picture's top left (full viewport, Ortho).
void Rect(int x0, int y0, int x1, int y1, float depth = 0.5f) {
    QuadRect(x0 / 320.0f - 1.0f, x1 / 320.0f - 1.0f, 1.0f - y1 / 228.0f, 1.0f - y0 / 228.0f, depth);
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
void Full() {
    Viewport(0, 0, kEfbW, kEfbH);
    Scissor(0, 0, kEfbW, kEfbH);
}
void Marker(unsigned kind) { BP(0xFF, kind); }

// ---- the picture
void World() { // the 3D: sky, floor, wall, a box, depth tested and written
    Full();
    g_cull = 0;
    Plain();
    Ortho();
    DstAlpha(false, 0);
    ZMode(true, 3, false);
    BlendReg(kCU | kAU);
    Color(60, 90, 200, 255);
    QuadRect(-1, 1, -1, 1, 1.0f);
    Persp();
    ZMode(true, 3, true);
    Color(120, 160, 90, 255);
    const float f0[3] = {-8, -1, -1}, f1[3] = {8, -1, -1}, f2[3] = {8, -1, -40}, f3[3] = {-8, -1, -40};
    Quad3(f0, f1, f2, f3);
    Color(170, 120, 100, 255);
    const float w0[3] = {1.5f, -1, -9}, w1[3] = {4, -1, -9}, w2[3] = {4, 2.6f, -9}, w3[3] = {1.5f, 2.6f, -9};
    Quad3(w0, w1, w2, w3);
    Color(220, 200, 40, 255);
    Box(-1.2f, -0.4f, -1.0f, 0.2f, -8.0f, -7.2f);
}
// 5x7 glyphs of "STAR 27" (rows, bit 4 = the left column)
const char* const kGlyph[][7] = {
    {"01111", "10000", "10000", "01110", "00001", "00001", "11110"},  // S
    {"11111", "00100", "00100", "00100", "00100", "00100", "00100"},  // T
    {"01110", "10001", "10001", "11111", "10001", "10001", "10001"},  // A
    {"11110", "10001", "10001", "11110", "10100", "10010", "10001"},  // R
    {"01110", "10001", "00001", "00110", "01000", "10000", "11111"},  // 2
    {"11111", "00001", "00010", "00100", "01000", "01000", "01000"},  // 7
};
void Text(int x, int y, int px) { // "STAR 27", one rectangle per lit glyph pixel, px EFB pixels each
    static const int order[] = {0, 1, 2, 3, -1, 4, 5};
    for (int ch = 0; ch < 7; ++ch) {
        if (order[ch] < 0) continue;
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (kGlyph[order[ch]][row][col] == '1') Rect(x + (ch * 6 + col) * px, y + row * px, x + (ch * 6 + col + 1) * px, y + (row + 1) * px);
    }
}
void Disc(float r) {
    std::vector<float> v;
    const unsigned seg = 24;
    for (unsigned k = 0; k < seg; ++k) {
        const float t0 = 6.2831853f * k / seg, t1 = 6.2831853f * (k + 1) / seg;
        const float c[3] = {0, 0, -0.5f}, p0[3] = {r * std::cos(t0), r * std::sin(t0), -0.5f}, p1[3] = {r * std::cos(t1), r * std::sin(t1), -0.5f};
        Tri(v, c, p0, p1);
    }
    DrawDecoded(0x90, 0, reinterpret_cast<const uint8_t*>(v.data()), static_cast<uint32_t>(v.size() / 4));
}
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
// The state of the game's layouts (setupDrawForNW4RLayout): 2D, no depth, alpha blending.
void LayoutState() {
    Full();
    Ortho();
    g_cull = 0;
    Plain();
    ZMode(false, 7, false);
    BlendReg(kAlphaBlend | kAU);
}
void HudA() { // the panel, the text, the meter
    LayoutState();
    Color(10, 10, 40, 160);
    Rect(8, 10, 176, 76);
    Color(250, 250, 250, 255);
    Text(18, 16, 2);
    Color(240, 60, 40, 255);
    Rect(18, 48, 18 + 100, 52);
    Color(40, 220, 60, 255);
    Rect(18, 56, 18 + 61, 60);
    Viewport(560, 16, 64, 64);
    Scissor(560, 16, 64, 64);
    Color(250, 200, 20, 255);
    Disc(0.95f);
    Color(30, 30, 200, 255);
    Disc(0.45f);
    Full();
}
void HudB() { // a star, culled triangles (one wound each way, back faces culled), a one-pixel frame
    LayoutState();
    Viewport(500, 16, 48, 48);
    Scissor(500, 16, 48, 48);
    Color(250, 120, 220, 255);
    Star(0.3f, 0.9f, 5);
    Full();
    g_cull = 2; // back faces culled
    Plain();
    Color(255, 140, 0, 255);
    const float a[3] = {-0.6f, -0.6f, -0.5f}, b[3] = {-0.3f, -0.6f, -0.5f}, c[3] = {-0.45f, -0.2f, -0.5f};
    Viewport(24, 392, 120, 48);
    Scissor(24, 392, 120, 48);
    std::vector<float> v;
    Tri(v, a, b, c); // wound one way
    DrawDecoded(0x90, 0, reinterpret_cast<const uint8_t*>(v.data()), 3);
    const float a2[3] = {0.3f, -0.6f, -0.5f}, b2[3] = {0.6f, -0.6f, -0.5f}, c2[3] = {0.45f, -0.2f, -0.5f};
    v.clear();
    Tri(v, a2, c2, b2); // wound the other way
    DrawDecoded(0x90, 0, reinterpret_cast<const uint8_t*>(v.data()), 3);
    Full();
    g_cull = 0;
    Plain();
    Color(255, 255, 255, 255);
    Rect(200, 380, 204, 440);   // a frame: 4-pixel bars
    Rect(300, 380, 304, 440);
    Rect(200, 380, 304, 384);
    Rect(200, 436, 304, 440);
}
// MR::clearZBuffer: a full-picture quad that writes depth only (ALWAYS), at the far plane
void DepthClear() {
    Full();
    Ortho();
    g_cull = 0;
    Plain();
    ZMode(true, 7, true);
    BlendReg(0);
    BP(0x41, 0);
    { // (GX_ZT_REPLACE with a one-valued texture: the draw's depth range is one value, here the far plane)
        const float w = kEfbW, h = kEfbH;
        const uint32_t v[] = {f2u(w / 2.0f), f2u(-h / 2.0f), f2u(0.0f), f2u(342.0f + w / 2.0f), f2u(342.0f + h / 2.0f), f2u(16777215.0f)};
        WriteXF(0x101A, 6, v);
    }
    QuadRect(-1, 1, -1, 1, std::getenv("HN_CLEARZ") ? static_cast<float>(std::atof(std::getenv("HN_CLEARZ"))) : 1.0f);
}
// A J3D-like model inside the layouts: two boxes, the nearer drawn first, perspective, depth test LEQUAL with write, its own viewport
void Model(float shift = 0.0f) {
    Full();
    Persp();
    g_cull = 0;
    Plain();
    ZMode(true, 3, true);
    BlendReg(kCU | kAU);
    Viewport(420, 90, 120, 120);
    Scissor(420, 90, 120, 120);
    Color(230, 60, 60, 255);
    Box(-0.2f + shift, 0.3f + shift, -0.25f, 0.25f, -3.0f, -2.6f);
    Color(60, 230, 90, 255);
    Box(-0.6f + shift, 0.1f + shift, -0.45f, 0.35f, -4.5f, -4.1f); // behind it: the depth test must hide part of it
    Full();
}
void Finish(bool clearGrey = true, bool second = false) {
    Full();
    ZMode(true, 3, true);
    BlendReg(kCU | kAU);
    if (clearGrey) ClearColour(90, 90, 90, 255, 0xFFFFFF);
    g.dispCopySrc = EfbRect{0, 0, kEfbW, kEfbH};
    QueueCopyDisp(true);
    (void)second;
}
void GreyBackdrop() { // a flat grey full-picture quad (not 2D: outside the markers), as the first draw of a HUD-only picture
    Full();
    Ortho();
    Plain();
    ZMode(false, 7, false);
    BlendReg(kCU | kAU);
    g_cull = 0;
    Color(90, 90, 90, 255);
    QuadRect(-1, 1, -1, 1, 0.5f);
}

// ---- the hook: the console's two halves, the display a texture-backed framebuffer object, bottom row first
GLuint g_dispFbo = 0, g_dispTex = 0;
unsigned g_calls[3] = {};
bool g_refuse1 = false;
std::vector<uint8_t> ReadDisplay();
std::vector<uint8_t> g_hookPx; // hud56: HN_TRANS reads the finished display inside the hook (a read between pictures upsets the next picture's EFB in this harness)
bool g_hookRead = false;
bool Hook(unsigned efb, int x, int y, int w, int h) {
    (void)efb;
    const int ph = backend::PresentPhase();
    ++g_calls[ph];
    if (ph == 2) { // the swap only
        if (g_hookRead) g_hookPx = ReadDisplay();
        return true;
    }
    if (ph == 1 && g_refuse1) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, g_dispFbo);
    if (std::getenv("HN_DEBUG")) { GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp); std::printf("hook ph%d vp %d %d %d %d rect %d,%d %dx%d\n", ph, vp[0], vp[1], vp[2], vp[3], x, y, w, h); }
    const float amount = backend::SharpenAmount(w, h, kScreenW, kScreenH);
    if (!backend::PresentSharpened(g_dispFbo, x, y, w, h, 0, 0, kScreenW, kScreenH, amount < 0.05f ? 0.0f : amount, true)) {
        std::printf("FAIL: PresentSharpened refused\n");
        ++failures;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_dispFbo);
    if (ph == 1) backend::SetHudNativeTarget(g_dispFbo, 0, 0, kScreenW, kScreenH, true);
    if (ph == 0 && g_hookRead) g_hookPx = ReadDisplay();
    return true;
}
std::vector<uint8_t> ReadDisplay() { // top row first
    std::vector<uint8_t> bottomFirst(static_cast<size_t>(kScreenW) * kScreenH * 4), out(bottomFirst.size());
    glBindFramebuffer(GL_FRAMEBUFFER, g_dispFbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, kScreenW, kScreenH, GL_RGBA, GL_UNSIGNED_BYTE, bottomFirst.data());
    for (int row = 0; row < kScreenH; ++row)
        std::memcpy(&out[static_cast<size_t>(row) * kScreenW * 4], &bottomFirst[static_cast<size_t>(kScreenH - 1 - row) * kScreenW * 4], static_cast<size_t>(kScreenW) * 4);
    return out;
}
const char* g_out = nullptr;

struct Scenario {
    const char* name;
    void (*draw)();
};
// markers: 1/2 = the game's HUD markers, 12/13 = the new ones for the scenes that draw layouts outside them
void ScHud3d() { World(); Marker(1); HudA(); HudB(); Marker(2); Finish(); }
void ScHudOnly() { GreyBackdrop(); Marker(1); HudA(); HudB(); Marker(2); Finish(); }
void ScTail3d() { // a 3D draw (depth tested) in the middle of the 2D: the tail is what follows it
    World();
    Marker(1);
    HudA();
    Marker(2);
    Persp();
    Plain();
    ZMode(true, 3, true);
    BlendReg(kCU | kAU);
    g_cull = 0;
    Color(40, 200, 220, 255);
    Box(-0.4f, 0.4f, -0.6f, 0.4f, -5.0f, -4.2f);
    Marker(1);
    HudB();
    Marker(2);
    Finish();
}
void ScCopyMid() { // an EFB capture between two 2D parts, the second reading it
    World();
    Marker(1);
    HudA();
    Marker(2);
    Copy(copyDest, 4, EfbRect{0, 0, kEfbW, kEfbH}, false);
    Marker(1);
    HudB();
    LayoutState();
    BindCopy(copyDest, kEfbW, kEfbH, 4);
    BP(0xC0, (8u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19)); // colour = TEXC
    BP(0xF3, (7u << 16) | (7u << 19));
    BlendReg(kCU | kAU);
    Color(0, 0, 0, 255);
    Viewport(360, 300, 120, 90);
    Scissor(360, 300, 120, 90);
    QuadRect(-1, 1, -1, 1, 0.5f, true);
    Full();
    Marker(2);
    Finish();
}
void ScNoHud() { World(); Finish(); }
void ScDepthLast() { // the last 2D draw tests depth: the tail is empty
    World();
    Marker(1);
    HudA();
    LayoutState();
    ZMode(true, 1, false);
    Color(255, 0, 255, 255);
    Rect(300, 20, 340, 60);
    Marker(2);
    Finish();
}
void ScDstAlpha() { // the last 2D draw blends with the destination's alpha
    World();
    Marker(1);
    HudA();
    LayoutState();
    BlendReg(kDstAlphaBlend);
    Color(255, 0, 255, 255);
    Rect(300, 20, 340, 60);
    Marker(2);
    Finish();
}
void ScTwoDisp() { // two display copies in one buffer
    World();
    Marker(1);
    HudA();
    Marker(2);
    Finish();
    GreyBackdrop();
    Marker(1);
    HudB();
    Marker(2);
    Finish();
}
void ScHudn() { World(); Marker(12); HudA(); HudB(); Marker(13); Finish(); }
void ScModel() { World(); DepthClear(); Marker(1); HudA(); Model(); HudB(); Marker(2); Finish(); }
void ScModelOnly() { GreyBackdrop(); DepthClear(); Marker(1); Model(); Marker(2); Finish(); }
void ScModelNoClear() { World(); Marker(1); HudA(); Model(); HudB(); Marker(2); Finish(); }
void ScModelLast() { World(); DepthClear(); Marker(1); HudA(); HudB(); Model(); Marker(2); Finish(); } // a depth-tested draw as the picture's last
void ScModelNonHud() { // a depth-tested model between two brackets (not flagged 2D)
    World();
    DepthClear();
    Marker(1);
    HudA();
    Marker(2);
    Model(0.1f);
    Marker(1);
    HudB();
    Marker(2);
    Finish();
}
void ScSkipDW() { // a depth-writing 3D draw between the clear and the first 2D draw stays in the EFB: the display's depth would differ
    World();
    DepthClear();
    Persp();
    Plain();
    ZMode(true, 3, true);
    BlendReg(kCU | kAU);
    Color(40, 40, 220, 255);
    Box(-0.3f, 0.3f, -0.3f, 0.3f, -3.0f, -2.5f);
    Marker(1);
    HudA();
    Model();
    HudB();
    Marker(2);
    Finish();
}
void ScLines() { // 2D only, 1-pixel lines and 1-pixel quads: coverage at the display's pixels
    GreyBackdrop();
    Marker(1);
    LayoutState();
    Color(255, 255, 255, 255);
    for (int i = 0; i < 20; ++i) Rect(20 + i * 12, 100, 21 + i * 12, 300);
    for (int i = 0; i < 12; ++i) Rect(20, 100 + i * 17, 260, 101 + i * 17);
    Marker(2);
    Finish();
}
const Scenario kScenarios[] = {
    {"hud3d", ScHud3d}, {"hudonly", ScHudOnly}, {"tail3d", ScTail3d}, {"copymid", ScCopyMid}, {"nohud", ScNoHud}, {"depthlast", ScDepthLast},
    {"dstalpha", ScDstAlpha}, {"twodisp", ScTwoDisp}, {"hudn", ScHudn}, {"lines", ScLines}, {"refuse", ScHud3d}, {"capture", ScHud3d},
    {"model", ScModel}, {"modelonly", ScModelOnly}, {"modelnoclear", ScModelNoClear}, {"modellast", ScModelLast}, {"modelnonhud", ScModelNonHud}, {"skipdw", ScSkipDW},
};
}

// hud56: a run of pictures through the hold's start (a picture with no 2D tail), the held fallbacks, the end of the hold (native again), and a 60 Assist pair's first
// picture (armed: never native). Each picture that has a HUD must show it (differs from the HUD-less picture's pixels in the HUD's corner), and the census must count
// 0 pictures whose 2D draws were drawn by neither path.
static int Transitions() {
    const char* const envSeq = std::getenv("HN_SEQ"); // (another run of the same letters: HN_SEQ="HHHHPPPHHHHHHHHH", the hold is GXV_HUD_NATIVE_HOLD)
    // 'H' hud picture, 'X' a picture with no HUD (starts a hold), 'P' a hud picture armed as a 60 Assist pair's first picture
    static const char seqDefault[] = "HXHHHHPHHXHHHH";
    const char* const seq = envSeq && *envSeq ? envSeq : seqDefault;
    const int kN = static_cast<int>(std::strlen(seq));
    const int holdN = std::getenv("GXV_HUD_NATIVE_HOLD") ? std::atoi(std::getenv("GXV_HUD_NATIVE_HOLD")) : 0;
    int sincePair = 1 << 20; // pictures since the last pair picture (fx59: a pair starts the hold, so the singles after it stay on the fallback path)
    uint64_t c0[backend::kHudNativeReasons + 4], c1[backend::kHudNativeReasons + 4];
    for (int pass = 0; pass < 2; ++pass) { ScNoHud(); FrontPresent(false, nullptr); } // (the first makes the shaders)
    for (int i = 0; i < (std::getenv("HN_WARM") ? std::atoi(std::getenv("HN_WARM")) : 3); ++i) { ScHud3d(); FrontPresent(false, nullptr); }                                  // (shader work done for the HUD's programs too)
    std::string order;
    int hudPictures = 0, nativePictures = 0, fallbackHudPictures = 0;
    for (int i = 0; i < kN; ++i) {
        const char k = seq[i];
        if (k == 'P') backend::WarpArm(true, 1000u + static_cast<unsigned>(i), true);
        g_calls[0] = g_calls[1] = g_calls[2] = 0;
        backend::HudNativeCounters(c0);
        if (k == 'X') ScNoHud(); else ScHud3d();
        g_hookRead = true;
        FrontPresent(false, nullptr);
        g_hookRead = false;
        backend::HudNativeCounters(c1);
        if (k == 'P') backend::WarpArm(false, 0, true);
        const bool native = c1[1] != c0[1];
        const std::vector<uint8_t>& px = g_hookPx;
        size_t diff = 0; // pixels of the HUD's panel corner that are not the clear grey
        for (int yy = 0; yy < 100; ++yy)
            for (int xx = 0; xx < 300; ++xx) {
                const uint8_t* q = &px[(static_cast<size_t>(yy) * kScreenW + xx) * 4];
                diff += (q[0] != 90 || q[1] != 90 || q[2] != 90) ? 1u : 0u;
            }
        if (std::getenv("HN_DEBUG")) std::printf("pic %d %c calls %u/%u/%u native %d diff %zu\n", i, k, g_calls[0], g_calls[1], g_calls[2], native ? 1 : 0, diff);
        if (g_out && std::getenv("HN_DUMP")) { char path[512]; std::snprintf(path, sizeof path, "%s/trans%02d.rgba", g_out, i); if (FILE* f = std::fopen(path, "wb")) { std::fwrite(px.data(), 1, px.size(), f); std::fclose(f); } }
        order += k == 'X' ? 'x' : (native ? 'N' : 'F');
        if (k != 'X') {
            ++hudPictures;
            native ? ++nativePictures : ++fallbackHudPictures;
            if (diff < 500) { std::printf("FAIL: transition picture %d (%c) shows no HUD (%zu pixels are not the clear colour in the HUD corner)\n", i, k, diff); ++failures; }
        }
        if (k == 'P') sincePair = 0; else ++sincePair;
        if (k != 'P' && holdN > 0 && sincePair <= holdN && native) { std::printf("FAIL: picture %d (%c) went native %d pictures after a pair picture (the pair starts the hold of %d)\n", i, k, sincePair, holdN); ++failures; }
        if (k == 'P' && native) { std::printf("FAIL: pair picture %d was taken natively\n", i); ++failures; }
    }
    std::printf("trans: %s (N native, F fallback with HUD, x picture without HUD)\n", order.c_str());
    backend::HudNativeCounters(c1);
    std::printf("trans: pictures %d, native %d, fallback %d, lost %llu, pair/swap %llu, pair reason %llu\n", hudPictures, nativePictures, fallbackHudPictures,
                static_cast<unsigned long long>(c1[backend::kHudNativeReasons + 2]), static_cast<unsigned long long>(c1[backend::kHudNativeReasons + 3]), static_cast<unsigned long long>(c1[4 + 13]));
    Check(nativePictures >= 3 && fallbackHudPictures >= 4, "the run has both native and fallback pictures with a HUD");
    Check(c1[backend::kHudNativeReasons + 2] == 0, "no picture had its 2D tail drawn by neither path");
    Check(c1[backend::kHudNativeReasons + 3] == 0, "no pair first picture native, no missed swap");
    Check(c1[4 + 13] >= 1, "the pair picture was counted");
    return failures ? 1 : 0;
}

int main() {
    const int rw = std::getenv("HN_RW") ? std::atoi(std::getenv("HN_RW")) : 720;
    const int rh = std::getenv("HN_RH") ? std::atoi(std::getenv("HN_RH")) : 404;
    g_out = std::getenv("HN_OUT");
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(rw, rh), "production backend init");
    if (failures) return 1;
    glGenTextures(1, &g_dispTex);
    glBindTexture(GL_TEXTURE_2D, g_dispTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kScreenW, kScreenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &g_dispFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_dispFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_dispTex, 0);
    GLuint depthRb = 0;
    glGenRenderbuffers(1, &depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, kScreenW, kScreenH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRb);
    Check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "display framebuffer complete");
    backend::SetPresentHook(Hook);
    backend::SetHudNativeHost(kScreenW, kScreenH);
    if (!std::getenv("HN_NODEPTH")) backend::SetHudNativeDepth(true); // (a host whose display has a depth buffer; the test attached one)
    ResetState();
    g.efbW = kEfbW;
    g.efbH = kEfbH;
    QueueSetTarget();
    BP(0x43, 1);
    Scissor(0, 0, kEfbW, kEfbH);
    BP(0x59, 171u | (171u << 10));
    const uint32_t m[] = {f2u(1), 0, 0, 0, 0, f2u(1), 0, 0, 0, 0, f2u(1), 0};
    WriteXF(0, 12, m);
    Ortho();
    if (std::getenv("HN_TRANS")) return Transitions(); // (hud56: the hold's start and end, a 60 Assist pair's pictures: every picture with a HUD shows it, the census counts 0 lost)
    for (const Scenario& sc : kScenarios) {
        const bool refuse = std::strcmp(sc.name, "refuse") == 0, capture = std::strcmp(sc.name, "capture") == 0;
        uint64_t before[backend::kHudNativeReasons + 4], after[backend::kHudNativeReasons + 4];
        for (int pass = 0; pass < 2; ++pass) { // (the first picture makes the shaders: a picture with shader work is never taken natively)
            g_refuse1 = refuse && pass == 1;
            if (capture && pass == 1) backend::RequestCapture(true);
            g_calls[0] = g_calls[1] = g_calls[2] = 0;
            backend::HudNativeCounters(before);
            sc.draw();
            FrontPresent(false, nullptr);
            backend::HudNativeCounters(after);
            if (capture && pass == 1) backend::RequestCapture(false);
            if (pass == 0) continue;
            const unsigned scenes = backend::DebugLastFrameScenes();
            const std::vector<uint8_t> px = ReadDisplay();
            std::string line = "scenes=" + std::to_string(scenes) + " calls=" + std::to_string(g_calls[0]) + "/" + std::to_string(g_calls[1]) + "/" + std::to_string(g_calls[2]) +
                               " pictures=" + std::to_string(after[0] - before[0]) + " taken=" + std::to_string(after[1] - before[1]) + " partial=" + std::to_string(after[2] - before[2]) +
                               " tail=" + std::to_string(after[3] - before[3]);
            for (int r = 0; r < backend::kHudNativeReasons; ++r)
                if (after[4 + r] != before[4 + r]) line += std::string(" [") + backend::HudNativeReasonName(r) + "]";
            std::printf("%-10s %s\n", sc.name, line.c_str());
            if (g_out) {
                char path[512];
                std::snprintf(path, sizeof path, "%s/%s.rgba", g_out, sc.name);
                if (FILE* f = std::fopen(path, "wb")) { std::fwrite(px.data(), 1, px.size(), f); std::fclose(f); }
                std::snprintf(path, sizeof path, "%s/%s.txt", g_out, sc.name);
                if (FILE* f = std::fopen(path, "w")) { std::fprintf(f, "%s\n", line.c_str()); std::fclose(f); }
            }
        }
    }
    if (std::getenv("HN_CENSUS")) { // 700 more pictures: the census line ("hud-native:", every 600)
        for (int i = 0; i < 700; ++i) {
            if (i % 5 == 4) ScNoHud(); else ScHud3d();
            FrontPresent(false, nullptr);
        }
    }
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
