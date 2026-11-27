// HD texture replacement through the real texture cache (tools/test_vgx_hdtex.sh).
//   vgx_hdtex <pack> replace|clamp|budget|dryrun|copy|changed|eyes|eyeson|trilinear|aged|agedcard|cap|capsmall|prefixcard|prefixram [player|notplayer]
// The optional last word says whether the pack's one entry (its name decides) must be known as one of Mario's or Luigi's own
// textures (vgx::IsPlayerTexture, GXV_CHARAA_AUTO); it is always a character texture (vgx::IsCharaTexture).
// The pack is tools/hdtex/make_test_pack.py's: one 8x8 CMPR original, a 16x16 DXT1 replacement
// with levels 16..1 whose bytes are 0x10 + level.
#include "vgx_hdtex.h"
#include "vgx_state.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    Require(argc == 3 || argc == 4, "usage: vgx_hdtex <pack> replace|clamp|budget|dryrun|copy|changed|eyes|eyeson|trilinear");
    const std::string mode = argv[2];
    setenv("GXV_HDTEX_PATH", argv[1], 1);
    setenv("GXV_MIPCLAMP", mode == "clamp" ? "1" : "0", 1);
    setenv("GXV_MIPGEN", "0", 1);
    if (mode == "budget") setenv("GXV_HDTEX_BUDGET", "0", 1);
    if (mode == "clamp") setenv("GXV_HDTEX_PRELOAD", "0", 1); // (the others read from the preloaded pack)
    if (mode == "dryrun") setenv("GXV_HDTEX", "2", 1);
    if (mode == "trilinear") setenv("GXV_HDTEX_TRILINEAR", "1", 1); // (replaced textures blend two mip levels; nothing else differs from "replace")
    if (mode == "eyeson") setenv("GXV_HDTEX_EYES", "1", 1); // (the pack here names its texture Test:SyntheticEye)
    // hdtex24: the texture cache's age rule and caps (GXV_TEX_SCENEDROP=3) against HD textures, and the preloaded prefix of a larger pack
    const bool ageMode = mode == "aged" || mode == "agedcard" || mode == "cap" || mode == "capsmall";
    if (ageMode) {
        setenv("GXV_TEX_SCENEDROP_MINFRAMES", "0", 1);
        setenv("GXV_TEX_SCENEDROP_AGE", mode == "cap" || mode == "capsmall" ? "1000" : "1", 1);
        setenv("GXV_TEX_SCENEDROP_MAXKB", mode == "cap" || mode == "capsmall" ? "1" : "0", 1); // (0: no cap)
        setenv("GXV_TEX_SCENEDROP_MAX", "0", 1);
        setenv("GXV_SPLIT", "0", 1);
        if (mode == "agedcard") setenv("GXV_HDTEX_PRELOAD", "0", 1);
    }
    if (mode == "prefixcard" || mode == "prefixram") setenv("GXV_HDTEX_PRELOAD_KB", "512", 1); // (the pack has a 1 MiB filler record before / after the test record)
    vgx::hdtex::Install(nullptr);
    vgx::hdtex::Counters c0;
    vgx::hdtex::GetCounters(c0);
    Require(c0.entries == ((mode == "prefixcard" || mode == "prefixram") ? 2u : 1u), "pack index loaded");
    if (mode == "prefixcard" || mode == "prefixram") {
        Require(c0.preloaded == 512u * 1024u && c0.fileSize > c0.preloaded, "only the first 512 KiB of the larger pack are in memory");
    } else if (mode != "clamp" && mode != "agedcard") {
        Require(c0.preloaded == c0.fileSize && c0.fileSize > 0, "a pack within GXV_HDTEX_PRELOAD is in memory whole");
    }

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
    if (argc > 3) {
        const bool wantPlayer = std::string(argv[3]) == "player";
        Require(vgx::IsCharaTexture(pending.uploads.front().texId), "the pack's texture is a character or key item texture");
        Require(vgx::IsPlayerTexture(pending.uploads.front().texId) == wantPlayer,
                wantPlayer ? "Mario's or Luigi's archive: the player's own texture" : "not Mario's or Luigi's archive: not the player's own texture");
        // mario24: the upload carries the flag the back end binds the player's own bias / anisotropy by, and the cache keeps the facts for "player-look:"
        Require(pending.uploads.front().player == wantPlayer, wantPlayer ? "the upload is flagged as the player's (TexUpload::player)" : "the upload is not flagged as the player's");
        vgx::PlayerTexFact pf[vgx::kPlayerTexFactsMax];
        const int pn = vgx::PlayerTexFacts(pf, vgx::kPlayerTexFactsMax);
        Require(pn == (wantPlayer ? 1 : 0), "PlayerTexFacts lists the player's textures only");
        if (wantPlayer && std::string(argv[2]) == "replace")
            Require(pf[0].hd && pf[0].w == 16 && pf[0].origW == 8 && pf[0].levels == 5 && pf[0].finalMin == vgx::FILT_LIN_MIP_NEAR && pf[0].origMin == vgx::FILT_LINEAR,
                    "the fact says: HD 16x16 over the original 8x8, 5 levels, mip-nearest bound, the game's own filter bilinear");
    }
    const bool mipVis = std::getenv("GXV_PLAYER_MIPVIS") != nullptr && std::atoi(std::getenv("GXV_PLAYER_MIPVIS")) != 0;
    const auto& up = pending.uploads.front();
    const bool replaced = mode == "replace" || mode == "clamp" || mode == "changed" || mode == "eyeson" || mode == "trilinear" || ageMode ||
                          mode == "prefixcard" || mode == "prefixram";
    const uint8_t replacedFilter = mode == "trilinear" ? vgx::FILT_LIN_MIP_LIN : vgx::FILT_LIN_MIP_NEAR;
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
    const uint32_t hdSide = (mode == "aged" || mode == "agedcard" || mode == "cap") ? 64u : 16u; // (tools/test_vgx_hdtex.sh: make_test_pack.py --hd)
    Require(up.width == hdSide && up.height == hdSide && up.format == vgx::TF_DXT1 && !up.genMips, "replacement uploaded as DXT1 at its own size");
    size_t want = 0; // mipClamp: down to the last level of at least 4x4; else down to 1x1
    for (uint32_t s2 = hdSide;; s2 >>= 1) {
        ++want;
        if (s2 == 1 || (mode == "clamp" && (s2 >> 1) < 4)) break;
    }
    Require(up.levels.size() == want && up.authored == want, "replacement mip chain");
    uint32_t bytes = 0;
    for (size_t l = 0; l < up.levels.size(); ++l) {
        const uint32_t side = hdSide >> l, blocks = ((side + 3) / 4) * ((side + 3) / 4);
        Require(up.levels[l].size() == blocks * 8u, "level sizes (whole 4x4 blocks)");
        if (mipVis && argc > 3 && std::string(argv[3]) == "player") {
            // GXV_PLAYER_MIPVIS=1: every block of level l is the flat colour of level l (both end points equal, every index 0)
            static const uint16_t kC[3] = {0xF800, 0x07E0, 0x001F}; // red, green, blue in RGB565
            for (size_t i = 0; i + 8 <= up.levels[l].size(); i += 8) {
                const uint8_t* p = up.levels[l].data() + i;
                const uint16_t c = static_cast<uint16_t>(kC[l < 3 ? l : 0]);
                Require(l >= 3 || (p[0] == (c & 0xFF) && p[1] == (c >> 8) && p[2] == p[0] && p[3] == p[1] && p[4] == 0 && p[5] == 0 && p[6] == 0 && p[7] == 0),
                        "GXV_PLAYER_MIPVIS: the player's levels are flat colours (red, green, blue, ...)");
            }
        } else {
            for (uint8_t b : up.levels[l]) Require(b == 0x10 + l, "level bytes passed through");
        }
        bytes += static_cast<uint32_t>(up.levels[l].size());
    }
    Require(bind.minFilter == replacedFilter, mode == "trilinear" ? "a replaced texture is sampled trilinearly (GXV_HDTEX_TRILINEAR=1)" : "a replaced texture is sampled with mips (nearest level)");
    Require(vgx::ReplacedTextureBytes() == bytes, "replacement bytes accounted");
    const uint32_t id = bind.texId;
    Require(vgx::ResolveTexture(0, bind, width, height) && pending.uploads.size() == 1 && bind.texId == id, "no lookup or upload on a later bind");
    Require(bind.minFilter == replacedFilter && width == 8, "later binds keep the mip filter and the original size");
    if (mode == "prefixcard" || mode == "prefixram") {
        vgx::hdtex::Counters cc;
        vgx::hdtex::GetCounters(cc);
        Require(cc.cardReads == (mode == "prefixcard" ? 1u : 0u),
                mode == "prefixcard" ? "a record past the preloaded prefix is read from the card, once" : "a record inside the preloaded prefix is not read from the card");
    }
    if (ageMode) {
        // The texture cache's age rule and caps against the replacement: it is counted at its HD size, an idle one is freed (by age, or for the cap) and uploaded
        // again from the pack (from memory: no card read; with GXV_HDTEX_PRELOAD=0 from the card again), a bound one is never freed.
        vgx::HdTexStats hs;
        vgx::HdTextureStats(hs);
        Require(hs.uploads == 1 && hs.bytes == bytes && hs.alive == 1 && hs.aliveBytes == bytes && hs.freed == 0, "the cache counts the replacement at its own size");
        char line[640];
        Require(vgx::hdtex::SceneLine(line, sizeof line) && std::strstr(line, "hdtex: scene 1: 1 HD textures substituted") != nullptr, "the scene line says what was substituted");
        Require(!vgx::hdtex::SceneLine(line, sizeof line), "and says nothing when nothing happened");
        const void* lo[1] = {nullptr};
        const void* hi[1] = {nullptr};
        vgx::FrontPresent(false, nullptr);
        const vgx::TexSceneDrop t1 = vgx::TextureCacheSceneGone(lo, hi, 0, 3);
        Require(t1.freed == 0 && t1.freedCap == 0, "a texture the scene bound is never freed, not by age, not for the caps");
        vgx::FrontPresent(false, nullptr);
        const vgx::TexSceneDrop t2 = vgx::TextureCacheSceneGone(lo, hi, 0, 3);
        if (mode == "capsmall") {
            Require(t2.freed == 0 && t2.freedCap == 0 && vgx::ReplacedTextureBytes() == bytes, "an idle replacement under the cap stays");
        } else {
            Require(t2.freed == 1 && t2.freedBytes == bytes, "the idle replacement is freed, with its HD bytes");
            if (mode == "cap") Require(t2.freedCap == 1 && t2.aged == 0 && t2.liveBytes == 0, "freed for the cap: 2.7 KiB of replacement against a 1 KiB cap, its original is 32 bytes");
            else Require(t2.aged == 1 && t2.freedCap == 0, "freed by age");
            vgx::HdTextureStats(hs);
            Require(vgx::ReplacedTextureBytes() == 0 && hs.freed == 1 && hs.freedBytes == bytes && hs.alive == 0, "the replacement bytes are released");
            Require(vgx::hdtex::SceneLine(line, sizeof line) && std::strstr(line, "1 freed (") != nullptr && std::strstr(line, "0 alive") != nullptr, "the scene line says it was freed");
            vgx::hdtex::Counters before;
            vgx::hdtex::GetCounters(before);
            Require(vgx::ResolveTexture(0, bind, width, height), "bound again");
            vgx::hdtex::Counters after;
            vgx::hdtex::GetCounters(after);
            vgx::HdTextureStats(hs);
            Require(after.hits == before.hits + 1 && hs.uploads == 2 && hs.alive == 1 && vgx::ReplacedTextureBytes() == bytes, "uploaded again from the pack");
            Require(after.cardReads == (mode == "agedcard" ? 2u : 0u), mode == "agedcard" ? "without the preload the second upload reads the card again" : "the second upload reads no card: the pack is in memory");
        }
    }
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
