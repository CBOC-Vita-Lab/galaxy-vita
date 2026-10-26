// HD texture replacement through the real texture cache (tools/test_vgx_hdtex.sh).
//   vgx_hdtex <pack> replace|clamp|budget|dryrun|copy|changed|eyes|eyeson
// The pack is tools/hdtex/make_test_pack.py's: one 8x8 CMPR original, a 16x16 DXT1 replacement
// with levels 16..1 whose bytes are 0x10 + level.
#include "vgx_hdtex.h"
#include "vgx_state.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// Standalone host hooks: Mach-O does not resolve the split frontend's optional
// weak declarations without definitions. These do not enter the game build.
extern "C" int64_t port_host_time_ns(void) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" int64_t port_thread_cpu_us(void) { return 0; } // CPU timing unavailable in this texture-only fixture.
extern "C" void port_log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}

namespace {
void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
}

int main(int argc, char** argv) {
    Require(argc == 3, "usage: vgx_hdtex <pack> replace|clamp|budget|dryrun|copy|changed|eyes|eyeson");
    const std::string mode = argv[2];
    setenv("GXV_HDTEX_PATH", argv[1], 1);
    setenv("GXV_MIPCLAMP", mode == "clamp" ? "1" : "0", 1);
    setenv("GXV_MIPGEN", "0", 1);
    if (mode == "budget") setenv("GXV_HDTEX_BUDGET", "0", 1);
    if (mode == "clamp") setenv("GXV_HDTEX_PRELOAD", "0", 1); // (the others read from the preloaded pack)
    if (mode == "dryrun") setenv("GXV_HDTEX", "2", 1);
    if (mode == "eyeson") setenv("GXV_HDTEX_EYES", "1", 1); // (the pack here names its texture Test:SyntheticEye)
    vgx::hdtex::Install(nullptr);
    vgx::hdtex::Counters c0;
    vgx::hdtex::GetCounters(c0);
    Require(c0.entries == 1, "pack index loaded");

    std::vector<uint8_t> data(32);
    for (unsigned i = 0; i < 32; ++i) data[i] = static_cast<uint8_t>((i * 7 + 3) & 255);
    vgx::ResetState();
    vgx::g.bpTexStale = 0;
    auto& slot = vgx::g.tex[0];
    slot.valid = true;
    slot.obj.data = data.data();
    slot.obj.width = slot.obj.height = 8;
    slot.obj.format = 0xE;
    slot.obj.mipmap = 0;
    slot.obj.maxLod = 0;
    slot.obj.minFilter = vgx::FILT_LINEAR;
    slot.obj.magFilter = 1;
    slot.avail = 32;
    if (mode == "copy") vgx::AcquireCopyTexture(data.data(), 8, 8, 0x6);

    vgx::TexBind bind;
    uint16_t width = 0, height = 0;
    auto& pending = vgx::Pending();
    Require(vgx::ResolveTexture(0, bind, width, height), "texture resolves");
    Require(width == 8 && height == 8, "the guest-visible size stays the original's (TEV texel sizes)");
    if (mode == "copy") {
        Require(pending.uploads.empty(), "an EFB copy's address is never replaced (nor uploaded)");
        std::printf("PASS: hdtex mode=%s\n", argv[2]);
        return 0;
    }
    Require(pending.uploads.size() == 1, "one upload");
    const auto& up = pending.uploads.front();
    const bool replaced = mode == "replace" || mode == "clamp" || mode == "changed" || mode == "eyeson";
    vgx::hdtex::Counters c;
    vgx::hdtex::GetCounters(c);
    Require(c.hashed == 1, "the texture's shape is in the pack: hashed once");
    if (!replaced) {
        // (as DXT1 or, the synthetic bytes using CMPR's transparent mode, decoded to RGBA8)
        Require(up.width == 8 && up.height == 8 && up.format != vgx::TF_DXT5, "original uploaded (budget / dry run)");
        Require(bind.minFilter == vgx::FILT_LINEAR, "original keeps its filter");
        Require(vgx::ReplacedTextureBytes() == 0, "no replacement bytes alive");
        Require(mode == "budget" ? (c.refused == 1 && c.hits == 0) : mode == "eyes" ? (c.refused == 0 && c.hits == 0) : (c.hits == 1 && c.refused == 0), "counted as refused / dry-run hit / eye texture kept");
        std::printf("PASS: hdtex mode=%s\n", argv[2]);
        return 0;
    }
    Require(up.width == 16 && up.height == 16 && up.format == vgx::TF_DXT1 && !up.genMips, "replacement uploaded as 16x16 DXT1");
    const size_t want = mode == "clamp" ? 3 : 5; // mipClamp: down to the last level of at least 4x4
    Require(up.levels.size() == want && up.authored == want, "replacement mip chain");
    uint32_t bytes = 0;
    for (size_t l = 0; l < up.levels.size(); ++l) {
        Require(up.levels[l].size() == (l == 0 ? 128u : l == 1 ? 32u : 8u), "level sizes (16x16, 8x8, then one block)");
        for (uint8_t b : up.levels[l]) Require(b == 0x10 + l, "level bytes passed through");
        bytes += static_cast<uint32_t>(up.levels[l].size());
    }
    Require(bind.minFilter == vgx::FILT_LIN_MIP_NEAR, "a replaced texture is sampled with mips");
    Require(vgx::ReplacedTextureBytes() == bytes, "replacement bytes accounted");
    const uint32_t id = bind.texId;
    Require(vgx::ResolveTexture(0, bind, width, height) && pending.uploads.size() == 1 && bind.texId == id, "no lookup or upload on a later bind");
    Require(bind.minFilter == vgx::FILT_LIN_MIP_NEAR && width == 8, "later binds keep the mip filter and the original size");
    if (mode == "changed") {
        data[0] ^= 0xFF; // the game rewrites the texture: no longer the pack's original
        Require(vgx::ResolveTexture(0, bind, width, height) && pending.uploads.size() == 2, "rewritten texture uploaded again");
        const auto& up2 = pending.uploads.back();
        Require(up2.width == 8 && up2.height == 8, "rewritten texture is not replaced");
        Require(bind.minFilter == vgx::FILT_LINEAR, "and keeps its own filter");
        Require(vgx::ReplacedTextureBytes() == 0, "replacement bytes released");
    }
    std::printf("PASS: hdtex mode=%s\n", argv[2]);
}
