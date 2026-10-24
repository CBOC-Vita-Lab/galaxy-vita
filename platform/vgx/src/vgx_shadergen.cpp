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

#include <cstdarg>

namespace vgx {

uint64_t HashKey(const void* key, size_t size) { return XXH3_64bits(key, size); }

bool UniformPacking() {
    static const bool on = EnvInt("GXV_UNIPACK", 1) != 0;
    return on;
}
int FsTevCount(const FsKey& k) {
    if (!UniformPacking() || k.numIndStages) return 28;
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
enum { V_REG = 0, V_KONST = 4, V_MISC = 8, V_FOG = 9, V_FOGCOL = 10, V_INDMTX = 11, V_INDSCALE = 17, V_TEXSIZE = 21 };

const char* ColorArg(uint32_t a) {
    static const char* const t[16] = {"prev.rgb", "prev.aaa", "c0.rgb", "c0.aaa", "c1.rgb", "c1.aaa", "c2.rgb", "c2.aaa",
                                      "tex.rgb", "tex.aaa", "ras.rgb", "ras.aaa", "vec3(1.0)", "vec3(0.5)", "kc", "vec3(0.0)"};
    return t[a & 15];
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

} // namespace

void GenerateFragmentShader(const FsKey& k, ShaderSource& src) {
    Out o;
    uint8_t texMask = 0;
    bool usesCol[2] = {false, false};
    for (int s = 0; s < k.numStages; ++s) {
        if (k.stage[s].texEnable) texMask |= static_cast<uint8_t>(1u << k.stage[s].texMap);
        if (k.stage[s].rasChan < 2) usesCol[k.stage[s].rasChan] = true;
    }
    for (int i = 0; i < k.numIndStages; ++i) texMask |= static_cast<uint8_t>(1u << k.ind[i].texMap);

    o("#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n");
    for (int m = 0; m < 8; ++m)
        if (texMask & (1u << m)) o("uniform sampler2D u_tex%d;\n", m);
    o("uniform vec4 u_tev[%d];\n", FsTevCount(k));
    if (usesCol[0] || (k.regFromVtx & 1)) o("varying vec4 v_col0;\n");
    if (usesCol[1] || (k.regFromVtx & 2)) o("varying vec4 v_col1;\n");
    for (int i = 0; i < k.numTexGens; ++i) o("varying vec3 v_tex%d;\n", i);
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
    if (k.numIndStages) o("  vec2 itc = vec2(0.0);\n");

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
            if (addPrev) o("    itc = tcx + io + itc;\n");
            else o("    itc = tcx + io;\n");
            o("    itc = itc / tsz;\n  }\n");
            coord = "itc";
        }
        if (st.texEnable) o("  tex = texture2D(u_tex%d, %s).%s;\n", st.texMap, coord.c_str(), Swizzle(st.texSwap).c_str());
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
        o("    vec3 a = %s, b = %s, c = %s, d = %s;\n", ColorArg(bits(tc, 12, 4)), ColorArg(bits(tc, 8, 4)), ColorArg(bits(tc, 4, 4)),
          ColorArg(bits(tc, 0, 4)));
        o("    float aa = %s, ab = %s, ac = %s, ad = %s;\n", AlphaArg(bits(ta, 13, 3)), AlphaArg(bits(ta, 10, 3)), AlphaArg(bits(ta, 7, 3)),
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
    const FsKey::Stage& last = k.stage[k.numStages - 1];
    o("  vec4 fin = clamp(vec4(%s.rgb, %s.a), 0.0, 1.0);\n", RegName(bits(last.color, 22, 2)), RegName(bits(last.alpha, 22, 2)));
    if (k.alphaTestOn) {
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
        if (k.fog & 8) o("  float fg = u_tev[%d].x * gl_FragCoord.z;\n", V_FOG);
        else o("  float fg = u_tev[%d].x / max(u_tev[%d].y - gl_FragCoord.z, 0.000001);\n", V_FOG, V_FOG);
        o("  fg = clamp(fg - u_tev[%d].z, 0.0, 1.0);\n", V_FOG);
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
    o("uniform mat4 u_proj;\n");
    const bool shortPal = k.perVtxPos == 2;
    if (needPal) o("uniform vec4 u_pal[%d];\n", shortPal ? kPalRowsShort : kPalRowsFull);
    if (!k.perVtxPos) o("uniform vec4 u_mv[6];\n");
    if (needTm) o("uniform vec4 u_tm[%d];\n", k.numTexGens * 3);
    if (needPtm) o("uniform vec4 u_ptm[%d];\n", k.numTexGens * 3);
    o("uniform vec4 u_lt[%d];\n", 4 + (maxLight + 1) * 5);
    o("varying vec4 v_col0;\nvarying vec4 v_col1;\n");
    for (int i = 0; i < k.numTexGens; ++i) o("varying vec3 v_tex%d;\n", i);
    o("void main() {\n  vec4 p = vec4(a_pos.xyz, 1.0);\n");
    if (k.perVtxPos) {
        RowIndex(o, "  ", "pi", "a_pos.w");
        if (shortPal) o("  if (pi > %d) pi = %d;\n", kPalShortMaxRow, kPalShortMaxRow);
        else o("  if (pi > 61) pi = 61;\n");
        o("  vec3 eye = vec3(dot(u_pal[pi], p), dot(u_pal[pi + 1], p), dot(u_pal[pi + 2], p));\n");
        if (needNrm) {
            if (shortPal) o("  int ni = 30 + pi;\n"); // (position rows 0..29, then normal rows 0..29)
            else o("  int ni = pi > 29 ? 93 : 64 + pi;\n");
            o("  vec3 n = vec3(dot(u_pal[ni].xyz, a_nrm), dot(u_pal[ni + 1].xyz, a_nrm), dot(u_pal[ni + 2].xyz, a_nrm));\n");
        }
    } else {
        o("  vec3 eye = vec3(dot(u_mv[0], p), dot(u_mv[1], p), dot(u_mv[2], p));\n");
        if (needNrm) o("  vec3 n = vec3(dot(u_mv[3].xyz, a_nrm), dot(u_mv[4].xyz, a_nrm), dot(u_mv[5].xyz, a_nrm));\n");
    }
    if (needNrm) o("  float nl = length(n);\n  if (nl > 0.0) n = n / nl;\n");
    o("  gl_Position = u_proj * vec4(eye, 1.0);\n");

    // Lighting channels.
    for (int ch = 0; ch < 2; ++ch) {
        if (ch >= k.numChans) {
            o("  vec4 col%d = a_col%d;\n", ch, ch);
            continue;
        }
        o("  vec4 col%d;\n  {\n", ch);
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
    o("}\n");
    src.text = std::move(o.s);
    src.usesPalette = needPal;
    src.usesLights = needLights;
    src.numTexGens = k.numTexGens;
    src.nMv = k.perVtxPos ? 0 : 6;
    src.nTm = needTm ? static_cast<uint8_t>(k.numTexGens * 3) : 0;
    src.nPtm = needPtm ? static_cast<uint8_t>(k.numTexGens * 3) : 0;
    src.nPal = needPal ? static_cast<uint8_t>(shortPal ? kPalRowsShort : kPalRowsFull) : 0;
    src.nLt = static_cast<uint8_t>(4 + (maxLight + 1) * 5);
}

} // namespace vgx
