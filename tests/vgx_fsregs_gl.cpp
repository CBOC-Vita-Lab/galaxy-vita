// fsregs25 (v2 / Fix B): GXV_FS_REGS=1 fragment shader u_tev[] compaction + back-end gather.
// Real generated GLES2 shaders rendered against GXV_FS_REGS=0 with:
//   * Distinct non-repeating floats in every canonical u_tev[0..31] row/component;
//   * Mip-mapped textures (levels 0..5 with distinct per-level colours) to catch any LODBIAS/INDSCALE/TEXSIZE shift;
//   * Variant rendering (general, fold/regFromVtx=3, rejectZero, atPass, colorDead, alphaDead, preAlpha, silhFuse,
//     dualWrap) driven from the GENERAL key's canonical UB_TEV block gathered through ShaderSource::tevSrc[];
//   * vgx_fs24_gl.cpp's key shapes (kFogHalfBit vertex fog ramp + colorDead alpha-tested ramp);
//   * AtPassFs PixStripTest verbatim-match check;
//   * 14 feature stress scenes + 200-key random corpus (40 rendered with variants) -> 0/255 max delta.
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
        const float ox = 0.07f * (i + 1), oy = 0.05f * (i + 1);
        const bool isV2 = s.find("varying vec2 v_tex" + std::to_string(i) + ";") != std::string::npos;
        char line[256];
        if (isV2) {
            std::snprintf(line, sizeof line, "  v_tex%d = (a_pos.xy * 0.5 + vec2(0.5)) * vec2(%.3f, %.3f) + vec2(%.3f, %.3f);\n",
                          i, 1.0f + 0.13f * i, 1.0f + 0.09f * i, ox, oy);
        } else if ((fk.texProjMask >> i) & 1u) {
            std::snprintf(line, sizeof line, "  float q%d = 1.25 + 0.35 * a_pos.x + 0.2 * a_pos.y;\n  v_tex%d = vec3(((a_pos.xy * 0.5 + vec2(0.5)) + vec2(%.3f, %.3f)) * q%d, q%d);\n",
                          i, i, ox, oy, i, i);
        } else {
            std::snprintf(line, sizeof line, "  v_tex%d = vec3((a_pos.xy * 0.5 + vec2(0.5)) * vec2(%.3f, %.3f) + vec2(%.3f, %.3f), 1.0);\n",
                          i, 1.0f + 0.13f * i, 1.0f + 0.09f * i, ox, oy);
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
        std::fprintf(stderr, "link failed:\n%s\n--- vs ---\n%s\n--- fs ---\n%s\n", log, vtx.c_str(), frag.c_str());
        std::exit(1);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

// Create 8 mip-mapped textures (32x32 down to 1x1) where EVERY mip level has a distinct colour shift
// so any wrong LOD bias, texsize, or indirect scale immediately yields a large pixel delta.
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
                    // Distinct per-map AND per-mip-level signatures
                    px[o + 0] = static_cast<uint8_t>(((x * (11 + 3 * m) + lvl * 43 + m * 29) & 255));
                    px[o + 1] = static_cast<uint8_t>(((y * (13 + 5 * m) + lvl * 71 + m * 53) & 255));
                    px[o + 2] = static_cast<uint8_t>((((x + y) * (7 + m) + lvl * 97 + m * 17) & 255));
                    px[o + 3] = static_cast<uint8_t>(((x * 7 + y * 5 + lvl * 37 + m * 31) & 255));
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

// Build a canonical UB_TEV block from the GENERAL key `genKey` with distinct non-repeating values
// in EVERY single row/component (regs, konsts, misc, fog, fogcol, foghl, indmtx, indscale, texsize, lodbias, indpre, dualWrap).
size_t BuildDistinctCanonicalTev(const vgx::FsKey& genKey, bool fold, float out[128], uint32_t seed) {
    const int canonCount = vgx::FsTevCount(genKey);
    const int canonBase = vgx::FsTevBase(genKey);
    std::memset(out, 0, 128 * sizeof(float));
    const float s = static_cast<float>(seed & 7u) * 0.017f;

    // Rows 0..3: TEV regs (distinct per row & channel; when fold=true rows 1 & 2 are zeroed like BuildTevC!)
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (fold && (r == 1 || r == 2)) out[r * 4 + c] = 0.0f;
            else if (genKey.inWrap & 1) {
                // Out-of-range reg values (> 1.0 or < 0.0) to exercise inWrap / dualWrap
                const int raw = ((r & 1) ? 270 : -19) + r * 31 + c * 17 + static_cast<int>(seed & 15u);
                out[r * 4 + c] = ((genKey.preWrap() >> (c < 3 ? r : r + 4)) & 1u)
                    ? static_cast<float>(raw & 255) / 255.0f
                    : static_cast<float>(raw) / 255.0f;
            } else {
                out[r * 4 + c] = 0.11f + 0.19f * r + 0.043f * c + s * 0.3f;
            }
        }
    }
    // Rows 4..7: KONST (distinct per row & channel)
    for (int k = 0; k < 4; ++k)
        for (int c = 0; c < 4; ++c)
            out[(4 + k) * 4 + c] = 0.08f + 0.21f * k + 0.037f * c + s * 0.2f;

    // Row 8: MISC (ref0, ref1, dstAlpha, ztex)
    out[8 * 4 + 0] = 72.0f + static_cast<float>((seed * 7u) & 31u);
    out[8 * 4 + 1] = 196.0f - static_cast<float>((seed * 5u) & 31u);
    out[8 * 4 + 2] = 0.31f + 0.04f * static_cast<float>(seed & 7u);
    out[8 * 4 + 3] = 0.47f;

    // Row 9: FOG or ATFAST thresholds
    if (vgx::FsAtFastKey(genKey)) {
        float thr[2] = {};
        vgx::AtFastValues(genKey.alphaTest, out[8 * 4 + 0], out[8 * 4 + 1], thr);
        out[9 * 4 + 0] = thr[0];
        out[9 * 4 + 1] = thr[1];
        out[9 * 4 + 2] = 0.0f;
        out[9 * 4 + 3] = 0.0f;
    } else if (genKey.fog) {
        const float start = 40.0f, end = 260.0f;
        const float c1 = 1.0f / (end - start), c0 = -start * c1;
        if (genKey.fog & vgx::kFogVtxBit) {
            out[9 * 4 + 0] = c1;
            out[9 * 4 + 1] = 0.0f;
            out[9 * 4 + 2] = 0.0f;
            out[9 * 4 + 3] = c0;
        } else if (genKey.fog & 8) {
            out[9 * 4 + 0] = 1.6f;
            out[9 * 4 + 1] = 0.0f;
            out[9 * 4 + 2] = 0.25f;
            out[9 * 4 + 3] = 0.0f;
        } else {
            out[9 * 4 + 0] = 1.8f;
            out[9 * 4 + 1] = 1.45f;
            out[9 * 4 + 2] = 0.95f;
            out[9 * 4 + 3] = 0.0f;
        }
        // Row 10: FOGCOL
        out[10 * 4 + 0] = 0.14f + s;
        out[10 * 4 + 1] = 0.53f;
        out[10 * 4 + 2] = 0.87f - s;
        out[10 * 4 + 3] = 0.0f;
        if (genKey.fog & vgx::kFogHalfBit) {
            const vgx::FogHalfLane l = vgx::FogHalfLanes(c1, c0);
            out[11 * 4 + 0] = l.sInv;
            out[11 * 4 + 1] = l.c1s;
            out[11 * 4 + 2] = l.bHi;
            out[11 * 4 + 3] = l.bLo;
            out[10 * 4 + 3] = l.k;
        }
    }

    if (canonBase >= 28) {
        // Rows 11..16: INDMTX (distinct 3x2 matrices; only overwrite row 11 when not kFogHalfBit!)
        const int startMtx = (genKey.fog & vgx::kFogHalfBit) ? 1 : 0;
        for (int m = startMtx; m < 3; ++m) {
            const float sc = 0.06f + 0.03f * m;
            out[(11 + m * 2) * 4 + 0] = (0.31f + 0.11f * m) * sc;
            out[(11 + m * 2) * 4 + 1] = (-0.19f + 0.07f * m) * sc;
            out[(11 + m * 2) * 4 + 2] = (0.13f + 0.05f * m) * sc;
            out[(11 + m * 2) * 4 + 3] = sc;
            out[(12 + m * 2) * 4 + 0] = (-0.23f - 0.09f * m) * sc;
            out[(12 + m * 2) * 4 + 1] = (0.29f + 0.08f * m) * sc;
            out[(12 + m * 2) * 4 + 2] = (0.17f - 0.04f * m) * sc;
            out[(12 + m * 2) * 4 + 3] = 0.0f;
        }
        // Rows 17..20: INDSCALE (distinct per ind stage)
        for (int i = 0; i < 4; ++i) {
            out[(17 + i) * 4 + 0] = 0.5f + 0.17f * i;
            out[(17 + i) * 4 + 1] = 0.4f + 0.19f * i;
            out[(17 + i) * 4 + 2] = 0.0f;
            out[(17 + i) * 4 + 3] = 0.0f;
        }
        // Rows 21..24: TEXSIZE (distinct per map)
        for (int m = 0; m < 8; ++m) {
            out[21 * 4 + m * 2 + 0] = 24.0f + 8.0f * m;
            out[21 * 4 + m * 2 + 1] = 28.0f + 6.0f * m;
        }
        // Rows 25..26: LODBIAS (distinct per map, spanning mip levels 0..5!)
        for (int m = 0; m < 8; ++m)
            out[25 * 4 + m] = 0.35f + 0.48f * m;
        // Row 27: INDPRE
        if (genKey.indPre() && genKey.indPre() <= genKey.numStages) {
            const unsigned map = genKey.stage[genKey.indPre() - 1].texMap & 7u;
            const uint16_t tw = static_cast<uint16_t>(out[21 * 4 + map * 2 + 0]);
            const uint16_t th = static_cast<uint16_t>(out[21 * 4 + map * 2 + 1]);
            vgx::IndPreValues(genKey, &out[11 * 4], tw, th, &out[27 * 4]);
        }
    }

    if (genKey.dualWrap()) {
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                const int raw = ((r & 1) ? 270 : -19) + r * 31 + c * 17 + static_cast<int>(seed & 15u);
                out[(canonBase + r) * 4 + c] = ((genKey.dualWrap() >> r) & 1u) ? static_cast<float>(raw & 255) / 255.0f : 0.0f;
            }
        }
    }
    return static_cast<size_t>(canonCount) * 4u;
}

std::vector<uint8_t> RenderVariant(const vgx::FsKey& drawKey, const vgx::ShaderSource& src, const float* canonTev, size_t canonFloats, bool foldColors) {
    const std::string vsText = MakeVs(drawKey, src.text);
    const GLuint prog = Link(vsText, src.text);
    glUseProgram(prog);
    for (int m = 0; m < 8; ++m) {
        char nm[16];
        std::snprintf(nm, sizeof nm, "u_tex%d", m);
        const GLint loc = glGetUniformLocation(prog, nm);
        if (loc >= 0) glUniform1i(loc, m);
    }

    float scratch[128] = {};
    const float* tevPtr = vgx::FsTevGather(src, canonTev, canonFloats, scratch);
    const GLint tevLoc = glGetUniformLocation(prog, "u_tev");
    if (tevLoc >= 0 && src.nTev > 0) glUniform4fv(tevLoc, static_cast<GLsizei>(src.nTev), tevPtr);

    // 4 vertices covering NDC [-1, 1]^2 with graded depth and vertex colours
    const float c0Const[4] = {0.83f, 0.27f, 0.61f, 0.74f};
    const float c1Const[4] = {0.29f, 0.77f, 0.41f, 0.63f};
    float v[4][12] = {};
    const float nx[4] = {-1, 1, -1, 1}, ny[4] = {-1, -1, 1, 1}, nz[4] = {-0.7f, -0.2f, 0.35f, 0.85f};
    for (int i = 0; i < 4; ++i) {
        v[i][0] = nx[i]; v[i][1] = ny[i]; v[i][2] = nz[i]; v[i][3] = 1.0f;
        if (foldColors) {
            for (int c = 0; c < 4; ++c) { v[i][4 + c] = c0Const[c]; v[i][8 + c] = c1Const[c]; }
        } else {
            v[i][4] = (nx[i] > 0 ? 0.91f : 0.17f);
            v[i][5] = (ny[i] > 0 ? 0.82f : 0.23f);
            v[i][6] = 0.44f + 0.25f * nx[i] * ny[i];
            v[i][7] = (nx[i] > 0 ? 0.95f : 0.12f);
            v[i][8] = 0.21f + 0.3f * (ny[i] > 0 ? 1.0f : 0.0f);
            v[i][9] = 0.71f - 0.2f * (nx[i] > 0 ? 1.0f : 0.0f);
            v[i][10] = 0.63f;
            v[i][11] = (ny[i] > 0 ? 0.88f : 0.19f);
        }
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
    glClearColor(0.2f, 0.3f, 0.4f, 0.1f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    std::vector<uint8_t> rgba(kW * kH * 4);
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    Require(glGetError() == GL_NO_ERROR, "GL draw clean");
    glDeleteBuffers(1, &vbo);
    glDeleteProgram(prog);
    return rgba;
}

int MaxDelta(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    Require(a.size() == b.size(), "image sizes match");
    int md = 0;
    for (size_t i = 0; i < a.size(); ++i)
        md = std::max(md, std::abs(int(a[i]) - int(b[i])));
    return md;
}

std::string PixStripTestCopy(const std::string& t) {
    std::string r;
    r.reserve(t.size());
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

void CheckCase(const char* name, const vgx::FsKey& generalKey, const vgx::FsKey& drawKey, bool foldColors, uint32_t seed, int& maxDeltaOut, uint64_t& rowsBefore, uint64_t& rowsAfter) {
    vgx::FsRegsModeForTest(0);
    vgx::ShaderSource srcOff;
    vgx::GenerateFragmentShader(drawKey, srcOff);
    Require(!srcOff.tevGather && srcOff.nTev == srcOff.nTevCanon, "off-state ShaderSource is canonical");

    vgx::FsRegsModeForTest(1);
    vgx::ShaderSource srcOn;
    vgx::GenerateFragmentShader(drawKey, srcOn);
    Require(srcOn.tevGather, "on-state ShaderSource uses Fix B gather");
    Require(srcOn.nTevCanon == static_cast<uint8_t>(vgx::FsTevCount(drawKey)), "nTevCanon matches canonical FsTevCount");
    Require(srcOn.text.find("u_tev[-1]") == std::string::npos, "no negative u_tev slot");
    const int genCanonCount = vgx::FsTevCount(generalKey);
    for (uint8_t i = 0; i < srcOn.nTev; ++i)
        Require(srcOn.tevSrc[i] < static_cast<uint8_t>(genCanonCount), "every tevSrc[i] is in general key's canonical block");

    // Canonical TEV block built from the GENERAL key (not drawKey!), matching vgx_draw.cpp::BuildTevC!
    float canonTev[128] = {};
    const size_t canonFloats = BuildDistinctCanonicalTev(generalKey, foldColors, canonTev, seed);

    const std::vector<uint8_t> imgOff = RenderVariant(drawKey, srcOff, canonTev, canonFloats, foldColors);
    const std::vector<uint8_t> imgOn = RenderVariant(drawKey, srcOn, canonTev, canonFloats, foldColors);
    const int md = MaxDelta(imgOff, imgOn);
    if (md > maxDeltaOut) maxDeltaOut = md;
    rowsBefore += srcOff.nTev;
    rowsAfter += srcOn.nTev;
    if (md != 0) {
        std::fprintf(stderr, "MISMATCH in case '%s': max delta %d/255 (off nTev=%u, on nTev=%u)\n", name, md, srcOff.nTev, srcOn.nTev);
        std::exit(1);
    }
}

vgx::FsKey MakeRichGeneralKey(bool withFog, bool halfFog, bool withInd, bool withLodBias, bool withDualWrap, bool withAlphaTest) {
    vgx::FsKey k;
    k.numStages = 3;
    k.numTexGens = 3;
    k.texProjMask = 1u << 1;
    if (withAlphaTest) {
        k.alphaTestOn = 1;
        k.alphaTest = 6u | (3u << 3) | (0u << 6); // GEQUAL ref0 AND LEQUAL ref1
    }
    if (withFog) {
        k.fog = static_cast<uint8_t>(2 | vgx::kFogVtxBit | (halfFog ? vgx::kFogHalfBit : 0));
    }
    if (withDualWrap) {
        k.inWrap = 1;
        k.setDualWrap(0x0Fu);
    }
    // Stage 0: reads tex0 + c0 + ras + konst0 -> prev
    k.stage[0].texEnable = 1; k.stage[0].texMap = 2; k.stage[0].texCoord = 0; k.stage[0].rasChan = 0;
    k.stage[0].kcsel = 12 + 1; k.stage[0].kasel = 16 + 2;
    k.stage[0].lodBias = withLodBias ? 1 : 0;
    k.stage[0].color = PackC(2, 8, 14, 10, 0, 0, 1, 0, 0);
    k.stage[0].alpha = PackA(1, 4, 6, 5, 0, 0, 1, 0, 0);
    // Stage 1: projective tex1 (optional ind) + c1 + prev -> c2
    k.stage[1].texEnable = 1; k.stage[1].texMap = 5; k.stage[1].texCoord = 1; k.stage[1].rasChan = 1;
    k.stage[1].kcsel = 12 + 3; k.stage[1].kasel = 20 + 0;
    k.stage[1].lodBias = withLodBias ? 1 : 0;
    if (withInd) {
        k.numIndStages = 2;
        k.ind[0].texMap = 0; k.ind[0].texCoord = 0;
        k.ind[1].texMap = 3; k.ind[1].texCoord = 2;
        k.stage[1].ind = PackInd(1, 0, 7, 2, 0, 0, 0);
    }
    k.stage[1].color = PackC(4, 8, 0, 14, 0, 0, 1, 0, 3);
    k.stage[1].alpha = PackA(2, 4, 0, 6, 0, 0, 1, 0, 3);
    // Stage 2: mixes c2, prev, c0 -> prev
    k.stage[2].texEnable = 0; k.stage[2].rasChan = 0;
    k.stage[2].color = PackC(6, 0, 2, 15, 0, 0, 1, 0, 0);
    k.stage[2].alpha = PackA(3, 0, 1, 7, 0, 0, 1, 0, 0);
    return k;
}

vgx::FsKey MakeRandomKey(std::mt19937& rng) {
    auto R = [&](uint32_t n) { return rng() % n; };
    vgx::FsKey k;
    k.numStages = static_cast<uint8_t>(1 + R(5));
    k.numTexGens = static_cast<uint8_t>(R(5));
    for (int i = 0; i < k.numTexGens; ++i)
        if (R(3) == 0) k.texProjMask |= static_cast<uint8_t>(1u << i);
    if (R(2) == 0) {
        k.alphaTestOn = 1;
        k.alphaTest = static_cast<uint8_t>(R(8) | (R(8) << 3) | (R(4) << 6));
    }
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
    if (k.numTexGens > 0 && R(3) == 0 && !(k.fog & vgx::kFogHalfBit)) {
        k.numIndStages = static_cast<uint8_t>(1 + R(3));
        for (int i = 0; i < k.numIndStages; ++i) {
            k.ind[i].texMap = static_cast<uint8_t>(R(8));
            k.ind[i].texCoord = static_cast<uint8_t>(R(k.numTexGens));
        }
    }
    for (int s = 0; s < k.numStages; ++s) {
        auto& st = k.stage[s];
        st.texEnable = (k.numTexGens > 0 && R(2) == 0) ? 1 : 0;
        st.texMap = static_cast<uint8_t>(R(8));
        st.texCoord = k.numTexGens ? static_cast<uint8_t>(R(k.numTexGens)) : 0;
        st.rasChan = static_cast<uint8_t>(R(3) == 0 ? 7 : R(2));
        st.kcsel = static_cast<uint8_t>(R(32));
        st.kasel = static_cast<uint8_t>(R(32));
        st.lodBias = (st.texEnable && R(4) == 0 && !(k.fog & vgx::kFogHalfBit)) ? 1 : 0;
        if (k.numIndStages && st.texEnable && R(3) == 0) {
            const uint32_t midChoices[6] = {0, 1, 2, 3, 5, 9};
            st.ind = PackInd(R(k.numIndStages), R(4), R(8), midChoices[R(6)], R(7), R(7), s > 0 ? R(2) : 0);
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

    // 1. ProfileWord & off-state identity
    const uint64_t pwOff = vgx::ProfileWord([](const char* n) -> const char* {
        if (std::strcmp(n, "GXV_FS_REGS") == 0) return "0";
        return nullptr;
    });
    const uint64_t pwUnset = vgx::ProfileWord([](const char*) -> const char* { return nullptr; });
    const uint64_t pwOn = vgx::ProfileWord([](const char* n) -> const char* {
        if (std::strcmp(n, "GXV_FS_REGS") == 0) return "1";
        return nullptr;
    });
    Require(pwOff == pwUnset, "ProfileWord unchanged when GXV_FS_REGS=0");
    Require(pwOn != pwOff, "ProfileWord changes when GXV_FS_REGS=1");

    int maxDelta = 0;
    uint64_t rowsBefore = 0, rowsAfter = 0;
    int renderedCases = 0;

    // 2. Variant-with-general-block oracle (the exact Fix B contract!)
    // General key 1: rich fog + alphaTest + lodBias + ind + proj
    {
        const vgx::FsKey gen = MakeRichGeneralKey(true, false, true, true, false, true);
        CheckCase("var/general-ind-fog-at", gen, gen, false, 1, maxDelta, rowsBefore, rowsAfter); ++renderedCases;

        vgx::FsKey vFold = gen; vFold.regFromVtx = 3;
        CheckCase("var/fold-ind-fog-at", gen, vFold, true, 2, maxDelta, rowsBefore, rowsAfter); ++renderedCases;

        vgx::FsKey vRej = gen; vRej.setRejectZero(1);
        CheckCase("var/rejectZero-ind-fog-at", gen, vRej, false, 3, maxDelta, rowsBefore, rowsAfter); ++renderedCases;

        vgx::FsKey vAtPass = gen; vAtPass.setAtPass(1);
        CheckCase("var/atPass-ind-fog-at", gen, vAtPass, false, 4, maxDelta, rowsBefore, rowsAfter); ++renderedCases;
        // Verify AtPassFs's PixStripTest matches verbatim under GXV_FS_REGS=1!
        vgx::FsRegsModeForTest(1);
        vgx::ShaderSource sTested, sAtPass;
        vgx::GenerateFragmentShader(gen, sTested);
        vgx::GenerateFragmentShader(vAtPass, sAtPass);
        Require(PixStripTestCopy(sTested.text) == sAtPass.text, "AtPassFs PixStripTest matches verbatim under GXV_FS_REGS=1");

        vgx::FsKey vColorDead = gen; vColorDead.setColorDead(1);
        CheckCase("var/colorDead-ind-fog-at", gen, vColorDead, false, 5, maxDelta, rowsBefore, rowsAfter); ++renderedCases;

        vgx::FsKey vPre = gen; vPre.setPreAlpha(1);
        CheckCase("var/preAlpha-ind-fog-at", gen, vPre, false, 6, maxDelta, rowsBefore, rowsAfter); ++renderedCases;
    }

    // General key 2: dualWrap + fold + rejectZero + alphaDead (no fog, no alphaTest for alphaDead)
    {
        const vgx::FsKey gen = MakeRichGeneralKey(false, false, false, true, true, true);
        CheckCase("var/dualWrap-general", gen, gen, false, 10, maxDelta, rowsBefore, rowsAfter); ++renderedCases;
        vgx::FsKey vFold = gen; vFold.regFromVtx = 3;
        CheckCase("var/dualWrap-fold", gen, vFold, true, 11, maxDelta, rowsBefore, rowsAfter); ++renderedCases;
        vgx::FsKey vRej = gen; vRej.setRejectZero(1);
        CheckCase("var/dualWrap-rejectZero", gen, vRej, false, 12, maxDelta, rowsBefore, rowsAfter); ++renderedCases;

        vgx::FsKey genNoAt = gen; genNoAt.alphaTestOn = 0; genNoAt.alphaTest = 0;
        vgx::FsKey vADead = genNoAt; vADead.setAlphaDead(1);
        CheckCase("var/dualWrap-alphaDead", genNoAt, vADead, false, 13, maxDelta, rowsBefore, rowsAfter); ++renderedCases;
    }

    // General key 3: vgx_fs24_gl.cpp shapes (kFogHalfBit vertex fog + colorDead with/without kFogHalfBit + fold!)
    {
        vgx::FsKey fs24;
        fs24.numStages = 1;
        fs24.stage[0].color = PackC(10, 15, 15, 15, 0, 0, 1, 0, 0);
        fs24.stage[0].alpha = PackA(5, 7, 7, 7, 0, 0, 1, 0, 0);
        fs24.stage[0].rasChan = 0;
        fs24.fog = static_cast<uint8_t>(2 | vgx::kFogVtxBit | vgx::kFogHalfBit);
        fs24.alphaTestOn = 1; fs24.alphaTest = 6u | (7u << 3) | (0u << 6);
        CheckCase("fs24/halfFog-at", fs24, fs24, false, 20, maxDelta, rowsBefore, rowsAfter); ++renderedCases;
        vgx::FsKey fs24Dead = fs24; fs24Dead.setColorDead(1);
        CheckCase("fs24/halfFog-colorDead", fs24, fs24Dead, false, 21, maxDelta, rowsBefore, rowsAfter); ++renderedCases;
        vgx::FsKey fs24Fold = fs24; fs24Fold.regFromVtx = 3;
        CheckCase("fs24/halfFog-fold", fs24, fs24Fold, true, 22, maxDelta, rowsBefore, rowsAfter); ++renderedCases;
        vgx::FsKey fs24NoFog = fs24; fs24NoFog.fog = 0;
        vgx::FsKey fs24NoFogDead = fs24NoFog; fs24NoFogDead.setColorDead(1);
        CheckCase("fs24/noFog-colorDead", fs24NoFog, fs24NoFogDead, false, 23, maxDelta, rowsBefore, rowsAfter); ++renderedCases;
    }

    // General key 4: silhFuse variant with general key's canonical block
    {
        vgx::FsKey silhGen;
        silhGen.numStages = 1; silhGen.numTexGens = 1;
        silhGen.alphaTestOn = 1; silhGen.alphaTest = 4u | (7u << 3) | (0u << 6);
        silhGen.stage[0].texEnable = 1; silhGen.stage[0].texMap = 1; silhGen.stage[0].texCoord = 0;
        silhGen.stage[0].color = PackC(15, 15, 15, 2, 0, 0, 1, 0, 0);
        silhGen.stage[0].alpha = PackA(7, 7, 7, 4, 0, 0, 1, 0, 0);
        vgx::FsKey silhF = silhGen;
        silhF.setSilhFuse(vgx::silhfuse::Code(false, vgx::silhfuse::K_SA, vgx::silhfuse::K_ISA), vgx::silhfuse::A_KEEP);
        CheckCase("var/silhFuse", silhGen, silhF, false, 30, maxDelta, rowsBefore, rowsAfter); ++renderedCases;
    }

    // General key 5: indPre + indPrev + compare-mode + dstAlpha + logicSrc + texReuse
    {
        vgx::FsKey indPreKey = MakeRichGeneralKey(false, false, true, false, false, false);
        indPreKey.stage[1].ind = PackInd(1, 0, 0, 2, 0, 0, 0);
        indPreKey.setIndPre(2);
        CheckCase("stress/indPre", indPreKey, indPreKey, false, 40, maxDelta, rowsBefore, rowsAfter); ++renderedCases;

        vgx::FsKey cmpKey;
        cmpKey.numStages = 2; cmpKey.numTexGens = 1; cmpKey.dstAlpha = 1; cmpKey.logicSrc = 1;
        cmpKey.stage[0].texEnable = 1; cmpKey.stage[0].texMap = 4; cmpKey.stage[0].texCoord = 0;
        cmpKey.stage[0].color = PackC(8, 2, 14, 0, 3, 0, 1, 0, 0); // compare mode R8
        cmpKey.stage[0].alpha = PackA(4, 1, 6, 0, 0, 0, 1, 0, 0);
        cmpKey.stage[1] = cmpKey.stage[0]; // identical tex read -> exercises texReuse
        cmpKey.stage[1].color = PackC(0, 8, 12, 15, 0, 0, 1, 0, 0);
        CheckCase("stress/cmp-dstA-logic-reuse", cmpKey, cmpKey, false, 41, maxDelta, rowsBefore, rowsAfter); ++renderedCases;
    }

    // 3. 200-key random corpus: all 200 compile & validate; first 40 ALSO render (general + fold + rejectZero/colorDead)!
    std::mt19937 rng(20261125u);
    int randomCompiled = 0, randomRendered = 0;
    for (int i = 0; i < 200; ++i) {
        const vgx::FsKey k = MakeRandomKey(rng);
        vgx::FsRegsModeForTest(1);
        vgx::ShaderSource src;
        vgx::GenerateFragmentShader(k, src);
        Require(src.tevGather, "random key uses Fix B gather");
        Require(src.text.find("u_tev[-1]") == std::string::npos, "random key has no -1 slot");
        const int canon = vgx::FsTevCount(k);
        for (uint8_t j = 0; j < src.nTev; ++j)
            Require(src.tevSrc[j] < static_cast<uint8_t>(canon), "random tevSrc in canonical range");
        const GLuint fs = Compile(GL_FRAGMENT_SHADER, src.text);
        glDeleteShader(fs);
        ++randomCompiled;

        if (i < 40) {
            CheckCase("rand/general", k, k, false, 100u + static_cast<uint32_t>(i), maxDelta, rowsBefore, rowsAfter);
            ++randomRendered; ++renderedCases;
            vgx::FsKey kf = k; kf.regFromVtx = 3;
            CheckCase("rand/fold", k, kf, true, 200u + static_cast<uint32_t>(i), maxDelta, rowsBefore, rowsAfter);
            ++renderedCases;
            if (k.alphaTestOn) {
                vgx::FsKey kr = k; kr.setRejectZero(1);
                CheckCase("rand/rejectZero", k, kr, false, 300u + static_cast<uint32_t>(i), maxDelta, rowsBefore, rowsAfter);
                ++renderedCases;
            }
            vgx::FsKey kd = k; kd.setColorDead(1);
            CheckCase("rand/colorDead", k, kd, false, 400u + static_cast<uint32_t>(i), maxDelta, rowsBefore, rowsAfter);
            ++renderedCases;
        }
    }

    vgx::FsRegsStats st;
    vgx::FsRegsCounters(st);
    Require(st.fallbacks == 0, "zero missing-slot fallbacks across entire suite");
    std::printf("fsregs-v2: %d rendered cases (incl. %d random + variants), %d random compiled; u_tev mean %.2f -> %.2f vec4; max delta %d/255; fallbacks=%llu\n",
                renderedCases, randomRendered, randomCompiled,
                renderedCases ? static_cast<double>(rowsBefore) / renderedCases : 0.0,
                renderedCases ? static_cast<double>(rowsAfter) / renderedCases : 0.0,
                maxDelta, static_cast<unsigned long long>(st.fallbacks));
    Require(maxDelta == 0, "exact 0/255 pixel match across all cases");
    std::printf("PASS\n");
    return 0;
}
