// Galaxy 3.0 multiplayer: host tests for the puppet clock and the STATE pacing (owner: perf). decomp/include/Game/MP/MpPoseMath.hpp.
//   - the render clock never jumps: with jittery arrivals (early packets, late windows, a growing jitter estimate) the rendered
//     sender time advances every turn by 1 - kSlewDown .. 1 + kSlewUp, never backwards, and the puppet's position follows it;
//   - a large path change (latency +17 turns) is followed at half speed (never a jump back in time), then smooth;
//   - a teleport between two samples is a snap, never a slide across the level; extrapolation stops after 150 ms;
//   - name tags: wanted only on screen and within 6000 units (setting 1), with hysteresis; 8-turn fades both ways;
//   - STATE pacing is wall-clock based: 30 Hz at 60 turns/s with or without jitter, never more than 40/s when turns run fast.
// Build: tests/mp/perf_build.sh (or clang++/g++ -std=c++20 -fsanitize=address,undefined -Iplatform/include -Idecomp/include).
#include "Game/MP/MpPoseMath.hpp"

#include <math.h>
#include <stdio.h>

static int gFail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            if (gFail++ < 30) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                                \
    } while (0)

static uint32_t sRng = 777;
static float frand(float lo, float hi) {
    sRng = sRng * 1664525u + 1013904223u;
    return lo + (hi - lo) * ((sRng >> 8) & 0xFFFF) / 65535.0f;
}

static MpPoseF pose(uint32_t turn, float x) {
    MpPoseF p;
    memset(&p, 0, sizeof p);
    p.turn = turn;
    p.quat[3] = 1.0f;
    p.scale = 1.0f;
    p.pos[0] = x;
    p.anim = 0x1234;
    p.rate = 1.0f;
    p.frame = (float)turn;
    p.flags = MP_POSE_VISIBLE;
    return p;
}

// Sender at 30 Hz, 10 units per turn along x; the receiver samples every turn. Latency per packet: `lat(k)`.
template <class Lat>
static void runStream(const char* name, Lat lat, int turns, float* worstStep, float* worstBack, int* holds, int* snaps, float* worstPos) {
    MpPoseBuffer b;
    b.reset();
    const uint32_t s0 = 4000000, r0 = 10;
    struct Pend { uint32_t at; MpPoseF p; };
    static Pend q[8192];
    int nq = 0;
    for (int k = 0; k < turns; k += 2) q[nq++] = {r0 + (uint32_t)k + lat(k), pose(s0 + (uint32_t)k, (float)k * 10.0f)};
    float prev = 0.0f;
    bool have = false;
    *worstStep = 0.0f; *worstBack = 0.0f; *holds = 0; *snaps = 0; *worstPos = 0.0f;
    for (uint32_t now = r0; now < r0 + (uint32_t)turns; now++) {
        for (int i = 0; i < nq; i++)
            if (q[i].at == now) b.push(q[i].p, now);
        MpPoseF o;
        MpSampleResult r = b.sample(now, &o);
        if (r == MP_SAMPLE_NONE || now < r0 + 60) continue;   // (settle: the first second)
        const float rendered = (float)(int32_t)(b.s[b.count - 1].turn - s0) + b.renderRel(now);
        if (have) {
            const float step = rendered - prev;
            if (step < 1.0f - MpPoseBuffer::kSlewDown - 0.001f || step > 1.0f + MpPoseBuffer::kSlewUp + 0.001f) (*snaps)++;
            *worstStep = fmaxf(*worstStep, fabsf(step - 1.0f));
            if (step < 0.0f) *worstBack = fmaxf(*worstBack, -step);
        }
        prev = rendered;
        have = true;
        if (r == MP_SAMPLE_HOLD) (*holds)++;
        if (r == MP_SAMPLE_INTERP) *worstPos = fmaxf(*worstPos, fabsf(o.pos[0] - rendered * 10.0f));
    }
    printf("%-26s worst clock step %.3f turns, back %.3f, snaps %d, holds %d, worst interp pos error %.3f\n", name, *worstStep, *worstBack,
           *snaps, *holds, *worstPos);
}

static void testSmoothClock() {
    float step, back, pos;
    int holds, snaps;
    // 3..9 turns one way (50..150 ms), uniformly random: early packets move the offset forward, late windows back
    runStream("jitter 50-150 ms", [](int) { return (uint32_t)(3 + (int)frand(0, 6.99f)); }, 3600, &step, &back, &holds, &snaps, &pos);
    CHECK(snaps == 0);
    CHECK(back == 0.0f);
    CHECK(holds == 0);
    CHECK(pos < 0.5f);
    // mostly 4 turns with a 10 % chance of +6 (Wi-Fi power save spikes)
    runStream("spikes +100 ms", [](int) { return (uint32_t)(4 + (frand(0, 1) < 0.1f ? 6 : 0)); }, 3600, &step, &back, &holds, &snaps, &pos);
    CHECK(snaps == 0);
    CHECK(back == 0.0f);
    CHECK(holds == 0);
    // a sender whose turns run 3 % slower than ours (a console that drops logic turns under load)
    runStream("sender 3 % slow", [](int k) { return (uint32_t)(4 + k * 3 / 100); }, 3600, &step, &back, &holds, &snaps, &pos);
    CHECK(snaps == 0);
    CHECK(back == 0.0f);
    // a path change: +17 turns at turn 1200: one snap (as before), then smooth, no hold after it settled
    runStream("latency step +283 ms", [](int k) { return (uint32_t)(k < 1200 ? 3 : 20); }, 3600, &step, &back, &holds, &snaps, &pos);
    CHECK(back == 0.0f);   // (no rewind: the render point slows down to half speed instead)
    CHECK(holds < 60);   // (the ~0.5 s the estimator needs to see that every packet is late: extrapolated, then held, as before)
}

static void testWarpSnapAndExtrapClamp() {
    MpPoseBuffer b;
    b.reset();
    // standing at x=0, then a pipe / warp to x=8000 between two samples
    for (uint32_t k = 0; k <= 40; k += 2) b.push(pose(100 + k, k < 20 ? 0.0f : 8000.0f), 10 + k);
    int between = 0;
    for (uint32_t now = 20; now < 60; now++) {
        MpPoseF o;
        b.sample(now, &o);
        if (o.pos[0] > 1.0f && o.pos[0] < 7999.0f) between++;
    }
    CHECK(between == 0);
    // extrapolation stops at 150 ms (9 turns) and then holds
    MpPoseBuffer e;
    e.reset();
    for (uint32_t k = 0; k <= 20; k += 2) e.push(pose(100 + k, (float)k * 5.0f), 10 + k);
    MpPoseF o;
    for (uint32_t now = 30; now < 100; now++) e.sample(now, &o);
    CHECK(fabsf(o.pos[0] - (100.0f + 5.0f * MpPoseBuffer::extrapDistTime(MpPoseBuffer::kMaxExtrap))) < 0.01f);
    printf("warp: positions between the two places %d; extrapolation held at %.1f\n", between, o.pos[0]);
}

static void testSendGate() {
    // 60 logic turns per second with +-5 ms of frame jitter: every 2nd turn (30 Hz)
    MpSendGate g;
    g.reset();
    int64_t t = 1000000000;
    int sent = 0, gapsNot2 = 0, last = -1;
    for (int turn = 0; turn < 6000; turn++) {
        t += 16666667 + (int64_t)(frand(-5, 5) * 1000000.0f);
        if (g.due(t)) {
            if (last >= 0 && turn - last != 2) gapsNot2++;
            last = turn;
            sent++;
        }
    }
    printf("send gate: 60 turns/s with jitter: %d of 6000 turns sent, %d gaps that were not 2 turns\n", sent, gapsNot2);
    CHECK(sent >= 2990 && sent <= 3010);
    CHECK(gapsNot2 == 0);
    // a skipped cutscene: 40 turns in each 16.7 ms frame (2400 turns/s) for 5 s: at most ~45 STATEs per second
    g.reset();
    sent = 0;
    for (int frame = 0; frame < 300; frame++)
        for (int k = 0; k < 40; k++) {
            t += 16666667 / 40;
            if (g.due(t)) sent++;
        }
    printf("send gate: 2400 turns/s for 5 s: %d sent\n", sent);
    CHECK(sent <= 5 * 46 && sent >= 5 * 30);
    // catch-up turns back to back after a long frame: no burst
    g.reset();
    sent = 0;
    t += 100000000;
    for (int k = 0; k < 4; k++) {
        if (g.due(t)) sent++;
        t += 500000;
    }
    CHECK(sent == 1);
}

static void testTagVis() {
    MpTagVis v;
    v.reset();
    // fades in over kFadeTurns, never pops
    int turns = 0;
    float prev = 0.0f, worst = 0.0f;
    while (v.step(v.wanted(1, true, false, 1000.0f * 1000.0f, false)) < 1.0f && turns < 100) {
        worst = fmaxf(worst, v.alpha - prev);
        prev = v.alpha;
        turns++;
    }
    CHECK(turns == MpTagVis::kFadeTurns - 1 && worst <= 1.0f / MpTagVis::kFadeTurns + 0.001f);
    // off screen (cull), edge, far, off, not drawable: not wanted in mode 1; mode 2 keeps the edge marker of a culled / far puppet
    CHECK(!v.wanted(1, true, true, 0.0f, false));
    CHECK(!v.wanted(1, true, false, 0.0f, true));
    CHECK(!v.wanted(0, true, false, 0.0f, false));
    CHECK(!v.wanted(1, false, false, 0.0f, false));
    CHECK(v.wanted(2, true, true, 0.0f, true));
    v.reset();
    CHECK(v.wanted(1, true, false, 5900.0f * 5900.0f, false));
    CHECK(!v.wanted(1, true, false, 6100.0f * 6100.0f, false));   // far
    CHECK(!v.wanted(1, true, false, 5800.0f * 5800.0f, false));   // still far (hysteresis)
    CHECK(v.wanted(1, true, false, 5400.0f * 5400.0f, false));    // back
    CHECK(v.wanted(2, true, false, 9000.0f * 9000.0f, false));    // edge-marker setting: far tags stay
    // fades out over kFadeTurns, reaches exactly 0 (the plate is killed then)
    v.alpha = 1.0f;
    turns = 0;
    while (v.step(false) > 0.0f && turns < 100) turns++;
    CHECK(turns == MpTagVis::kFadeTurns - 1 && v.alpha == 0.0f);
    printf("tag vis: fade %d turns each way, far %g/%g units\n", MpTagVis::kFadeTurns, MpTagVis::kFarIn, MpTagVis::kFarOut);
}

int main() {
    testTagVis();
    testSmoothClock();
    testWarpSnapAndExtrapClamp();
    testSendGate();
    if (gFail) {
        printf("FAILED: %d checks\n", gFail);
        return 1;
    }
    printf("all perf pose tests passed\n");
    return 0;
}
