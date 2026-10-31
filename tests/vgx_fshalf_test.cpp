// GXV_FS_HALF: half precision fragment shaders for the keys where it cannot show. No game data and no Vita needed.
//
//   1. Byte identity: with the switch off (unset, 0, invalid) and with 2 (every shader: vitaGL patch 0012, the text is not touched) the
//      generated source of 3000 random keys of every shape is the one of the generator before the switch (a pinned digest). With 1 and
//      3 only the keys FsHalfEligible takes change, and they change into the text GenerateFragmentShaderPrecision(.., true) makes.
//   2. The analysis (FsHalfAnalyse): refuses what it must; its bound is SOUND: for random eligible keys and random inputs, a CPU model of the
//      generated TEV pipeline in float and in half (a half rounding after EVERY operation, with both usual forms of mix) never differs
//      before the final 8-bit rounding by more than the bound the analysis gave.
//   3. How often the 8-bit result is the same (reported; the guarantee is "at most one level", not "always the same level").
//   4. On Mesa: the half text of an eligible key (on a PC the half types are plain floats) draws exactly what the float text draws, and the
//      CPU model agrees with what Mesa draws (this ties the model of 2. and 3. to the real generated text).
// Optional first argument: a vgx_shaderlist.bin (the warm-up list); its fragment keys are also run through 2. and 3. and counted.
#include "vgx_fshalf_keys.h"
#include "vgx_shadergen.h"
#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace vgx { bool LogEnabled() { return false; } namespace host { bool CreateContext(); } }
namespace {
using namespace fshalf_keys;
constexpr uint64_t kBaseDigest = 0xe7c6226babda9132ull; // SourceDigest of the generator before GXV_FS_HALF

void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
unsigned Bits(uint32_t v, int shift, int n) { return (v >> shift) & ((1u << n) - 1u); }

// ---- half precision on the host: round to nearest even to 11 significant bits (subnormals below 2^-14, infinity above 65504)
float R16(float x) {
    if (x == 0.0f || !std::isfinite(x)) return x;
    int e = 0;
    std::frexp(x, &e); // x = m * 2^e, 0.5 <= |m| < 1: the spacing of half there is 2^(e - 11), at least 2^-24
    const float q = std::ldexp(1.0f, std::max(e - 11, -24));
    const float y = std::nearbyint(x / q) * q; // (x / q and the product are exact; nearbyint rounds to nearest even)
    return std::fabs(y) > 65504.0f ? std::copysign(INFINITY, x) : y;
}

// ---- the TEV pipeline as GenerateFragmentShader writes it (no indirect stages, no fog, no compare-mode stage), float or half
bool g_half = false;
int g_mix = 2; // half path: which form the target compiler might make of mix(): 1 a + c * (b - a); 2 a * (1 - c) + b * c
inline float Rnd(float x) { return g_half ? R16(x) : x; }
inline float In(float x) { return Rnd(x); }
inline float Add(float a, float b) { return Rnd(a + b); }
inline float Sub(float a, float b) { return Rnd(a - b); }
inline float Mul(float a, float b) { return Rnd(a * b); }
inline float Mix(float a, float b, float c) { return g_mix == 1 ? Add(a, Mul(c, Sub(b, a))) : Add(Mul(a, Sub(1.0f, c)), Mul(b, c)); }
inline float Clamp(float x, float lo, float hi) { return std::min(std::max(x, lo), hi); }

struct Inputs {
    float tex[8][4];       // sampler results per texture map
    float col[2][4];       // v_col0, v_col1 (interpolated)
    float reg[4][4];       // u_tev[0..3]: prev, c0, c1, c2 (n / 255)
    float konst[4][4];     // u_tev[4..7] (n / 255)
    int ref0, ref1;        // alpha-test references 0..255
    float dstAlpha;        // u_tev[8].z
};
struct Result {
    float v[4];   // final value before the 8-bit rounding
    int q[4];     // after it (as the framebuffer does: round to nearest)
    bool discard = false;
};
bool CompareTrue(uint32_t c, float ai, float ref) {
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
Result Eval(const vgx::FsKey& k, const Inputs& in) {
    float reg[4][4], tex[4] = {1, 1, 1, 1}, ras[4] = {0, 0, 0, 0}, kc[3] = {1, 1, 1}, ka = 1.0f;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) reg[r][c] = In(in.reg[r][c]);
    for (int c = 0; c < 4; ++c) {
        if (k.regFromVtx & 1) reg[1][c] = In(in.col[0][c]);
        if (k.regFromVtx & 2) reg[2][c] = In(in.col[1][c]);
    }
    const float clampLo = g_half ? R16(-4.0157f) : -4.0157f, clampHi = g_half ? R16(4.0118f) : 4.0118f;
    const float lo01 = 0.0f, hi01 = 1.0f;
    for (int s = 0; s < k.numStages; ++s) {
        const vgx::FsKey::Stage& st = k.stage[s];
        if (st.texEnable) {
            for (int i = 0; i < 4; ++i) tex[i] = In(in.tex[st.texMap][(st.texSwap >> (2 * i)) & 3]);
        } else if (st.texOff) {
            for (int i = 0; i < 4; ++i) tex[i] = 1.0f;
        }
        if (st.rasChan < 2) {
            for (int i = 0; i < 4; ++i) ras[i] = In(in.col[st.rasChan][(st.rasSwap >> (2 * i)) & 3]);
        } else {
            for (int i = 0; i < 4; ++i) ras[i] = 0.0f;
        }
        const uint32_t tc = st.color, ta = st.alpha;
        bool kcUsed = false, kaUsed = false;
        for (int i = 0; i < 4; ++i) {
            if (Bits(tc, i * 4, 4) == 14) kcUsed = true;
            if (Bits(ta, 4 + i * 3, 3) == 6) kaUsed = true;
        }
        if (kcUsed) {
            const unsigned sel = st.kcsel;
            for (int i = 0; i < 3; ++i) {
                if (sel < 8) kc[i] = (8 - sel) / 8.0f;
                else if (sel < 12) kc[i] = 0.0f;
                else {
                    static const int comp[5][3] = {{0, 1, 2}, {0, 0, 0}, {1, 1, 1}, {2, 2, 2}, {3, 3, 3}};
                    kc[i] = In(in.konst[sel & 3][comp[(sel - 12) >> 2][i]]);
                }
            }
        }
        if (kaUsed) {
            const unsigned sel = st.kasel;
            ka = sel < 8 ? (8 - sel) / 8.0f : sel < 16 ? 0.0f : In(in.konst[sel & 3][(sel - 16) >> 2]);
        }
        auto ColorArg = [&](unsigned a, int i) -> float {
            switch (a & 15) {
            case 0: return reg[0][i];  case 1: return reg[0][3];
            case 2: return reg[1][i];  case 3: return reg[1][3];
            case 4: return reg[2][i];  case 5: return reg[2][3];
            case 6: return reg[3][i];  case 7: return reg[3][3];
            case 8: return tex[i];     case 9: return tex[3];
            case 10: return ras[i];    case 11: return ras[3];
            case 12: return 1.0f;      case 13: return 0.5f;
            case 14: return kc[i];     default: return 0.0f;
            }
        };
        auto AlphaArg = [&](unsigned a) -> float {
            switch (a & 7) {
            case 0: return reg[0][3]; case 1: return reg[1][3]; case 2: return reg[2][3]; case 3: return reg[3][3];
            case 4: return tex[3];    case 5: return ras[3];    case 6: return ka;        default: return 0.0f;
            }
        };
        auto Combine = [&](uint32_t reg32, float A, float B, float C, float D) {
            const uint32_t bias = Bits(reg32, 16, 2), op = Bits(reg32, 18, 1), clampOn = Bits(reg32, 19, 1), scale = Bits(reg32, 20, 2);
            float r = Mix(A, B, C);
            r = op ? Sub(D, r) : Add(D, r);
            if (bias == 1) r = Add(r, 0.5f);
            else if (bias == 2) r = Sub(r, 0.5f);
            static const float kScale[4] = {1.0f, 2.0f, 4.0f, 0.5f};
            if (scale) r = Rnd(r * kScale[scale]);
            return clampOn ? Clamp(r, lo01, hi01) : Clamp(r, clampLo, clampHi);
        };
        float rc[3], ra;
        for (int i = 0; i < 3; ++i) rc[i] = Combine(tc, ColorArg(Bits(tc, 12, 4), i), ColorArg(Bits(tc, 8, 4), i), ColorArg(Bits(tc, 4, 4), i), ColorArg(Bits(tc, 0, 4), i));
        ra = Combine(ta, AlphaArg(Bits(ta, 13, 3)), AlphaArg(Bits(ta, 10, 3)), AlphaArg(Bits(ta, 7, 3)), AlphaArg(Bits(ta, 4, 3)));
        for (int i = 0; i < 3; ++i) reg[Bits(tc, 22, 2)][i] = rc[i];
        reg[Bits(ta, 22, 2)][3] = ra;
    }
    const vgx::FsKey::Stage& last = k.stage[k.numStages - 1];
    Result res;
    for (int i = 0; i < 3; ++i) res.v[i] = Clamp(reg[Bits(last.color, 22, 2)][i], lo01, hi01);
    res.v[3] = Clamp(reg[Bits(last.alpha, 22, 2)][3], lo01, hi01);
    if (k.alphaTestOn) {
        const float ai = std::floor(res.v[3] * 255.0f + 0.5f); // (float in the half shader too)
        const bool e0 = CompareTrue(k.alphaTest & 7, ai, static_cast<float>(in.ref0)), e1 = CompareTrue((k.alphaTest >> 3) & 7, ai, static_cast<float>(in.ref1));
        bool pass = false;
        switch ((k.alphaTest >> 6) & 3) { case 0: pass = e0 && e1; break; case 1: pass = e0 || e1; break; case 2: pass = e0 != e1; break; default: pass = e0 == e1; }
        res.discard = !pass;
    }
    if (k.dstAlpha) res.v[3] = In(in.dstAlpha);
    for (int i = 0; i < 4; ++i) res.q[i] = static_cast<int>(std::floor(res.v[i] * 255.0f + 0.5f));
    return res;
}

float Sample(Rng& r, bool grid) { // 8-bit values, or any value in between (a filtered texel, an interpolated colour), with the ends over-represented
    const unsigned w = r.U(100);
    if (w < 6) return 0.0f;
    if (w < 12) return 1.0f;
    if (grid || w < 45) return static_cast<float>(r.U(256)) / 255.0f;
    return static_cast<float>(r.F());
}
Inputs RandomInputs(Rng& r) {
    Inputs in;
    for (auto& t : in.tex) for (float& c : t) c = Sample(r, false);
    for (auto& t : in.col) for (float& c : t) c = Sample(r, false);
    for (auto& t : in.reg) for (float& c : t) c = Sample(r, true);
    for (auto& t : in.konst) for (float& c : t) c = Sample(r, true);
    in.ref0 = static_cast<int>(r.U(256)); in.ref1 = static_cast<int>(r.U(256));
    // alpha test references near the values the pipeline tends to make are the dangerous ones: half of them are the 8-bit alpha of the texture or colour
    if (r.P(50)) in.ref0 = static_cast<int>(std::floor(in.tex[0][3] * 255.0f + 0.5f));
    if (r.P(50)) in.ref1 = static_cast<int>(std::floor(in.col[0][3] * 255.0f + 0.5f));
    in.dstAlpha = static_cast<float>(r.U(256)) / 255.0f;
    return in;
}

struct Totals {
    size_t keys = 0, samples = 0, same = 0, off1 = 0, off2 = 0;   // fragments with the same 8-bit result / one level off / more
    size_t sameColor = 0;                                         // colour channels only
    size_t discardKeys = 0, discardSamples = 0, discardFlips = 0; // alpha-tested keys
    int maxDiff = 0;
    double worstRatio = 0.0;                                      // largest (measured pre-rounding error) / (bound)
    void Add(const Totals& o) {
        keys += o.keys; samples += o.samples; same += o.same; off1 += o.off1; off2 += o.off2; sameColor += o.sameColor;
        discardKeys += o.discardKeys; discardSamples += o.discardSamples; discardFlips += o.discardFlips;
        maxDiff = std::max(maxDiff, o.maxDiff); worstRatio = std::max(worstRatio, o.worstRatio);
    }
};
// Runs one eligible key through float and half (both forms of mix) on `n` random inputs. Fails if the bound is ever exceeded.
Totals RunKey(const vgx::FsKey& k, const vgx::FsHalfReport& rep, Rng& r, unsigned n) {
    Totals t;
    t.keys = 1;
    if (rep.alphaTested) ++t.discardKeys;
    for (unsigned i = 0; i < n; ++i) {
        const Inputs in = RandomInputs(r);
        g_half = false; g_mix = 2;
        const Result ref = Eval(k, in);
        for (int form = 1; form <= 2; ++form) {
            g_half = true; g_mix = form;
            const Result h = Eval(k, in);
            g_half = false;
            // soundness of the bound, before the 8-bit rounding (a small allowance for the float shader's own rounding)
            for (int c = 0; c < 4; ++c) {
                const double err = std::fabs(static_cast<double>(h.v[c]) - ref.v[c]) * 255.0;
                const double bound = (c < 3 ? rep.colorLevels : rep.alphaLevels) + 1e-3;
                if (err > bound) {
                    std::fprintf(stderr, "bound violated: channel %d error %.4f levels > bound %.4f (mix form %d, %u stages)\n", c, err, bound, form, k.numStages);
                    Require(false, "the analysis bound holds for every input");
                }
                if (bound > 1e-2) t.worstRatio = std::max(t.worstRatio, err / bound);
            }
            ++t.samples;
            int d = 0, dc = 0;
            for (int c = 0; c < 4; ++c) {
                d = std::max(d, std::abs(h.q[c] - ref.q[c]));
                if (c < 3) dc = std::max(dc, std::abs(h.q[c] - ref.q[c]));
            }
            t.maxDiff = std::max(t.maxDiff, d);
            if (h.discard != ref.discard) { ++t.discardFlips; ++t.off2; continue; } // (counted as a different fragment, whatever the level)
            if (k.alphaTestOn) ++t.discardSamples;
            if (d == 0) ++t.same; else if (d == 1) ++t.off1; else ++t.off2;
            if (dc == 0) ++t.sameColor;
        }
    }
    return t;
}

// ---- the warm-up list
std::vector<vgx::FsKey> ReadList(const char* path) {
    std::vector<vgx::FsKey> keys;
    FILE* f = std::fopen(path, "rb");
    if (!f) return keys;
    struct Header { char magic[8]; uint32_t vs, fs, version, reserved; } h;
    if (std::fread(&h, sizeof h, 1, f) != 1 || std::memcmp(h.magic, "VGXSHL01", 8) != 0 || h.fs != sizeof(vgx::FsKey)) { std::fclose(f); return keys; }
    uint32_t type;
    while (std::fread(&type, 4, 1, f) == 1) {
        if (type == 1) { vgx::VsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; }
        else if (type == 2) { vgx::FsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; keys.push_back(k); }
        else if (type == 3) { uint32_t v[2]; if (std::fread(v, sizeof v, 1, f) != 1) break; }
        else if (type == 4) { uint32_t v[4]; if (std::fread(v, sizeof v, 1, f) != 1) break; }
        else break;
    }
    std::fclose(f);
    return keys;
}

void Print(const char* what, const Totals& t) {
    const double n = static_cast<double>(std::max<size_t>(t.samples, 1));
    std::printf("  %-34s %4zu keys %8zu fragments: same 8-bit result %.2f%%, one level off %.2f%%, more or discard flips %.2f%% (max %d); colour channels same %.2f%%; worst error/bound %.2f\n",
                what, t.keys, t.samples, 100.0 * t.same / n, 100.0 * t.off1 / n, 100.0 * t.off2 / n, t.maxDiff, 100.0 * t.sameColor / n, t.worstRatio);
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
GLuint Link(const std::string& vtx, const std::string& frag) {
    GLuint vs = Compile(GL_VERTEX_SHADER, vtx), fs = Compile(GL_FRAGMENT_SHADER, frag);
    GLuint p = glCreateProgram(); glAttachShader(p, vs); glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos"); glBindAttribLocation(p, 1, "a_col0"); glBindAttribLocation(p, 2, "a_col1"); glBindAttribLocation(p, 3, "a_tex");
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok); Require(ok, "generated shader links");
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}
void Uniform4(GLuint p, int i, const float v[4]) {
    const GLint loc = glGetUniformLocation(p, ("u_tev[" + std::to_string(i) + "]").c_str());
    if (loc >= 0) glUniform4fv(loc, 1, v);
}

struct GlTotals { size_t keys = 0, frags = 0, textSame = 0, modelSame = 0, modelOff1 = 0, modelMore = 0, discardAgree = 0, discardFrags = 0; };
GlTotals RunGl(const std::vector<vgx::FsKey>& keys, Rng& r, unsigned trials) {
    GlTotals gt;
    const std::string vtx =
        "attribute vec4 a_pos; attribute vec4 a_col0; attribute vec4 a_col1; attribute vec2 a_tex;\n"
        "varying vec4 v_col0; varying vec4 v_col1;\n"
        "varying vec3 v_tex0; varying vec3 v_tex1; varying vec3 v_tex2; varying vec3 v_tex3; varying vec3 v_tex4; varying vec3 v_tex5; varying vec3 v_tex6; varying vec3 v_tex7;\n"
        "void main() { gl_Position = a_pos; v_col0 = a_col0; v_col1 = a_col1; vec3 t = vec3(a_tex, 1.0);\n"
        "  v_tex0 = t; v_tex1 = t; v_tex2 = t; v_tex3 = t; v_tex4 = t; v_tex5 = t; v_tex6 = t; v_tex7 = t; }\n";
    GLuint tex[8];
    glGenTextures(8, tex);
    for (int m = 0; m < 8; ++m) {
        glActiveTexture(GL_TEXTURE0 + m); glBindTexture(GL_TEXTURE_2D, tex[m]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        const uint8_t z[4] = {};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, z);
    }
    const float tri[] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    GLuint vbo = 0; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr); glEnableVertexAttribArray(0);
    glVertexAttrib2f(3, 0.5f, 0.5f);
    for (const vgx::FsKey& k : keys) {
        vgx::ShaderSource sf, sh;
        vgx::GenerateFragmentShaderPrecision(k, sf, false);
        vgx::GenerateFragmentShaderPrecision(k, sh, true);
        Require(sh.text.find("hvec4") != std::string::npos && sf.text.find("hvec") == std::string::npos, "the half text declares half variables, the float text none");
        const GLuint pf = Link(vtx, sf.text), ph = Link(vtx, sh.text);
        ++gt.keys;
        for (unsigned t = 0; t < trials; ++t) {
            Inputs in = RandomInputs(r);
            // what the pipeline reads, as the GPU holds it: 8-bit textures; interpolated colours and registers as exact floats
            for (auto& tx : in.tex) for (float& c : tx) c = std::floor(c * 255.0f + 0.5f) / 255.0f;
            for (int m = 0; m < 8; ++m) {
                uint8_t b[4];
                for (int c = 0; c < 4; ++c) b[c] = static_cast<uint8_t>(std::floor(in.tex[m][c] * 255.0f + 0.5f));
                glActiveTexture(GL_TEXTURE0 + m); glBindTexture(GL_TEXTURE_2D, tex[m]);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, b);
            }
            glVertexAttrib4f(1, in.col[0][0], in.col[0][1], in.col[0][2], in.col[0][3]);
            glVertexAttrib4f(2, in.col[1][0], in.col[1][1], in.col[1][2], in.col[1][3]);
            uint8_t px[2][4];
            int w = 0;
            for (GLuint p : {pf, ph}) {
                glUseProgram(p);
                for (int m = 0; m < 8; ++m) {
                    const GLint loc = glGetUniformLocation(p, ("u_tex" + std::to_string(m)).c_str());
                    if (loc >= 0) glUniform1i(loc, m);
                }
                for (int i = 0; i < 4; ++i) Uniform4(p, i, in.reg[i]);
                for (int i = 0; i < 4; ++i) Uniform4(p, 4 + i, in.konst[i]);
                const float misc[4] = {static_cast<float>(in.ref0), static_cast<float>(in.ref1), in.dstAlpha, 0};
                Uniform4(p, 8, misc);
                glClearColor(17.0f / 255, 33.0f / 255, 49.0f / 255, 65.0f / 255); glClear(GL_COLOR_BUFFER_BIT);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[w++]);
            }
            Require(glGetError() == GL_NO_ERROR, "GL status");
            ++gt.frags;
            if (std::memcmp(px[0], px[1], 4) == 0) ++gt.textSame;
            else Require(false, "the half text draws exactly what the float text draws (on a PC the half types are floats)");
            // the CPU model against Mesa, float path
            g_half = false; g_mix = 2;
            const Result m = Eval(k, in);
            const bool glDiscard = px[0][0] == 17 && px[0][1] == 33 && px[0][2] == 49 && px[0][3] == 65;
            if (k.alphaTestOn) {
                ++gt.discardFrags;
                if (glDiscard == m.discard) ++gt.discardAgree;
            }
            if (m.discard || glDiscard) continue; // (a fragment both discard is left out; one only of them: counted above)
            int d = 0;
            for (int c = 0; c < 4; ++c) d = std::max(d, std::abs(static_cast<int>(px[0][c]) - m.q[c]));
            if (d == 0) ++gt.modelSame; else if (d == 1) ++gt.modelOff1; else ++gt.modelMore;
        }
        glDeleteProgram(pf); glDeleteProgram(ph);
    }
    return gt;
}

} // namespace

int main(int argc, char** argv) {
    const long mode = vgx::FsHalfMode();
    std::printf("GXV_FS_HALF=%ld\n", mode);

    // ---- 1. byte identity
    unsigned nk = 0;
    const uint64_t digest = SourceDigest(&nk);
    std::printf("digest (%u keys) %016llx\n", nk, static_cast<unsigned long long>(digest));
    if (mode == 0 || mode == 2) Require(digest == kBaseDigest, "switch off or 2: the generated sources are byte-identical to the generator before GXV_FS_HALF");
    else Require(digest != kBaseDigest, "switch 1 / 3 changes the text of some keys");
    {
        Rng r(77);
        size_t changed = 0, total = 3000;
        for (size_t i = 0; i < total; ++i) {
            const vgx::FsKey k = RandomKey(r, true);
            vgx::ShaderSource src, flt, hlf;
            vgx::GenerateFragmentShader(k, src);
            vgx::GenerateFragmentShaderPrecision(k, flt, false);
            vgx::GenerateFragmentShaderPrecision(k, hlf, true);
            const bool eligible = vgx::FsHalfEligible(k);
            Require(eligible == ((mode == 1 || mode == 3) && vgx::FsHalfAnalyse(k).Eligible(mode)), "FsHalfEligible follows the mode");
            Require(src.text == (eligible ? hlf.text : flt.text), "the generated text is the half text for an eligible key and the float text for every other key");
            Require((src.text.find("hvec") != std::string::npos) == eligible, "only eligible keys mention the half types");
            if (eligible) {
                ++changed;
                Require(!k.numIndStages && !k.fog && !k.logicSrc && !k.inWrap, "an eligible key has no indirect stage, fog, logic-op output or wide register");
                Require(src.text.find("uniform hvec4 u_tev") != std::string::npos && src.text.find("varying vec3 v_tex") == src.text.find("varying vec3 v_tex"),
                        "half uniforms");
                if (k.numTexGens) Require(src.text.find("varying hvec3 v_tex") == std::string::npos && src.text.find("hvec2") == std::string::npos,
                                          "texture coordinates stay float");
                Require(src.text.find("gl_FragColor = fin;") != std::string::npos, "the output stays a plain assignment");
            }
            if (k.inWrap == 0) { // inWrap bit 1 alone (a wide register, set by the front end for a key it would have made half) writes the float text of the key without it
                vgx::FsKey wide = k;
                wide.inWrap = 2;
                vgx::ShaderSource w;
                vgx::GenerateFragmentShader(wide, w);
                Require(w.text == flt.text && !vgx::FsHalfEligible(wide), "inWrap bit 1 is the float shader of the same key, without wrap code");
            }
            const vgx::FsHalfReport rep = vgx::FsHalfAnalyse(k);
            if (k.numIndStages || k.fog || k.logicSrc || k.inWrap) Require(!rep.eligibleShape, "indirect stages, fog, logic-op output and wide registers are refused");
            Require(!rep.Eligible(3) || rep.Eligible(1), "the strict set is inside the normal one");
        }
        std::printf("generated text: %zu of %zu random keys of every shape are half precision under this mode\n", changed, total);
        if (mode == 0 || mode == 2) Require(changed == 0, "no key is half precision with the switch off or 2");
        else Require(changed > 20, "some keys are half precision");
    }

    // ---- 2. + 3. the analysis and the numbers
    // (independent of the mode: the analysis and the model are used directly)
    {
        const vgx::FsKey k = [] { vgx::FsKey q; q.numStages = 1; q.stage[0].color = (15u << 12) | (8u << 8) | (10u << 4) | 15u; q.stage[0].alpha = (7u << 13) | (4u << 10) | (5u << 7) | 7u << 4; q.stage[0].texEnable = 1; q.stage[0].rasChan = 0; q.numTexGens = 1; return q; }();
        const vgx::FsHalfReport rep = vgx::FsHalfAnalyse(k);
        Require(rep.eligibleShape && rep.Eligible(1) && rep.colorLevels > 0.2 && rep.colorLevels < 0.5, "the plain texture * vertex colour modulate is taken (bound 0.37 levels)");
        vgx::FsKey ind = k; ind.numIndStages = 1; ind.stage[0].ind = 1;
        vgx::FsKey fog = k; fog.fog = 2;
        vgx::FsKey cmp = k; cmp.stage[0].color |= 3u << 16;
        vgx::FsKey wide = k; wide.inWrap = 2;
        Require(!vgx::FsHalfAnalyse(ind).eligibleShape && !vgx::FsHalfAnalyse(fog).eligibleShape && !vgx::FsHalfAnalyse(cmp).eligibleShape && !vgx::FsHalfAnalyse(wide).eligibleShape,
                "indirect, fog, compare-mode, wide register: refused");
        // four stages  prev = (prev + tex * ras - 0.5) * 4, not clamped: the error grows by 4 per stage
        vgx::FsKey scaled = k;
        scaled.numStages = 4;
        for (int s = 0; s < 4; ++s) { scaled.stage[s] = k.stage[0]; scaled.stage[s].color = (15u << 12) | (8u << 8) | (10u << 4) | 0u | (2u << 16) | (2u << 20); }
        Require(vgx::FsHalfAnalyse(scaled).eligibleShape && !vgx::FsHalfAnalyse(scaled).Eligible(1), "four stages of x4 with a bias and no clamp are refused by the bound");
        scaled.numStages = 1;
        Require(vgx::FsHalfAnalyse(scaled).Eligible(1) == (vgx::FsHalfAnalyse(scaled).colorLevels <= 1.0), "one such stage: decided by its bound");
    }
    Require(R16(1.0f / 3.0f) == 0.333251953125f && R16(0.1f) == 0.0999755859375f && R16(65504.0f) == 65504.0f && R16(1e-8f) == 0.0f &&
            R16(2049.0f) == 2048.0f && R16(2051.0f) == 2052.0f && R16(1023.75f) == 1024.0f && R16(1023.25f) == 1023.0f && R16(-0.5f) == -0.5f &&
            R16(6.0e-8f) == 5.9604644775390625e-08f && R16(70000.0f) > 1e30f, "the half rounding function");
    Totals all, strict;
    if (mode == 1) {
        std::printf("random keys of the shape real materials have (eligible under the bound of GXV_FS_HALF=1):\n");
        Rng gen(2024), inputs(99);
        size_t made = 0, tried = 0;
        Totals t;
        std::map<int, Totals> byBound; // bound in quarter levels
        std::vector<vgx::FsKey> eligibleKeys;
        while (made < 400 && tried < 200000) {
            ++tried;
            const vgx::FsKey k = RandomKey(gen, false);
            const vgx::FsHalfReport rep = vgx::FsHalfAnalyse(k);
            if (!rep.eligibleShape || !rep.Eligible(1)) continue;
            ++made;
            if (eligibleKeys.size() < 40) eligibleKeys.push_back(k);
            const Totals one = RunKey(k, rep, inputs, 400);
            t.Add(one);
            byBound[static_cast<int>(std::min(rep.colorLevels, 0.999) * 4)].Add(one);
            if (rep.Eligible(3)) strict.Add(one);
        }
        std::printf("  (%zu eligible among %zu random keys: %.1f%%)\n", made, tried, 100.0 * made / tried);
        Print("all eligible keys", t);
        Print("strict set (bound <= 0.5, mode 3)", strict);
        for (auto& b : byBound) {
            char name[64];
            std::snprintf(name, sizeof name, "bound %.2f..%.2f levels", b.first / 4.0, (b.first + 1) / 4.0);
            Print(name, b.second);
        }
        all.Add(t);
        Require(t.maxDiff <= 1, "eligible keys: the 8-bit result is at most one level off in every colour channel and in alpha");
        Require(t.worstRatio <= 1.0, "the measured error never exceeds the bound");
        std::printf("  alpha-tested eligible keys: %zu keys, %zu fragments, discard decision differs in %zu\n", t.discardKeys, t.discardSamples, t.discardFlips);
        Require(t.discardFlips * 1000 <= std::max<size_t>(t.discardSamples, 1), "the discard decision of an alpha-tested eligible key differs for at most 0.1% of the fragments");
        if (!eligibleKeys.empty()) {
            // 4. Mesa
            Require(vgx::host::CreateContext(), "Mesa GLES context");
            std::printf("GL renderer: %s\n", glGetString(GL_RENDERER));
            GLuint rt = 0, fbo = 0;
            glGenTextures(1, &rt); glActiveTexture(GL_TEXTURE0 + 7); glBindTexture(GL_TEXTURE_2D, rt);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
            Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "RGBA8 target");
            glViewport(0, 0, 1, 1); glDisable(GL_DITHER); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
            Rng glr(31337);
            const GlTotals g = RunGl(eligibleKeys, glr, 12);
            std::printf("Mesa: %zu eligible keys, %zu fragments: half text == float text in %zu; CPU model == Mesa float in %zu, one level off in %zu, more in %zu; alpha-test discard decision agrees in %zu of %zu\n",
                        g.keys, g.frags, g.textSame, g.modelSame, g.modelOff1, g.modelMore, g.discardAgree, g.discardFrags);
            Require(g.textSame == g.frags, "text equivalence");
            // (one level off happens where the exact result sits on a half level, e.g. n / 2 from a konst 0.5: the test inputs are 8-bit values, so such ties are
        // common, and which way a tie is rounded depends on the last bit of the float arithmetic, which differs between the model and llvmpipe)
        Require(g.modelMore == 0 && g.modelOff1 * 10 <= g.frags, "the CPU model is the Mesa float pipeline (at most one level off, in at most 10% of the fragments)");
            Require(g.discardAgree * 100 >= g.discardFrags * 98, "the CPU model decides the alpha test as Mesa does");
        }
    }

    // ---- the warm-up list
    if (argc > 1 && mode == 1) {
        const std::vector<vgx::FsKey> list = ReadList(argv[1]);
        Require(!list.empty(), "the warm-up list is readable");
        size_t shape = 0, e1 = 0, e3 = 0;
        std::map<std::string, size_t> why;
        Rng inputs(5);
        Totals t;
        size_t exactKeys = 0, near95 = 0;
        double exactBound = 0.0;
        for (const vgx::FsKey& k : list) {
            const vgx::FsHalfReport rep = vgx::FsHalfAnalyse(k);
            if (!rep.eligibleShape) { ++why[rep.why]; continue; }
            ++shape;
            if (rep.Eligible(3)) ++e3;
            if (!rep.Eligible(1)) continue;
            ++e1;
            const Totals one = RunKey(k, rep, inputs, 2000);
            t.Add(one);
            if (one.same * 1000 >= one.samples * 999) { ++exactKeys; exactBound = std::max(exactBound, std::max(rep.colorLevels, rep.alphaLevels)); }
            else if (one.same * 100 >= one.samples * 95) ++near95;
        }
        std::printf("warm-up list %s: %zu fragment shaders\n", argv[1], list.size());
        for (auto& w : why) std::printf("  refused (%s): %zu\n", w.first.c_str(), w.second);
        std::printf("  no refusal reason but over the bound: %zu\n", shape - e1);
        std::printf("  ELIGIBLE under GXV_FS_HALF=1: %zu of %zu (%.1f%%); under 3 (strict): %zu (%.1f%%)\n", e1, list.size(), 100.0 * e1 / list.size(), e3, 100.0 * e3 / list.size());
        Print("the list's eligible keys", t);
        std::printf("  keys with the same 8-bit result in at least 99.9%% of the fragments: %zu of %zu (largest bound among them %.2f levels); 95..99.9%%: %zu\n",
                    exactKeys, e1, exactBound, near95);
        Require(t.maxDiff <= 1 && t.worstRatio <= 1.0, "the list's eligible keys: at most one level off, bound holds");
    }
    std::printf("PASS\n");
    return 0;
}
