// asyncsh31: GXV_UBERSHADER stage 1 (vgx_ubershader.h has the what and why). The formulas below are those of GenerateFragmentShaderAs
// (vgx_shadergen.cpp), written once with the key's choices read from u_ucfg instead of being spelled into the text. Every choice is a select
// (an if on a uniform), never a weighted sum, so a value the key does not select cannot change one it does. Keep the two in step: a change to the
// generator's arithmetic must be made here too, and tools/test_vgx_ubershader_gl.sh fails until it is.
#include "vgx_ubershader.h"
#include "vgx_common.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace vgx {
namespace {
inline uint32_t Bits(uint32_t v, int s, int n) { return (v >> s) & ((1u << n) - 1u); }
// u_tev rows (as vgx_shadergen.cpp's V_*)
enum { R_KONST = 4, R_MISC = 8, R_FOG = 9, R_FOGCOL = 10, R_INDMTX = 11, R_INDSCALE = 17, R_TEXSIZE = 21, R_LODBIAS = 25 };
struct Out {
    std::string s;
    void operator()(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[2048];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        s += buf;
    }
};
} // namespace


bool UberFsConfig(const FsKey& k, float* cfg) {
    if (!UberFsCovers(k)) return false;
    std::fill(cfg, cfg + 4 * kUberRows, 0.0f);
    FsUberPlan p;
    FsUberPlanOf(k, p);
    auto row = [&](int r) { return cfg + 4 * r; };
    auto set = [&](int r, float a, float b, float c, float d) { float* v = row(r); v[0] = a; v[1] = b; v[2] = c; v[3] = d; };
    const FsKey::Stage& last = k.stage[k.numStages - 1];
    set(0, k.numStages, p.wrapIn ? 1.0f : 0.0f, (k.regFromVtx & 1) ? 1.0f : 0.0f, (k.regFromVtx & 2) ? 1.0f : 0.0f);
    for (int i = 0; i < 8; ++i) row(1 + i / 4)[i & 3] = (k.texProjMask >> i) & 1u ? 1.0f : 0.0f;
    set(3, k.numIndStages, p.indPrev ? 1.0f : 0.0f, p.wrapColor ? 1.0f : 0.0f, p.wrapAlpha ? 1.0f : 0.0f);
    for (int i = 0; i < 4; ++i) {
        row(4)[i] = k.ind[i].texMap & 7;
        row(5)[i] = k.ind[i].texCoord < k.numTexGens ? static_cast<float>(k.ind[i].texCoord) : 8.0f;
        row(6)[i] = (k.indVs(i) && k.ind[i].texCoord < k.numTexGens) ? 1.0f : 0.0f;
    }
    set(7, p.testText ? 1.0f : 0.0f, k.alphaTest & 7, (k.alphaTest >> 3) & 7, (k.alphaTest >> 6) & 3);
    set(8, p.rejectZero ? 1.0f : 0.0f, p.preOnly ? 1.0f : 0.0f, p.alphaZero ? 1.0f : 0.0f, k.logicSrc);
    set(9, k.fog ? 1.0f : 0.0f, k.fog & 7, (k.fog & kFogVtxBit) ? 1.0f : 0.0f, (k.fog & 8) ? 1.0f : 0.0f);
    set(10, k.dstAlpha ? 1.0f : 0.0f, Bits(last.color, 22, 2), Bits(last.alpha, 22, 2), 0.0f);
    for (int s = 0; s < k.numStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        const int b = kUberGlobalRows + s * kUberStageRows;
        const float texMode = st.texEnable ? (st.lodBias ? 3.0f : 1.0f) : (st.texOff ? 2.0f : 0.0f);
        // (the texgen the stage reads; an indirect stage starts from it too and then reads its computed itc)
        const float coord = st.texCoord < k.numTexGens ? static_cast<float>(st.texCoord) : 8.0f;
        set(b + 0, texMode, st.texMap & 7, coord, st.rasChan < 2 ? st.rasChan : 2.0f);
        for (int i = 0; i < 4; ++i) {
            row(b + 1)[i] = (st.texSwap >> (2 * i)) & 3u;
            row(b + 2)[i] = (st.rasSwap >> (2 * i)) & 3u;
        }
        set(b + 3, Bits(st.color, 12, 4), Bits(st.color, 8, 4), Bits(st.color, 4, 4), Bits(st.color, 0, 4));
        set(b + 4, Bits(st.alpha, 13, 3), Bits(st.alpha, 10, 3), Bits(st.alpha, 7, 3), Bits(st.alpha, 4, 3));
        auto op = [&](int r, uint32_t reg) {
            const uint32_t bias = Bits(reg, 16, 2), sub = Bits(reg, 18, 1), scale = Bits(reg, 20, 2);
            static const float kBias[3] = {0.0f, 0.5f, -0.5f}, kScale[4] = {1.0f, 2.0f, 4.0f, 0.5f};
            if (bias == 3) set(r, 1.0f + scale, sub, 0.0f, 1.0f);       // compare: mode 1 + width, eq flag
            else set(r, 0.0f, sub ? -1.0f : 1.0f, kBias[bias], kScale[scale]);
        };
        op(b + 5, st.color);
        op(b + 6, st.alpha);
        set(b + 7, Bits(st.color, 19, 1), Bits(st.alpha, 19, 1), Bits(st.color, 22, 2), Bits(st.alpha, 22, 2));
        const uint8_t keep = p.wrapIn ? p.wrapKeep[s] : 0;
        // konst: a non-negative constant, or -(1 + 8 * k + component) for a u_tev konst row (colour: 0 rgb 1 rrr 2 ggg 3 bbb 4 aaa; alpha: 0 r 1 g 2 b 3 a)
        const float kc = st.kcsel < 8 ? (8 - st.kcsel) / 8.0f : st.kcsel < 12 ? 0.0f : -(1.0f + 8.0f * (st.kcsel & 3) + ((st.kcsel - 12) >> 2));
        const float ka = st.kasel < 8 ? (8 - st.kasel) / 8.0f : st.kasel < 16 ? 0.0f : -(1.0f + 8.0f * (st.kasel & 3) + ((st.kasel - 16) >> 2));
        set(b + 8, (keep & 1) ? 1.0f : 0.0f, (keep & 2) ? 1.0f : 0.0f, (keep & 4) ? 1.0f : 0.0f, kc);
        set(b + 9, (keep & 8) ? 1.0f : 0.0f, (keep & 16) ? 1.0f : 0.0f, (keep & 32) ? 1.0f : 0.0f, ka);
        if (st.ind) {
            const uint32_t ind = st.ind;
            set(b + 10, 1.0f, Bits(ind, 0, 2) < k.numIndStages ? static_cast<float>(Bits(ind, 0, 2)) : 4.0f, Bits(ind, 2, 2), Bits(ind, 9, 4));
            const uint32_t bias = Bits(ind, 4, 3);
            set(b + 11, bias & 1, (bias >> 1) & 1, (bias >> 2) & 1, Bits(ind, 20, 1));
            set(b + 12, Bits(ind, 13, 3), Bits(ind, 16, 3), st.texMap & 7, 0.0f);
        }
    }
    return true;
}

void GenerateUberFragmentShader(const UberIface& f, ShaderSource& src) {
    Out o;
    const int G = kUberGlobalRows, K = kUberStageRows;
    o("#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n");
    // (vgx_shadergen.cpp's helpers, verbatim)
    o("float vgx_wrap_final(float value) {\n"
      "  if (value >= 0.0 && value <= 1.0) return value;\n"
      "  float byteValue = floor(value * 255.0 + 0.5);\n"
      "  return (byteValue - 256.0 * floor(byteValue / 256.0)) / 255.0;\n}\n");
    o("float vgx_w1(float value) {\n"
      "  if (value >= 0.0 && value <= 1.0) return value;\n"
      "  float byteValue = floor(value * 255.0 + 0.5);\n"
      "  return (byteValue - 256.0 * floor(byteValue / 256.0)) / 255.0;\n}\n"
      "vec3 vgx_w3(vec3 value) { return vec3(vgx_w1(value.x), vgx_w1(value.y), vgx_w1(value.z)); }\n");
    for (int m = 0; m < 8; ++m)
        if (f.texMask & (1u << m)) o("uniform sampler2D u_tex%d;\n", m);
    o("uniform vec4 u_tev[28];\nuniform vec4 u_ucfg[%d];\n", kUberRows);
    if (f.col & 1) o("varying vec4 v_col0;\n");
    if (f.col & 2) o("varying vec4 v_col1;\n");
    for (int i = 0; i < f.numTexGens; ++i) o((f.vec2Mask >> i) & 1u ? "varying vec2 v_tex%d;\n" : "varying vec3 v_tex%d;\n", i);
    if (f.fogVtx) o("varying float %s;\n", FogVaryingName());
    // the texture coordinate of texgen c (8: vec2(0.0), a texgen the key does not have)
    o("vec2 vgx_uv(float c) {\n");
    for (int i = 0; i < f.numTexGens; ++i) {
        if ((f.vec2Mask >> i) & 1u) o("  if (c < %d.5) return v_tex%d;\n", i, i);
        else o("  if (c < %d.5) return u_ucfg[%d].%c > 0.5 ? v_tex%d.xy / (v_tex%d.z == 0.0 ? 1.0 : v_tex%d.z) : v_tex%d.xy;\n", i, 1 + i / 4, "xyzw"[i & 3], i, i, i, i);
    }
    o("  return vec2(0.0);\n}\n");
    // texture m at coordinate t; vgx_texb with the map's LOD bias (GXV_FIX_LODBIAS). Two functions, not one with a select between the two reads: Mesa folds such
    // a select of a biased and an unbiased read of the same sampler into one read (a wrong level-of-detail on llvmpipe). White for a map the interface lacks (never selected).
    o("vec4 vgx_tex(float m, vec2 t) {\n");
    for (int m = 0; m < 8; ++m)
        if (f.texMask & (1u << m)) o("  if (m > %d.5 && m < %d.5) return texture2D(u_tex%d, t);\n", m - 1, m, m);
    o("  return vec4(1.0);\n}\n");
    o("vec4 vgx_texb(float m, vec2 t) {\n");
    for (int m = 0; m < 8; ++m)
        if (f.texMask & (1u << m)) o("  if (m > %d.5 && m < %d.5) return texture2D(u_tex%d, t, u_tev[%d].%c);\n", m - 1, m, m, R_LODBIAS + m / 4, "xyzw"[m & 3]);
    o("  return vec4(1.0);\n}\n");
    o("float vgx_pick(vec4 v, float i) { return i < 0.5 ? v.r : (i < 1.5 ? v.g : (i < 2.5 ? v.b : v.a)); }\n"
      "vec4 vgx_swz(vec4 v, vec4 s) { return vec4(vgx_pick(v, s.x), vgx_pick(v, s.y), vgx_pick(v, s.z), vgx_pick(v, s.w)); }\n"
      "vec4 vgx_reg(float r, vec4 prev, vec4 c0, vec4 c1, vec4 c2) { return r < 0.5 ? prev : (r < 1.5 ? c0 : (r < 2.5 ? c1 : c2)); }\n");
    // colour input i (ColorArg): 0..7 register parts (byte wrap when w), 8 tex.rgb, 9 tex.aaa, 10 ras.rgb, 11 ras.aaa, 12 one, 13 half, 14 konst, 15 zero
    o("vec3 vgx_cin(float i, float w, vec4 prev, vec4 c0, vec4 c1, vec4 c2, vec4 tex, vec4 ras, vec3 kc) {\n"
      "  if (i < 7.5) {\n"
      "    vec4 r = vgx_reg(floor(i * 0.5), prev, c0, c1, c2);\n"
      "    vec3 v = fract(i * 0.5) < 0.25 ? r.rgb : r.aaa;\n"
      "    return w > 0.5 ? vgx_w3(v) : v;\n  }\n"
      "  if (i < 8.5) return tex.rgb;\n  if (i < 9.5) return tex.aaa;\n  if (i < 10.5) return ras.rgb;\n  if (i < 11.5) return ras.aaa;\n"
      "  if (i < 12.5) return vec3(1.0);\n  if (i < 13.5) return vec3(0.5);\n  if (i < 14.5) return kc;\n  return vec3(0.0);\n}\n");
    o("float vgx_ain(float i, float w, vec4 prev, vec4 c0, vec4 c1, vec4 c2, vec4 tex, vec4 ras, float ka) {\n"
      "  if (i < 3.5) { float v = vgx_reg(i, prev, c0, c1, c2).a; return w > 0.5 ? vgx_w1(v) : v; }\n"
      "  if (i < 4.5) return tex.a;\n  if (i < 5.5) return ras.a;\n  if (i < 6.5) return ka;\n  return 0.0;\n}\n");
    // konst (KonstColor / KonstAlpha): a constant (8 - sel) / 8 or 0, or a component pattern of a konst row
    o("vec4 vgx_krow(float k) { return k < 0.5 ? u_tev[%d] : (k < 1.5 ? u_tev[%d] : (k < 2.5 ? u_tev[%d] : u_tev[%d])); }\n", R_KONST, R_KONST + 1, R_KONST + 2, R_KONST + 3);
    o("vec3 vgx_kc(float s) {\n  if (s >= 0.0) return vec3(s);\n  float x = -s - 1.0; float k = floor(x / 8.0); float c = x - 8.0 * k; vec4 r = vgx_krow(k);\n"
      "  return c < 0.5 ? r.rgb : (c < 1.5 ? r.rrr : (c < 2.5 ? r.ggg : (c < 3.5 ? r.bbb : r.aaa)));\n}\n");
    o("float vgx_ka(float s) {\n  if (s >= 0.0) return s;\n  float x = -s - 1.0; float k = floor(x / 8.0); float c = x - 8.0 * k; vec4 r = vgx_krow(k);\n"
      "  return vgx_pick(r, c);\n}\n");
    // alpha compare (CompareExpr)
    o("bool vgx_cmp(float c, float ai, float ref) {\n"
      "  if (c < 0.5) return false;\n  if (c < 1.5) return ai < ref - 0.5;\n  if (c < 2.5) return abs(ai - ref) < 0.5;\n  if (c < 3.5) return ai < ref + 0.5;\n"
      "  if (c < 4.5) return ai > ref + 0.5;\n  if (c < 5.5) return abs(ai - ref) > 0.5;\n  if (c < 6.5) return ai > ref - 0.5;\n  return true;\n}\n");
    // the indirect matrix rows and the texture sizes (u_tev rows chosen by a key value)
    o("vec4 vgx_imtx(float m, float r) { float j = m * 2.0 + r; return j < 0.5 ? u_tev[%d] : (j < 1.5 ? u_tev[%d] : (j < 2.5 ? u_tev[%d] : (j < 3.5 ? u_tev[%d] : "
      "(j < 4.5 ? u_tev[%d] : u_tev[%d])))); }\n", R_INDMTX, R_INDMTX + 1, R_INDMTX + 2, R_INDMTX + 3, R_INDMTX + 4, R_INDMTX + 5);
    o("vec2 vgx_tsz(float m) {\n  vec4 r = m < 1.5 ? u_tev[%d] : (m < 3.5 ? u_tev[%d] : (m < 5.5 ? u_tev[%d] : u_tev[%d]));\n"
      "  return fract(m * 0.5) < 0.25 ? r.xy : r.zw;\n}\n", R_TEXSIZE, R_TEXSIZE + 1, R_TEXSIZE + 2, R_TEXSIZE + 3);
    o("vec4 vgx_iscale(float i) { return i < 0.5 ? u_tev[%d] : (i < 1.5 ? u_tev[%d] : (i < 2.5 ? u_tev[%d] : u_tev[%d])); }\n", R_INDSCALE, R_INDSCALE + 1,
      R_INDSCALE + 2, R_INDSCALE + 3);
    // the indirect wrap of a texel coordinate (sw / tw codes 1..5: 256, 128, 64, 32, 16; 6: zero)
    o("float vgx_iwrap(float x, float w) {\n  if (w < 0.5) return x;\n  if (w > 5.5) return 0.0;\n"
      "  float m = w < 1.5 ? 256.0 : (w < 2.5 ? 128.0 : (w < 3.5 ? 64.0 : (w < 4.5 ? 32.0 : 16.0)));\n  return x - m * floor(x / m);\n}\n");
    // the TEV combine of one part (Combine): normal (mode 0) or compare (mode 1 + width); the clamp after it
    o("void main() {\n");
    o("  vec4 prev = u_tev[0], c0 = %s, c1 = %s, c2 = u_tev[3];\n", (f.col & 1) ? "u_ucfg[0].z > 0.5 ? v_col0 : u_tev[1]" : "u_tev[1]",
      (f.col & 2) ? "u_ucfg[0].w > 0.5 ? v_col1 : u_tev[2]" : "u_tev[2]");
    o("  vec4 tex = vec4(1.0), ras = vec4(0.0);\n  vec3 kc = vec3(1.0);\n  float ka = 1.0;\n");
    // indirect lookups: three 8-bit offsets each, from a, b, g
    o("  vec3 ind0 = vec3(0.0), ind1 = vec3(0.0), ind2 = vec3(0.0), ind3 = vec3(0.0);\n");
    static const char* const kIc[4] = {"x", "y", "z", "w"};
    for (int i = 0; i < 4; ++i) {
        o("  if (u_ucfg[3].x > %d.5) {\n", i);
        o("    vec2 t = u_ucfg[6].%s > 0.5 ? vgx_uv(u_ucfg[5].%s) : vgx_uv(u_ucfg[5].%s) * vgx_iscale(%d.0).xy;\n", kIc[i], kIc[i], kIc[i], i);
        o("    ind%d = vgx_tex(u_ucfg[4].%s, t).abg * 255.0;\n  }\n", i, kIc[i]);
    }
    o("  vec2 itc = vec2(0.0), itx = vec2(0.0);\n");
    o("  for (int s = 0; s < %d; ++s) {\n", kUberMaxStages);
    o("    if (float(s) > u_ucfg[0].x - 0.5) break;\n");
    o("    vec4 q0 = u_ucfg[%d + s * %d], q5 = u_ucfg[%d + s * %d], q6 = u_ucfg[%d + s * %d], q7 = u_ucfg[%d + s * %d];\n", G, K, G + 5, K, G + 6, K, G + 7, K);
    o("    vec4 q8 = u_ucfg[%d + s * %d], q9 = u_ucfg[%d + s * %d];\n", G + 8, K, G + 9, K);
    o("    vec2 coord = vgx_uv(q0.z);\n");
    // an indirect stage (the GXV_FIX_INDPREV form when the key has it: itx carries the texel-unit sum)
    o("    vec4 q10 = u_ucfg[%d + s * %d];\n", G + 10, K);
    o("    if (q10.x > 0.5) {\n");
    o("      vec4 q11 = u_ucfg[%d + s * %d], q12 = u_ucfg[%d + s * %d];\n", G + 11, K, G + 12, K);
    o("      vec2 tsz = vgx_tsz(q12.z);\n      vec2 tcx = coord * tsz;\n");
    o("      vec3 iv = q10.y < 0.5 ? ind0 : (q10.y < 1.5 ? ind1 : (q10.y < 2.5 ? ind2 : (q10.y < 3.5 ? ind3 : vec3(0.0))));\n");
    o("      if (q10.z > 0.5) { float md = q10.z < 1.5 ? 32.0 : (q10.z < 2.5 ? 16.0 : 8.0); iv = iv - md * floor(iv / md); }\n");
    o("      float bv = q10.z < 0.5 ? -128.0 : 1.0;\n");
    o("      iv += vec3(q11.x > 0.5 ? bv : 0.0, q11.y > 0.5 ? bv : 0.0, q11.z > 0.5 ? bv : 0.0);\n");
    o("      vec2 io = vec2(0.0);\n      float mid = q10.w;\n");
    o("      if (mid > 0.5 && mid < 3.5) io = vec2(dot(vgx_imtx(mid - 1.0, 0.0).xyz, iv), dot(vgx_imtx(mid - 1.0, 1.0).xyz, iv));\n");
    o("      else if (mid > 4.5 && mid < 7.5) io = tcx * (iv.x / 256.0) * vgx_imtx(mid - 5.0, 0.0).w;\n");
    o("      else if (mid > 8.5 && mid < 11.5) io = tcx * (iv.y / 256.0) * vgx_imtx(mid - 9.0, 0.0).w;\n");
    o("      tcx.x = vgx_iwrap(tcx.x, q12.x);\n      tcx.y = vgx_iwrap(tcx.y, q12.y);\n");
    o("      if (u_ucfg[3].y > 0.5) {\n        itx = q11.w > 0.5 ? tcx + io + itx : tcx + io;\n        itc = itx / tsz;\n      } else {\n"
      "        itc = q11.w > 0.5 ? tcx + io + itc : tcx + io;\n        itc = itc / tsz;\n      }\n");
    o("      coord = itc;\n    }\n");
    o("    if (q0.x > 0.5 && q0.x < 1.5) tex = vgx_swz(vgx_tex(q0.y, coord), u_ucfg[%d + s * %d]);\n", G + 1, K);
    o("    else if (q0.x > 2.5) tex = vgx_swz(vgx_texb(q0.y, coord), u_ucfg[%d + s * %d]);\n", G + 1, K);
    o("    else if (q0.x > 1.5) tex = vec4(1.0);\n");
    o("    ras = vec4(0.0);\n");
    if (f.col & 1) o("    if (q0.w < 0.5) ras = vgx_swz(v_col0, u_ucfg[%d + s * %d]);\n", G + 2, K);
    if (f.col & 2) o("    if (q0.w > 0.5 && q0.w < 1.5) ras = vgx_swz(v_col1, u_ucfg[%d + s * %d]);\n", G + 2, K);
    o("    kc = vgx_kc(q8.w);\n    ka = vgx_ka(q9.w);\n");
    o("    vec4 sc = u_ucfg[%d + s * %d], sa = u_ucfg[%d + s * %d];\n", G + 3, K, G + 4, K);
    o("    vec3 a = vgx_cin(sc.x, q8.x, prev, c0, c1, c2, tex, ras, kc), b = vgx_cin(sc.y, q8.y, prev, c0, c1, c2, tex, ras, kc), "
      "c = vgx_cin(sc.z, q8.z, prev, c0, c1, c2, tex, ras, kc), d = vgx_cin(sc.w, 0.0, prev, c0, c1, c2, tex, ras, kc);\n");
    o("    float aa = vgx_ain(sa.x, q9.x, prev, c0, c1, c2, tex, ras, ka), ab = vgx_ain(sa.y, q9.y, prev, c0, c1, c2, tex, ras, ka), "
      "ac = vgx_ain(sa.z, q9.z, prev, c0, c1, c2, tex, ras, ka), ad = vgx_ain(sa.w, 0.0, prev, c0, c1, c2, tex, ras, ka);\n");
    // (the compare constants exactly as the generator prints them: %.6f of 0.25 / 255 and twice that)
    const float e = 0.25f / 255.0f;
    char e1[32], e2[32];
    std::snprintf(e1, sizeof e1, "%.6f", e);
    std::snprintf(e2, sizeof e2, "%.6f", e * 2);
    o("    vec3 rc;\n    if (q5.x < 0.5) rc = (d + q5.y * mix(a, b, c) + q5.z) * q5.w;\n");
    o("    else if (q5.x < 1.5) rc = d + (((q5.y > 0.5) ? (abs(a.r - b.r) < %s) : (a.r > b.r + %s)) ? c : vec3(0.0));\n", e2, e1);
    o("    else if (q5.x < 2.5) rc = d + (((q5.y > 0.5) ? (abs(dot(a.rg - b.rg, vec2(255.0, 65280.0))) < 0.5) : (dot(a.rg - b.rg, vec2(255.0, 65280.0)) > 0.25)) ? c : vec3(0.0));\n");
    o("    else if (q5.x < 3.5) rc = d + (((q5.y > 0.5) ? (abs(dot(a.rgb - b.rgb, vec3(255.0, 65280.0, 16711680.0))) < 0.5) : "
      "(dot(a.rgb - b.rgb, vec3(255.0, 65280.0, 16711680.0)) > 0.25)) ? c : vec3(0.0));\n");
    o("    else rc = d + c * ((q5.y > 0.5) ? (vec3(1.0) - step(vec3(%s), abs(a - b))) : (vec3(1.0) - step(a, b + vec3(%s))));\n", e2, e1);
    o("    rc = q7.x > 0.5 ? clamp(rc, 0.0, 1.0) : clamp(rc, -4.0157, 4.0118);\n");
    o("    float ra;\n    if (q6.x < 0.5) ra = (ad + q6.y * mix(aa, ab, ac) + q6.z) * q6.w;\n");
    o("    else if (q6.x < 1.5) ra = ad + (((q6.y > 0.5) ? (abs(a.r - b.r) < %s) : (a.r > b.r + %s)) ? ac : 0.0);\n", e2, e1);
    o("    else if (q6.x < 2.5) ra = ad + (((q6.y > 0.5) ? (abs(dot(a.rg - b.rg, vec2(255.0, 65280.0))) < 0.5) : (dot(a.rg - b.rg, vec2(255.0, 65280.0)) > 0.25)) ? ac : 0.0);\n");
    o("    else if (q6.x < 3.5) ra = ad + (((q6.y > 0.5) ? (abs(dot(a.rgb - b.rgb, vec3(255.0, 65280.0, 16711680.0))) < 0.5) : "
      "(dot(a.rgb - b.rgb, vec3(255.0, 65280.0, 16711680.0)) > 0.25)) ? ac : 0.0);\n");
    o("    else ra = ad + (((q6.y > 0.5) ? (abs(aa - ab) < %s) : (aa > ab + %s)) ? ac : 0.0);\n", e2, e1);
    o("    ra = q7.y > 0.5 ? clamp(ra, 0.0, 1.0) : clamp(ra, -4.0157, 4.0118);\n");
    o("    if (q7.z < 0.5) prev.rgb = rc; else if (q7.z < 1.5) c0.rgb = rc; else if (q7.z < 2.5) c1.rgb = rc; else c2.rgb = rc;\n");
    o("    if (q7.w < 0.5) prev.a = ra; else if (q7.w < 1.5) c0.a = ra; else if (q7.w < 2.5) c1.a = ra; else c2.a = ra;\n");
    o("  }\n");
    // the final value: the last stage's registers, the byte wrap (GXV_TEVWRAP), the clamp
    o("  vec4 fin = vec4(vgx_reg(u_ucfg[10].y, prev, c0, c1, c2).rgb, vgx_reg(u_ucfg[10].z, prev, c0, c1, c2).a);\n");
    o("  if (u_ucfg[3].z > 0.5) fin.rgb = vec3(vgx_wrap_final(fin.r), vgx_wrap_final(fin.g), vgx_wrap_final(fin.b));\n");
    o("  if (u_ucfg[3].w > 0.5) fin.a = vgx_wrap_final(fin.a);\n");
    o("  fin = clamp(fin, 0.0, 1.0);\n");
    o("  bool vgx_ok = true;\n");
    o("  if (u_ucfg[7].x > 0.5) {\n    float ai = floor(fin.a * 255.0 + 0.5);\n"
      "    bool t0 = vgx_cmp(u_ucfg[7].y, ai, u_tev[%d].x), t1 = vgx_cmp(u_ucfg[7].z, ai, u_tev[%d].y);\n"
      "    float op = u_ucfg[7].w;\n    vgx_ok = op < 0.5 ? (t0 && t1) : (op < 1.5 ? (t0 || t1) : (op < 2.5 ? (t0 != t1) : (t0 == t1)));\n"
      "    if (!vgx_ok && u_ucfg[8].x < 0.5) discard;\n  }\n", R_MISC, R_MISC);
    o("  if (u_ucfg[8].y > 0.5) { gl_FragColor = vec4(0.0); return; }\n");
    // fog (the vertex form, GXV_FOGVTX, only where the interface has its varying)
    o("  if (u_ucfg[9].x > 0.5) {\n    float fg;\n");
    if (f.fogVtx) o("    if (u_ucfg[9].z > 0.5) fg = clamp(u_tev[%d].x * %s + u_tev[%d].w, 0.0, 1.0);\n    else {\n", R_FOG, FogVaryingName(), R_FOG);
    else o("    {\n");
    o("      if (u_ucfg[9].w > 0.5) fg = u_tev[%d].x * gl_FragCoord.z;\n      else fg = u_tev[%d].x / max(u_tev[%d].y - gl_FragCoord.z, 0.000001);\n"
      "      fg = clamp(fg - u_tev[%d].z, 0.0, 1.0);\n    }\n", R_FOG, R_FOG, R_FOG, R_FOG);
    o("    float fs = u_ucfg[9].y;\n"
      "    if (fs > 3.5 && fs < 4.5) fg = 1.0 - exp2(-8.0 * fg);\n"
      "    else if (fs > 4.5 && fs < 5.5) fg = 1.0 - exp2(-8.0 * fg * fg);\n"
      "    else if (fs > 5.5 && fs < 6.5) fg = exp2(-8.0 * (1.0 - fg));\n"
      "    else if (fs > 6.5) { fg = 1.0 - fg; fg = exp2(-8.0 * fg * fg); }\n");
    o("    fin.rgb = mix(fin.rgb, u_tev[%d].rgb, fg);\n  }\n", R_FOGCOL);
    o("  if (u_ucfg[10].x > 0.5) fin.a = u_tev[%d].z;\n  else if (u_ucfg[8].z > 0.5) fin.a = 0.0;\n", R_MISC);
    o("  if (u_ucfg[8].w > 0.5 && u_ucfg[8].w < 1.5) fin.rgb = vec3(1.0) - fin.rgb;\n  else if (u_ucfg[8].w > 1.5) fin.rgb = vec3(1.0);\n");
    o("  gl_FragColor = vgx_ok ? fin : vec4(0.0);\n}\n");
    src.fragment = true;
    src.text = std::move(o.s);
    src.texMask = f.texMask;
    src.numTexGens = f.numTexGens;
    src.nTev = 28;
}

} // namespace vgx
