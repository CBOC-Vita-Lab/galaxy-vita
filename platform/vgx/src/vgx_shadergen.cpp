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
extern "C" void port_log_summary(const char*, ...) __attribute__((weak, format(printf, 1, 2))); // (the Vita's basic log; absent on a host)

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdlib>

namespace vgx {

uint64_t HashKey(const void* key, size_t size) { return XXH3_64bits(key, size); }

bool UniformPacking() {
    const bool on = VGX_KNOB(EnvInt("GXV_UNIPACK", 1) != 0) != 0; // (GXV_FE_KNOBS: no barrier, see vgx_common.h)
    return on;
}
// GXV_LTMERGE (read once, default 0): a lit channel whose colour and alpha parts have the same light mask, attenuation and diffuse
// function runs each light once for a vec4 (rgb and a together) instead of twice (vec3, then float): the same sums in the same order.
static bool LightMerge() {
    static const bool on = EnvInt("GXV_LTMERGE", 0) != 0;
    return on;
}
long PaletteRowsMode() {
    const long m = static_cast<long>(VGX_KNOB(EnvInt("GXV_PALROWS", 0))); // see vgx_shadergen.h
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
bool DiscardBlend() {
    static const bool on = EnvInt("GXV_DISCARD_BLEND", 0) != 0; // 0: `discard` for every alpha test, as before
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
// GXV_EFBSWAP2 (ShaderSource::texChan): the texture channels a fragment shader of this key reads, bit 4 * texmap + channel, as the
// generator below writes it: a stage with its texture on samples it into `tex` through its swap table, and `tex` keeps that value
// for later stages until another one samples (or GXV_FIX_TEXOFF sets it to white); a colour input TEXC reads the swapped r, g, b,
// TEXA (colour or alpha input) the swapped a. Every input a stage selects counts, used by its operation or not. An indirect
// lookup reads its map's a, b, g: all four counted.
uint32_t FsTexChannels(const FsKey& k) {
    uint32_t chan = 0;
    int map = -1;       // the texmap `tex` was last sampled from (-1: none, or white)
    uint8_t swap = 0xE4;
    for (int s = 0; s < k.numStages && s < 16; ++s) {
        const FsKey::Stage& st = k.stage[s];
        if (st.texEnable) { map = st.texMap & 7; swap = st.texSwap; }
        else if (st.texOff) map = -1;
        bool rgb = false, a = false;
        for (int i = 0; i < 4; ++i) {
            const uint32_t c = bits(st.color, i * 4, 4);
            if (c == 8) rgb = true;
            if (c == 9) a = true;
            if (bits(st.alpha, 4 + i * 3, 3) == 4) a = true;
        }
        if (map < 0) continue;
        for (int j = 0; j < 4; ++j)
            if ((j < 3 && rgb) || (j == 3 && a)) chan |= 1u << (4 * map + ((swap >> (2 * j)) & 3));
    }
    for (int i = 0; i < k.numIndStages && i < 4; ++i) chan |= 0xFu << (4 * (k.ind[i].texMap & 7));
    return chan;
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

// A float literal that is exactly v (GXV_IND_VS: powers of two and their small multiples, at most 24 binary places): written in fixed
// point with every decimal digit (2^-n has exactly n), trailing zeros dropped.
std::string ExactLit(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.24f", v);
    std::string r = buf;
    while (r.size() > 2 && r.back() == '0' && r[r.size() - 2] != '.') r.pop_back();
    return r;
}

const char* const kSwz = "rgba";
std::string Swizzle(uint8_t swap) {
    std::string r;
    for (int i = 0; i < 4; ++i) r += kSwz[(swap >> (i * 2)) & 3];
    return r;
}

// UB_TEV as vec4 indices
enum { V_REG = 0, V_KONST = 4, V_MISC = 8, V_FOG = 9, V_FOGCOL = 10, V_INDMTX = 11, V_INDSCALE = 17, V_TEXSIZE = 21, V_LODBIAS = 25 };

// (t3: the three-component type the shader declares its colours with, "vec3", or "hvec3" for a half precision shader)
std::string ColorArg(uint32_t a, const char* t3) {
    static const char* const t[16] = {"prev.rgb", "prev.aaa", "c0.rgb", "c0.aaa", "c1.rgb", "c1.aaa", "c2.rgb", "c2.aaa",
                                      "tex.rgb", "tex.aaa", "ras.rgb", "ras.aaa", nullptr, nullptr, "kc", nullptr};
    switch (a & 15) {
    case 12: return std::string(t3) + "(1.0)";
    case 13: return std::string(t3) + "(0.5)";
    case 15: return std::string(t3) + "(0.0)";
    default: return t[a & 15];
    }
}
// The colour / alpha input with the low-8-bit wrap (FsKey::inWrap bit 0) on register reads (selectors 0..7 / 0..3).
std::string WrapArg(const std::string& plain, bool reg, bool color) {
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
std::string KonstColor(uint8_t sel, const char* t3) {
    char b[64];
    if (sel < 8) { std::snprintf(b, sizeof b, "%s(%.3f)", t3, (8 - sel) / 8.0); return b; }
    if (sel < 12) return std::string(t3) + "(0.0)";
    const int k = sel & 3;
    static const char* const comp[5] = {"rgb", "rrr", "ggg", "bbb", "aaa"};
    std::snprintf(b, sizeof b, "u_tev[%d].%s", V_KONST + k, comp[(sel - 12) >> 2]);
    return b;
}
std::string KonstAlpha(uint8_t sel, bool half) {
    char b[64];
    if (sel < 8) { std::snprintf(b, sizeof b, half ? "hfloat(%.3f)" : "%.3f", (8 - sel) / 8.0); return b; }
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

// The final-value wrap GXV_TEVWRAP asks for, for this key (GenerateFragmentShader and the half precision analysis both read it).
// GXV_TEVWRAP=1 repairs out-of-range final TEV bytes before alpha testing.
// Read once: shader code and its source-hashed compiled cache stay stable per process.
// GXV_TEVWRAP=2 wraps only the final alpha (enough for the eye repair: its biased alpha decides the
// alpha test) and leaves RGB clamped as in 1.0, at a quarter of the extra per-pixel work.
// GXV_TEVWRAP_ELIDE is read once: 0/default keeps the original source; 1 elides only
// proven unit-range copies; 2 independently verifies the proof with fallback.
void FinalWrapPlan(const FsKey& k, bool& wrapColor, bool& wrapAlpha) {
    static const long wrapMode = EnvInt("GXV_TEVWRAP", 0);
    static const bool wrapFinal = wrapMode == 1 || wrapMode == 2;
    static const bool wrapRgb = wrapMode == 1;
    static const long elide = EnvInt("GXV_TEVWRAP_ELIDE", 0);
    bool unitColor = false, unitAlpha = false;
    if (wrapFinal && (elide == 1 || elide == 2)) UnitFinal(k, elide == 2, unitColor, unitAlpha);
    const FsKey::Stage& last = k.stage[k.numStages - 1];
    wrapColor = wrapRgb && !bits(last.color, 19, 1) && !unitColor;
    wrapAlpha = wrapFinal && !bits(last.alpha, 19, 1) && !unitAlpha;
}
// GXV_FS_TRIM: vgx_wrap_final returns a value in 0..1 as it is, so a final part the range analysis proves to be in 0..1 needs no wrap
// (with GXV_TEVWRAP=2, the console default, every key whose last alpha stage does not clamp carries one: two compares, a branch and the
// byte arithmetic per pixel). The half analysis takes the same plan, so a key that loses its wrap may become half precision.
void FinalWrapPlanTrim(const FsKey& k, const FsTrimInfo* ti, bool& wrapColor, bool& wrapAlpha) {
    FinalWrapPlan(k, wrapColor, wrapAlpha);
    if (!ti) return;
    if (!ti->finC.lo && !ti->finC.hi) wrapColor = false;
    if (!ti->finA.lo && !ti->finA.hi) wrapAlpha = false;
}

// ---- GXV_FS_HALF: which fragment shaders may run in half precision --------------------------------------------------------------
//
// The half shader keeps texture coordinates (varyings v_texN, uvN, the projective divide), the alpha-test arithmetic and gl_FragColor in
// float. Its TEV colour variables (registers, tex, ras, kc, ka, a b c d, the stage results, fin) and the u_tev uniform array are half
// (10-bit mantissa, unit round-off u = 2^-11, |fl16(x) - x| <= u |x|); every arithmetic operation rounds its result to half. What the
// analysis below does is a forward error analysis of exactly the operations GenerateFragmentShader emits, on the keys it accepts:
//   * each value is an interval [lo, hi] with an absolute error bound e (the largest |half value - float value|);
//   * inputs: texture samples, rasterised colours, TEV registers and konst colours are in 0..1 (the texture unit and the lighting
//     are unit-range; the registers are, as long as the front end has not set FsKey::inWrap bit 1) and are rounded to half once (e = u);
//     0, 0.5, 1 and n/8 are exact;
//   * a + b, a - b, a * b round their result (e += u * max|result|); an operand that is exactly 0 (or, for a product, exactly 1) makes
//     the operation exact; the scales 2, 4 and 0.5 are exact; clamp does not grow the error;
//   * mix(a, b, c) is bounded as the worse of its two usual code forms a + c * (b - a) and a * (1 - c) + b * c;
//   * the bound of the final value is e * 255 in 8-bit levels. The float shader's own value is taken as exact (its error is 2^-24).
// If |x - y| <= 1 level, round(255 x) and round(255 y) differ by at most 1; so a bound of 1 level means "at most one level off in any
// pixel". Whether the same level is hit depends on where the float value sits between two levels: see tests/vgx_fshalf_test.cpp for
// how often that is.
//
// Keys the analysis does not take (the first one found is reported):
//   indirect stages       texel-unit arithmetic, coordinates times 256 and wrapped, offsets from textures
//   fog                   the factor c1 * w + c0 needs float parameters (large terms that cancel), and u_tev is one half array
//   compare-mode stages   (a > b) and (a == b) with a 0.25 / 255 tolerance decide on differences far below half precision
//   final wrap            vgx_wrap_final: byteValue = value * 255 up to 1023 has a spacing of 0.5 in half
//   inWrap                a register outside 0..255 (range assumption of the analysis, and TEVIN wrap code)
//   logicSrc              output inverted or whitened (not worth another operation to prove)
// Not a reason: texture size or projective coordinates (float), alpha-test references (integers 0..255 are exact in half), a texture LOD
// bias (a multiple of 1/32 in -4..4: exact), ztex (unused by the generator), dstAlpha (an exact 8-bit constant), regFromVtx.
constexpr double kU16 = 1.0 / 2048.0;     // unit round-off of half
constexpr double kTiny16 = 1.0 / 33554432.0; // 2^-25: largest rounding error of a half subnormal result

struct Ab {
    double lo, hi, e;
    double mag() const { return std::max(std::fabs(lo), std::fabs(hi)); }
    bool zero() const { return lo == 0.0 && hi == 0.0 && e == 0.0; }
    bool one() const { return lo == 1.0 && hi == 1.0 && e == 0.0; }
};
Ab AbExact(double v) { return {v, v, 0.0}; }
Ab AbIn(double lo, double hi) { // a float value rounded to half
    Ab r{lo, hi, 0.0};
    r.e = kU16 * r.mag() + kTiny16;
    return r;
}
double AbRound(double lo, double hi) { return kU16 * std::max(std::fabs(lo), std::fabs(hi)) + kTiny16; }
Ab AbAdd(const Ab& a, const Ab& b) {
    if (a.zero()) return b;
    if (b.zero()) return a;
    Ab r{a.lo + b.lo, a.hi + b.hi, a.e + b.e};
    r.e += AbRound(r.lo, r.hi);
    return r;
}
Ab AbSub(const Ab& a, const Ab& b) {
    if (b.zero()) return a;
    if (a.zero()) return {-b.hi, -b.lo, b.e};
    Ab r{a.lo - b.hi, a.hi - b.lo, a.e + b.e};
    r.e += AbRound(r.lo, r.hi);
    return r;
}
Ab AbMul(const Ab& a, const Ab& b) {
    if (a.zero() || b.zero()) return AbExact(0.0);
    if (a.one()) return b;
    if (b.one()) return a;
    const double p[4] = {a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi};
    Ab r{std::min(std::min(p[0], p[1]), std::min(p[2], p[3])), std::max(std::max(p[0], p[1]), std::max(p[2], p[3])), 0.0};
    r.e = a.mag() * b.e + b.mag() * a.e + a.e * b.e + AbRound(r.lo, r.hi);
    return r;
}
Ab AbScale(const Ab& a, double f) { // by a power of two: exact
    Ab r{std::min(a.lo * f, a.hi * f), std::max(a.lo * f, a.hi * f), a.e * std::fabs(f)};
    return r;
}
Ab AbClamp(const Ab& a, double lo, double hi) {
    auto C = [&](double v) { return std::min(std::max(v, lo), hi); };
    Ab r{C(a.lo), C(a.hi), a.e};
    r.e = std::min(r.e, C(a.hi + a.e) - C(a.lo - a.e)); // both values are inside the clamp range
    return r;
}
// mix(a, b, c): the worse of  a + c * (b - a)  and  a * (1 - c) + b * c;  the range is that of a convex combination when c is in 0..1
Ab AbMix(const Ab& a, const Ab& b, const Ab& c) {
    const Ab m1 = AbAdd(a, AbMul(c, AbSub(b, a)));
    const Ab m2 = AbAdd(AbMul(a, AbSub(AbExact(1.0), c)), AbMul(b, c));
    Ab r = m1.e >= m2.e ? m1 : m2;
    if (c.lo >= 0.0 && c.hi <= 1.0) {
        r.lo = std::min(a.lo, b.lo);
        r.hi = std::max(a.hi, b.hi);
    }
    return r;
}

struct HalfRegs { Ab rgb, a; };

} // namespace

// ---- GXV_FS_TRIM: which clamps of a TEV shader can act (see vgx_shadergen.h) -------------------------------------------------------
long FsTrimMode() {
    static const long m = EnvInt("GXV_FS_TRIM", 0) == 1 ? 1 : 0;
    return m;
}
// GXV_FS_TRIM_PARTS (read once with GXV_FS_TRIM=1; see vgx_shadergen.h kFsTrim*). Logged once to the basic log.
long FsTrimParts() {
    static const long p = [] {
        long v = EnvInt("GXV_FS_TRIM_PARTS", kFsTrimPartsDefault);
        if (v < 0 || v > kFsTrimAll) v = kFsTrimPartsDefault;
        if (FsTrimMode() && port_log_summary)
            port_log_summary("fstrim: GXV_FS_TRIM=1 GXV_FS_TRIM_PARTS=%ld (1 colour stage clamps, 2 alpha-test floor, 4 final wrap, 8 alpha stage clamps, "
                             "16 final clamp, 32 colour clamps in compare-mode keys too; 63 = everything, as r76-r90)", v);
        return v;
    }();
    return p;
}
namespace {
// Value ranges in real numbers (the text's float operations are taken as exact: see the header for what that leaves).
struct TrimRng { double lo, hi; };
inline TrimRng TrimHull(const TrimRng& a, const TrimRng& b) { return {std::min(a.lo, b.lo), std::max(a.hi, b.hi)}; }
// The text's bounds of an unclamped stage, as the float literals -4.0157 / 4.0118 are.
const double kTrimWideLo = static_cast<double>(-4.0157f), kTrimWideHi = static_cast<double>(4.0118f);
// One stage's colour or alpha: d (+/-) mix(a, b, c) (+ bias) (* scale), or a compare mode's d + (test ? c : 0); then its clamp. `out` gets
// which side of that clamp can act; the range returned is the clamped one.
TrimRng TrimStage(const TrimRng& a, const TrimRng& b, const TrimRng& c, const TrimRng& d, uint32_t reg, FsTrimClamp& out) {
    const uint32_t bias = bits(reg, 16, 2), minus = bits(reg, 18, 1), clampOn = bits(reg, 19, 1), scale = bits(reg, 20, 2);
    TrimRng r;
    if (bias == 3) {
        const TrimRng m = TrimHull(c, TrimRng{0.0, 0.0});
        r = TrimRng{d.lo + m.lo, d.hi + m.hi};
    } else {
        TrimRng m;
        if (c.lo >= 0.0 && c.hi <= 1.0) {
            m = TrimHull(a, b); // (a convex combination)
        } else {
            // (a (1 - c) + b c is bilinear in (a, b) and c: its extremes are at the corners of the box)
            m = TrimRng{1e300, -1e300};
            for (int i = 0; i < 8; ++i) {
                const double av = (i & 1) ? a.hi : a.lo, bv = (i & 2) ? b.hi : b.lo, cv = (i & 4) ? c.hi : c.lo;
                const double v = av * (1.0 - cv) + bv * cv;
                m.lo = std::min(m.lo, v);
                m.hi = std::max(m.hi, v);
            }
        }
        r = minus ? TrimRng{d.lo - m.hi, d.hi - m.lo} : TrimRng{d.lo + m.lo, d.hi + m.hi};
        if (bias) { const double b5 = bias == 1 ? 0.5 : -0.5; r.lo += b5; r.hi += b5; }
        static const double kScale[4] = {1.0, 2.0, 4.0, 0.5};
        r.lo *= kScale[scale];
        r.hi *= kScale[scale];
    }
    const double lo = clampOn ? 0.0 : kTrimWideLo, hi = clampOn ? 1.0 : kTrimWideHi;
    out.lo = r.lo < lo;
    out.hi = r.hi > hi;
    r.lo = std::min(std::max(r.lo, lo), hi);
    r.hi = std::min(std::max(r.hi, lo), hi);
    return r;
}
} // namespace

void FsTrimAnalyse(const FsKey& k, FsTrimInfo& out) {
    out = FsTrimInfo{};
    const TrimRng unit{0.0, 1.0}, one{1.0, 1.0}, zero{0.0, 0.0}, half{0.5, 0.5};
    // TEV registers: s11 / 255 (vgx_draw.cpp BuildTevC); 0..255 unless the front end marked the key (inWrap: GXV_FIX_TEVIN's wrap, or bit 1).
    const TrimRng reg = k.inWrap ? TrimRng{-1024.0 / 255.0, 1023.0 / 255.0} : unit;
    TrimRng rc[4] = {reg, reg, reg, reg}, ra[4] = {reg, reg, reg, reg};
    // (regFromVtx: registers 1 / 2 start from the vertex colours, 8-bit values: FoldRegs writes them only when they are 0..255)
    if (k.regFromVtx & 1) rc[1] = ra[1] = unit;
    if (k.regFromVtx & 2) rc[2] = ra[2] = unit;
    const bool wrapIn = (k.inWrap & 1) != 0; // (GXV_FIX_TEVIN: vgx_w3 / vgx_w1 give 0..1 for register reads into A, B, C)
    TrimRng tex = one, kc = one, ka = one;
    const int n = k.numStages < 1 ? 1 : (k.numStages > 16 ? 16 : k.numStages);
    for (int s = 0; s < n; ++s) {
        const FsKey::Stage& st = k.stage[s];
        if (st.texEnable) tex = unit;
        else if (st.texOff) tex = one;
        const TrimRng ras = st.rasChan < 2 ? unit : zero;
        const uint32_t cc = st.color, ca = st.alpha;
        bool kcUsed = false, kaUsed = false;
        for (int i = 0; i < 4; ++i) {
            if (bits(cc, i * 4, 4) == 14) kcUsed = true;
            if (bits(ca, 4 + i * 3, 3) == 6) kaUsed = true;
        }
        if (kcUsed) kc = st.kcsel < 8 ? TrimRng{(8 - st.kcsel) / 8.0, (8 - st.kcsel) / 8.0} : (st.kcsel < 12 ? zero : unit);
        if (kaUsed) ka = st.kasel < 8 ? TrimRng{(8 - st.kasel) / 8.0, (8 - st.kasel) / 8.0} : (st.kasel < 16 ? zero : unit);
        auto colourArg = [&](uint32_t a, bool abc) -> TrimRng {
            a &= 15;
            if (a < 8) return (wrapIn && abc) ? unit : ((a & 1) ? ra[a / 2] : rc[a / 2]);
            switch (a) {
            case 8: case 9: return tex;
            case 10: case 11: return ras;
            case 12: return one;
            case 13: return half;
            case 14: return kc;
            default: return zero;
            }
        };
        auto alphaArg = [&](uint32_t a, bool abc) -> TrimRng {
            a &= 7;
            if (a < 4) return (wrapIn && abc) ? unit : ra[a];
            return a == 4 ? tex : (a == 5 ? ras : (a == 6 ? ka : zero));
        };
        const TrimRng nc = TrimStage(colourArg(bits(cc, 12, 4), true), colourArg(bits(cc, 8, 4), true), colourArg(bits(cc, 4, 4), true),
                                     colourArg(bits(cc, 0, 4), false), cc, out.c[s]);
        const TrimRng na = TrimStage(alphaArg(bits(ca, 13, 3), true), alphaArg(bits(ca, 10, 3), true), alphaArg(bits(ca, 7, 3), true),
                                     alphaArg(bits(ca, 4, 3), false), ca, out.a[s]);
        // (TEV reads both sources before either destination is written: the text's `rc` / `ra` go to the registers after both)
        rc[bits(cc, 22, 2)] = nc;
        ra[bits(ca, 22, 2)] = na;
    }
    const FsKey::Stage& last = k.stage[n - 1];
    const TrimRng fc = rc[bits(last.color, 22, 2)], fa = ra[bits(last.alpha, 22, 2)];
    out.finC.lo = fc.lo < 0.0; out.finC.hi = fc.hi > 1.0;
    out.finA.lo = fa.lo < 0.0; out.finA.hi = fa.hi > 1.0;
}

namespace {

// (trim: the clamps the GXV_FS_TRIM text keeps; a side it leaves out does not bound the half error either)
FsHalfReport AnalyseHalfAs(const FsKey& k, bool trim) {
    FsHalfReport rep;
    auto No = [&](const char* why) { rep.why = why; return rep; };
    if (k.numIndStages) return No("indirect stage");
    if (k.fog) return No("fog");
    if (k.inWrap) return No("TEV register outside 0..255");
    if (k.logicSrc) return No("logic-op output");
    FsTrimInfo ti;
    if (trim) FsTrimAnalyse(k, ti);
    bool wrapColor = false, wrapAlpha = false;
    FinalWrapPlanTrim(k, trim ? &ti : nullptr, wrapColor, wrapAlpha);
    if (wrapColor || wrapAlpha) return No("final value wrap");
    for (int s = 0; s < k.numStages; ++s) {
        if (k.stage[s].ind) return No("indirect stage");
        if (bits(k.stage[s].color, 16, 2) == 3 || bits(k.stage[s].alpha, 16, 2) == 3) return No("compare-mode stage");
    }
    const Ab in01 = AbIn(0.0, 1.0);
    const Ab one = AbExact(1.0), zero = AbExact(0.0);
    HalfRegs reg[4];
    reg[0] = {in01, in01};
    reg[1] = {in01, in01};
    reg[2] = {in01, in01};
    reg[3] = {in01, in01};
    HalfRegs tex{one, one}, ras{zero, zero};
    Ab kc = one, ka = one;
    for (int s = 0; s < k.numStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        if (st.texEnable) tex = {in01, in01};
        else if (st.texOff) tex = {one, one};
        if (st.rasChan < 2) ras = {in01, in01};
        else ras = {zero, zero};
        const uint32_t tc = st.color, ta = st.alpha;
        bool kcUsed = false, kaUsed = false;
        for (int i = 0; i < 4; ++i) {
            if (bits(tc, i * 4, 4) == 14) kcUsed = true;
            if (bits(ta, 4 + i * 3, 3) == 6) kaUsed = true;
        }
        if (kcUsed) {
            const uint8_t sel = st.kcsel;
            kc = sel < 8 ? AbExact((8 - sel) / 8.0) : sel < 12 ? zero : in01;
        }
        if (kaUsed) {
            const uint8_t sel = st.kasel;
            ka = sel < 8 ? AbExact((8 - sel) / 8.0) : sel < 16 ? zero : in01;
        }
        auto ColorArg = [&](uint32_t a) -> Ab {
            switch (a & 15) {
            case 0: return reg[0].rgb;  case 1: return reg[0].a;
            case 2: return reg[1].rgb;  case 3: return reg[1].a;
            case 4: return reg[2].rgb;  case 5: return reg[2].a;
            case 6: return reg[3].rgb;  case 7: return reg[3].a;
            case 8: return tex.rgb;     case 9: return tex.a;
            case 10: return ras.rgb;    case 11: return ras.a;
            case 12: return one;        case 13: return AbExact(0.5);
            case 14: return kc;         default: return zero;
            }
        };
        auto AlphaArg = [&](uint32_t a) -> Ab {
            switch (a & 7) {
            case 0: return reg[0].a; case 1: return reg[1].a; case 2: return reg[2].a; case 3: return reg[3].a;
            case 4: return tex.a;    case 5: return ras.a;    case 6: return ka;        default: return zero;
            }
        };
        auto Combine = [&](uint32_t reg32, const Ab& A, const Ab& B, const Ab& C, const Ab& D, const FsTrimClamp& act) {
            const uint32_t bias = bits(reg32, 16, 2), op = bits(reg32, 18, 1), clampOn = bits(reg32, 19, 1), scale = bits(reg32, 20, 2);
            Ab r = AbMix(A, B, C);
            r = op ? AbSub(D, r) : AbAdd(D, r);
            if (bias == 1) r = AbAdd(r, AbExact(0.5));
            else if (bias == 2) r = AbSub(r, AbExact(0.5));
            static const double kScale[4] = {1.0, 2.0, 4.0, 0.5};
            r = AbScale(r, kScale[scale]);
            // (GXV_FS_TRIM: a side the trimmed text leaves out does not bound the value; with neither side, no clamp at all)
            if (!act.lo && !act.hi) return r;
            const double lo = clampOn ? 0.0 : -4.0157, hi = clampOn ? 1.0 : 4.0118;
            r = AbClamp(r, act.lo ? lo : -1e300, act.hi ? hi : 1e300);
            // (the half text clamps to the half values of -4.0157 / 4.0118: 4.015625 / 4.01171875, a difference of 1e-4 at most)
            if (!clampOn) r.e += 1e-4;
            return r;
        };
        const FsTrimClamp both{};
        const Ab cr = Combine(tc, ColorArg(bits(tc, 12, 4)), ColorArg(bits(tc, 8, 4)), ColorArg(bits(tc, 4, 4)), ColorArg(bits(tc, 0, 4)),
                              trim && s < 16 ? ti.c[s] : both);
        const Ab ar = Combine(ta, AlphaArg(bits(ta, 13, 3)), AlphaArg(bits(ta, 10, 3)), AlphaArg(bits(ta, 7, 3)), AlphaArg(bits(ta, 4, 3)),
                              trim && s < 16 ? ti.a[s] : both);
        reg[bits(tc, 22, 2)].rgb = cr;
        reg[bits(ta, 22, 2)].a = ar;
    }
    const FsKey::Stage& last = k.stage[k.numStages - 1];
    // (the final clamp: GXV_FS_TRIM keeps the sides that can act for colour or alpha, as one vec4 operation)
    const bool finLo = !trim || ti.finC.lo || ti.finA.lo, finHi = !trim || ti.finC.hi || ti.finA.hi;
    const Ab fr = AbClamp(reg[bits(last.color, 22, 2)].rgb, finLo ? 0.0 : -1e300, finHi ? 1.0 : 1e300);
    const Ab fa = AbClamp(reg[bits(last.alpha, 22, 2)].a, finLo ? 0.0 : -1e300, finHi ? 1.0 : 1e300);
    rep.eligibleShape = true;
    rep.colorLevels = fr.e * 255.0;
    rep.alphaLevels = fa.e * 255.0;
    rep.alphaTested = k.alphaTestOn && !(NoDiscard() && AlphaTestAlwaysPasses(k.alphaTest & 7, -1, (k.alphaTest >> 6) & 3, (k.alphaTest >> 3) & 7, -1));
    if (k.dstAlpha) rep.alphaLevels = AbIn(0.0, 1.0).e * 255.0; // a constant 8-bit alpha, rounded to half once
    return rep;
}

} // namespace

long FsHalfMode() {
    static const long m = [] {
        const long v = EnvInt("GXV_FS_HALF", 0); // 0 off, 1 eligible shaders, 2 every shader (vitaGL patch 0012), 3 eligible, strict bound
        return v >= 1 && v <= 3 ? v : 0L;
    }();
    return m;
}
// (GXV_FS_TRIM does not take part in the precision decision: see GenerateFragmentShader)
FsHalfReport FsHalfAnalyse(const FsKey& key) { return AnalyseHalfAs(key, false); }
bool FsHalfReport::Eligible(long mode) const {
    if (!eligibleShape) return false;
    const double limit = mode == 3 ? kHalfMaxLevelsStrict : kHalfMaxLevels;
    return colorLevels <= limit && alphaLevels <= (alphaTested ? kHalfMaxLevelsAlphaTest : limit);
}
bool FsHalfEligible(const FsKey& key) {
    const long m = FsHalfMode();
    return (m == 1 || m == 3) && AnalyseHalfAs(key, false).Eligible(m);
}

namespace {
void GenerateFragmentShaderAs(const FsKey& k, ShaderSource& src, bool hf, unsigned parts);
inline unsigned TrimParts(bool trim) { return trim ? static_cast<unsigned>(FsTrimParts()) : 0u; }
}
// GXV_FS_TRIM and half precision (r88 console, Beach Bowl: ground drop shadows and grass details as speckled "static" with GXV_FS_TRIM=1,
// GXV_FS_HALF=1): the trim applies to float shaders only, and never changes a key's precision. A key FsHalfEligible takes (the analysis
// of the FULL text, as with the trim off) is written as before, clamps and floor included, byte for byte; every other key is float and
// trimmed. Before, the trim also moved keys INTO half (a final alpha wrap proven idle no longer refused it, and the trimmed clamps were
// what the bound took), and half keys lost their clamps. Its bound does not cover what an alpha test sees in a key that writes the
// destination-alpha constant (FsHalfReport::alphaLevels is the constant's there, the tested TEV alpha unchecked: the opaque scene,
// GXSetDstAlpha(TRUE, 0), alpha-tested ground decals), so such a key went half with a tested alpha the analysis bounds only by tens to
// hundreds of 8-bit levels: per-pixel flips of the alpha test, its colour, depth and alpha-0 writes (the shadow volumes then read
// that depth / alpha). The float trimmed text is exact in real numbers (FsTrimAnalyse) and a float rounding from the full text.
// r90 console: the speckle REMAINED with this (also with GXV_FS_HALF=0): the float trimmed text misbehaves on the console as well, by a
// mechanism the host models (Mesa, IEEE float) do not show. Hence GXV_FS_TRIM_PARTS (vgx_shadergen.h) to bisect, defaulting to colour clamps only.
void GenerateFragmentShader(const FsKey& k, ShaderSource& src) {
    const bool hf = FsHalfEligible(k);
    GenerateFragmentShaderAs(k, src, hf, TrimParts(FsTrimMode() != 0 && !hf));
}
void GenerateFragmentShaderPrecision(const FsKey& k, ShaderSource& src, bool half) {
    const bool hf = half && AnalyseHalfAs(k, false).eligibleShape;
    GenerateFragmentShaderAs(k, src, hf, TrimParts(FsTrimMode() != 0 && !hf));
}
void GenerateFragmentShaderForm(const FsKey& k, ShaderSource& src, bool half, bool trim) {
    const bool hf = half && AnalyseHalfAs(k, false).eligibleShape;
    GenerateFragmentShaderAs(k, src, hf, TrimParts(trim && !hf));
}
void GenerateFragmentShaderParts(const FsKey& k, ShaderSource& src, bool half, long parts) {
    const bool hf = half && AnalyseHalfAs(k, false).eligibleShape;
    GenerateFragmentShaderAs(k, src, hf, hf ? 0u : static_cast<unsigned>(parts) & static_cast<unsigned>(kFsTrimAll));
}

namespace {
// parts (GXV_FS_TRIM, kFsTrim* bits): which of the trims FsTrimAnalyse proves inert are made. 0: the text as always.
void GenerateFragmentShaderAs(const FsKey& k, ShaderSource& src, bool hf, unsigned parts) {
    Out o;
    FsTrimInfo ti;
    // (a part left out keeps its clamp / floor / wrap: FsTrimInfo's sides set to "can act"; the analysis's ranges are those of the clamped
    // values, so keeping a clamp never widens what the other parts rely on)
    bool hasCompare = false;
    for (int s = 0; s < k.numStages && s < 16; ++s)
        if (bits(k.stage[s].color, 16, 2) == 3 || bits(k.stage[s].alpha, 16, 2) == 3) hasCompare = true;
    // (colour clamps: not in a key with a compare-mode stage, whose R8 / GR16 / BGR24 compares read colour inputs and scale a difference by
    // up to 2^24: a rounding at 0 or 1 that a clamp would have removed decides them)
    const bool trimCol = (parts & kFsTrimColourClamps) && (!hasCompare || (parts & kFsTrimCompareKeys)), trimAlpha = (parts & kFsTrimAlphaClamps) != 0;
    const bool trimFloor = (parts & kFsTrimFloor) != 0, trimWrap = (parts & kFsTrimFinalWrap) != 0, trimFinal = (parts & kFsTrimFinalClamp) != 0;
    const bool trim = trimCol || trimAlpha || trimFloor || trimWrap || trimFinal;
    if (trim) {
        FsTrimAnalyse(k, ti);
        for (int s = 0; s < 16; ++s) {
            if (!trimCol) ti.c[s] = FsTrimClamp{};
            if (!trimAlpha) ti.a[s] = FsTrimClamp{};
        }
    }
    unsigned trimOps = 0; // (census: ShaderSource::trimOps)
    // GXV_TEVWRAP / GXV_TEVWRAP_ELIDE: see FinalWrapPlan.
    // GXV_FIX_INDPREV (default 0 until seen on the console: it changes the text of add-previous keys, whose compiled shaders are then not in the shipped cache; 1 = the fix): an indirect stage that adds the previous stage's coordinate
    // (BP 0x10+n bit 20, GX_ITB addprev) adds it in texels, as GX does. The old text added the previous stage's
    // already-normalised coordinate to texel units, so the carried-over part was ~1/texture-size of what it should be.
    static const bool indPrevOn = EnvInt("GXV_FIX_INDPREV", 0) != 0;
    // (only a shader with an add-previous stage changes text; every other indirect shader keeps its source and compiled cache)
    bool indPrev = false;
    for (int s = 0; indPrevOn && s < k.numStages; ++s)
        if (k.stage[s].ind && bits(k.stage[s].ind, 20, 1)) indPrev = true;
    // hf (GXV_FS_HALF=1 / 3, a key the analysis accepts): the shader is written with half precision colour variables (see FsHalfAnalyse).
    // Every other key, and every key with the switch off or 2, is written exactly as before (hf false: the strings below are the old ones).
    const char* const T1 = hf ? "hfloat" : "float";
    const char* const T3 = hf ? "hvec3" : "vec3";
    const char* const T4 = hf ? "hvec4" : "vec4";
    // a float constant in a half expression: written as a half constant, so that no operation is promoted to float for its literal
    auto L = [&](const char* lit) -> std::string { return hf ? std::string("hfloat(") + lit + ")" : std::string(lit); };
    const FsKey::Stage& last = k.stage[k.numStages - 1];
    bool wrapColor = false, wrapAlpha = false;
    FinalWrapPlanTrim(k, trimWrap ? &ti : nullptr, wrapColor, wrapAlpha);
    if (trim) {
        bool fullColor = false, fullAlpha = false;
        FinalWrapPlan(k, fullColor, fullAlpha);
        trimOps += static_cast<unsigned>(fullColor && !wrapColor) * 3u + static_cast<unsigned>(fullAlpha && !wrapAlpha); // (wraps left out)
    }
    uint8_t texMask = 0;
    bool usesCol[2] = {false, false};
    for (int s = 0; s < k.numStages; ++s) {
        if (k.stage[s].texEnable) texMask |= static_cast<uint8_t>(1u << k.stage[s].texMap);
        if (k.stage[s].rasChan < 2) usesCol[k.stage[s].rasChan] = true;
    }
    for (int i = 0; i < k.numIndStages; ++i) texMask |= static_cast<uint8_t>(1u << k.ind[i].texMap);

    o("#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n");
    if (hf) {
        // The half types: Cg's own half under vitaGL (its GLSL preprocessor, which defines VITAGL, expands these before the translator
        // sees the text, and the translator does not rewrite them), plain float types on a PC.
        o("#ifdef VITAGL\n#define hfloat half\n#define hvec3 half3\n#define hvec4 half4\n#else\n"
          "#define hfloat float\n#define hvec3 vec3\n#define hvec4 vec4\n#endif\n");
    }
    if (wrapColor || wrapAlpha) {
        // Keep in-range fractions unchanged. This repairs final conversion only, not
        // the existing normalized-float approximation of intermediate GX arithmetic.
        o("float vgx_wrap_final(float value) {\n"
          "  if (value >= 0.0 && value <= 1.0) return value;\n"
          "  float byteValue = floor(value * 255.0 + 0.5);\n"
          "  return (byteValue - 256.0 * floor(byteValue / 256.0)) / 255.0;\n}\n");
    }
    if (k.inWrap & 1) {
        // (GXV_FIX_TEVIN) byte wrap of a register value, as GX's `& 255` on the inputs A, B, C; values in range are returned unchanged
        o("float vgx_w1(float value) {\n"
          "  if (value >= 0.0 && value <= 1.0) return value;\n"
          "  float byteValue = floor(value * 255.0 + 0.5);\n"
          "  return (byteValue - 256.0 * floor(byteValue / 256.0)) / 255.0;\n}\n"
          "vec3 vgx_w3(vec3 value) { return vec3(vgx_w1(value.x), vgx_w1(value.y), vgx_w1(value.z)); }\n");
    }
    for (int m = 0; m < 8; ++m)
        if (texMask & (1u << m)) o("uniform sampler2D u_tex%d;\n", m);
    o("uniform %s u_tev[%d];\n", T4, FsTevCount(k));
    if (usesCol[0] || (k.regFromVtx & 1)) o("varying %s v_col0;\n", T4);
    if (usesCol[1] || (k.regFromVtx & 2)) o("varying %s v_col1;\n", T4);
    // (GXV_IND_VS, FsKey::indVs: a texgen only indirect lookups read is a vec2 the vertex shader has scaled; IndVsPair)
    uint8_t indVsTg = 0;
    for (int i = 0; i < k.numIndStages; ++i)
        if (k.indVs(i) && k.ind[i].texCoord < k.numTexGens) indVsTg |= static_cast<uint8_t>(1u << k.ind[i].texCoord);
    // (GXV_TEXVEC2, FsKey::texVec2: a non-projective texgen's varying is a vec2, read as it is)
    auto texV2 = [&](int i) { return (k.texVec2() && !(k.texProjMask & (1u << i))) || ((indVsTg >> i) & 1u); };
    for (int i = 0; i < k.numTexGens; ++i) o(texV2(i) ? "varying vec2 v_tex%d;\n" : "varying vec3 v_tex%d;\n", i);
    if (k.fog & kFogVtxBit) o("varying float v_fog;\n");
    o("void main() {\n");
    o("  %s prev = u_tev[0], c0 = %s, c1 = %s, c2 = u_tev[3];\n", T4, (k.regFromVtx & 1) ? "v_col0" : "u_tev[1]", (k.regFromVtx & 2) ? "v_col1" : "u_tev[2]");
    if (hf) o("  hvec4 tex = hvec4(1.0), ras = hvec4(0.0);\n  hvec3 kc = hvec3(1.0);\n  hfloat ka = hfloat(1.0);\n");
    else o("  vec4 tex = vec4(1.0), ras = vec4(0.0);\n  vec3 kc = vec3(1.0);\n  float ka = 1.0;\n");
    // Texture coordinates (normalised).
    for (int i = 0; i < k.numTexGens; ++i) {
        if (k.texProjMask & (1u << i)) o("  vec2 uv%d = v_tex%d.xy / (v_tex%d.z == 0.0 ? 1.0 : v_tex%d.z);\n", i, i, i, i);
        else if (texV2(i)) o("  vec2 uv%d = v_tex%d;\n", i, i);
        else o("  vec2 uv%d = v_tex%d.xy;\n", i, i);
    }
    auto Uv = [&](int c) -> std::string {
        if (c >= k.numTexGens) return "vec2(0.0)";
        return "uv" + std::to_string(c);
    };
    // Indirect lookups: three 8-bit offsets per indirect stage, taken from A, B, G.
    for (int i = 0; i < k.numIndStages; ++i) {
        if ((indVsTg >> k.ind[i].texCoord) & 1u && k.indVs(i)) // (GXV_IND_VS: the varying as it is, a non-dependent read)
            o("  vec3 ind%d = texture2D(u_tex%d, v_tex%d).abg * 255.0;\n", i, k.ind[i].texMap, k.ind[i].texCoord);
        else
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
        if (hf) {
            // (the sample is a float vec4 from the translator's texture function; the coordinate stays float)
            if (st.texEnable && st.lodBias)
                o("  tex = hvec4(texture2D(u_tex%d, %s, float(u_tev[%d].%c)).%s);\n", st.texMap, coord.c_str(), V_LODBIAS + st.texMap / 4, "xyzw"[st.texMap & 3],
                  Swizzle(st.texSwap).c_str());
            else if (st.texEnable) o("  tex = hvec4(texture2D(u_tex%d, %s).%s);\n", st.texMap, coord.c_str(), Swizzle(st.texSwap).c_str());
            else if (st.texOff) o("  tex = hvec4(1.0);\n");
        } else if (st.texEnable && st.lodBias)
            o("  tex = texture2D(u_tex%d, %s, u_tev[%d].%c).%s;\n", st.texMap, coord.c_str(), V_LODBIAS + st.texMap / 4, "xyzw"[st.texMap & 3],
              Swizzle(st.texSwap).c_str());
        else if (st.texEnable) o("  tex = texture2D(u_tex%d, %s).%s;\n", st.texMap, coord.c_str(), Swizzle(st.texSwap).c_str());
        else if (st.texOff) o("  tex = vec4(1.0);\n");
        if (st.rasChan < 2) o("  ras = v_col%d.%s;\n", st.rasChan, Swizzle(st.rasSwap).c_str());
        else o("  ras = %s(0.0);\n", T4);
        const uint32_t tc = st.color, ta = st.alpha;
        bool kcUsed = false, kaUsed = false;
        for (int i = 0; i < 4; ++i) {
            if (bits(tc, i * 4, 4) == 14) kcUsed = true;
            if (bits(ta, 4 + i * 3, 3) == 6) kaUsed = true;
        }
        if (kcUsed) o("  kc = %s;\n", KonstColor(st.kcsel, T3).c_str());
        if (kaUsed) o("  ka = %s;\n", KonstAlpha(st.kasel, hf).c_str());
        o("  {\n");
        const bool w = (k.inWrap & 1) != 0;
        o("    %s a = %s, b = %s, c = %s, d = %s;\n", T3, WrapArg(ColorArg(bits(tc, 12, 4), T3), w && bits(tc, 12, 4) < 8, true).c_str(),
          WrapArg(ColorArg(bits(tc, 8, 4), T3), w && bits(tc, 8, 4) < 8, true).c_str(), WrapArg(ColorArg(bits(tc, 4, 4), T3), w && bits(tc, 4, 4) < 8, true).c_str(),
          ColorArg(bits(tc, 0, 4), T3).c_str());
        o("    %s aa = %s, ab = %s, ac = %s, ad = %s;\n", T1, WrapArg(AlphaArg(bits(ta, 13, 3)), w && bits(ta, 13, 3) < 4, false).c_str(),
          WrapArg(AlphaArg(bits(ta, 10, 3)), w && bits(ta, 10, 3) < 4, false).c_str(), WrapArg(AlphaArg(bits(ta, 7, 3)), w && bits(ta, 7, 3) < 4, false).c_str(),
          AlphaArg(bits(ta, 4, 3)));
        const std::string kBias[3] = {"", " + " + L("0.5"), " - " + L("0.5")};
        const std::string kScale[4] = {"", " * " + L("2.0"), " * " + L("4.0"), " * " + L("0.5")};
        auto Combine = [&](uint32_t reg, bool color) {
            const uint32_t bias = bits(reg, 16, 2), op = bits(reg, 18, 1), clampOn = bits(reg, 19, 1), scale = bits(reg, 20, 2);
            const char* res = color ? "rc" : "ra";
            const char* type = color ? T3 : T1;
            const char* A = color ? "a" : "aa"; const char* B = color ? "b" : "ab";
            const char* C = color ? "c" : "ac"; const char* D = color ? "d" : "ad";
            if (bias != 3) {
                o("    %s %s = (%s %s mix(%s, %s, %s)%s)%s;\n", type, res, D, op ? "-" : "+", A, B, C, kBias[bias].c_str(), kScale[scale].c_str());
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
            const FsTrimClamp act = trim ? (color ? ti.c[s] : ti.a[s]) : FsTrimClamp{};
            if (trim && !(act.lo && act.hi)) {
                // (GXV_FS_TRIM: one side that can act costs a max or a min, none costs nothing. The bound is written in the operand's own
                // type, as Cg has no min(float3, float).)
                trimOps += (act.lo ? 0u : 1u) + (act.hi ? 0u : 1u);
                if (act.lo) o("    %s = max(%s, %s(%s));\n", res, res, type, clampOn ? "0.0" : "-4.0157");
                else if (act.hi) o("    %s = min(%s, %s(%s));\n", res, res, type, clampOn ? "1.0" : "4.0118");
            } else if (clampOn) o("    %s = clamp(%s, %s, %s);\n", res, res, L("0.0").c_str(), L("1.0").c_str());
            else o("    %s = clamp(%s, %s, %s);\n", res, res, L("-4.0157").c_str(), L("4.0118").c_str());
        };
        Combine(tc, true);
        Combine(ta, false);
        o("    %s.rgb = rc;\n    %s.a = ra;\n  }\n", RegName(bits(tc, 22, 2)), RegName(bits(ta, 22, 2)));
    }
    // GXV_FS_TRIM: the final clamp keeps the sides that can act for colour or alpha (one vec4 operation). A wrapped part is 0..1 already:
    // vgx_wrap_final returns a value in 0..1 as it is and makes every other one a byte / 255.
    const bool finLo = !trimFinal || (!wrapColor && ti.finC.lo) || (!wrapAlpha && ti.finA.lo);
    const bool finHi = !trimFinal || (!wrapColor && ti.finC.hi) || (!wrapAlpha && ti.finA.hi);
    if (trim) trimOps += (finLo ? 0u : 1u) + (finHi ? 0u : 1u);
    if (wrapColor || wrapAlpha) {
        o("  vec4 fin = vec4(%s.rgb, %s.a);\n", RegName(bits(last.color, 22, 2)), RegName(bits(last.alpha, 22, 2)));
        if (wrapColor) o("  fin.rgb = vec3(vgx_wrap_final(fin.r), vgx_wrap_final(fin.g), vgx_wrap_final(fin.b));\n");
        if (wrapAlpha) o("  fin.a = vgx_wrap_final(fin.a);\n");
        if (finLo && finHi) o("  fin = clamp(fin, 0.0, 1.0);\n");
        else if (finLo) o("  fin = max(fin, vec4(0.0));\n");
        else if (finHi) o("  fin = min(fin, vec4(1.0));\n");
    } else if (finLo && finHi) {
        o("  %s fin = clamp(%s(%s.rgb, %s.a), %s, %s);\n", T4, T4, RegName(bits(last.color, 22, 2)), RegName(bits(last.alpha, 22, 2)), L("0.0").c_str(), L("1.0").c_str());
    } else if (finLo || finHi) {
        o("  %s fin = %s(%s(%s.rgb, %s.a), %s(%s));\n", T4, finLo ? "max" : "min", T4, RegName(bits(last.color, 22, 2)), RegName(bits(last.alpha, 22, 2)), T4,
          finLo ? "0.0" : "1.0");
    } else {
        o("  %s fin = %s(%s.rgb, %s.a);\n", T4, T4, RegName(bits(last.color, 22, 2)), RegName(bits(last.alpha, 22, 2)));
    }
    // GXV_NODISCARD: a test that cannot fail leaves no `discard` in the shader. (On the console's GPU any discard in a shader
    // turns off hidden-surface removal for the draws that use it; the front end already drops the combinations of ALWAYS and
    // NEVER, and with the switch on also the ones its references make constant: BuildFsKey. A key written by an older run
    // can still carry them.)
    // GXV_DISCARD_BLEND (FsKey::rejectZero): the test's result is kept in vgx_ok and a failing fragment outputs vec4(0) at the end (after
    // fog and every other change of fin); the front end sets the bit only where that output leaves the target as it is.
    bool rejectZero = false;
    if (k.alphaTestOn && !(NoDiscard() && AlphaTestAlwaysPasses(k.alphaTest & 7, -1, (k.alphaTest >> 6) & 3, (k.alphaTest >> 3) & 7, -1))) {
        rejectZero = k.rejectZero() != 0;
        char b0[96], b1[96], r0[32], r1[32];
        std::snprintf(r0, sizeof r0, hf ? "float(u_tev[%d].x)" : "u_tev[%d].x", V_MISC);
        std::snprintf(r1, sizeof r1, hf ? "float(u_tev[%d].y)" : "u_tev[%d].y", V_MISC);
        const char* e0 = CompareExpr(k.alphaTest & 7, r0, b0, sizeof b0);
        const char* e1 = CompareExpr((k.alphaTest >> 3) & 7, r1, b1, sizeof b1);
        static const char* const kOp[4] = {"&&", "||", "!=", "=="};
        if (trimFloor) {
            // (GXV_FS_TRIM: with ax = a * 255 + 0.5 and ai = floor(ax), and the reference r a whole number: ai < r is ax < r, ai <= r is
            // ax < r + 1, ai > r is ax >= r + 1, ai >= r is ax >= r. Exact: floor is exact and r, r + 1 are exact floats. Only EQUAL and
            // NEQUAL keep the floor.)
            auto ordered = [](uint32_t c, const char* ref, char* buf, size_t n, const char* have) -> const char* {
                switch (c) {
                case 1: std::snprintf(buf, n, "(ax < %s)", ref); break;
                case 3: std::snprintf(buf, n, "(ax < %s + 1.0)", ref); break;
                case 4: std::snprintf(buf, n, "(ax >= %s + 1.0)", ref); break;
                case 6: std::snprintf(buf, n, "(ax >= %s)", ref); break;
                default: return have;
                }
                return buf;
            };
            const uint32_t c0 = k.alphaTest & 7, c1 = (k.alphaTest >> 3) & 7;
            e0 = ordered(c0, r0, b0, sizeof b0, e0);
            e1 = ordered(c1, r1, b1, sizeof b1, e1);
            o(hf ? "  float ax = float(fin.a) * 255.0 + 0.5;\n" : "  float ax = fin.a * 255.0 + 0.5;\n");
            if (c0 == 2 || c0 == 5 || c1 == 2 || c1 == 5) o("  float ai = floor(ax);\n");
            else ++trimOps;
        } else {
            o(hf ? "  float ai = floor(float(fin.a) * 255.0 + 0.5);\n" : "  float ai = floor(fin.a * 255.0 + 0.5);\n");
        }
        if (rejectZero) o("  bool vgx_ok = %s %s %s;\n", e0, kOp[(k.alphaTest >> 6) & 3], e1);
        else o("  if (!(%s %s %s)) discard;\n", e0, kOp[(k.alphaTest >> 6) & 3], e1);
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
    if (rejectZero) o("  gl_FragColor = vgx_ok ? vec4(fin) : vec4(0.0);\n}\n");
    else o("  gl_FragColor = fin;\n}\n");
    src.text = std::move(o.s);
    src.texMask = texMask;
    src.texChan = FsTexChannels(k);
    src.numTexGens = k.numTexGens;
    src.nTev = static_cast<uint8_t>(FsTevCount(k));
    src.trimOps = static_cast<uint8_t>(trimOps > 255 ? 255 : trimOps);
}
} // namespace

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

long VsOutMode() {
    static const long m = EnvInt("GXV_VSOUT", 0) == 1 ? 1 : 0;
    return m;
}

// GXV_VSOUT: what GenerateFragmentShaderAs reads of the vertex shader's varyings. Every selection counts, whether the stage's
// operation uses the argument or not (a compare-mode stage reads only part of its A and B; counted whole). A stage with
// rasChan 0 / 1 assigns `ras = v_colN.<rasSwap>`: a colour argument RASC (10) reads ras.rgb, RASA (11) and the alpha argument
// RASA (5) ras.a, which are the v_colN components the swap table names. regFromVtx reads v_col0 / v_col1 whole (c0 / c1).
// A texture coordinate is read by a stage that samples (texEnable) or computes an indirect coordinate (ind), and by an
// indirect lookup; `uvN` of the other texgens is computed but never used (no static use of the varying beyond that line).
void VsOutPrune(VsKey& vs, const FsKey& fs, bool keepColours) {
    uint8_t comp[2] = {0, 0}; // bit c: component c (x y z w) of v_colN read
    bool declared[2] = {false, false};
    uint32_t texRead = 0;
    for (int s = 0; s < fs.numStages && s < 16; ++s) {
        const FsKey::Stage& st = fs.stage[s];
        if (st.rasChan < 2) {
            declared[st.rasChan] = true;
            bool rgb = false, a = false;
            for (int i = 0; i < 4; ++i) {
                const uint32_t c = bits(st.color, i * 4, 4);
                if (c == 10) rgb = true;
                if (c == 11) a = true;
                if (bits(st.alpha, 4 + i * 3, 3) == 5) a = true;
            }
            const uint8_t sw = st.rasSwap;
            if (rgb) comp[st.rasChan] |= static_cast<uint8_t>((1u << (sw & 3)) | (1u << ((sw >> 2) & 3)) | (1u << ((sw >> 4) & 3)));
            if (a) comp[st.rasChan] |= static_cast<uint8_t>(1u << ((sw >> 6) & 3));
        }
        if (st.texEnable || st.ind) texRead |= 1u << (st.texCoord & 7);
    }
    for (int i = 0; i < fs.numIndStages && i < 4; ++i) texRead |= 1u << (fs.ind[i].texCoord & 7);
    for (int ch = 0; ch < 2; ++ch)
        if ((fs.regFromVtx & (1u << ch)) || keepColours) { declared[ch] = true; comp[ch] = 0xF; }
    uint8_t code[2] = {0, 0};
    for (int ch = 0; ch < 2; ++ch) {
        if (!declared[ch]) code[ch] = VsKey::kOutRgb | VsKey::kOutA | VsKey::kOutGone;
        else code[ch] = static_cast<uint8_t>(((comp[ch] & 7) ? 0 : VsKey::kOutRgb) | ((comp[ch] & 8) ? 0 : VsKey::kOutA));
    }
    const uint32_t all = vs.numTexGens >= 8 ? 0xFFu : (1u << vs.numTexGens) - 1u;
    vs.setOut(code[0], code[1], static_cast<uint8_t>(all & ~texRead));
}

long VsTrimMode() {
    static const long m = EnvInt("GXV_VS_TRIM", 0) == 1 ? 1 : 0;
    return m;
}

long TexVec2Mode() {
    static const long m = EnvInt("GXV_TEXVEC2", 0) == 1 ? 1 : 0;
    return m;
}
bool TexVec2Pair(FsKey& fs, VsKey& vs) {
    if (!TexVec2Mode()) return false;
    const uint32_t all = fs.numTexGens >= 8 ? 0xFFu : (1u << fs.numTexGens) - 1u;
    const uint8_t v2 = static_cast<uint8_t>(all & ~static_cast<uint32_t>(fs.texProjMask));
    if (!v2) return false; // (no texgen, or every one projective: both keys as they were)
    fs.setTexVec2(1);
    vs.setTexVec2Mask(v2);
    return true;
}

long IndVsMode() {
    static const long m = EnvInt("GXV_IND_VS", 0) == 1 ? 1 : 0;
    return m;
}
bool IndVsPair(FsKey& fs, VsKey& vs, uint32_t bp25, uint32_t bp26) {
    if (!IndVsMode() || !fs.numIndStages) return false;
    // what reads each texgen besides the indirect lookups: a stage that samples or computes an indirect coordinate from it
    uint32_t stageRead = 0;
    for (int s = 0; s < fs.numStages && s < 16; ++s)
        if (fs.stage[s].texEnable || fs.stage[s].ind) stageRead |= 1u << (fs.stage[s].texCoord & 7);
    const int nInd = fs.numIndStages > 4 ? 4 : fs.numIndStages;
    uint8_t scale[4][2];
    for (int i = 0; i < nInd; ++i) {
        const uint32_t r = (i < 2 ? bp25 : bp26) >> ((i & 1) * 8); // (as BuildTevC reads it: BP 0x25 + i / 2, 8 bits per stage)
        scale[i][0] = static_cast<uint8_t>(bits(r, 0, 4));
        scale[i][1] = static_cast<uint8_t>(bits(r, 4, 4));
    }
    bool any = false;
    for (int c = 0; c < fs.numTexGens && c < 6 && c < vs.numTexGens; ++c) {
        if ((fs.texProjMask >> c) & 1u) continue;     // (projective: the divide stays per pixel)
        if ((stageRead >> c) & 1u) continue;          // (another reader needs the unscaled coordinate)
        int first = -1;
        bool same = true;
        for (int i = 0; i < nInd; ++i) {
            if (fs.ind[i].texCoord != c) continue;
            if (first < 0) first = i;
            else if (scale[i][0] != scale[first][0] || scale[i][1] != scale[first][1]) same = false;
        }
        if (first < 0 || !same) continue;
        for (int i = 0; i < nInd; ++i)
            if (fs.ind[i].texCoord == c) fs.ind[i].pad[0] = 1;
        vs.setIndScale(c, scale[first][0], scale[first][1]);
        any = true;
    }
    return any;
}

namespace {
// GXV_VS_TRIM (VsKey::ltCos1 / ltDist1 / ltTrim): one light's attenuation term `at` of a channel part. With cos attenuation (1, 0, 0) the
// spot polynomial max(0, 1 + 0 ca + 0 ca^2) is exactly 1 for a finite ca (GXV_LIGHTNORM, the default, makes ca 0 for such a light); with
// distance attenuation (1, 0, 0) the divisor 1 + 0 d1 + 0 d2 is exactly 1 for finite d1, d2. So the terms are left out, and what remains
// is the same arithmetic in the same order. Returns the number of terms left out (census).
unsigned EmitLightAttn(Out& o, const VsKey& k, const VsKey::Chan& c, int l, int b) {
    const bool one = (k.ltCos1() >> l) & 1u, dOne = (k.ltDist1() >> l) & 1u;
    if (c.attn == 2 && one && dOne) return 2; // (at stays 1.0)
    if (c.attn == 2 && one) {
        o("      at = 1.0 / dot(u_lt[%d].xyz, vec3(1.0, d1, d2));\n", b + 4);
        return 1;
    }
    if (c.attn == 2 && dOne) {
        o("      float ca = max(0.0, dot(ld, u_lt[%d].xyz));\n", b + 1);
        o("      at = max(0.0, u_lt[%d].x + u_lt[%d].y * ca + u_lt[%d].z * ca * ca);\n", b + 3, b + 3, b + 3);
        return 1;
    }
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
    return 0;
}
// GXV_VS_TRIM: every light term of the part is >= 0, so the sum (ambient and light colours are 8-bit values, >= 0) cannot be negative
// and clamp(x, 0, 1) is min(x, 1): a term is >= 0 with no attenuation or a spot attenuation of divisor 1 (max(0, ...)), and a
// diffuse factor of none or max(0, n.l); a signed diffuse (1), a specular attenuation or a spot divisor of any other value can be < 0.
bool LightSumNonNegative(const VsKey& k, const VsKey::Chan& c) {
    if (!k.ltTrim()) return false;
    for (int l = 0; l < 8; ++l) {
        if (!((c.lightMask >> l) & 1u)) continue;
        if (c.diffuse == 1) return false;
        if (!(c.attn == 0 || (c.attn == 2 && ((k.ltDist1() >> l) & 1u)))) return false;
    }
    return true;
}
} // namespace

void GenerateVertexShader(const VsKey& k, ShaderSource& src) {
    Out o;
    unsigned trimOps = 0; // (GXV_VS_TRIM census: ShaderSource::trimOps)
    bool needNrm = false, needLights = false, needCol[2] = {false, false}, needPal = k.perVtxPos != 0;
    bool needTm = false, needPtm = false, needTmi = false;
    uint8_t texSrcMask = 0;
    int maxLight = -1;
    // GXV_VSOUT (VsKey::setOut; all zero: everything is computed and written, the text as it always was). partOn[ch][0] / [1]: the
    // colour / alpha part of channel ch is computed; outGone[ch]: v_colN is not declared; tgOn(i): texgen i is computed.
    const uint8_t texOff = static_cast<uint8_t>(k.outTexOff() & (k.numTexGens >= 8 ? 0xFFu : (1u << k.numTexGens) - 1u));
    auto tgOn = [&](int i) { return !(texOff & (1u << i)); };
    bool partOn[2][2], outGone[2];
    for (int ch = 0; ch < 2; ++ch) {
        partOn[ch][0] = !(k.outCol(ch) & VsKey::kOutRgb);
        partOn[ch][1] = !(k.outCol(ch) & VsKey::kOutA);
        outGone[ch] = (k.outCol(ch) & VsKey::kOutGone) != 0;
    }
    for (int i = 0; i < k.numTexGens; ++i)
        if (tgOn(i) && k.tg[i].type >= 2) partOn[(k.tg[i].type - 2) & 1][0] = true; // (a colour texgen reads colN.rg)
    for (int i = 0; i < 4; ++i) {
        const VsKey::Chan& c = k.chan[i];
        if ((i & 1) >= k.numChans) continue;
        // (the declared light count stays that of every lit part: the light block and u_lt keep their size)
        if (c.enable && c.lightMask)
            for (int l = 0; l < 8; ++l)
                if (c.lightMask & (1u << l)) maxLight = l > maxLight ? l : maxLight;
        if (!partOn[i & 1][i >> 1]) continue;
        if (c.matSrcVtx || (c.enable && c.ambSrcVtx)) needCol[i & 1] = true;
        if (c.enable && c.lightMask) {
            needLights = true;
            if (c.diffuse || c.attn == 1) needNrm = true;
        }
    }
    if (k.numChans < 1 && (partOn[0][0] || partOn[0][1])) needCol[0] = true;
    if (k.numChans < 2 && (partOn[1][0] || partOn[1][1])) needCol[1] = true;
    for (int i = 0; i < k.numTexGens; ++i) {
        const VsKey::TexGen& t = k.tg[i];
        if (t.type >= 2) continue;
        // (the uniform arrays stay declared as for every texgen; only the attributes are read for the computed ones)
        if (tgOn(i)) {
            if (t.src == 1) needNrm = true;
            if (t.src >= 5 && t.src <= 12) texSrcMask |= static_cast<uint8_t>(1u << (t.src - 5));
        }
        if (t.perVtxMtx) { needPal = true; needTmi = needTmi || tgOn(i); }
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
    for (int ch = 0; ch < 2; ++ch)
        if (!outGone[ch]) o("varying vec4 v_col%d;\n", ch);
    // (GXV_TEXVEC2, VsKey::texVec2Mask: the fragment shader declares these as vec2)
    const uint8_t v2 = k.texVec2Mask();
    auto texV2 = [&](int i) { return (v2 & (1u << i)) != 0 || k.indScaled(i); }; // (GXV_IND_VS: an indirect-scaled texgen is a vec2 too)
    // GXV_IND_VS (VsKey::indScaled): " * vec2(2^-s, 2^-t)" (exact decimal literals), "" for a scale of 1 or a texgen without the mark
    auto indMul = [&](int i) -> std::string {
        if (!k.indScaled(i) || (k.indScaleS(i) == 0 && k.indScaleT(i) == 0)) return "";
        return " * vec2(" + ExactLit(std::ldexp(1.0, -k.indScaleS(i))) + ", " + ExactLit(std::ldexp(1.0, -k.indScaleT(i))) + ")";
    };
    for (int i = 0; i < k.numTexGens; ++i) o(texV2(i) ? "varying vec2 v_tex%d;\n" : "varying vec3 v_tex%d;\n", i);
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
        if (!partOn[ch][0] && !partOn[ch][1]) continue; // (GXV_VSOUT: nothing of this channel is read; no colN)
        if (ch >= k.numChans) {
            o("  vec4 col%d = a_col%d;\n", ch, ch);
            continue;
        }
        o("  vec4 col%d;\n  {\n", ch);
        {
            const VsKey::Chan& cc = k.chan[ch];
            const VsKey::Chan& ca = k.chan[ch + 2];
            // (GXV_VSOUT: with one part not read, that part's light loop is left out below: the merged loop needs both)
            if (LightMerge() && partOn[ch][0] && partOn[ch][1] && cc.enable && ca.enable && cc.lightMask && cc.lightMask == ca.lightMask &&
                cc.attn == ca.attn && cc.diffuse == ca.diffuse) {
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
                    trimOps += EmitLightAttn(o, k, cc, l, b);
                    if (cc.diffuse == 1) o("      at *= dot(ld, n);\n");
                    else if (cc.diffuse == 2) o("      at *= max(0.0, dot(ld, n));\n");
                    o("      lm += at * u_lt[%d];\n    }\n", b + 2);
                }
                if (LightSumNonNegative(k, cc)) { o("    lm = min(lm, vec4(1.0));\n"); ++trimOps; } // (GXV_VS_TRIM)
                else o("    lm = clamp(lm, 0.0, 1.0);\n");
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
            if (!partOn[ch][part]) {
                // (GXV_VSOUT: no fragment shader reads this part; a constant keeps colN defined)
                o("    col%d.%s = %s(0.0);\n", ch, sw, ty);
                continue;
            }
            if (c.enable) {
                if (c.ambSrcVtx) o("    %s %s = a_col%d.%s;\n", ty, nm, ch, sw);
                else o("    %s %s = u_lt[%d].%s;\n", ty, nm, ch, sw);
                for (int l = 0; l < 8; ++l) {
                    if (!(c.lightMask & (1u << l))) continue;
                    const int b = 4 + l * 5; // pos, dir, col, cosatt, distatt
                    o("    {\n      vec3 ld = u_lt[%d].xyz - eye;\n      float d2 = dot(ld, ld);\n      float d1 = sqrt(d2);\n"
                      "      if (d1 > 0.0) ld = ld / d1;\n      float at = 1.0;\n", b);
                    trimOps += EmitLightAttn(o, k, c, l, b);
                    if (c.diffuse == 1) o("      at *= dot(ld, n);\n");
                    else if (c.diffuse == 2) o("      at *= max(0.0, dot(ld, n));\n");
                    o("      %s += at * u_lt[%d].%s;\n    }\n", nm, b + 2, sw);
                }
                if (LightSumNonNegative(k, c)) { o("    %s = min(%s, %s(1.0));\n", nm, nm, ty); ++trimOps; } // (GXV_VS_TRIM)
                else o("    %s = clamp(%s, 0.0, 1.0);\n", nm, nm);
            } else {
                o("    %s %s = %s(1.0);\n", ty, nm, ty);
            }
            if (c.matSrcVtx) o("    col%d.%s = a_col%d.%s * %s;\n", ch, sw, ch, sw, nm);
            else o("    col%d.%s = u_lt[%d].%s * %s;\n", ch, sw, 2 + ch, sw, nm);
        }
        o("  }\n");
    }
    {
        // GXV_DBG_LIT (a test switch): 1 = every lit channel gives white (the material's own colour and texture show without
        // the scene's lights), 2 = the lights without the ambient (the channels' ambient colour is ignored). For telling a
        // dark-looking material from dark light data.
        static const int dbgLit = EnvInt("GXV_DBG_LIT", 0);
        if (dbgLit == 1) {
            for (int ch = 0; ch < k.numChans && ch < 2; ++ch)
                if (k.chan[ch].enable && (partOn[ch][0] || partOn[ch][1])) o("  col%d.rgb = vec3(1.0);\n", ch);
        }
    }
    for (int ch = 0; ch < 2; ++ch) {
        if (outGone[ch]) continue; // (GXV_VSOUT: the fragment shader does not declare it)
        if (partOn[ch][0] || partOn[ch][1]) o("  v_col%d = col%d;\n", ch, ch);
        else o("  v_col%d = vec4(0.0);\n", ch); // (declared there, never read)
    }

    // Texture coordinate generation.
    for (int i = 0; i < k.numTexGens; ++i) {
        const VsKey::TexGen& t = k.tg[i];
        if (!tgOn(i)) {
            o(texV2(i) ? "  v_tex%d = vec2(0.0);\n" : "  v_tex%d = vec3(0.0);\n", i); // (GXV_VSOUT: declared by the fragment shader, never read)
            continue;
        }
        if (t.type >= 2) {
            if (texV2(i)) o("  v_tex%d = col%d.rg%s;\n", i, t.type - 2, indMul(i).c_str());
            else o("  v_tex%d = vec3(col%d.rg, 1.0);\n", i, t.type - 2);
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
        if (texV2(i)) o("    v_tex%d = tc.xy%s;\n  }\n", i, indMul(i).c_str());
        else o("    v_tex%d = tc;\n  }\n", i);
    }
    // GXV_FIX_LINES: GXEnableTexOffsets. A line's far side (the +side of lc) is moved by the GX_TO_* amount in s; a point's right corners
    // in s and its bottom corners in t. (Dolphin VideoCommon does the same, on the NDC offset's sign.)
    if (lpMode && k.lpAmount() >= 1 && k.lpAmount() <= 5 && k.lpTexMask()) {
        static const char* const kAmt[6] = {"0.0", "0.0625", "0.125", "0.25", "0.5", "1.0"};
        const char* a = kAmt[k.lpAmount()];
        for (int i = 0; i < k.numTexGens; ++i) {
            if (!(k.lpTexMask() & (1u << i))) continue;
            if (k.indScaled(i)) {
                // (GXV_IND_VS: the offset in the scaled coordinate, amount * 2^-s: (x + a) * 2^-s and x * 2^-s + a * 2^-s are the same float)
                const double amt = std::strtod(a, nullptr);
                o("  if (lc.x > 0.0) v_tex%d.x += %s;\n", i, ExactLit(std::ldexp(amt, -k.indScaleS(i))).c_str());
                if (lpMode == 2) o("  if (lc.y > 0.0) v_tex%d.y += %s;\n", i, ExactLit(std::ldexp(amt, -k.indScaleT(i))).c_str());
                continue;
            }
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
    src.trimOps = static_cast<uint8_t>(trimOps > 255 ? 255 : trimOps);
    {
        // (GXV_VSOUT census, GXV_GPU_SCENES' line)
        uint8_t pr = static_cast<uint8_t>((outGone[0] ? 1u : 0u) | (outGone[1] ? 2u : 0u) | (texOff ? 8u : 0u));
        for (int ch = 0; ch < 2; ++ch)
            if (!outGone[ch] && (!partOn[ch][0] || !partOn[ch][1])) pr |= 4u;
        src.outPruned = pr;
    }
}

} // namespace vgx
