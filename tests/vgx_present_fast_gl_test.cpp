// Picture-quality test of the present (GXV_PRESENT_FAST, GXV_EDGE_AA, GXV_SHARPEN; vgx_backend_gl.cpp "the fast present"), on the desktop's
// GL (Mesa llvmpipe, surfaceless EGL): the real back end presents a synthetic picture, captured as the console shows it
// (GXV_CAP_PRESENT=960x544, GXV_CAP_AB=1: the present without edge smoothing above the one with it).
// The picture is drawn into the EFB texture as the GPU renders (one point sample per render pixel, no smoothing) from an analytic scene
// in [0,1]^2, and the same scene, averaged over 8x8 samples per screen pixel, is the ideal 960x544 picture ("truth"). Regions:
//   - 8 hard edges at 2, 8, 20, 45, 70, 88, 135 and 160 degrees (both diagonals; tiles in the top half), dark against bright;
//   - a thin line (1.3 render px wide) at 12 degrees;
//   - a smooth texture (sinusoids, periods 7-11 render px: what the unsharp mask is for), a flat area and a gradient;
//   - a HUD rectangle (CopyDispCmd::hud) holding 1-px strokes, which must come out exactly as the present without edge smoothing.
// Printed per mode and rung: RMS error against the truth in the edge bands (per angle) and in the texture; the worst change of a flat
// pixel against the panel without smoothing; the HUD's differing pixels; the steepest step across the edges (as tests/vgx_edgeaa_gl_test).
// Writes the two panels as PPM (<out>/<tag>_r<rung>_plain.ppm, _edge.ppm) and the truth (truth.ppm). Exit status 0 when the hard checks
// pass (HUD exact, flat pixels within 1 level, edges smoothed when edge smoothing is on); the error figures are for the report.
//   tools/test_vgx_present_fast_gl.sh [output-dir]
#include "vgx_autoq.h"
#include "vgx_cmd.h"

#include <GLES2/gl2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vgx {
bool LogEnabled() { return std::getenv("GXV_LOG") && std::atoi(std::getenv("GXV_LOG")) != 0; }
} // namespace vgx

using namespace vgx;

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}
void Push(CmdBuf& b, CmdType t, uint32_t arg) { b.cmds.push_back(Cmd{t, arg}); }
// The HUD rectangle in EFB units (640x456) and in the scene's [0,1]^2.
const EfbRect kHud{500, 360, 120, 80};
constexpr double kHx0 = 500.0 / 640, kHx1 = 620.0 / 640, kHy0 = 360.0 / 456, kHy1 = 440.0 / 456;
constexpr double kPi = 3.14159265358979323846;
const double kAngles[8] = {2, 8, 20, 45, 70, 88, 135, 160};

struct Rgb { double r, g, b; };
// The scene. Top half: 4 x 2 tiles, each a hard edge through its centre. Bottom half: texture | flat + gradient | thin line | HUD.
// tile(): which region a point is in (-1 none).
int EdgeTile(double u, double v) {
    if (v >= 0.5) return -1;
    return static_cast<int>(v / 0.25) * 4 + std::min(3, static_cast<int>(u / 0.25));
}
// signed distance (in scene units, x scaled to the screen's 960/544 aspect) from tile t's edge line
double EdgeDist(int t, double u, double v) {
    const double cx = (t % 4) * 0.25 + 0.125, cy = (t / 4) * 0.25 + 0.125;
    const double a = kAngles[t] * kPi / 180.0;
    const double dx = (u - cx) * 960.0, dy = (v - cy) * 544.0; // (screen pixels)
    return -std::sin(a) * dx + std::cos(a) * dy; // (the line's direction (cos a, sin a); y down)
}
Rgb Scene(double u, double v) {
    const int t = EdgeTile(u, v);
    if (t >= 0) {
        const bool side = EdgeDist(t, u, v) > 0;
        if (t % 2) return side ? Rgb{0.85, 0.80, 0.70} : Rgb{0.10, 0.12, 0.20};
        return side ? Rgb{0.20, 0.55, 0.85} : Rgb{0.90, 0.35, 0.15};
    }
    if (u < 0.25) { // texture
        const double f = 0.5 + 0.14 * std::sin(2 * kPi * u * 960.0 / 9.5) * std::cos(2 * kPi * v * 544.0 / 11.0) +
                         0.08 * std::sin(2 * kPi * (u * 960.0 + v * 544.0) / 7.5);
        return Rgb{f, f * 0.9 + 0.05, f * 0.8 + 0.1};
    }
    if (u < 0.5) { // flat (left) and gradient (right)
        if (u < 0.375) return Rgb{0.45, 0.50, 0.40};
        const double g = (v - 0.5) * 2.0;
        return Rgb{0.2 + 0.6 * g, 0.3 + 0.4 * g, 0.5};
    }
    if (u < 0.75) { // thin line at 12 degrees, 1.3 render px (720 wide) ~ 1.73 screen px
        const double a = 12.0 * kPi / 180.0;
        const double dx = (u - 0.625) * 960.0, dy = (v - 0.75) * 544.0;
        const double d = std::fabs(-std::sin(a) * dx + std::cos(a) * dy);
        return d < 0.87 ? Rgb{0.95, 0.95, 0.95} : Rgb{0.25, 0.25, 0.30};
    }
    if (u >= kHx0 && u < kHx1 && v >= kHy0 && v < kHy1) { // HUD: 1-px strokes (in 960 screen units: 1 render px of 720 = 1.33)
        const int px = static_cast<int>((u - kHx0) * 960.0 / 1.3333), py = static_cast<int>((v - kHy0) * 544.0 / 1.3333);
        const bool on = (px % 4 == 1) || (py % 5 == 2 && px % 7 < 5);
        return on ? Rgb{1.0, 1.0, 0.9} : Rgb{0.05, 0.05, 0.2};
    }
    return Rgb{0.3, 0.32, 0.35};
}
uint8_t Q(double x) { return static_cast<uint8_t>(std::clamp(std::lround(x * 255.0), 0L, 255L)); }

// The EFB texture filled with the scene, one sample at each render pixel's centre (the GPU's rasterisation, no smoothing).
void FillEfb(int rw, int rh) {
    const float black[4] = {0, 0, 0, 1};
    backend::DebugDrawEfbTriangle(black); // (binds the EFB's framebuffer)
    GLint tex = 0, prev = 0, align = 4;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &tex);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    std::vector<uint8_t> px(static_cast<size_t>(rw) * rh * 4);
    for (int y = 0; y < rh; ++y)
        for (int x = 0; x < rw; ++x) {
            const Rgb c = Scene((x + 0.5) / rw, (y + 0.5) / rh);
            uint8_t* p = &px[(static_cast<size_t>(y) * rw + x) * 4];
            p[0] = Q(c.r); p[1] = Q(c.g); p[2] = Q(c.b); p[3] = 255;
        }
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(tex));
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, px.data()); // (row 0: the picture's top row, as the EFB)
    glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev));
}

void Present(CmdBuf& b, bool clear, bool edge) {
    CopyDispCmd d;
    d.src = EfbRect{0, 0, 640, 456};
    d.clear.enable = clear;
    d.clear.rgba[0] = d.clear.rgba[1] = d.clear.rgba[2] = 0.5f;
    if (edge) {
        d.edge = true; d.hud[0] = kHud; d.hudN = 1;
        // PF_HUDS=1 (GXV_EDGE_AA_HUDCUT's test, tools/test_vgx_present_fast_gl.sh): four more HUD rectangles, overlapping, touching the
        // screen's edges and each other, a full-width strip and a full-height one (the pictures are then compared run against run)
        if (std::getenv("PF_HUDS") && std::atoi(std::getenv("PF_HUDS")) == 1) {
            const EfbRect more[4] = {{40, 40, 100, 60}, {120, 70, 81, 121}, {600, 0, 40, 456}, {0, 431, 640, 25}};
            for (const EfbRect& r : more) d.hud[d.hudN++] = r;
        }
    }
    b.copyDisp.push_back(d);
    Push(b, CMD_COPY_DISP, static_cast<uint32_t>(b.copyDisp.size() - 1));
    Push(b, CMD_PRESENT, 0);
}

constexpr int W = 960, H = 544;
std::vector<double> g_truth; // W x H x 3, top row first
void MakeTruth(const std::string& out) {
    g_truth.assign(static_cast<size_t>(W) * H * 3, 0.0);
    std::vector<uint8_t> ppm;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double r = 0, g = 0, b = 0;
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) {
                    const Rgb c = Scene((x + (i + 0.5) / 8.0) / W, (y + (j + 0.5) / 8.0) / H);
                    r += c.r; g += c.g; b += c.b;
                }
            double* t = &g_truth[(static_cast<size_t>(y) * W + x) * 3];
            t[0] = r / 64 * 255; t[1] = g / 64 * 255; t[2] = b / 64 * 255;
        }
    FILE* f = std::fopen((out + "/truth.ppm").c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (size_t i = 0; i < g_truth.size(); ++i) std::fputc(static_cast<int>(std::lround(g_truth[i])), f);
    std::fclose(f);
}
void WritePanel(const std::string& path, const std::vector<uint8_t>& px, int half) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) std::fwrite(&px[(static_cast<size_t>(half * H + y) * W + x) * 4], 1, 3, f);
    std::fclose(f);
}
inline int Lum(const uint8_t* p) { return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000; }

void Run(int rung, const std::string& out, const std::string& tag, bool edgeOn) {
    // (through another rung first: with GXV_AUTOQ off the first picture is the EFB's own size, while the rung already reads 2)
    for (int r : {rung == 3 ? 4 : 3, rung}) {
        backend::ForceQualityRung(r);
        static uint64_t frame = 1000;
        CmdBuf a; a.frame = frame++; Present(a, true, false); backend::Execute(a); // (the rung is applied at the next frame's start)
        CmdBuf a2; a2.frame = frame++; Present(a2, true, false); backend::Execute(a2);
    }
    int rw = 0, rh = 0, rs = 0;
    backend::QualityRungSize(rw, rh, rs);
    FillEfb(rw, rh);
    CmdBuf b;
    b.frame = 5000 + rung;
    Present(b, false, true);
    backend::RequestCapture(true);
    backend::Execute(b);
    backend::RequestCapture(false);
    int cw = 0, ch = 0;
    const std::vector<uint8_t> px = backend::LastDisplayCopy(cw, ch);
    std::printf("%s, rung %d (%dx%d shown at 960x544):\n", tag.c_str(), rung, rw, rh);
    if (cw != W || ch != 2 * H) { std::printf("  [FAIL] capture is %dx%d\n", cw, ch); ++g_fail; return; }
    WritePanel(out + "/" + tag + "_r" + std::to_string(rung) + "_plain.ppm", px, 0);
    WritePanel(out + "/" + tag + "_r" + std::to_string(rung) + "_edge.ppm", px, 1);
    auto at = [&](int half, int x, int y) { return &px[(static_cast<size_t>(half * H + y) * W + x) * 4]; };
    auto err2 = [&](int half, int x, int y) {
        const uint8_t* p = at(half, x, y);
        const double* t = &g_truth[(static_cast<size_t>(y) * W + x) * 3];
        double e = 0;
        for (int k = 0; k < 3; ++k) e += (p[k] - t[k]) * (p[k] - t[k]);
        return e / 3.0;
    };
    // edges: per tile, pixels within 2.5 screen px of the line, 8 px off the tile's borders
    char line[600];
    double sumP = 0, sumE = 0; long nAll = 0;
    std::string rowP = "    edge RMS plain:", rowE = "    edge RMS edge: ";
    int worse = 0;
    for (int t = 0; t < 8; ++t) {
        double sp = 0, se = 0; long n = 0;
        const int x0 = (t % 4) * W / 4 + 8, x1 = (t % 4 + 1) * W / 4 - 8, y0 = (t / 4) * H / 4 + 8, y1 = (t / 4 + 1) * H / 4 - 8;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                if (std::fabs(EdgeDist(t, (x + 0.5) / W, (y + 0.5) / H)) > 2.5) continue;
                sp += err2(0, x, y); se += err2(1, x, y); ++n;
            }
        const double rp = std::sqrt(sp / std::max(1L, n)), re = std::sqrt(se / std::max(1L, n));
        char cell[64];
        std::snprintf(cell, sizeof cell, " %3.0f:%5.1f", kAngles[t], rp); rowP += cell;
        std::snprintf(cell, sizeof cell, " %3.0f:%5.1f", kAngles[t], re); rowE += cell;
        sumP += sp; sumE += se; nAll += n;
        worse += re > rp + 1.0; // (1 level: GXV_PRESENT_FAST=1's panel without smoothing has the 3-read mask, the smoothed one the 5-read mask)
    }
    const double edgeP = std::sqrt(sumP / nAll), edgeE = std::sqrt(sumE / nAll);
    std::printf("%s\n%s\n", rowP.c_str(), rowE.c_str());
    // thin line: band 3 px around it
    double lp = 0, le = 0; long ln = 0;
    for (int y = H / 2 + 8; y < 3 * H / 4 + 60 && y < H - 8; ++y)
        for (int x = W / 2 + 8; x < 3 * W / 4 - 8; ++x) {
            const double a = 12.0 * kPi / 180.0, dx = ((x + 0.5) / W - 0.625) * 960.0, dy = ((y + 0.5) / H - 0.75) * 544.0;
            if (std::fabs(-std::sin(a) * dx + std::cos(a) * dy) > 3.0) continue;
            lp += err2(0, x, y); le += err2(1, x, y); ++ln;
        }
    // texture: RMS against the truth, and its contrast (std of green) against the truth's
    double tp = 0, te = 0; long tn = 0;
    for (int y = H / 2 + 8; y < H - 8; ++y)
        for (int x = 8; x < W / 4 - 8; ++x) { tp += err2(0, x, y); te += err2(1, x, y); ++tn; }
    // flat: worst change edge vs plain, and plain vs truth
    int flatWorst = 0, flatTruth = 0;
    for (int y = H / 2 + 8; y < H - 8; ++y)
        for (int x = W / 4 + 8; x < 3 * W / 8 - 8; ++x) {
            for (int k = 0; k < 3; ++k) {
                flatWorst = std::max(flatWorst, std::abs(at(0, x, y)[k] - at(1, x, y)[k]));
                flatTruth = std::max(flatTruth, static_cast<int>(std::lround(std::fabs(at(1, x, y)[k] - g_truth[(static_cast<size_t>(y) * W + x) * 3 + k]))));
            }
        }
    // gradient: worst change edge vs plain
    int gradWorst = 0;
    for (int y = H / 2 + 8; y < H - 8; ++y)
        for (int x = 3 * W / 8 + 8; x < W / 2 - 8; ++x)
            for (int k = 0; k < 3; ++k) gradWorst = std::max(gradWorst, std::abs(at(0, x, y)[k] - at(1, x, y)[k]));
    // HUD: the screen rectangle, 2 px inside
    const int hx0 = static_cast<int>(std::ceil(kHx0 * W)) + 2, hx1 = static_cast<int>(kHx1 * W) - 2;
    const int hy0 = static_cast<int>(std::ceil(kHy0 * H)) + 2, hy1 = static_cast<int>(kHy1 * H) - 2;
    long hudDiff = 0;
    for (int y = hy0; y < hy1; ++y)
        for (int x = hx0; x < hx1; ++x) hudDiff += std::memcmp(at(0, x, y), at(1, x, y), 3) != 0;
    // the steepest luma step per row across the 45 and 135 degree edges (tiles 3, 6), as the edge-aa test
    double sb = 0, sa = 0; int rows = 0;
    for (int t : {3, 6}) {
        const int x0 = (t % 4) * W / 4 + 8, x1 = (t % 4 + 1) * W / 4 - 9, y0 = (t / 4) * H / 4 + 8, y1 = (t / 4 + 1) * H / 4 - 8;
        for (int y = y0; y < y1; ++y) {
            int mb = 0, ma = 0;
            for (int x = x0; x < x1; ++x) {
                mb = std::max(mb, std::abs(Lum(at(0, x + 1, y)) - Lum(at(0, x, y))));
                ma = std::max(ma, std::abs(Lum(at(1, x + 1, y)) - Lum(at(1, x, y))));
            }
            sb += mb; sa += ma; ++rows;
        }
    }
    std::printf("    all edges RMS %.2f -> %.2f; thin line RMS %.2f -> %.2f; texture RMS %.2f -> %.2f; steepest step at 45/135 deg %.1f -> %.1f\n",
                edgeP, edgeE, std::sqrt(lp / std::max(1L, ln)), std::sqrt(le / std::max(1L, ln)), std::sqrt(tp / tn), std::sqrt(te / tn),
                sb / std::max(1, rows), sa / std::max(1, rows));
    std::snprintf(line, sizeof line, "HUD: %ld pixels differ from the present without smoothing (expected 0)", hudDiff);
    Check(hudDiff == 0, line);
    std::snprintf(line, sizeof line, "flat area: worst change %d (expected <= 1), worst against the truth %d; gradient: worst change %d (expected <= 2)",
                  flatWorst, flatTruth, gradWorst);
    Check(flatWorst <= 1 && gradWorst <= 2, line);
    if (edgeOn) {
        std::snprintf(line, sizeof line, "edges: RMS against the ideal picture lower with smoothing (%.2f -> %.2f), no angle worse by > 1 level (%d)", edgeP, edgeE, worse);
        Check(edgeE < edgeP && worse == 0, line);
    }
}
} // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    const char* tagEnv = std::getenv("PF_TAG");
    const std::string tag = tagEnv ? tagEnv : "run";
    if (!host::CreateContext()) { std::printf("no GL context\n"); return 2; }
    if (!backend::Init(0, 0)) { std::printf("back end Init failed\n"); return 2; }
    {
        CmdBuf b;
        b.frame = 1;
        b.targets.push_back(SetTargetCmd{640, 456});
        Push(b, CMD_SET_TARGET, 0);
        Present(b, true, false);
        backend::Execute(b);
    }
    MakeTruth(out);
    const bool edgeOn = std::getenv("GXV_EDGE_AA") && std::atoi(std::getenv("GXV_EDGE_AA")) == 1;
    Run(2, out, tag, edgeOn); // (GXV_WIDE_RUNGS=1: 720x404, the console's lit-hub picture)
    Run(6, out, tag, edgeOn); // 960x544 1:1
    uint64_t p = 0, s = 0, h = 0;
    backend::PresentFastCounters(p, s, h);
    std::printf("  fast presents %llu (with edges %llu, HUD rectangles %llu)\n", (unsigned long long)p, (unsigned long long)s, (unsigned long long)h);
    std::printf("%s (%d failed)\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed", g_fail);
    return g_fail ? 1 : 0;
}
