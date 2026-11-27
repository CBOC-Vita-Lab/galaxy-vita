// GXV_AUTOMIP (automip25): the pure parts, no GL: (1) the coverage-preserving alpha of an own chain (automip::Coverage) against the plain
// box average, on a chain-link fence (a texel line every fourth texel, 44 % covered) and on a hole pattern: the share of texels that pass an
// alpha test at 0.5 at each level; (2) the cost of the chain: decode, box filter and range-fit encode, timed on the host (tools/test_vgx_automip_gl.sh
// prints it with the factor for the console). Usage: vgx_automip_chain
#include "vgx_automip.h"
#include "vgx_texdecode.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}
using Chain = std::vector<std::vector<uint8_t>>;
Chain MakeChain(int n, int kind) {
    std::vector<uint8_t> l0(static_cast<size_t>(n) * n * 4, 255);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            bool op;
            if (kind == 0) op = (x & 3) == 0 || (y & 3) == 0;                          // chain-link: lines
            else if (kind == 1) op = !(((x * 7 + y * 13) % 10) < 3);                    // 30 % holes, scattered
            else op = ((x / 3 + y / 3) & 1) == 0 || ((x % 3) == 1 && (y % 5) == 0);     // blobs
            l0[(static_cast<size_t>(y) * n + x) * 4 + 3] = op ? 255 : 0;
        }
    Chain c;
    c.push_back(l0);
    uint32_t w = n, h = n;
    while (w > 1 || h > 1) {
        std::vector<uint8_t> next;
        uint32_t nw, nh;
        vgx::Downsample(c.back().data(), w, h, next, nw, nh);
        c.push_back(std::move(next));
        w = nw; h = nh;
    }
    return c;
}
double Share(const std::vector<uint8_t>& l) {
    size_t k = 0;
    for (size_t i = 3; i < l.size(); i += 4) k += l[i] >= 128;
    return 100.0 * static_cast<double>(k) / static_cast<double>(l.size() / 4);
}
} // namespace
int main() {
    const char* names[3] = {"chain-link (lines every 4th texel)", "scattered holes", "blobs"};
    for (int kind = 0; kind < 3; ++kind) {
        Chain plain = MakeChain(128, kind), fixed = plain;
        vgx::automip::Coverage(fixed);
        std::printf("%s: share of texels passing alpha >= 128 (%%), level 0 = %.1f\n   level:", names[kind], Share(plain[0]));
        for (size_t k = 0; k < plain.size() && k < 6; ++k) std::printf(" %5zu", k);
        std::printf("\n   plain:");
        for (size_t k = 0; k < plain.size() && k < 6; ++k) std::printf(" %5.1f", Share(plain[k]));
        std::printf("\n   fixed:");
        for (size_t k = 0; k < fixed.size() && k < 6; ++k) std::printf(" %5.1f", Share(fixed[k]));
        std::printf("\n");
        // level 1 and 2 (where the texels still differ) must be nearer the level-0 share with the fix than without
        const double want = Share(plain[0]);
        for (size_t k = 1; k <= 2; ++k) {
            const double dp = std::abs(Share(plain[k]) - want), df = std::abs(Share(fixed[k]) - want);
            char what[120];
            std::snprintf(what, sizeof what, "%s level %zu: error %.1f -> %.1f points", names[kind], k, dp, df);
            Check(df <= dp + 1e-9, what);
        }
        // colours are untouched by the fix
        bool same = true;
        for (size_t k = 1; k < plain.size(); ++k)
            for (size_t i = 0; i < plain[k].size(); ++i)
                if ((i & 3) != 3 && plain[k][i] != fixed[k][i]) same = false;
        Check(same, "the fix changes alpha only");
    }
    // (2) cost
    for (int n : {64, 128, 256, 512}) {
        std::vector<uint8_t> cmpr(static_cast<size_t>(n) * n / 2);
        for (size_t i = 0; i < cmpr.size(); ++i) cmpr[i] = static_cast<uint8_t>(i * 131 + (i >> 5));
        std::vector<uint8_t> rgba(static_cast<size_t>(n) * n * 4), next;
        using Clock = std::chrono::steady_clock;
        const int reps = 20;
        auto t0 = Clock::now();
        for (int r = 0; r < reps; ++r) {
            vgx::DecodeTexture(cmpr.data(), n, n, 0xE, nullptr, 0, 0, rgba.data());
        }
        const double dec = std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / reps;
        t0 = Clock::now();
        for (int r = 0; r < reps; ++r) {
            std::vector<uint8_t> cur = rgba;
            uint32_t w = n, h = n;
            std::vector<uint8_t> enc;
            while ((w >> 1) >= 4 && (h >> 1) >= 4) {
                uint32_t nw, nh;
                vgx::Downsample(cur.data(), w, h, next, nw, nh);
                enc.resize(vgx::Dxt1Size(nw, nh));
                vgx::EncodeDxt1(next.data(), nw, nh, enc.data());
                cur.swap(next);
                w = nw; h = nh;
            }
        }
        const double chain = std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / reps;
        t0 = Clock::now();
        for (int r = 0; r < reps; ++r) {
            std::vector<uint8_t> cur = rgba;
            uint32_t w = n, h = n;
            while (w > 1 || h > 1) {
                uint32_t nw, nh;
                vgx::Downsample(cur.data(), w, h, next, nw, nh);
                cur.swap(next);
                w = nw; h = nh;
            }
        }
        const double box = std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / reps;
        std::printf("cost %dx%d: CMPR decode %.0f us; box chain to 1x1 %.0f us; box + DXT1 encode of the levels to 4x4 %.0f us (host, -O1)\n", n, n, dec, box, chain);
    }
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "all checks passed", g_fail);
    return g_fail ? 1 : 0;
}
