// Wave 6 combined-shader verification harness (host, no GLES/Vita needed).
// Exercises all 10 Wave 6 shader defaults enabled simultaneously with Galaxy 3.0's release
// shader stack, checking structural GLSL validity, u_tev compaction / gather bounds, zero
// FsRegs fallbacks, ATEARLY implicit-LOD safety, FS_PROJ non-dependent reads, VS_NOBRANCH
// vs VS_NOBRANCH_LIGHT split (prevents Sony Cg 85-reg multi-light blowup), FS_HALF_FOG,
// FS_TEXREUSE, TEVIN_PREWRAP, FS_NOCOLOR, FS_ADEAD, FS_ATFAST, and ProfileWord invariants.
#include "vgx_shadergen.h"
#include "vgx_keys27.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

namespace {
int g_fail = 0;
#define CHECK(cond, ...)                                                                         \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            ++g_fail;                                                                            \
            std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);                        \
            std::printf(__VA_ARGS__);                                                            \
            std::printf("\n");                                                                   \
        }                                                                                        \
    } while (0)

size_t CountSub(const std::string& s, const char* pat) {
    size_t n = 0, p = 0, len = std::strlen(pat);
    while ((p = s.find(pat, p)) != std::string::npos) { ++n; p += len; }
    return n;
}

uint32_t PackColorOp(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t bias, uint32_t op, uint32_t clamp, uint32_t scale, uint32_t dst) {
    return (d & 15u) | ((c & 15u) << 4) | ((b & 15u) << 8) | ((a & 15u) << 12) | ((bias & 3u) << 16) | ((op & 1u) << 18) | ((clamp & 1u) << 19) |
           ((scale & 3u) << 20) | ((dst & 3u) << 22);
}

uint32_t PackAlphaOp(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t bias, uint32_t op, uint32_t clamp, uint32_t scale, uint32_t dst) {
    return ((d & 7u) << 4) | ((c & 7u) << 7) | ((b & 7u) << 10) | ((a & 7u) << 13) | ((bias & 3u) << 16) | ((op & 1u) << 18) | ((clamp & 1u) << 19) |
           ((scale & 3u) << 20) | ((dst & 3u) << 22);
}

void CheckBalanced(const char* label, const std::string& txt) {
    int br = 0, pa = 0;
    for (char ch : txt) {
        if (ch == '{') ++br;
        else if (ch == '}') { --br; if (br < 0) break; }
        else if (ch == '(') ++pa;
        else if (ch == ')') { --pa; if (pa < 0) break; }
    }
    CHECK(br == 0 && pa == 0, "%s: unbalanced delimiters (br=%d pa=%d)", label, br, pa);
}

void CheckTevBounds(const char* label, const vgx::FsKey& k, const vgx::ShaderSource& src) {
    CHECK(src.text.find("u_tev[-") == std::string::npos, "%s: negative u_tev index", label);
    CHECK(src.nTev >= 1 && src.nTev <= 32, "%s: nTev=%u out of range", label, static_cast<unsigned>(src.nTev));
    const int canon = vgx::FsTevCount(k);
    CHECK(src.nTevCanon == static_cast<uint8_t>(canon), "%s: nTevCanon=%u != FsTevCount=%d", label, static_cast<unsigned>(src.nTevCanon), canon);
    if (src.tevGather) {
        for (uint8_t i = 0; i < src.nTev; ++i)
            CHECK(src.tevSrc[i] < src.nTevCanon, "%s: tevSrc[%u]=%u >= nTevCanon=%u", label, i, src.tevSrc[i], src.nTevCanon);
        // Verify FsTevGather matches canonical rows
        std::vector<float> canonBuf(static_cast<size_t>(canon) * 4u);
        for (size_t i = 0; i < canonBuf.size(); ++i) canonBuf[i] = static_cast<float>(i) * 0.25f + 1.0f;
        float scratch[128] = {};
        const float* got = vgx::FsTevGather(src, canonBuf.data(), canonBuf.size(), scratch);
        for (uint8_t i = 0; i < src.nTev; ++i) {
            for (int c = 0; c < 4; ++c) {
                const float want = canonBuf[static_cast<size_t>(src.tevSrc[i]) * 4u + static_cast<size_t>(c)];
                CHECK(got[i * 4 + c] == want, "%s: gather mismatch slot %u comp %d", label, i, c);
            }
        }
    }
    // Check every u_tev[idx] in the generated text is in [0, src.nTev)
    size_t pos = 0;
    while ((pos = src.text.find("u_tev[", pos)) != std::string::npos) {
        pos += 6;
        char* end = nullptr;
        const long idx = std::strtol(src.text.c_str() + pos, &end, 10);
        CHECK(end && *end == ']', "%s: malformed u_tev[", label);
        // Declaration `uniform vec4 u_tev[N];` or `uniform hvec4 u_tev[N];` has idx == nTev; runtime accesses have idx < nTev
        const size_t lnStart = src.text.rfind('\n', pos);
        const size_t ls = lnStart == std::string::npos ? 0u : lnStart + 1u;
        const bool isDecl = src.text.compare(ls, 8, "uniform ") == 0;
        if (isDecl) CHECK(idx == static_cast<long>(src.nTev), "%s: decl u_tev[%ld] != nTev %u", label, idx, src.nTev);
        else CHECK(idx >= 0 && idx < static_cast<long>(src.nTev), "%s: access u_tev[%ld] >= nTev %u", label, idx, src.nTev);
    }
    // Check AtEarly invariant: when phase 2 exists, discard precedes phase 2 and zero texture2D* occur in phase 2
    const size_t p2 = src.text.find("GXV_FS_ATEARLY phase 2:");
    if (p2 != std::string::npos) {
        const size_t disc = src.text.find("discard;");
        CHECK(disc != std::string::npos && disc < p2, "%s: discard not before phase 2", label);
        CHECK(src.text.find("texture2D", p2) == std::string::npos, "%s: texture2D call leaked into phase 2!", label);
    }
    // Vita PSP2 Cg strict half/float overload hygiene (catches mixed half/float that PC GLSL masks)
    if (src.text.find("#define hfloat half") != std::string::npos) {
        CHECK(src.text.find(" vec3 rc =") == std::string::npos, "%s: Vita Cg hazard: float vec3 rc in half shader", label);
        CHECK(src.text.find("dot(a.rg - b.rg, vec2(") == std::string::npos, "%s: Vita Cg hazard: dot(half2, float2)", label);
        CHECK(src.text.find("dot(a.rgb - b.rgb, vec3(") == std::string::npos, "%s: Vita Cg hazard: dot(half3, float3)", label);
        CHECK(src.text.find("vec3(1.0) - fin.rgb") == std::string::npos, "%s: Vita Cg hazard: float3 - half3 logicOp", label);
        CHECK(src.text.find("vgx_wrap_final(") == std::string::npos, "%s: Vita Cg hazard: float final wrap in half shader", label);
    }
}

void CheckDeclaredRefs(const char* label, const std::string& txt) {
    // Check that any uvN used in the shader was declared as `vec2 uvN`
    for (int i = 0; i < 8; ++i) {
        const std::string nm = "uv" + std::to_string(i);
        const std::string decl = "vec2 " + nm + " =";
        size_t pos = 0;
        while ((pos = txt.find(nm, pos)) != std::string::npos) {
            const bool leftId = pos > 0 && (std::isalnum(static_cast<unsigned char>(txt[pos - 1])) || txt[pos - 1] == '_');
            const size_t end = pos + nm.size();
            const bool rightId = end < txt.size() && (std::isalnum(static_cast<unsigned char>(txt[end])) || txt[end] == '_');
            if (!leftId && !rightId) {
                const size_t dp = txt.find(decl);
                CHECK(dp != std::string::npos && dp <= pos, "%s: %s used before/without declaration", label, nm.c_str());
            }
            pos = end;
        }
    }
    // Check that any indN used was declared as `vec3 indN`
    for (int i = 0; i < 4; ++i) {
        const std::string nm = "ind" + std::to_string(i);
        const std::string decl = "vec3 " + nm + " =";
        size_t pos = 0;
        while ((pos = txt.find(nm, pos)) != std::string::npos) {
            const bool leftId = pos > 0 && (std::isalnum(static_cast<unsigned char>(txt[pos - 1])) || txt[pos - 1] == '_');
            const size_t end = pos + nm.size();
            const bool rightId = end < txt.size() && (std::isalnum(static_cast<unsigned char>(txt[end])) || txt[end] == '_');
            if (!leftId && !rightId) {
                const size_t dp = txt.find(decl);
                CHECK(dp != std::string::npos && dp <= pos, "%s: %s used before/without declaration", label, nm.c_str());
            }
            pos = end;
        }
    }
}

} // namespace

int main() {
    // 1. Verify VS_NOBRANCH vs VS_NOBRANCH_LIGHT split (prevents Sony Cg 85-reg multi-light blowup)
    {
        vgx::VsKey vk{};
        vk.perVtxPos = 1;
        vk.numChans = 1;
        vk.numTexGens = 1;
        vk.chan[0].enable = 1;
        vk.chan[0].matSrcVtx = 1;
        vk.chan[0].diffuse = 2;
        vk.chan[0].attn = 1;        // specular spot gate
        vk.chan[0].lightMask = 0x1F; // 5 lights (the Strikers 85-reg blowup case!)
        vk.tg[0].src = 5;
        vk.tg[0].proj = 1;
        vk.tg[0].post = 2;
        vk.tg[0].perVtxMtx = 1;

        vgx::ShaderSource vsOff, vsSplit, vsLight, vsLightOnly;
        vgx::FsProjModeForTest(1);
        vgx::VsNoBranchModeForTest(0); vgx::VsNoBranchLightModeForTest(0); vgx::GenerateVertexShader(vk, vsOff);
        vgx::VsNoBranchModeForTest(0); vgx::VsNoBranchLightModeForTest(1); vgx::GenerateVertexShader(vk, vsLightOnly);
        vgx::VsNoBranchModeForTest(1); vgx::VsNoBranchLightModeForTest(0); vgx::GenerateVertexShader(vk, vsSplit);
        vgx::VsNoBranchModeForTest(1); vgx::VsNoBranchLightModeForTest(1); vgx::GenerateVertexShader(vk, vsLight);
        vgx::VsNoBranchModeForTest(-1); vgx::VsNoBranchLightModeForTest(-1); vgx::FsProjModeForTest(-1);

        CHECK(vsOff.text == vsLightOnly.text, "GXV_VS_NOBRANCH_LIGHT=1 alone without GXV_VS_NOBRANCH=1 must be a no-op");
        CHECK(CountSub(vsSplit.text, "if (d1 > 0.0) ld = ld / d1;") == 5, "5-light split VS keeps per-light d1>0 branch (got %zu)",
              CountSub(vsSplit.text, "if (d1 > 0.0) ld = ld / d1;"));
        CHECK(CountSub(vsSplit.text, "dot(n, ld) >= 0.0 ?") == 5, "5-light split VS keeps per-light spot select (got %zu)",
              CountSub(vsSplit.text, "dot(n, ld) >= 0.0 ?"));
        CHECK(CountSub(vsSplit.text, "if (") == 5, "5-light split VS eliminates ALL non-light ifs (got %zu)", CountSub(vsSplit.text, "if ("));
        CHECK(vsSplit.text.find("tc.z += step(abs(tc.z), 0.0);") != std::string::npos, "split VS emits branchless FS_PROJ q==0 guard");
        CHECK(vsSplit.text.find("n = n / (nl + step(nl, 0.0));") != std::string::npos, "split VS emits branchless normalise");
        CHECK(CountSub(vsLight.text, "if (") == 0, "GXV_VS_NOBRANCH_LIGHT=1 eliminates all ifs (got %zu)", CountSub(vsLight.text, "if ("));
        CheckBalanced("vsSplit", vsSplit.text);
        CheckBalanced("vsLight", vsLight.text);
    }

    // 2. Verify specific Wave 6 features active under the release environment
    CHECK(vgx::FsProjMode() == 1, "expected GXV_FS_PROJ=1");
    CHECK(vgx::VsNoBranchMode() == 1, "expected GXV_VS_NOBRANCH=1");
    CHECK(vgx::VsNoBranchLightMode() == 0, "expected GXV_VS_NOBRANCH_LIGHT=0");
    CHECK(vgx::FsHalfFogMode(), "expected GXV_FS_HALF_FOG=1");
    CHECK(vgx::FsNoColorMode(), "expected GXV_FS_NOCOLOR=1");
    CHECK(vgx::FsAlphaDeadMode() == 1, "expected GXV_FS_ADEAD=1");
    CHECK(vgx::FsRegsMode() == 1, "expected GXV_FS_REGS=1");
    CHECK(vgx::FsAtEarlyMode() == 1, "expected GXV_FS_ATEARLY=1");
    CHECK(vgx::FsAtFastMode() == 1, "expected GXV_FS_ATFAST=1");
    CHECK(vgx::FsTexReuseMode() == 1, "expected GXV_FS_TEXREUSE=1");
    CHECK(vgx::TevInPrewrapMode(), "expected GXV_TEVIN_PREWRAP=1");

    // 3. Targeted feature checks with all 10 Wave 6 switches active simultaneously
    // 3a. Vertex-fogged 3D world shader -> half-precision + FogHalfLanes + compacted u_tev[] + texture2DProj
    {
        vgx::FsKey k{};
        k.numStages = 2;
        k.numTexGens = 2;
        k.texProjMask = 1; // tex0 projective, tex1 vec2
        k.setTexVec2(1);
        k.fog = static_cast<uint8_t>(2 | vgx::kFogVtxBit | vgx::kFogHalfBit);
        k.stage[0].texEnable = 1; k.stage[0].texMap = 0; k.stage[0].texCoord = 0; k.stage[0].rasChan = 0;
        k.stage[0].color = PackColorOp(15, 8, 10, 15, 0, 0, 1, 0, 0); // tex * ras -> prev
        k.stage[0].alpha = PackAlphaOp(7, 4, 5, 7, 0, 0, 1, 0, 0);
        // Stage 1 repeats the exact same projective read (FS_TEXREUSE!) and adds 0.5 * tex to prev (realistic 2-stage lit+detail world shader, error <= 1.0 level)
        k.stage[1].texEnable = 1; k.stage[1].texMap = 0; k.stage[1].texCoord = 0; k.stage[1].rasChan = 0;
        k.stage[1].color = PackColorOp(15, 8, 13, 0, 0, 0, 1, 0, 0); // prev + mix(0, tex, 0.5)
        k.stage[1].alpha = PackAlphaOp(7, 4, 7, 0, 0, 0, 1, 0, 0);
        vgx::ShaderSource src;
        vgx::GenerateFragmentShader(k, src);
        const vgx::FsHalfReport hr = vgx::FsHalfAnalyse(k);
        CHECK(src.text.find("#define hfloat half") != std::string::npos,
              "fogged 3D world shader unlocked fp16 via GXV_FS_HALF_FOG (elig=%d why='%s' c=%.3f a=%.3f)",
              hr.eligibleShape ? 1 : 0, hr.why, hr.colorLevels, hr.alphaLevels);
        CHECK(src.text.find("texture2DProj(u_tex0, v_tex0)") != std::string::npos, "projective read uses non-dependent texture2DProj");
        CHECK(CountSub(src.text, "texture2DProj(") == 1, "FS_TEXREUSE deduplicated repeated projective read");
        CHECK(src.texReused == 1, "texReused=%u expected 1", src.texReused);
        CHECK(src.tevGather && src.nTev < src.nTevCanon, "FS_REGS compacted u_tev (%u -> %u)", src.nTevCanon, src.nTev);
        CheckBalanced("3a", src.text);
        CheckTevBounds("3a", k, src);
        CheckDeclaredRefs("3a", src.text);
    }

    // 3b. Alpha-tested foliage shader with discard + FS_ATEARLY + FS_REGS + FS_ATFAST + FS_PROJ + FS_TEXREUSE
    {
        vgx::FsKey k{};
        k.numStages = 3;
        k.numTexGens = 2;
        k.texProjMask = 1;
        k.setTexVec2(1);
        k.alphaTestOn = 1;
        k.alphaTest = static_cast<uint8_t>(4 | (7 << 3) | (0 << 6)); // GREATER AND ALWAYS -> AtFastKind 1
        k.stage[0].texEnable = 1; k.stage[0].texMap = 0; k.stage[0].texCoord = 0; k.stage[0].rasChan = 0;
        k.stage[0].color = PackColorOp(15, 8, 10, 15, 0, 0, 1, 0, 1); // -> c0
        k.stage[0].alpha = PackAlphaOp(7, 4, 5, 7, 0, 0, 1, 0, 1);    // -> c0.a
        k.stage[1].texEnable = 1; k.stage[1].texMap = 0; k.stage[1].texCoord = 0; k.stage[1].rasChan = 0; // duplicate read!
        k.stage[1].color = PackColorOp(2, 8, 3, 15, 0, 0, 1, 0, 0);   // reads c0.rgb and c0.aaa (overwritten in stage 2 -> snapshot!)
        k.stage[1].alpha = PackAlphaOp(1, 4, 6, 7, 0, 0, 1, 0, 0);
        k.stage[2].texEnable = 1; k.stage[2].texMap = 1; k.stage[2].texCoord = 1; k.stage[2].rasChan = 7;
        k.stage[2].color = PackColorOp(0, 8, 14, 15, 0, 0, 1, 0, 0);
        k.stage[2].alpha = PackAlphaOp(0, 4, 7, 7, 0, 0, 1, 0, 1); // overwrites c0.a; last stage writes prev/c0
        vgx::ShaderSource src;
        vgx::GenerateFragmentShader(k, src);
        CHECK(src.text.find("GXV_FS_ATEARLY phase 2:") != std::string::npos, "ATEARLY split alpha-tested foliage shader");
        CHECK(src.text.find("at_sa0") != std::string::npos, "ATEARLY snapshotted overwritten c0.a for phase 2 colour");
        CHECK(src.text.find("fin_a >= u_tev[") != std::string::npos, "FS_ATFAST active inside ATEARLY+REGS");
        CHECK(src.text.find("texture2DProj(u_tex0, v_tex0)") != std::string::npos, "FS_PROJ active inside ATEARLY+REGS");
        CHECK(src.texReused == 1, "FS_TEXREUSE active inside ATEARLY+REGS");
        CHECK(src.tevGather, "FS_REGS active inside ATEARLY");
        CheckBalanced("3b", src.text);
        CheckTevBounds("3b", k, src);
        CheckDeclaredRefs("3b", src.text);
    }

    // 3c. Dual-wrap Mario shader (GXV_TEVIN_PREWRAP + GXV_FS_REGS + GXV_FS_ADEAD)
    {
        vgx::FsKey k{};
        k.numStages = 2;
        k.numTexGens = 1;
        k.setTexVec2(1);
        k.inWrap = 1;
        k.setAlphaDead(1);
        k.stage[0].texEnable = 1; k.stage[0].texMap = 0; k.stage[0].texCoord = 0; k.stage[0].rasChan = 0;
        // Reads c0 (reg 1) as BOTH A (sel=2) and D (sel=2) -> dualWrap bit 1!
        k.stage[0].color = PackColorOp(2, 8, 10, 2, 0, 0, 1, 0, 0);
        k.stage[0].alpha = PackAlphaOp(1, 4, 5, 1, 0, 0, 1, 0, 0);
        k.stage[1].texEnable = 0; k.stage[1].rasChan = 7;
        k.stage[1].color = PackColorOp(0, 12, 14, 15, 0, 0, 1, 0, 0);
        k.stage[1].alpha = PackAlphaOp(0, 7, 7, 7, 0, 0, 1, 0, 0);
        k.setPreWrap(vgx::FsTevInPreWrapMask(k));
        k.setDualWrap(vgx::FsTevInDualMask(k));
        CHECK((k.dualWrap() & 2u) != 0, "expected dualWrap bit 1 set");
        vgx::ShaderSource src;
        vgx::GenerateFragmentShader(k, src);
        CHECK(src.tevGather, "FS_REGS active on dualWrap+alphaDead key");
        CHECK(src.text.find("fin.a = 0.0;") != std::string::npos || src.text.find("fin.a = hfloat(0.0);") != std::string::npos,
              "FS_ADEAD zeroed alpha");
        CheckBalanced("3c", src.text);
        CheckTevBounds("3c", k, src);
        CheckDeclaredRefs("3c", src.text);
    }

    // 3d. Color-dead shadow/depth pass with BOTH colorDead + alphaDead + fog + FS_REGS
    {
        vgx::FsKey k{};
        k.numStages = 1;
        k.numTexGens = 1;
        k.setTexVec2(1);
        k.fog = static_cast<uint8_t>(2 | vgx::kFogVtxBit | vgx::kFogHalfBit);
        k.setColorDead(1);
        k.setAlphaDead(1);
        k.stage[0].texEnable = 1; k.stage[0].texMap = 0; k.stage[0].texCoord = 0; k.stage[0].rasChan = 0;
        k.stage[0].color = PackColorOp(15, 8, 10, 15, 0, 0, 1, 0, 0);
        k.stage[0].alpha = PackAlphaOp(7, 4, 5, 7, 0, 0, 1, 0, 0);
        vgx::ShaderSource src;
        vgx::GenerateFragmentShader(k, src);
        CHECK(src.text.find("float fg =") == std::string::npos, "FS_NOCOLOR dropped fog arithmetic");
        CHECK(src.text.find("fin.rgb = hvec3(0.0);") != std::string::npos || src.text.find("fin.rgb = vec3(0.0);") != std::string::npos,
              "FS_NOCOLOR zeroed colour");
        CheckBalanced("3d", src.text);
        CheckTevBounds("3d", k, src);
        CheckDeclaredRefs("3d", src.text);
    }

    // 3e. Carbon regression #1: colorDead + colour compare-mode stage MUST stay float (prevents Vita Cg half/float compile error)
    for (uint32_t scale = 0; scale < 4; ++scale) {
        vgx::FsKey k{};
        k.numStages = 1;
        k.numTexGens = 1;
        k.setTexVec2(1);
        k.setColorDead(1);
        k.stage[0].texEnable = 1; k.stage[0].texMap = 0; k.stage[0].texCoord = 0; k.stage[0].rasChan = 0;
        k.stage[0].color = PackColorOp(8, 10, 12, 15, 3 /* compare */, 0, 1, scale, 0);
        k.stage[0].alpha = PackAlphaOp(7, 4, 5, 7, 0, 0, 1, 0, 0);
        vgx::ShaderSource src;
        vgx::GenerateFragmentShader(k, src);
        CHECK(src.text.find("#define hfloat half") == std::string::npos, "colorDead + compare scale=%u must stay float", scale);
        CheckBalanced("3e", src.text);
        CheckTevBounds("3e", k, src);
    }

    // 3f. Carbon regression #2: folded (regFromVtx=3) + inWrap=1 + dualWrap matches CanonListFsKey and keys27::Siblings
    {
        vgx::FsKey base{};
        base.numStages = 2;
        base.numTexGens = 1;
        base.setTexVec2(1);
        base.inWrap = 1;
        // Stage 0 reads PREV (r=0) and REG0/c0 (r=1) as both A and D -> dualWrap has bits 0 and 1!
        base.stage[0].texEnable = 1; base.stage[0].texMap = 0; base.stage[0].texCoord = 0; base.stage[0].rasChan = 7;
        base.stage[0].color = PackColorOp(2, 0, 8, 2, 0, 0, 1, 0, 0);
        base.stage[0].alpha = PackAlphaOp(0, 1, 4, 0, 0, 0, 1, 0, 0);
        // Stage 1 reads REG1/c1 (r=2) only as A -> preWrap has bit 2!
        base.stage[1].texEnable = 0; base.stage[1].rasChan = 7;
        base.stage[1].color = PackColorOp(4, 15, 12, 0, 0, 0, 1, 0, 0);
        base.stage[1].alpha = PackAlphaOp(2, 7, 7, 0, 0, 0, 1, 0, 0);
        base.setPreWrap(vgx::FsTevInPreWrapMask(base));
        base.setDualWrap(vgx::FsTevInDualMask(base));
        CHECK((base.preWrap() & 0x44u) != 0 && (base.dualWrap() & 0x02u) != 0, "base key has c0 dualWrap and c1 preWrap");

        vgx::FsKey folded = base;
        folded.regFromVtx = 3;
        vgx::CanonListFsKey(folded);
        CHECK((folded.preWrap() & 0x66u) == 0 && (folded.dualWrap() & 0x06u) == 0, "CanonListFsKey stripped c0/c1 wrap bits on folded key");
        CHECK((folded.dualWrap() & 0x01u) != 0, "CanonListFsKey kept PREV dualWrap bit on folded key");

        vgx::FsKey sib[8];
        const int n = vgx::keys27::Siblings(base, true, sib, 8);
        bool matchedFold = false;
        for (int i = 0; i < n; ++i) {
            if (sib[i].regFromVtx == 3 && !sib[i].alphaDead()) {
                vgx::FsKey canon = sib[i];
                vgx::CanonListFsKey(canon);
                CHECK(std::memcmp(&sib[i], &canon, sizeof canon) == 0, "keys27::Siblings folded key matches CanonListFsKey byte-for-byte");
                CHECK(vgx::HashKey(&sib[i], sizeof sib[i]) == vgx::HashKey(&folded, sizeof folded), "folded sibling key hash matches runtime key");
                matchedFold = true;
            }
        }
        CHECK(matchedFold, "keys27::Siblings emitted folded sibling");
        vgx::ShaderSource src;
        vgx::GenerateFragmentShader(folded, src);
        CheckBalanced("3f", src.text);
        CheckTevBounds("3f", folded, src);
    }

    // 4. Stress-fuzz 1,500 diverse FsKey + VsKey combinations under ALL Wave 6 defaults ON simultaneously!
    vgx::FsRegsStats regsBefore{};
    vgx::FsRegsCounters(regsBefore);
    uint32_t rng = 0x5EED2026u;
    auto Rnd = [&]() { rng = rng * 1664525u + 1013904223u; return rng; };
    unsigned testedFs = 0, testedVs = 0;
    for (int iter = 0; iter < 1500; ++iter) {
        vgx::FsKey fk{};
        fk.numStages = static_cast<uint8_t>(1 + (Rnd() % 5));
        fk.numTexGens = static_cast<uint8_t>(Rnd() % 5);
        fk.texProjMask = static_cast<uint8_t>(Rnd() & ((1u << fk.numTexGens) - 1u));
        fk.setTexVec2(1);
        const unsigned fogCase = Rnd() % 4;
        if (fogCase == 1) fk.fog = static_cast<uint8_t>(2 | vgx::kFogVtxBit);
        else if (fogCase == 2) fk.fog = static_cast<uint8_t>(2 | vgx::kFogVtxBit | vgx::kFogHalfBit);
        else if (fogCase == 3) fk.fog = static_cast<uint8_t>(4 | (Rnd() & 8));
        fk.alphaTestOn = static_cast<uint8_t>((Rnd() % 3) == 0 ? 1 : 0);
        if (fk.alphaTestOn) {
            static const uint8_t kComps[] = {1, 2, 3, 4, 5, 6, 7};
            const uint8_t c0 = kComps[Rnd() % 7], c1 = kComps[Rnd() % 7], op = static_cast<uint8_t>(Rnd() & 3);
            fk.alphaTest = static_cast<uint8_t>(c0 | (c1 << 3) | (op << 6));
            if ((Rnd() % 5) == 0) fk.setRejectZero(1);
            else if ((Rnd() % 7) == 0) fk.setPreAlpha(1);
            else if ((Rnd() % 9) == 0) fk.setAtPass(1);
        } else {
            if ((Rnd() % 3) == 0) fk.setAlphaDead(1);
        }
        if ((Rnd() % 5) == 0) fk.setColorDead(1);
        if ((Rnd() % 6) == 0 && !fk.alphaTestOn) fk.dstAlpha = 1;
        if ((Rnd() % 5) == 0 && !(fk.fog & vgx::kFogHalfBit)) {
            fk.numIndStages = static_cast<uint8_t>(1 + (Rnd() % 2));
            for (int i = 0; i < fk.numIndStages; ++i) {
                fk.ind[i].texMap = static_cast<uint8_t>(Rnd() % 4);
                fk.ind[i].texCoord = fk.numTexGens ? static_cast<uint8_t>(Rnd() % fk.numTexGens) : 0;
            }
        }
        fk.regFromVtx = static_cast<uint8_t>(Rnd() % 4);
        if ((Rnd() % 6) == 0 && !(fk.fog & vgx::kFogHalfBit)) fk.inWrap = 1;

        for (int s = 0; s < fk.numStages; ++s) {
            auto& st = fk.stage[s];
            st.texEnable = (fk.numTexGens > 0 && (Rnd() % 3) != 0) ? 1 : 0;
            if (st.texEnable) {
                if (s > 0 && (Rnd() % 3) == 0 && fk.stage[s - 1].texEnable) {
                    st.texMap = fk.stage[s - 1].texMap;
                    st.texCoord = fk.stage[s - 1].texCoord;
                    st.texSwap = fk.stage[s - 1].texSwap;
                    st.lodBias = fk.stage[s - 1].lodBias;
                } else {
                    st.texMap = static_cast<uint8_t>(Rnd() % 4);
                    st.texCoord = static_cast<uint8_t>(Rnd() % fk.numTexGens);
                    st.lodBias = (Rnd() % 7) == 0 ? 1 : 0;
                }
            }
            if (fk.numIndStages > 0 && st.texEnable && (Rnd() % 3) == 0) {
                const uint32_t bt = Rnd() % fk.numIndStages;
                const uint32_t mid = 1u + (Rnd() % 3);
                const uint32_t addPrev = (s > 0 && (Rnd() % 4) == 0) ? 1u : 0u;
                st.ind = bt | (mid << 9) | (addPrev << 20);
            }
            st.rasChan = static_cast<uint8_t>((Rnd() % 3) == 0 ? 7 : (Rnd() % 2));
            st.kcsel = static_cast<uint8_t>(Rnd() % 32);
            st.kasel = static_cast<uint8_t>(Rnd() % 32);
            const bool cmpC = (Rnd() % 12) == 0;
            const bool cmpA = (Rnd() % 12) == 0;
            st.color = PackColorOp(Rnd() % 16, Rnd() % 16, Rnd() % 16, Rnd() % 16, cmpC ? 3 : (Rnd() % 3), Rnd() & 1, Rnd() & 1, Rnd() & 3, Rnd() & 3);
            st.alpha = PackAlphaOp(Rnd() % 8, Rnd() % 8, Rnd() % 8, Rnd() % 8, cmpA ? 3 : (Rnd() % 3), Rnd() & 1, Rnd() & 1, Rnd() & 3, Rnd() & 3);
        }
        if (fk.inWrap & 1) {
            fk.setPreWrap(vgx::FsTevInPreWrapMask(fk));
            fk.setDualWrap(vgx::FsTevInDualMask(fk));
        }
        if (fk.fog & vgx::kFogHalfBit) {
            if (!vgx::FsHalfEligible(fk)) fk.fog = static_cast<uint8_t>(fk.fog & ~vgx::kFogHalfBit);
        }
        vgx::IndPreMark(fk);

        vgx::ShaderSource fs;
        vgx::GenerateFragmentShader(fk, fs);
        char lbl[48];
        std::snprintf(lbl, sizeof lbl, "fuzz-fs-%d", iter);
        CheckBalanced(lbl, fs.text);
        CheckTevBounds(lbl, fk, fs);
        CheckDeclaredRefs(lbl, fs.text);
        ++testedFs;

        if ((iter % 3) == 0) {
            vgx::VsKey vk{};
            vk.numTexGens = fk.numTexGens;
            vk.numChans = static_cast<uint8_t>(1 + (Rnd() % 2));
            vk.perVtxPos = static_cast<uint8_t>(Rnd() % 4);
            if (fk.fog & vgx::kFogVtxBit) vk.fogVtx = static_cast<uint8_t>((fk.fog & 8) ? 3 : 1);
            for (int c = 0; c < 4; ++c) {
                vk.chan[c].enable = static_cast<uint8_t>(Rnd() & 1);
                vk.chan[c].matSrcVtx = static_cast<uint8_t>(Rnd() & 1);
                vk.chan[c].ambSrcVtx = static_cast<uint8_t>(Rnd() & 1);
                vk.chan[c].diffuse = static_cast<uint8_t>(Rnd() % 3);
                vk.chan[c].attn = static_cast<uint8_t>(Rnd() % 3);
                vk.chan[c].lightMask = static_cast<uint8_t>(Rnd() & 0x3Fu);
            }
            for (int t = 0; t < vk.numTexGens; ++t) {
                vk.tg[t].type = static_cast<uint8_t>((Rnd() % 5) == 0 ? 2 : 0);
                vk.tg[t].src = static_cast<uint8_t>((Rnd() % 3) == 0 ? 1 : (5 + (t & 3)));
                vk.tg[t].proj = static_cast<uint8_t>((fk.texProjMask >> t) & 1u);
                vk.tg[t].post = static_cast<uint8_t>(Rnd() % 3);
                vk.tg[t].perVtxMtx = static_cast<uint8_t>(Rnd() & 1);
            }
            vgx::TexVec2Pair(fk, vk);
            vgx::VsOutPrune(vk, fk, false);
            vgx::ShaderSource vs;
            vgx::GenerateVertexShader(vk, vs);
            std::snprintf(lbl, sizeof lbl, "fuzz-vs-%d", iter);
            CheckBalanced(lbl, vs.text);
            ++testedVs;
        }
    }
    vgx::FsRegsStats regsAfter{};
    vgx::FsRegsCounters(regsAfter);
    CHECK(regsAfter.fallbacks == regsBefore.fallbacks, "FS_REGS had %llu fallback(s) (missingSlot bug!)",
          static_cast<unsigned long long>(regsAfter.fallbacks - regsBefore.fallbacks));
    vgx::FsAtEarlyStats atStats{};
    vgx::FsAtEarlyCounters(atStats);
    CHECK(atStats.split > 100, "expected many ATEARLY splits in fuzz (got %llu)", static_cast<unsigned long long>(atStats.split));

    if (g_fail) {
        std::printf("FAILED (%d failures)\n", g_fail);
        return 1;
    }
    std::printf("PASS: Wave 6 combined-shader verification (%u FS, %u VS, %llu ATEARLY splits, 0 FS_REGS fallbacks, avg u_tev %.1f -> %.1f rows)\n",
                testedFs, testedVs, static_cast<unsigned long long>(atStats.split),
                regsAfter.shaders ? static_cast<double>(regsAfter.rowsOld) / static_cast<double>(regsAfter.shaders) : 0.0,
                regsAfter.shaders ? static_cast<double>(regsAfter.rowsNew) / static_cast<double>(regsAfter.shaders) : 0.0);
    return 0;
}
