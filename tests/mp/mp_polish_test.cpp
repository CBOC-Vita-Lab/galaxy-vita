// Galaxy 3.0 multiplayer polish verification host tests:
//   (a) 2P immediate STATE flush in MpCore (GXV_MP_STATE_IMMED=1 vs 0 A/B, and 3P bundling preserved)
//   (b) MpPoseMath C1-smooth recovery across a 120 ms Wi-Fi gap (bounded step/accel, zero teleport, micro-extrapolation)
//   (c) Warm-up frustum placement (mp_warm_pose) and fly-in base * joint0->_64 orientation math
// Build/run: see tests/mp/build.sh flags + -I decomp/include
#include "core_test_util.h"
#include "Game/MP/MpPoseMath.hpp"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static int gChecks = 0;
#define PCHECK(c)                                                              \
    do {                                                                       \
        ++gChecks;                                                             \
        if (!(c)) {                                                            \
            ++g_fail;                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);       \
        }                                                                      \
    } while (0)

// ---- (a) 2P immediate STATE flush (and GXV_MP_STATE_IMMED A/B knob + 3P bundling) -------------------------------------------
struct StateFlushObs {
    uint64_t hostToGuestDg = 0;
    std::vector<uint64_t> hostToGuestTimes;
};
static StateFlushObs g_obs;
static void FlushTap(void*, const MpAddr& from, const MpAddr& to, const void*, size_t) {
    if (from.host == 1 && to.host == 2) {
        g_obs.hostToGuestDg++;
        g_obs.hostToGuestTimes.push_back(g_now);
    }
}

static std::vector<MpCore*> MakePlayingSession(MpLoopHub& hub, int players, uint32_t seed) {
    std::vector<MpCore*> all;
    const char* names[4] = {"Player", "Luigi", "Peach", "Toad"};
    for (int i = 0; i < players; i++) all.push_back(MakeCore(hub, (uint32_t)(i + 1), names[i], seed + (uint32_t)i * 7919u));
    MpHostParams hp = HostParams();
    all[0]->uiHost(&hp);
    WaitPhase(all, all[0], MP_UI_LOBBY, 2000);
    for (int i = 1; i < players; i++) {
        ScanJoin(all, all[i]);
        WaitPhase(all, all[i], MP_UI_LOBBY, 8000);
    }
    all[0]->uiStart();
    Step(all, 500);
    return all;
}

static void testTwoPlayerImmediateFlush() {
    // 1. Default (GXV_MP_STATE_IMMED=1): in a 2P session (1 active remote peer), host MP_MSG_STATE flushes immediately
    //    inside send() (zero bundle hold: datagram leaves at exact send timestamp t0, not t0 + bundleMs).
    setenv("GXV_MP_STATE_IMMED", "1", 1);
    setenv("SMG_MP_NET_BUNDLE_MS", "15", 1);
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        hub.setImpairment(MpLoopImpairment{0, 0, 2, 0, 0}, 11);
        std::vector<MpCore*> all = MakePlayingSession(hub, 2, 1001);
        // Drain any startup messages so we measure a single clean STATE send
        Step(all, 50, 5);
        MpMsg m;
        for (MpCore* c : all) while (c->poll(&m)) {}

        g_obs = StateFlushObs{};
        hub.setTap(FlushTap, nullptr);
        uint8_t payload[MP_POSE_WIRE_BYTES] = {0};
        const uint64_t tSend = g_now;
        memcpy(payload + 8, &tSend, sizeof tSend);
        PCHECK(all[0]->send(MP_MSG_STATE, payload, sizeof payload, false));
        // Immediate flush: datagram already hit the wire at tSend (before any extra time passes!)
        PCHECK(g_obs.hostToGuestDg >= 1);
        PCHECK(!g_obs.hostToGuestTimes.empty() && g_obs.hostToGuestTimes[0] == tSend);
        // Guest receives it after pure link latency (2 ms), well under bundleMs (15 ms)
        uint64_t recvAt = 0;
        for (int ms = 0; ms < 25 && !recvAt; ms++) {
            Step(all, 1, 1);
            while (all[1]->poll(&m)) {
                if (m.type == MP_MSG_STATE) recvAt = g_now;
            }
        }
        hub.setTap(nullptr, nullptr);
        PCHECK(recvAt > 0 && recvAt - tSend <= 4);
        printf("2P flush (GXV_MP_STATE_IMMED=1, bundleMs=15): wire delay %llu ms, recv age %llu ms\n",
               (unsigned long long)(g_obs.hostToGuestTimes.empty() ? 999 : g_obs.hostToGuestTimes[0] - tSend),
               (unsigned long long)(recvAt ? recvAt - tSend : 999));
        for (MpCore* c : all) delete c;
    }

    // 2. A/B knob off (GXV_MP_STATE_IMMED=0): host holds MP_MSG_STATE in OutBuf for bundleMs (15 ms)
    setenv("GXV_MP_STATE_IMMED", "0", 1);
    setenv("SMG_MP_NET_BUNDLE_MS", "15", 1);
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        hub.setImpairment(MpLoopImpairment{0, 0, 2, 0, 0}, 12);
        std::vector<MpCore*> all = MakePlayingSession(hub, 2, 2001);
        Step(all, 50, 5);
        MpMsg m;
        for (MpCore* c : all) while (c->poll(&m)) {}

        g_obs = StateFlushObs{};
        hub.setTap(FlushTap, nullptr);
        uint8_t payload[MP_POSE_WIRE_BYTES] = {0};
        const uint64_t tSend = g_now;
        PCHECK(all[0]->send(MP_MSG_STATE, payload, sizeof payload, false));
        // With GXV_MP_STATE_IMMED=0, packet is held in m_ob and NOT flushed immediately at tSend
        PCHECK(g_obs.hostToGuestDg == 0);
        // Step 10 ms (< 15 ms bundleMs): still held
        Step(all, 10, 5);
        PCHECK(g_obs.hostToGuestDg == 0);
        // Step past 15 ms: now flushed!
        Step(all, 10, 5);
        PCHECK(g_obs.hostToGuestDg >= 1);
        PCHECK(!g_obs.hostToGuestTimes.empty() && g_obs.hostToGuestTimes[0] - tSend >= 15);
        hub.setTap(nullptr, nullptr);
        printf("2P flush (GXV_MP_STATE_IMMED=0, bundleMs=15): held %llu ms before flush\n",
               (unsigned long long)(g_obs.hostToGuestTimes.empty() ? 0 : g_obs.hostToGuestTimes[0] - tSend));
        for (MpCore* c : all) delete c;
    }

    // 3. 3P session with GXV_MP_STATE_IMMED=1: guestLinks() == 2 -> STILL bundles (does not flush at tSend)
    setenv("GXV_MP_STATE_IMMED", "1", 1);
    setenv("SMG_MP_NET_BUNDLE_MS", "15", 1);
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        hub.setImpairment(MpLoopImpairment{0, 0, 2, 0, 0}, 13);
        std::vector<MpCore*> all = MakePlayingSession(hub, 3, 3001);
        Step(all, 50, 5);
        MpMsg m;
        for (MpCore* c : all) while (c->poll(&m)) {}

        g_obs = StateFlushObs{};
        hub.setTap(FlushTap, nullptr);
        uint8_t payload[MP_POSE_WIRE_BYTES] = {0};
        const uint64_t tSend = g_now;
        PCHECK(all[0]->send(MP_MSG_STATE, payload, sizeof payload, false));
        PCHECK(g_obs.hostToGuestDg == 0);   // held for bundling with 2 guests
        Step(all, 20, 5);
        PCHECK(g_obs.hostToGuestDg >= 1 && g_obs.hostToGuestTimes[0] - tSend >= 15);
        hub.setTap(nullptr, nullptr);
        printf("3P flush (GXV_MP_STATE_IMMED=1, bundleMs=15): bundled %llu ms as expected\n",
               (unsigned long long)(g_obs.hostToGuestTimes.empty() ? 0 : g_obs.hostToGuestTimes[0] - tSend));
        for (MpCore* c : all) delete c;
    }

    unsetenv("GXV_MP_STATE_IMMED");
    unsetenv("SMG_MP_NET_BUNDLE_MS");
}

// ---- (b) MpPoseMath C1 smooth recovery across a 120 ms Wi-Fi gap ------------------------------------------------------------
static MpPoseF makePose(uint32_t turn, float x, float y, float z) {
    MpPoseF p;
    memset(&p, 0, sizeof p);
    p.turn = turn;
    p.quat[3] = 1.0f;
    p.scale = 1.0f;
    p.pos[0] = x;
    p.pos[1] = y;
    p.pos[2] = z;
    p.anim = 0x42;
    p.rate = 1.0f;
    p.frame = (float)(turn % 1000);
    p.flags = MP_POSE_VISIBLE | MP_POSE_ON_GROUND;
    return p;
}

static void testWifiGapC1Recovery() {
    // Verify updated tuning constants
    PCHECK(fabsf(MpPoseBuffer::kMinDelay - 5.25f) < 1e-5f);
    PCHECK(fabsf(MpPoseBuffer::kHistKeep - 0.996f) < 1e-5f);
    PCHECK(fabsf(MpPoseBuffer::kCorrW - 0.16f) < 1e-5f);

    // Scenario: player runs +x at 12 units/turn, then makes a sharp 90-deg turn to +z right inside a 120 ms (8-turn) Wi-Fi
    // stall (even more severe than normal straight-line jitter: creates a ~60-90 unit lateral deviation that used to whip 1-2 ft).
    MpPoseBuffer b;
    b.reset();
    const uint32_t s0 = 5000, r0 = 100;
    auto truth = [](int k, float out[3]) {
        out[1] = 0.0f;
        if (k < 200) {
            out[0] = 12.0f * (float)k;
            out[2] = 0.0f;
        } else {
            out[0] = 2400.0f;
            out[2] = 12.0f * (float)(k - 200);
        }
    };

    float prev[3] = {0, 0, 0}, prev2[3] = {0, 0, 0};
    float worstStep = 0.0f, worstAcc = 0.0f;
    float worstMicroSlewDrop = 0.0f;
    float prevRender = 0.0f;
    bool havePrevRender = false;
    uint32_t snapsBefore = 0;
    for (int k = 0; k < 340; k++) {
        const uint32_t now = r0 + (uint32_t)k;
        // 30 Hz sends (even turns), 2-turn base latency; 120 ms Wi-Fi stall holds packets 198..212 until k = 216
        for (int j = 0; j <= k; j += 2) {
            const uint32_t arr = (j >= 198 && j <= 212) ? 216u : (uint32_t)(j + 2);
            if (arr == (uint32_t)k) {
                float p[3];
                truth(j, p);
                b.push(makePose(s0 + (uint32_t)j, p[0], p[1], p[2]), now);
            }
        }
        MpPoseF o;
        const MpSampleResult r = b.sample(now, &o);
        if (r == MP_SAMPLE_NONE) continue;
        const float rel = b.renderRel(now);
        const float rendered = (float)(int32_t)(b.s[b.count - 1].turn - s0) + rel;
        if (k == 180) snapsBefore = b.snaps;
        if (havePrevRender && rel > 0.0f && rel <= MpPoseBuffer::kMicroExtrap) {
            // During micro-extrapolation (0 < renderRel <= 3), render clock must NOT slam 0.5x brake
            const float cstep = rendered - prevRender;
            worstMicroSlewDrop = fmaxf(worstMicroSlewDrop, 1.0f - cstep);
        }
        prevRender = rendered;
        havePrevRender = true;

        if (k > 80) {
            const float d[3] = {o.pos[0] - prev[0], o.pos[1] - prev[1], o.pos[2] - prev[2]};
            const float a[3] = {o.pos[0] - 2 * prev[0] + prev2[0], o.pos[1] - 2 * prev[1] + prev2[1],
                                o.pos[2] - 2 * prev[2] + prev2[2]};
            worstStep = fmaxf(worstStep, MpPoseMath::len3(d));
            worstAcc = fmaxf(worstAcc, MpPoseMath::len3(a));
        }
        for (int i = 0; i < 3; i++) {
            prev2[i] = prev[i];
            prev[i] = o.pos[i];
        }
        if (k == 339) {
            float t0[3], t1[3];
            const int i0 = (int)floorf(rendered);
            truth(i0, t0);
            truth(i0 + 1, t1);
            const float f = rendered - (float)i0;
            float err[3];
            for (int i = 0; i < 3; i++) err[i] = o.pos[i] - (t0[i] + (t1[i] - t0[i]) * f);
            const float endErr = MpPoseMath::len3(err);
            printf("120 ms Wi-Fi gap + 90-deg turn: worstStep=%.2f (truth 12.0), worstAcc=%.2f, microSlewDrop=%.3f, snaps=%u, endErr=%.3f\n",
                   worstStep, worstAcc, worstMicroSlewDrop, (unsigned)(b.snaps - snapsBefore), endErr);
            PCHECK(b.snaps == snapsBefore);                                     // zero teleports/snaps
            PCHECK(worstStep < 24.0f);                                          // strictly bounded (< 0.75 ft extra, no 1-2 ft jump!)
            PCHECK(worstAcc < 8.0f);                                            // C1 smooth acceleration across arrival
            PCHECK(worstMicroSlewDrop <= MpPoseBuffer::kSlewDown + 0.01f);      // no 0.5x brake in first 3 micro-extrap turns
            PCHECK(endErr < 0.5f);                                              // fully converged back onto truth
        }
    }
}

// ---- (c) Warm-up frustum placement & fly-in joint0->_64 matrix composition --------------------------------------------------
static void mtxMul34(const float a[3][4], const float b[3][4], float out[3][4]) {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
        out[r][3] = a[r][0] * b[0][3] + a[r][1] * b[1][3] + a[r][2] * b[2][3] + a[r][3];
    }
}

static void mtxInvRigid34(const float m[3][4], float inv[3][4]) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) inv[r][c] = m[c][r];
    for (int r = 0; r < 3; r++) inv[r][3] = -(inv[r][0] * m[0][3] + inv[r][1] * m[1][3] + inv[r][2] * m[2][3]);
}

static void testWarmFrustumAndFlyInMtx() {
    // 1. Warm pose sits at cam + fwd * (nearZ + 120) with scale 0.001 (strictly inside frustum)
    const float cam[3] = {100.0f, 250.0f, -400.0f};
    const float fwd[3] = {0.6f, 0.0f, -0.8f};
    const float nearZ = 80.0f;
    const MpWarmPose wp = mp_warm_pose(cam[0], cam[1], cam[2], fwd[0], fwd[1], fwd[2], nearZ);
    const float rel[3] = {wp.pos[0] - cam[0], wp.pos[1] - cam[1], wp.pos[2] - cam[2]};
    const float depth = rel[0] * fwd[0] + rel[1] * fwd[1] + rel[2] * fwd[2];
    PCHECK(fabsf(depth - (nearZ + 120.0f)) < 1e-3f);
    PCHECK(depth > nearZ + 50.0f);
    PCHECK(fabsf(wp.scale - 0.001f) < 1e-6f);

    // 2. Fly-in orientation recovery: MarioActor stores _E3C = inv(base) * receivedMtx on joint0->_64.
    //    MpLocalCapture computes base * joint0->_64 and extracts quat -> must match receivedMtx's banked/pitched rotation!
    const float base[3][4] = {{1, 0, 0, 500}, {0, 1, 0, 300}, {0, 0, 1, -200}};   // unbanked upright mtx2
    // Banked 60 deg around X + 45 deg around Z (flight pitch/bank)
    const float ax = 1.04719755f, az = 0.78539816f;
    const float rx[3][4] = {{1, 0, 0, 0}, {0, cosf(ax), -sinf(ax), 0}, {0, sinf(ax), cosf(ax), 0}};
    const float rz[3][4] = {{cosf(az), -sinf(az), 0, 0}, {sinf(az), cosf(az), 0, 0}, {0, 0, 1, 0}};
    float received[3][4];
    mtxMul34(rz, rx, received);
    received[0][3] = 500;
    received[1][3] = 300;
    received[2][3] = -200;

    float invBase[3][4], e3c[3][4], captured[3][4];
    mtxInvRigid34(base, invBase);
    mtxMul34(invBase, received, e3c);       // MarioActor.cpp:2392: PSMTXConcat(inverse, receivedMtx, _E3C.mMtx)
    mtxMul34(base, e3c, captured);          // MpLocalCapture.cpp: PSMTXConcat(base, joint0->_64, poseMtx)

    float qWant[4], qGot[4];
    MpPoseMath::quatFromMtx(received, qWant);
    MpPoseMath::quatFromMtx(captured, qGot);
    const float angErr = MpPoseMath::quatAngle(qWant, qGot);
    printf("fly-in joint0->_64 recovery: angle error %.6f rad (warm depth %.1f > nearZ %.1f)\n", angErr, depth, nearZ);
    PCHECK(angErr < 1e-4f);

    // 3. Non-commuting joint0 BCK rotation (R_anim0 != I, e.g. DemoRideSpinDriver ry=90 deg):
    //    Real Mario: R_real = R_base * R_anim0 * R_64.
    //    MpLocalCapture conjugates: R_cap = R_base * R_anim0 * R_64 * R_anim0^-1.
    //    Puppet (joint0->_64 = nullptr): R_puppet = R_cap * R_anim0 -> must match R_real identically!
    {
        const float ab = 0.52359878f;  // 30 deg base yaw
        const float rBase[3][4] = {{cosf(ab), 0, sinf(ab), 0}, {0, 1, 0, 0}, {-sinf(ab), 0, cosf(ab), 0}};
        const float ay = 1.57079633f;  // 90 deg joint0 animation rotation (DemoRideSpinDriver)
        const float rAnim0[3][4] = {{cosf(ay), 0, sinf(ay), 0}, {0, 1, 0, 0}, {-sinf(ay), 0, cosf(ay), 0}};
        float rAnim0Inv[3][4];
        mtxInvRigid34(rAnim0, rAnim0Inv);
        float rRealTmp[3][4], rReal[3][4];
        mtxMul34(rBase, rAnim0, rRealTmp);
        mtxMul34(rRealTmp, rx, rReal);

        float rCap[3][4], rPuppet[3][4];
        mtxMul34(rReal, rAnim0Inv, rCap);       // R_cap = R_base * R_anim0 * R_64 * R_anim0^-1
        mtxMul34(rCap, rAnim0, rPuppet);        // R_puppet = R_cap * R_anim0

        float qReal[4], qPup[4], qNaiveCap[4], qNaivePup[4];
        MpPoseMath::quatFromMtx(rReal, qReal);
        MpPoseMath::quatFromMtx(rPuppet, qPup);
        const float conjErr = MpPoseMath::quatAngle(qReal, qPup);

        // Sanity: naive base * R_64 * R_anim0 would be wrong by ~1 rad when R_anim0 != I!
        float rNaiveCap[3][4], rNaivePup[3][4];
        mtxMul34(rBase, rx, rNaiveCap);
        mtxMul34(rNaiveCap, rAnim0, rNaivePup);
        MpPoseMath::quatFromMtx(rNaiveCap, qNaiveCap);
        MpPoseMath::quatFromMtx(rNaivePup, qNaivePup);
        const float naiveErr = MpPoseMath::quatAngle(qReal, qNaivePup);
        (void)qNaiveCap;
        printf("non-commuting R_anim0 (90 deg Y) conjugation: err=%.6f rad (naive would be %.3f rad)\n", conjErr, naiveErr);
        PCHECK(conjErr < 1e-4f);
        PCHECK(naiveErr > 0.5f);
    }
}

int main() {
    mp_log_set_sink(LogSink, true);
    testTwoPlayerImmediateFlush();
    testWifiGapC1Recovery();
    testWarmFrustumAndFlyInMtx();
    printf("mp_polish_test: %d checks, %d failed\n", gChecks, g_fail);
    return g_fail ? 1 : 0;
}
