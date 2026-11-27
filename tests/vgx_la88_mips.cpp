// GXV_LA88_MIPS host proof (tools/test_vgx_la88_mips.sh; revisit23).
//   A. For every I/IA format (I4, I8, IA4, IA8), sizes 8..128 and random texels: the LA88 decode expanded with ExpandLA88ToRGBA8 (the
//      back end's fallback when the console's transfer unit is not exact) is byte for byte the RGBA8 decode the old path uploaded; so
//      the sampler's (L, L, L, A) of the LA88 level 0 is the old texel, and the fallback upload is the old upload.
//   B. The front end (ResolveTexture, null back end): a mipmapped IA8 / I8 texture with a mip filter goes as LA88 + genMips with
//      GXV_LA88_MIPS=1 or 2, as RGBA8 + genMips with 0 (the old path), and as RGBA8 again once the back end reports "not exact"
//      (La88MipsVerdict(-1)); non-mipmapped ones go as LA88 in every mode (GXV_LA88, unchanged).
#include "vgx_state.h"
#include "vgx_texdecode.h"
#include "vgx_la88_gate.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
void La88MipsVerdict(int state);
}
static int quiet = 0;
extern "C" uint32_t port_scene_key() { return 1; }
extern "C" int port_screen_quiet() { return quiet; }
extern "C" void port_log_summary(const char*, ...) {}
extern "C" void port_log(const char*, ...) {}
extern "C" int64_t port_host_time_ns() { return 0; }
extern "C" int port_perf_mode_60() { return 1; }
extern "C" void port_vgx2_boost(int) {}
extern "C" int port_vita_thread_class(int) { return 0; }
extern "C" int port_vita_thread_core3(int, int) { return 0; }
extern "C" void vgx_res_tex_note(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_fail; }
}

static uint32_t g_rng = 12345;
static uint8_t Rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return static_cast<uint8_t>(g_rng >> 7); }

static void PartA() {
    unsigned cases = 0;
    for (uint32_t fmt = 0; fmt <= 3; ++fmt)
        for (uint32_t w = 8; w <= 128; w *= 2)
            for (uint32_t h = 8; h <= 128; h *= 2) {
                std::vector<uint8_t> src(vgx::TexLevelSize(w, h, fmt));
                for (uint8_t& b : src) b = Rnd();
                std::vector<uint8_t> rgba(w * h * 4), la(w * h * 2), back(w * h * 4);
                vgx::DecodeTexture(src.data(), w, h, fmt, nullptr, 0, 0, rgba.data());
                Check(vgx::DecodeTextureLA88(src.data(), w, h, fmt, la.data()), "DecodeTextureLA88");
                vgx::ExpandLA88ToRGBA8(la.data(), w * h, back.data());
                if (back != rgba) { std::fprintf(stderr, "FAIL: format %u %ux%u: expanded LA88 differs from RGBA8\n", fmt, w, h); ++g_fail; }
                if (fmt <= 1) {
                    std::vector<uint8_t> i8(w * h);
                    Check(vgx::DecodeTextureI8(src.data(), w, h, fmt, i8.data()), "DecodeTextureI8");
                    vgx::ExpandI8ToRGBA8(i8.data(), w * h, back.data());
                    if (back != rgba) { std::fprintf(stderr, "FAIL: format %u %ux%u: expanded I8 differs from RGBA8\n", fmt, w, h); ++g_fail; }
                }
                ++cases;
            }
    std::printf("A: %u format/size cases, expanded LA88 == RGBA8 decode%s\n", cases, g_fail ? " (FAILURES)" : "");
    for (uint32_t fmt : {1u, 2u, 3u}) {
        const uint32_t w = 256, h = 256;
        std::vector<uint8_t> src(vgx::TexLevelSize(w, h, fmt)), rgba(w*h*4), la(w*h*2), back(w*h*4);
        if (fmt == 3) {
            for (unsigned i = 0; i < 65536; ++i) { src[2*i] = uint8_t(i >> 8); src[2*i+1] = uint8_t(i); }
        } else {
            for (size_t i = 0; i < src.size(); ++i) src[i] = uint8_t(i);
        }
        vgx::DecodeTexture(src.data(), w, h, fmt, nullptr, 0, 0, rgba.data());
        Check(vgx::DecodeTextureLA88(src.data(), w, h, fmt, la.data()), "compact full alphabet");
        vgx::ExpandLA88ToRGBA8(la.data(), w*h, back.data());
        Check(rgba == back, "every I8/IA4/IA8 source value is exact");
    }
}

// One resolve of a fresh texture (its own data, so the cache never hits).
static vgx::TexUpload ResolveOne(uint32_t fmt, bool mipmapped) {
    static std::vector<std::vector<uint8_t>> keep;
    keep.emplace_back(4096);
    for (uint8_t& b : keep.back()) b = Rnd();
    vgx::ResetState();
    vgx::g.bpTexStale = 0;
    auto& slot = vgx::g.tex[0];
    slot.valid = true;
    slot.obj.data = keep.back().data();
    slot.obj.width = slot.obj.height = 16;
    slot.obj.format = fmt;
    slot.obj.mipmap = mipmapped ? 1 : 0;
    slot.obj.maxLod = mipmapped ? 4 : 0;
    slot.obj.minFilter = mipmapped ? vgx::FILT_LIN_MIP_LIN : vgx::FILT_LINEAR;
    slot.obj.magFilter = 1;
    slot.avail = static_cast<uint32_t>(keep.back().size());
    vgx::TexBind bind;
    uint16_t width = 0, height = 0;
    auto& pending = vgx::Pending();
    pending.uploads.clear();
    pending.cmds.clear();
    const bool ok = vgx::ResolveTexture(0, bind, width, height);
    Check(ok && pending.uploads.size() == 1, "one upload");
    return pending.uploads.empty() ? vgx::TexUpload{} : pending.uploads.front();
}

int main(int argc, char** argv) {
    const int mode = argc > 1 ? std::atoi(argv[1]) : 0;
    PartA();
    for (unsigned f = 0; f < 16; ++f) for (unsigned w : {4u, 8u, 17u, 64u, 1024u, 2048u}) {
        const bool good = f >= 1 && f <= 3 && (w == 8 || w == 64 || w == 1024);
        Check(vgx::la88::Candidate(f, w, w, true, false) == good, "only proved dimensions/formats");
        Check(!vgx::la88::Candidate(f, w, w/2, true, false) && !vgx::la88::Candidate(f, w, w, false, false) &&
              !vgx::la88::Candidate(f, w, w, true, true), "rectangular, authored or packed fallback");
    }
    quiet = 0;
    Check(ResolveOne(1, true).format == (mode == 2 ? vgx::TF_LA88 : vgx::TF_RGBA8), "no first probe in play");
    quiet = 1;
    for (uint32_t fmt : {1u, 2u, 3u}) { // I8, IA4, IA8
        const vgx::TexUpload m = ResolveOne(fmt, true);
        const vgx::TexUpload n = ResolveOne(fmt, false);
        Check(n.format == vgx::TF_LA88 && !n.genMips && n.levels.size() == 1, "non-mipmapped: LA88, one level (unchanged)");
        if (mode == 0) {
            Check(m.format == vgx::TF_RGBA8 && m.genMips && m.levels.size() == 1, "GXV_LA88_MIPS=0: mipmapped as RGBA8 + genMips (old path)");
        } else {
            Check(m.format == vgx::TF_LA88 && m.genMips && m.levels.size() == 1 && m.levels[0].size() == 16 * 16 * 2,
                  "GXV_LA88_MIPS on: mipmapped as LA88 + genMips, level 0 only");
        }
        std::printf("B: mode %d format %u: mipmapped -> %s%s, non-mipmapped -> %s\n", mode, fmt, m.format == vgx::TF_LA88 ? "LA88" : "RGBA8",
                    m.genMips ? " + generated levels" : "", n.format == vgx::TF_LA88 ? "LA88" : "RGBA8");
    }
    if (mode) {
        quiet = 0;
        vgx::La88MipsVerdict(1);
        Check(ResolveOne(3, true).format == vgx::TF_LA88, "approved uploads eligible during play");
        vgx::La88MipsVerdict(-1); // (the console's probe said "not exact")
        const vgx::TexUpload m = ResolveOne(3, true);
        Check(m.format == vgx::TF_RGBA8 && m.genMips, "after a 'not exact' verdict: RGBA8 + genMips again");
        std::printf("B: mode %d after the back end's 'not exact': mipmapped -> %s\n", mode, m.format == vgx::TF_LA88 ? "LA88" : "RGBA8");
    }
    std::printf(g_fail ? "vgx_la88_mips: FAIL (%d)\n" : "vgx_la88_mips: PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
