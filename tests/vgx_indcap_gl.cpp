// GXV_INDCAP_SKIP / GXV_INDCAP_HALFRATE (gpu23; vgx_draw.cpp MarkTransientCopies): the game's "Indirect" screen capture, marked by
// PORT_GX_MARK_TRANSIENT_COPY (BP 0xFF kind 10, as port_vgx_marker writes it), through the production GX front end and GLES back
// end on Mesa (no game data). tools/test_vgx_indcap_gl.sh runs it with the switches off and on and compares the pictures.
//   frame 1: a floor, the marked RGB565 capture, no reader, a plain quad after it: with GXV_INDCAP_SKIP=1 the copy is left out
//            (fewer scenes), the picture is the same.
//   frames 2-3 (round 2, the console's stale reads): a reader of the previous picture's capture drawn BEFORE the picture's own
//            capture, nothing after it. Frame 2 is that reader's first picture (one stale read, counted). With the round-1 rule
//            (GXV_INDCAP_PREDICT=0 GXV_INDCAP_HOLD=0) frame 2's capture is left out too and frame 3 reads stale again (a wrong
//            picture: the reproduction); with the default PREDICT frame 2's capture is kept and frame 3 is exact.
//   frame 4: readers before and after the capture.
//   frames 5-7 (GXV_INDCAP_HALFRATE=1): with frame 4 made, a capture with a reader is left out (5), made (6), left out (7).
//   frame 8: the marker of another stage (tag 2), nothing reads the capture: with GXV_INDCAP_STAGE alone (PREDICT=0) it is left
//            out again (the stage has no early reader); inside the PREDICT window it is kept.
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
    uint64_t c0[8], c[8];
    IndCapCounters(c0);
    const long skip = std::getenv("GXV_INDCAP_SKIP") ? std::atol(std::getenv("GXV_INDCAP_SKIP")) : 0;
    const long half = std::getenv("GXV_INDCAP_HALFRATE") ? std::atol(std::getenv("GXV_INDCAP_HALFRATE")) : 0;
    const long predict = std::getenv("GXV_INDCAP_PREDICT") ? std::atol(std::getenv("GXV_INDCAP_PREDICT")) : 120;
    const long stageOn = std::getenv("GXV_INDCAP_STAGE") ? std::atol(std::getenv("GXV_INDCAP_STAGE")) : 1;

    unsigned s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;
    // frame 1: a capture nothing reads (left out with =1)
    Floor(40, 200, 90);
    MarkedCapture();
    Pass();
    Color(250, 250, 10, 255);
    Quad(-0.25f, 0.25f, 0.4f);
    const auto p1 = ReadAndFlush(s1);
    // frames 2 and 3: the console's stale reads (Honeyhive 1 start, round 1): a reader of the PREVIOUS picture's capture drawn
    // BEFORE the picture's own capture (an IndDummy material or particle in the lists before DrawType_CaptureScreenIndirect),
    // and no reader after it. Frame 2's reader is new (frame 1 left its capture out: one stale read under any rule). Frame 3's
    // reader reads frame 2's capture: the round-1 rule had left it out (a second stale read, a wrong picture); PREDICT keeps it.
    Floor(200, 40, 120);
    Reader(-1, -0.5f);
    MarkedCapture();
    const auto p2 = ReadAndFlush(s2);
    IndCapCounters(c);
    const uint64_t stale2 = c[5] - c0[5];
    Floor(10, 90, 220);
    Reader(-1, -0.5f);
    MarkedCapture();
    const auto p3 = ReadAndFlush(s3);
    IndCapCounters(c);
    const uint64_t stale3 = c[5] - c0[5] - stale2;
    // frame 4: readers before and after the capture
    Floor(90, 30, 30);
    Reader(-1, -0.5f);
    MarkedCapture();
    Reader(0.25f, 1);
    const auto p4 = ReadAndFlush(s4);
    IndCapCounters(c);
    const uint64_t half0 = c[4];
    // frames 5-7: captures with readers on three pictures in a row (GXV_INDCAP_HALFRATE: made, left out, made)
    Floor(120, 120, 20);
    MarkedCapture();
    Reader(0.25f, 1);
    const auto p5 = ReadAndFlush(s5);
    IndCapCounters(c);
    const uint64_t half5 = c[4] - half0;
    Floor(20, 120, 120);
    MarkedCapture();
    Reader(0.25f, 1);
    const auto p6 = ReadAndFlush(s6);
    IndCapCounters(c);
    const uint64_t half6 = c[4] - half0 - half5;
    if (std::getenv("TEST_CUT7")) ++g_testCuts; // (GXV_INDCAP_CAMGATE: the game's stage camera jumps before picture 7)
    Floor(160, 60, 200);
    MarkedCapture();
    Reader(0.25f, 1);
    const auto p7 = ReadAndFlush(s7);
    IndCapCounters(c);
    const uint64_t half7 = c[4] - half0 - half5 - half6;
    // frame 8: another stage (tag 2), a capture nothing reads: left out unless the predict window of the texture still runs
    unsigned s8 = 0;
    const uint64_t left7 = c[2];
    Floor(60, 60, 160);
    MarkedCapture(2);
    const auto p8 = ReadAndFlush(s8);
    IndCapCounters(c);
    const uint64_t left8 = c[2] - left7;
    if (const char* dump = std::getenv("INDCAP_DUMP")) { // (the script counts the pixels frames 2-4 differ by, run against run)
        const std::vector<uint8_t>* ps[3] = {&p2, &p3, &p4};
        for (int f = 0; f < 3; ++f) {
            char name[512];
            std::snprintf(name, sizeof name, "%s-f%d.rgba", dump, f + 2);
            if (FILE* out = std::fopen(name, "wb")) { std::fwrite(ps[f]->data(), 1, ps[f]->size(), out); std::fclose(out); }
        }
    }
    if (p1.empty() || p2.empty() || p3.empty() || p4.empty() || p5.empty() || p6.empty() || p7.empty()) return 1;
    std::printf("frame1 scenes=%u hash=%016llx\n", s1, static_cast<unsigned long long>(Hash(p1)));
    std::printf("frame3 scenes=%u hash=%016llx\n", s3, static_cast<unsigned long long>(Hash(p3)));
    std::printf("frame4 scenes=%u hash=%016llx\n", s4, static_cast<unsigned long long>(Hash(p4)));
    std::printf("frame5 scenes=%u hash=%016llx\n", s5, static_cast<unsigned long long>(Hash(p5)));
    std::printf("frame7 scenes=%u hash=%016llx\n", s7, static_cast<unsigned long long>(Hash(p7)));
    std::printf("frame8 scenes=%u hash=%016llx\n", s8, static_cast<unsigned long long>(Hash(p8)));
    std::printf("info frame2 scenes=%u stale=%llu hash=%016llx frame3 stale=%llu frame6 scenes=%u hash=%016llx (half rate) half5/6/7=%llu/%llu/%llu counters "
                "pictures=%llu marked=%llu left=%llu kept=%llu half=%llu stale=%llu held=%llu pre=%llu\n",
                s2, static_cast<unsigned long long>(stale2), static_cast<unsigned long long>(Hash(p2)), static_cast<unsigned long long>(stale3), s6,
                static_cast<unsigned long long>(Hash(p6)), static_cast<unsigned long long>(half5), static_cast<unsigned long long>(half6),
                static_cast<unsigned long long>(half7), static_cast<unsigned long long>(c[0] - c0[0]), static_cast<unsigned long long>(c[1] - c0[1]),
                static_cast<unsigned long long>(c[2] - c0[2]), static_cast<unsigned long long>(c[3] - c0[3]), static_cast<unsigned long long>(c[4] - c0[4]),
                static_cast<unsigned long long>(c[5] - c0[5]), static_cast<unsigned long long>(c[6] - c0[6]), static_cast<unsigned long long>(c[7] - c0[7]));
    const bool warmup = !std::getenv("GXV_INDCAP_WARMUP") || std::atol(std::getenv("GXV_INDCAP_WARMUP")) > 0;
    if (warmup) Check(c[2] - c0[2] == 0 && c[5] - c0[5] == 0, "GXV_INDCAP_WARMUP: nothing left out in the stage's first pictures, no stale read");
    if ((skip == 1 || skip == 2) && !warmup) {
        Check(stale2 == 1, "frame 2: the new reader of a left-out capture is counted (the first picture it appears in)");
        if (predict > 0 || stageOn) {
            Check(stale3 == 0, "frame 3: frame 2's capture was kept for its reader (GXV_INDCAP_PREDICT / _STAGE; no second stale read)");
            Check(c[6] - c0[6] >= 1 && c[7] - c0[7] >= 3, "the captures kept for earlier-picture readers are counted");
        } else {
            Check(stale3 == 1, "frame 3 (round-1 rule, GXV_INDCAP_PREDICT=0 GXV_INDCAP_STAGE=0): the second stale read reproduces the console's");
        }
        if (predict == 0 && stageOn)
            Check(left8 == (skip == 1 || skip == 2 ? 1u : 0u), "frame 8: a stage without early readers leaves its unread capture out again");
        if (predict > 0) Check(left8 == 0, "frame 8: still inside the texture's GXV_INDCAP_PREDICT window: kept");
    }
    if (skip == 0 && half == 0) Check(c[1] - c0[1] == 0, "off: no capture marked (the front end ignores the marker)");
    if (half == 1 && std::getenv("GXV_INDCAP_CAMGATE") && std::atol(std::getenv("GXV_INDCAP_CAMGATE")) && std::getenv("TEST_CUT7")) {
        Check(half5 == 1 && half6 == 0 && half7 == 0, "camera gate: the picture after a camera cut is made for real, not halved");
    } else
    if (half == 1) {
        Check(half5 == 1 && half6 == 0 && half7 == 1, "half rate: frame 4 made, then left out, made, left out");
        Check(Hash(p5) != 0 && Hash(p7) != 0, "frames 5 and 7 drawn");
    }
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
