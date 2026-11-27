// (copied from vgx_hud_native_gl.cpp for GXV_DIRECT_DISPLAY, direct24: see tools/test_vgx_direct_display_gl.sh) GXV_HUD_NATIVE (hud24; vgx_backend_gl.cpp "namespace hudn"): the 2D layer drawn at the display's own resolution over the upscaled 3D picture,
// through the production GX front end and the production GLES back end on Mesa (no game data). The present hook is the console's in shape:
// two halves (PresentPhase 1: the upscale into the display, SetHudNativeTarget; 2: the swap), the display a bottom-row-first 960x544 target.
// Each scenario is a game picture at the game's EFB size (640x456): a 3D world (World), then 2D draws between the HUD markers (a panel,
// pixel text, a disc and a star, culled triangles), then the display copy; run twice, the second picture is written to HN_OUT/<name>.rgba
// (960x544 RGBA8, top row first) with its scene count and hook calls in <name>.txt. tools/test_vgx_hud_native_gl.sh runs it with
// GXV_HUD_NATIVE=0 / 1 / 2 at render sizes 720x404 and 960x544 and tests/vgx_hud_native_check.py compares the pictures.
#include "vgx_state.h"
#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
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

// ---- the hook: the console's whole present (PresentPhase 0), with GXV_DIRECT_DISPLAY's part. The display is a texture-backed framebuffer object, bottom row
// first. A picture the back end offers (DirectDisplayOffer) is "queued to the display" as the console's host does: vglSetNextFlip(id), no present drawn, and
// the display shows that texture's memory - read when the picture is looked at, AFTER the rotation and the next clear, so a buffer clobbered by the EFB shows.
// The desktop has no display queue: the "swap" is a vertical-flip blit of the queued texture (the memory order the display wants is top row first, as the EFB's).
GLuint g_dispFbo = 0, g_dispTex = 0, g_srcFbo = 0, g_efbFbo = 0; // (g_efbFbo: the back end's EFB object, seen by the hook; the reads leave it bound as the back end expects)
std::set<GLuint> g_displayTex;     // vglMakeDisplayTexture'd
GLuint g_nextFlip = 0, g_shown = 0;// armed / the texture the display shows now (0: g_dispFbo's own picture)
unsigned g_flips = 0, g_offers = 0, g_hookCalls = 0;
bool g_refuseFlip = false;         // the fake vitaGL refuses vglSetNextFlip (the "refused" fallback)
uint32_t g_beginFailures = 0;
extern "C" {
bool g_makeFail = false;           // DD_MAKEFAIL: the fake vitaGL refuses every display texture (the machine must go off and cost nothing)
unsigned g_capRefused = 0;         // (pace25) vitaGL's VGL_DD_MAX: the sixteenth display texture is the last
int vglMakeDisplayTexture(GLuint name) { if (g_makeFail) return 0; if (!g_displayTex.count(name) && g_displayTex.size() >= 16) { ++g_capRefused; return 0; } g_displayTex.insert(name); return 1; }
uint32_t vglGetDisplayTextures(void) { return static_cast<uint32_t>(g_displayTex.size()); }
// (pace25) vitaGL forgets a display texture when its storage is freed (gpu_free_texture_data -> vgl_dd_forget): a deleted name is no longer one
void glDeleteTextures(GLsizei n, const GLuint* names) {
    static void (*real)(GLsizei, const GLuint*) = reinterpret_cast<void (*)(GLsizei, const GLuint*)>(dlsym(RTLD_NEXT, "glDeleteTextures"));
    for (GLsizei i = 0; i < n; ++i) g_displayTex.erase(names[i]);
    real(n, names);
}
int vglIsDisplayTexture(GLuint name) { return g_displayTex.count(name) ? 1 : 0; }
int vglSetNextFlip(GLuint name) { if (g_refuseFlip || !g_displayTex.count(name)) return 0; g_nextFlip = name; return 1; }
uint32_t vglGetDirectRefusals(void) { return 0; }
uint32_t vglGetSceneBeginFailures(void) { return g_beginFailures; }
}
bool Hook(unsigned efb, int x, int y, int w, int h) {
    ++g_hookCalls;
    g_efbFbo = efb;
    const int ph = backend::PresentPhase();
    if (ph == 2) return true;
    bool flip = false;
    if (ph == 0 && backend::DirectDisplayMode() == 1) {
        ++g_offers;
        const unsigned id = backend::DirectDisplayOffer(x, y, w, h, 0, kScreenW, kScreenW, kScreenH, true);
        flip = id != 0 && vglSetNextFlip(id) != 0;
        if (std::getenv("DD_DEBUG")) std::printf("hook: rect %d,%d %dx%d offer %u flip %d\n", x, y, w, h, id, flip ? 1 : 0);
        if (id != 0) {
            // (the swap) the queued texture becomes the display's picture
            if (flip) { g_shown = g_nextFlip; g_nextFlip = 0; ++g_flips; }
            backend::DirectDisplayResult(flip, flip ? 0 : 12 /* kRefused */);
        }
    }
    if (flip) return true;
    g_shown = 0;
    glDisable(GL_SCISSOR_TEST); // (PresentPicture does: the EFB's scissor is not the display's)
    glBindFramebuffer(GL_FRAMEBUFFER, g_dispFbo);
    const float amount = backend::SharpenAmount(w, h, kScreenW, kScreenH);
    if (amount < 0.05f && x == 0 && y == 0 && w == kScreenW && h == kScreenH) {
        // The console's plain 1:1 present is glBlitFramebuffer(x, y + h, x + w, y -> 0, 0, 960, 544, GL_LINEAR): all four channels, rows bottom-up. GLES2 has no blit;
        // the same copy on the CPU (read the EFB, flip the rows, put them into the display's texture).
        std::vector<uint8_t> src(static_cast<size_t>(w) * h * 4), dst(src.size());
        glBindFramebuffer(GL_FRAMEBUFFER, efb);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, src.data());
        for (int r = 0; r < h; ++r) std::memcpy(&dst[static_cast<size_t>(r) * w * 4], &src[static_cast<size_t>(h - 1 - r) * w * 4], static_cast<size_t>(w) * 4);
        glBindTexture(GL_TEXTURE_2D, g_dispTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dst.data());
        glBindFramebuffer(GL_FRAMEBUFFER, g_dispFbo);
        return true;
    }
    if (!backend::PresentSharpened(g_dispFbo, x, y, w, h, 0, 0, kScreenW, kScreenH, amount < 0.05f ? 0.0f : amount, true)) { // (the present's draw: a plain copy at 1:1)
        std::printf("FAIL: PresentSharpened refused\n");
        ++failures;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_dispFbo);
    return true;
}
// The display's picture, top row first: g_dispFbo's own (bottom row first, as today's present draws it), or the queued texture's memory (top row first).
std::vector<uint8_t> ReadTex(GLuint tex) {
    std::vector<uint8_t> out(static_cast<size_t>(kScreenW) * kScreenH * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, g_srcFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, kScreenW, kScreenH, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efbFbo); // (the back end believes its EFB object is bound)
    return out; // (GL row 0 = memory row 0 = the picture's top row)
}
std::vector<uint8_t> ReadDisplay() { // top row first
    if (g_shown) return ReadTex(g_shown);
    std::vector<uint8_t> bottomFirst(static_cast<size_t>(kScreenW) * kScreenH * 4), out(bottomFirst.size());
    glBindFramebuffer(GL_FRAMEBUFFER, g_dispFbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, kScreenW, kScreenH, GL_RGBA, GL_UNSIGNED_BYTE, bottomFirst.data());
    for (int row = 0; row < kScreenH; ++row)
        std::memcpy(&out[static_cast<size_t>(row) * kScreenW * 4], &bottomFirst[static_cast<size_t>(kScreenH - 1 - row) * kScreenW * 4], static_cast<size_t>(kScreenW) * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efbFbo);
    return out;
}
const char* g_out = nullptr;
std::vector<uint8_t> StripAlpha(const std::vector<uint8_t>& rgba) { // (the display ignores the alpha byte; today's present writes 255, a flipped EFB buffer holds the EFB's own)
    std::vector<uint8_t> rgb;
    rgb.reserve(rgba.size() / 4 * 3);
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) { rgb.push_back(rgba[i]); rgb.push_back(rgba[i + 1]); rgb.push_back(rgba[i + 2]); }
    return rgb;
}
void WriteFile(const char* name, const char* ext, const std::vector<uint8_t>& data) {
    char path[512];
    std::snprintf(path, sizeof path, "%s/%s.%s", g_out, name, ext);
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(data.data(), 1, data.size(), f); std::fclose(f); }
}

struct Scenario {
    const char* name;
    void (*draw)();
};
void FinishNoClear() { // the copy does not clear: the next picture goes on from this one's colour (never shown directly)
    Full();
    ZMode(true, 3, true);
    BlendReg(kCU | kAU);
    g.dispCopySrc = EfbRect{0, 0, kEfbW, kEfbH};
    QueueCopyDisp(false);
}
void ScHud3d() { World(); Marker(1); HudA(); HudB(); Marker(2); Finish(); }
void ScNoHud() { World(); Finish(); }
void ScHudOnly() { GreyBackdrop(); Marker(1); HudA(); HudB(); Marker(2); Finish(); }
void ScCopyMid() { // an EFB capture between two 2D parts, the second reading it (GXV_EFBSWAP hands the colour buffer to the texture)
    World();
    Marker(1);
    HudA();
    Marker(2);
    Copy(copyDest, 4, EfbRect{0, 0, kEfbW, kEfbH}, false);
    Marker(1);
    HudB();
    LayoutState();
    BindCopy(copyDest, kEfbW, kEfbH, 4);
    BP(0xC0, (8u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
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
void ScLines() { GreyBackdrop(); Marker(1); LayoutState(); Color(255, 255, 255, 255); for (int i = 0; i < 20; ++i) Rect(20 + i * 12, 100, 21 + i * 12, 300);
                 for (int i = 0; i < 12; ++i) Rect(20, 100 + i * 17, 260, 101 + i * 17); Marker(2); Finish(); }
void ScNoClear() { World(); Marker(1); HudA(); Marker(2); FinishNoClear(); }
void ScPlain() { // no clear colour set: the copy clears with whatever the registers hold (a picture that starts on the grey of the last clear)
    World(); Finish(false);
}
// The game's own picture pattern (MainLoopFramework): the copy does not clear; the picture starts with clearEfb's quad - a full-screen rectangle, colour and alpha written
// (dst alpha 0), no blending, depth ALWAYS with update, GX_CULL_BACK. `facing`: wound so that it faces the camera (drawn); else it is culled whole (draws nothing:
// the picture then goes on from the old colour, and the old colour must therefore be there).
void GameClear(bool facing) {
    Full();
    Ortho();
    g_cull = 2;
    Plain();
    DstAlpha(true, 0);
    ZMode(true, 7, true);
    BlendReg(kCU | kAU);
    Color(60, 90, 140, 255);
    if (facing) QuadRect(-1, 1, -1, 1, 0.5f); // (the winding the front end takes as facing under GX_CULL_BACK, the game's clearEfb)
    else QuadRect(1, -1, -1, 1, 0.5f);
    g_cull = 0;
    Plain();
    DstAlpha(false, 0);
}
void ScGame() { GameClear(true); World(); Marker(1); HudA(); HudB(); Marker(2); FinishNoClear(); }
void ScGameCulled() { GameClear(false); World(); Marker(1); HudA(); Marker(2); FinishNoClear(); }
void ScLive() { Marker(1); HudA(); HudB(); Marker(2); FinishNoClear(); } // the picture goes on from the old colour (blended HUD only)
const Scenario kScenarios[] = {{"hud3d", ScHud3d}, {"nohud", ScNoHud}, {"hudonly", ScHudOnly}, {"copymid", ScCopyMid}, {"lines", ScLines}, {"noclear", ScNoClear}, {"plain", ScPlain}, {"game", ScGame}, {"gameculled", ScGameCulled}, {"live", ScLive}};
}

// (pace25, DD_LONG=N) a long run under GXV_DD_DYN: every picture hands the EFB's colour buffer to one of six capture textures (the swap: the texture holds a display texture) and writes
// another of the six the ordinary way, whole size (a conversion the swap does not take: AdoptEfbBuffer turns a held buffer into that texture's own storage, or, with GXV_DD_DYN,
// gives it back), with a render-scale change (the machine's way switches) every 150 pictures. Reports and checks how many display textures plain textures hold and the largest live count.
void PicLong(int a, int b, unsigned fmtB) {
    World();
    Marker(1);
    HudA();
    Marker(2);
    Copy(copyDest + static_cast<size_t>(a) * 4096, 4, EfbRect{0, 0, kEfbW, kEfbH}, false);
    Marker(1);
    HudB();
    Marker(2);
    Copy(copyDest + static_cast<size_t>(b) * 4096, fmtB, EfbRect{0, 0, kEfbW, kEfbH}, false);
    Finish();
}
int LongRun(int pictures, bool rungs) {
    unsigned maxLive = 0, maxHeld = 0, flipsAtStart = g_flips, offersAtStart = g_offers;
    const unsigned adopts0 = backend::DebugEfbAdopts();
    uint64_t liveSum = 0;
    static const unsigned fmts[] = {0, 1, 3, 5};   // (formats whose conversion the swap does not take; the harness finds out which one adopts)
    for (int i = 0; i < pictures; ++i) {
        if (rungs) {
            const int phase = i % 150;
            if (phase == 0 && i) backend::ForceQualityRung(4);
            if (phase == 6) backend::ForceQualityRung(6);
        }
        const int a = i % 6, b = (i + 3) % 6;
        PicLong(a, b, fmts[(i / 6) % 4]);
        FrontPresent(false, nullptr);
        const unsigned live = static_cast<unsigned>(g_displayTex.size());
        maxLive = std::max(maxLive, live);
        liveSum += live;
        maxHeld = std::max(maxHeld, backend::DebugDisplayHeldByPlainTextures());
    }
    const unsigned offers = g_offers - offersAtStart, flips = g_flips - flipsAtStart;
    std::printf("long: %d pictures, %u offered, %u flipped (%.1f %%), display textures live: max %u, mean %.1f; held by plain textures: max %u, at the end %u; adopted as storage %u, returned to the spares %u, vitaGL cap refusals %u\n",
                pictures, offers, flips, offers ? 100.0 * flips / offers : 0.0, maxLive, static_cast<double>(liveSum) / std::max(1, pictures), maxHeld, backend::DebugDisplayHeldByPlainTextures(),
                backend::DebugEfbAdopts() - adopts0, backend::DebugDynReturned(), g_capRefused);
    if (const char* e = std::getenv("DD_LONG_EXPECT")) {
        if (!std::strcmp(e, "bounded")) {   // the fix: no display texture ever becomes a plain texture's storage; the count stays under the cap; direct display keeps running
            Check(maxHeld == 0, "long run: no display texture held by a plain texture");
            Check(maxLive <= 12 + 2, "long run: live display textures bounded (kDynMaxTextures 12 + the ones a refused make-up leaves)");
            Check(g_capRefused == 0, "long run: vitaGL's cap never reached");
            Check(offers && flips * 10 >= offers * 6, "long run: at least 60 % of the offered pictures flip");
        } else if (!std::strcmp(e, "leak")) {   // the negative control (GXV_DD_DYN_RETURN=0): the harness does exercise the leak
            Check(maxHeld >= 1, "negative control: without the return a display texture does end up held by a plain texture");
        }
    }
    return failures ? 1 : 0;
}
int main() {
    const int rw = std::getenv("HN_RW") ? std::atoi(std::getenv("HN_RW")) : 960;
    const int rh = std::getenv("HN_RH") ? std::atoi(std::getenv("HN_RH")) : 544;
    g_out = std::getenv("HN_OUT");
    g_makeFail = std::getenv("DD_MAKEFAIL") != nullptr;
    Check(host::CreateContext(), "GLES context");
    const bool rungs = std::getenv("DD_RUNGS") != nullptr; // (the render scale changes between pictures: ForceQualityRung, the ladder's own sizes)
    Check(rungs ? backend::Init(0, 0) : backend::Init(rw, rh), "production backend init");
    if (failures) return 1;
    glGenTextures(1, &g_dispTex);
    glBindTexture(GL_TEXTURE_2D, g_dispTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kScreenW, kScreenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &g_dispFbo);
    glGenFramebuffers(1, &g_srcFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_dispFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_dispTex, 0);
    Check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "display framebuffer complete");
    backend::SetPresentHook(Hook);
    backend::SetHudNativeHost(kScreenW, kScreenH);
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
    const bool ring = std::getenv("DD_RING") != nullptr;
    std::vector<uint8_t> prevPx;
    GLuint prevShown = 0;
    for (const Scenario& sc : kScenarios) {
        g_flips = g_offers = 0;
        uint64_t dead0 = 0, rest0 = 0;
        backend::DebugDirectStarts(dead0, rest0);
        std::vector<uint8_t> px;
        for (int pass = 0; pass < 7; ++pass) { // (the first pictures make the shaders and the buffers; then the ring goes round)
            g_refuseFlip = std::getenv("DD_REFUSE") != nullptr && pass % 2 == 1;
            sc.draw();
            FrontPresent(false, nullptr);
            px = ReadDisplay();
            if (g_out && std::getenv("DD_EACH")) {
                char path[512];
                std::snprintf(path, sizeof path, "%s/%s.pass%d.rgba", g_out, sc.name, pass);
                if (FILE* f = std::fopen(path, "wb")) { std::fwrite(px.data(), 1, px.size(), f); std::fclose(f); }
            }
            if (ring && prevShown && prevShown != g_shown) {
                // the previous picture's buffer must still hold that picture one picture later (it is in flight: no use of it yet)
                if (ReadTex(prevShown) != prevPx) { std::printf("FAIL: %s pass %d: the buffer queued one picture ago changed\n", sc.name, pass); ++failures; }
            }
            prevShown = g_shown;
            prevPx = px;
        }
        { uint64_t dead1 = 0, rest1 = 0; backend::DebugDirectStarts(dead1, rest1); std::printf("%-10s starts after a flip: colour dead %llu, drawn back %llu\n", sc.name, static_cast<unsigned long long>(dead1 - dead0), static_cast<unsigned long long>(rest1 - rest0)); }
        unsigned kinds[5] = {};
        backend::DebugLastFrameSceneKinds(kinds);
        std::printf("%-10s flips=%u offers=%u; the last picture: %u scenes, display scenes %u (%s)\n", sc.name, g_flips, g_offers, backend::DebugLastFrameScenes(), kinds[4],
                    g_shown ? "shown by a flip" : "shown by the present");
        if (g_out) { WriteFile(sc.name, "rgba", px); WriteFile(sc.name, "rgb", StripAlpha(px)); }
    }
    if (rungs) { // rung 6 (960x544: flips) <-> smaller rungs (the present as before, reason "size") and back, 4 pictures at each step
        static const int steps[] = {6, 4, 6, 3, 2, 6, 6, 5, 6, 0, 6};
        std::vector<uint8_t> all;
        for (int rung : steps) {
            backend::ForceQualityRung(rung);
            for (int k = 0; k < 4; ++k) {
                g_flips = g_offers = 0;
                ScHud3d();
                FrontPresent(false, nullptr);
                const std::vector<uint8_t> px = ReadDisplay();
                all.insert(all.end(), px.begin(), px.end());
            }
            int w = 0, h = 0, smp = 0;
            backend::QualityRungSize(w, h, smp);
            std::printf("rung %d: render %dx%d, %s\n", rung, w, h, g_shown ? "shown by a flip" : "shown by the present");
        }
        if (g_out) { WriteFile("rungs", "bin", all); WriteFile("rungs", "rgb", StripAlpha(all)); }
    }
    if (const char* e = std::getenv("DD_LONG")) { if (LongRun(std::atoi(e), rungs)) return 1; }
    if (std::getenv("DD_CENSUS")) { // 700 more pictures: the census line ("direct-display:", every 600)
        for (int i = 0; i < 700; ++i) {
            if (i % 5 == 4) ScNoClear(); else ScHud3d();
            FrontPresent(false, nullptr);
        }
    }
    { uint64_t dead = 0, restored = 0; backend::DebugDirectStarts(dead, restored); std::printf("starts: colour dead %llu, drawn back %llu\n", static_cast<unsigned long long>(dead), static_cast<unsigned long long>(restored)); }
    std::printf("spares-extra %u\n", backend::DebugDirectSparesExtra());
    std::printf("vramfree-calls %llu\n", static_cast<unsigned long long>(backend::DebugVramFreeCalls()));
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
