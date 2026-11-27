// crash33: rear touch aiming's rules (platform/include/port/rear_aim.h). tools/test_rear_aim.sh
#include "port/rear_aim.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace port::rear;

namespace {
const Rect kPad{0, 108, 1919, 889};
const float kDt = 1.0f / 60.0f;
struct Run {
    Aim aim;
    float moved = 0;  // sum of |dx| + |dy| handed to the pointer
    int shown = 0, aimedSamples = 0;
    explicit Run(const Params& p) { aim.Configure(p); aim.Reset(); }
    void Sample(const Contact* c, int n) {
        float dx, dy;
        bool show, fresh;
        if (aim.Step(c, n, kDt, kPad, &dx, &dy, &show, &fresh)) {
            ++aimedSamples;
            moved += std::fabs(dx) + std::fabs(dy);
            if (show) ++shown;
        } else {
            assert(dx == 0 && dy == 0 && !show);
        }
    }
};
// a small deterministic wobble (a hand working the sticks): +-amp units, period `per` samples
float Wob(int i, float amp, int per, int phase = 0) { return amp * std::sin(6.2831853f * float(i + phase) / float(per)); }
}

int main() {
    // 1. Holding the console: four fingers resting in the side bands, wobbling and creeping for 60 s. Strict: nothing. Legacy: it moves.
    for (int strict = 0; strict < 2; strict++) {
        Run r(strict ? StrictParams() : LegacyParams());
        for (int i = 0; i < 3600; i++) {
            const float creep = 0.05f * i; // 3 units a second
            Contact c[4] = {{1, 200 + Wob(i, 25, 50) + creep, 300 + Wob(i, 18, 37)}, {2, 330 + Wob(i, 20, 44, 9), 520 + creep},
                            {3, 1700 + Wob(i, 25, 50, 20) - creep, 320 + Wob(i, 15, 61)}, {4, 1580 - creep, 560 + Wob(i, 22, 41)}};
            r.Sample(c, 4);
        }
        std::printf("holding, %s: moved %.0f units, shown %d samples\n", strict ? "strict" : "legacy", r.moved, r.shown);
        if (strict) assert(r.moved == 0 && r.shown == 0 && r.aim.stats().arms == 0);
        else assert(r.moved > 500); // the fault of 14 Nov, reproduced
    }
    // 2. A finger resting in the MIDDLE that creeps and wobbles slowly (under 60 units in any 0.35 s): never arms in strict.
    {
        Run r(StrictParams());
        for (int i = 0; i < 3600; i++) {
            Contact c[1] = {{7, 900 + 0.2f * i + Wob(i, 20, 120), 500 + Wob(i, 20, 150)}};
            r.Sample(c, 1);
        }
        std::printf("middle rest: moved %.0f, creeps %u\n", r.moved, r.aim.stats().creeps);
        assert(r.moved == 0 && r.aim.stats().arms == 0 && r.aim.stats().creeps > 0);
    }
    // 3. One deliberate slide in the middle, with the holding fingers resting in the bands: the pointer follows it, nearly all of it.
    {
        Run r(StrictParams());
        for (int i = 0; i < 90; i++) {
            const float x = 800 + (i < 60 ? 10.0f * i : 600.0f); // 600 units in 1 s, then still
            Contact c[3] = {{1, 200 + Wob(i, 10, 50), 300}, {3, 1700, 320 + Wob(i, 10, 40)}, {9, x, 480}};
            r.Sample(c, 3);
        }
        std::printf("slide: moved %.0f of 600\n", r.moved);
        assert(r.aim.stats().arms == 1 && r.moved > 570 && r.moved < 601);
        // ... a slow careful slide after it still moves (1 unit a sample adds up)
        const float before = r.moved;
        for (int i = 0; i < 60; i++) { Contact c[1] = {{9, 1400 + 1.0f * i, 480}}; r.Sample(c, 1); }
        assert(r.moved - before > 45);
        // ... and 1.5 s at rest lets the pointer go: the wobble after it moves nothing
        for (int i = 0; i < 100; i++) { Contact c[1] = {{9, 1460, 480}}; r.Sample(c, 1); }
        assert(!r.aim.aiming() && r.aim.stats().idleDrops == 1);
        const float rest = r.moved;
        for (int i = 0; i < 600; i++) { Contact c[1] = {{9, 1460 + Wob(i, 20, 120), 480 + Wob(i, 15, 90)}}; r.Sample(c, 1); }
        assert(r.moved == rest);
    }
    // 4. Two fingers moving in the middle (a grip shift, a palm): nothing; when one lifts, the other needs a new slide.
    {
        Run r(StrictParams());
        for (int i = 0; i < 60; i++) { Contact c[2] = {{1, 700 + 8.0f * i, 400}, {2, 1100 + 8.0f * i, 600}}; r.Sample(c, 2); }
        assert(r.moved == 0 && r.aim.stats().multiSamples > 50);
        for (int i = 0; i < 120; i++) { Contact c[1] = {{2, 1580 + Wob(i, 10, 60), 600}}; r.Sample(c, 1); }
        assert(r.moved == 0);
        // a resting middle finger (still for 0.5 s) does not stop another finger from aiming
        Run q(StrictParams());
        for (int i = 0; i < 60; i++) { Contact c[1] = {{5, 700, 500}}; q.Sample(c, 1); }
        for (int i = 0; i < 40; i++) { Contact c[2] = {{5, 700, 500}, {6, 1000 + 10.0f * i, 450}}; q.Sample(c, 2); }
        std::printf("aim beside a resting finger: moved %.0f of 400\n", q.moved);
        assert(q.moved > 80); // (the first half second after the landing is not counted: two fingers in motion)
    }
    // 5. A contact id that jumps across the pad in one sample (two contacts merged / swapped): not applied.
    {
        Run r(StrictParams());
        for (int i = 0; i < 30; i++) { Contact c[1] = {{4, 800 + 10.0f * i, 450}}; r.Sample(c, 1); }
        const float before = r.moved;
        Contact j[1] = {{4, 1500, 700}};
        r.Sample(j, 1);
        r.Sample(j, 1);
        assert(r.aim.stats().jumps == 1 && r.moved - before < 10);
    }
    // 6. Contacts on the pad at a reset never take the pointer; a slide that started in a band does not either.
    {
        Run r(StrictParams());
        Contact c0[1] = {{3, 900, 450}};
        r.aim.Block(c0, 1);
        for (int i = 0; i < 60; i++) { Contact c[1] = {{3, 900 + 10.0f * i, 450}}; r.Sample(c, 1); }
        assert(r.moved == 0);
        Run b(StrictParams());
        for (int i = 0; i < 60; i++) { Contact c[1] = {{8, 100 + 15.0f * i, 450}}; b.Sample(c, 1); }
        assert(b.moved == 0 && b.aim.stats().gripLandings == 1);
        Run t(StrictParams());
        for (int i = 0; i < 60; i++) { Contact c[1] = {{8, 900 + 10.0f * i, 130}}; t.Sample(c, 1); } // the top band
        assert(t.moved == 0);
    }
    // 7. The earlier rules are kept as they were: a slide of 40 units arms, whatever the time, wherever it landed.
    {
        Run r(LegacyParams());
        for (int i = 0; i < 200; i++) { Contact c[1] = {{2, 100 + 0.5f * i, 450}}; r.Sample(c, 1); }
        assert(r.aim.stats().arms == 1 && r.shown > 0);
    }
    // 8. (crash33b) Contacts nobody makes: blips with a new id every few samples anywhere in the middle, and a contact whose
    //    position teleports 200 units now and then: the pointer never moves.
    {
        Run r(StrictParams());
        unsigned rng = 99;
        auto rnd = [&rng](float lo, float hi) { rng = rng * 1664525u + 1013904223u; return lo + (hi - lo) * float((rng >> 8) & 0xFFFF) / 65535.0f; };
        float gx = 900, gy = 450;
        for (int i = 0; i < 3600; i++) {
            if (i % 40 == 0) { gx = rnd(600, 1300); gy = rnd(250, 750); }     // the steady ghost teleports
            Contact c[2] = {{50, gx + rnd(-3, 3), gy + rnd(-3, 3)}, {100 + i / 3, rnd(550, 1350), rnd(220, 780)}};  // + a blip, a new id every 3 samples
            r.Sample(c, (i % 7) < 4 ? 2 : 1);
        }
        std::printf("ghost contacts: moved %.0f, arms %u, jumps %u\n", r.moved, r.aim.stats().arms, r.aim.stats().jumps);
        assert(r.moved == 0 && r.aim.stats().arms == 0);
    }
    std::printf("rear_aim: ok\n");
    return 0;
}
