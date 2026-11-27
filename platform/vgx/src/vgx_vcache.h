// GXV_DLVCACHE (ta23): triangle order for the GPU's post-transform vertex cache (Tipsify, Sander et al. 2007, the default; Tom
// Forsyth's "Linear-Speed Vertex Cache Optimisation", 2006, =2), for the welded static buffers of the display-list cache (vgx_dlcache.cpp Promote, GXV_DLWELD).
//
// The GX lists are triangle strips and fans in the order the exporter wrote them (for the GameCube, which has no post-transform
// cache); PrimIndices turns each into its own triangles, and with GXV_DLWELD the strips of a run share their vertices. The SGX's
// vertex stage shades an index again once it has left its small FIFO cache of transformed vertices, so the order of the triangles
// decides how many times the vertex shader runs (and how many outputs the TA writes): this reorders the triangles of a run so that
// they reuse what is in the cache. Every triangle is kept as it is (its three indices in their order: the same winding, the same
// first vertex), only the order of the triangles changes; the result is used only for draws whose picture cannot depend on that
// order except at depth ties (vgx_draw.cpp EmitStaticRun: no blending, depth test with a less/greater compare, depth writes).
//
// Pure functions on 16-bit indices, no state: host-tested by tests/vgx_vcache.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vgx {
namespace vcache {

// The vertex shader runs a FIFO cache of `size` entries would cause for the triangle list idx[0..n) (indices of any value).
inline uint32_t FifoMisses(const uint16_t* idx, uint32_t n, uint32_t size) {
    if (size == 0 || size > 64) size = 16;
    int32_t fifo[64];
    for (uint32_t i = 0; i < size; ++i) fifo[i] = -1;
    uint32_t head = 0, misses = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const int32_t v = idx[i];
        bool hit = false;
        for (uint32_t k = 0; k < size && !hit; ++k) hit = fifo[k] == v;
        if (hit) continue;
        ++misses;
        fifo[head] = v;
        head = head + 1 == size ? 0 : head + 1;
    }
    return misses;
}

// Scratch kept between calls (the front end's thread only).
struct Scratch {
    std::vector<uint32_t> adjStart, adjList, adjFill;
    std::vector<uint32_t> remaining;
    std::vector<int8_t> cachePos;
    std::vector<float> vScore, tScore;
    std::vector<uint8_t> done;
};

// Reorders the triangles of in[0..n) (n a multiple of 3; every index < nVerts) into out[0..n): Forsyth's scoring with a modelled
// cache of `size` entries (8..32). Each output triangle is an input triangle with its indices in the same order; every input
// triangle appears exactly once. False (out untouched) for bad input.
inline bool Optimize(const uint16_t* in, uint32_t n, uint32_t nVerts, uint32_t size, uint16_t* out, Scratch& s) {
    if (n % 3 || nVerts == 0 || nVerts > 0x10000) return false;
    if (size < 8) size = 8;
    if (size > 32) size = 32;
    const uint32_t nTri = n / 3;
    for (uint32_t i = 0; i < n; ++i)
        if (in[i] >= nVerts) return false;
    // Scores (Forsyth's constants): the three most recent vertices 0.75, then a power-1.5 fall over the rest of the cache; plus a
    // bonus for vertices with few triangles left, 2 / sqrt(remaining).
    constexpr uint32_t kMaxSize = 32, kValMax = 64;
    float cacheScore[kMaxSize + 3], valScore[kValMax + 1];
    for (uint32_t p = 0; p < size; ++p)
        cacheScore[p] = p < 3 ? 0.75f : std::pow(1.0f - static_cast<float>(p - 3) / static_cast<float>(size - 3), 1.5f);
    valScore[0] = 0.0f;
    for (uint32_t r = 1; r <= kValMax; ++r) valScore[r] = 2.0f / std::sqrt(static_cast<float>(r));
    s.adjStart.assign(nVerts + 1, 0);
    s.remaining.assign(nVerts, 0);
    for (uint32_t i = 0; i < n; ++i) {
        ++s.adjStart[in[i] + 1];
        ++s.remaining[in[i]];
    }
    for (uint32_t v = 0; v < nVerts; ++v) s.adjStart[v + 1] += s.adjStart[v];
    s.adjList.resize(n);
    s.adjFill.assign(s.adjStart.begin(), s.adjStart.end() - 1);
    for (uint32_t t = 0; t < nTri; ++t)
        for (uint32_t k = 0; k < 3; ++k) s.adjList[s.adjFill[in[t * 3 + k]]++] = t;
    s.cachePos.assign(nVerts, -1);
    s.vScore.resize(nVerts);
    auto vertexScore = [&](uint32_t v) -> float {
        const uint32_t r = s.remaining[v];
        if (r == 0) return -1.0f;
        const int p = s.cachePos[v];
        return (p >= 0 ? cacheScore[p] : 0.0f) + valScore[r < kValMax ? r : kValMax];
    };
    for (uint32_t v = 0; v < nVerts; ++v) s.vScore[v] = vertexScore(v);
    s.tScore.resize(nTri);
    for (uint32_t t = 0; t < nTri; ++t) s.tScore[t] = s.vScore[in[t * 3]] + s.vScore[in[t * 3 + 1]] + s.vScore[in[t * 3 + 2]];
    s.done.assign(nTri, 0);
    uint32_t cache[kMaxSize + 3], cacheN = 0, next[kMaxSize + 3];
    uint32_t cursor = 0, emitted = 0;
    int64_t best = -1;
    while (emitted < nTri) {
        if (best < 0) { // (nothing in the cache to go on with: the first triangle not yet emitted, in the input's order)
            while (s.done[cursor]) ++cursor;
            best = cursor;
        }
        const uint32_t t = static_cast<uint32_t>(best);
        s.done[t] = 1;
        std::memcpy(out + emitted * 3, in + t * 3, 3 * sizeof(uint16_t));
        ++emitted;
        // the triangle's vertices: one triangle fewer left; to the front of the cache
        uint32_t nn = 0;
        for (uint32_t k = 0; k < 3; ++k) {
            const uint32_t v = in[t * 3 + k];
            --s.remaining[v];
            // (take the triangle out of the vertex's list, so that the scans below see only open triangles)
            uint32_t* a = s.adjList.data() + s.adjStart[v];
            const uint32_t m = s.remaining[v] + 1u;
            for (uint32_t i = 0; i < m; ++i)
                if (a[i] == t) { a[i] = a[m - 1]; break; }
            bool dup = false;
            for (uint32_t i = 0; i < nn; ++i) dup = dup || next[i] == v;
            if (!dup) next[nn++] = v;
        }
        for (uint32_t i = 0; i < cacheN; ++i) {
            const uint32_t v = cache[i];
            bool in3 = false;
            for (uint32_t k = 0; k < nn; ++k) in3 = in3 || next[k] == v;
            if (!in3) next[nn++] = v;
        }
        // positions (beyond `size`: out of the cache, scored again as such)
        for (uint32_t i = 0; i < nn; ++i) s.cachePos[next[i]] = i < size ? static_cast<int8_t>(i) : static_cast<int8_t>(-1);
        for (uint32_t i = 0; i < nn; ++i) s.vScore[next[i]] = vertexScore(next[i]);
        // the open triangles of the cached vertices: their scores again, and the best of them
        best = -1;
        float bestScore = -1.0f;
        for (uint32_t i = 0; i < nn; ++i) {
            const uint32_t v = next[i];
            const uint32_t* a = s.adjList.data() + s.adjStart[v];
            for (uint32_t j = 0; j < s.remaining[v]; ++j) {
                const uint32_t u = a[j];
                const float sc = s.vScore[in[u * 3]] + s.vScore[in[u * 3 + 1]] + s.vScore[in[u * 3 + 2]];
                s.tScore[u] = sc;
                if (sc > bestScore) { bestScore = sc; best = u; }
            }
        }
        cacheN = nn < size ? nn : size;
        std::memcpy(cache, next, cacheN * sizeof(uint32_t));
    }
    return true;
}


// Tipsify (Sander, Nehab, Barczak, "Fast Triangle Reordering for Vertex Locality and Reduced Overdraw", 2007): the same contract as
// Optimize, a fraction of its cost (every triangle is visited once; the next fanning vertex comes from the few just emitted): about
// 0.05 us a triangle on a desktop core against Forsyth's 0.5, with a cache-miss ratio within a few per cent of it on strip meshes.
inline bool Tipsify(const uint16_t* in, uint32_t n, uint32_t nVerts, uint32_t size, uint16_t* out, Scratch& s) {
    if (n % 3 || nVerts == 0 || nVerts > 0x10000) return false;
    if (size < 4) size = 4;
    const uint32_t nTri = n / 3;
    for (uint32_t i = 0; i < n; ++i)
        if (in[i] >= nVerts) return false;
    s.adjStart.assign(nVerts + 1, 0);
    s.remaining.assign(nVerts, 0);
    for (uint32_t i = 0; i < n; ++i) { ++s.adjStart[in[i] + 1]; ++s.remaining[in[i]]; }
    for (uint32_t v = 0; v < nVerts; ++v) s.adjStart[v + 1] += s.adjStart[v];
    s.adjList.resize(n);
    s.adjFill.assign(s.adjStart.begin(), s.adjStart.end() - 1);
    for (uint32_t t = 0; t < nTri; ++t)
        for (uint32_t k = 0; k < 3; ++k) s.adjList[s.adjFill[in[t * 3 + k]]++] = t;
    s.done.assign(nTri, 0);
    // (adjFill is reused as the time stamp of each vertex's last entry into the modelled cache; cachePos is not needed)
    std::vector<uint32_t>& stamp = s.adjFill;
    std::fill(stamp.begin(), stamp.end(), 0u);
    std::vector<uint32_t> dead;   // (the dead-end stack: vertices of emitted triangles, latest last)
    std::vector<uint32_t> cand;   // (the vertices of the triangles of the current fan)
    dead.reserve(n);
    uint32_t time = size + 1, cursor = 0, emitted = 0;
    int64_t f = 0;
    while (f >= 0 && emitted < nTri) {
        cand.clear();
        const uint32_t fv = static_cast<uint32_t>(f);
        for (uint32_t a = s.adjStart[fv]; a < s.adjStart[fv + 1]; ++a) {
            const uint32_t t = s.adjList[a];
            if (s.done[t]) continue;
            s.done[t] = 1;
            std::memcpy(out + emitted * 3, in + t * 3, 3 * sizeof(uint16_t));
            ++emitted;
            for (uint32_t k = 0; k < 3; ++k) {
                const uint32_t v = in[t * 3 + k];
                dead.push_back(v);
                cand.push_back(v);
                --s.remaining[v];
                if (time - stamp[v] > size) { stamp[v] = time; ++time; }
            }
        }
        // the next fanning vertex: among the candidates with triangles left, the one that will still be in the cache after its fan
        // and has been in it longest; else a dead end from the stack; else the next vertex in input order with triangles left
        int64_t best = -1;
        int64_t bestP = -1;
        for (uint32_t v : cand) {
            if (!s.remaining[v]) continue;
            int64_t p = 0;
            if (static_cast<int64_t>(time) - stamp[v] + 2 * static_cast<int64_t>(s.remaining[v]) <= static_cast<int64_t>(size)) p = time - stamp[v];
            if (p > bestP) { bestP = p; best = v; }
        }
        if (best < 0) {
            while (!dead.empty()) {
                const uint32_t d = dead.back();
                dead.pop_back();
                if (s.remaining[d]) { best = d; break; }
            }
        }
        if (best < 0) {
            while (cursor < nVerts && !s.remaining[cursor]) ++cursor;
            best = cursor < nVerts ? static_cast<int64_t>(cursor) : -1;
        }
        f = best;
    }
    return emitted == nTri;
}

} // namespace vcache
} // namespace vgx
