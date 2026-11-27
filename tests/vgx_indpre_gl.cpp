// GXV_IND_PRECOMP (gpu23b; vgx_shadergen.cpp IndPreMark / IndPreValues): an indirect stage's coordinate written as
// uv + (M . ind) * (1 / tsz) + c instead of (uv * tsz + M . (ind + b)) / tsz. A MINOR CHANGE (the same arithmetic up to float rounding):
// this test measures how far the pictures move. It reuses tests/vgx_indvs_gl.cpp's keys (Beach Bowl's sea and random J3D-shaped
// indirect keys), vertex keys, textures and draws (included below, its main renamed), real generated GLES2 shaders on Mesa; the
// marked shader's u_tev[27] is filled the way BuildTevC fills it (IndPreValues over the draw's own indirect matrices and sizes).
//   tools/test_vgx_indpre_gl.sh [output-dir] [pairs]
// Pass: the sea marked and its picture within 1 level (of 255) in every channel; at most 0.1% of all bytes different (materials that
// compare, wrap or alpha-test the warped sample can turn a one-level change into a large one in rare pixels: reported).
#include <GLES2/gl2.h>
#include <cstdio>
#include <vector>
#include "vgx_shadergen.h"
namespace indpre {
const vgx::FsKey* g_key = nullptr; // (the marked key being drawn: its u_tev gets [27])
float g_texSize = 0;               // the indirect stages' texture size in the TEV block (IP_TSZ, default 37: not a power of two)
void Uniform4fv(GLint loc, GLsizei n, const GLfloat* v) {
    GLint prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    if (n == 28 && prog && loc == glGetUniformLocation(static_cast<GLuint>(prog), "u_tev")) {
        std::vector<float> t(v, v + 28 * 4);
        for (int i = 21 * 4; i < 25 * 4; ++i) t[i] = g_texSize; // (both programs: the same sizes)
        if (g_key) vgx::IndPreValues(*g_key, &t[11 * 4], g_texSize, g_texSize, &t[27 * 4]);
        glUniform4fv(loc, n, t.data());
        return;
    }
    glUniform4fv(loc, n, v);
}
}
#define glUniform4fv indpre::Uniform4fv
#define main indvs_main
#include "vgx_indvs_gl.cpp"
#undef main
#undef glUniform4fv
int main(int argc, char** argv) {
    const unsigned pairs = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 200;
    Require(vgx::host::CreateContext(), "GLES context");
    GLuint fbo = 0, tex = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "framebuffer");
    glViewport(0, 0, kW, kH);
    indpre::g_texSize = std::getenv("IP_TSZ") ? static_cast<float>(std::atof(std::getenv("IP_TSZ"))) : 37.0f;
    fshalf_keys::Rng r(0x1D5EA);
    unsigned marked = 0, unmarked = 0, pairsDiff = 0;
    uint64_t pixels = 0, bytesDiff = 0;
    int maxDiff = 0, seaMax = 0;
    bool seaMarked = false;
    for (unsigned i = 0; i < pairs; ++i) {
        const bool sea = i == 0;
        vgx::FsKey fs = sea ? SeaKey() : RandomIndKey(r);
        vgx::VsKey vs = sea ? SeaVs() : RandomVs(r, fs);
        uint32_t bp25 = 0, bp26 = 0;
        float sc[4][2];
        RandomScales(r, fs, bp25, bp26, sc);
        if (sea) { bp25 = 0x33; sc[0][0] = sc[0][1] = 0.125f; }
        vgx::TexVec2Pair(fs, vs);
        if (vgx::IndVsMode()) vgx::IndVsPair(fs, vs, bp25, bp26);
        vgx::FsKey fOn = fs;
        if (!vgx::IndPreMark(fOn)) { ++unmarked; continue; }
        ++marked;
        if (sea) seaMarked = true;
        vgx::ShaderSource vS, f0S, f1S;
        vgx::GenerateVertexShader(vs, vS);
        vgx::GenerateFragmentShader(fs, f0S);
        vgx::GenerateFragmentShader(fOn, f1S);
        Require(f1S.text.find("u_tev[27]") != std::string::npos, "the marked stage reads u_tev[27]");
        if (sea && std::getenv("VGX_TEST_VERBOSE")) std::printf("---- sea fs (marked)\n%s\n", f1S.text.c_str());
        const GLuint v = Compile(GL_VERTEX_SHADER, vS.text), f0 = Compile(GL_FRAGMENT_SHADER, f0S.text), f1 = Compile(GL_FRAGMENT_SHADER, f1S.text);
        const GLuint p0 = Link(v, f0, vS.text, f0S.text), p1 = Link(v, f1, vS.text, f1S.text);
        bool diff = false;
        for (uint32_t round = 0; round < 3; ++round) {
            std::vector<uint8_t> a, b;
            const uint32_t seed = i * 7919u + round * 104729u + 17u;
            indpre::g_key = nullptr;
            Draw(p0, vS, f0S.nTev, seed, sc, a);
            indpre::g_key = &fOn;
            Draw(p1, vS, f1S.nTev, seed, sc, b);
            indpre::g_key = nullptr;
            for (size_t j = 0; j < a.size(); ++j) {
                const int d = std::abs(static_cast<int>(a[j]) - static_cast<int>(b[j]));
                if (d) { ++bytesDiff; diff = true; }
                maxDiff = std::max(maxDiff, d);
                if (sea) seaMax = std::max(seaMax, d);
            }
            pixels += kW * kH;
        }
        pairsDiff += diff ? 1u : 0u;
        glDeleteProgram(p0); glDeleteProgram(p1); glDeleteShader(v); glDeleteShader(f0); glDeleteShader(f1);
    }
    std::printf("GXV_IND_PRECOMP=%ld GXV_IND_VS=%ld, indirect texture size %g: %u pairs marked (sea %s), %u not; %llu pixels compared: %llu bytes differ (%.4f%%) in %u pairs, "
                "largest difference %d (the sea: %d)\n", vgx::IndPreMode(), vgx::IndVsMode(), indpre::g_texSize, marked, seaMarked ? "yes" : "NO", unmarked,
                static_cast<unsigned long long>(pixels), static_cast<unsigned long long>(bytesDiff), 100.0 * static_cast<double>(bytesDiff) / (4.0 * pixels), pairsDiff,
                maxDiff, seaMax);
    Require(seaMarked, "the sea's indirect stage is marked");
    // (a material that compares, wraps or alpha-tests the warped sample turns a one-level change of a rare pixel into a large one: counted
    // and reported, not refused; the sea adds the warped screen to its colour, so it must stay within one level)
    Require(seaMax <= 1, "the sea's picture within one level");
    Require(bytesDiff * 1000 <= 4 * pixels, "at most 0.1% of the bytes of all pictures differ");
    return 0;
}
