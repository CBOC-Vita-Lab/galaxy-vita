// Host unit tests for the puppet pose maths (decomp/include/Game/MP/MpPoseMath.hpp).
// Build: clang++ -std=c++20 -fsanitize=address,undefined -g -Iplatform/include -Idecomp/include tests/mp/puppet_pose_test.cpp -o /tmp/puppet_pose_test
#include "Game/MP/MpPoseMath.hpp"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int gFail = 0, gChecks = 0;
#define CHECK(c)                                                                                                                       \
    do {                                                                                                                               \
        gChecks++;                                                                                                                     \
        if (!(c)) {                                                                                                                    \
            if (gFail++ < 40)                                                                                                          \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                                        \
        }                                                                                                                              \
    } while (0)

static uint32_t sRng = 12345;
static float frand(float lo, float hi) {
    sRng = sRng * 1664525u + 1013904223u;
    return lo + (hi - lo) * ((sRng >> 8) & 0xFFFF) / 65535.0f;
}

// Rotation matrix from axis/angle (rows, column vectors, like PSMTXRotAxisRad).
static void mtxAxisAngle(float m[3][4], float ax, float ay, float az, float ang) {
    float n = sqrtf(ax * ax + ay * ay + az * az);
    ax /= n; ay /= n; az /= n;
    float c = cosf(ang), s = sinf(ang), t = 1.0f - c;
    m[0][0] = t * ax * ax + c;      m[0][1] = t * ax * ay - s * az; m[0][2] = t * ax * az + s * ay; m[0][3] = 10.0f;
    m[1][0] = t * ax * ay + s * az; m[1][1] = t * ay * ay + c;      m[1][2] = t * ay * az - s * ax; m[1][3] = 20.0f;
    m[2][0] = t * ax * az - s * ay; m[2][1] = t * ay * az + s * ax; m[2][2] = t * az * az + c;      m[2][3] = 30.0f;
}

static float maxMtxErr(const float a[3][4], const float b[3][4]) {
    float e = 0.0f;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) e = fmaxf(e, fabsf(a[r][c] - b[r][c]));
    return e;
}

static MpPoseF basePose(uint32_t turn) {
    MpPoseF p;
    memset(&p, 0, sizeof p);
    p.turn = turn;
    p.quat[3] = 1.0f;
    p.scale = 1.0f;
    p.weights[3] = 1.0f;
    p.anim = 0x1234;
    p.rate = 1.0f;
    p.flags = MP_POSE_VISIBLE | MP_POSE_ON_GROUND;
    return p;
}

static void testQuatRoundTrip() {
    float worstMtx = 0.0f, worstAng = 0.0f;
    for (int i = 0; i < 20000; i++) {
        float m[3][4];
        mtxAxisAngle(m, frand(-1, 1), frand(-1, 1), frand(-1, 1) + 0.001f, frand(-3.14159f, 3.14159f));
        // a base matrix may carry a little scale: the encoder normalises columns
        float sc = frand(0.5f, 2.0f);
        float ms[3][4];
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) ms[r][c] = c < 3 ? m[r][c] * sc : m[r][c];
        MpPoseF p = basePose(1);
        MpPoseMath::quatFromMtx(ms, p.quat);
        MpPose wire;
        MpPoseMath::encodePose(p, &wire);
        uint8_t buf[128];
        MpWriter w(buf, sizeof buf);
        mp_write_pose(w, wire);
        CHECK(w.ok() && w.n == MP_POSE_WIRE_BYTES);
        MpReader r(buf, w.n);
        MpPose back;
        CHECK(mp_read_pose(r, &back) && r.done());
        MpPoseF d;
        CHECK(MpPoseMath::decodePose(back, &d));
        float m2[3][4];
        MpPoseMath::mtxFromQuat(d.quat, m2);
        worstMtx = fmaxf(worstMtx, maxMtxErr(m, m2));
        // angle = 4 asin(|q1 - q2| / 2) with the signs aligned (well conditioned near 0, unlike acos of the dot product)
        double sg = ((double)p.quat[0] * d.quat[0] + (double)p.quat[1] * d.quat[1] + (double)p.quat[2] * d.quat[2] +
                     (double)p.quat[3] * d.quat[3]) < 0.0 ? -1.0 : 1.0;
        double n2 = 0.0;
        for (int k = 0; k < 4; k++) n2 += ((double)p.quat[k] - sg * d.quat[k]) * ((double)p.quat[k] - sg * d.quat[k]);
        worstAng = fmaxf(worstAng, (float)(4.0 * asin(sqrt(n2) / 2.0)));
    }
    printf("quat round trip: worst matrix element error %.6f, worst angle %.6f rad\n", worstMtx, worstAng);
    CHECK(worstMtx < 0.0004f);
    CHECK(worstAng < 0.0005f);
}

static void testFieldQuantisation() {
    MpPoseF p = basePose(77);
    p.pos[0] = 12345.678f; p.pos[1] = -5000.25f; p.pos[2] = 0.5f;
    p.vel[0] = 31.0f; p.vel[1] = -300.0f; p.vel[2] = 1.3f;
    p.scale = 1.37f; p.frame = 123.4567f; p.rate = 0.73f; p.upperFrame = 9.9f; p.upperAnim = 99;
    p.weights[0] = 0.25f; p.weights[1] = 1.5f; p.weights[2] = -1.0f; p.weights[3] = 0.6f;
    p.flags = MP_POSE_VISIBLE | MP_POSE_UPPER; p.form = MP_FORM_BEE; p.sceneSerial = 513; p.zone = 2;
    p.hasTracks = true; p.tracks[0] = 1; p.tracks[1] = 0xFFFFFFFF; p.tracks[2] = 7; p.tracks[3] = 0;
    uint8_t buf[256];
    size_t n = MpPoseMath::writeState(buf, sizeof buf, p);
    CHECK(n == MP_POSE_WIRE_BYTES + MP_POSE_TAIL_BYTES);
    MpPoseF d;
    CHECK(MpPoseMath::readState(buf, n, &d));
    CHECK(d.pos[0] == p.pos[0] && d.pos[1] == p.pos[1] && d.pos[2] == p.pos[2]);   // positions are f32: exact
    CHECK(fabsf(d.vel[0] - 32.0f) < 1.01f && d.vel[1] == -254.0f);                 // /2, clamped to +-127
    CHECK(fabsf(d.scale - 1.37f) <= 1.0f / 128.0f);
    CHECK(fabsf(d.frame - p.frame) <= 1.0f / 32.0f);
    CHECK(fabsf(d.rate - p.rate) <= 1.0f / 64.0f);
    CHECK(d.weights[1] == 1.0f && d.weights[2] == 0.0f && fabsf(d.weights[0] - 0.25f) < 0.003f);
    CHECK(d.upperAnim == 99 && fabsf(d.upperFrame - 9.9f) <= 1.0f / 32.0f);
    CHECK(d.form == MP_FORM_BEE && d.sceneSerial == 513 && d.zone == 2 && d.turn == 77);
    CHECK(d.hasTracks && d.tracks[1] == 0xFFFFFFFF && d.tracks[2] == 7);
    // without the tail
    p.hasTracks = false;
    n = MpPoseMath::writeState(buf, sizeof buf, p);
    CHECK(n == MP_POSE_WIRE_BYTES && MpPoseMath::readState(buf, n, &d) && !d.hasTracks);
    // upper anim is ignored unless flagged
    p.flags = MP_POSE_VISIBLE;
    n = MpPoseMath::writeState(buf, sizeof buf, p);
    CHECK(MpPoseMath::readState(buf, n, &d) && d.upperAnim == 0);
}

static void testHostileInput() {
    MpPoseF p = basePose(5);
    uint8_t buf[256];
    size_t n = MpPoseMath::writeState(buf, sizeof buf, p);
    MpPoseF d;
    for (size_t cut = 0; cut < n; cut++) CHECK(!MpPoseMath::readState(buf, cut, &d));   // every truncation is dropped
    // wrong tail length / tag
    p.hasTracks = true;
    n = MpPoseMath::writeState(buf, sizeof buf, p);
    CHECK(!MpPoseMath::readState(buf, n - 1, &d));
    CHECK(!MpPoseMath::readState(buf, MP_POSE_WIRE_BYTES + 1, &d));
    buf[MP_POSE_WIRE_BYTES] = 0x55;
    CHECK(!MpPoseMath::readState(buf, n, &d));
    // NaN / huge positions, bad form, zero quaternion
    MpPose w;
    MpPoseMath::encodePose(basePose(1), &w);
    w.pos[1] = NAN;
    MpWriter wr(buf, sizeof buf); mp_write_pose(wr, w);
    CHECK(!MpPoseMath::readState(buf, wr.n, &d));
    MpPoseMath::encodePose(basePose(1), &w); w.pos[0] = 2.0e6f;
    MpWriter wr2(buf, sizeof buf); mp_write_pose(wr2, w);
    CHECK(!MpPoseMath::readState(buf, wr2.n, &d));
    MpPoseMath::encodePose(basePose(1), &w); w.form = MP_FORM_COUNT;
    MpWriter wr3(buf, sizeof buf); mp_write_pose(wr3, w);
    CHECK(MpPoseMath::readState(buf, wr3.n, &d) && d.form == MP_FORM_NORMAL);   // (puppetprep) an unknown form is clamped, not dropped
    MpPoseMath::encodePose(basePose(1), &w); w.quat[0] = w.quat[1] = w.quat[2] = w.quat[3] = 0;
    MpWriter wr4(buf, sizeof buf); mp_write_pose(wr4, w);
    CHECK(!MpPoseMath::readState(buf, wr4.n, &d));
    // random garbage never crashes and never yields a non-finite pose
    int accepted = 0;
    for (int i = 0; i < 200000; i++) {
        size_t len = (size_t)(frand(0, 80));
        for (size_t k = 0; k < len; k++) buf[k] = (uint8_t)frand(0, 255.99f);
        if (MpPoseMath::readState(buf, len, &d)) {
            accepted++;
            for (int k = 0; k < 3; k++) CHECK(MpPoseMath::finitef(d.pos[k]) && fabsf(d.pos[k]) < 1.0e6f);
            float qn = d.quat[0] * d.quat[0] + d.quat[1] * d.quat[1] + d.quat[2] * d.quat[2] + d.quat[3] * d.quat[3];
            CHECK(fabsf(qn - 1.0f) < 0.001f);
            CHECK(d.form < MP_FORM_COUNT && d.scale > 0.0f);
        }
    }
    printf("fuzz: %d of 200000 random payloads accepted (all finite)\n", accepted);
}

// A sender at 30 Hz (every 2nd turn) with a fixed one-way latency and optional jitter; the receiver samples every turn.
struct Sim {
    MpPoseBuffer buf;
    uint32_t senderStart = 1000000, recvStart = 5;
};

static void testInterpolationTiming() {
    Sim sim;
    sim.buf.reset();
    // pending packets: (arrival local turn, pose)
    struct Pend { uint32_t at; MpPoseF p; } q[4096];
    int nq = 0;
    for (uint32_t k = 0; k < 600; k += 2) {
        MpPoseF p = basePose(sim.senderStart + k);
        p.pos[0] = (float)k * 10.0f;   // 10 units/turn along x
        p.frame = (float)k;            // rate 1
        uint32_t lat = 4 + (uint32_t)frand(0, 3.99f);   // 4..7 turns one way (67..117 ms) with jitter
        q[nq++] = {sim.recvStart + k + lat, p};
    }
    float worstPos = 0.0f;
    int interp = 0, extrap = 0, hold = 0;
    for (uint32_t now = sim.recvStart; now < sim.recvStart + 600; now++) {
        for (int i = 0; i < nq; i++)
            if (q[i].at == now) sim.buf.push(q[i].p, now);
        MpPoseF o;
        MpSampleResult r = sim.buf.sample(now, &o);
        if (now < sim.recvStart + 40) continue;   // settle
        if (r == MP_SAMPLE_INTERP) interp++;
        if (r == MP_SAMPLE_EXTRAP) extrap++;
        if (r == MP_SAMPLE_HOLD) hold++;
        // what we show must be a sender state from 'delay' + latency ago: the x position is monotonic and close to the
        // linear motion at the rendered sender turn
        float renderSenderTurn = (float)(int32_t)(sim.buf.s[sim.buf.count - 1].turn - sim.senderStart) + sim.buf.renderRel(now);
        float expect = renderSenderTurn * 10.0f;
        worstPos = fmaxf(worstPos, fabsf(o.pos[0] - expect));
        CHECK(fabsf(o.frame - renderSenderTurn) < 0.2f || r != MP_SAMPLE_INTERP);
    }
    float behind = -sim.buf.renderRel(sim.recvStart + 599);
    printf("timing: interp %d extrap %d hold %d, worst pos error %.3f, delay %.2f turns, jitter %.2f\n", interp, extrap, hold, worstPos,
           sim.buf.delay(), sim.buf.jitter);
    CHECK(worstPos < 0.5f);
    CHECK(interp > 500);
    CHECK(hold == 0);
    CHECK(sim.buf.delay() >= MpPoseBuffer::kMinDelay && sim.buf.delay() <= MpPoseBuffer::kMaxDelay);
    CHECK(behind >= 0.0f && behind <= 14.0f);
}

static void testExtrapolateThenHold() {
    MpPoseBuffer b;
    b.reset();
    for (uint32_t k = 0; k <= 20; k += 2) {
        MpPoseF p = basePose(100 + k);
        p.pos[2] = (float)k * 5.0f;
        b.push(p, 10 + k);   // zero latency
    }
    MpPoseF o;
    // now = 30 -> render relative = 0 - delay: inside
    CHECK(b.sample(30, &o) == MP_SAMPLE_INTERP);
    // packets stop: after the delay we extrapolate up to 9 turns, then hold
    uint32_t firstHold = 0, lastExtrap = 0;
    float holdPos = -1.0f;
    for (uint32_t now = 31; now < 80; now++) {
        MpSampleResult r = b.sample(now, &o);
        if (r == MP_SAMPLE_EXTRAP) lastExtrap = now;
        if (r == MP_SAMPLE_HOLD && firstHold == 0) { firstHold = now; holdPos = o.pos[2]; }
        if (r == MP_SAMPLE_HOLD) CHECK(o.pos[2] == holdPos);
    }
    float d = b.delay();
    printf("extrap: delay %.1f, last extrap at +%u, first hold at +%u, held z %.1f\n", d, lastExtrap - 30, firstHold - 30, holdPos);
    CHECK(firstHold != 0 && lastExtrap != 0);
    CHECK(fabsf((float)(firstHold - 30) - (d + MpPoseBuffer::kMaxExtrap + 1.0f)) <= 1.0f);
    CHECK(fabsf(holdPos - (100.0f + 5.0f * MpPoseBuffer::extrapDistTime(MpPoseBuffer::kMaxExtrap))) < 0.01f);   // damped coasting up to kMaxExtrap
    // fast motion (launch star): at most 2 turns of extrapolation
    MpPoseBuffer f;
    f.reset();
    for (uint32_t k = 0; k <= 10; k += 2) {
        MpPoseF p = basePose(500 + k);
        p.pos[0] = (float)k * 150.0f;
        f.push(p, 10 + k);
    }
    MpPoseF last;
    f.sample(100, &last);
    CHECK(fabsf(last.pos[0] - (1500.0f + MpPoseBuffer::extrapDistTime(2.0f) * 150.0f)) < 0.01f);
}

static void testOrderingAndDuplicates() {
    MpPoseBuffer b;
    b.reset();
    CHECK(b.push(basePose(10), 1));
    CHECK(b.push(basePose(14), 3));
    CHECK(!b.push(basePose(14), 3));     // duplicate
    CHECK(b.push(basePose(12), 4));      // late but in range: inserted in order
    CHECK(b.count == 3 && b.s[0].turn == 10 && b.s[1].turn == 12 && b.s[2].turn == 14);
    // (netsmooth: generic in N, which grew from 8 to 12 for the longer jitter buffer)
    const uint32_t lastT = 10 + 2 * (MpPoseBuffer::N - 1) + 2;   // N + 1 samples 10, 12, 14, ..., lastT (12 lost its 13 below)
    for (uint32_t t = 16; t <= lastT; t += 2) CHECK(b.push(basePose(t), 6));
    CHECK(b.count == MpPoseBuffer::N && b.s[0].turn == 12 && b.s[MpPoseBuffer::N - 1].turn == lastT);
    CHECK(!b.push(basePose(8), 7));      // older than everything kept
    CHECK(b.push(basePose(13), 7));      // inside: the oldest is dropped
    CHECK(b.s[0].turn == 13 && b.s[1].turn == 14);
    // sender counter restart (a new process): starts again
    MpPoseBuffer r;
    r.reset();
    CHECK(r.push(basePose(500000), 7));
    CHECK(r.push(basePose(500002), 9));
    CHECK(r.push(basePose(3), 11));
    CHECK(r.count == 1 && r.s[0].turn == 3);
    // wrap-around of the 32-bit turn counter
    MpPoseBuffer w;
    w.reset();
    CHECK(w.push(basePose(0xFFFFFFFEu), 100));
    CHECK(w.push(basePose(0u), 102));
    CHECK(w.push(basePose(2u), 104));
    CHECK(w.s[0].turn == 0xFFFFFFFEu && w.s[2].turn == 2u);
    MpPoseF o;
    CHECK(w.sample(104, &o) != MP_SAMPLE_NONE);
}

static void testClockFollowsSlowerPath() {
    // latency rises from 3 to 20 turns and stays: the render point must follow within about a second instead of holding
    MpPoseBuffer b;
    b.reset();
    int holds = 0;
    for (uint32_t k = 0; k < 400; k += 2) {
        uint32_t lat = k < 100 ? 3 : 20;
        b.push(basePose(9000 + k), 50 + k + lat);
        if (k > 200) {
            MpPoseF o;
            for (uint32_t s = 0; s < 2; s++)
                if (b.sample(50 + k + lat + s, &o) == MP_SAMPLE_HOLD) holds++;
        }
    }
    printf("clock: holds after the latency step settled: %d\n", holds);
    CHECK(holds == 0);
}

static void testPacketLoss() {
    // 20% random loss plus one burst of 6 lost packets (200 ms): mostly interpolation, extrapolation only around the burst,
    // never a jump backwards in x (monotonic motion stays monotonic)
    MpPoseBuffer b;
    b.reset();
    int interp = 0, extrap = 0, hold = 0;
    float lastX = -1.0f;
    bool backwards = false;
    for (uint32_t now = 0; now < 1200; now++) {
        if ((now & 1) == 0) {
            bool burst = now >= 600 && now < 612;
            if (!burst && frand(0, 1) >= 0.2f) {
                MpPoseF p = basePose(70000 + now);
                p.pos[0] = (float)now * 3.0f;
                b.push(p, now + 5);   // arrives 5 turns later; pushed now with the arrival turn: same clock maths
            }
        }
        MpPoseF o;
        MpSampleResult r = b.sample(now + 5, &o);
        if (now < 30) continue;
        interp += r == MP_SAMPLE_INTERP;
        extrap += r == MP_SAMPLE_EXTRAP;
        hold += r == MP_SAMPLE_HOLD;
        if (o.pos[0] + 0.01f < lastX) backwards = true;
        lastX = o.pos[0];
    }
    printf("loss: interp %d extrap %d hold %d backwards %d\n", interp, extrap, hold, backwards ? 1 : 0);
    CHECK(interp > 1000);
    CHECK(hold < 10);
    CHECK(!backwards);
}

static void testArcLerp() {
    // a small planet of radius 300 centred at (1000, -200, 50); two samples 40 degrees apart on its surface
    float c[3] = {1000.0f, -200.0f, 50.0f}, R = 300.0f;
    float a0 = 0.2f, a1 = a0 + 40.0f * 3.14159265f / 180.0f;
    float ua[3] = {cosf(a0), sinf(a0), 0.0f}, ub[3] = {cosf(a1), sinf(a1), 0.0f};
    float pa[3], pb[3];
    for (int i = 0; i < 3; i++) { pa[i] = c[i] + ua[i] * R; pb[i] = c[i] + ub[i] * R; }
    float worstArc = 0.0f, worstLin = 0.0f;
    for (int s = 0; s <= 10; s++) {
        float t = s / 10.0f, o[3];
        MpPoseMath::arcLerp(pa, ua, pb, ub, t, o);
        float d[3] = {o[0] - c[0], o[1] - c[1], o[2] - c[2]};
        worstArc = fmaxf(worstArc, fabsf(MpPoseMath::len3(d) - R));
        float l[3];
        for (int i = 0; i < 3; i++) l[i] = pa[i] + (pb[i] - pa[i]) * t - c[i];
        worstLin = fmaxf(worstLin, fabsf(MpPoseMath::len3(l) - R));
    }
    printf("arc: radius error %.3f (straight lerp would cut %.1f units into the planet)\n", worstArc, worstLin);
    CHECK(worstArc < 0.5f);
    CHECK(worstLin > 15.0f);
    // flat ground: same up, plain lerp
    float up[3] = {0, 1, 0}, p0[3] = {0, 0, 0}, p1[3] = {100, 0, 0}, o[3];
    MpPoseMath::arcLerp(p0, up, p1, up, 0.5f, o);
    CHECK(o[0] == 50.0f && o[1] == 0.0f);
    // implausible centre (ups diverge but positions are far apart sideways): falls back to lerp, stays finite
    float uq[3] = {0.0f, 0.9f, 0.43588989f};
    float p2[3] = {0, 0, 0}, p3[3] = {5000, 3000, -7000};
    MpPoseMath::arcLerp(p2, up, p3, uq, 0.5f, o);
    CHECK(MpPoseMath::finitef(o[0]) && MpPoseMath::finitef(o[1]) && MpPoseMath::finitef(o[2]));
}

static void testSlerpAndInterpAnim() {
    MpPoseBuffer b;
    b.reset();
    MpPoseF a = basePose(100), c = basePose(102);
    // 90 degrees about Y
    c.quat[0] = 0.0f; c.quat[1] = sinf(0.785398f); c.quat[2] = 0.0f; c.quat[3] = cosf(0.785398f);
    a.frame = 10.0f; c.frame = 12.0f;
    b.push(a, 0);
    b.push(c, 2);
    MpPoseF o;
    b.sampleAt(-1.0f, &o);   // half way
    float half[4] = {0.0f, sinf(0.3926991f), 0.0f, cosf(0.3926991f)};
    CHECK(MpPoseMath::quatAngle(o.quat, half) < 1.0e-3f);
    CHECK(fabsf(o.frame - 11.0f) < 1.0e-4f);
    // animation change: the older animation advances on its own rate until the newer sample is reached
    MpPoseBuffer b2;
    b2.reset();
    MpPoseF x = basePose(200), y = basePose(204);
    x.anim = 1; x.frame = 30.0f; x.rate = 0.5f;
    y.anim = 2; y.frame = 0.0f;
    b2.push(x, 0);
    b2.push(y, 4);
    b2.sampleAt(-1.0f, &o);
    CHECK(o.anim == 1 && fabsf(o.frame - 31.5f) < 1.0e-4f);
    b2.sampleAt(0.0f, &o);
    CHECK(o.anim == 2 && o.frame == 0.0f);
    // a loop wrap within the same animation (frame went down) does not interpolate backwards
    MpPoseBuffer b3;
    b3.reset();
    MpPoseF l0 = basePose(300), l1 = basePose(302);
    l0.frame = 58.0f; l1.frame = 1.0f;
    b3.push(l0, 0);
    b3.push(l1, 2);
    b3.sampleAt(-1.0f, &o);
    CHECK(fabsf(o.frame - 59.0f) < 1.0e-4f);
    // warp: no sliding
    MpPoseBuffer b4;
    b4.reset();
    MpPoseF w0 = basePose(400), w1 = basePose(402);
    w1.pos[0] = 10000.0f;
    b4.push(w0, 0);
    b4.push(w1, 2);
    b4.sampleAt(-1.0f, &o);
    CHECK(o.pos[0] == 0.0f);
}

static void testNameMap() {
    static MpAnimNameMap m;
    m.clear();
    static const char* names[] = {"Wait", "Run", "LuigiRun", "\x8a\xee\x96\x7b", "WaitSlopeL", "WaitSlopeR", "SwimWait", "Jump"};
    for (const char* n : names) m.add(MpPoseMath::animHash(n), n);
    m.add(MpPoseMath::animHash("Run"), "Run");   // the same name twice is not a collision
    m.finish();
    CHECK(m.collisions == 0);
    for (const char* n : names) CHECK(m.find(MpPoseMath::animHash(n)) == n);
    CHECK(m.find(MpPoseMath::animHash("Nope")) == nullptr);
    // a real collision: two names, same hash -> unresolvable
    m.clear();
    m.add(42, "A");
    m.add(42, "B");
    m.add(7, "C");
    m.finish();
    CHECK(m.collisions == 1 && m.find(42) == nullptr && m.find(7) != nullptr);
    // capacity
    m.clear();
    static char store[2000][8];
    for (int i = 0; i < 2000; i++) { snprintf(store[i], 8, "b%d", i); m.add(MpPoseMath::animHash(store[i]), store[i]); }
    m.finish();
    CHECK(m.overflow == 2000 - MpAnimNameMap::CAP && m.n == MpAnimNameMap::CAP);
    for (int i = 1; i < m.n; i++) CHECK(m.e[i - 1].hash < m.e[i].hash);
    // MR::getHashCode compatibility: h = c + h * 31 over signed chars ("\x8a\xee\x96\x7b" = the default group name)
    uint32_t h = 0;
    for (const char* s = "\x8a\xee\x96\x7b"; *s; s++) h = (uint32_t)(int32_t)(signed char)*s + h * 31u;
    CHECK(MpPoseMath::animHash("\x8a\xee\x96\x7b") == h);
    CHECK(MpPoseMath::animHash("Wait") == 2688405u);
}

static void testUtf8AndEdge() {
    uint16_t out[17];
    CHECK(mp_utf8_to_utf16("Player", out, 17) == 6 && out[0] == 'P' && out[6] == 0);
    CHECK(mp_utf8_to_utf16("\xC3\xA9t\xC3\xA9", out, 17) == 3 && out[0] == 0xE9);      // "ete" with accents
    CHECK(mp_utf8_to_utf16("\xE3\x83\x9E", out, 17) == 1 && out[0] == 0x30DE);          // katakana
    CHECK(mp_utf8_to_utf16("\xF0\x9F\x98\x80!", out, 17) == 2 && out[0] == '?');        // emoji outside the BMP
    CHECK(mp_utf8_to_utf16("\xC3", out, 17) == 1 && out[0] == '?');                     // truncated
    CHECK(mp_utf8_to_utf16("\xC0\x80", out, 17) == 1 && out[0] == '?');                 // overlong
    CHECK(mp_utf8_to_utf16("abcdefghijklmnopqrstuvwxyz", out, 17) == 16 && out[16] == 0);
    CHECK(mp_utf8_to_utf16(nullptr, out, 17) == 0);
    float x, y;
    CHECK(!mp_edge_clamp(100, 100, false, 640, 480, 20, &x, &y) && x == 100 && y == 100);
    CHECK(mp_edge_clamp(2000, 240, false, 640, 480, 20, &x, &y) && x == 620 && y == 240);
    CHECK(mp_edge_clamp(320, -5000, false, 640, 480, 20, &x, &y) && x == 320 && y == 20);
    CHECK(mp_edge_clamp(400, 240, true, 640, 480, 20, &x, &y) && fabsf(x - 20) < 0.01f && y == 240);    // behind: mirrored to the left edge
    CHECK(mp_edge_clamp(320, 240, true, 640, 480, 20, &x, &y) && y == 460);              // dead behind: bottom centre
    CHECK(mp_edge_clamp(NAN, 1, false, 640, 480, 20, &x, &y) && MpPoseMath::finitef(x));
}

static void testLongOutageNoHold() {
    // Wave 3 (P1): after a 6 s (360-turn) drop (either arrival gap or sender clock pause), the buffer must reset / snap
    // and return to MP_SAMPLE_INTERP immediately instead of spending ~16 s frozen in MP_SAMPLE_HOLD.
    {
        MpPoseBuffer b;
        b.reset();
        MpPoseF o;
        for (uint32_t k = 0; k < 120; k += 2) {
            MpPoseF p = basePose(1000 + k);
            p.pos[0] = (float)k * 10.0f;
            b.push(p, 10 + k);
            b.sample(10 + k, &o);
        }
        // 6 s (360-turn) outage while local clock keeps ticking and sampling
        for (uint32_t t = 130; t < 490; t++) b.sample(t, &o);
        // Sender resumes (with sender clock paused during the 360-turn outage -> est shifted by -360, < old 600 threshold!)
        int postHolds = 0;
        int postInterps = 0;
        for (uint32_t k = 0; k < 120; k += 2) {
            const uint32_t now = 490 + k;
            MpPoseF p = basePose(1120 + k);
            p.pos[0] = 1200.0f + (float)k * 10.0f;
            b.push(p, now);
            for (uint32_t s = 0; s < 2; s++) {
                MpSampleResult r = b.sample(now + s, &o);
                if (k >= 16) {
                    if (r == MP_SAMPLE_HOLD) postHolds++;
                    if (r == MP_SAMPLE_INTERP) postInterps++;
                }
            }
        }
        printf("outage 6s (clock paused): post holds %d, post interps %d\n", postHolds, postInterps);
        CHECK(postHolds == 0);
        CHECK(postInterps > 80);
    }
    // Also verify arrival-gap reset when sender clock kept advancing across a 6 s Wi-Fi drop
    {
        MpPoseBuffer b;
        b.reset();
        MpPoseF o;
        for (uint32_t k = 0; k < 120; k += 2) {
            MpPoseF p = basePose(2000 + k);
            p.pos[0] = (float)k * 10.0f;
            b.push(p, 10 + k);
            b.sample(10 + k, &o);
        }
        for (uint32_t t = 130; t < 490; t++) b.sample(t, &o);
        int postHolds = 0;
        int postInterps = 0;
        for (uint32_t k = 0; k < 120; k += 2) {
            const uint32_t now = 490 + k;
            MpPoseF p = basePose(2480 + k);
            p.pos[0] = 4800.0f + (float)k * 10.0f;
            b.push(p, now);
            for (uint32_t s = 0; s < 2; s++) {
                MpSampleResult r = b.sample(now + s, &o);
                if (k >= 16) {
                    if (r == MP_SAMPLE_HOLD) postHolds++;
                    if (r == MP_SAMPLE_INTERP) postInterps++;
                }
            }
        }
        printf("outage 6s (clock running): post holds %d, post interps %d\n", postHolds, postInterps);
        CHECK(postHolds == 0);
        CHECK(postInterps > 80);
    }
}

static void testWave4Smoothness() {
    // 1. Visibility transition (hidden -> visible and visible -> hidden): hold A.pos before k >= 1.0 (no arcLerp slide),
    //    snap to B.pos when k >= 1.0 alongside B.flags.
    {
        MpPoseBuffer b;
        b.reset();
        MpPoseF a = basePose(100), v = basePose(102), c = basePose(104);
        a.flags = MP_POSE_ON_GROUND;             // hidden at x = 0
        a.pos[0] = 0.0f;
        v.flags = MP_POSE_VISIBLE | MP_POSE_ON_GROUND;   // visible at x = 600 (< 2500 warp threshold)
        v.pos[0] = 600.0f;
        c.flags = MP_POSE_VISIBLE | MP_POSE_ON_GROUND;
        c.pos[0] = 620.0f;
        b.push(a, 0); b.push(v, 2); b.push(c, 4);
        MpPoseF o;
        b.sampleAt(-3.5f, &o);   // k = 0.25 between a and v
        CHECK((o.flags & MP_POSE_VISIBLE) == 0 && o.pos[0] == 0.0f);
        b.sampleAt(-3.0f, &o);   // k = 0.50 between a and v
        CHECK((o.flags & MP_POSE_VISIBLE) == 0 && o.pos[0] == 0.0f);
        b.sampleAt(-2.1f, &o);   // k = 0.95 between a and v
        CHECK((o.flags & MP_POSE_VISIBLE) == 0 && o.pos[0] == 0.0f);
        b.sampleAt(-2.0f, &o);   // k = 1.00: becomes visible at B.pos
        CHECK((o.flags & MP_POSE_VISIBLE) != 0 && fabsf(o.pos[0] - 600.0f) < 1.0e-4f);
    }
    {
        MpPoseBuffer b;
        b.reset();
        MpPoseF v = basePose(200), h = basePose(202), c = basePose(204);
        v.flags = MP_POSE_VISIBLE; v.pos[0] = 100.0f;
        h.flags = 0;               h.pos[0] = 900.0f;
        c.flags = 0;               c.pos[0] = 950.0f;
        b.push(v, 0); b.push(h, 2); b.push(c, 4);
        MpPoseF o;
        b.sampleAt(-3.0f, &o);   // visible -> hidden at k = 0.5: still visible, holds A.pos (no slide)
        CHECK((o.flags & MP_POSE_VISIBLE) != 0 && o.pos[0] == 100.0f);
        b.sampleAt(-2.0f, &o);   // k = 1.0: hidden at B.pos
        CHECK((o.flags & MP_POSE_VISIBLE) == 0 && fabsf(o.pos[0] - 900.0f) < 1.0e-4f);
    }
    // 2. Loop-wrap weight interpolation: same anim & simple-bck flag with B.frame < A.frame still lerps weights[0..3]
    {
        MpPoseBuffer b;
        b.reset();
        MpPoseF a = basePose(300), c = basePose(304);
        a.anim = 0x42; c.anim = 0x42;
        a.frame = 58.0f; c.frame = 1.0f; a.rate = 1.0f;
        a.weights[0] = 1.0f; a.weights[1] = 0.0f; a.weights[2] = 0.2f; a.weights[3] = 0.8f;
        c.weights[0] = 0.0f; c.weights[1] = 1.0f; c.weights[2] = 0.6f; c.weights[3] = 0.4f;
        b.push(a, 0); b.push(c, 4);
        MpPoseF o;
        b.sampleAt(-2.0f, &o);   // k = 0.5, dtA = 2.0
        CHECK(fabsf(o.frame - 60.0f) < 1.0e-4f);
        CHECK(fabsf(o.weights[0] - 0.5f) < 1.0e-4f && fabsf(o.weights[1] - 0.5f) < 1.0e-4f);
        CHECK(fabsf(o.weights[2] - 0.4f) < 1.0e-4f && fabsf(o.weights[3] - 0.6f) < 1.0e-4f);
        // Different animation must NOT lerp weights across unrelated anims
        MpPoseBuffer b2;
        b2.reset();
        c.anim = 0x99;
        b2.push(a, 0); b2.push(c, 4);
        b2.sampleAt(-2.0f, &o);
        CHECK(fabsf(o.weights[0] - 1.0f) < 1.0e-4f && fabsf(o.weights[1] - 0.0f) < 1.0e-4f);
    }
    // 3. Upper-body loop-wrap frame advance: same upperAnim != 0 with B.upperFrame < A.upperFrame advances on A.rate (clamped >= 0)
    {
        MpPoseBuffer b;
        b.reset();
        MpPoseF a = basePose(400), c = basePose(404);
        a.upperAnim = 0x55; c.upperAnim = 0x55;
        a.upperFrame = 28.0f; c.upperFrame = 0.5f; a.rate = 1.5f;
        b.push(a, 0); b.push(c, 4);
        MpPoseF o;
        b.sampleAt(-2.0f, &o);   // dtA = 2.0 -> 28.0 + 1.5 * 2.0 = 31.0 (not frozen at 28.0)
        CHECK(fabsf(o.upperFrame - 31.0f) < 1.0e-4f);
        // Negative rate clamps >= 0.0f
        MpPoseBuffer bNeg;
        bNeg.reset();
        a.upperFrame = 0.5f; c.upperFrame = 0.0f; a.rate = -2.0f;
        bNeg.push(a, 0); bNeg.push(c, 4);
        bNeg.sampleAt(-2.0f, &o);
        CHECK(o.upperFrame == 0.0f);
    }
    // 4. Upper-body extrapolation frame advance: nw.upperAnim != 0 advances upperFrame by nw.rate * ee (clamped >= 0)
    {
        MpPoseBuffer b;
        b.reset();
        MpPoseF a = basePose(500), c = basePose(502);
        a.upperAnim = 0x77; c.upperAnim = 0x77;
        a.upperFrame = 8.0f; c.upperFrame = 10.0f; c.rate = 1.25f;
        b.push(a, 0); b.push(c, 2);
        MpPoseF o;
        CHECK(b.sampleAt(4.0f, &o) == MP_SAMPLE_EXTRAP);
        CHECK(fabsf(o.upperFrame - (10.0f + 1.25f * 4.0f)) < 1.0e-4f);
        // Beyond cap: holds at cap
        CHECK(b.sampleAt(25.0f, &o) == MP_SAMPLE_HOLD);
        CHECK(fabsf(o.upperFrame - (10.0f + 1.25f * MpPoseBuffer::kMaxExtrap)) < 1.0e-4f);
        // Negative rate clamps >= 0.0f
        MpPoseBuffer bNeg;
        bNeg.reset();
        c.upperFrame = 1.0f; c.rate = -2.0f;
        bNeg.push(a, 0); bNeg.push(c, 2);
        bNeg.sampleAt(4.0f, &o);
        CHECK(o.upperFrame == 0.0f);
        // upperAnim == 0 leaves upperFrame untouched
        MpPoseBuffer bNone;
        bNone.reset();
        c.upperAnim = 0; c.upperFrame = 0.0f; c.rate = 1.0f;
        bNone.push(a, 0); bNone.push(c, 2);
        bNone.sampleAt(4.0f, &o);
        CHECK(o.upperFrame == 0.0f);
    }
    // 5. Exact endpoint k == 1.0f (t == rb on middle sample B): ALL fields match B atomically (no mixed A.frame/weights/upperFrame/rate with B.anim)
    {
        MpPoseBuffer b;
        b.reset();
        MpPoseF a = basePose(100), mid = basePose(102), c = basePose(104);
        a.anim = 10; a.frame = 48.0f; a.weights[0] = 1.0f; a.upperAnim = 5; a.upperFrame = 28.0f; a.rate = 1.0f;
        mid.anim = 20; mid.frame = 0.0f; mid.weights[0] = 0.25f; mid.upperAnim = 9; mid.upperFrame = 1.0f; mid.rate = 1.5f;
        c.anim = 20; c.frame = 3.0f; c.weights[0] = 0.5f; c.upperAnim = 9; c.upperFrame = 4.0f; c.rate = 1.5f;
        b.push(a, 0); b.push(mid, 2); b.push(c, 4);
        MpPoseF o;
        CHECK(b.sampleAt(-2.0f, &o) == MP_SAMPLE_INTERP);   // t == rb of [a, mid] -> k == 1.0f
        CHECK(o.anim == 20 && fabsf(o.frame - 0.0f) < 1.0e-4f);
        CHECK(fabsf(o.weights[0] - 0.25f) < 1.0e-4f);
        CHECK(o.upperAnim == 9 && fabsf(o.upperFrame - 1.0f) < 1.0e-4f);
        CHECK(fabsf(o.rate - 1.5f) < 1.0e-4f);
    }
    // 6. Upper-body anim change advances A.upperFrame on A.rate for k < 1.0f; visFlip holds A.quat and A.scale for k < 1.0f
    {
        MpPoseBuffer b;
        b.reset();
        MpPoseF a = basePose(600), c = basePose(604);
        a.upperAnim = 5; a.upperFrame = 10.0f; a.rate = 1.5f;
        c.upperAnim = 9; c.upperFrame = 0.0f;
        a.flags = MP_POSE_VISIBLE; c.flags = 0;
        a.scale = 1.0f; c.scale = 2.0f;
        c.quat[0] = 0.0f; c.quat[1] = 0.70710678f; c.quat[2] = 0.0f; c.quat[3] = 0.70710678f;
        b.push(a, 0); b.push(c, 4);
        MpPoseF o;
        b.sampleAt(-2.0f, &o);   // k = 0.5, dtA = 2.0
        CHECK(fabsf(o.upperFrame - 13.0f) < 1.0e-4f);
        CHECK(fabsf(o.scale - 1.0f) < 1.0e-4f);
        CHECK(fabsf(o.quat[3] - 1.0f) < 1.0e-4f && fabsf(o.quat[1]) < 1.0e-4f);
    }
}

int main() {
    testQuatRoundTrip();
    testFieldQuantisation();
    testHostileInput();
    testInterpolationTiming();
    testExtrapolateThenHold();
    testOrderingAndDuplicates();
    testClockFollowsSlowerPath();
    testPacketLoss();
    testArcLerp();
    testSlerpAndInterpAnim();
    testNameMap();
    testUtf8AndEdge();
    testLongOutageNoHold();
    testWave4Smoothness();
    printf("%s: %d checks, %d failed\n", gFail ? "FAILED" : "OK", gChecks, gFail);
    return gFail ? 1 : 0;
}
