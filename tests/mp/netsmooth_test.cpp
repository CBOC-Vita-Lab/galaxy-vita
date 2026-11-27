// Galaxy 3.0 multiplayer: host tests for the remote-player smoothing (owner: netsmooth). decomp/include/Game/MP/MpPoseMath.hpp.
//   - the jitter buffer: on a clean link the delay settles at the minimum (67 ms); bunched arrivals (power save) raise it to
//     cover them; a single late packet does not; it comes back down once the link is clean again; losses count;
//   - error correction: a late STATE that contradicts an extrapolation is glided (no step larger than the motion's own plus a
//     bound, no velocity step), gone after ~30 turns; out-of-order arrivals too; a warp, a visibility change or a long gap snaps;
//   - extrapolation: a landing never sinks below the ground, a slope is kept, an arc in the air follows gravity, the fall speed
//     is limited; animation frames glide on a late STATE (same animation) and are never glided across a loop wrap;
//   - a hitch of the sender's game (its turns stop for 20 turns): back on the samples quickly;
//   - a lossy, jittery stream (150 +- 80 ms, 10 % loss) of a turning, jumping motion: no hold, no snap, bounded kinks.
// Build: g++ -std=c++20 -g -O1 -fsanitize=address,undefined -Iplatform/include -Idecomp/include -Itests/mp tests/mp/netsmooth_test.cpp
#include "Game/MP/MpPoseMath.hpp"
#include "netsmooth_v1_buffer.h"   // the mp-int buffer, for contrast

#include <math.h>
#include <stdio.h>

static int gFail = 0, gChecks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        gChecks++;                                                                \
        if (!(c)) {                                                               \
            if (gFail++ < 40) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                                         \
    } while (0)

static uint32_t sRng = 4242;
static float frand(float lo, float hi) {
    sRng = sRng * 1664525u + 1013904223u;
    return lo + (hi - lo) * ((sRng >> 8) & 0xFFFF) / 65535.0f;
}

static MpPoseF pose(uint32_t turn, float x, float y, float z, uint16_t flags = MP_POSE_VISIBLE | MP_POSE_ON_GROUND) {
    MpPoseF p;
    memset(&p, 0, sizeof p);
    p.turn = turn;
    p.quat[3] = 1.0f;
    p.scale = 1.0f;
    p.pos[0] = x; p.pos[1] = y; p.pos[2] = z;
    p.anim = 0x77;
    p.rate = 1.0f;
    p.frame = (float)(turn % 1000);
    p.flags = flags;
    return p;
}

static float dist3(const float a[3], const float b[3]) {
    float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return MpPoseMath::len3(d);
}

// ---- the jitter buffer ------------------------------------------------------------------------------------------------------
static void testDelayAdapts() {
    MpPoseBuffer b;
    b.reset();
    CHECK(b.delay() >= 5.0f && b.delay() <= 7.0f);   // ~100 ms before anything is known
    uint32_t now = 100;
    const uint32_t s0 = 50000;
    // 10 s of a clean link: 0..1 turn of jitter
    for (int k = 0; k < 600; k += 2) b.push(pose(s0 + k, 0, 0, 0), now + k + (frand(0, 1) < 0.3f ? 1 : 0));
    printf("delay: clean link %.1f turns\n", b.delay());
    CHECK(b.delay() <= MpPoseBuffer::kMinDelay + 0.01f);
    // one packet 12 turns late: not worth 12 turns of delay for everybody
    for (int k = 600; k < 700; k += 2) b.push(pose(s0 + k, 0, 0, 0), now + k + (k == 640 ? 12 : 0));
    CHECK(b.delay() <= MpPoseBuffer::kMinDelay + 1.01f);
    // power save: arrivals bunched every 6 turns, and a 20-turn sleep every ~2 s
    uint32_t lastAt = 0;
    for (int k = 700; k < 1600; k += 2) {
        uint32_t at = now + (uint32_t)k;
        at = (at + 5) / 6 * 6;                                                          // the next wake (100 ms)
        if (k % 120 < 20) at = now + (uint32_t)(k / 120) * 120 + 22;                  // a 22-turn sleep every 2 s
        if (at < lastAt) at = lastAt;
        lastAt = at;
        b.push(pose(s0 + k, 0, 0, 0), at);
    }
    printf("delay: power save %.1f turns\n", b.delay());
    CHECK(b.delay() >= 14.0f && b.delay() <= MpPoseBuffer::kMaxDelay);
    // clean again for 15 s: back to the minimum
    for (int k = 1600; k < 2500; k += 2) b.push(pose(s0 + k, 0, 0, 0), now + k + 30);   // (+30: no earlier than before)
    printf("delay: clean again %.1f turns\n", b.delay());
    CHECK(b.delay() <= MpPoseBuffer::kMinDelay + 1.01f);
    // our own game hitches (250 ms without turns, packets keep coming): the clock relation changes, the link did not: the
    // delay is not raised for the next seconds
    MpPoseBuffer h;
    h.reset();
    uint32_t hn = 500;
    for (int k = 0; k < 600; k += 2) {
        if (k >= 300 && k < 315) { h.push(pose(s0 + k, 0, 0, 0), hn); continue; }   // (hn stands still: the hitch)
        hn = 500 + (uint32_t)k - (k >= 315 ? 15u : 0u);
        h.push(pose(s0 + k, 0, 0, 0), hn);
    }
    printf("delay: 1 s after a 250 ms hitch of our own %.1f turns\n", h.delay());
    CHECK(h.delay() <= MpPoseBuffer::kMinDelay + 1.01f);
    // 20 % loss on a clean link: a lost STATE doubles the interval the next one closes
    MpPoseBuffer l;
    l.reset();
    for (int k = 0; k < 1200; k += 2)
        if (frand(0, 1) >= 0.2f) l.push(pose(s0 + k, 0, 0, 0), now + k);
    printf("delay: 20 %% loss %.1f turns\n", l.delay());
    CHECK(l.delay() >= 5.0f && l.delay() <= 9.0f);   // (at least one lost STATE covered)
}

// ---- the error correction ----------------------------------------------------------------------------------------------------
// The sender runs +x at 10 units/turn, turns to +z at turn 300; the STATEs from 296 to 320 are held back and all arrive at once:
// the receiver extrapolates +x, holds, then learns the turn. v1 jumps; v2 glides (a catch-up, no step).
struct GlideResult { float worstStep, worstAcc, endErr; int holds; };
template < class Buf >
static GlideResult runLateStateGlide() {
    Buf b;
    b.reset();
    const uint32_t s0 = 1000, r0 = 10;
    auto truth = [](int k, float out[3]) {
        out[1] = 0.0f;
        if (k < 300) { out[0] = 10.0f * k; out[2] = 0.0f; }
        else { out[0] = 3000.0f; out[2] = 10.0f * (k - 300); }
    };
    float prev[3] = {0, 0, 0}, prev2[3] = {0, 0, 0};
    GlideResult g = {0.0f, 0.0f, 0.0f, 0};
    for (int k = 0; k < 420; k++) {
        const uint32_t now = r0 + (uint32_t)k;
        // packets: sent at even sender turns, 2 turns of latency; 296..320 all arrive at sender turn 324
        for (int j = 0; j <= k; j += 2) {
            uint32_t at = (j >= 296 && j <= 320) ? 324u : (uint32_t)j + 2u;
            if (at == (uint32_t)k) {
                float p[3];
                truth(j, p);
                b.push(pose(s0 + j, p[0], p[1], p[2]), now);
            }
        }
        MpPoseF o;
        MpSampleResult r = b.sample(now, &o);
        if (r == MP_SAMPLE_HOLD) g.holds++;
        if (k > 60) {
            g.worstStep = fmaxf(g.worstStep, dist3(o.pos, prev));
            float acc[3] = {o.pos[0] - 2 * prev[0] + prev2[0], o.pos[1] - 2 * prev[1] + prev2[1], o.pos[2] - 2 * prev[2] + prev2[2]};
            g.worstAcc = fmaxf(g.worstAcc, MpPoseMath::len3(acc));
        }
        for (int i = 0; i < 3; i++) { prev2[i] = prev[i]; prev[i] = o.pos[i]; }
        if (k == 419) {
            float t[3];
            float st = (float)(int32_t)(b.s[b.count - 1].turn - s0) + b.renderRel(now);
            float t1[3];
            truth((int)floorf(st), t);
            truth((int)floorf(st) + 1, t1);
            for (int i = 0; i < 3; i++) t[i] += (t1[i] - t[i]) * (st - floorf(st));
            g.endErr = dist3(o.pos, t);
        }
    }
    return g;
}

static void testLateStateGlides() {
    GlideResult v1 = runLateStateGlide< MpPoseBufferV1 >(), v2 = runLateStateGlide< MpPoseBuffer >();
    printf("glide: worst step v1 %.1f v2 %.1f units/turn (running 10), worst acceleration v1 %.1f v2 %.1f, holds v1 %d v2 %d, "
           "error at the end %.2f\n", v1.worstStep, v2.worstStep, v1.worstAcc, v2.worstAcc, v1.holds, v2.holds, v2.endErr);
    CHECK(v1.worstStep > 60.0f);                  // (the case is real: v1 jumps)
    CHECK(v2.worstStep < 0.4f * v1.worstStep);    // v2: a catch-up at most ~2.5x the running speed, never a jump
    CHECK(v2.worstStep < 26.0f);
    CHECK(v2.worstAcc < 0.3f * v1.worstAcc);
    CHECK(v2.endErr < 1.0f);                      // the correction is gone
}

// The sender's game hitches for 20 turns (its turn counter stops, then goes on): every STATE after it is 20 turns "late". The
// render point ran past the newest sample; it must come back quickly (extrapolating / holding as little as possible).
template < class Buf >
static int runSenderHitch() {
    Buf b;
    b.reset();
    int beyond = 0;
    for (int k = 0; k < 900; k++) {   // k: wall turns (the receiver's); sender turn = k, or k - 20 after the hitch at 400
        const uint32_t now = 10 + (uint32_t)k;
        int st = k < 400 ? k : (k < 420 ? -1 : k - 20);
        if (st >= 0 && (st & 1) == 0) b.push(pose(7000 + (uint32_t)st, 10.0f * st, 0, 0), now + 2);
        MpPoseF o;
        if (b.sample(now + 2, &o) != MP_SAMPLE_INTERP && k > 60) beyond++;
    }
    return beyond;
}

static void testSenderHitch() {
    int v1 = runSenderHitch< MpPoseBufferV1 >(), v2 = runSenderHitch< MpPoseBuffer >();
    printf("sender hitch (20 turns): turns past the newest sample v1 %d, v2 %d\n", v1, v2);
    CHECK(v2 <= v1);
    CHECK(v2 < 70);   // (the 20 turns themselves, then the 16 STATEs the path-follow window needs, then the slew back)
}

static void testOutOfOrderGlides() {
    // sample 102 arrives after 104 (it was extrapolated over): inserted, and the change glides
    MpPoseBuffer b;
    b.reset();
    for (uint32_t k = 0; k < 100; k += 2) b.push(pose(1000 + k, 5.0f * k, 0, 0), 10 + k);
    MpPoseF o;
    for (uint32_t now = 10; now < 112; now++) b.sample(now, &o);
    // a zig: 100 is off the line by 60 units, sent late
    b.push(pose(1102, 510.0f, 0, 0), 112);
    float p0[3] = {o.pos[0], o.pos[1], o.pos[2]};
    b.sample(112, &o);
    b.push(pose(1100, 500.0f, 0, 60.0f), 113);
    b.sample(113, &o);
    CHECK(fabsf(o.pos[2]) < 6.0f);   // no jump sideways
    CHECK(dist3(o.pos, p0) < 16.0f);
}

static void testSnapCases() {
    // a warp of 6000 units in the newest sample while extrapolating: snapped, not glided across the level
    MpPoseBuffer b;
    b.reset();
    for (uint32_t k = 0; k < 60; k += 2) b.push(pose(1000 + k, 5.0f * k, 0, 0), 10 + k);
    MpPoseF o;
    for (uint32_t now = 10; now < 90; now++) b.sample(now, &o);   // extrapolating, then held
    uint32_t snaps0 = b.snaps;
    for (uint32_t k = 60; k < 100; k += 2) b.push(pose(1000 + k, 6000.0f, 0, 0), 90 + (k - 60) / 2);
    int between = 0;
    for (uint32_t now = 90; now < 160; now++) {
        b.sample(now, &o);
        if (o.pos[0] > 400.0f && o.pos[0] < 5900.0f) between++;
    }
    CHECK(between == 0);
    CHECK(b.snaps > snaps0);
    // visibility: a puppet that becomes visible at a new place is not glided there
    MpPoseBuffer v;
    v.reset();
    for (uint32_t k = 0; k < 40; k += 2) v.push(pose(1000 + k, 0, 0, 0, 0), 10 + k);   // invisible at 0
    for (uint32_t now = 10; now < 60; now++) v.sample(now, &o);
    v.push(pose(1040, 300, 0, 0), 60);   // visible at 300: (still extrapolating 0 at the render point; no correction)
    for (uint32_t now = 60; now < 90; now++) {
        v.sample(now, &o);
        if ((o.flags & MP_POSE_VISIBLE) != 0) CHECK(o.pos[0] == 300.0f);   // (no slide from where it was hidden either)
    }
    // a puppet not shown for a while (killed, culled by the caller) gets no correction from an old view
    MpPoseBuffer g;
    g.reset();
    for (uint32_t k = 0; k < 40; k += 2) g.push(pose(1000 + k, 5.0f * k, 0, 0), 10 + k);
    g.sample(50, &o);
    for (uint32_t k = 40; k < 200; k += 2) g.push(pose(1000 + k, 1000.0f + 5.0f * k, 0, 0), 10 + k);
    g.sample(210, &o);
    MpPoseF raw;
    g.sampleAt(g.renderRel(210), &raw);
    CHECK(dist3(o.pos, raw.pos) < 0.001f);
}

// ---- extrapolation --------------------------------------------------------------------------------------------------------
static void testExtrapolationGround() {
    MpPoseF o;
    // a landing: falling at 20 units/turn, the newest sample on the ground at y=0: the extrapolation stays on the ground
    MpPoseBuffer b;
    b.reset();
    const uint16_t air = MP_POSE_VISIBLE, gnd = MP_POSE_VISIBLE | MP_POSE_ON_GROUND;
    b.push(pose(100, 0, 80, 0, air), 0);
    b.push(pose(102, 10, 40, 0, air), 2);
    b.push(pose(104, 20, 0, 0, gnd), 4);
    b.sampleAt(6.0f, &o);
    const float te6 = MpPoseBuffer::extrapDistTime(6.0f);
    CHECK(fabsf(o.pos[1]) < 0.001f && fabsf(o.pos[0] - (20.0f + 5.0f * te6)) < 0.01f);
    // walking up a 30 degree slope: kept (coasting along the exact slope)
    MpPoseBuffer s;
    s.reset();
    for (uint32_t k = 0; k <= 8; k += 2) s.push(pose(200 + k, 8.66f * k, 5.0f * k, 0, gnd), k);
    s.sampleAt(4.0f, &o);
    const float te4 = MpPoseBuffer::extrapDistTime(4.0f);
    CHECK(fabsf(o.pos[1] - (40.0f + 5.0f * te4)) < 0.05f && fabsf(o.pos[0] - 8.66f * (8.0f + te4)) < 0.05f);
    // a jump arc: y = 30 t - 0.8 t^2 (gravity 1.6 units/turn^2): the extrapolation follows the arc (damped time along the parabola)
    MpPoseBuffer j;
    j.reset();
    auto y = [](float t) { return 30.0f * t - 0.8f * t * t; };
    for (uint32_t k = 2; k <= 10; k += 2) j.push(pose(300 + k, 4.0f * k, y((float)k), 0, air), k);
    j.sampleAt(8.0f, &o);
    const float te8 = MpPoseBuffer::extrapDistTime(8.0f);
    const float want = y(10.0f + te8), chord = y(10.0f) + (y(10.0f) - y(8.0f)) / 2.0f * te8;
    printf("extrap: arc at +8 turns (te=%.2f) %.1f (truth %.1f, straight line %.1f)\n", te8, o.pos[1], want, chord);
    CHECK(fabsf(o.pos[1] - want) < 1.0f);
    CHECK(fabsf(o.pos[0] - (40.0f + 4.0f * te8)) < 0.01f);
    // a long fall: the extrapolated speed stops at kMaxFallSpeed
    MpPoseBuffer f;
    f.reset();
    for (uint32_t k = 0; k <= 4; k += 2) f.push(pose(400 + k, 0, -1.6f * 0.5f * (float)(k + 30) * (float)(k + 30), 0, air), k);
    MpPoseF a, c;
    f.sampleAt(8.0f, &a);
    f.sampleAt(9.0f, &c);
    CHECK(a.pos[1] - c.pos[1] <= MpPoseBuffer::kMaxFallSpeed + 0.01f);
    // swimming: plain straight line (with smooth coasting decay)
    MpPoseBuffer w;
    w.reset();
    const uint16_t water = MP_POSE_VISIBLE | MP_POSE_IN_WATER;
    w.push(pose(500, 0, 0, 0, water), 0);
    w.push(pose(502, 0, 10, 0, water), 2);
    w.push(pose(504, 0, 30, 0, water), 4);
    w.sampleAt(4.0f, &o);
    CHECK(fabsf(o.pos[1] - (30.0f + 10.0f * te4)) < 0.01f);
}

static void testAnimationFrames() {
    // the sender's animation runs at rate 1; STATEs 40..56 arrive late in one go after the receiver extrapolated (frames keep
    // running), then held (frame stops): when they arrive the frame glides back onto the timeline instead of jumping
    MpPoseBuffer b;
    b.reset();
    float prevFrame = -1.0f, worstJump = 0.0f;
    for (int k = 0; k < 160; k++) {
        const uint32_t now = 10 + (uint32_t)k;
        for (int j = 0; j <= k; j += 2) {
            int at = (j >= 40 && j <= 56) ? 66 : j + 1;
            if (at == k) {
                MpPoseF p = pose(2000 + (uint32_t)j, 3.0f * j, 0, 0);
                p.frame = (float)j;
                b.push(p, now);
            }
        }
        MpPoseF o;
        if (b.sample(now, &o) == MP_SAMPLE_NONE) continue;
        if (prevFrame >= 0.0f && k > 20) worstJump = fmaxf(worstJump, fabsf(o.frame - prevFrame - 1.0f));
        prevFrame = o.frame;
    }
    printf("anim: worst frame step deviation %.2f frames\n", worstJump);
    CHECK(worstJump < 1.2f);   // (v1: the frame jumps back by the extrapolated frames)
    // a loop wrap is never glided: frame 58 -> 1 of the same animation
    MpPoseBuffer w;
    w.reset();
    MpPoseF a = pose(3000, 0, 0, 0), c = pose(3002, 0, 0, 0), o;
    a.frame = 56.0f; c.frame = 58.0f;
    w.push(a, 0);
    w.push(c, 2);
    for (uint32_t now = 2; now < 14; now++) w.sample(now, &o);
    MpPoseF d = pose(3004, 0, 0, 0);
    d.frame = 0.5f;
    w.push(d, 14);
    w.sample(14, &o);
    CHECK(w.corrF == 0.0f);
}

// ---- a lossy, jittery stream of a turning, jumping motion -------------------------------------------------------------------
struct NetResult { int holds, snaps, kinks, turns; float delay; };
template < class Buf >
static NetResult runNetwork(bool bad) {
    Buf b;
    b.reset();
    struct Pend { uint32_t at; MpPoseF p; };
    static Pend q[4096];
    int nq = 0;
    const int turns = 3600;
    float x = 0, z = 0, h = 0, vu = 0, yaw = 0;
    static float tx[3600][3];
    uint32_t rng = 99;
    auto rnd = [&rng]() { rng = rng * 1664525u + 1013904223u; return ((rng >> 8) & 0xFFFF) / 65535.0f; };
    for (int k = 0; k < turns; k++) {
        if (k % 90 == 0) yaw += (rnd() - 0.5f) * 3.0f;
        if (k % 140 == 0 && h <= 0.0f) vu = 24.0f;
        x += cosf(yaw) * 12.0f; z += sinf(yaw) * 12.0f;
        if (h > 0.0f || vu > 0.0f) { h += vu; vu -= 1.6f; if (h <= 0.0f) { h = 0.0f; vu = 0.0f; } }
        tx[k][0] = x; tx[k][1] = h; tx[k][2] = z;
        if (k % 2 != 0) continue;
        if (!bad) {
            q[nq++] = {(uint32_t)k + 2u, pose(90000 + (uint32_t)k, x, h, z, MP_POSE_VISIBLE | (h <= 0.0f ? MP_POSE_ON_GROUND : 0))};
        } else if (rnd() >= 0.1f) {   // 10 % loss, 150 +- 80 ms
            float lat = 150.0f + 80.0f * (rnd() + rnd() + rnd() - 1.5f) * 2.0f;
            if (lat < 50.0f) lat = 50.0f;
            q[nq++] = {(uint32_t)k + (uint32_t)(lat / 16.667f), pose(90000 + (uint32_t)k, x, h, z, MP_POSE_VISIBLE | (h <= 0.0f ? MP_POSE_ON_GROUND : 0))};
        }
    }
    NetResult res = {0, 0, 0, 0, 0.0f};
    float prev[3] = {0, 0, 0}, prevT[3] = {0, 0, 0}, prev2[3] = {0, 0, 0}, prev2T[3] = {0, 0, 0};
    for (int k = 0; k < turns; k++) {
        const uint32_t now = 7 + (uint32_t)k;
        for (int i = 0; i < nq; i++)
            if (q[i].at == (uint32_t)k) b.push(q[i].p, now);
        MpPoseF o;
        MpSampleResult r = b.sample(now, &o);
        if (r == MP_SAMPLE_NONE) continue;
        float st = (float)(int32_t)(b.s[b.count - 1].turn - 90000) + b.renderRel(now);
        int i0 = (int)st;
        if (i0 < 0) i0 = 0;
        if (i0 > turns - 2) i0 = turns - 2;
        float f = st - (float)i0, t[3];
        for (int i = 0; i < 3; i++) t[i] = tx[i0][i] + (tx[i0 + 1][i] - tx[i0][i]) * f;
        if (k > 180) {
            res.turns++;
            res.holds += r == MP_SAMPLE_HOLD;
            float s1[3], a[3];
            for (int i = 0; i < 3; i++) {
                s1[i] = (o.pos[i] - prev[i]) - (t[i] - prevT[i]);
                a[i] = (o.pos[i] - 2 * prev[i] + prev2[i]) - (t[i] - 2 * prevT[i] + prev2T[i]);
            }
            res.snaps += MpPoseMath::len3(s1) > 25.0f;
            res.kinks += MpPoseMath::len3(a) > 6.0f;
        }
        for (int i = 0; i < 3; i++) { prev2[i] = prev[i]; prev[i] = o.pos[i]; prev2T[i] = prevT[i]; prevT[i] = t[i]; }
    }
    res.delay = b.delay();
    return res;
}

static void testBadNetwork() {
    NetResult ideal = runNetwork< MpPoseBuffer >(false), v1 = runNetwork< MpPoseBufferV1 >(true), v2 = runNetwork< MpPoseBuffer >(true);
    printf("bad network (150 +- 80 ms, 10 %% loss), %d turns: kinks (bumps > 6 units/turn^2 beyond the motion's own) ideal link %d, "
           "v1 %d, v2 %d; snaps v1 %d v2 %d; holds v1 %d v2 %d; delay v2 %.1f turns\n", v2.turns, ideal.kinks, v1.kinks, v2.kinks,
           v1.snaps, v2.snaps, v1.holds, v2.holds, v2.delay);
    CHECK(v2.holds == 0);
    CHECK(v2.snaps == 0);
    CHECK(v2.kinks <= ideal.kinks * 3 / 2 + 5);   // near what 30 Hz interpolation itself gives
    CHECK(v2.kinks * 3 < v1.kinks * 2);
}

static void testSendGateAndPchip() {
    // wave8: MpSendGate::dueTurn() sends at a steady 2-turn cadence (~30 Hz), respects kTurnMinNs (18 ms) against bursts,
    // and falls back to kSlowFallbackNs (40 ms) when local FPS slows.
    MpSendGate g;
    g.reset();
    int64_t t = 1000000000ll;
    CHECK(g.dueTurn(1, t));                               // first turn always sends
    CHECK(!g.dueTurn(2, t + 16666667ll));                 // 1 turn later: wait
    CHECK(!g.dueTurn(3, t + 15000000ll));                 // 2 turns but < 18 ms burst: wait
    CHECK(g.dueTurn(3, t + 33333334ll));                  // 2 turns & >= 18 ms: send
    t += 33333334ll;
    CHECK(g.dueTurn(4, t + 45000000ll));                  // 1 turn at slow fps (>= 40 ms): fallback send
    MpSendGate::tMinNs = 50000000ll;                      // explicit env override uses wall clock
    t += 45000000ll;
    CHECK(!g.dueTurn(10, t + 40000000ll));
    CHECK(g.dueTurn(12, t + 55000000ll));
    MpSendGate::tMinNs = MpSendGate::kMinNs;

    // wave8: monotone PCHIP cubic Hermite interpolation never overshoots a step-and-stop motion (no bounce past the target).
    MpPoseBuffer b;
    b.reset();
    const uint16_t gnd = MP_POSE_VISIBLE | MP_POSE_ON_GROUND;
    b.push(pose(100, 0.0f, 0.0f, 0.0f, gnd), 0);
    b.push(pose(102, 20.0f, 0.0f, 0.0f, gnd), 2);
    b.push(pose(104, 40.0f, 0.0f, 0.0f, gnd), 4);
    b.push(pose(106, 40.0f, 0.0f, 0.0f, gnd), 6);
    b.push(pose(108, 40.0f, 0.0f, 0.0f, gnd), 8);
    MpPoseF o;
    for (int step = 0; step <= 20; step++) {
        b.sampleAt(-4.0f + 0.2f * step, &o);
        CHECK(o.pos[0] >= -0.01f && o.pos[0] <= 40.01f);
    }
    printf("sendgate + pchip: ok\n");
}

int main() {
    testDelayAdapts();
    testLateStateGlides();
    testSenderHitch();
    testOutOfOrderGlides();
    testSnapCases();
    testExtrapolationGround();
    testAnimationFrames();
    testBadNetwork();
    testSendGateAndPchip();
    printf("%s: %d checks, %d failed\n", gFail ? "FAILED" : "OK", gChecks, gFail);
    return gFail ? 1 : 0;
}
