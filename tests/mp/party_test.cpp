// Host tests for the party strip and the team star logic (owner: celebrate): decomp/include/Game/MP/MpPartyLogic.hpp (header only).
// TALLY codec (fuzzed), the local counters, the ledger and the TEAM STAR decision, the strip states and the star flash, the strip model.
//
// Build and run (from the worktree root):
//   g++ -std=c++20 -fshort-wchar -fsanitize=address,undefined -g -Wall -Wextra -Wno-unused-parameter -Idecomp/include -Iplatform/include
//       tests/mp/party_test.cpp -o /tmp/party_test && /tmp/party_test
#include <stdlib.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "Game/MP/MpPartyLogic.hpp"

static int gFails = 0, gChecks = 0;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace MpPartyLogic;

static void testCodec() {
    uint8_t b[16];
    MpWriter w(b, sizeof b);
    writeTally(w, Tally{0x1234, 321, 4567, 9, 1});
    CHECK(w.ok() && w.n == kTallyBytes);
    Tally t;
    CHECK(readTally(b, w.n, &t) && t.serial == 0x1234 && t.starBits == 321 && t.coins == 4567 && t.helped == 9 && t.nearStar == 1);
    // exact length only, limits, flags
    CHECK(!readTally(b, w.n - 1, &t) && !readTally(b, w.n + 1, &t) && !readTally(b, 0, &t));
    auto bad = [&](int at, int v, int width) {
        uint8_t c[16];
        memcpy(c, b, 8);
        c[at] = (uint8_t)v;
        if (width == 2) c[at + 1] = (uint8_t)(v >> 8);
        Tally x;
        return readTally(c, 8, &x);
    };
    CHECK(!bad(2, 10000, 2) && bad(2, 9999, 2));    // star bits
    CHECK(!bad(4, 10000, 2) && bad(4, 9999, 2));    // coins
    CHECK(!bad(6, 100, 1) && bad(6, 99, 1));        // helped
    CHECK(!bad(7, 2, 1) && bad(7, 0, 1));           // near
    // fuzz: whatever comes in, an accepted tally is inside the limits and re-encodes to the same bytes
    std::mt19937 rng(7);
    int accepted = 0;
    for (int i = 0; i < 300000; i++) {
        uint8_t c[12];
        for (auto& x : c) x = (uint8_t)rng();
        if (rng() % 3 == 0) c[3] &= 0x1F, c[5] &= 0x1F;   // (make the small ones likelier)
        if (rng() % 3 == 0) c[6] %= 120, c[7] %= 3;
        const size_t len = 6 + rng() % 5;
        Tally x;
        if (!readTally(c, len, &x)) continue;
        accepted++;
        CHECK(len == kTallyBytes && x.starBits <= kMaxCount && x.coins <= kMaxCount && x.helped <= kMaxHelped && x.nearStar <= 1);
        uint8_t o[16];
        MpWriter ow(o, sizeof o);
        writeTally(ow, x);
        CHECK(ow.n == 8 && memcmp(o, c, 8) == 0);
    }
    fprintf(stderr, "  tally fuzz: %d of 300000 accepted\n", accepted);
    // the wire contract: relayed by the host, sendable by guests, delivered to the game (0x20..0x5F), after the bubble ids 0x3B / 0x3C
    CHECK(MP_MSG_TALLY == 0x3D && MP_MSG_TALLY >= MP_MSG_GAME_FIRST && MP_MSG_TALLY <= MP_MSG_GAME_LAST);
    CHECK(mp_msg_host_relays(MP_MSG_TALLY) && !mp_msg_host_only(MP_MSG_TALLY));
}

static void testCounter() {
    Counter c;
    c.reset();
    c.observe(10, 100);                  // baseline: nothing counted for what the player already had
    CHECK(c.coins == 0 && c.bits == 0);
    c.observe(13, 100);
    c.observe(13, 104);
    CHECK(c.coins == 3 && c.bits == 4);
    c.observe(0, 90);                    // a reset coin counter, star bits fed to a Luma: nothing lost
    CHECK(c.coins == 3 && c.bits == 4);
    c.observe(2, 91);                    // counts again from the new level
    CHECK(c.coins == 5 && c.bits == 5);
    c.observe(-1, -1);                   // (no data this turn)
    CHECK(c.coins == 5 && c.bits == 5);
    c.observe(1000000, 5);
    CHECK(c.coins == kMaxCount);
    for (int i = 0; i < 200; i++) c.help();
    CHECK(c.helped == kMaxHelped);
    const Tally t = c.tally(9, true);
    CHECK(t.serial == 9 && t.coins == kMaxCount && t.helped == kMaxHelped && t.nearStar == 1);
    c.reset();
    CHECK(c.coins == 0 && c.bits == 0 && c.helped == 0 && c.lastCoins == -1);
}

static void testNear() {
    const float a[3] = {0, 0, 0}, b[3] = {kNearRadius - 1, 0, 0}, c[3] = {kNearRadius + 1, 0, 0}, d[3] = {2000, 2000, 2000};
    CHECK(isNear(a, b) && !isNear(a, c) && isNear(a, d) && isNear(a, a));
    const float nan[3] = {NAN, 0, 0}, inf[3] = {INFINITY, 0, 0}, huge[3] = {1e7f, 0, 0};
    CHECK(!isNear(a, nan) && !isNear(a, inf) && !isNear(a, huge));
    CHECK(nearDistance(a, nan) < 0.0f && nearDistance(a, a) == 0.0f && nearDistance(a, b) == kNearRadius - 1);
    // fix3: Good Egg (cooked stage data, world coordinates): the star and where a teammate stands
    const float dinoStar[3] = {-8400, -6374, -37800}, dinoFarSide[3] = {-8400, -8674, -37800};     // the other pole of its planet
    const float fortress[3] = {8300, -11373, -35900};                                               // the planet before it
    const float kalienteStar[3] = {29899, 22718, -32923}, bathRim[3] = {28699, 23544, -38273};      // an arena corner of the bath
    const float egg5Star[3] = {-19824, -15732, -27890}, eggD[3] = {-25250, -15500, -22850};         // the next egg planet's centre
    CHECK(isNear(dinoStar, dinoFarSide) && isNear(kalienteStar, bathRim));
    CHECK(!isNear(dinoStar, fortress) && !isNear(egg5Star, eggD));
}

static Tally tl(uint16_t serial, int near) { return Tally{serial, 1, 2, 0, (uint8_t)near}; }

static void testLedger() {
    Ledger l;
    l.reset(5);
    CHECK(l.by == -1 && !l.team(0x7));
    CHECK(l.note(0, tl(5, 1)) && l.note(1, tl(5, 1)));
    CHECK(!l.team(0x7));                 // slot 2 present and silent: not a team star
    CHECK(l.team(0x3));                  // the two who are present were both near
    CHECK(!l.note(1, tl(5, 0)));         // a repeat: the first one stays
    CHECK(l.team(0x3));
    CHECK(!l.note(2, tl(6, 1)) && !l.have[2]);   // another serial
    CHECK(!l.note(-1, tl(5, 1)) && !l.note(4, tl(5, 1)) && !l.note(255, tl(5, 1)));
    CHECK(l.note(2, tl(5, 0)));
    CHECK(!l.team(0x7));                 // present, but far from the star
    CHECK(l.team(0x3));                  // (a slot that left does not count against the team)
    CHECK(!l.team(0x1));                 // one player alone is not a team
    CHECK(!l.team(0));
    l.reset(6);
    CHECK(!l.have[0] && !l.team(0x3) && l.serial == 6);
    // every combination of four players: a team star exactly when all present were near and spoke
    for (int near = 0; near < 16; near++)
        for (int spoke = 0; spoke < 16; spoke++)
            for (int present = 0; present < 16; present++) {
                l.reset(1);
                for (int s = 0; s < 4; s++) if (spoke & (1 << s)) l.note(s, tl(1, (near >> s) & 1));
                bool want = __builtin_popcount(present) >= 2;
                for (int s = 0; s < 4; s++) if ((present >> s) & 1) want = want && ((spoke >> s) & 1) && ((near >> s) & 1);
                CHECK(l.team((unsigned)present) == want);
            }
}

static void testStates() {
    using namespace MpPanel;
    CHECK(partyState(false, 0, false) == PARTY_NORMAL);
    CHECK(partyState(true, 0, false) == PARTY_NORMAL);
    CHECK(partyState(true, MP_POSE_VISIBLE | MP_POSE_ON_GROUND, false) == PARTY_NORMAL);
    CHECK(partyState(true, 0, true) == PARTY_QUIET && partyState(false, 0, true) == PARTY_QUIET);
    CHECK(partyState(true, MP_POSE_DEAD, false) == PARTY_DOWN);
    CHECK(partyState(true, MP_POSE_DEAD, true) == PARTY_DOWN);      // down shows over quiet
    CHECK(partyState(false, MP_POSE_DEAD, false) == PARTY_NORMAL);  // (no pose, no flags)
    // a bubbled player (MP_POSE_BUBBLE, the bubble module's flag) floats in the strip, also over quiet / down
    CHECK(kPoseBubble == MP_POSE_BUBBLE);
    CHECK(partyState(true, MP_POSE_BUBBLE, false) == PARTY_BUBBLE && partyState(true, MP_POSE_BUBBLE, true) == PARTY_BUBBLE);
    CHECK(partyState(true, MP_POSE_BUBBLE | MP_POSE_DEAD, false) == PARTY_BUBBLE);
    CHECK(partyState(false, MP_POSE_BUBBLE, false) == PARTY_NORMAL);
    Flash f;
    f.reset();
    CHECK(f.progress(1) < 0 && f.progress(-1) < 0 && f.progress(9) < 0);
    f.start(1);
    f.start(7);                          // (ignored)
    CHECK(f.progress(1) == 0.0f);
    for (int i = 0; i < kFlashFrames / 2; i++) f.tick();
    CHECK(f.progress(1) > 0.49f && f.progress(1) < 0.51f && f.progress(2) < 0);
    f.start(1);                          // another star: starts over
    CHECK(f.progress(1) == 0.0f);
    for (int i = 0; i < kFlashFrames; i++) f.tick();
    CHECK(f.progress(1) < 0);
}

static void testStrip() {
    using namespace MpPanel;
    PartyInput in;
    in.localSlot = 1;
    in.presentMask = 0xF;
    for (int i = 0; i < 4; i++) in.look[i] = (uint8_t)i, in.state[i] = PARTY_NORMAL, in.flash[i] = -1.0f;
    in.state[2] = PARTY_QUIET;
    for (int i = 0; i < 4; i++) in.link[i] = LINK_GOOD, in.away[i] = false;
    in.flash[3] = 0.4f;
    Strip s;
    makeStrip(in, 0.5f, &s);
    CHECK(s.count == 3 && s.appear == 0.5f);                               // the others, in slot order, never the local player
    CHECK(s.m[0].look == 0 && s.m[1].look == 2 && s.m[1].state == PARTY_QUIET && s.m[2].look == 3 && s.m[2].flash == 0.4f);
    in.presentMask = 0x3;                                                  // two players: one head
    makeStrip(in, 1.0f, &s);
    CHECK(s.count == 1 && s.m[0].look == 0);
    in.presentMask = 0x2;                                                  // alone: nothing (single player never shows it)
    makeStrip(in, 1.0f, &s);
    CHECK(s.count == 0);
    in.presentMask = 0xF;
    in.look[0] = 77;                                                       // (a look past the characters: Mario)
    makeStrip(in, 1.0f, &s);
    CHECK(s.m[0].look == 0);
}

// touches: the connection bars
static void testLink() {
    using namespace MpPartyLogic;
    LinkGauge g;
    CHECK(g.update(-1, false) == MpPanel::LINK_GOOD);          // unknown ping: good
    CHECK(g.update(60, false) == MpPanel::LINK_GOOD);
    CHECK(g.update(249, false) == MpPanel::LINK_GOOD);
    CHECK(g.update(250, false) == MpPanel::LINK_WEAK);
    CHECK(g.update(200, false) == MpPanel::LINK_WEAK);         // (hysteresis: no flicker around the line)
    CHECK(g.update(169, false) == MpPanel::LINK_GOOD);
    CHECK(g.update(400, true) == MpPanel::LINK_LOST);          // quiet: lost, whatever the last ping said
    CHECK(g.update(400, true) == MpPanel::LINK_LOST);
    CHECK(g.update(80, false) == MpPanel::LINK_GOOD);          // a packet came, the ping is fine: good at once
    g.update(300, false);
    CHECK(g.update(-1, false) == MpPanel::LINK_WEAK);          // (no new ping yet: stays as it was)
    CHECK(g.update(300, true) == MpPanel::LINK_LOST && g.update(300, false) == MpPanel::LINK_WEAK);
    // the strip carries it
    MpPanel::Strip s;
    PartyInput in;
    in.localSlot = 0;
    in.presentMask = 7;
    for (int i = 0; i < 4; i++) in.look[i] = (uint8_t)i, in.state[i] = MpPanel::PARTY_NORMAL, in.flash[i] = -1.0f, in.link[i] = MpPanel::LINK_GOOD, in.away[i] = false;
    in.link[1] = MpPanel::LINK_WEAK;
    in.link[2] = MpPanel::LINK_LOST;
    in.away[2] = true;
    makeStrip(in, 1.0f, &s);
    CHECK(s.count == 2 && s.m[0].link == MpPanel::LINK_WEAK && !s.m[0].away && s.m[1].link == MpPanel::LINK_LOST && s.m[1].away);
}

// touches: one colour per character
static float Lum(MpCharColor::C c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }
static void testCharColors() {
    for (int a = 0; a < MpCharColor::kCount; a++) {
        const MpCharColor::C f = MpCharColor::fill(a), k = MpCharColor::ink(a);
        CHECK(Lum(f) > 110.0f);                 // readable on a dark background (and with the dark outline on a bright one)
        CHECK(Lum(k) < 130.0f && Lum(k) < Lum(f) - 60.0f);   // ink: readable on the balloon's white
        CHECK(Lum(MpCharColor::edge(a)) < Lum(f) * 0.5f);
        for (int b = a + 1; b < MpCharColor::kCount; b++) {   // all six apart
            const MpCharColor::C g = MpCharColor::fill(b);
            const int d = abs(f.r - g.r) + abs(f.g - g.g) + abs(f.b - g.b);
            CHECK(d > 150);
        }
    }
    CHECK(MpCharColor::fill(-1).r == MpCharColor::fill(0).r && MpCharColor::fill(99).g == MpCharColor::fill(0).g);   // (a bad look = Mario)
    CHECK(MpCharColor::fill(0).r > 200 && MpCharColor::fill(1).g > 200 && MpCharColor::fill(2).b > 200);   // red, green, blue
    const MpPanel::Rgba r = MpPanel::charColor(3);
    CHECK(r.r == MpCharColor::fill(3).r && r.a == 255);   // (the panels read the same table)
}

// touches: the life counter has Mario's and Luigi's pictures only: Toad and Yellow Toad (ingame: and Cosmic Mario, Cosmic Luigi) get their own head from the strip
static void testLocalHead() {
    MpPartyLogic::PartyInput in;
    memset(&in, 0, sizeof in);
    in.localSlot = 1;
    in.presentMask = 3;
    MpPanel::Strip st;
    const int looks[6] = {0, 1, 2, 3, 4, 5};
    const int want[6] = {-1, -1, 2, 3, 4, 5};
    for (int l = 0; l < 6; l++) {
        in.look[1] = (uint8_t)looks[l];
        MpPartyLogic::makeStrip(in, 1.0f, &st);
        CHECK(st.localLook == want[l]);
        CHECK(st.count == 1);
    }
    in.presentMask = 1;   // alone but for slot 0 being "local"
    in.localSlot = 0;
    in.look[0] = 2;
    MpPartyLogic::makeStrip(in, 1.0f, &st);
    CHECK(st.localLook == 2 && st.count == 0);
}

int main() {
    testLocalHead();
    testLink();
    testCharColors();
    testCodec();
    testCounter();
    testNear();
    testLedger();
    testStates();
    testStrip();
    fprintf(stderr, "%d checks, %d failures\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
