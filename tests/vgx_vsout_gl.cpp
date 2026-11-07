// Host-side test of GXV_VSOUT (platform/vgx/src/vgx_shadergen.cpp VsOutPrune / GenerateVertexShader): a vertex shader leaves out
// the colour varyings, the colour / alpha parts and the texgens its fragment shader never reads. Real generated GLES2 shaders on
// Mesa (pixel oracle); no game data, no Vita executable.
//   tools/test_vgx_vsout_gl.sh [output-dir]
//
// Also GXV_TEXVEC2 (TexVec2Pair: the non-projective texture coordinate varyings as vec2 in both shaders), alone and with GXV_VSOUT.
// For random pairs of fragment keys (the half precision tests' generator of J3D-shaped TEV keys, every shape) and vertex keys (lit
// channels, palettes, texgens of every source, post matrices, colour texgens, vertex fog), with the folding fragment variant where
// the pair can fold, the binary draws random triangles twice: with the vertex shader of the key as the front end makes it with the
// switch off, and with the key VsOutPrune marked. The pictures (RGBA8 readback, same uniforms, textures, vertices) must be equal
// bit for bit; a pruned key whose marks are all zero must give the very same text. It prints a digest of the switch-off texts:
// built with -DVSOUT_REF against the tree before the switch existed, it prints the same digest (the runner compares them).
#include "vgx_shadergen.h"
#include "vgx_fshalf_keys.h"
#include <GLES2/gl2.h>
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
// The front end's FoldKeysOk (vgx_draw.cpp), copied: the pair can draw with the folding fragment variant.
[[maybe_unused]] bool FoldKeysOk(const vgx::FsKey& fs, const vgx::VsKey& vs) {
    if (vs.numChans != 0) return false;
    for (int i = 0; i < vs.numTexGens; ++i)
        if (vs.tg[i].type >= 2) return false;
    bool reads = false;
    for (int s = 0; s < fs.numStages; ++s) {
        const vgx::FsKey::Stage& st = fs.stage[s];
        if (st.rasChan != 7) return false;
        for (int i = 0; i < 4; ++i) {
            const uint32_t c = (st.color >> (i * 4)) & 15u;
            if (c >= 2 && c <= 5) reads = true;
            const uint32_t a = (st.alpha >> (4 + i * 3)) & 7u;
            if (a == 1 || a == 2) reads = true;
        }
    }
    return reads;
}
// A vertex key for the fragment key (as BuildVsKey would pair them: the same texgen count, the fog flag, projective texgens).
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
        c.enable = r.P(80) ? 1 : 0;
        if (!c.enable) continue;
        c.ambSrcVtx = r.P(20) ? 1 : 0;
        c.lightMask = static_cast<uint8_t>(r.P(15) ? 0 : r.P(70) ? 1u + r.U(7) : r.U(256));
        if (c.lightMask) {
            c.diffuse = static_cast<uint8_t>(r.U(3));
            c.attn = static_cast<uint8_t>(r.U(3));
        }
    }
    if (k.numChans && r.P(40)) { k.chan[2 + 0] = k.chan[0]; } // colour and alpha alike (GXV_LTMERGE's case)
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

struct Vtx { float pos[4], nrm[3], c0[4], c1[4], tex[8][2], tmi0[4], tmi1[4]; };
constexpr int kW = 48, kH = 48, kTris = 24;

// Draws the scene with one program; the same seed gives the same uniforms, textures and vertices for both programs of a pair.
[[maybe_unused]] uint64_t Draw(GLuint prog, const vgx::ShaderSource& vs, const vgx::ShaderSource& fs, uint32_t seed, std::vector<uint8_t>& px) {
    std::mt19937 rng(seed);
    auto rf = [&](float lo, float hi) { return lo + (hi - lo) * static_cast<float>(rng() / 4294967296.0); };
    glUseProgram(prog);
    auto U4 = [&](const char* name, int n, const std::vector<float>& v) {
        const GLint loc = glGetUniformLocation(prog, name);
        if (loc >= 0 && n > 0) glUniform4fv(loc, n, v.data());
    };
    // (values are drawn whether or not the program has the uniform: both programs of a pair see the same sequence)
    std::vector<float> proj = {0.9f, 0.05f, 0.0f, 0.0f, -0.04f, 0.85f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.02f, -0.03f, 0.0f, 1.0f};
    for (float& f : proj) f += rf(-0.02f, 0.02f);
    if (GLint l = glGetUniformLocation(prog, "u_proj"); l >= 0) glUniformMatrix4fv(l, 1, GL_FALSE, proj.data());
    std::vector<float> mv(24);
    for (int i = 0; i < 24; ++i) mv[i] = ((i % 4) == (i / 4) % 3 ? 1.0f : 0.0f) + rf(-0.1f, 0.1f);
    U4("u_mv", 6, mv);
    std::vector<float> tm(96), ptm(96), pal(96 * 4), lt(44 * 4), tev(28 * 4);
    for (float& f : tm) f = rf(-1.5f, 1.5f);
    for (float& f : ptm) f = rf(-1.5f, 1.5f);
    for (int row = 0; row < 96; ++row)
        for (int c = 0; c < 4; ++c) pal[row * 4 + c] = ((row % 3) == c ? 1.0f : 0.0f) + rf(-0.15f, 0.15f);
    for (float& f : lt) f = rf(-2.0f, 2.0f);
    for (int i = 0; i < 16; ++i) lt[i] = rf(0.0f, 1.0f);
    for (int l = 4; l + 4 < 44; l += 5) {
        for (int c = 0; c < 4; ++c) lt[(l + 2) * 4 + c] = rf(0.0f, 1.0f);
        for (int c = 0; c < 3; ++c) lt[(l + 3) * 4 + c] = rf(0.1f, 1.0f);
        for (int c = 0; c < 3; ++c) lt[(l + 4) * 4 + c] = rf(0.1f, 1.0f);
    }
    for (float& f : tev) f = rf(0.0f, 1.0f);
    for (int i = 21; i < 25; ++i)
        for (int c = 0; c < 4; ++c) tev[i * 4 + c] = 8.0f; // (texture sizes of the indirect stages)
    tev[8 * 4 + 0] = static_cast<float>(rng() % 256); tev[8 * 4 + 1] = static_cast<float>(rng() % 256); // (alpha-test references)
    U4("u_tm", vs.nTm, tm);
    U4("u_ptm", vs.nPtm, ptm);
    U4("u_pal", vs.nPal, pal);
    U4("u_lt", vs.nLt, lt);
    U4("u_tev", fs.nTev, tev);
    static GLuint texNames[4] = {};
    if (!texNames[0]) glGenTextures(4, texNames);
    for (int t = 0; t < 4; ++t) {
        uint8_t texel[8 * 8 * 4];
        for (uint8_t& b : texel) b = static_cast<uint8_t>(rng());
        glActiveTexture(GL_TEXTURE0 + t);
        glBindTexture(GL_TEXTURE_2D, texNames[t]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
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
        for (auto& t : x.tex) { t[0] = rf(-2.0f, 2.0f); t[1] = rf(-2.0f, 2.0f); }
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
[[maybe_unused]] int Count(const std::string& s, const char* what) {
    int n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
    return n;
}
} // namespace

int main(int argc, char** argv) {
    const unsigned pairs = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 1500;
    Require(vgx::host::CreateContext(), "Mesa GLES context");
    GLuint fbo, tex;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "framebuffer");
    glViewport(0, 0, kW, kH);
    fshalf_keys::Rng r(0x75007);
    uint64_t offDigest = 14695981039346656037ull;
#ifdef VSOUT_REF
    (void)fbo;
#else
    unsigned changed = 0, same = 0, folded = 0, colGone = 0, partOff = 0, texOff = 0, attrDrop = 0, vec2s = 0, pixelsCompared = 0;
#endif
    for (unsigned i = 0; i < pairs; ++i) {
        const vgx::FsKey fs = fshalf_keys::RandomKey(r, r.P(30));
        const vgx::VsKey vs = RandomVs(r, fs);
        vgx::ShaderSource vOff;
        vgx::GenerateVertexShader(vs, vOff);
        offDigest = Fnv(offDigest, vOff.text.data(), vOff.text.size());
        {
            vgx::ShaderSource fOff0;
            vgx::GenerateFragmentShader(fs, fOff0);
            offDigest = Fnv(offDigest, fOff0.text.data(), fOff0.text.size());
        }
        const bool keepAnyway = r.P(5); // (drawn in every build: the reference build sees the same keys)
#ifdef VSOUT_REF
        (void)keepAnyway;
#endif
#ifndef VSOUT_REF
        const bool canFold = FoldKeysOk(fs, vs);
        const bool keep = canFold || keepAnyway;
        // the pair as the front end makes it with the switches of the environment (PairKeys in vgx_draw.cpp)
        vgx::FsKey fOn = fs;
        vgx::VsKey pk = vs;
        vgx::TexVec2Pair(fOn, pk);
        if (vgx::VsOutMode()) vgx::VsOutPrune(pk, fOn, keep);
        vgx::ShaderSource vOn;
        vgx::GenerateVertexShader(pk, vOn);
        const bool marked = std::memcmp(&pk, &vs, sizeof vs) != 0 || std::memcmp(&fOn, &fs, sizeof fs) != 0;
        Require(marked || vOn.text == vOff.text, "a key with nothing marked keeps its text");
        if (!marked) { ++same; continue; }
        ++changed;
        colGone += (vOn.outPruned & 1u) + ((vOn.outPruned >> 1) & 1u);
        partOff += (vOn.outPruned >> 2) & 1u;
        texOff += (vOn.outPruned >> 3) & 1u;
        vec2s += static_cast<unsigned>(Count(vOn.text, "varying vec2 v_tex"));
        attrDrop += static_cast<unsigned>(Count(vOff.text, "attribute ") - Count(vOn.text, "attribute "));
        // the fragment shaders the pair can be drawn with: the key's own, and its folding variant when the keys allow it
        std::vector<std::pair<vgx::FsKey, vgx::FsKey>> fsVariants{{fs, fOn}};
        if (canFold) {
            vgx::FsKey a = fs, b = fOn;
            a.regFromVtx = 3; b.regFromVtx = 3;
            fsVariants.push_back({a, b});
            ++folded;
        }
        for (const auto& f : fsVariants) {
            vgx::ShaderSource fOff, fsOn;
            vgx::GenerateFragmentShader(f.first, fOff);
            vgx::GenerateFragmentShader(f.second, fsOn);
            Require(f.second.texVec2() || fsOn.text == fOff.text, "an unmarked fragment key keeps its text");
            const GLuint f0 = Compile(GL_FRAGMENT_SHADER, fOff.text), f1 = Compile(GL_FRAGMENT_SHADER, fsOn.text);
            const GLuint v0 = Compile(GL_VERTEX_SHADER, vOff.text), v1 = Compile(GL_VERTEX_SHADER, vOn.text);
            const GLuint p0 = Link(v0, f0, vOff.text, fOff.text), p1 = Link(v1, f1, vOn.text, fsOn.text);
            for (uint32_t round = 0; round < 3; ++round) {
                std::vector<uint8_t> a, b;
                const uint32_t seed = i * 7919u + round * 104729u + 17u;
                const uint64_t ha = Draw(p0, vOff, fOff, seed, a), hb = Draw(p1, vOn, fsOn, seed, b);
                if (ha != hb || a != b) {
                    size_t bad = 0;
                    for (size_t j = 0; j < a.size(); ++j) bad += a[j] != b[j];
                    std::fprintf(stderr, "pair %u (round %u): %zu bytes differ\n---- vs off\n%s\n---- vs on\n%s\n---- fs off\n%s\n---- fs on\n%s\n", i, round, bad,
                                 vOff.text.c_str(), vOn.text.c_str(), fOff.text.c_str(), fsOn.text.c_str());
                    Require(false, "pictures equal with and without the marks");
                }
                pixelsCompared += kW * kH;
            }
            glDeleteProgram(p0); glDeleteProgram(p1);
            glDeleteShader(v0); glDeleteShader(v1); glDeleteShader(f0); glDeleteShader(f1);
        }
#endif
    }
    std::printf("off-digest=%016llx pairs=%u\n", static_cast<unsigned long long>(offDigest), pairs);
#ifndef VSOUT_REF
    std::printf("GXV_VSOUT=%ld GXV_TEXVEC2=%ld: %u pairs marked (%u with nothing to change), %u with the folding variant too; colour varyings left out %u, "
                "parts not computed %u, keys with a constant texgen %u, attribute declarations dropped %u, vec2 texture varyings %u; %u pixels compared, all equal\n",
                vgx::VsOutMode(), vgx::TexVec2Mode(), changed, same, folded, colGone, partOff, texOff, attrDrop, vec2s, pixelsCompared);
#endif
    return 0;
}
