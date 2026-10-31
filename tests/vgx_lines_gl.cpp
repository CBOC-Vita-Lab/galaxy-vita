// GXV_FIX_LINES: GX lines and points drawn at their GX size, through the production front end, shader generator and GLES back end (Mesa).
// A GX line is a parallelogram widened by width/6 EFB pixels along the minor axis of its screen-space direction (an x-major line vertically, a
// y-major one horizontally, a tie x-major); a GX point is an axis-aligned square of size/6 pixels. The pixels are compared with a reference
// rasteriser that evaluates exactly that polygon at the pixel centres (pixels closer than 0.05 pixel to an edge are not judged: the
// rasteriser snaps to 1/256 pixel). Also the texture coordinate offsets (GXSetLineWidth / GXSetPointSize texOffsets with
// GXEnableTexOffsets), line strips, an end point behind the eye, and end points on different palette matrices.
// Usage: vgx_lines_gl <expect-switch 0|1>; LINES_SCALE=n renders into a target n times the EFB
#include "vgx_fixes.h"
#include "vgx_fogvtx_digest.h"
#include "vgx_state.h"
#include "vgx_texdecode.h"
#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
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
constexpr uint32_t kTexAddr = 0x00100000;
constexpr int kW = 96, kH = 96;  // EFB size
int g_scale = 1;                  // the render target is g_scale times the EFB (LINES_SCALE); the copy to the texture averages g_scale^2 pixels
unsigned failures;
std::vector<uint8_t> g_tex;
void Check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
const uint8_t* Resolve(uint32_t addr, uint32_t* toEnd) {
    if (addr < kTexAddr || addr >= kTexAddr + g_tex.size()) return nullptr;
    if (toEnd) *toEnd = static_cast<uint32_t>(kTexAddr + g_tex.size() - addr);
    return g_tex.data() + (addr - kTexAddr);
}
// I8 texture, 64 x 64, value = 4 * x (ramp 0) or 4 * y (ramp 1); I8 is stored in 8 x 4 tiles.
void MakeRamp(int ramp) {
    g_tex.assign(TexDataSize(64, 64, 1, 1), 0);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            g_tex[((y / 4) * 8 + x / 8) * 32 + (y % 4) * 8 + x % 8] = static_cast<uint8_t>(4 * (ramp ? y : x));
}

enum class Fill { Black, White, Tex };
// The state of every case: an orthographic projection whose NDC is the whole target, no depth, colour update, TEV stage 0 outputs black, white or
// the texture.
void Setup(Fill fill, unsigned lpsize, unsigned texCoordBits) {
    ResetState();  // (the identity texture matrix among other things)
    g.efbW = kW;
    g.efbH = kH;
    QueueSetTarget();
    BP(0x43, 1);
    BP(0x20, 342u | (342u << 12)); BP(0x21, (342u + kW - 1) | ((342u + kH - 1) << 12));
    BP(0x59, 171u | (171u << 10));
    const uint32_t m[] = {f2u(1), 0, 0, 0, 0, f2u(1), 0, 0, 0, 0, f2u(1), 0};
    WriteXF(0, 12, m);
    const uint32_t proj[] = {f2u(1), 0, f2u(1), 0, f2u(1), 0, 1};
    WriteXF(0x1020, 7, proj);
    const uint32_t vp[] = {f2u(kW / 2.0f), f2u(-kH / 2.0f), f2u(16777216), f2u(342.0f + kW / 2.0f), f2u(342.0f + kH / 2.0f), f2u(16777216)};
    WriteXF(0x101A, 6, vp);
    const uint32_t one = 1; WriteXF(0x103F, 1, &one);
    const uint32_t tg = 5u << 7; WriteXF(0x1040, 1, &tg);
    BP(0x00, 1);
    BP(0x22, lpsize);
    BP(0x30, 63u | texCoordBits);  // texcoord 0: scale 64 (the texture's size - 1), and GXEnableTexOffsets' bits
    BP(0x31, 63u);
    BP(0x80, 0);  // texmap 0: clamp, nearest
    BP(0x84, 0);
    BP(0x88, 63u | (63u << 10) | (1u << 20));
    BP(0x94, kTexAddr >> 5);
    BP(0x28, fill == Fill::Tex ? ((1u << 6) | (7u << 7)) : (7u << 7));
    const unsigned d = fill == Fill::Tex ? 8u : fill == Fill::White ? 12u : 15u;
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | d | (1u << 19));
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, 0); BP(0x41, (1u << 3) | (1u << 4));
}
float Nx(float px) { return px / kW * 2.0f - 1.0f; }
float Ny(float py) { return 1.0f - py / kH * 2.0f; }

struct Image { std::vector<uint8_t> v; };  // red channel, kW x kH, rows top first
bool g_flip = false;                        // calibrated: the read-back has the top row last
Image Read() {
    g.texCopySrc = EfbRect{0, 0, kW, kH};
    g.texCopyDstW = kW;
    g.texCopyDstH = kH;
    g.texCopyFmt = 6;
    alignas(32) static unsigned char dest[32];
    QueueCopyTex(dest, false);
    const auto id = Pending().copyTex.back().texId;
    FrontPresent(false, nullptr);
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    Image img;
    if (!backend::DebugReadTexture(id, pixels, w, h) || (w != kW && w != kW * g_scale) || (h != kH && h != kH * g_scale)) {
        Check(false, "read actual EFB pixels");
        img.v.assign(kW * kH, 0);
        return img;
    }
    img.v.resize(kW * kH);
    const int n = w / kW;  // (1: the copy was already at the EFB's size; else the render target's: averaged here)
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            int sum = 0;
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) sum += pixels[(((g_flip ? kH * n - 1 - (y * n + j) : y * n + j)) * w + x * n + i) * 4];
            img.v[y * kW + x] = static_cast<uint8_t>(sum / (n * n));
        }
    return img;
}
void Clear() {
    Setup(Fill::Black, 6, 0);
    const float v[] = {-1, -1, -0.5f, 0, 1, -1, -0.5f, 0, 1, 1, -0.5f, 0, -1, 1, -0.5f, 0};
    DrawDecoded(0x80, 0, reinterpret_cast<const uint8_t*>(v), 4);
}
struct V { float x, y, s, t; };  // EFB pixels (y down) and texture coordinates
void DrawPrim(uint8_t prim, const std::vector<V>& vs) {
    std::vector<float> f;
    for (const V& v : vs) { f.push_back(Nx(v.x)); f.push_back(Ny(v.y)); f.push_back(-0.5f); f.push_back(0); f.push_back(v.s); f.push_back(v.t); }
    DrawDecoded(prim, VL_TEX0, reinterpret_cast<const uint8_t*>(f.data()), static_cast<uint32_t>(vs.size()));
}

// ---------------- the reference rasteriser: a convex quadrilateral evaluated at the pixel centres
struct Poly { double x[4], y[4]; };
double Cross(double ax, double ay, double bx, double by) { return ax * by - ay * bx; }
bool Inside(const Poly& p, double px, double py) {
    int pos = 0, neg = 0;
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) & 3;
        const double c = Cross(p.x[j] - p.x[i], p.y[j] - p.y[i], px - p.x[i], py - p.y[i]);
        pos += c > 0;
        neg += c < 0;
    }
    return pos == 0 || neg == 0;
}
double EdgeDist(const Poly& p, double px, double py) {
    double best = 1e30;
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) & 3;
        const double dx = p.x[j] - p.x[i], dy = p.y[j] - p.y[i];
        const double l2 = dx * dx + dy * dy;
        double t = l2 > 0 ? ((px - p.x[i]) * dx + (py - p.y[i]) * dy) / l2 : 0;
        t = std::max(0.0, std::min(1.0, t));
        best = std::min(best, std::hypot(px - (p.x[i] + t * dx), py - (p.y[i] + t * dy)));
    }
    return best;
}
// The GX parallelogram of a line of w6 / 6 pixels between two points (screen space, y down).
Poly LinePoly(double x0, double y0, double x1, double y1, unsigned w6) {
    const double hw = w6 / 12.0;
    const bool tall = std::fabs(y1 - y0) > std::fabs(x1 - x0);
    const double ox = tall ? hw : 0, oy = tall ? 0 : hw;
    return Poly{{x0 - ox, x0 + ox, x1 + ox, x1 - ox}, {y0 - oy, y0 + oy, y1 + oy, y1 - oy}};
}
Poly PointPoly(double x, double y, unsigned s6) {
    const double h = s6 / 12.0;
    return Poly{{x - h, x + h, x + h, x - h}, {y - h, y - h, y + h, y + h}};
}
struct Verdict { int mismatch = 0, band = 0, covered = 0, expected = 0; };
Verdict Judge(const Image& img, const std::vector<Poly>& polys) {
    Verdict r;
    const int n = g_scale;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            int inSub = 0;
            bool band = false;
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) {
                    const double cx = x + (i + 0.5) / n, cy = y + (j + 0.5) / n;
                    bool in = false;
                    double dist = 1e30;
                    for (const Poly& p : polys) {
                        in |= Inside(p, cx, cy);
                        dist = std::min(dist, EdgeDist(p, cx, cy));
                    }
                    inSub += in;
                    band |= dist < 0.05 / n;
                }
            const double want = 255.0 * inSub / (n * n);
            r.expected += inSub * 2 > n * n;
            r.covered += img.v[y * kW + x] > 127;
            if (band) { ++r.band; continue; }
            if (std::fabs(img.v[y * kW + x] - want) > 12.0) ++r.mismatch;
        }
    return r;
}
bool g_on = false;
bool GeomCase(const char* name, uint8_t prim, const std::vector<V>& vs, unsigned size6, bool point, const std::vector<Poly>& polys) {
    Clear();
    Setup(Fill::White, point ? (size6 << 8) : size6, 0);
    DrawPrim(prim, vs);
    const Image img = Read();
    const Verdict r = Judge(img, polys);
    if (!g_on) return true;  // (switch off: the old path; only that it ran)
    char msg[220];
    std::snprintf(msg, sizeof msg, "%s: %d mismatching pixels (%d near an edge not judged; %d lit, %d expected)", name, r.mismatch, r.band, r.covered, r.expected);
    Check(r.mismatch == 0 && r.expected > 0, msg);
    return r.mismatch == 0;
}
bool Near(double got, double want, double tol) { return std::fabs(got - want) <= tol; }
double SampleS(double s) { return 4.0 * std::min(63.0, std::max(0.0, std::floor(64.0 * s))); }  // the ramp texture, nearest
}  // namespace

int main(int argc, char** argv) {
    g_on = argc > 1 && std::atoi(argv[1]) == 1;
    Check(host::CreateContext(), "GLES context");
    if (const char* e = std::getenv("LINES_SCALE")) g_scale = std::max(1, std::atoi(e));
    Check(backend::Init(kW * g_scale, kH * g_scale), "production backend init");
    if (failures) return 1;
    // Everything the generator makes for a key without line / point fields is what it was.
    unsigned nv = 0;
    Check(fogvtx_digest::VertexDigest(&nv) == 0xe2cea692dfe69c89ull, "vertex sources of ordinary keys are byte-identical to the generator before GXV_FIX_LINES");
    Check(sizeof(VsKey) == 100, "sizeof(VsKey) unchanged");
    GuestHooks hooks;
    hooks.resolve = Resolve;
    SetGuestHooks(hooks);
    MakeRamp(0);

    // The read-back's row order: a white block along the top of the target.
    {
        Clear();
        Setup(Fill::White, 6, 0);
        const float v[] = {-1, 0.8f, -0.5f, 0, 1, 0.8f, -0.5f, 0, 1, 1, -0.5f, 0, -1, 1, -0.5f, 0};
        DrawDecoded(0x80, 0, reinterpret_cast<const uint8_t*>(v), 4);
        const Image img = Read();
        g_flip = img.v[2 * kW + 40] < 128;
        std::printf("read-back rows %s\n", g_flip ? "bottom first" : "top first");
    }

    if (g_on && g_scale == 1) {  // (the texture rig itself: a plain quad of one texture coordinate)
        Clear();
        Setup(Fill::Tex, 6, 0);
        DrawPrim(0x80, {{20, 20, 0.25f, 0.25f}, {70, 20, 0.25f, 0.25f}, {70, 70, 0.25f, 0.25f}, {20, 70, 0.25f, 0.25f}});
        Check(Near(Read().v[40 * kW + 40], SampleS(0.25), 1.0), "test rig: a plain quad of s = 0.25 reads the ramp at texel 16");
    }
    // ---- lines of widths 6 / 12 / 36 (1, 2, 6 pixels) at many angles, x-major and y-major, at sub-pixel positions
    const double kAngles[] = {0, 7, 20, 33, 41, 49, 60, 77, 90, 104, 125, 150, 173, 180, 200, 233, 271, 300, 330};
    for (unsigned w6 : {6u, 12u, 36u}) {
        int n = 0, bad = 0;
        for (double a : kAngles) {
            const double rad = a * M_PI / 180.0;
            const double cx = 47.31 + 0.37 * n, cy = 50.62 + 0.21 * n, len = 30.0 + n % 5;
            ++n;
            const double x0 = cx - len * std::cos(rad), y0 = cy - len * std::sin(rad), x1 = cx + len * std::cos(rad), y1 = cy + len * std::sin(rad);
            char name[96];
            std::snprintf(name, sizeof name, "line width %u/6 px at %g deg (%s-major)", w6, a, std::fabs(y1 - y0) > std::fabs(x1 - x0) ? "y" : "x");
            bad += !GeomCase(name, 0xA8, {{static_cast<float>(x0), static_cast<float>(y0), 0, 0}, {static_cast<float>(x1), static_cast<float>(y1), 0, 0}}, w6, false,
                             {LinePoly(x0, y0, x1, y1, w6)});
        }
        if (g_on) std::printf("width %u: %d of %d angles with a wrong pixel\n", w6, bad, n);
    }
    // ---- several lines in one draw (GX_LINES pairs) and a strip
    {
        const V a[] = {{10.3f, 12.2f, 0, 0}, {80.6f, 20.4f, 0, 0}, {15.5f, 30.1f, 0, 0}, {22.9f, 85.3f, 0, 0}, {40.2f, 40.7f, 0, 0}, {85.1f, 88.8f, 0, 0}};
        GeomCase("3 lines in one GX_LINES draw, width 12", 0xA8, std::vector<V>(a, a + 6), 12, false,
                 {LinePoly(a[0].x, a[0].y, a[1].x, a[1].y, 12), LinePoly(a[2].x, a[2].y, a[3].x, a[3].y, 12), LinePoly(a[4].x, a[4].y, a[5].x, a[5].y, 12)});
        const V s[] = {{12.4f, 20.3f, 0, 0}, {50.2f, 24.9f, 0, 0}, {58.8f, 70.6f, 0, 0}, {20.1f, 83.3f, 0, 0}};
        GeomCase("GX_LINESTRIP of 4 vertices, width 18", 0xB0, std::vector<V>(s, s + 4), 18, false,
                 {LinePoly(s[0].x, s[0].y, s[1].x, s[1].y, 18), LinePoly(s[1].x, s[1].y, s[2].x, s[2].y, 18), LinePoly(s[2].x, s[2].y, s[3].x, s[3].y, 18)});
    }
    // ---- points: sizes 6 / 27 (1 and 4.5 pixels) and larger, at sub-pixel positions
    for (unsigned s6 : {6u, 27u, 60u, 96u}) {
        const V p[] = {{20.37f, 21.62f, 0, 0}, {60.5f, 33.25f, 0, 0}, {44.81f, 70.11f, 0, 0}};
        char name[96];
        std::snprintf(name, sizeof name, "3 points of size %u/6 px", s6);
        GeomCase(name, 0xB8, std::vector<V>(p, p + 3), s6, true, {PointPoly(p[0].x, p[0].y, s6), PointPoly(p[1].x, p[1].y, s6), PointPoly(p[2].x, p[2].y, s6)});
    }

    if (g_on && g_scale == 1) {
        // ---- texture coordinate offsets. The ramp texture is 4 * x (or 4 * y): nearest sampling reads 4 * floor(64 s).
        // an x-major line of width 16 px (its far side is the bottom), s0 = 0.25, GX_TO_HALF on texcoord 0
        {
            MakeRamp(0);
            Clear();
            Setup(Fill::Tex, 96u | (4u << 16), 1u << 18);  // line width 96/6 = 16 px, GX_TO_HALF, line offsets of texcoord 0 enabled
            DrawPrim(0xA8, {{10.0f, 48.0f, 0.25f, 0.25f}, {86.0f, 48.0f, 0.25f, 0.25f}});
            const Image img = Read();
            int bad = 0;
            for (int y = 40; y < 56; ++y) bad += !Near(img.v[y * kW + 48], SampleS(0.25 + 0.5 * ((y + 0.5 - 40.0) / 16.0)), 5.0);
            Check(bad == 0, "line, GX_TO_HALF, x-major: s runs from 0.25 (top) to 0.75 (bottom) across the width");
            Check(img.v[39 * kW + 48] < 128 && img.v[56 * kW + 48] < 128, "line texture offsets: nothing outside the width");
            Clear();
            Setup(Fill::Tex, 96u | (4u << 16), 0);  // the same without GXEnableTexOffsets
            DrawPrim(0xA8, {{10.0f, 48.0f, 0.25f, 0.25f}, {86.0f, 48.0f, 0.25f, 0.25f}});
            const Image img2 = Read();
            int flat = 0;
            for (int y = 41; y < 55; ++y) flat += !Near(img2.v[y * kW + 48], SampleS(0.25), 5.0);
            Check(flat == 0, "line, GX_TO_HALF but texcoord offsets not enabled: s stays 0.25");
        }
        // a y-major line (its far side is the right)
        {
            Clear();
            Setup(Fill::Tex, 96u | (3u << 16), 1u << 18);  // GX_TO_FOURTH
            DrawPrim(0xA8, {{48.0f, 10.0f, 0.25f, 0.25f}, {48.0f, 86.0f, 0.25f, 0.25f}});
            const Image img = Read();
            int bad = 0;
            for (int x = 40; x < 56; ++x) bad += !Near(img.v[48 * kW + x], SampleS(0.25 + 0.25 * ((x + 0.5 - 40.0) / 16.0)), 5.0);
            Check(bad == 0, "line, GX_TO_FOURTH, y-major: s runs from 0.25 (left) to 0.5 (right) across the width");
        }
        // a point: s grows to the right, t downward
        for (int ramp = 0; ramp < 2; ++ramp) {
            MakeRamp(ramp);
            Clear();
            Setup(Fill::Tex, (120u << 8) | (4u << 19), 1u << 19);  // point size 120/6 = 20 px, GX_TO_HALF, point offsets of texcoord 0 enabled
            DrawPrim(0xB8, {{48.0f, 48.0f, 0.25f, 0.25f}});
            const Image img = Read();
            int bad = 0;
            for (int k = 38; k < 58; ++k) {
                const double want = SampleS(0.25 + 0.5 * ((k + 0.5 - 38.0) / 20.0));
                bad += !Near(ramp == 0 ? img.v[48 * kW + k] : img.v[k * kW + 48], want, 5.0);
            }
            char name[110];
            std::snprintf(name, sizeof name, "point, GX_TO_HALF: %s runs from 0.25 to 0.75 across the 20 px square", ramp == 0 ? "s (left to right)" : "t (top to bottom)");
            Check(bad == 0, name);
        }
        MakeRamp(0);
    }
    if (g_on) {
        // ---- a perspective projection with one end point behind the eye: clipped like any line, the pixels in front are the GX line
        {
            Clear();
            Setup(Fill::White, 36, 0);
            const double n = 1.0, f = 100.0;  // (near plane at 1: the cut end is inside the picture)
            const uint32_t proj[] = {f2u(1), 0, f2u(1), 0, f2u(static_cast<float>(-n / (f - n))), f2u(static_cast<float>(-f * n / (f - n))), 0};
            WriteXF(0x1020, 7, proj);
            // eye (-0.4, 0.1, -2): NDC (-0.2, 0.05), pixel (38.4, 45.6); the other end point (3, 0, +1) is behind the eye
            const float v[] = {-0.4f, 0.1f, -2.0f, 0, 3.0f, 0.0f, 1.0f, 0};
            DrawDecoded(0xA8, 0, reinterpret_cast<const uint8_t*>(v), 2);
            const Image img = Read();
            // The visible part runs from A to where the segment crosses the near plane (w = 1, at t = 1/3: eye (0.7333, 0.0667, -1), NDC the same).
            const Verdict r = Judge(img, {LinePoly(38.4, 45.6, (0.73333333 + 1.0) * 48.0, 48.0 - 0.06666667 * 48.0, 36)});
            const int lit = r.covered, outside = r.mismatch;
            std::printf("clipped line: %d lit pixels, %d expected, %d mismatching\n", lit, r.expected, outside);
            Check(lit > 100 && outside == 0, "line with an end point behind the eye: drawn, clipped, the GX parallelogram in front");
        }

        // ---- end points on different palette matrices (matrix index in the position's w): both end points through their own matrix
        {
            Clear();
            Setup(Fill::White, 12, 0);
            // matrix 3 (XF address 12): translate by +0.25 in x and -0.25 in y
            const uint32_t m3[] = {f2u(1), 0, 0, f2u(0.25f), 0, f2u(1), 0, f2u(-0.25f), 0, 0, f2u(1), 0};
            WriteXF(12, 12, m3);
            g.vcd[0] = 1;
            ++g.vcdGen;
            const float v[] = {Nx(20.0f), Ny(24.0f), -0.5f, 0, Nx(40.0f), Ny(60.0f), -0.5f, 3};
            DrawDecoded(0xA8, 0, reinterpret_cast<const uint8_t*>(v), 2);
            g.vcd[0] = 0;
            ++g.vcdGen;
            const Image img = Read();
            // matrix 3 moves end point 1 by eye (+0.25, -0.25) = NDC (+0.25, -0.25) = pixels (+12, +12): from (40, 60) to (52, 72)
            const Verdict r = Judge(img, {LinePoly(20.0, 24.0, 52.0, 72.0, 12)});
            char msg[160];
            std::snprintf(msg, sizeof msg, "skinned line, end points on matrix 0 and 3: %d mismatching pixels (%d lit, %d expected)", r.mismatch, r.covered, r.expected);
            Check(r.mismatch == 0 && r.expected > 0, msg);
        }
    }
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
