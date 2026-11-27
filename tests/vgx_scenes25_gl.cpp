// GXV_SCENES25 (the census) and GXV_SCENES25_DRY (captures no draw reads, left out by the Indirect capture's rules) through the production
// GX front end and GLES back end on Mesa (no game data). An ordinary game picture in small: a floor; the whole-EFB alpha capture (A8) and its
// reader; the player's corner capture (A8 tile at the top, x > 0) and its reader; the marked "Indirect" capture (RGB565) and its reader. The
// script tools/test_vgx_scenes25_gl.sh runs it with the switches off / on and compares pictures and scene counts, and against the base tree
// (compiled with -DS25_BASE: no counter accessors).
//   frames 1, 3: all three captures read.   frames 2, 4: the corner capture has no reader.
//   frame 5: the corner texture is read BEFORE its capture only (the previous picture's), none after.
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
// (the copies' destinations are guest buffers; a copy texture is found by the address range its texels would cover, so they are far apart)
alignas(65536) unsigned char destinations[4][65536];
unsigned char* const indirectDestination = destinations[0];
unsigned char* const readDestination = destinations[1];
unsigned char* const alphaDestination = destinations[2];
unsigned char* const cornerDestination = destinations[3];
unsigned failures = 0;
void Check(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
void XF(unsigned reg, unsigned value) { WriteXF(reg, 1, &value); }
void Viewport() {
    const uint32_t w[] = {f2u(32), f2u(-32), f2u(16777215.0f + 1), f2u(374), f2u(374), f2u(16777215.0f + 1)};
    WriteXF(0x101A, 6, w);
}
void Color(unsigned r, unsigned green, unsigned b, unsigned alpha) {
    BP(0xE2, r | alpha << 12);
    BP(0xE3, b | green << 12);
}
void Pass() { // no texture: colour and alpha = C0
    BP(0x00, 0);
    XF(0x103F, 0);
    BP(0x28, 7u << 7);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, 1 | (7u << 1) | (1u << 4));
    BP(0x41, (1u << 3) | (1u << 4));
    BP(0x42, 1u << 8);
}
void Quad(float left, float right, float depth, bool tex = false) {
    const float z = depth - 1;
    const float v[] = {left,-1,z,0, 0,1, right,-1,z,0, 1,1, right,1,z,0, 1,0, left,1,z,0, 0,0};
    if (tex) DrawDecoded(0x80, VL_TEX0, reinterpret_cast<const uint8_t*>(v), 4);
    else {
        float plain[16];
        for (int i = 0; i < 4; ++i) std::memcpy(plain + i * 4, v + i * 6, 16);
        DrawDecoded(0x80, 0, reinterpret_cast<const uint8_t*>(plain), 4);
    }
}
void Copy(void* dest, unsigned format) {
    g.texCopySrc = EfbRect{0, 0, 64, 64};
    g.texCopyDstW = g.texCopyDstH = 64;
    g.texCopyFmt = format;
    QueueCopyTex(dest, false);
}
void MarkedCapture(unsigned stage = 0) {
    BP(0xFF, 10 | (stage << 8)); // PORT_GX_MARK_TRANSIENT_COPY, with the game's stage tag
    Copy(indirectDestination, 4);
}
void Bind(void* data, unsigned format) {
    auto& t = g.tex[0];
    t = TexSlot{};
    t.valid = true;
    t.obj.data = data;
    t.obj.width = t.obj.height = 64;
    t.obj.format = format;
    g.bpTexStale = 0;
    g.texDirty = true;
    g.texSlotDirty = 1;
    XF(0x103F, 1);
    XF(0x1040, 5u << 7);
    BP(0x00, 1);
    BP(0x28, (1u << 6) | (7u << 7));
}
void Reader(float left, float right) { // the capture read as TEXC (an indirect model)
    Bind(indirectDestination, 4);
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19));
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (1u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, 0);
    BP(0x41, (1u << 3) | (1u << 4));
    BP(0x42, 0);
    Quad(left, right, 0.5f, true);
}
void Floor(unsigned r, unsigned gr, unsigned b) {
    Pass();
    Color(r, gr, b, 255);
    Quad(-1, 0, 0.5f);
    Color(b, r, gr, 255);
    Quad(0, 1, 0.5f);
}
std::vector<uint8_t> ReadAndFlush(unsigned& scenes) {
    Copy(readDestination, 6);
    const auto id = Pending().copyTex.back().texId;
    FrontPresent(false, nullptr);
    scenes = backend::DebugLastFrameScenes();
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    Check(backend::DebugReadTexture(id, pixels, w, h) && w == 64 && h == 64, "picture read back");
    return pixels;
}
uint64_t Hash(const std::vector<uint8_t>& p) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < p.size(); i += 4)
        for (int k = 0; k < 3; ++k) h = (h ^ p[i + k]) * 1099511628211ull;
    return h;
}
}


static uint32_t g_testCuts = 0;
extern "C" uint32_t port_camera_cut_count(void) { return g_testCuts; }
namespace {
using namespace vgx;
void CopyRect(void* dest, EfbRect src, int dw, int dh, unsigned format) {
    g.texCopySrc = src;
    g.texCopyDstW = static_cast<uint16_t>(dw);
    g.texCopyDstH = static_cast<uint16_t>(dh);
    g.texCopyFmt = format;
    QueueCopyTex(dest, false);
}
void BindTex(void* data, int w, int h, unsigned format) {
    auto& t = g.tex[0];
    t = TexSlot{};
    t.valid = true;
    t.obj.data = data;
    t.obj.width = static_cast<uint16_t>(w);
    t.obj.height = static_cast<uint16_t>(h);
    t.obj.format = format;
    g.bpTexStale = 0;
    g.texDirty = true;
    g.texSlotDirty = 0xFF;
    XF(0x103F, 1);
    XF(0x1040, 5u << 7);
    BP(0x00, 1);
    BP(0x28, (1u << 6) | (7u << 7));
}
void ReadOf(void* dest, int w, int h, unsigned texFormat, float left, float right) { // the copy's texels drawn as colour
    BindTex(dest, w, h, texFormat);
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19));
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (1u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, 0);
    BP(0x41, (1u << 3) | (1u << 4));
    BP(0x42, 0);
    Quad(left, right, 0.5f, true);
}
// One picture: floor; (corner read BEFORE) ; whole alpha capture + reader; corner capture + (reader); some draws; Indirect + reader.
std::vector<uint8_t> Picture(unsigned seed, bool cornerReadAfter, bool cornerReadBefore, unsigned& scenes) {
    Floor(30 + seed * 40, 200 - seed * 30, 60 + seed * 20);
    Pass();
    Color(250, 10 + seed * 30, 10, 255);
    Quad(-0.5f, -0.2f, 0.4f);
    if (cornerReadBefore) ReadOf(cornerDestination, 16, 16, 1, -0.9f, -0.7f);
    CopyRect(alphaDestination, EfbRect{0, 0, 64, 64}, 64, 64, 0x27); // the shadow / silhouette alpha capture
    ReadOf(alphaDestination, 64, 64, 1, 0.5f, 0.6f);
    Pass();
    Color(10 + seed * 20, 250, 10, 255);
    Quad(0.1f, 0.3f, 0.4f);
    CopyRect(cornerDestination, EfbRect{40, 0, 16, 16}, 16, 16, 0x27); // the player's drop-shadow corner
    if (cornerReadAfter) ReadOf(cornerDestination, 16, 16, 1, -0.3f, -0.1f);
    Pass();
    Color(10, 10 + seed * 40, 250, 255);
    Quad(0.3f, 0.45f, 0.4f);
    MarkedCapture();
    Reader(0.6f, 1);
    return ReadAndFlush(scenes);
}
}
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
    const uint32_t m[] = {f2u(1),0,0,0, 0,f2u(1),0,0, 0,0,f2u(1),0};
    WriteXF(0, 12, m);
    const uint32_t proj[] = {f2u(1),0,f2u(1),0,f2u(1),0,1};
    WriteXF(0x1020, 7, proj);
    Viewport();
    unsigned s[8] = {};
    std::vector<uint8_t> p[8];
    p[1] = Picture(0, true, false, s[1]);
    p[2] = Picture(1, false, false, s[2]);
    p[3] = Picture(2, true, false, s[3]);
    p[4] = Picture(3, false, false, s[4]);
    p[5] = Picture(4, false, true, s[5]);
    for (int f = 1; f <= 5; ++f) {
        if (p[f].empty()) return 1;
        std::printf("frame%d scenes=%u hash=%016llx\n", f, s[f], static_cast<unsigned long long>(Hash(p[f])));
    }
#ifndef S25_BASE
    uint64_t c[8];
    IndCapCounters(c);
    std::printf("info indcap pictures=%llu marked=%llu left=%llu kept=%llu half=%llu stale=%llu held=%llu pre=%llu\n", (unsigned long long)c[0], (unsigned long long)c[1],
                (unsigned long long)c[2], (unsigned long long)c[3], (unsigned long long)c[4], (unsigned long long)c[5], (unsigned long long)c[6], (unsigned long long)c[7]);
#endif
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
