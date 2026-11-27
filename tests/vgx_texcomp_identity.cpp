// GXV_TEX_COMPRESS=0 is the old front end, byte for byte (tools/test_vgx_texcomp.sh builds this twice: against the base tree's sources and
// against this tree's, and compares the digests). Only the front end's public texture API is used, so the same file builds against both.
// A fixed sequence of textures of every GX format the cache decodes (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, C8 with a palette, CMPR),
// sizes 4..256, square and not, powers of two and not, with and without levels; a texture changed and resolved again; the ends of frames.
// Printed: one digest of every upload (id, size, format, flags, levels' bytes) and command the front end queued.
#include "vgx_state.h"
#include <cstdio>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}

static uint32_t g_rng = 99;
static uint8_t Rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return static_cast<uint8_t>(g_rng >> 11); }
static uint64_t g_h = 0;
static unsigned g_up = 0;
static void Take() {
    auto& p = vgx::Pending();
    for (const auto& u : p.uploads) {
        const uint32_t head[6] = {u.texId, u.width, u.height, u.format, static_cast<uint32_t>(u.authored | (u.genMips ? 256 : 0) | (u.preserveI4Mips ? 512 : 0)),
                                  static_cast<uint32_t>(u.levels.size())};
        g_h = XXH3_64bits_withSeed(head, sizeof head, g_h);
        for (const auto& lv : u.levels) g_h = XXH3_64bits_withSeed(lv.data(), lv.size(), g_h);
        ++g_up;
    }
    for (const auto& c : p.cmds) {
        const uint32_t w[2] = {static_cast<uint32_t>(c.type), c.arg};
        g_h = XXH3_64bits_withSeed(w, sizeof w, g_h);
    }
    p.uploads.clear();
    p.cmds.clear();
}
static std::vector<std::vector<uint8_t>> g_keep;
static void Resolve(std::vector<uint8_t>& d, uint32_t fmt, uint32_t w, uint32_t h, bool mips, int filter) {
    vgx::ResetState();
    vgx::g.bpTexStale = 0;
    auto& slot = vgx::g.tex[0];
    slot.valid = true;
    slot.obj.data = d.data();
    slot.obj.width = static_cast<uint16_t>(w);
    slot.obj.height = static_cast<uint16_t>(h);
    slot.obj.format = fmt;
    slot.obj.mipmap = mips ? 1 : 0;
    slot.obj.maxLod = mips ? 10 : 0;
    slot.obj.minFilter = static_cast<uint8_t>(filter);
    slot.obj.magFilter = 1;
    slot.obj.tlut = 0;
    slot.avail = static_cast<uint32_t>(d.size());
    static std::vector<uint8_t> tlut(512);
    for (uint8_t& b : tlut) b = Rnd();
    vgx::g.tlut[0].valid = true;
    vgx::g.tlut[0].obj.data = tlut.data();
    vgx::g.tlut[0].obj.format = 2; // RGB5A3
    vgx::g.tlut[0].obj.entries = 256;
    vgx::TexBind b;
    uint16_t ww = 0, hh = 0;
    const bool ok = vgx::ResolveTexture(0, b, ww, hh);
    const uint32_t r[4] = {b.texId, static_cast<uint32_t>(ok), static_cast<uint32_t>(b.minFilter | (b.magFilter << 8) | (b.wrapS << 16) | (b.wrapT << 24)), ww | (hh << 16u)};
    g_h = XXH3_64bits_withSeed(r, sizeof r, g_h);
    Take();
}

int main() {
    const uint32_t fmts[] = {0, 1, 2, 3, 4, 5, 6, 9, 14};
    const uint32_t sizes[][2] = {{4, 4}, {8, 8}, {16, 32}, {64, 64}, {128, 64}, {96, 64}, {256, 256}, {128, 128}};
    for (uint32_t fmt : fmts)
        for (const auto& s : sizes)
            for (int mips = 0; mips < 2; ++mips) {
                g_keep.emplace_back(s[0] * s[1] * 4 * 2 + 64);
                for (uint8_t& b : g_keep.back()) b = Rnd();
                if (fmt == 6 || fmt == 5) for (size_t i = 0; i < g_keep.back().size(); i += 2) if (Rnd() & 1) g_keep.back()[i] |= 0x80; // (some opaque texels)
                Resolve(g_keep.back(), fmt, s[0], s[1], mips != 0, mips ? 5 : 1);
            }
    // a texture changed and resolved again, twice
    g_keep.emplace_back(128 * 128 * 2 + 64);
    for (uint8_t& b : g_keep.back()) b = Rnd();
    for (int k = 0; k < 3; ++k) {
        g_keep.back()[k * 7] ^= 0x5A;
        vgx::InvalidateAllTextures();
        Resolve(g_keep.back(), 4, 128, 128, false, 1);
    }
    for (int f = 0; f < 8; ++f) { vgx::TexCacheEndFrame(); Take(); }
    std::printf("identity digest %016llx over %u uploads\n", static_cast<unsigned long long>(g_h), g_up);
    return 0;
}
