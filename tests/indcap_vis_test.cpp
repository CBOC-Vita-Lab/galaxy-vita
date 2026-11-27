// GXV_INDCAP_VIS (vgx_indcap_vis.h): the pure decision. Host g++ only.
#include "../platform/vgx/src/vgx_indcap_vis.h"
#include <cstdio>
using namespace vgx::indcapvis;
static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("FAIL: %s\n", m); ++failures; } } while (0)
static Reader Vis() { Reader r; r.exX = 0; r.exY = 0; r.exW = 640; r.exH = 528; r.scX = 0; r.scY = 0; r.scW = 640; r.scH = 528; return r; }
static Reader EmptySc() { Reader r = Vis(); r.scW = 0; return r; }
static Reader EmptyEx() { Reader r = Vis(); r.exH = 0; return r; }
static Reader Outside() { Reader r = Vis(); r.exX = 700; r.exW = 50; return r; }
static Facts F() { Facts f; f.efbW = 640; f.efbH = 528; return f; }
int main() {
    { Reader a[] = {Vis()}; CHECK(Decide(a, 1, F()) == V_VISIBLE, "a visible reader keeps the capture"); }
    { Reader a[] = {EmptySc()}; CHECK(Decide(a, 1, F()) == V_DRY, "empty scissor: dry"); }
    { Reader a[] = {EmptyEx()}; CHECK(Decide(a, 1, F()) == V_DRY, "empty extent: dry"); }
    { Reader a[] = {Outside()}; CHECK(Decide(a, 1, F()) == V_DRY, "wholly outside the EFB: dry"); Facts f = F(); f.overscan = true; CHECK(Decide(a, 1, f) == V_VISIBLE, "outside with overscan: kept (margin)"); }
    { Reader a[] = {EmptySc(), Vis()}; CHECK(Decide(a, 2, F()) == V_VISIBLE, "one visible among invisible: kept"); }
    { Reader a[] = {EmptySc(), Outside(), EmptyEx()}; CHECK(Decide(a, 3, F()) == V_DRY, "all invisible: dry"); }
    { Reader t = EmptySc(); t.atlas = true; Reader a[] = {t}; CHECK(Decide(a, 1, F()) == V_ATLAS, "an atlas copy reader is visible"); }
    { Reader t = EmptySc(); t.occ = true; Reader a[] = {t}; CHECK(Decide(a, 1, F()) == V_OCC, "an occlusion-query reader is visible"); }
    { Reader a[] = {Reader()}; Reader z = Reader(); a[0] = z; Facts f = F(); f.efbW = 0; CHECK(Decide(a, 1, F()) == V_DRY, "all-zero box = empty"); (void)f; }
    { Reader a[] = {Outside()}; Facts f = F(); f.efbW = 0; CHECK(Decide(a, 1, f) == V_VISIBLE, "unknown EFB size: outside is not trusted"); }
    { Reader a[] = {Reader{}}; a[0].exW = 10; a[0].exH = 10; a[0].scW = 10; a[0].scH = 10; a[0].exX = -20; CHECK(Decide(a, 1, F()) == V_DRY, "left of the EFB: dry"); a[0].exX = -5; a[0].scX = -5; CHECK(Decide(a, 1, F()) == V_VISIBLE, "straddling the edge: kept"); }
    { Reader a[] = {EmptySc()}; Facts f;
      f = F(); f.held = true; CHECK(Decide(a, 1, f) == V_HELD, "held (previous-picture readers / stage / warm-up): kept");
      f = F(); f.pairing = true; CHECK(Decide(a, 1, f) == V_PAIR, "HRW pairing: kept");
      f = F(); f.cut = true; CHECK(Decide(a, 1, f) == V_CUT, "camera cut: kept");
      f = F(); f.shape = true; CHECK(Decide(a, 1, f) == V_SHAPE, "clear / class capture: kept");
      f = F(); f.prevcap = true; CHECK(Decide(a, 1, f) == V_PREVCAP, "PREVCAP on: kept"); }
    { Facts f = F(); CHECK(Decide(nullptr, 0, f) == V_VISIBLE, "no readers is the old rule's, not this one"); }
    { Reader a[] = {Vis()}; Facts f = F(); f.held = true; CHECK(Decide(a, 1, f) == V_VISIBLE, "visible beats held in the reason"); }
    // every verdict has a name
    for (int v = 0; v < V_COUNT; ++v) CHECK(Name(v)[0] != '?', "name");
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("PASS: indcap_vis_test\n");
    return 0;
}
