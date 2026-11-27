// fs24: GXV_FS_HALF_FOG (fog in the half precision fragment shaders) and GXV_FS_NOCOLOR (no colour pipeline when the colour is masked off).
// Real generated GLES2 shaders on Mesa (a PC has no half type: `hfloat` is `float` there, so the text and the lanes are checked exactly, the half ROUNDING
// by a model that rounds every operation the way vgx_shadergen.cpp's analysis assumes). No game data or Vita needed.
//   * FogHalfLanes: every lane is an exact half value; the float evaluation of the fog factor from them against the double reference over many ramps
//     (near / far / start / end of the game's kind) stays within the analysis's allowance (2 * 2^-11 + 4e-4);
//   * the whole fog mix in modelled half arithmetic against the exact mix, in 8-bit levels: within the bound FsHalfAnalyse gives the key, no reversal
//     and no jump of more than one level along a ramp (no banding that the float shader does not have);
//   * keys: a vertex-fog key without the half bit is refused for half ("fog"), with it accepted; the text; the TEV row count;
//   * pixels on Mesa: a kFogHalfBit draw against the plain vertex-fog draw (<= 1 level), and a colorDead draw against the ordinary draw: alpha identical in
//     every pixel (alpha-tested, graded alpha), colour 0.
#include "vgx_shadergen.h"
#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace vgx { bool LogEnabled() { return false; } }
namespace {
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
void Uniform4(GLuint p, const char* arr, int i, const float v[4]) {
    const std::string n = std::string(arr) + "[" + std::to_string(i) + "]";
    const GLint loc = glGetUniformLocation(p, n.c_str());
    if (loc >= 0) glUniform4fv(loc, 1, v);
}
float H(double x) { return vgx::RoundToHalf(static_cast<float>(x)); } // a half value

// the shader's float evaluation of the factor from the lanes (what the generated text computes)
float FgFromLanes(const vgx::FogHalfLane& l, float v) {
    const float bs = l.bHi + l.bLo; // (float(u_tev[11].z) + float(u_tev[11].w))
    const float f = (v * l.sInv - bs) * l.c1s + l.k;
    return std::min(1.0f, std::max(0.0f, f));
}

vgx::FsKey TevKey() { // stage 0: D = c0 (colour), D = c0 alpha
    vgx::FsKey k;
    k.stage[0].color = 2u | (15u << 4) | (15u << 8) | (15u << 12);
    k.stage[0].alpha = (1u << 4) | (7u << 7) | (7u << 10) | (7u << 13);
    return k;
}

// ---- the lanes against the reference
void CheckLanes() {
    std::mt19937_64 rng(24);
    auto U = [&](double a, double b) { return a + (b - a) * (static_cast<double>(rng() >> 11) / 9007199254740992.0); };
    double worstErr = 0, worstWide = 0, worstWideBs = 0; size_t ramps = 0, wide = 0, evals = 0;
    const double allow = 1.0 / 2048.0 + 4e-4; // (the analysis: the slope lane in half + the float evaluation, kFogFloatErr)
    for (int n = 0; n < 20000; ++n) {
        // a ramp: start, end in the units the game uses (view depth 1 .. 3e5), c1 = 1 / (end - start), c0 = -start * c1 (FogVtxCoeffs' own form)
        const double start = std::exp(U(std::log(1.0), std::log(2e5)));
        const double len = std::exp(U(std::log(0.5), std::log(3e5)));
        const bool ortho = (n & 3) == 0;
        double c1 = 1.0 / len, c0 = -start * c1;
        if (ortho) { c1 = U(0.5, 4.0) / len; c0 = U(-2.0, 1.0); }
        const float c1f = static_cast<float>(c1), c0f = static_cast<float>(c0);
        const vgx::FogHalfLane l = vgx::FogHalfLanes(c1f, c0f);
        for (float x : {l.sInv, l.c1s, l.bHi, l.bLo, l.k}) Require(H(x) == x, "every lane is an exact half value");
        Require(l.sInv > 0 && (std::ldexp(1.0f, std::ilogb(l.sInv)) == l.sInv), "sInv is a power of two");
        Require(l.sInv >= std::ldexp(1.0f, -14) || l.sInv == 1.0f, "sInv is a normal half (fix24: never a subnormal the console may flush)");
        if (l.c1s != 0.0f) Require(std::fabs(l.c1s) >= std::ldexp(1.0f, -14), "c1s is a normal half");
        ++ramps; wide += l.wide;
        const double w0 = -c0f / static_cast<double>(c1f); // where the factor is 0
        for (int i = 0; i < 64; ++i) {
            // v across the ramp and a little beyond (the part outside is clamped)
            const double v = w0 + (i / 63.0 * 1.4 - 0.2) / static_cast<double>(c1f);
            if (v < 0) continue;
            const double ref = std::min(1.0, std::max(0.0, static_cast<double>(c1f) * v + static_cast<double>(c0f)));
            const float got = FgFromLanes(l, static_cast<float>(v));
            if (!l.wide) { worstErr = std::max(worstErr, std::fabs(got - ref)); Require(std::fabs(got - ref) <= allow, "lane evaluation within the analysis's allowance"); }
            else if (std::fabs(static_cast<double>(l.bHi) + l.bLo) <= 16384.0) { worstWide = std::max(worstWide, std::fabs(got - ref)); worstWideBs = std::max(worstWideBs, std::fabs(static_cast<double>(l.bHi) + l.bLo)); }
            ++evals;
        }
    }
    std::printf("lanes (wide ramps, |bs| up to %.0f: worst error %.3g = %.2f levels)\n", worstWideBs, worstWide, worstWide * 255);
    std::printf("lanes: %zu ramps (%zu with |bs| > 4096 left out of the bound), %zu evaluations; worst |fg - reference| %.3g (allowance %.3g = %.3f levels)\n", ramps, wide, evals, worstErr, allow, allow * 255);
}

// ---- the fog mix (float, from half inputs) against the exact one, in 8-bit levels
void CheckMix() {
    std::mt19937_64 rng(2424);
    auto U = [&]() { return static_cast<double>(rng() >> 11) / 9007199254740992.0; };
    vgx::FsKey k = TevKey();
    k.fog = static_cast<uint8_t>(2 | vgx::kFogVtxBit | vgx::kFogHalfBit);
    const vgx::FsHalfReport rep = vgx::FsHalfAnalyse(k);
    vgx::FsKey kn = TevKey();
    const vgx::FsHalfReport repNoFog = vgx::FsHalfAnalyse(kn);
    Require(rep.eligibleShape, "the fog key with the half bit is analysable");
    std::printf("analysis: TEV without fog %.3f levels, with fog %.3f levels (alpha %.3f)\n", repNoFog.colorLevels, rep.colorLevels, rep.alphaLevels);
    double worst = 0; size_t samples = 0;
    for (int n = 0; n < 4000; ++n) {
        const double start = std::exp(std::log(1.0) + (std::log(2e5) - std::log(1.0)) * U());
        const double len = std::exp(std::log(10.0) + (std::log(3e5) - std::log(10.0)) * U());
        const double c1 = 1.0 / len, c0 = -start * c1;
        const vgx::FogHalfLane l = vgx::FogHalfLanes(static_cast<float>(c1), static_cast<float>(c0));
        if (l.wide) continue;
        const double w0 = start;
        for (int i = 0; i < 128; ++i) {
            const double v = w0 + (i / 127.0) * len;
            const double a = H(U()), b = H(U());                                   // fin.rgb (a TEV result, half) and the fog colour (half)
            const double fgExact = std::min(1.0, std::max(0.0, c1 * v + c0));
            const double ref = a + fgExact * (b - a);
            const float fg = FgFromLanes(l, static_cast<float>(v));                // float (the shader mixes in float: the factor and the colours are converted, not rounded again)
            const float m1 = static_cast<float>(a) + fg * (static_cast<float>(b) - static_cast<float>(a));      // mix() in float, both usual forms
            const float m2 = static_cast<float>(a) * (1.0f - fg) + static_cast<float>(b) * fg;
            const double e = std::max(std::fabs(m1 - ref), std::fabs(m2 - ref)) * 255;
            worst = std::max(worst, e); ++samples;
        }
    }
    std::printf("float mix of half inputs vs the exact mix: %zu samples, worst %.3f levels (the analysis's bound for this key: %.3f levels)\n", samples, worst, rep.colorLevels);
    Require(worst <= rep.colorLevels, "the modelled half error is inside the analysis's bound");
    // banding: along a ramp the quantised output is monotone and moves by at most one level between two close points
    size_t reversals = 0, jumps = 0, steps = 0;
    for (int n = 0; n < 300; ++n) {
        const double start = std::exp(std::log(1.0) + (std::log(2e5) - std::log(1.0)) * U());
        const double len = std::exp(std::log(10.0) + (std::log(3e5) - std::log(10.0)) * U());
        const double c1 = 1.0 / len, c0 = -start * c1;
        const vgx::FogHalfLane l = vgx::FogHalfLanes(static_cast<float>(c1), static_cast<float>(c0));
        const double a = H(U()), b = H(U());
        int prev = -1;
        const int N = 4096; // a pixel step is len / 960 at most: 4096 points over the ramp are finer than every pixel
        for (int i = 0; i <= N; ++i) {
            const double v = start + len * i / N;
            const float fg = FgFromLanes(l, static_cast<float>(v));
            const float m = static_cast<float>(a) + fg * (static_cast<float>(b) - static_cast<float>(a));
            const int q = static_cast<int>(std::floor(m * 255 + 0.5));
            if (prev >= 0) {
                const int d = q - prev;
                const int dir = b > a ? 1 : -1;
                if (d * dir < 0) ++reversals;
                if (std::abs(d) > 1) ++jumps;
                ++steps;
            }
            prev = q;
        }
    }
    std::printf("banding: %zu steps along 300 ramps: %zu reversals, %zu jumps of more than one level\n", steps, reversals, jumps);
    Require(reversals == 0 && jumps == 0, "no reversal and no jump of more than one level along a ramp");
}

// ---- keys and text
void CheckKeys() {
    vgx::FsKey plain = TevKey();
    plain.fog = static_cast<uint8_t>(2 | vgx::kFogVtxBit);
    vgx::FsKey half = plain;
    half.fog = static_cast<uint8_t>(plain.fog | vgx::kFogHalfBit);
    const vgx::FsHalfReport rp = vgx::FsHalfAnalyse(plain), rh = vgx::FsHalfAnalyse(half);
    Require(!rp.eligibleShape && rp.why && std::strcmp(rp.why, "fog") == 0, "a vertex-fog key without the half bit is refused for half (as it always was)");
    Require(rh.eligibleShape && rh.Eligible(1), "with the half bit the same key is taken");
    Require(vgx::FsTevCount(plain) == 11 && vgx::FsTevCount(half) == 12, "the half-fog key has row 11");
    vgx::FsKey perPixel = TevKey();
    perPixel.fog = 2; // per-pixel fog (no vertex bit)
    Require(!vgx::FsHalfAnalyse(perPixel).eligibleShape, "per-pixel fog is still refused");
    std::printf("keys: plain fog refused (%s); half-bit key colour bound %.3f levels; the same TEV without fog %.3f\n", rp.why, rh.colorLevels, vgx::FsHalfAnalyse(TevKey()).colorLevels);
    vgx::ShaderSource sf, sh, sd;
    vgx::GenerateFragmentShader(plain, sf);
    vgx::GenerateFragmentShader(half, sh);
    Require(sf.text.find("float fg = clamp(u_tev[9].x") != std::string::npos, "the plain vertex-fog text is the old one");
    Require(sh.text.find("u_tev[11].x") != std::string::npos && sh.text.find("float(u_tev[11].z) + float(u_tev[11].w)") != std::string::npos, "the half-bit text reads the lanes");
    vgx::FsKey dead = half;
    dead.setColorDead(1);
    vgx::GenerateFragmentShader(dead, sd);
    Require(sd.text.find("fogc") == std::string::npos && sd.text.find(" fg ") == std::string::npos && sd.text.find("u_tev[10]") == std::string::npos && (sd.text.find("fin.rgb = vec3(0.0)") != std::string::npos || sd.text.find("fin.rgb = hvec3(0.0)") != std::string::npos), "colorDead: no fog, colour 0");
    std::printf("text: half-bit fog line: %s\n", sh.text.substr(sh.text.find("fg = "), 200).c_str());
}

// ---- pixels
struct Pix { std::vector<uint8_t> rgba; };
// the vertex shader of the test: the fog varying is the one the fragment text declares (v_fog, or v_fogtN with GXV_FOG_BIND)
std::string VtxFor(const std::string& fsText) {
    std::string name = "v_fog_unused";
    const size_t at = fsText.find("varying float v_fog");
    if (at != std::string::npos) name = fsText.substr(at + 14, fsText.find(';', at) - at - 14);
    return "attribute vec4 a_pos; attribute vec4 a_col0; attribute vec4 a_col1; uniform mat4 u_proj; uniform vec4 u_mv[3]; varying vec4 v_col0; varying vec4 v_col1; varying float " + name + ";\n" +
           "void main(){ vec4 p = vec4(a_pos.xyz, 1.0); vec3 eye = vec3(dot(u_mv[0], p), dot(u_mv[1], p), dot(u_mv[2], p)); gl_Position = u_proj * vec4(eye, 1.0);\n" +
           " v_col0 = a_col0; v_col1 = a_col1; " + name + " = gl_Position.w; }\n";
}

Pix DrawQuad(const vgx::FsKey& fk, const float fogLanes[8], const float c1c0[2], const float col[4], bool maskColor) {
    vgx::ShaderSource fs;
    vgx::GenerateFragmentShader(fk, fs);
    const GLuint p = Link(VtxFor(fs.text), fs.text);
    glUseProgram(p);
    const double n = 1.0, f = 1000.0;
    const double p4 = -n / (f - n), p5 = -f * n / (f - n);
    float m[16] = {};
    m[0] = 1; m[5] = -1; m[10] = static_cast<float>(2 * p4 - 1); m[14] = static_cast<float>(2 * p5); m[11] = -1;
    glUniformMatrix4fv(glGetUniformLocation(p, "u_proj"), 1, GL_FALSE, m);
    const float mv[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    for (int i = 0; i < 3; ++i) Uniform4(p, "u_mv", i, mv[i]);
    const float c0[4] = {0.9f, 0.3f, 0.1f, 1.0f};
    Uniform4(p, "u_tev", 1, c0);
    const float ref[4] = {100.0f, 100.0f, 0.0f, 0.0f}; // alpha references (alpha test GEQUAL 100)
    Uniform4(p, "u_tev", 8, ref);
    if (fk.fog) {
        const float row9[4] = {c1c0[0], 0, 0, c1c0[1]};
        Uniform4(p, "u_tev", 9, row9);
        const float fogcol[4] = {0.1f, 0.5f, 0.9f, fogLanes[4]};
        Uniform4(p, "u_tev", 10, fogcol);
        if (fk.fog & vgx::kFogHalfBit) Uniform4(p, "u_tev", 11, fogLanes);
    }
    // a quad at depth 20 .. 300 (near edge to far edge), vertex colour alpha 0 .. 1 across it (the alpha the test cuts)
    float v[4][12] = {};
    const double nx[4] = {-1, 1, -1, 1}, ny[4] = {-1, -1, 1, 1}, depth[4] = {20, 20, 300, 300};
    for (int i = 0; i < 4; ++i) {
        const double ze = -depth[i], scale = -ze;
        v[i][0] = static_cast<float>(nx[i] * scale); v[i][1] = static_cast<float>(ny[i] * scale); v[i][2] = static_cast<float>(ze);
        v[i][4] = col[0]; v[i][5] = col[1]; v[i][6] = col[2]; v[i][7] = (nx[i] > 0 ? 1.0f : 0.15f) * col[3];
        v[i][8] = v[i][9] = v[i][10] = v[i][11] = 1;
    }
    const int idx[6] = {0, 1, 2, 2, 1, 3};
    float tri[6][12];
    for (int i = 0; i < 6; ++i) std::memcpy(tri[i], v[idx[i]], sizeof tri[i]);
    GLuint vbo = 0; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STREAM_DRAW);
    for (int a = 0; a < 3; ++a) { glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE, 12 * sizeof(float), reinterpret_cast<void*>(a * 4 * sizeof(float))); glEnableVertexAttribArray(a); }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.2f, 0.2f, 0.2f, 0.0f); glClear(GL_COLOR_BUFFER_BIT);
    (void)maskColor;
    glDrawArrays(GL_TRIANGLES, 0, 6);
    Pix px; px.rgba.resize(64 * 64 * 4);
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px.rgba.data());
    Require(glGetError() == GL_NO_ERROR, "draw GL status");
    glDeleteBuffers(1, &vbo); glDeleteProgram(p);
    return px;
}

void CheckPixels() {
    // ---- fog: the half-bit key against the plain vertex-fog key, same ramp. Perspective ramp 40 .. 260 across the quad's 20 .. 300.
    const double start = 40, end = 260;
    const float c1 = static_cast<float>(1.0 / (end - start)), c0 = static_cast<float>(-start / (end - start));
    const vgx::FogHalfLane l = vgx::FogHalfLanes(c1, c0);
    const float lanes[8] = {l.sInv, l.c1s, l.bHi, l.bLo, l.k, 0, 0, 0};
    const float cc[2] = {c1, c0};
    const float col[4] = {0.9f, 0.4f, 0.1f, 1.0f};
    vgx::FsKey plain = TevKey();
    plain.stage[0].color = 10u | (15u << 4) | (15u << 8) | (15u << 12); // D = rasterised colour
    plain.stage[0].alpha = (5u << 4) | (7u << 7) | (7u << 10) | (7u << 13);
    plain.stage[0].rasChan = 0;
    plain.fog = static_cast<uint8_t>(2 | vgx::kFogVtxBit);
    vgx::FsKey half = plain;
    half.fog = static_cast<uint8_t>(plain.fog | vgx::kFogHalfBit);
    const Pix a = DrawQuad(plain, lanes, cc, col, false), b = DrawQuad(half, lanes, cc, col, false);
    int maxd = 0; size_t differ = 0, graded = 0;
    for (size_t i = 0; i < a.rgba.size(); i += 4) {
        bool d = false;
        for (int c = 0; c < 4; ++c) { const int dl = std::abs(int(a.rgba[i + c]) - int(b.rgba[i + c])); maxd = std::max(maxd, dl); d |= dl != 0; }
        differ += d;
        graded += a.rgba[i] != 230 && a.rgba[i] != 26 && a.rgba[i + 3] != 0;
    }
    std::printf("pixels: half-bit fog vs plain vertex fog: %zu ramp pixels, %zu differ, max delta %d/255\n", graded, differ, maxd);
    Require(graded > 200, "a real ramp is drawn");
    Require(maxd <= 1, "the lanes give the plain vertex fog's picture within one level");
    // ---- colorDead: the alpha of every pixel is the ordinary draw's, with an alpha test that cuts part of the quad; the colour is 0
    for (int fogOn = 0; fogOn < 2; ++fogOn) {
        vgx::FsKey base = plain;
        base.alphaTestOn = 1; base.alphaTest = 6u | (7u << 3) | (0u << 6); // GEQUAL ref0 AND ALWAYS
        if (!fogOn) base.fog = 0;
        else base.fog = static_cast<uint8_t>(base.fog | vgx::kFogHalfBit);
        vgx::FsKey dead = base;
        dead.setColorDead(1);
        const Pix n = DrawQuad(base, lanes, cc, col, false), d = DrawQuad(dead, lanes, cc, col, true);
        size_t cut = 0, kept = 0, alphaDiff = 0, colourNonZero = 0, keptSame = 0;
        for (size_t i = 0; i < n.rgba.size(); i += 4) {
            const bool drawn = !(n.rgba[i] == 51 && n.rgba[i + 1] == 51 && n.rgba[i + 2] == 51 && n.rgba[i + 3] == 0);
            const bool drawnD = !(d.rgba[i] == 51 && d.rgba[i + 1] == 51 && d.rgba[i + 2] == 51 && d.rgba[i + 3] == 0);
            if (drawn != drawnD) { ++alphaDiff; continue; } // the discard decision is the same
            if (!drawn) { ++cut; continue; }
            ++kept;
            if (n.rgba[i + 3] != d.rgba[i + 3]) ++alphaDiff;
            else ++keptSame;
            if (d.rgba[i] || d.rgba[i + 1] || d.rgba[i + 2]) ++colourNonZero;
        }
        std::printf("pixels: colorDead%s: %zu kept (alpha identical in %zu), %zu cut by the alpha test (same in both), %zu alpha / discard differences, %zu non-zero colours\n", fogOn ? " with fog" : "", kept, keptSame, cut,
                    alphaDiff, colourNonZero);
        Require(kept > 500 && cut > 200, "the alpha test cuts part of the quad");
        Require(alphaDiff == 0, "colorDead: every pixel's alpha and every discard decision is the ordinary draw's");
        Require(colourNonZero == 0, "colorDead writes colour 0");
    }
}
}

int main() {
    std::printf("GXV_FS_HALF=%ld GXV_FS_HALF_FOG=%d GXV_FS_NOCOLOR=%d\n", vgx::FsHalfMode(), vgx::FsHalfFogMode() ? 1 : 0, vgx::FsNoColorMode() ? 1 : 0);
    // RoundToHalf itself: against the definition on a sweep (nearest of the half grid; ties to even)
    for (double x : {0.0, 1.0, 0.5, 1.0 / 3.0, 65504.0, 6.0e-8, 6.2e-5, 0.333251953125, 1023.5, 1024.5, 2047.0, 2049.0, 100000.0}) {
        const float r = vgx::RoundToHalf(static_cast<float>(x)), r2 = vgx::RoundToHalf(r);
        Require(r == r2, "RoundToHalf is idempotent");
        Require(std::fabs(r - x) <= std::fabs(x) / 2048.0 + 3e-8 || std::fabs(x) > 65504.0, "RoundToHalf is within a half unit");
    }
    Require(vgx::RoundToHalf(2049.0f) == 2048.0f && vgx::RoundToHalf(2051.0f) == 2052.0f && vgx::RoundToHalf(1.0f + 1.0f / 2048.0f) == 1.0f, "ties go to even");
    CheckLanes();
    CheckMix();
    CheckKeys();
    Require(vgx::host::CreateContext(), "Mesa GLES context");
    std::printf("GL renderer: %s\n", glGetString(GL_RENDERER));
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "RGBA8 target");
    glViewport(0, 0, 64, 64); glDisable(GL_DITHER); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    CheckPixels();
    std::printf("PASS\n");
    return 0;
}
