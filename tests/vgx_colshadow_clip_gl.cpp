// GXV_COLSHADOW_CLIP (dreadblend25; platform/include/port/colshadow_clip.h) through the production GX front end and GLES back end on Mesa, no game data.
// Mario's drop shadow (CollisionShadow::draw3) shades each ground triangle in two passes through a clamped, linearly filtered shadow texture whose alpha is 0 outside the
// silhouette and on the outermost texel ring, with the alpha test "alpha != 0" and no depth test: pass 1 colour update off (the alpha is written 0 through the
// destination-alpha constant), pass 2 colour on. The clip cuts every triangle to the texture's own uv extent (+ 1 texel) and drops the ones wholly outside.
// The picture (RGB and the stored ALPHA) must be bit-identical to the whole-triangle draws: pictures are compared by hash and PSNR. Frames (64 x 64 target, a 64 x 64
// IA8 texture standing in for the 160 x 160 one):
//   1 one huge triangle covering the screen (the deck plate), the texture covering the middle quarter of it;
//   2 a fan of 9 triangles of different sizes, some inside, some across the box border, some wholly outside, a pair sharing an edge across the border;
//   3 frame 2 over a floor with alpha 255 and an earlier blended layer (the destination alpha is then read by pass 2);
//   4 every triangle outside (nothing is drawn: the clipped list is empty);
//   5 a thin sliver through the silhouette across two borders and two triangles grazing the border lines.
// Controls (the test must SEE them): NEG1 a box shrunk to the middle 40 percent of the texture (cuts silhouette pixels: the picture must differ), NEG2 a clip that cuts
// but keeps triangles wholly outside nothing (identical picture: shows the drop is exactness-neutral, counted), NEG3 the clipped list drawn with the texture's alpha
// ring set to 255 (the premise broken: the picture must differ).
#include "vgx_state.h"
#include "port/stage_knob.h"
#include "port/colshadow_clip.h"
#include <GLES2/gl2.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" void port_log_summary(const char* fmt, ...) {
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
}  // namespace vgx
namespace {
using namespace vgx;
using port::colshadow::ClipVtx;
alignas(65536) unsigned char destinations[2][65536];
alignas(65536) unsigned char shadowTex[2][65536];  // 0: normal (ring 0), 1: ring 255 (NEG3)
unsigned failures = 0;
void Check(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
void XF(unsigned reg, unsigned value) { WriteXF(reg, 1, &value); }
void Color(unsigned r, unsigned green, unsigned b, unsigned alpha) {
    BP(0xE2, r | alpha << 12);
    BP(0xE3, b | green << 12);
}
constexpr int kTexN = 64;
// IA8: a disc of radius 20 texels around the centre (silhouette, alpha 255), 0 elsewhere; the outermost ring of texels 0 (ring255: 255)
void FillTex(int which) {
    unsigned char* d = shadowTex[which];
    for (int y = 0; y < kTexN; ++y)
        for (int x = 0; x < kTexN; ++x) {
            const float dx = x + 0.5f - 32.0f, dy = y + 0.5f - 32.0f;
            unsigned a = (dx * dx + dy * dy < 20.0f * 20.0f) ? 255u : 0u;
            const bool ring = x == 0 || y == 0 || x == kTexN - 1 || y == kTexN - 1;
            if (ring) a = which ? 255u : 0u;
            unsigned char* p = d + (((y >> 2) * 16 + (x >> 2)) * 32) + ((y & 3) * 4 + (x & 3)) * 2;
            p[0] = static_cast<unsigned char>(a);
            p[1] = static_cast<unsigned char>(a);  // (both bytes: the same alpha whichever byte the decoder takes)
        }
}
void BindShadow(int which) {
    auto& t = g.tex[0];
    t = TexSlot{};
    t.valid = true;
    t.obj.data = shadowTex[which];
    t.obj.width = t.obj.height = kTexN;
    t.obj.format = 3;
    g.bp[0x88] = 63u | (63u << 10) | (3u << 20);
    g.bp[0x94] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(shadowTex[which]) >> 5) & 0xFFFFFFu;
    g.bpTexStale = 0;
    g.texDirty = true;
    g.texSlotDirty = 1;
    XF(0x103F, 1);
    XF(0x1040, 5u << 7);
    BP(0x00, 1);
    BP(0x28, (1u << 6) | (7u << 7));
    BP(0x80, (1u << 4) | (1u << 5));  // clamp, linear mag and min
}
void TexturedState() {  // draw3's state: alpha test != 0, no depth test, dst-alpha blend, texture alpha as the alpha
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (4u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (5u << 16) | (5u << 19));
    BP(0x40, 0);
    Color(0, 0, 0, 255);
}
void Plain(unsigned r, unsigned g_, unsigned b, unsigned a, bool blend) {
    BP(0x00, 0);
    XF(0x103F, 0);
    BP(0x28, 7u << 7);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, 0);
    BP(0x41, (blend ? 1u | (4u << 8) | (5u << 5) : 0u) | (1u << 3) | (1u << 4));
    BP(0x42, 0);  // (the receiver keeps alpha 255: no constant destination alpha)
    Color(r, g_, b, a);
}
struct Tri { ClipVtx v[3]; };
// uv a linear function of the position: the texture covers [-0.25, 0.25]^2 of the screen (u = 0.5 + 2 x, v = 0.5 - 2 y)
ClipVtx Mk(float x, float y) { return ClipVtx{x, y, -0.5f, 0.5f + 2.0f * x, 0.5f - 2.0f * y}; }
Tri T3(float x0, float y0, float x1, float y1, float x2, float y2) { return Tri{{Mk(x0, y0), Mk(x1, y1), Mk(x2, y2)}}; }
void Send(const ClipVtx* v, int m) {  // a fan of m vertices as triangles (the game's EmitClipped)
    std::vector<float> f;
    for (int t = 1; t + 1 < m; ++t) {
        const int idx[3] = {0, t, t + 1};
        for (int k = 0; k < 3; ++k) {
            const ClipVtx& p = v[idx[k]];
            const float row[] = {p.x, p.y, p.z, 0.0f, p.u, p.v};
            f.insert(f.end(), row, row + 6);
        }
    }
    DrawDecoded(0x90, VL_TEX0, reinterpret_cast<const uint8_t*>(f.data()), static_cast<unsigned>(3 * (m - 2)));
}
void TwoPasses(const ClipVtx* v, int m) {  // CollisionShadow::draw3 for one polygon
    BP(0x41, 1 | (1u << 4) | (6u << 8) | (7u << 5));
    BP(0x42, 0);
    Send(v, m);
    BP(0x41, 1 | (1u << 3) | (1u << 4) | (6u << 8) | (7u << 5));
    BP(0x42, 1u << 8);
    Send(v, m);
}
enum Mode { WHOLE, CLIP, SHRUNK, CLIPKEEP };
struct Stats { int tris = 0, dropped = 0, cut = 0, inside = 0; double areaIn = 0, areaOut = 0; };
void Shade(const std::vector<Tri>& list, Mode mode, Stats& st) {
    for (const Tri& t : list) {
        st.tris++;
        if (mode == WHOLE) { TwoPasses(t.v, 3); continue; }
        ClipVtx out[port::colshadow::kClipMax];
        const float pad = 1.0f / kTexN;
        const float lo = mode == SHRUNK ? 0.30f : -pad, hi = mode == SHRUNK ? 0.70f : 1.0f + pad;
        const int m = port::colshadow::ClipUvBox(t.v, 3, lo, hi, out);
        st.areaIn += port::colshadow::UvArea(t.v, 3);
        if (m == 0) {
            st.dropped++;
            if (mode == CLIPKEEP) TwoPasses(t.v, 3);  // (NEG2: kept: must give the same picture as dropping)
            continue;
        }
        st.areaOut += port::colshadow::UvArea(out, m);
        if (m == 3 && std::memcmp(out, t.v, sizeof out[0] * 3) == 0) st.inside++; else st.cut++;
        TwoPasses(out, m);
    }
}
std::vector<uint8_t> ReadAndFlush() {
    g.texCopySrc = EfbRect{0, 0, 64, 64};
    g.texCopyDstW = g.texCopyDstH = 64;
    g.texCopyFmt = 6;
    QueueCopyTex(destinations[0], false);
    const auto id = Pending().copyTex.back().texId;
    FrontPresent(false, nullptr);
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
double Psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, unsigned& differing) {
    double se = 0;
    differing = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double d = static_cast<double>(a[i]) - b[i];
        se += d * d;
        if (a[i] != b[i]) ++differing;
    }
    return se == 0 ? 999.0 : 10.0 * std::log10(255.0 * 255.0 / (se / static_cast<double>(a.size())));
}
std::vector<Tri> List(int f) {
    std::vector<Tri> l;
    switch (f) {
    case 1: l.push_back(T3(-3, -3, 3, -3, 0, 3)); break;  // the deck plate: covers everything
    case 2:
    case 3:
        l.push_back(T3(-1.5f, -1.5f, 1.5f, -1.0f, 0.0f, 1.6f));       // huge, covers the box
        l.push_back(T3(-0.10f, -0.10f, 0.12f, -0.08f, 0.0f, 0.15f));  // wholly inside
        l.push_back(T3(0.15f, -0.2f, 0.9f, -0.1f, 0.3f, 0.5f));       // across the border
        l.push_back(T3(0.5f, 0.5f, 0.9f, 0.5f, 0.7f, 0.9f));          // wholly outside
        l.push_back(T3(-0.9f, 0.6f, -0.5f, 0.55f, -0.7f, 0.95f));     // wholly outside
        l.push_back(T3(-0.3f, -0.3f, 0.3f, -0.3f, -0.3f, 0.3f));      // a pair sharing the diagonal across the whole box
        l.push_back(T3(0.3f, -0.3f, 0.3f, 0.3f, -0.3f, 0.3f));
        l.push_back(T3(-0.9f, -0.05f, -0.2f, -0.02f, -0.5f, 0.04f));  // across the left border
        l.push_back(T3(0.05f, 0.2f, 0.07f, 0.9f, 0.09f, 0.21f));      // a thin sliver across the top border
        break;
    case 4:
        l.push_back(T3(0.5f, 0.5f, 0.9f, 0.5f, 0.7f, 0.9f));
        l.push_back(T3(-0.9f, -0.9f, -0.6f, -0.9f, -0.7f, -0.6f));
        l.push_back(T3(0.4f, -0.9f, 0.95f, -0.4f, 0.45f, -0.45f));
        break;
    case 5:
        l.push_back(T3(-0.9f, -0.01f, 0.9f, 0.0f, 0.0f, 0.04f));     // a thin sliver through the silhouette, across the left and right borders
        l.push_back(T3(-0.9f, 0.255f, 0.9f, 0.262f, 0.0f, 0.30f));   // grazing the top border line
        l.push_back(T3(-0.9f, -0.248f, 0.9f, -0.251f, 0.0f, -0.6f)); // grazing the bottom border line, mostly outside
        break;
    }
    return l;
}
std::vector<uint8_t> Frame(int f, Mode mode, int texWhich, Stats& st) {
    st = Stats{};
    BindShadow(texWhich);
    // floor: opaque, alpha 255 (frames 3: a blended layer on top of it first)
    Plain(40, 40, 60, 255, false);
    {
        const float v[] = {-1,-1,-0.9f,0, 1,-1,-0.9f,0, 1,1,-0.9f,0, -1,1,-0.9f,0};
        DrawDecoded(0x80, 0, reinterpret_cast<const uint8_t*>(v), 4);
    }
    if (f == 3) {
        Plain(200, 60, 60, 128, true);
        const float v[] = {-1,-0.3f,-0.8f,0, 1,-0.3f,-0.8f,0, 1,0.3f,-0.8f,0, -1,0.3f,-0.8f,0};
        DrawDecoded(0x80, 0, reinterpret_cast<const uint8_t*>(v), 4);
    }
    BindShadow(texWhich);
    TexturedState();
    Shade(List(f), mode, st);
    return ReadAndFlush();
}
}  // namespace
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
    const uint32_t w[] = {f2u(32), f2u(-32), f2u(16777216.0f), f2u(374), f2u(374), f2u(16777216.0f)};
    WriteXF(0x101A, 6, w);
    FillTex(0);
    FillTex(1);
    unsigned long long nonBlack = 0;
    for (int f = 1; f <= 5; ++f) {
        Stats sw, sc, ss, sk, sr;
        const auto whole = Frame(f, WHOLE, 0, sw);
        const auto clip = Frame(f, CLIP, 0, sc);
        const auto keep = Frame(f, CLIPKEEP, 0, sk);
        const auto shrunk = Frame(f, SHRUNK, 0, ss);
        const auto ring = Frame(f, CLIP, 1, sr);
        const auto ringWhole = Frame(f, WHOLE, 1, sw);
        if (whole.empty() || clip.empty() || shrunk.empty()) return 1;
        unsigned dc = 0, dk = 0, ds = 0, dr = 0;
        const double pc = Psnr(whole, clip, dc), pk = Psnr(clip, keep, dk), ps = Psnr(whole, shrunk, ds), pr = Psnr(ringWhole, ring, dr);
        std::printf("frame%d whole=%016llx clip=%016llx psnr=%.2f differing_bytes=%u | tris=%d dropped=%d cut=%d inside=%d uv_area_in=%.3f out=%.3f | NEG1 shrunk psnr=%.2f differing=%u | NEG2 keep-outside psnr=%.2f | NEG3 ring255 clip-vs-whole psnr=%.2f differing=%u\n",
                    f, static_cast<unsigned long long>(Hash(whole)), static_cast<unsigned long long>(Hash(clip)), pc, dc, sc.tris, sc.dropped, sc.cut, sc.inside,
                    sc.areaIn, sc.areaOut, ps, ds, pk, pr, dr);
        char name[96];
        std::snprintf(name, sizeof name, "frame %d: clipped picture bit-identical to the whole triangles (RGB and alpha)", f);
        Check(dc == 0 && Hash(whole) == Hash(clip), name);
        std::snprintf(name, sizeof name, "frame %d: dropping wholly-outside triangles is neutral (NEG2)", f);
        Check(dk == 0, name);
        for (size_t i = 0; i < whole.size(); i += 4) nonBlack += whole[i] > 60;
        if (f != 4) {  // (frame 4 has nothing to shade)
            std::snprintf(name, sizeof name, "frame %d: NEG1 (shrunk box) is detected, the picture differs", f);
            Check(ds > 0, name);
        }
        if (f == 1 || f == 2 || f == 3) {
            std::snprintf(name, sizeof name, "frame %d: NEG3 (premise broken, ring alpha 255) is detected", f);
            Check(dr > 0, name);
        }
    }
    Check(nonBlack > 0, "the shadow is visible in the compared pictures (the test is not comparing empty pictures)");
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
