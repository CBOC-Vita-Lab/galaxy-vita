// GXV_AUTOMIP (automip25): the production GX front end, the texture cache and the production GLES back end on Mesa, no game data.
// A road in perspective (a 256x256 RGB565 checker with a fine line pattern, repeating, a CMPR wall, an alpha cut-out fence, a nearest-filtered
// strip, a small texture, a texture whose bytes change every picture, and a 2D picture in the corner), the camera panning slowly: every
// picture is read back and printed as a hash; the runner (tools/test_vgx_automip_gl.sh) compares the runs:
//   GXV_AUTOMIP unset / 0 / 2   every picture bit-identical (default off; census only changes nothing);
//   GXV_AUTOMIP=1               the ground, the wall (and with GXV_AUTOMIP_ALPHA=1 the fence) are drawn from a chain after the settle time; the 2D
//                               picture, the nearest strip, the small and the changing texture are not given levels; far rows differ, near rows do not.
// Output lines: "frame <n> hash <h>", "stab <region> <mean absolute difference between consecutive pictures>" over the frames after the
// conversions, "automip ..." lines of the log. AM_DUMP=<prefix> writes two pictures as PPM; AM_FRAMES=<n> (default 90).
#include "vgx_state.h"
#include "vgx_hle.h"
#include <GLES2/gl2.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" void port_log_summary(const char* fmt, ...) {
    char line[1500];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("LOG %s\n", line);
}
namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
} // namespace vgx
namespace {
using namespace vgx;
constexpr int kEfbW = 640, kEfbH = 456;
constexpr uint32_t kRam = 8u << 20;
alignas(4096) uint8_t g_ram[kRam];
uint32_t g_brk = 0x1000;
uint32_t Alloc(uint32_t bytes) {
    const uint32_t a = g_brk;
    g_brk += (bytes + 31u) & ~31u;
    return a;
}
const uint8_t* Resolve(uint32_t address, uint32_t* avail) {
    *avail = 0;
    address &= 0x3FFFFFFFu;
    if (address == 0 || address >= kRam) return nullptr;
    *avail = kRam - address;
    return g_ram + address;
}
alignas(32) unsigned char readDestination[1 << 20];
unsigned failures = 0;
void Check(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
void XF(unsigned reg, unsigned value) { WriteXF(reg, 1, &value); }
void Viewport(float x, float y, float w, float h) {
    const uint32_t v[] = {f2u(w / 2.0f), f2u(-h / 2.0f), f2u(16777215.0f + 1), f2u(342.0f + x + w / 2.0f), f2u(342.0f + y + h / 2.0f), f2u(16777215.0f + 1)};
    WriteXF(0x101A, 6, v);
}
void Scissor(int x, int y, int w, int h) {
    BP(0x20, (342u + y) | ((342u + x) << 12));
    BP(0x21, (342u + y + h - 1) | ((342u + x + w - 1) << 12));
}
void Color(unsigned r, unsigned g_, unsigned b, unsigned a) {
    BP(0xE2, r | a << 12);
    BP(0xE3, b | g_ << 12);
}
void ZMode(bool test, unsigned func, bool update) { BP(0x40, (test ? 1u : 0u) | (func << 1) | (update ? 1u << 4 : 0u)); }
constexpr unsigned kCU = 1u << 3, kAU = 1u << 4;
void Ortho() {
    const uint32_t proj[] = {f2u(1), 0, f2u(1), 0, f2u(1), 0, 1};
    WriteXF(0x1020, 7, proj);
}
void Persp() {
    const float n = 0.5f, f = 200.0f;
    const uint32_t proj[] = {f2u(1.4f), 0, f2u(1.4f * kEfbW / kEfbH), 0, f2u(-n / (f - n)), f2u(-f * n / (f - n)), 0};
    WriteXF(0x1020, 7, proj);
}
void PlainState() {
    BP(0x00, 0 | (0u << 10));
    XF(0x103F, 0);
    BP(0x28, 7u << 7);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
}

// ---- the textures, in GX layouts, in guest RAM
struct Tex { uint32_t addr = 0; int w = 0, h = 0; unsigned fmt = 0; };
inline void Be16(uint8_t* p, unsigned v) { p[0] = static_cast<uint8_t>(v >> 8); p[1] = static_cast<uint8_t>(v); }
// the pattern: 8-texel checker, one bright texel line every 16 texels (a fine detail that aliases when minified)
void Pattern(int x, int y, int kind, unsigned& r, unsigned& g_, unsigned& b, unsigned& a) {
    const bool c = ((x >> 3) + (y >> 3)) & 1;
    r = c ? 220 : 40; g_ = c ? 200 : 60; b = c ? 120 : 160; a = 255;
    if (kind == 1) { r = c ? 30 : 230; g_ = c ? 120 : 230; b = c ? 200 : 230; }       // wall
    if ((x & 15) == 0 || (y & 15) == 0) { r = 255; g_ = 255; b = 255; }              // the fine lines
    if (kind == 2) { a = ((x & 15) < 12 && (y & 15) < 12) ? 255 : 0; }                // fence: cut-out squares
    if (kind == 3) { r = static_cast<unsigned>(x * 2); g_ = static_cast<unsigned>(y * 2); b = 90; } // the 2D picture
}
Tex MakeTex(int w, int h, unsigned fmt, int kind) {
    Tex t;
    t.w = w; t.h = h; t.fmt = fmt;
    const uint32_t bytes = fmt == 0xE ? static_cast<uint32_t>(w * h / 2) : static_cast<uint32_t>(w * h * 2);
    t.addr = Alloc(bytes);
    uint8_t* d = g_ram + t.addr;
    if (fmt == 0xE) {
        // CMPR: 8x8 tiles of four 4x4 blocks, each block c0 (BE), c1 (BE), four index rows; colours are read from the pattern's block centre
        const int tw = w / 8, th = h / 8;
        for (int ty = 0; ty < th; ++ty)
            for (int tx = 0; tx < tw; ++tx)
                for (int sb = 0; sb < 4; ++sb) {
                    const int bx = tx * 8 + (sb & 1) * 4, by = ty * 8 + (sb >> 1) * 4;
                    unsigned r, g_, b, a;
                    Pattern(bx + 1, by + 1, kind, r, g_, b, a);
                    const unsigned c0 = ((r >> 3) << 11) | ((g_ >> 2) << 5) | (b >> 3), c1 = (((r >> 3) / 2) << 11) | (((g_ >> 2) / 2) << 5) | ((b >> 3) / 2);
                    uint8_t* p = d + ((static_cast<size_t>(ty) * tw + tx) * 4 + sb) * 8;
                    Be16(p, c0 > c1 ? c0 : c1);
                    Be16(p + 2, c0 > c1 ? c1 : c0);
                    for (int row = 0; row < 4; ++row) {
                        unsigned bits = 0;
                        for (int col = 0; col < 4; ++col) {
                            unsigned rr, gg, bb, aa;
                            Pattern(bx + col, by + row, kind, rr, gg, bb, aa);
                            bits = bits << 2 | ((rr == 255 && gg == 255 && bb == 255) ? 0u : 0u); // (the lines are c0)
                        }
                        p[4 + row] = static_cast<uint8_t>(bits);
                    }
                }
        return t;
    }
    // 16-bit formats: 4x4 texel tiles
    const int tw = w / 4, th = h / 4;
    for (int ty = 0; ty < th; ++ty)
        for (int tx = 0; tx < tw; ++tx)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    unsigned r, g_, b, a;
                    Pattern(tx * 4 + x, ty * 4 + y, kind, r, g_, b, a);
                    unsigned v;
                    if (fmt == 4) v = ((r >> 3) << 11) | ((g_ >> 2) << 5) | (b >> 3);
                    else v = a >= 224 ? (0x8000u | ((r >> 3) << 10) | ((g_ >> 3) << 5) | (b >> 3)) : (((a >> 5) << 12) | ((r >> 4) << 8) | ((g_ >> 4) << 4) | (b >> 4));
                    Be16(d + ((static_cast<size_t>(ty) * tw + tx) * 16 + y * 4 + x) * 2, v);
                }
    return t;
}
// texture unit 0 from a guest texture: linear (hw 4) or nearest (0), repeat wrap, no mip (max LOD 0)
void BindGuest(const Tex& t, bool nearest) {
    BP(0x94, t.addr >> 5);
    BP(0x88, static_cast<unsigned>(t.w - 1) | (static_cast<unsigned>(t.h - 1) << 10) | (t.fmt << 20));
    BP(0x80, 1u | (1u << 2) | (1u << 4) | ((nearest ? 0u : 4u) << 5));
    BP(0x84, 0);
    BP(0x00, 1u | (0u << 10));
    XF(0x103F, 1);
    XF(0x1040, 5u << 7);
    BP(0x28, (1u << 6) | (7u << 7));
    BP(0xC0, (8u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19)); // colour = TEXC
    BP(0xC1, (4u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19)); // alpha = TEXA
}
void Rect2D(float l, float r, float bt, float tp, float depth) {
    const float z = depth - 1;
    const float v[] = {l, bt, z, 0, 0, 1, r, bt, z, 0, 1, 1, r, tp, z, 0, 1, 0, l, tp, z, 0, 0, 0};
    DrawDecoded(0x80, VL_TEX0, reinterpret_cast<const uint8_t*>(v), 4);
}
double g_prevSum = 0;
std::vector<uint8_t> g_prev;
double g_stab[4] = {}; // whole, far ground band, near ground band, left wall band
long g_stabN = 0;
unsigned long long Hash(const std::vector<uint8_t>& p) {
    unsigned long long h = 14695981039346656037ull;
    for (uint8_t b : p) h = (h ^ b) * 1099511628211ull;
    return h;
}
void WritePpm(const std::string& path, const std::vector<uint8_t>& px, int w, int h) {
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) std::fwrite(&px[(static_cast<size_t>(y) * w + x) * 4], 1, 3, f);
        std::fclose(f);
    }
}
} // namespace

int main() {
    const int frames = std::getenv("AM_FRAMES") ? std::atoi(std::getenv("AM_FRAMES")) : 90;
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(kEfbW, kEfbH), "production backend init");
    if (failures) return 1;
    hle::RegisterGuestRam(g_ram, kRam);
    GuestHooks hooks{};
    hooks.resolve = Resolve;
    SetGuestHooks(hooks);
    ResetState();
    g.efbW = kEfbW;
    g.efbH = kEfbH;
    QueueSetTarget();
    BP(0x43, 1);
    Scissor(0, 0, kEfbW, kEfbH);
    BP(0x59, 171u | (171u << 10));
    const uint32_t m[] = {f2u(1), 0, 0, 0, 0, f2u(1), 0, 0, 0, 0, f2u(1), 0};
    WriteXF(0, 12, m);
    const Tex road = MakeTex(256, 256, 4, 0), wall = MakeTex(128, 128, 0xE, 1), fence = MakeTex(128, 128, 5, 2), strip = MakeTex(128, 128, 4, 0),
              small = MakeTex(32, 32, 4, 0), anim = MakeTex(64, 64, 4, 0), pic2d = MakeTex(128, 128, 4, 3);
    hle::NotifyGuestWrite(g_ram, kRam);
    for (int f = 1; f <= frames; ++f) {
        Viewport(0, 0, kEfbW, kEfbH);
        Scissor(0, 0, kEfbW, kEfbH);
        // the changing texture: a few of its bytes differ in every picture
        g_ram[anim.addr + 7] = static_cast<uint8_t>(f * 13);
        hle::NotifyGuestWrite(g_ram + anim.addr, 64);
        PlainState();
        Ortho();
        BP(0x00, 0);
        ZMode(true, 3, false);
        BP(0x41, kCU | kAU);
        Color(110, 150, 220, 255);
        { const float z = 0.0f, sk[] = {-1, -1, z, 0, 1, -1, z, 0, 1, 1, z, 0, -1, 1, z, 0}; DrawDecoded(0x80, 0, reinterpret_cast<const uint8_t*>(sk), 4); }
        Persp();
        ZMode(true, 3, true);
        const float pan = 0.03f * static_cast<float>(f); // the camera moves right: the world moves left
        auto X = [&](float x) { return x - pan; };
        // the ground (the camera 1.2 above it)
        BindGuest(road, false);
        { const float a[3] = {X(-40), -1.2f, -1}, b[3] = {X(40), -1.2f, -1}, c[3] = {X(40), -1.2f, -160}, d[3] = {X(-40), -1.2f, -160};
          const float uvAdj = 0.08f; // 12.5 world units a texture repeat
          float v[24]; const float* ps[4] = {a, b, c, d};
          for (int i = 0; i < 4; ++i) { v[i*6+0]=ps[i][0]; v[i*6+1]=ps[i][1]; v[i*6+2]=ps[i][2]; v[i*6+3]=0; v[i*6+4]=(ps[i][0] + pan) * uvAdj; v[i*6+5]=ps[i][2] * uvAdj; }
          DrawDecoded(0x80, VL_TEX0, reinterpret_cast<const uint8_t*>(v), 4); }
        // a strip down the middle, a little above the ground, with the nearest-filtered texture
        BindGuest(strip, true);
        { float v[24]; const float xs[4] = {-0.8f, 0.8f, 0.8f, -0.8f}, zs[4] = {-1, -1, -160, -160};
          for (int i = 0; i < 4; ++i) { v[i*6+0]=X(xs[i]); v[i*6+1]=-1.15f; v[i*6+2]=zs[i]; v[i*6+3]=0; v[i*6+4]=(xs[i]) * 0.3f; v[i*6+5]=zs[i] * 0.3f; }
          DrawDecoded(0x80, VL_TEX0, reinterpret_cast<const uint8_t*>(v), 4); }
        // the left wall (CMPR), a vertical plane at x = -6
        BindGuest(wall, false);
        { float v[24]; const float ys[4] = {-1.2f, -1.2f, 6, 6}, zs[4] = {-1, -160, -160, -1};
          for (int i = 0; i < 4; ++i) { v[i*6+0]=X(-6); v[i*6+1]=ys[i]; v[i*6+2]=zs[i]; v[i*6+3]=0; v[i*6+4]=zs[i] * 0.1f; v[i*6+5]=ys[i] * 0.1f; }
          DrawDecoded(0x80, VL_TEX0, reinterpret_cast<const uint8_t*>(v), 4); }
        // the right fence (alpha cut-out, tested at 0.5)
        BindGuest(fence, false);
        BP(0xF3, 128u | (6u << 16) | (7u << 19));
        { float v[24]; const float ys[4] = {-1.2f, -1.2f, 3, 3}, zs[4] = {-1, -160, -160, -1};
          for (int i = 0; i < 4; ++i) { v[i*6+0]=X(6); v[i*6+1]=ys[i]; v[i*6+2]=zs[i]; v[i*6+3]=0; v[i*6+4]=zs[i] * 0.1f; v[i*6+5]=ys[i] * 0.3f; }
          DrawDecoded(0x80, VL_TEX0, reinterpret_cast<const uint8_t*>(v), 4); }
        BP(0xF3, (7u << 16) | (7u << 19));
        // a small texture and the changing one, on little quads in the middle of the road (3D)
        BindGuest(small, false);
        { const float a[3] = {X(-3), -1.1f, -6}, b[3] = {X(-2), -1.1f, -6}, c[3] = {X(-2), -1.1f, -9}, d[3] = {X(-3), -1.1f, -9};
          float v[24]; const float* ps[4] = {a, b, c, d}; const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
          for (int i = 0; i < 4; ++i) { v[i*6+0]=ps[i][0]; v[i*6+1]=ps[i][1]; v[i*6+2]=ps[i][2]; v[i*6+3]=0; v[i*6+4]=uv[i][0]; v[i*6+5]=uv[i][1]; }
          DrawDecoded(0x80, VL_TEX0, reinterpret_cast<const uint8_t*>(v), 4); }
        BindGuest(anim, false);
        { const float a[3] = {X(2), -1.1f, -6}, b[3] = {X(3), -1.1f, -6}, c[3] = {X(3), -1.1f, -9}, d[3] = {X(2), -1.1f, -9};
          float v[24]; const float* ps[4] = {a, b, c, d}; const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
          for (int i = 0; i < 4; ++i) { v[i*6+0]=ps[i][0]; v[i*6+1]=ps[i][1]; v[i*6+2]=ps[i][2]; v[i*6+3]=0; v[i*6+4]=uv[i][0]; v[i*6+5]=uv[i][1]; }
          DrawDecoded(0x80, VL_TEX0, reinterpret_cast<const uint8_t*>(v), 4); }
        // the 2D picture in the corner (orthographic): 64 x 64 pixels of a 128 x 128 texture (minified): it must keep level 0
        Ortho();
        ZMode(false, 7, false);
        BindGuest(pic2d, false);
        Rect2D(0.70f, 0.90f, 0.60f, 0.90f, 0.5f);
        // the display copy
        Viewport(0, 0, kEfbW, kEfbH);
        BP(0x41, kCU | kAU);
        Persp();
        g.texCopySrc = EfbRect{0, 0, kEfbW, kEfbH};
        g.texCopyDstW = kEfbW;
        g.texCopyDstH = kEfbH;
        g.texCopyFmt = 6;
        QueueCopyTex(readDestination, false);
        const uint32_t id = Pending().copyTex.back().texId;
        BP(0x4F, 0);
        BP(0x50, 0);
        BP(0x51, 0xFFFFFF);
        ZMode(true, 3, true);
        g.dispCopySrc = EfbRect{0, 0, kEfbW, kEfbH};
        QueueCopyDisp(true);
        FrontPresent(false, nullptr);
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (!backend::DebugReadTexture(id, px, w, h) || w <= 0) { Check(false, "picture read back"); return 1; }
        std::printf("frame %d hash %016llx\n", f, Hash(px));
        if (std::getenv("AM_DUMP") && (f == 30 || f == frames)) WritePpm(std::string(std::getenv("AM_DUMP")) + "-f" + std::to_string(f) + ".ppm", px, w, h);
        // temporal stability: mean absolute difference (RGB) of consecutive pictures, from the picture 40 on (the conversions are done)
        if (f >= 40 && !g_prev.empty() && g_prev.size() == px.size()) {
            auto band = [&](int y0, int y1, int x0, int x1) {
                double s = 0; long n = 0;
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x)
                        for (int c = 0; c < 3; ++c) { s += std::abs(static_cast<int>(px[(static_cast<size_t>(y) * w + x) * 4 + c]) - static_cast<int>(g_prev[(static_cast<size_t>(y) * w + x) * 4 + c])); ++n; }
                return s / static_cast<double>(n);
            };
            g_stab[0] += band(0, h, 0, w);
            g_stab[1] += band(h / 2 + 4, h / 2 + 60, 200, 440);  // the road just under the horizon
            g_stab[2] += band(h - 60, h - 4, 200, 440);          // the road near the camera
            g_stab[3] += band(h / 2 - 90, h / 2 - 8, 60, 200);   // (sky/wall region)
            ++g_stabN;
        }
        g_prev = px;
    }
    const char* names[4] = {"whole", "far-road", "near-road", "wall-band"};
    for (int i = 0; i < 4; ++i) std::printf("stab %s %.4f\n", names[i], g_stabN ? g_stab[i] / static_cast<double>(g_stabN) : 0.0);
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
