// finish44: GXV_HRW_WARP_SHARP (hrwfeel36, HRW-DESIGN S3a), the PRODUCTION depth-warp program (kVsWarp + D41Fragment, extracted from vgx_backend_gl.cpp by
// tools/test_hrw_warp_sharp.sh) drawn on Mesa and compared pixel by pixel with the arithmetic the design gives (port::hrw41::SharpCoord / SharpWeight, which
// tests/hrw41_test.cpp proves monotone, continuous and fixed at texel centres and edges). tests/hrw41_test.cpp tests the arithmetic only; this is the shader.
//   1. GXV_HRW_WARP_SHARP=40 (k = 4): every pixel is the bilinear mix of its two (four) texels with the SHARPENED weights, for sub-texel and whole-texel shifts;
//   2. where the shift's fraction is outside the blend zone (width 1 / k round the half-texel point) the pixel is a whole source texel: no blend at all;
//   3. at a half-texel shift it is the even mix (the zone's middle: nothing jumps);
//   4. GXV_HRW_WARP_SHARP=10 (k = 1) draws what the switch-off program draws (the identity), and =0 gives the program text without the two uniforms;
//   5. a still pair (no shift) is the source exactly at every k;
//   6. the picture keeps more fine detail than the plain bilinear read (the soft second picture the switch is for).
// The depth mask is one constant texel and u_z = 0: the read position is the rotation-only one, so the model is exact; the depth arithmetic before it is
// c185_depth_edges' subject. Synthetic raster test only (Mesa llvmpipe: its bilinear weights have 8 fractional bits, hence the tolerance of 3 / 255).
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "port/hrw41.h"
static int g_sharp = 0, g_depth41 = 1;
static int WarpSharpEnv() { return g_sharp; }
static int Depth41Env() { return g_depth41; }
static bool Depth41EdgeEnv() { return false; }
static bool Depth41DebugEnv() { return false; }
#include "sharp_sources.inc"

namespace {
constexpr int W = 96, H = 64;
using Pixel = std::array<unsigned char, 4>;
using Image = std::vector<Pixel>;
int g_fail = 0, g_checks = 0;
void check(bool ok, const char* why) { ++g_checks; if (!ok) { std::fprintf(stderr, "FAIL %s (GL %x)\n", why, glGetError()); ++g_fail; } }
void must(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FATAL %s (GL %x, EGL %x)\n", why, glGetError(), eglGetError()); std::exit(2); } }
struct Context {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY); EGLContext ctx; EGLSurface surf;
    Context() {
        must(eglInitialize(d, nullptr, nullptr), "initialize software EGL");
        must(eglBindAPI(EGL_OPENGL_ES_API), "bind GLES2");
        EGLConfig cfg; EGLint num;
        EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        must(eglChooseConfig(d, attrs, &cfg, 1, &num) && num == 1, "RGBA8 config");
        EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE}, sa[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
        ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ca); surf = eglCreatePbufferSurface(d, cfg, sa);
        must(eglMakeCurrent(d, surf, surf, ctx), "current pbuffer");
        std::printf("Renderer: %s (%s)\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
        glDisable(GL_DITHER);
    }
    ~Context() { eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglDestroySurface(d, surf); eglDestroyContext(d, ctx); eglTerminate(d); }
};
GLuint program(const std::string& vs, const std::string& fs) {
    GLuint p = glCreateProgram();
    for (auto e : {std::make_pair(GL_VERTEX_SHADER, &vs), std::make_pair(GL_FRAGMENT_SHADER, &fs)}) {
        GLuint s = glCreateShader(e.first); const char* src = e.second->c_str();
        glShaderSource(s, 1, &src, nullptr); glCompileShader(s);
        int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char msg[2048]; glGetShaderInfoLog(s, sizeof msg, nullptr, msg); must(false, msg); }
        glAttachShader(p, s); glDeleteShader(s);
    }
    glBindAttribLocation(p, 0, "a_pos"); glLinkProgram(p); int ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok); must(ok, "link the production depth-warp program"); return p;
}
void u(GLuint p, const char* n, float a, float b = 0, float c = 0, float d = 0) { const GLint l = glGetUniformLocation(p, n); if (l != -1) glUniform4f(l, a, b, c, d); }
// The depth warp of a whole-picture shift by (sx, sy) texels, as DrawWarp sets it (u_s = (w, h, k, 0), u_t = (1 / w, 1 / h)); k10 = the env value.
Image Warp(GLuint p, GLuint src, GLuint mask, float sx, float sy, int k10) {
    glUseProgram(p);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, mask); glUniform1i(glGetUniformLocation(p, "u_depth"), 1);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, src); glUniform1i(glGetUniformLocation(p, "u_src"), 0);
    u(p, "u_a", .5f, 0, 0, .5f + sx / W); u(p, "u_b", 0, .5f, 0, .5f + sy / H); u(p, "u_c", 0, 0, 0, 1);
    u(p, "u_f", 0, 0, 0); u(p, "u_e", 0, 0, 0, 0); u(p, "u_z", 0, 0, 0); u(p, "u_p", 0, 64, 0, 0); u(p, "u_d", 0, 0, .02f, 0);
    u(p, "u_k", .5f / W, .5f / H, 1 - .5f / W, 1 - .5f / H);
    u(p, "u_s", static_cast<float>(W), static_cast<float>(H), k10 * 0.1f, 0.0f); u(p, "u_t", 1.0f / W, 1.0f / H);
    glViewport(0, 0, W, H); glDisable(GL_BLEND);
    static constexpr GLfloat v[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, v); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    Image img(W * H); glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, img.data());
    must(glGetError() == GL_NO_ERROR, "draw and read back");
    return img;
}
int MaxDiff(const Pixel& a, const double b[3]) { int m = 0; for (int c = 0; c < 3; ++c) m = std::max(m, static_cast<int>(std::ceil(std::fabs(a[c] - b[c]) - 1e-6))); return m; }
int Diff(const Pixel& a, const Pixel& b) { return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]); }
double Energy(const Image& im, int m) {
    double s = 0; int n = 0;
    for (int y = m; y < H - m - 1; ++y) for (int x = m; x < W - m - 1; ++x) { s += Diff(im[y * W + x], im[y * W + x + 1]) + Diff(im[y * W + x], im[(y + 1) * W + x]); n += 2; }
    return s / n;
}
// The design's read for output pixel (x, y): t = its source position in texels minus a half; the mix of texels floor(t) and floor(t) + 1 with SharpWeight.
void Model(const Image& src, int x, int y, float sx, float sy, double k, double out[3], bool* whole) {
    const double tx = x + 0.5 + sx - 0.5, ty = y + 0.5 + sy - 0.5;
    const double ix = std::floor(tx), iy = std::floor(ty);
    const double wx = port::hrw41::SharpWeight(tx - ix, k), wy = port::hrw41::SharpWeight(ty - iy, k);
    // (the same thing through SharpCoord: the read's texel coordinate is SharpCoord(t, k), whose fraction is the weight)
    if (std::fabs(port::hrw41::SharpCoord(tx, k) - (ix + wx)) > 1e-9 || std::fabs(port::hrw41::SharpCoord(ty, k) - (iy + wy)) > 1e-9) std::abort();
    const int x0 = static_cast<int>(ix), y0 = static_cast<int>(iy);
    for (int c = 0; c < 3; ++c) {
        const double a = src[y0 * W + x0][c], b = src[y0 * W + x0 + 1][c], d = src[(y0 + 1) * W + x0][c], e = src[(y0 + 1) * W + x0 + 1][c];
        out[c] = (a * (1 - wx) + b * wx) * (1 - wy) + (d * (1 - wx) + e * wx) * wy;
    }
    *whole = (wx == 0.0 || wx == 1.0) && (wy == 0.0 || wy == 1.0);
}
}  // namespace

int main() {
    Context ctx;
    g_sharp = 0;
    const std::string fsOff = D41Fragment();
    g_sharp = 40;
    const std::string fsOn = D41Fragment();
    check(fsOff.find("vec4 u_s;") == std::string::npos && fsOff.find("vec4 u_t;") == std::string::npos && fsOff.find("u_s.") == std::string::npos, "GXV_HRW_WARP_SHARP=0: the program text has no sharp uniform (the text of before the switch)");
    check(fsOn != fsOff && fsOn.find("uniform vec4 u_s;") != std::string::npos && fsOn.find("uniform vec4 u_t;") != std::string::npos, "GXV_HRW_WARP_SHARP>0: a different text with u_s and u_t");
    g_sharp = 10;
    check(D41Fragment() == fsOn, "the text does not depend on the value (k is a uniform): one program for every k > 0");
    g_depth41 = 2; g_sharp = 40;
    const std::string fsOn2 = D41Fragment();
    g_depth41 = 1;
    const GLuint pOff = program(kVsWarp, fsOff), pOn = program(kVsWarp, fsOn), pOn2 = program(kVsWarp, fsOn2);
    Image src(W * H);
    uint32_t r = 4242u;
    for (auto& px : src) { for (int c = 0; c < 3; ++c) { r = r * 1664525u + 1013904223u; px[c] = static_cast<unsigned char>(r >> 24); } px[3] = 255; }
    GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, src.data());
    GLuint mask; glGenTextures(1, &mask); glBindTexture(GL_TEXTURE_2D, mask);
    const unsigned char far[4] = {200, 0, 0, 255};   // (one depth code everywhere; u_z = 0 makes it irrelevant to the read position)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, far);
    const int m = 4, interior = (W - 2 * m) * (H - 2 * m);
    const double e0 = Energy(src, m);
    char what[220];
    const float shifts[][2] = {{0.0f, 0.0f}, {0.20f, 0.0f}, {0.0f, 0.30f}, {0.37f, -0.23f}, {0.45f, 0.0f}, {0.50f, 0.0f}, {0.55f, 0.52f}, {0.63f, 0.0f}, {-1.38f, 0.41f}, {2.0f, -1.0f}, {0.80f, 0.90f}};
    for (const int k10 : {40, 20, 80}) {
        const double k = k10 * 0.1;
        for (const auto& s : shifts) {
            const Image out = Warp(pOn, tex, mask, s[0], s[1], k10);
            int worst = 0, wholePx = 0, wholeBad = 0;
            for (int y = m; y < H - m; ++y) for (int x = m; x < W - m; ++x) {
                double want[3]; bool whole;
                Model(src, x, y, s[0], s[1], k, want, &whole);
                const int d = MaxDiff(out[y * W + x], want);
                worst = std::max(worst, d);
                if (whole) { ++wholePx; wholeBad += d > 1; }
            }
            std::snprintf(what, sizeof what, "k %.0f, shift %+.2f,%+.2f: the shader against the design's read: worst channel error %d / 255 (want <= 3)", k, s[0], s[1], worst);
            check(worst <= 3, what);
            const double frx = s[0] - std::floor(s[0]), fry = s[1] - std::floor(s[1]);
            const bool outside = std::fabs(frx - 0.5) > 0.5 / k + 0.02 && std::fabs(fry - 0.5) > 0.5 / k + 0.02;
            if (outside) {
                std::snprintf(what, sizeof what, "k %.0f, shift %+.2f,%+.2f: outside the blend zone every pixel is one whole source texel (%d of %d whole, %d off by more than 1)", k, s[0], s[1], wholePx, interior, wholeBad);
                check(wholePx == interior && wholeBad == 0, what);
            }
            if (k10 == 40) std::printf("k 4, shift %+.2f,%+.2f: worst error %d, whole-texel pixels %d of %d, detail energy %.1f (source %.1f)\n", s[0], s[1], worst, wholePx, interior, Energy(out, m), e0);
        }
    }
    {   // 3. half-texel shift: the even mix
        const Image out = Warp(pOn, tex, mask, 0.5f, 0.0f, 40);
        int bad = 0;
        for (int y = m; y < H - m; ++y) for (int x = m; x < W - m; ++x)
            for (int c = 0; c < 3; ++c) bad += std::fabs(out[y * W + x][c] - 0.5 * (src[y * W + x][c] + src[y * W + x + 1][c])) > 3.0;
        check(bad == 0, "a half-texel shift is the even mix of the two texels (the middle of the blend zone)");
    }
    // 4. k = 1 is the switch-off picture
    for (const auto& s : shifts) {
        const Image a = Warp(pOff, tex, mask, s[0], s[1], 0), b = Warp(pOn, tex, mask, s[0], s[1], 10);
        int worst = 0;
        for (int y = m; y < H - m; ++y) for (int x = m; x < W - m; ++x) for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(a[y * W + x][c] - b[y * W + x][c]));
        std::snprintf(what, sizeof what, "shift %+.2f,%+.2f: GXV_HRW_WARP_SHARP=10 (k = 1) against the switch-off program: worst difference %d / 255 (want <= 2)", s[0], s[1], worst);
        check(worst <= 2, what);
    }
    // 5. a still pair is the source exactly, every program
    for (const GLuint p : {pOff, pOn, pOn2}) {
        const Image out = Warp(p, tex, mask, 0, 0, 40);
        int bad = 0; for (int y = m; y < H - m; ++y) for (int x = m; x < W - m; ++x) bad += Diff(out[y * W + x], src[y * W + x]) != 0;
        check(bad == 0, "a still pair is the first picture exactly (off, sharp with one tap, sharp with two taps)");
    }
    // 6. detail kept: a slow drift (the shimmer case) with k = 4 against plain bilinear
    for (const float sx : {0.20f, 0.30f, 0.70f}) {
        const double eOn = Energy(Warp(pOn, tex, mask, sx, 0.15f, 40), m), eOff = Energy(Warp(pOff, tex, mask, sx, 0.15f, 0), m);
        std::snprintf(what, sizeof what, "drift %.2f,0.15: detail energy source %.1f, sharp %.1f, plain bilinear %.1f (sharp within 5 %% of the source, plain under 85 %%)", sx, e0, eOn, eOff);
        check(std::fabs(eOn - e0) < 0.05 * e0 && eOff < 0.85 * e0, what);
    }
    std::printf("%s: %d of %d checks passed\n", g_fail ? "FAILED" : "ok", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
