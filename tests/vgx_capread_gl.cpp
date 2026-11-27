// GXV_CAPREAD (water23; vgx_draw.cpp CapReadLog): the diagnostic log of the whole-EFB screen captures' readers, through the production
// GX front end and GLES back end on Mesa (no game data), at a render size that is not the EFB's (96x72 for a 64x64 EFB, as the
// console's rungs scale 640x456). Each picture: a floor that changes per picture, a reader of the PREVIOUS picture's capture drawn
// before the capture through a projective texgen from the position (GX_TG_MTX3x4, TEXMTX0: a screen projection, as the game's
// IndDummy particles and models sample it), the RGB565 capture (as CaptureScreenDirector's "Indirect" one), a reader after it (TEXC
// through the vertex texture coordinates), the picture read back.
// tools/test_vgx_capread_gl.sh runs it with GXV_CAPREAD=0 and =1, each with GXV_EFBSWAP2=0 and =1:
//   - the pictures must be identical with the log on and off (it only reads the finished frame);
//   - with it on, the counters must show 1 reader before and 1 after each capture, and the log lines must name the projective
//     texgen ("0:t0 s0 proj") of the reader before the capture;
//   - the pictures with GXV_EFBSWAP2=0 and =1 are compared too (the captures' storage differs; the pictures must not).
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
alignas(32) unsigned char captureDestination[32];
alignas(32) unsigned char readDestination[32];
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
void Quad(float left, float right, float top, float bottom, float depth, bool tex) {
    const float z = depth - 1;
    const float v[] = {left,bottom,z,0, 0,1, right,bottom,z,0, 1,1, right,top,z,0, 1,0, left,top,z,0, 0,0};
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
void Bind(void* data, unsigned format, bool projective) {
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
    if (projective) {
        // texgen 0: GX_TG_MTX3x4 from the position (proj, input ABC1, source row 0) through TEXMTX0 (row 30): s = (x + 1) / 2,
        // t = (1 - y) / 2, q = 1 - z / 4 (a perspective-like q that varies over the quad)
        XF(0x1040, (1u << 1) | (1u << 2));
        const uint32_t tm[] = {f2u(0.5f), 0, 0, f2u(0.5f), 0, f2u(-0.5f), 0, f2u(0.5f), 0, 0, 0, f2u(1.0f)};
        WriteXF(30 * 4, 12, tm);
        XF(0x1018, 30u << 6);
    } else {
        XF(0x1040, 5u << 7);
        XF(0x1018, 60u << 6);
    }
    BP(0x00, 1);
    BP(0x28, (1u << 6) | (7u << 7));
}
void Reader(float left, float right, bool projective) { // the capture read as TEXC
    Bind(captureDestination, 4, projective);
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19));
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (1u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, 0);
    BP(0x41, (1u << 3) | (1u << 4));
    BP(0x42, 0);
    Quad(left, right, 0.6f, -0.6f, 0.5f, true);
    XF(0x1018, 60u << 6);
}
void Floor(unsigned k) {
    Pass();
    for (unsigned i = 0; i < 4; ++i) { // four bands whose colours change per picture
        Color((40 + 60 * i + 30 * k) & 255, (200 - 40 * i + 17 * k) & 255, (90 + 50 * i + 70 * k) & 255, 255);
        Quad(-1, 1, 1 - 0.5f * static_cast<float>(i), 0.5f - 0.5f * static_cast<float>(i), 0.6f, false);
    }
}
int g_w = 0, g_h = 0;
std::vector<uint8_t> ReadAndFlush() {
    Copy(readDestination, 6);
    const auto id = Pending().copyTex.back().texId;
    FrontPresent(false, nullptr);
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    Check(backend::DebugReadTexture(id, pixels, w, h) && w > 0 && h > 0, "picture read back");
    g_w = w;
    g_h = h;
    return pixels;
}
// The floor's band colour of picture k at clip-space height y (bands of 0.5 from the top).
void BandColour(unsigned k, float y, int out[3]) {
    const unsigned i = static_cast<unsigned>((1.0f - y) / 0.5f);
    out[0] = static_cast<int>((40 + 60 * i + 30 * k) & 255);
    out[1] = static_cast<int>((200 - 40 * i + 17 * k) & 255);
    out[2] = static_cast<int>((90 + 50 * i + 70 * k) & 255);
}
// The picture's pixel at clip-space (x, y); flipRows: the read-back's row 0 is the bottom.
const uint8_t* At(const std::vector<uint8_t>& p, float x, float y, bool flipRows) {
    int px = static_cast<int>((x + 1.0f) * 0.5f * static_cast<float>(g_w));
    int py = static_cast<int>((1.0f - y) * 0.5f * static_cast<float>(g_h));
    if (flipRows) py = g_h - 1 - py;
    px = px < 0 ? 0 : px >= g_w ? g_w - 1 : px;
    py = py < 0 ? 0 : py >= g_h ? g_h - 1 : py;
    return &p[(static_cast<size_t>(py) * static_cast<size_t>(g_w) + static_cast<size_t>(px)) * 4];
}
bool Near(const uint8_t* q, const int c[3], int tol) {
    for (int k = 0; k < 3; ++k)
        if (q[k] - c[k] > tol || c[k] - q[k] > tol) return false;
    return true;
}
uint64_t Hash(const std::vector<uint8_t>& p) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < p.size(); i += 4)
        for (int k = 0; k < 3; ++k) h = (h ^ p[i + k]) * 1099511628211ull;
    return h;
}
}

int main() {
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(96, 72), "production backend init (render 96x72 for a 64x64 EFB)");
    if (failures) return 1;
    ResetState();
    g.efbW = g.efbH = 64;
    QueueSetTarget();
    BP(0x43, 1);
    BP(0x20, 342u | (342u << 12));
    BP(0x21, 405u | (405u << 12));
    BP(0x59, 171u | (171u << 10));
    for (unsigned t = 0; t < 4; ++t) { BP(0xF6 + 2 * t, 0u | (1u << 2)); BP(0xF7 + 2 * t, 2u | (3u << 2)); } // (identity swap tables)
    const uint32_t m[] = {f2u(1),0,0,0, 0,f2u(1),0,0, 0,0,f2u(1),0};
    WriteXF(0, 12, m);
    const uint32_t ident[] = {f2u(1),0,0,0, 0,f2u(1),0,0, 0,0,f2u(1),0};
    WriteXF(60 * 4, 12, ident);
    const uint32_t proj[] = {f2u(1),0,f2u(1),0,f2u(1),0,1};
    WriteXF(0x1020, 7, proj);
    Viewport();
    const bool on = std::getenv("GXV_CAPREAD") && std::atol(std::getenv("GXV_CAPREAD")) == 1;
    uint64_t c0[7], c[7];
    CapReadCounters(c0);
    constexpr int kFrames = 6;
    std::vector<std::vector<uint8_t>> pics;
    for (int f = 0; f < kFrames; ++f) {
        Floor(static_cast<unsigned>(f));
        if (f > 0) Reader(-0.9f, -0.2f, true); // the previous picture's capture, through the screen projection
        Copy(captureDestination, 4);
        Reader(0.2f, 0.9f, false); // this picture's capture
        const auto p = ReadAndFlush();
        if (p.empty()) return 1;
        std::printf("frame%d hash=%016llx\n", f, static_cast<unsigned long long>(Hash(p)));
        pics.push_back(p);
    }
    // GX semantics of a reader before the picture's own capture (water23 round 2): texture memory keeps the previous picture's copy, and
    // that copy was made AFTER the reader had drawn in that picture. The projective reader at the left maps its screen position to the
    // same capture position, so from picture 2 on it sees its OWN output of the picture before: picture 1 shows picture 0's floor
    // (no reader in picture 0), and every later picture shows that same floor again (feedback), although the floor changes every
    // picture. This is what an IndDummy model drawn before DrawType_CaptureScreenIndirect does on the Wii and here; the floor beside it
    // (outside the reader) is the current one. Checked at four heights, whole, at this render size (96x72 for a 64x64 EFB), either row
    // order of the read-back. (Tolerance 8: the RGB565 copy's GX quantisation, which the port's copy does not apply.)
    const long hide = std::getenv("GXV_CAPREAD_HIDE") ? std::atol(std::getenv("GXV_CAPREAD_HIDE")) : 0;
    if (!(on && hide)) {
        bool okNorm = true, okFlip = true;
        for (int f = 1; f < kFrames; ++f)
            for (float y : {0.45f, 0.05f, -0.05f, -0.45f}) {
                int want[3];
                BandColour(0u, y, want);
                okNorm = okNorm && Near(At(pics[static_cast<size_t>(f)], -0.55f, y, false), want, 8);
                okFlip = okFlip && Near(At(pics[static_cast<size_t>(f)], -0.55f, y, true), want, 8);
            }
        if (std::getenv("CAPREAD_DEBUG"))
            for (int f = 1; f < kFrames; ++f)
                for (float y : {0.45f, -0.45f}) {
                    int want[3];
                    BandColour(0u, y, want);
                    const uint8_t* a = At(pics[static_cast<size_t>(f)], -0.55f, y, false);
                    const uint8_t* b = At(pics[static_cast<size_t>(f)], -0.55f, y, true);
                    std::printf("dbg f%d y%.2f want %d,%d,%d got %d,%d,%d / flipped %d,%d,%d\n", f, y, want[0], want[1], want[2], a[0], a[1], a[2], b[0], b[1], b[2]);
                }
        Check(okNorm || okFlip, "readers before the capture see the previous picture's capture whole, its own earlier output included (GX feedback)");
    }
    CapReadCounters(c);
    std::printf("info capread counters: pictures=%llu copies=%llu before=%llu after=%llu logged=%llu lines=%llu hidden=%llu\n",
                static_cast<unsigned long long>(c[0] - c0[0]), static_cast<unsigned long long>(c[1] - c0[1]), static_cast<unsigned long long>(c[2] - c0[2]),
                static_cast<unsigned long long>(c[3] - c0[3]), static_cast<unsigned long long>(c[4] - c0[4]), static_cast<unsigned long long>(c[5] - c0[5]),
                static_cast<unsigned long long>(c[6] - c0[6]));
    if (on && hide == 1) Check(c[6] - c0[6] == static_cast<uint64_t>(kFrames - 1), "GXV_CAPREAD_HIDE=1: the readers before the capture left out");
    if (on && hide == 2) Check(c[6] - c0[6] == static_cast<uint64_t>(kFrames), "GXV_CAPREAD_HIDE=2: the readers after the capture left out");
    if (!(on && hide)) Check(c[6] - c0[6] == 0, "nothing left out without GXV_CAPREAD_HIDE");
    if (on) {
        // (frame 0's reader before the capture samples a texture nothing has written yet: no copy of it, not counted as a reader of
        // a capture; frames 1..5: one before, one after)
        Check(c[1] - c0[1] >= kFrames, "the whole-EFB copies seen (the capture and the read-back copy of every picture)");
        Check(c[2] - c0[2] >= kFrames - 1 && c[3] - c0[3] >= kFrames, "readers before (an earlier picture's capture) and after the capture counted");
        Check(c[4] - c0[4] >= 1 && c[5] - c0[5] >= 2, "a picture logged with its reader lines");
    } else {
        Check(c[0] - c0[0] == 0, "off: nothing looked at");
    }
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
