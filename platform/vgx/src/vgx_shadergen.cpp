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
#include "vgx_ubershader.h"
#include "vgx_silhfuse.h" // (silhfuse26: GXV_SILH_FUSE)
extern "C" void port_log_summary(const char*, ...) __attribute__((weak, format(printf, 1, 2))); // (the Vita's basic log; absent on a host)

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
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
long FogBindMode() {
    static const long m = [] {
        const long v = EnvInt("GXV_FOG_BIND", 0);
        return (v == 1 || v == 2) ? v : 0L;
    }();
    return m;
}
int FogBindSlot() {
    static const int s = [] {
        const long v = EnvInt("GXV_FOG_SLOT", 7);
        return (v >= 0 && v <= 9) ? static_cast<int>(v) : 7;
    }();
    return s;
}
const char* FogVaryingName() {
    static const char* const name = [] {
        static char buf[16];
        switch (FogBindMode()) {
        case 1: return "v_fogc";
        case 2: std::snprintf(buf, sizeof buf, "v_fogt%d", FogBindSlot()); return static_cast<const char*>(buf);
        default: return "v_fog";
        }
    }();
    return name;
}
// Mode 2: whether this shader also declares the varying that TEXCOORD<slot> is bound to (v_tex<slot>, v_col0 for 8, v_col1 for 9).
static void NoteFogSlotCollision(bool vertex, int numTexGens, bool col0, bool col1) {
    if (FogBindMode() != 2) return;
    const int s = FogBindSlot();
    const bool hit = (s < 8 && s < numTexGens) || (s == 8 && col0) || (s == 9 && col1);
    if (!hit) return;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true) && port_log_summary)
        port_log_summary("fog slot collision: a %s shader with fog also uses TEXCOORD%d (GXV_FOG_SLOT); use GXV_FOG_BIND=1 or another slot",
                         vertex ? "vertex" : "fragment", s);
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
bool FsHasAlphaTestText(const FsKey& k) {
    return k.alphaTestOn && !k.atPass() && !(NoDiscard() && AlphaTestAlwaysPasses(k.alphaTest & 7, -1, (k.alphaTest >> 6) & 3, (k.alphaTest >> 3) & 7, -1));
}
// ---- GXV_FS_ATFAST (shader37): the cut-out as one compare / one interval test against thresholds the draw gives in alpha units
namespace { long g_atFastTest = -1, g_vsNoBranchTest = -1, g_vsNoBranchLightTest = -1; }
long FsAtFastMode() {
    static const long m = [] {
        const long v = EnvInt("GXV_FS_ATFAST", 0);
        return v >= 1 && v <= 2 ? v : 0L;
    }();
    return g_atFastTest >= 0 ? g_atFastTest : m;
}
void FsAtFastModeForTest(long m) { g_atFastTest = m; }
long VsNoBranchMode() {
    static const long m = EnvInt("GXV_VS_NOBRANCH", 0) == 1 ? 1 : 0;
    return g_vsNoBranchTest >= 0 ? g_vsNoBranchTest : m;
}
void VsNoBranchModeForTest(long m) { g_vsNoBranchTest = m; }
long VsNoBranchLightMode() {
    static const long m = EnvInt("GXV_VS_NOBRANCH_LIGHT", 0) == 1 ? 1 : 0;
    const long lm = g_vsNoBranchLightTest >= 0 ? g_vsNoBranchLightTest : m;
    return (VsNoBranchMode() && lm) ? 1 : 0;
}
void VsNoBranchLightModeForTest(long m) { g_vsNoBranchLightTest = m; }
namespace {
// the comparison's class: 1 lower bound (GREATER, GEQUAL), 2 upper bound (LESS, LEQUAL), 3 both (EQUAL), 4 no bound (ALWAYS), 0 not an interval
int AtCmpClass(uint32_t c) {
    switch (c) {
    case 1: case 3: return 2;
    case 4: case 6: return 1;
    case 2: return 3;
    case 7: return 4;
    default: return 0; // NEVER, NEQUAL
    }
}
} // namespace
int AtFastKind(uint8_t alphaTest) {
    const int a = AtCmpClass(alphaTest & 7), b = AtCmpClass((alphaTest >> 3) & 7), op = (alphaTest >> 6) & 3;
    if (!a || !b) return 0;
    if (op == 0) { // AND: the intersection
        const bool anyLo = a == 1 || a == 3 || b == 1 || b == 3, anyHi = a == 2 || a == 3 || b == 2 || b == 3;
        return anyLo && anyHi ? 3 : anyLo ? 1 : anyHi ? 2 : 0;
    }
    if (op == 1 && a == b && (a == 1 || a == 2)) return a; // OR of two lower (upper) bounds: the weaker one
    return 0;                                                // OR of unlike bounds, XOR, XNOR: not one interval
}
bool FsAtFastKey(const FsKey& k) {
    if (FsAtFastMode() < 1 || !k.alphaTestOn || k.atPass() || k.fog || k.silhFuse()) return false;
    if (NoDiscard() && AlphaTestAlwaysPasses(k.alphaTest & 7, -1, (k.alphaTest >> 6) & 3, (k.alphaTest >> 3) & 7, -1)) return false;
    return AtFastKind(k.alphaTest) != 0;
}
bool FsAtEarlyKey(const FsKey& k) {
    if (FsAtFastMode() < 2 || !FsAtFastKey(k) || k.rejectZero() || k.preAlpha()) return false;
    return bits(k.stage[k.numStages - 1].alpha, 19, 1) == 1; // (the last stage's alpha is clamped on both sides: its `ra` is the test's alpha)
}
void AtFastValues(uint8_t alphaTest, float ref0, float ref1, float out[2]) {
    const uint32_t c[2] = {static_cast<uint32_t>(alphaTest & 7), static_cast<uint32_t>((alphaTest >> 3) & 7)};
    const double r[2] = {ref0, ref1};
    const int op = (alphaTest >> 6) & 3;
    // In units of 8-bit levels: a fragment passes comparison c with reference r when its level t = a * 255 is in [lo, hi).
    double lo[2] = {-1e30, -1e30}, hi[2] = {1e30, 1e30};
    for (int i = 0; i < 2; ++i) {
        switch (c[i]) {
        case 1: hi[i] = r[i] - 0.5; break;                     // LESS
        case 3: hi[i] = r[i] + 0.5; break;                     // LEQUAL
        case 4: lo[i] = r[i] + 0.5; break;                     // GREATER
        case 6: lo[i] = r[i] - 0.5; break;                     // GEQUAL
        case 2: lo[i] = r[i] - 0.5; hi[i] = r[i] + 0.5; break; // EQUAL
        default: break;                                        // ALWAYS: no bound
        }
    }
    double L, U;
    if (op == 0) { L = std::max(lo[0], lo[1]); U = std::min(hi[0], hi[1]); }
    else { L = std::min(lo[0], lo[1]); U = std::max(hi[0], hi[1]); } // (OR of two like bounds: the weaker)
    const int kind = AtFastKind(alphaTest);
    out[0] = out[1] = 0.0f;
    if (kind == 1) out[0] = static_cast<float>(L / 255.0);
    else if (kind == 2) out[1] = static_cast<float>(U / 255.0);
    else if (kind == 3) {
        out[0] = static_cast<float>((L + U) * 0.5 / 255.0);
        out[1] = static_cast<float>((U - L) * 0.5 / 255.0);
    }
}
static int FsTevCountBase(const FsKey& k) {
    if (!UniformPacking() || k.numIndStages) return 28;
    for (int s = 0; s < k.numStages; ++s)
        if (k.stage[s].lodBias) return 28; // (the biases sit after the texture sizes: TEV_LODBIAS)
    const int n = k.fog ? ((k.fog & kFogHalfBit) ? 12 : 11) : 9; // registers, konst, misc (+ fog parameters and colour; GXV_FS_HALF_FOG: row 11, the half-exact lanes)
    return FsAtFastKey(k) && n < 10 ? 10 : n; // (GXV_FS_ATFAST: row 9, the fog row of a key without fog, holds the test's thresholds)
}
// (GXV_TEVIN_PREWRAP: a key with dualWrap has four more rows, the wrapped copies of the registers, after the ones it had)
int FsTevCount(const FsKey& k) { return FsTevCountBase(k) + (k.dualWrap() ? 4 : 0); }
int FsTevBase(const FsKey& k) { return FsTevCountBase(k); }
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
enum { V_REG = 0, V_KONST = 4, V_MISC = 8, V_FOG = 9, V_FOGCOL = 10, V_INDMTX = 11, V_INDSCALE = 17, V_TEXSIZE = 21, V_LODBIAS = 25, V_INDPRE = 27, V_ATTHR = 9, V_FOGHL = 11 };

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
constexpr double kFogFloatErr = 4e-4;     // FogHalfLanes: the float evaluation of the fog factor from its half-stored lanes, |bs| < 4096 (analytic worst 7.3e-4; measured 8e-4 with the slope lane: tests/vgx_fs24_gl.cpp)
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

// ---- GXV_TEVIN_CANON (shader23): which register reads of a key the out-of-range mark changes (see vgx_shadergen.h) -----------------
namespace { long g_tevInCanonTest = -1, g_fsProjTest = -1; } // (host tests: both forms in one process)
long TevInCanonMode() {
    static const long m = [] {
        const long v = EnvInt("GXV_TEVIN_CANON", 0);
        return v == 1 || v == 2 ? v : 0L;
    }();
    return g_tevInCanonTest >= 0 ? g_tevInCanonTest : m;
}
void TevInCanonModeForTest(long m) { g_tevInCanonTest = m; }
namespace { long g_finClampTest = -1, g_alphaDeadTest = -1; }
long FsFinClampMode() {
    static const long m = EnvInt("GXV_FS_FINCLAMP", 0) == 1 ? 1 : 0;
    return g_finClampTest >= 0 ? g_finClampTest : m;
}
long FsAlphaDeadMode() {
    static const long m = EnvInt("GXV_FS_ADEAD", 0) == 1 ? 1 : 0;
    return g_alphaDeadTest >= 0 ? g_alphaDeadTest : m;
}
// GXV_FS_TEXREUSE (gpucut23; 0 off, 1 on): see GenerateFragmentShaderAs. A stage's texture read that repeats the read `tex` holds is left out.
namespace { long g_texReuseTest = -1; }
long FsTexReuseMode() {
    static const long m = EnvInt("GXV_FS_TEXREUSE", 0) == 1 ? 1 : 0;
    return g_texReuseTest >= 0 ? g_texReuseTest : m;
}
void FsTexReuseModeForTest(long m) { g_texReuseTest = m; }
void FsFinClampModeForTest(long m) { g_finClampTest = m; }
void FsAlphaDeadModeForTest(long m) { g_alphaDeadTest = m; }
void FsProjModeForTest(long m) { g_fsProjTest = m; }
namespace {
// The register reads of a key, walked with the ranges FsTrimAnalyse propagates (TrimStage).
//   known: the initial values' ranges are known: the registers in oorMask are outside 0..255, every other one is in 0..1 (the front end's
//          decision); otherwise (the text of a key that keeps the mark) every initial value may be outside, as FsTrimAnalyse takes it.
// keep[s]: the A / B / C register reads of stage s that keep their wrap (colour a, b, c = 1, 2, 4; alpha a, b, c = 8, 16, 32).
// initAbc / initD: bit r (colour part) / r + 4 (alpha part) = register r's initial value is read as A / B / C, as D.
struct TevInWalk { bool needMark = false; uint8_t keep[16] = {}, keepInit[16] = {}; uint8_t initAbc = 0, initD = 0; uint8_t dualRegs = 0; };
void WalkTevIn(const FsKey& k, unsigned oorMask, bool wrapIn, bool known, TevInWalk& w) {
    w = TevInWalk{};
    const TrimRng unit{0.0, 1.0}, one{1.0, 1.0}, zero{0.0, 0.0}, half{0.5, 0.5}, wide{-1024.0 / 255.0, 1023.0 / 255.0};
    const int n = k.numStages < 1 ? 1 : (k.numStages > 16 ? 16 : k.numStages);
    bool hasCompare = false;
    for (int s = 0; s < n; ++s)
        if (bits(k.stage[s].color, 16, 2) == 3 || bits(k.stage[s].alpha, 16, 2) == 3) hasCompare = true;
    bool oorC[4], oorA[4], initC[4], initA[4]; // (out of range: the colour part / the alpha part of the register's initial value)
    TrimRng rc[4], ra[4];
    for (int r = 0; r < 4; ++r) {
        // (text of a marked key: a pre-wrapped register (GXV_TEVIN_CANON=2) arrives as its low byte / 255, in 0..1)
        oorC[r] = known ? ((oorMask >> r) & 1u) != 0 : ((k.preWrap() >> r) & 1u) == 0;
        oorA[r] = known ? ((oorMask >> r) & 1u) != 0 : ((k.preWrap() >> (r + 4)) & 1u) == 0;
        if ((r == 1 && (k.regFromVtx & 1)) || (r == 2 && (k.regFromVtx & 2))) oorC[r] = oorA[r] = false; // (vertex colours: FoldRegs only for 0..255)
        rc[r] = oorC[r] ? wide : unit;
        ra[r] = oorA[r] ? wide : unit;
        initC[r] = initA[r] = true;
    }
    TrimRng tex = one, kc = one, ka = one;
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
        // a register part read as an input (abcBit: its keep bit for A / B / C, 0 for D)
        auto regRead = [&](unsigned r, bool alpha, uint8_t abcBit) -> TrimRng {
            const bool init = alpha ? initA[r] : initC[r];
            const TrimRng v = alpha ? ra[r] : rc[r];
            const bool oor = alpha ? oorA[r] : oorC[r];
            if (init && oor && known) w.needMark = true; // (the initial value of an out-of-range register, as any input)
            if (init) (abcBit ? w.initAbc : w.initD) |= static_cast<uint8_t>(1u << (r + (alpha ? 4 : 0)));
            if (!abcBit || !wrapIn) return v;
            // the wrap returns its argument: an initial value in range (exactly), or a stage's value proven in 0..1 (not in a key with a
            // compare-mode stage, whose compares can turn on a rounding the wrap would remove)
            const bool identity = v.lo >= 0.0 && v.hi <= 1.0 && (init ? !oor : !hasCompare);
            if (identity) return v;
            w.keep[s] |= abcBit;
            if (init) { w.keepInit[s] |= abcBit; w.dualRegs |= static_cast<uint8_t>(1u << r); } // (an initial value read through a kept wrap: GXV_TEVIN_PREWRAP)
            if (known) w.needMark = true;
            return unit;
        };
        auto colourArg = [&](uint32_t a, uint8_t abcBit) -> TrimRng {
            a &= 15;
            if (a < 8) return regRead(a / 2, (a & 1) != 0, abcBit);
            switch (a) {
            case 8: case 9: return tex;
            case 10: case 11: return ras;
            case 12: return one;
            case 13: return half;
            case 14: return kc;
            default: return zero;
            }
        };
        auto alphaArg = [&](uint32_t a, uint8_t abcBit) -> TrimRng {
            a &= 7;
            if (a < 4) return regRead(a, true, abcBit);
            return a == 4 ? tex : (a == 5 ? ras : (a == 6 ? ka : zero));
        };
        FsTrimClamp dummy;
        const TrimRng ca0 = colourArg(bits(cc, 12, 4), 1), cb0 = colourArg(bits(cc, 8, 4), 2), cc0 = colourArg(bits(cc, 4, 4), 4),
                      cd0 = colourArg(bits(cc, 0, 4), 0);
        const TrimRng aa0 = alphaArg(bits(ca, 13, 3), 8), ab0 = alphaArg(bits(ca, 10, 3), 16), ac0 = alphaArg(bits(ca, 7, 3), 32),
                      ad0 = alphaArg(bits(ca, 4, 3), 0);
        const TrimRng nc = TrimStage(ca0, cb0, cc0, cd0, cc, dummy);
        const TrimRng na = TrimStage(aa0, ab0, ac0, ad0, ca, dummy);
        // (TEV reads both sources before either destination is written)
        rc[bits(cc, 22, 2)] = nc;
        initC[bits(cc, 22, 2)] = false;
        ra[bits(ca, 22, 2)] = na;
        initA[bits(ca, 22, 2)] = false;
    }
}
} // namespace
bool FsTevInNeedsMark(const FsKey& k, unsigned oorMask, bool wrapIn) {
    if (!(oorMask & 15u)) return false;
    TevInWalk w;
    WalkTevIn(k, oorMask & 15u, wrapIn, true, w);
    return w.needMark;
}
uint8_t FsTevInPreWrapMask(const FsKey& k) {
    TevInWalk w;
    WalkTevIn(k, 0xFu, true, true, w);
    uint8_t m = static_cast<uint8_t>(w.initAbc & ~w.initD);
    if (k.regFromVtx & 1) m &= static_cast<uint8_t>(~0x22u); // (registers from the vertices: never through the block)
    if (k.regFromVtx & 2) m &= static_cast<uint8_t>(~0x44u);
    return m;
}
namespace { long g_prewrapTest = -1; }
bool TevInPrewrapMode() {
    static const long m = EnvInt("GXV_TEVIN_PREWRAP", 0) == 1 ? 1 : 0;
    return (g_prewrapTest >= 0 ? g_prewrapTest : m) != 0;
}
void TevInPrewrapModeForTest(long m) { g_prewrapTest = m; }
// The registers whose initial value is read through a kept wrap (A / B / C) AND as D: the ones GXV_TEVIN_CANON=2's preWrap leaves alone (it needs "never as D").
// Computed as the generator will see the key: the walk of a marked key's text (known = false), with the parts preWrap takes already in range.
uint8_t FsTevInDualMask(const FsKey& k) {
    if (!(k.inWrap & 1)) return 0;
    TevInWalk w;
    WalkTevIn(k, 0xFu, true, false, w);
    uint8_t m = 0;
    for (int r = 0; r < 4; ++r) {
        const bool abc = ((w.initAbc >> r) | (w.initAbc >> (r + 4))) & 1u, d = ((w.initD >> r) | (w.initD >> (r + 4))) & 1u;
        if (abc && d && ((w.dualRegs >> r) & 1u)) m = static_cast<uint8_t>(m | (1u << r));
    }
    if (k.regFromVtx & 1) m &= static_cast<uint8_t>(~0x2u); // (registers from the vertices: never through the block)
    if (k.regFromVtx & 2) m &= static_cast<uint8_t>(~0x4u);
    return m;
}
// (canon24b) A fragment key of a warm-up list in the form the front end now makes it: under GXV_TEVIN_CANON=2 a marked key carries
// FsTevInPreWrapMask (BuildFsKey), otherwise no key has a preWrap mask. True when the key changed.
bool CanonListFsKey(FsKey& k) {
    const uint8_t want = (TevInCanonMode() == 2 && (k.inWrap & 1)) ? FsTevInPreWrapMask(k) : static_cast<uint8_t>(0);
    // (GXV_TEVIN_PREWRAP: the dual mask of the key with the pre-wrap mask it will have; 0 with the switch off, so old lists' keys are unchanged)
    uint8_t dual = 0;
    if (TevInCanonMode() == 2 && TevInPrewrapMode() && (k.inWrap & 1)) {
        FsKey t = k;
        t.setPreWrap(want);
        t.setDualWrap(0);
        dual = FsTevInDualMask(t);
    }
    if (k.preWrap() == want && k.dualWrap() == dual) return false;
    k.setPreWrap(want);
    k.setDualWrap(dual);
    return true;
}
// ---- GXV_FS_PROJ (shader23) ----------------------------------------------------------------------------------------------------------
long FsProjMode() {
    static const long m = EnvInt("GXV_FS_PROJ", 0) == 1 ? 1 : 0;
    return g_fsProjTest >= 0 ? g_fsProjTest : m;
}

namespace {

// (trim: the clamps the GXV_FS_TRIM text keeps; a side it leaves out does not bound the half error either)
FsHalfReport AnalyseHalfAs(const FsKey& k, bool trim) {
    FsHalfReport rep;
    auto No = [&](const char* why) { rep.why = why; return rep; };
    if (k.numIndStages) return No("indirect stage");
    // fog: only the vertex fog with half-exact parameters (kFogHalfBit, GXV_FS_HALF_FOG: FogHalfLanes), or a key whose colour output is dead (colorDead: no fog text)
    if (k.fog && !k.colorDead() && (k.fog & (kFogVtxBit | kFogHalfBit)) != (kFogVtxBit | kFogHalfBit)) return No("fog");
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
    if (k.fog && !k.colorDead()) {
        // fog (kFogHalfBit): out = (1 - fg) * fin.rgb + fg * fogcolour in FLOAT (the half TEV result and the half-stored fog colour are converted; the arithmetic is
        // float: its own rounding, 6e-8, is added). So the error is the convex combination of the two inputs' errors and |fogcolour - fin| * the factor's error:
        // at most max(e(fin), e(fogcolour)) + e(fg). e(fogcolour) is n / 255 rounded to half once; e(fg) is the slope lane c1s rounded to half (a relative 2^-11 on
        // a factor of at most 1, exactly 0 at the ramp's start) and the float evaluation from the lanes (kFogFloatErr for |bs| < 4096, FogHalfLanes).
        const double efog = AbIn(0.0, 1.0).e, efg = kU16 + kFogFloatErr;
        rep.colorLevels = (std::max(fr.e, efog) + efg + 2e-7) * 255.0;
    }
    if (k.colorDead()) rep.colorLevels = 0.0; // (the colour output is the constant 0: exact in half)
    rep.alphaLevels = fa.e * 255.0;
    rep.alphaTested = k.alphaTestOn && !(NoDiscard() && AlphaTestAlwaysPasses(k.alphaTest & 7, -1, (k.alphaTest >> 6) & 3, (k.alphaTest >> 3) & 7, -1));
    rep.testedAlphaLevels = fa.e * 255.0; // (GXV_FS_HALF_MINOR: what the alpha test compares, whatever the output's alpha becomes)
    if (k.dstAlpha) rep.alphaLevels = AbIn(0.0, 1.0).e * 255.0; // a constant 8-bit alpha, rounded to half once
    if (k.alphaDead() && !rep.alphaTested) rep.alphaLevels = 0.0; // (GXV_FS_ADEAD: the alpha output reaches nothing; colour's own bound covers what it reads of alpha)
    return rep;
}

} // namespace

bool FsHalfFogMode() {
    static const bool on = EnvInt("GXV_FS_HALF_FOG", 0) != 0;
    return on;
}
bool FsNoColorMode() {
    static const bool on = EnvInt("GXV_FS_NOCOLOR", 0) != 0;
    return on;
}
float RoundToHalf(float x) {
    if (!(x == x)) return 0.0f;
    const float a = std::fabs(x);
    if (a >= 65520.0f) return x < 0 ? -65504.0f : 65504.0f; // (the largest half; 65520 and up round to infinity: kept finite)
    if (a == 0.0f) return 0.0f;
    int e = 0;
    std::frexp(a, &e); // a = f * 2^e, f in [0.5, 1): the leading bit is 2^(e-1)
    const int lead = e - 1;
    const double q = std::ldexp(1.0, (lead < -14 ? -14 : lead) - 10); // (a half's spacing: 10 mantissa bits; subnormals share the spacing of 2^-14)
    const double r = std::nearbyint(static_cast<double>(a) / q) * q;   // (ties to even in the default rounding mode)
    return static_cast<float>(x < 0 ? -r : r);
}
FogHalfLane FogHalfLanes(float c1, float c0) {
    FogHalfLane r{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, false};
    const double k0 = std::isfinite(c0) ? std::min(1.0, std::max(0.0, static_cast<double>(c0))) : 0.0;
    r.k = RoundToHalf(static_cast<float>(k0));
    if (!std::isfinite(c1) || c1 == 0.0f) return r; // a flat factor
    int e = 0;
    std::frexp(std::fabs(c1), &e); // |c1| = f * 2^e, f in [0.5, 1)
    int ex = e;                    // sInv = 2^ex: c1 / sInv = f in [0.5, 1)
    if (ex < -24) { r.sInv = 1.0f; return r; } // too gentle for the lanes (a factor below 6e-8 a unit of v): flat, k only
    if (ex > 15) ex = 15;                       // too steep: a step, kept at the steepest slope the lanes give
    // (fix24) The scale lane is itself a half: below 2^-14 it would be a SUBNORMAL, which the console may flush to zero (the fog would vanish). Kept a normal half; the slope lane
    // c1s = c1 / sInv then falls to 2^-11 at the gentlest (still normal), and bs = -c0 / c1s only gets smaller in the units of v * sInv.
    if (ex < -14) ex = -14;
    const double sInv = std::ldexp(1.0, ex);
    const double c1s = static_cast<double>(c1) / sInv;
    if (std::fabs(c1s) > 60000.0) { r.sInv = 1.0f; return r; }
    const double bs = -static_cast<double>(c0) / c1s;
    const double bsc = std::max(-60000.0, std::min(60000.0, bs));
    r.wide = std::fabs(bs) > 4096.0; // (fix24: the analytic bound kFogFloatErr covers |bs| < 4096; above, counted, must be 0)
    r.sInv = static_cast<float>(sInv);
    r.c1s = RoundToHalf(static_cast<float>(c1s));
    r.bHi = RoundToHalf(static_cast<float>(bsc));
    r.bLo = RoundToHalf(static_cast<float>(bsc - static_cast<double>(r.bHi)));
    r.k = 0.0f; // (a ramp: the factor is c1s * (v * sInv - bs))
    return r;
}
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
bool FsHalfReport::EligibleMinor() const {
    if (!eligibleShape) return false;
    return colorLevels < kHalfMaxLevelsMinor && (alphaTested ? testedAlphaLevels <= kHalfMaxLevelsAlphaTest : alphaLevels < kHalfMaxLevelsMinor);
}
namespace { long g_fsHalfMinorTest = -1; }
long FsHalfMinorMode() {
    static const long m = [] {
        const char* e = std::getenv("GXV_FS_HALF_MINOR");
        if (!e || !*e) return 0L;
        if (*e == 'p' || *e == 'P') { // ("perf": Performance mode only, as port_perf_mode_60 decides it)
            const char* t = std::getenv("GXV_AUTOQ_TARGET");
            const char* f = std::getenv("GXV_FPS_LIMIT");
            return (t && std::atoi(t) == 60 && !(f && std::atoi(f) == 30)) ? 1L : 0L;
        }
        return std::atoi(e) == 1 ? 1L : 0L;
    }();
    return g_fsHalfMinorTest >= 0 ? g_fsHalfMinorTest : m;
}
void FsHalfMinorModeForTest(long m) { g_fsHalfMinorTest = m; }
bool FsHalfEligible(const FsKey& key) {
    const long m = FsHalfMode();
    if (m != 1 && m != 3) return false;
    const FsHalfReport r = AnalyseHalfAs(key, false);
    return r.Eligible(m) || (FsHalfMinorMode() == 1 && r.EligibleMinor()); // (GXV_FS_HALF_MINOR: only adds keys)
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
// asyncsh31 (GXV_UBERSHADER, vgx_ubershader.h): the per-key decisions GenerateFragmentShaderAs makes, taken by the same code paths it takes, so that the ubershader's
// configuration cannot drift from the specialised text. The untrimmed form: every trim GXV_FS_TRIM makes is proven inert (a clamp side that cannot act, a wrap of
// a value already in 0..1, a floor whose compare gives the same answer), so the ubershader keeps the full operations (the text GXV_FS_TRIM=0 writes).
void FsUberPlanOf(const FsKey& k, FsUberPlan& p) {
    p = FsUberPlan{};
    std::fill(p.wrapKeep, p.wrapKeep + 16, static_cast<uint8_t>(0x3F));
    if ((k.inWrap & 1) && TevInCanonMode() == 2) {
        TevInWalk tw;
        WalkTevIn(k, 0xFu, true, false, tw);
        for (int s = 0; s < 16; ++s) p.wrapKeep[s] = tw.keep[s];
    }
    p.wrapIn = (k.inWrap & 1) != 0;
    p.dualRegs = (k.inWrap & 1) && TevInCanonMode() == 2 ? k.dualWrap() : static_cast<uint8_t>(0);
    FinalWrapPlan(k, p.wrapColor, p.wrapAlpha);
    static const bool indPrevOn = EnvInt("GXV_FIX_INDPREV", 0) != 0; // (as GenerateFragmentShaderAs reads it)
    for (int s = 0; indPrevOn && s < k.numStages; ++s)
        if (k.stage[s].ind && bits(k.stage[s].ind, 20, 1)) p.indPrev = true;
    const bool alwaysPasses = NoDiscard() && AlphaTestAlwaysPasses(k.alphaTest & 7, -1, (k.alphaTest >> 6) & 3, (k.alphaTest >> 3) & 7, -1);
    p.alphaTestedText = k.alphaTestOn && !alwaysPasses;
    p.testText = k.alphaTestOn && !k.atPass() && !alwaysPasses;
    p.rejectZero = p.testText && (k.rejectZero() != 0 || k.silhFuse() != 0);
    p.preOnly = p.testText && k.preAlpha() != 0 && !p.rejectZero;
    p.alphaZero = !k.dstAlpha && k.alphaDead() && !p.alphaTestedText;
    p.half = FsHalfEligible(k) && k.silhFuse() == 0;
    p.projRead = FsProjMode() != 0;
}
// asyncsh31: the ubershader's interface and predicate live here, next to FsUberPlanOf, so that the front end's census (vgx_draw.cpp) links wherever the
// generator does; the text and the configuration are in vgx_ubershader.cpp.
UberIface UberIfaceOf(const FsKey& k) {
    UberIface f;
    f.numTexGens = k.numTexGens;
    for (int s = 0; s < k.numStages && s < 16; ++s) {
        if (k.stage[s].texEnable) f.texMask |= static_cast<uint8_t>(1u << (k.stage[s].texMap & 7));
        if (k.stage[s].rasChan < 2) f.col |= static_cast<uint8_t>(1u << k.stage[s].rasChan);
    }
    for (int i = 0; i < k.numIndStages && i < 4; ++i) f.texMask |= static_cast<uint8_t>(1u << (k.ind[i].texMap & 7));
    f.col |= static_cast<uint8_t>(k.regFromVtx & 3);
    uint8_t indVsTg = 0;
    for (int i = 0; i < k.numIndStages && i < 4; ++i)
        if (k.indVs(i) && k.ind[i].texCoord < k.numTexGens) indVsTg |= static_cast<uint8_t>(1u << k.ind[i].texCoord);
    for (int i = 0; i < k.numTexGens && i < 8; ++i)
        if ((k.texVec2() && !(k.texProjMask & (1u << i))) || ((indVsTg >> i) & 1u)) f.vec2Mask |= static_cast<uint8_t>(1u << i);
    f.fogVtx = (k.fog & kFogVtxBit) ? 1 : 0;
    return f;
}

bool UberFsCovers(const FsKey& k, const char** why) {
    auto no = [&](const char* w) { if (why) *why = w; return false; };
    if (k.numStages < 1 || k.numStages > kUberMaxStages) return no("stages");
    if (k.numTexGens > 8 || k.numIndStages > 4) return no("texgens");
    if (k.silhFuse()) return no("silhfuse");
    if (k.indPre()) return no("indpre");
    if (FsHalfMode() == 2) return no("fshalf2");
    if (FsRegsMode()) return no("fsregs");
    FsUberPlan p;
    FsUberPlanOf(k, p);
    if (p.dualRegs) return no("dualwrap");
    if (p.projRead && k.texProjMask) return no("fsproj");
    for (int s = 0; s < k.numStages; ++s) {
        const uint32_t ind = k.stage[s].ind;
        if (ind && (bits(ind, 13, 3) == 7 || bits(ind, 16, 3) == 7)) return no("indwrap7");
    }
    if (why) *why = "";
    return true;
}


namespace {
long g_fsAtEarlyTest = -1;
struct FsAtEarlyAtomic {
    std::atomic<uint64_t> split{0}, ineligible{0}, colorStagesDeferred{0}, texKeptBeforeDiscard{0}, snapVars{0};
} g_fsAtEarlyStats;
} // namespace

long FsAtEarlyMode() {
    static const long m = EnvInt("GXV_FS_ATEARLY", 0) == 1 ? 1 : 0;
    return g_fsAtEarlyTest >= 0 ? g_fsAtEarlyTest : m;
}
void FsAtEarlyModeForTest(long m) { g_fsAtEarlyTest = m; }
void FsAtEarlyCounters(FsAtEarlyStats& out) {
    out.split = g_fsAtEarlyStats.split.load(std::memory_order_relaxed);
    out.ineligible = g_fsAtEarlyStats.ineligible.load(std::memory_order_relaxed);
    out.colorStagesDeferred = g_fsAtEarlyStats.colorStagesDeferred.load(std::memory_order_relaxed);
    out.texKeptBeforeDiscard = g_fsAtEarlyStats.texKeptBeforeDiscard.load(std::memory_order_relaxed);
    out.snapVars = g_fsAtEarlyStats.snapVars.load(std::memory_order_relaxed);
}
bool FsAtEarlySplitEligible(const FsKey& k) {
    if (!FsHasAlphaTestText(k)) return false;
    if (k.silhFuse() || k.rejectZero() || k.preAlpha() || k.colorDead()) return false;
    if (k.numStages < 1 || k.numStages > 16) return false;
    for (int s = 0; s < k.numStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        // Alpha compare with scale < 3 (R8 / GR16 / BGR24) compares colour operands a, b.
        if (bits(st.alpha, 16, 2) == 3 && bits(st.alpha, 20, 2) < 3) return false;
        // Indirect addPrev carries state across stages.
        if (st.ind && bits(st.ind, 20, 1)) return false;
    }
    return true;
}


namespace {
long g_fsRegsTest = -1;
struct FsRegsAtomic {
    std::atomic<uint64_t> shaders{0}, rowsOld{0}, rowsNew{0}, maxOld{0}, maxNew{0};
    std::atomic<uint64_t> regsElided{0}, uvsSunk{0}, indSunk{0}, fallbacks{0};
} g_fsRegsStats;
void FsRegsAtomicMax(std::atomic<uint64_t>& dst, uint64_t v) {
    uint64_t cur = dst.load(std::memory_order_relaxed);
    while (v > cur && !dst.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
} // namespace

long FsRegsMode() {
    static const long m = EnvInt("GXV_FS_REGS", 0) == 1 ? 1 : 0;
    return g_fsRegsTest >= 0 ? g_fsRegsTest : m;
}
void FsRegsModeForTest(long m) { g_fsRegsTest = m; }
void FsRegsCounters(FsRegsStats& out) {
    out.shaders = g_fsRegsStats.shaders.load(std::memory_order_relaxed);
    out.rowsOld = g_fsRegsStats.rowsOld.load(std::memory_order_relaxed);
    out.rowsNew = g_fsRegsStats.rowsNew.load(std::memory_order_relaxed);
    out.maxOld = g_fsRegsStats.maxOld.load(std::memory_order_relaxed);
    out.maxNew = g_fsRegsStats.maxNew.load(std::memory_order_relaxed);
    out.regsElided = g_fsRegsStats.regsElided.load(std::memory_order_relaxed);
    out.uvsSunk = g_fsRegsStats.uvsSunk.load(std::memory_order_relaxed);
    out.indSunk = g_fsRegsStats.indSunk.load(std::memory_order_relaxed);
    out.fallbacks = g_fsRegsStats.fallbacks.load(std::memory_order_relaxed);
}

namespace {
struct FsRegsLayout {
    int8_t slot[32];
    uint8_t src[32] = {};
    uint8_t count = 0;
};

struct FsRegPlan {
    FsRegsLayout layout;
    uint8_t inWrapKeep[16] = {};
    uint8_t inWrapInit[16] = {};
    uint8_t dualRegs = 0;
    uint8_t indVsTg = 0;
    uint8_t projOnly = 0;
    bool indPrev = false;
    bool rgbWritten[4] = {}, aWritten[4] = {};
    bool rgbReadInit[4] = {}, aReadInit[4] = {};
    int firstRgbStage[4] = {-1, -1, -1, -1};
    int firstAStage[4] = {-1, -1, -1, -1};
    int texProducer[16] = {};
    bool texEmit[16] = {};
    bool texShared[16] = {};
    bool texReusedStage[16] = {};
    int finalTexProducer = -1;
    bool stageIndEmit[16] = {};
    int firstIndOpStage = -1;
    int firstIndStage[4] = {-1, -1, -1, -1};
    int firstProjUvStage[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
};

void BuildFsRegPlan(const FsKey& k, FsRegPlan& p) {
    p = FsRegPlan{};
    for (int i = 0; i < 4; ++i) p.firstRgbStage[i] = p.firstAStage[i] = p.firstIndStage[i] = -1;
    for (int i = 0; i < 8; ++i) p.firstProjUvStage[i] = -1;
    for (int i = 0; i < 16; ++i) p.texProducer[i] = -1;
    for (int i = 0; i < 32; ++i) p.layout.slot[i] = -1;

    const int nStages = k.numStages < 1 ? 1 : (k.numStages > 16 ? 16 : k.numStages);
    const int nInd = k.numIndStages > 4 ? 4 : k.numIndStages;
    const int nTg = k.numTexGens > 8 ? 8 : k.numTexGens;
    const bool fuse = k.silhFuse() != 0;
    const bool atFast = FsAtFastKey(k) && !fuse;
    const bool texReuse = FsTexReuseMode() != 0;
    const bool projRead = FsProjMode() != 0;
    static const bool indPrevOn = EnvInt("GXV_FIX_INDPREV", 0) != 0;
    for (int s = 0; indPrevOn && s < nStages; ++s)
        if (k.stage[s].ind && bits(k.stage[s].ind, 20, 1)) p.indPrev = true;

    std::fill(p.inWrapKeep, p.inWrapKeep + 16, static_cast<uint8_t>(0x3F));
    if ((k.inWrap & 1) && TevInCanonMode() == 2) {
        TevInWalk tw;
        WalkTevIn(k, 0xFu, true, false, tw);
        for (int s = 0; s < 16; ++s) { p.inWrapKeep[s] = tw.keep[s]; p.inWrapInit[s] = tw.keepInit[s]; }
    }
    p.dualRegs = (k.inWrap & 1) && TevInCanonMode() == 2 ? k.dualWrap() : static_cast<uint8_t>(0);

    for (int i = 0; i < nInd; ++i)
        if (k.indVs(i) && k.ind[i].texCoord < nTg) p.indVsTg |= static_cast<uint8_t>(1u << k.ind[i].texCoord);

    if (projRead) {
        p.projOnly = k.texProjMask;
        for (int s = 0; s < nStages; ++s) {
            const FsKey::Stage& st = k.stage[s];
            const bool usesCoord = st.texEnable || st.ind;
            if (usesCoord && (st.ind || st.lodBias)) p.projOnly &= static_cast<uint8_t>(~(1u << (st.texCoord & 7)));
        }
        for (int i = 0; i < nInd; ++i) p.projOnly &= static_cast<uint8_t>(~(1u << (k.ind[i].texCoord & 7)));
    }

    bool hasAddPrevAfter[16] = {};
    bool ap = false;
    for (int s = nStages - 1; s >= 0; --s) {
        hasAddPrevAfter[s] = ap;
        if (k.stage[s].ind && bits(k.stage[s].ind, 20, 1)) ap = true;
    }

    bool stageReadsTex[16] = {};
    for (int s = 0; s < nStages; ++s) {
        const uint32_t tc = k.stage[s].color, ta = k.stage[s].alpha;
        for (int i = 0; i < 4; ++i) {
            const uint32_t cs = bits(tc, i * 4, 4);
            if (cs == 8 || cs == 9) stageReadsTex[s] = true;
            if (bits(ta, 4 + i * 3, 3) == 4) stageReadsTex[s] = true;
        }
    }

    int curProd = -1, lastReadProd = -1;
    uint8_t lastMap = 0, lastCoord = 0, lastSwap = 0, lastBias = 0, lastProj = 0;
    for (int s = 0; s < nStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        if (st.texEnable) {
            const bool projHere = projRead && !st.lodBias && !st.ind && st.texCoord < nTg && ((k.texProjMask >> st.texCoord) & 1u) != 0;
            const bool reusable = !st.ind;
            if (texReuse && reusable && lastReadProd >= 0 && lastMap == st.texMap && lastCoord == st.texCoord && lastSwap == st.texSwap &&
                lastBias == st.lodBias && lastProj == (projHere ? 1 : 0)) {
                p.texReusedStage[s] = true;
                curProd = lastReadProd;
            } else {
                curProd = s;
                if (reusable) {
                    lastReadProd = s; lastMap = st.texMap; lastCoord = st.texCoord; lastSwap = st.texSwap;
                    lastBias = st.lodBias; lastProj = projHere ? 1 : 0;
                } else {
                    lastReadProd = -1;
                }
            }
        } else if (st.texOff) {
            curProd = -1;
            lastReadProd = -1;
        }
        p.texProducer[s] = curProd;
    }
    p.finalTexProducer = curProd;

    int prodUses[16] = {};
    int prodLast[16];
    std::fill(prodLast, prodLast + 16, -1);
    for (int s = 0; s < nStages; ++s) {
        if (stageReadsTex[s] && p.texProducer[s] >= 0) {
            ++prodUses[p.texProducer[s]];
            prodLast[p.texProducer[s]] = s;
        }
    }
    if (fuse && p.finalTexProducer >= 0) {
        ++prodUses[p.finalTexProducer];
        prodLast[p.finalTexProducer] = nStages;
    }
    for (int s = 0; s < nStages; ++s) {
        p.texEmit[s] = prodUses[s] > 0;
        p.texShared[s] = prodUses[s] > 0 && prodLast[s] > s;
        p.stageIndEmit[s] = k.stage[s].ind != 0 && (p.texEmit[s] || hasAddPrevAfter[s]);
        if (p.stageIndEmit[s]) {
            if (p.firstIndOpStage < 0) p.firstIndOpStage = s;
            const uint32_t bt = bits(k.stage[s].ind, 0, 2);
            if (bt < static_cast<uint32_t>(nInd) && p.firstIndStage[bt] < 0) p.firstIndStage[bt] = s;
        }
    }

    auto noteProjUv = [&](int c, int stg) {
        if (c < 0 || c >= nTg) return;
        if (!((k.texProjMask >> c) & 1u) || ((p.projOnly >> c) & 1u)) return;
        if (p.firstProjUvStage[c] < 0 || stg < p.firstProjUvStage[c]) p.firstProjUvStage[c] = stg;
    };
    for (int i = 0; i < nInd; ++i) {
        if (p.firstIndStage[i] >= 0 && !(((p.indVsTg >> k.ind[i].texCoord) & 1u) && k.indVs(i)))
            noteProjUv(k.ind[i].texCoord, p.firstIndStage[i]);
    }
    for (int s = 0; s < nStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        const bool projHere = projRead && st.texEnable && !st.lodBias && !st.ind && st.texCoord < nTg && ((k.texProjMask >> st.texCoord) & 1u) != 0;
        if (p.stageIndEmit[s] || (p.texEmit[s] && !st.ind && !projHere)) noteProjUv(st.texCoord, s);
    }

    bool dualUsed[4] = {};
    bool needRow[32] = {};
    const bool w = (k.inWrap & 1) != 0;
    static const uint8_t kCBit[4] = {0, 4, 2, 1};
    static const uint8_t kABit[4] = {0, 32, 16, 8};
    for (int s = 0; s < nStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        const uint32_t tc = st.color, ta = st.alpha;
        bool kcUsed = false, kaUsed = false;
        for (int i = 0; i < 4; ++i) {
            const uint32_t sel = bits(tc, i * 4, 4);
            if (sel == 14) kcUsed = true;
            if (sel < 8) {
                const int r = static_cast<int>(sel / 2);
                const bool isA = (sel & 1u) != 0;
                const uint8_t b = kCBit[i];
                const bool usesDual = w && b != 0 && (p.inWrapKeep[s] & b) != 0 && ((p.dualRegs >> r) & 1u) != 0 && (p.inWrapInit[s] & b) != 0;
                if (usesDual) {
                    dualUsed[r] = true;
                } else if (!isA) {
                    if (!p.rgbWritten[r]) p.rgbReadInit[r] = true;
                    if (p.firstRgbStage[r] < 0) p.firstRgbStage[r] = s;
                } else {
                    if (!p.aWritten[r]) p.aReadInit[r] = true;
                    if (p.firstAStage[r] < 0) p.firstAStage[r] = s;
                }
            }
            const uint32_t asel = bits(ta, 4 + i * 3, 3);
            if (asel == 6) kaUsed = true;
            if (asel < 4) {
                const int r = static_cast<int>(asel);
                const uint8_t b = kABit[i];
                const bool usesDual = w && b != 0 && (p.inWrapKeep[s] & b) != 0 && ((p.dualRegs >> r) & 1u) != 0 && (p.inWrapInit[s] & b) != 0;
                if (usesDual) {
                    dualUsed[r] = true;
                } else {
                    if (!p.aWritten[r]) p.aReadInit[r] = true;
                    if (p.firstAStage[r] < 0) p.firstAStage[r] = s;
                }
            }
        }
        if (kcUsed && st.kcsel >= 12) needRow[V_KONST + (st.kcsel & 3)] = true;
        if (kaUsed && st.kasel >= 16) needRow[V_KONST + (st.kasel & 3)] = true;
        const int dstC = static_cast<int>(bits(tc, 22, 2)), dstA = static_cast<int>(bits(ta, 22, 2));
        p.rgbWritten[dstC] = true;
        if (p.firstRgbStage[dstC] < 0) p.firstRgbStage[dstC] = s;
        p.aWritten[dstA] = true;
        if (p.firstAStage[dstA] < 0) p.firstAStage[dstA] = s;
    }

    for (int r = 0; r < 4; ++r) {
        const bool fromVtx = (r == 1 && (k.regFromVtx & 1)) || (r == 2 && (k.regFromVtx & 2));
        if (!fromVtx && (p.rgbReadInit[r] || p.aReadInit[r])) needRow[V_REG + r] = true;
        if (dualUsed[r]) needRow[28 + r] = true;
    }
    bool atPassKeepsMisc = false;
    if (k.atPass() && k.alphaTestOn && !FsAtFastKey(k)) {
        FsKey tested = k;
        tested.setAtPass(0);
        if (FsHasAlphaTestText(tested) && !FsAtFastKey(tested)) atPassKeepsMisc = true;
    }
    if (k.dstAlpha || (FsHasAlphaTestText(k) && !atFast) || atPassKeepsMisc) needRow[V_MISC] = true;
    if ((k.fog && !k.colorDead() && !(k.fog & kFogHalfBit)) || atFast) needRow[V_FOG] = true;
    if (k.fog && !k.colorDead()) {
        needRow[V_FOGCOL] = true;
        if (k.fog & kFogHalfBit) needRow[V_FOGHL] = true;
    }
    for (int s = 0; s < nStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        if (p.stageIndEmit[s]) {
            const uint32_t ind = st.ind;
            const uint32_t fmt = bits(ind, 2, 2), mid = bits(ind, 9, 4), sw = bits(ind, 13, 3), tw = bits(ind, 16, 3), addPrev = bits(ind, 20, 1);
            const bool isPre = (k.indPre() == s + 1 && fmt == 0 && mid >= 1 && mid <= 3 && sw == 0 && tw == 0 && !addPrev && !p.indPrev);
            if (isPre) {
                needRow[V_INDMTX + (mid - 1) * 2] = true;
                needRow[V_INDMTX + (mid - 1) * 2 + 1] = true;
                needRow[V_INDPRE] = true;
            } else {
                needRow[V_TEXSIZE + (st.texMap & 7) / 2] = true;
                if (mid >= 1 && mid <= 3) {
                    needRow[V_INDMTX + (mid - 1) * 2] = true;
                    needRow[V_INDMTX + (mid - 1) * 2 + 1] = true;
                } else if (mid >= 5 && mid <= 7) {
                    needRow[V_INDMTX + (mid - 5) * 2] = true;
                } else if (mid >= 9 && mid <= 11) {
                    needRow[V_INDMTX + (mid - 9) * 2] = true;
                }
            }
        }
        if (p.texEmit[s] && st.texEnable && st.lodBias) needRow[V_LODBIAS + (st.texMap & 7) / 4] = true;
    }
    for (int i = 0; i < nInd; ++i)
        if (p.firstIndStage[i] >= 0 && !(((p.indVsTg >> k.ind[i].texCoord) & 1u) && k.indVs(i))) needRow[V_INDSCALE + i] = true;

    uint8_t cnt = 0;
    for (int r = 0; r < 32; ++r) {
        if (needRow[r]) {
            p.layout.slot[r] = static_cast<int8_t>(cnt);
            p.layout.src[cnt] = static_cast<uint8_t>(r);
            ++cnt;
        }
    }
    if (cnt == 0) {
        p.layout.slot[0] = 0;
        p.layout.src[0] = 0;
        cnt = 1;
    }
    p.layout.count = cnt;
}

// GXV_FS_REGS=1 fragment shader generator (Fix B): exact arithmetic and precision as the default path,
// with compacted u_tev[] rows (gathered from canonical UB_TEV in the back end via ShaderSource::tevSrc[]),
// elided read-only/unwritten register temporaries, and sunk uv / ind / tex / ras / kc / ka expressions.
bool GenerateFragmentShaderRegs(const FsKey& k, ShaderSource& src, bool hf, unsigned parts) {
    const bool fuse = k.silhFuse() != 0;
    if (fuse) { hf = false; parts = 0; }
    Out o;
    FsRegPlan p;
    BuildFsRegPlan(k, p);
    const FsRegsLayout& L = p.layout;
    bool missingSlot = false;
    auto S = [&](int row) -> int {
        const int sl = (row >= 0 && row < 32) ? L.slot[row] : -1;
        if (sl < 0) { missingSlot = true; return 0; }
        return sl;
    };

    FsTrimInfo ti;
    bool hasCompare = false;
    const int nStages = k.numStages < 1 ? 1 : (k.numStages > 16 ? 16 : k.numStages);
    for (int s = 0; s < nStages; ++s)
        if (bits(k.stage[s].color, 16, 2) == 3 || bits(k.stage[s].alpha, 16, 2) == 3) hasCompare = true;
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
    unsigned trimOps = 0;
    bool loC[16] = {}, hiC[16] = {}, loA[16] = {}, hiA[16] = {};
    const char* const T1 = hf ? "hfloat" : "float";
    const char* const T3 = hf ? "hvec3" : "vec3";
    const char* const T4 = hf ? "hvec4" : "vec4";
    auto Lit = [&](const char* lit) -> std::string { return hf ? std::string("hfloat(") + lit + ")" : std::string(lit); };
    const FsKey::Stage& last = k.stage[nStages - 1];
    bool wrapColor = false, wrapAlpha = false;
    FinalWrapPlanTrim(k, trimWrap ? &ti : nullptr, wrapColor, wrapAlpha);
    if (trim) {
        bool fullColor = false, fullAlpha = false;
        FinalWrapPlan(k, fullColor, fullAlpha);
        trimOps += static_cast<unsigned>(fullColor && !wrapColor) * 3u + static_cast<unsigned>(fullAlpha && !wrapAlpha);
    }
    const bool projRead = FsProjMode() != 0;
    uint8_t texMask = 0;
    bool usesCol[2] = {false, false};
    for (int s = 0; s < nStages; ++s) {
        if (k.stage[s].texEnable) texMask |= static_cast<uint8_t>(1u << k.stage[s].texMap);
        if (k.stage[s].rasChan < 2) usesCol[k.stage[s].rasChan] = true;
    }
    for (int i = 0; i < k.numIndStages; ++i) texMask |= static_cast<uint8_t>(1u << k.ind[i].texMap);

    o("#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n");
    if (hf) {
        o("#ifdef VITAGL\n#define hfloat half\n#define hvec3 half3\n#define hvec4 half4\n#else\n"
          "#define hfloat float\n#define hvec3 vec3\n#define hvec4 vec4\n#endif\n");
    }
    if (wrapColor || wrapAlpha) {
        o("float vgx_wrap_final(float value) {\n"
          "  if (value >= 0.0 && value <= 1.0) return value;\n"
          "  float byteValue = floor(value * 255.0 + 0.5);\n"
          "  return (byteValue - 256.0 * floor(byteValue / 256.0)) / 255.0;\n}\n");
    }
    if (k.inWrap & 1) {
        o("float vgx_w1(float value) {\n"
          "  if (value >= 0.0 && value <= 1.0) return value;\n"
          "  float byteValue = floor(value * 255.0 + 0.5);\n"
          "  return (byteValue - 256.0 * floor(byteValue / 256.0)) / 255.0;\n}\n"
          "vec3 vgx_w3(vec3 value) { return vec3(vgx_w1(value.x), vgx_w1(value.y), vgx_w1(value.z)); }\n");
    }
    for (int m = 0; m < 8; ++m)
        if (texMask & (1u << m)) o("uniform sampler2D u_tex%d;\n", m);
    o("uniform %s u_tev[%d];\n", T4, static_cast<int>(L.count));
    if (usesCol[0] || (k.regFromVtx & 1)) o("varying %s v_col0;\n", T4);
    if (usesCol[1] || (k.regFromVtx & 2)) o("varying %s v_col1;\n", T4);
    auto texV2 = [&](int i) { return (k.texVec2() && !(k.texProjMask & (1u << i))) || ((p.indVsTg >> i) & 1u); };
    for (int i = 0; i < k.numTexGens; ++i) o(texV2(i) ? "varying vec2 v_tex%d;\n" : "varying vec3 v_tex%d;\n", i);
    if (k.fog & kFogVtxBit) {
        o("varying float %s;\n", FogVaryingName());
        NoteFogSlotCollision(false, k.numTexGens, usesCol[0] || (k.regFromVtx & 1), usesCol[1] || (k.regFromVtx & 2));
    }
    o("void main() {\n");

    auto InitRegSrc = [&](int r) -> std::string {
        if (r == 1 && (k.regFromVtx & 1)) return "v_col0";
        if (r == 2 && (k.regFromVtx & 2)) return "v_col1";
        return "u_tev[" + std::to_string(S(V_REG + r)) + "]";
    };
    auto RegRgbExpr = [&](int r) -> std::string {
        if (p.rgbWritten[r]) return std::string(RegName(static_cast<uint32_t>(r))) + "_rgb";
        return InitRegSrc(r) + ".rgb";
    };
    auto RegAExpr = [&](int r) -> std::string {
        if (p.aWritten[r]) return std::string(RegName(static_cast<uint32_t>(r))) + "_a";
        return InitRegSrc(r) + ".a";
    };
    auto RegAaaExpr = [&](int r) -> std::string {
        if (p.aWritten[r]) return std::string(T3) + "(" + RegName(static_cast<uint32_t>(r)) + "_a)";
        return InitRegSrc(r) + ".aaa";
    };
    auto UvExpr = [&](int c) -> std::string {
        if (c >= k.numTexGens) return "vec2(0.0)";
        if ((k.texProjMask >> c) & 1u) return "uv" + std::to_string(c);
        return texV2(c) ? ("v_tex" + std::to_string(c)) : ("v_tex" + std::to_string(c) + ".xy");
    };
    auto KonstColPacked = [&](uint8_t sel) -> std::string {
        char b[64];
        if (sel < 8) { std::snprintf(b, sizeof b, "%s(%.3f)", T3, (8 - sel) / 8.0); return b; }
        if (sel < 12) return std::string(T3) + "(0.0)";
        const int kk = sel & 3;
        static const char* const comp[5] = {"rgb", "rrr", "ggg", "bbb", "aaa"};
        std::snprintf(b, sizeof b, "u_tev[%d].%s", S(V_KONST + kk), comp[(sel - 12) >> 2]);
        return b;
    };
    auto KonstAlpPacked = [&](uint8_t sel) -> std::string {
        char b[64];
        if (sel < 8) { std::snprintf(b, sizeof b, hf ? "hfloat(%.3f)" : "%.3f", (8 - sel) / 8.0); return b; }
        if (sel < 16) return Lit("0.0");
        const int kk = sel & 3;
        static const char* const comp[4] = {"r", "g", "b", "a"};
        std::snprintf(b, sizeof b, "u_tev[%d].%s", S(V_KONST + kk), comp[(sel - 16) >> 2]);
        return b;
    };

    const bool atFast = FsAtFastKey(k) && !fuse;
    const bool atEarly = atFast && FsAtEarlyKey(k) && !trimAlpha && !wrapAlpha;
    bool atDone = false;
    auto AtFastTest = [&](bool late) {
        const char* av = late ? "fin.a" : "ra";
        const int kind = AtFastKind(k.alphaTest);
        const int thrRow = S(V_ATTHR);
        char pass[160];
        if (kind == 1) std::snprintf(pass, sizeof pass, "(%s >= u_tev[%d].x)", av, thrRow);
        else if (kind == 2) std::snprintf(pass, sizeof pass, "(%s < u_tev[%d].y)", av, thrRow);
        else std::snprintf(pass, sizeof pass, "(abs(%s - u_tev[%d].x) < u_tev[%d].y)", av, thrRow, thrRow);
        if (k.rejectZero() != 0) o("%s  bool vgx_ok = %s;\n", late ? "" : "  ", pass);
        else o("%s  if (!%s) discard;\n", late ? "" : "  ", pass);
        atDone = true;
    };

    unsigned texReused = 0;
    for (int s = 0; s < nStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        // Sink projective uvN right before its first consumer stage.
        for (int c = 0; c < k.numTexGens && c < 8; ++c) {
            if (p.firstProjUvStage[c] == s)
                o("  vec2 uv%d = v_tex%d.xy / (v_tex%d.z == 0.0 ? 1.0 : v_tex%d.z);\n", c, c, c, c);
        }
        // Sink ind<i> lookups right before their first consumer stage.
        for (int i = 0; i < k.numIndStages && i < 4; ++i) {
            if (p.firstIndStage[i] == s) {
                if (((p.indVsTg >> k.ind[i].texCoord) & 1u) && k.indVs(i))
                    o("  vec3 ind%d = texture2D(u_tex%d, v_tex%d).abg * 255.0;\n", i, k.ind[i].texMap, k.ind[i].texCoord);
                else
                    o("  vec3 ind%d = texture2D(u_tex%d, %s * u_tev[%d].xy).abg * 255.0;\n", i, k.ind[i].texMap, UvExpr(k.ind[i].texCoord).c_str(),
                      S(V_INDSCALE + i));
            }
        }
        if (p.firstIndOpStage == s)
            o(p.indPrev ? "  vec2 itc = vec2(0.0), itx = vec2(0.0);\n" : "  vec2 itc = vec2(0.0);\n");
        // Sink mutable TEV register parts right before their first use or write.
        for (int r = 0; r < 4; ++r) {
            if (p.rgbWritten[r] && p.firstRgbStage[r] == s) {
                if (p.rgbReadInit[r]) o("  %s %s_rgb = %s.rgb;\n", T3, RegName(static_cast<uint32_t>(r)), InitRegSrc(r).c_str());
                else o("  %s %s_rgb;\n", T3, RegName(static_cast<uint32_t>(r)));
            }
            if (p.aWritten[r] && p.firstAStage[r] == s) {
                if (p.aReadInit[r]) o("  %s %s_a = %s.a;\n", T1, RegName(static_cast<uint32_t>(r)), InitRegSrc(r).c_str());
                else o("  %s %s_a;\n", T1, RegName(static_cast<uint32_t>(r)));
            }
        }
        o("  // stage %d\n", s);
        std::string coord = UvExpr(st.texCoord);
        if (p.stageIndEmit[s]) {
            const uint32_t ind = st.ind;
            const uint32_t bt = bits(ind, 0, 2), fmt = bits(ind, 2, 2), bias = bits(ind, 4, 3), mid = bits(ind, 9, 4);
            const uint32_t sw = bits(ind, 13, 3), tw = bits(ind, 16, 3), addPrev = bits(ind, 20, 1);
            const int map = st.texMap;
            if (k.indPre() == s + 1 && fmt == 0 && mid >= 1 && mid <= 3 && sw == 0 && tw == 0 && !addPrev && !p.indPrev) {
                o("  {\n");
                if (bt < k.numIndStages) o("    vec3 iv = ind%u;\n", bt);
                else o("    vec3 iv = vec3(0.0);\n");
                o("    vec2 io = vec2(dot(u_tev[%d].xyz, iv), dot(u_tev[%d].xyz, iv));\n", S(V_INDMTX + (mid - 1) * 2), S(V_INDMTX + (mid - 1) * 2 + 1));
                o("    itc = %s + io * u_tev[%d].xy + u_tev[%d].zw;\n  }\n", coord.c_str(), S(V_INDPRE), S(V_INDPRE));
                coord = "itc";
            } else {
                o("  {\n    vec2 tsz = u_tev[%d].%s;\n", S(V_TEXSIZE + map / 2), (map & 1) ? "zw" : "xy");
                o("    vec2 tcx = %s * tsz;\n", coord.c_str());
                if (bt < k.numIndStages) o("    vec3 iv = ind%u;\n", bt);
                else o("    vec3 iv = vec3(0.0);\n");
                static const float kMod[4] = {256.0f, 32.0f, 16.0f, 8.0f};
                if (fmt) o("    iv = iv - %.1f * floor(iv / %.1f);\n", kMod[fmt], kMod[fmt]);
                const float bv = fmt == 0 ? -128.0f : 1.0f;
                o("    iv += vec3(%.1f, %.1f, %.1f);\n", (bias & 1) ? bv : 0.0f, (bias & 2) ? bv : 0.0f, (bias & 4) ? bv : 0.0f);
                if (mid == 0) o("    vec2 io = vec2(0.0);\n");
                else if (mid <= 3) o("    vec2 io = vec2(dot(u_tev[%d].xyz, iv), dot(u_tev[%d].xyz, iv));\n", S(V_INDMTX + (mid - 1) * 2), S(V_INDMTX + (mid - 1) * 2 + 1));
                else if (mid >= 5 && mid <= 7) o("    vec2 io = tcx * (iv.x / 256.0) * u_tev[%d].w;\n", S(V_INDMTX + (mid - 5) * 2));
                else if (mid >= 9 && mid <= 11) o("    vec2 io = tcx * (iv.y / 256.0) * u_tev[%d].w;\n", S(V_INDMTX + (mid - 9) * 2));
                else o("    vec2 io = vec2(0.0);\n");
                static const float kWrap[8] = {0, 256, 128, 64, 32, 16, 0, 0};
                if (sw == 6) o("    tcx.x = 0.0;\n");
                else if (sw) o("    tcx.x = tcx.x - %.1f * floor(tcx.x / %.1f);\n", kWrap[sw], kWrap[sw]);
                if (tw == 6) o("    tcx.y = 0.0;\n");
                else if (tw) o("    tcx.y = tcx.y - %.1f * floor(tcx.y / %.1f);\n", kWrap[tw], kWrap[tw]);
                if (p.indPrev) {
                    o(addPrev ? "    itx = tcx + io + itx;\n" : "    itx = tcx + io;\n");
                    o("    itc = itx / tsz;\n  }\n");
                } else {
                    if (addPrev) o("    itc = tcx + io + itc;\n");
                    else o("    itc = tcx + io;\n");
                    o("    itc = itc / tsz;\n  }\n");
                }
                coord = "itc";
            }
        }
        auto TexSampleExpr = [&]() -> std::string {
            const bool projHere = projRead && st.texEnable && !st.lodBias && !st.ind && st.texCoord < k.numTexGens &&
                                  ((k.texProjMask >> st.texCoord) & 1u) != 0;
            char buf[256];
            const std::string swz = Swizzle(st.texSwap);
            if (projHere) {
                if (hf) std::snprintf(buf, sizeof buf, "hvec4(texture2DProj(u_tex%d, v_tex%d).%s)", st.texMap, st.texCoord, swz.c_str());
                else std::snprintf(buf, sizeof buf, "texture2DProj(u_tex%d, v_tex%d).%s", st.texMap, st.texCoord, swz.c_str());
            } else if (hf) {
                if (st.lodBias)
                    std::snprintf(buf, sizeof buf, "hvec4(texture2D(u_tex%d, %s, float(u_tev[%d].%c)).%s)", st.texMap, coord.c_str(),
                                  S(V_LODBIAS + st.texMap / 4), "xyzw"[st.texMap & 3], swz.c_str());
                else
                    std::snprintf(buf, sizeof buf, "hvec4(texture2D(u_tex%d, %s).%s)", st.texMap, coord.c_str(), swz.c_str());
            } else if (st.lodBias) {
                std::snprintf(buf, sizeof buf, "texture2D(u_tex%d, %s, u_tev[%d].%c).%s", st.texMap, coord.c_str(),
                              S(V_LODBIAS + st.texMap / 4), "xyzw"[st.texMap & 3], swz.c_str());
            } else {
                std::snprintf(buf, sizeof buf, "texture2D(u_tex%d, %s).%s", st.texMap, coord.c_str(), swz.c_str());
            }
            return buf;
        };
        if (p.texReusedStage[s] && p.texProducer[s] >= 0 && p.texEmit[p.texProducer[s]]) ++texReused;
        if (p.texEmit[s] && p.texShared[s]) o("  %s tex_%d = %s;\n", T4, s, TexSampleExpr().c_str());

        o("  {\n");
        std::string texVar;
        if (p.texProducer[s] == s && p.texEmit[s] && !p.texShared[s]) {
            o("    %s tex = %s;\n", T4, TexSampleExpr().c_str());
            texVar = "tex";
        } else if (p.texProducer[s] >= 0 && p.texEmit[p.texProducer[s]]) {
            texVar = "tex_" + std::to_string(p.texProducer[s]);
        }

        const uint32_t tc = st.color, ta = st.alpha;
        bool kcUsed = false, kaUsed = false, rasRgbUsed = false, rasAUsed = false;
        for (int i = 0; i < 4; ++i) {
            const uint32_t cs = bits(tc, i * 4, 4);
            if (cs == 14) kcUsed = true;
            if (cs == 10) rasRgbUsed = true;
            if (cs == 11) rasAUsed = true;
            const uint32_t as = bits(ta, 4 + i * 3, 3);
            if (as == 6) kaUsed = true;
            if (as == 5) rasAUsed = true;
        }
        if (st.rasChan < 2 && (rasRgbUsed || rasAUsed)) {
            const std::string rsw = Swizzle(st.rasSwap);
            if (rasRgbUsed && rasAUsed) o("    %s ras = v_col%d.%s;\n", T4, st.rasChan, rsw.c_str());
            else if (rasRgbUsed) o("    %s ras_rgb = v_col%d.%s;\n", T3, st.rasChan, rsw.substr(0, 3).c_str());
            else o("    %s ras_a = v_col%d.%c;\n", T1, st.rasChan, rsw[3]);
        }
        if (kcUsed) o("    %s kc = %s;\n", T3, KonstColPacked(st.kcsel).c_str());
        if (kaUsed) o("    %s ka = %s;\n", T1, KonstAlpPacked(st.kasel).c_str());

        auto CArg = [&](uint32_t sel) -> std::string {
            switch (sel & 15u) {
            case 0: case 2: case 4: case 6: return RegRgbExpr(static_cast<int>(sel / 2));
            case 1: case 3: case 5: case 7: return RegAaaExpr(static_cast<int>(sel / 2));
            case 8: return texVar.empty() ? (std::string(T3) + "(1.0)") : (texVar + ".rgb");
            case 9: return texVar.empty() ? (std::string(T3) + "(1.0)") : (texVar + ".aaa");
            case 10: return st.rasChan < 2 ? (rasAUsed ? "ras.rgb" : "ras_rgb") : (std::string(T3) + "(0.0)");
            case 11: return st.rasChan < 2 ? (rasRgbUsed ? "ras.aaa" : (std::string(T3) + "(ras_a)")) : (std::string(T3) + "(0.0)");
            case 12: return std::string(T3) + "(1.0)";
            case 13: return std::string(T3) + "(0.5)";
            case 14: return "kc";
            default: return std::string(T3) + "(0.0)";
            }
        };
        auto AArg = [&](uint32_t sel) -> std::string {
            switch (sel & 7u) {
            case 0: case 1: case 2: case 3: return RegAExpr(static_cast<int>(sel));
            case 4: return texVar.empty() ? Lit("1.0") : (texVar + ".a");
            case 5: return st.rasChan < 2 ? (rasRgbUsed ? "ras.a" : "ras_a") : Lit("0.0");
            case 6: return "ka";
            default: return Lit("0.0");
            }
        };
        const bool w = (k.inWrap & 1) != 0;
        auto kw = [&](uint8_t bit) { return (p.inWrapKeep[s] & bit) != 0; };
        auto ki = [&](uint8_t bit, uint32_t reg) { return ((p.dualRegs >> reg) & 1u) != 0 && (p.inWrapInit[s] & bit) != 0; };
        auto cIn = [&](uint32_t sel, uint8_t bit) -> std::string {
            if (w && sel < 8 && kw(bit)) {
                const uint32_t reg = sel / 2;
                if (ki(bit, reg)) return "u_tev[" + std::to_string(S(28 + static_cast<int>(reg))) + "]" + ((sel & 1u) ? ".aaa" : ".rgb");
                return WrapArg(CArg(sel), true, true);
            }
            return CArg(sel);
        };
        auto aIn = [&](uint32_t sel, uint8_t bit) -> std::string {
            if (w && sel < 4 && kw(bit)) {
                if (ki(bit, sel)) return "u_tev[" + std::to_string(S(28 + static_cast<int>(sel))) + "].a";
                return WrapArg(AArg(sel), true, false);
            }
            return AArg(sel);
        };

        const bool cmpC = bits(tc, 16, 2) == 3, cmpA = bits(ta, 16, 2) == 3;
        const std::string cA = cIn(bits(tc, 12, 4), 1), cB = cIn(bits(tc, 8, 4), 2), cC = cIn(bits(tc, 4, 4), 4), cD = CArg(bits(tc, 0, 4));
        const std::string aA = aIn(bits(ta, 13, 3), 8), aB = aIn(bits(ta, 10, 3), 16), aC = aIn(bits(ta, 7, 3), 32), aD = AArg(bits(ta, 4, 3));
        const bool needColAB = cmpC || (cmpA && bits(ta, 20, 2) < 3);
        const bool needAlpAB = cmpA && bits(ta, 20, 2) == 3;
        if (needColAB) o("    %s a = %s, b = %s;\n", T3, cA.c_str(), cB.c_str());
        if (needAlpAB) o("    %s aa = %s, ab = %s;\n", T1, aA.c_str(), aB.c_str());

        const std::string kBias[3] = {"", " + " + Lit("0.5"), " - " + Lit("0.5")};
        const std::string kScale[4] = {"", " * " + Lit("2.0"), " * " + Lit("4.0"), " * " + Lit("0.5")};
        auto Combine = [&](uint32_t reg, bool color) {
            const uint32_t bias = bits(reg, 16, 2), op = bits(reg, 18, 1), clampOn = bits(reg, 19, 1), scale = bits(reg, 20, 2);
            const char* res = color ? "rc" : "ra";
            const char* type = color ? T3 : T1;
            const std::string& A = color ? cA : aA;
            const std::string& B = color ? cB : aB;
            const std::string& C = color ? cC : aC;
            const std::string& D = color ? cD : aD;
            if (bias != 3) {
                o("    %s %s = (%s %s mix(%s, %s, %s)%s)%s;\n", type, res, D.c_str(), op ? "-" : "+", A.c_str(), B.c_str(), C.c_str(),
                  kBias[bias].c_str(), kScale[scale].c_str());
            } else {
                const bool eq = op != 0;
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
                        if (eq) std::snprintf(buf, sizeof buf, "(vec3(1.0) - step(vec3(%.6f), abs(a - b)))", e * 2);
                        else std::snprintf(buf, sizeof buf, "(vec3(1.0) - step(a, b + vec3(%.6f)))", e);
                    } else {
                        if (eq) std::snprintf(buf, sizeof buf, "(abs(aa - ab) < %.6f)", e * 2);
                        else std::snprintf(buf, sizeof buf, "(aa > ab + %.6f)", e);
                    }
                    break;
                }
                if (scale == 3 && color) o("    vec3 rc = %s + %s * %s;\n", D.c_str(), C.c_str(), buf);
                else o("    %s %s = %s + (%s ? %s : %s(0.0));\n", type, res, D.c_str(), buf, C.c_str(), type);
            }
            const FsTrimClamp act = trim ? (color ? ti.c[s] : ti.a[s]) : FsTrimClamp{};
            if (trim && !(act.lo && act.hi)) {
                trimOps += (act.lo ? 0u : 1u) + (act.hi ? 0u : 1u);
                if (act.lo) o("    %s = max(%s, %s(%s));\n", res, res, type, clampOn ? "0.0" : "-4.0157");
                else if (act.hi) o("    %s = min(%s, %s(%s));\n", res, res, type, clampOn ? "1.0" : "4.0118");
                if (clampOn && act.lo) (color ? loC : loA)[s] = true;
                else if (clampOn && act.hi) (color ? hiC : hiA)[s] = true;
            } else if (clampOn) {
                o("    %s = clamp(%s, %s, %s);\n", res, res, Lit("0.0").c_str(), Lit("1.0").c_str());
                (color ? loC : loA)[s] = true;
                (color ? hiC : hiA)[s] = true;
            } else {
                o("    %s = clamp(%s, %s, %s);\n", res, res, Lit("-4.0157").c_str(), Lit("4.0118").c_str());
            }
        };
        if (atEarly && s == nStages - 1) {
            Combine(ta, false);
            AtFastTest(false);
            Combine(tc, true);
        } else {
            Combine(tc, true);
            Combine(ta, false);
        }
        o("    %s_rgb = rc;\n    %s_a = ra;\n  }\n", RegName(bits(tc, 22, 2)), RegName(bits(ta, 22, 2)));
    }

    const std::string finR = RegRgbExpr(static_cast<int>(bits(last.color, 22, 2)));
    const std::string finA = RegAExpr(static_cast<int>(bits(last.alpha, 22, 2)));
    const bool finLo = !trimFinal || (!wrapColor && ti.finC.lo) || (!wrapAlpha && ti.finA.lo);
    const bool finHi = !trimFinal || (!wrapColor && ti.finC.hi) || (!wrapAlpha && ti.finA.hi);
    if (trim) trimOps += (finLo ? 0u : 1u) + (finHi ? 0u : 1u);
    const bool alphaTestedText = k.alphaTestOn && !(NoDiscard() && AlphaTestAlwaysPasses(k.alphaTest & 7, -1, (k.alphaTest >> 6) & 3, (k.alphaTest >> 3) & 7, -1));
    const int lastS = nStages - 1;
    const bool deadA = (k.alphaDead() && !alphaTestedText) || (k.dstAlpha && !alphaTestedText);
    const bool doneCLo = !wrapColor && loC[lastS], doneCHi = !wrapColor && hiC[lastS];
    const bool doneALo = deadA || (!wrapAlpha && loA[lastS]), doneAHi = deadA || (!wrapAlpha && hiA[lastS]);
    if (FsFinClampMode() && (doneCLo || doneCHi || doneALo || doneAHi)) {
        bool cLo = !trimFinal || (!wrapColor && ti.finC.lo), cHi = !trimFinal || (!wrapColor && ti.finC.hi);
        bool aLo = !trimFinal || (!wrapAlpha && ti.finA.lo), aHi = !trimFinal || (!wrapAlpha && ti.finA.hi);
        if (doneCLo) cLo = false;
        if (doneCHi) cHi = false;
        if (doneALo) aLo = false;
        if (doneAHi) aHi = false;
        o("  %s fin = %s(%s, %s);\n", (wrapColor || wrapAlpha) ? "vec4" : T4, (wrapColor || wrapAlpha) ? "vec4" : T4, finR.c_str(), finA.c_str());
        if (wrapColor) o("  fin.rgb = vec3(vgx_wrap_final(fin.r), vgx_wrap_final(fin.g), vgx_wrap_final(fin.b));\n");
        if (wrapAlpha) o("  fin.a = vgx_wrap_final(fin.a);\n");
        const bool wv = wrapColor || wrapAlpha;
        const std::string z = wv ? "0.0" : Lit("0.0"), one = wv ? "1.0" : Lit("1.0");
        const char* t3 = wv ? "vec3" : T3;
        const char* t1 = wv ? "float" : T1;
        if (cLo && cHi) o("  fin.rgb = clamp(fin.rgb, %s, %s);\n", z.c_str(), one.c_str());
        else if (cLo) o("  fin.rgb = max(fin.rgb, %s(0.0));\n", t3);
        else if (cHi) o("  fin.rgb = min(fin.rgb, %s(1.0));\n", t3);
        if (aLo && aHi) o("  fin.a = clamp(fin.a, %s, %s);\n", z.c_str(), one.c_str());
        else if (aLo) o("  fin.a = max(fin.a, %s(0.0));\n", t1);
        else if (aHi) o("  fin.a = min(fin.a, %s(1.0));\n", t1);
    } else if (wrapColor || wrapAlpha) {
        o("  vec4 fin = vec4(%s, %s);\n", finR.c_str(), finA.c_str());
        if (wrapColor) o("  fin.rgb = vec3(vgx_wrap_final(fin.r), vgx_wrap_final(fin.g), vgx_wrap_final(fin.b));\n");
        if (wrapAlpha) o("  fin.a = vgx_wrap_final(fin.a);\n");
        if (finLo && finHi) o("  fin = clamp(fin, 0.0, 1.0);\n");
        else if (finLo) o("  fin = max(fin, vec4(0.0));\n");
        else if (finHi) o("  fin = min(fin, vec4(1.0));\n");
    } else if (finLo && finHi) {
        o("  %s fin = clamp(%s(%s, %s), %s, %s);\n", T4, T4, finR.c_str(), finA.c_str(), Lit("0.0").c_str(), Lit("1.0").c_str());
    } else if (finLo || finHi) {
        o("  %s fin = %s(%s(%s, %s), %s(%s));\n", T4, finLo ? "max" : "min", T4, finR.c_str(), finA.c_str(), T4, finLo ? "0.0" : "1.0");
    } else {
        o("  %s fin = %s(%s, %s);\n", T4, T4, finR.c_str(), finA.c_str());
    }

    bool rejectZero = false, preOnly = false, okDeclared = false;
    if (atFast && !atDone) {
        AtFastTest(true);
        rejectZero = k.rejectZero() != 0;
        okDeclared = rejectZero;
        preOnly = k.preAlpha() != 0 && !rejectZero;
    }
    if (!atDone && k.alphaTestOn && !k.atPass() && !(NoDiscard() && AlphaTestAlwaysPasses(k.alphaTest & 7, -1, (k.alphaTest >> 6) & 3, (k.alphaTest >> 3) & 7, -1))) {
        rejectZero = k.rejectZero() != 0 || fuse;
        char b0[96], b1[96], r0[32], r1[32];
        const int mr = S(V_MISC);
        std::snprintf(r0, sizeof r0, hf ? "float(u_tev[%d].x)" : "u_tev[%d].x", mr);
        std::snprintf(r1, sizeof r1, hf ? "float(u_tev[%d].y)" : "u_tev[%d].y", mr);
        const char* e0 = CompareExpr(k.alphaTest & 7, r0, b0, sizeof b0);
        const char* e1 = CompareExpr((k.alphaTest >> 3) & 7, r1, b1, sizeof b1);
        static const char* const kOp[4] = {"&&", "||", "!=", "=="};
        if (trimFloor) {
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
        if (rejectZero) { o("  bool vgx_ok = %s %s %s;\n", e0, kOp[(k.alphaTest >> 6) & 3], e1); okDeclared = true; }
        else o("  if (!(%s %s %s)) discard;\n", e0, kOp[(k.alphaTest >> 6) & 3], e1);
        preOnly = k.preAlpha() != 0 && !rejectZero;
    }
    if (fuse && !okDeclared) o("  bool vgx_ok = true;\n");
    if (preOnly) {
        o("  gl_FragColor = vec4(0.0);\n}\n");
    } else {
        bool fogMix = false;
        if (k.colorDead()) {
            o(hf ? "  fin.rgb = hvec3(0.0);\n" : "  fin.rgb = vec3(0.0);\n");
        } else if (k.fog) {
            const int fsel = k.fog & 7;
            if (k.fog & kFogHalfBit) {
                const int hl = S(V_FOGHL), fc = S(V_FOGCOL);
                o("  float fg = clamp((%s * float(u_tev[%d].x) - (float(u_tev[%d].z) + float(u_tev[%d].w))) * float(u_tev[%d].y) + float(u_tev[%d].w), 0.0, 1.0);\n",
                  FogVaryingName(), hl, hl, hl, hl, fc);
                fogMix = true;
            } else if (k.fog & kFogVtxBit) {
                const int fr = S(V_FOG);
                o("  float fg = clamp(u_tev[%d].x * %s + u_tev[%d].w, 0.0, 1.0);\n", fr, FogVaryingName(), fr);
            } else {
                const int fr = S(V_FOG);
                if (k.fog & 8) o("  float fg = u_tev[%d].x * gl_FragCoord.z;\n", fr);
                else o("  float fg = u_tev[%d].x / max(u_tev[%d].y - gl_FragCoord.z, 0.000001);\n", fr, fr);
                o("  fg = clamp(fg - u_tev[%d].z, 0.0, 1.0);\n", fr);
            }
            switch (fsel) {
            case 4: o("  fg = 1.0 - exp2(-8.0 * fg);\n"); break;
            case 5: o("  fg = 1.0 - exp2(-8.0 * fg * fg);\n"); break;
            case 6: o("  fg = exp2(-8.0 * (1.0 - fg));\n"); break;
            case 7: o("  fg = 1.0 - fg; fg = exp2(-8.0 * fg * fg);\n"); break;
            default: break;
            }
            if (!fogMix) o("  fin.rgb = mix(fin.rgb, u_tev[%d].rgb, fg);\n", S(V_FOGCOL));
        }
        if (k.dstAlpha) o("  fin.a = u_tev[%d].z;\n", S(V_MISC));
        else if (k.alphaDead() && !alphaTestedText) o(hf ? "  fin.a = hfloat(0.0);\n" : "  fin.a = 0.0;\n");
        if (k.logicSrc == 1) o("  fin.rgb = vec3(1.0) - fin.rgb;\n");
        else if (k.logicSrc == 2) o("  fin.rgb = vec3(1.0);\n");
        if (fuse) {
            const uint8_t code = k.silhFuse();
            const uint8_t sK = vgx::silhfuse::CodeS(code), dK = vgx::silhfuse::CodeD(code);
            auto factor = [](uint8_t kind) -> const char* {
                switch (kind) {
                case vgx::silhfuse::K_SA: return "vec3(vgx_a)";
                case vgx::silhfuse::K_ISA: return "vec3(255.0 - vgx_a)";
                case vgx::silhfuse::K_SC: return "vgx_s";
                default: return "(vec3(255.0) - vgx_s)";
                }
            };
            auto term = [&](const char* name, const char* x, uint8_t kind) {
                if (kind == vgx::silhfuse::K_ZERO) o("  vec3 %s = vec3(0.0);\n", name);
                else if (kind == vgx::silhfuse::K_ONE) o("  vec3 %s = %s;\n", name, x);
                else o("  vec3 %s = floor((%s * %s + vec3(127.0)) * 0.00392156862745098 + vec3(0.001));\n", name, x, factor(kind));
            };
            const std::string fTex = (p.finalTexProducer >= 0 && p.texEmit[p.finalTexProducer]) ? ("tex_" + std::to_string(p.finalTexProducer)) : std::string("vec4(1.0)");
            o("  vec4 vgx_old = %s;\n", fTex.c_str());
            if (sK != vgx::silhfuse::K_ZERO || dK == vgx::silhfuse::K_SC || dK == vgx::silhfuse::K_ISC) o("  vec3 vgx_s = floor(fin.rgb * 255.0 + vec3(0.5));\n");
            o("  float vgx_a = floor(fin.a * 255.0 + 0.5);\n");
            o("  vec3 vgx_d = floor(vgx_old.rgb * 255.0 + vec3(0.5));\n");
            term("vgx_ts", "vgx_s", sK);
            term("vgx_td", "vgx_d", dK);
            o("  vec3 vgx_r = clamp(vgx_td %s vgx_ts, 0.0, 255.0);\n", vgx::silhfuse::CodeRev(code) ? "-" : "+");
            const uint8_t am = k.silhFuseAlpha();
            const char* alpha = am == vgx::silhfuse::A_SRC ? "min((vgx_a + 0.25) * 0.00392156862745098, 1.0)" : (am == vgx::silhfuse::A_ZERO ? "0.0" : "vgx_old.a");
            o("  vec4 vgx_new = vec4(min((vgx_r + vec3(0.25)) * 0.00392156862745098, vec3(1.0)), %s);\n", alpha);
            o("  gl_FragColor = vgx_ok ? vgx_new : vgx_old;\n}\n");
        } else if (rejectZero) {
            if (fogMix) o("  vec3 fogc = mix(vec3(fin.rgb), vec3(u_tev[%d].rgb), fg);\n  gl_FragColor = vgx_ok ? vec4(fogc, float(fin.a)) : vec4(0.0);\n}\n", S(V_FOGCOL));
            else o("  gl_FragColor = vgx_ok ? vec4(fin) : vec4(0.0);\n}\n");
        } else if (fogMix) {
            o("  vec3 fogc = mix(vec3(fin.rgb), vec3(u_tev[%d].rgb), fg);\n  gl_FragColor = vec4(fogc, float(fin.a));\n}\n", S(V_FOGCOL));
        } else {
            o("  gl_FragColor = fin;\n}\n");
        }
    }
    if (missingSlot) {
        g_fsRegsStats.fallbacks.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const int canonBase = FsTevBase(k);
    const int canonCount = FsTevCount(k);
    src.text = std::move(o.s);
    src.texMask = texMask;
    src.texChan = FsTexChannels(k);
    src.numTexGens = k.numTexGens;
    src.nTev = L.count;
    src.nTevCanon = static_cast<uint8_t>(canonCount);
    src.tevGather = true;
    std::memset(src.tevSrc, 0, sizeof src.tevSrc);
    for (uint8_t i = 0; i < L.count; ++i) {
        const uint8_t r = L.src[i];
        src.tevSrc[i] = r < 28 ? r : static_cast<uint8_t>(canonBase + (r - 28));
    }
    src.trimOps = static_cast<uint8_t>(trimOps > 255 ? 255 : trimOps);
    src.texReused = static_cast<uint8_t>(texReused > 255 ? 255 : texReused);

    const uint64_t oldRows = static_cast<uint64_t>(FsTevCountBase(k) + (k.dualWrap() ? 4 : 0));
    const uint64_t newRows = static_cast<uint64_t>(L.count);
    uint64_t elided = 0, uvs = 0, inds = 0;
    for (int r = 0; r < 4; ++r) {
        if (!p.rgbWritten[r]) ++elided;
        if (!p.aWritten[r]) ++elided;
    }
    for (int c = 0; c < k.numTexGens && c < 8; ++c)
        if (!((p.projOnly >> c) & 1u)) ++uvs;
    for (int i = 0; i < k.numIndStages && i < 4; ++i) ++inds;
    g_fsRegsStats.shaders.fetch_add(1, std::memory_order_relaxed);
    g_fsRegsStats.rowsOld.fetch_add(oldRows, std::memory_order_relaxed);
    g_fsRegsStats.rowsNew.fetch_add(newRows, std::memory_order_relaxed);
    FsRegsAtomicMax(g_fsRegsStats.maxOld, oldRows);
    FsRegsAtomicMax(g_fsRegsStats.maxNew, newRows);
    g_fsRegsStats.regsElided.fetch_add(elided, std::memory_order_relaxed);
    g_fsRegsStats.uvsSunk.fetch_add(uvs, std::memory_order_relaxed);
    g_fsRegsStats.indSunk.fetch_add(inds, std::memory_order_relaxed);
    return true;
}

// GXV_FS_ATEARLY=1: evaluate all TEV alpha stages and execute `discard` BEFORE colour stages and fog.
// When `useRegs` is true (GXV_FS_REGS=1 + GXV_FS_ATEARLY=1), also compacts u_tev[], elides/sinks TEV
// register temporaries, sinks projective uv/ind lookups in Phase 1, narrows live tex_s variables to
// only the channels (.rgb / .a / vec4) actually read, narrows ras, and inlines non-compare operands.
bool GenerateFragmentShaderAtEarly(const FsKey& k, ShaderSource& src, bool hf, unsigned parts, bool useRegs) {
    Out o;
    const int nStages = k.numStages < 1 ? 1 : (k.numStages > 16 ? 16 : k.numStages);
    const int nInd = k.numIndStages > 4 ? 4 : k.numIndStages;
    const int nTg = k.numTexGens > 8 ? 8 : k.numTexGens;

    FsRegPlan p;
    if (useRegs) BuildFsRegPlan(k, p);
    bool missingSlot = false;
    auto Row = [&](int row) -> int {
        if (!useRegs) return row;
        const int sl = (row >= 0 && row < 32) ? p.layout.slot[row] : -1;
        if (sl < 0) { missingSlot = true; return 0; }
        return sl;
    };

    FsTrimInfo ti;
    bool hasCompare = false;
    for (int s = 0; s < nStages; ++s)
        if (bits(k.stage[s].color, 16, 2) == 3 || bits(k.stage[s].alpha, 16, 2) == 3) hasCompare = true;
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
    unsigned trimOps = 0;
    bool loC[16] = {}, hiC[16] = {}, loA[16] = {}, hiA[16] = {};
    const char* const T1 = hf ? "hfloat" : "float";
    const char* const T3 = hf ? "hvec3" : "vec3";
    const char* const T4 = hf ? "hvec4" : "vec4";
    auto Lit = [&](const char* lit) -> std::string { return hf ? std::string("hfloat(") + lit + ")" : std::string(lit); };
    const FsKey::Stage& last = k.stage[nStages - 1];
    bool wrapColor = false, wrapAlpha = false;
    FinalWrapPlanTrim(k, trimWrap ? &ti : nullptr, wrapColor, wrapAlpha);
    if (trim) {
        bool fullColor = false, fullAlpha = false;
        FinalWrapPlan(k, fullColor, fullAlpha);
        trimOps += static_cast<unsigned>(fullColor && !wrapColor) * 3u + static_cast<unsigned>(fullAlpha && !wrapAlpha);
    }

    uint8_t inWrapKeep[16], inWrapInit[16] = {};
    std::fill(inWrapKeep, inWrapKeep + 16, static_cast<uint8_t>(0x3F));
    if ((k.inWrap & 1) && TevInCanonMode() == 2) {
        TevInWalk tw;
        WalkTevIn(k, 0xFu, true, false, tw);
        for (int s = 0; s < 16; ++s) { inWrapKeep[s] = tw.keep[s]; inWrapInit[s] = tw.keepInit[s]; }
    }
    const uint8_t dualRegs = (k.inWrap & 1) && TevInCanonMode() == 2 ? k.dualWrap() : static_cast<uint8_t>(0);
    const int wb = FsTevBase(k);
    auto DualRow = [&](int r) -> int { return useRegs ? Row(28 + r) : (wb + r); };
    const bool projRead = FsProjMode() != 0;
    const bool texReuse = FsTexReuseMode() != 0;

    uint8_t texMask = 0;
    bool usesCol[2] = {false, false};
    for (int s = 0; s < nStages; ++s) {
        if (k.stage[s].texEnable) texMask |= static_cast<uint8_t>(1u << k.stage[s].texMap);
        if (k.stage[s].rasChan < 2) usesCol[k.stage[s].rasChan] = true;
    }
    for (int i = 0; i < nInd; ++i) texMask |= static_cast<uint8_t>(1u << k.ind[i].texMap);

    uint8_t indVsTg = 0;
    for (int i = 0; i < nInd; ++i)
        if (k.indVs(i) && k.ind[i].texCoord < nTg) indVsTg |= static_cast<uint8_t>(1u << k.ind[i].texCoord);
    auto texV2 = [&](int i) { return (k.texVec2() && !(k.texProjMask & (1u << i))) || ((indVsTg >> i) & 1u); };

    uint8_t projOnly = 0;
    if (projRead) {
        projOnly = k.texProjMask;
        for (int s = 0; s < nStages; ++s) {
            const FsKey::Stage& st = k.stage[s];
            const bool usesCoord = st.texEnable || st.ind;
            if (usesCoord && (st.ind || st.lodBias)) projOnly &= static_cast<uint8_t>(~(1u << (st.texCoord & 7)));
        }
        for (int i = 0; i < nInd; ++i) projOnly &= static_cast<uint8_t>(~(1u << (k.ind[i].texCoord & 7)));
    }

    // Partition texture producers, indirect lookups and texcoords between Phase 1 (alpha) and Phase 2 (colour).
    int texProducer[16];
    std::fill(texProducer, texProducer + 16, -1);
    bool texReusedStage[16] = {};
    int curProd = -1, lastReadProd = -1;
    uint8_t lastMap = 0, lastCoord = 0, lastSwap = 0, lastBias = 0, lastProj = 0;
    for (int s = 0; s < nStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        if (st.texEnable) {
            const bool projHere = projRead && !st.lodBias && !st.ind && st.texCoord < nTg && ((k.texProjMask >> st.texCoord) & 1u) != 0;
            const bool reusable = !st.ind;
            if (texReuse && reusable && lastReadProd >= 0 && lastMap == st.texMap && lastCoord == st.texCoord && lastSwap == st.texSwap &&
                lastBias == st.lodBias && lastProj == (projHere ? 1 : 0)) {
                texReusedStage[s] = true;
                curProd = lastReadProd;
            } else {
                curProd = s;
                if (reusable) {
                    lastReadProd = s; lastMap = st.texMap; lastCoord = st.texCoord; lastSwap = st.texSwap;
                    lastBias = st.lodBias; lastProj = projHere ? 1 : 0;
                } else {
                    lastReadProd = -1;
                }
            }
        } else if (st.texOff) {
            curProd = -1;
            lastReadProd = -1;
        }
        texProducer[s] = curProd;
    }

    bool texInAlpha[16] = {}, texInColor[16] = {};
    bool texNeedsRgb[16] = {}, texNeedsA[16] = {};
    for (int s = 0; s < nStages; ++s) {
        const uint32_t tc = k.stage[s].color, ta = k.stage[s].alpha;
        const int prod = texProducer[s];
        if (prod < 0) continue;
        for (int i = 0; i < 4; ++i) {
            const uint32_t cs = bits(tc, i * 4, 4);
            if (cs == 8) { texInColor[prod] = true; texNeedsRgb[prod] = true; }
            else if (cs == 9) { texInColor[prod] = true; texNeedsA[prod] = true; }
            if (bits(ta, 4 + i * 3, 3) == 4) { texInAlpha[prod] = true; texNeedsA[prod] = true; }
        }
    }
    // v2 (GXM implicit-LOD safety): keep EVERY texture read (and its indirect lookup + projective uv divide)
    // in Phase 1 BEFORE `discard;`, so no texture2D/texture2DProj ever executes after a fragment kill.
    uint64_t keptColorOnlyTex = 0;
    for (int s = 0; s < nStages; ++s) {
        if (!texInAlpha[s] && texInColor[s]) {
            texInAlpha[s] = true;
            ++keptColorOnlyTex;
        }
    }

    bool indInAlpha[4] = {};
    bool anyIndAlpha = false;
    for (int s = 0; s < nStages; ++s) {
        if (!k.stage[s].ind) continue;
        const uint32_t bt = bits(k.stage[s].ind, 0, 2);
        if (texInAlpha[s]) {
            anyIndAlpha = true;
            if (bt < static_cast<uint32_t>(nInd)) indInAlpha[bt] = true;
        }
    }

    bool uvInAlpha[8] = {};
    auto markUv = [&](int c) {
        if (c < 0 || c >= nTg || ((projOnly >> c) & 1u)) return;
        uvInAlpha[c] = true;
    };
    for (int i = 0; i < nInd; ++i) {
        if (((indVsTg >> k.ind[i].texCoord) & 1u) && k.indVs(i)) continue;
        if (indInAlpha[i]) markUv(k.ind[i].texCoord);
    }
    for (int s = 0; s < nStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        const bool projHere = projRead && st.texEnable && !st.lodBias && !st.ind && st.texCoord < nTg && ((k.texProjMask >> st.texCoord) & 1u) != 0;
        if (texInAlpha[s] && (st.ind || !projHere)) markUv(st.texCoord);
    }

    // Track cross-stage alpha reads from colour stages so any value overwritten by a later alpha stage is snapshotted in Phase 1.
    int colAlphaVer[16][4];
    bool colAlphaOverwritten[16][4] = {};
    bool saveStageA[16] = {};
    int lastAWrite[4] = {-1, -1, -1, -1};
    for (int s = 0; s < nStages; ++s) {
        const uint32_t tc = k.stage[s].color, ta = k.stage[s].alpha;
        for (int i = 0; i < 4; ++i) {
            colAlphaVer[s][i] = -2;
            const uint32_t sel = bits(tc, i * 4, 4);
            if (sel < 8 && (sel & 1u)) {
                const int r = static_cast<int>(sel / 2);
                const int ver = lastAWrite[r];
                colAlphaVer[s][i] = ver;
                bool over = false;
                for (int s2 = s; s2 < nStages; ++s2)
                    if (static_cast<int>(bits(k.stage[s2].alpha, 22, 2)) == r) { over = true; break; }
                colAlphaOverwritten[s][i] = over;
                if (over && ver >= 0) saveStageA[ver] = true;
            }
        }
        lastAWrite[bits(ta, 22, 2)] = s;
    }

    // Header & declarations (compacted u_tev[] when useRegs, canonical layout otherwise).
    o("#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n");
    if (hf) {
        o("#ifdef VITAGL\n#define hfloat half\n#define hvec3 half3\n#define hvec4 half4\n#else\n"
          "#define hfloat float\n#define hvec3 vec3\n#define hvec4 vec4\n#endif\n");
    }
    if (wrapColor || wrapAlpha) {
        o("float vgx_wrap_final(float value) {\n"
          "  if (value >= 0.0 && value <= 1.0) return value;\n"
          "  float byteValue = floor(value * 255.0 + 0.5);\n"
          "  return (byteValue - 256.0 * floor(byteValue / 256.0)) / 255.0;\n}\n");
    }
    if (k.inWrap & 1) {
        o("float vgx_w1(float value) {\n"
          "  if (value >= 0.0 && value <= 1.0) return value;\n"
          "  float byteValue = floor(value * 255.0 + 0.5);\n"
          "  return (byteValue - 256.0 * floor(byteValue / 256.0)) / 255.0;\n}\n"
          "vec3 vgx_w3(vec3 value) { return vec3(vgx_w1(value.x), vgx_w1(value.y), vgx_w1(value.z)); }\n");
    }
    for (int m = 0; m < 8; ++m)
        if (texMask & (1u << m)) o("uniform sampler2D u_tex%d;\n", m);
    o("uniform %s u_tev[%d];\n", T4, useRegs ? static_cast<int>(p.layout.count) : FsTevCount(k));
    if (usesCol[0] || (k.regFromVtx & 1)) o("varying %s v_col0;\n", T4);
    if (usesCol[1] || (k.regFromVtx & 2)) o("varying %s v_col1;\n", T4);
    for (int i = 0; i < nTg; ++i) o(texV2(i) ? "varying vec2 v_tex%d;\n" : "varying vec3 v_tex%d;\n", i);
    if (k.fog & kFogVtxBit) {
        o("varying float %s;\n", FogVaryingName());
        NoteFogSlotCollision(false, k.numTexGens, usesCol[0] || (k.regFromVtx & 1), usesCol[1] || (k.regFromVtx & 2));
    }
    o("void main() {\n");

    auto InitReg = [&](int r) -> std::string {
        if (r == 1 && (k.regFromVtx & 1)) return "v_col0";
        if (r == 2 && (k.regFromVtx & 2)) return "v_col1";
        return "u_tev[" + std::to_string(Row(V_REG + r)) + "]";
    };
    auto RegRgbExpr = [&](int r) -> std::string {
        if (!useRegs || p.rgbWritten[r]) return std::string(RegName(static_cast<uint32_t>(r))) + "_rgb";
        return InitReg(r) + ".rgb";
    };
    auto RegAExpr = [&](int r) -> std::string {
        if (!useRegs || p.aWritten[r]) return std::string(RegName(static_cast<uint32_t>(r))) + "_a";
        return InitReg(r) + ".a";
    };
    auto RegAaaExpr = [&](int r) -> std::string {
        if (!useRegs || p.aWritten[r]) return std::string(T3) + "(" + RegName(static_cast<uint32_t>(r)) + "_a)";
        return InitReg(r) + ".aaa";
    };
    auto KonstColExpr = [&](uint8_t sel) -> std::string {
        if (!useRegs) return KonstColor(sel, T3);
        char b[64];
        if (sel < 8) { std::snprintf(b, sizeof b, "%s(%.3f)", T3, (8 - sel) / 8.0); return b; }
        if (sel < 12) return std::string(T3) + "(0.0)";
        const int kk = sel & 3;
        static const char* const comp[5] = {"rgb", "rrr", "ggg", "bbb", "aaa"};
        std::snprintf(b, sizeof b, "u_tev[%d].%s", Row(V_KONST + kk), comp[(sel - 12) >> 2]);
        return b;
    };
    auto KonstAlpExpr = [&](uint8_t sel) -> std::string {
        if (!useRegs) return KonstAlpha(sel, hf);
        char b[64];
        if (sel < 8) { std::snprintf(b, sizeof b, hf ? "hfloat(%.3f)" : "%.3f", (8 - sel) / 8.0); return b; }
        if (sel < 16) return Lit("0.0");
        const int kk = sel & 3;
        static const char* const comp[4] = {"r", "g", "b", "a"};
        std::snprintf(b, sizeof b, "u_tev[%d].%s", Row(V_KONST + kk), comp[(sel - 16) >> 2]);
        return b;
    };
    auto Uv = [&](int c) -> std::string {
        if (c >= nTg) return "vec2(0.0)";
        if (useRegs && !((k.texProjMask >> c) & 1u))
            return texV2(c) ? ("v_tex" + std::to_string(c)) : ("v_tex" + std::to_string(c) + ".xy");
        return "uv" + std::to_string(c);
    };
    auto EmitUv = [&](int c) {
        if (k.texProjMask & (1u << c)) o("  vec2 uv%d = v_tex%d.xy / (v_tex%d.z == 0.0 ? 1.0 : v_tex%d.z);\n", c, c, c, c);
        else if (texV2(c)) o("  vec2 uv%d = v_tex%d;\n", c, c);
        else o("  vec2 uv%d = v_tex%d.xy;\n", c, c);
    };
    auto EmitIndLookup = [&](int i) {
        if (((indVsTg >> k.ind[i].texCoord) & 1u) && k.indVs(i))
            o("  vec3 ind%d = texture2D(u_tex%d, v_tex%d).abg * 255.0;\n", i, k.ind[i].texMap, k.ind[i].texCoord);
        else
            o("  vec3 ind%d = texture2D(u_tex%d, %s * u_tev[%d].xy).abg * 255.0;\n", i, k.ind[i].texMap, Uv(k.ind[i].texCoord).c_str(), Row(V_INDSCALE + i));
    };
    auto EmitStageTexSample = [&](int s) {
        const FsKey::Stage& st = k.stage[s];
        std::string coord = Uv(st.texCoord);
        if (st.ind) {
            const uint32_t ind = st.ind;
            const uint32_t bt = bits(ind, 0, 2), fmt = bits(ind, 2, 2), bias = bits(ind, 4, 3), mid = bits(ind, 9, 4);
            const uint32_t sw = bits(ind, 13, 3), tw = bits(ind, 16, 3);
            const int map = st.texMap;
            if (k.indPre() == s + 1 && fmt == 0 && mid >= 1 && mid <= 3 && sw == 0 && tw == 0) {
                o("  {\n");
                if (bt < static_cast<uint32_t>(nInd)) o("    vec3 iv = ind%u;\n", bt);
                else o("    vec3 iv = vec3(0.0);\n");
                o("    vec2 io = vec2(dot(u_tev[%d].xyz, iv), dot(u_tev[%d].xyz, iv));\n", Row(V_INDMTX + (mid - 1) * 2), Row(V_INDMTX + (mid - 1) * 2 + 1));
                o("    itc = %s + io * u_tev[%d].xy + u_tev[%d].zw;\n  }\n", coord.c_str(), Row(V_INDPRE), Row(V_INDPRE));
            } else {
                o("  {\n    vec2 tsz = u_tev[%d].%s;\n", Row(V_TEXSIZE + map / 2), (map & 1) ? "zw" : "xy");
                o("    vec2 tcx = %s * tsz;\n", coord.c_str());
                if (bt < static_cast<uint32_t>(nInd)) o("    vec3 iv = ind%u;\n", bt);
                else o("    vec3 iv = vec3(0.0);\n");
                static const float kMod[4] = {256.0f, 32.0f, 16.0f, 8.0f};
                if (fmt) o("    iv = iv - %.1f * floor(iv / %.1f);\n", kMod[fmt], kMod[fmt]);
                const float bv = fmt == 0 ? -128.0f : 1.0f;
                o("    iv += vec3(%.1f, %.1f, %.1f);\n", (bias & 1) ? bv : 0.0f, (bias & 2) ? bv : 0.0f, (bias & 4) ? bv : 0.0f);
                if (mid == 0) o("    vec2 io = vec2(0.0);\n");
                else if (mid <= 3) o("    vec2 io = vec2(dot(u_tev[%d].xyz, iv), dot(u_tev[%d].xyz, iv));\n", Row(V_INDMTX + (mid - 1) * 2), Row(V_INDMTX + (mid - 1) * 2 + 1));
                else if (mid >= 5 && mid <= 7) o("    vec2 io = tcx * (iv.x / 256.0) * u_tev[%d].w;\n", Row(V_INDMTX + (mid - 5) * 2));
                else if (mid >= 9 && mid <= 11) o("    vec2 io = tcx * (iv.y / 256.0) * u_tev[%d].w;\n", Row(V_INDMTX + (mid - 9) * 2));
                else o("    vec2 io = vec2(0.0);\n");
                static const float kWrap[8] = {0, 256, 128, 64, 32, 16, 0, 0};
                if (sw == 6) o("    tcx.x = 0.0;\n");
                else if (sw) o("    tcx.x = tcx.x - %.1f * floor(tcx.x / %.1f);\n", kWrap[sw], kWrap[sw]);
                if (tw == 6) o("    tcx.y = 0.0;\n");
                else if (tw) o("    tcx.y = tcx.y - %.1f * floor(tcx.y / %.1f);\n", kWrap[tw], kWrap[tw]);
                o("    itc = (tcx + io) / tsz;\n  }\n");
            }
            coord = "itc";
        }
        const bool projHere = projRead && st.texEnable && !st.lodBias && !st.ind && st.texCoord < nTg && ((k.texProjMask >> st.texCoord) & 1u) != 0;
        const bool needRgb = !useRegs || texNeedsRgb[s];
        const bool needA = !useRegs || texNeedsA[s];
        const std::string swz = Swizzle(st.texSwap);
        const char* declTy = (needRgb && needA) ? T4 : (needRgb ? T3 : T1);
        const std::string varName = (needRgb && needA) ? ("tex_" + std::to_string(s))
                                  : (needRgb ? ("tex_" + std::to_string(s) + "_rgb")
                                             : ("tex_" + std::to_string(s) + "_a"));
        const std::string swzUsed = (needRgb && needA) ? swz
                                  : (needRgb ? swz.substr(0, 3) : std::string(1, swz[3]));
        if (projHere) {
            if (hf) o("  %s %s = %s(texture2DProj(u_tex%d, v_tex%d).%s);\n", declTy, varName.c_str(), declTy, st.texMap, st.texCoord, swzUsed.c_str());
            else o("  %s %s = texture2DProj(u_tex%d, v_tex%d).%s;\n", declTy, varName.c_str(), st.texMap, st.texCoord, swzUsed.c_str());
        } else if (hf) {
            if (st.lodBias) o("  %s %s = %s(texture2D(u_tex%d, %s, float(u_tev[%d].%c)).%s);\n", declTy, varName.c_str(), declTy, st.texMap, coord.c_str(), Row(V_LODBIAS + st.texMap / 4), "xyzw"[st.texMap & 3], swzUsed.c_str());
            else o("  %s %s = %s(texture2D(u_tex%d, %s).%s);\n", declTy, varName.c_str(), declTy, st.texMap, coord.c_str(), swzUsed.c_str());
        } else if (st.lodBias) {
            o("  %s %s = texture2D(u_tex%d, %s, u_tev[%d].%c).%s;\n", declTy, varName.c_str(), st.texMap, coord.c_str(), Row(V_LODBIAS + st.texMap / 4), "xyzw"[st.texMap & 3], swzUsed.c_str());
        } else {
            o("  %s %s = texture2D(u_tex%d, %s).%s;\n", declTy, varName.c_str(), st.texMap, coord.c_str(), swzUsed.c_str());
        }
    };

    // ==================== PHASE 1: ALPHA STAGES + EARLY DISCARD ====================
    o("  // GXV_FS_ATEARLY phase 1: alpha + discard\n");
    if (!useRegs) {
        o("  %s prev_a = u_tev[0].a, c0_a = %s.a, c1_a = %s.a, c2_a = u_tev[3].a;\n", T1, InitReg(1).c_str(), InitReg(2).c_str());
        for (int c = 0; c < nTg; ++c) if (uvInAlpha[c]) EmitUv(c);
        for (int i = 0; i < nInd; ++i) if (indInAlpha[i]) EmitIndLookup(i);
        if (anyIndAlpha) o("  vec2 itc = vec2(0.0);\n");
    }

    const std::string kBias[3] = {"", " + " + Lit("0.5"), " - " + Lit("0.5")};
    const std::string kScale[4] = {"", " * " + Lit("2.0"), " * " + Lit("4.0"), " * " + Lit("0.5")};
    const bool w = (k.inWrap & 1) != 0;
    unsigned texReused = 0;
    uint64_t snapCount = 0;
    for (int s = 0; s < nStages; ++s) {
        if (useRegs) {
            for (int c = 0; c < nTg; ++c)
                if (p.firstProjUvStage[c] == s) EmitUv(c);
            for (int i = 0; i < nInd; ++i)
                if (p.firstIndStage[i] == s) EmitIndLookup(i);
            if (p.firstIndOpStage == s) o("  vec2 itc = vec2(0.0);\n");
            for (int r = 0; r < 4; ++r) {
                if (p.aWritten[r] && p.firstAStage[r] == s) {
                    if (p.aReadInit[r]) o("  %s %s_a = %s.a;\n", T1, RegName(static_cast<uint32_t>(r)), InitReg(r).c_str());
                    else o("  %s %s_a;\n", T1, RegName(static_cast<uint32_t>(r)));
                }
            }
        }
        if (texReusedStage[s] && texProducer[s] >= 0 && (texInAlpha[texProducer[s]] || texInColor[texProducer[s]])) ++texReused;
        if (texInAlpha[s]) EmitStageTexSample(s);
        const FsKey::Stage& st = k.stage[s];
        const uint32_t ta = st.alpha;
        const int prod = texProducer[s];
        std::string texA = Lit("1.0");
        if (prod >= 0 && texInAlpha[prod] && (!useRegs || texNeedsA[prod])) {
            texA = (!useRegs || texNeedsRgb[prod]) ? ("tex_" + std::to_string(prod) + ".a")
                                                   : ("tex_" + std::to_string(prod) + "_a");
        }
        bool kaUsed = false, rasAUsed = false;
        for (int i = 0; i < 4; ++i) {
            const uint32_t as = bits(ta, 4 + i * 3, 3);
            if (as == 6) kaUsed = true;
            if (as == 5) rasAUsed = true;
        }
        if (saveStageA[s]) { o("  %s at_sa%d;\n", T1, s); ++snapCount; }
        o("  { // alpha %d\n", s);
        if (rasAUsed) {
            if (st.rasChan < 2) o("    %s ras_a = v_col%d.%c;\n", T1, st.rasChan, Swizzle(st.rasSwap)[3]);
            else if (!useRegs) o("    %s ras_a = %s;\n", T1, Lit("0.0").c_str());
        }
        if (kaUsed) o("    %s ka = %s;\n", T1, KonstAlpExpr(st.kasel).c_str());
        auto aArg = [&](uint32_t sel) -> std::string {
            switch (sel & 7u) {
            case 0: case 1: case 2: case 3: return RegAExpr(static_cast<int>(sel));
            case 4: return texA;
            case 5: return (!useRegs || st.rasChan < 2) ? "ras_a" : Lit("0.0");
            case 6: return "ka";
            default: return Lit("0.0");
            }
        };
        auto aIn = [&](uint32_t sel, uint8_t bit) -> std::string {
            if (w && sel < 4 && (inWrapKeep[s] & bit)) {
                if (((dualRegs >> sel) & 1u) && (inWrapInit[s] & bit)) return "u_tev[" + std::to_string(DualRow(static_cast<int>(sel))) + "].a";
                return WrapArg(aArg(sel), true, false);
            }
            return aArg(sel);
        };
        const uint32_t bias = bits(ta, 16, 2), op = bits(ta, 18, 1), clampOn = bits(ta, 19, 1), scale = bits(ta, 20, 2);
        const std::string aA = aIn(bits(ta, 13, 3), 8), aB = aIn(bits(ta, 10, 3), 16), aC = aIn(bits(ta, 7, 3), 32), aD = aArg(bits(ta, 4, 3));
        if (!useRegs) {
            o("    %s aa = %s, ab = %s, ac = %s, ad = %s;\n", T1, aA.c_str(), aB.c_str(), aC.c_str(), aD.c_str());
            if (bias != 3) {
                o("    %s ra = (ad %s mix(aa, ab, ac)%s)%s;\n", T1, op ? "-" : "+", kBias[bias].c_str(), kScale[scale].c_str());
            } else {
                const float e = 0.25f / 255.0f;
                if (op) o("    %s ra = ad + ((abs(aa - ab) < %.6f) ? ac : %s(0.0));\n", T1, e * 2, T1);
                else o("    %s ra = ad + ((aa > ab + %.6f) ? ac : %s(0.0));\n", T1, e, T1);
            }
        } else if (bias != 3) {
            o("    %s ra = (%s %s mix(%s, %s, %s)%s)%s;\n", T1, aD.c_str(), op ? "-" : "+", aA.c_str(), aB.c_str(), aC.c_str(),
              kBias[bias].c_str(), kScale[scale].c_str());
        } else {
            o("    %s aa = %s, ab = %s;\n", T1, aA.c_str(), aB.c_str());
            const float e = 0.25f / 255.0f;
            if (op) o("    %s ra = %s + ((abs(aa - ab) < %.6f) ? %s : %s(0.0));\n", T1, aD.c_str(), e * 2, aC.c_str(), T1);
            else o("    %s ra = %s + ((aa > ab + %.6f) ? %s : %s(0.0));\n", T1, aD.c_str(), e, aC.c_str(), T1);
        }
        const FsTrimClamp act = trim ? ti.a[s] : FsTrimClamp{};
        if (trim && !(act.lo && act.hi)) {
            trimOps += (act.lo ? 0u : 1u) + (act.hi ? 0u : 1u);
            if (act.lo) o("    ra = max(ra, %s(%s));\n", T1, clampOn ? "0.0" : "-4.0157");
            else if (act.hi) o("    ra = min(ra, %s(%s));\n", T1, clampOn ? "1.0" : "4.0118");
            if (clampOn && act.lo) loA[s] = true;
            else if (clampOn && act.hi) hiA[s] = true;
        } else if (clampOn) {
            o("    ra = clamp(ra, %s, %s);\n", Lit("0.0").c_str(), Lit("1.0").c_str());
            loA[s] = hiA[s] = true;
        } else {
            o("    ra = clamp(ra, %s, %s);\n", Lit("-4.0157").c_str(), Lit("4.0118").c_str());
        }
        o("    %s_a = ra;\n", RegName(bits(ta, 22, 2)));
        if (saveStageA[s]) o("    at_sa%d = ra;\n", s);
        o("  }\n");
    }

    // Final alpha wrap / clamp matching the original path.
    const bool finLo = !trimFinal || (!wrapColor && ti.finC.lo) || (!wrapAlpha && ti.finA.lo);
    const bool finHi = !trimFinal || (!wrapColor && ti.finC.hi) || (!wrapAlpha && ti.finA.hi);
    if (trim) trimOps += (finLo ? 0u : 1u) + (finHi ? 0u : 1u);
    const int lastS = nStages - 1;
    const bool doneALo = !wrapAlpha && loA[lastS], doneAHi = !wrapAlpha && hiA[lastS];
    const bool wFin = wrapColor || wrapAlpha;
    const char* finA_ty = wFin ? "float" : T1;
    const std::string zFin = wFin ? "0.0" : Lit("0.0"), oneFin = wFin ? "1.0" : Lit("1.0");
    o("  %s fin_a = %s(%s_a);\n", finA_ty, finA_ty, RegName(bits(last.alpha, 22, 2)));
    if (wrapAlpha) o("  fin_a = vgx_wrap_final(fin_a);\n");
    const bool aLo = FsFinClampMode() ? ((!trimFinal || (!wrapAlpha && ti.finA.lo)) && !doneALo) : finLo;
    const bool aHi = FsFinClampMode() ? ((!trimFinal || (!wrapAlpha && ti.finA.hi)) && !doneAHi) : finHi;
    if (aLo && aHi) o("  fin_a = clamp(fin_a, %s, %s);\n", zFin.c_str(), oneFin.c_str());
    else if (aLo) o("  fin_a = max(fin_a, %s(0.0));\n", finA_ty);
    else if (aHi) o("  fin_a = min(fin_a, %s(1.0));\n", finA_ty);

    // Alpha test + early discard BEFORE any colour stage or fog!
    const bool atFast = FsAtFastKey(k);
    if (atFast) {
        const int thrRow = Row(V_ATTHR);
        const int kind = AtFastKind(k.alphaTest);
        if (kind == 1) o("  if (!(fin_a >= u_tev[%d].x)) discard;\n", thrRow);
        else if (kind == 2) o("  if (!(fin_a < u_tev[%d].y)) discard;\n", thrRow);
        else o("  if (!(abs(fin_a - u_tev[%d].x) < u_tev[%d].y)) discard;\n", thrRow, thrRow);
    } else {
        char b0[96], b1[96], r0[32], r1[32];
        const int mr = Row(V_MISC);
        std::snprintf(r0, sizeof r0, hf ? "float(u_tev[%d].x)" : "u_tev[%d].x", mr);
        std::snprintf(r1, sizeof r1, hf ? "float(u_tev[%d].y)" : "u_tev[%d].y", mr);
        const char* e0 = CompareExpr(k.alphaTest & 7, r0, b0, sizeof b0);
        const char* e1 = CompareExpr((k.alphaTest >> 3) & 7, r1, b1, sizeof b1);
        static const char* const kOp[4] = {"&&", "||", "!=", "=="};
        if (trimFloor) {
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
            o(hf ? "  float ax = float(fin_a) * 255.0 + 0.5;\n" : "  float ax = fin_a * 255.0 + 0.5;\n");
            if (c0 == 2 || c0 == 5 || c1 == 2 || c1 == 5) o("  float ai = floor(ax);\n");
            else ++trimOps;
        } else {
            o(hf ? "  float ai = floor(float(fin_a) * 255.0 + 0.5);\n" : "  float ai = floor(fin_a * 255.0 + 0.5);\n");
        }
        o("  if (!(%s %s %s)) discard;\n", e0, kOp[(k.alphaTest >> 6) & 3], e1);
    }

    // ==================== PHASE 2: COLOUR STAGES + FOG (NO TEXTURE FETCHES) ====================
    o("  // GXV_FS_ATEARLY phase 2: colour + fog (surviving fragments only)\n");
    if (!useRegs) {
        o("  %s prev_rgb = u_tev[0].rgb, c0_rgb = %s.rgb, c1_rgb = %s.rgb, c2_rgb = u_tev[3].rgb;\n", T3, InitReg(1).c_str(), InitReg(2).c_str());
    }

    for (int s = 0; s < nStages; ++s) {
        if (useRegs) {
            for (int r = 0; r < 4; ++r) {
                if (p.rgbWritten[r] && p.firstRgbStage[r] == s) {
                    if (p.rgbReadInit[r]) o("  %s %s_rgb = %s.rgb;\n", T3, RegName(static_cast<uint32_t>(r)), InitReg(r).c_str());
                    else o("  %s %s_rgb;\n", T3, RegName(static_cast<uint32_t>(r)));
                }
            }
        }
        const FsKey::Stage& st = k.stage[s];
        const uint32_t tc = st.color;
        const int prod = texProducer[s];
        const bool hasTex = prod >= 0 && (texInAlpha[prod] || texInColor[prod]);
        const std::string texName = hasTex ? ("tex_" + std::to_string(prod)) : std::string();
        bool kcUsed = false, rasRgbUsed = false, rasAUsed = false;
        for (int i = 0; i < 4; ++i) {
            const uint32_t cs = bits(tc, i * 4, 4);
            if (cs == 14) kcUsed = true;
            if (cs == 10) rasRgbUsed = true;
            if (cs == 11) rasAUsed = true;
        }
        o("  { // stage %d (colour)\n", s);
        if (rasRgbUsed || rasAUsed) {
            if (st.rasChan < 2) {
                const std::string rsw = Swizzle(st.rasSwap);
                if (!useRegs || (rasRgbUsed && rasAUsed)) o("    %s ras = v_col%d.%s;\n", T4, st.rasChan, rsw.c_str());
                else if (rasRgbUsed) o("    %s ras_rgb = v_col%d.%s;\n", T3, st.rasChan, rsw.substr(0, 3).c_str());
                else o("    %s ras_a = v_col%d.%c;\n", T1, st.rasChan, rsw[3]);
            } else if (!useRegs) {
                o("    %s ras = %s(0.0);\n", T4, T4);
            }
        }
        if (kcUsed) o("    %s kc = %s;\n", T3, KonstColExpr(st.kcsel).c_str());

        auto cArg = [&](int slotIdx, uint32_t sel) -> std::string {
            switch (sel & 15u) {
            case 0: case 2: case 4: case 6: return RegRgbExpr(static_cast<int>(sel / 2));
            case 1: case 3: case 5: case 7: {
                const int r = static_cast<int>(sel / 2);
                if (colAlphaOverwritten[s][slotIdx]) {
                    const int ver = colAlphaVer[s][slotIdx];
                    if (ver < 0) return InitReg(r) + ".aaa";
                    return std::string(T3) + "(at_sa" + std::to_string(ver) + ")";
                }
                return RegAaaExpr(r);
            }
            case 8:
                if (!hasTex || (useRegs && !texNeedsRgb[prod])) return std::string(T3) + "(1.0)";
                return (!useRegs || texNeedsA[prod]) ? (texName + ".rgb") : (texName + "_rgb");
            case 9:
                if (!hasTex || (useRegs && !texNeedsA[prod])) return std::string(T3) + "(1.0)";
                return (!useRegs || texNeedsRgb[prod]) ? (texName + ".aaa") : (std::string(T3) + "(" + texName + "_a)");
            case 10:
                if (!useRegs) return "ras.rgb";
                return st.rasChan < 2 ? (rasAUsed ? "ras.rgb" : "ras_rgb") : (std::string(T3) + "(0.0)");
            case 11:
                if (!useRegs) return "ras.aaa";
                return st.rasChan < 2 ? (rasRgbUsed ? "ras.aaa" : (std::string(T3) + "(ras_a)")) : (std::string(T3) + "(0.0)");
            case 12: return std::string(T3) + "(1.0)";
            case 13: return std::string(T3) + "(0.5)";
            case 14: return "kc";
            default: return std::string(T3) + "(0.0)";
            }
        };
        auto cIn = [&](int slotIdx, uint32_t sel, uint8_t bit) -> std::string {
            if (w && sel < 8 && (inWrapKeep[s] & bit)) {
                const uint32_t r = sel / 2;
                if (((dualRegs >> r) & 1u) && (inWrapInit[s] & bit))
                    return "u_tev[" + std::to_string(DualRow(static_cast<int>(r))) + "]" + ((sel & 1u) ? ".aaa" : ".rgb");
                return WrapArg(cArg(slotIdx, sel), true, true);
            }
            return cArg(slotIdx, sel);
        };
        const uint32_t bias = bits(tc, 16, 2), op = bits(tc, 18, 1), clampOn = bits(tc, 19, 1), scale = bits(tc, 20, 2);
        const std::string cA = cIn(3, bits(tc, 12, 4), 1), cB = cIn(2, bits(tc, 8, 4), 2),
                          cC = cIn(1, bits(tc, 4, 4), 4), cD = cArg(0, bits(tc, 0, 4));
        if (!useRegs) {
            o("    %s a = %s, b = %s, c = %s, d = %s;\n", T3, cA.c_str(), cB.c_str(), cC.c_str(), cD.c_str());
        } else if (bias == 3) {
            o("    %s a = %s, b = %s;\n", T3, cA.c_str(), cB.c_str());
        }
        if (bias != 3) {
            if (!useRegs) o("    %s rc = (d %s mix(a, b, c)%s)%s;\n", T3, op ? "-" : "+", kBias[bias].c_str(), kScale[scale].c_str());
            else o("    %s rc = (%s %s mix(%s, %s, %s)%s)%s;\n", T3, cD.c_str(), op ? "-" : "+", cA.c_str(), cB.c_str(), cC.c_str(),
                   kBias[bias].c_str(), kScale[scale].c_str());
        } else {
            const bool eq = op != 0;
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
                if (eq) std::snprintf(buf, sizeof buf, "(vec3(1.0) - step(vec3(%.6f), abs(a - b)))", e * 2);
                else std::snprintf(buf, sizeof buf, "(vec3(1.0) - step(a, b + vec3(%.6f)))", e);
                break;
            }
            const char* cc = useRegs ? cC.c_str() : "c";
            const char* dd = useRegs ? cD.c_str() : "d";
            if (scale == 3) o("    vec3 rc = %s + %s * %s;\n", dd, cc, buf);
            else o("    %s rc = %s + (%s ? %s : %s(0.0));\n", T3, dd, buf, cc, T3);
        }
        const FsTrimClamp act = trim ? ti.c[s] : FsTrimClamp{};
        if (trim && !(act.lo && act.hi)) {
            trimOps += (act.lo ? 0u : 1u) + (act.hi ? 0u : 1u);
            if (act.lo) o("    rc = max(rc, %s(%s));\n", T3, clampOn ? "0.0" : "-4.0157");
            else if (act.hi) o("    rc = min(rc, %s(%s));\n", T3, clampOn ? "1.0" : "4.0118");
            if (clampOn && act.lo) loC[s] = true;
            else if (clampOn && act.hi) hiC[s] = true;
        } else if (clampOn) {
            o("    rc = clamp(rc, %s, %s);\n", Lit("0.0").c_str(), Lit("1.0").c_str());
            loC[s] = hiC[s] = true;
        } else {
            o("    rc = clamp(rc, %s, %s);\n", Lit("-4.0157").c_str(), Lit("4.0118").c_str());
        }
        o("    %s_rgb = rc;\n  }\n", RegName(bits(tc, 22, 2)));
    }

    const bool doneCLo = !wrapColor && loC[lastS], doneCHi = !wrapColor && hiC[lastS];
    const char* finC_ty = wFin ? "vec3" : T3;
    o("  %s fin_rgb = %s(%s_rgb);\n", finC_ty, finC_ty, RegName(bits(last.color, 22, 2)));
    if (wrapColor) o("  fin_rgb = vec3(vgx_wrap_final(fin_rgb.r), vgx_wrap_final(fin_rgb.g), vgx_wrap_final(fin_rgb.b));\n");
    const bool cLo = FsFinClampMode() ? ((!trimFinal || (!wrapColor && ti.finC.lo)) && !doneCLo) : finLo;
    const bool cHi = FsFinClampMode() ? ((!trimFinal || (!wrapColor && ti.finC.hi)) && !doneCHi) : finHi;
    if (cLo && cHi) o("  fin_rgb = clamp(fin_rgb, %s, %s);\n", zFin.c_str(), oneFin.c_str());
    else if (cLo) o("  fin_rgb = max(fin_rgb, %s(0.0));\n", finC_ty);
    else if (cHi) o("  fin_rgb = min(fin_rgb, %s(1.0));\n", finC_ty);

    o("  %s fin = %s(fin_rgb, fin_a);\n", wFin ? "vec4" : T4, wFin ? "vec4" : T4);
    bool fogMix = false;
    if (k.fog) {
        const int fsel = k.fog & 7;
        if (k.fog & kFogHalfBit) {
            const int hl = Row(V_FOGHL), fc = Row(V_FOGCOL);
            o("  float fg = clamp((%s * float(u_tev[%d].x) - (float(u_tev[%d].z) + float(u_tev[%d].w))) * float(u_tev[%d].y) + float(u_tev[%d].w), 0.0, 1.0);\n",
              FogVaryingName(), hl, hl, hl, hl, fc);
            fogMix = true;
        } else if (k.fog & kFogVtxBit) {
            const int fr = Row(V_FOG);
            o("  float fg = clamp(u_tev[%d].x * %s + u_tev[%d].w, 0.0, 1.0);\n", fr, FogVaryingName(), fr);
        } else {
            const int fr = Row(V_FOG);
            if (k.fog & 8) o("  float fg = u_tev[%d].x * gl_FragCoord.z;\n", fr);
            else o("  float fg = u_tev[%d].x / max(u_tev[%d].y - gl_FragCoord.z, 0.000001);\n", fr, fr);
            o("  fg = clamp(fg - u_tev[%d].z, 0.0, 1.0);\n", fr);
        }
        switch (fsel) {
        case 4: o("  fg = 1.0 - exp2(-8.0 * fg);\n"); break;
        case 5: o("  fg = 1.0 - exp2(-8.0 * fg * fg);\n"); break;
        case 6: o("  fg = exp2(-8.0 * (1.0 - fg));\n"); break;
        case 7: o("  fg = 1.0 - fg; fg = exp2(-8.0 * fg * fg);\n"); break;
        default: break;
        }
        if (!fogMix) o("  fin.rgb = mix(fin.rgb, u_tev[%d].rgb, fg);\n", Row(V_FOGCOL));
    }
    if (k.dstAlpha) o("  fin.a = u_tev[%d].z;\n", Row(V_MISC));
    if (k.logicSrc == 1) o("  fin.rgb = vec3(1.0) - fin.rgb;\n");
    else if (k.logicSrc == 2) o("  fin.rgb = vec3(1.0);\n");
    if (fogMix) o("  vec3 fogc = mix(vec3(fin.rgb), vec3(u_tev[%d].rgb), fg);\n  gl_FragColor = vec4(fogc, float(fin.a));\n}\n", Row(V_FOGCOL));
    else o("  gl_FragColor = fin;\n}\n");

    if (useRegs && missingSlot) {
        g_fsRegsStats.fallbacks.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    src.text = std::move(o.s);
    src.texMask = texMask;
    src.texChan = FsTexChannels(k);
    src.numTexGens = k.numTexGens;
    if (useRegs) {
        const int canonBase = FsTevBase(k);
        const int canonCount = FsTevCount(k);
        src.nTev = p.layout.count;
        src.nTevCanon = static_cast<uint8_t>(canonCount);
        src.tevGather = true;
        std::memset(src.tevSrc, 0, sizeof src.tevSrc);
        for (uint8_t i = 0; i < p.layout.count; ++i) {
            const uint8_t r = p.layout.src[i];
            src.tevSrc[i] = r < 28 ? r : static_cast<uint8_t>(canonBase + (r - 28));
        }
        const uint64_t oldRows = static_cast<uint64_t>(FsTevCountBase(k) + (k.dualWrap() ? 4 : 0));
        const uint64_t newRows = static_cast<uint64_t>(p.layout.count);
        uint64_t elided = 0, uvs = 0, inds = 0;
        for (int r = 0; r < 4; ++r) {
            if (!p.rgbWritten[r]) ++elided;
            if (!p.aWritten[r]) ++elided;
        }
        for (int c = 0; c < nTg; ++c)
            if (!((p.projOnly >> c) & 1u)) ++uvs;
        for (int i = 0; i < nInd; ++i) ++inds;
        g_fsRegsStats.shaders.fetch_add(1, std::memory_order_relaxed);
        g_fsRegsStats.rowsOld.fetch_add(oldRows, std::memory_order_relaxed);
        g_fsRegsStats.rowsNew.fetch_add(newRows, std::memory_order_relaxed);
        FsRegsAtomicMax(g_fsRegsStats.maxOld, oldRows);
        FsRegsAtomicMax(g_fsRegsStats.maxNew, newRows);
        g_fsRegsStats.regsElided.fetch_add(elided, std::memory_order_relaxed);
        g_fsRegsStats.uvsSunk.fetch_add(uvs, std::memory_order_relaxed);
        g_fsRegsStats.indSunk.fetch_add(inds, std::memory_order_relaxed);
    } else {
        src.nTev = static_cast<uint8_t>(FsTevCount(k));
        src.nTevCanon = src.nTev;
        src.tevGather = false;
    }
    src.trimOps = static_cast<uint8_t>(trimOps > 255 ? 255 : trimOps);
    src.texReused = static_cast<uint8_t>(texReused > 255 ? 255 : texReused);

    g_fsAtEarlyStats.split.fetch_add(1, std::memory_order_relaxed);
    g_fsAtEarlyStats.colorStagesDeferred.fetch_add(static_cast<uint64_t>(nStages), std::memory_order_relaxed);
    g_fsAtEarlyStats.texKeptBeforeDiscard.fetch_add(keptColorOnlyTex, std::memory_order_relaxed);
    g_fsAtEarlyStats.snapVars.fetch_add(snapCount, std::memory_order_relaxed);
    return true;
}
// parts (GXV_FS_TRIM, kFsTrim* bits): which of the trims FsTrimAnalyse proves inert are made. 0: the text as always.
void GenerateFragmentShaderAs(const FsKey& k, ShaderSource& src, bool hf, unsigned parts) {
    if (FsAtEarlyMode()) {
        if (FsAtEarlySplitEligible(k)) {
            if (FsRegsMode() && GenerateFragmentShaderAtEarly(k, src, hf, parts, true)) return;
            GenerateFragmentShaderAtEarly(k, src, hf, parts, false);
            return;
        }
        if (FsHasAlphaTestText(k)) g_fsAtEarlyStats.ineligible.fetch_add(1, std::memory_order_relaxed);
    }
    if (FsRegsMode() && GenerateFragmentShaderRegs(k, src, hf, parts)) return;
    // GXV_SILH_FUSE: a fused key (FsKey::silhFuse) is always written in float and untrimmed: its tail does byte arithmetic on the colours (vgx_silhfuse.h)
    const bool fuse = k.silhFuse() != 0;
    if (fuse) { hf = false; parts = 0; }
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
    // (GXV_FS_FINCLAMP: the stages whose colour / alpha text ends in a bound at 0.0 (lo) / at 1.0 (hi): clamp(x, 0.0, 1.0) is both)
    bool loC[16] = {}, hiC[16] = {}, loA[16] = {}, hiA[16] = {};
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
    // GXV_TEVIN_CANON=2: in a key that keeps the mark, the A / B / C reads of stage-made values proven in 0..1 lose their wrap (WalkTevIn)
    uint8_t inWrapKeep[16], inWrapInit[16] = {};
    std::fill(inWrapKeep, inWrapKeep + 16, static_cast<uint8_t>(0x3F));
    if ((k.inWrap & 1) && TevInCanonMode() == 2) {
        TevInWalk tw;
        WalkTevIn(k, 0xFu, true, false, tw);
        for (int s = 0; s < 16; ++s) { inWrapKeep[s] = tw.keep[s]; inWrapInit[s] = tw.keepInit[s]; }
    }
    const uint8_t dualRegs = (k.inWrap & 1) && TevInCanonMode() == 2 ? k.dualWrap() : static_cast<uint8_t>(0); // (GXV_TEVIN_PREWRAP)
    // GXV_FS_PROJ: texgens whose texture reads are texture2DProj (projective, a vec3 varying)
    const bool projRead = FsProjMode() != 0;
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
    if (k.fog & kFogVtxBit) {
        o("varying float %s;\n", FogVaryingName()); // (GXV_FOG_BIND: see vgx_shadergen.h)
        NoteFogSlotCollision(false, k.numTexGens, usesCol[0] || (k.regFromVtx & 1), usesCol[1] || (k.regFromVtx & 2));
    }
    o("void main() {\n");
    o("  %s prev = u_tev[0], c0 = %s, c1 = %s, c2 = u_tev[3];\n", T4, (k.regFromVtx & 1) ? "v_col0" : "u_tev[1]", (k.regFromVtx & 2) ? "v_col1" : "u_tev[2]");
    if (dualRegs) { // (GXV_TEVIN_PREWRAP: the wrapped copies, uploaded as they are: BuildTevC)
        const int wb = FsTevBase(k);
        std::string decl;
        static const char* const kWn[4] = {"wprev", "wc0", "wc1", "wc2"};
        for (int r = 0; r < 4; ++r)
            if ((dualRegs >> r) & 1u) decl += std::string(decl.empty() ? "" : ", ") + kWn[r] + " = u_tev[" + std::to_string(wb + r) + "]";
        o("  %s %s;\n", T4, decl.c_str());
    }
    if (hf) o("  hvec4 tex = hvec4(1.0), ras = hvec4(0.0);\n  hvec3 kc = hvec3(1.0);\n  hfloat ka = hfloat(1.0);\n");
    else o("  vec4 tex = vec4(1.0), ras = vec4(0.0);\n  vec3 kc = vec3(1.0);\n  float ka = 1.0;\n");
    // Texture coordinates (normalised).
    // (GXV_FS_PROJ: a projective texgen that only texture2DProj reads needs no uvN)
    uint8_t projOnly = 0;
    if (projRead) {
        projOnly = k.texProjMask;
        for (int s = 0; s < k.numStages; ++s) {
            const FsKey::Stage& st = k.stage[s];
            const bool usesCoord = st.texEnable || st.ind; // (the stages that read Uv(st.texCoord) below)
            if (usesCoord && (st.ind || st.lodBias)) projOnly &= static_cast<uint8_t>(~(1u << (st.texCoord & 7)));
        }
        for (int i = 0; i < k.numIndStages; ++i) projOnly &= static_cast<uint8_t>(~(1u << (k.ind[i].texCoord & 7)));
    }
    for (int i = 0; i < k.numTexGens; ++i) {
        if ((projOnly >> i) & 1u) continue;
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

    // GXV_FS_TEXREUSE (gpucut23): the texture read `tex` already holds (the text of the last statement that wrote it, while nothing
    // else wrote it since) is not made again by a later stage whose read is that very statement: same map, same coordinate variable
    // (uvN, v_texN: written once above; never itc, which an indirect stage recomputes), same LOD-bias uniform, same swap. `tex` keeps
    // the value the repeated read would give bit for bit. Honeyhive's planet shaders read u_tex0 at uv0 in both of their stages.
    // GXV_FS_ATFAST (shader37): the cut-out as one compare / interval test (see vgx_shadergen.h); atEarly: made in the last stage
    const bool atFast = FsAtFastKey(k) && !fuse;
    const bool atEarly = atFast && FsAtEarlyKey(k) && !trimAlpha && !wrapAlpha;
    bool atDone = false;
    // the test's text for the alpha value `av` ("ra" in the last stage, "fin.a" at the end)
    auto AtFastTest = [&](bool late) {
        const char* av = late ? "fin.a" : "ra";
        const int kind = AtFastKind(k.alphaTest);
        char pass[160];
        if (kind == 1) std::snprintf(pass, sizeof pass, "(%s >= u_tev[%d].x)", av, V_ATTHR);
        else if (kind == 2) std::snprintf(pass, sizeof pass, "(%s < u_tev[%d].y)", av, V_ATTHR);
        else std::snprintf(pass, sizeof pass, "(abs(%s - u_tev[%d].x) < u_tev[%d].y)", av, V_ATTHR, V_ATTHR);
        if (k.rejectZero() != 0) o("%s  bool vgx_ok = %s;\n", late ? "" : "  ", pass);
        else o("%s  if (!%s) discard;\n", late ? "" : "  ", pass);
        atDone = true;
    };
    const bool texReuse = FsTexReuseMode() != 0;
    std::string texLast; // (the statement that made `tex`'s value; empty: its value is not a reusable read)
    unsigned texReused = 0;
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
            if (k.indPre() == s + 1 && fmt == 0 && mid >= 1 && mid <= 3 && sw == 0 && tw == 0 && !addPrev && !indPrev) {
                // (GXV_IND_PRECOMP: (uv tsz + M (iv + b)) / tsz written as uv + (M iv) (1 / tsz) + c, c = M b / tsz; IndPreValues)
                o("  {\n");
                if (bt < k.numIndStages) o("    vec3 iv = ind%u;\n", bt);
                else o("    vec3 iv = vec3(0.0);\n");
                o("    vec2 io = vec2(dot(u_tev[%d].xyz, iv), dot(u_tev[%d].xyz, iv));\n", V_INDMTX + (mid - 1) * 2, V_INDMTX + (mid - 1) * 2 + 1);
                o("    itc = %s + io * u_tev[%d].xy + u_tev[%d].zw;\n  }\n", coord.c_str(), V_INDPRE, V_INDPRE);
                coord = "itc";
            } else {
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
        }
        // (GXV_FS_PROJ: a projective texgen read straight from its vec3 varying, divided by the GPU's iterator)
        const bool projHere = projRead && st.texEnable && !st.lodBias && !st.ind && st.texCoord < k.numTexGens &&
                              ((k.texProjMask >> st.texCoord) & 1u) != 0;
        const size_t texAt = o.s.size(); // (GXV_FS_TEXREUSE: where this stage's `tex` statement, if any, begins)
        if (projHere) {
            if (hf) o("  tex = hvec4(texture2DProj(u_tex%d, v_tex%d).%s);\n", st.texMap, st.texCoord, Swizzle(st.texSwap).c_str());
            else o("  tex = texture2DProj(u_tex%d, v_tex%d).%s;\n", st.texMap, st.texCoord, Swizzle(st.texSwap).c_str());
        } else if (hf) {
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
        if (texReuse && o.s.size() > texAt) {
            const std::string rd = o.s.substr(texAt);
            const bool read = st.texEnable && coord != "itc" && rd.find("texture2D") != std::string::npos;
            if (read && rd == texLast) {
                o.s.resize(texAt); // (`tex` holds this read's value already)
                ++texReused;
            } else {
                texLast = read ? rd : std::string();
            }
        }
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
        // (GXV_TEVIN_CANON=2: inWrapKeep[s] says which A / B / C register reads keep the wrap; all of them otherwise)
        auto kw = [&](uint8_t bit) { return (inWrapKeep[s] & bit) != 0; };
        // (GXV_TEVIN_PREWRAP: a kept wrap of a register's INITIAL value whose wrapped copy the block carries reads that row; every other kept wrap is as before)
        auto ki = [&](uint8_t bit, uint32_t reg) { return ((dualRegs >> reg) & 1u) != 0 && (inWrapInit[s] & bit) != 0; };
        auto wc = [&](const std::string& plain, uint32_t reg, bool dual, bool color) { return dual ? "w" + plain : WrapArg(plain, true, color); };
        auto cIn = [&](uint32_t sel, uint8_t bit) -> std::string {
            const std::string plain = ColorArg(sel, T3);
            if (!(w && sel < 8 && kw(bit))) return plain;
            return wc(plain, sel / 2, ki(bit, sel / 2), true);
        };
        auto aIn = [&](uint32_t sel, uint8_t bit) -> std::string {
            const std::string plain = AlphaArg(sel);
            if (!(w && sel < 4 && kw(bit))) return plain;
            return wc(plain, sel, ki(bit, sel), false);
        };
        o("    %s a = %s, b = %s, c = %s, d = %s;\n", T3, cIn(bits(tc, 12, 4), 1).c_str(), cIn(bits(tc, 8, 4), 2).c_str(), cIn(bits(tc, 4, 4), 4).c_str(),
          ColorArg(bits(tc, 0, 4), T3).c_str());
        o("    %s aa = %s, ab = %s, ac = %s, ad = %s;\n", T1, aIn(bits(ta, 13, 3), 8).c_str(), aIn(bits(ta, 10, 3), 16).c_str(), aIn(bits(ta, 7, 3), 32).c_str(),
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
                if (clampOn && act.lo) (color ? loC : loA)[s] = true;
                else if (clampOn && act.hi) (color ? hiC : hiA)[s] = true;
            } else if (clampOn) {
                o("    %s = clamp(%s, %s, %s);\n", res, res, L("0.0").c_str(), L("1.0").c_str());
                (color ? loC : loA)[s] = true;
                (color ? hiC : hiA)[s] = true;
            }
            else o("    %s = clamp(%s, %s, %s);\n", res, res, L("-4.0157").c_str(), L("4.0118").c_str());
        };
        if (atEarly && s == k.numStages - 1) {
            // GXV_FS_ATFAST=2: this stage's alpha, the test, then its colour (the colour does not depend on the stage's own alpha result:
            // the registers are written below, after both)
            Combine(ta, false);
            AtFastTest(false);
            Combine(tc, true);
        } else {
            Combine(tc, true);
            Combine(ta, false);
        }
        o("    %s.rgb = rc;\n    %s.a = ra;\n  }\n", RegName(bits(tc, 22, 2)), RegName(bits(ta, 22, 2)));
    }
    // GXV_FS_TRIM: the final clamp keeps the sides that can act for colour or alpha (one vec4 operation). A wrapped part is 0..1 already:
    // vgx_wrap_final returns a value in 0..1 as it is and makes every other one a byte / 255.
    const bool finLo = !trimFinal || (!wrapColor && ti.finC.lo) || (!wrapAlpha && ti.finA.lo);
    const bool finHi = !trimFinal || (!wrapColor && ti.finC.hi) || (!wrapAlpha && ti.finA.hi);
    if (trim) trimOps += (finLo ? 0u : 1u) + (finHi ? 0u : 1u);
    // GXV_FS_FINCLAMP: the sides each part needs (as above, per part), none for a part already in 0..1 bit for bit
    const bool alphaTestedText = k.alphaTestOn && !(NoDiscard() && AlphaTestAlwaysPasses(k.alphaTest & 7, -1, (k.alphaTest >> 6) & 3, (k.alphaTest >> 3) & 7, -1));
    const int lastS = k.numStages - 1 < 15 ? k.numStages - 1 : 15;
    // (not after vgx_wrap_final: its byte / 255.0 is a multiply by a rounded reciprocal on GPUs, 255 / 255 can come out an ulp above 1)
    // (per side: the last stage's text bounds the part there already)
    const bool deadA = (k.alphaDead() && !alphaTestedText) || (k.dstAlpha && !alphaTestedText);
    const bool doneCLo = !wrapColor && loC[lastS], doneCHi = !wrapColor && hiC[lastS];
    const bool doneALo = deadA || (!wrapAlpha && loA[lastS]), doneAHi = deadA || (!wrapAlpha && hiA[lastS]);
    if (FsFinClampMode() && (doneCLo || doneCHi || doneALo || doneAHi)) {
        bool cLo = !trimFinal || (!wrapColor && ti.finC.lo), cHi = !trimFinal || (!wrapColor && ti.finC.hi);
        bool aLo = !trimFinal || (!wrapAlpha && ti.finA.lo), aHi = !trimFinal || (!wrapAlpha && ti.finA.hi);
        if (doneCLo) cLo = false;
        if (doneCHi) cHi = false;
        if (doneALo) aLo = false;
        if (doneAHi) aHi = false;
        o("  %s fin = %s(%s.rgb, %s.a);\n", (wrapColor || wrapAlpha) ? "vec4" : T4, (wrapColor || wrapAlpha) ? "vec4" : T4, RegName(bits(last.color, 22, 2)),
          RegName(bits(last.alpha, 22, 2)));
        if (wrapColor) o("  fin.rgb = vec3(vgx_wrap_final(fin.r), vgx_wrap_final(fin.g), vgx_wrap_final(fin.b));\n");
        if (wrapAlpha) o("  fin.a = vgx_wrap_final(fin.a);\n");
        const bool w = wrapColor || wrapAlpha;
        const std::string z = w ? "0.0" : L("0.0"), one = w ? "1.0" : L("1.0");
        const char* t3 = w ? "vec3" : T3;
        const char* t1 = w ? "float" : T1;
        if (cLo && cHi) o("  fin.rgb = clamp(fin.rgb, %s, %s);\n", z.c_str(), one.c_str());
        else if (cLo) o("  fin.rgb = max(fin.rgb, %s(0.0));\n", t3);
        else if (cHi) o("  fin.rgb = min(fin.rgb, %s(1.0));\n", t3);
        if (aLo && aHi) o("  fin.a = clamp(fin.a, %s, %s);\n", z.c_str(), one.c_str());
        else if (aLo) o("  fin.a = max(fin.a, %s(0.0));\n", t1);
        else if (aHi) o("  fin.a = min(fin.a, %s(1.0));\n", t1);
    } else if (wrapColor || wrapAlpha) {
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
    bool rejectZero = false, preOnly = false, okDeclared = false;
    // (GXV_PIX_ATPASS, FsKey::atPass: the front end proved the test passes for every fragment of the draws given this key: no test text)
    if (atFast && !atDone) {
        AtFastTest(true); // (GXV_FS_ATFAST=1: one compare / interval; the fin.a here is the clamped alpha as always)
        rejectZero = k.rejectZero() != 0;
        okDeclared = rejectZero;
        preOnly = k.preAlpha() != 0 && !rejectZero;
    }
    if (!atDone && k.alphaTestOn && !k.atPass() && !(NoDiscard() && AlphaTestAlwaysPasses(k.alphaTest & 7, -1, (k.alphaTest >> 6) & 3, (k.alphaTest >> 3) & 7, -1))) {
        rejectZero = k.rejectZero() != 0 || fuse; // (a fused key keeps the test's result in vgx_ok too: a failing fragment writes the texel, not nothing)
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
        if (rejectZero) { o("  bool vgx_ok = %s %s %s;\n", e0, kOp[(k.alphaTest >> 6) & 3], e1); okDeclared = true; }
        else o("  if (!(%s %s %s)) discard;\n", e0, kOp[(k.alphaTest >> 6) & 3], e1);
        preOnly = k.preAlpha() != 0 && !rejectZero; // (GXV_ALPHATEST_PREPASS: nothing after the test: the colour mask is off for this pass)
    }
    if (fuse && !okDeclared) o("  bool vgx_ok = true;\n"); // (no test text: the test passes for every fragment)
    if (preOnly) {
        o("  gl_FragColor = vec4(0.0);\n}\n");
    } else {
    bool fogMix = false; // (GXV_FS_HALF_FOG: the fog mix is made in float at the output: `fogc`)
    if (k.colorDead()) {
        // GXV_FS_NOCOLOR (FsKey::colorDead): the colour is not written (colour mask off) and no blend factor reads it: no fog, colour 0
        o(hf ? "  fin.rgb = hvec3(0.0);\n" : "  fin.rgb = vec3(0.0);\n");
    } else if (k.fog) {
        const int fsel = k.fog & 7;
        if (k.fog & kFogHalfBit) {
            // GXV_FS_HALF_FOG (FogHalfLanes): the factor in float from half-exact lanes (row 11: sInv, c1s, bs hi, bs lo; row 10 w: the constant of a flat factor),
            // clamped in float. The mix with the fog colour is made in float too (below, where the colour leaves the shader), so the half error of the TEV result
            // is not grown by a half mix and a half factor: fogMix
            o("  float fg = clamp((%s * float(u_tev[%d].x) - (float(u_tev[%d].z) + float(u_tev[%d].w))) * float(u_tev[%d].y) + float(u_tev[%d].w), 0.0, 1.0);\n",
              FogVaryingName(), V_FOGHL, V_FOGHL, V_FOGHL, V_FOGHL, V_FOGCOL);
            fogMix = true;
        } else if (k.fog & kFogVtxBit) {
            // (GXV_FOGVTX: linear fog only; the vertex shader's v_fog is affine in clip w / z, u_tev[V_FOG] = c1, -, -, c0: FogVtxCoeffs)
            o("  float fg = clamp(u_tev[%d].x * %s + u_tev[%d].w, 0.0, 1.0);\n", V_FOG, FogVaryingName(), V_FOG);
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
        if (!fogMix) o("  fin.rgb = mix(fin.rgb, u_tev[%d].rgb, fg);\n", V_FOGCOL);
    }
    if (k.dstAlpha) o("  fin.a = u_tev[%d].z;\n", V_MISC);
    else if (k.alphaDead() && !alphaTestedText) o(hf ? "  fin.a = hfloat(0.0);\n" : "  fin.a = 0.0;\n"); // (GXV_FS_ADEAD: the alpha output reaches nothing)
    if (k.logicSrc == 1) o("  fin.rgb = vec3(1.0) - fin.rgb;\n");
    else if (k.logicSrc == 2) o("  fin.rgb = vec3(1.0);\n");
    if (fuse) {
        // GXV_SILH_FUSE (vgx_silhfuse.h): `tex` is the texel the draw-back quad copied (the one stage's sample, identity swizzle: the front end checks); the blend's result for the
        // bytes s (fin.rgb), d (the texel) and a (fin.a), term by term, each non-exact term rounded to nearest as floor((x + 127) / 255); the +0.001 keeps the reciprocal multiply
        // from landing an ulp under a whole number; the written value is (r + 0.25) / 255 for the result byte r (any converter gives r).
        const uint8_t code = k.silhFuse();
        const uint8_t sK = vgx::silhfuse::CodeS(code), dK = vgx::silhfuse::CodeD(code);
        auto factor = [](uint8_t kind) -> const char* {
            switch (kind) {
            case vgx::silhfuse::K_SA: return "vec3(vgx_a)";
            case vgx::silhfuse::K_ISA: return "vec3(255.0 - vgx_a)";
            case vgx::silhfuse::K_SC: return "vgx_s";
            default: return "(vec3(255.0) - vgx_s)";
            }
        };
        auto term = [&](const char* name, const char* x, uint8_t kind) {
            if (kind == vgx::silhfuse::K_ZERO) o("  vec3 %s = vec3(0.0);\n", name);
            else if (kind == vgx::silhfuse::K_ONE) o("  vec3 %s = %s;\n", name, x);
            else o("  vec3 %s = floor((%s * %s + vec3(127.0)) * 0.00392156862745098 + vec3(0.001));\n", name, x, factor(kind));
        };
        o("  vec4 vgx_old = tex;\n");
        if (sK != vgx::silhfuse::K_ZERO || dK == vgx::silhfuse::K_SC || dK == vgx::silhfuse::K_ISC) o("  vec3 vgx_s = floor(fin.rgb * 255.0 + vec3(0.5));\n");
        o("  float vgx_a = floor(fin.a * 255.0 + 0.5);\n");
        o("  vec3 vgx_d = floor(vgx_old.rgb * 255.0 + vec3(0.5));\n");
        term("vgx_ts", "vgx_s", sK);
        term("vgx_td", "vgx_d", dK);
        o("  vec3 vgx_r = clamp(vgx_td %s vgx_ts, 0.0, 255.0);\n", vgx::silhfuse::CodeRev(code) ? "-" : "+");
        const uint8_t am = k.silhFuseAlpha();
        const char* alpha = am == vgx::silhfuse::A_SRC ? "min((vgx_a + 0.25) * 0.00392156862745098, 1.0)" : (am == vgx::silhfuse::A_ZERO ? "0.0" : "vgx_old.a");
        o("  vec4 vgx_new = vec4(min((vgx_r + vec3(0.25)) * 0.00392156862745098, vec3(1.0)), %s);\n", alpha);
        o("  gl_FragColor = vgx_ok ? vgx_new : vgx_old;\n}\n");
    } else if (rejectZero) {
        if (fogMix) o("  vec3 fogc = mix(vec3(fin.rgb), vec3(u_tev[%d].rgb), fg);\n  gl_FragColor = vgx_ok ? vec4(fogc, float(fin.a)) : vec4(0.0);\n}\n", V_FOGCOL);
        else o("  gl_FragColor = vgx_ok ? vec4(fin) : vec4(0.0);\n}\n");
    } else if (fogMix) o("  vec3 fogc = mix(vec3(fin.rgb), vec3(u_tev[%d].rgb), fg);\n  gl_FragColor = vec4(fogc, float(fin.a));\n}\n", V_FOGCOL);
    else o("  gl_FragColor = fin;\n}\n");
    } // (!preOnly)
    src.text = std::move(o.s);
    src.texMask = texMask;
    src.texChan = FsTexChannels(k);
    src.numTexGens = k.numTexGens;
    src.nTev = static_cast<uint8_t>(FsTevCount(k));
    src.nTevCanon = src.nTev;
    src.tevGather = false;
    src.trimOps = static_cast<uint8_t>(trimOps > 255 ? 255 : trimOps);
    src.texReused = static_cast<uint8_t>(texReused > 255 ? 255 : texReused);
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
// GXV_VS_NOBRANCH: a palette / matrix row index clamped without an `if` (min on the whole-numbered float, then the int cast: the same
// integer as `int i = int(floor(w + 0.5)); if (i > m) i = m;`). Only for the default index form (GXV_PALIDX=1); the others keep the old text.
void RowIndexClamped(Out& o, const char* ind, const char* var, const char* w, int maxv) {
    if (VsNoBranchMode() && PalIndexMode() == 1) {
        o("%sint %s = int(min(floor(%s + 0.5), %d.0));\n", ind, var, w, maxv);
        return;
    }
    RowIndex(o, ind, var, w);
    o("%sif (%s > %d) %s = %d;\n", ind, var, maxv, var, maxv);
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
long VsLtSkipMode() {
    static const long m = EnvInt("GXV_VS_LTSKIP", 0) == 1 ? 1 : 0;
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

// ---- GXV_IND_PRECOMP (gpu23b; gpuwater's item 2.1): an indirect stage's coordinate with the texture size folded in per draw
// The generator writes an indirect stage (8-bit offsets, a static matrix, no wrap) per pixel as
//     tcx = uv * tsz;  iv = ind + b;  io = (M0 . iv, M1 . iv);  itc = (tcx + io) / tsz
// (the offsets are in texels of the stage's texture). With the switch the marked stage is written
//     io = (M0 . ind, M1 . ind);  itc = uv + io * (1 / tsz) + c,   c = (M0 . b, M1 . b) / tsz
// with (1 / tsz, c) per draw in u_tev[V_INDPRE] (BuildTevC through IndPreValues): one vector multiply, the bias add and the per-pixel
// divide (a reciprocal and a multiply on the SGX) gone - Beach Bowl's sea (fs 304) computes this for 0.5-0.75 of the screen. The
// arithmetic is the same up to rounding: the coordinate differs from the old one by about one float step (1e-7 of the texture, under
// 1e-4 texel at 960x544), which can move a bilinear weight by one 1/256 step in rare pixels: a MINOR CHANGE (sea pixels off by at
// most one level here and there). Only the first such stage of a key (one vec4 is free in u_tev: [27]), never with an add-previous
// stage in the key. GXV_IND_PRECOMP=0 (default): every key and text as before.
long IndPreMode() {
    static const long m = EnvInt("GXV_IND_PRECOMP", 0) == 1 ? 1 : 0;
    return m;
}
bool IndPreMark(FsKey& fs) {
    fs.setIndPre(0);
    if (!IndPreMode() || !fs.numIndStages) return false;
    for (int s = 0; s < fs.numStages; ++s)
        if (fs.stage[s].ind && bits(fs.stage[s].ind, 20, 1)) return false; // (an add-previous stage: left alone)
    for (int s = 0; s < fs.numStages && s < 16; ++s) {
        const uint32_t ind = fs.stage[s].ind;
        if (!ind) continue;
        const uint32_t fmt = bits(ind, 2, 2), mid = bits(ind, 9, 4), sw = bits(ind, 13, 3), tw = bits(ind, 16, 3);
        if (fmt == 0 && mid >= 1 && mid <= 3 && sw == 0 && tw == 0) {
            fs.setIndPre(static_cast<uint8_t>(s + 1));
            return true;
        }
    }
    return false;
}
void IndPreValues(const FsKey& fs, const float* indMtx, float texW, float texH, float out[4]) {
    out[0] = out[1] = 1.0f;
    out[2] = out[3] = 0.0f;
    const int s = fs.indPre() - 1;
    if (s < 0 || s >= 16) return;
    const uint32_t ind = fs.stage[s].ind, bias = bits(ind, 4, 3), mid = bits(ind, 9, 4);
    if (mid < 1 || mid > 3) return;
    const double w = texW > 0 ? texW : 1.0, h = texH > 0 ? texH : 1.0;
    const float* m0 = indMtx + (mid - 1) * 8;
    const float* m1 = m0 + 4;
    double c0 = 0, c1 = 0;
    for (int i = 0; i < 3; ++i)
        if (bias & (1u << i)) { c0 += -128.0 * m0[i]; c1 += -128.0 * m1[i]; }
    out[0] = static_cast<float>(1.0 / w);
    out[1] = static_cast<float>(1.0 / h);
    out[2] = static_cast<float>(c0 / w);
    out[3] = static_cast<float>(c1 / h);
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
        if (VsNoBranchLightMode()) o("      float ca = step(0.0, dot(n, ld)) * max(0.0, dot(n, u_lt[%d].xyz));\n", b + 1); // (GXV_VS_NOBRANCH_LIGHT: the select as a product with 0 or 1)
        else o("      float ca = dot(n, ld) >= 0.0 ? max(0.0, dot(n, u_lt[%d].xyz)) : 0.0;\n", b + 1);
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
    uint8_t ltLoop[6] = {}; // (GXV_VTX_CENSUS: ShaderSource::ltLoop)
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
    if (k.fogVtx & 1) {
        o("varying float %s;\n", FogVaryingName()); // (GXV_FOG_BIND: see vgx_shadergen.h)
        NoteFogSlotCollision(true, k.numTexGens, !outGone[0], !outGone[1]);
    }
    o("void main() {\n  vec4 p = vec4(a_pos.xyz, 1.0);\n");
    if (k.perVtxPos) {
        RowIndexClamped(o, "  ", "pi", "a_pos.w", shortPal ? palK - 3 : 61);
        o("  vec3 eye = vec3(dot(u_pal[pi], p), dot(u_pal[pi + 1], p), dot(u_pal[pi + 2], p));\n");
        if (needNrm) {
            if (shortPal) o("  int ni = %d + pi;\n", palK); // (position rows 0..K-1, then normal rows 0..K-1)
            else if (VsNoBranchMode() && PalIndexMode() == 1) o("  int ni = 64 + int(min(floor(a_pos.w + 0.5), 29.0));\n"); // (GXV_VS_NOBRANCH: 64 + min(pi, 29), pi = min(w, 61))
            else o("  int ni = pi > 29 ? 93 : 64 + pi;\n");
            o("  vec3 n = vec3(dot(u_pal[ni].xyz, a_nrm), dot(u_pal[ni + 1].xyz, a_nrm), dot(u_pal[ni + 2].xyz, a_nrm));\n");
        }
    } else {
        o("  vec3 eye = vec3(dot(u_mv[0], p), dot(u_mv[1], p), dot(u_mv[2], p));\n");
        if (needNrm) o("  vec3 n = vec3(dot(u_mv[3].xyz, a_nrm), dot(u_mv[4].xyz, a_nrm), dot(u_mv[5].xyz, a_nrm));\n");
    }
    if (needNrm && VsNoBranchMode()) o("  float nl = length(n);\n  n = n / (nl + step(nl, 0.0));\n"); // (GXV_VS_NOBRANCH: a division by exactly 1.0 for a zero normal)
    else if (needNrm) o("  float nl = length(n);\n  if (nl > 0.0) n = n / nl;\n");
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
            RowIndexClamped(o, "  ", "pj", w2, shortPal ? palK - 3 : 61);
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
    if (k.fogVtx & 1) o("  %s = gl_Position.%c;\n", FogVaryingName(), (k.fogVtx & 2) ? 'z' : 'w');

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
                    ltLoop[ch * 3 + 2] |= static_cast<uint8_t>(1u << l);
                    if (k.ltSkip()) o("    if (any(notEqual(u_lt[%d], vec4(0.0)))) {\n", b + 2); // (GXV_VS_LTSKIP: a black light adds 0)
                    else o("    {\n");
                    o("      vec3 ld = u_lt[%d].xyz - eye;\n      float d2 = dot(ld, ld);\n      float d1 = sqrt(d2);\n"
                      "%s      float at = 1.0;\n", b, VsNoBranchLightMode() ? "      ld = ld / (d1 + step(d1, 0.0));\n" : "      if (d1 > 0.0) ld = ld / d1;\n");
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
                    ltLoop[ch * 3 + part] |= static_cast<uint8_t>(1u << l);
                    if (k.ltSkip()) { // (GXV_VS_LTSKIP: a black light adds 0 to this part)
                        if (part) o("    if (u_lt[%d].a != 0.0) {\n", b + 2);
                        else o("    if (any(notEqual(u_lt[%d].rgb, vec3(0.0)))) {\n", b + 2);
                    }
                    else o("    {\n");
                    o("      vec3 ld = u_lt[%d].xyz - eye;\n      float d2 = dot(ld, ld);\n      float d1 = sqrt(d2);\n"
                      "%s      float at = 1.0;\n", b, VsNoBranchLightMode() ? "      ld = ld / (d1 + step(d1, 0.0));\n" : "      if (d1 > 0.0) ld = ld / d1;\n");
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
            RowIndexClamped(o, "    ", "ti", tmi, 61);
            if (t.proj) o("    vec3 tc = vec3(dot(u_pal[ti], srcv), dot(u_pal[ti + 1], srcv), dot(u_pal[ti + 2], srcv));\n");
            else o("    vec3 tc = vec3(dot(u_pal[ti], srcv), dot(u_pal[ti + 1], srcv), 1.0);\n");
        } else {
            if (t.proj) o("    vec3 tc = vec3(dot(u_tm[%d], srcv), dot(u_tm[%d], srcv), dot(u_tm[%d], srcv));\n", i * 3, i * 3 + 1, i * 3 + 2);
            else o("    vec3 tc = vec3(dot(u_tm[%d], srcv), dot(u_tm[%d], srcv), 1.0);\n", i * 3, i * 3 + 1);
        }
        if (t.post) {
            if (t.post == 2) {
                if (VsNoBranchMode()) o("    float tl = length(tc);\n    tc = tc / (tl + step(tl, 0.0));\n");
                else o("    float tl = length(tc);\n    if (tl > 0.0) tc = tc / tl;\n");
            }
            o("    vec4 q = vec4(tc, 1.0);\n    tc = vec3(dot(u_ptm[%d], q), dot(u_ptm[%d], q), dot(u_ptm[%d], q));\n", i * 3, i * 3 + 1, i * 3 + 2);
        }
        // (GXV_FS_PROJ: the fragment shader's q == 0 guard, per vertex: the GPU divides by the interpolated q of a texture2DProj read)
        if (t.proj && !texV2(i) && FsProjMode()) {
            if (VsNoBranchMode()) o("    tc.z += step(abs(tc.z), 0.0);\n"); // (GXV_VS_NOBRANCH: z == 0 becomes 0 + 1)
            else o("    if (tc.z == 0.0) tc.z = 1.0;\n");
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
    std::memcpy(src.ltLoop, ltLoop, sizeof ltLoop);
    {
        // (GXV_VSOUT census, GXV_GPU_SCENES' line)
        uint8_t pr = static_cast<uint8_t>((outGone[0] ? 1u : 0u) | (outGone[1] ? 2u : 0u) | (texOff ? 8u : 0u));
        for (int ch = 0; ch < 2; ++ch)
            if (!outGone[ch] && (!partOn[ch][0] || !partOn[ch][1])) pr |= 4u;
        src.outPruned = pr;
    }
}


// ---- pix23: FsValueEval (see vgx_shadergen.h) ------------------------------------------------------------------------------------------
namespace {
struct PvRegs { Ab rgb, a; };
// An input byte value v / 255 (a register, a konst colour): 0 and 255 are exact (255.0 / 255.0 is 1.0 in float and half), the rest is
// rounded to half once (a float shader rounds less).
inline Ab PvByte(int v) {
    if (v == 0) return AbExact(0.0);
    if (v == 255) return AbExact(1.0);
    return AbIn(v / 255.0, v / 255.0);
}
// The hull of several values (an rgb read through a swizzle): exactly 0 only when all are.
inline Ab PvHull(const Ab* v, int n) {
    Ab r = v[0];
    for (int i = 1; i < n; ++i) {
        if (r.zero() && v[i].zero()) continue;
        r = Ab{std::min(r.lo, v[i].lo), std::max(r.hi, v[i].hi), std::max(r.e, v[i].e)};
    }
    return r;
}
// mix(a, b, c) as the generator writes it, with the cases that are exact in both code forms (a + c * (b - a) and a * (1 - c) + b * c):
// a = b = 0 gives 0; c = 0 gives a (0 when a is); c = 1 gives b when b is 0 (a - a = 0 exactly, and a * 0 + 0 * 1 = 0).
inline Ab PvMix(const Ab& a, const Ab& b, const Ab& c) {
    if (a.zero() && b.zero()) return AbExact(0.0);
    if (c.zero() && a.zero()) return AbExact(0.0);
    if (c.one() && b.zero()) return AbExact(0.0);
    if (c.zero() && a.one()) return AbExact(1.0);
    return AbMix(a, b, c);
}
inline Ab PvWiden(Ab v, double e) {
    if (v.zero()) return v;
    v.e += e;
    return v;
}
} // namespace

void FsValueEval(const FsKey& k, const FsValueIn& in, FsValueOut& out) {
    out = FsValueOut{};
    if ((k.inWrap & 1) || k.numStages < 1 || k.numStages > 16) return; // (wrapped register reads: not followed)
    const Ab zero = AbExact(0.0), one = AbExact(1.0);
    constexpr double kFilterE = 1.0 / 255.0; // (a filtered texture value: a convex combination of texels, to within a level)
    constexpr double kVaryE = 1.0 / 512.0;   // (an interpolated varying whose vertices carry the same value)
    PvRegs reg[4];
    for (int r = 0; r < 4; ++r) {
        Ab c[3];
        for (int i = 0; i < 3; ++i) c[i] = PvByte(in.reg[r][i]);
        reg[r].rgb = PvHull(c, 3);
        reg[r].a = PvByte(in.reg[r][3]);
    }
    // (GXV_FOLDREGS, FsKey::regFromVtx: c0 / c1 come from v_col0 / v_col1, which carry the registers' values per vertex)
    for (int r = 1; r <= 2; ++r)
        if (k.regFromVtx & (1u << (r - 1))) {
            reg[r].rgb = PvWiden(reg[r].rgb, kVaryE);
            reg[r].a = PvWiden(reg[r].a, kVaryE);
        }
    auto TexChan = [&](int m, int ch) -> Ab {
        m &= 7;
        if (!in.texKnown[m]) return PvWiden(AbIn(0.0, 1.0), kFilterE);
        const int lo = in.texLo[m][ch], hi = in.texHi[m][ch];
        if (hi == 0) return zero; // (every texel 0: any weighted sum of them is 0)
        return PvWiden(AbIn(lo / 255.0, hi / 255.0), kFilterE);
    };
    auto RasComp = [&](int ch, int comp) -> Ab {
        if (in.rasZero[ch][comp]) return zero;
        return PvWiden(AbIn(in.rasLo[ch][comp], in.rasHi[ch][comp]), kVaryE);
    };
    PvRegs tex{one, one}, ras{zero, zero};
    Ab kc = one, ka = one;
    for (int s = 0; s < k.numStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        if (st.texEnable) {
            Ab c[4];
            for (int i = 0; i < 4; ++i) c[i] = TexChan(st.texMap, (st.texSwap >> (2 * i)) & 3);
            tex = {PvHull(c, 3), c[3]};
        } else if (st.texOff) {
            tex = {one, one};
        }
        if (st.rasChan < 2) {
            Ab c[4];
            for (int i = 0; i < 4; ++i) c[i] = RasComp(st.rasChan, (st.rasSwap >> (2 * i)) & 3);
            ras = {PvHull(c, 3), c[3]};
        } else {
            ras = {zero, zero};
        }
        const uint32_t tc = st.color, ta = st.alpha;
        bool kcUsed = false, kaUsed = false;
        for (int i = 0; i < 4; ++i) {
            if (bits(tc, i * 4, 4) == 14) kcUsed = true;
            if (bits(ta, 4 + i * 3, 3) == 6) kaUsed = true;
        }
        if (kcUsed) {
            const uint8_t sel = st.kcsel;
            if (sel < 8) kc = AbExact((8 - sel) / 8.0);
            else if (sel < 12) kc = zero;
            else {
                const int kk = sel & 3, comp = (sel - 12) >> 2; // 0 rgb, 1 rrr, 2 ggg, 3 bbb, 4 aaa
                Ab c[3];
                for (int i = 0; i < 3; ++i) c[i] = PvByte(in.konst[kk][i] & 0xFF);
                kc = comp == 0 ? PvHull(c, 3) : comp <= 3 ? c[comp - 1] : PvByte(in.konst[kk][3] & 0xFF);
            }
        }
        if (kaUsed) {
            const uint8_t sel = st.kasel;
            if (sel < 8) ka = AbExact((8 - sel) / 8.0);
            else if (sel < 16) ka = zero;
            else ka = PvByte(in.konst[sel & 3][((sel - 16) >> 2) & 3] & 0xFF);
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
        // (the clamps as GenerateFragmentShader writes them without GXV_FS_TRIM: a trimmed text leaves out only sides proven not to act,
        // so the value is the same)
        auto Combine = [&](uint32_t r32, const Ab& A, const Ab& B, const Ab& C, const Ab& D) -> Ab {
            const uint32_t bias = bits(r32, 16, 2), op = bits(r32, 18, 1), clampOn = bits(r32, 19, 1), scale = bits(r32, 20, 2);
            Ab r;
            if (bias != 3) {
                r = PvMix(A, B, C);
                r = op ? AbSub(D, r) : AbAdd(D, r);
                if (bias == 1) r = AbAdd(r, AbExact(0.5));
                else if (bias == 2) r = AbSub(r, AbExact(0.5));
                static const double kScale[4] = {1.0, 2.0, 4.0, 0.5};
                r = AbScale(r, kScale[scale]);
            } else {
                // (compare modes: d + (a OP b ? c : 0), per component or for the whole colour: between d and d + c)
                Ab sel = C.zero() ? zero : Ab{std::min(0.0, C.lo), std::max(0.0, C.hi), C.e};
                r = AbAdd(D, sel);
            }
            const double lo = clampOn ? 0.0 : -4.0157, hi = clampOn ? 1.0 : 4.0118;
            r = AbClamp(r, lo, hi);
            if (!clampOn && !r.zero()) r.e += 1e-4; // (the half text's -4.015625 / 4.01171875)
            return r;
        };
        const Ab cr = Combine(tc, ColorArg(bits(tc, 12, 4)), ColorArg(bits(tc, 8, 4)), ColorArg(bits(tc, 4, 4)), ColorArg(bits(tc, 0, 4)));
        const Ab ar = Combine(ta, AlphaArg(bits(ta, 13, 3)), AlphaArg(bits(ta, 10, 3)), AlphaArg(bits(ta, 7, 3)), AlphaArg(bits(ta, 4, 3)));
        reg[bits(tc, 22, 2)].rgb = cr;
        reg[bits(ta, 22, 2)].a = ar;
    }
    const FsKey::Stage& last = k.stage[k.numStages - 1];
    Ab fr = reg[bits(last.color, 22, 2)].rgb, fa = reg[bits(last.alpha, 22, 2)].a;
    // (the final wrap, GXV_TEVWRAP: a value in 0..1 is returned as it is, anything else becomes some byte / 255)
    bool wrapColor = false, wrapAlpha = false;
    FinalWrapPlan(k, wrapColor, wrapAlpha);
    auto Wrap = [&](const Ab& v) { return (v.lo - v.e >= 0.0 && v.hi + v.e <= 1.0) ? v : AbIn(0.0, 1.0); };
    if (wrapColor) fr = Wrap(fr);
    if (wrapAlpha) fa = Wrap(fa);
    fr = AbClamp(fr, 0.0, 1.0);
    fa = AbClamp(fa, 0.0, 1.0);
    out.alphaLo = static_cast<float>(std::max(0.0, fa.lo - fa.e));
    out.alphaHi = static_cast<float>(std::min(1.0, fa.hi + fa.e));
    // The alpha test: ai = floor(alpha * 255 + 0.5) (in float), compared with whole-number references. Every whole ai the alpha can give.
    if (FsHasAlphaTestText(k)) {
        auto Cmp = [](uint32_t c, int ai, int ref) {
            switch (c & 7) {
            case 0: return false;
            case 1: return ai < ref;
            case 2: return ai == ref;
            case 3: return ai <= ref;
            case 4: return ai > ref;
            case 5: return ai != ref;
            case 6: return ai >= ref;
            default: return true;
            }
        };
        // (e: the error bound taken in: the full one for `test`, none for `testRaw`, the census's "how near is it")
        auto Decide = [&](double e) -> int {
            int aiLo = 0, aiHi = 0;
            if (!fa.zero()) {
                aiLo = static_cast<int>(std::floor((fa.lo - e) * 255.0 + 0.5 - 1e-3));
                aiHi = static_cast<int>(std::floor((fa.hi + e) * 255.0 + 0.5 + 1e-3));
            }
            aiLo = std::max(aiLo, 0);
            aiHi = std::min(aiHi, 255);
            bool anyT = false, anyF = false;
            for (int ai = aiLo; ai <= aiHi && !(anyT && anyF); ++ai) {
                const bool x = Cmp(k.alphaTest & 7, ai, in.ref0), y = Cmp((k.alphaTest >> 3) & 7, ai, in.ref1);
                bool r;
                switch ((k.alphaTest >> 6) & 3) {
                case 0: r = x && y; break;
                case 1: r = x || y; break;
                case 2: r = x != y; break;
                default: r = x == y; break;
                }
                (r ? anyT : anyF) = true;
            }
            return !anyF ? 1 : !anyT ? 0 : -1;
        };
        out.test = Decide(fa.e);
        out.testRaw = Decide(0.0);
    }
    // What gl_FragColor gets (after the test; a rejectZero key writes vec4(0) for a failing fragment, which is 0 as well).
    if (k.fog) {
        // (mix(fin.rgb, fog colour, fg) with fg in 0..1: 0 only when both are)
        const bool fogZero = in.fogColor[0] == 0 && in.fogColor[1] == 0 && in.fogColor[2] == 0;
        fr = (fr.zero() && fogZero) ? zero : AbIn(0.0, 1.0);
    }
    if (k.logicSrc) fr = k.logicSrc == 2 ? one : AbIn(0.0, 1.0);
    Ab ao = fa;
    if (k.dstAlpha) ao = PvByte(in.dstAlpha & 0xFF);
    else if (k.alphaDead() && !(k.alphaTestOn && !(NoDiscard() && AlphaTestAlwaysPasses(k.alphaTest & 7, -1, (k.alphaTest >> 6) & 3, (k.alphaTest >> 3) & 7, -1)))) ao = zero;
    out.colorZero = fr.zero();
    out.alphaZero = ao.zero();
    // gpu960: the alpha the blend factors read (ao: a dst-alpha constant key outputs its byte, an alpha-dead key zero, else the shader's)
    out.alphaLoOut = static_cast<float>(std::max(0.0, ao.lo - ao.e));
    // (AbClamp: a value that is >= 1 in every case, error bound included, leaves the clamp as an exact 1: lo = hi = 1, e = 0. Texture and rasterised
    // values carry the filter / interpolation error PvWiden adds, so a textured alpha is never "exactly 1" unless the sum with registers is above 1)
    out.alphaOne = ao.one();
    if (out.alphaOne) out.alphaLoOut = 1.0f;
    out.ok = true;
}

} // namespace vgx
