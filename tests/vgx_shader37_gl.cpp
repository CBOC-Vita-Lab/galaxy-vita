// shader37: GXV_FS_ATFAST (cut-out as one compare / interval) and GXV_VS_NOBRANCH (vertex shaders without data-dependent `if`).
// Real generated GLES2 shaders on Mesa (pixel oracle) plus CPU models; no game data or Vita needed.
//   1. Text: with a switch off the generated text of the keys below is the text of the generator before the switch (checked by the runner
//      against the generator at the parent commit: tools/test_vgx_shader37.sh), and a key the switch does not take keeps its text.
//   2. ATFAST: for every cut-out the switch takes (all comparison pairs x operators x many references) the picture of the new text with the
//      thresholds AtFastValues gives equals the picture of the old text over an alpha ramp (non-byte alphas: the columns' own values), for
//      the plain test, the DISCARD_BLEND form (rejectZero) and the early form (mode 2); a two-stage key whose colour reads the previous alpha
//      (the early form must not leak the stage's new alpha into the colour).
//   3. ATFAST in half precision: a CPU model rounds alpha and the thresholds to half: for byte-valued alphas the old and the new test agree.
//   4. NOBRANCH: the vertex shader text has no `if` left (keys without a line / point or a light skip); lit and palette keys draw the same
//      colours with the old and the new text over random normals (zero ones included) and palette indices; CPU models of the select forms
//      are bit-identical to the branches.
#include "vgx_shadergen.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#ifndef EGL_PLATFORM_SURFACELESS_MESA   // (finish44: libglvnd's egl.h does not carry Mesa's platform names; EGL_MESA_platform_surfaceless)
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vgx { bool LogEnabled() { return false; } }
namespace {
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
bool MakeContext() {
    EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (d == EGL_NO_DISPLAY) d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint a, b;
    if (!eglInitialize(d, &a, &b)) return false;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                         EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(d, ca, &cfg, 1, &n) || n < 1) return false;
    const EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
    const EGLint pa[] = {EGL_WIDTH, 256, EGL_HEIGHT, 64, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, cfg, pa);
    return c != EGL_NO_CONTEXT && s != EGL_NO_SURFACE && eglMakeCurrent(d, s, s, c);
}
GLuint Compile(GLenum type, const std::string& text) {
    GLuint s = glCreateShader(type);
    const char* p = text.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096] = {}; glGetShaderInfoLog(s, sizeof log, nullptr, log); std::fprintf(stderr, "%s\n%s\n", log, p); }
    Require(ok, "generated shader compiles");
    return s;
}
GLuint Link(const std::string& vtx, const std::string& frag) {
    GLuint vs = Compile(GL_VERTEX_SHADER, vtx), fs = Compile(GL_FRAGMENT_SHADER, frag);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_nrm");
    glBindAttribLocation(p, 2, "a_col0");
    glBindAttribLocation(p, 3, "a_col1");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    Require(ok, "generated shaders link");
    return p;
}
void Uniform4(GLuint p, const char* arr, int count, const float* v) {
    const std::string n = std::string(arr) + "[0]";
    const GLint loc = glGetUniformLocation(p, n.c_str());
    if (loc >= 0) glUniform4fv(loc, count, v);
}

// ---- the cut-out key: 1 stage, the colour is the raster colour, the alpha the raster alpha (the ramp)
vgx::FsKey RampKey(unsigned c0, unsigned c1, unsigned op, bool reject) {
    vgx::FsKey k;
    k.numStages = 1;
    k.stage[0].rasChan = 0;
    k.stage[0].color = 10u | (15u << 4) | (15u << 8) | (15u << 12) | (1u << 19);
    k.stage[0].alpha = (5u << 4) | (7u << 7) | (7u << 10) | (7u << 13) | (1u << 19);
    k.alphaTestOn = 1;
    k.alphaTest = static_cast<uint8_t>(c0 | (c1 << 3) | (op << 6));
    if (reject) k.setRejectZero(1);
    return k;
}
// two stages: stage 0 writes prev.a = ras.a; stage 1's colour reads prev.a (the OLD one) as its colour, its alpha is ras.a / 2 (written to prev.a)
vgx::FsKey LeakKey(unsigned c0, unsigned c1, unsigned op) {
    vgx::FsKey k = RampKey(c0, c1, op, false);
    k.numStages = 2;
    k.stage[0].color = 15u | (15u << 4) | (15u << 8) | (15u << 12) | (1u << 19); // prev.rgb = 0
    k.stage[1].rasChan = 0;
    k.stage[1].color = 1u | (15u << 4) | (15u << 8) | (15u << 12) | (1u << 19);  // d = prev.a as a colour
    k.stage[1].alpha = (5u << 4) | (7u << 7) | (7u << 10) | (7u << 13) | (3u << 20) | (1u << 19); // ras.a * 0.5
    return k;
}

struct Frame { std::vector<unsigned char> px; };
// Draw the key's shader over a 256 wide alpha ramp: pixel column i has alpha (i + .5) / 256. The u_tev block is built as BuildTevC does for the
// fields used here (the references in row 8, the cut-out's thresholds in row 9 for a key that uses them).
Frame DrawRamp(const vgx::FsKey& k, float r0, float r1) {
    vgx::VsKey vk;
    vgx::ShaderSource vs, fs;
    vgx::GenerateVertexShader(vk, vs);
    vgx::GenerateFragmentShader(k, fs);
    const GLuint p = Link(vs.text, fs.text);
    glUseProgram(p);
    const float id4[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    glUniformMatrix4fv(glGetUniformLocation(p, "u_proj"), 1, GL_FALSE, id4);
    const float mv[24] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    Uniform4(p, "u_mv", 3, mv);
    float tb[128] = {};
    tb[32] = r0;
    tb[33] = r1;
    tb[34] = 0.0f;
    if (vgx::FsAtFastKey(k)) {
        float thr[2];
        vgx::AtFastValues(k.alphaTest, r0, r1, thr);
        tb[36] = thr[0];
        tb[37] = thr[1];
    }
    Uniform4(p, "u_tev", vgx::FsTevCount(k), tb);
    const float pos[16] = {-1, -1, 0, 0, 1, -1, 0, 0, -1, 1, 0, 0, 1, 1, 0, 0};
    const float col[16] = {1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1};
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 0, col);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(2);
    glViewport(0, 0, 256, 4);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(0x0005 /* GL_TRIANGLE_STRIP */, 0, 4);
    Frame f;
    f.px.resize(256 * 4);
    glReadPixels(0, 1, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, f.px.data());
    return f;
}

// ---- the old test as a CPU model (the text's arithmetic), and the new one
bool OldPass(unsigned c, float ref, float a) {
    const float ai = std::floor(a * 255.0f + 0.5f);
    switch (c) {
    case 0: return false;
    case 1: return ai < ref - 0.5f;
    case 2: return std::fabs(ai - ref) < 0.5f;
    case 3: return ai < ref + 0.5f;
    case 4: return ai > ref + 0.5f;
    case 5: return std::fabs(ai - ref) > 0.5f;
    case 6: return ai > ref - 0.5f;
    default: return true;
    }
}
bool OldTest(uint8_t t, float r0, float r1, float a) {
    const bool x = OldPass(t & 7, r0, a), y = OldPass((t >> 3) & 7, r1, a);
    switch ((t >> 6) & 3) {
    case 0: return x && y;
    case 1: return x || y;
    case 2: return x != y;
    default: return x == y;
    }
}
bool NewTest(uint8_t t, float r0, float r1, float a, float (*round)(float)) {
    float thr[2];
    vgx::AtFastValues(t, r0, r1, thr);
    const float x = round(thr[0]), y = round(thr[1]);
    switch (vgx::AtFastKind(t)) {
    case 1: return a >= x;
    case 2: return a < y;
    default: return std::fabs(a - x) < y;
    }
}
float Id(float x) { return x; }
// half: round to nearest even to 11 significant bits (subnormals below 2^-14)
float R16(float x) {
    if (x == 0.0f || !std::isfinite(x)) return x;
    int e = 0;
    const float m = std::frexp(std::fabs(x), &e); // m in [0.5, 1)
    const int bitsKept = e < -13 ? 11 - (-13 - e) : 11;
    if (bitsKept <= 0) return 0.0f;
    const float scale = std::ldexp(1.0f, bitsKept);
    float r = std::nearbyint(m * scale) / scale;
    r = std::ldexp(r, e);
    return std::copysign(r, x);
}
} // namespace

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    // ---- 3. the CPU models (no GL)
    {
        size_t eligible = 0, checks = 0, diffs = 0, diffsHalf = 0;
        const float refs[] = {0, 1, 2, 7, 64, 100, 127, 128, 129, 200, 254, 255};
        for (unsigned c0 = 0; c0 < 8; ++c0)
            for (unsigned c1 = 0; c1 < 8; ++c1)
                for (unsigned op = 0; op < 4; ++op) {
                    const uint8_t t = static_cast<uint8_t>(c0 | (c1 << 3) | (op << 6));
                    if (!vgx::AtFastKind(t)) continue;
                    ++eligible;
                    for (float r0 : refs)
                        for (float r1 : refs)
                            for (int lv = 0; lv < 256; ++lv) {
                                const float a = static_cast<float>(lv) / 255.0f; // a byte-valued alpha
                                ++checks;
                                const bool o = OldTest(t, r0, r1, a);
                                if (NewTest(t, r0, r1, a, Id) != o) ++diffs;
                                // half: the alpha the half shader holds, the thresholds in half (the text's `u_tev` is half there)
                                const float ah = R16(a);
                                if (NewTest(t, r0, r1, ah, R16) != OldTest(t, r0, r1, ah)) ++diffsHalf;
                            }
                }
        std::printf("ATFAST model: %zu eligible tests, %zu (test, refs, level) checks: float %zu differ, half %zu differ\n", eligible, checks, diffs, diffsHalf);
        Require(eligible > 20, "many tests are eligible");
        Require(diffs == 0, "byte-valued alpha: the interval test agrees with the rounded-level test");
        Require(diffsHalf == 0, "byte-valued alpha in half precision: the thresholds' rounding to half never moves a level across");
        // random non-byte alphas: the only differences are within 1e-5 of a level boundary
        size_t nb = 0, nbDiff = 0, nbNear = 0;
        uint32_t s = 12345u;
        auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.0f; };
        for (unsigned c0 = 0; c0 < 8; ++c0)
            for (unsigned c1 = 0; c1 < 8; ++c1)
                for (unsigned op = 0; op < 4; ++op) {
                    const uint8_t t = static_cast<uint8_t>(c0 | (c1 << 3) | (op << 6));
                    if (!vgx::AtFastKind(t)) continue;
                    for (int i = 0; i < 400; ++i) {
                        const float r0 = std::floor(rnd() * 256.0f), r1 = std::floor(rnd() * 256.0f), a = rnd();
                        ++nb;
                        if (NewTest(t, r0, r1, a, Id) != OldTest(t, r0, r1, a)) {
                            ++nbDiff;
                            const float lv = a * 255.0f;
                            if (std::fabs(lv - std::floor(lv) - 0.5f) < 1e-3f) ++nbNear;
                        }
                    }
                }
        std::printf("ATFAST model, random alphas: %zu checks, %zu differ, %zu of those within 1e-3 level of a half-level boundary\n", nb, nbDiff, nbNear);
        Require(nbDiff == nbNear, "a difference only on a level boundary");
        // the classification: NEVER, NEQUAL, XOR, XNOR and unlike OR are not taken; AND of two lower bounds is a lower bound
        Require(vgx::AtFastKind(6 | (3 << 3) | (0 << 6)) == 3, "GEQUAL AND LEQUAL is an interval");
        Require(vgx::AtFastKind(4 | (7 << 3)) == 1 && vgx::AtFastKind(7 | (1 << 3)) == 2, "one side ALWAYS: the other's bound");
        Require(vgx::AtFastKind(4 | (4 << 3) | (1 << 6)) == 1, "GREATER OR GREATER is the weaker lower bound");
        Require(vgx::AtFastKind(6 | (1 << 3) | (1 << 6)) == 0 && vgx::AtFastKind(3 | (6 << 3) | (2 << 6)) == 0, "unlike OR / XOR keep the old text");
        Require(vgx::AtFastKind(0) == 0 && vgx::AtFastKind(5) == 0 && vgx::AtFastKind(7 | (7 << 3)) == 0, "NEVER, NEQUAL, ALWAYS-ALWAYS keep the old text");
    }
    // ---- 4a. the select forms of the vertex shader as CPU models, bit for bit
    {
        uint32_t s = 99u;
        auto rnd = [&]() { s = s * 1664525u + 1013904223u; return static_cast<int32_t>(s) / 2147483648.0f; };
        size_t n = 0;
        for (int i = 0; i < 200000; ++i) {
            float v[3] = {rnd(), rnd(), rnd()};
            if (i % 5 == 0) v[0] = v[1] = v[2] = 0.0f;                      // a zero normal / direction
            if (i % 7 == 0) { v[0] *= 1e-20f; v[1] *= 1e-20f; v[2] *= 1e-20f; } // squares underflow: the length is 0
            const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            float oldv[3] = {v[0], v[1], v[2]}, newv[3] = {v[0], v[1], v[2]};
            if (l > 0.0f) for (float& x : oldv) x = x / l;
            const float d = l + (0.0f >= l ? 1.0f : 0.0f); // step(l, 0.0) = 0.0 >= l
            for (float& x : newv) x = x / d;
            ++n;
            Require(std::memcmp(oldv, newv, sizeof oldv) == 0, "normalise: n / (l + step(l, 0)) is bit-identical to if (l > 0) n / l");
        }
        for (float z : {0.0f, -0.0f, 1e-30f, -1e-30f, 1.0f, -2.5f, 1e30f}) {
            float a = z, b = z;
            if (a == 0.0f) a = 1.0f;
            b += (0.0f >= std::fabs(b) ? 1.0f : 0.0f);
            Require(a == b, "q == 0 guard as an add");
        }
        for (int w = 0; w <= 300; ++w) {
            for (int mx : {27, 57, 61, 21}) {
                int oldi = static_cast<int>(std::floor(static_cast<float>(w) + 0.5f));
                if (oldi > mx) oldi = mx;
                const int newi = static_cast<int>(std::min(std::floor(static_cast<float>(w) + 0.5f), static_cast<float>(mx)));
                Require(oldi == newi, "palette index clamp");
            }
            const int pi = std::min(w, 61);
            const int oldni = pi > 29 ? 93 : 64 + pi;
            const int newni = 64 + static_cast<int>(std::min(static_cast<float>(w), 29.0f));
            Require(oldni == newni, "normal row index");
        }
        std::printf("NOBRANCH model: %zu normalisations bit-identical, guards and index clamps equal\n", n);
    }

    Require(MakeContext(), "an offscreen GLES2 context (Mesa)");
    std::printf("renderer: %s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    // ---- 2. cut-out pictures
    {
        const float refSets[][2] = {{0, 255}, {128, 255}, {1, 254}, {64, 192}, {100, 100}, {200, 40}, {255, 255}, {0, 0}, {127, 129}, {250, 3}};
        size_t programs = 0, compared = 0, tested = 0;
        for (unsigned c0 = 0; c0 < 8; ++c0)
            for (unsigned c1 = 0; c1 < 8; ++c1)
                for (unsigned op = 0; op < 4; ++op) {
                    const uint8_t t = static_cast<uint8_t>(c0 | (c1 << 3) | (op << 6));
                    if (!vgx::AtFastKind(t)) continue;
                    for (int reject = 0; reject < 2; ++reject)
                        for (int shape = 0; shape < 2; ++shape) { // 0 the ramp key, 1 the two-stage leak key (no reject form)
                            if (shape == 1 && reject) continue;
                            const vgx::FsKey k = shape ? LeakKey(c0, c1, op) : RampKey(c0, c1, op, reject != 0);
                            Frame ref[10], one[10], two[10];
                            vgx::FsAtFastModeForTest(0);
                            for (int i = 0; i < 10; ++i) ref[i] = DrawRamp(k, refSets[i][0], refSets[i][1]);
                            vgx::FsAtFastModeForTest(1);
                            Require(vgx::FsAtFastKey(k), "the key is taken with ATFAST=1");
                            for (int i = 0; i < 10; ++i) one[i] = DrawRamp(k, refSets[i][0], refSets[i][1]);
                            vgx::FsAtFastModeForTest(2);
                            for (int i = 0; i < 10; ++i) two[i] = DrawRamp(k, refSets[i][0], refSets[i][1]);
                            vgx::FsAtFastModeForTest(0);
                            programs += 3;
                            for (int i = 0; i < 10; ++i) {
                                for (int x = 0; x < 256; ++x) {
                                    const size_t o = static_cast<size_t>(x) * 4;
                                    // the colour channels (the pass / discard mask, and for the leak key the old alpha shown as a colour)
                                    const bool a1 = std::memcmp(&ref[i].px[o], &one[i].px[o], 3) != 0;
                                    const bool a2 = std::memcmp(&ref[i].px[o], &two[i].px[o], 3) != 0;
                                    if (a1 || a2) {
                                        std::fprintf(stderr, "FAIL: test %02x refs %g %g column %d (alpha %.5f): old %u new %u early %u (shape %d reject %d)\n", t,
                                                     refSets[i][0], refSets[i][1], x, (x + 0.5) / 256.0, ref[i].px[o], one[i].px[o], two[i].px[o], shape, reject);
                                        return 1;
                                    }
                                    ++compared;
                                }
                                if (shape == 0 && !reject)
                                    for (int x = 0; x < 256; ++x) tested += ref[i].px[static_cast<size_t>(x) * 4] != 0;
                            }
                        }
                }
        std::printf("ATFAST pictures: %zu programs, %zu columns compared, identical (%zu passing columns seen)\n", programs, compared, tested);
        Require(tested > 1000, "the ramp has passing and failing columns");
    }
    // text: the early form has its discard before the last stage's colour; keys not taken keep the old text
    {
        const vgx::FsKey k = LeakKey(6, 3, 0);
        vgx::ShaderSource a, b, c;
        vgx::FsAtFastModeForTest(0); vgx::GenerateFragmentShader(k, a);
        vgx::FsAtFastModeForTest(1); vgx::GenerateFragmentShader(k, b);
        vgx::FsAtFastModeForTest(2); vgx::GenerateFragmentShader(k, c);
        vgx::FsAtFastModeForTest(0);
        Require(a.text.find("floor(") != std::string::npos && b.text.find("floor(") == std::string::npos, "ATFAST=1: no floor in the test");
        const size_t dEarly = c.text.find("discard"), colour = c.text.rfind("vec3 rc");
        Require(dEarly != std::string::npos && dEarly < colour, "ATFAST=2: the discard is before the last stage's colour");
        Require(a.nTev == 9 && b.nTev == 10 && c.nTev == 10, "u_tev has one more row with the thresholds");
        const vgx::FsKey nk = RampKey(5, 7, 0, false); // NEQUAL: not an interval
        vgx::ShaderSource n0, n1;
        vgx::FsAtFastModeForTest(0); vgx::GenerateFragmentShader(nk, n0);
        vgx::FsAtFastModeForTest(2); vgx::GenerateFragmentShader(nk, n1);
        vgx::FsAtFastModeForTest(0);
        Require(n0.text == n1.text && n0.nTev == n1.nTev, "a test that is not one interval keeps its text and block");
        vgx::FsKey fk = RampKey(6, 7, 0, false);
        fk.fog = 2;
        vgx::ShaderSource f0, f1;
        vgx::FsAtFastModeForTest(0); vgx::GenerateFragmentShader(fk, f0);
        vgx::FsAtFastModeForTest(2); vgx::GenerateFragmentShader(fk, f1);
        vgx::FsAtFastModeForTest(0);
        Require(f0.text == f1.text, "a key with fog keeps its text (its row 9 is the fog row)");
        std::printf("ATFAST text: ok\n");
    }

    // ---- 4b. vertex shaders: no `if` left, same colours
    {
        auto Count = [](const std::string& s, const char* p) { size_t n = 0, i = 0; while ((i = s.find(p, i)) != std::string::npos) { ++n; i += std::strlen(p); } return n; };
        size_t keys = 0, ifsBefore = 0, ifsAfter = 0;
        for (int pvp = 0; pvp <= 2; ++pvp)
            for (int attn = 0; attn <= 2; ++attn)
                for (int diff = 0; diff <= 2; ++diff)
                    for (int post = 0; post <= 2; ++post) {
                        vgx::VsKey k;
                        k.perVtxPos = static_cast<uint8_t>(pvp);
                        k.numChans = 1;
                        k.numTexGens = 1;
                        k.chan[0].enable = 1; k.chan[0].matSrcVtx = 1; k.chan[0].diffuse = static_cast<uint8_t>(diff);
                        k.chan[0].attn = static_cast<uint8_t>(attn); k.chan[0].lightMask = 3;
                        k.tg[0].src = 5; k.tg[0].proj = 1; k.tg[0].post = static_cast<uint8_t>(post); k.tg[0].perVtxMtx = pvp ? 1 : 0;
                        vgx::ShaderSource a, b, c;
                        vgx::VsNoBranchModeForTest(0); vgx::VsNoBranchLightModeForTest(0); vgx::GenerateVertexShader(k, a);
                        vgx::VsNoBranchModeForTest(1); vgx::VsNoBranchLightModeForTest(0); vgx::GenerateVertexShader(k, b);
                        vgx::VsNoBranchModeForTest(1); vgx::VsNoBranchLightModeForTest(1); vgx::GenerateVertexShader(k, c);
                        vgx::VsNoBranchModeForTest(0); vgx::VsNoBranchLightModeForTest(0);
                        ++keys;
                        ifsBefore += Count(a.text, "if (");
                        ifsAfter += Count(c.text, "if (");
                        Require(Count(b.text, "if (") == 2, "non-light NOBRANCH keeps only the 2 per-light direction guards");
                        Require(Count(c.text, "if (") == 0, "no `if` left in the vertex shader with NOBRANCH_LIGHT=1");
                        Require(a.nLt == b.nLt && a.nPal == b.nPal && a.nTm == b.nTm, "the blocks are the same (non-light)");
                        Require(a.nLt == c.nLt && a.nPal == c.nPal && a.nTm == c.nTm, "the blocks are the same (with light)");
                    }
        std::printf("NOBRANCH text: %zu lit / palette / texgen keys: `if` count %zu -> %zu\n", keys, ifsBefore, ifsAfter);
        // pictures: v_col0 of a lit vertex over random positions, normals (zero ones included) and palette rows
        size_t drawn = 0;
        for (int pvp = 0; pvp <= 2; pvp += 2)
            for (int attn = 0; attn <= 2; ++attn)
                for (int diff = 1; diff <= 2; ++diff) {
                    vgx::VsKey k;
                    k.perVtxPos = static_cast<uint8_t>(pvp);
                    k.numChans = 1;
                    k.chan[0].enable = 1; k.chan[0].matSrcVtx = 1; k.chan[0].diffuse = static_cast<uint8_t>(diff);
                    k.chan[0].attn = static_cast<uint8_t>(attn); k.chan[0].lightMask = 3;
                    vgx::FsKey fk;
                    fk.numStages = 1;
                    fk.stage[0].rasChan = 0;
                    fk.stage[0].color = 10u | (15u << 4) | (15u << 8) | (15u << 12) | (1u << 19);
                    fk.stage[0].alpha = (5u << 4) | (7u << 7) | (7u << 10) | (7u << 13) | (1u << 19);
                    Frame out[3];
                    for (int mode = 0; mode < 3; ++mode) {
                        vgx::VsNoBranchModeForTest(mode > 0 ? 1 : 0);
                        vgx::VsNoBranchLightModeForTest(mode == 2 ? 1 : 0);
                        vgx::ShaderSource vs, fs;
                        vgx::GenerateVertexShader(k, vs);
                        vgx::GenerateFragmentShader(fk, fs);
                        const GLuint p = Link(vs.text, fs.text);
                        glUseProgram(p);
                        const float id4[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                        glUniformMatrix4fv(glGetUniformLocation(p, "u_proj"), 1, GL_FALSE, id4);
                        const float mv[24] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.8f, 0, 0, 0, 0, 0.8f, 0, 0, 0, 0, 0.8f, 0};
                        Uniform4(p, "u_mv", 6, mv);
                        std::vector<float> pal(96 * 4, 0.0f);
                        for (int r = 0; r < 96; ++r) pal[r * 4 + (r % 3)] = 1.0f; // rows 3j.. are the identity: a palette row index that is a multiple of 3 gives the position itself
                        Uniform4(p, "u_pal", 96, pal.data());
                        std::vector<float> lt((4 + 2 * 5) * 4, 0.0f);
                        uint32_t s = 77u;
                        auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.0f; };
                        for (float& x : lt) x = rnd();
                        for (int l = 0; l < 2; ++l) { // dist attn (1, .1, .01) and cos attn (1, .5, .2): finite
                            float* L = &lt[(4 + l * 5) * 4];
                            L[12 + 0] = 1; L[12 + 1] = 0.5f; L[12 + 2] = 0.2f; // cosatt (row b + 3)
                            L[16 + 0] = 1; L[16 + 1] = 0.1f; L[16 + 2] = 0.01f; // distatt (row b + 4)
                        }
                        Uniform4(p, "u_lt", 4 + 2 * 5, lt.data());
                        // 64 cells in a row, each a quad of 4 identical vertices; the cell's colour is that vertex's lighting
                        std::vector<float> pos, nrm, col;
                        std::vector<unsigned short> dummy;
                        uint32_t t = 4242u;
                        auto rn = [&]() { t = t * 1664525u + 1013904223u; return (t >> 8) / 16777216.0f; };
                        for (int c = 0; c < 64; ++c) {
                            const float x0 = -1.0f + c * (2.0f / 64.0f), x1 = x0 + 2.0f / 64.0f;
                            const float w = pvp ? 3.0f * static_cast<float>(static_cast<int>(rn() * 34.0f)) : 0.0f; // palette row (a multiple of 3), also beyond the clamp
                            float n[3] = {rn() * 2 - 1, rn() * 2 - 1, rn() * 2 - 1};
                            if (c % 9 == 0) n[0] = n[1] = n[2] = 0.0f;
                            const float cc[4] = {rn(), rn(), rn(), 1.0f};
                            const float xs[6] = {x0, x1, x0, x0, x1, x1}, ys[6] = {-1, -1, 1, 1, -1, 1};
                            for (int v = 0; v < 6; ++v) {
                                pos.insert(pos.end(), {xs[v], ys[v], 0.25f, w});
                                nrm.insert(nrm.end(), n, n + 3);
                                col.insert(col.end(), cc, cc + 4);
                            }
                        }
                        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, pos.data());
                        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nrm.data());
                        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 0, col.data());
                        glEnableVertexAttribArray(0);
                        glEnableVertexAttribArray(1);
                        glEnableVertexAttribArray(2);
                        glViewport(0, 0, 256, 4);
                        glClearColor(0, 0, 0, 0);
                        glClear(GL_COLOR_BUFFER_BIT);
                        glDrawArrays(GL_TRIANGLES, 0, 64 * 6);
                        out[mode].px.resize(256 * 4);
                        glReadPixels(0, 1, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, out[mode].px.data());
                    }
                    vgx::VsNoBranchModeForTest(0);
                    vgx::VsNoBranchLightModeForTest(0);
                    size_t nz = 0;
                    for (size_t i = 0; i < 256 * 4; ++i) nz += out[0].px[i] != 0;
                    Require(nz > 200, "the lit cells are drawn");
                    Require(out[0].px == out[1].px, "the lit vertex shader's colours are the same with NOBRANCH (non-light)");
                    Require(out[0].px == out[2].px, "the lit vertex shader's colours are the same with NOBRANCH_LIGHT");
                    ++drawn;
                }
        std::printf("NOBRANCH pictures: %zu lit key shapes (plain and palette), identical\n", drawn);
    }
    std::printf("PASS\n");
    return 0;
}
