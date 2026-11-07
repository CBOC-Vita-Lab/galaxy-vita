// GXV_EFBSWAP2: the two whole-EFB copies with a conversion that every game picture makes, through the production GX front end
// and the production GLES back end on Mesa (no game data). Frame 1, as SceneFunction::executeDrawSilhouetteAndFillShadow and the
// indirect capture draw it: a floor, alpha volumes, a GX_CTF_A8 capture read by a quad whose TEV compares TEXA only (as
// MR::fillSilhouetteColor), an RGB565 capture read by a quad as TEXC only (as an indirect model), the picture read back. Frame 2:
// a quad reads the RGB565 capture of frame 1 as TEXA (the alpha the conversion sets to 1), before any copy into it: the back end
// must convert the buffer it handed over first. Run once with GXV_EFBSWAP2=0 and once with =1 (tools/test_vgx_efbswap2_gl.sh):
// the pictures must be identical and frame 1 must have two scenes less. Frames 4-6 also check GXV_EFBSWAP2_HOLD (a texture
// converted for a later frame gets ordinary copies for a while) and GXV_EFBSWAP2_PREDICT (an earlier reader in the copy's own
// frame of a channel the conversion changes keeps the ordinary copy); the script runs them on and off, and GXV_EFBSWAP_FULLMASK.
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
alignas(32) unsigned char maskDestination[32];
alignas(32) unsigned char indirectDestination[32];
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
// The picture (an RGBA8 copy of the EFB) at the end of the frame, and the frame's scenes.
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
        for (int k = 0; k < 3; ++k) h = (h ^ p[i + k]) * 1099511628211ull; // (colour: the picture)
    return h;
}
const uint8_t* Px(const std::vector<uint8_t>& p, int x, int y) { return &p[(y * 64 + x) * 4]; }
}

int main() {
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(64, 64), "production backend init");
    if (failures) return 1;
    ResetState();
    g.efbW = g.efbH = 64;
    QueueSetTarget();
    BP(0x43, 1); // GX_PF_RGBA6_Z24
    BP(0x20, 342u | (342u << 12));
    BP(0x21, 405u | (405u << 12));
    BP(0x59, 171u | (171u << 10));
    const uint32_t m[] = {f2u(1),0,0,0, 0,f2u(1),0,0, 0,0,f2u(1),0};
    WriteXF(0, 12, m);
    const uint32_t proj[] = {f2u(1),0,f2u(1),0,f2u(1),0,1};
    WriteXF(0x1020, 7, proj);
    Viewport();
    const unsigned swaps0 = backend::DebugEfbSwapsPartial(), conv0 = backend::DebugRawConverts();

    // ---- frame 1
    Pass();
    // floor: a left / right colour step, alpha 0 (GXSetDstAlpha(TRUE, 0)), depth 0.5
    BP(0x40, 1 | (7u << 1) | (1u << 4));
    BP(0x41, (1u << 3) | (1u << 4));
    BP(0x42, 1u << 8);
    Color(40, 200, 90, 255);
    Quad(-1, 0, 0.5f);
    Color(220, 120, 30, 255);
    Quad(0, 1, 0.5f);
    // shadow-volume style alpha: +4 on the left half (front face in front of the floor), cancelling on the right
    Color(0, 0, 0, 4);
    BP(0x40, 1 | (6u << 1));
    BP(0x42, 0);
    BP(0x41, 1 | (1u << 4) | (1u << 8) | (1u << 5));
    Quad(-1, 1, 0.75f);
    BP(0x41, (1u << 4) | (1u << 11));
    Quad(-1, 0, 0.25f);
    Quad(0, 1, 0.6f);
    // the silhouette capture: GX_CTF_A8 of the whole EFB, read by a quad comparing TEXA with K0.a = 1 (A8_GT), blending black at
    // half over the shadowed pixels, constant alpha 0 written (MR::fillSilhouetteColor)
    Copy(maskDestination, 0x27);
    Bind(maskDestination, 1);
    Color(0, 0, 0, 128);
    BP(0xE0, (1u << 23) | (1u << 12));
    BP(0xE1, 1u << 23);
    BP(0xF6, (28u << 9) | (1u << 2)); // K0.a for stage 0; swap table 0 r, g ...
    BP(0xF7, 2u | (3u << 2));         // ... b, a (as GX's own register writes keep them)
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 2u | (1u << 19)); // colour = C0
    BP(0xC1, (4u << 13) | (6u << 10) | (1u << 7) | (7u << 4) | (3u << 16) | (1u << 19) | (3u << 20)); // (TEXA > K0.a) ? A0 : 0 (A8_GT)
    BP(0xF3, (4u << 16) | (7u << 19)); // alpha > 0
    BP(0x40, 0);
    BP(0x41, 1 | (1u << 3) | (1u << 4) | (4u << 8) | (5u << 5)); // blend SRCALPHA / INVSRCALPHA
    BP(0x42, 1u << 8);
    Quad(-1, 1, 0.5f, true);
    // the indirect capture: GX_TF_RGB565 of the whole EFB, read by a quad as TEXC only (squeezed into the right half)
    Copy(indirectDestination, 4);
    Bind(indirectDestination, 4);
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19)); // colour = TEXC
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (1u << 4) | (1u << 19)); // alpha = A0
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, 0);
    BP(0x41, (1u << 3) | (1u << 4));
    BP(0x42, 0);
    Quad(0.25f, 1, 0.5f, true);
    unsigned scenes1 = 0;
    const auto pic1 = ReadAndFlush(scenes1);
    const unsigned swaps1 = backend::DebugEfbSwapsPartial() - swaps0;

    // ---- frame 2: the RGB565 capture of frame 1 read as TEXA (1 after the conversion) over the left quarter, no copy into it first
    Bind(indirectDestination, 4);
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 9u | (1u << 19)); // colour = TEXA
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (1u << 4) | (1u << 19));
    Quad(-1, -0.5f, 0.5f, true);
    // and as TEXC over the next quarter (what the buffer already has)
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19));
    Quad(-0.5f, 0, 0.5f, true);
    unsigned scenes2 = 0;
    const auto pic2 = ReadAndFlush(scenes2);
    const unsigned conv = backend::DebugRawConverts() - conv0;

    // ---- frame 3: an RGB565 capture read as TEXA in its own frame: the front end must leave it an ordinary copy
    const unsigned swaps2 = backend::DebugEfbSwapsPartial();
    Copy(indirectDestination, 4);
    Bind(indirectDestination, 4);
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 9u | (1u << 19)); // colour = TEXA
    Quad(-1, -0.5f, 0.5f, true);
    unsigned scenes3 = 0;
    const auto pic3 = ReadAndFlush(scenes3);
    const unsigned swaps3 = backend::DebugEfbSwapsPartial() - swaps2;
    // ---- frames 4 and 5: the capture made without a copy scene again (TEXC reader), then an ordinary capture into the same texture
    // (TEXA reader in its frame): the buffer the texture holds becomes its storage (no allocation: AdoptEfbBuffer), same picture
    const unsigned adopt0 = backend::DebugEfbAdopts(), hold0 = backend::DebugEfbSwapHolds();
    Copy(indirectDestination, 4);
    Bind(indirectDestination, 4);
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19)); // TEXC
    Quad(0.25f, 1, 0.5f, true);
    unsigned scenes4 = 0;
    const auto pic4 = ReadAndFlush(scenes4);
    Copy(indirectDestination, 4);
    Bind(indirectDestination, 4);
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 9u | (1u << 19)); // TEXA
    Quad(-1, -0.5f, 0.5f, true);
    unsigned scenes5 = 0;
    const auto pic5 = ReadAndFlush(scenes5);
    const unsigned adopts = backend::DebugEfbAdopts() - adopt0, holds = backend::DebugEfbSwapHolds() - hold0;
    // ---- frame 6 (GXV_EFBSWAP2_PREDICT): the indirect capture's texture read as TEXA (the alpha RGB565 sets to 1) BEFORE this
    // frame's capture into it - the previous picture's reader -, then the capture and a TEXC reader after it. The frame's own
    // readers would allow the swap; the earlier reader says the next frame would need it converted: ordinary copy.
    const unsigned swaps5 = backend::DebugEfbSwapsPartial();
    Pass();
    BP(0x40, 1 | (7u << 1) | (1u << 4));
    BP(0x41, (1u << 3) | (1u << 4));
    BP(0x42, 1u << 8);
    Color(30, 60, 220, 255);
    Quad(-1, 1, 0.5f);
    Bind(indirectDestination, 4);
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 9u | (1u << 19)); // colour = TEXA
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (1u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, 0);
    BP(0x41, (1u << 3) | (1u << 4));
    BP(0x42, 0);
    Quad(-1, -0.5f, 0.5f, true);
    const uint32_t earlyTex = Pending().draws.empty() ? 0 : Pending().draws.back().tex[0].texId;
    Copy(indirectDestination, 4);
    if (std::getenv("EFBSWAP2_ROW")) std::printf("frame6 early reader texture %u, copy texture %u\n", earlyTex, Pending().copyTex.back().texId);
    Bind(indirectDestination, 4);
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19)); // colour = TEXC
    Quad(0.5f, 1, 0.5f, true);
    unsigned scenes6 = 0;
    const auto pic6 = ReadAndFlush(scenes6);
    const unsigned swaps6 = backend::DebugEfbSwapsPartial() - swaps5;
    if (pic1.empty() || pic2.empty() || pic3.empty() || pic4.empty() || pic5.empty() || pic6.empty()) return 1;
    std::printf("frame6 partial_swaps=%u hash=%016llx\n", swaps6, static_cast<unsigned long long>(Hash(pic6)));
    std::printf("frame4 hash=%016llx\nframe5 adopts=%u hash=%016llx\n", static_cast<unsigned long long>(Hash(pic4)), adopts,
                static_cast<unsigned long long>(Hash(pic5)));
    std::printf("frame3 scenes=%u partial_swaps=%u hash=%016llx\n", scenes3, swaps3, static_cast<unsigned long long>(Hash(pic3)));
    Check(swaps3 == 0 && Px(pic3, 4, 32)[0] == 255 && Px(pic3, 4, 32)[2] == 255, "frame 3: a TEXA reader in the same frame keeps the ordinary copy");
    if (std::getenv("EFBSWAP2_ROW"))
        for (int x = 0; x < 64; x += 4) std::printf("row x=%d: f1 %u,%u,%u,%u  f2 %u,%u,%u,%u\n", x, Px(pic1, x, 32)[0], Px(pic1, x, 32)[1], Px(pic1, x, 32)[2],
                                                     Px(pic1, x, 32)[3], Px(pic2, x, 32)[0], Px(pic2, x, 32)[1], Px(pic2, x, 32)[2], Px(pic2, x, 32)[3]);
    std::printf("frame1 scenes=%u partial_swaps=%u hash=%016llx px(8,32)=%u,%u,%u px(40,32)=%u,%u,%u\n", scenes1, swaps1,
                static_cast<unsigned long long>(Hash(pic1)), Px(pic1, 8, 32)[0], Px(pic1, 8, 32)[1], Px(pic1, 8, 32)[2], Px(pic1, 40, 32)[0],
                Px(pic1, 40, 32)[1], Px(pic1, 40, 32)[2]);
    std::printf("frame2 scenes=%u converts=%u hash=%016llx px(4,32)=%u,%u,%u px(20,32)=%u,%u,%u\n", scenes2, conv,
                static_cast<unsigned long long>(Hash(pic2)), Px(pic2, 4, 32)[0], Px(pic2, 4, 32)[1], Px(pic2, 4, 32)[2], Px(pic2, 20, 32)[0],
                Px(pic2, 20, 32)[1], Px(pic2, 20, 32)[2]);
    const char* mode = std::getenv("GXV_EFBSWAP2");
    const bool on = mode && std::strcmp(mode, "0") != 0; // (off unless asked for: the default since r32)
    Check(Px(pic2, 4, 32)[0] == 255 && Px(pic2, 4, 32)[1] == 255 && Px(pic2, 4, 32)[2] == 255, "frame 2: the capture's alpha reads 1 (converted)");
    Check(on ? swaps1 == 2 : swaps1 == 0, on ? "frame 1: both converted captures made without a copy scene" : "frame 1: ordinary copies");
    Check(on ? conv == 1 : conv == 0, on ? "frame 2: the capture converted for its TEXA reader" : "frame 2: nothing to convert");
    const char* holdEnv = std::getenv("GXV_EFBSWAP2_HOLD");
    const bool hold = !holdEnv || std::atol(holdEnv) > 0; // (default 1800 pictures)
    const char* predEnv = std::getenv("GXV_EFBSWAP2_PREDICT");
    const bool predict = !predEnv || std::strcmp(predEnv, "0") != 0;
    if (on && hold) {
        // (frame 2 converted the indirect capture: frame 4's capture into it is an ordinary copy, frame 5 has no buffer to adopt)
        Check(adopts == 0 && holds == 1, "frame 4: the capture converted in frame 2 is held to ordinary copies (GXV_EFBSWAP2_HOLD)");
    } else {
        Check(on ? adopts == 1 : adopts == 0, on ? "frame 5: the held buffer became the texture's storage" : "frame 5: no buffer to adopt");
        Check(holds == 0, "no holds");
    }
    // (frame 6's early reader also has frame 1's swapped A8 buffer converted first, which holds that texture too: the prediction
    // alone is seen with GXV_EFBSWAP2_HOLD=0)
    Check(on && !predict && !hold ? swaps6 == 1 : swaps6 == 0,
          on && !predict && !hold ? "frame 6: swapped (no prediction, no hold)" : "frame 6: the earlier reader of another channel keeps the ordinary copy");
    const char* lfEnv = std::getenv("GXV_VGL_LOADFROM");
    if (lfEnv && std::strcmp(lfEnv, "0") != 0)
        Check(backend::DebugEfbLoadFroms() > 0, "GXV_VGL_LOADFROM: swapped buffers drawn back by the next scene's load (emulated)");
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
