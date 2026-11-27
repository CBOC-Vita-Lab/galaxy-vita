// beefix25 (11 Nov): the Bee power-up entry rule on the host, for Mario and for Luigi (after the hand-off note of community PR #34, "Validation
// still needed" 2): both archives missing, either missing, each partial pointer state, late arrival, Bee entry / exit / re-entry, and the eyelid
// texture bounds. The decisions are the real functions of platform/include/port/player_assets.h (portPlayerModeMissing, portBeeRetryWanted,
// portBeeArchiveName, portBeeEyelidFirst, portBeeEyelidFits); the actor around them (Sim) mirrors MarioActor::initBeeMario / canEnterPlayerMode /
// setPlayerMode / MorphItemObjNeo::receiveOtherMsg step for step (tests/test_player_audit.py checks that the game source still has that shape).
//   tools/test_bee_late_init.sh
#include "port/player_assets.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

struct Sim {
    bool luigi = false;
    bool retryOn = false;                  // GXV_POWERUP_RETRY
    std::set<std::string> loaded;          // archives MR::isLoadedObjectOrLayoutArchive reports
    int d[4] = {};
    const void *bee = nullptr, *wing = nullptr, *body2 = nullptr, *fur = nullptr;  // _9E4 _9E8 mModels[2] _9EC
    unsigned mode = 0;
    int retriesLeft = 2;
    bool swapTextureRan = true;            // _B64 / _B6C exist (swapTextureInit ran at the stage start)
    int ctor = 0, lateTail = 0, kills = 0;

    PortPlayerAssets assets() const {
        static int dummy[32];
        PortPlayerAssets a; std::memset(&a, 0, sizeof a);
        a.fireBall[0] = &dummy[0]; a.fireBall[1] = &dummy[1]; a.fireBall[2] = &dummy[2]; a.foo = &dummy[3];
        a.bee = bee; a.beeWing = wing; a.body[2] = body2; a.beeFur = fur;
        return a;
    }
    bool has(const char* n) const { return loaded.count(n) != 0; }
    void initBeeMario() {                  // MarioActorDraw.cpp
        if (bee != nullptr || wing != nullptr) return;
        if (!has(portBeeArchiveName(luigi))) return;
        if (!has("BeeMarioWing")) return;
        ctor++;
        bee = &d[0]; body2 = &d[1]; fur = &d[2]; wing = &d[3];
        if (swapTextureRan) lateTail++;    // (initBeeTextureSwap + the hand joints; at the stage start swapTextureInit / initHand do it)
    }
    void stageStart() { bool keep = swapTextureRan; swapTextureRan = false; initBeeMario(); swapTextureRan = keep; }
    bool canEnter(unsigned m) {            // MarioActorMorph.cpp
        if (mode == m) return true;
        if (m == 4 && !bee && !wing && !body2 && retriesLeft > 0 && retryOn) {  // (the game code's cheap pre-check; the decision is portBeeRetryWanted)
            if (portBeeRetryWanted(assets(), has(portBeeArchiveName(luigi)), has("BeeMarioWing"), retriesLeft, true)) {
                retriesLeft--;
                initBeeMario();
            }
        }
        return portPlayerModeMissing(m, assets()) == nullptr;
    }
    void setPlayerMode(unsigned m) {
        if (mode == m) return;
        if (m == 4 && (!bee || !wing)) return;       // the 2ff45ac refusal
        if (!canEnter(m)) return;
        mode = m;
    }
    // MorphItemObjNeo::receiveOtherMsg + MarioAccess::changeItemStatus: one item object; true = it was used up
    struct Item { bool alive = true; };
    bool touch(Item& it, int itemCode) {
        if (!it.alive) return false;
        const int m = portPlayerItemMode(itemCode);
        if (m > 0 && !canEnter((unsigned)m)) return false;   // MR::canChangePlayerItemStatus: untouched
        setPlayerMode((unsigned)m);
        it.alive = false;
        return true;
    }
};

static void forBoth(void (*f)(bool)) { f(false); f(true); }

static void archivesMissing(bool luigi) {
    for (int retry = 0; retry < 2; retry++) {
        // both missing; only the body; only the wings; the OTHER player's body with the wings (a Mario archive for Luigi and the reverse)
        struct Case { const char* a; const char* b; };
        const char* other = portBeeArchiveName(!luigi);
        const char* own = portBeeArchiveName(luigi);
        const Case cases[] = {{nullptr, nullptr}, {own, nullptr}, {nullptr, "BeeMarioWing"}, {other, "BeeMarioWing"}, {other, nullptr}};
        for (const Case& c : cases) {
            Sim s; s.luigi = luigi; s.retryOn = retry;
            if (c.a) s.loaded.insert(c.a);
            if (c.b) s.loaded.insert(c.b);
            s.stageStart();
            CHECK(s.bee == nullptr && s.wing == nullptr && s.ctor == 0);
            Sim::Item it;
            for (int t = 0; t < 300; t++) CHECK(!s.touch(it, 2));   // standing on the mushroom
            CHECK(it.alive && s.mode == 0 && s.ctor == 0);
            CHECK(s.retriesLeft == 2);       // no try is spent while an archive is missing
        }
    }
}

static void partialPointers(bool luigi) {
    // every subset of {_9E4, _9E8, mModels[2], _9EC} except none and all: never retried (nothing is rebuilt over a half), always refused,
    // the item stays and the player stays as he was. Both archives loaded and the retry on: the worst case for a retry.
    for (unsigned mask = 1; mask < 15; mask++) {
        Sim s; s.luigi = luigi; s.retryOn = true;
        s.loaded = {portBeeArchiveName(luigi), "BeeMarioWing"};
        if (mask & 1) s.bee = &s.d[0];
        if (mask & 2) s.wing = &s.d[1];
        if (mask & 4) s.body2 = &s.d[2];
        if (mask & 8) s.fur = &s.d[3];
        Sim::Item it;
        CHECK(!s.touch(it, 2));
        CHECK(it.alive && s.mode == 0 && s.ctor == 0 && s.retriesLeft == 2);
        s.setPlayerMode(4);                  // a direct request (nothing asks canEnter first): refused as well
        CHECK(s.mode == 0);
    }
    // the same with all four present: accepted
    Sim s; s.luigi = luigi; s.bee = &s.d[0]; s.wing = &s.d[1]; s.body2 = &s.d[2]; s.fur = &s.d[3];
    Sim::Item it; CHECK(s.touch(it, 2) && s.mode == 4 && !it.alive);
}

static void lateArrival(bool luigi) {
    // the archives are not loaded when the stage starts and are when the mushroom is touched
    for (int retry = 0; retry < 2; retry++) {
        Sim s; s.luigi = luigi; s.retryOn = retry;
        s.stageStart();
        Sim::Item it;
        CHECK(!s.touch(it, 2) && it.alive);
        s.loaded = {portBeeArchiveName(luigi), "BeeMarioWing"};
        if (!retry) {
            for (int t = 0; t < 100; t++) CHECK(!s.touch(it, 2));    // default: the mushroom stays, nothing is built
            CHECK(it.alive && s.mode == 0 && s.ctor == 0);
        } else {
            CHECK(s.touch(it, 2));
            CHECK(!it.alive && s.mode == 4 && s.ctor == 1 && s.lateTail == 1 && s.retriesLeft == 1);
        }
    }
    // the wrong player's archive never satisfies the retry
    Sim s; s.luigi = luigi; s.retryOn = true;
    s.loaded = {portBeeArchiveName(!luigi), "BeeMarioWing"};
    Sim::Item it; CHECK(!s.touch(it, 2) && s.ctor == 0 && s.retriesLeft == 2);
}

static void entryExitReentry(bool luigi) {
    for (int retry = 0; retry < 2; retry++) {
        for (int late = 0; late < 2; late++) {
            if (late && !retry) continue;
            Sim s; s.luigi = luigi; s.retryOn = retry;
            if (!late) { s.loaded = {portBeeArchiveName(luigi), "BeeMarioWing"}; s.stageStart(); CHECK(s.ctor == 1 && s.lateTail == 0); }
            else { s.stageStart(); s.loaded = {portBeeArchiveName(luigi), "BeeMarioWing"}; }
            for (int round = 0; round < 3; round++) {
                Sim::Item it;
                CHECK(s.touch(it, 2));                 // entry (or re-entry)
                CHECK(s.mode == 4 && !it.alive);
                // already Bee: a second mushroom is accepted (timer refresh), no second build
                Sim::Item it2; CHECK(s.touch(it2, 2) && s.mode == 4);
                s.setPlayerMode(0);                    // damage / cure: exit
                CHECK(s.mode == 0);
                s.initBeeMario();                      // nothing may build a second pair over the first
            }
            CHECK(s.ctor == 1);
            CHECK(s.lateTail == (late ? 1 : 0));
        }
    }
}

static void eyelid() {
    CHECK(portBeeEyelidFirst(7, 3) == 7 && portBeeEyelidFirst(0, 3) == 0 && portBeeEyelidFirst(-1, 3) == 3);
    CHECK(portBeeEyelidFits(0, 4, 4));
    CHECK(portBeeEyelidFits(8, 12, 12));
    CHECK(!portBeeEyelidFits(9, 12, 12));      // 9..12: one past the table
    CHECK(!portBeeEyelidFits(0, 3, 12));       // the Bee has only 3 textures
    CHECK(!portBeeEyelidFits(0, 12, 3));       // the scratch table is shorter than the Bee's (allocated from the base body's count)
    CHECK(!portBeeEyelidFits(3, 20, 6));       // the community case: Bee larger than the base table
    CHECK(!portBeeEyelidFits(5, 4, 12));       // first beyond the Bee's table
    CHECK(!portBeeEyelidFits(0xFFFF, 0xFFFF, 0xFFFF) && !portBeeEyelidFits(0xFFFFFFFFu, 4, 4));
    CHECK(portBeeArchiveName(false)[3] == 'M' && portBeeArchiveName(true)[3] == 'L');
}

int main() {
    forBoth(archivesMissing);
    forBoth(partialPointers);
    forBoth(lateArrival);
    forBoth(entryExitReentry);
    eyelid();
    std::printf(fails ? "FAILED %d\n" : "PASS bee_late_init (Mario and Luigi)\n", fails);
    return fails ? 1 : 0;
}
