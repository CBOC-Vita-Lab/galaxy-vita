#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "vgx_maskfull.h"  // (the plan fixture uses maskfull::LiveAlphaStep, as the back end does inside namespace vgx)
using namespace vgx;

#ifdef C184_DEPTH_BENCH
template <typename T> using Scratch = std::vector<T>;
#else
static uint64_t initializedBytes;
template <typename T> struct Scratch : std::vector<T> {
    void assign(size_t n, T value) {
        initializedBytes += n * sizeof(T);
        std::vector<T>::assign(n, value);
    }
    void resize(size_t n) {
        if (n > this->size()) initializedBytes += (n - this->size()) * sizeof(T);
        std::vector<T>::resize(n);
    }
};
#endif
static std::vector<uint8_t> g_ev;
static Scratch<uint8_t> g_live, g_liveC, g_liveA;
static Scratch<uint32_t> g_nextLeave, g_nextRead, g_nextWrite, g_nextFull;

#include "depth_plan_fixture.inc"
#ifdef C184_DEPTH_BENCH
#include "depth_plan_baseline.inc"
#endif

#ifndef C184_DEPTH_BENCH
struct Reference {
    std::vector<uint8_t> live, color, alpha;
    std::vector<uint32_t> leave, read, write, full;
    __attribute__((noinline)) void Build(bool tail) {
        const size_t n = g_ev.size() - 1;
        live.assign(n + 1, 0);
        leave.assign(n + 1, n); read.assign(n + 1, n); write.assign(n + 1, n); full.assign(n + 1, n);
        color.assign(n + 1, 1); alpha.assign(n + 1, 1);
        live[n] = tail ? 1 : 0;
        for (size_t p = n; p-- > 0;) {
            const uint8_t e = g_ev[p];
            live[p] = e & EV_READ ? 1 : e & EV_FULL ? 0 : live[p + 1];
            leave[p] = (g_ev[p + 1] & EV_LEAVE) || p + 1 == n ? p + 1 : leave[p + 1];
            read[p] = e & EV_READ ? p : read[p + 1];
            write[p] = e & EV_WRITE ? p : write[p + 1];
            full[p] = e & EV_FULL ? p : full[p + 1];
            color[p] = e & EV_RC ? 1 : e & EV_FC ? 0 : color[p + 1];
            alpha[p] = e & EV_RA ? 1 : e & EV_FA ? 0 : alpha[p + 1];
        }
    }
    bool Equal() const {
        return live == g_live && color == g_liveC && alpha == g_liveA && leave == g_nextLeave && read == g_nextRead && write == g_nextWrite && full == g_nextFull;
    }
};
#endif
static unsigned failures;
static void Check(bool good, const char* label) {
    if (!good) { ++failures; std::printf("FAIL: %s\n", label); }
}
static uint32_t Random(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
#ifndef C184_DEPTH_BENCH
static void Poison() {
    for (auto* p : {&g_live, &g_liveC, &g_liveA}) std::fill(p->begin(), p->end(), 0xCD);
    for (auto* p : {&g_nextLeave, &g_nextRead, &g_nextWrite, &g_nextFull}) std::fill(p->begin(), p->end(), 0xCDCDCDCDu);
}
static void Equivalence() {
    Reference ref;
    g_ev.assign(1, 0);
    for (bool tail : {false, true}) { Build(tail); ref.Build(tail); Check(ref.Equal(), "empty plan endpoints"); }
    g_ev.assign(3, 0);
    for (unsigned a = 0; a <= 255; ++a) for (unsigned b = 0; b <= 255; ++b) {
        g_ev[0] = a; g_ev[1] = b;
        for (bool tail : {false, true}) {
            Poison(); Build(tail); ref.Build(tail);
            if (!ref.Equal()) { Check(false, "all two-event combinations, tail retained/dropped"); return; }
        }
    }
    uint32_t rng = 0xC184D00Du;
    for (unsigned frame = 0; frame < 3000; ++frame) {
        g_ev.resize(Random(rng) % 8193 + 1);
        for (auto& e : g_ev) e = Random(rng);
        g_ev.back() = 0;
        const bool tail = Random(rng) & 1;
        Poison(); Build(tail); ref.Build(tail);
        if (!ref.Equal()) { Check(false, "growing/shrinking poisoned plans preserve every derived element"); return; }
    }
    std::puts("PASS: 131072 exhaustive two-event/tail cases and 3000 variable-size poisoned plans match");
}
#else
static void Benchmark() {
    uint32_t rng = 0x184FACEu;
    for (unsigned commands : {120u, 600u, 1200u, 2400u}) {
        g_ev.resize(2 * commands + 1);
        for (auto& e : g_ev) e = (Random(rng) & 7) ? 0 : Random(rng);
        g_ev.back() = 0;
        Build(true); frozen::Build(true);
        std::vector<double> before, after;
        for (unsigned pass = 0; pass < 14; ++pass) {
            const auto run = [&](bool candidate) {
                const auto start = std::chrono::steady_clock::now();
                for (unsigned i = 0; i < 600; ++i) {
                    if (candidate) Build(i & 1); else frozen::Build(i & 1);
                }
                return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 600;
            };
            double a, b;
            if (pass & 1) { b = run(true); a = run(false); } else { a = run(false); b = run(true); }
            if (pass >= 2) { before.push_back(a); after.push_back(b); }
        }
        std::sort(before.begin(), before.end()); std::sort(after.begin(), after.end());
        Check(frozen::g_live == g_live && frozen::g_liveC == g_liveC && frozen::g_liveA == g_liveA && frozen::g_nextLeave == g_nextLeave && frozen::g_nextRead == g_nextRead && frozen::g_nextWrite == g_nextWrite && frozen::g_nextFull == g_nextFull, "benchmark outputs equal");
        std::printf("HOST ONLY: %u commands: %.3f -> %.3f us/build (%.3fx), %u redundant initialization bytes removed on a same-size frame\n", commands, before[6], after[6], before[6] / after[6], commands * 2 * 19);
    }
}
#endif
int main(int argc, char**) {
#ifdef C184_DEPTH_BENCH
    (void)argc;
    Benchmark();
#else
    (void)argc;
    Equivalence();
    g_ev.assign(1025, 0);
    Build(true);
    initializedBytes = 0;
    Build(true);
    std::printf("steady-size derived-vector initialization: %llu bytes (baseline 19475)\n", (unsigned long long)initializedBytes);
    Check(initializedBytes == 0, "derived interior values are not redundantly initialized before overwrite");
#endif
    return failures ? 1 : 0;
}
