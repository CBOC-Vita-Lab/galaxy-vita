// Host test for GXV_ALOD (actorlod23): port/actor_lod.h's schedule (SkipThisTurn), catch-up (PayOwed) and gates, against a model
// of an actor whose own state is order-sensitive. Build and run: bash tools/test_actor_lod.sh
//
// The model actor copies the parts of the game a left-out turn touches:
//  - the BCK clock and its latch, as XanimePlayer::updateAfterMovement / calcAnm make them (frame advances only when the last
//    calcAnm cleared _88; calcAnm poses _84 while _88 is set, then clears it), with a looping frame range;
//  - the nerve and its step (Spine::update), a nerve change every few steps, a velocity per nerve, the position (updateBinder);
//  - its own random stream (a per-actor LCG, so the reference and the test world can be compared value for value);
//  - GXV_ANIM_LOD's deferral with GXV_ANIM_LOD_XLATCH (a calcAnim moved to the drawn turn's flush: stale latch kept, _88 cleared,
//    and set back for the flush's calcAnm), on random turns, in both worlds.
// World O (original): every turn, every actor: movement, then calcAnim (or its deferral, flushed at the turn's end).
// World A (ALOD): every turn, each actor gets a rate (1, 2 or 4, random, from a stream both worlds share; rate 1 also on random
// "contact" turns); SkipThisTurn decides; a left-out turn does nothing; the next turn pays the debt (PayOwed: the lite step = the
// model's movement, then the latch clear of the left-out calcAnim), then makes its movement and calcAnim.
// Checks: whenever an actor of world A has made its turn, its whole state and its pose equal world O's of the same turn; no actor
// ever owes more than rate - 1 (<= 3) steps; with a constant rate r the share of left-out turns is (r - 1) / r. Negative controls
// (ALOD_NEG=1: no latch clear in the catch-up; 2: one owed step dropped; 3: the debt paid AFTER the movement) must fail.
#include "port/actor_lod.h"
#include "port/view_cone.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

uint32_t gPortAlodTurn = 1;  // (the game's counter; the header declares it)

namespace {

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast< uint32_t >(s >> 33);
    }
    float unit() { return (next() & 0xFFFFFF) / 16777216.0f; }
};

struct Actor {
    // XanimePlayer-like clock
    float frame = 0.0f, rate = 1.0f, end = 60.0f, f84 = 0.0f;
    bool latch88 = false;
    // Spine-like nerve
    int nerve = 0, step = 0;
    // body
    float px = 0, py = 0, pz = 0, vx = 0, vy = 0, vz = 0;
    uint32_t lcg = 1;
    float pose = 0.0f;  // the last calcAnm's frame
    // ANIM_LOD deferral (XLATCH)
    bool stale = false, staleLatch = false;
    // ALOD
    int owedN = 0;

    void update() {  // ModelManager::update -> XanimePlayer::updateAfterMovement
        if (!latch88) {
            const float prev = frame;
            frame += rate;
            if (frame >= end) {
                frame -= end;  // (loop)
            }
            f84 = prev;
            latch88 = true;
        }
    }
    uint32_t rnd() {
        lcg = lcg * 1103515245u + 12345u;
        return (lcg >> 16) & 0x7FFF;
    }
    void control() {  // Spine::update + control + updateBinder
        ++step;
        if (step > 7 + static_cast< int >(rnd() % 13)) {
            nerve = (nerve + 1 + static_cast< int >(rnd() % 3)) % 4;
            step = 0;
            rate = 0.5f + 0.25f * nerve;
        }
        const float sp = 1.0f + nerve * 0.37f;
        vx = sp * std::cos(step * 0.1f + nerve);
        vz = sp * std::sin(step * 0.07f - nerve);
        vy = (nerve == 3) ? -0.5f : 0.0f;
        px += vx;
        py += vy;
        pz += vz;
    }
    void movement() {
        update();
        control();
    }
    void calcAnm() {  // XanimePlayer::calcAnm: the pose, then the latch cleared
        pose = latch88 ? f84 : frame;
        latch88 = false;
    }
    void calcAnimOrDefer(bool defer) {
        if (defer) {  // AnimLodDefers + XLATCH (LiveActor.cpp)
            staleLatch = latch88;
            latch88 = false;
            stale = true;
            return;
        }
        if (stale) {  // (computed now; still listed: the flush recomputes from the same state)
            staleLatch = latch88;
        }
        calcAnm();
    }
    void flush() {  // port_anim_flush_stale
        if (!stale) {
            return;
        }
        stale = false;
        if (staleLatch) {
            latch88 = true;
        }
        staleLatch = false;
        calcAnm();
    }
};

bool SameState(const Actor& a, const Actor& b) {
    return a.frame == b.frame && a.rate == b.rate && a.f84 == b.f84 && a.latch88 == b.latch88 && a.nerve == b.nerve && a.step == b.step &&
           a.px == b.px && a.py == b.py && a.pz == b.pz && a.vx == b.vx && a.vy == b.vy && a.vz == b.vz && a.lcg == b.lcg;
}

int gNeg = 0;

struct Adapter {
    Actor* a;
    int owed() const { return a->owedN; }
    void decOwed() {
        --a->owedN;
    }
    void liteStep() { a->movement(); }
    void latchAsCalcAnim() {
        if (gNeg != 1) {
            a->latch88 = false;
        }
    }
};

int RunWorlds(uint64_t seed, int nActors, int nTurns, int constRate, uint64_t* checked, uint64_t* leftOut, uint64_t* turnsAll, int* maxOwed) {
    std::vector< Actor > O(nActors), A(nActors);
    Rng init(seed);
    for (int i = 0; i < nActors; ++i) {
        Actor x;
        x.frame = init.unit() * 59.0f;
        x.rate = 0.5f + init.unit();
        x.end = 30.0f + static_cast< float >(init.next() % 60);
        x.nerve = static_cast< int >(init.next() % 4);
        x.lcg = init.next() | 1u;
        x.px = init.unit() * 1000.0f;
        O[i] = A[i] = x;
    }
    Rng shared(seed ^ 0x9E3779B97F4A7C15ull);  // (rates, contacts, deferrals: one stream for both worlds)
    int bad = 0;
    for (int t = 0; t < nTurns; ++t) {
        ++gPortAlodTurn;
        const bool drawn = (shared.next() % 3) != 0;
        for (int i = 0; i < nActors; ++i) {
            int rate = constRate;
            if (rate == 0) {
                const uint32_t r = shared.next() % 10;
                rate = r < 3 ? 1 : r < 7 ? 2 : 4;
            }
            const bool contact = (shared.next() % 17) == 0 && constRate == 0;
            if (contact) {
                rate = 1;
            }
            const bool defer = (shared.next() % 4) == 0;
            // world O
            O[i].movement();
            O[i].calcAnimOrDefer(defer);
            // world A
            Actor& a = A[i];
            const bool skip = port::alod::SkipThisTurn(gPortAlodTurn, port::alod::PhaseOf(&A[i]), rate, a.owedN);
            ++*turnsAll;
            if (skip) {
                ++a.owedN;
                ++*leftOut;
                if (a.owedN > *maxOwed) {
                    *maxOwed = a.owedN;
                }
                if (a.owedN > 3 || a.owedN > rate - 1) {
                    ++bad;
                }
                continue;  // (no movement, no calcAnim)
            }
            Adapter ad{&a};
            if (gNeg == 3) {
                a.movement();
                port::alod::PayOwed(ad);
            } else {
                if (gNeg == 2 && a.owedN > 0) {
                    --a.owedN;  // (one owed step dropped)
                }
                port::alod::PayOwed(ad);
                a.movement();
            }
            a.calcAnimOrDefer(defer);
        }
        if (drawn) {
            for (int i = 0; i < nActors; ++i) {
                O[i].flush();
                A[i].flush();  // (the game's flush runs for every listed model, also one standing behind: the delayed calcAnim of its last turn)
            }
        }
        // compare every actor that is not behind (its turn was made): the whole state, and the pose after a picture
        for (int i = 0; i < nActors; ++i) {
            if (A[i].owedN != 0) {
                continue;
            }
            ++*checked;
            if (!SameState(O[i], A[i]) || (drawn && O[i].pose != A[i].pose)) {  // (list membership may differ: a left-out turn makes no deferral; the flush recomputes the same pose)
                if (bad < 5 && gNeg == 0) {
                    fprintf(stderr, "seed %llu turn %d actor %d differs: frame %.4f/%.4f latch %d/%d nerve %d/%d step %d/%d pos %.3f/%.3f pose %.3f/%.3f f84 %.4f/%.4f stale %d/%d slatch %d/%d rate %.3f/%.3f lcg %u/%u\n",
                            (unsigned long long)seed, t, i, O[i].frame, A[i].frame, O[i].latch88, A[i].latch88, O[i].nerve, A[i].nerve, O[i].step,
                            A[i].step, O[i].px, A[i].px, O[i].pose, A[i].pose, O[i].f84, A[i].f84, O[i].stale, A[i].stale, O[i].staleLatch, A[i].staleLatch,
                            O[i].rate, A[i].rate, O[i].lcg, A[i].lcg);
                }
                ++bad;
            }
        }
    }
    return bad;
}

int TestGates() {
    int bad = 0;
    // ChooseRate
    if (port::alod::ChooseRate(true, 1e8f, 1e8f, 5000.0f, 4) != 4) ++bad;
    if (port::alod::ChooseRate(false, 1e8f, 1e8f, 5000.0f, 4) != 2) ++bad;
    if (port::alod::ChooseRate(true, 1e6f, 1e8f, 5000.0f, 4) != 2) ++bad;
    if (port::alod::ChooseRate(true, 1e8f, 1e8f, 5000.0f, 2) != 2) ++bad;
    if (port::alod::ChooseRate(true, 1e8f, 1e8f, 5000.0f, 1) != 1) ++bad;
    // PlayerGate grows with reach, speed and rate
    if (!(port::alod::PlayerGate(2500, 100, 10, 4) > port::alod::PlayerGate(2500, 100, 10, 2))) ++bad;
    if (!(port::alod::PlayerGate(2500, 100, 0, 4) == 2600.0f)) ++bad;
    // PixelRadius: 100 units at 1000 with a 45-degree fovy on 544 lines = 100 * 272 / (0.4142 * 1000) = 65.7 px
    const float px = port::alod::PixelRadius(100.0f, 1000.0f, std::tan(22.5f * 3.14159265f / 180.0f), 544.0f);
    if (std::fabs(px - 65.67f) > 0.1f) ++bad;
    if (port::alod::PixelRadius(1.0f, 0.0f, 0.4f, 544.0f) < 1e8f) ++bad;  // (degenerate: never "tiny")
    // SkipThisTurn: never with a debt of rate - 1; rate 1 never
    for (uint32_t t = 0; t < 64; ++t) {
        for (int owed = 0; owed < 4; ++owed) {
            if (port::alod::SkipThisTurn(t, 7, 1, owed)) ++bad;
            if (owed >= 1 && port::alod::SkipThisTurn(t, 7, 2, owed)) ++bad;
            if (owed >= 3 && port::alod::SkipThisTurn(t, 7, 4, owed)) ++bad;
        }
    }
    // the off-screen test with a camera at the origin looking down -z (identity view), fovy 45, aspect 16:9
    port::viewcone::Cone c;
    const float view[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    port::viewcone::Make(&c, view, 45.0f, 16.0f / 9.0f);
    if (port::viewcone::Outside(c, 0, 0, -1000, 300)) ++bad;       // (in front: inside)
    if (!port::viewcone::Outside(c, 0, 0, 1000, 300)) ++bad;       // (behind: outside)
    if (!port::viewcone::Outside(c, 5000, 0, -1000, 300)) ++bad;   // (far to the side: outside)
    return bad;
}

}  // namespace

int main() {
    const char* e = getenv("ALOD_NEG");
    gNeg = e ? atoi(e) : 0;
    int bad = TestGates();
    if (bad) {
        fprintf(stderr, "gates: %d failures\n", bad);
    }
    uint64_t checked = 0, leftOut = 0, all = 0;
    int maxOwed = 0;
    int worldBad = 0;
    for (uint64_t seed = 1; seed <= 40; ++seed) {
        worldBad += RunWorlds(seed, 64, 3000, 0, &checked, &leftOut, &all, &maxOwed);
    }
    printf("mixed rates: 40 seeds x 64 actors x 3000 turns: %llu actor-turns, %llu left out (%.1f %%), %llu states compared, max debt %d, "
           "%d differences\n",
           (unsigned long long)all, (unsigned long long)leftOut, 100.0 * leftOut / all, (unsigned long long)checked, maxOwed, worldBad);
    bad += worldBad;
    for (int r = 2; r <= 4; r += 2) {
        uint64_t c2 = 0, l2 = 0, a2 = 0;
        int m2 = 0;
        int b2 = RunWorlds(100 + r, 64, 4000, r, &c2, &l2, &a2, &m2);
        const double share = static_cast< double >(l2) / a2, want = (r - 1.0) / r;
        printf("constant rate %d: left out %.4f of turns (want %.4f), max debt %d, %d differences\n", r, share, want, m2, b2);
        if (std::fabs(share - want) > 0.002 || m2 != r - 1) {
            ++b2;
        }
        bad += b2;
    }
    if (gNeg != 0) {
        printf("negative control ALOD_NEG=%d: %s (%d differences; it must FAIL)\n", gNeg, bad ? "FAIL" : "PASS", bad);
    } else {
        printf("%s\n", bad ? "FAIL" : "PASS");
    }
    return bad ? 1 : 0;
}
