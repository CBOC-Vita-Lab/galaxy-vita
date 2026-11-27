// canon24b: GXV_TEVIN_CANON=2 against =1 on the game's real fragment shader keys (platform/vgx/src/vgx_shadergen.cpp WalkTevIn,
// FsTevInNeedsMark, FsTevInPreWrapMask, the generator's wrap text; vgx_draw.cpp BuildTevC's pre-wrap). No game data beyond the key lists.
//   tools/test_vgx_canon24_keys_gl.sh [output-dir]      (runs it under the console's shader switches, and more)
//   vgx_canon24_keys_gl [--rounds N] [--pack stages.pack]... [--names out.tsv] <vgx_shaderlist.bin | dir of raw .key files>...
//
// Every distinct stage configuration of the lists (a fragment key with inWrap and preWrap cleared: the lists of 2.0 .. 2.2 were made under
// GXV_TEVIN_CANON=0, where any register outside 0..255 marked every key, so a list key stands for its configuration under any mask):
//  1. unmarked: =1 and =2 give the same text (=2 only changes marked keys).
//  2. for every mask of out-of-range registers (PREV, REG0..2; 15 masks) under which the front end keeps the mark (FsTevInNeedsMark, the
//     decision =1 and =2 share): the =1 key (inWrap 1) and the =2 key (inWrap 1 + preWrap = FsTevInPreWrapMask, as BuildFsKey makes it).
//     a. text: =1 changes nothing against =0; the =2 text is the =1 text with some vgx_w3 / vgx_w1 calls left out and nothing else
//        (line by line); each one left out is classified by an independent walk of the key's stages (not WalkTevIn): an initial register
//        read (then its part must be in preWrap, or come from the vertices), or a value a stage made (claimed in 0..1 by WalkTevIn).
//        The preWrap mask is recomputed independently too: parts whose initial value is read as A / B / C and never as D.
//     b. pictures on Mesa (highp): a 64x64 grid of 128 triangles with random per-vertex colours and coordinates, 8 textures (nearest or
//        linear, texels biased to 0 and 255), registers drawn from the 11-bit boundary values -1024 -257 -256 -255 -1 0 1 127 128 254 255
//        256 257 511 512 1023 (and random ones; the mask's registers have a component outside 0..255, the others are inside), konst /
//        references / fog random. The =1 text with the raw upload (v / 255) against the =2 text with BuildTevC's upload ((v & 255) / 255 for
//        preWrap parts): RGBA8 compared byte for byte.
//     c. witness search: the =1 text in which every wrap that =2 left out of a stage-made value records how far its argument is outside
//        0..1 (no discard, the result written as the colour): a claim "in 0..1" that a real value breaks shows here even where a later
//        stage hides it from the picture.
// The text's hash ("#version 100\n" + text, XXH3, the vitaGL cache file name) of every =1 and =2 text can be written out (--names), and
// the records of shader packs (vgx_shader_pack.h, VGXRAM01) are counted against them (--pack).
// Exit 1 on a failure. ASan / UBSan clean (tools/test_vgx_canon24_keys_gl.sh --asan).
#define XXH_INLINE_ALL
#include "vgx_shadergen.h"
#include "xxhash.h"
#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace vgx { bool LogEnabled() { return false; } namespace host { bool CreateContext(); } }
namespace {
using vgx::FsKey;
unsigned g_failures = 0;
void Fail(const char* what, const std::string& detail = std::string()) {
    ++g_failures;
    if (g_failures <= 40) std::fprintf(stderr, "FAIL: %s%s%s\n", what, detail.empty() ? "" : "\n", detail.c_str());
}
inline uint32_t bits(uint32_t v, int at, int n) { return (v >> at) & ((1u << n) - 1u); }

// ---- deterministic random numbers (xorshift64*)
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x2545F491ull) {}
    uint64_t Next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 0x2545F4914F6CDD1Dull; }
    uint32_t U(uint32_t n) { return static_cast<uint32_t>((Next() >> 33) % n); }
    bool P(unsigned pct) { return U(100) < pct; }
    double F() { return static_cast<double>(Next() >> 11) / 9007199254740992.0; }
};

// ---- key lists
bool ReadList(const char* path, std::vector<FsKey>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    struct Hdr { char magic[8]; uint32_t vs, fs, gen, res; } h;
    bool ok = std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, "VGXSHL01", 8) == 0 && h.vs == sizeof(vgx::VsKey) &&
              h.fs == sizeof(FsKey) && h.gen == vgx::kShaderGenVersion;
    uint32_t type;
    while (ok && std::fread(&type, 4, 1, f) == 1) {
        if (type == 1) { vgx::VsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; }
        else if (type == 2) { FsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; out.push_back(k); }
        else if (type == 3) { uint32_t v[2]; if (std::fread(v, 8, 1, f) != 1) break; }
        else if (type == 4) { uint32_t v[4]; if (std::fread(v, 16, 1, f) != 1) break; }
        else break;
    }
    std::fclose(f);
    return ok;
}
bool ReadKeyDir(const char* path, std::vector<FsKey>& out) {
    DIR* d = opendir(path);
    if (!d) return false;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".key") == 0 && n[0] == 'f') names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        FILE* f = std::fopen((std::string(path) + "/" + n).c_str(), "rb");
        if (!f) continue;
        FsKey k;
        if (std::fread(&k, sizeof k, 1, f) == 1) out.push_back(k);
        std::fclose(f);
    }
    return true;
}
struct KeyLess {
    bool operator()(const FsKey& a, const FsKey& b) const { return std::memcmp(&a, &b, sizeof a) < 0; }
};

// ---- independent walk of a key's stages: which register parts are still their initial value when a stage reads them
// part p = r (colour part of register r) or 4 + r (alpha part).
struct Walk {
    bool initial[16][8];     // [stage][part]: the part holds the register's initial value when stage s reads its inputs
    uint8_t initAbc = 0, initD = 0;
};
Walk WalkKey(const FsKey& k) {
    Walk w{};
    bool written[8] = {};
    const int n = std::clamp<int>(k.numStages, 1, 16);
    for (int s = 0; s < n; ++s) {
        for (int p = 0; p < 8; ++p) w.initial[s][p] = !written[p];
        const uint32_t cc = k.stage[s].color, ca = k.stage[s].alpha;
        auto note = [&](int part, bool abc) { if (!written[part]) (abc ? w.initAbc : w.initD) |= static_cast<uint8_t>(1u << part); };
        for (int i = 0; i < 4; ++i) { // colour inputs: D, C, B, A at bits 0, 4, 8, 12
            const uint32_t sel = bits(cc, i * 4, 4);
            if (sel < 8) note((sel & 1) ? 4 + static_cast<int>(sel / 2) : static_cast<int>(sel / 2), i != 0);
        }
        for (int i = 0; i < 4; ++i) { // alpha inputs: D, C, B, A at bits 4, 7, 10, 13
            const uint32_t sel = bits(ca, 4 + i * 3, 3);
            if (sel < 4) note(4 + static_cast<int>(sel), i != 0);
        }
        written[bits(cc, 22, 2)] = true;
        written[4 + bits(ca, 22, 2)] = true;
    }
    return w;
}
uint8_t IndependentPreWrap(const FsKey& k) {
    const Walk w = WalkKey(k);
    uint8_t m = static_cast<uint8_t>(w.initAbc & ~w.initD);
    if (k.regFromVtx & 1) m &= static_cast<uint8_t>(~0x22u);
    if (k.regFromVtx & 2) m &= static_cast<uint8_t>(~0x44u);
    return m;
}

// ---- text alignment: the =2 text must be the =1 text less some wrap calls
struct Align {
    bool ok = true;
    std::string why;
    std::string instrumented; // the =1 text, with the left-out wraps of stage-made values recording their argument's distance from 0..1
    unsigned droppedStage = 0, droppedInit = 0, keptWraps = 0;
};
int PartOf(const std::string& arg, bool alphaLine) {
    static const char* const regs[4] = {"prev", "c0", "c1", "c2"};
    for (int r = 0; r < 4; ++r) {
        const std::string n = regs[r];
        if (arg.compare(0, n.size() + 1, n + ".") != 0) continue;
        const std::string sw = arg.substr(n.size() + 1);
        if (alphaLine) return sw == "a" ? 4 + r : -1;
        if (sw == "rgb") return r;
        if (sw == "aaa") return 4 + r;
        return -1;
    }
    return -1;
}
std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> v;
    size_t at = 0;
    while (at <= s.size()) {
        const size_t e = s.find('\n', at);
        if (e == std::string::npos) { v.push_back(s.substr(at)); break; }
        v.push_back(s.substr(at, e - at));
        at = e + 1;
    }
    return v;
}
Align AlignTexts(const std::string& t1, const std::string& t2, const FsKey& k2) {
    Align a;
    const std::vector<std::string> l1 = SplitLines(t1), l2 = SplitLines(t2);
    if (l1.size() != l2.size()) { a.ok = false; a.why = "line counts differ"; return a; }
    const Walk w = WalkKey(k2);
    int stage = -1;
    std::string out;
    for (size_t i = 0; i < l1.size(); ++i) {
        const std::string& x = l1[i];
        const std::string& y = l2[i];
        if (x.compare(0, 11, "  // stage ") == 0) stage = std::atoi(x.c_str() + 11);
        if (x == y) {
            out += x;
            if (i + 1 < l1.size()) out += '\n';
            continue;
        }
        const bool colourLine = x.compare(0, 13, "    vec3 a = ") == 0 && y.compare(0, 13, "    vec3 a = ") == 0;
        const bool alphaLine = x.compare(0, 15, "    float aa = ") == 0 && y.compare(0, 15, "    float aa = ") == 0;
        if ((!colourLine && !alphaLine) || stage < 0 || stage > 15) {
            a.ok = false; a.why = "a line other than an input line differs:\n  =1: " + x + "\n  =2: " + y;
            return a;
        }
        // "    vec3 a = E, b = E, c = E, d = E;"
        auto items = [](const std::string& line) {
            std::vector<std::pair<std::string, std::string>> v;
            std::string body = line.substr(line.find_first_not_of(' '));
            body = body.substr(body.find(' ') + 1); // (after the type)
            if (!body.empty() && body.back() == ';') body.pop_back();
            size_t at = 0;
            while (at < body.size()) {
                size_t e = body.find(", ", at);
                if (e == std::string::npos) e = body.size();
                const std::string it = body.substr(at, e - at);
                const size_t eq = it.find(" = ");
                v.emplace_back(it.substr(0, eq), eq == std::string::npos ? std::string() : it.substr(eq + 3));
                at = e + 2;
            }
            return v;
        };
        const auto i1 = items(x), i2 = items(y);
        if (i1.size() != 4 || i2.size() != 4) { a.ok = false; a.why = "input line shape: " + x; return a; }
        std::string rebuilt = x.substr(0, x.find(colourLine ? " a = " : " aa = ") + 1);
        for (int j = 0; j < 4; ++j) {
            if (i1[j].first != i2[j].first) { a.ok = false; a.why = "input names differ: " + x; return a; }
            std::string expr = i1[j].second;
            if (i1[j].second != i2[j].second) {
                const std::string head = colourLine ? "vgx_w3(" : "vgx_w1(";
                const std::string& e1 = i1[j].second;
                const std::string& e2 = i2[j].second;
                if (j == 3 || e1 != head + e2 + ")") { a.ok = false; a.why = "an input differs other than by a wrap:\n  =1: " + x + "\n  =2: " + y; return a; }
                const int part = PartOf(e2, alphaLine);
                if (part < 0) { a.ok = false; a.why = "a wrap left out of a non-register input: " + e2; return a; }
                if (w.initial[stage][part]) {
                    // an initial register value read without the wrap: only as a pre-wrapped upload, or from the vertices (0..255)
                    const int r = part & 3;
                    const bool vtx = (r == 1 && (k2.regFromVtx & 1)) || (r == 2 && (k2.regFromVtx & 2));
                    if (!vtx && !((k2.preWrap() >> part) & 1u)) {
                        a.ok = false; a.why = "an initial register read lost its wrap without the pre-wrapped upload: " + e2;
                        return a;
                    }
                    ++a.droppedInit;
                    if (vtx) expr = std::string(colourLine ? "vgx_c3(" : "vgx_c1(") + e2 + ")"; // (checked: the vertices carry 0..1)
                } else {
                    ++a.droppedStage;
                    expr = std::string(colourLine ? "vgx_c3(" : "vgx_c1(") + e2 + ")";
                }
            } else if (i1[j].second.compare(0, 7, "vgx_w3(") == 0 || i1[j].second.compare(0, 7, "vgx_w1(") == 0) {
                ++a.keptWraps;
            }
            rebuilt += i1[j].first + " = " + expr + (j < 3 ? ", " : ";");
        }
        out += rebuilt;
        if (i + 1 < l1.size()) out += '\n';
    }
    // instrumentation: the checks after vgx_w3's definition, main renamed, no discard, the distance written out
    const std::string w3 = "vec3 vgx_w3(vec3 value) { return vec3(vgx_w1(value.x), vgx_w1(value.y), vgx_w1(value.z)); }\n";
    const size_t at = out.find(w3);
    if (at == std::string::npos) { a.ok = false; a.why = "no vgx_w3 in a marked text"; return a; }
    out.insert(at + w3.size(),
               "float vgx_bad = 0.0;\n"
               "float vgx_c1(float v) { vgx_bad = max(vgx_bad, max(-v, v - 1.0)); return vgx_w1(v); }\n"
               "vec3 vgx_c3(vec3 v) { vgx_bad = max(vgx_bad, max(max(max(-v.x, v.x - 1.0), max(-v.y, v.y - 1.0)), max(-v.z, v.z - 1.0)));"
               " return vgx_w3(v); }\n");
    size_t m = out.find("void main() {");
    if (m == std::string::npos) { a.ok = false; a.why = "no main"; return a; }
    out.replace(m, 13, "void vgx_main() {");
    for (size_t d = out.find("discard;"); d != std::string::npos; d = out.find("discard;", d)) out.replace(d, 8, "{}");
    // (R: distance > 1e-6, G: distance in levels * 16 (fine), B: distance > 1e-4 (0.0255 level), A: distance in levels / 16 (coarse))
    out += "\nvoid main() { vgx_main(); float lv = vgx_bad * 255.0;\n"
           "  gl_FragColor = vec4(vgx_bad > 1e-6 ? 1.0 : 0.0, clamp(lv * 16.0, 0.0, 1.0), vgx_bad > 1e-4 ? 1.0 : 0.0, clamp(lv / 16.0, 0.0, 1.0)); }\n";
    a.instrumented = out;
    return a;
}

// ---- GL
GLuint CompileSh(GLenum type, const std::string& text, bool& ok) {
    GLuint s = glCreateShader(type);
    const char* p = text.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint st = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &st);
    if (!st) {
        char log[4096] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        Fail("shader compiles", std::string(log) + "\n" + text);
        ok = false;
    }
    return s;
}
// The vertex shader for a fragment text: every varying the text declares, from four attributes.
std::string VertexFor(const std::string& fs) {
    std::string vs = "attribute vec4 a_pos; attribute vec4 a_c0; attribute vec4 a_c1; attribute vec4 a_t; attribute vec4 a_u;\n";
    std::string body;
    for (const std::string& line : SplitLines(fs)) {
        if (line.compare(0, 8, "varying ") != 0) continue;
        vs += line + "\n";
        char type[32] = {}, name[64] = {};
        if (std::sscanf(line.c_str(), "varying %31s %63[^;];", type, name) != 2) continue;
        const std::string t = type, n = name;
        if (n == "v_col0") body += "  v_col0 = a_c0;\n";
        else if (n == "v_col1") body += "  v_col1 = a_c1;\n";
        else if (n.compare(0, 5, "v_tex") == 0) {
            const int i = std::atoi(n.c_str() + 5);
            char b[256];
            // (texgen i: its own scale and offset of the two random coordinates; q 0.5 .. 1.5)
            std::snprintf(b, sizeof b, "vec2(a_t.%s) * %.3f + vec2(%.3f, %.3f)", (i & 1) ? "yx" : "xy", 1.0 + 0.37 * i, 0.11 * i, -0.07 * i);
            if (t == "vec2") body += "  " + n + " = " + b + ";\n";
            else body += "  " + n + " = vec3(" + b + ", a_t.z);\n";
        } else if (t == "float") body += "  " + n + " = a_u.x;\n";
        else if (t == "vec2") body += "  " + n + " = a_u.xy;\n";
        else if (t == "vec3") body += "  " + n + " = a_u.xyz;\n";
        else body += "  " + n + " = a_u;\n";
    }
    return vs + "void main() { gl_Position = a_pos;\n" + body + "}\n";
}
GLuint Link(const std::string& fs, bool& ok) {
    const std::string vt = VertexFor(fs);
    const GLuint v = CompileSh(GL_VERTEX_SHADER, vt, ok), f = CompileSh(GL_FRAGMENT_SHADER, fs, ok);
    const GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_c0");
    glBindAttribLocation(p, 2, "a_c1");
    glBindAttribLocation(p, 3, "a_t");
    glBindAttribLocation(p, 4, "a_u");
    glLinkProgram(p);
    GLint st = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &st);
    if (!st) {
        char log[4096] = {};
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        Fail("program links", std::string(log) + "\n---- vs\n" + vt + "---- fs\n" + fs);
        ok = false;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}
constexpr int kW = 64, kH = 64, kCells = 8;
GLuint g_tex[8];
struct Vtx { float pos[4], c0[4], c1[4], t[4], u[4]; };

const int kBound[16] = {-1024, -257, -256, -255, -1, 0, 1, 127, 128, 254, 255, 256, 257, 511, 512, 1023};
const int kBoundIn[6] = {0, 1, 127, 128, 254, 255};
// One draw's inputs (the same for the programs compared).
struct Inputs {
    int reg[4][4];
    float tev[112];
    std::vector<Vtx> v;
    std::vector<uint8_t> texel[8];
    bool linear = false;
};
float ColSample(Rng& r) {
    const unsigned w = r.U(100);
    if (w < 20) return 0.0f;
    if (w < 40) return 1.0f;
    if (w < 70) return static_cast<float>(r.U(256)) / 255.0f;
    return static_cast<float>(r.F());
}
void MakeInputs(Rng& r, unsigned mask, Inputs& in) {
    for (int reg = 0; reg < 4; ++reg) {
        const bool wide = (mask >> reg) & 1u;
        for (int c = 0; c < 4; ++c) {
            int v;
            if (wide) {
                const unsigned w = r.U(100);
                v = w < 70 ? kBound[r.U(16)] : static_cast<int>(r.U(2048)) - 1024;
            } else {
                v = r.P(60) ? kBoundIn[r.U(6)] : static_cast<int>(r.U(256));
            }
            in.reg[reg][c] = v;
        }
        if (wide) { // (the mask says: at least one component outside 0..255)
            bool any = false;
            for (int c = 0; c < 4; ++c) any = any || in.reg[reg][c] < 0 || in.reg[reg][c] > 255;
            if (!any) {
                static const int kOut[8] = {-1024, -257, -256, -255, -1, 256, 257, 1023};
                in.reg[reg][r.U(4)] = r.P(70) ? kOut[r.U(8)] : (r.P(50) ? -1 - static_cast<int>(r.U(1024)) : 256 + static_cast<int>(r.U(768)));
            }
        }
    }
    float* t = in.tev;
    std::fill(t, t + 112, 0.0f);
    for (int i = 0; i < 16; ++i) t[i] = static_cast<float>(in.reg[i / 4][i % 4]) / 255.0f; // (BuildTevC: v / 255)
    for (int i = 16; i < 32; ++i) t[i] = static_cast<float>(r.P(40) ? (r.P(50) ? 0 : 255) : r.U(256)) / 255.0f;
    t[32] = static_cast<float>(r.U(256)); t[33] = static_cast<float>(r.U(256)); t[34] = static_cast<float>(r.U(256)) / 255.0f;
    t[35] = static_cast<float>(r.F());
    for (int i = 36; i < 40; ++i) t[i] = static_cast<float>(r.F()) * 2.0f - 0.5f; // (fog parameters)
    for (int i = 40; i < 44; ++i) t[i] = static_cast<float>(r.U(256)) / 255.0f;
    for (int i = 44; i < 68; ++i) t[i] = static_cast<float>(r.F()) * 2.0f - 1.0f;
    for (int i = 68; i < 84; i += 4) { t[i] = 1.0f; t[i + 1] = 1.0f; t[i + 2] = 1.0f / 16; t[i + 3] = 1.0f / 16; }
    for (int i = 84; i < 100; ++i) t[i] = 16.0f;
    for (int i = 100; i < 108; ++i) t[i] = static_cast<float>(r.F()) * 2.0f - 1.0f;
    t[108] = 1.0f / 16; t[109] = 1.0f / 16; t[110] = static_cast<float>(r.F()) * 0.1f; t[111] = static_cast<float>(r.F()) * 0.1f;
    in.linear = r.P(40);
    for (int m = 0; m < 8; ++m) {
        in.texel[m].resize(16 * 16 * 4);
        for (uint8_t& b : in.texel[m]) b = static_cast<uint8_t>(r.P(40) ? (r.P(50) ? 0 : 255) : r.U(256));
    }
    in.v.clear();
    const float step = 2.0f / kCells;
    for (int cy = 0; cy < kCells; ++cy)
        for (int cx = 0; cx < kCells; ++cx) {
            const float x0 = -1.0f + cx * step, y0 = -1.0f + cy * step, x1 = x0 + step, y1 = y0 + step;
            const float corner[6][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1}};
            for (const auto& c : corner) {
                Vtx x{};
                x.pos[0] = c[0]; x.pos[1] = c[1]; x.pos[2] = 0.0f; x.pos[3] = 1.0f;
                for (float& f : x.c0) f = ColSample(r);
                for (float& f : x.c1) f = ColSample(r);
                x.t[0] = static_cast<float>(r.F()) * 4.0f - 2.0f; x.t[1] = static_cast<float>(r.F()) * 4.0f - 2.0f;
                x.t[2] = 0.5f + static_cast<float>(r.F()); x.t[3] = 1.0f;
                for (float& f : x.u) f = static_cast<float>(r.F());
                in.v.push_back(x);
            }
        }
}
void Draw(GLuint p, const Inputs& in, const float* tev, int nTev, std::vector<uint8_t>& px) {
    glUseProgram(p);
    if (GLint l = glGetUniformLocation(p, "u_tev"); l >= 0) glUniform4fv(l, nTev, tev);
    for (int m = 0; m < 8; ++m) {
        glActiveTexture(GL_TEXTURE0 + m);
        glBindTexture(GL_TEXTURE_2D, g_tex[m]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, in.texel[m].data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, in.linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, in.linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        char name[16];
        std::snprintf(name, sizeof name, "u_tex%d", m);
        if (GLint l = glGetUniformLocation(p, name); l >= 0) glUniform1i(l, m);
    }
    for (GLuint a = 0; a < 5; ++a) {
        glEnableVertexAttribArray(a);
        glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), reinterpret_cast<const float*>(in.v.data()) + a * 4);
    }
    glDisable(GL_BLEND);
    glClearColor(17.0f / 255, 33.0f / 255, 49.0f / 255, 65.0f / 255);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(in.v.size()));
    px.assign(kW * kH * 4, 0);
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    if (glGetError() != GL_NO_ERROR) Fail("GL status");
}

std::string Hex(const FsKey& k) {
    const uint64_t h = XXH3_64bits(&k, sizeof k);
    char b[20];
    std::snprintf(b, sizeof b, "%016llX", static_cast<unsigned long long>(h));
    return b;
}
std::string TextName(const std::string& text) {
    const std::string s = "#version 100\n" + text;
    const unsigned long long h = XXH3_64bits(s.data(), s.size());
    char b[40];
    std::snprintf(b, sizeof b, "f/%02X/%llX.gxp", static_cast<unsigned>(h >> 56), h);
    return b;
}
size_t Wraps(const std::string& s) {
    size_t n = 0;
    const size_t m = s.find("void main");
    for (const char* w : {"vgx_w1(", "vgx_w3("})
        for (size_t at = s.find(w, m); at != std::string::npos; at = s.find(w, at + 1)) ++n;
    return n;
}
// The records (stage, hash) of a VGXRAM01 pack.
bool ReadPack(const char* path, std::set<std::pair<uint32_t, uint64_t>>& recs) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> b;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    if (b.size() < 40 || std::memcmp(b.data(), "VGXRAM01", 8) != 0) return false;
    uint32_t count;
    std::memcpy(&count, b.data() + 12, 4);
    if (40 + static_cast<size_t>(count) * 24 > b.size()) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t stage;
        uint64_t hash;
        std::memcpy(&stage, b.data() + 40 + i * 24, 4);
        std::memcpy(&hash, b.data() + 40 + i * 24 + 8, 8);
        recs.insert({stage, hash});
    }
    return true;
}
uint64_t NameHash(const std::string& text) {
    const std::string s = "#version 100\n" + text;
    return XXH3_64bits(s.data(), s.size());
}
} // namespace

int main(int argc, char** argv) {
    unsigned rounds = 2;
    // CANON24_RECIP=1 (diagnosis only): the =2 draw gets each pre-wrapped part as low byte * (1 / 255), a GPU's divide, instead of BuildTevC's
    // correctly rounded low byte / 255: what is left then is not the upload's rounding.
    const bool recipUpload = std::getenv("CANON24_RECIP") && std::atoi(std::getenv("CANON24_RECIP")) == 1;
    const volatile float kOne = 1.0f, k255 = 255.0f;
    const float kRecip255 = kOne / k255;
    std::vector<std::string> packs;
    const char* namesOut = nullptr;
    std::vector<FsKey> all;
    std::map<std::string, size_t> perSource;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--rounds" && i + 1 < argc) { rounds = static_cast<unsigned>(std::atoi(argv[++i])); continue; }
        if (a == "--pack" && i + 1 < argc) { packs.push_back(argv[++i]); continue; }
        if (a == "--names" && i + 1 < argc) { namesOut = argv[++i]; continue; }
        const size_t before = all.size();
        if (!ReadList(argv[i], all) && !ReadKeyDir(argv[i], all)) { std::fprintf(stderr, "cannot read %s as a key list or a key folder\n", argv[i]); return 2; }
        perSource[a] = all.size() - before;
    }
    // distinct stage configurations (inWrap and preWrap cleared), and how many list keys came marked
    std::map<FsKey, unsigned, KeyLess> configs;
    size_t listMarked = 0, listPreWrap = 0;
    for (FsKey k : all) {
        if (k.inWrap & 1) ++listMarked;
        if (k.preWrap()) ++listPreWrap;
        k.inWrap = 0;
        k.setPreWrap(0);
        configs[k] |= (all.size() ? 1u : 0u);
    }
    for (const auto& s : perSource) std::printf("source %s: %zu fragment keys\n", s.first.c_str(), s.second);
    std::printf("%zu fragment keys, %zu marked (inWrap 1) as listed, %zu with a preWrap; %zu distinct stage configurations\n", all.size(), listMarked,
                listPreWrap, configs.size());

    if (!vgx::host::CreateContext()) { std::fprintf(stderr, "no Mesa GLES context\n"); return 2; }
    std::printf("GL renderer: %s; fragment highp: ", glGetString(GL_RENDERER));
    {
        GLint range[2] = {}, prec = 0;
        glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &prec);
        std::printf("range 2^%d, precision 2^-%d\n", range[1], prec);
    }
    GLuint fbo, rt;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &rt);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, rt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { std::fprintf(stderr, "framebuffer\n"); return 2; }
    glGenTextures(8, g_tex);
    glViewport(0, 0, kW, kH);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);

    std::set<std::pair<uint32_t, uint64_t>> packRecs;
    for (const std::string& p : packs)
        if (!ReadPack(p.c_str(), packRecs)) std::fprintf(stderr, "cannot read pack %s\n", p.c_str());
    FILE* names = namesOut ? std::fopen(namesOut, "w") : nullptr;
    if (names) std::fprintf(names, "config\tmarked\tname_canon1\tname_canon2\n");

    size_t unmarkedSame = 0, unmarkedChanged = 0, everMarked = 0, textChanged = 0, preWrapKeys = 0, keysDiffer = 0, keysWitness = 0;
    size_t variantTexts = 0, wraps1 = 0, wraps2 = 0, droppedStage = 0, droppedInit = 0, pairsByMask[16] = {}, draws = 0;
    uint64_t bytes = 0, bytesDiff = 0, pxWitness = 0, pxWitnessBig = 0, pxCompared = 0;
    int maxDiff = 0;
    double maxWitness = 0.0;
    std::set<uint64_t> names1, names2, namesUnmarked;
    std::vector<std::string> worst;
    size_t idx = 0;
    for (const auto& ce : configs) {
        const FsKey& c = ce.first;
        ++idx;
        // 1. unmarked: the same text under =1 and =2
        {
            vgx::TevInCanonModeForTest(1);
            vgx::ShaderSource s1;
            vgx::GenerateFragmentShader(c, s1);
            vgx::TevInCanonModeForTest(2);
            vgx::ShaderSource s2;
            vgx::GenerateFragmentShader(c, s2);
            if (s1.text == s2.text) ++unmarkedSame;
            else { ++unmarkedChanged; Fail("an unmarked key's text changes under =2", Hex(c)); }
            namesUnmarked.insert(NameHash(s1.text));
        }
        unsigned need = 0;
        for (unsigned m = 1; m < 16; ++m)
            if (vgx::FsTevInNeedsMark(c, m, true)) need |= 1u << m;
        if (!need) continue;
        ++everMarked;
        FsKey k1 = c, k2 = c;
        k1.inWrap = 1;
        k2.inWrap = 1;
        k2.setPreWrap(vgx::FsTevInPreWrapMask(k2));
        if (k2.preWrap()) ++preWrapKeys;
        if (k2.preWrap() != IndependentPreWrap(k2)) Fail("FsTevInPreWrapMask differs from the independent walk", Hex(c));
        vgx::TevInCanonModeForTest(0);
        vgx::ShaderSource s0;
        vgx::GenerateFragmentShader(k1, s0);
        vgx::TevInCanonModeForTest(1);
        vgx::ShaderSource s1;
        vgx::GenerateFragmentShader(k1, s1);
        vgx::TevInCanonModeForTest(2);
        vgx::ShaderSource s2;
        vgx::GenerateFragmentShader(k2, s2);
        if (s0.text != s1.text) Fail("=1 changes a marked key's text", Hex(c));
        names1.insert(NameHash(s1.text));
        names2.insert(NameHash(s2.text));
        if (names) std::fprintf(names, "%s\t1\t%s\t%s\n", Hex(c).c_str(), TextName(s1.text).c_str(), TextName(s2.text).c_str());
        wraps1 += Wraps(s1.text);
        wraps2 += Wraps(s2.text);
        if (s1.text == s2.text) continue;
        ++textChanged;
        const Align al = AlignTexts(s1.text, s2.text, k2);
        if (!al.ok) { Fail("the =2 text is not the =1 text less some wraps", Hex(c) + ": " + al.why); continue; }
        droppedStage += al.droppedStage;
        droppedInit += al.droppedInit;
        // (the key bits the 2.3 front end sets that the lists of 2.0 .. 2.2 may not have: GXV_TEXVEC2, GXV_DISCARD_BLEND's rejectZero,
        // GXV_FS_ADEAD's alphaDead, GXV_PIX_ATPASS's atPass. WalkTevIn does not read them; the =2 text must still be the =1 text less the
        // same wraps)
        for (int v = 0; v < 4; ++v) {
            FsKey a1 = k1;
            if (v == 0) a1.setTexVec2(a1.texVec2() ? 0 : 1);
            else if (v == 1) { if (!a1.alphaTestOn) continue; a1.setRejectZero(1); }
            else if (v == 2) { if (a1.alphaTestOn || a1.dstAlpha) continue; a1.setAlphaDead(1); }
            else { if (!a1.alphaTestOn) continue; a1.setAtPass(1); }
            FsKey a2 = a1;
            a2.setPreWrap(vgx::FsTevInPreWrapMask(a1));
            if (a2.preWrap() != k2.preWrap()) Fail("a 2.3 key bit changes the preWrap mask", Hex(c));
            vgx::TevInCanonModeForTest(1);
            vgx::ShaderSource v1;
            vgx::GenerateFragmentShader(a1, v1);
            vgx::TevInCanonModeForTest(2);
            vgx::ShaderSource v2;
            vgx::GenerateFragmentShader(a2, v2);
            const Align av = AlignTexts(v1.text, v2.text, a2);
            ++variantTexts;
            if (!av.ok || av.droppedStage != al.droppedStage || av.droppedInit != al.droppedInit)
                Fail("a 2.3 key bit changes which wraps =2 leaves out", Hex(c) + ": " + av.why);
        }
        bool ok = true;
        const GLuint p1 = Link(s1.text, ok), p2 = Link(s2.text, ok), pi = Link(al.instrumented, ok);
        if (!ok) { glDeleteProgram(p1); glDeleteProgram(p2); glDeleteProgram(pi); continue; }
        const int nTev = vgx::FsTevCount(c);
        int keyMax = 0;
        uint64_t keyDiff = 0, keyWit = 0;
        double keyWitMax = 0.0;
        for (unsigned m = 1; m < 16; ++m) {
            if (!((need >> m) & 1u)) continue;
            ++pairsByMask[m];
            for (unsigned rd = 0; rd < rounds; ++rd) {
                Rng r(XXH3_64bits(&c, sizeof c) ^ (static_cast<uint64_t>(m) << 40) ^ (static_cast<uint64_t>(rd) << 48));
                Inputs in;
                MakeInputs(r, m, in);
                float tev2[112];
                std::memcpy(tev2, in.tev, sizeof tev2);
                for (int i = 0; i < 16; ++i) // (BuildTevC under =2: a preWrap part as (v & 255) / 255)
                    if ((k2.preWrap() >> ((i & 3) < 3 ? i / 4 : i / 4 + 4)) & 1u)
                        tev2[i] = recipUpload ? static_cast<float>(in.reg[i / 4][i % 4] & 255) * kRecip255 // (diagnosis: the GPU's form)
                                              : static_cast<float>(in.reg[i / 4][i % 4] & 255) / 255.0f;
                std::vector<uint8_t> x, y, z;
                Draw(p1, in, in.tev, nTev, x);
                Draw(p2, in, tev2, nTev, y);
                Draw(pi, in, in.tev, nTev, z);
                ++draws;
                for (size_t j = 0; j < x.size(); j += 4) {
                    int d = 0;
                    for (int ch = 0; ch < 4; ++ch) {
                        const int e = std::abs(static_cast<int>(x[j + ch]) - y[j + ch]);
                        d = std::max(d, e);
                        bytesDiff += e != 0;
                    }
                    keyMax = std::max(keyMax, d);
                    keyDiff += d != 0;
                    ++pxCompared;
                    if (z[j] > 127) {
                        ++keyWit;
                        const double lv = z[j + 1] < 255 ? z[j + 1] / 255.0 / 16.0 : z[j + 3] / 255.0 * 16.0;
                        keyWitMax = std::max(keyWitMax, lv);
                        if (z[j + 2] > 127) ++pxWitnessBig;
                    }
                }
                bytes += x.size();
            }
        }
        glDeleteProgram(p1);
        glDeleteProgram(p2);
        glDeleteProgram(pi);
        maxDiff = std::max(maxDiff, keyMax);
        if (keyDiff) ++keysDiffer;
        if (keyWit) ++keysWitness;
        pxWitness += keyWit;
        maxWitness = std::max(maxWitness, keyWitMax);
        if (keyMax > 0 || keyWit > 0) {
            char b[256];
            std::snprintf(b, sizeof b, "%s: %llu pixels differ (max %d levels), %llu pixels with a dropped wrap's argument outside 0..1 (max %.4f levels); preWrap %02X",
                          Hex(c).c_str(), static_cast<unsigned long long>(keyDiff), keyMax, static_cast<unsigned long long>(keyWit), keyWitMax, k2.preWrap());
            worst.push_back(b);
        }
        if (idx % 200 == 0) std::fprintf(stderr, "... %zu of %zu configurations\n", idx, configs.size());
    }
    vgx::TevInCanonModeForTest(-1);
    if (names) std::fclose(names);
    std::printf("unmarked texts: %zu the same under =1 and =2, %zu changed\n", unmarkedSame, unmarkedChanged);
    std::printf("configurations the front end can mark (some mask needs it): %zu; their =2 text differs from =1 in %zu (%zu with pre-wrapped registers)\n",
                everMarked, textChanged, preWrapKeys);
    std::printf("wrap calls in main: =1 %zu, =2 %zu (left out: %zu of stage-made values, %zu initial reads of pre-wrapped / vertex registers)\n", wraps1, wraps2,
                droppedStage, droppedInit);
    std::printf("2.3 key-bit variants (texVec2, rejectZero, alphaDead, atPass) checked for the same left-out wraps: %zu texts\n", variantTexts);
    std::printf("key x mask pairs drawn:");
    for (unsigned m = 1; m < 16; ++m) std::printf(" %X:%zu", m, pairsByMask[m]);
    std::printf("\ndraws per program %zu (%u rounds per mask), pixels compared %llu, bytes %llu, bytes differing %llu, max difference %d levels; keys with a difference %zu\n",
                draws, rounds, static_cast<unsigned long long>(pxCompared), static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(bytesDiff), maxDiff,
                keysDiffer);
    std::printf("witness: pixels where a left-out wrap's argument was outside 0..1 by more than 1e-6: %llu (more than 1e-4: %llu) in %zu keys, largest %.4f levels\n",
                static_cast<unsigned long long>(pxWitness), static_cast<unsigned long long>(pxWitnessBig), keysWitness, maxWitness);
    for (size_t i = 0; i < worst.size() && i < 40; ++i) std::printf("  key %s\n", worst[i].c_str());
    std::printf("distinct =1 names of markable configurations %zu, =2 names %zu, of unmarked configurations %zu\n", names1.size(), names2.size(),
                namesUnmarked.size());
    if (!packRecs.empty()) {
        size_t f = 0, hit1 = 0, hit2 = 0, hitU = 0, hit1changed = 0;
        for (const auto& r : packRecs) {
            if (r.first != 2) continue;
            ++f;
            const bool a = names1.count(r.second) != 0, b = names2.count(r.second) != 0;
            hit1 += a;
            hit2 += b;
            hitU += namesUnmarked.count(r.second) != 0;
            hit1changed += a && !b;
        }
        size_t new2 = 0;
        for (uint64_t h : names2) {
            bool in = false;
            for (uint32_t st : {2u}) in = in || packRecs.count({st, h});
            if (!in && !names1.count(h)) ++new2;
        }
        std::printf("pack(s): %zu records, %zu fragment; fragment records that are =1 texts of markable keys %zu (of them not an =2 text: %zu), =2 texts %zu, "
                    "unmarked texts %zu; =2 texts new (in no pack, not an =1 text) %zu\n",
                    packRecs.size(), f, hit1, hit1changed, hit2, hitU, new2);
    }
    // a difference over two levels, or a dropped wrap whose argument was outside 0..1 by more than 1e-4, is a hole
    if (maxDiff > 2) Fail("an output differs by more than two levels");
    if (pxWitnessBig) Fail("a wrap =2 left out would have acted (argument outside 0..1 by more than 1e-4)");
    std::printf("%s (%u failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
