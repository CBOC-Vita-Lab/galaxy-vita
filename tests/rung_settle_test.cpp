// Host test of the post-load settle gate for the picture-size change's skip-resample evidence (platform/vgx/src/vgx_rung_settle.h; GXV_RUNG_SKIPRS_SETTLE).
#include "vgx_rung_settle.h"
#include <cstdio>
#include <cstdlib>
using namespace vgx::rungsettle;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
int main() {
    Gate g;
    // nothing happened: allowed, no clear
    CHECK(!Note(g, 0, 100, 300));
    CHECK(Allowed(g, 100));
    // a scene is destroyed: evidence cleared once, refused for 300 presents
    CHECK(Note(g, 1, 1000, 300));
    CHECK(!Note(g, 1, 1001, 300));  // the same counter again: no second clear
    CHECK(!Allowed(g, 1001));
    CHECK(!Allowed(g, 1299));
    CHECK(Allowed(g, 1300));
    CHECK(g.clears == 1 && g.refusals == 2);
    // two loads quickly: the window restarts from the later one
    CHECK(Note(g, 3, 2000, 300));
    CHECK(!Allowed(g, 2299));
    CHECK(Allowed(g, 2300));
    // the switch off (settle 0): never refuses, never clears
    Gate off;
    CHECK(!Note(off, 5, 10, 0));
    CHECK(Allowed(off, 10));
    // counter seen before the window length changes is not replayed
    Gate h;
    CHECK(Note(h, 1, 0, 10));
    CHECK(Allowed(h, 10));
    CHECK(!Note(h, 1, 11, 10));
    std::printf("rung settle gate: ok\n");
    return 0;
}
