// Fur density map byte order (GAL-023, GXV_FIX_FURMAP): port::fur::BuildMap against the production IA8 decoder.
// The map must read back through vgx::DecodeTexture as the big-endian console image: strand texels alpha = u8(255 * thickness),
// intensity = 255 - length; other texels alpha 0, intensity 255. Usage: fur_map [--env EXPECTED]  (Enabled() of the environment)
#include "port/fur_map.h"
#include "vgx_texdecode.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
struct Lcg {
    uint32_t s = 12345;
    float operator()(float a, float b) { s = s * 1664525u + 1013904223u; return a + (b - a) * ((s >> 8) / 16777216.0f); }
};
using Count = std::map<std::pair<int, int>, int>;  // (alpha, intensity) -> texels
Count Decode(const std::vector<uint8_t>& image, uint32_t w, uint32_t h) {
    std::vector<uint8_t> rgba(w * h * 4);
    vgx::DecodeTexture(image.data(), w, h, 3 /* IA8 */, nullptr, 0, 0, rgba.data());
    Count c;
    for (uint32_t i = 0; i < w * h; ++i) ++c[{rgba[i * 4 + 3], rgba[i * 4]}];
    return c;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "--env")) {
        const bool want = std::atoi(argv[2]) != 0;
        Check(port::fur::Enabled() == want, "GXV_FIX_FURMAP read once from the environment");
        return g_fail ? 1 : 0;
    }
    // Byte order of one store, on any host.
    uint8_t two[4] = {0xEE, 0xEE, 0xEE, 0xEE};
    port::fur::StoreU16(two, 1, 0x1234, true);
    Check(two[0] == 0xEE && two[1] == 0xEE && two[2] == 0x12 && two[3] == 0x34, "console order: high byte first, neighbours untouched");
    uint16_t host = 0;
    port::fur::StoreU16(&host, 0, 0x1234, false);
    Check(host == 0x1234, "host order is the plain u16 store");

    // The game's default fur (FurParam: densities, thicknesses, length map), a 32x32 map.
    const uint32_t W = 32, H = 32;
    const float density[4] = {0.8f, 0.6f, 0.3f, 0.09f};
    const float thickness[4] = {0.517f, 0.386f, 0.3645f, 0.1713f};
    const uint8_t length[4] = {26, 64, 98, 212};
    std::vector<uint8_t> be(W * H * 2), host16(W * H * 2);
    Lcg a, b;
    port::fur::BuildMap(be.data(), W, H, density, thickness, length, a, true);
    port::fur::BuildMap(host16.data(), W, H, density, thickness, length, b, false);
    const Count good = Decode(be, W, H), swapped = Decode(host16, W, H);
    int strands = 0, stray = 0;
    for (const auto& kv : good) {
        bool known = kv.first == std::make_pair(0, 255);
        for (int t = 0; t < 4; ++t) known |= kv.first == std::make_pair(int(uint16_t(255.0f * thickness[t]) & 0xFF), 255 - length[t]);
        if (kv.first != std::make_pair(0, 255)) strands += kv.second;
        if (!known) stray += kv.second;
    }
    std::printf("console order: %zu distinct (alpha,intensity) texels, %d strand texels of %u\n", good.size(), strands, W * H);
    Check(stray == 0, "every decoded texel is a base texel (0,255) or one of the four strand types");
    Check(strands > 0, "strand texels exist");
    int baseSwapped = 0;
    const auto it = swapped.find({255, 0});
    if (it != swapped.end()) baseSwapped = it->second;
    std::printf("host order on this CPU: %d texels decode as alpha 255 / intensity 0\n", baseSwapped);
    // The old store is the console image only on a big-endian CPU: on a little-endian one alpha and intensity are exchanged.
    const bool little = [] { const uint16_t x = 1; uint8_t c; std::memcpy(&c, &x, 1); return c == 1; }();
    Check(little ? (baseSwapped > 0 && good.find({255, 0}) == good.end()) : be == host16, "host-order store differs from the console image only on little-endian");
    // Same random calls, same positions: the two images are byte-swapped copies of each other.
    bool mirrored = true;
    for (size_t i = 0; i < be.size(); i += 2) mirrored &= be[i] == host16[i + 1] && be[i + 1] == host16[i];
    Check(little ? mirrored : true, "same texels, bytes exchanged");
    // Shell alpha: the fur TEV adds TEXA * previous alpha to a constant; the base texel must leave the constant alone (alpha 0).
    Check(good.count({0, 255}) && good.at({0, 255}) > 0, "texels outside the strands have alpha 0 (shells stay transparent there)");
    return g_fail ? 1 : 0;
}
