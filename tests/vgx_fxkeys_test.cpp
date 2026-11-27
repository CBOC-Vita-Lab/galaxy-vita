// Host test of vgx_fxkeys.h (the stage-settable shader-key list of GXV_FXLOWRES=1).
#include "vgx_fxkeys.h"
#include <cstdio>
using namespace vgx::fxkeys;
static int bad = 0;
#define CHECK(c) do { if (!(c)) { ++bad; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main() {
    const uint32_t keys[4] = {0xd97a8bu, 0x443366u, 0, 0};
    CHECK(Listed(keys, 0xf9d97a8bu));   // fs 1835's fh: only the low 24 bits count
    CHECK(Listed(keys, 0x97443366u));   // fs 1836
    CHECK(!Listed(keys, 0x6a2763acu));  // fs 1654: not listed
    CHECK(!Listed(keys, 0x00000000u));  // key 0 never matches the unused slots
    CHECK(!Listed(keys, 0xAB000000u));  // low 24 bits zero: never listed
    CHECK(Any(keys));
    const uint32_t none[4] = {0, 0, 0, 0};
    CHECK(!Any(none) && !Listed(none, 0xf9d97a8bu));
    CHECK(Clean(0) == 0 && Clean(-5) == 0 && Clean(1) == 1 && Clean(16777215) == 16777215u && Clean(16777216) == 0);
    // a stage value is a float: every 24-bit key is exact
    for (uint32_t k = 1; k <= kMask; k += 997) CHECK(static_cast<uint32_t>(static_cast<float>(k)) == k);
    std::printf(bad ? "fxkeys: FAILED\n" : "fxkeys: ok\n");
    return bad ? 1 : 0;
}
