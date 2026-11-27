// Host test of vgx_indflat.h (GXV_IND_FLAT): the eligibility of an indirect stage and the memo salt.
#include "vgx_indflat.h"
#include <cstdio>
#include <set>
using namespace vgx::indflat;
static int bad = 0;
#define CHECK(c) do { if (!(c)) { ++bad; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main() {
    // mode 0: never; every possible 21-bit word
    for (uint32_t w = 0; w < (1u << 21); w += 1) {
        CHECK(!Eligible(0, w, true) && !Eligible(0, w, false));
        const bool direct = (w & (kMatrixMask | kBiasMask)) == 0;
        const bool simple = (w & (kWrapMask | kAddPrev | kBumpAlpha)) == 0;
        CHECK(Eligible(2, w, false) == (!direct && simple));
        CHECK(Eligible(1, w, true) == Eligible(2, w, true));
        CHECK(!Eligible(1, w, false));
        if (bad > 20) return 1;
    }
    CHECK(Eligible(1, 1u << 9, true));          // matrix only
    CHECK(Eligible(1, 1u << 4, true));          // bias only
    CHECK(!Eligible(1, (1u << 9) | (1u << 13), true)); // wrap kept
    CHECK(!Eligible(1, (1u << 9) | (1u << 20), true)); // add-previous kept
    CHECK(!Eligible(1, (1u << 9) | (1u << 7), true));  // bump alpha kept
    CHECK(!Eligible(1, 0, true));                      // already direct
    CHECK(Salt(0) == 0 && Salt(-3) == 0);
    std::set<uint64_t> s{Salt(0), Salt(1), Salt(2)};
    CHECK(s.size() == 3 && Salt(7) == Salt(2));
    std::printf(bad ? "indflat: FAILED\n" : "indflat: ok\n");
    return bad ? 1 : 0;
}
