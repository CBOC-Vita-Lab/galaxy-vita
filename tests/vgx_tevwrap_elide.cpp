#include "vgx_shadergen.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vgx { bool LogEnabled() { return false; } }
namespace {
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
uint32_t Copy(bool color, unsigned arg, unsigned dest, bool clamp, bool fromD = false) {
    const unsigned z = color ? 15 : 7;
    const unsigned a = fromD ? z : arg, d = fromD ? arg : z;
    return (color ? (a << 12) | (z << 8) | (z << 4) | d :
                    (a << 13) | (z << 10) | (z << 7) | (d << 4)) |
           (dest << 22) | (unsigned(clamp) << 19);
}
vgx::FsKey Chain(unsigned source, unsigned dest, bool fromD) {
    vgx::FsKey k; k.numStages = 2;
    k.stage[0].color = Copy(true, 2, source, true);
    k.stage[0].alpha = Copy(false, 1, source, true);
    k.stage[1].color = Copy(true, source * 2, dest, false, fromD);
    k.stage[1].alpha = Copy(false, source, dest, false, fromD);
    return k;
}
size_t checked = 0;
uint64_t digest = 0;
bool enabled = false, wrapping = false;
void Expect(const vgx::FsKey& k, bool boundedColor, bool boundedAlpha, const char* why) {
    vgx::ShaderSource s; vgx::GenerateFragmentShader(k, s);
    const bool colorWrap = s.text.find("fin.rgb = vec3(vgx_wrap_final") != std::string::npos;
    const bool alphaWrap = s.text.find("fin.a = vgx_wrap_final") != std::string::npos;
    const auto& last = k.stage[k.numStages - 1];
    Require(colorWrap == (wrapping && !(last.color & (1u << 19)) && !(enabled && boundedColor)), why);
    Require(alphaWrap == (wrapping && !(last.alpha & (1u << 19)) && !(enabled && boundedAlpha)), why);
    digest ^= vgx::HashKey(s.text.data(), s.text.size()) + checked * 0x9e3779b97f4a7c15ull;
    ++checked;
}
void Corpus(const char* path) {
    struct Header { char magic[8]; uint32_t vs, fs, version, reserved; } h{};
    FILE* f = std::fopen(path, "rb"); Require(f, "open caller-supplied recipe");
    Require(std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, "VGXSHL01", 8) == 0 &&
            h.vs == sizeof(vgx::VsKey) && h.fs == sizeof(vgx::FsKey) && h.version >= 2 && h.version <= 4,
            "compatible recipe layout (read-only audit, not cache replay)");
    size_t fragments = 0, calls = 0, bytes = 0;
    uint64_t hash = 0;
    uint32_t type;
    while (const size_t n = std::fread(&type, 1, sizeof type, f)) {
        Require(n == sizeof type, "complete recipe record type");
        if (type == 2) {
            vgx::FsKey k{}; Require(std::fread(&k, sizeof k, 1, f) == 1, "complete fragment record");
            Require(k.numStages >= 1 && k.numStages <= 16, "valid stage count");
            vgx::ShaderSource s; vgx::GenerateFragmentShader(k, s);
            calls += s.text.find("fin.rgb = vec3(vgx_wrap_final") != std::string::npos ? 3 : 0;
            calls += s.text.find("fin.a = vgx_wrap_final") != std::string::npos ? 1 : 0;
            bytes += s.text.size(); hash ^= vgx::HashKey(s.text.data(), s.text.size()) + fragments++;
        } else {
            const size_t size = type == 1 ? sizeof(vgx::VsKey) : type == 3 ? 8 : type == 4 ? 16 : 0;
            Require(size, "known recipe record");
            unsigned char payload[sizeof(vgx::VsKey)]{};
            Require(std::fread(payload, size, 1, f) == 1, "complete nonfragment record");
        }
    }
    Require(std::feof(f) && !std::ferror(f), "recipe read reaches clean EOF"); std::fclose(f);
    std::printf("CORPUS: fragments=%zu scalar-wrap-calls=%zu source-bytes=%zu digest=%016llx\n",
                fragments, calls, bytes, static_cast<unsigned long long>(hash));
}
}
int main(int argc, char** argv) {
    Require(argc == 3 || argc == 4, "mode and wrap arguments, optional caller-supplied recipe");
    Require(vgx::kShaderGenVersion == 3, "optional source elision preserves the byte-wrap baseline cache namespace");
    enabled = std::strcmp(argv[1], "on") == 0;
    wrapping = std::strcmp(argv[2], "on") == 0;
    const auto first = Chain(0, 0, false);
    vgx::ShaderSource initial, again; vgx::GenerateFragmentShader(first, initial);
    setenv("GXV_TEVWRAP_ELIDE", enabled ? "0" : "1", 1);
    vgx::GenerateFragmentShader(first, again);
    Require(initial.text == again.text, "elision setting is read once");
    for (unsigned src = 0; src < 4; ++src) for (unsigned dst = 0; dst < 4; ++dst) for (bool fromD : {false, true}) {
        const auto chain = Chain(src, dst, fromD);
        Expect(chain, true, true, "bounded copy suppresses only unnecessary final wrap");
        auto independent = chain; independent.stage[0].color &= ~(1u << 19);
        Expect(independent, false, true, "RGB and alpha bounds stay independent");
        independent = chain; independent.stage[0].alpha &= ~(1u << 19);
        Expect(independent, true, false, "alpha and RGB bounds stay independent");
        auto cross = chain; cross.stage[1].color = Copy(true, src * 2 + 1, dst, false, fromD);
        cross.stage[0].color &= ~(1u << 19);
        Expect(cross, true, true, "RGB replicated alpha uses alpha provenance");
        auto unknown = chain; unknown.numStages = 1; unknown.stage[0] = chain.stage[1];
        Expect(unknown, false, false, "unwritten uniforms are not assumed bounded");
        for (unsigned bit : {16u, 17u, 18u, 20u, 21u}) {
            auto altered = chain; altered.stage[1].color |= 1u << bit; altered.stage[1].alpha |= 1u << bit;
            Expect(altered, false, false, "bias operation and scale changes cannot inherit copy bounds");
        }
        auto compare = chain; compare.stage[1].color |= 3u << 16; compare.stage[1].alpha |= 3u << 16;
        Expect(compare, false, false, "compare mode is not an identity copy");
        auto overwrite = chain; overwrite.numStages = 3; overwrite.stage[2] = overwrite.stage[1];
        overwrite.stage[1].color = Copy(true, 2, src, false);
        overwrite.stage[1].alpha = Copy(false, 1, src, false);
        if (src != 1) Expect(overwrite, false, false, "unknown writes invalidate old clamp provenance");
    }
    // Both expressions read the pre-stage registers, even if RGB writes that register.
    auto simultaneous = Chain(0, 0, false);
    simultaneous.stage[1].color = Copy(true, 2, 0, false);
    Expect(simultaneous, false, true, "same-stage reads precede writes");
    simultaneous = Chain(0, 0, false);
    simultaneous.stage[1].color = Copy(true, 1, 0, false);
    simultaneous.stage[1].alpha = Copy(false, 1, 0, false);
    Expect(simultaneous, true, false, "RGB reads pre-stage alpha before alpha is overwritten");
    for (unsigned c : {12u, 13u, 15u}) {
        vgx::FsKey literal; literal.stage[0].color = Copy(true, c, 0, false);
        literal.stage[0].alpha = Copy(false, 7, 0, false);
        Expect(literal, true, true, "literal unit values need no byte wrapping");
    }
    for (unsigned c : {8u, 9u, 10u, 11u, 14u}) {
        auto unknown = Chain(0, 0, false); unknown.numStages = 1;
        unknown.stage[0].color = Copy(true, c, 0, false);
        unknown.stage[0].alpha = Copy(false, c == 14 ? 6 : c / 2, 0, false);
        Expect(unknown, false, false, "texture raster and uniform konst values are not assumed bounded");
    }
    if (!wrapping) Require(digest == 0xf212c4f4f5dbb80bull, "all generated sources remain byte-identical when wrapping is off");
    else if (!enabled) Require(digest == 0xcf8d2a776de174e2ull, "default off and invalid values retain every original source byte");
    std::printf("PASS: %zu source checks, digest=%016llx\n", checked, static_cast<unsigned long long>(digest));
    if (argc == 4) Corpus(argv[3]);
}
