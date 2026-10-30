// Host-side test of GXV_COPYVAR (platform/vgx/src/vgx_copyvar.h): the dedicated copy programs draw exactly the pixels of the
// generic copy program (u_mat * texel + u_add), on Mesa's GLES2. No game data, no Vita executable.
//   tools/test_vgx_copyvar_gl.sh [output-dir]
//
// 1. The generic program's source is what it was before the header existed (a hash of the two strings).
// 2. Classify gives the form the transform of every GX copy format has in the front end (vgx_draw.cpp QueueCopyTex): the colour
//    formats' identity and the 565 / no-alpha form are specialised; the intensity, channel and depth forms are not; near
//    misses (1 + 2^-23, a stray -0 alpha term, a non-zero colour add) are not.
// 3. Every specialised form is rendered against the generic program over all 256 levels of every channel (a 256 x 256
//    texture), through full, flipped and sub-rectangle texel windows, into an 8-bit target: the bytes must be equal.
// 4. A constant fill draws with no texture bound.
#include "vgx_copyvar.h"

#include <GLES2/gl2.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
namespace host { bool CreateContext(); }
} // namespace vgx

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

uint64_t Fnv(std::initializer_list<const char*> strings) {
    uint64_t h = 14695981039346656037ull;
    for (const char* s : strings)
        for (size_t i = 0;; ++i) {
            h ^= static_cast<unsigned char>(s[i]);
            h *= 1099511628211ull;
            if (!s[i]) break;
        }
    return h;
}

GLuint Compile(GLenum type, const char* text) {
    const GLuint s = glCreateShader(type);
    const char* parts[2] = {"#version 100\n", text};
    glShaderSource(s, 2, parts, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048] = {}; glGetShaderInfoLog(s, sizeof log, nullptr, log); std::fprintf(stderr, "%s\n%s\n", log, text); }
    Require(ok, "shader compiles");
    return s;
}
struct Prog { GLuint id = 0; GLint rect = -1, mat = -1, add = -1; };
Prog Build(const char* fragment) {
    Prog p;
    p.id = glCreateProgram();
    glAttachShader(p.id, Compile(GL_VERTEX_SHADER, vgx::copyvar::kVertex));
    glAttachShader(p.id, Compile(GL_FRAGMENT_SHADER, fragment));
    glBindAttribLocation(p.id, 0, "a_pos");
    glLinkProgram(p.id);
    GLint ok = 0;
    glGetProgramiv(p.id, GL_LINK_STATUS, &ok);
    Require(ok, "program links");
    p.rect = glGetUniformLocation(p.id, "u_rect");
    p.mat = glGetUniformLocation(p.id, "u_mat");
    p.add = glGetUniformLocation(p.id, "u_add");
    glUseProgram(p.id);
    const GLint src = glGetUniformLocation(p.id, "u_src");
    if (src >= 0) glUniform1i(src, 0);
    return p;
}

constexpr int kW = 256, kH = 256;
GLuint g_quad = 0, g_tex = 0, g_target = 0, g_fbo = 0, g_white = 0;

void Draw(const Prog& p, vgx::copyvar::Form form, bool variant, const float rect[4], const float* m16, const float* add4, std::vector<uint8_t>& out) {
    using namespace vgx::copyvar;
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, kW, kH);
    glClearColor(0.2f, 0.4f, 0.6f, 0.8f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(p.id);
    glActiveTexture(GL_TEXTURE0);
    // A constant fill binds nothing it reads: bind a texture that is NOT the source, to show the result does not depend on it.
    glBindTexture(GL_TEXTURE_2D, variant && !UsesTexture(form) ? g_white : g_tex);
    if (!variant || UsesRect(form)) glUniform4f(p.rect, rect[0], rect[1], rect[2], rect[3]);
    if (!variant) glUniformMatrix4fv(p.mat, 1, GL_FALSE, m16);
    if (!variant || UsesAdd(form)) glUniform4f(p.add, add4[0], add4[1], add4[2], add4[3]);
    glBindBuffer(GL_ARRAY_BUFFER, g_quad);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    out.resize(static_cast<size_t>(kW) * kH * 4);
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
}

// The copy transform of each GX format, as vgx_draw.cpp QueueCopyTex builds it (row-major mat[r][c], then the colour-major
// m16 the back end hands to glUniformMatrix4fv), including the "no alpha plane" fold.
struct Transform { float mat[4][4] = {}; float add[4] = {}; };
Transform Make(unsigned fmt, bool hasAlpha) {
    Transform t;
    auto row = [&](int r, float a, float b, float c, float d) { t.mat[r][0] = a; t.mat[r][1] = b; t.mat[r][2] = c; t.mat[r][3] = d; };
    auto ident = [&] { row(0, 1, 0, 0, 0); row(1, 0, 1, 0, 0); row(2, 0, 0, 1, 0); row(3, 0, 0, 0, 1); };
    const float yr = 0.257f, yg = 0.504f, yb = 0.098f, yo = 16.0f / 255.0f;
    bool depth = false;
    switch (fmt) {
    case 0x00: case 0x01: for (int r = 0; r < 4; ++r) { row(r, yr, yg, yb, 0); t.add[r] = yo; } break;
    case 0x02: case 0x03: for (int r = 0; r < 3; ++r) { row(r, yr, yg, yb, 0); t.add[r] = yo; } row(3, 0, 0, 0, 1); break;
    case 0x04: ident(); row(3, 0, 0, 0, 0); t.add[3] = 1.0f; break;
    case 0x20: case 0x28: for (int r = 0; r < 4; ++r) row(r, 1, 0, 0, 0); break;
    case 0x22: case 0x23: for (int r = 0; r < 3; ++r) row(r, 1, 0, 0, 0); row(3, 0, 0, 0, 1); break;
    case 0x27: for (int r = 0; r < 4; ++r) row(r, 0, 0, 0, 1); break;
    case 0x29: for (int r = 0; r < 4; ++r) row(r, 0, 1, 0, 0); break;
    case 0x2A: for (int r = 0; r < 4; ++r) row(r, 0, 0, 1, 0); break;
    case 0x2B: for (int r = 0; r < 3; ++r) row(r, 0, 1, 0, 0); row(3, 1, 0, 0, 0); break;
    case 0x2C: for (int r = 0; r < 3; ++r) row(r, 0, 0, 1, 0); row(3, 0, 1, 0, 0); break;
    case 0x11: case 0x13: case 0x16: case 0x30: case 0x39: case 0x3A: case 0x3C: depth = true; ident(); break;
    default: ident(); break;
    }
    if (!hasAlpha && !depth) for (int r = 0; r < 4; ++r) { t.add[r] += t.mat[r][3]; t.mat[r][3] = 0; }
    if (depth) { // (the back end's depth copy: the red column only)
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) t.mat[r][c] = c == 0 ? 1.0f : 0.0f;
    }
    return t;
}
void ColumnMajor(const Transform& t, float m16[16]) {
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) m16[c * 4 + r] = t.mat[r][c];
}
} // namespace

int main() {
    using namespace vgx::copyvar;
    std::printf("generic program source\n");
    Check(Fnv({kVertex, kFragmentGeneric}) == 0xbafa0d6ca7b0fb6cull, "the generic copy program's two sources are byte for byte the ones the back end had");

    std::printf("classification of the GX copy formats (vgx_draw.cpp QueueCopyTex)\n");
    struct Row { const char* name; unsigned fmt; bool alpha; Form want; };
    const Row rows[] = {
        {"RGBA8 / RGB5A3 with alpha", 0x06, true, kIdentity},
        {"RGBA8 on an EFB without alpha", 0x06, false, kRgbConstA},
        {"RGB565", 0x04, true, kRgbConstA},
        {"RGB565 without alpha", 0x04, false, kRgbConstA},
        {"I4 / I8", 0x01, true, kGeneric},
        {"IA8", 0x03, true, kGeneric},
        {"R8", 0x28, true, kGeneric},
        {"RA8", 0x23, true, kGeneric},
        {"A8", 0x27, true, kGeneric},
        {"G8", 0x29, true, kGeneric},
        {"B8", 0x2A, true, kGeneric},
        {"RG8", 0x2B, true, kGeneric},
        {"GB8", 0x2C, true, kGeneric},
        {"depth (Z8 / Z16 / Z24X8)", 0x11, true, kGeneric},
    };
    for (const Row& r : rows) {
        float m16[16];
        const Transform t = Make(r.fmt, r.alpha);
        ColumnMajor(t, m16);
        const Form got = Classify(m16, t.add);
        char line[160];
        std::snprintf(line, sizeof line, "%s: %s", r.name, Name(got));
        Check(got == r.want, line);
    }
    {
        const float zero[16] = {}, white[4] = {1, 1, 1, 1}, none[4] = {};
        Check(Classify(zero, white) == kConstant && Classify(zero, none) == kConstant, "a zero matrix is a constant fill, whatever the colour");
        float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        Check(Classify(m, none) == kIdentity, "I with no add is the identity");
        m[0] = 1.0f + 1.1920929e-7f;
        Check(Classify(m, none) == kGeneric, "1 + 2^-23 on the diagonal is not the identity");
        m[0] = 1.0f;
        m[12] = 1e-30f;
        Check(Classify(m, none) == kGeneric, "a tiny translation column is not the identity");
        m[12] = 0.0f;
        float add[4] = {0, 0, 1e-30f, 0};
        Check(Classify(m, add) == kGeneric, "a tiny colour add is not the identity");
        float add2[4] = {0, 0, 0, 0.5f};
        Check(Classify(m, add2) == kGeneric, "identity with an alpha add keeps the matrix (alpha would be 1.5)");
        m[15] = 0.0f;
        Check(Classify(m, add2) == kRgbConstA, "identity with m33 = 0 and add.a = 0.5 is a constant alpha");
        float add3[4] = {0.25f, 0, 0, 1};
        Check(Classify(m, add3) == kGeneric, "a colour add on the rgb+constant-alpha form keeps the matrix");
        m[3] = 0.5f; // row 3, column 0
        Check(Classify(m, add2) == kGeneric, "alpha taken from the texel's red is not a constant alpha");
        const float nan = std::nanf("");
        float mn[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        mn[5] = nan;
        Check(Classify(mn, none) == kGeneric, "a NaN in the matrix is not any form");
    }

    Require(vgx::host::CreateContext(), "Mesa GLES context");
    std::printf("renderer: %s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    // source texture: every level of every channel (x: red and green ramp, y: blue and alpha ramp, plus a hash so channels differ)
    std::vector<uint8_t> src(static_cast<size_t>(kW) * kH * 4);
    {
        std::mt19937 rng(7);
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) {
                uint8_t* p = &src[(static_cast<size_t>(y) * kW + x) * 4];
                p[0] = static_cast<uint8_t>(x);
                p[1] = static_cast<uint8_t>(255 - x);
                p[2] = static_cast<uint8_t>(y);
                p[3] = static_cast<uint8_t>((x + 3 * y) & 255);
                if ((x ^ y) & 16) { p[0] = static_cast<uint8_t>(rng()); p[1] = static_cast<uint8_t>(rng()); p[2] = static_cast<uint8_t>(rng()); p[3] = static_cast<uint8_t>(rng()); }
            }
    }
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, src.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const uint8_t whitePixel[4] = {255, 255, 255, 255};
    glGenTextures(1, &g_white);
    glBindTexture(GL_TEXTURE_2D, g_white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, whitePixel);
    glGenTextures(1, &g_target);
    glBindTexture(GL_TEXTURE_2D, g_target);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_target, 0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "framebuffer complete");
    const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenBuffers(1, &g_quad);
    glBindBuffer(GL_ARRAY_BUFFER, g_quad);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);

    const Prog generic = Build(kFragmentGeneric);
    const Form forms[] = {kIdentity, kRgbConstA, kConstant};
    Prog variants[kForms];
    for (Form f : forms) variants[f] = Build(FragmentSource(f));
    std::printf("pixels: dedicated program against the generic one (256 x 256 target, bytes compared)\n");
    // texel windows: the whole texture, a sub-rectangle, flipped vertically (the picture draw), a magnified window (bilinear)
    const float windows[][4] = {{0, 0, 1, 1}, {0.25f, 0.125f, 0.5f, 0.625f}, {0, 1, 1, -1}, {0.4f, 0.4f, 0.1f, 0.1f}, {0.01f, 0.02f, 0.97f, 0.96f}};
    const float addAlphas[] = {1.0f, 0.0f, 0.5f, 7.0f / 255.0f};
    for (Form f : forms) {
        size_t compared = 0, mismatches = 0, draws = 0;
        std::vector<uint8_t> a, b;
        for (const auto& w : windows)
            for (float alpha : addAlphas) {
                float m16[16] = {};
                float add[4] = {};
                if (f == kIdentity) { m16[0] = m16[5] = m16[10] = m16[15] = 1.0f; }
                if (f == kRgbConstA) { m16[0] = m16[5] = m16[10] = 1.0f; add[3] = alpha; }
                // 8-bit levels, as every GX colour is: an exact .5 tie such as 0.1 * 255 may round either way in either program
                if (f == kConstant) { add[0] = 25.0f / 255.0f; add[1] = alpha; add[2] = 229.0f / 255.0f; add[3] = 1.0f - alpha; }
                if (f == kIdentity && alpha != 1.0f) continue; // (one pass: the identity has no free parameter)
                Require(Classify(m16, add) == f, "test transform classifies as the form under test");
                Draw(generic, kGeneric, false, w, m16, add, a);
                Draw(variants[f], f, true, w, m16, add, b);
                ++draws;
                compared += a.size();
                for (size_t i = 0; i < a.size(); ++i) mismatches += a[i] != b[i];
            }
        char line[200];
        std::snprintf(line, sizeof line, "%s: %zu draws, %zu bytes compared, %zu different", Name(f), draws, compared, mismatches);
        Check(mismatches == 0 && compared > 0, line);
    }
    {
        // the identity must also reproduce the source texel for texel through the whole-texture window
        float m16[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const float add[4] = {};
        std::vector<uint8_t> b;
        Draw(variants[kIdentity], kIdentity, true, windows[0], m16, add, b);
        size_t diff = 0;
        for (size_t i = 0; i < b.size(); ++i) diff += b[i] != src[i];
        Check(diff == 0, "the identity program copies the 256 x 256 texture byte for byte");
    }
    std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all checks passed", g_failures);
    return g_failures ? 1 : 0;
}
