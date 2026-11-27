// far30 host test: platform/include/port/distcensus.h (bands, classes, screen size, the display-list index count, the per-turn fold, the line format).
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "port/distcensus.h"
using namespace port::distcensus;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void TestBands() {
    CHECK(BandOf(0) == 0 && BandOf(1000.0f * 1000.0f) == 0, "10 m edge belongs to band 0");
    CHECK(BandOf(1000.1f * 1000.1f) == 1, "just over 10 m");
    CHECK(BandOf(2500.0f * 2500.0f) == 1 && BandOf(2500.5f * 2500.5f) == 2, "25 m");
    CHECK(BandOf(5000.0f * 5000.0f) == 2 && BandOf(10000.0f * 10000.0f) == 3 && BandOf(20000.0f * 20000.0f) == 4, "50, 100, 200 m");
    CHECK(BandOf(20001.0f * 20001.0f) == 5 && BandOf(1e12f) == 5, "200+");
    CHECK(BandLoM(0) == 0 && BandLoM(1) == 10 && BandLoM(2) == 25 && BandLoM(5) == 200, "band lows");
}
static void TestClasses() {
    const uint32_t enemy[] = {0x15}, coin[] = {0x4A}, mo[] = {0x46}, mix[] = {0x46, 0x4A}, mix2[] = {0x4A, 0x15}, none[] = {0xFFFF};
    CHECK(ClassOfSensorTypes(enemy, 1) == kChar, "enemy");
    CHECK(ClassOfSensorTypes(coin, 1) == kItem, "coin");
    CHECK(ClassOfSensorTypes(mo, 1) == kMap, "map object");
    CHECK(ClassOfSensorTypes(mix, 2) == kItem, "item beats map object");
    CHECK(ClassOfSensorTypes(mix2, 2) == kChar, "character beats item");
    CHECK(ClassOfSensorTypes(none, 1) == kOther && ClassOfSensorTypes(none, 0) == kOther, "unknown / none");
    CHECK(ClassOfSensorType(0x01) == kChar && ClassOfSensorType(0x05) == kChar && ClassOfSensorType(0x44) == kChar && ClassOfSensorType(0x45) == kMap, "ranges");
    CHECK(ClassOfSensorType(0x4C) == kItem && ClassOfSensorType(0x50) == kItem && ClassOfSensorType(0x51) == kMap, "item range");
    CHECK(ClassOfKind(1, kItem, false) == kMap && ClassOfKind(2, kItem, false) == kItem && ClassOfKind(0, kItem, false) == kPlant, "kinds");
    CHECK(ClassOfKind(3, kOther, false) == kPlant && ClassOfKind(4, kOther, false) == kPlant && ClassOfKind(5, kOther, false) == kOther, "decor, nomodel");
    CHECK(ClassOfKind(1, kOther, true) == kChar, "the player is a character whatever his parts say");
}
static void TestSize() {
    CHECK(SizeOf(0.0f) == 0 && SizeOf(7.99f) == 0 && SizeOf(8.0f) == 1 && SizeOf(31.9f) == 1 && SizeOf(32.0f) == 2 && SizeOf(127.0f) == 2 && SizeOf(128.0f) == 3, "edges");
    // radius 100 units at 2000 units, tan(fovy/2)=0.5: 100 * 272 / (2000*0.5) = 27.2 px
    CHECK(std::fabs(RadiusPx(100.0f, 2000.0f, 0.5f) - 27.2f) < 1e-3f, "projection %f", RadiusPx(100.0f, 2000.0f, 0.5f));
    CHECK(SizeOf(RadiusPx(500.0f, 400.0f, 0.5f)) == 3, "the eye inside the sphere is big");
    CHECK(SizeOf(RadiusPx(10.0f, 5000.0f, 0.5f)) == 0, "a speck is small");
    CHECK(SizeOf(RadiusPx(10.0f, 5000.0f, 0.0f)) == 3, "no fov: big (never cut)");
}
static void PutPrim(std::vector<uint8_t>& dl, uint8_t cmd, uint16_t n, uint32_t stride) {
    dl.push_back(cmd);
    dl.push_back(static_cast<uint8_t>(n >> 8));
    dl.push_back(static_cast<uint8_t>(n & 255));
    for (uint32_t i = 0; i < stride * n; ++i) dl.push_back(0x55);
}
static void TestDl() {
    std::vector<uint8_t> dl;
    PutPrim(dl, 0x98, 4, 5);   // strip of 4 vertices: 2 triangles = 6
    PutPrim(dl, 0xA0, 6, 5);   // fan of 6: 4 triangles = 12
    PutPrim(dl, 0x98, 3, 5);   // one triangle = 3
    PutPrim(dl, 0x98, 2, 5);   // degenerate: none
    dl.push_back(0x00);        // padding stops it
    dl.push_back(0x98); dl.push_back(0); dl.push_back(9);
    uint32_t v = 0;
    CHECK(DlIndices(dl.data(), static_cast<uint32_t>(dl.size()), 5, &v) == 21, "indices %u", DlIndices(dl.data(), static_cast<uint32_t>(dl.size()), 5, &v));
    CHECK(v == 15, "vertices %u", v);
    CHECK(DlIndices(dl.data(), 3 + 20 + 2, 5, &v) == 6, "a list cut inside the second primitive: the first only");
    std::vector<uint8_t> trunc(dl.begin(), dl.begin() + 3 + 4 * 5 + 3 + 6 * 5 - 1);
    CHECK(DlIndices(trunc.data(), static_cast<uint32_t>(trunc.size()), 5, &v) == 6 && v == 4, "a truncated primitive is not counted");
    CHECK(DlIndices(dl.data(), 0, 5, &v) == 0 && v == 0, "empty");
    CHECK(DlIndices(nullptr, 0, 0, nullptr) == 0, "null");
}
static void TestFold() {
    Table win, turn;
    turn.Logic(0, kChar); turn.Logic(0, kChar); turn.Posed(1, kPlant); turn.Particles(2, 40); turn.Shadow(0);
    turn.Draw(0, kChar, 3, 3, 900); turn.Draw(2, kPlant, 0, 1, 30);
    win.AddTurn(turn, false);   // an undrawn turn: logic yes, draws no
    CHECK(win.turns == 1 && win.pictures == 0, "turns");
    CHECK(win.cell[0][kChar].logic == 2 && win.cell[1][kPlant].posed == 1 && win.cell[2][kPtcl].particles == 40 && win.cell[0][kShadow].logic == 1, "logic kept");
    CHECK(win.cell[0][kChar].draws == 0 && win.size[0][3].draws == 0 && win.cell[2][kPlant].indices == 0, "draws dropped on an undrawn turn");
    win.AddTurn(turn, true);
    CHECK(win.pictures == 1 && win.cell[0][kChar].draws == 3 && win.cell[0][kChar].indices == 900 && win.size[0][3].draws == 3 && win.size[2][0].indices == 30, "draws kept on a drawn one");
    CHECK(win.cell[0][kChar].logic == 4, "logic doubled");
    Table st; st.AddTable(win); st.AddTable(win);
    CHECK(st.turns == 4 && st.pictures == 2 && st.cell[0][kChar].draws == 6, "window into stage");
    const Cell s = st.BandSum(0);
    CHECK(s.logic == 12 && s.draws == 6 && s.indices == 1800, "band sum %llu %llu", (unsigned long long)s.logic, (unsigned long long)s.draws);
    // the sums over the bands equal the table's total (nothing lost between cells)
    uint64_t d = 0; for (int b = 0; b < kBands; ++b) d += st.BandSum(b).draws;
    CHECK(d == 8, "total draws %llu", (unsigned long long)d);
    uint64_t z = 0; for (int b = 0; b < kBands; ++b) for (int k = 0; k < kSizes; ++k) z += st.size[b][k].draws;
    CHECK(z == d, "the by-size draws add up to the by-class draws (%llu vs %llu)", (unsigned long long)z, (unsigned long long)d);
}
static void TestModelIdx() {
    ModelIdx* m = new ModelIdx();
    uint32_t a = 0, b = 0;
    CHECK(!m->Find(0x1000, &a, &b), "empty");
    m->Put(0x1000, 3, 777); m->Put(0x2000, 5, 99);
    CHECK(m->Find(0x1000, &a, &b) && a == 3 && b == 777 && m->Find(0x2000, &a, &b) && a == 5 && b == 99, "found");
    m->Put(0x1000, 4, 1);
    CHECK(m->Find(0x1000, &a, &b) && a == 4 && b == 1 && m->used == 2, "replace");
    for (uintptr_t k = 1; k < 5000; ++k) m->Put(k << 4, 1, 1);
    CHECK(m->used <= ModelIdx::kCap - 8, "never fills up");
    delete m;
}
static void TestFormat() {
    Table t;
    t.turns = 300; t.pictures = 290;
    t.cell[0][kChar].logic = 300 * 4; t.cell[0][kChar].posed = 300 * 2; t.cell[0][kChar].draws = 290 * 10; t.cell[0][kChar].indices = 290 * 5000;
    t.cell[2][kPtcl].particles = 300 * 25;
    t.size[0][3].draws = 290 * 10; t.size[0][3].indices = 290 * 5000;
    char b[1100];
    int n = FormatBand(b, sizeof b, "HoneyBeeKingdomGalaxy", 0, t);
    CHECK(n > 0 && n < (int)sizeof b, "fits");
    CHECK(std::strstr(b, "dist: HoneyBeeKingdomGalaxy band 0-10 m: logic 4.0, posed 2.0, draws 10.0, indices 5000, particles 0.0 | by class map 0.0/0.0/0.0/0/0.0 chars 4.0/2.0/10.0/5000/0.0") == b, "line: %s", b);
    CHECK(std::strstr(b, "| by screen size <8px 0.0/0 8-32px 0.0/0 32-128px 0.0/0 >128px 10.0/5000") != nullptr, "sizes: %s", b);
    FormatBand(b, sizeof b, "X(stage)", 2, t);
    CHECK(std::strstr(b, "dist: X(stage) band 25-50 m: logic 0.0, posed 0.0, draws 0.0, indices 0, particles 25.0 |") == b, "%s", b);
    FormatBand(b, sizeof b, "X", 5, t);
    CHECK(std::strstr(b, "band 200-inf m:") != nullptr, "%s", b);
    FormatWin(b, sizeof b, "X", t, 5.0);
    CHECK(std::strcmp(b, "dist-win: X turns 300 pictures 290 secs 5.0") == 0, "%s", b);
    Table e; FormatBand(b, sizeof b, "X", 0, e);   // no turns: no division by zero
    CHECK(std::strstr(b, "logic 0.0") != nullptr, "%s", b);
    char small[40];
    FormatBand(small, sizeof small, "Honey", 0, t);   // a short buffer is cut, never overrun (ASan)
    CHECK(std::strlen(small) < sizeof small, "cut");
}
int main() {
    TestBands(); TestClasses(); TestSize(); TestDl(); TestFold(); TestModelIdx(); TestFormat();
    if (fails == 0) std::printf("distcensus: all passed\n");
    return fails == 0 ? 0 : 1;
}
