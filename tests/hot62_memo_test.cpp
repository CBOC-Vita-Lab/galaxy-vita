// hot62 host test for port/hot62_memo.h: every memo answer equals the plain computation, bit for bit, under table churn and address reuse.
#include "port/hot62_memo.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace port::hot62;

static uint32_t rng = 12345;
static uint32_t R() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

// ---- ResTable model: the real class's behaviour (array of hash words, count, add bumps the generation)
struct Tab {
    uint32_t* arr = nullptr;
    uint32_t count = 0;
    void make(uint32_t cap) { arr = static_cast<uint32_t*>(std::calloc(cap, 4)); ResBump(); }  // (ctor + newFileInfoTable)
    void add(uint32_t h) { arr[count] = h; ++count; ResBump(); }
};
static int plain(const Tab& t, uint32_t h) {
    for (uint32_t i = 0; i < t.count; ++i) if (t.arr[i] == h) return static_cast<int>(i);
    return -1;
}

// ---- the rail solve, copied operation for operation from BezierRailPart::getParam / getLength (decomp/src/Game/Map/BezierRail.cpp)
struct Part {
    float s[3], c1[3], c2[3], c3[3], len;
    float vlen(float t) const {
        float v5 = t * t, v7 = 2.0f * t, a[3];
        for (int i = 0; i < 3; ++i) { float x = c3[i] * v5, y = c2[i] * v7; a[i] = (c1[i] + y + x) * 3.0f; }
        return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    }
    float getLength(float t1, float t2, int k) const {
        float len1 = 0, len2 = 0, m3 = 0.5f * (vlen(t1) + vlen(t2)), tDiff = t2 - t1, temp = (1.0f / (k * 2.0)), delta = tDiff * temp;
        for (int seg = 1; seg <= k; seg++) {
            int ia = seg * 2 - 1, ib = seg * 2;
            float tA = t1 + delta * ia, tB = t1 + delta * ib;
            len1 += vlen(tA);
            if (seg != k) len2 += vlen(tB);
        }
        float l = std::floor(1024.0f * static_cast<float>((1.0 / 3.0) * delta * ((len1 * 4.0f) + (m3 + (len2 * 2.0f)))));
        return l * (1.0f / 1024.0f);
    }
    static float cl(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
    float getParam(float t) const {
        float param = t / len;
        float rl = getLength(0.0f, param, 10);
        if (std::fabs(t - rl) <= 0.01f) return param;
        float length = rl;
        for (int count = 0; count < 5; count++) {
            float mag = vlen(param);
            param = cl(param + (t - length) / mag);
            length = getLength(0.0f, param, 10);
            if (std::fabs(t - length) <= 0.01f) return param;
        }
        if (length < 0.0f || 1.0f < param) param = cl(param);
        return param;
    }
};
static uint32_t bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

int main() {
    int bad = 0;
    // 1. table index memo
    static ResMemo rm;
    std::vector<Tab> tabs(40);
    for (auto& t : tabs) t.make(300);
    long q = 0;
    for (int it = 0; it < 6000000; ++it) {
        const uint32_t op = it < 3000000 ? R() % 100 : (R() % 100000 < 3 ? 0 : 50);  // phase 2: steady state, rare churn (many hits)
        Tab& t = tabs[R() % tabs.size()];
        if (op < 2) {  // a table's life ends; a new one at the SAME address (the array's memory is reused, contents changed)
            std::memset(t.arr, 0, 300 * 4); t.count = 0; ResBump();
            const uint32_t n = R() % 250;
            for (uint32_t i = 0; i < n; ++i) t.add(R() % 400);
        } else if (op < 4 && t.count < 299) {
            t.add(R() % 400);
        } else {
            const uint32_t h = R() % 420;  // some always missing
            const Tab& ct = t;
            const int a = ResIndex(rm, &ct, ct.arr, ct.count, h, [&](uint32_t i) { return ct.arr[i]; });
            const int b = plain(ct, h);
            ++q;
            if (a != b) { if (bad++ < 5) std::printf("RES MISMATCH %d vs %d\n", a, b); }
        }
    }
    std::printf("res: %ld queries, hits %u misses %u, mismatches %d\n", q, rm.hits, rm.misses, bad);
    // 2. rail memo (the hit rate here is by construction; the point is bit equality)
    static RailMemo rl;
    std::vector<Part> parts(30);
    for (auto& p : parts) {
        auto f = [] { return (static_cast<int>(R() % 2000) - 1000) * 0.37f; };
        for (int i = 0; i < 3; ++i) { p.s[i] = f(); p.c1[i] = f(); p.c2[i] = f(); p.c3[i] = f(); }
        p.len = 0; p.len = p.getLength(0.0f, 1.0f, 10);
    }
    long rq = 0;
    for (int it = 0; it < 400000; ++it) {
        Part& p = parts[R() % parts.size()];
        if (R() % 50 == 0) { p.c2[R() % 3] += 1.0f; p.len = p.getLength(0.0f, 1.0f, 10); }  // a part set() again
        float t = (R() % 4 == 0) ? 0.0f : (R() % 1000) * 0.001f * p.len * ((R() % 9 == 0) ? 1.3f : 1.0f);
        if (R() % 3 == 0) t = std::floor(t);  // repeats
        uint32_t w[13]; std::memcpy(w, &p, 52);
        const uint32_t a = RailParam(rl, w, bits(t), [&] { return bits(p.getParam(t)); });
        const uint32_t b = bits(p.getParam(t));
        ++rq;
        if (a != b) { if (bad++ < 10) std::printf("RAIL MISMATCH t=%g %08x vs %08x\n", t, a, b); }
    }
    std::printf("rail: %ld queries, hits %u misses %u, total mismatches %d\n", rq, rl.hits, rl.misses, bad);
    std::puts(bad == 0 ? "PASS" : "FAIL");
    return bad != 0;
}
