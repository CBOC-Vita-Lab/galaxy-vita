#include "vgx_state.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}

namespace {
void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

void CheckBase(const vgx::TexUpload& upload, bool compressed, bool cmpr) {
    Require(upload.width == 8 && upload.height == 8, "base dimensions unchanged");
    if (compressed) {
        Require(upload.format == vgx::TF_DXT1 && upload.levels[0].size() == 32,
                "opaque CMPR keeps its lossless DXT1 route");
        const uint8_t block[] = {0x00, 0xF8, 0xE0, 0x07, 0, 0, 0, 0};
        for (unsigned i = 0; i < 32; ++i)
            Require(upload.levels[0][i] == block[i % 8], "base DXT1 blocks unchanged");
    } else {
        Require(upload.format == vgx::TF_RGBA8 && upload.levels[0].size() == 256,
                "base remains RGBA8");
        for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
            // Hand-checked I8 fixture: two 8x4 tiles, intensity also supplies alpha.
            const uint8_t value = static_cast<uint8_t>(y * 8 + x + 1);
            for (unsigned c = 0; c < 4; ++c)
                Require(upload.levels[0][(y * 8 + x) * 4 + c] == (cmpr ? (c == 0 || c == 3 ? 255 : 0) : value),
                        "all base pixels unchanged after mip fallback");
        }
    }
}
}

int main(int argc, char** argv) {
    Require(argc == 3, "usage: vgx_texcache_mips mode i8|cmpr");
    const std::string mode = argv[1];
    const bool cmpr = std::string(argv[2]) == "cmpr";
    const bool fallback = mode == "base-only" || mode == "partial-chain";
    const bool invalid = mode == "short-base";
    const bool nonmip = mode == "nonmip-filter";
    const bool baseLod = mode == "max-lod-zero";
    const unsigned baseBytes = cmpr ? 32 : 64;
    const unsigned fullBytes = cmpr ? 128 : 160;
    std::vector<uint8_t> data(fullBytes);
    if (cmpr) {
        for (unsigned i = 0; i < fullBytes; i += 8) {
            data[i] = 0xF8; data[i + 1] = 0; data[i + 2] = 0x07; data[i + 3] = 0xE0;
        }
    } else for (unsigned i = 0; i < fullBytes; ++i) data[i] = static_cast<uint8_t>(i + 1);

    vgx::ResetState();
    vgx::g.bpTexStale = 0;
    auto& slot = vgx::g.tex[0];
    slot.valid = true;
    slot.obj.data = data.data();
    slot.obj.width = slot.obj.height = 8;
    slot.obj.format = cmpr ? 0xE : 1;
    slot.obj.mipmap = 1;
    slot.obj.maxLod = baseLod ? 0 : 3;
    slot.obj.minFilter = nonmip ? vgx::FILT_LINEAR : vgx::FILT_LIN_MIP_LIN;
    slot.obj.magFilter = 1;
    slot.avail = invalid ? baseBytes - 1 : fallback ? baseBytes + (mode == "partial-chain" ? 31 : 0) : fullBytes;

    vgx::TexBind bind;
    uint16_t width = 0, height = 0;
    const bool resolved = vgx::ResolveTexture(0, bind, width, height);
    auto& pending = vgx::Pending();
    if (invalid) {
        Require(!resolved && pending.uploads.empty() && bind.texId == 0,
                "unreadable base rejected without an upload");
    } else {
        Require(resolved && pending.uploads.size() == 1 && pending.cmds.size() == 1,
                "exactly one initial texture upload");
        const auto& upload = pending.uploads.front();
        const auto& caps = vgx::backend::GetCaps();
        const bool mips = !fallback && !nonmip && !baseLod;
        const bool dxt = cmpr && caps.dxt1;
        CheckBase(upload, dxt, cmpr);
        Require(bind.minFilter == (mips ? vgx::FILT_LIN_MIP_LIN : vgx::FILT_LINEAR),
                "sampling falls back only when the readable mip range is incomplete");
        Require(upload.genMips == (mips && !dxt && caps.mipGen),
                "base-only sampling never requests unused GPU mip generation");
        const unsigned expectedLevels = !mips || upload.genMips ? 1 : dxt && caps.mipClamp ? 2 : 4;
        Require(upload.levels.size() == expectedLevels,
                "base-only sampling never constructs unused software mip levels");
        const unsigned expectedAuthored = !mips || upload.genMips ? 1 : dxt && caps.mipClamp ? 2 : 4;
        Require(upload.authored == expectedAuthored, "authored level count remains accurate");
        const uint32_t firstId = bind.texId;
        Require(vgx::ResolveTexture(0, bind, width, height) && bind.texId == firstId && pending.uploads.size() == 1,
                "unchanged fallback/full entry is reused without another upload");
        if (fallback) {
            vgx::g.tex[1]=slot;vgx::g.tex[1].obj.minFilter=vgx::FILT_LINEAR;
            Require(vgx::ResolveTexture(1,bind,width,height) && bind.texId==firstId && pending.uploads.size()==1,
                    "non-mip slot at the same address shares the safe base-only upload");
            slot.avail = fullBytes;
            Require(vgx::ResolveTexture(0, bind, width, height) && pending.uploads.size() == 2 && bind.texId != firstId,
                    "later readable authored mips are not hidden by the base-only cache entry");
            Require(bind.minFilter == vgx::FILT_LIN_MIP_LIN, "full mip sampling restored once readable");
            CheckBase(pending.uploads.back(), dxt, cmpr);
            Require(pending.uploads.back().genMips == (!dxt && caps.mipGen), "normal GPU mip policy preserved");
        }
    }
    std::printf("PASS: texture mip availability mode=%s format=%s (synthetic, not Vita FPS)\n", argv[1], argv[2]);
}
