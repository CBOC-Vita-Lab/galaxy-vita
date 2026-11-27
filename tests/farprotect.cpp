// far49 host test (tools/test_farprotect.sh): port/farprotect.h and the objective flag of port/farlogic.h's Classify.
#include <cstdio>
#include "port/farprotect.h"
#include "port/farlogic.h"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
using port::farprotect::Facts;
using port::farprotect::Protected;
int main() {
    // the chased penguin of Freezeflame: a PenguinSkater, NPC list, sensor, rail, switch, camera
    Facts f; f.name = "PenguinSkater"; f.sensors = true; f.rail = true; f.stageSwitch = true; f.camCtrl = true; f.npcList = true;
    CHECK(Protected(f));
    // by name alone (a penguin with nothing else the engine marks, in no list yet)
    Facts n; n.name = "PenguinRacer"; CHECK(Protected(n));
    n.name = "Kinopio"; CHECK(Protected(n));
    // a rail-follower, an NPC with a sensor, a pointer target
    Facts r; r.name = "SomeWalker"; r.sensors = true; r.rail = true; CHECK(Protected(r));
    Facts t; t.name = "SomeNpc"; t.sensors = true; t.npcList = true; CHECK(Protected(t));
    Facts p; p.name = "SomeThing"; p.sensors = true; p.pointer = true; CHECK(Protected(p));
    // what keeps its cuts: plain decoration, sensor-less NPC-list models, marked families, a sensor actor in no way special
    Facts d; d.name = "TreeDecoration"; d.npcList = true; CHECK(!Protected(d));
    Facts s; s.name = "SomeEnemy"; s.sensors = true; CHECK(!Protected(s));
    Facts b; b.name = "StarPiece"; b.sensors = true; b.pointer = true; b.family = 1; CHECK(!Protected(b));
    Facts k; k.name = "KoteBug"; k.sensors = true; k.rail = true; k.family = 3; CHECK(!Protected(k));  // (a family keeps the family rules)
    // far logic: an objective is never ambient / mob, with or without a family
    port::farlogic::Facts q; q.hasModel = true; q.family = port::farlogic::kFamKoteBug; q.objective = true;
    port::farlogic::Why w; CHECK(port::farlogic::Classify(q, &w) == port::farlogic::kNever && w == port::farlogic::kNvBound);
    q.objective = false; CHECK(port::farlogic::Classify(q, &w) == port::farlogic::kMob);
    q.family = 0; CHECK(port::farlogic::Classify(q, &w) == port::farlogic::kAmbient);
    q.objective = true; CHECK(port::farlogic::Classify(q, &w) == port::farlogic::kNever);
    std::printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
