// GXV_DL_KEEP_MIB's choice of entries to let go (platform/vgx/src/vgx_dlkeep.h).
#include <cstdio>
#include <vector>
#include "vgx_dlkeep.h"
using vgx::dlkeep::Due;
using vgx::dlkeep::EvictCount;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
    std::vector<Due> d;
    uint64_t sum = 0;
    for (int i = 0; i < 100; ++i) { d.push_back(Due{static_cast<uint64_t>(1000 + i), 100000}); sum += 100000; } // 10,000,000 bytes, 100 entries
    CHECK(EvictCount(d.data(), d.size(), sum, 0, 24) == 0);                  // rule off
    CHECK(EvictCount(d.data(), d.size(), sum, sum, 24) == 0);                // fits exactly
    CHECK(EvictCount(d.data(), d.size(), sum, sum + 1, 24) == 0);            // fits
    CHECK(EvictCount(d.data(), d.size(), sum, sum - 1, 24) == 1);            // one byte over: one entry
    CHECK(EvictCount(d.data(), d.size(), sum, sum - 250000, 24) == 3);       // 250,000 over: three entries of 100,000
    CHECK(EvictCount(d.data(), d.size(), sum, 1000, 24) == 24);              // far over: at most 24 a sweep
    CHECK(EvictCount(d.data(), d.size(), sum, 1000, 1000) == 99);            // never all
    CHECK(EvictCount(d.data(), 1, 100000, 10, 24) == 0);                     // a single due entry is never the one to go
    CHECK(EvictCount(d.data(), 0, 0, 10, 24) == 0);
    // successive sweeps converge: each takes 24 until the rest fits
    size_t left = d.size(); uint64_t bytes = sum; int sweeps = 0;
    while (left > 0 && sweeps < 20) {
        size_t g = EvictCount(d.data(), left, bytes, 3000000, 24);
        if (!g) break;
        bytes -= g * 100000; left -= g; d.erase(d.begin(), d.begin() + static_cast<long>(g)); ++sweeps;
    }
    CHECK(bytes <= 3000000 && sweeps == 3);
    std::printf("vgx_dlkeep: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
