// Picture test of the upscaler (GXV_UPSCALE; vgx_backend_gl.cpp "the upscaler") on the desktop's GL (Mesa llvmpipe, surfaceless EGL): the
// real back end presents a render-size picture, captured as the console shows it (GXV_CAP_PRESENT=960x544, GXV_CAP_AB=1: panel 0 = the
// present without edge smoothing (PresentSharpened), panel 1 = the full present (PresentSharpened, then PresentEdgeAA: today's edge
// smoothing, or the upscaler with GXV_UPSCALE on)).
// The EFB is filled as the GPU renders it (one sample per render pixel) from
//   - PU_SCENE=syn (default): tests/vgx_present_fast_gl_test.cpp's analytic scene (hard edges at 8 angles, a thin line, a texture,
//     a flat area and a gradient, a HUD of 1-px strokes);
//   - PU_SCENE=<file.ppm>: a picture taken as the scene (a console screenshot), point-sampled at the render size (nearest pick);
// at each rung of PU_RUNGS (default "2 3 4 5 6": with GXV_WIDE_RUNGS=1 720x404 784x440 848x480 896x512 960x544), or once at
// PU_SIZE=<W>x<H> (any render size, e.g. 832x472: the back end's fixed size, Init(W, H)).
// Writes <out>/<tag>_<W>x<H>_p0.ppm and _p1.ppm (960x544 each) and prints the counters; the pictures are scored by
// tools/upscale_score.py (PSNR / SSIM against the 960x544 reference, bilinear and today's present). Exit status 0 when the hard checks
// pass: a picture shown at 960x544 1:1 is not touched by the upscaler (the counters; byte-identity against GXV_UPSCALE=0 is checked by
// tools/test_vgx_upscale_gl.sh), and every enlarged picture went through it when GXV_UPSCALE is on.
//   tools/test_vgx_upscale_gl.sh [output-dir]
#include "vgx_autoq.h"
#include "vgx_cmd.h"

#include <GLES2/gl2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace vgx {
bool LogEnabled() { return std::getenv("GXV_LOG") && std::atoi(std::getenv("GXV_LOG")) != 0; }
bool VtxPackOn() { return false; } // (the packed-vertex counters live in vgx_dlcache.cpp, which this test does not link)
void VtxPackCounters(uint64_t*) {}
void VtxPackDrawCounters(uint64_t*) {}
} // namespace vgx

using namespace vgx;

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}
void Push(CmdBuf& b, CmdType t, uint32_t arg) { b.cmds.push_back(Cmd{t, arg}); }
const EfbRect kHud{500, 360, 120, 80};
constexpr double kHx0 = 500.0 / 640, kHx1 = 620.0 / 640, kHy0 = 360.0 / 456, kHy1 = 440.0 / 456;
constexpr double kPi = 3.14159265358979323846;
const double kAngles[8] = {2, 8, 20, 45, 70, 88, 135, 160};

struct Rgb { double r, g, b; };
// (the scene of tests/vgx_present_fast_gl_test.cpp, unchanged)
int EdgeTile(double u, double v) {
    if (v >= 0.5) return -1;
    return static_cast<int>(v / 0.25) * 4 + std::min(3, static_cast<int>(u / 0.25));
}
double EdgeDist(int t, double u, double v) {
    const double cx = (t % 4) * 0.25 + 0.125, cy = (t / 4) * 0.25 + 0.125;
    const double a = kAngles[t] * kPi / 180.0;
    const double dx = (u - cx) * 960.0, dy = (v - cy) * 544.0;
    return -std::sin(a) * dx + std::cos(a) * dy;
}
Rgb Scene(double u, double v) {
    const int t = EdgeTile(u, v);
    if (t >= 0) {
        const bool side = EdgeDist(t, u, v) > 0;
        if (t % 2) return side ? Rgb{0.85, 0.80, 0.70} : Rgb{0.10, 0.12, 0.20};
        return side ? Rgb{0.20, 0.55, 0.85} : Rgb{0.90, 0.35, 0.15};
    }
    if (u < 0.25) {
        const double f = 0.5 + 0.14 * std::sin(2 * kPi * u * 960.0 / 9.5) * std::cos(2 * kPi * v * 544.0 / 11.0) +
                         0.08 * std::sin(2 * kPi * (u * 960.0 + v * 544.0) / 7.5);
        return Rgb{f, f * 0.9 + 0.05, f * 0.8 + 0.1};
    }
    if (u < 0.5) {
        if (u < 0.375) return Rgb{0.45, 0.50, 0.40};
        const double g = (v - 0.5) * 2.0;
        return Rgb{0.2 + 0.6 * g, 0.3 + 0.4 * g, 0.5};
    }
    if (u < 0.75) {
        const double a = 12.0 * kPi / 180.0;
        const double dx = (u - 0.625) * 960.0, dy = (v - 0.75) * 544.0;
        const double d = std::fabs(-std::sin(a) * dx + std::cos(a) * dy);
        return d < 0.87 ? Rgb{0.95, 0.95, 0.95} : Rgb{0.25, 0.25, 0.30};
    }
    if (u >= kHx0 && u < kHx1 && v >= kHy0 && v < kHy1) {
        const int px = static_cast<int>((u - kHx0) * 960.0 / 1.3333), py = static_cast<int>((v - kHy0) * 544.0 / 1.3333);
        const bool on = (px % 4 == 1) || (py % 5 == 2 && px % 7 < 5);
        return on ? Rgb{1.0, 1.0, 0.9} : Rgb{0.05, 0.05, 0.2};
    }
    return Rgb{0.3, 0.32, 0.35};
}
uint8_t Q(double x) { return static_cast<uint8_t>(std::clamp(std::lround(x * 255.0), 0L, 255L)); }

// PU_SCENE=<file.ppm>: a binary PPM (P6, 8 bits)
struct Picture { int w = 0, h = 0; std::vector<uint8_t> rgb; };
bool ReadPpm(const char* path, Picture& p) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char magic[3] = {};
    int maxv = 0;
    bool ok = std::fscanf(f, "%2s %d %d %d", magic, &p.w, &p.h, &maxv) == 4 && std::strcmp(magic, "P6") == 0 && maxv == 255 && p.w > 0 && p.h > 0;
    if (ok) {
        std::fgetc(f);
        p.rgb.resize(static_cast<size_t>(p.w) * p.h * 3);
        ok = std::fread(p.rgb.data(), 1, p.rgb.size(), f) == p.rgb.size();
    }
    std::fclose(f);
    return ok;
}
Picture g_scene; // (empty: the analytic scene)

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
            uint8_t* p = &px[(static_cast<size_t>(y) * rw + x) * 4];
            if (g_scene.w) { // (nearest pick: the render pixel's centre in the scene picture)
                const int sx = std::min(g_scene.w - 1, static_cast<int>((x + 0.5) * g_scene.w / rw));
                const int sy = std::min(g_scene.h - 1, static_cast<int>((y + 0.5) * g_scene.h / rh));
                std::memcpy(p, &g_scene.rgb[(static_cast<size_t>(sy) * g_scene.w + sx) * 3], 3);
            } else {
                const Rgb c = Scene((x + 0.5) / rw, (y + 0.5) / rh);
                p[0] = Q(c.r); p[1] = Q(c.g); p[2] = Q(c.b);
            }
            p[3] = 255;
        }
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(tex));
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev));
}

void Present(CmdBuf& b, bool clear, bool edge) {
    CopyDispCmd d;
    d.src = EfbRect{0, 0, 640, 456};
    d.clear.enable = clear;
    d.clear.rgba[0] = d.clear.rgba[1] = d.clear.rgba[2] = 0.5f;
    if (edge && !g_scene.w) { d.edge = true; d.hud[0] = kHud; d.hudN = 1; }
    else if (edge) d.edge = true;
    b.copyDisp.push_back(d);
    Push(b, CMD_COPY_DISP, static_cast<uint32_t>(b.copyDisp.size() - 1));
    Push(b, CMD_PRESENT, 0);
}

constexpr int W = 960, H = 544;
void WritePanel(const std::string& path, const std::vector<uint8_t>& px, int half) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) std::fwrite(&px[(static_cast<size_t>(half * H + y) * W + x) * 4], 1, 3, f);
    std::fclose(f);
}

uint64_t g_frame = 1000;
// One captured present of the EFB as it is now (rung rw x rh); returns whether it was captured.
bool Capture(const std::string& out, const std::string& tag) {
    int rw = 0, rh = 0, rs = 0;
    backend::QualityRungSize(rw, rh, rs);
    FillEfb(rw, rh);
    uint64_t u0 = 0, sp = 0, st = 0;
    backend::UpscaleCounters(u0, sp, st);
    CmdBuf b;
    b.frame = g_frame++;
    Present(b, false, true);
    backend::RequestCapture(true);
    backend::Execute(b);
    backend::RequestCapture(false);
    uint64_t u1 = 0;
    backend::UpscaleCounters(u1, sp, st);
    int cw = 0, ch = 0;
    const std::vector<uint8_t> px = backend::LastDisplayCopy(cw, ch);
    if (cw != W || ch != 2 * H) { std::printf("  [FAIL] capture is %dx%d\n", cw, ch); ++g_fail; return false; }
    const std::string name = tag + "_" + std::to_string(rw) + "x" + std::to_string(rh);
    WritePanel(out + "/" + name + "_p0.ppm", px, 0);
    WritePanel(out + "/" + name + "_p1.ppm", px, 1);
    const int mode = std::getenv("GXV_UPSCALE") ? std::atoi(std::getenv("GXV_UPSCALE")) : 0;
    const bool enlarged = rw < W || rh < H;
    const uint64_t drawn = u1 - u0;
    char line[200];
    std::snprintf(line, sizeof line, "%s: %dx%d shown at 960x544, upscaler drawn %llu time(s) (expected %d)", name.c_str(), rw, rh,
                  static_cast<unsigned long long>(drawn), mode == 2 || (mode == 1 && enlarged) ? 1 : 0);
    if (std::getenv("GXV_UPSCALE_SPLIT") && std::atoi(std::getenv("GXV_UPSCALE_SPLIT")) != 0) std::printf("  %s (split: drawn per panel)\n", line);
    else Check(drawn == static_cast<uint64_t>(mode == 2 || (mode == 1 && enlarged) ? 1 : 0), line);
    return true;
}
} // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    const char* tagEnv = std::getenv("PU_TAG");
    const std::string tag = tagEnv ? tagEnv : "run";
    if (const char* sc = std::getenv("PU_SCENE"); sc && std::strcmp(sc, "syn") != 0 && *sc) {
        if (!ReadPpm(sc, g_scene)) { std::printf("cannot read %s\n", sc); return 2; }
    }
    int fw = 0, fh = 0;
    if (const char* sz = std::getenv("PU_SIZE")) std::sscanf(sz, "%dx%d", &fw, &fh);
    if (!host::CreateContext()) { std::printf("no GL context\n"); return 2; }
    if (!backend::Init(fw, fh)) { std::printf("back end Init failed\n"); return 2; }
    {
        CmdBuf b;
        b.frame = 1;
        b.targets.push_back(SetTargetCmd{640, 456});
        Push(b, CMD_SET_TARGET, 0);
        Present(b, true, false);
        backend::Execute(b);
    }
    if (fw > 0) {
        CmdBuf a; a.frame = g_frame++; Present(a, true, false); backend::Execute(a);
        Capture(out, tag);
    } else {
        std::vector<int> rungs;
        std::istringstream in(std::getenv("PU_RUNGS") ? std::getenv("PU_RUNGS") : "2 3 4 5 6");
        for (int r; in >> r;) rungs.push_back(r);
        for (int rung : rungs) {
            // (through another rung first: with GXV_AUTOQ off the first picture is the EFB's own size, while the rung already reads it)
            for (int r : {rung == 3 ? 4 : 3, rung}) {
                backend::ForceQualityRung(r);
                CmdBuf a; a.frame = g_frame++; Present(a, true, false); backend::Execute(a);
                CmdBuf a2; a2.frame = g_frame++; Present(a2, true, false); backend::Execute(a2);
            }
            Capture(out, tag);
        }
    }
    uint64_t p = 0, s = 0, st = 0;
    backend::UpscaleCounters(p, s, st);
    std::printf("  upscaled presents %llu (left half only %llu, left to the plain present by the edge step %llu)\n", (unsigned long long)p,
                (unsigned long long)s, (unsigned long long)st);
    std::printf("%s (%d failed)\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed", g_fail);
    return g_fail ? 1 : 0;
}
