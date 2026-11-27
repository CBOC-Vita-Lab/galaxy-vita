// luigi24: portPlayerModeMissing (platform/include/port/player_assets.h): MarioActor::setPlayerMode refuses a mode whose optional assets
// are missing. Checks every mode against every single missing asset, the stage that has only some items, and that the modes that need no
// asset (normal, 8, tornado) are never refused. crash24: fire needs its three fireballs and flying needs Mario::mFoo, which every real actor has
// (the helper bare() is such an actor with no item assets).
#include "port/player_assets.h"
#include <cstdio>
#include <cstring>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static PortPlayerAssets full() {
    static int dummy[32];
    PortPlayerAssets a;
    for (int i = 0; i < 6; i++) a.body[i] = &dummy[i];
    a.bee = &dummy[6]; a.beeWing = &dummy[7]; a.beeFur = &dummy[8];
    a.ice = &dummy[9]; a.iceHandL = &dummy[10]; a.iceHandR = &dummy[11]; a.iceStep = &dummy[12];
    a.rainbow = &dummy[13]; a.rainbowHandL = &dummy[14]; a.rainbowHandR = &dummy[15];
    a.spring = &dummy[16]; a.boo = &dummy[17]; a.booAnim = &dummy[18];
    a.fireBall[0] = &dummy[19]; a.fireBall[1] = &dummy[20]; a.fireBall[2] = &dummy[21]; a.foo = &dummy[22];
    return a;
}
// a real actor of a stage that placed no power-up item: no item assets, but the fireballs and the flying state every actor has
static PortPlayerAssets bare() {
    static int dummy[4];
    PortPlayerAssets a; std::memset(&a, 0, sizeof a);
    a.fireBall[0] = &dummy[0]; a.fireBall[1] = &dummy[1]; a.fireBall[2] = &dummy[2]; a.foo = &dummy[3];
    return a;
}

int main() {
    // everything present: nothing refused
    { PortPlayerAssets a = full(); for (unsigned m = 0; m < 16; m++) CHECK(portPlayerModeMissing(m, a) == nullptr); }
    // nothing present (a stage without any item): only the five asset modes refused, the others still enterable
    {
        PortPlayerAssets a = bare();
        for (unsigned m = 0; m < 16; m++) {
            const bool needs = m == 1 || m == 3 || m == 4 || m == 5 || m == 6;
            CHECK((portPlayerModeMissing(m, a) != nullptr) == needs);
        }
    }
    // each single missing asset refuses exactly the mode(s) that use it, and names it
    struct One { const void* PortPlayerAssets::*p; int body; unsigned mode; const char* word; };
    const One ones[] = {
        {&PortPlayerAssets::bee, -1, 4, "_9E4"}, {&PortPlayerAssets::beeWing, -1, 4, "_9E8"}, {&PortPlayerAssets::beeFur, -1, 4, "_9EC"},
        {&PortPlayerAssets::ice, -1, 3, "_9C0"}, {&PortPlayerAssets::iceHandL, -1, 3, "_A40"}, {&PortPlayerAssets::iceHandR, -1, 3, "_A44"},
        {&PortPlayerAssets::iceStep, -1, 3, "_B4C"}, {&PortPlayerAssets::rainbow, -1, 1, "_9C8"}, {&PortPlayerAssets::rainbowHandL, -1, 1, "_A50"},
        {&PortPlayerAssets::rainbowHandR, -1, 1, "_A54"}, {&PortPlayerAssets::spring, -1, 5, "_A00"}, {&PortPlayerAssets::boo, -1, 6, "_9A4"},
        {&PortPlayerAssets::booAnim, -1, 6, "_9B8"},
        {&PortPlayerAssets::foo, -1, 7, "mFoo"},
        {nullptr, 2, 4, "mModels[2]"}, {nullptr, 3, 3, "mModels[3]"}, {nullptr, 4, 1, "mModels[4]"}, {nullptr, 5, 5, "mModels[5]"},
    };
    for (const One& o : ones) {
        PortPlayerAssets a = full();
        if (o.p) a.*(o.p) = nullptr; else a.body[o.body] = nullptr;
        for (unsigned m = 0; m < 16; m++) {
            const char* why = portPlayerModeMissing(m, a);
            if (m == o.mode) { CHECK(why != nullptr); CHECK(why && std::strstr(why, o.word)); }
            else CHECK(why == nullptr);
        }
    }
    // fire: any one of its three fireballs missing refuses it (and only it)
    for (int i = 0; i < 3; i++) {
        PortPlayerAssets a = full(); a.fireBall[i] = nullptr;
        for (unsigned m = 0; m < 16; m++) {
            const char* why = portPlayerModeMissing(m, a);
            if (m == 2) { CHECK(why && std::strstr(why, "_B54")); } else CHECK(why == nullptr);
        }
    }
    // body 0 (the normal body) and body 1 (bone) are not optional assets of any mode here
    { PortPlayerAssets a = full(); a.body[0] = nullptr; a.body[1] = nullptr; for (unsigned m = 0; m < 16; m++) CHECK(portPlayerModeMissing(m, a) == nullptr); }
    // Honeyhive-like: only the bee archives loaded
    {
        PortPlayerAssets a = bare();
        static int d[8]; a.bee = &d[0]; a.beeWing = &d[1]; a.beeFur = &d[2]; a.body[2] = &d[3];
        CHECK(portPlayerModeMissing(4, a) == nullptr);
        CHECK(portPlayerModeMissing(1, a) && portPlayerModeMissing(3, a) && portPlayerModeMissing(5, a) && portPlayerModeMissing(6, a));
        CHECK(portPlayerModeMissing(0, a) == nullptr && portPlayerModeMissing(2, a) == nullptr && portPlayerModeMissing(7, a) == nullptr);
    }

    // relfix24: item codes -> modes (MarioAccess::changeItemStatus), and the pickup rule: a refused change leaves the item
    {
        const int want[11] = {0, 5, 4, 6, 3, 2, 7, 1, 0, -1, -1};  // codes 0..10
        for (int i = 0; i < 11; i++) CHECK(portPlayerItemMode(i) == want[i]);
        CHECK(portPlayerItemMode(-1) == -1 && portPlayerItemMode(11) == -1 && portPlayerItemMode(1000) == -1);
        // every item code whose mode needs an asset is the one portPlayerModeMissing refuses
        PortPlayerAssets none = bare();
        PortPlayerAssets all = full();
        for (int i = 0; i < 11; i++) {
            const int m = portPlayerItemMode(i);
            const bool needs = m == 1 || m == 3 || m == 4 || m == 5 || m == 6;
            CHECK(portPlayerItemAccepted(i, 0, all));
            CHECK(portPlayerItemAccepted(i, 0, none) == !needs);
            // already in that mode: accepted even with no assets (timer refresh)
            if (m >= 0) CHECK(portPlayerItemAccepted(i, (unsigned)m, none));
        }
    }
    // a model of the item: the pickup consumes the item only when the change is accepted, and does nothing at all otherwise
    {
        struct Sim { bool alive = true; int sounds = 0, demos = 0, kills = 0;
            bool touch(int item, unsigned cur, const PortPlayerAssets& a) {
                if (!portPlayerItemAccepted(item, cur, a)) return false;  // refused: untouched
                sounds++; demos++; kills++; alive = false; return true; } };
        PortPlayerAssets a = bare();
        Sim bee;
        for (int t = 0; t < 600; t++) CHECK(!bee.touch(2, 0, a));  // 10 s of standing on it, models missing
        CHECK(bee.alive && bee.sounds == 0 && bee.demos == 0 && bee.kills == 0);
        // the models arrive (the retry of initBeeMario, or a later load): the next touch succeeds, exactly once
        static int d[4]; a.bee = &d[0]; a.beeWing = &d[1]; a.beeFur = &d[2]; a.body[2] = &d[3];
        CHECK(bee.touch(2, 0, a));
        CHECK(!bee.alive && bee.sounds == 1 && bee.demos == 1 && bee.kills == 1);
        // success path unchanged: with all assets every item is accepted as before
        PortPlayerAssets all = full();
        for (int i = 0; i < 11; i++) { Sim s; CHECK(s.touch(i, 0, all) && !s.alive); }
        // the rainbow star and the other forms are refused independently
        Sim star, ice; CHECK(!star.touch(7, 0, a) && star.alive); CHECK(!ice.touch(4, 0, a) && ice.alive);
    }
    std::printf(fails ? "FAILED %d\n" : "PASS player_assets\n", fails);
    return fails ? 1 : 0;
}
