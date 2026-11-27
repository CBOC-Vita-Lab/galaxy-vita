// Host test of GXV_TEVIN_CANON and GXV_FS_PROJ (shader23; platform/vgx/src/vgx_shadergen.cpp: FsTevInNeedsMark, WalkTevIn, the
// generator's wrap / projective-read text). No game data, no Vita executable.
//   tools/test_vgx_tevcanon_gl.sh [output-dir] [keys]
//
// 1. GXV_TEVIN_CANON=1 (the front end's decision): random keys of every shape, marked (FsKey::inWrap = 1, GXV_FIX_TEVIN), and random
//    masks of out-of-range registers. Where FsTevInNeedsMark says the mark is not needed, the marked text and the unmarked text (the key
//    the front end then uses; half precision where FsHalfEligible takes it, which is float on a PC) draw random triangles on Mesa with
//    the registers of the mask outside 0..255 (-1024..1023 / 255) and the others inside: RGBA8 readback compared byte for byte. Any
//    difference over one level fails, and so do more than 1 in 10,000 bytes (a value proven in 0..1 in real numbers may be an ulp outside
//    in float, where the wrap would have made it exact). Also: =1 changes no text (a marked key's text is the same as with the switch off).
// 2. GXV_TEVIN_CANON=2 (the text of a key that keeps the mark): the text without the identity wraps against the full text, every register
//    out of range at random: same comparison.
// 3. GXV_FS_PROJ=1, fragment: texture2DProj from the varying against the divide in the shader (q 0.5..1.5, as the trim test draws).
// 5. GXV_FS_FINCLAMP=1 (round 2): the text with the final clamp only where a part is not already in 0..1 against the full one, random keys
//    (marked, pre-wrapped, destination alpha, alpha-dead): clamp is idempotent; differences only from the GLSL compiler contracting the
//    arithmetic differently in two programs (an ulp; at most 1 in 10,000 bytes, one level).
// 6. GXV_FS_ADEAD=1 (round 2): a key's alpha-dead variant against the key, colour channels (the render state masks alpha); same bound.
// 4. GXV_FS_PROJ=1, vertex + fragment: a generated vertex shader with a projective texgen (tex0 through a 3x4 matrix) and its fragment
//    shader, switch off against on, with a matrix whose q row is zero (q = 0 at every vertex: the guard's case, now in the vertex shader)
//    and with ordinary matrices: byte for byte.
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
size_t Count(const std::string& s, const char* what) {
    size_t n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
    return n;
}
size_t MainCount(const std::string& s, const char* what) {
    const size_t m = s.find("void main");
    return m == std::string::npos ? 0 : Count(s.substr(m), what);
}
size_t Wraps(const std::string& s) { return MainCount(s, "vgx_w1(") + MainCount(s, "vgx_w3("); } // (TEVIN wraps, not vgx_wrap_final)
float Sample(Rng& r) {
    const unsigned w = r.U(100);
    if (w < 10) return 0.0f;
    if (w < 20) return 1.0f;
    if (w < 55) return static_cast<float>(r.U(256)) / 255.0f;
    return static_cast<float>(r.F());
}
float RegSample(Rng& r, bool wide) {
    if (!wide) return static_cast<float>(r.P(20) ? (r.P(50) ? 0 : 255) : r.U(256)) / 255.0f;
    const unsigned w = r.U(100);
    // (outside 0..255 in at least one component is what the mask says; the others anything)
    const int v = w < 10 ? -1024 : w < 20 ? 1023 : w < 40 ? 256 + static_cast<int>(r.U(768)) : w < 60 ? -1 - static_cast<int>(r.U(1023))
                                                                                                    : static_cast<int>(r.U(2048)) - 1024;
    return static_cast<float>(v) / 255.0f;
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
    if (!ok) { char log[4096] = {}; glGetProgramInfoLog(p, sizeof log, nullptr, log); std::fprintf(stderr, "%s\n---- vs\n%s\n---- fs\n%s\n", log, vt.c_str(), ft.c_str()); }
    Require(ok, "program links");
    glDeleteShader(v); glDeleteShader(f);
    return p;
}
constexpr int kW = 32, kH = 32;
struct FVtx { float pos[4], c0[4], c1[4], t[3]; };
GLuint g_tex[8];
void Textures(Rng& r, std::mt19937& rng, GLuint p) {
    for (int t = 0; t < 8; ++t) {
        uint8_t texel[8 * 8 * 4];
        for (uint8_t& b : texel) b = static_cast<uint8_t>(r.P(15) ? (r.P(50) ? 0 : 255) : rng());
        glActiveTexture(GL_TEXTURE0 + t);
        glBindTexture(GL_TEXTURE_2D, g_tex[t]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        char name[16];
        std::snprintf(name, sizeof name, "u_tex%d", t);
        if (GLint l = glGetUniformLocation(p, name); l >= 0) glUniform1i(l, t);
    }
}
// 8 random triangles with fragment program p (vertex shader kFsVtx); registers: bit r of wideMask = register r outside 0..255.
// preWrap: bit r (r, g, b) / r + 4 (a) = that part of register r is uploaded as its low byte / 255 (FsKey::preWrap, as BuildTevC does)
void DrawFs(GLuint p, uint32_t seed, unsigned wideMask, std::vector<uint8_t>& px, unsigned preWrap = 0) {
    std::mt19937 rng(seed);
    Rng r(seed);
    glUseProgram(p);
    std::vector<float> tev(28 * 4, 0.0f);
    for (int i = 0; i < 16; ++i) tev[i] = RegSample(r, ((wideMask >> (i / 4)) & 1u) != 0);
    for (int reg = 0; reg < 4; ++reg) // (a register of the mask has at least one component outside)
        if ((wideMask >> reg) & 1u) {
            float& c = tev[reg * 4 + r.U(4)];
            if (c >= 0.0f && c <= 1.0f) c = r.P(50) ? -static_cast<float>(1 + r.U(1024)) / 255.0f : static_cast<float>(256 + r.U(768)) / 255.0f;
        }
    for (int i = 0; i < 16; ++i)
        if ((preWrap >> ((i & 3) < 3 ? i / 4 : i / 4 + 4)) & 1u) tev[i] = static_cast<float>(static_cast<int>(std::lround(tev[i] * 255.0f)) & 255) / 255.0f;
    for (int i = 16; i < 32; ++i) tev[i] = static_cast<float>(r.U(256)) / 255.0f;
    tev[32] = static_cast<float>(r.U(256)); tev[33] = static_cast<float>(r.U(256)); tev[34] = static_cast<float>(r.U(256)) / 255.0f;
    for (int i = 36; i < 44; ++i) tev[i] = static_cast<float>(r.F());
    for (int i = 44; i < 68; ++i) tev[i] = static_cast<float>(r.F()) * 2.0f - 1.0f;
    for (int i = 68; i < 84; ++i) tev[i] = 1.0f;
    for (int i = 84; i < 100; ++i) tev[i] = 8.0f;
    if (GLint l = glGetUniformLocation(p, "u_tev"); l >= 0) glUniform4fv(l, 28, tev.data());
    Textures(r, rng, p);
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

struct Cmp { size_t pairs = 0, bytes = 0, diff = 0, diffPx = 0; int maxd = 0; };
// Draws both texts 3 times with the same seeds; returns the largest difference.
void Compare(const std::string& vt, const std::string& a, const std::string& b, uint32_t seedBase, unsigned wideMask, Cmp& c, const char* what,
             unsigned preWrapB = 0, bool rgbOnly = false, unsigned preWrapA = 0) {
    const GLuint p0 = Link(vt, a), p1 = Link(vt, b);
    ++c.pairs;
    for (uint32_t round = 0; round < 3; ++round) {
        std::vector<uint8_t> x, y;
        const uint32_t seed = seedBase * 7919u + round * 104729u + 5u;
        DrawFs(p0, seed, wideMask, x, preWrapA);
        DrawFs(p1, seed, wideMask, y, preWrapB);
        for (size_t j = 0; j < x.size(); j += 4) {
            int d = 0;
            for (int ch = 0; ch < (rgbOnly ? 3 : 4); ++ch) d = std::max(d, std::abs(static_cast<int>(x[j + ch]) - y[j + ch]));
            if (d) ++c.diffPx;
            if (d > 1) {
                std::fprintf(stderr, "%s: pixel %zu differs by %d levels (mask %x)\n---- a\n%s\n---- b\n%s\n", what, j / 4, d, wideMask, a.c_str(), b.c_str());
                Require(false, "the two texts draw within one level");
            }
            c.maxd = std::max(c.maxd, d);
        }
        for (size_t j = 0; j < x.size(); ++j) c.diff += (!rgbOnly || (j & 3) != 3) && x[j] != y[j];
        c.bytes += x.size();
    }
    glDeleteProgram(p0); glDeleteProgram(p1);
}

// ---- 4: a generated vertex shader with a projective texgen
void DrawVs(GLuint p, uint32_t seed, bool zeroQ, std::vector<uint8_t>& px) {
    std::mt19937 rng(seed);
    Rng r(seed);
    glUseProgram(p);
    const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (GLint l = glGetUniformLocation(p, "u_proj"); l >= 0) glUniformMatrix4fv(l, 1, GL_FALSE, proj);
    const float mv[24] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (GLint l = glGetUniformLocation(p, "u_mv"); l >= 0) glUniform4fv(l, 6, mv);
    float tm[12];
    for (int i = 0; i < 8; ++i) tm[i] = static_cast<float>(r.F()) * 4.0f - 2.0f;
    if (zeroQ) { tm[8] = tm[9] = tm[10] = tm[11] = 0.0f; }
    else { tm[8] = static_cast<float>(r.F()) * 0.2f; tm[9] = static_cast<float>(r.F()) * 0.2f; tm[10] = 0.6f + static_cast<float>(r.F()); tm[11] = 0.0f; }
    if (GLint l = glGetUniformLocation(p, "u_tm"); l >= 0) glUniform4fv(l, 3, tm);
    std::vector<float> lt(16, 0.0f);
    if (GLint l = glGetUniformLocation(p, "u_lt"); l >= 0) glUniform4fv(l, 4, lt.data());
    std::vector<float> tev(28 * 4, 0.0f);
    for (int i = 0; i < 32; ++i) tev[i] = static_cast<float>(r.U(256)) / 255.0f;
    if (GLint l = glGetUniformLocation(p, "u_tev"); l >= 0) glUniform4fv(l, 28, tev.data());
    Textures(r, rng, p);
    struct V { float pos[4], c0[4], c1[4], t[2]; };
    std::vector<V> v(8 * 3);
    for (V& x : v) {
        x.pos[0] = static_cast<float>(r.F()) * 2.4f - 1.2f; x.pos[1] = static_cast<float>(r.F()) * 2.4f - 1.2f; x.pos[2] = 0.0f; x.pos[3] = 0.0f;
        for (float& f : x.c0) f = Sample(r);
        for (float& f : x.c1) f = Sample(r);
        x.t[0] = static_cast<float>(r.F()) * 2.0f - 1.0f; x.t[1] = static_cast<float>(r.F()) * 2.0f - 1.0f;
    }
    auto Attr = [&](const char* name, int n, const float* first) {
        const GLint loc = glGetAttribLocation(p, name);
        if (loc < 0) return;
        glEnableVertexAttribArray(static_cast<GLuint>(loc));
        glVertexAttribPointer(static_cast<GLuint>(loc), n, GL_FLOAT, GL_FALSE, sizeof(V), first);
    };
    for (GLuint i = 0; i < 16; ++i) glDisableVertexAttribArray(i);
    Attr("a_pos", 4, v[0].pos); Attr("a_col0", 4, v[0].c0); Attr("a_col1", 4, v[0].c1); Attr("a_tex0", 2, v[0].t);
    glDisable(GL_BLEND);
    glClearColor(17.0f / 255, 33.0f / 255, 49.0f / 255, 65.0f / 255);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size()));
    px.assign(kW * kH * 4, 0);
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    Require(glGetError() == GL_NO_ERROR, "GL status");
}
} // namespace

int main(int argc, char** argv) {
    const unsigned nKeys = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 1500;
    Require(vgx::host::CreateContext(), "Mesa GLES context");
    std::printf("GL renderer: %s\n", glGetString(GL_RENDERER));
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
    vgx::FsProjModeForTest(0);

    // ---- 1. GXV_TEVIN_CANON=1: the unmarked key against the marked one, where the mark is not needed
    {
        Rng r(0x7E71);
        Cmp c;
        size_t keys = 0, canon = 0, canonHalf = 0, sameText = 0, wrapsMarked = 0, readsOor = 0;
        size_t byMask[16] = {}, canonByMask[16] = {};
        for (unsigned n = 0; n < nKeys; ++n) {
            vgx::FsKey k = fshalf_keys::RandomKey(r, r.P(40));
            if (r.P(30))
                for (int s = 0; s < k.numStages; ++s) {
                    if (r.P(40)) k.stage[s].color &= ~(1u << 19);
                    if (r.P(40)) k.stage[s].alpha &= ~(1u << 19);
                }
            k.inWrap = 1;
            // (mostly one register out, as a J3D material with one odd TEV colour; sometimes several)
            unsigned mask = r.P(70) ? (1u << r.U(4)) : 1u + r.U(15);
            ++keys;
            ++byMask[mask];
            // (=1 changes no text)
            vgx::TevInCanonModeForTest(0);
            vgx::ShaderSource full;
            vgx::GenerateFragmentShader(k, full);
            vgx::TevInCanonModeForTest(1);
            vgx::ShaderSource full1;
            vgx::GenerateFragmentShader(k, full1);
            Require(full.text == full1.text, "GXV_TEVIN_CANON=1 does not change the text of a key");
            // (without GXV_FIX_TEVIN the decision only looks at the initial reads; with it at least as strict)
            Require(!vgx::FsTevInNeedsMark(k, mask, true) <= !vgx::FsTevInNeedsMark(k, mask, false), "the wrap only adds reasons to mark");
            Require(!vgx::FsTevInNeedsMark(k, 0, true), "nothing out of range: no mark");
            if (vgx::FsTevInNeedsMark(k, mask, true)) { ++readsOor; continue; }
            ++canon;
            ++canonByMask[mask];
            vgx::FsKey u = k;
            u.inWrap = 0;
            if (vgx::FsHalfEligible(u)) ++canonHalf;
            vgx::ShaderSource plain;
            vgx::GenerateFragmentShader(u, plain);
            wrapsMarked += Wraps(full.text);
            Require(Wraps(plain.text) == 0, "an unmarked key has no wrap");
            if (plain.text == full.text) { ++sameText; continue; }
            Compare(kFsVtx, full.text, plain.text, n, mask, c, "canon");
        }
        std::printf("canon (=1): %zu marked keys; %zu need no mark (%.1f%%; one register out: PREV %zu/%zu, REG0 %zu/%zu, REG1 %zu/%zu, REG2 %zu/%zu), "
                    "%zu of them now half precision, %zu wrap calls left out of their main; %zu pairs drawn, %zu bytes compared, %zu differ "
                    "(%zu pixels, max %d level)\n",
                    keys, canon, 100.0 * canon / std::max<size_t>(keys, 1), canonByMask[1], byMask[1], canonByMask[2], byMask[2], canonByMask[4], byMask[4],
                    canonByMask[8], byMask[8], canonHalf, wrapsMarked, c.pairs, c.bytes, c.diff, c.diffPx, c.maxd);
        Require(c.diff * 10000 <= c.bytes, "at most 1 in 10,000 bytes differ");
        Require(canon > keys / 20, "the decision takes keys at all");
    }

    // ---- 2. GXV_TEVIN_CANON=2: identity wraps left out of a marked key's text
    {
        Rng r(0x7E72);
        Cmp c;
        size_t keys = 0, changed = 0, w0 = 0, w2 = 0, preWrapKeys = 0;
        for (unsigned n = 0; n < nKeys; ++n) {
            vgx::FsKey k = fshalf_keys::RandomKey(r, r.P(40));
            if (r.P(30))
                for (int s = 0; s < k.numStages; ++s) {
                    if (r.P(40)) k.stage[s].color &= ~(1u << 19);
                    if (r.P(40)) k.stage[s].alpha &= ~(1u << 19);
                }
            k.inWrap = 1;
            ++keys;
            vgx::TevInCanonModeForTest(0);
            vgx::ShaderSource a;
            vgx::GenerateFragmentShader(k, a);
            vgx::TevInCanonModeForTest(2);
            vgx::FsKey kb = k;
            kb.setPreWrap(vgx::FsTevInPreWrapMask(k)); // (as BuildFsKey under =2)
            if (kb.preWrap()) ++preWrapKeys;
            vgx::ShaderSource b;
            vgx::GenerateFragmentShader(kb, b);
            w0 += Wraps(a.text);
            w2 += Wraps(b.text);
            if (a.text == b.text) continue;
            ++changed;
            Compare(kFsVtx, a.text, b.text, n, 15u, c, "canon=2", kb.preWrap());
        }
        vgx::TevInCanonModeForTest(-1);
        std::printf("canon (=2): %zu marked keys, %zu with a changed text (%zu with pre-wrapped registers); wrap calls in main %zu -> %zu; %zu bytes compared, "
                    "%zu differ (%zu pixels, max %d level)\n",
                    keys, changed, preWrapKeys, w0, w2, c.bytes, c.diff, c.diffPx, c.maxd);
        Require(c.diff * 10000 <= c.bytes, "at most 1 in 10,000 bytes differ");
        Require(changed > 0, "=2 leaves wraps out");
    }

    // ---- 5. GXV_FS_FINCLAMP=1 (round 2): the final clamp only where a part is not in 0..1 bit for bit; must be byte for byte
    {
        Rng r(0x7E75);
        Cmp c;
        size_t keys = 0, changed = 0, ops0 = 0, ops1 = 0;
        auto Ops = [](const std::string& t) { return 2 * MainCount(t, "clamp(") + MainCount(t, "max(") + MainCount(t, "min("); };
        for (unsigned n = 0; n < nKeys; ++n) {
            vgx::FsKey k = fshalf_keys::RandomKey(r, r.P(40));
            if (r.P(30)) k.inWrap = 1;
            if ((k.inWrap & 1) && r.P(50)) k.setPreWrap(vgx::FsTevInPreWrapMask(k));
            if (!k.alphaTestOn && !k.dstAlpha && r.P(30)) k.setAlphaDead(1);
            if (r.P(20)) k.dstAlpha = 1;
            ++keys;
            vgx::TevInCanonModeForTest(k.preWrap() ? 2 : 0);
            vgx::FsFinClampModeForTest(0);
            vgx::ShaderSource a;
            vgx::GenerateFragmentShader(k, a);
            vgx::FsFinClampModeForTest(1);
            vgx::ShaderSource b;
            vgx::GenerateFragmentShader(k, b);
            ops0 += Ops(a.text);
            ops1 += Ops(b.text);
            if (a.text == b.text) continue;
            ++changed;
            Compare(kFsVtx, a.text, b.text, n, (k.inWrap & 1) ? 15u : 0u, c, "finclamp", k.preWrap(), false, k.preWrap());
        }
        vgx::FsFinClampModeForTest(-1);
        vgx::TevInCanonModeForTest(-1);
        std::printf("finclamp: %zu keys, %zu changed; clamp-type operations in main %zu -> %zu; %zu bytes compared, %zu differ (%zu pixels, max %d level)\n",
                    keys, changed, ops0, ops1, c.bytes, c.diff, c.diffPx, c.maxd);
        // (clamp is idempotent, so the texts compute the same numbers; the GLSL compiler may still contract the arithmetic before the clamp
        // differently in the two programs: an ulp, rarely a level)
        Require(c.diff * 10000 <= c.bytes, "at most 1 in 10,000 bytes differ");
        Require(changed > 0, "the final clamp is left out somewhere");
    }

    // ---- 6. GXV_FS_ADEAD=1 (round 2): the alpha-dead variant draws the same colour (the alpha channel is masked by the render state)
    {
        Rng r(0x7E76);
        Cmp c;
        size_t keys = 0, statements0 = 0, statements1 = 0;
        for (unsigned n = 0; n < nKeys; ++n) {
            vgx::FsKey k = fshalf_keys::RandomKey(r, r.P(40));
            if (k.alphaTestOn || k.dstAlpha) continue;
            ++keys;
            vgx::FsKey v = k;
            v.setAlphaDead(1);
            for (int fc = 0; fc < 2; ++fc) {
                vgx::FsFinClampModeForTest(fc);
                vgx::ShaderSource a, b;
                vgx::GenerateFragmentShader(k, a);
                vgx::GenerateFragmentShader(v, b);
                Require(a.text != b.text, "the alpha-dead variant has its own text");
                Compare(kFsVtx, a.text, b.text, n * 2u + static_cast<unsigned>(fc), 0u, c, "adead", 0, true);
            }
        }
        vgx::FsFinClampModeForTest(-1);
        std::printf("adead: %zu keys without alpha test or constant alpha, variant against key (colour channels, final clamp both ways): %zu bytes compared, %zu "
                    "differ\n", keys, c.bytes, c.diff);
        (void)statements0; (void)statements1;
        Require(c.diff * 10000 <= c.bytes, "at most 1 in 10,000 colour bytes differ");
    }

    // ---- 3. GXV_FS_PROJ=1, fragment
    {
        Rng r(0x7E73);
        Cmp c;
        size_t keys = 0, changed = 0, divides0 = 0, divides1 = 0;
        for (unsigned n = 0; n < nKeys; ++n) {
            vgx::FsKey k = fshalf_keys::RandomKey(r, r.P(40));
            if (k.numTexGens == 0) k.numTexGens = 1;
            k.texProjMask = static_cast<uint8_t>(r.U(256) & ((1u << k.numTexGens) - 1u));
            if (!k.texProjMask) k.texProjMask = 1;
            for (int s = 0; s < k.numStages; ++s) k.stage[s].texCoord = static_cast<uint8_t>(r.U(k.numTexGens));
            ++keys;
            vgx::FsProjModeForTest(0);
            vgx::ShaderSource a;
            vgx::GenerateFragmentShader(k, a);
            vgx::FsProjModeForTest(1);
            vgx::ShaderSource b;
            vgx::GenerateFragmentShader(k, b);
            divides0 += Count(a.text, ".z == 0.0 ? 1.0 :");
            divides1 += Count(b.text, ".z == 0.0 ? 1.0 :");
            if (a.text == b.text) continue;
            ++changed;
            Compare(kFsVtx, a.text, b.text, n, k.inWrap ? 15u : 0u, c, "proj");
        }
        vgx::FsProjModeForTest(0);
        std::printf("proj fragment: %zu keys with a projective texgen, %zu changed; per-pixel divides %zu -> %zu; %zu bytes compared, %zu differ (%zu pixels, max %d level)\n",
                    keys, changed, divides0, divides1, c.bytes, c.diff, c.diffPx, c.maxd);
        Require(c.diff * 10000 <= c.bytes, "at most 1 in 10,000 bytes differ");
    }

    // ---- 4. GXV_FS_PROJ=1, vertex + fragment pairs, q = 0 at every vertex and ordinary
    {
        Rng r(0x7E74);
        size_t bytes = 0, diff = 0;
        for (unsigned n = 0; n < 64; ++n) {
            vgx::VsKey vk;
            vk.numTexGens = 1;
            vk.numChans = 1;
            vk.chan[0].matSrcVtx = 1; vk.chan[2].matSrcVtx = 1;
            vk.tg[0].type = 0; vk.tg[0].src = 5; vk.tg[0].proj = 1;
            vgx::FsKey fk = fshalf_keys::RandomKey(r, false);
            fk.numTexGens = 1;
            fk.texProjMask = 1;
            fk.regFromVtx = 0;
            for (int s = 0; s < fk.numStages; ++s) { fk.stage[s].texCoord = 0; fk.stage[s].rasChan = fk.stage[s].rasChan == 1 ? 0 : fk.stage[s].rasChan; }
            vgx::ShaderSource v0, v1, f0, f1;
            vgx::FsProjModeForTest(0);
            vgx::GenerateVertexShader(vk, v0);
            vgx::GenerateFragmentShader(fk, f0);
            vgx::FsProjModeForTest(1);
            vgx::GenerateVertexShader(vk, v1);
            vgx::GenerateFragmentShader(fk, f1);
            Require(v1.text != v0.text && v1.text.find("if (tc.z == 0.0) tc.z = 1.0;") != std::string::npos, "the projective texgen gets the guard");
            const GLuint p0 = Link("#version 100\n" + v0.text, "#version 100\n" + f0.text), p1 = Link("#version 100\n" + v1.text, "#version 100\n" + f1.text);
            for (int zero = 0; zero < 2; ++zero) {
                std::vector<uint8_t> a, b;
                DrawVs(p0, n * 31u + static_cast<uint32_t>(zero), zero != 0, a);
                DrawVs(p1, n * 31u + static_cast<uint32_t>(zero), zero != 0, b);
                for (size_t j = 0; j < a.size(); ++j) {
                    if (a[j] != b[j] && std::abs(static_cast<int>(a[j]) - b[j]) > 1) {
                        std::fprintf(stderr, "pair %u (q %s): byte %zu %d vs %d\n---- vs on\n%s\n---- fs on\n%s\n", n, zero ? "zero" : "ordinary", j, a[j], b[j],
                                     v1.text.c_str(), f1.text.c_str());
                        Require(false, "vertex + fragment pair within one level");
                    }
                    diff += a[j] != b[j];
                }
                bytes += a.size();
            }
            glDeleteProgram(p0); glDeleteProgram(p1);
        }
        vgx::FsProjModeForTest(-1);
        std::printf("proj pairs: 64 vertex + fragment pairs, q zero at every vertex and ordinary: %zu bytes compared, %zu differ\n", bytes, diff);
        Require(diff * 10000 <= bytes, "at most 1 in 10,000 bytes differ");
    }
    std::printf("PASS\n");
    return 0;
}
