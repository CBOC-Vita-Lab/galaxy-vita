// GXV_HSR_ORDER_TIES=4 (ties25) through the production GX front end, the display-list cache (static buffers) and the GLES back end on Mesa, no game data.
// 64 x 64 pictures of STATIC quads (display lists called every picture: the cache promotes them to static buffers after a few uses) with one transform:
//   frame 1  separated: alpha-tested and opaque quads at depths 0.05 .. 0.2 apart (the proofs free them: the reorder happens);
//   frame 2  crossing: an alpha-tested quad tilted one way and an opaque one tilted the other, equal depth along one row of pixel centres (a line of ties);
//   frame 3  coplanar decals: wall, alpha-tested decal and opaque decal at the wall's depth (ties over areas);
//   frame 4  parallel offset of 1e-9 (below the depth buffer's step: a tie after quantisation) between an alpha-tested and an opaque quad;
//   frame 5  parallel offset of 0.01 (above the resolution: freed) and 3e-6 (below thr: pinned) in one picture;
//   frame 6  a seam: an alpha-tested quad and an opaque quad sharing an edge at an angle (touching: pinned);
//   frame 7  static alpha-tested quads with a stream (immediate) opaque draw after them (dynamic: pinned) and static ones after that.
// tools/test_vgx_hsrgeo_gl.sh runs it with the switches off, GXV_HSR_ORDER=1 with TIES 1, 4 and 0, the census, and the proofs' margins relaxed (negative controls).
#include "vgx_state.h"
#include "port/stage_knob.h"
#include "vgx_prepass.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"
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
unsigned g_staticDraws = 0, g_streamDraws = 0;
void SubmitToBackend(CmdBuf& b) {
    for (const DrawRec& d : b.draws) (d.vbuf ? g_staticDraws : g_streamDraws)++;
    backend::Execute(b);
}
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
void Opaque(unsigned r, unsigned g_, unsigned b, unsigned func = LEQUAL, bool write = true) {
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
void BindPattern(int k) {
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
void Tested(int pattern, unsigned r, unsigned g_, unsigned b, unsigned func = LEQUAL, bool write = true) {
    BindPattern(pattern);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (4u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, 128u | (4u << 16) | (7u << 19));
    SetZ(func, write);
    BP(0x41, (1u << 3) | (1u << 4));
    BP(0x42, 1u << 8);
    Color(r, g_, b, 255);
}
// the immediate quad (a stream draw), as tests/vgx_hsrorder_gl.cpp
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
void SetXf() {
    const uint32_t proj[] = {f2u(1),0,f2u(1),0,f2u(1),0,1};
    WriteXF(0x1020, 7, proj);
}
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
void FillPatterns() {
    for (int k = 0; k < 3; ++k)
        for (int ty = 0; ty < 16; ++ty)
            for (int tx = 0; tx < 16; ++tx) {
                const bool on = k == 0 ? ((tx + ty) & 1) == 0 : k == 1 ? (tx & 1) == 0 : (ty % 3) != 0;
                std::memset(patDestination[k] + (ty * 16 + tx) * 32, on ? 255 : 0, 32);
            }
}
// ---- static quads: a display list of one quad (positions and texture coordinates by 16-bit index into two arrays in guest RAM); the cache makes a static buffer of
// a list that is called again (a few pictures in a row)
alignas(4096) uint8_t ram[1 << 18];
uint32_t posOff = 4096, texOff = 65536, listOff = 131072, nVerts = 0, nLists = 0;
void Put32(uint8_t* p, uint32_t v) { for (unsigned i = 0; i != 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i)); }
struct Quad { float x[4], y[4], z[4]; }; // corners BL, BR, TR, TL; z is the window depth (the immediate Rect's `depth`)
struct QuadList { uint32_t off = 0, bytes = 0; };
std::vector<QuadList> lists;
int NewQuad(const Quad& q) { // returns the list's id
    const uint32_t base = nVerts;
    static const float s[4] = {0, 1, 1, 0}, t[4] = {1, 1, 0, 0};
    for (int i = 0; i < 4; ++i) {
        Put32(ram + posOff + (base + i) * 12, f2u(q.x[i]));
        Put32(ram + posOff + (base + i) * 12 + 4, f2u(q.y[i]));
        Put32(ram + posOff + (base + i) * 12 + 8, f2u(q.z[i] - 1.0f));
        Put32(ram + texOff + (base + i) * 8, f2u(s[i]));
        Put32(ram + texOff + (base + i) * 8 + 4, f2u(t[i]));
    }
    nVerts += 4;
    QuadList l;
    l.off = listOff + nLists * 64;
    uint8_t* p = ram + l.off;
    *p++ = 0x80; *p++ = 0; *p++ = 4;
    for (int i = 0; i < 4; ++i) { const uint32_t v = base + i; *p++ = static_cast<uint8_t>(v >> 8); *p++ = static_cast<uint8_t>(v); *p++ = static_cast<uint8_t>(v >> 8); *p++ = static_cast<uint8_t>(v); }
    l.bytes = static_cast<uint32_t>(p - (ram + l.off));
    ++nLists;
    lists.push_back(l);
    return static_cast<int>(lists.size() - 1);
}
Quad Flat(float l, float r, float bt, float tp, float depth) { return Quad{{l, r, r, l}, {bt, bt, tp, tp}, {depth, depth, depth, depth}}; }
// a quad whose depth depends on y only: depth(y) = d0 + slope * (y - y0)
Quad Tilt(float l, float r, float bt, float tp, float d0, float slope, float y0) { return Quad{{l, r, r, l}, {bt, bt, tp, tp}, {d0 + slope * (bt - y0), d0 + slope * (bt - y0), d0 + slope * (tp - y0), d0 + slope * (tp - y0)}}; }
struct Pic { std::vector<std::pair<int, int>> order; }; // unused
void CallQuad(int id) { SetXf(); vgx::CallList(ram + lists[static_cast<size_t>(id)].off, lists[static_cast<size_t>(id)].bytes); }
struct FrameQuads { std::vector<int> ids; };
FrameQuads fq[8];
void Floor() { SetXf(); Opaque(40, 40, 60, ALWAYS, true); Rect(-1, 1, -1, 1, 0.95f, false); }
void O(int id, unsigned cr, unsigned cg, unsigned cb, unsigned func = LEQUAL) { SetXf(); Opaque(cr, cg, cb, func, true); CallQuad(id); }
void T(int id, int pat, unsigned cr, unsigned cg, unsigned cb, unsigned func = LEQUAL) { SetXf(); Tested(pat, cr, cg, cb, func); CallQuad(id); }
int Q[64];
void MakeQuads() {
    int n = 0;
    // frame 1: separated
    Q[n++] = NewQuad(Flat(-1, 0.4f, -1, 0.5f, 0.70f));    // 0  T
    Q[n++] = NewQuad(Flat(-0.5f, 1, -0.6f, 1, 0.60f));    // 1  O
    Q[n++] = NewQuad(Flat(-1, 1, -0.2f, 0.4f, 0.35f));    // 2  T
    Q[n++] = NewQuad(Flat(-0.8f, 0.3f, -1, 1, 0.50f));    // 3  O
    Q[n++] = NewQuad(Flat(-1, 1, -1, 0.9f, 0.40f));       // 4  T
    Q[n++] = NewQuad(Flat(0.2f, 1, -0.4f, 0.6f, 0.30f));  // 5  O
    // frame 2: crossing along the pixel-centre row y0 = 1/64 (dyadic numbers: exact depths); slopes +-0.25
    Q[n++] = NewQuad(Tilt(-1, 1, -1, 1, 0.5f, 0.25f, 1.0f / 64));   // 6  T (whole picture: 64 x 64 pixels, all plane coefficients exact)
    Q[n++] = NewQuad(Tilt(-1, 1, -1, 1, 0.5f, -0.25f, 1.0f / 64));  // 7  O
    // frame 3: coplanar decals
    Q[n++] = NewQuad(Flat(-1, 1, -1, 1, 0.5f));           // 8  wall O
    Q[n++] = NewQuad(Flat(-0.8f, 0.8f, -0.8f, 0.8f, 0.5f));     // 9  decal T
    Q[n++] = NewQuad(Flat(-0.4f, 0.4f, -0.4f, 0.4f, 0.5f));     // 10 decal O
    // frame 4: offset 1e-9
    Q[n++] = NewQuad(Flat(-0.9f, 0.9f, -0.9f, 0.9f, 0.5f));                  // 11 T
    Q[n++] = NewQuad(Flat(-0.5f, 1, -0.5f, 1, 0.5f + 1e-9f));                // 12 O
    // frame 5: offset 0.01 and 3e-6
    Q[n++] = NewQuad(Flat(-0.9f, 0.2f, -0.9f, 0.9f, 0.45f));                 // 13 T
    Q[n++] = NewQuad(Flat(-0.3f, 0.9f, -0.5f, 0.5f, 0.46f));                 // 14 O (0.01 behind)
    Q[n++] = NewQuad(Flat(-0.1f, 0.9f, 0.3f, 0.95f, 0.30f));                 // 15 T
    Q[n++] = NewQuad(Flat(0.2f, 0.95f, 0.1f, 0.8f, 0.30f + 3e-6f));          // 16 O (3e-6 behind: below the margin)
    // frame 6: a seam: T flat over y in [-1, 0], O rising from the seam
    Q[n++] = NewQuad(Flat(-1, 1, -1, 0, 0.5f));                              // 17 T
    Q[n++] = NewQuad(Tilt(-1, 1, 0, 1, 0.5f, -0.3f, 0));                     // 18 O
    // frame 7: static T quads around a stream opaque draw
    Q[n++] = NewQuad(Flat(-1, 0.5f, -1, 0.6f, 0.6f));                        // 19 T
    Q[n++] = NewQuad(Flat(-0.4f, 1, -0.3f, 1, 0.55f));                       // 20 T
    Q[n++] = NewQuad(Flat(-1, 1, -0.2f, 0.3f, 0.2f));                        // 21 O static
    fq[1].ids = {Q[0], Q[1], Q[2], Q[3], Q[4], Q[5]};
}
std::vector<uint8_t> Frame(int f, unsigned& scenes) {
    SetXf();
    Floor();
    switch (f) {
    case 1:
        T(Q[0], 0, 20, 250, 20); O(Q[1], 250, 20, 20); T(Q[2], 1, 250, 250, 20); O(Q[3], 20, 20, 250); T(Q[4], 2, 20, 250, 250); O(Q[5], 230, 230, 230);
        break;
    case 2:
        T(Q[6], 0, 20, 250, 20); O(Q[7], 250, 20, 20);
        break;
    case 3:
        O(Q[8], 90, 90, 90); T(Q[9], 2, 250, 250, 20); O(Q[10], 250, 20, 20);
        break;
    case 4:
        T(Q[11], 0, 20, 250, 20); O(Q[12], 250, 20, 20);
        break;
    case 5:
        T(Q[13], 0, 20, 250, 20); O(Q[14], 250, 20, 20); T(Q[15], 1, 250, 250, 20); O(Q[16], 20, 20, 250);
        break;
    case 6:
        T(Q[17], 2, 20, 250, 20); O(Q[18], 250, 20, 20);
        break;
    case 7:
        T(Q[19], 0, 20, 250, 20); T(Q[20], 1, 250, 250, 20);
        SetXf(); Opaque(230, 230, 230); Rect(-0.5f, 0.9f, -0.9f, 0.2f, 0.58f, false); // (a stream draw)
        O(Q[21], 20, 20, 250);
        break;
    }
    return ReadAndFlush(scenes);
}
}
int main() {
    setenv("GXV_ATLAS", "0", 1);
    setenv("GXV_DL_TINY", "0", 1);
    setenv("GXV_SOFTXFORM", "3", 1); // (a list of 4 vertices is a static buffer candidate: the cache keeps lists over this many vertices out of the arena)
    if (!std::getenv("GXV_DLCACHE")) setenv("GXV_DLCACHE", "1", 1);
    setenv("GXV_STATICBUF", "1", 1);
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
    SetXf();
    Viewport();
    FillPatterns();
    MakeQuads();
    // the vertex description of the lists: positions and texture coordinates by 16-bit index, floats; no matrix index (a rigid shape)
    g.cpArrayStale = 0;
    SetVcd(0, 0);
    SetVcd(9, 3);
    SetVcd(13, 3);
    SetVat(0, 9, 1, 4, 0);
    SetVat(0, 13, 1, 4, 0);
    g.arrays[9] = {ram + posOff, nVerts * 12, 12, false};
    g.arrays[13] = {ram + texOff, nVerts * 8, 8, false};
    hle::RegisterGuestRam(ram, sizeof ram);
    const int rounds = std::getenv("HSRGEO_ROUNDS") ? std::atoi(std::getenv("HSRGEO_ROUNDS")) : 8; // (the lists are promoted to static buffers after a few pictures)
    for (int r = 0; r < rounds; ++r) {
        g_staticDraws = g_streamDraws = 0;
        for (int f = 1; f <= 7; ++f) {
            unsigned scenes = 0;
            const std::vector<uint8_t> px = Frame(f, scenes);
            if (px.empty()) return 1;
            std::printf("frame%d.%d scenes=%u hash=%016llx\n", f, r, scenes, static_cast<unsigned long long>(Hash(px)));
        }
        std::printf("round %d: static draws %u stream draws %u\n", r, g_staticDraws, g_streamDraws);
    }
    uint64_t c[4];
    backend::DebugHsr(c);
    std::printf("info hsr planned=%llu reordered=%llu tmoved=%llu dropped=%llu\n", (unsigned long long)c[0], (unsigned long long)c[1], (unsigned long long)c[2], (unsigned long long)c[3]);
    uint64_t gq[8];
    backend::DebugHsrGeo(gq);
    std::printf("info geo ready=%llu bad=%llu pairs=%llu big=%llu queued=%llu pending=%llu work=%llu keep=%llu\n", (unsigned long long)gq[0], (unsigned long long)gq[1], (unsigned long long)gq[2],
                (unsigned long long)gq[3], (unsigned long long)gq[4], (unsigned long long)gq[5], (unsigned long long)gq[6], (unsigned long long)gq[7]);
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
