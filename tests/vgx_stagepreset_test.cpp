// stagepreset25: host test of the per-stage presets: the text parser and the class resolution (platform/vgx/src/vgx_stagepreset.h, pure), the stage-settable
// switch registry (platform/include/port/stage_knob.h) and the controller's preset frame (Controller::ApplyStagePreset in platform/vgx/src/vgx_autoq.cpp).
// tools/test_vgx_stagepreset.sh builds it with ASan + UBSan and runs it. Exit status 0 when every check passes.
#include "../platform/vgx/src/vgx_autoq.h"
#include "../platform/vgx/src/vgx_stagepreset.h"
#include "../platform/include/port/glow26.h"
#include "../platform/include/port/fegov.h"
#include "../platform/include/port/quality30.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

using namespace vgx::stagepreset;
extern "C" int64_t port_thread_cpu_us(void) { return 100; }

namespace {
int g_failures = 0, g_checks = 0;
void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("  [FAIL] %s\n", what); }
}
#define CHECKF(c, ...) do { char _b[200]; std::snprintf(_b, sizeof _b, __VA_ARGS__); Check((c), _b); } while (0)

Table* NewTable() { return new Table(); }
void Parse(Table& t, const char* text, Diag& d) { ParseText(t, text, std::strlen(text), d); }

// ---- 1. parser ----------------------------------------------------------------------------------------------------------------------------
void TestParser() {
    std::printf("parser\n");
    Table* t = NewTable();
    Diag d;
    Parse(*t, "@class c1 start=6 size=fixed fx=down indcap_half=2\n"
              "# a comment line\n"
              "\n"
              "StageA class=c1 pb_decor=3500   # trailing comment\n"
              "StageA:2 class=c1 start=848x480 floor=4\n"
              "* class=c1\n", d);
    Check(d.errors == 0, "a clean file has no problems");
    Check(t->nClasses == 1 && t->nRows == 3, "one class and three rows");
    Check(d.classes == 1 && d.rows == 3, "diag counts");
    // malformed lines: each one ignored with a message, the rest still read
    Table* m = NewTable();
    Diag e;
    Parse(*m, "StageB class=c1 start=banana\n"        // bad rung
              "StageC size=sideways\n"                 // bad word
              "novalue\n"                              // head only (a row with no keys: accepted, empty)
              "=5 x=1\n"                               // starts with =
              "StageD:x class=c1\n"                    // bad scenario
              "StageE foo\n"                           // a token without =
              "StageF pb_decor=abc\n"                  // bad number
              "@class\n"                               // no name
              "StageG key=1e99\n"                      // out of range number
              "StageOK class=c1\n", e);
    Check(e.errors >= 7, "malformed lines and tokens are counted");
    Check(e.nmsg > 0 && e.nmsg <= 6, "at most six messages kept");
    bool okFound = false;
    for (int i = 0; i < m->nRows; ++i) okFound = okFound || !std::strcmp(m->rows[i].name, "StageOK");
    Check(okFound, "a good line after bad ones is read");
    // a very long line
    std::string longLine(600, 'x');
    longLine = "Stage" + longLine + " class=a\nStageLong2 class=a\n";
    Table* l = NewTable();
    Diag ld;
    Parse(*l, longLine.c_str(), ld);
    Check(ld.dropped >= 1 && l->nRows == 1, "a line over 255 bytes is dropped, the next one read");
    // no trailing newline, CRLF line ends, tabs, text that is not NUL-terminated at len
    Table* c = NewTable();
    Diag cd;
    const char crlf[] = "StageX\tclass=c1\tstart=6\r\nStageY class=c1";
    ParseText(*c, crlf, sizeof crlf - 1, cd);
    Check(c->nRows == 2 && cd.errors == 0, "CRLF, tabs and a missing final newline");
    const char raw[] = {'S', 'T', ' ', 'c', 'l', 'a', 's', 's', '=', 'z', 'Q', 'Q'};
    Table* r = NewTable();
    Diag rd;
    ParseText(*r, raw, 10, rd); // (len 10: "ST class=z" only; never reads past it)
    Check(r->nRows == 1 && !std::strcmp(r->rows[0].cls, "z"), "len bounds the read");
    // the table is full: no overrun, a message
    Table* f = NewTable();
    Diag fd;
    std::string many;
    for (int i = 0; i < kMaxRows + 20; ++i) { char b[64]; std::snprintf(b, sizeof b, "S%d class=a\n", i); many += b; }
    Parse(*f, many.c_str(), fd);
    Check(f->nRows == kMaxRows && fd.dropped == 20, "rows beyond the table are dropped and counted");
    // a stage row twice: the later replaces
    Table* rr = NewTable();
    Diag rrd;
    Parse(*rr, "S1 class=a start=6\nS1 class=b\n", rrd);
    Check(rr->nRows == 1 && !std::strcmp(rr->rows[0].cls, "b") && rr->rows[0].start == kNone, "a later row of the same stage replaces the earlier");
    // keys: too many on one line
    Table* k = NewTable();
    Diag kd;
    std::string line = "S1";
    for (int i = 0; i < kMaxKvRow + 3; ++i) { char b[32]; std::snprintf(b, sizeof b, " k%d=1", i); line += b; }
    line += "\n";
    Parse(*k, line.c_str(), kd);
    Check(k->rows[0].nkv == kMaxKvRow && kd.errors == 3, "more keys than a row holds: the extra ones are refused");
    // rungs
    int rung = -1;
    Check(ParseRung("4", &rung) && rung == 4, "rung 4");
    Check(ParseRung("848x480", &rung) && rung == 4, "848x480 is rung 4");
    Check(ParseRung("848", &rung) && rung == 4, "848 is rung 4");
    Check(ParseRung("960x544", &rung) && rung == 6, "960x544 is rung 6");
    Check(!ParseRung("850x480", &rung) && !ParseRung("848xabc", &rung) && !ParseRung("", &rung) && !ParseRung("x", &rung), "not a size: refused");
    delete t; delete m; delete l; delete c; delete r; delete f; delete rr; delete k;
}

// ---- 2. resolution ------------------------------------------------------------------------------------------------------------------------
// the value of a key in a resolved spec (-1e9 when the spec does not carry it)
float KvOf(const Spec& s, const char* key) {
    for (int i = 0; i < s.nkv; ++i)
        if (!std::strcmp(s.kv[i].key, key)) return s.kv[i].val;
    return -1e9f;
}
// every stage the console has measured (the 42 entries of tour c54 share 37 names, plus SkullShark of the c69 tours)
const char* const kMeasuredStages[] = {
    "CocoonExGalaxy", "CannonFleetGalaxy", "CubeBubbleExLv1Galaxy", "KoopaBattleVs2Galaxy", "HoneyBeeKingdomGalaxy", "DarkRoomGalaxy", "IceVolcanoGalaxy",
    "TransformationExGalaxy", "HeavenlyBeachGalaxy", "KoopaBattleVs3Galaxy", "KoopaBattleVs1Galaxy", "CosmosGardenGalaxy", "OceanRingGalaxy", "SandClockGalaxy",
    "FlipPanelExGalaxy", "OceanFloaterLandGalaxy", "FactoryGalaxy", "OceanPhantomCaveGalaxy", "HeavensDoorGalaxy", "FloaterOtaKingGalaxy", "StarDustGalaxy",
    "BeltConveyerExGalaxy", "PhantomGalaxy", "ReverseKingdomGalaxy", "EggStarGalaxy", "SnowCapsuleGalaxy", "TearDropGalaxy", "HellProminenceGalaxy",
    "BattleShipGalaxy", "FishTunnelGalaxy", "KoopaJrShipLv1Galaxy", "HoneyBeeExGalaxy", "SurfingLv1Galaxy", "TamakoroExLv1Galaxy", "TeresaMario2DGalaxy",
    "BreakDownPlanetGalaxy", "TriLegLv1Galaxy", "SkullSharkGalaxy"};
// the only keys the compiled-in table may name: each was measured or verified on the console (presets26: clip_skipempty and indcap_skip with Dark Matter Plant /
// Ghostly / Dreadnought / Dusty Dune scene counts, the U set by the EU tour, bloom_fold 1 by the glow-fold tour, ind_flat 1 at Freezeflame). Everything else
// (draw-order planner, light volumes, shadow clip, compositor, far-speck cuts, the governor, fold 2) is for the file until a console run prices it.
const char* const kMeasuredKeys[] = {"clip_skipempty", "indcap_skip", "indcap_half", "indcap_camgate", "clip_half", "bloom_fold", "ind_flat", "cpulod",
    "pb_lod", "pb_decor", "pb_ptcl", "pb_fur", "pb_shadow", "ptcl_thin", "ptcl_thin_d0", "ptcl_thin_d1", "piecethin_dist"};   // (14 Nov c160: the level-2 values in force on every console run so far, pinned under the base class's cpulod=3; the hub rows put level 3's back)   // (14 Nov: cpulod measured on the console in the hub, c157 2990a/b)
bool KeyMeasured(const char* k) {
    for (const char* m : kMeasuredKeys)
        if (!std::strcmp(k, m)) return true;
    return false;
}

void TestResolve() {
    std::printf("resolve\n");
    Table* t = NewTable();
    Diag d;
    LoadBuiltin(*t, d);
    CHECKF(d.errors == 0, "the compiled-in table parses clean (%d problems)", d.errors);
    Spec s;
    // unknown stage: the default row's class = the base class: 960 fixed
    Resolve(*t, "NoSuchGalaxy", 1, s);
    Check(s.matched == 0 && !std::strcmp(s.cls, "base"), "an unknown stage takes the default class (base)");
    Check(s.start == 6 && s.floorR == 6 && s.ceil == 6 && s.size == kSizeFixed && s.fx >= kFxDown && !(s.flags & (kClampedFloor | kClassMissing)),
          "base: 960x544 fixed, effects down only, nothing corrected");
    // (14 Nov c160: base also carries the far-cut step cpulod=3 with the drawn cuts pinned at level 2's values; the hub rows put level 3's back)
    Check(KvOf(s, "clip_skipempty") == 1.0f && KvOf(s, "cpulod") == 3.0f && KvOf(s, "pb_lod") == 0.45f && KvOf(s, "pb_decor") == 4000.0f && KvOf(s, "ptcl_thin") == 0.35f &&
              KvOf(s, "piecethin_dist") == 4500.0f && s.nkv == 11, "base: the clip-chain skip, cpulod=3 for the update rates, the drawn cuts at level 2");
    Resolve(*t, "AstroGalaxy", 1, s);
    Check(KvOf(s, "cpulod") == 3.0f && KvOf(s, "pb_lod") == 0.3f && KvOf(s, "pb_decor") == 3000.0f && KvOf(s, "ptcl_thin") == 0.2f && KvOf(s, "piecethin_dist") == 3500.0f,
          "the hub row's own keys win over the base class: the whole level 3");
    Resolve(*t, "NoSuchGalaxy", 1, s);
    // classes
    Resolve(*t, "BattleShipGalaxy", 1, s);
    Check(!std::strcmp(s.cls, "base") && s.matched == 1 && s.start == 6 && s.floorR == 6 && s.ceil == 6 && s.size == kSizeFixed, "BattleShip: base, 960 fixed");
    Resolve(*t, "StarDustGalaxy", 1, s);
    Check(!std::strcmp(s.cls, "gpu-light") && s.start == 6 && s.size == kSizeFixed && s.cuts == 1 && KvOf(s, "indcap_half") == 2.0f && KvOf(s, "clip_half") == 2.0f &&
          KvOf(s, "indcap_camgate") == 1.0f && KvOf(s, "clip_skipempty") == 1.0f && KvOf(s, "indcap_skip") == -1e9f, "gpu-light: base + the U set, no capture skip");
    Resolve(*t, "KoopaBattleVs1Galaxy", 1, s);
    Check(!std::strcmp(s.cls, "gpu-glow") && KvOf(s, "bloom_fold") == -1e9f && KvOf(s, "indcap_half") == 2.0f && KvOf(s, "indcap_skip") == -1e9f, "gpu-glow: gpu-light (inherits GXV_BLOOM_FOLD=2 from mode_keys); Koopa 1 is a stale-read stage: no indcap_skip");
    Resolve(*t, "IceVolcanoGalaxy", 1, s);
    Check(!std::strcmp(s.cls, "gpu-warp") && KvOf(s, "ind_flat") == 1.0f && KvOf(s, "indcap_skip") == -1e9f, "gpu-warp: ind_flat 1 (never 2); no indcap_skip at Freezeflame");
    Resolve(*t, "PhantomGalaxy", 1, s);
    Check(!std::strcmp(s.cls, "gpu-cap") && KvOf(s, "indcap_skip") == 1.0f && KvOf(s, "indcap_half") == -1e9f, "Ghostly: gpu-cap, and not the U set (it regressed there)");
    Resolve(*t, "CannonFleetGalaxy", 1, s);
    Check(!std::strcmp(s.cls, "gpu-heavy") && s.start == 6 && s.size == kSizeFixed && s.edge == 1 && KvOf(s, "indcap_skip") == 1.0f, "Dreadnought: gpu-heavy, 960 fixed, edge smoothing starts off (gov77; the console-measured keys are gpu-cap's)");
    Resolve(*t, "SandClockGalaxy", 1, s);
    Check(!std::strcmp(s.cls, "cpu") && KvOf(s, "indcap_skip") == 1.0f, "Dusty Dune: cpu class + a row key (a row's own key adds to its class)");
    Resolve(*t, "CosmosGardenGalaxy", 1, s);
    Check(!std::strcmp(s.cls, "cpu") && KvOf(s, "indcap_skip") == -1e9f, "Gusty Garden: no capture skip (stale-read stage)");
    // scenario: exact beats the stage's any-scenario row beats the default
    Resolve(*t, "HoneyBeeKingdomGalaxy", 1, s);
    Check(s.matched == 2 && !std::strcmp(s.cls, "cpu-gov") && KvOf(s, "indcap_half") == 2.0f, "Honeyhive:1 is cpu-gov (= gpu-light in the compiled-in table)");
    Resolve(*t, "HoneyBeeKingdomGalaxy", 2, s);
    Check(s.matched == 2 && !std::strcmp(s.cls, "gpu-light"), "Honeyhive:2 is gpu-light");
    Resolve(*t, "HoneyBeeKingdomGalaxy", 3, s);
    Check(s.matched == 0 && !std::strcmp(s.cls, "base"), "Honeyhive:3 has no row (the rows name scenarios): the default");
    Resolve(*t, "IceVolcanoGalaxy", 2, s);
    Check(s.matched == 1 && !std::strcmp(s.cls, "gpu-warp"), "a row with no scenario serves every scenario");
    Resolve(*t, "IceVolcanoGalaxy", -1, s);
    Check(s.matched == 1, "unknown scenario still matches a row with none");
    // an override file changes a stage and a class
    Diag fd;
    Parse(*t, "IceVolcanoGalaxy:2 class=lastresort\n@class gpu-light class=base indcap_half=1\n", fd);
    Resolve(*t, "IceVolcanoGalaxy", 2, s);
    Check(s.matched == 2 && !std::strcmp(s.cls, "lastresort") && s.start == 4 && s.floorR == 4 && s.ceil == 4 && s.size == kSizeFixed && s.edge == 1, "a file row names the 848 class: 848 fixed, edge pass off");
    Resolve(*t, "IceVolcanoGalaxy", 1, s);
    Check(!std::strcmp(s.cls, "gpu-warp") && s.matched == 1 && s.start == 6 && KvOf(s, "indcap_half") == 1.0f && KvOf(s, "ind_flat") == 1.0f,
          "the other scenario keeps its row, and a redefined class reaches every class built on it (gpu-warp -> gpu-light)");
    // THE FLOOR: nothing under rung 4 without an emergency key
    Table* u = NewTable();
    Diag ud;
    LoadBuiltin(*u, ud);
    Parse(*u, "Low1 class=lastresort start=0 floor=0 ceil=0\n"
              "Low2 class=base floor=608 start=664\n"
              "Low3 class=lastresort emergency=2 start=2 floor=2 ceil=2\n"
              "Low4 class=base size=down emergency=none floor=1\n", ud);
    Resolve(*u, "Low1", 1, s);
    Check(s.start == 4 && s.floorR == 4 && s.ceil == 4 && (s.flags & kClampedFloor), "a row under the floor constant is lifted to rung 4 and flagged");
    Resolve(*u, "Low2", 1, s);
    Check(s.floorR == 4 && s.start == 4, "608 / 664 are lifted to 848 too");
    Resolve(*u, "Low3", 1, s);
    Check(s.start == 2 && s.floorR == 2 && s.ceil == 2 && !(s.flags & kClampedFloor), "emergency=<rung> is the only way under the floor");
    Resolve(*u, "Low4", 1, s);
    Check(s.floorR == 4, "emergency=none keeps the floor");
    // ordering and size modes
    Parse(*u, "Ord1 class=base size=down floor=6 ceil=4 start=5\n"
              "Ord2 class=base size=fixed start=4 floor=6 ceil=6\n"
              "Ord3 class=base size=down fx=free\n"
              "Ord4 class=base size=auto start=6 floor=4 ceil=6 fx=free\n", ud);
    Resolve(*u, "Ord1", 1, s);
    Check(s.floorR <= s.start && s.start <= s.ceil, "floor <= start <= ceil always holds");
    Resolve(*u, "Ord2", 1, s);
    Check(s.start == 4 && s.floorR == 4 && s.ceil == 4, "size=fixed: floor = ceil = start");
    Resolve(*u, "Ord3", 1, s);
    Check(s.size == kSizeDown && s.fx >= kFxDown, "size=down forces the effects down-only too (no hunting on the effects)");
    Resolve(*u, "Ord4", 1, s);
    Check(s.size == kSizeAuto && s.fx == kFxFree, "size=auto, fx=free stay as written (the measuring mode)");
    // a missing class
    Parse(*u, "Miss class=nosuchclass\n", ud);
    Resolve(*u, "Miss", 1, s);
    Check((s.flags & kClassMissing) != 0 && s.floorR >= 4, "an unknown class is flagged and still obeys the floor");
    // zoom: >= 1.4 takes the next lighter class; a row's hz= wins; a forced class is not lightened; below 1.4 nothing changes (a table of its own: the compiled-in one has no hz)
    {
        Table* z = NewTable();
        Diag zd;
        Parse(*z, "@class zl start=6 floor=6 ceil=6 size=fixed fx=down\n@class zs class=zl start=4 floor=4 ceil=4 hz=zl\n@class zx class=zs edge=off hz=zs\n"
                  "ZX class=zx\nZS class=zs\nZN class=zl\nZRow class=zs hz=none\nZRow2 class=zs hz=zl\n* class=zl\n", zd);
        Resolve(*z, "ZX", 1, s, nullptr, 15);
        Check(!std::strcmp(s.cls, "zs") && s.zoomed, "zoom 1.5: zx gives way to zs");
        Resolve(*z, "ZX", 1, s, nullptr, 13);
        Check(!std::strcmp(s.cls, "zx") && !s.zoomed, "zoom 1.3: unchanged");
        Resolve(*z, "ZS", 1, s, nullptr, 15);
        Check(!std::strcmp(s.cls, "zl") && s.zoomed && s.start == 6, "zoom 1.5: zs -> zl");
        Resolve(*z, "ZN", 1, s, nullptr, 15);
        Check(!std::strcmp(s.cls, "zl") && !s.zoomed, "a class with no hz= is not lightened");
        Resolve(*z, "ZX", 1, s, "zx", 15);
        Check(!std::strcmp(s.cls, "zx") && !s.zoomed, "a forced class is measured as it is at any zoom");
        Resolve(*z, "ZRow", 1, s, nullptr, 15);
        Check(!std::strcmp(s.cls, "zs") && !s.zoomed, "a row's hz=none keeps its class at 1.5");
        Resolve(*z, "ZRow2", 1, s, nullptr, 15);
        Check(!std::strcmp(s.cls, "zl") && s.zoomed, "a row's hz= names its own class");
        delete z;
    }
    // the tour switch: one class for every stage, rows ignored
    Resolve(*t, "CannonFleetGalaxy", 1, s, "gpu-light");
    Check(!std::strcmp(s.cls, "gpu-light") && s.matched == 0 && s.start == 6 && s.size == kSizeFixed, "forceClass: the class alone, the stage's own row is not read");
    Resolve(*t, "CannonFleetGalaxy", 1, s, "");
    Check(!std::strcmp(s.cls, "gpu-heavy"), "an empty forceClass is no force");
    // THE TABLE (presets26): fixed 960x544 everywhere, every measured stage has a row, the class of each, and the 848 class is used by nobody
    int bad = 0, n848 = 0, cBase = 0, cCap = 0, cLight = 0, cGlow = 0, cWarp = 0, cHeavy = 0, cCpu = 0, cGov = 0, cLast = 0;
    for (const char* n : kMeasuredStages) {
        Resolve(*t, n, 1, s);
        bad += s.matched == 0;
        n848 += s.start < 6 || s.floorR < 6 || s.ceil < 6;
        cBase += !std::strcmp(s.cls, "base");
        cCap += !std::strcmp(s.cls, "gpu-cap");
        cLight += !std::strcmp(s.cls, "gpu-light");
        cGlow += !std::strcmp(s.cls, "gpu-glow");
        cWarp += !std::strcmp(s.cls, "gpu-warp");
        cHeavy += !std::strcmp(s.cls, "gpu-heavy");
        cCpu += !std::strcmp(s.cls, "cpu");
        cGov += !std::strcmp(s.cls, "cpu-gov");
        cLast += !std::strcmp(s.cls, "lastresort");
        Check(s.floorR >= kShipFloorRung, "every preset obeys the floor");
        Check(s.start == 6 && s.floorR == 6 && s.ceil == 6 && s.size == kSizeFixed && !(s.flags & (kClampedFloor | kClassMissing | kKvFull)), "every row is 960x544 fixed, nothing corrected");
        Resolve(*t, n, 1, s, nullptr, 15);
        Check(!s.zoomed, "the table has no zoom rule: the same class at 1.5");
        Resolve(*t, n, 1, s);
        char buf[700];
        Describe(s, n, 1, buf, sizeof buf);
        Check(std::strstr(buf, "stage-preset:") == buf && std::strstr(buf, " class ") != nullptr, "the log line has the agreed form");
    }
    CHECKF(bad == 0, "every measured stage has a row (%d without)", bad);
    CHECKF(n848 == 0, "no stage of the table starts, floors or ceilings under 960x544 (%d do)", n848);
    CHECKF(cLast == 0, "no stage uses the last-resort 848 class (%d do)", cLast);
    CHECKF(cBase == 21 && cCap == 2 && cLight == 5 && cGlow == 2 && cWarp == 1 && cHeavy == 1 && cCpu == 5 && cGov == 1,
           "classes of the 38 stages (scenario 1): base %d (21), gpu-cap %d (2), gpu-light %d (5), gpu-glow %d (2), gpu-warp %d (1), gpu-heavy %d (1), cpu %d (5), cpu-gov %d (1)",
           cBase, cCap, cLight, cGlow, cWarp, cHeavy, cCpu, cGov);
    // the 848 class itself still works when a file asks for it (the owner's last resort): 848 fixed, the floor allows it
    Resolve(*t, "NoSuchGalaxy", 1, s, "lastresort");
    Check(s.start == 4 && s.floorR == 4 && s.ceil == 4 && s.size == kSizeFixed && s.edge == 1 && s.fx == kFxHold && !(s.flags & kClampedFloor), "lastresort: 848x480 fixed, edge pass off, effects held, no floor correction");
    // every key of the table is one a console run measured, stage-settable, in range, and none can be an "off" (the owner's rule: effects reduced, never removed)
    {
        int unmeasured = 0, unsettable = 0, zeroEffect = 0, hi = 0;
        auto scan = [&](const Delta& dd) {
            for (int k = 0; k < dd.nkv; ++k) {
                unmeasured += !KeyMeasured(dd.kv[k].key);
                const port::stageknob::Desc* de = port::stageknob::FindDesc(dd.kv[k].key);
                unsettable += !de || dd.kv[k].val < de->lo || dd.kv[k].val > de->hi;
                zeroEffect += dd.kv[k].val == 0.0f;                                  // no key of the table is an "off" value
                hi += (!std::strcmp(dd.kv[k].key, "ind_flat") && dd.kv[k].val > 1.0f) || (!std::strcmp(dd.kv[k].key, "bloom_fold") && dd.kv[k].val > 1.0f);  // never ind_flat 2, never fold 2
            }
        };
        for (int i = 0; i < t->nClasses; ++i) scan(t->classes[i]);
        for (int i = 0; i < t->nRows; ++i) scan(t->rows[i]);
        CHECKF(unmeasured == 0, "every key of the compiled-in table is on the console-measured list (%d are not)", unmeasured);
        CHECKF(unsettable == 0, "every key of the compiled-in table is stage-settable and in range (%d are not)", unsettable);
        CHECKF(zeroEffect == 0, "no key of the compiled-in table is an off value (%d)", zeroEffect);
        CHECKF(hi == 0, "ind_flat stays at 1 and bloom_fold at 1 in the compiled-in table (%d higher)", hi);
        // the resolved spec of every stage validates with no refusal
        int refused = 0;
        for (const char* n : kMeasuredStages) {
            Resolve(*t, n, 1, s);
            Applied ap[kMaxKvSpec + 24];   // (the back end's capacity: a line's cpulod=N adds its members)
            char bd[200];
            refused += ValidateKv(s, ap, kMaxKvSpec + 24, bd, sizeof bd) < s.nkv || bd[0] != 0;
        }
        CHECKF(refused == 0, "the resolved keys of every stage validate (%d stages refused a key)", refused);
    }
    delete t; delete u;
}

// ---- 3. validation of the switch keys ------------------------------------------------------------------------------------------------------
void TestValidate() {
    std::printf("validate\n");
    Table* t = NewTable();
    Diag d;
    Parse(*t, "@class c start=6\n"
              "S1 class=c indcap_half=2 GXV_INDCAP_CAMGATE=1 nosuchkey=3 indcap_half2=1 pb_lod=0.5 detail_level=9 hud_native=1\n", d);
    Spec s;
    Resolve(*t, "S1", 1, s);
    Applied ap[kMaxKvSpec];
    char bad[200];
    const int n = ValidateKv(s, ap, kMaxKvSpec, bad, sizeof bad);
    Check(n == 4, "four valid keys survive (alias, full name, a float factor, hud_native)");
    Check(std::strstr(bad, "nosuchkey") && std::strstr(bad, "indcap_half2") && std::strstr(bad, "detail_level (out of range)"), "unknown and out-of-range keys are named in the refusal text");
    bool full = false;
    for (int i = 0; i < n; ++i) full = full || !std::strcmp(ap[i].name, "GXV_INDCAP_CAMGATE");
    Check(full, "a key given by the switch's own name is accepted");
    // (freeze25) the reduced-size effect keys and ind_flat: a 24-bit shader key is exact as a stage value; a 25-bit one is refused
    {
        Table* tf = NewTable();
        Diag df;
        Parse(*tf, "Fz class=none ind_flat=1 fxlow=1 fxlow_runs=3 fxlow_k1=14252683 fxlow_k2=4469606 fxlow_k3=16777215\n"
                   "FzBad class=none ind_flat=3 fxlow_k1=16777216 fxlow_runs=0\n", df);
        Spec sf, sg;
        Resolve(*tf, "Fz", 1, sf);
        Resolve(*tf, "FzBad", 1, sg);
        Applied af[kMaxKvSpec], ag[kMaxKvSpec];
        char bf[200], bg[200];
        Check(ValidateKv(sf, af, kMaxKvSpec, bf, sizeof bf) == 6 && bf[0] == 0, "ind_flat, fxlow, fxlow_runs, fxlow_k1..k3 are stage-settable");
        bool exact = false;
        for (int i = 0; i < 6; ++i) exact = exact || (!std::strcmp(af[i].name, "GXV_FXLOWRES_K1") && static_cast<long>(af[i].val) == 14252683);
        Check(exact, "a 24-bit shader key survives as a stage value");
        Check(ValidateKv(sg, ag, kMaxKvSpec, bg, sizeof bg) == 0 && std::strstr(bg, "ind_flat") && std::strstr(bg, "fxlow_k1") && std::strstr(bg, "fxlow_runs"), "out-of-range ind_flat / key / runs are refused");
        delete tf;
    }
    // (cpulod26) `cpulod=N` stands for the umbrella's members the line does not name; a member the line names keeps its own value; level 0 and out of range add nothing
    {
        Table* tc = NewTable();
        Diag dc;
        Parse(*tc, "Cl class=none cpulod=2 matlod=4 far_draw=0\nCl1 class=none cpulod=1\nCl0 class=none cpulod=0\nClBad class=none cpulod=4\n\nCl3 class=none cpulod=3\n", dc);
        Spec s2, s1, s0, sb, s3;
        Resolve(*tc, "Cl3", 1, s3);
        Resolve(*tc, "Cl", 1, s2);
        Resolve(*tc, "Cl1", 1, s1);
        Resolve(*tc, "Cl0", 1, s0);
        Resolve(*tc, "ClBad", 1, sb);
        Applied a2[kMaxKvSpec + 24], a1[kMaxKvSpec + 24], a0[kMaxKvSpec + 24], ab[kMaxKvSpec + 24], a3[kMaxKvSpec + 24];
        char b2[200], b1[200], b0[200], bb[200], b3[200];
        int perfOnly = 0;
        for (int i = 0; i < port::cpulod::kMemberCount; ++i) perfOnly += port::cpulod::kMembers[i].perfOnly ? 1 : 0;
        const int n2 = ValidateKv(s2, a2, kMaxKvSpec + 24, b2, sizeof b2), n1 = ValidateKv(s1, a1, kMaxKvSpec + 24, b1, sizeof b1);
        const int n0 = ValidateKv(s0, a0, kMaxKvSpec + 24, b0, sizeof b0), nb = ValidateKv(sb, ab, kMaxKvSpec + 24, bb, sizeof bb);
        const int n3 = ValidateKv(s3, a3, kMaxKvSpec + 24, b3, sizeof b3);
        Check(n1 == 1 + port::cpulod::kMemberCount - perfOnly && b1[0] == 0, "cpulod=1 expands into the key itself and every member (but the perf-only ones, which perf.cpp reads from the level)");
        Check(n3 == n1 && b3[0] == 0, "cpulod=3 is accepted (cpu2-26)");
        Check(n2 == 3 + port::cpulod::kMemberCount - perfOnly - 2 && b2[0] == 0, "cpulod=2 with two named members: the key, the two named, the rest");
        float matlod = -1, fardraw = -1, thin = -1;
        for (int i = 0; i < n2; ++i) {
            if (!std::strcmp(a2[i].name, "GXV_MATLOD")) matlod = a2[i].val;
            if (!std::strcmp(a2[i].name, "GXV_FARDRAW")) fardraw = a2[i].val;
            if (!std::strcmp(a2[i].name, "GXV_PTCL_THIN")) thin = a2[i].val;
        }
        Check(matlod == 4.0f && fardraw == 0.0f && thin == 0.35f, "named members keep their value (matlod 4, far_draw 0); an unnamed one gets the level's (ptcl_thin 0.35)");
        Check(n0 == 1, "cpulod=0 adds nothing");
        Check(nb == 0 && std::strstr(bb, "cpulod (out of range)"), "cpulod=4 is refused");
        delete tc;
    }
    // (across26) `across=N`: expands into the stage-settable members the line does not name (silh_gate, silh_occ, indcap_skip; GXV_SHADOW_STENCIL has no stage key); silh_occ is a key of its own
    {
        Table* ta = NewTable();
        Diag da;
        Parse(*ta, "Ac class=none across=2 silh_gate=0\nAc1 class=none across=1\nAc0 class=none across=0\nAcBad class=none across=3\nOc class=none silh_gate=1 silh_occ=2\nOcBad class=none silh_occ=4\n", da);
        Spec s2, s1, s0, sb, so, sx;
        Resolve(*ta, "Ac", 1, s2);
        Resolve(*ta, "Ac1", 1, s1);
        Resolve(*ta, "Ac0", 1, s0);
        Resolve(*ta, "AcBad", 1, sb);
        Resolve(*ta, "Oc", 1, so);
        Resolve(*ta, "OcBad", 1, sx);
        Applied a2[kMaxKvSpec + 24], a1[kMaxKvSpec + 24], a0[kMaxKvSpec + 24], ab[kMaxKvSpec + 24], ao[kMaxKvSpec + 24], ax[kMaxKvSpec + 24];
        char b2[200], b1[200], b0[200], bb[200], bo[200], bx[200];
        const int n2 = ValidateKv(s2, a2, kMaxKvSpec + 24, b2, sizeof b2), n1 = ValidateKv(s1, a1, kMaxKvSpec + 24, b1, sizeof b1);
        const int n0 = ValidateKv(s0, a0, kMaxKvSpec + 24, b0, sizeof b0), nb = ValidateKv(sb, ab, kMaxKvSpec + 24, bb, sizeof bb);
        const int no = ValidateKv(so, ao, kMaxKvSpec + 24, bo, sizeof bo), nx = ValidateKv(sx, ax, kMaxKvSpec + 24, bx, sizeof bx);
        float gate2 = -1, occ2 = -1, occ1 = -1, ind1 = -1, tail2 = -1, tail1 = -1;
        for (int i = 0; i < n2; ++i) {
            if (!std::strcmp(a2[i].name, "GXV_SILH_GATE")) gate2 = a2[i].val;
            if (!std::strcmp(a2[i].name, "GXV_SILH_OCC")) occ2 = a2[i].val;
            if (!std::strcmp(a2[i].name, "GXV_TAILDEPTH")) tail2 = a2[i].val; // (scenes26)
        }
        for (int i = 0; i < n1; ++i) {
            if (!std::strcmp(a1[i].name, "GXV_SILH_OCC")) occ1 = a1[i].val;
            if (!std::strcmp(a1[i].name, "GXV_INDCAP_SKIP")) ind1 = a1[i].val;
            if (!std::strcmp(a1[i].name, "GXV_TAILDEPTH")) tail1 = a1[i].val;
        }
        Check(n1 == 1 + 3 && b1[0] == 0 && occ1 == 1.0f && ind1 == 1.0f, "across=1: the key and the three stage-settable members (gate, occ, indcap_skip)");
        Check(gate2 == 0.0f && occ2 == 2.0f && n2 == 1 + 1 + 4, "across=2 with silh_gate=0 named: the named value stays 0, the others come from the level");
        Check(tail2 == 1.0f && tail1 == -1.0f, "scenes26: across=2 adds tail_depth=1, across=1 leaves it alone (a member that is 0 at a level is not written)");
        Check(n0 == 1, "across=0 adds nothing");
        Check(nb == 0 && std::strstr(bb, "across (out of range)"), "across=3 is refused");
        Check(no == 2 && bo[0] == 0, "silh_occ=2 is accepted on its own");
        Check(nx == 0 && std::strstr(bx, "silh_occ"), "silh_occ=4 is refused");
        delete ta;
    }
    // (dread25) the draw-order and tiny-run keys: accepted in range, refused out of range, found by alias and by switch name
    {
        Table* t2 = NewTable();
        Diag d2;
        Parse(*t2, "@class dread class=none hsr_order=1 hsr_ties=1 tiny_run=1 tiny_px=2.5\n"
                   "Dread class=dread\n"
                   "Bad class=none hsr_order=4 tiny_run=3 tiny_px=65 GXV_HSR_ORDER_TIES=0\n", d2);
        Spec sd, sb;
        Resolve(*t2, "Dread", 1, sd);
        Applied ad[kMaxKvSpec], ab[kMaxKvSpec];
        char badd[200], badb[200];
        Check(ValidateKv(sd, ad, kMaxKvSpec, badd, sizeof badd) == 4 && badd[0] == 0, "hsr_order, hsr_ties, tiny_run, tiny_px are stage-settable");
        bool o = false, r = false;
        for (int i = 0; i < 4; ++i) {
            o = o || (!std::strcmp(ad[i].name, "GXV_HSR_ORDER") && ad[i].val == 1.0f);
            r = r || (!std::strcmp(ad[i].name, "GXV_TINYRUN_PX") && ad[i].val == 2.5f);
        }
        Check(o && r, "the keys map to the switches' own names with their values");
        Resolve(*t2, "Bad", 1, sb);
        Check(ValidateKv(sb, ab, kMaxKvSpec, badb, sizeof badb) == 1 && std::strstr(badb, "hsr_order (out of range)") && std::strstr(badb, "tiny_run (out of range)") &&
                  std::strstr(badb, "tiny_px (out of range)"), "values out of range are refused, a switch name given whole is accepted");
        Check(port::stageknob::FindDesc("GXV_TINYRUN") && port::stageknob::FindDesc("tiny_px") && port::stageknob::FindDesc("GXV_HSR_ORDER_TIES"), "found by alias and by name");
        { // (geom26) hsr_ties=7 (the decal-aware rule) and hsr_auto (the per-stage decision) are stage-settable; 8 / 3 are out of range
            Table* t3 = NewTable();
            Diag d3;
            Parse(*t3, "G26 class=none hsr_auto=1 hsr_ties=7\nG26b class=none hsr_ties=8 hsr_auto=3\n", d3);
            Spec s7, s8;
            Resolve(*t3, "G26", 1, s7);
            Applied a7[kMaxKvSpec], a8[kMaxKvSpec];
            char b7[200], b8[200];
            Check(ValidateKv(s7, a7, kMaxKvSpec, b7, sizeof b7) == 2 && b7[0] == 0, "geom26: hsr_auto=1 and hsr_ties=7 are accepted");
            Resolve(*t3, "G26b", 1, s8);
            Check(ValidateKv(s8, a8, kMaxKvSpec, b8, sizeof b8) == 0 && std::strstr(b8, "hsr_ties (out of range)") && std::strstr(b8, "hsr_auto (out of range)"), "geom26: hsr_ties=8 and hsr_auto=3 are refused");
            Check(port::stageknob::FindDesc("GXV_HSR_AUTO") == port::stageknob::FindDesc("hsr_auto") && !port::stageknob::IsExact(*port::stageknob::FindDesc("hsr_auto"), 1), "geom26: hsr_auto is found by both names and is a sacrifice (not applied in Quality mode)");
            delete t3;
        }
        {
            const port::stageknob::Desc* d = port::stageknob::FindDesc("alphatest_prepass");
            Check(d && d == port::stageknob::FindDesc("GXV_ALPHATEST_PREPASS") && d->lo == 0 && d->hi == 5 && d->cls == 'N', "alphatest_prepass: found by alias and by name, 0..5, class N");
        }
        {
            Table* t3 = NewTable();
            Diag d3;
            Parse(*t3, "@class dp class=none alphatest_prepass=5 indcap_skip=1\nP class=dp\nBadP class=none alphatest_prepass=6\n", d3);
            Spec sp, sq;
            Resolve(*t3, "P", 1, sp);
            Applied ap3[kMaxKvSpec], aq3[kMaxKvSpec];
            char b3[200], b4[200];
            const int n3 = ValidateKv(sp, ap3, kMaxKvSpec, b3, sizeof b3);
            bool seen = false;
            for (int i = 0; i < n3; ++i) seen = seen || (!std::strcmp(ap3[i].name, "GXV_ALPHATEST_PREPASS") && ap3[i].val == 5.0f);
            Check(n3 == 2 && seen && b3[0] == 0, "alphatest_prepass=5 is accepted and maps to GXV_ALPHATEST_PREPASS");
            Resolve(*t3, "BadP", 1, sq);
            ValidateKv(sq, aq3, kMaxKvSpec, b4, sizeof b4);
            Check(std::strstr(b4, "alphatest_prepass (out of range)") != nullptr, "alphatest_prepass=6 is refused");
            delete t3;
        }
        delete t2;
    }
    // every descriptor: its alias and name resolve, ranges are sane, no duplicates
    for (int i = 0; i < port::stageknob::kDescCount; ++i) {
        const port::stageknob::Desc& dd = port::stageknob::kDesc[i];
        Check(port::stageknob::FindDesc(dd.alias) == &dd && port::stageknob::FindDesc(dd.name) == &dd, "descriptor found by alias and name");
        Check(dd.lo <= dd.hi, "range sane");
        for (int j = i + 1; j < port::stageknob::kDescCount; ++j)
            Check(std::strcmp(dd.alias, port::stageknob::kDesc[j].alias) && std::strcmp(dd.name, port::stageknob::kDesc[j].name), "no duplicate descriptor");
    }
    // the owner's rule: no key of the compiled-in table can be an "off" of a glow / bloom / haze / refraction, and a key that could is clamped
    {
        Table* b = NewTable(); Diag bd; LoadBuiltin(*b, bd);
        int offs = 0;
        for (int i = 0; i < b->nClasses; ++i)
            for (int k = 0; k < b->classes[i].nkv; ++k) { const auto* dd = port::stageknob::FindDesc(b->classes[i].kv[k].key); offs += dd && dd->offVal > -1e8f && b->classes[i].kv[k].val == dd->offVal; }
        for (int i = 0; i < b->nRows; ++i)
            for (int k = 0; k < b->rows[i].nkv; ++k) { const auto* dd = port::stageknob::FindDesc(b->rows[i].kv[k].key); offs += dd && dd->offVal > -1e8f && b->rows[i].kv[k].val == dd->offVal; }
        Check(offs == 0, "the compiled-in table holds no off value of an effect");
        port::stageknob::Desc fake{"fakebloom", "GXV_FAKE_BLOOM", 0, 3, 'N', 'a', 0.0f, 3.0f};
        bool cl = false;
        Check(port::stageknob::ClampOff(fake, 0.0f, false, &cl) == 3.0f && cl, "an off value is clamped to the weakest reduced form");
        Check(port::stageknob::ClampOff(fake, 0.0f, true, &cl) == 0.0f && !cl, "unless the test override allows it");
        Check(port::stageknob::ClampOff(fake, 2.0f, false, &cl) == 2.0f && !cl, "a reduced value passes");
        delete b;
    }
    delete t;
}

// ---- 4. the registry: overrides, caches, generation -----------------------------------------------------------------------------------------
int ReadKnobA() { return PORT_STAGE_KNOB(int32_t, static_cast<int32_t>(port::stageknob::Int("STAGEPRESET_TEST_A", 7))); }
float ReadKnobB() { return PORT_STAGE_KNOB(float, port::stageknob::Num("STAGEPRESET_TEST_B", 0.6f)); }
void TestKnobs() {
    std::printf("knobs\n");
    using namespace port::stageknob;
    ClearAll();
    Commit();
    Check(Int("STAGEPRESET_TEST_UNSET", 3) == 3, "no override, no environment: the default");
    setenv("STAGEPRESET_TEST_ENV", "5", 1);
    Check(Int("STAGEPRESET_TEST_ENV", 3) == 5, "the environment is the global default");
    setenv("STAGEPRESET_TEST_EMPTY", "", 1);
    Check(Int("STAGEPRESET_TEST_EMPTY", 9) == 9, "an empty variable counts as unset, as EnvInt did");
    Check(ReadKnobA() == 7 && ReadKnobB() == 0.6f, "knobs read the default");
    Check(KnobsRegistered() >= 2, "the knobs registered themselves");
    // a stage sets both
    Set("STAGEPRESET_TEST_A", 2);
    Set("STAGEPRESET_TEST_B", 0.25f);
    Check(ReadKnobA() == 7, "a Set takes effect at Commit, not before (a cached knob keeps its value)");
    Commit();
    Check(ReadKnobA() == 2 && ReadKnobB() == 0.25f, "after Commit the knobs read the stage's values");
    Check(Int("STAGEPRESET_TEST_ENV", 3) == 5, "other switches are untouched");
    // the environment still wins nothing over the stage; the next stage clears
    setenv("STAGEPRESET_TEST_A", "11", 1);
    Check(ReadKnobA() == 2, "the stage's value beats the environment");
    ClearAll();
    Commit();
    Check(ReadKnobA() == 11 && ReadKnobB() == 0.6f, "leaving the stage: back to the environment's (global) value");
    // the table is bounded
    int sets = 0;
    for (int i = 0; i < kSlots + 8; ++i) { char b[32]; std::snprintf(b, sizeof b, "K%d", i); sets += Set(b, 1.0f) ? 1 : 0; }
    Check(sets == kSlots, "the override table holds kSlots entries and refuses the rest");
    ClearAll();
    Commit();
    Check(Count() == 0, "ClearAll empties it");
    const uint32_t g0 = Gen();
    Commit();
    Check(Gen() == g0 + 1, "every Commit changes the generation");
    unsetenv("STAGEPRESET_TEST_A");
}

// ---- 5. the controller's frame ---------------------------------------------------------------------------------------------------------------
using namespace vgx::autoq;
struct Sim {
    Controller ctl;
    uint64_t now = 1000000, backend = 1;
    uint32_t game = 1, turns = 1, front = 1, pics = 1;
    unsigned changesInPlay = 0;
    int lastRung;
    std::vector<int> rungsSeen;
    explicit Sim(const Config& c) : ctl(c), lastRung(c.startRung) {}
    // one picture; gpuMs from the load model; interval = what the GPU and the retrace allow
    void Frame(double gpuMs, uint32_t scene, bool loading, uint64_t gapUs = 0, bool quiet = false) {
        const double work = std::max(8.0, gpuMs);
        const unsigned n = static_cast<unsigned>(std::max(1.0, std::ceil(work * 1000.0 / 16683.0 - 1e-9)));
        const uint64_t interval = gapUs ? gapUs : static_cast<uint64_t>(n * 16683.0);
        FrameTimes f;
        f.startUs = now;
        now += interval;
        f.presentUs = now;
        backend += 4000;
        game += 8000 * n;
        turns += n;
        front += 6000;
        ++pics;
        f.backendRunUs = backend;
        f.gameRunUs = game;
        f.gameTurns = turns;
        f.gameKnown = true;
        f.frontRunUs = front;
        f.frontPictures = pics;
        f.frontKnown = true;
        f.timers = f.busyKnown = true;
        f.gpuBusyUs = static_cast<uint32_t>(gpuMs * 1000.0);
        f.quiet = quiet;
        f.scene = scene;
        f.loading = loading;
        const int before = ctl.Current();
        ctl.Add(f);
        if (!loading && !quiet && ctl.Current() != before) ++changesInPlay;
        if (ctl.Current() != lastRung) { lastRung = ctl.Current(); rungsSeen.push_back(lastRung); }
    }
    void Warm() { for (int i = 0; i < 3; ++i) Frame(8.0, 0x0001, false); } // (the first picture has no interval before it)
    // a load: a long gap with a new scene key (the screen closed), then a few pictures of the new scene
    void Load(uint32_t scene) {
        Frame(5.0, scene, true, 3000000);
        for (int i = 0; i < 30; ++i) Frame(5.0, scene, true);
    }
    void Play(double seconds, double (*gpu)(int rung), uint32_t scene) {
        const uint64_t end = now + static_cast<uint64_t>(seconds * 1e6);
        while (now < end) {
            // (the effects' own cost: the present's edge pass 3 ms while it is on, the half-rate captures save 1 ms)
            const double g = gpu(ctl.Current()) + (ctl.EdgeStepOn() || ctl.HoldEdgeOff() ? 0.0 : 3.0) - (ctl.ShedCutOn() ? 1.0 : 0.0);
            Frame(g, scene, false);
        }
    }
};
double HeavyGpu(int rung) { return 3.0 + 17.0 * Cost(rung) / Cost(6); }          // 20 ms at 960x544 without the edge pass (23 with it), ~16.2 at 848x480: only 848 fits 16.7
double EdgeHeavy(int rung) { return 3.0 + 11.5 * Cost(rung) / Cost(6); }         // 17.5 at 960x544 with the edge pass: only the effects are needed
double LightGpu(int rung) { return 3.0 + 6.0 * Cost(rung) / Cost(6); }          // 9 ms at 960x544
double WorseGpu(int rung) { return 6.0 + 40.0 * Cost(rung) / Cost(6); }         // a boss: 46 ms

// (the console's Performance configuration as tests/vgx_autoq_shed.cpp builds it: target 60, the shed ladder with captures, the edge step, wipe cap)
Config PerfConfig() {
    Config c;
    c.targetFps = 60;
    c.budgetMs = 1000.0 / 60.0;
    c.minRung = 0;
    c.maxRung = kTopSingle;
    c.startRung = kTopSingle;
    c.recover = true;
    c.recoverHoldSec = 20.0;
    c.overload = c.overloadFx = true;
    c.freeFrac = 0.85;
    c.wipe = c.lateRoom = true;
    c.cadenceUp = true;
    c.bgStep = true;
    c.edgeStep = true;
    c.edgeCheck = true;
    c.aniso = true;
    c.anisoRung = 2;
    c.fast60 = true;
    c.qg = false;
    c.seam = true;
    c.shed = true;
    c.shedCuts = true;
    c.shedDirectMs = 0.0;
    return c;
}
Controller::StagePreset Preset(int start, int mn, int mx, bool fixed, bool down, bool edge, bool cuts, int fx) {
    Controller::StagePreset p;
    p.start = start; p.minRung = mn; p.maxRung = mx; p.fixedSize = fixed; p.noSizeUp = down; p.edgeOff = edge; p.cuts = cuts; p.fx = fx;
    return p;
}
void TestController() {
    std::printf("controller\n");
    // (a) a stage preset, size fixed at 848 with effects held: the new scene starts there, nothing ever changes in play
    {
        Sim s(PerfConfig());
        s.Warm();
        s.ctl.ApplyStagePreset(Preset(4, 4, 4, true, false, true, true, 2));
        Check(s.ctl.StagePresetActive() && s.ctl.StagePresetMinRung() == 4 && s.ctl.StagePresetMaxRung() == 4, "applied: floor = ceiling = 4");
        Check(s.ctl.Current() == kTopSingle, "applying a preset resizes nothing at once (the picture on the screen is the old stage's)");
        s.Load(0x1111);
        Check(s.ctl.Current() == 4, "the stage's load puts the picture at 848x480");
        Check(s.ctl.EdgeStepOn() || s.ctl.HoldEdgeOff(), "the edge pass is off from the first picture");
        Check(s.ctl.ShedCutOn(), "the half-rate captures are engaged from the first picture");
        s.changesInPlay = 0;
        s.Play(40, HeavyGpu, 0x1111);
        s.Play(40, WorseGpu, 0x1111);
        s.Play(40, LightGpu, 0x1111);
        CHECKF(s.ctl.Current() == 4 && s.changesInPlay == 0, "fixed size, held effects: 2 minutes of heavy, boss and light play change nothing (rung %d, %u changes)", s.ctl.Current(), s.changesInPlay);
        Check(s.ctl.ShedCutOn() && (s.ctl.EdgeStepOn() || s.ctl.HoldEdgeOff()), "the held effects stay as the preset set them");
    }
    // (b) fixed at 960 with the effects free-down: size never moves; a boss sheds effects, and they stay shed (no hunting)
    {
        Sim s(PerfConfig());
        s.Warm();
        s.ctl.ApplyStagePreset(Preset(6, 6, 6, true, false, false, false, 1));
        s.Load(0x2222);
        Check(s.ctl.Current() == 6, "fixed 960x544: the wipe cap does not apply to a stage with a preset");
        s.changesInPlay = 0;
        s.Play(30, LightGpu, 0x2222);
        s.Play(60, EdgeHeavy, 0x2222);
        s.Play(120, LightGpu, 0x2222);
        Check(s.ctl.HoldEdgeOff() || s.ctl.EdgeStepOn(), "a heavy moment at fixed size gave up the edge pass");
        CHECKF(s.ctl.Current() == 6, "size stays 960x544 through a boss (rung %d)", s.ctl.Current());
        CHECKF(s.changesInPlay == 0, "no size change in play at all (%u)", s.changesInPlay);
        // the effects: count their changes after the boss: down only means never a way back up
        const unsigned fxAfterBoss = s.ctl.ShedChanges();
        s.Play(120, LightGpu, 0x2222);
        CHECKF(s.ctl.ShedChanges() == fxAfterBoss, "fx=down: after the heavy moment no step is taken back (%u -> %u changes)", fxAfterBoss, s.ctl.ShedChanges());
    }
    // (c) normal: 960 start, may go down to 848 once in play, never up again, never to 896 (two sizes)
    {
        Sim s(PerfConfig());
        s.Warm();
        s.ctl.ApplyStagePreset(Preset(6, 4, 6, false, true, false, false, 1));
        s.Load(0x3333);
        Check(s.ctl.Current() == 6, "normal starts at 960x544");
        s.Play(90, HeavyGpu, 0x3333);
        Check(s.ctl.Current() == 4, "normal: a stage that needs 848x480 steps down to it");
        bool saw5 = false;
        for (int r : s.rungsSeen) saw5 = saw5 || r == 5;
        Check(!saw5, "two sizes: the rung between (896x512) is never used");
        Check(s.ctl.Current() == 4 || s.ctl.Current() == 6, "only 848x480 or 960x544");
        const int low = s.ctl.Current();
        s.changesInPlay = 0;
        s.Play(300, LightGpu, 0x3333);
        CHECKF(s.ctl.Current() == low || low == 6, "size down only: after the step down no way back up in 5 minutes of light play (rung %d -> %d)", low, s.ctl.Current());
        if (low == 4) Check(s.ctl.StagePresetMaxRung() == 4, "the ceiling came down with the step");
    }
    // (d) the next stage: another preset replaces it; clearing restores the configuration's frame
    {
        Sim s(PerfConfig());
        s.Warm();
        s.ctl.ApplyStagePreset(Preset(4, 4, 4, true, false, true, true, 2));
        s.Load(0x4444);
        Check(s.ctl.Current() == 4 && s.ctl.ShedCutOn(), "stage one: 848, captures engaged");
        s.ctl.ApplyStagePreset(Preset(6, 6, 6, true, false, false, false, 1));
        s.Load(0x5555);
        Check(s.ctl.Current() == 6, "stage two: the new preset's size at its load");
        Check(!s.ctl.ShedCutOn() && !s.ctl.HoldEdgeOff(), "stage two: the captures and the edge pass the first stage forced are back");
        s.ctl.ClearStagePreset();
        Check(!s.ctl.StagePresetActive() && s.ctl.StagePresetMinRung() == 0 && s.ctl.StagePresetMaxRung() == 6, "cleared: the configuration's rung frame is back");
        s.Load(0x6666);
        Check(s.ctl.Current() != 99, "a stage with no preset loads under the old rules");
    }
    // (e) the same stage again (a retry): back to the stage's own start
    {
        Sim s(PerfConfig());
        s.Warm();
        s.ctl.ApplyStagePreset(Preset(6, 4, 6, false, true, false, false, 1));
        s.Load(0x7777);
        s.Play(60, HeavyGpu, 0x7777);
        s.ctl.ApplyStagePreset(Preset(6, 4, 6, false, true, false, false, 1)); // (the load of the same stage begins: the same preset again)
        s.Load(0x7777);
        Check(s.ctl.Current() == 6 && s.ctl.StagePresetMaxRung() == 6, "a retry starts at the preset's 960x544 again with the ceiling back");
    }
    // (g) policy26 (GXV_SMOOTH60): a size change inside the stage, put in force at once, and the way to 960x544 again for the even 30
    {
        Sim s(PerfConfig());
        s.Warm();
        s.ctl.ApplyStagePreset(Preset(6, 6, 6, true, false, false, false, 1));
        s.Load(0x9999);
        Check(s.ctl.Current() == 6, "smooth60: the stage begins at 960x544");
        s.changesInPlay = 0;
        s.Play(20, LightGpu, 0x9999);
        s.ctl.ApplyStagePreset(Preset(4, 4, 4, true, false, true, false, 1));  // (the @848 frame: applying it moves nothing by itself)
        Check(s.ctl.Current() == 6, "smooth60: the 848 frame alone resizes nothing");
        Check(s.ctl.ForceRung(4, s.now) && s.ctl.Current() == 4, "smooth60: ForceRung takes the picture to 848x480 at once");
        Check(s.ctl.LastChange().why == Why::Forced, "smooth60: the change is recorded as forced (urgent for the back end)");
        s.Play(60, HeavyGpu, 0x9999);
        CHECKF(s.ctl.Current() == 4, "smooth60: a fixed 848 frame holds through a heavy minute (rung %d)", s.ctl.Current());
        s.ctl.ApplyStagePreset(Preset(6, 6, 6, true, false, false, false, 1));  // (the @30 frame: 960x544 again, the quality back)
        Check(s.ctl.ForceRung(6, s.now) && s.ctl.Current() == 6, "smooth60: ForceRung takes it back to 960x544 for the even 30");
        s.Play(60, LightGpu, 0x9999);
        CHECKF(s.ctl.Current() == 6, "smooth60: and it stays there (rung %d)", s.ctl.Current());
        Check(!s.ctl.ForceRung(6, s.now), "smooth60: ForceRung to the rung it is at changes nothing");
    }
    // (f) a controller never given a preset behaves as before: the wipe cap and remembered rungs
    {
        Sim s(PerfConfig());
        s.Warm();
        s.Load(0x8888);
        Check(!s.ctl.StagePresetActive() && s.ctl.Current() == 2, "no preset: a new scene starts at today's rung (the wipe cap) as it did");
    }
}

// ---- 6. modes26: the two picture modes (Performance 60 / Quality 30) ----------------------------------------------------------------------------
bool HasKey(const Spec& s, const char* key) { for (int i = 0; i < s.nkv; ++i) if (!std::strcmp(s.kv[i].key, key)) return true; return false; }
bool ClampedHas(const Spec& s, const char* text) { for (int i = 0; i < s.nclamp; ++i) if (!std::strcmp(s.clamp[i], text)) return true; return false; }
bool Skipped(const Spec& s, const char* key) { for (int i = 0; i < s.nskip; ++i) if (!std::strcmp(s.skip[i], key)) return true; return false; }
void TestModes() {
    std::printf("modes\n");
    using port::stageknob::Desc; using port::stageknob::kDesc; using port::stageknob::kDescCount; using port::stageknob::IsExact; using port::stageknob::FindDesc;
    // the classification: every key has a letter; every 'E' key is exact; nothing noticeable ('U' / 'N') is exact except the three partial-by-value keys of the ledger
    int nExact = 0, nPartial = 0, nBarely = 0, nSacrifice = 0;
    for (int i = 0; i < kDescCount; ++i) {
        const Desc& d = kDesc[i];
        Check(d.ex == 's' || d.ex == 'x' || d.ex == 'p' || d.ex == 'b', "every key has a mode class (x exact, p partial by value, b barely noticeable, s sacrifice)");
        if (d.cls == 'E' && std::strcmp(d.alias, "hsr_ties") != 0) CHECKF(d.ex == 'x', "class E key %s is exact in both modes", d.alias);   // (hsr_ties: only 4 is exact; q30-26: 'b' in [4, 7])
        if (d.ex == 'x') ++nExact; else if (d.ex == 'p') ++nPartial; else if (d.ex == 'b') ++nBarely; else ++nSacrifice;
        if (d.ex == 'b') CHECKF(d.bMin <= d.bMax, "b key %s: bMin <= bMax", d.alias);
        if (d.ex != 'b') CHECKF(d.bMin < -1e8f && d.bMax > 1e8f, "%s is not 'b': no clamp range", d.alias);
        if (d.cls != 'E' && d.ex == 'x') CHECKF(false, "%s is exact but not class E (ledger-proven keys are partial 'p')", d.alias);
    }
    std::printf("  %d exact, %d partial, %d barely noticeable, %d sacrifice keys\n", nExact, nPartial, nBarely, nSacrifice);
    Check(nExact == 6 && nPartial == 3 && nBarely == 32 && nSacrifice == kDescCount - 41, "exact: indcap_skip indcap_vis scenes_dry clearq mirror_census clip_skipempty; partial: mirror colshadow_clip lightvol; 32 barely noticeable (q30-26 without ind_flat/clip_half)");
    Check(IsExact(*FindDesc("lightvol"), 3) && !IsExact(*FindDesc("lightvol"), 4) && !IsExact(*FindDesc("lightvol"), 7), "lightvol: the cull and the groups (3) are exact, the LOD bit is not");
    Check(IsExact(*FindDesc("mirror"), 1) && !IsExact(*FindDesc("mirror"), 2), "mirror: skip outside the frustum (1) exact, one pass (2) not");
    Check(IsExact(*FindDesc("colshadow_clip"), 1) && !IsExact(*FindDesc("colshadow_clip"), 2), "colshadow_clip=1 exact");
    Check(!IsExact(*FindDesc("pb"), 1) && !IsExact(*FindDesc("cpulod"), 1) && !IsExact(*FindDesc("far_logic"), 2) && !IsExact(*FindDesc("hsr_order"), 1) && !IsExact(*FindDesc("s60_start"), 1),
          "none of pb, cpulod, far_logic, hsr_order, s60_start is exact (q30-26: cpulod and hsr_order are 'b', the rest sacrifices)");
    {   // q30-26: QualityValue, the three outcomes
        using port::stageknob::QualityValue;
        float o = -1;
        Check(QualityValue(*FindDesc("indcap_skip"), 2, &o) == 1 && o == 2, "Quality: an exact key is kept as it is");
        Check(QualityValue(*FindDesc("lightvol"), 3, &o) == 1 && o == 3 && QualityValue(*FindDesc("lightvol"), 7, &o) == 2 && o == 3 && QualityValue(*FindDesc("mirror"), 2, &o) == 2 && o == 1, "Quality: lightvol 3 kept, 7 (the LOD) lowered to the exact 3, mirror 2 to 1");
        Check(QualityValue(*FindDesc("pb"), 1, &o) == 0 && QualityValue(*FindDesc("far_draw"), 8, &o) == 0 && QualityValue(*FindDesc("ptcl_thin"), 0.5f, &o) == 0 && QualityValue(*FindDesc("star_far"), 40, &o) == 0 && QualityValue(*FindDesc("fxlow"), 1, &o) == 0 && QualityValue(*FindDesc("bloom_fold"), 1, &o) == 0 && QualityValue(*FindDesc("far_logic"), 1, &o) == 0 && QualityValue(*FindDesc("s60_848"), 1, &o) == 0,
              "Quality: the visible cuts stay Performance's (pb, far_draw, ptcl_thin, star_far, fxlow, bloom_fold, far_logic, s60_848)");
        Check(QualityValue(*FindDesc("cpulod"), 2, &o) == 2 && o == 1 && QualityValue(*FindDesc("cpulod"), 1, &o) == 1 && o == 1 && QualityValue(*FindDesc("cpulod"), 0, &o) == 1 && o == 0, "Quality: cpulod=2 / 3 is clamped to the q level 1, 1 and 0 as they are");
        Check(QualityValue(*FindDesc("glow26"), 2, &o) == 2 && o == 1 && QualityValue(*FindDesc("across"), 2, &o) == 2 && o == 1 && QualityValue(*FindDesc("fe_gov"), 3, &o) == 2 && o == 1 && QualityValue(*FindDesc("game_gov"), 3, &o) == 2 && o == 1,
              "Quality: the umbrellas glow26, across and the governors fe_gov, game_gov are clamped to level 1");
        Check(QualityValue(*FindDesc("matlod"), 4, &o) == 2 && o == 2 && QualityValue(*FindDesc("matlod_dist"), 1800, &o) == 2 && o == 3000 && QualityValue(*FindDesc("matlod_dist"), 5000, &o) == 1 && o == 5000,
              "Quality: matlod count clamped to 2 (down), its distance raised to 3000 (a distance is stronger when smaller)");
        Check(QualityValue(*FindDesc("hsr_ties"), 0, &o) == 2 && o == 4 && QualityValue(*FindDesc("hsr_ties"), 7, &o) == 1 && o == 7 && QualityValue(*FindDesc("hsr_order"), 3, &o) == 2 && o == 2 && QualityValue(*FindDesc("alphatest_prepass"), 1, &o) == 2 && o == 3,
              "Quality: hsr_ties 0 raised to the exact 4, 7 (the decal-aware rule) kept, hsr_order 3 (the alternating test aid) lowered to 2, alphatest_prepass 1 raised to the census 3");
        Check(QualityValue(*FindDesc("ind_flat"), 1, &o) == 0 && QualityValue(*FindDesc("ind_flat"), 2, &o) == 0, "Quality: ind_flat 1/2 stripped (keeps full indirect warp wobble in Quality 30)");
        Check(QualityValue(*FindDesc("clip_half"), 1, &o) == 0 && QualityValue(*FindDesc("clip_half"), 2, &o) == 0, "Quality: clip_half 1/2 stripped (avoids clamping perf-only 2 down to 1)");
        Check(QualityValue(*FindDesc("char_pose"), 3, &o) == 2 && o == 2 && QualityValue(*FindDesc("char_pose_lines"), 24, &o) == 2 && o == 12 && QualityValue(*FindDesc("char_pose_move"), 3, &o) == 2 && o == 1 && QualityValue(*FindDesc("char_pose_dist"), 2000, &o) == 2 && o == 2500,
              "Quality: the character pose rule is held to every 2nd turn, 12 lines, 1 line of movement, 2500 units");
    }

    Table* t = NewTable();
    Diag d;
    Parse(*t, "@class sac start=6 size=down fx=down edge=off cuts=1 pb=1 pb_decor=3500 cpulod=2 lightvol=7 mirror=2 indcap_skip=1 clip_skipempty=1 s60_start=2\n"
              "@class cheap class=sac fxlow=1\n"
              "StageA class=sac colshadow_clip=1 far_logic=2\n"
              "StageA mode=30 class=none2 hsr_ties=4\n"
              "StageA:2 class=sac start=4 floor=4 ceil=4 size=fixed\n"
              "StageB class=cheap\n"
              "StageC class=sac\n"
              "StageC mode=30 indcap_skip=0 fxlow=0\n"
              "* class=sac\n"
              "* mode=30 clearq=1\n"
              "@class none2 lightvol=3\n", d);
    Check(d.errors == 0, "modes: a file with mode=30 rows parses clean");
    Check(t->nRows == 8, "modes: a stage row and its mode=30 row are two rows");
    Spec a60, a30;
    Resolve(*t, "StageA", -1, a60);
    Check(a60.pictureMode == 60 && a60.nskip == 0 && !HasKey(a60, "hsr_ties") && HasKey(a60, "pb") && HasKey(a60, "far_logic") && a60.cuts == 1 && a60.edge == 1 && a60.size == kSizeDown,
          "60: the 30 row is not read, every key of the 60 row stays, nothing is skipped");
    Resolve(*t, "StageA", -1, a30, nullptr, 10, 30);
    Check(a30.pictureMode == 30, "30: the spec says so");
    Check(!HasKey(a30, "pb") && !HasKey(a30, "pb_decor") && !HasKey(a30, "far_logic") && !HasKey(a30, "s60_start") && SpecGet(a30, "mirror", 0) == 1, "30: the cuts are gone (pb, pb_decor, far_logic, s60_start); mirror=2 is lowered to the exact 1");
    Check(HasKey(a30, "cpulod") && SpecGet(a30, "cpulod", 0) == 1 && !Skipped(a30, "cpulod") && ClampedHas(a30, "cpulod=2>1") && ClampedHas(a30, "lightvol=7>3") && ClampedHas(a30, "mirror=2>1"), "30 (q30-26): cpulod=2 is kept, clamped to the q level 1, and named in the clamped list");
    Check(HasKey(a30, "indcap_skip") && HasKey(a30, "clip_skipempty") && HasKey(a30, "colshadow_clip"), "30: the exact keys stay (indcap_skip, clip_skipempty, colshadow_clip=1)");
    Check(SpecGet(a30, "lightvol", 0) == 3, "30: lightvol=7 is lowered to the exact 3");
    Check(SpecGet(a30, "lightvol", 0) == 3 && SpecGet(a30, "hsr_ties", 0) == 4, "30: the stage's mode=30 row (and its class) is applied (a 30 row is trusted whole)");
    Check(Skipped(a30, "pb") && !Skipped(a30, "cpulod") && Skipped(a30, "far_logic") && !Skipped(a30, "lightvol") && !Skipped(a30, "mirror") && Skipped(a30, "frame") && !Skipped(a30, "indcap_skip"),
          "30: the skipped list names the cuts and the frame, never an exact or a barely-noticeable key");
    Check(a30.start == 6 && a30.floorR == 6 && a30.ceil == 6 && a30.size == kSizeFixed && a30.fx == kFxHold && a30.edge == 0 && a30.cuts == 0 && a30.two == 1, "30: 960x544 fixed, effect steps held, the edge pass on, no half-rate captures");
    Check(std::strcmp(a30.cls, "none") != 0, "30: the class is not none (the controller gets its frame)");
    // a scenario row that asks for 848: the 30 mode still shows 960x544
    Spec a30s;
    Resolve(*t, "StageA", 2, a30s, nullptr, 10, 30);
    Check(a30s.start == 6 && a30s.floorR == 6 && Skipped(a30s, "frame"), "30: a scenario row's 848 is skipped (frame)");
    // a stage with only a 60 row: 30 gets its exact keys, plus the `*` mode=30 row only when the stage has none of its own
    Spec b30;
    Resolve(*t, "StageB", -1, b30, nullptr, 10, 30);
    Check(!HasKey(b30, "fxlow") && !HasKey(b30, "pb") && HasKey(b30, "indcap_skip") && SpecGet(b30, "clearq", 0) == 1 && b30.matched == 0, "30: a stage with no 30 row gets the default 30 row (clearq=1) after its exact keys");
    Spec c30;
    Resolve(*t, "StageC", -1, c30, nullptr, 10, 30);
    Check(SpecGet(c30, "indcap_skip", -1) == 0 && SpecGet(c30, "fxlow", -1) == 0 && !HasKey(c30, "clearq") && c30.matched == 1, "30: a stage's own 30 row replaces the default 30 row and its keys win (even for a cut-looking key: the row is trusted)");
    // an unknown stage: the default rows
    Spec u30, u60;
    Resolve(*t, "NoSuchGalaxy", -1, u30, nullptr, 10, 30);
    Resolve(*t, "NoSuchGalaxy", -1, u60);
    Check(!HasKey(u30, "pb") && HasKey(u30, "indcap_skip") && SpecGet(u30, "clearq", 0) == 1 && HasKey(u60, "pb") && !HasKey(u60, "clearq"), "default rows: 60 keeps its cuts, 30 keeps the exact ones and the 30 default row");
    // 60-mode resolution is bit-for-bit what it was without mode rows in the table (a table without any mode=30 row, resolved with the default mode argument)
    Table* plain = NewTable();
    Diag pd;
    Parse(*plain, "@class sac start=6 size=down pb=1 indcap_skip=1\nStageA class=sac\n* class=sac\n", pd);
    Spec p1, p2;
    Resolve(*plain, "StageA", -1, p1);
    Resolve(*plain, "StageA", -1, p2, nullptr, 10, 60);
    Check(std::memcmp(&p1.kv, &p2.kv, sizeof p1.kv) == 0 && p1.start == p2.start && p1.nkv == p2.nkv && p1.nskip == 0, "60: the explicit mode argument changes nothing");
    // the validation of the 30 spec: what ValidateKv lets through is exactly the exact keys
    Applied ap[kMaxKvSpec + 24];
    char bad[160];
    const int n = ValidateKv(c30, ap, kMaxKvSpec + 24, bad, sizeof bad);
    for (int i = 0; i < n; ++i) {
        const port::stageknob::Desc* dd = port::stageknob::FindDesc(ap[i].name);
        CHECKF(dd != nullptr, "applied key %s is known", ap[i].name);
    }
    // bad mode word
    Table* bm = NewTable();
    Diag bd;
    Parse(*bm, "StageX mode=45 pb=1\nStageY mode=30\n", bd);
    Check(bd.errors == 1, "mode=45 is refused with one message");
    // the built-in table, every stage, 30 mode: only exact keys, 960x544 fixed, nothing skipped that was exact
    Table* bt = NewTable();
    Diag bdg;
    LoadBuiltin(*bt, bdg);
    int rows = 0, cutsSeen = 0;
    for (int i = 0; i < bt->nRows; ++i) {
        if (bt->rows[i].mode != 0 || !std::strcmp(bt->rows[i].name, "*")) continue;
        Spec x;
        Resolve(*bt, bt->rows[i].name, bt->rows[i].scenario, x, nullptr, 10, 30);
        ++rows;
        bool allExact = true;
        for (int k = 0; k < x.nkv; ++k) { const Desc* dk = FindDesc(x.kv[k].key); allExact = allExact && dk && (IsExact(*dk, x.kv[k].val) || (dk->ex == 'b' && x.kv[k].val >= dk->bMin && x.kv[k].val <= dk->bMax)); }
        CHECKF(allExact && x.start == 6 && x.size == kSizeFixed && x.edge == 0 && x.cuts == 0 && x.fx == kFxHold, "built-in row %s in the 30 mode keeps only exact and barely-noticeable keys (within their range) at 960x544 fixed", bt->rows[i].name);
        cutsSeen += x.nskip;
    }
    std::printf("  %d built-in rows resolved for 30, %d cuts skipped in all\n", rows, cutsSeen);
    Check(rows > 5, "the built-in table has rows to test");
    // the log line
    char line[1500];
    FormatModeLine(a30, "StageA", -1, true, line, sizeof line);
    std::printf("  %s\n", line);
    Check(std::strstr(line, "mode: 30, stage StageA, preset keys applied: ") == line && std::strstr(line, "clamped for Quality: ") && std::strstr(line, "cpulod=2>1") && std::strstr(line, ", skipped for mode: ") && std::strstr(line, "pb") && std::strstr(line, "frame"), "log line: mode, stage, applied, clamped for Quality, skipped");
    FormatModeLine(a60, "StageA", 2, true, line, sizeof line);
    std::printf("  %s\n", line);
    Check(std::strstr(line, "mode: 60, stage StageA:2, preset keys applied: ") == line && std::strstr(line, "GXV_PB_DECOR=3500") && std::strstr(line, "clamped for Quality: none, skipped for mode: none"), "log line, 60: scenario, switch names and values, nothing skipped");
    FormatModeLine(a60, "StageA", -1, false, line, sizeof line);
    Check(std::strstr(line, "applied: none (GXV_STAGE_PRESETS=0)") != nullptr, "log line with the presets off says so");

    // ---- q30-26: the umbrellas' Quality levels, the silhouette rule, the env rewrite, glow26's level, the governors' budgets
    {
        namespace sk = port::stageknob;
        Table* uq = NewTable();
        Diag ud;
        Parse(*uq, "@class u1 cpulod=3 across=2 glow26=2 fe_gov=3 game_gov=2 fe_gov_ms=12 game_gov_ms=12 hsr_ties=0 hsr_order=3 hsr_auto=1 far_draw=8\n"
                   "@class u2 class=u1 ptcl_thin=0.5 piecethin=2 matlod=4 matlod_dist=1800 far_anim=3 tiny_cull=2 ind_flat=2 bloom_fold=2 fxlow=1\n"
                   "StageU class=u2\n"
                   "StageG silh_gate=1\n"
                   "StageH silh_gate=1 silh_occ=3\n", ud);
        Check(ud.errors == 0, "q30: the test table parses");
        Spec u60, u30;
        Resolve(*uq, "StageU", -1, u60);
        Resolve(*uq, "StageU", -1, u30, nullptr, 10, 30);
        Check(u30.nclamp >= 9, "q30: cpulod, across, glow26, fe_gov, game_gov, hsr_ties, hsr_order, matlod, matlod_dist, far_anim are clamped");
        for (const char* k : {"cpulod", "across", "glow26", "fe_gov", "game_gov", "hsr_ties", "hsr_order", "hsr_auto", "matlod", "matlod_dist", "far_anim"}) CHECKF(HasKey(u30, k), "q30: %s is kept in 30", k);
        for (const char* k : {"fe_gov_ms", "game_gov_ms", "far_draw", "ptcl_thin", "piecethin", "tiny_cull", "bloom_fold", "fxlow", "ind_flat"}) { CHECKF(!HasKey(u30, k), "q30: %s is left out of 30", k); CHECKF(Skipped(u30, k), "q30: %s is named as skipped", k); }
        Check(SpecGet(u30, "cpulod", 0) == 1 && SpecGet(u30, "across", 0) == 1 && SpecGet(u30, "glow26", 0) == 1 && SpecGet(u30, "fe_gov", 0) == 1 && SpecGet(u30, "game_gov", 0) == 1, "q30: every umbrella and governor is at level 1 in 30");
        Check(SpecGet(u30, "hsr_ties", 0) == 4 && SpecGet(u30, "hsr_order", 0) == 2 && SpecGet(u30, "matlod", 0) == 2 && SpecGet(u30, "matlod_dist", 0) == 3000 && SpecGet(u30, "far_anim", 0) == 2, "q30: levels are clamped into their ranges");
        Check(SpecGet(u60, "cpulod", 0) == 3 && SpecGet(u60, "fe_gov", 0) == 3 && SpecGet(u60, "matlod_dist", 0) == 1800 && u60.nclamp == 0, "q30: the 60 mode is untouched by all of it");
        // the validation of the 30 spec expands the umbrellas to their q members only
        Applied ap[kMaxKvSpec + 24];
        char bad[160];
        const int n = ValidateKv(u30, ap, kMaxKvSpec + 24, bad, sizeof bad);
        auto has = [&](const char* sw) { for (int i = 0; i < n; ++i) if (!std::strcmp(ap[i].name, sw)) return true; return false; };
        auto val = [&](const char* sw) { for (int i = 0; i < n; ++i) if (!std::strcmp(ap[i].name, sw)) return ap[i].val; return -1.0f; };
        Check(has("GXV_CHARPOSE") && val("GXV_CHARPOSE") == 2 && has("GXV_FAMPOSE_DIST") && !has("GXV_FARDRAW") && !has("GXV_PTCL_THIN") && !has("GXV_PIECETHIN") && !has("GXV_PB_ANIM"), "q30: cpulod=1 in 30 expands to the pose and material members only (no far draw, thinning, star-bit cut)");
        Check(has("GXV_SILH_OCC") && has("GXV_INDCAP_SKIP") && has("GXV_CLEARQ") && val("GXV_SILH_OCC") == 1, "q30: across=1 in 30 expands to the gate, the occlusion confirmation and the exact members");
        Spec g30, h30;
        Resolve(*uq, "StageG", -1, g30, nullptr, 10, 30);
        Check(!HasKey(g30, "silh_gate") && Skipped(g30, "silh_gate(no silh_occ)"), "q30: the silhouette gate on the collision rays alone is a sacrifice (Mario's outline)");
        Resolve(*uq, "StageH", -1, h30, nullptr, 10, 30);
        Check(HasKey(h30, "silh_gate") && SpecGet(h30, "silh_occ", 0) == 1 && ClampedHas(h30, "silh_occ=3>1"), "q30: the gate with its occlusion confirmation stays, the census level 3 lowered to 1");
        delete uq;
    }
    {   // the tables by mode
        namespace sk = port::stageknob;
        sk::SetQuality(false);
        float v = 0;
        Check(port::cpulod::TableValue(1, "GXV_FARDRAW", &v) && v == 8 && port::cpulod::TableValue(2, "GXV_MATLOD", &v) && v == 3, "q30: Performance table is as before");
        sk::SetQuality(true);
        Check(!port::cpulod::TableValue(1, "GXV_FARDRAW", &v) && !port::cpulod::TableValue(3, "GXV_PTCL_THIN", &v) && !port::cpulod::TableValue(2, "GXV_PIECETHIN", &v) && !port::cpulod::TableValue(3, "GXV_PB_LOD", &v), "q30: Quality: the visible cuts answer nothing at any level");
        Check(port::cpulod::TableValue(3, "GXV_MATLOD", &v) && v == 2 && port::cpulod::TableValue(2, "GXV_MATLOD_DIST", &v) && v == 3000 && port::cpulod::TableValue(1, "GXV_CHARPOSE_LINES", &v) && v == 12, "q30: Quality: the pose / material members answer their q value at any level");
        int qMembers = 0;
        for (int i = 0; i < port::cpulod::kMemberCount; ++i) if (port::cpulod::kMembers[i].q > 0.0f) ++qMembers;
        Check(qMembers == 7, "q30: the cpulod q level has seven members (charpose x2, matlod x2, fampose x3)");
        Check(port::across::TableValue(2, "GXV_SILH_OCC", &v) && v == 1 && port::across::TableValue(2, "GXV_INDCAP_SKIP", &v) && !port::across::TableValue(2, "GXV_SHADOW_STENCIL", &v) && !port::across::TableValue(2, "GXV_FIRSTCLEAR", &v) && !port::across::TableValue(2, "GXV_TAILDEPTH", &v), "q30: across q level: gate, occlusion, indcap_skip, clearq; not the stencil / first clear / tail depth");
        // glow26: Quality's level
        Check(port::glow26::ClampLevel(1) == port::glow26::kQ && port::glow26::ClampLevel(2) == port::glow26::kQ && port::glow26::ClampLevel(0) == 0, "q30: glow26 level 1 or 2 is the q level in Quality");
        port::glow26::Profile p0, pq = port::glow26::Apply(port::glow26::kQ, p0);
        Check(pq.autoMode == 0 && pq.ups == 0 && pq.defer == 0 && pq.indFlat == 1 && pq.bloomFold == 0 && (pq.lightvol & 7) == 3, "q30: glow26 q level: IND_FLAT=1 and the exact light-volume cull only (no compositor, no bloom fold, no LOD)");
        Check(port::glow26::BloomFoldFloor(port::glow26::kQ, 0) == 0 && port::glow26::LightVolFloor(port::glow26::kQ, 0) == 3 && port::glow26::IndFlatFloor(port::glow26::kQ, 0) == 1, "q30: the single-switch floors at the q level");
        sk::SetQuality(false);
        Check(port::glow26::ClampLevel(2) == 2 && port::glow26::Apply(2, p0).bloomFold == 2, "q30: glow26 in Performance is unchanged");
        // the env rewrite after the fan-out
        sk::SetQuality(false);
        for (int i = 0; i < port::cpulod::kMemberCount; ++i) port::cpulod::NoteFanned(i);
        for (int i = 0; i < port::across::kMemberCount; ++i) port::across::NoteFanned(i);
        std::string env[64][2];
        int ne = 0;
        unsetenv("GXV_PROTECT_SPRITES");
        char text[320];
        port::quality30::Applied() = false;
        const bool first = port::quality30::Apply([](const char* k) { return static_cast<const char*>(std::getenv(k)); }, [&](const char* k, const char* val2) { env[ne][0] = k; env[ne][1] = val2; ++ne; }, text, sizeof text);
        auto envOf = [&](const char* k) { for (int i = ne - 1; i >= 0; --i) if (env[i][0] == k) return env[i][1]; return std::string("?"); };
        Check(first && sk::Quality() && envOf("GXV_FARDRAW") == "0" && envOf("GXV_PTCL_THIN") == "0" && envOf("GXV_CHARPOSE") == "2" && envOf("GXV_CHARPOSE_LINES") == "12" && envOf("GXV_SILH_OCC") == "1" && envOf("GXV_SHADOW_STENCIL") == "0" && envOf("GXV_PROTECT_SPRITES") == "1",
              "q30: the env rewrite sets the q members, zeroes the others and shields the sprites");
        Check(!port::quality30::Apply([](const char*) { return static_cast<const char*>(nullptr); }, [&](const char*, const char*) {}, text, sizeof text), "q30: applied once");
        std::printf("  %s\n", text);
        sk::SetQuality(false);
        port::quality30::Applied() = false;
        // the governors' budgets
        Check(port::fegov::kBudget30Ms > 2.0f * 15.5f - 1.0f && port::fegov::kBudget30Ms < 33.3f && port::fegov::kGameBudget30Ms < port::fegov::kBudget30Ms, "q30: the governors' Quality budgets aim at 33.3 ms (front end 30.9, game thread 25.0 for the drawn turn)");
    }
    delete t; delete plain; delete bm; delete bt;
}
}  // namespace

// deadfix27: the S60 ladder's keys are taken out of a spec while GXV_SMOOTH60 is off, the others stay in order
void TestDropInertS60() {
    using namespace vgx::stagepreset;
    Spec s;
    std::memset(&s, 0, sizeof s);
    SpecKv(s, "clip_skipempty", 1.0f);
    SpecKv(s, "s60_848", 1.0f);
    SpecKv(s, "s60_30", 1.0f);
    SpecKv(s, "colshadow_clip", 1.0f);
    const int n = DropInertS60(s);
    Check(n == 2 && s.nkv == 2, "two s60 keys dropped, two kept");
    Check(SpecGet(s, "clip_skipempty", 0.0f) == 1.0f && SpecGet(s, "colshadow_clip", 0.0f) == 1.0f && SpecGet(s, "s60_848", -1.0f) == -1.0f, "the others stay");
    Check(s.nskip == 1 && DropInertS60(s) == 0, "one skip note, a second call drops nothing");
}


// ---- gpu896 (21 Nov): Performance steps to 896x512 when the GPU is the limit; Quality unchanged ----------------------------------------------------------------
// The switch is PerfGpu896() (the back end sets it from GXV_PERF_GPU896); off = every spec as before (checked against the whole built-in table).
Config Gpu896Config() {   // (the console's Performance controller: the effect steps on, no shed ladder: GXV_AUTOQ_SHED is 0 there)
    Config c = PerfConfig();
    c.shed = false;
    c.shedCuts = false;
    c.fxStep = true;
    return c;
}
Controller::StagePreset Gpu896Preset() {
    Controller::StagePreset p = Preset(6, 4, 6, false, false, false, false, 0);
    p.twoSize = false;
    p.ladder = true;
    return p;
}
double CpuBoundGpu(int rung) { return 4.0 + 3.0 * Cost(rung) / Cost(6); }      // the GPU is idle: 7 ms at 960x544 (the game thread is the limit)
// the ladder's levels as an index: (960,0) 0, (960,bg) 1, (960,bg+fx) 2, 896 3, 848 4, 848 with the edge smoothing off 5; -1 = not a level
int LadLevel(int rung, unsigned steps) {
    steps &= 7u;
    if (rung == 6) return steps == 0 ? 0 : steps == 2 ? 1 : steps == 3 ? 2 : -1;
    if (rung == 5) return steps == 3 ? 3 : -1;
    if (rung == 4) return steps == 3 ? 4 : steps == 7 ? 5 : -1;
    return -1;
}
// a GPU model: `k` ms of pixel work at 960x544 scaled by the pixels, 7 % off with the cheaper background, 12 % off with half-size effects, plus 3 ms of edge pass while it is on
double LadGpu(double k, int rung, unsigned steps) {
    return 3.0 + k * Cost(rung) / Cost(6) * ((steps & 2u) ? 0.93 : 1.0) * ((steps & 1u) ? 0.88 : 1.0) - ((steps & 4u) ? 3.0 : 0.0);
}
void TestGpu896() {
    std::printf("gpu896\n");
    Table* t = NewTable();
    Diag d;
    LoadBuiltin(*t, d);
    Spec off, on;
    // (a) off (the default): every row of the built-in table and the default row resolve exactly as before, Performance and Quality
    PerfGpu896() = false;
    int rows = 0;
    for (int i = 0; i < t->nRows; ++i) {
        if (t->rows[i].mode != 0) continue;
        ++rows;
        Resolve(*t, t->rows[i].name, -1, off);
        CHECKF(!off.perf896 && off.floorR == off.start && off.size != kSizeAuto, "off: row %s has no gpu896 change", t->rows[i].name);
    }
    Check(rows > 20, "the built-in table has its rows");
    // (b) on: the plain 960x544-fixed rows become 960 start / 896 floor / size down / two sizes / hud native; nothing else about them changes
    PerfGpu896() = true;
    int changed = 0, kept = 0;
    for (int i = 0; i < t->nRows; ++i) {
        if (t->rows[i].mode != 0) continue;
        PerfGpu896() = false;
        Resolve(*t, t->rows[i].name, t->rows[i].scenario, off);
        PerfGpu896() = true;
        Resolve(*t, t->rows[i].name, t->rows[i].scenario, on);
        const bool plain = off.start == 6 && off.floorR == 6 && off.ceil == 6 && off.size == kSizeFixed && off.fx == kFxDown && off.emergency == kNone;
        if (plain) {
            ++changed;
            CHECKF(on.perf896 && on.start == 6 && on.floorR == 4 && on.ceil == 6 && on.size == kSizeAuto && on.two == 0 && on.fx == kFxFree && on.edge == off.edge && on.cuts == off.cuts,
                   "on: %s is 960x544 with the Performance ladder 896 / 848 below it (both ways), the same edge and captures", t->rows[i].name);
            CHECKF(SpecGet(on, "hud_native", -1.0f) == 1.0f && on.nkv == off.nkv + (SpecGet(off, "hud_native", -1.0f) < 0.0f ? 1 : 0), "on: %s gets hud_native=1 and no other key changes", t->rows[i].name);
            CHECKF(on.floorR >= kShipFloorRung && !(on.flags & (kClampedFloor | kOrderFixed | kStartFixed)), "on: %s needs no correction and stays above the 848 floor", t->rows[i].name);
            CHECKF(on.floorR >= kShipFloorRung, "on: %s never goes under the 848x480 floor", t->rows[i].name);
        } else {
            ++kept;
            CHECKF(!on.perf896 && on.start == off.start && on.floorR == off.floorR && on.ceil == off.ceil && on.size == off.size && on.nkv == off.nkv,
                   "on: %s (not the plain frame: %s) is left exactly as it was", t->rows[i].name, off.cls);
        }
    }
    Check(changed > 20, "on: most stages take the rule");
    CHECKF(kept >= 0, "kept %d", kept);
    // lastresort (848 fixed, fx=hold) is not touched; an explicit row is not either
    {
        Table* u = NewTable();
        Diag e;
        Parse(*u, "@class base start=6 floor=6 ceil=6 size=fixed fx=down\n@class lastresort class=base start=4 floor=4 ceil=4 size=fixed fx=hold edge=off\n* class=base\n"
                  "Low class=lastresort\nNo896 class=base hud_native=0\nHeld class=base fx=hold\nDownRow class=base size=down floor=4\nHud class=base hud_native=2\n", e);
        PerfGpu896() = true;
        Spec s;
        Resolve(*u, "Low", -1, s);
        Check(!s.perf896 && s.start == 4 && s.floorR == 4 && s.size == kSizeFixed, "lastresort (848x480 fixed) is left alone");
        Resolve(*u, "Held", -1, s);
        Check(!s.perf896 && s.size == kSizeFixed && s.fx == kFxHold, "fx=hold (the preset is the whole answer): left alone");
        Resolve(*u, "DownRow", -1, s);
        Check(!s.perf896 && s.floorR == 4 && s.size == kSizeDown, "a row that already says size=down is the owner's own ladder: left alone");
        Resolve(*u, "No896", -1, s);
        Check(s.perf896 && SpecGet(s, "hud_native", -1.0f) == 0.0f, "a row's own hud_native wins");
        Resolve(*u, "Hud", -1, s);
        Check(s.perf896 && SpecGet(s, "hud_native", -1.0f) == 2.0f, "a row's own hud_native=2 wins");
        Resolve(*u, "Anything", -1, s);
        Check(s.perf896 && s.floorR == 4 && s.size == kSizeAuto, "the default row takes it");
        Resolve(*u, "Anything", -1, s, "base");   // (tours: a forced class is measured as it is)
        Check(!s.perf896 && s.floorR == 6 && s.size == kSizeFixed, "a forced class (tours) is not changed");
        delete u;
    }
    // (c) Quality: byte for byte the same with the switch on or off: 960x544 fixed, effects held, the edge pass on, every stage of the table
    {
        int same = 0;
        for (int i = 0; i < t->nRows; ++i) {
            if (t->rows[i].mode != 0) continue;
            Spec a, b;
            PerfGpu896() = false;
            Resolve(*t, t->rows[i].name, t->rows[i].scenario, a, nullptr, 10, kQuality);
            PerfGpu896() = true;
            Resolve(*t, t->rows[i].name, t->rows[i].scenario, b, nullptr, 10, kQuality);
            const bool eq = std::memcmp(&a, &b, sizeof(Spec)) == 0 || (a.start == b.start && a.floorR == b.floorR && a.ceil == b.ceil && a.size == b.size && a.fx == b.fx && a.edge == b.edge &&
                a.cuts == b.cuts && a.two == b.two && a.nkv == b.nkv && a.perf896 == b.perf896 && a.nskip == b.nskip && a.nclamp == b.nclamp);
            CHECKF(eq && !b.perf896 && b.start == 6 && b.floorR == 6 && b.ceil == 6 && b.size == kSizeFixed && b.fx == kFxHold && SpecGet(b, "hud_native", -1.0f) == SpecGet(a, "hud_native", -1.0f),
                   "Quality %s: identical with the switch on, 960x544 fixed, no hud_native added", t->rows[i].name);
            same += eq;
        }
        Check(same > 20, "Quality compared on the whole table");
    }
    // (d) the 848 / 896 / 30 modes of the smooth-60 ladder resolve as before (they take their own rungs)
    {
        Spec a, b;
        PerfGpu896() = false;
        ResolveMode(*t, "BigObsidianGalaxy", 1, 3, a);
        PerfGpu896() = true;
        ResolveMode(*t, "BigObsidianGalaxy", 1, 3, b);
        Check(b.start == 5 && b.floorR == 5 && b.ceil == 5 && b.size == kSizeFixed && a.start == b.start && a.size == b.size, "the 896 mode of GXV_SMOOTH60 is unchanged");
        ResolveMode(*t, "BigObsidianGalaxy", 1, 2, b);
        Check(b.start == 6 && b.floorR == 6 && b.size == kSizeFixed, "the even-30 mode of GXV_SMOOTH60 stays 960x544 fixed");
    }
    PerfGpu896() = false;
    delete t;
    // (e) the controller's Performance ladder: bg -> fx -> 896 -> 848 -> AA off, GPU-limited only; back up with room
    {
        Sim s(Gpu896Config());
        s.Warm();
        s.ctl.ApplyStagePreset(Gpu896Preset());
        Check(s.ctl.LadderActive(), "the ladder is on for the preset");
        s.Load(0x8960);
        Check(s.ctl.Current() == 6 && s.ctl.Steps() == 0, "a stage load starts at 960x544 with every step off");
        // a boss that needs everything: 36 ms at the top (3 ms of it the edge pass)
        std::vector<int> seq;
        int last = -1;
        double firstUnder[6] = {-1, -1, -1, -1, -1, -1};
        const uint64_t t0 = s.now;
        for (int i = 0; i < 400; ++i) {
            const double g = LadGpu(20.0, s.ctl.Current(), s.ctl.Steps());
            s.Frame(g, 0x8960, false);
            const int lv = LadLevel(s.ctl.Current(), s.ctl.Steps());
            if (lv != last) { seq.push_back(lv); last = lv; if (lv >= 0 && firstUnder[lv] < 0) firstUnder[lv] = static_cast<double>(s.now - t0) / 1e6; }
        }
        bool mono = true;
        for (size_t i = 1; i < seq.size(); ++i) mono = mono && seq[i] > seq[i - 1];
        CHECKF(mono && !seq.empty() && seq.back() == 5, "a GPU-limited boss walks the ladder in order, never back and forth, to the bottom (levels: %zu changes, last %d)", seq.size(), seq.empty() ? -1 : seq.back());
        CHECKF(s.ctl.Current() == 4 && (s.ctl.Steps() & 4u) == 4u && (s.ctl.Steps() & 3u) == 3u, "bottom: 848x480, cheaper background and half-size effects on, edge smoothing off (rung %d steps %u)", s.ctl.Current(), s.ctl.Steps());
        bool saw5ahead = true;
        for (size_t i = 0; i < seq.size(); ++i) if (seq[i] >= 3 && i == 0) saw5ahead = false;
        Check(saw5ahead, "the first change is never below a cheaper picture's level: 896x512 / 848x480 / edge never come first");
        CHECKF(s.ctl.LadderDowns() >= 1 && s.ctl.LadderDowns() <= 5, "at most one change a level (%u downs)", s.ctl.LadderDowns());
        // every change is a ladder change (the back end fades the edge step and takes a size change from the made-ahead targets; see SeamUrgent)
        Check(s.ctl.LastChange().why == Why::LadderDown, "the last size change is a ladder step down");
        // recovery: the boss is over; a light stage. Back to the full picture quickly, edge smoothing first, never hunting
        const uint64_t tr = s.now;
        std::vector<int> up;
        last = LadLevel(s.ctl.Current(), s.ctl.Steps());
        up.push_back(last);
        for (int i = 0; i < 1200 && LadLevel(s.ctl.Current(), s.ctl.Steps()) != 0; ++i) {
            s.Frame(LadGpu(9.0, s.ctl.Current(), s.ctl.Steps()), 0x8960, false);
            const int lv = LadLevel(s.ctl.Current(), s.ctl.Steps());
            if (lv != last) { up.push_back(lv); last = lv; }
        }
        const double rec = static_cast<double>(s.now - tr) / 1e6;
        bool down = true;
        for (size_t i = 1; i < up.size(); ++i) down = down && up[i] < up[i - 1];
        CHECKF(LadLevel(s.ctl.Current(), s.ctl.Steps()) == 0 && down, "recovery: back to 960x544 with every step off, in order (%zu changes)", up.size());
        CHECKF(rec <= 6.0, "recovery is quick: %.1f s from the bottom to the full picture (2 s hold, one change)", rec);
        Check(s.ctl.LastChange().why == Why::LadderUp, "the way up is a ladder step");
    }
    // (e1b) a boss that needs the ladder step by step (20 ms at the top): every level on the way is visited in order, 896 before 848 before the edge smoothing
    {
        Sim s(Gpu896Config());
        s.Warm();
        s.ctl.ApplyStagePreset(Gpu896Preset());
        s.Load(0x8967);
        std::vector<int> seq;
        int last = 0;
        for (int i = 0; i < 1500; ++i) {
            s.Frame(LadGpu(21.0, s.ctl.Current(), s.ctl.Steps()), 0x8967, false);
            const int lv = LadLevel(s.ctl.Current(), s.ctl.Steps());
            if (lv != last) { seq.push_back(lv); last = lv; }
        }
        bool mono = !seq.empty() && seq[0] > 0;
        for (size_t i = 1; i < seq.size(); ++i) mono = mono && seq[i] > seq[i - 1];
        Check(mono && seq.size() >= 2, "a boss the ladder has to walk: levels in order, no level is revisited");
    }
    // (e2) a mild overload (19 ms at the top) stops where the GPU fits: a cheaper picture / 896 at most, never the edge smoothing, never 848
    {
        Sim s(Gpu896Config());
        s.Warm();
        s.ctl.ApplyStagePreset(Gpu896Preset());
        s.Load(0x8964);
        int lowest = 6;
        unsigned steps = 0;
        for (int i = 0; i < 1500; ++i) {
            s.Frame(LadGpu(14.5, s.ctl.Current(), s.ctl.Steps()), 0x8964, false);
            lowest = std::min(lowest, s.ctl.Current());
            steps |= s.ctl.Steps();
        }
        CHECKF(lowest >= 5 && !(steps & 4u), "a mild GPU overload costs a cheaper picture and at most 896x512 (lowest rung %d, steps seen %u)", lowest, steps);
        CHECKF(s.ctl.Current() >= 5 && s.ctl.LadderDowns() >= 1, "and it holds there (rung %d)", s.ctl.Current());
    }
    // (e3) no hunting on a noisy trace at the threshold: 5 minutes, the GPU near the budget with +-1.5 ms of noise
    {
        Sim s(Gpu896Config());
        s.Warm();
        s.ctl.ApplyStagePreset(Gpu896Preset());
        s.Load(0x8965);
        std::mt19937 rng(7);
        std::normal_distribution<double> noise(0.0, 1.0);
        unsigned changes = 0;
        int last = 0;
        for (int i = 0; i < 18000; ++i) {
            const double g = LadGpu(15.5, s.ctl.Current(), s.ctl.Steps()) + 1.5 * noise(rng);
            s.Frame(std::max(2.0, g), 0x8965, false);
            const int lv = LadLevel(s.ctl.Current(), s.ctl.Steps());
            if (lv != last) { ++changes; last = lv; }
        }
        CHECKF(changes <= 8, "a noisy GPU at the threshold does not hunt: %u level changes in 5 minutes", changes);
    }
    // (e4) a failed step up (the GPU got heavier right after it) is blocked: the next try waits (the doubling block)
    {
        Sim s(Gpu896Config());
        s.Warm();
        s.ctl.ApplyStagePreset(Gpu896Preset());
        s.Load(0x8966);
        for (int i = 0; i < 400; ++i) s.Frame(LadGpu(20.0, s.ctl.Current(), s.ctl.Steps()), 0x8966, false);   // down to the bottom
        unsigned ups = 0;
        int flips = 0, prev = LadLevel(s.ctl.Current(), s.ctl.Steps());
        // the stage looks light at the bottom, heavy one level up: every climb fails
        for (int i = 0; i < 12000; ++i) {
            const int lv = LadLevel(s.ctl.Current(), s.ctl.Steps());
            const double g = lv >= 5 ? 7.0 : 24.0;   // (edge smoothing on costs the stage its budget)
            s.Frame(g, 0x8966, false);
            const int nl = LadLevel(s.ctl.Current(), s.ctl.Steps());
            if (nl != prev) { ++flips; prev = nl; }
        }
        ups = s.ctl.LadderUps();
        CHECKF(ups >= 1 && ups <= 6 && flips <= 14, "a climb that fails is backed off: %u climbs, %d level changes in 3.3 minutes", ups, flips);
    }
    // (e5) "don't sacrifice graphics if it doesn't help": a stage whose GPU time does not scale (fixed cost, 22 ms at every size: TA / many scenes) keeps no size step and no edge step
    {
        Sim s(Gpu896Config());
        s.Warm();
        s.ctl.ApplyStagePreset(Gpu896Preset());
        s.Load(0x8990);
        for (int i = 0; i < 600; ++i) s.Frame(22.0, 0x8990, false);
        const unsigned undone = s.ctl.LadderUndone();
        CHECKF(s.ctl.Current() == 6 && !(s.ctl.Steps() & 4u), "a fixed-cost stage ends at 960x544 with the edge smoothing on (rung %d steps %u)", s.ctl.Current(), s.ctl.Steps());
        CHECKF(undone >= 1 || s.ctl.LadderRefused() != 0, "the steps were proved useless and refused (%u undone, mask %x)", undone, s.ctl.LadderRefused());
        s.changesInPlay = 0;
        const unsigned dn = s.ctl.LadderDowns();
        for (int i = 0; i < 6000; ++i) s.Frame(22.0, 0x8990, false);
        CHECKF(s.ctl.Current() == 6 && s.changesInPlay == 0 && s.ctl.LadderDowns() <= dn + 1, "and not retried for 100 s: no hunting (%u changes in play)", s.changesInPlay);
        // a revisit of the same stage remembers it
        s.ctl.ApplyStagePreset(Gpu896Preset());
        s.Load(0x8990);
        const unsigned undone2 = s.ctl.LadderUndone();
        for (int i = 0; i < 1200; ++i) s.Frame(22.0, 0x8990, false);
        CHECKF(s.ctl.Current() == 6 && !(s.ctl.Steps() & 4u) && s.ctl.LadderUndone() == undone2, "a revisit does not try the refused steps again (rung %d)", s.ctl.Current());
    }
    // (e6) a pixel-bound stage keeps its step (and the proof says so)
    {
        Sim s(Gpu896Config());
        s.Warm();
        s.ctl.ApplyStagePreset(Gpu896Preset());
        s.Load(0x8991);
        for (int i = 0; i < 1500; ++i) s.Frame(LadGpu(20.0, s.ctl.Current(), s.ctl.Steps()), 0x8991, false);
        CHECKF(s.ctl.Current() <= 5 && s.ctl.LadderUndone() == 0, "a pixel-bound stage keeps its smaller picture (rung %d, %u undone)", s.ctl.Current(), s.ctl.LadderUndone());
    }
    // (f) a CPU / game-thread-limited stage (the GPU idle, or even busy but under the budget) never gives anything up
    for (int variant = 0; variant < 2; ++variant) {
        Sim s(Gpu896Config());
        s.Warm();
        s.ctl.ApplyStagePreset(Gpu896Preset());
        s.Load(0x8970 + static_cast<uint32_t>(variant));
        s.changesInPlay = 0;
        const uint64_t end = s.now + 120000000ull;
        while (s.now < end) s.Frame(variant ? 12.0 : CpuBoundGpu(s.ctl.Current()), 0x8970 + static_cast<uint32_t>(variant), false, 33366);   // (30 fps pictures: the game thread's)
        CHECKF(s.ctl.Current() == 6 && s.ctl.Steps() == 0 && s.changesInPlay == 0 && s.ctl.LadderDowns() == 0,
               "a CPU-limited stage (GPU %s) takes no step of the ladder in 2 minutes (rung %d, steps %u)", variant ? "12 ms" : "7 ms", s.ctl.Current(), s.ctl.Steps());
    }
    // (f2) a stage whose frames are long for another reason with the GPU gate closed (GPU busy 30 % of the picture) takes nothing either
    {
        Config c = Gpu896Config();
        c.gpuGate = true;
        Sim s(c);
        s.Warm();
        s.ctl.ApplyStagePreset(Gpu896Preset());
        s.Load(0x8972);
        const uint64_t end = s.now + 60000000ull;
        while (s.now < end) s.Frame(9.0, 0x8972, false, 40000);
        CHECKF(s.ctl.Current() == 6 && s.ctl.LadderDowns() == 0, "long pictures with a GPU that is not the limit: nothing given up (rung %d)", s.ctl.Current());
    }
    // (g) Quality's frame (960x544 fixed) holds whatever the load
    {
        Sim s(Gpu896Config());
        s.Warm();
        s.ctl.ApplyStagePreset(Preset(6, 6, 6, true, false, false, false, 2));
        s.Load(0x8980);
        s.changesInPlay = 0;
        s.Play(120, WorseGpu, 0x8980);
        CHECKF(s.ctl.Current() == 6 && s.changesInPlay == 0, "a fixed-960 frame (Quality's) does not move under a heavy boss (rung %d)", s.ctl.Current());
    }
}


// ---- halfglow38 (GXV_PERF_HALFGLOW): the ladder's "half-rate glow + shadow texture" step: before the first size, proved or undone, GPU-limited only ----------------
Config GlowConfig(bool on = true) { Config c = Gpu896Config(); c.glowStep = on; return c; }
// a continuous model (the picture interval follows the GPU time, as on the console): `fixed` ms that nothing scales, `pix` ms that scale with the pixels, the glow step takes `glowSave` of the total
double GlowGpu(const Controller& c, double fixedMs, double pixMs, double glowSave) {
    return (fixedMs + pixMs * Cost(c.Current()) / Cost(6)) * (c.HalfGlowOn() ? 1.0 - glowSave : 1.0);
}
void GlowRun(Sim& s, uint32_t scene, int frames, double fixedMs, double pixMs, double glowSave, std::vector<int>* sizeAtGlow = nullptr) {
    bool was = s.ctl.HalfGlowOn();
    for (int i = 0; i < frames; ++i) {
        const double g = GlowGpu(s.ctl, fixedMs, pixMs, glowSave);
        s.Frame(g, scene, false, static_cast<uint64_t>(g * 1000.0));
        if (sizeAtGlow && s.ctl.HalfGlowOn() != was) { was = s.ctl.HalfGlowOn(); if (was) sizeAtGlow->push_back(s.ctl.Current()); }
    }
}
void TestHalfGlow() {
    std::printf("halfglow\n");
    // (a) off (Config::glowStep false), or not allowed by the back end (HRW pairs under GXV_HRW_CAPFIX, a knob already on): never taken, the ladder is the one it was
    for (int variant = 0; variant < 3; ++variant) {
        Sim s(GlowConfig(variant != 0));
        s.Warm();
        s.ctl.ApplyStagePreset(Gpu896Preset());
        s.ctl.SetHalfGlowAllowed(variant == 1 ? false : true);
        if (variant == 2) s.ctl.SetHalfGlowAllowed(false);
        s.Load(0x7100 + static_cast<uint32_t>(variant));
        unsigned glowOn = 0;
        for (int i = 0; i < 1500; ++i) {
            const double g = GlowGpu(s.ctl, 22.0, 0.0, 0.08);
            s.Frame(g, 0x7100 + static_cast<uint32_t>(variant), false, static_cast<uint64_t>(g * 1000.0));
            glowOn += s.ctl.HalfGlowOn() ? 1u : 0u;
        }
        CHECKF(glowOn == 0, "variant %d (%s): the glow step is never taken", variant, variant == 0 ? "switch off" : variant == 1 ? "HRW pairing / knob already on" : "not allowed");
    }
    // (b) a GPU-limited stage where pixels do not pay (Beach Bowl: 24 ms, all fixed) and the glow gives 8 %: the glow step is taken BEFORE any smaller size, kept, and the sizes are then refused
    {
        Sim s(GlowConfig());
        s.Warm();
        s.ctl.ApplyStagePreset(Gpu896Preset());
        s.ctl.SetHalfGlowAllowed(true);
        s.Load(0x7110);
        std::vector<int> rungAtGlow;
        GlowRun(s, 0x7110, 1500, 24.0, 0.0, 0.08, &rungAtGlow);
        CHECKF(!rungAtGlow.empty() && rungAtGlow[0] == 6, "the glow step comes at 960x544, before the first size (rung %d)", rungAtGlow.empty() ? -1 : rungAtGlow[0]);
        CHECKF(s.ctl.HalfGlowOn(), "and it is kept: it bought %.1f ms", 24.0 * 0.08);
        CHECKF(!(s.ctl.LadderRefused() & 0x200u), "kept, not refused (mask %x)", s.ctl.LadderRefused());
        CHECKF(s.ctl.Current() == 6 && !(s.ctl.Steps() & 4u), "the sizes and the edge step buy nothing there and stay off (rung %d)", s.ctl.Current());
    }
    // (c) the glow buys nothing (no glow in the stage): taken on trial, undone, refused for the stage, not retried; a pixel-bound stage then goes on down the sizes
    {
        Sim s(GlowConfig());
        s.Warm();
        s.ctl.ApplyStagePreset(Gpu896Preset());
        s.ctl.SetHalfGlowAllowed(true);
        s.Load(0x7111);
        unsigned onFrames = 0;
        for (int i = 0; i < 1800; ++i) {
            const double g = GlowGpu(s.ctl, 3.0, 20.0, 0.0);
            s.Frame(g, 0x7111, false, static_cast<uint64_t>(g * 1000.0));
            onFrames += s.ctl.HalfGlowOn() ? 1u : 0u;
        }
        CHECKF(onFrames > 0 && !s.ctl.HalfGlowOn(), "the glow step was tried (%u pictures) and is off again", onFrames);
        CHECKF((s.ctl.LadderRefused() & 0x200u) != 0 && s.ctl.LadderUndone() >= 1, "undone and refused for the stage (mask %x, %u undone)", s.ctl.LadderRefused(), s.ctl.LadderUndone());
        CHECKF(s.ctl.Current() < 6, "the pixel-bound stage went on to a smaller size (rung %d)", s.ctl.Current());
        // not retried for the rest of the stage
        unsigned again = 0;
        for (int i = 0; i < 6000; ++i) {
            const double g = GlowGpu(s.ctl, 3.0, 20.0, 0.0);
            s.Frame(g, 0x7111, false, static_cast<uint64_t>(g * 1000.0));
            again += s.ctl.HalfGlowOn() ? 1u : 0u;
        }
        CHECKF(again == 0, "and not taken again for 100 s (%u pictures)", again);
        // a revisit remembers it
        s.ctl.ApplyStagePreset(Gpu896Preset());
        s.ctl.SetHalfGlowAllowed(true);
        s.Load(0x7111);
        unsigned revisit = 0;
        for (int i = 0; i < 1500; ++i) {
            const double g = GlowGpu(s.ctl, 3.0, 20.0, 0.0);
            s.Frame(g, 0x7111, false, static_cast<uint64_t>(g * 1000.0));
            revisit += s.ctl.HalfGlowOn() ? 1u : 0u;
        }
        CHECKF(revisit == 0, "a revisit does not try it again (%u pictures)", revisit);
    }
    // (d) a CPU / game-thread-limited stage never takes it (7 ms of GPU in 30 fps pictures; 12 ms in long pictures)
    for (int variant = 0; variant < 2; ++variant) {
        Sim s(GlowConfig());
        s.Warm();
        s.ctl.ApplyStagePreset(Gpu896Preset());
        s.ctl.SetHalfGlowAllowed(true);
        s.Load(0x7120 + static_cast<uint32_t>(variant));
        unsigned on = 0;
        const uint64_t end = s.now + 90000000ull;
        while (s.now < end) {
            s.Frame(variant ? 12.0 : 7.0, 0x7120 + static_cast<uint32_t>(variant), false, 33366);
            on += s.ctl.HalfGlowOn() ? 1u : 0u;
        }
        CHECKF(on == 0 && s.ctl.LadderDowns() == 0, "a CPU-limited stage (GPU %s) never takes the glow step (%u pictures, %u downs)", variant ? "12 ms" : "7 ms", on, s.ctl.LadderDowns());
    }
    // (e) the way back: with the GPU light again the glow step is taken back, in order (after the sizes)
    {
        Sim s(GlowConfig());
        s.Warm();
        s.ctl.ApplyStagePreset(Gpu896Preset());
        s.ctl.SetHalfGlowAllowed(true);
        s.Load(0x7130);
        GlowRun(s, 0x7130, 1500, 24.0, 0.0, 0.08);
        CHECKF(s.ctl.HalfGlowOn(), "taken at 24 ms");
        GlowRun(s, 0x7130, 1500, 6.0, 0.0, 0.08);   // the heavy part is over
        CHECKF(!s.ctl.HalfGlowOn() && s.ctl.Current() == 6, "back to the full glow with room (glow %d rung %d)", s.ctl.HalfGlowOn() ? 1 : 0, s.ctl.Current());
    }
    // (f) the allowance withdrawn in play (60 Assist engages: pairs): the step is taken back at once
    {
        Sim s(GlowConfig());
        s.Warm();
        s.ctl.ApplyStagePreset(Gpu896Preset());
        s.ctl.SetHalfGlowAllowed(true);
        s.Load(0x7131);
        GlowRun(s, 0x7131, 1500, 24.0, 0.0, 0.08);
        CHECKF(s.ctl.HalfGlowOn(), "taken");
        s.ctl.SetHalfGlowAllowed(false);
        GlowRun(s, 0x7131, 30, 24.0, 0.0, 0.08);
        CHECKF(!s.ctl.HalfGlowOn(), "withdrawn: off at the next window");
    }
    // (g) a new stage starts with it off
    {
        Sim s(GlowConfig());
        s.Warm();
        s.ctl.ApplyStagePreset(Gpu896Preset());
        s.ctl.SetHalfGlowAllowed(true);
        s.Load(0x7132);
        GlowRun(s, 0x7132, 1500, 24.0, 0.0, 0.08);
        CHECKF(s.ctl.HalfGlowOn(), "taken");
        s.ctl.ApplyStagePreset(Gpu896Preset());
        CHECKF(!s.ctl.HalfGlowOn(), "a stage load clears it");
    }
}

void TestQualityMsaaAndMelty() {
    std::printf("quality msaa and melty\n");
    Table* t = NewTable();
    Diag d;
    LoadBuiltin(*t, d);
    Check(d.errors == 0, "builtin parses cleanly");
    // Melty Molten carries Observatory L3 draw cuts in Performance and strips them in Quality
    Spec pm, qm;
    QualityMsaa() = false;
    Resolve(*t, "HellProminenceGalaxy", 1, pm, nullptr, 10, kPerformance);
    Resolve(*t, "HellProminenceGalaxy", 1, qm, nullptr, 10, kQuality);
    Check(std::fabs(KvOf(pm, "pb_lod") - 0.3f) < 1e-4f && std::fabs(KvOf(pm, "pb_decor") - 3000.0f) < 1e-4f, "Melty Molten Performance L3 cuts");
    Check(KvOf(qm, "pb_lod") < -1e8f && KvOf(qm, "pb_decor") < -1e8f, "Melty Molten Quality strips sacrifices");
    Check(qm.start == kTopRung && qm.floorR == kTopRung && qm.ceil == kTopRung && qm.size == kSizeFixed, "QualityMsaa off stays rung 6 fixed");
    // QualityMsaa on: Quality gets rung 7..6 auto; Performance stays rung 6 fixed
    QualityMsaa() = true;
    Spec pm2, qm2;
    Resolve(*t, "HellProminenceGalaxy", 1, pm2, nullptr, 10, kPerformance);
    Resolve(*t, "HellProminenceGalaxy", 1, qm2, nullptr, 10, kQuality);
    Check(pm2.start == kTopRung && pm2.floorR == kTopRung && pm2.ceil == kTopRung && pm2.size == kSizeFixed, "Performance unaffected by QualityMsaa");
    Check(qm2.start == kMsaa2Rung && qm2.floorR == kTopRung && qm2.ceil == kMsaa2Rung && qm2.size == kSizeAuto, "QualityMsaa on sets rung 7..6 auto");
    QualityMsaa() = false;
    delete t;
}

int main() {
    TestDropInertS60();
    setenv("GXV_WIDE_RUNGS", "1", 1);
    TestParser();
    TestResolve();
    TestValidate();
    TestKnobs();
    TestController();
    TestModes();
    TestGpu896();
    TestHalfGlow();
    TestQualityMsaaAndMelty();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
