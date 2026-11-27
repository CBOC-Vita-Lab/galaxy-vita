// GXV_INDCAP_PEEK (peek25): a TWO-FRAME QUEUE through the production front end and GLES back end on Mesa. Every picture is built and queued first (the front
// end's buffers are kept, not executed); then each is executed with the next one visible, as RenderMain does, with vgx::indcappeek::Decide in between.
// tools/test_vgx_indcap_peek.sh runs it with GXV_INDCAP_PEEK=0 / 2 / 1 and compares every picture. (Helpers copied from vgx_indcap_gl.cpp.)
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
#include "vgx_indcap_peek.h"
#include <deque>
#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
std::deque<CmdBuf> g_queue;
void SubmitToBackend(CmdBuf& b) { g_queue.emplace_back(); std::swap(g_queue.back(), b); }
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

uint32_t BuildReadback() { // the picture, copied into the read-back texture; queued
    Copy(readDestination, 6);
    const uint32_t id = Pending().copyTex.back().texId;
    FrontPresent(false, nullptr);
    return id;
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
    std::vector<uint32_t> ids;
    auto cap = [&] { MarkedCapture(); };
    // 1: a capture nothing reads; 2 overwrites it first                                           -> 1 dead via next
    Floor(40, 200, 90); cap(); Pass(); Color(250, 250, 10, 255); Quad(-0.25f, 0.25f, 0.4f); ids.push_back(BuildReadback());
    // 2: capture, no reader here; 3 reads the previous capture BEFORE its own                      -> 2 kept (next reads first)
    Floor(200, 40, 120); cap(); ids.push_back(BuildReadback());
    // 3: reads 2's capture, then captures (nothing reads it here); 4 overwrites first               -> 3 dead via next; the picture shows 2's capture
    Floor(10, 90, 220); Reader(-1, -0.5f); cap(); ids.push_back(BuildReadback());
    // 4: capture with a reader after it                                                            -> 4 kept (reader here)
    Floor(90, 30, 30); cap(); Reader(0.25f, 1); ids.push_back(BuildReadback());
    // 5: capture, no reader; 6 reads it without capturing                                          -> 5 kept (next reads first)
    Floor(120, 120, 20); cap(); ids.push_back(BuildReadback());
    Floor(20, 120, 120); Reader(-1, -0.5f); ids.push_back(BuildReadback());
    // 7: capture, no reader; 8 neither reads nor writes it; 9 reads it                              -> 7 kept (next has no whole overwrite): the induction case
    Floor(160, 60, 200); cap(); ids.push_back(BuildReadback());
    Floor(60, 160, 60); ids.push_back(BuildReadback());
    Floor(30, 30, 160); Reader(-1, -0.5f); ids.push_back(BuildReadback());
    // 10, 11, 12: three captures in a row, nothing reads them; 13 is the last queued, 14 reads 13's  -> 10, 11, 12 dead via next (a chain), 13 kept
    Floor(200, 200, 40); cap(); ids.push_back(BuildReadback());
    Floor(40, 200, 200); cap(); ids.push_back(BuildReadback());
    Floor(200, 40, 200); cap(); ids.push_back(BuildReadback());
    Floor(100, 160, 30); cap(); ids.push_back(BuildReadback());
    Floor(30, 100, 160); Reader(-1, -0.5f); ids.push_back(BuildReadback());
    // 15 (the last buffer): a capture and nothing after it: no next buffer                         -> kept (next not queued)
    Floor(150, 150, 150); cap(); ids.push_back(BuildReadback());
    Check(g_queue.size() == ids.size(), "every picture queued");
    const int mode = vgx::indcappeek::Mode();
    const bool bad = std::getenv("TEST_FORCE_BAD") != nullptr; // (a negative control: capture 2 is left out although 3 reads it)
    namespace ip = vgx::indcappeek;
    unsigned left = 0;
    for (size_t i = 0; i < g_queue.size(); ++i) {
        CmdBuf& b = g_queue[i];
        const CmdBuf* next = i + 1 < g_queue.size() ? &g_queue[i + 1] : nullptr;
        ip::Verify(b, ip::g_shadow, ip::g_win);
        if (mode) left += ip::Decide(b, next, mode == 1, ip::g_win, ip::g_shadow);
        if (bad && i == 1) for (CopyTexCmd& c : b.copyTex) if (c.transient) c.skip = true;
        backend::Execute(b);
        const unsigned scenes = backend::DebugLastFrameScenes();
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        Check(backend::DebugReadTexture(ids[i], px, w, h) && w == 64 && h == 64, "picture read back");
        std::printf("pic%zu scenes=%u hash=%016llx\n", i + 1, scenes, static_cast<unsigned long long>(Hash(px)));
        if (!mode) continue;
    }
    const ip::Counters& w = ip::g_win;
    std::printf("info peek mode=%d pictures=%llu queued=%llu captures=%llu readerHere=%llu deadOwn=%llu viaNext=%llu noNext=%llu nextReads=%llu nextNoWrite=%llu shadow=%llu bad=%llu left=%u\n",
                mode, (unsigned long long)w.pictures, (unsigned long long)w.nextQueued, (unsigned long long)w.captures, (unsigned long long)w.readerHere,
                (unsigned long long)w.deadOwn, (unsigned long long)w.viaNext, (unsigned long long)w.noNext, (unsigned long long)w.nextReads,
                (unsigned long long)w.nextNoWrite, (unsigned long long)w.shadowChecks, (unsigned long long)w.shadowBad, left);
    if (mode) {
        Check(w.pictures == ids.size(), "every picture looked at");
        Check(w.captures == 11 && w.readerHere == 1 && w.viaNext == 5 && w.nextReads == 3 && w.nextNoWrite == 1 && w.noNext == 1, "the 11 captures: 1 reader here, 5 dead via the next buffer, 3 next reads first, 1 next without overwrite (7), 1 no next");
        Check(w.shadowChecks == 5 && w.shadowBad == 0, "the shadow check: every left-out capture's next buffer wrote the texture first");
        char line[900];
        ip::Counters tot;
        Check(ip::Line(line, sizeof line, ip::g_win, tot, 1) && std::strstr(line, "indcap-peek:") != nullptr, "the summary line");
        std::printf("%s\n", line);
    }
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
