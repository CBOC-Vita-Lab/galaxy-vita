// idle25: host test of platform/include/port/idle25.h (GXV_IDLE25's pure parts) against TRANSCRIPTIONS of the game's own code, plus a multi-thousand-turn
// simulation (full run vs idle run, state hash compared every turn). Build and run: tools/test_idle25.sh
//
// What is proved here: the micro-steps equal the transcribed movement for the state vectors the predicates admit, bit for bit; the catch-up closed form
// (n micro-steps) is exactly n single steps (repeated addition, not a multiplication, incl. angles that are not multiples of 15); the J3DFrameCtrl quiet rule
// is sound against a verbatim copy of J3DFrameCtrl::update; the wake rules sit exactly on the game's comparisons (float boundaries). What is NOT proved
// here: that the transcriptions are the game's code (the console's GXV_IDLE25=2 compares the real movement with the prediction).
#include "port/idle25.h"
#include "port/actor_lod.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

using namespace port::idle25;

static long gChecks = 0, gFail = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        ++gChecks;                                                                       \
        if (!(c)) {                                                                      \
            ++gFail;                                                                     \
            if (gFail < 30) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                                \
    } while (0)

static uint32_t sRng = 12345;
static uint32_t Rnd() {
    sRng = sRng * 1664525u + 1013904223u;
    return sRng >> 8;
}
static float RndF(float lo, float hi) { return lo + (hi - lo) * (Rnd() & 0xFFFF) / 65535.0f; }
static uint32_t Bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// ---- verbatim copy of J3DFrameCtrl::update (decomp/src/JSystem/J3DGraphAnimator/J3DAnimation.cpp) ----
struct FC {
    uint8_t attr, state;
    int16_t start, end, loop;
    float rate, frame;
    void update() {
        state = 0;
        frame += rate;
        switch (attr) {
        case 0:
            if (frame < start) { frame = start; rate = 0.0f; state |= 1; }
            if (frame >= end) { frame = end - 0.001f; rate = 0.0f; state |= 1; }
            break;
        case 1:
            if (frame < start) { frame = start; rate = 0.0f; state |= 1; }
            if (frame >= end) { frame = start; rate = 0.0f; state |= 1; }
            break;
        case 2:
            while (frame < start) { state |= 2; if (loop - start <= 0.0f) break; frame += loop - start; }
            while (frame >= end) { state |= 2; if (end - loop <= 0.0f) break; frame -= end - loop; }
            break;
        case 3:
            if (frame >= end) { frame = end - (frame - end); rate = -rate; }
            if (frame < start) { frame = start - (frame - start); rate = 0.0f; state |= 1; }
            break;
        case 4:
            if (frame >= end - 1.0f) { frame = (end - 1.0f) - (frame - (end - 1.0f)); rate = -rate; }
            if (frame < start) { frame = start - (frame - start); rate = -rate; state |= 2; }
            break;
        }
    }
};

static void TestFrameCtrl() {
    long quiet = 0;
    for (int it = 0; it < 400000; ++it) {
        FC c;
        c.attr = static_cast<uint8_t>(Rnd() % 5);
        c.state = static_cast<uint8_t>(Rnd() % 4);
        c.start = static_cast<int16_t>(Rnd() % 20);
        c.end = static_cast<int16_t>(c.start + Rnd() % 60);
        c.loop = static_cast<int16_t>(c.start + (c.end > c.start ? Rnd() % (c.end - c.start + 1) : 0));
        c.rate = (Rnd() % 3 == 0) ? 0.0f : RndF(-2.0f, 2.0f);
        c.frame = RndF(-5.0f, 90.0f);
        if (Rnd() % 4 == 0) c.rate = 0.0f;
        if (Rnd() % 8 == 0) c.state = 0;
        const bool q = FrameCtrlQuiet(c.frame, c.rate, c.state, c.start, c.end);
        FC d = c;
        d.update();
        if (q) {
            ++quiet;
            CHECK(d.attr == c.attr && d.state == c.state && d.start == c.start && d.end == c.end && d.loop == c.loop);
            CHECK(Bits(d.rate) == Bits(c.rate) && Bits(d.frame) == Bits(c.frame));
        }
    }
    CHECK(quiet > 2000);  // the property is exercised, not vacuous
}

// ---- RepeatDegree == the game's repeat(): min + (f32)fmod(max + (value - min), max) ----
static void TestRepeat() {
    for (int it = 0; it < 200000; ++it) {
        const float v = RndF(-720.0f, 1080.0f);
        const float ref = 0.0f + static_cast<float>(fmod(static_cast<double>(360.0f + (v - 0.0f)), static_cast<double>(360.0f)));
        CHECK(Bits(RepeatDegree(v)) == Bits(ref));
    }
    // the lattice of the game: multiples of 15 stay on it forever
    float r = 0.0f;
    for (int i = 0; i < 100000; ++i) {
        r += 15.0f;
        r = RepeatDegree(r);
        CHECK(r >= 0.0f && r < 360.0f && fmodf(r, 15.0f) == 0.0f);
    }
}

// ---- the star bit: transcription of the game's Floating step ----
struct SbWorld {
    float rotY;
    int32_t step, c8, delay;
    bool flag1;
    // a real step; returns events through the out parameters
};
// Spine::update + exeFloating (not first) + tryGotJudge + control, as written in StarPiece.cpp. `playerD2`: squared distance to the player's centre.
static void RealStar(SbWorld& s, float playerD2, bool pointing, int* events, int* pointerCalls, int* emitted) {
    // exeFloating
    s.rotY += 15.0f;
    s.rotY = 0.0f + static_cast<float>(fmod(static_cast<double>(360.0f + (s.rotY - 0.0f)), 360.0));
    // tryGotJudge
    if (225000000.0f < playerD2 || !s.flag1) {
        s.c8 = -1;
    } else {
        s.c8 += 91;
        s.c8 %= 90;
        if (s.c8 == 20) {
            ++*emitted;  // emitGettableEffect
            ++*events;
        }
        if (s.delay < 0) {
            ++*pointerCalls;
            if (pointing) { /* goToPlayer: the nerve changes; the oracle only counts it */ }
        }
    }
    s.step += 1;  // Spine::update
    // control()
    if (s.delay >= 0) s.delay -= 1;
}
static uint64_t Hash(uint64_t h, uint32_t x) { return (h ^ x) * 1099511628211ull; }

static void TestStarSim() {
    long idleTurns = 0, wakeEvent = 0, wakePointer = 0, total = 0;
    for (int world = 0; world < 40; ++world) {
        const int N = 60;
        std::vector<SbWorld> full(N), idle(N);
        std::vector<float> px(N), py(N), pz(N);
        for (int i = 0; i < N; ++i) {
            SbWorld w;
            w.rotY = (Rnd() % 3 == 0) ? RndF(0.0f, 359.0f) : 15.0f * (Rnd() % 24);  // off-lattice angles too
            w.step = 1 + static_cast<int32_t>(Rnd() % 5);
            w.c8 = (Rnd() % 4 == 0) ? static_cast<int32_t>(Rnd() % 90) : -1;
            w.delay = (Rnd() % 3 == 0) ? static_cast<int32_t>(Rnd() % 40) : -1;
            w.flag1 = Rnd() % 8 != 0;
            full[i] = idle[i] = w;
            px[i] = RndF(-30000.0f, 30000.0f);
            py[i] = RndF(-3000.0f, 3000.0f);
            pz[i] = RndF(-30000.0f, 30000.0f);
        }
        float ux = RndF(-20000.0f, 20000.0f), uy = 0.0f, uz = RndF(-20000.0f, 20000.0f);
        float tx = ux, tz = uz;
        uint64_t hf = 1469598103934665603ull, hi = hf;
        for (int turn = 0; turn < 4000; ++turn) {
            // the player: runs 20 units a turn toward a random target, picks a new one at random (random approaches)
            if (Rnd() % 200 == 0) { tx = RndF(-30000.0f, 30000.0f); tz = RndF(-30000.0f, 30000.0f); }
            const float dx = tx - ux, dz = tz - uz, dl = sqrtf(dx * dx + dz * dz) + 1e-6f;
            ux += dx / dl * 20.0f;
            uz += dz / dl * 20.0f;
            const bool pointerOn = (turn / 300) % 3 == 1;  // the star pointer is in play for stretches
            for (int i = 0; i < N; ++i) {
                const float ddx = ux - px[i], ddy = uy - py[i], ddz = uz - pz[i];
                const float d2 = ddx * ddx + ddy * ddy + ddz * ddz;
                const bool pointing = pointerOn && ((Rnd() % 50) == 0 || (i % 7 == 0 && turn % 11 == 0));
                int ev = 0, pc = 0, em = 0;
                RealStar(full[i], d2, pointing, &ev, &pc, &em);
                // idle run: the predicate, then the micro-step or the real step
                SbWorld& s = idle[i];
                const bool active = !(225000000.0f < d2 || !s.flag1);
                SbState st{s.rotY, s.step, s.c8, s.delay};
                bool real = false;
                if (SbEventNext(st, active)) { real = true; ++wakeEvent; }
                else if (SbPointerAsked(st, active) && pointing) { real = true; ++wakePointer; }
                ++total;
                if (real) {
                    int e2 = 0, p2 = 0, m2 = 0;
                    RealStar(s, d2, pointing, &e2, &p2, &m2);
                    CHECK(e2 == ev && m2 == em);
                } else {
                    ++idleTurns;
                    CHECK(ev == 0 && em == 0);                  // the predicate never skips a turn that emits
                    CHECK(!(pc != 0 && pointing));              // nor one where the pointer would have pointed
                    SbAdvance(st, active);
                    s.rotY = st.rotY; s.step = st.step; s.c8 = st.c8; s.delay = st.delay;
                }
                CHECK(Bits(full[i].rotY) == Bits(idle[i].rotY) && full[i].step == idle[i].step && full[i].c8 == idle[i].c8 && full[i].delay == idle[i].delay);
                hf = Hash(Hash(Hash(Hash(hf, Bits(full[i].rotY)), full[i].step), full[i].c8), full[i].delay);
                hi = Hash(Hash(Hash(Hash(hi, Bits(idle[i].rotY)), idle[i].step), idle[i].c8), idle[i].delay);
            }
            CHECK(hf == hi);
        }
    }
    printf("star bit sim: %ld actor-turns, %ld idle (%.1f%%), woke for event %ld, for pointer %ld\n", total, idleTurns, 100.0 * idleTurns / total, wakeEvent, wakePointer);
    CHECK(idleTurns > total / 4 && wakeEvent > 100 && wakePointer > 10);
}

static void TestCatchUp() {
    for (int it = 0; it < 20000; ++it) {
        SbState a{(Rnd() % 2) ? RndF(0.0f, 359.9f) : 15.0f * (Rnd() % 24), static_cast<int32_t>(Rnd() % 100), static_cast<int32_t>(Rnd() % 90) - (Rnd() % 2),
                  static_cast<int32_t>(Rnd() % 60) - 20};
        const bool active = Rnd() % 2 == 0;
        const int n = static_cast<int>(Rnd() % 500);
        SbState b = a;
        SbCatchUp(a, n, active);
        for (int i = 0; i < n; ++i) SbAdvance(b, active);
        CHECK(Bits(a.rotY) == Bits(b.rotY) && a.step == b.step && a.c8 == b.c8 && a.delay == b.delay);
    }
    // float accumulation: repeated addition differs from multiplication for an off-lattice angle, so the catch-up must not multiply
    float r = 7.3f;
    for (int i = 0; i < 1000; ++i) r = RepeatDegree(r + 15.0f);
    const float viaMul = RepeatDegree(7.3f + 15.0f * 1000.0f);
    printf("repeated addition %.9g vs multiplication %.9g (%s)\n", r, viaMul, Bits(r) == Bits(viaMul) ? "equal here" : "differ: why the catch-up repeats the addition");
    StepState s{5};
    StepCatchUp(s, 1000);
    StepState t{5};
    for (int i = 0; i < 1000; ++i) StepAdvance(t);
    CHECK(s == t);
}

// ---- petit piranha: Wait <-> NonActive, wake distance exactly the game's compare ----
enum PNerve { NonActive, Wait, Find };
struct Pet {
    PNerve curr, next;
    bool hasNext;
    int32_t step;
    bool operator==(const Pet& o) const { return curr == o.curr && next == o.next && hasNext == o.hasNext && step == o.step; }
};
static void PetSet(Pet& p, PNerve n) { p.next = n; p.hasNext = true; p.step = -1; }
static void PetChange(Pet& p) { if (p.hasNext) { p.curr = p.next; p.hasNext = false; p.step = 0; } }
static void RealPet(Pet& p, float d2, int32_t* scaleStep) {
    PetChange(p);
    // execute
    if (p.curr == NonActive) {
        if (d2 < 5000.0f * 5000.0f) PetSet(p, Wait);
    } else if (p.curr == Wait) {
        if (d2 < 1700.0f * 1700.0f) PetSet(p, Find);
        else if (!(d2 < 5000.0f * 5000.0f)) PetSet(p, NonActive);
    }
    p.step++;
    PetChange(p);
    *scaleStep += 1;  // control(): the scale controller's updateNerve (Stop)
}
static void TestPet() {
    // the boundary: nextafter around 5000^2
    const float b = 5000.0f * 5000.0f;
    CHECK(PetNear(nextafterf(b, 0.0f)) && !PetNear(b) && !PetNear(nextafterf(b, 1e30f)));
    long idleTurns = 0, total = 0;
    for (int world = 0; world < 30; ++world) {
        Pet f{NonActive, NonActive, false, 1}, d = f;
        int32_t sf = 3, sd = 3;
        float dist = RndF(3000.0f, 14000.0f), v = 0.0f;
        for (int turn = 0; turn < 6000; ++turn) {
            if (Rnd() % 100 == 0) v = RndF(-40.0f, 40.0f);
            dist += v;
            if (dist < 100.0f) { dist = 100.0f; v = 0.0f; }
            if (dist > 16000.0f) { dist = 16000.0f; v = 0.0f; }
            const float d2 = dist * dist;
            RealPet(f, d2, &sf);
            const bool idle = d.curr == NonActive && !d.hasNext && d.step >= 1 && !PetNear(d2);
            ++total;
            if (idle) { ++idleTurns; d.step += 1; sd += 1; } else { RealPet(d, d2, &sd); }
            CHECK(f == d && sf == sd);
        }
    }
    printf("petit sim: %ld actor-turns, %ld idle (%.1f%%)\n", total, idleTurns, 100.0 * idleTurns / total);
    CHECK(idleTurns > total / 10);
}

// negative controls: the checks above must be able to fail
static void TestNegativeControls() {
    // (1) a skipper that ignores the event turn loses effects
    long lost = 0;
    SbState st{0.0f, 5, -1, -1};
    for (int i = 0; i < 2000; ++i) {
        if (SbEventNext(st, true)) ++lost;  // a turn the real movement emits on
        SbAdvance(st, true);
    }
    CHECK(lost >= 20);
    // (2) catch-up by multiplication is not the game's accumulation for an off-lattice angle
    long differ = 0;
    for (int i = 0; i < 2000; ++i) {
        const float a0 = RndF(0.0f, 359.0f);
        SbState a{a0, 0, -1, -1};
        SbCatchUp(a, 300, false);
        if (Bits(a.rotY) != Bits(RepeatDegree(a0 + 15.0f * 300.0f))) ++differ;
    }
    CHECK(differ > 100);
    // (3) a quiet rule without the state test would admit a frame control that update() changes
    FC c{2, 1, 0, 40, 0, 0.0f, 5.0f};
    CHECK(!FrameCtrlQuiet(c.frame, c.rate, c.state, c.start, c.end));
    FC d = c;
    d.update();
    CHECK(d.state != c.state);
    printf("negative controls: %ld event turns a skipper would have lost, %ld multiplication catch-ups off by bits\n", lost, differ);
}

// verifyfix31: GXV_IDLE25=2 under GXV_ALOD half-speed. A coin (idle: the only state is the nerve step) is moved by an ALOD model (rest turn = owe a step, no
// movement), the verify compares the micro-step prediction with what "the real closure" did. With the rest turn taken as the reference about half of the
// checks differ (the bug); with the fix (AlodSkipUnderVerify) none does. Mutant: -DPORT_IDLE25_MUTANT_VERIFY_SKIP (tools/test_idle25.sh with EXTRA=...).
static void TestVerifyUnderAlod() {
    long checks = 0, bad = 0;
    for (int rate = 2; rate <= 4; rate += 2) {
        for (uint32_t phase = 0; phase < 4; ++phase) {
            StepState coin{1};
            for (uint32_t turn = 0; turn < 400; ++turn) {
                const bool skip = AlodSkipUnderVerify(port::alod::SkipThisTurn(turn, phase, rate, 0), true);
                StepState expect = coin;
                StepAdvance(expect);
                if (!skip) {
                    StepAdvance(coin);  // (the closure made the movement)
                }  // (a rest turn only books a step owed: the actor did not move)
                ++checks;
                if (!(coin == expect)) ++bad;
                coin = expect;  // (a real run: an actor that owes steps is not idle (IdCommon), the next check starts from the predicted state)
            }
        }
    }
    printf("verify under ALOD: %ld checks, %ld differ\n", checks, bad);
    CHECK(checks == 2 * 4 * 400);
    CHECK(bad == 0);
}

int main() {
    TestVerifyUnderAlod();
    TestNegativeControls();
    TestRepeat();
    TestFrameCtrl();
    TestCatchUp();
    TestStarSim();
    TestPet();
    printf("idle25 host test: %ld checks, %ld failed\n", gChecks, gFail);
    return gFail == 0 ? 0 : 1;
}
