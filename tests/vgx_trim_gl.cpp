// Host test of GXV_FS_TRIM and GXV_VS_TRIM (platform/vgx/src/vgx_shadergen.cpp: FsTrimAnalyse, GenerateFragmentShaderForm,
// EmitLightAttn / LightSumNonNegative). No game data, no Vita executable.
//   tools/test_vgx_trim_gl.sh [output-dir] [keys]
//
// 1. The range analysis, on a CPU model of the TEV pipeline as the generator writes it (float, every shape: compare modes, indirect stages,
//    registers outside 0..255, the TEVIN wrap, register folding): for random keys and random inputs in the ranges the front end
//    guarantees (texels, konst colours and vertex colours 0..1; registers 0..255 / 255 unless the key is marked), every clamp side the
//    analysis calls idle must never be crossed by the value the untrimmed text clamps (beyond float rounding: 1e-5).
// 2. Half precision (GXV_FS_HALF=1) under the trim: the trim takes no part in the precision decision and a half key is never trimmed
//    (r88 console, Beach Bowl "static"): FsHalfEligible is the full text's analysis, and every key it takes is written byte for byte as
//    with the trim off; the admitted keys run through the same model in half (both forms of mix) stay within FsHalfAnalyse's bound.
// 3. Mesa, fragment: the untrimmed and the trimmed text of a key (and the half text of an admitted key, which is float on a PC) draw
//    random triangles with interpolated vertex colours, filtered textures and the same uniforms; RGBA8 readback compared byte for byte.
//    Differences are counted and printed (the trim is near-exact: an ulp past a bound is not clamped any more); more than one level, or
//    more than 1 in 10,000 bytes, fails.
// 4. Mesa, vertex: random lit vertex keys and light data in which some lights have the attenuation (1, 0, 0) / (1, 0, 0) exactly; the key
//    without and with the facts (VsKey::setLtTrim) draw the same triangles, the colour varyings written out plain and amplified (fract of
//    4096 times the value): byte for byte equal. A key whose facts change nothing keeps its text.
// 5. Regression (r88 console, Beach Bowl: ground drop shadows and grass details as speckled "static" with GXV_FS_TRIM=1 only): Galaxy's
//    shapes, drawn exhaustively on Mesa (a 256x256 target: texel alpha 0..255 across, interpolated vertex colour 0..1 up, every alpha
//    compare with references 0..255), full and trimmed text byte for byte equal: an alpha-tested ground decal of the opaque scene (one stage
//    tex * ras, alpha unclamped, GXSetDstAlpha(TRUE, 0): the class the old trim moved into half precision, where the analysis did not
//    bound the tested alpha), the shadow volume (MR::setupShadowVolumeDraw + ShadowVolumeDrawer), the shadow fill (MR::fillSilhouetteColor)
//    and the fall-out field's edge pass. The half model shows what the old precision change did to the decal's alpha test.
// Needs GXV_FS_TRIM=1 and GXV_FS_HALF=1 in the environment (the runner sets them).
#include "vgx_shadergen.h"
#include "vgx_fshalf_keys.h"
#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace vgx { bool LogEnabled() { return false; } namespace host { bool CreateContext(); } }
namespace {
using fshalf_keys::Rng;
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
unsigned Bits(uint32_t v, int shift, int n) { return (v >> shift) & ((1u << n) - 1u); }

// ---- half on the host (as tests/vgx_fshalf_test.cpp)
float R16(float x) {
    if (x == 0.0f || !std::isfinite(x)) return x;
    int e = 0;
    std::frexp(x, &e);
    const float q = std::ldexp(1.0f, std::max(e - 11, -24));
    const float y = std::nearbyint(x / q) * q;
    return std::fabs(y) > 65504.0f ? std::copysign(INFINITY, x) : y;
}

struct Inputs {
    float tex[8][4], col[2][4], reg[4][4], konst[4][4];
    int ref0, ref1;
};
struct Trace { float lo[16][2], hi[16][2]; float finLo[2], finHi[2]; }; // pre-clamp extremes per stage and part (0 colour, 1 alpha)
struct Model {
    bool half = false;
    int mix = 2;
    float Rnd(float x) const { return half ? R16(x) : x; }
    float Add(float a, float b) const { return Rnd(a + b); }
    float Sub(float a, float b) const { return Rnd(a - b); }
    float Mul(float a, float b) const { return Rnd(a * b); }
    float Mix(float a, float b, float c) const { return mix == 1 ? Add(a, Mul(c, Sub(b, a))) : Add(Mul(a, Sub(1.0f, c)), Mul(b, c)); }
};
float Wrap1(float v) { // vgx_w1
    if (v >= 0.0f && v <= 1.0f) return v;
    const float b = std::floor(v * 255.0f + 0.5f);
    return (b - 256.0f * std::floor(b / 256.0f)) / 255.0f;
}
// The TEV pipeline of GenerateFragmentShaderAs. ti: the trimmed text (clamp sides left out), nullptr: the full text. Returns the final value
// (before the 8-bit rounding); no fog, no logic op, no destination alpha (the Mesa part covers the text of those).
void Eval(const vgx::FsKey& k, const Inputs& in, const vgx::FsTrimInfo* ti, const Model& m, float out[4], Trace* tr) {
    float reg[4][4], tex[4] = {1, 1, 1, 1}, ras[4] = {0, 0, 0, 0}, kc[3] = {1, 1, 1}, ka = 1.0f;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) reg[r][c] = m.Rnd(in.reg[r][c]);
    for (int c = 0; c < 4; ++c) {
        if (k.regFromVtx & 1) reg[1][c] = m.Rnd(in.col[0][c]);
        if (k.regFromVtx & 2) reg[2][c] = m.Rnd(in.col[1][c]);
    }
    const bool w = (k.inWrap & 1) != 0;
    for (int s = 0; s < k.numStages; ++s) {
        const vgx::FsKey::Stage& st = k.stage[s];
        if (st.texEnable) for (int i = 0; i < 4; ++i) tex[i] = m.Rnd(in.tex[st.texMap][(st.texSwap >> (2 * i)) & 3]);
        else if (st.texOff) for (int i = 0; i < 4; ++i) tex[i] = 1.0f;
        if (st.rasChan < 2) for (int i = 0; i < 4; ++i) ras[i] = m.Rnd(in.col[st.rasChan][(st.rasSwap >> (2 * i)) & 3]);
        else for (int i = 0; i < 4; ++i) ras[i] = 0.0f;
        const uint32_t tc = st.color, ta = st.alpha;
        bool kcUsed = false, kaUsed = false;
        for (int i = 0; i < 4; ++i) {
            if (Bits(tc, i * 4, 4) == 14) kcUsed = true;
            if (Bits(ta, 4 + i * 3, 3) == 6) kaUsed = true;
        }
        if (kcUsed) {
            const unsigned sel = st.kcsel;
            for (int i = 0; i < 3; ++i) {
                static const int comp[5][3] = {{0, 1, 2}, {0, 0, 0}, {1, 1, 1}, {2, 2, 2}, {3, 3, 3}};
                kc[i] = sel < 8 ? (8 - sel) / 8.0f : sel < 12 ? 0.0f : m.Rnd(in.konst[sel & 3][comp[(sel - 12) >> 2][i]]);
            }
        }
        if (kaUsed) ka = st.kasel < 8 ? (8 - st.kasel) / 8.0f : st.kasel < 16 ? 0.0f : m.Rnd(in.konst[st.kasel & 3][(st.kasel - 16) >> 2]);
        auto CArg = [&](unsigned a, int i, bool abc) -> float {
            float v;
            switch (a & 15) {
            case 0: v = reg[0][i]; break; case 1: v = reg[0][3]; break;
            case 2: v = reg[1][i]; break; case 3: v = reg[1][3]; break;
            case 4: v = reg[2][i]; break; case 5: v = reg[2][3]; break;
            case 6: v = reg[3][i]; break; case 7: v = reg[3][3]; break;
            case 8: return tex[i];  case 9: return tex[3];
            case 10: return ras[i]; case 11: return ras[3];
            case 12: return 1.0f;   case 13: return 0.5f;
            case 14: return kc[i];  default: return 0.0f;
            }
            return (w && abc) ? Wrap1(v) : v;
        };
        auto AArg = [&](unsigned a, bool abc) -> float {
            switch (a & 7) {
            case 0: case 1: case 2: case 3: return (w && abc) ? Wrap1(reg[a & 3][3]) : reg[a & 3][3];
            case 4: return tex[3]; case 5: return ras[3]; case 6: return ka; default: return 0.0f;
            }
        };
        float a3[3], b3[3], c3[3], d3[3];
        for (int i = 0; i < 3; ++i) {
            a3[i] = CArg(Bits(tc, 12, 4), i, true); b3[i] = CArg(Bits(tc, 8, 4), i, true);
            c3[i] = CArg(Bits(tc, 4, 4), i, true); d3[i] = CArg(Bits(tc, 0, 4), i, false);
        }
        const float aa = AArg(Bits(ta, 13, 3), true), ab = AArg(Bits(ta, 10, 3), true), ac = AArg(Bits(ta, 7, 3), true), ad = AArg(Bits(ta, 4, 3), false);
        auto Combine = [&](uint32_t r32, int part, float* v, int n, const float* A, const float* B, const float* C, const float* D) {
            const uint32_t bias = Bits(r32, 16, 2), op = Bits(r32, 18, 1), clampOn = Bits(r32, 19, 1), scale = Bits(r32, 20, 2);
            for (int i = 0; i < n; ++i) {
                float r;
                if (bias == 3) {
                    // compare modes, as the text: (a > b + e) / (|a - b| < 2e) on r, rg (16 bits), rgb (24 bits) or per channel
                    const float e = 0.25f / 255.0f;
                    bool cond;
                    if (part == 0 && scale == 3) cond = op ? std::fabs(A[i] - B[i]) < 2 * e : A[i] > B[i] + e;
                    else if (scale == 0) cond = op ? std::fabs(A[0] - B[0]) < 2 * e : A[0] > B[0] + e;
                    else if (scale == 1 || scale == 2) {
                        const float d = (A[0] - B[0]) * 255.0f + (part == 0 ? (A[1] - B[1]) * 65280.0f : 0.0f) +
                                        (part == 0 && scale == 2 ? (A[2] - B[2]) * 16711680.0f : 0.0f);
                        cond = op ? std::fabs(d) < 0.5f : d > 0.25f;
                    } else cond = op ? std::fabs(A[0] - B[0]) < 2 * e : A[0] > B[0] + e;
                    r = D[i] + (cond ? C[i] : 0.0f);
                } else {
                    r = m.Mix(A[i], B[i], C[i]);
                    r = op ? m.Sub(D[i], r) : m.Add(D[i], r);
                    if (bias == 1) r = m.Add(r, 0.5f);
                    else if (bias == 2) r = m.Sub(r, 0.5f);
                    static const float kScale[4] = {1.0f, 2.0f, 4.0f, 0.5f};
                    if (scale) r = m.Rnd(r * kScale[scale]);
                }
                if (tr) { tr->lo[s][part] = std::min(tr->lo[s][part], r); tr->hi[s][part] = std::max(tr->hi[s][part], r); }
                const vgx::FsTrimClamp act = ti ? (part ? ti->a[s] : ti->c[s]) : vgx::FsTrimClamp{};
                const float lo = clampOn ? 0.0f : (m.half ? R16(-4.0157f) : -4.0157f), hi = clampOn ? 1.0f : (m.half ? R16(4.0118f) : 4.0118f);
                if (act.lo) r = std::max(r, lo);
                if (act.hi) r = std::min(r, hi);
                v[i] = r;
            }
        };
        float rc[3], ra;
        Combine(tc, 0, rc, 3, a3, b3, c3, d3);
        // (the alpha part's compare reads a.r, ab: one channel)
        Combine(ta, 1, &ra, 1, &aa, &ab, &ac, &ad);
        for (int i = 0; i < 3; ++i) reg[Bits(tc, 22, 2)][i] = rc[i];
        reg[Bits(ta, 22, 2)][3] = ra;
    }
    const vgx::FsKey::Stage& last = k.stage[k.numStages - 1];
    for (int i = 0; i < 4; ++i) {
        const float v = i < 3 ? reg[Bits(last.color, 22, 2)][i] : reg[Bits(last.alpha, 22, 2)][3];
        if (tr) { const int p = i < 3 ? 0 : 1; tr->finLo[p] = std::min(tr->finLo[p], v); tr->finHi[p] = std::max(tr->finHi[p], v); }
        const bool lo = !ti || ti->finC.lo || ti->finA.lo, hi = !ti || ti->finC.hi || ti->finA.hi;
        float r = v;
        if (lo) r = std::max(r, 0.0f);
        if (hi) r = std::min(r, 1.0f);
        out[i] = r;
    }
}
float Sample(Rng& r) { // the ends over-represented, 8-bit values, and values in between
    const unsigned w = r.U(100);
    if (w < 10) return 0.0f;
    if (w < 20) return 1.0f;
    if (w < 55) return static_cast<float>(r.U(256)) / 255.0f;
    return static_cast<float>(r.F());
}
float RegSample(Rng& r, bool wide) {
    if (!wide) return static_cast<float>(r.P(20) ? (r.P(50) ? 0 : 255) : r.U(256)) / 255.0f;
    const unsigned w = r.U(100);
    const int v = w < 10 ? -1024 : w < 20 ? 1023 : static_cast<int>(r.U(2048)) - 1024;
    return static_cast<float>(v) / 255.0f;
}
Inputs RandomInputs(Rng& r, bool wide) {
    Inputs in;
    for (auto& t : in.tex) for (float& c : t) c = static_cast<float>(r.U(256)) / 255.0f;
    for (auto& t : in.col) for (float& c : t) c = Sample(r);
    for (auto& t : in.reg) for (float& c : t) c = RegSample(r, wide);
    for (auto& t : in.konst) for (float& c : t) c = static_cast<float>(r.U(256)) / 255.0f;
    in.ref0 = static_cast<int>(r.U(256)); in.ref1 = static_cast<int>(r.U(256));
    return in;
}
vgx::FsKey TrimKey(Rng& r) {
    vgx::FsKey k = fshalf_keys::RandomKey(r, r.P(40));
    const unsigned w = r.U(100);
    k.inWrap = static_cast<uint8_t>(w < 60 ? 0 : w < 85 ? 1 : 2);
    if (r.P(30)) { // more unclamped and scaled stages than real materials have: the analysis's harder cases
        for (int s = 0; s < k.numStages; ++s) {
            if (r.P(40)) k.stage[s].color &= ~(1u << 19);
            if (r.P(40)) k.stage[s].alpha &= ~(1u << 19);
        }
    }
    return k;
}

// ---- Mesa
GLuint Compile(GLenum type, const std::string& text) {
    GLuint s = glCreateShader(type);
    const char* p = text.c_str(); glShaderSource(s, 1, &p, nullptr); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096] = {}; glGetShaderInfoLog(s, sizeof log, nullptr, log); std::fprintf(stderr, "%s\n%s\n", log, p); }
    Require(ok, "generated shader compiles");
    return s;
}
GLuint Link(const std::string& vt, const std::string& ft) {
    const GLuint v = Compile(GL_VERTEX_SHADER, vt), f = Compile(GL_FRAGMENT_SHADER, ft);
    const GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[4096] = {}; glGetProgramInfoLog(p, sizeof log, nullptr, log); std::fprintf(stderr, "%s\n---- vs\n%s\n---- fs\n%s\n", log, vt.c_str(), ft.c_str()); }
    Require(ok, "program links");
    glDeleteShader(v); glDeleteShader(f);
    return p;
}
constexpr int kW = 32, kH = 32;
struct FVtx { float pos[4], c0[4], c1[4], t[3]; };
// One draw of 8 random triangles with the fragment program p; the seed fixes uniforms, textures and vertices for both programs of a pair.
void DrawFs(GLuint p, const vgx::FsKey& k, uint32_t seed, std::vector<uint8_t>& px) {
    std::mt19937 rng(seed);
    Rng r(seed);
    glUseProgram(p);
    std::vector<float> tev(28 * 4, 0.0f);
    const bool wide = k.inWrap != 0;
    for (int i = 0; i < 16; ++i) tev[i] = RegSample(r, wide);
    for (int i = 16; i < 32; ++i) tev[i] = static_cast<float>(r.U(256)) / 255.0f;
    tev[32] = static_cast<float>(r.U(256)); tev[33] = static_cast<float>(r.U(256)); tev[34] = static_cast<float>(r.U(256)) / 255.0f;
    for (int i = 36; i < 44; ++i) tev[i] = static_cast<float>(r.F());             // fog parameters and colour
    for (int i = 44; i < 68; ++i) tev[i] = static_cast<float>(r.F()) * 2.0f - 1.0f; // indirect matrices
    for (int i = 68; i < 84; ++i) tev[i] = 1.0f;                                   // indirect scales
    for (int i = 84; i < 100; ++i) tev[i] = 8.0f;                                  // texture sizes
    if (GLint l = glGetUniformLocation(p, "u_tev"); l >= 0) glUniform4fv(l, 28, tev.data());
    static GLuint names[8] = {};
    if (!names[0]) glGenTextures(8, names);
    for (int t = 0; t < 8; ++t) {
        uint8_t texel[8 * 8 * 4];
        for (uint8_t& b : texel) b = static_cast<uint8_t>(r.P(15) ? (r.P(50) ? 0 : 255) : rng());
        glActiveTexture(GL_TEXTURE0 + t);
        glBindTexture(GL_TEXTURE_2D, names[t]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        char name[16];
        std::snprintf(name, sizeof name, "u_tex%d", t);
        if (GLint l = glGetUniformLocation(p, name); l >= 0) glUniform1i(l, t);
    }
    std::vector<FVtx> v(8 * 3);
    for (FVtx& x : v) {
        x.pos[0] = static_cast<float>(r.F()) * 2.4f - 1.2f; x.pos[1] = static_cast<float>(r.F()) * 2.4f - 1.2f; x.pos[2] = 0.0f; x.pos[3] = 1.0f;
        for (float& f : x.c0) f = Sample(r);
        for (float& f : x.c1) f = Sample(r);
        x.t[0] = static_cast<float>(r.F()) * 4.0f - 2.0f; x.t[1] = static_cast<float>(r.F()) * 4.0f - 2.0f; x.t[2] = 0.5f + static_cast<float>(r.F());
    }
    auto Attr = [&](const char* name, int n, const float* first) {
        const GLint loc = glGetAttribLocation(p, name);
        if (loc < 0) return;
        glEnableVertexAttribArray(static_cast<GLuint>(loc));
        glVertexAttribPointer(static_cast<GLuint>(loc), n, GL_FLOAT, GL_FALSE, sizeof(FVtx), first);
    };
    for (GLuint i = 0; i < 16; ++i) glDisableVertexAttribArray(i);
    Attr("a_pos", 4, v[0].pos); Attr("a_col0", 4, v[0].c0); Attr("a_col1", 4, v[0].c1); Attr("a_tex", 3, v[0].t);
    glDisable(GL_BLEND);
    glClearColor(17.0f / 255, 33.0f / 255, 49.0f / 255, 65.0f / 255);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size()));
    px.assign(kW * kH * 4, 0);
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    Require(glGetError() == GL_NO_ERROR, "GL status");
}
const char* kFsVtx =
    "attribute vec4 a_pos; attribute vec4 a_col0; attribute vec4 a_col1; attribute vec3 a_tex;\n"
    "varying vec4 v_col0; varying vec4 v_col1; varying float v_fog;\n"
    "varying vec3 v_tex0; varying vec3 v_tex1; varying vec3 v_tex2; varying vec3 v_tex3; varying vec3 v_tex4; varying vec3 v_tex5; varying vec3 v_tex6; varying vec3 v_tex7;\n"
    "void main() { gl_Position = a_pos; v_col0 = a_col0; v_col1 = a_col1; v_fog = a_tex.z;\n"
    "  v_tex0 = a_tex; v_tex1 = a_tex.yxz; v_tex2 = a_tex * 0.5; v_tex3 = a_tex.xzy; v_tex4 = a_tex; v_tex5 = a_tex; v_tex6 = a_tex; v_tex7 = a_tex; }\n";

// ---- vertex keys (as tests/vgx_vsout_gl.cpp makes them, lit channels more often)
vgx::VsKey LitVs(Rng& r) {
    vgx::VsKey k;
    k.numTexGens = 0;
    k.numChans = static_cast<uint8_t>(r.P(60) ? 1 : 2);
    k.perVtxPos = static_cast<uint8_t>(r.P(70) ? 0 : 1);
    for (int i = 0; i < 4; ++i) {
        if ((i & 1) >= k.numChans) continue;
        vgx::VsKey::Chan& c = k.chan[i];
        c.matSrcVtx = r.P(50) ? 1 : 0;
        c.enable = r.P(90) ? 1 : 0;
        if (!c.enable) continue;
        c.ambSrcVtx = r.P(20) ? 1 : 0;
        c.lightMask = static_cast<uint8_t>(r.P(60) ? 1u + r.U(7) : 1u + r.U(255));
        c.diffuse = static_cast<uint8_t>(r.P(60) ? 2 : r.U(3));
        c.attn = static_cast<uint8_t>(r.P(60) ? 2 : r.U(3));
    }
    if (r.P(40)) { k.chan[2] = k.chan[0]; if (k.numChans > 1) k.chan[3] = k.chan[1]; } // (colour and alpha alike: GXV_LTMERGE's case)
    return k;
}
struct VVtx { float pos[4], nrm[3], c0[4], c1[4]; };
void DrawVs(GLuint p, const vgx::ShaderSource& vs, uint32_t seed, const std::vector<float>& lt, std::vector<uint8_t>& px) {
    std::mt19937 rng(seed);
    auto rf = [&](float lo, float hi) { return lo + (hi - lo) * static_cast<float>(rng() / 4294967296.0); };
    glUseProgram(p);
    std::vector<float> proj = {0.9f, 0.05f, 0.0f, 0.0f, -0.04f, 0.85f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.02f, -0.03f, 0.0f, 1.0f};
    if (GLint l = glGetUniformLocation(p, "u_proj"); l >= 0) glUniformMatrix4fv(l, 1, GL_FALSE, proj.data());
    std::vector<float> mv(24), pal(96 * 4);
    for (int i = 0; i < 24; ++i) mv[i] = ((i % 4) == (i / 4) % 3 ? 1.0f : 0.0f) + rf(-0.1f, 0.1f);
    for (int row = 0; row < 96; ++row)
        for (int c = 0; c < 4; ++c) pal[row * 4 + c] = ((row % 3) == c ? 1.0f : 0.0f) + rf(-0.15f, 0.15f);
    if (GLint l = glGetUniformLocation(p, "u_mv"); l >= 0) glUniform4fv(l, 6, mv.data());
    if (GLint l = glGetUniformLocation(p, "u_pal"); l >= 0 && vs.nPal) glUniform4fv(l, vs.nPal, pal.data());
    if (GLint l = glGetUniformLocation(p, "u_lt"); l >= 0) glUniform4fv(l, vs.nLt, lt.data());
    std::vector<VVtx> v(16 * 3);
    for (VVtx& x : v) {
        x.pos[0] = rf(-1.0f, 1.0f); x.pos[1] = rf(-1.0f, 1.0f); x.pos[2] = rf(-0.8f, 0.8f); x.pos[3] = static_cast<float>(3 * (rng() % 19));
        for (float& f : x.nrm) f = rf(-1.0f, 1.0f);
        for (float& f : x.c0) f = rf(0.0f, 1.0f);
        for (float& f : x.c1) f = rf(0.0f, 1.0f);
    }
    auto Attr = [&](const char* name, int n, const float* first) {
        const GLint loc = glGetAttribLocation(p, name);
        if (loc < 0) return;
        glEnableVertexAttribArray(static_cast<GLuint>(loc));
        glVertexAttribPointer(static_cast<GLuint>(loc), n, GL_FLOAT, GL_FALSE, sizeof(VVtx), first);
    };
    for (GLuint i = 0; i < 16; ++i) glDisableVertexAttribArray(i);
    Attr("a_pos", 4, v[0].pos); Attr("a_nrm", 3, v[0].nrm); Attr("a_col0", 4, v[0].c0); Attr("a_col1", 4, v[0].c1);
    glDisable(GL_BLEND);
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size()));
    px.assign(kW * kH * 4, 0);
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    Require(glGetError() == GL_NO_ERROR, "GL status");
}
} // namespace

int main(int argc, char** argv) {
    const unsigned nKeys = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 1500;
    std::printf("GXV_FS_TRIM=%ld GXV_VS_TRIM=%ld GXV_FS_HALF=%ld\n", vgx::FsTrimMode(), vgx::VsTrimMode(), vgx::FsHalfMode());
    Require(vgx::FsTrimMode() == 1 && vgx::FsHalfMode() == 1, "run with GXV_FS_TRIM=1 GXV_FS_HALF=1 (tools/test_vgx_trim_gl.sh)");

    // ---- 1. + 2. the analysis on the CPU model
    {
        Rng r(0x7713);
        size_t sides = 0, idle = 0, checked = 0, halfKeys = 0, halfSamples = 0, keysAllIdle = 0;
        double worst = 0.0, worstHalf = 0.0;
        for (unsigned n = 0; n < nKeys * 4; ++n) {
            const vgx::FsKey k = TrimKey(r);
            vgx::FsTrimInfo ti;
            vgx::FsTrimAnalyse(k, ti);
            bool allIdle = true;
            for (int s = 0; s < k.numStages; ++s) {
                sides += 4;
                idle += !ti.c[s].lo + !ti.c[s].hi + !ti.a[s].lo + !ti.a[s].hi;
                allIdle = allIdle && !ti.c[s].lo && !ti.c[s].hi && !ti.a[s].lo && !ti.a[s].hi;
            }
            keysAllIdle += allIdle;
            Trace tr;
            for (int s = 0; s < 16; ++s) for (int p = 0; p < 2; ++p) { tr.lo[s][p] = 1e30f; tr.hi[s][p] = -1e30f; }
            tr.finLo[0] = tr.finLo[1] = 1e30f; tr.finHi[0] = tr.finHi[1] = -1e30f;
            const Model fl;
            for (int i = 0; i < 200; ++i) {
                const Inputs in = RandomInputs(r, k.inWrap != 0);
                float o[4];
                Eval(k, in, nullptr, fl, o, &tr);
            }
            for (int s = 0; s < k.numStages; ++s) {
                const bool clampC = Bits(k.stage[s].color, 19, 1) != 0, clampA = Bits(k.stage[s].alpha, 19, 1) != 0;
                for (int p = 0; p < 2; ++p) {
                    const vgx::FsTrimClamp act = p ? ti.a[s] : ti.c[s];
                    const bool cl = p ? clampA : clampC;
                    const double lo = cl ? 0.0 : -4.0157f, hi = cl ? 1.0 : 4.0118f;
                    ++checked;
                    if (!act.lo) { worst = std::max(worst, lo - tr.lo[s][p]); Require(tr.lo[s][p] >= lo - 1e-5, "an idle low side is never crossed"); }
                    if (!act.hi) { worst = std::max(worst, tr.hi[s][p] - hi); Require(tr.hi[s][p] <= hi + 1e-5, "an idle high side is never crossed"); }
                }
            }
            if (!ti.finC.lo) Require(tr.finLo[0] >= -1e-5, "final colour >= 0 when the analysis says so");
            if (!ti.finC.hi) Require(tr.finHi[0] <= 1.0 + 1e-5, "final colour <= 1 when the analysis says so");
            if (!ti.finA.lo) Require(tr.finLo[1] >= -1e-5, "final alpha >= 0 when the analysis says so");
            if (!ti.finA.hi) Require(tr.finHi[1] <= 1.0 + 1e-5, "final alpha <= 1 when the analysis says so");
            // 2. half under the trim: the precision of the full text, and a half key's text exactly the full one
            const vgx::FsHalfReport rep = vgx::FsHalfAnalyse(k);
            Require(vgx::FsHalfEligible(k) == rep.Eligible(1), "the trim takes no part in the precision decision");
            {
                vgx::ShaderSource prod, full;
                vgx::GenerateFragmentShader(k, prod);
                vgx::GenerateFragmentShaderForm(k, full, rep.Eligible(1), false);
                Require(prod.text == full.text || !rep.Eligible(1), "a half key keeps its full text under GXV_FS_TRIM");
                Require((prod.text.find("hvec4") != std::string::npos) == rep.Eligible(1), "half exactly where the full text's analysis says");
                if (!rep.Eligible(1)) {
                    vgx::ShaderSource trimmed;
                    vgx::GenerateFragmentShaderForm(k, trimmed, false, true);
                    Require(prod.text == trimmed.text, "a float key is the trimmed text");
                }
            }
            if (rep.Eligible(1)) {
                ++halfKeys;
                for (int i = 0; i < 100; ++i) {
                    const Inputs in = RandomInputs(r, false);
                    float ref[4];
                    Eval(k, in, nullptr, fl, ref, nullptr);
                    for (int form = 1; form <= 2; ++form) {
                        Model h;
                        h.half = true; h.mix = form;
                        float o[4];
                        Eval(k, in, nullptr, h, o, nullptr); // (a half key is written with every clamp)
                        ++halfSamples;
                        for (int c = 0; c < (k.dstAlpha ? 3 : 4); ++c) { // (destination alpha: a constant the model does not write)
                            const double err = std::fabs(static_cast<double>(o[c]) - ref[c]) * 255.0, bound = (c < 3 ? rep.colorLevels : rep.alphaLevels) + 1e-3;
                            if (bound > 1e-2) worstHalf = std::max(worstHalf, err / bound);
                            if (err > bound) {
                                std::fprintf(stderr, "half bound: channel %d error %.4f > %.4f (mix form %d)\n", c, err, bound, form);
                                vgx::ShaderSource src;
                                vgx::GenerateFragmentShaderForm(k, src, true, true);
                                std::fprintf(stderr, "%s\n", src.text.c_str());
                            }
                            Require(err <= bound, "the half bound holds");
                        }
                    }
                }
            }
        }
        std::printf("analysis: %u keys, %zu stage parts checked; %zu of %zu clamp sides idle (%.1f%%), %zu keys with no stage clamp left; worst excursion past an idle "
                    "side %.2g (float rounding); half (full text, untrimmed): %zu admitted keys, %zu samples, worst error / bound %.2f\n",
                    nKeys * 4, checked, idle, sides, 100.0 * idle / std::max<size_t>(sides, 1), keysAllIdle, worst, halfKeys, halfSamples, worstHalf);
    }

    Require(vgx::host::CreateContext(), "Mesa GLES context");
    std::printf("GL renderer: %s\n", glGetString(GL_RENDERER));
    GLuint fbo, rt;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &rt); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, rt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "framebuffer");
    glViewport(0, 0, kW, kH);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_DITHER);

    // ---- 3. Mesa, fragment
    {
        Rng r(0x7714);
        size_t keys = 0, changed = 0, bytes = 0, diff = 0, diffPx = 0, halfPairs = 0, clampOps0 = 0, clampOps1 = 0, trimOps = 0;
        int maxd = 0;
        auto Count = [](const std::string& s, const char* what) {
            size_t n = 0;
            for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
            return n;
        };
        for (unsigned n = 0; n < nKeys; ++n) {
            const vgx::FsKey k = TrimKey(r);
            vgx::ShaderSource off, on;
            vgx::GenerateFragmentShaderForm(k, off, false, false);
            vgx::GenerateFragmentShaderForm(k, on, false, true);
            ++keys;
            // (clamp() is a VMIN and a VMAX on the console, max() / min() one each)
            clampOps0 += 2 * Count(off.text, "clamp(") + Count(off.text, "max(") + Count(off.text, "min(") + Count(off.text, "floor(");
            clampOps1 += 2 * Count(on.text, "clamp(") + Count(on.text, "max(") + Count(on.text, "min(") + Count(on.text, "floor(");
            trimOps += on.trimOps;
            if (on.text == off.text) { Require(on.trimOps == 0, "an unchanged text reports nothing left out"); continue; }
            ++changed;
            std::vector<std::pair<std::string, std::string>> forms{{off.text, on.text}};
            const vgx::FsHalfReport rep = vgx::FsHalfAnalyse(k);
            if (rep.Eligible(1)) {
                vgx::ShaderSource h;
                vgx::GenerateFragmentShaderForm(k, h, true, true);
                Require(h.text.find("hvec4") != std::string::npos, "an admitted key has a half text");
                forms.push_back({off.text, h.text});
                ++halfPairs;
            }
            for (const auto& f : forms) {
                const GLuint p0 = Link(kFsVtx, f.first), p1 = Link(kFsVtx, f.second);
                for (uint32_t round = 0; round < 3; ++round) {
                    std::vector<uint8_t> a, b;
                    const uint32_t seed = n * 7919u + round * 104729u + 5u;
                    DrawFs(p0, k, seed, a);
                    DrawFs(p1, k, seed, b);
                    for (size_t j = 0; j < a.size(); j += 4) {
                        int d = 0;
                        for (int c = 0; c < 4; ++c) d = std::max(d, std::abs(static_cast<int>(a[j + c]) - b[j + c]));
                        if (d) {
                            ++diffPx;
                            if (d > 1) {
                                std::fprintf(stderr, "key %u: pixel %zu differs by %d levels\n---- off\n%s\n---- on\n%s\n", n, j / 4, d, f.first.c_str(), f.second.c_str());
                                Require(false, "the trimmed text draws within one level of the full text");
                            }
                        }
                        maxd = std::max(maxd, d);
                    }
                    for (size_t j = 0; j < a.size(); ++j) diff += a[j] != b[j];
                    bytes += a.size();
                }
                glDeleteProgram(p0); glDeleteProgram(p1);
            }
        }
        std::printf("Mesa fragment: %zu keys, %zu with a trimmed text (%zu also as half text); clamp-type operations (VMIN/VMAX/floor count) %zu -> %zu (%.1f%% fewer), "
                    "census %zu; %zu bytes compared, %zu differ (%zu pixels, max %d level)\n",
                    keys, changed, halfPairs, clampOps0, clampOps1, 100.0 * (1.0 - static_cast<double>(clampOps1) / std::max<size_t>(clampOps0, 1)), trimOps, bytes,
                    diff, diffPx, maxd);
        Require(diff * 10000 <= bytes, "at most 1 in 10,000 bytes differ (an ulp past a bound)");
    }

    // ---- 4. Mesa, vertex
    {
        Rng r(0x7715);
        const char* fsPlain = "precision highp float; varying vec4 v_col0; varying vec4 v_col1; void main() { gl_FragColor = vec4(v_col0.rg, v_col1.ba); }\n";
        const char* fsPlain2 = "precision highp float; varying vec4 v_col0; varying vec4 v_col1; void main() { gl_FragColor = vec4(v_col1.rg, v_col0.ba); }\n";
        const char* fsAmp = "precision highp float; varying vec4 v_col0; varying vec4 v_col1; void main() { gl_FragColor = fract(vec4(v_col0.rg, v_col1.ba) * 4096.0); }\n";
        const char* fsAmp2 = "precision highp float; varying vec4 v_col0; varying vec4 v_col1; void main() { gl_FragColor = fract(vec4(v_col1.rg, v_col0.ba) * 4096.0); }\n";
        size_t keys = 0, changed = 0, sameText = 0, terms = 0, pixels = 0, minOnly = 0;
        for (unsigned n = 0; n < nKeys; ++n) {
            const vgx::VsKey k = LitVs(r);
            // light data: per light, the cos / distance attenuation exactly (1, 0, 0) (as SMG's actor lights), or random
            std::vector<float> lt(44 * 4);
            uint8_t cos1 = 0, dist1 = 0;
            for (float& f : lt) f = static_cast<float>(r.F()) * 4.0f - 2.0f;
            for (int i = 0; i < 16; ++i) lt[i] = static_cast<float>(r.U(256)) / 255.0f; // ambient, material: 8-bit
            for (int l = 0; l < 8; ++l) {
                float* L = &lt[(4 + l * 5) * 4];
                for (int c = 0; c < 4; ++c) L[8 + c] = static_cast<float>(r.U(256)) / 255.0f; // colour: 8-bit
                for (int c = 0; c < 3; ++c) L[c] *= 2.0f;                                     // position
                if (r.P(65)) { L[12] = 1.0f; L[13] = r.P(50) ? 0.0f : -0.0f; L[14] = 0.0f; cos1 |= static_cast<uint8_t>(1u << l); }
                else for (int c = 0; c < 3; ++c) L[12 + c] = static_cast<float>(r.F()) * 1.5f - 0.2f;
                if (r.P(65)) { L[16] = 1.0f; L[17] = 0.0f; L[18] = r.P(50) ? 0.0f : -0.0f; dist1 |= static_cast<uint8_t>(1u << l); }
                else for (int c = 0; c < 3; ++c) L[16 + c] = static_cast<float>(r.F()) * 1.5f - 0.2f;
            }
            uint8_t lit = 0;
            for (int i = 0; i < 4; ++i)
                if ((i & 1) < k.numChans && k.chan[i].enable) lit |= k.chan[i].lightMask;
            vgx::VsKey kt = k;
            if (lit) kt.setLtTrim(static_cast<uint8_t>(cos1 & lit), static_cast<uint8_t>(dist1 & lit));
            vgx::ShaderSource off, on;
            vgx::GenerateVertexShader(k, off);
            vgx::GenerateVertexShader(kt, on);
            ++keys;
            if (on.text == off.text) { ++sameText; Require(on.trimOps == 0, "an unchanged vertex text reports nothing left out"); continue; }
            ++changed;
            terms += on.trimOps;
            minOnly += on.text.find("min(l") != std::string::npos;
            for (const char* f : {fsPlain, fsPlain2, fsAmp, fsAmp2}) {
                const GLuint p0 = Link(off.text, f), p1 = Link(on.text, f);
                std::vector<uint8_t> a, b;
                DrawVs(p0, off, n * 31u + 7u, lt, a);
                DrawVs(p1, on, n * 31u + 7u, lt, b);
                if (a != b) {
                    size_t bad = 0;
                    for (size_t j = 0; j < a.size(); ++j) bad += a[j] != b[j];
                    std::fprintf(stderr, "vertex key %u: %zu bytes differ\n---- off\n%s\n---- on\n%s\n", n, bad, off.text.c_str(), on.text.c_str());
                    Require(false, "the trimmed vertex shader draws exactly what the full one draws");
                }
                pixels += kW * kH;
                glDeleteProgram(p0); glDeleteProgram(p1);
            }
        }
        std::printf("Mesa vertex (GXV_LTMERGE from the environment): %zu lit keys, %zu with a trimmed text (%zu with min-only light clamps), %zu unchanged; %zu light terms / "
                    "clamp sides left out; %zu pixels compared (plain and amplified), all equal\n",
                    keys, changed, minOnly, sameText, terms, pixels);
    }
    // ---- 5. regression: Galaxy's ground decal / shadow shapes, exhaustively
    {
        auto Col = [](unsigned a, unsigned b, unsigned c, unsigned d, bool clampOn, unsigned bias = 0, unsigned scale = 0) {
            return (a << 12) | (b << 8) | (c << 4) | d | (bias << 16) | (clampOn ? 1u << 19 : 0u) | (scale << 20);
        };
        auto Alp = [](unsigned a, unsigned b, unsigned c, unsigned d, bool clampOn, unsigned bias = 0, unsigned scale = 0) {
            return (a << 13) | (b << 10) | (c << 7) | (d << 4) | (bias << 16) | (clampOn ? 1u << 19 : 0u) | (scale << 20);
        };
        struct Case { const char* name; vgx::FsKey k; float reg[4][4]; float konst[4][4]; };
        std::vector<Case> cases;
        { // an alpha-tested decal of the opaque scene: colour TEXC * RASC (clamped), alpha TEXA * RASA unclamped, destination alpha 0
            Case c{"ground decal (tex*ras, alpha unclamped, alpha test, GXSetDstAlpha(TRUE, 0))", {}, {}, {}};
            c.k.numStages = 1; c.k.numTexGens = 1;
            c.k.stage[0].color = Col(15, 8, 10, 15, true); c.k.stage[0].alpha = Alp(7, 4, 5, 7, false);
            c.k.stage[0].texEnable = 1; c.k.stage[0].rasChan = 0;
            c.k.alphaTestOn = 1; c.k.dstAlpha = 1;
            cases.push_back(c);
        }
        { // MR::setupShadowVolumeDraw + ShadowVolumeDrawer::draw: C0 / A0 = (0xC0, 0xC0, 0, 4), ONE / ONE then SUBTRACT into alpha
            Case c{"shadow volume (C0, A0)", {}, {{0, 0, 0, 0}, {192 / 255.f, 192 / 255.f, 0, 4 / 255.f}}, {}};
            c.k.numStages = 1;
            c.k.stage[0].color = Col(2, 15, 15, 15, true); c.k.stage[0].alpha = Alp(1, 7, 7, 7, true); c.k.stage[0].rasChan = 0;
            cases.push_back(c);
        }
        { // MR::fillSilhouetteColor: alpha = TEXA > K0_A (1/255) ? A0 (0x80) : 0 (COMP_A8_GT), alpha test GREATER 0
            Case c{"shadow fill (A8 compare, alpha test)", {}, {{0, 0, 0, 0}, {0, 0, 0, 128 / 255.f}}, {{0, 0, 0, 1 / 255.f}}};
            c.k.numStages = 1; c.k.numTexGens = 1;
            c.k.stage[0].color = Col(2, 15, 15, 15, true); c.k.stage[0].alpha = Alp(4, 6, 1, 7, true, 3, 3);
            c.k.stage[0].texEnable = 1; c.k.stage[0].rasChan = 7; c.k.stage[0].kasel = 28;
            c.k.alphaTestOn = 1;
            cases.push_back(c);
        }
        { // FallOutFieldDraw::setUpEdgeAndClearAlpha: colour C1, alpha A1 * TEXA, destination alpha 0
            Case c{"fall-out field edge (C1, A1 * TEXA)", {}, {{0, 0, 0, 0}, {0, 0, 0, 0}, {40 / 255.f, 90 / 255.f, 200 / 255.f, 1.0f}}, {}};
            c.k.numStages = 1; c.k.numTexGens = 1;
            c.k.stage[0].color = Col(4, 15, 15, 15, true); c.k.stage[0].alpha = Alp(7, 2, 4, 7, true);
            c.k.stage[0].texEnable = 1; c.k.stage[0].rasChan = 7; c.k.dstAlpha = 1;
            cases.push_back(c);
        }
        // What the old trim did to the decal (GXV_TEVWRAP=2, the console's: its full text wraps the final alpha, so it was float): a half
        // text with the trimmed clamps, whose alpha test the half analysis did not bound (destination alpha). The half model, every 8-bit
        // texel alpha x vertex alpha x reference, against the float full text: alpha-test decisions that differ.
        {
            const vgx::FsKey& k = cases[0].k;
            vgx::FsTrimInfo ti;
            vgx::FsTrimAnalyse(k, ti);
            vgx::FsKey probe = k;
            probe.dstAlpha = 0;
            size_t flips = 0, tests = 0;
            for (int form = 1; form <= 2; ++form)
                for (int x = 0; x < 256; ++x)
                    for (int y = 0; y < 256; ++y) {
                        Inputs in{};
                        for (int c = 0; c < 4; ++c) { in.tex[0][c] = x / 255.0f; in.col[0][c] = y / 255.0f; }
                        float full[4], half[4];
                        Model h; h.half = true; h.mix = form;
                        Eval(probe, in, nullptr, Model{}, full, nullptr);
                        Eval(probe, in, &ti, h, half, nullptr);
                        const float af = std::floor(full[3] * 255.0f + 0.5f), ah = std::floor(R16(half[3]) * 255.0f + 0.5f);
                        for (int ref = 0; ref < 256; ++ref) { ++tests; flips += (af >= ref) != (ah >= ref); }
                    }
            const vgx::FsHalfReport tested = vgx::FsHalfAnalyse(probe);
            std::printf("regression: the decal in half with the trimmed clamps (the old GXV_FS_TRIM text): %zu of %zu GEQUAL alpha-test decisions differ "
                        "from the float full text; production now: %s\n", flips, tests, vgx::FsHalfEligible(k) ? "half, full text" : "float, trimmed text");
            (void)tested;
        }
        const int N = 256;
        GLuint fbo2, rt2, tex[8];
        glGenFramebuffers(1, &fbo2);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo2);
        glGenTextures(1, &rt2); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, rt2);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt2, 0);
        Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "framebuffer 256");
        glViewport(0, 0, N, N);
        // texel (x, y): alpha x, colour bytes from x and y; the vertex colours run 0 -> 1 (col1 1 -> 0) up the target
        glGenTextures(8, tex);
        {
            std::vector<uint8_t> t(N * N * 4);
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x) {
                    uint8_t* p = &t[(y * N + x) * 4];
                    p[0] = static_cast<uint8_t>(y); p[1] = static_cast<uint8_t>(255 - x); p[2] = static_cast<uint8_t>((x * 7 + y * 13) & 255); p[3] = static_cast<uint8_t>(x);
                }
            for (int m = 0; m < 8; ++m) {
                glActiveTexture(GL_TEXTURE0 + m);
                glBindTexture(GL_TEXTURE_2D, tex[m]);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            }
        }
        const FVtx quad[6] = {
            {{-1, -1, 0, 1}, {0, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 1}}, {{1, -1, 0, 1}, {0, 0, 0, 0}, {1, 1, 1, 1}, {1, 0, 1}}, {{1, 1, 0, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {1, 1, 1}},
            {{-1, -1, 0, 1}, {0, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 1}}, {{1, 1, 0, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {1, 1, 1}}, {{-1, 1, 0, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 1, 1}}};
        auto Draw = [&](GLuint p, const Case& c, int r0, int r1, std::vector<uint8_t>& px) {
            glUseProgram(p);
            std::vector<float> tev(28 * 4, 0.0f);
            for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { tev[i * 4 + j] = c.reg[i][j]; tev[16 + i * 4 + j] = c.konst[i][j]; }
            tev[32] = static_cast<float>(r0); tev[33] = static_cast<float>(r1);
            for (int i = 84; i < 100; ++i) tev[i] = static_cast<float>(N);
            if (GLint l = glGetUniformLocation(p, "u_tev"); l >= 0) glUniform4fv(l, 28, tev.data());
            for (int m = 0; m < 8; ++m) {
                char name[16];
                std::snprintf(name, sizeof name, "u_tex%d", m);
                if (GLint l = glGetUniformLocation(p, name); l >= 0) glUniform1i(l, m);
            }
            auto Attr = [&](const char* name, int n, const float* first) {
                const GLint loc = glGetAttribLocation(p, name);
                if (loc < 0) return;
                glEnableVertexAttribArray(static_cast<GLuint>(loc));
                glVertexAttribPointer(static_cast<GLuint>(loc), n, GL_FLOAT, GL_FALSE, sizeof(FVtx), first);
            };
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            for (GLuint i = 0; i < 16; ++i) glDisableVertexAttribArray(i);
            Attr("a_pos", 4, quad[0].pos); Attr("a_col0", 4, quad[0].c0); Attr("a_col1", 4, quad[0].c1); Attr("a_tex", 3, quad[0].t);
            glDisable(GL_BLEND);
            glClearColor(17.0f / 255, 33.0f / 255, 49.0f / 255, 65.0f / 255);
            glClear(GL_COLOR_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            px.assign(N * N * 4, 0);
            glReadPixels(0, 0, N, N, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            Require(glGetError() == GL_NO_ERROR, "GL status 256");
        };
        size_t draws = 0, bytes = 0, changedTexts = 0;
        // (the production parts, GXV_FS_TRIM_PARTS from the environment, and every part at once: 63, the r76-r90 text)
        const long maskList[2] = {vgx::FsTrimParts(), vgx::kFsTrimAll};
        for (int mi = 0; mi < (maskList[0] == maskList[1] ? 1 : 2); ++mi)
        for (Case& c : cases) {
            const bool tested = c.k.alphaTestOn != 0;
            for (int comp = tested ? 1 : 7; comp <= (tested ? 6 : 7); ++comp) {
                for (int rej = 0; rej <= (tested ? 1 : 0); ++rej) { // (GXV_DISCARD_BLEND's variant: a failing fragment writes vec4(0))
                    vgx::FsKey k = c.k;
                    if (tested) k.alphaTest = static_cast<uint8_t>(comp | (7 << 3));
                    k.setRejectZero(static_cast<uint8_t>(rej));
                    vgx::ShaderSource full, prod;
                    vgx::GenerateFragmentShaderForm(k, full, vgx::FsHalfEligible(k), false);
                    if (mi == 0) vgx::GenerateFragmentShader(k, prod); // (GXV_FS_TRIM=1, GXV_FS_HALF=1: the production text)
                    else vgx::GenerateFragmentShaderParts(k, prod, vgx::FsHalfEligible(k), maskList[1]);
                    changedTexts += prod.text != full.text;
                    if (vgx::FsHalfEligible(k)) Require(prod.text == full.text, "a half key keeps its full text");
                    const GLuint p0 = Link(kFsVtx, full.text), p1 = Link(kFsVtx, prod.text);
                    for (int ref = 0; ref < (tested ? 256 : 1); ++ref) {
                        std::vector<uint8_t> a, b;
                        Draw(p0, c, ref, ref, a);
                        Draw(p1, c, ref, ref, b);
                        if (a != b) {
                            size_t bad = 0;
                            for (size_t j = 0; j < a.size(); ++j) bad += a[j] != b[j];
                            std::fprintf(stderr, "%s, compare %d, reference %d: %zu bytes differ\n---- full\n%s\n---- production\n%s\n", c.name, comp, ref, bad,
                                         full.text.c_str(), prod.text.c_str());
                            Require(false, "Galaxy's shapes: the production text draws exactly the full text");
                        }
                        ++draws;
                        bytes += a.size();
                    }
                    glDeleteProgram(p0); glDeleteProgram(p1);
                }
            }
        }
        glDeleteTextures(8, tex);
        glDeleteTextures(1, &rt2);
        glDeleteFramebuffers(1, &fbo2);
        std::printf("regression (Galaxy's decal / shadow volume / shadow fill / fall-out edge): %zu exhaustive draws (every texel alpha, vertex colour 0..1, "
                    "every compare and reference; parts %ld and %ld), %zu bytes, all equal; %zu of the texts trimmed\n", draws, maskList[0], maskList[1], bytes, changedTexts);
    }
    // ---- 6. GXV_FS_TRIM_PARTS: each part alone and all together, against the full text (as part 3); 0 is the full text byte for byte
    {
        Rng r(0x7716);
        const long masks[8] = {0, vgx::kFsTrimColourClamps, vgx::kFsTrimFloor, vgx::kFsTrimFinalWrap, vgx::kFsTrimAlphaClamps, vgx::kFsTrimFinalClamp,
                                vgx::kFsTrimColourClamps | vgx::kFsTrimCompareKeys, vgx::kFsTrimAll};
        size_t changed[8] = {}, bytes = 0, diff = 0;
        for (unsigned n = 0; n < nKeys / 4 + 1; ++n) {
            const vgx::FsKey k = TrimKey(r);
            vgx::ShaderSource full;
            vgx::GenerateFragmentShaderForm(k, full, false, false);
            GLuint p0 = 0;
            for (int m = 0; m < 8; ++m) {
                vgx::ShaderSource t;
                vgx::GenerateFragmentShaderParts(k, t, false, masks[m]);
                if (masks[m] == 0) { Require(t.text == full.text && t.trimOps == 0, "GXV_FS_TRIM_PARTS=0 is the full text"); continue; }
                if (t.text == full.text) continue;
                ++changed[m];
                if (!p0) { glBindFramebuffer(GL_FRAMEBUFFER, fbo); glViewport(0, 0, kW, kH); p0 = Link(kFsVtx, full.text); }
                const GLuint p1 = Link(kFsVtx, t.text);
                for (uint32_t round = 0; round < 2; ++round) {
                    std::vector<uint8_t> a, b;
                    const uint32_t seed = n * 6151u + round * 3079u + static_cast<uint32_t>(m);
                    DrawFs(p0, k, seed, a);
                    DrawFs(p1, k, seed, b);
                    for (size_t j = 0; j < a.size(); ++j) {
                        const int d = std::abs(static_cast<int>(a[j]) - b[j]);
                        diff += d != 0;
                        if (d > 1) {
                            std::fprintf(stderr, "parts %ld, key %u: byte %zu differs by %d\n---- full\n%s\n---- parts\n%s\n", masks[m], n, j, d, full.text.c_str(), t.text.c_str());
                            Require(false, "each trim part draws within one level of the full text");
                        }
                    }
                    bytes += a.size();
                }
                glDeleteProgram(p1);
            }
            if (p0) glDeleteProgram(p0);
        }
        std::printf("parts: texts changed by colour clamps %zu, floor %zu, final wrap %zu, alpha clamps %zu, final clamp %zu, colour incl. compare keys %zu, "
                    "all %zu (of %u keys); %zu bytes compared, %zu differ\n", changed[1], changed[2], changed[3], changed[4], changed[5], changed[6], changed[7],
                    nKeys / 4 + 1, bytes, diff);
        Require(diff * 10000 <= bytes, "parts: at most 1 in 10,000 bytes differ");
    }
    std::printf("PASS\n");
    return 0;
}
