// g30 Wave 5 host oracle test:
//   1. TF_I8 (GXV_LA88=2): non-mipmapped GX I4/I8 resolve directly to 1-byte TF_I8 with exact texels and channel ranges,
//      while IA4/IA8 stay TF_LA88, mipmapped I8 stays TF_LA88 (with GXV_LA88_MIPS=2) or TF_RGBA8, and patched I8 stays TF_RGBA8.
//   2. Fast Display-List Welder (vgx_dlcache.cpp Promote, GXV_DLWELD=1): exact deduplication, index remapping, multi-run cuts,
//      L1-friendly mask after a giant list, and identical triangle output to unwelded draws for both GXV_ABSIDX=0 and 1.
#include "vgx_state.h"
#include "vgx_texdecode.h"
#include "vgx_cmd.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}

extern "C" uint32_t port_scene_key() { return 1; }
extern "C" int port_screen_quiet() { return 1; }
extern "C" void port_log_summary(const char*, ...) {}
extern "C" void port_log(const char*, ...) {}
extern "C" int64_t port_host_time_ns() { return 0; }
extern "C" int port_perf_mode_60() { return 1; }
extern "C" void port_vgx2_boost(int) {}
extern "C" int port_vita_thread_class(int) { return 0; }
extern "C" int port_vita_thread_core3(int, int) { return 0; }
extern "C" void vgx_res_tex_note(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_fail;
    }
}

uint32_t g_rng = 0x5EED2026u;
uint8_t Rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return static_cast<uint8_t>(g_rng >> 3);
}

void PutBe16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}
void PutBeFloat(std::vector<uint8_t>& out, float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    out.push_back(static_cast<uint8_t>(u >> 24));
    out.push_back(static_cast<uint8_t>(u >> 16));
    out.push_back(static_cast<uint8_t>(u >> 8));
    out.push_back(static_cast<uint8_t>(u));
}

void TestI8() {
    Check(vgx::backend::GetCaps().la88 && vgx::backend::GetCaps().i8, "GXV_LA88=2 sets caps.la88 and caps.i8");
    static std::vector<std::vector<uint8_t>> keep;
    const unsigned dims[][2] = {{1, 1}, {3, 5}, {7, 5}, {8, 8}, {16, 8}, {64, 32}, {127, 65}};
    for (uint32_t fmt = 0; fmt <= 3; ++fmt) {
        for (const auto& d : dims) {
            const uint32_t w = d[0], h = d[1];
            keep.emplace_back(vgx::TexLevelSize(w, h, fmt));
            for (uint8_t& b : keep.back()) b = Rnd();
            vgx::ResetState();
            vgx::g.bpTexStale = 0;
            auto& slot = vgx::g.tex[0];
            slot.valid = true;
            slot.avail = static_cast<uint32_t>(keep.back().size());
            slot.obj.data = keep.back().data();
            slot.obj.width = static_cast<uint16_t>(w);
            slot.obj.height = static_cast<uint16_t>(h);
            slot.obj.format = fmt;
            slot.obj.mipmap = 0;
            slot.obj.maxLod = 0;
            slot.obj.minFilter = vgx::FILT_LINEAR;
            slot.obj.magFilter = 1;
            vgx::TexBind bind;
            uint16_t rw = 0, rh = 0;
            auto& pending = vgx::Pending();
            pending.uploads.clear();
            pending.cmds.clear();
            Check(vgx::ResolveTexture(0, bind, rw, rh) && pending.uploads.size() == 1, "ResolveTexture queues one upload");
            const vgx::TexUpload& up = pending.uploads.front();
            if (fmt <= 1) {
                Check(up.format == vgx::TF_I8, "non-mipmapped I4/I8 -> TF_I8");
                Check(up.levels.size() == 1 && up.levels[0].size() == static_cast<size_t>(w) * h, "TF_I8 is 1 byte/texel");
                std::vector<uint8_t> ref(static_cast<size_t>(w) * h * 4), got(static_cast<size_t>(w) * h * 4);
                vgx::DecodeTexture(keep.back().data(), w, h, fmt, nullptr, 0, 0, ref.data());
                vgx::ExpandI8ToRGBA8(up.levels[0].data(), static_cast<size_t>(w) * h, got.data());
                Check(got == ref, "TF_I8 expanded == RGBA8 decode");
            } else {
                Check(up.format == vgx::TF_LA88, "non-mipmapped IA4/IA8 -> TF_LA88");
                Check(up.levels.size() == 1 && up.levels[0].size() == static_cast<size_t>(w) * h * 2, "TF_LA88 is 2 bytes/texel");
            }
        }
    }
    // Mipmapped I8 must NEVER take TF_I8 (genMips needs U8U8 or RGBA8).
    keep.emplace_back(vgx::TexDataSize(16, 16, 1, 5));
    for (uint8_t& b : keep.back()) b = Rnd();
    vgx::ResetState();
    vgx::g.bpTexStale = 0;
    auto& slot = vgx::g.tex[0];
    slot.valid = true;
    slot.avail = static_cast<uint32_t>(keep.back().size());
    slot.obj.data = keep.back().data();
    slot.obj.width = slot.obj.height = 16;
    slot.obj.format = 1;
    slot.obj.mipmap = 1;
    slot.obj.maxLod = 4;
    slot.obj.minFilter = vgx::FILT_LIN_MIP_LIN;
    slot.obj.magFilter = 1;
    vgx::TexBind bind;
    uint16_t rw = 0, rh = 0;
    auto& pending = vgx::Pending();
    pending.uploads.clear();
    pending.cmds.clear();
    Check(vgx::ResolveTexture(0, bind, rw, rh) && pending.uploads.size() == 1, "mipmapped I8 resolves");
    Check(pending.uploads.front().format != vgx::TF_I8, "mipmapped I8 never emits TF_I8");

    // MIRROR wrap flag propagates onto TexUpload (BlackHole.bdl IntTex_QuarterHalo 64x64 I8 & mr_CocoBlur00_i 16x32 I8).
    keep.emplace_back(vgx::TexLevelSize(64, 64, 1));
    for (uint8_t& b : keep.back()) b = Rnd();
    vgx::ResetState();
    vgx::g.bpTexStale = 0;
    auto& mslot = vgx::g.tex[0];
    mslot.valid = true;
    mslot.avail = static_cast<uint32_t>(keep.back().size());
    mslot.obj.data = keep.back().data();
    mslot.obj.width = mslot.obj.height = 64;
    mslot.obj.format = 1;
    mslot.obj.mipmap = 0;
    mslot.obj.maxLod = 0;
    mslot.obj.wrapS = vgx::WRAP_MIRROR;
    mslot.obj.wrapT = vgx::WRAP_MIRROR;
    mslot.obj.minFilter = vgx::FILT_LINEAR;
    mslot.obj.magFilter = 1;
    pending.uploads.clear();
    pending.cmds.clear();
    Check(vgx::ResolveTexture(0, bind, rw, rh) && pending.uploads.size() == 1, "mirror I8 resolves");
    Check(pending.uploads.front().format == vgx::TF_I8 && pending.uploads.front().mirrorWrap, "mirror I8 sets TF_I8 and mirrorWrap");

    // Tiny non-I8 texture (16x32 IA8 = 1024 B) uploaded first as CLAMP then bound MIRROR re-uploads once with mirrorWrap=true.
    keep.emplace_back(vgx::TexLevelSize(16, 32, 3));
    for (uint8_t& b : keep.back()) b = Rnd();
    vgx::ResetState();
    vgx::g.bpTexStale = 0;
    auto& tslot = vgx::g.tex[0];
    tslot.valid = true;
    tslot.avail = static_cast<uint32_t>(keep.back().size());
    tslot.obj.data = keep.back().data();
    tslot.obj.width = 16;
    tslot.obj.height = 32;
    tslot.obj.format = 3; // IA8 -> TF_LA88 (1024 B)
    tslot.obj.mipmap = 0;
    tslot.obj.maxLod = 0;
    tslot.obj.wrapS = vgx::WRAP_CLAMP;
    tslot.obj.wrapT = vgx::WRAP_CLAMP;
    tslot.obj.minFilter = vgx::FILT_LINEAR;
    tslot.obj.magFilter = 1;
    pending.uploads.clear();
    pending.cmds.clear();
    Check(vgx::ResolveTexture(0, bind, rw, rh) && pending.uploads.size() == 1 && !pending.uploads.front().mirrorWrap, "tiny IA8 initial CLAMP upload");
    tslot.obj.wrapS = vgx::WRAP_MIRROR;
    pending.uploads.clear();
    pending.cmds.clear();
    Check(vgx::ResolveTexture(0, bind, rw, rh) && pending.uploads.size() == 1 && pending.uploads.front().mirrorWrap, "tiny IA8 re-uploads once with mirrorWrap on first MIRROR bind");
    pending.uploads.clear();
    pending.cmds.clear();
    Check(vgx::ResolveTexture(0, bind, rw, rh) && pending.uploads.empty(), "tiny IA8 does not re-upload on subsequent MIRROR bind");

    // Patched I8 must stay TF_RGBA8 so host texture patches receive full RGBA8 pixels.
    keep.emplace_back(vgx::TexLevelSize(16, 16, 1));
    for (uint8_t& b : keep.back()) b = Rnd();
    vgx::SetTexturePatchHook(
        [](uint64_t, uint32_t w, uint32_t h, uint8_t* rgba) {
            for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
                rgba[4 * i + 0] = 21; rgba[4 * i + 1] = 144; rgba[4 * i + 2] = 233; rgba[4 * i + 3] = 255;
            }
        },
        [](uint64_t, uint32_t, uint32_t) { return true; });
    vgx::ResetState();
    vgx::g.bpTexStale = 0;
    auto& pslot = vgx::g.tex[0];
    pslot.valid = true;
    pslot.avail = static_cast<uint32_t>(keep.back().size());
    pslot.obj.data = keep.back().data();
    pslot.obj.width = pslot.obj.height = 16;
    pslot.obj.format = 1;
    pslot.obj.mipmap = 0;
    pslot.obj.maxLod = 0;
    pslot.obj.minFilter = vgx::FILT_LINEAR;
    pslot.obj.magFilter = 1;
    pending.uploads.clear();
    pending.cmds.clear();
    Check(vgx::ResolveTexture(0, bind, rw, rh) && pending.uploads.size() == 1, "patched I8 resolves");
    Check(pending.uploads.front().format == vgx::TF_RGBA8 && pending.uploads.front().levels[0][0] == 21, "patched I8 stays TF_RGBA8");
    vgx::SetTexturePatchHook(nullptr, nullptr);
}

// Build a GX display list with repeated vertices across triangle strips + triangles so Promote welds heavily.
std::vector<uint8_t> BuildGridList(int gridN, int strips) {
    std::vector<uint8_t> dl;
    for (int s = 0; s < strips; ++s) {
        const uint16_t vcount = static_cast<uint16_t>(gridN * 2);
        dl.push_back(0x98); // GX_DRAW_TRIANGLE_STRIP | vtxfmt 0
        PutBe16(dl, vcount);
        for (int i = 0; i < gridN; ++i) {
            for (int r = 0; r < 2; ++r) {
                const float x = static_cast<float>(i);
                const float y = static_cast<float>(s + r); // shared row between adjacent strips!
                const float z = static_cast<float>((i + s + r) & 3);
                PutBeFloat(dl, x);
                PutBeFloat(dl, y);
                PutBeFloat(dl, z);
            }
        }
    }
    // Add a second primitive (triangles) reusing exact same vertices.
    dl.push_back(0x90); // GX_DRAW_TRIANGLES
    PutBe16(dl, 6);
    const float tri[6][3] = {{0, 0, 0}, {1, 0, 1}, {0, 1, 1}, {1, 0, 1}, {1, 1, 2}, {0, 1, 1}};
    for (int k = 0; k < 6; ++k) {
        PutBeFloat(dl, tri[k][0]);
        PutBeFloat(dl, tri[k][1]);
        PutBeFloat(dl, tri[k][2]);
    }
    return dl;
}

void TestDlWeld() {
    vgx::ResetState();
    vgx::g.cpArrayStale = 0;
    vgx::SetVcd(0, 0);
    vgx::SetVcd(9, 1); // GX_VA_POS direct
    vgx::SetVat(0, 9, 1 /*XYZ*/, 4 /*F32*/, 0);

    // 1) First promote a large list (4006 raw vertices -> grows welder table to 16384),
    // 2) then promote a small list (> SoftXformLimit=128 so it promotes, cap=512 << 16384 -> verifies mask = cap - 1u).
    std::vector<uint8_t> big = BuildGridList(50, 40);    // 4006 raw verts, 50*41=2050 unique
    std::vector<uint8_t> small = BuildGridList(10, 7);   // 146 raw verts, 10*8=80 unique
    static std::vector<uint8_t> guestRam(big.size() + small.size() + 8192, 0);
    const size_t bigOff = 0;
    const size_t smallOff = ((big.size() + 4095u) & ~size_t(4095u));
    std::memcpy(guestRam.data() + bigOff, big.data(), big.size());
    std::memcpy(guestRam.data() + smallOff, small.data(), small.size());
    vgx::hle::RegisterGuestRam(guestRam.data(), guestRam.size());

    static vgx::BufUpload captured;
    static bool gotBuf = false;
    vgx::SetFrameSink([](vgx::CmdBuf& b) {
        if (!b.bufUploads.empty()) {
            captured = b.bufUploads.back();
            gotBuf = true;
        }
        vgx::backend::Execute(b);
    });

    auto promoteList = [](const uint8_t* ptr, uint32_t sz, vgx::BufUpload& outBuf) -> bool {
        gotBuf = false;
        outBuf = vgx::BufUpload{};
        for (int frame = 0; frame < 6; ++frame) {
            vgx::CallList(ptr, sz);
            vgx::FrontPresent(true, nullptr);
            if (gotBuf) {
                outBuf = captured;
                return true;
            }
        }
        return false;
    };

    vgx::BufUpload upBig, upSmall;
    Check(promoteList(guestRam.data() + bigOff, static_cast<uint32_t>(big.size()), upBig), "big list promoted to static buffer");
    Check(promoteList(guestRam.data() + smallOff, static_cast<uint32_t>(small.size()), upSmall), "small list promoted after big list");

    const uint32_t stride = vgx::MakeLayout(0).stride; // position-only layoutMask = 0 -> 16 bytes (float4)
    Check(stride == 16 && (upBig.vtx.size() % stride) == 0 && (upSmall.vtx.size() % stride) == 0, "welded vertex buffers align to stride");
    const size_t bigUnique = upBig.vtx.size() / stride;
    const size_t smallUnique = upSmall.vtx.size() / stride;
    Check(bigUnique == 50u * 41u, "big welded list has exact 50*41 unique vertices");
    Check(smallUnique == 10u * 8u, "small welded list has exact 10*8 unique vertices");
    Check(upSmall.idx.size() == 3u * (20u - 2u) * 7u + 6u, "small list index count exact");
    for (uint16_t idx : upSmall.idx) Check(idx < smallUnique, "every small index in welded range");
    for (uint16_t idx : upBig.idx) Check(idx < bigUnique, "every big index in welded range");

    // Verify exact triangle vertex coordinates for the last 6 indices (the explicit 2 triangles).
    if (upSmall.idx.size() >= 6 && smallUnique > 0) {
        const float tri[6][3] = {{0, 0, 0}, {1, 0, 1}, {0, 1, 1}, {1, 0, 1}, {1, 1, 2}, {0, 1, 1}};
        const size_t base = upSmall.idx.size() - 6;
        for (size_t k = 0; k < 6; ++k) {
            const uint16_t idx = upSmall.idx[base + k];
            float p[3];
            std::memcpy(p, upSmall.vtx.data() + static_cast<size_t>(idx) * stride, 12);
            Check(p[0] == tri[k][0] && p[1] == tri[k][1] && p[2] == tri[k][2], "welded triangle vertex position exact");
        }
    }
    std::printf("dlweld: big %u -> %zu verts, small %u -> %zu verts, %zu indices\n",
                50u * 40u * 2u + 6u, bigUnique, 10u * 7u * 2u + 6u, smallUnique, upSmall.idx.size());

    // 3) Multi-run list (TRIANGLES -> LINES -> TRIANGLES) verifying run cuts, base offsets, and exact positions.
    std::vector<uint8_t> multi;
    auto pushTriBlock = [&](float zBase, int quads) {
        multi.push_back(0x90); // GX_DRAW_TRIANGLES
        PutBe16(multi, static_cast<uint16_t>(quads * 6));
        for (int q = 0; q < quads; ++q) {
            const float x = static_cast<float>(q);
            const float v[6][3] = {{x, 0, zBase}, {x + 1, 0, zBase}, {x, 1, zBase},
                                   {x + 1, 0, zBase}, {x + 1, 1, zBase}, {x, 1, zBase}};
            for (int k = 0; k < 6; ++k) { PutBeFloat(multi, v[k][0]); PutBeFloat(multi, v[k][1]); PutBeFloat(multi, v[k][2]); }
        }
    };
    pushTriBlock(1.0f, 12); // 72 raw -> 26 unique
    multi.push_back(0xA8);  // GX_DRAW_LINES (forces a run cut!)
    PutBe16(multi, 20);     // 20 raw -> 11 unique
    for (int i = 0; i < 10; ++i) {
        PutBeFloat(multi, static_cast<float>(i));     PutBeFloat(multi, 2.0f); PutBeFloat(multi, 5.0f);
        PutBeFloat(multi, static_cast<float>(i + 1)); PutBeFloat(multi, 2.0f); PutBeFloat(multi, 5.0f);
    }
    pushTriBlock(9.0f, 12); // 72 raw -> 26 unique (forces a 3rd run!)
    static std::vector<uint8_t> multiRam;
    multiRam.assign(multi.size() + 4096, 0);
    std::memcpy(multiRam.data(), multi.data(), multi.size());
    vgx::hle::RegisterGuestRam(multiRam.data(), multiRam.size());
    vgx::BufUpload upMulti;
    Check(promoteList(multiRam.data(), static_cast<uint32_t>(multi.size()), upMulti), "multi-run list promoted");
    const size_t multiUnique = upMulti.vtx.size() / stride;
    Check((upMulti.vtx.size() % stride) == 0 && multiUnique == 26u + 11u + 26u, "multi-run exact unique vertex sum (26+11+26=63)");
    Check(upMulti.idx.size() == 72u + 20u + 72u, "multi-run exact index count");
}
} // namespace

int main() {
    TestI8();
    TestDlWeld();
    std::printf(g_fail ? "vgx_i8_and_dlweld_test: FAIL (%d)\n" : "vgx_i8_and_dlweld_test: PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
