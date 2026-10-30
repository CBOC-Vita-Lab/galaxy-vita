// Host-side test of GXV_LTMERGE (platform/vgx/src/vgx_shadergen.cpp): a lit channel whose colour and alpha parts have the same
// light mask, attenuation and diffuse function runs each light once for a vec4 instead of twice. Real generated GLES2 vertex
// shaders on Mesa; no game data, no Vita executable.
//   tools/test_vgx_ltmerge_gl.sh [output-dir]
//
// The binary reads the switch from its environment (the runner starts it with each value) and writes, for a fixed set of vertex keys,
// the digest of the generated sources and of the vertex colours the shaders produce (v_col0 and v_col1, 8-bit readback of many random
// vertices). The runner compares the two runs:
//   * switch off: every source is the one the generator made before the switch (a digest made from the commit before);
//   * switch on: the keys with equal colour / alpha light control have other (shorter) text, all others the same text, and every
//     vertex colour read back is equal to the switch-off one.
#include "vgx_shadergen.h"
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
GLuint Compile(GLenum type, const std::string& text) {
    GLuint s = glCreateShader(type);
    const char* p = text.c_str(); glShaderSource(s, 1, &p, nullptr); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096] = {}; glGetShaderInfoLog(s, sizeof log, nullptr, log); std::fprintf(stderr, "%s\n%s\n", log, p); }
    Require(ok, "generated shader compiles"); return s;
}
// The keys: colour channel 0 / alpha channel 0 (and, with two channels, 1) over light masks, attenuation, diffuse and sources.
std::vector<vgx::VsKey> Keys() {
    std::vector<vgx::VsKey> v;
    const uint8_t masks[] = {0x01, 0x03, 0x81, 0xFF};
    for (int chans = 1; chans <= 2; ++chans)
        for (uint8_t mask : masks)
            for (int attn = 0; attn < 3; ++attn)
                for (int diff = 0; diff < 3; ++diff)
                    for (int variant = 0; variant < 4; ++variant) { // 0 alike, 1 other alpha mask, 2 other sources, 3 alpha off
                        vgx::VsKey k;
                        k.numChans = static_cast<uint8_t>(chans);
                        for (int ch = 0; ch < chans; ++ch) {
                            vgx::VsKey::Chan c;
                            c.enable = 1; c.lightMask = mask; c.attn = static_cast<uint8_t>(attn); c.diffuse = static_cast<uint8_t>(diff);
                            c.matSrcVtx = static_cast<uint8_t>(ch == 0); c.ambSrcVtx = 0;
                            vgx::VsKey::Chan a = c;
                            if (variant == 1) a.lightMask = static_cast<uint8_t>(mask & 0x0F ? mask & 0x0F : 0x01);
                            if (variant == 1 && a.lightMask == mask) a.lightMask = 0x02;
                            if (variant == 2) { a.matSrcVtx = !c.matSrcVtx; a.ambSrcVtx = 1; }
                            if (variant == 3) a.enable = 0;
                            k.chan[ch] = c;
                            k.chan[ch + 2] = a;
                        }
                        k.numTexGens = 1;
                        k.tg[0].src = 5;
                        v.push_back(k);
                    }
    return v;
}
}

int main() {
    Require(vgx::host::CreateContext(), "Mesa GLES context");
    const bool on = std::getenv("GXV_LTMERGE") && std::atoi(std::getenv("GXV_LTMERGE")) != 0;
    const std::vector<vgx::VsKey> keys = Keys();
    uint64_t srcHash = 14695981039346656037ull, pixHash = 14695981039346656037ull;
    std::vector<uint64_t> perKeySrc, perKeyPix;
    const char* fsText = "precision mediump float;\nvarying vec4 v_col0;\nvarying vec4 v_col1;\nvarying vec3 v_tex0;\nuniform float u_which;\n"
                         "void main() { gl_FragColor = u_which < 0.5 ? v_col0 : v_col1; }\n";
    const GLuint fs = Compile(GL_FRAGMENT_SHADER, fsText);
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "framebuffer");
    glViewport(0, 0, 64, 64);
    std::mt19937 rng(12345);
    auto rf = [&](float lo, float hi) { return lo + (hi - lo) * (rng() / 4294967296.0f); };
    for (const vgx::VsKey& k : keys) {
        vgx::ShaderSource vs;
        vgx::GenerateVertexShader(k, vs);
        perKeySrc.push_back(Fnv(14695981039346656037ull, vs.text.data(), vs.text.size()));
        srcHash = Fnv(srcHash, vs.text.data(), vs.text.size());
        const GLuint v = Compile(GL_VERTEX_SHADER, vs.text);
        const GLuint p = glCreateProgram();
        glAttachShader(p, v); glAttachShader(p, fs);
        glBindAttribLocation(p, vgx::AT_POS, "a_pos"); glBindAttribLocation(p, vgx::AT_NRM, "a_nrm");
        glBindAttribLocation(p, vgx::AT_COL0, "a_col0"); glBindAttribLocation(p, vgx::AT_COL1, "a_col1");
        glLinkProgram(p);
        GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok); Require(ok, "program links");
        glUseProgram(p);
        float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        glUniformMatrix4fv(glGetUniformLocation(p, "u_proj"), 1, GL_FALSE, proj);
        // 64 points, one pixel each: the position (x, y) is the pixel, eye z random; normals, colours random.
        uint64_t keyPix = 14695981039346656037ull;
        for (int round = 0; round < 6; ++round) {
            float mv[24] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            glUniform4fv(glGetUniformLocation(p, "u_mv"), 6, mv);
            const GLint lt = glGetUniformLocation(p, "u_lt");
            std::vector<float> ltv(static_cast<size_t>(vgx::VsLightCount(k)) * 4);
            for (float& f : ltv) f = rf(-2.0f, 2.0f);
            for (int i = 0; i < 4 * 4; ++i) ltv[i] = rf(0.0f, 1.0f); // ambient / material registers
            for (size_t l = 4; l * 4 + 3 < ltv.size(); l += 5) { // light colour in [0,1], cos attenuation coefficients positive, distance attenuation > 0
                for (int c = 0; c < 4; ++c) ltv[(l + 2) * 4 + c] = rf(0.0f, 1.0f);
                for (int c = 0; c < 3; ++c) ltv[(l + 3) * 4 + c] = rf(0.1f, 1.0f);
                for (int c = 0; c < 3; ++c) ltv[(l + 4) * 4 + c] = rf(0.1f, 1.0f);
                for (int c = 0; c < 3; ++c) ltv[(l + 1) * 4 + c] = rf(-1.0f, 1.0f);
            }
            glUniform4fv(lt, static_cast<GLsizei>(ltv.size() / 4), ltv.data());
            struct V { float pos[4], nrm[3], c0[4], c1[4]; } vtx[64];
            for (int i = 0; i < 64; ++i) {
                V& x = vtx[i];
                x.pos[0] = (i % 8 + 0.5f) / 4.0f - 1.0f; x.pos[1] = (i / 8 + 0.5f) / 4.0f - 1.0f; x.pos[2] = rf(-3.0f, 3.0f); x.pos[3] = 1.0f;
                for (float& f : x.nrm) f = rf(-1.0f, 1.0f);
                for (float& f : x.c0) f = rf(0.0f, 1.0f);
                for (float& f : x.c1) f = rf(0.0f, 1.0f);
            }
            glEnableVertexAttribArray(vgx::AT_POS); glEnableVertexAttribArray(vgx::AT_NRM);
            glEnableVertexAttribArray(vgx::AT_COL0); glEnableVertexAttribArray(vgx::AT_COL1);
            glVertexAttribPointer(vgx::AT_POS, 4, GL_FLOAT, GL_FALSE, sizeof(V), vtx[0].pos);
            glVertexAttribPointer(vgx::AT_NRM, 3, GL_FLOAT, GL_FALSE, sizeof(V), vtx[0].nrm);
            glVertexAttribPointer(vgx::AT_COL0, 4, GL_FLOAT, GL_FALSE, sizeof(V), vtx[0].c0);
            glVertexAttribPointer(vgx::AT_COL1, 4, GL_FLOAT, GL_FALSE, sizeof(V), vtx[0].c1);
            glDisable(GL_BLEND);
            for (int which = 0; which < 2; ++which) {
                glUniform1f(glGetUniformLocation(p, "u_which"), which);
                glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
                glDrawArrays(GL_POINTS, 0, 64);
                std::vector<uint8_t> px(64 * 64 * 4);
                glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                keyPix = Fnv(keyPix, px.data(), px.size());
            }
        }
        Require(glGetError() == GL_NO_ERROR, "GL status");
        perKeyPix.push_back(keyPix);
        pixHash = Fnv(pixHash, &keyPix, sizeof keyPix);
        glDeleteProgram(p); glDeleteShader(v);
    }
    std::printf("ltmerge=%d keys=%zu src=%016llx pix=%016llx\n", on ? 1 : 0, keys.size(), static_cast<unsigned long long>(srcHash), static_cast<unsigned long long>(pixHash));
    for (size_t i = 0; i < keys.size(); ++i)
        std::printf("key %zu src=%016llx pix=%016llx\n", i, static_cast<unsigned long long>(perKeySrc[i]), static_cast<unsigned long long>(perKeyPix[i]));
    return 0;
}
