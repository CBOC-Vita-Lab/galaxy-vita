// GXV_HSR_ORDER (platform/vgx/src/vgx_hsrorder.h, vgx_backend_gl.cpp "hsrx") through the production GX front end and GLES back end on Mesa, no game data.
// Synthetic pictures (64 x 64) made of opaque, alpha-tested (a texture's alpha, discard), blended and no-depth-write draws, coplanar decal pairs:
//   frame 1  mixed: opaque / alpha-tested draws at different depths with a blended draw and two more opaque ones around;
//   frame 2  ties: an alpha-tested draw, then an opaque draw at the very same depth over it (and the same transform): the pair the pin protects;
//   frame 3  a no-depth-write opaque draw among them (a barrier);
//   frame 4  decal pairs: wall, alpha-tested decal at the wall's depth, opaque on top at the same depth; a LESS opaque draw at the same depth;
//   frame 5  frame 1 with an EFB copy (a texture capture and its reader) in the middle (a command barrier);
//   frame 6  alpha-tested draws only (nothing to move).
// The script tools/test_vgx_hsrorder_gl.sh runs it with the switch off, on (1, ties 1 and 3), 3 (alternating) and 2 (census), and with GXV_HSR_ORDER_TIES=0, and
// GXV_DROP_CLASS / GXV_DROP_FS, and compares the printed hashes (frame lines) between runs.
#include "vgx_state.h"
#include "port/stage_knob.h"
#include "vgx_prepass.h"
#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <cstdarg>
extern "C" void port_log_summary(const char* fmt, ...) { // (the basic log: the census and dump lines go to the test's output)
    std::va_list ap;
    va_start(ap, fmt);
    std::printf("log: ");
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
alignas(65536) unsigned char destinations[6][65536];
unsigned char* const readDestination = destinations[0];
unsigned char* const capDestination = destinations[1];
unsigned char* const patDestination[3] = {destinations[2], destinations[3], destinations[4]};
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
enum { LESS = 1, LEQUAL = 3, GREATER = 4, ALWAYS = 7 };
void SetZ(unsigned func, bool write) { BP(0x40, 1 | (func << 1) | (write ? 1u << 4 : 0)); }
void Opaque(unsigned r, unsigned g_, unsigned b, unsigned func = LEQUAL, bool write = true) { // colour C0, no texture, no alpha test, no blend
    BP(0x00, 0);
    XF(0x103F, 0);
    BP(0x28, 7u << 7);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    SetZ(func, write);
    BP(0x41, (1u << 3) | (1u << 4));
    BP(0x42, 1u << 8);
    Color(r, g_, b, 255);
}
void BindPattern(int k) { // IA8 texture; alpha 0 or 255 per 4 x 4 block, three patterns
    auto& t = g.tex[0];
    t = TexSlot{};
    t.valid = true;
    t.obj.data = patDestination[k];
    t.obj.width = t.obj.height = 64;
    t.obj.format = 3;
    g.bp[0x88] = 63u | (63u << 10) | (3u << 20);
    g.bp[0x94] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(patDestination[k]) >> 5) & 0xFFFFFFu;
    g.bpTexStale = 0;
    g.texDirty = true;
    g.texSlotDirty = 1;
    XF(0x103F, 1);
    XF(0x1040, 5u << 7);
    BP(0x00, 1);
    BP(0x28, (1u << 6) | (7u << 7));
}
void Tested(int pattern, unsigned r, unsigned g_, unsigned b, unsigned func = LEQUAL, bool write = true) { // colour C0, alpha = the texture's, test alpha > 128
    BindPattern(pattern);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (4u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, 128u | (4u << 16) | (7u << 19));
    SetZ(func, write);
    BP(0x41, (1u << 3) | (1u << 4));
    BP(0x42, 1u << 8);
    Color(r, g_, b, 255);
}
void Blended(unsigned r, unsigned g_, unsigned b, unsigned a, unsigned func = LEQUAL) { // SRC_ALPHA / INV_SRC_ALPHA, depth test, no depth write
    BP(0x00, 0);
    XF(0x103F, 0);
    BP(0x28, 7u << 7);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    SetZ(func, false);
    BP(0x41, 1 | (1u << 3) | (1u << 4) | (4u << 8) | (5u << 5));
    BP(0x42, 1u << 8);
    Color(r, g_, b, a);
}
void Rect(float l, float r, float bt, float tp, float depth, bool tex) {
    const float z = depth - 1;
    const float v[] = {l,bt,z,0, 0,1, r,bt,z,0, 1,1, r,tp,z,0, 1,0, l,tp,z,0, 0,0};
    if (tex) DrawDecoded(0x80, VL_TEX0, reinterpret_cast<const uint8_t*>(v), 4);
    else {
        float plain[16];
        for (int i = 0; i < 4; ++i) std::memcpy(plain + i * 4, v + i * 6, 16);
        DrawDecoded(0x80, 0, reinterpret_cast<const uint8_t*>(plain), 4);
    }
}
// (every draw of a frame with `distinct` set gets its own position matrix, a shift of a thousandth: the draws are then not "the same transform" for the
// tie rule, and flat quads keep their depths; frames 2 and 4 use the one matrix on purpose)
bool distinct = false;
int xfCount = 0;
void SetXf() {
    const float shift = distinct ? 0.001f * static_cast<float>(++xfCount) : 0.0f;
    const uint32_t proj[] = {f2u(1),f2u(shift),f2u(1),0,f2u(1),0,1}; // (a different projection block: the matrices are not what the front end re-reads here)
    WriteXF(0x1020, 7, proj);
}
void Floor() { Opaque(40, 40, 60, ALWAYS, true); Rect(-1, 1, -1, 1, 0.95f, false); }
void O(float l, float r, float bt, float tp, float depth, unsigned cr, unsigned cg, unsigned cb, unsigned func = LEQUAL, bool write = true) { SetXf(); Opaque(cr, cg, cb, func, write); Rect(l, r, bt, tp, depth, false); }
void T(int pat, float l, float r, float bt, float tp, float depth, unsigned cr, unsigned cg, unsigned cb, unsigned func = LEQUAL) { SetXf(); Tested(pat, cr, cg, cb, func); Rect(l, r, bt, tp, depth, true); }
void B(float l, float r, float bt, float tp, float depth, unsigned cr, unsigned cg, unsigned cb, unsigned a) { SetXf(); Blended(cr, cg, cb, a); Rect(l, r, bt, tp, depth, false); }
void Copy(void* dest, unsigned format) {
    g.texCopySrc = EfbRect{0, 0, 64, 64};
    g.texCopyDstW = g.texCopyDstH = 64;
    g.texCopyFmt = format;
    QueueCopyTex(dest, false);
}
std::vector<uint8_t> ReadAndFlush(unsigned& scenes) {
    Copy(readDestination, 6);
    const auto id = Pending().copyTex.back().texId;
    FrontPresent(false, nullptr);
    scenes = backend::DebugLastFrameScenes();
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    if (!(backend::DebugReadTexture(id, pixels, w, h) && w == 64 && h == 64)) { Check(false, "picture read back"); pixels.clear(); }
    return pixels;
}
uint64_t Hash(const std::vector<uint8_t>& p) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < p.size(); ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
void FillPatterns() { // alpha 0 / 255 by 4 x 4 block; IA8: 32 bytes a block, both bytes the same
    for (int k = 0; k < 3; ++k)
        for (int ty = 0; ty < 16; ++ty)
            for (int tx = 0; tx < 16; ++tx) {
                const bool on = k == 0 ? ((tx + ty) & 1) == 0 : k == 1 ? (tx & 1) == 0 : (ty % 3) != 0;
                std::memset(patDestination[k] + (ty * 16 + tx) * 32, on ? 255 : 0, 32);
            }
}
std::vector<uint8_t> Frame(int f, unsigned& scenes) {
    distinct = f != 2 && f != 4 && f != 7; // (frame 7: the one matrix, as the tie frames)
    xfCount = 0;
    SetXf();
    Floor();
    switch (f) {
    case 1:
    case 5:
        O(-1, 0.2f, -1, 0.6f, 0.7f, 250, 20, 20);
        T(0, -0.5f, 1, -1, 1, 0.5f, 20, 250, 20);
        O(-0.2f, 1, -0.3f, 1, 0.6f, 20, 20, 250);
        T(1, -1, 0.4f, -1, 0.4f, 0.3f, 250, 250, 20);
        if (f == 5) { Copy(capDestination, 6); }
        O(-0.6f, 0.6f, -0.5f, 0.5f, 0.45f, 230, 230, 230);
        B(-1, 1, -0.2f, 0.2f, 0.2f, 250, 20, 250, 128);
        O(-1, -0.2f, -1, 1, 0.8f, 120, 60, 200);
        T(2, -1, 1, -1, 1, 0.35f, 20, 250, 250);
        break;
    case 2: // the same depth: the alpha-tested draw first, then an opaque one over it
        T(0, -1, 0.5f, -1, 1, 0.5f, 20, 250, 20);
        O(-0.5f, 1, -1, 1, 0.5f, 250, 20, 20);
        T(1, -1, 1, -0.5f, 0.5f, 0.4f, 250, 250, 20);
        O(-1, 1, -0.2f, 0.9f, 0.4f, 20, 20, 250);
        break;
    case 3:
        O(-1, 0.3f, -1, 1, 0.6f, 250, 20, 20);
        T(1, -0.4f, 1, -1, 1, 0.5f, 20, 250, 20);
        O(-1, 1, -0.2f, 0.2f, 0.4f, 20, 20, 250, LEQUAL, false); // no depth write
        O(-0.3f, 0.9f, -1, 0.5f, 0.55f, 250, 250, 20);
        T(2, -1, 1, -1, 1, 0.45f, 20, 250, 250);
        O(0.1f, 1, 0, 1, 0.3f, 230, 230, 230);
        break;
    case 4: // decals
        O(-1, 1, -1, 1, 0.5f, 90, 90, 90);
        T(2, -0.8f, 0.8f, -0.8f, 0.8f, 0.5f, 250, 250, 20);
        O(-0.4f, 0.4f, -0.4f, 0.4f, 0.5f, 250, 20, 20);
        T(0, -0.2f, 0.9f, -0.2f, 0.9f, 0.5f, 20, 250, 20);
        O(-0.6f, 0.2f, -0.6f, 0.2f, 0.5f, 20, 20, 250, LESS);
        break;
    case 7: // (dread25, GXV_ALPHATEST_PREPASS) a LESS tested draw at the depth of a wall and of an opaque draw before and after it: not split (a tie fails the prepass, passes EQUAL)
        O(-1, 1, -1, 1, 0.5f, 90, 90, 90);
        T(0, -0.9f, 0.9f, -0.9f, 0.9f, 0.5f, 250, 250, 20, LESS);
        O(-0.3f, 0.5f, -0.3f, 0.5f, 0.5f, 20, 20, 250);
        T(1, -0.6f, 0.9f, -0.6f, 0.9f, 0.4f, 20, 250, 20);
        O(0.2f, 0.9f, -0.9f, 0.3f, 0.4f, 250, 20, 20);
        break;
    case 6:
        T(0, -1, 1, -1, 1, 0.6f, 250, 20, 20);
        T(1, -1, 1, -1, 1, 0.5f, 20, 250, 20);
        T(2, -1, 1, -1, 1, 0.4f, 20, 20, 250);
        break;
    }
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
    FillPatterns();
    const int rounds = 2; // (two rounds of the six frames: GXV_HSR_ORDER=3 alternates by picture)
    // (dread25) HSR_TEST_STAGEKNOB=<order>[:<ties>]: the mode comes from a stage preset (port::stageknob Set + Commit) instead of the environment, before the first picture;
    // HSR_TEST_STAGEKNOB_LATE=1 sets it after round 0 instead (the back end reads it again at the next picture: round 0 in the original order, round 1 reordered).
    const char* sk = std::getenv("HSR_TEST_STAGEKNOB");
    const bool late = std::getenv("HSR_TEST_STAGEKNOB_LATE") != nullptr;
    auto applyKnob = [&] {
        if (!sk) return;
        port::stageknob::ClearAll();
        port::stageknob::Set("GXV_HSR_ORDER", static_cast<float>(std::atoi(sk)));
        if (const char* c = std::strchr(sk, ':')) port::stageknob::Set("GXV_HSR_ORDER_TIES", static_cast<float>(std::atoi(c + 1)));
        port::stageknob::Commit();
    };
    if (!late) applyKnob();
    for (int r = 0; r < rounds; ++r) {
        if (late && r == 1) applyKnob();
        for (int f = 1; f <= 7; ++f) {
            unsigned scenes = 0;
            const std::vector<uint8_t> px = Frame(f, scenes);
            if (px.empty()) return 1;
            std::printf("frame%d.%d scenes=%u hash=%016llx\n", f, r, scenes, static_cast<unsigned long long>(Hash(px)));
        }
    }
    uint64_t c[4];
    backend::DebugHsr(c);
    std::printf("info hsr planned=%llu reordered=%llu tmoved=%llu dropped=%llu\n", (unsigned long long)c[0], (unsigned long long)c[1], (unsigned long long)c[2], (unsigned long long)c[3]);
    {
        uint64_t a[5];
        backend::DebugAtpp(a);
        std::printf("info atpp pairs=%llu hoisted=%llu fallback=%llu planned=%llu pics=%llu\n", (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2], (unsigned long long)a[3], (unsigned long long)a[4]);
    }
    {
        uint64_t f[9];
        PrepassCounters(f);
        std::printf("info atppfe keys=%llu draws=%llu refused=%llu left: state=%llu fold=%llu flags=%llu id=%llu key=%llu\n", (unsigned long long)f[1], (unsigned long long)f[0], (unsigned long long)f[2],
                    (unsigned long long)f[3], (unsigned long long)f[4], (unsigned long long)f[6], (unsigned long long)f[7], (unsigned long long)f[8]);
    }
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
