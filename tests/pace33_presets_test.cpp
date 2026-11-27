// pace33 PATCH6: tools/presets/stage-presets-1114-reproj.txt against the real parser (platform/vgx/src/vgx_stagepreset.h) and the real switch registry (port/stage_knob.h):
// the file parses clean on top of the compiled-in table, the heavy stages resolve to reproj 1, every other stage to reproj 2 + warp_depth 4, each heavy row keeps its compiled-in
// class and keys, and WITHOUT the file no stage carries either key (the compiled-in default is unchanged).
//   g++ -std=gnu++20 -O1 tests/pace33_presets_test.cpp platform/vgx/src/vgx_autoq.cpp -o /tmp/pace33/pt && /tmp/pace33/pt tools/presets/stage-presets-1114-reproj.txt
#include "../platform/vgx/src/vgx_autoq.h"
#include "../platform/vgx/src/vgx_stagepreset.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace vgx::stagepreset;
static int g_fail = 0;
static Table* NewTable() { return new Table(); }
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
struct Got { int reproj = -1, depth = -1, n = 0; std::string cls, bad; std::vector<std::string> keys; };
static Got Look(const Table& t, const char* stage, int scenario) {
    Spec s;
    Resolve(t, stage, scenario, s);
    Applied ap[kMaxKvSpec + 24];
    char bad[160];
    Got g;
    g.n = ValidateKv(s, ap, kMaxKvSpec + 24, bad, sizeof bad);
    g.bad = bad;
    g.cls = s.cls;
    for (int i = 0; i < g.n; ++i) {
        if (!std::strcmp(ap[i].name, "GXV_HRW_STAGE")) g.reproj = static_cast<int>(ap[i].val);
        else if (!std::strcmp(ap[i].name, "GXV_HRW_WARP_DEPTH")) g.depth = static_cast<int>(ap[i].val);
        else g.keys.push_back(std::string(ap[i].name) + "=" + std::to_string(ap[i].val));
    }
    return g;
}
int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "tools/presets/stage-presets-1114-reproj.txt";
    const bool qfile = std::strstr(path, "reproj-q") != nullptr, fresh = std::strstr(path, "-fresh-") != nullptr;
    std::string text;
    if (FILE* f = std::fopen(path, "rb")) { char b[4096]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) text.append(b, n); std::fclose(f); }
    CHECK(!text.empty());
    Table* base = NewTable();
    Table* with = NewTable();
    Diag d0, d1, d2;
    LoadBuiltin(*base, d0);
    LoadBuiltin(*with, d1);
    ParseText(*with, text.c_str(), text.size(), d2);
    std::printf("the file: %d lines, %d rows, %d classes, %d problems, %d dropped\n", d2.lines, d2.rows, d2.classes, d2.errors, d2.dropped);
    CHECK(d0.errors == 0 && d2.errors == 0 && d2.dropped == 0);
    struct St { const char* name; int scenario; bool heavy; } stages[] = {
        {"HoneyBeeKingdomGalaxy", 1, true}, {"HoneyBeeKingdomGalaxy", 2, true}, {"HoneyBeeKingdomGalaxy", 3, true}, {"HeavenlyBeachGalaxy", 1, true}, {"HeavenlyBeachGalaxy", 2, true},
        {"IceVolcanoGalaxy", 1, true}, {"IceVolcanoGalaxy", 2, true}, {"CannonFleetGalaxy", 1, true}, {"EggStarGalaxy", 1, false}, {"StarDustGalaxy", 1, false},
        {"BattleShipGalaxy", 1, false}, {"SandClockGalaxy", 1, false}, {"CosmosGardenGalaxy", 1, false}, {"PhantomGalaxy", 1, false}, {"KoopaBattleVs1Galaxy", 1, false},
        {"NoSuchGalaxy", 1, false}, {"FileSelect", 1, false}};
    for (const St& s : stages) {
        const Got a = Look(*base, s.name, s.scenario), b = Look(*with, s.name, s.scenario);
        std::printf("  %-24s:%d  class %-10s  compiled-in: reproj %2d depth %2d (%d keys) | with the file: reproj %d depth %d (%d keys)%s%s\n", s.name, s.scenario, b.cls.c_str(), a.reproj, a.depth, a.n,
                    b.reproj, b.depth, b.n, b.bad.empty() ? "" : "  REFUSED: ", b.bad.c_str());
        CHECK(a.reproj == -1 && a.depth == -1);                 // the compiled-in default is unchanged: no stage says anything
        CHECK(b.bad.empty());
        CHECK(b.reproj == (fresh ? 2 : s.heavy ? (qfile && std::strcmp(s.name, "CannonFleetGalaxy") ? 3 : 1) : 2));
        CHECK(b.depth == 4);
        if ((!qfile || b.reproj != 3) && !(fresh && !std::strcmp(s.name, "HeavenlyBeachGalaxy"))) CHECK(a.cls == b.cls && a.keys == b.keys);   // nothing else of the stage row changed (the Quality rows change picture keys on purpose)
        CHECK(a.cls == b.cls);
        if ((!qfile || b.reproj != 3) && !(fresh && !std::strcmp(s.name, "HeavenlyBeachGalaxy"))) CHECK(b.n == a.n + 2);
        if (fresh && !std::strcmp(s.name, "HeavenlyBeachGalaxy")) CHECK(b.n >= a.n + 6);   // the far field and the governors on top of the stage row
    }
    std::printf(g_fail ? "pace33_presets_test: FAILED (%d)\n" : "pace33_presets_test: ALL PASSED\n", g_fail);
    return g_fail != 0;
}
