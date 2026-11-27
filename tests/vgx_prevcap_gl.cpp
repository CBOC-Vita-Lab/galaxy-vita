// GXV_PREVCAP bit 1 (prevcap25; vgx_draw.cpp PrevCapPass, vgx_backend_gl.cpp PrevKeep): the game's marked "Indirect" capture left out and its
// readers answered by the PREVIOUS picture, through the production GX front end and GLES back end on Mesa (no game data).
// Each picture: a floor, the marked whole-EFB RGB565 capture, a full-screen reader of the capture texture (TEXC only), the final picture read
// back, the display copy (clears). Scenarios (env): TEST_REF=1 the reference run: picture 1 as the game does it, every later picture NO capture
// and its reader samples the test's own copy of the previous picture's final EFB (the one-picture-old source made by an ordinary copy);
// TEST_CUT=k a camera cut before picture k (port_camera_cut_count); TEST_STAGE=k picture k carries another stage tag.
// tools/test_vgx_prevcap_gl.sh compares the runs: with GXV_PREVCAP=1 pictures 2.. must be bit-identical to the reference, with fewer scenes than
// GXV_PREVCAP=0 from picture 2; the fallbacks (cut, other stage) must equal the unchanged run picture for picture.
#include "vgx_state.h"
#ifdef PREVCAP_BASE_SHIM // (built against the base tree, which has no PrevCapCounters: the "default path is the base's path" run)
namespace vgx { inline void PrevCapCounters(uint64_t o[16]) { for (int i = 0; i < 16; ++i) o[i] = 0; } }
#endif
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
void Reader(float left, float right, void* tex = indirectDestination, unsigned fmt = 4) { // the capture read as TEXC (an indirect model)
    Bind(tex, fmt);
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
    if (std::getenv("TEST_NOREAD")) { // (the game's own ending: the display copy alone; nothing is read back, the scene count is what it shows)
        g.dispCopySrc = EfbRect{0, 0, 64, 64};
        QueueCopyDisp(true);
        FrontPresent(false, nullptr);
        scenes = backend::DebugLastFrameScenes();
        return std::vector<uint8_t>(64 * 64 * 4, 0);
    }
    Copy(readDestination, 6);
    const auto id = Pending().copyTex.back().texId;
    g.dispCopySrc = EfbRect{0, 0, 64, 64}; // (the picture's display copy, which clears: GXV_PREVCAP's keeper is taken here)
    QueueCopyDisp(true);
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


static bool FakePresent(unsigned, int, int, int, int) { return true; } // (as the console's: the display is drawn and the EFB's next scene is a new one)
static uint32_t g_testCuts = 0;
extern "C" uint32_t port_camera_cut_count(void) { return g_testCuts; }
int main() {
    backend::SetPresentHook(FakePresent);
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
    const bool ref = std::getenv("TEST_REF") != nullptr;
    const int cutAt = std::getenv("TEST_CUT") ? std::atoi(std::getenv("TEST_CUT")) : 0;
    const int stageAt = std::getenv("TEST_STAGE") ? std::atoi(std::getenv("TEST_STAGE")) : 0;
    const int N = 8;
    uint64_t c0[16], c[16];
    PrevCapCounters(c0);
    std::vector<uint8_t> pics[N + 1];
    unsigned scenes[N + 1] = {};
    for (int i = 1; i <= N; ++i) {
        if (i == cutAt) ++g_testCuts;
        Floor(40 + 20 * i, 200 - 15 * i, 90 + 10 * i);
        // (the reference run's pictures 2.. read the previous picture's final copy and capture nothing, except at a cut or another stage:
        // there the reference run is the unchanged game)
        const bool realRef = ref && !(i == 1);
        const unsigned stage = i == stageAt ? 2u : 0u;
        if (!realRef) MarkedCapture(stage);
        if (realRef) Reader(-1, 1, readDestination, 6);
        else Reader(-1, 1);
        pics[i] = ReadAndFlush(scenes[i]);
        if (pics[i].empty()) return 1;
        std::printf("pic%d scenes=%u hash=%016llx\n", i, scenes[i], static_cast<unsigned long long>(Hash(pics[i])));
    }
    PrevCapCounters(c);
    std::printf("info counters pictures=%llu marked=%llu used=%llu readers=%llu pre=%llu keeps=%llu noPrev=%llu cut=%llu alpha=%llu atlas=%llu noRead=%llu shape=%llu direct=%llu skipped=%llu\n",
                (unsigned long long)(c[0] - c0[0]), (unsigned long long)(c[1] - c0[1]), (unsigned long long)(c[2] - c0[2]), (unsigned long long)(c[3] - c0[3]),
                (unsigned long long)(c[4] - c0[4]), (unsigned long long)(c[5] - c0[5]), (unsigned long long)(c[6] - c0[6]), (unsigned long long)(c[7] - c0[7]),
                (unsigned long long)(c[8] - c0[8]), (unsigned long long)(c[9] - c0[9]), (unsigned long long)(c[10] - c0[10]), (unsigned long long)(c[11] - c0[11]),
                (unsigned long long)(c[12] - c0[12]), (unsigned long long)(c[13] - c0[13]));
    // the picture's colour at pixel (8, 32) per picture: with a full-screen reader of the previous picture it freezes at picture 1's floor
    // (the feedback: the reader's footprint holds the reader's own earlier output)
    if (std::getenv("TEST_NOREAD")) { Check(glGetError() == GL_NO_ERROR, "no GL error"); return failures ? 1 : 0; }
    std::printf("centre");
    for (int i = 1; i <= N; ++i) {
        const size_t o = (32u * 64u + 8u) * 4u;
        std::printf(" %02x%02x%02x", pics[i][o], pics[i][o + 1], pics[i][o + 2]);
    }
    std::printf("\n");
    if (const char* dump = std::getenv("PREVCAP_DUMP")) {
        for (int i = 1; i <= N; ++i) {
            char name[512];
            std::snprintf(name, sizeof name, "%s-p%d.rgba", dump, i);
            if (FILE* out = std::fopen(name, "wb")) { std::fwrite(pics[i].data(), 1, pics[i].size(), out); std::fclose(out); }
        }
    }
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
