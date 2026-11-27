// Host proof of GXV_UBERSHADER stage 1 (platform/vgx/src/vgx_ubershader.{h,cpp}): the ubershader of a key's interface, configured by UberFsConfig, draws
// the same pixels as the key's own text. No game data or Vita executable needed; a real key list can be given.
//   tools/test_vgx_ubershader_gl.sh [output-dir] [random-keys] [key-list ...]
//
// For every key the ubershader takes (UberFsCovers), four programs draw the same 8 random triangles (random TEV registers -- outside 0..255 for a marked
// key --, konst colours, indirect matrices, fog parameters, 8x8 textures, vertex colours, texture coordinates; a vertex shader written for the
// interface), 3 rounds of seeds, RGBA8 read back:
//   U  the ubershader (GenerateUberFragmentShader + u_ucfg from UberFsConfig + the same u_tev block)
//   W  the key's full float text (GXV_FS_TRIM parts 0, float: GenerateFragmentShaderParts(k, half = false, 0)) with its texture reads behind a function
//      with control flow, as the ubershader's are (Wrapped: Mesa filters such reads by another path; see there)
//   F  the same text as it is;  S  the key's text as this run ships it (trimmed, half where FsHalfEligible takes it: a plain float on a PC)
// THE PROOF is U against W: at most one level, at most 1 in 10,000 bytes (measured: byte for byte). U against F and S: at most one level and 5 in 10,000
// bytes for keys without an indirect stage (Mesa folds the constants a key's text spells out, mix(a, vec3(1.0), c), and contracts differently: an ulp that
// now and then crosses a byte); keys with one are reported (Mesa's two filtering paths, amplified through the indirect offset). Keys from a list are made
// canonical for this run first (CanonListFsKey), as the game does.
// Census: keys taken / not taken by reason, distinct interfaces (the family's program count), the ubershader's text size, the longest key text.
#include "vgx_shadergen.h"
#include "vgx_ubershader.h"
#include "vgx_fshalf_keys.h"
#include <GLES2/gl2.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace vgx { bool LogEnabled() { return false; } namespace host { bool CreateContext(); } }
namespace {
using fshalf_keys::Rng;
using vgx::FsKey;
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
GLuint Compile(GLenum type, const std::string& text) {
    GLuint s = glCreateShader(type);
    const char* p = text.c_str(); glShaderSource(s, 1, &p, nullptr); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096] = {}; glGetShaderInfoLog(s, sizeof log, nullptr, log); std::fprintf(stderr, "%s\n%s\n", log, p); }
    Require(ok, "shader compiles");
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
// The vertex shader of an interface: every varying the fragment side may declare, from a few attributes (vec2 varyings get the xy of what a vec3 one would)
std::string IfaceVs(const vgx::UberIface& f) {
    std::string s = "attribute vec4 a_pos; attribute vec4 a_col0; attribute vec4 a_col1; attribute vec3 a_tex;\n";
    char b[160];
    if (f.col & 1) s += "varying vec4 v_col0;\n";
    if (f.col & 2) s += "varying vec4 v_col1;\n";
    for (int i = 0; i < f.numTexGens; ++i) { std::snprintf(b, sizeof b, (f.vec2Mask >> i) & 1u ? "varying vec2 v_tex%d;\n" : "varying vec3 v_tex%d;\n", i); s += b; }
    if (f.fogVtx) { std::snprintf(b, sizeof b, "varying float %s;\n", vgx::FogVaryingName()); s += b; }
    s += "void main() { gl_Position = a_pos;\n";
    if (f.col & 1) s += "  v_col0 = a_col0;\n";
    if (f.col & 2) s += "  v_col1 = a_col1;\n";
    static const char* const kTc[8] = {"a_tex", "a_tex.yxz", "a_tex * 0.5", "a_tex.xzy", "vec3(a_tex.x * 3.0, a_tex.y - 0.25, a_tex.z)", "a_tex.zyx", "-a_tex", "a_tex * 2.0"};
    for (int i = 0; i < f.numTexGens; ++i) { std::snprintf(b, sizeof b, (f.vec2Mask >> i) & 1u ? "  v_tex%d = (%s).xy;\n" : "  v_tex%d = %s;\n", i, kTc[i]); s += b; }
    if (f.fogVtx) { std::snprintf(b, sizeof b, "  %s = a_tex.z * 1.5 - 0.25;\n", vgx::FogVaryingName()); s += b; }
    s += "}\n";
    return s;
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
float RegSample(Rng& r, bool wide) {
    if (!wide) return static_cast<float>(r.P(20) ? (r.P(50) ? 0 : 255) : r.U(256)) / 255.0f;
    const unsigned w = r.U(100);
    const int v = w < 10 ? -1024 : w < 20 ? 1023 : w < 40 ? 256 + static_cast<int>(r.U(768)) : w < 60 ? -1 - static_cast<int>(r.U(1023))
                                                                                                    : static_cast<int>(r.U(2048)) - 1024;
    return static_cast<float>(v) / 255.0f;
}
// One draw of program p: the same seed gives the same uniforms, textures and triangles. cfg: u_ucfg (the ubershader only).
void Draw(GLuint p, const FsKey& k, uint32_t seed, const float* cfg, std::vector<uint8_t>& px) {
    std::mt19937 rng(seed);
    Rng r(seed);
    glUseProgram(p);
    std::vector<float> tev(28 * 4, 0.0f);
    const unsigned wideMask = (k.inWrap & 1) ? r.U(16) : 0u;
    for (int i = 0; i < 16; ++i) tev[i] = RegSample(r, ((wideMask >> (i / 4)) & 1u) != 0);
    for (int i = 0; i < 16; ++i) // (GXV_TEVIN_CANON=2: a pre-wrapped part is uploaded as its low byte / 255, BuildTevC)
        if ((k.preWrap() >> ((i & 3) < 3 ? i / 4 : i / 4 + 4)) & 1u) tev[i] = static_cast<float>(static_cast<int>(std::lround(tev[i] * 255.0f)) & 255) / 255.0f;
    for (int i = 16; i < 32; ++i) tev[i] = static_cast<float>(r.U(256)) / 255.0f;
    tev[32] = static_cast<float>(r.U(256)); tev[33] = static_cast<float>(r.U(256)); tev[34] = static_cast<float>(r.U(256)) / 255.0f;
    for (int i = 36; i < 44; ++i) tev[i] = static_cast<float>(r.F());
    for (int i = 44; i < 68; ++i) tev[i] = static_cast<float>(r.F()) * 2.0f - 1.0f;
    for (int i = 68; i < 84; ++i) tev[i] = r.P(50) ? 1.0f : 0.5f;
    for (int i = 84; i < 100; ++i) tev[i] = 8.0f;
    for (int i = 100; i < 108; ++i) tev[i] = static_cast<float>(r.F()) * 2.0f - 1.0f;
    for (int i = 108; i < 112; ++i) tev[i] = static_cast<float>(r.F());
    if (GLint l = glGetUniformLocation(p, "u_tev"); l >= 0) glUniform4fv(l, 28, tev.data());
    if (cfg) { const GLint l = glGetUniformLocation(p, "u_ucfg"); Require(l >= 0, "u_ucfg is active"); glUniform4fv(l, vgx::kUberRows, cfg); }
    for (int t = 0; t < 8; ++t) {
        uint8_t texel[8 * 8 * 4];
        for (uint8_t& b : texel) b = static_cast<uint8_t>(r.P(15) ? (r.P(50) ? 0 : 255) : rng());
        glActiveTexture(GL_TEXTURE0 + t);
        glBindTexture(GL_TEXTURE_2D, g_tex[t]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        char name[16];
        std::snprintf(name, sizeof name, "u_tex%d", t);
        if (GLint l = glGetUniformLocation(p, name); l >= 0) glUniform1i(l, t);
    }
    std::vector<FVtx> v(8 * 3);
    for (FVtx& x : v) {
        x.pos[0] = static_cast<float>(r.F()) * 2.4f - 1.2f; x.pos[1] = static_cast<float>(r.F()) * 2.4f - 1.2f;
        x.pos[2] = static_cast<float>(r.F()) * 1.6f - 0.8f; x.pos[3] = 1.0f;
        for (float& f : x.c0) f = Sample(r);
        for (float& f : x.c1) f = Sample(r);
        x.t[0] = static_cast<float>(r.F()) * 4.0f - 2.0f; x.t[1] = static_cast<float>(r.F()) * 4.0f - 2.0f;
        x.t[2] = r.P(5) ? 0.0f : 0.5f + static_cast<float>(r.F());
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
struct Cmp { size_t keys = 0, bytes = 0, diff = 0, diffPx = 0; int maxd = 0; size_t hist[4] = {}; }; // hist: bytes 1, 2, 3, 4+ levels apart
void Diff(const std::vector<uint8_t>& x, const std::vector<uint8_t>& y, Cmp& c) {
    for (size_t j = 0; j < x.size(); j += 4) {
        int d = 0;
        for (int ch = 0; ch < 4; ++ch) { const int e = std::abs(static_cast<int>(x[j + ch]) - y[j + ch]); d = std::max(d, e); c.diff += e != 0; if (e) ++c.hist[std::min(e, 4) - 1]; }
        if (d) ++c.diffPx;
        c.maxd = std::max(c.maxd, d);
    }
    c.bytes += x.size();
}
std::string Hex(const FsKey& k) {
    std::string s;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&k);
    char h[3];
    for (size_t i = 0; i < sizeof k; ++i) { std::snprintf(h, sizeof h, "%02x", b[i]); s += h; }
    return s;
}
bool ReadList(const char* path, std::vector<FsKey>& out, std::vector<uint32_t>* pairFs = nullptr) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    struct Hdr { char magic[8]; uint32_t vs, fs, gen, res; } h;
    bool ok = std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, "VGXSHL01", 8) == 0 && h.vs == sizeof(vgx::VsKey) && h.fs == sizeof(FsKey) &&
              h.gen == vgx::kShaderGenVersion;
    uint32_t type;
    while (ok && std::fread(&type, 4, 1, f) == 1) {
        if (type == 1) { vgx::VsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; }
        else if (type == 2) { FsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; out.push_back(k); }
        else if (type == 3) { uint32_t v[2]; if (std::fread(v, 8, 1, f) != 1) break; if (pairFs) pairFs->push_back(v[1]); }
        else if (type == 4) { uint32_t v[4]; if (std::fread(v, 16, 1, f) != 1) break; if (pairFs) pairFs->push_back(v[1] | 0x80000000u); }
        else break;
    }
    std::fclose(f);
    return ok;
}

struct Run {
    Cmp uw, uf, us, ufInd, usInd; // (uw: the proof; uf / us: keys without an indirect stage; *Ind: with one)
    std::map<std::string, unsigned> notTaken;
    std::set<uint32_t> ifaces;
    size_t keys = 0, taken = 0, uberLen = 0, keyLenMax = 0;
    std::set<std::string> keysDiff;
    std::map<std::string, size_t> keyDiffBytes;

    // (memory: llvmpipe keeps every linked program's code; the keys come grouped by interface and only the current interface's ubershader is alive)
    uint32_t uberWord = 0xFFFFFFFFu;
    GLuint uberProg = 0;
};
// The key's text with every texture2D read behind a function that has control flow, as the ubershader's reads are. Mesa's llvmpipe filters a read in such a
// function by a different path from a straight-line read (the same coordinate, a different sub-level value: up to 0.006 of a level measured); an indirect stage turns
// that into a different coordinate for its texture (tens of levels where the texture changes fast). The console's texture unit has one filtering path.
std::string Wrapped(std::string t) {
    for (size_t at = 0; (at = t.find("texture2D(", at)) != std::string::npos; at += 8) t.replace(at, 10, "vgx_t2d(");
    const size_t m = t.find("uniform vec4 u_tev[");
    Require(m != std::string::npos, "a key text declares u_tev");
    t.insert(t.find('\n', m) + 1, "vec4 vgx_t2d(sampler2D s, vec2 c) { if (u_tev[0].x > 1e30) return vec4(0.0); return texture2D(s, c); }\n"
                                 "vec4 vgx_t2d(sampler2D s, vec2 c, float b) { if (u_tev[0].x > 1e30) return vec4(0.0); return texture2D(s, c, b); }\n"
                                 "uniform float vgx_z;\n");
    // The literals of main() as values the compiler cannot see (+ vgx_z, a uniform left at 0.0: x + 0.0 is x): the ubershader reads the same constants from
    // uniforms, so Mesa folds neither (a key's own text lets it rewrite mix(a, vec3(1.0), c) or a sum with 0.5 into other roundings: an ulp at a byte boundary).
    const size_t body = t.find("void main()");
    std::string out = t.substr(0, body);
    for (size_t i = body; i < t.size();) {
        const bool start = std::isdigit(static_cast<unsigned char>(t[i])) && (i == 0 || !(std::isalnum(static_cast<unsigned char>(t[i - 1])) || t[i - 1] == '_' || t[i - 1] == '.'));
        size_t j = i;
        if (start) { while (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j]))) ++j; }
        if (start && j < t.size() && t[j] == '.') {
            ++j;
            while (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j]))) ++j;
            out += "(" + t.substr(i, j - i) + " + vgx_z)";
            i = j;
        } else {
            out += t[i++];
        }
    }
    return out;
}
void One(const FsKey& k, Run& run, uint32_t seedBase) {
    static const char* only = std::getenv("GXV_UBER_TEST_ONLY"); // (debugging: only the keys whose hex starts so; their first differing pixels are printed)
    if (only && Hex(k).compare(0, std::strlen(only), only) != 0) return;
    ++run.keys;
    const char* why = "";
    if (!vgx::UberFsCovers(k, &why)) { ++run.notTaken[why]; return; }
    ++run.taken;
    const vgx::UberIface f = vgx::UberIfaceOf(k);
    run.ifaces.insert(f.Word());
    std::vector<float> cfg(4 * vgx::kUberRows);
    Require(vgx::UberFsConfig(k, cfg.data()), "a covered key has a configuration");
    const std::string vt = IfaceVs(f);
    if (!run.uberProg || run.uberWord != f.Word()) {
        if (run.uberProg) glDeleteProgram(run.uberProg);
        vgx::ShaderSource u;
        vgx::GenerateUberFragmentShader(f, u);
        run.uberLen = std::max(run.uberLen, u.text.size());
        run.uberProg = Link(vt, u.text);
        run.uberWord = f.Word();
    }
    const GLuint uberProg = run.uberProg;
    vgx::ShaderSource full, ship;
    vgx::GenerateFragmentShaderParts(k, full, false, 0);
    vgx::GenerateFragmentShader(k, ship);
    run.keyLenMax = std::max(run.keyLenMax, full.text.size());
    const std::string wrapped = Wrapped(full.text);
    const GLuint pw = Link(vt, wrapped), pf = Link(vt, full.text), ps = Link(vt, ship.text);
    const bool indirect = k.numIndStages > 0;
    for (uint32_t round = 0; round < 3; ++round) {
        const uint32_t seed = seedBase * 7919u + round * 104729u + 11u;
        std::vector<uint8_t> xu, xw, xf, xs;
        Draw(uberProg, k, seed, cfg.data(), xu);
        Draw(pw, k, seed, nullptr, xw);
        Draw(pf, k, seed, nullptr, xf);
        Draw(ps, k, seed, nullptr, xs);
        // 1. the proof: the ubershader against the key's full text with its reads in the same shape (Wrapped)
        Cmp w;
        Diff(xu, xw, w);
        if (only)
            for (size_t j = 0, n = 0; j < xu.size() && n < 6; j += 4)
                if (std::memcmp(&xu[j], &xw[j], 4)) { ++n; std::printf("  px %zu: uber %u %u %u %u full %u %u %u %u\n", j / 4, xu[j], xu[j + 1], xu[j + 2], xu[j + 3], xw[j], xw[j + 1], xw[j + 2], xw[j + 3]); }
        if (w.maxd > 1) {
            std::fprintf(stderr, "ubershader vs full text (reads wrapped): key %s, %zu bytes differ (max %d levels)\n---- full\n%s\n", Hex(k).c_str(), w.diff, w.maxd, wrapped.c_str());
            Require(false, "the ubershader draws the key's full float text (reads in the same shape): at most one level");
        }
        if (w.diff) { run.keysDiff.insert(Hex(k)); run.keyDiffBytes[Hex(k)] += w.diff; }
        Diff(xu, xw, run.uw);
        // 2. against the key's own texts as they are (statistics; one level at most for a key without an indirect stage)
        Cmp one, two;
        Diff(xu, xf, one);
        Diff(xu, xs, two);
        Diff(xu, xf, indirect ? run.ufInd : run.uf);
        Diff(xu, xs, indirect ? run.usInd : run.us);
        if (!indirect && (one.maxd > 1 || two.maxd > 1)) {
            std::fprintf(stderr, "ubershader vs the key's text: key %s, %d / %d levels\n---- full\n%s\n", Hex(k).c_str(), one.maxd, two.maxd, full.text.c_str());
            Require(false, "without an indirect stage the ubershader is within one level of the key's full and shipped texts");
        }
    }
    glDeleteProgram(pw);
    glDeleteProgram(pf);
    glDeleteProgram(ps);
}
// The keys in interface order (stable: the same seeds per key whatever the order), the taken ones first; at most `cap` of them (0 = all), spread over the list.
void RunKeys(const std::vector<FsKey>& in, Run& run, uint32_t seedBase, size_t cap) {
    std::vector<std::pair<uint32_t, size_t>> order;
    for (size_t i = 0; i < in.size(); ++i) order.push_back({vgx::UberFsCovers(in[i]) ? vgx::UberIfaceOf(in[i]).Word() : 0xFFFFFFFFu, i});
    std::vector<std::pair<uint32_t, size_t>> pick;
    const size_t step = cap && order.size() > cap ? (order.size() + cap - 1) / cap : 1;
    for (size_t i = 0; i < order.size(); i += step) pick.push_back(order[i]);
    std::stable_sort(pick.begin(), pick.end());
    for (const auto& o : pick) One(in[o.second], run, seedBase + static_cast<uint32_t>(o.second));
    if (run.uberProg) { glDeleteProgram(run.uberProg); run.uberProg = 0; run.uberWord = 0xFFFFFFFFu; }
}
void Report(const char* what, const Run& run) {
    std::printf("%s: %zu keys, %zu taken by the ubershader (%.1f%%), %zu interfaces (programs of the family)", what, run.keys, run.taken,
                run.keys ? 100.0 * static_cast<double>(run.taken) / static_cast<double>(run.keys) : 0.0, run.ifaces.size());
    if (!run.notTaken.empty()) {
        std::printf("; not taken:");
        for (const auto& kv : run.notTaken) std::printf(" %s %u", kv.first.c_str(), kv.second);
    }
    auto line = [](const char* what, const Cmp& c) {
        std::printf("  %s: %zu bytes compared, %zu differ in %zu pixels (max %d level; bytes 1 / 2 / 3 / 4+ levels apart: %zu / %zu / %zu / %zu)\n", what, c.bytes, c.diff, c.diffPx,
                    c.maxd, c.hist[0], c.hist[1], c.hist[2], c.hist[3]);
    };
    std::printf("\n");
    line("PROOF ubershader vs full text, reads in the same shape", run.uw);
    std::printf("    (keys with any byte apart: %zu", run.keysDiff.size());
    { int n = 0; for (const std::string& h : run.keysDiff) if (n++ < 4) std::printf("%s %.40s", n == 1 ? ":" : ",", h.c_str()); }
    std::printf(")\n");
    line("no indirect stage: ubershader vs full float text", run.uf);
    line("no indirect stage: ubershader vs shipped text (trims; half is float here)", run.us);
    line("indirect stages: ubershader vs full float text (Mesa's filtering paths, see Wrapped)", run.ufInd);
    line("indirect stages: ubershader vs shipped text", run.usInd);
    std::printf("  text: ubershader %zu bytes, longest key text %zu bytes\n", run.uberLen, run.keyLenMax);
    Require(run.uw.bytes == 0 || run.uw.diff * 10000 <= run.uw.bytes, "at most 1 in 10,000 bytes differ from the full text (reads in the same shape)");
    Require(run.uf.bytes == 0 || run.uf.diff * 2000 <= run.uf.bytes, "at most 5 in 10,000 bytes differ from the full text (no indirect stage)");
    Require(run.us.bytes == 0 || run.us.diff * 2000 <= run.us.bytes, "at most 5 in 10,000 bytes differ from the shipped text (no indirect stage)");
}
// A rough per-pixel cost on Mesa (llvmpipe, not the console's GPU): a 256x256 quad drawn 40 times with a key's text and with the ubershader.
void Cost(const FsKey& k, Run& run, double& msKey, double& msUber) {
    const vgx::UberIface f = vgx::UberIfaceOf(k);
    std::vector<float> cfg(4 * vgx::kUberRows);
    vgx::UberFsConfig(k, cfg.data());
    vgx::ShaderSource full, u;
    vgx::GenerateFragmentShaderParts(k, full, false, 0);
    vgx::GenerateUberFragmentShader(f, u);
    const std::string vt = IfaceVs(f);
    const GLuint pk = Link(vt, full.text), pu = Link(vt, u.text);
    GLuint fbo = 0, rb = 0;
    glGenFramebuffers(1, &fbo); glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA4, 256, 256);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glViewport(0, 0, 256, 256);
    auto time = [&](GLuint p, const float* c) {
        std::vector<uint8_t> px;
        Draw(p, k, 5, c, px); // (uniforms and textures set)
        const float quad[6 * 4] = {-1, -1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1, -1, -1, 0, 1, 1, 1, 0, 1, -1, 1, 0, 1};
        const GLint loc = glGetAttribLocation(p, "a_pos");
        glVertexAttribPointer(static_cast<GLuint>(loc), 4, GL_FLOAT, GL_FALSE, 0, quad);
        glFinish();
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 40; ++i) glDrawArrays(GL_TRIANGLES, 0, 6);
        glFinish();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    msKey += time(pk, nullptr);
    msUber += time(pu, cfg.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo); glDeleteRenderbuffers(1, &rb);
    glDeleteProgram(pk); glDeleteProgram(pu);
    (void)run;
}
} // namespace

int main(int argc, char** argv) {
    Require(vgx::host::CreateContext(), "EGL context");
    std::printf("GL renderer: %s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    GLuint fbo = 0, rb = 0;
    glGenFramebuffers(1, &fbo); glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA4, kW, kH);
    // (RGBA8 where the driver has it)
    glRenderbufferStorage(GL_RENDERBUFFER, 0x8058 /* GL_RGBA8_OES */, kW, kH);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "framebuffer");
    glViewport(0, 0, kW, kH);
    glGenTextures(8, g_tex);
    const unsigned randomKeys = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 400u;
    // GXV_UBER_TEST_LISTKEYS=<n>: at most n keys of each list, spread over it (0 or unset: all)
    const size_t listCap = std::getenv("GXV_UBER_TEST_LISTKEYS") ? static_cast<size_t>(std::atol(std::getenv("GXV_UBER_TEST_LISTKEYS"))) : 0u;
    {
        Run run;
        std::vector<FsKey> rk;
        Rng r(0x0BE5);
        for (unsigned i = 0; i < randomKeys; ++i) {
            FsKey k = fshalf_keys::RandomKey(r, true);
            // (shapes the old random keys never make: the TEXVEC2 / IND_VS interfaces, alpha-dead, reject-zero, pre-pass, lod bias)
            if (r.P(30)) k.setTexVec2(1);
            for (int j = 0; j < k.numIndStages; ++j) // (IndVsPair: only a non-projective texgen)
                if (r.P(40) && k.ind[j].texCoord < k.numTexGens && !((k.texProjMask >> k.ind[j].texCoord) & 1u)) k.ind[j].pad[0] = 1;
            if (r.P(10)) k.setAlphaDead(1);
            if (k.alphaTestOn && r.P(20)) k.setRejectZero(1);
            if (k.alphaTestOn && r.P(10)) k.setPreAlpha(1);
            if (k.alphaTestOn && r.P(10)) k.setAtPass(1);
            for (int s = 0; s < k.numStages; ++s) if (k.stage[s].texEnable && r.P(5)) k.stage[s].lodBias = 1;
            if (k.inWrap & 1) vgx::CanonListFsKey(k);
            rk.push_back(k);
        }
        RunKeys(rk, run, 1, 0);
        Report("random keys", run);
    }
    for (int a = 2; a < argc; ++a) {
        std::vector<FsKey> keys;
        std::vector<uint32_t> pairFs; // (program pairs: fragment key index; bit 31 = a draw variant record)
        Require(ReadList(argv[a], keys, &pairFs), "key list readable (VGXSHL01, this tree's key sizes and generator version)");
        {
            size_t pairs = 0, pairsTaken = 0, vars = 0, varsTaken = 0;
            for (uint32_t pf : pairFs) {
                const uint32_t id = pf & 0x7FFFFFFFu;
                FsKey k = id < keys.size() ? keys[id] : FsKey{};
                vgx::CanonListFsKey(k);
                const bool t = id < keys.size() && vgx::UberFsCovers(k);
                if (pf >> 31) { ++vars; varsTaken += t; } else { ++pairs; pairsTaken += t; }
            }
            std::printf("%s: program pairs %zu, of them with a fragment key the ubershader takes %zu; draw variants %zu, taken %zu\n", argv[a], pairs, pairsTaken, vars, varsTaken);
        }
        Run run;
        for (FsKey& k : keys) vgx::CanonListFsKey(k);
        RunKeys(keys, run, 1000003u, listCap);
        Report(argv[a], run);
        // per-pixel cost on Mesa for a few real keys (indicative only: llvmpipe is not the console's GPU)
        double msKey = 0, msUber = 0;
        unsigned n = 0;
        for (size_t j = 0; j < keys.size() && n < 24; j += std::max<size_t>(1, keys.size() / 24)) {
            FsKey k = keys[j];
            vgx::CanonListFsKey(k);
            if (!vgx::UberFsCovers(k)) continue;
            Cost(k, run, msKey, msUber);
            ++n;
        }
        if (n) std::printf("  Mesa cost of %u keys, 40 x 256x256 quads each: key texts %.1f ms, ubershader %.1f ms (x%.1f)\n", n, msKey, msUber, msUber / std::max(msKey, 1e-3));
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, kW, kH);
    }
    std::printf("PASS\n");
    return 0;
}
