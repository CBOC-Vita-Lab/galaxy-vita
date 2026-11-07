// Host-side test of GXV_IND_VS (platform/vgx/src/vgx_shadergen.cpp IndVsPair, GenerateFragmentShaderAs / GenerateVertexShader): a
// texgen that feeds only indirect lookups is written by the vertex shader already multiplied by the lookups' coordinate scale
// (2^-s, 2^-t; GXSetIndTexCoordScale), and the lookup reads the varying as it is (a non-dependent texture read on the console's GPU).
// Real generated GLES2 shaders on Mesa; no game data, no Vita executable.
//   tools/test_vgx_indvs_gl.sh [output-dir] [pairs]
//
// For Beach Bowl's sea (OceanBowl::loadMaterial: the indirect map on texgen 2, nothing else reads it) and random pairs of J3D-shaped
// fragment keys with indirect stages (half of them given a texgen of their own for the lookup) and vertex keys (every texgen source,
// post matrices, palettes, colour texgens), with random scales per indirect stage (BP 0x25 / 0x26), the binary draws random triangles
// with the pair as the front end makes it with the switch off and with IndVsPair's marks:
//   - pictures: RGBA8 readback, same uniforms (u_tev's scales hold what the unmarked shader multiplies by), textures and vertices,
//     equal bit for bit;
//   - coordinate probe: for each marked texgen, a fragment shader that writes the interpolated lookup coordinate's bits
//     (fract(c * 2^k) for k = 0, 8, 16, 24: bits 2^-1 .. 2^-32 of each component) from the unmarked pair (v_texN.xy * scale, per
//     pixel) and from the marked one (v_texN as interpolated): equal bit for bit, i.e. the very same float coordinate;
//   - control: the marked vertex shader with its coordinate made 1 ulp larger (* 1.0000001) must make the probe differ (the probe
//     sees a one-ulp change), and is reported for the pictures.
// It prints a digest of the switch-off texts; built with -DINDVS_REF against the tree before the switch, it prints the same digest
// (the runner compares them): unmarked keys keep their text.
#include "vgx_shadergen.h"
#include "vgx_fshalf_keys.h"
#include <GLES2/gl2.h>
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
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
uint64_t Fnv(uint64_t h, const void* d, size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(d);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
[[maybe_unused]] GLuint Compile(GLenum type, const std::string& text) {
    GLuint s = glCreateShader(type);
    const char* p = text.c_str(); glShaderSource(s, 1, &p, nullptr); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096] = {}; glGetShaderInfoLog(s, sizeof log, nullptr, log); std::fprintf(stderr, "%s\n%s\n", log, p); }
    Require(ok, "generated shader compiles"); return s;
}
[[maybe_unused]] GLuint Link(GLuint v, GLuint f, const std::string& vt, const std::string& ft) {
    const GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[4096] = {}; glGetProgramInfoLog(p, sizeof log, nullptr, log); std::fprintf(stderr, "%s\n---- vs\n%s\n---- fs\n%s\n", log, vt.c_str(), ft.c_str()); }
    Require(ok, "program links"); return p;
}
uint32_t Col(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t bias, uint32_t op, uint32_t cl, uint32_t sc) {
    return d | c << 4 | b << 8 | a << 12 | bias << 16 | op << 18 | cl << 19 | sc << 20;
}
uint32_t Alp(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t bias, uint32_t op, uint32_t cl, uint32_t sc) {
    return d << 4 | c << 7 | b << 10 | a << 13 | bias << 16 | op << 18 | cl << 19 | sc << 20;
}
// The sea: OceanBowl::loadMaterial above the water (as tests/vgx_discard_blend_gl.cpp has it).
vgx::FsKey SeaKey() {
    vgx::FsKey k;
    k.numStages = 4; k.numTexGens = 5; k.texProjMask = 1u << 3;
    k.alphaTestOn = 1; k.alphaTest = 4 | 4 << 3 | 1 << 6;
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
vgx::VsKey SeaVs() {
    vgx::VsKey k;
    k.numTexGens = 5; k.numChans = 1;
    k.chan[0].matSrcVtx = 1; k.chan[2].matSrcVtx = 1;
    for (int i = 0; i < 5; ++i) { k.tg[i].src = static_cast<uint8_t>(5 + (i & 1)); }
    k.tg[3].src = 0; k.tg[3].proj = 1; k.tg[3].inputAbc1 = 1; // (the screen projection)
    return k;
}
// Random fragment keys with at least one indirect stage; often a texgen of its own for the lookup (the shape the switch takes).
vgx::FsKey RandomIndKey(fshalf_keys::Rng& r) {
    vgx::FsKey k = fshalf_keys::RandomKey(r, true);
    if (!k.numIndStages) {
        k.numIndStages = static_cast<uint8_t>(1 + r.U(2));
        for (int s = 0; s < k.numStages; ++s)
            if (r.P(50)) k.stage[s].ind = 1u + r.U(0x1FFFFE);
        for (int i = 0; i < k.numIndStages; ++i) {
            k.ind[i].texMap = static_cast<uint8_t>(r.U(4));
            k.ind[i].texCoord = static_cast<uint8_t>(k.numTexGens ? r.U(k.numTexGens) : 0);
        }
    }
    if (r.P(65) && k.numTexGens < 6) {
        const int c = k.numTexGens++;
        k.texProjMask = static_cast<uint8_t>(k.texProjMask & ~(1u << c));
        k.ind[0].texCoord = static_cast<uint8_t>(c);
        if (k.numIndStages > 1 && r.P(50)) k.ind[1].texCoord = static_cast<uint8_t>(c);
    }
    // (some stage reads the indirect results: make one indirect stage use ind stage 0 now and then)
    if (r.P(50)) k.stage[k.numStages - 1].ind = (k.stage[k.numStages - 1].ind & ~3u) | 0x200u;
    return k;
}
// A vertex key for the fragment key (as tests/vgx_vsout_gl.cpp makes them).
vgx::VsKey RandomVs(fshalf_keys::Rng& r, const vgx::FsKey& fs) {
    vgx::VsKey k;
    k.numTexGens = fs.numTexGens;
    if (fs.fog & vgx::kFogVtxBit) k.fogVtx = static_cast<uint8_t>((fs.fog & 8) ? 3 : 1);
    const unsigned nc = r.U(100);
    k.numChans = static_cast<uint8_t>(nc < 20 ? 0 : nc < 80 ? 1 : 2);
    const unsigned pv = r.U(100);
    k.perVtxPos = static_cast<uint8_t>(pv < 55 ? 0 : pv < 70 ? 1 : pv < 85 ? 2 : 3);
    for (int i = 0; i < 4; ++i) {
        if ((i & 1) >= k.numChans) continue;
        vgx::VsKey::Chan& c = k.chan[i];
        c.matSrcVtx = r.P(50) ? 1 : 0;
        c.enable = r.P(50) ? 1 : 0;
        if (!c.enable) continue;
        c.lightMask = static_cast<uint8_t>(r.P(30) ? 0 : 1u + r.U(3));
        if (c.lightMask) { c.diffuse = static_cast<uint8_t>(r.U(3)); c.attn = static_cast<uint8_t>(r.U(3)); }
    }
    bool texFromPal = false;
    for (int i = 0; i < k.numTexGens; ++i) {
        vgx::VsKey::TexGen& t = k.tg[i];
        const unsigned ty = r.U(100);
        t.type = static_cast<uint8_t>(ty < 88 ? 0 : ty < 94 ? 2 : 3);
        if (t.type >= 2) { t.src = 2; continue; }
        const unsigned s = r.U(100);
        t.src = static_cast<uint8_t>(s < 15 ? 0 : s < 25 ? 1 : 5 + r.U(4));
        t.proj = static_cast<uint8_t>((fs.texProjMask >> i) & 1u);
        t.inputAbc1 = r.P(30) ? 1 : 0;
        t.post = static_cast<uint8_t>(r.P(25) ? 1 + r.U(2) : 0);
        if (k.perVtxPos <= 1 && r.P(15)) { t.perVtxMtx = 1; texFromPal = true; }
    }
    if (texFromPal && k.perVtxPos >= 2) k.perVtxPos = 1;
    return k;
}
// BP 0x25 / 0x26 for the key: s, t per indirect stage (mostly GX_ITS_1 .. GX_ITS_256, now and then up to 15); stages that share a
// texgen get the same scale most of the time.
void RandomScales(fshalf_keys::Rng& r, const vgx::FsKey& k, uint32_t& bp25, uint32_t& bp26, float sc[4][2]) {
    uint32_t st[4];
    for (int i = 0; i < 4; ++i) {
        const uint32_t s = r.P(90) ? r.U(9) : r.U(16), t = r.P(50) ? s : (r.P(90) ? r.U(9) : r.U(16));
        st[i] = s | t << 4;
    }
    for (int i = 1; i < 4; ++i)
        for (int j = 0; j < i; ++j)
            if (k.ind[i].texCoord == k.ind[j].texCoord && r.P(70)) st[i] = st[j];
    bp25 = st[0] | st[1] << 8;
    bp26 = st[2] | st[3] << 8;
    for (int i = 0; i < 4; ++i) {
        sc[i][0] = std::ldexp(1.0f, -static_cast<int>(st[i] & 15u));
        sc[i][1] = std::ldexp(1.0f, -static_cast<int>(st[i] >> 4));
    }
}

struct Vtx { float pos[4], nrm[3], c0[4], c1[4], tex[8][2], tmi0[4], tmi1[4]; };
constexpr int kW = 64, kH = 64, kTris = 24, kTexSize = 32;

// Uniforms, textures and vertices from `seed` (the same for both programs of a pair), then one draw; px: the RGBA8 picture.
// probe: a probe program; its u_s gets the scale of `probeStage` (the unmarked probe multiplies by it) and u_k = 2^k.
[[maybe_unused]] uint64_t Draw(GLuint prog, const vgx::ShaderSource& vs, int nTev, uint32_t seed, const float sc[4][2], std::vector<uint8_t>& px,
                               int probeStage = -1, float probeK = 1.0f) {
    std::mt19937 rng(seed);
    auto rf = [&](float lo, float hi) { return lo + (hi - lo) * static_cast<float>(rng() / 4294967296.0); };
    glUseProgram(prog);
    auto U4 = [&](const char* name, int n, const std::vector<float>& v) {
        const GLint loc = glGetUniformLocation(prog, name);
        if (loc >= 0 && n > 0) glUniform4fv(loc, n, v.data());
    };
    std::vector<float> proj = {0.9f, 0.05f, 0.0f, 0.0f, -0.04f, 0.85f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.02f, -0.03f, 0.0f, 1.0f};
    for (float& f : proj) f += rf(-0.02f, 0.02f);
    proj[3] = rf(0.0f, 0.3f); proj[7] = rf(-0.3f, 0.3f); // (a w that varies over the triangle: perspective-correct interpolation)
    if (GLint l = glGetUniformLocation(prog, "u_proj"); l >= 0) glUniformMatrix4fv(l, 1, GL_FALSE, proj.data());
    std::vector<float> mv(24);
    for (int i = 0; i < 24; ++i) mv[i] = ((i % 4) == (i / 4) % 3 ? 1.0f : 0.0f) + rf(-0.1f, 0.1f);
    U4("u_mv", 6, mv);
    std::vector<float> tm(96), ptm(96), pal(96 * 4), lt(44 * 4), tev(28 * 4);
    for (float& f : tm) f = rf(-6.0f, 6.0f);
    for (float& f : ptm) f = rf(-1.5f, 1.5f);
    for (int row = 0; row < 96; ++row)
        for (int c = 0; c < 4; ++c) pal[row * 4 + c] = ((row % 3) == c ? 1.0f : 0.0f) + rf(-0.15f, 0.15f);
    for (float& f : lt) f = rf(0.0f, 1.0f);
    for (float& f : tev) f = rf(0.0f, 1.0f);
    for (int i = 11 * 4; i < 17 * 4; ++i) tev[i] = rf(-0.02f, 0.02f);          // indirect matrices (texel offsets of a few texels)
    for (int i = 0; i < 4; ++i) { tev[(17 + i) * 4 + 0] = sc[i][0]; tev[(17 + i) * 4 + 1] = sc[i][1]; } // the scales
    for (int i = 21 * 4; i < 25 * 4; ++i) tev[i] = static_cast<float>(kTexSize); // texture sizes
    tev[8 * 4 + 0] = static_cast<float>(rng() % 256); tev[8 * 4 + 1] = static_cast<float>(rng() % 256);
    U4("u_tm", vs.nTm, tm);
    U4("u_ptm", vs.nPtm, ptm);
    U4("u_pal", vs.nPal, pal);
    U4("u_lt", vs.nLt, lt);
    U4("u_tev", nTev, tev);
    if (GLint l = glGetUniformLocation(prog, "u_s"); l >= 0 && probeStage >= 0) glUniform2f(l, sc[probeStage][0], sc[probeStage][1]);
    if (GLint l = glGetUniformLocation(prog, "u_k"); l >= 0) glUniform1f(l, probeK);
    static GLuint texNames[4] = {};
    if (!texNames[0]) glGenTextures(4, texNames);
    for (int t = 0; t < 4; ++t) {
        std::vector<uint8_t> texel(kTexSize * kTexSize * 4);
        for (uint8_t& b : texel) b = static_cast<uint8_t>(rng());
        glActiveTexture(GL_TEXTURE0 + t);
        glBindTexture(GL_TEXTURE_2D, texNames[t]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kTexSize, kTexSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        char name[16];
        std::snprintf(name, sizeof name, "u_tex%d", t);
        if (GLint l = glGetUniformLocation(prog, name); l >= 0) glUniform1i(l, t);
    }
    std::vector<Vtx> v(kTris * 3);
    const int palMax = vs.nPal == 24 ? 9 : vs.nPal == 60 ? 27 : 57;
    for (Vtx& x : v) {
        x.pos[0] = rf(-1.0f, 1.0f); x.pos[1] = rf(-1.0f, 1.0f); x.pos[2] = rf(-0.8f, 0.8f);
        x.pos[3] = static_cast<float>(3 * (rng() % (palMax / 3 + 1)));
        for (float& f : x.nrm) f = rf(-1.0f, 1.0f);
        for (float& f : x.c0) f = rf(0.0f, 1.0f);
        for (float& f : x.c1) f = rf(0.0f, 1.0f);
        for (auto& t : x.tex) { t[0] = rf(-40.0f, 40.0f); t[1] = rf(-40.0f, 40.0f); }
        for (float& f : x.tmi0) f = static_cast<float>(3 * (rng() % 20));
        for (float& f : x.tmi1) f = static_cast<float>(3 * (rng() % 20));
    }
    auto Attr = [&](const char* name, int n, const float* first) {
        const GLint loc = glGetAttribLocation(prog, name);
        if (loc < 0) return;
        glEnableVertexAttribArray(static_cast<GLuint>(loc));
        glVertexAttribPointer(static_cast<GLuint>(loc), n, GL_FLOAT, GL_FALSE, sizeof(Vtx), first);
    };
    for (GLuint i = 0; i < 16; ++i) glDisableVertexAttribArray(i);
    Attr("a_pos", 4, v[0].pos); Attr("a_nrm", 3, v[0].nrm); Attr("a_col0", 4, v[0].c0); Attr("a_col1", 4, v[0].c1);
    for (int t = 0; t < 8; ++t) {
        char name[16];
        std::snprintf(name, sizeof name, "a_tex%d", t);
        Attr(name, 2, v[0].tex[t]);
    }
    Attr("a_tmi0", 4, v[0].tmi0); Attr("a_tmi1", 4, v[0].tmi1);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, kTris * 3);
    px.assign(kW * kH * 4, 0);
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    Require(glGetError() == GL_NO_ERROR, "GL status");
    return Fnv(14695981039346656037ull, px.data(), px.size());
}
// The probe's fragment shader: the lookup coordinate of texgen c as the unmarked shader forms it (marked false: v_texN.xy * u_s; the
// varying a vec3 or vec2 as the unmarked vertex shader declares it) or as the marked one reads it (v_texN), as bits.
[[maybe_unused]] std::string ProbeFs(int c, bool marked, bool vec2Off) {
    char b[1024];
    std::snprintf(b, sizeof b,
                  "precision highp float;\nvarying %s v_tex%d;\nuniform vec2 u_s;\nuniform float u_k;\nvoid main() {\n"
                  "  vec2 c = %s;\n  vec2 f = fract(c * u_k);\n  vec2 g = fract(c * u_k * 256.0);\n  gl_FragColor = vec4(f, g);\n}\n",
                  marked || vec2Off ? "vec2" : "vec3", c, marked ? ("v_tex" + std::to_string(c)).c_str() : ("v_tex" + std::to_string(c) + ".xy * u_s").c_str());
    return b;
}
} // namespace

int main(int argc, char** argv) {
    const unsigned pairs = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 600;
    Require(vgx::host::CreateContext(), "Mesa GLES context");
    GLuint fbo, tex;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "framebuffer");
    glViewport(0, 0, kW, kH);
    fshalf_keys::Rng r(0x1D5EA);
    uint64_t offDigest = 14695981039346656037ull;
    [[maybe_unused]] unsigned marked = 0, unmarked = 0, probes = 0, ctlProbeSeen = 0, ctlProbeRuns = 0, ctlPicSeen = 0, ctlPicRuns = 0;
    [[maybe_unused]] uint64_t pixels = 0;
    [[maybe_unused]] bool seaMarked = false;
    for (unsigned i = 0; i < pairs; ++i) {
        const bool sea = i == 0;
        vgx::FsKey fs = sea ? SeaKey() : RandomIndKey(r);
        vgx::VsKey vs = sea ? SeaVs() : RandomVs(r, fs);
        uint32_t bp25 = 0, bp26 = 0;
        float sc[4][2];
        RandomScales(r, fs, bp25, bp26, sc);
        if (sea) { bp25 = 0x33; sc[0][0] = sc[0][1] = 0.125f; } // (a typical scale: GX_ITS_8)
        // the pair as the front end makes it (PairKeys): TexVec2Pair, [IndVsPair], VsOutPrune
        vgx::FsKey fOff = fs;
        vgx::VsKey vOffK = vs;
        vgx::TexVec2Pair(fOff, vOffK);
        [[maybe_unused]] vgx::FsKey fOn = fOff;
        [[maybe_unused]] vgx::VsKey vOnK = vOffK;
        if (vgx::VsOutMode()) vgx::VsOutPrune(vOffK, fOff, false);
        vgx::ShaderSource vOff, fOffS;
        vgx::GenerateVertexShader(vOffK, vOff);
        vgx::GenerateFragmentShader(fOff, fOffS);
        offDigest = Fnv(offDigest, vOff.text.data(), vOff.text.size());
        offDigest = Fnv(offDigest, fOffS.text.data(), fOffS.text.size());
#ifndef INDVS_REF
        const bool any = vgx::IndVsPair(fOn, vOnK, bp25, bp26);
        if (vgx::VsOutMode()) vgx::VsOutPrune(vOnK, fOn, false);
        if (sea) seaMarked = any;
        if (!any) {
            Require(std::memcmp(&fOn, &fOff, sizeof fOn) == 0 && std::memcmp(&vOnK, &vOffK, sizeof vOnK) == 0, "unmarked keys unchanged");
            ++unmarked;
            continue;
        }
        ++marked;
        vgx::ShaderSource vOn, fOnS;
        vgx::GenerateVertexShader(vOnK, vOn);
        vgx::GenerateFragmentShader(fOn, fOnS);
        if (sea && std::getenv("VGX_TEST_VERBOSE")) std::printf("---- sea vs (marked)\n%s\n---- sea fs (marked)\n%s\n", vOn.text.c_str(), fOnS.text.c_str());
        for (int j = 0; j < fOn.numIndStages; ++j)
            if (fOn.indVs(j)) {
                const std::string want = "texture2D(u_tex" + std::to_string(fOn.ind[j].texMap) + ", v_tex" + std::to_string(fOn.ind[j].texCoord) + ").abg";
                Require(fOnS.text.find(want) != std::string::npos, "a marked lookup reads the varying as it is");
            }
        // pictures
        {
            const GLuint f0 = Compile(GL_FRAGMENT_SHADER, fOffS.text), f1 = Compile(GL_FRAGMENT_SHADER, fOnS.text);
            const GLuint v0 = Compile(GL_VERTEX_SHADER, vOff.text), v1 = Compile(GL_VERTEX_SHADER, vOn.text);
            const GLuint p0 = Link(v0, f0, vOff.text, fOffS.text), p1 = Link(v1, f1, vOn.text, fOnS.text);
            for (uint32_t round = 0; round < 3; ++round) {
                std::vector<uint8_t> a, b;
                const uint32_t seed = i * 7919u + round * 104729u + 17u;
                const uint64_t ha = Draw(p0, vOff, fOffS.nTev, seed, sc, a), hb = Draw(p1, vOn, fOnS.nTev, seed, sc, b);
                if (ha != hb || a != b) {
                    size_t bad = 0;
                    for (size_t j = 0; j < a.size(); ++j) bad += a[j] != b[j];
                    std::fprintf(stderr, "pair %u (round %u): %zu bytes differ\n---- vs off\n%s\n---- vs on\n%s\n---- fs off\n%s\n---- fs on\n%s\n", i, round, bad,
                                 vOff.text.c_str(), vOn.text.c_str(), fOffS.text.c_str(), fOnS.text.c_str());
                    Require(false, "pictures equal with and without the marks");
                }
                pixels += kW * kH;
            }
            glDeleteProgram(p0); glDeleteProgram(p1);
            glDeleteShader(v0); glDeleteShader(v1); glDeleteShader(f0); glDeleteShader(f1);
        }
        // coordinate probe (and its control) for each marked texgen
        for (int c = 0; c < 6; ++c) {
            if (!vOnK.indScaled(c)) continue;
            int stage = -1;
            for (int j = 0; j < fOn.numIndStages; ++j)
                if (fOn.indVs(j) && fOn.ind[j].texCoord == c) { stage = j; break; }
            Require(stage >= 0, "a scaled texgen has a marked lookup");
            const bool v2Off = (vOffK.texVec2Mask() >> c) & 1u;
            const std::string pf0 = ProbeFs(c, false, v2Off), pf1 = ProbeFs(c, true, false);
            // control: the marked coordinate one ulp larger
            std::string vCtl = vOn.text;
            const std::string pat = "v_tex" + std::to_string(c) + " = ";
            const size_t at = vCtl.find(pat);
            Require(at != std::string::npos, "the marked texgen is written");
            vCtl.insert(at + pat.size(), "1.0000001 * ");
            const GLuint f0 = Compile(GL_FRAGMENT_SHADER, pf0), f1 = Compile(GL_FRAGMENT_SHADER, pf1);
            const GLuint v0 = Compile(GL_VERTEX_SHADER, vOff.text), v1 = Compile(GL_VERTEX_SHADER, vOn.text), v2 = Compile(GL_VERTEX_SHADER, vCtl);
            const GLuint p0 = Link(v0, f0, vOff.text, pf0), p1 = Link(v1, f1, vOn.text, pf1), p2 = Link(v2, f1, vCtl, pf1);
            bool ctlDiff = false;
            for (uint32_t round = 0; round < 2; ++round)
                for (int k = 0; k <= 16; k += 16) {
                    std::vector<uint8_t> a, b, d;
                    const uint32_t seed = i * 7919u + round * 104729u + 17u;
                    const float kk = std::ldexp(1.0f, k);
                    Draw(p0, vOff, 0, seed, sc, a, stage, kk);
                    Draw(p1, vOn, 0, seed, sc, b, stage, kk);
                    Draw(p2, vOn, 0, seed, sc, d, stage, kk);
                    if (a != b) {
                        size_t bad = 0;
                        for (size_t j = 0; j < a.size(); ++j) bad += a[j] != b[j];
                        std::fprintf(stderr, "pair %u texgen %d probe k=%d: %zu bytes differ\n---- vs off\n%s\n---- vs on\n%s\n", i, c, k, bad, vOff.text.c_str(),
                                     vOn.text.c_str());
                        Require(false, "the interpolated lookup coordinate is the same float");
                    }
                    if (a != d) ctlDiff = true;
                    ++probes;
                }
            ++ctlProbeRuns;
            ctlProbeSeen += ctlDiff ? 1u : 0u;
            // control in the full shader: the 1-ulp coordinate through the real lookup
            {
                const GLuint g0 = Compile(GL_FRAGMENT_SHADER, fOffS.text), g1 = Compile(GL_FRAGMENT_SHADER, fOnS.text);
                const GLuint q0 = Link(v0, g0, vOff.text, fOffS.text), q2 = Link(v2, g1, vCtl, fOnS.text);
                std::vector<uint8_t> a, d;
                const uint32_t seed = i * 7919u + 17u;
                Draw(q0, vOff, fOffS.nTev, seed, sc, a);
                Draw(q2, vOn, fOnS.nTev, seed, sc, d);
                ++ctlPicRuns;
                ctlPicSeen += a != d ? 1u : 0u;
                glDeleteProgram(q0); glDeleteProgram(q2); glDeleteShader(g0); glDeleteShader(g1);
            }
            glDeleteProgram(p0); glDeleteProgram(p1); glDeleteProgram(p2);
            glDeleteShader(v0); glDeleteShader(v1); glDeleteShader(v2); glDeleteShader(f0); glDeleteShader(f1);
        }
#endif
    }
    std::printf("off-digest=%016llx pairs=%u\n", static_cast<unsigned long long>(offDigest), pairs);
#ifndef INDVS_REF
    Require(seaMarked, "the sea's lookup is marked");
    Require(ctlProbeSeen > 0, "the probe sees a one-ulp coordinate change");
    std::printf("GXV_IND_VS=%ld GXV_TEXVEC2=%ld GXV_VSOUT=%ld: %u pairs marked (the sea among them), %u not; %llu pixels compared, all equal; "
                "%u coordinate probes, all the same float; control (+1 ulp): probe differs in %u of %u texgens, picture in %u of %u\n",
                vgx::IndVsMode(), vgx::TexVec2Mode(), vgx::VsOutMode(), marked, unmarked, static_cast<unsigned long long>(pixels), probes, ctlProbeSeen,
                ctlProbeRuns, ctlPicSeen, ctlPicRuns);
#endif
    return 0;
}
