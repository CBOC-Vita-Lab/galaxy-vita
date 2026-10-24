// GLSL ES 3.2 generation for the GX pipeline.
#include "gl_shadergen.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "gpu.h"
#include "gx_record.h"

namespace gpu {

namespace {

struct Out {
    std::string s;
    void w(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        s += buf;
    }
};

const char* kUniformBlocks = R"(
layout(std430, binding = 0) readonly buffer RowBuf { vec4 rows[]; };
layout(std430, binding = 1) readonly buffer RecBuf { uint recs[]; };
layout(std430, binding = 2) readonly buffer StateBuf { vec4 st[]; };
layout(std140, binding = 2) uniform EyeBlock {
    vec4 vrView[4];
    vec4 vrProj[4];
    ivec4 vrFlags;
    vec4 vrFocus;
    vec4 vrCut;
    vec4 vrEyePos;
    vec4 vrStereo;
    vec4 vrStereo2;
};
)";

}  // namespace

ShaderUid makeShaderUid(const uint32_t* bp, const uint32_t* xf, uint32_t vtxFlags, bool hud) {
    ShaderUid u;
    memset(&u, 0, sizeof(u));
    u.vtxFlags = vtxFlags;
    u.numTexGens = xf[0x3F] & 15;
    u.numChans = xf[0x09] & 3;
    u.dualTex = xf[0x12] & 1;
    for (int i = 0; i < 2; i++) {
        u.colorCtrl[i] = i < (int)u.numChans ? (xf[0x0E + i] & 0x7FFF) : 0;
        u.alphaCtrl[i] = i < (int)u.numChans ? (xf[0x10 + i] & 0x7FFF) : 0;
    }
    for (uint32_t i = 0; i < u.numTexGens && i < 8; i++) {
        u.texMtxInfo[i] = xf[0x40 + i] & 0x3FFFF;
        u.postMtxInfo[i] = u.dualTex ? (xf[0x50 + i] & 0x100) : 0;  // the matrix comes from the draw record
    }
    uint32_t gen = bp[0x00];
    u.numTevStages = ((gen >> 10) & 15) + 1;
    u.numIndStages = (gen >> 16) & 7;
    for (uint32_t s = 0; s < u.numTevStages; s++) {
        u.tevColor[s] = bp[0xC0 + 2 * s] & 0xFFFFFF;
        u.tevAlpha[s] = bp[0xC1 + 2 * s] & 0xFFFFFF;
    }
    for (uint32_t i = 0; i < (u.numTevStages + 1) / 2; i++) {
        u.tevOrder[i] = bp[0x28 + i] & 0xFFFFFF;
    }
    for (int i = 0; i < 8; i++) {
        u.ksel[i] = bp[0xF6 + i] & 0xFFFFFF;
    }
    u.alphaFunc = (bp[0xF3] >> 16) & 0xFF;
    u.fogSel = (bp[0xF1] >> 20) & 0xF;
    if (u.numIndStages) {
        for (uint32_t s = 0; s < u.numTevStages; s++) {
            u.indCmd[s] = bp[0x10 + s] & 0x1FFFFF;
        }
        u.iref = bp[0x27] & 0xFFFFFF;
        u.indScale[0] = bp[0x25] & 0xFFFF;
        u.indScale[1] = bp[0x26] & 0xFFFF;
    }
    uint32_t pixFmt = bp[0x43] & 7;
    u.efbHasAlpha = pixFmt == 1;  // RGBA6_Z24
    u.hud = hud;
    u.zTex = ((bp[0xF5] >> 2) & 3) ? (bp[0xF5] & 0xF) : 0;
    return u;
}

// ---------------------------------------------------------------------------
// Vertex shader
// ---------------------------------------------------------------------------
std::string genVertexShader(const ShaderUid& u) {
    Out o;
    o.w("#version 320 es\nprecision highp float;\nprecision highp int;\n");
    o.s += kUniformBlocks;
    uint32_t f = u.vtxFlags;
    o.w("layout(location = 0) in vec3 a_pos;\n");
    if (f & VF_POSMTX) o.w("layout(location = 1) in uint a_posmtx;\n");
    if (f & VF_NRM) o.w("layout(location = 2) in vec3 a_nrm;\n");
    if (f & VF_NBT) o.w("layout(location = 3) in vec3 a_bin;\nlayout(location = 4) in vec3 a_tan;\n");
    if (f & VF_CLR0) o.w("layout(location = 5) in vec4 a_clr0;\n");
    if (f & VF_CLR1) o.w("layout(location = 6) in vec4 a_clr1;\n");
    for (int i = 0; i < 8; i++) {
        if (f & (VF_TEX0 << i)) o.w("layout(location = %d) in vec2 a_tex%d;\n", 7 + i, i);
    }
    if (f & VF_TEXMTX) o.w("layout(location = 15) in uvec2 a_texmtx;\n");
    // Draw record of this vertex: indices are relative to the batch's first
    // vertex, so gl_VertexID + the batch's first ordinal indexes vtxRec[].
    o.w("layout(std430, binding = 3) readonly buffer VtxRecBuf { uint vtxRec[]; };\n");
    o.w("uniform uint u_firstOrdinal;\n");
    o.w("flat out uint v_pix;\n");
    o.w("out vec4 v_clr0;\nout vec4 v_clr1;\n");
    for (uint32_t i = 0; i < u.numTexGens; i++) o.w("out vec3 v_tex%u;\n", i);
    o.w("out float v_viewz;\nout float v_gxz;\n");
    if (u.cutaway) o.w("out vec3 v_eye;\n");

    // Matrix rows: slot bases come from the draw record (3 rows per slot).
    o.w("uint slotRow(uint rb, uint firstSlot, int idx) { return recs[rb + firstSlot + uint(idx / 3)] + uint(idx %% 3); }\n");
    o.w("vec3 mulRows(uint r, vec4 v) { return vec3(dot(rows[r], v), dot(rows[r + 1u], v), dot(rows[r + 2u], v)); }\n");
    o.w("#define L(i) st[lbase + uint(i)]\n");
    o.w("void main() {\n");
    o.w("  uint rb = vtxRec[u_firstOrdinal + uint(gl_VertexID)] * %uu;\n", (unsigned)kRecWords);
    o.w("  uint lbase = recs[rb + %uu];\n  uint pjbase = recs[rb + %uu];\n", (unsigned)kRecLights, (unsigned)kRecProj);
    o.w("  v_pix = recs[rb + %uu];\n", (unsigned)kRecPixel);
    o.w("  uint mA = recs[rb + %uu];\n  uint mB = recs[rb + %uu];\n", (unsigned)kRecMtxIdxA, (unsigned)kRecMtxIdxB);
    o.w("  vec4 proj[4] = vec4[4](st[pjbase], st[pjbase + 1u], st[pjbase + 2u], st[pjbase + 3u]);\n");
    o.w("  vec4 depthParams = st[pjbase + 4u];\n");
    o.w("  vec4 matColor[2] = vec4[2](L(40), L(41));\n  vec4 ambColor[2] = vec4[2](L(42), L(43));\n");
    if (f & VF_POSMTX) {
        o.w("  int pidx = int(a_posmtx);\n");
    } else {
        o.w("  int pidx = int(mA & 63u);\n");
    }
    o.w("  vec4 p4 = vec4(a_pos, 1.0);\n");
    o.w("  vec3 pos = mulRows(slotRow(rb, %uu, pidx), p4);\n", (unsigned)kRecPosSlots);
    o.w("  int nidx = pidx & 31;\n");
    o.w("  uint nr = slotRow(rb, %uu, nidx);\n", (unsigned)kRecNrmSlots);
    o.w("  vec3 nrm = vec3(0.0, 0.0, 1.0);\n  vec3 bin = vec3(0.0);\n  vec3 tan = vec3(0.0);\n");
    if (f & VF_NRM) {
        o.w("  nrm = vec3(dot(rows[nr].xyz, a_nrm), dot(rows[nr + 1u].xyz, a_nrm), dot(rows[nr + 2u].xyz, a_nrm));\n");
        o.w("  if (dot(nrm, nrm) > 0.0) nrm = normalize(nrm);\n");
    }
    if (f & VF_NBT) {
        o.w("  bin = vec3(dot(rows[nr].xyz, a_bin), dot(rows[nr + 1u].xyz, a_bin), dot(rows[nr + 2u].xyz, a_bin));\n");
        o.w("  tan = vec3(dot(rows[nr].xyz, a_tan), dot(rows[nr + 1u].xyz, a_tan), dot(rows[nr + 2u].xyz, a_tan));\n");
    }

    // Projection (GX clip space: z/w in [-1, 0]) with GX depth mapping, or
    // the VR camera for 3D draws.
    o.w("  vec4 v4 = vec4(pos, 1.0);\n");
    // GX clip position and window depth (also drives fog in VR, where the
    // rasterized depth comes from the eye projection instead).
    o.w("  vec4 c = vec4(dot(proj[0], v4), dot(proj[1], v4), dot(proj[2], v4), dot(proj[3], v4));\n");
    o.w("  float depth = (c.z * depthParams.x + c.w * depthParams.y) * (1.0 / 16777215.0);\n");
    o.w("  v_gxz = c.w != 0.0 ? depth / c.w : depth;\n");
    o.w("  bool vrDraw = (recs[rb + %uu] & 1u) != 0u && vrFlags.x == 1;\n", (unsigned)kRecFlags);
    o.w("  bool stereoDraw = (recs[rb + %uu] & 1u) != 0u && vrFlags.x == 2;\n", (unsigned)kRecFlags);
    o.w("  vec4 clipPos;\n");
    o.w("  if (vrDraw && (recs[rb + %uu] & 2u) != 0u) {\n", (unsigned)kRecFlags);
    // A sky is modelled around the game camera, which is metres away from
    // the eye in the diorama: draw it around the eye, at the far plane.
    o.w("    vec4 e = vec4(dot(vrView[0].xyz, pos), dot(vrView[1].xyz, pos), dot(vrView[2].xyz, pos), 0.0);\n");
    o.w("    clipPos = vec4(dot(vrProj[0], e), dot(vrProj[1], e), 0.0, dot(vrProj[3], e));\n");
    o.w("    clipPos.z = clipPos.w * 0.99999;\n");
    if (u.cutaway) o.w("    v_eye = vec3(0.0, 0.0, 1.0e6);  // never cut\n");
    o.w("  } else if (vrDraw) {\n");
    o.w("    vec4 e = vec4(dot(vrView[0], v4), dot(vrView[1], v4), dot(vrView[2], v4), 1.0);\n");
    o.w("    clipPos = vec4(dot(vrProj[0], e), dot(vrProj[1], e), dot(vrProj[2], e), dot(vrProj[3], e));\n");
    if (u.cutaway) o.w("    v_eye = e.xyz;\n");
    o.w("  } else {\n");
    // One picture of the flat screen's stereo pair: the game's own camera a
    // little to the side, its frustum sheared so what is vrStereo.y away
    // lands where it does without (the screen's own depth).  Nearer things
    // shift the other way, at most vrStereo.z times as far as the farthest
    // ones; a sky belongs with the farthest.
    o.w("    if (stereoDraw) c.x += vrStereo.x * min(((recs[rb + %uu] & 2u) != 0u ? 0.0 : vrStereo.y) - c.w, vrStereo.z * c.w);\n",
        (unsigned)kRecFlags);
    if (u.hud) {
        // The HUD of a stereo pair: shifted as a whole (it floats before
        // the screen, in front of the scene it is drawn over) and widened by
        // as much, so what covers the whole picture (a fade) still does.
        // The pointer's cursor on the scene goes where what it points at is
        // drawn instead (on menus it stays with the HUD).
        o.w("    if (vrFlags.x == 2) {\n");
        o.w("      if ((recs[rb + %uu] & 4u) != 0u && vrStereo2.y == 0.0) c.x += vrStereo2.x * c.w;\n", (unsigned)kRecFlags);
        o.w("      else c.x = c.x * (1.0 + abs(vrStereo.w)) + vrStereo.w * c.w;\n");
        o.w("    }\n");
    }
    o.w("    clipPos = vec4(c.x, c.y, depth * 2.0 * 1.0 - c.w, c.w);\n");
    if (u.cutaway) o.w("    v_eye = vec3(0.0, 0.0, 1.0e6);  // never cut\n");
    o.w("  }\n");
    o.w("  gl_Position = clipPos;\n");
    o.w("  v_viewz = pos.z;\n");

    // Color channels.
    o.w("  vec4 vclr0 = %s;\n", (f & VF_CLR0) ? "a_clr0" : "vec4(1.0)");
    o.w("  vec4 vclr1 = %s;\n", (f & VF_CLR1) ? "a_clr1" : "vec4(1.0)");
    for (int c = 0; c < 2; c++) {
        if ((uint32_t)c >= u.numChans) {
            o.w("  v_clr%d = vclr%d;\n", c, c);
            continue;
        }
        for (int alpha = 0; alpha < 2; alpha++) {
            uint32_t ctrl = alpha ? u.alphaCtrl[c] : u.colorCtrl[c];
            const char* sw = alpha ? "a" : "rgb";
            char mat[64], amb[64];
            snprintf(mat, sizeof(mat), (ctrl & 1) ? "vclr%d.%s" : "matColor[%d].%s", c, sw);
            snprintf(amb, sizeof(amb), ((ctrl >> 6) & 1) ? "vclr%d.%s" : "ambColor[%d].%s", c, sw);
            bool enable = (ctrl >> 1) & 1;
            uint32_t mask = ((ctrl >> 2) & 15) | (((ctrl >> 11) & 15) << 4);
            if (!enable) {
                o.w("  v_clr%d.%s = %s;\n", c, sw, mat);
                continue;
            }
            uint32_t diffuse = (ctrl >> 7) & 3;
            bool atten = (ctrl >> 9) & 1;
            bool spot = (ctrl >> 10) & 1;
            o.w("  {\n    %s lit = %s;\n", alpha ? "float" : "vec3", amb);
            for (int l = 0; l < 8; l++) {
                if (!(mask & (1u << l))) {
                    continue;
                }
                o.w("    {\n");
                o.w("      vec4 lcol = L(%d); vec3 lcos = L(%d).xyz; vec3 ldist = L(%d).xyz;\n", l * 5, l * 5 + 1, l * 5 + 2);
                o.w("      vec3 lpos = L(%d).xyz; vec3 ldirv = L(%d).xyz;\n", l * 5 + 3, l * 5 + 4);
                // A light at the view space origin sits on the camera (the
                // characters' rim light, LightFunction::loadActorLightInfo):
                // in VR it belongs on the eye, or rims land mid-face.
                o.w("      if (vrDraw && lpos == vec3(0.0)) lpos = vrEyePos.xyz;\n");
                if (!atten) {
                    o.w("      vec3 ldir = normalize(lpos - pos); float att = 1.0;\n");
                } else if (spot) {
                    o.w("      vec3 ldir = lpos - pos; float d2 = dot(ldir, ldir); float d = sqrt(d2); ldir = ldir / d;\n");
                    o.w("      float a = max(0.0, dot(ldir, ldirv));\n");
                    o.w("      float att = max(0.0, dot(lcos, vec3(1.0, a, a * a))) / dot(ldist, vec3(1.0, d, d2));\n");
                } else {
                    o.w("      vec3 ldir = normalize(lpos);\n");
                    o.w("      float a = (dot(nrm, ldir) >= 0.0) ? max(0.0, dot(nrm, ldirv)) : 0.0;\n");
                    o.w("      float att = max(0.0, dot(lcos, vec3(1.0, a, a * a))) / dot(ldist, vec3(1.0, a, a * a));\n");
                }
                const char* dif = diffuse == 0 ? "1.0" : diffuse == 1 ? "dot(ldir, nrm)" : "max(0.0, dot(ldir, nrm))";
                o.w("      lit += att * %s * lcol.%s;\n", dif, sw);
                o.w("    }\n");
            }
            o.w("    v_clr%d.%s = %s * clamp(lit, 0.0, 1.0);\n  }\n", c, sw, mat);
        }
    }

    // Texture coordinate generation.
    for (uint32_t i = 0; i < u.numTexGens; i++) {
        uint32_t info = u.texMtxInfo[i];
        bool stq = (info >> 1) & 1;
        bool abc1 = (info >> 2) & 1;
        uint32_t type = (info >> 4) & 7;
        uint32_t src = (info >> 7) & 31;
        o.w("  {\n");
        if (type == 2 || type == 3) {  // color texgen
            o.w("    v_tex%u = vec3(v_clr%u.rg, 1.0);\n  }\n", i, type - 2);
            continue;
        }
        if (type == 1) {  // emboss
            uint32_t esrc = (info >> 12) & 7, elight = (info >> 15) & 7;
            o.w("    vec3 ldir = normalize(L(%u).xyz - pos);\n", elight * 5 + 3);
            if (esrc < i) {
                o.w("    v_tex%u = v_tex%u + vec3(dot(ldir, tan), dot(ldir, bin), 0.0);\n  }\n", i, esrc);
            } else {
                o.w("    v_tex%u = vec3(0.0, 0.0, 1.0);\n  }\n", i);
            }
            continue;
        }
        const char* srcExpr;
        char buf[96];
        switch (src) {
        case 0:
            srcExpr = "vec4(a_pos, 1.0)";
            break;
        case 1:
            srcExpr = (f & VF_NRM) ? "vec4(a_nrm, 1.0)" : "vec4(0.0, 0.0, 1.0, 1.0)";
            break;
        case 3:
            srcExpr = (f & VF_NBT) ? "vec4(a_tan, 1.0)" : "vec4(0.0, 0.0, 1.0, 1.0)";
            break;
        case 4:
            srcExpr = (f & VF_NBT) ? "vec4(a_bin, 1.0)" : "vec4(0.0, 0.0, 1.0, 1.0)";
            break;
        default:
            if (src >= 5 && src <= 12 && (f & (VF_TEX0 << (src - 5)))) {
                snprintf(buf, sizeof(buf), "vec4(a_tex%u, 1.0, 1.0)", src - 5);
                srcExpr = buf;
            } else {
                srcExpr = "vec4(0.0, 0.0, 1.0, 1.0)";
            }
            break;
        }
        o.w("    vec4 src = %s;\n", srcExpr);
        if (!abc1) {
            o.w("    src.z = 1.0;\n");
        }
        // Matrix index: per-vertex or default.
        if (f & VF_TEXMTX) {
            o.w("    int midx = int((a_texmtx[%u] >> %uu) & 255u);\n", i / 4, (i % 4) * 8);
        } else {
            if (i < 4) {
                o.w("    int midx = int((mA >> %uu) & 63u);\n", 6 * (i + 1));
            } else {
                o.w("    int midx = int((mB >> %uu) & 63u);\n", 6 * (i - 4));
            }
        }
        o.w("    uint tr = slotRow(rb, %uu, midx);\n", (unsigned)kRecPosSlots);
        if (stq) {
            o.w("    vec3 t = mulRows(tr, src);\n");
        } else {
            o.w("    vec3 t = vec3(dot(rows[tr], src), dot(rows[tr + 1u], src), 1.0);\n");
        }
        if (u.dualTex) {
            uint32_t post = u.postMtxInfo[i];
            if ((post >> 8) & 1) {
                o.w("    t = normalize(t);\n");
            }
            o.w("    vec4 t4 = vec4(t, 1.0);\n");
            o.w("    t = mulRows(recs[rb + %uu], t4);\n", (unsigned)kRecPost + i);
        }
        if ((u.screenTexGens >> i) & 1) {
            // A capture of the screen sampled where the vertex lands on it.
            // In VR (and in a stereo pair) the capture holds this eye's view:
            // use the eye's screen position (texture rows run top-down).
            o.w("    if (vrDraw || stereoDraw) t = vec3((clipPos.x + clipPos.w) * 0.5, (clipPos.w - clipPos.y) * 0.5, clipPos.w);\n");
        }
        o.w("    v_tex%u = t;\n  }\n", i);
    }
    o.w("}\n");
    return o.s;
}

// ---------------------------------------------------------------------------
// Fragment shader (TEV)
// ---------------------------------------------------------------------------
namespace {

// The TEV works on integers, like Dolphin's shaders: registers hold 11-bit
// signed values, texture / rasterized / konst inputs 8-bit ones.
const char* kColorIn[16] = {"prev.rgb", "prev.aaa", "c0.rgb", "c0.aaa", "c1.rgb", "c1.aaa", "c2.rgb", "c2.aaa",
                            "tex.rgb",  "tex.aaa",  "ras.rgb", "ras.aaa", "ivec3(255)", "ivec3(128)", "kc", "ivec3(0)"};
const char* kAlphaIn[8] = {"prev.a", "c0.a", "c1.a", "c2.a", "tex.a", "ras.a", "ka", "0"};
const char* kDest[4] = {"prev", "c0", "c1", "c2"};
const int kFrac[8] = {255, 223, 191, 159, 128, 96, 64, 32};  // konst 8/8 .. 1/8
const char kComp[4] = {'r', 'g', 'b', 'a'};

std::string konstColor(uint32_t sel) {
    char buf[64];
    if (sel < 8) {
        snprintf(buf, sizeof(buf), "ivec3(%d)", kFrac[sel]);
    } else if (sel >= 12 && sel < 16) {
        snprintf(buf, sizeof(buf), "konst[%u].rgb", sel - 12);
    } else if (sel >= 16) {
        snprintf(buf, sizeof(buf), "konst[%u].%c%c%c", (sel - 16) & 3, kComp[(sel - 16) >> 2], kComp[(sel - 16) >> 2], kComp[(sel - 16) >> 2]);
    } else {
        snprintf(buf, sizeof(buf), "ivec3(0)");
    }
    return buf;
}

std::string konstAlpha(uint32_t sel) {
    char buf[64];
    if (sel < 8) {
        snprintf(buf, sizeof(buf), "%d", kFrac[sel]);
    } else if (sel >= 16) {
        snprintf(buf, sizeof(buf), "konst[%u].%c", (sel - 16) & 3, kComp[(sel - 16) >> 2]);
    } else {
        snprintf(buf, sizeof(buf), "0");
    }
    return buf;
}

// A regular TEV operation, (d + bias +- lerp(a, b, c)) * scale, with the
// hardware's rounding: c runs 0..256, a scale above 1 is applied inside the
// lerp, and a rounding bias precedes the division by 256 (as in Dolphin).
std::string tevRegular(const char* comp, uint32_t bias, uint32_t sub, uint32_t scale, bool alpha) {
    static const char* biasStr[3] = {"", " + 128", " - 128"};
    static const char* scaleLeft[4] = {"", " << 1", " << 2", ""};
    static const char* scaleRight[4] = {"", "", "", " >> 1"};
    const char* lerpBias = ((scale == 3) == alpha) ? (sub ? " + 127" : " + 128") : "";
    char buf[512];
    snprintf(buf, sizeof(buf),
             "(((tevin_d.%s%s)%s) %c (((((tevin_a.%s << 8) + (tevin_b.%s - tevin_a.%s) * (tevin_c.%s + (tevin_c.%s >> 7)))%s)%s) >> 8))%s", comp,
             biasStr[bias], scaleLeft[scale], sub ? '-' : '+', comp, comp, comp, comp, comp, scaleLeft[scale], lerpBias, scaleRight[scale]);
    return buf;
}

std::string swapString(const ShaderUid& u, uint32_t table) {
    uint32_t lo = u.ksel[table * 2], hi = u.ksel[table * 2 + 1];
    std::string s = ".";
    s += kComp[lo & 3];
    s += kComp[(lo >> 2) & 3];
    s += kComp[hi & 3];
    s += kComp[(hi >> 2) & 3];
    return s;
}

const char* alphaTest(uint32_t func, const char* ref) {
    static char buf[4][96];
    static int slot;
    char* b = buf[slot++ & 3];
    switch (func) {
    case 0:
        return "false";
    case 1:
        snprintf(b, 96, "(a < %s)", ref);
        return b;
    case 2:
        snprintf(b, 96, "(a == %s)", ref);
        return b;
    case 3:
        snprintf(b, 96, "(a <= %s)", ref);
        return b;
    case 4:
        snprintf(b, 96, "(a > %s)", ref);
        return b;
    case 5:
        snprintf(b, 96, "(a != %s)", ref);
        return b;
    case 6:
        snprintf(b, 96, "(a >= %s)", ref);
        return b;
    default:
        return "true";
    }
}

}  // namespace

std::string genFragmentShader(const ShaderUid& u) {
    Out o;
    o.w("#version 320 es\nprecision highp float;\nprecision highp int;\n");
    o.s += kUniformBlocks;
    for (int i = 0; i < 8; i++) {
        o.w("layout(binding = %d) uniform sampler2D s_tex%d;\n", i, i);
    }
    o.w("flat in uint v_pix;\n");
    o.w("in vec4 v_clr0;\nin vec4 v_clr1;\n");
    for (uint32_t i = 0; i < u.numTexGens; i++) o.w("in vec3 v_tex%u;\n", i);
    o.w("in float v_viewz;\nin float v_gxz;\nout vec4 o_color;\n");
    o.w("vec2 projUv(vec3 t) { return (t.z != 0.0) ? t.xy / t.z : t.xy; }\n");
    o.w("int idot(ivec3 a, ivec3 b) { ivec3 t = a * b; return t.x + t.y + t.z; }\n");
    if (((u.zTex >> 2) & 3) == 1) o.s += kZ24WindowDepthGlsl;
    if (u.cutaway) {
        o.w("in vec3 v_eye;\n");
        o.w("const float kBayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);\n");
    }

    o.w("void main() {\n");
    // VR cutaway (see gpu::EyeView): dither out 3D geometry between the eye
    // and the player, and geometry right in front of the eye.  Only in the
    // cutaway variant: a discard costs Adreno its early depth rejection.
    if (u.cutaway) {
    o.w("  if (vrFocus.w > 0.0) {\n");
    o.w("    float len = length(vrFocus.xyz);\n");
    o.w("    float along = clamp(dot(v_eye, vrFocus.xyz) / len, 0.0, len);\n");
    o.w("    float d = length(v_eye - vrFocus.xyz * (along / len));\n");
    o.w("    float radius = mix(vrCut.x, vrCut.y, along / len);\n");
    o.w("    float cut = (1.0 - smoothstep(radius * 0.7, radius, d)) * (1.0 - smoothstep(len - vrCut.z - 0.06, len - vrCut.z, along));\n");
    o.w("    cut = max(cut, 1.0 - smoothstep(vrCut.w * 0.6, vrCut.w, length(v_eye)));\n");
    o.w("    ivec2 q = ivec2(gl_FragCoord.xy) & 3;\n");
    o.w("    if (cut > (kBayer[q.y * 4 + q.x] + 0.5) / 16.0) discard;\n");
    o.w("  }\n");
    }
    o.w("  vec4 tevReg[4] = vec4[4](st[v_pix], st[v_pix + 1u], st[v_pix + 2u], st[v_pix + 3u]);\n");
    o.w("  ivec4 konst[4];\n  for (int i = 0; i < 4; i++) konst[i] = ivec4(round(st[v_pix + 4u + uint(i)] * 255.0));\n");
    o.w("  vec4 alphaRef = st[v_pix + 8u];\n  vec4 fogColor = st[v_pix + 9u];\n  vec4 fogParams = st[v_pix + 10u];\n");
    o.w("  vec4 fogRange = st[v_pix + 11u];\n");
    o.w("  vec4 texSize[8];\n  for (int i = 0; i < 8; i++) texSize[i] = st[v_pix + 12u + uint(i)];\n");
    o.w("  vec4 indMtx[6];\n  for (int i = 0; i < 6; i++) indMtx[i] = st[v_pix + 20u + uint(i)];\n");
    o.w("  ivec4 prev = ivec4(round(tevReg[0] * 255.0));\n  ivec4 c0 = ivec4(round(tevReg[1] * 255.0));\n");
    o.w("  ivec4 c1 = ivec4(round(tevReg[2] * 255.0));\n  ivec4 c2 = ivec4(round(tevReg[3] * 255.0));\n");
    o.w("  ivec4 tex = ivec4(0);\n  ivec4 ras = ivec4(0);\n  ivec3 kc = ivec3(0);\n  int ka = 0;\n");
    o.w("  ivec4 tevin_a, tevin_b, tevin_c, tevin_d;\n");
    o.w("  const ivec3 comp16 = ivec3(1, 256, 0);\n  const ivec3 comp24 = ivec3(1, 256, 65536);\n");
    if (u.zTex) {
        o.w("  ivec4 ztex = ivec4(0);\n");
    }

    // Indirect texture lookups (per indirect stage): 8-bit S/T/U offsets from
    // the texture's A/B/G channels.
    for (uint32_t s = 0; s < u.numIndStages; s++) {
        uint32_t map = (u.iref >> (s * 6)) & 7, coord = (u.iref >> (s * 6 + 3)) & 7;
        uint32_t scale = (u.indScale[s / 2] >> ((s & 1) * 8)) & 0xFF;
        uint32_t scaleS = scale & 15, scaleT = (scale >> 4) & 15;
        if (coord < u.numTexGens) {
            o.w("  vec3 ind%u = floor(texture(s_tex%u, projUv(v_tex%u) / vec2(%.1f, %.1f), texSize[%u].x).abg * 255.0 + 0.5);\n", s, map, coord,
                (double)(1u << (scaleS & 15)), (double)(1u << (scaleT & 15)), map);
        } else {
            o.w("  vec3 ind%u = vec3(0.0);\n", s);
        }
    }

    uint32_t lastColorDest = 0, lastAlphaDest = 0;
    for (uint32_t s = 0; s < u.numTevStages; s++) {
        uint32_t order = (u.tevOrder[s / 2] >> ((s & 1) * 12)) & 0xFFF;
        uint32_t map = order & 7, coord = (order >> 3) & 7, en = (order >> 6) & 1, chan = (order >> 7) & 7;
        uint32_t cc = u.tevColor[s], ac = u.tevAlpha[s];
        uint32_t ksel = u.ksel[s / 2] >> ((s & 1) * 10);
        uint32_t kcsel = (ksel >> 4) & 31, kasel = (ksel >> 9) & 31;
        uint32_t rswap = ac & 3, tswap = (ac >> 2) & 3;
        o.w("  // stage %u\n", s);

        // Texture coordinate (with optional indirect offset).
        std::string uv = "vec2(0.0)";
        if (coord < u.numTexGens) {
            char b[64];
            snprintf(b, sizeof(b), "projUv(v_tex%u)", coord);
            uv = b;
        }
        uint32_t ind = u.indCmd[s];
        if (u.numIndStages && ind) {
            uint32_t bt = ind & 3, bias = (ind >> 4) & 7, mtx = (ind >> 9) & 15;
            if (bt < u.numIndStages && (mtx & 3)) {
                uint32_t m = (mtx & 3) - 1;
                const char* bs = (bias & 1) ? "128.0" : "0.0";
                const char* bt2 = (bias & 2) ? "128.0" : "0.0";
                const char* bu = (bias & 4) ? "128.0" : "0.0";
                char b[512];
                snprintf(b, sizeof(b), "(%s + vec2(dot(indMtx[%u].xyz, ind%u - vec3(%s, %s, %s)), dot(indMtx[%u].xyz, ind%u - vec3(%s, %s, %s))) * texSize[%u].zw)",
                         uv.c_str(), m * 2, bt, bs, bt2, bu, m * 2 + 1, bt, bs, bt2, bu, map);
                uv = b;
            }
        }
        if (en) {
            o.w("  tex = ivec4(round(texture(s_tex%u, %s, texSize[%u].x)%s * 255.0));\n", map, uv.c_str(), map, swapString(u, tswap).c_str());
            if (u.zTex) {
                o.w("  ztex = tex;\n");
            }
        } else {
            o.w("  tex = ivec4(0);\n");
        }
        const char* rasSrc = chan == 0 ? "v_clr0" : chan == 1 ? "v_clr1" : "vec4(0.0)";
        o.w("  ras = ivec4(round(clamp(%s, 0.0, 1.0)%s * 255.0));\n", rasSrc, swapString(u, rswap).c_str());
        o.w("  kc = %s;\n  ka = %s;\n", konstColor(kcsel).c_str(), konstAlpha(kasel).c_str());
        // Inputs A, B and C are the low 8 bits of their source (a register's
        // sign and overflow bits are dropped); D keeps all 11 bits.
        {
            uint32_t ca = (cc >> 12) & 15, cb = (cc >> 8) & 15, cc2 = (cc >> 4) & 15, cd = cc & 15;
            uint32_t aa = (ac >> 13) & 7, ab = (ac >> 10) & 7, ac2 = (ac >> 7) & 7, ad = (ac >> 4) & 7;
            o.w("  tevin_a = ivec4(%s, %s) & 255;\n", kColorIn[ca], kAlphaIn[aa]);
            o.w("  tevin_b = ivec4(%s, %s) & 255;\n", kColorIn[cb], kAlphaIn[ab]);
            o.w("  tevin_c = ivec4(%s, %s) & 255;\n", kColorIn[cc2], kAlphaIn[ac2]);
            o.w("  tevin_d = ivec4(%s, %s);\n", kColorIn[cd], kAlphaIn[ad]);
        }

        // Color combiner.
        {
            uint32_t bias = (cc >> 16) & 3, sub = (cc >> 18) & 1, clampv = (cc >> 19) & 1, scale = (cc >> 20) & 3, dest = (cc >> 22) & 3;
            std::string expr;
            if (bias != 3) {
                expr = tevRegular("rgb", bias, sub, scale, false);
            } else {
                static const char* cmp[8] = {
                    "((tevin_a.r > tevin_b.r) ? tevin_c.rgb : ivec3(0))",
                    "((tevin_a.r == tevin_b.r) ? tevin_c.rgb : ivec3(0))",
                    "((idot(tevin_a.rgb, comp16) > idot(tevin_b.rgb, comp16)) ? tevin_c.rgb : ivec3(0))",
                    "((idot(tevin_a.rgb, comp16) == idot(tevin_b.rgb, comp16)) ? tevin_c.rgb : ivec3(0))",
                    "((idot(tevin_a.rgb, comp24) > idot(tevin_b.rgb, comp24)) ? tevin_c.rgb : ivec3(0))",
                    "((idot(tevin_a.rgb, comp24) == idot(tevin_b.rgb, comp24)) ? tevin_c.rgb : ivec3(0))",
                    "(max(sign(tevin_a.rgb - tevin_b.rgb), ivec3(0)) * tevin_c.rgb)",
                    "((ivec3(1) - sign(abs(tevin_a.rgb - tevin_b.rgb))) * tevin_c.rgb)",
                };
                expr = std::string("tevin_d.rgb + ") + cmp[(scale << 1) | sub];
            }
            o.w("  %s.rgb = clamp(%s, ivec3(%s), ivec3(%s));\n", kDest[dest], expr.c_str(), clampv ? "0" : "-1024", clampv ? "255" : "1023");
            if (s == u.numTevStages - 1) {
                lastColorDest = dest;
            }
        }
        // Alpha combiner (its compare modes other than A8 test the color
        // inputs).
        {
            uint32_t bias = (ac >> 16) & 3, sub = (ac >> 18) & 1, clampv = (ac >> 19) & 1, scale = (ac >> 20) & 3, dest = (ac >> 22) & 3;
            std::string expr;
            if (bias != 3) {
                expr = tevRegular("a", bias, sub, scale, true);
            } else {
                static const char* cmp[8] = {
                    "((tevin_a.r > tevin_b.r) ? tevin_c.a : 0)",
                    "((tevin_a.r == tevin_b.r) ? tevin_c.a : 0)",
                    "((idot(tevin_a.rgb, comp16) > idot(tevin_b.rgb, comp16)) ? tevin_c.a : 0)",
                    "((idot(tevin_a.rgb, comp16) == idot(tevin_b.rgb, comp16)) ? tevin_c.a : 0)",
                    "((idot(tevin_a.rgb, comp24) > idot(tevin_b.rgb, comp24)) ? tevin_c.a : 0)",
                    "((idot(tevin_a.rgb, comp24) == idot(tevin_b.rgb, comp24)) ? tevin_c.a : 0)",
                    "((tevin_a.a > tevin_b.a) ? tevin_c.a : 0)",
                    "((tevin_a.a == tevin_b.a) ? tevin_c.a : 0)",
                };
                expr = std::string("tevin_d.a + ") + cmp[(scale << 1) | sub];
            }
            o.w("  %s.a = clamp(%s, %s, %s);\n", kDest[dest], expr.c_str(), clampv ? "0" : "-1024", clampv ? "255" : "1023");
            if (s == u.numTevStages - 1) {
                lastAlphaDest = dest;
            }
        }
    }
    // The TEV's output is 8 bits wide: the upper bits are dropped, not
    // clamped (Mario's eyelids rely on it for their alpha test).
    o.w("  ivec4 outi = ivec4(%s.rgb, %s.a) & 255;\n", kDest[lastColorDest], kDest[lastAlphaDest]);
    o.w("  vec4 outc = vec4(outi) / 255.0;\n");
    // Alpha test (on the 8-bit value).
    {
        uint32_t f0 = u.alphaFunc & 7, f1 = (u.alphaFunc >> 3) & 7, logic = (u.alphaFunc >> 6) & 3;
        if (!(f0 == 7 && f1 == 7)) {
            o.w("  float a = float(outi.a);\n");
            const char* t0 = alphaTest(f0, "alphaRef.x");
            const char* t1 = alphaTest(f1, "alphaRef.y");
            const char* ops[4] = {"&&", "||", "!=", "=="};
            o.w("  if (!((%s) %s (%s))) discard;\n", t0, ops[logic], t1);
        }
    }
    // Z texture: the last texture sample, read as a 8/16/24-bit depth, plus
    // the bias, replaces or offsets the fragment depth.
    if (u.zTex) {
        uint32_t type = u.zTex & 3, op = (u.zTex >> 2) & 3;
        o.w("  {\n    ivec4 zt = ztex;\n");
        if (type == 0) {
            o.w("    int zv = zt.a;\n");
        } else if (type == 1) {
            o.w("    int zv = zt.r + zt.a * 256;\n");
        } else {
            o.w("    int zv = zt.r * 65536 + zt.g * 256 + zt.b;\n");
        }
        o.w("    zv += int(alphaRef.z);\n");
        if (op == 1) {
            o.w("    zv += int(gxWindowDepth(gl_FragCoord.z));\n");
        }
        o.w("    gl_FragDepth = float(zv & 0xFFFFFF) / 16777215.0;\n  }\n");
    }
    // Fog.
    // GX fog: ze = A / (B - (Z >> shift)) for perspective, A * Z for
    // orthographic (Z = 24-bit window depth), minus C; then the curve.
    uint32_t fogType = (u.fogSel >> 1) & 7, fogOrtho = u.fogSel & 1;
    if (fogType >= 2) {
        o.w("  {\n    float zc = floor(clamp(v_gxz, 0.0, 1.0) * 16777215.0);\n");
        if (fogOrtho) {
            o.w("    float ze = fogParams.x * zc / 16777216.0;\n");
        } else {
            o.w("    float ze = (fogParams.x * 16777216.0) / (fogParams.z - floor(zc / exp2(fogParams.w)));\n");
        }
        o.w("    float fz = clamp(ze - fogParams.y, 0.0, 1.0);\n");
        switch (fogType) {
        case 4:
            o.w("    fz = 1.0 - exp2(-8.0 * fz);\n");
            break;
        case 5:
            o.w("    fz = 1.0 - exp2(-8.0 * fz * fz);\n");
            break;
        case 6:
            o.w("    fz = exp2(-8.0 * (1.0 - fz));\n");
            break;
        case 7:
            o.w("    fz = exp2(-8.0 * (1.0 - fz) * (1.0 - fz));\n");
            break;
        default:
            break;
        }
        o.w("    outc.rgb = mix(outc.rgb, fogColor.rgb, fz);\n  }\n");
    }
    // Without an EFB alpha channel the blend still uses this alpha; the
    // renderer masks alpha writes instead.
    o.w("  o_color = clamp(outc, 0.0, 1.0);\n");
    // Debug views (PETARI_GLVIS=uv|tex): texture coordinates / raw texture 0.
    const char* vis = debugEnv("PETARI_GLVIS");
    if (vis && u.numTexGens > 0) {
        if (!strcmp(vis, "uv")) {
            o.w("  o_color = vec4(fract(projUv(v_tex0)), 0.0, 1.0);\n");
        } else if (!strcmp(vis, "red")) {
            o.w("  o_color = vec4(1.0, 0.0, 0.0, 1.0);\n");
        } else if (!strcmp(vis, "tex")) {
            o.w("  o_color = vec4(texture(s_tex0, projUv(v_tex0)).rgb, 1.0);\n");
        }
    }
    o.w("}\n");
    return o.s;
}

}  // namespace gpu
