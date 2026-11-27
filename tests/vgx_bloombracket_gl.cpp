// GXV_BLOOM_BRACKET (bloom37; vgx_draw.cpp BloomBracketPass): the normal bloom's scene bracket through the production GX front end and GLES back end on Mesa (no game data).
// tools/test_vgx_bloombracket_gl.sh runs it with the switch off, on and =2 and compares the pictures byte for byte (colour AND alpha).
// A bracket as BloomEffect writes it (PORT_GX_MARK_BLOOM_BRACKET, BP 0xFF kind 40): CAPTURE marker, the scene's RGBA8 whole copy, FILL_BEGIN, the black fill (alpha 255), FILL_END, glow draws
// (if any), [DEAD marker], PUT_BEGIN, the put-back (a covering quad sampling the capture, colour and TEXTURE alpha), PUT_END. Each picture ends with a read-back copy.
//   frame 1: dead bracket, floor with alpha 100: with =1 the three are left out (fewer scenes), the picture (colour and alpha) is the same.
//   frame 2: a glow quad drawn on the black, no DEAD marker: untouched.
//   frame 3: the DEAD marker but a stray draw between the fill and the put-back (the game's own check failed to hold): refused, "foreign command".
//   frame 4: dead, but the fill writes alpha and the put-back does not (colour mask differs): refused, "planes".
//   frame 5: dead, but another draw of the picture samples the capture's texture: refused, "shared reader".
//   frame 6: a dead bracket again after the refusals: dropped (the pass has no memory).
//   frame 7: dead, the capture is the 565 format (not RGBA8): refused, "copy shape".
#include "vgx_state.h"
#include "vgx_bloombracket.h"
#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <vector>

// the basic log: the back end calls it without a test (the console has it; here it prints what it is told)
extern "C" void port_log_summary(const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    std::printf("LOG ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
}
namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}
namespace {
using namespace vgx;
alignas(32) unsigned char capDest[32];
alignas(32) unsigned char readDest[32];
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
void Mark(unsigned sub) { BP(0xFF, 40u | (sub << 8)); }
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
// the put-back's TEV: colour = TEXC, alpha = TEXA (BloomEffect: GX_MODULATE with intensity 255, matching colour 255)
void PutState(bool alphaWrite = true) {
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19));
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (4u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, 0); // (GXSetZMode(FALSE, ...): no depth test, no depth write)
    BP(0x41, (1u << 3) | (alphaWrite ? (1u << 4) : 0u));
    BP(0x42, 0);
}
void Floor(unsigned r, unsigned gr, unsigned b, unsigned alpha) {
    Pass();
    BP(0x42, 0); // (the alpha is the TEV's, not a constant destination alpha)
    Color(r, gr, b, alpha);
    Quad(-1, 0, 0.5f);
    Color(b, r, gr, alpha);
    Quad(0, 1, 0.5f);
}
struct Opts { bool dead = true, glow = false, stray = false, putNoAlpha = false, sharedReader = false; unsigned capFormat = 6; };
void Bracket(const Opts& o) {
    Mark(1);                       // PORT_BB_CAPTURE
    Copy(capDest, o.capFormat);    // the scene capture
    Mark(2);                       // FILL_BEGIN
    Pass();
    BP(0x40, 0);                   // (fillScreenSetup: GXSetZMode(FALSE, GX_LEQUAL, FALSE))
    BP(0x42, 0);
    Color(0, 0, 0, 255);
    Quad(-1, 1, 0.5f);
    Mark(3);                       // FILL_END
    if (o.glow) { Pass(); Color(250, 240, 10, 255); Quad(-0.25f, 0.25f, 0.4f); }
    if (o.stray) { Pass(); Color(10, 10, 10, 255); Quad(-0.5f, -0.4f, 0.4f); }
    if (o.dead) Mark(6);           // PORT_BB_DEAD
    Mark(4);                       // PUT_BEGIN
    Bind(capDest, 6);
    PutState(!o.putNoAlpha);
    Quad(-1, 1, 0.5f, true);
    Mark(5);                       // PUT_END
    if (o.sharedReader) { Quad(0.5f, 0.75f, 0.45f, true); }
}
std::vector<uint8_t> ReadAndFlush(unsigned& scenes) {
    Copy(readDest, 6);
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
    for (size_t i = 0; i < p.size(); ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
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
    for (unsigned t = 0; t < 4; ++t) { BP(0xF6 + 2 * t, 0u | (1u << 2)); BP(0xF7 + 2 * t, 2u | (3u << 2)); } // (the game's swap tables: identity)
    const long mode = std::getenv("GXV_BLOOM_BRACKET") ? std::atol(std::getenv("GXV_BLOOM_BRACKET")) : 0;
    uint64_t c0[15], c[15];
    BloomBracketCounters(c0);
    unsigned s[8] = {};
    std::vector<uint8_t> p[8];
    Floor(40, 200, 90, 100);   Bracket({});                                    p[1] = ReadAndFlush(s[1]);
    Floor(200, 40, 120, 100);  { Opts o; o.dead = false; o.glow = true; Bracket(o); } p[2] = ReadAndFlush(s[2]);
    Floor(10, 90, 220, 100);   { Opts o; o.stray = true; Bracket(o); }         p[3] = ReadAndFlush(s[3]);
    Floor(90, 30, 30, 100);    { Opts o; o.putNoAlpha = true; Bracket(o); }    p[4] = ReadAndFlush(s[4]);
    Floor(120, 120, 20, 100);  { Opts o; o.sharedReader = true; Bracket(o); }  p[5] = ReadAndFlush(s[5]);
    Floor(20, 120, 120, 100);  Bracket({});                                    p[6] = ReadAndFlush(s[6]);
    Floor(160, 60, 200, 100);  { Opts o; o.capFormat = 4; Bracket(o); }        p[7] = ReadAndFlush(s[7]);
    BloomBracketCounters(c);
    if (const char* dump = std::getenv("BB_DUMP")) { // (a failing comparison: the pictures as RGBA bytes, one file a frame)
        for (int f = 1; f <= 7; ++f) {
            char name[512];
            std::snprintf(name, sizeof name, "%s-f%d.rgba", dump, f);
            if (FILE* o = std::fopen(name, "wb")) { std::fwrite(p[f].data(), 1, p[f].size(), o); std::fclose(o); }
        }
    }
    for (int f = 1; f <= 7; ++f) {
        if (p[f].empty()) return 1;
        std::printf("frame%d scenes=%u hash=%016llx\n", f, s[f], static_cast<unsigned long long>(Hash(p[f])));
    }
    const auto d = [&](int i) { return c[i] - c0[i]; };
    std::printf("info counters: pictures=%llu brackets=%llu dead=%llu dropped=%llu markers=%llu foreign=%llu copy-shape=%llu draw-shape=%llu planes=%llu shared=%llu hrw=%llu msaa=%llu host=%llu\n",
                (unsigned long long)d(0), (unsigned long long)d(1), (unsigned long long)d(2), (unsigned long long)d(3), (unsigned long long)d(4 + bb::kMarkers), (unsigned long long)d(4 + bb::kForeign),
                (unsigned long long)d(4 + bb::kCopyShape), (unsigned long long)d(4 + bb::kDrawShape), (unsigned long long)d(4 + bb::kPlanes), (unsigned long long)d(4 + bb::kSharedRead),
                (unsigned long long)d(4 + bb::kHrw), (unsigned long long)d(4 + bb::kMsaa), (unsigned long long)d(4 + bb::kHost));
    const bool hrw = std::getenv("GXV_HRW") && std::atol(std::getenv("GXV_HRW")) != 0;
    if (mode == 0 || hrw) {
        Check(d(1) == 0 && d(0) == 0, "off or GXV_HRW != 0: no bracket seen (the front end ignores the markers)");
    } else {
        Check(d(0) == 7 && d(1) == 7, "7 pictures, 7 brackets");
        Check(d(2) == 6, "6 of them said dead");
        Check(d(3) == 2, "dropped (or would be, =2): frames 1 and 6");
        Check(d(4 + bb::kForeign) == 1, "frame 3: refused, foreign command between");
        Check(d(4 + bb::kPlanes) == 1, "frame 4: refused, the put-back does not write the alpha the fill wrote");
        Check(d(4 + bb::kSharedRead) == 1, "frame 5: refused, another draw samples the capture");
        Check(d(4 + bb::kCopyShape) == 1, "frame 7: refused, the capture is not RGBA8");
        Check(d(4 + bb::kMarkers) == 0 && d(4 + bb::kDrawShape) == 0, "no marker or shape refusal");
    }
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
