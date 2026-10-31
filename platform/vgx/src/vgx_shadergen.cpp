// Names in generated code: vitaGL's GLSL-to-Cg translator #defines the
// identifiers s, t, st, ts, ss and tt (texture-coordinate swizzles), so none
// of them is used as a variable name, and mod() is avoided (it becomes Cg's
// truncating fmod there).
//
// GLSL ES 1.00 generation from the structural keys. TEV runs in floats (0..1
// for 0..255); everything that is only a value comes from uniform arrays that
// mirror the uniform blocks in vgx_cmd.h, so one glUniform4fv per block is
// enough on the back end.
#include "vgx_shadergen.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>

namespace vgx {

uint64_t HashKey(const void* key, size_t size) { return XXH3_64bits(key, size); }

bool UniformPacking() {
    static const bool on = EnvInt("GXV_UNIPACK", 1) != 0;
    return on;
}
// GXV_LTMERGE (read once, default 0): a lit channel whose colour and alpha parts have the same light mask, attenuation and diffuse
// function runs each light once for a vec4 (rgb and a together) instead of twice (vec3, then float): the same sums in the same order.
static bool LightMerge() {
    static const bool on = EnvInt("GXV_LTMERGE", 0) != 0;
    return on;
}
long PaletteRowsMode() {
    static const long m = EnvInt("GXV_PALROWS", 0); // see vgx_shadergen.h
    return m;
}
long FogVtxMode() {
    static const long m = EnvInt("GXV_FOGVTX", 0); // 0 per-pixel fog (as always), 1 exact vertex fog (ortho linear), 2 + perspective linear
    return m;
}
bool FogVtxEligible(uint8_t fog) {
    const long m = FogVtxMode();
    if (m < 1 || (fog & 7) != 2) return false; // linear only: the exp variants keep their per-pixel curve
    return (fog & 8) ? m >= 1 : m >= 2;
}
FogVtxCoef FogVtxCoeffs(double a, double b, double c, double p4, double p5, double zn, double zf, bool ortho) {
    const double hd = (zf - zn) * 0.5;
    if (ortho) {
        // fg = a * z_window - c, z_window = zn + hd * (z_clip + 1)
        return {static_cast<float>(a * hd), static_cast<float>(a * (zn + hd) - c)};
    }
    // z_window = zn + hd * (z_clip / w + 1), z_clip = m10 * ze + m14, w = -ze (BuildProjC), so with
    // q(w) = b - z_window = kc - mq / w:   fg = a / max(q, 1e-6) - c
    const double m10 = 2.0 * p4 - 1.0, m14 = 2.0 * p5;
    const double kc = b - zn - hd * (1.0 - m10), mq = hd * m14;
    auto Finish = [](double c1, double c0) { return FogVtxCoef{static_cast<float>(c1), static_cast<float>(c0)}; };
    if (!(a > 0.0) || !(mq < 0.0) || !std::isfinite(kc) || !std::isfinite(mq)) return Finish(0.0, std::min(1.0, std::max(0.0, -c)));
    // w where the factor (before the clamp) reaches t: a / q = c + t, q = kc - mq / w
    auto WAt = [&](double t, double& w) {
        const double want = c + t;
        if (!(want > 0.0)) return false;
        const double den = kc - a / want;
        if (!(den * mq > 0.0)) return false; // never reached for w > 0
        w = mq / den;
        return w > 0.0 && std::isfinite(w);
    };
    double wb = 0.0, wa = 0.0, fa = -c;
    if (!WAt(1.0, wb)) return Finish(0.0, std::min(1.0, std::max(0.0, -c))); // does not reach 1: ramp not representable
    if (c > 0.0) {
        fa = 0.0;
        if (!WAt(0.0, wa) || !(wa < wb)) return Finish(0.0, 0.0);
    } else if (c <= -1.0) {
        return Finish(0.0, 1.0);
    }
    const double c1 = (1.0 - fa) / (wb - wa);
    double c0 = fa - c1 * wa;
    // The chord is on one side of the curve between its two points: lower the line by half of the largest gap (the
    // best straight line in the least-maximum-error sense), so that the error is shared by both sides.
    double lo = 0.0, hi = 0.0;
    for (int i = 1; i < 64; ++i) {
        const double w = wa + (wb - wa) * i / 64.0;
        const double q = kc - mq / w;
        const double truth = a / std::max(q, 1e-6) - c;
        const double gap = (c1 * w + c0) - truth;
        lo = std::min(lo, gap);
        hi = std::max(hi, gap);
    }
    c0 -= 0.5 * (lo + hi);
    return Finish(c1, c0);
}
bool NoDiscard() {
    static const bool on = EnvInt("GXV_NODISCARD", 0) != 0; // 0: `discard` for every alpha test, as before
    return on;
}
bool AlphaTestAlwaysPasses(uint32_t comp0, int ref0, uint32_t op, uint32_t comp1, int ref1) {
    enum { F = 0, T = 1, V = 2 };
    auto Val = [](uint32_t c, int ref) {
        if (c == 7) return T;
        if (c == 0) return F;
        if (ref < 0) return V;
        if (c == 6 && ref == 0) return T;   // alpha >= 0
        if (c == 3 && ref >= 255) return T; // alpha <= 255
        if (c == 1 && ref <= 0) return F;   // alpha < 0
        if (c == 4 && ref >= 255) return F; // alpha > 255
        return V;
    };
    const int x = Val(comp0, ref0), y = Val(comp1, ref1);
    if (x != V && y != V) {
        switch (op & 3) {
        case 0: return x && y;
        case 1: return x || y;
        case 2: return x != y;
        default: return x == y;
        }
    }
    if ((op & 3) == 0) return false; // AND: only both true passes
    if ((op & 3) == 1) return x == T || y == T;
    // XOR / XNOR of the same comparison with the same reference: XNOR is always true
    return (op & 3) == 3 && comp0 == comp1 && ref0 == ref1 && ref0 >= 0;
}
int FsTevCount(const FsKey& k) {
    if (!UniformPacking() || k.numIndStages) return 28;
    for (int s = 0; s < k.numStages; ++s)
        if (k.stage[s].lodBias) return 28; // (the biases sit after the texture sizes: TEV_LODBIAS)
    return k.fog ? 11 : 9; // registers, konst, misc (+ fog parameters and colour)
}
int VsLightCount(const VsKey& k) {
    int maxLight = -1;
    for (int i = 0; i < 4; ++i) {
        const VsKey::Chan& c = k.chan[i];
        if ((i & 1) >= k.numChans || !c.enable || !c.lightMask) continue;
        for (int l = 0; l < 8; ++l)
            if ((c.lightMask & (1u << l)) && l > maxLight) maxLight = l;
    }
    return 4 + (maxLight + 1) * 5;
}

namespace {

struct Out {
    std::string s;
    void operator()(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        s += buf;
    }
};

const char* const kSwz = "rgba";
std::string Swizzle(uint8_t swap) {
    std::string r;
    for (int i = 0; i < 4; ++i) r += kSwz[(swap >> (i * 2)) & 3];
    return r;
}

// UB_TEV as vec4 indices
enum { V_REG = 0, V_KONST = 4, V_MISC = 8, V_FOG = 9, V_FOGCOL = 10, V_INDMTX = 11, V_INDSCALE = 17, V_TEXSIZE = 21, V_LODBIAS = 25 };

const char* ColorArg(uint32_t a) {
    static const char* const t[16] = {"prev.rgb", "prev.aaa", "c0.rgb", "c0.aaa", "c1.rgb", "c1.aaa", "c2.rgb", "c2.aaa",
                                      "tex.rgb", "tex.aaa", "ras.rgb", "ras.aaa", "vec3(1.0)", "vec3(0.5)", "kc", "vec3(0.0)"};
    return t[a & 15];
}
// The colour / alpha input with the low-8-bit wrap (FsKey::inWrap) on register reads (selectors 0..7 / 0..3).
std::string WrapArg(const char* plain, bool reg, bool color) {
    if (!reg) return plain;
    return std::string(color ? "vgx_w3(" : "vgx_w1(") + plain + ")";
}
const char* AlphaArg(uint32_t a) {
    static const char* const t[8] = {"prev.a", "c0.a", "c1.a", "c2.a", "tex.a", "ras.a", "ka", "0.0"};
    return t[a & 7];
}
const char* RegName(uint32_t r) {
    static const char* const t[4] = {"prev", "c0", "c1", "c2"};
    return t[r & 3];
}
std::string KonstColor(uint8_t sel) {
    char b[64];
    if (sel < 8) { std::snprintf(b, sizeof b, "vec3(%.3f)", (8 - sel) / 8.0); return b; }
    if (sel < 12) return "vec3(0.0)";
    const int k = sel & 3;
    static const char* const comp[5] = {"rgb", "rrr", "ggg", "bbb", "aaa"};
    std::snprintf(b, sizeof b, "u_tev[%d].%s", V_KONST + k, comp[(sel - 12) >> 2]);
    return b;
}
std::string KonstAlpha(uint8_t sel) {
    char b[64];
    if (sel < 8) { std::snprintf(b, sizeof b, "%.3f", (8 - sel) / 8.0); return b; }
    if (sel < 16) return "0.0";
    const int k = sel & 3;
    static const char* const comp[4] = {"r", "g", "b", "a"};
    std::snprintf(b, sizeof b, "u_tev[%d].%s", V_KONST + k, comp[(sel - 16) >> 2]);
    return b;
}
const char* CompareExpr(uint32_t c, const char* ref, char* buf, size_t n) {
    switch (c) {
    case 0: return "false";
    case 1: std::snprintf(buf, n, "(ai < %s - 0.5)", ref); break;
    case 2: std::snprintf(buf, n, "(abs(ai - %s) < 0.5)", ref); break;
    case 3: std::snprintf(buf, n, "(ai < %s + 0.5)", ref); break;
    case 4: std::snprintf(buf, n, "(ai > %s + 0.5)", ref); break;
    case 5: std::snprintf(buf, n, "(abs(ai - %s) > 0.5)", ref); break;
    case 6: std::snprintf(buf, n, "(ai > %s - 0.5)", ref); break;
    default: return "true";
    }
    return buf;
}

// Only exact copies of an explicitly clamped register (or a unit literal).
// Do not infer bounds from uniforms, textures, raster colours, or general mix math.
int UnitCopyArg(uint32_t r, bool color) {
    if (bits(r, 16, 3) || bits(r, 20, 2)) return -1;
    const unsigned z = color ? 15 : 7;
    const unsigned a = color ? bits(r, 12, 4) : bits(r, 13, 3);
    const unsigned b = color ? bits(r, 8, 4) : bits(r, 10, 3);
    const unsigned c = color ? bits(r, 4, 4) : bits(r, 7, 3);
    const unsigned d = color ? bits(r, 0, 4) : bits(r, 4, 3);
    if (b == z && c == z && d == z) return static_cast<int>(a);
    if (a == z && b == z && c == z) return static_cast<int>(d);
    return -1;
}

// Independent verifier: walk definitions backwards instead of propagating flags.
// Every link moves to an earlier stage, including cross-component RGB <- alpha.
bool VerifyUnitFinal(const FsKey& k, bool color) {
    int stage = k.numStages - 1;
    while (stage >= 0) {
        const uint32_t r = color ? k.stage[stage].color : k.stage[stage].alpha;
        if (r & (1u << 19)) return true;
        if (r & ((7u << 16) | (3u << 20))) return false;
        int arg = -1;
        if (color) {
            if ((r & 0xfffu) == 0xfffu) arg = (r >> 12) & 15;
            else if ((r & 0xfff0u) == 0xfff0u) arg = r & 15;
            if (arg == 12 || arg == 13 || arg == 15) return true;
            if (arg < 0 || arg >= 8) return false;
            color = !(arg & 1);
            arg /= 2;
        } else {
            if ((r & 0x1ff0u) == 0x1ff0u) arg = (r >> 13) & 7;
            else if ((r & 0xff80u) == 0xff80u) arg = (r >> 4) & 7;
            if (arg == 7) return true;
            if (arg < 0 || arg >= 4) return false;
        }
        --stage;
        while (stage >= 0 && static_cast<int>(bits(color ? k.stage[stage].color : k.stage[stage].alpha, 22, 2)) != arg) --stage;
    }
    return false;
}

void UnitFinal(const FsKey& k, bool verify, bool& color, bool& alpha) {
    bool unit[4][2] = {};
    auto bounded = [&](uint32_t r, bool rgb) {
        if (bits(r, 19, 1)) return true;
        const int arg = UnitCopyArg(r, rgb);
        if (arg < 0) return false;
        if (rgb) return arg == 12 || arg == 13 || arg == 15 || (arg < 8 && unit[arg / 2][arg & 1]);
        return arg == 7 || (arg < 4 && unit[arg][1]);
    };
    for (int i = 0; i < k.numStages; ++i) {
        const auto& st = k.stage[i];
        color = bounded(st.color, true);
        alpha = bounded(st.alpha, false);
        // TEV reads both source components before either destination is updated.
        unit[bits(st.color, 22, 2)][0] = color;
        unit[bits(st.alpha, 22, 2)][1] = alpha;
    }
    if (verify) {
        const bool vc = VerifyUnitFinal(k, true), va = VerifyUnitFinal(k, false);
        if (color != vc || alpha != va) VGX_LOG_N(1, "final-wrap range verifier disagreement; retaining conversion\n");
        color = color && vc;
        alpha = alpha && va;
    }
}

} // namespace

void GenerateFragmentShader(const FsKey& k, ShaderSource& src) {
    Out o;
    // GXV_TEVWRAP=1 repairs out-of-range final TEV bytes before alpha testing.
    // Read once: shader code and its source-hashed compiled cache stay stable per process.
    // GXV_TEVWRAP=2 wraps only the final alpha (enough for the eye repair: its biased alpha decides the
    // alpha test) and leaves RGB clamped as in 1.0, at a quarter of the extra per-pixel work.
    static const long wrapMode = EnvInt("GXV_TEVWRAP", 0);
    static const bool wrapFinal = wrapMode == 1 || wrapMode == 2;
    static const bool wrapRgb = wrapMode == 1;
    // GXV_TEVWRAP_ELIDE is read once: 0/default keeps the original source; 1 elides only
    // proven unit-range copies; 2 independently verifies the proof with fallback.
    static const long elide = EnvInt("GXV_TEVWRAP_ELIDE", 0);
    // GXV_FIX_INDPREV (default 0 until seen on the console: it changes the text of add-previous keys, whose compiled shaders are then not in the shipped cache; 1 = the fix): an indirect stage that adds the previous stage's coordinate
    // (BP 0x10+n bit 20, GX_ITB addprev) adds it in texels, as GX does. The old text added the previous stage's
    // already-normalised coordinate to texel units, so the carried-over part was ~1/texture-size of what it should be.
    static const bool indPrevOn = EnvInt("GXV_FIX_INDPREV", 0) != 0;
    // (only a shader with an add-previous stage changes text; every other indirect shader keeps its source and compiled cache)
    bool indPrev = false;
    for (int s = 0; indPrevOn && s < k.numStages; ++s)
        if (k.stage[s].ind && bits(k.stage[s].ind, 20, 1)) indPrev = true;
    bool unitColor = false, unitAlpha = false;
    if (wrapFinal && (elide == 1 || elide == 2)) UnitFinal(k, elide == 2, unitColor, unitAlpha);
    const FsKey::Stage& last = k.stage[k.numStages - 1];
    const bool wrapColor = wrapRgb && !bits(last.color, 19, 1) && !unitColor;
    const bool wrapAlpha = wrapFinal && !bits(last.alpha, 19, 1) && !unitAlpha;
    uint8_t texMask = 0;
    bool usesCol[2] = {false, false};
    for (int s = 0; s < k.numStages; ++s) {
        if (k.stage[s].texEnable) texMask |= static_cast<uint8_t>(1u << k.stage[s].texMap);
        if (k.stage[s].rasChan < 2) usesCol[k.stage[s].rasChan] = true;
    }
    for (int i = 0; i < k.numIndStages; ++i) texMask |= static_cast<uint8_t>(1u << k.ind[i].texMap);

    o("#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n");
    if (wrapColor || wrapAlpha) {
        // Keep in-range fractions unchanged. This repairs final conversion only, not
        // the existing normalized-float approximation of intermediate GX arithmetic.
        o("float vgx_wrap_final(float value) {\n"
          "  if (value >= 0.0 && value <= 1.0) return value;\n"
          "  float byteValue = floor(value * 255.0 + 0.5);\n"
          "  return (byteValue - 256.0 * floor(byteValue / 256.0)) / 255.0;\n}\n");
    }
    if (k.inWrap) {
        // (GXV_FIX_TEVIN) byte wrap of a register value, as GX's `& 255` on the inputs A, B, C; values in range are returned unchanged
        o("float vgx_w1(float value) {\n"
          "  if (value >= 0.0 && value <= 1.0) return value;\n"
          "  float byteValue = floor(value * 255.0 + 0.5);\n"
          "  return (byteValue - 256.0 * floor(byteValue / 256.0)) / 255.0;\n}\n"
          "vec3 vgx_w3(vec3 value) { return vec3(vgx_w1(value.x), vgx_w1(value.y), vgx_w1(value.z)); }\n");
    }
    for (int m = 0; m < 8; ++m)
        if (texMask & (1u << m)) o("uniform sampler2D u_tex%d;\n", m);
    o("uniform vec4 u_tev[%d];\n", FsTevCount(k));
    if (usesCol[0] || (k.regFromVtx & 1)) o("varying vec4 v_col0;\n");
    if (usesCol[1] || (k.regFromVtx & 2)) o("varying vec4 v_col1;\n");
    for (int i = 0; i < k.numTexGens; ++i) o("varying vec3 v_tex%d;\n", i);
    if (k.fog & kFogVtxBit) o("varying float v_fog;\n");
    o("void main() {\n");
    o("  vec4 prev = u_tev[0], c0 = %s, c1 = %s, c2 = u_tev[3];\n", (k.regFromVtx & 1) ? "v_col0" : "u_tev[1]", (k.regFromVtx & 2) ? "v_col1" : "u_tev[2]");
    o("  vec4 tex = vec4(1.0), ras = vec4(0.0);\n  vec3 kc = vec3(1.0);\n  float ka = 1.0;\n");
    // Texture coordinates (normalised).
    for (int i = 0; i < k.numTexGens; ++i) {
        if (k.texProjMask & (1u << i)) o("  vec2 uv%d = v_tex%d.xy / (v_tex%d.z == 0.0 ? 1.0 : v_tex%d.z);\n", i, i, i, i);
        else o("  vec2 uv%d = v_tex%d.xy;\n", i, i);
    }
    auto Uv = [&](int c) -> std::string {
        if (c >= k.numTexGens) return "vec2(0.0)";
        return "uv" + std::to_string(c);
    };
    // Indirect lookups: three 8-bit offsets per indirect stage, taken from A, B, G.
    for (int i = 0; i < k.numIndStages; ++i) {
        o("  vec3 ind%d = texture2D(u_tex%d, %s * u_tev[%d].xy).abg * 255.0;\n", i, k.ind[i].texMap, Uv(k.ind[i].texCoord).c_str(),
          V_INDSCALE + i);
    }
    if (k.numIndStages) o(indPrev ? "  vec2 itc = vec2(0.0), itx = vec2(0.0);\n" : "  vec2 itc = vec2(0.0);\n");

    for (int s = 0; s < k.numStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        o("  // stage %d\n", s);
        std::string coord = Uv(st.texCoord);
        if (st.ind) {
            // Indirect texture coordinate computation, in texels of this stage's texture.
            const uint32_t ind = st.ind;
            const uint32_t bt = bits(ind, 0, 2), fmt = bits(ind, 2, 2), bias = bits(ind, 4, 3), mid = bits(ind, 9, 4);
            const uint32_t sw = bits(ind, 13, 3), tw = bits(ind, 16, 3), addPrev = bits(ind, 20, 1);
            const int map = st.texMap;
            o("  {\n    vec2 tsz = u_tev[%d].%s;\n", V_TEXSIZE + map / 2, (map & 1) ? "zw" : "xy");
            o("    vec2 tcx = %s * tsz;\n", coord.c_str());
            if (bt < k.numIndStages) o("    vec3 iv = ind%u;\n", bt);
            else o("    vec3 iv = vec3(0.0);\n");
            static const float kMod[4] = {256.0f, 32.0f, 16.0f, 8.0f};
            // Not mod(): vitaGL's GLSL translator maps it to Cg fmod, which truncates instead of flooring.
            if (fmt) o("    iv = iv - %.1f * floor(iv / %.1f);\n", kMod[fmt], kMod[fmt]);
            const float bv = fmt == 0 ? -128.0f : 1.0f;
            o("    iv += vec3(%.1f, %.1f, %.1f);\n", (bias & 1) ? bv : 0.0f, (bias & 2) ? bv : 0.0f, (bias & 4) ? bv : 0.0f);
            if (mid == 0) o("    vec2 io = vec2(0.0);\n");
            else if (mid <= 3) o("    vec2 io = vec2(dot(u_tev[%d].xyz, iv), dot(u_tev[%d].xyz, iv));\n", V_INDMTX + (mid - 1) * 2, V_INDMTX + (mid - 1) * 2 + 1);
            else if (mid >= 5 && mid <= 7) o("    vec2 io = tcx * (iv.x / 256.0) * u_tev[%d].w;\n", V_INDMTX + (mid - 5) * 2);
            else if (mid >= 9 && mid <= 11) o("    vec2 io = tcx * (iv.y / 256.0) * u_tev[%d].w;\n", V_INDMTX + (mid - 9) * 2);
            else o("    vec2 io = vec2(0.0);\n");
            static const float kWrap[8] = {0, 256, 128, 64, 32, 16, 0, 0};
            if (sw == 6) o("    tcx.x = 0.0;\n");
            else if (sw) o("    tcx.x = tcx.x - %.1f * floor(tcx.x / %.1f);\n", kWrap[sw], kWrap[sw]);
            if (tw == 6) o("    tcx.y = 0.0;\n");
            else if (tw) o("    tcx.y = tcx.y - %.1f * floor(tcx.y / %.1f);\n", kWrap[tw], kWrap[tw]);
            if (indPrev) {
                o(addPrev ? "    itx = tcx + io + itx;\n" : "    itx = tcx + io;\n");
                o("    itc = itx / tsz;\n  }\n");
            } else {
                if (addPrev) o("    itc = tcx + io + itc;\n");
                else o("    itc = tcx + io;\n");
                o("    itc = itc / tsz;\n  }\n");
            }
            coord = "itc";
        }
        if (st.texEnable && st.lodBias)
            o("  tex = texture2D(u_tex%d, %s, u_tev[%d].%c).%s;\n", st.texMap, coord.c_str(), V_LODBIAS + st.texMap / 4, "xyzw"[st.texMap & 3],
              Swizzle(st.texSwap).c_str());
        else if (st.texEnable) o("  tex = texture2D(u_tex%d, %s).%s;\n", st.texMap, coord.c_str(), Swizzle(st.texSwap).c_str());
        else if (st.texOff) o("  tex = vec4(1.0);\n");
        if (st.rasChan < 2) o("  ras = v_col%d.%s;\n", st.rasChan, Swizzle(st.rasSwap).c_str());
        else o("  ras = vec4(0.0);\n");
        const uint32_t tc = st.color, ta = st.alpha;
        bool kcUsed = false, kaUsed = false;
        for (int i = 0; i < 4; ++i) {
            if (bits(tc, i * 4, 4) == 14) kcUsed = true;
            if (bits(ta, 4 + i * 3, 3) == 6) kaUsed = true;
        }
        if (kcUsed) o("  kc = %s;\n", KonstColor(st.kcsel).c_str());
        if (kaUsed) o("  ka = %s;\n", KonstAlpha(st.kasel).c_str());
        o("  {\n");
        const bool w = k.inWrap != 0;
        o("    vec3 a = %s, b = %s, c = %s, d = %s;\n", WrapArg(ColorArg(bits(tc, 12, 4)), w && bits(tc, 12, 4) < 8, true).c_str(),
          WrapArg(ColorArg(bits(tc, 8, 4)), w && bits(tc, 8, 4) < 8, true).c_str(), WrapArg(ColorArg(bits(tc, 4, 4)), w && bits(tc, 4, 4) < 8, true).c_str(),
          ColorArg(bits(tc, 0, 4)));
        o("    float aa = %s, ab = %s, ac = %s, ad = %s;\n", WrapArg(AlphaArg(bits(ta, 13, 3)), w && bits(ta, 13, 3) < 4, false).c_str(),
          WrapArg(AlphaArg(bits(ta, 10, 3)), w && bits(ta, 10, 3) < 4, false).c_str(), WrapArg(AlphaArg(bits(ta, 7, 3)), w && bits(ta, 7, 3) < 4, false).c_str(),
          AlphaArg(bits(ta, 4, 3)));
        static const char* const kBias[3] = {"", " + 0.5", " - 0.5"};
        static const char* const kScale[4] = {"", " * 2.0", " * 4.0", " * 0.5"};
        auto Combine = [&](uint32_t reg, bool color) {
            const uint32_t bias = bits(reg, 16, 2), op = bits(reg, 18, 1), clampOn = bits(reg, 19, 1), scale = bits(reg, 20, 2);
            const char* res = color ? "rc" : "ra";
            const char* type = color ? "vec3" : "float";
            const char* A = color ? "a" : "aa"; const char* B = color ? "b" : "ab";
            const char* C = color ? "c" : "ac"; const char* D = color ? "d" : "ad";
            if (bias != 3) {
                o("    %s %s = (%s %s mix(%s, %s, %s)%s)%s;\n", type, res, D, op ? "-" : "+", A, B, C, kBias[bias], kScale[scale]);
            } else {
                // Compare modes: d + ((a OP b) ? c : 0). scale selects the operand width.
                const bool eq = op != 0;
                std::string cond;
                const float e = 0.25f / 255.0f;
                char buf[256];
                switch (scale) {
                case 0:
                    if (eq) std::snprintf(buf, sizeof buf, "(abs(a.r - b.r) < %.6f)", e * 2);
                    else std::snprintf(buf, sizeof buf, "(a.r > b.r + %.6f)", e);
                    break;
                case 1:
                    if (eq) std::snprintf(buf, sizeof buf, "(abs(dot(a.rg - b.rg, vec2(255.0, 65280.0))) < 0.5)");
                    else std::snprintf(buf, sizeof buf, "(dot(a.rg - b.rg, vec2(255.0, 65280.0)) > 0.25)");
                    break;
                case 2:
                    if (eq) std::snprintf(buf, sizeof buf, "(abs(dot(a.rgb - b.rgb, vec3(255.0, 65280.0, 16711680.0))) < 0.5)");
                    else std::snprintf(buf, sizeof buf, "(dot(a.rgb - b.rgb, vec3(255.0, 65280.0, 16711680.0)) > 0.25)");
                    break;
                default:
                    if (color) {
                        // (step, not lessThan / greaterThan: vitaGL's translator maps those to the wrong comparisons)
                        if (eq) std::snprintf(buf, sizeof buf, "(vec3(1.0) - step(vec3(%.6f), abs(a - b)))", e * 2);
                        else std::snprintf(buf, sizeof buf, "(vec3(1.0) - step(a, b + vec3(%.6f)))", e);
                    } else {
                        if (eq) std::snprintf(buf, sizeof buf, "(abs(aa - ab) < %.6f)", e * 2);
                        else std::snprintf(buf, sizeof buf, "(aa > ab + %.6f)", e);
                    }
                    break;
                }
                if (scale == 3 && color) o("    vec3 rc = d + c * %s;\n", buf);
                else o("    %s %s = %s + (%s ? %s : %s(0.0));\n", type, res, D, buf, C, type);
            }
            if (clampOn) o("    %s = clamp(%s, 0.0, 1.0);\n", res, res);
            else o("    %s = clamp(%s, -4.0157, 4.0118);\n", res, res);
        };
        Combine(tc, true);
        Combine(ta, false);
        o("    %s.rgb = rc;\n    %s.a = ra;\n  }\n", RegName(bits(tc, 22, 2)), RegName(bits(ta, 22, 2)));
    }
    if (wrapColor || wrapAlpha) {
        o("  vec4 fin = vec4(%s.rgb, %s.a);\n", RegName(bits(last.color, 22, 2)), RegName(bits(last.alpha, 22, 2)));
        if (wrapColor) o("  fin.rgb = vec3(vgx_wrap_final(fin.r), vgx_wrap_final(fin.g), vgx_wrap_final(fin.b));\n");
        if (wrapAlpha) o("  fin.a = vgx_wrap_final(fin.a);\n");
        o("  fin = clamp(fin, 0.0, 1.0);\n");
    } else {
        o("  vec4 fin = clamp(vec4(%s.rgb, %s.a), 0.0, 1.0);\n", RegName(bits(last.color, 22, 2)), RegName(bits(last.alpha, 22, 2)));
    }
    // GXV_NODISCARD: a test that cannot fail leaves no `discard` in the shader. (On the console's GPU any discard in a shader
    // turns off hidden-surface removal for the draws that use it; the front end already drops the combinations of ALWAYS and
    // NEVER, and with the switch on also the ones its references make constant: BuildFsKey. A key written by an older run
    // can still carry them.)
    if (k.alphaTestOn && !(NoDiscard() && AlphaTestAlwaysPasses(k.alphaTest & 7, -1, (k.alphaTest >> 6) & 3, (k.alphaTest >> 3) & 7, -1))) {
        char b0[96], b1[96], r0[32], r1[32];
        std::snprintf(r0, sizeof r0, "u_tev[%d].x", V_MISC);
        std::snprintf(r1, sizeof r1, "u_tev[%d].y", V_MISC);
        const char* e0 = CompareExpr(k.alphaTest & 7, r0, b0, sizeof b0);
        const char* e1 = CompareExpr((k.alphaTest >> 3) & 7, r1, b1, sizeof b1);
        static const char* const kOp[4] = {"&&", "||", "!=", "=="};
        o("  float ai = floor(fin.a * 255.0 + 0.5);\n");
        o("  if (!(%s %s %s)) discard;\n", e0, kOp[(k.alphaTest >> 6) & 3], e1);
    }
    if (k.fog) {
        const int fsel = k.fog & 7;
        if (k.fog & kFogVtxBit) {
            // (GXV_FOGVTX: linear fog only; the vertex shader's v_fog is affine in clip w / z, u_tev[V_FOG] = c1, -, -, c0: FogVtxCoeffs)
            o("  float fg = clamp(u_tev[%d].x * v_fog + u_tev[%d].w, 0.0, 1.0);\n", V_FOG, V_FOG);
        } else {
            if (k.fog & 8) o("  float fg = u_tev[%d].x * gl_FragCoord.z;\n", V_FOG);
            else o("  float fg = u_tev[%d].x / max(u_tev[%d].y - gl_FragCoord.z, 0.000001);\n", V_FOG, V_FOG);
            o("  fg = clamp(fg - u_tev[%d].z, 0.0, 1.0);\n", V_FOG);
        }
        switch (fsel) {
        case 4: o("  fg = 1.0 - exp2(-8.0 * fg);\n"); break;
        case 5: o("  fg = 1.0 - exp2(-8.0 * fg * fg);\n"); break;
        case 6: o("  fg = exp2(-8.0 * (1.0 - fg));\n"); break;
        case 7: o("  fg = 1.0 - fg; fg = exp2(-8.0 * fg * fg);\n"); break;
        default: break;
        }
        o("  fin.rgb = mix(fin.rgb, u_tev[%d].rgb, fg);\n", V_FOGCOL);
    }
    if (k.dstAlpha) o("  fin.a = u_tev[%d].z;\n", V_MISC);
    if (k.logicSrc == 1) o("  fin.rgb = vec3(1.0) - fin.rgb;\n");
    else if (k.logicSrc == 2) o("  fin.rgb = vec3(1.0);\n");
    o("  gl_FragColor = fin;\n}\n");
    src.text = std::move(o.s);
    src.texMask = texMask;
    src.numTexGens = k.numTexGens;
    src.nTev = static_cast<uint8_t>(FsTevCount(k));
}

namespace {
// The palette row a vertex names (a_pos.w, a_tmi*: whole numbers stored as floats) as an int.
//
// The cast must be given a WHOLE number. The first console run drew every skinned character
// as a head with streaks below it (hw/run3-gl) with `int(a_pos.w + 0.5)` here: Sony's compiler
// turns a float-to-int cast into pack instructions (VPCKU16F32 / VPCKS16F32, no scaling)
// WITHOUT truncating first, and on the console's GPU a pack rounds to the nearest integer
// (Imagination's assembler: "Only nearest rounding is supported on this processor" for every
// core but the SGX545; its own compiler front end puts a floor / truncate in front of the
// conversion for that reason, usc2/icvt_core.c ExpandMOV). An exact x.5 therefore became
// x + 1 for some or all x: rows of the NEXT matrix element. The emulator translates the same
// pack as a C cast (truncation), so there it was right. With a whole number in the cast every
// rounding gives the same result; floor(w + 0.5) also absorbs any error below a half.
//   GXV_PALIDX=1  (default) int(floor(w + 0.5))
//   GXV_PALIDX=0  the old text, int(w + 0.5): wrong on the console (to see the fault again)
//   GXV_PALIDX=2  int(w): the form the THUG port's skinning shader uses on consoles
//   GXV_PALIDX=3 / 4  (PC only, to compare pictures with a console's) what the old text is
//                     believed to do on the console: 3 = nearest, ties to even; 4 = ties up
int PalIndexMode() {
    static const int m = static_cast<int>(EnvInt("GXV_PALIDX", 1));
    return m;
}
void RowIndex(Out& o, const char* ind, const char* var, const char* w) {
    switch (PalIndexMode()) {
    case 0: o("%sint %s = int(%s + 0.5);\n", ind, var, w); break;
    case 2: o("%sint %s = int(%s);\n", ind, var, w); break;
    case 3:
        // (fract(x * 0.5) is 0.5 exactly for an odd whole x)
        o("%sfloat %sf = floor(%s + 0.5);\n%sif (fract(%sf * 0.5) > 0.25) %sf += 1.0;\n%sint %s = int(%sf);\n", ind, var, w, ind, var, var, ind, var, var);
        break;
    case 4: o("%sint %s = int(floor(%s + 0.5) + 1.0);\n", ind, var, w); break;
    default: o("%sint %s = int(floor(%s + 0.5));\n", ind, var, w); break;
    }
}

} // namespace

void GenerateVertexShader(const VsKey& k, ShaderSource& src) {
    Out o;
    bool needNrm = false, needLights = false, needCol[2] = {false, false}, needPal = k.perVtxPos != 0;
    bool needTm = false, needPtm = false, needTmi = false;
    uint8_t texSrcMask = 0;
    int maxLight = -1;
    for (int i = 0; i < 4; ++i) {
        const VsKey::Chan& c = k.chan[i];
        if ((i & 1) >= k.numChans) continue;
        if (c.matSrcVtx || (c.enable && c.ambSrcVtx)) needCol[i & 1] = true;
        if (c.enable && c.lightMask) {
            needLights = true;
            if (c.diffuse || c.attn == 1) needNrm = true;
            for (int l = 0; l < 8; ++l)
                if (c.lightMask & (1u << l)) maxLight = l > maxLight ? l : maxLight;
        }
    }
    if (k.numChans < 1) needCol[0] = true;
    if (k.numChans < 2) needCol[1] = true;
    for (int i = 0; i < k.numTexGens; ++i) {
        const VsKey::TexGen& t = k.tg[i];
        if (t.type >= 2) continue;
        if (t.src == 1) needNrm = true;
        if (t.src >= 5 && t.src <= 12) texSrcMask |= static_cast<uint8_t>(1u << (t.src - 5));
        if (t.perVtxMtx) { needPal = true; needTmi = true; }
        else needTm = true;
        if (t.post) needPtm = true;
    }

    o("attribute vec4 a_pos;\n");
    if (needNrm) o("attribute vec3 a_nrm;\n");
    if (needCol[0]) o("attribute vec4 a_col0;\n");
    if (needCol[1]) o("attribute vec4 a_col1;\n");
    for (int t = 0; t < 8; ++t)
        if (texSrcMask & (1u << t)) o("attribute vec2 a_tex%d;\n", t);
    if (needTmi) o("attribute vec4 a_tmi0;\nattribute vec4 a_tmi1;\n");
    // GXV_FIX_LINES (VsKey::lpMode): the corner of the quad a line or point is widened into, and for a line the other end point.
    // The slots are ones no texgen of this key reads (the front end picks them).
    const int lpMode = (k.lpMode() == 1 || k.lpMode() == 2) && k.lpBase() + (k.lpMode() == 1 ? 3 : 1) <= 8 ? k.lpMode() : 0;
    const int lpB = k.lpBase();
    if (lpMode == 1) o("attribute vec2 a_tex%d;\nattribute vec2 a_tex%d;\nattribute vec2 a_tex%d;\n", lpB, lpB + 1, lpB + 2);
    else if (lpMode == 2) o("attribute vec2 a_tex%d;\n", lpB);
    o("uniform mat4 u_proj;\n");
    const bool shortPal = k.perVtxPos == 2 || k.perVtxPos == 3;
    const int palK = PalRowsOfKey(k.perVtxPos); // rows of the short palette's position half (its normal half starts there)
    if (needPal) o("uniform vec4 u_pal[%d];\n", k.perVtxPos == 3 ? kPalRowsSmall : shortPal ? kPalRowsShort : kPalRowsFull);
    if (!k.perVtxPos) o("uniform vec4 u_mv[6];\n");
    if (needTm) o("uniform vec4 u_tm[%d];\n", k.numTexGens * 3);
    if (needPtm) o("uniform vec4 u_ptm[%d];\n", k.numTexGens * 3);
    o("uniform vec4 u_lt[%d];\n", 4 + (maxLight + 1) * 5);
    o("varying vec4 v_col0;\nvarying vec4 v_col1;\n");
    for (int i = 0; i < k.numTexGens; ++i) o("varying vec3 v_tex%d;\n", i);
    if (k.fogVtx & 1) o("varying float v_fog;\n");
    o("void main() {\n  vec4 p = vec4(a_pos.xyz, 1.0);\n");
    if (k.perVtxPos) {
        RowIndex(o, "  ", "pi", "a_pos.w");
        if (shortPal) o("  if (pi > %d) pi = %d;\n", palK - 3, palK - 3);
        else o("  if (pi > 61) pi = 61;\n");
        o("  vec3 eye = vec3(dot(u_pal[pi], p), dot(u_pal[pi + 1], p), dot(u_pal[pi + 2], p));\n");
        if (needNrm) {
            if (shortPal) o("  int ni = %d + pi;\n", palK); // (position rows 0..K-1, then normal rows 0..K-1)
            else o("  int ni = pi > 29 ? 93 : 64 + pi;\n");
            o("  vec3 n = vec3(dot(u_pal[ni].xyz, a_nrm), dot(u_pal[ni + 1].xyz, a_nrm), dot(u_pal[ni + 2].xyz, a_nrm));\n");
        }
    } else {
        o("  vec3 eye = vec3(dot(u_mv[0], p), dot(u_mv[1], p), dot(u_mv[2], p));\n");
        if (needNrm) o("  vec3 n = vec3(dot(u_mv[3].xyz, a_nrm), dot(u_mv[4].xyz, a_nrm), dot(u_mv[5].xyz, a_nrm));\n");
    }
    if (needNrm) o("  float nl = length(n);\n  if (nl > 0.0) n = n / nl;\n");
    o("  gl_Position = u_proj * vec4(eye, 1.0);\n");
    if (lpMode == 1) {
        // GX widens a line along the minor axis of its screen-space direction (x-major lines vertically, y-major horizontally; a tie is
        // x-major), by lc = +-width / viewport size in NDC on each side. The offset is applied in clip space (times w), so it is exact under
        // any transform. An end point behind the near plane is moved onto it along the line first: the two triangles of a quad with a
        // corner behind the eye (w < 0) do not tile the ribbon any more. (Colours and texture coordinates keep the end point's own values there.) The direction comes from the two end points' NDC positions.
        o("  vec4 p2 = vec4(a_tex%d.x, a_tex%d.y, a_tex%d.x, 1.0);\n", lpB, lpB, lpB + 1);
        o("  vec2 lc = a_tex%d;\n", lpB + 2);
        if (k.perVtxPos) {
            char w2[24];
            std::snprintf(w2, sizeof w2, "a_tex%d.y", lpB + 1);
            RowIndex(o, "  ", "pj", w2);
            if (shortPal) o("  if (pj > %d) pj = %d;\n", palK - 3, palK - 3);
            else o("  if (pj > 61) pj = 61;\n");
            o("  vec3 eye2 = vec3(dot(u_pal[pj], p2), dot(u_pal[pj + 1], p2), dot(u_pal[pj + 2], p2));\n");
        } else {
            o("  vec3 eye2 = vec3(dot(u_mv[0], p2), dot(u_mv[1], p2), dot(u_mv[2], p2));\n");
        }
        o("  vec4 c1 = u_proj * vec4(eye2, 1.0);\n  vec4 q0 = gl_Position;\n  vec4 q1 = c1;\n");
        o("  float d0 = q0.z + q0.w;\n  float d1 = q1.z + q1.w;\n");
        o("  if (d0 < 0.0 && d1 >= 0.0) q0 = gl_Position + (c1 - gl_Position) * (d0 / (d0 - d1));\n");
        o("  if (d1 < 0.0 && d0 >= 0.0) q1 = c1 + (gl_Position - c1) * (d1 / (d1 - d0));\n");
        o("  vec2 dn = vec2(0.0);\n  if (q0.w > 0.0 && q1.w > 0.0) dn = abs(q1.xy / q1.w - q0.xy / q0.w);\n");
        o("  float tall = (dn.y * abs(lc.x) > dn.x * abs(lc.y)) ? 1.0 : 0.0;\n");
        o("  gl_Position = q0;\n");
        o("  gl_Position.xy += vec2(lc.x * tall, lc.y * (1.0 - tall)) * q0.w;\n");
    } else if (lpMode == 2) {
        // A point is an axis-aligned square of side size / 6 pixels around its centre; lc is the corner's offset.
        o("  vec2 lc = a_tex%d;\n  gl_Position.xy += lc * gl_Position.w;\n", lpB);
    }
    if (k.fogVtx & 1) o("  v_fog = gl_Position.%c;\n", (k.fogVtx & 2) ? 'z' : 'w');

    // Lighting channels.
    for (int ch = 0; ch < 2; ++ch) {
        if (ch >= k.numChans) {
            o("  vec4 col%d = a_col%d;\n", ch, ch);
            continue;
        }
        o("  vec4 col%d;\n  {\n", ch);
        {
            const VsKey::Chan& cc = k.chan[ch];
            const VsKey::Chan& ca = k.chan[ch + 2];
            if (LightMerge() && cc.enable && ca.enable && cc.lightMask && cc.lightMask == ca.lightMask && cc.attn == ca.attn &&
                cc.diffuse == ca.diffuse) {
                // Colour and alpha light the same way: one pass over the lights for rgb and a together.
                char c0[32], c1[32];
                std::snprintf(c0, sizeof c0, cc.ambSrcVtx ? "a_col%d" : "u_lt[%d]", ch);
                std::snprintf(c1, sizeof c1, ca.ambSrcVtx ? "a_col%d" : "u_lt[%d]", ch);
                if (cc.ambSrcVtx == ca.ambSrcVtx) o("    vec4 lm = %s;\n", c0);
                else o("    vec4 lm = vec4(%s.rgb, %s.a);\n", c0, c1);
                for (int l = 0; l < 8; ++l) {
                    if (!(cc.lightMask & (1u << l))) continue;
                    const int b = 4 + l * 5;
                    o("    {\n      vec3 ld = u_lt[%d].xyz - eye;\n      float d2 = dot(ld, ld);\n      float d1 = sqrt(d2);\n"
                      "      if (d1 > 0.0) ld = ld / d1;\n      float at = 1.0;\n", b);
                    if (cc.attn == 2) {
                        o("      float ca = max(0.0, dot(ld, u_lt[%d].xyz));\n", b + 1);
                        o("      at = max(0.0, u_lt[%d].x + u_lt[%d].y * ca + u_lt[%d].z * ca * ca) / dot(u_lt[%d].xyz, vec3(1.0, d1, d2));\n", b + 3, b + 3,
                          b + 3, b + 4);
                    } else if (cc.attn == 1) {
                        o("      float ca = dot(n, ld) >= 0.0 ? max(0.0, dot(n, u_lt[%d].xyz)) : 0.0;\n", b + 1);
                        if (cc.diffuse == 0) o("      vec3 da = u_lt[%d].xyz;\n", b + 4);
                        else o("      vec3 da = normalize(u_lt[%d].xyz);\n", b + 4);
                        o("      at = max(0.0, dot(u_lt[%d].xyz, vec3(1.0, ca, ca * ca))) / dot(da, vec3(1.0, ca, ca * ca));\n", b + 3);
                    }
                    if (cc.diffuse == 1) o("      at *= dot(ld, n);\n");
                    else if (cc.diffuse == 2) o("      at *= max(0.0, dot(ld, n));\n");
                    o("      lm += at * u_lt[%d];\n    }\n", b + 2);
                }
                o("    lm = clamp(lm, 0.0, 1.0);\n");
                if (cc.matSrcVtx) o("    col%d.rgb = a_col%d.rgb * lm.rgb;\n", ch, ch);
                else o("    col%d.rgb = u_lt[%d].rgb * lm.rgb;\n", ch, 2 + ch);
                if (ca.matSrcVtx) o("    col%d.a = a_col%d.a * lm.a;\n", ch, ch);
                else o("    col%d.a = u_lt[%d].a * lm.a;\n", ch, 2 + ch);
                o("  }\n");
                continue;
            }
        }
        for (int part = 0; part < 2; ++part) { // 0 colour, 1 alpha
            const VsKey::Chan& c = k.chan[ch + part * 2];
            const char* sw = part ? "a" : "rgb";
            const char* ty = part ? "float" : "vec3";
            const char* nm = part ? "la" : "lc";
            if (c.enable) {
                if (c.ambSrcVtx) o("    %s %s = a_col%d.%s;\n", ty, nm, ch, sw);
                else o("    %s %s = u_lt[%d].%s;\n", ty, nm, ch, sw);
                for (int l = 0; l < 8; ++l) {
                    if (!(c.lightMask & (1u << l))) continue;
                    const int b = 4 + l * 5; // pos, dir, col, cosatt, distatt
                    o("    {\n      vec3 ld = u_lt[%d].xyz - eye;\n      float d2 = dot(ld, ld);\n      float d1 = sqrt(d2);\n"
                      "      if (d1 > 0.0) ld = ld / d1;\n      float at = 1.0;\n", b);
                    if (c.attn == 2) {
                        o("      float ca = max(0.0, dot(ld, u_lt[%d].xyz));\n", b + 1);
                        o("      at = max(0.0, u_lt[%d].x + u_lt[%d].y * ca + u_lt[%d].z * ca * ca) / dot(u_lt[%d].xyz, vec3(1.0, d1, d2));\n", b + 3, b + 3,
                          b + 3, b + 4);
                    } else if (c.attn == 1) {
                        o("      float ca = dot(n, ld) >= 0.0 ? max(0.0, dot(n, u_lt[%d].xyz)) : 0.0;\n", b + 1);
                        if (c.diffuse == 0) o("      vec3 da = u_lt[%d].xyz;\n", b + 4);
                        else o("      vec3 da = normalize(u_lt[%d].xyz);\n", b + 4);
                        o("      at = max(0.0, dot(u_lt[%d].xyz, vec3(1.0, ca, ca * ca))) / dot(da, vec3(1.0, ca, ca * ca));\n", b + 3);
                    }
                    if (c.diffuse == 1) o("      at *= dot(ld, n);\n");
                    else if (c.diffuse == 2) o("      at *= max(0.0, dot(ld, n));\n");
                    o("      %s += at * u_lt[%d].%s;\n    }\n", nm, b + 2, sw);
                }
                o("    %s = clamp(%s, 0.0, 1.0);\n", nm, nm);
            } else {
                o("    %s %s = %s(1.0);\n", ty, nm, ty);
            }
            if (c.matSrcVtx) o("    col%d.%s = a_col%d.%s * %s;\n", ch, sw, ch, sw, nm);
            else o("    col%d.%s = u_lt[%d].%s * %s;\n", ch, sw, 2 + ch, sw, nm);
        }
        o("  }\n");
    }
    o("  v_col0 = col0;\n  v_col1 = col1;\n");

    // Texture coordinate generation.
    for (int i = 0; i < k.numTexGens; ++i) {
        const VsKey::TexGen& t = k.tg[i];
        if (t.type >= 2) {
            o("  v_tex%d = vec3(col%d.rg, 1.0);\n", i, t.type - 2);
            continue;
        }
        o("  {\n");
        if (t.src == 0) o("    vec4 srcv = vec4(a_pos.xy, %s, 1.0);\n", t.inputAbc1 ? "a_pos.z" : "1.0");
        else if (t.src == 1) o("    vec4 srcv = vec4(a_nrm.xy, %s, 1.0);\n", t.inputAbc1 ? "a_nrm.z" : "1.0");
        else if (t.src >= 5 && t.src <= 12) o("    vec4 srcv = vec4(a_tex%d, 1.0, 1.0);\n", t.src - 5);
        else o("    vec4 srcv = vec4(0.0, 0.0, 1.0, 1.0);\n");
        if (t.perVtxMtx) {
            char tmi[16];
            std::snprintf(tmi, sizeof tmi, "a_tmi%d.%c", i / 4, "xyzw"[i & 3]);
            RowIndex(o, "    ", "ti", tmi);
            o("    if (ti > 61) ti = 61;\n");
            if (t.proj) o("    vec3 tc = vec3(dot(u_pal[ti], srcv), dot(u_pal[ti + 1], srcv), dot(u_pal[ti + 2], srcv));\n");
            else o("    vec3 tc = vec3(dot(u_pal[ti], srcv), dot(u_pal[ti + 1], srcv), 1.0);\n");
        } else {
            if (t.proj) o("    vec3 tc = vec3(dot(u_tm[%d], srcv), dot(u_tm[%d], srcv), dot(u_tm[%d], srcv));\n", i * 3, i * 3 + 1, i * 3 + 2);
            else o("    vec3 tc = vec3(dot(u_tm[%d], srcv), dot(u_tm[%d], srcv), 1.0);\n", i * 3, i * 3 + 1);
        }
        if (t.post) {
            if (t.post == 2) o("    float tl = length(tc);\n    if (tl > 0.0) tc = tc / tl;\n");
            o("    vec4 q = vec4(tc, 1.0);\n    tc = vec3(dot(u_ptm[%d], q), dot(u_ptm[%d], q), dot(u_ptm[%d], q));\n", i * 3, i * 3 + 1, i * 3 + 2);
        }
        o("    v_tex%d = tc;\n  }\n", i);
    }
    // GXV_FIX_LINES: GXEnableTexOffsets. A line's far side (the +side of lc) is moved by the GX_TO_* amount in s; a point's right corners
    // in s and its bottom corners in t. (Dolphin VideoCommon does the same, on the NDC offset's sign.)
    if (lpMode && k.lpAmount() >= 1 && k.lpAmount() <= 5 && k.lpTexMask()) {
        static const char* const kAmt[6] = {"0.0", "0.0625", "0.125", "0.25", "0.5", "1.0"};
        const char* a = kAmt[k.lpAmount()];
        for (int i = 0; i < k.numTexGens; ++i) {
            if (!(k.lpTexMask() & (1u << i))) continue;
            o("  if (lc.x > 0.0) v_tex%d.x += %s;\n", i, a);
            if (lpMode == 2) o("  if (lc.y > 0.0) v_tex%d.y += %s;\n", i, a);
        }
    }
    o("}\n");
    src.text = std::move(o.s);
    src.usesPalette = needPal;
    src.usesLights = needLights;
    src.numTexGens = k.numTexGens;
    src.nMv = k.perVtxPos ? 0 : 6;
    src.nTm = needTm ? static_cast<uint8_t>(k.numTexGens * 3) : 0;
    src.nPtm = needPtm ? static_cast<uint8_t>(k.numTexGens * 3) : 0;
    src.nPal = needPal ? static_cast<uint8_t>(k.perVtxPos == 3 ? kPalRowsSmall : shortPal ? kPalRowsShort : kPalRowsFull) : 0;
    src.nLt = static_cast<uint8_t>(4 + (maxLight + 1) * 5);
}

} // namespace vgx
