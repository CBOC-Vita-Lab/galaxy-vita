// GXV_PX_NOOP / GXV_PX_DEADCLEAR / GXV_PX_SCISSOR (vgx_backend_gl.cpp, "pixcut"): pixel-work cuts that must keep the picture exactly,
// through the production GX front end and the production GLES back end on Mesa (no game data). tools/test_vgx_pxcut_gl.sh runs it with
// every switch off and on, at a render size equal to the EFB's and at a non-integer scale; the pictures (every channel) must be identical
// and, with a switch on, its cut must have happened.
//   Frame 1: a floor; a copy of the EFB with a clear of colour, alpha and depth; a quad over the whole EFB that writes colour and alpha
//            (no depth test, no blending: the clear's colour is dead - GXV_PX_DEADCLEAR); two draws that change nothing (colour and alpha
//            updates off with no depth update; blended ZERO / ONE - GXV_PX_NOOP); small quads alternating between the full scissor and a
//            scissor that holds them (GXV_PX_SCISSOR keeps the scissor in place) and quads that a scissor really cuts; blended quads over
//            all of it (order matters); the picture read back (RGBA).
//   Frame 2: an isolated offscreen pass (a corner of the EFB drawn and copied with a clear, vgx_draw.cpp pass isolation) whose first draw
//            is a rectangle over the whole corner: the pass's clear is dead (colour; depth too, no draw of the pass tests depth); the
//            pass's picture drawn into the EFB; read back.
//   Frame 3: the same as frame 1 with a scissor that cuts the covering quad (its clear must stay) and with the quad's alpha update off.
//   Frame 4: frame 2 with a pass whose second draw tests depth (only the colour part of its clear goes).
//   Frame 5: frame 1 with the covering quad one EFB column short of the cleared EFB (the clear must stay).
#include "vgx_state.h"
#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) {
    if (std::getenv("PXCUT_DUMP")) { // (the commands the back end gets: draws, passes, copies)
        for (size_t i = 0; i < b.cmds.size(); ++i) {
            const Cmd& c = b.cmds[i];
            if (c.type == CMD_DRAW) {
                const DrawRec& d = b.draws[c.arg];
                std::printf("  %2zu draw: %u idx, %u vtx, cover %u, ex %d,%d %dx%d, sc %d,%d %dx%d, blend %u cw %u aw %u zt %u zw %u\n", i, d.idxCount, d.vtxCount, d.cover, d.exX,
                            d.exY, d.exW, d.exH, d.rs.scX, d.rs.scY, d.rs.scW, d.rs.scH, d.rs.blend, d.rs.colorWrite, d.rs.alphaWrite, d.rs.depthTest, d.rs.depthWrite);
            } else if (c.type == CMD_COPY_TEX) {
                const CopyTexCmd& t = b.copyTex[c.arg];
                std::printf("  %2zu copy -> %u: src %d,%d %dx%d, fromPass %u, skip %u, fill %u, clear %u/%u%u%u\n", i, t.texId, t.src.x, t.src.y, t.src.w, t.src.h, t.fromPass, t.skip,
                            t.fill, t.clear.enable, t.clear.color, t.clear.alpha, t.clear.depth);
            } else if (c.type == CMD_PASS_BEGIN) {
                const PassCmd& p = b.passes[c.arg];
                std::printf("  %2zu pass: %d,%d %dx%d direct %u atlas %u\n", i, p.rect.x, p.rect.y, p.rect.w, p.rect.h, p.directTex, p.atlasTex);
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
alignas(32) unsigned char scratch[32];
alignas(32) unsigned char passDest[32];
alignas(32) unsigned char passDest2[32];
alignas(32) unsigned char readDestination[32];
unsigned failures = 0;
void Check(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
void XF(unsigned reg, unsigned value) { WriteXF(reg, 1, &value); }
// Viewport and scissor to the EFB rectangle (x, y, w, h).
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
void Plain() { // no texture: colour and alpha = C0
    BP(0x00, 0);
    XF(0x103F, 0);
    BP(0x28, 7u << 7);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
}
// A quad from (l, b) to (r, t) in the viewport's -1..1, at depth d (0 near .. 1 far); textured: coordinates 0..1.
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
void Bind(void* data, unsigned format, int w, int h) {
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
    BP(0x00, 1);
    BP(0x28, (1u << 6) | (7u << 7));
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19)); // colour = TEXC
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (4u << 4) | (1u << 19)); // alpha = TEXA
}
void ZMode(bool test, unsigned func, bool update) { BP(0x40, (test ? 1u : 0u) | (func << 1) | (update ? 1u << 4 : 0u)); }
// GX blend mode: enable, colour update, alpha update, dst factor, src factor
void Blend(bool on, bool colour, bool alpha, unsigned dst = 0, unsigned src = 1) {
    BP(0x41, (on ? 1u : 0u) | (colour ? 1u << 3 : 0u) | (alpha ? 1u << 4 : 0u) | (dst << 5) | (src << 8));
}
std::vector<uint8_t> ReadAndFlush() {
    Scissor(0, 0, 64, 64);
    Copy(readDestination, 6, EfbRect{0, 0, 64, 64}, false);
    const auto id = Pending().copyTex.back().texId;
    FrontPresent(false, nullptr);
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    Check(backend::DebugReadTexture(id, pixels, w, h) && w > 0 && h > 0, "picture read back");
    std::printf("picture %dx%d\n", w, h);
    return pixels;
}
uint64_t Hash(const std::vector<uint8_t>& p) {
    uint64_t h = 14695981039346656037ull;
    for (uint8_t b : p) h = (h ^ b) * 1099511628211ull; // (every channel)
    return h;
}
void ClearColour(unsigned r, unsigned green, unsigned b, unsigned a, unsigned z24) {
    BP(0x4F, r | (a << 8));
    BP(0x50, b | (green << 8));
    BP(0x51, z24);
}
// Frame 1 (cutCover: frame 3's variant, a scissor cutting the covering quad and its alpha not written)
std::vector<uint8_t> Frame1(bool cutCover, bool shortCover = false) {
    Plain();
    Viewport(0, 0, 64, 64);
    Scissor(0, 0, 64, 64);
    // floor, depth-tested, depth written
    ZMode(true, 7, true);
    Blend(false, true, true);
    Color(40, 200, 90, 200);
    QuadRect(-1, 0, -1, 1, 0.5f);
    Color(220, 120, 30, 100);
    QuadRect(0, 1, -1, 1, 0.4f);
    // the copy, clearing colour, alpha and depth (to dark blue, alpha 77, depth 0.75)
    ClearColour(10, 20, 120, 77, 0xC00000);
    Copy(scratch, 6, EfbRect{0, 0, 64, 64}, true);
    // the covering quad: no depth test, no blending, colour (and alpha) written
    ZMode(false, 7, false);
    Blend(false, true, !cutCover);
    if (cutCover) Scissor(0, 0, 64, 40);
    Color(90, 60, 180, 140);
    QuadRect(-1, shortCover ? 1 - 2.0f / 64 : 1, -1, 1, 0.5f); // (short: one EFB column of the cleared EFB is left uncovered)
    Scissor(0, 0, 64, 64);
    // two draws that change nothing
    Blend(false, false, false);
    Color(255, 255, 255, 255);
    QuadRect(-0.5f, 0.5f, -0.5f, 0.5f, 0.5f);
    Blend(true, true, true, 1, 0); // dst ONE, src ZERO
    QuadRect(-0.8f, 0.2f, -0.8f, 0.2f, 0.5f);
    // depth-tested quad against the cleared depth (0.75): the near half is drawn
    ZMode(true, 3, false); // LEQUAL
    Blend(false, true, true);
    Color(250, 10, 10, 255);
    QuadRect(-1, -0.6f, -1, 1, 0.5f);
    QuadRect(-0.6f, -0.2f, -1, 1, 0.9f);
    ZMode(false, 7, false);
    // small blended quads: full scissor, a scissor that holds the quad, full again, a scissor that cuts
    Blend(true, true, true, 5, 4); // src alpha / inv src alpha
    // (0, 2: the full scissor; 1, 3: a scissor that holds the quad; 4: one that cuts it, right after a full one)
    const float q[5][4] = {{-0.9f, -0.5f, -0.9f, -0.5f}, {-0.3f, 0.1f, -0.3f, 0.1f}, {0.2f, 0.6f, 0.3f, 0.7f}, {-0.2f, 0.2f, 0.4f, 0.9f}, {0.5f, 0.95f, -0.95f, -0.4f}};
    const int sc[5][4] = {{0, 0, 64, 64}, {16, 16, 32, 32}, {0, 0, 64, 64}, {20, 1, 40, 30}, {8, 8, 44, 44}};
    const int order[5] = {0, 1, 3, 2, 4};
    for (int o = 0; o < 5; ++o) {
        const int k = order[o];
        Scissor(sc[k][0], sc[k][1], sc[k][2], sc[k][3]);
        Color(30 + 40 * k, 200 - 30 * k, 60 + 35 * k, 100 + 25 * k);
        QuadRect(q[k][0], q[k][1], q[k][2], q[k][3], 0.5f);
    }
    // the same once more in another order (scissors held by the quads, then cut)
    for (int o = 4; o >= 0; --o) {
        const int k = order[(o + 2) % 5];
        Scissor(sc[k][0], sc[k][1], sc[k][2], sc[k][3]);
        Color(200 - 30 * k, 30 + 40 * k, 160, 90 + 30 * k);
        QuadRect(q[k][0] + 0.03f, q[k][1] + 0.03f, q[k][2] + 0.02f, q[k][3] + 0.02f, 0.5f);
    }
    Scissor(0, 0, 64, 64);
    return ReadAndFlush();
}
// One isolated pass into the EFB's corner (x, y, 32, 32), copied with a clear into dest; depthTested: its second draw tests depth.
void PassInto(void* dest, int x, int y, bool depthTested, unsigned shade) {
    Plain();
    Viewport(x, y, 32, 32);
    Scissor(x, y, 32, 32);
    ZMode(false, 7, false);
    Blend(false, true, true);
    Color(shade, 255 - shade, 90, 180);
    QuadRect(-1, 1, -1, 1, 0.5f); // the whole corner
    Blend(true, true, true, 5, 4);
    if (depthTested) ZMode(true, 3, true);
    Color(250, 250, 20, 120);
    QuadRect(-0.5f, 0.6f, -0.7f, 0.3f, 0.3f);
    ZMode(false, 7, false);
    // the copy clears every plane the pass writes (colour, alpha, depth)
    Blend(false, true, true);
    ZMode(false, 7, true);
    ClearColour(0, 0, 0, 0, 0xFFFFFF);
    Copy(dest, 6, EfbRect{static_cast<int16_t>(x), static_cast<int16_t>(y), 32, 32}, true);
}
std::vector<uint8_t> Frame2(bool depthTested) {
    // the EFB cleared to a known value first (so the corners hold constants: pass isolation)
    Plain();
    Viewport(0, 0, 64, 64);
    Scissor(0, 0, 64, 64);
    Blend(false, true, true);
    ZMode(false, 7, true);
    ClearColour(0, 0, 0, 0, 0xFFFFFF);
    Copy(scratch, 6, EfbRect{0, 0, 64, 64}, true);
    void* const dest = depthTested ? passDest2 : passDest;
    PassInto(dest, depthTested ? 32 : 0, depthTested ? 16 : 0, depthTested, depthTested ? 200 : 40);
    // the picture into the EFB
    Viewport(0, 0, 64, 64);
    Scissor(0, 0, 64, 64);
    ZMode(false, 7, false);
    Blend(false, true, true);
    Bind(dest, 6, 32, 32);
    QuadRect(-1, 0.5f, -1, 0.2f, 0.5f, true);
    Plain();
    return ReadAndFlush();
}
}

int main() {
    const int rw = std::getenv("PXCUT_RW") ? std::atoi(std::getenv("PXCUT_RW")) : 64;
    const int rh = std::getenv("PXCUT_RH") ? std::atoi(std::getenv("PXCUT_RH")) : 64;
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(rw, rh), "production backend init");
    if (failures) return 1;
    ResetState();
    g.efbW = g.efbH = 64;
    QueueSetTarget();
    BP(0x43, 1); // GX_PF_RGBA6_Z24
    Scissor(0, 0, 64, 64);
    BP(0x59, 171u | (171u << 10));
    const uint32_t m[] = {f2u(1), 0, 0, 0, 0, f2u(1), 0, 0, 0, 0, f2u(1), 0};
    WriteXF(0, 12, m);
    const uint32_t proj[] = {f2u(1), 0, f2u(1), 0, f2u(1), 0, 1};
    WriteXF(0x1020, 7, proj);
    const auto p1 = Frame1(false);
    const auto p2 = Frame2(false);
    const auto p3 = Frame1(true);
    const auto p4 = Frame2(true);
    // (GXV_GPU_SCENES=2: its window is 5 s; wait so that the last frame writes the lines - the hooks' smoke test, no GPU times here)
    if (std::getenv("GXV_GPU_SCENES")) usleep(5100 * 1000);
    const auto p5 = Frame1(false, true);
    uint64_t noop = 0, cc = 0, cd = 0, cw = 0, sk = 0;
    backend::DebugPxCuts(noop, cc, cd, cw, sk);
    std::printf("frame1 hash=%016llx\nframe2 hash=%016llx\nframe3 hash=%016llx\nframe4 hash=%016llx\nframe5 hash=%016llx\n", static_cast<unsigned long long>(Hash(p1)),
                static_cast<unsigned long long>(Hash(p2)), static_cast<unsigned long long>(Hash(p3)), static_cast<unsigned long long>(Hash(p4)),
                static_cast<unsigned long long>(Hash(p5)));
    std::printf("cuts noop=%llu clear_colour=%llu clear_depth=%llu clear_whole=%llu scissor_kept=%llu\n", static_cast<unsigned long long>(noop),
                static_cast<unsigned long long>(cc), static_cast<unsigned long long>(cd), static_cast<unsigned long long>(cw), static_cast<unsigned long long>(sk));
    auto on = [](const char* n) { const char* e = std::getenv(n); return e && std::strcmp(e, "0") != 0; };
    if (on("GXV_PX_NOOP")) Check(noop == 6, "GXV_PX_NOOP: the six draws that change nothing (two in frames 1, 3 and 5) left out");
    else Check(noop == 0, "GXV_PX_NOOP off: nothing left out");
    if (on("GXV_PX_DEADCLEAR")) {
        // frame 1: the EFB clear's colour (depth stays); frame 2: the pass's whole clear; frame 4: its colour part; frames 3 and 5: nothing
        Check(cc == 3 && cd == 1 && cw == 1, "GXV_PX_DEADCLEAR: frame 1's EFB clear colour, frame 2's pass clear whole, frame 4's pass clear colour; frames 3 and 5 keep theirs");
    } else {
        Check(cc == 0 && cd == 0 && cw == 0, "GXV_PX_DEADCLEAR off: every clear made");
    }
    if (on("GXV_PX_SCISSOR")) Check(sk > 0, "GXV_PX_SCISSOR: scissor changes kept");
    else Check(sk == 0, "GXV_PX_SCISSOR off: every change made");
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
