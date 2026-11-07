// Host test of GXV_DISCARD_BLEND (FsKey::rejectZero, vgx_shadergen.cpp; RejectIsNoop, vgx_cmd.h; RejectZeroFs, vgx_draw.cpp). No game
// data, no Vita executable.
//   tools/test_vgx_discard_blend_gl.sh [output-dir] [keys]
// 1. Text: a key without the bit is written exactly as before (the bit is the only difference the generator looks at); with the bit, an
//    alpha test that can fail leaves no `discard` (and no word "discard" for the gpu-scenes census), the rest of the text is the same up
//    to the test.
// 2. Mesa: random J3D-shaped keys with an alpha test (and Beach Bowl's sea material, OceanBowl::loadMaterial: 4 stages, 5 texture reads,
//    an indirect stage, a projective texgen), drawn over a random RGBA8 target with a depth buffer, with random render states that
//    RejectIsNoop accepts (every blend factor as the source factor, the three destination factors, ADD and REVERSE_SUBTRACT, colour /
//    alpha masks, depth test on with the write off, or off): the picture of the `discard` text and of the rejectZero text must be equal
//    byte for byte. A control with states RejectIsNoop refuses must find differences (the test sees what it is meant to see).
// 3. RejectIsNoop on every combination of the RenderState fields it reads, against the GL blend equation evaluated with a zero source.
#include "vgx_shadergen.h"
#include "vgx_cmd.h"
#include "vgx_fshalf_keys.h"
#include <GLES2/gl2.h>
#include <algorithm>
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
    Require(ok, "program links");
    glDeleteShader(v); glDeleteShader(f);
    return p;
}
const char* kFsVtx =
    "attribute vec4 a_pos; attribute vec4 a_col0; attribute vec4 a_col1; attribute vec3 a_tex;\n"
    "varying vec4 v_col0; varying vec4 v_col1; varying float v_fog;\n"
    "varying vec3 v_tex0; varying vec3 v_tex1; varying vec3 v_tex2; varying vec3 v_tex3; varying vec3 v_tex4; varying vec3 v_tex5; varying vec3 v_tex6; varying vec3 v_tex7;\n"
    "void main() { gl_Position = a_pos; v_col0 = a_col0; v_col1 = a_col1; v_fog = a_tex.z;\n"
    "  v_tex0 = a_tex; v_tex1 = a_tex.yxz; v_tex2 = a_tex * 0.5; v_tex3 = a_tex.xzy; v_tex4 = a_tex; v_tex5 = a_tex; v_tex6 = a_tex; v_tex7 = a_tex; }\n";
constexpr int kW = 32, kH = 32;
float Sample(Rng& r) {
    const unsigned w = r.U(100);
    if (w < 15) return 0.0f;
    if (w < 30) return 1.0f;
    if (w < 60) return static_cast<float>(r.U(256)) / 255.0f;
    return static_cast<float>(r.F());
}
GLenum Factor(uint8_t f) {
    static const GLenum k[] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA,
                               GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR};
    return k[f % 10];
}
// The render state as the back end applies it (vgx_backend_gl.cpp ApplyRenderState / SetBlend / SetColorMask / SetDepthMask).
void Apply(const vgx::RenderState& rs) {
    if (rs.blend) {
        glEnable(GL_BLEND);
        glBlendEquation(rs.blendEq == vgx::BE_REVSUB ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
        if (rs.dstAlphaZero == 1) glBlendFuncSeparate(Factor(rs.blendSrc), Factor(rs.blendDst), GL_ZERO, GL_ZERO);
        else if (rs.dstAlphaZero == 2) glBlendFuncSeparate(Factor(rs.blendSrc), Factor(rs.blendDst), GL_ONE, GL_ZERO);
        else glBlendFunc(Factor(rs.blendSrc), Factor(rs.blendDst));
    } else {
        glDisable(GL_BLEND);
    }
    if (rs.depthTest) {
        glEnable(GL_DEPTH_TEST);
        static const GLenum kCmp[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
        glDepthFunc(kCmp[rs.depthFunc & 7]);
        glDepthMask(rs.depthWrite ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }
    glColorMask(rs.colorWrite, rs.colorWrite, rs.colorWrite, rs.alphaWrite);
}
vgx::RenderState RandomState(Rng& r) {
    vgx::RenderState rs;
    rs.blend = r.P(90) ? 1 : 0;
    rs.blendEq = r.P(25) ? vgx::BE_REVSUB : vgx::BE_ADD;
    rs.blendSrc = static_cast<uint8_t>(r.U(10));
    rs.blendDst = static_cast<uint8_t>(r.U(10));
    rs.depthTest = r.P(70) ? 1 : 0;
    rs.depthFunc = static_cast<uint8_t>(r.U(8));
    rs.depthWrite = r.P(50) ? 1 : 0;
    rs.colorWrite = r.P(90) ? 1 : 0;
    rs.alphaWrite = r.P(70) ? 1 : 0;
    rs.dstAlphaZero = static_cast<uint8_t>(r.P(80) ? 0 : 1 + r.U(2));
    return rs;
}
vgx::RenderState EligibleState(Rng& r) {
    for (;;) {
        vgx::RenderState rs = RandomState(r);
        if (r.P(70)) { static const uint8_t d[3] = {vgx::BF_ONE, vgx::BF_INV_SRC_ALPHA, vgx::BF_INV_SRC_COLOR}; rs.blendDst = d[r.U(3)]; rs.blend = 1; rs.depthWrite = 0; rs.dstAlphaZero = 0; }
        if (vgx::RejectIsNoop(rs)) return rs;
    }
}
// The sea: OceanBowl::loadMaterial above the water (BP values as BuildFsKey reads them).
uint32_t Col(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t bias, uint32_t op, uint32_t cl, uint32_t sc) {
    return d | c << 4 | b << 8 | a << 12 | bias << 16 | op << 18 | cl << 19 | sc << 20;
}
uint32_t Alp(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t bias, uint32_t op, uint32_t cl, uint32_t sc) {
    return d << 4 | c << 7 | b << 10 | a << 13 | bias << 16 | op << 18 | cl << 19 | sc << 20;
}
vgx::FsKey SeaKey() {
    vgx::FsKey k;
    k.numStages = 4; k.numTexGens = 5; k.texProjMask = 1u << 3;
    k.alphaTestOn = 1; k.alphaTest = 4 | 4 << 3 | 1 << 6; // GREATER 0 OR GREATER 0
    k.stage[0].color = Col(8, 15, 15, 15, 0, 0, 0, 0); k.stage[0].alpha = Alp(4, 7, 7, 7, 0, 0, 0, 0);
    k.stage[0].texEnable = 1; k.stage[0].texMap = 0; k.stage[0].texCoord = 0;
    k.stage[1].color = Col(15, 8, 0, 15, 0, 0, 0, 3); k.stage[1].alpha = Alp(7, 4, 0, 7, 0, 0, 0, 1);
    k.stage[1].texEnable = 1; k.stage[1].texMap = 0; k.stage[1].texCoord = 1;
    k.stage[2].color = Col(0, 3, 2, 0, 3, 1, 0, 0); k.stage[2].alpha = Alp(7, 5, 4, 7, 0, 0, 0, 0);
    k.stage[2].texEnable = 1; k.stage[2].texMap = 3; k.stage[2].texCoord = 4; k.stage[2].rasChan = 0;
    k.numIndStages = 1; k.ind[0].texMap = 2; k.ind[0].texCoord = 2;
    k.stage[3].color = Col(15, 8, 4, 0, 0, 0, 1, 0); k.stage[3].alpha = Alp(0, 7, 7, 7, 0, 0, 0, 0);
    k.stage[3].texEnable = 1; k.stage[3].texMap = 1; k.stage[3].texCoord = 3; k.stage[3].ind = 0x270;
    return k;
}
struct FVtx { float pos[4], c0[4], c1[4], t[3]; };
GLuint g_target = 0;
// One draw of 8 random triangles (random depth) with program p over the target filled from `seed`; px: the RGBA8 picture.
void Draw(GLuint p, uint32_t seed, const vgx::RenderState& rs, std::vector<uint8_t>& px, bool sentinel = false) {
    Rng r(seed);
    std::mt19937 rng(seed);
    glUseProgram(p);
    std::vector<float> tev(28 * 4, 0.0f);
    for (int i = 0; i < 16; ++i) tev[i] = static_cast<float>(r.U(256)) / 255.0f;
    for (int i = 16; i < 32; ++i) tev[i] = static_cast<float>(r.U(256)) / 255.0f;
    tev[32] = static_cast<float>(r.U(256)); tev[33] = static_cast<float>(r.U(256)); tev[34] = static_cast<float>(r.U(256)) / 255.0f;
    if (r.P(30)) { tev[32] = 0; tev[33] = 0; }                                    // the sea's references
    for (int i = 36; i < 44; ++i) tev[i] = static_cast<float>(r.F());             // fog parameters and colour
    for (int i = 44; i < 68; ++i) tev[i] = static_cast<float>(r.F()) * 2.0f - 1.0f; // indirect matrices
    for (int i = 68; i < 84; ++i) tev[i] = 1.0f;                                   // indirect scales
    for (int i = 84; i < 100; ++i) tev[i] = 8.0f;                                  // texture sizes
    if (GLint l = glGetUniformLocation(p, "u_tev"); l >= 0) glUniform4fv(l, 28, tev.data());
    static GLuint names[8] = {};
    if (!names[0]) glGenTextures(8, names);
    for (int t = 0; t < 8; ++t) {
        uint8_t texel[8 * 8 * 4];
        for (uint8_t& b : texel) b = static_cast<uint8_t>(r.P(25) ? (r.P(50) ? 0 : 255) : rng());
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
        x.pos[0] = static_cast<float>(r.F()) * 2.4f - 1.2f; x.pos[1] = static_cast<float>(r.F()) * 2.4f - 1.2f;
        x.pos[2] = static_cast<float>(r.F()) * 1.6f - 0.8f; x.pos[3] = 1.0f;
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
    // the target: random bytes (a sentinel for the census), depth a random level
    std::vector<uint8_t> init(kW * kH * 4);
    for (size_t i = 0; i < init.size(); ++i) init[i] = sentinel ? static_cast<uint8_t>(1 + (i & 3)) : static_cast<uint8_t>(r.P(10) ? (r.P(50) ? 0 : 255) : rng());
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, g_target);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, init.data());
    glBindTexture(GL_TEXTURE_2D, names[7]);
    glDepthMask(GL_TRUE);
    glClearDepthf(static_cast<float>(0.3 + 0.4 * r.F()));
    glClear(GL_DEPTH_BUFFER_BIT);
    Apply(rs);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size()));
    // a second draw of the same triangles moved: sees the depth the first left (a depth write would show here)
    for (FVtx& x : v) { x.pos[0] = -x.pos[0]; x.pos[2] *= 0.5f; }
    Attr("a_pos", 4, v[0].pos); Attr("a_col0", 4, v[0].c0); Attr("a_col1", 4, v[0].c1); Attr("a_tex", 3, v[0].t);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size()));
    px.assign(kW * kH * 4, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    Require(glGetError() == GL_NO_ERROR, "GL status");
}
} // namespace

int main(int argc, char** argv) {
    const unsigned nKeys = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 300;
    const int statesPerKey = 6;
    // ---- 3. RejectIsNoop against the GL equation with a zero source (destination value d, every factor evaluated for src = 0)
    {
        size_t yes = 0, all = 0;
        for (int blend = 0; blend < 2; ++blend)
            for (int eq = 0; eq < 2; ++eq)
                for (int sf = 0; sf < 10; ++sf)
                    for (int df = 0; df < 10; ++df)
                        for (int dz = 0; dz < 3; ++dz)
                            for (int m = 0; m < 4; ++m)
                                for (int dt = 0; dt < 2; ++dt)
                                    for (int dw = 0; dw < 2; ++dw) {
                                        vgx::RenderState rs;
                                        rs.blend = static_cast<uint8_t>(blend); rs.blendEq = static_cast<uint8_t>(eq);
                                        rs.blendSrc = static_cast<uint8_t>(sf); rs.blendDst = static_cast<uint8_t>(df);
                                        rs.dstAlphaZero = static_cast<uint8_t>(dz); rs.colorWrite = m & 1; rs.alphaWrite = (m >> 1) & 1;
                                        rs.depthTest = static_cast<uint8_t>(dt); rs.depthWrite = static_cast<uint8_t>(dw);
                                        // the truth: with src = 0, every written channel keeps d for every d in 0..1
                                        bool keep = !(dt && dw);
                                        for (double d : {0.0, 0.25, 1.0}) {
                                            const double dstA = 0.6; // (the target's alpha, for DST_ALPHA factors)
                                            auto F = [&](int f, bool alphaCh) {
                                                switch (f) {
                                                case vgx::BF_ZERO: return 0.0; case vgx::BF_ONE: return 1.0;
                                                case vgx::BF_SRC_COLOR: case vgx::BF_SRC_ALPHA: return 0.0;
                                                case vgx::BF_INV_SRC_COLOR: case vgx::BF_INV_SRC_ALPHA: return 1.0;
                                                case vgx::BF_DST_ALPHA: return dstA; case vgx::BF_INV_DST_ALPHA: return 1.0 - dstA;
                                                case vgx::BF_DST_COLOR: return alphaCh ? dstA : d; default: return 1.0 - (alphaCh ? dstA : d);
                                                }
                                            };
                                            const double c = blend ? (eq ? d * F(df, false) : d * F(df, false)) : 0.0; // (+/- 0 * sf)
                                            if (m & 1) keep = keep && blend && c == d;
                                            if (m & 2) {
                                                double a = 0.0;
                                                if (!blend) a = 0.0;
                                                else if (dz == 1) a = 0.0;          // ZERO/ZERO
                                                else if (dz == 2) a = 0.0;          // ONE/ZERO: the source alpha, 0
                                                else a = dstA * F(df, true);
                                                keep = keep && blend && a == dstA;
                                            }
                                        }
                                        Require(vgx::RejectIsNoop(rs) == keep, "RejectIsNoop is the zero-source truth table");
                                        yes += keep; ++all;
                                    }
        std::printf("RejectIsNoop: %zu of %zu render states take the variant; equal to the zero-source truth table\n", yes, all);
    }

    Require(vgx::DiscardBlend(), "run with GXV_DISCARD_BLEND=1 (the runner sets it)");
    Require(vgx::host::CreateContext(), "Mesa GLES context");
    std::printf("GL renderer: %s\n", glGetString(GL_RENDERER));
    glGenTextures(1, &g_target);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, g_target);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    GLuint fbo = 0, depth = 0;
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_target, 0);
    glGenRenderbuffers(1, &depth); glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, kW, kH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "RGBA8 + depth target");
    glViewport(0, 0, kW, kH); glDisable(GL_DITHER); glDisable(GL_CULL_FACE);

    Rng r(0xD15CA5Du);
    size_t keys = 0, draws = 0, bytes = 0, rejected = 0, ctrlDraws = 0, ctrlDiffer = 0;
    for (unsigned i = 0; i < nKeys + 1; ++i) {
        vgx::FsKey k = i == 0 ? SeaKey() : fshalf_keys::RandomKey(r, true);
        k.alphaTestOn = 1;
        if (i) k.alphaTest = static_cast<uint8_t>((1 + r.U(6)) | ((1 + r.U(6)) << 3) | (r.U(4) << 6)); // (comparisons that can fail)
        if (vgx::AlphaTestAlwaysPasses(k.alphaTest & 7, -1, (k.alphaTest >> 6) & 3, (k.alphaTest >> 3) & 7, -1)) continue;
        vgx::FsKey kz = k;
        kz.setRejectZero(1);
        vgx::ShaderSource a, b;
        vgx::GenerateFragmentShader(k, a);
        vgx::GenerateFragmentShader(kz, b);
        Require(a.text.find("discard") != std::string::npos, "the plain key keeps its discard");
        Require(b.text.find("discard") == std::string::npos, "the rejectZero key has no discard (nor the word: the census reads it)");
        const size_t cut = a.text.find("  if (!(");
        Require(cut != std::string::npos && a.text.compare(0, cut, b.text, 0, cut) == 0, "the texts agree up to the test");
        if (i == 0 && std::getenv("VGX_TEST_VERBOSE")) std::printf("---- sea, rejectZero text\n%s\n", b.text.c_str());
        const GLuint pa = Link(kFsVtx, a.text), pb = Link(kFsVtx, b.text);
        ++keys;
        for (int s = 0; s < statesPerKey; ++s) {
            const uint32_t seed = static_cast<uint32_t>(i * 977u + static_cast<uint32_t>(s) * 131u + 7u);
            const vgx::RenderState rs = EligibleState(r);
            std::vector<uint8_t> pxa, pxb;
            Draw(pa, seed, rs, pxa);
            Draw(pb, seed, rs, pxb);
            ++draws; bytes += pxa.size();
            if (pxa != pxb) {
                size_t n = 0;
                for (size_t j = 0; j < pxa.size(); ++j) n += pxa[j] != pxb[j];
                std::fprintf(stderr, "key %u state %d (blend %d eq %d src %d dst %d cw %d aw %d dz %d zt %d zw %d): %zu bytes differ\n", i, s, rs.blend, rs.blendEq,
                             rs.blendSrc, rs.blendDst, rs.colorWrite, rs.alphaWrite, rs.dstAlphaZero, rs.depthTest, rs.depthWrite, n);
                Require(false, "the rejectZero text draws exactly the picture of the discard text under a state RejectIsNoop accepts");
            }
            // census: fragments that failed the test (vec4(0) written, blend off, over a sentinel)
            {
                vgx::RenderState off;
                off.depthTest = 1; off.depthFunc = 7;
                std::vector<uint8_t> pz;
                Draw(pb, seed, off, pz, true);
                for (size_t j = 0; j < pz.size(); j += 4) rejected += (pz[j] | pz[j + 1] | pz[j + 2] | pz[j + 3]) == 0;
            }
            // control: a state RejectIsNoop refuses
            vgx::RenderState bad;
            do bad = RandomState(r); while (vgx::RejectIsNoop(bad) || !bad.colorWrite);
            Draw(pa, seed, bad, pxa);
            Draw(pb, seed, bad, pxb);
            ++ctrlDraws;
            ctrlDiffer += pxa != pxb;
        }
        glDeleteProgram(pa); glDeleteProgram(pb);
    }
    std::printf("Mesa: %zu keys (the sea and random alpha-tested keys), %zu draws over random targets, %zu bytes compared, 0 differ; "
                "%zu pixels where the test failed\n", keys, draws, bytes, rejected);
    std::printf("control: %zu of %zu draws with states RejectIsNoop refuses differ (expected: many)\n", ctrlDiffer, ctrlDraws);
    Require(rejected > 1000, "the test fails on enough pixels to mean something");
    Require(ctrlDiffer * 4 > ctrlDraws, "the comparison sees a wrong state");
    std::printf("PASS\n");
    return 0;
}
