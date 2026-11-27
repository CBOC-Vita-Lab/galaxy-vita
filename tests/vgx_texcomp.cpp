// GXV_TEX_COMPRESS host proof (tools/test_vgx_texcomp.sh; texcomp23).
//   A. The encoder (vgx_texcomp.cpp): colour blocks always in four-colour mode (the same colours as DXT1 and as DXT5's colour half on any
//      decoder); binary alpha and constant / two-value alpha exact; solid colours within 3 steps; the published channel range holds every
//      decoded texel; level chains down to 4x4; a smooth gradient above the floor, white noise below it.
//   B. The front end with the null back end and the console's caps (GXV_VITA=1): switch off, nothing is offered and the end of a frame adds
//      nothing; on, the RGBA8 upload is unchanged, and the DXT1 / DXT5 replacement under the same id arrives only after a 3D use; 2D and
//      indirect uses, a changed texture, small / I/IA / non-power-of-two ones are left as they were; mipmapped ones get levels to 4x4.
//   C. (second process, same card cache) every conversion is read back from the card, byte for byte what the first process made.
#include "vgx_state.h"
#include "vgx_texcomp.h"
#include "vgx_texdecode.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}
namespace tc = vgx::texcomp;
static std::vector<std::string> g_lines; // (the basic log)
extern "C" void port_log_summary(const char* fmt, ...) {
    char text[1200];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    g_lines.emplace_back(text);
}

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_fail; }
}
static uint32_t g_rng = 2323;
static uint32_t Rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

// ---------------------------------------------------------------- A. the encoder
static void PartA() {
    unsigned blocks = 0;
    // colour mode and solid colours
    for (int n = 0; n < 4000; ++n) {
        uint8_t px[64];
        const int kind = n % 4;
        const uint8_t base[3] = {static_cast<uint8_t>(Rnd()), static_cast<uint8_t>(Rnd()), static_cast<uint8_t>(Rnd())};
        for (int i = 0; i < 16; ++i)
            for (int k = 0; k < 4; ++k)
                px[i * 4 + k] = k == 3 ? 255 : kind == 0 ? base[k] : kind == 1 ? static_cast<uint8_t>(Rnd()) : static_cast<uint8_t>(std::min(255, base[k] / 2 + i * 7 + k));
        uint8_t out[8], dec[64];
        tc::EncodeColorBlock(px, out);
        const uint32_t c0 = out[0] | (out[1] << 8), c1 = out[2] | (out[3] << 8);
        Check(c0 > c1 || (c0 == c1 && !out[4] && !out[5] && !out[6] && !out[7]), "four-colour mode (or one colour, index 0)");
        tc::DecodeDxt(out, 4, 4, false, dec);
        if (kind == 0) {
            int worst = 0;
            for (int i = 0; i < 16; ++i) for (int k = 0; k < 3; ++k) worst = std::max(worst, std::abs(dec[i * 4 + k] - px[i * 4 + k]));
            Check(worst <= 3, "solid colour within 3 steps");
        }
        for (int i = 0; i < 16; ++i) Check(dec[i * 4 + 3] == 255, "opaque stays opaque");
        ++blocks;
    }
    // alpha: binary, constant, two-value exact
    for (int n = 0; n < 3000; ++n) {
        uint8_t px[64];
        const int kind = n % 3;
        const uint8_t v0 = static_cast<uint8_t>(Rnd()), v1 = static_cast<uint8_t>(Rnd());
        for (int i = 0; i < 16; ++i) {
            px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 128;
            px[i * 4 + 3] = kind == 0 ? ((Rnd() & 1) ? 255 : 0) : kind == 1 ? v0 : ((Rnd() & 1) ? v0 : v1);
        }
        uint8_t blk[16], dec[64];
        tc::EncodeAlphaBlock(px, blk);
        tc::EncodeColorBlock(px, blk + 8);
        tc::DecodeDxt(blk, 4, 4, true, dec);
        bool exact = true;
        for (int i = 0; i < 16; ++i) exact = exact && dec[i * 4 + 3] == px[i * 4 + 3];
        Check(exact, kind == 0 ? "binary alpha exact" : kind == 1 ? "constant alpha exact" : "two-value alpha exact");
        ++blocks;
    }
    // textures: ranges, chains, quality floor
    struct Case { uint32_t w, h; bool mips, alpha, noise; };
    const Case cases[] = {{64, 64, true, false, false}, {256, 64, true, true, false}, {128, 128, false, true, false}, {64, 64, false, false, true}, {8, 8, true, false, false}};
    for (const Case& c : cases) {
        std::vector<uint8_t> rgba(c.w * c.h * 4);
        for (uint32_t y = 0; y < c.h; ++y)
            for (uint32_t x = 0; x < c.w; ++x) {
                uint8_t* p = &rgba[(y * c.w + x) * 4];
                if (c.noise) { for (int k = 0; k < 3; ++k) p[k] = static_cast<uint8_t>(Rnd()); p[3] = 255; continue; }
                p[0] = static_cast<uint8_t>(x * 255 / c.w);
                p[1] = static_cast<uint8_t>(y * 255 / c.h);
                p[2] = static_cast<uint8_t>(128 + 60 * std::sin(x * 0.2) * std::cos(y * 0.15));
                p[3] = c.alpha ? static_cast<uint8_t>(((x / 8 + y / 8) & 1) ? 255 : (x * 4) & 255) : 255;
            }
        tc::Encoded e;
        Check(tc::EncodeTexture(rgba.data(), c.w, c.h, c.mips, e), "EncodeTexture takes power-of-two sizes");
        Check(e.format == (c.alpha ? tc::kFmtDxt5 : tc::kFmtDxt1), "DXT1 for opaque, DXT5 for alpha");
        uint32_t want = 1, lw = c.w, lh = c.h;
        if (c.mips) while (lw / 2 >= 4 && lh / 2 >= 4) { lw /= 2; lh /= 2; ++want; }
        Check(e.levels.size() == want, "level chain down to 4x4");
        // the range holds every decoded texel of every level
        lw = c.w; lh = c.h;
        for (const auto& lv : e.levels) {
            Check(lv.size() == ((lw + 3) / 4) * ((lh + 3) / 4) * (c.alpha ? 16u : 8u), "level size");
            std::vector<uint8_t> dec(lw * lh * 4);
            tc::DecodeDxt(lv.data(), lw, lh, c.alpha, dec.data());
            for (size_t i = 0; i < dec.size(); ++i) Check(dec[i] >= e.lo[i & 3] && dec[i] <= e.hi[i & 3], "published range holds the decoded texels");
            lw = std::max(1u, lw / 2); lh = std::max(1u, lh / 2);
        }
        if (c.noise) Check(e.q.psnrRgb < 32.0, "white noise falls below the 32 dB floor");
        else if (c.w >= 64) Check(e.q.psnrRgb > 36.0, "smooth texture above the floor"); // (the 8x8 case ramps 32 steps a texel: a size check only)
        std::printf("A: %ux%u%s%s%s: %s, %zu levels, PSNR %.2f dB (alpha %.2f)\n", c.w, c.h, c.mips ? " mips" : "", c.alpha ? " alpha" : "", c.noise ? " noise" : "",
                    e.format == tc::kFmtDxt1 ? "DXT1" : "DXT5", e.levels.size(), e.q.psnrRgb, e.q.psnrA);
    }
    std::printf("A: %u blocks checked%s\n", blocks, g_fail ? " (FAILURES)" : "");
}

// ---------------------------------------------------------------- B. the front end
// GX tiled encoders (4x4 tiles, big-endian words)
static std::vector<uint8_t> GxRgb565(uint32_t w, uint32_t h, uint32_t seed) {
    std::vector<uint8_t> d(w * h * 2);
    size_t o = 0;
    for (uint32_t ty = 0; ty < h; ty += 4)
        for (uint32_t tx = 0; tx < w; tx += 4)
            for (uint32_t y = ty; y < ty + 4; ++y)
                for (uint32_t x = tx; x < tx + 4; ++x) {
                    const uint32_t r = (x * 31 / w + seed) & 31, g = (y * 63 / h) & 63, b = (16 + static_cast<int>(8 * std::sin((x + seed) * 0.3))) & 31;
                    const uint32_t v = (r << 11) | (g << 5) | b;
                    d[o++] = static_cast<uint8_t>(v >> 8);
                    d[o++] = static_cast<uint8_t>(v);
                }
    return d;
}
static std::vector<uint8_t> GxRgba8(uint32_t w, uint32_t h, uint32_t seed) { // (alpha: a checkerboard of 0 / 255 and a ramp)
    std::vector<uint8_t> d(w * h * 4);
    size_t o = 0;
    for (uint32_t ty = 0; ty < h; ty += 4)
        for (uint32_t tx = 0; tx < w; tx += 4) {
            for (int pass = 0; pass < 2; ++pass)
                for (uint32_t y = ty; y < ty + 4; ++y)
                    for (uint32_t x = tx; x < tx + 4; ++x) {
                        const uint8_t r = static_cast<uint8_t>(x * 255 / w), g = static_cast<uint8_t>(y * 255 / h), b = static_cast<uint8_t>(seed * 37);
                        const uint8_t a = ((x / 8 + y / 8) & 1) ? 255 : 0;
                        if (pass == 0) { d[o++] = a; d[o++] = r; } else { d[o++] = g; d[o++] = b; }
                    }
        }
    return d;
}
static std::vector<std::vector<uint8_t>> g_keep;
static vgx::TexUpload g_lastUp;
static size_t g_uploads = 0;
// Binds a texture on texmap 0 and resolves it; returns the texture id. data: the guest bytes (kept alive).
static uint32_t Resolve(std::vector<uint8_t>& data, uint32_t fmt, uint32_t w, uint32_t h, bool mipmapped) {
    vgx::ResetState();
    vgx::g.bpTexStale = 0;
    auto& slot = vgx::g.tex[0];
    slot.valid = true;
    slot.obj.data = data.data();
    slot.obj.width = static_cast<uint16_t>(w);
    slot.obj.height = static_cast<uint16_t>(h);
    slot.obj.format = fmt;
    slot.obj.mipmap = mipmapped ? 1 : 0;
    slot.obj.maxLod = mipmapped ? 10 : 0;
    slot.obj.minFilter = mipmapped ? vgx::FILT_LIN_MIP_LIN : vgx::FILT_LINEAR;
    slot.obj.magFilter = 1;
    slot.avail = static_cast<uint32_t>(data.size());
    vgx::TexBind bind;
    uint16_t ww = 0, hh = 0;
    auto& pending = vgx::Pending();
    pending.uploads.clear();
    pending.cmds.clear();
    Check(vgx::ResolveTexture(0, bind, ww, hh), "resolves");
    g_uploads = pending.uploads.size();
    if (!pending.uploads.empty()) g_lastUp = pending.uploads.back();
    return bind.texId;
}
// One front-end frame end; returns the uploads it added.
static std::vector<vgx::TexUpload> EndFrame() {
    auto& pending = vgx::Pending();
    pending.uploads.clear();
    pending.cmds.clear();
    vgx::TexCacheEndFrame();
    return pending.uploads;
}
static uint64_t Digest(const vgx::TexUpload& u) {
    uint64_t h = XXH3_64bits_withSeed(&u.format, 1, u.width | (static_cast<uint64_t>(u.height) << 16));
    for (const auto& lv : u.levels) h = XXH3_64bits_withSeed(lv.data(), lv.size(), h);
    return h;
}

static void PartB(int mode, FILE* digests, FILE* expect) {
    const char* modeName = mode ? "on" : "off";
    // 1. an opaque 128x128 RGB565 texture, no levels
    g_keep.push_back(GxRgb565(128, 128, 1));
    const uint32_t id1 = Resolve(g_keep.back(), 4, 128, 128, false);
    Check(g_uploads == 1 && g_lastUp.format == vgx::TF_RGBA8 && g_lastUp.levels.size() == 1, "the first upload is the old RGBA8 one");
    tc::TestDrain();
    Check(EndFrame().empty(), "nothing replaces a texture before its first 3D draw");
    // 2. an alpha 64x64 RGBA8 texture with levels
    g_keep.push_back(GxRgba8(64, 64, 2));
    g_keep.back().resize(g_keep.back().size() * 2, 0); // (the GX levels after level 0: the cache asks for the whole chain's bytes)
    const uint32_t id2 = Resolve(g_keep.back(), 6, 64, 64, true);
    Check(g_uploads == 1 && g_lastUp.format == vgx::TF_RGBA8 && g_lastUp.genMips, "mipmapped: RGBA8 + generated levels as before");
    // 3. drawn in 2D / as an indirect map / changed before the replacement
    g_keep.push_back(GxRgb565(64, 64, 3));
    const uint32_t id3 = Resolve(g_keep.back(), 4, 64, 64, false);
    g_keep.push_back(GxRgb565(64, 64, 4));
    const uint32_t id4 = Resolve(g_keep.back(), 4, 64, 64, false);
    g_keep.push_back(GxRgb565(64, 64, 5));
    std::vector<uint8_t>& changing = g_keep.back();
    const uint32_t id5 = Resolve(changing, 4, 64, 64, false);
    // 4. not offered: small, I8, non-power-of-two
    g_keep.push_back(GxRgb565(32, 32, 6));
    Resolve(g_keep.back(), 4, 32, 32, false);
    g_keep.push_back(std::vector<uint8_t>(128 * 128));
    for (size_t i = 0; i < g_keep.back().size(); ++i) g_keep.back()[i] = static_cast<uint8_t>(64 + (i % 128) / 2); // (I8 tiles: a smooth ramp)
    const uint32_t id6 = Resolve(g_keep.back(), 1, 128, 128, false);
    Check(g_uploads == 1 && g_lastUp.format == vgx::TF_LA88, "I8 without levels: LA88 as before");
    g_keep.push_back(GxRgb565(96, 64, 7));
    Resolve(g_keep.back(), 4, 96, 64, false);
    tc::TestDrain();
    // the back end's marks
    tc::NoteUse(id1, tc::kUse3D);
    tc::NoteUse(id2, tc::kUse3D);
    tc::NoteUse(id3, tc::kUse3D | tc::kUse2D);
    tc::NoteUse(id4, tc::kUse3D | tc::kUseInd);
    tc::NoteUse(id6, tc::kUse3D);
    // id5 changes in guest memory and is resolved again (a new upload of the same id) before its result is applied
    changing[100] ^= 0xFF;
    Resolve(changing, 4, 64, 64, false);
    Check(g_uploads == 1 && g_lastUp.texId == id5 && g_lastUp.format == vgx::TF_RGBA8, "a changed texture is uploaded again as before");
    tc::NoteUse(id5, tc::kUse3D);
    tc::TestDrain();
    std::vector<vgx::TexUpload> got;
    for (int f = 0; f < 4; ++f) for (auto& u : EndFrame()) got.push_back(u);
    const tc::Counters c = tc::GetCounters();
    if (!mode) {
        Check(got.empty(), "switch off: the end of a frame adds nothing");
        Check(c.offered == 0 && c.queued == 0, "switch off: nothing offered");
        std::printf("B: switch %s: no replacement, nothing offered\n", modeName);
        return;
    }
    const bool ia = std::getenv("GXV_TEX_COMPRESS_IA") && std::atoi(std::getenv("GXV_TEX_COMPRESS_IA")) == 1;
    const vgx::TexUpload* u1 = nullptr;
    const vgx::TexUpload* u2 = nullptr;
    const vgx::TexUpload* u6 = nullptr;
    for (const auto& u : got) {
        if (u.texId == id1) u1 = &u;
        if (u.texId == id2) u2 = &u;
        if (u.texId == id6) u6 = &u;
        Check(u.texId == id1 || u.texId == id2 || (ia && u.texId == id6), "only the 3D-only, unchanged textures are replaced");
    }
    if (ia) {
        Check(u6 && u6->format == vgx::TF_DXT5 && u6->levels.size() == 1, "GXV_TEX_COMPRESS_IA=1: I8 (L, L, L, L) -> DXT5");
        if (u6) {
            std::vector<uint8_t> la(128 * 128 * 2), ref(128 * 128 * 4), dec(128 * 128 * 4);
            vgx::DecodeTextureLA88(g_keep[g_keep.size() - 2].data(), 128, 128, 1, la.data());
            vgx::ExpandLA88ToRGBA8(la.data(), 128 * 128, ref.data());
            tc::DecodeDxt(u6->levels[0].data(), 128, 128, true, dec.data());
            const tc::Quality q = tc::Measure(ref.data(), dec.data(), 128 * 128);
            Check(q.psnrRgb >= 32.0 && q.psnrA >= 36.0, "I8 as DXT5 within the floor");
            std::printf("B: GXV_TEX_COMPRESS_IA=1: id %u I8 128x128 -> DXT5 %.2f dB (alpha %.2f dB)\n", id6, q.psnrRgb, q.psnrA);
        }
    } else {
        Check(!u6, "I8 left as it was without GXV_TEX_COMPRESS_IA");
    }
    Check(u1 && u1->format == vgx::TF_DXT1 && u1->levels.size() == 1 && !u1->genMips && u1->width == 128, "opaque 128x128 -> DXT1, one level");
    Check(u2 && u2->format == vgx::TF_DXT5 && u2->levels.size() == 5 && !u2->genMips, "alpha 64x64 with levels -> DXT5, levels 64..4");
    Check(c.drop2D == 1 && c.dropInd == 1 && c.stale == 1, "2D, indirect and changed textures left as they were");
    Check(c.cls[tc::kClsSmall] == 1 && c.cls[tc::kClsIA] == (ia ? 0u : 1u) && c.cls[tc::kClsNpot] == 1, "small, I/IA and non-power-of-two not offered");
    Check(c.cls[tc::kClsDynamic] == 1, "the changed texture's second upload is not offered");
    Check(c.applied == (ia ? 3u : 2u), "applied: the 3D-only unchanged ones");
    // the replacement's quality against the RGBA8 the old path uploaded
    if (u1) {
        g_keep.push_back(GxRgb565(128, 128, 1));
        std::vector<uint8_t> ref(128 * 128 * 4), dec(128 * 128 * 4);
        vgx::DecodeTexture(g_keep.back().data(), 128, 128, 4, nullptr, 0, 0, ref.data());
        tc::DecodeDxt(u1->levels[0].data(), 128, 128, false, dec.data());
        const tc::Quality q = tc::Measure(ref.data(), dec.data(), 128 * 128);
        Check(q.psnrRgb >= 32.0, "replacement within the floor");
        std::printf("B: switch on: id %u RGB565 128x128 -> DXT1 %.2f dB; id %u RGBA8 64x64 mips -> DXT5 %zu levels; 2D %llu, indirect %llu, stale %llu; "
                    "card cache hits %llu, encoded %llu\n", id1, q.psnrRgb, id2, u2 ? u2->levels.size() : 0, static_cast<unsigned long long>(c.drop2D),
                    static_cast<unsigned long long>(c.dropInd), static_cast<unsigned long long>(c.stale), static_cast<unsigned long long>(c.hits),
                    static_cast<unsigned long long>(c.encoded));
    }
    // C. the card cache: the first process writes the digests, the second must read every replacement from the card, identical
    if (digests) {
        for (const auto* u : {u1, u2}) if (u) std::fprintf(digests, "%016llx\n", static_cast<unsigned long long>(Digest(*u)));
    }
    if (expect) {
        char line[64];
        for (const auto* u : {u1, u2}) {
            if (!u || !std::fgets(line, sizeof line, expect)) { Check(false, "card cache digest present"); continue; }
            Check(std::strtoull(line, nullptr, 16) == Digest(*u), "the card cache gives back the same bytes");
        }
        Check(c.hits >= 2 && c.encoded == 0, "second process: read from the card, nothing encoded");
        std::printf("C: second process: %llu card-cache hits, %llu encoded, same bytes\n", static_cast<unsigned long long>(c.hits),
                    static_cast<unsigned long long>(c.encoded));
    }
}

// D. The census (GXV_TEXCOMP_CENSUS=1 GXV_TEXCOMP_CENSUS_S=0): bytes read a picture by class, the saving, the top list.
static void PartD() {
    if (!tc::CensusOn()) return;
    g_lines.clear();
    tc::CensusInfo(9001, 6, 128, 128, vgx::TF_RGBA8, 128 * 128 * 4, false, tc::kClsQueued);  // RGBA8, convertible
    tc::CensusInfo(9002, 14, 256, 256, vgx::TF_DXT1, 256 * 256 / 2, true, tc::kClsDxt);     // CMPR as DXT1
    tc::CensusInfo(9003, 1, 64, 64, vgx::TF_LA88, 64 * 64 * 2, false, tc::kClsLa88);        // I8 as LA88
    tc::CensusDraw(9001, 10000.0f, false, tc::kUse3D, false, 0, 0);    // 10000 texels of 16384 read: 40000 B
    tc::CensusDraw(9002, 100000.0f, false, tc::kUse3D, false, 0, 0);   // magnified: the chain, 87381 texels at 0.5 B
    tc::CensusDraw(9003, 5000.0f, false, tc::kUse3D, false, 0, 0);     // 4096 texels at 2 B
    tc::CensusDraw(9004, 291000.0f, true, tc::kUse2D, true, 640, 456); // an EFB copy over the screen: 291000 at 4 B
    tc::CensusPicture();
    bool head = false, top1 = false;
    for (const std::string& l : g_lines) {
        if (l.find("tex-census: 1 pictures") == 0 && l.find("RGBA/16-bit convertible 0.04 MB") != std::string::npos &&
            l.find("would save at least 0.03 MB") != std::string::npos && l.find("DXT 0.04 MB") != std::string::npos)
            head = true;
        if (l.find("tex-census top 1: id 9004 render target 640x456") == 0) top1 = true;
        std::printf("D: %s\n", l.c_str());
    }
    Check(head, "census: groups and saving");
    Check(top1, "census: the top list in order");
}

int main(int argc, char** argv) {
    const int mode = tc::Mode();
    const std::string phase = argc > 1 ? argv[1] : "";
    if (phase != "second") PartA();
    FILE* digests = phase == "first" && argc > 2 ? std::fopen(argv[2], "w") : nullptr;
    FILE* expect = phase == "second" && argc > 2 ? std::fopen(argv[2], "r") : nullptr;
    PartB(mode, digests, expect);
    PartD();
    if (digests) std::fclose(digests);
    if (expect) std::fclose(expect);
    std::printf(g_fail ? "vgx_texcomp: FAIL (%d)\n" : "vgx_texcomp: PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
