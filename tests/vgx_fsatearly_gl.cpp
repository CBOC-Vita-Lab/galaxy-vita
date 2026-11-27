// fsatearly25 (v2): GXV_FS_ATEARLY=1 early alpha + discard BEFORE colour stages and fog,
// with ALL texture reads (and indirect lookups / projective divides) kept BEFORE `discard;`
// so zero implicit-LOD texture fetches ever execute after a kill on SGX543/GXM.
// Real generated GLES2 shaders rendered against GXV_FS_ATEARLY=0 with:
//   * Mip-mapped textures (levels 0..5 with distinct per-level colours);
//   * Distinct non-repeating floats in every canonical u_tev[0..31] row/component;
//   * Structural assertions: ZERO `texture2D`/`texture2DProj` after `discard;`, `discard;` before
//     phase-2 colour and fog, `// stage ` count == numStages;
//   * LeakKey (cross-stage alpha overwrite snapshots), indirect + lodBias + proj + dualWrap +
//     kFogHalfBit + alpha compare (R8/A8) + 150 compiled / 40 rendered random cutout keys -> 0/255.
#include "vgx_shadergen.h"
#include "vgx_silhfuse.h"
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

namespace vgx {
bool LogEnabled() { return false; }
namespace host { bool CreateContext(); }
}

namespace {
constexpr int kW = 64, kH = 64;

void Require(bool ok, const char* why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", why);
        std::exit(1);
    }
}

uint32_t PackC(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t bias = 0, uint32_t op = 0, uint32_t clamp = 1, uint32_t scale = 0, uint32_t dst = 0) {
    return (a & 15u) | ((b & 15u) << 4) | ((c & 15u) << 8) | ((d & 15u) << 12) | ((bias & 3u) << 16) | ((op & 1u) << 18) |
           ((clamp & 1u) << 19) | ((scale & 3u) << 20) | ((dst & 3u) << 22);
}
uint32_t PackA(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t bias = 0, uint32_t op = 0, uint32_t clamp = 1, uint32_t scale = 0, uint32_t dst = 0) {
    return ((a & 7u) << 4) | ((b & 7u) << 7) | ((c & 7u) << 10) | ((d & 7u) << 13) | ((bias & 3u) << 16) | ((op & 1u) << 18) |
           ((clamp & 1u) << 19) | ((scale & 3u) << 20) | ((dst & 3u) << 22);
}
uint32_t PackInd(uint32_t bt, uint32_t fmt, uint32_t bias, uint32_t mid, uint32_t sw = 0, uint32_t tw = 0, uint32_t addPrev = 0) {
    return (bt & 3u) | ((fmt & 3u) << 2) | ((bias & 7u) << 4) | ((mid & 15u) << 9) | ((sw & 7u) << 13) | ((tw & 7u) << 16) | ((addPrev & 1u) << 20);
}

GLuint Compile(GLenum type, const std::string& text) {
    GLuint s = glCreateShader(type);
    const char* p = text.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "compile failed:\n%s\n--- shader ---\n%s\n", log, p);
        std::exit(1);
    }
    return s;
}

std::string MakeVs(const vgx::FsKey& fk, const std::string& fsText) {
    std::string s = "attribute vec4 a_pos;\nattribute vec4 a_col0;\nattribute vec4 a_col1;\n";
    if (fsText.find("varying vec4 v_col0;") != std::string::npos || fsText.find("varying hvec4 v_col0;") != std::string::npos)
        s += "varying vec4 v_col0;\n";
    if (fsText.find("varying vec4 v_col1;") != std::string::npos || fsText.find("varying hvec4 v_col1;") != std::string::npos)
        s += "varying vec4 v_col1;\n";
    for (int i = 0; i < fk.numTexGens; ++i) {
        const std::string v2 = "varying vec2 v_tex" + std::to_string(i) + ";";
        if (fsText.find(v2) != std::string::npos) s += "varying vec2 v_tex" + std::to_string(i) + ";\n";
        else s += "varying vec3 v_tex" + std::to_string(i) + ";\n";
    }
    std::string fogName;
    const size_t at = fsText.find("varying float v_fog");
    if (at != std::string::npos) {
        fogName = fsText.substr(at + 14, fsText.find(';', at) - at - 14);
        s += "varying float " + fogName + ";\n";
    }
    s += "void main() {\n  gl_Position = vec4(a_pos.xy, a_pos.z, 1.0);\n";
    if (s.find("varying vec4 v_col0;") != std::string::npos) s += "  v_col0 = a_col0;\n";
    if (s.find("varying vec4 v_col1;") != std::string::npos) s += "  v_col1 = a_col1;\n";
    for (int i = 0; i < fk.numTexGens; ++i) {
        const float ox = 0.06f * (i + 1), oy = 0.04f * (i + 1);
        const bool isV2 = s.find("varying vec2 v_tex" + std::to_string(i) + ";") != std::string::npos;
        char line[256];
        if (isV2) {
            std::snprintf(line, sizeof line, "  v_tex%d = (a_pos.xy * 0.5 + vec2(0.5)) * vec2(%.3f, %.3f) + vec2(%.3f, %.3f);\n",
                          i, 1.0f + 0.11f * i, 1.0f + 0.08f * i, ox, oy);
        } else if ((fk.texProjMask >> i) & 1u) {
            std::snprintf(line, sizeof line, "  float q%d = 1.2 + 0.3 * a_pos.x + 0.25 * a_pos.y;\n  v_tex%d = vec3(((a_pos.xy * 0.5 + vec2(0.5)) + vec2(%.3f, %.3f)) * q%d, q%d);\n",
                          i, i, ox, oy, i, i);
        } else {
            std::snprintf(line, sizeof line, "  v_tex%d = vec3((a_pos.xy * 0.5 + vec2(0.5)) * vec2(%.3f, %.3f) + vec2(%.3f, %.3f), 1.0);\n",
                          i, 1.0f + 0.11f * i, 1.0f + 0.08f * i, ox, oy);
        }
        s += line;
    }
    if (!fogName.empty()) s += "  " + fogName + " = 20.0 + 280.0 * (a_pos.y * 0.5 + 0.5);\n";
    s += "}\n";
    return s;
}

GLuint Link(const std::string& vtx, const std::string& frag) {
    GLuint vs = Compile(GL_VERTEX_SHADER, vtx);
    GLuint fs = Compile(GL_FRAGMENT_SHADER, frag);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_col0");
    glBindAttribLocation(p, 2, "a_col1");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        std::fprintf(stderr, "link failed:\n%s\n", log);
        std::exit(1);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

void CreateMipTextures(GLuint tex[8]) {
    glGenTextures(8, tex);
    for (int m = 0; m < 8; ++m) {
        glActiveTexture(GL_TEXTURE0 + m);
        glBindTexture(GL_TEXTURE_2D, tex[m]);
        for (int lvl = 0; lvl <= 5; ++lvl) {
            const int dim = 32 >> lvl;
            std::vector<uint8_t> px(static_cast<size_t>(dim * dim * 4));
            for (int y = 0; y < dim; ++y) {
                for (int x = 0; x < dim; ++x) {
                    const size_t o = static_cast<size_t>((y * dim + x) * 4);
                    px[o + 0] = static_cast<uint8_t>(((x * (9 + 3 * m) + lvl * 41 + m * 23) & 255));
                    px[o + 1] = static_cast<uint8_t>(((y * (15 + 4 * m) + lvl * 67 + m * 47) & 255));
                    px[o + 2] = static_cast<uint8_t>((((x + y) * (6 + m) + lvl * 89 + m * 19) & 255));
                    px[o + 3] = static_cast<uint8_t>(((x * 8 + y * 4 + lvl * 33 + m * 29) & 255));
                }
            }
            glTexImage2D(GL_TEXTURE_2D, lvl, GL_RGBA, dim, dim, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
}

void BuildDistinctTev(const vgx::FsKey& k, float out[128], uint32_t seed) {
    const int canonBase = vgx::FsTevBase(k);
    std::memset(out, 0, 128 * sizeof(float));
    const float s = static_cast<float>(seed & 7u) * 0.015f;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (k.inWrap & 1) {
                const int raw = ((r & 1) ? 275 : -23) + r * 29 + c * 19 + static_cast<int>(seed & 15u);
                out[r * 4 + c] = ((k.preWrap() >> (c < 3 ? r : r + 4)) & 1u)
                    ? static_cast<float>(raw & 255) / 255.0f
                    : static_cast<float>(raw) / 255.0f;
            } else {
                out[r * 4 + c] = 0.13f + 0.18f * r + 0.041f * c + s * 0.25f;
            }
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 4; ++c)
            out[(4 + i) * 4 + c] = 0.09f + 0.20f * i + 0.039f * c + s * 0.2f;
    out[8 * 4 + 0] = 84.0f + static_cast<float>((seed * 7u) & 31u);
    out[8 * 4 + 1] = 204.0f - static_cast<float>((seed * 5u) & 31u);
    out[8 * 4 + 2] = 0.37f;
    out[8 * 4 + 3] = 0.51f;
    if (vgx::FsAtFastKey(k)) {
        float thr[2] = {};
        vgx::AtFastValues(k.alphaTest, out[8 * 4 + 0], out[8 * 4 + 1], thr);
        out[9 * 4 + 0] = thr[0]; out[9 * 4 + 1] = thr[1];
    } else if (k.fog) {
        const float start = 40.0f, end = 260.0f;
        const float c1 = 1.0f / (end - start), c0 = -start * c1;
        if (k.fog & vgx::kFogVtxBit) { out[9 * 4 + 0] = c1; out[9 * 4 + 3] = c0; }
        else if (k.fog & 8) { out[9 * 4 + 0] = 1.5f; out[9 * 4 + 2] = 0.2f; }
        else { out[9 * 4 + 0] = 1.7f; out[9 * 4 + 1] = 1.4f; out[9 * 4 + 2] = 0.9f; }
        out[10 * 4 + 0] = 0.16f + s; out[10 * 4 + 1] = 0.55f; out[10 * 4 + 2] = 0.85f - s;
        if (k.fog & vgx::kFogHalfBit) {
            const vgx::FogHalfLane l = vgx::FogHalfLanes(c1, c0);
            out[11 * 4 + 0] = l.sInv; out[11 * 4 + 1] = l.c1s; out[11 * 4 + 2] = l.bHi; out[11 * 4 + 3] = l.bLo;
            out[10 * 4 + 3] = l.k;
        }
    }
    if (canonBase >= 28) {
        const int startMtx = (k.fog & vgx::kFogHalfBit) ? 1 : 0;
        for (int m = startMtx; m < 3; ++m) {
            const float sc = 0.05f + 0.03f * m;
            out[(11 + m * 2) * 4 + 0] = (0.33f + 0.1f * m) * sc;
            out[(11 + m * 2) * 4 + 1] = (-0.21f + 0.08f * m) * sc;
            out[(11 + m * 2) * 4 + 2] = (0.14f + 0.04f * m) * sc;
            out[(11 + m * 2) * 4 + 3] = sc;
            out[(12 + m * 2) * 4 + 0] = (-0.25f - 0.07f * m) * sc;
            out[(12 + m * 2) * 4 + 1] = (0.27f + 0.09f * m) * sc;
            out[(12 + m * 2) * 4 + 2] = (0.15f - 0.03f * m) * sc;
        }
        for (int i = 0; i < 4; ++i) { out[(17 + i) * 4 + 0] = 0.5f + 0.15f * i; out[(17 + i) * 4 + 1] = 0.45f + 0.18f * i; }
        for (int m = 0; m < 8; ++m) { out[21 * 4 + m * 2 + 0] = 24.0f + 8.0f * m; out[21 * 4 + m * 2 + 1] = 28.0f + 6.0f * m; }
        for (int m = 0; m < 8; ++m) out[25 * 4 + m] = 0.4f + 0.45f * m;
        if (k.indPre() && k.indPre() <= k.numStages) {
            const unsigned map = k.stage[k.indPre() - 1].texMap & 7u;
            vgx::IndPreValues(k, &out[11 * 4], static_cast<uint16_t>(out[21 * 4 + map * 2]), static_cast<uint16_t>(out[21 * 4 + map * 2 + 1]), &out[27 * 4]);
        }
    }
    if (k.dualWrap()) {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                const int raw = ((r & 1) ? 275 : -23) + r * 29 + c * 19 + static_cast<int>(seed & 15u);
                out[(canonBase + r) * 4 + c] = ((k.dualWrap() >> r) & 1u) ? static_cast<float>(raw & 255) / 255.0f : 0.0f;
            }
    }
}

std::vector<uint8_t> RenderKey(const vgx::FsKey& k, const vgx::ShaderSource& src, const float tev[128]) {
    const std::string vsText = MakeVs(k, src.text);
    const GLuint prog = Link(vsText, src.text);
    glUseProgram(prog);
    for (int m = 0; m < 8; ++m) {
        char nm[16];
        std::snprintf(nm, sizeof nm, "u_tex%d", m);
        const GLint loc = glGetUniformLocation(prog, nm);
        if (loc >= 0) glUniform1i(loc, m);
    }
    const GLint tevLoc = glGetUniformLocation(prog, "u_tev");
    if (tevLoc >= 0 && src.nTev > 0) {
        if (src.tevGather) {
            float gathered[128] = {};
            for (uint8_t i = 0; i < src.nTev; ++i)
                std::memcpy(&gathered[i * 4], &tev[src.tevSrc[i] * 4], 4 * sizeof(float));
            glUniform4fv(tevLoc, static_cast<GLsizei>(src.nTev), gathered);
        } else {
            glUniform4fv(tevLoc, static_cast<GLsizei>(src.nTev), tev);
        }
    }

    float v[4][12] = {};
    const float nx[4] = {-1, 1, -1, 1}, ny[4] = {-1, -1, 1, 1}, nz[4] = {-0.6f, -0.1f, 0.4f, 0.9f};
    for (int i = 0; i < 4; ++i) {
        v[i][0] = nx[i]; v[i][1] = ny[i]; v[i][2] = nz[i]; v[i][3] = 1.0f;
        v[i][4] = nx[i] > 0 ? 0.92f : 0.14f;
        v[i][5] = ny[i] > 0 ? 0.84f : 0.21f;
        v[i][6] = 0.45f + 0.2f * nx[i];
        v[i][7] = nx[i] > 0 ? 0.96f : 0.08f;
        v[i][8] = 0.25f + 0.35f * (ny[i] > 0 ? 1.0f : 0.0f);
        v[i][9] = 0.73f - 0.22f * (nx[i] > 0 ? 1.0f : 0.0f);
        v[i][10] = 0.61f;
        v[i][11] = ny[i] > 0 ? 0.91f : 0.15f;
    }
    const int idx[6] = {0, 1, 2, 2, 1, 3};
    float tri[6][12];
    for (int i = 0; i < 6; ++i) std::memcpy(tri[i], v[idx[i]], sizeof tri[i]);
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STREAM_DRAW);
    for (int a = 0; a < 3; ++a) {
        glVertexAttribPointer(static_cast<GLuint>(a), 4, GL_FLOAT, GL_FALSE, 12 * sizeof(float), reinterpret_cast<void*>(static_cast<uintptr_t>(a * 4 * sizeof(float))));
        glEnableVertexAttribArray(static_cast<GLuint>(a));
    }
    glClearColor(0.18f, 0.28f, 0.38f, 0.05f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    std::vector<uint8_t> rgba(kW * kH * 4);
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    Require(glGetError() == GL_NO_ERROR, "GL draw clean");
    glDeleteBuffers(1, &vbo);
    glDeleteProgram(prog);
    return rgba;
}

void VerifyStructure(const vgx::FsKey& k, const std::string& text) {
    const size_t discPos = text.find("discard;");
    Require(discPos != std::string::npos, "split shader has discard;");
    const size_t phase2Pos = text.find("GXV_FS_ATEARLY phase 2:");
    Require(phase2Pos != std::string::npos && discPos < phase2Pos, "discard precedes phase 2 colour");
    // CRITICAL v2 INVARIANT: ZERO texture2D / texture2DProj calls anywhere after `discard;`!
    const std::string afterDiscard = text.substr(discPos + 8);
    Require(afterDiscard.find("texture2D(") == std::string::npos, "ZERO texture2D() after discard");
    Require(afterDiscard.find("texture2DProj(") == std::string::npos, "ZERO texture2DProj() after discard");
    if (k.fog) {
        const size_t fgPos = text.find("float fg = ");
        Require(fgPos != std::string::npos && discPos < fgPos, "discard precedes fog calculation");
    }
    // Check `// stage ` count matches numStages for gpu-scenes census!
    int stageMarks = 0;
    size_t pos = 0;
    while ((pos = text.find("// stage ", pos)) != std::string::npos) { ++stageMarks; pos += 9; }
    Require(stageMarks == static_cast<int>(k.numStages), "// stage count matches numStages");
}

void CheckCase(const char* name, const vgx::FsKey& k, uint32_t seed, bool expectSplit, int& maxDeltaOut) {
    Require(vgx::FsAtEarlySplitEligible(k) == expectSplit, name);
    vgx::FsRegsModeForTest(0);
    vgx::FsAtEarlyModeForTest(0);
    vgx::ShaderSource srcOff;
    vgx::GenerateFragmentShader(k, srcOff);

    vgx::FsAtEarlyModeForTest(1);
    vgx::ShaderSource srcOn;
    vgx::GenerateFragmentShader(k, srcOn);

    vgx::FsRegsModeForTest(1);
    vgx::ShaderSource srcBoth;
    vgx::GenerateFragmentShader(k, srcBoth);
    vgx::FsRegsModeForTest(0);

    if (expectSplit) {
        VerifyStructure(k, srcOn.text);
        VerifyStructure(k, srcBoth.text);
        Require(srcBoth.tevGather, "combined FS_REGS+FS_ATEARLY uses tevGather");
        Require(srcBoth.text.find("u_tev[-1]") == std::string::npos, "combined shader has no -1 slot");
        Require(srcBoth.nTev <= srcOn.nTev, "combined shader compacts u_tev[]");
    } else {
        Require(srcOn.text == srcOff.text, "ineligible key produces byte-identical shader");
    }

    float tev[128] = {};
    BuildDistinctTev(k, tev, seed);
    const std::vector<uint8_t> a = RenderKey(k, srcOff, tev);
    const std::vector<uint8_t> b = RenderKey(k, srcOn, tev);
    const std::vector<uint8_t> c = RenderKey(k, srcBoth, tev);
    int md = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        md = std::max(md, std::abs(int(a[i]) - int(b[i])));
        md = std::max(md, std::abs(int(a[i]) - int(c[i])));
    }
    if (md > maxDeltaOut) maxDeltaOut = md;
    if (md != 0) {
        std::fprintf(stderr, "MISMATCH in '%s': max delta %d/255\n", name, md);
        std::exit(1);
    }
}

vgx::FsKey MakeLeakKey() {
    // Stage 0 writes c0.a; Stage 1 colour reads c0.a (before stage 2 overwrites c0.a!); Stage 2 overwrites c0.a;
    // Stage 3 colour reads BOTH old/new alpha + separate colour-only texture with lodBias + fog!
    vgx::FsKey k;
    k.numStages = 4; k.numTexGens = 3; k.texProjMask = 1u << 2;
    k.alphaTestOn = 1; k.alphaTest = 6u | (3u << 3) | (0u << 6); // GEQUAL ref0 AND LEQUAL ref1
    k.fog = static_cast<uint8_t>(2 | vgx::kFogVtxBit);
    k.stage[0].texEnable = 1; k.stage[0].texMap = 0; k.stage[0].texCoord = 0; k.stage[0].rasChan = 0;
    k.stage[0].color = PackC(2, 8, 10, 15, 0, 0, 1, 0, 0);
    k.stage[0].alpha = PackA(1, 4, 5, 7, 0, 0, 1, 0, 1); // writes c0.a
    // Stage 1: colour reads c0.a (sel=3 -> overwritten at stage 2 -> MUST snapshot!), plus colour-only texMap 3 with lodBias!
    k.stage[1].texEnable = 1; k.stage[1].texMap = 3; k.stage[1].texCoord = 1; k.stage[1].lodBias = 1; k.stage[1].rasChan = 1;
    k.stage[1].color = PackC(0, 8, 3, 10, 0, 0, 1, 0, 2); // sel=3 is c0.aaa!
    k.stage[1].alpha = PackA(1, 5, 6, 7, 0, 0, 1, 0, 0);
    // Stage 2: overwrites c0.a!
    k.stage[2].texEnable = 1; k.stage[2].texMap = 5; k.stage[2].texCoord = 2;
    k.stage[2].color = PackC(4, 8, 1, 15, 0, 0, 1, 0, 3);
    k.stage[2].alpha = PackA(0, 4, 2, 7, 0, 0, 1, 0, 1); // OVERWRITES c0.a!
    // Stage 3: reads overwritten c0.a + prev.a
    k.stage[3].texEnable = 0; k.stage[3].rasChan = 0;
    k.stage[3].color = PackC(6, 3, 0, 10, 0, 0, 1, 0, 0);
    k.stage[3].alpha = PackA(1, 0, 5, 7, 0, 0, 1, 0, 0);
    return k;
}

vgx::FsKey MakeRandomCutoutKey(std::mt19937& rng) {
    auto R = [&](uint32_t n) { return rng() % n; };
    vgx::FsKey k;
    k.numStages = static_cast<uint8_t>(1 + R(5));
    k.numTexGens = static_cast<uint8_t>(1 + R(4));
    for (int i = 0; i < k.numTexGens; ++i)
        if (R(3) == 0) k.texProjMask |= static_cast<uint8_t>(1u << i);
    k.alphaTestOn = 1;
    const uint32_t cmps[6] = {1, 2, 3, 4, 5, 6}; // non-trivial comparisons
    k.alphaTest = static_cast<uint8_t>(cmps[R(6)] | (cmps[R(6)] << 3) | (R(4) << 6));
    const uint32_t fm = R(5);
    if (fm == 1) k.fog = 2;
    else if (fm == 2) k.fog = static_cast<uint8_t>(2 | vgx::kFogVtxBit);
    else if (fm == 3) k.fog = static_cast<uint8_t>(2 | vgx::kFogVtxBit | vgx::kFogHalfBit);
    else if (fm == 4) k.fog = static_cast<uint8_t>(4 | 8);
    k.dstAlpha = static_cast<uint8_t>(R(4) == 0 ? 1 : 0);
    k.logicSrc = static_cast<uint8_t>(R(6) == 0 ? (1 + R(2)) : 0);
    if (R(4) == 0 && !(k.fog & vgx::kFogHalfBit)) {
        k.inWrap = 1;
        if (R(2) == 0) k.setDualWrap(static_cast<uint8_t>(1u + R(15)));
    }
    if (R(3) == 0 && !(k.fog & vgx::kFogHalfBit)) {
        k.numIndStages = static_cast<uint8_t>(1 + R(2));
        for (int i = 0; i < k.numIndStages; ++i) {
            k.ind[i].texMap = static_cast<uint8_t>(R(8));
            k.ind[i].texCoord = static_cast<uint8_t>(R(k.numTexGens));
        }
    }
    for (int s = 0; s < k.numStages; ++s) {
        auto& st = k.stage[s];
        st.texEnable = R(2) ? 1 : 0;
        st.texMap = static_cast<uint8_t>(R(8));
        st.texCoord = static_cast<uint8_t>(R(k.numTexGens));
        st.rasChan = static_cast<uint8_t>(R(3) == 0 ? 7 : R(2));
        st.kcsel = static_cast<uint8_t>(R(32));
        st.kasel = static_cast<uint8_t>(R(32));
        st.lodBias = (st.texEnable && R(3) == 0 && !(k.fog & vgx::kFogHalfBit)) ? 1 : 0;
        if (k.numIndStages && st.texEnable && R(3) == 0) {
            const uint32_t mids[4] = {0, 1, 2, 3};
            st.ind = PackInd(R(k.numIndStages), R(4), R(8), mids[R(4)], R(7), R(7), 0);
        }
        const uint32_t dstC = (s == k.numStages - 1) ? 0 : R(4);
        const uint32_t dstA = (s == k.numStages - 1) ? 0 : R(4);
        st.color = PackC(R(16), R(16), R(16), R(16), R(3), R(2), 1, R(3), dstC);
        st.alpha = PackA(R(8), R(8), R(8), R(8), R(3), R(2), 1, R(3), dstA);
    }
    return k;
}

} // namespace

int main() {
    Require(vgx::host::CreateContext(), "GL host context");
    std::printf("GL renderer: %s\n", glGetString(GL_RENDERER));

    GLuint fboTex = 0, fbo = 0;
    glGenTextures(1, &fboTex);
    glBindTexture(GL_TEXTURE_2D, fboTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboTex, 0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "RGBA8 FBO");
    glViewport(0, 0, kW, kH);
    glDisable(GL_DITHER); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);

    GLuint tex[8] = {};
    CreateMipTextures(tex);

    // 1. ProfileWord check
    const uint64_t pwOff = vgx::ProfileWord([](const char* n) -> const char* {
        return std::strcmp(n, "GXV_FS_ATEARLY") == 0 ? "0" : nullptr;
    });
    const uint64_t pwUnset = vgx::ProfileWord([](const char*) -> const char* { return nullptr; });
    const uint64_t pwOn = vgx::ProfileWord([](const char* n) -> const char* {
        return std::strcmp(n, "GXV_FS_ATEARLY") == 0 ? "1" : nullptr;
    });
    Require(pwOff == pwUnset && pwOn != pwOff, "ProfileWord tracks GXV_FS_ATEARLY");

    int maxDelta = 0;
    int rendered = 0;

    // 2. Targeted scenes (including LeakKey with colour-only mipmapped texture + snapshots + fog!)
    const vgx::FsKey leak = MakeLeakKey();
    CheckCase("leak-coltex-fog-snap", leak, 1, true, maxDelta); ++rendered;

    // Half-precision fog + cutout + colour-only texture (must stay before discard!)
    {
        vgx::FsKey hf = leak;
        hf.fog = static_cast<uint8_t>(2 | vgx::kFogVtxBit | vgx::kFogHalfBit);
        hf.stage[1].lodBias = 0;
        CheckCase("cutout-halfFog", hf, 2, true, maxDelta); ++rendered;
    }
    // DualWrap + cutout + colour-only indirect texture (indirect + tex MUST stay before discard!)
    {
        vgx::FsKey dw = leak;
        dw.fog = 0; dw.inWrap = 1; dw.setDualWrap(0x0Fu);
        dw.numIndStages = 1; dw.ind[0].texMap = 2; dw.ind[0].texCoord = 0;
        dw.stage[1].ind = PackInd(0, 0, 7, 1, 0, 0, 0); // colour-only indirect stage!
        CheckCase("cutout-dualWrap-colInd", dw, 3, true, maxDelta); ++rendered;
    }
    // Ineligible cases (must stay identical to GXV_FS_ATEARLY=0)
    {
        vgx::FsKey rej = leak; rej.setRejectZero(1);
        CheckCase("ineligible-rejectZero", rej, 4, false, maxDelta); ++rendered;
        vgx::FsKey cd = leak; cd.setColorDead(1);
        CheckCase("ineligible-colorDead", cd, 5, false, maxDelta); ++rendered;
        vgx::FsKey pre = leak; pre.setPreAlpha(1);
        CheckCase("ineligible-preAlpha", pre, 6, false, maxDelta); ++rendered;
        vgx::FsKey ap = leak; ap.setAtPass(1);
        CheckCase("ineligible-atPass", ap, 7, false, maxDelta); ++rendered;
        vgx::FsKey r8 = leak;
        r8.stage[1].alpha = PackA(1, 4, 5, 7, 3, 0, 1, 0, 0); // R8 alpha compare reads colour!
        CheckCase("ineligible-alphaCmpR8", r8, 8, false, maxDelta); ++rendered;
        vgx::FsKey addp = leak;
        addp.numIndStages = 1; addp.ind[0].texMap = 0; addp.ind[0].texCoord = 0;
        addp.stage[1].ind = PackInd(0, 0, 0, 1, 0, 0, 1); // addPrev=1
        CheckCase("ineligible-indAddPrev", addp, 9, false, maxDelta); ++rendered;
    }

    // Targeted channel-narrowing check under combined FS_REGS=1 + FS_ATEARLY=1:
    // Stage 0 reads ONLY tex.a (must narrow to tex_0_a); Stage 1 reads ONLY tex.rgb (must narrow to tex_1_rgb)!
    {
        vgx::FsKey narrow;
        narrow.numStages = 2; narrow.numTexGens = 2;
        narrow.alphaTestOn = 1; narrow.alphaTest = 6u | (7u << 3) | (0u << 6);
        narrow.stage[0].texEnable = 1; narrow.stage[0].texMap = 0; narrow.stage[0].texCoord = 0; narrow.stage[0].rasChan = 0;
        narrow.stage[0].color = PackC(10, 15, 15, 15, 0, 0, 1, 0, 0); // colour ignores tex0!
        narrow.stage[0].alpha = PackA(4, 7, 7, 7, 0, 0, 1, 0, 0);     // alpha reads tex0.a ONLY!
        narrow.stage[1].texEnable = 1; narrow.stage[1].texMap = 1; narrow.stage[1].texCoord = 1; narrow.stage[1].rasChan = 1;
        narrow.stage[1].color = PackC(0, 8, 10, 15, 0, 0, 1, 0, 0);   // colour reads tex1.rgb ONLY!
        narrow.stage[1].alpha = PackA(0, 7, 7, 7, 0, 0, 1, 0, 0);     // alpha ignores tex1!
        vgx::FsRegsModeForTest(1); vgx::FsAtEarlyModeForTest(1);
        vgx::ShaderSource srcN;
        vgx::GenerateFragmentShader(narrow, srcN);
        Require(srcN.tevGather, "narrow combined key uses tevGather");
        Require(srcN.text.find("tex_0_a") != std::string::npos && srcN.text.find("tex_0 =") == std::string::npos, "alpha-only tex narrowed to tex_0_a");
        Require(srcN.text.find("tex_1_rgb") != std::string::npos && srcN.text.find("tex_1 =") == std::string::npos, "rgb-only tex narrowed to tex_1_rgb");
        vgx::FsRegsModeForTest(0);
        CheckCase("combined-channel-narrow", narrow, 12, true, maxDelta); ++rendered;
    }

    // 3. 150 random cutout keys: ALL 150 compiled & structurally verified (0 tex after discard!) in BOTH modes; first 40 rendered!
    std::mt19937 rng(20261126u);
    int randCompiled = 0, randRendered = 0;
    for (int i = 0; i < 150; ++i) {
        const vgx::FsKey k = MakeRandomCutoutKey(rng);
        vgx::FsAtEarlyModeForTest(1);
        vgx::FsRegsModeForTest(0);
        vgx::ShaderSource src;
        vgx::GenerateFragmentShader(k, src);
        Require(vgx::FsAtEarlySplitEligible(k), "random cutout key is eligible");
        VerifyStructure(k, src.text);
        const GLuint fs = Compile(GL_FRAGMENT_SHADER, src.text);
        glDeleteShader(fs);
        vgx::FsRegsModeForTest(1);
        vgx::ShaderSource srcC;
        vgx::GenerateFragmentShader(k, srcC);
        Require(srcC.tevGather, "random combined cutout key uses tevGather");
        VerifyStructure(k, srcC.text);
        const GLuint fsC = Compile(GL_FRAGMENT_SHADER, srcC.text);
        glDeleteShader(fsC);
        vgx::FsRegsModeForTest(0);
        ++randCompiled;
        if (i < 40) {
            CheckCase("rand-cutout", k, 100u + static_cast<uint32_t>(i), true, maxDelta);
            ++randRendered; ++rendered;
        }
    }

    vgx::FsAtEarlyStats st;
    vgx::FsAtEarlyCounters(st);
    vgx::FsRegsStats rst;
    vgx::FsRegsCounters(rst);
    Require(rst.fallbacks == 0, "zero FS_REGS missing-slot fallbacks in combined AtEarly suite");
    std::printf("fsatearly-v2+regs: %d rendered cases (incl. %d random), %d random compiled (both modes); split=%llu colorStages=%llu colorOnlyTexKeptBeforeDiscard=%llu snapVars=%llu regsShaders=%llu regsFallbacks=%llu; max delta %d/255\n",
                rendered, randRendered, randCompiled,
                static_cast<unsigned long long>(st.split), static_cast<unsigned long long>(st.colorStagesDeferred),
                static_cast<unsigned long long>(st.texKeptBeforeDiscard), static_cast<unsigned long long>(st.snapVars),
                static_cast<unsigned long long>(rst.shaders), static_cast<unsigned long long>(rst.fallbacks), maxDelta);
    Require(st.texKeptBeforeDiscard > 0, "colour-only texture promotion exercised");
    Require(st.snapVars > 0, "alpha snapshot mechanism exercised");
    Require(maxDelta == 0, "exact 0/255 pixel match across all cases");
    std::printf("PASS\n");
    return 0;
}
