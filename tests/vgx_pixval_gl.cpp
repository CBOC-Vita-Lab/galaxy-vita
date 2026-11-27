// Host test of pix23's FsValueEval (platform/vgx/src/vgx_shadergen.cpp): what a TEV fragment shader can output, worked out from the values
// it reads, is what the generated shader does on Mesa. No game data, no Vita executable.
//   tools/test_vgx_pixval_gl.sh [output-dir] [keys]
//
// Random keys of every shape (vgx_fshalf_keys.h, most with an alpha test) are generated as the game's switches make them; random inputs
// are drawn: TEV registers and konst colours (many exactly 0 or 255, some outside 0..255), alpha references, the destination-alpha
// constant, fog colour, eight 8x8 textures whose channels each have a chosen range (all 0, all 255, a constant, a band, anything) and
// random triangles whose vertex colours are 0 at every vertex for some components. FsValueEval gets the same values (the textures' real
// channel minima / maxima, the vertex colours' ranges). Each claim is then checked on the pixels the key's own text draws:
//   - "colour / alpha exactly 0": the text with gl_FragColor = vec4(notEqual(fin, vec4(0.0))) gives 0 in that channel at every pixel
//     drawn (exactly 0.0, not merely below half a level);
//   - "the alpha test passes for every fragment": the key's atPass text (no test) draws exactly the pixels the tested text draws, and
//     the same values there;
//   - "the alpha test fails for every fragment": the tested text draws no pixel.
// Pixels drawn are found by drawing twice over two different clear colours (a pixel that keeps the clear colour was not drawn).
// Also: an atPass text is the key's text less the alpha test's lines (vgx_draw.cpp's check), and with the switches off no text changes.
#include "vgx_shadergen.h"
#include "vgx_fshalf_keys.h"
#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vgx { bool LogEnabled() { return false; } namespace host { bool CreateContext(); } }
namespace {
using fshalf_keys::Rng;
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
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
    if (!ok) { char log[4096] = {}; glGetProgramInfoLog(p, sizeof log, nullptr, log); std::fprintf(stderr, "%s\n", log); }
    Require(ok, "program links");
    glDeleteShader(v); glDeleteShader(f);
    return p;
}
constexpr int kW = 32, kH = 32;
const char* kFsVtx =
    "attribute vec4 a_pos; attribute vec4 a_col0; attribute vec4 a_col1; attribute vec3 a_tex;\n"
    "varying vec4 v_col0; varying vec4 v_col1; varying float v_fog;\n"
    "varying vec3 v_tex0; varying vec3 v_tex1; varying vec3 v_tex2; varying vec3 v_tex3; varying vec3 v_tex4; varying vec3 v_tex5; varying vec3 v_tex6; varying vec3 v_tex7;\n"
    "void main() { gl_Position = a_pos; v_col0 = a_col0; v_col1 = a_col1; v_fog = a_tex.z;\n"
    "  v_tex0 = a_tex; v_tex1 = a_tex.yxz; v_tex2 = a_tex * 0.5; v_tex3 = a_tex.xzy; v_tex4 = a_tex; v_tex5 = a_tex; v_tex6 = a_tex; v_tex7 = a_tex; }\n";
struct FVtx { float pos[4], c0[4], c1[4], t[3]; };

// One draw's inputs, the same for the GL draw and for FsValueEval.
struct Inputs {
    std::vector<float> tev;       // u_tev (28 vec4)
    uint8_t texel[8][8 * 8 * 4];  // eight 8x8 RGBA8 textures
    std::vector<FVtx> v;          // 8 triangles
    vgx::FsValueIn in;
};
int RegByte(Rng& r) {
    const unsigned w = r.U(100);
    if (w < 25) return 0;
    if (w < 40) return 255;
    if (w < 45) return 256 + static_cast<int>(r.U(768));   // (outside 0..255: no FIX_TEVIN wrap in these keys, the value as it is)
    if (w < 48) return -1 - static_cast<int>(r.U(1023));
    return static_cast<int>(r.U(256));
}
void MakeInputs(Rng& r, const vgx::FsKey& k, Inputs& x) {
    vgx::FsValueIn& in = x.in;
    in = vgx::FsValueIn{};
    x.tev.assign(28 * 4, 0.0f);
    for (int reg = 0; reg < 4; ++reg)
        for (int c = 0; c < 4; ++c) {
            in.reg[reg][c] = RegByte(r);
            x.tev[reg * 4 + c] = static_cast<float>(in.reg[reg][c]) / 255.0f;
            const int kb = r.P(25) ? 0 : r.P(20) ? 255 : static_cast<int>(r.U(256));
            in.konst[reg][c] = kb;
            x.tev[16 + reg * 4 + c] = static_cast<float>(kb) / 255.0f;
        }
    in.ref0 = static_cast<int>(r.P(20) ? 0 : r.P(10) ? 255 : r.U(256));
    in.ref1 = static_cast<int>(r.P(20) ? 0 : r.P(10) ? 255 : r.U(256));
    in.dstAlpha = r.P(40) ? 0 : static_cast<int>(r.U(256));
    x.tev[32] = static_cast<float>(in.ref0);
    x.tev[33] = static_cast<float>(in.ref1);
    x.tev[34] = static_cast<float>(in.dstAlpha) / 255.0f;
    x.tev[35] = 0.5f;
    // fog: c1, -, -, c0 (vertex fog) or a, b, c (per pixel); the colour as bytes
    x.tev[36] = static_cast<float>(r.F()) * 2.0f - 1.0f; x.tev[37] = static_cast<float>(r.F()) * 2.0f; x.tev[38] = static_cast<float>(r.F());
    x.tev[39] = static_cast<float>(r.F());
    for (int c = 0; c < 3; ++c) {
        in.fogColor[c] = static_cast<uint8_t>(r.P(50) ? 0 : r.U(256));
        x.tev[40 + c] = in.fogColor[c] / 255.0f;
    }
    for (int i = 44; i < 68; ++i) x.tev[i] = static_cast<float>(r.F()) * 2.0f - 1.0f; // (indirect matrices)
    for (int i = 68; i < 84; ++i) x.tev[i] = 1.0f;
    for (int i = 84; i < 100; ++i) x.tev[i] = 8.0f;
    // the textures: per channel a range
    for (int t = 0; t < 8; ++t) {
        int lo[4], hi[4];
        for (int c = 0; c < 4; ++c) {
            const unsigned w = r.U(100);
            if (w < 15) lo[c] = hi[c] = 0;
            else if (w < 30) lo[c] = hi[c] = 255;
            else if (w < 40) lo[c] = hi[c] = static_cast<int>(r.U(256));
            else if (w < 80) { lo[c] = static_cast<int>(r.U(256)); hi[c] = std::min(255, lo[c] + static_cast<int>(r.U(64))); }
            else { lo[c] = 0; hi[c] = 255; }
        }
        uint8_t mn[4] = {255, 255, 255, 255}, mx[4] = {0, 0, 0, 0};
        for (int i = 0; i < 64; ++i)
            for (int c = 0; c < 4; ++c) {
                const uint8_t b = static_cast<uint8_t>(lo[c] + static_cast<int>(r.U(static_cast<uint32_t>(hi[c] - lo[c] + 1))));
                x.texel[t][i * 4 + c] = b;
                mn[c] = std::min(mn[c], b);
                mx[c] = std::max(mx[c], b);
            }
        in.texKnown[t] = !r.P(10);
        for (int c = 0; c < 4; ++c) { in.texLo[t][c] = mn[c]; in.texHi[t][c] = mx[c]; }
    }
    // the vertex colours: per component 0 at every vertex, a constant, or a band
    bool zero[2][4];
    float clo[2][4], chi[2][4];
    for (int ch = 0; ch < 2; ++ch)
        for (int c = 0; c < 4; ++c) {
            zero[ch][c] = r.P(30);
            clo[ch][c] = static_cast<float>(r.F());
            chi[ch][c] = std::min(1.0f, clo[ch][c] + static_cast<float>(r.F()) * 0.5f);
            if (r.P(20)) chi[ch][c] = clo[ch][c];
        }
    x.v.assign(8 * 3, FVtx{});
    for (FVtx& p : x.v) {
        p.pos[0] = static_cast<float>(r.F()) * 2.4f - 1.2f; p.pos[1] = static_cast<float>(r.F()) * 2.4f - 1.2f; p.pos[2] = 0.0f; p.pos[3] = 1.0f;
        for (int c = 0; c < 4; ++c) {
            p.c0[c] = zero[0][c] ? 0.0f : clo[0][c] + (chi[0][c] - clo[0][c]) * static_cast<float>(r.F());
            p.c1[c] = zero[1][c] ? 0.0f : clo[1][c] + (chi[1][c] - clo[1][c]) * static_cast<float>(r.F());
        }
        p.t[0] = static_cast<float>(r.F()) * 4.0f - 2.0f; p.t[1] = static_cast<float>(r.F()) * 4.0f - 2.0f; p.t[2] = 0.5f + static_cast<float>(r.F());
    }
    // (FsKey::regFromVtx: c0 / c1 come from the vertex colours, which then carry registers 1 / 2 at every vertex, as GXV_FOLDREGS writes them)
    for (int ch = 0; ch < 2; ++ch)
        if (k.regFromVtx & (1u << ch)) {
            for (int c = 0; c < 4; ++c) {
                const int b = std::clamp(in.reg[1 + ch][c], 0, 255);
                in.reg[1 + ch][c] = b;
                for (FVtx& p : x.v) (ch ? p.c1 : p.c0)[c] = static_cast<float>(b) / 255.0f;
            }
        }
    for (int ch = 0; ch < 2; ++ch)
        for (int c = 0; c < 4; ++c) {
            float mn = 1.0f, mx = 0.0f;
            for (const FVtx& p : x.v) { const float f = ch ? p.c1[c] : p.c0[c]; mn = std::min(mn, f); mx = std::max(mx, f); }
            in.rasLo[ch][c] = mn;
            in.rasHi[ch][c] = mx;
            in.rasZero[ch][c] = mx == 0.0f;
        }
}
GLuint g_tex[8];
// Draws program p with x over a clear colour; returns the RGBA8 pixels.
void Draw(GLuint p, const Inputs& x, const float clear[4], std::vector<uint8_t>& px) {
    glUseProgram(p);
    if (GLint l = glGetUniformLocation(p, "u_tev"); l >= 0) glUniform4fv(l, 28, x.tev.data());
    for (int t = 0; t < 8; ++t) {
        glActiveTexture(GL_TEXTURE0 + t);
        glBindTexture(GL_TEXTURE_2D, g_tex[t]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, x.texel[t]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        char name[16];
        std::snprintf(name, sizeof name, "u_tex%d", t);
        if (GLint l = glGetUniformLocation(p, name); l >= 0) glUniform1i(l, t);
    }
    auto Attr = [&](const char* name, int n, const float* first) {
        const GLint loc = glGetAttribLocation(p, name);
        if (loc < 0) return;
        glEnableVertexAttribArray(static_cast<GLuint>(loc));
        glVertexAttribPointer(static_cast<GLuint>(loc), n, GL_FLOAT, GL_FALSE, sizeof(FVtx), first);
    };
    for (GLuint i = 0; i < 16; ++i) glDisableVertexAttribArray(i);
    Attr("a_pos", 4, x.v[0].pos); Attr("a_col0", 4, x.v[0].c0); Attr("a_col1", 4, x.v[0].c1); Attr("a_tex", 3, x.v[0].t);
    glDisable(GL_BLEND);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(x.v.size()));
    px.assign(kW * kH * 4, 0);
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    Require(glGetError() == GL_NO_ERROR, "GL status");
}
const float kClearA[4] = {17.0f / 255, 33.0f / 255, 49.0f / 255, 65.0f / 255};
const float kClearB[4] = {200.0f / 255, 150.0f / 255, 100.0f / 255, 50.0f / 255};
// The pixels a program draws (two clears) and their values.
void Drawn(GLuint p, const Inputs& x, std::vector<uint8_t>& mask, std::vector<uint8_t>& px) {
    std::vector<uint8_t> b;
    Draw(p, x, kClearA, px);
    Draw(p, x, kClearB, b);
    mask.assign(kW * kH, 0);
    for (int i = 0; i < kW * kH; ++i) mask[i] = std::memcmp(&px[i * 4], &b[i * 4], 4) == 0 ? 1 : 0;
}
std::string ZeroProbe(const std::string& t) {
    const std::string tail = "  gl_FragColor = fin;\n}\n";
    const size_t at = t.rfind(tail);
    if (at == std::string::npos) return std::string();
    return t.substr(0, at) + "  gl_FragColor = vec4(notEqual(vec4(fin), vec4(0.0)));\n}\n";
}
// gpu960: the same text, writing the clamped final alpha as exactly-1 (255 / 0 in all channels) or itself (8 bits).
std::string AlphaProbe(const std::string& t, bool exactOne) {
    const std::string tail = "  gl_FragColor = fin;\n}\n";
    const size_t at = t.rfind(tail);
    if (at == std::string::npos) return std::string();
    return t.substr(0, at) + (exactOne ? "  gl_FragColor = vec4(clamp(float(fin.a), 0.0, 1.0) == 1.0 ? 1.0 : 0.0);\n}\n"
                                       : "  gl_FragColor = vec4(clamp(float(fin.a), 0.0, 1.0));\n}\n");
}
// The key's text less the alpha test's lines (as vgx_draw.cpp PixStripTest).
std::string StripTest(const std::string& t) {
    std::string r;
    size_t i = 0;
    while (i < t.size()) {
        size_t j = t.find('\n', i);
        if (j == std::string::npos) j = t.size() - 1;
        const std::string line = t.substr(i, j + 1 - i);
        const bool test = line.rfind("  float ax = ", 0) == 0 || line.rfind("  float ai = ", 0) == 0 ||
                          (line.rfind("  if (!(", 0) == 0 && line.find("discard;") != std::string::npos);
        if (!test) r += line;
        i = j + 1;
    }
    return r;
}
} // namespace

int main(int argc, char** argv) {
    const unsigned nKeys = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 2000;
    Require(vgx::host::CreateContext(), "Mesa GLES context");
    GLuint fbo, rt;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &rt); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, rt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "framebuffer");
    glGenTextures(8, g_tex);
    glViewport(0, 0, kW, kH);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_DITHER);

    Rng r(0x91C23);
    size_t keys = 0, analysed = 0, tested = 0, cZero = 0, aZero = 0, pass = 0, fail = 0, either = 0, pxChecked = 0, stripSame = 0;
    size_t cZeroPx = 0, aZeroPx = 0, passPx = 0, aOne = 0, aLo = 0, aOnePx = 0, aLoPx = 0;
    for (unsigned n = 0; n < nKeys; ++n) {
        vgx::FsKey k = fshalf_keys::RandomKey(r, r.P(50));
        k.inWrap &= static_cast<uint8_t>(~1u); // (wrapped register reads: FsValueEval does not take them)
        if (r.P(60)) {
            k.alphaTestOn = 1;
            k.alphaTest = static_cast<uint8_t>(r.U(8) | (r.U(8) << 3) | (r.U(4) << 6));
        }
        // (zeros in the arithmetic: some stages' D and B inputs set to ZERO, some registers' results kept)
        for (int s = 0; s < k.numStages; ++s) {
            if (r.P(30)) k.stage[s].color = (k.stage[s].color & ~0xFu) | 15u;
            if (r.P(30)) k.stage[s].alpha = (k.stage[s].alpha & ~0x70u) | (7u << 4);
        }
        ++keys;
        Inputs x;
        MakeInputs(r, k, x);
        vgx::FsValueOut o;
        vgx::FsValueEval(k, x.in, o);
        if (!o.ok) continue;
        ++analysed;
        vgx::ShaderSource base;
        vgx::GenerateFragmentShader(k, base);
        const bool hasTest = vgx::FsHasAlphaTestText(k);
        // (the atPass text: the key's text less the test lines, always)
        vgx::FsKey ka = k;
        ka.setAtPass(1);
        vgx::ShaderSource at;
        vgx::GenerateFragmentShader(ka, at);
        Require(StripTest(base.text) == at.text, "an atPass text is the key's text less the alpha test's lines");
        if (hasTest) ++stripSame;
        else Require(base.text == at.text, "a key without a test text: atPass changes nothing");
        const bool claimZero = o.colorZero || o.alphaZero;
        const bool claimTest = hasTest && o.test != -1;
        if (hasTest) {
            ++tested;
            if (o.test == 1) ++pass;
            else if (o.test == 0) ++fail;
            else ++either;
        }
        if (o.colorZero) ++cZero;
        if (o.alphaZero) ++aZero;
        // (a register outside 0..255 in a key that is not marked: the front end always marks such a state (inWrap), so that no key meets it unmarked;
        // under GXV_FS_TRIM the text of an unmarked key leaves clamps out on the registers-in-range assumption: the new claims are made only where it holds)
        bool regsIn = true;
        for (int rr = 0; rr < 4; ++rr)
            for (int cc = 0; cc < 4; ++cc) regsIn = regsIn && x.in.reg[rr][cc] >= 0 && x.in.reg[rr][cc] <= 255;
        const bool claimOne = o.alphaOne && (regsIn || k.inWrap != 0), claimLo = o.alphaLoOut > 0.02f && (regsIn || k.inWrap != 0);
        if (claimOne) ++aOne;
        if (claimLo) ++aLo;
        if (!claimZero && !claimTest && !claimOne && !claimLo) continue;
        const GLuint p = Link(kFsVtx, base.text);
        std::vector<uint8_t> mask, px;
        Drawn(p, x, mask, px);
        if (claimTest && o.test == 0)
            for (int i = 0; i < kW * kH; ++i)
                if (mask[i]) { std::fprintf(stderr, "key %u: pixel %d drawn\n%s\n", n, i, base.text.c_str()); Require(false, "test fails everywhere: nothing drawn"); }
        if (claimTest && o.test == 1) {
            const GLuint pa = Link(kFsVtx, at.text);
            std::vector<uint8_t> maskA, pxA;
            Drawn(pa, x, maskA, pxA);
            for (int i = 0; i < kW * kH; ++i) {
                if (mask[i] != maskA[i] || (mask[i] && std::memcmp(&px[i * 4], &pxA[i * 4], 4) != 0)) {
                    std::fprintf(stderr, "key %u: pixel %d killed (alpha %.4f..%.4f, refs %d %d)\n%s\n", n, i, o.alphaLo, o.alphaHi, x.in.ref0, x.in.ref1, base.text.c_str());
                    Require(false, "test passes everywhere: the text without the test draws the same pixels");
                }
                passPx += maskA[i];
            }
            glDeleteProgram(pa);
        }
        if (claimZero) {
            const std::string zt = ZeroProbe(base.text);
            Require(!zt.empty(), "the text ends as expected");
            const GLuint pz = Link(kFsVtx, zt);
            std::vector<uint8_t> z;
            Draw(pz, x, kClearA, z);
            for (int i = 0; i < kW * kH; ++i) {
                if (!mask[i]) continue;
                ++pxChecked;
                if (o.colorZero) {
                    ++cZeroPx;
                    if (z[i * 4] || z[i * 4 + 1] || z[i * 4 + 2]) {
                        std::fprintf(stderr, "key %u: pixel %d colour not exactly 0\n%s\n", n, i, base.text.c_str());
                        Require(false, "colour exactly 0 where claimed");
                    }
                }
                if (o.alphaZero) {
                    ++aZeroPx;
                    if (z[i * 4 + 3]) {
                        std::fprintf(stderr, "key %u: pixel %d alpha not exactly 0\n%s\n", n, i, base.text.c_str());
                        Require(false, "alpha exactly 0 where claimed");
                    }
                }
            }
            glDeleteProgram(pz);
        }
        if (claimOne || claimLo) {
            // (the alpha the blend reads: exactly 1 where claimed; never below the lower bound)
            for (int pass2 = 0; pass2 < 2; ++pass2) {
                if (pass2 == 0 && !claimOne) continue;
                if (pass2 == 1 && !claimLo) continue;
                const std::string zt = AlphaProbe(base.text, pass2 == 0);
                Require(!zt.empty(), "the text ends as expected (alpha probe)");
                const GLuint pz = Link(kFsVtx, zt);
                std::vector<uint8_t> z;
                Draw(pz, x, kClearA, z);
                for (int i = 0; i < kW * kH; ++i) {
                    if (!mask[i]) continue;
                    if (pass2 == 0) {
                        ++aOnePx;
                        if (z[i * 4 + 3] != 255) { std::fprintf(stderr, "key %u: pixel %d alpha not exactly 1\n%s\n", n, i, base.text.c_str()); Require(false, "alpha exactly 1 where claimed"); }
                    } else {
                        ++aLoPx;
                        const int floorByte = static_cast<int>(std::floor(o.alphaLoOut * 255.0 + 0.5 - 1e-3));
                        if (z[i * 4 + 3] < floorByte) { std::fprintf(stderr, "key %u: pixel %d alpha byte %d below the bound %.4f\n%s\n", n, i, z[i * 4 + 3], o.alphaLoOut, base.text.c_str()); std::fprintf(stderr, "regs: "); for (int rr = 0; rr < 4; ++rr) std::fprintf(stderr, "[%d %d %d %d] ", x.in.reg[rr][0], x.in.reg[rr][1], x.in.reg[rr][2], x.in.reg[rr][3]);
                            for (int m = 0; m < 4; ++m) std::fprintf(stderr, "\ntex%d known %d lo %d %d %d %d hi %d %d %d %d", m, x.in.texKnown[m], x.in.texLo[m][0], x.in.texLo[m][1], x.in.texLo[m][2], x.in.texLo[m][3], x.in.texHi[m][0], x.in.texHi[m][1], x.in.texHi[m][2], x.in.texHi[m][3]);
                            std::fprintf(stderr, "\nras a %.3f..%.3f alphaLo %.4f alphaHi %.4f\n", x.in.rasLo[0][3], x.in.rasHi[0][3], o.alphaLo, o.alphaHi);
                            Require(false, "alpha lower bound holds"); }
                    }
                }
                glDeleteProgram(pz);
            }
        }
        glDeleteProgram(p);
    }
    std::printf("gpu960 pixval: alpha exactly 1 claimed in %zu keys (%zu pixels checked), alpha lower bound in %zu keys (%zu pixels)\n", aOne, aOnePx, aLo, aLoPx);
    std::printf("pixval: %zu keys, %zu analysed; %zu with an alpha test: passes everywhere %zu (%zu pixels checked), fails everywhere %zu, either "
                "%zu; colour exactly 0 in %zu (%zu pixels checked), alpha exactly 0 in %zu (%zu pixels); %zu atPass texts = key text less the test\n",
                keys, analysed, tested, pass, passPx, fail, either, cZero, cZeroPx, aZero, aZeroPx, stripSame);
    Require(pass > 20 && fail > 20 && cZero > 20 && aZero > 20, "every kind of claim made often enough to be tested");
    Require(cZeroPx > 1000 && aZeroPx > 1000 && passPx > 1000, "claims checked on pixels");
    Require(nKeys < 1000 || (aOne > 5 && aLo > 20 && aOnePx > 500 && aLoPx > 1000), "alpha-one / alpha-bound claims made and checked on pixels");
    std::printf("PASS\n");
    return 0;
}
