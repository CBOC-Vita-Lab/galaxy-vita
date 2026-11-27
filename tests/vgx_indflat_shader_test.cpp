// GXV_IND_FLAT: the key the flattened stage makes generates a shader with one texture read and no indirect arithmetic; the key it replaces has
// the dependent read. Same colour arithmetic otherwise (the stage's TEV words are untouched). Host: no GL needed, the generator only.
#include "vgx_shadergen.h"
#include "vgx_indflat.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace vgx;
static int bad = 0;
#define CHECK(c) do { if (!(c)) { ++bad; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static int Count(const std::string& t, const char* w) { int n = 0; for (size_t p = t.find(w); p != std::string::npos; p = t.find(w, p + 1)) ++n; return n; }
int main() {
    // the structure of fs 1836 (scene 3 of Freezeflame): one stage, tex0 read through a 2x3 matrix from the indirect map tex1
    const uint32_t ind = (1u << 9) | (0u << 4); // matrix 0 (bits 9..12 non-zero), no bias/wrap/add-previous/bump alpha
    CHECK(indflat::Eligible(1, ind, true) && !indflat::Eligible(1, ind, false) && indflat::Eligible(2, ind, false));
    FsKey full{}, flat{};
    full.numStages = flat.numStages = 1;
    full.numTexGens = flat.numTexGens = 2;
    full.stage[0].color = flat.stage[0].color = 0x08FAF0u; // (any word: the colour stage is the same in both keys)
    full.stage[0].texEnable = flat.stage[0].texEnable = 1;
    full.stage[0].texMap = flat.stage[0].texMap = 0;
    full.stage[0].texCoord = flat.stage[0].texCoord = 0;
    full.numIndStages = 1; full.stage[0].ind = ind; full.ind[0].texMap = 1; full.ind[0].texCoord = 1;
    // what BuildFsKey makes when IndFlatMode() says the stage is eligible: the stage keeps ind = 0 and numIndStages = 0
    ShaderSource a, b;
    GenerateFragmentShader(full, a);
    GenerateFragmentShader(flat, b);
    const std::string ta = a.text, tb = b.text;
    CHECK(Count(ta, "texture2D") == 2 && Count(ta, "itc") > 0);
    CHECK(Count(tb, "texture2D") == 1 && Count(tb, "itc") == 0 && tb.find("ind0") == std::string::npos);
    CHECK(tb.size() < ta.size());
    std::printf("indflat shader: %d -> %d texture reads, %zu -> %zu bytes\n", Count(ta, "texture2D"), Count(tb, "texture2D"), ta.size(), tb.size());
    std::printf(bad ? "indflat shader: FAILED\n" : "indflat shader: ok\n");
    return bad ? 1 : 0;
}
