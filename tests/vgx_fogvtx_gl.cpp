// GXV_FOGVTX / GXV_NODISCARD: real generated GLES2 shaders on Mesa, pixel oracle. No game data or Vita needed.
//   * with both switches off the generated source of a fixed key set is byte-identical to the generator before the switches;
//   * a draw's fog from the vertex shader (kFogVtxBit / VsKey::fogVtx) against the per-pixel fog: orthographic fog must be
//     pixel-identical; perspective fog is measured (it is a straight line through the factor's 0 and 1 points, not the curve);
//   * a trivially passing alpha test leaves no `discard` with GXV_NODISCARD=1, and the picture is the one without the test.
// The binary reads the switches from its environment (the runner starts it with each value) and checks what each value must do.
#include "vgx_fogvtx_digest.h"
#include "vgx_shadergen.h"
#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vgx { bool LogEnabled() { return false; } }
namespace {
// The digests of tests/vgx_fogvtx_digest.h made with the generator as it was before the two switches (git: the commit before).
constexpr uint64_t kBaseFragment = 0xe0d633972486f353ull, kBaseVertex = 0xe2cea692dfe69c89ull;

void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
GLuint Compile(GLenum type, const std::string& text) {
    GLuint s = glCreateShader(type);
    const char* p = text.c_str(); glShaderSource(s, 1, &p, nullptr); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096] = {}; glGetShaderInfoLog(s, sizeof log, nullptr, log); std::fprintf(stderr, "%s\n%s\n", log, p); }
    Require(ok, "generated shader compiles"); return s;
}
GLuint Link(const std::string& vtx, const std::string& frag) {
    GLuint vs = Compile(GL_VERTEX_SHADER, vtx), fs = Compile(GL_FRAGMENT_SHADER, frag);
    GLuint p = glCreateProgram(); glAttachShader(p, vs); glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos"); glBindAttribLocation(p, 1, "a_col0"); glBindAttribLocation(p, 2, "a_col1");
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok); Require(ok, "generated shader links");
    glDeleteShader(vs); glDeleteShader(fs); return p;
}
vgx::FsKey Base() {
    vgx::FsKey k;
    k.stage[0].color = 2u | (15u << 4) | (15u << 8) | (15u << 12); // D = c0
    k.stage[0].alpha = (1u << 4) | (7u << 7) | (7u << 10) | (7u << 13);
    return k;
}
void Uniform4(GLuint p, const char* arr, int i, const float v[4]) {
    const std::string n = std::string(arr) + "[" + std::to_string(i) + "]";
    const GLint loc = glGetUniformLocation(p, n.c_str());
    if (loc >= 0) glUniform4fv(loc, 1, v);
}

// ---- GXSetFog (decomp/src/RVL_SDK/gx/GXPixel.c) as the three numbers BuildTevC puts in the fog block
struct FogBlock { double a, b, c; };
FogBlock GxFog(bool ortho, double startz, double endz, double nearz, double farz) {
    if (ortho) {
        const double A = 1.0 / (endz - startz);
        return {(farz - nearz) * A, 0.0, (startz - nearz) * A};
    }
    const double A = (farz * nearz) / ((farz - nearz) * (endz - startz)), B = farz / (farz - nearz), C = startz / (endz - startz);
    double mant = B; int expn = 0;
    while (mant > 1.0) { mant /= 2; ++expn; }
    while (mant > 0 && mant < 0.5) { mant *= 2; --expn; }
    const float a = static_cast<float>(A / static_cast<double>(1u << (expn + 1)));
    const uint32_t bm = static_cast<uint32_t>(mant * 8388638.0);
    const float sh = std::ldexp(1.0f, expn + 1);
    return {static_cast<double>(a * sh), static_cast<double>(static_cast<float>(bm) * sh / 16777216.0f), static_cast<double>(static_cast<float>(C))};
}

struct Pix { std::vector<uint8_t> rgba; };
struct FogCase { bool ortho; double nearz, farz, startz, endz, zn, zf; };

// Draws a quad (two triangles) whose four corners are at the given eye depths, both ways, and reads the pixels.
// vtxFog: 0 per-pixel; otherwise the vertex-fog keys.
Pix DrawFog(const FogCase& fc, bool vtxFog, const double depth[4], const float fogRgb[3]) {
    vgx::FsKey fk = Base();
    fk.fog = static_cast<uint8_t>(2 | (fc.ortho ? 8 : 0) | (vtxFog ? vgx::kFogVtxBit : 0));
    vgx::VsKey vk;
    vk.fogVtx = static_cast<uint8_t>(vtxFog ? (fc.ortho ? 3 : 1) : 0);
    vgx::ShaderSource fs, vs;
    vgx::GenerateFragmentShader(fk, fs);
    vgx::GenerateVertexShader(vk, vs);
    const GLuint p = Link(vs.text, fs.text);
    glUseProgram(p);
    // projection (BuildProjC), p0 = p2 = 1, p1 = p3 = 0
    double p4, p5;
    float m[16] = {};
    if (!fc.ortho) {
        p4 = -fc.nearz / (fc.farz - fc.nearz); p5 = -fc.farz * fc.nearz / (fc.farz - fc.nearz);
        m[0] = 1; m[5] = -1; m[10] = static_cast<float>(2 * p4 - 1); m[14] = static_cast<float>(2 * p5); m[11] = -1;
    } else {
        p4 = -1.0 / (fc.farz - fc.nearz); p5 = -fc.farz / (fc.farz - fc.nearz);
        m[0] = 1; m[5] = -1; m[10] = static_cast<float>(2 * p4); m[14] = static_cast<float>(2 * p5 + 1); m[15] = 1;
    }
    glUniformMatrix4fv(glGetUniformLocation(p, "u_proj"), 1, GL_FALSE, m);
    const float mv[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    for (int i = 0; i < 3; ++i) Uniform4(p, "u_mv", i, mv[i]);
    const float c0[4] = {0.9f, 0.3f, 0.1f, 1.0f};
    Uniform4(p, "u_tev", 1, c0);
    const FogBlock fb = GxFog(fc.ortho, fc.startz, fc.endz, fc.nearz, fc.farz);
    float block[4] = {static_cast<float>(fb.a), static_cast<float>(fb.b), static_cast<float>(fb.c), 0};
    if (vtxFog) {
        const vgx::FogVtxCoef co = vgx::FogVtxCoeffs(block[0], block[1], block[2], static_cast<float>(p4), static_cast<float>(p5), fc.zn, fc.zf, fc.ortho);
        block[0] = co.c1; block[1] = 0; block[2] = 0; block[3] = co.c0;
    }
    Uniform4(p, "u_tev", 9, block);
    const float col[4] = {fogRgb[0], fogRgb[1], fogRgb[2], 0};
    Uniform4(p, "u_tev", 10, col);
    // corners in clip order: (-1,-1) (1,-1) (-1,1) (1,1); x, y chosen so that the normalised device position is the corner
    float v[4][12] = {};
    const double cx[4] = {-1, 1, -1, 1}, cy[4] = {-1, 1 * -1, 1, 1};
    const double nx[4] = {-1, 1, -1, 1}, ny[4] = {-1, -1, 1, 1};
    (void)cx; (void)cy;
    for (int i = 0; i < 4; ++i) {
        const double ze = -depth[i];
        const double scale = fc.ortho ? 1.0 : -ze;
        v[i][0] = static_cast<float>(nx[i] * scale); v[i][1] = static_cast<float>(ny[i] * scale); v[i][2] = static_cast<float>(ze);
        v[i][4] = v[i][5] = v[i][6] = v[i][7] = 1; v[i][8] = v[i][9] = v[i][10] = v[i][11] = 1;
    }
    const int idx[6] = {0, 1, 2, 2, 1, 3};
    float tri[6][12];
    for (int i = 0; i < 6; ++i) std::memcpy(tri[i], v[idx[i]], sizeof tri[i]);
    GLuint vbo = 0; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STREAM_DRAW);
    for (int a = 0; a < 3; ++a) { glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE, 12 * sizeof(float), reinterpret_cast<void*>(a * 4 * sizeof(float))); glEnableVertexAttribArray(a); }
    glDepthRangef(static_cast<float>(fc.zn), static_cast<float>(fc.zf));
    glClearColor(0.2f, 0.2f, 0.2f, 1); glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    Pix px; px.rgba.resize(64 * 64 * 4);
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px.rgba.data());
    Require(glGetError() == GL_NO_ERROR, "fog draw GL status");
    glDeleteBuffers(1, &vbo); glDeleteProgram(p);
    return px;
}

struct Diff { size_t differing = 0; int maxDelta = 0; size_t covered = 0, graded = 0; };
Diff Compare(const Pix& a, const Pix& b) {
    Diff d;
    for (size_t i = 0; i < a.rgba.size(); i += 4) {
        const bool covered = a.rgba[i] != 51 || a.rgba[i + 1] != 51; // (0.2 * 255)
        d.covered += covered;
        bool diff = false;
        for (int c = 0; c < 4; ++c) {
            const int dl = std::abs(int(a.rgba[i + c]) - int(b.rgba[i + c]));
            d.maxDelta = std::max(d.maxDelta, dl);
            diff |= dl != 0;
        }
        d.differing += diff;
        // pixels that are neither unfogged nor fully fogged
        d.graded += a.rgba[i] != 230 && a.rgba[i] != 26 && covered;
    }
    return d;
}

// ---- alpha test model (the comparisons of CompareExpr, on 8-bit alpha)
bool Cmp(uint32_t c, int a, int ref) {
    switch (c) {
    case 0: return false; case 1: return a < ref; case 2: return a == ref; case 3: return a <= ref;
    case 4: return a > ref; case 5: return a != ref; case 6: return a >= ref; default: return true;
    }
}
bool Test(uint32_t c0, int r0, uint32_t op, uint32_t c1, int r1, int a) {
    const bool x = Cmp(c0, a, r0), y = Cmp(c1, a, r1);
    switch (op) { case 0: return x && y; case 1: return x || y; case 2: return x != y; default: return x == y; }
}
}

int main() {
    const long fogMode = vgx::FogVtxMode();
    const bool noDiscard = vgx::NoDiscard();
    std::printf("GXV_FOGVTX=%ld GXV_NODISCARD=%d\n", fogMode, noDiscard ? 1 : 0);

    // ---- byte identity with both switches off
    unsigned nf = 0, nv = 0;
    const uint64_t df = fogvtx_digest::FragmentDigest(&nf), dv = fogvtx_digest::VertexDigest(&nv);
    std::printf("digest fragment(%u) %016llx vertex(%u) %016llx\n", nf, static_cast<unsigned long long>(df), nv, static_cast<unsigned long long>(dv));
    Require(dv == kBaseVertex, "vertex sources are byte-identical to the generator before the switches (they never depend on them)");
    if (fogMode == 0 && !noDiscard) Require(df == kBaseFragment, "switches off: fragment sources are byte-identical to the generator before the switches");
    if (fogMode != 0 && !noDiscard) Require(df == kBaseFragment, "GXV_FOGVTX alone leaves every key without the vertex-fog bit byte-identical");

    // ---- which fog values take the vertex path
    for (int fog = 0; fog < 16; ++fog) {
        const bool want = (fog == 10 && fogMode >= 1) || (fog == 2 && fogMode >= 2);
        Require(vgx::FogVtxEligible(static_cast<uint8_t>(fog)) == want, "eligibility: only linear fog, ortho from 1, perspective from 2");
    }

    // ---- alpha test algebra: sound on every alpha, and how much it finds
    size_t proven = 0, knownCombos = 0;
    static const int kRefs[] = {-1, 0, 1, 2, 127, 128, 254, 255};
    for (uint32_t c0 = 0; c0 < 8; ++c0)
        for (uint32_t c1 = 0; c1 < 8; ++c1)
            for (uint32_t op = 0; op < 4; ++op)
                for (int r0 : kRefs)
                    for (int r1 : kRefs) {
                        if (!vgx::AlphaTestAlwaysPasses(c0, r0, op, c1, r1)) continue;
                        // unknown references stand for every reference
                        std::vector<int> v0, v1;
                        if (r0 < 0) v0 = {0, 1, 2, 127, 128, 254, 255}; else v0 = {r0};
                        if (r1 < 0) v1 = {0, 1, 2, 127, 128, 254, 255}; else v1 = {r1};
                        for (int a0 : v0) for (int a1 : v1) for (int a = 0; a < 256; ++a)
                            if (!Test(c0, a0, op, c1, a1, a)) { std::fprintf(stderr, "unsound: %u %d op%u %u %d alpha %d\n", c0, r0, op, c1, r1, a); Require(false, "AlphaTestAlwaysPasses is sound"); }
                        if (r0 >= 0 && r1 >= 0) ++proven;
                    }
    for (uint32_t c0 = 0; c0 < 8; ++c0) for (uint32_t c1 = 0; c1 < 8; ++c1) knownCombos += 4 * 7 * 7;
    std::printf("alpha test: %zu of the %zu (comparison, reference, operator) combinations with known references always pass\n", proven, knownCombos + 0);
    Require(vgx::AlphaTestAlwaysPasses(7, -1, 0, 7, -1) && vgx::AlphaTestAlwaysPasses(6, 0, 0, 7, 0) && vgx::AlphaTestAlwaysPasses(3, 255, 1, 1, 255) &&
            vgx::AlphaTestAlwaysPasses(0, -1, 3, 0, -1) && vgx::AlphaTestAlwaysPasses(4, 9, 3, 4, 9),
            "known always-passing tests are recognised");
    Require(!vgx::AlphaTestAlwaysPasses(4, 0, 1, 4, 0) && !vgx::AlphaTestAlwaysPasses(4, 1, 0, 7, 0) && !vgx::AlphaTestAlwaysPasses(0, -1, 0, 7, -1) &&
            !vgx::AlphaTestAlwaysPasses(5, 255, 0, 7, 0) && !vgx::AlphaTestAlwaysPasses(2, 0, 3, 5, 0),
            "tests that can fail are not removed");

    Require(vgx::host::CreateContext(), "Mesa GLES context");
    std::printf("GL renderer: %s\n", glGetString(GL_RENDERER));
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "RGBA8 target");
    glViewport(0, 0, 64, 64); glDisable(GL_DITHER); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);

    // ---- fog: per vertex against per pixel
    const float fogRgb[3] = {0.1f, 0.5f, 0.9f};
    const FogCase cases[] = {
        // ortho
        {true, 0, 1000, 100, 900, 0, 1}, {true, 10, 500, 0, 400, 0, 1}, {true, 0, 1000, 100, 900, 0.25, 0.75}, {true, 1, 200, 50, 150, 0.1, 0.9},
        // perspective: (near, far, start, end) as the game sets them
        {false, 1, 1000, 100, 800, 0, 1}, {false, 10, 5000, 0, 4000, 0, 1}, {false, 0.5, 100, 20, 90, 0, 1}, {false, 100, 50000, 5000, 40000, 0, 1},
        {false, 100, 60000, 2000, 30000, 0, 1}, {false, 20, 100000, 1000, 90000, 0, 1},
        {false, 1, 100, 10, 60, 0, 1}, {false, 1, 100, 1, 99, 0, 1},
    };
    int worstOrtho = 0, worstPersp = 0;
    size_t differingPersp = 0, gradedPersp = 0;
    for (const FogCase& fc : cases) {
        // four corner depths: distinct, spanning the ramp and beyond it (as eye distance in front of the camera)
        const double lo = std::max(fc.nearz * 1.05, fc.startz * 0.5), hi = std::min(fc.farz * 0.98, fc.endz * 1.2);
        const double depthA[4] = {lo, lo + (hi - lo) * 0.55, lo + (hi - lo) * 0.3, hi};
        const double depthB[4] = {lo, lo, hi, hi};
        for (const double* depth : {depthA, depthB}) {
            const Pix ref = DrawFog(fc, false, depth, fogRgb), vtx = DrawFog(fc, true, depth, fogRgb);
            const Diff d = Compare(ref, vtx);
            if (std::getenv("VGX_TEST_VERBOSE")) std::printf("%s n=%g f=%g start=%g end=%g depth range %g..%g : %zu covered, %zu in the ramp, %zu differ, max delta %d/255\n", fc.ortho ? "ortho" : "persp", fc.nearz,
                        fc.farz, fc.startz, fc.endz, fc.zn, fc.zf, d.covered, d.graded, d.differing, d.maxDelta);
            Require(d.covered > 3000, "the quad covers the target");
            if (fc.ortho ? fc.zn == 0.0 : true) Require(d.graded > 100, "the test draws a real ramp");
            if (fc.ortho) { worstOrtho = std::max(worstOrtho, d.maxDelta); Require(d.differing == 0, "orthographic vertex fog is pixel-identical"); }
            else { worstPersp = std::max(worstPersp, d.maxDelta); differingPersp += d.differing; gradedPersp += d.graded; }
        }
    }
    std::printf("fog: ortho identical (max delta %d); perspective max delta %d/255, %zu of %zu ramp pixels differ\n", worstOrtho, worstPersp, differingPersp, gradedPersp);
    Require(worstPersp <= 1, "perspective vertex fog is within 1/255");

    // ---- alpha test: no discard when it cannot fail
    {
        vgx::FsKey none = Base();
        vgx::FsKey trivial = Base();
        trivial.alphaTestOn = 1; trivial.alphaTest = 0u | (0u << 3) | (3u << 6); // NEVER xnor NEVER: true for every alpha
        vgx::FsKey real = Base();
        real.alphaTestOn = 1; real.alphaTest = 4u | (4u << 3) | (1u << 6);       // GREATER ref0 OR GREATER ref1
        vgx::ShaderSource sn, st, sr;
        vgx::GenerateFragmentShader(none, sn); vgx::GenerateFragmentShader(trivial, st); vgx::GenerateFragmentShader(real, sr);
        Require(sn.text.find("discard") == std::string::npos, "no test, no discard");
        Require(sr.text.find("discard") != std::string::npos, "a test that can fail keeps its discard");
        if (noDiscard) Require(st.text == sn.text, "GXV_NODISCARD: a test that cannot fail generates the very source of no test");
        else Require(st.text.find("discard") != std::string::npos, "switch off: the discard stays");
        const std::string vtx = "attribute vec2 a_pos; attribute vec4 a_col0, a_col1; varying vec4 v_col0, v_col1;\nvoid main(){gl_Position=vec4(a_pos,0.0,1.0);v_col0=a_col0;v_col1=a_col1;}\n";
        const GLuint pn = Link(vtx, sn.text), pt = Link(vtx, st.text);
        const float quad[] = {-1, -1, 3, -1, -1, 3};
        GLuint vbo = 0; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
        glDisableVertexAttribArray(1); glDisableVertexAttribArray(2);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr); glEnableVertexAttribArray(0);
        glViewport(0, 0, 1, 1);
        size_t checked = 0;
        for (int a = 0; a < 256; ++a) {
            uint8_t px[2][4];
            int w = 0;
            for (GLuint p : {pn, pt}) {
                glUseProgram(p);
                const float c[4] = {a / 255.0f, 0.5f, 0.25f, a / 255.0f};
                Uniform4(p, "u_tev", 1, c);
                glClearColor(17.0f / 255, 33.0f / 255, 49.0f / 255, 0); glClear(GL_COLOR_BUFFER_BIT);
                glDrawArrays(GL_TRIANGLES, 0, 3); glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[w++]);
            }
            Require(std::memcmp(px[0], px[1], 4) == 0, "a trivially passing alpha test draws exactly what no test draws"); ++checked;
            Require(px[0][0] == a, "the draw is not discarded");
        }
        std::printf("alpha: %zu pixel comparisons, trivial test %s\n", checked, noDiscard ? "has no discard" : "keeps its discard (switch off)");
    }
    Require(glGetError() == GL_NO_ERROR, "final GL status");
    std::printf("PASS\n");
    return 0;
}
