// GXV_TAILFOLD=3 (tailfold39; vgx_backend_gl.cpp "namespace tailfoldbe"): a bloom picture's tail (put-back, glow add, fade quad, 2D layer) folded into the present, through the production GX front end and
// GLES back end on Mesa (no game data). The hook is the console's in shape (hud24's: two halves, a bottom-row-first 960x544 display). Each scenario is a picture as BloomEffect writes it: the 3D world, the
// bracket's capture (RGBA8, marked), the black fill, glow draws, the glow chain's capture (a leave), the put-back (marked, a texel-for-pixel copy of the capture), the glow add (blend ONE, ONE), a fade
// quad, the 2D layer, the display copy. Run twice (the first makes the shaders), the second picture is written to TF_OUT/<name>.rgba (960x544 RGBA8, top row first) with its scene count and the
// fold counters in <name>.txt. tools/test_vgx_tailfold_gl.sh runs it with GXV_TAILFOLD=0 and 3 (and =1) at the 960x544 render size and tests/vgx_tailfold_check.py compares the pictures.
#include "vgx_state.h"
#include <GLES2/gl2.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// the basic log: the back end calls it without a test (the console has it; here it prints what it is told)
extern "C" void port_log_summary(const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    std::printf("LOG ");
    std::vprintf(fmt, ap);
    va_end(ap);
}
namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}
namespace {
using namespace vgx;
int kEfbW = 640, kEfbH = 456;
constexpr int kScreenW = 960, kScreenH = 544;
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
    QuadRect(x0 * 2.0f / kEfbW - 1.0f, x1 * 2.0f / kEfbW - 1.0f, 1.0f - y1 * 2.0f / kEfbH, 1.0f - y0 * 2.0f / kEfbH, depth);
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
    g.dispCopySrc = EfbRect{0, 0, static_cast<int16_t>(kEfbW), static_cast<int16_t>(kEfbH)};
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

// ---- the bloom picture
alignas(32) unsigned char capDest[1 << 21];
alignas(32) unsigned char glowDest[1 << 21];
void Mark(unsigned sub) { Marker(40u | (sub << 8)); }
EfbRect Whole() { return EfbRect{0, 0, static_cast<int16_t>(kEfbW), static_cast<int16_t>(kEfbH)}; }
void Fill(unsigned r, unsigned gr, unsigned b, unsigned a, float depth = 0.5f) {
    Full(); Ortho(); g_cull = 0; Plain(); ZMode(false, 7, false); BlendReg(kCU | kAU); Color(r, gr, b, a); QuadRect(-1, 1, -1, 1, depth);
}
struct Opts {
    bool markers = true, glow = true, putBack = true, glowAdd = true, fade = false, hud = true, hudB = true, ties = false;
    bool fill = true, depthDraw = false, dstAlphaDraw = false, copyAfter = false, colourLive = false, extraReader = false, partialPut = false;
};
void PutBackState() { // BloomEffect::bloomPutBack: MODULATE by white, colour only, no blend, no depth
    Full(); Ortho(); g_cull = 0;
    BindCopy(capDest, kEfbW, kEfbH, 6);
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19));
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (4u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    ZMode(false, 7, false);
    BlendReg(kCU);
    DstAlpha(false, 0);
    Color(255, 255, 255, 255);
}
void GlowAddState() { // the glow add: texture * constant, added to what is there
    Full(); Ortho(); g_cull = 0;
    BindCopy(glowDest, kEfbW, kEfbH, 4);
    BP(0xC0, (15u << 12) | (8u << 8) | (2u << 4) | 15u | (1u << 19));
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    ZMode(false, 7, false);
    BlendReg(1u | kCU | (1u << 5) | (1u << 8));
    Color(150, 150, 150, 255);
}
void TiesLayer() { // rectangles whose edges lie on pixel centres (EFB = render size): the fill rule decides a row
    LayoutState();
    Color(240, 240, 240, 255);
    for (int i = 0; i < 12; ++i) {
        const float x0 = 20.0f + i * 31.0f, y0 = 40.5f + i * 13.0f;
        QuadRect(x0 * 2.0f / kEfbW - 1.0f, (x0 + 17.0f) * 2.0f / kEfbW - 1.0f, 1.0f - (y0 + 9.0f) * 2.0f / kEfbH, 1.0f - y0 * 2.0f / kEfbH, 0.5f);
    }
    Color(200, 40, 40, 255);
    for (int i = 0; i < 12; ++i) {
        const float x0 = 30.5f + i * 31.0f, y0 = 300.5f - i * 11.0f;
        QuadRect(x0 * 2.0f / kEfbW - 1.0f, (x0 + 15.0f) * 2.0f / kEfbW - 1.0f, 1.0f - (y0 + 6.0f) * 2.0f / kEfbH, 1.0f - y0 * 2.0f / kEfbH, 0.5f);
    }
}
void Scene2D() { // the picture's 3D world, without depth (the swap harness on Mesa does not keep a depth buffer through the hand-over): bands, a floor, boxes, a gradient of 17 strips
    Fill(60, 90, 200, 255);
    LayoutState();
    BlendReg(kCU | kAU);
    for (int i = 0; i < 17; ++i) { Color(40 + i * 11, 160 - i * 6, 90 + i * 5, 255); Rect(0, 280 + i * 11, kEfbW, 280 + (i + 1) * 11); }
    Color(170, 120, 100, 255); Rect(kEfbW / 2 + 40, 100, kEfbW / 2 + 170, 300);
    Color(220, 200, 40, 255); Rect(kEfbW / 5, 170, kEfbW / 5 + 90, 290);
    Color(250, 250, 250, 255); Rect(kEfbW / 3, 30, kEfbW / 3 + 7, 31); // one-pixel marks that a 1:1 copy keeps and a resample would not
    Rect(kEfbW / 3 + 20, 33, kEfbW / 3 + 21, 80);
    if (!std::getenv("TF_NODEPTHSCENE")) { // a depth-tested model: the picture's first run reads the depth the fill does not write, so the capture is not an isolated pass (as the game's)
        Persp(); Plain(); ZMode(true, 3, true); BlendReg(kCU | kAU); g_cull = 0; Full();
        Color(220, 200, 40, 255);
        Box(-1.2f, -0.4f, -1.0f, 0.2f, -8.0f, -7.2f);
    }
}
void Bloom(const Opts& o) {
    Scene2D();
    if (o.markers) Mark(1);
    Copy(capDest, 6, Whole(), false);               // the scene capture (RGBA8, the whole EFB)
    if (o.markers) Mark(2);
    if (o.fill) Fill(0, 0, 0, 255);                 // the black fill
    if (o.markers) Mark(3);
    if (o.glow) { // glow sources on the black
        Fill(0, 0, 0, 255);
        Color(250, 240, 20, 255); Rect(kEfbW / 4, kEfbH / 3, kEfbW / 4 + 90, kEfbH / 3 + 40);
        Color(40, 200, 250, 255); Rect(kEfbW / 2, kEfbH / 2, kEfbW / 2 + 60, kEfbH / 2 + 120);
    }
    if (o.glow || o.glowAdd) Copy(glowDest, 4, Whole(), false); // the glow chain's capture: the last command that leaves the EFB
    if (o.markers) Mark(4);
    if (o.putBack) { PutBackState(); QuadRect(-1, 1, -1, 1, 0.5f, true); }
    if (o.markers) Mark(5);
    if (o.extraReader) { PutBackState(); Viewport(300, 300, 100, 80); Scissor(300, 300, 100, 80); QuadRect(-1, 1, -1, 1, 0.45f, true); Full(); }
    if (o.glowAdd) { GlowAddState(); QuadRect(-1, 1, -1, 1, 0.5f, true); }
    if (o.fade) { LayoutState(); Color(10, 10, 40, 70); Rect(0, 0, kEfbW, kEfbH); }
    if (o.depthDraw) { Persp(); Plain(); ZMode(true, 3, true); BlendReg(kCU | kAU); g_cull = 0; Color(40, 200, 220, 255); Box(-0.4f, 0.4f, -0.6f, 0.4f, -5.0f, -4.2f); }
    if (o.dstAlphaDraw) { LayoutState(); BlendReg(kDstAlphaBlend); Color(255, 0, 255, 255); Rect(300, 20, 340, 60); }
    if (o.copyAfter) { Copy(copyDest, 4, EfbRect{0, 0, 64, 64}, false); LayoutState(); Color(255, 255, 0, 255); Rect(100, 100, 140, 140); }
    if (o.hud) { HudA(); if (o.hudB) HudB(); }
    if (o.ties) TiesLayer();
    Finish();
    if (o.colourLive) { /* (Finish clears: the colour-live case is made by the display copy of ScLive below) */ }
}
void BloomLive() { // the display copy does not clear: the colour is live at the end
    Opts o; o.hud = false;
    Scene2D();
    Mark(1); Copy(capDest, 6, Whole(), false); Mark(2); Fill(0, 0, 0, 255); Mark(3);
    Copy(glowDest, 4, Whole(), false);
    Mark(4); PutBackState(); QuadRect(-1, 1, -1, 1, 0.5f, true); Mark(5);
    GlowAddState(); QuadRect(-1, 1, -1, 1, 0.5f, true);
    Full(); ZMode(true, 3, true); BlendReg(kCU | kAU);
    g.dispCopySrc = EfbRect{0, 0, static_cast<int16_t>(kEfbW), static_cast<int16_t>(kEfbH)};
    QueueCopyDisp(false);
}

// ---- the hook: the console's two halves, the display a texture-backed framebuffer object, bottom row first
GLuint g_dispFbo = 0, g_dispTex = 0;
unsigned g_calls[3] = {};
bool Hook(unsigned efb, int x, int y, int w, int h) {
    (void)efb;
    const int ph = backend::PresentPhase();
    ++g_calls[ph];
    if (ph == 2) return true; // the swap only
    glBindFramebuffer(GL_FRAMEBUFFER, g_dispFbo);
    const float amount = backend::SharpenAmount(w, h, kScreenW, kScreenH);
    if (!backend::PresentSharpened(g_dispFbo, x, y, w, h, 0, 0, kScreenW, kScreenH, amount < 0.05f ? 0.0f : amount, true)) {
        std::printf("FAIL: PresentSharpened refused\n");
        ++failures;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_dispFbo);
    if (ph == 1) backend::SetHudNativeTarget(g_dispFbo, 0, 0, kScreenW, kScreenH, true);
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
struct Scenario { const char* name; void (*draw)(); };

#define SC(NAME, BODY) void Sc_##NAME() { Opts o; BODY; Bloom(o); }
SC(bloom, (void)o)
SC(nohud, o.hud = false)
SC(fade, o.fade = true)
SC(hudonly, o.glow = false; o.glowAdd = false)
SC(ties, o.hud = false; o.ties = true; o.fade = true)
SC(nomarker, o.markers = false)
SC(noputback, o.putBack = false)
SC(depth, o.depthDraw = true)
SC(dstalpha, o.dstAlphaDraw = true)
SC(copyafter, o.copyAfter = true)
SC(reader, o.extraReader = true)
void Sc_live() { BloomLive(); }
void Sc_dbgworld() { Scene2D(); Finish(); }
void Sc_dbgnofill() { Opts o; o.glow = false; o.glowAdd = false; o.hud = false; o.fill = false; Bloom(o); }
void Sc_dbgnomark() { Opts o; o.glow = false; o.glowAdd = false; o.hud = false; o.markers = false; Bloom(o); }
void Sc_dbgput() { Opts o; o.glow = false; o.glowAdd = false; o.hud = false; Bloom(o); }
const Scenario kScenarios[] = {
    {"bloom", Sc_bloom}, {"nohud", Sc_nohud}, {"fade", Sc_fade}, {"hudonly", Sc_hudonly}, {"ties", Sc_ties}, {"nomarker", Sc_nomarker}, {"noputback", Sc_noputback},
    {"depth", Sc_depth}, {"dstalpha", Sc_dstalpha}, {"copyafter", Sc_copyafter}, {"reader", Sc_reader}, {"live", Sc_live}, {"capture", Sc_bloom}, {"dbgworld", Sc_dbgworld}, {"dbgput", Sc_dbgput}, {"dbgnofill", Sc_dbgnofill}, {"dbgnomark", Sc_dbgnomark},
};
}

// (debug) the EFB's content right after the put-back, through a read-back copy
void DebugRead() {
    Scene2D(); Mark(1); Copy(capDest, 6, Whole(), false); Mark(2); Fill(0, 0, 0, 255); Mark(3); Mark(4); PutBackState(); QuadRect(-1, 1, -1, 1, 0.5f, true); Mark(5);
    Copy(copyDest, 6, Whole(), false);
    const auto id = Pending().copyTex.back().texId;
    const auto capId = Pending().copyTex.front().texId;
    Finish();
    FrontPresent(false, nullptr);
    std::vector<uint8_t> px; int w = 0, h = 0;
    const bool ok = backend::DebugReadTexture(id, px, w, h);
    auto at = [&](int x, int y) { const size_t i = (static_cast<size_t>(y) * w + x) * 4; return std::to_string(px[i]) + "," + std::to_string(px[i + 1]) + "," + std::to_string(px[i + 2]); };
    if (ok) std::printf("dbgread %dx%d: %s %s %s\n", w, h, at(100, 100).c_str(), at(100, 400).c_str(), at(w / 2, h / 2).c_str());
    else std::printf("dbgread: no texture\n");
    std::vector<uint8_t> cp; int cw = 0, ch = 0;
    if (backend::DebugReadTexture(capId, cp, cw, ch)) { const size_t i = (static_cast<size_t>(100) * cw + 100) * 4; std::printf("dbgread capture %dx%d: %d,%d,%d\n", cw, ch, cp[i], cp[i + 1], cp[i + 2]); }
    else std::printf("dbgread capture: no texture\n");
}
int main() {
    const int rw = std::getenv("TF_RW") ? std::atoi(std::getenv("TF_RW")) : 960;
    const int rh = std::getenv("TF_RH") ? std::atoi(std::getenv("TF_RH")) : 544;
    if (std::getenv("TF_EFBW")) kEfbW = std::atoi(std::getenv("TF_EFBW"));
    if (std::getenv("TF_EFBH")) kEfbH = std::atoi(std::getenv("TF_EFBH"));
    const char* out = std::getenv("TF_OUT");
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
    Check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "display framebuffer complete");
    backend::SetPresentHook(Hook);
    backend::SetHudNativeHost(kScreenW, kScreenH);
    ResetState();
    g.efbW = static_cast<uint16_t>(kEfbW);
    g.efbH = static_cast<uint16_t>(kEfbH);
    QueueSetTarget();
    BP(0x43, 1);
    Scissor(0, 0, kEfbW, kEfbH);
    BP(0x59, 171u | (171u << 10));
    const uint32_t m[] = {f2u(1), 0, 0, 0, 0, f2u(1), 0, 0, 0, 0, f2u(1), 0};
    WriteXF(0, 12, m);
    for (unsigned t = 0; t < 4; ++t) { BP(0xF6 + 2 * t, 0u | (1u << 2)); BP(0xF7 + 2 * t, 2u | (3u << 2)); } // (the game's swap tables: identity)
    Ortho();
    if (std::getenv("TF_DEBUGREAD")) { for (int i = 0; i < 6; ++i) DebugRead(); return 0; }
    for (const Scenario& sc : kScenarios) {
        if (std::getenv("TF_ONLY") && std::strcmp(std::getenv("TF_ONLY"), sc.name) != 0) continue;
        const bool capture = std::strcmp(sc.name, "capture") == 0;
        uint64_t before[4], after[4];
        constexpr int kPasses = 7;
        for (int pass = 0; pass < kPasses; ++pass) { // (the first picture makes the shaders: a picture with shader work is never folded)
            if (capture && pass == kPasses - 1) backend::RequestCapture(true);
            g_calls[0] = g_calls[1] = g_calls[2] = 0;
            backend::TailFoldCounters(before);
            sc.draw();
            FrontPresent(false, nullptr);
            backend::TailFoldCounters(after);
            if (capture && pass == kPasses - 1) backend::RequestCapture(false);
            if (pass != kPasses - 1) continue;
            const unsigned scenes = backend::DebugLastFrameScenes();
            const std::vector<uint8_t> px = ReadDisplay();
            const std::string line = "scenes=" + std::to_string(scenes) + " calls=" + std::to_string(g_calls[0]) + "/" + std::to_string(g_calls[1]) + "/" + std::to_string(g_calls[2]) +
                                     " pictures=" + std::to_string(after[0] - before[0]) + " armed=" + std::to_string(after[1] - before[1]) + " folded=" + std::to_string(after[2] - before[2]) +
                                     " late=" + std::to_string(after[3] - before[3]);
            std::printf("%-10s %s\n", sc.name, line.c_str());
            if (out) {
                char path[512];
                std::snprintf(path, sizeof path, "%s/%s.rgba", out, sc.name);
                if (FILE* f = std::fopen(path, "wb")) { std::fwrite(px.data(), 1, px.size(), f); std::fclose(f); }
                std::snprintf(path, sizeof path, "%s/%s.txt", out, sc.name);
                if (FILE* f = std::fopen(path, "w")) { std::fprintf(f, "%s\n", line.c_str()); std::fclose(f); }
            }
        }
    }
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
