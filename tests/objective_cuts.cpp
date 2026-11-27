// prog32b host test (tools/test_objective_cuts.sh): no CPU / draw cut may take an actor that a star can depend on.
//
// What is checked, against the real headers (port/cpucuts.h, port/farlogic.h) and the real decomp sources:
//  1. Sensor types. Every ATYPE_* value of Game/Util/ActorSensorUtil.hpp is run through cpucuts::ClassOfSensorType: only the plain NPC / animal, Enemy range
//     (without the launcher) and MapObj / MapObjSimple kinds are takeable; every objective kind (power star, scenario starter, switches, star bits, coins, morph
//     items, bosses' body parts, Koopa plates, ...) is kSensorKeep, with every possible GXV_FARDRAW_KINDS mask.
//  2. Actor classes. Every decomp source's MR::addHitSensor* calls give the sensor kinds an actor class carries (the same table the game reads at run time).
//     The classes that a star can depend on (a hand-kept list of the ones the audit found with takeable sensor kinds: switches, bosses, bunnies, penguins, Toads,
//     Rosalina, Lumas, cages) must come out as class 0 ("never") through ClassOfActorGuarded, under every mask 0..31; with the guard switched off (the 3.0
//     candidate's behaviour) most of them were takeable: that is the negative control, the test fails if it finds nothing to protect.
//  3. The engine's own signals: a stage switch, a camera controller, a star-pointer target (any kind) or a rail (a sensor actor) make an actor never-taken.
//  4. Far logic (farlogic::Classify): the same facts, with and without a listed family, are never "ambient" or "mob".
//  5. Coverage: every decomp actor class whose name contains "Switch" or starts with "Boss" is protected (a new class added later is caught here).
// Build and run: bash tools/test_objective_cuts.sh
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "port/cpucuts.h"
#include "port/farlogic.h"

#ifndef DECOMP_ROOT
#error "DECOMP_ROOT"
#endif
namespace fs = std::filesystem;
using namespace port::cpucuts;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::string Slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

int main() {
    const fs::path root = DECOMP_ROOT;
    // ---- 1. sensor types ---------------------------------------------------------------------------------------------------------------------
    std::map<std::string, uint32_t> atype;
    {
        const std::string h = Slurp(root / "include/Game/Util/ActorSensorUtil.hpp");
        std::regex re(R"((ATYPE_\w+)\s*=\s*(0x[0-9A-Fa-f]+|[0-9]+))");
        for (auto it = std::sregex_iterator(h.begin(), h.end(), re); it != std::sregex_iterator(); ++it) atype[(*it)[1]] = static_cast<uint32_t>(std::stoul((*it)[2], nullptr, 0));
    }
    CHECK(atype.size() > 100, "parsed %zu sensor types", atype.size());
    int takeable = 0;
    for (const auto& kv : atype) {
        const uint8_t c = ClassOfSensorType(kv.second);
        const bool take = c != kSensorKeep;
        takeable += take;
        const std::string& n = kv.first;
        const bool objectiveType = n == "ATYPE_POWER_STAR_BIND" || n == "ATYPE_SCENARIO_STARTER_BIND" || n == "ATYPE_SWITCH" || n == "ATYPE_SWITCH_BIND" ||
            n == "ATYPE_KEY_SWITCH_AVOID" || n == "ATYPE_STAR_PIECE" || n == "ATYPE_COIN" || n == "ATYPE_COIN_RED" || n == "ATYPE_MORPH_ITEM" ||
            n == "ATYPE_KINOKO_ONEUP" || n == "ATYPE_GOLF_CUP" || n == "ATYPE_BREAKABLE_CAGE" || n == "ATYPE_BEE_FLOWER" || n == "ATYPE_PUNCH_BOX" ||
            n == "ATYPE_ICEJUMP_WALL" || n == "ATYPE_KAMECK_BARRIER" || n == "ATYPE_SWITCH_MESSENGER" || n == "ATYPE_MESSAGE_SENSOR" || n == "ATYPE_DEMO" ||
            n.rfind("ATYPE_KOOPA_", 0) == 0 ||
            n.rfind("ATYPE_TOMB_SPIDER", 0) == 0 || n.rfind("ATYPE_SAND_GOLEM", 0) == 0 || n.rfind("ATYPE_TRIPODBOSS", 0) == 0 || n.rfind("ATYPE_PLAYER", 0) == 0 ||
            n.find("_BIND") != std::string::npos || n == "ATYPE_RECEIVER" || n == "ATYPE_PUSH";
        if (objectiveType) CHECK(!take, "objective sensor type %s (0x%X) is takeable", n.c_str(), kv.second);
        for (unsigned mask = 0; mask <= kCbAll; ++mask)
            if (!take) CHECK(!Allowed(ClassOfActor(2, c), mask), "%s allowed under mask %u", n.c_str(), mask);
    }
    CHECK(takeable > 20 && takeable < 60, "takeable sensor types: %d (the Enemy range + NPC + animal + 2 MapObj kinds)", takeable);

    // ---- 2. actor classes from the decomp ----------------------------------------------------------------------------------------------------
    const std::map<std::string, uint32_t> helper = {
        {"addHitSensorEnemy", 0x14}, {"addHitSensorEnemySimple", 0x15}, {"addHitSensorEnemyAttack", 0x16}, {"addHitSensorNpc", 5}, {"addHitSensorMapObj", 0x46},
        {"addHitSensorMapObjSimple", 0x47}, {"addHitSensorMtxEnemy", 0x14}, {"addHitSensorMtxEnemyAttack", 0x16}, {"addHitSensorMtxNpc", 5},
        {"addHitSensorMtxMapObj", 0x46}, {"addHitSensorPosMapObj", 0x46}, {"addHitSensorMtxAnimal", 7}, {"addHitSensorPosEye", 0x7F}, {"addHitSensorEye", 0x7F},
        {"addHitSensorMapObjMoveCollision", 0x48}, {"addHitSensorMapObjPress", 0x76}, {"addHitSensorBinder", 0x61}, {"addHitSensorRide", 9}, {"addHitSensorMtxRide", 9},
        {"addHitSensorPush", 0x80}, {"addHitSensorPosBinder", 0x61}, {"addHitSensorPosRide", 9}, {"addHitSensorTransferableBinder", 0x62}, {"addHitSensorPriorBinder", 0x63}};
    std::map<std::string, std::vector<uint32_t>> classes;  // actor class (file stem) -> sensor types (unknown helper / type: 0xFFFF, a kind the cut never takes)
    for (auto& e : fs::recursive_directory_iterator(root / "src")) {
        if (e.path().extension() != ".cpp") continue;
        const std::string s = Slurp(e.path());
        std::regex call(R"(MR::(addHitSensor\w*)\(([^;]*?)\);)");
        std::vector<uint32_t> ts;
        for (auto it = std::sregex_iterator(s.begin(), s.end(), call); it != std::sregex_iterator(); ++it) {
            const std::string h = (*it)[1], args = (*it)[2];
            auto hv = helper.find(h);
            if (hv != helper.end()) { ts.push_back(hv->second); continue; }
            std::smatch m;
            if (std::regex_search(args, m, std::regex(R"((ATYPE_\w+))")) && atype.count(m[1])) ts.push_back(atype[m[1]]);
            else ts.push_back(0xFFFF);
        }
        if (!ts.empty()) classes[e.path().stem().string()] = ts;
    }
    CHECK(classes.size() > 200, "actor classes with sensors: %zu", classes.size());
    const char* const objective[] = {"KeySwitch", "HipDropSwitch", "HipDropTimerSwitch", "CrystalSwitch", "SwitchBox", "PunchBox", "CrystalCage", "BossBegoman", "BossKameck",
                                     "Kameck", "TripodBossLeg", "Kinopio", "Penguin", "Rabbit", "Rosetta", "RosettaPictureBook", "StrayTico", "TicoRail", "Teresa",
                                     "SnowplowSwitch", "DesertLandMoveSwitch"};
    int unguardedBefore = 0, found = 0;
    for (const char* name : objective) {
        auto it = classes.find(name);
        if (it == classes.end()) continue;  // (a class a later decomp change renamed: the coverage check of part 5 still runs on what exists)
        ++found;
        const uint8_t bits = ClassOfSensors(it->second.data(), static_cast<int>(it->second.size()));
        const uint8_t before = ClassOfActorGuarded(2, bits, false, false, name, false);
        const uint8_t after = ClassOfActorGuarded(2, bits, false, false, name, true);
        unguardedBefore += before != 0;
        CHECK(after == 0, "%s (sensor bits 0x%X) is still takeable with the guard", name, bits);
        for (unsigned mask = 0; mask <= kCbAll; ++mask) CHECK(!Allowed(after, mask), "%s allowed under mask %u", name, mask);
    }
    CHECK(found >= 15, "only %d of the listed objective classes exist in the decomp", found);
    CHECK(unguardedBefore >= 10, "negative control: only %d of the objective classes were takeable without the guard (the test protects nothing)", unguardedBefore);

    // ---- 3. the engine's own signals ---------------------------------------------------------------------------------------------------------
    const uint32_t enemy[] = {0x14}, mapobj[] = {0x46}, npc[] = {5};
    for (const uint32_t* t : {enemy, mapobj, npc}) {
        const uint8_t bits = ClassOfSensors(t, 1);
        CHECK(ClassOfActorGuarded(2, bits, false, false, "Plain") == bits, "a plain actor stays takeable (the lever keeps its saving)");
        CHECK(ClassOfActorGuarded(2, bits, true, false, "Plain") == 0, "stage switch / camera / pointer");
        CHECK(ClassOfActorGuarded(2, bits, false, true, "Plain") == 0, "rail on a sensor actor");
    }
    CHECK(ClassOfActorGuarded(4, 0, true, false, "Deco") == 0, "decoration with a stage switch / camera controller / pointer target");
    CHECK(ClassOfActorGuarded(4, 0, false, true, "Deco") == kCbDecorFx, "decoration on a rail is still a speck");
    CHECK(ClassOfActorGuarded(3, 0, false, false, "Deco") == kCbDecor, "pure decoration unchanged");
    for (int kind : {0, 1, 5}) CHECK(ClassOfActorGuarded(kind, kCbEnemy, false, false, "X") == 0, "plant / collision / no model are never taken");

    // ---- 4. far logic -------------------------------------------------------------------------------------------------------------------------
    using namespace port::farlogic;
    for (uint8_t fam = 0; fam < kFamN; ++fam) {
        Facts f; f.hasModel = true; f.sensors = true; f.family = fam;
        Facts a = f; a.stageSwitch = true; Facts b = f; b.rail = true; Facts c = f; c.camCtrl = true; Facts d = f; d.collision = true;
        CHECK(Classify(a, nullptr) == kNever && Classify(b, nullptr) == kNever && Classify(c, nullptr) == kNever && Classify(d, nullptr) == kNever, "farlogic family %d with a bound / collision part", fam);
    }
    {
        Facts f; f.hasModel = true; f.sensors = true;  // an enemy / NPC / switch with a sensor and no listed family
        CHECK(Classify(f, nullptr) == kNever, "farlogic: unlisted sensor actors (bosses, switches, NPCs, enemies) are never touched");
    }

    // ---- 5. coverage -------------------------------------------------------------------------------------------------------------------------
    int covered = 0;
    for (const auto& kv : classes) {
        const std::string& name = kv.first;
        if (name.find("Switch") == std::string::npos && name.rfind("Boss", 0) != 0) continue;
        const uint8_t bits = ClassOfSensors(kv.second.data(), static_cast<int>(kv.second.size()));
        ++covered;
        CHECK(ClassOfActorGuarded(2, bits, false, false, name.c_str()) == 0, "%s (a switch or boss class) is takeable", name.c_str());
    }
    CHECK(covered >= 8, "switch / boss classes seen: %d", covered);
    std::printf("objective_cuts: %zu actor classes, %d sensor types (%d takeable), %d objective classes checked, %d of them takeable without the guard, %d switch/boss classes: %s\n",
                classes.size(), static_cast<int>(atype.size()), takeable, found, unguardedBefore, covered, fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
