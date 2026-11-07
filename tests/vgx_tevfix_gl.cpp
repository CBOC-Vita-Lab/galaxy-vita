// GX -> GLES2 translation repairs, on real generated shaders and real GL blending (Mesa):
//   logic ops (LogicOpPlan), destination alpha with blending (PlanDstAlpha), the indirect "add previous stage"
//   coordinate (GXV_FIX_INDPREV), a stage that reads a switched-off texture (FsKey::Stage::texOff).
// Each section prints the old output next to the repaired one for the same synthetic GX case.
// Usage: vgx_tevfix_gl <expect-indprev 0|1>
#include "vgx_fixes.h"
#include "vgx_shadergen.h"
#include <GLES2/gl2.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vgx { bool LogEnabled() { return false; } }
namespace {
using namespace vgx;
int g_bad = 0;
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void Check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "MISMATCH: %s\n", why); ++g_bad; }
}
GLuint Compile(GLenum type, const std::string& text) {
    GLuint s = glCreateShader(type);
    const char* p = text.c_str(); glShaderSource(s, 1, &p, nullptr); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096] = {}; glGetShaderInfoLog(s, sizeof log, nullptr, log); std::fprintf(stderr, "%s\n%s\n", log, p); }
    Require(ok, "generated shader compiles"); return s;
}
GLuint Program(const FsKey& key) {
    ShaderSource src; GenerateFragmentShader(key, src);
    std::string vertex = "attribute vec2 a_pos; attribute vec4 a_col0, a_col1; varying vec4 v_col0, v_col1;\n";
    for (int i = 0; i < key.numTexGens; ++i) vertex += "varying vec3 v_tex" + std::to_string(i) + ";\n";
    vertex += "void main(){gl_Position=vec4(a_pos,0.0,1.0);v_col0=a_col0;v_col1=a_col1;\n";
    for (int i = 0; i < key.numTexGens; ++i) vertex += "v_tex" + std::to_string(i) + "=vec3(a_pos*0.25+0.5,1.0);\n";
    vertex += "}\n";
    GLuint vs = Compile(GL_VERTEX_SHADER, vertex), fs = Compile(GL_FRAGMENT_SHADER, src.text);
    GLuint p = glCreateProgram(); glAttachShader(p, vs); glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos"); glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok); Require(ok, "generated shader links");
    glDeleteShader(vs); glDeleteShader(fs); return p;
}
void Uni(GLuint p, int i, float x, float y, float z, float w) {
    const std::string name = "u_tev[" + std::to_string(i) + "]";
    const GLint loc = glGetUniformLocation(p, name.c_str());
    if (loc >= 0) glUniform4f(loc, x, y, z, w);
}
GLenum Gl(uint8_t f) {
    switch (f) {
    case BF_ZERO: return GL_ZERO; case BF_ONE: return GL_ONE; case BF_SRC_COLOR: return GL_SRC_COLOR;
    case BF_INV_SRC_COLOR: return GL_ONE_MINUS_SRC_COLOR; case BF_SRC_ALPHA: return GL_SRC_ALPHA;
    case BF_INV_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA; case BF_DST_ALPHA: return GL_DST_ALPHA;
    case BF_INV_DST_ALPHA: return GL_ONE_MINUS_DST_ALPHA; case BF_DST_COLOR: return GL_DST_COLOR;
    default: return GL_ONE_MINUS_DST_COLOR;
    }
}
// c0 straight to the output: colour D = c0, alpha D = c0.a, clamped.
FsKey PassC0() {
    FsKey k;
    k.stage[0].color = 2u | (15u << 4) | (15u << 8) | (15u << 12) | (1u << 19);
    k.stage[0].alpha = (1u << 4) | (7u << 7) | (7u << 10) | (7u << 13) | (1u << 19);
    return k;
}
void Px(uint8_t px[4]) { glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); }

// ---------------------------------------------------------------- logic ops
uint8_t RefLogic(unsigned op, uint8_t s, uint8_t d) {
    switch (op) {
    case 0: return 0; case 1: return s & d; case 2: return s & ~d; case 3: return s; case 4: return ~s & d; case 5: return d;
    case 6: return s ^ d; case 7: return s | d; case 8: return ~(s | d); case 9: return ~(s ^ d); case 10: return ~d;
    case 11: return s | ~d; case 12: return ~s; case 13: return ~s | d; case 14: return ~(s & d); default: return 0xFF;
    }
}
const char* const kLogicName[16] = {"CLEAR", "AND", "REVAND", "COPY", "INVAND", "NOOP", "XOR", "OR", "NOR", "EQUIV", "INV", "REVOR", "INVCOPY", "INVOR", "NAND", "SET"};
unsigned LogicRun(unsigned op, bool fixed) {
    uint8_t lsf, ldf;
    fixes::LegacyLogicOpFactors(op, lsf, ldf);
    const fixes::LogicPlan lp = fixes::LogicOpPlan(op);
    FsKey k = PassC0();
    if (fixed) k.logicSrc = lp.src;
    GLuint p = Program(k);
    glUseProgram(p);
    unsigned bad = 0;
    for (unsigned sbits = 0; sbits < 8; ++sbits)
        for (unsigned dbits = 0; dbits < 8; ++dbits) {
            Uni(p, 1, (sbits & 1) ? 1.f : 0.f, (sbits & 2) ? 1.f : 0.f, (sbits & 4) ? 1.f : 0.f, 1.f);
            glClearColor((dbits & 1) ? 1.f : 0.f, (dbits & 2) ? 1.f : 0.f, (dbits & 4) ? 1.f : 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(Gl(fixed ? lp.sf : lsf), Gl(fixed ? lp.df : ldf));
            glDrawArrays(GL_TRIANGLES, 0, 3);
            uint8_t px[4]; Px(px);
            for (int c = 0; c < 3; ++c) {
                const uint8_t s = (sbits >> c & 1) ? 255 : 0, d = (dbits >> c & 1) ? 255 : 0;
                if (px[c] != RefLogic(op, s, d)) ++bad;
            }
        }
    glDisable(GL_BLEND);
    glDeleteProgram(p);
    return bad;
}

// ---------------------------------------------------------------- destination alpha
struct DaCase {
    const char* name;
    unsigned constA;
    bool colorWrite, blendBit, subtract;
    uint8_t srcF, dstF;
    float srcRgb, srcA, dstRgb, dstA;  // shader colour / TEV alpha, framebuffer colour / alpha before the draw
};
// The framebuffer a GX pipeline leaves: colour by the blend formula, alpha = the constant (the case writes alpha).
void RefDa(const DaCase& c, float rgb[1], float* a) {
    auto fac = [&](uint8_t f) {
        switch (f) {
        case BF_ZERO: return 0.f; case BF_ONE: return 1.f; case BF_SRC_COLOR: return c.srcRgb; case BF_INV_SRC_COLOR: return 1 - c.srcRgb;
        case BF_SRC_ALPHA: return c.srcA; case BF_INV_SRC_ALPHA: return 1 - c.srcA; case BF_DST_ALPHA: return c.dstA;
        case BF_INV_DST_ALPHA: return 1 - c.dstA; case BF_DST_COLOR: return c.dstRgb; default: return 1 - c.dstRgb;
        }
    };
    float v;
    if (c.subtract) v = c.dstRgb - c.srcRgb;
    else if (c.blendBit) v = c.srcRgb * fac(c.srcF) + c.dstRgb * fac(c.dstF);
    else v = c.srcRgb;
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    rgb[0] = c.colorWrite ? v : c.dstRgb;
    *a = c.constA / 255.f;
}
struct DaOut { float rgb, a; };
DaOut DaRun(const DaCase& c, bool fixed) {
    fixes::DstAlphaPlan plan;
    if (fixed) plan = fixes::PlanDstAlpha(true, c.constA, true, c.colorWrite, c.blendBit, c.subtract, c.srcF, c.dstF);
    else plan.fsConst = !c.blendBit && !c.subtract;  // the shipped behaviour with GXV_DSTALPHA_ZERO off
    FsKey k = PassC0();
    k.dstAlpha = plan.fsConst ? 1 : 0;
    GLuint p = Program(k);
    glUseProgram(p);
    Uni(p, 1, c.srcRgb, c.srcRgb, c.srcRgb, c.srcA);
    Uni(p, 8, 0, 0, c.constA / 255.f, 0);
    glClearColor(c.dstRgb, c.dstRgb, c.dstRgb, c.dstA); glClear(GL_COLOR_BUFFER_BIT);
    const bool glBlend = c.subtract || (c.blendBit && !(c.srcF == BF_ONE && c.dstF == BF_ZERO));
    const bool blendOn = glBlend && !plan.dropBlend;
    if (blendOn) {
        glEnable(GL_BLEND);
        glBlendEquation(c.subtract ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
        const GLenum sf = c.subtract ? GL_ONE : Gl(c.srcF), df = c.subtract ? GL_ONE : Gl(c.dstF);
        if (plan.rsMode == 1) glBlendFuncSeparate(sf, df, GL_ZERO, GL_ZERO);
        else if (plan.rsMode == 2) glBlendFuncSeparate(sf, df, GL_ONE, GL_ZERO);
        else glBlendFunc(sf, df);
    } else {
        glDisable(GL_BLEND);
    }
    glColorMask(c.colorWrite, c.colorWrite, c.colorWrite, GL_TRUE);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
    uint8_t px[4]; Px(px);
    glDeleteProgram(p);
    return {px[0] / 255.f, px[3] / 255.f};
}
} // namespace

int main(int argc, char** argv) {
    Require(argc == 2, "usage: vgx_tevfix_gl <expect-indprev 0|1>");
    const bool expectIndPrev = std::atoi(argv[1]) != 0;
    Require(vgx::host::CreateContext(), "Mesa GLES context");
    std::printf("GL renderer: %s\n", glGetString(GL_RENDERER));
    GLuint tex = 0, fbo = 0, vbo = 0;
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "RGBA8 target");
    const float verts[] = {-1, -1, 3, -1, -1, 3};
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr); glEnableVertexAttribArray(0);
    glViewport(0, 0, 1, 1); glDisable(GL_DITHER); glDisable(GL_BLEND);

    // ---- 1. logic ops: every op, every pair of 0/255 channel values (8 source x 8 destination colours, 3 channels)
    std::printf("logic ops (mismatching channel results of 192; old table / GXV_FIX_LOGICOP table):\n");
    unsigned oldTotal = 0, newTotal = 0, oldWrongOps = 0;
    for (unsigned op = 0; op < 16; ++op) {
        const unsigned o = LogicRun(op, false), n = LogicRun(op, true);
        const bool exact = fixes::LogicOpPlan(op).exact;
        std::printf("  %2u %-8s old %3u  fixed %3u  %s\n", op, kLogicName[op], o, n, exact ? "exact" : "nearest single blend");
        oldTotal += o; newTotal += n; oldWrongOps += o != 0;
        if (exact) Check(n == 0, "an op the plan calls exact reproduces the bitwise result");
    }
    std::printf("  ops wrong with the old table: %u of 16; wrong channel results old %u, fixed %u\n", oldWrongOps, oldTotal, newTotal);
    Require(oldWrongOps >= 8, "the old table is wrong for most ops (documents what the repair changes)");
    Check(newTotal < oldTotal, "the repaired table is closer overall");

    // ---- 2. destination alpha with blending
    static const DaCase kDa[] = {
        // Galaxy: VolumeModelDrawer pass 4 / MarioSearchLight: additive by destination alpha, constant 0
        {"BLEND dstalpha/one, const 0", 0, true, true, false, BF_DST_ALPHA, BF_ONE, 0.5f, 0.8f, 0.25f, 1.0f},
        // same with constant 255 (MarioActorSpecialDraw)
        {"BLEND dstalpha/one, const 255", 255, true, true, false, BF_DST_ALPHA, BF_ONE, 0.5f, 0.8f, 0.25f, 0.5f},
        // VolumeModelDrawer pass 3: SUBTRACT, colour update off, alpha only
        {"SUBTRACT colour off, const 255", 255, false, false, true, BF_ONE, BF_ONE, 0.5f, 1.0f, 0.25f, 0.0f},
        // TDDraw dark mask: BLEND srcalpha/one with colour update off, const 255
        {"BLEND srcalpha/one colour off, const 255", 255, false, true, false, BF_SRC_ALPHA, BF_ONE, 0.5f, 0.4f, 0.25f, 0.0f},
        // BLEND one/zero collapses to no blend
        {"BLEND one/zero, const 77", 77, true, true, false, BF_ONE, BF_ZERO, 0.5f, 0.9f, 0.25f, 1.0f},
        // invdstalpha / dstalpha (MarioActorSpecialDraw), constant 0
        {"BLEND invdstalpha/dstalpha, const 0", 0, true, true, false, BF_INV_DST_ALPHA, BF_DST_ALPHA, 0.75f, 0.6f, 0.5f, 0.5f},
    };
    std::printf("destination alpha (colour, alpha as 0..255; GX wants the reference):\n");
    for (const DaCase& c : kDa) {
        float rc[1], ra;
        RefDa(c, rc, &ra);
        const DaOut o = DaRun(c, false), n = DaRun(c, true);
        std::printf("  %-42s ref rgb %3.0f a %3.0f | old rgb %3.0f a %3.0f | fixed rgb %3.0f a %3.0f\n", c.name, rc[0] * 255, ra * 255, o.rgb * 255, o.a * 255,
                    n.rgb * 255, n.a * 255);
        auto close = [](float x, float y) { return std::fabs(x - y) <= 1.5f / 255.f; };
        Check(close(n.rgb, rc[0]), "repaired colour follows the GX blend");
        Check(close(n.a, ra), "repaired alpha is the constant");
    }
    {
        const auto p = fixes::PlanDstAlpha(true, 128, true, true, true, false, BF_SRC_ALPHA, BF_INV_SRC_ALPHA);
        Check(p.unresolved && !p.fsConst && p.rsMode == 0, "constant with source-alpha factors and colour written is reported unresolved");
        const auto off = fixes::PlanDstAlpha(false, 128, true, true, true, false, BF_DST_ALPHA, BF_ONE);
        Check(!off.fsConst && off.rsMode == 0 && !off.dropBlend && !off.unresolved, "destination alpha off changes nothing");
        const auto noAlpha = fixes::PlanDstAlpha(true, 0, false, true, true, false, BF_DST_ALPHA, BF_ONE);
        Check(!noAlpha.fsConst && noAlpha.rsMode == 0, "alpha update off changes nothing");
    }

    // ---- 3. indirect add-previous
    {
        // Indirect texture (map 0): one texel, alpha byte 1 -> offset triple (1, 0, 0). Main texture (map 1): 16 x 1, red = 16 * texel.
        GLuint tInd = 0, tMain = 0;
        glGenTextures(1, &tInd); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tInd);
        const uint8_t ind[4] = {0, 0, 0, 1};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, ind);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenTextures(1, &tMain); glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, tMain);
        uint8_t row[16 * 4];
        for (int i = 0; i < 16; ++i) { row[i * 4] = static_cast<uint8_t>(16 * i); row[i * 4 + 1] = 0; row[i * 4 + 2] = 0; row[i * 4 + 3] = 255; }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, row);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        FsKey k;
        k.numStages = 2; k.numTexGens = 1; k.numIndStages = 1;
        k.ind[0].texMap = 0; k.ind[0].texCoord = 0;
        for (int s = 0; s < 2; ++s) {
            k.stage[s].texEnable = 1; k.stage[s].texMap = 1; k.stage[s].texCoord = 0;
            k.stage[s].color = 8u | (15u << 4) | (15u << 8) | (15u << 12) | (1u << 19);  // D = texture colour
            k.stage[s].alpha = (7u << 4) | (7u << 7) | (7u << 10) | (7u << 13) | (1u << 19);
        }
        // stage 0: matrix 0 (mid 1); stage 1: matrix 1 (mid 2), both coordinates wrapped to zero (sw = tw = 6), add previous (bit 20)
        k.stage[0].ind = 1u << 9;
        k.stage[1].ind = (2u << 9) | (6u << 13) | (6u << 16) | (1u << 20);
        ShaderSource src; GenerateFragmentShader(k, src);
        const bool hasItx = src.text.find("itx") != std::string::npos;
        Check(hasItx == expectIndPrev, "GXV_FIX_INDPREV selects the add-previous text");
        GLuint p = Program(k);
        glUseProgram(p);
        glUniform1i(glGetUniformLocation(p, "u_tex0"), 0); glUniform1i(glGetUniformLocation(p, "u_tex1"), 1);
        for (int i = 0; i < 28; ++i) Uni(p, i, 0, 0, 0, 0);
        Uni(p, 11, 0.5f, 0, 0, 1.f);   // matrix 0 row 0: offset.x = 0.5 * iv.x
        Uni(p, 13, 1.0f, 0, 0, 1.f);   // matrix 1 row 0: offset.x = 1.0 * iv.x
        Uni(p, 17, 1.f, 1.f, 0, 0);    // indirect coordinate scale of indirect stage 0
        Uni(p, 21, 1.f, 1.f, 16.f, 1.f); // texture sizes: map 0 = 1x1 (xy), map 1 = 16x1 (zw)
        glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        uint8_t px[4]; Px(px);
        // Pixel centre: coordinate 0.5 -> 8.0 texels. Stage 0: 8.0 + 0.5 = 8.5. Stage 1: wrapped to 0, + 1.0, + previous.
        // GX (texels): 0 + 1.0 + 8.5 = 9.5 -> texel 9 -> red 144. Old text: 0 + 1.0 + 8.5 / 16 = 1.53 -> texel 1 -> red 16.
        std::printf("indirect add-previous: red %u (GX: 144 = texel 9; the old text gives 16 = texel 1)\n", px[0]);
        Check(px[0] == (expectIndPrev ? 144 : 16), "add-previous stage sums texel coordinates");
        glDeleteProgram(p);
        // A key without add-previous keeps the exact old source under both settings.
        FsKey k2 = k; k2.stage[1].ind = (2u << 9);
        ShaderSource s2; GenerateFragmentShader(k2, s2);
        Check(s2.text.find("itx") == std::string::npos, "an indirect shader without add-previous keeps its source");
        std::printf("source hash (indirect, no add-previous): %016llx\n", static_cast<unsigned long long>(HashKey(s2.text.data(), s2.text.size())));
        glDeleteTextures(1, &tInd); glDeleteTextures(1, &tMain);
        glActiveTexture(GL_TEXTURE0);
    }

    // ---- 4. a stage that reads a switched-off texture
    {
        GLuint t = 0;
        glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
        const uint8_t gray[4] = {64, 64, 64, 255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, gray);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        unsigned out[2];
        for (int off = 0; off < 2; ++off) {
            FsKey k;
            k.numStages = 2; k.numTexGens = 1;
            k.stage[0].texEnable = 1; k.stage[0].texMap = 0; k.stage[0].texCoord = 0;
            k.stage[1].texOff = static_cast<uint8_t>(off);
            for (int s = 0; s < 2; ++s) {
                k.stage[s].color = 8u | (15u << 4) | (15u << 8) | (15u << 12) | (1u << 19);
                k.stage[s].alpha = (4u << 4) | (7u << 7) | (7u << 10) | (7u << 13) | (1u << 19); // D = texture alpha
            }
            GLuint p = Program(k);
            glUseProgram(p); glUniform1i(glGetUniformLocation(p, "u_tex0"), 0);
            for (int i = 0; i < 9; ++i) Uni(p, i, 0, 0, 0, 0);
            glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            uint8_t px[4]; Px(px); out[off] = px[0];
            glDeleteProgram(p);
        }
        std::printf("switched-off texture read in stage 1: old %u (the earlier stage's sample), fixed %u (white)\n", out[0], out[1]);
        Check(out[0] == 64 && out[1] == 255, "texOff reads white, otherwise the earlier sample");
        glDeleteTextures(1, &t);
    }

    Require(glGetError() == GL_NO_ERROR, "no GL errors");
    glDeleteBuffers(1, &vbo); glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &tex);
    if (g_bad) { std::fprintf(stderr, "FAIL: %d mismatches\n", g_bad); return 1; }
    std::printf("PASS\n");
    return 0;
}
