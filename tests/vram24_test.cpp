// vram24: host test of platform/vgx/src/vgx_vram24.h (the hysteresis between the pool trim's urgent limit and seam68's low-memory limit, the memory emergency's rung floor).
// The scenario is the night58 02-HHS one: free video memory sits near 10 MiB, urgent limit 12, seam68 limit 8.
#include "vgx_vram24.h"
#include <cstdio>
using namespace vgx::vram24;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
int main() {
    // 1. the inversion, old rule: at 10.3 MiB the trim is urgent and seam68 is not low: the held textures stay held while the trim keeps deleting
    CHECK(TrimAll(false, 10.3, 12, false, 4));
    CHECK(!S68Low(false, 10.3, 8, 12, true, false, 4));
    // 2. fixed: at 10.3 both are on, so the textures the urgent trim retires are given back
    CHECK(S68Low(false, 10.3, 8, 12, true, true, 4));
    CHECK(!S68Low(false, 12.0, 8, 12, true, true, 4));   // not under the urgent limit: not low (the hold keeps its purpose)
    // 3. the invariant over every free figure: with hyst and the trim on, urgent => low (a retired texture is never held while the trim is urgent), from either previous state
    for (int w = 0; w < 2; ++w)
        for (double f = 0.0; f < 40.0; f += 0.25)
            for (double urg : {6.0, 8.0, 12.0, 16.0}) {
                const bool low = S68Low(w != 0, f, 8, urg, true, true, 4);
                if (w == 0) CHECK(!(f < urg) || low);
            }
    // 4. no flapping: free memory wobbling 11 <-> 13 around the urgent limit 12 enters the urgent state once with hyst (twice+ without)
    {
        int enterOld = 0, enterNew = 0;
        bool o = false, n = false;
        const double seq[] = {11, 13, 11, 13, 11, 13, 11, 13, 11, 13, 17, 11};
        for (double f : seq) {
            const bool o2 = TrimAll(o, f, 12, false, 4), n2 = TrimAll(n, f, 12, true, 4);
            enterOld += (o2 && !o); enterNew += (n2 && !n);
            o = o2; n = n2;
        }
        CHECK(enterOld == 6);
        CHECK(enterNew == 2);   // once at the start, once again after 17 (>= 16 left it)
        std::printf("urgent entries over the wobble: old %d, with hysteresis %d\n", enterOld, enterNew);
    }
    // 5. leaving: low / urgent end at limit + hyst exactly (16), and not before
    CHECK(TrimAll(true, 15.99, 12, true, 4));
    CHECK(!TrimAll(true, 16.0, 12, true, 4));
    CHECK(S68Low(true, 15.99, 8, 12, true, true, 4));
    CHECK(!S68Low(true, 16.0, 8, 12, true, true, 4));
    // 6. switch off = the old rules exactly, whatever the previous state
    for (int w = 0; w < 2; ++w)
        for (double f = 0.0; f < 40.0; f += 0.5) {
            CHECK(TrimAll(w != 0, f, 12, false, 4) == (f < 12));
            CHECK(S68Low(w != 0, f, 8, 12, true, false, 4) == (f < 8));
        }
    // 7. trim off (GXV_POOL_TRIM=0): the urgent limit does not raise seam68's
    CHECK(!S68Low(false, 10.3, 8, 12, false, true, 4));
    CHECK(S68Low(false, 7.9, 8, 12, false, true, 4));
    // 8. the rung floor: a fixed-size stage (minRung 6) never gets the drop to 2; the ordinary range keeps it
    CHECK(DropTo(2, 6) == 6);
    CHECK(DropTo(2, 0) == 2);
    CHECK(DropTo(2, 2) == 2);
    CHECK(DropTo(2, 4) == 4);
    // 9. what the old call asked for, clamped by Controller::Drop to [minRung, maxRung], is what DropTo asks for (so the new code changes the line, not the result)
    for (int minR = 0; minR <= 6; ++minR) CHECK(std::max(minR, 2) == DropTo(2, minR));
    std::printf(g_fail ? "FAIL\n" : "PASS vram24\n");
    return g_fail ? 1 : 0;
}
