// crisp31: GXV_HRW_WARP_NEAREST, the 60 Assist warp with point reads of the first picture. Draws the PRODUCTION rotation-only warp
// (kVsWarp + WarpFragment(0), extracted from vgx_backend_gl.cpp by tools/test_crisp31_warp_nearest.sh) over a random-texel source with
// sub-texel and whole-texel shifts, once with the source LINEAR (today) and once NEAREST (the switch), and checks:
//   1. identity warp: both filters give the source exactly (the switch changes nothing on a still camera);
//   2. a fractional shift: NEAREST gives source texels exactly (no value that is not in the source; every pixel the nearest texel), LINEAR
//      gives blends (the softening the switch removes), measured as the neighbour-difference energy against the source's;
//   3. after the filter is put back to LINEAR (WarpSourceFilter(tex, false)), a 1:1 read of the same texture is the source exactly
//      (the HUD layer and every later reader see the texture as before).
//   4. finish44, GXV_HRW_WARP_SNAP=<px x 10> (hrwfeel36; the back end's rule is port::hrw41::SnapPair(largest source move, limit), which turns the same
//      point reads on for one pair): for the target value 5 (0.5 px) every pair the rule snaps is the first picture EXACTLY (each pixel's nearest texel is
//      its own), where the linear read of the same pair blends (the 30 Hz softening the switch removes); a pair at or over the limit is not snapped; value
//      0 never snaps. And why the limit must stay at or under 0.5 px: a 0.7 px move read with point sampling is NOT the first picture.
// Synthetic raster test only (Mesa llvmpipe); the console's point sampling is the same GXM filter mode vitaGL uses for GL_NEAREST.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
bool EdgeMirrorEnv() { return false; }
#include "warp_sources.inc"
#include "port/hrw41.h"

namespace {
constexpr int W = 96, H = 64;
using Pixel = std::array<unsigned char, 4>;
using Image = std::vector<Pixel>;
int g_fail = 0, g_checks = 0;
void check(bool ok, const char* why) {
    ++g_checks;
    if (!ok) { std::fprintf(stderr, "FAIL %s (GL %x)\n", why, glGetError()); ++g_fail; }
}
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
    glBindAttribLocation(p, 0, "a_pos"); glLinkProgram(p); int ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok); must(ok, "link production warp program"); return p;
}
// The back end's WarpSourceFilter, on the test's texture.
void SourceFilter(GLuint tex, bool nearest) {
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
    const GLint f = nearest ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
}
void u(GLuint p, const char* n, float a, float b = 0, float c = 0, float d = 0) { glUniform4f(glGetUniformLocation(p, n), a, b, c, d); }
// The warp of a whole-picture shift by (sx, sy) texels: the source of output pixel x is x + sx (rotation-only form, no depth: q = r).
Image Warp(GLuint p, GLuint src, float sx, float sy) {
    glUseProgram(p); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, src); glUniform1i(glGetUniformLocation(p, "u_src"), 0);
    u(p, "u_a", .5f, 0, 0, .5f + sx / W); u(p, "u_b", 0, .5f, 0, .5f + sy / H); u(p, "u_c", 0, 0, 0, 1);
    u(p, "u_f", 0, 0, 0); u(p, "u_e", 0, 0, 0, 0); u(p, "u_z", 0, 0, 0); u(p, "u_p", 0, 0, 0, 1); u(p, "u_d", 0, 0, .02f, 0);
    u(p, "u_k", .5f / W, .5f / H, 1 - .5f / W, 1 - .5f / H);
    glViewport(0, 0, W, H); glDisable(GL_BLEND);
    static constexpr GLfloat v[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, v); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    Image img(W * H); glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, img.data());
    must(glGetError() == GL_NO_ERROR, "draw and read back");
    return img;
}
int Diff(const Pixel& a, const Pixel& b) { return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]); }
// Mean absolute difference between horizontal and vertical neighbours inside the margin: the picture's fine-detail energy.
double Energy(const Image& im, int m) {
    double s = 0; int n = 0;
    for (int y = m; y < H - m - 1; ++y) for (int x = m; x < W - m - 1; ++x) { s += Diff(im[y * W + x], im[y * W + x + 1]) + Diff(im[y * W + x], im[(y + 1) * W + x]); n += 2; }
    return s / n;
}
}  // namespace

int main() {
    Context ctx;
    const GLuint p = program(kVsWarp, WarpFragment(0));
    Image src(W * H);
    uint32_t r = 12345u;
    for (auto& px : src) { for (int c = 0; c < 3; ++c) { r = r * 1664525u + 1013904223u; px[c] = static_cast<unsigned char>(r >> 24); } px[3] = 255; }
    GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, src.data());
    SourceFilter(tex, false);  // (NewRenderTexture: linear)
    const int m = 3;           // (interior: no clamped edge strip)
    // 1. identity
    for (int near = 0; near < 2; ++near) {
        SourceFilter(tex, near != 0);
        const Image out = Warp(p, tex, 0, 0);
        int bad = 0; for (int i = 0; i < W * H; ++i) bad += Diff(out[i], src[i]) != 0;
        check(bad == 0, near ? "identity warp, NEAREST: the source exactly" : "identity warp, LINEAR: the source exactly");
    }
    // 2. fractional shifts: NEAREST = the nearest texel exactly; LINEAR = blends, less fine detail
    const double e0 = Energy(src, m);
    const float shifts[][2] = {{0.37f, 0.0f}, {0.0f, 0.29f}, {0.31f, -0.23f}, {0.63f, 0.0f}, {-1.38f, 0.41f}};
    for (const auto& s : shifts) {
        SourceFilter(tex, true);
        const Image n = Warp(p, tex, s[0], s[1]);
        SourceFilter(tex, false);
        const Image l = Warp(p, tex, s[0], s[1]);
        int bad = 0, blends = 0;
        for (int y = m; y < H - m; ++y) for (int x = m; x < W - m; ++x) {
            const int sx = static_cast<int>(std::floor(x + 0.5f + s[0])), sy = static_cast<int>(std::floor(y + 0.5f + s[1]));
            bad += Diff(n[y * W + x], src[sy * W + sx]) != 0;
            blends += Diff(l[y * W + x], src[sy * W + sx]) != 0;
        }
        const double en = Energy(n, m), el = Energy(l, m);
        char what[160];
        std::snprintf(what, sizeof what, "shift %.2f,%.2f: NEAREST pixels that are not the nearest source texel %d (want 0)", s[0], s[1], bad);
        check(bad == 0, what);
        std::snprintf(what, sizeof what, "shift %.2f,%.2f: LINEAR blends %d of %d interior pixels (want most)", s[0], s[1], blends, (W - 2 * m) * (H - 2 * m));
        check(blends > (W - 2 * m) * (H - 2 * m) / 2, what);
        std::snprintf(what, sizeof what, "shift %.2f,%.2f: detail energy source %.1f, NEAREST %.1f, LINEAR %.1f (NEAREST keeps it, LINEAR loses it)", s[0], s[1], e0, en, el);
        check(std::fabs(en - e0) < 0.05 * e0 && el < 0.85 * e0, what);
        std::printf("shift %+.2f,%+.2f texels: detail energy source %.1f | NEAREST %.1f (%d pixels off the nearest texel) | LINEAR %.1f (%d blended)\n", s[0], s[1], e0, en, bad, el, blends);
    }
    // 3. put back: a 1:1 read after the switch's restore is the source exactly
    SourceFilter(tex, true); (void)Warp(p, tex, 0.4f, 0.4f); SourceFilter(tex, false);
    {
        const Image out = Warp(p, tex, 0, 0);
        int bad = 0; for (int i = 0; i < W * H; ++i) bad += Diff(out[i], src[i]) != 0;
        check(bad == 0, "after the restore to LINEAR a 1:1 read is the source exactly");
        GLint mn = 0, mg = 0; glBindTexture(GL_TEXTURE_2D, tex); glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &mn); glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mg);
        check(mn == GL_LINEAR && mg == GL_LINEAR, "the texture is LINEAR again after the warp (later readers, pooled reuse)");
    }
    // 4. GXV_HRW_WARP_SNAP: the pairs the rule snaps at the target value 5 are the first picture exactly
    {
        const int snapEnv = 5;                      // (GXV_HRW_WARP_SNAP=5, the Performance target: 0.5 px)
        const double limit = snapEnv * 0.1;         // (the back end: SnapPair(W.maxMovePx, WarpSnapEnv() * 0.1))
        const float moves[][2] = {{0.10f, 0.0f}, {0.0f, -0.45f}, {0.30f, 0.30f}, {-0.49f, 0.0f}, {0.35f, -0.35f}, {0.0f, 0.0f}};
        for (const auto& s : moves) {
            const double move = std::hypot(s[0], s[1]);   // (a whole-picture shift: every test point's source lies this far from its own place)
            char what[200];
            std::snprintf(what, sizeof what, "snap: a pair that moved %.3f px is snapped at GXV_HRW_WARP_SNAP=5 and not at 0", move);
            check(port::hrw41::SnapPair(move, limit) && !port::hrw41::SnapPair(move, 0.0), what);
            SourceFilter(tex, true);                 // (what DrawWarp does for a snapped pair: WarpSourceFilter(src, true))
            const Image n = Warp(p, tex, s[0], s[1]);
            SourceFilter(tex, false);
            const Image l = Warp(p, tex, s[0], s[1]);
            int bad = 0, blends = 0;
            for (int y = m; y < H - m; ++y) for (int x = m; x < W - m; ++x) { bad += Diff(n[y * W + x], src[y * W + x]) != 0; blends += Diff(l[y * W + x], src[y * W + x]) != 0; }
            std::snprintf(what, sizeof what, "snap: move %+.2f,%+.2f: the snapped second picture differs from the first in %d interior pixels (want 0)", s[0], s[1], bad);
            check(bad == 0, what);
            if (move > 0.05) {
                std::snprintf(what, sizeof what, "snap: move %+.2f,%+.2f: the unsnapped (linear) read differs in %d interior pixels (want most: what the snap removes)", s[0], s[1], blends);
                check(blends > (W - 2 * m) * (H - 2 * m) / 2, what);
            }
            std::printf("snap %+.2f,%+.2f (%.3f px): snapped pixels off the first picture %d | linear pixels off %d\n", s[0], s[1], move, bad, blends);
        }
        // not snapped: at the limit, over it (also a diagonal whose axes are each under it), and a pair whose move is not known
        check(!port::hrw41::SnapPair(0.5, limit) && !port::hrw41::SnapPair(std::hypot(0.4, 0.4), limit) && !port::hrw41::SnapPair(2.0, limit) && !port::hrw41::SnapPair(1.0e9, limit),
              "snap: a pair at or over 0.5 px, or of unknown move (1e9), is not snapped");
        // negative control: point reads of a 0.7 px move are a different picture (a limit over 0.5 px would not be 'B = A')
        SourceFilter(tex, true);
        const Image far = Warp(p, tex, 0.7f, 0.0f);
        SourceFilter(tex, false);
        int moved = 0;
        for (int y = m; y < H - m; ++y) for (int x = m; x < W - m; ++x) moved += Diff(far[y * W + x], src[y * W + x]) != 0;
        check(moved > (W - 2 * m) * (H - 2 * m) / 2 && port::hrw41::SnapPair(0.7, 1.0), "snap: control: a 0.7 px move with point reads is not the first picture (a limit of 10 would snap it: keep the value at or under 5)");
    }
    std::printf("%s: %d of %d checks passed\n", g_fail ? "FAILED" : "ok", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
