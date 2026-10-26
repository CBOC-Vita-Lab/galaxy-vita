// Final conversion only: integer-valued TEV outputs, not a full integer TEV emulator.
#include "vgx_shadergen.h"
#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vgx { bool LogEnabled() { return false; } }
namespace {
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
GLuint Compile(GLenum type, const std::string& text) {
    GLuint s = glCreateShader(type);
    const char* p = text.c_str(); glShaderSource(s, 1, &p, nullptr); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096] = {}; glGetShaderInfoLog(s, sizeof log, nullptr, log); std::fprintf(stderr, "%s\n%s\n", log, p); }
    Require(ok, "generated shader compiles"); return s;
}
GLuint Program(const vgx::FsKey& key) {
    vgx::ShaderSource src; vgx::GenerateFragmentShader(key, src);
    std::string vertex = "attribute vec2 a_pos; attribute vec4 a_col0, a_col1; varying vec4 v_col0, v_col1;\n";
    for (int i = 0; i < key.numTexGens; ++i) vertex += "varying vec3 v_tex" + std::to_string(i) + ";\n";
    vertex += "void main(){gl_Position=vec4(a_pos,0.0,1.0);v_col0=a_col0;v_col1=a_col1;\n";
    for (int i = 0; i < key.numTexGens; ++i) vertex += "v_tex" + std::to_string(i) + "=vec3(a_pos*0.25+0.5,1.0);\n";
    vertex += "}\n";
    GLuint vs = Compile(GL_VERTEX_SHADER, vertex);
    GLuint fs = Compile(GL_FRAGMENT_SHADER, src.text);
    GLuint p = glCreateProgram(); glAttachShader(p, vs); glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos"); glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok); Require(ok, "generated shader links");
    glDeleteShader(vs); glDeleteShader(fs); return p;
}
vgx::FsKey RecipeKey(const char* path, unsigned index) {
    struct Header { char magic[8]; uint32_t vs, fs, version, reserved; } h{};
    FILE* f = std::fopen(path, "rb"); Require(f, "open caller-supplied recipe");
    Require(std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, "VGXSHL01", 8) == 0 &&
            h.vs == sizeof(vgx::VsKey) && h.fs == sizeof(vgx::FsKey) && h.version >= 2 && h.version <= 4,
            "compatible read-only recipe layout");
    uint32_t type;
    while (std::fread(&type, sizeof type, 1, f) == 1) {
        if (type == 2) {
            vgx::FsKey key{}; Require(std::fread(&key, sizeof key, 1, f) == 1, "complete fragment record");
            if (index-- == 0) { std::fclose(f); return key; }
        } else {
            const size_t size = type == 1 ? sizeof(vgx::VsKey) : type == 3 ? 8 : type == 4 ? 16 : 0;
            unsigned char payload[sizeof(vgx::VsKey)]{};
            Require(size && std::fread(payload, size, 1, f) == 1, "complete known nonfragment record");
        }
    }
    Require(false, "requested fragment exists"); return {};
}
vgx::FsKey Pass(bool alphaTest) {
    vgx::FsKey k;
    k.stage[0].color = 2u | (15u << 4) | (15u << 8) | (15u << 12); // D=c0, rest zero
    k.stage[0].alpha = (1u << 4) | (7u << 7) | (7u << 10) | (7u << 13);
    k.alphaTestOn = alphaTest;
    k.alphaTest = 6u | (3u << 3); // >= ref0 AND <= ref1
    return k;
}
uint8_t Byte(int n, bool wrap) { return static_cast<uint8_t>(wrap ? (n & 255) : std::clamp(n, 0, 255)); }
uint64_t pixelDigest = 14695981039346656037ull;
void Render(GLuint p, const float c[4], uint8_t px[4], int ref0 = 128) {
    glUseProgram(p);
    glUniform4fv(glGetUniformLocation(p, "u_tev[1]"), 1, c);
    glUniform4fv(glGetUniformLocation(p, "u_tev[2]"), 1, c);
    glUniform4f(glGetUniformLocation(p, "u_tev[3]"), 0, 0, 0, -c[3]);
    glUniform4f(glGetUniformLocation(p, "u_tev[4]"), 0.5f, 0, 0, 0);
    glUniform4f(glGetUniformLocation(p, "u_tev[5]"), 0.75f, 0, 0, 0);
    glUniform4f(glGetUniformLocation(p, "u_tev[8]"), ref0, 255, 0, 0);
    glClearColor(17.0f / 255, 33.0f / 255, 49.0f / 255, 0); glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3); glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int ch = 0; ch < 4; ++ch) pixelDigest = (pixelDigest ^ px[ch]) * 1099511628211ull;
}
}
int main(int argc, char** argv) {
    Require(argc == 2 || (argc == 3 && std::strcmp(argv[2], "copy-probe") == 0) ||
            (argc == 5 && std::strcmp(argv[2], "key-probe") == 0), "expected mode and optional compiler probe");
    const bool wrap = std::strcmp(argv[1], "on") == 0;
    vgx::FsKey k = Pass(false);
    vgx::ShaderSource initial; vgx::GenerateFragmentShader(k, initial);
    const bool emitted = initial.text.find("vgx_wrap_final") != std::string::npos;
    Require(emitted == wrap, "switch selects final conversion");
    if (!wrap) Require(vgx::HashKey(initial.text.data(), initial.text.size()) == 0x228c375e37cc5759ull,
                       "off/default/invalid source is byte-identical to release base");
    // Changing the environment after first generation cannot change code within this process.
    setenv("GXV_TEVWRAP", wrap ? "0" : "1", 1);
    vgx::ShaderSource again; vgx::GenerateFragmentShader(k, again);
    Require(initial.text == again.text, "environment switch is read once");
    vgx::FsKey clamped = k;
    clamped.stage[0].color |= 1u << 19; clamped.stage[0].alpha |= 1u << 19;
    vgx::ShaderSource clampedSource; vgx::GenerateFragmentShader(clamped, clampedSource);
    Require(clampedSource.text.find("vgx_wrap_final") == std::string::npos,
            "fully clamped final stage needs no conversion helper");
    std::printf("source hash %016llx mode=%s\n", static_cast<unsigned long long>(vgx::HashKey(initial.text.data(), initial.text.size())), argv[1]);
    Require(vgx::host::CreateContext(), "Mesa GLES context");
    std::printf("GL renderer: %s\n", glGetString(GL_RENDERER));
    GLuint tex = 0, fbo = 0, vbo = 0;
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "RGBA8 target");
    const float verts[] = {-1,-1, 3,-1, -1,3};
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr); glEnableVertexAttribArray(0);
    glViewport(0, 0, 1, 1); glDisable(GL_DITHER); glDisable(GL_BLEND);
    if (argc >= 3) {
        auto copy = Pass(false); copy.numStages = 2;
        copy.stage[0].color |= 1u << 19; copy.stage[0].alpha |= 1u << 19;
        copy.stage[1].color = 0xfffu; copy.stage[1].alpha = 0x1ff0u;
        if (argc == 5) copy = RecipeKey(argv[3], static_cast<unsigned>(std::strtoul(argv[4], nullptr, 10)));
        const GLuint p = Program(copy);
        GLuint sampled = 0; glGenTextures(1, &sampled); glBindTexture(GL_TEXTURE_2D, sampled);
        const uint8_t rgba[] = {64, 191, 128, 255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glUseProgram(p);
        for (int i = 0; i < vgx::FsTevCount(copy); ++i) {
            const std::string name = "u_tev[" + std::to_string(i) + "]";
            glUniform4f(glGetUniformLocation(p, name.c_str()), 0.5f, 0.5f, 0.5f, 0.5f);
        }
        const float c[] = {0.25f, 0.75f, 0.5f, 1}; uint8_t px[4]; Render(p, c, px);
        if (argc == 3) Require(px[0] == 64 && px[1] == 191 && px[2] == 128 && px[3] == 255, "compiler probe output");
        Require(glGetError() == GL_NO_ERROR, "compiler probe GL status");
        std::printf("PASS: compiler probe pixel digest %016llx\n", static_cast<unsigned long long>(pixelDigest));
        glDeleteProgram(p); glDeleteBuffers(1, &vbo); glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &tex); glDeleteTextures(1, &sampled);
        return 0;
    }
    GLuint plain = Program(k), tested = Program(Pass(true));
    vgx::FsKey zeroTest = Pass(true); zeroTest.alphaTest = 4u | (3u << 3);
    GLuint nonzero = Program(zeroTest);
    size_t checked = 0;
    for (int n = -1024; n <= 1023; ++n) {
        const float c[] = {n / 255.0f, n / 255.0f, n / 255.0f, n / 255.0f};
        uint8_t px[4]; Render(plain, c, px);
        for (int ch = 0; ch < 4; ++ch) {
            if (px[ch] != Byte(n, wrap)) std::fprintf(stderr, "n=%d ch=%d got=%u want=%u\n", n, ch, px[ch], Byte(n, wrap));
            Require(px[ch] == Byte(n, wrap), "signed final output matches independent integer conversion"); ++checked;
        }
        Render(tested, c, px);
        const uint8_t a = Byte(n, wrap);
        Require(px[3] == (a >= 128 ? a : 0), "alpha comparison follows converted byte"); ++checked;
        if (a < 128) Require(px[0] == 17 && px[1] == 33 && px[2] == 49, "failed alpha test really discards RGB");
        Render(nonzero, c, px, 0);
        Require(px[3] == a, "strict greater-than-zero alpha boundary"); ++checked;
        if (a == 0) Require(px[0] == 17 && px[1] == 33 && px[2] == 49, "zero alpha really discards RGB");
    }
    // In-range fractional components remain exactly the old source expression, not quantized.
    for (int n = 0; n < 255; ++n) {
        const float c[] = {(n + 0.25f) / 255, (n + 0.75f) / 255, n / 255.0f, 1};
        uint8_t px[4]; Render(plain, c, px);
        Require(px[0] == n && px[1] == n + 1 && px[2] == n && px[3] == 255, "in-range fractions retain framebuffer conversion"); ++checked;
    }
    for (int colorClamped = 0; colorClamped <= 1; ++colorClamped) {
        vgx::FsKey mixed = Pass(false);
        // Color reads c1 and exits c2; alpha reads c2 and exits c0.
        mixed.stage[0].color = (mixed.stage[0].color & ~15u) | 4u | (3u << 22) | (colorClamped ? 1u << 19 : 0u);
        mixed.stage[0].alpha = (mixed.stage[0].alpha & ~(7u << 4)) | (3u << 4) | (1u << 22) | (colorClamped ? 0u : 1u << 19);
        GLuint p = Program(mixed);
        for (int n = -511; n <= 511; ++n) {
            const float c[] = {n / 255.0f, n / 255.0f, n / 255.0f, n / 255.0f};
            uint8_t px[4]; Render(p, c, px);
            Require(px[0] == Byte(n, wrap && !colorClamped) && px[3] == Byte(-n, wrap && colorClamped),
                    "independent final destination registers and color/alpha clamp bits"); ++checked;
        }
        glDeleteProgram(p);
    }
    // Synthetic three-stage lid-like alpha exit: K0*K0 + input*K1 + half bias.
    // Deliberately chosen binary-exact constants, not a copied game material or a full GX oracle.
    vgx::FsKey lid = Pass(false); lid.numStages = 3;
    lid.stage[1] = lid.stage[0]; lid.stage[2] = lid.stage[0];
    lid.stage[1].alpha = (7u << 4) | (6u << 7) | (6u << 10) | (7u << 13) | (1u << 19);
    lid.stage[1].kasel = 16; // K0.r = 0.5
    lid.stage[2].alpha = (0u << 4) | (6u << 7) | (1u << 10) | (7u << 13) | (1u << 16);
    lid.stage[2].kasel = 17; // K1.r = 0.75
    GLuint lidPlain = Program(lid);
    lid.alphaTestOn = 1; lid.alphaTest = 4u | (3u << 3);
    GLuint lidTested = Program(lid);
    for (int n = 0; n <= 255; ++n) {
        const float x = n / 256.0f;
        const float c[] = {x, x, x, x};
        const int expected = static_cast<int>(std::floor((0.75f + x * 0.75f) * 255.0f + 0.5f));
        uint8_t px[4]; Render(lidPlain, c, px);
        if (px[3] != Byte(expected, wrap)) std::fprintf(stderr, "lid n=%d got=%u want=%u expected=%d\n", n, px[3], Byte(expected, wrap), expected);
        Require(px[3] == Byte(expected, wrap), "three-stage synthetic alpha exit conversion"); ++checked;
        Render(lidTested, c, px, 0);
        Require(px[3] == Byte(expected, wrap), "three-stage alpha test consumes converted output"); ++checked;
        if (Byte(expected, wrap) == 0) Require(px[0] == 17 && px[1] == 33 && px[2] == 49,
                                             "three-stage wrapped zero alpha really discards RGB");
    }
    // Clamp writes establish independent RGB/alpha bounds. Later unclamped exact
    // copies retain fractions; neither destination order nor alpha testing changes that.
    for (unsigned source = 0; source < 4; ++source) for (unsigned dest = 0; dest < 4; ++dest) {
        for (bool fromD : {false, true}) {
            auto copy = Pass(false); copy.numStages = 2;
            copy.stage[0].color |= (source << 22) | (1u << 19);
            copy.stage[0].alpha |= (source << 22) | (1u << 19);
            copy.stage[1].color = fromD ? (15u << 12) | (15u << 8) | (15u << 4) | source * 2 :
                                                        (source * 2 << 12) | 0xfffu;
            copy.stage[1].alpha = fromD ? (7u << 13) | (7u << 10) | (7u << 7) | (source << 4) :
                                                        (source << 13) | 0x1ff0u;
            copy.stage[1].color |= dest << 22; copy.stage[1].alpha |= ((dest + 1) & 3) << 22;
            GLuint p = Program(copy);
            copy.alphaTestOn = 1; copy.alphaTest = 4u | (3u << 3);
            GLuint testedCopy = Program(copy);
            for (int n = -1024; n <= 1023; n += 17) {
                const float c[] = {n / 255.0f, n / 255.0f, n / 255.0f, n / 255.0f};
                uint8_t px[4]; Render(p, c, px);
                for (int ch = 0; ch < 4; ++ch) Require(px[ch] == Byte(n, false), "bounded copy preserves clamped signed source");
                Render(testedCopy, c, px, 0);
                const auto expected = Byte(n, false);
                Require(px[3] == expected, "bounded copy retains alpha-tested output");
                if (!expected) Require(px[0] == 17 && px[1] == 33 && px[2] == 49, "bounded zero copy really discards");
                ++checked;
            }
            for (int n = 0; n < 255; n += 3) {
                const float c[] = {(n + 0.25f) / 255, (n + 0.75f) / 255, n / 255.0f, 1};
                uint8_t px[4]; Render(p, c, px);
                Require(px[0] == n && px[1] == n + 1 && px[2] == n && px[3] == 255,
                        "bounded copy never quantizes in-range fractions"); ++checked;
            }
            glDeleteProgram(p); glDeleteProgram(testedCopy);
        }
    }
    Require(glGetError() == GL_NO_ERROR, "no GL errors");
    std::printf("PASS: %zu final-output/alpha checks\n", checked);
    std::printf("pixel digest %016llx\n", static_cast<unsigned long long>(pixelDigest));
    glDeleteProgram(plain); glDeleteProgram(tested); glDeleteBuffers(1, &vbo);
    glDeleteProgram(nonzero); glDeleteProgram(lidPlain); glDeleteProgram(lidTested);
    glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &tex);
}
