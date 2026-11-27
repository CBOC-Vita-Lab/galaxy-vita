// GXV_DLVCACHE (ta23): vgx_vcache.h's triangle order on meshes shaped like the game's welded display lists.
//   * every output is a permutation of the input's triangles, each triangle with its three indices in their order (winding and
//     first vertex kept), for grids cut into strips of 3..8 vertices (as J3D's exporter writes them), fans, random soups, meshes with
//     repeated and degenerate triangles, one-triangle lists, and every modelled cache size;
//   * the modelled vertex-shader runs (FIFO of 16 and of 32 entries) fall, and the figures are printed (ACMR: runs per triangle).
// Build: g++ -std=c++20 -O1 -Iplatform/vgx/src tests/vgx_vcache.cpp -o vgx_vcache && ./vgx_vcache
#include "vgx_vcache.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <tuple>
#include <vector>

namespace {
unsigned failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
uint32_t g_r = 12345;
uint32_t Rnd(uint32_t n) { g_r = g_r * 1103515245u + 12345u; return ((g_r >> 8) & 0xFFFFFF) % n; }
using Tri = std::tuple<uint16_t, uint16_t, uint16_t>;
std::map<Tri, int> Bag(const std::vector<uint16_t>& v) {
    std::map<Tri, int> b;
    for (size_t i = 0; i + 2 < v.size(); i += 3) ++b[Tri{v[i], v[i + 1], v[i + 2]}];
    return b;
}
// A w x h grid of vertices, its quads as GX strips running along the rows, cut into strips of 3..8 vertices (welded: the strips
// share the grid's vertex numbers). Strip triangles alternate winding as GX makes them (PrimIndices: odd triangles swapped).
std::vector<uint16_t> GridStrips(uint32_t w, uint32_t h, uint32_t& nVerts) {
    nVerts = w * h;
    std::vector<uint16_t> out;
    for (uint32_t y = 0; y + 1 < h; ++y) {
        std::vector<uint16_t> row; // the full strip of this row: top, bottom, top, bottom...
        for (uint32_t x = 0; x < w; ++x) { row.push_back(static_cast<uint16_t>(y * w + x)); row.push_back(static_cast<uint16_t>((y + 1) * w + x)); }
        for (size_t s = 0; s + 2 < row.size();) {
            size_t len = 3 + Rnd(6);
            if (s + len > row.size()) len = row.size() - s;
            if (len < 3) break;
            for (size_t k = 0; k + 2 < len; ++k) {
                const uint16_t a = row[s + k], b = row[s + k + 1], c = row[s + k + 2];
                if (k & 1) { out.push_back(b); out.push_back(a); out.push_back(c); }
                else { out.push_back(a); out.push_back(b); out.push_back(c); }
            }
            s += len - 2; // (the next strip starts on this one's last edge)
        }
    }
    return out;
}
using Fn = bool (*)(const uint16_t*, uint32_t, uint32_t, uint32_t, uint16_t*, vgx::vcache::Scratch&);
void Run1(const char* name, Fn fn, const char* algo, const std::vector<uint16_t>& in, uint32_t nVerts, bool expectGain) {
    vgx::vcache::Scratch s;
    const uint32_t n = static_cast<uint32_t>(in.size());
    double acmr[2][2] = {};
    for (uint32_t size : {8u, 12u, 16u, 24u, 32u}) {
        std::vector<uint16_t> out(n, 0xFFFF);
        const bool ok = fn(in.data(), n, nVerts, size, out.data(), s);
        Check(ok, name);
        if (!ok) return;
        Check(Bag(out) == Bag(in), name); // (the same triangles, each as it was)
        if (size == 16 || size == 32) {
            const int k = size == 16 ? 0 : 1;
            acmr[k][0] = static_cast<double>(vgx::vcache::FifoMisses(in.data(), n, size)) / (n / 3);
            acmr[k][1] = static_cast<double>(vgx::vcache::FifoMisses(out.data(), n, size)) / (n / 3);
        }
    }
    std::printf("%-8s %-34s %6u triangles %6u vertices: ACMR FIFO16 %.3f -> %.3f, FIFO32 %.3f -> %.3f\n", algo, name, n / 3, nVerts, acmr[0][0],
                acmr[0][1], acmr[1][0], acmr[1][1]);
    if (expectGain) Check(acmr[0][1] < acmr[0][0] * 0.9, name);
}
void Run(const char* name, const std::vector<uint16_t>& in, uint32_t nVerts, bool expectGain) {
    Run1(name, vgx::vcache::Optimize, "forsyth", in, nVerts, expectGain);
    Run1(name, vgx::vcache::Tipsify, "tipsify", in, nVerts, expectGain);
}
} // namespace

int main() {
    // bad input refused
    {
        vgx::vcache::Scratch s;
        uint16_t in[6] = {0, 1, 2, 2, 1, 5}, out[6];
        Check(!vgx::vcache::Optimize(in, 6, 4, 16, out, s), "an index past the vertex count is refused");
        Check(!vgx::vcache::Optimize(in, 5, 8, 16, out, s), "a count not a multiple of 3 is refused");
        Check(!vgx::vcache::Tipsify(in, 6, 4, 16, out, s), "an index past the vertex count is refused (tipsify)");
        Check(!vgx::vcache::Tipsify(in, 5, 8, 16, out, s), "a count not a multiple of 3 is refused (tipsify)");
    }
    uint32_t nv = 0;
    {
        auto g = GridStrips(20, 20, nv);
        Run("grid 20x20, strips of 3..8", g, nv, true);
    }
    {
        auto g = GridStrips(120, 60, nv);
        Run("grid 120x60, strips of 3..8", g, nv, true);
    }
    {
        auto g = GridStrips(400, 3, nv);
        Run("grid 400x3 (long thin)", g, nv, false);
    }
    {
        // a fan of 50 around vertex 0, then a second around 51
        std::vector<uint16_t> f;
        for (uint16_t i = 1; i < 50; ++i) { f.push_back(0); f.push_back(i); f.push_back(static_cast<uint16_t>(i + 1)); }
        for (uint16_t i = 52; i < 90; ++i) { f.push_back(51); f.push_back(i); f.push_back(static_cast<uint16_t>(i + 1)); }
        Run("two fans", f, 91, false);
    }
    {
        // a random soup with repeats and degenerate triangles (an index twice)
        std::vector<uint16_t> r;
        for (int i = 0; i < 3000; ++i) {
            const uint16_t a = static_cast<uint16_t>(Rnd(700));
            r.push_back(a);
            r.push_back(i % 17 == 0 ? a : static_cast<uint16_t>(Rnd(700)));
            r.push_back(static_cast<uint16_t>(Rnd(700)));
        }
        for (int i = 0; i < 300; ++i) { const size_t t = Rnd(3000) * 3; r.push_back(r[t]); r.push_back(r[t + 1]); r.push_back(r[t + 2]); }
        Run("random soup + repeats + degenerates", r, 700, false);
    }
    {
        std::vector<uint16_t> one = {3, 1, 2};
        Run("one triangle", one, 4, false);
    }
    {
        // many random grids
        for (int k = 0; k < 40; ++k) {
            auto g = GridStrips(2 + Rnd(40), 2 + Rnd(40), nv);
            if (g.empty()) continue;
            vgx::vcache::Scratch s;
            std::vector<uint16_t> out(g.size());
            Check(vgx::vcache::Optimize(g.data(), static_cast<uint32_t>(g.size()), nv, 8 + Rnd(25), out.data(), s) && Bag(out) == Bag(g), "random grid");
            Check(vgx::vcache::Tipsify(g.data(), static_cast<uint32_t>(g.size()), nv, 8 + Rnd(25), out.data(), s) && Bag(out) == Bag(g), "random grid (tipsify)");
        }
    }
    std::printf("%s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
