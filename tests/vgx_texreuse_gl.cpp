// Host test of GXV_FS_TEXREUSE (gpucut23; platform/vgx/src/vgx_shadergen.cpp GenerateFragmentShaderAs): a TEV stage whose texture read is
// the very statement that made the value `tex` still holds does not read again. No game data or Vita executable needed.
//   tools/test_vgx_texreuse_gl.sh [output-dir] [keys] [key-dir]
// For every key (random keys of every shape, the same keys with each stage's read copied from the stage before - the repeated read the
// switch is for -, and, if a directory of FsKey files is given, those keys: tools/test_vgx_texreuse_gl.sh takes Honeyhive's 131):
//  1. the text with the switch on is the text with it off less exactly ShaderSource::texReused lines, each a `tex = ...` read equal to the
//     `tex` statement before it with no other `tex` statement between (checked on the text itself);
//  2. both texts draw random triangles on Mesa (random registers, konst, vertex colours, 8x8 RGBA textures, coordinates, LOD bias values)
//     and the RGBA8 read-backs are compared byte for byte: any difference fails (the same value is read: exact);
//  3. a key without a repeated read has the same text with the switch on and off (texReused 0).
#include "vgx_shadergen.h"
#include "vgx_fshalf_keys.h"
#include <GLES2/gl2.h>
#include <dirent.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sstream>
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
    if (!ok) { char log[4096] = {}; glGetProgramInfoLog(p, sizeof log, nullptr, log); std::fprintf(stderr, "%s\n---- vs\n%s\n---- fs\n%s\n", log, vt.c_str(), ft.c_str()); }
    Require(ok, "program links");
    glDeleteShader(v); glDeleteShader(f);
    return p;
}
// A vertex shader that feeds every varying the fragment text declares (its own types: vec2 / vec3 texgens, colours, fog).
std::string VertexFor(const std::string& fs) {
    std::string decl, body;
    std::istringstream in(fs);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (line.rfind("varying ", 0) != 0) continue;
        std::istringstream w(line.substr(8));
        std::string type, name;
        w >> type >> name;
        if (!name.empty() && name.back() == ';') name.pop_back();
        const char last = type.empty() ? 'x' : type.back();
        const char* glType = last == '4' ? "vec4" : last == '3' ? "vec3" : last == '2' ? "vec2" : "float";
        decl += std::string("varying ") + glType + " " + name + ";\n";
        static const char* const tex3[4] = {"a_tex", "a_tex.yxz", "a_tex * 0.5", "a_tex.xzy"};
        if (last == '4') body += "  " + name + " = " + (name.find("col1") != std::string::npos ? "a_col1" : "a_col0") + ";\n";
        else if (last == '3') body += "  " + name + " = " + tex3[n++ & 3] + ";\n";
        else if (last == '2') body += "  " + name + " = (" + tex3[n++ & 3] + ").xy;\n";
        else body += "  " + name + " = a_tex.z;\n";
    }
    return "attribute vec4 a_pos; attribute vec4 a_col0; attribute vec4 a_col1; attribute vec3 a_tex;\n" + decl +
           "void main() { gl_Position = a_pos;\n" + body + "}\n";
}
constexpr int kW = 32, kH = 32;
struct FVtx { float pos[4], c0[4], c1[4], t[3]; };
GLuint g_tex[8];
float Sample(Rng& r) {
    const unsigned w = r.U(100);
    if (w < 10) return 0.0f;
    if (w < 20) return 1.0f;
    if (w < 55) return static_cast<float>(r.U(256)) / 255.0f;
    return static_cast<float>(r.F());
}
void DrawFs(GLuint p, uint32_t seed, std::vector<uint8_t>& px) {
    std::mt19937 rng(seed);
    Rng r(seed);
    glUseProgram(p);
    std::vector<float> tev(28 * 4, 0.0f);
    for (int i = 0; i < 16; ++i) tev[i] = r.P(20) ? static_cast<float>(static_cast<int>(r.U(2048)) - 1024) / 255.0f : static_cast<float>(r.U(256)) / 255.0f;
    for (int i = 16; i < 32; ++i) tev[i] = static_cast<float>(r.U(256)) / 255.0f;
    tev[32] = static_cast<float>(r.U(256)); tev[33] = static_cast<float>(r.U(256)); tev[34] = static_cast<float>(r.U(256)) / 255.0f;
    for (int i = 36; i < 44; ++i) tev[i] = static_cast<float>(r.F());
    for (int i = 44; i < 68; ++i) tev[i] = static_cast<float>(r.F()) * 2.0f - 1.0f;
    for (int i = 68; i < 84; ++i) tev[i] = 1.0f;
    for (int i = 84; i < 100; ++i) tev[i] = 8.0f;
    for (int i = 100; i < 112; ++i) tev[i] = static_cast<float>(r.F()) * 2.0f - 1.0f; // (LOD bias and the rest of the block: any value)
    if (GLint l = glGetUniformLocation(p, "u_tev"); l >= 0) {
        GLint n = 0;
        glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &n);
        glUniform4fv(l, 28, tev.data());
    }
    for (int t = 0; t < 8; ++t) {
        uint8_t texel[8 * 8 * 4];
        for (uint8_t& b : texel) b = static_cast<uint8_t>(r.P(15) ? (r.P(50) ? 0 : 255) : rng());
        glActiveTexture(GL_TEXTURE0 + t);
        glBindTexture(GL_TEXTURE_2D, g_tex[t]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
        glGenerateMipmap(GL_TEXTURE_2D); // (LOD-bias reads choose among the levels)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (t & 1) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        char name[16];
        std::snprintf(name, sizeof name, "u_tex%d", t);
        if (GLint l = glGetUniformLocation(p, name); l >= 0) glUniform1i(l, t);
    }
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
std::vector<std::string> Lines(const std::string& t) {
    std::vector<std::string> v;
    std::istringstream in(t);
    std::string l;
    while (std::getline(in, l)) v.push_back(l);
    return v;
}
// 1: on = off less exactly `reused` lines, each a `tex = ` line equal to the last `tex = ` line before it in off's main.
bool TextRule(const std::string& off, const std::string& on, unsigned reused) {
    const std::vector<std::string> a = Lines(off), b = Lines(on);
    if (a.size() != b.size() + reused) return false;
    size_t j = 0;
    unsigned gone = 0;
    std::string lastTex;
    for (size_t i = 0; i < a.size(); ++i) {
        const bool isTex = a[i].rfind("  tex = ", 0) == 0;
        if (j < b.size() && a[i] == b[j]) {
            ++j;
            if (isTex) lastTex = a[i];
            continue;
        }
        if (!isTex || a[i] != lastTex || a[i].find("itc") != std::string::npos) return false; // (only a repeat of the read `tex` holds)
        ++gone;
    }
    return j == b.size() && gone == reused;
}
struct Tally { size_t keys = 0, reusedKeys = 0, reads = 0, bytes = 0, diff = 0, sameText = 0; };
void CheckKey(const vgx::FsKey& k, uint32_t seed, Tally& t, const char* what) {
    vgx::ShaderSource off, on;
    vgx::FsTexReuseModeForTest(0);
    vgx::GenerateFragmentShader(k, off);
    vgx::FsTexReuseModeForTest(1);
    vgx::GenerateFragmentShader(k, on);
    vgx::FsTexReuseModeForTest(-1);
    ++t.keys;
    Require(off.texReused == 0, "nothing is left out with the switch off");
    if (!TextRule(off.text, on.text, on.texReused)) {
        std::fprintf(stderr, "%s: text rule broken (texReused %u)\n---- off\n%s\n---- on\n%s\n", what, on.texReused, off.text.c_str(), on.text.c_str());
        Require(false, "the text with the switch is the text without it less the repeated reads");
    }
    if (on.texReused == 0) {
        Require(on.text == off.text, "a key without a repeated read keeps its text");
        ++t.sameText;
        return;
    }
    ++t.reusedKeys;
    t.reads += on.texReused;
    const GLuint p0 = Link(VertexFor(off.text), off.text), p1 = Link(VertexFor(on.text), on.text);
    for (uint32_t round = 0; round < 3; ++round) {
        std::vector<uint8_t> x, y;
        const uint32_t s = seed * 7919u + round * 104729u + 11u;
        DrawFs(p0, s, x);
        DrawFs(p1, s, y);
        for (size_t j = 0; j < x.size(); ++j) t.diff += x[j] != y[j];
        t.bytes += x.size();
        if (x != y) {
            std::fprintf(stderr, "%s: pictures differ\n---- off\n%s\n---- on\n%s\n", what, off.text.c_str(), on.text.c_str());
            Require(false, "byte-identical pictures");
        }
    }
    glDeleteProgram(p0); glDeleteProgram(p1);
}
} // namespace

int main(int argc, char** argv) {
    const unsigned nKeys = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 1500;
    const char* keyDir = argc > 2 ? argv[2] : nullptr;
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
    Tally rnd, rep, dir;
    Rng r(0x7E1C5u);
    for (unsigned i = 0; i < nKeys; ++i) {
        vgx::FsKey k = fshalf_keys::RandomKey(r, true);
        CheckKey(k, i, rnd, "random key");
        // the same key with reads repeated: each stage after the first takes the stage before's read (map, coordinate, swap, bias,
        // indirect stage) with probability 1/2, or switches its read off between two equal reads (tex keeps its value there)
        for (int s = 1; s < k.numStages; ++s) {
            if (!r.P(50)) continue;
            vgx::FsKey::Stage& a = k.stage[s];
            const vgx::FsKey::Stage& b = k.stage[s - 1];
            a.texEnable = b.texEnable; a.texMap = b.texMap; a.texCoord = b.texCoord; a.texSwap = b.texSwap; a.lodBias = b.lodBias;
            a.texOff = b.texOff;
            if (r.P(30)) a.ind = b.ind;
            if (r.P(10)) { a.texEnable = 0; a.texOff = 0; } // (no read here: tex keeps its value for the stage after)
        }
        CheckKey(k, i + 100000u, rep, "repeated reads");
    }
    if (keyDir) {
        DIR* d = opendir(keyDir);
        Require(d != nullptr, "key directory");
        std::vector<std::string> names;
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".key") == 0) names.push_back(n);
        }
        closedir(d);
        std::sort(names.begin(), names.end());
        uint32_t seed = 200000;
        for (const std::string& n : names) {
            FILE* f = std::fopen((std::string(keyDir) + "/" + n).c_str(), "rb");
            vgx::FsKey k;
            const bool ok = f && std::fread(&k, sizeof k, 1, f) == 1;
            if (f) std::fclose(f);
            if (!ok) continue;
            CheckKey(k, seed++, dir, n.c_str());
        }
    }
    auto Print = [](const char* what, const Tally& t) {
        std::printf("%s: %zu keys, %zu with a repeated read (%zu reads left out), %zu unchanged texts; %zu bytes compared, %zu differ\n", what, t.keys,
                    t.reusedKeys, t.reads, t.sameText, t.bytes, t.diff);
    };
    Print("random keys", rnd);
    Print("random keys with repeated reads", rep);
    if (keyDir) Print("key directory", dir);
    Require(rep.reusedKeys > 0, "the repeated-read keys exercise the switch");
    std::printf("PASS\n");
    return 0;
}
