// heap23b (9 Nov 2026): the renderer's texture KiB in the per-scene heap line. Before heap23b it was Stats::textureBytes, every texel byte
// uploaded since start-up (a re-upload adds again, a deleted texture is never taken off): c24's "textures (N KiB)" climbed 33 -> 70 MiB
// over 30 minutes while the texture count stayed ~1,250 and vitaGL's video pool stayed flat. Stats::textureBytesLive is the texels of
// the textures alive now. The real GLES back end on Mesa (tests/vgx_backend_quiet.cpp's fixture), uploads / re-uploads / frees:
//   - textureBytes keeps its old meaning (sum of all uploads);
//   - textureBytesLive is the sum of each live texture's last upload (RGBA8 4 bytes a texel, 16-bit 2, DXT1 its blocks, generated
//     levels not counted, as textureBytes never counted them), and 0 when every texture is freed.
// tools/test_vgx_tex_bytes.sh.
#define main quiet_fixture_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "vgx_backend_quiet.cpp"
#pragma clang diagnostic pop
#undef main

namespace {
vgx::TexUpload Tex(uint32_t id, uint16_t w, uint16_t h, uint8_t format, bool mips) {
    vgx::TexUpload u;
    u.texId = id;
    u.width = w;
    u.height = h;
    u.format = format;
    const unsigned bpp = format == vgx::TF_RGBA8 ? 4u : 2u;
    if (format == vgx::TF_DXT1) {
        // a full chain of DXT1 levels down to 4x4
        for (uint16_t lw = w, lh = h; lw >= 4 && lh >= 4; lw >>= 1, lh >>= 1) {
            u.levels.emplace_back(static_cast<size_t>(lw / 4) * (lh / 4) * 8u, uint8_t{0x5A});
            if (!mips) break;
        }
        u.authored = static_cast<uint8_t>(u.levels.size());
        return u;
    }
    u.levels.emplace_back(static_cast<size_t>(w) * h * bpp, uint8_t{0x7F});
    u.genMips = mips && format == vgx::TF_RGBA8;
    return u;
}
uint64_t Bytes(const vgx::TexUpload& u) {
    uint64_t n = 0;
    for (const auto& l : u.levels) n += l.size();
    return n;
}
void Run(std::vector<vgx::TexUpload> ups, std::vector<uint32_t> frees, uint64_t frame) {
    vgx::CmdBuf b;
    b.frame = frame;
    for (size_t i = 0; i < ups.size(); ++i) {
        b.uploads.push_back(std::move(ups[i]));
        b.cmds.push_back({vgx::CMD_TEX_UPLOAD, static_cast<uint32_t>(i)});
    }
    for (uint32_t id : frees) b.cmds.push_back({vgx::CMD_TEX_FREE, id});
    b.copyDisp.push_back({{0, 0, 32, 32}, {}, false});
    b.cmds.push_back({vgx::CMD_COPY_DISP, 0});
    b.cmds.push_back({vgx::CMD_PRESENT, 0});
    vgx::backend::Execute(b);
}
} // namespace

int main() {
    for (const auto* name : diagnosticNames) unsetenv(name);
    for (const auto* name : functionalNames) unsetenv(name);
    setenv("GXV_AUTOQ", "0", 1);
    Context();
    Require(vgx::backend::Init(32, 32), "real backend initialized");
    using vgx::backend::GetStats;
    const uint64_t total0 = GetStats().textureBytes, live0 = GetStats().textureBytesLive;
    const unsigned count0 = GetStats().textures;
    // frame 1: four textures
    std::vector<vgx::TexUpload> ups = {Tex(1, 64, 64, vgx::TF_RGBA8, false), Tex(2, 128, 64, vgx::TF_RGB565, false),
                                       Tex(3, 64, 64, vgx::TF_DXT1, true), Tex(4, 32, 32, vgx::TF_RGBA8, true)};
    uint64_t expectLive = 0, expectTotal = 0;
    for (const auto& u : ups) expectLive += Bytes(u);
    expectTotal = expectLive;
    Run(ups, {}, 1);
    std::printf("after 4 uploads: %u textures, live %llu (expected %llu), total %llu (expected %llu)\n", GetStats().textures - count0,
                (unsigned long long)(GetStats().textureBytesLive - live0), (unsigned long long)expectLive,
                (unsigned long long)(GetStats().textureBytes - total0), (unsigned long long)expectTotal);
    Require(GetStats().textures - count0 == 4, "four textures");
    Require(GetStats().textureBytesLive - live0 == expectLive, "live bytes = the four uploads");
    Require(GetStats().textureBytes - total0 == expectTotal, "total bytes = the four uploads");
    // frame 2: texture 1 uploaded again at another size (a changed texture), texture 2 again at its size
    std::vector<vgx::TexUpload> again = {Tex(1, 128, 128, vgx::TF_RGBA8, false), Tex(2, 128, 64, vgx::TF_RGB565, false)};
    expectLive += Bytes(again[0]) - Bytes(ups[0]);
    expectTotal += Bytes(again[0]) + Bytes(again[1]);
    Run(again, {}, 2);
    std::printf("after 2 re-uploads: live %llu (expected %llu), total %llu (expected %llu)\n", (unsigned long long)(GetStats().textureBytesLive - live0),
                (unsigned long long)expectLive, (unsigned long long)(GetStats().textureBytes - total0), (unsigned long long)expectTotal);
    Require(GetStats().textureBytesLive - live0 == expectLive, "a re-upload replaces the texture's bytes");
    Require(GetStats().textureBytes - total0 == expectTotal, "a re-upload adds to the total, as before");
    // frame 3: textures 1 and 3 freed
    expectLive -= Bytes(again[0]) + Bytes(ups[2]);
    Run({}, {1, 3}, 3);
    std::printf("after 2 frees: %u textures, live %llu (expected %llu), total %llu\n", GetStats().textures - count0,
                (unsigned long long)(GetStats().textureBytesLive - live0), (unsigned long long)expectLive,
                (unsigned long long)(GetStats().textureBytes - total0));
    Require(GetStats().textures - count0 == 2, "two textures left");
    Require(GetStats().textureBytesLive - live0 == expectLive, "freed textures leave the live bytes");
    Require(GetStats().textureBytes - total0 == expectTotal, "frees do not change the total (its old meaning)");
    // frame 4..203: a session's churn (a cache that uploads and frees at the same rate): the total climbs, the live bytes do not
    for (uint64_t f = 4; f < 204; ++f) {
        const uint32_t id = 10 + static_cast<uint32_t>(f % 2);
        Run({Tex(id, 64, 64, vgx::TF_RGBA8, false)}, {}, f);
        Run({}, {id}, f);
    }
    std::printf("after 200 upload/free pairs: %u textures, live %llu (expected %llu), total %llu\n", GetStats().textures - count0,
                (unsigned long long)(GetStats().textureBytesLive - live0), (unsigned long long)expectLive,
                (unsigned long long)(GetStats().textureBytes - total0));
    Require(GetStats().textureBytesLive - live0 == expectLive, "churn leaves the live bytes where they were");
    Require(GetStats().textureBytes - total0 == expectTotal + 200ull * 64 * 64 * 4, "churn climbs the total (the old figure)");
    Run({}, {2, 4}, 300);
    Require(GetStats().textureBytesLive == live0, "every texture freed: live bytes back to the start");
    std::printf("PASS: live texture bytes follow uploads, re-uploads and frees; the total keeps its old meaning\n");
    return 0;
}
